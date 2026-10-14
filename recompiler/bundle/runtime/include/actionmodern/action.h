#pragma once

#include <swf.h>
#include <variables.h>
#include <stackvalue.h>
#include <setjmp.h>

// SWF version — set at startup from constants.h, controls runtime behavior
extern int g_swf_version;
// Recursion depth limit (set by tagScriptLimits, default 256)
extern u32 g_max_call_depth;
// Execution halt flag — set when recursion limit is hit, prevents all further script execution
extern u8 g_execution_halted;

// Forward declarations
typedef struct MovieClip MovieClip;

// MovieClip structure for Flash movie clip properties
// Phase C: a registered binding between a container MovieClip's property and
// a TextField that should mirror its value. Mirrors Ruffle's
// `Avm1TextFieldBinding { text_field, variable_name }`
// (core/src/display_object.rs:3015). Owned by the *container* MC, not the TF.
typedef struct Avm1TextFieldBinding {
	MovieClip* text_field;
	char* variable_name;  // heap-allocated; freed on unregister.
} Avm1TextFieldBinding;

struct MovieClip {
	float x, y;
	float xscale, yscale;
	float rotation;
	float alpha;
	float width, height;
	int visible;
	int currentframe;
	int totalframes;
	int framesloaded;
	char name[256];
	char target[256];
	char original_target[256]; // Target path from creation time (never updated by _name changes, used for removed MC toString)
	char droptarget[256];
	char url[512];
	// SWF 4+ properties
	float highquality;     // Property 16: _highquality (0, 1, or 2)
	float focusrect;       // Property 17: _focusrect (0 or 1)
	float soundbuftime;    // Property 18: _soundbuftime (in seconds)
	char quality[16];      // Property 19: _quality ("LOW", "MEDIUM", "HIGH", "BEST")
	float xmouse;
	float ymouse;
	MovieClip* parent;     // Parent MovieClip (_root has NULL parent)
	void* dynamic_props;   // ASObject* for user-defined properties (lazily allocated)
	u8 lockroot;           // _lockroot property (0 = false, 1 = true)
	u8 blend_mode;         // blendMode as integer (0=default→"normal", 1="normal", 2="layer", ...14="hardlight")
	u8 is_button_mc;       // 1 if this MC represents a SWF button (affects _parent resolution in SWF5)
	int depth;             // ActionScript display depth (-16384 for _root, SWF_depth-16384 for timeline clips, AS-space for dynamic clips)
	u8 depth_swapped;      // 1 if depth was changed by swapDepths (prevents display list overwrite in actionGetMember)
	u8 unloaded;           // 1 if unloadMovie was called on this MC (frame/bytes properties return 0)
	u8 load_failed;        // 1 if loadMovie was called but the URL was not found (_framesloaded/-1, getBytesTotal/-1, getSWFVersion/-1)
	u8 pending_removal;    // 1 if MC was removed from display list but persists for one more frame (depth transformed)
	u8 avm1_removed;       // 1 if MC was removed from display list (halts script execution)
	u8 name_displaced;     // SWAPDEPTHS_REWIND_UNBLOCK Phase 4: 1 if a fresh placement during a backward goto created a new MC at this MC's tag-defined depth while this MC was at a swap-target depth. Skip in findOrCreateMovieClip name lookups so AS variable resolution gets the fresh MC; direct pointer access (saved refs, getInstanceAtDepth) still works.
	u8 as_created;         // 1 for clips created by AS (createEmptyMovieClip/attachMovie/duplicateMovieClip). These have no display-list entry, so name lookups may resolve them via a child_mc_cache scan; lookup-minted MCs (findOrCreateMovieClip misses) stay 0 and are never name-resolved that way.
	u32 byte_size;         // getBytesLoaded/getBytesTotal value (0 = dynamic/attached clip)
	u16 swf_version;       // SWF version of the movie loaded into this MC (for getSWFVersion)
	void* display_obj;     // Pointer to this MC's DisplayObject entry (for direct child lookup without global display_list)
	u32 last_transform_id; // Last synced transform_id (for _x/_y from display list)
	// Per-tick dynamic GPU transform slot for dynamic (createEmptyMovieClip /
	// duplicateMovieClip / attachMovie-without-placement) MCs that have AS-set
	// transforms. Allocated by tag.c each tick when mc->as_set_flags != 0 and
	// mc->display_obj == NULL; read by fillDrawingInfos to route the MC's
	// Drawing-API paths through the freshly built GPU matrix instead of
	// last_transform_id (which would otherwise resolve to slot 0 / identity for
	// a dynamic MC). 0 means "no override — use last_transform_id".
	u32 dynamic_xform_slot;
	u8 as_set_flags;       // Bitmask: bit 0 = _x set by AS, bit 1 = _y set by AS
	int ng_textfield_idx;  // index into ng_textfields, or -1 if not a textfield
	// Cached skew (radians), preserved across _xscale/_yscale/_rotation setters (Ruffle parity).
	// Decomposed from any direct `transform.matrix =` assignment or timeline PlaceObject2 matrix.
	// 0 for the common rotation-only case.
	float skew;
	// Exact matrix from a direct `transform.matrix =` assignment, preserved
	// verbatim so reading transform.matrix back doesn't lose precision to the
	// xscale/yscale/rotation/skew decompose→recompose round-trip. The getter
	// uses these only while the MC's live xscale/yscale/rotation/skew still
	// bit-match the snapshot taken at assignment time — any later _xscale /
	// _yscale / _rotation setter writes a different value, the snapshot stops
	// matching, and the getter self-invalidates back to recomposition.
	double exact_m_a, exact_m_b, exact_m_c, exact_m_d;
	float exact_m_xs, exact_m_ys, exact_m_rot, exact_m_skew;
	u8 has_exact_matrix;
	// Drawing API bounds tracking (updated by moveTo/lineTo calls)
	float draw_xmin, draw_xmax, draw_ymin, draw_ymax;
	int draw_has_bounds;   // 1 if any moveTo/lineTo was called
	void* drawing_state;   // DrawingState* (lazily allocated, used by Drawing API)
	void* mask_mc;         // MovieClip* that masks this MC (set by setMask), NULL if none
	u8 is_mask;            // 1 if this MC is used as a mask for another MC (skip normal rendering)
	// Inverse of mask_mc: the MovieClip* this MC masks (Ruffle's masker->maskee
	// link). Set/cleared only through the setMask pair registry in action.c.
	// NEVER store a DisplayObject* for mask pairing — MovieClip structs are
	// immortal (tombstoned at depth == INT_MIN, never freed), display-list
	// entries are relocatable and reclaimable. See
	// SWFRecompDocs/plans/session13-fanout-reports/wave1-gfx-maskB.md §2/§3.
	void* maskee_mc;
	// MovieClip.scrollRect (AVM1). Ruffle core/src/avm1/globals/movie_clip.rs
	// set_scroll_rect: ANY object value sets has_scroll_rect, and the rect is
	// only overwritten when all four of x/y/width/height are readable. Stored
	// in TWIPS, each edge already collapsed to a whole pixel by ES ToInt32
	// (object_to_rectangle coerces every field with coerce_to_i32 BEFORE the
	// x+width / y+height additions — truncation toward zero, NOT rounding).
	// Read by tag.c's compose/render walks; see the scrollRect registry below.
	u8 has_scroll_rect;
	s32 sr_xmin, sr_ymin, sr_xmax, sr_ymax;
	// AS2 event dispatch state
	u8 mc_mouse_inside;    // 1 if mouse is currently inside this MC's hit area
	u8 mc_as_pressed;      // 1 if button was pressed while mouse was inside this MC
	u8 ts_stale_source;    // 1 if this MC was used as the source of a duplicateMovieClip call, or inherited the flag from such a source. Ruffle rejects `new TextSnapshot(mc)` for these in AVM1 (as_movie_clip() returns None), so we skip setting NATIVE_TEXTSNAPSHOT and getText returns undefined.
	u8 mc_enterframe_eligible; // 0 on creation tick, set to 1 on next tick so onEnterFrame doesn't fire on creation frame
	u8 attached_playable;      // attachMovie'd multi-frame clip playhead auto-advance state: 0=not applicable, 1=pending (attached this tick, not yet eligible), 2=active (promoted after attach-init drain; advanced by ng_advance_attached_clip_playheads). See PROGRESS #15.
	u8 movie_id;               // Which SWF movie defined this MC (0=main, 1+=child). Used for per-movie export table isolation in attachMovie.
	// Color transform for dynamic MCs (createEmptyMovieClip, etc.) not in the display list.
	// Default: multipliers=100, offsets=0. Used by Color.getTransform/setTransform when
	// ng_getColorTransform fails (no display_list entry).
	float cx_ra, cx_ga, cx_ba, cx_aa;
	float cx_rb, cx_gb, cx_bb, cx_ab;
	// Loaded image dimensions (from loadMovie with a PNG/JPEG file, swf_version==0 in MovieEntry)
	// Non-zero means an image is loaded into this MC (overrides _width/_height/getBounds/getSWFVersion)
	u16 loaded_image_width;
	u16 loaded_image_height;
	// Attached BitmapData (from attachBitmap) — raw pixel pointer for GPU rendering
	uint32_t* attached_bitmap_pixels;   // ARGB premultiplied pixel data (NULL if none)
	u16 attached_bitmap_width;
	u16 attached_bitmap_height;
	// Phase C: TextField bindings registered against this MC's property scope.
	// Lazily allocated; freed when the MC is unloaded (also pushes TFs back to
	// the unbound retry queue, mirroring Ruffle's unregister_bindings).
	Avm1TextFieldBinding* avm1_text_field_bindings;
	u16 avm1_text_field_binding_count;
	u16 avm1_text_field_binding_capacity;
	// MovieClip.opaqueBackground (s16 P2). Ruffle avm1/globals/movie_clip.rs:
	// the getter answers `color.to_rgb()` or UNDEFINED (not null — that is the
	// AVM2 spelling), and the setter treats undefined/null as "clear" and
	// everything else as coerce_to_u32 with the alpha forced to 255.
	// Mirrored onto the display-list entry, which is what the renderer reads.
	u8 opaque_bg_set;
	u32 opaque_bg_rgb;
};

// s21 w2-px-b: 1 while a `transform.matrix =` assignment's exact f32 a/b/c/d
// is still authoritative (no later _xscale/_yscale/_rotation setter changed the
// decomposed snapshot). Ruffle's Transform stores the matrix and only CACHES
// scale/rotation; rebuilding a/b/c/d from f32 degrees loses ~1e-4 relative,
// which near-cancelling nested matrices amplify (small_shear: b 0.0102 vs
// 0.001). Shared by getLocalMatrixForMC{,_render} and tag.c::apply_as_transform.
static inline int mc_exact_matrix_live(const MovieClip* mc)
{
	return mc->has_exact_matrix &&
		mc->xscale == mc->exact_m_xs && mc->yscale == mc->exact_m_ys &&
		mc->rotation == mc->exact_m_rot && mc->skew == mc->exact_m_skew;
}

// Global root MovieClip
extern MovieClip root_movieclip;

// Current execution context (MovieClip scope for variable resolution)
extern MovieClip* g_current_context;

// Event 'this' MC — set by onEnterFrame/onLoad dispatch before calling
// DefineFunction2 with this_obj=NULL. The generated code's 'preload this'
// path consumes this (sets it to NULL after reading).
extern MovieClip* g_event_this_mc;

// Override 'this' — passes arbitrary ActionVar as 'this' to DefineFunction2.
// Used for primitive thisArg in call/apply and undefined this in empty-method-name calls.
#ifndef DYNAMIC_GUEST
extern ActionVar g_override_this;
extern int g_override_this_set;
#endif

// Check if the current execution context's base clip has been removed.
// Used by generated code to halt script execution after function calls.
int actionBaseClipRemoved(void);

// Execution timeout — halts all script execution after time limit exceeded
void actionSetMaxExecutionDuration(double ms);
void actionResetExecutionTimer(void);
void actionSetTimeoutJmp(void* jmp_buf_ptr);

// Set the current execution context
void actionSetCurrentContext(MovieClip* mc);

// CONSTRUCT clip-action parameter capture + post-constructor replay.
// See action.c definition for rationale. Wired from:
//   - tag.c (aq_dispatch_clip_construct): Begin/End bracket the CONSTRUCT loop.
//   - action.c (actionSetVariable non-root MC path): Capture each SetVariable
//     whose name resolves to an addProperty setter on the MC's prototype chain.
//   - registered_class.c (actionInvokeRegisteredClassConstructor): Replay
//     captured params for that MC after the constructor body returns.
void actionBeginConstructCapture(MovieClip* mc);
void actionEndConstructCapture(void);
int  actionCaptureConstructSetVar(MovieClip* mc, const char* name, u32 name_len,
                                   ActionVar* value);
void actionReplayConstructParams(SWFAppContext* app_context, MovieClip* mc);
// Set/get the base clip (the clip whose timeline code is executing).
// actionSetTarget("") resets to base_clip, not root.
void actionSetBaseClip(MovieClip* mc);
MovieClip* actionGetBaseClip(void);
// Create or find a cached MovieClip by instance name
MovieClip* actionFindOrCreateMovieClip(SWFAppContext* app_context, const char* instance_name, MovieClip* parent);
// Initialize default textfield properties on a clone of a dynamic textfield
void actionInitDynTextFieldClone(SWFAppContext* app_context, MovieClip* mc);
// Invalidate cached MovieClip when removed from display list (swf_depth for disambiguation)
void actionInvalidateCachedMovieClip(SWFAppContext* app_context, const char* name, int swf_depth);
// Mark MC for deferred removal: transform depth and set pending_removal flag
void actionMarkMCPendingRemoval(SWFAppContext* app_context, const char* name, int swf_depth);
// Finalize pending removals: invalidate MCs marked for removal in a previous frame
void actionFinalizePendingRemovals(SWFAppContext* app_context);
// Detach mc->dynamic_props (NULLs the field) and queue the object for release
// at the next tick-boundary drain. Detach sites must not release inline —
// script may still hold borrowed refs (`this` in SWF6+ type-1 calls, WITH
// scopes). No-op if mc or its dynamic_props is NULL.
void actionDeferDpropsRelease(SWFAppContext* app_context, MovieClip* mc);
// Release everything queued by actionDeferDpropsRelease. Call only at a
// quiescent point (tick boundary in the frame loops / end of run).
void actionDrainDpropsReleases(SWFAppContext* app_context);
// Check if a named MC at given depth has an AS-level onUnload property
int actionMCHasOnUnloadProperty(const char* name, int swf_depth);
// Enqueue the AS-set onUnload handler on a MovieClip being removed (deferred — fires at next ShowFrame drain).
// Lookup happens at queue time so the (func, mc) pair is captured. The depth shift (mc->depth = -(swf_depth)-1-16384)
// and avm1_removed=1 happen at drain time inside the dispatcher so same-frame typeof(mc) returns 'movieclip' until ShowFrame.
void actionFireOnUnload(SWFAppContext* app_context, const char* instance_name, int swf_depth);
// Fire all pending deferred onUnload handlers (queued by removeMovieClip); call from tagShowFrame
void actionFirePendingUnloads(SWFAppContext* app_context);
// Enqueue a tag-level CLIP_EVENT_UNLOAD clip-action callback. The action fn pointer is recompiler-emitted
// static code; mc is captured for g_current_context restore at drain time.
void actionQueueClipActionUnload(void (*fn)(SWFAppContext*), MovieClip* mc);
// Same payload as actionQueueClipActionUnload, but queued with kind=SCRIPT
// (is_unload=0) so it doesn't drain via actionFirePendingUnloads during a
// goto rewind's nested tagShowFrame. Used by the tagPlaceObject2 backward-
// rewind clear-and-replace path so the displaced MC's UNLOAD trace lands
// after the calling script (loop_test8 trailing mc5unloaded after totals()).
void actionQueueClipActionUnloadDeferred(void (*fn)(SWFAppContext*), MovieClip* mc);
// Queue AS-level onUnload handlers on dynamic children of a MovieClip being removed.
// Handles clones + createEmptyMovieClip children that live in child_mc_cache (not the
// parent sprite's display_list). Called by tagRemoveObject2 before the parent's own
// UNLOAD clip actions fire so only pre-existing dynamic children enqueue.
void actionQueueDynamicChildUnloads(MovieClip* parent_mc);
// Fire all pending deferred onLoadInit handlers (queued by MCL loadClip); call from tagShowFrame
void actionFirePendingLoadInits(SWFAppContext* app_context);
// Fire the onLoadInit/onLoadError handlers the drain above PARKED for the next
// tick. Must run immediately before actionAdvancePlayingLevels (and once after
// the tick loop), so the sequence is enterFrame -> onLoadInit -> the loaded
// movie's next frame. See regression/avm1_mcl_load_tick.
void actionDrainPendingLoadInits(SWFAppContext* app_context);
// Same, but for the final tick / loop exit: no next tick exists, so entries
// queued by THIS tick fire too instead of being dropped.
void actionDrainPendingLoadInitsFinal(SWFAppContext* app_context);
// Non-zero while at least one onLoadInit is parked — an exit-condition reason
// to keep ticking, like a pending MCL load.
int actionHasPendingLoadInits(void);
// ImportAssets: load an imported SWF's init function in the current context
void actionImportAssets(SWFAppContext* app_context, const char* url);
// ImportAssets: issue the imported URL's GET at PRELOAD time (called from
// tagInit, before any frame script) — Ruffle MovieClip::preload.
void actionPreloadImportAssets(SWFAppContext* app_context, const char* url);
// Query button MC state for the button state machine in tag.c
// Returns mc->visible for the named button MC (1=visible, 0=hidden; defaults to 1 if MC not found)
int actionGetMCVisible(SWFAppContext* app_context, const char* instance_name, MovieClip* parent);
// Returns the 'enabled' property for the named button MC (1=enabled, 0=disabled; defaults to 1)
int actionGetMCEnabled(SWFAppContext* app_context, const char* instance_name, MovieClip* parent);

// VAL macro must be defined before other macros that use it
#define VAL(type, x) *((type*) x)

// Stack macros - use STACK, SP, OLDSP from swf.h (app_context->stack, etc.)
#define PUSH(t, v) \
	OLDSP = SP; \
	SP -= 4 + 4 + 8 + 8; \
	SP &= ~7; \
	STACK[SP] = t; \
	VAL(u32, &STACK[SP + 4]) = OLDSP; \
	VAL(u64, &STACK[SP + 16]) = v;

// Push string with ID: accepts char* from generated code, converts to UTF-16 with caching
void push_str_id_fn(SWFAppContext* app_context, const char* str, u32 byte_len, u32 id);
#define PUSH_STR_ID(v, n, id) push_str_id_fn(app_context, (const char*)(v), (u32)(n), (u32)(id))

// Push string without ID (for dynamic strings from runtime code, converts char* to UTF-16)
#define PUSH_STR(v, n) push_str_id_fn(app_context, (const char*)(v), (u32)(n), 0)

// Push pre-existing UTF-16 string directly (no conversion)
#define PUSH_U16(v, n) \
	OLDSP = SP; \
	SP -= 4 + 4 + 8 + 8; \
	SP &= ~7; \
	STACK[SP] = ACTION_STACK_VALUE_STRING; \
	VAL(u32, &STACK[SP + 4]) = OLDSP; \
	VAL(u32, &STACK[SP + 8]) = (u32)(n); \
	VAL(u32, &STACK[SP + 12]) = 0; \
	VAL(u64, &STACK[SP + 16]) = (u64)(v);

// Push pre-existing UTF-16 string with string ID
#define PUSH_U16_ID(v, n, id) \
	OLDSP = SP; \
	SP -= 4 + 4 + 8 + 8; \
	SP &= ~7; \
	STACK[SP] = ACTION_STACK_VALUE_STRING; \
	VAL(u32, &STACK[SP + 4]) = OLDSP; \
	VAL(u32, &STACK[SP + 8]) = (u32)(n); \
	VAL(u32, &STACK[SP + 12]) = (u32)(id); \
	VAL(u64, &STACK[SP + 16]) = (u64)(v);

#define PUSH_VAR(p) pushVar(app_context, p);

#define POP() \
	if (SP < INITIAL_STACK_SIZE) SP = VAL(u32, &STACK[SP + 4]);

#define POP_2() \
	POP(); \
	POP();

#define STACK_TOP_TYPE STACK[SP]
#define STACK_TOP_N VAL(u32, &STACK[SP + 8])
#define STACK_TOP_ID VAL(u32, &STACK[SP + 12])
#define STACK_TOP_VALUE VAL(u64, &STACK[SP + 16])

#define SP_SECOND_TOP VAL(u32, &STACK[SP + 4])
#define STACK_SECOND_TOP_TYPE STACK[SP_SECOND_TOP]
#define STACK_SECOND_TOP_N VAL(u32, &STACK[SP_SECOND_TOP + 8])
#define STACK_SECOND_TOP_ID VAL(u32, &STACK[SP_SECOND_TOP + 12])
#define STACK_SECOND_TOP_VALUE VAL(u64, &STACK[SP_SECOND_TOP + 16])

#define INITIAL_STACK_SIZE 8388608  // 8 MB
#define INITIAL_SP INITIAL_STACK_SIZE

extern ActionVar* temp_val;

void initTime(SWFAppContext* app_context);

void pushVar(SWFAppContext* app_context, ActionVar* p);
void popVar(SWFAppContext* app_context, ActionVar* var);
void peekVar(SWFAppContext* app_context, ActionVar* var);
void peekSecondVar(SWFAppContext* app_context, ActionVar* var);
void setVariableByName(const char* var_name, ActionVar* value);
void setGlobalVariableByName(const char* var_name, ActionVar* value);

void actionPrevFrame(SWFAppContext* app_context);
void actionToggleQuality(SWFAppContext* app_context);

void actionAdd(SWFAppContext* app_context);
void actionAdd2(SWFAppContext* app_context, char* str_buffer);
void actionSubtract(SWFAppContext* app_context);
void actionMultiply(SWFAppContext* app_context);
void actionDivide(SWFAppContext* app_context);
void actionModulo(SWFAppContext* app_context);
void actionEquals(SWFAppContext* app_context);
void actionLess(SWFAppContext* app_context);
void actionLess2(SWFAppContext* app_context);
void actionEquals2(SWFAppContext* app_context);
void actionAnd(SWFAppContext* app_context);
void actionOr(SWFAppContext* app_context);
void actionNot(SWFAppContext* app_context);
void actionToInteger(SWFAppContext* app_context);
void actionToNumber(SWFAppContext* app_context);
void actionToString(SWFAppContext* app_context, char* str_buffer);
void actionStackSwap(SWFAppContext* app_context);
void actionDuplicate(SWFAppContext* app_context);
void actionGetMember(SWFAppContext* app_context);
void actionTargetPath(SWFAppContext* app_context, char* str_buffer);
void actionEnumerate(SWFAppContext* app_context, char* str_buffer);

// Movie control
void actionGoToLabel(SWFAppContext* app_context, const char* label);
void actionGotoFrame2(SWFAppContext* app_context, u8 play_flag, u16 scene_bias);

// Frame label lookup (returns -1 if not found, otherwise frame index)
int findFrameByLabel(const char* label);

void actionStringEquals(SWFAppContext* app_context, char* a_str, char* b_str);
void actionStringLength(SWFAppContext* app_context, char* v_str);
void actionStringExtract(SWFAppContext* app_context, char* str_buffer);
void actionMbStringLength(SWFAppContext* app_context, char* v_str);
void actionMbStringExtract(SWFAppContext* app_context, char* str_buffer);
void actionStringAdd(SWFAppContext* app_context, char* a_str, char* b_str);
void actionStringLess(SWFAppContext* app_context);
void actionImplementsOp(SWFAppContext* app_context);
void actionCharToAscii(SWFAppContext* app_context);

void actionGetVariable(SWFAppContext* app_context);
void actionSetVariable(SWFAppContext* app_context);
void actionSetTarget2(SWFAppContext* app_context);
void actionDefineLocal(SWFAppContext* app_context);
void actionDeclareLocal(SWFAppContext* app_context);
void actionGetProperty(SWFAppContext* app_context);
void actionSetProperty(SWFAppContext* app_context);
void actionCloneSprite(SWFAppContext* app_context);
void actionMarkCloneStripped(SWFAppContext* app_context);
void actionRemoveSprite(SWFAppContext* app_context);
void actionSetTarget(SWFAppContext* app_context, const char* target_name);

void actionNextFrame(SWFAppContext* app_context);
void actionPlay(SWFAppContext* app_context);
void actionStop(SWFAppContext* app_context);
void actionGotoFrame(SWFAppContext* app_context, u16 frame);
void actionTrace(SWFAppContext* app_context);
void actionStartDrag(SWFAppContext* app_context);
void actionEndDrag(SWFAppContext* app_context);
void actionStopSounds(SWFAppContext* app_context);
void actionGetURL(SWFAppContext* app_context, const char* url, const char* target);
void actionRandomNumber(SWFAppContext* app_context);
void actionAsciiToChar(SWFAppContext* app_context, char* str_buffer);
void actionMbCharToAscii(SWFAppContext* app_context, char* str_buffer);
void actionGetTime(SWFAppContext* app_context);
void actionMbAsciiToChar(SWFAppContext* app_context, char* str_buffer);
void actionTypeof(SWFAppContext* app_context, char* str_buffer);
void actionCastOp(SWFAppContext* app_context);
void actionCallFunction(SWFAppContext* app_context, char* str_buffer);
void actionReturn(SWFAppContext* app_context);
void actionInitArray(SWFAppContext* app_context);
void actionInitObject(SWFAppContext* app_context);
void actionIncrement(SWFAppContext* app_context);
void actionDecrement(SWFAppContext* app_context);
void actionInstanceOf(SWFAppContext* app_context);
void actionEnumerate2(SWFAppContext* app_context, char* str_buffer);
void actionDelete(SWFAppContext* app_context);
void actionDelete2(SWFAppContext* app_context, char* str_buffer);
void actionBitAnd(SWFAppContext* app_context);
void actionBitOr(SWFAppContext* app_context);
void actionBitXor(SWFAppContext* app_context);
void actionBitLShift(SWFAppContext* app_context);
void actionBitRShift(SWFAppContext* app_context);
void actionBitURShift(SWFAppContext* app_context);
void actionStrictEquals(SWFAppContext* app_context);
void actionGreater(SWFAppContext* app_context);
void actionStringGreater(SWFAppContext* app_context);
void actionExtends(SWFAppContext* app_context);
void actionStoreRegister(SWFAppContext* app_context, u8 register_num);
void actionPushRegister(SWFAppContext* app_context, u8 register_num);
void actionResetRegisters(void);

// Super context for OOP constructor/method chains
void actionGetCurrentSuperInfo(u64* out_this, u32* out_depth);

// Returns nearest lockroot ancestor MC (for preload_root in DefineFunction2)
MovieClip* actionGetLockRoot(void);

// Resolve a __proto__ ActionVar to an ASObject* (returned as void*).
// Handles OBJECT (direct cast) and SUPER (unwrap via proto chain walk).
void* resolveProtoVar(ActionVar* proto_var);

void actionDefineFunction(SWFAppContext* app_context, const char* name, void (*func)(SWFAppContext*), u32 param_count);
int actionCall(SWFAppContext* app_context);
void actionCallMethod(SWFAppContext* app_context, char* str_buffer);
void actionGetURL2(SWFAppContext* app_context, u8 send_vars_method, u8 load_target_flag, u8 load_variables_flag);
void actionSetMember(SWFAppContext* app_context);
void actionNewObject(SWFAppContext* app_context);
void actionNewMethod(SWFAppContext* app_context);

// Function pointer type for DefineFunction2
typedef ActionVar (*Function2Ptr)(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj);

void actionDefineFunction2(SWFAppContext* app_context, const char* name, Function2Ptr func, u32 param_count, u8 register_count, u16 flags);
int actionWithStart(SWFAppContext* app_context);
void actionWithEnd(SWFAppContext* app_context);

// Exception handling (try-catch-finally)
void actionThrow(SWFAppContext* app_context);
void actionThrowPending(SWFAppContext* app_context);
void actionTryBegin(SWFAppContext* app_context);
jmp_buf* actionGetExceptionJmpBuf(SWFAppContext* app_context);
void actionCatchEnter(SWFAppContext* app_context);
void actionCatchToVariable(SWFAppContext* app_context, const char* var_name);
void actionCatchToRegister(SWFAppContext* app_context, u8 reg_num);
void actionCatchGetException(SWFAppContext* app_context, ActionVar* out);
void actionTryEnd(SWFAppContext* app_context);
void actionClearException(SWFAppContext* app_context);
bool actionExceptionPending(SWFAppContext* app_context);
void actionSetReturnPending(SWFAppContext* app_context, ActionVar* value);
bool actionReturnPending(SWFAppContext* app_context);
ActionVar actionGetPendingReturn(SWFAppContext* app_context);

// Macro for inline setjmp in generated code
#define ACTION_TRY_SETJMP(app_context) setjmp(*actionGetExceptionJmpBuf(app_context))

// --- try-helper emission mode (recompiler option `try_helper`) --------------
//
// The AVM1 jmp_buf already lives in the runtime (g_exception_state), but the
// SETJMP still executes in generated code, which the in-browser guest
// toolchain cannot lower (it links the WASIX libc's stack_checkpoint version,
// which the page stubs out). In helper mode the try BODY is emitted as a
// lifted function and avm1_try_run executes the setjmp in its own frame, so
// generated code performs no setjmp at all. The body's locals/parameters stay
// in the enclosing frame and reach it through Avm1TryEnv; control flow that
// leaves the try body (a `return`, or a jump to a label outside it) comes back
// as an exit code the enclosing function's dispatch replays.
typedef struct Avm1TryEnv
{
	SWFAppContext* app_context;
	ActionVar* args;       // DefineFunction2 only (NULL otherwise)
	u32 arg_count;
	ActionVar* regs;       // DefineFunction2 only (NULL otherwise)
	void* this_obj;        // DefineFunction2 only (NULL otherwise)
	char* str_buffer;
	ActionVar ret;         // value for an AVM1_TRY_EXIT_RETURN_VALUE exit
} Avm1TryEnv;

#define AVM1_TRY_THROWN            (-1)  // avm1_try_run: a throw was caught
#define AVM1_TRY_EXIT_FALLTHROUGH    0   // body ran off its end
#define AVM1_TRY_EXIT_RETURN         1   // `return;` in the enclosing function
#define AVM1_TRY_EXIT_RETURN_VALUE   2   // `return env->ret;`
#define AVM1_TRY_EXIT_GOTO_BASE    100   // +k: goto the k'th external label

typedef int (*Avm1TryBodyFn)(void* env);

// setjmp + call. Returns AVM1_TRY_THROWN when a throw landed in this frame,
// otherwise the body's exit code. actionTryBegin must already have pushed the
// handler frame (this arms it, exactly as ACTION_TRY_SETJMP did).
int avm1_try_run(SWFAppContext* app_context, Avm1TryBodyFn fn, void* env);

// Control flow
int evaluateCondition(SWFAppContext* app_context);
bool actionWaitForFrame(SWFAppContext* app_context, u16 frame);
bool actionWaitForFrame2(SWFAppContext* app_context);

// Forward declaration (defined in object.h)
typedef struct ASArray ASArray;

// Public helper called from recompiler-generated code when DefineFunction2 has
// the preload_arguments flag. Sets up callee/caller/__proto__ on the preloaded
// arguments array, using the global g_current_executing_func / g_prev_executing_func.
void swf_setup_arguments_props(SWFAppContext* app_context, ASArray* arr);

// Key event dispatch — called from swf_core.c after delivering EV_KEY_DOWN/EV_KEY_UP.
// Broadcasts onKeyDown/onKeyUp to all registered Key listeners.
void actionDispatchKeyDown(SWFAppContext* app_context);
void actionDispatchKeyUp(SWFAppContext* app_context);
// Mouse event dispatch — called from swf_core.c after delivering mouse events.
// Broadcasts onMouseDown/onMouseUp/onMouseMove to all registered Mouse listeners.
void actionDispatchMouseDown(SWFAppContext* app_context);
void actionDispatchMouseUp(SWFAppContext* app_context);
void actionDispatchMouseMove(SWFAppContext* app_context);
// Tab key focus advance — called on Tab key press (before Key.broadcastMessage).
// reversed=1 for Shift+Tab (go backward), 0 for Tab (go forward).
void actionAdvanceTabFocus(SWFAppContext* app_context, int reversed);
// Flush deferred rollOver/rollOut events queued by Selection.setFocus().
void actionFlushDeferredRollEvents(SWFAppContext* app_context);

// AS2 onEnterFrame dispatch — call after sprite initialization to fire mc.onEnterFrame
// for any MovieClip that has the property set. Iterates cache in reverse creation order
// (front-to-back: higher depth fires first) to match Flash's dispatch order.
void actionDispatchEnterFrameHandlers(SWFAppContext* app_context);
// Mark dynamic MCs (createEmptyMovieClip, no display_obj) as eligible for next tick's enterFrame.
// Called after frame processing to ensure MCs created mid-script get marked on the same tick.
void actionMarkDynamicMCsEnterFrameEligible(void);
// Dispatch root timeline onEnterFrame stored in var_map (DefineFunction/SetVariable path).
void actionDispatchRootVarMapEnterFrame(SWFAppContext* app_context);
// Dispatch _root.onLoad (fires once after first frame completes).
void actionDispatchRootOnLoad(SWFAppContext* app_context);
// Dispatch AS-level onLoad on a MovieClip (walks __proto__ chain for onLoad handler).
void actionDispatchMCOnLoad(SWFAppContext* app_context, MovieClip* mc);
// Queue a MovieClip for deferred onLoad dispatch.
void actionQueueMCOnLoad(MovieClip* mc);
// Flush pending onLoad dispatches for dynamically-attached MCs.
void actionFlushPendingOnLoads(SWFAppContext* app_context);
int actionHasPendingOnLoads(void);
// Check if any onEnterFrame handlers are registered.
int actionHasEnterFrameHandlers(void);

// Timer system (setInterval / setTimeout / clearInterval)
void processTimers(SWFAppContext* app_context, double frame_duration_ms);
int hasActiveTimers(void);

// ExternalInterface bridge
// External call handler: called when AS code invokes ExternalInterface.call(name, ...args)
typedef ActionVar (*ExternalCallHandler)(SWFAppContext*, const char* name, ActionVar* args, int arg_count);
extern ExternalCallHandler g_external_call_handler;

// After-tick handler: called at the end of each frame tick (for test harness injection)
typedef void (*AfterTickHandler)(SWFAppContext*, int tick_number);
extern AfterTickHandler g_after_tick_handler;

// Call a registered EI callback by name (for test harness / host environment)
ActionVar actionEI_callInternalInterface(SWFAppContext* app_context, const char* name, ActionVar* args, int arg_count);

// Convert an ActionVar STRING to a UTF-8 C string in buf. Returns bytes written.
int ei_actionvar_to_utf8(ActionVar* var, char* buf, int buf_size);

// Set __proto__ on an object to Object.prototype (for test harness use)
void ei_set_object_proto(SWFAppContext* app_context, void* obj);

// Drawing API data structures (used by beginFill/moveTo/lineTo/curveTo/endFill)
typedef struct {
	u8 type;           // 0=MOVE_TO, 1=LINE_TO, 2=CURVE_TO
	float x, y;        // endpoint (pixels)
	float cx, cy;      // control point (CURVE_TO only)
} DrawCmd;

typedef struct {
	float fill_r, fill_g, fill_b, fill_a;
	int has_fill;
	float line_width;   // pixels (0 = no stroke)
	float line_r, line_g, line_b, line_a;
	int has_line;
	// Gradient fill data (mutually exclusive with solid fill)
	int has_gradient;
	u8 gradient_type;      // 0x10=linear, 0x12=radial, 0x13=focal_radial
	u8 spread_mode;        // 0=pad, 1=reflect, 2=repeat
	u8 interpolation;      // 0=RGB, 1=linearRGB
	float focal_ratio;     // -1.0 to 1.0 for focal radial
	u8 gradient_ramp[256 * 4];    // pre-generated 256 RGBA8 entries
	float gradient_matrix[16];    // 4x4 column-major matrix (gradient space → stage twips)
	// Gradient line style data
	int has_line_gradient;
	u8 line_gradient_type;
	u8 line_spread_mode;
	u8 line_interpolation;
	float line_focal_ratio;
	u8 line_gradient_ramp[256 * 4];
	float line_gradient_matrix[16];
	// Bitmap fill data (mutually exclusive with solid/gradient fill)
	int has_bitmap_fill;
	void* bitmap_native;        // BitmapDataNative* — read live at render time
	float bitmap_matrix[6];     // user-supplied affine in PIXEL space (a,b,c,d,tx,ty)
	int bitmap_repeat;
	int bitmap_smooth;
	// Tessellated output (filled after endFill)
	float* fill_verts;     // x,y pairs in twips (triangle vertices)
	u32 fill_vert_count;   // number of vertices (multiple of 3)
	float* line_verts;     // x,y pairs in twips (line quad triangles)
	u32 line_vert_count;
	// Retained stroke source polyline for scale-aware re-expansion to Flash's
	// minimum 1px on-screen stroke width (rebuilt by drawingBuildStroke when the
	// MC's render scale changes). NULL when the path carries no stroke.
	float* stroke_poly;        // x,y pairs in PIXELS (contour vertices)
	u32 stroke_poly_count;
	u32* stroke_contours;      // per-contour start indices into stroke_poly
	u32 stroke_contour_count;
	int stroke_filled;         // path had a fill (controls stroke auto-close)
	float stroke_built_half_w; // half-width (twips) line_verts were built at; <0 = unbuilt
	// Stroke join/cap style snapshot (Flash LINESTYLE2 semantics, as the
	// drawing API exposes them). 0 is the API default in BOTH fields, so a
	// zero-initialized DrawPath is Ruffle's `round`/`round` default.
	u8 line_joints;            // 0 round, 1 miter, 2 bevel
	u8 line_caps;              // 0 round, 1 none (butt), 2 square
	float line_miter;          // miter limit; <1 degrades to bevel (lyon rule)
} DrawPath;

typedef struct {
	DrawPath* paths;
	u32 path_count;
	u32 path_capacity;
	// Current pen position
	float pen_x, pen_y;
	int pen_set;
	// Current fill style (set by beginFill or beginGradientFill)
	float fill_r, fill_g, fill_b, fill_a;
	int has_fill;
	// Gradient fill state (set by beginGradientFill)
	int has_gradient;
	u8 gradient_type;
	u8 gradient_spread;
	u8 gradient_interp;
	float gradient_focal;
	u8 gradient_ramp[256 * 4];
	float gradient_matrix[16];
	// Current line style (set by lineStyle)
	float line_w;
	float line_r, line_g, line_b, line_a;
	int has_line;
	// Join/cap style of the current line style (see DrawPath above; 0/0 =
	// the AVM1 `lineStyle` defaults, which are round/round).
	u8 line_joints;
	u8 line_caps;
	float line_miter;
	// Line gradient state (set by lineGradientStyle)
	int has_line_gradient;
	u8 line_gradient_type;
	u8 line_gradient_spread;
	u8 line_gradient_interp;
	float line_gradient_focal;
	u8 line_gradient_ramp[256 * 4];
	float line_gradient_matrix[16];
	// Bitmap fill state (set by beginBitmapFill)
	int has_bitmap_fill;
	void* bitmap_native;        // BitmapDataNative* — read live at render time
	float bitmap_matrix[6];     // user-supplied affine in PIXEL space (a,b,c,d,tx,ty)
	int bitmap_repeat;
	int bitmap_smooth;
	// Active path commands (between beginFill and endFill)
	DrawCmd* cmds;
	u32 cmd_count;
	u32 cmd_capacity;
} DrawingState;

// Drawing render info (used by tag.c in graphics mode)
typedef struct {
	const float* fill_verts;  // twips, triangle vertices (x,y pairs)
	u32 fill_count;           // vertex count
	float fill_r, fill_g, fill_b, fill_a;
	int has_gradient;
	u8 gradient_type;         // 0x10, 0x12, 0x13
	u8 spread_mode;
	u8 interpolation;         // 0=RGB, 1=linearRGB
	float focal_ratio;
	const u8* gradient_ramp;  // 256*4 RGBA8 entries
	const float* gradient_matrix; // 4x4 column-major
	// Bitmap fill (beginBitmapFill) — resolved live from the BitmapDataNative
	int has_bitmap_fill;
	const u32* bitmap_pixels;  // premultiplied ARGB, row-major
	u32 bitmap_width, bitmap_height;
	const float* bitmap_matrix;      // 6 floats: a,b,c,d,tx_px,ty_px
	int bitmap_repeat;
	int bitmap_smooth;
	const float* line_verts;
	u32 line_count;
	float line_r, line_g, line_b, line_a;
	int has_line_gradient;
	u8 line_gradient_type;
	u8 line_spread_mode;
	u8 line_interpolation;
	float line_focal_ratio;
	const u8* line_gradient_ramp;
	const float* line_gradient_matrix;
	u32 transform_id;         // MC's transform slot
	u32 cxform_id;            // MC's cxform slot
} DrawingRenderInfo;

typedef void (*DrawingRenderCallback)(const DrawingRenderInfo* info, void* user_data);
int actionIterateDrawings(DrawingRenderCallback cb, void* user_data);

// Masked MC iteration: callback receives (masked_mc_paths, mask_mc_paths) pairs
typedef struct {
	int path_count;
	const DrawingRenderInfo* paths;   // array of path_count entries
} DrawingMCInfo;

typedef void (*DrawingMaskedCallback)(const DrawingMCInfo* masked, const DrawingMCInfo* mask, void* user_data);
int actionIterateMaskedDrawings(DrawingMaskedCallback cb, void* user_data);

// Get Drawing API paths for a specific MovieClip (by instance name).
// Returns the number of paths filled into out[]. Used by tag.c for sprite clip masks.
int actionGetMCDrawingPathsByName(const char* instance_name, DrawingRenderInfo* out, int max_out);

// --- AVM1 setMask pairing (mask defect B) -----------------------------------
// The registry of live `maskee.setMask(masker)` pairs. All state lives on
// MovieClip (immortal); no DisplayObject* is ever stored. tag.c's paint loops
// use these to (a) suppress a live masker as ordinary content and (b) push the
// masker's geometry into the stencil around the maskee.
//
// Number of registered pairs. 0 in every movie that never calls setMask, which
// is the one-branch fast-out at the top of each paint hook.
int actionAvm1MaskPairCount(void);
// Copy up to `max` live pairs into the caller's arrays (MovieClip* each).
// "Live" = both partners are non-tombstoned. Returns the number written.
int actionAvm1GetMaskPairs(void** out_maskees, void** out_maskers, int max);
// 1 when `mc_ptr` is the masker of a live pair (skip it as content).
int actionAvm1IsLiveMasker(const void* mc_ptr);
// The masker MovieClip* of the live pair whose maskee is `mc_ptr`, else NULL.
void* actionAvm1MaskerForMC(const void* mc_ptr);

// --- AVM1 MovieClip.scrollRect ----------------------------------------------
// Same shape as the setMask registry above, and for the same reason: the
// renderer works in display-list entries, but a DisplayObject* stored across
// frames is a use-after-free (ng_spriteDLRealloc / ng_freeSpriteDL / depth
// reuse). Only immortal MovieClip*s are registered; tag.c re-reads
// `mc->display_obj` at the point of use.
typedef struct Avm1ScrollRect {
	s32 xmin, ymin, xmax, ymax;   // twips
} Avm1ScrollRect;
// Number of live clips carrying a scrollRect. 0 for every movie that never
// assigns one — the zero-cost early-out for compose_children's hot loop.
int actionAvm1ScrollRectCount(void);
// Copy up to `max` live (clip, rect) pairs out. Returns the number written.
int actionAvm1GetScrollRects(void** out_mcs, Avm1ScrollRect* out_rects, int max);

// Attached bitmap iteration: callback for rendering BitmapData attached to MCs
typedef struct AttachedBitmapInfo {
	const uint32_t* pixels;   // ARGB premultiplied pixel data
	u32 width;
	u32 height;
	float x_twips;            // position in stage coordinates (twips)
	float y_twips;
	// GPU transform slot holding the MC's full AS transform (_x/_y/_xscale/
	// _yscale/_rotation), allocated per tick by apply_dynamic_mc_transforms.
	// Non-zero means the quad must be drawn at the LOCAL origin through this
	// slot instead of at x_twips/y_twips through identity — otherwise
	// _xscale = -100 and friends have no effect at all on the render.
	u32 xform_slot;
	float alpha;              // 0..100, for the cxform slot
} AttachedBitmapInfo;

typedef void (*AttachedBitmapCallback)(const AttachedBitmapInfo* info, void* user_data);
int actionIterateAttachedBitmaps(AttachedBitmapCallback cb, void* user_data);

// Video frame access for headless rendering (decoded FLV frames).
// Returns 1 if a decoded frame is available. Caller must free *out_argb.
// Format: ARGB u32 (A=bits31-24, R=bits23-16, G=bits15-8, B=bits7-0).
//
// If target_w > 0 && target_h > 0 and the cached frame is at different
// dimensions, the frame is resampled (via libswscale when available) to the
// target before ARGB packing. Pass target_w=target_h=0 to get native size.
int actionGetVideoFramePixels(uint32_t** out_argb, int target_w, int target_h,
                              int* out_w, int* out_h);

// Embedded-video (DefineVideoStream + VideoFrame) per-stream pixel accessor.
// Looks up the most recently decoded frame for (char_id, frame_num); returns 1
// if available and fills *out_argb (caller frees) + *out_w/*out_h. The
// renderer's two video sites in tag.c try this path first, then fall back to
// the NetStream-keyed actionGetVideoFramePixels.
int actionGetEmbeddedVideoFramePixels(size_t char_id, u16 frame_num,
                                      uint32_t** out_argb,
                                      int* out_w, int* out_h);

// Text field rendering info (used by tag.c in graphics mode)
typedef struct TextFieldRenderInfo {
	int has_background;
	u32 background_color;  // 24-bit RGB
	int has_border;
	u32 border_color;      // 24-bit RGB
	float x, y, w, h;     // position and size in pixels
	// World matrix of the field. When has_matrix != 0, x/y are the box origin
	// in FIELD-LOCAL pixels (the createTextField visual offset, usually 0) and
	// w/h are the box size in field-local pixels; the renderer emits the box
	// in local twips through this matrix, so _xscale/_yscale/_rotation and any
	// ancestor matrix survive. When has_matrix == 0 (orphan DefineEditText
	// path) x/y/w/h are pre-composed stage pixels and the matrix is identity.
	// m_tx / m_ty are in TWIPS; a/b/c/d are unitless.
	// When has_matrix != 0, w/h are the RAW local field size and edge_x/edge_y
	// say whether Flash's extra right/bottom edge pixel applies. That pixel is
	// a DEVICE pixel (it comes from rasterising the border polyline), so the
	// renderer adds it after dividing by the matrix's per-axis scale.
	int has_matrix;
	int edge_x, edge_y;
	// !embedFonts. Device-font boxes take Ruffle's axis-aligned, pixel-snapped
	// `draw_device_text_box` instead of the matrix-transformed one.
	int device_font;
	float m_a, m_b, m_c, m_d;
	float m_tx, m_ty;
} TextFieldRenderInfo;

// Get text field count and info for rendering. Returns count of text fields.
// For each text field, calls callback with the render info and user data.
typedef void (*TextFieldRenderCallback)(const TextFieldRenderInfo* info, void* user_data);
int actionIterateTextFields(TextFieldRenderCallback cb, void* user_data);

// EditText z-order window (s17 w2-gfx-edittext-bg). Ruffle paints a field's
// background/border/glyphs from render_self, i.e. at the field's own display
// depth; our iterators are a post-pass in child_mc_cache order. The root render
// loops drive them per depth with these modes; every other caller leaves the
// window at TF_WINDOW_ALL and sees the historical single-pass behaviour.
#define TF_WINDOW_ALL      0   // no filtering (default)
#define TF_WINDOW_AT_DEPTH 1   // only root-timeline fields at exactly this depth
#define TF_WINDOW_REST     2   // everything the root walk could not place
void actionSetTextFieldDepthWindow(int mode, int swf_depth, int swf_max_depth);
void actionBeginTextFieldDepthPass(int swf_max_depth);
int actionHasTextFieldAtDepth(int swf_depth);

// Per-run formatting info (color, font_height) keyed by UTF-8 byte offsets
// into TextFieldGlyphInfo.text_utf8. Runs cover the text consecutively. Used
// only when text was set via htmlText/text with a TFRunTable populated; for
// plain-text fields with no run table, runs is NULL and run_count is 0.
typedef struct TextFieldGlyphRun {
    u32 byte_start;
    u32 byte_length;
    u32 color;       // 24-bit RGB
    u16 font_height; // twips
    u8 align;        // 0=left, 1=right, 2=center, 3=justify
    u8 bullet;       // non-zero when the run is inside <li>
} TextFieldGlyphRun;

// Text field glyph rendering info (used by tag.c for headless glyph rendering)
typedef struct TextFieldGlyphInfo {
    u16 font_id;          // SWF font ID
    u16 font_height;      // font size in twips (fallback default when no runs)
    u32 text_color;       // 24-bit RGB (fallback default when no runs)
    float x, y, w, h;    // field bounds in pixels
    const char* text_utf8; // text content (UTF-8, null-terminated); may contain
                            // SENTINEL_TAG_BREAK (0xFE) / SENTINEL_BR_BREAK (0xFF)
                            // bytes when sourced from a TFRunTable — treat as newline.
    size_t text_len;      // text length in bytes
    const TextFieldGlyphRun* runs; // per-run color/font_height, NULL if none
    int run_count;
    // Layout margins/indent in twips (left_margin already includes block_indent).
    // s32 so we can hold s16 indent + non-negative block/left margin without overflow.
    s32 left_margin_twips;
    s32 right_margin_twips;
    s32 indent_twips;
    // Line-to-line leading in twips, from the field's TextFormat (or the
    // DefineEditText tag's layout leading). Ruffle's line height is
    // `max_ascent + max_descent + line_leading_adjustment()`, where the
    // adjustment is the span's leading and font-provided leading is ignored
    // outright (html/layout.rs:252-255). 0 when unset — which is what Ruffle
    // uses for a field whose TextFormat never set `leading`.
    s32 leading_twips;
    // The field's local bounds-RECT min (twips). Flash/Ruffle lay text out at
    // matrix.translate(bounds.x_min + GUTTER, ...) (edit_text.rs), i.e. the text
    // origin is offset from the placement matrix by the bounds-RECT min. info.x/y
    // carry only the placement translation, so the renderer adds this. 0 for
    // dynamic/createTextField fields (no static DefineEditText bounds).
    s32 bounds_xmin_twips;
    s32 bounds_ymin_twips;
    // Caret position in UTF-16 code units when this field is keyboard-focused,
    // or -1 when it isn't (the common case — nothing is focused in headless /
    // offscreen runs, so the caret is never drawn there and CI is unaffected).
    int caret_char;
    // The field's MovieClip (only set for the child_mc_cache path; NULL for
    // orphan/static fields). Lets the renderer read/write the cached horizontal
    // scroll offset to keep the caret visible.
    void* mc;
    // Selection range in UTF-16 code units for the focused field (sel_begin..
    // sel_end, unordered), or -1/-1 when there's no selection or the field isn't
    // focused — so the highlight, like the caret, is browser-only / never in CI.
    int sel_begin;
    int sel_end;
    // 1 for a DefineEditText field WITHOUT UseOutlines (a device-font field):
    // Ruffle rounds each device-font glyph advance to a whole pixel
    // (font_like.rs FontLike::evaluate, FontType::Device). 0 otherwise.
    int device_font;
} TextFieldGlyphInfo;

typedef void (*TextFieldGlyphCallback)(const TextFieldGlyphInfo* info, void* user_data);
int actionIterateTextFieldGlyphs(TextFieldGlyphCallback cb, void* user_data);

// Render text fields that have no AVM1 MovieClip wrapper in child_mc_cache —
// i.e. placed DefineEditText DisplayObjects that no script has touched.
// Walks the display list recursively, builds glyph + border/background info
// from the static metadata + the current value of the bound variable (if
// any), and invokes the supplied callbacks. Mirrors Flash's behavior where
// every placed DisplayObject renders regardless of script interaction.
// One root-timeline orphan EditText at swf `depth` (the filtered-entry route
// in tag.c's root loops; the orphan walk skips such entries). Returns 1 if
// emitted.
int actionEmitOrphanRootTextField(SWFAppContext* app_context, int depth,
	TextFieldRenderCallback render_cb, TextFieldGlyphCallback glyph_cb,
	void* user_data);
int actionIterateOrphanTextFields(SWFAppContext* app_context,
	TextFieldRenderCallback render_cb, TextFieldGlyphCallback glyph_cb,
	void* user_data);

// Focus rect rendering info (used by tag.c in graphics mode)
typedef struct FocusRectInfo {
	float x, y, w, h;  // world-space bounds in twips
} FocusRectInfo;

// Get focus rect bounds if one should be drawn. Returns 1 if should draw, 0 if not.
int actionGetFocusRectInfo(FocusRectInfo* out);

// AS2 MC event dispatch — called from swf_core.c on mouse events.
// Iterates child_mc_cache, checks hit area, and calls onPress/onDragOver etc.
void actionDispatchMCPress(SWFAppContext* app_context);
void actionDispatchMCRelease(SWFAppContext* app_context);
void actionDispatchMCMouseMove(SWFAppContext* app_context);
// Global AS2 onMouseDown/onMouseUp/onMouseMove dispatch — fires on ALL sprite MCs.
void actionDispatchMCMouseDown(SWFAppContext* app_context);
void actionDispatchMCMouseUp(SWFAppContext* app_context);
void actionDispatchMCMouseMoveGlobal(SWFAppContext* app_context);
// Dispatch key events to focused MC — fires onKeyDown/onKeyUp on g_focused_mc.
// Enter/Space on focused MC with onPress → simulated press+release.
void actionDispatchKeyDownToFocused(SWFAppContext* app_context, int key_code);
void actionDispatchKeyPressToFocused(SWFAppContext* app_context, int key_code);
void actionDispatchKeyUpToFocused(SWFAppContext* app_context, int key_code);
// Focus highlight state: 0=INACTIVE, 1=ACTIVE_HIDDEN, 2=ACTIVE_VISIBLE.
// MC key handlers (onKeyDown/Up) require is_active (>=1).
// Enter/Space press simulation requires is_visible (==2).
void actionResetHighlightState(void);
// Version-aware highlight reset matching Ruffle's should_reset_highlight:
// event_type: 0=mouse_move, 1=left_down, 2=left_up, 3=right_down, 4=right_up
// Left down always resets. Others only reset for SWF < 9.
// Middle mouse never resets (not listed).
void actionResetHighlightForEvent(int event_type);
void actionUpdateHighlightState(void);
// Virtual hover tracking: Tab focus sets g_tab_hovered_mc; mouse events clear it.
// Per-frame ng_update_button_states should be skipped while virtual hover is active.
int actionHasVirtualHover(void);
void actionClearVirtualHover(void);
// End virtual hover: fire rollOut (DoAction + AS2) on Tab-hovered MC, then clear.
// Must be called BEFORE ng_update_button_states/actionDispatchMCMouseMove.
void actionEndVirtualHoverOnMouse(SWFAppContext* app_context);
// Mouse click focus acquisition — on mouse down, check if clicked MC is focusable.
void actionMouseClickFocus(SWFAppContext* app_context);
// Window focus lost — clear keyboard focus, firing onRollOut + onKillFocus.
void actionWindowFocusLost(SWFAppContext* app_context);

// Clipboard and text control operations — called from swf_core.c on input events.
void actionSetClipboardText(const char* text);
void actionTextControlPaste(SWFAppContext* app_context);
void actionTextControlCopy(SWFAppContext* app_context);
void actionTextControlCut(SWFAppContext* app_context);
void actionTextControlSelectAll(SWFAppContext* app_context);
void actionTextControlMoveRight(SWFAppContext* app_context);
void actionTextControlMoveLeft(SWFAppContext* app_context);
void actionTextControlMoveHome(SWFAppContext* app_context);
void actionTextControlMoveEnd(SWFAppContext* app_context);
void actionTextControlEnter(SWFAppContext* app_context);
void actionTextControlBackspace(SWFAppContext* app_context);
void actionTextControlDelete(SWFAppContext* app_context);
void actionTextFieldInput(SWFAppContext* app_context, int codepoint);
int  ng_is_textfield_focused(void);
float ng_get_textfield_scroll_x(void* mc);
void  ng_set_textfield_scroll_x(SWFAppContext* app_context, void* mc, float twips);
