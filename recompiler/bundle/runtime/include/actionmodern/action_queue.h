#pragma once

#include <libswf/swf.h>
#include <actionmodern/action.h>

// Action queue priorities (match Ruffle's ActionQueue / context.rs).
// Higher number = higher priority. Drain scans highest-first, FIFO within.
typedef enum {
	AQ_PRIORITY_NORMAL     = 0,
	AQ_PRIORITY_CONSTRUCT  = 1,
	AQ_PRIORITY_INITIALIZE = 2,
	AQ_PRIORITY_COUNT      = 3,
} ActionQueuePriority;

// Action kinds. Used by phased migration to disambiguate drain sites that
// historically had their own storage. Each drain site filters by kind so the
// pre-migration ordering is preserved. ONLOAD is the default kind (Phase 1 +
// Phase 2) and is drained by actionDrainActionQueueFiltered. New kinds
// introduced in Phase 3+ must drain via actionDrainActionQueueByKind.
typedef enum {
	AQ_KIND_ONLOAD         = 0, // Phase 1/2: pending_onloads + pending_unloads
	AQ_KIND_LOAD           = 1, // Phase 3: CLIP_EVENT_LOAD for duplicated clips
	AQ_KIND_ATTACH_INIT    = 2, // Phase 3c: attachMovie frame-0 init
	AQ_KIND_ROLL           = 3, // Phase 3: deferred rollOver/rollOut from focus changes
	AQ_KIND_CLIP_INIT      = 4, // Phase 4: CLIP_EVENT_INITIALIZE from tagPlaceObject2
	AQ_KIND_CLIP_CONSTRUCT = 5, // Phase 5: CLIP_EVENT_CONSTRUCT from tagPlaceObject2
	AQ_KIND_REGISTER_CTOR  = 6, // Phase 5: registerClass constructor from tagPlaceObject2
	AQ_KIND_SCRIPT         = 7, // Phase 6/7b: root DoAction, sprite DoAction,
	                            // sprite CLIP_EVENT_LOAD. All three consolidated
	                            // at NORMAL priority, FIFO-interleaved so the
	                            // per-sprite LOAD→frame_0 interleave emerges
	                            // naturally from SWF-tag-order inline queueing.
	AQ_KIND_CLIP_LOAD      = 8, // Retired in Phase 7b (merged into AQ_KIND_SCRIPT).
	                            // Enum slot kept for ABI stability of the kind
	                            // index; no new entries push at this kind.
	AQ_KIND_COUNT          = 9,
} ActionQueueKind;

// Generic queue callback: user code provides the dispatch logic.
// When the entry drains, `fn(app_context, user)` is invoked.
typedef void (*ActionQueueFn)(SWFAppContext* app_context, void* user);

// Queue a generic callback with default kind (AQ_KIND_ONLOAD).
//   clip       — MovieClip this action is bound to (NULL = unbound). Used by
//                is_unload gating: if clip is removed (avm1_removed or
//                pending_removal) and is_unload==0, the callback is skipped.
//   is_unload  — 1 if this action is an unload event that must fire even when
//                the clip is gone.
//   user       — opaque pointer passed to fn. The queue does not take
//                ownership; caller owns lifetime until drain.
void actionQueueCallback(SWFAppContext* app_context,
                         ActionQueueFn fn,
                         void* user,
                         ActionQueuePriority priority,
                         MovieClip* clip,
                         int is_unload);

// Kind-aware enqueue. Callers that push non-ONLOAD kinds must use this so
// the entries drain at their own drain sites instead of leaking into the
// ONLOAD drain.
void actionQueueCallbackEx(SWFAppContext* app_context,
                           ActionQueueFn fn,
                           void* user,
                           ActionQueuePriority priority,
                           MovieClip* clip,
                           int is_unload,
                           ActionQueueKind kind);

// Drain the queue until empty. Pops highest-priority non-empty bucket first,
// FIFO within each priority. New entries queued during dispatch are picked up
// by subsequent iterations (matching Ruffle Player::run_actions).
void actionDrainActionQueue(SWFAppContext* app_context);

// Filtered drain for AQ_KIND_ONLOAD entries by is_unload. Pops only entries
// whose kind == AQ_KIND_ONLOAD AND whose `is_unload` matches `is_unload_filter`
// (0 or 1). Other kinds and non-matching is_unload stay queued. Phase 1/2
// uses this at the unload drain site (is_unload_filter=1) and the onload
// drain site (is_unload_filter=0) to preserve ordering.
void actionDrainActionQueueFiltered(SWFAppContext* app_context,
                                    int is_unload_filter);

// Kind-filtered drain. Pops entries whose kind matches `kind_filter`
// regardless of is_unload. Phase 3+ drain sites (pending_loads, deferred
// rolls, etc.) use this to drain only their own entries.
void actionDrainActionQueueByKind(SWFAppContext* app_context,
                                  ActionQueueKind kind_filter);

// Drain AQ_KIND_ONLOAD + AQ_KIND_SCRIPT entries together in FIFO order.
// Used at the recompiler-emitted SHOW_FRAME drain site so RemoveObject2-queued
// unload handlers and DoAction scripts fire in queue-time order — preserving
// the Flash ActionQueue ordering for tag streams that interleave RemoveObject2
// and DoAction. (See DEFERRED_CLIP_UNLOAD_PLAN.)
void actionDrainOnloadAndScript(SWFAppContext* app_context);

// SWAPDEPTHS_REWIND_UNBLOCK Phase B: pop and dispatch a single
// ONLOAD/SCRIPT entry. Used by actionDrainAllInPriorityOrder to drain
// priority buckets (CLIP_INIT/CONSTRUCT/REGISTER_CTOR) between each
// SCRIPT pop, mirroring Ruffle's run_actions priority-pop loop.
void actionDrainOnloadAndScriptOne(SWFAppContext* app_context);

// Returns the current number of entries in the action queue. Used as a
// snapshot value for actionDrainOnloadScriptAbove.
size_t actionAQCount(void);

// Drain ONLOAD + SCRIPT entries whose queue index is >= `floor`. Used by
// actionCall to isolate a called frame's drain from the outer drain's
// pending entries. Without this isolation, calling a frame from inside an
// outer drain would leak the outer's pending scripts into the inner drain.
void actionDrainOnloadScriptAbove(SWFAppContext* app_context, size_t floor);

// Phase 3 of CLIP_EVENT_ROUND_DISPATCH: unified frame-end priority drain.
// Drains AQ_KIND_CLIP_INIT → AQ_KIND_CLIP_CONSTRUCT → AQ_KIND_REGISTER_CTOR
// in priority rounds, then ONLOAD+SCRIPT via actionDrainOnloadAndScript.
// Mirrors Ruffle's Player::run_actions priority pop loop. Used at the
// recompiler-emitted SHOW_FRAME drain site (replacing actionDrainOnloadAndScript)
// so cross-sprite INIT/CONSTRUCT/REG_CTOR ordering matches Flash batch model.
void actionDrainAllInPriorityOrder(SWFAppContext* app_context);

// Pop the highest-priority entry of `kind_filter` whose user payload
// satisfies `pred(user, ctx)`, returning the user pointer (caller owns
// lifetime). Returns NULL when no matching entries remain. Unlike
// actionDrainActionQueueByKind, the entry's dispatch fn is NOT called —
// the caller has full control over dispatch. Used by Phase 7a's
// per-sprite CLIP_LOAD drain: queue-time findOrCreate would misparent
// child MCs when a clip_actions-less parent's eager init hadn't yet
// set g_current_context to the parent MC, so the payload instead
// stores a DisplayObject pointer and the MC lookup happens at drain
// time against the correct parent chain.
void* actionQueuePopMatching(ActionQueueKind kind_filter,
                             int (*pred)(void* user, void* ctx),
                             void* ctx);

// Discard all queued entries without dispatching (e.g. catch-up resets).
void actionResetActionQueue(SWFAppContext* app_context);

// Non-zero if the queue has any pending entries (for loop-exit checks).
size_t actionActionQueuePending(void);

// Find the first queued entry of `kind` whose user payload satisfies `pred`
// (pred returns non-zero). Returns the user pointer, or NULL if no match.
// Used by coalescing enqueue sites that need to update an already-queued
// entry in place (e.g. attachMovie at the same swf_depth supersedes prior).
// The returned pointer remains owned by the queue and is freed by the
// dispatch callback when the entry drains; mutate its fields, do not free.
void* actionQueueFindUserByKind(ActionQueueKind kind,
                                int (*pred)(void* user, void* ctx),
                                void* ctx);

// Phase 6: queue a recompiler-generated root DoAction script. Thin wrapper
// over actionQueueCallbackEx with AQ_KIND_SCRIPT / AQ_PRIORITY_NORMAL. The
// dispatch callback simply invokes fn(app_context) and frees its payload.
void actionQueueScript(SWFAppContext* app_context,
                       void (*fn)(SWFAppContext*));

// Phase 7b: queue a recompiler-generated sprite DoAction script with its
// firing context bound at queue time. Captures g_current_context (and
// base_clip + g_current_sprite_obj) so that at drain time — which happens
// at the root-level SHOW_FRAME pre-drain where g_current_context has been
// restored to root — the script still fires in the sprite's MC context.
// This is necessary because sprite DoActions can access `this`, `_parent`,
// local with-scope vars, etc., which depend on g_current_context.
void actionQueueSpriteScript(SWFAppContext* app_context,
                              void (*fn)(SWFAppContext*));

// Phase C (GOTO_FIFO_UNIFICATION_INCREMENTAL): same as actionQueueSpriteScript
// but takes the sprite context as explicit arguments instead of capturing
// from globals. Used by the deferred sprite-script flush path
// (actionFlushPendingSpriteScriptsToScriptQueue) which captures context at
// goto-issue time and replays it at drain-side queue-up.
//   ctx_mc          — MovieClip* the script should fire under (g_current_context)
//   ctx_base        — MovieClip* base_clip for closure scoping
//   ctx_sprite_obj  — DisplayObject* of the sprite (for g_current_sprite_obj)
// Pointers are typed as void* so callers in non-libswf headers don't need
// to pull in <libswf/swf.h>. NULL ctx_mc is allowed but will leave the
// active context unchanged at dispatch.
void actionQueueSpriteScriptCaptured(SWFAppContext* app_context,
                                      void (*fn)(SWFAppContext*),
                                      void* ctx_mc,
                                      void* ctx_base,
                                      void* ctx_sprite_obj);

// Phase C (GOTO_FIFO_UNIFICATION_INCREMENTAL): runtime flag controlling
// whether actionDrainOnloadAndScript consults the deferred sprite-script
// queue (g_pending in sprite_frame_scripts.c) and transfers entries into
// AQ_KIND_SCRIPT before each scan iteration. Default 0; Phase D flips
// this to 1 globally (or version-gated to SWF6+) once the unified-FIFO
// behavior is validated.
//
// Per-iteration flush guarantees that sprite goto issued from inside a
// drained script is picked up by the same drain — the new entry lands in
// AQ_KIND_SCRIPT and the next loop iteration's scan finds it.
void actionSetUnifySpriteDrain(int v);
int  actionUnifySpriteDrain(void);

// Phase 7b: gate accessors used by recompiler-emitted sprite DoAction
// queue calls. The gate fires `actionQueueScript` when any of:
//   - normal root tag stream (!catch_up_mode)
//   - target-frame scripts-only replay during goto (g_tag_skip_mode)
//   - parent's Phase 1 eager init of a sprite's frame_0 (actionEagerInitActive)
// ...but only when not inside Phase 2's was_eager script-only re-run
// (actionScriptOnlyMode) — that path is a safety net and must not re-queue
// scripts already queued during Phase 1.
int actionEagerInitActive(void);
int actionScriptOnlyMode(void);

// True during ng_executeGotoCatchUp's step 2 (intermediate + target frame
// tag replay with catch_up_mode=1). The gate must NOT queue sprite scripts
// during this window, because they'd end up in the queue BEFORE the target
// frame's root DoAction (queued later in step 3 with g_tag_skip_mode=1),
// producing a sprite-first-then-root FIFO drain — wrong. Instead, sprite
// scripts for goto-placed sprites fire synchronously during the deferred
// Phase 2 re-run later (actionDeferredSpriteInitActive below).
int actionGotoCatchupActive(void);

// True during ng_run_deferred_sprite_init_impl's process_sprite_needs_init
// call (goto Phase 2 re-run). Combined with actionScriptOnlyMode in the
// sprite DoAction gate, this takes the synchronous-fire path — scripts
// fire inline here because the target frame's root DoAction has already
// drained, and these sprite scripts must fire after it.
int actionDeferredSpriteInitActive(void);

// Phase 7b: runtime-attach paths (ng_attachMovie / ng_cloneSprite /
// ng_duplicateMovieClip) bracket their frame_0 invocation with these
// wrappers, so actionEagerInitActive() reports true during those attaches
// — making recompiler-emitted sprite DoAction queue calls fire the same
// way as timeline-placed eager init.
void actionEagerInitEnter(void);
void actionEagerInitLeave(void);

// Phase 7b: Enter/Leave for the two new goto-sensitive contexts above.
void actionGotoCatchupEnter(void);
void actionGotoCatchupLeave(void);
void actionDeferredSpriteInitEnter(void);
void actionDeferredSpriteInitLeave(void);

// Phase F (GOTO_FIFO_UNIFICATION_INCREMENTAL): runtime-attach Phase 2
// re-run path (aq_dispatch_pending_attach_init) brackets its
// pai->func + process_sprite_needs_init_public calls with these so the
// recompiler-emitted gate can distinguish attachMovie's deferred init
// (which must sync-fire to preserve PAI ordering) from goto's Phase 2
// deferred init (which must queue to FIFO-interleave with target script).
// Both paths set actionScriptOnlyMode=1 and actionDeferredSpriteInitActive=1,
// so the attach-init flag is needed to disambiguate.
int actionAttachInitActive(void);
void actionAttachInitEnter(void);
void actionAttachInitLeave(void);

// Drain-suppress primitive (Path A foundation for GOTO_FIFO_UNIFICATION).
// While the suppress depth is > 0, actionDrainOnloadAndScript early-returns —
// queue calls inside the bracketed region still run, but the recompiler-emitted
// SHOW_FRAME drain is no-op'd so an outer drain that's already in progress can
// pick up the new entries in FIFO order.
//
// Use a counter (not a flag) so reentrant goto-catch-up calls don't have an
// inner Leave clear suppression set up by an outer Enter. The expected pattern:
//
//     actionDrainSuppressEnter();
//     funcs[target](app_context);   // queue calls run, drain calls no-op
//     actionDrainSuppressLeave();
//
// Contract: only actionDrainOnloadAndScript honors the suppress. The kind /
// filtered drains keep firing because their callers are not nested under the
// SHOW_FRAME drain.
//
// Invariant: depth must return to 0 by the start of every tick. swf_core.c /
// swf.c assert this in debug builds.
void actionDrainSuppressEnter(void);
void actionDrainSuppressLeave(void);
int actionDrainSuppressed(void);

// Stage 3 collector root helper — iterate queued (fn, user) pairs so the
// root walk can mark object refs inside heap payloads (see actionGcMarkRoots).
void actionQueueGcForEach(void (*cb)(void* fn, void* user));

// Sprite display-list realloc rebase (see swf.h ALIASING RULE): repoint
// queued sprite-script payloads whose captured ctx_sprite_obj falls inside
// the moved buffer [old_base, old_base+old_bytes).
void actionQueueRebaseSpriteObjPayloads(void* old_base, size_t old_bytes,
                                        void* new_base);
