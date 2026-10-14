// Guest main for graphics mode: exports frame functions, data arrays, and bridge_init.
// Compiled in-browser with Wasmer clang, linked with wasm-ld --allow-undefined.
// Runtime functions (tag*, action*, etc.) become WASM imports resolved by the host.

#include <recomp.h>
#include <out.h>
#include "constants.h"
#include "draws.h"
#include "bridge_globals.h"

// Layout check for runtime structs that embed jmp_buf (see guest_setjmp_shim.h).
#include <setjmp.h>
__attribute__((export_name("get_guest_jmp_buf_size")))
int get_guest_jmp_buf_size(void) { return (int)sizeof(jmp_buf); }

// Export the frame function array info for JS to read
__attribute__((export_name("get_frame_count")))
int get_frame_count(void) { return FRAME_COUNT; }

__attribute__((export_name("get_fps")))
int get_fps(void) {
#ifdef FRAME_RATE
    return FRAME_RATE;
#else
    return 12;
#endif
}

__attribute__((export_name("get_swf_version")))
int get_swf_version(void) { return SWF_VERSION; }

__attribute__((export_name("get_frame_width")))
int get_frame_width(void) {
#ifdef FRAME_WIDTH
    return FRAME_WIDTH;
#else
    return 550;
#endif
}

__attribute__((export_name("get_frame_height")))
int get_frame_height(void) {
#ifdef FRAME_HEIGHT
    return FRAME_HEIGHT;
#else
    return 400;
#endif
}

// Export data array addresses and sizes for JS to pass to the host
__attribute__((export_name("get_shape_data")))
void* get_shape_data(void) { return (void*)shape_data; }
__attribute__((export_name("get_shape_data_size")))
int get_shape_data_size(void) { return sizeof(shape_data); }

__attribute__((export_name("get_transform_data")))
void* get_transform_data(void) { return (void*)transform_data; }
__attribute__((export_name("get_transform_data_size")))
int get_transform_data_size(void) { return sizeof(transform_data); }

__attribute__((export_name("get_color_data")))
void* get_color_data(void) { return (void*)color_data; }
__attribute__((export_name("get_color_data_size")))
int get_color_data_size(void) { return sizeof(color_data); }

__attribute__((export_name("get_stage_to_ndc")))
void* get_stage_to_ndc(void) { return (void*)stage_to_ndc; }

__attribute__((export_name("get_bitmap_count")))
int get_bitmap_count(void) { return BITMAP_COUNT; }
__attribute__((export_name("get_bitmap_highest_w")))
int get_bitmap_highest_w(void) { return BITMAP_HIGHEST_W; }
__attribute__((export_name("get_bitmap_highest_h")))
int get_bitmap_highest_h(void) { return BITMAP_HIGHEST_H; }

// These may be empty arrays but need valid pointers
__attribute__((export_name("get_uninv_mat_data")))
void* get_uninv_mat_data(void) { return (void*)uninv_mat_data; }
__attribute__((export_name("get_uninv_mat_data_size")))
int get_uninv_mat_data_size(void) { return sizeof(uninv_mat_data); }

__attribute__((export_name("get_gradient_data")))
void* get_gradient_data(void) { return (void*)gradient_data; }
__attribute__((export_name("get_gradient_data_size")))
int get_gradient_data_size(void) { return sizeof(gradient_data); }

__attribute__((export_name("get_bitmap_data")))
void* get_bitmap_data(void) { return (void*)bitmap_data; }
__attribute__((export_name("get_bitmap_data_size")))
int get_bitmap_data_size(void) { return sizeof(bitmap_data); }

__attribute__((export_name("get_glyph_data")))
void* get_glyph_data(void) { return (void*)glyph_data; }
__attribute__((export_name("get_glyph_data_size")))
int get_glyph_data_size(void) { return sizeof(glyph_data); }

__attribute__((export_name("get_text_data")))
void* get_text_data(void) { return (void*)text_data; }
__attribute__((export_name("get_text_data_size")))
int get_text_data_size(void) { return sizeof(text_data); }

__attribute__((export_name("get_cxform_data")))
void* get_cxform_data(void) { return (void*)cxform_data; }
__attribute__((export_name("get_cxform_data_size")))
int get_cxform_data_size(void) { return sizeof(cxform_data); }

__attribute__((export_name("get_morph_end_shape_data")))
void* get_morph_end_shape_data(void) { return (void*)morph_end_shape_data; }
__attribute__((export_name("get_morph_end_shape_data_size")))
int get_morph_end_shape_data_size(void) { return sizeof(morph_end_shape_data); }

__attribute__((export_name("get_morph_end_color_data")))
void* get_morph_end_color_data(void) { return (void*)morph_end_color_data; }
__attribute__((export_name("get_morph_end_color_data_size")))
int get_morph_end_color_data_size(void) { return sizeof(morph_end_color_data); }

#ifdef MAX_STRING_ID
__attribute__((export_name("get_max_string_id")))
int get_max_string_id(void) { return MAX_STRING_ID; }
#else
__attribute__((export_name("get_max_string_id")))
int get_max_string_id(void) { return 0; }
#endif

// --- Tables the host reads via pointer (DYNAMIC_HOST, see generated_data.h) ---
// With the guest's function table mirrored into the host's reserved low table
// slots (build_graphics_host.sh --table-base), the function pointers stored in
// these guest arrays are valid in the host as-is: no translation needed.
__attribute__((export_name("get_frame_funcs")))
void* get_frame_funcs(void) { return (void*)frame_funcs; }

extern FrameLabelEntry frame_label_data[];
extern size_t frame_label_count;
__attribute__((export_name("get_frame_label_data")))
void* get_frame_label_data(void) { return (void*)frame_label_data; }
__attribute__((export_name("get_frame_label_count")))
int get_frame_label_count(void) { return (int)frame_label_count; }

extern u16 __attribute__((weak)) text_char_codes[];
__attribute__((export_name("get_text_char_codes")))
void* get_text_char_codes(void) { return (void*)text_char_codes; }

// tagInit as a function pointer: taking its address puts it in the guest's
// function table, so the mirrored index is a valid host-side pointer.
static void (*const tagInit_ptr)(SWFAppContext*) = tagInit;
__attribute__((export_name("get_tagInit_ptr")))
int get_tagInit_ptr(void) { return (int)tagInit_ptr; }

#ifdef SWF_AVM2
// --- AVM2 guests (RecompiledABC/ present; the page compiles with -DSWF_AVM2) ---
// The AVM2 host variant (build_graphics_host.sh AVM2=1) reads the generated
// tables through pointers filled by setAvm2Tables from the per-movie
// aggregate avm2_movie_tables (RecompiledABC/avm2_movie_tables.c) plus the
// few arrays the aggregate does not carry. The method function pointers in
// the guest's Avm2AbcMethodData tables are valid host-side through the
// mirrored function table, exactly like frame_funcs.
#include <avm2/avm2_abc.h>
extern const Avm2MovieTables avm2_movie_tables;
__attribute__((export_name("get_avm2_movie_tables")))
const void* get_avm2_movie_tables(void) { return &avm2_movie_tables; }
__attribute__((export_name("get_avm2_abc_frames")))
const void* get_avm2_abc_frames(void) { return avm2_generated_abc_frames; }
__attribute__((export_name("get_avm2_abc_lazy")))
const void* get_avm2_abc_lazy(void) { return avm2_generated_abc_lazy; }
__attribute__((export_name("get_avm2_symbol_class_frames")))
const void* get_avm2_symbol_class_frames(void) { return avm2_generated_symbol_class_frames; }
__attribute__((export_name("get_avm2_device_fonts")))
const void* get_avm2_device_fonts(void) { return avm2_generated_device_fonts; }
__attribute__((export_name("get_avm2_device_font_count")))
int get_avm2_device_font_count(void) { return (int)avm2_generated_device_font_count; }
#endif
