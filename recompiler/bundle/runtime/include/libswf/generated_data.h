// Declarations of the per-SWF data tables that the recompiler emits
// (draws.c / constants.c / tagMain.c) and that the runtime reads directly.
//
// Normal builds declare them as the arrays the generated code defines. The
// in-browser recompiler's graphics HOST build (-DDYNAMIC_HOST) has no
// generated code linked in: the per-SWF module is compiled in the browser
// and instantiated into the host's memory afterwards, so the host declares
// the same names as POINTERS that the JS bridge sets to the guest's arrays
// (see SWFRecomp/wasm_wrappers/host_main_graphics.c). Indexing syntax is
// identical for both forms, so call sites need no change; the runtime never
// takes sizeof() of these (sizes come from app_context.*_size).
#ifndef GENERATED_DATA_H
#define GENERATED_DATA_H

#include <common.h>
#include <tag.h>   // FrameLabelEntry

#ifdef DYNAMIC_HOST
#define GEN_EXTERN_TRANSFORM_DATA       extern float (*transform_data)[16]
#define GEN_EXTERN_SHAPE_DATA           extern u32 (*shape_data)[4]
#define GEN_EXTERN_GLYPH_DATA           extern u32 (*glyph_data)[1]
#define GEN_EXTERN_TEXT_DATA            extern u32* text_data
#define GEN_EXTERN_TEXT_CHAR_CODES      extern u16* text_char_codes
#define GEN_EXTERN_CXFORM_DATA          extern float* cxform_data
#define GEN_EXTERN_BITMAP_DATA          extern u8* bitmap_data
#define GEN_EXTERN_MORPH_END_SHAPE_DATA extern float (*morph_end_shape_data)[2]
#define GEN_EXTERN_FRAME_LABEL_DATA     extern FrameLabelEntry* frame_label_data
// Read directly by the AVM2 runtime (avm2_cpu_raster.c / avm2_display.c).
#define GEN_EXTERN_COLOR_DATA           extern float (*color_data)[4]
#define GEN_EXTERN_UNINV_MAT_DATA       extern float* uninv_mat_data
#define GEN_EXTERN_GRADIENT_DATA        extern u8 (*gradient_data)[4]
#define GEN_EXTERN_MORPH_END_COLOR_DATA extern float (*morph_end_color_data)[4]
#else
#define GEN_EXTERN_TRANSFORM_DATA       extern float transform_data[][16]
#define GEN_EXTERN_SHAPE_DATA           extern u32 shape_data[][4]
#define GEN_EXTERN_GLYPH_DATA           extern u32 glyph_data[][1]
#define GEN_EXTERN_TEXT_DATA            extern u32 text_data[]
// text_char_codes is emitted by newer recompiler builds (Unicode code points);
// weak so older generated code without it still links (NULL -> fallback).
#define GEN_EXTERN_TEXT_CHAR_CODES      extern u16 __attribute__((weak)) text_char_codes[]
#define GEN_EXTERN_CXFORM_DATA          extern float cxform_data[]
#define GEN_EXTERN_BITMAP_DATA          extern u8 bitmap_data[]
#define GEN_EXTERN_MORPH_END_SHAPE_DATA extern float morph_end_shape_data[][2]
#define GEN_EXTERN_FRAME_LABEL_DATA     extern FrameLabelEntry frame_label_data[]
#define GEN_EXTERN_COLOR_DATA           extern float color_data[][4]
#define GEN_EXTERN_UNINV_MAT_DATA       extern float uninv_mat_data[]
#define GEN_EXTERN_GRADIENT_DATA        extern u8 gradient_data[][4]
#define GEN_EXTERN_MORPH_END_COLOR_DATA extern float morph_end_color_data[][4]
#endif
// frame_label_count is a scalar in both builds (the host copies the value).

#endif // GENERATED_DATA_H
