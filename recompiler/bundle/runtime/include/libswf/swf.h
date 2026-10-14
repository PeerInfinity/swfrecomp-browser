#pragma once

#include <stackvalue.h>

// Forward declaration for o1heap
typedef struct O1HeapInstance O1HeapInstance;

#define HEAP_SIZE 1024*1024*1024  // 1 GB

// Forward declaration for SWFAppContext (needed for frame_func typedef)
typedef struct SWFAppContext SWFAppContext;

// Frame function now takes app_context parameter
typedef void (*frame_func)(SWFAppContext* app_context);

extern frame_func frame_funcs[];

// Button action: condition bitmask + function to call on matching transition
typedef struct ButtonAction {
	u16 condition;        // bitmask of triggering transitions
	frame_func action;    // function to call
} ButtonAction;

// Clip action: event bitmask + function to call on matching event
typedef struct ClipAction {
	u32 event_flags;      // bitmask of triggering events
	frame_func action;    // function to call
} ClipAction;

// Character type enum for shapes, text, sprites, and buttons
typedef enum
{
	CHAR_TYPE_SHAPE,
	CHAR_TYPE_MORPH_SHAPE,
	CHAR_TYPE_TEXT,
	CHAR_TYPE_SPRITE,
	CHAR_TYPE_BUTTON,
} CharacterType;

#define INITIAL_DICTIONARY_CAPACITY 8192  // Increased to support per-movie char_id offsetting (1000 per child SWF)
#define INITIAL_DISPLAYLIST_CAPACITY 1024
// Per-sprite child display lists are sized by occupancy, not by the depth
// range: they start small and are grown on demand via ng_spriteDLRealloc /
// ng_ensureDisplayListSize (tag.c), which rebase every aliased DisplayObject*
// after the buffer moves. Sizing every sprite at INITIAL_DISPLAYLIST_CAPACITY
// (1024 entries x ~432 B = ~440 KB each) exhausted the 1 GB o1heap arena on
// sprite-heavy games (Minesweeper board play held ~2000 live lists = ~1 GB).
#define INITIAL_SPRITE_DL_CAPACITY 64
// AVM1 SWF-depth bias: AS depth + AVM_DEPTH_BIAS = SWF depth (display_list slot index).
// Clones live in slots [AVM_DEPTH_BIAS, AVM_CLONE_SLOT_CAP); the cap is the upper
// bound on display_list slots reachable by CloneSprite/duplicateMovieClip. Beyond
// the cap, the runtime falls back to the child_mc_cache / clone_depth_table only
// (skip-DL path).
#define AVM_DEPTH_BIAS 16384
#define AVM_CLONE_SLOT_CAP 32768

// Clip event flag bits (SWF spec)
#define CLIP_EVENT_LOAD         0x00001
#define CLIP_EVENT_ENTER_FRAME  0x00002
#define CLIP_EVENT_UNLOAD       0x00004
#define CLIP_EVENT_MOUSE_MOVE   0x00008
#define CLIP_EVENT_MOUSE_DOWN   0x00010
#define CLIP_EVENT_MOUSE_UP     0x00020
#define CLIP_EVENT_KEY_DOWN     0x00040
#define CLIP_EVENT_KEY_UP       0x00080
#define CLIP_EVENT_DATA         0x00100
#define CLIP_EVENT_INITIALIZE   0x00200
#define CLIP_EVENT_PRESS        0x00400
#define CLIP_EVENT_RELEASE      0x00800
#define CLIP_EVENT_RELEASE_OUTSIDE 0x01000
#define CLIP_EVENT_ROLL_OVER    0x02000
#define CLIP_EVENT_ROLL_OUT     0x04000
#define CLIP_EVENT_DRAG_OVER    0x08000
#define CLIP_EVENT_DRAG_OUT     0x10000
#define CLIP_EVENT_KEY_PRESS    0x20000
#define CLIP_EVENT_CONSTRUCT    0x40000

typedef struct Character
{
	CharacterType type;
	union
	{
		// DefineShape
		struct
		{
			size_t shape_offset;
			size_t size;
		};
		// DefineMorphShape
		struct
		{
			size_t morph_start_offset;
			size_t morph_start_size;
			size_t morph_end_offset;
			size_t morph_color_start;
			size_t morph_color_count;
			// Base of this character's run in morph_end_color_data. The START
			// colours are indexed by morph_color_start, but the END colours are
			// a SEPARATE table with its own per-character base, so they need
			// their own offset — without it every morph character reads the
			// FIRST morph character's end colours.
			size_t morph_end_color_start;
		};
		// DefineText
		struct
		{
			size_t text_start;
			size_t text_size;
			u32 transform_start;
			u32 cxform_id;
		};
		// DefineSprite
		struct
		{
			frame_func* sprite_frame_funcs;
			size_t sprite_frame_count;
			size_t sprite_byte_size;
			// 0 only for a malformed DefineSprite whose body is not
			// terminated by an End record. Such a clip does NOT loop —
			// it stops on its last frame. Mirrors Ruffle's
			// `preload_progress.has_end_tag` / `determine_next_frame`
			// (core/src/display_object/movie_clip.rs): "Clips without an
			// End tag should not loop, even if they have multiple
			// frames." Well-formed authoring tools always emit the End
			// record, so this is 1 for every real-world sprite.
			u8 sprite_has_end_tag;
			// Ruffle's `frames_loaded` (movie_clip.rs:3314 —
			// `cur_preload_frame - 1`): the number of ShowFrame records in
			// the DefineSprite body, floored at 1 because `preload()`
			// treats the end of a ShowFrame-less clip as one ShowFrame.
			// This is what decides whether the clip loops; the DefineSprite
			// HEADER count decides nothing about playback.
			// `sprite_frame_count` above is the number of frame FUNCTIONS
			// the recompiler generated, which is `sprite_loaded_frames + 1`
			// when tags trail the last ShowFrame — Ruffle runs those exactly
			// once through run_frame_internal's NextFrame::Same fall-through.
			size_t sprite_loaded_frames;
			// The DefineSprite header's frameCount field, verbatim. Only the
			// AS-visible _totalframes / _framesloaded read this; it may
			// disagree with both counts above (Flash reports it unchanged —
			// see avm2_display.c total_frames() for the AVM2 twin).
			size_t sprite_declared_frames;
		};
		// DefineButton
		struct
		{
			frame_func* button_state_funcs;  // [up, over, down]
			size_t button_hit_char_id;       // character ID for hit-test shape
			u32 button_hit_transform_id;     // transform for hit-test shape
			ButtonAction* button_actions;    // condition→action pairs
			size_t button_action_count;
		};
	};
} Character;

typedef struct DisplayObject
{
	size_t char_id;
	u32 transform_id;
	u32 cxform_id;
	u32 has_cxform;
	u16 clip_depth;
	u16 ratio;
	u8 button_state;       // 0=idle, 1=over, 2=down, 3=outDown (CHAR_TYPE_BUTTON)
	u8 button_prev_state;  // previous frame's state (for transition detection)
	u8 sticky_button_state; // preserved state across remove+re-place (same char)
	size_t sticky_char_id;  // char_id that sticky_button_state belongs to
	u8 blend_mode;         // 0=normal (default), see SWF spec blend modes
	// Per-sprite persistent display list (for multi-frame sprites).
	// ALIASING RULE: this buffer can MOVE (ng_spriteDLRealloc /
	// ng_ensureDisplayListSize grow it on demand and FREE the old buffer).
	// Any persistent holder of a pointer INTO the buffer (&dl[i], e.g.
	// MovieClip.display_obj, queued ctor/script payloads) or of the buffer
	// BASE itself (copies of this field in other entries or globals) must be
	// registered in tag.c ng_spriteDLRealloc's rebase walk, or it dangles on
	// the next grow. C-stack locals held across a call that can grow a list
	// (scripts, attachMovie, placement replays) must be re-read after the
	// call instead.
	// FREE-SIDE: ng_freeSpriteDL is the ONLY way to free this buffer; it
	// NULLs the same holder classes (scrub_mc_display_obj_in_range also
	// clears base-pointer copies of the field in standalone attachMovie /
	// clone display_objs). Every free site must NULL the entry field it
	// freed through — the buffer is aliased by the CI-mode attachMovie
	// registration entry AND the attached clip's own standalone display_obj,
	// and freeing through one alias must not leave the other dangling.
	struct DisplayObject* sprite_display_list;
	size_t sprite_max_depth;
	size_t sprite_dl_capacity;
	size_t sprite_current_frame;
	u8 sprite_is_playing;         // 0=stopped, 1=playing (default 1)
	u8 goto_play_active;          // browser-WASM: set by ng_gotoFrameByMC(play=1); gates advance_attached_clip_natural so ONLY explicitly gotoAndPlay'd standalone attached clips auto-advance (coins' COLLECTED, drones' prefire). Cleared by gotoAndStop / one-shot completion.
	u8 natural_oneshot;          // browser-WASM: set at attach for auto-play one-shot particle clips (ParticleManager "pfx*"); advance_attached_clip_natural plays them forward (placement tags) and REMOVES them on wrap (mimics their last-frame removeMovieClip). Fixes Metanet N leftover laser/death lines + the particle-overdraw fps cost (both gone when the stuck particle self-removes). See [[n-laser-is-frozen-particle-sprite-not-drawing-api]].
	int sprite_manual_next_frame;  // pending manual frame nav
	size_t sprite_next_frame;      // target frame
	char* instance_name;           // from PlaceObject2 HasName (or NULL)
	u8 instance_name_owned;        // 1 if instance_name was malloc'd (auto-assigned), 0 if pointer to static string
	// Clip actions (PlaceObject2 HasClipActions)
	ClipAction* clip_actions;
	size_t clip_action_count;
	// Accumulated clip actions from a prior Remove that was immediately followed by a Re-place
	// at the same depth in the same frame. Fired before clip_actions on the next removal.
	ClipAction* accumulated_clip_actions;
	size_t accumulated_clip_action_count;
	// Visual filter (PlaceObject3 FilterList)
	u8 filter_type;       // 0=none, 1=blur, 2=drop_shadow, 3=glow, 4=bevel
	u8 filter_quality;    // blur passes (1-3)
	u8 filter_flags;      // inner/knockout/compositeSource
	double filter_blur_x;
	double filter_blur_y;
	double filter_color_r;
	double filter_color_g;
	double filter_color_b;
	double filter_color_a;
	double filter_strength;
	double filter_angle;
	double filter_distance;
	// Bevel highlight color (shadow color uses filter_color_*)
	double filter_highlight_r;
	double filter_highlight_g;
	double filter_highlight_b;
	double filter_highlight_a;
	// Scriptable color transform override (mutable at runtime by ActionScript)
	double cx_ra, cx_ga, cx_ba, cx_aa;  // multipliers (percentage: 100.0 = normal)
	double cx_rb, cx_gb, cx_bb, cx_ab;  // addends (0..255 range, 0 = normal)
	int cx_overridden;                   // 1 if cx_* fields override cxform_data[]
	// Per-INSTANCE base GPU transform slot for this entry's composed static-text
	// glyphs (CHAR_TYPE_TEXT). 0 = none → the draw uses the CHARACTER-shared
	// ch->transform_start. compose_children allocates ch->text_size dynamic slots
	// per text instance so multiple instances of the same DefineText (e.g. all
	// Minesweeper "1" cells) don't overwrite each other's glyph transforms.
	u32 text_glyph_xform_base;
	// Timeline tracking
	u8 sprite_needs_init;   // 1 if frame_0 needs to run this tick (NO_GRAPHICS)
	u8 depth_swapped;       // 1 if moved here by swapDepths (skip timeline modifies)
	u8 transformed_by_script; // 1 if AS-level setter wrote a transform attribute (matrix), Ruffle DisplayObjectFlags::TRANSFORMED_BY_SCRIPT
	u8 name_overridden;     // 1 if instance name was overridden (script _name= or modify-branch rename)
	size_t placed_at_frame; // frame index when this object was placed
	size_t placed_at_tick;  // value of g_tick_count when this object was placed (new-entry path only).
	                        // Used by advance_sprite_frames to skip newly-placed sprites in the same tick,
	                        // matching Ruffle's clip_exec_list iteration which never visits clips added
	                        // mid-iter (see SWFRecompDocs/plans/defer-newly-placed-sprite-advance-plan.md).
	size_t place_gen;       // monotonic generation counter for same-frame detection
	size_t place_seq;       // monotonic per-placement counter (NOT per-frame).
	                        // Reverse order = most-recently-placed first =
	                        // reverse-instantiation iteration order
	                        // (Ruffle's clip_exec_list LIFO equivalent).
	// Clip event interaction state
	u8 clip_mc_pressed;     // 1 if CLIP_EVENT_PRESS was fired for this clip (awaiting RELEASE/RELEASE_OUTSIDE)
	u8 clip_mouse_inside;   // 1 if mouse is currently inside this clip's hit area (for ROLL_OVER/ROLL_OUT/DRAG_OVER/DRAG_OUT transitions)
	u8 enterframe_eligible; // 1 if AS2 onEnterFrame should fire this tick (set by init/advance, cleared after dispatch)
	// Per-tick generation stamp for set_enterframe_eligible_recursive pruning:
	// == g_tick_count iff this entry is on the display-tree path to an MC with an
	// onEnterFrame handler this tick (stamped by tag.c stamp_onenterframe_paths).
	// When != g_tick_count the walk skips this subtree — its enterframe_eligible
	// flag would be consumed by nothing (onEnterFrame dispatch is the sole reader).
	size_t subtree_ef_gen;
	u8 constructor_invoked; // 1 if registered class constructor was already invoked during eager init
	u8 sprite_initialized;  // 0=not init, 1=init'd this tick, 2=init'd on previous tick (for per-tick EnterFrame gating)
	u8 clone_replaced;      // 1 if slot was overwritten by an AS clone (CloneSprite / duplicateMovieClip).
	                        // Mirrors Ruffle's `avm1_clone_target.is_some()` — survives_rewind returns false
	                        // so backward goto removes the clone and freshly re-places the static MC.
	u8 pending_remove;      // browser-WASM only: tagRemoveObject(2) deferred the
	                        // immediate invalidate+clear so a same-tick Place at the
	                        // same depth can reclaim the cached MC instead of leaking
	                        // a fresh one each frame_func re-run. Cleared by
	                        // tagPlaceObject2/Ratio (reclaim path) or finalized by
	                        // tagShowFrame fallback. See browser_wasm_frame_func_rerun
	                        // auto-memory.
	u8 as_hidden;           // 1 if AS set _visible=false on the MC linked to this entry
	                        // (synced from actionSetProperty via mc->display_obj). Zero-init
	                        // = visible. The render loop skips drawing entries with this set;
	                        // a name lookup from root can't find a timeline sprite's MC, but
	                        // mc->display_obj points straight back at the display_list entry.
	// Per-frame-walk resolution cache: memoizes the MovieClip* that the hot
	// per-frame tree walks (advance/presync/button) resolve this display entry
	// to by string name. The underlying resolvers do an O(child_mc_count) scan
	// with swf_name_match per call; this collapses a hit to a single
	// swf_name_match + liveness check. Validated against the SAME predicate each
	// resolver keys on (see tag.c tag_cached_walk_mc); a stale entry falls
	// through to the real resolver and is re-cached. void* to avoid pulling the
	// MovieClip definition into this header.
	void* resolved_mc;
	// The loaded-movie HOLDER whose frame tag created this entry, or NULL when
	// the main movie placed it. `MovieClip*`, kept as void* so this header does
	// not pull in action.h; MovieClip structs are immortal (tombstoned at
	// depth == INT_MIN, never freed) so the pointer can never dangle.
	//
	// This is the PLACING movie, not the DEFINING one — the same distinction
	// `place_transform_data` below draws, and the opposite of
	// `child_transform_data`. It answers exactly one question: "when this loaded
	// movie's timeline wraps back to its frame 1, which display entries did it
	// put there and must therefore take away?" A movie loaded twice into two
	// different holders has two identities here and one `movie_id`, which is why
	// this records the holder and not `g_current_movie_id`.
	void* placed_by_holder;
	// Cached transform values (populated at placement time for correct bounds on child SWFs)
	float place_a, place_b, place_c, place_d, place_tx, place_ty;
	// The transform table `transform_id` indexes — i.e. the table belonging to
	// the movie whose TAG performed this placement, recorded by
	// ng_cache_transform from g_active_transform_data. NULL = never cached, use
	// the main movie's `transform_data`.
	//
	// This is NOT `child_transform_data` below: that one is keyed on the movie
	// that DEFINED the character (so a sprite's own frame funcs run against the
	// right table), and the two disagree exactly when a parent's tag places a
	// character imported from a child (tagImportCharacter / ng_shared.c copies
	// the child's movie id onto the local char id, but the tid is the PARENT's).
	// Indexing the wrong table is an out-of-bounds read, not a wrong number:
	// a parent with no timeline content has `float transform_data[1][16]`, so a
	// loaded child's tid 1 reads past the end and `_x` returns a different
	// garbage float every run.
	float (*place_transform_data)[16];
	// Child movie transform data override (set during placement when g_active_transform_data != NULL)
	float (*child_transform_data)[16];
	// DisplayObject.opaqueBackground (s16 P2). Sources: PlaceObject3's
	// BackgroundColor field (via tagSetOpaqueBackground) and AVM1
	// `mc.opaqueBackground = 0xRRGGBB`. Ruffle forces alpha to 255 and drops
	// a PlaceObject3 colour whose alpha is 0, so only the RGB is stored.
	// The renderer paints a filled box of the entry's bounds UNDER it.
	u8 opaque_bg_set;
	u32 opaque_bg_rgb;
	// DisplayObject.cacheAsBitmap (s17). Sources: PlaceObject3's BitmapCache
	// field (via tagSetCacheAsBitmap) and AVM1 `mc.cacheAsBitmap = true`.
	// The renderer does not actually rasterise into an offscreen texture; the
	// one OBSERVABLE consequence it reproduces is Ruffle's
	// `PixelSnapping::Always` on the cache blit (render/src/bitmap.rs:89) —
	// the object's world matrix is translated so its render bounds' top-left
	// lands on a whole pixel. See tag.c cab_pixel_snap.
	u8 cache_as_bitmap;
	// PlaceObject3 SurfaceFilterList, the WHOLE chain (s18 w2-gfx-filter-chain).
	// The scalar filter_* fields above carry only the FIRST filter of the list —
	// that is all the recompiler ever emitted through tagSetFilter, and the
	// renderer applied exactly one filter, silently dropping the rest
	// (visual/filters/blur_size_grows is [Blur, Glow] and lost the blue glow;
	// from_shumway/acid/acid-filter-2 is [Glow, Blur], acid-filter is
	// [Blur, Blur]). tagBeginFilterList already recorded the full list for the
	// `mc.filters` reflection; this links that record to the display entry so
	// render_filtered_object can run the chain in order.
	//
	// `const FilterListData*`, kept as void* so this header does not pull in
	// tag.h. It points into a file-static table in tag.c that is never freed, so
	// it cannot dangle; it is cleared wherever filter_type is cleared (the two
	// placement arms), and re-set by the tagBeginFilterList that follows the
	// tagSetFilter of the same placement.
	const void* filter_chain;

	// 1 on the STANDALONE heap DisplayObject that ng_attachMovie allocates as an
	// attachMovie'd clip's display_obj. That struct lives in no display-list
	// array (root attaches are never registered; a non-root attach's parent
	// registration is a separate copy that is never sprite_initialized), so the
	// root enterFrame walk cannot reach the clip's children. The CI modes
	// (NO_GRAPHICS / OFFSCREEN_RENDER) walk flagged lists explicitly: promotion
	// (ng_upgrade_attached_standalone_initialized), CLIP_EVENT_ENTER_FRAME
	// dispatch and hasClipEnterFrameHandlers. Clones' standalone structs are
	// deliberately NOT flagged — their root registration entry shares the child
	// list and is already walked (flagging them would double-fire).
	u8 attach_standalone;
} DisplayObject;

typedef struct KeyState {
	uint8_t down[256];     // 1 if key currently held (indexed by ASCII/keyCode)
	uint8_t toggled[256];  // toggle state for lock keys (CapsLock=20, NumLock=144, ScrollLock=145)
	// Edge latches set from DOM event callbacks (render_webgpu.c on_key_down /
	// on_key_up). swf.c's per-tick transition loop also fires on these so a
	// keydown+keyup pair that collapses inside a single 60Hz tick still
	// dispatches keyPress / KeyDown handlers (otherwise fast Playwright/JS
	// keys are silently lost). Cleared by swf.c after firing.
	uint8_t edge_down[256];
	uint8_t edge_up[256];
	int last_key_down;     // keyCode of most recently pressed key (-1 if none)
	int last_key_ascii;    // ASCII value of last key press (for Key.getAscii())
} KeyState;

typedef struct MouseState {
	float stage_x;      // Mouse X in twips (stage coordinates)
	float stage_y;      // Mouse Y in twips
	int button_down;    // 1 if left mouse button is held
	int clicked;        // 1 if button was pressed this frame (edge)
	int released;       // 1 if button was released this frame (edge)
	int moved;          // 1 if mouse moved this frame (edge)
} MouseState;

// Macros for stack access via app_context
#define STACK (app_context->stack)
#define SP (app_context->sp)
#define OLDSP (app_context->oldSP)

typedef struct SWFAppContext
{
	// Stack management (moved from globals)
	char* stack;
	u32 sp;
	u32 oldSP;

	frame_func* frame_funcs;
	size_t frame_count;  // Local addition - kept for compatibility
	u16 fps;

	// Shape/transform data (available in all modes for hit testing)
	char* shape_data;
	size_t shape_data_size;
	char* transform_data;
	size_t transform_data_size;

	// Font/static-text data. Available in all modes, not just graphics: the
	// CPU glyph hit tester (shape_hit_test.c) and TextSnapshot
	// (actionmodern/action.c) both read them, and ng_buildMovieRenderTables
	// needs the ROOT's row counts in every mode to size the combined tables.
	u32* glyph_data;            // u32[glyph_count * 4][1]
	size_t glyph_data_size;
	u32* text_data;             // u32[text_count]
	size_t text_data_size;
	u16* text_char_codes;       // u16[text_count], parallel to text_data
	size_t text_char_codes_size;

	// Fill-style and morph END tables. Available in all modes for the same
	// reason the static-text tables above are: the AVM2 CPU raster
	// (avm2_cpu_raster.c, reached from BitmapData.draw and the CPU frame dump)
	// and the AVM2 exact hit test read them in NO_GRAPHICS too, and
	// ng_buildMovieRenderTables needs the ROOT's row counts in every mode to
	// size the combined tables a loaded child's indices are re-based onto.
	// (cxform_data and bitmap_data stay graphics-only: nothing outside the
	// renderer indexes them, so combining them there would give a nonzero base
	// with no combined array behind it.)
	char* color_data;
	size_t color_data_size;
	char* uninv_mat_data;
	size_t uninv_mat_data_size;
	char* gradient_data;
	size_t gradient_data_size;
	char* morph_end_shape_data;
	size_t morph_end_shape_data_size;
	char* morph_end_color_data;
	size_t morph_end_color_data_size;

#ifndef NO_GRAPHICS
	// Stage size, in *stage* pixels (the SWF header frame size). Anything that
	// reasons in stage coordinates — tag.c's drop-shadow/bevel NDC offsets, the
	// AVM2 dynamic-bitmap budget — must keep reading these.
	int width;
	int height;

	// Render-target size, in device pixels. Ruffle renders a test at its
	// [player_options] viewport_dimensions, which need not match the movie box;
	// the offscreen colour target, the readback buffer and the captured PNG are
	// all sized off these. 0 means "not set" -> fall back to width/height.
	int render_width;
	int render_height;
	// StageScaleMode::ShowAll fit: stage_scale is the uniform min-fit factor
	// from stage px to render px; stage_fit_{x,y} are the fraction of the render
	// target the fitted content covers on each axis (1.0 on the axis that fits
	// exactly, < 1.0 on the letterboxed one). 0 means "not set" -> 1.0f.
	float stage_scale;
	float stage_fit_x;
	float stage_fit_y;

	const float* stage_to_ndc;

	size_t bitmap_count;
	size_t bitmap_highest_w;
	size_t bitmap_highest_h;

	char* bitmap_data;
	size_t bitmap_data_size;

	char* cxform_data;
	size_t cxform_data_size;

	void* audio_ctx;  // AudioContext* (opaque to avoid header dependency)
#endif

	// Input state (works in both graphics and NO_GRAPHICS modes)
	MouseState mouse;
	KeyState keys;

	// Heap management fields
	O1HeapInstance* heap_instance;
	char* heap;
	size_t heap_size;
	size_t heap_full_size;
	size_t heap_current_size;
	int heap_inited;

	// String ID support (from upstream)
	size_t max_string_id;

	// UTF-16 string cache (indexed by string_id, lazily populated)
	uint16_t** str_cache;
	u32* str_cache_len;
} SWFAppContext;

#ifndef DYNAMIC_GUEST
extern int quit_swf;
#endif
extern int is_playing;
extern size_t current_frame;
extern size_t next_frame;
extern int manual_next_frame;

// Global frame access for ActionCall opcode
extern frame_func* g_frame_funcs;
extern size_t g_frame_count;

// Drag state tracking (works in both graphics and NO_GRAPHICS modes)
extern int is_dragging;         // 1 if a sprite is being dragged, 0 otherwise
extern char* dragged_target;    // Name of the target being dragged (or NULL)
// Virtual drag position: registration point of dragged/last-dragged clip (twips).
// Updated on mouse move while dragging; persists after stopDrag for PRESS hit-testing.
extern float g_drag_virt_x;
extern float g_drag_virt_y;
// Name of the most recently dragged clip (persists after stopDrag for PRESS hit-testing).
extern char g_drag_target_name[256];

extern Character* dictionary;

extern DisplayObject* display_list;
extern size_t max_depth;

// Movie entry for pre-compiled child SWFs (multi-SWF / loadMovie support)
typedef struct MovieEntry {
	const char* filename;              // "target.swf"
	frame_func* frame_funcs;           // child's frame function array
	void (*init_func)(SWFAppContext*); // child's tagInit function
	u16 swf_version;
	u16 frame_count;
	u16 stage_width;
	u16 stage_height;
	u32 file_size;                     // SWF file size in bytes (for onLoadProgress)
	u8 movie_id;                       // 0 = main SWF, 1+ = child SWFs (for per-movie export isolation)
	u8 is_prelude;                     // 1 = prelude SWF (runs before main SWF, shares scope)
	float (*transform_data_ptr)[16];   // pointer to child SWF's transform_data (NULL = use main SWF's)
	// AVM2 child SWF (loader-arc tranche 6). All of these are zero-init-safe:
	// NULL/0 means "AVM1 child, image shell, or self-load", which is every
	// entry the generator produced before tranche 6.
	const void* avm2_tables;           // const Avm2MovieTables* (avm2_abc.h)
	const u8* swf_bytes;               // DECOMPRESSED movie image
	u32 swf_bytes_len;                 // = the header's declared file length
	// The file EXACTLY as it sits on disk, compression and all. This is what a
	// URLLoader fetch of the .swf hands back (Flash does not decompress for a
	// binary read), and therefore what a later loadBytes() sees — so its length
	// is `file_size`, not `swf_bytes_len`.
	const u8* raw_bytes;
	u32 raw_bytes_len;
	// ---- Per-movie RENDER tables (multi-SWF render slice) -----------------
	// Every geometry and style index the recompiler emits is numbered from 0
	// in the movie that emitted it, so a loaded child's indices mean nothing
	// against the root's arrays: its shapes drew the root's triangles and its
	// fills sampled the root's colours. ng_buildMovieRenderTables()
	// concatenates all movies' arrays into one set of combined tables -- the
	// ROOT FIRST, so a build with no child movies skips the whole pass and a
	// build with them keeps every root index unchanged -- and fills in the
	// *_base fields below.
	//
	// transform_data and cxform_data are combined too, but their ids are
	// re-based at PLACEMENT time rather than at define time -- a placement id
	// comes from the placing movie's tag, not from a character. ng_cache_transform
	// is the single funnel every one of the 14 tagPlaceObject* call sites goes
	// through, so re-basing there makes obj->transform_id a COMBINED-table index
	// everywhere and leaves all ~45 of its readers untouched.
	//
	// All zero-init safe: a NULL table contributes nothing and leaves its base
	// at 0, which is what every entry the generator produced before this slice
	// carries.
	const u32*   shape_data_ptr;      // u32[shape_vert_count][4]
	size_t       shape_vert_count;
	const float* color_data_ptr;      // float[color_count][4]
	size_t       color_count;
	const float* uninv_mat_data_ptr;  // float[uninv_mat_count * 16]
	size_t       uninv_mat_count;
	const u8*    gradient_data_ptr;   // u8[gradient_count * 256][4]
	size_t       gradient_count;      // ramps, 256 RGBA rows each
	// path_data is NOT combined: it is read only by the CPU hit tester, so the
	// movie's own table is recorded per CHARACTER at ng_record_char_path time
	// (the same shape as place_transform_data) instead.
	const float* path_data_ptr;       // float[path_count][3]
	size_t       path_count;
	size_t       transform_count;     // rows in transform_data_ptr
	const float* cxform_data_ptr;     // float[cxform_count * 20]
	size_t       cxform_count;
	const u8*    bitmap_data_ptr;     // this movie's raw pixel array
	const u32*   bitmap_descs_ptr;    // u32[bitmap_count][4] {offset,size,w,h}
	size_t       bitmap_count;
	// Static text (DefineText/DefineText2). text_data holds GLOBAL glyph
	// indices into glyph_data, and glyph_data's rows hold a vertex offset into
	// shape_data and a path offset into path_data -- so combining these two
	// re-writes their CONTENTS as well as concatenating them (the same shape as
	// a vertex's style word). text_char_codes is parallel to text_data.
	const u32*   text_data_ptr;       // u32[text_count]
	const u16*   text_char_codes_ptr; // u16[text_count] (may be NULL)
	size_t       text_count;
	const u32*   glyph_data_ptr;      // u32[glyph_count * 4][1]
	size_t       glyph_count;         // GLYPHS, 4 rows each
	// Morph shapes (DefineMorphShape/2). The END vertex and END colour tables;
	// the START halves live in shape_data / color_data and are already covered.
	const float* morph_end_shape_data_ptr;  // float[morph_end_vert_count][2]
	size_t       morph_end_vert_count;
	const float* morph_end_color_data_ptr;  // float[morph_end_color_count][4]
	size_t       morph_end_color_count;
	// Bases into the combined tables, assigned by ng_buildMovieRenderTables.
	// gradient_base and uninv_mat_base are always EQUAL: a vertex's style word
	// carries ONE index that the shader uses as both a gradient ramp row and an
	// inverse-matrix slot, so the two arrays are padded to a shared per-movie
	// stride rather than packed independently.
	u32 shape_vert_base;
	u32 color_base;
	u32 gradient_base;
	u32 uninv_mat_base;
	u32 bitmap_base;
	u32 transform_base;
	u32 cxform_base;
	u32 text_base;
	u32 glyph_base;
	u32 morph_end_vert_base;
	u32 morph_end_color_base;
} MovieEntry;

// Find a pre-compiled movie entry by filename (defined in movie_registry.c when HAS_CHILD_MOVIES)
MovieEntry* findMovieEntry(const char* filename);

// Iterate prelude movie entries (returns NULL when exhausted; call with idx=0,1,2,...)
MovieEntry* getPreludeEntry(int idx);

// Iterate ALL movie entries (returns NULL when exhausted). Used by
// Loader.loadBytes, which has a byte payload and no filename to key on
// (loader-arc tranche 6).
MovieEntry* getMovieEntryAt(int idx);

// Data file entry for loadVariables pre-bundled data
typedef struct DataFileEntry {
	const char* filename;
	const char* content;
	int content_length;
} DataFileEntry;

// Find a pre-bundled data file by filename (defined in data_registry.c when HAS_DATA_FILES)
DataFileEntry* findDataFile(const char* filename);

// Scan bundled data files for the largest image (GIF/JPEG/PNG/BMP) dimensions.
// Returns 0 in both outputs when no data files are linked or none are images.
// Defined in data_registry.c when HAS_DATA_FILES; stubbed otherwise.
// Used by the renderer to size the dynamic bitmap texture array for
// MovieClipLoader.loadClip("foo.{gif,jpg,png}") image loads.
void getDataFilesMaxImageDims(int* out_w, int* out_h);

void swfStart(SWFAppContext* app_context);