// Compiled in NO_GRAPHICS AND graphics builds — most
// of the helpers here (display-list inspection, depth queries, sprite
// frame helpers, transform/cxform getters/setters) are graphics-friendly
// and useful for the un-gated AS2 dispatch / focus / textfield code in
// action.c. The few sections that genuinely depend on NO_GRAPHICS-only
// data structures (the global display_list/dictionary backing storage,
// frame-loop catch-up, sprite-init-depth tracking) are wrapped inline.

#include <tag.h>
#include <swf.h>
#include <common.h>
#include <action.h>
#include <object.h>
#include <heap.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <actionmodern/action_queue.h>
#include <actionmodern/sprite_frame_scripts.h>

// ---------------------------------------------------------------------------
// Global display state — defined here in NO_GRAPHICS (swf.c provides in GRAPHICS)
// ---------------------------------------------------------------------------
#include <libswf/generated_data.h>
#ifdef NO_GRAPHICS
Character* dictionary = NULL;
DisplayObject* display_list = NULL;
size_t max_depth = 0;
#endif

// ---------------------------------------------------------------------------
// Access generated data arrays (from draws.c / tagMain.c, linked per-test)
// ---------------------------------------------------------------------------
GEN_EXTERN_TRANSFORM_DATA;
GEN_EXTERN_CXFORM_DATA;
extern int catch_up_mode;
extern size_t display_list_capacity;

// ---------------------------------------------------------------------------
// Provided by ng_shared.c (compiled in all build modes)
// ---------------------------------------------------------------------------
extern int ng_find_button(size_t char_id);
extern int ng_find_video(size_t char_id);
extern int ng_find_textfield(size_t char_id);
extern int ng_find_char_path(size_t char_id, size_t* out_offset, size_t* out_size);
extern int ng_find_morph_path(size_t char_id, size_t* out_offset, size_t* out_size);
extern int ng_uses_nonzero_winding(size_t char_id);
extern unsigned int ng_get_auto_instance_counter(void);
extern unsigned int ng_increment_auto_instance_counter(void);
extern void ng_registerSoundMetadata(u16 sound_id, u8 rate, u32 sample_count);
extern const char* ng_lookupExportName(size_t char_id);

// ---------------------------------------------------------------------------
// Clone depth table — tracks which variable name occupies each SWF depth for
// script-created clones (CloneSprite / duplicateMovieClip). When a new clone
// takes an occupied SWF depth, the old variable is set to undefined so that
// GetVariable(old_name) returns undefined after the replacement.
// ---------------------------------------------------------------------------
#define MAX_CLONE_DEPTH_ENTRIES 128
typedef struct { int swf_depth; char name[64]; } CloneDepthEntry;
static CloneDepthEntry g_clone_depth_table[MAX_CLONE_DEPTH_ENTRIES];
static size_t g_clone_depth_count = 0;

extern MovieClip root_movieclip;

// Evict any clone registered at swf_depth: kill the old MC and remove its
// instance-name binding from root's dynamic_props so GetVariable(old_name)
// falls through to "not found" (undefined) after the replacement. The global
// var_map is deliberately NOT touched: with instance names no longer
// auto-registered there, any var_map entry holding the old clone is a genuine
// user variable, which in Flash becomes a dangling soft reference (re-resolved
// by original_target on use) — not a destroyed variable. Previously this wrote
// a typed UNDEFINED into var_map, which both destroyed user vars and shadowed
// the fresh clone's name resolution (avm1/attach_movie re-attach at the same
// depth read `clip` as undefined).
// `keep` is the freshly-created MC about to be re-registered at this depth; it
// must NOT be matched/nuked by the name walk. After game-over → restart in
// SWFs that re-clone the same names at the same SWF depths (e.g. Snake's
// duplicateMovieClip("Snake", "<head>", 16384+head) loop), the stale table
// entry from the prior session would otherwise find the fresh MC by name and
// stamp depth=INT_MIN on it, hiding the clone forever — even though the OLD
// MC of the same name was long gone (NULL'd in child_mc_cache by an earlier
// actionRemoveSprite or alive but no longer needing eviction).
static void clone_depth_evict(SWFAppContext* app_context, int swf_depth, MovieClip* keep)
{
	for (size_t i = 0; i < g_clone_depth_count; i++)
	{
		if (g_clone_depth_table[i].swf_depth == swf_depth)
		{
			// Mark old MC as removed (depth = INT_MIN) and clear from child_mc_cache
			const char* old_name = g_clone_depth_table[i].name;
			size_t name_len = strlen(old_name);
			MovieClip* evicted = NULL;
			{
				extern MovieClip* child_mc_cache[];
				extern int child_mc_count;
				for (int ci = 0; ci < child_mc_count; ci++) {
					if (child_mc_cache[ci] != NULL &&
					    child_mc_cache[ci] != keep &&
					    strcmp(child_mc_cache[ci]->name, old_name) == 0) {
						evicted = child_mc_cache[ci];
						evicted->avm1_removed = 1;
						evicted->depth = INT_MIN;
						child_mc_cache[ci] = NULL;
						break;
					}
				}
			}
			// Drop the instance-name binding — only when it references the
			// evicted MC (or a dead leftover), never a user property.
			if (root_movieclip.dynamic_props != NULL)
			{
				ActionVar* dp = getProperty((ASObject*)root_movieclip.dynamic_props,
				                            old_name, (u32)name_len);
				if (dp != NULL && dp->type == ACTION_STACK_VALUE_MOVIECLIP)
				{
					MovieClip* dp_mc = (MovieClip*)(uintptr_t)dp->data.numeric_value;
					if (dp_mc != NULL && dp_mc != keep &&
					    (dp_mc == evicted || dp_mc->depth == INT_MIN))
					{
						deleteProperty(app_context, (ASObject*)root_movieclip.dynamic_props,
						               old_name, (u32)name_len);
					}
				}
			}
			// Remove entry by swapping with last
			g_clone_depth_table[i] = g_clone_depth_table[--g_clone_depth_count];
			return;
		}
	}
}

// Register a clone variable name at a SWF depth (evicts old entry first).
// `keep` is the freshly-created MC being registered; eviction must not match
// it by name even if a stale entry from a prior session shares the name.
static void clone_depth_register(SWFAppContext* app_context, int swf_depth, const char* name, MovieClip* keep)
{
	clone_depth_evict(app_context, swf_depth, keep);
	if (g_clone_depth_count < MAX_CLONE_DEPTH_ENTRIES)
	{
		g_clone_depth_table[g_clone_depth_count].swf_depth = swf_depth;
		strncpy(g_clone_depth_table[g_clone_depth_count].name, name, 63);
		g_clone_depth_table[g_clone_depth_count].name[63] = '\0';
		g_clone_depth_count++;
	}
}

// Look up the registered SWF depth for a clone by name.
// Returns INT_MIN if the name isn't registered. Used by actionRewindCleanup
// to decide whether a display-list-less clone survives a backward goto —
// Ruffle's survives_rewind rule preserves objects whose SWF depth lives in
// the dynamic range (>= AVM_DEPTH_BIAS = 16384). We key off the registered
// SWF depth rather than `ch->depth` because the MC's stored depth can be a
// mix of AS-depth and SWF-depth conventions (see comments in
// ng_cloneSprite about the Ming-vs-Shumway bias heuristic).
int ng_clone_get_swf_depth(const char* name)
{
	if (name == NULL) return INT_MIN;
	for (size_t i = 0; i < g_clone_depth_count; i++) {
		if (strcmp(g_clone_depth_table[i].name, name) == 0) {
			return g_clone_depth_table[i].swf_depth;
		}
	}
	return INT_MIN;
}

// Rewrite a clone's registered SWF depth (no eviction). Used by swapDepths
// to keep the clone-depth table in sync when a clone is moved — without
// this, a duplicateMovieClip clone that swapDepths'd into the static range
// would still appear "dynamic" to survives_rewind and vice-versa.
void ng_clone_update_swf_depth(const char* name, int new_swf_depth)
{
	if (name == NULL) return;
	for (size_t i = 0; i < g_clone_depth_count; i++) {
		if (strcmp(g_clone_depth_table[i].name, name) == 0) {
			g_clone_depth_table[i].swf_depth = new_swf_depth;
			return;
		}
	}
}

// ng_sync_root_display_obj, ng_get_root_display_obj — now in ng_shared.c

// ---------------------------------------------------------------------------
// Pending attach init queue — deferred frame-0 script execution for attachMovie
// ---------------------------------------------------------------------------
// Phase 3c of ACTION_QUEUE_PLAN: backing storage is the unified ActionQueue
// (AQ_KIND_ATTACH_INIT entries). Coalesce-by-swf_depth at enqueue time is
// preserved via actionQueueFindUserByKind — if attachMovie fires at a depth
// that already has a queued entry, its payload is mutated in place so the
// last-attached version wins (the prior init never runs). The outer
// while-loop dispatch the old implementation used is handled for free by
// the unified drain's pop-until-empty behavior: re-entrant attachMovie calls
// push new entries that the outer drain picks up naturally. The old
// MAX_PENDING_ATTACH_INITS=64 silent-overflow limit is gone.
extern MovieClip* actionFindOrCreateMovieClip(SWFAppContext* app_context, const char* name, MovieClip* parent);
extern void setVariableByName(const char* var_name, ActionVar* value);
extern void actionSetCurrentContext(MovieClip* mc);
extern MovieClip root_movieclip;
extern int g_settarget_explicit_root;
extern DisplayObject* g_current_sprite_obj;
extern MovieClip* g_current_context;

typedef struct {
	char instance_name[256];
	char export_name[128];  // For registered class constructor invocation
	frame_func func;
	int swf_depth;
	MovieClip* parent;      // Actual parent of the attached clip (NULL ⇒ _root).
	                        // Without this, deferred init re-resolved the clip
	                        // against _root and minted a ghost root-child for any
	                        // non-root attachMovie (see aq_dispatch_pending_attach_init).
} PendingAttachInit;

static void aq_dispatch_pending_attach_init(SWFAppContext* app_context, void* user)
{
	PendingAttachInit* pai = (PendingAttachInit*) user;
	if (pai == NULL) return;

	// Set context and base clip to the attached clip for correct variable resolution
	MovieClip* saved_ctx = g_current_context;
	extern void actionSetBaseClip(MovieClip* mc);
	extern MovieClip* actionGetBaseClip(void);
	MovieClip* saved_base = actionGetBaseClip();
	MovieClip* mc = actionFindOrCreateMovieClip(
		app_context, pai->instance_name,
		pai->parent ? pai->parent : &root_movieclip);
	if (mc) { actionSetCurrentContext(mc); actionSetBaseClip(mc); }

	// Save display list state and switch to the MC's sprite display list
	DisplayObject* saved_dl = display_list;
	size_t saved_max = max_depth;
	size_t saved_cap = display_list_capacity;
	DisplayObject* saved_sprite_obj = g_current_sprite_obj;
	int saved_settarget = g_settarget_explicit_root;
	extern int g_settarget_invalid;
	extern int g_settarget_none;
	int saved_invalid = g_settarget_invalid;
	int saved_none = g_settarget_none;

	// Use the MC's own sprite display list (created during ng_attachMovie)
	if (mc != NULL && mc->display_obj != NULL) {
		DisplayObject* dobj = (DisplayObject*)mc->display_obj;
		display_list = dobj->sprite_display_list;
		max_depth = dobj->sprite_max_depth;
		display_list_capacity = dobj->sprite_dl_capacity;
	} else {
		// Fallback: create a temp display list
		size_t tmp_cap = 64;
		display_list = calloc(tmp_cap, sizeof(DisplayObject));
		max_depth = 0;
		display_list_capacity = tmp_cap;
	}
	g_settarget_explicit_root = 0;
	g_settarget_invalid = 0;
	g_settarget_none = 0;
	// PROGRESS #15: route a bare stop()/play()/gotoAndStop in the attached clip's
	// OWN frame-1 script to the clip's display_obj. These compile to
	// actionStop/actionPlay/actionGotoFrame which act on g_current_sprite_obj via
	// ng_stopCurrentSprite/ng_playCurrentSprite — with this NULL the bare stop()
	// fell through and never froze the clip, so the auto-advance pump over-advanced
	// it (e.g. Pacman's ghosts _root.Demo.G.0, frozen at _cf=1 in Ruffle via a bare
	// frame-1 stop()). The method-form `this.stop()` (gate's c, N's menuMC) is
	// handled separately by the step-1 method-form fix. Scoped to the two test-pump
	// modes that auto-advance attached clips; browser-WASM keeps the established
	// NULL path (its attached clips don't auto-advance, and its attach timing is
	// not covered by the CI suites). NULL fallback for clips with no display_obj.
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	g_current_sprite_obj = (mc != NULL && mc->display_obj != NULL)
	                       ? (DisplayObject*)mc->display_obj : NULL;
#else
	g_current_sprite_obj = NULL;
#endif

	// Run the frame function in script-only mode: placement tags already ran during
	// ng_attachMovie with catch_up_mode=1, so only scripts need to execute now.
	// Without script_only_mode, tagPlaceObject2's loop-back preservation check
	// would clear sprite_needs_init on children (e.g. "box"), preventing Phase 2.
	// Phase 7b: bracket with actionDeferredSpriteInitEnter so the recompiler-
	// emitted sprite DoAction gate's sync-fire path activates
	// (scriptOnly && deferred) and scripts run inline here (with the correct
	// MC context already set above) instead of being silently dropped.
	{
		extern void ng_set_script_only_mode(int mode);
		ng_set_script_only_mode(1);
		actionAttachInitEnter();
		actionDeferredSpriteInitEnter();
		pai->func(app_context);
		actionDeferredSpriteInitLeave();
		actionAttachInitLeave();
		ng_set_script_only_mode(0);
	}

	// Recursively initialize any child sprites placed by the frame function.
	// Without this, children of attachMovie'd sprites (e.g. sprite_2's child
	// sprite_1 instances) never run their frame 0 scripts.
	// Phase 7b fix: bracket with DeferredSpriteInit so the Phase 2 re-run for
	// child sprites (scriptOnly=1) takes the sync-fire branch of the sprite
	// DoAction gate. Without this, the children's DoActions were only firing
	// because ng_attachMovie's outer funcs[0] call queued them into SCRIPT
	// (which reorders them ahead of the parent's PAI — bug in default_names).
	// Now that ng_attachMovie suppresses that queueing via goto-catchup, the
	// children must fire through this path instead.
	// Phase F: also bracket with AttachInit so the recompiler gate routes
	// these to the sync-fire branch (not the goto Phase 2 queue branch).
	{
		extern void process_sprite_needs_init_public(SWFAppContext* app_context, MovieClip* parent_mc);
		actionAttachInitEnter();
		actionDeferredSpriteInitEnter();
		process_sprite_needs_init_public(app_context, mc);
		actionDeferredSpriteInitLeave();
		actionAttachInitLeave();
	}

	// Persist updated display list state back to the MC's display obj
	if (mc != NULL && mc->display_obj != NULL) {
		DisplayObject* dobj = (DisplayObject*)mc->display_obj;
		dobj->sprite_display_list = display_list;
		dobj->sprite_max_depth = max_depth;
		dobj->sprite_dl_capacity = display_list_capacity;
		// Mark eligible for AS2 onEnterFrame dispatch on the init tick
		// (mirrors process_sprite_init_at_depth which sets this for timeline sprites)
		dobj->enterframe_eligible = 1;
	} else {
		free(display_list);
	}

	// Restore
	actionSetCurrentContext(saved_ctx);
	actionSetBaseClip(saved_base);
	g_current_sprite_obj = saved_sprite_obj;
	g_settarget_explicit_root = saved_settarget;
	g_settarget_invalid = saved_invalid;
	g_settarget_none = saved_none;
	display_list = saved_dl;
	max_depth = saved_max;
	display_list_capacity = saved_cap;

	// NOTE: Registered class constructor is now fired synchronously during
	// attachMovie (in action.c), not deferred here. This ensures the constructor
	// runs before attachMovie returns to the caller script.

	free(pai);
}

struct AttachInitMatchCtx { MovieClip* parent; int swf_depth; };
static int attach_init_match_depth(void* user, void* ctx)
{
	PendingAttachInit* pai = (PendingAttachInit*) user;
	struct AttachInitMatchCtx* mc = (struct AttachInitMatchCtx*) ctx;
	// Depth spaces are per-parent for non-root attaches, so dedup must key on
	// both parent and depth — otherwise e.g. three sibling radio buttons each
	// attaching a "fLabel_mc" at the same depth would collapse to one PAI.
	return pai && pai->swf_depth == mc->swf_depth && pai->parent == mc->parent;
}

void ng_fire_pending_attach_inits(SWFAppContext* app_context)
{
	actionDrainActionQueueByKind(app_context, AQ_KIND_ATTACH_INIT);
}

// ---------------------------------------------------------------------------
// Instance-name binding on a parent's dynamic_props (root included).
// Instance names are deliberately NOT registered in the global var_map —
// that store holds only genuine user variables, so a `_name` rename can't
// destroy a same-named user var (see avm1/_investigation/blocked/
// INSTANCE_NAME_VS_VARIABLE_BINDING_PLAN.md). The binding is skipped when an
// existing entry is "authoritative": a pre-existing own property (root
// timeline vars shadow display children, mirroring Ruffle's no-slot model)
// or a LIVE MC whose current name still matches the key. Dead MCs
// (depth == INT_MIN), renamed-away MCs and UNDEFINED leftovers rebind.
// ---------------------------------------------------------------------------
static void ng_register_instance_on_parent_dprops(SWFAppContext* app_context,
                                                  MovieClip* parent,
                                                  const char* name,
                                                  MovieClip* child)
{
	if (parent == NULL || name == NULL || name[0] == '\0') return;
	if (parent->dynamic_props == NULL) {
		parent->dynamic_props = (void*) allocDynamicProps(app_context, 8);
	}
	u32 name_len = (u32)strlen(name);
	ActionVar* existing = getProperty((ASObject*) parent->dynamic_props, name, name_len);
	int blocked = 0;
	if (existing != NULL && existing->type != ACTION_STACK_VALUE_UNDEFINED) {
		blocked = 1;
		if (existing->type == ACTION_STACK_VALUE_MOVIECLIP) {
			MovieClip* old_mc = (MovieClip*)(uintptr_t)existing->data.numeric_value;
			if (old_mc == NULL || old_mc->depth == INT_MIN ||
			    !swf_name_match(old_mc->name, name))
				blocked = 0;
		}
	}
	if (!blocked) {
		ActionVar v = {0};
		v.type = ACTION_STACK_VALUE_MOVIECLIP;
		v.data.numeric_value = (u64)(uintptr_t)child;
		setProperty(app_context, (ASObject*) parent->dynamic_props,
		            name, name_len, &v);
	}
}

// ---------------------------------------------------------------------------
// ng_attachMovie — instantiate an exported library symbol at runtime
// ---------------------------------------------------------------------------

MovieClip* ng_attachMovie(SWFAppContext* app_context, size_t char_id, const char* new_name, int as_depth, MovieClip* parent)
{
	extern size_t display_list_capacity;

	if (char_id >= INITIAL_DICTIONARY_CAPACITY) return NULL;
	int _am_is_button = (dictionary[char_id].type == CHAR_TYPE_BUTTON);
	if (dictionary[char_id].type != CHAR_TYPE_SPRITE && !_am_is_button) return NULL;
	// Flash valid depth range: -16384 to 2130690044
	if (as_depth > 2130690044 || as_depth < -16384) return NULL;

	// Create MC for the attached clip (reset position/scale to defaults for re-attach)
	MovieClip* new_mc = actionFindOrCreateMovieClip(app_context, new_name, parent);
	if (new_mc == NULL) return NULL;
	new_mc->as_created = 1;  /* AS-created: name-resolvable via cache scan (soft refs) */
	new_mc->is_button_mc = _am_is_button ? 1 : 0;
	new_mc->movie_id = parent->movie_id;  // Attached clips belong to same movie as parent

	// Remove any existing clone at this SWF depth BEFORE setting depth/props.
	// clone_depth_evict may set depth=INT_MIN on the old MC at this depth,
	// which could be the same struct if actionFindOrCreateMovieClip reused it.
	// Only use global clone depth system for root-level attachments; non-root
	// children have independent depth spaces and must not collide.
	int swf_depth = as_depth + 16384;
	if (parent == &root_movieclip)
		clone_depth_register(app_context, swf_depth, new_name, new_mc);

	new_mc->depth = as_depth;
	new_mc->x = 0.0f;
	new_mc->y = 0.0f;
	new_mc->xscale = 100.0f;
	new_mc->yscale = 100.0f;
	new_mc->rotation = 0.0f;
	new_mc->alpha = 100.0f;
	new_mc->visible = 1;
	new_mc->width = 0.0f;
	new_mc->height = 0.0f;
	// Don't reset frame counters here — they stay at createMovieClip defaults (1,1,1).
	// attachMovie clips that run frame 0 get their counters set at the end of ng_attachMovie.
#ifdef NO_GRAPHICS
	new_mc->as_set_flags = 0;
#endif

	// Register the attached clip so name resolution finds it. In AVM1,
	// mc.attachMovie(id, name, depth) binds the clip only as a display-list
	// child of `mc` (root included) — never as a global/_root VARIABLE.
	// The binding lives on parent->dynamic_props for GetMember/path parity;
	// child_mc_cache scans cover bare-name reads. Root attaches previously
	// went to var_map, which conflated instance names with user variables
	// (see ng_register_instance_on_parent_dprops above).
	ng_register_instance_on_parent_dprops(app_context, parent, new_name, new_mc);

	// Execute the sprite's frame 0 placement tags (scripts deferred to end of frame)
	frame_func* funcs = dictionary[char_id].sprite_frame_funcs;
	size_t frame_count = dictionary[char_id].sprite_frame_count;
	if (funcs != NULL && frame_count > 0 && funcs[0] != NULL) {
		// Context swap: save current display list and switch to a temporary one
		// for the sprite's children
		DisplayObject* saved_dl    = display_list;
		size_t         saved_max   = max_depth;
		size_t         saved_cap   = display_list_capacity;
		MovieClip*     saved_ctx   = NULL;
		DisplayObject* saved_sprite_obj = g_current_sprite_obj;
		int            saved_settarget = g_settarget_explicit_root;
		extern int g_settarget_invalid;
		extern int g_settarget_none;
		int            saved_invalid2 = g_settarget_invalid;
		int            saved_none2 = g_settarget_none;
		int            saved_catch_up = catch_up_mode;

		// Create a display object for the attached clip to hold its children
		if (new_mc->display_obj == NULL) {
			DisplayObject* dobj = calloc(1, sizeof(DisplayObject));
			dobj->char_id = char_id;
			dobj->attach_standalone = 1;   // see swf.h: walked for clip enterFrame
			dobj->sprite_dl_capacity = 64;
			// Use HCALLOC so grow_ptr/ENSURE_SIZE/FREE can operate on it consistently
			dobj->sprite_display_list = HCALLOC(dobj->sprite_dl_capacity, sizeof(DisplayObject));
			dobj->sprite_max_depth = 0;
			new_mc->display_obj = dobj;
		} else {
			// Re-attach: clear existing children
			DisplayObject* dobj = (DisplayObject*)new_mc->display_obj;
			if (dobj->sprite_display_list) {
#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
				// Browser-WASM: re-attaching a reused parent MC memsets away its
				// child display list, but the cached child MovieClips (auto-named
				// "instanceN" nested placements) keep parent==new_mc, which is
				// still alive — so the dead-parent cascade never reaches them and
				// they orphan in child_mc_cache every re-attach. Metanet "N"'s
				// particle ring buffer re-attaches the same "pfx*" names cyclically,
				// leaking their nested instance children unbounded until the game
				// freezes. Mark all of new_mc's current children dead here; the
				// per-frame cascade reaches grandchildren (their parent is now
				// INT_MIN), and frame 0 below re-materializes the fresh placements.
				extern MovieClip* child_mc_cache[];
				extern int child_mc_count;
				extern void actionInvalidateCachedMovieClipDirect(SWFAppContext*, MovieClip*);
				for (int _rai = 0; _rai < child_mc_count; _rai++) {
					MovieClip* _rac = child_mc_cache[_rai];
					if (_rac != NULL && _rac->depth != INT_MIN && _rac->parent == new_mc)
						actionInvalidateCachedMovieClipDirect(app_context, _rac);
				}
#endif
				memset(dobj->sprite_display_list, 0, dobj->sprite_dl_capacity * sizeof(DisplayObject));
				dobj->sprite_max_depth = 0;
			}
		}

		// Use the MC's own sprite display list for placement tags
		DisplayObject* dobj = (DisplayObject*)new_mc->display_obj;
		display_list = dobj->sprite_display_list;
		max_depth = dobj->sprite_max_depth;
		display_list_capacity = dobj->sprite_dl_capacity;
		g_settarget_explicit_root = 0;
		g_settarget_invalid = 0;
		g_settarget_none = 0;

		// Set the MC context so _name, _x etc resolve correctly
		saved_ctx = g_current_context;
		actionSetCurrentContext(new_mc);
		g_current_sprite_obj = NULL;

		// Run frame 0 with catch_up_mode=1 to skip scripts (only placement tags run).
		// Phase 7b note: we intentionally do NOT bump eager here, because
		// ng_attachMovie's scripts are run later by aq_dispatch_pending_attach_init
		// via its own pai->func call. Bumping eager here would queue scripts
		// now, and then the deferred re-run (with scriptOnly=1) would be a
		// no-op, but the queued scripts would fire at next SHOW_FRAME pre-drain
		// in root context (bound to the attached MC via actionQueueSpriteScript,
		// but too early relative to the attach_init contract). See
		// aq_dispatch_pending_attach_init which bumps eager + deferred for
		// its pai->func call so scripts fire synchronously there.
		//
		// ALSO save/restore g_tag_skip_mode: when attachMovie is called from
		// inside a deferred-goto Phase 2 target-frame replay (tag_skip_mode=1),
		// inheriting tag_skip=1 into funcs[0] would match the sprite gate's
		// g_tag_skip_mode branch and queue the nested sprite's script here,
		// then PAI drain would fire it AGAIN — double-fire. Reset tag_skip=0
		// for the nested init (we want catch_up_mode=1 to fully skip scripts).
		extern int g_tag_skip_mode;
		int _am_saved_tag_skip = g_tag_skip_mode;
		g_tag_skip_mode = 0;
		catch_up_mode = 1;
		// If this character was imported from a child SWF, switch
		// g_active_transform_data to the child's transform_data so that
		// tagPlaceObject2 calls inside funcs[0] cache transforms from the
		// correct table. Without this, the child's frame_0 indexes into
		// the parent's transform_data and produces wrong _width/_height
		// (key tests: from_gnash misc-ming.all/attachImported,
		// attachMovieLoopingTest).
		extern u8* g_char_movie_id;
		extern size_t g_char_movie_id_capacity;
		extern float (*g_active_transform_data)[16];
		extern float (*ng_getMovieTransformData(u8 movie_id))[16];
		float (*_am_saved_active_td)[16] = g_active_transform_data;
		if (g_char_movie_id != NULL && char_id < g_char_movie_id_capacity
		    && g_char_movie_id[char_id] != 0) {
			float (*child_td)[16] = ng_getMovieTransformData(g_char_movie_id[char_id]);
			if (child_td != NULL) g_active_transform_data = child_td;
		}
		// Phase 7b fix: suppress sprite DoAction queueing for nested Phase 1
		// eager inits that run inside funcs[0]. Without this, a child sprite's
		// frame_0 Phase 1 eager (catch_up=1, eager=1, gotoCatchup=0) hits the
		// queue branch of the sprite gate — queueing the child's DoAction into
		// AQ_KIND_SCRIPT ahead of the parent's PAI dispatch. Reusing goto
		// catchup's flag semantically says "placement-only replay: scripts
		// fire later via PAI, not inline". Key test: avm1/default_names.
		extern void actionGotoCatchupEnter(void);
		extern void actionGotoCatchupLeave(void);
		actionGotoCatchupEnter();
		funcs[0](app_context);
		actionGotoCatchupLeave();
		g_active_transform_data = _am_saved_active_td;
		catch_up_mode = saved_catch_up;
		g_tag_skip_mode = _am_saved_tag_skip;

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
		// Build the nested sprite content placed by frame-0 (browser-WASM). funcs[0]
		// above placed this clip's children, but a NESTED sprite child is left as a
		// just-allocated holder with a NULL sprite_display_list. render_display_list
		// recurses into nested sprite entries, so an unbuilt nested sprite draws
		// nothing — e.g. Minesweeper's frb_states_mc circle ring is two nested
		// sprites (cid 14/17) whose 3D-shading layers never rendered, so no radio
		// circle appeared. NO_GRAPHICS/OFFSCREEN build these via
		// process_sprite_needs_init (tagShowFrame); browser-WASM has no such pass.
		// The globals are still swapped to this clip's list and g_current_context is
		// the attached MC, so advance_sprite_frames runs each child's just_allocated
		// frame-0. Done before the content-bounds computation below so nested
		// content is included in _width/_height. (Only builds unbuilt 1-frame-style
		// holders; already-built/playing clips are advanced as usual.)
		{
			extern void advance_sprite_frames(SWFAppContext*);
			extern int g_advance_defer_nested;
			// Force the FULL nested build (all levels deep). advance_sprite_frames
			// gates its recurse-into-nested-children step on !g_advance_defer_nested
			// (tag.c:1185). When attachMovie runs inside the root frame advance —
			// e.g. a registerClass constructor's init() calling attachMovie — that
			// flag is still 1, so the advance below builds only this clip's DIRECT
			// children and leaves their own nested-sprite children as unbuilt
			// holders (NULL sprite_display_list). The bounds engine then can't
			// recurse into them and the clip's _width/_height compute 0 — e.g.
			// Minesweeper's frb_states_mc circle (cid 14/17 built, but their
			// nested-sprite children cid 7/9/11/13/16 left empty → frb_states._width
			// = 0 → FRadioButton.setLabelPlacement puts the label on top of the
			// circle). Clear the flag locally so the recursion fully builds the
			// subtree, then restore.
			int _am_saved_defer = g_advance_defer_nested;
			g_advance_defer_nested = 0;
			advance_sprite_frames(app_context);
			g_advance_defer_nested = _am_saved_defer;
		}
#endif

		// Restore context
		actionSetCurrentContext(saved_ctx);
		g_current_sprite_obj = saved_sprite_obj;
		g_settarget_explicit_root = saved_settarget;
		g_settarget_invalid = saved_invalid2;
		g_settarget_none = saved_none2;

		// Persist the updated display list state on the MC's display obj
		dobj->sprite_display_list = display_list;
		dobj->sprite_max_depth = max_depth;
		dobj->sprite_dl_capacity = display_list_capacity;

		// Compute content bounds from children placed during frame 0.
		// Uses sprite_content_bounds_twips which accounts for child transforms
		// (e.g., a 1x1 twip shape placed with a 20x scale transform → 20x20 twips).
		{
			float bxmin, bxmax, bymin, bymax;
			if (sprite_content_bounds_twips(dobj->sprite_display_list, dobj->sprite_max_depth,
			        &bxmin, &bxmax, &bymin, &bymax)) {
				new_mc->width = (bxmax - bxmin) / 20.0f;
				new_mc->height = (bymax - bymin) / 20.0f;
			}
		}

		// Restore the parent display list
		display_list = saved_dl;
		max_depth = saved_max;
		display_list_capacity = saved_cap;

		// Queue the frame function for deferred script execution
		// (Flash runs attached clip init scripts at end of frame)
		// If an init is already pending for the same SWF depth, mutate it in
		// place (re-attaching at the same depth supersedes the previous init).
		{
			const char* exp_name = ng_lookupExportName(char_id);
			struct AttachInitMatchCtx _aim = { parent, swf_depth };
			PendingAttachInit* pai = (PendingAttachInit*)
				actionQueueFindUserByKind(AQ_KIND_ATTACH_INIT,
				                          attach_init_match_depth, &_aim);
			if (pai == NULL) {
				pai = (PendingAttachInit*) malloc(sizeof(PendingAttachInit));
				if (pai == NULL) goto skip_attach_init_queue;
				pai->swf_depth = swf_depth;
				actionQueueCallbackEx(app_context,
				                      aq_dispatch_pending_attach_init,
				                      (void*)pai,
				                      AQ_PRIORITY_NORMAL, NULL,
				                      /*is_unload=*/0,
				                      AQ_KIND_ATTACH_INIT);
			}
			pai->parent = parent;
			strncpy(pai->instance_name, new_name, sizeof(pai->instance_name) - 1);
			pai->instance_name[sizeof(pai->instance_name) - 1] = '\0';
			pai->func = funcs[0];
			if (exp_name) {
				strncpy(pai->export_name, exp_name, sizeof(pai->export_name) - 1);
				pai->export_name[sizeof(pai->export_name) - 1] = '\0';
			} else {
				pai->export_name[0] = '\0';
			}
		skip_attach_init_queue: ;
		}
	}

	// Register the new child in the parent's sprite display list so that
	// resolveSlashPathToMC can find it (e.g., GetVariable("target2") inside base2).
	// Root attaches deliberately skip this: the global display_list is dense
	// and indexed by depth; growing it to as_depth + 16384 would force the
	// HCALLOC/memcpy/FREE in grow_ptr to invalidate the `mc->display_obj`
	// pointers that every timeline-placed root child stores back into the
	// array (tag.c sets `mc->display_obj = &display_list[depth]`). Until
	// those pointers are made grow-safe (the broader refactor that would
	// finally let Flash's single-tree depth model fit cleanly), root attaches
	// reach the renderer via a separate child_mc_cache walk in tag.c's
	// render path — see render_root_attached_mcs(). resolveSlashPathToMC's
	// name lookup already covers them via the parent dynamic_props binding above.
	//
	// Browser-WASM (USE_WEBGPU without OFFSCREEN_RENDER) ALSO
	// skips this for non-root attaches: there the child reaches the renderer via
	// the child_mc_cache pass (render_attached_child), not the parent's
	// sprite_display_list recursion, and is resolvable via child_mc_cache /
	// parent->dynamic_props — so the registration is rendering-inert. Worse, it
	// is actively harmful: `target_d = swf_depth` (= as_depth + 16384, e.g. 16386
	// for a clip attached at AS depth 2) grows the parent MC's display_obj
	// sprite_display_list to a ~16400-entry buffer and FREEs the old one — which
	// a timeline-placed parent shares with its ROOT display_list registration
	// entry (a distinct DisplayObject struct holding the same buffer pointer),
	// leaving that entry dangling. finalize_pending_removes_recursive / the
	// render recursion then walk the freed buffer → "memory access out of
	// bounds". Surfaced once registered-class constructors began running in
	// browser-WASM (Minesweeper FRadioButton init attaches frb_states_mc /
	// fLabel_mc / frb_hitArea_mc onto each level_* radio). The CI modes
	// (NO_GRAPHICS / OFFSCREEN_RENDER) keep the registration
	// unchanged.
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (parent != &root_movieclip && parent->display_obj != NULL) {
		DisplayObject* pdobj = (DisplayObject*)parent->display_obj;
		if (pdobj->sprite_display_list != NULL) {
			size_t target_d = (size_t)swf_depth;
			// Grow if needed — ng_spriteDLRealloc reallocates on the custom heap
			// AND rebases every aliased pointer into the old buffer (queued
			// register-ctor payloads, child_mc_cache display_obj, base-pointer
			// copies; see swf.h ALIASING RULE). The old raw HCALLOC/memcpy/FREE
			// here was the root cause of the aq_dispatch_register_ctor
			// use-after-free: growing a radio component's list mid-ctor-drain
			// freed the buffer that later queued ctor entries still pointed at.
			if (target_d >= pdobj->sprite_dl_capacity)
				ng_spriteDLRealloc(app_context, &pdobj->sprite_display_list,
				                   &pdobj->sprite_dl_capacity, target_d + 16);
			if (target_d >= pdobj->sprite_dl_capacity)
				goto skip_parent_dl_registration;
			pdobj->sprite_display_list[target_d].char_id = char_id;
			if (pdobj->sprite_display_list[target_d].instance_name)
				free(pdobj->sprite_display_list[target_d].instance_name);
			pdobj->sprite_display_list[target_d].instance_name = strdup(new_name);
			// Copy child's display list pointers if it has a display_obj
			if (new_mc->display_obj != NULL) {
				DisplayObject* cdobj = (DisplayObject*)new_mc->display_obj;
				pdobj->sprite_display_list[target_d].sprite_display_list = cdobj->sprite_display_list;
				pdobj->sprite_display_list[target_d].sprite_max_depth = cdobj->sprite_max_depth;
				pdobj->sprite_display_list[target_d].sprite_dl_capacity = cdobj->sprite_dl_capacity;
			}
			if (target_d > pdobj->sprite_max_depth)
				pdobj->sprite_max_depth = target_d;
		skip_parent_dl_registration: ;
		}
	}
#endif // CI modes only; browser-WASM renders non-root attaches via child_mc_cache

	// AS-visible counters read the DECLARED DefineSprite header field, not
	// the playback frame-function count (`frame_count` above).
	new_mc->totalframes = (int)dictionary[char_id].sprite_declared_frames;
	new_mc->framesloaded = (int)dictionary[char_id].sprite_declared_frames;
	new_mc->currentframe = 1;

	// PROGRESS #15: flag a multi-frame attached clip for playhead auto-advance.
	// The clip starts playing (set inside); the pump (ng_advance_attached_clip_
	// playheads) advances it each tick once promoted, unless its frame-1 script
	// stop()s it (routed to its own display_obj by the step-1 method-form fix).
	{
		extern void ng_record_attached_playable(MovieClip* mc);
		ng_record_attached_playable(new_mc);
	}

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM one-shot particle auto-advance (Metanet "N"). N's
	// ParticleManager attachMovie's every effect (dust, blood, laser spark/charge,
	// ...) as "pfx"+depth into a cyclic ring buffer and relies on each clip's OWN
	// timeline playing forward to a final-frame this.removeMovieClip() to clean
	// itself up. Browser-WASM has no auto-advance for attachMovie'd clips' own
	// timelines (the OFFSCREEN ng_advance_attached_clip_playheads pump is gated
	// out; advance_attached_clip_natural only services EXPLICITLY gotoAndPlay'd
	// clips), so a particle FREEZES on frame 1 forever — leaving the "leftover
	// laser/death lines" the user sees AND a permanent pile of full-opacity
	// particle sprites whose overdraw is N's dominant GPU cost (user A/B-confirmed:
	// skipping particle draws recovers fps far more than skipping the tile grid).
	// Flag genuine auto-play particles (attached during normal play, not catch-up;
	// multi-frame) so advance_attached_clip_natural plays them forward (placement
	// tags) and removes them on wrap. NAME-gated to "pfx*" to stay surgical: it
	// cannot touch GUI/HUD clips (the v1 broad auto-advance exploded timeIndicator
	// `bar`), and it matches exactly the clips the user's SkipParticles A/B hid.
	if (!catch_up_mode && new_mc->display_obj != NULL && new_name != NULL
	    && strncmp(new_name, "pfx", 3) == 0
	    && dictionary[char_id].sprite_frame_count > 1) {
		extern size_t g_tick_count;
		DisplayObject* pdo = (DisplayObject*)new_mc->display_obj;
		pdo->natural_oneshot = 1;
		pdo->sprite_is_playing = 1;
		pdo->sprite_current_frame = 1;        // frame 0 shown at attach; frame 1 next
		pdo->placed_at_tick = g_tick_count;   // show frame 1 this tick, advance next
	}
#endif

	return new_mc;
}

// ---------------------------------------------------------------------------
// Pending onLoad queue for duplicated clips (fired at end of tagShowFrame)
// Duplicate clips live at SWF depths 16384+ — too large for display_list.
// Instead, enqueue their onLoad callbacks here and fire them in tagShowFrame.
// ---------------------------------------------------------------------------
// Phase 3b of ACTION_QUEUE_PLAN: backing storage is the unified ActionQueue
// (AQ_KIND_LOAD entries). The payload is a heap-allocated PendingLoad; the
// aq_dispatch_pending_load callback owns its lifetime and frees it after
// dispatch. The MAX_PENDING_LOADS=64 silent-overflow limit is gone —
// dynamic queue growth replaces it.
typedef struct {
	char instance_name[64];
	ClipAction* clip_actions;
	size_t clip_action_count;
} PendingLoad;

static void aq_dispatch_pending_load(SWFAppContext* app_context, void* user)
{
	PendingLoad* pl = (PendingLoad*) user;
	if (pl == NULL) return;
	MovieClip* saved_ctx = g_current_context;
	MovieClip* mc = actionFindOrCreateMovieClip(
		app_context, pl->instance_name, &root_movieclip);
	if (mc) actionSetCurrentContext(mc);
	for (size_t a = 0; a < pl->clip_action_count; a++)
	{
		if (pl->clip_actions[a].event_flags & CLIP_EVENT_LOAD)
			pl->clip_actions[a].action(app_context);
	}
	actionSetCurrentContext(saved_ctx);
	free(pl);
}

static void ng_queue_pending_load(const char* target_name,
                                  ClipAction* clip_actions,
                                  size_t clip_action_count)
{
	PendingLoad* pl = (PendingLoad*) malloc(sizeof(PendingLoad));
	if (pl == NULL) return;
	strncpy(pl->instance_name, target_name, sizeof(pl->instance_name) - 1);
	pl->instance_name[sizeof(pl->instance_name) - 1] = '\0';
	pl->clip_actions = clip_actions;
	pl->clip_action_count = clip_action_count;
	actionQueueCallbackEx(NULL, aq_dispatch_pending_load, (void*)pl,
	                      AQ_PRIORITY_NORMAL, NULL, /*is_unload=*/0,
	                      AQ_KIND_LOAD);
}

void ng_fire_pending_loads(SWFAppContext* app_context)
{
	actionDrainActionQueueByKind(app_context, AQ_KIND_LOAD);
}

// ---------------------------------------------------------------------------
// Helper: initialize cx_* from cxform_data (percentage format: 100.0 = normal)
// Non-static: tag.c's init_cx_fields browser arm also populates entries from
// it (browser-WASM skips ng_on_place_object2 on most placement paths).
// ---------------------------------------------------------------------------
void ng_init_cxform_from_data(DisplayObject* obj, u32 cxform_id)
{
	// With child movies linked, cxform_id indexes the COMBINED table and the
	// generated `cxform_data` symbol is only the root's prefix of it (multi-SWF
	// render slice). This is the one reader with no app_context to hand.
	const float* cxd = ng_combinedCxformData();
	const float* cx = (cxd != NULL) ? cxd + (size_t)cxform_id * 20
	                                : &cxform_data[cxform_id * 20];
	obj->cx_ra = (double)(int16_t)roundf(cx[0]  * 256.0f) * 100.0 / 256.0;
	obj->cx_ga = (double)(int16_t)roundf(cx[5]  * 256.0f) * 100.0 / 256.0;
	obj->cx_ba = (double)(int16_t)roundf(cx[10] * 256.0f) * 100.0 / 256.0;
	obj->cx_aa = (double)(int16_t)roundf(cx[15] * 256.0f) * 100.0 / 256.0;
	obj->cx_rb = (double)(int16_t)roundf(cx[16] * 255.0f);
	obj->cx_gb = (double)(int16_t)roundf(cx[17] * 255.0f);
	obj->cx_bb = (double)(int16_t)roundf(cx[18] * 255.0f);
	obj->cx_ab = (double)(int16_t)roundf(cx[19] * 255.0f);
}

// ---------------------------------------------------------------------------
// Placement/removal callbacks (called from tag.c after display_list update)
// ---------------------------------------------------------------------------

void ng_on_place_object2(SWFAppContext* app_context, size_t depth, size_t char_id)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return;
	DisplayObject* obj = &display_list[depth];

	// Initialize cx_* from cxform_data if there's a cxform applied
	if (obj->has_cxform && obj->cxform_id != 0)
		ng_init_cxform_from_data(obj, obj->cxform_id);

	// Determine character type
	int is_sprite = (char_id < INITIAL_DICTIONARY_CAPACITY && dictionary[char_id].type == CHAR_TYPE_SPRITE);
	int is_button = ng_find_button(char_id);
	int tf_idx    = ng_find_textfield(char_id);
	int is_tf     = (tf_idx >= 0);
	int is_video  = ng_find_video(char_id);

	// Apply pending instance name (from tagSetInstanceName before tagPlaceObject2) to ANY
	// display object type. Must consume for non-scriptable types too (shapes, morph shapes)
	// to avoid leaking the name to the next scriptable object.
	// Then auto-assign "instanceN" for scriptable types (sprites/buttons/textfields) without names.
	{
		extern const char* g_pending_instance_name;
		if (g_pending_instance_name != NULL)
		{
			obj->instance_name = (char*)g_pending_instance_name;
			obj->instance_name_owned = 0;
			g_pending_instance_name = NULL;
		}
		else if ((is_sprite || is_button || is_tf) && obj->instance_name == NULL)
		{
			char auto_name[32];
			snprintf(auto_name, sizeof(auto_name), "instance%u", ng_increment_auto_instance_counter());
			obj->instance_name = strdup(auto_name);
			obj->instance_name_owned = 1;
		}
	}

	// Initialize textfield variable binding (placement-time scope seeding).
	// Preserved across Phase B for tests / paths that read the bound variable
	// before the wrapper MC's bind logic runs (e.g. scope-chain lookups that
	// don't go through findOrCreateMovieClip).
	if (is_tf && tf_idx >= 0)
	{
		const char* var_name  = ng_getTextFieldVariableName(tf_idx);
		const char* init_text = ng_getTextFieldInitialTextByIdx(tf_idx);
		if (var_name[0] != '\0')
		{
			// The legacy seeding for a SIMPLE (unqualified) variable name writes
			// it into the global var_map, which exposes it as an enumerable
			// _root.<var>. That is only correct for a textfield whose parent IS
			// _root; for a field nested in a child sprite the variable binds to
			// the parent MC's scope (handled by the Phase B bind below, which
			// resolves the parent from g_current_context), and seeding the global
			// var_map wrongly leaks _root.<var> (Ruffle binds parent-scope only;
			// e.g. glaiel krazykar2's preloader "perc" field inside instance1).
			// Path-variable names ("_root.mc.var") name their own container and
			// are parent-independent, so always seed those.
			extern MovieClip root_movieclip;
			int is_path_var = (strchr(var_name, '.') != NULL);
			MovieClip* tf_parent = g_current_context ? g_current_context : &root_movieclip;
			if (is_path_var || tf_parent == &root_movieclip)
				actionInitTextFieldVariable(app_context, var_name, init_text);
		}
	}

	// Phase B: eagerly create the AVM1 wrapper for placed text fields, so
	// every placed TF behaves like a real DisplayObject from placement —
	// matches Flash, where DefineEditText DisplayObjects exist immediately
	// at PlaceObject2 time rather than lazily on first AS access.
	// findOrCreateMovieClip's init path calls actionTryBindTextFieldVariable
	// (set_initial_value=1) as part of TF property pre-population. If the
	// path can't yet be resolved (parent path not in scope), push to the
	// unbound retry queue.
	if (is_tf && tf_idx >= 0 && obj->instance_name != NULL)
	{
		extern MovieClip root_movieclip;
		MovieClip* parent_mc = g_current_context ? g_current_context : &root_movieclip;
		MovieClip* tf_mc = actionFindOrCreateMovieClip(app_context, obj->instance_name, parent_mc);
		const char* var_name = ng_getTextFieldVariableName(tf_idx);
		if (tf_mc != NULL && var_name[0] != '\0') {
			if (!actionTryBindTextFieldVariable(app_context, tf_mc, /*set_initial_value=*/1)) {
				// Path-variable container not yet reachable — queue for retry.
				actionUnboundTextFieldsPush(tf_mc);
			}
		}
	}

	// Allocate sprite display list and mark for initialization.
	// The actual eager init (frame 0 with catch_up_mode=1) happens in tagPlaceObject2
	// in tag.c, which handles clip event ordering (INITIALIZE before, CONSTRUCT after).
	// Auto-instance names are assigned depth-first because tag.c's eager init runs
	// the child sprite's frame function immediately, triggering recursive ng_on_place_object2
	// calls that increment the counter before the parent continues placing siblings.
	if (is_sprite)
	{
		Character* ch = &dictionary[char_id];
		if (ch->sprite_frame_funcs != NULL && ch->sprite_frame_funcs[0] != NULL)
		{
			if (obj->sprite_display_list == NULL)
			{
				obj->sprite_dl_capacity = INITIAL_SPRITE_DL_CAPACITY;
				obj->sprite_display_list = HCALLOC(obj->sprite_dl_capacity, sizeof(DisplayObject));
				obj->sprite_max_depth = 0;
			}
			obj->sprite_needs_init = 1;
		}
	}

	// Buttons also get a display list for their child sprites (up-state children).
	// tagShowFrame will initialize the button's up-state by running button_state_funcs[0].
	if (is_button)
	{
		if (obj->sprite_display_list == NULL)
		{
			obj->sprite_dl_capacity = INITIAL_SPRITE_DL_CAPACITY;
			obj->sprite_display_list = HCALLOC(obj->sprite_dl_capacity, sizeof(DisplayObject));
			obj->sprite_max_depth = 0;
		}
		obj->sprite_needs_init = 1;
	}
}

// Recursively check a sprite's children for UNLOAD clip actions.
int has_child_unload_handler(DisplayObject* dl, size_t dl_max)
{
	for (size_t d = 1; d <= dl_max; d++) {
		if (dl[d].char_id == 0) continue;
		// Check this child's clip_actions
		for (size_t ca = 0; ca < dl[d].clip_action_count; ca++) {
			if (dl[d].clip_actions[ca].event_flags & 0x4)
				return 1;
		}
		for (size_t ca = 0; ca < dl[d].accumulated_clip_action_count; ca++) {
			if (dl[d].accumulated_clip_actions[ca].event_flags & 0x4)
				return 1;
		}
		// Recurse into child sprites
		if (dl[d].sprite_display_list != NULL && dl[d].sprite_max_depth > 0) {
			if (has_child_unload_handler(dl[d].sprite_display_list, dl[d].sprite_max_depth))
				return 1;
		}
	}
	return 0;
}

// Compute has_unload for an entry: returns 1 if any of clip_actions /
// accumulated_clip_actions / AS-level onUnload / recursive child unloads is
// present. Used by ng_on_remove_object inline (CloneSprite paths) and by
// tagRemoveObject2's deferred path.
int ng_compute_has_unload(size_t depth)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return 0;
	if (display_list[depth].instance_name == NULL) return 0;
	int has_unload = 0;
	for (size_t ca = 0; ca < display_list[depth].clip_action_count; ca++) {
		if (display_list[depth].clip_actions[ca].event_flags & 0x4) {
			has_unload = 1;
			break;
		}
	}
	if (!has_unload) {
		for (size_t ca = 0; ca < display_list[depth].accumulated_clip_action_count; ca++) {
			if (display_list[depth].accumulated_clip_actions[ca].event_flags & 0x4) {
				has_unload = 1;
				break;
			}
		}
	}
	if (!has_unload) {
		has_unload = actionMCHasOnUnloadProperty(display_list[depth].instance_name, (int)depth);
	}
	if (!has_unload && display_list[depth].sprite_display_list != NULL &&
	    display_list[depth].sprite_max_depth > 0) {
		has_unload = has_child_unload_handler(display_list[depth].sprite_display_list,
		                                      display_list[depth].sprite_max_depth);
	}
	return has_unload;
}

void ng_on_remove_object(SWFAppContext* app_context, size_t depth)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return;
	if (display_list[depth].instance_name != NULL) {
		// Determine has_unload BEFORE firing the AS-level handler — the handler
		// shifts the MC's depth, which would invalidate name+depth lookups in
		// actionMCHasOnUnloadProperty if we did the check after.
		int has_unload = ng_compute_has_unload(depth);
		// Fire AS-set onUnload handler (now queues — fires at next ShowFrame).
		actionFireOnUnload(app_context, display_list[depth].instance_name, (int)depth);
		if (has_unload) {
			actionMarkMCPendingRemoval(app_context, display_list[depth].instance_name, (int)depth);
		} else {
			actionInvalidateCachedMovieClip(app_context, display_list[depth].instance_name, (int)depth);
		}
	}
}

// ---------------------------------------------------------------------------
// Sprite control helpers (use g_current_sprite_obj set by exec_sprite_frame)
// ---------------------------------------------------------------------------

int ng_isInsideSprite(void) { return g_current_sprite_obj != NULL; }

void ng_stopCurrentSprite(void)
{
	if (g_current_sprite_obj != NULL)
		g_current_sprite_obj->sprite_is_playing = 0;
}

void ng_playCurrentSprite(void)
{
	if (g_current_sprite_obj != NULL)
		g_current_sprite_obj->sprite_is_playing = 1;
}

void ng_gotoFrameCurrentSprite(u16 frame)
{
	DisplayObject* obj = g_current_sprite_obj;
	if (obj == NULL || obj->char_id == 0) return;
	Character* ch = &dictionary[obj->char_id];
	if (ch->type != CHAR_TYPE_SPRITE) return;
	if (ch->sprite_frame_count == 0) return;
	// Clamp to last frame
	if (frame >= ch->sprite_frame_count)
		frame = (u16)(ch->sprite_frame_count - 1);
	obj->sprite_manual_next_frame = 1;
	obj->sprite_next_frame = frame;
	obj->sprite_is_playing = 0;
	// Record this self-goto (a sprite's OWN frame script navigating itself) so the
	// post-script-drain pass can apply it WITHIN this tick (#10). Sprite frame
	// scripts are queued and drained AFTER advance_sprite_frames, so the manual
	// flag is set too late for that pass — left to the next tick it produces a
	// one-tick-late goto + a stutter. See ng_apply_pending_sprite_self_gotos.
	// Scoped to the two modes whose pump calls the apply pass (swf_core.c /
	// swf.c-OFFSCREEN_RENDER); browser-WASM keeps the established deferred path so
	// the recorder never accumulates stale entries no apply pass would clear.
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	extern void ng_record_sprite_self_goto(DisplayObject* obj);
	ng_record_sprite_self_goto(obj);
#endif

	// Phase B (GOTO_FIFO_UNIFICATION_INCREMENTAL): record the resolved
	// target-frame script onto the deferred sprite-script queue. Nothing
	// drains it yet — Phase C wires drain into actionDrainOnloadAndScript
	// behind g_unify_sprite_drain. fn may be NULL when no DoAction exists
	// at this (char_id, frame); the entry is still recorded for sequencing.
	{
		SpriteFrameScriptFn fn = actionGetSpriteFrameScript(obj->char_id, frame);
		actionQueuePendingSpriteScript(obj->char_id, frame, fn);
	}
}

size_t ng_getSpriteFrameCount(void)
{
	DisplayObject* obj = g_current_sprite_obj;
	if (obj == NULL || obj->char_id == 0) return 0;
	Character* ch = &dictionary[obj->char_id];
	if (ch->type != CHAR_TYPE_SPRITE) return 0;
	return ch->sprite_frame_count;
}

// Navigate a specific MovieClip's sprite to a given 0-based frame.
// Searches display_list for the entry matching mc->name.
// Returns 1 if sprite found and navigated, 0 if not found.
int ng_gotoFrameByMC(SWFAppContext* app_context, MovieClip* mc, u16 frame, int play)
{
	extern MovieClip root_movieclip;
	if (!mc || mc == &root_movieclip) return 0;
	if (!mc->name || mc->name[0] == '\0') return 0;

	DisplayObject* obj = NULL;
	size_t depth = ng_findDisplayEntryByName(mc->name);
	if (depth == SIZE_MAX)
	{
		// Not in root display list — could be (a) a nested sprite (e.g. mc11
		// inside mc1) addressed by slash-path, or (b) a dynamically created
		// MC (createEmptyMovieClip) with no display entry. For (a) the MC's
		// display_obj points at the DisplayObject in its parent's
		// sprite_display_list; for (b) display_obj is NULL.
		if (mc->display_obj != NULL)
		{
			obj = (DisplayObject*)mc->display_obj;
		}
		else
		{
			// Dynamic MC: just update currentframe and play state.
			u16 clamped = frame;
			if (mc->totalframes > 0 && clamped >= (u16)mc->totalframes)
				clamped = (u16)(mc->totalframes - 1);
			mc->currentframe = (int)clamped + 1;  // 1-indexed
			return 1;
		}
	}
	else
	{
		obj = &display_list[depth];
	}
	if (obj->char_id == 0) return 0;
	Character* ch = &dictionary[obj->char_id];
	if (ch->type != CHAR_TYPE_SPRITE) return 0;

	size_t fc = ch->sprite_frame_count;
	if (fc == 0) return 0;

	// Clamp to last frame
	if (frame >= (u16)fc) frame = (u16)(fc - 1);

	// Execute frames synchronously (like advance_sprite_frames but immediate).
	// After sprite init, sprite_current_frame is bumped to 1 optimistically
	// (ready for advance_sprite_frames), but the target frame's tags may not
	// have been executed yet. When frame == current, execute just that frame's
	// function to ensure its tags are applied.
	{
		// Swap to sprite's display list context
		DisplayObject* saved_dl = display_list;
		size_t saved_max = max_depth;
		size_t saved_cap = display_list_capacity;

		display_list = obj->sprite_display_list;
		max_depth = obj->sprite_max_depth;
		display_list_capacity = obj->sprite_dl_capacity;

		if (display_list == NULL)
		{
			// Allocate sprite display list if needed
			obj->sprite_dl_capacity = 32;
			obj->sprite_display_list = HCALLOC(obj->sprite_dl_capacity, sizeof(DisplayObject));
			display_list = obj->sprite_display_list;
			display_list_capacity = obj->sprite_dl_capacity;
		}

		// Run the frame funcs with g_current_context = this MC so named child
		// placements (e.g. frb_frame_mc) parent to THIS sprite, not the caller's
		// context. exec_sprite_frame sets the context from obj->instance_name, but
		// an attachMovie'd clip's holder DisplayObject has a NULL instance_name
		// (ng_attachMovie never sets it), so the context wouldn't change and the
		// frame's placements would bind to whatever was current — leaking the
		// sprite's named children onto _root (Minesweeper frb_states
		// gotoAndStop("selectedEnabled") leaked _root.frb_frame_mc=undefined). We
		// already hold the exact MC, so set it directly (no name-ambiguity: three
		// radios share the name "frb_states_mc").
		extern MovieClip* g_current_context;
		extern void actionSetCurrentContext(MovieClip*);
		extern MovieClip* actionGetBaseClip(void);
		extern void actionSetBaseClip(MovieClip*);
		MovieClip* _gfbmc_saved_ctx = g_current_context;
		MovieClip* _gfbmc_saved_base = actionGetBaseClip();
		actionSetCurrentContext(mc);
		actionSetBaseClip(mc);

		// Execute sprite frame funcs via exec_sprite_frame so g_current_sprite_obj /
		// g_current_context / base_clip are set correctly. Without this, sprite
		// scripts queued by these frames (e.g. via actionQueueSpriteScript) capture
		// a NULL ctx_sprite_obj, and a subsequent play() inside them no-ops because
		// ng_playCurrentSprite has no current sprite. Required for goto_frame_test
		// (script_4's `play()` after mc_red.gotoAndStop(3) must set mc_red playing
		// so the sprite advances to frame 3 and fires script_5 → "7+" trace).
		extern void exec_sprite_frame(SWFAppContext*, DisplayObject*, frame_func);
		// sprite_current_frame holds the NEXT frame to execute ((shown+1) % fc,
		// see the post-loop assignment below), but the forward/backward/same
		// branch logic needs the currently-DISPLAYED frame. Un-wrap it. Without
		// this the comparison was off by one and, crucially, WRAPPED at fc: a
		// clip stopped on the last frame had sprite_current_frame == 0, so a
		// gotoAndStop back to frame 0 read as "same frame" and skipped the
		// clear+replay — leaving the old frame's shapes on screen. That is the
		// Tetris board-cell erase bug: the `block` sprite colours live at
		// depth 1/2 (frames 1-7) while the "empty" frame 0 only places bang_mc
		// at depth 3 and clears nothing, so erasing a cell (drawBlock(0,pos) ->
		// gotoAndStop(1)) REQUIRES the backward-jump rewind. The off-by-one also
		// made forward colour jumps start at the wrong frame (skipping the
		// depth-1 establishing place). Deriving `current` from the displayed
		// frame fixes both. (mc->currentframe-1 is the same value, un-wrapped,
		// and the caller's no-op guard already trusts it.)
		// Intermediate frames of a goto build display state but must NOT run their
		// DoActions (Flash semantics — only the target frame's script runs). Sprite
		// frame functions queue their script under the recompiler-emitted
		// `!catch_up_mode` gate, so running the in-between frames with catch_up_mode=1
		// suppresses that queuing; the target frame runs with the saved mode so its
		// script queues normally. Without this, the intermediate scripts queue and
		// drain AFTER this goto set sprite_is_playing — e.g. a backward
		// gotoAndPlay re-runs frame 1's stop() (Pacman BGSnd "NewLife" resume never
		// reached frame 87, freezing the game at "Get Ready"), and a forward goto
		// re-runs an in-between self-loop gotoAndPlay (Pacman's frame-5
		// gotoAndPlay(1) bounced the Die animation). Mirrors advance_sprite_frames'
		// manual-nav backward replay, which already forces catch_up_mode=1.
		extern int catch_up_mode;
		int _gfbmc_saved_cm = catch_up_mode;
		size_t current = (fc > 0) ? ((obj->sprite_current_frame + fc - 1) % fc) : 0;
		if (frame > current)
		{
			// Forward jump: execute frames current+1..frame
			for (size_t f = current + 1; f <= frame; f++)
			{
				catch_up_mode = (f < frame) ? 1 : _gfbmc_saved_cm;
				if (f < fc && ch->sprite_frame_funcs[f] != NULL)
					exec_sprite_frame(app_context, obj, ch->sprite_frame_funcs[f]);
			}
			catch_up_mode = _gfbmc_saved_cm;
		}
		else if (frame < current)
		{
			// Backward jump: clear display list and re-execute from frame 0
#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
			// Browser-WASM: this clear frees the sprite's child display-list
			// entries but leaves their materialized child MovieClips (auto-named
			// "instanceN" nested placements) live in child_mc_cache with
			// parent==mc (mc is still alive), so the dead-parent cascade never
			// reaches them and they orphan every backward goto. Metanet "N"'s
			// drones re-fire each shot via gotoAndPlay("laserdrone_prefire") —
			// a BACKWARD jump from the later postfire frame — and (since the
			// d701257e4 forward-advance now actually plays prefire/fire forward,
			// placing nested effect clips) those instanceN children accumulate
			// unbounded here, ratcheting child_mc_count + every per-frame O(N)
			// scan until the title demo collapses to <1fps. Mark all of mc's
			// current children dead before the clear; frame 0..target below
			// re-materializes the fresh placements, and swf.c's dead-slot reclaim
			// NULLs the old slots. Mirrors the ng_attachMovie re-attach fix above
			// and advance_sprite_frames' loop-back invalidation.
			{
				extern MovieClip* child_mc_cache[];
				extern int child_mc_count;
				extern void actionInvalidateCachedMovieClipDirect(SWFAppContext*, MovieClip*);
				for (int _bji = 0; _bji < child_mc_count; _bji++) {
					MovieClip* _bjc = child_mc_cache[_bji];
					if (_bjc != NULL && _bjc->depth != INT_MIN && _bjc->parent == mc)
						actionInvalidateCachedMovieClipDirect(app_context, _bjc);
				}
			}
#endif
			// Ruffle run_goto survives_rewind (AVM1): only depth < AVM_DEPTH_BIAS
			// (16384) is a removal candidate — dynamic-range children (attachMovie /
			// duplicateMovieClip, swf_depth >= 16384) survive an explicit backward
			// goto with their identity intact, same rule the natural loop-back
			// (advance_sprite_frames) and the root timeline (ng_display_clear_after)
			// already apply. Freeing them here was also a UAF: the CI-mode
			// attachMovie registration entry at [swf_depth] holds a COPY of the
			// attached clip's own DL base, so this clear freed the child's list
			// while the clip's standalone display_obj kept the pointer — N's
			// drone gotoAndPlay("laserdrone_prefire") then read it via
			// actionGetMember's child-name walk.
			size_t bj_surviving_max = 0;
			for (size_t j = 1; j <= max_depth; ++j)
			{
				if (j >= 16384)
				{
					if (display_list[j].char_id != 0) bj_surviving_max = j;
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
			max_depth = bj_surviving_max;

			for (size_t f = 0; f <= frame; f++)
			{
				catch_up_mode = (f < frame) ? 1 : _gfbmc_saved_cm;
				if (f < fc && ch->sprite_frame_funcs[f] != NULL)
					exec_sprite_frame(app_context, obj, ch->sprite_frame_funcs[f]);
			}
			catch_up_mode = _gfbmc_saved_cm;
		}
		else
		{
			// Same frame: execute this frame's tags. Handles the case where
			// sprite_current_frame was bumped after init but the target frame's
			// tags were never actually executed (sprite was stopped before
			// advance_sprite_frames could run them).
			// Increment place_gen so placement tags don't conflict with
			// entries placed during the previous frame's execution.
			extern size_t g_place_gen;
			g_place_gen++;
			if (frame < fc && ch->sprite_frame_funcs[frame] != NULL)
				exec_sprite_frame(app_context, obj, ch->sprite_frame_funcs[frame]);
		}

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
		// Finalize deferred removes from the synchronous replay. In browser-WASM,
		// tagRemoveObject2 only marks pending_remove=1 (it does
		// NOT clear char_id) and relies on tagShowFrame's later pending-remove walk
		// to finalize. This replay has no such pass, so a frame's RemoveObject2 left
		// the old entry live: e.g. the Tetris `block` sprite swaps its colour shape
		// between depth 1 and depth 2 (frames 3/5/6/7 remove one depth and place the
		// other), so without finalizing, a goto landed with BOTH colour depths
		// occupied and the higher depth rendered on top — the wrong-colour squares
		// (purple on the orange I-piece). A re-place at the same depth already
		// consumed its own pending_remove (tagPlaceObject2's consume path), so only
		// genuinely-removed depths remain flagged here. Clear them to reflect the
		// target frame's true state. (NO_GRAPHICS/OFFSCREEN clear inline in
		// tagRemoveObject2, so they never hit this.)
		for (size_t j = 1; j <= max_depth; ++j)
		{
			if (display_list[j].pending_remove)
			{
				if (display_list[j].sprite_display_list != NULL)
				{
					ng_freeSpriteDL(app_context, display_list[j].sprite_display_list,
				                display_list[j].sprite_dl_capacity);
					display_list[j].sprite_display_list = NULL;
				}
				display_list[j].char_id = 0;
				display_list[j].pending_remove = 0;
			}
		}
#endif

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
		// Build the nested sprites the replayed target frame just placed (run
		// their frame-0). The frame replay above places child sprites but does
		// NOT build their content — e.g. Minesweeper's frb_states
		// "selectedEnabled" frame places cid 28 (the radio dot), itself a sprite
		// wrapping the dot shape (cid 27). Left unbuilt, the dot holder renders
		// empty (the selected radio showed a hollow ring). g_advance_defer_nested
		// is cleared so the FULL subtree builds (cont.40f deep-build) and
		// g_build_only is set so we BUILD just-allocated children but do NOT
		// advance pre-existing playing siblings — without that, a clip whose base
		// frame holds a playing multi-frame effect clip (Tetris's bang_mc, placed
		// at the block's frame 0) would have it stepped on every board-cell
		// gotoAndStop, painting stray explosion frames over the piece colours.
		// g_current_context is still this clip, so children parent correctly.
		{
			extern void advance_sprite_frames(SWFAppContext*);
			extern int g_advance_defer_nested;
			extern int g_build_only;
			int _gfbmc_saved_defer = g_advance_defer_nested;
			int _gfbmc_saved_bo = g_build_only;
			g_advance_defer_nested = 0;  // recurse the full subtree (cont.40f)
			g_build_only = 1;            // build unbuilt children, don't advance siblings
			advance_sprite_frames(app_context);
			g_build_only = _gfbmc_saved_bo;
			g_advance_defer_nested = _gfbmc_saved_defer;
		}
#endif

		actionSetCurrentContext(_gfbmc_saved_ctx);
		actionSetBaseClip(_gfbmc_saved_base);

		obj->sprite_display_list = display_list;
		obj->sprite_max_depth = max_depth;
		obj->sprite_dl_capacity = display_list_capacity;

		display_list = saved_dl;
		max_depth = saved_max;
		display_list_capacity = saved_cap;
	}

	// sprite_current_frame is the NEXT frame to execute (matches the post-init
	// convention at process_sprite_init_at_depth and the natural-advance loop's
	// `(frame + 1) % fc` increment). After executing the target frame inline,
	// the next play-driven advance should start at target+1. Without this, a
	// subsequent play() would re-execute the already-shown target frame and
	// stall progress for one tick — see goto_frame_test, where mc_red was at
	// the last frame after gotoAndStop and the missing "+1" caused the
	// natural-advance to re-fire sprite_4_frame_2 (queueing script_4 a second
	// time, asOrder += "3+") before reaching sprite_4_frame_3 (script_5,
	// "7+") only one tick too late.
	obj->sprite_current_frame = (fc > 0) ? ((frame + 1) % fc) : 0;
	obj->sprite_manual_next_frame = 0;
	obj->sprite_is_playing = play ? 1 : 0;
	// Browser-WASM: mark an explicit gotoAndPlay so advance_attached_clip_natural
	// will play this STANDALONE attached clip forward (coins' "COLLECTED" disappear,
	// drones' "prefire"). gotoAndStop (play=0) clears it. Harmless on timeline-placed
	// clips — the pump only processes standalone attached clips (display_obj outside
	// the global display_list), which advance_sprite_frames doesn't reach.
	obj->goto_play_active = play ? 1 : 0;
	mc->currentframe = (int)frame + 1;  // 1-indexed

	// Propagate the updated display list to the parent's registration entry.
	// An attachMovie'd clip's mc->display_obj (== obj) is a SEPARATE holder from
	// the entry render_display_list iterates in the parent's sprite_display_list
	// (ng_attachMovie copies the holder's list pointer + sprite_max_depth into
	// that entry at attach time). The goto above advanced the holder to the
	// target frame's content — e.g. Minesweeper's frb_states "selectedEnabled"
	// frame places the filled radio dot (chid 28) at a NEW depth (11) beyond the
	// attach-time max_depth — but the registration entry's stale sprite_max_depth
	// would clip the iteration short and the dot would never render. Re-copy the
	// live fields. (No-op for timeline-placed clips whose mc->display_obj already
	// IS the iterated entry — they aren't found in the parent's sprite list.)
	if (mc->parent != NULL && mc->parent->display_obj != NULL && mc->name[0] != '\0') {
		DisplayObject* pdobj = (DisplayObject*)mc->parent->display_obj;
		if (pdobj->sprite_display_list != NULL) {
			for (size_t d = 1; d <= pdobj->sprite_max_depth; d++) {
				DisplayObject* e = &pdobj->sprite_display_list[d];
				if (e != obj && e->instance_name != NULL &&
				    swf_name_match(e->instance_name, mc->name)) {
					e->sprite_display_list = obj->sprite_display_list;
					e->sprite_max_depth = obj->sprite_max_depth;
					e->sprite_dl_capacity = obj->sprite_dl_capacity;
					break;
				}
			}
		}
	}
	return 1;
}

// Get the character ID for a MovieClip's display entry (for sprite label lookup).
// Returns 0 if not found.
size_t ng_getCharIdByMC(MovieClip* mc)
{
	extern MovieClip root_movieclip;
	if (!mc || mc == &root_movieclip) return 0;
	// Attached / dynamic clips (attachMovie / createEmptyMovieClip), and clips
	// nested inside them, are NOT in the global display_list, so the name lookup
	// below can't find them. Read the char_id straight off the clip's own display
	// object (ng_attachMovie sets dobj->char_id). Timeline-placed clips' display_obj
	// points into the global display_list and carries the same char_id, so this is
	// authoritative for both. Without it, gotoAndStop("label") on an attachMovie'd
	// multi-frame clip couldn't resolve its sprite frame labels (Minesweeper's
	// FRadioButton frb_states_mc never reached the "selectedEnabled" frame, so the
	// selected radio showed no filled dot).
	if (mc->display_obj != NULL) {
		size_t cid = ((DisplayObject*)mc->display_obj)->char_id;
		if (cid != 0) return cid;
	}
	if (!mc->name || mc->name[0] == '\0') return 0;
	size_t depth = ng_findDisplayEntryByName(mc->name);
	if (depth == SIZE_MAX) return 0;
	return display_list[depth].char_id;
}

// ---------------------------------------------------------------------------
// Character type queries (root-level display_list lookup)
// ---------------------------------------------------------------------------

int ng_isSpriteAtDepth(size_t depth)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return 0;
	size_t cid = display_list[depth].char_id;
	return (dictionary[cid].type == CHAR_TYPE_SPRITE);
}

size_t ng_getSpriteByteSize(size_t char_id)
{
	if (char_id == 0 || char_id >= INITIAL_DICTIONARY_CAPACITY) return 0;
	if (dictionary[char_id].type != CHAR_TYPE_SPRITE) return 0;
	return dictionary[char_id].sprite_byte_size;
}

int ng_isButtonAtDepth(size_t depth)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return 0;
	return ng_find_button(display_list[depth].char_id);
}

int ng_isTextFieldAtDepth(size_t depth)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return 0;
	return (ng_find_textfield(display_list[depth].char_id) >= 0);
}

int ng_isScriptableAtDepth(size_t depth)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return 0;
	size_t cid = display_list[depth].char_id;
	return (dictionary[cid].type == CHAR_TYPE_SPRITE) ||
	       ng_find_button(cid) ||
	       (ng_find_textfield(cid) >= 0) ||
	       ng_find_video(cid);
}

// ---------------------------------------------------------------------------
// Display entry lookup (returns entry_idx = encoded depth)
// ---------------------------------------------------------------------------

// Find root-level display entry by name.  Returns SWF depth (entry_idx encoding: upper bits=0).
size_t ng_findDisplayEntryByName(const char* name)
{
	size_t result = SIZE_MAX;
	for (size_t d = 0; d <= max_depth; d++)
	{
		if (display_list[d].char_id == 0) continue;
		if (display_list[d].instance_name == NULL) continue;
		if (swf_name_match(display_list[d].instance_name, name))
		{
			if (result == SIZE_MAX || d < result)
				result = d;
		}
	}
	return result;
}

// Find root-level SWF depth by entry name (returns depth, SIZE_MAX if not found).
// Alias used by action.c for timeline targeting.
size_t ng_findDisplayEntryByName_depth(const char* name)
{
	return ng_findDisplayEntryByName(name);
}

// ---------------------------------------------------------------------------
// Root-level depth queries used by action.c for SWF depth targeting
// ---------------------------------------------------------------------------

int ng_findRootChildAtSWFDepth(size_t swf_depth, char* out_name, size_t out_name_size)
{
	if (swf_depth < 1 || swf_depth > max_depth || display_list[swf_depth].char_id == 0)
		return 0;

	size_t cid = display_list[swf_depth].char_id;

	if (dictionary[cid].type == CHAR_TYPE_SPRITE)
	{
		if (out_name && out_name_size > 0)
		{
			const char* n = display_list[swf_depth].instance_name;
			strncpy(out_name, n ? n : "", out_name_size - 1);
			out_name[out_name_size - 1] = '\0';
		}
		return 2;
	}
	else if (ng_find_textfield(cid) >= 0)
	{
		if (out_name && out_name_size > 0)
		{
			const char* n = display_list[swf_depth].instance_name;
			strncpy(out_name, n ? n : "", out_name_size - 1);
			out_name[out_name_size - 1] = '\0';
		}
		return 3;
	}
	else
	{
		if (out_name && out_name_size > 0) out_name[0] = '\0';
		return 1;
	}
}

// Search for a named child within a named parent's display list.
// Returns child's SWF depth within the parent, or SIZE_MAX if not found.
size_t ng_findChildEntryDepth(const char* parent_name, const char* child_name)
{
	// Find parent at root level
	size_t parent_depth = ng_findDisplayEntryByName(parent_name);
	if (parent_depth == SIZE_MAX) return SIZE_MAX;

	DisplayObject* parent_obj = &display_list[parent_depth];
	if (parent_obj->sprite_display_list == NULL) return SIZE_MAX;

	for (size_t d = 1; d <= parent_obj->sprite_max_depth; d++)
	{
		DisplayObject* child = &parent_obj->sprite_display_list[d];
		if (child->char_id == 0) continue;
		if (child->instance_name != NULL && swf_name_match(child->instance_name, child_name))
			return d;
	}
	return SIZE_MAX;
}

// ---------------------------------------------------------------------------
// Transform queries (root-level)
// ---------------------------------------------------------------------------

// A display entry's transform_id indexes the transform table of the movie whose
// TAG placed it — the main movie's `transform_data` for everything the main SWF
// placed, but a loaded child's own array for characters the child placed. Those
// arrays are unrelated allocations of unrelated sizes (a parent with no timeline
// content emits `float transform_data[1][16]`), so indexing the main one with a
// child's id reads past the end and returns a different garbage float each run.
// Multi-SWF render slice: this must be ng_entryTransformData, not
// ng_entryChildTransformData + a `transform_data` fallback. Once child movies
// are linked, ng_cache_transform re-bases the entry's id onto the COMBINED
// table and records that table here, and the generated `transform_data` symbol
// is only the root's prefix of it -- so the "child?" question and the "which
// table?" question stopped having the same answer.
static inline float (*ENTRY_TRANSFORM_DATA(const DisplayObject* obj))[16] {
	return ng_entryTransformData((const DisplayObject*) obj);
}

int ng_getTransformId(size_t depth, u32* out_id)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return 0;
	*out_id = display_list[depth].transform_id;
	return 1;
}

int ng_getTransformXY(size_t depth, float* out_x, float* out_y)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return 0;
	u32 tid = display_list[depth].transform_id;
	float (*td)[16] = ENTRY_TRANSFORM_DATA(&display_list[depth]);
	*out_x = td[tid][12] / 20.0f;
	*out_y = td[tid][13] / 20.0f;
	return 1;
}

// Get translation (in pixels) from a DisplayObject pointer (for walking parent chains).
void ng_getDisplayObjTranslation(void* dobj_ptr, float* out_tx, float* out_ty)
{
	if (dobj_ptr == NULL) { *out_tx = 0; *out_ty = 0; return; }
	DisplayObject* dobj = (DisplayObject*)dobj_ptr;
	u32 tid = dobj->transform_id;
	float (*td)[16] = ENTRY_TRANSFORM_DATA(dobj);
	*out_tx = td[tid][12] / 20.0f;
	*out_ty = td[tid][13] / 20.0f;
}

int ng_getTransformXY_d(size_t depth, double* out_x, double* out_y)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return 0;
	u32 tid = display_list[depth].transform_id;
	float (*td)[16] = ENTRY_TRANSFORM_DATA(&display_list[depth]);
	if (out_x) *out_x = (double)td[tid][12] / 20.0;
	if (out_y) *out_y = (double)td[tid][13] / 20.0;
	return 1;
}

int ng_getTransformScaleRotation(size_t depth, float* out_xscale, float* out_yscale, float* out_rotation)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return 0;
	u32 tid = display_list[depth].transform_id;
	float (*td)[16] = ENTRY_TRANSFORM_DATA(&display_list[depth]);
	float m00 = td[tid][0];
	float m10 = td[tid][1];
	float m01 = td[tid][4];
	float m11 = td[tid][5];
	if (out_xscale)  *out_xscale  = sqrtf(m00*m00 + m10*m10) * 100.0f;
	if (out_yscale)  *out_yscale  = sqrtf(m01*m01 + m11*m11) * 100.0f;
	if (out_rotation) *out_rotation = atan2f(m10, m00) * 180.0f / 3.14159265358979323846f;
	return 1;
}

int ng_getTransformScaleRotationSkew(size_t depth, float* out_xscale, float* out_yscale, float* out_rotation, float* out_skew)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return 0;
	u32 tid = display_list[depth].transform_id;
	float (*td)[16] = ENTRY_TRANSFORM_DATA(&display_list[depth]);
	float m00 = td[tid][0];
	float m10 = td[tid][1];
	float m01 = td[tid][4];
	float m11 = td[tid][5];
	float rot_x = atan2f(m10, m00);
	float rot_y = atan2f(-m01, m11);
	if (out_xscale)  *out_xscale  = sqrtf(m00*m00 + m10*m10) * 100.0f;
	if (out_yscale)  *out_yscale  = sqrtf(m01*m01 + m11*m11) * 100.0f;
	if (out_rotation) *out_rotation = rot_x * 180.0f / 3.14159265358979323846f;
	if (out_skew)     *out_skew     = rot_y - rot_x;
	return 1;
}

// ---------------------------------------------------------------------------
// Matrix and color transform queries by entry_idx
// ---------------------------------------------------------------------------

// Flat root depth of a display entry, or (size_t)-1 if obj is NULL or not a
// root-level entry (e.g. a pointer into a nested sprite_display_list[]). Used to
// key the depth-keyed filter side-tables (tag.c) off a DisplayObject*.
size_t ng_objRootDepth(DisplayObject* obj)
{
	if (obj == NULL) return (size_t)-1;
	if (obj >= display_list && obj <= &display_list[max_depth])
		return (size_t)(obj - display_list);
	return (size_t)-1;
}

// Pointer-form matrix/CT accessors (the entry_idx variants below delegate here).
int ng_getMatrixFromObj(DisplayObject* obj,
    double* out_a, double* out_b, double* out_c, double* out_d,
    double* out_tx, double* out_ty)
{
	if (!obj) return 0;
	u32 tid = obj->transform_id;
	// The entry's own table: `transform_id` is an index into the COMBINED
	// placement table for anything a loaded movie placed (multi-SWF render
	// slice) and into the main movie's for everything else.
	float (*td)[16] = ng_entryTransformData(obj);
	if (out_a)  *out_a  = (double)td[tid][0];
	if (out_b)  *out_b  = (double)td[tid][1];
	if (out_c)  *out_c  = (double)td[tid][4];
	if (out_d)  *out_d  = (double)td[tid][5];
	if (out_tx) *out_tx = (double)td[tid][12] / 20.0;
	if (out_ty) *out_ty = (double)td[tid][13] / 20.0;
	return 1;
}

int ng_getCTFromObj(DisplayObject* obj,
    double* ra, double* ga, double* ba, double* aa,
    double* rb, double* gb, double* bb, double* ab)
{
	if (!obj) return 0;
	if (ra) *ra = obj->cx_ra; if (ga) *ga = obj->cx_ga;
	if (ba) *ba = obj->cx_ba; if (aa) *aa = obj->cx_aa;
	if (rb) *rb = obj->cx_rb; if (gb) *gb = obj->cx_gb;
	if (bb) *bb = obj->cx_bb; if (ab) *ab = obj->cx_ab;
	return 1;
}

int ng_setCTOnObj(DisplayObject* obj,
    double ra, double ga, double ba, double aa,
    double rb, double gb, double bb, double ab)
{
	if (!obj) return 0;
	obj->cx_ra = ra; obj->cx_ga = ga; obj->cx_ba = ba; obj->cx_aa = aa;
	obj->cx_rb = rb; obj->cx_gb = gb; obj->cx_bb = bb; obj->cx_ab = ab;
	obj->cx_overridden = 1;
	return 1;
}

// ---------------------------------------------------------------------------
// Color transform by name (root-level display_list scan)
// ---------------------------------------------------------------------------

int ng_getColorTransform(const char* name, double* ra, double* ga, double* ba, double* aa,
                          double* rb, double* gb, double* bb, double* ab)
{
	for (size_t d = 0; d <= max_depth; d++)
	{
		if (display_list[d].char_id == 0) continue;
		if (display_list[d].instance_name != NULL &&
		    strcmp(display_list[d].instance_name, name) == 0)
		{
			*ra = display_list[d].cx_ra; *ga = display_list[d].cx_ga;
			*ba = display_list[d].cx_ba; *aa = display_list[d].cx_aa;
			*rb = display_list[d].cx_rb; *gb = display_list[d].cx_gb;
			*bb = display_list[d].cx_bb; *ab = display_list[d].cx_ab;
			return 1;
		}
	}
	return 0;
}

int ng_setColorTransform(const char* name, double ra, double ga, double ba, double aa,
                          double rb, double gb, double bb, double ab)
{
	for (size_t d = 0; d <= max_depth; d++)
	{
		if (display_list[d].char_id == 0) continue;
		if (display_list[d].instance_name != NULL &&
		    strcmp(display_list[d].instance_name, name) == 0)
		{
			display_list[d].cx_ra = ra; display_list[d].cx_ga = ga;
			display_list[d].cx_ba = ba; display_list[d].cx_aa = aa;
			display_list[d].cx_rb = rb; display_list[d].cx_gb = gb;
			display_list[d].cx_bb = bb; display_list[d].cx_ab = ab;
			display_list[d].cx_overridden = 1;
			return 1;
		}
	}
	return 0;
}

void ng_setCTAlpha(size_t depth, double aa)
{
	if (depth <= max_depth && display_list[depth].char_id != 0) {
		display_list[depth].cx_aa = aa;
		display_list[depth].cx_overridden = 1;
	}
}

// Single-filter accessor: reads filter fields directly off a DisplayObject.
int ng_getObjFilterData(DisplayObject* obj, u8* type, float* blur_x, float* blur_y,
    u8* quality, u8* flags, float* r, float* g, float* b, float* a,
    float* strength, float* angle, float* distance,
    float* hr, float* hg, float* hb, float* ha)
{
	if (obj == NULL || obj->filter_type == 0) return 0;
	*type = obj->filter_type;
	*blur_x = obj->filter_blur_x;
	*blur_y = obj->filter_blur_y;
	*quality = obj->filter_quality;
	*flags = obj->filter_flags;
	*r = obj->filter_color_r;
	*g = obj->filter_color_g;
	*b = obj->filter_color_b;
	*a = obj->filter_color_a;
	*strength = obj->filter_strength;
	*angle = obj->filter_angle;
	*distance = obj->filter_distance;
	*hr = obj->filter_highlight_r;
	*hg = obj->filter_highlight_g;
	*hb = obj->filter_highlight_b;
	*ha = obj->filter_highlight_a;
	return 1;
}

// Double-precision wrappers kept for non-getBounds uses
static void boundsUnionCorner(double px, double py,
	double ma, double mb, double mc, double md, double mtx, double mty,
	int* has, double* gxmin, double* gymin, double* gxmax, double* gymax)
{
	// Ruffle uses f32 matrix * i32 twips → round_to_i32 per corner, then wrapping_add(tx).
	// tx/ty are Twips (integer). Simulate by rounding rotation+scale product and translation.
	double tx = round((float)(ma * px + mc * py)) + round(mtx);
	double ty = round((float)(mb * px + md * py)) + round(mty);
	if (!*has) { *gxmin = *gxmax = tx; *gymin = *gymax = ty; *has = 1; }
	else {
		if (tx < *gxmin) *gxmin = tx;
		if (tx > *gxmax) *gxmax = tx;
		if (ty < *gymin) *gymin = ty;
		if (ty > *gymax) *gymax = ty;
	}
}

// Overlay an AS-set (_x/_y/scale/rotation) transform onto a display entry's
// cached LOCAL placement matrix for the bounds / hit-test engines. The placement
// cache (place_*) and transform_data slots hold only the STATIC timeline
// transform; when ActionScript moves a nested child (e.g. Doodle Jump's blue
// platform charId 32 "aaa" sliding via clip_action_8's `this._x += ac`), that
// delta lives solely on the child MovieClip's as_set_flags — so a pure
// display-list walk contributes the child's original, un-moved bounds (the blue
// platform's collision stayed pinned at the left edge while it visibly slid).
// Mirrors apply_as_transform() in tag.c (the render path's equivalent overlay).
// Matches the entry by display_obj pointer — every recycled instance owns a
// distinct MC, and only that MC's display_obj points at THIS entry. a/b/c/d are
// the 2x2; tx/ty are in twips. No-op unless the entry has a named, AS-moved MC.
static void ng_overlay_entry_as_transform(DisplayObject* child,
    float* a, float* b, float* c, float* d, float* tx, float* ty)
{
    if (child == NULL || child->instance_name == NULL) return;
    extern MovieClip* child_mc_cache[];
    extern int child_mc_count;
    for (int i = 0; i < child_mc_count; i++) {
        MovieClip* mc = child_mc_cache[i];
        if (mc == NULL || mc->depth == INT_MIN) continue;
        if ((DisplayObject*)mc->display_obj != child) continue;
        u8 flags = mc->as_set_flags;
        // Overlay TRANSLATION only (_x/_y). Deliberately NOT scale/rotation
        // (bits 4|8|16): a clip's mc->xscale/yscale can hold extreme or
        // placement-synced values that are flagged AS-set, and replacing the
        // cached placement matrix with a scale-only reconstruction blew up
        // getBounds for movieclip_invalid_get_bounds_6/7 (boundingBox_mc,
        // yscale~110462 → bounds exploded). The placement matrix (place_*)
        // already carries the correct scale, and the moving-platform case this
        // fix targets (Doodle Jump blue "aaa") only slides via _x. Nested-child
        // AS scale/rotation inside a parent's bounds is a known gap that matches
        // current Ruffle expectations.
        if ((flags & 3) == 0) return;  // no _x/_y AS-set — leave placement as-is
        if (flags & 1) *tx = rintf(mc->x * 20.0f);
        if (flags & 2) *ty = rintf(mc->y * 20.0f);
        return;
    }
}

int ng_computeBoundsFromDL_matrix(DisplayObject* dl, size_t dl_max,
    double ma, double mb, double mc, double md, double mtx, double mty,
    int* has, double* gxmin, double* gymin, double* gxmax, double* gymax)
{
	for (size_t i = 1; i <= dl_max; i++) {
		DisplayObject* child = &dl[i];
		if (child->char_id == 0) continue;
		// Use cached transform values (stored at placement time by ng_cache_transform).
		// This ensures correct transforms for child SWFs whose transform_data
		// is a different array than the main SWF's.
		float ca = child->place_a;
		float cb = child->place_b;
		float cc = child->place_c;
		float cd = child->place_d;
		float ctxf = child->place_tx;
		float ctyf = child->place_ty;
		// Overlay any AS-set transform on this child (nested AS-moved clips —
		// e.g. Doodle Jump's blue platform — are otherwise frozen at their
		// placement position). No-op for un-moved / unnamed children.
		ng_overlay_entry_as_transform(child, &ca, &cb, &cc, &cd, &ctxf, &ctyf);
		// Ruffle composes matrices in f32 arithmetic
		float fma = (float)ma, fmb = (float)mb, fmc = (float)mc, fmd = (float)md;
		float fmtx = (float)mtx, fmty = (float)mty;
		double na = (double)(fma*ca + fmc*cb), nb = (double)(fmb*ca + fmd*cb);
		double nc = (double)(fma*cc + fmc*cd), nd = (double)(fmb*cc + fmd*cd);
		double ntx = (double)(fma*ctxf + fmc*ctyf + fmtx);
		double nty = (double)(fmb*ctxf + fmd*ctyf + fmty);

		if (child->sprite_display_list != NULL && child->sprite_max_depth > 0) {
			ng_computeBoundsFromDL_matrix(child->sprite_display_list, child->sprite_max_depth,
				na, nb, nc, nd, ntx, nty, has, gxmin, gymin, gxmax, gymax);
		} else {
			s32 cxmin, cxmax, cymin, cymax;
			int child_has = ng_getCharBounds(child->char_id, &cxmin, &cxmax, &cymin, &cymax);
			if (!child_has) {
				int tf_idx = ng_find_textfield(child->char_id);
				if (tf_idx >= 0) {
					ng_getTextFieldBounds(tf_idx, &cxmin, &cxmax, &cymin, &cymax);
					child_has = 1;
				}
			}
			if (!child_has) continue;
			boundsUnionCorner((double)cxmin, (double)cymin, na, nb, nc, nd, ntx, nty, has, gxmin, gymin, gxmax, gymax);
			boundsUnionCorner((double)cxmax, (double)cymin, na, nb, nc, nd, ntx, nty, has, gxmin, gymin, gxmax, gymax);
			boundsUnionCorner((double)cxmin, (double)cymax, na, nb, nc, nd, ntx, nty, has, gxmin, gymin, gxmax, gymax);
			boundsUnionCorner((double)cxmax, (double)cymax, na, nb, nc, nd, ntx, nty, has, gxmin, gymin, gxmax, gymax);
		}
	}
	return *has;
}

// Unified local-bounds helper (the engine getBounds already trusts). Returns the
// content bounds (twips, double) of a display list, the queried object's own
// transform NOT applied — i.e. ng_computeBoundsFromDL_matrix with an identity
// matrix. Full 2x3 matrix, arbitrary nesting depth. Returns 1 if any bounds
// found. This supersedes the legacy ng_getDisplayEntryBounds (pixels/float,
// scale+translate-only, 2-level cap).
int ng_localBoundsOfDL(DisplayObject* dl, size_t dl_max,
    double* out_xmin, double* out_ymin, double* out_xmax, double* out_ymax)
{
	int has = 0;
	double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
	if (dl != NULL)
		ng_computeBoundsFromDL_matrix(dl, dl_max, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0,
			&has, &xmin, &ymin, &xmax, &ymax);
	if (has) {
		if (out_xmin) *out_xmin = xmin;
		if (out_ymin) *out_ymin = ymin;
		if (out_xmax) *out_xmax = xmax;
		if (out_ymax) *out_ymax = ymax;
	}
	return has;
}

// Simple wrapper: compute bounds in local space (identity matrix)
int ng_computeBoundsFromDL(DisplayObject* dl, size_t dl_max,
    int32_t* out_xmin, int32_t* out_ymin, int32_t* out_xmax, int32_t* out_ymax)
{
	int has = 0;
	double gxmin = 0, gymin = 0, gxmax = 0, gymax = 0;
	ng_computeBoundsFromDL_matrix(dl, dl_max, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0,
		&has, &gxmin, &gymin, &gxmax, &gymax);
	if (has) {
		*out_xmin = (int32_t)rintf((float)gxmin); *out_ymin = (int32_t)rintf((float)gymin);
		*out_xmax = (int32_t)rintf((float)gxmax); *out_ymax = (int32_t)rintf((float)gymax);
	}
	return has;
}

// Defined in shape_hit_test.c (compiled in all modes).
extern int ng_hitTestShapeChar(size_t char_id, u16 ratio,
    double ma, double mb, double mc_m, double md, double mtx, double mty,
    double test_x, double test_y);

int ng_hitTestShapeFromDL(DisplayObject* dl, size_t dl_max,
    double ma, double mb, double mc_m, double md, double mtx, double mty,
    double test_x, double test_y)
{
	size_t clip_depth_end = 0;  // if >0, masking active until this depth
	int clip_hit = 0;           // whether test point hits the current clip shape

	for (size_t i = 1; i <= dl_max; i++) {
		DisplayObject* child = &dl[i];
		if (child->char_id == 0) continue;

		u32 tid = child->transform_id;
		// See ng_getMatrixFromObj: a loaded movie's placement ids index the
		// combined table, and this walk descends INTO a loaded child's sprites.
		float (*ctd)[16] = ng_entryTransformData(child);
		float oa = ctd[tid][0], ob = ctd[tid][1];
		float oc = ctd[tid][4], od = ctd[tid][5];
		float otx = ctd[tid][12], oty = ctd[tid][13];
		// Overlay any AS-set transform (nested AS-moved clips otherwise hit-test
		// at their static placement — same gap as the bounds engine above).
		ng_overlay_entry_as_transform(child, &oa, &ob, &oc, &od, &otx, &oty);
		double ca = (double)oa;
		double cb = (double)ob;
		double cc = (double)oc;
		double cd = (double)od;
		double ctx_v = (double)otx;
		double cty_v = (double)oty;
		double na = ma*ca + mc_m*cb, nb = mb*ca + md*cb;
		double nc = ma*cc + mc_m*cd, nd = mb*cc + md*cd;
		double ntx = ma*ctx_v + mc_m*cty_v + mtx;
		double nty = mb*ctx_v + md*cty_v + mty;

		// Clip-depth masking: this child is a clipping layer
		if (child->clip_depth > 0) {
			int mask_hit = 0;
			if (child->sprite_display_list != NULL && child->sprite_max_depth > 0) {
				mask_hit = ng_hitTestShapeFromDL(child->sprite_display_list, child->sprite_max_depth,
					na, nb, nc, nd, ntx, nty, test_x, test_y);
			}
			if (!mask_hit) {
				mask_hit = ng_hitTestShapeChar(child->char_id, child->ratio, na, nb, nc, nd, ntx, nty,
					test_x, test_y);
			}
			if (mask_hit) {
				clip_depth_end = 0;  // point hits mask: deactivate masking
			} else {
				clip_depth_end = child->clip_depth;  // point misses mask: activate
			}
			continue;  // clip layers are never directly hittable
		}

		// If masking is active and this child is within the masked range, skip it
		if (clip_depth_end > 0) {
			if (i <= clip_depth_end) continue;  // masked out
			clip_depth_end = 0;  // past the clip region
		}

		if (child->sprite_display_list != NULL && child->sprite_max_depth > 0) {
			if (ng_hitTestShapeFromDL(child->sprite_display_list, child->sprite_max_depth,
				na, nb, nc, nd, ntx, nty, test_x, test_y))
				return 1;
			// Sprites that are also textfields (EditText with MC association):
			// if recursive display list test fails, test textfield bounds
			if (ng_hitTestShapeChar(child->char_id, child->ratio, na, nb, nc, nd, ntx, nty,
				test_x, test_y))
				return 1;
		} else {
			if (ng_hitTestShapeChar(child->char_id, child->ratio, na, nb, nc, nd, ntx, nty,
				test_x, test_y))
				return 1;
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Depth manipulation (swapDepths, updateDepth)
// ---------------------------------------------------------------------------

void ng_updateDisplayDepth(const char* name, int new_as_depth)
{
	size_t new_swf_depth = (size_t)(new_as_depth + 16384);

	// Find the entry by name
	size_t old_depth = SIZE_MAX;
	for (size_t d = 0; d <= max_depth; d++)
	{
		if (display_list[d].char_id == 0) continue;
		if (display_list[d].instance_name != NULL &&
		    strcmp(display_list[d].instance_name, name) == 0)
		{
			old_depth = d;
			break;
		}
	}
	if (old_depth == SIZE_MAX || old_depth == new_swf_depth) return;

	// Ensure display_list is large enough for new_swf_depth
	DisplayObject* udd_old_dl = display_list;
	if (new_swf_depth >= display_list_capacity)
	{
		// Grow display_list using HCALLOC (copy + free old)
		size_t new_cap = new_swf_depth + 64;
		DisplayObject* new_dl = (DisplayObject*) calloc(new_cap, sizeof(DisplayObject));
		if (new_dl == NULL) return;
		memcpy(new_dl, display_list, (max_depth + 1) * sizeof(DisplayObject));
		// Note: can't FREE the original HCALLOC'd list safely here,
		// but we need to update the pointer. In practice swapDepths
		// to very high depths is rare and the old memory will leak.
		display_list = new_dl;
		display_list_capacity = new_cap;
	}

	if (display_list[new_swf_depth].char_id != 0)
	{
		// Target depth occupied: swap entries and update the other MC's depth.
		// placed_at_frame / place_gen track the *depth's* last PlaceObject2
		// tag, not the MC's identity — Ruffle's AVM1 survives_rewind matches
		// the MC currently at a depth against final_placements.get(depth),
		// which is independent of which MC originally landed there. So these
		// two fields stay bound to the depth slot during swap; only the
		// MC-identity state (char_id, sprite_display_list, instance_name,
		// transforms, etc.) moves. Without this, rewind misidentifies which
		// depth was "freshly placed after target" (e.g. loop_test3).
		size_t paf_old = display_list[old_depth].placed_at_frame;
		size_t paf_new = display_list[new_swf_depth].placed_at_frame;
		size_t pg_old  = display_list[old_depth].place_gen;
		size_t pg_new  = display_list[new_swf_depth].place_gen;
		DisplayObject tmp = display_list[old_depth];
		display_list[old_depth] = display_list[new_swf_depth];
		display_list[new_swf_depth] = tmp;
		display_list[old_depth].placed_at_frame     = paf_old;
		display_list[new_swf_depth].placed_at_frame = paf_new;
		display_list[old_depth].place_gen           = pg_old;
		display_list[new_swf_depth].place_gen       = pg_new;
		// Mark both as swapped so timeline modifies are ignored
		display_list[old_depth].depth_swapped = 1;
		display_list[new_swf_depth].depth_swapped = 1;

		// Update the other MC's cached depth (AS depth = SWF depth - 16384)
		if (display_list[old_depth].instance_name != NULL)
		{
			extern int child_mc_count;
			extern MovieClip* child_mc_cache[];
			const char* other_name = display_list[old_depth].instance_name;
			for (int _ci = 0; _ci < child_mc_count; _ci++)
			{
				if (child_mc_cache[_ci] != NULL &&
				    strcmp(child_mc_cache[_ci]->name, other_name) == 0)
				{
					child_mc_cache[_ci]->depth = (int)old_depth - 16384;
					break;
				}
			}
		}
	}
	else
	{
		// Target depth empty: move entry and clear old slot.
		// The destination had no prior PlaceObject2 tag; treat the move as
		// happening at the current frame so that rewind to any earlier
		// target removes this depth (no final_placement at it).
		extern size_t current_frame;
		extern size_t g_place_gen;
		display_list[new_swf_depth] = display_list[old_depth];
		display_list[new_swf_depth].placed_at_frame = current_frame;
		display_list[new_swf_depth].place_gen       = g_place_gen;
		display_list[new_swf_depth].depth_swapped   = 1;
		memset(&display_list[old_depth], 0, sizeof(DisplayObject));
	}

	// Update max_depth
	if (new_swf_depth > max_depth) max_depth = new_swf_depth;
	// Shrink max_depth if we just cleared the last entry
	while (max_depth > 0 && display_list[max_depth].char_id == 0) max_depth--;

	// The grow above strands the root sentinel (root_movieclip.display_obj,
	// ng_shared.c) on the OLD buffer, which is leaked, not freed, and still
	// holds the pre-move entry. resolveSlashPathToMC's display_obj fallback
	// then finds the moved clip at its OLD depth and conjures a fresh MC
	// for it — e.g. `child.swapDepths(1000); child.removeMovieClip()` left
	// "/child" resolvable for the rest of the frame, so a removed closure
	// base clip re-resolved to that ghost (avm1/removed_clip_function_scope).
	if (display_list != udd_old_dl)
	{
		extern void* ng_get_root_display_obj(void);
		extern void ng_sync_root_display_obj(void);
		DisplayObject* udd_root = (DisplayObject*)ng_get_root_display_obj();
		if (udd_root != NULL && udd_root->sprite_display_list == udd_old_dl)
			ng_sync_root_display_obj();
	}
}

void ng_swapDisplayDepths(const char* name1, const char* name2)
{
	size_t d1 = SIZE_MAX, d2 = SIZE_MAX;
	for (size_t d = 0; d <= max_depth; d++)
	{
		if (display_list[d].char_id == 0) continue;
		if (display_list[d].instance_name != NULL)
		{
			if (strcmp(display_list[d].instance_name, name1) == 0) d1 = d;
			else if (strcmp(display_list[d].instance_name, name2) == 0) d2 = d;
		}
	}
	if (d1 != SIZE_MAX && d2 != SIZE_MAX)
	{
		// Swap the entire display entries (including their transform_ids etc.).
		// placed_at_frame / place_gen stay pinned to the depth — see the
		// matching comment in ng_updateDisplayDepth above.
		size_t paf_d1 = display_list[d1].placed_at_frame;
		size_t paf_d2 = display_list[d2].placed_at_frame;
		size_t pg_d1  = display_list[d1].place_gen;
		size_t pg_d2  = display_list[d2].place_gen;
		DisplayObject tmp = display_list[d1];
		display_list[d1] = display_list[d2];
		display_list[d2] = tmp;
		display_list[d1].placed_at_frame = paf_d1;
		display_list[d2].placed_at_frame = paf_d2;
		display_list[d1].place_gen       = pg_d1;
		display_list[d2].place_gen       = pg_d2;
		// Mark both as swapped so timeline modifies are ignored
		display_list[d1].depth_swapped = 1;
		display_list[d2].depth_swapped = 1;
	}
}

// ---------------------------------------------------------------------------
// Instance name management
// ---------------------------------------------------------------------------

void ng_renameDisplayEntry(const char* old_name, const char* new_name)
{
	for (size_t d = 0; d <= max_depth; d++)
	{
		if (display_list[d].char_id == 0) continue;
		if (display_list[d].instance_name != NULL &&
		    strcmp(display_list[d].instance_name, old_name) == 0)
		{
			// If we own the old name, free it
			if (display_list[d].instance_name_owned)
			{
				free(display_list[d].instance_name);
			}
			// Must strdup since new_name may point to a stack buffer
			display_list[d].instance_name = strdup(new_name);
			display_list[d].instance_name_owned = 1;
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// TextField property accessors (display-list-dependent, kept in tag_stubs.c)
// ---------------------------------------------------------------------------

const char* ng_getTextFieldInitialText(size_t depth)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return "";
	int tf_idx = ng_find_textfield(display_list[depth].char_id);
	if (tf_idx < 0) return "";
	return ng_getTextFieldInitialTextByIdx(tf_idx);
}

u32 ng_getTextFieldColor(size_t depth)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return 0;
	int tf_idx = ng_find_textfield(display_list[depth].char_id);
	if (tf_idx < 0) return 0;
	return ng_getTextFieldColorByIdx(tf_idx);
}

int ng_getTextFieldIdx(size_t depth)
{
	if (depth > max_depth || display_list[depth].char_id == 0) return -1;
	return ng_find_textfield(display_list[depth].char_id);
}

// ---------------------------------------------------------------------------
// Clone/duplicate sprite helpers
// ---------------------------------------------------------------------------

// Register a freshly-created clone under its real parent. AVM1 clone semantics:
// duplicate/clone binds the new clip as a display-list child of its parent
// (the source's parent), NOT as a global _root variable. Root clones keep the
// global clone-depth table (the only mode that makes it meaningful); the name
// binding goes to the parent's dynamic_props in ALL cases via
// ng_register_instance_on_parent_dprops — instance names never enter var_map
// (user-variable store). Without the non-root binding, a nested
// `child.duplicateMovieClip("dup", n)` minted the clone as a _root ghost and
// `parent.dup` never resolved (Ruffle: parent.dup is a movieclip).
static void ng_register_clone_under_parent(SWFAppContext* app_context,
                                           MovieClip* parent, const char* name,
                                           MovieClip* clone_mc, int swf_depth_for_table)
{
	if (parent == NULL) parent = &root_movieclip;
	if (parent == &root_movieclip)
		clone_depth_register(app_context, swf_depth_for_table, name, clone_mc);
	ng_register_instance_on_parent_dprops(app_context, parent, name, clone_mc);
}

MovieClip* ng_cloneSprite(SWFAppContext* app_context, const char* source_name,
                           const char* target_name, int depth, MovieClip* parent)
{
	if (!source_name || !target_name) return NULL;
	if (depth > 2130706428) return NULL;
	MovieClip* clone_parent = parent ? parent : &root_movieclip;

	// Phase 2c (CLONESPRITE_DEPTH_BIAS): two callers with different conventions.
	// (1) Phase 2a stripped (recompiler set g_clone_depth_already_unbiased=1):
	//     `depth` is RAW AS depth. SWF depth (slot index) = depth + AVM_DEPTH_BIAS.
	// (2) Unstripped: `depth` is RAW SWF depth (Ming-biased or hand-pushed).
	//     SWF depth (slot index) = depth directly. clone_mc->depth via heuristic.
	// We resolve to as_depth (always raw AS depth) for clone_mc->depth and
	// clone_depth_register, but use different `target_swf_depth` per branch
	// to preserve pre-Phase-2c semantics for unstripped pushes (e.g., Shumway
	// `dontremove` pushes raw SWF depth 16379 expecting static-range behavior).
	extern int g_clone_depth_already_unbiased;
	int is_stripped = g_clone_depth_already_unbiased;
	g_clone_depth_already_unbiased = 0;  // consume the marker
	int as_depth = is_stripped
	               ? depth
	               : ((depth >= AVM_DEPTH_BIAS) ? (depth - AVM_DEPTH_BIAS) : depth);
	// SWF depth: post-strip we bias up; unstripped we trust the input as SWF depth.
	int swf_depth_for_table = is_stripped ? (as_depth + AVM_DEPTH_BIAS) : depth;

	// Find source by instance name at root level
	size_t src_depth = ng_findDisplayEntryByName(source_name);

	if (src_depth != SIZE_MAX)
	{
		size_t cid = display_list[src_depth].char_id;
		int scriptable = (dictionary[cid].type == CHAR_TYPE_SPRITE) ||
		                 ng_find_button(cid) || (ng_find_textfield(cid) >= 0) ||
		                 ng_find_video(cid);
		if (!scriptable) return NULL;

		// Slot index: stripped path biases up to dynamic range so AS depths
		// 1, 2, 3 don't collide with timeline static MCs; unstripped path
		// uses depth directly (preserving pre-Phase-2c semantics for SWFs
		// that push raw SWF depths like Shumway dontremove).
		size_t target_swf_depth = (size_t)(unsigned int)swf_depth_for_table;
		// Pre-clear target depth if occupied
		if (target_swf_depth >= 1 && target_swf_depth <= max_depth &&
		    target_swf_depth < display_list_capacity &&
		    display_list[target_swf_depth].char_id != 0)
		{
			ng_on_remove_object(app_context, target_swf_depth);
			// clear_display_entry handled by tagRemoveObject2 caller — do minimal cleanup
			if (display_list[target_swf_depth].instance_name_owned &&
			    display_list[target_swf_depth].instance_name != NULL)
			{
				free(display_list[target_swf_depth].instance_name);
				display_list[target_swf_depth].instance_name_owned = 0;
				display_list[target_swf_depth].instance_name = NULL;
			}
		}

		// Copy display entry to clone depth (clones live up to AVM_CLONE_SLOT_CAP).
		// Browser-WASM graphics: skip the global display_list write. Reason: the
		// struct copy shares the source's transform_id with the clone, and tag.c's
		// runtime-transform-update loop (tag.c:~3070) then mutates the shared
		// transform_data[transform_id] slot every tick for whichever entry it
		// processes last — corrupting the source MC's stage transform (the visible
		// "play area shifts to match the snake clone's position" symptom in
		// Snake). In browser-WASM the clone is rendered exclusively through the
		// child_mc_cache loop (tag.c:~3912), which uses the separately-calloc'd
		// clone_mc->display_obj wrapper and applies the AS-set transform via
		// apply_as_transform without touching transform_data. NO_GRAPHICS /
		// OFFSCREEN_RENDER (trace + graphics-native) still need the display_list
		// write — they don't have the child_mc_cache render fallback and they
		// rely on display_list[clone_depth] for name lookups / instance walks.
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
		if (target_swf_depth < AVM_CLONE_SLOT_CAP)
		{
			// Ensure capacity — realloc with aliased-pointer rebase (the old
			// HCALLOC/memcpy/FREE here stranded every mc->display_obj pointing
			// into the root display_list; see swf.h ALIASING RULE).
			if (target_swf_depth >= display_list_capacity)
				ng_spriteDLRealloc(app_context, &display_list,
				                   &display_list_capacity,
				                   target_swf_depth + 64);
			display_list[target_swf_depth] = display_list[src_depth];
			// Give clone its own strdup'd name
			display_list[target_swf_depth].instance_name = strdup(target_name);
			display_list[target_swf_depth].instance_name_owned = 1;
			display_list[target_swf_depth].sprite_display_list = NULL;
			display_list[target_swf_depth].sprite_max_depth = 0;
			display_list[target_swf_depth].sprite_dl_capacity = 0;
			display_list[target_swf_depth].sprite_needs_init = 0;
			// CLONE_CLIP_EVENT_DISPATCH Phase 2: inherit the source's clip_actions
			// pointer (it's recompiler-emitted static data, never freed
			// per-instance). Phase 5 ensures the slot is cleared on
			// removeMovieClip / actionRemoveSprite so dispatch doesn't
			// re-fire forever.
			// (clip_actions / clip_action_count come along via the slot copy.)
			// CLONE_CLIP_EVENT_DISPATCH Phase 3: mark the slot as fully
			// initialized (>=2) so dispatch_enterframe_clip_actions picks it
			// up next tick. The manual frame_0 runner below populates
			// sprite_display_list children, so we don't go through
			// process_sprite_needs_init (which would double-fire frame_0).
			display_list[target_swf_depth].sprite_initialized = 2;
			display_list[target_swf_depth].enterframe_eligible = 0;
			display_list[target_swf_depth].constructor_invoked = 0;
			// CLONE_CLIP_EVENT_DISPATCH Phase 6: mark slot as clone-replaced
			// so backward goto's survives_rewind returns false and freshly
			// re-places the original static MC (re-firing CONSTRUCT).
			display_list[target_swf_depth].clone_replaced = 1;
			if (target_swf_depth > max_depth) max_depth = target_swf_depth;
		}

		// CLONE_CLIP_EVENT_DISPATCH Phase 4: drain INIT/CONSTRUCT inline
		// for the clone slot (when one exists). LOAD is queued separately
		// below via ng_queue_pending_load so out-of-cap clones (no slot)
		// and in-cap clones share insertion order in a single queue —
		// avm1/duplicate_movie_clip mixes both and expects FIFO LOAD order.
		if (target_swf_depth < AVM_CLONE_SLOT_CAP &&
		    display_list[target_swf_depth].clip_action_count > 0)
		{
			ng_queue_placement_clip_events(app_context, target_swf_depth);
		}
#endif

		// CloneSprite fires onLoad for the clone (unlike duplicateMovieClip).
		if (display_list[src_depth].clip_action_count > 0)
		{
			ng_queue_pending_load(target_name,
				display_list[src_depth].clip_actions,
				display_list[src_depth].clip_action_count);
		}
	}

	// Find source MC and create clone MC under the clone's real parent.
	MovieClip* src_mc = actionFindOrCreateMovieClip(app_context, source_name, clone_parent);
	if (src_mc == NULL || src_mc == &root_movieclip) return NULL; // cannot clone root
	MovieClip* clone_mc = actionFindOrCreateMovieClip(app_context, target_name, clone_parent);
	if (clone_mc == NULL) return NULL;
	clone_mc->as_created = 1;  /* AS-created: name-resolvable via cache scan (soft refs) */

	if (src_mc != NULL)
	{
		clone_mc->x       = src_mc->x;
		clone_mc->y       = src_mc->y;
		clone_mc->xscale  = src_mc->xscale;
		clone_mc->yscale  = src_mc->yscale;
		clone_mc->rotation = src_mc->rotation;
		clone_mc->alpha   = src_mc->alpha;
		// Clones are born visible. Ruffle's clone_sprite copies only the matrix
		// and color transform, never _visible — a duplicate of a clip the script
		// hid with _visible=false is still shown. Inheriting src visibility left
		// Minesweeper's 480 board-cell clones invisible (the game sets
		// cell._visible=false before the duplicateMovieClip loop) → blank board.
		clone_mc->visible = 1;
		clone_mc->totalframes   = src_mc->totalframes;
		clone_mc->framesloaded  = src_mc->framesloaded;
		clone_mc->as_set_flags  = src_mc->as_set_flags;
		clone_mc->draw_has_bounds = src_mc->draw_has_bounds;
		clone_mc->draw_xmin = src_mc->draw_xmin;
		clone_mc->draw_xmax = src_mc->draw_xmax;
		clone_mc->draw_ymin = src_mc->draw_ymin;
		clone_mc->draw_ymax = src_mc->draw_ymax;
		// For static-sprite clones (no drawing API bounds), capture the source's
		// bounds so the clone's _width/_height fall through to the
		// mc->width/height fallback in mcGetOriginalBounds. The clone's
		// display_list entry is skipped when target_swf_depth >= 1024
		// (INITIAL_DISPLAYLIST_CAPACITY guard), so without this the dup
		// returns 0 for _width/_height. Dynamic clones with drawing API
		// bounds already have draw_xmin/xmax/ymin/ymax copied above.
		if (!clone_mc->draw_has_bounds && src_depth != SIZE_MAX && src_depth <= max_depth
		    && display_list[src_depth].sprite_display_list != NULL) {
			double bxmin, bymin, bxmax, bymax;
			if (ng_localBoundsOfDL(display_list[src_depth].sprite_display_list,
			                       display_list[src_depth].sprite_max_depth,
			                       &bxmin, &bymin, &bxmax, &bymax)) {
				clone_mc->width  = (float)((bxmax - bxmin) / 20.0);
				clone_mc->height = (float)((bymax - bymin) / 20.0);
			}
		}
		// Inherit the source's TextSnapshot stale flag BEFORE marking the
		// source itself. In Ruffle, clone_sprite via instantiate_by_id
		// produces an instance whose TextSnapshot availability mirrors its
		// source, so cloning an already-stale source yields a clone that
		// is born stale — matches avm1/textsnapshot_available_text.
		clone_mc->ts_stale_source = src_mc->ts_stale_source;
		// Mark source as "used as dup source" so later `new TextSnapshot(src)`
		// calls reject it (Ruffle's as_movie_clip() returns None for these
		// after clone).
		src_mc->ts_stale_source = 1;
	}
	clone_mc->currentframe = 1;
	// Phase 2c: store the unbiased AS depth (computed above via the heuristic
	// that handles both stripped patterns and direct pre-biased pushes).
	clone_mc->depth = as_depth;

	// For dynamic textfield clones (no DefineEditText tag), init default props
	if (src_mc != NULL && src_mc->ng_textfield_idx == -2 && clone_mc->ng_textfield_idx != -2) {
		actionInitDynTextFieldClone(app_context, clone_mc);
		// If source has active autoSize, restore clone to original createTextField position
		if (src_mc->dynamic_props != NULL && clone_mc->dynamic_props != NULL) {
			ASObject* _src_p = (ASObject*) src_mc->dynamic_props;
			ASObject* _cln_p = (ASObject*) clone_mc->dynamic_props;
			ActionVar* cx = getProperty(_src_p, "_tf_createX", 11);
			ActionVar* cy = getProperty(_src_p, "_tf_createY", 11);
			if (cx) setProperty(app_context, _cln_p, "_tf_createX", 11, cx);
			if (cy) setProperty(app_context, _cln_p, "_tf_createY", 11, cy);
			ActionVar* as_prop = getProperty(_src_p, "autoSize", 8);
			int has_autosize = 0;
			if (as_prop && as_prop->type == ACTION_STACK_VALUE_STRING && as_prop->str_size > 0) {
				// Check if autoSize != "none" by comparing UTF-16 directly
				const uint16_t* as_u16 = (const uint16_t*)(uintptr_t)as_prop->data.numeric_value;
				int is_none = (as_prop->str_size == 4 &&
					as_u16[0] == 'n' && as_u16[1] == 'o' &&
					as_u16[2] == 'n' && as_u16[3] == 'e');
				if (!is_none) has_autosize = 1;
			}
			if (has_autosize) {
				if (cx && cx->type == ACTION_STACK_VALUE_F64) {
					double ox; memcpy(&ox, &cx->data.numeric_value, sizeof(double));
					clone_mc->x = (float)ox;
				}
				if (cy && cy->type == ACTION_STACK_VALUE_F64) {
					double oy; memcpy(&oy, &cy->data.numeric_value, sizeof(double));
					clone_mc->y = (float)oy;
				}
			}
		}
	}

	// Evict any old clone registered at this SWF depth, then register new one.
	// Use swf_depth_for_table (post-strip biased; unstripped uses raw depth).
	// Pass clone_mc so the by-name walk skips the freshly-created clone — a
	// stale entry from a prior session with the same name would otherwise
	// match it and stamp depth=INT_MIN, hiding the clone from the render loop.
	ng_register_clone_under_parent(app_context, clone_parent, target_name,
	                               clone_mc, swf_depth_for_table);

	// If source is a sprite, run frame 0 to populate clone's sprite_display_list.
	// After cloning, clear the source MC's display_obj so TextSnapshot of source
	// returns empty (Flash behavior: text "moves" from source to clone).
	//
	// Source char_id falls back to the source MC's DisplayObject when the global
	// display_list lookup misses (nested timeline source — see ng_duplicateMovieClip
	// + gates/nested_timeline_clone).
	size_t src_cid = 0;
	if (src_depth != SIZE_MAX)
		src_cid = display_list[src_depth].char_id;
	else if (src_mc != NULL && src_mc->display_obj != NULL)
		src_cid = ((DisplayObject*)src_mc->display_obj)->char_id;
	{
		size_t cid = src_cid;
		if (cid != 0 && dictionary[cid].type == CHAR_TYPE_SPRITE)
		{
			frame_func* funcs = dictionary[cid].sprite_frame_funcs;
			size_t frame_count = dictionary[cid].sprite_frame_count;
			if (funcs != NULL && frame_count > 0 && funcs[0] != NULL)
			{
				DisplayObject* saved_dl  = display_list;
				size_t         saved_max = max_depth;
				size_t         saved_cap = display_list_capacity;
				MovieClip*     saved_ctx = NULL;
				DisplayObject* saved_sprite_obj = g_current_sprite_obj;
				int            saved_catch_up = catch_up_mode;

				// Create (or reset) display_obj for clone to hold its children.
				// Re-cloning into a clip that ALREADY exists (e.g. Minesweeper's
				// Restart re-runs the board-build duplicateMovieClip loop over the
				// still-live 480 cells) must start from a clean, frame-0 state — the
				// same state a fresh allocation gives. Without the reset, funcs[0]
				// below merges frame-0 place tags ON TOP of the previous frame's
				// content, and the stale sprite_current_frame makes the subsequent
				// gotoAndStop seek (ng_gotoFrameByMC) take its same-frame no-rebuild
				// branch — leaving the clip frozen on the polluted frame-0 geometry
				// while currentframe reports the requested frame. (The cell0_0-
				// after-Restart misplacement: the FIRST re-clone re-ran the template's
				// frame_0 onto the stale frame-12 "Unknown" list; later cells were
				// spared only because cloning nulls the template's display_obj, so
				// their funcs[0] was skipped.)
				DisplayObject* dobj = (DisplayObject*)clone_mc->display_obj;
				if (dobj == NULL) {
					dobj = calloc(1, sizeof(DisplayObject));
					dobj->sprite_dl_capacity = 64;
					// HCALLOC (not calloc): growth paths realloc/FREE this buffer on
					// the custom heap (ng_spriteDLRealloc); a calloc'd buffer would
					// SIGABRT heap_free's ownership check.
					dobj->sprite_display_list = HCALLOC(dobj->sprite_dl_capacity, sizeof(DisplayObject));
					clone_mc->display_obj = dobj;
				} else if (dobj->sprite_display_list != NULL) {
					// Free nested child sprite lists, then zero the entries (mirrors
					// the backward-jump rewind in ng_gotoFrameByMC). memset (not free)
					// keeps the top-level buffer regardless of which allocator owns it.
					for (size_t _rj = 0; _rj < dobj->sprite_dl_capacity; _rj++) {
						if (dobj->sprite_display_list[_rj].sprite_display_list != NULL) {
							ng_freeSpriteDL(app_context, dobj->sprite_display_list[_rj].sprite_display_list,
						                dobj->sprite_display_list[_rj].sprite_dl_capacity);
							dobj->sprite_display_list[_rj].sprite_display_list = NULL;
						}
					}
					memset(dobj->sprite_display_list, 0,
					       dobj->sprite_dl_capacity * sizeof(DisplayObject));
				}
				dobj->char_id = cid;
				dobj->sprite_max_depth = 0;
				dobj->sprite_current_frame = 0;
				display_list = dobj->sprite_display_list;
				max_depth = dobj->sprite_max_depth;
				display_list_capacity = dobj->sprite_dl_capacity;

				saved_ctx = g_current_context;
				actionSetCurrentContext(clone_mc);
				g_current_sprite_obj = NULL;

				// Phase 7b note: do NOT bump eager here. Pre-7b skipped scripts
				// during ng_cloneSprite (catch_up_mode=1 + old `if (!catch_up_mode) script_N` gate).
				// Under 7b gate, this stays equivalent: catch=1, eager=0 → queue path FALSE,
				// sync path FALSE. Matches pre-7b behavior.
				catch_up_mode = 1;
				funcs[0](app_context);
				catch_up_mode = saved_catch_up;

				actionSetCurrentContext(saved_ctx);
				g_current_sprite_obj = saved_sprite_obj;

				dobj->sprite_display_list = display_list;
				dobj->sprite_max_depth = max_depth;
				dobj->sprite_dl_capacity = display_list_capacity;

				display_list = saved_dl;
				max_depth = saved_max;
				display_list_capacity = saved_cap;
			}

			// Clear source MC's display_obj so new TextSnapshot(source) returns empty
			if (src_mc != NULL) {
				src_mc->display_obj = NULL;
			}
		}
	}

	return clone_mc;
}

MovieClip* ng_cloneSpriteFromMC(SWFAppContext* app_context, MovieClip* src_mc,
                                  const char* target_name, int depth)
{
	if (!src_mc || !target_name) return NULL;
	if (src_mc->parent == NULL) return NULL;  // cannot clone root

	// Look up source display entry — needed for display list copy & clip_actions
	size_t src_depth = ng_findDisplayEntryByName(src_mc->name);

	// Create display list entry for the clone (mirrors ng_cloneSprite logic).
	// Phase 2c: same dual-path as ng_cloneSprite — see that function.
	extern int g_clone_depth_already_unbiased;
	int is_stripped = g_clone_depth_already_unbiased;
	g_clone_depth_already_unbiased = 0;  // consume the marker
	int as_depth = is_stripped
	               ? depth
	               : ((depth >= AVM_DEPTH_BIAS) ? (depth - AVM_DEPTH_BIAS) : depth);
	int swf_depth_for_table = is_stripped ? (as_depth + AVM_DEPTH_BIAS) : depth;
	size_t target_swf_depth = (size_t)(unsigned int)swf_depth_for_table;
	// Browser-WASM graphics: skip the global display_list write. See
	// the corresponding comment in ng_cloneSprite for rationale.
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (src_depth != SIZE_MAX && target_swf_depth < AVM_CLONE_SLOT_CAP)
	{
		// Pre-clear target depth if occupied
		if (target_swf_depth >= 1 && target_swf_depth <= max_depth &&
		    target_swf_depth < display_list_capacity &&
		    display_list[target_swf_depth].char_id != 0)
		{
			ng_on_remove_object(app_context, target_swf_depth);
			if (display_list[target_swf_depth].instance_name_owned &&
			    display_list[target_swf_depth].instance_name != NULL)
			{
				free(display_list[target_swf_depth].instance_name);
				display_list[target_swf_depth].instance_name_owned = 0;
				display_list[target_swf_depth].instance_name = NULL;
			}
		}
		// Ensure capacity — realloc with aliased-pointer rebase (the old
		// HCALLOC/memcpy/FREE here stranded every mc->display_obj pointing
		// into the root display_list; see swf.h ALIASING RULE).
		if (target_swf_depth >= display_list_capacity)
			ng_spriteDLRealloc(app_context, &display_list,
			                   &display_list_capacity,
			                   target_swf_depth + 64);
		display_list[target_swf_depth] = display_list[src_depth];
		display_list[target_swf_depth].instance_name = strdup(target_name);
		display_list[target_swf_depth].instance_name_owned = 1;
		display_list[target_swf_depth].sprite_display_list = NULL;
		display_list[target_swf_depth].sprite_max_depth = 0;
		display_list[target_swf_depth].sprite_dl_capacity = 0;
		display_list[target_swf_depth].sprite_needs_init = 0;
		// CLONE_CLIP_EVENT_DISPATCH Phase 2/3: inherit clip_actions and mark
		// slot fully initialized — see ng_cloneSprite for rationale.
		display_list[target_swf_depth].sprite_initialized = 2;
		display_list[target_swf_depth].enterframe_eligible = 0;
		display_list[target_swf_depth].constructor_invoked = 0;
		// CLONE_CLIP_EVENT_DISPATCH Phase 6: see ng_cloneSprite for rationale.
		display_list[target_swf_depth].clone_replaced = 1;
		if (target_swf_depth > max_depth) max_depth = target_swf_depth;
	}
#endif

	// Clone is a sibling of the source → child of the source's parent (AVM1
	// clone semantics). src_mc->parent is guaranteed non-NULL by the guard above.
	MovieClip* clone_parent = src_mc->parent;
	MovieClip* clone_mc = actionFindOrCreateMovieClip(app_context, target_name, clone_parent);
	if (clone_mc == NULL) return NULL;
	clone_mc->as_created = 1;  /* AS-created: name-resolvable via cache scan (soft refs) */

	clone_mc->x       = src_mc->x;
	clone_mc->y       = src_mc->y;
	clone_mc->xscale  = src_mc->xscale;
	clone_mc->yscale  = src_mc->yscale;
	clone_mc->rotation = src_mc->rotation;
	clone_mc->alpha   = src_mc->alpha;
	// Clones are born visible (see ng_cloneSprite) — Ruffle's clone_sprite never
	// copies _visible, so a duplicate of a hidden clip is still shown.
	clone_mc->visible = 1;
	clone_mc->totalframes   = src_mc->totalframes;
	clone_mc->framesloaded  = src_mc->framesloaded;
	clone_mc->as_set_flags  = src_mc->as_set_flags;
	clone_mc->draw_has_bounds = src_mc->draw_has_bounds;
	clone_mc->draw_xmin = src_mc->draw_xmin;
	clone_mc->draw_xmax = src_mc->draw_xmax;
	clone_mc->draw_ymin = src_mc->draw_ymin;
	clone_mc->draw_ymax = src_mc->draw_ymax;
	// Inherit the source's TextSnapshot stale flag BEFORE marking the source.
	// See ng_cloneSprite for rationale (avm1/textsnapshot_available_text).
	clone_mc->ts_stale_source = src_mc->ts_stale_source;
	src_mc->ts_stale_source = 1;
	clone_mc->currentframe = 1;
	// Phase 2c: store the unbiased AS depth (computed above).
	clone_mc->depth = as_depth;

	// For dynamic textfield clones (src has ng_textfield_idx == -2 but no DefineEditText tag),
	// actionFindOrCreateMovieClip won't detect the textfield from the display list.
	// Initialize default textfield properties manually.
	if (src_mc->ng_textfield_idx == -2 && clone_mc->ng_textfield_idx != -2) {
		actionInitDynTextFieldClone(app_context, clone_mc);
		if (src_mc->dynamic_props != NULL && clone_mc->dynamic_props != NULL) {
			ASObject* src_props = (ASObject*) src_mc->dynamic_props;
			ASObject* clone_props = (ASObject*) clone_mc->dynamic_props;
			// Copy "type" property from source (preserved for clones)
			ActionVar* type_prop = getProperty(src_props, "type", 4);
			if (type_prop != NULL && type_prop->type != ACTION_STACK_VALUE_UNDEFINED) {
				setProperty(app_context, clone_props, "type", 4, type_prop);
			}
			// Copy original createTextField position to clone's properties
			ActionVar* cx_prop = getProperty(src_props, "_tf_createX", 11);
			ActionVar* cy_prop = getProperty(src_props, "_tf_createY", 11);
			if (cx_prop) setProperty(app_context, clone_props, "_tf_createX", 11, cx_prop);
			if (cy_prop) setProperty(app_context, clone_props, "_tf_createY", 11, cy_prop);
			// If source has active autoSize (not "none"), restore clone position
			// to original createTextField coordinates (source mc->x was modified by autoSize)
			ActionVar* as_prop = getProperty(src_props, "autoSize", 8);
			int has_autosize = 0;
			if (as_prop && as_prop->type == ACTION_STACK_VALUE_STRING && as_prop->str_size > 0) {
				// Check if autoSize != "none" by comparing UTF-16 directly
				const uint16_t* as_u16 = (const uint16_t*)(uintptr_t)as_prop->data.numeric_value;
				int is_none = (as_prop->str_size == 4 &&
					as_u16[0] == 'n' && as_u16[1] == 'o' &&
					as_u16[2] == 'n' && as_u16[3] == 'e');
				if (!is_none) has_autosize = 1;
			}
			if (has_autosize) {
				if (cx_prop && cx_prop->type == ACTION_STACK_VALUE_F64) {
					double ox; memcpy(&ox, &cx_prop->data.numeric_value, sizeof(double));
					clone_mc->x = (float)ox;
				}
				if (cy_prop && cy_prop->type == ACTION_STACK_VALUE_F64) {
					double oy; memcpy(&oy, &cy_prop->data.numeric_value, sizeof(double));
					clone_mc->y = (float)oy;
				}
			}
		}
	}

	// Both sprite and textfield clones are born visible now (see the visible=1
	// above); this is left as a defensive no-op for the textfield path.
	if (clone_mc->ng_textfield_idx >= 0 || clone_mc->ng_textfield_idx == -2) {
		clone_mc->visible = 1;
	}

	// CLONE_CLIP_EVENT_DISPATCH Phase 4: drain INIT/CONSTRUCT inline for the
	// clone slot. LOAD goes through the existing pending-load queue for
	// FIFO ordering with out-of-cap clones (see ng_cloneSprite). Gated on
	// NO_GRAPHICS||OFFSCREEN_RENDER because browser-WASM doesn't grow
	// display_list past 1024 — display_list[target_swf_depth] would read
	// out-of-bounds when target_swf_depth is in the 16384+ range.
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
	if (src_depth != SIZE_MAX && target_swf_depth < AVM_CLONE_SLOT_CAP &&
	    display_list[target_swf_depth].clip_action_count > 0)
	{
		ng_queue_placement_clip_events(app_context, target_swf_depth);
	}
#endif
	if (src_depth != SIZE_MAX && display_list[src_depth].clip_action_count > 0)
	{
		ng_queue_pending_load(target_name,
			display_list[src_depth].clip_actions,
			display_list[src_depth].clip_action_count);
	}

#if !defined(NO_GRAPHICS) && !defined(OFFSCREEN_RENDER)
	// Browser-WASM render path: a sprite clone needs its OWN standalone display_obj
	// (sprite content holder) populated by frame 0, because the child_mc_cache
	// render pass (render_attached_child) draws nothing when mc->display_obj == NULL.
	// ng_cloneSprite and ng_duplicateMovieClip already do this; ng_cloneSpriteFromMC
	// did not, so old-form duplicateMovieClip of a timeline clip routed here left the
	// clone with no renderable content. Minesweeper builds its 480-cell board with
	// duplicateMovieClip(cell, ...) → blank (pink) board. The global display_list
	// write above is skipped in browser-WASM, so display_obj is the only content path.
	{
		size_t src_cid = 0;
		if (src_depth != SIZE_MAX)
			src_cid = display_list[src_depth].char_id;
		else if (src_mc->display_obj != NULL)
			src_cid = ((DisplayObject*)src_mc->display_obj)->char_id;
		if (src_cid != 0 && dictionary[src_cid].type == CHAR_TYPE_SPRITE &&
		    clone_mc->display_obj == NULL)
		{
			frame_func* funcs = dictionary[src_cid].sprite_frame_funcs;
			size_t frame_count = dictionary[src_cid].sprite_frame_count;
			if (funcs != NULL && frame_count > 0 && funcs[0] != NULL)
			{
				DisplayObject* saved_dl  = display_list;
				size_t         saved_max = max_depth;
				size_t         saved_cap = display_list_capacity;
				DisplayObject* saved_sprite_obj = g_current_sprite_obj;
				int            saved_catch_up = catch_up_mode;

				DisplayObject* dobj = calloc(1, sizeof(DisplayObject));
				dobj->char_id = src_cid;
				dobj->sprite_dl_capacity = 64;
				// HCALLOC (not calloc): see ng_cloneSprite re-clone note above.
				dobj->sprite_display_list = HCALLOC(dobj->sprite_dl_capacity, sizeof(DisplayObject));
				dobj->sprite_max_depth = 0;
				clone_mc->display_obj = dobj;

				display_list = dobj->sprite_display_list;
				max_depth = dobj->sprite_max_depth;
				display_list_capacity = dobj->sprite_dl_capacity;

				MovieClip* saved_ctx = g_current_context;
				actionSetCurrentContext(clone_mc);
				g_current_sprite_obj = NULL;

				catch_up_mode = 1;
				funcs[0](app_context);
				catch_up_mode = saved_catch_up;

				actionSetCurrentContext(saved_ctx);
				g_current_sprite_obj = saved_sprite_obj;

				dobj->sprite_display_list = display_list;
				dobj->sprite_max_depth = max_depth;
				dobj->sprite_dl_capacity = display_list_capacity;

				display_list = saved_dl;
				max_depth = saved_max;
				display_list_capacity = saved_cap;
			}
		}
	}
#endif

	// Register under the clone's real parent (root → depth table + var_map;
	// non-root → parent->dynamic_props only).
	ng_register_clone_under_parent(app_context, clone_parent, target_name,
	                               clone_mc, swf_depth_for_table);

	return clone_mc;
}

MovieClip* ng_duplicateMovieClip(SWFAppContext* app_context, const char* source_name,
                                  const char* target_name, int as_depth, MovieClip* parent)
{
	if (!source_name || !target_name) return NULL;
	MovieClip* clone_parent = parent ? parent : &root_movieclip;

	int swf_depth = as_depth + 16384;

	size_t src_depth = ng_findDisplayEntryByName(source_name);
	if (src_depth != SIZE_MAX)
	{
		size_t cid = display_list[src_depth].char_id;
		int scriptable = (dictionary[cid].type == CHAR_TYPE_SPRITE) ||
		                 ng_find_button(cid) || (ng_find_textfield(cid) >= 0) ||
		                 ng_find_video(cid);
		if (!scriptable) return NULL;
		// Note: duplicateMovieClip does NOT fire onLoad for the clone (unlike CloneSprite).
	}

	MovieClip* src_mc = actionFindOrCreateMovieClip(app_context, source_name, clone_parent);
	MovieClip* clone_mc = actionFindOrCreateMovieClip(app_context, target_name, clone_parent);
	if (clone_mc == NULL) return NULL;
	clone_mc->as_created = 1;  /* AS-created: name-resolvable via cache scan (soft refs) */

	if (src_mc != NULL)
	{
		clone_mc->x       = src_mc->x;
		clone_mc->y       = src_mc->y;
		clone_mc->xscale  = src_mc->xscale;
		clone_mc->yscale  = src_mc->yscale;
		clone_mc->rotation = src_mc->rotation;
		clone_mc->alpha   = src_mc->alpha;
		// Clones are born visible. Ruffle's clone_sprite copies only the matrix
		// and color transform, never _visible — a duplicate of a clip the script
		// hid with _visible=false is still shown. Inheriting src visibility left
		// Minesweeper's 480 board-cell clones invisible (the game sets
		// cell._visible=false before the duplicateMovieClip loop) → blank board.
		clone_mc->visible = 1;
		clone_mc->totalframes   = src_mc->totalframes;
		clone_mc->framesloaded  = src_mc->framesloaded;
		clone_mc->as_set_flags  = src_mc->as_set_flags;
		// Drawing-API bounds — cloning a dynamically-built MC (createEmptyMovieClip
		// + lineTo/beginFill) must preserve the source's accumulated bounds, since
		// the clone is born empty and mcGetOriginalBounds would otherwise fall
		// through to the child-MC bounds walk (and the clone has no children).
		clone_mc->draw_has_bounds = src_mc->draw_has_bounds;
		clone_mc->draw_xmin = src_mc->draw_xmin;
		clone_mc->draw_xmax = src_mc->draw_xmax;
		clone_mc->draw_ymin = src_mc->draw_ymin;
		clone_mc->draw_ymax = src_mc->draw_ymax;
		// Inherit the source's TextSnapshot stale flag BEFORE marking the
		// source itself. Ruffle's clone_sprite via instantiate_by_id
		// produces an instance whose TextSnapshot availability tracks the
		// same state as its source, so a clone from an already-stale
		// source (one that has itself been used as a dup source) is born
		// stale — matches avm1/textsnapshot_available_text expectations.
		clone_mc->ts_stale_source = src_mc->ts_stale_source;
		// Mark the source as "used as dup source" so subsequent
		// `new TextSnapshot(src)` calls reject it.
		src_mc->ts_stale_source = 1;
	}
	clone_mc->currentframe = 1;
	clone_mc->depth = as_depth;

	// Register under the clone's real parent so name resolution finds it
	// (root → depth table + var_map; non-root → parent->dynamic_props only).
	ng_register_clone_under_parent(app_context, clone_parent, target_name,
	                               clone_mc, swf_depth);

	// If source is a sprite, run frame 0 to populate clone's sprite_display_list
	// (so children like text objects are available for TextSnapshot etc.)
	//
	// Source char_id: the global display_list only holds root-level entries, so a
	// NESTED timeline source (placed inside another sprite) misses the
	// ng_findDisplayEntryByName lookup above (src_depth==SIZE_MAX). Fall back to
	// the source MC's own DisplayObject, which carries char_id at any nesting
	// depth — without it the clone's frame 0 never runs and nested children
	// (e.g. `dup.leaf`) are missing. See gates/nested_timeline_clone.
	size_t src_cid = 0;
	if (src_depth != SIZE_MAX)
		src_cid = display_list[src_depth].char_id;
	else if (src_mc != NULL && src_mc->display_obj != NULL)
		src_cid = ((DisplayObject*)src_mc->display_obj)->char_id;
	{
		extern size_t display_list_capacity;
		size_t cid = src_cid;
		if (cid != 0 && dictionary[cid].type == CHAR_TYPE_SPRITE)
		{
			frame_func* funcs = dictionary[cid].sprite_frame_funcs;
			size_t frame_count = dictionary[cid].sprite_frame_count;
			if (funcs != NULL && frame_count > 0 && funcs[0] != NULL)
			{
				DisplayObject* saved_dl  = display_list;
				size_t         saved_max = max_depth;
				size_t         saved_cap = display_list_capacity;
				MovieClip*     saved_ctx = NULL;
				DisplayObject* saved_sprite_obj = g_current_sprite_obj;
				int            saved_catch_up = catch_up_mode;

				if (clone_mc->display_obj == NULL) {
					DisplayObject* dobj = calloc(1, sizeof(DisplayObject));
					dobj->char_id = cid;
					dobj->sprite_dl_capacity = 64;
					// HCALLOC (not calloc): growth paths realloc/FREE this buffer on
					// the custom heap (ng_spriteDLRealloc); a calloc'd buffer would
					// SIGABRT heap_free's ownership check.
					dobj->sprite_display_list = HCALLOC(dobj->sprite_dl_capacity, sizeof(DisplayObject));
					dobj->sprite_max_depth = 0;
					clone_mc->display_obj = dobj;
				}

				DisplayObject* dobj = (DisplayObject*)clone_mc->display_obj;
				display_list = dobj->sprite_display_list;
				max_depth = dobj->sprite_max_depth;
				display_list_capacity = dobj->sprite_dl_capacity;

				saved_ctx = g_current_context;
				actionSetCurrentContext(clone_mc);
				g_current_sprite_obj = NULL;

				// Phase 7b note: no eager bump (see ng_cloneSprite for rationale).
				catch_up_mode = 1;
				funcs[0](app_context);
				catch_up_mode = saved_catch_up;

				actionSetCurrentContext(saved_ctx);
				g_current_sprite_obj = saved_sprite_obj;

				dobj->sprite_display_list = display_list;
				dobj->sprite_max_depth = max_depth;
				dobj->sprite_dl_capacity = display_list_capacity;

				display_list = saved_dl;
				max_depth = saved_max;
				display_list_capacity = saved_cap;
			}
		}
	}

	return clone_mc;
}

// ---------------------------------------------------------------------------
// No-op stubs for functions not needed in NO_GRAPHICS mode
// ---------------------------------------------------------------------------

#ifdef NO_GRAPHICS
// In graphics modes, tag.c provides real
// implementations that call the renderer. These stubs are only needed in
// pure NO_GRAPHICS (no renderer at all).
void defineBitmap(const u8* data, size_t offset, size_t size, u32 width, u32 height, u16 char_id)
{
	// Register metadata so BitmapData.loadBitmap can resolve dimensions/pixels
	// even when the renderer is absent (NO_GRAPHICS builds).
	ng_registerBitmapMetadata(char_id, (data != NULL) ? data + offset : NULL,
	                          size, width, height);
}

void finalizeBitmaps(void)
{
}
#endif

// Sound tag stubs — real implementations live in audio.c for graphics
// builds. tag_stubs.c provides no-ops only when audio.c isn't linked
// (NO_GRAPHICS / HEADLESS without audio).
#ifdef NO_GRAPHICS
void tagDefineSound(SWFAppContext* app_context, u16 sound_id,
	u8 format, u8 rate, u8 sample_size, u8 stereo,
	u32 sample_count, const u8* data, size_t data_size)
{
	(void)app_context; (void)format; (void)sample_size; (void)stereo;
	(void)data; (void)data_size;
	// Store metadata for attachSound/getDuration
	ng_registerSoundMetadata(sound_id, rate, sample_count);
}

void tagStartSound(SWFAppContext* app_context, u16 sound_id,
	int stop, u32 loop_count, u32 in_point, u32 out_point)
{
	(void)app_context; (void)sound_id; (void)stop; (void)loop_count;
	(void)in_point; (void)out_point;
}

void tagSoundStreamHead(SWFAppContext* app_context,
	u8 format, u8 rate, u8 sample_size, u8 stereo,
	u16 avg_sample_count)
{
	(void)app_context; (void)format; (void)rate; (void)sample_size;
	(void)stereo; (void)avg_sample_count;
}

void tagSoundStreamBlock(SWFAppContext* app_context,
	const u8* data, size_t data_size)
{
	(void)app_context; (void)data; (void)data_size;
}

void tagStopAllSounds(SWFAppContext* app_context)
{
	(void)app_context;
}
#endif // sound stubs

void tagScriptLimits(u16 max_recursion, u16 timeout)
{
	(void)timeout;
	g_max_call_depth = max_recursion;
}
