#ifndef AVM2_CPU_RASTER_H
#define AVM2_CPU_RASTER_H

#include <stdint.h>

// AVM2 CPU shape rasterizer (T5). Dawn-free triangle rasterizer for the
// headless AVM2 render path (AVM2_CPU_DUMP) and BitmapData.draw() of a shape
// source. Composites a resident, pre-tessellated timeline-shape vertex range
// (shape_data[vert_offset .. vert_offset+vert_count)) into a premultiplied-ARGB
// target, mirroring the WGSL fragment shader (render_webgpu.c) so the CPU output
// is identical to the GPU/Dawn sink — hence to Ruffle (which the GPU path is
// already pixel-matched against).
//
// `w*` are the node's world matrix (shape-local twips -> target twips); the
// raster divides by 20 to reach device pixels, the same convention
// avm2_cpu_composite_bitmap uses (1 px = 20 twips, pixel centres at +0.5).
// `node_alpha` (0..1) is the concatenated display-tree alpha. `transparent` is
// the target BitmapData's transparency flag (kept for the store representation;
// blending itself always uses the true source alpha).
//
// Fill classes this tranche: solid (0x00), strokes (packed 0x80000000 -> low
// byte 0x00 -> solid via colour index), and gradients (0x10 linear / 0x12
// radial / 0x13 focal, pad/reflect/repeat). Bitmap fills (0x40-0x43) and masks
// (clip_depth, T7) are skipped.
void avm2_cpu_raster_shape(uint32_t* buf, int W, int H, int transparent,
                           uint32_t vert_offset, uint32_t vert_count,
                           double wa, double wb, double wc, double wd,
                           double wtx, double wty, double node_alpha);

// T4 Part B — fill descriptor for runtime (flash.display.Graphics) triangles.
// kind: 1 solid, 2 gradient. Solid uses r/g/b/a (straight 0..1). Gradient uses
// grad_type (0x10/0x12/0x13), spread (0/1/2), interp (0 rgb / 1 linearRGB),
// focal, a 256*4 RGBA8 `ramp`, and `inv2d` (the normalized inverse 2D affine
// mapping shape-local twips -> UV[0,1], as render_webgpu.c's dynamic path does).
typedef struct
{
	int kind;
	float r, g, b, a;
	unsigned char grad_type, spread, interp;
	float focal;
	const unsigned char* ramp;   // 256*4
	float inv2d[6];              // a,b,c,d,tx,ty
} Avm2GfxFill;

// Rasterize an explicit runtime triangle list (`xy_twips`, shape-local twips,
// vert_count % 3 == 0) into a premultiplied-ARGB target under the node world
// matrix (shape-local twips -> target twips; /20 -> device px) + concatenated
// alpha, shaded per `fill`. Mirrors avm2_cpu_raster_shape but for script-built
// geometry (no static shape_data range).
void avm2_cpu_raster_tris(uint32_t* buf, int W, int H, int transparent,
                          const float* xy_twips, uint32_t vert_count,
                          const Avm2GfxFill* fill,
                          double wa, double wb, double wc, double wd,
                          double wtx, double wty, double node_alpha);

// T6 — rasterize a DefineMorphShape at placement `ratio` (0..1): per-vertex
// lerp of the START vertex range (shape_data[vert_offset..]) with its paired
// END range (morph_end_shape_data[morph_end_offset..]), and per-triangle lerp
// of the solid fill colour. Solid fills only (gradient/stroke morph deferred).
void avm2_cpu_raster_morph(uint32_t* buf, int W, int H, int transparent,
                           uint32_t vert_offset, uint32_t vert_count,
                           uint32_t morph_end_offset, double ratio,
                           double wa, double wb, double wc, double wd,
                           double wtx, double wty, double node_alpha);

// Native TEXT/EDITTEXT — composite a timeline-placed TextField's laid-out glyphs
// (avm2_edittext_collect_glyphs -> flattened outlines, non-zero winding scanline)
// into a premultiplied-ARGB target under the node world matrix (`w*`: field-local
// TWIPS -> target TWIPS; /20 -> device px) + concatenated `node_alpha`. Impl lives
// in avm2_bitmap.c (needs the glyph rasterizer + cxform/blend helpers); this is the
// walk-facing entry (identity cxform, normal blend). Device-font (no outlines) and
// GPU-side x-clip are skipped (see avm2-native-text-render-plan.md).
typedef struct Avm2Context Avm2Context;
typedef struct Avm2Object Avm2Object;
void avm2_cpu_raster_text(uint32_t* buf, int W, int H, int transparent,
                          Avm2Context* ctx, Avm2Object* tf_obj,
                          double wa, double wb, double wc, double wd,
                          double wtx, double wty, double node_alpha);

// Native static TEXT (DefineText/2 -> StaticText) — walk-facing twin of
// avm2_cpu_raster_text, sharing the same glyph-scanline core. Sources placements
// from the node's resolved ext->statictext (avm2_generated_static_glyphs) rather
// than the EditText layout engine. Unclipped; identity cxform, normal blend.
void avm2_cpu_raster_statictext(uint32_t* buf, int W, int H, int transparent,
                                Avm2Context* ctx, Avm2Object* obj,
                                double wa, double wb, double wc, double wd,
                                double wtx, double wty, double node_alpha);

#endif // AVM2_CPU_RASTER_H
