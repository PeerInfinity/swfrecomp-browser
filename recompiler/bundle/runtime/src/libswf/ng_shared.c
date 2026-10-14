// ng_shared.c — shared ng_* registry functions compiled in ALL build modes
// (NO_GRAPHICS and GRAPHICS/USE_WEBGPU)
//
// These functions provide access to SWF definition registries (fonts, textfields,
// exports, character bounds/paths, sounds, videos, buttons) that are populated
// during tag processing and queried by action.c and tag.c at runtime.
//
// NO #ifdef guard — this file is always compiled.

#include <tag.h>
#include <swf.h>
#include <common.h>
#include <action.h>
#include <object.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>

// ---------------------------------------------------------------------------
// External globals (defined per build mode: tag_stubs.c / swf.c / swf_core.c)
// ---------------------------------------------------------------------------
extern Character* dictionary;
extern DisplayObject* display_list;
extern size_t max_depth;
extern size_t display_list_capacity;
extern size_t dictionary_capacity;

// ---------------------------------------------------------------------------
// Root display object sentinel (allows child lookup on root_movieclip)
// ---------------------------------------------------------------------------
static DisplayObject ng_root_display_obj;

void ng_sync_root_display_obj(void)
{
	ng_root_display_obj.sprite_display_list = display_list;
	ng_root_display_obj.sprite_max_depth    = max_depth;
}

void* ng_get_root_display_obj(void)
{
	return &ng_root_display_obj;
}

// ---------------------------------------------------------------------------
// Character bounds registry (shapes/morph shapes)
// ---------------------------------------------------------------------------
#define MAX_CHAR_BOUNDS_NG 256
static struct {
	size_t char_id;
	s32 xmin, xmax, ymin, ymax;  // twips
} ng_char_bounds[MAX_CHAR_BOUNDS_NG];
static size_t ng_char_bounds_count = 0;

#define MAX_MORPH_END_BOUNDS_NG 64
static struct {
	size_t char_id;
	s32 xmin, xmax, ymin, ymax;  // twips (end shape bounds)
} ng_morph_end_bounds[MAX_MORPH_END_BOUNDS_NG];
static size_t ng_morph_end_bounds_count = 0;

void ng_record_char_bounds(size_t char_id, s32 xmin, s32 xmax, s32 ymin, s32 ymax)
{
	// Idempotent per char_id: a character is defined once per id (last-wins,
	// matching dictionary[].type). Without this, define tags that re-execute
	// (e.g. browser-WASM frame_func re-runs, where DefineShape lives in the
	// re-running root frame functions) append duplicates and overflow the
	// fixed table, silently dropping later-defined chars — which broke Pong's
	// ball-vs-paddle hitTest (the ball's collision shape charId 63 fell off
	// the end, so ng_getCharBounds returned 0 and the AABB was empty).
	for (size_t i = 0; i < ng_char_bounds_count; i++) {
		if (ng_char_bounds[i].char_id == char_id) {
			ng_char_bounds[i].xmin = xmin; ng_char_bounds[i].xmax = xmax;
			ng_char_bounds[i].ymin = ymin; ng_char_bounds[i].ymax = ymax;
			return;
		}
	}
	if (ng_char_bounds_count >= MAX_CHAR_BOUNDS_NG) return;
	ng_char_bounds[ng_char_bounds_count].char_id = char_id;
	ng_char_bounds[ng_char_bounds_count].xmin = xmin;
	ng_char_bounds[ng_char_bounds_count].xmax = xmax;
	ng_char_bounds[ng_char_bounds_count].ymin = ymin;
	ng_char_bounds[ng_char_bounds_count].ymax = ymax;
	ng_char_bounds_count++;
}

void ng_record_morph_end_bounds(size_t char_id, s32 xmin, s32 xmax, s32 ymin, s32 ymax)
{
	// Idempotent per char_id (see ng_record_char_bounds).
	for (size_t i = 0; i < ng_morph_end_bounds_count; i++) {
		if (ng_morph_end_bounds[i].char_id == char_id) {
			ng_morph_end_bounds[i].xmin = xmin; ng_morph_end_bounds[i].xmax = xmax;
			ng_morph_end_bounds[i].ymin = ymin; ng_morph_end_bounds[i].ymax = ymax;
			return;
		}
	}
	if (ng_morph_end_bounds_count >= MAX_MORPH_END_BOUNDS_NG) return;
	ng_morph_end_bounds[ng_morph_end_bounds_count].char_id = char_id;
	ng_morph_end_bounds[ng_morph_end_bounds_count].xmin = xmin;
	ng_morph_end_bounds[ng_morph_end_bounds_count].xmax = xmax;
	ng_morph_end_bounds[ng_morph_end_bounds_count].ymin = ymin;
	ng_morph_end_bounds[ng_morph_end_bounds_count].ymax = ymax;
	ng_morph_end_bounds_count++;
}

int ng_getCharBounds(size_t char_id, s32* out_xmin, s32* out_xmax, s32* out_ymin, s32* out_ymax)
{
	for (size_t i = 0; i < ng_char_bounds_count; i++)
	{
		if (ng_char_bounds[i].char_id == char_id)
		{
			if (out_xmin) *out_xmin = ng_char_bounds[i].xmin;
			if (out_xmax) *out_xmax = ng_char_bounds[i].xmax;
			if (out_ymin) *out_ymin = ng_char_bounds[i].ymin;
			if (out_ymax) *out_ymax = ng_char_bounds[i].ymax;
			return 1;
		}
	}
	return 0;
}

int ng_getCharBoundsForRatio(size_t char_id, u16 ratio,
    s32* out_xmin, s32* out_xmax, s32* out_ymin, s32* out_ymax)
{
	if (!ng_getCharBounds(char_id, out_xmin, out_xmax, out_ymin, out_ymax))
		return 0;
	if (ratio == 0)
		return 1;
	for (size_t i = 0; i < ng_morph_end_bounds_count; i++)
	{
		if (ng_morph_end_bounds[i].char_id == char_id)
		{
			double t = (double)ratio / 65535.0;
			s32 sx = *out_xmin, sy = *out_ymin;
			s32 ex = *out_xmax, ey = *out_ymax;
			s32 esx = ng_morph_end_bounds[i].xmin;
			s32 eex = ng_morph_end_bounds[i].xmax;
			s32 esy = ng_morph_end_bounds[i].ymin;
			s32 eey = ng_morph_end_bounds[i].ymax;
			*out_xmin = (s32)(sx + (esx - sx) * t);
			*out_xmax = (s32)(ex + (eex - ex) * t);
			*out_ymin = (s32)(sy + (esy - sy) * t);
			*out_ymax = (s32)(ey + (eey - ey) * t);
			return 1;
		}
	}
	return 1;
}

// ---------------------------------------------------------------------------
// Path data registry (vector-path hit testing)
// ---------------------------------------------------------------------------
#define MAX_CHAR_PATHS_NG 256
static struct {
	size_t char_id;
	size_t path_offset;
	size_t path_size;
	// The path_data array the offset indexes: the DEFINING movie's, because
	// ng_record_char_path runs from that movie's tagInit. NULL = the main
	// movie's generated `path_data`. Same shape as
	// DisplayObject::place_transform_data, and load-bearing for the same
	// reason: a loaded child's offset against the root's array is either the
	// wrong outline or an out-of-bounds read.
	const float (*table)[3];
} ng_char_paths[MAX_CHAR_PATHS_NG];
static size_t ng_char_paths_count = 0;

#define MAX_MORPH_PATHS_NG 64
static struct {
	size_t char_id;
	size_t path_offset;
	size_t path_size;
	// Same field, same reason as ng_char_paths above: a loaded child's morph
	// path offsets are numbered in ITS path_data. Without this a child's
	// hitTest(x, y, true) on a morph shape walked the root's outline.
	const float (*table)[3];
} ng_morph_paths[MAX_MORPH_PATHS_NG];
static size_t ng_morph_paths_count = 0;

#define MAX_WINDING_NG 64
static size_t ng_winding_ids[MAX_WINDING_NG];
static size_t ng_winding_count = 0;

// The path_data table of the movie whose tagInit is running (NULL = main).
static const float (*ng_current_path_table(void))[3]
{
	extern u8 g_current_movie_id;
	if (g_current_movie_id == 0) return NULL;
	for (int i = 0; ; i++) {
		MovieEntry* e = getMovieEntryAt(i);
		if (e == NULL) break;
		if (e->movie_id == g_current_movie_id)
			return (const float (*)[3]) e->path_data_ptr;
	}
	return NULL;
}

void ng_record_char_path(size_t char_id, size_t path_offset, size_t path_size)
{
	const float (*table)[3] = ng_current_path_table();
	// Idempotent per char_id (see ng_record_char_bounds): prevents re-running
	// define tags from overflowing the table and dropping later chars' paths.
	for (size_t i = 0; i < ng_char_paths_count; i++) {
		if (ng_char_paths[i].char_id == char_id) {
			ng_char_paths[i].path_offset = path_offset;
			ng_char_paths[i].path_size = path_size;
			ng_char_paths[i].table = table;
			return;
		}
	}
	if (ng_char_paths_count >= MAX_CHAR_PATHS_NG) return;
	ng_char_paths[ng_char_paths_count].char_id = char_id;
	ng_char_paths[ng_char_paths_count].path_offset = path_offset;
	ng_char_paths[ng_char_paths_count].path_size = path_size;
	ng_char_paths[ng_char_paths_count].table = table;
	ng_char_paths_count++;
}

// The path_data table `ng_find_char_path`'s offset indexes for this character,
// or NULL when that is the main movie's generated array.
const float (*ng_findCharPathTable(size_t char_id))[3]
{
	for (size_t i = 0; i < ng_char_paths_count; i++)
		if (ng_char_paths[i].char_id == char_id)
			return ng_char_paths[i].table;
	return NULL;
}

void ng_record_morph_path(size_t char_id, size_t path_offset, size_t path_size)
{
	const float (*table)[3] = ng_current_path_table();
	// Idempotent per char_id (see ng_record_char_bounds).
	for (size_t i = 0; i < ng_morph_paths_count; i++) {
		if (ng_morph_paths[i].char_id == char_id) {
			ng_morph_paths[i].path_offset = path_offset;
			ng_morph_paths[i].path_size = path_size;
			ng_morph_paths[i].table = table;
			return;
		}
	}
	if (ng_morph_paths_count >= MAX_MORPH_PATHS_NG) return;
	ng_morph_paths[ng_morph_paths_count].char_id = char_id;
	ng_morph_paths[ng_morph_paths_count].path_offset = path_offset;
	ng_morph_paths[ng_morph_paths_count].path_size = path_size;
	ng_morph_paths[ng_morph_paths_count].table = table;
	ng_morph_paths_count++;
}

// The path_data table a MORPH character's offsets index (NULL = the main
// movie's generated array). Separate registry from ng_findCharPathTable
// because a morph character is recorded in ng_morph_paths only.
const float (*ng_findMorphPathTable(size_t char_id))[3]
{
	for (size_t i = 0; i < ng_morph_paths_count; i++)
		if (ng_morph_paths[i].char_id == char_id)
			return ng_morph_paths[i].table;
	return NULL;
}

// A static-text character has no path run of its own, but its GLYPHS' path
// offsets (glyph_data[4g+2]) are numbered in the DEFINING movie's path_data.
// Record the table so the CPU glyph hit tester can resolve them; the entry
// carries no run, so ng_find_char_path keeps answering "no" for it.
void ng_record_text_path_table(size_t char_id)
{
	const float (*table)[3] = ng_current_path_table();
	if (table == NULL) return;         // main movie: the default is already right
	for (size_t i = 0; i < ng_char_paths_count; i++) {
		if (ng_char_paths[i].char_id == char_id) {
			ng_char_paths[i].table = table;
			return;
		}
	}
	if (ng_char_paths_count >= MAX_CHAR_PATHS_NG) return;
	ng_char_paths[ng_char_paths_count].char_id = char_id;
	ng_char_paths[ng_char_paths_count].path_offset = 0;
	ng_char_paths[ng_char_paths_count].path_size = 0;   // no run: see above
	ng_char_paths[ng_char_paths_count].table = table;
	ng_char_paths_count++;
}

void ng_record_char_winding(size_t char_id)
{
	// Idempotent per char_id (see ng_record_char_bounds).
	for (size_t i = 0; i < ng_winding_count; i++)
		if (ng_winding_ids[i] == char_id) return;
	if (ng_winding_count < MAX_WINDING_NG)
		ng_winding_ids[ng_winding_count++] = char_id;
}

// Public lookup functions for hit testing (called from tag_stubs.c)
int ng_find_char_path(size_t char_id, size_t* out_offset, size_t* out_size)
{
	for (size_t i = 0; i < ng_char_paths_count; i++) {
		if (ng_char_paths[i].char_id == char_id) {
			// path_size 0 = a table-only entry recorded by
			// ng_record_text_path_table; it names no path run.
			if (ng_char_paths[i].path_size == 0) return 0;
			*out_offset = ng_char_paths[i].path_offset;
			*out_size = ng_char_paths[i].path_size;
			return 1;
		}
	}
	return 0;
}

int ng_find_morph_path(size_t char_id, size_t* out_offset, size_t* out_size)
{
	for (size_t i = 0; i < ng_morph_paths_count; i++) {
		if (ng_morph_paths[i].char_id == char_id) {
			*out_offset = ng_morph_paths[i].path_offset;
			*out_size = ng_morph_paths[i].path_size;
			return 1;
		}
	}
	return 0;
}

int ng_uses_nonzero_winding(size_t char_id)
{
	for (size_t i = 0; i < ng_winding_count; i++)
		if (ng_winding_ids[i] == char_id) return 1;
	return 0;
}

// ---------------------------------------------------------------------------
// Button char_id registry (for typeof discrimination)
// ---------------------------------------------------------------------------
#define MAX_BUTTONS_NG 64
static size_t ng_button_ids[MAX_BUTTONS_NG];
static size_t ng_button_count = 0;

int ng_find_button(size_t char_id)
{
	for (size_t i = 0; i < ng_button_count; i++)
		if (ng_button_ids[i] == char_id) return 1;
	return 0;
}

void ng_record_button(size_t char_id)
{
	if (ng_button_count < MAX_BUTTONS_NG && !ng_find_button(char_id))
		ng_button_ids[ng_button_count++] = char_id;
}

// ---------------------------------------------------------------------------
// Video stream char_id registry
// ---------------------------------------------------------------------------
#define MAX_VIDEOS_NG 32
static size_t ng_video_ids[MAX_VIDEOS_NG];
static u16    ng_video_widths[MAX_VIDEOS_NG];
static u16    ng_video_heights[MAX_VIDEOS_NG];
static u8     ng_video_codecs[MAX_VIDEOS_NG];
static u8     ng_video_deblocking[MAX_VIDEOS_NG];
static size_t ng_video_count = 0;

int ng_find_video(size_t char_id)
{
	for (size_t i = 0; i < ng_video_count; i++)
		if (ng_video_ids[i] == char_id) return 1;
	return 0;
}

void ng_record_video(SWFAppContext* app_context, u16 char_id, u16 width, u16 height,
                     u8 codec_id, u8 deblocking)
{
	(void)app_context;
	// Idempotent per char_id (see ng_record_char_bounds).
	for (size_t i = 0; i < ng_video_count; i++) {
		if (ng_video_ids[i] == (size_t)char_id) {
			ng_video_widths[i] = width; ng_video_heights[i] = height;
			ng_video_codecs[i] = codec_id;
			ng_video_deblocking[i] = deblocking;
			return;
		}
	}
	if (ng_video_count < MAX_VIDEOS_NG) {
		ng_video_ids[ng_video_count] = (size_t)char_id;
		ng_video_widths[ng_video_count] = width;
		ng_video_heights[ng_video_count] = height;
		ng_video_codecs[ng_video_count] = codec_id;
		ng_video_deblocking[ng_video_count] = deblocking;
		ng_video_count++;
	}
}

int ng_isVideoChar(size_t char_id)
{
	return ng_find_video(char_id);
}

// Look up the declared display dimensions from DefineVideoStream. Returns 1
// on success and fills *out_w / *out_h with pixel dimensions. Returns 0 if
// the char_id isn't a known video or the dimensions weren't recorded (zero).
int ng_getVideoDimensions(size_t char_id, u16* out_w, u16* out_h)
{
	for (size_t i = 0; i < ng_video_count; i++) {
		if (ng_video_ids[i] == char_id) {
			if (ng_video_widths[i] == 0 || ng_video_heights[i] == 0) return 0;
			if (out_w) *out_w = ng_video_widths[i];
			if (out_h) *out_h = ng_video_heights[i];
			return 1;
		}
	}
	return 0;
}

// FLV/SWF CodecID for the given video char_id. Returns 0 if char_id
// isn't a registered video.
u8 ng_getVideoCodec(size_t char_id)
{
	for (size_t i = 0; i < ng_video_count; i++)
		if (ng_video_ids[i] == char_id) return ng_video_codecs[i];
	return 0;
}

// DefineVideoStream `VideoFlagsDeblocking` (UB[3]) for the given video
// char_id — a VIDEO_DEBLOCK_* value. Returns 0 (UseVideoPacketValue, the SWF
// default and what Ruffle uses for FLV) for an unknown char_id.
u8 ng_getVideoDeblocking(size_t char_id)
{
	for (size_t i = 0; i < ng_video_count; i++)
		if (ng_video_ids[i] == char_id) return ng_video_deblocking[i];
	return 0;
}

// ---------------------------------------------------------------------------
// Scriptable character queries
// ---------------------------------------------------------------------------

// Defined in the textfield registry section below. Must be declared before
// use: the trace-WASM emcc build has no -Wno-error=implicit-function-declaration
// (unlike the graphics build), so an implicit declaration fails that build.
int ng_find_textfield(size_t char_id);

int ng_isScriptableChar(size_t char_id)
{
	return (dictionary[char_id].type == CHAR_TYPE_SPRITE) ||
	       ng_find_button(char_id) ||
	       (ng_find_textfield(char_id) >= 0) ||
	       ng_find_video(char_id);
}

int ng_getCharTextfieldIdx(size_t char_id)
{
	return ng_find_textfield(char_id);
}

// ---------------------------------------------------------------------------
// Font info registry
// ---------------------------------------------------------------------------
#define MAX_FONTS_NG 32
#define MAX_FONT_GLYPHS 512
static struct {
	u16 font_id;
	char name[128];
	int bold;
	int italic;
	int has_metrics;
	s16 ascent;
	s16 descent;
	s16 leading;
	int em_square;
	u16 code_table[MAX_FONT_GLYPHS];
	/* u16 on the wire (Ruffle Glyph::advance: u16); s32 keeps 33000 intact
	   and leaves -1 free as the "no such glyph" sentinel. */
	s32 advance_table[MAX_FONT_GLYPHS];
	size_t glyph_count;
	size_t glyph_base;
	int is_builtin;
} ng_fonts[MAX_FONTS_NG];
static size_t ng_font_count = 0;

// Built-in Noto Sans fallback font for createTextField (no embedded font).
static const u16 builtin_noto_sans_codes[] = {
	32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
	48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
	64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,
	80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,
	96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,
	112,113,114,115,116,117,118,119,120,121,122,123,124,125,126
};
static const s32 builtin_noto_sans_advances[] = {
	260,269,408,646,572,831,732,225,300,300,551,572,268,322,268,372,
	572,572,572,572,572,572,572,572,572,572,268,268,572,572,572,434,
	899,639,650,632,730,556,519,728,741,339,273,619,524,907,760,781,
	605,781,622,549,556,731,600,930,586,566,572,329,372,329,572,444,
	281,561,615,480,615,564,344,615,618,258,258,534,258,935,618,605,
	615,615,413,479,361,618,508,786,529,510,470,380,551,380,572
};
#define BUILTIN_NOTO_SANS_GLYPH_COUNT 95
#define BUILTIN_NOTO_SANS_ASCENT 1069
#define BUILTIN_NOTO_SANS_DESCENT 293
#define BUILTIN_NOTO_SANS_LEADING 0
#define BUILTIN_NOTO_SANS_EM 1000

static s32 builtin_font_glyph_advance(u16 code_point)
{
	if (code_point >= 32 && code_point <= 126)
		return builtin_noto_sans_advances[code_point - 32];
	return -1;
}

static int ng_builtin_font_registered = 0;
static void ng_ensure_builtin_font(void)
{
	if (ng_builtin_font_registered) return;
	ng_builtin_font_registered = 1;
	if (ng_font_count >= MAX_FONTS_NG) return;
	size_t idx = ng_font_count++;
	ng_fonts[idx].font_id = 0;
	strncpy(ng_fonts[idx].name, "Noto Sans", sizeof(ng_fonts[idx].name) - 1);
	ng_fonts[idx].bold = 0;
	ng_fonts[idx].italic = 0;
	ng_fonts[idx].is_builtin = 1;
	ng_fonts[idx].has_metrics = 1;
	ng_fonts[idx].ascent = BUILTIN_NOTO_SANS_ASCENT;
	ng_fonts[idx].descent = BUILTIN_NOTO_SANS_DESCENT;
	ng_fonts[idx].leading = BUILTIN_NOTO_SANS_LEADING;
	ng_fonts[idx].em_square = BUILTIN_NOTO_SANS_EM;
	ng_fonts[idx].glyph_count = BUILTIN_NOTO_SANS_GLYPH_COUNT;
	for (size_t j = 0; j < BUILTIN_NOTO_SANS_GLYPH_COUNT; j++) {
		ng_fonts[idx].code_table[j] = builtin_noto_sans_codes[j];
		ng_fonts[idx].advance_table[j] = builtin_noto_sans_advances[j];
	}
}

static int ng_find_font(u16 font_id)
{
	for (size_t i = 0; i < ng_font_count; i++)
		if (ng_fonts[i].font_id == font_id) return (int)i;
	if (font_id == 0) {
		ng_ensure_builtin_font();
		for (size_t i = 0; i < ng_font_count; i++)
			if (ng_fonts[i].font_id == 0) return (int)i;
	}
	return -1;
}

int ng_find_font_with_metrics(u16 font_id)
{
	int fi = ng_find_font(font_id);
	if (fi >= 0 && ng_fonts[fi].has_metrics) return fi;
	ng_ensure_builtin_font();
	for (size_t i = 0; i < ng_font_count; i++)
		if (ng_fonts[i].font_id == 0 && ng_fonts[i].has_metrics) return (int)i;
	return -1;
}

static s32 ng_font_glyph_advance(int font_idx, u16 code_point)
{
	for (size_t j = 0; j < ng_fonts[font_idx].glyph_count; j++) {
		if (ng_fonts[font_idx].code_table[j] == code_point)
			return ng_fonts[font_idx].advance_table[j];
	}
	return -1;
}

int ng_font_find_glyph(int font_idx, u16 code_point)
{
	if (font_idx < 0 || (size_t)font_idx >= ng_font_count) return -1;
	for (size_t j = 0; j < ng_fonts[font_idx].glyph_count; j++) {
		if (ng_fonts[font_idx].code_table[j] == code_point)
			return (int)j;
	}
	return -1;
}

size_t ng_font_get_glyph_base(int font_idx)
{
	if (font_idx < 0 || (size_t)font_idx >= ng_font_count) return 0;
	return ng_fonts[font_idx].glyph_base;
}

// The built-in Noto Sans fallback font carries metrics + advance widths only —
// it has NO glyph outlines in the generated `glyph_data` global. Callers that
// render glyph shapes must skip it (reading glyph_data at its glyph indices is
// out of bounds, e.g. for trace-only SWFs whose glyph_data is a 1-element
// placeholder).
int ng_font_is_builtin(int font_idx)
{
	if (font_idx < 0 || (size_t)font_idx >= ng_font_count) return 0;
	return ng_fonts[font_idx].is_builtin;
}

int ng_font_get_metrics(int font_idx, s16* ascent, s16* descent, int* em_square)
{
	if (font_idx < 0 || (size_t)font_idx >= ng_font_count) return 0;
	if (!ng_fonts[font_idx].has_metrics) return 0;
	if (ascent) *ascent = ng_fonts[font_idx].ascent;
	if (descent) *descent = ng_fonts[font_idx].descent;
	if (em_square) *em_square = ng_fonts[font_idx].em_square;
	return 1;
}

s32 ng_font_glyph_advance_by_idx(int font_idx, int glyph_idx)
{
	if (font_idx < 0 || (size_t)font_idx >= ng_font_count) return -1;
	if (glyph_idx < 0 || (size_t)glyph_idx >= ng_fonts[font_idx].glyph_count) return -1;
	return ng_fonts[font_idx].advance_table[glyph_idx];
}

void ng_record_font(SWFAppContext* app_context, u16 font_id, const char* name, int bold, int italic)
{
	(void)app_context;
	// Idempotent per font_id (see ng_record_char_bounds): update in place if
	// already registered, so re-running DefineFont tags don't append duplicate
	// slots and overflow the table (which would drop later fonts; the metrics/
	// glyph-base setters resolve the first matching font_id anyway).
	size_t slot = ng_font_count;
	for (size_t i = 0; i < ng_font_count; i++) {
		if (ng_fonts[i].font_id == font_id) { slot = i; break; }
	}
	if (slot == ng_font_count) {
		if (ng_font_count >= MAX_FONTS_NG) return;
		ng_font_count++;
	}
	ng_fonts[slot].font_id = font_id;
	strncpy(ng_fonts[slot].name, name ? name : "", sizeof(ng_fonts[slot].name) - 1);
	ng_fonts[slot].name[sizeof(ng_fonts[slot].name) - 1] = '\0';
	ng_fonts[slot].bold = bold;
	ng_fonts[slot].italic = italic;
}

void ng_record_font_metrics(SWFAppContext* app_context, u16 font_id,
    s16 ascent, s16 descent, s16 leading, int em_square,
    const u16* code_table, const s32* advance_table, size_t glyph_count)
{
	(void)app_context;
	for (size_t i = 0; i < ng_font_count; i++) {
		if (ng_fonts[i].font_id == font_id) {
			ng_fonts[i].has_metrics = 1;
			ng_fonts[i].ascent = ascent;
			ng_fonts[i].descent = descent;
			ng_fonts[i].leading = leading;
			ng_fonts[i].em_square = em_square;
			size_t count = glyph_count < MAX_FONT_GLYPHS ? glyph_count : MAX_FONT_GLYPHS;
			ng_fonts[i].glyph_count = count;
			for (size_t j = 0; j < count; j++) {
				ng_fonts[i].code_table[j] = code_table[j];
				ng_fonts[i].advance_table[j] = advance_table[j];
			}
			return;
		}
	}
}

void ng_record_font_glyph_base(u16 font_id, size_t glyph_base)
{
	for (size_t i = 0; i < ng_font_count; i++) {
		if (ng_fonts[i].font_id == font_id) {
			ng_fonts[i].glyph_base = glyph_base;
			return;
		}
	}
}

const char* ng_getFontName(u16 font_id)
{
	for (size_t i = 0; i < ng_font_count; i++)
		if (ng_fonts[i].font_id == font_id) return ng_fonts[i].name;
	return "";
}

const char* ng_getFontNameSWFDefined(u16 font_id)
{
	for (size_t i = 0; i < ng_font_count; i++)
		if (ng_fonts[i].font_id == font_id && !ng_fonts[i].is_builtin) return ng_fonts[i].name;
	return "";
}

u16 ng_findFontIdByName(const char* name)
{
	for (size_t i = 0; i < ng_font_count; i++)
		if (strcasecmp(ng_fonts[i].name, name) == 0) return ng_fonts[i].font_id;
	return 0;
}

// A device-font EditText (UseOutlines/embedFonts = false) renders with a host
// system font, never with the SWF's embedded outlines. Many SWFs embed a font
// (e.g. "Arial") with only the glyph subset used by their static text, then
// point device-font fields at the same font expecting the player to supply the
// rest from a system font. We approximate that by re-pointing such fields at a
// reserved device-font alias ("_sans"/"_serif"/"_typewriter"), which the
// recompiler's Phase-A synthesis has populated with a full ASCII glyph set from
// the bundled NotoSans. Returns the alias font's id (preferring "_sans"), or -1
// when the SWF declares no such synthesized device font.
int ng_find_device_fallback_font(void)
{
	static const char* prefs[3] = { "_sans", "_serif", "_typewriter" };
	for (int pi = 0; pi < 3; pi++) {
		for (size_t i = 0; i < ng_font_count; i++) {
			if (!ng_fonts[i].is_builtin && ng_fonts[i].has_metrics &&
			    ng_fonts[i].glyph_count > 0 &&
			    strcasecmp(ng_fonts[i].name, prefs[pi]) == 0)
				return (int)ng_fonts[i].font_id;
		}
	}
	return -1;
}

int ng_getFontBold(u16 font_id)
{
	for (size_t i = 0; i < ng_font_count; i++)
		if (ng_fonts[i].font_id == font_id) return ng_fonts[i].bold;
	return 0;
}

int ng_getFontItalic(u16 font_id)
{
	for (size_t i = 0; i < ng_font_count; i++)
		if (ng_fonts[i].font_id == font_id) return ng_fonts[i].italic;
	return 0;
}

// ---------------------------------------------------------------------------
// TextField property registry
// ---------------------------------------------------------------------------
#define MAX_TEXTFIELDS_NG 64
static struct {
	size_t char_id;
	char plain_text[1024];
	char raw_html_text[1024];
	u32 text_color;
	u16 font_id;
	u16 font_height;
	s16 max_length;
	u8 align;
	u16 left_margin, right_margin;
	s16 indent;
	s16 leading;
	char variable_name[256];
	u16 flags;
	s32 bounds_xmin, bounds_xmax, bounds_ymin, bounds_ymax;
	char csm_antiAliasType[16];
	char csm_gridFitType[16];
	float csm_thickness;
	float csm_sharpness;
	int csm_applied;
} ng_textfields[MAX_TEXTFIELDS_NG];
static size_t ng_textfield_count = 0;

int ng_find_textfield(size_t char_id)
{
	for (size_t i = 0; i < ng_textfield_count; i++)
		if (ng_textfields[i].char_id == char_id) return (int)i;
	return -1;
}

void ng_record_textfield_props(SWFAppContext* app_context, size_t char_id,
    const char* plain_text, const char* raw_html_text, u32 text_color,
    u16 font_id, u16 font_height, s16 max_length,
    u8 align, u16 left_margin, u16 right_margin, s16 indent, s16 leading,
    const char* variable_name, u16 flags,
    s32 bounds_xmin, s32 bounds_xmax, s32 bounds_ymin, s32 bounds_ymax)
{
	(void)app_context;
	// Idempotent per char_id (see ng_record_char_bounds): reuse the existing
	// slot if this textfield was already recorded, so re-running define tags
	// don't overflow the table and drop later textfields' bounds.
	int _existing = ng_find_textfield(char_id);
	size_t i;
	if (_existing >= 0) {
		i = (size_t)_existing;
	} else {
		if (ng_textfield_count >= MAX_TEXTFIELDS_NG) return;
		i = ng_textfield_count++;
	}
	ng_textfields[i].char_id = char_id;
	strncpy(ng_textfields[i].plain_text, plain_text ? plain_text : "", sizeof(ng_textfields[i].plain_text) - 1);
	ng_textfields[i].plain_text[sizeof(ng_textfields[i].plain_text) - 1] = '\0';
	strncpy(ng_textfields[i].raw_html_text, raw_html_text ? raw_html_text : "", sizeof(ng_textfields[i].raw_html_text) - 1);
	ng_textfields[i].raw_html_text[sizeof(ng_textfields[i].raw_html_text) - 1] = '\0';
	ng_textfields[i].text_color = text_color;
	ng_textfields[i].font_id = font_id;
	ng_textfields[i].font_height = font_height;
	ng_textfields[i].max_length = max_length;
	ng_textfields[i].align = align;
	ng_textfields[i].left_margin = left_margin;
	ng_textfields[i].right_margin = right_margin;
	ng_textfields[i].indent = indent;
	ng_textfields[i].leading = leading;
	strncpy(ng_textfields[i].variable_name, variable_name ? variable_name : "", sizeof(ng_textfields[i].variable_name) - 1);
	ng_textfields[i].variable_name[sizeof(ng_textfields[i].variable_name) - 1] = '\0';
	ng_textfields[i].flags = flags;
	ng_textfields[i].bounds_xmin = bounds_xmin;
	ng_textfields[i].bounds_xmax = bounds_xmax;
	ng_textfields[i].bounds_ymin = bounds_ymin;
	ng_textfields[i].bounds_ymax = bounds_ymax;
	ng_textfields[i].csm_applied = 0;
}

void ng_record_csm(size_t text_id, const char* anti_alias_type, const char* grid_fit_type,
    float thickness, float sharpness)
{
	int idx = ng_find_textfield(text_id);
	if (idx < 0) return;
	strncpy(ng_textfields[idx].csm_antiAliasType, anti_alias_type ? anti_alias_type : "normal",
	        sizeof(ng_textfields[idx].csm_antiAliasType) - 1);
	ng_textfields[idx].csm_antiAliasType[sizeof(ng_textfields[idx].csm_antiAliasType) - 1] = '\0';
	strncpy(ng_textfields[idx].csm_gridFitType, grid_fit_type ? grid_fit_type : "pixel",
	        sizeof(ng_textfields[idx].csm_gridFitType) - 1);
	ng_textfields[idx].csm_gridFitType[sizeof(ng_textfields[idx].csm_gridFitType) - 1] = '\0';
	ng_textfields[idx].csm_thickness = thickness;
	ng_textfields[idx].csm_sharpness = sharpness;
	ng_textfields[idx].csm_applied = 1;
}

// --- TextField getters ---

int ng_getTextFieldCSMApplied(int idx)
{
	if (idx < 0 || (size_t)idx >= ng_textfield_count) return 0;
	return ng_textfields[idx].csm_applied;
}
const char* ng_getTextFieldCSMAntiAliasType(int idx)
{
	if (idx < 0 || (size_t)idx >= ng_textfield_count) return "normal";
	return ng_textfields[idx].csm_antiAliasType;
}
const char* ng_getTextFieldCSMGridFitType(int idx)
{
	if (idx < 0 || (size_t)idx >= ng_textfield_count) return "pixel";
	return ng_textfields[idx].csm_gridFitType;
}
float ng_getTextFieldCSMThickness(int idx)
{
	if (idx < 0 || (size_t)idx >= ng_textfield_count) return 0.0f;
	return ng_textfields[idx].csm_thickness;
}
float ng_getTextFieldCSMSharpness(int idx)
{
	if (idx < 0 || (size_t)idx >= ng_textfield_count) return 0.0f;
	return ng_textfields[idx].csm_sharpness;
}

u16 ng_getTextFieldFlags(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return 0;
	return ng_textfields[tf_idx].flags;
}

u16 ng_getTextFieldFontId(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return 0;
	return ng_textfields[tf_idx].font_id;
}

u16 ng_getTextFieldFontHeight(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return 0;
	return ng_textfields[tf_idx].font_height;
}

s16 ng_getTextFieldMaxLength(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return -1;
	return ng_textfields[tf_idx].max_length;
}

u8 ng_getTextFieldAlign(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return 0;
	return ng_textfields[tf_idx].align;
}

u16 ng_getTextFieldLeftMargin(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return 0;
	return ng_textfields[tf_idx].left_margin;
}

u16 ng_getTextFieldRightMargin(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return 0;
	return ng_textfields[tf_idx].right_margin;
}

s16 ng_getTextFieldIndent(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return 0;
	return ng_textfields[tf_idx].indent;
}

s16 ng_getTextFieldLeading(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return 0;
	return ng_textfields[tf_idx].leading;
}

const char* ng_getTextFieldVariableName(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return "";
	return ng_textfields[tf_idx].variable_name;
}

void ng_getTextFieldBounds(int tf_idx, s32* xmin, s32* xmax, s32* ymin, s32* ymax)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) {
		*xmin = *xmax = *ymin = *ymax = 0;
		return;
	}
	*xmin = ng_textfields[tf_idx].bounds_xmin;
	*xmax = ng_textfields[tf_idx].bounds_xmax;
	*ymin = ng_textfields[tf_idx].bounds_ymin;
	*ymax = ng_textfields[tf_idx].bounds_ymax;
}

const char* ng_getTextFieldRawHtml(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return "";
	return ng_textfields[tf_idx].raw_html_text;
}

const char* ng_getTextFieldInitialTextByIdx(int tf_idx)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return "";
	return ng_textfields[tf_idx].plain_text;
}

u32 ng_getTextFieldColorByIdx(int idx)
{
	if (idx < 0 || (size_t)idx >= ng_textfield_count) return 0;
	return ng_textfields[idx].text_color;
}

int ng_getTextFieldIdxByCharId(size_t char_id)
{
	return ng_find_textfield(char_id);
}

// ---------------------------------------------------------------------------
// Exported symbols registry (DoExportAssets → attachMovie linkage)
// ---------------------------------------------------------------------------
#define MAX_EXPORTED_SYMBOLS 1024
static struct {
	char name[128];
	size_t char_id;
	u8 swf_version;
	u8 movie_id;
} ng_exported_symbols[MAX_EXPORTED_SYMBOLS];
static size_t ng_exported_symbol_count = 0;

u8 g_current_movie_id = 0;

// Register (or update) one export entry, keyed by (name, movie_id). Flash's
// export table is name-keyed within a movie, so re-exporting a name overwrites
// the prior mapping ("last export wins") rather than appending a second entry.
// Deduping is essential because some authoring tools emit a separate
// ExportAssets tag for every USE of a symbol (e.g. N.swf emits ~1600
// ExportAssets, almost all duplicate exports of one particle symbol -> chid
// 135). Without dedup, the fixed table overflows and later, genuinely-distinct
// exports (fpsBox, timeIndicator, ...) are silently dropped, so attachMovie
// can't resolve them. The (name, movie_id) key keeps cross-movie imports
// (tagImportCharacter re-registers a name under the importing movie's id)
// distinct from the source movie's export of the same name.
static void ng_register_export_entry(const char* name, size_t char_id,
                                     u8 swf_version, u8 movie_id)
{
	for (size_t i = 0; i < ng_exported_symbol_count; i++) {
		if (ng_exported_symbols[i].movie_id == movie_id
		    && strcasecmp(ng_exported_symbols[i].name, name) == 0) {
			ng_exported_symbols[i].char_id = char_id;
			ng_exported_symbols[i].swf_version = swf_version;
			return;
		}
	}
	if (ng_exported_symbol_count < MAX_EXPORTED_SYMBOLS) {
		strncpy(ng_exported_symbols[ng_exported_symbol_count].name, name, 127);
		ng_exported_symbols[ng_exported_symbol_count].name[127] = '\0';
		ng_exported_symbols[ng_exported_symbol_count].char_id = char_id;
		ng_exported_symbols[ng_exported_symbol_count].swf_version = swf_version;
		ng_exported_symbols[ng_exported_symbol_count].movie_id = movie_id;
		ng_exported_symbol_count++;
	}
}

size_t ng_lookupExport(const char* name)
{
	size_t result = (size_t)-1;
	for (size_t i = 0; i < ng_exported_symbol_count; i++)
		if (strcasecmp(ng_exported_symbols[i].name, name) == 0)
			result = ng_exported_symbols[i].char_id;
	return result;
}

size_t ng_lookupExportForMovie(const char* name, u8 movie_id)
{
	size_t result = (size_t)-1;
	for (size_t i = 0; i < ng_exported_symbol_count; i++)
		if (ng_exported_symbols[i].movie_id == movie_id
			&& strcasecmp(ng_exported_symbols[i].name, name) == 0)
			result = ng_exported_symbols[i].char_id;
	return result;
}

int ng_lookupExportVersion(const char* name)
{
	int result = 0;
	for (size_t i = 0; i < ng_exported_symbol_count; i++)
		if (strcasecmp(ng_exported_symbols[i].name, name) == 0)
			result = ng_exported_symbols[i].swf_version;
	return result;
}

int ng_lookupExportVersionForMovie(const char* name, u8 movie_id)
{
	int result = 0;
	for (size_t i = 0; i < ng_exported_symbol_count; i++)
		if (ng_exported_symbols[i].movie_id == movie_id
			&& strcasecmp(ng_exported_symbols[i].name, name) == 0)
			result = ng_exported_symbols[i].swf_version;
	return result;
}

const char* ng_lookupExportName(size_t char_id)
{
	for (size_t i = 0; i < ng_exported_symbol_count; i++)
		if (ng_exported_symbols[i].char_id == char_id)
			return ng_exported_symbols[i].name;
	return NULL;
}

int ng_forEachExportName(size_t char_id, int (*callback)(const char* name, void* user_data), void* user_data)
{
	for (size_t i = 0; i < ng_exported_symbol_count; i++)
		if (ng_exported_symbols[i].char_id == char_id)
			if (callback(ng_exported_symbols[i].name, user_data))
				return 1;
	return 0;
}

void tagRegisterExport(SWFAppContext* app_context, const char* name, size_t char_id)
{
	(void)app_context;
	extern int g_swf_version;
	ng_register_export_entry(name, char_id, (u8)g_swf_version, g_current_movie_id);
}

void tagImportCharacter(SWFAppContext* app_context, size_t local_char_id, const char* export_name)
{
	(void)app_context;
	size_t exported_char_id = ng_lookupExport(export_name);
	if (exported_char_id == (size_t)-1 || exported_char_id == 0) return;
	if (local_char_id >= dictionary_capacity || exported_char_id >= dictionary_capacity)
		return;
	dictionary[local_char_id] = dictionary[exported_char_id];
	extern u8* g_char_movie_id;
	extern size_t g_char_movie_id_capacity;
	if (g_char_movie_id != NULL && exported_char_id < g_char_movie_id_capacity
	    && g_char_movie_id[exported_char_id] != 0) {
		if (local_char_id >= g_char_movie_id_capacity) {
			size_t new_cap = local_char_id + 64;
			u8* new_arr = (u8*)calloc(new_cap, 1);
			if (g_char_movie_id) {
				memcpy(new_arr, g_char_movie_id, g_char_movie_id_capacity);
				free(g_char_movie_id);
			}
			g_char_movie_id = new_arr;
			g_char_movie_id_capacity = new_cap;
		}
		g_char_movie_id[local_char_id] = g_char_movie_id[exported_char_id];
	}
	// Register the imported symbol as an export in the importing movie's
	// namespace so attachMovie / ng_lookupExportForMovie can find it.
	// Without this, the symbol is only registered under the source child
	// movie's id, and per-movie attachMovie lookup from the parent fails.
	u8 src_version = 0;
	for (size_t i = 0; i < ng_exported_symbol_count; i++) {
		if (ng_exported_symbols[i].char_id == exported_char_id
		    && strcasecmp(ng_exported_symbols[i].name, export_name) == 0) {
			src_version = ng_exported_symbols[i].swf_version;
			break;
		}
	}
	ng_register_export_entry(export_name, local_char_id, src_version, g_current_movie_id);
}

// ---------------------------------------------------------------------------
// Sound metadata registry
// ---------------------------------------------------------------------------
#define MAX_SOUND_METADATA 128
static struct {
	u16 char_id;
	u8 rate;
	u32 sample_count;
} g_sound_metadata[MAX_SOUND_METADATA];
static size_t g_sound_metadata_count = 0;

int32_t ng_getSoundDuration(u16 char_id)
{
	for (size_t i = 0; i < g_sound_metadata_count; i++) {
		if (g_sound_metadata[i].char_id == char_id) {
			static const u32 rates[] = { 5512, 11025, 22050, 44100 };
			u32 sr = rates[g_sound_metadata[i].rate & 3];
			double dur = (double)g_sound_metadata[i].sample_count * 1000.0 / (double)sr;
			return (int32_t)(dur + 0.5);
		}
	}
	return -1;
}

void ng_registerSoundMetadata(u16 sound_id, u8 rate, u32 sample_count)
{
	if (g_sound_metadata_count < MAX_SOUND_METADATA) {
		g_sound_metadata[g_sound_metadata_count].char_id = sound_id;
		g_sound_metadata[g_sound_metadata_count].rate = rate;
		g_sound_metadata[g_sound_metadata_count].sample_count = sample_count;
		g_sound_metadata_count++;
	}
}

// ---------------------------------------------------------------------------
// Auto instance naming
// ---------------------------------------------------------------------------
static unsigned int ng_auto_instance_counter = 1;

void ng_try_reclaim_auto_instance_name(const char* auto_name)
{
	if (auto_name == NULL) return;
	unsigned int n = 0;
	if (sscanf(auto_name, "instance%u", &n) == 1 && n == ng_auto_instance_counter - 1)
		ng_auto_instance_counter--;
}

unsigned int ng_get_auto_instance_counter(void)
{
	return ng_auto_instance_counter;
}

unsigned int ng_increment_auto_instance_counter(void)
{
	return ng_auto_instance_counter++;
}

// ---------------------------------------------------------------------------
// Child enumeration (for for-in over MovieClip children)
// ---------------------------------------------------------------------------

void ng_enumerateChildren(const char* parent_name,
    void (*callback)(const char* name, u32 name_len, void* user_data), void* user_data)
{
	if (parent_name != NULL && parent_name[0] != '\0')
	{
		extern size_t ng_findDisplayEntryByName(const char* name);
		size_t parent_depth = ng_findDisplayEntryByName(parent_name);
		if (parent_depth == SIZE_MAX) return;

		DisplayObject* parent_obj = &display_list[parent_depth];
		if (parent_obj->sprite_display_list == NULL) return;

		for (size_t d = 1; d <= parent_obj->sprite_max_depth; d++)
		{
			DisplayObject* child = &parent_obj->sprite_display_list[d];
			if (child->char_id == 0) continue;
			if (child->instance_name != NULL && child->instance_name[0] != '\0')
				callback(child->instance_name, (u32)strlen(child->instance_name), user_data);
		}
	}
	else
	{
		for (size_t d = 0; d <= max_depth; d++)
		{
			if (display_list[d].char_id == 0) continue;
			if (display_list[d].instance_name != NULL && display_list[d].instance_name[0] != '\0')
				callback(display_list[d].instance_name, (u32)strlen(display_list[d].instance_name), user_data);
		}
	}
}

// ---------------------------------------------------------------------------
// Text measurement functions
// ---------------------------------------------------------------------------

static u16 ng_decode_utf8_char(const char* text, size_t text_len, size_t* pos)
{
	unsigned char c = (unsigned char)text[*pos];
	u16 cp = c;
	if (c >= 0xC0 && c < 0xE0 && *pos + 1 < text_len) {
		cp = ((c & 0x1F) << 6) | ((unsigned char)text[*pos + 1] & 0x3F);
		*pos += 2;
	} else if (c >= 0xE0 && c < 0xF0 && *pos + 2 < text_len) {
		cp = ((c & 0x0F) << 12) | (((unsigned char)text[*pos + 1] & 0x3F) << 6) | ((unsigned char)text[*pos + 2] & 0x3F);
		*pos += 3;
	} else {
		*pos += 1;
	}
	return cp;
}

static int ng_device_font_mode = 0;
void ng_setDeviceFontMode(int mode) { ng_device_font_mode = mode; }

static int ng_round_to_pixel(int twips) {
	double px = (double)twips / 20.0;
	int rounded_px = (px >= 0) ? (int)(px + 0.5) : -(int)(-px + 0.5);
	return rounded_px * 20;
}

static int ng_round_ls_to_pixel(int ls_twips) {
	if (ls_twips == 0) return 0;
	double px = (double)ls_twips / 20.0;
	double frac = px - (int)px;
	int rounded_px;
	if (frac == 0.5 || frac == -0.5) {
		int trunc = (int)px;
		rounded_px = (trunc % 2 == 0) ? trunc : trunc + (px > 0 ? 1 : -1);
	} else {
		rounded_px = (px >= 0) ? (int)(px + 0.5) : -(int)(-px + 0.5);
	}
	return rounded_px * 20;
}

// Tab stop state
static int g_tab_stops_twips[64];
static int g_tab_stop_count = 0;
static u16 g_tab_font_height = 0;

void ng_setTabStops(const int* stops_twips, int count, u16 font_height)
{
	g_tab_stop_count = count > 64 ? 64 : count;
	for (int i = 0; i < g_tab_stop_count; i++) g_tab_stops_twips[i] = stops_twips[i];
	g_tab_font_height = font_height;
}

void ng_clearTabStops(void) { g_tab_stop_count = 0; g_tab_font_height = 0; }

static int ng_next_tab_stop(int cursor_twips)
{
	if (g_tab_stop_count > 0) {
		for (int i = 0; i < g_tab_stop_count; i++) {
			if (g_tab_stops_twips[i] > cursor_twips)
				return g_tab_stops_twips[i];
		}
		return cursor_twips;
	}
	int modulo = (int)(g_tab_font_height * 2.7f);
	if (modulo <= 0) return cursor_twips;
	return ((cursor_twips / modulo) + 1) * modulo;
}

static int ng_measure_substr_twips(int font_idx, int em, u16 font_height,
    const char* text, size_t start, size_t end, int letter_spacing_twips)
{
	int w = 0;
	int ls_rounded = ng_device_font_mode ? ng_round_ls_to_pixel(letter_spacing_twips) : 0;
	size_t i = start;
	while (i < end) {
		u16 cp = ng_decode_utf8_char(text, end, &i);
		s32 adv = ng_font_glyph_advance(font_idx, cp);
		int glyph_twips = 0;
		if (adv >= 0) {
			int raw = (int)((float)adv * (float)font_height / (float)em);
			if (ng_device_font_mode) {
				int unspaced = ng_round_to_pixel(raw);
				if (ls_rounded != 0) {
					int spaced = unspaced + ls_rounded;
					glyph_twips = (spaced > 0) ? spaced : unspaced;
				} else {
					glyph_twips = unspaced;
				}
			} else {
				glyph_twips = raw + letter_spacing_twips;
				if (glyph_twips <= 0 && letter_spacing_twips < 0)
					glyph_twips = raw;
			}
		} else if (ng_device_font_mode ? (ls_rounded > 0) : (letter_spacing_twips > 0)) {
			glyph_twips = ng_device_font_mode ? ls_rounded : letter_spacing_twips;
		}
		w += glyph_twips;
	}
	return w;
}

static int ng_wrap_count_lines(int font_idx, int em, u16 font_height,
    const char* text, size_t text_len, int field_width_twips, int swf_version,
    int left_margin_twips, int right_margin_twips, int indent_twips,
    int* max_width_out, int* max_width_full_out, int letter_spacing_twips)
{
	if (text_len == 0) {
		*max_width_out = 0;
		if (max_width_full_out) *max_width_full_out = 0;
		return 1;
	}

	int line_count = 1;
	int base_avail = field_width_twips - left_margin_twips - right_margin_twips;
	if (base_avail < 0) base_avail = 0;
	int first_line_offset = indent_twips > 0 ? indent_twips : 0;
	int cur_avail = base_avail - first_line_offset;
	if (cur_avail < 0) cur_avail = 0;
	int cur_line_w = 0;
	int trimmed_line_w = 0;
	int max_line_w = 0;
	int max_line_w_full = 0;
	int is_swf8 = (swf_version >= 8);

	size_t seg_start = 0;

	while (seg_start < text_len) {
		if ((unsigned char)text[seg_start] == '\t') {
			int next_tab = ng_next_tab_stop(cur_line_w);
			if (next_tab > cur_line_w) {
				cur_line_w = next_tab;
				trimmed_line_w = cur_line_w;
				if (trimmed_line_w > max_line_w) max_line_w = trimmed_line_w;
				if (cur_line_w > max_line_w_full) max_line_w_full = cur_line_w;
			}
			seg_start++;
			continue;
		}

		size_t seg_end = text_len;
		size_t i = seg_start;

		if (is_swf8) {
			int in_spaces = 0;
			while (i < text_len) {
				unsigned char c = (unsigned char)text[i];
				if (c == '\t') {
					seg_end = i;
					break;
				} else if (c == ' ') {
					in_spaces = 1;
					i++;
				} else if (in_spaces) {
					seg_end = i;
					break;
				} else if (c == '-') {
					i++;
					seg_end = i;
					break;
				} else {
					if (c >= 0xC0 && c < 0xE0) i += 2;
					else if (c >= 0xE0 && c < 0xF0) i += 3;
					else if (c >= 0xF0) i += 4;
					else i++;
					in_spaces = 0;
				}
			}
		} else {
			while (i < text_len) {
				unsigned char c = (unsigned char)text[i];
				if (c == '\t') {
					seg_end = i;
					break;
				} else if (c == ' ') {
					i++;
					seg_end = i;
					break;
				} else if (c == '-') {
					if (i == seg_start) {
						i++;
						seg_end = i;
					} else {
						seg_end = i;
					}
					break;
				} else {
					if (c >= 0xC0 && c < 0xE0) i += 2;
					else if (c >= 0xE0 && c < 0xF0) i += 3;
					else if (c >= 0xF0) i += 4;
					else i++;
				}
			}
		}

		int seg_w = ng_measure_substr_twips(font_idx, em, font_height, text, seg_start, seg_end, letter_spacing_twips);

		int seg_trimmed_w = seg_w;
		if (is_swf8) {
			size_t trimmed_end = seg_end;
			while (trimmed_end > seg_start && (unsigned char)text[trimmed_end - 1] == ' ')
				trimmed_end--;
			if (trimmed_end < seg_end)
				seg_trimmed_w = ng_measure_substr_twips(font_idx, em, font_height, text, seg_start, trimmed_end, letter_spacing_twips);
		}

		int test_w = cur_line_w + (is_swf8 ? seg_trimmed_w : seg_w);
		if (test_w <= cur_avail) {
			cur_line_w += seg_w;
			if (is_swf8) {
				trimmed_line_w = cur_line_w - (seg_w - seg_trimmed_w);
			} else {
				trimmed_line_w = cur_line_w;
			}
			if (trimmed_line_w > max_line_w) max_line_w = trimmed_line_w;
			if (cur_line_w > max_line_w_full) max_line_w_full = cur_line_w;
			seg_start = seg_end;
		} else if (cur_line_w == 0) {
			size_t char_pos = seg_start;
			int char_w = 0;
			int last_fitting_pos = seg_start;
			int last_fitting_w = 0;
			int _wr_ls = ng_device_font_mode ? ng_round_ls_to_pixel(letter_spacing_twips) : 0;
			while (char_pos < seg_end) {
				size_t next_pos = char_pos;
				u16 cp = ng_decode_utf8_char(text, seg_end, &next_pos);
				s32 adv = ng_font_glyph_advance(font_idx, cp);
				int gw = 0;
				if (adv >= 0) {
					int raw = (int)((float)adv * (float)font_height / (float)em);
					if (ng_device_font_mode) {
						int unsp = ng_round_to_pixel(raw);
						if (_wr_ls != 0) { int sp = unsp + _wr_ls; gw = (sp > 0) ? sp : unsp; }
						else gw = unsp;
					} else {
						gw = raw + letter_spacing_twips;
						if (gw <= 0 && letter_spacing_twips < 0) gw = raw;
					}
				} else if (ng_device_font_mode ? (_wr_ls > 0) : (letter_spacing_twips > 0)) {
					gw = ng_device_font_mode ? _wr_ls : letter_spacing_twips;
				}
				if (char_w + gw > cur_avail && last_fitting_pos > (int)seg_start) {
					break;
				}
				char_w += gw;
				last_fitting_pos = next_pos;
				last_fitting_w = char_w;
				if (is_swf8 && last_fitting_pos > (int)seg_start && char_w > cur_avail) {
					break;
				}
				char_pos = next_pos;
			}
			if (last_fitting_pos == (int)seg_start) {
				size_t tmp = seg_start;
				ng_decode_utf8_char(text, seg_end, &tmp);
				last_fitting_pos = tmp;
				last_fitting_w = ng_measure_substr_twips(font_idx, em, font_height, text, seg_start, last_fitting_pos, letter_spacing_twips);
			}
			if (last_fitting_w > max_line_w) max_line_w = last_fitting_w;
			if (last_fitting_w > max_line_w_full) max_line_w_full = last_fitting_w;
			line_count++;
			cur_line_w = 0;
			trimmed_line_w = 0;
			cur_avail = base_avail;
			seg_start = last_fitting_pos;
		} else {
			if (trimmed_line_w > max_line_w) max_line_w = trimmed_line_w;
			if (cur_line_w > max_line_w_full) max_line_w_full = cur_line_w;
			line_count++;
			cur_line_w = 0;
			trimmed_line_w = 0;
			cur_avail = base_avail;
		}
	}

	if (trimmed_line_w > max_line_w) max_line_w = trimmed_line_w;
	if (cur_line_w > max_line_w_full) max_line_w_full = cur_line_w;

	*max_width_out = max_line_w;
	if (max_width_full_out) *max_width_full_out = max_line_w_full;
	return line_count;
}

int ng_computeTextWidth(u16 font_id, u16 font_height, const char* text, size_t text_len,
    int word_wrap, int field_width_twips, int swf_version,
    int left_margin_twips, int right_margin_twips, int indent_twips, int align,
    int letter_spacing_twips)
{
	int fi = ng_find_font_with_metrics(font_id);
	if (fi < 0 || text == NULL || text_len == 0) return 0;

	int em = ng_fonts[fi].em_square;
	if (em <= 0) em = 1024;

	if (!word_wrap || field_width_twips <= 0) {
		int trim_trailing = (swf_version >= 8 && align != 0);
		int max_width_twips = 0;
		int cur_width_twips = 0;
		int cur_trimmed_twips = 0;
		int _tw_ls_r = ng_device_font_mode ? ng_round_ls_to_pixel(letter_spacing_twips) : 0;
		for (size_t i = 0; i < text_len; ) {
			unsigned char c = (unsigned char)text[i];
			if (c == '\r' || c == '\n') {
				if (trim_trailing && cur_trimmed_twips < cur_width_twips)
					cur_width_twips = cur_trimmed_twips;
				if (cur_width_twips > max_width_twips) max_width_twips = cur_width_twips;
				cur_width_twips = 0;
				cur_trimmed_twips = 0;
				i++;
				continue;
			}
			if (c == '\t') {
				int next_tab = ng_next_tab_stop(cur_width_twips);
				if (next_tab > cur_width_twips)
					cur_width_twips = next_tab;
				cur_trimmed_twips = cur_width_twips;
				i++;
				continue;
			}
			u16 code_point = ng_decode_utf8_char(text, text_len, &i);
			s32 adv = ng_font_glyph_advance(fi, code_point);
			int glyph_twips = 0;
			if (adv >= 0) {
				int raw = (int)((float)adv * (float)font_height / (float)em);
				if (ng_device_font_mode) {
					int unspaced = ng_round_to_pixel(raw);
					if (_tw_ls_r != 0) {
						int spaced = unspaced + _tw_ls_r;
						glyph_twips = (spaced > 0) ? spaced : unspaced;
					} else {
						glyph_twips = unspaced;
					}
				} else {
					glyph_twips = raw + letter_spacing_twips;
					if (glyph_twips <= 0 && letter_spacing_twips < 0)
						glyph_twips = raw;
				}
			} else if (ng_device_font_mode ? (_tw_ls_r > 0) : (letter_spacing_twips > 0)) {
				glyph_twips = ng_device_font_mode ? _tw_ls_r : letter_spacing_twips;
			}
			cur_width_twips += glyph_twips;
			if (trim_trailing) {
				if (code_point != ' ')
					cur_trimmed_twips = cur_width_twips;
			}
		}
		if (trim_trailing && cur_trimmed_twips < cur_width_twips)
			cur_width_twips = cur_trimmed_twips;
		if (cur_width_twips > max_width_twips) max_width_twips = cur_width_twips;
		return max_width_twips;
	}

	int use_full = (swf_version >= 8 && align == 0);
	int max_width_twips = 0;
	size_t line_start = 0;
	for (size_t i = 0; i <= text_len; i++) {
		int is_end = (i == text_len);
		int is_newline = (!is_end && (text[i] == '\r' || text[i] == '\n'));
		if (is_end || is_newline) {
			int line_max_w = 0, line_max_w_full = 0;
			ng_wrap_count_lines(fi, em, font_height, text + line_start, i - line_start,
			    field_width_twips, swf_version,
			    left_margin_twips, right_margin_twips, indent_twips,
			    &line_max_w, &line_max_w_full, letter_spacing_twips);
			int w = use_full ? line_max_w_full : line_max_w;
			if (w > max_width_twips) max_width_twips = w;
			line_start = i + 1;
		}
	}
	return max_width_twips;
}

int ng_computeTextHeight(u16 font_id, u16 font_height, s16 leading_twips, const char* text, size_t text_len,
    int word_wrap, int field_width_twips, int swf_version,
    int left_margin_twips, int right_margin_twips, int indent_twips,
    int letter_spacing_twips)
{
	int fi = ng_find_font_with_metrics(font_id);
	if (fi < 0) return 0;

	int em = ng_fonts[fi].em_square;
	if (em <= 0) em = 1024;

	int ascent_twips = (int)((float)ng_fonts[fi].ascent * (float)font_height / (float)em);
	int descent_twips = (int)((float)ng_fonts[fi].descent * (float)font_height / (float)em);

	if (text == NULL || text_len == 0) return 0;

	int line_count = 1;
	{
		if (!word_wrap || field_width_twips <= 0) {
			for (size_t i = 0; i < text_len; i++) {
				if (text[i] == '\r' || text[i] == '\n') {
					line_count++;
				}
			}
		} else {
			line_count = 0;
			size_t line_start = 0;
			for (size_t i = 0; i <= text_len; i++) {
				int is_end = (i == text_len);
				int is_newline = (!is_end && (text[i] == '\r' || text[i] == '\n'));
				if (is_end || is_newline) {
					int dummy_w = 0;
					line_count += ng_wrap_count_lines(fi, em, font_height,
					    text + line_start, i - line_start,
					    field_width_twips, swf_version,
					    left_margin_twips, right_margin_twips, indent_twips, &dummy_w, NULL,
					    letter_spacing_twips);
					line_start = i + 1;
				}
			}
			if (line_count == 0) line_count = 1;
		}
	}

	int leading_count = line_count > 1 ? line_count - 1 : 1;
	int total_twips = line_count * (ascent_twips + descent_twips) + leading_count * (int)leading_twips;

	return total_twips;
}

int ng_computeTextLineCount(u16 font_id, u16 font_height, const char* text, size_t text_len,
    int word_wrap, int field_width_twips, int swf_version,
    int left_margin_twips, int right_margin_twips, int indent_twips,
    int letter_spacing_twips)
{
	int fi = ng_find_font_with_metrics(font_id);
	if (fi < 0) return 1;

	int em = ng_fonts[fi].em_square;
	if (em <= 0) em = 1024;

	int line_count = 1;
	if (text != NULL && text_len > 0) {
		if (!word_wrap || field_width_twips <= 0) {
			for (size_t i = 0; i < text_len; i++) {
				if (text[i] == '\r' || text[i] == '\n') {
					line_count++;
				}
			}
		} else {
			line_count = 0;
			size_t line_start = 0;
			for (size_t i = 0; i <= text_len; i++) {
				int is_end = (i == text_len);
				int is_newline = (!is_end && (text[i] == '\r' || text[i] == '\n'));
				if (is_end || is_newline) {
					int dummy_w = 0;
					line_count += ng_wrap_count_lines(fi, em, font_height,
					    text + line_start, i - line_start,
					    field_width_twips, swf_version,
					    left_margin_twips, right_margin_twips, indent_twips, &dummy_w, NULL,
					    letter_spacing_twips);
					line_start = i + 1;
				}
			}
			if (line_count == 0) line_count = 1;
		}
	}
	return line_count;
}

int ng_computeVisibleLines(u16 font_id, u16 font_height, s16 leading_twips, float field_height_pixels)
{
	int fi = ng_find_font_with_metrics(font_id);
	if (fi < 0) return 1;

	int em = ng_fonts[fi].em_square;
	if (em <= 0) em = 1024;

	int ascent_twips = (int)((float)ng_fonts[fi].ascent * (float)font_height / (float)em);
	int descent_twips = (int)((float)ng_fonts[fi].descent * (float)font_height / (float)em);
	int line_height_twips = ascent_twips + descent_twips + (int)leading_twips;
	if (line_height_twips <= 0) return 1;

	int avail_twips = (int)(field_height_pixels * 20.0f) - 80;
	if (avail_twips <= 0) return 1;

	int first_line_twips = ascent_twips + descent_twips;
	if (avail_twips < first_line_twips) return 1;

	int remaining = avail_twips - first_line_twips;
	int extra_lines = remaining / line_height_twips;
	return 1 + extra_lines;
}

static int ng_measure_substr_mixed_twips(int font_idx, int em,
    const char* text, size_t start, size_t end, int letter_spacing_twips,
    const u32* run_starts, const u32* run_lengths, const u16* run_font_heights,
    int run_count, u16 base_font_height)
{
	int w = 0;
	int ls_rounded = ng_device_font_mode ? ng_round_ls_to_pixel(letter_spacing_twips) : 0;
	size_t i = start;
	while (i < end) {
		size_t prev_i = i;
		u16 cp = ng_decode_utf8_char(text, end, &i);
		u16 fh = base_font_height;
		for (int r = 0; r < run_count; r++) {
			if (prev_i >= run_starts[r] && prev_i < run_starts[r] + run_lengths[r]) {
				fh = run_font_heights[r]; break;
			}
		}
		s32 adv = ng_font_glyph_advance(font_idx, cp);
		int glyph_twips = 0;
		if (adv >= 0) {
			int raw = (int)((float)adv * (float)fh / (float)em);
			if (ng_device_font_mode) {
				int unspaced = ng_round_to_pixel(raw);
				if (ls_rounded != 0) {
					int spaced = unspaced + ls_rounded;
					glyph_twips = (spaced > 0) ? spaced : unspaced;
				} else {
					glyph_twips = unspaced;
				}
			} else {
				glyph_twips = raw + letter_spacing_twips;
				if (glyph_twips <= 0 && letter_spacing_twips < 0)
					glyph_twips = raw;
			}
		} else if (ng_device_font_mode ? (ls_rounded > 0) : (letter_spacing_twips > 0)) {
			glyph_twips = ng_device_font_mode ? ls_rounded : letter_spacing_twips;
		}
		w += glyph_twips;
	}
	return w;
}

void ng_computeScrollMixedFont(u16 font_id, u16 base_font_height, s16 leading_twips,
    const char* text, size_t text_len,
    int word_wrap, int field_width_twips, int swf_version,
    int left_margin_twips, int right_margin_twips, int indent_twips,
    int letter_spacing_twips,
    float field_height_pixels, int scroll,
    const u32* run_starts, const u32* run_lengths, const u16* run_font_heights, int run_count,
    int is_input,
    int* out_maxscroll, int* out_bottomscroll, int* out_text_height_twips)
{
	int fi = ng_find_font_with_metrics(font_id);
	if (fi < 0) { if (out_maxscroll) *out_maxscroll = 1; if (out_bottomscroll) *out_bottomscroll = 1; if (out_text_height_twips) *out_text_height_twips = 0; return; }

	int em = ng_fonts[fi].em_square;
	if (em <= 0) em = 1024;

	#define MAX_LAYOUT_LINES 256
	int line_offsets[MAX_LAYOUT_LINES];
	int line_extents[MAX_LAYOUT_LINES];
	int total_visual_lines = 0;

	#define GET_FONT_HEIGHT_AT(pos) ({ \
		u16 _fh = base_font_height; \
		for (int _r = 0; _r < run_count; _r++) { \
			if ((pos) >= run_starts[_r] && (pos) < run_starts[_r] + run_lengths[_r]) { \
				_fh = run_font_heights[_r]; break; \
			} \
		} \
		_fh; \
	})

	int cursor_y = 0;
	size_t line_start = 0;
	int last_line_was_empty = 0;

	for (size_t i = 0; i <= text_len; i++) {
		int is_end = (i == text_len);
		int is_newline = (!is_end && (text[i] == '\r' || text[i] == '\n'));
		if (!is_end && !is_newline) continue;

		const char* hline = text + line_start;
		size_t hline_len = i - line_start;

		u16 terminator_fh = 0;
		if (is_newline && run_count > 0) {
			for (int r = 0; r < run_count; r++) {
				u32 rs = run_starts[r], rl = run_lengths[r];
				if ((u32)i >= rs && (u32)i < rs + rl) {
					terminator_fh = run_font_heights[r];
					break;
				}
			}
		}

		if (!word_wrap || field_width_twips <= 0 || hline_len == 0) {
			u16 max_fh = 0;
			if (run_count > 0) {
				for (int r = 0; r < run_count; r++) {
					u32 rs = run_starts[r], rl = run_lengths[r];
					if (rs < (u32)i && rs + rl > (u32)line_start) {
						if (run_font_heights[r] > max_fh) max_fh = run_font_heights[r];
					}
				}
			}
			if (terminator_fh > max_fh) max_fh = terminator_fh;
			if (max_fh == 0) max_fh = base_font_height;
			int asc = (int)((float)ng_fonts[fi].ascent * (float)max_fh / (float)em);
			int desc = (int)((float)ng_fonts[fi].descent * (float)max_fh / (float)em);
			int lh = asc + desc;

			if (total_visual_lines == 0) {
				lh += (int)leading_twips;
			}

			last_line_was_empty = (hline_len == 0 && is_end);
			if (total_visual_lines < MAX_LAYOUT_LINES) {
				line_offsets[total_visual_lines] = cursor_y;
				line_extents[total_visual_lines] = cursor_y + lh;
			}
			total_visual_lines++;
			cursor_y += asc + desc + (int)leading_twips;
		} else {
			int base_avail = field_width_twips - left_margin_twips - right_margin_twips;
			if (base_avail < 0) base_avail = 0;
			int first_line_offset = indent_twips > 0 ? indent_twips : 0;
			int cur_avail = (total_visual_lines == 0) ? (base_avail - first_line_offset) : base_avail;
			int is_swf8 = (swf_version >= 8);

			size_t seg_start = 0;
			int cur_line_w = 0;
			u16 cur_line_max_fh = 0;
			int is_first_visual_line_in_para = 1;
			(void)is_first_visual_line_in_para;

			while (seg_start < hline_len) {
				size_t seg_end = seg_start;
				if (is_swf8) {
					int in_space = (hline[seg_end] == ' ');
					while (seg_end < hline_len) {
						if (hline[seg_end] == '-' && seg_end > seg_start) { seg_end++; break; }
						int cur_space = (hline[seg_end] == ' ');
						if (in_space && !cur_space && seg_end > seg_start) break;
						in_space = cur_space;
						seg_end++;
					}
				} else {
					while (seg_end < hline_len && hline[seg_end] != ' ' && hline[seg_end] != '-') seg_end++;
					if (seg_end < hline_len) {
						if (hline[seg_end] == '-') seg_end++;
						else seg_end++;
					}
				}

				u16 seg_max_fh = 0;
				if (run_count > 0) {
					size_t abs_seg_start = line_start + seg_start;
					size_t abs_seg_end = line_start + seg_end;
					for (int r = 0; r < run_count; r++) {
						u32 rs = run_starts[r], rl = run_lengths[r];
						if (rs < (u32)abs_seg_end && rs + rl > (u32)abs_seg_start) {
							if (run_font_heights[r] > seg_max_fh) seg_max_fh = run_font_heights[r];
						}
					}
				}
				if (seg_max_fh == 0) seg_max_fh = base_font_height;

				int seg_w = ng_measure_substr_mixed_twips(fi, em,
					text, line_start + seg_start, line_start + seg_end, letter_spacing_twips,
					run_starts, run_lengths, run_font_heights, run_count, base_font_height);

				size_t trim_end = seg_end;
				if (is_swf8) { while (trim_end > seg_start && hline[trim_end-1] == ' ') trim_end--; }
				int seg_trimmed_w = (trim_end < seg_end) ?
					ng_measure_substr_mixed_twips(fi, em,
						text, line_start + seg_start, line_start + trim_end, letter_spacing_twips,
						run_starts, run_lengths, run_font_heights, run_count, base_font_height) : seg_w;

				int test_w = cur_line_w + (is_swf8 ? seg_trimmed_w : seg_w);

				if (test_w <= cur_avail) {
					cur_line_w += seg_w;
					if (seg_max_fh > cur_line_max_fh) cur_line_max_fh = seg_max_fh;
					seg_start = seg_end;
				} else if (cur_line_w == 0) {
					if (seg_max_fh > cur_line_max_fh) cur_line_max_fh = seg_max_fh;
					size_t char_pos = seg_start;
					int char_w = 0;
					size_t last_fitting_pos = seg_start;
					while (char_pos < seg_end) {
						size_t next_pos = char_pos;
						u16 cp = ng_decode_utf8_char(hline, seg_end, &next_pos);
						s32 adv = ng_font_glyph_advance(fi, cp);
						int gw = 0;
						if (adv >= 0) {
							u16 cfh = GET_FONT_HEIGHT_AT(line_start + char_pos);
							gw = (int)((float)adv * (float)cfh / (float)em);
						}
						if (char_w + gw > cur_avail && last_fitting_pos > seg_start) break;
						char_w += gw;
						last_fitting_pos = next_pos;
						if (is_swf8 && last_fitting_pos > seg_start && char_w > cur_avail) break;
						char_pos = next_pos;
					}
					if (last_fitting_pos == seg_start) {
						size_t tmp = seg_start;
						ng_decode_utf8_char(hline, seg_end, &tmp);
						last_fitting_pos = tmp;
					}

					u16 fh = cur_line_max_fh > 0 ? cur_line_max_fh : base_font_height;
					int asc = (int)((float)ng_fonts[fi].ascent * (float)fh / (float)em);
					int desc = (int)((float)ng_fonts[fi].descent * (float)fh / (float)em);
					int lh = asc + desc;
					if (total_visual_lines == 0) lh += (int)leading_twips;
					if (total_visual_lines < MAX_LAYOUT_LINES) {
						line_offsets[total_visual_lines] = cursor_y;
						line_extents[total_visual_lines] = cursor_y + lh;
					}
					total_visual_lines++;
					cursor_y += asc + desc + (int)leading_twips;

					cur_line_w = 0;
					cur_line_max_fh = 0;
					cur_avail = base_avail;
					seg_start = last_fitting_pos;
				} else {
					u16 fh = cur_line_max_fh > 0 ? cur_line_max_fh : base_font_height;
					int asc = (int)((float)ng_fonts[fi].ascent * (float)fh / (float)em);
					int desc = (int)((float)ng_fonts[fi].descent * (float)fh / (float)em);
					int lh = asc + desc;
					if (total_visual_lines == 0) lh += (int)leading_twips;
					if (total_visual_lines < MAX_LAYOUT_LINES) {
						line_offsets[total_visual_lines] = cursor_y;
						line_extents[total_visual_lines] = cursor_y + lh;
					}
					total_visual_lines++;
					cursor_y += asc + desc + (int)leading_twips;

					cur_line_w = 0;
					cur_line_max_fh = 0;
					cur_avail = base_avail;
				}
			}

			if (terminator_fh > cur_line_max_fh) cur_line_max_fh = terminator_fh;
			u16 fh = cur_line_max_fh > 0 ? cur_line_max_fh : base_font_height;
			int asc = (int)((float)ng_fonts[fi].ascent * (float)fh / (float)em);
			int desc = (int)((float)ng_fonts[fi].descent * (float)fh / (float)em);
			int lh = asc + desc;
			if (total_visual_lines == 0) lh += (int)leading_twips;
			if (total_visual_lines < MAX_LAYOUT_LINES) {
				line_offsets[total_visual_lines] = cursor_y;
				line_extents[total_visual_lines] = cursor_y + lh;
			}
			total_visual_lines++;
			cursor_y += asc + desc + (int)leading_twips;
		}

		line_start = i + 1;
	}

	if (total_visual_lines == 0) {
		if (out_maxscroll) *out_maxscroll = 1;
		if (out_bottomscroll) *out_bottomscroll = 1;
		if (out_text_height_twips) *out_text_height_twips = 0;
		return;
	}

	int full_last_idx = (total_visual_lines - 1 < MAX_LAYOUT_LINES) ? total_visual_lines - 1 : MAX_LAYOUT_LINES - 1;
	int full_text_height = line_extents[full_last_idx] - line_offsets[0];

	int reported_text_height = full_text_height;
	if (last_line_was_empty && total_visual_lines > 1) {
		int th_last_idx = (total_visual_lines - 2 < MAX_LAYOUT_LINES) ? total_visual_lines - 2 : MAX_LAYOUT_LINES - 1;
		reported_text_height = line_extents[th_last_idx] - line_offsets[0];
	}

	if (out_text_height_twips) *out_text_height_twips = reported_text_height;

	int window_height = (int)(field_height_pixels * 20.0f) - 80;
	if (window_height <= 0) window_height = 1;

	// Ruffle edit_text.rs::maxscroll measures against layout.text_size(),
	// which (html/layout.rs) drops an empty LAST line for non-input fields
	// only — the phantom line a trailing `</p>` leaves. Input fields keep it
	// so the caret can be placed there.
	int target = (is_input ? full_text_height : reported_text_height) - window_height;
	int maxscroll = 1;
	if (target > 0) {
		for (int i = 0; i < total_visual_lines && i < MAX_LAYOUT_LINES; i++) {
			if (line_offsets[i] >= target) {
				maxscroll = i + 1;
				break;
			}
			maxscroll = i + 1;
		}
	}
	if (maxscroll < 1) maxscroll = 1;
	if (out_maxscroll) *out_maxscroll = maxscroll;

	if (out_bottomscroll) {
		int scroll_idx = scroll - 1;
		if (scroll_idx < 0) scroll_idx = 0;
		if (scroll_idx >= total_visual_lines) scroll_idx = total_visual_lines - 1;
		int scroll_offset = (scroll_idx < MAX_LAYOUT_LINES) ? line_offsets[scroll_idx] : 0;
		int bottom_target = window_height + scroll_offset;

		int bottomscroll = total_visual_lines;
		for (int i = 0; i < total_visual_lines && i < MAX_LAYOUT_LINES; i++) {
			if (line_extents[i] > bottom_target) {
				bottomscroll = i;
				if (bottomscroll < 1) bottomscroll = 1;
				break;
			}
		}
		if (bottomscroll < 1) bottomscroll = 1;
		*out_bottomscroll = bottomscroll;
	}
	#undef MAX_LAYOUT_LINES
	#undef GET_FONT_HEIGHT_AT
}

void ng_getTextExtent(u16 font_id, double font_size_px, const char* text, size_t text_len,
    double width_px, double* out_ascent, double* out_descent,
    double* out_width, double* out_height, double* out_tf_height, double* out_tf_width)
{
	int fi = ng_find_font_with_metrics(font_id);
	if (fi < 0) {
		*out_ascent = *out_descent = *out_width = *out_height = *out_tf_height = *out_tf_width = 0;
		return;
	}

	int em = ng_fonts[fi].em_square;
	if (em <= 0) em = 1024;

	double scale = font_size_px / (double)em;
	double ascent_px = ng_fonts[fi].ascent * scale;
	double descent_px = ng_fonts[fi].descent * scale;

	if (out_ascent) *out_ascent = ascent_px;
	if (out_descent) *out_descent = descent_px;

	u16 font_height_twips = (u16)(font_size_px * 20.0);
	double line_height_px = ascent_px + descent_px;

	if (text == NULL || text_len == 0) {
		if (out_width) *out_width = 0;
		if (out_height) *out_height = line_height_px;
		if (out_tf_height) *out_tf_height = line_height_px + 4.0;
		if (out_tf_width) *out_tf_width = 4.0;
		return;
	}

	int has_wrap = (width_px > 0);
	int field_width_twips = has_wrap ? (int)(width_px * 20.0) : 0;

	int max_w_twips = 0;
	int line_count = 1;
	if (!has_wrap) {
		for (size_t i = 0; i < text_len; ) {
			unsigned char c = (unsigned char)text[i];
			if (c == '\r' || c == '\n') { line_count++; i++; continue; }
			i++;
		}
		max_w_twips = ng_measure_substr_twips(fi, em, font_height_twips, text, 0, text_len, 0);
	} else {
		line_count = 0;
		size_t line_start = 0;
		for (size_t i = 0; i <= text_len; i++) {
			int is_end = (i == text_len);
			int is_newline = (!is_end && (text[i] == '\r' || text[i] == '\n'));
			if (is_end || is_newline) {
				int line_max_w = 0;
				line_count += ng_wrap_count_lines(fi, em, font_height_twips,
				    text + line_start, i - line_start,
				    field_width_twips, 8, 0, 0, 0, &line_max_w, NULL, 0);
				if (line_max_w > max_w_twips) max_w_twips = line_max_w;
				line_start = i + 1;
			}
		}
		if (line_count == 0) line_count = 1;
	}

	double text_width_px = (double)max_w_twips / 20.0;
	double text_height_px = line_count * line_height_px;

	if (out_width) *out_width = text_width_px;
	if (out_height) *out_height = text_height_px;
	if (out_tf_height) *out_tf_height = text_height_px + 4.0;
	// When a wrap width is provided, textFieldWidth equals that constraint
	// (Flash AutoSize::Left + word_wrap preserves the assigned width). Without
	// wrap, AutoSize::Left expands to fit content + 4px gutter.
	if (out_tf_width) *out_tf_width = has_wrap ? width_px : (text_width_px + 4.0);
}

int ng_getCharIndexAtPoint(int tf_idx, float local_x_px, float local_y_px,
                           const char* text, size_t text_len, int scroll_lines)
{
	if (tf_idx < 0 || (size_t)tf_idx >= ng_textfield_count) return 0;

	u16 font_id = ng_textfields[tf_idx].font_id;
	u16 font_height = ng_textfields[tf_idx].font_height;
	if (text == NULL) {
		text = ng_textfields[tf_idx].plain_text;
		text_len = strlen(text);
	}
	s16 leading = ng_textfields[tf_idx].leading;
	u16 left_margin = ng_textfields[tf_idx].left_margin;

	int fi = ng_find_font_with_metrics(font_id);
	if (fi < 0 || text_len == 0) return 0;

	int em = ng_fonts[fi].em_square;
	if (em <= 0) em = 1024;

	float gutter_px = 2.0f;
	float left_margin_px = (float)left_margin / 20.0f;

	s16 ascent = 0, descent = 0;
	int em_sq = 0;
	ng_font_get_metrics(fi, &ascent, &descent, &em_sq);
	if (em_sq <= 0) em_sq = em;
	float line_height_twips = (float)(ascent + descent) / (float)em_sq * (float)font_height + (float)leading;
	float line_height_px = line_height_twips / 20.0f;
	if (line_height_px <= 0) line_height_px = (float)font_height / 20.0f;

	float text_y = local_y_px - gutter_px;
	int target_line = 0;
	if (text_y > 0 && line_height_px > 0)
		target_line = (int)(text_y / line_height_px);
	// The view is scrolled: line 0 of the VIEW is line `scroll - 1` of the
	// layout (text/links_in_scrolled_text). 0 for every unscrolled field, so
	// this is a no-op everywhere else.
	if (scroll_lines > 0) target_line += scroll_lines;
	if (target_line < 0) target_line = 0;
	// Ruffle Layout::find_line_index_by_y clamps past-the-end to the LAST line
	// (`Err(max_line)`), so a click below the text places the caret on the final
	// line rather than falling off into "end of text". Visible whenever the
	// field box is much taller than its text — text/text_caret_placement_*.
	{
		int last_line = 0;
		for (size_t i = 0; i < text_len; i++)
			if (text[i] == '\r' || text[i] == '\n') last_line++;
		if (target_line > last_line) target_line = last_line;
	}

	int current_line = 0;
	size_t line_start_byte = 0;
	int line_start_u16 = 0;
	int char_u16_idx = 0;

	for (size_t i = 0; i < text_len && current_line < target_line; ) {
		unsigned char c = (unsigned char)text[i];
		if (c == '\r' || c == '\n') {
			current_line++;
			i++;
			char_u16_idx++;
			if (current_line == target_line) {
				line_start_byte = i;
				line_start_u16 = char_u16_idx;
			}
			continue;
		}
		size_t old_i = i;
		ng_decode_utf8_char(text, text_len, &i);
		(void)old_i;
		char_u16_idx++;
	}

	if (current_line < target_line)
		return char_u16_idx;

	float text_x = local_x_px - gutter_px - left_margin_px;
	float cum_x_px = 0.0f;
	int idx_on_line = 0;

	for (size_t i = line_start_byte; i < text_len; ) {
		unsigned char c = (unsigned char)text[i];
		if (c == '\r' || c == '\n') break;

		size_t old_i = i;
		u16 cp = ng_decode_utf8_char(text, text_len, &i);

		s32 adv = ng_font_glyph_advance(fi, cp);
		float glyph_px = 0;
		if (adv >= 0) {
			int raw_twips = (int)((float)adv * (float)font_height / (float)em);
			if (ng_device_font_mode)
				raw_twips = ng_round_to_pixel(raw_twips);
			glyph_px = (float)raw_twips / 20.0f;
		}

		if (text_x < cum_x_px + glyph_px * 0.5f)
			return line_start_u16 + idx_on_line;

		cum_x_px += glyph_px;
		idx_on_line++;
	}

	int end_idx = line_start_u16 + idx_on_line;
	size_t end_byte = line_start_byte;
	for (int ci = 0; ci < idx_on_line && end_byte < text_len; ci++)
		ng_decode_utf8_char(text, text_len, &end_byte);
	if (end_byte < text_len) {
		unsigned char c = (unsigned char)text[end_byte];
		if ((c == '\r' || c == '\n') && end_byte + 1 >= text_len)
			end_idx++;
	}
	return end_idx;
}

// ===========================================================================
// Combined per-movie render tables (multi-SWF render slice)
// ===========================================================================
// Every geometry and style index the recompiler emits is numbered from 0 in
// the movie that emitted it. The renderer has ONE vertex buffer, ONE colour
// buffer, ONE gradient texture and ONE static-bitmap slot table, all uploaded
// from the ROOT movie's arrays, so a loaded child's shape drew the root's
// triangles with the root's colours -- or, when the root had none, nothing.
//
// This pass concatenates every linked movie's arrays into one set. The root
// goes first and keeps base 0, so a build with no child movies skips the pass
// entirely and every other build leaves the root's indices untouched. A child
// contributes its own rows at its own base, and the STYLE WORD of each of its
// vertices is re-based as it is copied -- that word carries colour / gradient /
// bitmap indices baked at recompile time, which no draw-time argument can
// reach.
//
// What is NOT here, and why:
//   * path_data -- read only by the CPU hit tester, so the movie's own table
//     is recorded per character (ng_record_char_path below) rather than
//     concatenated. That also avoids needing the root's path_data SIZE, which
//     the generated main.c does not put in app_context.
//   * text_data / glyph_data / text_char_codes ARE here, and unlike the arrays
//     above their CONTENTS are re-written too: a text_data row is a glyph
//     index into glyph_data, and a glyph_data row is a vertex offset into
//     shape_data. Same shape as the style word.
//   * morph_end_shape_data / morph_end_color_data are here in GRAPHICS builds
//     only, because they are read only by the renderer and by the AVM2 morph
//     raster; NO_GRAPHICS never combines them (and keeps their bases 0), the
//     same rule color_data / cxform_data already follow.

#define NG_MAX_RENDER_MOVIES 32

typedef struct {
	u32 shape_vert;
	u32 color;
	u32 transform;
	u32 cxform;
	// One index. The shader uses a vertex's style id as BOTH a gradient ramp
	// row and an inverse-matrix slot (`inv_mats[style_id]` for a gradient fill,
	// `inv_mats[style_upper]` for a bitmap one), and the recompiler advances
	// current_uninv for bitmap fills but current_gradient only for gradients.
	// So the two arrays share a per-movie stride of max(uninv, gradient) and
	// one base; the padding rows are never read.
	u32 style;
	u32 bitmap;
	// Static text: rows of text_data / text_char_codes, and GLYPHS (4
	// glyph_data rows each) of glyph_data.
	u32 text;
	u32 glyph;
	// Morph shapes: rows of morph_end_shape_data / morph_end_color_data.
	u32 morph_vert;
	u32 morph_color;
} NgRenderBases;

static NgRenderBases g_ng_render_bases[NG_MAX_RENDER_MOVIES];
static int g_ng_render_tables_built = 0;

// The combined vertex table, or NULL when this build never built one (no child
// movies, or none of them carries geometry). shape_hit_test.c reads it through
// ng_combinedShapeData() and falls back to the generated `shape_data` symbol.
static const u32 (*g_ng_combined_shape_data)[4] = NULL;

const u32 (*ng_combinedShapeData(void))[4] { return g_ng_combined_shape_data; }

// The combined static-text tables, or NULL when none was built. Same
// fall-back contract as ng_combinedShapeData: every reader keeps its old
// expression for a build that has no child geometry.
static const u32 (*g_ng_combined_glyph_data)[1] = NULL;
static const u32*  g_ng_combined_text_data = NULL;
static const u16*  g_ng_combined_text_char_codes = NULL;
static size_t      g_ng_combined_glyph_rows = 0;
const u32 (*ng_combinedGlyphData(void))[1] { return g_ng_combined_glyph_data; }
const u32* ng_combinedTextData(void) { return g_ng_combined_text_data; }
const u16* ng_combinedTextCharCodes(void) { return g_ng_combined_text_char_codes; }
size_t ng_combinedGlyphRows(void) { return g_ng_combined_glyph_rows; }

// The combined morph END tables (graphics builds only; NULL otherwise).
static const float (*g_ng_combined_morph_end_shape)[2] = NULL;
static const float (*g_ng_combined_morph_end_color)[4] = NULL;
const float (*ng_combinedMorphEndShapeData(void))[2] { return g_ng_combined_morph_end_shape; }
const float (*ng_combinedMorphEndColorData(void))[4] { return g_ng_combined_morph_end_color; }

// The combined fill-style tables (graphics builds only; NULL otherwise). The
// AVM1 renderer reaches all three through `app_context`, which the combine
// pass rewrites in place; the AVM2 CPU raster and morph walk read the
// generated symbols DIRECTLY, so they need the same fall-back contract as
// ng_combinedShapeData. gradient_data and uninv_mat_data share one index
// space (a vertex's style word carries one id for both), so they are combined
// to a shared row count and exposed as a pair.
static const float (*g_ng_combined_color_data)[4] = NULL;
static const u8    (*g_ng_combined_gradient_data)[4] = NULL;
static const float*  g_ng_combined_uninv_mat_data = NULL;
const float (*ng_combinedColorData(void))[4] { return g_ng_combined_color_data; }
const u8 (*ng_combinedGradientData(void))[4] { return g_ng_combined_gradient_data; }
const float* ng_combinedUninvMatData(void) { return g_ng_combined_uninv_mat_data; }

// Base of `movie_id`'s rows in each combined table. Movie 0 is the main SWF and
// is always 0.
u32 ng_movieShapeVertBase(u8 movie_id)
{
	if (movie_id >= NG_MAX_RENDER_MOVIES) return 0;
	return g_ng_render_bases[movie_id].shape_vert;
}

u32 ng_movieBitmapBase(u8 movie_id)
{
	if (movie_id >= NG_MAX_RENDER_MOVIES) return 0;
	return g_ng_render_bases[movie_id].bitmap;
}

// Define-time bases, keyed on the DEFINING movie (g_current_movie_id), for the
// character fields tagDefineText / tagDefineMorphShape bake. Placement ids are
// re-based elsewhere and on a different key -- see ng_cache_transform.
u32 ng_movieTextBase(u8 movie_id)
{
	if (movie_id >= NG_MAX_RENDER_MOVIES) return 0;
	return g_ng_render_bases[movie_id].text;
}

u32 ng_movieGlyphBase(u8 movie_id)
{
	if (movie_id >= NG_MAX_RENDER_MOVIES) return 0;
	return g_ng_render_bases[movie_id].glyph;
}

u32 ng_movieMorphEndVertBase(u8 movie_id)
{
	if (movie_id >= NG_MAX_RENDER_MOVIES) return 0;
	return g_ng_render_bases[movie_id].morph_vert;
}

u32 ng_movieMorphEndColorBase(u8 movie_id)
{
	if (movie_id >= NG_MAX_RENDER_MOVIES) return 0;
	return g_ng_render_bases[movie_id].morph_color;
}

u32 ng_movieColorBase(u8 movie_id)
{
	if (movie_id >= NG_MAX_RENDER_MOVIES) return 0;
	return g_ng_render_bases[movie_id].color;
}

// A DefineText character's glyph transform slots and its colour transform are
// DEFINE-time ids into the placing-independent transform / cxform tables, so
// they take the defining movie's base rather than a placement's.
u32 ng_movieTransformBase(u8 movie_id)
{
	if (movie_id >= NG_MAX_RENDER_MOVIES) return 0;
	return g_ng_render_bases[movie_id].transform;
}

u32 ng_movieCxformBase(u8 movie_id)
{
	if (movie_id >= NG_MAX_RENDER_MOVIES) return 0;
	return g_ng_render_bases[movie_id].cxform;
}

// Base for the movie whose ORIGINAL transform_data array is `td`. That pointer
// is the one signal in the runtime that already tracks the PLACING movie
// correctly through every path -- g_active_transform_data is swapped for a
// loaded movie's tags AND for a child-defined sprite's frame funcs -- so
// ng_cache_transform keys on it rather than on g_current_movie_id.
u32 ng_movieTransformBaseForTable(const float (*td)[16])
{
	if (td == NULL) return 0;
	for (int i = 0; ; i++) {
		MovieEntry* e = getMovieEntryAt(i);
		if (e == NULL) break;
		if ((const float*) e->transform_data_ptr == (const float*) td)
			return e->transform_base;
	}
	return 0;
}

u32 ng_movieCxformBaseForTable(const float (*td)[16])
{
	if (td == NULL) return 0;
	for (int i = 0; ; i++) {
		MovieEntry* e = getMovieEntryAt(i);
		if (e == NULL) break;
		if ((const float*) e->transform_data_ptr == (const float*) td)
			return e->cxform_base;
	}
	return 0;
}

// The combined transform table, or NULL when this build never built one.
static const float (*g_ng_combined_transform_data)[16] = NULL;
static size_t g_ng_combined_transform_rows = 0;
const float (*ng_combinedTransformData(void))[16] { return g_ng_combined_transform_data; }
size_t ng_combinedTransformRows(void) { return g_ng_combined_transform_rows; }

// The combined colour-transform table (flat, 20 floats per slot), or NULL when
// none was built. Only ng_init_cxform_from_data needs this — every other reader
// already holds app_context->cxform_data, which IS this array.
static const float* g_ng_combined_cxform_data = NULL;
const float* ng_combinedCxformData(void) { return g_ng_combined_cxform_data; }

// 1 once ng_buildMovieRenderTables has actually combined something. The hot
// placement paths (ng_cache_transform, ng_rebaseCxformId) test this first so a
// single-movie build never walks the movie registry.
static int g_ng_render_tables_active = 0;
int ng_movieRenderTablesActive(void) { return g_ng_render_tables_active; }

// Re-base one vertex's style word from movie-local indices to combined ones.
// The layout is the recompiler's (SWFRecomp/src/swf.cpp parseFillStyles) and
// the shader's (render_webgpu.c vertex_wgsl), which agree: style.x's low byte
// is the SWF fill type, style.y's low 16 bits are the fill's own index and its
// high 16 bits are the inverse-matrix slot for a bitmap fill.
static u32 ng_rebase_style_word(u32 style_type, u32 style_index,
                                const NgRenderBases* b)
{
	u32 ftype = style_type & 0xFFu;
	u32 lo = style_index & 0xFFFFu;
	u32 hi = (style_index >> 16) & 0xFFFFu;
	if (ftype == 0x00u) {
		// Solid: low half is a color_data index, high half a morph END colour
		// index into morph_end_color_data (SWFRecomp/src/swf.cpp, "Morph solid
		// fills carry the END colour index in the high 16 bits"). The high half
		// is written for EVERY solid fill and read only through the morph
		// raster, so re-basing it unconditionally is correct where it is read
		// and unobservable where it is not.
		lo = (lo + b->color) & 0xFFFFu;
		hi = (hi + b->morph_color) & 0xFFFFu;
	} else if ((ftype & 0xF0u) == 0x10u) {
		// Gradient: one index into both the ramp texture and inv_mats.
		lo = (lo + b->style) & 0xFFFFu;
	} else if ((ftype & 0xF0u) == 0x40u) {
		// Bitmap: low = static bitmap slot, high = inv_mats slot.
		lo = (lo + b->bitmap) & 0xFFFFu;
		hi = (hi + b->style) & 0xFFFFu;
	} else {
		return style_index;
	}
	return (hi << 16) | lo;
}

void ng_buildMovieRenderTables(SWFAppContext* app_context)
{
	if (g_ng_render_tables_built) return;
	g_ng_render_tables_built = 1;
	if (app_context == NULL) return;

	MovieEntry* movies[NG_MAX_RENDER_MOVIES];
	int movie_count = 0;
	int have_tables = 0;
	for (int i = 0; i < NG_MAX_RENDER_MOVIES; i++) {
		MovieEntry* e = getMovieEntryAt(i);
		if (e == NULL) break;
		if (e->movie_id == 0 || e->movie_id >= NG_MAX_RENDER_MOVIES) continue;
		movies[movie_count++] = e;
		if (e->shape_vert_count > 0 || e->color_count > 0 ||
		    e->uninv_mat_count > 0 || e->gradient_count > 0 ||
		    e->bitmap_count > 0 || e->transform_count > 0 ||
		    e->cxform_count > 0 || e->text_count > 0 ||
		    e->glyph_count > 0 || e->morph_end_vert_count > 0 ||
		    e->morph_end_color_count > 0)
			have_tables = 1;
	}
	// No child movies, or a generator that predates these fields: leave every
	// array exactly as the root emitted it. This is the path every
	// single-movie build takes, so the pass cannot move a byte there.
	if (!have_tables) return;
	g_ng_render_tables_active = 1;

	// --- root counts -------------------------------------------------------
	size_t root_verts = app_context->shape_data_size / (4 * sizeof(u32));
	size_t root_xforms = app_context->transform_data_size / (16 * sizeof(float));
	// Static text is combined in EVERY mode: the CPU glyph hit tester and
	// TextSnapshot read these tables in NO_GRAPHICS too.
	size_t root_text = app_context->text_data_size / sizeof(u32);
	size_t root_glyphs = app_context->glyph_data_size / (4 * sizeof(u32));
	// Fill styles and morph ENDs are combined in EVERY mode too: the AVM2 CPU
	// raster (BitmapData.draw, the CPU frame dump) and the AVM2 exact hit test
	// read them under NO_GRAPHICS, with indices that resolve_shape_geom has
	// re-based onto the combined numbering.
	size_t root_colors = app_context->color_data_size / (4 * sizeof(float));
	size_t root_uninv = app_context->uninv_mat_data_size / (16 * sizeof(float));
	size_t root_grads = app_context->gradient_data_size / (256 * 4);
	size_t root_style = root_uninv > root_grads ? root_uninv : root_grads;
	size_t root_mverts = app_context->morph_end_shape_data_size / (2 * sizeof(float));
	size_t root_mcolors = app_context->morph_end_color_data_size / (4 * sizeof(float));
#ifndef NO_GRAPHICS
	// Graphics only: NO_GRAPHICS has no cxform_data / bitmap_data on
	// app_context, so neither is combined and neither base ever moves.
	size_t root_cxforms = app_context->cxform_data_size / (20 * sizeof(float));
	size_t root_bitmaps = app_context->bitmap_count;
#endif

	size_t tot_verts = root_verts;
	size_t tot_colors = root_colors;
	size_t tot_style = root_style;
	size_t tot_xforms = root_xforms;
#ifndef NO_GRAPHICS
	size_t tot_bitmaps = root_bitmaps;
	size_t tot_cxforms = root_cxforms;
#endif
	size_t tot_text = root_text;
	size_t tot_glyphs = root_glyphs;
	size_t tot_mverts = root_mverts;
	size_t tot_mcolors = root_mcolors;
	for (int i = 0; i < movie_count; i++) {
		MovieEntry* e = movies[i];
		NgRenderBases* b = &g_ng_render_bases[e->movie_id];
		size_t style_n = e->uninv_mat_count > e->gradient_count
			? e->uninv_mat_count : e->gradient_count;
		b->shape_vert = (u32) tot_verts;
		b->transform  = (u32) tot_xforms;
		b->text       = (u32) tot_text;
		b->glyph      = (u32) tot_glyphs;
		e->shape_vert_base = b->shape_vert;
		e->transform_base  = b->transform;
		e->text_base       = b->text;
		e->glyph_base      = b->glyph;
		tot_verts   += e->shape_vert_count;
		tot_xforms  += e->transform_count;
		tot_text    += e->text_count;
		tot_glyphs  += e->glyph_count;
		b->color      = (u32) tot_colors;
		b->style      = (u32) tot_style;
		b->morph_vert  = (u32) tot_mverts;
		b->morph_color = (u32) tot_mcolors;
		e->color_base      = b->color;
		e->gradient_base   = b->style;
		e->uninv_mat_base  = b->style;
		e->morph_end_vert_base  = b->morph_vert;
		e->morph_end_color_base = b->morph_color;
		tot_colors  += e->color_count;
		tot_style   += style_n;
		tot_mverts  += e->morph_end_vert_count;
		tot_mcolors += e->morph_end_color_count;
#ifndef NO_GRAPHICS
		b->bitmap     = (u32) tot_bitmaps;
		b->cxform     = (u32) tot_cxforms;
		e->bitmap_base     = b->bitmap;
		e->cxform_base     = b->cxform;
		tot_bitmaps += e->bitmap_count;
		tot_cxforms += e->cxform_count;
#else
		// NO_GRAPHICS has no bitmap/cxform arrays on app_context at all, so
		// neither is combined and both bases must stay 0 — a nonzero cxform
		// base with an un-combined array is an out-of-bounds read, and it
		// would only appear with a SECOND child movie (the first one's base is
		// 0 either way).
		b->bitmap = b->cxform = 0;
		e->bitmap_base = e->cxform_base = 0;
#endif
	}

	// --- vertices ----------------------------------------------------------
	// Always rebuilt when there is any child geometry, because the CPU hit
	// tester and the GPU upload must agree on one numbering.
	if (tot_verts > root_verts) {
		u32 (*sd)[4] = (u32 (*)[4]) calloc(tot_verts, 4 * sizeof(u32));
		if (sd != NULL) {
			if (app_context->shape_data != NULL && root_verts > 0)
				memcpy(sd, app_context->shape_data, root_verts * 4 * sizeof(u32));
			for (int i = 0; i < movie_count; i++) {
				MovieEntry* e = movies[i];
				if (e->shape_data_ptr == NULL || e->shape_vert_count == 0) continue;
				const NgRenderBases* b = &g_ng_render_bases[e->movie_id];
				const u32* src = e->shape_data_ptr;
				u32 (*dst)[4] = sd + e->shape_vert_base;
				for (size_t v = 0; v < e->shape_vert_count; v++) {
					dst[v][0] = src[v * 4 + 0];
					dst[v][1] = src[v * 4 + 1];
					dst[v][2] = src[v * 4 + 2];
					dst[v][3] = ng_rebase_style_word(src[v * 4 + 2],
					                                 src[v * 4 + 3], b);
				}
			}
			g_ng_combined_shape_data = (const u32 (*)[4]) sd;
			app_context->shape_data = (char*) sd;
			app_context->shape_data_size = tot_verts * 4 * sizeof(u32);
		}
	}

	// --- placement matrices -------------------------------------------------
	// Combined in ALL build modes: ng_cache_transform re-bases every
	// obj->transform_id onto this table, and the AVM1 _x/_y getters read it
	// through ng_entryTransformData in NO_GRAPHICS too.
	if (tot_xforms > root_xforms) {
		float (*td)[16] = (float (*)[16]) calloc(tot_xforms, 16 * sizeof(float));
		if (td != NULL) {
			if (app_context->transform_data != NULL && root_xforms > 0)
				memcpy(td, app_context->transform_data,
				       root_xforms * 16 * sizeof(float));
			for (int i = 0; i < movie_count; i++) {
				MovieEntry* e = movies[i];
				if (e->transform_data_ptr == NULL || e->transform_count == 0) continue;
				memcpy(td + e->transform_base, e->transform_data_ptr,
				       e->transform_count * 16 * sizeof(float));
			}
			g_ng_combined_transform_data = (const float (*)[16]) td;
			g_ng_combined_transform_rows = tot_xforms;
			app_context->transform_data = (char*) td;
			app_context->transform_data_size = tot_xforms * 16 * sizeof(float);
		}
	}

	// --- static text: glyph outlines, then the per-character glyph runs ------
	// Combined in ALL build modes (see root_text/root_glyphs above). Two of the
	// three indices a DefineText uses live IN these arrays rather than on the
	// character, so this pass re-writes contents as well as concatenating:
	//   glyph_data[4g+0] is a VERTEX offset into shape_data,
	//   text_data[i]     is a GLYPH index into glyph_data.
	// glyph_data[4g+2] is a path_data offset and is deliberately NOT re-based:
	// path_data is per-movie (see the header comment), and the reader resolves
	// it through the defining movie's table, which tagDefineText records.
	if (tot_glyphs > root_glyphs) {
		u32 (*gd)[1] = (u32 (*)[1]) calloc(tot_glyphs * 4, sizeof(u32));
		if (gd != NULL) {
			if (app_context->glyph_data != NULL && root_glyphs > 0)
				memcpy(gd, app_context->glyph_data, root_glyphs * 4 * sizeof(u32));
			for (int i = 0; i < movie_count; i++) {
				MovieEntry* e = movies[i];
				if (e->glyph_data_ptr == NULL || e->glyph_count == 0) continue;
				const u32* src = e->glyph_data_ptr;
				u32 (*dst)[1] = gd + (size_t) e->glyph_base * 4;
				u32 vbase = e->shape_vert_base;
				for (size_t g = 0; g < e->glyph_count; g++) {
					dst[g * 4 + 0][0] = src[g * 4 + 0] + vbase;  // vertex offset
					dst[g * 4 + 1][0] = src[g * 4 + 1];          // vertex count
					dst[g * 4 + 2][0] = src[g * 4 + 2];          // path offset
					dst[g * 4 + 3][0] = src[g * 4 + 3];          // path count
				}
			}
			g_ng_combined_glyph_data = (const u32 (*)[1]) gd;
			g_ng_combined_glyph_rows = tot_glyphs * 4;
			app_context->glyph_data = (u32*) gd;
			app_context->glyph_data_size = tot_glyphs * 4 * sizeof(u32);
		}
	}

	if (tot_text > root_text) {
		u32* txt = (u32*) calloc(tot_text, sizeof(u32));
		if (txt != NULL) {
			if (app_context->text_data != NULL && root_text > 0)
				memcpy(txt, app_context->text_data, root_text * sizeof(u32));
			for (int i = 0; i < movie_count; i++) {
				MovieEntry* e = movies[i];
				if (e->text_data_ptr == NULL || e->text_count == 0) continue;
				for (size_t k = 0; k < e->text_count; k++)
					txt[e->text_base + k] = e->text_data_ptr[k] + e->glyph_base;
			}
			g_ng_combined_text_data = txt;
			app_context->text_data = txt;
			app_context->text_data_size = tot_text * sizeof(u32);
		}
		// text_char_codes is a parallel array of Unicode code points (no index
		// re-basing) that older recompiler output may not emit at all, so a
		// movie without one contributes zeroes rather than dropping the table.
		u16* codes = (u16*) calloc(tot_text, sizeof(u16));
		if (codes != NULL) {
			if (app_context->text_char_codes != NULL && root_text > 0)
				memcpy(codes, app_context->text_char_codes, root_text * sizeof(u16));
			for (int i = 0; i < movie_count; i++) {
				MovieEntry* e = movies[i];
				if (e->text_char_codes_ptr == NULL || e->text_count == 0) continue;
				memcpy(codes + e->text_base, e->text_char_codes_ptr,
				       e->text_count * sizeof(u16));
			}
			g_ng_combined_text_char_codes = codes;
			app_context->text_char_codes = codes;
			app_context->text_char_codes_size = tot_text * sizeof(u16);
		}
	}

#ifndef NO_GRAPHICS
	// --- placement colour transforms ---------------------------------------
	if (tot_cxforms > root_cxforms) {
		float* cx = (float*) calloc(tot_cxforms * 20, sizeof(float));
		if (cx != NULL) {
			if (app_context->cxform_data != NULL && root_cxforms > 0)
				memcpy(cx, app_context->cxform_data,
				       root_cxforms * 20 * sizeof(float));
			for (int i = 0; i < movie_count; i++) {
				MovieEntry* e = movies[i];
				if (e->cxform_data_ptr == NULL || e->cxform_count == 0) continue;
				memcpy(cx + (size_t) e->cxform_base * 20, e->cxform_data_ptr,
				       e->cxform_count * 20 * sizeof(float));
			}
			g_ng_combined_cxform_data = cx;
			app_context->cxform_data = (char*) cx;
			app_context->cxform_data_size = tot_cxforms * 20 * sizeof(float);
		}
	}

#endif

	// --- solid fill colours ------------------------------------------------
	if (tot_colors > root_colors) {
		float (*cd)[4] = (float (*)[4]) calloc(tot_colors, 4 * sizeof(float));
		if (cd != NULL) {
			if (app_context->color_data != NULL && root_colors > 0)
				memcpy(cd, app_context->color_data, root_colors * 4 * sizeof(float));
			for (int i = 0; i < movie_count; i++) {
				MovieEntry* e = movies[i];
				if (e->color_data_ptr == NULL || e->color_count == 0) continue;
				memcpy(cd + e->color_base, e->color_data_ptr,
				       e->color_count * 4 * sizeof(float));
			}
			g_ng_combined_color_data = (const float (*)[4]) cd;
			app_context->color_data = (char*) cd;
			app_context->color_data_size = tot_colors * 4 * sizeof(float);
		}
	}

	// --- gradient ramps + inverse fill matrices (shared index space) --------
	if (tot_style > root_style) {
		float* um = (float*) calloc(tot_style * 16, sizeof(float));
		if (um != NULL) {
			if (app_context->uninv_mat_data != NULL && root_uninv > 0)
				memcpy(um, app_context->uninv_mat_data,
				       root_uninv * 16 * sizeof(float));
			for (int i = 0; i < movie_count; i++) {
				MovieEntry* e = movies[i];
				if (e->uninv_mat_data_ptr == NULL || e->uninv_mat_count == 0) continue;
				memcpy(um + (size_t) e->uninv_mat_base * 16, e->uninv_mat_data_ptr,
				       e->uninv_mat_count * 16 * sizeof(float));
			}
			g_ng_combined_uninv_mat_data = um;
			app_context->uninv_mat_data = (char*) um;
			app_context->uninv_mat_data_size = tot_style * 16 * sizeof(float);
		}

		u8 (*gd)[4] = (u8 (*)[4]) calloc(tot_style * 256, 4);
		if (gd != NULL) {
			if (app_context->gradient_data != NULL && root_grads > 0)
				memcpy(gd, app_context->gradient_data, root_grads * 256 * 4);
			for (int i = 0; i < movie_count; i++) {
				MovieEntry* e = movies[i];
				if (e->gradient_data_ptr == NULL || e->gradient_count == 0) continue;
				memcpy(gd + (size_t) e->gradient_base * 256, e->gradient_data_ptr,
				       e->gradient_count * 256 * 4);
			}
			g_ng_combined_gradient_data = (const u8 (*)[4]) gd;
			app_context->gradient_data = (char*) gd;
			app_context->gradient_data_size = tot_style * 256 * 4;
		}
	}

	// --- morph shape END vertices / END colours ------------------------------
	// All modes: tagShowFrame/tagRerenderFrame's ratio lerp is graphics-only,
	// but the AVM2 morph raster and the AVM2 exact hit test read these under
	// NO_GRAPHICS too. The three character-side offsets
	// (morph_end_offset, morph_color_start, morph_end_color_start) are re-based
	// at define time in tagDefineMorphShape, and the per-vertex morph END
	// colour index in the solid style word is re-based by ng_rebase_style_word.
	if (tot_mverts > root_mverts) {
		float (*me)[2] = (float (*)[2]) calloc(tot_mverts, 2 * sizeof(float));
		if (me != NULL) {
			if (app_context->morph_end_shape_data != NULL && root_mverts > 0)
				memcpy(me, app_context->morph_end_shape_data,
				       root_mverts * 2 * sizeof(float));
			for (int i = 0; i < movie_count; i++) {
				MovieEntry* e = movies[i];
				if (e->morph_end_shape_data_ptr == NULL || e->morph_end_vert_count == 0)
					continue;
				memcpy(me + e->morph_end_vert_base, e->morph_end_shape_data_ptr,
				       e->morph_end_vert_count * 2 * sizeof(float));
			}
			g_ng_combined_morph_end_shape = (const float (*)[2]) me;
			app_context->morph_end_shape_data = (char*) me;
			app_context->morph_end_shape_data_size = tot_mverts * 2 * sizeof(float);
		}
	}

	if (tot_mcolors > root_mcolors) {
		float (*mc)[4] = (float (*)[4]) calloc(tot_mcolors, 4 * sizeof(float));
		if (mc != NULL) {
			if (app_context->morph_end_color_data != NULL && root_mcolors > 0)
				memcpy(mc, app_context->morph_end_color_data,
				       root_mcolors * 4 * sizeof(float));
			for (int i = 0; i < movie_count; i++) {
				MovieEntry* e = movies[i];
				if (e->morph_end_color_data_ptr == NULL || e->morph_end_color_count == 0)
					continue;
				memcpy(mc + e->morph_end_color_base, e->morph_end_color_data_ptr,
				       e->morph_end_color_count * 4 * sizeof(float));
			}
			g_ng_combined_morph_end_color = (const float (*)[4]) mc;
			app_context->morph_end_color_data = (char*) mc;
			app_context->morph_end_color_data_size = tot_mcolors * 4 * sizeof(float);
		}
	}

#ifndef NO_GRAPHICS
	// --- static bitmap slots ------------------------------------------------
	// Only the COUNT moves here: the pixels stay in each movie's own array and
	// are pre-declared into the renderer's slot table by
	// ng_predeclareChildBitmaps(), called once the renderer exists.
	app_context->bitmap_count = tot_bitmaps;
#endif
}

