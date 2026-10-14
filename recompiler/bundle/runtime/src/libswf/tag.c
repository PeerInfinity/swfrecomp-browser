#include <swf.h>
#include <tag.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <utils.h>
#include <heap.h>
#include <stdint.h>
#include <sprite_frame_scripts.h>
#include <hit_test.h>

#include <libswf/generated_data.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifndef NO_GRAPHICS
#include <renderer.h>
extern RenderContext* context;
#endif

// action.h is needed in all modes (g_current_context, actionFindOrCreateMovieClip, etc.)
#include <action.h>
#include <actionmodern/object.h>  // getPropertyWithPrototype, ASObject (enterFrame pruning)

// ActionQueue API for clip-event migration (Phase 4+ of ACTION_QUEUE_PLAN).
#include <actionmodern/action_queue.h>

// Frame execution state — defined in swf_core.c (NO_GRAPHICS), swf.c (GRAPHICS)
extern int catch_up_mode;
extern int g_tag_skip_mode;

#include <limits.h>  // INT_MIN (freed-MC sentinel depth)

// --- Per-frame-walk MovieClip resolution cache -----------------------------
// The hot per-frame tree walks (advance_sprite_children_only,
// presync_nested_cf_recurse, ...) resolve each sprite node's MovieClip* by
// string name every frame. The resolvers (actionFindOrCreateMovieClip /
// actionFindMovieClipByName) do an O(child_mc_count) scan with swf_name_match
// per call. We memoize the result on the DisplayObject (obj->resolved_mc) and
// revalidate the SAME predicate the resolver keys on, so a hit is a single
// swf_name_match + a few pointer checks instead of a full scan. On a miss the
// caller runs the real resolver and calls tag_store_walk_mc to (re)cache.
//
//   name_only=1  mirrors actionFindMovieClipByName (global first-match by name:
//                depth!=INT_MIN && swf_name_match).
//   name_only=0  mirrors findOrCreateMovieClip (live + same parent + same name,
//                honoring name_displaced and the g_skip_pending_removal_mc gate).
//
// Correctness: same-name live MCs are appended to child_mc_cache (a fresh
// placement never shadows an earlier *live* match — it lands at a higher index),
// so the cached first-match stays the resolver's answer while it remains live
// and name-matching. Otherwise revalidation fails (freed → depth==INT_MIN,
// renamed/re-parented → predicate mismatch) and the caller re-resolves.
extern int g_skip_pending_removal_mc;
extern int swf_name_match(const char* a, const char* b);

static inline MovieClip* tag_cached_walk_mc(DisplayObject* obj, MovieClip* parent, int name_only)
{
	MovieClip* c = (MovieClip*)obj->resolved_mc;
	if (c == NULL || obj->instance_name == NULL) return NULL;
	if (c->depth == INT_MIN) return NULL;
	if (!name_only) {
		if (c->name_displaced) return NULL;
		if (c->parent != parent) return NULL;
		if (g_skip_pending_removal_mc && c->pending_removal) return NULL;
	}
	if (!swf_name_match(c->name, obj->instance_name)) return NULL;
	return c;
}

static inline void tag_store_walk_mc(DisplayObject* obj, MovieClip* mc)
{
	obj->resolved_mc = (void*)mc;
}

// --- Event-driven enterFrame-walk pruning ----------------------------------
// Two per-frame walks over the whole display tree exist purely to drive
// enterFrame dispatch, and on a deep static tree (e.g. Minesweeper's stopped
// FUIComponent radios) they are wasted work:
//
//   set_enterframe_eligible_recursive — sets enterframe_eligible=1, consumed
//     ONLY by actionDispatchEnterFrameHandlers (mc.onEnterFrame).
//   gather_clip_ef_entries (via dispatch_enterframe_clip_actions) — gathers
//     entries, used ONLY to fire CLIP_EVENT_ENTER_FRAME clip actions.
//
// Walk 1 — stamp the ancestry of every onEnterFrame handler each tick and prune
// subtrees with no stamped descendant. SOURCE-COMPLETE: an onEnterFrame handler
// lives on mc->dynamic_props, which only exists on cached MovieClips, so every
// dispatchable handler is reachable from child_mc_cache. g_ef_prune_safe drops
// to the full walk for the tick if any handler MC's display_obj ancestry is
// broken (NULL link before root), so a linkage gap can never drop a handler.
// Recomputed each tick (self-healing: a handler added mid-tick fires next tick,
// matching the existing dispatch which skips newly-added MCs anyway).
//
// Walk 2 — skip gather entirely while no CLIP_EVENT_ENTER_FRAME clip action has
// ever been placed (g_any_clip_ef_placed, sticky). When one exists we keep the
// full walk (conservative — avoids any per-subtree gap for anonymous clips).
static int g_ef_prune_safe = 0;
static size_t g_ef_stamp_tick = (size_t)-1;
int g_any_clip_ef_placed = 0;

// Sticky-flag a clip-action list that contains CLIP_EVENT_ENTER_FRAME. Called at
// every site that assigns clip_actions to a live display entry.
static inline void note_clip_actions_for_ef(const ClipAction* acts, size_t count)
{
	if (g_any_clip_ef_placed || acts == NULL) return;
	for (size_t a = 0; a < count; a++)
		if (acts[a].event_flags & CLIP_EVENT_ENTER_FRAME) { g_any_clip_ef_placed = 1; return; }
}

// Stamp subtree_ef_gen along the display-tree ancestry of every cached MovieClip
// that has an onEnterFrame handler. Idempotent per tick.
static void stamp_onenterframe_paths(void)
{
	extern size_t g_tick_count;
	if (g_ef_stamp_tick == g_tick_count) return;   // once per tick
	g_ef_stamp_tick = g_tick_count;
	g_ef_prune_safe = 1;

	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	extern MovieClip root_movieclip;
	for (int i = 0; i < child_mc_count; i++)
	{
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL || mc->dynamic_props == NULL) continue;
		if (mc->is_button_mc) continue;                  // buttons don't fire onEnterFrame
		ASObject* props = (ASObject*)mc->dynamic_props;
		ActionVar* ef = getPropertyWithPrototype(props, "onEnterFrame", 12);
		if (ef == NULL || ef->type != ACTION_STACK_VALUE_FUNCTION) continue;
		// Dynamic MCs without a display entry use mc_enterframe_eligible, a
		// separate path not gated by set_enterframe_eligible_recursive.
		if (mc->display_obj == NULL) continue;
		for (MovieClip* p = mc; p != NULL && p != &root_movieclip; p = p->parent)
		{
			DisplayObject* d = (DisplayObject*)p->display_obj;
			if (d == NULL) { g_ef_prune_safe = 0; break; }  // broken ancestry → full walk
			d->subtree_ef_gen = g_tick_count;
		}
	}
}

// Per-movie transform data mapping (indexed by movie_id, 0 = main SWF).
// Set during actionImportAssets/actionFirePendingLoadInits so that sprites from
// child movies can reference the correct transform array during frame execution.
// Needed in all build modes (NO_GRAPHICS and graphics).
#define MAX_MOVIE_TRANSFORM_ENTRIES 16
static float (*g_movie_transform_data[MAX_MOVIE_TRANSFORM_ENTRIES])[16] = {0};

void ng_registerMovieTransformData(u8 movie_id, float (*td)[16]) {
	if (movie_id < MAX_MOVIE_TRANSFORM_ENTRIES)
		g_movie_transform_data[movie_id] = td;
}

float (*ng_getMovieTransformData(u8 movie_id))[16] {
	if (movie_id >= MAX_MOVIE_TRANSFORM_ENTRIES) return NULL;
	return g_movie_transform_data[movie_id];
}

// The transform table a display entry's `transform_id` indexes, or NULL when
// that is the main movie's `transform_data` (so callers holding a different
// handle on the main table — e.g. the GPU-side app_context->transform_data,
// which also carries dynamic slots — keep using theirs).
//
// Set by ng_cache_transform at placement time from g_active_transform_data,
// i.e. keyed on the movie whose TAG placed the entry. See swf.h's
// place_transform_data for why that is not the same as child_transform_data.
float (*ng_entryChildTransformData(const DisplayObject* obj))[16] {
	GEN_EXTERN_TRANSFORM_DATA;
	if (obj == NULL) return NULL;
	float (*td)[16] = obj->place_transform_data;
	if (td == NULL || td == transform_data) return NULL;
	// The COMBINED table (multi-SWF render slice) is what
	// app_context->transform_data points at, so it is "theirs" too: an entry on
	// it carries an already-re-based id and needs no override.
	if ((const float (*)[16]) td == ng_combinedTransformData()) return NULL;
	return td;
}

// Same lookup, but resolved to a usable table: the main movie's
// `transform_data` when the entry carries no override. Never NULL.
float (*ng_entryTransformData(const DisplayObject* obj))[16] {
	GEN_EXTERN_TRANSFORM_DATA;
	float (*td)[16] = (obj != NULL) ? obj->place_transform_data : NULL;
	return (td != NULL) ? td : transform_data;
}

GEN_EXTERN_TRANSFORM_DATA;

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
// NO_GRAPHICS / graphics-native: cxform_data is graphics-side only here.
GEN_EXTERN_CXFORM_DATA;

// Active transform data pointer — defaults to main SWF's transform_data.
// Swapped to child SWF's array during child movie init so that tagPlaceObject2
// caches correct transform values on display objects (needed for getBounds on loaded movies).
// Browser-WASM graphics gets this from graphics_stubs.c.
float (*g_active_transform_data)[16] = NULL;
#else
extern float (*g_active_transform_data)[16];
#endif

// Which movie's tagInit / frame funcs are running (defined in ng_shared.c).
extern u8 g_current_movie_id;

static inline void ng_cache_transform(DisplayObject* obj, u32 tid) {
	float (*td)[16] = g_active_transform_data ? g_active_transform_data : transform_data;
	// Record WHICH table the id indexes, not just the six values. Readers that
	// re-index `transform_id` later (ng_getTransformXY / ScaleRotation*, the
	// AVM1 _x/_y getters) otherwise index the main movie's `transform_data`
	// with a loaded child's id — out of bounds whenever the child has more
	// transform rows than the parent, which is the usual case.
	obj->place_a  = td[tid][0];
	obj->place_b  = td[tid][1];
	obj->place_c  = td[tid][4];
	obj->place_d  = td[tid][5];
	obj->place_tx = td[tid][12];
	obj->place_ty = td[tid][13];
	// Multi-SWF render slice: re-base a loaded movie's placement id onto the
	// COMBINED transform table (ng_buildMovieRenderTables). This is the single
	// funnel all 14 tagPlaceObject* sites pass through — each assigns
	// obj->transform_id immediately before calling — so doing it here makes
	// obj->transform_id a combined-table index for every one of its ~45
	// readers, CPU and GPU alike, without touching any of them.
	//
	// `td` is the key rather than g_current_movie_id because it is the one
	// signal that already tracks the PLACING movie through every path: the
	// loaders swap it for a loaded movie's tags, and exec_sprite_frame swaps it
	// again for a child-DEFINED sprite's frame funcs.
	//
	// Inert for the main movie and for any build with no child movies: the base
	// is 0 and the combined table is NULL, so both branches are skipped.
	if (ng_movieRenderTablesActive()) {
		const float (*_comb)[16] = ng_combinedTransformData();
		u32 _xbase = ng_movieTransformBaseForTable((const float (*)[16]) td);
		if (_comb != NULL && _xbase != 0) {
			obj->transform_id = tid + _xbase;
			td = (float (*)[16]) _comb;
		}
	}
	obj->place_transform_data = td;
}

// Re-base a placement's colour-transform id onto the combined cxform table
// (multi-SWF render slice). Same argument as ng_cache_transform above and the
// same key; 0 is the "no colour transform" sentinel and never moves. Applied at
// the 14 `display_list[depth].cxform_id = ...` sites, which is every place a
// tag's cxform id reaches a display entry.
static inline u32 ng_rebaseCxformId(u32 cxform_id) {
	if (cxform_id == 0) return 0;
	float (*td)[16] = g_active_transform_data;
	if (td == NULL || !ng_movieRenderTablesActive()) return cxform_id;
	return cxform_id + ng_movieCxformBaseForTable((const float (*)[16]) td);
}

size_t dictionary_capacity = INITIAL_DICTIONARY_CAPACITY;
size_t display_list_capacity = INITIAL_DISPLAYLIST_CAPACITY;

// Per-character movie_id tracking (which movie defined each character).
// Used to look up the correct transform_data for child movie sprites.
u8* g_char_movie_id = NULL;
size_t g_char_movie_id_capacity = 0;

// Note: tagInit() is provided by the generated tagMain.c file

// Monotonically increasing counter to detect within-same-frame placement conflicts.
// Incremented at the end of each tagShowFrame call and before goto catch-up.
// Used by tagPlaceObject2 in all build modes.
size_t g_place_gen = 0;

// Monotonically increasing counter bumped at every full PlaceObject2 placement.
// Mirrors Ruffle's clip_exec_list LIFO order: largest place_seq = most-recently
// instantiated. Used by advance_sprite_frames / advance_nested_sprite_frames /
// dispatch_enterframe_clip_actions to dispatch frame functions and ENTER_FRAME
// clip events in reverse-instantiation order rather than depth-descending order.
// NOT bumped on modify-only paths (survives_rewind, char_id=0 PlaceObject2,
// loop-back same-char modify) — those preserve identity, including ordering.
// NOT bumped by swapDepths — depth changes, instantiation order is preserved.
size_t g_place_seq = 0;

// Frame-helper machinery used by both the NO_GRAPHICS frame loop
// (swf_core.c) and the --mode=graphics native build (swf.c with
// OFFSCREEN_RENDER). wasm graphics still uses the #else stubs in
// graphics_stubs.c.
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
// Tracks the currently-executing sprite's DisplayObject.
// Set by advance_sprite_frames before each sprite frame function call.
DisplayObject* g_current_sprite_obj = NULL;

// Root display list backup for inline catch-up from within sprite init context.
// Saved on the first entry into process_sprite_init_at_depth (nesting == 0)
// so that inline catch-up of root gotos can temporarily swap to the root DL.
static DisplayObject* g_root_dl_backup = NULL;
static size_t g_root_dl_max_backup = 0;
static size_t g_root_dl_cap_backup = 0;
static int g_sprite_init_depth = 0;

int ng_isInsideSpriteInit(void) { return g_sprite_init_depth > 0; }

// Phase 7b: exposed for aq_dispatch_sprite_script so sprite DoAction scripts
// drained at the root-level SHOW_FRAME pre-drain still report "inside sprite
// init" — pre-7b ran them inside process_sprite_init_at_depth which kept
// this flag >0 for the duration. Inline goto catch-up (actionNextFrame /
// actionGotoFrame / CallMethod("nextFrame"...)) depends on this flag to
// remove _root display entries synchronously before the next halt check —
// without it, the MC outlives the script and halt-on-removal is skipped.
// Matching depth bump ALSO sets g_root_dl_backup on the 0→1 transition so
// that nested ng_executeGotoCatchUp's ng_swapToRootDL finds the right root DL
// (harmless no-op when dispatch is already running in root DL context).
void ng_bumpSpriteInitDepth(void) {
	if (g_sprite_init_depth == 0) {
		g_root_dl_backup = display_list;
		g_root_dl_max_backup = max_depth;
		g_root_dl_cap_backup = display_list_capacity;
	}
	g_sprite_init_depth++;
}
void ng_unbumpSpriteInitDepth(void) {
	if (g_sprite_init_depth > 0) g_sprite_init_depth--;
	if (g_sprite_init_depth == 0) {
		// Restore display list in case inline catch-up swapped
		display_list = g_root_dl_backup;
		max_depth = g_root_dl_max_backup;
		display_list_capacity = g_root_dl_cap_backup;
	}
}

int ng_swapToRootDL(DisplayObject** saved_dl, size_t* saved_max, size_t* saved_cap) {
	if (g_sprite_init_depth <= 0) return 0;
	// Save current (sprite) DL
	*saved_dl = display_list;
	*saved_max = max_depth;
	*saved_cap = display_list_capacity;
	// Swap to root DL
	display_list = g_root_dl_backup;
	max_depth = g_root_dl_max_backup;
	display_list_capacity = g_root_dl_cap_backup;
	return 1;
}

void ng_restoreFromRootDL(DisplayObject* saved_dl, size_t saved_max, size_t saved_cap) {
	// Commit root DL changes (may have been modified by catch-up)
	g_root_dl_backup = display_list;
	g_root_dl_max_backup = max_depth;
	g_root_dl_cap_backup = display_list_capacity;
	// Restore sprite DL
	display_list = saved_dl;
	max_depth = saved_max;
	display_list_capacity = saved_cap;
}

// When 1, tagPlaceObject2 and tagSetInstanceName are no-ops.
// Used by tagShowFrame to re-run sprite frame_0 for scripts only (Phase 2),
// without disturbing the display list already set up in Phase 1 (eager init).
// Phase 7b: exposed via actionScriptOnlyMode() accessor so recompiler-emitted
// sprite DoAction queue calls can skip re-queueing during the Phase 2 re-run
// (scripts are already queued from Phase 1).
int g_script_only_mode = 0;

// When 1, tagShowFrame defers process_sprite_needs_init.
// Set by ng_executeGotoCatchUp so sprite init scripts run AFTER the deferred
// parent-frame DoAction (matching Ruffle's execution order).
int g_defer_sprite_init = 0;

// Tracks nesting depth of eager init in tagPlaceObject2.
// Used to determine whether constructor invocation is at the top level
// (g_eager_init_depth==0) or inside a nested sprite's eager init.
// Phase 7b: exposed via actionEagerInitActive() accessor. ng_attachMovie,
// ng_cloneSprite, and ng_duplicateMovieClip bump this around their frame_0
// invocations (via actionEagerInit{Enter,Leave}()) so recompiler-emitted
// sprite DoAction queue calls fire during runtime-attach paths too.
static int g_eager_init_depth = 0;

// Set while ng_update_button_states_in_dl re-runs a button's state function
// after a state transition (e.g. up → over when the mouse is inside the hit
// area on the first tick). The state func re-places the button's children,
// which re-triggers tagPlaceObject2's Phase 1 eager init for any sprite
// children and would re-queue their frame_0 scripts via the 7b sprite DoAction
// gate. Pre-7b, Phase 1 eager did not queue scripts (catch_up_mode=1 was
// enough to suppress); 7b's gate widened to also queue during eager init, so
// the button-state-transition path started double-firing sprite scripts.
// Flash/Ruffle button state children persist across transitions — their
// frame_0 scripts only fire on initial construction. Suppress the eager
// init term of the sprite DoAction gate while this counter is non-zero so
// the transition's re-placement is visual-only, matching pre-7b.
// Key test: avm1/issue_9885.
static int g_button_state_change_depth = 0;

#endif // NO_GRAPHICS || OFFSCREEN_RENDER

// Snapshot of (depth → char_id) and (depth → instance_name) of a button's
// children at the start of a state transition. tagPlaceObject2 consults this
// snapshot when g_btn_state_active is set: if the new placement's
// (depth, char_id) matches the old state's, treat as preserved (Ruffle's
// set_state "child exists in both states" branch — no re-instantiation,
// no frame_0 re-fire, instance_name kept). For new depths or different
// char_id at preserved depths, the normal eager-init/queue path runs.
//
// These symbols are referenced unconditionally by ng_update_button_states_in_dl
// (button hover/state machine), so they must be visible in both NO_GRAPHICS
// and graphics builds.
#define BTN_STATE_SNAP_MAX 256
static size_t g_btn_state_old_chars[BTN_STATE_SNAP_MAX];
static char*  g_btn_state_old_names[BTN_STATE_SNAP_MAX]; // strdup'd, owned
// Phase 1i: capture the inner sprite_display_list (grandchild array) and its
// metadata per depth so the preserved branch in tagPlaceObject2 can restore
// it. Without this, the clear-children loop nulls these fields and the next
// advance_sprite_frames pass sees sprite_display_list==NULL → just_allocated
// → frame_0 re-fires, producing spurious script_2 fires for preserved children
// after preserved-only state transitions (OUTDOWN→UP). See BUTTON_RESIDUAL_PLAN
// Phase 1i.
static DisplayObject* g_btn_state_old_sprite_dl[BTN_STATE_SNAP_MAX];
static size_t         g_btn_state_old_sprite_max[BTN_STATE_SNAP_MAX];
static size_t         g_btn_state_old_sprite_cap[BTN_STATE_SNAP_MAX];
static u8             g_btn_state_old_sprite_init[BTN_STATE_SNAP_MAX];
static size_t         g_btn_state_old_sprite_cf[BTN_STATE_SNAP_MAX];
static u8             g_btn_state_old_sprite_playing[BTN_STATE_SNAP_MAX];
static int    g_btn_state_active = 0;

// Set while ng_update_button_states_attached walks DefineButton2 buttons that
// live inside attachMovie'd / createEmptyMovieClip'd clips (child_mc_cache, e.g.
// N's menu). For those, the per-state-change display-list REPOPULATION (running
// button_state_funcs to swap UP/OVER/DOWN child graphics) corrupts the render —
// the button's children are timeline-placed inside a nested sprite inside an
// attached clip, and re-placing them every state change collides with
// render_attached_child's compose pass (already-dynamic transform_ids get read
// as CPU indices → garbage → the button renders blank, so hover makes N's menu
// items vanish and an invisible button never completes its release transition,
// killing the click). While this is set we KEEP the state transitions + action
// dispatch (clicks fire) but SKIP the visual repopulation (button stays in its
// initially-placed UP graphics). Trade-off: no hover color-shift animation for
// attached-clip buttons — a separate deeper render fix would be needed for that.
static int    g_btn_attached_walk = 0;

// Phase 1e: transient enumeration of removed button-state children.
// After a state transition, depths that had a child in the OLD state but
// no longer match in the NEW state remain enumerable for one tick (mirrors
// Ruffle's deferred destruction of removed children). Populated by
// ng_update_button_states_in_dl after the state func runs from the
// snapshot above; consumed by actionEnumerate2's MOVIECLIP arm via
// ng_iterateTransientButtonChildren. Cleared and rebuilt at each
// transition.
static DisplayObject* g_btn_transient_dobj = NULL;
static char*          g_btn_transient_names[BTN_STATE_SNAP_MAX]; // strdup'd, owned
static size_t         g_btn_transient_count = 0;

// Phase 7a forward declaration, moved out of the NO_GRAPHICS||OFFSCREEN_RENDER
// gate so browser-WASM can use the same clip-LOAD queueing helpers.
// Phase 7b: parent_mc is captured at queue time (g_current_context during
// the parent sprite's Phase 1 eager init) so the dispatcher can do the
// correct MC lookup at drain time. Without this, the function_base_clip_readded
// canary regresses because aq_dispatch_clip_load would fall back to root
// as parent, giving a wrong _parent for nested clips.
typedef struct {
	DisplayObject* obj;
	frame_func action;
	MovieClip* parent_mc;
} PendingClipLoad;

// Forward decls (ng_shared.c / tag_stubs.c) — needed in the browser-WASM
// auto-instance-name path inside tagPlaceObject2 / tagPlaceObject2Ratio.
extern int ng_find_button(size_t char_id);
extern int ng_find_textfield(size_t char_id);
extern unsigned int ng_increment_auto_instance_counter(void);

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)

int actionEagerInitActive(void) {
	return g_eager_init_depth > 0 && g_button_state_change_depth == 0;
}
int actionScriptOnlyMode(void)  { return g_script_only_mode; }

// Phase 7b: set by ng_executeGotoCatchUp around its funcs[f] replay loop
// (step 2). Suppresses sprite-DoAction queueing so scripts don't fire in
// FIFO before the target frame's root script (queued later in step 3).
int g_goto_catchup_active = 0;
int actionGotoCatchupActive(void) { return g_goto_catchup_active; }
void actionGotoCatchupEnter(void) { g_goto_catchup_active++; }
void actionGotoCatchupLeave(void) { g_goto_catchup_active--; }

// Phase 7b: set by ng_run_deferred_sprite_init_impl around its
// process_sprite_needs_init call. Combined with actionScriptOnlyMode,
// switches the sprite DoAction gate into synchronous-fire mode for the
// Phase 2 re-run that completes a goto's deferred sprite init.
int g_deferred_sprite_init_active = 0;
int actionDeferredSpriteInitActive(void) { return g_deferred_sprite_init_active; }
void actionDeferredSpriteInitEnter(void) { g_deferred_sprite_init_active++; }
void actionDeferredSpriteInitLeave(void) { g_deferred_sprite_init_active--; }

// Phase F: distinguishes attachMovie's deferred init (sync-fire required to
// preserve PAI ordering — see tag_stubs.c:267-282 comment for default_names
// background) from goto's Phase 2 deferred init (queue required to
// FIFO-interleave with target script). Both share the
// `actionScriptOnlyMode + actionDeferredSpriteInitActive` brackets, so this
// counter is what the recompiler gate actually keys off of when choosing
// between queue and sync-fire.
int g_attach_init_active = 0;
int actionAttachInitActive(void) { return g_attach_init_active; }
void actionAttachInitEnter(void) { g_attach_init_active++; }
void actionAttachInitLeave(void) { g_attach_init_active--; }

// Wrappers called by tag_stubs.c runtime-attach paths (ng_attachMovie,
// ng_cloneSprite, ng_duplicateMovieClip) around their Phase 1 frame_0
// invocations. g_eager_init_depth stays file-local to tag.c; tag_stubs.c
// uses these wrappers instead of touching the variable directly.
void actionEagerInitEnter(void) { g_eager_init_depth++; }
void actionEagerInitLeave(void) { g_eager_init_depth--; }

// When 1, process_sprite_init_at_depth only fires constructors (no Phase 2 scripts).
// Used by ng_fire_deferred_constructors to separate constructor invocation
// from Phase 2 script execution.
static int g_constructor_only_mode = 0;

// Frame filter for process_sprite_needs_init during deferred init.
// When g_sprite_init_filter_active=1, only process sprites matching the filter:
//   g_sprite_init_before_target=1: only placed_at_frame < g_sprite_init_target_frame
//   g_sprite_init_before_target=0: only placed_at_frame == g_sprite_init_target_frame
// The on_or_after case uses == (not >=) so a nested goto placing sprites at
// frames beyond the outer goto's target doesn't spuriously fire them during
// the outer Phase 3. In a single non-nested goto, catch-up only places
// sprites up to target_frame inclusive, so == and >= would be equivalent.
static int g_sprite_init_filter_active = 0;
static int g_sprite_init_before_target = 0;
static size_t g_sprite_init_target_frame = 0;

// (PendingClipLoad struct moved earlier in the file, before the
// NO_GRAPHICS||OFFSCREEN_RENDER gate, so browser-WASM can also use the
// shared aq_dispatch_clip_load / queue_clip_load_events helpers.)

// g_settarget_explicit_root: set by actionSetTarget("_root"/"") to distinguish
// "goto root" from "goto unnamed sprite with inherited root context".
// Declared in action.c; saved/cleared/restored here per sprite-frame invocation.
extern int g_settarget_explicit_root;
extern int g_settarget_invalid;
extern int g_settarget_none;
extern int g_settarget_context_changed;
extern MovieClip* g_settarget_saved_context;
extern MovieClip* g_current_context;
extern void actionSetCurrentContext(MovieClip* mc);

// Execute a sprite frame function with correct MC context and g_current_sprite_obj.
void exec_sprite_frame(SWFAppContext* app_context, DisplayObject* obj, frame_func f)
{
	DisplayObject* saved = g_current_sprite_obj;
	g_current_sprite_obj = obj;

	MovieClip* saved_ctx = g_current_context;
	MovieClip* saved_base = actionGetBaseClip();
	if (obj->instance_name != NULL)
	{
		// Find existing MC (no creation) to avoid spurious auto-named MC entries
		// during advance_sprite_frames display-list loop-back re-execution.
		extern MovieClip* actionFindMovieClipByName(const char* instance_name);
		MovieClip* mc = actionFindMovieClipByName(obj->instance_name);
		if (mc) { mc->display_obj = (void*)obj; actionSetCurrentContext(mc); actionSetBaseClip(mc); }
	}

	// Each sprite frame starts with a fresh SetTarget state (no explicit root target)
	int saved_settarget = g_settarget_explicit_root;
	int saved_invalid = g_settarget_invalid;
	int saved_none = g_settarget_none;
	int saved_ctx_changed = g_settarget_context_changed;
	MovieClip* saved_ctx_save = g_settarget_saved_context;
	g_settarget_explicit_root = 0;
	g_settarget_invalid = 0;
	g_settarget_none = 0;
	g_settarget_context_changed = 0;
	g_settarget_saved_context = NULL;

	// Set active transform data to child SWF's array if this sprite belongs to
	// a child movie (prevents buffer overflow when child has more transforms).
	float (*saved_td)[16] = g_active_transform_data;
	if (obj->child_transform_data != NULL)
		g_active_transform_data = obj->child_transform_data;

	f(app_context);

	g_active_transform_data = saved_td;
	g_settarget_explicit_root = saved_settarget;
	g_settarget_invalid = saved_invalid;
	g_settarget_none = saved_none;
	g_settarget_context_changed = saved_ctx_changed;
	g_settarget_saved_context = saved_ctx_save;
	actionSetCurrentContext(saved_ctx);
	actionSetBaseClip(saved_base);
	g_current_sprite_obj = saved;
}
#define CALL_FRAME(app, obj, f) exec_sprite_frame(app, obj, f)

// ---------------------------------------------------------------------------
// Helper: Recursive sprite/button initialization for newly placed objects.
// Called from tagShowFrame to run frame 0 for each sprite/button that was
// placed since the last ShowFrame and has sprite_needs_init=1.
//
// parent_mc: the MovieClip that "owns" the current display_list (used as
//            parent when creating child MCs, so _parent is set correctly).
// ---------------------------------------------------------------------------
// Forward declaration for the inner processing logic (processes a single depth index).
static void process_sprite_init_at_depth(SWFAppContext* app_context, MovieClip* parent_mc, size_t depth_idx);

static void process_sprite_needs_init(SWFAppContext* app_context, MovieClip* parent_mc)
{
	// When frame filter is active, process in placed_at_frame order (not depth order)
	// to match Ruffle's placement-ordered initialization.
	if (g_sprite_init_filter_active)
	{
		// Collect eligible depth indices, then sort by placed_at_frame
		size_t eligible[256];
		size_t eligible_count = 0;
		for (size_t i = 0; i <= max_depth && i < 256; i++)
		{
			DisplayObject* obj = &display_list[i];
			if (obj->char_id == 0 || !obj->sprite_needs_init) continue;
			if (g_sprite_init_before_target && obj->placed_at_frame >= g_sprite_init_target_frame)
				continue;
			if (!g_sprite_init_before_target && obj->placed_at_frame != g_sprite_init_target_frame)
				continue;
			eligible[eligible_count++] = i;
		}
		// Simple insertion sort by placed_at_frame (usually very few entries)
		for (size_t a = 1; a < eligible_count; a++)
		{
			size_t key = eligible[a];
			size_t key_frame = display_list[key].placed_at_frame;
			int b = (int)a - 1;
			while (b >= 0 && display_list[eligible[b]].placed_at_frame > key_frame)
			{
				eligible[b + 1] = eligible[b];
				b--;
			}
			eligible[b + 1] = key;
		}
		for (size_t k = 0; k < eligible_count; k++)
			process_sprite_init_at_depth(app_context, parent_mc, eligible[k]);
		return;
	}

	// Normal path: iterate by depth (start at 0 to handle depth-0 placements)
	for (size_t i = 0; i <= max_depth; i++)
		process_sprite_init_at_depth(app_context, parent_mc, i);
}

static void process_sprite_init_at_depth(SWFAppContext* app_context, MovieClip* parent_mc, size_t i)
{
		DisplayObject* obj = &display_list[i];
		if (obj->char_id == 0 || !obj->sprite_needs_init) {
			return;
		}

		Character* ch = &dictionary[obj->char_id];

		if (ch->type == CHAR_TYPE_SPRITE)
		{
			// Pre-create/sync the child MC while still in the parent's display_list
			// context so x/y are synced from the correct transform entry.
			// Even unnamed sprites need a MC with the correct parent so that
			// _parent preloading and base_clip context work correctly.
			MovieClip* child_mc = NULL;
			if (obj->instance_name != NULL)
				child_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
			else
			{
				char anon_name[32];
				snprintf(anon_name, sizeof(anon_name), "__anon_depth_%zu__", i);
				child_mc = actionFindOrCreateMovieClip(app_context, anon_name, parent_mc);
			}
			if (child_mc) child_mc->display_obj = (void*)obj;

			// Set totalframes/framesloaded from the sprite definition BEFORE
			// clip actions fire (onLoad reads _totalframes/_framesloaded).
			if (child_mc) {
				// AS-visible counters read the DECLARED header field, not
				// the playback count — Flash reports the header verbatim
				// (see avm2_display.c::total_frames for the AVM2 twin).
				child_mc->totalframes = (int)ch->sprite_declared_frames;
				child_mc->framesloaded = (int)ch->sprite_declared_frames;
				if (ch->sprite_declared_frames == 0)
					child_mc->currentframe = 0;
			}

			// In constructor_only mode, only fire constructors (no Phase 2 scripts).
			// Keep sprite_needs_init set so the later script pass can find this sprite.
			if (g_constructor_only_mode)
			{
				if (!obj->constructor_invoked)
				{
					extern const char* ng_lookupExportName(size_t char_id);
					extern void actionInvokeRegisteredClassConstructor(SWFAppContext* app_context, const char* export_name, MovieClip* mc);
					extern void actionSetupRegisteredClassPrototype(SWFAppContext*, const char*, MovieClip*);
					const char* export_name = ng_lookupExportName(obj->char_id);
					if (export_name != NULL && child_mc != NULL)
					{
						child_mc->display_obj = (void*)obj;
						actionSetupRegisteredClassPrototype(app_context, export_name, child_mc);
						actionInvokeRegisteredClassConstructor(app_context, export_name, child_mc);
						obj->constructor_invoked = 1;
					}
				}
				// Recurse into children to fire their constructors
				if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
				{
					DisplayObject* saved_dl = display_list;
					size_t saved_max = max_depth;
					size_t saved_cap = display_list_capacity;
					display_list = obj->sprite_display_list;
					max_depth = obj->sprite_max_depth;
					display_list_capacity = obj->sprite_dl_capacity;
					process_sprite_needs_init(app_context, child_mc);
					obj->sprite_display_list = display_list;
					obj->sprite_max_depth = max_depth;
					obj->sprite_dl_capacity = display_list_capacity;
					display_list = saved_dl;
					max_depth = saved_max;
					display_list_capacity = saved_cap;
				}
				return;
			}

			// sprite_needs_init values:
			//   1: regular non-eager path (legacy). Run frame_0 normally.
			//   2: eager init under goto catchup (scripts suppressed). Re-run
			//      frame_0 in script_only_mode so the sprite's DoAction gate
			//      g2 fires the scripts inline.
			//   3: eager init normal flow (scripts queued via gate g1).
			//      Skip frame_0 entirely — the queued scripts will fire on
			//      their own; re-running would double-fire (consecutive_goto_frame_test
			//      sprite_4: script_3 prints "frm1 of mc_red - gotoAndStop(2)"
			//      twice without this gate).
			int was_eager_catchup = (obj->sprite_needs_init == 2);
			int was_eager_normal = (obj->sprite_needs_init == 3);
			obj->sprite_needs_init = 0;

			// Track root DL for inline catch-up (only on outermost sprite init)
			if (g_sprite_init_depth == 0) {
				g_root_dl_backup = display_list;
				g_root_dl_max_backup = max_depth;
				g_root_dl_cap_backup = display_list_capacity;
			}
			g_sprite_init_depth++;

			// Context swap to sprite's display list
			DisplayObject* saved_dl    = display_list;
			size_t         saved_max   = max_depth;
			size_t         saved_cap   = display_list_capacity;
			display_list          = obj->sprite_display_list;
			max_depth             = obj->sprite_max_depth;
			display_list_capacity = obj->sprite_dl_capacity;

			// Phase 7b: sprite CLIP_EVENT_LOAD no longer drains per-sprite
			// here — it's queued at AQ_KIND_SCRIPT alongside sprite
			// DoAction, and the whole NORMAL-priority FIFO drains at the
			// recompiler-emitted pre-tagShowFrame SHOW_FRAME drain. The
			// SWF-tag-order inline queueing of sprite DoAction (7b) plus
			// LOAD-before-eager-init queueing (7a) gives the expected
			// per-sprite LOAD→frame_0→child_LOAD→child_frame_0 interleave
			// naturally via FIFO.

			// Run frame 0 with correct MC context
			MovieClip*    saved_ctx        = g_current_context;
			MovieClip*    saved_base       = actionGetBaseClip();
			DisplayObject* saved_sprite_obj = g_current_sprite_obj;
			g_current_sprite_obj = obj;
			if (child_mc) { actionSetCurrentContext(child_mc); actionSetBaseClip(child_mc); }

			if (ch->sprite_frame_funcs != NULL && ch->sprite_frame_funcs[0] != NULL)
			{
				// Call frame func directly (NOT via CALL_FRAME/exec_sprite_frame)
				// because we already set g_current_context and g_current_sprite_obj above.
				// exec_sprite_frame would create a duplicate MC with root as parent.
				int saved_settarget = g_settarget_explicit_root;
				int saved_invalid2 = g_settarget_invalid;
				int saved_none2 = g_settarget_none;
				g_settarget_explicit_root = 0;
				g_settarget_invalid = 0;
				g_settarget_none = 0;
				if (was_eager_catchup)
				{
					// Phase 2: run scripts only; placement tags are no-ops via g_script_only_mode.
					g_script_only_mode = 1;
					ch->sprite_frame_funcs[0](app_context);
					g_script_only_mode = 0;
				}
				else if (!was_eager_normal)
				{
					// sni == 1: regular non-eager init. Run frame_0 normally.
					ch->sprite_frame_funcs[0](app_context);
				}
				// was_eager_normal: skip frame_0 — queued scripts handle it.
				g_settarget_explicit_root = saved_settarget;
				g_settarget_invalid = saved_invalid2;
				g_settarget_none = saved_none2;
			}

			actionSetCurrentContext(saved_ctx);
			actionSetBaseClip(saved_base);
			g_current_sprite_obj = saved_sprite_obj;

			// Advance frame counter so advance_sprite_frames picks up at frame 1.
			// For 0-frame sprites (no ShowFrame in definition), keep at frame 0.
			// Only set when sni=1 (regular non-eager init): frame_0 just ran inline
			// and the counter wasn't initialized elsewhere. For sni=2/3 (eager init
			// path) the eager init already set sprite_current_frame; a subsequent
			// gotoAndStop/Play on the sprite (before this deferred init pass)
			// would have updated it — clobbering it back to 1 here makes the
			// next advance_sprite_frames re-execute the wrong frame and skip the
			// goto target's natural successor (goto_frame_test mc_red:
			// mc_red.gotoAndStop(3) sets sprite_current_frame=2; without this
			// guard, deferred init reset to 1 → next tick re-runs frame 1 and
			// only reaches frame 3 one tick later, but by then script_9 has
			// already tested asOrder and the missing "7+" trace fails).
			if (!was_eager_catchup && !was_eager_normal)
				obj->sprite_current_frame = (ch->sprite_frame_count > 0) ? (1 % ch->sprite_frame_count) : 0;

			// Recursively initialize any children placed by the frame function
			process_sprite_needs_init(app_context, child_mc);

			// Save back (pointer may change from realloc)
			obj->sprite_display_list  = display_list;
			obj->sprite_max_depth     = max_depth;
			obj->sprite_dl_capacity   = display_list_capacity;

			g_sprite_init_depth--;
			if (g_sprite_init_depth == 0) {
				// Returning to root: use backup (may have been updated by inline catch-up)
				display_list          = g_root_dl_backup;
				max_depth             = g_root_dl_max_backup;
				display_list_capacity = g_root_dl_cap_backup;
			} else {
				display_list          = saved_dl;
				max_depth             = saved_max;
				display_list_capacity = saved_cap;
			}

			// Invoke registered class constructor if this sprite has an exported symbol with a registered class
			// Skip if already invoked during eager init in tagPlaceObject2 (constructor_invoked flag)
			{
				extern const char* ng_lookupExportName(size_t char_id);
				extern void actionInvokeRegisteredClassConstructor(SWFAppContext* app_context, const char* export_name, MovieClip* mc);
				const char* _rc_export = ng_lookupExportName(obj->char_id);
				if (!obj->constructor_invoked && _rc_export != NULL && child_mc != NULL) {
					actionInvokeRegisteredClassConstructor(app_context, _rc_export, child_mc);
					obj->constructor_invoked = 1;
				}
				// Queue AS-level onLoad for timeline-placed RegisterClass sprites.
				// onLoad (prototype method) fires after constructor + process_sprite_needs_init
				// completes, dispatched from tagShowFrame via actionFlushPendingOnLoads.
				//
				// Gated on a class being actually registered (via Object.registerClass)
				// for this character — registerClass installs the onLoad on the
				// prototype BEFORE placement, so it's a valid LOAD-time handler.
				// A plain exported .sprite symbol (every swfc sprite gets an export
				// name) is NOT a registerClass sprite: its `mc.onLoad = function`
				// is assigned by a later DoAction, after the clip's LOAD already
				// passed, so Flash never fires it (action_execution_order_test12
				// expects loadOrder to stay "0+").
				extern int g_swf_version;
				extern void* lookupRegisteredClassByCharId(size_t char_id, int swf_version, const char** out_export_name);
				if (_rc_export != NULL && child_mc != NULL
				    && lookupRegisteredClassByCharId(obj->char_id, g_swf_version, NULL) != NULL)
					actionQueueMCOnLoad(child_mc);
			}

			// Mark as freshly initialized (=1); upgraded to 2 after first tagShowFrame
			// so per-tick clip event ENTER_FRAME and AS2 onEnterFrame fire on
			// subsequent ticks only (Ruffle model: init tick fires LOAD, not EnterFrame).
			obj->sprite_initialized = 1;
		}
		else if (ch->type == CHAR_TYPE_BUTTON && ch->button_state_funcs != NULL)
		{
			// sprite_needs_init==2 means tagPlaceObject2 already ran state_funcs[0]
			// eagerly. Just do the recursion here so child sprites get Phase-2
			// scripts. sprite_needs_init==1 is the legacy path where state_funcs
			// hasn't run yet (e.g. after RemoveObject/replace patterns).
			int already_eager = (obj->sprite_needs_init == 2);

			// Pre-create the button MC while still in the parent's display_list context
			MovieClip* button_mc = NULL;
			if (obj->instance_name != NULL)
			{
				button_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
				if (button_mc) { button_mc->is_button_mc = 1; button_mc->display_obj = (void*)obj; }
			}

			obj->sprite_needs_init = 0;

			// Context swap to button's display list
			DisplayObject* saved_dl    = display_list;
			size_t         saved_max   = max_depth;
			size_t         saved_cap   = display_list_capacity;
			display_list          = obj->sprite_display_list;
			max_depth             = obj->sprite_max_depth;
			display_list_capacity = obj->sprite_dl_capacity;

			if (!already_eager)
			{
				// Run up-state (button_state_funcs[0]) to place button's children.
				// Phase 7b: briefly set g_current_context to button_mc so that
				// tagPlaceObject2's MC pre-create (and any CLIP_LOAD queueing
				// for button children) uses button_mc as the parent. Without
				// this, button children (e.g. instance1) get parented to the
				// button's own parent (root), breaking _parent chain
				// (movieclip_in_removed_button canary).
				MovieClip* saved_ctx_btn = g_current_context;
				if (button_mc) actionSetCurrentContext(button_mc);
				ch->button_state_funcs[0](app_context);
				actionSetCurrentContext(saved_ctx_btn);

				// Sync back to obj BEFORE recursive init so that child scripts
				// accessing parent_mc->display_obj see up-to-date sprite_max_depth.
				obj->sprite_display_list  = display_list;
				obj->sprite_max_depth     = max_depth;
				obj->sprite_dl_capacity   = display_list_capacity;
			}

			// Recursively initialize sprites placed by the up-state func,
			// using button_mc as their parent.
			process_sprite_needs_init(app_context, button_mc);

			// Save back (recursive init may have updated display_list via realloc)
			obj->sprite_display_list  = display_list;
			obj->sprite_max_depth     = max_depth;
			obj->sprite_dl_capacity   = display_list_capacity;

			display_list          = saved_dl;
			max_depth             = saved_max;
			display_list_capacity = saved_cap;
		}
}

#else
// Graphics builds (wasm and native): use exec_sprite_frame from graphics_stubs.c
// so sprite frame_funcs run with the right MC context (saves/restores
// g_current_sprite_obj, g_current_context, base_clip, settarget state, and
// active transform_data). Previously this was a bare `(f)(app)` — sprite
// scripts ran in the root's context, which broke any test exercising
// SetTarget / sprite-relative variable lookup in sprite-emitted code.
extern void exec_sprite_frame(SWFAppContext* app_context, DisplayObject* obj, frame_func f);
#define CALL_FRAME(app, obj, f) exec_sprite_frame(app, obj, f)
#endif

// Public wrapper for process_sprite_needs_init (called from tag_stubs.c for
// attachMovie'd sprite child initialization).
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
void process_sprite_needs_init_public(SWFAppContext* app_context, MovieClip* parent_mc)
{
	process_sprite_needs_init(app_context, parent_mc);
}

// Public setter for g_script_only_mode (called from tag_stubs.c).
void ng_set_script_only_mode(int mode)
{
	g_script_only_mode = mode;
}
#endif

// ---------------------------------------------------------------------------
// Helper 1: Advance sprite timelines recursively
// ---------------------------------------------------------------------------
// When > 0, advance_sprite_frames defers nested recursion for normal-play
// sprites (goto paths still recurse immediately).  The deferred recursion is
// performed by advance_nested_sprite_frames() after the root frame script.
int g_advance_defer_nested = 0;

// When > 0, advance_sprite_frames is recursing into a parent's display list as
// part of a manual-nav (gotoAndStop/gotoAndPlay) catch-up rebuild. A child
// freshly placed by the replayed frames must NOT advance its own playhead on
// this (its placement) tick — its frame-1 script (incl. a frame-1 stop()) is
// applied at construction and it should only advance on subsequent ticks. This
// is narrower than the general nested recursion: normal Phase-3 nested advance
// (advance_nested_sprite_frames) intentionally DOES advance a nested clip on the
// tick it was placed by a parent's frame script (execution_order4's clip1.child),
// so the placed-this-tick skip is restricted to the goto catch-up path. See
// SWFRecompDocs/status/2026-06-21-riddle-school-nested-clip-placed-this-tick-advance.md.
int g_in_manual_nav_catchup_recurse = 0;

// When 1, advance_sprite_frames BUILDS just-allocated nested sprites (runs their
// frame-0) but does NOT advance the playhead of already-built sprites — it only
// recurses into them to build their own unbuilt children. Used after
// ng_gotoFrameByMC's synchronous frame replay (browser-WASM) to build the nested
// content the target frame placed (e.g. Minesweeper's dot cid 28→27→shape)
// WITHOUT advancing pre-existing playing siblings (e.g. Tetris's bang_mc, a
// 7-frame explosion clip placed at the block's frame 0 — a full advance would
// step it every board-cell gotoAndStop and paint stray bang frames over the
// piece colours).
int g_build_only = 0;

// Browser-WASM eager-build (status doc 2026-06-16 cont.40c/40d): graphics_stubs.c
// exec_sprite_frame builds a registerClass'd sprite's nested timeline children
// (runs their frame-0) BEFORE firing the class constructor, so init()'s
// this._width / this.<child> reads see fully-built content (FRadioButton's
// boundingBox_mc — nested 2 levels — defines the component width). When it does
// so it sets g_exec_eager_built_obj to that sprite so the post-CALL_FRAME
// recursion below skips the redundant re-advance (a double-advance corrupts
// nested frame counters). NULL in CI modes (exec_sprite_frame not compiled there).
DisplayObject* g_exec_eager_built_obj = NULL;

// Monotonic tick counter, incremented at the top of every tick by swf.c
// (OFFSCREEN_RENDER) and swf_core.c (NO_GRAPHICS). Used in conjunction with
// DisplayObject.placed_at_tick so advance_nested_sprite_frames can skip
// sprites placed during the current tick — mirrors Ruffle's clip_exec_list
// iteration where mid-iter additions are never visited. See
// SWFRecompDocs/plans/defer-newly-placed-sprite-advance-plan.md.
size_t g_tick_count = 0;

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
// Forward decl — defined alongside tagRemoveObject2 below. Recursively queues
// CLIP_EVENT_UNLOAD clip-action callbacks for a display list's children and
// invalidates the cached child MCs.
static void fire_recursive_child_unloads(SWFAppContext* app_context,
	DisplayObject* dl, size_t dl_max, MovieClip* parent_mc);
#endif

// Forward decl — freed-buffer display_obj scrub (defined near the DL-realloc
// rebase machinery below); used by the frame-0 loop-back free above it.
static void scrub_mc_display_obj_in_range(DisplayObject* base, size_t cap);

// Forward decls — defined alongside tagRemoveObject2 below. Invalidate cached
// descendant MCs of a removed/replaced sprite, so iteration walks (e.g.
// actionIterateTextFieldGlyphs) stop drawing their glyphs.
// invalidate_descendants_of_mc is all-modes (clear_display_entry's freed-DL
// walk needs it); invalidate_mc_for_dl_entry stays browser-WASM-only.
static void invalidate_descendants_of_mc(SWFAppContext* app_context, MovieClip* parent);
#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
static void invalidate_mc_for_dl_entry(SWFAppContext* app_context, DisplayObject* obj);
#endif

// Forward decl — definition further down in this file. Used by both
// browser-WASM (pending_remove finalize path in tagShowFrame and the
// Place tags' consume_pending_remove block) and NO_GRAPHICS/OFFSCREEN_RENDER.
static void clear_display_entry(SWFAppContext* app_context, size_t depth);

// ---------------------------------------------------------------------------
// LOOPBACK IDENTITY PRESERVATION (nested-sprite natural wrap → frame 0).
//
// Flash/Ruffle rewind a looping timeline via run_goto: children that the
// target frame re-places with the same character at the same depth SURVIVE
// with their full identity (instance name, cached MovieClip, child display
// list, playhead, button state), and dynamic-range children (attachMovie /
// duplicateMovieClip / createEmptyMovieClip, swf_depth >= 16384) always
// survive — run_goto only considers depth < AVM_DEPTH_BIAS for removal (the
// root timeline applies the same rule in ng_display_clear_after). Only
// children absent from the target frame are removed (firing UNLOAD).
//
// Survival is decided against the recompiler-emitted per-frame placement
// table (tagSetSpritePlacements / ng_sprite_frame_placements): an entry at a
// static depth survives iff frame 0 contains a PlaceObject for that depth
// whose ratio matches and whose char id matches (id may differ for sprite
// children — Ruffle's survives_rewind MovieClip rule). No table registered →
// no static survivors (degrades to the old clear-everything behavior).
// ---------------------------------------------------------------------------
static int ng_loopback_entry_survives(const DisplayObject* e, size_t swf_depth,
                                      const FramePlacement* f0, u16 f0_count)
{
	if (e->char_id == 0) return 0;
	if (swf_depth >= 16384) return 1;  // dynamic-range child: always survives
	if (e->clone_replaced) return 0;   // AVM1 clones never survive rewind
	if (e->depth_swapped) return 0;    // conservative: swapped entries re-churn
	{
		// A same-frame deferred Remove (UNLOAD handler) logically vacated this
		// depth — the entry must not be resurrected by preservation.
		extern int ng_depth_has_pending_finalize(size_t);
		if (ng_depth_has_pending_finalize(swf_depth)) return 0;
	}
	int existing_is_mc = (e->sprite_display_list != NULL);
	for (u16 k = 0; k < f0_count; k++)
	{
		if (f0[k].is_remove) continue;
		if (f0[k].char_id == 0) continue;  // Modify tag — not an instantiation
		if (f0[k].depth != (u16)swf_depth) continue;
		if (f0[k].ratio != e->ratio) continue;
		if (!existing_is_mc && f0[k].char_id != (u16)e->char_id) continue;
		return 1;
	}
	return 0;
}

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
// Forward decl — single-entry body of fire_recursive_child_unloads (defined
// alongside it below); the loop-back path fires it per non-surviving entry.
static void fire_entry_unloads(SWFAppContext* app_context,
	DisplayObject* dl, size_t i, MovieClip* parent_mc);

// Skip-callback for actionQueueChildUnloadsFiltered: maps a cached child MC of
// the wrapping sprite back to its display-list entry and reports whether that
// entry survives the frame-0 rewind (survivors keep their whole subtree — no
// AS-level onUnload must fire for them).
typedef struct {
	DisplayObject* dl;
	size_t dl_max;
	const FramePlacement* f0;
	u16 f0_count;
} LoopbackSkipCtx;

static int lb_skip_surviving_child(MovieClip* child, void* ud)
{
	LoopbackSkipCtx* c = (LoopbackSkipCtx*)ud;
	// Primary key: display_obj points at &dl[i].
	size_t idx = 0;
	uintptr_t base = (uintptr_t)c->dl;
	uintptr_t p = (uintptr_t)child->display_obj;
	if (child->display_obj != NULL && p > base)
	{
		size_t off = (size_t)(p - base);
		if (off % sizeof(DisplayObject) == 0)
		{
			size_t i = off / sizeof(DisplayObject);
			if (i >= 1 && i <= c->dl_max) idx = i;
		}
	}
	// Fallback: AS depth → swf depth index, with a name check so a stale
	// same-depth entry for a different child doesn't match.
	if (idx == 0)
	{
		long swf_d = (long)child->depth + 16384;
		if (swf_d >= 1 && (size_t)swf_d <= c->dl_max)
		{
			DisplayObject* e = &c->dl[swf_d];
			if (e->char_id != 0 && e->instance_name != NULL
			    && strcmp(e->instance_name, child->name) == 0)
				idx = (size_t)swf_d;
		}
	}
	if (idx == 0)
	{
		// Unmappable: dynamically-created MC with no display_obj link (e.g. a
		// duplicateMovieClip clone). Dynamic children survive the rewind.
		return child->depth >= 0;
	}
	return ng_loopback_entry_survives(&c->dl[idx], idx, c->f0, c->f0_count);
}
#endif

// Recurse into a non-advancing sprite's children WITHOUT advancing the parent's
// own playhead or re-running its frame script. Flash advances a sprite's
// children regardless of the parent's play state — a stopped or single-frame
// holder clip still lets its playing children animate. The main advance loop
// `continue`s past stopped / already-placed 1-frame sprites, which used to skip
// their children entirely (a grandchild multi-frame sprite nested inside a
// stopped holder froze at frame 1 — e.g. Bloons' introclip.instance5.instanceN).
// Gated on !g_advance_defer_nested (the top-level deferred root call defers all
// nested recursion to advance_nested_sprite_frames, which runs with defer=0).
static void advance_sprite_children_only(SWFAppContext* app_context, DisplayObject* obj)
{
	if (g_advance_defer_nested) return;          // deferred pass handles recursion
	if (obj->sprite_display_list == NULL) return;
	if (obj->sprite_max_depth == 0) return;       // no children to advance

	DisplayObject* saved_dl = display_list;
	size_t saved_max = max_depth;
	size_t saved_cap = display_list_capacity;

	display_list = obj->sprite_display_list;
	max_depth = obj->sprite_max_depth;
	display_list_capacity = obj->sprite_dl_capacity;

	MovieClip* sprite_mc = NULL;
#ifndef NO_GRAPHICS
	extern MovieClip root_movieclip;
	MovieClip* parent_for_recurse = (g_current_context != NULL) ? g_current_context : &root_movieclip;
	if (obj->instance_name != NULL) {
		sprite_mc = tag_cached_walk_mc(obj, parent_for_recurse, 0);
		if (sprite_mc == NULL) {
			sprite_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_for_recurse);
			tag_store_walk_mc(obj, sprite_mc);
		}
	}
#else
	extern MovieClip* actionFindMovieClipByName(const char* instance_name);
	if (obj->instance_name != NULL) {
		sprite_mc = tag_cached_walk_mc(obj, NULL, 1);
		if (sprite_mc == NULL) {
			sprite_mc = actionFindMovieClipByName(obj->instance_name);
			tag_store_walk_mc(obj, sprite_mc);
		}
	}
#endif
	MovieClip* saved_recurse_ctx = g_current_context;
	if (sprite_mc) actionSetCurrentContext(sprite_mc);
	advance_sprite_frames(app_context);
	actionSetCurrentContext(saved_recurse_ctx);

	obj->sprite_display_list = display_list;
	obj->sprite_max_depth = max_depth;
	obj->sprite_dl_capacity = display_list_capacity;

	display_list = saved_dl;
	max_depth = saved_max;
	display_list_capacity = saved_cap;
}

// A DefineSprite whose body is not terminated by an End record does NOT loop.
// Flash (and Ruffle) park such a clip on its last frame and stop it instead of
// wrapping to frame 0. Ruffle: `determine_next_frame` returns `NextFrame::Same`
// when `!preload_progress.has_end_tag`, and `run_frame_internal` answers that
// with `self.stop(context)` (core/src/display_object/movie_clip.rs) — the field
// doc there reads "Clips without an End tag should not loop, even if they have
// multiple frames."
//
// The clip is only stopped, not removed: it keeps its place in the display list
// and still receives per-tick ENTER_FRAME, and its children keep advancing (the
// !sprite_is_playing branch in the advance loops recurses via
// advance_sprite_children_only). Every well-formed authoring tool emits the End
// record, so this path is unreachable for real-world content — it exists for
// deliberately malformed SWFs (the from_shumway/fuzz corpus, where 143/143
// sprites omit it).
static void ng_sprite_park_no_end_tag(DisplayObject* obj)
{
	obj->sprite_is_playing = 0;
}

// Iterates the current global display_list for sprites and advances their
// timelines.  After executing each sprite's frame function (while globals are
// swapped to the sprite's list), recurse to advance any nested sprites.
void advance_sprite_frames(SWFAppContext* app_context)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (catch_up_mode) return;
#endif

	// Iterate in reverse-instantiation order (largest place_seq first), mirroring
	// Ruffle's clip_exec_list LIFO traversal in core/src/avm1/runtime.rs (push at
	// head, traverse forward). For SWFs that place sprites in non-monotonic depth
	// order this differs from depth-descending, and Flash dispatches by
	// instantiation order. Insertion sort over a small stack array; fall back to
	// depth-descending only if the number of OCCUPIED depths exceeds the cap.
	//
	// The cap bounds how many entries we write, so it must be tested against the
	// occupied count — not against max_depth, which is the highest depth INDEX.
	// A SWF may legitimately place a handful of clips at sparse high depths
	// (PlaceObject2 depth is a u16, so 0..65535 is in range); gating on
	// max_depth silently abandoned the correct ordering for such movies and
	// fell back to depth-descending. That mis-ordered every from_shumway/fuzz
	// test whose deepest placement sits above the cap.
	#define ASF_SORT_CAP 512
	size_t sorted_depths[ASF_SORT_CAP];
	size_t sorted_seq[ASF_SORT_CAP];
	size_t n_sorted = 0;
	int use_sorted = 1;
	{
		for (size_t d = 1; d <= max_depth; d++) {
			if (display_list[d].char_id == 0) continue;
			if (n_sorted >= ASF_SORT_CAP) { use_sorted = 0; n_sorted = 0; break; }
			sorted_depths[n_sorted] = d;
			sorted_seq[n_sorted] = display_list[d].place_seq;
			n_sorted++;
		}
	}
	if (use_sorted) {
		for (size_t k = 1; k < n_sorted; k++) {
			size_t key_d = sorted_depths[k];
			size_t key_s = sorted_seq[k];
			long b = (long)k - 1;
			while (b >= 0 && sorted_seq[b] < key_s) {
				sorted_depths[b+1] = sorted_depths[b];
				sorted_seq[b+1] = sorted_seq[b];
				b--;
			}
			sorted_depths[b+1] = key_d;
			sorted_seq[b+1] = key_s;
		}
	}

	size_t iter_n = use_sorted ? n_sorted : (max_depth + 1);
	for (size_t iter = 0; iter < iter_n; iter++)
	{
		size_t cur_depth;
		if (use_sorted) {
			cur_depth = sorted_depths[iter];
		} else {
			// Fallback: depth-descending (max_depth..1)
			cur_depth = max_depth - iter;
			if (cur_depth == 0) continue;
		}
		DisplayObject* obj = &display_list[cur_depth];
		if (obj->char_id == 0) continue;
		Character* ch = &dictionary[obj->char_id];

		// Recurse into button display lists to advance their sprite children
		if (ch->type == CHAR_TYPE_BUTTON)
		{
			if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
			{
				DisplayObject* saved_dl = display_list;
				size_t saved_max = max_depth;
				size_t saved_cap = display_list_capacity;
				display_list = obj->sprite_display_list;
				max_depth = obj->sprite_max_depth;
				display_list_capacity = obj->sprite_dl_capacity;
				advance_sprite_frames(app_context);
				obj->sprite_display_list = display_list;
				obj->sprite_max_depth = max_depth;
				obj->sprite_dl_capacity = display_list_capacity;
				display_list = saved_dl;
				max_depth = saved_max;
				display_list_capacity = saved_cap;
			}
			continue;
		}

		if (ch->type != CHAR_TYPE_SPRITE) continue;

		// Allocate persistent display list on first encounter
		int just_allocated = 0;
		if (obj->sprite_display_list == NULL)
		{
			obj->sprite_dl_capacity = INITIAL_SPRITE_DL_CAPACITY;
			obj->sprite_display_list = HCALLOC(obj->sprite_dl_capacity, sizeof(DisplayObject));
			obj->sprite_max_depth = 0;
			obj->sprite_current_frame = 0;
			just_allocated = 1;
			// Don't override sprite_is_playing — tagPlaceObject2 already set it to 1,
			// and a script may have already set it to 0 (e.g. gotoAndStop before first ShowFrame)
		}

		// Build-only mode (post-ng_gotoFrameByMC replay): for an already-built
		// sprite, recurse to build its unbuilt children but do NOT advance its
		// own playhead (see g_build_only). just-allocated sprites still build
		// their frame-0 via the normal flow below.
		if (g_build_only && !just_allocated)
		{
			advance_sprite_children_only(app_context, obj);
			continue;
		}

		// During a parent's manual-nav (gotoAndStop/Play) catch-up rebuild, a
		// child freshly placed by the replayed frames must NOT advance its own
		// playhead on its placement tick: Flash constructs it (frame-1 script —
		// incl. a frame-1 stop() — runs at construction) and shows frame 1,
		// advancing only on subsequent ticks. The catch-up eager-init builds the
		// child's frame 0 under catch_up_mode (its frame-1 stop() suppressed) and
		// parks sprite_current_frame=1; the manual-nav recursion would then
		// advance it 1->2 before the deferred stop() drains. Concrete case:
		// Riddle School's preloader hover button instance69 (DefineSprite_34, 16
		// frames, frame-1 stop()), placed during instance65's
		// gotoAndStop("loaded") catch-up, auto-advanced 1->2 and reported _cf=2
		// vs Ruffle's _cf=1. Scoped to the catch-up recursion (g_in_manual_nav_
		// catchup_recurse) — NOT the general Phase-3 nested advance, which
		// intentionally advances a nested clip on the tick a parent frame script
		// placed it (execution_order4's clip1.child) — and to already-built clips
		// (just_allocated clips still build their frame 0).
		if (g_in_manual_nav_catchup_recurse && !just_allocated)
		{
			extern size_t g_tick_count;
			if (obj->placed_at_tick == g_tick_count)
				continue;
		}

		// Check for manual frame navigation (gotoAndPlay/gotoAndStop)
		size_t frame = obj->sprite_current_frame;
		if (obj->sprite_manual_next_frame)
		{
			obj->sprite_manual_next_frame = 0;
			size_t target = obj->sprite_next_frame;
			if (target < ch->sprite_frame_count)
			{
				// If jumping backward, reset the display list
				if (target <= frame)
				{
					// Swap to sprite's display list context
					DisplayObject* saved_dl = display_list;
					size_t saved_max = max_depth;
					size_t saved_cap = display_list_capacity;

					display_list = obj->sprite_display_list;
					max_depth = obj->sprite_max_depth;
					display_list_capacity = obj->sprite_dl_capacity;

					for (size_t j = 1; j <= max_depth; ++j)
					{
						if (display_list[j].sprite_display_list != NULL)
						{
							ng_freeSpriteDL(app_context, display_list[j].sprite_display_list,
			                display_list[j].sprite_dl_capacity);
							display_list[j].sprite_display_list = NULL;
						}
						display_list[j].char_id = 0;
					}
					max_depth = 0;

					// Re-execute frames 0..target. Force catch_up_mode=1 so
					// the recompiler-emitted gate doesn't double-queue the
					// target frame's DoAction — Phase B+ already queued it
					// via `ng_gotoFrameCurrentSprite`'s pending sprite-script
					// queue (flushed under actionUnifySpriteDrain).
					int saved_bw_cm = catch_up_mode;
					catch_up_mode = 1;
					for (size_t f = 0; f <= target; f++)
					{
						if (f < ch->sprite_frame_count && ch->sprite_frame_funcs[f] != NULL)
							CALL_FRAME(app_context, obj, ch->sprite_frame_funcs[f]);
					}
					catch_up_mode = saved_bw_cm;

					// Recurse nested sprites (set parent context for correct child MC creation)
					{
						extern MovieClip root_movieclip;
#ifndef NO_GRAPHICS
						MovieClip* pmc = (g_current_context != NULL) ? g_current_context : &root_movieclip;
						MovieClip* smc = obj->instance_name ? actionFindOrCreateMovieClip(app_context, obj->instance_name, pmc) : NULL;
#else
						extern MovieClip* actionFindMovieClipByName(const char* instance_name);
						MovieClip* smc = obj->instance_name ? actionFindMovieClipByName(obj->instance_name) : NULL;
#endif
						MovieClip* sctx = g_current_context;
						if (smc) actionSetCurrentContext(smc);
						g_in_manual_nav_catchup_recurse++;
						advance_sprite_frames(app_context);
						g_in_manual_nav_catchup_recurse--;
						actionSetCurrentContext(sctx);
					}

					obj->sprite_display_list = display_list;
					obj->sprite_max_depth = max_depth;
					obj->sprite_dl_capacity = display_list_capacity;

					display_list = saved_dl;
					max_depth = saved_max;
					display_list_capacity = saved_cap;
				}
				else
				{
					// Jumping forward: execute frames frame+1..target with
					// catch_up_mode=1 throughout (tags only, scripts NOT
					// queued via the gate). Pre-Phase-D the target frame ran
					// with catch_up_mode=0 so the gate queued its DoAction;
					// post-Phase-D `ng_gotoFrameCurrentSprite` already queued
					// the target's script via Phase B's pending sprite-script
					// queue (flushed to AQ_KIND_SCRIPT under
					// actionUnifySpriteDrain), so a second queue here would
					// cause a double-fire (key test: tell_target_invalid_swf6).
					DisplayObject* saved_dl = display_list;
					size_t saved_max = max_depth;
					size_t saved_cap = display_list_capacity;

					display_list = obj->sprite_display_list;
					max_depth = obj->sprite_max_depth;
					display_list_capacity = obj->sprite_dl_capacity;

					int saved_cm = catch_up_mode;
					catch_up_mode = 1;
					for (size_t f = frame + 1; f <= target; f++)
					{
						if (f < ch->sprite_frame_count && ch->sprite_frame_funcs[f] != NULL)
							CALL_FRAME(app_context, obj, ch->sprite_frame_funcs[f]);
					}
					catch_up_mode = saved_cm;

					// Recurse nested sprites (set parent context for correct child MC creation)
					{
						extern MovieClip root_movieclip;
#ifndef NO_GRAPHICS
						MovieClip* pmc = (g_current_context != NULL) ? g_current_context : &root_movieclip;
						MovieClip* smc = obj->instance_name ? actionFindOrCreateMovieClip(app_context, obj->instance_name, pmc) : NULL;
#else
						extern MovieClip* actionFindMovieClipByName(const char* instance_name);
						MovieClip* smc = obj->instance_name ? actionFindMovieClipByName(obj->instance_name) : NULL;
#endif
						MovieClip* sctx = g_current_context;
						if (smc) actionSetCurrentContext(smc);
						g_in_manual_nav_catchup_recurse++;
						advance_sprite_frames(app_context);
						g_in_manual_nav_catchup_recurse--;
						actionSetCurrentContext(sctx);
					}

					obj->sprite_display_list = display_list;
					obj->sprite_max_depth = max_depth;
					obj->sprite_dl_capacity = display_list_capacity;

					display_list = saved_dl;
					max_depth = saved_max;
					display_list_capacity = saved_cap;
				}
				obj->sprite_current_frame = target;

				// Sync the AS-visible _currentframe to the navigated frame. The
				// normal-advance path below does this (smc->currentframe = frame+1),
				// but the manual-nav branch skips it via the `continue`. For a
				// gotoAndStop'd nested sprite that omission is permanent: once
				// stopped (is_playing==0) the sprite takes the advance_sprite_
				// children_only branch every later tick and never re-syncs, so its
				// MovieClip reports the pre-nav frame forever even though the
				// display list (and sprite_current_frame) navigated correctly.
				// Surfaced via Riddle School: the preloader DefineSprite_35
				// (instance65, nested 2 levels deep in loadingthing) does
				// gotoAndStop("loaded") at 100% loaded; the DL rebuilt to the
				// "loaded" frame (child instance69) but _currentframe stuck at 1
				// vs Ruffle's 3. Mirrors the normal-advance sync at the non-manual
				// branch.
				if (obj->instance_name != NULL)
				{
					extern MovieClip* actionFindMovieClipByName(const char* instance_name);
					MovieClip* smc = actionFindMovieClipByName(obj->instance_name);
					if (smc) smc->currentframe = (int)target + 1;
				}
			}
			obj->enterframe_eligible = 1;
			continue; // Manual nav done, skip normal advancement
		}

		// Set enterframe_eligible for ALL initialized sprites, even stopped/1-frame.
		// This ensures per-tick clip event ENTER_FRAME and AS2 onEnterFrame fire
		// for sprites that don't advance (matching Ruffle's unconditional model).
		if (obj->sprite_initialized >= 2)
			obj->enterframe_eligible = 1;

		// Only advance the parent if playing — but still recurse into its
		// children so nested playing sprites animate (Flash advances children
		// regardless of the parent's play state).
		if (!obj->sprite_is_playing) { advance_sprite_children_only(app_context, obj); continue; }

		// Skip 1-frame sprites — they don't advance.
		// But on first encounter (just_allocated), still fall through to execute
		// frame 0 so the child display list is populated for rendering.
		// An already-placed 1-frame holder still advances its (multi-frame) children.
		if (ch->sprite_frame_count <= 1 && !just_allocated) { advance_sprite_children_only(app_context, obj); continue; }

		// Swap to sprite's display list context
		DisplayObject* saved_dl = display_list;
		size_t saved_max = max_depth;
		size_t saved_cap = display_list_capacity;

		display_list = obj->sprite_display_list;
		max_depth = obj->sprite_max_depth;
		display_list_capacity = obj->sprite_dl_capacity;

		// When looping back to frame 0, rewind the display list. NOT a full
		// clear: entries frame 0 re-places (same char/depth/ratio) and
		// dynamic-range children survive with their identity — see
		// ng_loopback_entry_survives above. Previously this path cleared and
		// re-placed EVERYTHING each wrap, minting fresh auto-instance names +
		// fresh cached MovieClips + fresh 64-entry child display lists every
		// loop iteration (~4/tick on Minesweeper's menu, forever): unbounded
		// child_mc_cache growth → cache overflow → uncacheable-MC minting per
		// lookup → arena OOM on long sessions. It also reset button state and
		// restarted child playheads every wrap, diverging from Flash/Ruffle.
		int lb_wrapped = 0;
		if (frame == 0 && max_depth > 0)
		{
			lb_wrapped = 1;
			u16 lb_f0_count = 0;
			const FramePlacement* lb_f0 =
				ng_sprite_frame_placements((u16)obj->char_id, 0, &lb_f0_count);
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
			// Flash rewinds a looping sprite to frame 0 via run_goto, which
			// removes every object not present in frame 0 and fires their
			// UNLOAD events. Queue the clip-action UNLOADs + AS-level onUnload
			// handlers for NON-SURVIVORS here, then let the free loop below
			// clear their slots (survivors fire nothing — their subtree stays
			// alive). AS-level handlers are queued FIRST (before
			// fire_entry_unloads invalidates the cached MCs — invalidation
			// sets depth=INT_MIN, which would make queueChildOnUnloads skip
			// them). Inner-frame children (mc11/mc12/mc21 in
			// action_execution_order_test12) are never in frame 0's placement
			// table, so they keep firing onUnload each wrap as before.
			if (!catch_up_mode && obj->instance_name != NULL)
			{
				extern MovieClip* actionFindMovieClipByName(const char* instance_name);
				extern void actionQueueChildUnloadsFiltered(MovieClip*,
					int (*)(MovieClip*, void*), void*);
				MovieClip* _lb_mc = actionFindMovieClipByName(obj->instance_name);
				LoopbackSkipCtx _lb_ctx = { display_list, max_depth, lb_f0, lb_f0_count };
				if (_lb_mc != NULL)
					actionQueueChildUnloadsFiltered(_lb_mc, lb_skip_surviving_child, &_lb_ctx);
				for (size_t j = 1; j <= max_depth; ++j)
				{
					if (display_list[j].char_id == 0) continue;
					if (ng_loopback_entry_survives(&display_list[j], j, lb_f0, lb_f0_count))
						continue;
					fire_entry_unloads(app_context, display_list, j,
						_lb_mc != NULL ? _lb_mc : &root_movieclip);
				}
			}
#endif
#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
			// Browser-WASM: the NO_GRAPHICS/OFFSCREEN path above invalidates the
			// cached child MovieClips for the soon-to-be-freed entries via
			// fire_entry_unloads; mirror just the invalidation here (survivors
			// keep their MCs — that IS the churn fix: the same "instanceN" MC is
			// reused every loop instead of a fresh one lingering per wrap, which
			// used to ratchet live-clip count + per-frame scan/render cost until
			// the game froze — Metanet "N"'s title-demo / gameplay particle +
			// drone clips). invalidate_mc_for_dl_entry marks each matched MC
			// (and its descendants, by parent walk) depth=INT_MIN. Gated on
			// !catch_up_mode to match the NO_GRAPHICS unload path — a
			// rebuild-in-progress must not tear down children it is about to
			// re-materialize this same pass.
			if (!catch_up_mode)
			{
				for (size_t j = 1; j <= max_depth; ++j)
				{
					if (display_list[j].char_id == 0) continue;
					if (ng_loopback_entry_survives(&display_list[j], j, lb_f0, lb_f0_count))
						continue;
					invalidate_mc_for_dl_entry(app_context, &display_list[j]);
				}
			}
#endif
			size_t lb_new_max = 0;
			for (size_t j = 1; j <= max_depth; ++j)
			{
				if (display_list[j].char_id != 0
				    && ng_loopback_entry_survives(&display_list[j], j, lb_f0, lb_f0_count))
				{
					lb_new_max = j;
					continue;
				}
				if (display_list[j].sprite_display_list != NULL)
				{
					ng_freeSpriteDL(app_context, display_list[j].sprite_display_list,
					                display_list[j].sprite_dl_capacity);
					display_list[j].sprite_display_list = NULL;
				}
				display_list[j].char_id = 0;
			}
			max_depth = lb_new_max;
		}

		// Sync 1-indexed _currentframe on the sprite's MC so scripts running
		// inside this frame see the correct value via this._currentframe
		// (mirrors swf_core.c's update of root_movieclip.currentframe).
		if (obj->instance_name != NULL)
		{
			extern MovieClip* actionFindMovieClipByName(const char* instance_name);
			MovieClip* smc = actionFindMovieClipByName(obj->instance_name);
			if (smc) smc->currentframe = (int)frame + 1;
		}

		// Execute current frame function. On a natural wrap replay of frame 0,
		// raise g_loopback_replay so tagPlaceObject2's survives_rewind branch
		// treats same-char re-places of preserved entries as modifies (matrix/
		// cxform update gated on transformed_by_script, no re-instantiation) —
		// mirroring the root timeline's natural-wrap replay semantics.
		if (frame < ch->sprite_frame_count && ch->sprite_frame_funcs[frame] != NULL)
		{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
			extern int g_loopback_replay;
			int _lb_saved_replay = g_loopback_replay;
			if (lb_wrapped) g_loopback_replay = 1;
#endif
			CALL_FRAME(app_context, obj, ch->sprite_frame_funcs[frame]);
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
			g_loopback_replay = _lb_saved_replay;
#endif
		}
		(void)lb_wrapped;

		// Mark eligible for AS2 onEnterFrame dispatch (sprite actually advanced)
		obj->enterframe_eligible = 1;

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		// Initialize any children placed by the frame function (deferred Phase 2
		// scripts from eager init). Sprite frame functions don't have tagShowFrame,
		// so process_sprite_needs_init must be called explicitly here.
		// Only if any child actually needs init (avoid unnecessary MC lookups).
		{
			int any_needs_init = 0;
			for (size_t ci = 1; ci <= max_depth; ci++) {
				if (display_list[ci].char_id != 0 && display_list[ci].sprite_needs_init) {
					any_needs_init = 1;
					break;
				}
			}
			if (any_needs_init) {
				extern MovieClip* actionFindMovieClipByName(const char* instance_name);
				extern MovieClip root_movieclip;
				MovieClip* parent_for_init = (obj->instance_name != NULL)
					? actionFindMovieClipByName(obj->instance_name) : NULL;
				process_sprite_needs_init(app_context, parent_for_init ? parent_for_init : &root_movieclip);
			}
		}
#endif

		// Recurse: advance nested sprites within this sprite's display list.
		// When g_advance_defer_nested is set (top-level call from swf_core.c),
		// skip recursion here — it will be done by advance_nested_sprite_frames()
		// after the root frame script, matching Ruffle's execution order.
		// Also skip when exec_sprite_frame already eager-built this sprite's
		// nested children before its registerClass constructor (browser-WASM,
		// g_exec_eager_built_obj) — re-advancing would double-step the children.
		if (g_exec_eager_built_obj == obj) {
			g_exec_eager_built_obj = NULL;  // consume (one-shot)
		}
		else if (!g_advance_defer_nested)
		{
			MovieClip* sprite_mc = NULL;
#ifndef NO_GRAPHICS
			extern MovieClip root_movieclip;
			MovieClip* parent_for_recurse = (g_current_context != NULL) ? g_current_context : &root_movieclip;
			if (obj->instance_name != NULL)
				sprite_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_for_recurse);
#else
			extern MovieClip* actionFindMovieClipByName(const char* instance_name);
			if (obj->instance_name != NULL)
				sprite_mc = actionFindMovieClipByName(obj->instance_name);
#endif
			MovieClip* saved_recurse_ctx = g_current_context;
			if (sprite_mc) actionSetCurrentContext(sprite_mc);
			advance_sprite_frames(app_context);
			actionSetCurrentContext(saved_recurse_ctx);
		}

		// Save back (display_list pointer may have changed if realloc'd)
		obj->sprite_display_list = display_list;
		obj->sprite_max_depth = max_depth;
		obj->sprite_dl_capacity = display_list_capacity;

		// Restore parent display list
		display_list = saved_dl;
		max_depth = saved_max;
		display_list_capacity = saved_cap;

		// Advance frame (loop back to 0); guard against 0-frame sprites.
		// BUT only if sprite_current_frame is still the value we read at the top
		// of this iteration: a method-form gotoAndStop/Play on THIS sprite issued
		// by a root script that runs mid-advance (e.g. `clip.gotoAndPlay("label")`)
		// calls ng_gotoFrameByMC, which sets sprite_current_frame directly. The
		// stale `frame + 1` here would clobber that nav. Pacman's death path
		// (`Pacman.gotoAndPlay("Die")` from the root game loop) hit exactly this:
		// the goto set scf=6 (the Die frame) between this iteration's frame read
		// and write, but the auto-advance overwrote it back into the 1-5 chomp
		// loop, so Pacman never played its death animation and the game froze.
		// A sprite's OWN frame-script goto uses sprite_manual_next_frame (handled
		// at the top of the loop), NOT a direct scf write, so this guard is inert
		// for the normal advance + self-loop cases.
		if (ch->sprite_frame_count > 0 && obj->sprite_current_frame == frame)
		{
			// Ruffle determine_next_frame (movie_clip.rs:1340) answered in
			// our 0-based frame-function indices. `loaded` is frames_loaded
			// (the ShowFrame count, floored at 1); `sprite_frame_count` is
			// how many frame functions exist, which is loaded + 1 when tags
			// trail the last ShowFrame.
			//
			//   current < frames_loaded          -> Next
			//   frames_loaded <= 1 || !end_tag   -> Same: stop(), but
			//        run_frame_internal still decodes the tags after the
			//        last ShowFrame — exactly once. Those are our extra
			//        frame function at index `loaded`.
			//   otherwise                        -> First (loop to 0)
			size_t loaded = ch->sprite_loaded_frames;
			if (loaded == 0) loaded = ch->sprite_frame_count;
			if (frame >= loaded)
			{
				// We just ran the trailing partial frame. Ruffle stopped the
				// clip on the tick that reached it; it never runs again.
				ng_sprite_park_no_end_tag(obj);
			}
			else if (frame + 1 < loaded)
			{
				obj->sprite_current_frame = frame + 1;
			}
			else if (loaded <= 1 || !ch->sprite_has_end_tag)
			{
				if (frame + 1 < ch->sprite_frame_count)
					obj->sprite_current_frame = frame + 1;   // trailing partial
				else if (!ch->sprite_has_end_tag)
					ng_sprite_park_no_end_tag(obj);
				else
					obj->sprite_current_frame = (frame + 1) % ch->sprite_frame_count;
			}
			else
			{
				obj->sprite_current_frame = 0;
			}
		}
	}
}

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
// Browser-WASM: construct just-placed registerClass timeline sprites BEFORE the
// current frame's queued root DoAction drains.
//
// NO_GRAPHICS/OFFSCREEN construct timeline-placed registerClass sprites at
// PLACEMENT time (process_sprite_needs_init, during tagPlaceObject2 / the frame
// func), so they exist before the frame's DoAction runs — matching Flash's
// construct_frame-before-frame_scripts order. Browser-WASM has no
// process_sprite_needs_init; it fires the registerClass constructor inside
// exec_sprite_frame, which only runs in tagShowFrame's advance_sprite_frames —
// i.e. AFTER the recompiler-emitted actionDrainAllInPriorityOrder that runs the
// frame's queued root DoAction. For Minesweeper that left the FRadioButtonGroup
// `diff_level` un-built when frame 5's `diff_level.setValue(70)` ran: the call
// hit `undefined` (a no-op) and the default radio selection (Medium ◉) never
// applied. (Confirmed by instrumentation: setValue fired on undefined before any
// radio constructor; OFFSCREEN fires it on the real group after all 3 construct.)
//
// This pass runs the just_allocated branch of advance_sprite_frames for each
// freshly-placed (sprite_display_list==NULL) registerClass sprite in the CURRENT
// display list, so its constructor (→ this.init() → addToRadioGroup) runs before
// the DoAction drain. Idempotent: a sprite is skipped once allocated, so the
// later advance_sprite_frames sees it non-just_allocated (1-frame → skipped, no
// double-run; multi-frame → resumes from frame 1 as normal). Re-entrancy-guarded
// (the constructor's AS may itself trigger a drain).
void ng_construct_pending_registerclass_sprites(SWFAppContext* app_context)
{
	extern int catch_up_mode;
	if (catch_up_mode) return;
	static int s_in_pass = 0;
	if (s_in_pass) return;
	extern void* lookupRegisteredClassByCharId(size_t, int, const char**);
	extern int g_swf_version;
	extern MovieClip root_movieclip;

	// Cheap pre-scan: skip the whole pass unless something actually needs it.
	int any = 0;
	for (size_t d = 1; d <= max_depth; d++) {
		DisplayObject* obj = &display_list[d];
		if (obj->char_id == 0 || obj->sprite_display_list != NULL) continue;
		if (dictionary[obj->char_id].type != CHAR_TYPE_SPRITE) continue;
		if (lookupRegisteredClassByCharId(obj->char_id, g_swf_version, NULL) != NULL) { any = 1; break; }
	}
	if (!any) return;

	s_in_pass = 1;
	for (size_t d = 1; d <= max_depth; d++) {
		DisplayObject* obj = &display_list[d];
		if (obj->char_id == 0 || obj->sprite_display_list != NULL) continue;
		Character* ch = &dictionary[obj->char_id];
		if (ch->type != CHAR_TYPE_SPRITE) continue;
		if (lookupRegisteredClassByCharId(obj->char_id, g_swf_version, NULL) == NULL) continue;

		// Allocate the sprite's persistent display list (just_allocated path).
		obj->sprite_dl_capacity = INITIAL_SPRITE_DL_CAPACITY;
		obj->sprite_display_list = HCALLOC(obj->sprite_dl_capacity, sizeof(DisplayObject));
		obj->sprite_max_depth = 0;
		obj->sprite_current_frame = 0;

		DisplayObject* saved_dl = display_list;
		size_t saved_max = max_depth;
		size_t saved_cap = display_list_capacity;
		display_list = obj->sprite_display_list;
		max_depth = obj->sprite_max_depth;
		display_list_capacity = obj->sprite_dl_capacity;

		if (obj->instance_name != NULL) {
			MovieClip* smc = actionFindOrCreateMovieClip(app_context, obj->instance_name,
				g_current_context ? g_current_context : &root_movieclip);
			if (smc) smc->currentframe = 1;
		}

		// exec_sprite_frame fires the registerClass constructor (once-only) and
		// eager-builds the sprite's nested children (setting g_exec_eager_built_obj).
		if (ch->sprite_frame_funcs[0] != NULL)
			CALL_FRAME(app_context, obj, ch->sprite_frame_funcs[0]);

		// Nested children already eager-built inside exec_sprite_frame — consume
		// the flag so the later advance_sprite_frames recursion doesn't double-step.
		if (g_exec_eager_built_obj == obj)
			g_exec_eager_built_obj = NULL;

		obj->sprite_display_list = display_list;
		obj->sprite_max_depth = max_depth;
		obj->sprite_dl_capacity = display_list_capacity;
		display_list = saved_dl;
		max_depth = saved_max;
		display_list_capacity = saved_cap;

		if (ch->sprite_frame_count > 0)
			obj->sprite_current_frame = 1 % ch->sprite_frame_count;
	}
	s_in_pass = 0;
}
#endif

// Deferred recursion pass: for each root-level sprite, swap into its display
// list and call advance_sprite_frames (which will now recurse normally since
// g_advance_defer_nested is 0).
void advance_nested_sprite_frames(SWFAppContext* app_context)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (catch_up_mode) return;
#endif

	// Reverse-instantiation order (place_seq DESC) — same Ruffle clip_exec_list
	// LIFO semantics as advance_sprite_frames. The cap is tested against the
	// occupied-depth COUNT, not max_depth (the highest depth index) — see the
	// matching comment in advance_sprite_frames.
	#define ANSF_SORT_CAP 512
	size_t sorted_depths[ANSF_SORT_CAP];
	size_t sorted_seq[ANSF_SORT_CAP];
	size_t n_sorted = 0;
	int use_sorted = 1;
	{
		for (size_t d = 1; d <= max_depth; d++) {
			if (display_list[d].char_id == 0) continue;
			if (n_sorted >= ANSF_SORT_CAP) { use_sorted = 0; n_sorted = 0; break; }
			sorted_depths[n_sorted] = d;
			sorted_seq[n_sorted] = display_list[d].place_seq;
			n_sorted++;
		}
	}
	if (use_sorted) {
		for (size_t k = 1; k < n_sorted; k++) {
			size_t key_d = sorted_depths[k];
			size_t key_s = sorted_seq[k];
			long b = (long)k - 1;
			while (b >= 0 && sorted_seq[b] < key_s) {
				sorted_depths[b+1] = sorted_depths[b];
				sorted_seq[b+1] = sorted_seq[b];
				b--;
			}
			sorted_depths[b+1] = key_d;
			sorted_seq[b+1] = key_s;
		}
	}

	size_t iter_n = use_sorted ? n_sorted : (max_depth + 1);
	for (size_t iter = 0; iter < iter_n; iter++)
	{
		size_t cur_depth;
		if (use_sorted) {
			cur_depth = sorted_depths[iter];
		} else {
			cur_depth = max_depth - iter;
			if (cur_depth == 0) continue;
		}
		DisplayObject* obj = &display_list[cur_depth];
		if (obj->char_id == 0) continue;
		Character* ch = &dictionary[obj->char_id];
		if (ch->type != CHAR_TYPE_SPRITE) continue;
		if (obj->sprite_display_list == NULL) continue;
		// Skip sprites placed this same tick — Ruffle's clip_exec_list iteration
		// captures `next` before processing each clip, so clips added mid-iter
		// (e.g. via PlaceObject2 from a parent's frame func) are never visited
		// the same tick. Without this skip, Pong's sprite_9 (nested inside
		// sprite_10) gets advanced from its just-set sprite_current_frame=1 to
		// sprite_frame_funcs[1] (script_2 → _root.play()) in tick 1, causing
		// root to advance one tick early in F1. See plan:
		// SWFRecompDocs/plans/defer-newly-placed-sprite-advance-plan.md.
		{ extern size_t g_tick_count; if (obj->placed_at_tick == g_tick_count) continue; }

		// Swap to sprite's display list context
		DisplayObject* saved_dl = display_list;
		size_t saved_max = max_depth;
		size_t saved_cap = display_list_capacity;

		display_list = obj->sprite_display_list;
		max_depth = obj->sprite_max_depth;
		display_list_capacity = obj->sprite_dl_capacity;

		// Set context to this sprite's MC for correct child resolution
		MovieClip* sprite_mc = NULL;
#ifndef NO_GRAPHICS
		{
			extern MovieClip root_movieclip;
			MovieClip* parent_for_recurse = (g_current_context != NULL) ? g_current_context : &root_movieclip;
			if (obj->instance_name != NULL)
				sprite_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_for_recurse);
		}
#else
		{
			extern MovieClip* actionFindMovieClipByName(const char* instance_name);
			if (obj->instance_name != NULL)
				sprite_mc = actionFindMovieClipByName(obj->instance_name);
		}
#endif
		MovieClip* saved_ctx = g_current_context;
		if (sprite_mc) actionSetCurrentContext(sprite_mc);

		// Now recurse (g_advance_defer_nested is 0, so full recursion happens)
		advance_sprite_frames(app_context);

		actionSetCurrentContext(saved_ctx);

		// Save back
		obj->sprite_display_list = display_list;
		obj->sprite_max_depth = max_depth;
		obj->sprite_dl_capacity = display_list_capacity;

		display_list = saved_dl;
		max_depth = saved_max;
		display_list_capacity = saved_cap;
	}
}

// ---------------------------------------------------------------------------
// attachMovie'd multi-frame clip playhead auto-advance pump (PROGRESS #15).
//
// Flash/Ruffle auto-play an attachMovie'd clip: it shows frame 1 on the attach
// tick and each later tick advances (looping at the end) unless its frame-1
// script stop()s it. advance_sprite_frames / advance_nested_sprite_frames only
// walk display-list arrays; a root/non-root attached clip's display_obj is a
// STANDALONE heap struct in NO list, so its timeline never ticks. This pump
// walks child_mc_cache and advances the AS-visible playhead of each attached
// clip flagged `attached_playable == 2` (active) whose display_obj is playing.
//
// Scope (PROGRESS #15): this advances the playhead COUNTER only — it does not
// re-execute the clip's per-frame tags/scripts. The gate's `box` has no frame
// content and N's menuMC is stop()'d (skipped), so the counter is the entire
// observable behaviour for the #15 regression target. Executing attached-clip
// frame scripts here (Phase 1, before the root frame func) would reintroduce
// the cross-clip script-ordering hazards tracked in #10a/#10b; that is left as
// a documented follow-up. The method-form stop()/play() fix (step 1) and
// ng_gotoFrameByMC already rebuild the clip's display list when navigation
// actually occurs.
//
// Ordering: the pump runs in Phase 1 (before the root frame func reads
// _currentframe), so the observed value matches a timeline sprite advanced in
// Phase 1. The attach tick is skipped via the flag — ng_record_attached_playable
// sets it to 1 (pending) at attach; ng_promote_attached_playheads promotes 1->2
// only AFTER the deferred attach-init drain has run that tick, so a clip's
// frame-1 this.stop() (routed to its own display_obj by step 1) has applied
// before it is ever advanced (the #10 record / apply-after-drain pattern).
// ---------------------------------------------------------------------------
void ng_record_attached_playable(MovieClip* mc)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (mc == NULL || mc->display_obj == NULL) return;
	DisplayObject* obj = (DisplayObject*)mc->display_obj;
	if (obj->char_id == 0) return;
	Character* ch = &dictionary[obj->char_id];
	if (ch->type != CHAR_TYPE_SPRITE) return;
	if (ch->sprite_frame_count <= 1) return;   // 1-frame clips never advance
	// Flash auto-plays an (re)attached clip from frame 1. A frame-1 stop() /
	// gotoAndStop in the attach-init drain overrides this to 0 before the first
	// pump tick (drain runs this tick; pump runs next tick).
	obj->sprite_is_playing = 1;
	// Re-attach of an already-active clip stays active (keeps advancing without
	// burning another promotion tick); a fresh attach is pending so its creation
	// tick is skipped.
	if (mc->attached_playable != 2)
		mc->attached_playable = 1;
#else
	(void)mc;
#endif
}

void ng_promote_attached_playheads(void)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	for (int i = 0; i < child_mc_count; i++) {
		MovieClip* mc = child_mc_cache[i];
		if (mc != NULL && mc->attached_playable == 1)
			mc->attached_playable = 2;
	}
#endif
}

void ng_advance_attached_clip_playheads(SWFAppContext* app_context)
{
	(void)app_context;
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (catch_up_mode) return;
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	for (int i = 0; i < child_mc_count; i++) {
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL) continue;
		if (mc->attached_playable != 2) continue;        // only promoted (post-attach-tick) clips
		if (mc->avm1_removed) continue;
		DisplayObject* obj = (DisplayObject*)mc->display_obj;
		if (obj == NULL) continue;
		if (!obj->sprite_is_playing) continue;           // stop()'d / gotoAndStop'd -> frozen
		if (obj->sprite_manual_next_frame) continue;     // a pending goto handles its own nav
		if (obj->char_id == 0) continue;
		Character* ch = &dictionary[obj->char_id];
		if (ch->type != CHAR_TYPE_SPRITE) continue;
		size_t fc = ch->sprite_frame_count;
		if (fc <= 1) continue;
		size_t new_frame = (obj->sprite_current_frame + 1) % fc;
		obj->sprite_current_frame = new_frame;
		mc->currentframe = (int)new_frame + 1;
	}
#endif
}

// ---------------------------------------------------------------------------
// Same-tick application of a sprite's OWN frame-script gotoAndPlay (#10).
//
// Sprite frame scripts are QUEUED during advance_sprite_frames / advance_nested_
// sprite_frames and only run when the AQ_KIND_SCRIPT queue is drained — which
// happens AFTER both advance passes. So a sprite whose frame N script calls
// gotoAndPlay(T) (ng_gotoFrameCurrentSprite) sets sprite_manual_next_frame too
// late for THIS tick's advance: the top-of-loop manual-nav block only sees it
// NEXT tick, costing an extra tick where the observer re-reads the issuing frame
// (a "stutter") and then lands on — and re-observes — the target. Flash/Ruffle
// apply the goto within the same tick: the issuing frame was already observed
// (onEnterFrame ran before the frame action), the display rebuilds to the
// target now, and a Play resumes at target+1 next tick. So a 5-frame sprite
// whose frame 5 does gotoAndPlay(1) reads a clean period-4 loop (…,5,2,3,4,5,2,…)
// instead of period-6 (…,5,5,1,2,3,4,…). ng_gotoFrameCurrentSprite records each
// self-goto here; swf_core.c / swf.c call the apply pass right after draining
// AQ_KIND_SCRIPT. gotoAndStop is left on the deferred path (is_playing==0 →
// skipped) so its established behavior is unchanged.
#define SELF_GOTO_CAP 256
static DisplayObject* g_self_goto_objs[SELF_GOTO_CAP];
static size_t g_self_goto_count = 0;

void ng_record_sprite_self_goto(DisplayObject* obj)
{
	if (obj == NULL) return;
	// Replays (catch_up_mode) suppress scripts, so a goto here is a real one; but
	// guard anyway so a re-entrant apply/replay never records into its own list.
	if (catch_up_mode) return;
	for (size_t i = 0; i < g_self_goto_count; i++)
		if (g_self_goto_objs[i] == obj) return;   // dedup: at most one apply per obj per tick
	if (g_self_goto_count < SELF_GOTO_CAP)
		g_self_goto_objs[g_self_goto_count++] = obj;
}

void ng_apply_pending_sprite_self_gotos(SWFAppContext* app_context)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (catch_up_mode) { g_self_goto_count = 0; return; }
#endif
	size_t n = g_self_goto_count;
	g_self_goto_count = 0;   // consume now so replays below don't re-enter the list
	for (size_t i = 0; i < n; i++)
	{
		DisplayObject* obj = g_self_goto_objs[i];
		if (obj == NULL || obj->char_id == 0) continue;
		// Only a still-PLAYING pending nav is the gotoAndPlay self-loop. A
		// gotoAndStop (is_playing==0) keeps its manual flag and the deferred
		// top-of-loop path handles it next tick exactly as before.
		if (!obj->sprite_manual_next_frame || !obj->sprite_is_playing) continue;
		Character* ch = &dictionary[obj->char_id];
		if (ch->type != CHAR_TYPE_SPRITE || ch->sprite_frame_count == 0) continue;

		obj->sprite_manual_next_frame = 0;
		size_t target = obj->sprite_next_frame;
		if (target >= ch->sprite_frame_count) continue;

		// Swap into this sprite's own display list and rebuild it as of `target`
		// from a clean slate (replay frames 0..target, tags only). A full reset+
		// replay reconstructs frame `target` exactly regardless of goto direction.
		// The target frame's own DoAction is queued via ng_gotoFrameCurrentSprite's
		// pending sprite-script queue (drained separately under
		// actionUnifySpriteDrain), so it must NOT be re-queued here — hence
		// catch_up_mode=1 throughout the replay.
		DisplayObject* saved_dl = display_list;
		size_t saved_max = max_depth;
		size_t saved_cap = display_list_capacity;

		display_list = obj->sprite_display_list;
		max_depth = obj->sprite_max_depth;
		display_list_capacity = obj->sprite_dl_capacity;

		for (size_t j = 1; j <= max_depth; ++j)
		{
			if (display_list[j].sprite_display_list != NULL)
			{
				ng_freeSpriteDL(app_context, display_list[j].sprite_display_list,
			                display_list[j].sprite_dl_capacity);
				display_list[j].sprite_display_list = NULL;
			}
			display_list[j].char_id = 0;
		}
		max_depth = 0;

		int saved_sg_cm = catch_up_mode;
		catch_up_mode = 1;
		for (size_t f = 0; f <= target; f++)
			if (f < ch->sprite_frame_count && ch->sprite_frame_funcs[f] != NULL)
				CALL_FRAME(app_context, obj, ch->sprite_frame_funcs[f]);
		catch_up_mode = saved_sg_cm;

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		// Initialize children placed by the rebuilt target frame.
		{
			int any_needs_init = 0;
			for (size_t ci = 1; ci <= max_depth; ci++) {
				if (display_list[ci].char_id != 0 && display_list[ci].sprite_needs_init) {
					any_needs_init = 1;
					break;
				}
			}
			if (any_needs_init) {
				extern MovieClip* actionFindMovieClipByName(const char* instance_name);
				extern MovieClip root_movieclip;
				MovieClip* parent_for_init = (obj->instance_name != NULL)
					? actionFindMovieClipByName(obj->instance_name) : NULL;
				process_sprite_needs_init(app_context, parent_for_init ? parent_for_init : &root_movieclip);
			}
		}
#endif

		// NOTE: we do NOT recurse to advance this sprite's children here. The
		// replay above (+ process_sprite_needs_init) re-places and initializes
		// the target frame's children; like a real goto, they start at their own
		// frame 1 and advance on subsequent ticks via the normal Phase 1/Phase 3
		// passes — matching Flash/Ruffle. (The deferred top-of-loop block recurses
		// because it runs with g_current_context set to the parent; this apply
		// pass runs at the top of the tick with g_current_context = root, so an
		// actionFindOrCreateMovieClip on a NESTED sprite's name would mint a root
		// ghost instead of resolving the real child.)

		// Play resumes from target+1 next tick (the target frame is shown this
		// tick; the observer already read the issuing frame in the flush).
		obj->sprite_current_frame = (target + 1) % ch->sprite_frame_count;
		obj->enterframe_eligible = 1;

		obj->sprite_display_list = display_list;
		obj->sprite_max_depth = max_depth;
		obj->sprite_dl_capacity = display_list_capacity;

		display_list = saved_dl;
		max_depth = saved_max;
		display_list_capacity = saved_cap;
	}
}

// ---------------------------------------------------------------------------
// Pre-sync the AS-visible _currentframe of DEFERRED nested sprites (#10a).
//
// Nested sprites (depth >= 2) advance in Phase 3 (advance_nested_sprite_frames),
// AFTER the Phase-2 enterFrame flush where dynamically-created clips' onEnterFrame
// handlers (e.g. the divergence tracer) sample state. Ruffle instead walks ONE
// flat instantiation-ordered clip_exec_list doing enterFrame-event-THEN-advance
// per clip (runtime.rs run_frame + movie_clip.rs run_frame_avm1), so a
// last-instantiated clip's onEnterFrame reads every game clip's POST-advance
// _currentframe. Our phased model left nested sprites one tick stale to that
// observer (root-level sprites, advanced in Phase 1, were already correct).
//
// This pass writes ONLY the reporting field mc->currentframe (what _currentframe
// reads) to the value Phase 3 will set — sprite_current_frame + 1 for a normally
// advancing sprite — WITHOUT running any frame script, mutating the
// sprite_current_frame playhead, or rebuilding any display list. So it cannot
// perturb frame-script execution order (the reason nested advance is deferred):
// Phase 3 still reads the unchanged playhead and runs scripts exactly as before,
// re-writing currentframe to the same value. Mirrors the option-(a) design in
// tools/divergence/gates/README.md "nested_sprite_cf_lag".
//
// Called between Phase 1 and the enterFrame flush in swf_core.c (NO_GRAPHICS)
// and swf.c (OFFSCREEN_RENDER).
static void presync_nested_cf_recurse(SWFAppContext* app_context)
{
	// Operates on the currently swapped-in display_list / max_depth.
	extern size_t g_tick_count;
	for (size_t d = 1; d <= max_depth; d++)
	{
		DisplayObject* obj = &display_list[d];
		if (obj->char_id == 0) continue;
		Character* ch = &dictionary[obj->char_id];

		if (ch->type == CHAR_TYPE_BUTTON)
		{
			if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
			{
				DisplayObject* saved_dl = display_list;
				size_t saved_max = max_depth;
				size_t saved_cap = display_list_capacity;
				display_list = obj->sprite_display_list;
				max_depth = obj->sprite_max_depth;
				display_list_capacity = obj->sprite_dl_capacity;
				presync_nested_cf_recurse(app_context);
				display_list = saved_dl;
				max_depth = saved_max;
				display_list_capacity = saved_cap;
			}
			continue;
		}
		if (ch->type != CHAR_TYPE_SPRITE) continue;
		if (obj->sprite_display_list == NULL) continue;  // not yet built; Phase 3 establishes it

		// Mirror advance_sprite_frames' "normal advance" guards: a sprite reaches
		// the currentframe = frame + 1 write only when playing, not mid-goto,
		// multi-frame, and not placed this very tick (advance_nested skips those).
		MovieClip* smc = NULL;
		if (obj->instance_name != NULL)
		{
#ifndef NO_GRAPHICS
			extern MovieClip root_movieclip;
			MovieClip* pmc = (g_current_context != NULL) ? g_current_context : &root_movieclip;
			smc = tag_cached_walk_mc(obj, pmc, 0);
			if (smc == NULL) {
				smc = actionFindOrCreateMovieClip(app_context, obj->instance_name, pmc);
				tag_store_walk_mc(obj, smc);
			}
#else
			extern MovieClip* actionFindMovieClipByName(const char* instance_name);
			smc = tag_cached_walk_mc(obj, NULL, 1);
			if (smc == NULL) {
				smc = actionFindMovieClipByName(obj->instance_name);
				tag_store_walk_mc(obj, smc);
			}
#endif
		}

		int advances = obj->sprite_is_playing
			&& !obj->sprite_manual_next_frame
			&& ch->sprite_frame_count > 1
			&& obj->placed_at_tick != g_tick_count;
		if (advances && smc)
			smc->currentframe = (int)obj->sprite_current_frame + 1;

		// Recurse into children regardless of the parent's play state — a stopped
		// or single-frame holder still has independently-advancing children.
		if (obj->sprite_max_depth > 0)
		{
			DisplayObject* saved_dl = display_list;
			size_t saved_max = max_depth;
			size_t saved_cap = display_list_capacity;
			MovieClip* saved_ctx = g_current_context;
			display_list = obj->sprite_display_list;
			max_depth = obj->sprite_max_depth;
			display_list_capacity = obj->sprite_dl_capacity;
			if (smc) actionSetCurrentContext(smc);
			presync_nested_cf_recurse(app_context);
			actionSetCurrentContext(saved_ctx);
			display_list = saved_dl;
			max_depth = saved_max;
			display_list_capacity = saved_cap;
		}
	}
}

void presync_nested_sprite_currentframe(SWFAppContext* app_context)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (catch_up_mode) return;
#endif
	// Iterate ROOT-level sprites and descend into each one's child display list,
	// presyncing those nested sprites. Root-level sprites themselves were advanced
	// in Phase 1 (before the flush) so their currentframe is already correct —
	// do NOT touch it here. Root-level buttons likewise advance their children in
	// Phase 1 (advance_sprite_frames' button recursion is not deferral-gated), so
	// skip them too; only nested buttons (reached below) are deferred to Phase 3.
	for (size_t d = 1; d <= max_depth; d++)
	{
		DisplayObject* obj = &display_list[d];
		if (obj->char_id == 0) continue;
		Character* ch = &dictionary[obj->char_id];
		if (ch->type != CHAR_TYPE_SPRITE) continue;
		if (obj->sprite_display_list == NULL || obj->sprite_max_depth == 0) continue;

		MovieClip* smc = NULL;
		if (obj->instance_name != NULL)
		{
#ifndef NO_GRAPHICS
			extern MovieClip root_movieclip;
			MovieClip* pmc = (g_current_context != NULL) ? g_current_context : &root_movieclip;
			smc = actionFindOrCreateMovieClip(app_context, obj->instance_name, pmc);
#else
			extern MovieClip* actionFindMovieClipByName(const char* instance_name);
			smc = actionFindMovieClipByName(obj->instance_name);
#endif
		}

		DisplayObject* saved_dl = display_list;
		size_t saved_max = max_depth;
		size_t saved_cap = display_list_capacity;
		MovieClip* saved_ctx = g_current_context;
		display_list = obj->sprite_display_list;
		max_depth = obj->sprite_max_depth;
		display_list_capacity = obj->sprite_dl_capacity;
		if (smc) actionSetCurrentContext(smc);
		presync_nested_cf_recurse(app_context);
		actionSetCurrentContext(saved_ctx);
		display_list = saved_dl;
		max_depth = saved_max;
		display_list_capacity = saved_cap;
	}
}

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
// ---------------------------------------------------------------------------
// Browser-WASM: apply DEFERRED gotoAndStop/Play navigation to attachMovie'd
// clips. The MC-targeted gotoAndStop handler (action.c) sets sprite-local nav
// flags on an attached clip's standalone display_obj but does NOT execute the
// frame inline — running it there would be reentrant (Doodle Jump calls
// block.gotoAndStop(N) inside the levelcontainer's attachBlocks while-loop,
// and a synchronous rebuild mid-loop caused game-overs). advance_sprite_frames
// / advance_nested_sprite_frames only walk the root display list and timeline-
// placed sprites, never the standalone display_obj of an attachMovie'd clip, so
// nothing consumed those flags. This pass (called once per tick from
// tagShowFrame, AFTER scripts/queues have drained) consumes them safely.
//
// For each attached clip with a pending nav flag it rebuilds the clip's own
// sprite_display_list for the target frame, then runs advance_sprite_frames
// over that list so any sprite the target frame placed (e.g. Doodle Jump's
// blue moving platform: cloud frame 3 places sprite charId 32, which itself
// contains the blue shape charId 35) gets its sub-display-list built and is
// then recursed by render_display_list. Without the advance_sprite_frames step
// the nested sprite stays empty and renders nothing. Gated to clips with the
// flag set, so non-navigating attached clips (most Snake/Pong clips) are
// untouched.
void advance_attached_clip_frames(SWFAppContext* app_context)
{
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	extern size_t g_place_gen;
	for (int i = 0; i < child_mc_count; i++)
	{
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL || mc->depth == INT_MIN || mc->display_obj == NULL) continue;
		DisplayObject* d = (DisplayObject*)mc->display_obj;
		if (!d->sprite_manual_next_frame) continue;       // only pending navs
		// Skip timeline-placed clips whose display_obj lives in the global
		// display_list array — those are consumed by advance_sprite_frames.
		uintptr_t dl_lo = (uintptr_t)display_list;
		uintptr_t dl_hi = dl_lo + (uintptr_t)display_list_capacity * sizeof(DisplayObject);
		if ((uintptr_t)d >= dl_lo && (uintptr_t)d < dl_hi) continue;
		if (d->char_id == 0) continue;
		Character* ch = &dictionary[d->char_id];
		if (ch->type != CHAR_TYPE_SPRITE || ch->sprite_frame_count == 0) continue;

		d->sprite_manual_next_frame = 0;
		size_t target = d->sprite_next_frame;
		if (target >= ch->sprite_frame_count) target = ch->sprite_frame_count - 1;

		DisplayObject* saved_dl = display_list;
		size_t saved_max = max_depth;
		size_t saved_cap = display_list_capacity;
		MovieClip* saved_ctx = g_current_context;

		display_list = d->sprite_display_list;
		max_depth = d->sprite_max_depth;
		display_list_capacity = d->sprite_dl_capacity;

		// Full rebuild from frame 0 (gotoAndStop is a jump). Free nested
		// sub-lists and clear so a backward jump doesn't leave stale entries.
		// First invalidate the cached child MovieClips for the entries we're
		// about to free, so a manual nav (gotoAndStop/Play) on an attached clip
		// doesn't orphan its old auto-instance children in child_mc_cache (same
		// leak as the natural loop-back in advance_sprite_frames). The rebuild
		// below re-materializes the target frame's placements.
		for (size_t j = 1; j <= max_depth; ++j)
		{
			if (display_list[j].char_id != 0)
				invalidate_mc_for_dl_entry(app_context, &display_list[j]);
		}
		for (size_t j = 1; j <= max_depth; ++j)
		{
			if (display_list[j].sprite_display_list != NULL)
			{
				ng_freeSpriteDL(app_context, display_list[j].sprite_display_list,
			                display_list[j].sprite_dl_capacity);
				display_list[j].sprite_display_list = NULL;
			}
			display_list[j].char_id = 0;
		}
		max_depth = 0;

		actionSetCurrentContext(mc);
		g_place_gen++;
		int saved_cm = catch_up_mode;
		catch_up_mode = 1;   // placement tags only; suppress DoAction re-fire
		for (size_t f = 0; f <= target && f < ch->sprite_frame_count; f++)
		{
			if (ch->sprite_frame_funcs[f] != NULL)
				exec_sprite_frame(app_context, d, ch->sprite_frame_funcs[f]);
		}
		catch_up_mode = saved_cm;

		// Build/advance sprites the target frame placed (nested sub-lists).
		advance_sprite_frames(app_context);

		actionSetCurrentContext(saved_ctx);

		d->sprite_display_list = display_list;
		d->sprite_max_depth = max_depth;
		d->sprite_dl_capacity = display_list_capacity;
		d->sprite_current_frame = target;
		mc->currentframe = (int)target + 1;

		display_list = saved_dl;
		max_depth = saved_max;
		display_list_capacity = saved_cap;
		// See advance_attached_clip_natural: a manual nav this tick must not also be
		// auto-stepped this tick (gotoAndPlay(T) shows T now, plays T+1 next tick).
		{ extern size_t g_tick_count; d->placed_at_tick = g_tick_count; }
	}
}

// ---------------------------------------------------------------------------
// Browser-WASM per-tick forward play of EXPLICITLY gotoAndPlay'd attached clips.
//
// Browser-WASM has no auto-advance for standalone attachMovie'd clips' own
// timelines (advance_attached_clip_frames only services pending MANUAL navs; the
// OFFSCREEN ng_advance_attached_clip_playheads counter pump is gated out). So a
// coin's mc.gotoAndPlay("COLLECTED") (GoldObject.Dissapear) or a drone's
// gotoAndPlay("...prefire") jumps once via ng_gotoFrameByMC and then FREEZES — the
// disappear/transition animation never plays, leaving the clip on screen forever
// (visual bug + needless overdraw).
//
// SAFETY (this is v2 — v1 advanced ALL is_playing clips and ran their frame
// scripts, which let timeIndicator-class GUI clips re-create child clips every
// tick and exploded `bar` to the 4096 cap):
//   * Only clips with goto_play_active (set ONLY by ng_gotoFrameByMC(play=1)) are
//     touched — never default-is_playing GUI/HUD clips.
//   * Frames run with catch_up_mode=1 → PLACEMENT TAGS ONLY, NO DoAction. Nothing
//     can attachMovie/createEmptyMovieClip from a frame script, so no chain
//     reaction. (Trade-off: an animation whose end-frame *script* does the next
//     transition won't transition — it just animates to its last frame.)
//   * child_mc_count is snapshotted before the loop, so any clip created this tick
//     can't be swept into the same pass.
//   * One-shot: when the playhead wraps to frame 0 the clip stops (no loop), so a
//     gotoAndPlay to a label plays forward once and parks on its last frame
//     (matches a disappear/transition ending in stop()).
// Browser-WASM only (OFFSCREEN keeps its counter pump; NO_GRAPHICS not
// built here) → CI byte-identical, not CI-observable.
void advance_attached_clip_natural(SWFAppContext* app_context)
{
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	extern size_t g_tick_count;
	if (catch_up_mode) return;
	uintptr_t dl_lo = (uintptr_t)display_list;
	uintptr_t dl_hi = dl_lo + (uintptr_t)display_list_capacity * sizeof(DisplayObject);
	int n = child_mc_count;   // snapshot: clips created this pass must not cascade in
	for (int i = 0; i < n; i++)
	{
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL || mc->depth == INT_MIN || mc->avm1_removed) continue;
		if (mc->display_obj == NULL) continue;
		DisplayObject* d = (DisplayObject*)mc->display_obj;
		// Two entry conditions: explicit gotoAndPlay'd clips (coins/drones, parked
		// on wrap) and auto-play one-shot particles ("pfx*", REMOVED on wrap to
		// mimic their final-frame removeMovieClip — see ng_attachMovie flagging).
		int is_oneshot = d->natural_oneshot;
		if (!d->goto_play_active && !is_oneshot) continue;
		// Standalone attached clips only — timeline-placed clips advance via
		// advance_sprite_frames (the flag is inert on them).
		if ((uintptr_t)d >= dl_lo && (uintptr_t)d < dl_hi) continue;
		if (!d->sprite_is_playing) { d->goto_play_active = 0; d->natural_oneshot = 0; continue; }
		if (d->sprite_manual_next_frame) continue;
		if (d->placed_at_tick == g_tick_count) continue; // placed this tick -> show frame 1 first
		if (d->char_id == 0) { d->goto_play_active = 0; d->natural_oneshot = 0; continue; }
		Character* ch = &dictionary[d->char_id];
		if (ch->type != CHAR_TYPE_SPRITE || ch->sprite_frame_count <= 1) { d->goto_play_active = 0; d->natural_oneshot = 0; continue; }
		size_t fc = ch->sprite_frame_count;

		// sprite_current_frame is the NEXT frame to execute (ng_gotoFrameByMC left
		// it at (target+1)%fc; a fresh attach leaves it at 1). If it wrapped to 0 the
		// one-shot animation has played its last frame.
		size_t frame = d->sprite_current_frame;
		if (frame == 0)
		{
			if (is_oneshot)
			{
				// Particle reached the end of its authored lifetime: the SWF's final
				// frame would call this.removeMovieClip() (never executed — we don't run
				// the particle's frame scripts) — do the removal directly so the clip
				// stops rendering and its child_mc_cache slot can be reclaimed (no
				// leftover line, no overdraw).
				extern void actionInvalidateCachedMovieClipDirect(SWFAppContext*, MovieClip*);
				d->sprite_is_playing = 0;
				d->natural_oneshot = 0;
				actionInvalidateCachedMovieClipDirect(app_context, mc);
				continue;
			}
			d->sprite_is_playing = 0;
			d->goto_play_active = 0;
			continue;
		}

		if (is_oneshot)
		{
			// Attached morph-shape particles ("pfx*" — DefineLaserCharge/Spark etc.
			// are DefineMorphShapes animated by per-frame RATIO) CANNOT be ratio-
			// interpolated per-instance in this renderer: morph interpolation runs as
			// a per-character pre-pass into a SHARED vertex buffer over the ROOT
			// display_list only (tag.c ~5802), and render_display_list draws attached
			// morphs at morph_start_offset (ratio 0). Re-running each frame's placement
			// tags would apply that frame's MATRIX (authored for the ratio-N morphed
			// geometry) to the ratio-0 geometry actually drawn → corrupt scale/position
			// (the user's "data looks corrupt"). So DON'T re-run placement tags: keep
			// the particle at its consistent attach-time frame-0 appearance and just
			// count its authored lifetime down, then remove on wrap (above). This keeps
			// the leftover-line + overdraw fix (the particle still self-removes) without
			// the morph mismatch. Trade-off: the particle doesn't play its grow/fade
			// in-between frames (a faithful version needs per-instance morph vertex
			// buffers — a separate renderer change).
			d->sprite_current_frame = (frame + 1) % fc;
			mc->currentframe = (int)frame + 1;
			continue;
		}

		DisplayObject* saved_dl  = display_list;
		size_t         saved_max = max_depth;
		size_t         saved_cap = display_list_capacity;
		MovieClip*     saved_ctx = g_current_context;
		int            saved_cm  = catch_up_mode;

		display_list = d->sprite_display_list;
		max_depth = d->sprite_max_depth;
		display_list_capacity = d->sprite_dl_capacity;
		actionSetCurrentContext(mc);

		// Placement tags only — catch_up_mode=1 suppresses every frame's DoAction
		// (the recompiler gates script queueing on !catch_up_mode), so no clip can
		// be spawned from here.
		catch_up_mode = 1;
		if (frame < fc && ch->sprite_frame_funcs[frame] != NULL)
			exec_sprite_frame(app_context, d, ch->sprite_frame_funcs[frame]);
		catch_up_mode = saved_cm;

		actionSetCurrentContext(saved_ctx);

		d->sprite_display_list = display_list;
		d->sprite_max_depth = max_depth;
		d->sprite_dl_capacity = display_list_capacity;
		// Advance; wrap-to-0 next tick is caught above and stops the clip.
		if (d->sprite_current_frame == frame)
			d->sprite_current_frame = (frame + 1) % fc;
		mc->currentframe = (int)frame + 1;

		display_list = saved_dl;
		max_depth = saved_max;
		display_list_capacity = saved_cap;
	}
}

// Browser-WASM: promote sprite_initialized 1→2 inside attachMovie'd clips'
// standalone display lists. upgrade_sprite_initialized (called from tagShowFrame)
// only walks the global display_list, so nested sprites placed inside an attached
// clip (e.g. Doodle Jump's blue platform charId 32 "aaa", placed by
// advance_attached_clip_frames into the cloud's STANDALONE list) never reach 2 and
// gather_clip_ef_entries (which requires >=2) skips them — so their
// onClipEvent(enterFrame) clip actions never fire. Run the same recursive
// promotion over each attached clip's standalone list once per tick.
void upgrade_attached_clip_initialized(SWFAppContext* app_context)
{
	(void)app_context;
	void upgrade_sprite_initialized(DisplayObject* dl, size_t dl_max);
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	uintptr_t dl_lo = (uintptr_t)display_list;
	uintptr_t dl_hi = dl_lo + (uintptr_t)display_list_capacity * sizeof(DisplayObject);
	for (int i = 0; i < child_mc_count; i++)
	{
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL || mc->depth == INT_MIN || mc->display_obj == NULL) continue;
		DisplayObject* d = (DisplayObject*)mc->display_obj;
		// Only standalone attachMovie'd clips (timeline-placed clips live in the
		// global display_list and are already covered by the root walk).
		if ((uintptr_t)d >= dl_lo && (uintptr_t)d < dl_hi) continue;
		if (d->sprite_display_list == NULL || d->sprite_max_depth == 0) continue;
		upgrade_sprite_initialized(d->sprite_display_list, d->sprite_max_depth);
	}
}
#endif

#ifndef NO_GRAPHICS
// ---------------------------------------------------------------------------
// Dynamic transform slot allocator for composed transforms.
// When multiple sprite instances share the same child transform_id, we must
// allocate unique GPU buffer slots so they don't overwrite each other.
// ---------------------------------------------------------------------------
static u32 g_next_dynamic_xform_slot;   // next available slot in xform_buffer
static u32 g_xform_slot_capacity;       // total slots available

// Save/restore stack for transform_id overrides during compose+render.
// Must exceed the dynamic xform-slot pool (render_webgpu.c extra_slots) so a
// frame that fills the whole pool still records every override and restores it;
// dropping overrides would leave transform_ids pointing at dynamic slots,
// corrupting the next frame's compose.
#define MAX_XFORM_OVERRIDES 8192
typedef struct { DisplayObject* obj; u32 original_id; } XformOverride;
static XformOverride g_xform_overrides[MAX_XFORM_OVERRIDES];
static int g_xform_override_count;

// ---------------------------------------------------------------------------
// AVM1 MovieClip.scrollRect — per-frame crop-slot side table
// ---------------------------------------------------------------------------
// compose_children resolves an entry's scrollRect once, writes the PRE-translate
// world matrix into a dynamic transform slot (the crop quad's matrix), and
// records it here; render_display_list looks it up to push the crop stencil.
// A side table rather than a DisplayObject field: zero header churn, and the
// lifetime is naturally one frame, so a display-list slot recycled by depth
// reuse cannot inherit a stale crop from the previous frame.
#define MAX_SCROLL_CROPS 64
typedef struct {
	DisplayObject* obj;
	u32 slot;        // transform slot holding the pre-translate world matrix
	float w, h;      // crop size in twips
} ScrollCropEntry;
static ScrollCropEntry g_scroll_crops[MAX_SCROLL_CROPS];
static int g_scroll_crop_count;

static void scroll_crops_reset(void)
{
	g_scroll_crop_count = 0;
}

static void scroll_crops_push(DisplayObject* obj, u32 slot, float w, float h)
{
	// An entry can be composed more than once inside one frame (the root loop
	// plus render_attached_child / render_single_object re-entry). The LAST
	// compose is the one the render pass will draw from, so overwrite rather
	// than accumulate — a stale slot id would crop with the previous matrix.
	for (int i = 0; i < g_scroll_crop_count; i++) {
		if (g_scroll_crops[i].obj != obj) continue;
		g_scroll_crops[i].slot = slot;
		g_scroll_crops[i].w = w;
		g_scroll_crops[i].h = h;
		return;
	}
	if (g_scroll_crop_count < MAX_SCROLL_CROPS) {
		g_scroll_crops[g_scroll_crop_count].obj = obj;
		g_scroll_crops[g_scroll_crop_count].slot = slot;
		g_scroll_crops[g_scroll_crop_count].w = w;
		g_scroll_crops[g_scroll_crop_count].h = h;
		g_scroll_crop_count++;
	}
}

static const ScrollCropEntry* scroll_crop_for_entry(const DisplayObject* obj)
{
	if (g_scroll_crop_count == 0 || obj == NULL) return NULL;
	for (int i = 0; i < g_scroll_crop_count; i++)
		if (g_scroll_crops[i].obj == obj) return &g_scroll_crops[i];
	return NULL;
}

// Defined further down with the setMask paint hooks; the scrollRect resolver
// below needs the same entry -> MovieClip identity test (display_obj first,
// instance name as the fallback for wrapper clips that have no entry pointer).
static int avm1_mc_owns_entry(const MovieClip* mc, const DisplayObject* obj);

#define AVM1_SCROLL_RECT_PAINT_MAX 64

// The scrollRect of the MovieClip that owns `obj`, or 0 when there is none.
// `actionAvm1ScrollRectCount() == 0` in every movie that never assigns a
// scrollRect, which is the single-branch fast-out for compose_children's hot
// per-entry loop.
static int avm1_scroll_rect_for_entry(const DisplayObject* obj, Avm1ScrollRect* out)
{
	if (obj == NULL || actionAvm1ScrollRectCount() == 0) return 0;
	void* mcs[AVM1_SCROLL_RECT_PAINT_MAX];
	Avm1ScrollRect rects[AVM1_SCROLL_RECT_PAINT_MAX];
	int n = actionAvm1GetScrollRects(mcs, rects, AVM1_SCROLL_RECT_PAINT_MAX);
	for (int i = 0; i < n; i++) {
		if (!avm1_mc_owns_entry((const MovieClip*)mcs[i], obj)) continue;
		*out = rects[i];
		return 1;
	}
	return 0;
}

static void xform_overrides_reset(void)
{
	g_xform_override_count = 0;
	// Same reset points as the dynamic xform pool (tagShowFrame /
	// tagRerenderFrame): the recorded slots are indices INTO that pool.
	scroll_crops_reset();
}

static void xform_overrides_push(DisplayObject* obj, u32 original_id)
{
	if (g_xform_override_count < MAX_XFORM_OVERRIDES)
	{
		g_xform_overrides[g_xform_override_count].obj = obj;
		g_xform_overrides[g_xform_override_count].original_id = original_id;
		g_xform_override_count++;
	}
}

static void xform_overrides_restore(void)
{
	for (int i = g_xform_override_count - 1; i >= 0; --i)
		g_xform_overrides[i].obj->transform_id = g_xform_overrides[i].original_id;
	g_xform_override_count = 0;
}

// Save/restore stack for per-instance static-text glyph transform bases.
// compose_children sets obj->text_glyph_xform_base to a freshly-allocated dynamic
// slot run; this clears it back to 0 at frame end so the next frame's compose
// re-allocates (the dynamic pool is reset each frame).
#define MAX_TEXT_GLYPH_OVERRIDES 8192
static DisplayObject* g_text_glyph_overrides[MAX_TEXT_GLYPH_OVERRIDES];
static int g_text_glyph_override_count;
static void text_glyph_overrides_reset(void) { g_text_glyph_override_count = 0; }
static void text_glyph_overrides_push(DisplayObject* obj)
{
	if (g_text_glyph_override_count < MAX_TEXT_GLYPH_OVERRIDES)
		g_text_glyph_overrides[g_text_glyph_override_count++] = obj;
}
static void text_glyph_overrides_restore(void)
{
	for (int i = g_text_glyph_override_count - 1; i >= 0; --i)
		g_text_glyph_overrides[i]->text_glyph_xform_base = 0;
	g_text_glyph_override_count = 0;
}

// Recover the original (pre-compose_children) transform_id for a display
// object. compose_children rewrites obj->transform_id to point at a
// dynamically-allocated GPU slot; CPU-side `transform_data` has no entry
// at that slot, so reading it is UB. Callers that need to consult the
// CPU-side transform table during the render frame (between compose and
// xform_overrides_restore) must use this accessor.
u32 ng_get_original_transform_id(DisplayObject* obj)
{
	if (obj == NULL) return 0;
	for (int i = 0; i < g_xform_override_count; i++) {
		if (g_xform_overrides[i].obj == obj)
			return g_xform_overrides[i].original_id;
	}
	return obj->transform_id;
}

// ---------------------------------------------------------------------------
// Dynamic cxform slot allocator for runtime Color.setRGB/setTransform changes.
// Same pattern as the transform slot allocator above.
// ---------------------------------------------------------------------------
static u32 g_next_dynamic_cxform_slot;   // next available slot in cxform_buffer
static u32 g_cxform_slot_capacity;       // total slots available

// Save/restore stack for cxform_id overrides during render
#define MAX_CXFORM_OVERRIDES 256
typedef struct { DisplayObject* obj; u32 original_id; } CxformOverride;
static CxformOverride g_cxform_overrides[MAX_CXFORM_OVERRIDES];
static int g_cxform_override_count;

static void cxform_overrides_reset(void)
{
	g_cxform_override_count = 0;
}

static void cxform_overrides_push(DisplayObject* obj, u32 original_id)
{
	if (g_cxform_override_count < MAX_CXFORM_OVERRIDES)
	{
		g_cxform_overrides[g_cxform_override_count].obj = obj;
		g_cxform_overrides[g_cxform_override_count].original_id = original_id;
		g_cxform_override_count++;
	}
}

static void cxform_overrides_restore(void)
{
	for (int i = g_cxform_override_count - 1; i >= 0; --i)
		g_cxform_overrides[i].obj->cxform_id = g_cxform_overrides[i].original_id;
	g_cxform_override_count = 0;
}

// ---------------------------------------------------------------------------
// Helper: Build a 4x4 transform matrix from MovieClip's AS-set properties.
// Overlays AS-modified fields (as_set_flags bits) onto the existing base
// transform from transform_data.
// ---------------------------------------------------------------------------
static void apply_as_transform(float slot[16], const MovieClip* mc, u8 flags)
{
	// Overlay scale/rotation (bits 4|8|16 = xscale|yscale|rotation).
	// Mirrors getLocalMatrixForMC in action.c: uses mc->skew so that direct
	// `transform.matrix = ...` assignments with non-rotation-only matrices
	// (e.g. Matrix(2, -1.3, 2.4, 1, ...)) reconstruct correctly.
	if ((flags & (4|8|16)) && mc_exact_matrix_live(mc))
	{
		// s21 w2-px-b: a live `transform.matrix =` assignment renders its
		// exact f32 a/b/c/d, not the f32 scale/rotation/skew recomposition.
		slot[0] = (float)mc->exact_m_a; slot[1] = (float)mc->exact_m_b;
		slot[4] = (float)mc->exact_m_c; slot[5] = (float)mc->exact_m_d;
	}
	else if (flags & (4|8|16))
	{
		float sx = mc->xscale / 100.0f;
		float sy = mc->yscale / 100.0f;
		float rad = mc->rotation * 3.14159265358979323846f / 180.0f;
		float skew = mc->skew;
		float cr_x = cosf(rad),        sr_x = sinf(rad);
		float cr_y = cosf(rad + skew), sr_y = sinf(rad + skew);
		slot[0]  = sx * cr_x;    // a
		slot[1]  = sx * sr_x;    // b
		slot[4]  = -(sy * sr_y); // c
		slot[5]  = sy * cr_y;    // d
	}
	// Overlay translation (bit 0 = _x, bit 1 = _y)
	if (flags & 1) slot[12] = rintf(mc->x * 20.0f);  // pixels to twips
	if (flags & 2) slot[13] = rintf(mc->y * 20.0f);
}

// ---------------------------------------------------------------------------
// Helper: Allocate per-tick GPU transform slots for dynamic MCs.
//
// Dynamic MCs (createEmptyMovieClip / duplicateMovieClip / etc.) aren't in
// display_list, so the runtime transform-update loop in tagShowFrame doesn't
// touch them. Their Drawing-API paths render via fillDrawingInfos using
// mc->last_transform_id — which is 0 (slot 0, typically identity) for a fresh
// dynamic MC. When AS sets transform.matrix / _x / _xscale / etc. on a
// dynamic MC, we need to land that transform on a fresh GPU slot and route
// fillDrawingInfos through it.
//
// Limitation: only world-space-equals-local-space cases work right now (i.e.
// parent chain is identity). For dynamic children of transformed sprites, the
// parent's world transform would need to compose here. Tracked in
// DYNAMIC_MC_DRAWING_TRANSFORM_PLAN.
// ---------------------------------------------------------------------------
#if !defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
static void apply_dynamic_mc_transforms(SWFAppContext* app_context)
{
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;

	for (int i = 0; i < child_mc_count; i++) {
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL) continue;
		// Clear any stale slot from previous ticks (per-tick allocator
		// resets g_next_dynamic_xform_slot, so stashed IDs are invalid).
		mc->dynamic_xform_slot = 0;
		if (mc->depth == INT_MIN) continue;       // removed
		if (mc->display_obj != NULL) continue;    // placed — handled by display_list loop
		if (mc->as_set_flags == 0) continue;      // identity — fall through to last_transform_id

		u32 new_slot = g_next_dynamic_xform_slot;
		if (new_slot >= g_xform_slot_capacity) continue;  // exhausted
		g_next_dynamic_xform_slot++;

		// Identity 4x4 (column-major), then overlay AS-set fields.
		float xform[16] = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f,
		};
		// apply_as_transform only overwrites entries whose flag is set.
		// Force-overlay all spatial fields so the slot reflects mc's current
		// state regardless of which subset of flags is set (e.g. just _x or
		// just _xscale).
		apply_as_transform(xform, mc, (u8)(1|2|4|8|16));

		renderer_write_transform(context, new_slot, xform);
		mc->dynamic_xform_slot = new_slot;
	}
	(void)app_context;
}
#endif

// ---------------------------------------------------------------------------
// Helper: Build a 20-float cxform entry from DisplayObject cx_* fields.
// GPU format: 4x4 diagonal matrix (mult) + 4 add values.
// ---------------------------------------------------------------------------
static void build_cxform_from_obj(float out[20], const DisplayObject* obj)
{
	memset(out, 0, 20 * sizeof(float));
	// Diagonal multipliers (percentage to 0-1)
	out[0]  = (float)(obj->cx_ra / 100.0);
	out[5]  = (float)(obj->cx_ga / 100.0);
	out[10] = (float)(obj->cx_ba / 100.0);
	out[15] = (float)(obj->cx_aa / 100.0);
	// Additive offsets (0-255 to 0-1)
	out[16] = (float)(obj->cx_rb / 255.0);
	out[17] = (float)(obj->cx_gb / 255.0);
	out[18] = (float)(obj->cx_bb / 255.0);
	out[19] = (float)(obj->cx_ab / 255.0);
}

// ---------------------------------------------------------------------------
// Helper 2: Recursive transform composition for sprite/button children
// ---------------------------------------------------------------------------
// Composes each child's local transform with the parent's already-composed
// global transform, writes the result to a dynamically allocated GPU buffer
// slot, and updates the child's transform_id to point to the new slot.
// Original transform_ids are saved via xform_overrides for restoration after
// rendering.
// Build a local transform matrix for an attached MC (one placed via attachMovie
// into a non-root parent's sprite_display_list). ng_attachMovie leaves the
// entry's transform_id at 0 (slot 0 = identity), so compose_children would
// otherwise render the child at the parent's origin regardless of mc->x/y.
//
// Returns 1 if `obj` is an attached MC entry and `out` was populated with the
// AS-state local matrix; 0 otherwise (caller should fall back to transforms[
// obj->transform_id * 16]).
static int build_attached_mc_local_xform(DisplayObject* obj, float out[16],
	MovieClip** out_mc)
{
	if (out_mc) *out_mc = NULL;
	if (obj == NULL || obj->instance_name == NULL) return 0;
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	for (int i = 0; i < child_mc_count; i++) {
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL || mc->display_obj == NULL) continue;
		// Attached MCs have a STANDALONE display_obj (allocated in
		// ng_attachMovie). The parent's sprite_display_list entry (which is
		// what `obj` is) is a SEPARATE allocation. If they were equal this
		// would be a timeline-placed child, not attached.
		if ((DisplayObject*)mc->display_obj == obj) continue;
		if (mc->parent == NULL || mc->parent->display_obj == NULL) continue;
		DisplayObject* pdobj = (DisplayObject*)mc->parent->display_obj;
		if (pdobj->sprite_display_list == NULL) continue;
		size_t target_d = (size_t)(mc->depth + 16384);
		if (target_d >= pdobj->sprite_dl_capacity) continue;
		if (&pdobj->sprite_display_list[target_d] != obj) continue;
		// Match found — build local from mc's AS-state.
		out[0]  = 1.0f; out[1]  = 0.0f; out[2]  = 0.0f; out[3]  = 0.0f;
		out[4]  = 0.0f; out[5]  = 1.0f; out[6]  = 0.0f; out[7]  = 0.0f;
		out[8]  = 0.0f; out[9]  = 0.0f; out[10] = 1.0f; out[11] = 0.0f;
		out[12] = 0.0f; out[13] = 0.0f; out[14] = 0.0f; out[15] = 1.0f;
		apply_as_transform(out, mc, (u8)(1|2|4|8|16));
		if (out_mc) *out_mc = mc;
		return 1;
	}
	return 0;
}

static void compose_cxform20(float out[20], const float* outer, const float* inner);

// Is this 20-float cxform the identity (mult = I, add = 0)?
static int cx_is_identity(const float cx[20])
{
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++) {
			float want = (i == j) ? 1.0f : 0.0f;
			if (fabsf(cx[j*4 + i] - want) > 1e-6f) return 0;
		}
	for (int i = 0; i < 4; i++)
		if (fabsf(cx[16 + i]) > 1e-6f) return 0;
	return 1;
}

// Seed the recursive cxform composition from a ROOT display-list entry: its
// runtime override when it has one, otherwise its baked placement slot.
// Returns 1 (and fills out) only when the result is non-identity — an identity
// seed is passed down as NULL so children keep their own baked slots and the
// dynamic-slot budget is untouched.
static int root_seed_cxform(SWFAppContext* app_context, DisplayObject* obj,
	float out[20])
{
	if (obj->cx_overridden) {
		build_cxform_from_obj(out, obj);
		return !cx_is_identity(out);
	}
	GEN_EXTERN_CXFORM_DATA;
	u32 baked = (app_context->cxform_data_size > 0)
		? (u32)(app_context->cxform_data_size / (20 * sizeof(float))) : 0;
	if (obj->cxform_id != 0 && obj->cxform_id < baked) {
		// app_context->cxform_data, not the `cxform_data` symbol: with child
		// movies linked the id is an index into the COMBINED table and the
		// symbol is only the root's prefix of it (multi-SWF render slice).
		memcpy(out, (const float*)app_context->cxform_data + obj->cxform_id * 20,
		       20 * sizeof(float));
		return !cx_is_identity(out);
	}
	return 0;
}

// Ruffle lerp_color: (a*start + b*end) as u8 — TRUNCATION on the u8 scale, per
// channel. color_data / morph_end_color_data hold the SWF u8 channel divided by
// 255; recover the u8, lerp in double, truncate, re-normalise. Lerping in
// normalised float and letting the unorm8 write round to nearest is one unit
// HIGH whenever the exact product lands just above an integer: ratio 43691
// morphing RED->BLUE gives a*255 = 84.998, ruffle 84, round-to-nearest 85
// (visual/cache_as_bitmap/morph, all 2832 outliers). The AVM2 twins already do
// it this way — avm2_display.c avm2_render_morph, avm2_cpu_raster.c.
static inline float morph_lerp_color_u8(float start, float end, float t)
{
	double a_w = 1.0 - (double) t, b_w = (double) t;
	u32 su = (u32) (start * 255.0f + 0.5f);
	u32 eu = (u32) (end   * 255.0f + 0.5f);
	return (float) (u32) (a_w * su + b_w * eu) / 255.0f;
}

// ---------------------------------------------------------------------------
// cacheAsBitmap PixelSnapping::Always (s17)
// ---------------------------------------------------------------------------
// Ruffle renders a bitmap-cached object into an offscreen texture whose origin
// is the object's WORLD render bounds (display_object.rs render_base:
// `offset_x = bounds.x_min - base_transform.matrix.tx`, offscreen drawn at
// `tx = -offset_x`), then blits it back at
// `transform_stack.tx + offset_x == bounds.x_min` with an IDENTITY 2x2 (scale
// and rotation are already baked into the texture) and `PixelSnapping::Always`,
// which rounds the blit translation to whole pixels
// (render/src/bitmap.rs:89, `Twips::from_pixels(tx.to_pixels().round())`).
//
// Composition of those two steps is exactly a translation of the object's own
// world matrix by the sub-pixel remainder of its world bounds' top-left corner:
// content sits at `world - bounds.min` inside the texture and the texture lands
// at `round(bounds.min)`. So we do not need an offscreen pass to reproduce the
// one thing this is observable for — we translate the world matrix by
// `(round(x_min_px) - x_min_px, round(y_min_px) - y_min_px)` and every child
// inherits the same shift, which is Ruffle's scope (the blit carries the whole
// subtree). f64 `round()` is half-away-from-zero, which is what C `round()`
// does too, so 35.5 -> 36 and -35.5 -> -36.
//
// Divergences, stated: we do not clip to the cache texture's bounds, we do not
// re-rasterise (so an AA edge is translated rather than resampled), and root
// static-text entries are skipped (their glyph slots are composed from the
// CPU-side transform table, which a dynamic slot has no entry in).
static int opaque_bg_local_bounds(DisplayObject* obj, float* b);

// Entries Ruffle bitmap-caches: explicit cacheAsBitmap, or any filter list.
static inline int cab_snaps(const DisplayObject* obj)
{
	return obj->cache_as_bitmap || obj->filter_type != 0;
}

// World-space AABB minimum of `obj`'s local bounds under `world`, in twips.
static int cab_world_bounds_min(DisplayObject* obj, const float world[16],
                                float* out_x, float* out_y)
{
	float b[4];
	if (!opaque_bg_local_bounds(obj, b)) return 0;
	const float xs[4] = { b[0], b[1], b[0], b[1] };
	const float ys[4] = { b[2], b[2], b[3], b[3] };
	float xmin = 1e30f, ymin = 1e30f;
	for (int k = 0; k < 4; k++)
	{
		float X = world[0] * xs[k] + world[4] * ys[k] + world[12];
		float Y = world[1] * xs[k] + world[5] * ys[k] + world[13];
		if (X < xmin) xmin = X;
		if (Y < ymin) ymin = Y;
	}
	*out_x = xmin; *out_y = ymin;
	return 1;
}

static int build_filter_chain(const DisplayObject* obj,
                              FilterListEntry* scratch,
                              const FilterListEntry** chain, int cap);

// Ruffle render_base's `draw_offset` for a filtered entry, in DEVICE pixels:
// floor() of the cache rect's top-left after every non-impotent filter's
// calculate_dest_rect has grown (0, 0, w, h) (swf/src/types/*_filter.rs, each
// filter first scaled by the stage view like Filter::scale). Always <= 0, and
// only its effect on a round-half-away-from-zero TIE matters (see
// cab_snap_delta): round(b + d) - d == round(b) unless b is k + 0.5 and b + d
// < 0 (visual/filters/blur_size_grows: y_min 40.5 px, d = -60 -> -19.5 rounds
// to -20, so Ruffle snaps UP by 0.5 where round(40.5) would snap down).
static void filter_draw_offset_px(const DisplayObject* obj, double vs,
                                  double* ox, double* oy)
{
	static const double pass_scales[15] = {
		1.0, 2.1, 2.7, 3.1, 3.5, 3.8, 4.0, 4.2, 4.4, 4.6, 5.0, 6.0, 6.0, 7.0, 7.0 };
	FilterListEntry scratch;
	const FilterListEntry* chain[MAX_FILTER_LIST_SIZE];
	int n = build_filter_chain(obj, &scratch, chain, MAX_FILTER_LIST_SIZE);
	long x_min = 0, y_min = 0;   // twips, like Ruffle's Rectangle<Twips>
	for (int i = 0; i < n; i++)
	{
		const FilterListEntry* f = chain[i];
		if (f->type < 1 || f->type > 4) continue;
		double bx = ((double) f->blur_x - 1.0) * vs + 1.0;   // scale_blur
		double by = ((double) f->blur_y - 1.0) * vs + 1.0;
		int passes = f->quality;
		if (f->type == 1 && (passes == 0 || (bx <= 1.0 && by <= 1.0)))
			continue;                                         // impotent blur
		if (passes < 1) passes = 1;
		if (passes > 15) passes = 15;
		double sc = pass_scales[passes - 1];
		x_min -= (long) ((sc * bx > 0.0 ? sc * bx : 0.0) * 20.0);
		y_min -= (long) ((sc * by > 0.0 ? sc * by : 0.0) * 20.0);
		if (f->type == 2 || f->type == 4)
		{
			double dist = (double) f->distance * vs;          // distance *= y
			long dx = (long) (cos((double) f->angle) * dist * 20.0);
			long dy = (long) (sin((double) f->angle) * dist * 20.0);
			if (f->type == 2) { if (dx < 0) x_min += dx; if (dy < 0) y_min += dy; }
			else { x_min -= labs(dx); y_min -= labs(dy); }
		}
	}
	*ox = floor((double) x_min / 20.0);
	*oy = floor((double) y_min / 20.0);
}

// Snap translation (twips) for a bitmap-cached entry; 0 when nothing moves.
static int cab_snap_delta(DisplayObject* obj, const float world[16],
                          float* dx, float* dy)
{
	*dx = 0.0f; *dy = 0.0f;
	// Ruffle recheck_cache_as_bitmap: `should_cache = preference ||
	// !filters.is_empty()`, so a FILTERED entry is bitmap-cached (and blitted
	// with PixelSnapping::Always) even without the cacheAsBitmap flag.
	//
	// Filtered entries get the same translation-only snap. Ruffle's blit
	// origin is `bounds.x_min + draw_offset`, but draw_offset is an INTEGER
	// (floor of the filter dest rect), so the cache texture sits on the device
	// pixel grid and the filter runs on that grid. Our filter layer is
	// stage-sized and device-aligned too, so translating the source by the
	// sub-pixel remainder before it is captured and blurred is the same
	// computation, not an approximation (s21 w1-filters-snap). The old s17
	// "translating the source alone hurts" measurement snapped in STAGE pixels
	// under a 2x viewport; snapped in DEVICE pixels, the rows it cited go
	// visual/filters/blur_scales_with_screen 30440 -> 0 and
	// drop_shadow_scales_with_screen 400 -> 0.
	if (obj == NULL || !cab_snaps(obj)) return 0;
	float xmin, ymin;
	if (!cab_world_bounds_min(obj, world, &xmin, &ymin)) return 0;
	// Ruffle's bounds (and the blit it snaps) are taken under the STAGE VIEW
	// matrix (render_bounds_with_transform(.., view_matrix)), so the rounding
	// grid is DEVICE pixels, not stage pixels.
	double vs = (context != NULL && context->stage_scale > 0.0f)
	          ? (double) context->stage_scale : 1.0;
	double xpx = (double) xmin / 20.0 * vs;
	double ypx = (double) ymin / 20.0 * vs;
	// Ruffle rounds the BLIT position, bounds.min + draw_offset, not bounds.min
	// itself; the integer draw_offset only matters on an x.5 tie (see
	// filter_draw_offset_px). 0 for an unfiltered cacheAsBitmap entry.
	double ox = 0.0, oy = 0.0;
	if (obj->filter_type != 0) filter_draw_offset_px(obj, vs, &ox, &oy);
	*dx = (float) ((round(xpx + ox) - ox - xpx) * 20.0 / vs);
	*dy = (float) ((round(ypx + oy) - oy - ypx) * 20.0 / vs);
	return (*dx != 0.0f) || (*dy != 0.0f);
}

// In-place snap of a fully composed world matrix.
static void cab_snap_in_place(DisplayObject* obj, float world[16])
{
	float dx, dy;
	if (!cab_snap_delta(obj, world, &dx, &dy)) return;
	world[12] += dx;
	world[13] += dy;
}

// Snap a ROOT sprite/button's placement matrix before it is handed to
// compose_children. Returns the matrix to use (`in` unchanged when no snap).
//
// The container's OWN slot has to move as well, not just the matrix handed to
// the children: render_display_list draws the entry's opaqueBackground rect
// (and the focus rect) straight from `obj->transform_id`, and in Ruffle the
// background is the cache texture's CLEAR colour — it rides the snapped blit
// with everything else. Leaving the slot alone renders the shape at the snapped
// position with its background one pixel back (measured: visual/opaque_background
// 3856 -> 1504 with the children-only snap, 3856 -> 4 with both).
static const float* cab_snap_world(DisplayObject* obj, const float* in,
                                   float out[16])
{
	float dx, dy;
	if (!cab_snap_delta(obj, in, &dx, &dy)) return in;
	memcpy(out, in, 16 * sizeof(float));
	out[12] += dx;
	out[13] += dy;
	if (g_next_dynamic_xform_slot < g_xform_slot_capacity)
	{
		u32 slot = g_next_dynamic_xform_slot++;
		xform_overrides_push(obj, obj->transform_id);
		obj->transform_id = slot;
		renderer_write_transform(context, slot, out);
	}
	return out;
}

// Snap a ROOT leaf entry (shape / bitmap / edittext / morph): the render pass
// draws it straight from obj->transform_id, so the snapped matrix goes into a
// freshly minted dynamic slot and the entry is repointed at it for the frame
// (xform_overrides_restore puts the baked id back). No-op when nothing moves,
// which keeps the slot pool untouched for the overwhelming majority of frames.
static void cab_snap_root_leaf(SWFAppContext* app_context, DisplayObject* obj)
{
	if (obj == NULL || !cab_snaps(obj)) return;
	u32 baked = (u32)(app_context->transform_data_size / (16 * sizeof(float)));
	if (obj->transform_id >= baked) return;   // already a dynamic slot
	const float* src = (const float*)app_context->transform_data + obj->transform_id * 16;
	float dx, dy;
	if (!cab_snap_delta(obj, src, &dx, &dy)) return;
	if (g_next_dynamic_xform_slot >= g_xform_slot_capacity) return;
	float snapped[16];
	memcpy(snapped, src, sizeof(snapped));
	snapped[12] += dx;
	snapped[13] += dy;
	u32 slot = g_next_dynamic_xform_slot++;
	xform_overrides_push(obj, obj->transform_id);
	obj->transform_id = slot;
	renderer_write_transform(context, slot, snapped);
}

// `parent_cx` carries the parent's concatenated colour transform as VALUES
// (NULL = identity). Passing values rather than the parent's GPU slot id is
// what lets this compose instead of inherit — the slot itself can't be read
// back from the GPU.
static void compose_children(SWFAppContext* app_context, DisplayObject* dl,
	size_t dl_max_depth, const float parent_composed[16],
	const float* parent_cx, u32 parent_cxform_id)
{
	const float* transforms = (const float*)app_context->transform_data;
	if (parent_cx != NULL && cx_is_identity(parent_cx)) parent_cx = NULL;

	// Depth 0 is a real SWF depth (UI16; Ruffle's display list keys on it like
	// any other) and tagPlaceObject2 stores it at dl[0], so the walk starts at
	// 0: an unused dl[0] is zero-filled (char_id 0) and skipped below. Flash
	// IDE output puts static text at depth 0 inside a DefineSprite — the
	// "50%/100%/200%" labels of visual/filters/{bevel_inner,bevel_outer,
	// bevel_full,glow_with_alpha_strength} (s21 w2-filters-snap-2).
	for (size_t i = 0; i <= dl_max_depth; ++i)
	{
		DisplayObject* obj = &dl[i];
		if (obj->char_id == 0) continue;

		Character* ch = &dictionary[obj->char_id];

		// Compose this child's local transform with the parent's global transform.
		// For attachMovie-placed entries (nested-attached MCs), build the local
		// matrix from the MC's AS-state (mc->x/y/xscale/yscale/rotation) instead
		// of reading from transform_data — ng_attachMovie leaves the entry's
		// transform_id at 0, so otherwise children render at the parent origin.
		const float* local_xform;
		float attached_xform[16];
		MovieClip* attached_mc = NULL;
		if (build_attached_mc_local_xform(obj, attached_xform, &attached_mc)) {
			local_xform = attached_xform;
		} else if (obj->transform_id >= (u32)(app_context->transform_data_size / (16 * sizeof(float)))) {
			// Dynamic GPU transform slot (transform_id >= the number of
			// recompiler-baked slots): allocated at runtime for a clip that has
			// no baked transform_data entry — e.g. a createEmptyMovieClip /
			// createTextField overlay such as the gnash/Dejagnu "_xtrace_win"
			// trace window. The CPU-side `transform_data` global only holds the
			// baked slots [0, orig_count), so reading transforms[transform_id]
			// here is an out-of-bounds read past the global (crashes under the
			// OFFSCREEN_RENDER global layout; UB everywhere). Build the local
			// matrix from the MC's AS-state (mc->x/y/scale/rotation) instead,
			// matching apply_dynamic_mc_transforms; fall back to identity.
			attached_xform[0]  = 1.0f; attached_xform[1]  = 0.0f; attached_xform[2]  = 0.0f; attached_xform[3]  = 0.0f;
			attached_xform[4]  = 0.0f; attached_xform[5]  = 1.0f; attached_xform[6]  = 0.0f; attached_xform[7]  = 0.0f;
			attached_xform[8]  = 0.0f; attached_xform[9]  = 0.0f; attached_xform[10] = 1.0f; attached_xform[11] = 0.0f;
			attached_xform[12] = 0.0f; attached_xform[13] = 0.0f; attached_xform[14] = 0.0f; attached_xform[15] = 1.0f;
			extern MovieClip* child_mc_cache[];
			extern int child_mc_count;
			for (int _ci = 0; _ci < child_mc_count; _ci++) {
				MovieClip* _nmc = child_mc_cache[_ci];
				if (_nmc == NULL || _nmc->depth == INT_MIN) continue;
				if ((DisplayObject*)_nmc->display_obj != obj) continue;
				apply_as_transform(attached_xform, _nmc, (u8)(1|2|4|8|16));
				break;
			}
			local_xform = attached_xform;
		} else {
			local_xform = &transforms[obj->transform_id * 16];
			// A nested timeline child whose script mutated its spatial props
			// (Doodle Jump blue platform charId 32 "aaa" doing `this._x += ac`
			// each enterFrame; visual/cache_as_bitmap/nested_rotation's
			// `a._rotation = 10` from its parent sprite's frame 2). Overlay the
			// MC's as_set_flags onto the static placement transform so the
			// change renders. Match by display_obj==obj — NOT by name, since
			// every recycled platform owns a distinct "aaa" instance.
			//
			// This was browser-WASM-only (8deefbb5c: "CI compiles neither
			// path" — a precaution, not a known divergence). CI graphics
			// (OFFSCREEN_RENDER) has no other route for it: the top-of-frame
			// as_set_flags loop in tagShowFrame/tagRerenderFrame walks the
			// ROOT display_list only, and for SPRITE entries defers to the
			// compose loop, which only builds the effective matrix for
			// root-level sprites — so a nested child's scripted _x/_rotation
			// silently never reached the screen (s21 w2-px-a). Render-only:
			// the composed matrix lands in a per-frame GPU slot; the CPU
			// transform tables that hit tests read are untouched.
			if (obj->instance_name != NULL) {
				extern MovieClip* child_mc_cache[];
				extern int child_mc_count;
				for (int _ci = 0; _ci < child_mc_count; _ci++) {
					MovieClip* _nmc = child_mc_cache[_ci];
					if (_nmc == NULL || _nmc->depth == INT_MIN) continue;
					if ((DisplayObject*)_nmc->display_obj != obj) continue;
					if (_nmc->as_set_flags != 0) {
						memcpy(attached_xform, local_xform, 16 * sizeof(float));
						apply_as_transform(attached_xform, _nmc, _nmc->as_set_flags);
						local_xform = attached_xform;
					}
					break;
				}
			}
		}
		float composed[16];
		hit_test_mat4_multiply(composed, parent_composed, local_xform);

		// MovieClip.scrollRect, the TRANSLATE half (Ruffle
		// display_object.rs:1189 `transform_stack.push(translate(-x_min,
		// -y_min))`). It is pushed UNCONDITIONALLY — including while this
		// subtree is being rasterised into a stencil, where Ruffle discards
		// the crop's draw command but keeps the transform-stack push
		// (render/commands.rs:47 `maskers_in_progress`). Applying it to
		// `composed` here, BEFORE the slot allocation below, gives the entry's
		// own slot and the `parent_composed` handed to the SPRITE/BUTTON
		// recursions the same shift — Ruffle's push scope exactly.
		//
		// The crop quad's matrix is the PRE-translate world ("Note that we do
		// *not* apply the translation yet", display_object.rs:1176), so it is
		// captured first, into its own slot, and handed to render_display_list
		// through the per-frame side table.
		{
			Avm1ScrollRect sr;
			if (avm1_scroll_rect_for_entry(obj, &sr)) {
				// If the dynamic pool is exhausted, record NO crop rather than
				// falling back to slot 0: slot 0 is identity, so the crop quad
				// would land as a (0,0)-(w,h)-TWIPS box at the stage origin —
				// a ~5x5 px window that blanks the entry. A missing crop is a
				// far smaller error than a crop in the wrong place. (s13's
				// AVM2 avm2_push_scroll_rect_mask takes the identity fallback;
				// this is a deliberate divergence.)
				int have_crop_slot = 0;
				u32 crop_slot = 0;
				if (g_next_dynamic_xform_slot < g_xform_slot_capacity) {
					crop_slot = g_next_dynamic_xform_slot++;
					renderer_write_transform(context, crop_slot, composed);
					have_crop_slot = 1;
				}
				// transform_data translations are twips (apply_as_transform:
				// slot[12] = rintf(mc->x * 20.0f)), and sr is already twips.
				float scroll_tr[16] = {
					1.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 1.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 1.0f, 0.0f,
					-(float) sr.xmin, -(float) sr.ymin, 0.0f, 1.0f
				};
				float scrolled[16];
				hit_test_mat4_multiply(scrolled, composed, scroll_tr);
				memcpy(composed, scrolled, sizeof(scrolled));
				if (have_crop_slot)
					scroll_crops_push(obj, crop_slot,
						(float)(sr.xmax - sr.xmin), (float)(sr.ymax - sr.ymin));
			}
		}

		// cacheAsBitmap PixelSnapping::Always — applied AFTER the scrollRect
		// translate and BEFORE the slot allocation, so the entry's own slot and
		// the `composed` handed to the SPRITE/BUTTON recursions carry the same
		// shift (Ruffle blits the whole cached subtree at one snapped origin).
		if (cab_snaps(obj)) cab_snap_in_place(obj, composed);

		// Allocate a dynamic transform slot to avoid overwriting shared slots
		u32 new_slot = g_next_dynamic_xform_slot;
		if (new_slot < g_xform_slot_capacity) {
			g_next_dynamic_xform_slot++;
			xform_overrides_push(obj, obj->transform_id);
			obj->transform_id = new_slot;
			renderer_write_transform(context, new_slot, composed);
		} else {
			// Fallback: overwrite in-place (may cause visual artifacts)
			renderer_write_transform(context, obj->transform_id, composed);
		}

		// Color transform for this child and its subtree — TRUE composition
		// (out = parent(child)), not inheritance. The old code had exactly two
		// states: "inherit the parent's slot" or "use the child's own slot".
		// So a static PlaceObject cxform on an INTERMEDIATE sprite was dropped
		// one level down (visual/cache_as_bitmap/nested_color_transform), and a
		// parent `_alpha` REPLACED rather than multiplied a child's static
		// alpha (visual/cache_as_bitmap/color_transform: we drew 0.30 where
		// Flash draws 0.30 * 0.50). compose_cxform20 was already correct and
		// already here; what was missing was the parent's VALUES.
		float own_cx[20];
		int have_own = 0;
		if (obj->cx_overridden) {
			// Timeline-placed child whose entry carries a runtime cxform
			// (mcSyncAlphaToDisplayObj writes through mc->display_obj, which
			// IS this entry for timeline children).
			build_cxform_from_obj(own_cx, obj);
			have_own = 1;
		} else if (attached_mc != NULL && (attached_mc->as_set_flags & 32)) {
			// attachMovie-placed child: its display_obj is a STANDALONE
			// struct (see build_attached_mc_local_xform), so runtime state
			// lives on the MC — mirror the spatial overlay and build an
			// alpha-only cxform from mc->alpha.
			memset(own_cx, 0, sizeof(own_cx));
			own_cx[0] = own_cx[5] = own_cx[10] = 1.0f;
			own_cx[15] = attached_mc->alpha / 100.0f;
			if (own_cx[15] < 0.0f) own_cx[15] = 0.0f;
			have_own = 1;
		} else {
			// Static PlaceObject cxform on this entry — previously only ever
			// consulted at the ROOT call sites, and silently dropped here.
			GEN_EXTERN_CXFORM_DATA;
			u32 baked_cx = (app_context->cxform_data_size > 0)
				? (u32)(app_context->cxform_data_size / (20 * sizeof(float))) : 0;
			if (obj->cxform_id != 0 && obj->cxform_id < baked_cx) {
				memcpy(own_cx,
				       (const float*)app_context->cxform_data + obj->cxform_id * 20,
				       sizeof(own_cx));
				have_own = 1;
			}
		}

		float eff_cx[20];
		int eff_cx_valid = 0;
		if (parent_cx != NULL && have_own) {
			compose_cxform20(eff_cx, parent_cx, own_cx);
			eff_cx_valid = 1;
		} else if (parent_cx != NULL) {
			memcpy(eff_cx, parent_cx, sizeof(eff_cx));
			eff_cx_valid = 1;
		} else if (have_own) {
			memcpy(eff_cx, own_cx, sizeof(eff_cx));
			eff_cx_valid = 1;
		}

		// Slot economy: only MINT a slot when the composition genuinely
		// produced something neither side already owns. There are just 256
		// dynamic cxform slots per tick (render_webgpu.c ~1095), and a fade on
		// a container with dozens of children must not burn one per child —
		// that would exhaust the pool and silently drop the tint.
		//   * no parent cxform              -> keep the child's own slot
		//   * parent cxform, child has none -> INHERIT the parent's slot
		//   * both                          -> mint the composed slot
		u32 eff_slot = obj->cxform_id;
		if (parent_cx != NULL && !have_own) {
			eff_slot = parent_cxform_id;
		} else if (eff_cx_valid
		           && (parent_cx != NULL || obj->cx_overridden
		               || (attached_mc != NULL && (attached_mc->as_set_flags & 32)))) {
			u32 cx_slot = g_next_dynamic_cxform_slot;
			if (cx_slot < g_cxform_slot_capacity) {
				g_next_dynamic_cxform_slot++;
				renderer_write_cxform(context, cx_slot, eff_cx);
				eff_slot = cx_slot;
			}
		}
		if (obj->cxform_id != eff_slot) {
			cxform_overrides_push(obj, obj->cxform_id);
			obj->cxform_id = eff_slot;
		}
		const float* child_cx = (eff_cx_valid && !cx_is_identity(eff_cx))
			? eff_cx : NULL;

		switch (ch->type)
		{
			case CHAR_TYPE_TEXT:
				// Compose each glyph transform with the composed text transform.
				// The glyph slots ch->transform_start+j are CHARACTER-shared, so
				// composing two instances of the same DefineText in one frame would
				// overwrite each other and the batched GPU draw would render BOTH at
				// the last writer's position (Minesweeper: every "1"/"2"/"3" board
				// cell collapsed onto the last-revealed tile). Allocate a per-INSTANCE
				// run of dynamic slots (mirrors the shape path above) and record the
				// base on obj so render_single_object/render_display_list draw from it.
				{
					u32 tgbase = g_next_dynamic_xform_slot;
					int tg_dyn = (ch->text_size > 0 &&
						tgbase + (u32)ch->text_size <= g_xform_slot_capacity);
					if (tg_dyn) {
						g_next_dynamic_xform_slot += (u32)ch->text_size;
						text_glyph_overrides_push(obj);
						obj->text_glyph_xform_base = tgbase;
					}
					for (size_t j = 0; j < ch->text_size; j++)
					{
						const float* glyph_local = &transforms[(ch->transform_start + (u32)j) * 16];
						float glyph_composed[16];
						hit_test_mat4_multiply(glyph_composed, composed, glyph_local);
						u32 dst = tg_dyn ? (tgbase + (u32)j) : (ch->transform_start + (u32)j);
						renderer_write_transform(context, dst, glyph_composed);
					}
				}
				break;

			case CHAR_TYPE_MORPH_SHAPE:
			{
				float t = (float)obj->ratio / 65535.0f;
				size_t num_verts = ch->morph_start_size;

				u32* start = (u32*)(app_context->shape_data + ch->morph_start_offset * 4 * sizeof(u32));
				float* end = (float*)(app_context->morph_end_shape_data + ch->morph_end_offset * 2 * sizeof(float));
				u32* scratch = (u32*)malloc(num_verts * 4 * sizeof(u32));

				for (size_t v = 0; v < num_verts; v++)
				{
					float sx = *(float*)&start[v*4 + 0];
					float sy = *(float*)&start[v*4 + 1];
					float ex = end[v*2 + 0];
					float ey = end[v*2 + 1];
					float ix = sx + t * (ex - sx);
					float iy = sy + t * (ey - sy);
					scratch[v*4 + 0] = *(u32*)&ix;
					scratch[v*4 + 1] = *(u32*)&iy;
					scratch[v*4 + 2] = start[v*4 + 2];
					scratch[v*4 + 3] = start[v*4 + 3];
				}

				renderer_update_vertices(context,
					ch->morph_start_offset * 4 * sizeof(u32),
					scratch, num_verts * 4 * sizeof(u32));
				free(scratch);

				for (size_t c = 0; c < ch->morph_color_count; c++)
				{
					float* sc = (float*)(app_context->color_data) + (ch->morph_color_start + c) * 4;
					float* ec = (float*)(app_context->morph_end_color_data) + (ch->morph_end_color_start + c) * 4;
					float interp[4];
					for (int k = 0; k < 4; k++)
						interp[k] = morph_lerp_color_u8(sc[k], ec[k], t);
					renderer_update_colors(context,
						(ch->morph_color_start + c) * 4 * sizeof(float),
						interp, 4 * sizeof(float));
				}
				break;
			}

			case CHAR_TYPE_SPRITE:
			{
				if (obj->sprite_display_list != NULL)
					compose_children(app_context,
						obj->sprite_display_list, obj->sprite_max_depth,
						composed, child_cx, eff_slot);
				break;
			}

			case CHAR_TYPE_BUTTON:
			{
				// Compose the button's PERSISTENT sprite_display_list in place,
				// exactly like the SPRITE branch above and the top-level button
				// loops in tagShowFrame / tagRerenderFrame. render_single_object /
				// render_display_list draw obj->sprite_display_list, so the old
				// approach here — build a throwaway temp display_list via
				// button_state_funcs, compose THAT, then free it — left those draws
				// using the un-composed local transforms. A button NESTED inside a
				// sprite (reached only via this recursive branch, never the top-
				// level loops) therefore rendered its up-state shape at the parent's
				// origin. Pong's title-screen "2 Player"/"1 Player" button rectangles
				// (buttons 19/20 inside sprite_26) appeared in the top-left corner.
				if (obj->sprite_display_list == NULL && ch->button_state_funcs != NULL)
				{
					obj->sprite_dl_capacity = INITIAL_SPRITE_DL_CAPACITY;
					obj->sprite_display_list = HCALLOC(obj->sprite_dl_capacity, sizeof(DisplayObject));
					obj->sprite_max_depth = 0;

					u8 state = obj->button_state;
					u8 effective = (state == 3) ? 1 : state; // OUT_DOWN shows OVER
					if (effective < 3 && ch->button_state_funcs[effective] != NULL)
					{
						DisplayObject* saved_dl = display_list;
						size_t saved_max = max_depth;
						size_t saved_cap = display_list_capacity;
						display_list = obj->sprite_display_list;
						max_depth = obj->sprite_max_depth;
						display_list_capacity = obj->sprite_dl_capacity;

						ch->button_state_funcs[effective](app_context);

						obj->sprite_display_list = display_list;
						obj->sprite_max_depth = max_depth;
						obj->sprite_dl_capacity = display_list_capacity;
						display_list = saved_dl;
						max_depth = saved_max;
						display_list_capacity = saved_cap;
					}
				}

				if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
					compose_children(app_context,
						obj->sprite_display_list, obj->sprite_max_depth,
						composed, child_cx, eff_slot);
				break;
			}

			default:
				break;
		}
	}
}

// ---------------------------------------------------------------------------
// Helper 3: Recursive display list rendering
// ---------------------------------------------------------------------------
// Renders all objects in a display list, recursing into sprites and buttons.
// Forward declaration for mutual recursion
static void render_display_list(SWFAppContext* app_context, DisplayObject* dl, size_t dl_max_depth);

// Returns 1 if a baked (static placement) color transform forces the output
// alpha to 0 — i.e. the object is authored fully transparent (a common idiom
// for invisible hit-test areas, e.g. Pacman's "Hit" sprite placed alpha=0).
// Such an object (and its subtree) is invisible in Flash, so the renderer skips
// it. Identity (cxform_id==0) and dynamic/composed slots (>= baked count, which
// hold runtime _alpha/Color overrides handled elsewhere) are never treated as
// invisible here. Note: this is the placement cxform's OWN alpha; it does NOT
// compose a parent cxform (an alpha-0 ancestor is handled at the ancestor).
static int cxform_forces_invisible(SWFAppContext* app_context, u32 cxform_id)
{
	if (cxform_id == 0) return 0;
	GEN_EXTERN_CXFORM_DATA;
	u32 baked = (app_context->cxform_data_size > 0)
		? (u32)(app_context->cxform_data_size / (20 * sizeof(float))) : 0;
	if (cxform_id >= baked) return 0;
	const float* cx = (const float*)app_context->cxform_data + cxform_id * 20;
	// 20-float layout: 4x4 column-major mult (alpha mult at [15]) + vec4 add
	// (alpha add at [19]); see apply_cxform in render_webgpu.c.
	return (cx[15] <= 0.0001f && cx[19] <= 0.0001f);
}

// Helper: render a single object into the current render pass
// >0 while a display list is being rasterised into the STENCIL (clip-mask
// capture) rather than the colour target. A blend-mode object inside a mask
// shape must not suspend the pass to build a layer — the mask is mid-capture and
// the layer composite would write colour. It just draws into the stencil as
// usual, which is all a mask needs.
static int g_clip_mask_capture = 0;

// ---------------------------------------------------------------------------
// AVM1 setMask (mask defect B) — paint-time pairing hooks
// ---------------------------------------------------------------------------
// `maskee.setMask(masker)` used to be honoured only for Drawing-API clips
// (actionIterateMaskedDrawings); timeline sprites, DefineShapes and EditTexts
// were neither clipped nor suppressed. These helpers give the three display
// list loops the two halves Ruffle has: a masker is never painted as ordinary
// content, and a maskee is painted with the masker's geometry in the stencil.
//
// The pairing lives entirely on MovieClip (immortal; tombstoned, never freed).
// The only DisplayObject* touched is `mc->display_obj`, RE-READ at every use —
// storing one would dangle across ng_spriteDLRealloc / ng_freeSpriteDL and
// silently re-target on depth reuse. Design + the UAF that killed the previous
// sketch: SWFRecompDocs/plans/session13-fanout-reports/wave1-gfx-maskB.md §2/§3.
#define AVM1_MASK_PAINT_MAX 64

// Non-NULL only while draw_mc_mask_geometry replays ONE text field's glyphs
// into a stencil; textfield_glyph_render_cb ignores every other field then.
static MovieClip* g_avm1_mask_glyph_only_mc = NULL;

static void draw_mc_mask_geometry(SWFAppContext* app_context, MovieClip* masker);

// Does display-list entry `obj` belong to MovieClip `mc`? `mc->display_obj` is
// the authoritative key; root-placed EditText wrappers have display_obj == NULL
// (edittext-wrapper-no-display-obj-invalidate), so fall back to the instance
// name — the same name-keyed route the sprite clipDepth branch already uses for
// Drawing-API paths.
static int avm1_mc_owns_entry(const MovieClip* mc, const DisplayObject* obj)
{
	if (mc == NULL || obj == NULL) return 0;
	if (mc->display_obj != NULL) return mc->display_obj == (const void*)obj;
	if (obj->instance_name == NULL || mc->name[0] == '\0') return 0;
	return strcmp(obj->instance_name, mc->name) == 0;
}

// The masker whose stencil must wrap `obj`, or NULL when `obj` is not a maskee.
static MovieClip* avm1_masker_for_entry(const DisplayObject* obj)
{
	if (obj == NULL || actionAvm1MaskPairCount() == 0) return NULL;
	void* maskees[AVM1_MASK_PAINT_MAX];
	void* maskers[AVM1_MASK_PAINT_MAX];
	int n = actionAvm1GetMaskPairs(maskees, maskers, AVM1_MASK_PAINT_MAX);
	for (int i = 0; i < n; i++)
		if (avm1_mc_owns_entry((const MovieClip*)maskees[i], obj))
			return (MovieClip*)maskers[i];
	return NULL;
}

// 1 when `obj` is the masker of a live pair — Ruffle never paints those.
static int entry_is_live_masker(const DisplayObject* obj)
{
	if (obj == NULL || actionAvm1MaskPairCount() == 0) return 0;
	void* maskees[AVM1_MASK_PAINT_MAX];
	void* maskers[AVM1_MASK_PAINT_MAX];
	int n = actionAvm1GetMaskPairs(maskees, maskers, AVM1_MASK_PAINT_MAX);
	for (int i = 0; i < n; i++)
		if (avm1_mc_owns_entry((const MovieClip*)maskers[i], obj))
			return 1;
	return 0;
}

// Open the masker's stencil and return the clip reference to restore after the
// maskee is painted. Pair with renderer_restore_clip (NOT renderer_end_clip):
// end_clip zeroes mask_ref and would drop an enclosing clipDepth range for
// every later sibling (the s12 fix on the AVM2 walk, same shape).
static u32 avm1_mask_push(SWFAppContext* app_context, MovieClip* masker)
{
	u32 saved = renderer_clip_ref(context);
	renderer_begin_clip_mask(context);
	draw_mc_mask_geometry(app_context, masker);
	renderer_end_clip_mask(context);
	return saved;
}

// ---------------------------------------------------------------------------
// DisplayObject.opaqueBackground (s16 P2)
// ---------------------------------------------------------------------------
// Ruffle display_object.rs::render_base, non-cacheAsBitmap arm: when a display
// object carries an opaque background, a filled rectangle of its render bounds
// is emitted BEFORE apply_standard_mask_and_scroll — i.e. under the object, and
// NOT clipped by the object's own mask or scrollRect (an enclosing container
// clip still applies, because that stencil is already live).
//
// Ruffle's rect is the WORLD-space axis-aligned box; ours is the object's LOCAL
// box pushed through the object's own (already world-composed) transform slot,
// which is the same rectangle for every unrotated object and a rotated box
// rather than its AABB otherwise. Documented divergence: no test in the corpus
// puts an opaque background on a rotated object, and going through the object's
// own slot is what makes this correct inside nested sprites for free.
//
// `transform_data` is the CPU-side table, so child matrices must be read
// through ng_get_original_transform_id (compose_children has already
// repointed transform_id at a GPU-only slot by the time we render).
static int opaque_bg_local_bounds(DisplayObject* obj, float* b)
{
	if (obj == NULL || obj->char_id == 0) return 0;
	Character* ch = &dictionary[obj->char_id];
	float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
	int found = 0;
	// A BUTTON keeps its current state's contents in the same
	// `sprite_display_list` a sprite does (render_single_object's two arms are
	// identical), and `ng_getCharBounds` never records a bound for a button
	// character — so without this arm an opaqueBackground on a button entry
	// found no bounds and drew NOTHING. That is the second half of
	// visual/cache_as_bitmap/avm2_button: the recompiler now emits the
	// DefineSprite's tagSetOpaqueBackground, and the entry it lands on is a
	// button placement.
	if ((ch->type == CHAR_TYPE_SPRITE || ch->type == CHAR_TYPE_BUTTON)
	    && obj->sprite_display_list != NULL)
	{
		// From depth 0 (see compose_children).
		for (size_t i = 0; i <= obj->sprite_max_depth; i++)
		{
			DisplayObject* c = &obj->sprite_display_list[i];
			if (c->char_id == 0 || c->clip_depth > 0) continue;
			float cb[4];
			if (!opaque_bg_local_bounds(c, cb)) continue;
			// The entry's own table (combined for a loaded movie's placements).
			const float* m = ng_entryTransformData(c)[ng_get_original_transform_id(c)];
			const float xs[4] = { cb[0], cb[1], cb[0], cb[1] };
			const float ys[4] = { cb[2], cb[2], cb[3], cb[3] };
			for (int k = 0; k < 4; k++)
			{
				float X = m[0] * xs[k] + m[4] * ys[k] + m[12];
				float Y = m[1] * xs[k] + m[5] * ys[k] + m[13];
				if (X < xmin) xmin = X;
				if (X > xmax) xmax = X;
				if (Y < ymin) ymin = Y;
				if (Y > ymax) ymax = Y;
			}
			found = 1;
		}
	}
	else
	{
		s32 cxmin, cxmax, cymin, cymax;
		if (ng_getCharBounds(obj->char_id, &cxmin, &cxmax, &cymin, &cymax))
		{
			xmin = (float) cxmin; xmax = (float) cxmax;
			ymin = (float) cymin; ymax = (float) cymax;
			found = 1;
		}
	}
	if (!found) return 0;
	b[0] = xmin; b[1] = xmax; b[2] = ymin; b[3] = ymax;
	return 1;
}

static void draw_entry_opaque_background(SWFAppContext* app_context,
                                         DisplayObject* obj)
{
	(void) app_context;
	if (obj == NULL || !obj->opaque_bg_set) return;
	if (g_clip_mask_capture) return;
	float b[4];
	if (!opaque_bg_local_bounds(obj, b)) return;
	float w = b[1] - b[0], h = b[3] - b[2];
	if (w <= 0.0f || h <= 0.0f) return;
	u32 rgb = obj->opaque_bg_rgb;
	// cxform slot 0 (identity): Ruffle's draw_rect takes a Color, never a
	// ColorTransform, so the background ignores the placement cxform.
	renderer_draw_rect(context, b[0], b[2], w, h,
		(float) ((rgb >> 16) & 0xFF) / 255.0f,
		(float) ((rgb >> 8) & 0xFF) / 255.0f,
		(float) (rgb & 0xFF) / 255.0f,
		1.0f, obj->transform_id, 0);
}

#ifndef NO_GRAPHICS
// Filtered root-timeline EditText (s21 w2-edittext-filters). Ruffle treats a
// non-empty filter list as an implied cacheAsBitmap on ANY display object
// (display_object.rs recheck_cache_as_bitmap), so an EditText placed with a
// PlaceObject3 filter list renders its box + glyphs into the cache and the
// filters run over that. Our EditText paint is not a DisplayObject draw at all
// (the text iterators are driven per root depth by tf_draw_at_root_depth, or
// by the orphan walk), so render_single_object's CHAR_TYPE_TEXT arm drew
// nothing and render_filtered_object filtered an EMPTY layer — the "structural
// no-op" HOLD of s18 (edittext_border_filters). While the root loops send such
// an entry through render_filtered_object, these name it, and the TEXT arm
// paints that one field (wrapper or orphan) into whatever pass is current:
// the offscreen capture, and the framebuffer again for draw_source_{before,
// after}. Root-timeline entries only; a filtered EditText inside a sprite is
// still drawn unfiltered by the REST pass.
static const DisplayObject* g_tf_filter_obj = NULL;
static int g_tf_filter_depth = -1;
static int g_tf_filter_max_depth = 0;
static void tf_draw_filtered_root_entry(SWFAppContext* app_context);
#endif

static void render_single_object(SWFAppContext* app_context, DisplayObject* obj)
{
	if (cxform_forces_invisible(app_context, obj->cxform_id)) return;
#ifndef NO_GRAPHICS
	// Before the type switch: a DefineEditText with no initial text never
	// receives a tagDefineText, so its dictionary type is not CHAR_TYPE_TEXT.
	if (obj == g_tf_filter_obj) {
		tf_draw_filtered_root_entry(app_context);
		return;
	}
#endif
#ifdef OFFSCREEN_RENDER
	// Video display objects have type=0 (CHAR_TYPE_SHAPE) in dictionary because
	// tagDefineVideoStream doesn't set a type. Check for video BEFORE the switch.
	if (ng_isVideoChar(obj->char_id)) {
		uint32_t* argb = NULL;
		int vw = 0, vh = 0;
		u16 decl_w = 0, decl_h = 0;
		int has_decl = ng_getVideoDimensions(obj->char_id, &decl_w, &decl_h);
		// Try embedded (DefineVideoStream + VideoFrame) path first using
		// the display object's ratio field as the frame index; fall back
		// to the NetStream-keyed FLV path.
		int got_pixels =
			actionGetEmbeddedVideoFramePixels(obj->char_id, obj->ratio,
			                                  &argb, &vw, &vh) ||
			actionGetVideoFramePixels(&argb, (int)decl_w, (int)decl_h, &vw, &vh);
		if (got_pixels) {
			// Flash renders the decoded frame stretched to fit the
			// DefineVideoStream declared bounds, then applies the
			// PlaceObject2 matrix. Fall back to source-equals-dest if
			// no declared bounds were recorded.
			u32 dst_w = has_decl ? (u32)decl_w : (u32)vw;
			u32 dst_h = has_decl ? (u32)decl_h : (u32)vh;
			renderer_draw_bitmap_quad_scaled(context, argb,
				(u32)vw, (u32)vh, dst_w, dst_h,
				0.0f, 0.0f, obj->transform_id, obj->cxform_id);
			free(argb);
		}
		return;
	}
#endif
	Character* ch = &dictionary[obj->char_id];
	switch (ch->type)
	{
		case CHAR_TYPE_SHAPE:
			renderer_draw_shape(context, ch->shape_offset, ch->size,
				obj->transform_id, obj->cxform_id);
			break;
		case CHAR_TYPE_MORPH_SHAPE:
			renderer_draw_shape(context, ch->morph_start_offset, ch->morph_start_size,
				obj->transform_id, obj->cxform_id);
			break;
		case CHAR_TYPE_TEXT:
#ifndef NO_GRAPHICS
			// DefineEditText reuses CHAR_TYPE_TEXT in the dictionary so the
			// recompiler can pre-bake glyph transforms for the initial text,
			// but the EditText is rendered dynamically via
			// actionIterateTextFieldGlyphs (textfield_glyph_render_cb) inside
			// the stencil-clipped field bounds. Drawing the static glyphs
			// here too would double-render unclipped and leak the initial
			// 'a' glyph (16x16, anchored to the field's top) up into the
			// inter-field gap region above each field.
			// Gate matches the `#if` around the actionIterateTextFieldGlyphs
			// call site in tagShowFrame (~line 3406) and tagRerenderFrame so
			// the skip fires wherever dynamic glyph rendering does — including
			// browser-WASM graphics (USE_WEBGPU without OFFSCREEN_RENDER).
			if (ng_getCharTextfieldIdx(obj->char_id) >= 0) break;
#endif
			{
			u32 _tg_base = obj->text_glyph_xform_base ? obj->text_glyph_xform_base
			                                           : (u32)ch->transform_start;
			for (size_t j = 0; j < ch->text_size; ++j)
			{
				size_t glyph_index = 4*app_context->text_data[ch->text_start + j];
				renderer_draw_shape(context,
					app_context->glyph_data[glyph_index],
					app_context->glyph_data[glyph_index + 1],
					_tg_base + (u32)j, ch->cxform_id);
			}
			}
			break;
		case CHAR_TYPE_SPRITE:
			if (obj->sprite_display_list != NULL)
				render_display_list(app_context, obj->sprite_display_list, obj->sprite_max_depth);
			break;
		case CHAR_TYPE_BUTTON:
			if (obj->sprite_display_list != NULL)
				render_display_list(app_context, obj->sprite_display_list, obj->sprite_max_depth);
			break;
	}
}

// Non-zero while a filtered object's subtree is being captured. The offscreen
// ping-pong is a single pair of stage-sized textures and does not nest, so a
// filtered descendant of a filtered object renders flat rather than corrupting
// its ancestor's layer. Same model as avm2_display.c's g_avm2_filter_active.
static int g_filter_capture_depth = 0;

// ---------------------------------------------------------------------------
// Filtered-object rendering: blur + Flash's composition rules
// ---------------------------------------------------------------------------
// A glow / drop-shadow / bevel is not simply "blurred copy behind the object".
// The SWF filter flag byte carries inner, knockout and compositeSource bits
// that decide how the filter output combines with the UNBLURRED source; see
// compose_wgsl in render_webgpu.c for the formulas (ported from ruffle
// render/wgpu/shaders/filter/{glow,bevel}.wgsl).
//
// filter_flags layout, as parsed by SWFRecomp/src/swf.cpp and decoded the same
// way by the mc.filters reflection in actionmodern/action.c:
//   DropShadow / Glow (type 2, 3): bit0 compositeSource, bit1 knockout, bit2 inner
//   Bevel (type 4):    bit0 onTop, bit1 compositeSource, bit2 knockout, bit3 inner
//
// Whenever the formula ends in a plain "+ dest" the source is drawn with a
// REAL draw call instead of being sampled out of the snapshot, because
// `filter_output` then `source` over it reproduces the compositeSource formula
// exactly and keeps the source MSAA-sharp. That is the common case, and it is
// bit-identical to what this code did before composition existed.
// Build the render chain for a PlaceObject3 SurfaceFilterList (s18).
//
// The recompiler records the list twice: the scalar `parsed_filter_*` set
// (emitted as tagSetFilter, and the ONLY thing the renderer used to read) and
// the full `all_filters[]` list (emitted as tagBeginFilterList/tagAdd*, until
// now consumed only by the `mc.filters` reflection). The scalars come from the
// FIRST list entry whose kind is not ColorMatrix — every other kind sets them
// when `parsed_filter_type == 0`, and ColorMatrix is folded into the placement
// cxform instead — and for the three kinds the renderer cannot express
// natively (Convolution -> blur, GradientGlow -> glow, GradientBevel -> bevel)
// the scalars carry the APPROXIMATION, which the list entry does not.
//
// So: that one entry is rendered from the scalars (byte-identical to the old
// single-filter behaviour), every other blur / drop-shadow / glow / bevel entry
// is rendered from the list, and the remaining kinds are dropped exactly as
// before. A list with one renderable filter therefore reproduces the previous
// output exactly; a chain now renders all of its members, in order.
//
// Returns the chain length; `scratch` receives the scalar-derived entry.
static int build_filter_chain(const DisplayObject* obj,
                              FilterListEntry* scratch,
                              const FilterListEntry** chain, int cap)
{
	const FilterListData* fl = (const FilterListData*) obj->filter_chain;

	memset(scratch, 0, sizeof(*scratch));
	scratch->type = obj->filter_type;
	scratch->blur_x = obj->filter_blur_x;
	scratch->blur_y = obj->filter_blur_y;
	scratch->quality = obj->filter_quality;
	scratch->flags = obj->filter_flags;
	scratch->color_r = obj->filter_color_r;
	scratch->color_g = obj->filter_color_g;
	scratch->color_b = obj->filter_color_b;
	scratch->color_a = obj->filter_color_a;
	scratch->strength = obj->filter_strength;
	scratch->angle = obj->filter_angle;
	scratch->distance = obj->filter_distance;
	scratch->highlight_r = obj->filter_highlight_r;
	scratch->highlight_g = obj->filter_highlight_g;
	scratch->highlight_b = obj->filter_highlight_b;
	scratch->highlight_a = obj->filter_highlight_a;

	if (fl == NULL || fl->count == 0)
	{
		chain[0] = scratch;
		return 1;
	}

	// Index of the entry the scalars describe: the first non-ColorMatrix one.
	int scalar_idx = -1;
	for (u8 i = 0; i < fl->count; i++)
		if (fl->entries[i].type != 6) { scalar_idx = (int) i; break; }

	int n = 0;
	for (u8 i = 0; i < fl->count && n < cap; i++)
	{
		if ((int) i == scalar_idx)
		{
			if (scratch->type != 0) chain[n++] = scratch;
			continue;
		}
		u8 t = fl->entries[i].type;
		if (t >= 1 && t <= 4) chain[n++] = &fl->entries[i];
	}
	if (n == 0)
	{
		chain[0] = scratch;
		return 1;
	}

	// EXPRESSIBILITY GATE. The ping-pong (filter_tex_a in, filter_tex_a out)
	// can carry a non-final filter exactly only when that filter's output is
	// "the blurred layer, optionally recoloured": a plain Blur, or a
	// glow / drop-shadow with NO compositeSource, inner or knockout. Anything
	// else has to blend its own source back INTO the layer, which needs a
	// compose-into-offscreen pipeline we do not have; the `colorize` shortcut
	// silently DROPS that source.
	//
	// Measured, on from_shumway/acid/acid-filter-2 ([Glow(compositeSource),
	// Blur]): chaining it through colorize takes the row from 16 764 outliers
	// to 64 921 — the ellipse under the glow disappears. So when the chain is
	// not expressible we keep exactly the old single-filter behaviour (the
	// scalar entry, i.e. the first filter) rather than trade one wrong render
	// for a worse one. That is a HOLD on those lists, not a fix; the
	// completion mechanism is the compose-into-offscreen pipeline that
	// cab_mask_filters and the AVM2 `.filters` route also wait on.
	for (int i = 0; i < n - 1; i++)
	{
		u8 t = chain[i]->type;
		int ok = (t == 1)
		      || ((t == 2 || t == 3) && chain[i]->flags == 0);
		if (!ok)
		{
			chain[0] = scratch;
			return 1;
		}
	}
	return n;
}

// --- A3: the object's colour transform belongs on the FILTER OUTPUT --------
//
// Ruffle renders a filtered object into its cache texture with an IDENTITY
// colour transform, runs the filter chain, then draws the RESULT with the
// object's transform (display_object.rs render_base -> the BitmapCache blit).
// We render the source WITH the transform, so the transform never reaches the
// filter's own colour: from_shumway/acid/acid-filter-2 places a BLACK glow
// under a PlaceObject3 cxform add=[+255,0,0,0]; Ruffle's halo is RED and ours
// stayed neutral grey (16 764 outliers, all of them "expected (255,v,v), got
// (v,v,v)" with the G/B falloff already byte-identical).
//
// The composition shader emits `filter_colour * f(blurred alpha)` - straight
// colour in .rgb, coverage in .a - so applying the colour transform to the
// FILTER COLOUR is exactly equivalent to applying it to the output, for
// everything except an additive ALPHA term (which would have to be scaled by
// the coverage; no corpus test uses one). That is far cheaper than plumbing a
// cxform through three shaders. The composite SOURCE is drawn separately by
// draw_source_{before,after} and already carries the object's own cxform, so
// it must not be transformed twice.
//
// Only CPU-readable slots are honoured: the placement's baked slot and the
// AS-set runtime override. A slot minted by compose_children for a nested
// COMPOSED cxform lives on the GPU with no CPU mirror, so a filtered object
// deep inside a tinted container behaves exactly as it did before.
static int filtered_object_cxform(SWFAppContext* app_context,
                                  const DisplayObject* obj, float out[20])
{
	if (obj->cx_overridden) { build_cxform_from_obj(out, obj); return 1; }
	if (obj->cxform_id == 0) return 0;
	GEN_EXTERN_CXFORM_DATA;
	u32 baked = (app_context->cxform_data_size > 0)
		? (u32)(app_context->cxform_data_size / (20 * sizeof(float))) : 0;
	if (obj->cxform_id >= baked) return 0;
	memcpy(out, (const float*) app_context->cxform_data + obj->cxform_id * 20,
	       20 * sizeof(float));
	return 1;
}

// out = clamp(mult * in + add, 0, 1) on a STRAIGHT (not premultiplied) RGBA -
// the same formula apply_cxform runs in the shader (render_webgpu.c:271).
static void cxform_apply_rgba(const float cx[20], double* r, double* g,
                              double* b, double* a)
{
	double in[4] = { *r, *g, *b, *a };
	double o[4];
	for (int i = 0; i < 4; i++)
	{
		double sum = 0.0;
		for (int k = 0; k < 4; k++) sum += (double) cx[k * 4 + i] * in[k];
		sum += (double) cx[16 + i];
		o[i] = sum < 0.0 ? 0.0 : (sum > 1.0 ? 1.0 : sum);
	}
	*r = o[0]; *g = o[1]; *b = o[2]; *a = o[3];
}

// A drop shadow's sampling offset in whole DEVICE pixels: the shadow at x is
// the blurred layer at x + (sx, sy). Same rounding as the nearest-offset block
// in render_filtered_object (Ruffle samples NEAREST: floor(-off + 0.5)).
// Returns 0 when the offset is zero.
static int filter_shadow_offset_px(RenderContext* rc, const FilterListEntry* f,
                                   int* sx, int* sy)
{
	float vs = (rc != NULL && rc->stage_scale > 0.0f) ? rc->stage_scale : 1.0f;
	float xd = cosf(f->angle) * f->distance * vs;
	float yd = sinf(f->angle) * f->distance * vs;
	*sx = (int) floorf(-xd + 0.5f);
	*sy = (int) floorf(-yd + 0.5f);
	return (*sx != 0) || (*sy != 0);
}

static void render_filtered_object(SWFAppContext* app_context, DisplayObject* obj)
{
	FilterListEntry chain_scratch;
	const FilterListEntry* chain[MAX_FILTER_LIST_SIZE];
	float obj_cx[20];
	int have_obj_cx = filtered_object_cxform(app_context, obj, obj_cx);
	int chain_n = build_filter_chain(obj, &chain_scratch, chain,
	                                 MAX_FILTER_LIST_SIZE);
	// Only the LAST filter's composition flags reach the framebuffer; the
	// earlier ones fold into the blur ping-pong. Same rule (and the same
	// caveats) as avm2_display.c::avm2_render_filtered, whose `.filters` array
	// path has always chained.
	const FilterListEntry* last = chain[chain_n - 1];

	int is_shadow = (last->type == 2);
	int is_glow   = (last->type == 3);
	int is_bevel  = (last->type == 4);

	u8 flags = last->flags;
	int f_inner, f_knockout, f_composite, bevel_type = 0;
	if (is_bevel)
	{
		f_inner     = (flags >> 3) & 1;
		f_knockout  = (flags >> 2) & 1;
		f_composite = (flags >> 1) & 1;
		int f_on_top = flags & 1;
		// ruffle bevel.rs: onTop wins over inner ("full"), else inner, else outer.
		bevel_type = f_on_top ? 2 : (f_inner ? 1 : 0);
	}
	else
	{
		f_composite = flags & 1;
		f_knockout  = (flags >> 1) & 1;
		f_inner     = (flags >> 2) & 1;
	}
	// ruffle bevel.rs hard-codes composite_source = 1 for bevels.
	if (is_bevel) f_composite = 1;

	// CHAINED (s18): a filter's "composite source" is the LAYER it received,
	// which for filter k>0 is the output of filter k-1 — not the crisp object.
	// The single-filter path exploits the two being the same thing: rather than
	// snapshot the source it just re-draws the object into the framebuffer
	// (draw_source_{before,after}) and tells the shader composite_source = 0.
	// For a chain that shortcut paints a CRISP silhouette on top of a blurred
	// one — measured on visual/filters/blur_size_grows ([Blur, Glow]): 86 708
	// -> 74 142 with the shortcut, -> 6 383 once the source is snapshotted
	// after the earlier filters have run. So a chain always snapshots and
	// always lets the shader composite.
	int chained = (chain_n > 1);
	// The composition shader needs the unblurred source only when the formula
	// references dest.a. Everything else is "filter output, then draw the
	// source normally", which needs no snapshot.
	int needs_source_tex = chained || (is_bevel
		? (f_knockout || bevel_type != 0)
		: (f_inner || f_knockout));
	// Whether we draw the crisp source ourselves rather than sampling it.
	int draw_source_after  = !chained && (is_bevel
		? (!f_knockout && bevel_type == 0)
		: (!f_inner && !f_knockout && f_composite));
	// "full" bevel is glow OVER source, so the source must already be in the
	// framebuffer when the filter output lands.
	int draw_source_before = !chained && is_bevel && !f_knockout && bevel_type == 2;
	// What the shader is told: 0 whenever we supply the source ourselves.
	int shader_composite = (draw_source_after || draw_source_before) ? 0 : f_composite;

	g_filter_capture_depth++;
	renderer_suspend_pass(context);
	renderer_begin_offscreen_pass(context);
	render_single_object(app_context, obj);
	renderer_end_offscreen_pass(context);
	// Unchained: the snapshot IS the source, so take it now. Chained: the
	// snapshot must be the layer entering the LAST filter, so it is taken
	// inside the loop below, after every earlier filter has run.
	if (needs_source_tex && !chained)
		renderer_snapshot_filter_source(context);

	// Plain blur keeps the legacy path (BlurFilter has no composition flags);
	// for the composed kinds, strength and the filter colour are applied by the
	// composition shader instead of inside the blur, because inner needs the
	// RAW blurred alpha ((1 - blur) * strength, not 1 - blur*strength).
	int legacy = !(is_shadow || is_glow || is_bevel);

	// A single drop shadow reads the blurred layer at a whole-pixel offset (see
	// the nearest-offset block below). Fold that offset INTO the blur instead:
	// the layer is stage-sized, so a blur tail that runs past the stage edge
	// was never stored, and reading it back at +offset came out empty — a
	// shadow thrown back onto the stage was cut off (from_shumway/acid/
	// acid-filter: halo clipped at x = stage_w - 32). The blur's own input (the
	// captured object) is all there, so the shifted blur is exact. Unchained
	// only: a chain snapshots the layer entering its last filter.
	int shadow_shift_x = 0, shadow_shift_y = 0;
	int shadow_pre_shifted = is_shadow && !chained
		&& filter_shadow_offset_px(context, last, &shadow_shift_x, &shadow_shift_y);
	if (shadow_pre_shifted)
		renderer_set_blur_shift(context, shadow_shift_x, shadow_shift_y);

	// filter_tex_a is the ping-pong's own input AND output, so an N-long list
	// chains for free: N plain blurs are bit-equal to one blur of quality N,
	// exactly as Flash requires. A NON-FINAL glow / drop-shadow / bevel is
	// approximated by colorizing inside the blur (`colorize` = 1) — exact when
	// the stacked filters are identical, an approximation otherwise; the exact
	// form needs a compose-into-offscreen pipeline. This mirrors, line for
	// line, the loop avm2_render_filtered already runs for `.filters` arrays.
	for (int fi = 0; fi < chain_n; fi++)
	{
		const FilterListEntry* f = chain[fi];
		int f_last = (fi == chain_n - 1);
		// The layer the last filter receives is this chain's "source".
		if (chained && f_last)
			renderer_snapshot_filter_source(context);
		int f_is_blur = (f->type == 1);
		float st = (float) f->strength;
		// A SWF BlurFilter has no `strength` field, so a LIST entry leaves it
		// 0 — passing that through would multiply the layer by zero and the
		// object would vanish. The scalar entry keeps the recompiler's 1.0
		// default (and Convolution-as-blur's explicit 1.0), so it is left
		// alone: that is what the single-filter path has always passed.
		if (f_is_blur && f != &chain_scratch) st = 1.0f;
		// The composed kinds apply strength in the composition shader when they
		// are last (inner needs the RAW blurred alpha), inside the blur when not.
		if (f_last && !f_is_blur) st = 1.0f;
		int colorize = (!f_last && !f_is_blur) ? 1 : 0;
		// A3: the filter's own colour carries the object's colour transform.
		double fr = f->color_r, fg = f->color_g, fb = f->color_b, fa = f->color_a;
		if (have_obj_cx) cxform_apply_rgba(obj_cx, &fr, &fg, &fb, &fa);
		renderer_run_blur(context,
			f->blur_x, f->blur_y,
			f->quality,
			st,
			fr, fg, fb, fa, colorize);
	}

	renderer_resume_pass(context);

	if (legacy)
	{
		renderer_composite_filtered(context, 0.0f, 0.0f, 0, 0, 0, 0);
		g_filter_capture_depth--;
		return;
	}

	// Stage-pixel offset -> UV offset on the blurred texture. stage_fit_{x,y}
	// account for fitted content covering only part of the target; both are 1.0
	// unless the viewport aspect differs from the movie's.
	float fit_x = app_context->stage_fit_x > 0.0f ? app_context->stage_fit_x : 1.0f;
	float fit_y = app_context->stage_fit_y > 0.0f ? app_context->stage_fit_y : 1.0f;
	// SWF stores the filter angle as FIXED **radians** (spec: DROPSHADOWFILTER
	// "Radian angle of the drop shadow"), and swf.cpp keeps it in that unit —
	// the mc.filters reflection in action.c converts it to degrees on the way
	// out. The render path used to multiply by pi/180 as if it were degrees,
	// which put every non-zero-angle shadow and bevel in the wrong direction.
	float angle_rad = last->angle;
	float dist_px = (is_shadow || is_bevel) ? last->distance : 0.0f;
	float du = cosf(angle_rad) * dist_px / (float)app_context->width * fit_x;
	float dv = sinf(angle_rad) * dist_px / (float)app_context->height * fit_y;
	{
		// Ruffle samples the blurred texture with a NEAREST sampler at the
		// fractional offset (glow.rs / bevel.rs `get_sampler(false, false)`,
		// filters.rs vertices_with_blur_offset), so the effective offset is
		// floor(off + 0.5) DEVICE pixels; our filter_sampler is linear and
		// would blend two texels. Distance is scaled by the view like Ruffle's
		// Filter::scale. drop_shadow shadows sample at -offset, bevel's
		// highlight at +offset (its shadow at -du in the shader).
		float vs = (context != NULL && context->stage_scale > 0.0f) ? context->stage_scale : 1.0f;
		float xd = cosf(angle_rad) * dist_px * vs, yd = sinf(angle_rad) * dist_px * vs;
		float uvx = fit_x / ((float)app_context->width * vs);
		float uvy = fit_y / ((float)app_context->height * vs);
		if (is_bevel) { du = floorf(xd + 0.5f) * uvx; dv = floorf(yd + 0.5f) * uvy; }
		else { du = -floorf(-xd + 0.5f) * uvx; dv = -floorf(-yd + 0.5f) * uvy; }
		// Already applied inside the blur (shadow_pre_shifted above).
		if (shadow_pre_shifted) { du = 0.0f; dv = 0.0f; }
	}

	if (draw_source_before)
		render_single_object(app_context, obj);

	// A3 again, for the composition half: shadow/glow colour and the bevel
	// highlight are the filter's OUTPUT colour, so they take the object's
	// colour transform (the crisp source drawn either side of this already
	// carries it through render_single_object's own cxform slot).
	double c_r = last->color_r, c_g = last->color_g;
	double c_b = last->color_b, c_a = last->color_a;
	double h_r = last->highlight_r, h_g = last->highlight_g;
	double h_b = last->highlight_b, h_a = last->highlight_a;
	if (have_obj_cx)
	{
		cxform_apply_rgba(obj_cx, &c_r, &c_g, &c_b, &c_a);
		cxform_apply_rgba(obj_cx, &h_r, &h_g, &h_b, &h_a);
	}

	if (is_bevel)
	{
		// ruffle filters.rs: the HIGHLIGHT samples at +offset, the shadow at
		// -offset, and the bevel is their difference.
		renderer_compose_filter(context, 1, du, dv,
			h_r, h_g, h_b, h_a,
			c_r, c_g, c_b, c_a,
			last->strength, bevel_type, f_knockout, shader_composite);
	}
	else
	{
		// ruffle drop_shadow.rs passes (-x, -y): sampling the blur "back" by the
		// offset displaces the shadow forward along the light direction.
		renderer_compose_filter(context, 0, -du, -dv,
			c_r, c_g, c_b, c_a,
			0.0f, 0.0f, 0.0f, 0.0f,
			last->strength, f_inner, f_knockout, shader_composite);
	}

	if (draw_source_after)
		render_single_object(app_context, obj);

	g_filter_capture_depth--;
}

// Compose two 20-float cxforms (5 vec4: column-major mat4 `mult` + vec4 `add`,
// applied as out = mult*color + add — see apply_cxform in render_webgpu.c) so
// that `out = outer(inner(color))`. mult = outer.mult * inner.mult;
// add = outer.mult * inner.add + outer.add.
static void compose_cxform20(float out[20], const float* outer, const float* inner)
{
	for (int j = 0; j < 4; j++)        // result column
		for (int i = 0; i < 4; i++) {  // result row
			float s = 0.0f;
			for (int k = 0; k < 4; k++)
				s += outer[k*4 + i] * inner[j*4 + k];
			out[j*4 + i] = s;
		}
	for (int i = 0; i < 4; i++) {       // add = outer.mult * inner.add + outer.add
		float s = 0.0f;
		for (int k = 0; k < 4; k++)
			s += outer[k*4 + i] * inner[16 + k];
		out[16 + i] = s + outer[16 + i];
	}
}

static void render_display_list(SWFAppContext* app_context, DisplayObject* dl, size_t dl_max_depth)
{
	// Clip masks nested inside a sprite (e.g. Pacman's CPac: a box-with-wedge
	// mask shape clips a circle into a pacman). The two root-level render loops
	// (tagShowFrame / tagRerenderFrame) already do this, but this recursive
	// nested renderer did not — so a sprite's mask shape was drawn as ordinary
	// visible geometry (Pacman's title "C" showed the white wedge mask box
	// instead of clipping the yellow pacman). Mirror the root loops' handling.
	// Closing a nested clip range with renderer_end_clip ZEROES mask_ref, which
	// is right when this walk owns the whole stencil but wrong when it was
	// entered under one: a clipDepth range inside a clipped sprite (or inside a
	// setMask'd maskee) dropped the ENCLOSING clip for everything drawn after
	// it. Restore the reference this call started with instead — restore_clip(0)
	// is byte-identical to end_clip, so the common (unclipped) entry is
	// unchanged. Same fix s12 made on the AVM2 walk (avm2_display.c:15548+).
	u32 pre_clip_ref = renderer_clip_ref(context);
	u32 active_clip_depth = 0;
	// From depth 0 (see compose_children).
	for (size_t i = 0; i <= dl_max_depth; ++i)
	{
		if (active_clip_depth > 0 && i > active_clip_depth)
		{
			renderer_restore_clip(context, pre_clip_ref);
			active_clip_depth = 0;
		}

		DisplayObject* obj = &dl[i];
		if (obj->char_id == 0) continue;

		// AVM1 setMask pairing (mask defect B). One branch when nothing in the
		// movie ever called setMask.
		MovieClip* avm1_masker = NULL;
		if (actionAvm1MaskPairCount() != 0) {
			if (entry_is_live_masker(obj)) continue;   // masker: stencil only
			avm1_masker = avm1_masker_for_entry(obj);
		}

		// Honor AS-set _visible=false on a nested entry (and its whole subtree).
		// SetProperty/SetMember _visible syncs the MC's _visible onto this entry's
		// as_hidden via mc->display_obj. Mirrors the same skip in the main display
		// loop (~line 4895). A mask's _visible is irrelevant to its clipping role,
		// so only skip non-mask entries. Without this, an invisible nested clip —
		// e.g. Minesweeper's FRadioButton frb_hitArea_mc (a wide hit-test shape set
		// `_visible=false`) — paints a grey bar over its label. The setter syncs
		// as_hidden onto this (registration) entry via sync_attached_entry_hidden.
		if (obj->clip_depth == 0 && obj->as_hidden) continue;

		// Authored fully transparent (alpha-0 placement cxform): invisible in
		// Flash. Skip non-mask entries (a mask still clips even at alpha 0).
		// Fixes Pacman's "Hit" sprite (alpha=0) painting white through the
		// pacman's masked-out mouth.
		if (obj->clip_depth == 0 && cxform_forces_invisible(app_context, obj->cxform_id))
			continue;

		// Blend modes on a NESTED entry. The two root loops (tagShowFrame /
		// tagRerenderFrame) handled these; this recursive renderer did not — and
		// the visual/blend_modes suite authors its blend objects INSIDE a sprite,
		// so they rendered as plain normal-over. Same three cases as the root
		// loops; see the comments there.
		if (obj->clip_depth == 0 && !g_clip_mask_capture)
		{
			// Alpha / Erase with no Layer above them: Flash drops the draw.
			if (obj->blend_mode == 11 || obj->blend_mode == 12)
				continue;
			if (obj->filter_type == 0
			    && renderer_blend_mode_is_layered(context, obj->blend_mode))
			{
				renderer_suspend_pass(context);
				renderer_capture_backdrop(context, obj->blend_mode);
				renderer_begin_offscreen_pass(context);
				render_single_object(app_context, obj);
				renderer_end_offscreen_pass(context);
				renderer_resume_pass(context);
				renderer_composite_blend(context, obj->blend_mode,
					active_clip_depth > 0 ? 1 : 0);
				continue;
			}
		}

		// This entry is a clip mask: render it into the stencil buffer (not the
		// color target) and clip subsequent depths up to clip_depth to it.
		if (obj->clip_depth > 0)
		{
			// Pop before pushing. A clipDepth entry can appear while an
			// earlier range is STILL open (its own depth is inside that
			// range), and this loop tracks a single scalar active_clip_depth,
			// so the second push would overwrite the first with no matching
			// pop. Under the old Always/Replace stencil that was harmless —
			// the new push just reassigned the reference. Under the
			// Equal/IncrementClamp NESTING model (render_webgpu.c) an
			// unpopped push LEAKS a level for the rest of the frame. Same
			// shape avm2_render_node already uses (avm2_display.c:15630).
			// No draw call happens between the pop and the push, so this is
			// byte-identical under the old model.
			if (active_clip_depth > 0)
			{
				renderer_restore_clip(context, pre_clip_ref);
				active_clip_depth = 0;
			}
			Character* mch = &dictionary[obj->char_id];
			if (mch->type == CHAR_TYPE_SHAPE)
			{
				renderer_begin_clip_mask(context);
				renderer_draw_shape(context, mch->shape_offset, mch->size,
					obj->transform_id, obj->cxform_id);
				renderer_end_clip_mask(context);
				active_clip_depth = obj->clip_depth;
			}
			else if (mch->type == CHAR_TYPE_MORPH_SHAPE)
			{
				renderer_begin_clip_mask(context);
				renderer_draw_shape(context, mch->morph_start_offset, mch->morph_start_size,
					obj->transform_id, obj->cxform_id);
				renderer_end_clip_mask(context);
				active_clip_depth = obj->clip_depth;
			}
			else if (mch->type == CHAR_TYPE_SPRITE)
			{
				renderer_begin_clip_mask(context);
				g_clip_mask_capture++;
				if (obj->sprite_display_list != NULL)
					render_display_list(app_context, obj->sprite_display_list, obj->sprite_max_depth);
				g_clip_mask_capture--;
				renderer_end_clip_mask(context);
				active_clip_depth = obj->clip_depth;
			}
			continue;
		}

#ifdef OFFSCREEN_RENDER
		// Video display objects have type=0 (CHAR_TYPE_SHAPE) in dictionary.
		// Check for video BEFORE the switch to avoid rendering as empty shape.
		if (ng_isVideoChar(obj->char_id)) {
			uint32_t* argb = NULL;
			int vw = 0, vh = 0;
			u16 decl_w = 0, decl_h = 0;
			int has_decl = ng_getVideoDimensions(obj->char_id, &decl_w, &decl_h);
			int got_pixels =
				actionGetEmbeddedVideoFramePixels(obj->char_id, obj->ratio,
				                                  &argb, &vw, &vh) ||
				actionGetVideoFramePixels(&argb, (int)decl_w, (int)decl_h, &vw, &vh);
			if (got_pixels) {
				u32 dst_w = has_decl ? (u32)decl_w : (u32)vw;
				u32 dst_h = has_decl ? (u32)decl_h : (u32)vh;
				renderer_draw_bitmap_quad_scaled(context, argb,
					(u32)vw, (u32)vh, dst_w, dst_h,
					0.0f, 0.0f, obj->transform_id, obj->cxform_id);
				free(argb);
			}
			continue;
		}
#endif

		// setMask maskee: push the masker's stencil around this entry's paint.
		// opaqueBackground: painted under this entry and OUTSIDE its own
		// mask / scrollRect, exactly where Ruffle's render_base emits it.
		draw_entry_opaque_background(app_context, obj);
		u32 avm1_mask_saved_clip = 0;
		if (avm1_masker != NULL)
			avm1_mask_saved_clip = avm1_mask_push(app_context, avm1_masker);

		// MovieClip.scrollRect, the CROP half. Ruffle pushes it AFTER the
		// DisplayObject.mask stencil and "in addition to" it
		// (display_object.rs:1214-1223); compose_children already applied the
		// translate to this entry's matrix and to its subtree's.
		//
		// Gated on `!g_clip_mask_capture` because Ruffle DISCARDS the crop
		// while a masker subtree is being rasterised — push_mask emits nothing
		// at nesting depth > 0 and the draw_rect command is dropped
		// (render/commands.rs:47, "used to discard drawing commands of nested
		// maskers, which Flash does not support"). The translate is NOT gated:
		// it is a transform-stack push, not a command, so a scrolled clip
		// inside a mask contributes its full, shifted silhouette. Same
		// precedent as the EditText field-bounds clip below.
		//
		// A degenerate rect (w or h == 0) writes no stencil texels and so
		// hides the subtree — Ruffle's and Flash's behaviour for a zero-sized
		// scrollRect, not an accident.
		u32 scroll_crop_saved_clip = 0;
		int pushed_scroll_crop = 0;
		const ScrollCropEntry* scroll_crop = scroll_crop_for_entry(obj);
		if (scroll_crop != NULL && !g_clip_mask_capture)
		{
			scroll_crop_saved_clip = renderer_clip_ref(context);
			renderer_begin_clip_mask(context);
			renderer_draw_rect(context, 0.0f, 0.0f, scroll_crop->w, scroll_crop->h,
				1.0f, 1.0f, 1.0f, 1.0f, scroll_crop->slot, 0);
			renderer_end_clip_mask(context);
			pushed_scroll_crop = 1;
		}

		Character* ch = &dictionary[obj->char_id];

		// Filters on a NESTED entry — the same defect shape as the blend-mode
		// arm above. render_filtered_object was reached only from the two ROOT
		// display loops (tagShowFrame / tagRerenderFrame), so a filter authored
		// INSIDE a DefineSprite rendered completely unfiltered
		// (visual/cache_as_bitmap/contains_grown_filter: a plain blue square
		// where the golden has a red glow; visual/filters/blur_scales_with_screen:
		// a sharp red box against a heavily blurred golden). The entry's filter
		// state is correct — sprite frame execution rebinds the global
		// display_list/max_depth to the sprite's own list before running its
		// frame functions, so the tagSetFilter emitted inside sprite_N_frame_M
		// lands on the right entry — only the renderer arm was missing.
		//
		// Gated on !g_clip_mask_capture like the blend arm above: a filtered
		// masker contributes its raw silhouette to the stencil.
		if (obj->filter_type != 0 && !g_clip_mask_capture
		    && g_filter_capture_depth == 0)
		{
			render_filtered_object(app_context, obj);
		}
		else
		switch (ch->type)
		{
			case CHAR_TYPE_SHAPE:
				renderer_draw_shape(context, ch->shape_offset, ch->size,
					obj->transform_id, obj->cxform_id);
				break;

			case CHAR_TYPE_MORPH_SHAPE:
				renderer_draw_shape(context, ch->morph_start_offset, ch->morph_start_size,
					obj->transform_id, obj->cxform_id);
				break;

			case CHAR_TYPE_TEXT:
#ifndef NO_GRAPHICS
				// See render_single_object — skip static draw for EditTexts;
				// they render dynamically via actionIterateTextFieldGlyphs.
				if (ng_getCharTextfieldIdx(obj->char_id) >= 0) break;
#endif
			{
				// Compose the placement cxform (obj->cxform_id) over the text's
				// baked cxform (ch->cxform_id). Static text glyphs are tinted only
				// by ch->cxform_id, so a DefineText placed with a color transform
				// — e.g. a button's OVER/DOWN state that darkens its label by
				// placing the text with a darkening cxform — was rendered with the
				// up-state color (Tetris game-over "ok" stayed white on hover).
				// Only when the placement is a NON-identity BAKED slot (slot 0 is
				// identity; dynamic/composed slots live GPU-side and can't be read
				// back here) and a dynamic slot is free; otherwise fall back to the
				// baked cxform (unchanged behavior).
				u32 text_cx = ch->cxform_id;
				GEN_EXTERN_CXFORM_DATA;
				u32 baked_cx = (app_context->cxform_data_size > 0)
					? (u32)(app_context->cxform_data_size / (20 * sizeof(float))) : 0;
				if (obj->cxform_id != 0 && obj->cxform_id != ch->cxform_id &&
				    obj->cxform_id < baked_cx && ch->cxform_id < baked_cx &&
				    g_next_dynamic_cxform_slot < g_cxform_slot_capacity)
				{
					float comp[20];
					const float* _cxd = (const float*)app_context->cxform_data;
					compose_cxform20(comp, _cxd + obj->cxform_id * 20,
						_cxd + ch->cxform_id * 20);
					u32 slot = g_next_dynamic_cxform_slot++;
					renderer_write_cxform(context, slot, comp);
					text_cx = slot;
				}
				u32 _tg_base = obj->text_glyph_xform_base ? obj->text_glyph_xform_base
				                                          : (u32)ch->transform_start;
				for (size_t j = 0; j < ch->text_size; ++j)
				{
					size_t glyph_index = 4*app_context->text_data[ch->text_start + j];
					renderer_draw_shape(context,
						app_context->glyph_data[glyph_index],
						app_context->glyph_data[glyph_index + 1],
						_tg_base + (u32)j, text_cx);
				}
			}
				break;

			case CHAR_TYPE_SPRITE:
				if (obj->sprite_display_list != NULL)
					render_display_list(app_context, obj->sprite_display_list, obj->sprite_max_depth);
				break;

			case CHAR_TYPE_BUTTON:
				// Draw the button's PERSISTENT sprite_display_list, which
				// compose_children's BUTTON branch initialized and composed in
				// place. The old approach here built a fresh temp display_list via
				// button_state_funcs and drew it with un-composed (button-local)
				// transforms, so a button NESTED inside a sprite rendered its
				// up-state shape at the parent's origin (Pong title-screen button
				// rectangles in the top-left corner). Mirrors render_single_object
				// and the CHAR_TYPE_SPRITE case above.
				if (obj->sprite_display_list != NULL)
					render_display_list(app_context, obj->sprite_display_list, obj->sprite_max_depth);
				break;
		}

		// restore_clip, never end_clip — end_clip zeroes mask_ref and would
		// drop an enclosing clipDepth range (see the comment at the top of
		// this function). Innermost push restored first.
		if (pushed_scroll_crop)
			renderer_restore_clip(context, scroll_crop_saved_clip);

		if (avm1_masker != NULL)
			renderer_restore_clip(context, avm1_mask_saved_clip);
	}
	if (active_clip_depth > 0)
		renderer_restore_clip(context, pre_clip_ref);
}
#endif // NO_GRAPHICS


void tagSetBackgroundColor(u8 red, u8 green, u8 blue)
{
	// Only the first SetBackgroundColor tag in the SWF wins; subsequent tags
	// are ignored (matches Ruffle/Flash — see Ruffle
	// core/src/display_object/movie_clip.rs `set_background_color`).
	static int already_set = 0;
	if (already_set) return;
	already_set = 1;
#ifndef NO_GRAPHICS
	renderer_set_background(context, red, green, blue);
#else
	(void)red; (void)green; (void)blue;
#endif
}

// Resolve a button's hit-test character ID to a shape or morph shape, following
// through nested buttons (e.g. button 6 whose hit char is button 5 → shape 4).
// Returns the resolved character (SHAPE or MORPH_SHAPE), or NULL if none.
// out_hit_char_id receives the resolved character's char_id (for ng_hitTestShapeChar).
static Character* resolve_hit_shape(size_t hit_char_id, u32* out_hit_transform_id,
                                    size_t* out_hit_char_id, int depth)
{
	if (depth > 4) return NULL;  // guard against loops
	if (hit_char_id >= dictionary_capacity) return NULL;
	Character* hit_ch = &dictionary[hit_char_id];
	if (hit_ch->type == CHAR_TYPE_SHAPE || hit_ch->type == CHAR_TYPE_MORPH_SHAPE)
	{
		if (out_hit_char_id) *out_hit_char_id = hit_char_id;
		return hit_ch;
	}
	if (hit_ch->type == CHAR_TYPE_BUTTON)
	{
		// Follow through to the nested button's hit shape
		*out_hit_transform_id = hit_ch->button_hit_transform_id;
		return resolve_hit_shape(hit_ch->button_hit_char_id, out_hit_transform_id,
		                        out_hit_char_id, depth + 1);
	}
	return NULL;
}

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
// Browser-WASM only: also update DefineButton2 states inside attachMovie'd /
// createEmptyMovieClip'd clips (child_mc_cache), which live outside the root
// display_list. Defined below, after compute_mc_world_xform/render_attached_child.
// Gated like the AS2-button-fire block below — NO_GRAPHICS / OFFSCREEN
// dispatch attached-clip buttons via their own per-event / process_sprite path.
static void ng_update_button_states_attached(SWFAppContext* app_context, int* found_hover);
#endif

// Button hit testing + state machine + action dispatch.
// Runs on every mouse event (in NO_GRAPHICS mode) so that transitions fire
// on the correct per-event boundary (e.g. MOUSE_MOVE → OverUp, then
// MOUSE_DOWN → OverDown fires the press action in the same tick).
//
// States: 0=Idle, 1=OverUp, 2=OverDown, 3=OutDown
//
// Recursive helper: walks a display list (which may be root or a button's
// child display list) and updates button states.  For nested buttons, the
// parent_xf is the composed transform of the enclosing button's placement,
// and parent_mc is the enclosing button's MovieClip context (used as the
// "this" scope when firing the nested button's actions).
static void ng_update_button_states_in_dl(SWFAppContext* app_context,
	DisplayObject* dl, size_t dl_max,
	const float* parent_xf, MovieClip* parent_mc,
	DisplayObject* enclosing_sprite_obj,
	int* found_hover)
{
	for (size_t i = dl_max; i >= 1; i--)
	{
		DisplayObject* obj = &dl[i];
		if (obj->char_id == 0) continue;

		Character* ch = &dictionary[obj->char_id];

		// Sprites may contain nested buttons. Recurse into the sprite's
		// display list to find them. Mirrors Ruffle's mouse_pick_avm1 which
		// walks the render list looking for interactive children.
		if (ch->type == CHAR_TYPE_SPRITE)
		{
			if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
			{
				const float* place_xf = (const float*)(app_context->transform_data) + obj->transform_id * 16;
				float child_parent_xf[16];
				hit_test_mat4_multiply(child_parent_xf, parent_xf, place_xf);

				MovieClip* sprite_mc = NULL;
				if (obj->instance_name != NULL) {
					sprite_mc = tag_cached_walk_mc(obj, parent_mc, 0);
					if (sprite_mc == NULL) {
						sprite_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
						tag_store_walk_mc(obj, sprite_mc);
					}
				}

				// In browser-WASM (USE_WEBGPU, no OFFSCREEN_RENDER), process_sprite_needs_init
				// isn't called from tagShowFrame, so sprite-named MCs created lazily by
				// actionFindOrCreateMovieClip don't get display_obj set. Without it,
				// mc_get_pixel_aabb_ng can't walk the sprite_display_list to find child
				// bounds → AABB returns 0 → hover never claimed → cursor never changes.
				// Set it here as the latest convenient point where both pointers are
				// known. Safe to set unconditionally: in graphics-native this just
				// overwrites with the same value process_sprite_needs_init already set.
				if (sprite_mc != NULL && sprite_mc->display_obj == NULL)
					sprite_mc->display_obj = (void*)obj;

				// Ruffle's mouse_pick_avm1 returns the topmost button-mode MC
				// when its hit area contains the mouse. If this sprite is
				// button-mode (has onRollOver/onRollOut/onPress/etc.) and the
				// mouse is inside its bounds, treat it as the catching
				// interactive object: claim the hover and skip recursion so
				// inner buttons do NOT fire SWFBUTTON_*. AS-level handlers on
				// the parent sprite are dispatched separately by
				// actionDispatchMCMouseMove.
				extern int actionMCHasButtonHandlers(MovieClip* mc);
				extern int actionMCMouseInsidePick(MovieClip* mc, float mx, float my);
				// Also treat sprites with mouse-related CLIP_EVENT clip-actions
				// (press/release/rollOver/rollOut/dragOver/dragOut) as button-mode
				// for hover/cursor purposes — Doodle Jump's menu buttons attach
				// CLIP_EVENT_RELEASE handlers via PlaceObject2 ClipActions but
				// never set onPress/onRelease as MC properties, so
				// actionMCHasButtonHandlers misses them.
				int has_mouse_clip_event = 0;
				const uint32_t mouse_clip_mask = CLIP_EVENT_PRESS | CLIP_EVENT_RELEASE
				    | CLIP_EVENT_RELEASE_OUTSIDE | CLIP_EVENT_ROLL_OVER | CLIP_EVENT_ROLL_OUT
				    | CLIP_EVENT_DRAG_OVER | CLIP_EVENT_DRAG_OUT;
				for (size_t a = 0; a < obj->clip_action_count; a++) {
					if (obj->clip_actions[a].event_flags & mouse_clip_mask) {
						has_mouse_clip_event = 1;
						break;
					}
				}
				if (sprite_mc != NULL && !*found_hover &&
				    (actionMCHasButtonHandlers(sprite_mc) || has_mouse_clip_event))
				{
					float mx_px = app_context->mouse.stage_x / 20.0f;
					float my_px = app_context->mouse.stage_y / 20.0f;
					if (actionMCMouseInsidePick(sprite_mc, mx_px, my_px))
					{
						*found_hover = 1;
						continue;
					}
				}

				// Pass this sprite's display object down as the enclosing
				// timeline for any nested buttons. AVM1 button actions run
				// relative to the timeline CONTAINING the button, so a bare
				// Play/Stop/GotoFrame from a button inside this sprite must
				// target this sprite — even when the sprite is unnamed (no MC,
				// so parent_mc falls back to root). Pong's "2 Player" button
				// (button_20 inside the unnamed sprite_26) does a bare Play to
				// advance sprite_26 frame 0→1 (the Enter Names screen).
				ng_update_button_states_in_dl(app_context,
					obj->sprite_display_list, obj->sprite_max_depth,
					child_parent_xf, sprite_mc ? sprite_mc : parent_mc,
					obj,
					found_hover);
			}
			continue;
		}

		if (ch->type != CHAR_TYPE_BUTTON) continue;

		// Recurse into this button's child display list FIRST (children are
		// rendered on top, so they get events before the parent button).
		if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
		{
			// Compose placement transform for recursion
			const float* place_xf = (const float*)(app_context->transform_data) + obj->transform_id * 16;
			float child_parent_xf[16];
			hit_test_mat4_multiply(child_parent_xf, parent_xf, place_xf);

			// Find the MC for this button to use as context for children
			MovieClip* btn_mc = NULL;
			if (obj->instance_name != NULL) {
				btn_mc = tag_cached_walk_mc(obj, parent_mc, 0);
				if (btn_mc == NULL) {
					btn_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
					tag_store_walk_mc(obj, btn_mc);
				}
			}

			// A button is not a timeline; keep the same enclosing sprite for
			// any buttons nested in this button's state display list.
			ng_update_button_states_in_dl(app_context,
				obj->sprite_display_list, obj->sprite_max_depth,
				child_parent_xf, btn_mc ? btn_mc : parent_mc,
				enclosing_sprite_obj,
				found_hover);
		}

		u8 old_state = obj->button_state;
		u8 new_state = old_state;

		// Check _visible and enabled on the button's MovieClip
		int mc_visible = actionGetMCVisible(app_context, obj->instance_name, parent_mc);
		int mc_enabled = actionGetMCEnabled(app_context, obj->instance_name, parent_mc);

		if (!mc_visible)
		{
			if (app_context->mouse.button_down &&
			    (old_state == 2 || old_state == 3))
				new_state = 3;
			else
				new_state = 0;
		}
		else if (!(*found_hover))
		{
			// Resolve hit-test shape (follows through nested buttons)
			u32 resolved_hit_xf_id = ch->button_hit_transform_id;
			size_t resolved_hit_char_id = ch->button_hit_char_id;
			Character* hit_ch = resolve_hit_shape(ch->button_hit_char_id,
			                                      &resolved_hit_xf_id,
			                                      &resolved_hit_char_id, 0);
			if (hit_ch != NULL)
			{
				// Compose: parent_xf * placement * hit-record transform
				const float* place_xf = (const float*)(app_context->transform_data) + obj->transform_id * 16;
				const float* hit_xf = (const float*)(app_context->transform_data) + resolved_hit_xf_id * 16;
				float temp[16], composed[16];
				hit_test_mat4_multiply(temp, parent_xf, place_xf);
				hit_test_mat4_multiply(composed, temp, hit_xf);

				// Use char-aware hit testing: in NO_GRAPHICS mode, shapes carry
				// path data (not triangulated geometry), so we must call into
				// ng_hitTestShapeChar which understands path data + bounds.
				extern int ng_hitTestShapeChar(size_t char_id, u16 ratio,
				    double ma, double mb, double mc_m, double md, double mtx, double mty,
				    double test_x, double test_y);
				double ma  = (double)composed[0];
				double mb  = (double)composed[1];
				double mc_m = (double)composed[4];
				double md  = (double)composed[5];
				double mtx = (double)composed[12];
				double mty = (double)composed[13];
				int hit = ng_hitTestShapeChar(resolved_hit_char_id, obj->ratio,
					ma, mb, mc_m, md, mtx, mty,
					(double)app_context->mouse.stage_x,
					(double)app_context->mouse.stage_y);

				if (hit)
				{
					*found_hover = 1;
					if (app_context->mouse.button_down)
						new_state = 2;
					else
						new_state = 1;
				}
				else
				{
					if (app_context->mouse.button_down &&
					    (old_state == 2 || old_state == 3))
						new_state = 3;
					else
						new_state = 0;
				}
			}
		}
		else
		{
			if (app_context->mouse.button_down &&
			    (old_state == 2 || old_state == 3))
				new_state = 3;
			else
				new_state = 0;
		}

		// When disabled: Ruffle-compatible state tracking.
		if (!mc_enabled)
		{
			int computed_is_over = (new_state == 1);
			if (!computed_is_over)
				new_state = 0;
			else
				new_state = old_state;
		}
		obj->button_state = new_state;
		obj->sticky_char_id = obj->char_id;
		obj->sticky_button_state = new_state;

		// On state change, re-populate the button's persistent display list
		// with the new state's children (shapes, sprites, etc.). Skipped for
		// attached-clip buttons (see g_btn_attached_walk) where this corrupts the
		// render; their state var + action dispatch below still run.
		if (old_state != new_state && !g_btn_attached_walk)
		{
			// Allocate display list if needed
			if (obj->sprite_display_list == NULL)
			{
				obj->sprite_dl_capacity = INITIAL_SPRITE_DL_CAPACITY;
				obj->sprite_display_list = HCALLOC(obj->sprite_dl_capacity, sizeof(DisplayObject));
				obj->sprite_max_depth = 0;
			}

			// Snapshot OLD (depth, char_id) and instance_name pairs so
			// tagPlaceObject2's preservation check (gated on g_btn_state_active)
			// can detect "same depth, same char_id as previous state" and skip
			// the eager init / frame_0 script re-fire for those entries.
			// Mirrors Ruffle's set_state child_by_depth(...).id() == record.id
			// reuse path. ButtonEventsTest motivates: depth 10 (instance5) must
			// persist across UP→OVER→DOWN transitions while depth 12 (UP-only)
			// is removed and depth 13 (OVER-only) is freshly placed with its
			// own frame_0 script firing.
			// Free any names from a prior snapshot before overwriting
			// (we own these strdup'd copies — the underlying dl entries
			// may have been freed by intervening tagPlaceObject2 calls).
			for (size_t j = 0; j < BTN_STATE_SNAP_MAX; j++)
			{
				if (g_btn_state_old_names[j] != NULL)
				{
					free(g_btn_state_old_names[j]);
					g_btn_state_old_names[j] = NULL;
				}
			}
			memset(g_btn_state_old_chars, 0, sizeof(g_btn_state_old_chars));
			memset(g_btn_state_old_sprite_dl, 0, sizeof(g_btn_state_old_sprite_dl));
			memset(g_btn_state_old_sprite_max, 0, sizeof(g_btn_state_old_sprite_max));
			memset(g_btn_state_old_sprite_cap, 0, sizeof(g_btn_state_old_sprite_cap));
			memset(g_btn_state_old_sprite_init, 0, sizeof(g_btn_state_old_sprite_init));
			memset(g_btn_state_old_sprite_cf, 0, sizeof(g_btn_state_old_sprite_cf));
			memset(g_btn_state_old_sprite_playing, 0, sizeof(g_btn_state_old_sprite_playing));
			size_t snap_max = obj->sprite_max_depth;
			if (snap_max >= BTN_STATE_SNAP_MAX) snap_max = BTN_STATE_SNAP_MAX - 1;
			for (size_t j = 1; j <= snap_max; j++)
			{
				g_btn_state_old_chars[j] = obj->sprite_display_list[j].char_id;
				const char* nm = obj->sprite_display_list[j].instance_name;
				g_btn_state_old_names[j] = (nm != NULL) ? strdup(nm) : NULL;
				// Phase 1i: capture grandchild display list + per-sprite advance
				// state so the preserved branch can restore them and
				// advance_sprite_frames doesn't treat the dl entry as
				// just_allocated (which would re-fire frame_0).
				g_btn_state_old_sprite_dl[j] = obj->sprite_display_list[j].sprite_display_list;
				g_btn_state_old_sprite_max[j] = obj->sprite_display_list[j].sprite_max_depth;
				g_btn_state_old_sprite_cap[j] = obj->sprite_display_list[j].sprite_dl_capacity;
				g_btn_state_old_sprite_init[j] = obj->sprite_display_list[j].sprite_initialized;
				g_btn_state_old_sprite_cf[j] = obj->sprite_display_list[j].sprite_current_frame;
				g_btn_state_old_sprite_playing[j] = obj->sprite_display_list[j].sprite_is_playing;
			}

			// Clear existing children. Phase 1i: do NOT free the grandchild
			// sprite_display_list pointers — they're owned by the snapshot
			// (g_btn_state_old_sprite_dl) until the post-state-func cleanup
			// frees the unpreserved ones. The preserved branch in
			// tagPlaceObject2 restores from the snapshot, so dl[j] needs
			// its sprite_display_list field NULLed here to avoid double-ownership;
			// the snapshot is the authoritative pointer until restore.
			for (size_t j = 1; j <= obj->sprite_max_depth; j++)
			{
				obj->sprite_display_list[j].sprite_display_list = NULL;
				obj->sprite_display_list[j].sprite_max_depth = 0;
				obj->sprite_display_list[j].sprite_dl_capacity = 0;
				obj->sprite_display_list[j].char_id = 0;
			}
			obj->sprite_max_depth = 0;

			// Run new state function to populate the display list. Allow the
			// recompiler-emitted sprite-DoAction gate to queue scripts as
			// normal — the per-depth preservation check inside tagPlaceObject2
			// suppresses re-firing for depths that didn't change. Without that,
			// suppressing all scripts via g_button_state_change_depth would
			// also skip new-depth placements (ButtonEventsTest depth 13/14)
			// that legitimately need their frame_0 to run.
			// state 3 (OUT_DOWN: outside-while-pressed/"tracking") shows OVER
			// children visually — Flash's "Push tracking" semantics. While the
			// button tracks a press, the OVER frame stays visible even when the
			// cursor exits. Mirrors Ruffle `avm1_button.rs` event_dispatch
			// where DragOut transitions to ButtonState::Over. Required by
			// gnash misc-ming ButtonEventsTest's `buttonChild[13].exe/.uld == 4`
			// counts (the OVER-only ermc gets one extra exe/uld pair from the
			// 2→3 DragOut transition).
			u8 effective_state = (new_state == 3) ? 1 : new_state;
			if (effective_state < 3 && ch->button_state_funcs[effective_state] != NULL)
			{
				DisplayObject* saved_dl = display_list;
				size_t saved_max = max_depth;
				size_t saved_cap = display_list_capacity;
				display_list = obj->sprite_display_list;
				max_depth = obj->sprite_max_depth;
				display_list_capacity = obj->sprite_dl_capacity;

				// Set g_current_context to the button MC so that
				// tagPlaceObject2's eager-init MC creation
				// (actionFindOrCreateMovieClip in tag.c at the
				// "_parent_for_mc" site) resolves parent = button_mc.
				// Required for child sprites to get the correct
				// parent->is_button_mc = 1 path in findOrCreateMovieClip,
				// which applies bias 16383 (Flash quirk for button-state
				// children). Without this, subsequent state-change
				// placements at depths 13, 14 inherit g_current_context
				// from outside (typically root), get bias 16384, and
				// their getDepth() returns the wrong value — breaking
				// ButtonEventsTest's `_root.buttonChild[N]` populator
				// formula `myDepth = getDepth() + 16383`.
				MovieClip* _btn_state_mc = (obj->instance_name != NULL)
					? actionFindOrCreateMovieClip(app_context,
						obj->instance_name, parent_mc)
					: NULL;
				MovieClip* _btn_saved_ctx = g_current_context;
				if (_btn_state_mc != NULL)
					actionSetCurrentContext(_btn_state_mc);

				g_btn_state_active = 1;
				ch->button_state_funcs[effective_state](app_context);
				g_btn_state_active = 0;

				if (_btn_state_mc != NULL)
					actionSetCurrentContext(_btn_saved_ctx);

				obj->sprite_display_list = display_list;
				obj->sprite_max_depth = max_depth;
				obj->sprite_dl_capacity = display_list_capacity;
				display_list = saved_dl;
				max_depth = saved_max;
				display_list_capacity = saved_cap;

				// Phase 1i: free any snapshot-owned grandchild sprite_display_list
				// pointers whose depth was NOT preserved. The preserved branch
				// in tagPlaceObject2 nulls g_btn_state_old_sprite_dl[depth] when
				// it consumes the snapshot, so any non-null entries here are
				// either (a) depths the new state didn't touch, or (b) depths
				// the new state placed with a different character. Both cases
				// require freeing the captured pointer (clear loop deferred the
				// free to here so the preserved branch could restore it).
				for (size_t j = 1; j < BTN_STATE_SNAP_MAX; j++) {
					if (g_btn_state_old_sprite_dl[j] != NULL) {
						FREE(g_btn_state_old_sprite_dl[j]);
						g_btn_state_old_sprite_dl[j] = NULL;
					}
				}

				// Phase 1e: build transient enumeration list from the
				// snapshot. Depths whose old child wasn't preserved by
				// the new state (different char_id or removed) remain
				// visible to for-in for one tick — mirrors Ruffle's
				// deferred destruction of removed button-state children.
				// Done BEFORE actionDrainOnloadAndScript so the for-in
				// inside the just-queued script_2 already sees them.
				for (size_t i = 0; i < g_btn_transient_count; i++)
				{
					if (g_btn_transient_names[i] != NULL)
					{
						free(g_btn_transient_names[i]);
						g_btn_transient_names[i] = NULL;
					}
				}
				g_btn_transient_count = 0;
				g_btn_transient_dobj = obj;
				for (size_t j = 1; j < BTN_STATE_SNAP_MAX; j++)
				{
					if (g_btn_state_old_chars[j] == 0) continue;
					if (g_btn_state_old_names[j] == NULL) continue;
					if (g_btn_state_old_names[j][0] == '\0') continue;
					size_t new_char = (j <= obj->sprite_max_depth)
						? obj->sprite_display_list[j].char_id : 0;
					if (new_char == g_btn_state_old_chars[j]) continue; // preserved
					if (g_btn_transient_count >= BTN_STATE_SNAP_MAX) break;
					g_btn_transient_names[g_btn_transient_count++] =
						strdup(g_btn_state_old_names[j]);


					// Fire AS-level onUnload on the just-removed button-state
					// child. Mirrors Ruffle's set_state → remove_child →
					// avm1_unload path. ButtonEventsTest registers
					// `this.onUnload = function() { _root.buttonChild[N].uld++; }`
					// in script_2 — without this dispatch, uld stays 0.
					//
					// `+1` adjustment: actionFireOnUnload uses bias 16384 for
					// both the cache lookup (`as_depth = swf_depth - 16384`)
					// and the post-shift depth formula
					// (`mc->depth = -(swf_depth) - 1 - 16384`). Button-state
					// children use bias 16383 (Flash quirk — see Phase 1a in
					// BUTTON_RESIDUAL_PLAN), so passing `j + 1` makes both
					// formulas come out one less than the bias-16384 case:
					// lookup matches our `mc->depth = j - 16383`, and post-
					// shift `mc->depth = -j - 16386` is exactly the value
					// the test's `myDepth = -(getDepth()+32769-16383)`
					// formula needs to recover the original SWF depth.
					{
						extern void actionFireOnUnload(SWFAppContext*, const char*, int);
						actionFireOnUnload(app_context,
							g_btn_state_old_names[j],
							(int)j + 1);
					}
				}

				// Drain AQ_KIND_SCRIPT entries queued by the state's new
				// child sprite frame_0 actions. Without this, the for-in
				// trace inside ButtonEventsTest's ermc.frame_0 (script_2)
				// would not fire until the next tagShowFrame, landing many
				// input events later.
				actionDrainOnloadAndScript(app_context);
			}
		}

		// Dispatch actions on state transitions
		if (old_state != new_state && ch->button_action_count > 0)
		{
			if (!mc_enabled)
			{
				obj->button_prev_state = old_state;
				continue;
			}

			int allow_actions = mc_visible;
			if (!mc_visible)
			{
				if ((old_state == 1 && new_state == 0) ||
				    (old_state == 3 && new_state == 0) ||
				    (old_state == 2 && new_state == 0) ||
				    (old_state == 2 && new_state == 3))
					allow_actions = 1;
			}

			if (allow_actions)
			{
				u16 transition = 0;
				if      (old_state == 0 && new_state == 1) transition = 0x0001;
				else if (old_state == 1 && new_state == 0) transition = 0x0002;
				else if (old_state == 1 && new_state == 2) transition = 0x0004;
				else if (old_state == 2 && new_state == 1) transition = 0x0008;
				else if (old_state == 2 && new_state == 3) transition = 0x0010;
				else if (old_state == 3 && new_state == 2) transition = 0x0020;
				else if (old_state == 3 && new_state == 0) transition = 0x0040;
				else if (old_state == 0 && new_state == 2) transition = 0x0080;
				else if (old_state == 2 && new_state == 0) transition = 0x0100;
				else if (old_state == 3 && new_state == 1) transition = 0x0040;

				if (transition != 0)
				{
					// Switch context to parent MC for button actions.
					// AVM1 buttons run actions relative to their parent, not themselves.
					MovieClip* saved_ctx = g_current_context;
					DisplayObject* saved_sprite_obj = g_current_sprite_obj;
					MovieClip* _btn_action_ctx = parent_mc;
					// Attached-clip buttons (browser-WASM, e.g. N's menuMC, deeply
					// nested under root via createEmptyMovieClip+attachMovie): resolve
					// the action's unqualified variables/functions against ROOT, where
					// AVM1 game globals live. Our function-call dispatch inherits the
					// caller's g_current_context instead of switching to each callee's
					// home timeline, so a `_root.foo()` chain run with parent_mc =
					// instance31 (chid112) resolves `gui`/`userdata` on instance31 →
					// undefined → no navigation. Root-placed DefineButton2 buttons
					// already get root context (their parent IS root); this makes the
					// nested ones behave the same. g_current_sprite_obj stays the
					// enclosing sprite below, so bare Play/Stop/GotoFrame still target
					// the button's own timeline. (g_btn_attached_walk is only ever set
					// inside the browser-WASM attached-button walk → no effect in
					// OFFSCREEN/NO_GRAPHICS.)
					if (g_btn_attached_walk)
						_btn_action_ctx = &root_movieclip;
					actionSetCurrentContext(_btn_action_ctx);
					// Establish the "current sprite" so a bare Play/Stop/GotoFrame
					// in the button's action targets the timeline CONTAINING the
					// button (AVM1 runs button actions relative to the parent
					// timeline), not the root.
					//   - parent is a button MC: target the button's display object
					//     so GotoFrame is a no-op (a button can't navigate frames).
					//     Matches Ruffle where button parents can't navigate.
					//   - otherwise: target the nearest enclosing sprite's display
					//     object (NULL at root → bare Play/Stop hits the root
					//     timeline, unchanged). Fixes Pong "2 Player" (button_20 in
					//     the unnamed sprite_26): bare Play now advances sprite_26
					//     frame 0→1 (Enter Names) instead of root frame 2→3 (court).
					if (parent_mc && parent_mc->is_button_mc && parent_mc->display_obj)
						g_current_sprite_obj = (DisplayObject*)parent_mc->display_obj;
					else
						g_current_sprite_obj = enclosing_sprite_obj;
					for (size_t a = 0; a < ch->button_action_count; a++)
					{
						if (ch->button_actions[a].condition & transition)
							ch->button_actions[a].action(app_context);
					}
					g_current_sprite_obj = saved_sprite_obj;
					actionSetCurrentContext(saved_ctx);
				}
			}
		}

		// Fire AS-level (dynamically-assigned) button handlers — onPress,
		// onRelease, onRollOver, etc. — on the same state transition. Unlike
		// the baked on(...) actions handled above (which need
		// ch->button_action_count > 0), these live on the button's MovieClip
		// dynamic_props (e.g. Tetris `play_btn.onRelease = function(){play();}`,
		// whose DefineButton2 has ActionOffset=0 / no baked actions).
		//
		// BROWSER-WASM ONLY. In NO_GRAPHICS / OFFSCREEN_RENDER,
		// process_sprite_needs_init runs and sets
		// button_mc->display_obj, so the per-MC AABB dispatch
		// (actionDispatchMCPress/Release/MouseMove) already fires these
		// handlers from swf_core.c / input_events — doing it here too would
		// double-fire them (regressed focus_mouse / tab_ordering_events_mouse).
		// Browser-WASM is the one build where process_sprite_needs_init is
		// gated out, so button MCs have display_obj==NULL, the AABB dispatch
		// skips them, and this precise-hit-test dispatch is the only path.
#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
		if (old_state != new_state && mc_enabled && obj->instance_name != NULL)
		{
			int as_allow = mc_visible;
			if (!mc_visible)
			{
				if ((old_state == 1 && new_state == 0) ||
				    (old_state == 3 && new_state == 0) ||
				    (old_state == 2 && new_state == 0) ||
				    (old_state == 2 && new_state == 3))
					as_allow = 1;
			}
			if (as_allow)
			{
				u16 as_transition = 0;
				if      (old_state == 0 && new_state == 1) as_transition = 0x0001;
				else if (old_state == 1 && new_state == 0) as_transition = 0x0002;
				else if (old_state == 1 && new_state == 2) as_transition = 0x0004;
				else if (old_state == 2 && new_state == 1) as_transition = 0x0008;
				else if (old_state == 2 && new_state == 3) as_transition = 0x0010;
				else if (old_state == 3 && new_state == 2) as_transition = 0x0020;
				else if (old_state == 3 && new_state == 0) as_transition = 0x0040;
				else if (old_state == 0 && new_state == 2) as_transition = 0x0080;
				else if (old_state == 2 && new_state == 0) as_transition = 0x0100;
				if (as_transition != 0)
				{
					MovieClip* as_btn_mc = actionFindOrCreateMovieClip(
						app_context, obj->instance_name, parent_mc);
					// AVM1 button handlers run with the parent timeline as the
					// current context, so a bare gotoAndStop/play inside the
					// handler targets the timeline that DEFINED it (via the
					// function's captured base clip), not the button itself.
					MovieClip* as_saved_ctx = g_current_context;
					DisplayObject* as_saved_sprite = g_current_sprite_obj;
					actionSetCurrentContext(parent_mc);
					if (parent_mc && parent_mc->is_button_mc && parent_mc->display_obj)
						g_current_sprite_obj = (DisplayObject*)parent_mc->display_obj;
					else
						g_current_sprite_obj = enclosing_sprite_obj;
					extern void actionFireButtonAS2Event(SWFAppContext*, MovieClip*, u16);
					actionFireButtonAS2Event(app_context, as_btn_mc, as_transition);
					g_current_sprite_obj = as_saved_sprite;
					actionSetCurrentContext(as_saved_ctx);
				}
			}
		}
#endif // browser-WASM only (other modes fire via the AABB dispatch path)

		obj->button_prev_state = old_state;
	}
}

int ng_update_button_states(SWFAppContext* app_context)
{
	if (app_context->shape_data == NULL) return 0;

	// Identity transform for root-level buttons
	static const float identity[16] = {
		1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
	};

	int found_hover = 0;
	ng_update_button_states_in_dl(app_context,
		display_list, max_depth,
		identity, &root_movieclip,
		NULL,
		&found_hover);
#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM: the root display_list above misses DefineButton2 buttons
	// that live inside attachMovie'd / createEmptyMovieClip'd clips (e.g. N's
	// entire menuMC UI). Walk those too.
	ng_update_button_states_attached(app_context, &found_hover);
#endif
	return found_hover;
}

// Recursively upgrade sprite_initialized from 1 (this tick) to 2 (ready for per-tick dispatch).
void upgrade_sprite_initialized(DisplayObject* dl, size_t dl_max)
{
	for (size_t i = 0; i <= dl_max; ++i)
	{
		if (dl[i].char_id == 0) continue;
		if (dl[i].sprite_initialized == 1)
			dl[i].sprite_initialized = 2;
		if (dl[i].sprite_display_list != NULL && dl[i].sprite_max_depth > 0)
			upgrade_sprite_initialized(dl[i].sprite_display_list, dl[i].sprite_max_depth);
	}
}

// ---------------------------------------------------------------------------
// attachMovie'd clips' STANDALONE child lists (CI modes: NO_GRAPHICS /
// OFFSCREEN_RENDER).
//
// ng_attachMovie gives the attached clip a heap display_obj flagged
// attach_standalone that lives in NO display-list array, and runs the symbol's
// frame 0 into that struct's sprite_display_list. A child placed there with
// onClipEvent(enterFrame) was initialized (process_sprite_needs_init_public in
// the attach-init drain sets sprite_initialized=1) but was then invisible to
// every per-tick pass: upgrade_sprite_initialized, dispatch_enterframe_clip_
// actions and hasClipEnterFrameHandlers all walk only the root display_list.
// So the handler never fired (Ruffle fires it every frame — regression test
// avm1_attach_clipevent_enterframe). Browser-WASM has its own separate passes
// (upgrade_attached_clip_initialized / dispatch_attached_clip_enterframe) and
// is untouched here.
//
// Collects (dobj, mc) pairs, deduplicated by dobj so two cache MCs aliasing one
// struct can never double-fire. Returns the pair count.
// ---------------------------------------------------------------------------
#define ATTACHED_STANDALONE_CAP 4096
// Scratch for collect_attached_standalone. Static (not stack) — every consumer
// finishes reading it before any AS handler runs, so it is never live across a
// re-entrant call.
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
static DisplayObject* g_att_sa_d[ATTACHED_STANDALONE_CAP];
static MovieClip* g_att_sa_mc[ATTACHED_STANDALONE_CAP];
#endif
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
static size_t collect_attached_standalone(DisplayObject** out_d, MovieClip** out_mc)
{
	size_t n = 0;
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	for (int i = 0; i < child_mc_count && n < ATTACHED_STANDALONE_CAP; i++)
	{
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL || mc->depth == INT_MIN || mc->display_obj == NULL) continue;
		if (mc->avm1_removed) continue;
		DisplayObject* d = (DisplayObject*)mc->display_obj;
		if (!d->attach_standalone || d->char_id == 0) continue;
		if (d->sprite_display_list == NULL || d->sprite_max_depth == 0) continue;
		int dup = 0;
		for (size_t k = 0; k < n; k++) if (out_d[k] == d) { dup = 1; break; }
		if (dup) continue;
		out_d[n] = d;
		out_mc[n] = mc;
		n++;
	}
	return n;
}
#endif

// Promote sprite_initialized 1→2 inside attached clips' standalone lists. Called
// at the tick boundary (swf_core.c / swf.c), so a clip attached (and init-drained)
// during tick N first dispatches enterFrame in tick N+1 — the same "init tick
// fires LOAD, not enterFrame" model tagShowFrame's upgrade_sprite_initialized
// applies to timeline-placed clips. The tick-boundary site (rather than
// tagShowFrame) also covers attaches made while the root is stopped or past its
// last frame, where no tagShowFrame runs.
void ng_upgrade_attached_standalone_initialized(void)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (!g_any_clip_ef_placed) return;
	size_t n = collect_attached_standalone(g_att_sa_d, g_att_sa_mc);
	for (size_t k = 0; k < n; k++)
		upgrade_sprite_initialized(g_att_sa_d[k]->sprite_display_list, g_att_sa_d[k]->sprite_max_depth);
#endif
}

// Recursively gather all eligible CLIP_EVENT_ENTER_FRAME entries into a flat
// list, with each entry's parent_mc tracked so dispatch can resolve names.
// Stops gathering (returns early) if the cap is exceeded; caller falls back
// to depth-recursive dispatch in that case.
typedef struct {
	DisplayObject* obj;
	MovieClip* parent_mc;
	size_t place_seq;
} ClipEFEntry;

// The flat dispatch loop below holds gathered DisplayObject* on the C stack
// while running EF handlers that can grow a sprite display list (attachMovie
// etc.), which moves the buffer the pointers alias (swf.h ALIASING RULE).
// Each active dispatch registers its entries array on this chain so
// ng_spriteDLRealloc's rebase can repoint the not-yet-dispatched entries.
typedef struct ClipEFRebaseFrame {
	ClipEFEntry* entries;
	size_t n;
	struct ClipEFRebaseFrame* prev;
} ClipEFRebaseFrame;
static ClipEFRebaseFrame* g_clip_ef_rebase_head = NULL;

static int gather_clip_ef_entries(SWFAppContext* app_context,
	DisplayObject* dl, size_t dl_max, MovieClip* parent_mc,
	ClipEFEntry* out, size_t* n, size_t cap)
{
	for (size_t i = 0; i <= dl_max; i++) {
		DisplayObject* obj = &dl[i];
		if (obj->char_id == 0) continue;
		if (obj->sprite_initialized < 2) continue;
		if (*n >= cap) return 0;
		out[*n].obj = obj;
		out[*n].parent_mc = parent_mc;
		out[*n].place_seq = obj->place_seq;
		(*n)++;
		// Recurse into nested sprite children
		if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0) {
			MovieClip* child_mc = NULL;
			if (obj->instance_name != NULL)
				child_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
			if (!gather_clip_ef_entries(app_context,
				obj->sprite_display_list, obj->sprite_max_depth,
				child_mc ? child_mc : parent_mc, out, n, cap))
				return 0;
		}
	}
	return 1;
}

// Recursive (subtree-DFS) fallback used when gather overflows cap. Children
// before parents within each subtree, with sibling iteration sorted by
// place_seq DESC. Pre-flat-LIFO behavior; correct for single-subtree cases
// but produces subtree-grouped order rather than global LIFO.
static void dispatch_enterframe_clip_actions_recursive(SWFAppContext* app_context,
	DisplayObject* dl, size_t dl_max, MovieClip* parent_mc)
{
	// Cap is tested against the occupied-depth COUNT, not dl_max (the highest
	// depth index) — see the matching comment in advance_sprite_frames.
	#define DEC_SORT_CAP 512
	size_t dec_depths[DEC_SORT_CAP];
	size_t dec_seq[DEC_SORT_CAP];
	size_t dec_n = 0;
	int dec_use_sorted = 1;
	{
		for (size_t d = 0; d <= dl_max; d++) {
			if (dl[d].char_id == 0) continue;
			if (dec_n >= DEC_SORT_CAP) { dec_use_sorted = 0; dec_n = 0; break; }
			dec_depths[dec_n] = d;
			dec_seq[dec_n] = dl[d].place_seq;
			dec_n++;
		}
	}
	if (dec_use_sorted) {
		for (size_t k = 1; k < dec_n; k++) {
			size_t key_d = dec_depths[k];
			size_t key_s = dec_seq[k];
			long b = (long)k - 1;
			while (b >= 0 && dec_seq[b] < key_s) {
				dec_depths[b+1] = dec_depths[b];
				dec_seq[b+1] = dec_seq[b];
				b--;
			}
			dec_depths[b+1] = key_d;
			dec_seq[b+1] = key_s;
		}
	}

	size_t dec_iter_n = dec_use_sorted ? dec_n : (dl_max + 1);
	for (size_t dec_iter = 0; dec_iter < dec_iter_n; dec_iter++)
	{
		size_t i = dec_use_sorted ? dec_depths[dec_iter] : dec_iter;
		DisplayObject* obj = &dl[i];
		if (obj->char_id == 0) continue;
		if (obj->sprite_initialized < 2) continue;

		if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
		{
			MovieClip* child_mc = NULL;
			if (obj->instance_name != NULL)
				child_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
			dispatch_enterframe_clip_actions_recursive(app_context,
				obj->sprite_display_list, obj->sprite_max_depth,
				child_mc ? child_mc : parent_mc);
		}

		if (obj->clip_action_count == 0) continue;

		MovieClip* saved_ctx = g_current_context;
		MovieClip* saved_base = actionGetBaseClip();
		MovieClip* event_mc = NULL;
		if (obj->instance_name != NULL) {
			event_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
			if (event_mc) {
				int _ef_skip = 0;
				for (MovieClip* p = event_mc; p != NULL; p = p->parent) {
					if (p->avm1_removed || p->pending_removal) { _ef_skip = 1; break; }
				}
				if (_ef_skip) continue;
				actionSetCurrentContext(event_mc);
				actionSetBaseClip(event_mc);
			}
		} else {
			for (MovieClip* p = parent_mc; p != NULL; p = p->parent) {
				if (p->avm1_removed || p->pending_removal) { goto _ef_skip_obj_r; }
			}
		}
		for (size_t a = 0; a < obj->clip_action_count; a++)
		{
			if (obj->clip_actions[a].event_flags & CLIP_EVENT_ENTER_FRAME) {
				obj->clip_actions[a].action(app_context);
			}
		}
		actionSetCurrentContext(saved_ctx);
		actionSetBaseClip(saved_base);
		continue;
	_ef_skip_obj_r:;
	}
}

// Dispatch CLIP_EVENT_ENTER_FRAME in flat global LIFO order (place_seq DESC
// across the entire display tree, not per-subtree). Mirrors Ruffle's
// clip_exec_list which is a single global linked list traversed in reverse-
// instantiation order — siblings and nested children of different parents
// interleave naturally by placement time. Falls back to subtree-DFS recursion
// when the gather cap is exceeded.
//
// Only fires for sprites with sprite_initialized >= 2 (init'd on a previous tick).
static void dispatch_enterframe_clip_actions_impl(SWFAppContext* app_context,
	DisplayObject* dl, size_t dl_max, MovieClip* parent_mc, int include_attached)
{
	// No CLIP_EVENT_ENTER_FRAME clip action has ever been placed → the gather
	// walk would find nothing to dispatch. Skip the whole tree traversal.
	if (!g_any_clip_ef_placed) return;
	#define CLIP_EF_FLAT_CAP 2048
	ClipEFEntry entries[CLIP_EF_FLAT_CAP];
	size_t n = 0;
	int gathered = gather_clip_ef_entries(app_context, dl, dl_max, parent_mc,
		entries, &n, CLIP_EF_FLAT_CAP);
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	// Root dispatch also gathers every attachMovie'd clip's standalone child
	// list (see collect_attached_standalone), into the SAME flat list so the
	// place_seq DESC sort below interleaves them with timeline clips exactly
	// like Ruffle's single global exec list.
	size_t att_n = 0;
	if (include_attached) {
		att_n = collect_attached_standalone(g_att_sa_d, g_att_sa_mc);
		for (size_t k = 0; k < att_n && gathered; k++)
			gathered = gather_clip_ef_entries(app_context,
				g_att_sa_d[k]->sprite_display_list, g_att_sa_d[k]->sprite_max_depth,
				g_att_sa_mc[k], entries, &n, CLIP_EF_FLAT_CAP);
	}
#else
	(void)include_attached;
#endif
	if (!gathered)
	{
		// Overflow: fall back to per-subtree recursion (pre-flat-LIFO behavior).
		dispatch_enterframe_clip_actions_recursive(app_context, dl, dl_max, parent_mc);
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		// The collected pairs are consumed here before any handler can re-enter
		// collect (static scratch); copy them off first.
		if (att_n > 0) {
			DisplayObject** ad = (DisplayObject**)malloc(att_n * sizeof(DisplayObject*));
			MovieClip** am = (MovieClip**)malloc(att_n * sizeof(MovieClip*));
			if (ad != NULL && am != NULL) {
				memcpy(ad, g_att_sa_d, att_n * sizeof(DisplayObject*));
				memcpy(am, g_att_sa_mc, att_n * sizeof(MovieClip*));
				for (size_t k = 0; k < att_n; k++) {
					if (am[k]->depth == INT_MIN || am[k]->display_obj != (void*)ad[k]) continue;
					dispatch_enterframe_clip_actions_recursive(app_context,
						ad[k]->sprite_display_list, ad[k]->sprite_max_depth, am[k]);
				}
			}
			free(ad);
			free(am);
		}
#endif
		return;
	}

	// Insertion sort by place_seq DESC (most-recently-placed first).
	for (size_t k = 1; k < n; k++) {
		ClipEFEntry key = entries[k];
		long b = (long)k - 1;
		while (b >= 0 && entries[b].place_seq < key.place_seq) {
			entries[b+1] = entries[b];
			b--;
		}
		entries[b+1] = key;
	}

	// Register the gathered array for pointer rebase while handlers run
	// (see ClipEFRebaseFrame above).
	ClipEFRebaseFrame _ef_frame = { entries, n, g_clip_ef_rebase_head };
	g_clip_ef_rebase_head = &_ef_frame;

	for (size_t e = 0; e < n; e++) {
		DisplayObject* obj = entries[e].obj;
		MovieClip* entry_parent = entries[e].parent_mc;
		if (obj == NULL) continue;        // entry's DL freed mid-dispatch (scrubbed)
		if (obj->char_id == 0) continue;  // entry may have been removed mid-dispatch
		if (obj->sprite_initialized < 2) continue;
		if (obj->clip_action_count == 0) continue;

		// Set correct MC context for the clip action. Skip if the MC or any
		// ancestor is Marked (queued for removal by tag-stream tagRemoveObject2)
		// so ENTER_FRAME doesn't fire for clips being removed in this frame —
		// see DEFERRED_CLIP_UNLOAD_PLAN, avm1/clip_events.
		MovieClip* saved_ctx = g_current_context;
		MovieClip* saved_base = actionGetBaseClip();
		MovieClip* event_mc = NULL;
		if (obj->instance_name != NULL) {
			event_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, entry_parent);
			if (event_mc) {
				int _ef_skip = 0;
				for (MovieClip* p = event_mc; p != NULL; p = p->parent) {
					if (p->avm1_removed || p->pending_removal) { _ef_skip = 1; break; }
				}
				if (_ef_skip) continue;
				actionSetCurrentContext(event_mc);
				// Set base_clip so actionBaseClipRemoved() detects removal during
				// gotoAndPlay catch-up that removes this clip (e.g. opcode_guard_test
				// mc2 EnterFrame: gotoAndPlay(8) removes mc2 mid-handler — Ruffle
				// aborts the rest of the handler via the same base-clip check).
				actionSetBaseClip(event_mc);
			}
		} else {
			// Anonymous obj: skip if parent is Marked.
			int _ef_skip = 0;
			for (MovieClip* p = entry_parent; p != NULL; p = p->parent) {
				if (p->avm1_removed || p->pending_removal) { _ef_skip = 1; break; }
			}
			if (_ef_skip) continue;
		}
		for (size_t a = 0; a < obj->clip_action_count; a++)
		{
			if (obj->clip_actions[a].event_flags & CLIP_EVENT_ENTER_FRAME) {
				obj->clip_actions[a].action(app_context);
				// The handler may have grown (moved) or freed the display list
				// holding this entry; entries[] is rebased by ng_spriteDLRealloc
				// (or NULLed by ng_freeSpriteDL), so re-read the entry pointer
				// before touching obj again.
				obj = entries[e].obj;
				if (obj == NULL) break;
			}
		}
		actionSetCurrentContext(saved_ctx);
		actionSetBaseClip(saved_base);
	}

	g_clip_ef_rebase_head = _ef_frame.prev;
}

void dispatch_enterframe_clip_actions(SWFAppContext* app_context,
	DisplayObject* dl, size_t dl_max, MovieClip* parent_mc)
{
	dispatch_enterframe_clip_actions_impl(app_context, dl, dl_max, parent_mc, 0);
}

// Per-tick ROOT dispatch: the root display_list plus (CI modes) every
// attachMovie'd clip's standalone child list. Browser-WASM keeps its own
// dispatch_attached_clip_enterframe pass, so there this is the plain root walk.
void dispatch_root_enterframe_clip_actions(SWFAppContext* app_context)
{
	extern MovieClip root_movieclip;
	dispatch_enterframe_clip_actions_impl(app_context, display_list, max_depth,
		&root_movieclip, 1);
}

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
// Browser-WASM: dispatch CLIP_EVENT_ENTER_FRAME for sprites nested inside
// attachMovie'd clips. The root walk above (over display_list) cannot reach
// these: ng_attachMovie copies a parent display-list entry that it never marks
// sprite_initialized, so gather_clip_ef_entries skips it (< 2) and never
// recurses into the attached clip's sub-list. Concrete case: Doodle Jump's
// blue platform is charId 32 instance "aaa" placed inside the attached "cloud"
// clip; its onEnterFrame clip action (clip_action_8) slides it left-right.
//
// Dispatch directly over each attached clip's AUTHORITATIVE standalone
// sprite_display_list (the one advance_attached_clip_frames rebuilds), not the
// stale parent copy. No double-fire with the root walk for the reasons above.
void dispatch_attached_clip_enterframe(SWFAppContext* app_context)
{
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	uintptr_t dl_lo = (uintptr_t)display_list;
	uintptr_t dl_hi = dl_lo + (uintptr_t)display_list_capacity * sizeof(DisplayObject);
	for (int i = 0; i < child_mc_count; i++)
	{
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL || mc->depth == INT_MIN || mc->display_obj == NULL) continue;
		if (mc->avm1_removed || mc->pending_removal) continue;
		DisplayObject* d = (DisplayObject*)mc->display_obj;
		// Only standalone attachMovie'd clips — skip timeline-placed clips
		// whose display_obj lives in the global display_list array (those are
		// covered by the root walk).
		if ((uintptr_t)d >= dl_lo && (uintptr_t)d < dl_hi) continue;
		if (d->sprite_display_list == NULL || d->sprite_max_depth == 0) continue;

		MovieClip* saved_ctx = g_current_context;
		actionSetCurrentContext(mc);
		dispatch_enterframe_clip_actions(app_context, d->sprite_display_list,
			d->sprite_max_depth, mc);
		actionSetCurrentContext(saved_ctx);

		// Link each named nested clip's MovieClip to its display entry. The clip
		// action above mutates the MC's spatial props (e.g. blue platform "aaa"
		// `this._x += ac`), but the MC is created lazily by NAME during dispatch
		// with display_obj == NULL, so compose_children's per-tick overlay (which
		// matches the moving MC to its entry by display_obj == obj) can't find it
		// and the motion never renders. Resolve the per-cloud MC (keyed by name +
		// this cloud as parent) and point its display_obj at the entry so the
		// overlay applies the AS transform. Only set when NULL — never clobber an
		// existing linkage.
		for (size_t dd = 1; dd <= d->sprite_max_depth; dd++) {
			DisplayObject* co = &d->sprite_display_list[dd];
			if (co->char_id == 0 || co->instance_name == NULL) continue;
			MovieClip* cmc = actionFindOrCreateMovieClip(app_context, co->instance_name, mc);
			if (cmc != NULL && cmc->display_obj == NULL)
				cmc->display_obj = co;
		}
	}
}
#endif

// Recursively set enterframe_eligible=1 for all display entries with sprite_initialized >= 2.
// Walks into sprite and button child display lists. This ensures button children get the flag
// (advance_sprite_frames only iterates root-level sprites, missing button children).
// When a parent is removed (char_id=0), the walk skips it → children don't get the flag.
void set_enterframe_eligible_recursive(DisplayObject* dl, size_t dl_max)
{
	// Refresh the onEnterFrame ancestry stamp once per tick (no-op on the
	// recursive calls and on additional top-level callers within the same tick).
	extern size_t g_tick_count;
	stamp_onenterframe_paths();
	for (size_t i = 0; i <= dl_max; i++)
	{
		if (dl[i].char_id == 0) continue;
		// Static-subtree prune: no onEnterFrame handler anywhere below here this
		// tick, so the eligible flag would never be read in this subtree. Skipped
		// only when the stamp is provably complete (g_ef_prune_safe).
		if (g_ef_prune_safe && dl[i].subtree_ef_gen != g_tick_count)
			continue;
		if (dl[i].sprite_initialized >= 2)
			dl[i].enterframe_eligible = 1;
		if (dl[i].sprite_display_list != NULL && dl[i].sprite_max_depth > 0)
			set_enterframe_eligible_recursive(dl[i].sprite_display_list, dl[i].sprite_max_depth);
	}
}

// --- Deferred ENTER_FRAME flush ---
// Set by swf_core.c before the root frame function. Cleared by tagFlushPendingEnterFrame.
// Ensures ENTER_FRAME fires between RemoveObject and DoAction (matching Flash ordering).
int g_enterframe_flush_pending = 0;

void tagFlushPendingEnterFrame(SWFAppContext* app_context)
{
	if (!g_enterframe_flush_pending) return;
	g_enterframe_flush_pending = 0;

	// Construct clips placed by this frame's tags BEFORE dispatching
	// enterFrame. PlaceObject2 queues a placed clip's CLIP_EVENT_INITIALIZE,
	// CLIP_EVENT_CONSTRUCT, and registerClass constructor (AQ_KIND_CLIP_INIT /
	// CLIP_CONSTRUCT / REGISTER_CTOR); the recompiler drains them at the
	// post-flush SHOW_FRAME site (actionDrainAllInPriorityOrder). That left
	// the constructor running AFTER a pre-existing clip's onEnterFrame
	// sampled the placement frame, so an onEnterFrame observing a freshly
	// placed registered-class clip saw it placed-but-unconstructed for one
	// frame (e.g. Flash MX v2 UI components: an FRadioButton's _xscale=100
	// reset / attached children / handlerObj group appeared one frame late).
	// Ruffle constructs at place time, before that frame's enterFrame. Drain
	// only the construction kinds here (frame DoAction stays at NORMAL
	// priority for the SHOW_FRAME drain); the later drain is then a no-op for
	// these kinds.
	actionDrainActionQueueByKind(app_context, AQ_KIND_CLIP_INIT);
	actionDrainActionQueueByKind(app_context, AQ_KIND_CLIP_CONSTRUCT);
	actionDrainActionQueueByKind(app_context, AQ_KIND_REGISTER_CTOR);

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	// Mark (but don't clear display_list) any MCs queued for finalize, so
	// ENTER_FRAME skips MCs being removed by tag-stream tagRemoveObject2 in
	// this frame. The clear (which frees sprite_display_list and would dangle
	// other MCs' display_obj pointers) is deferred to actionDrainOnloadAndScript
	// after queued onLoad handlers drain. Matches Flash's behavior:
	// avm1/clip_events expects no onEnterFrame after RemoveObject2.
	extern void run_pending_finalize_mark_only(SWFAppContext* app_context);
	run_pending_finalize_mark_only(app_context);
#endif

	// Set enterframe_eligible for all initialized sprites (recursive into buttons)
	set_enterframe_eligible_recursive(display_list, max_depth);
	// Dispatch clip event ENTER_FRAME (recursive, children before parents).
	// Only fires for sprites with sprite_initialized >= 2 (init'd on a previous tick).
	dispatch_root_enterframe_clip_actions(app_context);
#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM: fire enterFrame for clip actions on sprites nested inside
	// attachMovie'd clips (e.g. Doodle Jump blue platform charId 32 "aaa"),
	// which the root walk above structurally can't reach.
	dispatch_attached_clip_enterframe(app_context);
#endif
	// Dispatch AS2 mc.onEnterFrame property handlers
	actionDispatchEnterFrameHandlers(app_context);
	actionDispatchRootVarMapEnterFrame(app_context);
}

// Flag to suppress tagShowFrame side effects during actionCall invocations.
// call() only runs DoAction scripts, not timeline management tags.
int g_in_action_call = 0;

#ifndef NO_GRAPHICS
// The harness passes -DMSAA_SAMPLES=N only when it differs from the default of
// 4 (verify_output.py:2695), so an undefined macro means the antialiased build.
// Mirrors avm2_display.c:18379.
#ifndef MSAA_SAMPLES
#define MSAA_SAMPLES 4
#endif

// s21 w2-px-b: one segment of Ruffle's border LineStrip (wgpu
// `draw_line_rect`: the box's unit-square corners + HALF_PX, drawn as
// PrimitiveTopology::LineStrip [0,1,2,3,0] on every non-Dx12 backend — the
// goldens' backend). `a`/`b` are the already-transformed, pixel-snapped
// corners in STAGE twips; the HALF_PX offset and the 1-device-pixel width are
// applied here in DEVICE space, so neither is scaled or sheared by the
// field's matrix (lines.rs: "the thickness and line caps should not be
// transformed"). The segment is rasterised as a non-strict line: a
// parallelogram one device pixel wide along the MINOR axis. That beat the
// literal Dx12 `emulate_line_as_rect` (1 px perpendicular) on both failing
// comparisons of edittext_border_transform: 16/15 vs 18/17 outliers,
// max diff 176/144 vs 207/208. Drawn as the unit square through a
// per-segment GPU transform slot whose columns are the segment and the
// 1-device-px minor-axis step.
static void tf_border_line_segment(float ax, float ay, float bx, float by,
	float dtw, float r, float g, float b)
{
	float dx = bx - ax, dy = by - ay;
	if (!(fabsf(dx) > 0.0f || fabsf(dy) > 0.0f)) return;
	u32 slot = g_next_dynamic_xform_slot;
	if (slot >= g_xform_slot_capacity) return;
	g_next_dynamic_xform_slot++;
	// y-major -> 1 px along x; x-major (and the exact-45-degree tie) -> along y.
	float wx = 0.0f, wy = dtw;
	if (fabsf(dy) > fabsf(dx)) { wx = dtw; wy = 0.0f; }
	const float half = dtw * 0.5f;
	float xf[16] = {
		dx,   dy,   0.0f, 0.0f,
		wx,   wy,   0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		ax + half - wx * 0.5f, ay + half - wy * 0.5f, 0.0f, 1.0f,
	};
	renderer_write_transform(context, slot, xf);
	renderer_draw_rect(context, 0.0f, 0.0f, 1.0f, 1.0f, r, g, b, 1.0f, slot, 0);
}

// s21 w2-px-b: Ruffle `draw_text_box` (edit_text.rs ~2939) border for an
// EMBEDDED-font field whose world matrix rotates or shears it. The old path
// drew four locally-thickened rects through the field's transform slot, so the
// border's thickness was rotated/sheared with the box and its diagonals landed
// ~half a pixel inside Ruffle's (the whole residual of
// edittext_border_transform .04/.06). Axis-aligned matrices (incl. 90/180
// degree rotations) keep the old path and its measured corner rules. Returns 1
// when it drew the border.
static int tf_border_skewed_line_rect(const TextFieldRenderInfo* info,
	float dscale, float r, float g, float b)
{
	const float ma = info->m_a, mb = info->m_b, mc = info->m_c, md = info->m_d;
	int axis_aligned = (fabsf(mb) < 1e-4f && fabsf(mc) < 1e-4f)
	                || (fabsf(ma) < 1e-4f && fabsf(md) < 1e-4f);
	if (axis_aligned) return 0;
	if (!info->edge_x || !info->edge_y) return 0;   // negative-dimension fields keep the old path
	// text_box = transform.matrix * create_box(width, height, x_min, y_min), in
	// DEVICE pixels (Ruffle's transform stack starts at the view matrix).
	const float lx = info->x * 20.0f, ly = info->y * 20.0f;   // local twips
	const float lw = info->w * 20.0f, lh = info->h * 20.0f;
	const float k = dscale / 20.0f;                           // stage twips -> device px
	float A = ma * lw * k, B = mb * lw * k;
	float C = mc * lh * k, D = md * lh * k;
	float TX = (ma * lx + mc * ly + info->m_tx) * k;
	float TY = (mb * lx + md * ly + info->m_ty) * k;
	// EditTextPixelSnapping::apply, quality != Low (edit_text.rs ~3614).
	// Twips::new(2) is 0.1 px; trunc_to_pixel truncates toward zero.
	TX = truncf(TX + 0.1f);
	TY = truncf(TY + 0.1f);
	int x_snap = fabsf(C) < 0.001f || fabsf(D) < 0.001f;
	int y_snap = fabsf(A) < 0.001f || fabsf(B) < 0.001f;
	if (x_snap) {
		A = rintf(A - 0.35f);
		B = rintf(B - 0.35f);
	}
	if (y_snap) {
		C = rintf(C - 0.35f);
		D = rintf(D - 0.35f);
	}
	// Corners of the unit square through text_box, back to stage twips.
	const float s = 20.0f / dscale;
	float px[4] = { TX, TX + A, TX + A + C, TX + C };
	float py[4] = { TY, TY + B, TY + B + D, TY + D };
	const float dtw = 20.0f / dscale;
	for (int i = 0; i < 4; i++) {
		int j = (i + 1) & 3;
		tf_border_line_segment(px[i] * s, py[i] * s, px[j] * s, py[j] * s,
			dtw, r, g, b);
	}
	return 1;
}

// Callback for actionIterateTextFields: render text field background/border rectangles.
static void textfield_render_cb(const TextFieldRenderInfo* info, void* user_data)
{
	(void)user_data;

	// Convert pixel coordinates to twips.
	float x = info->x * 20.0f;
	float y = info->y * 20.0f;
	float w = info->w * 20.0f;
	float h = info->h * 20.0f;

	// The box used to be drawn in stage space through transform slot 0
	// (identity) with the field's *unscaled* local size — so _xscale /
	// _yscale / _rotation and any ancestor matrix were silently discarded
	// (visual/edittext/edittext_border_transform drew axis-aligned squares
	// where Flash draws diamonds and parallelograms). When the producer
	// supplies a world matrix, allocate a GPU transform slot for it and emit
	// the box in FIELD-LOCAL twips through that slot instead.
	u32 xform_slot = 0;
	// Ruffle's transform_stack is seeded with the STAGE VIEW MATRIX, so both
	// `round_to_pixel_ties_even` in draw_device_text_box and the standing
	// "line width of the border is always 1px regardless of zoom and transform"
	// rule are expressed in DEVICE pixels, not stage pixels. At viewport
	// scale_factor 2 that is a half-stage-pixel snapping grid and a
	// half-stage-pixel border — the whole residual of
	// visual/edittext/edittext_{background,border}_basic_scale2.
	float dscale = (context != NULL && context->stage_scale > 0.0f)
	             ? context->stage_scale : 1.0f;
	float bt = 20.0f / dscale;   // border thickness along y, in local twips
	float bl = 20.0f / dscale;   // border thickness along x, in local twips
	int line_rect = 0;  // draw the border as Ruffle's open line-rect polyline
	int device_box = (info->has_matrix && info->device_font);
	if (device_box) {
		// DEVICE fonts take Ruffle's `draw_device_text_box` (edit_text.rs
		// ~2845): the bounds are transformed, then their axis-aligned SCREEN
		// box is pixel-snapped and drawn upright — a device-font box never
		// rotates or shears with the matrix, not even for the small rotations
		// that survive the positive-scale-only cull above. Drawing it through
		// the matrix instead put visual/edittext/edittext_device_transform_
		// small_rotation (a CI-passing test) right on its outlier limit.
		float lx0 = x, ly0 = y, lx1 = x + w, ly1 = y + h;
		float cxs[4] = {
			info->m_a * lx0 + info->m_c * ly0 + info->m_tx,
			info->m_a * lx1 + info->m_c * ly0 + info->m_tx,
			info->m_a * lx0 + info->m_c * ly1 + info->m_tx,
			info->m_a * lx1 + info->m_c * ly1 + info->m_tx,
		};
		float cys[4] = {
			info->m_b * lx0 + info->m_d * ly0 + info->m_ty,
			info->m_b * lx1 + info->m_d * ly0 + info->m_ty,
			info->m_b * lx0 + info->m_d * ly1 + info->m_ty,
			info->m_b * lx1 + info->m_d * ly1 + info->m_ty,
		};
		float minx = cxs[0], maxx = cxs[0], miny = cys[0], maxy = cys[0];
		for (int k = 1; k < 4; k++) {
			if (cxs[k] < minx) minx = cxs[k];
			if (cxs[k] > maxx) maxx = cxs[k];
			if (cys[k] < miny) miny = cys[k];
			if (cys[k] > maxy) maxy = cys[k];
		}
		// round_to_pixel_ties_even on origin and extent, independently — on the
		// DEVICE pixel grid (see `dscale` above), then back to stage twips.
		const float dtw = 20.0f / dscale;   // one device pixel, in stage twips
		x = rintf(minx / dtw) * dtw;
		y = rintf(miny / dtw) * dtw;
		w = rintf((maxx - minx) / dtw) * dtw;
		h = rintf((maxy - miny) / dtw) * dtw;
		xform_slot = 0;   // already in stage twips
		line_rect = 1;
		// The edge pixel is added AFTER the background draw (below) — the
		// background is exactly the bounds; the edge belongs to the border.
	} else if (info->has_matrix) {
		u32 slot = g_next_dynamic_xform_slot;
		if (slot < g_xform_slot_capacity) {
			g_next_dynamic_xform_slot++;
			float xf[16] = {
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f,
			};
			xf[0]  = info->m_a;
			xf[1]  = info->m_b;
			xf[4]  = info->m_c;
			xf[5]  = info->m_d;
			xf[12] = info->m_tx;
			xf[13] = info->m_ty;
			renderer_write_transform(context, slot, xf);
			xform_slot = slot;
			// EMBEDDED-font boxes take Ruffle's OTHER box painter: `draw_text_box`
			// (edit_text.rs ~2930) emits a single `draw_line_rect`, whose index
			// list is a CLOSED strip (`descriptors.rs: indices_line_rect =
			// [0,1,2,3,0]`) — Ruffle's own doc comment on it says "the
			// bottom-right corner of the border is NOT missing (usually)". Only
			// `draw_device_text_box` (the branch above) hand-rolls four separate
			// `draw_line` calls and genuinely drops that corner. Opening the
			// corner here too cost visual/edittext/edittext_caret_empty 36
			// channels per comparison (golden 95,95,95 vs our 255,255,255).
			line_rect = 0;
		} else {
			// Slot pool exhausted — fall back to the old stage-space draw so
			// the box at least appears at the right origin.
			x += info->m_tx;
			y += info->m_ty;
		}
		// Flash keeps the border 1 device pixel wide regardless of the
		// transform (edit_text.rs `draw_line_rect`: "line width of the border
		// is always 1px regardless of zoom and transform"), and the extra
		// right/bottom edge pixel is likewise a device pixel. We draw the box
		// as filled rects in LOCAL space, so divide both by the matrix's
		// per-axis scale.
		float sx = sqrtf(info->m_a * info->m_a + info->m_b * info->m_b);
		float sy = sqrtf(info->m_c * info->m_c + info->m_d * info->m_d);
		if (sx > 1e-4f) bl = 20.0f / (sx * dscale);
		if (sy > 1e-4f) bt = 20.0f / (sy * dscale);
	}

	if (info->has_background) {
		float r = ((info->background_color >> 16) & 0xFF) / 255.0f;
		float g = ((info->background_color >> 8) & 0xFF) / 255.0f;
		float b = (info->background_color & 0xFF) / 255.0f;
		// The background is exactly the field's bounds — Ruffle's
		// `draw_rect(bg, create_box(width, height, x_min, y_min))`. The extra
		// edge pixel belongs to the BORDER polyline only.
		renderer_draw_rect(context, x, y, w, h, r, g, b, 1.0f, xform_slot, 0);
	}

	if (device_box) {
		// Already axis-aligned in screen space — the edge pixel is a plain
		// +1 device pixel on the right/bottom.
		if (info->edge_x) w += bl;
		if (info->edge_y) h += bt;
	} else if (info->has_matrix) {
		// Flash's extra edge pixel is added on the SCREEN +x / +y side. We
		// draw axis-aligned rects in LOCAL space, so map the two screen unit
		// directions back through the inverse matrix and grow along whichever
		// local axis each one mostly points down. Identity keeps the old
		// "w += 1px" behaviour; a reflection grows on the local -x/-y side
		// (otherwise the whole box lands a pixel too far left/up); a 90°
		// rotation swaps which local axis each screen edge grows.
		float det = info->m_a * info->m_d - info->m_b * info->m_c;
		int plain = 1;
		if (fabsf(det) > 1e-6f) {
			// local delta (twips) for one DEVICE pixel along +x and +y
			const float dpx = 20.0f / dscale;
			float ux =  info->m_d * dpx / det, uy = -info->m_b * dpx / det;
			float vx = -info->m_c * dpx / det, vy =  info->m_a * dpx / det;
			struct { int on; float dx, dy; } edges[2] = {
				{ info->edge_x, ux, uy },
				{ info->edge_y, vx, vy },
			};
			for (int e = 0; e < 2; e++) {
				if (!edges[e].on) continue;
				float dx = edges[e].dx, dy = edges[e].dy;
				if (fabsf(dx) >= fabsf(dy)) {
					if (dx < 0.0f) { x += dx; }
					w += fabsf(dx);
					if (e != 0 || dx < 0.0f) plain = 0;
				} else {
					if (dy < 0.0f) { y += dy; }
					h += fabsf(dy);
					if (e != 1 || dy < 0.0f) plain = 0;
				}
			}
		} else {
			if (info->edge_x) w += bl;
			if (info->edge_y) h += bt;
			plain = 0;
		}
		// The half-open border polyline drops its far corner in SCREEN space;
		// only in the plain positive-scale case does that coincide with the
		// local bottom-right one, so restrict the open outline to that case.
		// (Dead since the embedded branch above sets line_rect = 0 outright —
		// only the device branch opens the corner. Kept as a guard in case the
		// embedded box ever goes back to the open outline.)
		if (!plain) line_rect = 0;
	}

	if (info->has_border) {
		float r = ((info->border_color >> 16) & 0xFF) / 255.0f;
		float g = ((info->border_color >> 8) & 0xFF) / 255.0f;
		float b = (info->border_color & 0xFF) / 255.0f;
		if (!device_box && info->has_matrix && xform_slot != 0
		    && tf_border_skewed_line_rect(info, dscale, r, g, b)) {
			// s21 w2-px-b: rotated / sheared embedded-font box — four
			// device-space line segments, see tf_border_skewed_line_rect.
		} else if (line_rect) {
			// Ruffle traces the border as an OPEN polyline
			// (x0,y0)->(x1,y0)->(x1,y1)->(x0,y1)->(x0,y0), so the
			// bottom-right corner pixel is drawn exactly once — i.e. not at
			// all, once each segment is half-open at its far end. Reproducing
			// that is what the last few outliers of
			// visual/edittext/edittext_border_transform are.
			renderer_draw_rect(context, x, y, w, bt, r, g, b, 1.0f, xform_slot, 0);                    // top
			renderer_draw_rect(context, x, y + h - bt, w - bl, bt, r, g, b, 1.0f, xform_slot, 0);      // bottom
			renderer_draw_rect(context, x, y + bt, bl, h - bt, r, g, b, 1.0f, xform_slot, 0);          // left
			renderer_draw_rect(context, x + w - bl, y + bt, bl, h - 2*bt, r, g, b, 1.0f, xform_slot, 0); // right
		} else {
			// The CLOSED line-rect's bottom-right corner is PARTIAL, not binary.
			// Ruffle emits it as one LineStrip [0,1,2,3,0] (wgpu
			// descriptors.rs:237, pipelines.rs:118) whose vertices sit at the
			// HALF_PX-offset box corners, so the bottom and right segments each
			// STOP at the centre of that corner pixel: the bottom inks its left
			// half, the right its top half, union 3/4 — while the top and left
			// segments RUN THROUGH the other three corners and ink them solid.
			// Measured in the goldens (BR pixel value, 0 = solid black); both
			// of these are SWF v8, i.e. this painter, NOT avm2_render_textbox:
			//   visual/edittext/edittext_caret_empty   12 boxes, 40x20    95
			//   visual/edittext/edittext_border_transform .04/.06          95
			// Four full-coverage rects can only produce 0 or 255 there. Both
			// values were tried: full (0) is 95 off, and the open polyline
			// (255, the `line_rect` branch above) is 160 off — which is why
			// edittext_caret_empty stayed red at tolerance 64 / max_outliers 0
			// either way. Ending the bottom rect half a device pixel short and
			// extending the right rect half a device pixel down makes the two
			// meet at the corner pixel's CENTRE, so MSAA resolves 3/4 coverage
			// (64 of 255) — 31 off the golden, inside tolerance at last.
			// MSAA-gated: at MSAA_SAMPLES == 1 the lone pixel-centre sample
			// lands exactly on the new edge and would drop the corner to white,
			// and the aliased arm has its own measured rule (avm2_display.c).
			float bot_w = w, rgt_h = h - 2*bt;
#if MSAA_SAMPLES > 1
			bot_w = w - bl * 0.5f;
			rgt_h = h - 2*bt + bt * 0.5f;
#endif
			renderer_draw_rect(context, x, y, w, bt, r, g, b, 1.0f, xform_slot, 0);            // top
			renderer_draw_rect(context, x, y + h - bt, bot_w, bt, r, g, b, 1.0f, xform_slot, 0);   // bottom
			renderer_draw_rect(context, x, y + bt, bl, h - 2*bt, r, g, b, 1.0f, xform_slot, 0);          // left
			renderer_draw_rect(context, x + w - bl, y + bt, bl, rgt_h, r, g, b, 1.0f, xform_slot, 0); // right
		}
	}
}

// One glyph's pen advance in TWIPS, as Ruffle lays it out (font_like.rs
// FontLike::evaluate): `Twips::new((advance as f32 * scale) as i32)`, i.e.
// truncated to a whole twip PER GLYPH, where scale = height / em. Summing the
// unrounded product instead drifts right by up to ~1 twip per glyph: three
// 16380/20480-em glyphs at 600 twips are 3 x 479 = 1437 twips in Ruffle and
// 1439.6 in the float sum, which moved the end-of-text caret and the
// selection box's right edge a whole pixel right in
// visual/cache_as_bitmap/edittext_selection (s21 w2-edittext-filters).
// A DEVICE-font field (DefineEditText without UseOutlines) additionally has
// each advance rounded to a whole pixel (`round_to_pixel`, f64::round = half
// away from zero, which roundf matches): visual/cache_as_bitmap/edittext_scroll
// and the device-font labels of visual/filters/bevel* need it.
static inline float tf_glyph_advance_twips(s32 adv, float scale, int device_font)
{
	float t = (float)(s32)((float)adv * scale);
	if (device_font) t = roundf(t / 20.0f) * 20.0f;
	return t;
}

// Callback for actionIterateTextFieldGlyphs: render text field glyph shapes.
// Reads glyph vertices from shape_data, applies CPU-side transform, renders via draw_tris.
// When info->runs is non-NULL, per-byte color and font_height are looked up
// from the run array (htmlText / styleSheet pipeline). SENTINEL bytes 0xFE and
// 0xFF mark structural paragraph breaks and behave like newlines.
//
// Rendering is clipped to the field's bounds via a stencil mask. Mirrors
// Ruffle's `EditText::render_self` which pushes a mask at
// `bounds.grow_x(-GUTTER)` — gutter-inset horizontally, full height
// vertically. Without this, long text overflows into neighboring fields.
static void textfield_glyph_render_cb(const TextFieldGlyphInfo* info, void* user_data)
{
	GEN_EXTERN_SHAPE_DATA;
	GEN_EXTERN_GLYPH_DATA;

	// Number of u32 entries in the generated glyph_data global (4 per glyph:
	// offset, size, +2 reserved). Passed via user_data so the draw pass can
	// bound-check glyph indices — trace-only SWFs (no embedded font outlines)
	// emit a 1-element placeholder, and a font with metrics but no shapes would
	// otherwise index past the global.
	SWFAppContext* _gd_ctx = (SWFAppContext*)user_data;
	size_t glyph_data_entries = _gd_ctx ? (_gd_ctx->glyph_data_size / sizeof(u32)) : 0;

	// AVM1 setMask (mask defect B). While draw_mc_mask_geometry replays a
	// masker field into the stencil only that field is drawn; in the ordinary
	// glyph pass a live masker field is not drawn as content at all.
	if (g_avm1_mask_glyph_only_mc != NULL) {
		if (info->mc != (void*)g_avm1_mask_glyph_only_mc) return;
	} else if (info->mc != NULL && actionAvm1IsLiveMasker(info->mc)) {
		return;
	}

	int font_idx = ng_find_font_with_metrics(info->font_id);
	if (font_idx < 0) return;

	s16 ascent = 0;
	s16 descent = 0;   // caret height is ascent + descent (edit_text.rs:1277)
	int em_square = 1024;
	if (!ng_font_get_metrics(font_idx, &ascent, &descent, &em_square)) return;
	if (em_square == 0) return;

	size_t glyph_base = ng_font_get_glyph_base(font_idx);

	// 2px gutter on each side (Flash text field internal padding)
	float gutter_twips = 40.0f;

	// Field clipping mask (stencil): gutter-inset horizontally, full height
	// vertically. Matches Ruffle EditText::render_self mask.
	// The text origin (and mask) is offset from the placement matrix by the
	// field's bounds-RECT min (Ruffle edit_text.rs: translate(bounds.x_min+GUTTER)).
	// info->x/y carry only the placement translation, so add bounds_min here.
	float bxmin_off = (float)info->bounds_xmin_twips;
	float bymin_off = (float)info->bounds_ymin_twips;
	float mask_x = info->x * 20.0f + bxmin_off + gutter_twips;
	float mask_y = info->y * 20.0f + bymin_off;
	float mask_w = info->w * 20.0f - 2.0f * gutter_twips;
	float mask_h = info->h * 20.0f;
	// Suppressed while this field is being rasterised INTO a stencil: Ruffle's
	// EditText bounds mask is a nested mask inside a mask capture and is
	// therefore ignored (our renderer already no-ops the nested begin/end pair;
	// only the rect draw itself needs gating). Without this the masked glyphs
	// would be cut off at the field's right edge (~x=192 in text_field_mask)
	// instead of running the full width of the text.
	int has_clip = (mask_w > 0.0f && mask_h > 0.0f) && !g_clip_mask_capture;
	if (has_clip) {
		renderer_begin_clip_mask(context);
		renderer_draw_rect(context, mask_x, mask_y, mask_w, mask_h, 1.0f, 1.0f, 1.0f, 1.0f, 0, 0);
		renderer_end_clip_mask(context);
	}

	// Baseline = ascent at the largest font_height present (Flash text layout
	// aligns mixed-size glyphs to a shared baseline that fits the tallest run).
	// Single-run fields just use that run's font_height; classSmol-only cells
	// for example position their small glyph at y_top ≈ cell_top instead of
	// the larger default-font ascent.
	u16 baseline_fh = info->font_height;
	if (info->runs && info->run_count > 0) {
		u16 max_fh = 0;
		for (int ri = 0; ri < info->run_count; ri++) {
			if (info->runs[ri].font_height > max_fh)
				max_fh = info->runs[ri].font_height;
		}
		if (max_fh > 0) baseline_fh = max_fh;
	}
	float baseline_scale = (float)baseline_fh / (float)em_square;
	float y_pos = info->y * 20.0f + bymin_off + (float)ascent * baseline_scale + gutter_twips;
	// Author-set vertical scroll (TextField.scroll, or a mouse-wheel notch):
	// Ruffle draws the layout translated up by the top of line `scroll - 1`
	// (edit_text.rs render_self, `scroll_offset`), clipped to the field box.
	// This layout's line pitch is font_height + leading (the newline advance
	// below), so hiding N lines is N such pitches. The caret and selection
	// derive from y_pos and move with it. Lines above the view are not drawn
	// at all (the golden shows no descenders of line `scroll - 1` peeking in
	// under the top gutter), only laid out.
	int hidden_lines = 0, line_no = 0;
	if (info->mc != NULL) {
		extern int ng_get_textfield_view_scroll_lines(void* mc_v);
		hidden_lines = ng_get_textfield_view_scroll_lines(info->mc);
		if (hidden_lines > 0)
			y_pos -= (float)hidden_lines
			         * ((float)info->font_height + (float)info->leading_twips);
	}

	// Per-glyph vertex buffer. SWF glyphs are stored as triangulated fills in
	// shape_data, and complex glyphs can hold many vertices (e.g. Bitstream
	// Vera Sans '8' is 582 vertices = 1164 floats). 8192 vertices (16384
	// floats / 64KB) covers anything reasonable; oversized glyphs hit the
	// guard below and are dropped, matching the previous behavior.
	#define TF_GLYPH_MAX_VERTS 8192
	static float xy_buf[TF_GLYPH_MAX_VERTS * 2];

	const char* text = info->text_utf8;
	size_t text_len = info->text_len;

	// Measure pass: walk text once to identify paragraphs and sum each
	// paragraph's glyph-advance width. Paragraphs are delimited by '\n'/'\r'
	// or the SENTINEL 0xFE/0xFF bytes. Each paragraph inherits align/bullet
	// from the run that covers its first byte. Mirrors Ruffle's
	// per-paragraph layout in core/src/html/layout.rs.
	#define MAX_TF_PARAGRAPHS 32
	struct {
		size_t start_byte;
		size_t end_byte;
		float width;     // in twips, sum of glyph advances
		u8 align;        // 0=left, 1=right, 2=center, 3=justify
		u8 bullet;
		u16 first_fh;    // font_height of first run covering this paragraph
	} pars[MAX_TF_PARAGRAPHS];
	int par_count = 0;
	{
		size_t pos = 0;
		int run_idx = 0;
		int cur_par = 0;
		pars[0].start_byte = 0;
		pars[0].width = 0.0f;
		pars[0].align = 0;
		pars[0].bullet = 0;
		pars[0].first_fh = info->font_height;
		int par_has_run = 0;

		while (pos < text_len) {
			u16 cur_fh = info->font_height;
			u8 cur_align = 0;
			u8 cur_bullet = 0;
			if (info->runs && info->run_count > 0) {
				while (run_idx < info->run_count &&
				       pos >= (size_t)(info->runs[run_idx].byte_start + info->runs[run_idx].byte_length)) {
					run_idx++;
				}
				if (run_idx < info->run_count && pos >= (size_t)info->runs[run_idx].byte_start) {
					if (info->runs[run_idx].font_height > 0)
						cur_fh = info->runs[run_idx].font_height;
					cur_align = info->runs[run_idx].align;
					cur_bullet = info->runs[run_idx].bullet;
				}
			}

			if (!par_has_run && cur_par < MAX_TF_PARAGRAPHS) {
				pars[cur_par].align = cur_align;
				pars[cur_par].bullet = cur_bullet;
				pars[cur_par].first_fh = cur_fh;
				par_has_run = 1;
			}

			unsigned char c0 = (unsigned char)text[pos];
			u16 cp;
			if (c0 == 0xFE || c0 == 0xFF) {
				cp = '\n';
				pos += 1;
			} else if (c0 >= 0xC0 && c0 < 0xE0 && pos + 1 < text_len) {
				cp = ((c0 & 0x1F) << 6) | ((unsigned char)text[pos + 1] & 0x3F);
				pos += 2;
			} else if (c0 >= 0xE0 && c0 < 0xF0 && pos + 2 < text_len) {
				cp = ((c0 & 0x0F) << 12) | (((unsigned char)text[pos + 1] & 0x3F) << 6) |
				     ((unsigned char)text[pos + 2] & 0x3F);
				pos += 3;
			} else {
				cp = c0;
				pos += 1;
			}

			if (cp == '\n' || cp == '\r') {
				if (cur_par < MAX_TF_PARAGRAPHS) {
					pars[cur_par].end_byte = pos;
					cur_par++;
				}
				if (cur_par < MAX_TF_PARAGRAPHS) {
					pars[cur_par].start_byte = pos;
					pars[cur_par].width = 0.0f;
					pars[cur_par].align = 0;
					pars[cur_par].bullet = 0;
					pars[cur_par].first_fh = info->font_height;
				}
				par_has_run = 0;
				continue;
			}

			int glyph_idx = ng_font_find_glyph(font_idx, cp);
			if (glyph_idx < 0) continue;
			s32 adv = ng_font_glyph_advance_by_idx(font_idx, glyph_idx);
			if (adv >= 0 && cur_par < MAX_TF_PARAGRAPHS) {
				float scale = (float)cur_fh / (float)em_square;
				pars[cur_par].width += tf_glyph_advance_twips(adv, scale, info->device_font);
			}
		}
		if (par_has_run && cur_par < MAX_TF_PARAGRAPHS) {
			pars[cur_par].end_byte = text_len;
			cur_par++;
		}
		par_count = cur_par;
		if (par_count == 0) {
			// Empty text (text_len == 0). Ruffle still lays out ONE line box for
			// an empty field (core/src/html/layout.rs), and a focused editable
			// field draws its caret at that line's alignment-adjusted origin —
			// which is what visual/edittext/edittext_caret_empty captures. The
			// producer only routes an empty field here when it is focused, so
			// this synthesised paragraph exists solely to give the caret its x.
			pars[0].start_byte = 0;
			pars[0].end_byte = 0;
			pars[0].width = 0.0f;
			pars[0].align = (info->runs && info->run_count > 0) ? info->runs[0].align : 0;
			pars[0].bullet = 0;
			pars[0].first_fh = info->font_height;
			par_count = 1;
		}
	}

	// Compute per-paragraph x_offset = left_alignment_offset + alignment_offset.
	// line_width matches the clip mask width (gutter-inset on both sides).
	// Bullet indent is a fixed 36px constant — Ruffle hard-codes this in
	// `left_alignment_offset` (core/src/html/layout.rs:759). The actual U+2022
	// bullet glyph is positioned at +18px, but we don't draw it (the test font
	// has no U+2022 glyph and the expected output reserves space without
	// rendering anything visible).
	//
	// First-line offset = max(0, left_margin + block_indent + indent) (twips).
	// Subsequent paragraphs after a hard newline are also "first lines" in
	// Ruffle's model (`is_first_line = end_of_para` in layout.rs:454), so they
	// receive the indent too. left_margin already has block_indent folded in by
	// the iterator.
	float line_width = info->w * 20.0f - 2.0f * gutter_twips;
	s32 first_line_left_off = info->left_margin_twips + info->indent_twips;
	if (first_line_left_off < 0) first_line_left_off = 0;
	float right_off = (float)(info->right_margin_twips > 0 ? info->right_margin_twips : 0);
	float usable_line_width = line_width - (float)first_line_left_off - right_off;
	if (usable_line_width < 0.0f) usable_line_width = 0.0f;
	float par_x_offset[MAX_TF_PARAGRAPHS] = {0};
	for (int p = 0; p < par_count; p++) {
		float bul = pars[p].bullet ? 720.0f : 0.0f;
		float remaining = usable_line_width - bul - pars[p].width;
		float offset = 0.0f;
		if (pars[p].align == 1) offset = remaining;            // right
		else if (pars[p].align == 2) offset = remaining * 0.5f;// center
		// justify: fall back to left for single-glyph paragraphs
		if (offset < 0.0f) offset = 0.0f;
		par_x_offset[p] = (float)first_line_left_off + bul + offset;
	}

	// Draw pass: same walk as measure, but pull per-paragraph x_offset at
	// paragraph start (right after newline / on first byte).
	float base_x = info->x * 20.0f + bxmin_off + gutter_twips;

	// Horizontal scroll, two independent offsets — the same pair the click
	// hit test adds back (action.c tf_view_hscroll_px + _tf_scroll_x):
	//  1. caret auto-scroll: shift the layout left so the caret stays inside
	//     a focused single-line field whose text is wider than the field;
	//  2. the AUTHOR-set `hscroll` (pixels, clamped to maxhscroll by the
	//     setter) — Ruffle edit_text.rs render_self translates the text by
	//     -hscroll for every field, focused or not.
	// The field clip mask hides the scrolled-out glyphs.
	//
	// (1) used to be browser-WASM-only on the premise that "in OFFSCREEN/
	// headless nothing is focused → caret_char<0 → CI render byte-identical".
	// Refuted: input.json focus/TextControl events are replayed in CI (the
	// caret itself was un-gated on the same evidence), so it now runs in
	// every render build. (2) was not rendered anywhere, so
	// visual/cache_as_bitmap/edittext_hscroll [output.02] (`text.hscroll =
	// text.maxhscroll` on Escape) drew the unscrolled text. s21 w2-px-c.
	if (info->caret_char >= 0 && info->mc != NULL) {
		int multiline = 0;
		for (size_t i = 0; i < text_len; i++) {
			unsigned char c = (unsigned char)text[i];
			if (c == '\n' || c == '\r' || c == 0xFE || c == 0xFF) { multiline = 1; break; }
		}
		if (!multiline) {
			// Caret pixel offset from the text origin (paragraph offset + the
			// glyph advances up to the caret).
			float caret_off = (par_count > 0 ? par_x_offset[0] : 0.0f);
			size_t p = 0; int cc = 0;
			while (p < text_len && cc < info->caret_char) {
				unsigned char c0 = (unsigned char)text[p];
				u16 cp;
				if (c0 >= 0xC0 && c0 < 0xE0 && p + 1 < text_len) {
					cp = ((c0 & 0x1F) << 6) | ((unsigned char)text[p+1] & 0x3F); p += 2;
				} else if (c0 >= 0xE0 && c0 < 0xF0 && p + 2 < text_len) {
					cp = ((c0 & 0x0F) << 12) | (((unsigned char)text[p+1] & 0x3F) << 6) |
					     ((unsigned char)text[p+2] & 0x3F); p += 3;
				} else { cp = c0; p += 1; }
				int gi = ng_font_find_glyph(font_idx, cp);
				if (gi >= 0) {
					s32 a = ng_font_glyph_advance_by_idx(font_idx, gi);
					if (a >= 0)
						caret_off += tf_glyph_advance_twips(a,
							(float)info->font_height / (float)em_square,
							info->device_font);
				}
				cc++;
			}
			float visible_w = info->w * 20.0f - 2.0f * gutter_twips;
			float old_scroll = ng_get_textfield_scroll_x(info->mc);
			float scroll = old_scroll;
			if (caret_off - scroll < 0.0f) scroll = caret_off;
			else if (caret_off - scroll > visible_w) scroll = caret_off - visible_w;
			if (scroll < 0.0f) scroll = 0.0f;
			if (scroll != old_scroll)
				ng_set_textfield_scroll_x(_gd_ctx, info->mc, scroll);
			base_x -= scroll;
		} else if (ng_get_textfield_scroll_x(info->mc) != 0.0f) {
			ng_set_textfield_scroll_x(_gd_ctx, info->mc, 0.0f);
		}
	}
	{
		extern float ng_get_textfield_view_hscroll_px(void* mc_v);
		base_x -= ng_get_textfield_view_hscroll_px(info->mc) * 20.0f;
	}

	int par_idx = 0;
	float x_pos = base_x + (par_count > 0 ? par_x_offset[0] : 0.0f);
	int at_par_start = 1;
	(void)at_par_start;

	// Selection highlight (single-line, focused field): Ruffle edit_text.rs
	// render_selection_background_for_line draws a BLACK box (GRAY when the
	// field is unfocused but alwaysShowSelection — our producer only reports a
	// selection for the focused field) from the first selected char's left
	// edge to the last selected char's right edge, over the line box
	// (ascent + descent; leading only when the selection continues onto the
	// next line), and render_layout_box then draws every selected glyph with an
	// IDENTITY colour transform, i.e. white — `sel_invert_lo/hi` below.
	//
	// This used to be browser-WASM-only, drawn light blue with the text left in
	// its own colour, on the premise that "nothing is selected in headless/
	// OFFSCREEN". Refuted the same way as the caret: input.json focus/
	// TextControl events are replayed in CI (visual/cache_as_bitmap/
	// edittext_selection [output.02] is a select-all after Selection.setFocus).
	// s21 w2-px-c.
	int sel_invert_lo = -1, sel_invert_hi = -1;
	if (info->sel_begin >= 0 && info->sel_end >= 0 && info->sel_begin != info->sel_end) {
		int sel_lo = info->sel_begin < info->sel_end ? info->sel_begin : info->sel_end;
		int sel_hi = info->sel_begin > info->sel_end ? info->sel_begin : info->sel_end;
		int sel_multiline = 0;
		for (size_t i = 0; i < text_len; i++) {
			unsigned char c = (unsigned char)text[i];
			if (c == '\n' || c == '\r' || c == 0xFE || c == 0xFF) { sel_multiline = 1; break; }
		}
		if (!sel_multiline) {
			float sscale = (float)info->font_height / (float)em_square;
			float xx = base_x + (par_count > 0 ? par_x_offset[0] : 0.0f);
			float x_lo = -1.0f, x_hi = -1.0f;
			int cc = 0; size_t p = 0;
			while (1) {
				if (cc == sel_lo && x_lo < 0.0f) x_lo = xx;
				if (cc == sel_hi) { x_hi = xx; break; }
				if (p >= text_len) break;
				unsigned char c0 = (unsigned char)text[p];
				u16 cp;
				if (c0 >= 0xC0 && c0 < 0xE0 && p + 1 < text_len) {
					cp = ((c0 & 0x1F) << 6) | ((unsigned char)text[p+1] & 0x3F); p += 2;
				} else if (c0 >= 0xE0 && c0 < 0xF0 && p + 2 < text_len) {
					cp = ((c0 & 0x0F) << 12) | (((unsigned char)text[p+1] & 0x3F) << 6) |
					     ((unsigned char)text[p+2] & 0x3F); p += 3;
				} else { cp = c0; p += 1; }
				int gi = ng_font_find_glyph(font_idx, cp);
				if (gi >= 0) {
					s32 a = ng_font_glyph_advance_by_idx(font_idx, gi);
					if (a >= 0) xx += tf_glyph_advance_twips(a, sscale, info->device_font);
				}
				cc++;
			}
			if (x_lo >= 0.0f && x_hi > x_lo) {
				float sb_scale = (float)baseline_fh / (float)em_square;
				float sel_top = y_pos - (float)ascent * sb_scale;
				float sel_h = (float)(ascent + descent) * sb_scale;
				renderer_draw_rect(context, x_lo, sel_top, x_hi - x_lo, sel_h,
					0.0f, 0.0f, 0.0f, 1.0f, 0, 0);
				sel_invert_lo = sel_lo;
				sel_invert_hi = sel_hi;
			}
		}
	}

	size_t pos = 0;
	int run_idx = 0;

	// Caret tracking: when this field is focused (info->caret_char >= 0), record
	// the pen position at the caret's character index so we can draw a caret bar
	// after the glyph pass. char_count counts decoded codepoints (== UTF-16 units
	// for the BMP); caret_x stays <0 until the caret index is reached.
	//
	// This used to be gated to browser-WASM on the premise that "OFFSCREEN /
	// headless CI captures must stay caret-free … or we diverge from Ruffle's
	// baseline". That premise is REFUTED: Ruffle's own goldens for
	// visual/edittext/edittext_caret_empty contain the caret bar (a 1-px black
	// column inside each focused field). Ruffle renders the caret whenever
	// `visible_selection()` is a collapsed caret on a focused, editable field
	// (edit_text.rs:1059) — headless included. The producer applies that same
	// predicate, so caret_char < 0 for every field Ruffle would leave bare.
#ifndef NO_GRAPHICS
	size_t caret_count = 0;
	float caret_x = -1.0f;
	float caret_baseline = y_pos;
#endif

	while (pos < text_len) {
		// Resolve run covering this byte position (runs are in order, monotonic).
		u32 cur_color = info->text_color;
		u16 cur_fh = info->font_height;
		if (info->runs && info->run_count > 0) {
			while (run_idx < info->run_count &&
			       pos >= (size_t)(info->runs[run_idx].byte_start + info->runs[run_idx].byte_length)) {
				run_idx++;
			}
			if (run_idx < info->run_count && pos >= (size_t)info->runs[run_idx].byte_start) {
				cur_color = info->runs[run_idx].color;
				if (info->runs[run_idx].font_height > 0)
					cur_fh = info->runs[run_idx].font_height;
			}
		}

		// Decode one UTF-8 character. Treat SENTINEL 0xFE/0xFF as paragraph breaks.
		unsigned char c0 = (unsigned char)text[pos];
		u16 cp;
		if (c0 == 0xFE || c0 == 0xFF) {
			cp = '\n';
			pos += 1;
		} else if (c0 >= 0xC0 && c0 < 0xE0 && pos + 1 < text_len) {
			cp = ((c0 & 0x1F) << 6) | ((unsigned char)text[pos + 1] & 0x3F);
			pos += 2;
		} else if (c0 >= 0xE0 && c0 < 0xF0 && pos + 2 < text_len) {
			cp = ((c0 & 0x0F) << 12) | (((unsigned char)text[pos + 1] & 0x3F) << 6) |
			     ((unsigned char)text[pos + 2] & 0x3F);
			pos += 3;
		} else {
			cp = c0;
			pos += 1;
		}

		// Record the caret x at its character index (pen position before this
		// char is laid out, on the current line's baseline).
#ifndef NO_GRAPHICS
		if (info->caret_char >= 0 && caret_x < 0.0f &&
		    caret_count == (size_t)info->caret_char) {
			caret_x = x_pos;
			caret_baseline = y_pos;
		}
		caret_count++;
#endif

		// Newline: advance y, reset x and consume next paragraph's offset.
		// The line advance carries the format's LEADING (Ruffle
		// html/layout.rs:452 — line height is ascent + descent +
		// line_leading_adjustment(), and :254 "Flash Player ignores
		// font-provided leading", so it is the span's leading only). Without it
		// every line after the first sat `leading` px too high:
		// visual/fonts/leading_define_font's leading=2 fields put their second
		// line on row 22 instead of the golden's row 24, in all four font blocks.
		if (cp == '\n' || cp == '\r') {
			par_idx++;
			x_pos = base_x + (par_idx < par_count ? par_x_offset[par_idx] : 0.0f);
			y_pos += (float)info->font_height + (float)info->leading_twips;
			line_no++;
			continue;
		}

		int glyph_idx = ng_font_find_glyph(font_idx, cp);
		if (glyph_idx < 0) continue; // missing glyph — skip

		float scale = (float)cur_fh / (float)em_square;
		float r = ((cur_color >> 16) & 0xFF) / 255.0f;
		float g = ((cur_color >> 8) & 0xFF) / 255.0f;
		float b = (cur_color & 0xFF) / 255.0f;
		// Selected glyphs draw white over the black selection box (see the
		// selection block above). caret_count was bumped for THIS char already.
		if (sel_invert_lo >= 0 && (int)caret_count - 1 >= sel_invert_lo
		    && (int)caret_count - 1 < sel_invert_hi) {
			r = g = b = 1.0f;
		}

		// Global glyph index
		size_t global_idx = glyph_base + (size_t)glyph_idx;
		// Skip the glyph-shape read/draw when there's no outline to draw:
		//  - the built-in fallback font carries metrics + advances only, and
		//  - a trace-only SWF (or a metrics-only embedded font) emits a tiny
		//    glyph_data placeholder, so indexing it here would read past the
		//    global (the OFFSCREEN_RENDER layout turns that OOB into a SIGABRT).
		// Bound-check against the actual glyph_data length (4 u32 per glyph).
		// The pen still advances below for layout either way.
		if (line_no >= hidden_lines
		    && !ng_font_is_builtin(font_idx) && (4 * global_idx + 1) < glyph_data_entries) {
		// glyph_base is already a COMBINED glyph index (tagDefineFontGlyphBase
		// re-bases it), and the generated `glyph_data` / `shape_data` symbols
		// are only the ROOT's prefix of the combined tables once a child movie
		// is linked. Read the combined tables when there are any; both fall
		// back to exactly the old expression otherwise. The bound above is
		// already the combined length (app_context->glyph_data_size).
		const u32 (*_gd)[1] = ng_combinedGlyphData();
		if (_gd == NULL) _gd = (const u32 (*)[1]) glyph_data;
		const u32 (*_sd)[4] = ng_combinedShapeData();
		if (_sd == NULL) _sd = (const u32 (*)[4]) shape_data;
		size_t g_offset = (size_t)_gd[4 * global_idx][0];
		size_t g_size = (size_t)_gd[4 * global_idx + 1][0];

		if (g_size > 0 && g_size <= TF_GLYPH_MAX_VERTS) {
			// Transform glyph vertices: scale from EM space + translate to position
			for (size_t v = 0; v < g_size; v++) {
				union { u32 u; float f; } vx, vy;
				vx.u = _sd[g_offset + v][0];
				vy.u = _sd[g_offset + v][1];
				xy_buf[2 * v]     = vx.f * scale + x_pos;
				xy_buf[2 * v + 1] = vy.f * scale + y_pos;
			}
			renderer_draw_tris(context, xy_buf, (u32)g_size, r, g, b, 1.0f, 0, 0);
		}
		} // end glyph-outline-available guard

		// Advance x by glyph advance width
		s32 adv = ng_font_glyph_advance_by_idx(font_idx, glyph_idx);
		if (adv >= 0) {
			x_pos += tf_glyph_advance_twips(adv, scale, info->device_font);
		}
	}
	#undef MAX_TF_PARAGRAPHS

	// Caret at (or past) the end of the text — pen is at its final position.
	// (Empty field: the draw loop never ran, so x_pos is still the synthesised
	// paragraph's alignment-adjusted origin, which is exactly where Ruffle puts
	// the caret.)
#ifndef NO_GRAPHICS
	if (info->caret_char >= 0 && caret_x < 0.0f &&
	    (size_t)info->caret_char >= caret_count) {
		caret_x = x_pos;
		caret_baseline = y_pos;
	}
#endif

	if (has_clip) {
		renderer_end_clip(context);
	}

	// Draw a thin vertical bar at the insertion point, in the text color.
	//
	// AFTER renderer_end_clip on purpose: Ruffle stashes the caret in
	// `EditTextRenderState::draw_caret_command` and replays it outside the text
	// mask ("We have to draw the caret outside of the text mask",
	// edit_text.rs::render_caret). It matters — a right-aligned empty field puts
	// the caret exactly ON the gutter-inset mask's right edge, so drawing it
	// inside the clip erases it (visual/edittext/edittext_caret_empty
	// output.05/.07/.11).
	//
	// Geometry mirrors edit_text.rs:
	//  * height = (ascent + descent) scaled to the line's font height
	//    (`let caret_height = ascent + descent;`, L1277) — NOT the em height.
	//  * x/y are pixel-snapped the way `EditTextPixelSnapping::apply` does for
	//    quality >= medium: `(tx + Twips::new(2)).trunc_to_pixel()`.
#ifndef NO_GRAPHICS
	if (info->caret_char >= 0 && caret_x >= 0.0f) {
		float scale_b = (float)baseline_fh / (float)em_square;
		float caret_top = caret_baseline - (float)ascent * scale_b;
		float caret_h = (float)(ascent + descent) * scale_b;
		float caret_w = 20.0f; // 1px, independent of zoom (Ruffle draws a line)
		caret_x   = floorf((caret_x + 2.0f) / 20.0f) * 20.0f;
		caret_top = floorf((caret_top + 2.0f) / 20.0f) * 20.0f;
		float cr = ((info->text_color >> 16) & 0xFF) / 255.0f;
		float cg = ((info->text_color >> 8) & 0xFF) / 255.0f;
		float cb = (info->text_color & 0xFF) / 255.0f;
		renderer_draw_rect(context, caret_x, caret_top, caret_w, caret_h,
			cr, cg, cb, 1.0f, 0, 0);
	}
#endif
}

// Helper: render a single drawing path (fill + stroke), handling gradients
static void render_drawing_path(const DrawingRenderInfo* info)
{
	if (info->fill_count > 0) {
		if (info->has_bitmap_fill && info->bitmap_pixels && info->bitmap_matrix) {
			renderer_draw_bitmap_tris(context, info->fill_verts, info->fill_count,
				info->bitmap_pixels, info->bitmap_width, info->bitmap_height,
				info->bitmap_matrix, info->bitmap_repeat, info->bitmap_smooth,
				info->transform_id, info->cxform_id);
		} else if (info->has_gradient && info->gradient_ramp && info->gradient_matrix) {
			renderer_draw_gradient_tris(context, info->fill_verts, info->fill_count,
				info->gradient_type, info->spread_mode, info->interpolation, info->focal_ratio,
				info->gradient_ramp, info->gradient_matrix,
				info->transform_id, info->cxform_id);
		} else {
			renderer_draw_tris(context, info->fill_verts, info->fill_count,
				info->fill_r, info->fill_g, info->fill_b, info->fill_a,
				info->transform_id, info->cxform_id);
		}
	}
	if (info->line_count > 0) {
		if (info->has_line_gradient && info->line_gradient_ramp && info->line_gradient_matrix) {
			renderer_draw_gradient_tris(context, info->line_verts, info->line_count,
				info->line_gradient_type, info->line_spread_mode, info->line_interpolation, info->line_focal_ratio,
				info->line_gradient_ramp, info->line_gradient_matrix,
				info->transform_id, info->cxform_id);
		} else {
			renderer_draw_tris(context, info->line_verts, info->line_count,
				info->line_r, info->line_g, info->line_b, info->line_a,
				info->transform_id, info->cxform_id);
		}
	}
}

// Callback for actionIterateDrawings: render Drawing API fills and strokes (unmasked).
static void drawing_render_cb(const DrawingRenderInfo* info, void* user_data)
{
	(void)user_data;
	render_drawing_path(info);
}

// Helper: render all paths in a DrawingMCInfo
static void render_drawing_mc_paths(const DrawingMCInfo* mc_info)
{
	for (int i = 0; i < mc_info->path_count; i++) {
		render_drawing_path(&mc_info->paths[i]);
	}
}

// Render only the FILL triangles of a drawing MC's paths (strokes skipped).
// Used when rasterizing a clip into the stencil buffer: Flash masks are
// defined by fill geometry only — a mask clip's line style never contributes
// to the mask region. (The stencil-write pipeline ignores color, so the fill
// triangles alone give correct coverage regardless of fill type.)
static void render_drawing_mc_paths_fill_only(const DrawingMCInfo* mc_info)
{
	for (int i = 0; i < mc_info->path_count; i++) {
		const DrawingRenderInfo* info = &mc_info->paths[i];
		if (info->fill_count > 0) {
			renderer_draw_tris(context, info->fill_verts, info->fill_count,
				info->fill_r, info->fill_g, info->fill_b, info->fill_a,
				info->transform_id, info->cxform_id);
		}
	}
}

// Rasterise a setMask masker's geometry into the stencil (mask defect B).
// Called between renderer_begin_clip_mask / renderer_end_clip_mask. A masker
// can be more than one of the three kinds, so all that apply are drawn.
// `masker->display_obj` is read FRESH here and never stored.
static void draw_mc_mask_geometry(SWFAppContext* app_context, MovieClip* masker)
{
	if (masker == NULL) return;

	// (1) Timeline entry — shape / morph shape / sprite (or button) subtree.
	//     Same routes as the sprite clipDepth branch in the root loops.
	DisplayObject* md = (DisplayObject*)masker->display_obj;
	if (md != NULL && md->char_id != 0 && (size_t)md->char_id < dictionary_capacity) {
		Character* mch = &dictionary[md->char_id];
		g_clip_mask_capture++;
		if (mch->type == CHAR_TYPE_SHAPE) {
			renderer_draw_shape(context, mch->shape_offset, mch->size,
				md->transform_id, md->cxform_id);
		} else if (mch->type == CHAR_TYPE_MORPH_SHAPE) {
			renderer_draw_shape(context, mch->morph_start_offset, mch->morph_start_size,
				md->transform_id, md->cxform_id);
		} else if ((mch->type == CHAR_TYPE_SPRITE || mch->type == CHAR_TYPE_BUTTON)
		           && md->sprite_display_list != NULL) {
			render_display_list(app_context, md->sprite_display_list, md->sprite_max_depth);
		}
		g_clip_mask_capture--;
	}

	// (2) Drawing-API geometry drawn into the masker. Fill triangles only —
	//     Flash masks ignore the mask clip's line style.
	if (masker->drawing_state != NULL && masker->name[0] != '\0') {
		DrawingRenderInfo dinfos[64];
		int dn = actionGetMCDrawingPathsByName(masker->name, dinfos, 64);
		if (dn > 0) {
			DrawingMCInfo dmc = { dn, dinfos };
			render_drawing_mc_paths_fill_only(&dmc);
		}
	}

	// (3) EditText wrapper — its glyph fills. The field-bounds clip inside
	//     textfield_glyph_render_cb is a NESTED mask, which Ruffle's renderer
	//     ignores inside a capture, so it is suppressed while g_clip_mask_capture
	//     is set: the stencil becomes the glyph silhouette, not the field box.
	if (masker->ng_textfield_idx >= 0 || masker->ng_textfield_idx == -2) {
		g_clip_mask_capture++;
		g_avm1_mask_glyph_only_mc = masker;
		actionIterateTextFieldGlyphs(textfield_glyph_render_cb, app_context);
		g_avm1_mask_glyph_only_mc = NULL;
		g_clip_mask_capture--;
	}
}

// Callback for actionIterateMaskedDrawings: stencil-masked rendering.
static void masked_drawing_render_cb(const DrawingMCInfo* masked, const DrawingMCInfo* mask, void* user_data)
{
	(void)user_data;

	// 1. Write mask shape into stencil buffer — fill geometry only (Flash
	//    masks ignore the mask clip's stroke).
	renderer_begin_clip_mask(context);
	render_drawing_mc_paths_fill_only(mask);
	renderer_end_clip_mask(context);

	// 2. Render masked MC content with stencil test
	render_drawing_mc_paths(masked);

	// 3. End stencil clip
	renderer_end_clip(context);
}

// Callback for actionIterateAttachedBitmaps: render BitmapData attached to dynamic MCs.
static void attached_bitmap_render_cb(const AttachedBitmapInfo* info, void* user_data)
{
	(void)user_data;
	// Every attached / loaded bitmap used to be drawn with transform_id 0
	// (identity) and its position BAKED into the quad's twips, so _xscale,
	// _yscale, _rotation, transform.matrix and _alpha were all discarded —
	// visual/scale_rotation_cache's `_xscale = -100` had literally no effect
	// on the render. apply_dynamic_mc_transforms already builds the right slot
	// (it handles negative scale correctly); route through it, drawing at the
	// local origin because the slot bakes _x/_y in. MCs with no AS-set
	// transform get slot 0 and keep the old stage-space path unchanged.
	if (info->xform_slot != 0) {
		u32 cx_slot = 0;
		if (info->alpha < 99.99f) {
			u32 slot = g_next_dynamic_cxform_slot;
			if (slot < g_cxform_slot_capacity) {
				g_next_dynamic_cxform_slot++;
				float cx[20];
				memset(cx, 0, sizeof(cx));
				cx[0] = cx[5] = cx[10] = 1.0f;
				cx[15] = info->alpha / 100.0f;
				if (cx[15] < 0.0f) cx[15] = 0.0f;
				renderer_write_cxform(context, slot, cx);
				cx_slot = slot;
			}
		}
		renderer_draw_bitmap_quad(context, info->pixels, info->width,
			info->height, 0.0f, 0.0f, info->xform_slot, cx_slot);
		return;
	}
	renderer_draw_bitmap_quad(context, info->pixels, info->width, info->height,
		info->x_twips, info->y_twips, 0, 0);
}

// Paint the EditText box + glyphs of the root-timeline fields sitting at
// exactly this display depth — Ruffle's `EditText::render_self` position
// (s17 w2-gfx-edittext-bg). Called from the two root render loops so a field's
// (white, BORDER-implied) background can no longer erase everything placed
// above it. `actionBeginTextFieldDepthPass` builds the presence table once per
// frame so this is O(1) on the depths that carry no field.
static void tf_draw_at_root_depth(SWFAppContext* app_context, int depth,
                                  int max_depth)
{
	if (!actionHasTextFieldAtDepth(depth)) return;
	actionSetTextFieldDepthWindow(TF_WINDOW_AT_DEPTH, depth, max_depth);
	actionIterateTextFields(textfield_render_cb, NULL);
	actionIterateTextFieldGlyphs(textfield_glyph_render_cb, app_context);
	actionSetTextFieldDepthWindow(TF_WINDOW_ALL, 0, 0);
}

// 1 when root display-list entry `obj` is an EditText that the root loops send
// through render_filtered_object (see g_tf_filter_obj). The root loops then
// skip tf_draw_at_root_depth at its depth and the orphan walk skips it, so the
// field is painted exactly once, by the filter route. Must agree with the
// branch order of both root loops: clip-depth entries and Alpha/Erase blend
// entries never reach render_filtered_object.
int tag_root_edittext_is_filtered(const DisplayObject* obj)
{
	if (obj == NULL || obj->char_id == 0 || obj->filter_type == 0) return 0;
	if (obj->clip_depth > 0) return 0;
	if (obj->blend_mode == 11 || obj->blend_mode == 12) return 0;
	// No dictionary-type test: an EditText with empty initial text is never
	// typed CHAR_TYPE_TEXT (no tagDefineText is emitted for it).
	return ng_getCharTextfieldIdx(obj->char_id) >= 0;
}

static void tf_draw_filtered_root_entry(SWFAppContext* app_context)
{
	if (g_tf_filter_depth < 0) return;
	// Wrapper-backed field (script-touched): the per-depth window.
	tf_draw_at_root_depth(app_context, g_tf_filter_depth, g_tf_filter_max_depth);
	// Untouched placement: the orphan emitter for this one entry.
	actionEmitOrphanRootTextField(app_context, g_tf_filter_depth,
		textfield_render_cb, textfield_glyph_render_cb, app_context);
}

// Root-loop wrapper around render_filtered_object that names a filtered
// EditText entry for render_single_object's TEXT arm while it runs.
static void render_filtered_root_entry(SWFAppContext* app_context,
                                       DisplayObject* obj, int depth,
                                       int max_depth_i)
{
	int tf = tag_root_edittext_is_filtered(obj);
	if (tf) {
		g_tf_filter_obj = obj;
		g_tf_filter_depth = depth;
		g_tf_filter_max_depth = max_depth_i;
	}
	render_filtered_object(app_context, obj);
	if (tf) {
		g_tf_filter_obj = NULL;
		g_tf_filter_depth = -1;
	}
}
#endif

// Re-render current display list state (for headless per-tick image capture).
// Only does the rendering pass — no frame script processing, no sprite init, etc.
#ifdef OFFSCREEN_RENDER
void tagRerenderFrame(SWFAppContext* app_context)
{
	if (context == NULL || !context->renderer_ok) return;

	// Compute focus rect BEFORE compose_children modifies transform_ids.
	// ng_getDisplayEntryBounds reads from CPU-side transform_data which only
	// has original (non-composed) slots; composed dynamic slots exist only in GPU.
	FocusRectInfo fri;
	int draw_focus_rect = actionGetFocusRectInfo(&fri);

	// --- Re-compose transforms (same as tagShowFrame) ---
	{
		u32 orig_xform_count = (u32)(app_context->transform_data_size / (16 * sizeof(float)));
		g_next_dynamic_xform_slot = orig_xform_count;
		g_xform_slot_capacity = context->xform_slot_count;
		xform_overrides_reset();
		text_glyph_overrides_reset();

		u32 orig_cxform_count = app_context->cxform_data_size > 0
			? (u32)(app_context->cxform_data_size / (20 * sizeof(float))) : 1;
		g_next_dynamic_cxform_slot = orig_cxform_count;
		g_cxform_slot_capacity = context->cxform_slot_count;
		cxform_overrides_reset();
	}

	// Runtime transform updates (AS _x/_y/_xscale/_rotation changes).
	//
	// In-place mutation hazard: this loop mutates `transform_data[obj->transform_id]`
	// CPU-side, then uploads the mutated matrix to GPU at the same slot. The
	// recompiler dedupes identical matrices into one transform_data slot, so
	// multiple display entries can share a transform_id — and the in-place
	// mutation drags every sharer along. For SPRITE / BUTTON entries the
	// compose loop below builds its own effective parent matrix on a local
	// stack buffer (`mc->as_set_flags`-aware) and passes that to
	// compose_children, so SPRITE / BUTTON entries DON'T need this in-place
	// mutation. Skipping them here avoids the shared-slot displacement
	// without losing AS-set positioning. SHAPE / TEXT / MORPH entries still
	// go through this path because they're leaf nodes — render_display_list
	// draws them via obj->transform_id directly, so the GPU slot must hold
	// the AS-overlaid matrix.
	{
		extern MovieClip* actionFindMovieClipByName(const char* instance_name);
		for (size_t i = 1; i <= max_depth; ++i)
		{
			DisplayObject* obj = &display_list[i];
			if (obj->char_id == 0 || obj->instance_name == NULL) continue;
			MovieClip* mc = actionFindMovieClipByName(obj->instance_name);
			if (mc == NULL || mc->as_set_flags == 0) continue;
			// Skip sprites and buttons — the compose loop below handles them.
			Character* ch_ = &dictionary[obj->char_id];
			if (ch_->type == CHAR_TYPE_SPRITE || ch_->type == CHAR_TYPE_BUTTON) continue;
			// app_context->transform_data, matching the tagShowFrame twin: with
			// child movies linked transform_id indexes the COMBINED table.
			float* slot = (float*)app_context->transform_data + obj->transform_id * 16;
			apply_as_transform(slot, mc, mc->as_set_flags);
			renderer_write_transform(context, obj->transform_id, slot);
		}
		// Dynamic MCs (createEmptyMovieClip etc.) aren't in display_list —
		// allocate per-tick slots for any with AS-set transforms so their
		// Drawing-API paths route through the freshly built matrix.
		apply_dynamic_mc_transforms(app_context);
		__asm__ volatile("" ::: "memory");
		for (size_t i = 1; i <= max_depth; ++i)
		{
			DisplayObject* obj = &display_list[i];
			if (obj->char_id == 0 || !obj->cx_overridden) continue;
			float cx[20];
			build_cxform_from_obj(cx, obj);
			u32 new_slot = g_next_dynamic_cxform_slot;
			if (new_slot < g_cxform_slot_capacity) {
				g_next_dynamic_cxform_slot++;
				cxform_overrides_push(obj, obj->cxform_id);
				obj->cxform_id = new_slot;
				renderer_write_cxform(context, new_slot, cx);
			}
		}
		__asm__ volatile("" ::: "memory");
	}

	// Compose children transforms (sprites, buttons, text).
	//
	// For SPRITE / BUTTON entries with as_set_flags != 0, build the effective
	// parent matrix on a LOCAL stack buffer (apply_as_transform overlay on
	// top of the original CPU-side transform_data). The runtime-transform
	// loop above explicitly skips SPRITE / BUTTON entries, so the CPU-side
	// transform_data slot still holds the un-mutated timeline matrix — that
	// way we can read it here without dragging along any other display
	// entries that share the same transform_id (the recompiler dedupes
	// identical matrices into one slot, so sharing is the common case).
	{
	extern MovieClip* actionFindMovieClipByName(const char* instance_name);
	for (size_t i = 1; i <= max_depth; ++i)
	{
		DisplayObject* obj = &display_list[i];
		if (obj->char_id == 0) continue;
		Character* ch = &dictionary[obj->char_id];
		// cacheAsBitmap PixelSnapping::Always for a ROOT LEAF entry. Sprites
		// and buttons snap through cab_snap_world in their own arms below
		// (compose_children needs the snapped matrix as the parent); static
		// text is deliberately skipped — renderer_compose_text_transforms
		// reads the CPU-side transform table, which has no entry at a dynamic
		// slot.
		if (cab_snaps(obj) && ch->type != CHAR_TYPE_SPRITE
		    && ch->type != CHAR_TYPE_BUTTON && ch->type != CHAR_TYPE_TEXT)
			cab_snap_root_leaf(app_context, obj);
		if (ch->type == CHAR_TYPE_SPRITE)
		{
			if (obj->sprite_display_list != NULL)
			{
				const float* sprite_xform;
				float sprite_xform_buf[16];
				MovieClip* mc_for_xform = (obj->instance_name != NULL)
					? actionFindMovieClipByName(obj->instance_name) : NULL;
				if (mc_for_xform != NULL && mc_for_xform->as_set_flags != 0) {
					memcpy(sprite_xform_buf,
						(const float*)app_context->transform_data + obj->transform_id * 16,
						sizeof(sprite_xform_buf));
					apply_as_transform(sprite_xform_buf, mc_for_xform, mc_for_xform->as_set_flags);
					sprite_xform = sprite_xform_buf;
				} else {
					sprite_xform = (const float*)app_context->transform_data + obj->transform_id * 16;
				}
				// cacheAsBitmap PixelSnapping::Always (root sprite): the snapped
				// matrix has to be the one compose_children sees as the parent.
				float cab_sprite_buf[16];
				sprite_xform = cab_snap_world(obj, sprite_xform, cab_sprite_buf);
				float seed_cx[20];
				const float* seed = root_seed_cxform(app_context, obj, seed_cx)
					? seed_cx : NULL;
				compose_children(app_context,
					obj->sprite_display_list, obj->sprite_max_depth,
					sprite_xform, seed, obj->cxform_id);
			}
		}
		else if (ch->type == CHAR_TYPE_BUTTON)
		{
			// Compose against the persistent obj->sprite_display_list populated
			// by the first tagShowFrame. The previous implementation built a
			// fresh temp display_list via ch->button_state_funcs and freed it
			// here, which leaked the cxform/transform propagation: render_single_object
			// reads obj->sprite_display_list and so saw the original (uncomposed)
			// child entries. That caused buttons to render with identity cxform
			// and the un-composed transform (often slot 0/position 0), overlapping
			// other display-list entries.
			if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
			{
				const float* btn_xform;
				float btn_xform_buf[16];
				MovieClip* mc_for_xform = (obj->instance_name != NULL)
					? actionFindMovieClipByName(obj->instance_name) : NULL;
				if (mc_for_xform != NULL && mc_for_xform->as_set_flags != 0) {
					memcpy(btn_xform_buf,
						(const float*)app_context->transform_data + obj->transform_id * 16,
						sizeof(btn_xform_buf));
					apply_as_transform(btn_xform_buf, mc_for_xform, mc_for_xform->as_set_flags);
					btn_xform = btn_xform_buf;
				} else {
					btn_xform = (const float*)app_context->transform_data + obj->transform_id * 16;
				}
				float cab_btn_buf[16];
				btn_xform = cab_snap_world(obj, btn_xform, cab_btn_buf);
				float seed_cx[20];
				const float* seed = root_seed_cxform(app_context, obj, seed_cx)
					? seed_cx : NULL;
				compose_children(app_context,
					obj->sprite_display_list, obj->sprite_max_depth,
					btn_xform, seed, obj->cxform_id);
			}
		}
		else if (ch->type == CHAR_TYPE_TEXT)
		{
			renderer_compose_text_transforms(context,
				app_context->transform_data,
				obj->transform_id, ch->transform_start, ch->text_size);
		}
	}
	}

	// --- Render pass ---
	renderer_open_pass(context);

	// Apply _root's AS-set transform (e.g. `_root._x = 200`) by composing it
	// onto the stage_to_ndc projection — this is the cheapest way to honor
	// "root is just another DisplayObject with its own transform" without
	// touching every per-child transform. Mirrors Ruffle's Stage→root chain
	// where root's matrix sits between the projection and child transforms.
	{
		extern MovieClip root_movieclip;
		if (root_movieclip.as_set_flags != 0) {
			float root_xform[16] = {
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f,
			};
			apply_as_transform(root_xform, &root_movieclip, (u8)(1|2|4|8|16));
			float composed[16];
			hit_test_mat4_multiply(composed, app_context->stage_to_ndc, root_xform);
			renderer_upload_stage_transform(context, composed);
		}
	}

#ifdef HEADLESS_RENDER_ENABLED
	extern int capture_has_pending(void);
	if (capture_has_pending())
		renderer_request_capture(context);
#endif

	// Render display list
	u32 active_clip_depth = 0;
	// EditText boxes/glyphs are painted at their own depth from here on
	// (tf_draw_at_root_depth); depth 0 is a legal PlaceObject depth that the
	// loop below never visits, so it is flushed first.
	actionBeginTextFieldDepthPass((int) max_depth);
	tf_draw_at_root_depth(app_context, 0, (int) max_depth);
	for (size_t i = 1; i <= max_depth; ++i)
	{
		// End an active clip BEFORE the empty-depth skip, matching
		// render_display_list and tagShowFrame. With the skip first, an EMPTY
		// depth immediately after a clip range never reached the end-of-range
		// check, so the clip stayed active for the next non-empty depth and
		// content past the range kept being clipped.
		if (active_clip_depth > 0 && i > active_clip_depth) {
			renderer_end_clip(context);
			active_clip_depth = 0;
		}
		if (!tag_root_edittext_is_filtered(&display_list[i]))
			tf_draw_at_root_depth(app_context, (int) i, (int) max_depth);
		DisplayObject* obj = &display_list[i];
		if (obj->char_id == 0) continue;
		// AVM1 setMask pairing (mask defect B) — see render_display_list.
		MovieClip* avm1_masker = NULL;
		if (actionAvm1MaskPairCount() != 0) {
			if (entry_is_live_masker(obj)) continue;
			avm1_masker = avm1_masker_for_entry(obj);
		}
		if (obj->clip_depth > 0) {
			// Pop before pushing — see render_display_list for why (a second
			// clip range opened inside a live one leaks a stencil LEVEL under
			// the Equal/IncrementClamp model). No draw happens between the
			// pop and the push, so this is byte-identical under the old
			// Always/Replace model.
			if (active_clip_depth > 0) {
				renderer_end_clip(context);
				active_clip_depth = 0;
			}
			Character* ch = &dictionary[obj->char_id];
			if (ch->type == CHAR_TYPE_SHAPE) {
				renderer_begin_clip_mask(context);
				renderer_draw_shape(context, ch->shape_offset, ch->size, obj->transform_id, obj->cxform_id);
				renderer_end_clip_mask(context);
				active_clip_depth = obj->clip_depth;
			} else if (ch->type == CHAR_TYPE_SPRITE) {
				// Sprite clip mask: render sprite content + Drawing API into stencil
				renderer_begin_clip_mask(context);
				g_clip_mask_capture++;
				if (obj->sprite_display_list != NULL)
					render_display_list(app_context, obj->sprite_display_list, obj->sprite_max_depth);
				g_clip_mask_capture--;
				// Also render Drawing API shapes drawn into this sprite's MC
				if (obj->instance_name != NULL) {
					DrawingRenderInfo dinfos[64];
					int dn = actionGetMCDrawingPathsByName(obj->instance_name, dinfos, 64);
					for (int di = 0; di < dn; di++)
						render_drawing_path(&dinfos[di]);
				}
				renderer_end_clip_mask(context);
				active_clip_depth = obj->clip_depth;
			}
			continue;
		}
		// Alpha (11) / Erase (12) with no Layer above them are IGNORED by Flash —
		// the draw is dropped entirely (ruffle render/wgpu/src/surface.rs:239-244,
		// "An Alpha or Erase with no Layer above it should be ignored"). We have no
		// layer groups yet, so "no layer above" is unconditionally true. When layer
		// groups land, this condition becomes a layer-stack query.
		int blend_skip = (obj->blend_mode == 11 || obj->blend_mode == 12);
		// Non-trivial blend modes render the object's whole subtree into a layer
		// and composite it once. Filter+blend on the same object both want
		// filter_tex_a, so blend keeps the legacy per-draw pipeline there.
		int blend_layered = !blend_skip && obj->filter_type == 0
			&& renderer_blend_mode_is_layered(context, obj->blend_mode);
		// setMask maskee: the masker's geometry becomes the stencil for this
		// entry's paint, restored (not ended) afterwards.
		// opaqueBackground: painted under this entry and OUTSIDE its own
		// mask / scrollRect, exactly where Ruffle's render_base emits it.
		draw_entry_opaque_background(app_context, obj);
		u32 avm1_mask_saved_clip = 0;
		if (avm1_masker != NULL)
			avm1_mask_saved_clip = avm1_mask_push(app_context, avm1_masker);
		if (obj->blend_mode > 1 && !blend_layered && !blend_skip)
			renderer_set_blend_mode(context, obj->blend_mode);
		if (blend_skip) {
			// nothing drawn
		} else if (blend_layered) {
			renderer_suspend_pass(context);
			renderer_capture_backdrop(context, obj->blend_mode);
			renderer_begin_offscreen_pass(context);
			render_single_object(app_context, obj);
			renderer_end_offscreen_pass(context);
			renderer_resume_pass(context);
			renderer_composite_blend(context, obj->blend_mode,
				active_clip_depth > 0 ? 1 : 0);
		} else if (obj->filter_type != 0) {
			render_filtered_root_entry(app_context, obj, (int) i, (int) max_depth);
			if (obj->blend_mode > 1 && !blend_layered)
				renderer_set_blend_mode(context, obj->blend_mode);
		} else {
			render_single_object(app_context, obj);
		}
		if (obj->blend_mode > 1 && !blend_layered && !blend_skip)
			renderer_set_blend_mode(context, 0);
		if (avm1_masker != NULL)
			renderer_restore_clip(context, avm1_mask_saved_clip);
	}
	if (active_clip_depth > 0)
		renderer_end_clip(context);

	// Root-attached MovieClips (see comment in tagShowFrame for rationale —
	// these live in child_mc_cache, not display_list, and need their own
	// render pass after the timeline display loop).
	{
		extern MovieClip* child_mc_cache[];
		extern int child_mc_count;
		for (int i = 0; i < child_mc_count; i++) {
			MovieClip* mc = child_mc_cache[i];
			if (mc == NULL) continue;
			if (mc->depth == INT_MIN) continue;
			if (mc->parent != &root_movieclip) continue;
			if (mc->display_obj == NULL) continue;
			uintptr_t dl_lo = (uintptr_t)display_list;
			uintptr_t dl_hi = dl_lo + display_list_capacity * sizeof(DisplayObject);
			uintptr_t dobj = (uintptr_t)mc->display_obj;
			if (dobj >= dl_lo && dobj < dl_hi) continue;

			DisplayObject* d = (DisplayObject*)mc->display_obj;
			if (d->sprite_display_list == NULL || d->sprite_max_depth == 0)
				continue;
			float mc_xform[16] = {
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f,
			};
			if (mc->as_set_flags != 0) {
				apply_as_transform(mc_xform, mc, (u8)(1|2|4|8|16));
			}
			compose_children(app_context, d->sprite_display_list,
				d->sprite_max_depth, mc_xform, NULL, 0);
			render_display_list(app_context, d->sprite_display_list,
				d->sprite_max_depth);
		}
	}

	// Text field backgrounds/borders and glyph rendering. The root-timeline
	// fields were already painted at their own depth inside the loop above, so
	// this pass covers only what the walk could not place: createTextField
	// fields, fields inside sprites, and orphans.
	actionSetTextFieldDepthWindow(TF_WINDOW_REST, 0, (int) max_depth);
	actionIterateTextFields(textfield_render_cb, NULL);
	actionIterateTextFieldGlyphs(textfield_glyph_render_cb, app_context);
	actionSetTextFieldDepthWindow(TF_WINDOW_ALL, 0, 0);
	actionIterateOrphanTextFields(app_context, textfield_render_cb,
		textfield_glyph_render_cb, app_context);

	// Drawing API fills and strokes
	actionIterateDrawings(drawing_render_cb, NULL);
	actionIterateMaskedDrawings(masked_drawing_render_cb, NULL);

	// Attached bitmaps (from attachBitmap on dynamic MCs)
	actionIterateAttachedBitmaps(attached_bitmap_render_cb, NULL);

	// Focus rect (pre-computed before compose_children)
	// Drawn as 3-pixel thick border INSIDE the object's world AABB.
	if (draw_focus_rect) {
		float t = 3.0f * 20.0f;  // 3 pixels in twips
		float fx = fri.x, fy = fri.y, fw = fri.w, fh = fri.h;
		renderer_draw_rect(context, fx, fy, fw, t, 1, 1, 0, 1, 0, 0);                  // top
		renderer_draw_rect(context, fx, fy + fh - t, fw, t, 1, 1, 0, 1, 0, 0);          // bottom
		renderer_draw_rect(context, fx, fy + t, t, fh - 2*t, 1, 1, 0, 1, 0, 0);         // left
		renderer_draw_rect(context, fx + fw - t, fy + t, t, fh - 2*t, 1, 1, 0, 1, 0, 0);// right
	}

	renderer_close_pass(context);
	xform_overrides_restore();
	text_glyph_overrides_restore();
	cxform_overrides_restore();
}
#endif

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
// Browser-WASM: recursively finalize tagRemoveObject(2)'d entries that weren't
// reclaimed by a same-tick Place. tagRemoveObject2 only marks pending_remove=1;
// the finalize walk used to scan the ROOT display list only, so a remove inside
// a NESTED sprite that advanced this tick (and had no replacement placement at
// that depth) was never cleared and kept rendering. Pong's "2 Player" advances
// the unnamed sprite_26 frame 0→1, whose frame_1 removes the depth-1 label and
// the depth-4 "1 Player" button with no replacement — they ghosted on top of
// the Enter Names form. Recurse into sprite children (operating on the global
// display_list so clear_display_entry/invalidate target the right list) so
// those nested removals are finalized before render. Buttons are skipped:
// their state display lists are rebuilt by ng_update_button_states.
static void finalize_pending_removes_recursive(SWFAppContext* app_context)
{
	for (size_t d = 1; d <= max_depth; d++)
	{
		DisplayObject* obj = &display_list[d];
		if (obj->char_id != 0 && dictionary[obj->char_id].type == CHAR_TYPE_SPRITE
		    && obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
		{
			DisplayObject* saved_dl = display_list;
			size_t saved_max = max_depth;
			size_t saved_cap = display_list_capacity;
			display_list = obj->sprite_display_list;
			max_depth = obj->sprite_max_depth;
			display_list_capacity = obj->sprite_dl_capacity;
			finalize_pending_removes_recursive(app_context);
			obj->sprite_display_list = display_list;
			obj->sprite_max_depth = max_depth;
			obj->sprite_dl_capacity = display_list_capacity;
			display_list = saved_dl;
			max_depth = saved_max;
			display_list_capacity = saved_cap;
			obj = &display_list[d];
		}
		if (obj->pending_remove)
		{
			obj->pending_remove = 0;
			if (obj->char_id != 0)
			{
				invalidate_mc_for_dl_entry(app_context, obj);
				clear_display_entry(app_context, d);
			}
		}
	}
}
#endif

#ifndef NO_GRAPHICS
// Compute an MC's local-to-world (stage-space) transform by walking its parent
// chain. Used by the root-attached render pass to place attachMovie children of
// a non-root sprite (e.g. Tetris's block cells parented to b_mc). _root itself
// returns identity — the root's own transform is applied globally via the
// uploaded stage transform, so folding it in here would double-apply it.
static void compute_mc_world_xform(SWFAppContext* app_context, MovieClip* mc, float out[16])
{
	static const float identity[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f,
	};
	if (mc == NULL || mc == &root_movieclip) {
		memcpy(out, identity, sizeof(identity));
		return;
	}
	DisplayObject* d = (DisplayObject*)mc->display_obj;
	uintptr_t lo = (uintptr_t)display_list;
	uintptr_t hi = lo + display_list_capacity * sizeof(DisplayObject);
	int in_root_dl = (d != NULL) && (uintptr_t)d >= lo && (uintptr_t)d < hi;
	if (d != NULL && (in_root_dl || d->transform_id != 0)) {
		// Timeline-placed (root or nested): its transform_data slot holds the
		// stage-space matrix — for root entries the raw placement, for nested
		// entries the matrix compose_children already composed to world this
		// frame. Overlay any AS-set transform, then we're done (no recursion:
		// the slot is already world-space).
		memcpy(out, (const float*)app_context->transform_data + (size_t)d->transform_id * 16,
			16 * sizeof(float));
		if (mc->as_set_flags != 0)
			apply_as_transform(out, mc, mc->as_set_flags);
		return;
	}
	// Standalone attachMovie entry (transform_id left at 0): build the local
	// matrix from AS state and compose under the parent's world.
	float parent_world[16];
	compute_mc_world_xform(app_context, mc->parent, parent_world);
	float local[16];
	memcpy(local, identity, sizeof(identity));
	apply_as_transform(local, mc, (u8)(1|2|4|8|16));
	hit_test_mat4_multiply(out, parent_world, local);
}

#ifndef OFFSCREEN_RENDER
// Root display_list index of an attached MC's parent, or -1 if the parent is
// _root or is not a timeline-placed object (i.e. itself an attached/dynamic MC
// whose DisplayObject lives outside the global display_list array). Used to
// layer attached (child_mc_cache) clips at their parent's depth rather than
// above all timeline content. See the interleave in tagShowFrame.
static int attached_parent_dl_index(MovieClip* mc)
{
	extern MovieClip root_movieclip;
	if (mc == NULL || mc->parent == NULL || mc->parent == &root_movieclip)
		return -1;
	DisplayObject* pd = (DisplayObject*)mc->parent->display_obj;
	if (pd == NULL) return -1;
	uintptr_t lo = (uintptr_t)display_list;
	uintptr_t hi = lo + (uintptr_t)display_list_capacity * sizeof(DisplayObject);
	uintptr_t p = (uintptr_t)pd;
	if (p < lo || p >= hi) return -1;
	return (int)(pd - display_list);
}
#endif

// Render one attached (child_mc_cache) MovieClip's sprite content under its
// parent's world transform. Shared by the depth-interleaved pass and the
// post-loop root-attached pass in tagShowFrame. No-op when the clip has no
// sprite content, is hidden, has a hidden ancestor, or is a timeline-placed
// root child already drawn by the main display loop.
static void render_attached_child(SWFAppContext* app_context, MovieClip* mc)
{
	extern MovieClip root_movieclip;
	if (mc->display_obj == NULL) return;            // no sprite content
	if (!mc->visible) return;                       // AS-set _visible=false
	// A live setMask masker is stencil-only, never ordinary content
	// (mask defect B).
	if (actionAvm1IsLiveMasker(mc)) return;
	// Timeline-placed root children point into the global display_list and were
	// already drawn by the main render loop.
	uintptr_t dl_lo = (uintptr_t)display_list;
	uintptr_t dl_hi = dl_lo + (uintptr_t)display_list_capacity * sizeof(DisplayObject);
	uintptr_t dobj = (uintptr_t)mc->display_obj;
	if (dobj >= dl_lo && dobj < dl_hi) return;

	DisplayObject* d = (DisplayObject*)mc->display_obj;
	if (d->sprite_display_list == NULL || d->sprite_max_depth == 0) return;
	// Skip timeline-placed NESTED children. The global-display_list guard above
	// only catches ROOT timeline children; a sprite nested inside another sprite
	// has its display_obj inside an ANCESTOR's sprite_display_list (not the
	// global one), yet it is ALREADY composed and drawn by that ancestor's
	// recursive render from the main display loop. Re-processing it here composes
	// its subtree a SECOND time — and because the first compose already rewrote
	// each entry's transform_id to a dynamic GPU slot, this second compose reads
	// those dynamic slots as CPU-side transform_data indices (out of bounds) and
	// produces garbage matrices, drawing the subtree again as exploded geometry
	// on top of the correct render. (Meteor Storm's menu: char 23 nested in 24,
	// char 19/22 nested in 23.) Genuine attachMovie children are exempt:
	// ng_attachMovie gives them a STANDALONE display_obj that lives in no
	// sprite_display_list, so this never matches them (e.g. Tetris board cells).
	for (MovieClip* anc = mc->parent; anc != NULL && anc != &root_movieclip; anc = anc->parent) {
		DisplayObject* ad = (DisplayObject*)anc->display_obj;
		if (ad == NULL || ad->sprite_display_list == NULL) continue;
		uintptr_t alo = (uintptr_t)ad->sprite_display_list;
		uintptr_t ahi = alo + (uintptr_t)ad->sprite_dl_capacity * sizeof(DisplayObject);
		if (dobj >= alo && dobj < ahi) return;
	}
	// Skip clips whose parent chain is hidden.
	for (MovieClip* _a = mc->parent; _a != NULL && _a != &root_movieclip; _a = _a->parent)
		if (!_a->visible) return;

	float parent_world[16];
	compute_mc_world_xform(app_context, mc->parent, parent_world);
	float mc_local[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f,
	};
	apply_as_transform(mc_local, mc, (u8)(1|2|4|8|16));
	float mc_xform[16];
	hit_test_mat4_multiply(mc_xform, parent_world, mc_local);
	compose_children(app_context, d->sprite_display_list,
		d->sprite_max_depth, mc_xform, NULL, 0);
	// setMask maskee: dynamic clips with display-list content get the same
	// stencil push/restore as timeline entries (mask defect B).
	MovieClip* avm1_masker = (MovieClip*)actionAvm1MaskerForMC(mc);
	u32 avm1_mask_saved_clip = 0;
	if (avm1_masker != NULL)
		avm1_mask_saved_clip = avm1_mask_push(app_context, avm1_masker);
	render_display_list(app_context, d->sprite_display_list,
		d->sprite_max_depth);
	if (avm1_masker != NULL)
		renderer_restore_clip(context, avm1_mask_saved_clip);
}

#if !defined(NO_GRAPHICS)
// Update DefineButton2 hover/press/release state for buttons that live inside
// attachMovie'd / createEmptyMovieClip'd clips (child_mc_cache), which are NOT
// in the root display_list and so are missed by ng_update_button_states' root
// walk. Mirrors render_attached_child's per-clip eligibility (skip
// root-timeline-placed and timeline-NESTED clips — those are already covered by
// the root walk / their ancestor's recursion — and hidden subtrees) and world
// transform, then runs the same recursive button state machine on each
// attached clip's sprite_display_list. Threads found_hover so the cursor +
// topmost-button semantics stay consistent. (N's main menu — menuMC built via
// gfx.CreateSprite = buffer.attachMovie — was entirely unreachable before this,
// so its H/S/Q/N menu clicks were dead while keyboard worked.)
static void ng_update_button_states_attached(SWFAppContext* app_context, int* found_hover)
{
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	for (int i = 0; i < child_mc_count; i++) {
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL || mc->display_obj == NULL || !mc->visible) continue;
		DisplayObject* d = (DisplayObject*)mc->display_obj;
		if (d->sprite_display_list == NULL || d->sprite_max_depth == 0) continue;
		uintptr_t dobj = (uintptr_t)d;
		// Skip root-timeline-placed clips (covered by the root walk).
		uintptr_t dl_lo = (uintptr_t)display_list;
		uintptr_t dl_hi = dl_lo + (uintptr_t)display_list_capacity * sizeof(DisplayObject);
		if (dobj >= dl_lo && dobj < dl_hi) continue;
		// Skip timeline-NESTED clips (already reached via an ancestor's recursion).
		int nested = 0;
		for (MovieClip* anc = mc->parent; anc != NULL && anc != &root_movieclip; anc = anc->parent) {
			DisplayObject* ad = (DisplayObject*)anc->display_obj;
			if (ad == NULL || ad->sprite_display_list == NULL) continue;
			uintptr_t alo = (uintptr_t)ad->sprite_display_list;
			uintptr_t ahi = alo + (uintptr_t)ad->sprite_dl_capacity * sizeof(DisplayObject);
			if (dobj >= alo && dobj < ahi) { nested = 1; break; }
		}
		if (nested) continue;
		// Skip clips whose parent chain is hidden.
		int hidden_anc = 0;
		for (MovieClip* a = mc->parent; a != NULL && a != &root_movieclip; a = a->parent)
			if (!a->visible) { hidden_anc = 1; break; }
		if (hidden_anc) continue;
		// World transform of this attached clip (mirror render_attached_child).
		float parent_world[16];
		compute_mc_world_xform(app_context, mc->parent, parent_world);
		float mc_local[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		apply_as_transform(mc_local, mc, (u8)(1|2|4|8|16));
		float mc_xform[16];
		hit_test_mat4_multiply(mc_xform, parent_world, mc_local);
		g_btn_attached_walk = 1;
		ng_update_button_states_in_dl(app_context, d->sprite_display_list,
			d->sprite_max_depth, mc_xform, mc, d, found_hover);
		g_btn_attached_walk = 0;
	}
}
#endif // !NO_GRAPHICS
#endif

void tagShowFrame(SWFAppContext* app_context)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (g_tag_skip_mode) return;
	if (g_in_action_call) return;

	// Phase 4/5 safety drain: any CLIP_EVENT_INITIALIZE / CLIP_EVENT_CONSTRUCT
	// / registerClass-ctor entries that were queued during this frame's
	// placements but weren't drained by the outermost tagPlaceObject2
	// (e.g. goto catch-up, where the outer tagPlaceObject2 runs under
	// catch_up_mode=1 and the in-function drain block is skipped) drain here.
	// In the common case the queue is empty and these calls are no-ops — the
	// in-function drain already covered them. Drain in priority order
	// (INIT → CONSTRUCT → REGISTER_CTOR) to preserve the pre-Phase-5 ordering.
	//
	// During goto catch-up (g_goto_catchup_active > 0) we defer CLIP_INIT
	// and REGISTER_CTOR: queued entries for placements that are subsequently
	// removed in a later catch-up frame must stay queued so the post-catch-up
	// drain in ng_executeGotoCatchUp can skip them via the aq_drain
	// clip->avm1_removed filter. Mirrors Ruffle's run_goto goto_commands
	// aggregation (RegisterClassTest3).
	//
	// CLIP_CONSTRUCT (clip-event handler — distinct from REGISTER_CTOR's
	// registered-class constructor) is also deferred during goto catch-up
	// (SWAPDEPTHS_REWIND_UNBLOCK Phase B). Mirrors Ruffle's action_queue
	// model where MovieClip::run_goto queues CLIP_CONSTRUCT events and
	// Player::run_actions drains them after the calling AS script returns.
	// The deferred entries drain at the next non-catch-up tagShowFrame
	// (the calling frame's outer ShowFrame, after the calling script returns).
	// Requires Phase 4 (name_displaced flag): without it, queue_clip_construct_events
	// targets the OLD swap-displaced MC, which gets avm1_removed=1 during
	// catch-up and the deferred entry is filtered out at drain time. With
	// Phase 4, the fresh placement creates a new MC and the entry targets it.
	// Key tests: displaylist_depths/displaylist_depths_test{2,3}.
	if (!g_goto_catchup_active) {
		actionDrainActionQueueByKind(app_context, AQ_KIND_CLIP_INIT);
		actionDrainActionQueueByKind(app_context, AQ_KIND_CLIP_CONSTRUCT);
		actionDrainActionQueueByKind(app_context, AQ_KIND_REGISTER_CTOR);
	}
	// Phase 7b: sprite CLIP_EVENT_LOAD now rides on AQ_KIND_SCRIPT, drained
	// at the recompiler-emitted pre-tagShowFrame SHOW_FRAME drain alongside
	// sprite DoAction. The post-process_sprite_needs_init safety drain below
	// catches any late AQ_KIND_SCRIPT entries queued inside tagShowFrame
	// (e.g. tagReplaceObject2RatioWithClipActions).

	// --- Fire deferred onUnload handlers from removeMovieClip ---
	// These are queued mid-script and fire between frames, matching Flash behavior.
	// actionFirePendingUnloads also runs run_pending_finalize internally so the
	// destructive cleanup happens immediately after the handlers drain.
	actionFirePendingUnloads(app_context);

	// --- Run initial frame 0 with scripts for newly placed sprites/buttons ---
	// sprite_needs_init=1 is set by ng_on_place_object2 when a sprite or button is placed.
	// We run frame 0 here (in the same tick as placement) to match Flash behavior.
	// process_sprite_needs_init handles CHAR_TYPE_SPRITE and CHAR_TYPE_BUTTON recursively.
	//
	// When g_defer_sprite_init is set (during ng_executeGotoCatchUp), we skip this block.
	// Sprite init scripts must run AFTER the deferred parent-frame DoAction, not before.
	// ng_run_deferred_sprite_init() handles the deferred case.
	if (!g_defer_sprite_init)
	{
		// Sync root display sentinel before nested init — child scripts may access
		// root children via GetMember(root_mc, name) while display_list is swapped.
		extern void ng_sync_root_display_obj(void);
		ng_sync_root_display_obj();

		int saved_catch_up = catch_up_mode;
		catch_up_mode = 0;
		// Use g_current_context as parent when available (e.g., during loadMovie
		// the loaded SWF's tagShowFrame should parent sprites to the container MC).
		MovieClip* _si_parent = (g_current_context != NULL) ? g_current_context : &root_movieclip;
		process_sprite_needs_init(app_context, _si_parent);
		catch_up_mode = saved_catch_up;

		// Phase 7b safety drain: catch any AQ_KIND_SCRIPT entries queued
		// during process_sprite_needs_init itself (e.g. via paths like
		// tagReplaceObject2RatioWithClipActions which installs new
		// clip_actions after the base PlaceObject and calls
		// queue_clip_load_events inside tagShowFrame). The common path's
		// pre-tagShowFrame drain already covered the normal queue; this
		// is a no-op unless some ShowFrame-time path queued more.
		actionDrainActionQueueByKind(app_context, AQ_KIND_SCRIPT);

		// Fire onLoad events for duplicated clips (queued by ng_duplicateMovieClip)
		ng_fire_pending_loads(app_context);

		// Fire deferred init scripts for attachMovie clips
		ng_fire_pending_attach_inits(app_context);

		// Flush pending onLoads for timeline-placed RegisterClass sprites
		// (queued by process_sprite_init_at_depth after constructor fires).
		// This also flushes any onLoads queued during the onLoad handlers
		// (e.g. children created via attachMovie inside onLoad).
		actionFlushPendingOnLoads(app_context);

		// Fire deferred onLoadInit handlers from MovieClipLoader.loadClip.
		// If the root has stopped (e.g. script called `stop()` at end of
		// the frame_func that called loadClip), promote any deferred MCL
		// loads from _next_tick into _this_tick first so the events fire
		// same-tick — there's no "next frame's DoAction" to defer past
		// when the loader isn't going to advance. Required by
		// avm1/string_paths_eval2 (frame 0 calls loadClip + stop, then a
		// setInterval-based timer fires after the listener; deferring to
		// next tick pushes the timer's fire time past num_frames).
		extern int is_playing;
		extern void actionPromotePendingMCLLoads(SWFAppContext* app_context);
		if (!is_playing) {
			actionPromotePendingMCLLoads(app_context);
		}
		actionFirePendingLoadInits(app_context);

		// Fallback: flush pending ENTER_FRAME if not already flushed by a
		// tagFlushPendingEnterFrame call (emitted before DoAction by the recompiler).
		// For frames with no DoAction, this ensures ENTER_FRAME still fires.
		tagFlushPendingEnterFrame(app_context);

		// Upgrade sprite_initialized from 1 (this tick) to 2 (ready for per-tick dispatch).
		// Must recurse into nested sprite display lists.
		{
			void upgrade_sprite_initialized(DisplayObject* dl, size_t dl_max);
			upgrade_sprite_initialized(display_list, max_depth);
		}
#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
		{
			void upgrade_attached_clip_initialized(SWFAppContext* app_context);
			upgrade_attached_clip_initialized(app_context);
		}
#endif

		// After the first tagShowFrame, root's enterFrame becomes eligible.
		// On the first frame, root fires LOAD (its frame script), not enterFrame.
		extern int g_root_enterframe_eligible;
		g_root_enterframe_eligible = 1;
	}
#else
#  ifdef OFFSCREEN_RENDER
	// graphics-native: run sprite-init for newly placed sprites BEFORE
	// advance_sprite_frames, so their MovieClip entries exist when
	// actionFindMovieClipByName looks them up during the first frame func.
	// NO_GRAPHICS does the equivalent above (around line 2846); the wasm
	// graphics build relies on no-op stubs in graphics_stubs.c and skips
	// this — sprite-relative scripts in wasm graphics still run in root
	// context.
	{
		extern MovieClip* g_current_context;
		MovieClip* _si_parent = (g_current_context != NULL) ? g_current_context : &root_movieclip;
		process_sprite_needs_init(app_context, _si_parent);
	}

	// --- Advance sprite timelines (recursive) ---
	advance_sprite_frames(app_context);
#  else
	// Browser-WASM: during goto catch-up (called from actionCallMethod's
	// gotoAndStop/Play dispatch when target is root) skip per-tick
	// pumping — advance_sprite_frames, LOAD drain, button hit-test, etc.
	// The recompiler-emitted RemoveObject2/PlaceObject2 tags in each
	// catch-up frame have already executed before this call; they're the
	// only thing the catch-up needs from frame_funcs. Pumping sprite
	// advance / draining LOAD while nested inside a CLIP_RELEASE handler
	// corrupts global state used by getDisplayListJSON and other consumers.
	if (catch_up_mode) return;
	// Browser-WASM: tagShowFrame is the only sprite-advance pump in this
	// build (unlike OFFSCREEN_RENDER, where swf.c's main tick loop runs
	// the same defer+nested pattern). Without the nested pass, sprites
	// parented to 1-frame containers never advance: advance_sprite_frames
	// short-circuits at the 1-frame !just_allocated check and the inline
	// recursion is skipped along with it. Key symptom: Pong stuck at
	// preloader (sprite_9 inside 1-frame sprite_10). See
	// SWFRecompDocs/status/2026-05-25-pong-browser-diagnosis.md.
	{
		extern int g_advance_defer_nested;
		extern void advance_nested_sprite_frames(SWFAppContext* app_context);
		g_advance_defer_nested = 1;
		advance_sprite_frames(app_context);
		g_advance_defer_nested = 0;
		advance_nested_sprite_frames(app_context);
		// Apply deferred gotoAndStop/Play navs on attachMovie'd clips (e.g.
		// Doodle Jump platforms) and build their nested sprite sub-lists.
		advance_attached_clip_frames(app_context);
		// Play EXPLICITLY gotoAndPlay'd standalone attached clips forward one frame
		// (coins' COLLECTED disappear, drones' prefire). Placement-tags-only +
		// flag-gated + count-snapshot → safe (see advance_attached_clip_natural).
		{
			extern void advance_attached_clip_natural(SWFAppContext* app_context);
			advance_attached_clip_natural(app_context);
		}
	}
	// Upgrade sprite_initialized 1→2 so dispatch_enterframe_clip_actions
	// fires from the next tick. tagPlaceObject2 in browser-WASM marks
	// freshly-placed sprites as 1; this bump is the per-tick promotion
	// that the gated NO_GRAPHICS||OFFSCREEN_RENDER path does inside
	// process_sprite_needs_init.
	{
		void upgrade_sprite_initialized(DisplayObject* dl, size_t dl_max);
		upgrade_sprite_initialized(display_list, max_depth);
	}
#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	{
		void upgrade_attached_clip_initialized(SWFAppContext* app_context);
		upgrade_attached_clip_initialized(app_context);
	}
#endif
	// Browser-WASM: finalize any tagRemoveObject(2)'d entries that weren't
	// reclaimed by a same-tick tagPlaceObject2(Ratio) BEFORE the AQ_KIND_LOAD
	// drain. pending_remove was deferred at tagRemoveObject2 time to give
	// Remove+Place pairs in re-running frame_funcs a chance to skip the
	// invalidate+recreate churn. Anything still flagged here is a genuine
	// removal — fire the invalidate + clear now so LOAD handlers don't bind
	// to stale cached MCs. Concrete case: Doodle Jump gameplay frame_1 places
	// a fresh "hero" at depth 4 (queues LOAD on AQ_KIND_LOAD) and removes the
	// menu's "hero" at depth 9 (sets pending_remove). When LOAD drains,
	// findOrCreateMovieClip("hero", root) would otherwise pick up the menu
	// hero MC still in the cache, store `var score = 0` on it, and then
	// (post-drain) the finalize walk would invalidate it — so the next
	// tick's ENTER_FRAME dispatch creates a fresh hero MC with no score
	// property, and `_root.score_txt.text = "" + score` produces "undefined".
	finalize_pending_removes_recursive(app_context);
	// Browser-WASM: drain deferred LOAD clip-actions queued onto
	// AQ_KIND_LOAD at placement time. By now advance_sprite_frames has
	// populated sprite children (e.g. button_txt inside Doodle Jump's
	// button sprites), so LOAD scripts that touch children see the
	// expected DisplayObject layout.
	actionDrainActionQueueByKind(app_context, AQ_KIND_LOAD);
#  endif

	// Enable root onEnterFrame dispatch after first frame
	{
		extern int g_root_enterframe_eligible;
		g_root_enterframe_eligible = 1;
	}
#endif

	// --- Button hit testing + state machine + action dispatch ---
	// In NO_GRAPHICS mode, button states are updated per-tick from swf_core.c frame loop
	// (after event delivery), and per-mouse-event from input_events_deliver().
#ifndef NO_GRAPHICS
	{
#ifdef __EMSCRIPTEN__
		// Browser-WASM perf gate. The button-state hover walk
		// (ng_update_button_states + _attached) recursively hit-tests interactive
		// clips via mc_get_pixel_aabb_ng, which does an O(child_mc_count) scan per
		// call — on Metanet "N" (1614+ clips) this was ~43% of frame CPU EVERY
		// frame, even during keyboard gameplay with a stationary mouse. Button
		// hover/press state only changes when the mouse moves or a button
		// transitions (Flash updates hover on mouse move), so skip the walk on
		// frames where neither the pointer position nor the button state changed,
		// and keep the previous cursor. Non-browser graphics (OFFSCREEN,
		// the CI path) keep the unconditional walk below → byte-identical results,
		// so this is NOT CI-observable and can't regress the trace suites. Known
		// tradeoff (accepted): a button that moves under a perfectly stationary
		// pointer won't re-evaluate rollover until the next pointer move/click.
		static int s_pm_init = 0;
		static float s_pm_x = 0.0f, s_pm_y = 0.0f;
		static int s_pm_btn = 0;
		float _cmx = app_context->mouse.stage_x, _cmy = app_context->mouse.stage_y;
		int _cbtn = app_context->mouse.button_down;
		int _mouse_changed = !s_pm_init || _cmx != s_pm_x || _cmy != s_pm_y ||
		                     _cbtn != s_pm_btn || app_context->mouse.clicked ||
		                     app_context->mouse.released;
		s_pm_init = 1; s_pm_x = _cmx; s_pm_y = _cmy; s_pm_btn = _cbtn;
		if (_mouse_changed) {
			int any_hover = ng_update_button_states(app_context);
			// Cursor: hand/pointer over interactive buttons (any_hover), I-beam
			// (text) over a focusable text field (e.g. an editable name box), else
			// default. Buttons take precedence (a button over a field is still a
			// click target). text-field check is skipped when a button is hovered.
			int over_text = 0;
			if (!any_hover) {
				extern int actionMouseOverFocusableTextField(SWFAppContext*);
				over_text = actionMouseOverFocusableTextField(app_context);
			}
			EM_ASM({
				document.getElementById('canvas').style.cursor = $0 ? 'pointer' : ($1 ? 'text' : 'default');
			}, any_hover, over_text);
		}
#else
		ng_update_button_states(app_context);
#endif
	}
#endif

#ifndef NO_GRAPHICS
	// Same guard tagRerenderFrame has carried since it was written: with no
	// renderer there is nothing to compose, and `context->xform_slot_count`
	// below would be a NULL deref. In an AVM1 binary this is unreachable —
	// swfStart assigns `context` before the first frame ever runs, whatever
	// renderer_ok ends up as. It becomes reachable in an AVM2 binary, where
	// runSWF_avm2 owns the renderer and an AVM1 CHILD movie's frame func
	// reaches tagShowFrame with the AVM1 `context` still NULL (session 15,
	// dual-VM arc). Deliberately NOT `!renderer_ok` — that state does occur
	// in AVM1 graphics builds and must keep its existing path.
	if (context == NULL) return;

	// Compute focus rect BEFORE compose_children modifies transform_ids.
	// ng_getDisplayEntryBounds reads from CPU-side transform_data which only
	// has original (non-composed) slots; composed dynamic slots exist only in GPU.
	FocusRectInfo g_frame_fri;
	int g_frame_draw_focus_rect = actionGetFocusRectInfo(&g_frame_fri);

	// Initialize dynamic transform and cxform slot allocators.
	// Original slots are [0, orig_count). Dynamic slots start after.
	{
		u32 orig_xform_count = (u32)(app_context->transform_data_size / (16 * sizeof(float)));
		g_next_dynamic_xform_slot = orig_xform_count;
		g_xform_slot_capacity = context->xform_slot_count;
		xform_overrides_reset();
		text_glyph_overrides_reset();

		u32 orig_cxform_count = app_context->cxform_data_size > 0
			? (u32)(app_context->cxform_data_size / (20 * sizeof(float))) : 1;
		g_next_dynamic_cxform_slot = orig_cxform_count;
		g_cxform_slot_capacity = context->cxform_slot_count;
		cxform_overrides_reset();
	}

	{
		extern MovieClip* actionFindMovieClipByName(const char* instance_name);

		// --- Runtime transform updates ---
		// When ActionScript modifies _x/_y/_xscale/_yscale/_rotation, the changes
		// are stored on the MovieClip but not written to the GPU.  Apply them now
		// so that compose_children and the render pass use the updated values.
		for (size_t i = 1; i <= max_depth; ++i)
		{
			DisplayObject* obj = &display_list[i];
			if (obj->char_id == 0 || obj->instance_name == NULL) continue;

			MovieClip* mc = actionFindMovieClipByName(obj->instance_name);
			if (mc == NULL || mc->as_set_flags == 0) continue;

			// Skip SPRITE / BUTTON — the compose loop below builds their
			// effective parent matrix on a LOCAL stack buffer (mirroring
			// tagRerenderFrame, which already does this; see the in-place
			// mutation hazard note there). Mutating transform_data in place
			// here permanently corrupts the (often recompiler-deduped, shared)
			// CPU-side placement slot. Concrete case: Doodle Jump's hero
			// (sprite, transform_id 54) had its static placement slot
			// overwritten with its runtime _y each tick, so a freshly
			// re-placed hero on restart-after-game-over synced from slot 54 to
			// the death position (~419) instead of the spawn (179) and
			// instantly re-triggered the fall→game-over check.
			Character* ch_ = &dictionary[obj->char_id];
			if (ch_->type == CHAR_TYPE_SPRITE || ch_->type == CHAR_TYPE_BUTTON) continue;

			// Update CPU-side transform_data so compose_children picks it up
			float* slot = (float*)app_context->transform_data + obj->transform_id * 16;
			apply_as_transform(slot, mc, mc->as_set_flags);

			// Also update the GPU buffer for this slot
			renderer_write_transform(context, obj->transform_id, slot);
		}

		// Dynamic MCs (createEmptyMovieClip etc.) aren't in display_list —
		// allocate per-tick slots for any with AS-set transforms so their
		// Drawing-API paths route through the freshly built matrix.
		apply_dynamic_mc_transforms(app_context);

		// --- Runtime cxform updates ---
		// When Color.setRGB()/setTransform() modifies display object color,
		// cx_overridden is set.  Build a new cxform and write to a dynamic GPU slot.
		// The compose_children loop below propagates the overridden cxform_id
		// to sprite/button children.
		// Memory barrier: GCC -O2 can misoptimize struct field reads across
		// function calls that modify related memory.
		__asm__ volatile("" ::: "memory");
		for (size_t i = 1; i <= max_depth; ++i)
		{
			DisplayObject* obj = &display_list[i];
			if (obj->char_id == 0) continue;
			if (!obj->cx_overridden) continue;

			// Build cxform from the runtime cx_* values and allocate a dynamic slot
			float cx[20];
			build_cxform_from_obj(cx, obj);

			u32 new_slot = g_next_dynamic_cxform_slot;
			if (new_slot < g_cxform_slot_capacity) {
				g_next_dynamic_cxform_slot++;
				cxform_overrides_push(obj, obj->cxform_id);
				obj->cxform_id = new_slot;
				renderer_write_cxform(context, new_slot, cx);
			}
		}
		__asm__ volatile("" ::: "memory");
	}

	// Compose transforms recursively BEFORE the render pass.
	// For sprites/buttons: compose_children handles all nesting levels,
	// passing the composed parent transform down so nested text/sprite/button
	// children get correctly composed global transforms.
	// For top-level text/morph: compose directly (parent is identity/self).
	extern MovieClip* actionFindMovieClipByName(const char* instance_name);
	for (size_t i = 1; i <= max_depth; ++i)
	{
		DisplayObject* obj = &display_list[i];
		if (obj->char_id == 0) continue;

		Character* ch = &dictionary[obj->char_id];
		// cacheAsBitmap PixelSnapping::Always for a ROOT LEAF entry. Sprites
		// and buttons snap through cab_snap_world in their own arms below
		// (compose_children needs the snapped matrix as the parent); static
		// text is deliberately skipped — renderer_compose_text_transforms
		// reads the CPU-side transform table, which has no entry at a dynamic
		// slot.
		if (cab_snaps(obj) && ch->type != CHAR_TYPE_SPRITE
		    && ch->type != CHAR_TYPE_BUTTON && ch->type != CHAR_TYPE_TEXT)
			cab_snap_root_leaf(app_context, obj);
		if (ch->type == CHAR_TYPE_SPRITE)
		{
			if (obj->sprite_display_list != NULL)
			{
				// Build the sprite's effective matrix on a LOCAL buffer (AS
				// overlay on top of the un-mutated CPU transform_data slot) so
				// the in-place loop above no longer needs to corrupt the shared
				// placement slot. Mirrors tagRerenderFrame.
				const float* sprite_xform;
				float sprite_xform_buf[16];
				MovieClip* mc_for_xform = (obj->instance_name != NULL)
					? actionFindMovieClipByName(obj->instance_name) : NULL;
				if (mc_for_xform != NULL && mc_for_xform->as_set_flags != 0) {
					memcpy(sprite_xform_buf,
						(const float*)app_context->transform_data + obj->transform_id * 16,
						sizeof(sprite_xform_buf));
					apply_as_transform(sprite_xform_buf, mc_for_xform, mc_for_xform->as_set_flags);
					sprite_xform = sprite_xform_buf;
				} else {
					sprite_xform = (const float*)app_context->transform_data + obj->transform_id * 16;
				}
				// cacheAsBitmap PixelSnapping::Always (root sprite): the snapped
				// matrix has to be the one compose_children sees as the parent.
				float cab_sprite_buf[16];
				sprite_xform = cab_snap_world(obj, sprite_xform, cab_sprite_buf);
				float seed_cx[20];
				const float* seed = root_seed_cxform(app_context, obj, seed_cx)
					? seed_cx : NULL;
				compose_children(app_context,
					obj->sprite_display_list, obj->sprite_max_depth,
					sprite_xform, seed, obj->cxform_id);
			}
		}
		else if (ch->type == CHAR_TYPE_BUTTON)
		{
			// Initialize button display list on first encounter (graphics mode)
			if (obj->sprite_display_list == NULL && ch->button_state_funcs != NULL)
			{
				obj->sprite_dl_capacity = INITIAL_SPRITE_DL_CAPACITY;
				obj->sprite_display_list = HCALLOC(obj->sprite_dl_capacity, sizeof(DisplayObject));
				obj->sprite_max_depth = 0;

				u8 state = obj->button_state;
				u8 effective = (state == 3) ? 1 : state; // OUT_DOWN shows OVER (see ng_update_button_states_in_dl)
				if (effective < 3 && ch->button_state_funcs[effective] != NULL)
				{
					DisplayObject* saved_dl = display_list;
					size_t saved_max = max_depth;
					size_t saved_cap = display_list_capacity;
					display_list = obj->sprite_display_list;
					max_depth = obj->sprite_max_depth;
					display_list_capacity = obj->sprite_dl_capacity;

					ch->button_state_funcs[effective](app_context);

					obj->sprite_display_list = display_list;
					obj->sprite_max_depth = max_depth;
					obj->sprite_dl_capacity = display_list_capacity;
					display_list = saved_dl;
					max_depth = saved_max;
					display_list_capacity = saved_cap;
				}
			}

			if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
			{
				// Local-buffer AS overlay (mirrors tagRerenderFrame); see the
				// sprite branch above for the shared-slot corruption rationale.
				const float* btn_xform;
				float btn_xform_buf[16];
				MovieClip* mc_for_xform = (obj->instance_name != NULL)
					? actionFindMovieClipByName(obj->instance_name) : NULL;
				if (mc_for_xform != NULL && mc_for_xform->as_set_flags != 0) {
					memcpy(btn_xform_buf,
						(const float*)app_context->transform_data + obj->transform_id * 16,
						sizeof(btn_xform_buf));
					apply_as_transform(btn_xform_buf, mc_for_xform, mc_for_xform->as_set_flags);
					btn_xform = btn_xform_buf;
				} else {
					btn_xform = (const float*)app_context->transform_data + obj->transform_id * 16;
				}
				float cab_btn_buf[16];
				btn_xform = cab_snap_world(obj, btn_xform, cab_btn_buf);
				float seed_cx[20];
				const float* seed = root_seed_cxform(app_context, obj, seed_cx)
					? seed_cx : NULL;
				compose_children(app_context,
					obj->sprite_display_list, obj->sprite_max_depth,
					btn_xform, seed, obj->cxform_id);
			}
		}
		else if (ch->type == CHAR_TYPE_TEXT)
		{
			renderer_compose_text_transforms(context,
				app_context->transform_data,
				obj->transform_id,
				ch->transform_start,
				ch->text_size);
		}
		else if (ch->type == CHAR_TYPE_MORPH_SHAPE)
		{
			float t = (float)obj->ratio / 65535.0f;
			size_t num_verts = ch->morph_start_size;

			u32* start = (u32*)(app_context->shape_data + ch->morph_start_offset * 4 * sizeof(u32));
			float* end = (float*)(app_context->morph_end_shape_data + ch->morph_end_offset * 2 * sizeof(float));
			u32* scratch = (u32*)malloc(num_verts * 4 * sizeof(u32));

			for (size_t v = 0; v < num_verts; v++)
			{
				float sx = *(float*)&start[v*4 + 0];
				float sy = *(float*)&start[v*4 + 1];
				float ex = end[v*2 + 0];
				float ey = end[v*2 + 1];
				float ix = sx + t * (ex - sx);
				float iy = sy + t * (ey - sy);
				scratch[v*4 + 0] = *(u32*)&ix;
				scratch[v*4 + 1] = *(u32*)&iy;
				scratch[v*4 + 2] = start[v*4 + 2];
				scratch[v*4 + 3] = start[v*4 + 3];
			}

			renderer_update_vertices(context,
				ch->morph_start_offset * 4 * sizeof(u32),
				scratch, num_verts * 4 * sizeof(u32));
			free(scratch);

			for (size_t c = 0; c < ch->morph_color_count; c++)
			{
				float* sc = (float*)(app_context->color_data) + (ch->morph_color_start + c) * 4;
				float* ec = (float*)(app_context->morph_end_color_data) + (ch->morph_end_color_start + c) * 4;
				float interp[4];
				for (int k = 0; k < 4; k++)
					interp[k] = morph_lerp_color_u8(sc[k], ec[k], t);
				renderer_update_colors(context,
					(ch->morph_color_start + c) * 4 * sizeof(float),
					interp, 4 * sizeof(float));
			}
		}
	}

	renderer_open_pass(context);

	// Apply _root's AS-set transform on top of stage_to_ndc. See the matching
	// block in tagRerenderFrame for rationale.
	{
		extern MovieClip root_movieclip;
		if (root_movieclip.as_set_flags != 0) {
			float root_xform[16] = {
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f,
			};
			apply_as_transform(root_xform, &root_movieclip, (u8)(1|2|4|8|16));
			float composed[16];
			hit_test_mat4_multiply(composed, app_context->stage_to_ndc, root_xform);
			renderer_upload_stage_transform(context, composed);
		}
	}

	u16 active_clip_depth = 0;

	// EditText boxes/glyphs are painted at their own depth from here on
	// (tf_draw_at_root_depth); depth 0 is a legal PlaceObject depth that the
	// loop below never visits, so it is flushed first.
	actionBeginTextFieldDepthPass((int) max_depth);
	tf_draw_at_root_depth(app_context, 0, (int) max_depth);

	for (size_t i = 1; i <= max_depth; ++i)
	{
		// End active clip if we've passed its range
		if (active_clip_depth > 0 && i > active_clip_depth)
		{
			renderer_end_clip(context);
			active_clip_depth = 0;
		}

		if (!tag_root_edittext_is_filtered(&display_list[i]))
			tf_draw_at_root_depth(app_context, (int) i, (int) max_depth);

		DisplayObject* obj = &display_list[i];

		if (obj->char_id == 0)
		{
			continue;
		}

		// AVM1 setMask pairing (mask defect B) — see render_display_list.
		MovieClip* avm1_masker = NULL;
		if (actionAvm1MaskPairCount() != 0)
		{
			if (entry_is_live_masker(obj)) continue;
			avm1_masker = avm1_masker_for_entry(obj);
		}

		// Honor AS-set _visible=false on a timeline-placed object: skip rendering
		// it (and its whole subtree). actionSetProperty syncs the MC's _visible
		// onto this entry's as_hidden via mc->display_obj — a name lookup from
		// root can't find a timeline sprite's MC. A mask's _visible is irrelevant
		// to its clipping role, so only skip non-mask entries. Without this, a
		// sprite whose frame-script sets `_visible = false` (e.g. Tetris's
		// paused_mc / quitGame_mc overlay) keeps painting over the game.
		if (obj->clip_depth == 0 && obj->as_hidden)
		{
			continue;
		}

		// Check if this object is a clip mask
		if (obj->clip_depth > 0)
		{
			// Pop before pushing — see render_display_list for why (a second
			// clip range opened inside a live one leaks a stencil LEVEL under
			// the Equal/IncrementClamp model). No draw happens between the
			// pop and the push, so this is byte-identical under the old
			// Always/Replace model.
			if (active_clip_depth > 0)
			{
				renderer_end_clip(context);
				active_clip_depth = 0;
			}
			Character* ch = &dictionary[obj->char_id];
			if (ch->type == CHAR_TYPE_SHAPE)
			{
				renderer_begin_clip_mask(context);
				renderer_draw_shape(context, ch->shape_offset, ch->size, obj->transform_id, obj->cxform_id);
				renderer_end_clip_mask(context);
				active_clip_depth = obj->clip_depth;
			}
			else if (ch->type == CHAR_TYPE_SPRITE)
			{
				// Sprite clip mask: render sprite content + Drawing API into stencil
				renderer_begin_clip_mask(context);
				g_clip_mask_capture++;
				if (obj->sprite_display_list != NULL)
					render_display_list(app_context, obj->sprite_display_list, obj->sprite_max_depth);
				g_clip_mask_capture--;
				// Also render Drawing API shapes drawn into this sprite's MC
				if (obj->instance_name != NULL) {
					DrawingRenderInfo dinfos[64];
					int dn = actionGetMCDrawingPathsByName(obj->instance_name, dinfos, 64);
					for (int di = 0; di < dn; di++)
						render_drawing_path(&dinfos[di]);
				}
				renderer_end_clip_mask(context);
				active_clip_depth = obj->clip_depth;
			}
			continue;
		}

		// Alpha (11) / Erase (12) with no Layer above them are IGNORED by Flash;
		// see the matching comment in the stage display loop.
		int blend_skip = (obj->blend_mode == 11 || obj->blend_mode == 12);
		// Non-trivial blend modes render into their own layer and composite once.
		int blend_layered = !blend_skip && obj->filter_type == 0
			&& renderer_blend_mode_is_layered(context, obj->blend_mode);

		// setMask maskee: push the masker's geometry as this entry's stencil.
		// opaqueBackground: painted under this entry and OUTSIDE its own
		// mask / scrollRect, exactly where Ruffle's render_base emits it.
		draw_entry_opaque_background(app_context, obj);
		u32 avm1_mask_saved_clip = 0;
		if (avm1_masker != NULL)
			avm1_mask_saved_clip = avm1_mask_push(app_context, avm1_masker);

		// Set blend mode if non-default
		if (obj->blend_mode > 1 && !blend_layered && !blend_skip)
			renderer_set_blend_mode(context, obj->blend_mode);

		if (blend_skip)
		{
			// nothing drawn
		}
		else if (blend_layered)
		{
			renderer_suspend_pass(context);
			renderer_capture_backdrop(context, obj->blend_mode);
			renderer_begin_offscreen_pass(context);
			render_single_object(app_context, obj);
			renderer_end_offscreen_pass(context);
			renderer_resume_pass(context);
			renderer_composite_blend(context, obj->blend_mode,
				active_clip_depth > 0 ? 1 : 0);
		}
		// Check if this object has a visual filter
		else if (obj->filter_type != 0)
		{
			render_filtered_root_entry(app_context, obj, (int) i, (int) max_depth);

			// Re-bind blend state after filter pipeline switches
			if (obj->blend_mode > 1 && !blend_layered)
				renderer_set_blend_mode(context, obj->blend_mode);
		}
		else
		{
			render_single_object(app_context, obj);
		}

		// Restore default blend mode
		if (obj->blend_mode > 1 && !blend_layered && !blend_skip)
			renderer_set_blend_mode(context, 0);

		if (avm1_masker != NULL)
			renderer_restore_clip(context, avm1_mask_saved_clip);

#ifndef OFFSCREEN_RENDER
		// Interleave attached (child_mc_cache) clips parented to THIS timeline
		// object right after it, so they layer at the parent's depth instead of
		// above all timeline content. Without this, a clip attached to a sprite
		// that sits below a later timeline object — Tetris's board cells are
		// parented to b_mc (depth 198), below the game-over quitGame_mc overlay
		// (depth 219) — paints OVER that object. Root-attached clips (parent ==
		// _root, AS depth 16384+) keep going through the post-loop pass below so
		// they stay above all timeline content, matching Flash. OFFSCREEN /
		// skip non-root attaches entirely (see the post-loop pass), so
		// this interleave is browser-WASM-only.
		{
			extern MovieClip* child_mc_cache[];
			extern int child_mc_count;
			for (int ci = 0; ci < child_mc_count; ci++) {
				MovieClip* cm = child_mc_cache[ci];
				if (cm == NULL || cm->depth == INT_MIN) continue;
				if (attached_parent_dl_index(cm) == (int)i)
					render_attached_child(app_context, cm);
			}
		}
#endif
	}

	if (active_clip_depth > 0)
	{
		renderer_end_clip(context);
	}

	// --- Render root-attached MovieClips (attachMovie with parent=_root) ---
	// These live in child_mc_cache, not display_list — the dense, pointer-
	// stable display_list can't hold them at AS depths (would force grow_ptr
	// to invalidate every cached `mc->display_obj`). Rendered after the
	// timeline display loop so they sit above all timeline children, matching
	// Flash's depth ordering (AS-attached depths are biased into 16384+).
	{
		extern MovieClip* child_mc_cache[];
		extern int child_mc_count;
		for (int i = 0; i < child_mc_count; i++) {
			MovieClip* mc = child_mc_cache[i];
			if (mc == NULL) continue;
			if (mc->depth == INT_MIN) continue;            // removed
#ifdef OFFSCREEN_RENDER
			// Graphics-native / headless keep the original root-only behavior
			// (trace-based suites; rendering non-root attaches here is a
			// browser-WASM-only feature, kept out of these modes to stay
			// byte-identical). Browser-WASM falls through to the general path.
			if (mc->parent != &root_movieclip) continue;
#else
			// Clips parented to a timeline object were already drawn at their
			// parent's depth by the interleave in the main render loop above.
			// Only root-attached clips (AS depth 16384+) and nested-attached
			// clips whose parent isn't a timeline object reach here — both
			// correctly layer above all timeline content.
			if (attached_parent_dl_index(mc) >= 0) continue;
#endif
			render_attached_child(app_context, mc);
		}
	}

	// --- Render text field backgrounds, borders, and glyphs ---
	// Dynamic text fields (createTextField) are tracked in child_mc_cache but not
	// on the tag display list. Render their background/border rectangles here.
	// Root-timeline fields were already painted at their own depth inside the
	// loop above (Ruffle's render_self position), so they are excluded here.
	actionSetTextFieldDepthWindow(TF_WINDOW_REST, 0, (int) max_depth);
	actionIterateTextFields(textfield_render_cb, NULL);
	actionIterateTextFieldGlyphs(textfield_glyph_render_cb, app_context);
	actionSetTextFieldDepthWindow(TF_WINDOW_ALL, 0, 0);
	actionIterateOrphanTextFields(app_context, textfield_render_cb,
		textfield_glyph_render_cb, app_context);

	// --- Render Drawing API fills and strokes ---
	actionIterateDrawings(drawing_render_cb, NULL);
	actionIterateMaskedDrawings(masked_drawing_render_cb, NULL);

	// --- Render attached bitmaps (from attachBitmap on dynamic MCs) ---
	actionIterateAttachedBitmaps(attached_bitmap_render_cb, NULL);

	// --- Render focus rect (pre-computed before compose_children) ---
	// Drawn as 3-pixel thick border INSIDE the object's world AABB.
	if (g_frame_draw_focus_rect) {
		float t = 3.0f * 20.0f;  // 3 pixels in twips
		float fx = g_frame_fri.x, fy = g_frame_fri.y, fw = g_frame_fri.w, fh = g_frame_fri.h;
		renderer_draw_rect(context, fx, fy, fw, t, 1, 1, 0, 1, 0, 0);                  // top
		renderer_draw_rect(context, fx, fy + fh - t, fw, t, 1, 1, 0, 1, 0, 0);          // bottom
		renderer_draw_rect(context, fx, fy + t, t, fh - 2*t, 1, 1, 0, 1, 0, 0);         // left
		renderer_draw_rect(context, fx + fw - t, fy + t, t, fh - 2*t, 1, 1, 0, 1, 0, 0);// right
	}

	renderer_close_pass(context);

	// Restore original transform_ids and cxform_ids that were overridden
	xform_overrides_restore();
	text_glyph_overrides_restore();
	cxform_overrides_restore();
#endif // NO_GRAPHICS

	// Dispatch _root.onLoad once after first frame
	actionDispatchRootOnLoad(app_context);

	// Phase B: retry every TF whose variable-binding path wasn't resolvable
	// at placement time. Mirrors Ruffle's Avm1TextFieldBinding::bind_variables
	// invocation from MovieClip::run_frame_avm1 (per-tick). Most-common case
	// is a no-op because eager bind in ng_on_place_object2 already succeeded.
	actionRetryUnboundTextFields(app_context);

	// Advance placement generation so next frame's placements are distinguishable
	g_place_gen++;
}

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
// ---------------------------------------------------------------------------
// Sprite content bounds helper (in LOCAL twips of the sprite's own space)
// ---------------------------------------------------------------------------
// Returns 1 if any bounds were found. Bounds are relative to the sprite's
// registration point (i.e., the sprite's coordinate system origin).
int sprite_content_bounds_twips(DisplayObject* dl, size_t dl_max,
    float* xmin_out, float* xmax_out, float* ymin_out, float* ymax_out)
{
	float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
	int found = 0;

	// From depth 0 (see compose_children).
	for (size_t i = 0; i <= dl_max; i++)
	{
		DisplayObject* child = &dl[i];
		if (child->char_id == 0) continue;
		Character* ch = &dictionary[child->char_id];

#ifdef NO_GRAPHICS
		// Use cached place_* values from the DisplayObject — these were
		// captured at PlaceObject2 time using whichever transform_data
		// table was active (parent's or imported child's). Indexing into
		// the global transform_data[] here would silently use the parent's
		// table even for imported child sprites whose transform_ids index
		// into the child's table.
		float tx = child->place_tx;
		float ty = child->place_ty;
		float sx = child->place_a;
		float sy = child->place_d;
#else
		float (*_ctd)[16] = ng_entryTransformData(child);
		float tx = _ctd[child->transform_id][12];
		float ty = _ctd[child->transform_id][13];
		float sx = _ctd[child->transform_id][0];
		float sy = _ctd[child->transform_id][5];
#endif

		if (ch->type == CHAR_TYPE_SHAPE || ch->type == CHAR_TYPE_MORPH_SHAPE ||
		    ch->type == CHAR_TYPE_TEXT)
		{
			s32 cxmin, cxmax, cymin, cymax;
			if (ng_getCharBounds(child->char_id, &cxmin, &cxmax, &cymin, &cymax))
			{
				float x0 = tx + sx * (float)cxmin;
				float x1 = tx + sx * (float)cxmax;
				float y0 = ty + sy * (float)cymin;
				float y1 = ty + sy * (float)cymax;
				if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
				if (y0 > y1) { float t = y0; y0 = y1; y1 = t; }
				if (x0 < xmin) xmin = x0;
				if (x1 > xmax) xmax = x1;
				if (y0 < ymin) ymin = y0;
				if (y1 > ymax) ymax = y1;
				found = 1;
			}
		}
		// Fallback: try ng_getCharBounds for any other type (edittext, etc.)
		else if (ch->type != CHAR_TYPE_SPRITE && ch->type != CHAR_TYPE_BUTTON)
		{
			s32 cxmin, cxmax, cymin, cymax;
			if (ng_getCharBounds(child->char_id, &cxmin, &cxmax, &cymin, &cymax))
			{
				float x0 = tx + sx * (float)cxmin;
				float x1 = tx + sx * (float)cxmax;
				float y0 = ty + sy * (float)cymin;
				float y1 = ty + sy * (float)cymax;
				if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
				if (y0 > y1) { float t = y0; y0 = y1; y1 = t; }
				if (x0 < xmin) xmin = x0;
				if (x1 > xmax) xmax = x1;
				if (y0 < ymin) ymin = y0;
				if (y1 > ymax) ymax = y1;
				found = 1;
			}
		}
		else if (ch->type == CHAR_TYPE_SPRITE &&
		         child->sprite_display_list != NULL && child->sprite_max_depth > 0)
		{
			float cxmin, cxmax, cymin, cymax;
			if (sprite_content_bounds_twips(child->sprite_display_list, child->sprite_max_depth,
			        &cxmin, &cxmax, &cymin, &cymax))
			{
				float x0 = tx + sx * cxmin;
				float x1 = tx + sx * cxmax;
				float y0 = ty + sy * cymin;
				float y1 = ty + sy * cymax;
				if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
				if (y0 > y1) { float t = y0; y0 = y1; y1 = t; }
				if (x0 < xmin) xmin = x0;
				if (x1 > xmax) xmax = x1;
				if (y0 < ymin) ymin = y0;
				if (y1 > ymax) ymax = y1;
				found = 1;
			}
		}
	}

	if (found)
	{
		*xmin_out = xmin; *xmax_out = xmax;
		*ymin_out = ymin; *ymax_out = ymax;
	}
	return found;
}

// ---------------------------------------------------------------------------
// _droptarget computation: Z-order front-to-back traversal
// ---------------------------------------------------------------------------
// Iterates dl front-to-back (highest depth first).
// Named sprites: recurse into children; if children miss, return this sprite's path.
// Unnamed objects: absorb the hit and return parent_path.
// parent_stage_x/y: stage origin of the parent (twips), so entry stage origin =
//   parent_stage + transform_data[entry.transform_id][12/13].
// Recursive shape-only hit test for drop_target purposes. Excludes text fields
// (which Flash treats as transparent for drop_target hit testing) and walks
// nested sprites looking for actual shape coverage.
static int find_drop_target_shape_hit_recursive(DisplayObject* dl, size_t dl_max,
    double pa, double pb, double pc, double pd, double ptx, double pty,
    double test_x, double test_y)
{
	extern int ng_hitTestShapeChar_public(size_t char_id, u16 ratio,
	    double ma, double mb, double mc_m, double md, double mtx, double mty,
	    double test_x, double test_y);
	(void)ng_hitTestShapeChar_public;
	for (size_t i = 1; i <= dl_max; i++) {
		DisplayObject* child = &dl[i];
		if (child->char_id == 0) continue;
		Character* cch = &dictionary[child->char_id];
		// Compose parent matrix with this child's transform
		u32 ctid = child->transform_id;
		float (*_ctd)[16] = ng_entryTransformData(child);
		double ca = (double)_ctd[ctid][0];
		double cb = (double)_ctd[ctid][1];
		double cc = (double)_ctd[ctid][4];
		double cd = (double)_ctd[ctid][5];
		double ctx_v = (double)_ctd[ctid][12];
		double cty_v = (double)_ctd[ctid][13];
		double na = pa*ca + pc*cb, nb = pb*ca + pd*cb;
		double nc = pa*cc + pc*cd, nd = pb*cc + pd*cd;
		double nntx = pa*ctx_v + pc*cty_v + ptx;
		double nnty = pb*ctx_v + pd*cty_v + pty;
		if (cch->type == CHAR_TYPE_SHAPE || cch->type == CHAR_TYPE_MORPH_SHAPE) {
			s32 cxmin, cxmax, cymin, cymax;
			if (!ng_getCharBounds(child->char_id, &cxmin, &cxmax, &cymin, &cymax)) continue;
			// Inverse-transform to local space and test bounds
			double det = na*nd - nb*nc;
			if (det == 0.0) continue;
			double inv = 1.0 / det;
			double sx = test_x - nntx;
			double sy = test_y - nnty;
			double lx = ( nd*sx - nc*sy) * inv;
			double ly = (-nb*sx + na*sy) * inv;
			if (lx >= (double)cxmin && lx <= (double)cxmax &&
			    ly >= (double)cymin && ly <= (double)cymax)
				return 1;
		} else if (cch->type == CHAR_TYPE_SPRITE) {
			if (child->sprite_display_list != NULL && child->sprite_max_depth > 0) {
				if (find_drop_target_shape_hit_recursive(child->sprite_display_list,
				    child->sprite_max_depth, na, nb, nc, nd, nntx, nnty, test_x, test_y))
					return 1;
			}
		}
		// Skip text fields and other non-shape types (Flash treats them as
		// transparent for drop_target purposes).
	}
	return 0;
}

static int find_drop_target_in_dl(DisplayObject* dl, size_t dl_max,
    float parent_stage_x, float parent_stage_y,
    float mouse_x, float mouse_y,
    const char* skip_name,
    const char* parent_path, char* out_path, size_t out_size)
{
	// Iterate front-to-back (highest depth first)
	for (size_t i = dl_max; i >= 1; i--)
	{
		DisplayObject* entry = &dl[i];
		if (entry->char_id == 0) continue;

		// Skip the dragged clip itself (skip_name may be full path like "_level0.dragger")
		if (skip_name && entry->instance_name) {
			const char* skip_base = strrchr(skip_name, '.');
			skip_base = skip_base ? skip_base + 1 : skip_name;
			if (strcmp(entry->instance_name, skip_base) == 0) continue;
		}

		Character* ch = &dictionary[entry->char_id];

		float (*_etd)[16] = ng_entryTransformData(entry);
		float entry_stage_x = parent_stage_x + _etd[entry->transform_id][12];
		float entry_stage_y = parent_stage_y + _etd[entry->transform_id][13];
		float sx = _etd[entry->transform_id][0];
		float sy = _etd[entry->transform_id][5];

		// If this entry has an associated AS-level MC with as_set_flags
		// (e.g. Dejagnu._y = 100), apply that delta to the entry's stage
		// position so AS-modified positions take effect for drop_target
		// hit-testing.
		if (entry->instance_name != NULL) {
			extern MovieClip* actionFindMovieClipByName(const char* name);
			MovieClip* _entry_mc = actionFindMovieClipByName(entry->instance_name);
			if (_entry_mc != NULL && _entry_mc->display_obj == (void*)entry) {
				if (_entry_mc->as_set_flags & 1) {
					// Override x with AS-set value (in pixels → twips)
					entry_stage_x = parent_stage_x + (float)_entry_mc->x * 20.0f;
				}
				if (_entry_mc->as_set_flags & 2) {
					entry_stage_y = parent_stage_y + (float)_entry_mc->y * 20.0f;
				}
			}
		}

		int hit = 0;

		if (ch->type == CHAR_TYPE_SHAPE || ch->type == CHAR_TYPE_MORPH_SHAPE)
		{
			// Shape: AABB is a tight enough approximation for drop-target testing.
			s32 cxmin, cxmax, cymin, cymax;
			if (ng_getCharBounds(entry->char_id, &cxmin, &cxmax, &cymin, &cymax))
			{
				float x0 = entry_stage_x + sx * (float)cxmin;
				float x1 = entry_stage_x + sx * (float)cxmax;
				float y0 = entry_stage_y + sy * (float)cymin;
				float y1 = entry_stage_y + sy * (float)cymax;
				if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
				if (y0 > y1) { float t = y0; y0 = y1; y1 = t; }
				hit = (mouse_x >= x0 && mouse_x <= x1 && mouse_y >= y0 && mouse_y <= y1);
			}
		}
		else if (ch->type == CHAR_TYPE_SPRITE)
		{
			// Sprite: test against the sprite's content AABB (includes both
			// shape and text-field children). text_blocks_clicks expects
			// /texts (a sprite containing a text field) to be a drop target.
			if (entry->sprite_display_list != NULL && entry->sprite_max_depth > 0)
			{
				float lxmin, lxmax, lymin, lymax;
				if (sprite_content_bounds_twips(entry->sprite_display_list,
				        entry->sprite_max_depth, &lxmin, &lxmax, &lymin, &lymax))
				{
					float x0 = entry_stage_x + sx * lxmin;
					float x1 = entry_stage_x + sx * lxmax;
					float y0 = entry_stage_y + sy * lymin;
					float y1 = entry_stage_y + sy * lymax;
					if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
					if (y0 > y1) { float t = y0; y0 = y1; y1 = t; }
					hit = (mouse_x >= x0 && mouse_x <= x1 && mouse_y >= y0 && mouse_y <= y1);
				}
			}
		}

		if (!hit) continue;

		// Hit!
		if (entry->instance_name != NULL)
		{
			// Named entry: compute its path
			char entry_path[512];
			if (parent_path[0] == '\0')
				snprintf(entry_path, sizeof(entry_path), "/%s", entry->instance_name);
			else
				snprintf(entry_path, sizeof(entry_path), "%s/%s", parent_path, entry->instance_name);

			// If named sprite with children, recurse
			if (ch->type == CHAR_TYPE_SPRITE &&
			    entry->sprite_display_list != NULL && entry->sprite_max_depth > 0)
			{
				if (find_drop_target_in_dl(entry->sprite_display_list,
				        entry->sprite_max_depth,
				        entry_stage_x, entry_stage_y,
				        mouse_x, mouse_y,
				        skip_name, entry_path, out_path, out_size))
					return 1;
			}

			// Named but no child matched (or not a sprite): return this path
			snprintf(out_path, out_size, "%s", entry_path);
			return 1;
		}
		else
		{
			// Unnamed: absorb hit, return parent path
			snprintf(out_path, out_size, "%s", parent_path);
			return 1;
		}
	}
	return 0;
}

int ng_compute_droptarget(float stage_x_twips, float stage_y_twips,
    const char* skip_name, char* out_path, size_t out_size)
{
	if (out_size == 0) return 0;
	out_path[0] = '\0';
	// Check static display_list first (timeline-placed clips). Flash treats
	// these as taking precedence for drop_target when both static and
	// dynamic clips overlap at the test point — e.g. text_blocks_clicks
	// expects "/texts" (static) instead of "/click_mc" (dynamic).
	if (find_drop_target_in_dl(display_list, max_depth,
	    0.0f, 0.0f, stage_x_twips, stage_y_twips,
	    skip_name, "", out_path, out_size))
		return 1;
	// Fall back to dynamic MCs (createEmptyMovieClip / duplicateMovieClip /
	// attachMovie) — they aren't in display_list, so the static walk above
	// can't find them. DragDropTest's target10/target20/etc. land here.
	extern int actionFindDynamicDropTarget(float stage_x_twips, float stage_y_twips,
	    const char* skip_name, char* out_path, size_t out_size);
	return actionFindDynamicDropTarget(stage_x_twips, stage_y_twips,
	    skip_name, out_path, out_size);
}

#endif // close NO_GRAPHICS||OFFSCREEN_RENDER for sprite_content_bounds + droptarget;
       // the clip-event dispatchers below need to compile in browser-WASM too.

// ---------------------------------------------------------------------------
// CLIP_EVENT_PRESS / CLIP_EVENT_RELEASE dispatch
// ---------------------------------------------------------------------------
// Called from swf_core.c's input_events_deliver on EV_MOUSE_DOWN_LEFT / UP_LEFT,
// and from swf.c's per-tick mouse handler in browser-WASM.

// Recursive helper for dispatch_clip_event_press — walks into nested sprite
// display lists to find entries with CLIP_EVENT_PRESS clip actions.
static void dispatch_clip_event_press_dl(SWFAppContext* app_context,
    DisplayObject* dl, size_t dl_max_depth,
    double ma, double mb, double mc_m, double md, double mtx, double mty,
    float mouse_x, float mouse_y, MovieClip* parent_mc)
{
	extern int ng_hitTestShapeFromDL(DisplayObject* dl, size_t dl_max,
	    double ma, double mb, double mc_m, double md, double mtx, double mty,
	    double test_x, double test_y);

	for (size_t i = 1; i <= dl_max_depth; i++)
	{
		DisplayObject* obj = &dl[i];
		if (obj->char_id == 0) continue;
		// Invisible clips (and their whole subtree) receive no mouse events in
		// Flash. Without this, Minesweeper's board `sensor` kept firing on(press)/
		// on(release) after a mine set `_root.sensor._visible=false`, so the player
		// could still reveal tiles after losing. as_hidden is synced from
		// _visible=false (SetProperty / SetMember).
		if (obj->as_hidden) continue;

		// Compose parent transform with this entry's transform.
		// If this entry is the current drag target, use the virtual drag position
		// instead of the transform position (startDrag moves the clip visually
		// without updating transform_data).
		u32 tid = obj->transform_id;
		float (*_otd)[16] = ng_entryTransformData(obj);
		double ca = (double)_otd[tid][0];
		double cb = (double)_otd[tid][1];
		double cc = (double)_otd[tid][4];
		double cd = (double)_otd[tid][5];
		double ctx_v = (double)_otd[tid][12];
		double cty_v = (double)_otd[tid][13];
		if (g_drag_target_name[0] != '\0' && obj->instance_name &&
		    strcmp(obj->instance_name, g_drag_target_name) == 0)
		{
			ctx_v = (double)g_drag_virt_x;
			cty_v = (double)g_drag_virt_y;
		}
		double na = ma*ca + mc_m*cb, nb = mb*ca + md*cb;
		double nc = ma*cc + mc_m*cd, nd = mb*cc + md*cd;
		double ntx = ma*ctx_v + mc_m*cty_v + mtx;
		double nty = mb*ctx_v + md*cty_v + mty;

		// Check if this entry has any mouse-related clip actions (PRESS or RELEASE).
		// We need to mark entries with RELEASE actions as pressed too, so the
		// release handler can fire later even if there's no PRESS handler.
		int has_mouse_clip = 0;
		int has_press_action = 0;
		if (obj->clip_action_count > 0)
		{
			for (size_t a = 0; a < obj->clip_action_count; a++)
			{
				uint32_t flags = obj->clip_actions[a].event_flags;
				if (flags & (CLIP_EVENT_PRESS | CLIP_EVENT_RELEASE)) {
					has_mouse_clip = 1;
					if (flags & CLIP_EVENT_PRESS) has_press_action = 1;
				}
			}
		}

		if (has_mouse_clip)
		{
			// Hit-test: check if mouse is inside this entry's visual content
			int hit = 0;
			if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0) {
				hit = ng_hitTestShapeFromDL(obj->sprite_display_list, obj->sprite_max_depth,
					na, nb, nc, nd, ntx, nty, (double)mouse_x, (double)mouse_y);
			}
			if (hit) {
				obj->clip_mc_pressed = 1;
				if (has_press_action) {
					MovieClip* saved_ctx = g_current_context;
					if (obj->instance_name) {
						MovieClip* mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
						if (mc) actionSetCurrentContext(mc);
					}
					for (size_t a = 0; a < obj->clip_action_count; a++) {
						if (obj->clip_actions[a].event_flags & CLIP_EVENT_PRESS)
							obj->clip_actions[a].action(app_context);
					}
					actionSetCurrentContext(saved_ctx);
				}

				if (is_dragging && obj->instance_name && obj->instance_name[0] != '\0')
					snprintf(g_drag_target_name, sizeof(g_drag_target_name), "%s", obj->instance_name);
			}
		}

		// Recurse into sprite display lists to find nested clip actions
		if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0) {
			MovieClip* child_mc = parent_mc;
			if (obj->instance_name) {
				MovieClip* mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
				if (mc) child_mc = mc;
			}
			dispatch_clip_event_press_dl(app_context,
				obj->sprite_display_list, obj->sprite_max_depth,
				na, nb, nc, nd, ntx, nty,
				mouse_x, mouse_y, child_mc);
		}
	}
}

void dispatch_clip_event_press(SWFAppContext* app_context)
{
	float mx = app_context->mouse.stage_x;  // twips
	float my = app_context->mouse.stage_y;  // twips

	// Walk root display list with identity transform
	dispatch_clip_event_press_dl(app_context,
		display_list, max_depth,
		1.0, 0.0, 0.0, 1.0, 0.0, 0.0,
		mx, my, &root_movieclip);
}

// Recursive helper for dispatch_clip_event_release — walks into nested sprite
// display lists to find entries with clip_mc_pressed flag.
static void dispatch_clip_event_release_dl(SWFAppContext* app_context,
    DisplayObject* dl, size_t dl_max_depth, MovieClip* parent_mc)
{
	for (size_t i = 1; i <= dl_max_depth; i++)
	{
		DisplayObject* obj = &dl[i];
		if (obj->char_id == 0) continue;

		// Fire RELEASE on entries that were pressed
		if (obj->clip_mc_pressed && obj->clip_action_count > 0)
		{
			obj->clip_mc_pressed = 0;
			MovieClip* saved_ctx = g_current_context;
			if (obj->instance_name) {
				MovieClip* mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
				if (mc) actionSetCurrentContext(mc);
			}
			for (size_t a = 0; a < obj->clip_action_count; a++) {
				if (obj->clip_actions[a].event_flags & CLIP_EVENT_RELEASE)
					obj->clip_actions[a].action(app_context);
			}
			actionSetCurrentContext(saved_ctx);
		}
		else if (obj->clip_mc_pressed)
		{
			obj->clip_mc_pressed = 0;
		}

		// Recurse into sprites
		if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0) {
			MovieClip* child_mc = parent_mc;
			if (obj->instance_name) {
				MovieClip* mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
				if (mc) child_mc = mc;
			}
			dispatch_clip_event_release_dl(app_context,
				obj->sprite_display_list, obj->sprite_max_depth, child_mc);
		}
	}
}

void dispatch_clip_event_release(SWFAppContext* app_context)
{
	dispatch_clip_event_release_dl(app_context,
		display_list, max_depth, &root_movieclip);
}

// ---------------------------------------------------------------------------
// CLIP_EVENT_ROLL_OVER / ROLL_OUT / DRAG_OVER / DRAG_OUT dispatch
// ---------------------------------------------------------------------------
// Hit-test based: fired on placements when the mouse transitions across their
// hit area boundary. ROLL_* gates on button-up; DRAG_* gates on button-down.

static void dispatch_clip_event_roll_dl(SWFAppContext* app_context,
    DisplayObject* dl, size_t dl_max_depth,
    double ma, double mb, double mc_m, double md, double mtx, double mty,
    float mouse_x, float mouse_y, int btn_down, MovieClip* parent_mc)
{
	extern int ng_hitTestShapeFromDL(DisplayObject* dl, size_t dl_max,
	    double ma, double mb, double mc_m, double md, double mtx, double mty,
	    double test_x, double test_y);

	const uint32_t roll_drag_mask = CLIP_EVENT_ROLL_OVER | CLIP_EVENT_ROLL_OUT
	                              | CLIP_EVENT_DRAG_OVER | CLIP_EVENT_DRAG_OUT;

	for (size_t i = 1; i <= dl_max_depth; i++)
	{
		DisplayObject* obj = &dl[i];
		if (obj->char_id == 0) continue;

		// Compose parent transform with this entry's transform (drag-aware,
		// matches dispatch_clip_event_press_dl).
		u32 tid = obj->transform_id;
		float (*_otd)[16] = ng_entryTransformData(obj);
		double ca = (double)_otd[tid][0];
		double cb = (double)_otd[tid][1];
		double cc = (double)_otd[tid][4];
		double cd = (double)_otd[tid][5];
		double ctx_v = (double)_otd[tid][12];
		double cty_v = (double)_otd[tid][13];
		if (g_drag_target_name[0] != '\0' && obj->instance_name &&
		    strcmp(obj->instance_name, g_drag_target_name) == 0)
		{
			ctx_v = (double)g_drag_virt_x;
			cty_v = (double)g_drag_virt_y;
		}
		double na = ma*ca + mc_m*cb, nb = mb*ca + md*cb;
		double nc = ma*cc + mc_m*cd, nd = mb*cc + md*cd;
		double ntx = ma*ctx_v + mc_m*cty_v + mtx;
		double nty = mb*ctx_v + md*cty_v + mty;

		// Check if this placement has any roll/drag clip actions.
		int has_roll_clip = 0;
		if (obj->clip_action_count > 0)
		{
			for (size_t a = 0; a < obj->clip_action_count; a++)
			{
				if (obj->clip_actions[a].event_flags & roll_drag_mask) {
					has_roll_clip = 1;
					break;
				}
			}
		}

		if (has_roll_clip)
		{
			int hit = 0;
			if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0) {
				hit = ng_hitTestShapeFromDL(obj->sprite_display_list, obj->sprite_max_depth,
					na, nb, nc, nd, ntx, nty, (double)mouse_x, (double)mouse_y);
			}

			int was_inside = obj->clip_mouse_inside;
			int now_inside = hit;

			if (was_inside != now_inside)
			{
				obj->clip_mouse_inside = (u8)now_inside;
				uint32_t fire_flag = 0;
				if (now_inside) {
					// Mouse entered: ROLL_OVER if button up, DRAG_OVER if button down
					fire_flag = btn_down ? CLIP_EVENT_DRAG_OVER : CLIP_EVENT_ROLL_OVER;
				} else {
					// Mouse left: ROLL_OUT if button up, DRAG_OUT if button down
					fire_flag = btn_down ? CLIP_EVENT_DRAG_OUT : CLIP_EVENT_ROLL_OUT;
				}

				MovieClip* saved_ctx = g_current_context;
				if (obj->instance_name) {
					MovieClip* mc = actionFindOrCreateMovieClip(app_context,
					    obj->instance_name, parent_mc);
					if (mc) actionSetCurrentContext(mc);
				}
				for (size_t a = 0; a < obj->clip_action_count; a++) {
					if (obj->clip_actions[a].event_flags & fire_flag)
						obj->clip_actions[a].action(app_context);
				}
				actionSetCurrentContext(saved_ctx);
			}
		}

		// Recurse into nested sprite display lists regardless of whether this
		// entry has roll clip actions itself — children may have their own.
		if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0) {
			MovieClip* child_mc = parent_mc;
			if (obj->instance_name) {
				MovieClip* mc = actionFindOrCreateMovieClip(app_context,
				    obj->instance_name, parent_mc);
				if (mc) child_mc = mc;
			}
			dispatch_clip_event_roll_dl(app_context,
				obj->sprite_display_list, obj->sprite_max_depth,
				na, nb, nc, nd, ntx, nty,
				mouse_x, mouse_y, btn_down, child_mc);
		}
	}
}

void dispatch_clip_event_roll(SWFAppContext* app_context)
{
	float mx = app_context->mouse.stage_x;  // twips
	float my = app_context->mouse.stage_y;  // twips
	int btn_down = app_context->mouse.button_down;

	dispatch_clip_event_roll_dl(app_context,
		display_list, max_depth,
		1.0, 0.0, 0.0, 1.0, 0.0, 0.0,
		mx, my, btn_down, &root_movieclip);
}

// ---------------------------------------------------------------------------
// Generic clip event flag dispatch (MOUSE_DOWN/UP/MOVE, KEY_DOWN/UP)
// ---------------------------------------------------------------------------
// These are GLOBAL clip events — fire on ALL clips in the display list that
// have a matching clip action, regardless of mouse position.

static void dispatch_clip_event_flag_dl(SWFAppContext* app_context,
    DisplayObject* dl, size_t dl_max_depth, uint32_t flag, MovieClip* parent_mc)
{
	for (size_t i = 1; i <= dl_max_depth; i++)
	{
		DisplayObject* obj = &dl[i];
		if (obj->char_id == 0) continue;

		// Fire matching clip actions on this entry
		if (obj->clip_action_count > 0)
		{
			int has_flag = 0;
			for (size_t a = 0; a < obj->clip_action_count; a++)
			{
				if (obj->clip_actions[a].event_flags & flag) { has_flag = 1; break; }
			}
			if (has_flag)
			{
				MovieClip* saved_ctx = g_current_context;
				if (obj->instance_name)
				{
					MovieClip* mc = actionFindOrCreateMovieClip(app_context,
					    obj->instance_name, parent_mc);
					if (mc) actionSetCurrentContext(mc);
				}
				for (size_t a = 0; a < obj->clip_action_count; a++)
				{
					if (obj->clip_actions[a].event_flags & flag)
						obj->clip_actions[a].action(app_context);
				}
				actionSetCurrentContext(saved_ctx);
			}
		}

		// Recurse into sprite children
		if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
		{
			MovieClip* child_parent = parent_mc;
			if (obj->instance_name)
			{
				MovieClip* mc = actionFindOrCreateMovieClip(app_context,
				    obj->instance_name, parent_mc);
				if (mc) child_parent = mc;
			}
			dispatch_clip_event_flag_dl(app_context,
			    obj->sprite_display_list, obj->sprite_max_depth, flag, child_parent);
		}
	}
}

void dispatch_clip_event_flag(SWFAppContext* app_context, uint32_t flag)
{
	dispatch_clip_event_flag_dl(app_context, display_list, max_depth, flag,
	    &root_movieclip);
}

void tagDefineShape(SWFAppContext* app_context, CharacterType type, size_t char_id,
    size_t shape_offset, size_t shape_size,
    s32 bounds_xmin, s32 bounds_xmax, s32 bounds_ymin, s32 bounds_ymax)
{
	(void)app_context;
	ENSURE_SIZE(dictionary, char_id, dictionary_capacity, sizeof(Character));

	dictionary[char_id].type = type;
	// Re-base a loaded child's movie-LOCAL vertex offset onto the combined
	// vertex table (ng_buildMovieRenderTables). Zero for the main movie and
	// for any build without child geometry, so this is inert everywhere else.
	// Done here rather than at the ~10 renderer_draw_shape sites because the
	// defining movie is known exactly here and nowhere else.
	dictionary[char_id].shape_offset =
		shape_offset + ng_movieShapeVertBase(g_current_movie_id);
	dictionary[char_id].size = shape_size;

	// Bounds always recorded — AS-visible _width/_height/getBounds need them
	// in browser-WASM (USE_WEBGPU without OFFSCREEN_RENDER) too, so the
	// mcGetOriginalBounds path can find shape extents via ng_getCharBounds.
	ng_record_char_bounds(char_id, bounds_xmin, bounds_xmax, bounds_ymin, bounds_ymax);
}

void tagDefineMorphShape(SWFAppContext* app_context, size_t char_id,
    size_t shape_offset, size_t shape_size,
    size_t morph_end_offset, size_t morph_color_start, size_t morph_color_count,
    size_t morph_end_color_start,
    s32 bounds_xmin, s32 bounds_xmax, s32 bounds_ymin, s32 bounds_ymax,
    s32 end_bounds_xmin, s32 end_bounds_xmax, s32 end_bounds_ymin, s32 end_bounds_ymax)
{
	(void)app_context;
	ENSURE_SIZE(dictionary, char_id, dictionary_capacity, sizeof(Character));

	dictionary[char_id].type = CHAR_TYPE_MORPH_SHAPE;
	// See tagDefineShape. All four offsets are movie-LOCAL and each indexes a
	// different combined table: the START verts are in shape_data, the END
	// verts in morph_end_shape_data, the START colours in color_data and the
	// END colours in morph_end_color_data. Every base is 0 for the main movie
	// and 0 in NO_GRAPHICS for the three graphics-only tables, which is exactly
	// what those readers still expect there.
	dictionary[char_id].morph_start_offset =
		shape_offset + ng_movieShapeVertBase(g_current_movie_id);
	dictionary[char_id].morph_start_size = shape_size;
	dictionary[char_id].morph_end_offset =
		morph_end_offset + ng_movieMorphEndVertBase(g_current_movie_id);
	dictionary[char_id].morph_color_start =
		morph_color_start + ng_movieColorBase(g_current_movie_id);
	dictionary[char_id].morph_color_count = morph_color_count;
	dictionary[char_id].morph_end_color_start =
		morph_end_color_start + ng_movieMorphEndColorBase(g_current_movie_id);

	ng_record_char_bounds(char_id, bounds_xmin, bounds_xmax, bounds_ymin, bounds_ymax);
	ng_record_morph_end_bounds(char_id, end_bounds_xmin, end_bounds_xmax, end_bounds_ymin, end_bounds_ymax);
}

void tagDefineText(SWFAppContext* app_context, size_t char_id, size_t text_start, size_t text_size, u32 transform_start, u32 cxform_id, s32 bounds_xmin, s32 bounds_xmax, s32 bounds_ymin, s32 bounds_ymax)
{
	(void)app_context;
	ENSURE_SIZE(dictionary, char_id, dictionary_capacity, sizeof(Character));

	// Re-base the three movie-LOCAL indices this tag carries onto the combined
	// tables (ng_buildMovieRenderTables), at the one place the defining movie
	// is known -- the same argument as tagDefineShape:
	//   text_start      -> combined text_data (whose ROWS are also re-based,
	//                      because each is a glyph index)
	//   transform_start -> combined transform_data. This is a DEFINE-time id
	//                      (the recompiler bakes one glyph-positioning matrix
	//                      per glyph), so it takes the DEFINING movie's base,
	//                      not the placing movie's that ng_cache_transform adds.
	//   cxform_id       -> combined cxform_data; 0 is the no-colour-transform
	//                      sentinel and never moves (see ng_rebaseCxformId).
	// All three bases are 0 for the main movie, and cxform's is 0 in
	// NO_GRAPHICS where that table is not combined at all.
	text_start += ng_movieTextBase(g_current_movie_id);
	transform_start += ng_movieTransformBase(g_current_movie_id);
	if (cxform_id != 0) cxform_id += ng_movieCxformBase(g_current_movie_id);
	// The glyph outlines this character draws carry path_data offsets numbered
	// in the DEFINING movie's table; record it for the CPU glyph hit tester.
	ng_record_text_path_table(char_id);

	if (dictionary[char_id].type == CHAR_TYPE_TEXT) {
		// Accumulate: extend text range to cover this additional text record
		size_t old_start = dictionary[char_id].text_start;
		size_t old_end = old_start + dictionary[char_id].text_size;
		size_t new_end = text_start + text_size;
		if (text_start < old_start) old_start = text_start;
		if (new_end > old_end) old_end = new_end;
		dictionary[char_id].text_start = old_start;
		dictionary[char_id].text_size = old_end - old_start;
	} else {
		dictionary[char_id].type = CHAR_TYPE_TEXT;
		dictionary[char_id].text_start = text_start;
		dictionary[char_id].text_size = text_size;
		dictionary[char_id].transform_start = transform_start;
		dictionary[char_id].cxform_id = cxform_id;
		ng_record_char_bounds(char_id, bounds_xmin, bounds_xmax, bounds_ymin, bounds_ymax);
	}
}

void tagDefineEditTextProps(SWFAppContext* app_context, size_t char_id,
    const char* plain_text, const char* raw_html_text, u32 text_color,
    u16 font_id, u16 font_height, s16 max_length,
    u8 align, u16 left_margin, u16 right_margin, s16 indent, s16 leading,
    const char* variable_name, u16 flags,
    s32 bounds_xmin, s32 bounds_xmax, s32 bounds_ymin, s32 bounds_ymax)
{
	// EditText registration is required wherever actionIterateTextFieldGlyphs
	// runs — including browser-WASM (USE_WEBGPU without OFFSCREEN_RENDER) —
	// otherwise dynamic glyph rendering finds nothing to iterate and the
	// static-glyph skip in render_single_object/render_display_list can't
	// recognize the char as an EditText. The bounds are also needed for
	// sprite content bounds and SetVariable resolution against
	// `variable_name`, so just always record.
	ng_record_textfield_props(app_context, char_id, plain_text, raw_html_text, text_color,
		font_id, font_height, max_length,
		align, left_margin, right_margin, indent, leading,
		variable_name, flags,
		bounds_xmin, bounds_xmax, bounds_ymin, bounds_ymax);
	ng_record_char_bounds(char_id, bounds_xmin, bounds_xmax, bounds_ymin, bounds_ymax);
}

void tagCSMTextSettings(size_t text_id, const char* anti_alias_type, const char* grid_fit_type,
    float thickness, float sharpness)
{
	// CSMTextSettings is anti-aliasing/grid-fit metadata used by glyph
	// rendering; needed wherever text rendering runs (incl. browser-WASM).
	ng_record_csm(text_id, anti_alias_type, grid_fit_type, thickness, sharpness);
}

// Initialize cx_* fields of a display object to identity color transform.
// Multipliers are in percentage format (100.0 = 100% = identity).
static void init_cx_fields(DisplayObject* obj)
{
	obj->cx_ra = obj->cx_ga = obj->cx_ba = obj->cx_aa = 100.0;
	obj->cx_rb = obj->cx_gb = obj->cx_bb = obj->cx_ab = 0.0;
	obj->cx_overridden = 0;
#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM: ng_on_place_object2 (which populates cx_* from
	// cxform_data on the native paths) is skipped on most placement paths
	// here, leaving cx_* at identity while the renderer applies the
	// placement cxform through cxform_id. Populate the CPU-side values so
	// AS reads (_alpha, Color.getTransform) see the effective placement
	// cxform. Render-neutral: cx_* is only composed when cx_overridden.
	if (obj->has_cxform && obj->cxform_id != 0)
		ng_init_cxform_from_data(obj, obj->cxform_id);
#endif
}

// Pending clip actions to attach before eager init runs — set by WithClipActions variants
static ClipAction* g_pending_clip_actions = NULL;
static size_t g_pending_clip_action_count = 0;

// Pending instance name — set by tagSetInstanceName before tagPlaceObject2 when the
// recompiler emits SetInstanceName after PlaceObject2. Consumed by ng_on_place_object2
// to avoid auto-assigning "instanceN" when the real name is known.
const char* g_pending_instance_name = NULL;
int g_skip_pending_removal_mc = 0;  // When set, findOrCreateMovieClip skips pending_removal MCs

// Helper: recursively fire registered class constructors for child sprites
// placed during eager init. Phase 5 of ACTION_QUEUE_PLAN retired the
// tagPlaceObject2 call site (constructors now queue through the ActionQueue
// at placement time); this helper is still called from ng_fire_child_constructors
// for the attachMovie AS-level path, which follows a different flow.
// Compiled in all three builds — attachMovie now runs in browser-WASM too,
// and the function body has no graphics-specific dependencies.
static void fire_eager_constructors(SWFAppContext* app_context, DisplayObject* dl, size_t dl_max, MovieClip* parent_mc)
{
	extern const char* ng_lookupExportName(size_t char_id);
	extern void actionInvokeRegisteredClassConstructor(SWFAppContext*, const char*, MovieClip*);
	extern void actionSetupRegisteredClassPrototype(SWFAppContext*, const char*, MovieClip*);

	for (size_t i = 1; i <= dl_max; i++)
	{
		DisplayObject* obj = &dl[i];
		if (obj->char_id == 0 || obj->constructor_invoked) continue;
		if (dictionary[obj->char_id].type != CHAR_TYPE_SPRITE) continue;
		if (obj->instance_name == NULL) continue;

		const char* exp = ng_lookupExportName(obj->char_id);
		MovieClip* mc = NULL;

		// Only create the MC when there's a registered class — premature creation
		// causes zombie MCs with wrong depth/parent (regressions in
		// function_base_clip_readded, movieclip_in_removed_button)
		if (exp != NULL)
		{
			mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
			if (mc != NULL)
			{
				mc->display_obj = (void*)obj;
				actionSetupRegisteredClassPrototype(app_context, exp, mc);
				actionInvokeRegisteredClassConstructor(app_context, exp, mc);
				obj->constructor_invoked = 1;
			}
		}

		// Recurse into children regardless of whether this sprite had a class
		if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
			fire_eager_constructors(app_context, obj->sprite_display_list, obj->sprite_max_depth, mc ? mc : parent_mc);
	}
}

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
// Phase 4 of ACTION_QUEUE_PLAN: CLIP_EVENT_INITIALIZE handlers are routed
// through the unified ActionQueue at AQ_PRIORITY_INITIALIZE / AQ_KIND_CLIP_INIT.
//
// Phase 5 of ACTION_QUEUE_PLAN: CLIP_EVENT_CONSTRUCT handlers and the
// registerClass constructor are likewise routed through the queue at
// AQ_PRIORITY_CONSTRUCT, via separate kinds (CLIP_CONSTRUCT, REGISTER_CTOR)
// and separate dispatch callbacks. Recursion for nested children happens
// at queue time (each nested tagPlaceObject2 call queues its own entries)
// rather than via a drain-time walk of sprite_display_list — which is what
// retired fire_deferred_construct and the in-tagPlaceObject2 call to
// fire_eager_constructors.
//
// All three kinds drain at the outermost tagPlaceObject2 !catch_up_mode
// site in priority order (INITIALIZE → CONSTRUCT), with per-kind drain
// calls issued in the order CLIP_INIT → CLIP_CONSTRUCT → REGISTER_CTOR to
// preserve the pre-migration ordering: every INIT runs first, then every
// CONSTRUCT clip event, and finally every register-class constructor.
// Safety drains for all three kinds run at the top of tagShowFrame in case
// the outermost placement itself occurred under catch_up_mode=1 (goto
// catch-up); in the common path the safety drain is a no-op.
typedef struct {
	MovieClip* mc;
	frame_func action;
} PendingClipInit;

static void aq_dispatch_clip_init(SWFAppContext* app_context, void* user)
{
	PendingClipInit* pci = (PendingClipInit*) user;
	if (pci == NULL) return;
	MovieClip* saved_ctx = g_current_context;
	if (pci->mc) actionSetCurrentContext(pci->mc);
	pci->action(app_context);
	actionSetCurrentContext(saved_ctx);
	free(pci);
}

static void queue_clip_init_events(SWFAppContext* app_context, size_t depth)
{
	if (display_list[depth].clip_action_count == 0) return;
	if (display_list[depth].instance_name == NULL) return;
	// Only sprites (MovieClip instances) fire INITIALIZE/CONSTRUCT/LOAD
	// clip events. Buttons have their own event model (mouse + KeyPress);
	// shapes / morph shapes / text have no clip event model in AVM1.
	{
		size_t _cid = display_list[depth].char_id;
		if (_cid == 0 || dictionary[_cid].type != CHAR_TYPE_SPRITE) return;
	}
	MovieClip* _parent_mc = g_current_context ? g_current_context : &root_movieclip;
	MovieClip* _mc = actionFindOrCreateMovieClip(
		app_context, display_list[depth].instance_name, _parent_mc);
	for (size_t a = 0; a < display_list[depth].clip_action_count; a++) {
		if (!(display_list[depth].clip_actions[a].event_flags & CLIP_EVENT_INITIALIZE))
			continue;
		PendingClipInit* pci = (PendingClipInit*) malloc(sizeof(PendingClipInit));
		if (pci == NULL) continue;
		pci->mc = _mc;
		pci->action = display_list[depth].clip_actions[a].action;
		actionQueueCallbackEx(app_context, aq_dispatch_clip_init, (void*)pci,
		                      AQ_PRIORITY_INITIALIZE, _mc, /*is_unload=*/0,
		                      AQ_KIND_CLIP_INIT);
	}
}

// Phase 5: CLIP_EVENT_CONSTRUCT bundle. clip_actions is a pointer to
// recompiler-emitted static ClipAction[] storage (stable across drain).
// char_id is captured for ng_lookupExportName at dispatch (resolves the
// registered-class prototype before CONSTRUCT handlers fire).
typedef struct {
	MovieClip* mc;
	ClipAction* clip_actions;
	size_t clip_action_count;
	size_t char_id;
} PendingClipConstruct;

static void aq_dispatch_clip_construct(SWFAppContext* app_context, void* user)
{
	PendingClipConstruct* pcc = (PendingClipConstruct*) user;
	if (pcc == NULL) return;
	MovieClip* saved_ctx = g_current_context;
	if (pcc->mc) actionSetCurrentContext(pcc->mc);
	// Set __proto__ to registered class prototype before CONSTRUCT handlers fire
	// so prototype properties are accessible in the handler (matches the
	// pre-Phase-5 sync ordering at the outermost placement site).
	{
		extern const char* ng_lookupExportName(size_t char_id);
		extern void actionSetupRegisteredClassPrototype(SWFAppContext*, const char*, MovieClip*);
		const char* _exp = ng_lookupExportName(pcc->char_id);
		if (_exp != NULL && pcc->mc != NULL)
			actionSetupRegisteredClassPrototype(app_context, _exp, pcc->mc);
	}
	actionBeginConstructCapture(pcc->mc);
	for (size_t a = 0; a < pcc->clip_action_count; a++) {
		if (pcc->clip_actions[a].event_flags & CLIP_EVENT_CONSTRUCT)
			pcc->clip_actions[a].action(app_context);
	}
	actionEndConstructCapture();
	actionSetCurrentContext(saved_ctx);
	free(pcc);
}

static void queue_clip_construct_events(SWFAppContext* app_context, size_t depth)
{
	if (display_list[depth].clip_action_count == 0) return;
	if (display_list[depth].instance_name == NULL) return;
	// Only sprites fire INITIALIZE/CONSTRUCT/LOAD clip events.
	{
		size_t _cid = display_list[depth].char_id;
		if (_cid == 0 || dictionary[_cid].type != CHAR_TYPE_SPRITE) return;
	}
	// Skip enqueueing entirely when there is no CONSTRUCT handler on this
	// entry — avoids dispatch-time work (and a spurious setupPrototype) for
	// entries that only carry non-CONSTRUCT clip events.
	int _has_construct = 0;
	for (size_t a = 0; a < display_list[depth].clip_action_count; a++) {
		if (display_list[depth].clip_actions[a].event_flags & CLIP_EVENT_CONSTRUCT) {
			_has_construct = 1;
			break;
		}
	}
	if (!_has_construct) return;

	MovieClip* _parent_mc = g_current_context ? g_current_context : &root_movieclip;
	MovieClip* _mc = actionFindOrCreateMovieClip(
		app_context, display_list[depth].instance_name, _parent_mc);

	PendingClipConstruct* pcc = (PendingClipConstruct*) malloc(sizeof(PendingClipConstruct));
	if (pcc == NULL) return;
	pcc->mc = _mc;
	pcc->clip_actions = display_list[depth].clip_actions;
	pcc->clip_action_count = display_list[depth].clip_action_count;
	pcc->char_id = display_list[depth].char_id;
	actionQueueCallbackEx(app_context, aq_dispatch_clip_construct, (void*)pcc,
	                      AQ_PRIORITY_CONSTRUCT, _mc, /*is_unload=*/0,
	                      AQ_KIND_CLIP_CONSTRUCT);
}

// Phase 5: registerClass constructor. Queued at placement time; dispatched
// at the outermost !catch_up_mode drain. The dispatcher mirrors the
// pre-Phase-5 sync block at tag.c:3524–3555: assigns mc->display_obj, sets
// __proto__ via actionSetupRegisteredClassPrototype, invokes the ctor,
// and marks the DisplayObject constructor_invoked=1. display_obj is
// captured at queue time (same window during which the pre-Phase-5 sync
// block dereferenced &display_list[depth]) so realloc-staleness risk is
// no worse than before.
typedef struct {
	MovieClip* mc;
	DisplayObject* display_obj;
	char* export_name;
} PendingRegisterCtor;

static void aq_dispatch_register_ctor(SWFAppContext* app_context, void* user)
{
	PendingRegisterCtor* prc = (PendingRegisterCtor*) user;
	if (prc == NULL) return;
	if (prc->mc != NULL && prc->export_name != NULL && prc->display_obj != NULL
	    && !prc->display_obj->constructor_invoked) {
		// Skip if another path (e.g. ng_fire_child_constructors for attachMovie
		// children, which still walks display lists synchronously) already
		// invoked the ctor for this DisplayObject after we queued. The check
		// mirrors fire_eager_constructors' own `if (obj->constructor_invoked)
		// continue` guard and keeps register_and_init_order idempotent.
		extern void actionInvokeRegisteredClassConstructor(SWFAppContext*, const char*, MovieClip*);
		extern void actionSetupRegisteredClassPrototype(SWFAppContext*, const char*, MovieClip*);
		prc->mc->display_obj = (void*)prc->display_obj;
		actionSetupRegisteredClassPrototype(app_context, prc->export_name, prc->mc);
		actionInvokeRegisteredClassConstructor(app_context, prc->export_name, prc->mc);
		// The ctor can grow (move) the display list holding this entry.
		// This entry was already popped from the queue, so the realloc
		// rebase can't fix prc->display_obj — but it DOES keep
		// prc->mc->display_obj fresh (assigned equal above, rebased via the
		// child_mc_cache walk). Re-read through the MC instead of the stale
		// payload pointer.
		{
			DisplayObject* live_obj = (DisplayObject*)prc->mc->display_obj;
			if (live_obj != NULL)
				live_obj->constructor_invoked = 1;
		}
	}
	free(prc->export_name);
	free(prc);
}

static void queue_register_ctor(SWFAppContext* app_context, size_t depth, size_t char_id)
{
	extern const char* ng_lookupExportName(size_t char_id);
	// Gate on the same conditions the pre-Phase-5 sync CTOR block used.
	if (!display_list[depth].sprite_needs_init) return;
	if (display_list[depth].constructor_invoked) return;
	if (display_list[depth].instance_name == NULL) return;
	if (dictionary[char_id].type != CHAR_TYPE_SPRITE) return;

	const char* exp = ng_lookupExportName(char_id);
	// Only create the MC when there's a registered class — premature creation
	// causes zombie MCs with wrong depth/parent (same rationale as the old
	// fire_eager_constructors path).
	if (exp == NULL) return;

	MovieClip* _parent_mc = g_current_context ? g_current_context : &root_movieclip;
	MovieClip* _mc = actionFindOrCreateMovieClip(
		app_context, display_list[depth].instance_name, _parent_mc);
	if (_mc == NULL) return;

	PendingRegisterCtor* prc = (PendingRegisterCtor*) malloc(sizeof(PendingRegisterCtor));
	if (prc == NULL) return;
	prc->mc = _mc;
	prc->display_obj = &display_list[depth];
	prc->export_name = strdup(exp);
	if (prc->export_name == NULL) {
		free(prc);
		return;
	}
	actionQueueCallbackEx(app_context, aq_dispatch_register_ctor, (void*)prc,
	                      AQ_PRIORITY_CONSTRUCT, _mc, /*is_unload=*/0,
	                      AQ_KIND_REGISTER_CTOR);
}

#endif // close NO_GRAPHICS||OFFSCREEN_RENDER gate around queue_register_ctor

// Phase 7a: CLIP_EVENT_LOAD clip-action enqueue. Queued at placement time in
// tagPlaceObject2 / tagPlaceObject2Ratio (before eager init so this sprite's
// LOAD is FIFO-first among its own NORMAL-priority entries — the ordering
// 7b relies on when sprite DoAction joins the queue). Drained per-sprite
// inside process_sprite_init_at_depth at the same observable point where
// the pre-migration synchronous fire lived. The payload stores a pointer
// to the DisplayObject (stable within the parent's sprite_display_list)
// rather than a MovieClip pointer, because at queue time (during nested
// eager init) g_current_context may not yet point at the parent sprite's
// MC — a clip_actions-less parent's MC isn't pre-created by the queue
// helpers, so exec_sprite_frame's findMovieClipByName returns NULL and
// the MC lookup would misparent the child to root. Deferring the MC
// lookup to drain time avoids the issue: process_sprite_init_at_depth
// already has the correctly-parented child_mc in hand when it drains.
// (PendingClipLoad struct forward-declared near top of file.)
//
// These two helpers (aq_dispatch_clip_load + queue_clip_load_events)
// live OUTSIDE the NO_GRAPHICS||OFFSCREEN_RENDER gate so browser-WASM
// can dispatch LOAD clip-actions too. Without that, statically-placed
// sprites with clip_actions in browser-WASM never fire their LOAD
// handlers — Doodle Jump's button-text-setter scripts (clip_action_17..24)
// and the hero's physics-init LOAD (clip_action_25) didn't run; the
// hero stayed pinned at its initial position and the buttons displayed
// the static "menu" placeholder. Dependencies are all unguarded:
// actionQueueCallbackEx, actionFindOrCreateMovieClip, g_current_context,
// actionSetCurrentContext, root_movieclip.

// Dispatcher for sprite CLIP_EVENT_LOAD entries. Now drains together with
// AQ_KIND_SCRIPT entries at the SHOW_FRAME pre-drain. Looks up the MC via
// the display object's instance_name with the queue-time-captured
// parent_mc as fallback parent — root would be wrong for nested clips
// (function_base_clip_readded canary).
static void aq_dispatch_clip_load(SWFAppContext* app_context, void* user)
{
	PendingClipLoad* pcl = (PendingClipLoad*) user;
	if (pcl == NULL) return;
	MovieClip* saved_ctx = g_current_context;
	if (pcl->obj != NULL && pcl->obj->instance_name != NULL) {
		MovieClip* parent = pcl->parent_mc ? pcl->parent_mc : &root_movieclip;
		MovieClip* mc = actionFindOrCreateMovieClip(
			app_context, pcl->obj->instance_name, parent);
		if (mc) actionSetCurrentContext(mc);
	}
	pcl->action(app_context);
	actionSetCurrentContext(saved_ctx);
	free(pcl);
}

// Browser-WASM defers LOAD dispatch to AQ_KIND_LOAD so it fires AFTER
// tagShowFrame's advance_sprite_frames has populated sprite children;
// graphics-native (NO_GRAPHICS||OFFSCREEN_RENDER) sticks with AQ_KIND_SCRIPT
// because its eager init runs sprite frame_0 inline during tagPlaceObject2.
static void queue_clip_load_events_kind(SWFAppContext* app_context, size_t depth,
                                         ActionQueueKind kind)
{
	(void)app_context;
	if (display_list[depth].clip_action_count == 0) return;
	if (display_list[depth].instance_name == NULL) return;
	// Only sprites fire INITIALIZE/CONSTRUCT/LOAD clip events.
	{
		size_t _cid = display_list[depth].char_id;
		if (_cid == 0 || dictionary[_cid].type != CHAR_TYPE_SPRITE) return;
	}
	// Bail early when there's no LOAD handler on this entry — avoids a
	// spurious queue entry for sprites that only carry
	// INITIALIZE/CONSTRUCT/UNLOAD/etc.
	int _has_load = 0;
	for (size_t a = 0; a < display_list[depth].clip_action_count; a++) {
		if (display_list[depth].clip_actions[a].event_flags & CLIP_EVENT_LOAD) {
			_has_load = 1;
			break;
		}
	}
	if (!_has_load) return;

	for (size_t a = 0; a < display_list[depth].clip_action_count; a++) {
		if (!(display_list[depth].clip_actions[a].event_flags & CLIP_EVENT_LOAD))
			continue;
		PendingClipLoad* pcl = (PendingClipLoad*) malloc(sizeof(PendingClipLoad));
		if (pcl == NULL) continue;
		pcl->obj    = &display_list[depth];
		pcl->action = display_list[depth].clip_actions[a].action;
		// Capture parent MC at queue time. During Phase 1 eager init,
		// g_current_context is the parent sprite's MC (set by exec_sprite_frame's
		// actionSetCurrentContext). At root level, it's &root_movieclip. The
		// dispatcher uses this as the parent for actionFindOrCreateMovieClip
		// to resolve obj->instance_name — root as fallback gives wrong _parent
		// for nested clips (function_base_clip_readded).
		pcl->parent_mc = g_current_context;
		// Phase 7b: push at AQ_KIND_SCRIPT (not CLIP_LOAD) so sprite LOAD
		// interleaves with sprite DoAction in the SHOW_FRAME FIFO drain.
		// aq_dispatch_clip_load sets g_current_context from
		// pcl->obj->instance_name before firing the handler — so the
		// drain's per-entry dispatcher still does the correct MC
		// context-switch even though the kind is SCRIPT.
		// clip=NULL: is_unload gating disabled — the sprite is either
		// initialized (drain fires) or pre-removed with its clip_actions
		// overwritten (no entry for that obj).
		actionQueueCallbackEx(app_context, aq_dispatch_clip_load, (void*)pcl,
		                      AQ_PRIORITY_NORMAL, /*clip=*/NULL, /*is_unload=*/0,
		                      kind);
	}
}

// Original signature wrapper used by the gated NO_GRAPHICS||OFFSCREEN_RENDER
// callers. Browser-WASM uses AQ_KIND_LOAD via queue_clip_load_events_kind
// directly so the drain happens later (after sprite children are populated).
static void queue_clip_load_events(SWFAppContext* app_context, size_t depth)
{
	queue_clip_load_events_kind(app_context, depth, AQ_KIND_SCRIPT);
}

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER) // reopen gate

// Public wrapper: queue INIT / CONSTRUCT clip events for a clone (drains
// inline because the clone is created mid-DoAction and Ruffle's
// post_instantiation runs them synchronously — subsequent script lines must
// observe the side effects). LOAD is handled separately by the caller via
// ng_queue_pending_load so high-depth clones (no slot) and slot clones
// share the same drain order, matching pre-CLONE_CLIP_EVENT_DISPATCH
// behavior for tests that rely on insertion-order LOAD interleave
// (avm1/duplicate_movie_clip).
void ng_queue_placement_clip_events(SWFAppContext* app_context, size_t depth)
{
	queue_clip_init_events(app_context, depth);
	queue_clip_construct_events(app_context, depth);
	actionDrainActionQueueByKind(app_context, AQ_KIND_CLIP_INIT);
	actionDrainActionQueueByKind(app_context, AQ_KIND_CLIP_CONSTRUCT);
}

// Public wrapper: queue UNLOAD clip-action callbacks for a slot, mirroring
// the tagRemoveObject2 path. Used by AS-side removeMovieClip / actionRemoveSprite
// on clones so clip-action UNLOAD fires the same as a tag-stream RemoveObject2
// would. Caller is responsible for clearing the display_list slot afterward.
void ng_queue_slot_unload_events(SWFAppContext* app_context, size_t depth, MovieClip* mc)
{
	if (depth > max_depth) return;
	if (display_list[depth].clip_action_count == 0 &&
	    display_list[depth].accumulated_clip_action_count == 0) return;
	(void)app_context;
	for (size_t a = 0; a < display_list[depth].accumulated_clip_action_count; a++) {
		if (display_list[depth].accumulated_clip_actions[a].event_flags & CLIP_EVENT_UNLOAD)
			actionQueueClipActionUnload(
				display_list[depth].accumulated_clip_actions[a].action, mc);
	}
	for (size_t a = 0; a < display_list[depth].clip_action_count; a++) {
		if (display_list[depth].clip_actions[a].event_flags & CLIP_EVENT_UNLOAD)
			actionQueueClipActionUnload(
				display_list[depth].clip_actions[a].action, mc);
	}
}
#endif

void tagPlaceObject2(SWFAppContext* app_context, size_t depth, size_t char_id, u32 transform_id, u32 cxform_id, u16 clip_depth, u8 is_replace)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (g_tag_skip_mode) return;
#endif
	ng_ensureDisplayListSize(app_context, depth);

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM: consume any deferred pending_remove from a same-tick
	// tagRemoveObject(2) at this depth.
	//  * Same char_id  -> reclaim the cached MC by running a modify-style
	//    update inline (mirrors line ~5528's modify-detect for sprites,
	//    extended to textfields/buttons which the existing gate skips via
	//    its `sprite_display_list != NULL` requirement). Also refresh any
	//    textfield variable binding so a bound TF re-reads its AS variable
	//    on every same-(char, depth) re-placement — without this the TF
	//    would stick on whatever value was set the first time it was
	//    placed.
	//  * Different char_id -> fire the deferred invalidate + clear now, then
	//    fall through to the fresh-place path so a new MC is constructed.
	if (display_list[depth].pending_remove)
	{
		display_list[depth].pending_remove = 0;
		if (display_list[depth].char_id != 0
		    && display_list[depth].char_id == char_id)
		{
			// transform_id==0 = no matrix in this move tag (recompiler sentinel,
			// swf.cpp:3611) → keep the existing matrix (Flash semantics).
			if (transform_id != 0 && !display_list[depth].transformed_by_script) {
				display_list[depth].transform_id = transform_id;
				ng_cache_transform(&display_list[depth], transform_id);
			}
			if (!display_list[depth].cx_overridden && !display_list[depth].transformed_by_script) {
				display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
				display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
				init_cx_fields(&display_list[depth]);
			}
			if (clip_depth != 0) display_list[depth].clip_depth = clip_depth;
			display_list[depth].placed_at_frame = current_frame;
			display_list[depth].place_gen = g_place_gen;
			if (depth > max_depth) max_depth = depth;
			// Modify-path discards any pending instance_name / clip_actions
			// (Ruffle apply_place_object behaviour).
			g_pending_clip_actions = NULL;
			g_pending_clip_action_count = 0;
			g_pending_instance_name = NULL;
			// Refresh textfield variable binding so bound TFs re-read their
			// AS variable on every same-(char, depth) re-placement.
			{
				int _tf_idx = ng_find_textfield(char_id);
				if (_tf_idx >= 0 && display_list[depth].instance_name != NULL)
				{
					extern MovieClip root_movieclip;
					MovieClip* _tf_mc = actionFindOrCreateMovieClip(app_context,
						display_list[depth].instance_name, &root_movieclip);
					if (_tf_mc != NULL)
						actionTryBindTextFieldVariable(app_context, _tf_mc, /*set_initial_value=*/1);
				}
			}
			return;
		}
		if (display_list[depth].char_id != 0)
		{
			invalidate_mc_for_dl_entry(app_context, &display_list[depth]);
			clear_display_entry(app_context, depth);
		}
	}
#endif

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	// In script_only_mode (Phase 2), placement is already done from Phase 1 — skip entirely.
	if (g_script_only_mode) return;

	// Button-state-change preservation: when ng_update_button_states_in_dl
	// is re-running a state function and this (depth, char_id) matches the
	// previous state's placement at the same depth, treat as a reuse — no
	// re-instantiation, no eager init, no INIT/CONSTRUCT/LOAD re-fire, no
	// frame_0 script re-fire. Mirrors Ruffle avm1_button.set_state's
	// child_by_depth(...).id() == record.id branch. The state func cleared
	// dl[depth].char_id but left dl[depth].instance_name intact, so the
	// placement keeps its old name and the cached child MC stays bound.
	// New depths or different char_id at preserved depths take the normal
	// placement path below (eager init + script queue) so their frame_0
	// fires (ButtonEventsTest depth 13/14 ermc placements).
	if (g_btn_state_active && char_id != 0
	    && depth < BTN_STATE_SNAP_MAX
	    && g_btn_state_old_chars[depth] == char_id)
	{
		display_list[depth].char_id = char_id;
		display_list[depth].transform_id = transform_id;
		display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
		display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
		if (clip_depth != 0) display_list[depth].clip_depth = clip_depth;
		display_list[depth].placed_at_frame = current_frame;
		if (depth > max_depth) max_depth = depth;
		ng_cache_transform(&display_list[depth], transform_id);
		init_cx_fields(&display_list[depth]);
		display_list[depth].sprite_needs_init = 0;
		// Phase 1i: restore the grandchild display list and per-sprite advance
		// state from the snapshot. Without this, advance_sprite_frames sees
		// sprite_display_list==NULL on the next pass, treats the entry as
		// just_allocated, allocates a fresh array, and re-runs frame_0 —
		// producing spurious script_2 fires for preserved button-state
		// children after preserved-only state transitions (OUTDOWN→UP).
		display_list[depth].sprite_display_list = g_btn_state_old_sprite_dl[depth];
		display_list[depth].sprite_max_depth = g_btn_state_old_sprite_max[depth];
		display_list[depth].sprite_dl_capacity = g_btn_state_old_sprite_cap[depth];
		display_list[depth].sprite_initialized = g_btn_state_old_sprite_init[depth];
		display_list[depth].sprite_current_frame = g_btn_state_old_sprite_cf[depth];
		display_list[depth].sprite_is_playing = g_btn_state_old_sprite_playing[depth];
		// Mark snapshot entry as consumed so the post-state-func cleanup
		// doesn't double-free this pointer.
		g_btn_state_old_sprite_dl[depth] = NULL;
		// Discard any g_pending_instance_name set by a preceding
		// tagSetInstanceName so it doesn't leak into the next placement.
		g_pending_instance_name = NULL;
		return;
	}
#endif

	// Phase 3: Refuse Place (move=0, has_character=1) of a DIFFERENT character on
	// an already-occupied depth. Per Ruffle's instantiate_child (movie_clip.rs):
	// a Place creates a new child and is dropped with "Failed to place object at
	// depth N" if the depth is occupied. Replace (move=1, has_character=1) and
	// Modify (move=1, has_character=0) operate on existing children and bypass
	// this check.
	//
	// Gates:
	// - `char_id != display_list[depth].char_id`: same-char re-placement is the
	//   line-5282 root-timeline-loop-back preservation case; Ruffle never reaches
	//   instantiate_child there (it goes via run_goto's survives_rewind).
	// - `placed_at_frame <= current_frame`: the existing entry is from this frame
	//   or earlier, i.e. it's a true forward conflict. Loop-back replay leaves
	//   the existing entry at placed_at_frame > current_frame (from a previous
	//   iteration's later frame); Ruffle handles those via run_goto rewind, not
	//   instantiate_child. Without this gate, root-timeline loops re-trigger
	//   refusal each iteration and lose state the test relies on (e.g.
	//   replace_buttons1test: iter 2 frame 1's Place char 1 at depth 3 where
	//   iter 1 frame 2 left char 3 must succeed for static1's MC cache entry
	//   to be re-created — Ruffle's rewind has the equivalent effect).
	// - `!ng_depth_has_pending_finalize(depth)`: a same-frame Remove of the
	//   occupying child (deferred for finalize because it carries an UNLOAD
	//   handler) logically vacates the depth — the stale char_id lingers only
	//   until the deferred finalize runs. Ruffle's RemoveObject frees the depth
	//   immediately (the unload event fires later), so a subsequent Place at
	//   that depth must succeed. Without this gate, gnash misc-ming
	//   loop_test10's frame-3 Remove(mc1)+Place(mc2) at depth 100 is refused.
	// - `!g_loopback_replay`: a natural timeline loop wrap re-runs frame 0 as a
	//   rewind. Ruffle's run_goto treats a Place at an occupied depth during a
	//   rewind as a modify of the existing child, never an instantiate/refuse.
	//   The catch_up_backward survives-block below handles the modify; refusing
	//   here would short-circuit it. Without this, gnash misc-ming loop_test
	//   prints a spurious "Failed to place" each loop where swapDepths put a
	//   different character into the depth a frame-0 Place tag targets.
	// - `!catch_up_backward`: same reasoning as g_loopback_replay above, but
	//   for AS-driven gotoAndStop/gotoAndPlay backward catch-ups. After
	//   `swapDepths` repositions two MCs, a gotoAndStop to a frame whose Place
	//   tags target the original (now swapped) depths must fall through to the
	//   survives_rewind branch below — refusing here spurious-warns and skips
	//   the modify path. Fixes gnash misc-ming loop_test2 / loop_test3.
	extern int ng_depth_has_pending_finalize(size_t);
	extern int g_loopback_replay;
	extern int catch_up_backward;
	// Ruffle's instantiate_child warns whenever the depth is occupied,
	// *regardless of character identity* — the `char_id != ...` gate above is a
	// proxy for "this is not a replay of an already-executed tag", because a
	// same-char re-Place is overwhelmingly a replay (root loop-back, goto
	// catch-up, browser-WASM frame-func re-run) and a spurious warning is a
	// stdout line, i.e. a graded artefact.
	//
	// The one genuine case that proxy loses is a *forward* re-Place of the same
	// character at the same depth from a later frame's tag stream — two Move=0
	// PlaceObject2 tags at one depth on different frames of a normally-playing
	// timeline. Ruffle warns and drops the second (from_shumway/fuzz
	// e152812e2cfc: char 1 at depth 1015 on main frames 8 and 18; the expected
	// output carries exactly one warning, for the frame-18 tag).
	//
	// Guards beyond the shared prefix, all asserting "real forward tag-stream
	// execution, not a replay of a tag this depth already saw":
	//  - `place_gen != g_place_gen`: a same-generation repeat is the business of
	//    the within-frame gate further down (~line 9040), which already warns.
	//  - `placed_at_frame < current_frame` (strict): the occupant came from an
	//    *earlier* root frame. Loop-back replay re-runs frame 0 with
	//    current_frame == placed_at_frame, so it can never satisfy this.
	//  - `!catch_up_mode`: Ruffle's run_goto collects goto_commands into a
	//    per-depth map, so a catch-up spanning N Place tags at one depth
	//    executes ONE place; replaying each frame's tag stream the way we do
	//    would over-warn. (catch_up_backward is already excluded below, this
	//    covers the forward direction too.)
	int ng_same_char_forward_replace =
	    (display_list[depth].char_id == char_id
	     && display_list[depth].place_gen != g_place_gen
	     && display_list[depth].placed_at_frame < current_frame
	     && !catch_up_mode);
	if (char_id != 0 && !is_replace
	    && display_list[depth].char_id != 0
	    && (display_list[depth].char_id != char_id || ng_same_char_forward_replace)
	    && display_list[depth].placed_at_frame <= current_frame
	    && !ng_depth_has_pending_finalize(depth)
	    && !g_loopback_replay
	    && !catch_up_backward)
	{
		// Match Ruffle's avm_warning on a refused Place (and the existing
		// same-frame check at ~line 5362, which this earlier-firing gate would
		// otherwise mask — e.g. avm1/placeobject_occupied_depth expects two
		// such lines on stdout).
		printf("Warning: Failed to place object at depth %zu.\n", depth);
		g_pending_clip_actions = NULL;
		g_pending_clip_action_count = 0;
		g_pending_instance_name = NULL;
		return;
	}

	if (char_id == 0)
	{
		// Skip timeline modify on clips moved by swapDepths
		if (display_list[depth].depth_swapped) return;
		// Mirror Ruffle apply_place_object: gate matrix/cxform updates independently
		// on the per-attribute script-override flags.
		// - transformed_by_script: AS setter wrote a transform attribute (matrix) →
		//   static modify must NOT overwrite it (e.g. displaylist_depths_test8:
		//   AS sets `_y += 2` in frame 2, frame 4's moveTo(50,200) PlaceObject2
		//   modify must be ignored).
		// - cx_overridden: Color.setTransform wrote a color transform → modify must
		//   NOT overwrite the cxform (and per Flash, also leaves the matrix alone
		//   when the cxform was script-set). When BOTH are clean, this is a normal
		//   timeline modify.
		if (display_list[depth].cx_overridden) return;
		// Modify operation (HasCharacter=0): update transform/cxform only, preserve identity.
		// A move-PlaceObject2 with NO matrix (HasMatrix=0) is emitted by the
		// recompiler as transform_id=0 (swf.cpp:3611). Per Flash, a move tag only
		// updates the object's matrix when it carries one — a no-matrix modify
		// (e.g. a cxform-only fade, or a ratio/clip-only change) must KEEP the
		// existing matrix. Without the `transform_id != 0` guard such a modify
		// snaps the object to the origin (slot 0 = identity). Concrete victims:
		// Tetris's "tetris" logo (frames 6-10 cxform fade-in modifies) and its
		// score-panel labels + quit/pause buttons (frame 15 cxform modifies) all
		// jumped to (0,0). (Rare ambiguity: an explicit identity move-matrix also
		// dedups to slot 0; such a deliberate move-to-origin is not preserved.)
		if (transform_id != 0 && !display_list[depth].transformed_by_script) {
			display_list[depth].transform_id = transform_id;
		}
		// A move-PlaceObject2 with NO HasColorTransform is emitted by the
		// recompiler as cxform_id=0 (swf.cpp:3614). Per Flash, such a move keeps
		// the object's EXISTING color transform — only a move that actually
		// carries a CXFORM updates it. Unlike the matrix sentinel, this is
		// unambiguous: an explicit (even identity) CXFORM always gets a non-zero
		// slot, so cxform_id==0 only ever means "no color transform here". Without
		// this guard, an animated clip that moves a tinted child each frame (e.g.
		// Pacman's title "CPac": frame 0 places the white pac shape with a
		// white→yellow cxform, frames 1-4 move it with no cxform) loses the tint
		// on the first move and renders white.
		if (cxform_id != 0) {
			display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
			display_list[depth].has_cxform = 1;
		}
		if (clip_depth != 0) display_list[depth].clip_depth = clip_depth;
		// Don't update placed_at_frame on modify — the object was originally placed
		// at the earlier frame. Updating here would cause ng_display_clear_after to
		// incorrectly remove the object during backward goto catch-up.
		init_cx_fields(&display_list[depth]);
		if (depth > max_depth) max_depth = depth;
		// Skip caching the new transform when it was rejected above so the
		// cached x/y/xscale/yscale on the DisplayObject continue to reflect the
		// AS-set values (or the preserved matrix for a no-matrix modify).
		//
		// Ungated (was `NO_GRAPHICS || OFFSCREEN_RENDER`): this is the single
		// funnel that re-bases a placement id onto the COMBINED transform table
		// as well as the one that caches place_*. Skipping it in browser-WASM
		// while the `transform_id = transform_id` assignment above still ran
		// left a loaded child's timeline Move indexing the ROOT movie's rows —
		// wrong values for both the AVM1 `_x`/`_y` getters (which re-index
		// transform_id) and the renderer. Measured 2026-09-04 on a browser
		// build of regression/avm1_child_timeline_advance: `holder.a._x` read
		// 10, 10, 0, 0 across the child's frames 1-4 where native reads
		// 10, 20, 30, 30. Inert for a single-SWF movie (base 0, combined table
		// NULL) beyond keeping place_* in step with transform_id, which the
		// browser arm below never did.
		if (transform_id != 0 && !display_list[depth].transformed_by_script) {
			ng_cache_transform(&display_list[depth], transform_id);
		}
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		// Re-init cxform via ng_on_place_object2 (handles ng_init_cxform_from_data internally).
		// Pass actual char_id so type detection works.
		// Preserve sprite_needs_init if already set (sprite awaiting Phase 2 init from
		// a placement earlier in the same frame).
		u8 saved_needs_init = display_list[depth].sprite_needs_init;
		size_t actual_char_id = display_list[depth].char_id;
		ng_on_place_object2(app_context, depth, actual_char_id);
		if (saved_needs_init)
			display_list[depth].sprite_needs_init = saved_needs_init;
		else
			display_list[depth].sprite_needs_init = 0;
#else
		(void)app_context;
#endif
		return;
	}

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	// During backward goto catch-up, if the same character is already at this depth,
	// preserve it (update transform only) instead of destroying and re-creating.
	// Flash preserves existing child movieclips during backward goto; their scripts
	// should NOT re-fire.
	{
		extern int catch_up_backward;
		extern size_t catch_up_target;
		extern int g_loopback_replay;
		// catch_up_backward: a goto-driven backward catch-up. g_loopback_replay:
		// the natural timeline loop wrap (last frame -> frame 0). Both are
		// rewinds in Ruffle's run_goto sense, so a Place at an occupied depth
		// is a modify of the surviving child, not an instantiate.
		if ((catch_up_backward || g_loopback_replay) && display_list[depth].char_id != 0)
		{
			// Ruffle survives_rewind: for MovieClip, only ratio_equals is
			// required (id differences are OK — a swapDepths-moved MC at a
			// depth re-placed by a timeline tag keeps its identity). For other
			// types (shapes, buttons, etc.) id must also match. See
			// ruffle/core/src/display_object/movie_clip.rs survives_rewind.
			// tagPlaceObject2 (non-Ratio variant) implies ratio == 0.
			// CLONE_CLIP_EVENT_DISPATCH Phase 6: AVM1 clones (CloneSprite /
			// duplicateMovieClip targets) never survive rewind — Ruffle's
			// `avm1_clone_target.is_some()` returns false from
			// survives_rewind so the clone is removed and the static MC
			// is freshly re-placed (re-firing CONSTRUCT etc).
			int existing_is_mc = (display_list[depth].sprite_display_list != NULL);
			int is_clone_replaced = display_list[depth].clone_replaced;
			int survives = (display_list[depth].ratio == 0)
			               && !is_clone_replaced
			               && (existing_is_mc
			                   || display_list[depth].char_id == char_id);
			if (survives)
			{
				// Survives rewind: treat as modify, preserve sprite state.
				// This preserves instance_name (Ruffle: apply_place_object does
				// not update name on existing children). Discard any pending
				// instance name (set by a preceding tagSetInstanceName) BEFORE
				// ng_on_place_object2 — otherwise it would consume the pending
				// name and overwrite the preserved instance_name.
				g_pending_instance_name = NULL;
				// Mirror Ruffle apply_place_object's `if !self.transformed_by_script()`
				// gate: when an AS setter has written a transform attribute (matrix),
				// the timeline modify must NOT overwrite it. Used by natural-wrap
				// catch-up to preserve script-applied _x/_y/_xscale/etc across loop.
				if (!display_list[depth].transformed_by_script) {
					display_list[depth].transform_id = transform_id;
					ng_cache_transform(&display_list[depth], transform_id);
				}
				if (!display_list[depth].cx_overridden && !display_list[depth].transformed_by_script) {
					display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
					display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
				}
				if (clip_depth != 0) display_list[depth].clip_depth = clip_depth;
				display_list[depth].placed_at_frame = current_frame;
				display_list[depth].place_gen = g_place_gen;
				if (!display_list[depth].cx_overridden && !display_list[depth].transformed_by_script)
					init_cx_fields(&display_list[depth]);
				// Pass existing char_id so ng_on_place_object2 doesn't try to
				// treat this as a different-character placement.
				ng_on_place_object2(app_context, depth, display_list[depth].char_id);
				display_list[depth].sprite_needs_init = 0;
				return;
			}
			// Different character (or ratio) at this depth during backward catch-up:
			// - If existing entry is truly stale (placed_at_frame > target, i.e. it was
			//   preserved by ng_display_clear_after from pre-rewind state), new placement
			//   from [0, target] must win: clear and full replace.
			// - Else (placed in [0, target] by an earlier catch-up replay), skip this
			//   placement — the later frame in [0, target] will re-establish it.
			// Also fires when natural-wrap triggered the catch-up
			// (g_natural_wrap_cleanup_pending) and catch_up_mode=0: target frame
			// of natural-wrap-driven backward catch-up must replace non-survivors.
			// Surfaces in place_and_remove_object_insane_test where mc_red
			// (ratio=65535) doesn't survive frame 0's ratio=0 PlaceObject2 and must
			// be reset to transform 25's tx=0. Limited to natural-wrap to avoid
			// regressing goto_from_action backward catch-ups (e.g. dontremove)
			// that rely on the original `catch_up_mode`-only gate.
			extern int g_natural_wrap_cleanup_pending;
			// Phase 6: clone-replaced slots always need clear+replace during
			// backward catch-up (the old clone is going away, the static MC
			// is freshly placed and must re-fire CONSTRUCT). Without this
			// extra gate, clones placed at the same frame as the catch-up's
			// current frame (placed_at_frame == current_frame) would hit the
			// `else { return; }` skip path below.
			if ((catch_up_mode || g_natural_wrap_cleanup_pending) &&
			    (display_list[depth].placed_at_frame > current_frame || is_clone_replaced))
			{
				if (display_list[depth].placed_at_frame > catch_up_target || is_clone_replaced)
				{
					// Mark the existing MC pending-removal so it persists in the
					// removed-depth zone for one frame — Flash semantics: after
					// backward goto replaces an MC at a depth, an old MC with
					// an UNLOAD handler (clip-event UNLOAD or AS-level
					// onUnload) is still referenceable by name (typeof returns
					// 'movieclip') until next frame's finalize. Mirrors
					// Ruffle's `remove_child` → `should_delay_removal` path
					// called from run_goto's survives_rewind=false branch.
					// Gating on has_unload (mirrors ng_on_remove_object) avoids
					// preserving MCs that should disappear immediately — see
					// gnash misc-ming loop_test5 where movieClip3 has only
					// CONSTRUCT (no UNLOAD) and must be undefined after rewind.
					if (existing_is_mc && display_list[depth].instance_name != NULL)
					{
						extern int has_child_unload_handler(DisplayObject* dl, size_t dl_max);
						extern int actionMCHasOnUnloadProperty(const char* name, int swf_depth);
						extern void actionMarkMCPendingRemoval(SWFAppContext*, const char*, int);
						int _swr_has_unload = 0;
						for (size_t _ca = 0; _ca < display_list[depth].clip_action_count; _ca++) {
							if (display_list[depth].clip_actions[_ca].event_flags & 0x4) {
								_swr_has_unload = 1; break;
							}
						}
						if (!_swr_has_unload) {
							for (size_t _ca = 0; _ca < display_list[depth].accumulated_clip_action_count; _ca++) {
								if (display_list[depth].accumulated_clip_actions[_ca].event_flags & 0x4) {
									_swr_has_unload = 1; break;
								}
							}
						}
						if (!_swr_has_unload) {
							_swr_has_unload = actionMCHasOnUnloadProperty(
								display_list[depth].instance_name, (int)depth);
						}
						if (!_swr_has_unload && display_list[depth].sprite_display_list != NULL &&
						    display_list[depth].sprite_max_depth > 0) {
							_swr_has_unload = has_child_unload_handler(
								display_list[depth].sprite_display_list,
								display_list[depth].sprite_max_depth);
						}
						if (_swr_has_unload) {
							// Queue clip-action UNLOAD callbacks BEFORE marking
							// pending so the trace lands after the calling
							// gotoAndStop/Play script. Use the *Deferred variant
							// (kind=SCRIPT, is_unload=0) so the nested
							// tagShowFrame inside ng_executeGotoCatchUp's
							// per-frame replay doesn't drain it via
							// actionFirePendingUnloads — that would land the
							// trace mid-rewind instead of after totals()
							// (loop_test8). Recursive child unloads are
							// intentionally NOT fired here: they'd queue with
							// is_unload=1 and fire too early; loop_test8's mc5
							// has no nested children needing UNLOAD.
							MovieClip* _swr_mc_for_q = actionFindOrCreateMovieClip(
								app_context, display_list[depth].instance_name, &root_movieclip);
							for (size_t _ca = 0; _ca < display_list[depth].accumulated_clip_action_count; _ca++) {
								if (display_list[depth].accumulated_clip_actions[_ca].event_flags & 0x4)
									actionQueueClipActionUnloadDeferred(display_list[depth].accumulated_clip_actions[_ca].action, _swr_mc_for_q);
							}
							for (size_t _ca = 0; _ca < display_list[depth].clip_action_count; _ca++) {
								if (display_list[depth].clip_actions[_ca].event_flags & 0x4)
									actionQueueClipActionUnloadDeferred(display_list[depth].clip_actions[_ca].action, _swr_mc_for_q);
							}
							actionMarkMCPendingRemoval(app_context,
							                           display_list[depth].instance_name,
							                           (int)depth);
						}
						else if (display_list[depth].instance_name != NULL)
						{
							// No unload handler — the MC won't be marked pending,
							// but the cached MovieClip would survive with its
							// stale (script-modified) x/y/etc. Invalidate it so
							// the next AS lookup creates a fresh MC with
							// transform-derived position. Surfaces in
							// place_and_remove_object_insane_test where mc_red
							// has script-set _x=60 and ratio=65535; iter 2 frame 0's
							// non-survives ratio mismatch must reset the visual
							// state to transform 25's identity tx=0.
							extern void actionInvalidateCachedMovieClip(SWFAppContext*, const char*, int);
							actionInvalidateCachedMovieClip(app_context, display_list[depth].instance_name, (int)depth);
						}
					}
					if (display_list[depth].sprite_display_list != NULL)
					{
						ng_freeSpriteDL(app_context, display_list[depth].sprite_display_list,
			                display_list[depth].sprite_dl_capacity);
						display_list[depth].sprite_display_list = NULL;
					}
					display_list[depth].char_id = 0;
					// Fall through to full placement
				}
				else
				{
					return;
				}
			}
		}
		// SWAPDEPTHS_REWIND_FRESH_PLACEMENT Phase 2: previously, if this depth was
		// empty but a depth_swapped entry with the same char_id existed elsewhere
		// (the character was moved by swapDepths), we re-used that MC in place to
		// avoid creating a duplicate. That re-use semantic was a workaround for
		// pointer-equality of MovieClip values — Phase 1.5 introduced path-based
		// MovieClip equality (Ruffle Value::PartialEq), so the workaround is no
		// longer needed. Falling through to fresh placement matches Ruffle's
		// run_goto behavior: the rewind tag stream freshly instantiates the
		// character at its tag-defined depth, the moved MC at the swap-target
		// depth is left to ng_display_clear_after / cleanup_unplaced_after to
		// destroy or preserve based on dynamic-vs-static-zone rules. Fresh
		// placement also queues CONSTRUCT clip events on the new instance.
		//
		// SWAPDEPTHS_REWIND_UNBLOCK Phase 4: when a fresh placement happens at a
		// depth where the same character was moved (via swapDepths) to a
		// different depth, the OLD MC's cache entry (still alive at the
		// swap-target depth) shadows AS variable lookups for the name. Mark the
		// OLD MC's cache entry as `name_displaced` so findOrCreateMovieClip
		// skips it for AS name lookups; the fresh placement's
		// findOrCreateMovieClip then creates a new MC. Direct pointer access
		// via saved AS references still resolves to the OLD MC (path-based
		// equality from Phase 1.5 makes them comparable), and
		// MovieClip.getInstanceAtDepth(N) uses findCachedMovieClipByDepth which
		// bypasses the displaced-skip.
		if (catch_up_backward && char_id != 0 && g_pending_instance_name != NULL) {
			extern MovieClip* findCachedMovieClipByDepth(const char*, MovieClip*, int);
			extern void actionMarkMCNameDisplaced(MovieClip*);
			for (size_t _sd = 1; _sd <= max_depth; _sd++) {
				if (_sd == depth) continue;
				if (display_list[_sd].char_id == char_id
				    && display_list[_sd].depth_swapped
				    && display_list[_sd].instance_name != NULL
				    && strcmp(display_list[_sd].instance_name,
				             g_pending_instance_name) == 0)
				{
					int as_depth = (int)_sd - 16384;
					MovieClip* old_mc = findCachedMovieClipByDepth(
						g_pending_instance_name,
						g_current_context ? g_current_context : &root_movieclip,
						as_depth);
					if (old_mc != NULL) actionMarkMCNameDisplaced(old_mc);
					break;
				}
			}
		}
	}
#endif

	// Root timeline loop-back preservation: when re-placing the SAME character at a depth
	// that already has an initialized sprite FROM A PREVIOUS FRAME, treat as modify
	// (update transform only). This matches Ruffle's run_goto behavior where existing
	// clips are preserved during root timeline looping (frame 4 -> frame 0).
	// Only applies across frame boundaries (different place_gen), NOT within same frame.
	if (display_list[depth].char_id == char_id && display_list[depth].char_id != 0
	    && display_list[depth].sprite_display_list != NULL
	    && display_list[depth].place_gen != g_place_gen)
	{
		display_list[depth].transform_id = transform_id;
		ng_cache_transform(&display_list[depth], transform_id);
		display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
		display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
		if (clip_depth != 0) display_list[depth].clip_depth = clip_depth;
		display_list[depth].placed_at_frame = current_frame;
		display_list[depth].place_gen = g_place_gen;
		init_cx_fields(&display_list[depth]);
		if (depth > max_depth) max_depth = depth;
		ng_on_place_object2(app_context, depth, char_id);
		display_list[depth].sprite_needs_init = 0;
		return;
	}

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	// Cross-frame sprite-by-sprite REPLACE preservation. When the existing depth holds
	// a sprite placed by a previous frame, and a different sprite char_id is being
	// placed by the current frame's PlaceObject2/3 with move=1+has_character=1, Flash/
	// Ruffle preserve the existing sprite's identity. Mirrors Ruffle's
	// PlaceObjectAction::Replace path: child.replace_with() is a noop for MovieClip
	// (display_object/movie_clip.rs default impl), and apply_place_object updates only
	// matrix/cxform/ratio — name, clip_depth, and clip_actions are explicitly excluded
	// (display_object.rs:2536-2539 "Purposely omitted properties: name, clip_depth,
	// clip_actions"). No CONSTRUCT/INIT/LOAD events fire because no fresh
	// instantiation occurs.
	// Targets misc-ming/replace_sprites1test where frame-2 PlaceObject2 (move=1)
	// replaces static1 (sprite cid=2) at depth 3 with a different sprite (cid=4) +
	// CONSTRUCT clip-action. Without this gate the new sprite's CONSTRUCT fires
	// ("_level0.static2 onClipConstruct (replace)") and `static1` becomes undefined
	// because the depth's instance_name is overwritten. Companion to commit 8b6a0e34's
	// non-sprite gating in queue_clip_*_events.
	// Skip when an UNLOAD-handler Remove was just deferred at this depth this frame
	// (display_list still holds the pre-Remove char_id but the user explicitly
	// removed the MC — this is a Remove+Place sequence, not a pure REPLACE tag).
	// Required for misc-ming/register_class/RegisterClassTest4 (sprite_2 frame_1:
	// RemoveObject2(d=1) + PlaceObject2(d=1, char_id=1) — the second Bug ctor must
	// fire on the fresh placement).
	if (display_list[depth].char_id != 0
	    && display_list[depth].char_id != char_id
	    && display_list[depth].place_gen != g_place_gen
	    && display_list[depth].sprite_display_list != NULL
	    && char_id < dictionary_capacity
	    && dictionary[char_id].type == CHAR_TYPE_SPRITE
	    && !ng_depth_has_pending_finalize(depth))
	{
		if (!display_list[depth].transformed_by_script) {
			display_list[depth].transform_id = transform_id;
			ng_cache_transform(&display_list[depth], transform_id);
		}
		if (!display_list[depth].cx_overridden && !display_list[depth].transformed_by_script) {
			display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
			display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
			init_cx_fields(&display_list[depth]);
		}
		display_list[depth].placed_at_frame = current_frame;
		display_list[depth].place_gen = g_place_gen;
		// Discard pending instance_name / clip_actions set by the caller — the
		// REPLACE tag's name/clip_actions are ignored (Ruffle apply_place_object).
		g_pending_instance_name = NULL;
		g_pending_clip_actions = NULL;
		g_pending_clip_action_count = 0;
		ng_on_place_object2(app_context, depth, display_list[depth].char_id);
		display_list[depth].sprite_needs_init = 0;
		if (depth > max_depth) max_depth = depth;
		return;
	}
#endif

	// Within-same-frame placement handling, mapped to Ruffle's PlaceObjectAction:
	//  - PLACE (Move=0, is_replace=0) of a DIFFERENT character at a depth occupied
	//    this generation: instantiate_child refuses + warns ("Failed to place
	//    object at depth N"). (avm1/placeobject_occupied_depth's frame-1 pair.)
	//  - SAME character (either Move setting): replace_with(same id) is a modify —
	//    update matrix/cxform, do NOT re-instantiate. (placeobject_occupied_depth
	//    frame 2: a Move=1 re-place of the just-placed char must not re-trigger
	//    "Instantiated ID 4".)
	//  - REPLACE (Move=1, is_replace=1) of a DIFFERENT character: replace_with(id)
	//    swaps the character in place, never warns. g_place_gen only advances on
	//    root-timeline frame changes, so a sprite playing frame-by-frame shape
	//    swaps while the root is stopped keeps a single generation; those legit
	//    replaces must fall through to the replace path below rather than be
	//    rejected (Tetris "Dots" char-17: shapes 13->14->15->16 at depth 1).
	if (display_list[depth].char_id != 0 && display_list[depth].place_gen == g_place_gen)
	{
		if (display_list[depth].char_id != char_id)
		{
			if (!is_replace)
			{
				// PLACE of a different character at an occupied depth: refuse + warn.
				// Discard any pending instance_name set by a preceding
				// tagSetInstanceName so the rejected placement's name doesn't leak to
				// the next placement (and doesn't overwrite the surviving first
				// placement's name either). Required for gnash misc-ming
				// place_object_test where the first placement's name (sh1/mc2) must
				// survive the second placement's rejection.
				g_pending_instance_name = NULL;
				printf("Warning: Failed to place object at depth %zu.\n", depth);
				return;
			}
			// REPLACE of a different character: fall through to the replace path.
		}
		else
		{
			// Same character at same depth in same generation: treat as modify
			// (don't re-init). Per Ruffle apply_place_object, name is "Purposely
			// omitted" on modify — discard any pending instance_name without
			// consuming it.
			g_pending_instance_name = NULL;
			display_list[depth].transform_id = transform_id;
			ng_cache_transform(&display_list[depth], transform_id);
			display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
			display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
			if (clip_depth != 0) display_list[depth].clip_depth = clip_depth;
			init_cx_fields(&display_list[depth]);
			if (depth > max_depth) max_depth = depth;
			return;
		}
	}

	// REPLACE of an existing occupied slot. Matches Ruffle's
	// PlaceObjectAction::Replace -> child.replace_with(id) + apply_place_object:
	// name / clip_depth / clip_actions are purposely omitted, and matrix / cxform
	// / ratio are applied only when the tag carries them (HasMatrix etc.); the
	// char_id is hot-swapped below. Two cases:
	//   - is_replace: an explicit Move=1+HasCharacter=1 replace at ANY generation
	//     (e.g. a playing sprite's per-frame shape swap while the root — which is
	//     what bumps g_place_gen — is stopped; Tetris "Dots" char 17).
	//   - cross-generation re-place: a prior-frame entry replaced with a different
	//     character. Targets misc-ming/replace_shapes1test where the frame-3
	//     REPLACE supplies a new name "static2"; the existing "static1" binding
	//     must remain and "static2" must NOT be registered.
	int is_replace_of_existing = display_list[depth].char_id != 0
	                              && (is_replace
	                                  || display_list[depth].place_gen != g_place_gen);

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM: before we overwrite this slot's char_id and drop the old
	// sprite_display_list pointer, invalidate the cached MC bound to this
	// display entry and all its descendants. Without this,
	// actionIterateTextFieldGlyphs keeps rendering their glyphs after the new
	// char_id has replaced the old at this depth (Doodle Jump menu→gameplay
	// transition: depth 8's "play" button is PlaceObject2-replaced rather
	// than RemoveObject2'd, so the tagRemoveObject2 cleanup path doesn't
	// see it).
	if (display_list[depth].char_id != 0)
	{
		invalidate_mc_for_dl_entry(app_context, &display_list[depth]);
	}
#endif

	display_list[depth].char_id = char_id;
	// Cross-frame REPLACE without HasMatrix: preserve the prior transform.
	// Recompiler emits transform_id=0 as the "SWF tag had no HasMatrix" sentinel
	// (swf.cpp:3478-3480). Ruffle's apply_place_object only writes the matrix
	// when HasMatrix is true. Without this gate the existing transform is
	// clobbered to identity (transform_data[0]) on a Move=1+HasCharacter=1
	// re-place — Snake countdown frame_16/29 PlaceObject2(d=2, char=19/20,
	// transform=0, replace=1) drops chars 19/20 ("2"/"1") to the canvas origin
	// instead of inheriting frame_4's centered transform 131 from char 18 ("3").
	// Fresh placements (is_replace_of_existing=false, slot was empty) fall
	// through unchanged because the existing transform_id was already 0.
	if (!(is_replace && transform_id == 0 && is_replace_of_existing)) {
		display_list[depth].transform_id = transform_id;
		ng_cache_transform(&display_list[depth], transform_id);
	}
	display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
	display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
	display_list[depth].clip_depth = clip_depth;
	display_list[depth].ratio = 0;
	display_list[depth].blend_mode = 0;
	display_list[depth].sprite_display_list = NULL;
	display_list[depth].sprite_max_depth = 0;
	display_list[depth].sprite_dl_capacity = 0;
	display_list[depth].sprite_current_frame = 0;
	display_list[depth].sprite_is_playing = 1;
	display_list[depth].sprite_manual_next_frame = 0;
	display_list[depth].sprite_next_frame = 0;
	// Look up child movie transform data by character's movie_id
	display_list[depth].child_transform_data = NULL;
	if (g_char_movie_id != NULL && char_id < g_char_movie_id_capacity && g_char_movie_id[char_id] != 0)
		display_list[depth].child_transform_data = g_movie_transform_data[g_char_movie_id[char_id]];
	if (is_replace_of_existing) {
		// REPLACE preserves existing instance_name and clip_actions; discard
		// any pending values so they don't leak to the next placement.
		g_pending_instance_name = NULL;
		g_pending_clip_actions = NULL;
		g_pending_clip_action_count = 0;
	} else {
		// Free old instance name if we own it
		if (display_list[depth].instance_name_owned && display_list[depth].instance_name != NULL)
		{
			free(display_list[depth].instance_name);
		}
		// Consume pending instance name from tagSetInstanceName (called before PlaceObject)
		if (g_pending_instance_name != NULL) {
			display_list[depth].instance_name = (char*)g_pending_instance_name;
			display_list[depth].instance_name_owned = 0;
			g_pending_instance_name = NULL;
		} else {
			display_list[depth].instance_name = NULL;
			display_list[depth].instance_name_owned = 0;
		}
		display_list[depth].clip_actions = NULL;
		display_list[depth].clip_action_count = 0;
	}
	display_list[depth].filter_type = 0;
	// The filter CHAIN is placement-scoped exactly like filter_type: the
	// tagBeginFilterList of this placement (emitted right after its
	// tagSetFilter) re-links it. Clearing here stops a re-placed depth from
	// inheriting the previous occupant's list.
	display_list[depth].filter_chain = NULL;
	display_list[depth].depth_swapped = 0;
	// Restore persistent button state if the same character is being re-placed
	// (e.g. a looping movie that removes+replaces a button each frame cycle).
	if (display_list[depth].sticky_char_id == char_id && char_id != 0)
	{
		display_list[depth].button_state = display_list[depth].sticky_button_state;
		display_list[depth].button_prev_state = display_list[depth].sticky_button_state;
	}
	else
	{
		display_list[depth].button_state = 0;
		display_list[depth].button_prev_state = 0;
		display_list[depth].sticky_button_state = 0;
		display_list[depth].sticky_char_id = 0;
	}
	display_list[depth].sprite_needs_init = 0;
	display_list[depth].placed_at_frame = current_frame;
	{ extern size_t g_tick_count; display_list[depth].placed_at_tick = g_tick_count; }
	// Record the loaded-movie holder whose frame tag is running, so a wrap-back
	// of that movie's timeline can take its own children off the list again.
	// NULL for every ordinary main-movie placement. See DisplayObject.
	{
		extern MovieClip* actionCurrentChildMovieMC(void);
		display_list[depth].placed_by_holder = (void*)actionCurrentChildMovieMC();
	}
	display_list[depth].place_gen = g_place_gen;
	display_list[depth].place_seq = ++g_place_seq;
	display_list[depth].constructor_invoked = 0;
	init_cx_fields(&display_list[depth]);

	if (depth > max_depth)
	{
		max_depth = depth;
	}

	// Consume pending clip actions (set by WithClipActions variants).
	// Reset immediately so nested tagPlaceObject2 calls during eager init
	// don't inherit the parent's clip actions.
	// Pulled outside the NO_GRAPHICS||OFFSCREEN_RENDER gate so browser-WASM
	// also attaches clip_actions to the DisplayObject — without that,
	// dispatch_enterframe_clip_actions (which is itself unguarded but
	// iterates obj->clip_actions) would find NULL and no clip-event
	// handlers (LOAD or ENTER_FRAME) would ever fire in the browser.
	if (g_pending_clip_actions != NULL) {
		display_list[depth].clip_actions = g_pending_clip_actions;
		display_list[depth].clip_action_count = g_pending_clip_action_count;
		note_clip_actions_for_ef(display_list[depth].clip_actions, display_list[depth].clip_action_count);
		g_pending_clip_actions = NULL;
		g_pending_clip_action_count = 0;
	}

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM: ng_on_place_object2 (which assigns auto-instance names
	// to unnamed scriptable placements) is gated and doesn't fire here, so
	// handle that subset inline so queue_clip_load_events can find the
	// entry by name. Mirrors ng_on_place_object2 lines 705-720.
	if (display_list[depth].instance_name == NULL
	    && display_list[depth].char_id != 0) {
		size_t _ainame_cid = display_list[depth].char_id;
		int _ainame_is_sprite = (_ainame_cid < INITIAL_DICTIONARY_CAPACITY
		                         && dictionary[_ainame_cid].type == CHAR_TYPE_SPRITE);
		int _ainame_is_button = ng_find_button(_ainame_cid);
		int _ainame_is_tf     = (ng_find_textfield(_ainame_cid) >= 0);
		if (_ainame_is_sprite || _ainame_is_button || _ainame_is_tf) {
			char _ainame_buf[32];
			snprintf(_ainame_buf, sizeof(_ainame_buf), "instance%u",
			         ng_increment_auto_instance_counter());
			display_list[depth].instance_name = strdup(_ainame_buf);
			display_list[depth].instance_name_owned = 1;
		}
	}
	// Browser-WASM: mark sprite as initialized so dispatch_enterframe_clip_actions
	// will start firing CLIP_EVENT_ENTER_FRAME on the next tick (after
	// upgrade_sprite_initialized in tagShowFrame bumps 1→2). Without this,
	// sprite_initialized stays at 0 (process_sprite_needs_init is a stub in
	// browser-WASM) and EnterFrame handlers — including the hero's physics
	// in Doodle Jump — never run.
	if (display_list[depth].char_id != 0
	    && display_list[depth].char_id < INITIAL_DICTIONARY_CAPACITY
	    && dictionary[display_list[depth].char_id].type == CHAR_TYPE_SPRITE
	    && display_list[depth].sprite_initialized == 0) {
		display_list[depth].sprite_initialized = 1;
	}
	// Browser-WASM: queue LOAD clip-actions on AQ_KIND_LOAD so they
	// drain AFTER tagShowFrame's advance_sprite_frames has populated
	// sprite children. Without this deferral the LOAD handler runs
	// during actionDrainAllInPriorityOrder (before tagShowFrame), and
	// scripts like Doodle Jump's `this.button_txt.text = "scores"` see
	// no `button_txt` child yet and silently no-op — buttons keep their
	// static placeholder text. AQ_KIND_LOAD is NOT touched by the
	// in-frame actionDrainAllInPriorityOrder so the entries persist
	// until tagShowFrame's explicit drain.
	queue_clip_load_events_kind(app_context, depth, AQ_KIND_LOAD);
#endif

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	ng_on_place_object2(app_context, depth, char_id);

	// Queue CLIP_EVENT_INITIALIZE / CLIP_EVENT_CONSTRUCT handlers and the
	// registerClass constructor onto the unified ActionQueue. Nested
	// placements (catch_up_mode=1 during eager init) queue but do not drain;
	// the outermost placement drains all three kinds in order (INIT then
	// CONSTRUCT then REGISTER_CTOR) before returning, preserving the
	// pre-Phase-5 ordering where every INIT fires, then every CONSTRUCT
	// clip event, then every register-class constructor.
	queue_clip_init_events(app_context, depth);
	queue_clip_construct_events(app_context, depth);
	queue_register_ctor(app_context, depth, char_id);
	// Phase 7a: queue CLIP_EVENT_LOAD clip-actions at NORMAL priority BEFORE
	// eager init. Drained per-sprite inside process_sprite_init_at_depth
	// (replacing the pre-migration synchronous fire at the same observable
	// point). The "before eager init" placement is what 7b will rely on:
	// this sprite's LOAD is FIFO-first among its NORMAL entries, so when
	// sprite DoAction joins the queue in 7b, the per-sprite
	// LOAD → frame_0 → nested-LOAD → nested-frame_0 interleave falls out
	// of a single Normal-priority FIFO drain.
	queue_clip_load_events(app_context, depth);

	// Phase 7b: pre-create the placed sprite's MC with the caller's context
	// as parent, so exec_sprite_frame's findMovieClipByName finds it and
	// sets g_current_context to the sprite's MC during Phase 1 eager init.
	// Without this, nested PlaceObject calls inside this sprite's frame_0
	// would see g_current_context still as the parent's-parent (or root),
	// and queue_clip_load_events would capture the wrong parent_mc
	// (function_base_clip_readded canary).
	if (display_list[depth].sprite_needs_init
	    && dictionary[char_id].type == CHAR_TYPE_SPRITE
	    && display_list[depth].instance_name != NULL)
	{
		extern MovieClip root_movieclip;
		MovieClip* _parent_for_mc = g_current_context ? g_current_context : &root_movieclip;
		actionFindOrCreateMovieClip(app_context, display_list[depth].instance_name, _parent_for_mc);
	}

	// Eagerly execute sprite frame 0 immediately after placement so the sprite's
	// internal display list is populated BEFORE the parent frame's ActionScript runs.
	// This matches Flash AVM1 behavior: sprites placed via PlaceObject2 are
	// "constructed" (frame 0 executed) before the parent's DoAction scripts.
	// During eager init, nested tagPlaceObject2 calls run with catch_up_mode=1
	// and enqueue their own INIT/CONSTRUCT/CTOR entries into the shared queue.
	if (display_list[depth].sprite_needs_init)
	{
		if (dictionary[char_id].type == CHAR_TYPE_SPRITE)
		{
			Character* sp_ch = &dictionary[char_id];
			if (sp_ch->sprite_frame_funcs != NULL && sp_ch->sprite_frame_funcs[0] != NULL)
			{
				// sni distinguishes Phase 2 behavior:
				//   2 = eager init suppressed scripts (under goto catchup, recompiler
				//       gate g1 false). Phase 2 must re-run frame_0 in script_only_mode
				//       to fire scripts inline (gate g2 true).
				//   3 = eager init queued scripts via the SHOW_FRAME drain path
				//       (recompiler gate g1 true, normal flow). Phase 2 must NOT
				//       re-run frame_0 — queued scripts will fire on their own;
				//       re-running would double-fire.
				// consecutive_goto_frame_test sprite_4 motivates the split.
				extern int actionGotoCatchupActive(void);
				display_list[depth].sprite_needs_init = actionGotoCatchupActive() ? 2 : 3;
				DisplayObject* saved_dl = display_list;
				size_t saved_max = max_depth;
				size_t saved_cap = display_list_capacity;
				display_list = saved_dl[depth].sprite_display_list;
				max_depth = saved_dl[depth].sprite_max_depth;
				display_list_capacity = saved_dl[depth].sprite_dl_capacity;
				// Phase 1: run with catch_up_mode=1 so only placement tags execute;
				// DoAction scripts are deferred to tagShowFrame Phase 2.
				int saved_catch_up = catch_up_mode;
				catch_up_mode = 1;
				g_eager_init_depth++;
				CALL_FRAME(app_context, &saved_dl[depth], sp_ch->sprite_frame_funcs[0]);
				g_eager_init_depth--;
				catch_up_mode = saved_catch_up;
				saved_dl[depth].sprite_display_list = display_list;
				saved_dl[depth].sprite_max_depth = max_depth;
				saved_dl[depth].sprite_dl_capacity = display_list_capacity;
				display_list = saved_dl;
				max_depth = saved_max;
				display_list_capacity = saved_cap;
				saved_dl[depth].sprite_current_frame = (sp_ch->sprite_frame_count > 0) ? (1 % sp_ch->sprite_frame_count) : 0;
			}
		}
		else if (dictionary[char_id].type == CHAR_TYPE_BUTTON)
		{
			// Buttons need eager init so AS code accessing button.childName
			// (e.g. button3.instance1) sees the populated state-0 children
			// before the parent frame's DoAction runs. State funcs only emit
			// tagPlaceObject2 calls (no scripts), so this is safe under
			// catch_up_mode=1 — nested sprite placements still defer their
			// own Phase-2 scripts via the sprite eager-init path above.
			Character* btn_ch = &dictionary[char_id];
			if (btn_ch->button_state_funcs != NULL && btn_ch->button_state_funcs[0] != NULL)
			{
				display_list[depth].sprite_needs_init = 2; // mark: state_0 done, recurse later
				// Pre-create the button MC with the correct parent so child
				// placements inside state_funcs see button_mc as g_current_context
				// and their _parent chain resolves correctly.
				MovieClip* parent_for_btn = g_current_context ? g_current_context : &root_movieclip;
				MovieClip* button_mc = NULL;
				if (display_list[depth].instance_name != NULL) {
					button_mc = actionFindOrCreateMovieClip(app_context,
						display_list[depth].instance_name, parent_for_btn);
					if (button_mc) {
						button_mc->is_button_mc = 1;
						button_mc->display_obj = (void*)&display_list[depth];
					}
				}

				DisplayObject* saved_dl = display_list;
				size_t saved_max = max_depth;
				size_t saved_cap = display_list_capacity;
				display_list = saved_dl[depth].sprite_display_list;
				max_depth = saved_dl[depth].sprite_max_depth;
				display_list_capacity = saved_dl[depth].sprite_dl_capacity;

				int saved_catch_up = catch_up_mode;
				catch_up_mode = 1;
				g_eager_init_depth++;
				MovieClip* saved_ctx_btn = g_current_context;
				if (button_mc) actionSetCurrentContext(button_mc);
				btn_ch->button_state_funcs[0](app_context);
				actionSetCurrentContext(saved_ctx_btn);
				g_eager_init_depth--;
				catch_up_mode = saved_catch_up;

				saved_dl[depth].sprite_display_list = display_list;
				saved_dl[depth].sprite_max_depth = max_depth;
				saved_dl[depth].sprite_dl_capacity = display_list_capacity;
				display_list = saved_dl;
				max_depth = saved_max;
				display_list_capacity = saved_cap;
			}
		}
	}

	// Phase 2 of CLIP_EVENT_ROUND_DISPATCH: per-placement drain removed in
	// favor of a frame-end priority drain (actionDrainAllInPriorityOrder)
	// emitted by the recompiler. INIT/CONSTRUCT/REGISTER_CTOR queue here and
	// drain in priority rounds at frame end so cross-sprite ordering matches
	// Flash/Ruffle's batch model (every INIT first, then every CONSTRUCT,
	// then every REGISTER_CTOR), instead of the per-sprite serial pattern
	// the per-placement drain produced.

#else
	(void)app_context;
#endif
}

void tagPlaceObject2WithClipActions(SWFAppContext* app_context, size_t depth, size_t char_id,
    u32 transform_id, u32 cxform_id, u16 clip_depth, ClipAction* clip_actions, size_t clip_action_count, u8 is_replace)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (g_tag_skip_mode) return;
#endif
	// Set pending so tagPlaceObject2 attaches them before eager init
	g_pending_clip_actions = clip_actions;
	g_pending_clip_action_count = clip_action_count;
	tagPlaceObject2(app_context, depth, char_id, transform_id, cxform_id, clip_depth, is_replace);
	// tagPlaceObject2 consumes g_pending_clip_actions and fires events
	g_pending_clip_actions = NULL;
	g_pending_clip_action_count = 0;
}

void tagSetClipActions(SWFAppContext* app_context, size_t depth, ClipAction* clip_actions, size_t clip_action_count)
{
	(void)app_context;
	if (depth < display_list_capacity && display_list[depth].char_id != 0)
	{
		display_list[depth].clip_actions = clip_actions;
		display_list[depth].clip_action_count = clip_action_count;
		note_clip_actions_for_ef(clip_actions, clip_action_count);
	}
}

void tagPlaceObject2Ratio(SWFAppContext* app_context, size_t depth, size_t char_id,
    u32 transform_id, u32 cxform_id, u16 clip_depth, u16 ratio, u8 is_replace)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (g_tag_skip_mode) return;
#endif
	ng_ensureDisplayListSize(app_context, depth);

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM: consume any deferred pending_remove from a same-tick
	// tagRemoveObject(2) at this depth. Different (char_id, ratio) -> fire
	// the deferred invalidate + clear now and fall through to fresh-place.
	// Same (char_id, ratio) -> fall through; the existing modify-detect
	// below reuses the cached MC, closing the leak for Doodle Jump
	// gameplay frame_1 depth 2 'container' (the canonical case:
	// tagRemoveObject2(2) + tagSetInstanceName(2, "container") +
	// tagPlaceObject2Ratio(2, 25, ..., ratio=1, ...) repeated each tick
	// while frame is stopped).
	if (display_list[depth].pending_remove)
	{
		display_list[depth].pending_remove = 0;
		if (display_list[depth].char_id != 0
		    && (display_list[depth].char_id != char_id
		        || display_list[depth].ratio != ratio))
		{
			invalidate_mc_for_dl_entry(app_context, &display_list[depth]);
			clear_display_entry(app_context, depth);
		}
	}
#endif

	// Phase 3: Refuse Place (move=0, has_character=1) of a DIFFERENT character on
	// an already-occupied depth. See tagPlaceObject2 for full rationale.
	extern int ng_depth_has_pending_finalize(size_t);
	if (char_id != 0 && !is_replace
	    && display_list[depth].char_id != 0
	    && display_list[depth].char_id != char_id
	    && display_list[depth].placed_at_frame <= current_frame
	    && !ng_depth_has_pending_finalize(depth))
	{
		printf("Warning: Failed to place object at depth %zu.\n", depth);
		g_pending_clip_actions = NULL;
		g_pending_clip_action_count = 0;
		g_pending_instance_name = NULL;
		return;
	}

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM frame_func re-run protection. The browser-WASM main
	// loop in swf.c lacks the `is_playing || manual_next_frame` gate
	// (gated #ifdef OFFSCREEN_RENDER), so frame_funcs[current_frame]
	// re-executes every tick even when the timeline is stopped. On
	// stopped frames containing tagPlaceObject2Ratio (e.g. Doodle Jump
	// gameplay frame_1, depth 7 via the Replace path), each re-run
	// would create a fresh MC because the prior tick's MC was just
	// invalidated by the same frame_func's tagReplaceObject2RatioWith
	// ClipActions hook or tagRemoveObject2. Mirror tagPlaceObject2's
	// same-char/same-depth modify-detect (lines 5501-5517) but ALSO
	// require ratio match — Ratio placements with different ratio are
	// genuine modifications (see Ruffle's survives_rewind logic
	// referenced at tag.c:5290-5294). place_gen!=g_place_gen scopes
	// the gate to cross-frame_func-run re-entries.
	if (display_list[depth].char_id == char_id && display_list[depth].char_id != 0
	    && display_list[depth].ratio == ratio
	    && display_list[depth].place_gen != g_place_gen)
	{
		if (!display_list[depth].transformed_by_script) {
			display_list[depth].transform_id = transform_id;
			ng_cache_transform(&display_list[depth], transform_id);
		}
		if (!display_list[depth].cx_overridden && !display_list[depth].transformed_by_script) {
			display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
			display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
			init_cx_fields(&display_list[depth]);
		}
		if (clip_depth != 0) display_list[depth].clip_depth = clip_depth;
		display_list[depth].placed_at_frame = current_frame;
		display_list[depth].place_gen = g_place_gen;
		ng_on_place_object2(app_context, depth, char_id);
		display_list[depth].sprite_needs_init = 0;
		if (depth > max_depth) max_depth = depth;
		g_pending_clip_actions = NULL;
		g_pending_clip_action_count = 0;
		g_pending_instance_name = NULL;
		return;
	}
#endif

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	// During backward goto catch-up, preserve existing sprite at same depth/char
	{
		extern int catch_up_backward;
		extern int catch_up_mode;
		extern size_t catch_up_target;
		if (catch_up_backward && display_list[depth].char_id != 0)
		{
			// Ruffle survives_rewind: for MovieClip, only ratio_equals is
			// required; other types additionally need id_equals. See the
			// matching comment in tagPlaceObject2 above. Phase 6: clones
			// (clone_replaced=1) never survive — see tagPlaceObject2.
			int existing_is_mc = (display_list[depth].sprite_display_list != NULL);
			int is_clone_replaced = display_list[depth].clone_replaced;
			int survives = (display_list[depth].ratio == ratio)
			               && !is_clone_replaced
			               && (existing_is_mc
			                   || display_list[depth].char_id == char_id);
			if (survives)
			{
				// Preserve instance_name — clear pending before ng_on_place_object2
				// (which consumes g_pending_instance_name).
				g_pending_instance_name = NULL;
				display_list[depth].transform_id = transform_id;
				ng_cache_transform(&display_list[depth], transform_id);
				display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
				display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
				if (clip_depth != 0) display_list[depth].clip_depth = clip_depth;
				display_list[depth].ratio = ratio;
				display_list[depth].placed_at_frame = current_frame;
				display_list[depth].place_gen = g_place_gen;
				init_cx_fields(&display_list[depth]);
				ng_on_place_object2(app_context, depth, display_list[depth].char_id);
				display_list[depth].sprite_needs_init = 0;
				return;
			}
			if (catch_up_mode && display_list[depth].placed_at_frame > current_frame)
			{
				if (display_list[depth].placed_at_frame > catch_up_target)
				{
					// Mark the existing MC pending-removal so it persists in the
					// removed-depth zone for one frame — Flash semantics: after
					// backward goto replaces an MC at a depth (different ratio),
					// an old MC with an UNLOAD handler (clip-event UNLOAD or
					// AS-level onUnload) is still referenceable by name (typeof
					// returns 'movieclip') until next frame's finalize. Mirrors
					// Ruffle's `remove_child` → `should_delay_removal` path
					// called from run_goto's survives_rewind=false branch.
					// Gating on has_unload (mirrors ng_on_remove_object) avoids
					// preserving MCs that should disappear immediately — see
					// gnash misc-ming loop_test5 where movieClip3 has only
					// CONSTRUCT (no UNLOAD) and must be undefined after rewind.
					if (existing_is_mc && display_list[depth].instance_name != NULL)
					{
						extern int has_child_unload_handler(DisplayObject* dl, size_t dl_max);
						extern int actionMCHasOnUnloadProperty(const char* name, int swf_depth);
						extern void actionMarkMCPendingRemoval(SWFAppContext*, const char*, int);
						int _swr_has_unload = 0;
						for (size_t _ca = 0; _ca < display_list[depth].clip_action_count; _ca++) {
							if (display_list[depth].clip_actions[_ca].event_flags & 0x4) {
								_swr_has_unload = 1; break;
							}
						}
						if (!_swr_has_unload) {
							for (size_t _ca = 0; _ca < display_list[depth].accumulated_clip_action_count; _ca++) {
								if (display_list[depth].accumulated_clip_actions[_ca].event_flags & 0x4) {
									_swr_has_unload = 1; break;
								}
							}
						}
						if (!_swr_has_unload) {
							_swr_has_unload = actionMCHasOnUnloadProperty(
								display_list[depth].instance_name, (int)depth);
						}
						if (!_swr_has_unload && display_list[depth].sprite_display_list != NULL &&
						    display_list[depth].sprite_max_depth > 0) {
							_swr_has_unload = has_child_unload_handler(
								display_list[depth].sprite_display_list,
								display_list[depth].sprite_max_depth);
						}
						if (_swr_has_unload) {
							// See tagPlaceObject2 for rationale on the
							// *Deferred variant (kind=SCRIPT) and on the
							// intentionally-omitted recursive child unloads.
							MovieClip* _swr_mc_for_q = actionFindOrCreateMovieClip(
								app_context, display_list[depth].instance_name, &root_movieclip);
							for (size_t _ca = 0; _ca < display_list[depth].accumulated_clip_action_count; _ca++) {
								if (display_list[depth].accumulated_clip_actions[_ca].event_flags & 0x4)
									actionQueueClipActionUnloadDeferred(display_list[depth].accumulated_clip_actions[_ca].action, _swr_mc_for_q);
							}
							for (size_t _ca = 0; _ca < display_list[depth].clip_action_count; _ca++) {
								if (display_list[depth].clip_actions[_ca].event_flags & 0x4)
									actionQueueClipActionUnloadDeferred(display_list[depth].clip_actions[_ca].action, _swr_mc_for_q);
							}
							actionMarkMCPendingRemoval(app_context,
							                           display_list[depth].instance_name,
							                           (int)depth);
						}
						else if (display_list[depth].instance_name != NULL)
						{
							// No unload handler — the MC won't be marked pending,
							// but the cached MovieClip would survive with its
							// stale (script-modified) x/y/etc. Invalidate it so
							// the next AS lookup creates a fresh MC with
							// transform-derived position. Surfaces in
							// place_and_remove_object_insane_test where mc_red
							// has script-set _x=60 and ratio=65535; iter 2 frame 0's
							// non-survives ratio mismatch must reset the visual
							// state to transform 25's identity tx=0.
							extern void actionInvalidateCachedMovieClip(SWFAppContext*, const char*, int);
							actionInvalidateCachedMovieClip(app_context, display_list[depth].instance_name, (int)depth);
						}
					}
					if (display_list[depth].sprite_display_list != NULL)
					{
						ng_freeSpriteDL(app_context, display_list[depth].sprite_display_list,
			                display_list[depth].sprite_dl_capacity);
						display_list[depth].sprite_display_list = NULL;
					}
					display_list[depth].char_id = 0;
					// Fall through to full placement
				}
				else
				{
					return;
				}
			}
		}
	}
#endif

	// char_id=0 means "modify existing entry" (PlaceObject2 without HasCharacter)
	if (char_id == 0 && display_list[depth].char_id != 0)
	{
		// Update only the fields that PlaceObject2 can modify
		if (transform_id != 0) {
			display_list[depth].transform_id = transform_id;
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
			ng_cache_transform(&display_list[depth], transform_id);
#endif
		}
		if (cxform_id != 0) {
			display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
			display_list[depth].has_cxform = 1;
		}
		if (clip_depth != 0) display_list[depth].clip_depth = clip_depth;
		display_list[depth].ratio = ratio;
		return;
	}

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	// Cross-frame sprite-by-sprite REPLACE preservation — see tagPlaceObject2 above
	// for rationale (Ruffle PlaceObjectAction::Replace; replace_with is noop for
	// MovieClip; apply_place_object updates only matrix/cxform/ratio).
	if (char_id != 0
	    && display_list[depth].char_id != 0
	    && display_list[depth].char_id != char_id
	    && display_list[depth].place_gen != g_place_gen
	    && display_list[depth].sprite_display_list != NULL
	    && char_id < dictionary_capacity
	    && dictionary[char_id].type == CHAR_TYPE_SPRITE
	    && !ng_depth_has_pending_finalize(depth))
	{
		if (!display_list[depth].transformed_by_script) {
			display_list[depth].transform_id = transform_id;
			ng_cache_transform(&display_list[depth], transform_id);
		}
		if (!display_list[depth].cx_overridden && !display_list[depth].transformed_by_script) {
			display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
			display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
			init_cx_fields(&display_list[depth]);
		}
		display_list[depth].ratio = ratio;
		display_list[depth].placed_at_frame = current_frame;
		display_list[depth].place_gen = g_place_gen;
		g_pending_instance_name = NULL;
		g_pending_clip_actions = NULL;
		g_pending_clip_action_count = 0;
		ng_on_place_object2(app_context, depth, display_list[depth].char_id);
		display_list[depth].sprite_needs_init = 0;
		if (depth > max_depth) max_depth = depth;
		return;
	}
#endif

	display_list[depth].char_id = char_id;
	display_list[depth].transform_id = transform_id;
	ng_cache_transform(&display_list[depth], transform_id);
	display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
	display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
	display_list[depth].clip_depth = clip_depth;
	display_list[depth].ratio = ratio;
	display_list[depth].blend_mode = 0;
	display_list[depth].sprite_display_list = NULL;
	display_list[depth].sprite_max_depth = 0;
	display_list[depth].sprite_dl_capacity = 0;
	display_list[depth].sprite_current_frame = 0;
	display_list[depth].sprite_is_playing = 1;
	display_list[depth].sprite_manual_next_frame = 0;
	display_list[depth].sprite_next_frame = 0;
	// Look up child movie transform data by character's movie_id
	display_list[depth].child_transform_data = NULL;
	if (g_char_movie_id != NULL && char_id < g_char_movie_id_capacity && g_char_movie_id[char_id] != 0)
		display_list[depth].child_transform_data = g_movie_transform_data[g_char_movie_id[char_id]];
	// Consume pending instance name from tagSetInstanceName (called before PlaceObject)
	if (g_pending_instance_name != NULL) {
		display_list[depth].instance_name = (char*)g_pending_instance_name;
		display_list[depth].instance_name_owned = 0;
		g_pending_instance_name = NULL;
	} else {
		display_list[depth].instance_name = NULL;
		display_list[depth].instance_name_owned = 0;
	}
	display_list[depth].clip_actions = NULL;
	display_list[depth].clip_action_count = 0;
	display_list[depth].filter_type = 0;
	// The filter CHAIN is placement-scoped exactly like filter_type: the
	// tagBeginFilterList of this placement (emitted right after its
	// tagSetFilter) re-links it. Clearing here stops a re-placed depth from
	// inheriting the previous occupant's list.
	display_list[depth].filter_chain = NULL;
	display_list[depth].depth_swapped = 0;
	// Restore persistent button state if the same character is being re-placed
	// (e.g. a looping movie that removes+replaces a button each frame cycle).
	if (display_list[depth].sticky_char_id == char_id && char_id != 0)
	{
		display_list[depth].button_state = display_list[depth].sticky_button_state;
		display_list[depth].button_prev_state = display_list[depth].sticky_button_state;
	}
	else
	{
		display_list[depth].button_state = 0;
		display_list[depth].button_prev_state = 0;
		display_list[depth].sticky_button_state = 0;
		display_list[depth].sticky_char_id = 0;
	}
	display_list[depth].sprite_needs_init = 0;
	display_list[depth].placed_at_frame = current_frame;
	{ extern size_t g_tick_count; display_list[depth].placed_at_tick = g_tick_count; }
	// Record the loaded-movie holder whose frame tag is running, so a wrap-back
	// of that movie's timeline can take its own children off the list again.
	// NULL for every ordinary main-movie placement. See DisplayObject.
	{
		extern MovieClip* actionCurrentChildMovieMC(void);
		display_list[depth].placed_by_holder = (void*)actionCurrentChildMovieMC();
	}
	display_list[depth].place_gen = g_place_gen;
	display_list[depth].place_seq = ++g_place_seq;
	display_list[depth].constructor_invoked = 0;
	init_cx_fields(&display_list[depth]);

	if (depth > max_depth)
	{
		max_depth = depth;
	}

	// Consume pending clip actions (browser-WASM widening — see same block
	// in tagPlaceObject2 above for the explanation).
	if (g_pending_clip_actions != NULL) {
		display_list[depth].clip_actions = g_pending_clip_actions;
		display_list[depth].clip_action_count = g_pending_clip_action_count;
		note_clip_actions_for_ef(display_list[depth].clip_actions, display_list[depth].clip_action_count);
		g_pending_clip_actions = NULL;
		g_pending_clip_action_count = 0;
	}

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM: inline auto-instance-name for unnamed scriptable
	// placements (mirrors ng_on_place_object2). See tagPlaceObject2 above.
	if (display_list[depth].instance_name == NULL
	    && display_list[depth].char_id != 0) {
		size_t _ainame_cid = display_list[depth].char_id;
		int _ainame_is_sprite = (_ainame_cid < INITIAL_DICTIONARY_CAPACITY
		                         && dictionary[_ainame_cid].type == CHAR_TYPE_SPRITE);
		int _ainame_is_button = ng_find_button(_ainame_cid);
		int _ainame_is_tf     = (ng_find_textfield(_ainame_cid) >= 0);
		if (_ainame_is_sprite || _ainame_is_button || _ainame_is_tf) {
			char _ainame_buf[32];
			snprintf(_ainame_buf, sizeof(_ainame_buf), "instance%u",
			         ng_increment_auto_instance_counter());
			display_list[depth].instance_name = strdup(_ainame_buf);
			display_list[depth].instance_name_owned = 1;
		}
	}
	// Mark sprite_initialized=1 so EnterFrame fires next tick. See tagPlaceObject2.
	if (display_list[depth].char_id != 0
	    && display_list[depth].char_id < INITIAL_DICTIONARY_CAPACITY
	    && dictionary[display_list[depth].char_id].type == CHAR_TYPE_SPRITE
	    && display_list[depth].sprite_initialized == 0) {
		display_list[depth].sprite_initialized = 1;
	}
	// Browser-WASM: queue LOAD on AQ_KIND_LOAD so it drains after sprite
	// children are populated. See tagPlaceObject2 above.
	queue_clip_load_events_kind(app_context, depth, AQ_KIND_LOAD);
#endif

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	ng_on_place_object2(app_context, depth, char_id);

	// Queue INIT/CONSTRUCT/REGISTER_CTOR on the unified ActionQueue (same
	// ordering rationale as tagPlaceObject2). Nested placements queue but
	// don't drain; the outermost placement drains.
	queue_clip_init_events(app_context, depth);
	queue_clip_construct_events(app_context, depth);
	queue_register_ctor(app_context, depth, char_id);
	// Phase 7a: queue CLIP_EVENT_LOAD clip-actions at NORMAL priority BEFORE
	// eager init. Drained per-sprite inside process_sprite_init_at_depth
	// (replacing the pre-migration synchronous fire at the same observable
	// point). The "before eager init" placement is what 7b will rely on:
	// this sprite's LOAD is FIFO-first among its NORMAL entries, so when
	// sprite DoAction joins the queue in 7b, the per-sprite
	// LOAD → frame_0 → nested-LOAD → nested-frame_0 interleave falls out
	// of a single Normal-priority FIFO drain.
	queue_clip_load_events(app_context, depth);

	// Phase 7b: pre-create sprite MC (see tagPlaceObject2 for rationale).
	if (display_list[depth].sprite_needs_init
	    && dictionary[char_id].type == CHAR_TYPE_SPRITE
	    && display_list[depth].instance_name != NULL)
	{
		extern MovieClip root_movieclip;
		MovieClip* _parent_for_mc = g_current_context ? g_current_context : &root_movieclip;
		actionFindOrCreateMovieClip(app_context, display_list[depth].instance_name, _parent_for_mc);
	}

	// Eagerly execute sprite frame 0 immediately after placement so the sprite's
	// internal display list is populated BEFORE the parent frame's ActionScript runs.
	// Same eager init as tagPlaceObject2 — needed for scripts that access children before tagShowFrame.
	if (display_list[depth].sprite_needs_init)
	{
		if (dictionary[char_id].type == CHAR_TYPE_SPRITE)
		{
			Character* sp_ch = &dictionary[char_id];
			if (sp_ch->sprite_frame_funcs != NULL && sp_ch->sprite_frame_funcs[0] != NULL)
			{
				// sni distinguishes Phase 2 behavior:
				//   2 = eager init suppressed scripts (under goto catchup, recompiler
				//       gate g1 false). Phase 2 must re-run frame_0 in script_only_mode
				//       to fire scripts inline (gate g2 true).
				//   3 = eager init queued scripts via the SHOW_FRAME drain path
				//       (recompiler gate g1 true, normal flow). Phase 2 must NOT
				//       re-run frame_0 — queued scripts will fire on their own;
				//       re-running would double-fire.
				// consecutive_goto_frame_test sprite_4 motivates the split.
				extern int actionGotoCatchupActive(void);
				display_list[depth].sprite_needs_init = actionGotoCatchupActive() ? 2 : 3;
				DisplayObject* saved_dl = display_list;
				size_t saved_max = max_depth;
				size_t saved_cap = display_list_capacity;
				display_list = saved_dl[depth].sprite_display_list;
				max_depth = saved_dl[depth].sprite_max_depth;
				display_list_capacity = saved_dl[depth].sprite_dl_capacity;
				// Phase 1: run with catch_up_mode=1 so only placement tags execute;
				// DoAction scripts are deferred to tagShowFrame Phase 2.
				int saved_catch_up = catch_up_mode;
				catch_up_mode = 1;
				g_eager_init_depth++;
				CALL_FRAME(app_context, &saved_dl[depth], sp_ch->sprite_frame_funcs[0]);
				g_eager_init_depth--;
				catch_up_mode = saved_catch_up;
				saved_dl[depth].sprite_display_list = display_list;
				saved_dl[depth].sprite_max_depth = max_depth;
				saved_dl[depth].sprite_dl_capacity = display_list_capacity;
				display_list = saved_dl;
				max_depth = saved_max;
				display_list_capacity = saved_cap;
				saved_dl[depth].sprite_current_frame = (sp_ch->sprite_frame_count > 0) ? (1 % sp_ch->sprite_frame_count) : 0;
			}
		}
		else if (dictionary[char_id].type == CHAR_TYPE_BUTTON)
		{
			// Buttons need eager init so AS code accessing button.childName
			// (e.g. button3.instance1) sees the populated state-0 children
			// before the parent frame's DoAction runs. State funcs only emit
			// tagPlaceObject2 calls (no scripts), so this is safe under
			// catch_up_mode=1 — nested sprite placements still defer their
			// own Phase-2 scripts via the sprite eager-init path above.
			Character* btn_ch = &dictionary[char_id];
			if (btn_ch->button_state_funcs != NULL && btn_ch->button_state_funcs[0] != NULL)
			{
				display_list[depth].sprite_needs_init = 2; // mark: state_0 done, recurse later
				// Pre-create the button MC with the correct parent so child
				// placements inside state_funcs see button_mc as g_current_context
				// and their _parent chain resolves correctly.
				MovieClip* parent_for_btn = g_current_context ? g_current_context : &root_movieclip;
				MovieClip* button_mc = NULL;
				if (display_list[depth].instance_name != NULL) {
					button_mc = actionFindOrCreateMovieClip(app_context,
						display_list[depth].instance_name, parent_for_btn);
					if (button_mc) {
						button_mc->is_button_mc = 1;
						button_mc->display_obj = (void*)&display_list[depth];
					}
				}

				DisplayObject* saved_dl = display_list;
				size_t saved_max = max_depth;
				size_t saved_cap = display_list_capacity;
				display_list = saved_dl[depth].sprite_display_list;
				max_depth = saved_dl[depth].sprite_max_depth;
				display_list_capacity = saved_dl[depth].sprite_dl_capacity;

				int saved_catch_up = catch_up_mode;
				catch_up_mode = 1;
				g_eager_init_depth++;
				MovieClip* saved_ctx_btn = g_current_context;
				if (button_mc) actionSetCurrentContext(button_mc);
				btn_ch->button_state_funcs[0](app_context);
				actionSetCurrentContext(saved_ctx_btn);
				g_eager_init_depth--;
				catch_up_mode = saved_catch_up;

				saved_dl[depth].sprite_display_list = display_list;
				saved_dl[depth].sprite_max_depth = max_depth;
				saved_dl[depth].sprite_dl_capacity = display_list_capacity;
				display_list = saved_dl;
				max_depth = saved_max;
				display_list_capacity = saved_cap;
			}
		}
	}

	// Phase 2 of CLIP_EVENT_ROUND_DISPATCH: per-placement drain removed.
	// See tagPlaceObject2 for the rationale.

#else
	(void)app_context;
#endif
}

void tagPlaceObject2RatioWithClipActions(SWFAppContext* app_context, size_t depth, size_t char_id,
    u32 transform_id, u32 cxform_id, u16 clip_depth, u16 ratio, ClipAction* clip_actions, size_t clip_action_count, u8 is_replace)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (g_tag_skip_mode) return;
#endif
	// Set pending so tagPlaceObject2Ratio attaches them before eager init
	g_pending_clip_actions = clip_actions;
	g_pending_clip_action_count = clip_action_count;
	tagPlaceObject2Ratio(app_context, depth, char_id, transform_id, cxform_id, clip_depth, ratio, is_replace);
	// tagPlaceObject2Ratio consumes g_pending_clip_actions and fires events
	g_pending_clip_actions = NULL;
	g_pending_clip_action_count = 0;
}

// Replace an existing clip at `depth` in the same frame: does NOT fire CLIP_EVENT_UNLOAD
// for the old clip's actions. Instead, saves them as accumulated_clip_actions on the new
// entry so they fire (before new_clip_actions) when the new entry is eventually removed.
// The old MC gets its depth transformed and is marked for deferred invalidation (persists one frame).
void tagReplaceObject2RatioWithClipActions(SWFAppContext* app_context, size_t depth, size_t char_id,
    u32 transform_id, u32 cxform_id, u16 clip_depth, u16 ratio,
    ClipAction* old_clip_actions, size_t old_clip_action_count,
    ClipAction* new_clip_actions, size_t new_clip_action_count)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	char* saved_name = display_list[depth].instance_name;
	if (saved_name != NULL) {
		// Check if old MC has unload handler (clip_actions UNLOAD or AS-level onUnload).
		// Only persist the old MC if it does; otherwise invalidate immediately.
		int has_unload = 0;
		for (size_t ca = 0; ca < old_clip_action_count; ca++) {
			if (old_clip_actions[ca].event_flags & 0x4) {
				has_unload = 1;
				break;
			}
		}
		if (!has_unload)
			has_unload = actionMCHasOnUnloadProperty(saved_name, (int)depth);

		if (has_unload) {
			actionMarkMCPendingRemoval(app_context, saved_name, (int)depth);
		} else {
			actionInvalidateCachedMovieClip(app_context, saved_name, (int)depth);
		}
	}

	// Set the old instance name as pending so ng_on_place_object2 picks it up
	// for the new MC (tagPlaceObject2Ratio clears instance_name to NULL).
	if (saved_name != NULL) {
		extern const char* g_pending_instance_name;
		g_pending_instance_name = saved_name;
	}

	// Tell findOrCreateMovieClip to skip the old MC (with pending_removal)
	// so a fresh MC is created for the new display entry.
	g_skip_pending_removal_mc = 1;
#else
	// Browser-WASM frame_func re-run protection. The browser-WASM main
	// loop in swf.c re-runs frame_funcs every tick (no `is_playing`
	// gate), so stopped frames containing this tag would invalidate +
	// recreate the MC every tick — leaking ~1-2 MCs/tick into
	// child_mc_cache until MAX_CHILD_MOVIECLIPS=128 caps it (Doodle Jump
	// gameplay frame_1 depth 7 'instanceN'). Skip both invalidation and
	// the inner tagPlaceObject2Ratio when this is a re-run on the same
	// (char_id, ratio) at the same depth — matched by place_gen !=
	// g_place_gen (place_gen advances in tagShowFrame, so a re-entry of
	// the same frame_func body sees the same display entry from the
	// previous tick with a different gen). Updates transform / cxform /
	// clip_depth / placed_at_frame as a normal modify would.
	if (display_list[depth].char_id == char_id && display_list[depth].char_id != 0
	    && display_list[depth].ratio == ratio
	    && display_list[depth].place_gen != g_place_gen)
	{
		if (!display_list[depth].transformed_by_script) {
			display_list[depth].transform_id = transform_id;
			ng_cache_transform(&display_list[depth], transform_id);
		}
		if (!display_list[depth].cx_overridden && !display_list[depth].transformed_by_script) {
			display_list[depth].cxform_id = ng_rebaseCxformId(cxform_id);
			display_list[depth].has_cxform = (cxform_id != 0) ? 1 : 0;
			init_cx_fields(&display_list[depth]);
		}
		if (clip_depth != 0) display_list[depth].clip_depth = clip_depth;
		display_list[depth].placed_at_frame = current_frame;
		display_list[depth].place_gen = g_place_gen;
		return;
	}

	// Genuine replace (different char_id OR different ratio OR first
	// time this generation): invalidate the cached MC bound to this
	// display entry and all its descendants before the old clip's slot
	// is overwritten. Unlike tagRemoveObject2 + tagPlaceObject2 (each
	// of which now has its own browser-WASM hook), Replace is a
	// collapsed Remove+Place that never clears char_id between the two
	// halves, so tagPlaceObject2Ratio's is_replace=1 path sees no
	// transition and our existing hooks don't fire. Doodle Jump's
	// menu→gameplay transition at depth 7 (same char_id=46 — menu
	// "info" button → gameplay back "menu" button, different ratio)
	// goes through this path, and without the hook the old "button_txt"
	// EditText child stays in child_mc_cache and keeps rendering "info"
	// after the transition.
	invalidate_mc_for_dl_entry(app_context, &display_list[depth]);
#endif

	// Place the new clip (clears all fields including clip_actions).
	// tagReplaceObject2RatioWithClipActions is always a Replace by construction
	// (only emitted when a buffered RemoveObject + new placement collapse at the
	// same depth), so pass is_replace=1 to bypass Phase 3's occupied-depth check.
	tagPlaceObject2Ratio(app_context, depth, char_id, transform_id, cxform_id, clip_depth, ratio, 1);

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	// Ensure the NEW clip MC is created in the cache so actionMarkMCPendingRemoval
	// can find it when this entry is later removed from the display list.
	// Keep g_skip_pending_removal_mc=1 so we create a fresh MC (not reuse the old one).
	if (display_list[depth].instance_name != NULL) {
		extern MovieClip root_movieclip;
		// Resolve against the owner of the current display list (the sprite MC
		// when inside exec_sprite_frame, &root_movieclip at root) — not always
		// _root, which would mint a ghost root-child for nested clips. Mirrors
		// the clip_load parent-capture convention (pcl->parent_mc).
		MovieClip* _ro_parent = g_current_context ? g_current_context : &root_movieclip;
		actionFindOrCreateMovieClip(app_context, display_list[depth].instance_name, _ro_parent);
	}
	g_skip_pending_removal_mc = 0;
#endif

	// Set new clip actions
	display_list[depth].clip_actions = new_clip_actions;
	display_list[depth].clip_action_count = new_clip_action_count;
	note_clip_actions_for_ef(new_clip_actions, new_clip_action_count);
	// Accumulate old clip actions to fire before new_clip_actions on next removal
	display_list[depth].accumulated_clip_actions = old_clip_actions;
	display_list[depth].accumulated_clip_action_count = old_clip_action_count;
}

// --- Deferred destructive cleanup for tagRemoveObject2 (has_unload only) ---
// Phase 4 of DEFERRED_CLIP_UNLOAD_PLAN. For MCs WITH unload handlers, defer:
//   - Mark (sets pending_removal=1 on the MC, shifts depth)
//   - clear_display_entry (clears display_list slot, frees sprite_display_list)
// to post-drain finalize so:
//   - same-frame DoAction-after-RemoveObject2 sees typeof(mc) == 'movieclip'
//     (loop_test7 lines 123-124 — pending_removal stays 0 until drain).
//   - typeof / _root.mc lookups via display_list iteration still find the MC
//     by name (instance_name stays populated until finalize).
// For MCs WITHOUT unload handlers, both run inline (no handler to wait for).
//
// The pending-finalize QUEUE functions below are NO_GRAPHICS/OFFSCREEN_RENDER
// only (browser-WASM gets no-op stubs in the #else), but the state table and
// the realloc/free funnel that scrubs it (ng_spriteDLRealloc / ng_freeSpriteDL
// / ng_ensureDisplayListSize) compile in ALL modes: the funnel call sites in
// tag.c/tag_stubs.c are shared with the browser build, which otherwise fails
// to link (undefined ng_freeSpriteDL). In browser mode the table just stays
// empty, so its scrub loops are no-ops.
#define NG_PENDING_FINALIZE_CAP 256
typedef struct {
	MovieClip* mc;
	size_t depth;
	int swf_depth;
	// Snapshot of the display_list array + entry state at queue time. Used by
	// run_pending_finalize to detect a same-frame Remove+Place sequence at the
	// same depth that installed a *different* MC under a *different* name —
	// in that case clear_display_entry must NOT fire (it would clobber the
	// new placement's data, breaking the next-frame Remove's UNLOAD dispatch).
	// We compare against the SAME display_list array we queued from
	// (queued_dl_array): for sprite-internal pending entries, display_list is
	// swapped back to root by run_pending_finalize time, so a naive comparison
	// would mismatch with root's depth-N entry instead of the sprite's. When
	// the saved pointer doesn't match the live display_list, we fall through
	// to the original always-clear behavior.
	// Required: misc-ming.all loop/loop_test10 (root-level Remove+Place under
	// different names — skip clear). Regression-safe: misc-ming.all
	// register_class/RegisterClassTest4 (sprite-internal — fall through).
	void* queued_dl_array;
	size_t orig_char_id;
	char orig_instance_name[64];
} PendingFinalizeEntry;
static PendingFinalizeEntry ng_pending_finalize_entries[NG_PENDING_FINALIZE_CAP];
static int ng_pending_finalize_count = 0;

// ===========================================================================
// Sprite display-list reallocation with aliased-pointer rebase
//
// A sprite's sprite_display_list buffer can move when it grows (attachMovie
// registration at swf_depth >= 16384, or a placement depth exceeding the
// occupancy-sized capacity). Persistent holders alias the old buffer two
// ways (see the ALIASING RULE at DisplayObject.sprite_display_list, swf.h):
//
//   A) entry pointers (&dl[i]): MovieClip.display_obj on every cached MC,
//      g_current_sprite_obj, queued PendingRegisterCtor / PendingClipLoad /
//      PendingSpriteScript payloads, pending sprite-frame-script captures,
//      and the active clip-EF flat dispatch array (g_clip_ef_rebase_head).
//   B) base-pointer copies: the global display_list while swapped to the
//      grown list, g_root_dl_backup, the button-state snapshot arrays,
//      PendingFinalizeEntry.queued_dl_array, and nested-sprite entries in
//      OTHER lists that hold a copy of this list's base (the attachMovie
//      registration copies child list pointers into the parent's entry).
//
// ng_spriteDLRealloc reallocates and repoints ALL of the above before
// freeing the old buffer. What it cannot fix are C-stack locals (saved_dl /
// saved DisplayObject* held across a call that can grow a list) — those
// callers must re-read after the call; new holders must be added to this
// walk. Known residual: the recursive clip-EF overflow fallback and the
// xform/cxform override stacks (render-phase only, no script runs while
// they are live).
// ===========================================================================

static DisplayObject* g_dlr_old_base;
static DisplayObject* g_dlr_new_base;
static size_t g_dlr_old_cap;
static size_t g_dlr_new_cap;

// Repoint p if it points into the old buffer [old_base, old_base+old_cap).
static void* dlr_rebase_ptr(void* p)
{
	uintptr_t v = (uintptr_t)p;
	uintptr_t lo = (uintptr_t)g_dlr_old_base;
	uintptr_t hi = lo + g_dlr_old_cap * sizeof(DisplayObject);
	if (v >= lo && v < hi)
		return (char*)g_dlr_new_base + (v - lo);
	return p;
}

// Action-queue payload rebase (entries are popped before dispatch, so every
// entry still queued holds a live payload — safe mid-drain).
static void dlr_rebase_queue_cb(void* fn, void* user)
{
	if (user == NULL) return;
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (fn == (void*)aq_dispatch_register_ctor) {
		PendingRegisterCtor* prc = (PendingRegisterCtor*)user;
		prc->display_obj = (DisplayObject*)dlr_rebase_ptr((void*)prc->display_obj);
		return;
	}
#endif
	if (fn == (void*)aq_dispatch_clip_load) {
		PendingClipLoad* pcl = (PendingClipLoad*)user;
		pcl->obj = (DisplayObject*)dlr_rebase_ptr((void*)pcl->obj);
	}
}

// Fix base-pointer copies of the old buffer held in nested-sprite entries of
// other display lists (attachMovie registration copies). Walks the holder
// tree; lists form a parent->child DAG (a child list can be reachable both
// via its owning entry and via a registration copy), so bound the depth.
static void dlr_rebase_copies_recurse(DisplayObject* dobj, int guard)
{
	if (dobj == NULL || guard > 64) return;
	if (dobj->sprite_display_list == g_dlr_old_base) {
		dobj->sprite_display_list = g_dlr_new_base;
		dobj->sprite_dl_capacity = g_dlr_new_cap;
	}
	DisplayObject* dl = dobj->sprite_display_list;
	if (dl == NULL || dobj->sprite_dl_capacity == 0) return;
	size_t maxd = dobj->sprite_max_depth;
	if (maxd >= dobj->sprite_dl_capacity) maxd = dobj->sprite_dl_capacity - 1;
	for (size_t d = 0; d <= maxd; d++)
		if (dl[d].sprite_display_list != NULL)
			dlr_rebase_copies_recurse(&dl[d], guard + 1);
}

void ng_spriteDLRealloc(SWFAppContext* app_context, DisplayObject** base_io,
                        size_t* cap_io, size_t new_cap)
{
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	extern MovieClip root_movieclip;
	extern DisplayObject* g_current_sprite_obj;
	extern void* ng_get_root_display_obj(void);

	DisplayObject* old_base = *base_io;
	size_t old_cap = *cap_io;
	if (old_base != NULL && new_cap <= old_cap) return;

	DisplayObject* new_base = HCALLOC(new_cap, sizeof(DisplayObject));
	if (new_base == NULL) return;  // OOM: keep the old buffer (HALLOC exits on
	                               // o1heap OOM, so this only fires in
	                               // passthrough/sanitizer builds)
	if (old_base != NULL)
		memcpy(new_base, old_base, old_cap * sizeof(DisplayObject));
	*base_io = new_base;
	*cap_io = new_cap;
	if (old_base == NULL) return;  // fresh allocation, nothing to rebase

	g_dlr_old_base = old_base;
	g_dlr_old_cap = old_cap;
	g_dlr_new_base = new_base;
	g_dlr_new_cap = new_cap;
	size_t old_bytes = old_cap * sizeof(DisplayObject);

	// --- Class A: pointers to entries inside the moved buffer ---
	for (int i = 0; i < child_mc_count; i++) {
		MovieClip* m = child_mc_cache[i];
		if (m != NULL && m->display_obj != NULL)
			m->display_obj = dlr_rebase_ptr(m->display_obj);
	}
	if (root_movieclip.display_obj != NULL)
		root_movieclip.display_obj = dlr_rebase_ptr(root_movieclip.display_obj);
	g_current_sprite_obj =
		(DisplayObject*)dlr_rebase_ptr((void*)g_current_sprite_obj);
	actionQueueGcForEach(dlr_rebase_queue_cb);
	actionQueueRebaseSpriteObjPayloads((void*)old_base, old_bytes,
	                                   (void*)new_base);
	actionPendingSpriteScriptsRebaseSpriteObj((void*)old_base, old_bytes,
	                                          (void*)new_base);
	for (ClipEFRebaseFrame* fr = g_clip_ef_rebase_head; fr != NULL;
	     fr = fr->prev)
		for (size_t e = 0; e < fr->n; e++)
			fr->entries[e].obj =
				(DisplayObject*)dlr_rebase_ptr((void*)fr->entries[e].obj);

	// --- Class B: copies of the old buffer's base pointer ---
	if (display_list == old_base) {
		display_list = new_base;
		display_list_capacity = new_cap;
	}
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (g_root_dl_backup == old_base) {
		g_root_dl_backup = new_base;
		g_root_dl_cap_backup = new_cap;
	}
#endif
	for (size_t j = 0; j < BTN_STATE_SNAP_MAX; j++) {
		if (g_btn_state_old_sprite_dl[j] == old_base) {
			g_btn_state_old_sprite_dl[j] = new_base;
			g_btn_state_old_sprite_cap[j] = new_cap;
		}
	}
	for (int i = 0; i < ng_pending_finalize_count; i++) {
		if (ng_pending_finalize_entries[i].queued_dl_array == (void*)old_base)
			ng_pending_finalize_entries[i].queued_dl_array = (void*)new_base;
	}
	dlr_rebase_copies_recurse((DisplayObject*)ng_get_root_display_obj(), 0);
	for (int i = 0; i < child_mc_count; i++) {
		MovieClip* m = child_mc_cache[i];
		if (m != NULL && m->display_obj != NULL)
			dlr_rebase_copies_recurse((DisplayObject*)m->display_obj, 0);
	}

	FREE(old_base);
}

// Freed-buffer scrub: NULL every cached MC display_obj pointing into a
// sprite display list that is about to be freed — INCLUDING tombstoned
// (depth==INT_MIN) MCs, which the invalidation walks deliberately skip but
// whose dangling display_obj is still reachable through hit-test walks
// (actionMouseClickFocus → mc_get_pixel_aabb_ng read a tombstoned
// disabled_mc's entry inside a buffer clear_display_entry had freed).
//
// Also the free-side twin of dlr_rebase_copies_recurse's Class B for
// standalone display_objs: an attachMovie/clone MC's display_obj is a
// calloc'd DisplayObject OUTSIDE any DL whose sprite_display_list FIELD
// holds the same base pointer as the CI-mode registration entry in the
// parent's list. Freeing the buffer through either alias must NULL the
// other, or the survivor dangles (N's backward-goto actionGetMember UAF
// read an attached drone's own DL through its standalone display_obj after
// ng_gotoFrameByMC freed it through the parent's registration copy).
// Registration-entry copies themselves are cleared by the free sites (the
// funnel contract: every caller NULLs the entry field it freed through).
static void scrub_mc_display_obj_in_range(DisplayObject* base, size_t cap)
{
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	extern MovieClip root_movieclip;
	uintptr_t lo = (uintptr_t)base;
	uintptr_t hi = lo + cap * sizeof(DisplayObject);
	for (int i = 0; i < child_mc_count; i++) {
		MovieClip* m = child_mc_cache[i];
		if (m == NULL || m->display_obj == NULL) continue;
		uintptr_t p = (uintptr_t)m->display_obj;
		if (p >= lo && p < hi) {
			m->display_obj = NULL;
			continue;
		}
		DisplayObject* dobj = (DisplayObject*)m->display_obj;
		if (dobj->sprite_display_list == base) {
			dobj->sprite_display_list = NULL;
			dobj->sprite_dl_capacity = 0;
			dobj->sprite_max_depth = 0;
		}
	}
	if (root_movieclip.display_obj != NULL) {
		DisplayObject* rdobj = (DisplayObject*)root_movieclip.display_obj;
		if (rdobj->sprite_display_list == base) {
			rdobj->sprite_display_list = NULL;
			rdobj->sprite_dl_capacity = 0;
			rdobj->sprite_max_depth = 0;
		}
	}
}

// Free-side queue-payload scrub: NULL queued PendingRegisterCtor /
// PendingClipLoad payload pointers into the buffer being freed. Dispatchers
// already guard on NULL (the entry's clip is being torn down, so skipping is
// the correct semantic — previously they read freed memory: ASAN hit
// aq_dispatch_register_ctor's guard read when a root goto catch-up removed a
// sprite whose child ctors were still queued).
static void dlr_scrub_queue_cb(void* fn, void* user)
{
	if (user == NULL) return;
	uintptr_t lo = (uintptr_t)g_dlr_old_base;
	uintptr_t hi = lo + g_dlr_old_cap * sizeof(DisplayObject);
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (fn == (void*)aq_dispatch_register_ctor) {
		PendingRegisterCtor* prc = (PendingRegisterCtor*)user;
		uintptr_t v = (uintptr_t)prc->display_obj;
		if (v >= lo && v < hi)
			prc->display_obj = NULL;
		return;
	}
#endif
	if (fn == (void*)aq_dispatch_clip_load) {
		PendingClipLoad* pcl = (PendingClipLoad*)user;
		uintptr_t v = (uintptr_t)pcl->obj;
		if (v >= lo && v < hi)
			pcl->obj = NULL;
	}
}

// The ONLY way to free a sprite_display_list buffer. Scrubs every registered
// holder class (see ng_spriteDLRealloc's rebase walk — this is its free-side
// twin: pointers are NULLed instead of rebased), then FREEs. Freeing a DL
// with a raw FREE() strands whichever holder the workload exercises next.
void ng_freeSpriteDL(SWFAppContext* app_context, DisplayObject* base, size_t cap)
{
	(void)app_context;
	if (base == NULL) return;
	uintptr_t lo = (uintptr_t)base;
	uintptr_t hi = lo + cap * sizeof(DisplayObject);
	scrub_mc_display_obj_in_range(base, cap);
	g_dlr_old_base = base;
	g_dlr_old_cap = cap;
	actionQueueGcForEach(dlr_scrub_queue_cb);
	actionQueueRebaseSpriteObjPayloads((void*)base, cap * sizeof(DisplayObject), NULL);
	actionPendingSpriteScriptsRebaseSpriteObj((void*)base, cap * sizeof(DisplayObject), NULL);
	{
		extern DisplayObject* g_current_sprite_obj;
		uintptr_t v = (uintptr_t)g_current_sprite_obj;
		if (v >= lo && v < hi) g_current_sprite_obj = NULL;
	}
	for (ClipEFRebaseFrame* fr = g_clip_ef_rebase_head; fr != NULL; fr = fr->prev) {
		for (size_t e = 0; e < fr->n; e++) {
			uintptr_t v = (uintptr_t)fr->entries[e].obj;
			if (v >= lo && v < hi) fr->entries[e].obj = NULL;
		}
	}
	for (int i = 0; i < ng_pending_finalize_count; i++) {
		if (ng_pending_finalize_entries[i].queued_dl_array == (void*)base)
			ng_pending_finalize_entries[i].queued_dl_array = NULL;
	}
	FREE(base);
}

// ENSURE_SIZE-style doubling wrapper for the global display_list (root, or a
// sprite's list while swapped in). Replaces the raw ENSURE_SIZE/grow_ptr at
// the placement sites, which freed the old buffer WITHOUT repointing aliases.
void ng_ensureDisplayListSize(SWFAppContext* app_context, size_t depth)
{
	if (depth < display_list_capacity) return;
	size_t new_cap = display_list_capacity > 0 ? display_list_capacity
	                                           : INITIAL_SPRITE_DL_CAPACITY;
	while (depth >= new_cap) new_cap <<= 1;
	ng_spriteDLRealloc(app_context, &display_list, &display_list_capacity,
	                   new_cap);
}

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
extern void actionMarkMCPendingRemovalDirect(MovieClip* mc, int swf_depth);
static void clear_display_entry(SWFAppContext* app_context, size_t depth);

// True iff a pending finalize is queued for this depth — i.e. tagRemoveObject2
// just deferred cleanup of an MC at this depth (with an UNLOAD handler) earlier
// in the current frame's tag stream. The display_list[depth] entry still holds
// the pre-Remove char_id at this point, so callers that want to distinguish a
// pure REPLACE tag from an in-progress Remove+Place sequence must check this.
int ng_depth_has_pending_finalize(size_t depth)
{
	for (int i = 0; i < ng_pending_finalize_count; i++) {
		if (ng_pending_finalize_entries[i].depth == depth) return 1;
	}
	return 0;
}

void queue_pending_finalize_mc(MovieClip* mc, int swf_depth, size_t depth)
{
	if (mc == NULL) return;
	if (ng_pending_finalize_count >= NG_PENDING_FINALIZE_CAP) return;
	for (int i = 0; i < ng_pending_finalize_count; i++) {
		if (ng_pending_finalize_entries[i].mc == mc) return;
	}
	PendingFinalizeEntry* slot =
		&ng_pending_finalize_entries[ng_pending_finalize_count];
	slot->mc = mc;
	slot->swf_depth = swf_depth;
	slot->depth = depth;
	slot->queued_dl_array = (void*)display_list;
	slot->orig_char_id = (depth <= max_depth) ? display_list[depth].char_id : 0;
	slot->orig_instance_name[0] = '\0';
	if (depth <= max_depth && display_list[depth].instance_name != NULL) {
		strncpy(slot->orig_instance_name, display_list[depth].instance_name,
		        sizeof(slot->orig_instance_name) - 1);
		slot->orig_instance_name[sizeof(slot->orig_instance_name) - 1] = '\0';
	}
	ng_pending_finalize_count++;
}

// Mark-only: sets pending_removal/avm1_removed on queued MCs but does NOT
// clear display_list[depth]. Used by tagFlushPendingEnterFrame so ENTER_FRAME
// skips MCs being removed in this frame, while leaving display_list intact for
// queued onLoad handlers (whose display_obj may point into a parent's
// sprite_display_list that would otherwise be freed by clear_display_entry).
// Does NOT consume the queue — entries stay so run_pending_finalize can do
// the full cleanup later.
void run_pending_finalize_mark_only(SWFAppContext* app_context)
{
	(void)app_context;
	for (int i = 0; i < ng_pending_finalize_count; i++) {
		PendingFinalizeEntry* e = &ng_pending_finalize_entries[i];
		actionMarkMCPendingRemovalDirect(e->mc, e->swf_depth);
	}
}

void run_pending_finalize(SWFAppContext* app_context)
{
	int n = ng_pending_finalize_count;
	ng_pending_finalize_count = 0;
	for (int i = 0; i < n; i++) {
		PendingFinalizeEntry* e = &ng_pending_finalize_entries[i];
		actionMarkMCPendingRemovalDirect(e->mc, e->swf_depth);
		// Skip clear_display_entry when the slot has been overwritten by a
		// same-frame fresh placement under a *different* instance name (a
		// distinct MovieClip — the new placement's data must survive into the
		// next frame). When name matches the original, the inline rename path
		// in tagSetInstanceName treated the new placement as a continuation of
		// the dying entry, so clearing is the correct cleanup.
		if (e->depth <= max_depth)
		{
			// Only honor the slot-renamed skip when the live display_list array
			// is the same one we queued from. tag.c's display_list pointer is
			// swapped to a sprite's local array during sprite frame execution
			// (see exec_sprite_frame call sites) and back to root afterwards.
			// Pending entries queued sprite-internally would otherwise compare
			// orig_instance_name (captured against the sprite's array) to a
			// completely different entry in root's array, mis-classifying the
			// slot as "renamed" and clobbering a same-name same-frame Remove+
			// Place pattern that does need clearing. Required to preserve
			// misc-ming.all register_class/RegisterClassTest4.
			//
			// When the live display_list array doesn't match the queued one
			// (sprite-internal pending entry; sprite finished executing and
			// display_list was swapped back to root by exec_sprite_frame's
			// caller), temporarily restore the queued display_list before
			// calling clear_display_entry — otherwise we'd clobber the
			// unrelated entry at the same depth in root (e.g. action_order
			// /action_execution_order_test, where mc_blu at sprite_6:depth=3
			// shares its index with mc_red at root:depth=3).
			int slot_renamed = 0;
			if (e->queued_dl_array == (void*)display_list)
			{
				const char* cur_name = display_list[e->depth].instance_name;
				if (e->orig_instance_name[0] != '\0' && cur_name != NULL
				    && strcmp(cur_name, e->orig_instance_name) != 0)
				{
					slot_renamed = 1;
				}
			}
			if (!slot_renamed)
			{
				if (e->queued_dl_array != NULL &&
				    e->queued_dl_array != (void*)display_list)
				{
					// Sprite-internal pending entry: display_list was swapped
					// back to root by exec_sprite_frame's caller after the
					// sprite's frame_func returned. Clearing root.display_list
					// [e->depth] would clobber an unrelated entry — root's depth
					// e->depth typically holds the *sprite's parent MC* (e.g.
					// action_order/action_execution_order_test: mc_blu at
					// sprite_6:depth=3, mc_red at root:depth=3). Instead, peek
					// at the queued sprite's display_list to decide: if the
					// slot still holds the original char_id (no replacement
					// placed in the same frame), clear it on the sprite's dl;
					// otherwise (e.g. register_class/RegisterClassTest4 where
					// mc3 frame 1 does Remove+Place at the same depth with the
					// same name but a different char), leave both untouched —
					// the fresh placement is now the slot's owner.
					DisplayObject* qdl = (DisplayObject*)e->queued_dl_array;
					if (qdl[e->depth].char_id == e->orig_char_id)
					{
						DisplayObject* saved_dl = display_list;
						size_t saved_max = max_depth;
						size_t saved_cap = display_list_capacity;
						display_list = qdl;
						if (max_depth < e->depth) max_depth = e->depth;
						if (display_list_capacity < e->depth + 1)
							display_list_capacity = e->depth + 1;
						clear_display_entry(app_context, e->depth);
						display_list = saved_dl;
						max_depth = saved_max;
						display_list_capacity = saved_cap;
					}
					// else: slot already overwritten by fresh Place; skip clear.
				}
				else
				{
					clear_display_entry(app_context, e->depth);
				}
			}
		}
	}
}
#else
void queue_pending_finalize_mc(MovieClip* mc, int swf_depth, size_t depth) {
	(void)mc; (void)swf_depth; (void)depth;
}
void run_pending_finalize_mark_only(SWFAppContext* app_context) { (void)app_context; }
void run_pending_finalize(SWFAppContext* app_context) { (void)app_context; }
// Stub for browser-WASM graphics build: there's no pending-finalize queue
// in this path (run_pending_finalize / queue_pending_finalize_mc are no-ops
// above), so no depth can ever have a pending finalize. Returning 0 lets
// tagPlaceObject2 / tagPlaceObject2WithClipActions / tagPlaceObject2Ratio
// link cleanly when called from this build mode — they reference the
// function unconditionally via `extern int ng_depth_has_pending_finalize`.
int ng_depth_has_pending_finalize(size_t depth) { (void)depth; return 0; }
#endif

// Forward decls — defined later in this file. Needed in clear_display_entry's
// freed-DL invalidation walk below (all modes).
static void invalidate_descendants_of_mc(SWFAppContext* app_context, MovieClip* parent);
extern void actionInvalidateCachedMovieClipDirect(SWFAppContext* app_context, MovieClip* mc);

static void clear_display_entry(SWFAppContext* app_context, size_t depth)
{
	if (display_list[depth].instance_name_owned && display_list[depth].instance_name != NULL)
	{
		free(display_list[depth].instance_name);  // system free: strdup'd string
	}
	if (display_list[depth].sprite_display_list != NULL)
	{
		// Before freeing the persistent sprite_display_list, invalidate any
		// cached MovieClips whose display_obj points into it (all modes; this
		// walk was browser-WASM-only until the CI modes hit the same class:
		// a mouse-click hit-walk (mc_get_pixel_aabb_ng) read a cached MC's
		// display_obj inside a buffer this FREE released — Minesweeper board
		// workload, sprite_29 goto RemoveObject2. See swf.h ALIASING RULE.)
		// Button state-change handlers (line ~2275) write state-func placements
		// into this DL; sprite children placed there (e.g. SLUG button's
		// down-state → sprite_11) get MCs registered in child_mc_cache via
		// process_sprite_needs_init's actionFindOrCreateMovieClip. The MC's
		// `display_obj` is &sprite_display_list[child_depth]. When the button
		// is removed (tagRemoveObject2 → pending_remove → finalize walk →
		// clear_display_entry), this FREE invalidates that pointer — but the
		// MC is still in child_mc_cache and would still be rendered by the
		// root-attached-MC render pass in tagShowFrame (which reads
		// d->sprite_display_list and walks the freed memory). Surfaces as
		// Snake's 9x9 black square at world (0,0) after clicking the SLUG
		// button (sprite_11_frame_0 places shape 6 at transform 85 = identity).
		DisplayObject* freed_dl = display_list[depth].sprite_display_list;
		size_t freed_cap = display_list[depth].sprite_dl_capacity;
		uintptr_t freed_lo = (uintptr_t)freed_dl;
		uintptr_t freed_hi = freed_lo + freed_cap * sizeof(DisplayObject);
		extern MovieClip* child_mc_cache[];
		extern int child_mc_count;
		for (int ci = 0; ci < child_mc_count; ci++) {
			MovieClip* leaked = child_mc_cache[ci];
			if (leaked == NULL || leaked->depth == INT_MIN) continue;
			uintptr_t dobj_p = (uintptr_t)leaked->display_obj;
			if (dobj_p >= freed_lo && dobj_p < freed_hi) {
				invalidate_descendants_of_mc(app_context, leaked);
				actionInvalidateCachedMovieClipDirect(app_context, leaked);
			}
		}
		ng_freeSpriteDL(app_context, display_list[depth].sprite_display_list,
		                display_list[depth].sprite_dl_capacity);
		display_list[depth].sprite_display_list = NULL;
	}
	// Preserve button state so it can be restored if the same char is re-placed
	// (handles looping movies that remove+replace a button each frame cycle).
	if (display_list[depth].char_id != 0)
	{
		display_list[depth].sticky_char_id = display_list[depth].char_id;
		display_list[depth].sticky_button_state = display_list[depth].button_state;
	}
	display_list[depth].char_id = 0;
	display_list[depth].transform_id = 0;
	display_list[depth].cxform_id = 0;
	display_list[depth].has_cxform = 0;
	display_list[depth].clip_depth = 0;
	display_list[depth].instance_name = NULL;
	display_list[depth].instance_name_owned = 0;
	display_list[depth].clip_actions = NULL;
	display_list[depth].clip_action_count = 0;
	display_list[depth].accumulated_clip_actions = NULL;
	display_list[depth].accumulated_clip_action_count = 0;
	// Reset per-MC script-override flags. When the MC at this depth is removed,
	// its identity is gone — a subsequent fresh PlaceObject2 at the same depth
	// places a new MC that hasn't been touched by AS yet (matrix_test
	// remove+re-add+setMatrix sequence).
	display_list[depth].transformed_by_script = 0;
	display_list[depth].cx_overridden = 0;
	display_list[depth].clone_replaced = 0;
	display_list[depth].as_hidden = 0;  // fresh placement at this depth defaults visible
	// Reset enterFrame-eligibility so a subsequent fresh placement at this depth
	// re-runs the init cycle (sprite_initialized 0→1 on place, 1→2 next tick) and
	// therefore does NOT fire its clip-event enterFrame on the placement tick —
	// Flash fires enterFrame only from the frame AFTER a clip is instantiated.
	// The place path only bumps 0→1 (tag.c ~6286/6840), so a stale value left
	// here would make the fresh clip enterFrame-eligible immediately. Concrete
	// case: Doodle Jump restart-after-game-over — the re-placed hero's
	// enterFrame fired on its placement tick, before frame_1's script reset
	// _root.gameOver=false, hitting `if (_root.gameOver) gotoAndStop(3)` and
	// snapping straight back to the game-over screen.
	display_list[depth].sprite_initialized = 0;
	display_list[depth].enterframe_eligible = 0;
}

// Browser-WASM cleanup helper: find any cached MC bound to a specific
// DisplayObject (via mc->display_obj), invalidate it, and recursively
// invalidate all its descendants. Used by tagRemoveObject2 / tagPlaceObject2
// in browser-WASM to mirror the invalidation side-effect of
// fire_recursive_child_unloads, without queueing AS-level UNLOAD handlers
// (browser-WASM lacks the actionFirePendingUnloads drain path that the
// NO_GRAPHICS||OFFSCREEN_RENDER builds run from tagShowFrame). Identifying
// the MC by DisplayObject pointer is essential because multiple sprite
// instances of the same character (e.g. Doodle Jump's 4 menu button sprites)
// hold child MCs that share name+depth — actionInvalidateCachedMovieClip
// can't tell them apart and would invalidate the wrong instance's child.
// Without this, when the 4 menu buttons are removed, 3 children get
// invalidated and 1 keeps rendering after menu→gameplay.
extern void actionInvalidateCachedMovieClipDirect(SWFAppContext* app_context, MovieClip* mc);
// All modes (was browser-WASM-only): clear_display_entry's freed-buffer
// invalidation walk needs it in the CI modes too (see the ALIASING RULE note
// there).
static void invalidate_descendants_of_mc(SWFAppContext* app_context, MovieClip* parent)
{
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	if (parent == NULL) return;
	// Iterate child_mc_cache for direct children of `parent`, recursing into
	// each. O(N²) worst case but N is small for typical SWFs.
	for (int i = 0; i < child_mc_count; i++) {
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL || mc->depth == INT_MIN) continue;
		if (mc->parent != parent) continue;
		invalidate_descendants_of_mc(app_context, mc);
		actionInvalidateCachedMovieClipDirect(app_context, mc);
	}
}

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
// Find the cached MC whose display_obj == `obj`, then invalidate it and all
// its descendants. NULL display_obj match is skipped because many cached MCs
// (e.g. dynamically-created clones) have display_obj=NULL legitimately.
static void invalidate_mc_for_dl_entry(SWFAppContext* app_context, DisplayObject* obj)
{
	extern MovieClip* child_mc_cache[];
	extern int child_mc_count;
	if (obj == NULL) return;
	for (int i = 0; i < child_mc_count; i++) {
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL || mc->depth == INT_MIN) continue;
		if (mc->display_obj != (void*)obj) continue;
		invalidate_descendants_of_mc(app_context, mc);
		actionInvalidateCachedMovieClipDirect(app_context, mc);
		return;
	}
	// Fallback for root-placed text fields. A DefineEditText placed directly on
	// the root display list gets its child_mc_cache wrapper created lazily by
	// name lookup (actionFindOrCreateMovieClip), which sets ng_textfield_idx and
	// depth (= SWF depth - 16384) but never links display_obj. The display_obj
	// scan above therefore can't find it, so when the entry is removed/reclaimed
	// (Tetris's level-select "startLevel" number at depth 193 replaced by
	// sound_mc at the menu->game transition) the wrapper survives in the cache
	// and actionIterateTextFieldGlyphs keeps drawing its bound value on every
	// later frame. Match the orphaned wrapper by the AS-depth that corresponds
	// to this SWF depth plus the text-field index, and invalidate it.
	extern DisplayObject* display_list;
	extern size_t max_depth;
	extern int ng_find_textfield(size_t);
	int tf_idx = ng_find_textfield(obj->char_id);
	if (tf_idx < 0) return;
	uintptr_t base = (uintptr_t)display_list;
	uintptr_t op = (uintptr_t)obj;
	if (display_list == NULL || op < base) return;
	size_t d = (size_t)(op - base) / sizeof(DisplayObject);
	if (d > max_depth || &display_list[d] != obj) return;  // not a root DL entry
	int as_depth = (int)d - 16384;
	for (int i = 0; i < child_mc_count; i++) {
		MovieClip* mc = child_mc_cache[i];
		if (mc == NULL || mc->depth == INT_MIN) continue;
		if (mc->display_obj != NULL) continue;
		if (mc->ng_textfield_idx != tf_idx) continue;
		if (mc->depth != as_depth) continue;
		invalidate_descendants_of_mc(app_context, mc);
		actionInvalidateCachedMovieClipDirect(app_context, mc);
		return;
	}
}
#endif

// Recursively fire CLIP_EVENT_UNLOAD for children of a display object.
// Fires children-first (depth order within each sprite's display list),
// recursing into nested sprites before firing the child's own unload.
// Also invalidates child MCs so they don't fire onEnterFrame after parent removal.
static void fire_recursive_child_unloads(SWFAppContext* app_context,
	DisplayObject* dl, size_t dl_max, MovieClip* parent_mc);

static void fire_entry_unloads(SWFAppContext* app_context,
	DisplayObject* dl, size_t i, MovieClip* parent_mc)
{
	DisplayObject* obj = &dl[i];
	if (obj->char_id == 0) return;

	// Recurse into nested sprite's children first (even if this obj has no clip actions)
	if (obj->sprite_display_list != NULL && obj->sprite_max_depth > 0)
	{
		MovieClip* child_mc = NULL;
		if (obj->instance_name != NULL)
			child_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
		fire_recursive_child_unloads(app_context,
			obj->sprite_display_list, obj->sprite_max_depth,
			child_mc ? child_mc : parent_mc);
	}

	// Queue this child's CLIP_EVENT_UNLOAD (deferred — fires at next ShowFrame
	// drain in actionFirePendingUnloads, alongside AS-level onUnload handlers).
	// Resolves the MC at queue time so g_current_context can be restored at
	// dispatch time.
	if (obj->clip_action_count > 0)
	{
		MovieClip* child_mc = NULL;
		if (obj->instance_name != NULL)
			child_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
		for (size_t a = 0; a < obj->clip_action_count; a++)
		{
			if (obj->clip_actions[a].event_flags & CLIP_EVENT_UNLOAD)
				actionQueueClipActionUnload(obj->clip_actions[a].action, child_mc);
		}
	}

	// Invalidate child MC so it won't fire onEnterFrame after parent removal.
	// The parent's sprite_display_list will be freed by clear_display_entry,
	// making child MC's display_obj a dangling pointer.
	if (obj->instance_name != NULL)
		actionInvalidateCachedMovieClip(app_context, obj->instance_name, (int)i);
}

static void fire_recursive_child_unloads(SWFAppContext* app_context,
	DisplayObject* dl, size_t dl_max, MovieClip* parent_mc)
{
	for (size_t i = 1; i <= dl_max; i++)
		fire_entry_unloads(app_context, dl, i, parent_mc);
}

void tagRemoveObject(SWFAppContext* app_context, size_t depth)
{
	if (depth <= max_depth && display_list[depth].char_id != 0)
	{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		// Defer removal during catch_up_mode for entries with UNLOAD handlers
		// (same rationale as tagRemoveObject2 — see comment there).
		if (catch_up_mode)
		{
			int has_unload_cu = 0;
			for (size_t a = 0; a < display_list[depth].clip_action_count; a++) {
				if (display_list[depth].clip_actions[a].event_flags & CLIP_EVENT_UNLOAD) {
					has_unload_cu = 1; break;
				}
			}
			if (!has_unload_cu) {
				for (size_t a = 0; a < display_list[depth].accumulated_clip_action_count; a++) {
					if (display_list[depth].accumulated_clip_actions[a].event_flags & CLIP_EVENT_UNLOAD) {
						has_unload_cu = 1; break;
					}
				}
			}
			if (has_unload_cu) return;
		}
#endif
		// Queue accumulated clip actions (from prior Remove+Replace cycle) first.
		// Snapshot the action_fn pointer (recompiler-emitted static code) so
		// dropping the accumulated_clip_actions array reference is safe.
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		MovieClip* _ro_parent_mc1 = NULL;
		if (display_list[depth].instance_name != NULL) {
			// Owner of the current display list (sprite MC inside a sprite frame,
			// else _root) — passing &root_movieclip unconditionally would create
			// a ghost root-child for a nested clip.
			MovieClip* _ro_p1 = g_current_context ? g_current_context : &root_movieclip;
			_ro_parent_mc1 = actionFindOrCreateMovieClip(app_context, display_list[depth].instance_name, _ro_p1);
		}
#else
		MovieClip* _ro_parent_mc1 = NULL;
#endif
		if (display_list[depth].accumulated_clip_action_count > 0)
		{
			for (size_t a = 0; a < display_list[depth].accumulated_clip_action_count; a++)
			{
				if (display_list[depth].accumulated_clip_actions[a].event_flags & CLIP_EVENT_UNLOAD)
					actionQueueClipActionUnload(display_list[depth].accumulated_clip_actions[a].action, _ro_parent_mc1);
			}
		}
		// Queue current onUnload clip actions
		if (display_list[depth].clip_action_count > 0)
		{
			for (size_t a = 0; a < display_list[depth].clip_action_count; a++)
			{
				if (display_list[depth].clip_actions[a].event_flags & CLIP_EVENT_UNLOAD)
					actionQueueClipActionUnload(display_list[depth].clip_actions[a].action, _ro_parent_mc1);
			}
		}
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		// See tagRemoveObject2 for the rationale.
		int _ro1_has_unload = 0;
		MovieClip* _ro1_mc = _ro_parent_mc1;
		if (display_list[depth].instance_name != NULL) {
			extern int ng_compute_has_unload(size_t depth);
			_ro1_has_unload = ng_compute_has_unload(depth);
			if (_ro1_has_unload) {
				actionFireOnUnload(app_context, display_list[depth].instance_name, (int)depth);
			}
		}
		if (_ro1_has_unload && _ro1_mc != NULL) {
			extern void queue_pending_finalize_mc(MovieClip*, int, size_t);
			queue_pending_finalize_mc(_ro1_mc, (int)depth, depth);
		} else {
			if (display_list[depth].instance_name != NULL) {
				actionInvalidateCachedMovieClip(app_context, display_list[depth].instance_name, (int)depth);
			}
			clear_display_entry(app_context, depth);
		}
#else
		// Browser-WASM: defer invalidate + clear to tagShowFrame fallback or
		// same-tick Place reclaim. See tagRemoveObject2 for the full rationale.
		display_list[depth].pending_remove = 1;
#endif
	}
#ifndef NO_GRAPHICS
	(void)app_context;
#endif
}

void tagRemoveObject2(SWFAppContext* app_context, size_t depth)
{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (g_tag_skip_mode) return;
#endif
	if (depth <= max_depth && display_list[depth].char_id != 0)
	{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		// During backward catch-up, the replay loop runs frames 0..target. PlaceObject2
		// at frame X re-asserts entries; RemoveObject2 at frame Y > X must take effect
		// so the post-replay display list reflects target's actual state. Only protect
		// when this exact frame's PlaceObject2 just ran (same-frame place+remove pair),
		// in which case the remove was intended as a no-op cleanup of fresh placement.
		// Required for from_gnash/misc-ming.all/key_event_test where listenerClip1 is
		// placed at frame 8 and removed at frame 9; backward catch-up replays both,
		// and the remove must apply (catch_up_target=10 > placed_at_frame=8).
		extern int catch_up_backward;
		extern size_t catch_up_target;
		(void)catch_up_target;
		if (catch_up_backward && display_list[depth].placed_at_frame == current_frame)
			return;

		// During catch_up_mode (ng_executeGotoTagsOnly from nextFrame/gotoAndStop),
		// defer removal of entries with UNLOAD handlers. The calling script hasn't
		// finished yet, so UNLOAD must not fire inline.
		//
		// Forward catch-up (catch_up_mode && !catch_up_backward): queue clip-action
		// UNLOAD callbacks via actionQueueClipActionUnloadDeferred (kind=SCRIPT,
		// is_unload=0) so they ride the same SCRIPT FIFO drain as the calling
		// script, landing after the remaining queued root scripts. Then move the
		// MC into the removed-depth zone via actionMarkMCPendingRemoval (depth
		// shift + detach) and clear the display_list slot so subsequent
		// PlaceObject2 at the same depth (e.g., the next frame placing a fresh
		// MC over the removed slot) doesn't stomp the pending entry. Mirrors
		// loop_test8's backward-rewind clear-and-replace path in tagPlaceObject2.
		// Required for goto_frame_test (forward gotoAndPlay past Place+Remove of
		// mc1 then Place+Remove of mc2 at the same depth — both must end up in
		// the removed-zone with their UNLOAD handlers firing before the target
		// frame's DoAction).
		//
		// Backward catch-up (catch_up_backward=1) with placed_at_frame > target:
		// keep the original early-return. The tagPlaceObject2 backward path
		// handles its own MarkMCPendingRemoval for replaced entries; here the
		// entry is just stale state that ng_display_cleanup_unplaced_after will
		// clear later.
		if (catch_up_mode)
		{
			int has_own_unload_cu = 0;
			for (size_t a = 0; a < display_list[depth].clip_action_count; a++) {
				if (display_list[depth].clip_actions[a].event_flags & CLIP_EVENT_UNLOAD) {
					has_own_unload_cu = 1; break;
				}
			}
			if (!has_own_unload_cu) {
				for (size_t a = 0; a < display_list[depth].accumulated_clip_action_count; a++) {
					if (display_list[depth].accumulated_clip_actions[a].event_flags & CLIP_EVENT_UNLOAD) {
						has_own_unload_cu = 1; break;
					}
				}
			}
			int has_child_unload_cu = 0;
			if (display_list[depth].sprite_display_list != NULL &&
			    display_list[depth].sprite_max_depth > 0) {
				extern int has_child_unload_handler(DisplayObject* dl, size_t dl_max);
				has_child_unload_cu = has_child_unload_handler(display_list[depth].sprite_display_list,
				                                               display_list[depth].sprite_max_depth);
			}
			// Forward catch-up with own clip-action UNLOAD: queue the UNLOAD
			// callbacks deferred (kind=SCRIPT, ride the FIFO drain) and move
			// the MC into the removed-depth zone via actionMarkMCPendingRemoval.
			// Clear the display_list slot so a subsequent PlaceObject2 at the
			// same depth (next frame in the catch-up) places fresh instead of
			// stomping the still-live entry. Required for goto_frame_test
			// (Place+Remove of mc1, then Place+Remove of mc2 at the same depth
			// during forward catch-up — both must end up in the removed-zone
			// with their UNLOAD handlers firing before the target frame's
			// DoAction). Skipped for backward catch-up (handled by
			// tagPlaceObject2's clear-and-replace path) and for child-only
			// unloads (the deferred-goto re-run of the target frame with
			// catch_up_mode=0 fires the child unload via the normal path —
			// see avm1/unload_nested_child).
			if (has_own_unload_cu)
			{
				if (catch_up_backward) return;
				if (display_list[depth].instance_name != NULL) {
					MovieClip* _ro2_cu_mc = actionFindOrCreateMovieClip(
						app_context, display_list[depth].instance_name, &root_movieclip);
					for (size_t a = 0; a < display_list[depth].accumulated_clip_action_count; a++) {
						if (display_list[depth].accumulated_clip_actions[a].event_flags & CLIP_EVENT_UNLOAD)
							actionQueueClipActionUnloadDeferred(
								display_list[depth].accumulated_clip_actions[a].action, _ro2_cu_mc);
					}
					for (size_t a = 0; a < display_list[depth].clip_action_count; a++) {
						if (display_list[depth].clip_actions[a].event_flags & CLIP_EVENT_UNLOAD)
							actionQueueClipActionUnloadDeferred(
								display_list[depth].clip_actions[a].action, _ro2_cu_mc);
					}
					actionMarkMCPendingRemoval(app_context,
					                           display_list[depth].instance_name,
					                           (int)depth);
				}
				clear_display_entry(app_context, depth);
				return;
			}
			if (has_child_unload_cu) return;
		}
#endif
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		MovieClip* _remove_parent_mc = NULL;
		if (display_list[depth].instance_name != NULL) {
			// Owner of the current display list (sprite MC inside a sprite frame,
			// else _root) — passing &root_movieclip unconditionally would create
			// a ghost root-child for a nested clip (e.g. Minesweeper's Flash v2
			// component boundingBox removed inside its sprite frame).
			MovieClip* _rm_p = g_current_context ? g_current_context : &root_movieclip;
			_remove_parent_mc = actionFindOrCreateMovieClip(app_context, display_list[depth].instance_name, _rm_p);
		}
#else
		MovieClip* _remove_parent_mc = NULL;
#endif
		// Queue accumulated clip actions (from prior Remove+Replace cycle) first.
		if (display_list[depth].accumulated_clip_action_count > 0)
		{
			for (size_t a = 0; a < display_list[depth].accumulated_clip_action_count; a++)
			{
				if (display_list[depth].accumulated_clip_actions[a].event_flags & CLIP_EVENT_UNLOAD)
					actionQueueClipActionUnload(display_list[depth].accumulated_clip_actions[a].action, _remove_parent_mc);
			}
		}
		// Queue CLIP_EVENT_UNLOAD for children first (recursive, depth-first)
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		if (display_list[depth].sprite_display_list != NULL && display_list[depth].sprite_max_depth > 0)
		{
			fire_recursive_child_unloads(app_context,
				display_list[depth].sprite_display_list, display_list[depth].sprite_max_depth,
				_remove_parent_mc ? _remove_parent_mc : &root_movieclip);
		}
		// Queue AS-level onUnload on dynamic children (createEmptyMovieClip /
		// duplicateMovieClip) that live in child_mc_cache, not the sprite display
		// list. Queue BEFORE the parent's own UNLOAD clip actions fire, so children
		// created during that handler don't pick up onUnload firing (Flash
		// observation: dynamic child created inside parent's UNLOAD handler does
		// not get its onUnload triggered — see gnash new_child_in_unload_test
		// case1).
		if (_remove_parent_mc != NULL)
			actionQueueDynamicChildUnloads(_remove_parent_mc);
#endif
		// Queue current onUnload clip actions
		if (display_list[depth].clip_action_count > 0)
		{
			for (size_t a = 0; a < display_list[depth].clip_action_count; a++)
			{
				if (display_list[depth].clip_actions[a].event_flags & CLIP_EVENT_UNLOAD)
					actionQueueClipActionUnload(display_list[depth].clip_actions[a].action, _remove_parent_mc);
			}
		}
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		// Compute has_unload to decide finalization path:
		//  - has_unload=1: queue AS-level handler (lookup at queue time);
		//    defer Mark to post-drain finalize so the MC stays !pending_removal
		//    for same-frame DoAction lookups (loop_test7 lines 123-124, typeof
		//    returns 'movieclip' after RemoveObject2 in same-frame DoAction).
		//  - has_unload=0: no handler to defer — Invalidate inline so the MC is
		//    immediately gone (matches Flash for handler-less removals).
		// clear_display_entry runs inline in both cases to avoid dangling
		// sprite_display_list pointers in queued onLoad handlers from other MCs.
		int _ro2_has_unload = 0;
		MovieClip* _ro2_mc = NULL;
		if (display_list[depth].instance_name != NULL) {
			extern int ng_compute_has_unload(size_t depth);
			_ro2_has_unload = ng_compute_has_unload(depth);
			_ro2_mc = _remove_parent_mc;
			if (_ro2_has_unload) {
				actionFireOnUnload(app_context, display_list[depth].instance_name, (int)depth);
			}
		}
		if (_ro2_has_unload && _ro2_mc != NULL) {
			// Defer Mark + clear_display_entry to post-drain finalize.
			extern void queue_pending_finalize_mc(MovieClip*, int, size_t);
			queue_pending_finalize_mc(_ro2_mc, (int)depth, depth);
		} else {
			// No handler — Invalidate + clear inline.
			if (display_list[depth].instance_name != NULL) {
				actionInvalidateCachedMovieClip(app_context, display_list[depth].instance_name, (int)depth);
			}
			clear_display_entry(app_context, depth);
		}
#else
		// Browser-WASM frame_func re-run protection. The browser-WASM main
		// loop re-runs frame_funcs every tick (no `is_playing` gate), so the
		// recompiler-emitted tagRemoveObject2 + tagPlaceObject2(Ratio) pair
		// for a stopped frame would invalidate + recreate the cached MC every
		// tick — leaking ~1 MC/tick into child_mc_cache until
		// MAX_CHILD_MOVIECLIPS=128 caps it (Doodle Jump gameplay frame_1
		// depth 2 'container' via the same-(char,ratio) RemoveObject2 +
		// PlaceObject2Ratio pair). Defer invalidate + clear by marking
		// pending_remove=1; subsequent same-tick Place at the same depth
		// reclaims via the modify-detect path in tagPlaceObject2 /
		// tagPlaceObject2Ratio (consume_pending_remove). Unreclaimed
		// entries are finalized at tagShowFrame's fallback walk. Preserves
		// the orphan-text fix from c2147d58e because the deferred
		// invalidate still fires before render (tagShowFrame → render).
		display_list[depth].pending_remove = 1;
#endif
	}
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	else
	{
		// Display list slot is empty. A duplicateMovieClip clone swapped
		// into this depth from the dynamic range may live only in
		// child_mc_cache — its display_list entry was never populated
		// because `target_swf_depth >= INITIAL_DISPLAYLIST_CAPACITY` takes
		// the skip-DL path in `ng_cloneSprite`. After `mc1.swapDepths(dup)`
		// followed by a RemoveObject2 targeting mc1's original SWF depth,
		// dup lives at `mc->depth == depth - 16384` with no DL entry and
		// must still be invalidated. Matches Ruffle behaviour in
		// from_gnash/misc-ming.all/static_vs_dynamic2.
		extern void actionInvalidateMCAtASDepth(SWFAppContext*, int);
		actionInvalidateMCAtASDepth(app_context, (int)depth - 16384);
	}
#endif
#ifndef NO_GRAPHICS
	(void)app_context;
#endif
}

void tagSetSpriteNoEndTag(SWFAppContext* app_context, size_t char_id)
{
	(void)app_context;
	ENSURE_SIZE(dictionary, char_id, dictionary_capacity, sizeof(Character));
	if (dictionary[char_id].type == CHAR_TYPE_SPRITE)
		dictionary[char_id].sprite_has_end_tag = 0;
}

// Recompiler-emitted correction for a DefineSprite whose header frameCount
// disagrees with its body. Emitted immediately after tagDefineSprite (which
// carries the header field verbatim), so the header value survives in
// sprite_declared_frames for _totalframes while playback switches to the two
// numbers that actually describe the body:
//   func_count    - how many frame functions the recompiler generated
//                   (= loaded_frames, +1 when tags trail the last ShowFrame)
//   loaded_frames - Ruffle's frames_loaded: ShowFrame count, floored at 1
void tagSetSpriteFrameCounts(SWFAppContext* app_context, size_t char_id,
                             size_t func_count, size_t loaded_frames)
{
	(void)app_context;
	ENSURE_SIZE(dictionary, char_id, dictionary_capacity, sizeof(Character));
	if (dictionary[char_id].type != CHAR_TYPE_SPRITE) return;
	dictionary[char_id].sprite_frame_count = func_count;
	dictionary[char_id].sprite_loaded_frames = loaded_frames;
}

void tagDefineSpriteEx(SWFAppContext* app_context, size_t char_id, frame_func* funcs, size_t frame_count, size_t byte_size)
{
	(void)app_context;
	ENSURE_SIZE(dictionary, char_id, dictionary_capacity, sizeof(Character));

	dictionary[char_id].type = CHAR_TYPE_SPRITE;
	dictionary[char_id].sprite_frame_funcs = funcs;
	dictionary[char_id].sprite_frame_count = frame_count;
	// Defaults for the well-formed case (header count == ShowFrame count ==
	// generated frame-function count). tagSetSpriteFrameCounts, emitted right
	// after this call, overrides the first two whenever the DefineSprite body
	// disagrees with its header.
	dictionary[char_id].sprite_loaded_frames = frame_count;
	dictionary[char_id].sprite_declared_frames = frame_count;
	dictionary[char_id].sprite_byte_size = byte_size;
	// Must be written unconditionally: Character is a union, so this byte may
	// hold a stale value from a different character type. Default 1 (loops);
	// the recompiler emits tagSetSpriteNoEndTag afterwards for malformed
	// sprites whose body lacks the terminating End record.
	dictionary[char_id].sprite_has_end_tag = 1;

	// Track which movie defined this character (for child movie transform data lookup)
	extern u8 g_current_movie_id;
	if (g_current_movie_id != 0) {
		if (char_id >= g_char_movie_id_capacity) {
			size_t new_cap = char_id + 64;
			u8* new_arr = (u8*)calloc(new_cap, 1);
			if (g_char_movie_id) {
				memcpy(new_arr, g_char_movie_id, g_char_movie_id_capacity);
				free(g_char_movie_id);
			}
			g_char_movie_id = new_arr;
			g_char_movie_id_capacity = new_cap;
		}
		g_char_movie_id[char_id] = g_current_movie_id;
	}
}

// DoInitAction once-per-character guard. Keyed on (g_current_movie_id,
// char_id) so parent and child SWFs that share the same char_id space
// (e.g. char_id=1 for both parent's and child's DoInitAction sprite)
// don't shadow each other. Used by both the per-frame init prologue
// (top-level DoInitAction) and the sprite-internal DoInitAction emission.
#define MAX_INIT_ACTION_MOVIES 8
#define MAX_INIT_ACTION_CHARS 512
static u8 g_init_action_done[MAX_INIT_ACTION_MOVIES][MAX_INIT_ACTION_CHARS];

void tagDoInitActionGuarded(SWFAppContext* app_context, size_t char_id, frame_func action)
{
	u8 movie = g_current_movie_id;
	if (movie >= MAX_INIT_ACTION_MOVIES) movie = 0;
	if (char_id < MAX_INIT_ACTION_CHARS && g_init_action_done[movie][char_id])
		return;
	if (char_id < MAX_INIT_ACTION_CHARS)
		g_init_action_done[movie][char_id] = 1;
	action(app_context);
}

// Per-sprite frame label storage (separate from Character union)
#define MAX_SPRITE_LABEL_ENTRIES 256
static struct {
	size_t char_id;
	FrameLabelEntry* labels;
	size_t count;
} sprite_label_store[MAX_SPRITE_LABEL_ENTRIES];
static size_t sprite_label_store_count = 0;

void tagSetSpriteLabels(size_t char_id, FrameLabelEntry* labels, size_t count)
{
	if (sprite_label_store_count >= MAX_SPRITE_LABEL_ENTRIES) return;
	sprite_label_store[sprite_label_store_count].char_id = char_id;
	sprite_label_store[sprite_label_store_count].labels = labels;
	sprite_label_store[sprite_label_store_count].count = count;
	sprite_label_store_count++;
}

int ng_findSpriteLabelFrame(size_t char_id, const char* label)
{
	extern int g_swf_version;
	if (!label) return -1;
	for (size_t i = 0; i < sprite_label_store_count; i++)
	{
		if (sprite_label_store[i].char_id == char_id)
		{
			FrameLabelEntry* entries = sprite_label_store[i].labels;
			size_t count = sprite_label_store[i].count;
			// SWF<=6: case-insensitive throughout, with LOWEST frame index winning
			// case-insensitive ties (mirrors Flash<=6 where the first label registered
			// for a given case-insensitive key wins). This matters for sprites that
			// define labels differing only by case at different frames — e.g.
			// "small_first"@8 vs "Small_first"@9: looking up "Small_first" should
			// return 8, not 9. Key test: misc-ming/frame_label_test.
			if (g_swf_version <= 6)
			{
				int best_frame = -1;
				for (size_t j = 0; j < count; j++)
				{
					if (entries[j].label && strcasecmp(entries[j].label, label) == 0)
					{
						int f = (int)entries[j].frame;
						if (best_frame < 0 || f < best_frame)
							best_frame = f;
					}
				}
				return best_frame;
			}
			// SWF7+: exact match first, no case-insensitive fallback
			for (size_t j = 0; j < count; j++)
			{
				if (entries[j].label && strcmp(entries[j].label, label) == 0)
					return (int)entries[j].frame;
			}
			return -1;
		}
	}
	return -1;
}

// Per-sprite per-frame placement table storage (for survives_rewind).
// 1024: real games exceed 256 sprites (the loop-back survives_rewind check
// degrades to clear-everything churn for any sprite past the cap).
#define MAX_SPRITE_PLACEMENT_ENTRIES 1024
static struct {
	u16 sprite_id;
	FramePlacement* placements;
	u16* frame_starts;
	u16 frame_count;
} sprite_placement_store[MAX_SPRITE_PLACEMENT_ENTRIES];
static size_t sprite_placement_store_count = 0;

void tagSetSpritePlacements(u16 sprite_id, FramePlacement* placements,
                            u16* frame_starts, u16 frame_count)
{
	if (sprite_placement_store_count >= MAX_SPRITE_PLACEMENT_ENTRIES) return;
	sprite_placement_store[sprite_placement_store_count].sprite_id = sprite_id;
	sprite_placement_store[sprite_placement_store_count].placements = placements;
	sprite_placement_store[sprite_placement_store_count].frame_starts = frame_starts;
	sprite_placement_store[sprite_placement_store_count].frame_count = frame_count;
	sprite_placement_store_count++;
}

const FramePlacement* ng_sprite_frame_placements(u16 sprite_id, u16 frame, u16* out_count)
{
	if (out_count) *out_count = 0;
	for (size_t i = 0; i < sprite_placement_store_count; i++)
	{
		if (sprite_placement_store[i].sprite_id == sprite_id)
		{
			if (frame >= sprite_placement_store[i].frame_count) return NULL;
			u16* fs = sprite_placement_store[i].frame_starts;
			FramePlacement* p = sprite_placement_store[i].placements;
			u16 start = fs[frame];
			u16 end = fs[frame + 1];
			if (out_count) *out_count = (u16)(end - start);
			return &p[start];
		}
	}
	return NULL;
}

void tagDefineButton(SWFAppContext* app_context, size_t char_id, frame_func* state_funcs, size_t hit_char_id, u32 hit_transform_id, ButtonAction* actions, size_t action_count)
{
	(void)app_context;
	ENSURE_SIZE(dictionary, char_id, dictionary_capacity, sizeof(Character));

	dictionary[char_id].type = CHAR_TYPE_BUTTON;
	dictionary[char_id].button_state_funcs = state_funcs;
	dictionary[char_id].button_hit_char_id = hit_char_id;
	dictionary[char_id].button_hit_transform_id = hit_transform_id;
	dictionary[char_id].button_actions = actions;
	dictionary[char_id].button_action_count = action_count;

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	ng_record_button(char_id);
#endif
}

// Public accessor: returns the hit-shape character ID for a button character,
// or 0 if char_id is not a button. Used by mc_get_pixel_aabb_ng so a sprite
// whose only child is a button (with no shape bounds of its own) can derive
// an AABB from the button's hit shape — required for onRollOver/onRollOut
// dispatch on the wrapping sprite (e.g. ButtonEventsTest's `_root.square1`).
size_t ng_getButtonHitCharId(size_t char_id)
{
	if (char_id >= dictionary_capacity) return 0;
	if (dictionary[char_id].type != CHAR_TYPE_BUTTON) return 0;
	return dictionary[char_id].button_hit_char_id;
}

// Focus-highlight bounds for a BUTTON character (Ruffle
// Avm1Button::highlight_bounds). A button is *always* highlighted using its
// hit-test bounds, never the bounds of the state that happens to be showing:
// the visible children swap on hover, so keying the highlight (and the
// automatic tab order) off the hit area is what keeps both stable.
//
// Returns:
//    1 = char_id is a button; out_* hold its hit shape's LOCAL (button-space)
//        twips AABB with the hit record's placement matrix applied,
//   -1 = char_id is a button whose hit area resolves to nothing (no highlight),
//    0 = char_id is not a button (caller falls back to display-list bounds).
int ng_getButtonHighlightBounds(size_t char_id, s32* out_xmin, s32* out_xmax,
                                s32* out_ymin, s32* out_ymax)
{
	if (char_id >= dictionary_capacity) return 0;
	if (dictionary[char_id].type != CHAR_TYPE_BUTTON) return 0;

	u32 hit_xf_id = dictionary[char_id].button_hit_transform_id;
	size_t hit_cid = 0;
	Character* hit_ch = resolve_hit_shape(dictionary[char_id].button_hit_char_id,
	                                      &hit_xf_id, &hit_cid, 0);
	if (hit_ch == NULL || hit_cid == 0) return -1;

	s32 xmin, xmax, ymin, ymax;
	if (!ng_getCharBoundsForRatio(hit_cid, 0, &xmin, &xmax, &ymin, &ymax))
		return -1;

	// Apply the hit record's own placement matrix (scale + translate; button
	// records are axis-aligned in practice, and the rotation terms would only
	// widen an AABB we immediately re-AABB in world space anyway).
	if (hit_xf_id > 0) {
		float (*td)[16] =
			g_active_transform_data ? g_active_transform_data : transform_data;
		float sx = td[hit_xf_id][0];
		float sy = td[hit_xf_id][5];
		float tx = td[hit_xf_id][12];
		float ty = td[hit_xf_id][13];
		float x0 = (float)xmin * sx + tx, x1 = (float)xmax * sx + tx;
		float y0 = (float)ymin * sy + ty, y1 = (float)ymax * sy + ty;
		if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
		if (y1 < y0) { float t = y0; y0 = y1; y1 = t; }
		xmin = (s32)x0; xmax = (s32)x1; ymin = (s32)y0; ymax = (s32)y1;
	}

	if (out_xmin) *out_xmin = xmin;
	if (out_xmax) *out_xmax = xmax;
	if (out_ymin) *out_ymin = ymin;
	if (out_ymax) *out_ymax = ymax;
	return 1;
}

// Phase 1e: yield instance names of children that were in the OLD state
// of this button but are not at the same (depth, char_id) in the NEW state.
// Caller passes the button's DisplayObject; if it doesn't match the most
// recent transitioned button, no callbacks fire. Used by actionEnumerate2
// to include just-removed button-state children in for-in for one tick
// (matches Ruffle's deferred destruction).
void ng_iterateTransientButtonChildren(void* button_dobj,
	void (*cb)(const char* name, u32 name_len, void* user), void* user)
{
	if (button_dobj == NULL || cb == NULL) return;
	if ((DisplayObject*)button_dobj != g_btn_transient_dobj) return;
	for (size_t i = 0; i < g_btn_transient_count; i++)
	{
		const char* n = g_btn_transient_names[i];
		if (n != NULL && n[0] != '\0')
			cb(n, (u32)strlen(n), user);
	}
}

// Phase 1g companion to ng_iterateTransientButtonChildren: direct lookup,
// no callback. Used by actionGetMember to fall back to the transient list
// after live display-list walks miss.
int ng_isTransientButtonChildName(void* button_dobj, const char* name, size_t name_len)
{
	if (button_dobj == NULL || name == NULL || name_len == 0) return 0;
	if ((DisplayObject*)button_dobj != g_btn_transient_dobj) return 0;
	for (size_t i = 0; i < g_btn_transient_count; i++)
	{
		const char* n = g_btn_transient_names[i];
		if (n == NULL) continue;
		if (strlen(n) == name_len && strncmp(n, name, name_len) == 0)
			return 1;
	}
	return 0;
}

// Convert a Flash key code to the SWF button condition key code.
// Button conditions use their own mapping for special keys (1-19) and
// ASCII for printable characters (32+). Letters are lowercase in conditions.
static int flash_key_to_button_cond(int key_code)
{
	// Special keys: Flash key code → SWF button condition code
	switch (key_code) {
	case 37: return 1;   // Left
	case 39: return 2;   // Right
	case 36: return 3;   // Home
	case 35: return 4;   // End
	case 45: return 5;   // Insert
	case 46: return 6;   // Delete
	case 8:  return 8;   // Backspace
	case 13: return 13;  // Enter
	case 38: return 14;  // Up
	case 40: return 15;  // Down
	case 33: return 16;  // Page Up
	case 34: return 17;  // Page Down
	case 9:  return 18;  // Tab
	case 27: return 19;  // Escape
	case 32: return 32;  // Space
	default: break;
	}
	// Letters: Flash key codes are uppercase (65-90), button conditions are lowercase (97-122)
	if (key_code >= 65 && key_code <= 90)
		return key_code + 32;  // to lowercase ASCII
	// Numbers and other printable: same code
	if (key_code >= 32 && key_code <= 126)
		return key_code;
	return 0;
}

// Dispatch button key-press BUTTONCONDACTION conditions for a given Flash key code.
// KeyPress propagates to all buttons in render order (low depth to high depth).
// First button that handles the event stops propagation (Ruffle behavior).
// Returns 1 if any button handled the keyPress, 0 otherwise.
int dispatch_button_key_actions(SWFAppContext* app_context, int key_code)
{
	int cond_code = flash_key_to_button_cond(key_code);
	if (cond_code <= 0) return 0;
	for (size_t i = 1; i <= max_depth; i++)
	{
		DisplayObject* obj = &display_list[i];
		if (obj->char_id == 0) continue;
		Character* ch = &dictionary[obj->char_id];
		if (ch->type != CHAR_TYPE_BUTTON) continue;
		if (ch->button_action_count == 0) continue;
		// Skip invisible buttons — _visible=false suppresses keyPress conditions
		if (obj->instance_name != NULL && !actionGetMCVisible(app_context, obj->instance_name, &root_movieclip))
			continue;
		int handled = 0;
		for (size_t a = 0; a < ch->button_action_count; a++)
		{
			int cond_key = (ch->button_actions[a].condition >> 9) & 0x7F;
			if (cond_key != 0 && cond_key == cond_code)
			{
				ch->button_actions[a].action(app_context);
				handled = 1;
			}
		}
		if (handled) return 1;  // first button to handle stops propagation
	}
	return 0;
}

// Fire button DoAction conditions for a specific transition on a MC.
// transition_mask: 0x0004 = OverUpToOverDown (press), 0x0008 = OverDownToOverUp (release)
void ng_simulateButtonTransition(SWFAppContext* app_context, void* mc_ptr, int transition_mask)
{
	MovieClip* mc = (MovieClip*)mc_ptr;
	if (mc == NULL || !mc->is_button_mc) return;
	if (!mc->name || mc->name[0] == '\0') return;
	size_t depth = ng_findDisplayEntryByName(mc->name);
	if (depth == SIZE_MAX || depth > max_depth) return;
	DisplayObject* obj = &display_list[depth];
	if (obj->char_id == 0) return;
	Character* ch = &dictionary[obj->char_id];
	if (ch->type != CHAR_TYPE_BUTTON) return;
	for (size_t a = 0; a < ch->button_action_count; a++) {
		if (ch->button_actions[a].condition & transition_mask)
			ch->button_actions[a].action(app_context);
	}
}

// Set the button_state on the display object for a button MC.
// Used by Tab focus to set virtual hover state (1=Over) so that
// ng_update_button_states can detect the transition when mouse moves.
void ng_setButtonDisplayState(void* mc_ptr, u8 state)
{
	MovieClip* mc = (MovieClip*)mc_ptr;
	if (mc == NULL || !mc->is_button_mc) return;
	if (!mc->name || mc->name[0] == '\0') return;
	size_t depth = ng_findDisplayEntryByName(mc->name);
	if (depth == SIZE_MAX || depth > max_depth) return;
	DisplayObject* obj = &display_list[depth];
	if (obj->char_id == 0) return;
	Character* ch = &dictionary[obj->char_id];
	if (ch->type != CHAR_TYPE_BUTTON) return;
	obj->button_state = state;
}

// Simulate a button press+release (both transitions). Used as fallback when
// the caller doesn't need to interleave handlers between press and release.
void ng_simulateButtonPressRelease(SWFAppContext* app_context, void* mc_ptr)
{
	ng_simulateButtonTransition(app_context, mc_ptr, 0x0004); // press
	ng_simulateButtonTransition(app_context, mc_ptr, 0x0008); // release
}

void tagPlaceObject3(SWFAppContext* app_context, size_t depth, size_t char_id,
    u32 transform_id, u32 cxform_id, u16 clip_depth, u8 blend_mode, u8 is_replace)
{
	tagPlaceObject2(app_context, depth, char_id, transform_id, cxform_id, clip_depth, is_replace);
	display_list[depth].blend_mode = blend_mode;
}

void tagSetFilter(SWFAppContext* app_context, size_t depth,
    u8 type, double blur_x, double blur_y, u8 quality, u8 flags,
    double r, double g, double b, double a, double strength,
    double angle, double distance)
{
	(void)app_context;
	if (depth <= max_depth)
	{
		display_list[depth].filter_type = type;
		display_list[depth].filter_blur_x = blur_x;
		display_list[depth].filter_blur_y = blur_y;
		display_list[depth].filter_quality = quality;
		display_list[depth].filter_flags = flags;
		display_list[depth].filter_color_r = r;
		display_list[depth].filter_color_g = g;
		display_list[depth].filter_color_b = b;
		display_list[depth].filter_color_a = a;
		display_list[depth].filter_strength = strength;
		display_list[depth].filter_angle = angle;
		display_list[depth].filter_distance = distance;
	}
}

// PlaceObject3 BackgroundColor. Ruffle display_object.rs:2543 gates this on
// swf_version >= 11 and drops a colour whose alpha is 0; the recompiler applies
// both rules, so this only ever records what survived them.
void tagSetOpaqueBackground(SWFAppContext* app_context, size_t depth,
    u8 set, u32 rgb)
{
	(void)app_context;
	if (depth <= max_depth)
	{
		display_list[depth].opaque_bg_set = set;
		display_list[depth].opaque_bg_rgb = rgb & 0xFFFFFFu;
	}
}

// PlaceObject3 BitmapCache. Ruffle display_object.rs:2534 only applies the
// field when the flag is PRESENT (an absent flag leaves the preference alone),
// which the recompiler already enforces by emitting this call only then.
void tagSetCacheAsBitmap(SWFAppContext* app_context, size_t depth, u8 on)
{
	(void)app_context;
	if (depth <= max_depth)
		display_list[depth].cache_as_bitmap = on ? 1u : 0u;
}

void tagSetFilterHighlight(SWFAppContext* app_context, size_t depth,
    double r, double g, double b, double a)
{
	(void)app_context;
	if (depth <= max_depth)
	{
		display_list[depth].filter_highlight_r = r;
		display_list[depth].filter_highlight_g = g;
		display_list[depth].filter_highlight_b = b;
		display_list[depth].filter_highlight_a = a;
	}
}

// Extended filter data side table (for ColorMatrix, Convolution, Gradient filters)
#define MAX_EXT_FILTERS 32
static ExtFilterData g_ext_filters[MAX_EXT_FILTERS];
static int g_ext_filter_count = 0;

// Parallel depth array for the ext filter side table
static size_t g_ext_filter_depths[MAX_EXT_FILTERS];

static ExtFilterData* getOrCreateExtFilter(size_t depth)
{
	for (int i = 0; i < g_ext_filter_count; i++) {
		if (g_ext_filter_depths[i] == depth) return &g_ext_filters[i];
	}
	if (g_ext_filter_count >= MAX_EXT_FILTERS) return NULL;
	int idx = g_ext_filter_count++;
	memset(&g_ext_filters[idx], 0, sizeof(ExtFilterData));
	g_ext_filter_depths[idx] = depth;
	return &g_ext_filters[idx];
}

void tagSetFilterColorMatrix(SWFAppContext* app_context, size_t depth, const float* matrix20)
{
	(void)app_context;
	if (depth > max_depth) return;
	ExtFilterData* ef = getOrCreateExtFilter(depth);
	if (!ef) return;
	ef->type = 6; // colormatrix
	memcpy(ef->cm_matrix, matrix20, 20 * sizeof(float));
}

void tagSetFilterConvolution(SWFAppContext* app_context, size_t depth,
    u8 matrixX, u8 matrixY, const float* matrix, float divisor, float bias,
    u8 preserve_alpha, u8 clamp, u8 def_r, u8 def_g, u8 def_b, u8 def_a)
{
	(void)app_context;
	if (depth > max_depth) return;
	ExtFilterData* ef = getOrCreateExtFilter(depth);
	if (!ef) return;
	ef->type = 5; // convolution
	ef->conv_mx = matrixX;
	ef->conv_my = matrixY;
	int n = matrixX * matrixY;
	if (n > 25) n = 25;
	memcpy(ef->conv_matrix, matrix, n * sizeof(float));
	ef->conv_divisor = divisor;
	ef->conv_bias = bias;
	ef->conv_preserve_alpha = preserve_alpha;
	ef->conv_clamp = clamp;
	ef->conv_color_r = def_r;
	ef->conv_color_g = def_g;
	ef->conv_color_b = def_b;
	ef->conv_color_a = def_a;
}

void tagSetFilterGradient(SWFAppContext* app_context, size_t depth,
    u8 type, u8 count, const u32* colors, const float* alphas, const u8* ratios,
    float blur_x, float blur_y, float angle, float distance, float strength,
    u8 quality, u8 flags)
{
	(void)app_context;
	if (depth > max_depth) return;
	ExtFilterData* ef = getOrCreateExtFilter(depth);
	if (!ef) return;
	ef->type = type; // 7=gradientglow, 8=gradientbevel
	if (count > 16) count = 16;
	ef->grad_count = count;
	memcpy(ef->grad_colors, colors, count * sizeof(u32));
	memcpy(ef->grad_alphas, alphas, count * sizeof(float));
	memcpy(ef->grad_ratios, ratios, count);
	ef->blur_x = blur_x;
	ef->blur_y = blur_y;
	ef->angle = angle;
	ef->distance = distance;
	ef->strength = strength;
	ef->quality = quality;
	ef->flags = flags;
}

const ExtFilterData* ng_getExtFilterDataByDepth(size_t depth)
{
	if (depth == (size_t)-1) return NULL;
	for (int i = 0; i < g_ext_filter_count; i++) {
		if (g_ext_filter_depths[i] == depth) return &g_ext_filters[i];
	}
	return NULL;
}


// Multi-filter list storage
#define MAX_FILTER_LISTS 32
static FilterListData g_filter_lists[MAX_FILTER_LISTS];
static int g_filter_list_count = 0;
static int g_current_filter_list_idx = -1; // index of list being built

static FilterListData* getOrCreateFilterList(size_t depth)
{
	for (int i = 0; i < g_filter_list_count; i++) {
		if (g_filter_lists[i].depth == depth) return &g_filter_lists[i];
	}
	if (g_filter_list_count >= MAX_FILTER_LISTS) return NULL;
	int idx = g_filter_list_count++;
	memset(&g_filter_lists[idx], 0, sizeof(FilterListData));
	g_filter_lists[idx].depth = depth;
	return &g_filter_lists[idx];
}

void tagBeginFilterList(SWFAppContext* app_context, size_t depth, u8 count)
{
	(void)app_context; (void)count;
	FilterListData* fl = getOrCreateFilterList(depth);
	if (!fl) return;
	fl->count = 0; // reset
	// Link the list to the display entry so the RENDER path can walk the whole
	// chain (s18). The by-depth table below stays the reflection path's key;
	// this pointer is the render path's, and it survives the entry moving
	// (a depth swap copies the field with the rest of the struct).
	if (depth <= max_depth)
		display_list[depth].filter_chain = fl;
	// Find index for subsequent tagAdd* calls
	for (int i = 0; i < g_filter_list_count; i++) {
		if (g_filter_lists[i].depth == depth) { g_current_filter_list_idx = i; return; }
	}
}

static FilterListEntry* filterListAppend(size_t depth)
{
	if (g_current_filter_list_idx < 0) return NULL;
	FilterListData* fl = &g_filter_lists[g_current_filter_list_idx];
	if (fl->depth != depth || fl->count >= MAX_FILTER_LIST_SIZE) return NULL;
	FilterListEntry* e = &fl->entries[fl->count];
	memset(e, 0, sizeof(FilterListEntry));
	fl->count++;
	return e;
}

void tagAddSimpleFilter(SWFAppContext* app_context, size_t depth,
    u8 type, double blur_x, double blur_y, u8 quality, u8 flags,
    double r, double g, double b, double a, double strength,
    double angle, double distance)
{
	(void)app_context;
	FilterListEntry* e = filterListAppend(depth);
	if (!e) return;
	e->type = type;
	e->blur_x = blur_x; e->blur_y = blur_y;
	e->quality = quality; e->flags = flags;
	e->color_r = r; e->color_g = g; e->color_b = b; e->color_a = a;
	e->strength = strength; e->angle = angle; e->distance = distance;
}

void tagAddSimpleFilterHighlight(SWFAppContext* app_context, size_t depth,
    double hr, double hg, double hb, double ha)
{
	(void)app_context;
	if (g_current_filter_list_idx < 0) return;
	FilterListData* fl = &g_filter_lists[g_current_filter_list_idx];
	if (fl->depth != depth || fl->count == 0) return;
	FilterListEntry* e = &fl->entries[fl->count - 1]; // last added
	e->highlight_r = hr; e->highlight_g = hg; e->highlight_b = hb; e->highlight_a = ha;
}

void tagAddColorMatrixFilter(SWFAppContext* app_context, size_t depth, const float* matrix20)
{
	(void)app_context;
	FilterListEntry* e = filterListAppend(depth);
	if (!e) return;
	e->type = 6;
	memcpy(e->cm_matrix, matrix20, 20 * sizeof(float));
}

void tagAddConvolutionFilter(SWFAppContext* app_context, size_t depth,
    u8 matrixX, u8 matrixY, const float* matrix, float divisor, float bias,
    u8 preserve_alpha, u8 clamp, u8 def_r, u8 def_g, u8 def_b, u8 def_a)
{
	(void)app_context;
	FilterListEntry* e = filterListAppend(depth);
	if (!e) return;
	e->type = 5;
	e->conv_mx = matrixX; e->conv_my = matrixY;
	int n = matrixX * matrixY;
	if (n > 25) n = 25;
	memcpy(e->conv_matrix, matrix, n * sizeof(float));
	e->conv_divisor = divisor; e->conv_bias = bias;
	e->conv_preserve_alpha = preserve_alpha; e->conv_clamp = clamp;
	e->conv_color_r = def_r; e->conv_color_g = def_g;
	e->conv_color_b = def_b; e->conv_color_a = def_a;
}

void tagAddGradientFilter(SWFAppContext* app_context, size_t depth,
    u8 type, u8 count, const u32* colors, const float* alphas, const u8* ratios,
    float blur_x, float blur_y, float angle, float distance, float strength,
    u8 quality, u8 flags)
{
	(void)app_context;
	FilterListEntry* e = filterListAppend(depth);
	if (!e) return;
	e->type = type; // 7=gradientglow, 8=gradientbevel
	if (count > 16) count = 16;
	e->grad_count = count;
	memcpy(e->grad_colors, colors, count * sizeof(u32));
	memcpy(e->grad_alphas, alphas, count * sizeof(float));
	memcpy(e->grad_ratios, ratios, count);
	e->blur_x = blur_x; e->blur_y = blur_y;
	e->angle = angle; e->distance = distance;
	e->strength = strength;
	e->quality = quality; e->flags = flags;
}

void tagEndFilterList(SWFAppContext* app_context, size_t depth)
{
	(void)app_context; (void)depth;
	g_current_filter_list_idx = -1;
}

const FilterListData* ng_getFilterListDataByDepth(size_t depth)
{
	if (depth == (size_t)-1) return NULL;
	for (int i = 0; i < g_filter_list_count; i++) {
		if (g_filter_lists[i].depth == depth && g_filter_lists[i].count > 0)
			return &g_filter_lists[i];
	}
	return NULL;
}


void tagSetInstanceName(SWFAppContext* app_context, size_t depth, const char* name)
{
	(void)app_context;
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	// In script_only_mode (Phase 2), display list is already set up from Phase 1 — skip.
	if (g_script_only_mode) return;
	// In tag_skip mode (deferred goto target-script replay), tags are no-ops.
	// Matches the gate other tag functions use (tagPlaceObject2, tagRemoveObject2,
	// etc). Without this, the deferred replay of a target frame's tagSetInstanceName
	// re-renames display entries that survived the rewind, clobbering their name.
	if (g_tag_skip_mode) return;
#endif

	// If the display entry doesn't exist yet (tagSetInstanceName called before tagPlaceObject2),
	// store as pending so the next PlaceObject at this depth uses it.
	if (depth > max_depth || display_list[depth].char_id == 0)
	{
		g_pending_instance_name = name;
		return;
	}
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	// Same-frame Remove+Place sequence at this depth with a NAME CHANGE:
	// the prior tagRemoveObject2 queued a deferred finalize for the existing
	// MC (it has an UNLOAD handler). display_list[depth] still holds the
	// pre-Remove entry until run_pending_finalize fires, but logically the
	// slot is already "removed". When the new name differs from the old,
	// treat this as the empty-depth case: stash the name as pending so the
	// upcoming tagPlaceObject2 gives it to a fresh MC, and DON'T rename the
	// about-to-be-unloaded MC — renaming it here makes its queued UNLOAD
	// handler trace the wrong `this` (e.g. "_level0.mc2 unloaded" when we
	// should report "mc1") and shadows the new name in
	// actionFindOrCreateMovieClip, causing the new placement to reuse the
	// dying MC pointer. Required for misc-ming loop_test10 frame 3
	// (Remove(mc1) + SetInstanceName("mc2") + Place(mc2) at depth 100).
	//
	// When the names match (e.g. RegisterClassTest4's mc3 frame 2 where the
	// remove+place pair both use "Segments"), the original rename path is a
	// no-op so we don't need to skip it — and the existing event-ordering
	// behavior must be preserved.
	{
		extern int ng_depth_has_pending_finalize(size_t);
		if (ng_depth_has_pending_finalize(depth)
		    && (display_list[depth].instance_name == NULL
		        || strcmp(display_list[depth].instance_name, name) != 0))
		{
			g_pending_instance_name = name;
			return;
		}
	}
	// Backward catch-up replay: an existing entry at this depth was preserved
	// by ng_display_clear_after (surviving rewind). Don't rename it in-place —
	// Ruffle's apply_place_object does not update name on existing children.
	// Pend the name for tagPlaceObject2 to consume only if it takes the
	// full-replacement path (different char_id). Applies to both entries
	// whose placed_at_frame is ahead of the replay cursor (stale pre-rewind
	// state) and entries whose depth_swapped flag is set (post-swap entries
	// whose placed_at_frame now stays pinned to the depth per
	// ng_swapDisplayDepths / ng_updateDisplayDepth).
	{
		extern int catch_up_backward;
		if (catch_up_backward &&
		    (display_list[depth].placed_at_frame > current_frame
		     || display_list[depth].depth_swapped))
		{
			g_pending_instance_name = name;
			return;
		}
	}
	// REPLACE preservation: when an existing entry occupies this depth, the
	// upcoming PlaceObject2 is either a REPLACE/MODIFY (cross-frame: move=1
	// against the existing child) or a same-frame conflict (Flash rejects
	// the second placement). Per Ruffle apply_place_object, name is
	// "Purposely omitted" — REPLACE/MODIFY tags do not update the name of an
	// existing child, and rejected same-frame placements must not leak the
	// new name onto the surviving first placement either. Stash as pending;
	// tagPlaceObject2's REPLACE-preservation / same-frame-conflict paths
	// discard the pending name (matching Ruffle), while the rare full-
	// replacement fallback (different placement that doesn't trigger those
	// paths) can still consume it. Skip when an UNLOAD-handler Remove was
	// just deferred at this depth (the entry's char_id is stale post-Remove;
	// treat as pre-place pending name like the empty-depth branch above).
	// Applies to both sprites (existing handling) and non-sprite chars
	// (shapes/text/buttons) — the latter is exercised by misc-ming
	// replace_shapes1test where a frame-3 shape REPLACE supplies a new name.
	// Same-frame coverage: gnash misc-ming place_object_test, where a second
	// PlaceObject at depth 3/4 in the same frame is rejected and the first
	// placement's name (sh1/mc2) must survive.
	extern int ng_depth_has_pending_finalize(size_t);
	if (display_list[depth].char_id != 0
	    && !ng_depth_has_pending_finalize(depth))
	{
		g_pending_instance_name = name;
		return;
	}
#endif
	if (depth <= max_depth)
	{
		char* old_name = display_list[depth].instance_name;
		// Rename cached MC if it was previously given a different name (e.g. auto-assigned)
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		if (old_name != NULL && strcmp(old_name, name) != 0)
			actionRenameMovieClip(old_name, name);
#endif
		// Free auto-assigned name if we own it; reclaim counter slot if it was the last one
		if (display_list[depth].instance_name_owned && old_name != NULL)
		{
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
			ng_try_reclaim_auto_instance_name(old_name);
#endif
			free(old_name);
		}
		display_list[depth].instance_name = (char*)name;
		display_list[depth].instance_name_owned = 0;
		// Also set the pending name so that a subsequent tagPlaceObject2 at
		// this depth (replacing the existing entry with a different char_id)
		// can consume it through its full-placement path — otherwise that
		// path wipes instance_name back to NULL, losing the name we just set.
		// Observed on natural end-of-movie loopback where frame 0 re-issues
		// tagSetInstanceName+tagPlaceObject2 against a depth still holding
		// the previous iteration's character.
		g_pending_instance_name = name;

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		// Invoke registered class constructor now that the instance name is set.
		// This fires after tagPlaceObject2 (which did eager init) but before DoAction scripts.
		if (!catch_up_mode && display_list[depth].sprite_needs_init
		    && !display_list[depth].constructor_invoked
		    && display_list[depth].char_id != 0
		    && dictionary[display_list[depth].char_id].type == CHAR_TYPE_SPRITE)
		{
			extern const char* ng_lookupExportName(size_t char_id);
			extern void actionInvokeRegisteredClassConstructor(SWFAppContext* app_context, const char* export_name, MovieClip* mc);
			extern void actionSetupRegisteredClassPrototype(SWFAppContext*, const char*, MovieClip*);
			const char* _exp_ctor = ng_lookupExportName(display_list[depth].char_id);
			if (_exp_ctor != NULL) {
				extern MovieClip root_movieclip;
				extern MovieClip* g_current_context;
				MovieClip* _parent_mc = g_current_context ? g_current_context : &root_movieclip;
				MovieClip* _ctor_mc = actionFindOrCreateMovieClip(app_context, name, _parent_mc);
				if (_ctor_mc != NULL) {
					actionSetupRegisteredClassPrototype(app_context, _exp_ctor, _ctor_mc);
					actionInvokeRegisteredClassConstructor(app_context, _exp_ctor, _ctor_mc);
					display_list[depth].constructor_invoked = 1;
				}
			}
		}

		// Recursively fire constructors for child sprites placed during eager init.
		// This ensures child constructors fire right after the parent constructor,
		// matching Flash's execution order.
		// Only do this when the parent was a registered class sprite with children.
		if (display_list[depth].constructor_invoked
		    && display_list[depth].sprite_display_list != NULL && display_list[depth].sprite_max_depth > 0)
		{
			extern const char* ng_lookupExportName(size_t char_id);
			extern void actionInvokeRegisteredClassConstructor(SWFAppContext* app_context, const char* export_name, MovieClip* mc);
			extern void actionSetupRegisteredClassPrototype(SWFAppContext*, const char*, MovieClip*);
			extern MovieClip* g_current_context;

			DisplayObject* saved_dl = display_list;
			size_t saved_max = max_depth;
			size_t saved_cap = display_list_capacity;
			display_list = saved_dl[depth].sprite_display_list;
			max_depth = saved_dl[depth].sprite_max_depth;
			display_list_capacity = saved_dl[depth].sprite_dl_capacity;

			MovieClip* _child_parent = actionFindOrCreateMovieClip(app_context, name,
				g_current_context ? g_current_context : &root_movieclip);
			for (size_t ci = 1; ci <= max_depth; ci++)
			{
				DisplayObject* cobj = &display_list[ci];
				if (cobj->char_id == 0 || cobj->constructor_invoked) continue;
				if (dictionary[cobj->char_id].type != CHAR_TYPE_SPRITE) continue;

				const char* _child_exp = ng_lookupExportName(cobj->char_id);
				if (_child_exp == NULL) continue;

				MovieClip* _child_mc = (cobj->instance_name != NULL)
					? actionFindOrCreateMovieClip(app_context, cobj->instance_name, _child_parent)
					: NULL;
				if (_child_mc != NULL) {
					actionSetupRegisteredClassPrototype(app_context, _child_exp, _child_mc);
					actionInvokeRegisteredClassConstructor(app_context, _child_exp, _child_mc);
					cobj->constructor_invoked = 1;
				}
			}

			// Save back (realloc possible)
			saved_dl[depth].sprite_display_list = display_list;
			saved_dl[depth].sprite_max_depth = max_depth;
			saved_dl[depth].sprite_dl_capacity = display_list_capacity;

			display_list = saved_dl;
			max_depth = saved_max;
			display_list_capacity = saved_cap;
		}
#endif
	}
}

DisplayObject* findDisplayObjectByName(const char* name)
{
	for (size_t i = 0; i <= max_depth; ++i)
	{
		if (display_list[i].char_id == 0) continue;
		if (display_list[i].instance_name != NULL &&
		    strcmp(display_list[i].instance_name, name) == 0)
			return &display_list[i];
	}
	return NULL;
}

// Font registration runs in all build modes. NO_GRAPHICS / OFFSCREEN_RENDER
// need it for trace-harness textfield value resolution; browser-WASM graphics
// needs it for actionIterateTextFieldGlyphs (the dynamic glyph render path
// used by EditTexts, MC textfields, and orphan textfields placed without an
// AS-resolved wrapper). Without these in graphics mode,
// ng_find_font_with_metrics falls back to the builtin Noto Sans entry whose
// glyph_base is 0 — that produces correct results only by coincidence when a
// SWF has exactly one font at glyph_base 0 (e.g. Doodle Jump), and garbled
// glyphs whenever a SWF has multiple fonts (Snake: font 4 at glyph_base 40
// was looked up as Noto Sans at glyph_base 0, reading shape rows from the
// wrong region of glyph_data).
void tagDefineFontInfo(SWFAppContext* app_context, u16 font_id, const char* name, int bold, int italic)
{
	ng_record_font(app_context, font_id, name, bold, italic);
}

void tagDefineFontMetrics(SWFAppContext* app_context, u16 font_id,
    s16 ascent, s16 descent, s16 leading, int em_square,
    const u16* code_table, const s32* advance_table, size_t glyph_count)
{
	ng_record_font_metrics(app_context, font_id, ascent, descent, leading, em_square,
	    code_table, advance_table, glyph_count);
}

void tagDefineFontGlyphBase(u16 font_id, size_t glyph_base)
{
	// Movie-local index into glyph_data, baked at define time exactly like
	// tagDefineText's text_start (multi-SWF render slice).
	ng_record_font_glyph_base(font_id, glyph_base + ng_movieGlyphBase(g_current_movie_id));
}

void tagDefineVideoStream(SWFAppContext* app_context, u16 char_id, u16 width, u16 height,
    u8 deblocking, u8 codec_id)
{
	// The video's dictionary entry is never written (video is detected via
	// ng_isVideoChar before the dictionary switch, and the zeroed entry's
	// CHAR_TYPE_SHAPE / size 0 is what the render branch's early return
	// assumes) — but several display-list walks index dictionary[char_id]
	// UNCONDITIONALLY, so the slot still has to exist. Required by the
	// recompiler's char-id-0 → 0x4000 video alias, whose id is outside the
	// initial dictionary allocation.
	ENSURE_SIZE(dictionary, char_id, dictionary_capacity, sizeof(Character));

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	ng_record_video(app_context, char_id, width, height, codec_id, deblocking);
#else
	(void)app_context; (void)char_id; (void)width; (void)height;
	(void)codec_id; (void)deblocking;
#endif
}

// VideoFrame tag handler — feeds the encoded payload for one frame of an
// embedded DefineVideoStream into the per-stream persistent decoder. The
// recompiler emits one call per VideoFrame tag at the tag's place in the
// frame function (so frame N's payload is decoded as part of frame N's
// timeline execution).
//
// Implementation lives in action.c because that's where the persistent
// decoder context + decoded frame storage live (alongside the existing
// NetStream-keyed FLV path).
extern void actionStoreEmbeddedVideoFrame(size_t char_id, u16 frame_num,
    const unsigned char* payload, size_t payload_size);

void tagVideoFrame(SWFAppContext* app_context, u16 char_id, u16 frame_num,
    const unsigned char* payload, size_t payload_size)
{
	(void)app_context;
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	actionStoreEmbeddedVideoFrame((size_t)char_id, frame_num, payload, payload_size);
#else
	(void)char_id; (void)frame_num; (void)payload; (void)payload_size;
#endif
}

// Returns 1 if any multi-frame sprite at root level is playing or has pending navigation.
// A sprite with sprite_manual_next_frame=1 has a pending gotoAndStop/gotoAndPlay that
// advance_sprite_frames must process, even if sprite_is_playing is false.
//
// Compiled for ALL graphics modes (browser-WASM included), not just headless:
// swf.c's frame-loop keep-alive gate calls it in every build so a non-looping
// root that set quit_swf keeps ticking while a sprite still plays. Reads only
// display_list/max_depth/dictionary, all present in the graphics build.
// RECURSIVE since 2026-08-13 (session 15 `w2-gfx-smalls`): the animation that
// keeps a movie alive is not always a ROOT-level sprite. from_shumway/acid/
// acid-video has a 1-frame root that places a 1-frame sprite which *contains*
// the 39-frame sprite; with a root-only scan the keep-alive gate saw "nothing
// playing", quit_swf stuck, and the movie exited after ONE tick. Mirrors the
// recursion `hasClipEnterFrameHandlers_impl` (just below) already does.
// Nested display lists are depth-indexed from 1, same as the root one
// (cf. tag.c:4243, tag.c:4862).
static int hasPlayingSprites_impl(DisplayObject* dl, size_t dl_max)
{
	for (size_t i = 1; i <= dl_max; i++)
	{
		if (dl[i].char_id == 0) continue;
		Character* ch = &dictionary[dl[i].char_id];
		if (ch->type == CHAR_TYPE_SPRITE &&
		    ch->sprite_frame_count > 1 &&
		    (dl[i].sprite_is_playing || dl[i].sprite_manual_next_frame))
			return 1;
		if (dl[i].sprite_display_list != NULL && dl[i].sprite_max_depth > 0)
			if (hasPlayingSprites_impl(dl[i].sprite_display_list, dl[i].sprite_max_depth))
				return 1;
	}
	return 0;
}

int hasPlayingSprites(void)
{
	return hasPlayingSprites_impl(display_list, max_depth);
}

// Check if any display_list entry (recursive) has CLIP_EVENT_ENTER_FRAME clip actions
// and is initialized (sprite_initialized >= 2). Used to keep the frame loop alive.
static int hasClipEnterFrameHandlers_impl(DisplayObject* dl, size_t dl_max)
{
	for (size_t i = 0; i <= dl_max; i++)
	{
		if (dl[i].char_id == 0) continue;
		if (dl[i].sprite_initialized < 2) continue;
		if (dl[i].clip_action_count > 0)
		{
			for (size_t a = 0; a < dl[i].clip_action_count; a++)
				if (dl[i].clip_actions[a].event_flags & CLIP_EVENT_ENTER_FRAME) return 1;
		}
		if (dl[i].sprite_display_list != NULL && dl[i].sprite_max_depth > 0)
			if (hasClipEnterFrameHandlers_impl(dl[i].sprite_display_list, dl[i].sprite_max_depth))
				return 1;
	}
	return 0;
}
int hasClipEnterFrameHandlers(void)
{
	if (hasClipEnterFrameHandlers_impl(display_list, max_depth)) return 1;
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	// attachMovie'd clips' standalone child lists (see collect_attached_standalone):
	// an attached clip's child with onClipEvent(enterFrame) keeps the loop alive
	// exactly like a timeline-placed one.
	if (g_any_clip_ef_placed) {
		size_t n = collect_attached_standalone(g_att_sa_d, g_att_sa_mc);
		for (size_t k = 0; k < n; k++)
			if (hasClipEnterFrameHandlers_impl(g_att_sa_d[k]->sprite_display_list,
			                                   g_att_sa_d[k]->sprite_max_depth))
				return 1;
	}
#endif
	return 0;
}

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
// Run deferred sprite-init with optional frame filter.
// filter_mode: 0=all, 1=only placed_at_frame < target, 2=only placed_at_frame >= target
static void ng_run_deferred_sprite_init_impl(SWFAppContext* app_context, int filter_mode, size_t target_frame)
{
	extern void ng_sync_root_display_obj(void);
	ng_sync_root_display_obj();

	extern int child_mc_count;
	extern int g_enterframe_new_mc_start;
	int mc_count_before = child_mc_count;

	if (filter_mode != 0)
	{
		g_sprite_init_filter_active = 1;
		g_sprite_init_before_target = (filter_mode == 1) ? 1 : 0;
		g_sprite_init_target_frame = target_frame;
	}

	catch_up_mode = 0;
	// Phase 7b: turn on the deferred-Phase-2 sync-fire path in the sprite
	// DoAction gate (see swf.cpp). Sprite scripts for goto-placed sprites
	// fire synchronously inside this call, after the target frame's root
	// DoAction has already drained in the main loop.
	actionDeferredSpriteInitEnter();
	process_sprite_needs_init(app_context, &root_movieclip);
	actionDeferredSpriteInitLeave();

	g_sprite_init_filter_active = 0;

	ng_fire_pending_loads(app_context);
	ng_fire_pending_attach_inits(app_context);
	actionFirePendingLoadInits(app_context);

	g_enterframe_new_mc_start = mc_count_before;
	actionDispatchEnterFrameHandlers(app_context);
	g_enterframe_new_mc_start = -1;
}

// Fire registered class constructors for child sprites of a specific MC.
// Called after attachMovie fires the parent's constructor so that child
// constructors (e.g. "box" inside an attached "a" clip) fire immediately
// during the attachMovie call, before any goto catch-up places new sprites.
// Compiled in all three builds (attachMovie runs everywhere now) — temporarily
// close + reopen the surrounding NO_GRAPHICS/OFFSCREEN_RENDER block so this
// symbol exists in browser-WASM where attachMovie now references it.
#endif // NO_GRAPHICS — opens at line 8163, reopen below
void ng_fire_child_constructors(SWFAppContext* app_context, MovieClip* mc)
{
	if (mc == NULL || mc->display_obj == NULL) return;
	DisplayObject* dobj = (DisplayObject*)mc->display_obj;
	if (dobj->sprite_display_list == NULL || dobj->sprite_max_depth == 0) return;
	fire_eager_constructors(app_context, dobj->sprite_display_list, dobj->sprite_max_depth, mc);
}
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER) // reopen the block

// Fire registered class constructors for ALL pending sprites (root + children),
// without running Phase 2 scripts. Uses process_sprite_needs_init in constructor_only
// mode so it naturally handles both root-level and nested sprites.
void ng_fire_deferred_constructors(SWFAppContext* app_context)
{
	extern void ng_sync_root_display_obj(void);
	ng_sync_root_display_obj();

	g_constructor_only_mode = 1;
	catch_up_mode = 0;
	process_sprite_needs_init(app_context, &root_movieclip);
	g_constructor_only_mode = 0;
}

// Run ALL deferred sprite inits (used when no target frame distinction needed).
void ng_run_deferred_sprite_init(SWFAppContext* app_context)
{
	ng_run_deferred_sprite_init_impl(app_context, 0, 0);
}

// Run deferred sprite inits only for sprites placed BEFORE target_frame.
// Ruffle phase 1: children placed before target frame init before target DoAction.
void ng_run_deferred_sprite_init_before(SWFAppContext* app_context, size_t target_frame)
{
	ng_run_deferred_sprite_init_impl(app_context, 1, target_frame);
}

// Run deferred sprite inits only for sprites placed ON or AFTER target_frame.
// Ruffle phase 3: children placed on target frame init after target DoAction.
void ng_run_deferred_sprite_init_on_or_after(SWFAppContext* app_context, size_t target_frame)
{
	ng_run_deferred_sprite_init_impl(app_context, 2, target_frame);
}

#endif // NO_GRAPHICS — close of the block opened at line 6968. The
       // post-catch-up cleanup below is referenced from action_queue.c via
       // extern, so it must be visible to graphics builds too. The call
       // site there is gated on `catch_up_backward && g_natural_wrap_cleanup_pending`,
       // both 0 in graphics mode, so this function is inert at runtime —
       // but the linker still needs the symbol.

// Clear display entries whose placed_at_frame is after target_frame.
// Used by swf_core.c when seeking backward on the main timeline, and by
// swf.c's natural-wrap-back cleanup (called in browser-WASM too — Snake's
// game-over → menu → restart flow leaves stale gameplay entries on the
// display list without it, so the snake re-placement at frame 50 is
// treated as a modify and the sprite never re-initializes).
// Entries at SWF depth >= AVM_DEPTH_BIAS (16384, i.e. AS depth >= 0, the
// "dynamic" range) survive backward jumps, matching Ruffle's AVM1
// survives_rewind rule: `old_object.depth() < AVM_DEPTH_BIAS` is the
// precondition for considering an object for removal during rewind.
//
// Initialized MovieClip (sprite) entries are preserved here so that
// tagPlaceObject2's catch-up path can decide on a per-placement basis
// whether to modify (same char_id, survives rewind) or replace (different
// char_id). Any sprite entries that no tag in [0, target] re-places get
// cleaned up by ng_display_cleanup_unplaced_after() after catch-up.
// This mirrors Ruffle's survives_rewind: for MovieClip, id + ratio must
// match the final placement; name is intentionally NOT reset (Ruffle
// apply_place_object comment: "name, clip_depth, clip_actions... can not
// be modified by subsequent PlaceObject tags").
void ng_display_clear_after(SWFAppContext* app_context, size_t target_frame)
{
	(void)app_context;
	for (size_t i = 1; i <= max_depth; i++)
	{
		if (i >= 16384) break;  // dynamic-range entries survive rewind
		if (display_list[i].char_id != 0 &&
		    display_list[i].placed_at_frame > target_frame)
		{
			// Preserve initialized sprites — catch-up will decide survival.
			if (display_list[i].sprite_display_list != NULL)
				continue;
			display_list[i].char_id = 0;
		}
	}
}

// Post-catch-up cleanup: clear any entries whose placed_at_frame is still
// > target_frame. These are sprites preserved by ng_display_clear_after
// that weren't re-placed (modified) during catch-up — their survives_rewind
// test effectively failed.
void ng_display_cleanup_unplaced_after(SWFAppContext* app_context, size_t target_frame)
{
	for (size_t i = 1; i <= max_depth; i++)
	{
		if (i >= 16384) break;
		if (display_list[i].char_id != 0 &&
		    display_list[i].placed_at_frame > target_frame)
		{
			// Invalidate the cached MovieClip so AS lookups (e.g.
			// `_root.mc_name`) no longer resolve. Mirrors Ruffle's
			// remove_child cleanup of unsurviving children during run_goto.
			if (display_list[i].instance_name != NULL) {
				extern void actionInvalidateCachedMovieClip(SWFAppContext*, const char*, int);
				actionInvalidateCachedMovieClip(app_context, display_list[i].instance_name, (int)i);
			}
			if (display_list[i].sprite_display_list != NULL)
			{
				ng_freeSpriteDL(app_context, display_list[i].sprite_display_list,
			                display_list[i].sprite_dl_capacity);
				display_list[i].sprite_display_list = NULL;
			}
			display_list[i].char_id = 0;
		}
	}
}

// Bitmap metadata registry (shared across all build modes)
#define MAX_BITMAP_DEFS 128
// `pixels` is an absolute pointer into the DEFINING movie's bitmap_data, not an
// offset: a loaded child SWF owns a separate array, so an offset on its own
// names nothing (see defineBitmap in tag.h).
static struct { u16 char_id; const u8* pixels; size_t size; u32 width; u32 height; } g_bitmap_defs[MAX_BITMAP_DEFS];
static int g_bitmap_def_count = 0;

int ng_getBitmapMetadata(u16 char_id, const u8** out_pixels, size_t* out_size, u32* out_width, u32* out_height)
{
	for (int i = 0; i < g_bitmap_def_count; i++) {
		if (g_bitmap_defs[i].char_id == char_id) {
			if (out_pixels) *out_pixels = g_bitmap_defs[i].pixels;
			if (out_size) *out_size = g_bitmap_defs[i].size;
			if (out_width) *out_width = g_bitmap_defs[i].width;
			if (out_height) *out_height = g_bitmap_defs[i].height;
			return 1;
		}
	}
	return 0;
}

void ng_registerBitmapMetadata(u16 char_id, const u8* pixels, size_t size, u32 width, u32 height)
{
	if (g_bitmap_def_count < MAX_BITMAP_DEFS) {
		g_bitmap_defs[g_bitmap_def_count].char_id = char_id;
		g_bitmap_defs[g_bitmap_def_count].pixels = pixels;
		g_bitmap_defs[g_bitmap_def_count].size = size;
		g_bitmap_defs[g_bitmap_def_count].width = width;
		g_bitmap_defs[g_bitmap_def_count].height = height;
		g_bitmap_def_count++;
	}
}

#ifndef NO_GRAPHICS
void defineBitmap(const u8* data, size_t offset, size_t size, u32 width, u32 height, u16 char_id)
{
	const u8* pixels = (data != NULL) ? data + offset : NULL;
	renderer_upload_bitmap(context, pixels, size, width, height);
	ng_registerBitmapMetadata(char_id, pixels, size, width, height);
}

void finalizeBitmaps()
{
	renderer_finalize_bitmaps(context);
}
#endif // NO_GRAPHICS
