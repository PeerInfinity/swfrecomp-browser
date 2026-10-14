#pragma once

#include <common.h>
#include <swf.h>

// Core tag functions - always available
void tagInit(SWFAppContext* app_context);
// Dispatch button key-press conditions for a given key code.
// Called from swf_core.c after delivering EV_KEY_DOWN.
int dispatch_button_key_actions(SWFAppContext* app_context, int key_code);
// Fire button DoAction conditions for a specific transition on a MC.
// transition_mask: 0x0004 = press, 0x0008 = release
void ng_simulateButtonTransition(SWFAppContext* app_context, void* mc, int transition_mask);
// Set button_state on the display object for a button MC (for Tab virtual hover).
void ng_setButtonDisplayState(void* mc, u8 state);
// Simulate a button press+release on a specific MC (for keyboard focus activation).
void ng_simulateButtonPressRelease(SWFAppContext* app_context, void* mc);
// Run button hit-test + state machine + action dispatch for current mouse state.
// In NO_GRAPHICS mode called per mouse event; in graphics mode called from tagShowFrame.
int ng_update_button_states(SWFAppContext* app_context);

// Recover the original (pre-compose_children) transform_id for a DisplayObject.
// During the render frame, compose_children remaps obj->transform_id to a
// dynamic GPU slot that has no entry in the CPU-side transform_data array;
// callers that need the static transform must use this accessor. Defined in
// tag.c in graphics builds; not available in NO_GRAPHICS.
#ifndef NO_GRAPHICS
u32 ng_get_original_transform_id(DisplayObject* obj);
#endif

// Dispatch CLIP_EVENT_PRESS for all sprites whose hit area contains the mouse.
// Called from swf_core.c on EV_MOUSE_DOWN_LEFT, input_events.c, and from
// swf.c's per-tick mouse handler in browser-WASM.
void dispatch_clip_event_press(SWFAppContext* app_context);
// Dispatch CLIP_EVENT_RELEASE for sprites that had PRESS fired (clip_mc_pressed=1).
// Called from swf_core.c on EV_MOUSE_UP_LEFT and from swf.c in browser-WASM.
void dispatch_clip_event_release(SWFAppContext* app_context);
// Dispatch CLIP_EVENT_ROLL_OVER/ROLL_OUT/DRAG_OVER/DRAG_OUT on hit-area transitions.
// Called from swf_core.c on EV_MOUSE_MOVE and from swf.c in browser-WASM.
void dispatch_clip_event_roll(SWFAppContext* app_context);
// Dispatch a generic clip event flag (CLIP_EVENT_MOUSE_DOWN/UP/MOVE, KEY_DOWN/UP)
// to ALL clips in the display list (global events, no hit-test).
void dispatch_clip_event_flag(SWFAppContext* app_context, uint32_t flag);
#ifdef NO_GRAPHICS
// Compute the _droptarget slash-path for a point (in stage twips), skipping skip_name.
// Writes result to out_path (at most out_size-1 chars + NUL). Returns 1 if found, 0 if not.
int ng_compute_droptarget(float stage_x_twips, float stage_y_twips,
    const char* skip_name, char* out_path, size_t out_size);
#endif
// ---------------------------------------------------------------------------
// CHARACTER ID WRAPPER
//
// Every character id the recompiler writes into generated C goes through
// CHARID() — call arguments (`tagDefineSprite(app_context, CHARID(1), ...)`)
// AND data-table fields alike (`FramePlacement` / `SpriteFrameScriptEntry`
// initialisers). It is the identity macro, so every ordinary build is
// bit-identical in behaviour and CHARID(7) stays a constant expression usable
// in a static initialiser.
//
// It exists so a consumer that must shift a whole movie's dictionary can find
// every id BY VALUE with one substitution: verify_output.py's
// `generate_child_movie_file` re-bases a loaded child SWF's ids by
// `movie_id * 1000`, and used to do it with one regex per emitted call name —
// a hand-maintained list that was both stale (four regexes named calls the
// recompiler no longer emits) and structurally incomplete (a regex keyed on a
// call name cannot see a struct-initialiser field).
//
// If you add an emission site that writes a character id, wrap it.
// `scripts/check_charid_wrapping.py` derives the char-id argument positions
// and struct field indices from THIS header and fails on any bare integer
// literal in one of them.
// ---------------------------------------------------------------------------
#define CHARID(x) (x)

void tagSetBackgroundColor(u8 red, u8 green, u8 blue);
void tagShowFrame(SWFAppContext* app_context);
void tagFlushPendingEnterFrame(SWFAppContext* app_context);

// Tag functions needed by both graphics and trace (NO_GRAPHICS) builds.
void tagDefineShape(SWFAppContext* app_context, CharacterType type, size_t char_id, size_t shape_offset, size_t shape_size,
    s32 bounds_xmin, s32 bounds_xmax, s32 bounds_ymin, s32 bounds_ymax);
void tagDefineMorphShape(SWFAppContext* app_context, size_t char_id,
    size_t shape_offset, size_t shape_size,
    size_t morph_end_offset, size_t morph_color_start, size_t morph_color_count,
    size_t morph_end_color_start,
    s32 bounds_xmin, s32 bounds_xmax, s32 bounds_ymin, s32 bounds_ymax,
    s32 end_bounds_xmin, s32 end_bounds_xmax, s32 end_bounds_ymin, s32 end_bounds_ymax);
void tagDefineText(SWFAppContext* app_context, size_t char_id, size_t text_start, size_t text_size, u32 transform_start, u32 cxform_id, s32 bounds_xmin, s32 bounds_xmax, s32 bounds_ymin, s32 bounds_ymax);
void tagDefineEditTextProps(SWFAppContext* app_context, size_t char_id,
    const char* plain_text, const char* raw_html_text, u32 text_color,
    u16 font_id, u16 font_height, s16 max_length,
    u8 align, u16 left_margin, u16 right_margin, s16 indent, s16 leading,
    const char* variable_name, u16 flags,
    s32 bounds_xmin, s32 bounds_xmax, s32 bounds_ymin, s32 bounds_ymax);
void tagCSMTextSettings(size_t text_id, const char* anti_alias_type, const char* grid_fit_type, float thickness, float sharpness);
// is_replace: 1 if the source SWF tag had both PlaceFlagMove and PlaceFlagHasCharacter
// (Ruffle's PlaceObjectAction::Replace — overwrite the depth's character without firing
// UNLOAD); 0 otherwise (PlaceObjectAction::Place — must fail if the depth is occupied).
// Modify-only tags (char_id=0) ignore is_replace.
void tagPlaceObject2(SWFAppContext* app_context, size_t depth, size_t char_id, u32 transform_id, u32 cxform_id, u16 clip_depth, u8 is_replace);
void tagPlaceObject2Ratio(SWFAppContext* app_context, size_t depth, size_t char_id,
    u32 transform_id, u32 cxform_id, u16 clip_depth, u16 ratio, u8 is_replace);
void tagPlaceObject2RatioWithClipActions(SWFAppContext* app_context, size_t depth, size_t char_id,
    u32 transform_id, u32 cxform_id, u16 clip_depth, u16 ratio, ClipAction* clip_actions, size_t clip_action_count, u8 is_replace);
void tagReplaceObject2RatioWithClipActions(SWFAppContext* app_context, size_t depth, size_t char_id,
    u32 transform_id, u32 cxform_id, u16 clip_depth, u16 ratio,
    ClipAction* old_clip_actions, size_t old_clip_action_count,
    ClipAction* new_clip_actions, size_t new_clip_action_count);
void tagPlaceObject2WithClipActions(SWFAppContext* app_context, size_t depth, size_t char_id,
    u32 transform_id, u32 cxform_id, u16 clip_depth, ClipAction* clip_actions, size_t clip_action_count, u8 is_replace);
// Attach clip actions to a display entry that was already placed (e.g. by tagPlaceObject2Ratio).
void tagSetClipActions(SWFAppContext* app_context, size_t depth, ClipAction* clip_actions, size_t clip_action_count);
void tagPlaceObject3(SWFAppContext* app_context, size_t depth, size_t char_id,
    u32 transform_id, u32 cxform_id, u16 clip_depth, u8 blend_mode, u8 is_replace);
void tagSetFilter(SWFAppContext* app_context, size_t depth,
    u8 type, double blur_x, double blur_y, u8 quality, u8 flags,
    double r, double g, double b, double a, double strength,
    double angle, double distance);
// PlaceObject3 BackgroundColor -> DisplayObject.opaqueBackground.
// `rgb` is 0xRRGGBB; `set` 0 clears (Ruffle drops an alpha==0 colour).
void tagSetOpaqueBackground(SWFAppContext* app_context, size_t depth,
    u8 set, u32 rgb);
// PlaceObject3 BitmapCache -> DisplayObject.cacheAsBitmap.
void tagSetCacheAsBitmap(SWFAppContext* app_context, size_t depth, u8 on);
void tagSetFilterHighlight(SWFAppContext* app_context, size_t depth,
    double r, double g, double b, double a);
void tagSetFilterColorMatrix(SWFAppContext* app_context, size_t depth, const float* matrix20);
void tagSetFilterConvolution(SWFAppContext* app_context, size_t depth,
    u8 matrixX, u8 matrixY, const float* matrix, float divisor, float bias,
    u8 preserve_alpha, u8 clamp, u8 def_r, u8 def_g, u8 def_b, u8 def_a);
void tagSetFilterGradient(SWFAppContext* app_context, size_t depth,
    u8 type, u8 count, const u32* colors, const float* alphas, const u8* ratios,
    float blur_x, float blur_y, float angle, float distance, float strength,
    u8 quality, u8 flags);
// Extended filter data accessor (returns pointer to static data or NULL)
typedef struct ExtFilterData {
    u8 type;          // 5=convolution, 6=colormatrix, 7=gradientglow, 8=gradientbevel
    float cm_matrix[20];
    u8 conv_mx, conv_my;
    float conv_matrix[25];
    float conv_divisor, conv_bias;
    u8 conv_preserve_alpha, conv_clamp;
    u8 conv_color_r, conv_color_g, conv_color_b, conv_color_a;
    u8 grad_count;
    u32 grad_colors[16];
    float grad_alphas[16];
    u8 grad_ratios[16];
    float blur_x, blur_y, angle, distance, strength;
    u8 quality, flags;
} ExtFilterData;
const ExtFilterData* ng_getExtFilterDataByDepth(size_t depth);

// Multi-filter list: stores ALL SWF-authored filters per display entry
typedef struct FilterListEntry {
    u8 type;  // 1=blur, 2=dropshadow, 3=glow, 4=bevel, 5=conv, 6=cm, 7=gradglow, 8=gradbevel
    // Common fields (types 1-4, 7-8)
    double blur_x, blur_y;
    u8 quality, flags;
    double color_r, color_g, color_b, color_a;
    double strength, angle, distance;
    double highlight_r, highlight_g, highlight_b, highlight_a;
    // ColorMatrix (type 6)
    float cm_matrix[20];
    // Convolution (type 5)
    u8 conv_mx, conv_my;
    float conv_matrix[25];
    float conv_divisor, conv_bias;
    u8 conv_preserve_alpha, conv_clamp;
    u8 conv_color_r, conv_color_g, conv_color_b, conv_color_a;
    // Gradient (types 7, 8)
    u8 grad_count;
    u32 grad_colors[16];
    float grad_alphas[16];
    u8 grad_ratios[16];
} FilterListEntry;

#define MAX_FILTER_LIST_SIZE 16
typedef struct FilterListData {
    size_t depth;
    u8 count;
    FilterListEntry entries[MAX_FILTER_LIST_SIZE];
} FilterListData;

void tagBeginFilterList(SWFAppContext* app_context, size_t depth, u8 count);
void tagAddSimpleFilter(SWFAppContext* app_context, size_t depth,
    u8 type, double blur_x, double blur_y, u8 quality, u8 flags,
    double r, double g, double b, double a, double strength,
    double angle, double distance);
void tagAddSimpleFilterHighlight(SWFAppContext* app_context, size_t depth,
    double hr, double hg, double hb, double ha);
void tagAddColorMatrixFilter(SWFAppContext* app_context, size_t depth, const float* matrix20);
void tagAddConvolutionFilter(SWFAppContext* app_context, size_t depth,
    u8 matrixX, u8 matrixY, const float* matrix, float divisor, float bias,
    u8 preserve_alpha, u8 clamp, u8 def_r, u8 def_g, u8 def_b, u8 def_a);
void tagAddGradientFilter(SWFAppContext* app_context, size_t depth,
    u8 type, u8 count, const u32* colors, const float* alphas, const u8* ratios,
    float blur_x, float blur_y, float angle, float distance, float strength,
    u8 quality, u8 flags);
void tagEndFilterList(SWFAppContext* app_context, size_t depth);
const FilterListData* ng_getFilterListDataByDepth(size_t depth);
void tagSetInstanceName(SWFAppContext* app_context, size_t depth, const char* name);
void tagRemoveObject(SWFAppContext* app_context, size_t depth);
void tagRemoveObject2(SWFAppContext* app_context, size_t depth);
void tagDefineSpriteEx(SWFAppContext* app_context, size_t char_id, frame_func* funcs, size_t frame_count, size_t byte_size);
// Backwards-compatible macro: old 4-arg calls get byte_size=0
#define tagDefineSprite_5(ctx, cid, fn, fc, bs) tagDefineSpriteEx(ctx, cid, fn, fc, bs)
#define tagDefineSprite_4(ctx, cid, fn, fc)     tagDefineSpriteEx(ctx, cid, fn, fc, 0)
#define tagDefineSprite_GET(_1, _2, _3, _4, _5, NAME, ...) NAME
#define tagDefineSprite(...) tagDefineSprite_GET(__VA_ARGS__, tagDefineSprite_5, tagDefineSprite_4)(__VA_ARGS__)
// Emitted by the recompiler ONLY for a malformed DefineSprite whose body is not
// terminated by an End record. Such a clip does not loop (see the comment on
// Character.sprite_has_end_tag). Well-formed SWFs never emit this call, so
// their generated output is unchanged.
void tagSetSpriteNoEndTag(SWFAppContext* app_context, size_t char_id);
// Split the DefineSprite header frameCount (kept for _totalframes) from the
// body's real frame-function count and Ruffle's frames_loaded. Emitted only
// for sprites where the three numbers are not all equal.
void tagSetSpriteFrameCounts(SWFAppContext* app_context, size_t char_id,
                             size_t func_count, size_t loaded_frames);
void tagDefineButton(SWFAppContext* app_context, size_t char_id, frame_func* state_funcs, size_t hit_char_id, u32 hit_transform_id, ButtonAction* actions, size_t action_count);
// `data` is the EMITTING movie's own bitmap_data array and `offset` a byte
// offset into it. A loaded child SWF has its own array (renamed
// <prefix>_bitmap_data by the child-movie wrapper), so the offset alone does
// not identify the pixels — the base has to travel with it.
void defineBitmap(const u8* data, size_t offset, size_t size, u32 width, u32 height, u16 char_id);
void finalizeBitmaps();
void tagDefineSound(SWFAppContext* app_context, u16 sound_id,
    u8 format, u8 rate, u8 sample_size, u8 stereo,
    u32 sample_count, const u8* data, size_t data_size);
void tagStartSound(SWFAppContext* app_context, u16 sound_id,
    int stop, u32 loop_count, u32 in_point, u32 out_point);
void tagSoundStreamHead(SWFAppContext* app_context,
    u8 format, u8 rate, u8 sample_size, u8 stereo,
    u16 avg_sample_count);
void tagSoundStreamBlock(SWFAppContext* app_context,
    const u8* data, size_t data_size);
void tagStopAllSounds(SWFAppContext* app_context);
void tagScriptLimits(u16 max_recursion, u16 timeout);
void tagDefineFontInfo(SWFAppContext* app_context, u16 font_id, const char* name, int bold, int italic);
void tagDefineFontMetrics(SWFAppContext* app_context, u16 font_id,
    s16 ascent, s16 descent, s16 leading, int em_square,
    const u16* code_table, const s32* advance_table, size_t glyph_count);
void tagDefineFontGlyphBase(u16 font_id, size_t glyph_base);
// `deblocking` is the tag's VideoFlagsDeblocking field (UB[3]) — a
// VIDEO_DEBLOCK_* value from actionmodern/video_codec.h.
void tagDefineVideoStream(SWFAppContext* app_context, u16 char_id, u16 width, u16 height,
    u8 deblocking, u8 codec_id);
void tagVideoFrame(SWFAppContext* app_context, u16 char_id, u16 frame_num,
    const unsigned char* payload, size_t payload_size);
void tagRegisterExport(SWFAppContext* app_context, const char* name, size_t char_id);
void tagImportCharacter(SWFAppContext* app_context, size_t local_char_id, const char* export_name);

// Run a DoInitAction script, guarded by once-per-character-id.
// Used for DoInitAction tags inside DefineSprite definitions.
void tagDoInitActionGuarded(SWFAppContext* app_context, size_t char_id, frame_func action);

// Forward declaration (defined in actionmodern/action.h)
typedef struct MovieClip MovieClip;
// ActionVar is defined in actionmodern/variables.h. Include via direct path
// rather than re-declaring (it's an anonymous-struct typedef, so a forward
// declaration with the same name doesn't link).
#include <actionmodern/variables.h>

// Frame label entry for label→frame mapping
typedef struct {
	const char* label;
	size_t frame;
} FrameLabelEntry;

// Register per-sprite frame labels (called from tagInit after tagDefineSprite)
void tagSetSpriteLabels(size_t char_id, FrameLabelEntry* labels, size_t count);
// Find a frame label in a sprite's label table. Returns 0-based frame or -1 if not found.
int ng_findSpriteLabelFrame(size_t char_id, const char* label);

// Per-sprite per-frame placement metadata. One entry per PlaceObject2/3 (and
// RemoveObject/2) inside the sprite's frame body. Used by the runtime to
// compute survives_rewind on sprite loop-back, mirroring Ruffle's run_goto.
typedef struct {
	u16 depth;
	u16 char_id;       // 0 means "Modify" (PlaceObject2 with no character)
	u16 ratio;
	u8  is_remove;     // 1 for RemoveObject/RemoveObject2 entries; 0 for places
	u8  has_clip_actions;
} FramePlacement;

// Register per-sprite per-frame placement table. frame_starts has length
// frame_count+1 (cumulative offsets into placements; final entry = total).
void tagSetSpritePlacements(u16 sprite_id, FramePlacement* placements,
                            u16* frame_starts, u16 frame_count);

// Lookup helper: returns pointer to first placement of frame `frame` for
// sprite `sprite_id`, and writes the count to *out_count. Returns NULL and
// sets *out_count = 0 if no table is registered or the frame is out of range.
const FramePlacement* ng_sprite_frame_placements(u16 sprite_id, u16 frame, u16* out_count);
// Navigate a MovieClip's sprite to a given 0-based frame.
// Returns 1 if sprite found and navigated, 0 if not found.
int ng_gotoFrameByMC(SWFAppContext* app_context, MovieClip* mc, u16 frame, int play);
// Get the character ID for a MovieClip's display entry.
size_t ng_getCharIdByMC(MovieClip* mc);
// Get the DefineSprite tag data size for a sprite character.
size_t ng_getSpriteByteSize(size_t char_id);

// Functions needed by action.c / tag.c in all build modes (NO_GRAPHICS, HEADLESS, GRAPHICS).
// Implementations live in tag_stubs.c (NO_GRAPHICS/HEADLESS) or swf.c stubs (GRAPHICS).
u16 ng_findFontIdByName(const char* name);
void ng_getTextExtent(u16 font_id, double font_size_px, const char* text, size_t text_len,
    double width_px, double* out_ascent, double* out_descent,
    double* out_width, double* out_height, double* out_tf_height, double* out_tf_width);
size_t ng_lookupExport(const char* name);
size_t ng_lookupExportForMovie(const char* name, u8 movie_id);
int ng_lookupExportVersion(const char* name);
int ng_lookupExportVersionForMovie(const char* name, u8 movie_id);
int32_t ng_getSoundDuration(u16 char_id);
// The registry records WHICH array the pixels are in, not just where in one:
// `pixels` already has the emitting movie's base folded in.
int ng_getBitmapMetadata(u16 char_id, const u8** out_pixels, size_t* out_size, u32* out_width, u32* out_height);
void ng_registerBitmapMetadata(u16 char_id, const u8* pixels, size_t size, u32 width, u32 height);
extern u8 g_current_movie_id;
void ng_on_place_object2(SWFAppContext* app_context, size_t depth, size_t char_id);
void ng_on_remove_object(SWFAppContext* app_context, size_t depth);
size_t ng_findDisplayEntryByName(const char* name);

// Currently-executing sprite DisplayObject (set by advance_sprite_frames in NO_GRAPHICS,
// or defined as dummy in swf.c for GRAPHICS mode). Used by action.c in all modes.
extern DisplayObject* g_current_sprite_obj;

// NO_GRAPHICS helpers for sprite timeline control from action.c
// (was: #ifdef NO_GRAPHICS — un-gated so tag_stubs.c symbols are declared in graphics builds)
// Advance sprite timelines (replaces old ng_advanceSprites; called by swf_core.c)
void advance_sprite_frames(SWFAppContext* app_context);
// Advance only nested sprites within root-level sprites (deferred recursion).
// Call after advance_sprite_frames and the root frame script to match Ruffle's
// execution order: root-level sprites → root → nested children.
void advance_nested_sprite_frames(SWFAppContext* app_context);
// Returns 1 if any multi-frame sprite at root level is still playing
int hasPlayingSprites(void);
// Returns 1 if any initialized sprite has CLIP_EVENT_ENTER_FRAME clip actions
int hasClipEnterFrameHandlers(void);

int ng_isInsideSprite(void);
void ng_stopCurrentSprite(void);
void ng_playCurrentSprite(void);

// Root DL swap for inline catch-up from sprite init context
int ng_isInsideSpriteInit(void);
void ng_bumpSpriteInitDepth(void);
void ng_unbumpSpriteInitDepth(void);
int ng_swapToRootDL(DisplayObject** saved_dl, size_t* saved_max, size_t* saved_cap);
void ng_restoreFromRootDL(DisplayObject* saved_dl, size_t saved_max, size_t saved_cap);

// Sprite display-list reallocation with aliased-pointer rebase (see the
// ALIASING RULE comment at DisplayObject.sprite_display_list in swf.h).
// ng_spriteDLRealloc: grow *base_io to new_cap entries, rebase every
// registered holder of the old buffer (entry pointers and base copies),
// then FREE the old buffer. ng_ensureDisplayListSize: ENSURE_SIZE-style
// doubling wrapper for the global display_list (root or a swapped-in
// sprite list).
void ng_spriteDLRealloc(SWFAppContext* app_context, DisplayObject** base_io,
                        size_t* cap_io, size_t new_cap);
void ng_ensureDisplayListSize(SWFAppContext* app_context, size_t depth);
// The ONLY way to free a sprite_display_list buffer: scrubs (NULLs) every
// registered holder of pointers into it, then FREEs. Raw FREE() strands them.
void ng_freeSpriteDL(SWFAppContext* app_context, DisplayObject* base, size_t cap);
void ng_gotoFrameCurrentSprite(u16 frame);
void ng_record_sprite_self_goto(DisplayObject* obj);
void ng_apply_pending_sprite_self_gotos(SWFAppContext* app_context);
size_t ng_getSpriteFrameCount(void);
size_t ng_findDisplayEntryByName(const char* name);
// Find a display entry at the given SWF depth that is a direct child of root.
// Returns: 0=not found, 1=non-sprite (shape/text), 2=sprite (name written to out_name)
int ng_findRootChildAtSWFDepth(size_t swf_depth, char* out_name, size_t out_name_size);
size_t ng_findChildEntryDepth(const char* parent_name, const char* child_name);
void ng_updateDisplayDepth(const char* name, int new_as_depth);
void ng_swapDisplayDepths(const char* name1, const char* name2);
void ng_renameDisplayEntry(const char* old_name, const char* new_name);
void ng_display_clear_after(SWFAppContext* app_context, size_t target_frame);
int ng_isSpriteAtDepth(size_t depth);
int ng_isButtonAtDepth(size_t depth);
int ng_isTextFieldAtDepth(size_t depth);
const char* ng_getTextFieldInitialText(size_t depth);
u32 ng_getTextFieldColor(size_t depth);
u32 ng_getTextFieldColorByIdx(int idx);
int ng_getTextFieldIdx(size_t depth);
int ng_getCharTextfieldIdx(size_t char_id);
u16 ng_getTextFieldFlags(int tf_idx);
u16 ng_getTextFieldFontId(int tf_idx);
u16 ng_getTextFieldFontHeight(int tf_idx);
s16 ng_getTextFieldMaxLength(int tf_idx);
u8 ng_getTextFieldAlign(int tf_idx);
u16 ng_getTextFieldLeftMargin(int tf_idx);
u16 ng_getTextFieldRightMargin(int tf_idx);
s16 ng_getTextFieldIndent(int tf_idx);
s16 ng_getTextFieldLeading(int tf_idx);
const char* ng_getTextFieldVariableName(int tf_idx);
void ng_getTextFieldBounds(int tf_idx, s32* xmin, s32* xmax, s32* ymin, s32* ymax);
const char* ng_getTextFieldRawHtml(int tf_idx);
const char* ng_getTextFieldInitialTextByIdx(int tf_idx);
const char* ng_getFontName(u16 font_id);
const char* ng_getFontNameSWFDefined(u16 font_id);
// Returns the font_id of a synthesized device-font ("_sans"/"_serif"/
// "_typewriter") that carries real glyph outlines (Phase-A compile-time
// synthesis fills these for zero-glyph named fonts), or -1 if none exists.
// Used as the render fallback for device-font EditTexts whose referenced
// embedded font only carries a partial glyph subset.
int ng_find_device_fallback_font(void);
int ng_getFontBold(u16 font_id);
int ng_getFontItalic(u16 font_id);
int ng_getTransformId(size_t depth, u32* out_id);
int ng_getTransformXY(size_t depth, float* out_x, float* out_y);
int ng_getTransformXY_d(size_t depth, double* out_x, double* out_y);
int ng_getTransformScaleRotation(size_t depth, float* out_xscale, float* out_yscale, float* out_rotation);
// Decomposed scale/rotation/skew. Out_skew is in radians (Ruffle parity:
// rotation = atan2(b,a), skew = atan2(-c,d) - rotation).
int ng_getTransformScaleRotationSkew(size_t depth, float* out_xscale, float* out_yscale, float* out_rotation, float* out_skew);
int ng_getColorTransform(const char* name, double* ra, double* ga, double* ba, double* aa,
                          double* rb, double* gb, double* bb, double* ab);
int ng_setColorTransform(const char* name, double ra, double ga, double ba, double aa,
                          double rb, double gb, double bb, double ab);
// Set only the alpha multiplier (cx_aa) on a display list entry. Used for _alpha ↔ Color sync.
void ng_setCTAlpha(size_t depth, double aa);
int ng_getCharBounds(size_t char_id, s32* out_xmin, s32* out_xmax, s32* out_ymin, s32* out_ymax);
// Compute union of child bounds in a sprite's display list, in LOCAL twips.
// Accounts for child transforms (scale, translation). Only available in NO_GRAPHICS builds.
int sprite_content_bounds_twips(DisplayObject* dl, size_t dl_max,
    float* xmin_out, float* xmax_out, float* ymin_out, float* ymax_out);
int ng_getCharBoundsForRatio(size_t char_id, u16 ratio,
    s32* out_xmin, s32* out_xmax, s32* out_ymin, s32* out_ymax);

// ---------------------------------------------------------------------------
// Pointer-form display-entry accessors (bounds-engine unification, 2026-05-28).
// These supersede the entry_idx-encoded variants above; callers resolve a
// DisplayObject* directly (arbitrary depth, no 2-level encoding cap).
// ---------------------------------------------------------------------------
// Flat root depth of a display entry, or (size_t)-1 if NULL / not a root-level
// entry. Used to key the depth-keyed filter side-tables off a DisplayObject*.
size_t ng_objRootDepth(DisplayObject* obj);
int ng_getMatrixFromObj(DisplayObject* obj, double* out_a, double* out_b, double* out_c, double* out_d, double* out_tx, double* out_ty);
int ng_getMatrixFromObj_render(DisplayObject* obj, float* out_a, float* out_b, float* out_c, float* out_d, int32_t* out_tx_twips, int32_t* out_ty_twips);
// Same read with a caller-supplied transform-table row — needed inside a render
// frame, where compose_children has rewritten a child entry's transform_id to a
// GPU-only slot (resolve with ng_get_original_transform_id first).
int ng_getMatrixFromObj_render_tid(DisplayObject* obj, u32 tid, float* out_a, float* out_b, float* out_c, float* out_d, int32_t* out_tx_twips, int32_t* out_ty_twips);
int ng_getCTFromObj(DisplayObject* obj, double* ra, double* ga, double* ba, double* aa, double* rb, double* gb, double* bb, double* ab);
// Populate a display entry's cx_* fields from the baked cxform_data slot.
void ng_init_cxform_from_data(DisplayObject* obj, u32 cxform_id);
int ng_setCTOnObj(DisplayObject* obj, double ra, double ga, double ba, double aa, double rb, double gb, double bb, double ab);
int ng_getObjFilterData(DisplayObject* obj, u8* type, float* blur_x, float* blur_y,
    u8* quality, u8* flags, float* r, float* g, float* b, float* a,
    float* strength, float* angle, float* distance,
    float* hr, float* hg, float* hb, float* ha);
// Local content bounds (twips, double) of a display list, queried object's own
// transform NOT applied. == ng_computeBoundsFromDL_matrix with identity.
int ng_localBoundsOfDL(DisplayObject* dl, size_t dl_max,
    double* out_xmin, double* out_ymin, double* out_xmax, double* out_ymax);
// TextField variable binding — called from tag_stubs.c at placement time
void actionInitTextFieldVariable(SWFAppContext* app_context, const char* var_name, const char* init_text);

// Phase B (textfield-variable-binding-plan.md): try to bind a placed
// TextField's variable to its parent's scope.
//   set_initial_value=1: at placement / wrapper creation. Initial-value
//     direction matches Flash: variable wins if defined, else TF.text seeds
//     the variable.
//   set_initial_value=0: retry from the unbound queue (variable was already
//     handled when the TF was first queued; just confirm the path resolves).
// Returns 1 if binding resolved (or no variable to bind); 0 if the path
// can't be resolved yet (caller queues the TF for retry).
int actionTryBindTextFieldVariable(SWFAppContext* app_context, MovieClip* tf_mc, int set_initial_value);

// Retry every TF in the unbound queue. Drains those whose path now resolves.
// Called per-tick from the frame loop, mirroring Ruffle's bind_variables
// invocation from MovieClip::run_frame_avm1.
int actionRetryUnboundTextFields(SWFAppContext* app_context);

// Push a TF MC onto the unbound queue (called from placement when the bind
// path can't be resolved yet). Idempotent.
void actionUnboundTextFieldsPush(MovieClip* mc);

// Drop a freed/unloaded MC from the unbound queue (called from MC destruction
// sites to avoid retrying dangling pointers).
void actionUnboundTextFieldsDrop(MovieClip* mc);

// Phase C: per-container TextField binding registry.
// actionRegisterTextFieldBinding records (tf, var_name) on the container;
// actionNotifyPropertyChange propagates a property write to bound TFs;
// actionUnregisterTextFieldBindings drains all bindings on container unload
// (and pushes the TFs back to the unbound retry queue).
void actionRegisterTextFieldBinding(MovieClip* container, MovieClip* tf_mc, const char* variable_name);
int  actionNotifyPropertyChange(SWFAppContext* app_context, MovieClip* container,
	const char* prop_name, u32 prop_name_len, ActionVar* value);
void actionUnregisterTextFieldBindings(MovieClip* container);
void actionDropTextFieldBindingForTF(MovieClip* container, MovieClip* tf_mc);
// Rename a cached MovieClip when tagSetInstanceName updates a sprite's display entry
void actionRenameMovieClip(const char* old_name, const char* new_name);
// Enumerate child instance names for a MovieClip (for for-in enumeration)
// callback receives (name, name_len, user_data) for each child
void ng_enumerateChildren(const char* parent_name, void (*callback)(const char* name, u32 name_len, void* user_data), void* user_data);
// Phase 1e: yield names of children removed in the most recent button-state
// transition. button_dobj is the button's DisplayObject; if it doesn't match
// the most recent transitioned button, no callbacks fire.
void ng_iterateTransientButtonChildren(void* button_dobj, void (*callback)(const char* name, u32 name_len, void* user_data), void* user_data);
// Phase 1g: returns 1 iff button_dobj is the most recent transitioned button
// AND `name` (case-sensitive) matches one of the transient (just-removed)
// child instance names. Used by actionGetMember to resolve direct property
// access (`button.instance6`) on a child whose dl entry has been replaced
// in the new state and whose MC is now flagged avm1_removed.
int ng_isTransientButtonChildName(void* button_dobj, const char* name, size_t name_len);
// Check if a tag-placed display entry at the given root depth is scriptable (sprite/button/textfield)
int ng_isScriptableAtDepth(size_t depth);
// Clone a tag-placed sprite to target_name at AS depth. `parent` is the clone's
// intended parent (the source's timeline owner / caller context); NULL → _root.
// Returns clone MC, or NULL if non-scriptable/not found.
MovieClip* ng_cloneSprite(SWFAppContext* app_context, const char* source_name, const char* target_name, int depth, MovieClip* parent);
// Clone a script-created MovieClip (not in ng_display) to target_name at AS depth.
// The clone is parented under src_mc->parent (AVM1 sibling clone semantics).
MovieClip* ng_cloneSpriteFromMC(SWFAppContext* app_context, MovieClip* src_mc, const char* target_name, int depth);
// duplicateMovieClip clone: stores at SWF depth (as_depth+16384), no onLoad.
// `parent` is the clone's intended parent (the source's parent); NULL → _root.
MovieClip* ng_duplicateMovieClip(SWFAppContext* app_context, const char* source_name, const char* target_name, int as_depth, MovieClip* parent);

// Active transform data pointer — swapped to child SWF's array during child movie init.
// Defined in tag.c (NO_GRAPHICS builds only). NULL = use main SWF's transform_data.
extern float (*g_active_transform_data)[16];

// The transform table `obj->transform_id` indexes, or NULL when that is the
// main movie's `transform_data`. Defined in tag.c (all build modes).
float (*ng_entryChildTransformData(const DisplayObject* obj))[16];
// Same, resolved to a usable table (main movie's when there is no override).
float (*ng_entryTransformData(const DisplayObject* obj))[16];

// (was: #endif — see comment above)

// --- Shared ng_* declarations (from ng_shared.c, always compiled) ---
// Register a child movie's transform data for sprite frame execution.
void ng_registerMovieTransformData(u8 movie_id, float (*td)[16]);

// Callbacks from tag.c → ng_shared.c supplemental registries (all build modes)
void ng_record_char_bounds(size_t char_id, s32 xmin, s32 xmax, s32 ymin, s32 ymax);
void ng_record_morph_end_bounds(size_t char_id, s32 xmin, s32 xmax, s32 ymin, s32 ymax);
// Record that a shape uses non-zero winding rule (DefineShape4 UsesFillWindingRule flag)
void ng_record_char_winding(size_t char_id);
// Record vector path data for a shape (for path-based hit testing)
void ng_record_char_path(size_t char_id, size_t path_offset, size_t path_size);
// Record interleaved morph path data (for interpolated hit testing)
void ng_record_morph_path(size_t char_id, size_t path_offset, size_t path_size);
// The path_data table ng_find_char_path's offset indexes for this character:
// the DEFINING movie's, or NULL when that is the main movie's generated array.
const float (*ng_findCharPathTable(size_t char_id))[3];
// Same, for a MORPH character (ng_find_morph_path's offsets).
const float (*ng_findMorphPathTable(size_t char_id))[3];
// Record which path_data table a static-text character's GLYPH path offsets
// index. No path run of its own; a no-op for the main movie.
void ng_record_text_path_table(size_t char_id);

// ---------------------------------------------------------------------------
// Combined per-movie render tables (multi-SWF render slice, ng_shared.c)
// ---------------------------------------------------------------------------
// Concatenate every linked movie's geometry/style arrays into one set, so a
// loaded child's shapes reach the GPU and the CPU hit tester with indices that
// mean something. No-op for a build with no child movies. Call before the
// renderer reads app_context's tables.
void ng_buildMovieRenderTables(SWFAppContext* app_context);
// Pre-declare child movies' static bitmaps into the renderer's slot table.
// Call after renderer_init and before the root's tagInit finalizes the pools.
void ng_predeclareChildBitmaps(void);
// The combined vertex table, or NULL when this build never built one.
const u32 (*ng_combinedShapeData(void))[4];
// Base of movie_id's rows in the combined tables (movie 0 = main = 0).
u32 ng_movieShapeVertBase(u8 movie_id);
u32 ng_movieBitmapBase(u8 movie_id);
u32 ng_movieTextBase(u8 movie_id);
u32 ng_movieGlyphBase(u8 movie_id);
u32 ng_movieColorBase(u8 movie_id);
u32 ng_movieMorphEndVertBase(u8 movie_id);
u32 ng_movieMorphEndColorBase(u8 movie_id);
// Define-time bases into the placement tables. tagDefineText's glyph transform
// slots and colour transform are DEFINING-movie ids, unlike a placement's.
u32 ng_movieTransformBase(u8 movie_id);
u32 ng_movieCxformBase(u8 movie_id);
// Base for the movie whose OWN transform_data array is `td` (NULL/main = 0).
u32 ng_movieTransformBaseForTable(const float (*td)[16]);
u32 ng_movieCxformBaseForTable(const float (*td)[16]);
// The combined placement-matrix table, or NULL when none was built.
const float (*ng_combinedTransformData(void))[16];
size_t ng_combinedTransformRows(void);
// The combined colour-transform table (20 floats per slot), or NULL.
const float* ng_combinedCxformData(void);
// The combined static-text tables, or NULL when none was built. Built in every
// build mode -- the CPU glyph hit tester and TextSnapshot read them.
const u32 (*ng_combinedGlyphData(void))[1];
size_t ng_combinedGlyphRows(void);
const u32* ng_combinedTextData(void);
const u16* ng_combinedTextCharCodes(void);
// The combined morph END tables, or NULL when none was built. Built in every
// mode: the AVM2 morph raster and the AVM2 exact hit test read them under
// NO_GRAPHICS too.
const float (*ng_combinedMorphEndShapeData(void))[2];
const float (*ng_combinedMorphEndColorData(void))[4];
// The combined fill-style tables, or NULL when none was built. Only the AVM2
// raster/morph walk needs these accessors: every AVM1 reader holds
// app_context's pointers, which the combine pass rewrites in place.
const float (*ng_combinedColorData(void))[4];
const u8 (*ng_combinedGradientData(void))[4];
const float* ng_combinedUninvMatData(void);
// 1 once the combine pass has actually run with child tables to fold in.
int ng_movieRenderTablesActive(void);

// (was: #ifdef NO_GRAPHICS — un-gated; same reason as above)
// Shape-accurate point-in-shape test for a display list.
// Walks the display list recursively; for leaf shapes, tests the test point against triangles.
// ma..mty is the accumulated parent matrix (twips space).
// test_x/test_y are the test point in twips.
// Returns: 1 = hit, 0 = miss.
int ng_hitTestShapeFromDL(DisplayObject* dl, size_t dl_max,
    double ma, double mb, double mc, double md, double mtx, double mty,
    double test_x, double test_y);
void ng_record_button(size_t char_id);
void ng_record_textfield_props(SWFAppContext* app_context, size_t char_id,
    const char* plain_text, const char* raw_html_text, u32 text_color,
    u16 font_id, u16 font_height, s16 max_length,
    u8 align, u16 left_margin, u16 right_margin, s16 indent, s16 leading,
    const char* variable_name, u16 flags,
    s32 bounds_xmin, s32 bounds_xmax, s32 bounds_ymin, s32 bounds_ymax);
void ng_record_csm(size_t text_id, const char* anti_alias_type, const char* grid_fit_type,
    float thickness, float sharpness);
void ng_record_font(SWFAppContext* app_context, u16 font_id, const char* name, int bold, int italic);
void ng_record_font_metrics(SWFAppContext* app_context, u16 font_id,
    s16 ascent, s16 descent, s16 leading, int em_square,
    const u16* code_table, const s32* advance_table, size_t glyph_count);
void ng_record_font_glyph_base(u16 font_id, size_t glyph_base);
int ng_find_font_with_metrics(u16 font_id);
int ng_font_find_glyph(int font_idx, u16 code_point);
size_t ng_font_get_glyph_base(int font_idx);
int ng_font_is_builtin(int font_idx);
int ng_font_get_metrics(int font_idx, s16* ascent, s16* descent, int* em_square);
s32 ng_font_glyph_advance_by_idx(int font_idx, int glyph_idx);
void ng_setTabStops(const int* stops_twips, int count, u16 font_height);
void ng_clearTabStops(void);
// `scroll_lines` is the field's AVM1 `scroll` property minus one — the number
// of whole lines scrolled off the top of the view. 0 for an unscrolled field.
int ng_getCharIndexAtPoint(int tf_idx, float local_x_px, float local_y_px,
                           const char* text, size_t text_len, int scroll_lines);
int ng_computeTextWidth(u16 font_id, u16 font_height, const char* text, size_t text_len,
    int word_wrap, int field_width_twips, int swf_version,
    int left_margin_twips, int right_margin_twips, int indent_twips, int align,
    int letter_spacing_twips);
int ng_computeTextHeight(u16 font_id, u16 font_height, s16 leading_twips, const char* text, size_t text_len,
    int word_wrap, int field_width_twips, int swf_version,
    int left_margin_twips, int right_margin_twips, int indent_twips,
    int letter_spacing_twips);
int ng_computeTextLineCount(u16 font_id, u16 font_height, const char* text, size_t text_len,
    int word_wrap, int field_width_twips, int swf_version,
    int left_margin_twips, int right_margin_twips, int indent_twips,
    int letter_spacing_twips);
int ng_computeVisibleLines(u16 font_id, u16 font_height, s16 leading_twips, float field_height_pixels);
void ng_computeScrollMixedFont(u16 font_id, u16 base_font_height, s16 leading_twips,
    const char* text, size_t text_len,
    int word_wrap, int field_width_twips, int swf_version,
    int left_margin_twips, int right_margin_twips, int indent_twips,
    int letter_spacing_twips,
    float field_height_pixels, int scroll,
    const u32* run_starts, const u32* run_lengths, const u16* run_font_heights, int run_count,
    int is_input, /* keep an empty last line in the scroll window (input fields) */
    int* out_maxscroll, int* out_bottomscroll, int* out_text_height_twips);
// ng_findFontIdByName, ng_getTextExtent, ng_lookupExport, ng_getSoundDuration
// are declared above (outside #ifdef NO_GRAPHICS) since action.c needs them in all modes.
void ng_record_video(SWFAppContext* app_context, u16 char_id, u16 width, u16 height,
                     u8 codec_id, u8 deblocking);
int ng_isVideoChar(size_t char_id);
int ng_getVideoDimensions(size_t char_id, u16* out_w, u16* out_h);
u8 ng_getVideoCodec(size_t char_id);
u8 ng_getVideoDeblocking(size_t char_id);
// Attach a library symbol by char_id. Returns created MC, or NULL if not sprite.
MovieClip* ng_attachMovie(SWFAppContext* app_context, size_t char_id, const char* new_name, int as_depth, MovieClip* parent);
// Called after tagPlaceObject2 places an object (handles auto-naming, MC creation, textfield init)
void ng_on_place_object2(SWFAppContext* app_context, size_t depth, size_t char_id);
// Called before tagRemoveObject clears an object (handles MC invalidation, cleanup)
void ng_on_remove_object(SWFAppContext* app_context, size_t depth);
// Reclaim auto-instance counter slot if auto_name was the last assigned name
void ng_try_reclaim_auto_instance_name(const char* auto_name);
// Fire queued onLoad events for duplicated clips (called from tagShowFrame)
void ng_fire_pending_loads(SWFAppContext* app_context);
// Fire deferred init scripts for attachMovie clips (called from tagShowFrame)
void ng_fire_pending_attach_inits(SWFAppContext* app_context);
// Queue INIT/CONSTRUCT/LOAD clip events for a placement or clone (mirrors tagPlaceObject2).
void ng_queue_placement_clip_events(SWFAppContext* app_context, size_t depth);
// Queue UNLOAD clip-action callbacks for a slot (mirrors tagRemoveObject2).
void ng_queue_slot_unload_events(SWFAppContext* app_context, size_t depth, MovieClip* mc);
// (was: #endif — see comment above)
