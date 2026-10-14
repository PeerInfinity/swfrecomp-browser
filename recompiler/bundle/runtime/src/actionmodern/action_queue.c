#include <actionmodern/action_queue.h>
#include <sprite_frame_scripts.h>

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>

// Single dynamic-growth array. Drain scans for highest-priority non-empty
// bucket. For tens-of-entries scale (typical frame queue is <20 entries)
// this beats three separate ring buffers on simplicity with negligible cost.
// See ACTION_QUEUE_PLAN.md §"Data structure" for the rationale.

typedef struct {
	ActionQueueFn fn;
	void* user;
	MovieClip* clip;
	ActionQueuePriority priority;
	int is_unload;
	int queued_in_catchup;  // captured catch_up_mode at queue time
	ActionQueueKind kind;
} ActionQueueEntry;

// Read from libswf — global goto-catchup flag set during ng_executeGotoCatchUp.
extern int catch_up_mode;

static ActionQueueEntry* g_aq = NULL;
static size_t g_aq_count = 0;
static size_t g_aq_cap = 0;

// Drain-suppress depth (Path A foundation for GOTO_FIFO_UNIFICATION).
// Counter, not flag — see action_queue.h's docstring on
// actionDrainSuppressEnter for rationale.
static int g_drain_suppress_depth = 0;

void actionDrainSuppressEnter(void) { g_drain_suppress_depth++; }
void actionDrainSuppressLeave(void) { g_drain_suppress_depth--; }
int  actionDrainSuppressed(void)    { return g_drain_suppress_depth; }

// Browser-WASM frame-entry gate for the root timeline's DoAction.
// In real Flash a frame's DoAction runs ONCE when the playhead enters the
// frame, then the frame sits parked (no re-execution) until the playhead
// advances. The browser-WASM main loop (swf.c) re-runs frame_funcs[current_frame]
// every tick to keep rendering + sprite advance pumping (tagShowFrame lives
// inside the frame_func body), so without a gate the recompiler-emitted
// actionQueueScript would re-queue the root DoAction every tick — a Stop() on
// a parked frame would re-fire and clobber a sprite-driven _root.play().
// swf.c sets this to 1 while re-running a parked root frame and back to 0 on a
// fresh frame entry. Always 0 under NO_GRAPHICS / OFFSCREEN_RENDER, whose loops
// gate the whole frame_func on (is_playing || manual_next_frame) instead.
// See [[browser-wasm-frame-func-rerun]].
int g_suppress_root_doaction = 0;

// Phase D (GOTO_FIFO_UNIFICATION_INCREMENTAL): default flipped from 0 → 1.
// Activates the unified sprite-script dispatch path: gotos issued from inside
// sprite frame scripts FIFO-interleave with the surrounding drain instead of
// firing on the next tick.
static int g_unify_sprite_drain_flag = 1;
void actionSetUnifySpriteDrain(int v) { g_unify_sprite_drain_flag = v ? 1 : 0; }
int  actionUnifySpriteDrain(void)     { return g_unify_sprite_drain_flag; }

static int aq_grow(size_t needed)
{
	if (needed <= g_aq_cap) return 1;
	size_t new_cap = g_aq_cap ? g_aq_cap * 2 : 32;
	while (new_cap < needed) new_cap *= 2;
	ActionQueueEntry* p = (ActionQueueEntry*)realloc(g_aq, new_cap * sizeof(*p));
	if (!p) return 0;
	g_aq = p;
	g_aq_cap = new_cap;
	return 1;
}

void actionQueueCallbackEx(SWFAppContext* app_context,
                           ActionQueueFn fn,
                           void* user,
                           ActionQueuePriority priority,
                           MovieClip* clip,
                           int is_unload,
                           ActionQueueKind kind)
{
	(void)app_context;
	if (!fn) return;
	if (priority >= AQ_PRIORITY_COUNT) priority = AQ_PRIORITY_NORMAL;
	if (kind >= AQ_KIND_COUNT) kind = AQ_KIND_ONLOAD;
	if (!aq_grow(g_aq_count + 1)) return;
	ActionQueueEntry* e = &g_aq[g_aq_count++];
	e->fn = fn;
	e->user = user;
	e->clip = clip;
	e->priority = priority;
	e->is_unload = is_unload ? 1 : 0;
	e->queued_in_catchup = catch_up_mode ? 1 : 0;
	e->kind = kind;
}

void actionQueueCallback(SWFAppContext* app_context,
                         ActionQueueFn fn,
                         void* user,
                         ActionQueuePriority priority,
                         MovieClip* clip,
                         int is_unload)
{
	actionQueueCallbackEx(app_context, fn, user, priority, clip,
	                      is_unload, AQ_KIND_ONLOAD);
}

// Drain predicate. kind_filter == -1 means "any kind"; is_unload_filter == -1
// means "any is_unload value".
typedef struct {
	int kind_filter;
	int is_unload_filter;
} DrainFilter;

static int aq_entry_matches(const ActionQueueEntry* e, const DrainFilter* f)
{
	if (f->kind_filter >= 0 && (int)e->kind != f->kind_filter) return 0;
	if (f->is_unload_filter >= 0 && e->is_unload != f->is_unload_filter) return 0;
	return 1;
}

static int aq_pop_index(int* out_index, const DrainFilter* f)
{
	// Ruffle drains highest priority first, FIFO within priority. Scan all
	// entries; track the earliest index at the highest priority seen,
	// restricted to entries matching the filter.
	int best = -1;
	int best_pri = -1;
	for (size_t i = 0; i < g_aq_count; i++) {
		if (!aq_entry_matches(&g_aq[i], f)) continue;
		int pri = (int)g_aq[i].priority;
		if (pri > best_pri) {
			best_pri = pri;
			best = (int)i;
		}
	}
	if (best < 0) return 0;
	*out_index = best;
	return 1;
}

static void aq_drain(SWFAppContext* app_context, const DrainFilter* f)
{
	// Loop until no matching entries remain. Re-entrant queues (dispatch
	// pushes more entries) are handled naturally: each iteration re-scans.
	for (;;) {
		int idx = 0;
		if (!aq_pop_index(&idx, f)) break;
		ActionQueueEntry entry = g_aq[idx];
		// Remove the entry by sliding tail down (preserves FIFO order within
		// each priority bucket).
		if ((size_t)idx + 1 < g_aq_count) {
			memmove(&g_aq[idx], &g_aq[idx + 1],
			        (g_aq_count - (size_t)idx - 1) * sizeof(*g_aq));
		}
		g_aq_count--;

		// is_unload gating: skip if clip was removed before this entry ran,
		// unless this entry is itself an unload event.
		// CLIP_INIT/CLIP_CONSTRUCT/REGISTER_CTOR queued during natural play
		// (not catch_up_mode) fire chronologically — their placement was real,
		// and Flash semantics fire INIT/CTOR even if the clip is unloaded
		// later in the same drain window. Catch-up-queued entries keep the
		// existing skip filter so goto_commands aggregation (place+remove in
		// same goto sweep) remains canceled.
		int fires_chronologically =
			(entry.kind == AQ_KIND_CLIP_INIT
			 || entry.kind == AQ_KIND_CLIP_CONSTRUCT
			 || entry.kind == AQ_KIND_REGISTER_CTOR)
			&& !entry.queued_in_catchup;
		if (!entry.is_unload && entry.clip && !fires_chronologically) {
			if (entry.clip->avm1_removed || entry.clip->pending_removal) {
				continue;
			}
		}
		entry.fn(app_context, entry.user);
	}
}

void actionDrainActionQueue(SWFAppContext* app_context)
{
	DrainFilter f = { .kind_filter = -1, .is_unload_filter = -1 };
	aq_drain(app_context, &f);
}

void actionDrainActionQueueFiltered(SWFAppContext* app_context,
                                    int is_unload_filter)
{
	// Clamp defensively to {0,1}; any other value means "any is_unload".
	if (is_unload_filter != 0 && is_unload_filter != 1) is_unload_filter = -1;
	// Phase 1/2 semantics: only drain ONLOAD-kind entries. Other kinds
	// (LOAD, ATTACH_INIT, ROLL) are owned by their own drain sites.
	DrainFilter f = {
		.kind_filter = (int)AQ_KIND_ONLOAD,
		.is_unload_filter = is_unload_filter,
	};
	aq_drain(app_context, &f);
}

// Drain ONLOAD + SCRIPT entries together in FIFO order (used at the
// recompiler-emitted SHOW_FRAME drain site). Preserves the relative
// queue-time order between RemoveObject2-queued unload handlers and
// recompiler-queued DoAction scripts so that their trace output appears
// in the order Flash's ActionQueue model produces. Drain predicate:
//   (kind == AQ_KIND_ONLOAD || kind == AQ_KIND_SCRIPT) AND any is_unload.
//
// After the last unload entry dispatches (no more is_unload entries remain),
// run_pending_finalize() fires so MC Mark/Invalidate happens BEFORE any
// subsequent SCRIPT entry — matching Flash's "MC is removed by the time
// the next-tag DoAction reads its properties" semantics.
extern void run_pending_finalize(SWFAppContext* app_context);

extern int g_natural_wrap_cleanup_pending;
extern int catch_up_backward;
extern size_t catch_up_target;

static int aq_drain_one_onload_or_script(SWFAppContext* app_context);

// Phase 3 of CLIP_EVENT_ROUND_DISPATCH: unified frame-end priority drain.
// Drains INIT → CONSTRUCT → REGISTER_CTOR rounds, then ONLOAD+SCRIPT in
// FIFO. Mirrors Ruffle's Player::run_actions priority pop loop.
// Honors g_drain_suppress_depth so nested drains under an outer drain
// (ng_executeGotoCatchUp's inline target script call) are no-op'd.
//
// Loops until all queues are empty. SCRIPT execution can queue more
// CLIP_INIT/CLIP_CONSTRUCT/REGISTER_CTOR entries (e.g. via gotoAndStop's
// goto catch-up replaying placement tags); those higher-priority entries
// must drain BEFORE any subsequent SCRIPT entry queued in the same round.
// Mirrors Ruffle's run_actions priority-pop loop semantics.
// Key test: displaylist_depths/displaylist_depths_test3 (the gotoAndStop's
// deferred CLIP_CONSTRUCT must fire between the calling script and the
// inline target frame's queued DoAction).
void actionDrainAllInPriorityOrder(SWFAppContext* app_context)
{
	if (g_drain_suppress_depth > 0) return;
#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM: construct just-placed registerClass timeline sprites (e.g.
	// Minesweeper's FRadioButton components) before this frame's queued root
	// DoAction runs, so a same-frame `group.setValue(...)` sees the constructed
	// component group. NO_GRAPHICS/OFFSCREEN build these at placement time
	// (process_sprite_needs_init); browser-WASM otherwise builds them later in
	// tagShowFrame's advance, after this drain. Idempotent + re-entrancy-guarded.
	{
		extern void ng_construct_pending_registerclass_sprites(SWFAppContext*);
		ng_construct_pending_registerclass_sprites(app_context);
	}
#endif
	// Phase 4 (TRANSFORMED_BY_SCRIPT_WRAP_BACK): preserved from the
	// original actionDrainOnloadAndScript path. Run unsurvivor cleanup
	// before scripts run on natural-wrap catch-up.
	if (g_natural_wrap_cleanup_pending && catch_up_backward) {
		g_natural_wrap_cleanup_pending = 0;
		extern void ng_display_cleanup_unplaced_after(SWFAppContext*, size_t);
		ng_display_cleanup_unplaced_after(app_context, catch_up_target);
	}
	for (;;) {
		int had_work = 0;
		for (size_t i = 0; i < g_aq_count; i++) {
			ActionQueueKind k = g_aq[i].kind;
			if (k == AQ_KIND_CLIP_INIT || k == AQ_KIND_CLIP_CONSTRUCT
			    || k == AQ_KIND_REGISTER_CTOR) {
				had_work = 1;
				break;
			}
		}
		if (had_work) {
			actionDrainActionQueueByKind(app_context, AQ_KIND_CLIP_INIT);
			actionDrainActionQueueByKind(app_context, AQ_KIND_CLIP_CONSTRUCT);
			actionDrainActionQueueByKind(app_context, AQ_KIND_REGISTER_CTOR);
		}
		// Flush deferred sprite scripts before checking for ONLOAD/SCRIPT
		// entries — Phase D unification: gotos issued from sprite frame
		// scripts queue into the deferred list and become visible only
		// after this flush.
		if (g_unify_sprite_drain_flag) {
			actionFlushPendingSpriteScriptsToScriptQueue(app_context);
		}
		// Pop a single ONLOAD/SCRIPT entry, then loop back so any new
		// CLIP_INIT/CLIP_CONSTRUCT/REGISTER_CTOR entries that the SCRIPT
		// queued get drained before the next SCRIPT entry.
		int found_script = 0;
		for (size_t i = 0; i < g_aq_count; i++) {
			ActionQueueKind k = g_aq[i].kind;
			if (k == AQ_KIND_ONLOAD || k == AQ_KIND_SCRIPT) {
				found_script = 1;
				break;
			}
		}
		if (!found_script) {
			if (!had_work) break;
			else continue;
		}
		aq_drain_one_onload_or_script(app_context);
	}
}

// Pop and dispatch a single ONLOAD/SCRIPT entry (highest priority, FIFO).
// Returns 1 if an entry was dispatched (or skipped due to avm1_removed),
// 0 if no matching entries remain. Used by actionDrainAllInPriorityOrder
// to interleave priority-bucket drains between SCRIPT pops.
static int aq_drain_one_onload_or_script(SWFAppContext* app_context)
{
	int best = -1;
	int best_pri = -1;
	for (size_t i = 0; i < g_aq_count; i++) {
		ActionQueueEntry* e = &g_aq[i];
		if (e->kind != AQ_KIND_ONLOAD && e->kind != AQ_KIND_SCRIPT) continue;
		int pri = (int)e->priority;
		if (pri > best_pri) {
			best_pri = pri;
			best = (int)i;
		}
	}
	if (best < 0) return 0;
	ActionQueueEntry entry = g_aq[best];
	if ((size_t)best + 1 < g_aq_count) {
		memmove(&g_aq[best], &g_aq[best + 1],
		        (g_aq_count - (size_t)best - 1) * sizeof(*g_aq));
	}
	g_aq_count--;
	if (!entry.is_unload && entry.clip) {
		if (entry.clip->avm1_removed || entry.clip->pending_removal) {
			return 1;
		}
	}
	entry.fn(app_context, entry.user);
	if (entry.is_unload) {
		int has_more_unload = 0;
		for (size_t i = 0; i < g_aq_count; i++) {
			if (g_aq[i].is_unload) { has_more_unload = 1; break; }
		}
		if (!has_more_unload) {
			run_pending_finalize(app_context);
		}
	}
	return 1;
}

void actionDrainOnloadAndScriptOne(SWFAppContext* app_context)
{
	if (g_drain_suppress_depth > 0) return;
	if (g_unify_sprite_drain_flag) {
		actionFlushPendingSpriteScriptsToScriptQueue(app_context);
	}
	aq_drain_one_onload_or_script(app_context);
}

// Returns the current number of entries in the action queue. Used as a
// snapshot value for actionDrainOnloadScriptAbove (see below).
size_t actionAQCount(void)
{
	return g_aq_count;
}

// Drain ONLOAD + SCRIPT entries whose queue index is >= `floor`. Used by
// actionCall's CALL_FRAME_FUNC to drain only the entries newly queued by the
// invoked frame's tags, leaving the outer drain's pre-existing pending entries
// untouched. Without this isolation, the inner frame's actionDrainOnloadAndScript
// would leak into the outer queue and process scripts out of order — e.g.
// frame_label_test's `call('/:1')` would re-fire the parent frame's check_equals
// scripts before the called frame's variable-reset script ran. Mirrors Ruffle's
// per-call action stack semantics where each call() pushes/pops a private
// action layer.
void actionDrainOnloadScriptAbove(SWFAppContext* app_context, size_t floor)
{
	for (;;) {
		if (g_unify_sprite_drain_flag) {
			actionFlushPendingSpriteScriptsToScriptQueue(app_context);
		}
		int best = -1;
		int best_pri = -1;
		for (size_t i = floor; i < g_aq_count; i++) {
			ActionQueueEntry* e = &g_aq[i];
			if (e->kind != AQ_KIND_ONLOAD && e->kind != AQ_KIND_SCRIPT) continue;
			int pri = (int)e->priority;
			if (pri > best_pri) {
				best_pri = pri;
				best = (int)i;
			}
		}
		if (best < 0) break;
		ActionQueueEntry entry = g_aq[best];
		if ((size_t)best + 1 < g_aq_count) {
			memmove(&g_aq[best], &g_aq[best + 1],
			        (g_aq_count - (size_t)best - 1) * sizeof(*g_aq));
		}
		g_aq_count--;
		if (!entry.is_unload && entry.clip) {
			if (entry.clip->avm1_removed || entry.clip->pending_removal) {
				continue;
			}
		}
		entry.fn(app_context, entry.user);
		if (entry.is_unload) {
			int has_more_unload = 0;
			for (size_t i = 0; i < g_aq_count; i++) {
				if (g_aq[i].is_unload) { has_more_unload = 1; break; }
			}
			if (!has_more_unload) {
				run_pending_finalize(app_context);
			}
		}
	}
}

void actionDrainOnloadAndScript(SWFAppContext* app_context)
{
	// Path A: when an outer drain is in progress (e.g. ng_executeGotoCatchUp
	// runs funcs[target] inline to queue its DoAction), suppress the nested
	// recompiler-emitted SHOW_FRAME drain so the outer drain picks up the
	// new entries in FIFO order.
	if (g_drain_suppress_depth > 0) return;

	// Phase 4 (TRANSFORMED_BY_SCRIPT_WRAP_BACK): when a natural backward wrap
	// triggered the catch-up (g_natural_wrap_cleanup_pending=1), clean up
	// unsurvivors BEFORE scripts run. Otherwise AS lookups like
	// `_root.mc_green` resolve to a placed-at-later-frame entry that should
	// have been removed by the implicit goto's survives_rewind. Mirrors
	// Ruffle's run_goto remove_child(unsurvivor) ordering: the unsurvivor
	// list is computed from goto_commands aggregation BEFORE running target's
	// run_frame_internal. We approximate by running target's tags (which
	// updates placed_at_frame for survivors) BEFORE the drain, then doing
	// the cleanup here. One-shot: cleared after first drain so later drains
	// (e.g. timer callbacks within the same frame) don't re-run cleanup.
	if (g_natural_wrap_cleanup_pending && catch_up_backward) {
		g_natural_wrap_cleanup_pending = 0;
		extern void ng_display_cleanup_unplaced_after(SWFAppContext*, size_t);
		ng_display_cleanup_unplaced_after(app_context, catch_up_target);
	}

	for (;;) {
		// Phase C (GOTO_FIFO_UNIFICATION_INCREMENTAL): when the unify flag is
		// set, transfer any pending deferred sprite-script entries (queued via
		// ng_gotoFrameCurrentSprite during script execution) into the
		// AQ_KIND_SCRIPT bucket so they FIFO-interleave with the rest of this
		// drain's pops. Per-iteration flush picks up entries queued by scripts
		// that just dispatched. Default-off: with g_unify_sprite_drain=0 the
		// pending queue stays untouched and is cleaned up at end of tick.
		if (g_unify_sprite_drain_flag) {
			actionFlushPendingSpriteScriptsToScriptQueue(app_context);
		}

		int best = -1;
		int best_pri = -1;
		for (size_t i = 0; i < g_aq_count; i++) {
			ActionQueueEntry* e = &g_aq[i];
			if (e->kind != AQ_KIND_ONLOAD && e->kind != AQ_KIND_SCRIPT) continue;
			int pri = (int)e->priority;
			if (pri > best_pri) {
				best_pri = pri;
				best = (int)i;
			}
		}
		if (best < 0) break;
		ActionQueueEntry entry = g_aq[best];
		if ((size_t)best + 1 < g_aq_count) {
			memmove(&g_aq[best], &g_aq[best + 1],
			        (g_aq_count - (size_t)best - 1) * sizeof(*g_aq));
		}
		g_aq_count--;
		if (!entry.is_unload && entry.clip) {
			if (entry.clip->avm1_removed || entry.clip->pending_removal) {
				continue;
			}
		}
		entry.fn(app_context, entry.user);
		// After dispatching an UNLOAD, if no more unload entries remain in the
		// queue, fire pending Mark/Invalidate so the next SCRIPT sees the
		// MC's pending_removal/avm1_removed flags.
		if (entry.is_unload) {
			int has_more_unload = 0;
			for (size_t i = 0; i < g_aq_count; i++) {
				if (g_aq[i].is_unload) { has_more_unload = 1; break; }
			}
			if (!has_more_unload) {
				run_pending_finalize(app_context);
			}
		}
	}
}

void actionDrainActionQueueByKind(SWFAppContext* app_context,
                                  ActionQueueKind kind_filter)
{
	if (kind_filter >= AQ_KIND_COUNT) return;
	DrainFilter f = {
		.kind_filter = (int)kind_filter,
		.is_unload_filter = -1,
	};
	aq_drain(app_context, &f);
}

void* actionQueuePopMatching(ActionQueueKind kind_filter,
                             int (*pred)(void* user, void* ctx),
                             void* ctx)
{
	if (kind_filter >= AQ_KIND_COUNT) return NULL;
	if (pred == NULL) return NULL;
	// Highest-priority, FIFO-within-priority scan matching both kind and
	// the predicate. Pops the chosen entry by sliding the tail. Caller is
	// responsible for dispatching + freeing the user payload.
	int best = -1;
	int best_pri = -1;
	for (size_t i = 0; i < g_aq_count; i++) {
		if ((int)g_aq[i].kind != (int)kind_filter) continue;
		if (!pred(g_aq[i].user, ctx)) continue;
		int pri = (int)g_aq[i].priority;
		if (pri > best_pri) {
			best_pri = pri;
			best = (int)i;
		}
	}
	if (best < 0) return NULL;
	void* user = g_aq[best].user;
	if ((size_t)best + 1 < g_aq_count) {
		memmove(&g_aq[best], &g_aq[best + 1],
		        (g_aq_count - (size_t)best - 1) * sizeof(*g_aq));
	}
	g_aq_count--;
	return user;
}

void actionResetActionQueue(SWFAppContext* app_context)
{
	(void)app_context;
	g_aq_count = 0;
}

size_t actionActionQueuePending(void)
{
	return g_aq_count;
}

void* actionQueueFindUserByKind(ActionQueueKind kind,
                                int (*pred)(void* user, void* ctx),
                                void* ctx)
{
	if (pred == NULL) return NULL;
	if (kind >= AQ_KIND_COUNT) return NULL;
	for (size_t i = 0; i < g_aq_count; i++) {
		if (g_aq[i].kind != kind) continue;
		if (pred(g_aq[i].user, ctx)) return g_aq[i].user;
	}
	return NULL;
}

// Phase 6: AQ_KIND_SCRIPT dispatcher. The payload is just the function
// pointer boxed so the generic ActionQueueFn signature can carry it.
typedef struct {
	void (*fn)(SWFAppContext*);
} PendingScript;

static void aq_dispatch_script(SWFAppContext* app_context, void* user)
{
	PendingScript* p = (PendingScript*)user;
	if (p && p->fn) p->fn(app_context);
	free(p);
}

// Non-zero while actionImportAssets replays an IMPORTED movie's frame 0 to
// register its character definitions. Flash's ImportAssets imports SYMBOLS
// only — the imported movie's own timeline never plays — so the root DoAction
// the recompiler queues out of that frame must be dropped
// (import_assets/avm1_imports_avm1 otherwise traces the loadee's own
// "Hello from right_eye.swf"). The imported movie's DoInitAction scripts are
// unaffected: those run from `entry->init_func`, not from this replay.
int g_import_assets_frame0 = 0;

void actionQueueScript(SWFAppContext* app_context,
                       void (*fn)(SWFAppContext*))
{
	if (!fn) return;
	if (g_import_assets_frame0) return;
	// Browser-WASM: skip re-queueing the root DoAction while the playhead is
	// parked on a stopped frame (set by swf.c). See g_suppress_root_doaction.
	if (g_suppress_root_doaction) return;
	PendingScript* p = (PendingScript*)malloc(sizeof(*p));
	if (!p) return;
	p->fn = fn;
	actionQueueCallbackEx(app_context, aq_dispatch_script, p,
	                      AQ_PRIORITY_NORMAL, NULL, 0, AQ_KIND_SCRIPT);
}

// Phase 7b: sprite DoAction dispatcher. Restores the sprite's MC context
// (captured at queue time) before firing the script, then restores whatever
// context was active at drain time. Required because the SHOW_FRAME
// pre-drain runs with root-level g_current_context, but sprite scripts
// expect to see their sprite's MC — they resolve `this`, `_parent`,
// local vars, etc. against it.
typedef struct {
	void (*fn)(SWFAppContext*);
	MovieClip* ctx_mc;
	MovieClip* ctx_base;
	DisplayObject* ctx_sprite_obj;
} PendingSpriteScript;

extern MovieClip* g_current_context;
extern DisplayObject* g_current_sprite_obj;
extern void actionSetCurrentContext(MovieClip* mc);
extern void actionSetBaseClip(MovieClip* mc);
extern MovieClip* actionGetBaseClip(void);

extern int g_settarget_explicit_root;
extern int g_settarget_invalid;
extern int g_settarget_none;
extern int g_settarget_context_changed;
extern MovieClip* g_settarget_saved_context;

// Phase 7b fix: bump sprite-init depth during sprite-script dispatch so
// ng_isInsideSpriteInit() reports true for the duration. Pre-7b ran sprite
// DoActions from inside process_sprite_init_at_depth (g_sprite_init_depth>0),
// which made actionGotoFrame / actionNextFrame / CallMethod("nextFrame" etc.)
// take the inline-goto-catch-up path inside the script — so removing _root
// went through tagRemoveObject2 synchronously, updating avm1_removed before
// the next halt check. Under 7b, the script runs from the drain at root
// level where sprite_init_depth was 0, and those paths silently deferred the
// goto — the MC stayed alive for the rest of the script, so continuation
// lines like "clip 6 BAD" printed that pre-7b halted at.
extern int ng_isInsideSpriteInit(void);
extern void ng_bumpSpriteInitDepth(void);
extern void ng_unbumpSpriteInitDepth(void);

// Phase 7b fix: return 1 if the captured MC or any ancestor has been removed
// (avm1_removed, pending_removal, or depth==INT_MIN marker). Covers nested
// children whose own avm1_removed isn't set because fire_recursive_child_unloads
// invalidates by (name, parent-DL-depth) and the child MC's depth field wasn't
// synced to that DL depth. Pre-7b, timeline sprite scripts fired via
// tagShowFrame's Phase 2 which was a no-op if sprite_needs_init was cleared by
// removal; 7b queues scripts at Phase 1 eager and fires them at root-level
// drain, where a mid-drain GotoFrame can remove the MC before its script runs.
// Key canary: avm1/register_and_init_order — script_4's GotoFrame(1) removes
// timeline "a"/"b" before the FIFO drain reaches their queued sprite DoActions.
static int aq_sprite_ctx_removed(MovieClip* mc)
{
	for (MovieClip* cur = mc; cur != NULL; cur = cur->parent) {
		if (cur->avm1_removed || cur->pending_removal || cur->depth == INT_MIN)
			return 1;
	}
	return 0;
}

static void aq_dispatch_sprite_script(SWFAppContext* app_context, void* user)
{
	PendingSpriteScript* p = (PendingSpriteScript*)user;
	if (!p) return;
	// Skip if the captured MC (or any ancestor) was removed between queue and
	// dispatch — see aq_sprite_ctx_removed above.
	if (p->ctx_mc != NULL && aq_sprite_ctx_removed(p->ctx_mc)) {
		free(p);
		return;
	}
	MovieClip* saved_ctx = g_current_context;
	MovieClip* saved_base = actionGetBaseClip();
	DisplayObject* saved_obj = g_current_sprite_obj;
	// Save g_settarget_* flags so any SetTarget state mutated by the sprite
	// script (e.g. SetTarget("") against a removed base_clip which sets
	// g_settarget_none=1) doesn't leak out to sibling scripts in the same
	// drain batch (script_10 actionPlay would early-return and root stays
	// stopped). Pre-7b's process_sprite_init_at_depth Phase 2 path already
	// did this; aq_dispatch_sprite_script needs to match.
	int saved_explicit_root = g_settarget_explicit_root;
	int saved_invalid = g_settarget_invalid;
	int saved_none = g_settarget_none;
	int saved_ctx_changed = g_settarget_context_changed;
	MovieClip* saved_ctx_save = g_settarget_saved_context;
	g_settarget_explicit_root = 0;
	g_settarget_invalid = 0;
	g_settarget_none = 0;
	g_settarget_context_changed = 0;
	g_settarget_saved_context = NULL;
	if (p->ctx_mc) {
		actionSetCurrentContext(p->ctx_mc);
		actionSetBaseClip(p->ctx_base ? p->ctx_base : p->ctx_mc);
	}
	// Restore g_current_sprite_obj unconditionally. In browser-WASM
	// (USE_WEBGPU without OFFSCREEN_RENDER) the exec_sprite_frame stub
	// in graphics_stubs.c sets g_current_sprite_obj but doesn't swap
	// g_current_context, so ctx_mc captured here is NULL even though
	// ctx_sprite_obj is valid. Without restoring it, the dispatched
	// sprite script sees g_current_sprite_obj == NULL and an inline
	// actionStop()/actionPlay() routes via the root-timeline fallback
	// (is_playing = 0) instead of stopping the sprite — Doodle Jump's
	// hero kept cycling through its 4-frame timeline.
	g_current_sprite_obj = p->ctx_sprite_obj;
	ng_bumpSpriteInitDepth();
	if (p->fn) p->fn(app_context);
	ng_unbumpSpriteInitDepth();
	actionSetCurrentContext(saved_ctx);
	actionSetBaseClip(saved_base);
	g_current_sprite_obj = saved_obj;
	g_settarget_explicit_root = saved_explicit_root;
	g_settarget_invalid = saved_invalid;
	g_settarget_none = saved_none;
	g_settarget_context_changed = saved_ctx_changed;
	g_settarget_saved_context = saved_ctx_save;
	free(p);
}

void actionQueueSpriteScriptCaptured(SWFAppContext* app_context,
                                      void (*fn)(SWFAppContext*),
                                      void* ctx_mc,
                                      void* ctx_base,
                                      void* ctx_sprite_obj)
{
	if (!fn) return;
	PendingSpriteScript* p = (PendingSpriteScript*)malloc(sizeof(*p));
	if (!p) return;
	p->fn = fn;
	p->ctx_mc = (MovieClip*)ctx_mc;
	p->ctx_base = (MovieClip*)ctx_base;
	p->ctx_sprite_obj = (DisplayObject*)ctx_sprite_obj;
	actionQueueCallbackEx(app_context, aq_dispatch_sprite_script, p,
	                      AQ_PRIORITY_NORMAL, NULL, 0, AQ_KIND_SCRIPT);
}

void actionQueueSpriteScript(SWFAppContext* app_context,
                              void (*fn)(SWFAppContext*))
{
	// Capture at queue time. During Phase 1 eager init, g_current_context is
	// the sprite whose frame_0 is executing; exec_sprite_frame sets it via
	// actionSetCurrentContext + actionSetBaseClip; g_current_sprite_obj tracks
	// the DisplayObject.
	actionQueueSpriteScriptCaptured(app_context, fn,
	                                 (void*)g_current_context,
	                                 (void*)actionGetBaseClip(),
	                                 (void*)g_current_sprite_obj);
}

// Stage 3 collector root helper: expose every queued entry's (fn, user) so
// action.c can mark object references held inside heap payloads (e.g. the
// deferred XML-load payload's doc pointer). Entries are opaque here — the
// callback owner knows which dispatch fns carry object-bearing payloads.
void actionQueueGcForEach(void (*cb)(void* fn, void* user))
{
	for (size_t i = 0; i < g_aq_count; i++)
		cb((void*)g_aq[i].fn, g_aq[i].user);
}

// Sprite display-list realloc rebase (see swf.h ALIASING RULE): queued
// sprite-script payloads capture ctx_sprite_obj = a DisplayObject* into a
// sprite display list. When that list's buffer moves, still-queued payloads
// must be repointed or the dispatch dereferences freed memory. Safe to call
// mid-drain: aq_drain pops an entry before dispatching it, so every entry
// still in g_aq[] holds a live payload.
void actionQueueRebaseSpriteObjPayloads(void* old_base, size_t old_bytes,
                                        void* new_base)
{
	uintptr_t lo = (uintptr_t)old_base;
	uintptr_t hi = lo + old_bytes;
	for (size_t i = 0; i < g_aq_count; i++) {
		if (g_aq[i].fn != aq_dispatch_sprite_script || g_aq[i].user == NULL)
			continue;
		PendingSpriteScript* p = (PendingSpriteScript*)g_aq[i].user;
		uintptr_t v = (uintptr_t)p->ctx_sprite_obj;
		if (v >= lo && v < hi)
			p->ctx_sprite_obj = new_base
				? (DisplayObject*)((char*)new_base + (v - lo))
				: NULL;  // new_base==NULL: buffer being FREED, not moved
	}
}
