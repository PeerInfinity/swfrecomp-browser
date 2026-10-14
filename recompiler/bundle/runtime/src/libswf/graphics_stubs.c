// graphics_stubs.c — definitions for symbols that exist as full
// implementations in NO_GRAPHICS / OFFSCREEN_RENDER builds but
// are referenced (via extern in action_queue.c, etc.) in graphics builds.
//
// Compiled in both:
//   - wasm browser graphics (USE_WEBGPU, no NO_GRAPHICS / OFFSCREEN_RENDER)
//   - --mode=graphics native (USE_WEBGPU + OFFSCREEN_RENDER)
//
// Symbols split into two groups:
//   1. Always-active stubs (zero-init globals, no-op functions whose
//      semantics are correct for any graphics mode).
//   2. Wasm-only stubs (#ifndef OFFSCREEN_RENDER) for symbols that tag.c's
//      widened (NO_GRAPHICS || OFFSCREEN_RENDER) arm now provides for
//      --mode=graphics — wasm graphics doesn't get those, so this file
//      still needs to fill the gap.

#if defined(USE_WEBGPU) && !defined(NO_GRAPHICS)

#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <swf.h>
#include <tag.h>
#include <action.h>  // full struct MovieClip needed by exec_sprite_frame body

// ---------------------------------------------------------------------------
// Group 1: Always-active stubs (both wasm and graphics-native)
// ---------------------------------------------------------------------------

// Backward-goto catch-up state. swf_core.c manages these in the
// NO_GRAPHICS frame loop; in graphics builds the frame loop has
// no catch-up phase, so the variables stay 0 here — that's the correct
// "no catch-up in progress" state, not a stub. action_queue.c reads them
// to gate cleanup work that would otherwise happen during a backward goto;
// with the variables permanently 0 those branches are inert, matching the
// intent that graphics-mode swf.c skips that work.
int catch_up_backward = 0;
size_t catch_up_target = 0;
int g_natural_wrap_cleanup_pending = 0;

// SetTarget state flags. Real definitions live in action.c (now widened).
// In wasm graphics, these stubs keep the save→reset→restore cycle a no-op.
#ifndef OFFSCREEN_RENDER
int g_settarget_explicit_root = 0;
int g_settarget_invalid = 0;
int g_settarget_none = 0;
#endif

// Force-quit flag — set by exit handlers in NO_GRAPHICS swf_core.c.
// Graphics frame loop in swf.c uses its own quit_swf flag.
int g_force_quit = 0;

// Active transform data pointer. Real impl in tag.c (NO_GRAPHICS arm)
// swaps to a child SWF's transform array; with NULL, ng_cache_transform
// in tag.c falls back to the main SWF's transform_data — correct for
// single-SWF tests, possibly wrong for multi-SWF (loadMovie) tests.
// In OFFSCREEN_RENDER, tag.c provides this via the widened gate.
#ifndef OFFSCREEN_RENDER
float (*g_active_transform_data)[16] = NULL;
#endif

// Clone-depth-already-unbiased flag — set by createEmptyMovieClip in
// swf_core.c. In OFFSCREEN_RENDER action.c (widened) provides it.
#ifndef OFFSCREEN_RENDER
int g_clone_depth_already_unbiased = 0;
#endif

// JS-callback inputs (text input, IME, focus). In emscripten browser builds
// these live in render_webgpu.c populated by JS event listeners (gated by
// __EMSCRIPTEN__ there), so we must NOT redefine them here for wasm graphics.
// In native offscreen mode render_webgpu.c's __EMSCRIPTEN__ arm is inactive,
// so the symbols are missing — provide zero-init stubs. swf.c's per-frame
// drain then reads zero and no events fire.
#ifdef OFFSCREEN_RENDER
#define _OR_TEXT_INPUT_RING_SIZE 64
#define _OR_IME_TEXT_BUF_SIZE 256
int g_text_input_ring[_OR_TEXT_INPUT_RING_SIZE];
int g_text_input_ring_head = 0;
int g_text_input_ring_tail = 0;
int g_window_focus_lost = 0;
char g_ime_compose_text[_OR_IME_TEXT_BUF_SIZE];
char g_ime_commit_text[_OR_IME_TEXT_BUF_SIZE];
int g_ime_compose_pending = 0;
int g_ime_commit_pending = 0;
#endif

// sprite_content_bounds_twips and ng_queue_placement_clip_events:
// stubs for wasm graphics. In OFFSCREEN_RENDER tag.c provides real impls.
#ifndef OFFSCREEN_RENDER
int sprite_content_bounds_twips(DisplayObject* dl, size_t dl_max,
                                float* xmin_out, float* xmax_out,
                                float* ymin_out, float* ymax_out) {
    // Real bounds (twips) of a display list, recursing into nested sprites, via
    // the shared bounds engine (tag_stubs.c, linked in browser-WASM). This was a
    // 0-returning stub, so ng_attachMovie set every attached clip's mc->width/
    // height to 0 — e.g. Minesweeper's FRadioButton circle frb_states_mc._width
    // read 0, so FRadioButton.setLabelPlacement put fLabel_mc._x = radioWidth = 0
    // and the label drew on top of the circle (circle invisible), and the
    // component width math fell short (clipped labels). NO_GRAPHICS/OFFSCREEN have
    // the real impl in tag.c. Output order matches the callers: (xmin, xmax, ymin,
    // ymax).
    extern int ng_computeBoundsFromDL_matrix(DisplayObject*, size_t,
        double, double, double, double, double, double,
        int*, double*, double*, double*, double*);
    int has = 0;
    double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
    if (dl != NULL)
        ng_computeBoundsFromDL_matrix(dl, dl_max, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0,
            &has, &xmin, &ymin, &xmax, &ymax);
    if (xmin_out) *xmin_out = (float)xmin;
    if (xmax_out) *xmax_out = (float)xmax;
    if (ymin_out) *ymin_out = (float)ymin;
    if (ymax_out) *ymax_out = (float)ymax;
    return has;
}

void ng_queue_placement_clip_events(SWFAppContext* app_context, size_t depth) {
    (void)app_context; (void)depth;
}

// actionRemoveSprite (action.c) calls this when a clone has a backing
// display_list slot — but in browser-WASM graphics ng_cloneSprite skips
// the display_list write (see tag_stubs.c gate), so the lookup that gates
// the call returns SIZE_MAX and this stub is never actually reached for
// clones. Stub exists only to satisfy the linker. attachMovie clones go
// through ng_attachMovie which uses a separate display_obj wrapper outside
// the global display_list, so they don't have UNLOAD clip-action events
// queued through this path either.
void ng_queue_slot_unload_events(SWFAppContext* app_context, size_t depth, MovieClip* mc) {
    (void)app_context; (void)depth; (void)mc;
}
#endif

// ---------------------------------------------------------------------------
// Group 2: Wasm-only stubs (#ifndef OFFSCREEN_RENDER)
// In --mode=graphics native, tag.c's (NO_GRAPHICS || OFFSCREEN_RENDER) arm
// provides real impls for these — defining them here would multi-define.
// Wasm graphics still needs the stubs.
// ---------------------------------------------------------------------------

#ifndef OFFSCREEN_RENDER

// Sprite-init / catch-up state-machine accessors. Real impls in tag.c.
// In wasm graphics, frame execution is straight-through — the accessors all
// return 0 and Enter/Leave wrappers are no-ops. The recompiler-generated
// tagMain.c queries these via the accessor to choose between sync-fire and
// queue modes; with all returning 0, sync-fire path is used.
int actionEagerInitActive(void)            { return 0; }
int actionScriptOnlyMode(void)             { return 0; }
int actionGotoCatchupActive(void)          { return 0; }
int actionDeferredSpriteInitActive(void)   { return 0; }
int actionAttachInitActive(void)           { return 0; }
void actionEagerInitEnter(void)            {}
void actionEagerInitLeave(void)            {}
void actionGotoCatchupEnter(void)          {}
void actionGotoCatchupLeave(void)          {}
void actionDeferredSpriteInitEnter(void)   {}
void actionDeferredSpriteInitLeave(void)   {}
void actionAttachInitEnter(void)           {}
void actionAttachInitLeave(void)           {}

// Sprite-init-depth bump/unbump (real impls in tag.c NO_GRAPHICS arm).
void ng_bumpSpriteInitDepth(void)          {}
void ng_unbumpSpriteInitDepth(void)        {}
int ng_isInsideSpriteInit(void)            { return 0; }

// process_sprite_needs_init_public: NO_GRAPHICS sprite-needs-init phase.
// In wasm graphics this is a no-op; in --mode=graphics tag.c provides
// the real impl.
void process_sprite_needs_init_public(SWFAppContext* app_context, MovieClip* parent_mc) {
    (void)app_context; (void)parent_mc;
}

// ng_set_script_only_mode: NO_GRAPHICS script-only re-run phase.
void ng_set_script_only_mode(int mode) { (void)mode; }

// exec_sprite_frame: in wasm graphics this is a minimal context tracker.
// Sets g_current_sprite_obj for the duration of the frame call so that
// ng_isInsideSprite() / ng_stopCurrentSprite() / ng_playCurrentSprite() —
// which the recompiler-emitted scripts hit when a sprite frame calls
// actionStop/actionPlay directly — can identify "we're inside a sprite"
// and route the call to the sprite's display entry rather than falling
// through to the root timeline's is_playing flag. (Doodle Jump's hero
// sprite has a frame-0 actionStop() that was stopping the root and
// leaving the sprite cycling.) --mode=graphics native uses tag.c's
// full impl which does the same plus context/base-clip/transform swaps.
void exec_sprite_frame(SWFAppContext* app_context, DisplayObject* obj, frame_func f) {
    extern DisplayObject* g_current_sprite_obj;
    DisplayObject* saved = g_current_sprite_obj;
    g_current_sprite_obj = obj;

    // Browser-WASM doesn't run process_sprite_needs_init (gated to
    // NO_GRAPHICS/OFFSCREEN_RENDER) which is where timeline-placed sprites
    // normally get their MovieClip created & the frame-script context swap
    // happens. For sprites WITHOUT clip actions (Doodle Jump's "container"
    // at depth 2, an attachMovie target whose sprite-frame-script
    // `this.attachMovie("cloud", ...)` needs `this`=container, not root),
    // we mirror tag.c here: find-or-create the MC + swap g_current_context.
    // Sprites WITH clip actions (DJ's menu buttons sprite_46 at depths
    // 3/5/7/8 — clip_actions_145..148) are skipped because their MCs are
    // already created via the clip-event LOAD dispatch path; creating a
    // shadow MC here breaks button hit-test + the LOAD-set button label.
    MovieClip* saved_ctx = g_current_context;
    MovieClip* saved_base = actionGetBaseClip();
    int did_swap = 0;
    MovieClip* swapped_mc = NULL;
    if (obj != NULL && obj->instance_name != NULL && obj->clip_action_count == 0) {
        extern MovieClip* actionFindOrCreateMovieClip(SWFAppContext*, const char*, MovieClip*);
        extern MovieClip root_movieclip;
        MovieClip* parent = (g_current_context != NULL) ? g_current_context : &root_movieclip;
        MovieClip* mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent);
        if (mc) {
            mc->display_obj = (void*)obj;
            actionSetCurrentContext(mc);
            actionSetBaseClip(mc);
            swapped_mc = mc;
            did_swap = 1;
        }
    }

    if (f) f(app_context);

    // Fire the registered-class constructor for a timeline-placed sprite the
    // first time its frame script runs, mirroring process_sprite_needs_init's
    // post-frame-0 constructor dispatch (tag.c, gated to NO_GRAPHICS/OFFSCREEN).
    // Browser-WASM skips process_sprite_needs_init entirely, so without this a
    // sprite bound to a class via Object.registerClass never runs its
    // constructor — e.g. Minesweeper's FRadioButton instances (level_eazy/
    // medium/tough) whose constructor calls this.init() to attachMovie the
    // radio circle/dot (frb_states_mc) and label (fLabel_mc). The component
    // sub-clips are only created by that init(); without it the radio shows
    // just the timeline-placed boundingBox.
    // actionInvokeRegisteredClassConstructor self-guards: it no-ops when no
    // class is registered for the symbol, so ordinary exported sprites (Tetris
    // board cells, DJ platforms, etc.) are unaffected. The constructor_invoked
    // flag (on the persistent display_list entry) makes it fire once per
    // instance even though exec_sprite_frame runs every tick. Attached clips
    // get their constructor from attachMovie's own ng_fire_child_constructors
    // path and don't reach here.
    if (obj != NULL && obj->instance_name != NULL && !obj->constructor_invoked) {
        extern void* lookupRegisteredClassByCharId(size_t, int, const char**);
        extern int g_swf_version;
        // Gate on a class actually being registered for this character (via
        // Object.registerClass). Without this gate, the find-or-create below
        // would mint an MC for every exported clip-action sprite — risking a
        // duplicate "shadow" MC for sprites whose MC is normally created by the
        // clip-event LOAD path (breaks button hit-test, per the did_swap skip
        // above). lookupRegisteredClassByCharId is side-effect-free.
        if (lookupRegisteredClassByCharId(obj->char_id, g_swf_version, NULL) != NULL) {
            extern const char* ng_lookupExportName(size_t char_id);
            extern void actionInvokeRegisteredClassConstructor(SWFAppContext*, const char*, MovieClip*);
            extern MovieClip* actionFindOrCreateMovieClip(SWFAppContext*, const char*, MovieClip*);
            extern MovieClip root_movieclip;
            const char* _rc_export = ng_lookupExportName(obj->char_id);
            if (_rc_export != NULL) {
                // did_swap sprites already have their MC; clip-action sprites
                // (did_swap==0) don't, and the clip-event LOAD path that
                // normally creates it may not have run yet on this first
                // frame-execution. Find-or-create with the caller's context as
                // parent resolves the canonical MC (the LOAD path
                // find-or-creates the same one by name+parent, so no duplicate).
                MovieClip* cmc = swapped_mc;
                if (cmc == NULL) {
                    MovieClip* _p = g_current_context ? g_current_context : &root_movieclip;
                    cmc = actionFindOrCreateMovieClip(app_context, obj->instance_name, _p);
                    if (cmc != NULL) cmc->display_obj = (void*)obj;
                }
                if (cmc != NULL) {
                    obj->constructor_invoked = 1;
                    // The sprite's frame func (`f`, run above) placed this
                    // sprite's timeline children into the GLOBAL display_list/
                    // max_depth (the caller — advance_sprite_frames / the goto
                    // replay paths — swapped the globals to this sprite's list
                    // before calling us, and only writes obj->sprite_* back
                    // AFTER we return). So obj->sprite_max_depth is still stale
                    // (0 on the first run) right now. Sync it from the globals
                    // before invoking the constructor, so init()'s
                    // `this.<timelineChild>` GetMember (which scans
                    // mc->display_obj->sprite_display_list, i.e. obj's) finds
                    // the children (e.g. FRadioButton.init's
                    // this.boundingBox_mc.unloadMovie()). Also repoints
                    // obj->sprite_display_list at the (possibly realloc'd)
                    // global buffer, avoiding a stale/freed pointer read.
                    {
                        extern DisplayObject* display_list;
                        extern size_t max_depth;
                        extern size_t display_list_capacity;
                        obj->sprite_display_list = display_list;
                        obj->sprite_max_depth = max_depth;
                        obj->sprite_dl_capacity = display_list_capacity;
                    }
                    // Reflect the sprite's PLACEMENT matrix scale onto its MC's
                    // _xscale/_yscale before the constructor runs. In Flash a
                    // timeline-placed clip's _xscale/_yscale equal its placement
                    // matrix scale (e.g. Minesweeper's radios are placed at 108%/125%
                    // to fit their labels). The constructor's init() reads
                    // `this._width` = local-bounds × _xscale to size the component;
                    // without this, _xscale stays 100 so every radio reads the same
                    // unscaled width and the labels clip to a single fixed width.
                    // Browser-WASM's exec_sprite_frame never set this (NO_GRAPHICS/
                    // OFFSCREEN do it in their placement/init path). Gate on the
                    // scale not being AS-overridden yet (init later sets _xscale=100,
                    // which sets the flags, so subsequent frames won't re-apply).
                    {
                        if (!(cmc->as_set_flags & (4|8))) {
                            if (obj->place_a != 0.0f || obj->place_b != 0.0f) {
                                float _sx = sqrtf(obj->place_a*obj->place_a + obj->place_b*obj->place_b);
                                float _sy = sqrtf(obj->place_c*obj->place_c + obj->place_d*obj->place_d);
                                if (_sx > 0.0f) cmc->xscale = _sx * 100.0f;
                                if (_sy > 0.0f) cmc->yscale = _sy * 100.0f;
                            }
                        }
                        // Reflect the PLACEMENT matrix translation onto _x/_y too
                        // (the natural completion of the _xscale/_yscale fix above).
                        // A timeline-placed clip's _x/_y equal its placement origin
                        // in Flash; browser-WASM left registerClass MCs at (0,0), so
                        // their stage position lived only in display_obj's transform.
                        // Without _x/_y the AABB / hit-test path (which walks mc->x +
                        // parent chain) computes a near-origin box, and the component
                        // is un-clickable (Minesweeper's radios). Gate on _x/_y not
                        // being AS-overridden yet. Rendering uses the display-list
                        // transform (not mc->x), so this is hit-test/_x metadata only.
                        {
                            const float* _pt = (const float*)app_context->transform_data
                                + (size_t)obj->transform_id * 16;
                            if (!(cmc->as_set_flags & 1)) cmc->x = _pt[12] / 20.0f;
                            if (!(cmc->as_set_flags & 2)) cmc->y = _pt[13] / 20.0f;
                        }
                    }
                    // Eager-build this sprite's NESTED timeline children (run their
                    // frame-0) before the constructor. The frame func `f` above
                    // placed this sprite's direct children (e.g. FRadioButton's
                    // boundingBox_mc at depth 1), but their own sub-content (the
                    // boundingBox shape, nested one level deeper) is only built by
                    // the deferred nested-sprite advance that the caller runs AFTER
                    // we return — i.e. after the constructor. init() then reads
                    // this._width (the component width, defined by boundingBox_mc)
                    // and gets 0, collapsing the radio label to ~2 chars. The
                    // caller already swapped the globals to this sprite's list, so
                    // advance_sprite_frames here iterates this sprite's children and
                    // runs their just_allocated frame-0. Mark g_exec_eager_built_obj
                    // so the caller's post-CALL_FRAME recursion skips the redundant
                    // re-advance (double-stepping corrupts nested frame counters).
                    // catch_up_mode is suppressed-script replay — skip eager-build
                    // there to match the caller's own catch-up behavior.
                    {
                        extern int catch_up_mode;
                        extern void advance_sprite_frames(SWFAppContext*);
                        extern DisplayObject* g_exec_eager_built_obj;
                        extern int g_advance_defer_nested;
                        if (!catch_up_mode) {
                            MovieClip* _eb_saved = g_current_context;
                            actionSetCurrentContext(cmc);
                            // Force the FULL nested build: advance_sprite_frames
                            // gates its recurse-into-nested-children step on
                            // !g_advance_defer_nested (tag.c:1185), and this eager
                            // build runs inside the root advance where the flag is
                            // 1. Without clearing it, the constructed sprite's
                            // DIRECT children build but their own nested-sprite
                            // children stay unbuilt — so init()'s `this._width`
                            // (e.g. FRadioButton reading boundingBox_mc, a nested
                            // sprite containing a shape) reads short. Clear locally
                            // and restore.
                            int _eb_saved_defer = g_advance_defer_nested;
                            g_advance_defer_nested = 0;
                            advance_sprite_frames(app_context);
                            g_advance_defer_nested = _eb_saved_defer;
                            actionSetCurrentContext(_eb_saved);
                            extern DisplayObject* display_list;
                            extern size_t max_depth;
                            extern size_t display_list_capacity;
                            obj->sprite_display_list = display_list;
                            obj->sprite_max_depth = max_depth;
                            obj->sprite_dl_capacity = display_list_capacity;
                            g_exec_eager_built_obj = obj;
                        }
                    }
                    // Run the sprite's INITIALIZE (0x200) clip events before the
                    // registerClass constructor, matching AVM1's
                    // Initialize → Construct order. The IDE emits component
                    // parameters (e.g. the radio's `label = " Easy (40 mines)"`)
                    // as an Initialize clip handler; the constructor's init()
                    // reads `this.label` and pushes it to the label field. In
                    // browser-WASM only CLIP_EVENT_LOAD is dispatched
                    // (tag_stubs.c pending-load queue) — Initialize never ran, so
                    // `this.label` was empty and the radios rendered blank. Run
                    // it here in the clip's own context so its SetVariable lands
                    // on this MC. Once-only (gated by constructor_invoked above).
                    {
                        MovieClip* _init_saved_ctx = g_current_context;
                        actionSetCurrentContext(cmc);
                        for (size_t _ca = 0; _ca < obj->clip_action_count; _ca++) {
                            if ((obj->clip_actions[_ca].event_flags & CLIP_EVENT_INITIALIZE)
                                && obj->clip_actions[_ca].action != NULL)
                                obj->clip_actions[_ca].action(app_context);
                        }
                        for (size_t _ca = 0; _ca < obj->accumulated_clip_action_count; _ca++) {
                            if ((obj->accumulated_clip_actions[_ca].event_flags & CLIP_EVENT_INITIALIZE)
                                && obj->accumulated_clip_actions[_ca].action != NULL)
                                obj->accumulated_clip_actions[_ca].action(app_context);
                        }
                        actionSetCurrentContext(_init_saved_ctx);
                    }
                    actionInvokeRegisteredClassConstructor(app_context, _rc_export, cmc);
                }
            }
        }
    }

    if (did_swap) {
        actionSetCurrentContext(saved_ctx);
        actionSetBaseClip(saved_base);
    }
    g_current_sprite_obj = saved;
}

#endif // !OFFSCREEN_RENDER

#endif // USE_WEBGPU && !NO_GRAPHICS
