// WebGPU rendering backend for SWFModernRuntime.
// Implements the same API surface as flashbang.c using the standardized
// webgpu.h C API. Works for both native (Dawn/wgpu-native) and WASM (emdawnwebgpu).

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include <common.h>
#include <render_webgpu.h>
#include <swf.h>
#include <heap.h>

// Renderer-mode flag. Set when the renderer must run without a window
// surface — no SDL, no JS canvas. The --mode=graphics native build uses
// offscreen rendering with the same swf.c frame loop the browser runs.
#ifdef OFFSCREEN_RENDER
// Offscreen mode: no SDL, no Emscripten — render to a buffer for capture.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#elif !defined(__EMSCRIPTEN__)
#include <SDL3/SDL.h>
#include "sdl3webgpu.h"
#else
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

// Helper: convert C string to WGPUStringView for descriptor labels.
#define WGPU_LABEL(s) ((WGPUStringView){.data = (s), .length = WGPU_STRLEN})

// Console/stderr diagnostic line (see the definition next to the bitmap pools:
// browser console via emscripten_log, native stderr under SWF_WARN_BITMAP_CAP).
static void bitmap_pool_log(const char* fmt, ...);
// Browser-only init/allocation trace: one line per GPU buffer and init step,
// so a device loss on a real GPU can be placed (Dawn's own message only names
// the allocation that failed AFTER the device was already gone).
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
#define RENDER_INIT_LOG(...) bitmap_pool_log(__VA_ARGS__)
#else
#define RENDER_INIT_LOG(...) do { } while (0)
#endif

// ---------------------------------------------------------------------------
// Antialiasing sample count. Flash's Stage.quality selects the AA level; Ruffle
// maps StageQuality::Low -> 1 sample, Medium -> 2, High/Best -> 4
// (ruffle/render/src/quality.rs). The test harness passes -DMSAA_SAMPLES=N per
// test from [player_options].with_renderer.quality so low-quality goldens (which
// Ruffle renders with NO antialiasing at all) are graded against a matching
// render. Unset — browser, every game/demo build, and every quality != "low"
// test — keeps the historical 4x, so those paths are bit-identical.
//
// At MSAA_SAMPLES == 1 the multisample colour targets have no resolve step: the
// passes render straight into the single-sample destination (see the guarded
// attachment pairs in begin_pass / resume_pass / begin_offscreen_pass).
// Must live outside the OFFSCREEN_RENDER / __EMSCRIPTEN__ include block above.
// ---------------------------------------------------------------------------
#ifndef MSAA_SAMPLES
#define MSAA_SAMPLES 4
#endif

// ---------------------------------------------------------------------------
// Browser-WASM render perf counters (per close_pass): writeBuffer call/byte
// count + draw-call count + queue-submit time, published to JS via
// swf_render_stats(...) on window.__swfRender for the Ruffle-vs-SWFRecomp
// comparison harness (tools/divergence/perf/). These are the cross-backend-real
// levers (unlike wall-clock frame time, which on SwiftShader is GPU-emulation
// bound). Gated to __EMSCRIPTEN__ && !OFFSCREEN_RENDER so native / offscreen
// (the CI suites) compile byte-identically — not CI-observable.
// ---------------------------------------------------------------------------
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
static unsigned long g_perf_wb_calls = 0;
static unsigned long g_perf_wb_bytes = 0;
static unsigned long g_perf_draw_calls = 0;
static unsigned long g_perf_wt_calls = 0;
static unsigned long g_perf_wt_bytes = 0;
// Count + forward to the real API. The parenthesised callee name is NOT a
// macro invocation (not followed by '('), so this does not recurse.
#define wgpuQueueWriteBuffer(q, b, o, d, s) \
	do { g_perf_wb_calls++; g_perf_wb_bytes += (unsigned long)(s); FRAME_DIGEST_U64(o); FRAME_DIGEST_BYTES(d, s); \
	     (wgpuQueueWriteBuffer)(q, b, o, d, s); } while (0)
#define wgpuQueueWriteTexture(q, d, data, sz, l, e) \
	do { g_perf_wt_calls++; g_perf_wt_bytes += (unsigned long)(sz); FRAME_DIGEST_BYTES(data, sz); \
	     (wgpuQueueWriteTexture)(q, d, data, sz, l, e); } while (0)
#define wgpuRenderPassEncoderDraw(p, vc, ic, fv, fi) \
	do { g_perf_draw_calls++; FRAME_DIGEST_DRAW(vc, ic, fv, fi); (wgpuRenderPassEncoderDraw)(p, vc, ic, fv, fi); } while (0)

EM_JS(void, swf_render_stats, (double wb_calls, double wb_bytes, double draws, double submit_ms, double wt_calls, double wt_bytes), {
	var R = globalThis.__swfRender;
	if (!R) R = globalThis.__swfRender = { wb: [], bytes: [], draws: [], submit: [], wt: [], wtbytes: [], cap: 240, i: 0 };
	var push = function(k, v) { if (R[k].length < R.cap) R[k].push(v); else R[k][R.i] = v; };
	push('wb', wb_calls); push('bytes', wb_bytes); push('draws', draws); push('submit', submit_ms);
	push('wt', wt_calls); push('wtbytes', wt_bytes);
	if (R.wb.length >= R.cap) R.i = (R.i + 1) % R.cap;
});
#endif

// ---------------------------------------------------------------------------
// Embedded WGSL shader sources
// ---------------------------------------------------------------------------

static const char* vertex_wgsl =
"@group(0) @binding(0) var<storage, read> transforms: array<mat4x4f>;\n"
"@group(0) @binding(1) var<storage, read> colors: array<vec4f>;\n"
"@group(0) @binding(2) var<storage, read> inv_mats: array<mat4x4f>;\n"
// bitmap_sizes: 4 u32 per texture-array layer — {content_w, content_h,
// padded_w, padded_h}. `content` is the bitmap's OWN size (the repeat period,
// per Ruffle's swf_bitmap_to_gl_matrix, render/src/tessellator.rs:356-383);
// `padded` is the shared layer size, which is the max over every bitmap in the
// movie and so is generally LARGER. Tiling on `padded` is what left transparent
// gaps in from_shumway/acid/acid-bitmap-fill.
"@group(0) @binding(3) var<storage, read> bitmap_sizes: array<vec4u>;\n"
"\n"
"struct StageTransform { matrix: mat4x4f };\n"
"@group(1) @binding(0) var<uniform> stage_transform: StageTransform;\n"
"\n"
"struct TransformID { id: u32 };\n"
"@group(1) @binding(1) var<uniform> current_transform: TransformID;\n"
"\n"
"struct VertexInput {\n"
"  @location(0) position: vec2f,\n"
"  @location(1) style: vec2u,\n"
"};\n"
"struct VertexOutput {\n"
"  @builtin(position) position: vec4f,\n"
"  @location(0) @interpolate(flat) v_style_type: u32,\n"
"  @location(1) @interpolate(flat) v_style_id: u32,\n"
"  @location(2) v_args: vec4f,\n"
"  @location(3) @interpolate(flat) v_cxform_id: u32,\n"
"};\n"
"\n"
"@vertex\n"
"fn vs_main(in: VertexInput, @builtin(instance_index) instance_id: u32) -> VertexOutput {\n"
"  var out: VertexOutput;\n"
"  let transform_id = instance_id & 0xFFFFu;\n"
"  let cxform_id = instance_id >> 16u;\n"
"  let transform = transforms[transform_id];\n"
"  let pos = vec4f(in.position, 0.0, 1.0);\n"
"  let spread_mode = (in.style.x >> 8u) & 0x3u;\n"
"  let is_linear_rgb = (in.style.x >> 10u) & 0x1u;\n"
"  out.v_style_type = in.style.x & 0xFFu;\n"
"  out.v_style_id = in.style.y & 0xFFFFu;\n"
"  let style_upper = (in.style.y >> 16u) & 0xFFFFu;\n"
"  out.position = stage_transform.matrix * transform * pos;\n"
"  if (out.v_style_type == 0x00u) {\n"
"    out.v_args = colors[out.v_style_id];\n"
"  } else if ((out.v_style_type & 0xF0u) == 0x10u) {\n"
"    let inv_pos = inv_mats[out.v_style_id] * pos;\n"
"    var focal_z = 0.0;\n"
"    if (out.v_style_type == 0x13u) {\n"
"      focal_z = (f32(style_upper) - 32768.0) / 16384.0;\n"
"    }\n"
"    out.v_args = vec4f(inv_pos.xy, focal_z, f32(spread_mode) + f32(is_linear_rgb) * 4.0);\n"
"  } else if ((out.v_style_type & 0xF0u) == 0x40u) {\n"
"    let inv_pos = inv_mats[style_upper] * pos;\n"
"    let sizes = bitmap_sizes[out.v_style_id * 2u];\n"
"    let slot = bitmap_sizes[out.v_style_id * 2u + 1u];\n"
"    let content = vec2f(f32(sizes.x), f32(sizes.y));\n"
"    let padded = vec2f(f32(sizes.z), f32(sizes.w));\n"
// The vertex carries a bitmap SLOT (static bitmap index, or a per-frame
// dynamic slot); the second vec4u of its table entry says which texture POOL
// holds it and at which layer (see "Bitmap texture pools" in the C code).
// Re-pack that as pool<<16 | layer for the fragment stage.
"    out.v_style_id = (slot.y & 0xFFFFu) | ((slot.x & 0xFFu) << 16u);\n"
// Style-word bit 11 (dynamic beginBitmapFill, clipped): clamp the fill to the
// bitmap's own content instead of reading the rest of the pool layer, which a
// previous frame may have written. Forwarded as bit 8 of v_style_type; the
// fragment stage compares the fill type on the low byte only.
"    out.v_style_type = out.v_style_type | ((in.style.x >> 3u) & 0x100u);\n"
// Repeating fills (0x40/0x42) tile on the bitmap's own size: v_args.xy is the
// position in TILE units, v_args.zw scales one tile back into layer UV. The
// clipped arm (0x41/0x43) keeps the old expression character-for-character so
// the patch is provably a no-op for every non-repeating bitmap fill.
"    if ((out.v_style_type & 0x1u) == 0u) {\n"
"      out.v_args = vec4f(inv_pos.x / content.x, inv_pos.y / content.y, content.x / padded.x, content.y / padded.y);\n"
"    } else if ((out.v_style_type & 0x100u) != 0u) {\n"
"      out.v_args = vec4f(inv_pos.x / padded.x, inv_pos.y / padded.y, content.x / padded.x, content.y / padded.y);\n"
"    } else {\n"
"      out.v_args = vec4f(inv_pos.x / padded.x, inv_pos.y / padded.y, 1.0, 1.0);\n"
"    }\n"
"  } else {\n"
"    out.v_args = vec4f(0.0);\n"
"  }\n"
"  out.v_cxform_id = cxform_id;\n"
"  return out;\n"
"}\n";

static const char* fragment_wgsl =
"@group(0) @binding(4) var<storage, read> cxforms: array<vec4f>;\n"
"\n"
"@group(2) @binding(0) var gradient_tex: texture_2d<f32>;\n"
"@group(2) @binding(1) var gradient_samp: sampler;\n"
"@group(2) @binding(2) var bitmap_samp: sampler;\n"
// Smoothed bitmap fills (SWF fill types 0x40 / 0x41) sample through a LINEAR
// sampler; non-smoothed ones (0x42 / 0x43) keep the Nearest sampler above. The
// bit lives in the fill-type byte itself and the recompiler emits that byte
// verbatim as the vertex style type, so the split costs one extra sampler
// binding and no plumbing. A GLOBAL linear sampler is REFUTED: it takes
// from_shumway/acid/acid-blend (a 0x43 non-smoothed fill) from 101 outliers to
// 1961 against a 348 budget (w1 gfx-vram section 4.2).
"@group(2) @binding(3) var bitmap_samp_linear: sampler;\n"
// Bitmap texture POOLS (BITMAP_POOL_COUNT of them, bindings 4..15): each is a
// texture_2d_array whose layers are all the same size, but the size differs
// per pool, so a movie's bitmaps no longer share one (widest x tallest)
// envelope. Empty pools are bound to the 1x1 dummy array. The pool index
// travels in the upper 16 bits of v_style_id, the layer in the lower 16.
"@group(2) @binding(4) var bitmap_pool0: texture_2d_array<f32>;\n"
"@group(2) @binding(5) var bitmap_pool1: texture_2d_array<f32>;\n"
"@group(2) @binding(6) var bitmap_pool2: texture_2d_array<f32>;\n"
"@group(2) @binding(7) var bitmap_pool3: texture_2d_array<f32>;\n"
"@group(2) @binding(8) var bitmap_pool4: texture_2d_array<f32>;\n"
"@group(2) @binding(9) var bitmap_pool5: texture_2d_array<f32>;\n"
"@group(2) @binding(10) var bitmap_pool6: texture_2d_array<f32>;\n"
"@group(2) @binding(11) var bitmap_pool7: texture_2d_array<f32>;\n"
"@group(2) @binding(12) var bitmap_pool8: texture_2d_array<f32>;\n"
"@group(2) @binding(13) var bitmap_pool9: texture_2d_array<f32>;\n"
"@group(2) @binding(14) var bitmap_pool10: texture_2d_array<f32>;\n"
"@group(2) @binding(15) var bitmap_pool11: texture_2d_array<f32>;\n"
"\n"
"struct FragmentInput {\n"
"  @location(0) @interpolate(flat) v_style_type: u32,\n"
"  @location(1) @interpolate(flat) v_style_id: u32,\n"
"  @location(2) v_args: vec4f,\n"
"  @location(3) @interpolate(flat) v_cxform_id: u32,\n"
"};\n"
"\n"
"fn apply_spread(t: f32, mode: f32) -> f32 {\n"
"  let m = u32(mode + 0.5) & 0x3u;\n"
"  if (m == 1u) {\n"
"    // Reflect: triangle wave\n"
"    let p = t - 2.0 * floor(t / 2.0);\n"
"    return select(2.0 - p, p, p <= 1.0);\n"
"  } else if (m == 2u) {\n"
"    // Repeat: fract\n"
"    return t - floor(t);\n"
"  }\n"
"  // Pad: clamp 0-1\n"
"  return clamp(t, 0.0, 1.0);\n"
"}\n"
"\n"
"// UVs are now in [0,1] range (normalization baked into inverse matrix)\n"
"fn linear_t(v_args: vec4f) -> f32 { return apply_spread(v_args.x, v_args.w); }\n"
"fn radial_t(v_args: vec4f) -> f32 { return apply_spread(length(v_args.xy * 2.0 - 1.0), v_args.w); }\n"
"fn focal_radial_t(v_args: vec4f) -> f32 {\n"
"  // Match Ruffle's focal gradient formula exactly (gradient.wgsl)\n"
"  let f = v_args.z;\n"
"  let uv = v_args.xy * 2.0 - 1.0;\n"
"  var d = vec2f(f, 0.0) - uv;\n"
"  let l = length(d);\n"
"  if (l < 0.00001) { return 0.0; }\n"
"  d = d / l;\n"
"  let denom = sqrt(max(1.0 - f * f * d.y * d.y, 0.0)) + f * d.x;\n"
"  if (abs(denom) < 0.00001) { return 0.0; }\n"
"  return apply_spread(l / denom, v_args.w);\n"
"}\n"
"\n"
"fn linear_to_srgb_ch(c: f32) -> f32 {\n"
"  if (c <= 0.0031308) { return c * 12.92; }\n"
"  return 1.055 * pow(c, 1.0 / 2.4) - 0.055;\n"
"}\n"
"fn apply_linear_to_srgb(color: vec4f) -> vec4f {\n"
"  var rgb = color.rgb;\n"
"  if (color.a > 0.0) { rgb = rgb / color.a; }\n"
"  let result = vec3f(linear_to_srgb_ch(rgb.r), linear_to_srgb_ch(rgb.g), linear_to_srgb_ch(rgb.b));\n"
"  return vec4f(result * color.a, color.a);\n"
"}\n"
"\n"
"fn apply_cxform(color: vec4f, cxform_id: u32) -> vec4f {\n"
"  let ci = cxform_id * 5u;\n"
"  let mult = mat4x4f(cxforms[ci], cxforms[ci+1u], cxforms[ci+2u], cxforms[ci+3u]);\n"
"  let add = cxforms[ci+4u];\n"
"  return clamp(mult * color + add, vec4f(0.0), vec4f(1.0));\n"
"}\n"
"\n"
"// Sample a gradient ramp (256x1) stored as `row` of the packed 256xN gradient\n"
"// texture. textureLoad selects the row with EXACT integer coords (no V\n"
"// filtering, so no bleed into adjacent ramps even on adapters whose linear\n"
"// filtering doesn't land precisely on the texel center — e.g. SwiftShader),\n"
"// and we lerp along U by hand to keep the smooth ramp. `t` is already in [0,1]\n"
"// (spread/clamp applied by the *_t helpers).\n"
"fn sample_gradient(t: f32, row: i32) -> vec4f {\n"
"  // Replicate Ruffle's HARDWARE linear sampler on a 256-texel ramp with\n"
"  // clamp-to-edge addressing: texcoord t maps to texel-space coord t*256, with\n"
"  // texel centers at i+0.5, so the effective fractional index is t*256-0.5\n"
"  // (clamped to [0,255]). Using t*255 here instead introduced a sub-pixel\n"
"  // sampling-phase offset of (0.5 - t) ramp indices — invisible on slow\n"
"  // pad/linear gradients but landing exactly on the steep parts and wrap edges\n"
"  // of focal/repeat/reflect cycles. We keep textureLoad (not textureSample) to\n"
"  // pick the ROW exactly (no V-filter bleed across ramps on SwiftShader) but\n"
"  // do the U interpolation by hand to match the sampler convention.\n"
"  let u = clamp(clamp(t, 0.0, 1.0) * 256.0 - 0.5, 0.0, 255.0);\n"
"  let u0 = i32(floor(u));\n"
"  let u1 = min(u0 + 1, 255);\n"
"  let f = u - f32(u0);\n"
"  let c0 = textureLoad(gradient_tex, vec2i(u0, row), 0);\n"
"  let c1 = textureLoad(gradient_tex, vec2i(u1, row), 0);\n"
"  return mix(c0, c1, f);\n"
"}\n"
"\n"
// Sample one bitmap pool at (uv, layer). WGSL cannot index a binding by a
// runtime value, so this is a switch; textureSampleLevel with an explicit LOD
// has no uniformity requirement, so being inside a non-uniform branch is fine.
"fn sample_bitmap(pool: u32, uv: vec2f, layer: i32, smoothed: bool) -> vec4f {\n"
"  var c = vec4f(0.0);\n"
"  if (smoothed) {\n"
"    switch (pool) {\n"
"      case 0u: { c = textureSampleLevel(bitmap_pool0, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      case 1u: { c = textureSampleLevel(bitmap_pool1, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      case 2u: { c = textureSampleLevel(bitmap_pool2, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      case 3u: { c = textureSampleLevel(bitmap_pool3, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      case 4u: { c = textureSampleLevel(bitmap_pool4, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      case 5u: { c = textureSampleLevel(bitmap_pool5, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      case 6u: { c = textureSampleLevel(bitmap_pool6, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      case 7u: { c = textureSampleLevel(bitmap_pool7, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      case 8u: { c = textureSampleLevel(bitmap_pool8, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      case 9u: { c = textureSampleLevel(bitmap_pool9, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      case 10u: { c = textureSampleLevel(bitmap_pool10, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      case 11u: { c = textureSampleLevel(bitmap_pool11, bitmap_samp_linear, uv, layer, 0.0); }\n"
"      default: {}\n"
"    }\n"
"  } else {\n"
"    switch (pool) {\n"
"      case 0u: { c = textureSampleLevel(bitmap_pool0, bitmap_samp, uv, layer, 0.0); }\n"
"      case 1u: { c = textureSampleLevel(bitmap_pool1, bitmap_samp, uv, layer, 0.0); }\n"
"      case 2u: { c = textureSampleLevel(bitmap_pool2, bitmap_samp, uv, layer, 0.0); }\n"
"      case 3u: { c = textureSampleLevel(bitmap_pool3, bitmap_samp, uv, layer, 0.0); }\n"
"      case 4u: { c = textureSampleLevel(bitmap_pool4, bitmap_samp, uv, layer, 0.0); }\n"
"      case 5u: { c = textureSampleLevel(bitmap_pool5, bitmap_samp, uv, layer, 0.0); }\n"
"      case 6u: { c = textureSampleLevel(bitmap_pool6, bitmap_samp, uv, layer, 0.0); }\n"
"      case 7u: { c = textureSampleLevel(bitmap_pool7, bitmap_samp, uv, layer, 0.0); }\n"
"      case 8u: { c = textureSampleLevel(bitmap_pool8, bitmap_samp, uv, layer, 0.0); }\n"
"      case 9u: { c = textureSampleLevel(bitmap_pool9, bitmap_samp, uv, layer, 0.0); }\n"
"      case 10u: { c = textureSampleLevel(bitmap_pool10, bitmap_samp, uv, layer, 0.0); }\n"
"      case 11u: { c = textureSampleLevel(bitmap_pool11, bitmap_samp, uv, layer, 0.0); }\n"
"      default: {}\n"
"    }\n"
"  }\n"
"  return c;\n"
"}\n"
"\n"
"@fragment\n"
"fn fs_main(in: FragmentInput) -> @location(0) vec4f {\n"
"  // Compute ONLY the texture sample the fill type actually needs, so a solid-colour\n"
"  // fragment does zero texture ops (the common case for fill-heavy/high-overdraw\n"
"  // content). Two WGSL facts make this legal inside non-uniform branches:\n"
"  //   - textureLoad has no uniform-control-flow requirement (the gradient ramp\n"
"  //     samples already use it), so sample_gradient can move into its branch as-is.\n"
"  //   - textureSampleLevel(..., 0.0) takes an explicit LOD => no implicit derivatives\n"
"  //     => also legal in non-uniform flow. Bitmaps here are single-level, so LOD 0.0\n"
"  //     reads the same texels textureSample would (which only ever sampled level 0).\n"
"  // The old per-fill OOB layer guard (select(0, v_style_id, is_*)) is no longer needed:\n"
"  // a texture is only sampled inside its own branch, where v_style_id is a valid layer.\n"
"  // Gradients are packed one ramp per ROW of a 256xN 2D texture (not array layers —\n"
"  // avoids the 256 maxTextureArrayLayers cap on some adapters); the row is selected\n"
"  // with textureLoad (exact, no V-filter bleed); see sample_gradient.\n"
"  let is_gradient = (in.v_style_type & 0xF0u) == 0x10u;\n"
// Low byte = SWF fill type; bit 8 = clamp-to-content flag (bitmap fills only).
"  let ftype = in.v_style_type & 0xFFu;\n"
"  var color: vec4f;\n"
"  if (ftype == 0x00u) {\n"
"    color = in.v_args;\n"
"  } else if (ftype == 0x10u) {\n"
"    color = sample_gradient(linear_t(in.v_args), i32(in.v_style_id));\n"
"  } else if (ftype == 0x12u) {\n"
"    color = sample_gradient(radial_t(in.v_args), i32(in.v_style_id));\n"
"  } else if (ftype == 0x13u) {\n"
"    color = sample_gradient(focal_radial_t(in.v_args), i32(in.v_style_id));\n"
"  } else if (ftype == 0x40u || ftype == 0x42u) {\n"
"    let bm_ratio = max(in.v_args.zw, vec2f(0.001));\n"
"    let uv = fract(in.v_args.xy) * bm_ratio;\n"
"    color = sample_bitmap(in.v_style_id >> 16u, uv, i32(in.v_style_id & 0xFFFFu), ftype == 0x40u);\n"
"  } else if (ftype == 0x41u || ftype == 0x43u) {\n"
"    var uv = in.v_args.xy;\n"
"    if ((in.v_style_type & 0x100u) != 0u) { uv = clamp(uv, vec2f(0.0), in.v_args.zw); }\n"
"    color = sample_bitmap(in.v_style_id >> 16u, uv, i32(in.v_style_id & 0xFFFFu), ftype == 0x41u);\n"
"  } else {\n"
"    color = vec4f(0.0);\n"
"  }\n"
"  // LinearRGB gradients: ramp stored in linear space, convert to sRGB\n"
"  if (is_gradient && (u32(in.v_args.w + 0.5) & 4u) != 0u) {\n"
"    color = apply_linear_to_srgb(color);\n"
"  }\n"
"  // Ruffle's bitmap shader (render/wgpu/shaders/bitmap.wgsl) guards the whole\n"
"  // colour-transform step with `if (color.a > 0.0)`: a FULLY TRANSPARENT texel\n"
"  // is left exactly as it is, so an additive alpha term cannot conjure ink out\n"
"  // of nothing. color.wgsl (solid fills, gradients) has no such guard, hence\n"
"  // the bitmap-only test here. s17 P3: cab_mask_transform's masker is a bitmap\n"
"  // whose last column is alpha 0 under a ColorTransform with alphaOffset 64 —\n"
"  // without the guard that column masked at 25 percent instead of 0.\n"
"  if ((in.v_style_type & 0xF0u) == 0x40u && color.a <= 0.0) { return color; }\n"
"  return apply_cxform(color, in.v_cxform_id);\n"
"}\n";

static const char* compute_wgsl =
"@group(0) @binding(0) var<storage, read> gradmats: array<mat4x4f>;\n"
"@group(1) @binding(0) var<storage, read_write> inv_gradmats: array<mat4x4f>;\n"
"\n"
"fn mat4_inverse(m: mat4x4f) -> mat4x4f {\n"
"  let a00 = m[0][0]; let a01 = m[0][1]; let a02 = m[0][2]; let a03 = m[0][3];\n"
"  let a10 = m[1][0]; let a11 = m[1][1]; let a12 = m[1][2]; let a13 = m[1][3];\n"
"  let a20 = m[2][0]; let a21 = m[2][1]; let a22 = m[2][2]; let a23 = m[2][3];\n"
"  let a30 = m[3][0]; let a31 = m[3][1]; let a32 = m[3][2]; let a33 = m[3][3];\n"
"  let s0 = a00*a11 - a10*a01; let s1 = a00*a12 - a10*a02;\n"
"  let s2 = a00*a13 - a10*a03; let s3 = a01*a12 - a11*a02;\n"
"  let s4 = a01*a13 - a11*a03; let s5 = a02*a13 - a12*a03;\n"
"  let c5 = a22*a33 - a32*a23; let c4 = a21*a33 - a31*a23;\n"
"  let c3 = a21*a32 - a31*a22; let c2 = a20*a33 - a30*a23;\n"
"  let c1 = a20*a32 - a30*a22; let c0 = a20*a31 - a30*a21;\n"
"  let det = s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0;\n"
"  let inv_det = 1.0 / det;\n"
"  return mat4x4f(\n"
"    vec4f( (a11*c5-a12*c4+a13*c3)*inv_det, (-a01*c5+a02*c4-a03*c3)*inv_det,\n"
"           (a31*s5-a32*s4+a33*s3)*inv_det, (-a21*s5+a22*s4-a23*s3)*inv_det),\n"
"    vec4f((-a10*c5+a12*c2-a13*c1)*inv_det, ( a00*c5-a02*c2+a03*c1)*inv_det,\n"
"          (-a30*s5+a32*s2-a33*s1)*inv_det, ( a20*s5-a22*s2+a23*s1)*inv_det),\n"
"    vec4f( (a10*c4-a11*c2+a13*c0)*inv_det, (-a00*c4+a01*c2-a03*c0)*inv_det,\n"
"           (a30*s4-a31*s2+a33*s0)*inv_det, (-a20*s4+a21*s2-a23*s0)*inv_det),\n"
"    vec4f((-a10*c3+a11*c1-a12*c0)*inv_det, ( a00*c3-a01*c1+a02*c0)*inv_det,\n"
"          (-a30*s3+a31*s1-a32*s0)*inv_det, ( a20*s3-a21*s1+a22*s0)*inv_det),\n"
"  );\n"
"}\n"
"\n"
"@compute @workgroup_size(64, 1, 1)\n"
"fn cs_main(@builtin(global_invocation_id) gid: vec3u) {\n"
"  let mat_i = gid.x;\n"
"  inv_gradmats[mat_i] = mat4_inverse(gradmats[mat_i]);\n"
"}\n";

// ---------------------------------------------------------------------------
// Helper: create a WGPUBuffer and optionally upload data
// ---------------------------------------------------------------------------
static WGPUBuffer create_buffer(WGPUDevice device, WGPUQueue queue,
                                WGPUBufferUsage usage,
                                const void* data, size_t size,
                                const char* label)
{
	// Ensure minimum buffer size of 64 bytes. WebGPU requires non-zero buffers,
	// and shader bindings like array<mat4x4f> require at least 64 bytes even
	// when no data is present.
	size_t data_size = size;  // actual data to upload
	if (size < 64) size = 64;
	RENDER_INIT_LOG("[render] buffer %s: %.2f MiB", label, (double)size / (1024.0 * 1024.0));

	WGPUBufferDescriptor desc = {0};
	desc.label = WGPU_LABEL(label);
	desc.size = size;
	desc.usage = usage;
	if (data)
		desc.usage |= WGPUBufferUsage_CopyDst;
	desc.mappedAtCreation = false;

	WGPUBuffer buffer = wgpuDeviceCreateBuffer(device, &desc);
	if (data && data_size > 0)
	{
		wgpuQueueWriteBuffer(queue, buffer, 0, data, data_size);
	}
	return buffer;
}

// ---------------------------------------------------------------------------
// Helper: create a shader module from WGSL source
// ---------------------------------------------------------------------------
static WGPUShaderModule create_shader(WGPUDevice device, const char* wgsl,
                                      const char* label)
{
	WGPUShaderSourceWGSL wgsl_src = {0};
	wgsl_src.chain.sType = WGPUSType_ShaderSourceWGSL;
	wgsl_src.code.data = wgsl;
	wgsl_src.code.length = strlen(wgsl);

	WGPUShaderModuleDescriptor desc = {0};
	desc.label = WGPU_LABEL(label);
	desc.nextInChain = (const WGPUChainedStruct*)&wgsl_src;

	return wgpuDeviceCreateShaderModule(device, &desc);
}

// ---------------------------------------------------------------------------
// Adapter/device request helpers
// ---------------------------------------------------------------------------
// The webgpu.h callback API varies across implementations. These helpers use
// the standard callback-based pattern. On native (Dawn/wgpu-native), the
// callback typically fires synchronously. On WASM with emdawnwebgpu + Asyncify,
// control returns to the browser until the callback fires.

static void on_adapter_ready(WGPURequestAdapterStatus status,
                             WGPUAdapter adapter,
                             struct WGPUStringView message,
                             void* userdata1, void* userdata2)
{
	(void)message;
	(void)userdata2;
	WebGPURenderContext* ctx = (WebGPURenderContext*)userdata1;
	if (status == WGPURequestAdapterStatus_Success)
	{
		ctx->adapter = adapter;
	}
	else
	{
		fprintf(stderr, "Failed to get WebGPU adapter (status %d)\n", (int)status);
	}
}

static void request_adapter_sync(WebGPURenderContext* ctx,
                                 const WGPURequestAdapterOptions* opts)
{
	WGPURequestAdapterCallbackInfo cb_info = {0};
	cb_info.mode = WGPUCallbackMode_AllowSpontaneous;
	cb_info.callback = on_adapter_ready;
	cb_info.userdata1 = ctx;

	WGPUFuture future = wgpuInstanceRequestAdapter(ctx->instance, opts, cb_info);

#ifdef __EMSCRIPTEN__
	// In the browser, async WebGPU operations need us to yield back to
	// the event loop so the browser can process the request.
	while (ctx->adapter == NULL)
	{
		emscripten_sleep(10);
	}
#elif defined(OFFSCREEN_RENDER)
	// Headless: use ProcessEvents polling (WaitAny requires timed wait features)
	(void)future;
	while (ctx->adapter == NULL)
		wgpuInstanceProcessEvents(ctx->instance);
#else
	// Native: poll until the callback fires
	WGPUFutureWaitInfo wait_info = {0};
	wait_info.future = future;
	wgpuInstanceWaitAny(ctx->instance, 1, &wait_info, UINT64_MAX);
#endif
}

static void on_uncaptured_error(const WGPUDevice* device, WGPUErrorType type,
                                struct WGPUStringView message, void* u1, void* u2)
{
	(void)device;(void)u1;(void)u2;
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	// In the browser one invalid object costs an error PER FRAME, forever (a
	// pre-pool Seedling under SwiftShader: 4,458 in 30 s), which buries the
	// first one, the only one that says what went wrong. Print the first few,
	// then count: window.__swfGpu.errors.
	static unsigned errors = 0;
	errors++;
	EM_ASM({
		var g = globalThis.__swfGpu || (globalThis.__swfGpu = { lost: 0, stalls: 0 });
		g.errors = $0;
	}, errors);
	if (errors > 8)
	{
		if (errors == 9)
			fprintf(stderr, "WebGPU: further errors are not printed "
			        "(window.__swfGpu.errors counts them)\n");
		return;
	}
#endif
	fprintf(stderr, "WebGPU error (type %d): %.*s\n", (int)type,
		(int)message.length, message.data ? message.data : "");
}

static void on_device_ready(WGPURequestDeviceStatus status,
                            WGPUDevice device,
                            struct WGPUStringView message,
                            void* userdata1, void* userdata2)
{
	(void)message;
	(void)userdata2;
	WebGPURenderContext* ctx = (WebGPURenderContext*)userdata1;
	if (status == WGPURequestDeviceStatus_Success)
	{
		ctx->device = device;
		ctx->queue = wgpuDeviceGetQueue(device);
	}
	else
	{
		fprintf(stderr, "Failed to get WebGPU device (status %d)\n", (int)status);
	}
}

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
// ---------------------------------------------------------------------------
// Device loss (browser-WASM).
// ---------------------------------------------------------------------------
// The browser can take the device away at any time. Headless Chromium on
// SwiftShader does it at the FIRST canvas present of every page (the GPU
// process finds no shared-image backing for the WebGPU swapchain when the
// compositor runs on ANGLE-SwiftShader GL; SWFRecompDocs/status/
// browser-webgpu-device-lost.md). A lost device is not an error state for the
// API: every call on it stays valid and does nothing, so the render walk keeps
// running and keeps its side effects. What breaks is anything that WAITS on the
// GPU. emdawnwebgpu's glue chains queue.onSubmittedWorkDone() with no rejection
// handler ("assumed not to reject", library_webgpu.js), so on a lost device
// each registration becomes an unhandled promise rejection and its C callback
// never fires. The frames-in-flight park (render_webgpu_open_pass) then ran to
// its guard on every other frame: ~4.4 s each, 0.4 fps. Once g_device_lost is
// set, the park and the work-done registration both stand down.
//
// The device is NOT re-requested: in that environment a fresh device on a fresh
// adapter is lost again at its own first present, and a rebuild would have to
// recreate every buffer, texture pool, pipeline and bind group.
static volatile int g_device_lost = 0;
static void on_device_lost(const WGPUDevice* device, WGPUDeviceLostReason reason,
                           struct WGPUStringView message, void* u1, void* u2)
{
	(void)device; (void)u1; (void)u2;
	if (g_device_lost) return;
	g_device_lost = 1;
	fprintf(stderr, "WebGPU device lost (reason %d): %.*s -- rendering stops, "
	        "the movie keeps running\n", (int)reason,
	        (int)message.length, message.data ? message.data : "");
	// For harnesses: window.__swfGpu.lost counts losses (see park note below).
	EM_ASM({
		var g = globalThis.__swfGpu || (globalThis.__swfGpu = { lost: 0, stalls: 0 });
		g.lost++;
	});
}
#endif

static void request_device_sync(WebGPURenderContext* ctx,
                                const WGPUDeviceDescriptor* desc)
{
	WGPURequestDeviceCallbackInfo cb_info = {0};
	cb_info.mode = WGPUCallbackMode_AllowSpontaneous;
	cb_info.callback = on_device_ready;
	cb_info.userdata1 = ctx;

	WGPUFuture future = wgpuAdapterRequestDevice(ctx->adapter, desc, cb_info);

#ifdef __EMSCRIPTEN__
	while (ctx->device == NULL)
	{
		emscripten_sleep(10);
	}
#elif defined(OFFSCREEN_RENDER)
	(void)future;
	while (ctx->device == NULL)
		wgpuInstanceProcessEvents(ctx->instance);
#else
	WGPUFutureWaitInfo wait_info = {0};
	wait_info.future = future;
	wgpuInstanceWaitAny(ctx->instance, 1, &wait_info, UINT64_MAX);
#endif
}

// ---------------------------------------------------------------------------
// Helper: create a 1x1 RGBA dummy texture
// ---------------------------------------------------------------------------
static void create_dummy_texture(WebGPURenderContext* ctx)
{
	WGPUTextureDescriptor tex_desc = {0};
	tex_desc.label = WGPU_LABEL("dummy_tex");
	tex_desc.dimension = WGPUTextureDimension_2D;
	tex_desc.size = (WGPUExtent3D){1, 1, 1};
	tex_desc.format = WGPUTextureFormat_RGBA8Unorm;
	tex_desc.mipLevelCount = 1;
	tex_desc.sampleCount = 1;
	tex_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	ctx->dummy_tex = wgpuDeviceCreateTexture(ctx->device, &tex_desc);

	WGPUTextureViewDescriptor view_desc = {0};
	view_desc.dimension = WGPUTextureViewDimension_2DArray;
	view_desc.arrayLayerCount = 1;
	view_desc.mipLevelCount = 1;
	ctx->dummy_tex_view = wgpuTextureCreateView(ctx->dummy_tex, &view_desc);

	// A plain 2D view of the same dummy, for the gradient binding (now texture_2d).
	WGPUTextureViewDescriptor view2d_desc = {0};
	view2d_desc.dimension = WGPUTextureViewDimension_2D;
	view2d_desc.arrayLayerCount = 1;
	view2d_desc.mipLevelCount = 1;
	ctx->dummy_tex_2d_view = wgpuTextureCreateView(ctx->dummy_tex, &view2d_desc);

	WGPUSamplerDescriptor samp_desc = {0};
	samp_desc.label = WGPU_LABEL("dummy_sampler");
	samp_desc.addressModeU = WGPUAddressMode_ClampToEdge;
	samp_desc.addressModeV = WGPUAddressMode_ClampToEdge;
	samp_desc.magFilter = WGPUFilterMode_Nearest;
	samp_desc.minFilter = WGPUFilterMode_Nearest;
	samp_desc.maxAnisotropy = 1;
	ctx->dummy_sampler = wgpuDeviceCreateSampler(ctx->device, &samp_desc);

	// Upload a single white pixel
	u32 white = 0xFFFFFFFF;
	WGPUTexelCopyTextureInfo dest = {0};
	dest.texture = ctx->dummy_tex;
	WGPUTexelCopyBufferLayout layout = {0};
	layout.bytesPerRow = 4;
	layout.rowsPerImage = 1;
	WGPUExtent3D extent = {1, 1, 1};
	wgpuQueueWriteTexture(ctx->queue, &dest, &white, 4, &layout, &extent);
}

// ---------------------------------------------------------------------------
// Forward declarations for init sub-steps
// ---------------------------------------------------------------------------
static void create_buffers_and_upload(WebGPURenderContext* ctx);
static void create_textures(WebGPURenderContext* ctx);
static void create_pipelines(WebGPURenderContext* ctx);
static void create_bind_groups(WebGPURenderContext* ctx);
static void run_compute_pass(WebGPURenderContext* ctx);
static int invert_4x4_matrix(const float m[16], float inv[16]);
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
static int ensure_browser_capture_resources(WebGPURenderContext* ctx);
static void browser_capture_finish(WebGPURenderContext* ctx);
#endif

// ---------------------------------------------------------------------------
// WASM mouse input callbacks (registered in render_webgpu_init)
// ---------------------------------------------------------------------------
#ifdef __EMSCRIPTEN__
static SWFAppContext* g_mouse_app_context = NULL;

// EmscriptenMouseEvent's targetX/Y are CSS-pixel coordinates relative to the
// canvas element. When the canvas is displayed at a different size than its
// internal pixel buffer (e.g. canvas.style.width=100% but canvas.width=320),
// targetX needs to be rescaled to canvas-internal pixel coordinates before
// being converted to twips. Without this scaling, button hit-tests miss
// whenever the canvas is rendered at a non-1:1 CSS:internal ratio.
// canvasX/canvasY in EmscriptenMouseEvent are deprecated and not populated
// (see html5.h note), so compute the scale here via canvas.width / clientWidth.
static void compute_canvas_mouse_pos(int target_x, int target_y, float* out_x, float* out_y) {
	double sx = (double)target_x;
	double sy = (double)target_y;
	EM_ASM({
		var c = document.getElementById('canvas');
		if (c) {
			var rw = c.clientWidth || c.width;
			var rh = c.clientHeight || c.height;
			if (rw > 0) HEAPF64[$0 >> 3] = HEAPF64[$0 >> 3] * c.width / rw;
			if (rh > 0) HEAPF64[$1 >> 3] = HEAPF64[$1 >> 3] * c.height / rh;
		}
	}, &sx, &sy);
	*out_x = (float)sx;
	*out_y = (float)sy;
}

#ifdef SWF_AVM2
// AVM2 live mouse bridge (RWK-3, the 13c key-ring pattern): forward browser
// mouse events into the AVM2 input ring (drained by avm2_input_pump_tick).
// AVM2 ignores app_context->mouse; without this the AVM2 stage never sees
// MouseEvents in the browser, so menus are unreachable. Declared locally so
// this rendering TU stays free of AVM2 headers. See avm2_globals.h.
extern void avm2_input_inject_mouse(int kind, float x, float y, int button,
                                    int click_count);
#endif

static EM_BOOL on_mouse_move(int type, const EmscriptenMouseEvent* evt, void* ud) {
	(void)type; (void)ud;
	float px = 0.0f, py = 0.0f;
	compute_canvas_mouse_pos(evt->targetX, evt->targetY, &px, &py);
	if (g_mouse_app_context) {
		g_mouse_app_context->mouse.stage_x = px * 20.0f;
		g_mouse_app_context->mouse.stage_y = py * 20.0f;
	}
#ifdef SWF_AVM2
	avm2_input_inject_mouse(0, px, py, (int) evt->button, 0);
#endif
	return EM_TRUE;
}

static EM_BOOL on_mouse_down(int type, const EmscriptenMouseEvent* evt, void* ud) {
	(void)type; (void)ud;
	float px = 0.0f, py = 0.0f;
	compute_canvas_mouse_pos(evt->targetX, evt->targetY, &px, &py);
	if (g_mouse_app_context && evt->button == 0) {
		g_mouse_app_context->mouse.button_down = 1;
		g_mouse_app_context->mouse.clicked = 1;
		g_mouse_app_context->mouse.stage_x = px * 20.0f;
		g_mouse_app_context->mouse.stage_y = py * 20.0f;
	}
#ifdef SWF_AVM2
	if (evt->button <= 2) {
		// EmscriptenMouseEvent carries no DOM `detail` (click count), so
		// detect double-clicks here: second left-down within 500 ms and a
		// few pixels of the first. Reset after a double so a triple click
		// reads single-double-single, like the browser's own counter parity.
		static double last_left_down_ms = -1e9;
		static float last_left_x, last_left_y;
		int click_count = 1;
		if (evt->button == 0) {
			if (evt->timestamp - last_left_down_ms < 500.0 &&
			    px - last_left_x < 4.0f && last_left_x - px < 4.0f &&
			    py - last_left_y < 4.0f && last_left_y - py < 4.0f) {
				click_count = 2;
				last_left_down_ms = -1e9;
			} else {
				last_left_down_ms = evt->timestamp;
				last_left_x = px;
				last_left_y = py;
			}
		}
		avm2_input_inject_mouse(1, px, py, (int) evt->button, click_count);
	}
#endif
	return EM_TRUE;
}

static EM_BOOL on_mouse_up(int type, const EmscriptenMouseEvent* evt, void* ud) {
	(void)type; (void)ud;
	float px = 0.0f, py = 0.0f;
	compute_canvas_mouse_pos(evt->targetX, evt->targetY, &px, &py);
	if (g_mouse_app_context && evt->button == 0) {
		g_mouse_app_context->mouse.button_down = 0;
		g_mouse_app_context->mouse.released = 1;
		g_mouse_app_context->mouse.stage_x = px * 20.0f;
		g_mouse_app_context->mouse.stage_y = py * 20.0f;
	}
#ifdef SWF_AVM2
	if (evt->button <= 2)
		avm2_input_inject_mouse(2, px, py, (int) evt->button, 0);
#endif
	return EM_TRUE;
}

#ifdef SWF_AVM2
// AVM2 live-input bridge (Stage 13c): forward browser key events into the AVM2
// KeyboardEvent queue (drained by avm2_input_pump_tick each frame). Declared
// locally so this rendering TU stays free of AVM2 headers; only compiled in the
// AVM2 build, where avm2_display.c defines the symbol. See avm2_globals.h.
extern void avm2_input_inject_key(int is_down, int key_code,
                                  int char_code, int key_location);
#endif

static EM_BOOL on_key_down(int type, const EmscriptenKeyboardEvent* evt, void* ud) {
	(void)type; (void)ud;
	if (g_mouse_app_context) {
		int code = evt->keyCode;
		if (code >= 0 && code < 256) {
			g_mouse_app_context->keys.down[code] = 1;
			// Latch a press-edge so swf.c can dispatch keyPress / keyDown
			// even if a paired keyup arrives in the same 60Hz tick.
			g_mouse_app_context->keys.edge_down[code] = 1;
		}
		g_mouse_app_context->keys.last_key_down = code;
		g_mouse_app_context->keys.last_key_ascii = evt->which;
	}
#ifdef SWF_AVM2
	// AVM2 ignores app_context->keys; feed its own KeyboardEvent queue instead.
	avm2_input_inject_key(1, evt->keyCode, (int) evt->which, (int) evt->location);
#endif
	// When a text field is focused, the browser must still emit the `keypress`
	// event that drives text input — and preventDefault() on keydown cancels it.
	// So for a focused field, only preventDefault the non-character navigation /
	// editing keys (which never produce a keypress anyway, and whose browser
	// defaults — scroll, back-navigation, tab-out — we don't want); let printable
	// keys (incl. space) through so keypress fires. With no field focused, keep
	// the original behavior (preventDefault everything) so game canvases don't
	// scroll the page on arrows/space.
	extern int ng_is_textfield_focused(void);
	if (ng_is_textfield_focused()) {
		int code = evt->keyCode;
		switch (code) {
			case 8:  // Backspace (browser back-nav)
			case 9:  // Tab (focus-out)
			case 33: case 34:            // PageUp / PageDown
			case 35: case 36:            // End / Home
			case 37: case 38: case 39: case 40: // arrows
			case 46:                     // Delete
				return EM_TRUE;          // preventDefault — no keypress lost
			default:
				return EM_FALSE;         // printable keys: let keypress fire
		}
	}
	// Prevent default for arrow keys / space to stop page scrolling
	return EM_TRUE;
}

static EM_BOOL on_key_up(int type, const EmscriptenKeyboardEvent* evt, void* ud) {
	(void)type; (void)ud;
	if (g_mouse_app_context) {
		int code = evt->keyCode;
		if (code >= 0 && code < 256) {
			g_mouse_app_context->keys.down[code] = 0;
			g_mouse_app_context->keys.edge_up[code] = 1;
		}
	}
#ifdef SWF_AVM2
	avm2_input_inject_key(0, evt->keyCode, (int) evt->which, (int) evt->location);
#endif
	return EM_TRUE;
}

// Text input: ring buffer of typed Unicode codepoints (from emscripten
// keypress events, which fire AFTER dead-key composition and IME). Drained
// per-frame by swf.c into actionTextFieldInput.
#define TEXT_INPUT_RING_SIZE 64
int g_text_input_ring[TEXT_INPUT_RING_SIZE];
int g_text_input_ring_head = 0;  // write position (next free)
int g_text_input_ring_tail = 0;  // read position (next to consume)

static EM_BOOL on_keypress(int type, const EmscriptenKeyboardEvent* evt, void* ud) {
	(void)type; (void)ud;
	if (!g_mouse_app_context) return EM_TRUE;
	int cp = evt->charCode ? evt->charCode : evt->which;
	if (cp <= 0) return EM_TRUE;
	int next = (g_text_input_ring_head + 1) % TEXT_INPUT_RING_SIZE;
	if (next == g_text_input_ring_tail) return EM_TRUE;  // ring full — drop
	g_text_input_ring[g_text_input_ring_head] = cp;
	g_text_input_ring_head = next;
	return EM_TRUE;
}

// Window-blur flag — set by on_blur, drained per-frame in swf.c.
int g_window_focus_lost = 0;

static EM_BOOL on_blur(int type, const EmscriptenFocusEvent* evt, void* ud) {
	(void)type; (void)evt; (void)ud;
	g_window_focus_lost = 1;
	return EM_TRUE;
}

// IME (composition) state — emscripten has no native compositionstart/end
// callback, so we register listeners via EM_JS and bridge into these
// globals. swf.c drains them per-frame and dispatches actionTextFieldImeCompose
// / actionTextFieldImeCommit.
//
// During composition: g_ime_compose_text holds the current composition string.
// On compositionupdate: g_ime_compose_pending=1, swf.c calls Compose with text
//   and the caret position (set to end of text).
// On compositionend: g_ime_commit_pending=1, swf.c calls Commit with the
//   final text. Also clears any pending compose state.
#define IME_TEXT_BUF_SIZE 256
char g_ime_compose_text[IME_TEXT_BUF_SIZE];
char g_ime_commit_text[IME_TEXT_BUF_SIZE];
int  g_ime_compose_pending = 0;
int  g_ime_commit_pending  = 0;

EM_JS(void, ng_register_ime_listeners, (), {
	var c = document.getElementById('canvas');
	if (!c) return;
	c.addEventListener('compositionstart', function(e) {
		// Mark composition active — swf.c will start sending Compose calls.
		Module._ng_ime_compose_set('');
	});
	c.addEventListener('compositionupdate', function(e) {
		Module._ng_ime_compose_set(e.data || '');
	});
	c.addEventListener('compositionend', function(e) {
		Module._ng_ime_commit_set(e.data || '');
	});
});

// Called from JS via EM_ASM_INT-style bridging. Simple ASCII-truncation
// for the buffer; non-ASCII codepoints in compose text get UTF-8-encoded
// by the JS engine, which we accept as-is (the runtime decodes UTF-8).
EMSCRIPTEN_KEEPALIVE
void ng_ime_compose_set(const char* text) {
	if (!text) text = "";
	size_t n = 0;
	while (text[n] && n < IME_TEXT_BUF_SIZE - 1) { g_ime_compose_text[n] = text[n]; n++; }
	g_ime_compose_text[n] = '\0';
	g_ime_compose_pending = 1;
}

EMSCRIPTEN_KEEPALIVE
void ng_ime_commit_set(const char* text) {
	if (!text) text = "";
	size_t n = 0;
	while (text[n] && n < IME_TEXT_BUF_SIZE - 1) { g_ime_commit_text[n] = text[n]; n++; }
	g_ime_commit_text[n] = '\0';
	g_ime_commit_pending = 1;
}
#endif

// ---------------------------------------------------------------------------
// render_webgpu_new
// ---------------------------------------------------------------------------
WebGPURenderContext* render_webgpu_new(void)
{
	WebGPURenderContext* ctx = calloc(1, sizeof(WebGPURenderContext));
	// Default background is white when the SWF has no SetBackgroundColor tag
	// (matches Flash Player behavior). tagSetBackgroundColor overrides this
	// during recompiled tag execution.
	ctx->red = 255;
	ctx->green = 255;
	ctx->blue = 255;
	return ctx;
}

// Test knob: clamp the texture-array layer limit (see the device request in
// render_webgpu_init). The env var wins over a compiled-in -D default.
static u32 gpu_layer_knob(void)
{
	const char* e = getenv("SWF_GPU_MAX_TEXTURE_ARRAY_LAYERS");
	if (e != NULL && *e != '\0') return (u32)strtoul(e, NULL, 10);
#ifdef SWF_GPU_MAX_TEXTURE_ARRAY_LAYERS
	return (u32)(SWF_GPU_MAX_TEXTURE_ARRAY_LAYERS);
#else
	return 0;
#endif
}

// ---------------------------------------------------------------------------
// render_webgpu_init
// ---------------------------------------------------------------------------
void render_webgpu_init(SWFAppContext* app_context, WebGPURenderContext* ctx)
{
	RENDER_INIT_LOG("[render] init: start (%dx%d, %u static bitmaps)", ctx->width, ctx->height,
	                (unsigned)ctx->bitmap_count);
	ctx->current_bitmap = 0;
	ctx->bitmap_static_built = 0;
	// 8 u32 per static slot: {content_w, content_h, padded_w, padded_h}, {pool, layer, 0, 0}.
	ctx->bitmap_sizes = (u32*)HALLOC(8 * sizeof(u32) * ctx->bitmap_count);
	ctx->bitmap_ptrs = (const u8**)HALLOC(sizeof(const u8*) * ctx->bitmap_count);
	// Callers (swf.c::swfStart, avm2_display.c) may pre-set the dynamic source
	// limit before renderer_init; only apply the default floor when unset.
	if (ctx->dynamic_bitmap_max_w == 0) ctx->dynamic_bitmap_max_w = 256;
	if (ctx->dynamic_bitmap_max_h == 0) ctx->dynamic_bitmap_max_h = 256;

	// --- Create WebGPU instance ---
	WGPUInstanceDescriptor inst_desc = {0};
	ctx->instance = wgpuCreateInstance(&inst_desc);
	assert(ctx->instance != NULL);

	// --- Create surface ---
#ifdef OFFSCREEN_RENDER
	// Headless mode: no surface, no window
	ctx->surface = NULL;
#elif defined(__EMSCRIPTEN__)
	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_src = {0};
	canvas_src.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
	canvas_src.selector = WGPU_LABEL("#canvas");

	WGPUSurfaceDescriptor surf_desc = {0};
	surf_desc.nextInChain = (const WGPUChainedStruct*)&canvas_src;
	ctx->surface = wgpuInstanceCreateSurface(ctx->instance, &surf_desc);
#else
	// Native: create SDL window and use sdl3webgpu bridge
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		fprintf(stderr, "Failed to init SDL: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}
	ctx->window = SDL_CreateWindow("TestSWFRecompiled", ctx->width, ctx->height,
	                               SDL_WINDOW_RESIZABLE);
	assert(ctx->window != NULL);

	ctx->surface = SDL_GetWGPUSurface(ctx->instance, ctx->window);
#endif
#ifndef OFFSCREEN_RENDER
	assert(ctx->surface != NULL);
#endif

	// --- Request adapter ---
	{
		WGPURequestAdapterOptions opts = {0};
#ifdef OFFSCREEN_RENDER
		opts.compatibleSurface = NULL;  // No surface in headless mode
#else
		opts.compatibleSurface = ctx->surface;
#endif
		opts.powerPreference = WGPUPowerPreference_HighPerformance;

		request_adapter_sync(ctx, &opts);
		assert(ctx->adapter != NULL);
		RENDER_INIT_LOG("[render] init: adapter %s", ctx->adapter ? "ok" : "NULL");
	}

	// --- Request device ---
	{
		WGPUDeviceDescriptor dev_desc = {0};
		dev_desc.label = WGPU_LABEL("swf_device");
		dev_desc.defaultQueue.label = WGPU_LABEL("swf_queue");
		dev_desc.uncapturedErrorCallbackInfo.callback = on_uncaptured_error;
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
		dev_desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
		dev_desc.deviceLostCallbackInfo.callback = on_device_lost;
#endif

		// Request the adapter's full limits rather than the WebGPU defaults.
		// The default maxTextureArrayLayers is 256, but the gradient (and bitmap)
		// texture arrays use one layer per distinct gradient/bitmap — content-heavy
		// SWFs (e.g. Meteor Storm: 519 gradients) blow past 256, which makes the
		// array-texture creation fail. An invalid texture poisons the bind group
		// and silently drops EVERY command buffer that references it (including the
		// clear + MSAA resolve), so the offscreen target reads back as fully
		// transparent black. Requesting the adapter maximum (often 2048 layers,
		// larger buffer sizes) lets these games render. Limits queried from the
		// adapter are by definition grantable.
		//
		// Test knob SWF_GPU_MAX_TEXTURE_ARRAY_LAYERS (env var, or a -D of the same
		// name; off by default): request at most that many layers, and have the
		// pool code plan for at most that many. lavapipe (every native graphics
		// run) grants 2048, so without the knob a native run never sees the
		// 256-layer wall SwiftShader and the WebGPU defaults impose; with it set
		// to 256 the device really enforces 256 (WebGPU never grants a device
		// less than the default, so smaller values only clamp the planner).
		u32 layer_knob = gpu_layer_knob();
		WGPULimits adapter_limits = WGPU_LIMITS_INIT;
		WGPULimits required_limits = WGPU_LIMITS_INIT;
		if (wgpuAdapterGetLimits(ctx->adapter, &adapter_limits) == WGPUStatus_Success) {
			required_limits = adapter_limits;
			required_limits.nextInChain = NULL;
			if (layer_knob && required_limits.maxTextureArrayLayers > layer_knob)
				required_limits.maxTextureArrayLayers = layer_knob < 256 ? 256 : layer_knob;
			dev_desc.requiredLimits = &required_limits;
		}

		request_device_sync(ctx, &dev_desc);
		assert(ctx->device != NULL);
		RENDER_INIT_LOG("[render] init: device %s", ctx->device ? "ok" : "NULL");

		// What the device actually grants is what every texture array must fit.
		// Fall back to the WebGPU defaults, which every device guarantees.
		ctx->max_texture_array_layers = 256;
		ctx->max_texture_dimension_2d = 8192;
		WGPULimits device_limits = WGPU_LIMITS_INIT;
		if (wgpuDeviceGetLimits(ctx->device, &device_limits) == WGPUStatus_Success) {
			ctx->max_texture_array_layers = device_limits.maxTextureArrayLayers;
			ctx->max_texture_dimension_2d = device_limits.maxTextureDimension2D;
		}
		if (layer_knob && ctx->max_texture_array_layers > layer_knob)
			ctx->max_texture_array_layers = layer_knob;
		bitmap_pool_log("[render] device limits: maxTextureArrayLayers %u%s, "
		                "maxTextureDimension2D %u", ctx->max_texture_array_layers,
		                layer_knob ? " (SWF_GPU_MAX_TEXTURE_ARRAY_LAYERS)" : "",
		                ctx->max_texture_dimension_2d);
	}

	// --- Configure surface (or create offscreen texture for headless) ---
	ctx->surface_format = WGPUTextureFormat_BGRA8Unorm;

#ifdef OFFSCREEN_RENDER
	// Create offscreen render target texture (used as MSAA resolve target)
	{
		WGPUTextureDescriptor offscreen_desc = {0};
		offscreen_desc.label = WGPU_LABEL("offscreen_target");
		offscreen_desc.dimension = WGPUTextureDimension_2D;
		offscreen_desc.size = (WGPUExtent3D){ctx->width, ctx->height, 1};
		offscreen_desc.format = ctx->surface_format;
		offscreen_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
		offscreen_desc.mipLevelCount = 1;
		offscreen_desc.sampleCount = 1;
		ctx->offscreen_texture = wgpuDeviceCreateTexture(ctx->device, &offscreen_desc);
		assert(ctx->offscreen_texture != NULL);
		ctx->offscreen_view = wgpuTextureCreateView(ctx->offscreen_texture, NULL);

		// Create staging buffer for readback
		size_t row_bytes = (size_t)ctx->width * 4;
		// WebGPU requires bytesPerRow aligned to 256
		size_t aligned_row = (row_bytes + 255) & ~(size_t)255;
		ctx->readback_row_stride = aligned_row;
		WGPUBufferDescriptor buf_desc = {0};
		buf_desc.label = WGPU_LABEL("readback_buffer");
		buf_desc.size = aligned_row * ctx->height;
		buf_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
		buf_desc.mappedAtCreation = false;
		ctx->readback_buffer = wgpuDeviceCreateBuffer(ctx->device, &buf_desc);
		assert(ctx->readback_buffer != NULL);
	}
#else
	WGPUSurfaceConfiguration surf_config = {0};
	surf_config.device = ctx->device;
	surf_config.format = ctx->surface_format;
	// CopySrc: complex blend modes snapshot the resolved frame as their backdrop.
	surf_config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
	surf_config.alphaMode = WGPUCompositeAlphaMode_Auto;
	surf_config.width = ctx->width;
	surf_config.height = ctx->height;
	surf_config.presentMode = WGPUPresentMode_Fifo;
	wgpuSurfaceConfigure(ctx->surface, &surf_config);
	RENDER_INIT_LOG("[render] init: surface configured %dx%d", ctx->width, ctx->height);
#endif

	// --- Create all GPU resources ---
	create_dummy_texture(ctx);
	RENDER_INIT_LOG("[render] init: %s done", "create_dummy_texture");
	create_buffers_and_upload(ctx);
	RENDER_INIT_LOG("[render] init: %s done", "create_buffers_and_upload");
	create_textures(ctx);
	RENDER_INIT_LOG("[render] init: %s done", "create_textures");
	create_pipelines(ctx);
	RENDER_INIT_LOG("[render] init: %s done", "create_pipelines");
	create_bind_groups(ctx);
	RENDER_INIT_LOG("[render] init: %s done", "create_bind_groups");

	// --- Run compute pass to invert gradient matrices ---
	size_t num_gradients = ctx->uninv_mat_data_size / (16 * sizeof(float));
	if (num_gradients > 0)
	{
		run_compute_pass(ctx);
		RENDER_INIT_LOG("[render] init: compute pass done");
		// Recreate bind groups after compute (inv_mat_buffer now has data)
		// Actually the buffer is the same, just the contents changed, so
		// the bind group remains valid.
	}

	// --- Register mouse input callbacks (WASM only, not headless) ---
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	g_mouse_app_context = app_context;
	emscripten_set_mousemove_callback("#canvas", NULL, 0, on_mouse_move);
	emscripten_set_mousedown_callback("#canvas", NULL, 0, on_mouse_down);
	emscripten_set_mouseup_callback("#canvas", NULL, 0, on_mouse_up);
	// Make canvas focusable, auto-focus, and re-focus on click
	EM_ASM({
		var c = document.getElementById('canvas');
		if (c) {
			c.setAttribute('tabindex', '0');
			c.style.outline = 'none';
			c.focus();
			c.addEventListener('mousedown', function() { c.focus(); });
		}
	});
	emscripten_set_keydown_callback("#canvas", NULL, 0, on_key_down);
	emscripten_set_keyup_callback("#canvas", NULL, 0, on_key_up);
	emscripten_set_keypress_callback("#canvas", NULL, 0, on_keypress);
	emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0, on_blur);
	ng_register_ime_listeners();
#endif

	// Mark renderer as ready only if critical objects were created
	if (ctx->device && ctx->render_pipeline && ctx->queue)
		ctx->renderer_ok = 1;
	else
		fprintf(stderr, "Warning: WebGPU renderer not fully initialized, rendering disabled\n");
}

// ---------------------------------------------------------------------------
// create_buffers_and_upload: create GPU buffers and upload static data
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Bitmap texture pools
// ---------------------------------------------------------------------------
// Until 2026-09-02 every bitmap of the movie (plus a dynamic reserve) lived in
// ONE texture_2d_array whose layer was (widest+1) x (tallest+1) over ALL of
// them — two unrelated bitmaps set the envelope. The original Seedling has a
// 4480x82 strip and a 486x1106 image among 284 bitmaps holding 13 MiB of
// pixels; the array came to 4481x1107x284x4 B = 5.25 GiB, D3D12 refused it and
// the canvas stayed black (in-browser-recompiler-refresh-assessment.md §8).
//
// Now there are BITMAP_POOL_COUNT arrays, bound separately (fragment bindings
// 4..15) and selected per fragment by the WGSL sample_bitmap():
//   * static pools 0..7 — size classes by max(w+1, h+1) <= 64, 128, 256, 512,
//     1024, 2048, 4096, and everything larger. Each pool's layer is the
//     bounding box of ITS members, so Seedling's two outliers cost two
//     4481x1107 layers (40 MB) instead of 284 of them. Built in
//     render_webgpu_finalize_bitmaps, once every static size is known (the
//     recompiler emits all defineBitmap calls, then finalizeBitmaps, in
//     tag_init — before the first frame).
//   * dynamic pools 8..11 — nominal squares of 256, 512, 1024 (each clipped to
//     dynamic_bitmap_max) and the full dynamic_bitmap_max box. A dynamic draw
//     takes the first pool its source fits. A pool is created on first use
//     and DOUBLES its layer count when a frame exhausts it (up to
//     dynamic_pool_cap), so nothing is allocated for sizes a movie never
//     draws — an AVM2 movie raises dynamic_bitmap_max to its largest embedded
//     bitmap (avm2_display.c), which used to size every dynamic layer.
// Layer contents are unchanged: a bitmap sits at the layer origin with the
// same +1 edge-clamp row/column, and the rest of the layer is transparent
// (never written; WebGPU zero-initialises), so a clipped static fill reading
// past its bitmap still reads transparent. Dynamic layers ARE rewritten every
// frame, so the one dynamic path with arbitrary geometry (beginBitmapFill,
// clipped) clamps its UVs to the content in the shader (style bit 11).
//
// Slots: vertex data still names a bitmap SLOT — the recompiler's static
// bitmap index, or dynamic_bitmap_base + a per-frame counter — and the
// bitmap_sizes table maps a slot to {content, padded} and {pool, layer}.

// Per-frame dynamic slots (table + inverse-matrix entries), shared by the
// four dynamic pools (each capped at MAX_DYNAMIC_BITMAPS_GROWN layers).
#define MAX_DYNAMIC_BITMAP_SLOTS 512
// Dynamic pool growth. 64 is the floor every layer size keeps (a frame
// issuing <= 64 dynamic draws of one class never truncates, as before); the
// ceiling is reached when layers are cheap against DYNAMIC_BITMAP_VRAM_BUDGET.
#define MAX_DYNAMIC_BITMAPS 64
#define MAX_DYNAMIC_BITMAPS_GROWN 128
#define DYNAMIC_POOL_INITIAL_LAYERS 16
#define DYNAMIC_BITMAP_VRAM_BUDGET ((size_t)384 * 1024 * 1024)
// A single texture allocation must also fit the DRIVER's
// maxMemoryAllocationSize: Mesa lavapipe (CI and every local --mode=graphics
// run, forced by verify_output.py's VK_ICD_FILENAMES) reports exactly 2 GiB.
// Measured on from_shumway/acid/acid-large (3613x2681 layers, 36.95 MB each):
// 54 layers allocate, 55 return VK_ERROR_OUT_OF_DEVICE_MEMORY, after which Dawn
// drops every command buffer that binds the texture and the run reads back
// empty. 1.5 GiB keeps 25% headroom. It is the one clamp that may take a
// dynamic pool below the 64-layer floor: under-allocating costs dropped
// dynamic draws, breaching the allocator costs the entire frame.
#define BITMAP_ARRAY_HARD_LIMIT ((size_t)1536 * 1024 * 1024)

#define BITMAP_POOL_BINDING_BASE 4

static void rebuild_fragment_sampler_bg(WebGPURenderContext* ctx);

// Pool allocations are the VRAM number that matters for a movie: log them to
// the browser console (emscripten_log goes straight to console.log, never
// through Module.print, so browser-side trace capture is untouched) and,
// natively, to stderr when SWF_WARN_BITMAP_CAP is set (the same gate as the
// capacity notice below).
static void bitmap_pool_log(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	emscripten_log(EM_LOG_CONSOLE, "%s", buf);
#else
	if (getenv("SWF_WARN_BITMAP_CAP") != NULL) fprintf(stderr, "%s\n", buf);
#endif
}

// One legible report for a renderer texture that cannot be made within the
// device's limits, or whose creation failed. An over-limit texture is not a
// crash: it is an invalid handle, which invalidates the bind group holding it,
// which drops every command buffer that binds that group. The movie keeps
// ticking and the canvas stays black, and the only symptom used to be a
// validation error per frame (4,458 in 30 s on a pre-pool Seedling under
// SwiftShader). Printed ONCE (browser: console.error, always; native: stderr
// under SWF_WARN_BITMAP_CAP); every later report only counts.
// window.__swfGpu.texFail counts reports, .texMsg holds the first.
static u32 g_texture_problems = 0;
static void render_texture_problem(const char* fmt, ...)
{
	char buf[384];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_texture_problems++;
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	if (g_texture_problems == 1) emscripten_log(EM_LOG_CONSOLE | EM_LOG_ERROR, "%s", buf);
	EM_ASM({
		var g = globalThis.__swfGpu || (globalThis.__swfGpu = { lost: 0, stalls: 0 });
		g.texFail = (g.texFail | 0) + 1;
		if (!g.texMsg) g.texMsg = UTF8ToString($0);
	}, buf);
#else
	if (g_texture_problems == 1 && getenv("SWF_WARN_BITMAP_CAP") != NULL)
		fprintf(stderr, "%s\n", buf);
#endif
}

// Creation failures the planning above cannot foresee (out of memory, a limit
// this file does not check) are caught by an error scope around each texture
// array's creation and reported through render_texture_problem instead of the
// uncaptured-error stream. The label is a string literal, so it outlives the
// (possibly asynchronous) callback.
static void on_texture_scope_popped(WGPUPopErrorScopeStatus status, WGPUErrorType type,
                                    struct WGPUStringView message, void* u1, void* u2)
{
	(void)u2;
	// Only the two types these scopes filter for. A pop that REJECTS (the
	// device was lost while it was pending: "Instance dropped in
	// popErrorScope") comes back from emdawnwebgpu as Success + Unknown, and is
	// not a creation failure; the device-lost path reports that.
	if (status != WGPUPopErrorScopeStatus_Success) return;
	if (type != WGPUErrorType_Validation && type != WGPUErrorType_OutOfMemory) return;
	render_texture_problem("[render] GPU texture \"%s\" could not be created, so nothing "
	                       "that samples it will draw: %.*s", (const char*)u1,
	                       (int)(message.length > 240 ? 240 : message.length),
	                       message.data ? message.data : "");
}

static void texture_scope_push(WebGPURenderContext* ctx)
{
	wgpuDevicePushErrorScope(ctx->device, WGPUErrorFilter_OutOfMemory);
	wgpuDevicePushErrorScope(ctx->device, WGPUErrorFilter_Validation);
}

static void texture_scope_pop(WebGPURenderContext* ctx, const char* label)
{
	WGPUPopErrorScopeCallbackInfo cb = {0};
	cb.mode = WGPUCallbackMode_AllowSpontaneous;
	cb.callback = on_texture_scope_popped;
	cb.userdata1 = (void*)label;
	wgpuDevicePopErrorScope(ctx->device, cb);  // Validation
	wgpuDevicePopErrorScope(ctx->device, cb);  // OutOfMemory
}

// Static pool membership (2026-09-11, bitmap-pool-layer-cap slice). A static
// pool used to take one layer per member of its size class with no cap, so a
// movie with more than maxTextureArrayLayers bitmaps in one class (256 on
// SwiftShader and under the WebGPU default limits; 2048+ on real GPUs and
// lavapipe) created an invalid texture and rendered black. Membership is now
// planned against the DEVICE's limits, read back after device creation:
//   * a pool holds at most max_texture_array_layers members, and at most
//     BITMAP_ARRAY_HARD_LIMIT bytes (lavapipe's 2 GiB allocation ceiling);
//   * a class that would exceed either keeps its SMALLEST members and spills
//     the rest into the next pool up, whose layer box grows to take them if it
//     has to (the spilled bitmaps are the largest of their class, and the next
//     class is larger anyway, so the box rarely moves);
//   * a bitmap wider or taller than maxTextureDimension2D - 1 fits no layer,
//     and neither does whatever the last pool cannot hold: those slots name
//     BITMAP_POOL_NONE, which sample_bitmap() reads as transparent, and
//     render_texture_problem reports them once.
// Spilling needs no new binding and no shader change, and costs VRAM only
// where a class actually overflows: the spilled layers take the larger
// pool's box. Under the limits every class keeps exactly its own members, so
// the layout (and every pixel) is the plain size-class one.
#define BITMAP_POOL_NONE 0xFFu

// qsort order for a class's candidates: smallest first (max side, then area,
// then slot, so the plan is deterministic). qsort takes no context pointer.
static const u32* g_bitmap_sort_sizes;
static int bitmap_size_cmp(const void* a, const void* b)
{
	u32 i = *(const u32*)a, j = *(const u32*)b;
	const u32* sz = g_bitmap_sort_sizes;
	u32 wi = sz[8 * i], hi = sz[8 * i + 1], wj = sz[8 * j], hj = sz[8 * j + 1];
	u32 mi = wi > hi ? wi : hi, mj = wj > hj ? wj : hj;
	if (mi != mj) return mi < mj ? -1 : 1;
	unsigned long long ai = (unsigned long long)wi * hi, aj = (unsigned long long)wj * hj;
	if (ai != aj) return ai < aj ? -1 : 1;
	return i < j ? -1 : (i > j ? 1 : 0);
}

// Static size class of a (w x h) bitmap: smallest k with max(w,h)+1 <= 64<<k.
static u32 bitmap_static_class(u32 w, u32 h)
{
	u32 m = (w > h ? w : h) + 1;
	u32 t = 64;
	for (u32 k = 0; k + 1 < BITMAP_STATIC_POOLS; k++, t <<= 1)
		if (m <= t) return k;
	return BITMAP_STATIC_POOLS - 1;
}

// Layer size of dynamic pool d (0-based among the dynamic pools).
static void dynamic_pool_dims(WebGPURenderContext* ctx, u32 d, u32* pw, u32* ph)
{
	static const u32 nominal[BITMAP_DYNAMIC_POOLS - 1] = { 256, 512, 1024 };
	u32 mw = ctx->dynamic_bitmap_max_w, mh = ctx->dynamic_bitmap_max_h;
	if (d + 1 < BITMAP_DYNAMIC_POOLS)
	{
		if (mw > nominal[d]) mw = nominal[d];
		if (mh > nominal[d]) mh = nominal[d];
	}
	// The layer must fit maxTextureDimension2D. dynamic_bitmap_max is raised
	// by callers to the movie's largest embedded bitmap (avm2_display.c) with
	// no GPU cap; a larger source now fits no pool and its draw is dropped
	// (dynamic_bitmap_acquire), instead of making the whole bind group invalid.
	u32 dmax = ctx->max_texture_dimension_2d ? ctx->max_texture_dimension_2d - 1 : 8191;
	if (mw > dmax) mw = dmax;
	if (mh > dmax) mh = dmax;
	*pw = mw + 1;
	*ph = mh + 1;
}

static u32 dynamic_pool_cap(WebGPURenderContext* ctx, u32 pw, u32 ph)
{
	size_t layer_bytes = (size_t)pw * (size_t)ph * 4;
	size_t cap = DYNAMIC_BITMAP_VRAM_BUDGET / layer_bytes;
	if (cap > MAX_DYNAMIC_BITMAPS_GROWN) cap = MAX_DYNAMIC_BITMAPS_GROWN;
	if (cap < MAX_DYNAMIC_BITMAPS) cap = MAX_DYNAMIC_BITMAPS;
	size_t hard = BITMAP_ARRAY_HARD_LIMIT / layer_bytes;
	if (cap > hard) cap = hard;
	// The device's layer limit. MAX_DYNAMIC_BITMAPS_GROWN (128) is under the
	// 256 every device guarantees, so this only bites under the test knob.
	if (cap > ctx->max_texture_array_layers) cap = ctx->max_texture_array_layers;
	if (cap < 1) cap = 1;
	return (u32)cap;
}

// (Re)allocate pool `pi` as a (w x h x layers) RGBA8 array. A previous texture
// is only released: draws already recorded against it this frame hold it
// through their bind group until the queue has executed them.
static void bitmap_pool_alloc(WebGPURenderContext* ctx, u32 pi, u32 w, u32 h,
                              u32 layers, const char* label)
{
	BitmapPool* p = &ctx->bitmap_pools[pi];
	if (p->view) wgpuTextureViewRelease(p->view);
	if (p->tex) wgpuTextureRelease(p->tex);

	WGPUTextureDescriptor tex_desc = {0};
	tex_desc.label = WGPU_LABEL(label);
	tex_desc.dimension = WGPUTextureDimension_2D;
	tex_desc.size = (WGPUExtent3D){w, h, layers};
	tex_desc.format = WGPUTextureFormat_RGBA8Unorm;
	tex_desc.mipLevelCount = 1;
	tex_desc.sampleCount = 1;
	tex_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	texture_scope_push(ctx);
	p->tex = wgpuDeviceCreateTexture(ctx->device, &tex_desc);
	texture_scope_pop(ctx, label);

	WGPUTextureViewDescriptor view_desc = {0};
	view_desc.dimension = WGPUTextureViewDimension_2DArray;
	view_desc.arrayLayerCount = layers;
	view_desc.mipLevelCount = 1;
	p->view = wgpuTextureCreateView(p->tex, &view_desc);

	p->w = w;
	p->h = h;
	p->layers = layers;
	bitmap_pool_log("[render] bitmap pool %u (%s): %ux%u x %u layers = %.1f MiB",
	                pi, label, w, h, layers,
	                (double)w * (double)h * 4.0 * (double)layers / (1024.0 * 1024.0));
}

// GPU slot-table entry for `slot`: {content, padded}, {pool, layer}.
static void write_bitmap_slot(WebGPURenderContext* ctx, u32 slot, u32 cw, u32 ch,
                              u32 pi, u32 layer)
{
	BitmapPool* p = &ctx->bitmap_pools[pi];
	u32 e[8] = { cw, ch, p->w, p->h, pi, layer, 0, 0 };
	wgpuQueueWriteBuffer(ctx->queue, ctx->bitmap_sizes_buffer,
		(uint64_t)slot * sizeof(e), e, sizeof(e));
}

// One-shot notice that a frame asked for more dynamic bitmap layers than a
// pool may grow to (or more slots than the table has). Both dynamic call
// sites drop the draw SILENTLY, which reads downstream as missing ink rather
// than as a capacity failure — 36 of 100 tiles rendered blank in
// visual/filters/blur_* for a whole session before anyone traced it back here
// (w1 gfx-blur report S2.3). Off unless SWF_WARN_BITMAP_CAP is set, since
// tests capture these streams.
static void dynamic_bitmap_capacity_hit(WebGPURenderContext* ctx, u32 pi)
{
	static int warned = 0;
	if (warned || getenv("SWF_WARN_BITMAP_CAP") == NULL) return;
	warned = 1;
	if (pi < BITMAP_POOL_COUNT)
	{
		BitmapPool* p = &ctx->bitmap_pools[pi];
		fprintf(stderr, "[render] dynamic bitmap pool %u exhausted at its ceiling "
		        "(%u layers of %ux%u); further bitmap draws this frame are dropped\n",
		        pi, p->layers, p->w, p->h);
	}
	else
		fprintf(stderr, "[render] dynamic bitmap slots exhausted (%u); further "
		        "bitmap draws this frame are dropped\n", ctx->dynamic_bitmap_capacity);
}

// Take a per-frame dynamic slot plus a layer in the first dynamic pool that
// fits (src_w x src_h), creating or growing that pool when it is full. Returns
// 0 — the draw must be dropped — when the slot table or the pool's ceiling is
// exhausted. Callers guard src <= dynamic_bitmap_max, so a pool always fits.
static int dynamic_bitmap_acquire(WebGPURenderContext* ctx, u32 src_w, u32 src_h,
                                  u32* out_slot, u32* out_pool, u32* out_layer)
{
	if (ctx->dynamic_bitmap_used >= ctx->dynamic_bitmap_capacity)
	{
		dynamic_bitmap_capacity_hit(ctx, BITMAP_POOL_COUNT);
		return 0;
	}
	u32 d, pw = 0, ph = 0;
	for (d = 0; d < BITMAP_DYNAMIC_POOLS; d++)
	{
		dynamic_pool_dims(ctx, d, &pw, &ph);
		if (src_w + 1 <= pw && src_h + 1 <= ph) break;
	}
	if (d == BITMAP_DYNAMIC_POOLS)
	{
		// Only reachable when the source exceeds maxTextureDimension2D - 1.
		static int reported = 0;
		if (!reported)
		{
			reported = 1;
			render_texture_problem("[render] a %ux%u dynamic bitmap is larger than the GPU's "
			                       "%u px texture limit and will not draw", src_w, src_h,
			                       ctx->max_texture_dimension_2d);
		}
		return 0;
	}
	u32 pi = BITMAP_STATIC_POOLS + d;
	BitmapPool* p = &ctx->bitmap_pools[pi];
	if (p->used >= p->layers)
	{
		if (p->w == 0) p->cap = dynamic_pool_cap(ctx, pw, ph);
		if (p->layers >= p->cap)
		{
			dynamic_bitmap_capacity_hit(ctx, pi);
			return 0;
		}
		u32 want = p->layers ? p->layers * 2 : DYNAMIC_POOL_INITIAL_LAYERS;
		if (want > p->cap) want = p->cap;
		// Layers used earlier this frame stay in the old texture (their draws
		// were recorded against the old bind group); `used` keeps counting so
		// no layer index repeats within the frame.
		bitmap_pool_alloc(ctx, pi, pw, ph, want, "bitmap_dynamic_pool");
		rebuild_fragment_sampler_bg(ctx);
	}
	*out_layer = p->used++;
	*out_pool = pi;
	*out_slot = ctx->dynamic_bitmap_base + ctx->dynamic_bitmap_used++;
	return 1;
}

// Group static bitmaps into size-class pools, allocate them, upload every
// bitmap to its layer and publish the slot table. Called from
// render_webgpu_finalize_bitmaps (and defensively from open_pass).
static void build_static_bitmap_pools(WebGPURenderContext* ctx)
{
	if (ctx->bitmap_static_built) return;
	ctx->bitmap_static_built = 1;
	// The root's uploads occupy [0, current_bitmap); a loaded child's slots were
	// pre-declared above that (see render_webgpu_predeclare_bitmap). Sizing off
	// current_bitmap alone silently dropped every child bitmap. NOT
	// ctx->bitmap_count: a movie can declare more slots than it uploads (an AS3
	// SWF uploads none at all), and giving those a pool layer each is what the
	// size-class pools exist to avoid.
	u32 n = (u32)ctx->current_bitmap;
	if (ctx->bitmap_predeclared_end > n) n = ctx->bitmap_predeclared_end;
	if (n == 0 || ctx->bitmap_data == NULL) return;

	// Membership, planned against the DEVICE's limits (see "Static pool
	// membership" above): class k's candidates are its own members plus what
	// class k-1 could not hold, smallest first; the pool keeps the longest
	// prefix that fits, the rest carry up. Under the limits every class keeps
	// exactly its own members, i.e. the layout is the plain size-class one.
	u32 L = ctx->max_texture_array_layers, D = ctx->max_texture_dimension_2d;
	u32* pool_of = (u32*)malloc(n * sizeof(u32));
	u32* cand = (u32*)malloc(n * sizeof(u32));
	u32 pw[BITMAP_STATIC_POOLS] = {0}, ph[BITMAP_STATIC_POOLS] = {0};
	u32 cnt[BITMAP_STATIC_POOLS] = {0};
	u32 ncand = 0, too_big = 0, dropped = 0, spilled = 0;
	for (u32 i = 0; i < n; i++)
	{
		pool_of[i] = BITMAP_POOL_NONE;
		u32 w = ctx->bitmap_sizes[8 * i], h = ctx->bitmap_sizes[8 * i + 1];
		if (w + 1 > D || h + 1 > D) too_big++;
	}
	g_bitmap_sort_sizes = ctx->bitmap_sizes;
	for (u32 k = 0; k < BITMAP_STATIC_POOLS; k++)
	{
		for (u32 i = 0; i < n; i++)
		{
			u32 w = ctx->bitmap_sizes[8 * i], h = ctx->bitmap_sizes[8 * i + 1];
			if (w + 1 <= D && h + 1 <= D && bitmap_static_class(w, h) == k)
				cand[ncand++] = i;
		}
		if (ncand == 0) continue;
		qsort(cand, ncand, sizeof(u32), bitmap_size_cmp);
		u32 bw = 0, bh = 0, m = 0;
		for (; m < ncand; m++)
		{
			u32 i = cand[m];
			u32 nw = ctx->bitmap_sizes[8 * i] + 1, nh = ctx->bitmap_sizes[8 * i + 1] + 1;
			if (nw < bw) nw = bw;
			if (nh < bh) nh = bh;
			if (m + 1 > L || (size_t)nw * nh * 4 * (m + 1) > BITMAP_ARRAY_HARD_LIMIT) break;
			bw = nw;
			bh = nh;
			pool_of[i] = k;
			if (bitmap_static_class(ctx->bitmap_sizes[8 * i], ctx->bitmap_sizes[8 * i + 1]) != k)
				spilled++;
		}
		pw[k] = bw;
		ph[k] = bh;
		cnt[k] = m;
		memmove(cand, cand + m, (ncand - m) * sizeof(u32));
		ncand -= m;
	}
	dropped = ncand + too_big;
	free(cand);

	double total_mib = 0.0;
	for (u32 k = 0; k < BITMAP_STATIC_POOLS; k++)
	{
		if (cnt[k] == 0) continue;
		bitmap_pool_alloc(ctx, k, pw[k], ph[k], cnt[k], "bitmap_static_pool");
		total_mib += (double)pw[k] * (double)ph[k] * 4.0 * cnt[k] / (1024.0 * 1024.0);
	}

	for (u32 i = 0; i < n; i++)
	{
		u32 width = ctx->bitmap_sizes[8 * i], height = ctx->bitmap_sizes[8 * i + 1];
		u32 k = pool_of[i];
		if (k == BITMAP_POOL_NONE)
		{
			// No pool can hold it: the slot names no pool, which the fragment
			// stage reads as transparent (sample_bitmap's default arm).
			ctx->bitmap_sizes[8 * i + 2] = width + 1;
			ctx->bitmap_sizes[8 * i + 3] = height + 1;
			ctx->bitmap_sizes[8 * i + 4] = BITMAP_POOL_NONE;
			ctx->bitmap_sizes[8 * i + 5] = 0;
			continue;
		}
		BitmapPool* p = &ctx->bitmap_pools[k];
		u32 layer = p->used++;
		if (width == 0 || height == 0 || ctx->bitmap_ptrs[i] == NULL)
		{
			// Degenerate bitmap (or one whose defining movie emitted no payload):
			// keep the slot (pointing at an all-transparent layer), upload nothing.
			ctx->bitmap_sizes[8 * i + 2] = p->w;
			ctx->bitmap_sizes[8 * i + 3] = p->h;
			ctx->bitmap_sizes[8 * i + 4] = k;
			ctx->bitmap_sizes[8 * i + 5] = layer;
			continue;
		}

		// (width+1) x (height+1) region at the layer origin, edge-clamped by one
		// row/column (matching flashbang_upload_bitmap); the rest of the layer is
		// never written and stays transparent.
		u32 up_w = width + 1, up_h = height + 1;
		size_t up_bytes = (size_t)up_w * up_h * 4;
		u32* temp = (u32*)malloc(up_bytes);
		const u32* src = (const u32*)ctx->bitmap_ptrs[i];
		for (u32 y = 0; y < up_h; y++)
		{
			u32 sy = (y < height) ? y : height - 1;
			for (u32 x = 0; x < up_w; x++)
			{
				u32 sx = (x < width) ? x : width - 1;
				temp[y * up_w + x] = src[sy * width + sx];
			}
		}
		WGPUTexelCopyTextureInfo dest = {0};
		dest.texture = p->tex;
		dest.origin = (WGPUOrigin3D){0, 0, layer};
		WGPUTexelCopyBufferLayout layout = {0};
		layout.bytesPerRow = up_w * 4;
		layout.rowsPerImage = up_h;
		WGPUExtent3D extent = {up_w, up_h, 1};
		wgpuQueueWriteTexture(ctx->queue, &dest, temp, up_bytes, &layout, &extent);
		free(temp);

		ctx->bitmap_sizes[8 * i + 2] = p->w;
		ctx->bitmap_sizes[8 * i + 3] = p->h;
		ctx->bitmap_sizes[8 * i + 4] = k;
		ctx->bitmap_sizes[8 * i + 5] = layer;
		ctx->bitmap_sizes[8 * i + 6] = 0;
		ctx->bitmap_sizes[8 * i + 7] = 0;
	}
	free(pool_of);
	wgpuQueueWriteBuffer(ctx->queue, ctx->bitmap_sizes_buffer, 0,
	                     ctx->bitmap_sizes, 8 * sizeof(u32) * n);
	rebuild_fragment_sampler_bg(ctx);
	bitmap_pool_log("[render] %u static bitmaps in size-class pools: %.1f MiB total",
	                n, total_mib);
	if (spilled)
	{
		bitmap_pool_log("[render] %u static bitmaps spilled into a larger pool "
		                "(maxTextureArrayLayers %u)", spilled, L);
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
		EM_ASM({
			var g = globalThis.__swfGpu || (globalThis.__swfGpu = { lost: 0, stalls: 0 });
			g.spilled = $0;
		}, spilled);
#endif
	}
	if (dropped)
		render_texture_problem("[render] %u of %u static bitmaps do not fit the GPU's "
		                       "texture limits (%u layers per array, %u px per side) and "
		                       "will not draw", dropped, n, L, D);
}

static void create_buffers_and_upload(WebGPURenderContext* ctx)
{
	// Vertex buffer (shape geometry) — over-allocate for dynamic rendering.
	// MAX_DYNAMIC_RECTS must accommodate the largest per-frame draw count: each
	// dynamic createTextField with a border emits 4 draw_rect calls (one per
	// border edge), plus one draw_tris per glyph rendered. Tests like
	// avm1/edittext_stylesheet create 60+ text fields with borders + glyphs.
	// T5 (2026-08-01): raised 1024/32768 -> 4096/262144. The old vertex budget
	// capped native text at ~74 DefineFont3 glyphs per FRAME (a 30pt glyph
	// tessellates to ~340-440 vertices), which truncated a dense page mid-word
	// (fonts/embed_matching/fallback_preferences stopped inside "Regular" and
	// blanked every field below it). Cost: the dynamic vertex staging mirror and
	// its GPU region grow 0.5 MB -> 4 MB, the colour region 16 KB -> 64 KB.
	#define MAX_DYNAMIC_RECTS 4096      // max color slots for dynamic rendering
	#define MAX_DYNAMIC_VERTICES 262144 // max vertices for dynamic rendering
	{
		u32 orig_verts = (u32)(ctx->shape_data_size / (4 * sizeof(u32)));
		size_t extra_bytes = (size_t)MAX_DYNAMIC_VERTICES * 4 * sizeof(u32);
		size_t total_size = ctx->shape_data_size + extra_bytes;
		ctx->vertex_buffer = create_buffer(ctx->device, ctx->queue,
			WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
			NULL, total_size, "vertex_buffer");
		if (ctx->shape_data && ctx->shape_data_size > 0)
			wgpuQueueWriteBuffer(ctx->queue, ctx->vertex_buffer, 0,
				ctx->shape_data, ctx->shape_data_size);
		ctx->dynamic_vertex_base = orig_verts;
	}

	// Storage buffers
	// Over-allocate xform buffer to leave room for dynamic composed transform
	// slots.  compose_children() in tag.c needs unique slots when multiple
	// sprite instances share the same child transform_id.
	{
		u32 orig_slots = (u32)(ctx->transform_data_size / (16 * sizeof(float)));
		// Dynamic composed-transform slots, allocated fresh each frame by
		// compose_children() (one per nested/attached child instance). Games
		// that build a large grid of attachMovie'd clips need many: Tetris's
		// board alone is up to ~180 cells x 3 child shapes. 512 exhausted
		// mid-board, so the overflow fell back to overwriting the shared child
		// transform_id in place — every block collapsed onto the last one's
		// matrix (only one cell rendered). 4096 covers a full board + falling
		// piece + preview with headroom.
		u32 extra_slots = 4096;
		u32 total_slots = orig_slots + extra_slots;
		size_t total_size = (size_t)total_slots * 16 * sizeof(float);
		ctx->xform_buffer = create_buffer(ctx->device, ctx->queue,
			WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
			NULL, total_size, "xform_buffer");
		// Upload original data into the first orig_slots
		if (ctx->transform_data && ctx->transform_data_size > 0)
			wgpuQueueWriteBuffer(ctx->queue, ctx->xform_buffer, 0,
				ctx->transform_data, ctx->transform_data_size);
		ctx->xform_slot_count = total_slots;
	}

	// Color buffer — over-allocate for dynamic rect colors
	{
		u32 orig_colors = ctx->color_data_size > 0
			? (u32)(ctx->color_data_size / (4 * sizeof(float))) : 1;
		size_t extra_bytes = (size_t)MAX_DYNAMIC_RECTS * 4 * sizeof(float);
		size_t total_size = ctx->color_data_size + extra_bytes;
		ctx->color_buffer = create_buffer(ctx->device, ctx->queue,
			WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
			NULL, total_size, "color_buffer");
		if (ctx->color_data && ctx->color_data_size > 0)
			wgpuQueueWriteBuffer(ctx->queue, ctx->color_buffer, 0,
				ctx->color_data, ctx->color_data_size);
		ctx->dynamic_color_base = orig_colors;
	}

	// CPU staging mirrors for the dynamic vertex/color regions (batched upload —
	// see the comment on dyn_vtx_staging in render_webgpu.h).
	ctx->dyn_vtx_staging = (u32*)malloc((size_t)MAX_DYNAMIC_VERTICES * 4 * sizeof(u32));
	ctx->dyn_color_staging = (float*)malloc((size_t)MAX_DYNAMIC_RECTS * 4 * sizeof(float));

	// Retained-upload-skip mirrors (browser only — see prev_dyn_vtx in the header).
	// Native/OFFSCREEN leave these NULL so the skip is disabled and every frame
	// uploads as before (keeps the graphics-native test suite bit-for-bit).
#ifdef __EMSCRIPTEN__
	ctx->prev_dyn_vtx = (u32*)malloc((size_t)MAX_DYNAMIC_VERTICES * 4 * sizeof(u32));
	ctx->prev_dyn_color = (float*)malloc((size_t)MAX_DYNAMIC_RECTS * 4 * sizeof(float));
	// Transform-slot retained-skip mirror (zeroed → all slots start "invalid", so the
	// first write_transform per slot always uploads and syncs the mirror to the GPU).
	ctx->xform_mirror = (float*)calloc((size_t)ctx->xform_slot_count * 16, sizeof(float));
	ctx->xform_mirror_valid = (u8*)calloc((size_t)ctx->xform_slot_count, 1);
#endif
	ctx->prev_dyn_vtx_used = 0;
	ctx->prev_dyn_rect_count = 0;

	// Over-allocate uninv_mat and inv_mat buffers for dynamic gradient + bitmap matrices
	#define MAX_DYNAMIC_GRADIENTS 64
	u32 dynamic_bitmap_cap = MAX_DYNAMIC_BITMAP_SLOTS;
	{
		u32 static_mats = ctx->uninv_mat_data_size > 0
			? (u32)(ctx->uninv_mat_data_size / (16 * sizeof(float))) : 0;
		size_t total_mat_size = (size_t)(static_mats + MAX_DYNAMIC_GRADIENTS + dynamic_bitmap_cap) * 16 * sizeof(float);
		if (total_mat_size == 0) total_mat_size = 16 * sizeof(float); // minimum 1 slot

		ctx->uninv_mat_buffer = create_buffer(ctx->device, ctx->queue,
			WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
			NULL, total_mat_size, "uninv_mat_buffer");
		if (ctx->uninv_mat_data && ctx->uninv_mat_data_size > 0)
			wgpuQueueWriteBuffer(ctx->queue, ctx->uninv_mat_buffer, 0,
				ctx->uninv_mat_data, ctx->uninv_mat_data_size);

		ctx->inv_mat_buffer = create_buffer(ctx->device, ctx->queue,
			WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
			NULL, total_mat_size, "inv_mat_buffer");

		ctx->static_mat_count = static_mats;
		ctx->dynamic_gradient_capacity = MAX_DYNAMIC_GRADIENTS;
	}

	// Slot table: static bitmaps + per-frame dynamic slots, 8 u32 each.
	{
		u32 total_bmp_slots = (u32)ctx->bitmap_count + dynamic_bitmap_cap;
		ctx->bitmap_sizes_buffer = create_buffer(ctx->device, ctx->queue,
			WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
			NULL, 8 * sizeof(u32) * total_bmp_slots, "bitmap_sizes_buffer");
		ctx->dynamic_bitmap_base = (u32)ctx->bitmap_count;
		ctx->dynamic_bitmap_capacity = dynamic_bitmap_cap;
		ctx->dynamic_bitmap_used = 0;
	}

	// Over-allocate cxform buffer for dynamic runtime cxform slots
	// (Color.setRGB/setTransform modify cx_* at runtime)
	{
		u32 orig_cxform_slots = ctx->cxform_data_size > 0
			? (u32)(ctx->cxform_data_size / (20 * sizeof(float))) : 1;
		u32 extra_cxform_slots = 256;
		u32 total_cxform_slots = orig_cxform_slots + extra_cxform_slots;
		size_t total_cxform_size = (size_t)total_cxform_slots * 20 * sizeof(float);
		ctx->cxform_buffer = create_buffer(ctx->device, ctx->queue,
			WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
			NULL, total_cxform_size, "cxform_buffer");
		if (ctx->cxform_data && ctx->cxform_data_size > 0)
			wgpuQueueWriteBuffer(ctx->queue, ctx->cxform_buffer, 0,
				ctx->cxform_data, ctx->cxform_data_size);
		ctx->cxform_slot_count = total_cxform_slots;
	}

	// Uniform buffers (small, updated per-frame/per-draw)
	ctx->stage_to_ndc_buf = create_buffer(ctx->device, ctx->queue,
		WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
		NULL, 64, "stage_to_ndc_uniform");  // mat4 = 64 bytes

	ctx->transform_id_buf = create_buffer(ctx->device, ctx->queue,
		WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
		NULL, 16, "transform_id_uniform");  // u32 padded to 16 bytes (min uniform alignment)

	ctx->extra_transform_id_buf = create_buffer(ctx->device, ctx->queue,
		WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
		NULL, 16, "extra_transform_id_uniform");  // u32 padded to 16 bytes

	ctx->extra_transform_buf = create_buffer(ctx->device, ctx->queue,
		WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
		NULL, 64, "extra_transform_uniform");  // mat4 = 64 bytes

	ctx->cxform_id_buf = create_buffer(ctx->device, ctx->queue,
		WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
		NULL, 16, "cxform_id_uniform");  // u32 padded to 16 bytes

	ctx->cxform_uniform_buf = create_buffer(ctx->device, ctx->queue,
		WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
		NULL, 80, "cxform_uniform");  // 20 floats = 80 bytes
}

// ---------------------------------------------------------------------------
// create_textures: gradient texture array, bitmap texture array, MSAA
// ---------------------------------------------------------------------------
static void create_textures(WebGPURenderContext* ctx)
{
	size_t sizeof_gradient = 256 * 4; // 256 RGBA8 entries per gradient = 1024 bytes
	size_t num_gradients = ctx->gradient_data_size / sizeof_gradient;
	u32 total_gradient_layers = (u32)num_gradients + MAX_DYNAMIC_GRADIENTS;
	if (total_gradient_layers == 0) total_gradient_layers = 1; // minimum 1 layer
	ctx->static_gradient_count = (u32)num_gradients;
	// One row per gradient, so the texture's HEIGHT is the ramp count and must
	// fit maxTextureDimension2D (8192 by default: 8128 static gradients plus
	// the dynamic rows). Past that the texture would be invalid and take every
	// draw with it; instead the rows stop at the limit, the dynamic rows go
	// first, and the gradients that lost their row draw from whatever an
	// out-of-range textureLoad returns.
	u32 grad_rows_max = ctx->max_texture_dimension_2d ? ctx->max_texture_dimension_2d : 8192;
	u32 static_grad_rows = (u32)num_gradients;
	if (total_gradient_layers > grad_rows_max)
	{
		total_gradient_layers = grad_rows_max;
		if (static_grad_rows > grad_rows_max) static_grad_rows = grad_rows_max;
		if (ctx->dynamic_gradient_capacity > grad_rows_max - static_grad_rows)
			ctx->dynamic_gradient_capacity = grad_rows_max - static_grad_rows;
		render_texture_problem("[render] %u gradients exceed the GPU's %u-row texture limit; "
		                       "%u static gradients and %u dynamic ones per frame get a row",
		                       (unsigned)num_gradients, grad_rows_max, static_grad_rows,
		                       grad_rows_max - static_grad_rows);
	}

	// --- Gradient texture (always created, over-allocated for dynamic gradients) ---
	// Each gradient ramp is 256x1 RGBA8. We pack one ramp per ROW of a single
	// 256 x total_gradient_layers 2D texture rather than using array layers,
	// because maxTextureArrayLayers is only 256 on some adapters (notably
	// SwiftShader / WSL2 browsers) and content-heavy SWFs have far more
	// gradients than that (e.g. Meteor Storm: 519). maxTextureDimension2D is
	// 8192+, so the row-packed layout scales to thousands of gradients.
	{
		WGPUTextureDescriptor tex_desc = {0};
		tex_desc.label = WGPU_LABEL("gradient_tex");
		tex_desc.dimension = WGPUTextureDimension_2D;
		tex_desc.size = (WGPUExtent3D){256, total_gradient_layers, 1};
		tex_desc.format = WGPUTextureFormat_RGBA8Unorm;
		tex_desc.mipLevelCount = 1;
		tex_desc.sampleCount = 1;
		tex_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
		texture_scope_push(ctx);
		ctx->gradient_tex = wgpuDeviceCreateTexture(ctx->device, &tex_desc);
		texture_scope_pop(ctx, "gradient_tex");

		WGPUTextureViewDescriptor view_desc = {0};
		view_desc.dimension = WGPUTextureViewDimension_2D;
		view_desc.arrayLayerCount = 1;
		view_desc.mipLevelCount = 1;
		ctx->gradient_tex_view = wgpuTextureCreateView(ctx->gradient_tex, &view_desc);

		// Upload static gradient data — consecutive 256-texel ramps map to
		// consecutive rows (bytesPerRow = one ramp, rowsPerImage = ramp count).
		if (static_grad_rows > 0)
		{
			WGPUTexelCopyTextureInfo dest = {0};
			dest.texture = ctx->gradient_tex;
			WGPUTexelCopyBufferLayout layout = {0};
			layout.bytesPerRow = 256 * 4;
			layout.rowsPerImage = static_grad_rows;
			WGPUExtent3D extent = {256, static_grad_rows, 1};
			wgpuQueueWriteTexture(ctx->queue, &dest, ctx->gradient_data,
			                      (size_t)static_grad_rows * 256 * 4, &layout, &extent);
		}

		WGPUSamplerDescriptor samp_desc = {0};
		samp_desc.label = WGPU_LABEL("gradient_sampler");
		samp_desc.addressModeU = WGPUAddressMode_ClampToEdge;
		samp_desc.addressModeV = WGPUAddressMode_ClampToEdge;
		samp_desc.magFilter = WGPUFilterMode_Linear;
		samp_desc.minFilter = WGPUFilterMode_Linear;
		samp_desc.maxAnisotropy = 1;
		ctx->gradient_sampler = wgpuDeviceCreateSampler(ctx->device, &samp_desc);
	}

	// --- Bitmap samplers. The pool textures are created later: static pools
	// in render_webgpu_finalize_bitmaps, dynamic pools on first use. ---
	{
		WGPUSamplerDescriptor samp_desc = {0};
		samp_desc.label = WGPU_LABEL("bitmap_sampler");
		samp_desc.addressModeU = WGPUAddressMode_ClampToEdge;
		samp_desc.addressModeV = WGPUAddressMode_ClampToEdge;
		samp_desc.magFilter = WGPUFilterMode_Nearest;
		samp_desc.minFilter = WGPUFilterMode_Nearest;
		samp_desc.maxAnisotropy = 1;
		ctx->bitmap_sampler = wgpuDeviceCreateSampler(ctx->device, &samp_desc);

		// Same addressing, Linear filtering — bound at group(2) binding(4) and
		// used only by the smoothed fill types (0x40 / 0x41).
		samp_desc.label = WGPU_LABEL("bitmap_sampler_linear");
		samp_desc.magFilter = WGPUFilterMode_Linear;
		samp_desc.minFilter = WGPUFilterMode_Linear;
		ctx->bitmap_sampler_linear = wgpuDeviceCreateSampler(ctx->device, &samp_desc);
	}

	// --- MSAA texture (MSAA_SAMPLES x) ---
	{
		WGPUTextureDescriptor tex_desc = {0};
		tex_desc.label = WGPU_LABEL("msaa_texture");
		tex_desc.dimension = WGPUTextureDimension_2D;
		tex_desc.size = (WGPUExtent3D){(u32)ctx->width, (u32)ctx->height, 1};
		tex_desc.format = ctx->surface_format;
		tex_desc.mipLevelCount = 1;
		tex_desc.sampleCount = MSAA_SAMPLES;
		tex_desc.usage = WGPUTextureUsage_RenderAttachment;
		ctx->msaa_texture = wgpuDeviceCreateTexture(ctx->device, &tex_desc);

		WGPUTextureViewDescriptor view_desc = {0};
		view_desc.mipLevelCount = 1;
		view_desc.arrayLayerCount = 1;
		ctx->msaa_view = wgpuTextureCreateView(ctx->msaa_texture, &view_desc);
	}

	// --- Depth-stencil MSAA texture (for clip masks) ---
	{
		WGPUTextureDescriptor tex_desc = {0};
		tex_desc.label = WGPU_LABEL("depth_stencil_texture");
		tex_desc.dimension = WGPUTextureDimension_2D;
		tex_desc.size = (WGPUExtent3D){(u32)ctx->width, (u32)ctx->height, 1};
		tex_desc.format = WGPUTextureFormat_Depth24PlusStencil8;
		tex_desc.mipLevelCount = 1;
		tex_desc.sampleCount = MSAA_SAMPLES;
		tex_desc.usage = WGPUTextureUsage_RenderAttachment;
		ctx->depth_stencil_texture = wgpuDeviceCreateTexture(ctx->device, &tex_desc);

		WGPUTextureViewDescriptor view_desc = {0};
		view_desc.mipLevelCount = 1;
		view_desc.arrayLayerCount = 1;
		ctx->depth_stencil_view = wgpuTextureCreateView(ctx->depth_stencil_texture, &view_desc);
	}
}

// Full-screen NDC quad, colour writes off — the geometry of a clip-mask POP.
//
// Ruffle re-draws the masker itself for its ClearMaskStencil pass
// (core/src/display_object.rs:1256-1269); we would have to keep "re-draw this
// masker" alive across five different push sites, so we cover the screen
// instead. Equivalent, because Equal(level) selects precisely the texels the
// innermost mask raised. NOT render_webgpu_draw_rect: that goes through the
// dynamic vertex/colour staging and silently no-ops when it is full, which
// would turn a stencil pop into a capacity bug, and it depends on whatever
// stage transform is installed. An NDC quad has neither problem and needs no
// vertex buffer, no bind group data and no transform slot.
static const char* stencil_clear_wgsl =
"@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4f {\n"
"  var positions = array<vec2f, 6>(\n"
"    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),\n"
"    vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0)\n"
"  );\n"
"  return vec4f(positions[vi], 0.0, 1.0);\n"
"}\n"
"\n"
"@fragment fn fs_main() -> @location(0) vec4f {\n"
"  return vec4f(0.0, 0.0, 0.0, 0.0);\n"
"}\n";

// ---------------------------------------------------------------------------
// create_pipelines: render pipeline + compute pipeline
// ---------------------------------------------------------------------------
static void create_pipelines(WebGPURenderContext* ctx)
{
	// --- Bind group layouts ---

	// Group 0: storage buffers (vertex + fragment)
	WGPUBindGroupLayoutEntry bg0_entries[5] = {0};
	for (int i = 0; i < 4; i++)
	{
		bg0_entries[i].binding = i;
		bg0_entries[i].visibility = WGPUShaderStage_Vertex;
		bg0_entries[i].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
	}
	// Entry 4: cxform buffer (fragment shader)
	bg0_entries[4].binding = 4;
	bg0_entries[4].visibility = WGPUShaderStage_Fragment;
	bg0_entries[4].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;

	WGPUBindGroupLayoutDescriptor bg0_desc = {0};
	bg0_desc.label = WGPU_LABEL("storage_bgl");
	bg0_desc.entryCount = 5;
	bg0_desc.entries = bg0_entries;
	ctx->vertex_storage_bgl = wgpuDeviceCreateBindGroupLayout(ctx->device, &bg0_desc);

	// Group 1: vertex uniforms
	WGPUBindGroupLayoutEntry bg1_entries[2] = {0};
	bg1_entries[0].binding = 0;
	bg1_entries[0].visibility = WGPUShaderStage_Vertex;
	bg1_entries[0].buffer.type = WGPUBufferBindingType_Uniform;
	bg1_entries[1].binding = 1;
	bg1_entries[1].visibility = WGPUShaderStage_Vertex;
	bg1_entries[1].buffer.type = WGPUBufferBindingType_Uniform;

	WGPUBindGroupLayoutDescriptor bg1_desc = {0};
	bg1_desc.label = WGPU_LABEL("vertex_uniform_bgl");
	bg1_desc.entryCount = 2;
	bg1_desc.entries = bg1_entries;
	ctx->vertex_uniform_bgl = wgpuDeviceCreateBindGroupLayout(ctx->device, &bg1_desc);

	// Group 2: fragment textures + samplers: gradient tex (0), gradient samp (1),
	// bitmap samp nearest (2), bitmap samp linear (3), then one texture_2d_array
	// per bitmap pool (BITMAP_POOL_BINDING_BASE + i). 13 sampled textures per
	// stage, under WebGPU's guaranteed 16.
	WGPUBindGroupLayoutEntry bg2_entries[BITMAP_POOL_BINDING_BASE + BITMAP_POOL_COUNT] = {0};
	bg2_entries[0].binding = 0;
	bg2_entries[0].visibility = WGPUShaderStage_Fragment;
	bg2_entries[0].texture.sampleType = WGPUTextureSampleType_Float;
	bg2_entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;  // gradient_tex: 256xN rows
	bg2_entries[1].binding = 1;
	bg2_entries[1].visibility = WGPUShaderStage_Fragment;
	bg2_entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
	bg2_entries[2].binding = 2;
	bg2_entries[2].visibility = WGPUShaderStage_Fragment;
	bg2_entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
	bg2_entries[3].binding = 3;
	bg2_entries[3].visibility = WGPUShaderStage_Fragment;
	bg2_entries[3].sampler.type = WGPUSamplerBindingType_Filtering;
	for (u32 i = 0; i < BITMAP_POOL_COUNT; i++)
	{
		WGPUBindGroupLayoutEntry* e = &bg2_entries[BITMAP_POOL_BINDING_BASE + i];
		e->binding = BITMAP_POOL_BINDING_BASE + i;
		e->visibility = WGPUShaderStage_Fragment;
		e->texture.sampleType = WGPUTextureSampleType_Float;
		e->texture.viewDimension = WGPUTextureViewDimension_2DArray;
	}

	WGPUBindGroupLayoutDescriptor bg2_desc = {0};
	bg2_desc.label = WGPU_LABEL("fragment_sampler_bgl");
	bg2_desc.entryCount = BITMAP_POOL_BINDING_BASE + BITMAP_POOL_COUNT;
	bg2_desc.entries = bg2_entries;
	ctx->fragment_sampler_bgl = wgpuDeviceCreateBindGroupLayout(ctx->device, &bg2_desc);

	// Render pipeline layout
	WGPUBindGroupLayout render_bgls[3] = {
		ctx->vertex_storage_bgl,
		ctx->vertex_uniform_bgl,
		ctx->fragment_sampler_bgl,
	};
	WGPUPipelineLayoutDescriptor pl_desc = {0};
	pl_desc.label = WGPU_LABEL("render_pipeline_layout");
	pl_desc.bindGroupLayoutCount = 3;
	pl_desc.bindGroupLayouts = render_bgls;
	ctx->render_pipeline_layout = wgpuDeviceCreatePipelineLayout(ctx->device, &pl_desc);

	// --- Create shader modules ---
	WGPUShaderModule vs_module = create_shader(ctx->device, vertex_wgsl, "vertex_shader");
	WGPUShaderModule fs_module = create_shader(ctx->device, fragment_wgsl, "fragment_shader");

	// --- Render pipeline ---
	WGPUVertexAttribute attrs[2] = {0};
	attrs[0].format = WGPUVertexFormat_Float32x2;  // position
	attrs[0].offset = 0;
	attrs[0].shaderLocation = 0;
	attrs[1].format = WGPUVertexFormat_Uint32x2;   // style
	attrs[1].offset = 2 * sizeof(u32);
	attrs[1].shaderLocation = 1;

	WGPUVertexBufferLayout vb_layout = {0};
	vb_layout.arrayStride = 4 * sizeof(u32);  // 16 bytes per vertex
	vb_layout.stepMode = WGPUVertexStepMode_Vertex;
	vb_layout.attributeCount = 2;
	vb_layout.attributes = attrs;

	WGPUBlendState blend = {0};
	blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
	blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
	blend.color.operation = WGPUBlendOperation_Add;
	// Alpha: standard Porter-Duff "source over" — src_alpha + dst_alpha * (1 - src_alpha)
	// Using SrcAlpha here would square the alpha: src_alpha² + dst_alpha * (1 - src_alpha)
	blend.alpha.srcFactor = WGPUBlendFactor_One;
	blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
	blend.alpha.operation = WGPUBlendOperation_Add;

	WGPUColorTargetState color_target = {0};
	color_target.format = ctx->surface_format;
	color_target.blend = &blend;
	color_target.writeMask = WGPUColorWriteMask_All;

	WGPUFragmentState frag_state = {0};
	frag_state.module = fs_module;
	frag_state.entryPoint.data = "fs_main";
	frag_state.entryPoint.length = 7;
	frag_state.targetCount = 1;
	frag_state.targets = &color_target;

	// --- Depth-stencil state (shared base for all pipelines) ---
	// All pipelines must have depth-stencil state since the render pass
	// always has a depth-stencil attachment (needed for clip masks).

	// Normal pipeline: stencil always passes, never written (no-op)
	WGPUDepthStencilState ds_normal = {0};
	ds_normal.format = WGPUTextureFormat_Depth24PlusStencil8;
	ds_normal.depthWriteEnabled = false;
	ds_normal.depthCompare = WGPUCompareFunction_Always;
	ds_normal.stencilFront.compare = WGPUCompareFunction_Always;
	ds_normal.stencilFront.passOp = WGPUStencilOperation_Keep;
	ds_normal.stencilFront.failOp = WGPUStencilOperation_Keep;
	ds_normal.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
	ds_normal.stencilBack = ds_normal.stencilFront;
	ds_normal.stencilReadMask = 0xFF;
	ds_normal.stencilWriteMask = 0x00;

	WGPURenderPipelineDescriptor rp_desc = {0};
	rp_desc.label = WGPU_LABEL("render_pipeline");
	rp_desc.layout = ctx->render_pipeline_layout;
	rp_desc.vertex.module = vs_module;
	rp_desc.vertex.entryPoint.data = "vs_main";
	rp_desc.vertex.entryPoint.length = 7;
	rp_desc.vertex.bufferCount = 1;
	rp_desc.vertex.buffers = &vb_layout;
	rp_desc.fragment = &frag_state;
	rp_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	rp_desc.multisample.count = MSAA_SAMPLES;
	rp_desc.multisample.mask = ~0u;
	rp_desc.depthStencil = &ds_normal;

	ctx->render_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rp_desc);
	assert(ctx->render_pipeline != NULL);

	// --- Stencil write pipeline: raises the clip level, no color output ---
	// Used for clip mask shapes. The mask's geometry defines the clipping
	// region, and Equal(level-1)/IncrementClamp raises ONLY the texels the
	// enclosing mask already owns — a nested mask therefore clips to the
	// INTERSECTION for free (ruffle render/wgpu/src/pipelines.rs:387-395,
	// MaskState::DrawMaskStencil). failOp = Keep makes the write idempotent
	// under self-overlapping mask geometry: a second triangle of the SAME
	// mask finds level != level-1, fails the compare and is left alone, so a
	// masker whose subtree draws overlapping shapes cannot increment itself
	// into a hole. Same argument covers the matching decrement.
	WGPUDepthStencilState ds_write = ds_normal;
	ds_write.stencilFront.compare = WGPUCompareFunction_Equal;
	ds_write.stencilFront.passOp = WGPUStencilOperation_IncrementClamp;
	ds_write.stencilWriteMask = 0xFF;
	ds_write.stencilBack = ds_write.stencilFront;

	WGPUColorTargetState color_target_masked = color_target;
	color_target_masked.writeMask = WGPUColorWriteMask_None;
	WGPUFragmentState frag_state_masked = frag_state;
	frag_state_masked.targets = &color_target_masked;

	rp_desc.label = WGPU_LABEL("stencil_write_pipeline");
	rp_desc.depthStencil = &ds_write;
	rp_desc.fragment = &frag_state_masked;

	ctx->stencil_write_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rp_desc);
	assert(ctx->stencil_write_pipeline != NULL);

	// --- Stencil test pipeline: only draws where stencil == ref ---
	// Used for objects clipped by a mask.
	WGPUDepthStencilState ds_test = ds_normal;
	ds_test.stencilFront.compare = WGPUCompareFunction_Equal;
	ds_test.stencilBack = ds_test.stencilFront;

	rp_desc.label = WGPU_LABEL("stencil_test_pipeline");
	rp_desc.depthStencil = &ds_test;
	rp_desc.fragment = &frag_state;  // normal color output

	ctx->stencil_test_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rp_desc);
	assert(ctx->stencil_test_pipeline != NULL);

	// --- Stencil clear pipeline: lowers the clip level, no color output ---
	// ruffle MaskState::ClearMaskStencil (pipelines.rs:406-414). Drawn as the
	// full-screen NDC quad above, so it needs no vertex buffer — but it keeps
	// the render pipeline LAYOUT, so the three bind groups the pass set at
	// open time stay bound (a pipeline with an incompatible layout would unset
	// them and every later draw would fail validation).
	{
		WGPUDepthStencilState ds_clear = ds_normal;
		ds_clear.stencilFront.compare = WGPUCompareFunction_Equal;
		ds_clear.stencilFront.passOp = WGPUStencilOperation_DecrementClamp;
		ds_clear.stencilWriteMask = 0xFF;
		ds_clear.stencilBack = ds_clear.stencilFront;

		WGPUShaderModule clear_sm = create_shader(ctx->device, stencil_clear_wgsl,
		                                          "stencil_clear_shader");
		WGPUFragmentState frag_state_clear = frag_state_masked;   // colour writes OFF
		frag_state_clear.module = clear_sm;

		rp_desc.label = WGPU_LABEL("stencil_clear_pipeline");
		rp_desc.depthStencil = &ds_clear;
		rp_desc.fragment = &frag_state_clear;
		rp_desc.vertex.module = clear_sm;
		rp_desc.vertex.bufferCount = 0;
		rp_desc.vertex.buffers = NULL;
		// rp_desc.multisample.count is still MSAA_SAMPLES — never a literal 4;
		// quality="low" goldens compile the whole renderer at 1 sample and a
		// mismatch here fails pipeline creation loudly.

		ctx->stencil_clear_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rp_desc);
		assert(ctx->stencil_clear_pipeline != NULL);
		wgpuShaderModuleRelease(clear_sm);

		// Put the shared descriptor back for the pipelines created below.
		rp_desc.vertex.module = vs_module;
		rp_desc.vertex.bufferCount = 1;
		rp_desc.vertex.buffers = &vb_layout;
	}

	// --- Blend mode pipelines ---
	// Legacy per-draw blend pipelines. The layer-composite path in
	// render_webgpu_composite_blend() supersedes these for stage/sprite display
	// objects; they remain the fallback for nested-layer and filter+blend cases.
	//
	// Every one of them uses BlendComponent::OVER for ALPHA (ruffle
	// render/wgpu/src/blend.rs: Add and Subtract are `alpha: OVER`). A saturating,
	// min/max-ing or subtracting alpha component destroys the destination alpha:
	// `subtract` used to compute dst.a - src.a, which over an opaque backdrop is
	// 0, so the object rendered as a fully transparent HOLE.
	rp_desc.depthStencil = &ds_normal;
	rp_desc.fragment = &frag_state;

	// Add (blend mode 8): SrcAlpha / One / Add colour, OVER alpha
	{
		WGPUBlendState blend_add = {0};
		blend_add.color.srcFactor = WGPUBlendFactor_SrcAlpha;
		blend_add.color.dstFactor = WGPUBlendFactor_One;
		blend_add.color.operation = WGPUBlendOperation_Add;
		blend_add.alpha.srcFactor = WGPUBlendFactor_One;
		blend_add.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend_add.alpha.operation = WGPUBlendOperation_Add;

		WGPUColorTargetState ct_add = color_target;
		ct_add.blend = &blend_add;
		WGPUFragmentState fs_add = frag_state;
		fs_add.targets = &ct_add;

		rp_desc.label = WGPU_LABEL("blend_add_pipeline");
		rp_desc.fragment = &fs_add;
		ctx->blend_add_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rp_desc);
		assert(ctx->blend_add_pipeline != NULL);
	}

	// Lighten (blend mode 5): One / One / Max
	{
		WGPUBlendState blend_lighten = {0};
		blend_lighten.color.srcFactor = WGPUBlendFactor_One;
		blend_lighten.color.dstFactor = WGPUBlendFactor_One;
		blend_lighten.color.operation = WGPUBlendOperation_Max;
		blend_lighten.alpha.srcFactor = WGPUBlendFactor_One;
		blend_lighten.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend_lighten.alpha.operation = WGPUBlendOperation_Add;

		WGPUColorTargetState ct_lighten = color_target;
		ct_lighten.blend = &blend_lighten;
		WGPUFragmentState fs_lighten = frag_state;
		fs_lighten.targets = &ct_lighten;

		rp_desc.label = WGPU_LABEL("blend_lighten_pipeline");
		rp_desc.fragment = &fs_lighten;
		ctx->blend_lighten_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rp_desc);
		assert(ctx->blend_lighten_pipeline != NULL);
	}

	// Darken (blend mode 6): One / One / Min
	{
		WGPUBlendState blend_darken = {0};
		blend_darken.color.srcFactor = WGPUBlendFactor_One;
		blend_darken.color.dstFactor = WGPUBlendFactor_One;
		blend_darken.color.operation = WGPUBlendOperation_Min;
		blend_darken.alpha.srcFactor = WGPUBlendFactor_One;
		blend_darken.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend_darken.alpha.operation = WGPUBlendOperation_Add;

		WGPUColorTargetState ct_darken = color_target;
		ct_darken.blend = &blend_darken;
		WGPUFragmentState fs_darken = frag_state;
		fs_darken.targets = &ct_darken;

		rp_desc.label = WGPU_LABEL("blend_darken_pipeline");
		rp_desc.fragment = &fs_darken;
		ctx->blend_darken_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rp_desc);
		assert(ctx->blend_darken_pipeline != NULL);
	}

	// Subtract (blend mode 9): SrcAlpha / One / ReverseSubtract
	{
		WGPUBlendState blend_sub = {0};
		blend_sub.color.srcFactor = WGPUBlendFactor_SrcAlpha;
		blend_sub.color.dstFactor = WGPUBlendFactor_One;
		blend_sub.color.operation = WGPUBlendOperation_ReverseSubtract;
		blend_sub.alpha.srcFactor = WGPUBlendFactor_One;
		blend_sub.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend_sub.alpha.operation = WGPUBlendOperation_Add;

		WGPUColorTargetState ct_sub = color_target;
		ct_sub.blend = &blend_sub;
		WGPUFragmentState fs_sub = frag_state;
		fs_sub.targets = &ct_sub;

		rp_desc.label = WGPU_LABEL("blend_subtract_pipeline");
		rp_desc.fragment = &fs_sub;
		ctx->blend_subtract_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rp_desc);
		assert(ctx->blend_subtract_pipeline != NULL);
	}

	// Premultiplied alpha (for attached BitmapData): One / OneMinusSrcAlpha
	{
		WGPUBlendState blend_premul = {0};
		blend_premul.color.srcFactor = WGPUBlendFactor_One;
		blend_premul.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend_premul.color.operation = WGPUBlendOperation_Add;
		blend_premul.alpha.srcFactor = WGPUBlendFactor_One;
		blend_premul.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend_premul.alpha.operation = WGPUBlendOperation_Add;

		WGPUColorTargetState ct_premul = color_target;
		ct_premul.blend = &blend_premul;
		WGPUFragmentState fs_premul = frag_state;
		fs_premul.targets = &ct_premul;

		rp_desc.label = WGPU_LABEL("blend_premul_pipeline");
		rp_desc.fragment = &fs_premul;
		ctx->blend_premul_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rp_desc);
		assert(ctx->blend_premul_pipeline != NULL);

		// ...and the SAME blend state behind the Equal-compare stencil test.
		//
		// The plain premul pipeline carries ds_normal (stencil compare Always),
		// so EVERY bitmap draw silently dropped the enclosing clip: a Bitmap
		// display object, an attachBitmap, or a beginBitmapFill inside a mask
		// painted its whole quad. Ruffle has no such hole — its bitmap pipeline
		// is `pipelines.bitmap.pipeline_for(mask_state)`
		// (render/wgpu/src/pipelines.rs), i.e. the mask state picks the stencil
		// variant of the *same* blend for bitmaps exactly as it does for shapes.
		// This is that variant; bind_premul_pipeline() below chooses between
		// the two the way restore_draw_pipeline() already does for shapes.
		rp_desc.label = WGPU_LABEL("blend_premul_stencil_pipeline");
		rp_desc.depthStencil = &ds_test;
		ctx->blend_premul_stencil_pipeline =
			wgpuDeviceCreateRenderPipeline(ctx->device, &rp_desc);
		assert(ctx->blend_premul_stencil_pipeline != NULL);
		rp_desc.depthStencil = &ds_normal;
	}

	wgpuShaderModuleRelease(vs_module);
	wgpuShaderModuleRelease(fs_module);

	// --- Compute pipeline (for gradient matrix inversion) ---
	size_t num_gradients = ctx->uninv_mat_data_size / (16 * sizeof(float));
	if (num_gradients > 0)
	{
		WGPUBindGroupLayoutEntry comp0_entry = {0};
		comp0_entry.binding = 0;
		comp0_entry.visibility = WGPUShaderStage_Compute;
		comp0_entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;

		WGPUBindGroupLayoutDescriptor comp0_desc = {0};
		comp0_desc.label = WGPU_LABEL("compute_read_bgl");
		comp0_desc.entryCount = 1;
		comp0_desc.entries = &comp0_entry;
		ctx->compute_read_bgl = wgpuDeviceCreateBindGroupLayout(ctx->device, &comp0_desc);

		WGPUBindGroupLayoutEntry comp1_entry = {0};
		comp1_entry.binding = 0;
		comp1_entry.visibility = WGPUShaderStage_Compute;
		comp1_entry.buffer.type = WGPUBufferBindingType_Storage;

		WGPUBindGroupLayoutDescriptor comp1_desc = {0};
		comp1_desc.label = WGPU_LABEL("compute_write_bgl");
		comp1_desc.entryCount = 1;
		comp1_desc.entries = &comp1_entry;
		ctx->compute_write_bgl = wgpuDeviceCreateBindGroupLayout(ctx->device, &comp1_desc);

		WGPUBindGroupLayout comp_bgls[2] = {ctx->compute_read_bgl, ctx->compute_write_bgl};
		WGPUPipelineLayoutDescriptor comp_pl_desc = {0};
		comp_pl_desc.label = WGPU_LABEL("compute_pipeline_layout");
		comp_pl_desc.bindGroupLayoutCount = 2;
		comp_pl_desc.bindGroupLayouts = comp_bgls;
		ctx->compute_pipeline_layout = wgpuDeviceCreatePipelineLayout(ctx->device, &comp_pl_desc);

		WGPUShaderModule cs_module = create_shader(ctx->device, compute_wgsl, "compute_shader");

		WGPUComputePipelineDescriptor cp_desc = {0};
		cp_desc.label = WGPU_LABEL("compute_pipeline");
		cp_desc.layout = ctx->compute_pipeline_layout;
		cp_desc.compute.module = cs_module;
		cp_desc.compute.entryPoint.data = "cs_main";
		cp_desc.compute.entryPoint.length = 7;

		ctx->compute_pipeline = wgpuDeviceCreateComputePipeline(ctx->device, &cp_desc);
		assert(ctx->compute_pipeline != NULL);

		wgpuShaderModuleRelease(cs_module);
	}
}

// ---------------------------------------------------------------------------
// create_bind_groups
// ---------------------------------------------------------------------------
// Group 2 bind group: gradient texture + sampler, the two bitmap samplers, and
// one texture view per bitmap pool (the 1x1 dummy array for pools that do not
// exist yet). Called at init, when the static pools are built, and whenever a
// dynamic pool is created or grown; if a render pass is open the new group is
// bound immediately, so the draws recorded from here on see the new pool while
// earlier draws keep the old group (and through it the old texture) alive.
static void rebuild_fragment_sampler_bg(WebGPURenderContext* ctx)
{
	WGPUTextureView grad_view = ctx->gradient_tex_view ? ctx->gradient_tex_view : ctx->dummy_tex_2d_view;
	WGPUSampler grad_samp = ctx->gradient_sampler ? ctx->gradient_sampler : ctx->dummy_sampler;
	WGPUSampler bmp_samp = ctx->bitmap_sampler ? ctx->bitmap_sampler : ctx->dummy_sampler;
	WGPUSampler bmp_samp_lin = ctx->bitmap_sampler_linear ? ctx->bitmap_sampler_linear : bmp_samp;

	WGPUBindGroupEntry entries[BITMAP_POOL_BINDING_BASE + BITMAP_POOL_COUNT] = {0};
	entries[0].binding = 0;
	entries[0].textureView = grad_view;
	entries[1].binding = 1;
	entries[1].sampler = grad_samp;
	entries[2].binding = 2;
	entries[2].sampler = bmp_samp;
	entries[3].binding = 3;
	entries[3].sampler = bmp_samp_lin;
	for (u32 i = 0; i < BITMAP_POOL_COUNT; i++)
	{
		entries[BITMAP_POOL_BINDING_BASE + i].binding = BITMAP_POOL_BINDING_BASE + i;
		entries[BITMAP_POOL_BINDING_BASE + i].textureView =
			ctx->bitmap_pools[i].view ? ctx->bitmap_pools[i].view : ctx->dummy_tex_view;
	}

	WGPUBindGroupDescriptor desc = {0};
	desc.label = WGPU_LABEL("fragment_sampler_bg");
	desc.layout = ctx->fragment_sampler_bgl;
	desc.entryCount = BITMAP_POOL_BINDING_BASE + BITMAP_POOL_COUNT;
	desc.entries = entries;
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(ctx->device, &desc);
	if (ctx->fragment_sampler_bg) wgpuBindGroupRelease(ctx->fragment_sampler_bg);
	ctx->fragment_sampler_bg = bg;
	if (ctx->render_pass)
		wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 2, ctx->fragment_sampler_bg, 0, NULL);
}

static void create_bind_groups(WebGPURenderContext* ctx)
{
	// --- Group 0: storage buffers ---
	WGPUBindGroupEntry bg0_entries[5] = {0};
	bg0_entries[0].binding = 0;
	bg0_entries[0].buffer = ctx->xform_buffer;
	bg0_entries[0].size = wgpuBufferGetSize(ctx->xform_buffer);
	bg0_entries[1].binding = 1;
	bg0_entries[1].buffer = ctx->color_buffer;
	bg0_entries[1].size = wgpuBufferGetSize(ctx->color_buffer);
	bg0_entries[2].binding = 2;
	bg0_entries[2].buffer = ctx->inv_mat_buffer;
	bg0_entries[2].size = wgpuBufferGetSize(ctx->inv_mat_buffer);
	bg0_entries[3].binding = 3;
	bg0_entries[3].buffer = ctx->bitmap_sizes_buffer;
	bg0_entries[3].size = wgpuBufferGetSize(ctx->bitmap_sizes_buffer);
	bg0_entries[4].binding = 4;
	bg0_entries[4].buffer = ctx->cxform_buffer;
	bg0_entries[4].size = wgpuBufferGetSize(ctx->cxform_buffer);

	WGPUBindGroupDescriptor bg0_desc = {0};
	bg0_desc.label = WGPU_LABEL("storage_bg");
	bg0_desc.layout = ctx->vertex_storage_bgl;
	bg0_desc.entryCount = 5;
	bg0_desc.entries = bg0_entries;
	ctx->vertex_storage_bg = wgpuDeviceCreateBindGroup(ctx->device, &bg0_desc);

	// --- Group 1: vertex uniforms ---
	WGPUBindGroupEntry bg1_entries[2] = {0};
	bg1_entries[0].binding = 0;
	bg1_entries[0].buffer = ctx->stage_to_ndc_buf;
	bg1_entries[0].size = 64; // mat4
	bg1_entries[1].binding = 1;
	bg1_entries[1].buffer = ctx->transform_id_buf;
	bg1_entries[1].size = 16; // u32 padded

	WGPUBindGroupDescriptor bg1_desc = {0};
	bg1_desc.label = WGPU_LABEL("vertex_uniform_bg");
	bg1_desc.layout = ctx->vertex_uniform_bgl;
	bg1_desc.entryCount = 2;
	bg1_desc.entries = bg1_entries;
	ctx->vertex_uniform_bg = wgpuDeviceCreateBindGroup(ctx->device, &bg1_desc);

	// --- Group 2: fragment textures + samplers (rebuilt whenever a bitmap pool
	// is created or grown; empty pools bind the dummy array) ---
	rebuild_fragment_sampler_bg(ctx);

	// --- Compute bind groups ---
	size_t num_gradients = ctx->uninv_mat_data_size / (16 * sizeof(float));
	if (num_gradients > 0)
	{
		WGPUBindGroupEntry comp0_entry = {0};
		comp0_entry.binding = 0;
		comp0_entry.buffer = ctx->uninv_mat_buffer;
		comp0_entry.size = wgpuBufferGetSize(ctx->uninv_mat_buffer);

		WGPUBindGroupDescriptor comp0_desc = {0};
		comp0_desc.label = WGPU_LABEL("compute_read_bg");
		comp0_desc.layout = ctx->compute_read_bgl;
		comp0_desc.entryCount = 1;
		comp0_desc.entries = &comp0_entry;
		ctx->compute_read_bg = wgpuDeviceCreateBindGroup(ctx->device, &comp0_desc);

		WGPUBindGroupEntry comp1_entry = {0};
		comp1_entry.binding = 0;
		comp1_entry.buffer = ctx->inv_mat_buffer;
		comp1_entry.size = wgpuBufferGetSize(ctx->inv_mat_buffer);

		WGPUBindGroupDescriptor comp1_desc = {0};
		comp1_desc.label = WGPU_LABEL("compute_write_bg");
		comp1_desc.layout = ctx->compute_write_bgl;
		comp1_desc.entryCount = 1;
		comp1_desc.entries = &comp1_entry;
		ctx->compute_write_bg = wgpuDeviceCreateBindGroup(ctx->device, &comp1_desc);
	}
}

// ---------------------------------------------------------------------------
// run_compute_pass: invert the static gradient matrices (CPU).
//
// These were inverted in a GPU compute shader (mat4_inverse over the gradient
// matrix array). That works on real GPUs and lavapipe, but SwiftShader (the
// software WebGPU backend in WSL2 browsers) miscomputes it, leaving garbage in
// inv_mat_buffer → every static gradient's coordinate transform is wrong and
// the ramp smears across the whole stage (giant gray gradient over the menu).
// The matrices are simple 2D affines, the count is small (hundreds), and this
// runs once at init, so inverting them on the CPU — with the SAME
// invert_4x4_matrix the dynamic-gradient path already uses — is both correct on
// every adapter and negligibly cheap. The compute pipeline is left allocated
// but unused.
// ---------------------------------------------------------------------------
static void run_compute_pass(WebGPURenderContext* ctx)
{
	size_t num_gradients = ctx->uninv_mat_data_size / (16 * sizeof(float));
	if (num_gradients == 0) return;

	const float* src = (const float*)ctx->uninv_mat_data;
	float* inv = (float*)malloc(num_gradients * 16 * sizeof(float));
	if (!inv) return;
	for (size_t i = 0; i < num_gradients; i++)
	{
		if (!invert_4x4_matrix(src + i * 16, inv + i * 16))
		{
			// Singular — fall back to identity so the gradient is inert, not garbage.
			for (int j = 0; j < 16; j++) inv[i * 16 + j] = (j % 5 == 0) ? 1.0f : 0.0f;
		}
	}
	wgpuQueueWriteBuffer(ctx->queue, ctx->inv_mat_buffer, 0,
	                     inv, num_gradients * 16 * sizeof(float));
	free(inv);
}

// ---------------------------------------------------------------------------
// render_webgpu_poll: check for quit/mouse events
// ---------------------------------------------------------------------------
int render_webgpu_poll(SWFAppContext* app_context)
{
#ifdef OFFSCREEN_RENDER
	// Headless mode: no events to poll
	(void)app_context;
	return 0;
#elif defined(__EMSCRIPTEN__)
	// In WASM, mouse events are handled by callbacks registered in init.
	return 0;
#else
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		if (event.type == SDL_EVENT_QUIT)
			return 1;
		if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
			return 1;
		if (event.type == SDL_EVENT_MOUSE_MOTION)
		{
			app_context->mouse.stage_x = event.motion.x * 20.0f;
			app_context->mouse.stage_y = event.motion.y * 20.0f;
		}
		else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT)
		{
			app_context->mouse.button_down = 1;
			app_context->mouse.clicked = 1;
			app_context->mouse.stage_x = event.button.x * 20.0f;
			app_context->mouse.stage_y = event.button.y * 20.0f;
		}
		else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT)
		{
			app_context->mouse.button_down = 0;
			app_context->mouse.released = 1;
			app_context->mouse.stage_x = event.button.x * 20.0f;
			app_context->mouse.stage_y = event.button.y * 20.0f;
		}
	}
	return 0;
#endif
}

// ---------------------------------------------------------------------------
// render_webgpu_set_background
// ---------------------------------------------------------------------------
void render_webgpu_set_background(WebGPURenderContext* ctx, u8 r, u8 g, u8 b)
{
	ctx->red = r;
	ctx->green = g;
	ctx->blue = b;
}

// Overwrites the stage_to_ndc uniform buffer. Called from tag.c after
// render_webgpu_open_pass when _root has an AS-set transform to compose
// (e.g. _root._x = 200 shifting the whole scene).
void render_webgpu_upload_stage_transform(WebGPURenderContext* ctx, const float matrix[16])
{
	wgpuQueueWriteBuffer(ctx->queue, ctx->stage_to_ndc_buf, 0, matrix, 16 * sizeof(float));
}

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
// ---------------------------------------------------------------------------
// GPU frames-in-flight backpressure (browser-WASM).
// ---------------------------------------------------------------------------
// The browser render loop paces via emscripten_sleep (a wall-clock timer), NOT
// requestAnimationFrame, so it has no present/GPU backpressure. Under sustained
// GPU load (e.g. N's 825-tile level + particle overdraw) the loop submits one
// command buffer per iteration FASTER than the GPU drains them; the queue backs
// up without bound and display latency climbs until the screen updates <1fps
// while the CPU loop still spins at ~80fps (frame-CPU stays ~7ms — the cost is
// off-CPU, in the never-drained backlog). The screenshot path already documents
// and works around this same "GPU backlog" by parking the loop (browser_capture_
// finish). Bound it generally: register an OnSubmittedWorkDone callback per
// submit, and park at the start of the next frame until the GPU has caught up to
// within MAX_FRAMES_IN_FLIGHT. This caps display latency to ~2 frames and
// throttles the loop to the GPU's real sustainable rate (smooth N fps instead of
// a frozen <1fps). When the GPU keeps up (the common case / light scenes) the
// callback fires immediately and the park is a no-op, so no throughput penalty.
// AllowSpontaneous so the callback fires during the park's emscripten_sleep
// event-loop turns (the browser never calls wgpuInstanceProcessEvents — matches
// browser_capture_finish + the adapter/device requests).
//
// The park gives up after FRAME_PARK_QUIET_MS with NO callback at all: not
// "this park is long", but "the GPU has reported nothing for that long". The
// guard used to be 1000 emscripten_sleep(1) turns: ~4.4 s, because the browser
// clamps nested timers to 4 ms (and ~17 min in a background tab, where they
// clamp to 1 s). When callbacks stopped coming, every other frame paid it, then
// zeroed the count. Now a park that goes quiet marks the channel stalled: no
// more parks, so a channel that never answers costs one quiet period, once. The
// next successful callback revives it. Registration continues while stalled
// and the count is never reset, so it stays the true number of unfinished
// frames and the first park after a revival drains whatever backlog built up.
// A lost device (g_device_lost, above) stops the park and the registrations for
// good. window.__swfGpu.stalls counts stalls.
//
// Why "quiet", not a per-park cap: a slow live GPU is not a dead one. A per-park
// cap that zeroes the count and stops registering hides the backlog from the
// count, so draining it looks like more silence. Measured on headless
// SwiftShader with a live device (a frame completes in ~70 ms, early frames in
// up to ~1.8 s while pipelines compile): 12-18 stalls in 30 s with a 250 ms or
// 1 s cap, frames up to the cap. Even with the quiet rule, 1 s still tripped at
// startup there, and the revived park then paid the backlog back as one long
// frame. So the threshold sits well above any live GPU's slowest completion:
// it only has to catch a channel that never answers, and device LOSS (the
// case that actually happens) is handled at once by g_device_lost.
#define MAX_FRAMES_IN_FLIGHT 2
#define FRAME_PARK_QUIET_MS 3000.0
static volatile int g_frames_in_flight = 0;
static volatile int g_wd_stalled = 0;
static volatile double g_last_work_done_ms = 0.0;
static void on_frame_work_done(WGPUQueueWorkDoneStatus status, WGPUStringView message,
                               void* u1, void* u2)
{
	(void)message; (void)u1; (void)u2;
	if (g_frames_in_flight > 0) g_frames_in_flight--;
	g_last_work_done_ms = emscripten_get_now();
	if (status == WGPUQueueWorkDoneStatus_Success)
		g_wd_stalled = 0;
}
#endif

// ---------------------------------------------------------------------------
// render_webgpu_open_pass
// ---------------------------------------------------------------------------
void render_webgpu_open_pass(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	// Wait for the GPU to drain to within MAX_FRAMES_IN_FLIGHT before building a
	// new frame (see the backpressure note above). A GPU that goes quiet for
	// FRAME_PARK_QUIET_MS stalls the channel instead of being waited on again.
	if (!g_device_lost && !g_wd_stalled)
	{
		double park_t0 = emscripten_get_now();
		while (g_frames_in_flight >= MAX_FRAMES_IN_FLIGHT && !g_device_lost) {
			double quiet_since = g_last_work_done_ms > park_t0 ? g_last_work_done_ms : park_t0;
			if (emscripten_get_now() - quiet_since >= FRAME_PARK_QUIET_MS) {
				g_wd_stalled = 1;
				static int stall_logged = 0;
				if (!stall_logged) {
					stall_logged = 1;
					fprintf(stderr, "WebGPU: no frame completed in %.0f ms; frames stop "
					        "waiting on the GPU until one does\n", FRAME_PARK_QUIET_MS);
				}
				EM_ASM({
					var g = globalThis.__swfGpu || (globalThis.__swfGpu = { lost: 0, stalls: 0 });
					g.stalls++;
				});
				break;
			}
			emscripten_sleep(1);
		}
	}
#endif
	// Static pools are normally built by finalizeBitmaps in tag_init; build
	// them here if a movie defined bitmaps but never finalized.
	if (!ctx->bitmap_static_built && ctx->current_bitmap > 0)
		build_static_bitmap_pools(ctx);
	ctx->dynamic_rect_count = 0;
	ctx->dynamic_vertex_used = 0;
	ctx->dynamic_gradient_used = 0;
	ctx->dynamic_bitmap_used = 0;
	for (u32 i = BITMAP_STATIC_POOLS; i < BITMAP_POOL_COUNT; i++)
		ctx->bitmap_pools[i].used = 0;
	// Filter uniform slots only have to stay distinct WITHIN a frame (the frame's
	// commands have all run by the time the next frame's writes land), so the
	// ring restarts here rather than growing without bound.
	ctx->filter_uniform_cursor = 0;
	// main_color_texture is whatever this frame's MSAA resolve target is; the
	// complex blend modes snapshot it as their backdrop. NULL disables them.
	ctx->main_color_texture = NULL;
#ifdef OFFSCREEN_RENDER
	// Headless: use the persistent offscreen texture as resolve target
	ctx->surface_view = ctx->offscreen_view;
	ctx->main_color_texture = ctx->offscreen_texture;
#else
	ctx->browser_capture_active = 0;
#if defined(__EMSCRIPTEN__)
	if (ctx->browser_capture_state == 1 && ensure_browser_capture_resources(ctx))
	{
		// Capture frame: resolve into our own CopySrc texture instead of the
		// swapchain. We skip getCurrentTexture/present for this single frame
		// (the canvas simply doesn't update for one tick) so the readback is a
		// direct GPU copy that never touches the saturated present queue.
		ctx->surface_view = ctx->browser_capture_view;
		ctx->main_color_texture = ctx->browser_capture_texture;
		ctx->browser_capture_active = 1;
	}
	else
#endif
	{
		// Get the current surface texture
		WGPUSurfaceTexture surf_tex;
		wgpuSurfaceGetCurrentTexture(ctx->surface, &surf_tex);
		if (surf_tex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
		    surf_tex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
		{
			fprintf(stderr, "Failed to get surface texture\n");
			return;
		}

		ctx->surface_view = wgpuTextureCreateView(surf_tex.texture, NULL);
		ctx->main_color_texture = surf_tex.texture;
	}
#endif

	// Create command encoder
	WGPUCommandEncoderDescriptor enc_desc = {0};
	enc_desc.label = WGPU_LABEL("frame_encoder");
	ctx->encoder = wgpuDeviceCreateCommandEncoder(ctx->device, &enc_desc);

	// Begin render pass with MSAA
	WGPURenderPassColorAttachment color_att = {0};
	color_att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#if MSAA_SAMPLES == 1
	// No antialiasing: render straight into the destination, no resolve.
	color_att.view = ctx->surface_view;
	color_att.resolveTarget = NULL;
#else
	color_att.view = ctx->msaa_view;
	color_att.resolveTarget = ctx->surface_view;
#endif
	color_att.loadOp = WGPULoadOp_Clear;
	color_att.storeOp = WGPUStoreOp_Store; // Store so filters can suspend/resume pass
	color_att.clearValue = (WGPUColor){
		ctx->red / 255.0, ctx->green / 255.0, ctx->blue / 255.0, 1.0
	};

	WGPURenderPassDepthStencilAttachment ds_att = {0};
	ds_att.view = ctx->depth_stencil_view;
	ds_att.depthLoadOp = WGPULoadOp_Clear;
	ds_att.depthStoreOp = WGPUStoreOp_Discard;
	ds_att.depthClearValue = 1.0f;
	ds_att.stencilLoadOp = WGPULoadOp_Clear;
	ds_att.stencilStoreOp = WGPUStoreOp_Store; // Store for filter suspend/resume
	ds_att.stencilClearValue = 0;

	// The stencil starts at 0 everywhere, which IS nesting level 0. Drop any
	// clip state a previous pass left behind so the level and the texels agree.
	ctx->mask_ref = 0;
	ctx->mask_capture_depth = 0;
	ctx->mask_save_sp = 0;

	WGPURenderPassDescriptor rp_desc = {0};
	rp_desc.label = WGPU_LABEL("render_pass");
	rp_desc.colorAttachmentCount = 1;
	rp_desc.colorAttachments = &color_att;
	rp_desc.depthStencilAttachment = &ds_att;

	ctx->render_pass = wgpuCommandEncoderBeginRenderPass(ctx->encoder, &rp_desc);

	// Bind pipeline and static bind groups
	wgpuRenderPassEncoderSetPipeline(ctx->render_pass, ctx->render_pipeline);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 0, ctx->vertex_storage_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 2, ctx->fragment_sampler_bg, 0, NULL);

	// Upload stage_to_ndc uniform
	wgpuQueueWriteBuffer(ctx->queue, ctx->stage_to_ndc_buf, 0,
	                     ctx->stage_to_ndc, 16 * sizeof(float));
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 1, ctx->vertex_uniform_bg, 0, NULL);

	// Initialize extra_transform_id to 0, extra_transform to identity,
	// cxform_id to 0 (matching flashbang_open_pass behavior)
	u32 identity_id[4] = {0, 0, 0, 0};
	static const float identity_mat[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	static const float identity_cxform[20] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 0.0f, 0.0f
	};

	wgpuQueueWriteBuffer(ctx->queue, ctx->extra_transform_id_buf, 0, identity_id, 16);
	wgpuQueueWriteBuffer(ctx->queue, ctx->extra_transform_buf, 0, identity_mat, 64);
	wgpuQueueWriteBuffer(ctx->queue, ctx->cxform_id_buf, 0, identity_id, 16);
	wgpuQueueWriteBuffer(ctx->queue, ctx->cxform_uniform_buf, 0, identity_cxform, 80);
}

// ---------------------------------------------------------------------------
// mask_stencil_vert_count — ruffle omits STROKE geometry when a shape is
// rasterised into a mask stencil (render/src/tessellator.rs:149-160 "strokes
// need to be omitted when using this shape as a mask", and
// render/wgpu/src/surface/commands.rs:281-288, which swaps in num_mask_indices
// for the DrawMaskStencil / ClearMaskStencil states).
//
// SWFRecomp emits every fill group of a character first and every stroke run
// after (SWFRecomp/src/swf.cpp; stroke vertices are the only ones whose style
// word sets bit 31, 0x80000000), so the LEADING run of non-stroke vertices in a
// character's static vertex range is exactly ruffle's num_mask_indices. A
// stroke-ONLY masker keeps all of its vertices, matching ruffle's
// `unwrap_or(len)` fallback — otherwise it would mask away everything.
//
// Only STATIC geometry is trimmed: dynamic (drawing-API) vertices live past
// shape_data_size and already have their own fill-only helper
// (tag.c render_drawing_mc_paths_fill_only).
// ---------------------------------------------------------------------------
static size_t mask_stencil_vert_count(const WebGPURenderContext* ctx,
                                      size_t offset, size_t num_verts)
{
	if (ctx->mask_capture_depth == 0) return num_verts;
	if (ctx->shape_data == NULL || num_verts == 0) return num_verts;
	if (offset + num_verts > ctx->shape_data_size / (4 * sizeof(u32)))
		return num_verts;                      // dynamic range — not ours to trim
	const u32* sd = (const u32*) ctx->shape_data;
	size_t n = 0;
	while (n < num_verts && !(sd[(offset + n) * 4 + 2] & 0x80000000u)) n++;
	return (n > 0 && n < num_verts) ? n : num_verts;
}

// ---------------------------------------------------------------------------
// render_webgpu_draw_shape
// ---------------------------------------------------------------------------
void render_webgpu_draw_shape(WebGPURenderContext* ctx, size_t offset,
                              size_t num_verts, u32 transform_id, u32 cxform_id)
{
	if (!ctx->renderer_ok) return;
	// Ruffle rasterises only a masker's fill geometry into the stencil.
	num_verts = mask_stencil_vert_count(ctx, offset, num_verts);
	// Set vertex buffer with byte offset
	uint64_t byte_offset = offset * 4 * sizeof(u32);
	uint64_t byte_size = num_verts * 4 * sizeof(u32);
	wgpuRenderPassEncoderSetVertexBuffer(ctx->render_pass, 0, ctx->vertex_buffer,
	                                     byte_offset, byte_size);

	// Pack transform_id (low 16 bits) and cxform_id (high 16 bits) into
	// firstInstance. The vertex shader extracts both via @builtin(instance_index).
	// This avoids uniform buffer writes between draw calls, which don't take
	// effect until the next queue submit.
	u32 packed_id = transform_id | (cxform_id << 16);
	wgpuRenderPassEncoderDraw(ctx->render_pass, (u32)num_verts, 1, 0, packed_id);
}

// ---------------------------------------------------------------------------
// Dynamic vertex/color staging — accumulate into a CPU mirror of the dynamic
// region, flushed by render_webgpu_close_pass as one writeBuffer per buffer
// (instead of one tiny writeBuffer per shape). See dyn_vtx_staging in the header.
// ---------------------------------------------------------------------------
static inline void stage_dyn_verts(WebGPURenderContext* ctx, u32 vert_base,
	const void* verts, u32 vcount)
{
	if (ctx->dyn_vtx_staging == NULL) {   // staging unavailable — fall back to direct upload
		wgpuQueueWriteBuffer(ctx->queue, ctx->vertex_buffer,
			(uint64_t)vert_base * 4 * sizeof(u32), verts, (size_t)vcount * 4 * sizeof(u32));
		return;
	}
	memcpy(&ctx->dyn_vtx_staging[(size_t)(vert_base - ctx->dynamic_vertex_base) * 4],
		verts, (size_t)vcount * 4 * sizeof(u32));
}

static inline void stage_dyn_color(WebGPURenderContext* ctx, u32 color_idx,
	const float color[4])
{
	if (ctx->dyn_color_staging == NULL) {
		wgpuQueueWriteBuffer(ctx->queue, ctx->color_buffer,
			(uint64_t)color_idx * 4 * sizeof(float), color, 4 * sizeof(float));
		return;
	}
	float* dst = &ctx->dyn_color_staging[(size_t)(color_idx - ctx->dynamic_color_base) * 4];
	dst[0] = color[0]; dst[1] = color[1]; dst[2] = color[2]; dst[3] = color[3];
}

// ---------------------------------------------------------------------------
// render_webgpu_draw_rect — dynamic filled rectangle (for text field bg/border)
// ---------------------------------------------------------------------------
void render_webgpu_draw_rect(WebGPURenderContext* ctx,
	float x, float y, float w, float h,      // position/size in twips
	float r, float g, float b, float a,      // fill color (0-1)
	u32 transform_id, u32 cxform_id)
{
	if (!ctx->renderer_ok) return;
	if (ctx->dynamic_rect_count >= MAX_DYNAMIC_RECTS) return;
	if (ctx->dynamic_vertex_used + 6 > MAX_DYNAMIC_VERTICES) return;

	u32 idx = ctx->dynamic_rect_count++;
	u32 color_idx = ctx->dynamic_color_base + idx;
	u32 vert_base = ctx->dynamic_vertex_base + ctx->dynamic_vertex_used;
	ctx->dynamic_vertex_used += 6;

	// Stage color into the dynamic color slot (flushed once in close_pass)
	float color[4] = { r, g, b, a };
	stage_dyn_color(ctx, color_idx, color);

	// Generate 6 vertices (2 triangles) for the quad
	// Vertex format: { float x, float y, u32 style_x, u32 style_y }
	// style_x = 0x00 (solid color), style_y = color_idx
	union { float f; u32 u; } x0, y0, x1, y1;
	x0.f = x;       y0.f = y;
	x1.f = x + w;   y1.f = y + h;

	u32 sx = 0x00;   // solid color fill
	u32 sy = color_idx;

	u32 verts[6][4] = {
		{ x0.u, y0.u, sx, sy },  // top-left
		{ x1.u, y0.u, sx, sy },  // top-right
		{ x0.u, y1.u, sx, sy },  // bottom-left
		{ x1.u, y0.u, sx, sy },  // top-right
		{ x1.u, y1.u, sx, sy },  // bottom-right
		{ x0.u, y1.u, sx, sy },  // bottom-left
	};

	// Stage vertices into the dynamic area (flushed once in close_pass)
	stage_dyn_verts(ctx, vert_base, verts, 6);

	// Issue draw call
	render_webgpu_draw_shape(ctx, vert_base, 6, transform_id, cxform_id);
}

// ---------------------------------------------------------------------------
// Clip-mask stencil state helpers
// ---------------------------------------------------------------------------

// Bind the pipeline a normal (colour-writing) draw must use RIGHT NOW: the
// stencil-test pipeline at the active clip's reference while a clip mask is
// open, the plain render pipeline otherwise.
//
// EVERY site that clobbers the pipeline for its own draw has to come back
// through here instead of unconditionally re-binding render_pipeline — an
// unconditional re-bind drops the stencil test, so a bitmap, a blend-mode
// object or a filtered object drawn inside a mask silently lost its clip.
static void restore_draw_pipeline(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok || ctx->render_pass == NULL) return;
	// Inside a mask CAPTURE the "normal" pipeline is the stencil-WRITE one:
	// everything drawn between begin_clip_mask and end_clip_mask is masker
	// geometry, not content. Coming back to stencil_test here (mask_ref is
	// already the level being written) left the rest of the masker's subtree
	// testing a level nothing had raised yet — it vanished, and the mask ended
	// up with only the geometry drawn before the first pipeline clobber.
	// s17 P3: a Bitmap masker clobbers on its very first draw, so its stencil
	// was empty and the maskee was never clipped at all.
	if (ctx->mask_capture_depth > 0)
	{
		wgpuRenderPassEncoderSetPipeline(ctx->render_pass,
			ctx->stencil_write_pipeline);
		wgpuRenderPassEncoderSetStencilReference(ctx->render_pass,
			ctx->mask_ref > 0 ? ctx->mask_ref - 1 : 0);
		return;
	}
	if (ctx->mask_ref != 0)
	{
		wgpuRenderPassEncoderSetPipeline(ctx->render_pass, ctx->stencil_test_pipeline);
		wgpuRenderPassEncoderSetStencilReference(ctx->render_pass, ctx->mask_ref);
	}
	else
	{
		wgpuRenderPassEncoderSetPipeline(ctx->render_pass, ctx->render_pipeline);
	}
}

// Bind the premultiplied-alpha blend a BITMAP draw needs right now, honouring
// whatever stencil state is live (s17 P3). Three cases, and only the third was
// implemented before:
//
//   * mask CAPTURE open — the bitmap IS masker geometry. Leave the
//     stencil-write pipeline bound so the quad raises the stencil with colour
//     writes off. Ruffle does the same thing: `render_bitmap` under
//     MaskState::DrawMaskStencil picks the bitmap pipeline's mask variant, so a
//     Bitmap masker's whole quad becomes the mask window (its ALPHA is not
//     consulted — that is what separates the stencil arm from the alpha arm of
//     display_object.rs::get_render_mask).
//   * a mask is open and we are drawing CONTENT — test the stencil at the
//     active level.
//   * no mask — the plain premul pipeline, byte-for-byte the old behaviour.
static void bind_premul_pipeline(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok || ctx->render_pass == NULL) return;
	if (ctx->mask_capture_depth > 0)
	{
		wgpuRenderPassEncoderSetPipeline(ctx->render_pass,
			ctx->stencil_write_pipeline);
		wgpuRenderPassEncoderSetStencilReference(ctx->render_pass,
			ctx->mask_ref > 0 ? ctx->mask_ref - 1 : 0);
		return;
	}
	if (ctx->mask_ref != 0 && ctx->blend_premul_stencil_pipeline != NULL)
	{
		wgpuRenderPassEncoderSetPipeline(ctx->render_pass,
			ctx->blend_premul_stencil_pipeline);
		wgpuRenderPassEncoderSetStencilReference(ctx->render_pass, ctx->mask_ref);
		return;
	}
	wgpuRenderPassEncoderSetPipeline(ctx->render_pass, ctx->blend_premul_pipeline);
}

// Pop ONE clip-mask nesting level: draw the full-screen NDC quad through the
// Equal(level)/DecrementClamp pipeline, which touches exactly the texels the
// innermost mask raised and puts them back to level-1.
//
// The 8-bit stencil saturates at 255 by construction (IncrementClamp), so the
// old 255-reference allocator and its mid-pass full-screen rewrite are gone;
// masks 256-deep degrade to "the 255th mask wins" instead of aliasing. The
// busiest graded frame has 6 masks (from_shumway/invalidClipDepth's 257 clip
// ranges are one PER FRAME and are sibling ranges, so they never nest).
static void pop_mask_level(WebGPURenderContext* ctx)
{
	if (ctx->mask_ref == 0) return;
	if (ctx->render_pass != NULL)
	{
		wgpuRenderPassEncoderSetPipeline(ctx->render_pass, ctx->stencil_clear_pipeline);
		wgpuRenderPassEncoderSetStencilReference(ctx->render_pass, ctx->mask_ref);
		wgpuRenderPassEncoderDraw(ctx->render_pass, 6, 1, 0, 0);
	}
	ctx->mask_ref--;
}

// ---------------------------------------------------------------------------
// render_webgpu_draw_tris — arbitrary solid-color triangles (for Drawing API)
// ---------------------------------------------------------------------------
void render_webgpu_draw_tris(WebGPURenderContext* ctx,
	const float* xy_pairs, u32 vertex_count,   // pre-tessellated triangle verts in twips
	float r, float g, float b, float a,        // fill color (0-1)
	u32 transform_id, u32 cxform_id)
{
	if (!ctx->renderer_ok || vertex_count == 0 || xy_pairs == NULL) return;
	if (ctx->dynamic_rect_count >= MAX_DYNAMIC_RECTS) return;
	if (ctx->dynamic_vertex_used + vertex_count > MAX_DYNAMIC_VERTICES)
		vertex_count = MAX_DYNAMIC_VERTICES - ctx->dynamic_vertex_used;
	if (vertex_count == 0) return;

	u32 idx = ctx->dynamic_rect_count++;
	u32 color_idx = ctx->dynamic_color_base + idx;

	// Stage color into the dynamic color slot (flushed once in close_pass)
	float color[4] = { r, g, b, a };
	stage_dyn_color(ctx, color_idx, color);

	u32 vert_base = ctx->dynamic_vertex_base + ctx->dynamic_vertex_used;
	ctx->dynamic_vertex_used += vertex_count;

	// Build vertex data: { float x, float y, u32 style_x, u32 style_y } per vertex
	u32 sx = 0x00;   // solid color fill
	u32 sy = color_idx;
	u32* verts = (u32*)malloc(vertex_count * 4 * sizeof(u32));
	for (u32 i = 0; i < vertex_count; i++) {
		union { float f; u32 u; } xv, yv;
		xv.f = xy_pairs[i * 2];
		yv.f = xy_pairs[i * 2 + 1];
		verts[i * 4 + 0] = xv.u;
		verts[i * 4 + 1] = yv.u;
		verts[i * 4 + 2] = sx;
		verts[i * 4 + 3] = sy;
	}

	// Write vertices to the dynamic area
	stage_dyn_verts(ctx, vert_base, verts, vertex_count);
	free(verts);

	// Issue draw call
	render_webgpu_draw_shape(ctx, vert_base, vertex_count, transform_id, cxform_id);
}

// ---------------------------------------------------------------------------
// CPU-side 4x4 matrix inverse (for dynamic gradient matrices)
// ---------------------------------------------------------------------------
static int invert_4x4_matrix(const float m[16], float inv[16])
{
	// For our 2D affine matrices, only the 2x2 + translation part matters.
	// Full 4x4 inverse for generality:
	float a = m[0], b = m[1], c = m[4], d = m[5];
	float tx = m[12], ty = m[13];
	float det = a * d - b * c;
	if (det == 0.0f) return 0;  // singular
	float inv_det = 1.0f / det;
	// Zero-initialize
	for (int i = 0; i < 16; i++) inv[i] = 0.0f;
	inv[0]  =  d * inv_det;
	inv[1]  = -b * inv_det;
	inv[4]  = -c * inv_det;
	inv[5]  =  a * inv_det;
	inv[10] = 1.0f;
	inv[15] = 1.0f;
	inv[12] = -(inv[0] * tx + inv[4] * ty);
	inv[13] = -(inv[1] * tx + inv[5] * ty);
	return 1;
}

// ---------------------------------------------------------------------------
// render_webgpu_draw_gradient_tris — gradient-filled triangles (Drawing API)
// ---------------------------------------------------------------------------
void render_webgpu_draw_gradient_tris(WebGPURenderContext* ctx,
	const float* xy_pairs, u32 vertex_count,
	u8 gradient_type, u8 spread_mode, u8 interpolation, float focal_ratio,
	const u8* gradient_ramp, const float* gradient_matrix,
	u32 transform_id, u32 cxform_id)
{
	if (!ctx->renderer_ok || vertex_count == 0 || xy_pairs == NULL) return;
	if (ctx->dynamic_gradient_used >= ctx->dynamic_gradient_capacity) return;
	if (ctx->dynamic_vertex_used + vertex_count > MAX_DYNAMIC_VERTICES)
		vertex_count = MAX_DYNAMIC_VERTICES - ctx->dynamic_vertex_used;
	if (vertex_count == 0) return;

	// Allocate a gradient row (after all static gradients)
	u32 grad_id = ctx->static_gradient_count + ctx->dynamic_gradient_used;
	ctx->dynamic_gradient_used++;

	// Upload gradient ramp to this texture row
	{
		WGPUTexelCopyTextureInfo dest = {0};
		dest.texture = ctx->gradient_tex;
		dest.origin = (WGPUOrigin3D){0, grad_id, 0};
		WGPUTexelCopyBufferLayout layout = {0};
		layout.bytesPerRow = 256 * 4;
		layout.rowsPerImage = 1;
		WGPUExtent3D extent = {256, 1, 1};
		wgpuQueueWriteTexture(ctx->queue, &dest, gradient_ramp,
		                      256 * 4, &layout, &extent);
	}

	// Compute inverse of the gradient matrix on CPU, then compose normalization
	// so that the vertex shader outputs [0,1] UVs instead of [-16384, 16384].
	// This matches Ruffle's precision characteristics (operations near 1.0 instead of 16384).
	{
		float inv_mat[16];
		if (!invert_4x4_matrix(gradient_matrix, inv_mat)) {
			// Singular matrix — use identity as fallback
			for (int i = 0; i < 16; i++) inv_mat[i] = (i % 5 == 0) ? 1.0f : 0.0f;
		}
		// Compose normalization: uv = (grad_coord + 16384) / 32768
		// In matrix form: scale by 1/32768, translate by +0.5
		float s = 1.0f / 32768.0f;
		float norm_inv[16];
		for (int i = 0; i < 16; i++) norm_inv[i] = 0.0f;
		norm_inv[0]  = inv_mat[0]  * s;
		norm_inv[1]  = inv_mat[1]  * s;
		norm_inv[4]  = inv_mat[4]  * s;
		norm_inv[5]  = inv_mat[5]  * s;
		norm_inv[10] = 1.0f;
		norm_inv[12] = inv_mat[12] * s + 0.5f;
		norm_inv[13] = inv_mat[13] * s + 0.5f;
		norm_inv[15] = 1.0f;
		uint64_t mat_offset = (uint64_t)grad_id * 16 * sizeof(float);
		wgpuQueueWriteBuffer(ctx->queue, ctx->inv_mat_buffer, mat_offset,
			norm_inv, 16 * sizeof(float));
	}

	// Allocate dynamic vertices (shared counter with draw_rect/draw_tris)
	u32 vert_base = ctx->dynamic_vertex_base + ctx->dynamic_vertex_used;
	ctx->dynamic_vertex_used += vertex_count;

	// Style encoding: gradient_type | (spread_mode << 8) | (interpolation << 10)
	u32 sx = (u32)gradient_type | ((u32)spread_mode << 8) | ((u32)interpolation << 10);

	// Style Y: gradient_id in lower 16, focal_encoded in upper 16
	u16 focal_encoded = (u16)(focal_ratio * 16384.0f + 32768.0f);
	u32 sy = (u32)grad_id | ((u32)focal_encoded << 16);

	u32* verts = (u32*)malloc(vertex_count * 4 * sizeof(u32));
	for (u32 i = 0; i < vertex_count; i++) {
		union { float f; u32 u; } xv, yv;
		xv.f = xy_pairs[i * 2];
		yv.f = xy_pairs[i * 2 + 1];
		verts[i * 4 + 0] = xv.u;
		verts[i * 4 + 1] = yv.u;
		verts[i * 4 + 2] = sx;
		verts[i * 4 + 3] = sy;
	}

	stage_dyn_verts(ctx, vert_base, verts, vertex_count);
	free(verts);

	render_webgpu_draw_shape(ctx, vert_base, vertex_count, transform_id, cxform_id);
}

// ---------------------------------------------------------------------------
// render_webgpu_draw_bitmap_quad_scaled — render an attached BitmapData as a
// textured quad with separate source (texture) and destination (quad) sizes.
//
// src_w/src_h: pixel dims of argb_pixels — drives texture upload + UV range.
// dst_w/dst_h: on-stage pixel dims — drives quad twips. The shader sample-
// stretches via UV mapping when src != dst (Flash's Video render rule).
// When src == dst, equivalent to the original unscaled function.
// ---------------------------------------------------------------------------
void render_webgpu_draw_bitmap_quad_scaled(WebGPURenderContext* ctx,
	const uint32_t* argb_pixels, u32 src_w, u32 src_h,
	u32 dst_w, u32 dst_h,
	float x_twips, float y_twips,
	u32 transform_id, u32 cxform_id)
{
	if (!ctx->renderer_ok || argb_pixels == NULL) return;
	if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return;
	if (src_w > ctx->dynamic_bitmap_max_w || src_h > ctx->dynamic_bitmap_max_h) return;
	if (ctx->dynamic_vertex_used + 6 > MAX_DYNAMIC_VERTICES) return;

	// Allocate a dynamic slot + a pool layer that fits the source.
	u32 bmp_slot, bmp_pool, bmp_layer;
	if (!dynamic_bitmap_acquire(ctx, src_w, src_h, &bmp_slot, &bmp_pool, &bmp_layer))
		return;
	BitmapPool* pool = &ctx->bitmap_pools[bmp_pool];

	// Upload ARGB pixels as RGBA to the pool layer.
	//
	// This quad's inverse matrix maps the on-stage quad EXACTLY onto bitmap
	// coords [0, src] (see sx/sy below), and the sampler is Nearest +
	// ClampToEdge, so the shader only ever addresses texels [0, src] of the
	// layer. So upload just the (src+1)x(src+1) sub-region at the layer origin
	// — the +1 edge-clamp row/col covers the boundary texel — and leave the
	// rest of the layer untouched (never sampled, even though a previous frame
	// may have written it).
	{
		u32 bw = pool->w;
		u32 bh = pool->h;
		u32 up_w = (src_w + 1 <= bw) ? src_w + 1 : bw;
		u32 up_h = (src_h + 1 <= bh) ? src_h + 1 : bh;
		size_t up_bytes = (size_t)up_w * up_h * 4;
		u8* temp = (u8*)calloc(1, up_bytes);

		// Convert premultiplied ARGB to premultiplied RGBA with edge clamping.
		// Keep premultiplied — renderer uses premultiplied alpha blend mode.
		u32* dst = (u32*)temp;
		for (u32 y = 0; y < up_h; y++) {
			u32 sy = (y < src_h) ? y : src_h - 1;
			for (u32 x = 0; x < up_w; x++) {
				u32 sx_px = (x < src_w) ? x : src_w - 1;
				u32 argb = argb_pixels[sy * src_w + sx_px];
				// ARGB u32: A=bits31-24, R=bits23-16, G=bits15-8, B=bits7-0
				u32 a = (argb >> 24) & 0xFF;
				u32 r = (argb >> 16) & 0xFF;
				u32 g = (argb >> 8) & 0xFF;
				u32 b = argb & 0xFF;
				// RGBA bytes on little-endian: R=byte0, G=byte1, B=byte2, A=byte3
				dst[y * up_w + x] = r | (g << 8) | (b << 16) | (a << 24);
			}
		}

		WGPUTexelCopyTextureInfo dest = {0};
		dest.texture = pool->tex;
		dest.origin = (WGPUOrigin3D){0, 0, bmp_layer};
		WGPUTexelCopyBufferLayout layout = {0};
		layout.bytesPerRow = up_w * 4;
		layout.rowsPerImage = up_h;
		WGPUExtent3D extent = {up_w, up_h, 1};
		wgpuQueueWriteTexture(ctx->queue, &dest, temp, up_bytes, &layout, &extent);

		// Slot entry: {content, padded}, {pool, layer}. This quad is emitted as
		// style 0x43 (clipped, non-smoothed), so the shader normalizes by the
		// padded half — `inv_pos / padded`; the content half is recorded
		// truthfully for completeness.
		write_bitmap_slot(ctx, bmp_slot, src_w, src_h, bmp_pool, bmp_layer);

		free(temp);
	}

	// Compute and upload inverse matrix for UV mapping (twips → pixel coordinates).
	// Maps vertex position (x_twips, y_twips) → (0, 0) in bitmap space and
	// (x_twips + dst_w*20, y_twips + dst_h*20) → (src_w, src_h) in bitmap space.
	// Sample-stretch happens for free when src != dst.
	// Column-major 4x4: [sx, 0, 0, 0,  0, sy, 0, 0,  0, 0, 1, 0,  tx, ty, 0, 1]
	u32 inv_mat_id = ctx->static_mat_count + MAX_DYNAMIC_GRADIENTS + (bmp_slot - ctx->dynamic_bitmap_base);
	{
		float sx = (float)src_w / ((float)dst_w * 20.0f);
		float sy = (float)src_h / ((float)dst_h * 20.0f);
		float tx = -x_twips * sx;
		float ty = -y_twips * sy;
		float inv_mat[16] = {
			sx,   0.0f, 0.0f, 0.0f,
			0.0f, sy,   0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			tx,   ty,   0.0f, 1.0f
		};
		uint64_t mat_offset = (uint64_t)inv_mat_id * 16 * sizeof(float);
		wgpuQueueWriteBuffer(ctx->queue, ctx->inv_mat_buffer, mat_offset,
			inv_mat, 16 * sizeof(float));
	}

	// Generate 6 vertices (2 triangles) for the quad
	// Vertex format: { float x, float y, u32 style_type, u32 style_id }
	// style_type = 0x43 (clipped bitmap fill, no repeat, NOT smoothed)
	// style_id = bitmap slot (lower 16) | inv_mat_id (upper 16)
	//
	// This path (attachBitmap / Bitmap display objects / the video fallback)
	// carries no smoothing information, and Flash's default for both
	// Bitmap.smoothing and attachBitmap is false — so it asks for the Nearest
	// sampler explicitly. Before per-fill smoothing existed it emitted 0x41 and
	// got Nearest anyway from the single fixed sampler, so this keeps every such
	// draw byte-identical. Wire a real flag through here (not a literal) if
	// Bitmap.smoothing is ever honoured.
	float w_twips = (float)dst_w * 20.0f;
	float h_twips = (float)dst_h * 20.0f;

	union { float f; u32 u; } x0, y0, x1, y1;
	x0.f = x_twips;            y0.f = y_twips;
	x1.f = x_twips + w_twips;  y1.f = y_twips + h_twips;

	u32 sx = render_webgpu_bitmap_fill_style_word(0, 0);  // clipped, non-smoothed
	u32 sy = (bmp_slot & 0xFFFF) | ((inv_mat_id & 0xFFFF) << 16);

	u32 vert_base = ctx->dynamic_vertex_base + ctx->dynamic_vertex_used;
	ctx->dynamic_vertex_used += 6;

	u32 verts[6][4] = {
		{ x0.u, y0.u, sx, sy },  // top-left
		{ x1.u, y0.u, sx, sy },  // top-right
		{ x0.u, y1.u, sx, sy },  // bottom-left
		{ x1.u, y0.u, sx, sy },  // top-right
		{ x1.u, y1.u, sx, sy },  // bottom-right
		{ x0.u, y1.u, sx, sy },  // bottom-left
	};

	stage_dyn_verts(ctx, vert_base, verts, 6);

	// Switch to premultiplied alpha blending for bitmap rendering — through
	// bind_premul_pipeline so an open clip mask survives the switch.
	bind_premul_pipeline(ctx);
	render_webgpu_draw_shape(ctx, vert_base, 6, transform_id, cxform_id);
	// Restore normal blend pipeline — and the ACTIVE CLIP with it.
	restore_draw_pipeline(ctx);
}

// Unscaled wrapper — quad geometry matches source dims (no GPU sample-stretch).
// Used by attachBitmap and the video render fallback when declared bounds are 0.
void render_webgpu_draw_bitmap_quad(WebGPURenderContext* ctx,
	const uint32_t* argb_pixels, u32 bmp_width, u32 bmp_height,
	float x_twips, float y_twips,
	u32 transform_id, u32 cxform_id)
{
	render_webgpu_draw_bitmap_quad_scaled(ctx, argb_pixels,
		bmp_width, bmp_height, bmp_width, bmp_height,
		x_twips, y_twips, transform_id, cxform_id);
}

// ---------------------------------------------------------------------------
// render_webgpu_draw_bitmap_tris — bitmap-filled triangles for the Drawing
// API's beginBitmapFill. Source `argb_pixels` is uploaded as a
// (src_w+1) x (src_h+1) region at the pool layer's origin, with the +1
// row/column a WRAP copy (repeat=true — so a smoothed tile edge blends into
// the tile's own first texel) or an EDGE copy (repeat=false). The repeating
// arm of the shader tiles on the content size (fract), so it never leaves the
// region; the clipped arm sets style bit 11 so the shader clamps its UVs to
// the content (Flash extends the edge pixels of a clipped bitmap fill) instead
// of reading whatever a previous frame left in the rest of the layer.
// user_matrix6: a, b, c, d, tx_pixels, ty_pixels — maps bitmap pixel coords →
// MC local pixel coords (Flash semantics).
// ---------------------------------------------------------------------------
void render_webgpu_draw_bitmap_tris(WebGPURenderContext* ctx,
	const float* xy_pairs, u32 vertex_count,
	const uint32_t* argb_pixels, u32 src_w, u32 src_h,
	const float* user_matrix6, int repeat, int smooth,
	u32 transform_id, u32 cxform_id)
{
	if (!ctx->renderer_ok || vertex_count == 0 || xy_pairs == NULL) return;
	if (argb_pixels == NULL || src_w == 0 || src_h == 0) return;
	if (src_w > ctx->dynamic_bitmap_max_w || src_h > ctx->dynamic_bitmap_max_h) return;
	if (ctx->dynamic_vertex_used + vertex_count > MAX_DYNAMIC_VERTICES)
		vertex_count = MAX_DYNAMIC_VERTICES - ctx->dynamic_vertex_used;
	if (vertex_count == 0) return;

	u32 bmp_slot, bmp_pool, bmp_layer;
	if (!dynamic_bitmap_acquire(ctx, src_w, src_h, &bmp_slot, &bmp_pool, &bmp_layer))
		return;
	BitmapPool* pool = &ctx->bitmap_pools[bmp_pool];

	// (src+1)^2 region: wrap copy for repeat, edge copy for clipped.
	{
		u32 up_w = src_w + 1, up_h = src_h + 1;
		size_t up_bytes = (size_t)up_w * up_h * 4;
		u32* dst = (u32*)malloc(up_bytes);
		for (u32 y = 0; y < up_h; y++) {
			u32 sy;
			if (repeat) sy = y % src_h;
			else        sy = (y < src_h) ? y : src_h - 1;
			for (u32 x = 0; x < up_w; x++) {
				u32 sx;
				if (repeat) sx = x % src_w;
				else        sx = (x < src_w) ? x : src_w - 1;
				u32 argb = argb_pixels[sy * src_w + sx];
				u32 a = (argb >> 24) & 0xFF;
				u32 r = (argb >> 16) & 0xFF;
				u32 g = (argb >> 8)  & 0xFF;
				u32 b =  argb        & 0xFF;
				dst[y * up_w + x] = r | (g << 8) | (b << 16) | (a << 24);
			}
		}

		WGPUTexelCopyTextureInfo dest = {0};
		dest.texture = pool->tex;
		dest.origin = (WGPUOrigin3D){0, 0, bmp_layer};
		WGPUTexelCopyBufferLayout layout = {0};
		layout.bytesPerRow = up_w * 4;
		layout.rowsPerImage = up_h;
		WGPUExtent3D extent = {up_w, up_h, 1};
		wgpuQueueWriteTexture(ctx->queue, &dest, dst, up_bytes, &layout, &extent);

		// content = the source size: the repeat period for repeat=true, the
		// clamp box for repeat=false (style bit 11 below).
		write_bitmap_slot(ctx, bmp_slot, src_w, src_h, bmp_pool, bmp_layer);

		free(dst);
	}

	// Compose inv_mat: pos_twips → bitmap_pixel_coord in layer space.
	// Flash matrix M = [a, c, tx; b, d, ty] maps bitmap_pixel → mc_local_pixel.
	// M^-1 maps mc_local_pixel → bitmap_pixel. We also fold a (1/20) factor so
	// the matrix consumes pos_twips directly.
	u32 inv_mat_id = ctx->static_mat_count + MAX_DYNAMIC_GRADIENTS + (bmp_slot - ctx->dynamic_bitmap_base);
	{
		float a = user_matrix6[0], b = user_matrix6[1];
		float c = user_matrix6[2], d = user_matrix6[3];
		float tx = user_matrix6[4], ty = user_matrix6[5];
		float det = a * d - b * c;
		float inv_mat[16];
		for (int i = 0; i < 16; i++) inv_mat[i] = 0.0f;
		inv_mat[10] = 1.0f;
		inv_mat[15] = 1.0f;
		if (det != 0.0f) {
			float inv_det = 1.0f / det;
			float ai =  d * inv_det, bi = -b * inv_det;
			float ci = -c * inv_det, di =  a * inv_det;
			// Translation in inverse: -M^-1 * t (pixel space)
			float txi = -(ai * tx + ci * ty);
			float tyi = -(bi * tx + di * ty);
			// Combined matrix: (1/20) scale on the 2x2 part folds in the twips→pixel divide.
			inv_mat[0]  = ai / 20.0f;
			inv_mat[1]  = bi / 20.0f;
			inv_mat[4]  = ci / 20.0f;
			inv_mat[5]  = di / 20.0f;
			inv_mat[12] = txi;
			inv_mat[13] = tyi;
		} else {
			// Singular: collapse to (0,0) — fill samples bitmap origin
			inv_mat[0] = 0.0f; inv_mat[5] = 0.0f;
			inv_mat[12] = 0.0f; inv_mat[13] = 0.0f;
		}
		uint64_t mat_offset = (uint64_t)inv_mat_id * 16 * sizeof(float);
		wgpuQueueWriteBuffer(ctx->queue, ctx->inv_mat_buffer, mat_offset,
			inv_mat, 16 * sizeof(float));
	}

	// Generate triangle vertices. The fill-type byte carries BOTH the repeat and
	// the smoothing bit (0x40/0x41 smoothed, 0x42/0x43 not), which is how
	// beginBitmapFill's `smooth` argument reaches the fragment shader's sampler
	// choice — see render_webgpu_bitmap_fill_style_word.
	// Bit 11 (outside the SWF fill byte): clamp UVs to the content — dynamic
	// layers are reused across frames, so the clipped arm must not read past
	// the (src+1)^2 region uploaded above. Static fills never set it.
	u32 sx_word = render_webgpu_bitmap_fill_style_word(repeat, smooth)
	            | (repeat ? 0u : 0x800u);
	u32 sy_word = (bmp_slot & 0xFFFFu) | ((inv_mat_id & 0xFFFFu) << 16);

	u32 vert_base = ctx->dynamic_vertex_base + ctx->dynamic_vertex_used;
	ctx->dynamic_vertex_used += vertex_count;

	u32* verts = (u32*)malloc((size_t)vertex_count * 4 * sizeof(u32));
	for (u32 i = 0; i < vertex_count; i++) {
		union { float f; u32 u; } xv, yv;
		xv.f = xy_pairs[i * 2];
		yv.f = xy_pairs[i * 2 + 1];
		verts[i * 4 + 0] = xv.u;
		verts[i * 4 + 1] = yv.u;
		verts[i * 4 + 2] = sx_word;
		verts[i * 4 + 3] = sy_word;
	}
	stage_dyn_verts(ctx, vert_base, verts, vertex_count);
	free(verts);

	// Premultiplied blend (bitmap data is pre-multiplied via fillRect), through
	// bind_premul_pipeline so an open clip mask survives the switch.
	bind_premul_pipeline(ctx);
	render_webgpu_draw_shape(ctx, vert_base, vertex_count, transform_id, cxform_id);
	restore_draw_pipeline(ctx);
}

// ---------------------------------------------------------------------------
// Clip mask control: stencil-based clipping for PlaceObject2 clipDepth
// ---------------------------------------------------------------------------
void render_webgpu_begin_clip_mask(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;
	// A mask that is a SPRITE re-enters the display-list walk (tag.c's
	// g_clip_mask_capture branch), and a clipDepth entry INSIDE that subtree
	// calls begin/end_clip_mask again. Those inner pairs must not allocate a
	// second reference: the allocator would walk away from the outer mask, and
	// the inner end_clip_mask would switch COLOUR WRITES back on in the middle
	// of a mask capture. Everything drawn inside the capture belongs to the
	// same mask, so the inner pair is a no-op and the outer reference stands.
	if (ctx->mask_capture_depth++ > 0) return;
	// Ruffle's push_mask(): num_masks += 1, DrawMaskStencil at num_masks - 1
	// (render/wgpu/src/surface/commands.rs:399-433). The geometry drawn next
	// can therefore only raise texels the ENCLOSING mask owns.
	if (ctx->mask_ref < 255u) ctx->mask_ref++;
	// Switch to stencil-write pipeline: draws to stencil buffer only (no color)
	wgpuRenderPassEncoderSetPipeline(ctx->render_pass, ctx->stencil_write_pipeline);
	wgpuRenderPassEncoderSetStencilReference(ctx->render_pass, ctx->mask_ref - 1);
}

void render_webgpu_end_clip_mask(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;
	if (ctx->mask_capture_depth > 0) ctx->mask_capture_depth--;
	if (ctx->mask_capture_depth > 0) return;   // still capturing the outer mask
	// Ruffle's activate_mask(): DrawMaskedContent at num_masks. Content passes
	// only where the stencil reached THIS level, i.e. inside every mask that
	// is currently open.
	wgpuRenderPassEncoderSetPipeline(ctx->render_pass, ctx->stencil_test_pipeline);
	wgpuRenderPassEncoderSetStencilReference(ctx->render_pass, ctx->mask_ref);
}

void render_webgpu_end_clip(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;
	// A nested display-list walk under a sprite mask runs its own clip loop and
	// can call end_clip while the outer capture is still open; ignore it.
	if (ctx->mask_capture_depth > 0) return;
	// Pop exactly ONE level (ruffle's deactivate_mask + pop_mask), which is
	// what the callers that own a single range mean by "end". It used to zero
	// mask_ref outright and so DROPPED any enclosing clip; popping one level
	// restores it instead — the EditText field-bounds clip (tag.c:5005) and
	// masked_drawing_render_cb are nested pushes that now come back correctly.
	render_webgpu_restore_clip(ctx, ctx->mask_ref > 0 ? ctx->mask_ref - 1 : 0);
}

// Save / restore the active clip level around a NESTED mask.
//
// A mask can be opened inside an already-masked subtree — `a.mask = m` on a
// child of a clipDepth range, a clipDepth range inside a masked container, a
// scrollRect crop on top of a DisplayObject.mask — and every such pair saves
// the level with clip_ref() and gives it back with restore_clip().
//
// restore_clip is where the mask geometry is UNDONE: it pops one level per
// step, each pop a full-screen Equal(level)/DecrementClamp quad, so the
// stencil is left exactly as the enclosing mask had it. That is what makes
// the model composable — under the old Replace stencil a nested mask
// overwrote its parent's reference and restore_clip could put the reference
// back but not the texels, which is the hole the s13 maskC report described.
u32 render_webgpu_clip_ref(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return 0;
	return ctx->mask_ref;
}

void render_webgpu_restore_clip(WebGPURenderContext* ctx, u32 ref)
{
	if (!ctx->renderer_ok) return;
	if (ctx->mask_capture_depth > 0) return;
	// Every caller restores monotonically downward (tag.c:3469/3543/3683/3686/
	// 5920/6173/6905, avm2_display.c:15624/15635/15648/15654/15656); a raise is
	// a bug, not a case — clamp rather than pretend the texels came back.
	if (ref > ctx->mask_ref) ref = ctx->mask_ref;
	while (ctx->mask_ref > ref)
		pop_mask_level(ctx);
	restore_draw_pipeline(ctx);
}

void render_webgpu_set_blend_mode(WebGPURenderContext* ctx, u8 blend_mode)
{
	if (!ctx->renderer_ok) return;
	WGPURenderPipeline pipeline;
	switch (blend_mode)
	{
		case 5: pipeline = ctx->blend_lighten_pipeline; break;
		case 6: pipeline = ctx->blend_darken_pipeline; break;
		case 8: pipeline = ctx->blend_add_pipeline; break;
		case 9: pipeline = ctx->blend_subtract_pipeline; break;
		default:
			// Normal or unsupported — and this is also how the display loops
			// UNDO a blend mode (`set_blend_mode(context, 0)`), so it has to
			// come back to the active clip, not to a bare render_pipeline.
			restore_draw_pipeline(ctx);
			return;
	}
	// NOTE: the legacy per-draw blend pipelines carry no stencil test, so a
	// blend-mode object inside a clip mask is unclipped for the duration of its
	// draw. The layer-composite path (render_webgpu_composite_blend) DOES test
	// the stencil and is what the display loops use for non-trivial modes.
	wgpuRenderPassEncoderSetPipeline(ctx->render_pass, pipeline);
}

// ---------------------------------------------------------------------------
// render_webgpu_close_pass
// ---------------------------------------------------------------------------
void render_webgpu_close_pass(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;

	// Flush this frame's batched dynamic vertex/color staging — ONE writeBuffer per
	// buffer instead of one tiny writeBuffer per shape (the per-call overhead
	// dominated browser frame CPU). queue.writeBuffer is ordered before the submit
	// below, so the dynamic regions hold their final data before any recorded draw
	// executes. The used ranges are fully populated (each draw writes its slice
	// contiguously from offset 0), so flushing [0, used) is exact.
	// Retained-mode upload skip: a fully static screen (e.g. the Minesweeper
	// difficulty screen) re-tessellates and re-stages byte-identical dynamic
	// geometry every frame — ~281 KB/frame of vertices that never change. The
	// vertex/color buffers still hold the previous frame's bytes (we never clear
	// them), and the per-frame draw calls reference the same offsets, so when this
	// frame's staged content matches what was last uploaded we can skip the
	// writeBuffer entirely and the GPU reads the identical retained data →
	// pixel-identical output, zero re-upload. We compare against an exact CPU copy
	// of the last upload (memcmp, not a hash) so correctness is provable, and skip
	// the vertex and color regions independently. writeBuffer bytes were the only
	// frame-CPU cost that survives on a real GPU (the SwiftShader createView cost
	// is a software-present artifact), so cutting them is the lever toward 30fps.
	if (ctx->dyn_vtx_staging != NULL && ctx->dynamic_vertex_used > 0)
	{
		size_t vbytes = (size_t)ctx->dynamic_vertex_used * 4 * sizeof(u32);
		int unchanged = (ctx->prev_dyn_vtx != NULL &&
			ctx->prev_dyn_vtx_used == ctx->dynamic_vertex_used &&
			memcmp(ctx->prev_dyn_vtx, ctx->dyn_vtx_staging, vbytes) == 0);
		if (!unchanged)
		{
			wgpuQueueWriteBuffer(ctx->queue, ctx->vertex_buffer,
				(uint64_t)ctx->dynamic_vertex_base * 4 * sizeof(u32),
				ctx->dyn_vtx_staging, vbytes);
			if (ctx->prev_dyn_vtx != NULL)
			{
				memcpy(ctx->prev_dyn_vtx, ctx->dyn_vtx_staging, vbytes);
				ctx->prev_dyn_vtx_used = ctx->dynamic_vertex_used;
			}
		}
	}
	else { ctx->prev_dyn_vtx_used = 0; }  // empty frame: invalidate retained copy

	if (ctx->dyn_color_staging != NULL && ctx->dynamic_rect_count > 0)
	{
		size_t cbytes = (size_t)ctx->dynamic_rect_count * 4 * sizeof(float);
		int unchanged = (ctx->prev_dyn_color != NULL &&
			ctx->prev_dyn_rect_count == ctx->dynamic_rect_count &&
			memcmp(ctx->prev_dyn_color, ctx->dyn_color_staging, cbytes) == 0);
		if (!unchanged)
		{
			wgpuQueueWriteBuffer(ctx->queue, ctx->color_buffer,
				(uint64_t)ctx->dynamic_color_base * 4 * sizeof(float),
				ctx->dyn_color_staging, cbytes);
			if (ctx->prev_dyn_color != NULL)
			{
				memcpy(ctx->prev_dyn_color, ctx->dyn_color_staging, cbytes);
				ctx->prev_dyn_rect_count = ctx->dynamic_rect_count;
			}
		}
	}
	else { ctx->prev_dyn_rect_count = 0; }  // empty frame: invalidate retained copy

	wgpuRenderPassEncoderEnd(ctx->render_pass);

#ifdef OFFSCREEN_RENDER
	// Headless: copy offscreen texture to staging buffer for readback
	if (ctx->capture_requested)
	{
		WGPUTexelCopyTextureInfo src = {0};
		src.texture = ctx->offscreen_texture;
		src.mipLevel = 0;
		src.origin = (WGPUOrigin3D){0, 0, 0};
		src.aspect = WGPUTextureAspect_All;

		WGPUTexelCopyBufferInfo dst = {0};
		dst.buffer = ctx->readback_buffer;
		dst.layout.offset = 0;
		dst.layout.bytesPerRow = (uint32_t)ctx->readback_row_stride;
		dst.layout.rowsPerImage = ctx->height;

		WGPUExtent3D copy_size = {(uint32_t)ctx->width, (uint32_t)ctx->height, 1};
		wgpuCommandEncoderCopyTextureToBuffer(ctx->encoder, &src, &dst, &copy_size);
	}
#endif

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	// Browser debug capture: this frame resolved into browser_capture_texture
	// instead of the swapchain; copy it into the readback staging buffer in the
	// same command buffer (a direct GPU copy, no present involved).
	if (ctx->browser_capture_active)
	{
		WGPUTexelCopyTextureInfo src = {0};
		src.texture = ctx->browser_capture_texture;
		src.mipLevel = 0;
		src.origin = (WGPUOrigin3D){0, 0, 0};
		src.aspect = WGPUTextureAspect_All;

		WGPUTexelCopyBufferInfo dst = {0};
		dst.buffer = ctx->browser_capture_readback;
		dst.layout.offset = 0;
		dst.layout.bytesPerRow = (uint32_t)ctx->browser_capture_row_stride;
		dst.layout.rowsPerImage = ctx->height;

		WGPUExtent3D copy_size = {(uint32_t)ctx->width, (uint32_t)ctx->height, 1};
		wgpuCommandEncoderCopyTextureToBuffer(ctx->encoder, &src, &dst, &copy_size);
	}
#endif

	WGPUCommandBufferDescriptor cmd_desc = {0};
	WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(ctx->encoder, &cmd_desc);
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	double _submit_t0 = emscripten_get_now();
#endif
	wgpuQueueSubmit(ctx->queue, 1, &cmd);
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	// Publish this frame's render workload (writeBuffer calls/bytes, draw calls,
	// queue-submit time) and reset the per-frame counters for the next pass.
	swf_render_stats((double)g_perf_wb_calls, (double)g_perf_wb_bytes,
	                 (double)g_perf_draw_calls, emscripten_get_now() - _submit_t0,
	                 (double)g_perf_wt_calls, (double)g_perf_wt_bytes);
	g_perf_wb_calls = 0; g_perf_wb_bytes = 0; g_perf_draw_calls = 0;
	g_perf_wt_calls = 0; g_perf_wt_bytes = 0; FRAME_DIGEST_FLUSH();  // (frame_digest.h; keep on this line: no __LINE__ shift)
#endif

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	// Track this frame as in-flight; on_frame_work_done decrements when the GPU
	// finishes it, and render_webgpu_open_pass parks until we're within
	// MAX_FRAMES_IN_FLIGHT (see the backpressure note above). Without this the
	// timer-paced loop outruns the GPU and the present queue backs up unbounded.
	// Not on a lost device: each registration there is one unhandled promise
	// rejection in the page and a callback that never comes.
	if (!g_device_lost)
	{
		WGPUQueueWorkDoneCallbackInfo wd_info = {0};
		wd_info.mode = WGPUCallbackMode_AllowSpontaneous;
		wd_info.callback = on_frame_work_done;
		wgpuQueueOnSubmittedWorkDone(ctx->queue, wd_info);
		g_frames_in_flight++;
	}
#endif

#ifdef OFFSCREEN_RENDER
	// Headless: no surface to present
#elif !defined(__EMSCRIPTEN__)
	wgpuSurfacePresent(ctx->surface);
#endif

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	// This frame resolved into the capture texture and submitted the readback
	// copy. Finish the capture HERE, inside the render loop's ASYNCIFY context:
	// map the buffer and spin on emscripten_sleep until it completes. Parking
	// here pauses the main loop, so NO new frames are submitted while we wait —
	// the GPU backlog drains and our copy executes promptly (otherwise, on a
	// busy board, a steady stream of present/writeBuffer work starves the copy
	// and the map never fires; that is exactly why naive screenshots hang). This
	// is NOT re-entrant: close_pass runs from the single main-loop stack, and
	// dbgCapturePNG (the JS entry) only sets a flag and returns without sleeping.
	if (ctx->browser_capture_active)
	{
		ctx->browser_capture_active = 0;
		browser_capture_finish(ctx);   // state 1 -> 3 (or 0 on failure)
	}
#endif

	// Release per-frame objects
	wgpuCommandBufferRelease(cmd);
	wgpuRenderPassEncoderRelease(ctx->render_pass);
	wgpuCommandEncoderRelease(ctx->encoder);
#ifndef OFFSCREEN_RENDER
	// The capture view is persistent (reused across captures) — only the
	// per-frame swapchain view is created/released each frame.
	if (ctx->surface_view && ctx->surface_view != ctx->browser_capture_view)
		wgpuTextureViewRelease(ctx->surface_view);
#endif

	ctx->render_pass = NULL;
	ctx->encoder = NULL;
#ifndef OFFSCREEN_RENDER
	ctx->surface_view = NULL;
#endif
}

// ---------------------------------------------------------------------------
// Bitmap upload (called during init, before rendering starts)
// ---------------------------------------------------------------------------
// Records the bitmap's size and data offset; the GPU work happens in
// render_webgpu_finalize_bitmaps, once every static bitmap's size is known
// and the size-class pools can be dimensioned (see "Bitmap texture pools").
void render_webgpu_upload_bitmap(WebGPURenderContext* ctx, const u8* pixels,
                                 size_t size, u32 width, u32 height)
{
	(void) size;
	if (!ctx->renderer_ok || ctx->bitmap_static_built) return;
	// A loaded child's bitmaps never get here: the static pools are finalized at
	// the end of the ROOT movie's tagInit (bitmap_static_built above) and the
	// slot table is sized by the root's BITMAP_COUNT (below). `pixels` still
	// carries the defining movie's base so the slot table cannot inherit the
	// ambiguity if that ever changes. Measured on
	// regression/avm1_parent_child_bitmap: the root uploads 4x4 then finalizes,
	// then the child's 2x2 arrives with BOTH gates already closed
	// (built=1, cur=1 cnt=1) and every movie's tagInit calls finalizeBitmaps.
	// Lifting these two gates alone would render nothing: a child's SHAPE
	// vertices are not in the vertex buffer either (it is uploaded once from
	// the root's shape_data), and the bitmap index baked into a child's fill
	// style is movie-local. The whole feature is per-movie render tables --
	// SWFRecompDocs/status/child-embedded-asset-lookup.md, section
	// "Half B: what it actually needs".
	if (ctx->current_bitmap >= ctx->bitmap_count) return;

	// Every defineBitmap call owns the next slot (the recompiler numbers bitmap
	// ids in emission order), so a degenerate 0x0 bitmap still consumes its
	// index; build_static_bitmap_pools skips its upload.
	size_t i = ctx->current_bitmap;
	ctx->bitmap_ptrs[i] = pixels;
	ctx->bitmap_sizes[8 * i]     = width;
	ctx->bitmap_sizes[8 * i + 1] = height;
	ctx->bitmap_sizes[8 * i + 2] = 0;
	ctx->bitmap_sizes[8 * i + 3] = 0;
	ctx->bitmap_sizes[8 * i + 4] = 0;
	ctx->bitmap_sizes[8 * i + 5] = 0;
	ctx->bitmap_sizes[8 * i + 6] = 0;
	ctx->bitmap_sizes[8 * i + 7] = 0;
	ctx->current_bitmap++;
}

// Fill one static bitmap slot directly, bypassing the sequential
// `current_bitmap` cursor. This is how a LOADED CHILD movie's bitmaps get into
// the size-class pools: the pools are dimensioned once, at the root's
// finalizeBitmaps, and a child's own defineBitmap does not run until
// loadMovie -- so the child's slots are pre-declared from static descriptors
// (ng_predeclareChildBitmaps) before the root's tagInit ever runs.
//
// ctx->bitmap_count is the COMBINED count (ng_buildMovieRenderTables), so the
// child's slots are inside the table and inside bitmap_sizes_buffer; the root's
// own uploads keep the low slots and the cursor, untouched.
void render_webgpu_predeclare_bitmap(WebGPURenderContext* ctx, u32 slot,
                                     const u8* pixels, u32 width, u32 height)
{
	if (ctx == NULL || !ctx->renderer_ok || ctx->bitmap_static_built) return;
	if (pixels == NULL || slot >= (u32) ctx->bitmap_count) return;
	ctx->bitmap_ptrs[slot] = pixels;
	ctx->bitmap_sizes[8 * slot]     = width;
	ctx->bitmap_sizes[8 * slot + 1] = height;
	ctx->bitmap_sizes[8 * slot + 2] = 0;
	ctx->bitmap_sizes[8 * slot + 3] = 0;
	ctx->bitmap_sizes[8 * slot + 4] = 0;
	ctx->bitmap_sizes[8 * slot + 5] = 0;
	ctx->bitmap_sizes[8 * slot + 6] = 0;
	ctx->bitmap_sizes[8 * slot + 7] = 0;
	if (slot + 1 > ctx->bitmap_predeclared_end) ctx->bitmap_predeclared_end = slot + 1;
}

void render_webgpu_finalize_bitmaps(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok || ctx->bitmap_count == 0) return;
	build_static_bitmap_pools(ctx);
}

// ---------------------------------------------------------------------------
// Extra transform / cxform uploads (used for text rendering)
// Data is uploaded to GPU buffers (matching flashbang behavior) but not yet
// bound to bind groups or used by shaders. Shader/bind-group integration is
// deferred until both backends' shaders are updated.
// ---------------------------------------------------------------------------
void render_webgpu_upload_extra_transform_id(WebGPURenderContext* ctx, u32 transform_id)
{
	if (!ctx->renderer_ok) return;
	// Upload to GPU buffer (matching flashbang behavior).
	// Not yet bound to shaders — deferred until both backends' shaders are updated.
	u32 id_data[4] = {transform_id, 0, 0, 0};
	wgpuQueueWriteBuffer(ctx->queue, ctx->extra_transform_id_buf, 0, id_data, 16);
}

void render_webgpu_upload_extra_transform(WebGPURenderContext* ctx, float* transform)
{
	if (!ctx->renderer_ok) return;
	// Upload mat4 to GPU buffer (matching flashbang behavior).
	// Not yet bound to shaders — deferred until both backends' shaders are updated.
	wgpuQueueWriteBuffer(ctx->queue, ctx->extra_transform_buf, 0, transform, 16 * sizeof(float));
}

void render_webgpu_upload_cxform_id(WebGPURenderContext* ctx, u32 cxform_id)
{
	if (!ctx->renderer_ok) return;
	// Upload to GPU buffer (matching flashbang behavior).
	// Not yet bound to shaders — deferred until both backends' shaders are updated.
	u32 id_data[4] = {cxform_id, 0, 0, 0};
	wgpuQueueWriteBuffer(ctx->queue, ctx->cxform_id_buf, 0, id_data, 16);
}

void render_webgpu_upload_cxform(WebGPURenderContext* ctx, float* cxform)
{
	if (!ctx->renderer_ok) return;
	// Upload 20 floats (5x4 color transform) to GPU buffer (matching flashbang behavior).
	// Not yet bound to shaders — deferred until both backends' shaders are updated.
	wgpuQueueWriteBuffer(ctx->queue, ctx->cxform_uniform_buf, 0, cxform, 20 * sizeof(float));
}

// ---------------------------------------------------------------------------
// mat4_multiply: C = A * B (column-major 4x4)
// ---------------------------------------------------------------------------
static void mat4_multiply(float* out, const float* A, const float* B)
{
	for (int col = 0; col < 4; col++)
	{
		for (int row = 0; row < 4; row++)
		{
			out[col * 4 + row] =
				A[0 * 4 + row] * B[col * 4 + 0] +
				A[1 * 4 + row] * B[col * 4 + 1] +
				A[2 * 4 + row] * B[col * 4 + 2] +
				A[3 * 4 + row] * B[col * 4 + 3];
		}
	}
}

// ---------------------------------------------------------------------------
// xform_slot_write: write one composed 16-float matrix to GPU xform_buffer at
// `transform_id`, with the retained-skip described on xform_mirror in the header.
// Single choke point for ALL per-frame xform_buffer writers so the mirror stays
// consistent (compose_text/sprite/write_transform). Mirror NULL → always upload.
// ---------------------------------------------------------------------------
static inline void xform_slot_write(WebGPURenderContext* ctx, u32 transform_id,
                                    const float composed[16])
{
	if (ctx->xform_mirror != NULL && transform_id < ctx->xform_slot_count)
	{
		float* slot = &ctx->xform_mirror[(size_t)transform_id * 16];
		if (ctx->xform_mirror_valid[transform_id] &&
			memcmp(slot, composed, 16 * sizeof(float)) == 0)
			return;  // unchanged — GPU still holds identical bytes
		memcpy(slot, composed, 16 * sizeof(float));
		ctx->xform_mirror_valid[transform_id] = 1;
	}
	uint64_t offset = (uint64_t)transform_id * 16 * sizeof(float);
	wgpuQueueWriteBuffer(ctx->queue, ctx->xform_buffer, offset,
	                     composed, 16 * sizeof(float));
}

// ---------------------------------------------------------------------------
// render_webgpu_compose_text_transforms: compose PlaceObject2 transform with
// each glyph transform and write composed results to GPU xform_buffer.
// Called before renderer_open_pass so all writes happen before the render pass.
// Does NOT modify CPU-side transform_data (safe for multi-frame rendering).
// ---------------------------------------------------------------------------
void render_webgpu_compose_text_transforms(WebGPURenderContext* ctx,
                                            const char* transform_data,
                                            u32 place_transform_id,
                                            u32 glyph_start,
                                            size_t count)
{
	if (!ctx->renderer_ok) return;
	const float* transforms = (const float*)transform_data;
	const float* place_xform = &transforms[place_transform_id * 16];

	for (size_t i = 0; i < count; i++)
	{
		u32 glyph_xform_id = glyph_start + (u32)i;
		const float* glyph_xform = &transforms[glyph_xform_id * 16];

		float composed[16];
		mat4_multiply(composed, place_xform, glyph_xform);

		// Write composed transform to the GPU buffer at the glyph's slot
		xform_slot_write(ctx, glyph_xform_id, composed);
	}
}

// ---------------------------------------------------------------------------
// render_webgpu_compose_sprite_transform: compose parent PlaceObject2 transform
// with a single child transform and write the result to the child's xform slot
// in the GPU buffer.  Called before renderer_open_pass.
// ---------------------------------------------------------------------------
void render_webgpu_compose_sprite_transform(WebGPURenderContext* ctx,
                                            const char* transform_data,
                                            u32 parent_transform_id,
                                            u32 child_transform_id)
{
	if (!ctx->renderer_ok) return;
	const float* transforms = (const float*)transform_data;
	const float* parent_xform = &transforms[parent_transform_id * 16];
	const float* child_xform  = &transforms[child_transform_id * 16];

	float composed[16];
	mat4_multiply(composed, parent_xform, child_xform);

	xform_slot_write(ctx, child_transform_id, composed);
}

// ---------------------------------------------------------------------------
// render_webgpu_write_transform: write a pre-computed composed transform to the
// GPU xform_buffer at the given slot.  Used by tag.c's recursive composition.
// ---------------------------------------------------------------------------
void render_webgpu_write_transform(WebGPURenderContext* ctx,
                                   u32 transform_id, const float composed[16])
{
	if (!ctx->renderer_ok) return;
	xform_slot_write(ctx, transform_id, composed);
}

// ---------------------------------------------------------------------------
// render_webgpu_write_cxform: write a 20-float cxform entry to the GPU
// cxform_buffer at the given slot.  Used for runtime Color.setRGB/setTransform.
// ---------------------------------------------------------------------------
void render_webgpu_write_cxform(WebGPURenderContext* ctx,
                                u32 cxform_slot, const float cxform[20])
{
	if (!ctx->renderer_ok) return;
	uint64_t offset = (uint64_t)cxform_slot * 20 * sizeof(float);
	wgpuQueueWriteBuffer(ctx->queue, ctx->cxform_buffer, offset,
	                     cxform, 20 * sizeof(float));
}

// ---------------------------------------------------------------------------
// render_webgpu_update_vertices / render_webgpu_update_colors
// Write interpolated morph data into GPU buffers before the render pass.
// ---------------------------------------------------------------------------
void render_webgpu_update_vertices(WebGPURenderContext* ctx,
	size_t byte_offset, const void* data, size_t byte_size)
{
	if (!ctx->renderer_ok) return;
	wgpuQueueWriteBuffer(ctx->queue, ctx->vertex_buffer, byte_offset, data, byte_size);
}

void render_webgpu_update_colors(WebGPURenderContext* ctx,
	size_t byte_offset, const void* data, size_t byte_size)
{
	if (!ctx->renderer_ok) return;
	wgpuQueueWriteBuffer(ctx->queue, ctx->color_buffer, byte_offset, data, byte_size);
}

// ---------------------------------------------------------------------------
// Filter support: WGSL shaders, resource creation, blur + composite passes
// ---------------------------------------------------------------------------

// Flash's blur is a BOX blur, not a gaussian: one pass averages a window
// `full_size` texels wide, the two outermost texels carry a fractional weight,
// and the filter's `quality` counts how many times that box runs per axis.
// Weights and the linear-sampling fusion are ported from ruffle
// render/wgpu/shaders/filter/blur.wgsl + render/wgpu/src/filters/blur.rs,
// which follow fgiesen's "fast blurs 2" fractional-box formulation. The C-side
// half of the port is blur_box_kernel() next to render_webgpu_run_blur().
static const char* blur_wgsl =
	"struct Params {\n"
	"  direction: vec2f,\n"     // (1,0) for H, (0,1) for V
	"  texel_size: vec2f,\n"    // 1/width, 1/height
	"  full_size: f32,\n"       // box width in texels; the weights sum to this
	"  m: f32,\n"               // unit-weight texel PAIRS either side of centre
	"  m2: f32,\n"              // m * 2
	"  first_weight: f32,\n"    // fractional weight of the outermost texel
	"  last_offset: f32,\n"     // sub-texel offset that fuses the last pair
	"  last_weight: f32,\n"     // 1 + first_weight
	"  strength: f32,\n"
	"  colorize: f32,\n"        // 0=blur, 1=colorize alpha
	"  color: vec4f,\n"         // for colorize (glow/shadow)
	"  shift: vec2f,\n"         // whole-texel read offset (set_blur_shift)
	"  border: f32,\n"          // 1 = texels outside the layer read as 0
	"  shift_pad: f32,\n"
	"}\n"
	"@group(0) @binding(0) var in_tex: texture_2d<f32>;\n"
	"@group(0) @binding(1) var in_samp: sampler;\n"
	"@group(0) @binding(2) var<uniform> params: Params;\n"
	"\n"
	// A linear tap at `p` (along params.direction) with transparent texels
	// outside the layer instead of ClampToEdge's replicated edge. When exactly
	// one of the tap's two texels is outside, the clamped sample is that one
	// in-range texel, and the true value is it times its own bilinear weight;
	// both outside -> 0; both inside -> unchanged. Only a SHIFTED run sets
	// params.border: its first pass reads the capture at +offset, so it reaches
	// past the edge on purpose and must not smear the edge row/column inward.
	"fn tap(p: vec2f) -> vec4f {\n"
	"  let s = textureSample(in_tex, in_samp, p);\n"
	"  let dims = vec2f(textureDimensions(in_tex));\n"
	"  let c = dot(p * dims, params.direction) - 0.5;\n"
	"  let n = dot(dims, params.direction);\n"
	"  let i = floor(c);\n"
	"  let f = c - i;\n"
	"  let in0 = select(0.0, 1.0, i >= 0.0 && i < n);\n"
	"  let in1 = select(0.0, 1.0, i + 1.0 >= 0.0 && i + 1.0 < n);\n"
	"  let k = in0 * (1.0 - f) + in1 * f;\n"
	"  return select(s, s * k, params.border > 0.5 && !(in0 > 0.5 && in1 > 0.5));\n"
	"}\n"
	"\n"
	"struct VSOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f }\n"
	"\n"
	"@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VSOut {\n"
	"  var positions = array<vec2f, 6>(\n"
	"    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),\n"
	"    vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0)\n"
	"  );\n"
	"  var uvs = array<vec2f, 6>(\n"
	"    vec2f(0.0, 1.0), vec2f(1.0, 1.0), vec2f(0.0, 0.0),\n"
	"    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(1.0, 0.0)\n"
	"  );\n"
	"  var out: VSOut;\n"
	"  out.pos = vec4f(positions[vi], 0.0, 1.0);\n"
	"  out.uv = uvs[vi];\n"
	"  return out;\n"
	"}\n"
	"\n"
	"@fragment fn fs_main(@location(0) uv: vec2f) -> @location(0) vec4f {\n"
	"  let step = params.direction * params.texel_size;\n"
	// Integer texel shift (0 except on a shifted run's first H / first V pass):
	// out(x) = blur(in)(x + shift). A whole-texel move keeps the paired linear
	// taps below on texel boundaries, so the blur arithmetic is unchanged.
	"  let uv_s = uv + params.shift * params.texel_size;\n"
	// Pre-shift so the first unit-weight texel sits at offset 0. Ruffle does
	// this in the vertex stage; doing it here is the same arithmetic and keeps
	// the uniform buffer out of the vertex shader's visibility set.
	"  let base = uv_s - step * params.m;\n"
	"  var total = vec4f(0.0);\n"
	// The outermost texel on the low side, at the fractional weight.
	"  total += tap(base - step) * params.first_weight;\n"
	// The unit-weight interior: sampling exactly between two texels with a
	// linear sampler returns their average, so one sample x2 covers a pair.
	"  var center = vec4f(0.0);\n"
	"  for (var i = 0.5; i < params.m2; i = i + 2.0) {\n"
	"    center += tap(base + step * i);\n"
	"  }\n"
	"  total += center * 2.0;\n"
	// The last pair, fused into one sample: last_offset/last_weight are chosen
	// so texel m lands on weight 1 and texel m+1 on first_weight.
	"  total += tap(base + step * (params.m2 + params.last_offset)) * params.last_weight;\n"
	"  var result = total / params.full_size;\n"
	"  if (params.full_size > 1.0) {\n"
	// Imitate FP's fixed-point accumulator (ruffle blur.wgsl does the same).
	// Skipped for the full_size<=1 identity pass, where an 8-bit round-trip
	// through floor() could shave a level off an already-exact copy.
	"    result = floor(result * 255.0) / 255.0;\n"
	"  }\n"
	"  result = clamp(result * params.strength, vec4f(0.0), vec4f(1.0));\n"
	"  if (params.colorize > 0.5) {\n"
	"    let a2 = result.a * params.color.a;\n"
	"    result = vec4f(params.color.rgb * a2, a2);\n"  // pre-multiplied alpha
	"  }\n"
	"  return result;\n"
	"}\n";

static const char* composite_wgsl =
	"struct Params {\n"
	"  offset_and_mode: vec4f,\n"  // xy=NDC offset, z=tint_mode (0=passthrough, 1=tint), w=unused
	"  tint: vec4f,\n"             // RGBA tint color (used when tint_mode > 0.5)
	"}\n"
	"@group(0) @binding(0) var in_tex: texture_2d<f32>;\n"
	"@group(0) @binding(1) var in_samp: sampler;\n"
	"@group(0) @binding(2) var<uniform> params: Params;\n"
	"\n"
	"struct VSOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f }\n"
	"\n"
	"@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VSOut {\n"
	"  var positions = array<vec2f, 6>(\n"
	"    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),\n"
	"    vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0)\n"
	"  );\n"
	"  var uvs = array<vec2f, 6>(\n"
	"    vec2f(0.0, 1.0), vec2f(1.0, 1.0), vec2f(0.0, 0.0),\n"
	"    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(1.0, 0.0)\n"
	"  );\n"
	"  var out: VSOut;\n"
	"  out.pos = vec4f(positions[vi] + params.offset_and_mode.xy, 0.0, 1.0);\n"
	"  out.uv = uvs[vi];\n"
	"  return out;\n"
	"}\n"
	"\n"
	"@fragment fn fs_main(@location(0) uv: vec2f) -> @location(0) vec4f {\n"
	"  let c = textureSample(in_tex, in_samp, uv);\n"
	"  let a_tinted = c.a * params.tint.a;\n"
	"  let tinted = vec4f(params.tint.rgb * a_tinted, a_tinted);\n"
	"  return select(c, tinted, params.offset_and_mode.z > 0.5);\n"
	"}\n";

// Flash's glow/drop-shadow/bevel filters do not simply draw a blurred copy
// behind the object: the SWF filter flag byte carries inner, knockout and
// compositeSource bits that decide how the filter output is combined with the
// UNBLURRED source. This shader is a direct port of ruffle
// render/wgpu/shaders/filter/{glow,bevel}.wgsl, fused into one module with a
// `kind` selector, since we have a single composite pipeline slot.
//
// The blurred texture contributes only its ALPHA; strength and the filter
// colour are applied here (ruffle does the same), which is why
// render_webgpu_run_blur is now called with strength=1/colorize=0 for these
// filter kinds. `src_tex` is the snapshot taken by
// render_webgpu_snapshot_filter_source() before the blur ping-pong ran.
//
// compositeSource=0 is also how the caller asks for "filter output only" when
// it intends to draw the crisp source itself with a normal draw call — the
// over-blend then reproduces the compositeSource=1 formula exactly, so the
// common outer+compositeSource case keeps its MSAA-sharp source.
static const char* compose_wgsl =
	"struct Params {\n"
	"  color1: vec4f,\n"   // glow colour / bevel highlight (straight alpha in .a)
	"  color2: vec4f,\n"   // bevel shadow
	"  blur_off: vec4f,\n" // xy = UV offset added to the blurred sample
	"  mode: vec4f,\n"     // x=kind (0 glow, 1 bevel), y=variant, z=knockout, w=composite_source
	"  misc: vec4f,\n"     // x=strength
	"}\n"
	"@group(0) @binding(0) var blur_tex: texture_2d<f32>;\n"
	"@group(0) @binding(1) var in_samp: sampler;\n"
	"@group(0) @binding(2) var<uniform> params: Params;\n"
	"@group(0) @binding(3) var src_tex: texture_2d<f32>;\n"
	"\n"
	"struct VSOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f }\n"
	"\n"
	"@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VSOut {\n"
	"  var positions = array<vec2f, 6>(\n"
	"    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),\n"
	"    vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0)\n"
	"  );\n"
	"  var uvs = array<vec2f, 6>(\n"
	"    vec2f(0.0, 1.0), vec2f(1.0, 1.0), vec2f(0.0, 0.0),\n"
	"    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(1.0, 0.0)\n"
	"  );\n"
	"  var out: VSOut;\n"
	"  out.pos = vec4f(positions[vi], 0.0, 1.0);\n"
	"  out.uv = uvs[vi];\n"
	"  return out;\n"
	"}\n"
	"\n"
	// Returns 1 inside the texture, 0 outside. Everything outside the blurred
	// texture reads as fully transparent (ruffle zeroes it the same way; our
	// sampler is ClampToEdge, which would smear the border instead). The MASK
	// is computed here rather than an early return, because WGSL requires
	// textureSample to sit in uniform control flow: all three samples below are
	// taken unconditionally and only their VALUES are gated.
	"fn in_bounds(uv: vec2f) -> f32 {\n"
	"  let ok = uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;\n"
	"  return select(0.0, 1.0, ok);\n"
	"}\n"
	"\n"
	"@fragment fn fs_main(@location(0) uv: vec2f) -> @location(0) vec4f {\n"
	"  let knockout = params.mode.z > 0.5;\n"
	"  let composite_source = params.mode.w > 0.5;\n"
	"  let uv_l = uv + params.blur_off.xy;\n"
	"  let uv_r = uv - params.blur_off.xy;\n"
	"  let dest = textureSample(src_tex, in_samp, uv);\n"
	"  let blur_l = textureSample(blur_tex, in_samp, uv_l).a * in_bounds(uv_l);\n"
	"  let blur_r = textureSample(blur_tex, in_samp, uv_r).a * in_bounds(uv_r);\n"
	"  if (params.mode.x < 0.5) {\n"
	// ---- glow / drop shadow ----
	"    let blur = blur_l;\n"
	"    let inner = params.mode.y > 0.5;\n"
	"    var color = vec4f(params.color1.rgb, 1.0);\n"
	"    if (inner) {\n"
	"      let alpha = params.color1.a * saturate((1.0 - blur) * params.misc.x);\n"
	"      if (knockout) {\n"
	"        color = color * alpha * dest.a;\n"
	"      } else if (composite_source) {\n"
	"        color = color * alpha * dest.a + dest * (1.0 - alpha);\n"
	"      } else {\n"
	// Intentionally the same as knockout for inner; outer differs. Flash quirk,
	// documented in ruffle glow.wgsl.
	"        color = color * alpha * dest.a;\n"
	"      }\n"
	"    } else {\n"
	"      let alpha = params.color1.a * saturate(blur * params.misc.x);\n"
	"      if (knockout) {\n"
	"        color = color * alpha * (1.0 - dest.a);\n"
	"      } else if (composite_source) {\n"
	"        color = color * alpha * (1.0 - dest.a) + dest;\n"
	"      } else {\n"
	"        color = color * alpha;\n"
	"      }\n"
	"    }\n"
	"    return color;\n"
	"  }\n"
	// ---- bevel ----
	// The bevel is the DIFFERENCE of the blurred alpha sampled either side of
	// the light direction: positive difference lights, negative shades.
	"  let hc = vec4f(params.color1.rgb * params.color1.a, params.color1.a);\n"
	"  let sc = vec4f(params.color2.rgb * params.color2.a, params.color2.a);\n"
	"  let hi_a = saturate((blur_l - blur_r) * params.misc.x);\n"
	"  let sh_a = saturate((blur_r - blur_l) * params.misc.x);\n"
	"  let glow = hc * hi_a + sc * sh_a;\n"
	"  let is_outer = params.mode.y < 0.5 || params.mode.y > 1.5;\n"
	"  let is_inner = params.mode.y > 0.5;\n"
	"  if (is_inner && is_outer) {\n"
	"    if (knockout || !composite_source) { return glow; }\n"
	"    return dest - dest * glow.a + glow;\n"
	"  } else if (is_inner) {\n"
	"    if (knockout) { return glow * dest.a; }\n"
	"    if (composite_source) { return glow * dest.a + dest * (1.0 - glow.a); }\n"
	"    return glow * dest.a;\n"
	"  }\n"
	"  if (knockout) { return glow - glow * dest.a; }\n"
	"  if (composite_source) { return dest + glow - glow * dest.a; }\n"
	"  return glow;\n"
	"}\n";

// --- Filter uniform ring ---
// See the `filter_uniform_ring` comment in render_webgpu.h: a uniform buffer
// that is re-written between draws of the SAME frame does not work, because
// wgpuQueueWriteBuffer is ordered against Submit rather than against the
// commands already on the encoder. Every filter draw takes its own slot.
//
// This was the mechanism behind three separate wrong renders: the blur's H
// pass ran with the V pass's direction/kernel (so every blur was one-axis),
// a bevel's shadow composite ran with the highlight's tint, and every filter
// composition in a frame ran with the last object's inner/knockout flags.
#define FILTER_UNIFORM_SLOT   256u   // >= minUniformBufferOffsetAlignment
// A filtered object uses 2*quality blur slots + 1 composition slot, so 1024
// slots covers ~145 filtered objects in a single frame. Beyond that the ring
// wraps and the aliasing returns; no test in the corpus comes close.
#define FILTER_UNIFORM_SLOTS  1024u  // 256 KB

static u32 filter_uniform_push(WebGPURenderContext* ctx, const void* data, size_t size)
{
	u32 offset = ctx->filter_uniform_cursor * FILTER_UNIFORM_SLOT;
	ctx->filter_uniform_cursor = (ctx->filter_uniform_cursor + 1) % FILTER_UNIFORM_SLOTS;
	wgpuQueueWriteBuffer(ctx->queue, ctx->filter_uniform_ring, offset, data, size);
	return offset;
}

void render_webgpu_ensure_filter_resources(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;
	if (ctx->filter_resources_created) return;
	ctx->filter_resources_created = 1;

	// --- Filter textures (non-MSAA, RGBA8) ---
	WGPUTextureDescriptor ftex_desc = {0};
	ftex_desc.label = WGPU_LABEL("filter_tex_a");
	ftex_desc.dimension = WGPUTextureDimension_2D;
	ftex_desc.size = (WGPUExtent3D){(u32)ctx->width, (u32)ctx->height, 1};
	ftex_desc.format = ctx->surface_format;
	ftex_desc.mipLevelCount = 1;
	ftex_desc.sampleCount = 1;
	// CopyDst so filter_tex_b can receive the backdrop snapshot the complex blend
	// shaders sample (render_webgpu_capture_backdrop); CopySrc so filter_tex_a
	// can be snapshotted into filter_src_tex before the blur overwrites it.
	ftex_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding
	                | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
	ctx->filter_tex_a = wgpuDeviceCreateTexture(ctx->device, &ftex_desc);
	ftex_desc.label = WGPU_LABEL("filter_tex_b");
	ctx->filter_tex_b = wgpuDeviceCreateTexture(ctx->device, &ftex_desc);
	// The unblurred source the composition shader needs as `dest`.
	ftex_desc.label = WGPU_LABEL("filter_src");
	ftex_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	ctx->filter_src_tex = wgpuDeviceCreateTexture(ctx->device, &ftex_desc);

	WGPUTextureViewDescriptor fview_desc = {0};
	fview_desc.mipLevelCount = 1;
	fview_desc.arrayLayerCount = 1;
	ctx->filter_view_a = wgpuTextureCreateView(ctx->filter_tex_a, &fview_desc);
	ctx->filter_view_b = wgpuTextureCreateView(ctx->filter_tex_b, &fview_desc);
	ctx->filter_src_view = wgpuTextureCreateView(ctx->filter_src_tex, &fview_desc);

	// --- Separate MSAA texture for offscreen rendering ---
	{
		WGPUTextureDescriptor msaa_desc = {0};
		msaa_desc.label = WGPU_LABEL("filter_msaa");
		msaa_desc.dimension = WGPUTextureDimension_2D;
		msaa_desc.size = (WGPUExtent3D){(u32)ctx->width, (u32)ctx->height, 1};
		msaa_desc.format = ctx->surface_format;
		msaa_desc.mipLevelCount = 1;
		msaa_desc.sampleCount = MSAA_SAMPLES;
		msaa_desc.usage = WGPUTextureUsage_RenderAttachment;
		ctx->filter_msaa_texture = wgpuDeviceCreateTexture(ctx->device, &msaa_desc);
		ctx->filter_msaa_view = wgpuTextureCreateView(ctx->filter_msaa_texture, NULL);
	}

	// --- Filter sampler ---
	WGPUSamplerDescriptor samp_desc = {0};
	samp_desc.label = WGPU_LABEL("filter_sampler");
	samp_desc.addressModeU = WGPUAddressMode_ClampToEdge;
	samp_desc.addressModeV = WGPUAddressMode_ClampToEdge;
	samp_desc.magFilter = WGPUFilterMode_Linear;
	samp_desc.minFilter = WGPUFilterMode_Linear;
	samp_desc.maxAnisotropy = 1;
	ctx->filter_sampler = wgpuDeviceCreateSampler(ctx->device, &samp_desc);

	// --- Filter uniform ring (blur params, composite params, compose params) ---
	{
		WGPUBufferDescriptor buf_desc = {0};
		buf_desc.label = WGPU_LABEL("filter_uniform_ring");
		buf_desc.size = (u64)FILTER_UNIFORM_SLOT * FILTER_UNIFORM_SLOTS;
		buf_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
		ctx->filter_uniform_ring = wgpuDeviceCreateBuffer(ctx->device, &buf_desc);
		ctx->filter_uniform_cursor = 0;
	}

	// --- Blur pipeline ---
	{
		WGPUBindGroupLayoutEntry entries[3] = {0};
		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Fragment;
		entries[0].texture.sampleType = WGPUTextureSampleType_Float;
		entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Fragment;
		entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
		entries[2].binding = 2;
		entries[2].visibility = WGPUShaderStage_Fragment;
		entries[2].buffer.type = WGPUBufferBindingType_Uniform;
		entries[2].buffer.minBindingSize = 80;

		WGPUBindGroupLayoutDescriptor bgl_desc = {0};
		bgl_desc.entryCount = 3;
		bgl_desc.entries = entries;
		ctx->blur_bgl = wgpuDeviceCreateBindGroupLayout(ctx->device, &bgl_desc);

		WGPUPipelineLayoutDescriptor pl_desc = {0};
		pl_desc.bindGroupLayoutCount = 1;
		pl_desc.bindGroupLayouts = &ctx->blur_bgl;
		ctx->blur_pipeline_layout = wgpuDeviceCreatePipelineLayout(ctx->device, &pl_desc);

		WGPUShaderModule blur_sm = create_shader(ctx->device, blur_wgsl, "blur_shader");

		WGPUColorTargetState ct = {0};
		ct.format = ctx->surface_format;
		ct.writeMask = WGPUColorWriteMask_All;
		// Pre-multiplied alpha blending for blur output
		WGPUBlendState blend = {0};
		blend.color.srcFactor = WGPUBlendFactor_One;
		blend.color.dstFactor = WGPUBlendFactor_Zero;
		blend.color.operation = WGPUBlendOperation_Add;
		blend.alpha.srcFactor = WGPUBlendFactor_One;
		blend.alpha.dstFactor = WGPUBlendFactor_Zero;
		blend.alpha.operation = WGPUBlendOperation_Add;
		ct.blend = &blend;

		WGPUFragmentState fs = {0};
		fs.module = blur_sm;
		fs.entryPoint = WGPU_LABEL("fs_main");
		fs.targetCount = 1;
		fs.targets = &ct;

		WGPURenderPipelineDescriptor rpd = {0};
		rpd.label = WGPU_LABEL("blur_pipeline");
		rpd.layout = ctx->blur_pipeline_layout;
		rpd.vertex.module = blur_sm;
		rpd.vertex.entryPoint = WGPU_LABEL("vs_main");
		rpd.fragment = &fs;
		rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		rpd.multisample.count = 1;
		rpd.multisample.mask = 0xFFFFFFFF;
		ctx->blur_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rpd);
		wgpuShaderModuleRelease(blur_sm);
	}

	// --- Composite pipeline (draws into MSAA 4x main pass) ---
	{
		WGPUBindGroupLayoutEntry entries[3] = {0};
		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Fragment;
		entries[0].texture.sampleType = WGPUTextureSampleType_Float;
		entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Fragment;
		entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
		entries[2].binding = 2;
		entries[2].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entries[2].buffer.type = WGPUBufferBindingType_Uniform;
		entries[2].buffer.minBindingSize = 32;

		WGPUBindGroupLayoutDescriptor bgl_desc = {0};
		bgl_desc.entryCount = 3;
		bgl_desc.entries = entries;
		ctx->composite_bgl = wgpuDeviceCreateBindGroupLayout(ctx->device, &bgl_desc);

		WGPUPipelineLayoutDescriptor pl_desc = {0};
		pl_desc.bindGroupLayoutCount = 1;
		pl_desc.bindGroupLayouts = &ctx->composite_bgl;
		ctx->composite_pipeline_layout = wgpuDeviceCreatePipelineLayout(ctx->device, &pl_desc);

		WGPUShaderModule comp_sm = create_shader(ctx->device, composite_wgsl, "composite_shader");

		WGPUColorTargetState ct = {0};
		ct.format = ctx->surface_format;
		ct.writeMask = WGPUColorWriteMask_All;
		// Pre-multiplied alpha blending (over operator)
		WGPUBlendState blend = {0};
		blend.color.srcFactor = WGPUBlendFactor_One;
		blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend.color.operation = WGPUBlendOperation_Add;
		blend.alpha.srcFactor = WGPUBlendFactor_One;
		blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend.alpha.operation = WGPUBlendOperation_Add;
		ct.blend = &blend;

		WGPUFragmentState fs = {0};
		fs.module = comp_sm;
		fs.entryPoint = WGPU_LABEL("fs_main");
		fs.targetCount = 1;
		fs.targets = &ct;

		// Depth-stencil: test stencil (so clip masks work) but don't write
		WGPUDepthStencilState ds = {0};
		ds.format = WGPUTextureFormat_Depth24PlusStencil8;
		ds.depthWriteEnabled = 0;
		ds.depthCompare = WGPUCompareFunction_Always;
		ds.stencilFront.compare = WGPUCompareFunction_Equal;
		ds.stencilFront.failOp = WGPUStencilOperation_Keep;
		ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
		ds.stencilFront.passOp = WGPUStencilOperation_Keep;
		ds.stencilBack = ds.stencilFront;
		ds.stencilReadMask = 0xFF;
		ds.stencilWriteMask = 0x00;

		WGPURenderPipelineDescriptor rpd = {0};
		rpd.label = WGPU_LABEL("composite_pipeline");
		rpd.layout = ctx->composite_pipeline_layout;
		rpd.vertex.module = comp_sm;
		rpd.vertex.entryPoint = WGPU_LABEL("vs_main");
		rpd.fragment = &fs;
		rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		rpd.multisample.count = MSAA_SAMPLES;
		rpd.multisample.mask = 0xFFFFFFFF;
		rpd.depthStencil = &ds;
		ctx->composite_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rpd);
		wgpuShaderModuleRelease(comp_sm);
	}

	// --- Filter composition pipeline (inner / knockout / compositeSource) ---
	// Same target, blend and stencil configuration as composite_pipeline; the
	// only structural difference is the 4th binding (the source snapshot).
	{
		WGPUBindGroupLayoutEntry entries[4] = {0};
		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Fragment;
		entries[0].texture.sampleType = WGPUTextureSampleType_Float;
		entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Fragment;
		entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
		entries[2].binding = 2;
		entries[2].visibility = WGPUShaderStage_Fragment;
		entries[2].buffer.type = WGPUBufferBindingType_Uniform;
		entries[2].buffer.minBindingSize = 80;
		entries[3].binding = 3;
		entries[3].visibility = WGPUShaderStage_Fragment;
		entries[3].texture.sampleType = WGPUTextureSampleType_Float;
		entries[3].texture.viewDimension = WGPUTextureViewDimension_2D;

		WGPUBindGroupLayoutDescriptor bgl_desc = {0};
		bgl_desc.entryCount = 4;
		bgl_desc.entries = entries;
		ctx->compose_bgl = wgpuDeviceCreateBindGroupLayout(ctx->device, &bgl_desc);

		WGPUPipelineLayoutDescriptor pl_desc = {0};
		pl_desc.bindGroupLayoutCount = 1;
		pl_desc.bindGroupLayouts = &ctx->compose_bgl;
		ctx->compose_pipeline_layout = wgpuDeviceCreatePipelineLayout(ctx->device, &pl_desc);

		WGPUShaderModule sm = create_shader(ctx->device, compose_wgsl, "compose_shader");

		WGPUColorTargetState ct = {0};
		ct.format = ctx->surface_format;
		ct.writeMask = WGPUColorWriteMask_All;
		WGPUBlendState blend = {0};
		blend.color.srcFactor = WGPUBlendFactor_One;
		blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend.color.operation = WGPUBlendOperation_Add;
		blend.alpha.srcFactor = WGPUBlendFactor_One;
		blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend.alpha.operation = WGPUBlendOperation_Add;
		ct.blend = &blend;

		WGPUFragmentState fs = {0};
		fs.module = sm;
		fs.entryPoint = WGPU_LABEL("fs_main");
		fs.targetCount = 1;
		fs.targets = &ct;

		WGPUDepthStencilState ds = {0};
		ds.format = WGPUTextureFormat_Depth24PlusStencil8;
		ds.depthWriteEnabled = 0;
		ds.depthCompare = WGPUCompareFunction_Always;
		ds.stencilFront.compare = WGPUCompareFunction_Equal;
		ds.stencilFront.failOp = WGPUStencilOperation_Keep;
		ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
		ds.stencilFront.passOp = WGPUStencilOperation_Keep;
		ds.stencilBack = ds.stencilFront;
		ds.stencilReadMask = 0xFF;
		ds.stencilWriteMask = 0x00;

		WGPURenderPipelineDescriptor rpd = {0};
		rpd.label = WGPU_LABEL("compose_pipeline");
		rpd.layout = ctx->compose_pipeline_layout;
		rpd.vertex.module = sm;
		rpd.vertex.entryPoint = WGPU_LABEL("vs_main");
		rpd.fragment = &fs;
		rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		rpd.multisample.count = MSAA_SAMPLES;
		rpd.multisample.mask = 0xFFFFFFFF;
		rpd.depthStencil = &ds;
		ctx->compose_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rpd);
		wgpuShaderModuleRelease(sm);

		// --- Alpha-mask composite (s17 P3) -------------------------------
		// Ruffle's SECOND masking mechanism. display_object.rs::get_render_mask
		// picks RenderMask::Alpha over RenderMask::Stencil when the maskee AND
		// the masker are BOTH bitmap-cached, and apply_standard_mask_and_scroll
		// then renders the two into separate command lists and calls
		// commands.render_alpha_mask(maskee, mask) instead of pushing a stencil.
		// The wgpu backend draws each into its own texture and multiplies the
		// maskee by the mask's ALPHA — so a semi-transparent masker produces a
		// semi-transparent result, which a binary stencil can never express.
		//
		// Same pipeline layout, blend and stencil state as compose_pipeline;
		// binding 0 is the maskee layer, binding 3 the mask layer, and the
		// uniform at binding 2 is unused (the layout still requires it to be
		// bound). Both layers are premultiplied, so scaling all four channels
		// by the mask's alpha keeps the result premultiplied and the ordinary
		// One / OneMinusSrcAlpha over-blend below is correct.
		static const char* alpha_mask_wgsl =
			"@group(0) @binding(0) var maskee_tex: texture_2d<f32>;\n"
			"@group(0) @binding(1) var in_samp: sampler;\n"
			"@group(0) @binding(3) var mask_tex: texture_2d<f32>;\n"
			"struct VSOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f }\n"
			"@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VSOut {\n"
			"  var positions = array<vec2f, 6>(\n"
			"    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),\n"
			"    vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0)\n"
			"  );\n"
			"  var uvs = array<vec2f, 6>(\n"
			"    vec2f(0.0, 1.0), vec2f(1.0, 1.0), vec2f(0.0, 0.0),\n"
			"    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(1.0, 0.0)\n"
			"  );\n"
			"  var out: VSOut;\n"
			"  out.pos = vec4f(positions[vi], 0.0, 1.0);\n"
			"  out.uv = uvs[vi];\n"
			"  return out;\n"
			"}\n"
			"@fragment fn fs_main(@location(0) uv: vec2f) -> @location(0) vec4f {\n"
			"  let c = textureSample(maskee_tex, in_samp, uv);\n"
			"  let m = textureSample(mask_tex, in_samp, uv);\n"
			"  return c * m.a;\n"
			"}\n";

		WGPUShaderModule am_sm = create_shader(ctx->device, alpha_mask_wgsl,
		                                       "alpha_mask_shader");
		WGPUFragmentState am_fs = fs;
		am_fs.module = am_sm;
		rpd.label = WGPU_LABEL("alpha_mask_pipeline");
		rpd.vertex.module = am_sm;
		rpd.fragment = &am_fs;
		ctx->alpha_mask_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rpd);
		wgpuShaderModuleRelease(am_sm);
	}
}

// ---------------------------------------------------------------------------
// Blend-mode LAYER compositing
// ---------------------------------------------------------------------------
// Flash does NOT blend each of an object's draw calls against the live backdrop.
// It renders the object's whole subtree into a transparent layer and composites
// that layer against the backdrop exactly once (ruffle
// render/wgpu/src/surface/commands.rs:679-790). Doing it per draw makes an
// object's own overlapping sub-shapes blend with themselves — that was the
// blown-out overlap inside the wordmark in visual/blend_modes/add.
//
// Three of the fourteen modes composite with fixed hardware blend factors
// ("trivial", ruffle render/wgpu/src/blend.rs:76-105); seven need a shader that
// samples BOTH the layer and the backdrop ("complex",
// render/wgpu/shaders/blend/*.wgsl). Alpha (11) and Erase (12) are handled by
// the caller: with no Layer above them Flash ignores the object entirely
// (surface.rs:239-244), and we have no layer groups yet, so that is always true.
// Layer (2) itself still falls through to normal.
//
// The layer texture is filter_tex_a, which begin_offscreen_pass clears to
// (0,0,0,0) and draws into with SrcAlpha/OneMinusSrcAlpha — so it comes out
// PREMULTIPLIED, exactly ruffle's layer surface.

static int blend_mode_is_trivial_layer(u8 m) { return m == 4 || m == 8 || m == 9; }
static int blend_mode_is_complex(u8 m)
{
	return m == 3 || m == 5 || m == 6 || m == 7 || m == 10 || m == 13 || m == 14;
}

int render_webgpu_blend_mode_is_layered(WebGPURenderContext* ctx, u8 blend_mode)
{
	if (!ctx->renderer_ok) return 0;
	// Nested layers would need a texture pool; an inner blend object falls back to
	// the legacy per-draw pipeline rather than clobbering the outer layer.
	if (ctx->offscreen_depth > 0) return 0;
	if (blend_mode_is_trivial_layer(blend_mode)) return 1;
	// A complex blend samples the backdrop, so the resolved main colour target has
	// to be copyable. It always is headless; in the browser it depends on the
	// surface configuration.
	if (blend_mode_is_complex(blend_mode)) return ctx->main_color_texture != NULL;
	return 0;
}

// Fragment stage of the seven complex blends, ported from ruffle
// render/wgpu/shaders/blend/{multiply,lighten,darken,difference,invert,overlay,
// hardlight}.wgsl. Both inputs are premultiplied; `s`/`d` are the
// un-premultiplied colours the Flash blend function is defined on.
//
// `discard` where the layer is empty is what lets these run with NO hardware
// blending (ruffle's BlendState::REPLACE) without erasing the frame outside the
// object. dst.a == 0 returns src, which is exactly the limit of compose() there
// and avoids 0/0 in dst.rgb/dst.a.
static const char* blend_shader_wgsl_head =
	"@group(0) @binding(0) var parent_tex: texture_2d<f32>;\n"
	"@group(0) @binding(1) var current_tex: texture_2d<f32>;\n"
	"@group(0) @binding(2) var tex_samp: sampler;\n"
	"\n"
	"struct VSOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f }\n"
	"\n"
	"@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VSOut {\n"
	"  var positions = array<vec2f, 6>(\n"
	"    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),\n"
	"    vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0)\n"
	"  );\n"
	"  var uvs = array<vec2f, 6>(\n"
	"    vec2f(0.0, 1.0), vec2f(1.0, 1.0), vec2f(0.0, 0.0),\n"
	"    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(1.0, 0.0)\n"
	"  );\n"
	"  var out: VSOut;\n"
	"  out.pos = vec4f(positions[vi], 0.0, 1.0);\n"
	"  out.uv = uvs[vi];\n"
	"  return out;\n"
	"}\n"
	"\n"
	"fn compose(src: vec4f, dst: vec4f, b: vec3f) -> vec4f {\n"
	"  return vec4f(src.rgb * (1.0 - dst.a) + dst.rgb * (1.0 - src.a) + src.a * dst.a * b,\n"
	"               src.a + dst.a * (1.0 - src.a));\n"
	"}\n"
	"fn hard_mix(s: vec3f, d: vec3f, sel: vec3<bool>) -> vec3f {\n"
	"  return select(vec3f(1.0) - 2.0 * (vec3f(1.0) - d) * (vec3f(1.0) - s), 2.0 * s * d, sel);\n"
	"}\n";

// %s = entry-point name, %s = blend function over `s` (src) and `d` (dst)
static const char* blend_shader_wgsl_frag =
	"@fragment fn %s(@location(0) uv: vec2f) -> @location(0) vec4f {\n"
	"  let dst = textureSample(parent_tex, tex_samp, uv);\n"
	"  let src = textureSample(current_tex, tex_samp, uv);\n"
	"  if (src.a > 0.0) {\n"
	"    if (dst.a > 0.0) {\n"
	"      let s = src.rgb / src.a;\n"
	"      let d = dst.rgb / dst.a;\n"
	"      return compose(src, dst, %s);\n"
	"    } else {\n"
	"      return src;\n"
	"    }\n"
	"  } else {\n"
	"    discard;\n"
	"  }\n"
	"  return dst;\n"
	"}\n";

typedef struct { u8 mode; const char* entry; const char* func; } BlendShaderDef;

static const BlendShaderDef blend_shader_defs[] = {
	{  3, "fs_multiply",   "s * d" },
	{  5, "fs_lighten",    "max(s, d)" },
	{  6, "fs_darken",     "min(s, d)" },
	{  7, "fs_difference", "abs(d - s)" },
	{ 10, "fs_invert",     "vec3f(1.0) - d" },
	{ 13, "fs_overlay",    "hard_mix(s, d, d <= vec3f(0.5))" },
	{ 14, "fs_hardlight",  "hard_mix(s, d, s <= vec3f(0.5))" },
};
#define BLEND_SHADER_COUNT ((int)(sizeof(blend_shader_defs) / sizeof(blend_shader_defs[0])))

static void render_webgpu_ensure_blend_resources(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;
	if (ctx->blend_resources_created) return;
	render_webgpu_ensure_filter_resources(ctx);
	ctx->blend_resources_created = 1;

	// The offscreen pass needs its OWN depth-stencil (see begin_offscreen_pass).
	{
		WGPUTextureDescriptor td = {0};
		td.label = WGPU_LABEL("filter_depth_stencil");
		td.dimension = WGPUTextureDimension_2D;
		td.size = (WGPUExtent3D){(u32)ctx->width, (u32)ctx->height, 1};
		td.format = WGPUTextureFormat_Depth24PlusStencil8;
		td.mipLevelCount = 1;
		td.sampleCount = MSAA_SAMPLES;
		td.usage = WGPUTextureUsage_RenderAttachment;
		ctx->filter_ds_texture = wgpuDeviceCreateTexture(ctx->device, &td);
		WGPUTextureViewDescriptor vd = {0};
		vd.mipLevelCount = 1;
		vd.arrayLayerCount = 1;
		ctx->filter_ds_view = wgpuTextureCreateView(ctx->filter_ds_texture, &vd);
	}

	// composite_wgsl's params, permanently zero (no offset, no tint). A dedicated
	// buffer keeps blend composites independent of the filter path's per-call
	// writes into the filter uniform ring.
	{
		WGPUBufferDescriptor bd = {0};
		bd.label = WGPU_LABEL("blend_params");
		bd.size = 32;
		bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
		ctx->blend_params_buf = wgpuDeviceCreateBuffer(ctx->device, &bd);
		float zeros[8] = {0};
		wgpuQueueWriteBuffer(ctx->queue, ctx->blend_params_buf, 0, zeros, 32);
	}

	// --- Trivial layer composites: composite_pipeline with a different blend
	//     state. Colour factors from ruffle blend.rs:76-105; alpha is OVER on all
	//     three, so an opaque backdrop stays opaque. ---
	{
		WGPUShaderModule sm = create_shader(ctx->device, composite_wgsl, "blend_layer_shader");

		WGPUColorTargetState ct = {0};
		ct.format = ctx->surface_format;
		ct.writeMask = WGPUColorWriteMask_All;

		WGPUFragmentState fs = {0};
		fs.module = sm;
		fs.entryPoint = WGPU_LABEL("fs_main");
		fs.targetCount = 1;
		fs.targets = &ct;

		// Same stencil-Equal / no-write depth-stencil as composite_pipeline, so a
		// blend-mode object inside a clip mask stays clipped.
		WGPUDepthStencilState ds = {0};
		ds.format = WGPUTextureFormat_Depth24PlusStencil8;
		ds.depthWriteEnabled = 0;
		ds.depthCompare = WGPUCompareFunction_Always;
		ds.stencilFront.compare = WGPUCompareFunction_Equal;
		ds.stencilFront.failOp = WGPUStencilOperation_Keep;
		ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
		ds.stencilFront.passOp = WGPUStencilOperation_Keep;
		ds.stencilBack = ds.stencilFront;
		ds.stencilReadMask = 0xFF;
		ds.stencilWriteMask = 0x00;

		WGPURenderPipelineDescriptor rpd = {0};
		rpd.layout = ctx->composite_pipeline_layout;
		rpd.vertex.module = sm;
		rpd.vertex.entryPoint = WGPU_LABEL("vs_main");
		rpd.fragment = &fs;
		rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		rpd.multisample.count = MSAA_SAMPLES;
		rpd.multisample.mask = 0xFFFFFFFF;
		rpd.depthStencil = &ds;

		static const struct {
			u8 mode; WGPUBlendFactor dst; WGPUBlendOperation op; const char* label;
		} trivial[] = {
			{ 4, WGPUBlendFactor_OneMinusSrc, WGPUBlendOperation_Add,             "blend_layer_screen" },
			{ 8, WGPUBlendFactor_One,         WGPUBlendOperation_Add,             "blend_layer_add" },
			{ 9, WGPUBlendFactor_One,         WGPUBlendOperation_ReverseSubtract, "blend_layer_subtract" },
		};
		for (int i = 0; i < 3; i++)
		{
			WGPUBlendState bs = {0};
			bs.color.srcFactor = WGPUBlendFactor_One;
			bs.color.dstFactor = trivial[i].dst;
			bs.color.operation = trivial[i].op;
			bs.alpha.srcFactor = WGPUBlendFactor_One;
			bs.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
			bs.alpha.operation = WGPUBlendOperation_Add;
			ct.blend = &bs;
			rpd.label = WGPU_LABEL(trivial[i].label);
			ctx->blend_layer_pipeline[trivial[i].mode] =
				wgpuDeviceCreateRenderPipeline(ctx->device, &rpd);
		}
		wgpuShaderModuleRelease(sm);
	}

	// --- Complex blends: one pipeline per mode, no hardware blending. ---
	{
		WGPUBindGroupLayoutEntry entries[3] = {0};
		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Fragment;
		entries[0].texture.sampleType = WGPUTextureSampleType_Float;
		entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Fragment;
		entries[1].texture.sampleType = WGPUTextureSampleType_Float;
		entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[2].binding = 2;
		entries[2].visibility = WGPUShaderStage_Fragment;
		entries[2].sampler.type = WGPUSamplerBindingType_Filtering;

		WGPUBindGroupLayoutDescriptor bgl_desc = {0};
		bgl_desc.label = WGPU_LABEL("blend_shader_bgl");
		bgl_desc.entryCount = 3;
		bgl_desc.entries = entries;
		ctx->blend_shader_bgl = wgpuDeviceCreateBindGroupLayout(ctx->device, &bgl_desc);

		WGPUPipelineLayoutDescriptor pl_desc = {0};
		pl_desc.bindGroupLayoutCount = 1;
		pl_desc.bindGroupLayouts = &ctx->blend_shader_bgl;
		ctx->blend_shader_pl = wgpuDeviceCreatePipelineLayout(ctx->device, &pl_desc);

		// One module holding all seven fragment entry points.
		size_t cap = strlen(blend_shader_wgsl_head) + 1;
		for (int i = 0; i < BLEND_SHADER_COUNT; i++)
			cap += strlen(blend_shader_wgsl_frag) + strlen(blend_shader_defs[i].entry)
			     + strlen(blend_shader_defs[i].func) + 8;
		char* src = (char*) malloc(cap);
		size_t len = (size_t) snprintf(src, cap, "%s", blend_shader_wgsl_head);
		for (int i = 0; i < BLEND_SHADER_COUNT; i++)
			len += (size_t) snprintf(src + len, cap - len, blend_shader_wgsl_frag,
				blend_shader_defs[i].entry, blend_shader_defs[i].func);

		WGPUShaderModule sm = create_shader(ctx->device, src, "blend_shader");
		free(src);

		WGPUColorTargetState ct = {0};
		ct.format = ctx->surface_format;
		ct.writeMask = WGPUColorWriteMask_All;
		ct.blend = NULL;   // == ruffle's BlendState::REPLACE; `discard` masks it

		WGPUDepthStencilState ds = {0};
		ds.format = WGPUTextureFormat_Depth24PlusStencil8;
		ds.depthWriteEnabled = 0;
		ds.depthCompare = WGPUCompareFunction_Always;
		ds.stencilFront.compare = WGPUCompareFunction_Equal;
		ds.stencilFront.failOp = WGPUStencilOperation_Keep;
		ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
		ds.stencilFront.passOp = WGPUStencilOperation_Keep;
		ds.stencilBack = ds.stencilFront;
		ds.stencilReadMask = 0xFF;
		ds.stencilWriteMask = 0x00;

		for (int i = 0; i < BLEND_SHADER_COUNT; i++)
		{
			WGPUFragmentState fs = {0};
			fs.module = sm;
			fs.entryPoint = WGPU_LABEL(blend_shader_defs[i].entry);
			fs.targetCount = 1;
			fs.targets = &ct;

			WGPURenderPipelineDescriptor rpd = {0};
			rpd.label = WGPU_LABEL("blend_shader_pipeline");
			rpd.layout = ctx->blend_shader_pl;
			rpd.vertex.module = sm;
			rpd.vertex.entryPoint = WGPU_LABEL("vs_main");
			rpd.fragment = &fs;
			rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
			rpd.multisample.count = MSAA_SAMPLES;
			rpd.multisample.mask = 0xFFFFFFFF;
			rpd.depthStencil = &ds;
			ctx->blend_shader_pipeline[blend_shader_defs[i].mode] =
				wgpuDeviceCreateRenderPipeline(ctx->device, &rpd);
		}
		wgpuShaderModuleRelease(sm);
	}
}

// Snapshot the frame so far into filter_tex_b, the "parent" the complex blends
// sample. MUST be called while the main pass is SUSPENDED — that is when the
// MSAA resolve into surface_view/offscreen_view has already happened.
void render_webgpu_capture_backdrop(WebGPURenderContext* ctx, u8 blend_mode)
{
	if (!ctx->renderer_ok) return;
	if (!blend_mode_is_complex(blend_mode)) return;   // trivial modes blend in HW
	if (ctx->main_color_texture == NULL) return;
	render_webgpu_ensure_blend_resources(ctx);

	WGPUTexelCopyTextureInfo src = {0};
	src.texture = ctx->main_color_texture;
	src.mipLevel = 0;
	src.origin = (WGPUOrigin3D){0, 0, 0};
	src.aspect = WGPUTextureAspect_All;

	WGPUTexelCopyTextureInfo dst = {0};
	dst.texture = ctx->filter_tex_b;
	dst.mipLevel = 0;
	dst.origin = (WGPUOrigin3D){0, 0, 0};
	dst.aspect = WGPUTextureAspect_All;

	WGPUExtent3D extent = {(u32)ctx->width, (u32)ctx->height, 1};
	wgpuCommandEncoderCopyTextureToTexture(ctx->encoder, &src, &dst, &extent);
}

// Composite the layer in filter_tex_a onto the resumed main pass.
void render_webgpu_composite_blend(WebGPURenderContext* ctx, u8 blend_mode, u32 stencil_ref)
{
	if (!ctx->renderer_ok) return;
	render_webgpu_ensure_blend_resources(ctx);

	WGPUBindGroup bg = NULL;
	WGPURenderPipeline pipeline = NULL;

	if (blend_mode_is_complex(blend_mode))
	{
		pipeline = ctx->blend_shader_pipeline[blend_mode];
		WGPUBindGroupEntry e[3] = {0};
		e[0].binding = 0;
		e[0].textureView = ctx->filter_view_b;   // backdrop ("parent")
		e[1].binding = 1;
		e[1].textureView = ctx->filter_view_a;   // this object's layer ("current")
		e[2].binding = 2;
		e[2].sampler = ctx->filter_sampler;
		WGPUBindGroupDescriptor bd = {0};
		bd.layout = ctx->blend_shader_bgl;
		bd.entryCount = 3;
		bd.entries = e;
		bg = wgpuDeviceCreateBindGroup(ctx->device, &bd);
	}
	else
	{
		pipeline = ctx->blend_layer_pipeline[blend_mode];
		WGPUBindGroupEntry e[3] = {0};
		e[0].binding = 0;
		e[0].textureView = ctx->filter_view_a;
		e[1].binding = 1;
		e[1].sampler = ctx->filter_sampler;
		e[2].binding = 2;
		e[2].buffer = ctx->blend_params_buf;
		e[2].size = 32;
		WGPUBindGroupDescriptor bd = {0};
		bd.layout = ctx->composite_bgl;
		bd.entryCount = 3;
		bd.entries = e;
		bg = wgpuDeviceCreateBindGroup(ctx->device, &bd);
	}

	if (pipeline == NULL || bg == NULL)
	{
		if (bg) wgpuBindGroupRelease(bg);
		return;
	}

	wgpuRenderPassEncoderSetPipeline(ctx->render_pass, pipeline);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 0, bg, 0, NULL);
	// The caller's stencil_ref is only a boolean "is a clip active" (it passed a
	// literal 1 back when every mask wrote 1). The reference is per-mask now, so
	// take it from the context — 0 when no clip is open, which is exactly what
	// the caller's "no clip" case meant.
	(void)stencil_ref;
	wgpuRenderPassEncoderSetStencilReference(ctx->render_pass, ctx->mask_ref);
	wgpuRenderPassEncoderDraw(ctx->render_pass, 6, 1, 0, 0);

	// Restore the normal pipeline + bind groups the display loop expects.
	restore_draw_pipeline(ctx);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 0, ctx->vertex_storage_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 2, ctx->fragment_sampler_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 1, ctx->vertex_uniform_bg, 0, NULL);

	wgpuBindGroupRelease(bg);
}

void render_webgpu_suspend_pass(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;
	if (ctx->render_pass)
	{
		wgpuRenderPassEncoderEnd(ctx->render_pass);
		wgpuRenderPassEncoderRelease(ctx->render_pass);
		ctx->render_pass = NULL;
	}
}

void render_webgpu_resume_pass(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;
	// Resume the main render pass with loadOp=Load to preserve existing content + stencil
	WGPURenderPassColorAttachment color_att = {0};
	color_att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#if MSAA_SAMPLES == 1
	color_att.view = ctx->surface_view;
	color_att.resolveTarget = NULL;
#else
	color_att.view = ctx->msaa_view;
	color_att.resolveTarget = ctx->surface_view;
#endif
	color_att.loadOp = WGPULoadOp_Load;
	color_att.storeOp = WGPUStoreOp_Store;

	WGPURenderPassDepthStencilAttachment ds_att = {0};
	ds_att.view = ctx->depth_stencil_view;
	ds_att.depthLoadOp = WGPULoadOp_Load;
	ds_att.depthStoreOp = WGPUStoreOp_Discard;
	ds_att.stencilLoadOp = WGPULoadOp_Load;
	ds_att.stencilStoreOp = WGPUStoreOp_Store;

	WGPURenderPassDescriptor rp_desc = {0};
	rp_desc.label = WGPU_LABEL("resume_pass");
	rp_desc.colorAttachmentCount = 1;
	rp_desc.colorAttachments = &color_att;
	rp_desc.depthStencilAttachment = &ds_att;

	ctx->render_pass = wgpuCommandEncoderBeginRenderPass(ctx->encoder, &rp_desc);

	// Re-bind everything. The stencil is reloaded (loadOp = Load), so the clip
	// mask that was open when the pass was suspended is still there — come back
	// to it rather than to a bare render_pipeline.
	restore_draw_pipeline(ctx);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 0, ctx->vertex_storage_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 2, ctx->fragment_sampler_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 1, ctx->vertex_uniform_bg, 0, NULL);
}

void render_webgpu_begin_offscreen_pass(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;
	render_webgpu_ensure_filter_resources(ctx);
	render_webgpu_ensure_blend_resources(ctx);
	ctx->offscreen_depth++;

	// The offscreen pass has its OWN depth-stencil, which it CLEARS below — the
	// main pass's clip mask does not exist in it, and any mask opened inside it
	// belongs to that cleared stencil. Park the clip state so (a) draws inside
	// the offscreen pass are not stencil-tested against a level that no texel
	// there has reached, and (b) an inner mask cannot pop the outer clip that
	// the resumed main pass still needs. The parked value is a NESTING LEVEL,
	// and the offscreen pass's own stencilClearValue = 0 is exactly the level
	// it is parked to, so the two agree without any geometry replay.
	if (ctx->mask_save_sp >= 0
	    && ctx->mask_save_sp < (int)(sizeof(ctx->mask_save_ref) / sizeof(ctx->mask_save_ref[0])))
	{
		ctx->mask_save_ref[ctx->mask_save_sp] = ctx->mask_ref;
		ctx->mask_save_cap[ctx->mask_save_sp] = ctx->mask_capture_depth;
	}
	ctx->mask_save_sp++;
	ctx->mask_ref = 0;
	ctx->mask_capture_depth = 0;

	// Use a SEPARATE MSAA texture (filter_msaa_view) so we don't clobber
	// the main pass content stored in ctx->msaa_view during suspend.
	WGPURenderPassColorAttachment color_att = {0};
	color_att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#if MSAA_SAMPLES == 1
	color_att.view = ctx->filter_view_a;
	color_att.resolveTarget = NULL;
#else
	color_att.view = ctx->filter_msaa_view;
	color_att.resolveTarget = ctx->filter_view_a;
#endif
	color_att.loadOp = WGPULoadOp_Clear;
	color_att.storeOp = WGPUStoreOp_Store;
	color_att.clearValue = (WGPUColor){0.0, 0.0, 0.0, 0.0};

	// Depth-stencil required because render_pipeline was created with it. It has
	// to be the offscreen pass's OWN texture: this pass clears + discards the
	// stencil, and doing that to depth_stencil_view would destroy the main pass's
	// active clip mask (the resumed pass reloads it with loadOp=Load).
	WGPURenderPassDepthStencilAttachment ds_att = {0};
	ds_att.view = ctx->filter_ds_view ? ctx->filter_ds_view : ctx->depth_stencil_view;
	ds_att.depthLoadOp = WGPULoadOp_Clear;
	ds_att.depthStoreOp = WGPUStoreOp_Discard;
	ds_att.depthClearValue = 1.0f;
	ds_att.stencilLoadOp = WGPULoadOp_Clear;
	ds_att.stencilStoreOp = WGPUStoreOp_Discard;
	ds_att.stencilClearValue = 0;

	WGPURenderPassDescriptor rp_desc = {0};
	rp_desc.label = WGPU_LABEL("offscreen_pass");
	rp_desc.colorAttachmentCount = 1;
	rp_desc.colorAttachments = &color_att;
	rp_desc.depthStencilAttachment = &ds_att;

	ctx->render_pass = wgpuCommandEncoderBeginRenderPass(ctx->encoder, &rp_desc);

	// Bind the main pipeline (MSAA 4x compatible). mask_ref was just parked at 0,
	// so this resolves to render_pipeline — routed through the helper so the one
	// rule about which pipeline a draw starts from lives in one place.
	restore_draw_pipeline(ctx);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 0, ctx->vertex_storage_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 2, ctx->fragment_sampler_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 1, ctx->vertex_uniform_bg, 0, NULL);
}

void render_webgpu_end_offscreen_pass(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;
	if (ctx->offscreen_depth > 0) ctx->offscreen_depth--;
	// Un-park the main pass's clip state (see begin_offscreen_pass).
	if (ctx->mask_save_sp > 0)
	{
		ctx->mask_save_sp--;
		if (ctx->mask_save_sp < (int)(sizeof(ctx->mask_save_ref) / sizeof(ctx->mask_save_ref[0])))
		{
			ctx->mask_ref = ctx->mask_save_ref[ctx->mask_save_sp];
			ctx->mask_capture_depth = ctx->mask_save_cap[ctx->mask_save_sp];
		}
	}
	if (ctx->render_pass)
	{
		wgpuRenderPassEncoderEnd(ctx->render_pass);
		wgpuRenderPassEncoderRelease(ctx->render_pass);
		ctx->render_pass = NULL;
	}
}

// One axis of Flash's fractional box blur, as six shader uniforms.
// `out` receives {full_size, m, m2, first_weight, last_offset, last_weight};
// see blur_wgsl above for how the fragment shader consumes them. Ported from
// ruffle render/wgpu/src/filters/blur.rs.
static void blur_box_kernel(float full_size, float out[6])
{
	// FP caps the window at 255 texels.
	if (full_size > 255.0f) full_size = 255.0f;
	if (!(full_size > 1.0f))
	{
		// A window of 1 texel or less is a no-op. Ruffle skips the pass; we
		// keep it, because the filter_tex_a/filter_tex_b ping-pong needs both
		// axes to run for the result to land back in filter_tex_a. These
		// weights make the pass an exact copy: weight 1 on the centre texel,
		// sampled dead on its centre, and the shader skips the 8-bit round.
		out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
		out[3] = 0.0f; out[4] = 0.0f; out[5] = 1.0f;
		return;
	}
	// How far the window reaches past the centre texel, either side.
	float radius = (full_size - 1.0f) * 0.5f;
	// ceil, not floor: with a whole-number radius this leaves alpha at 1 rather
	// than 0, so the outermost two texels fuse into a single sample instead of
	// wasting one and dividing by zero below.
	float m = ceilf(radius) - 1.0f;
	// Almost the fractional part of radius, quantised to 1/255 to imitate FP's
	// fixed-point weights.
	float alpha = floorf((radius - m) * 255.0f) / 255.0f;
	out[0] = full_size;
	out[1] = m;
	out[2] = m * 2.0f;
	out[3] = alpha;
	// Offset the fused last sample so the next-to-last texel ends up at weight
	// 1 and the last at weight alpha. alpha == 0 only when radius - m rounds
	// below 1/255, and then the pair degenerates to a single unit-weight texel.
	out[4] = (alpha > 0.0f) ? alpha / (1.0f + alpha) : 0.0f;
	out[5] = alpha + 1.0f;
}

void render_webgpu_run_blur(WebGPURenderContext* ctx,
	float blur_x, float blur_y, u8 quality,
	float strength, float r, float g, float b, float a, int colorize)
{
	if (!ctx->renderer_ok) return;
	float texel_w = 1.0f / (float)ctx->width;
	float texel_h = 1.0f / (float)ctx->height;

	// blur_x/blur_y arrive in the filtered object's own coordinate space, and
	// are the FULL width of Flash's box window (not a radius). Flash scales
	// filter sizes with the STAGE matrix, and scales (size - 1) so a 1px window
	// stays a no-op at any zoom — ruffle swf/src/types/blur_filter.rs
	// ::scale_blur. texel_w/h are in TARGET pixels, so without this a 2x render
	// would halve the apparent blur; that is what the *_scales_with_screen
	// tests assert. stage_scale is 1.0 whenever the target is the stage size.
	float stage_scale = ctx->stage_scale > 0.0f ? ctx->stage_scale : 1.0f;
	float kx[6], ky[6];
	blur_box_kernel((blur_x - 1.0f) * stage_scale + 1.0f, kx);
	blur_box_kernel((blur_y - 1.0f) * stage_scale + 1.0f, ky);

	// Params layout (80 bytes): vec2f direction (8), vec2f texel_size (8),
	// f32 full_size/m/m2/first_weight (16), f32 last_offset/last_weight/
	// strength/colorize (16), vec4f color (16), vec2f shift + pad (16).
	float params[20]; // 80 bytes
	// One-shot whole-texel shift (render_webgpu_set_blur_shift): applied on
	// the first H pass (x) and the first V pass (y) only — a shift commutes
	// with the box passes, so once is the whole of it.
	float shift_x = (float) ctx->blur_shift_x, shift_y = (float) ctx->blur_shift_y;
	ctx->blur_shift_x = 0; ctx->blur_shift_y = 0;
	// The whole shifted run reads transparent past the layer edge (tap()).
	float border = (shift_x != 0.0f || shift_y != 0.0f) ? 1.0f : 0.0f;

	for (u8 q = 0; q < quality; q++)
	{
		// Horizontal blur: filter_tex_a -> filter_tex_b
		params[0] = 1.0f; params[1] = 0.0f;   // direction
		params[2] = texel_w; params[3] = texel_h; // texel_size
		params[4] = kx[0]; params[5] = kx[1]; params[6] = kx[2]; params[7] = kx[3];
		params[8] = kx[4]; params[9] = kx[5];
		// strength and colorize belong to the V half of the final pass only.
		// They used to be written once per quality iteration and left in place
		// for the V pass too, so a glow with strength s and alpha a came out at
		// s^2 / a^2 (glow_with_alpha_strength). Flash applies both exactly once,
		// after every blur pass (ruffle glow.wgsl composites them separately).
		params[10] = 1.0f;                                 // strength: V pass only
		params[11] = 0.0f;                                 // colorize: V pass only
		params[12] = r; params[13] = g; params[14] = b; params[15] = a; // color
		params[16] = (q == 0) ? shift_x : 0.0f; params[17] = 0.0f;
		params[18] = border; params[19] = 0.0f;

		// Create bind group for H-blur: read filter_tex_a
		WGPUBindGroupEntry bg_entries[3] = {0};
		bg_entries[0].binding = 0;
		bg_entries[0].textureView = ctx->filter_view_a;
		bg_entries[1].binding = 1;
		bg_entries[1].sampler = ctx->filter_sampler;
		bg_entries[2].binding = 2;
		bg_entries[2].buffer = ctx->filter_uniform_ring;
		bg_entries[2].offset = filter_uniform_push(ctx, params, 80);
		bg_entries[2].size = 80;

		WGPUBindGroupDescriptor bg_desc = {0};
		bg_desc.layout = ctx->blur_bgl;
		bg_desc.entryCount = 3;
		bg_desc.entries = bg_entries;
		WGPUBindGroup bg = wgpuDeviceCreateBindGroup(ctx->device, &bg_desc);

		// Render to filter_tex_b
		WGPURenderPassColorAttachment color_att = {0};
		color_att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
		color_att.view = ctx->filter_view_b;
		color_att.loadOp = WGPULoadOp_Clear;
		color_att.storeOp = WGPUStoreOp_Store;
		color_att.clearValue = (WGPUColor){0, 0, 0, 0};

		WGPURenderPassDescriptor rp_desc = {0};
		rp_desc.label = WGPU_LABEL("blur_h");
		rp_desc.colorAttachmentCount = 1;
		rp_desc.colorAttachments = &color_att;

		WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(ctx->encoder, &rp_desc);
		wgpuRenderPassEncoderSetPipeline(pass, ctx->blur_pipeline);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
		wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
		wgpuBindGroupRelease(bg);

		// Vertical blur: filter_tex_b -> filter_tex_a
		params[0] = 0.0f; params[1] = 1.0f; // direction = vertical
		params[4] = ky[0]; params[5] = ky[1]; params[6] = ky[2]; params[7] = ky[3];
		params[8] = ky[4]; params[9] = ky[5];
		params[10] = (q == quality - 1) ? strength : 1.0f;
		params[11] = (q == quality - 1 && colorize) ? 1.0f : 0.0f;
		params[16] = 0.0f; params[17] = (q == 0) ? shift_y : 0.0f;

		bg_entries[0].textureView = ctx->filter_view_b;
		bg_entries[2].offset = filter_uniform_push(ctx, params, 80);
		bg = wgpuDeviceCreateBindGroup(ctx->device, &bg_desc);

		color_att.view = ctx->filter_view_a;
		rp_desc.label = WGPU_LABEL("blur_v");

		pass = wgpuCommandEncoderBeginRenderPass(ctx->encoder, &rp_desc);
		wgpuRenderPassEncoderSetPipeline(pass, ctx->blur_pipeline);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
		wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
		wgpuBindGroupRelease(bg);
	}
}

void render_webgpu_set_blur_shift(WebGPURenderContext* ctx, int sx, int sy)
{
	if (ctx == NULL) return;
	ctx->blur_shift_x = sx;
	ctx->blur_shift_y = sy;
}

void render_webgpu_composite_filtered(WebGPURenderContext* ctx,
	float offset_x, float offset_y,
	float tint_r, float tint_g, float tint_b, float tint_a)
{
	if (!ctx->renderer_ok) return;
	// offset_x/y are in NDC space; tint rgba = 0 means passthrough
	float has_tint = (tint_r != 0 || tint_g != 0 || tint_b != 0 || tint_a != 0) ? 1.0f : 0.0f;
	float params[8] = {offset_x, offset_y, has_tint, 0, tint_r, tint_g, tint_b, tint_a};
	u32 params_offset = filter_uniform_push(ctx, params, 32);

	// Create bind group for composite: read filter_tex_a
	WGPUBindGroupEntry bg_entries[3] = {0};
	bg_entries[0].binding = 0;
	bg_entries[0].textureView = ctx->filter_view_a;
	bg_entries[1].binding = 1;
	bg_entries[1].sampler = ctx->filter_sampler;
	bg_entries[2].binding = 2;
	bg_entries[2].buffer = ctx->filter_uniform_ring;
	bg_entries[2].offset = params_offset;
	bg_entries[2].size = 32;

	WGPUBindGroupDescriptor bg_desc = {0};
	bg_desc.layout = ctx->composite_bgl;
	bg_desc.entryCount = 3;
	bg_desc.entries = bg_entries;
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(ctx->device, &bg_desc);

	wgpuRenderPassEncoderSetPipeline(ctx->render_pass, ctx->composite_pipeline);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 0, bg, 0, NULL);
	// composite_pipeline stencil-tests Equal, so a hard-coded 0 composited a
	// filtered object OUTSIDE any enclosing mask. Use the active clip's
	// reference; it is 0 when no clip is open, i.e. unchanged in the common case.
	wgpuRenderPassEncoderSetStencilReference(ctx->render_pass, ctx->mask_ref);
	wgpuRenderPassEncoderDraw(ctx->render_pass, 6, 1, 0, 0);

	// Restore normal pipeline (and the active clip with it)
	restore_draw_pipeline(ctx);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 0, ctx->vertex_storage_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 2, ctx->fragment_sampler_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 1, ctx->vertex_uniform_bg, 0, NULL);

	wgpuBindGroupRelease(bg);
}

// Copy the offscreen source out of filter_tex_a before run_blur's ping-pong
// overwrites it. Must run with the main pass suspended (a texture-to-texture
// copy cannot be encoded inside a render pass).
void render_webgpu_snapshot_filter_source(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok) return;
	render_webgpu_ensure_filter_resources(ctx);

	WGPUTexelCopyTextureInfo src = {0};
	src.texture = ctx->filter_tex_a;
	src.mipLevel = 0;
	src.origin = (WGPUOrigin3D){0, 0, 0};
	src.aspect = WGPUTextureAspect_All;

	WGPUTexelCopyTextureInfo dst = {0};
	dst.texture = ctx->filter_src_tex;
	dst.mipLevel = 0;
	dst.origin = (WGPUOrigin3D){0, 0, 0};
	dst.aspect = WGPUTextureAspect_All;

	WGPUExtent3D extent = {(u32)ctx->width, (u32)ctx->height, 1};
	wgpuCommandEncoderCopyTextureToTexture(ctx->encoder, &src, &dst, &extent);
}

// Compose the blurred filter against the snapshotted source; see compose_wgsl.
void render_webgpu_compose_filter(WebGPURenderContext* ctx, int kind,
	float blur_off_x, float blur_off_y,
	float c1r, float c1g, float c1b, float c1a,
	float c2r, float c2g, float c2b, float c2a,
	float strength, int variant, int knockout, int composite_source)
{
	if (!ctx->renderer_ok) return;

	// 80 bytes: color1, color2, blur_off, mode, misc — five vec4f.
	float params[20] = {0};
	params[0] = c1r; params[1] = c1g; params[2] = c1b; params[3] = c1a;
	params[4] = c2r; params[5] = c2g; params[6] = c2b; params[7] = c2a;
	params[8] = blur_off_x; params[9] = blur_off_y;
	params[12] = (float)kind;
	params[13] = (float)variant;
	params[14] = knockout ? 1.0f : 0.0f;
	params[15] = composite_source ? 1.0f : 0.0f;
	params[16] = strength;
	u32 params_offset = filter_uniform_push(ctx, params, 80);

	WGPUBindGroupEntry bg_entries[4] = {0};
	bg_entries[0].binding = 0;
	bg_entries[0].textureView = ctx->filter_view_a;   // blurred
	bg_entries[1].binding = 1;
	bg_entries[1].sampler = ctx->filter_sampler;
	bg_entries[2].binding = 2;
	bg_entries[2].buffer = ctx->filter_uniform_ring;
	bg_entries[2].offset = params_offset;
	bg_entries[2].size = 80;
	bg_entries[3].binding = 3;
	bg_entries[3].textureView = ctx->filter_src_view; // unblurred source

	WGPUBindGroupDescriptor bg_desc = {0};
	bg_desc.layout = ctx->compose_bgl;
	bg_desc.entryCount = 4;
	bg_desc.entries = bg_entries;
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(ctx->device, &bg_desc);

	wgpuRenderPassEncoderSetPipeline(ctx->render_pass, ctx->compose_pipeline);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 0, bg, 0, NULL);
	// Same stencil-reference rule as render_webgpu_composite_filtered: honour
	// the enclosing clip mask instead of a hard-coded 0.
	wgpuRenderPassEncoderSetStencilReference(ctx->render_pass, ctx->mask_ref);
	wgpuRenderPassEncoderDraw(ctx->render_pass, 6, 1, 0, 0);

	restore_draw_pipeline(ctx);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 0, ctx->vertex_storage_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 2, ctx->fragment_sampler_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 1, ctx->vertex_uniform_bg, 0, NULL);

	wgpuBindGroupRelease(bg);
}

// Composite an ALPHA MASK (s17 P3): filter_tex_a holds the maskee layer and
// filter_src_tex the mask layer (put there by snapshot_filter_source before the
// second offscreen pass overwrote filter_tex_a). Draws maskee * mask.a over the
// resumed main pass, honouring any enclosing clip.
//
// This is ruffle's CommandList::render_alpha_mask — the arm
// apply_standard_mask_and_scroll takes when both maskee and masker are
// bitmap-cached (display_object.rs::get_render_mask). The uniform at binding 2
// is unused by alpha_mask_wgsl but the shared compose_bgl still requires a
// buffer there, so a zeroed 80-byte block is pushed for it.
void render_webgpu_composite_alpha_mask(WebGPURenderContext* ctx)
{
	if (!ctx->renderer_ok || ctx->render_pass == NULL) return;
	if (ctx->alpha_mask_pipeline == NULL) return;

	float params[20] = {0};
	u32 params_offset = filter_uniform_push(ctx, params, 80);

	WGPUBindGroupEntry bg_entries[4] = {0};
	bg_entries[0].binding = 0;
	bg_entries[0].textureView = ctx->filter_view_a;   // maskee layer
	bg_entries[1].binding = 1;
	bg_entries[1].sampler = ctx->filter_sampler;
	bg_entries[2].binding = 2;
	bg_entries[2].buffer = ctx->filter_uniform_ring;
	bg_entries[2].offset = params_offset;
	bg_entries[2].size = 80;
	bg_entries[3].binding = 3;
	bg_entries[3].textureView = ctx->filter_src_view; // mask layer

	WGPUBindGroupDescriptor bg_desc = {0};
	bg_desc.layout = ctx->compose_bgl;
	bg_desc.entryCount = 4;
	bg_desc.entries = bg_entries;
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(ctx->device, &bg_desc);
	if (bg == NULL) return;

	wgpuRenderPassEncoderSetPipeline(ctx->render_pass, ctx->alpha_mask_pipeline);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 0, bg, 0, NULL);
	wgpuRenderPassEncoderSetStencilReference(ctx->render_pass, ctx->mask_ref);
	wgpuRenderPassEncoderDraw(ctx->render_pass, 6, 1, 0, 0);

	restore_draw_pipeline(ctx);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 0, ctx->vertex_storage_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 2, ctx->fragment_sampler_bg, 0, NULL);
	wgpuRenderPassEncoderSetBindGroup(ctx->render_pass, 1, ctx->vertex_uniform_bg, 0, NULL);

	wgpuBindGroupRelease(bg);
}

// ---------------------------------------------------------------------------
// DisplacementMapFilter (filters cut 2).
//
// Port of ruffle render/wgpu/shaders/filter/displacement_map.wgsl +
// render/wgpu/src/filters/displacement_map.rs, with ONE structural adaptation
// that the whole cut turns on.
//
// Ruffle draws a filtered DisplayObject into a cache texture sized to its
// render bounds *under the stage view matrix* (display_object.rs render_base),
// so its FilterSource is object-sized and every rect-relative rule in the
// shader - the map offset, the /source_size normalisation, the out-of-bounds
// test, and the REPEAT sampler that implements `wrap` - is defined on the
// OBJECT. We have one stage-sized ping-pong layer instead (filters cut 1), and
// for blur/glow/dropShadow/bevel that is provably the same computation
// (glow_pass_scaling lands byte-identical). For displacement it is NOT: a
// stage-sized port would wrap around the STAGE.
//
// So the object's screen rect is passed in as a uniform and the shader works in
// rect-local pixels: `local = frag_pixel - rect_origin` is ruffle's
// `source_pos`, `rect_size` replaces `source_size`, and the source fetch is a
// MANUAL bilinear tap with explicit wrap/clamp inside the rect (textureLoad, no
// sampler) instead of leaning on an address mode that can only wrap the whole
// texture. Everything outside the rect returns transparent, exactly like
// ruffle's fresh CommandTarget::FreshWithColor(TRANSPARENT).
//
// The tap arithmetic is deliberately the same as the CPU twin in
// avm2_bitmap.c::bd_displace_source_region / dm_sample_repeat (s15), which is
// already CI-graded against the same content through
// visual/filters/displacement_map_through_applyFilter: nearest+ClampToEdge on
// the map, the neutral vec4(0.5) = 127.5 (half a level, not 128) for a map_uv
// outside 0..1, channels read AS STORED (premultiplied both sides), and
// bilinear+repeat on the source.
static const char* displace_wgsl =
	"struct DParams {\n"
	"  color: vec4f,\n"      // mode 3 colour, straight rgb + alpha
	"  comp: vec4f,\n"       // x=componentX, y=componentY, z=mode, w=unused
	"  rect: vec4f,\n"       // xy = rect origin (texture px), zw = rect size (px)
	"  mapinfo: vec4f,\n"    // xy = map size (px), zw = mapPoint (stage px)
	"  misc: vec4f,\n"       // xy = viewscale, zw = scaleX/scaleY
	"  texsize: vec4f,\n"    // xy = source texture size (px)
	"}\n"
	"@group(0) @binding(0) var src_tex: texture_2d<f32>;\n"
	"@group(0) @binding(1) var map_tex: texture_2d<f32>;\n"
	"@group(0) @binding(2) var<uniform> p: DParams;\n"
	"\n"
	"struct VSOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f }\n"
	"\n"
	"@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VSOut {\n"
	"  var positions = array<vec2f, 6>(\n"
	"    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),\n"
	"    vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0)\n"
	"  );\n"
	"  var uvs = array<vec2f, 6>(\n"
	"    vec2f(0.0, 1.0), vec2f(1.0, 1.0), vec2f(0.0, 0.0),\n"
	"    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(1.0, 0.0)\n"
	"  );\n"
	"  var out: VSOut;\n"
	"  out.pos = vec4f(positions[vi], 0.0, 1.0);\n"
	"  out.uv = uvs[vi];\n"
	"  return out;\n"
	"}\n"
	"\n"
	// ruffle get_component(): 1=R, 2=G, 4=B, 8=A, anything else = 128 (which
	// here means "zero displacement on this axis"). componentX/Y of 0 is
	// undocumented but real - visual/filters/displacement_map's last two tiles.
	"fn get_component(m: vec4f, c: f32) -> f32 {\n"
	"  if (c == 1.0) { return m.r; }\n"
	"  if (c == 2.0) { return m.g; }\n"
	"  if (c == 4.0) { return m.b; }\n"
	"  if (c == 8.0) { return m.a; }\n"
	"  return 128.0;\n"
	"}\n"
	"\n"
	"fn wrapi(v: i32, n: i32) -> i32 {\n"
	"  let r = v % n;\n"
	"  if (r < 0) { return r + n; }\n"
	"  return r;\n"
	"}\n"
	"\n"
	// Bilinear + Repeat over the RECT (ruffle's source sampler is
	// get_sampler(true, true) = repeat + filtering over its object-sized
	// texture). pp is in rect-local pixels.
	"fn sample_src(pp: vec2f) -> vec4f {\n"
	"  let w = i32(p.rect.z);\n"
	"  let h = i32(p.rect.w);\n"
	"  let f = pp - vec2f(0.5, 0.5);\n"
	"  let i0 = floor(f);\n"
	"  let t = f - i0;\n"
	"  let ox = i32(p.rect.x);\n"
	"  let oy = i32(p.rect.y);\n"
	"  let x0 = wrapi(i32(i0.x), w);\n"
	"  let x1 = wrapi(i32(i0.x) + 1, w);\n"
	"  let y0 = wrapi(i32(i0.y), h);\n"
	"  let y1 = wrapi(i32(i0.y) + 1, h);\n"
	"  let c00 = textureLoad(src_tex, vec2i(ox + x0, oy + y0), 0);\n"
	"  let c10 = textureLoad(src_tex, vec2i(ox + x1, oy + y0), 0);\n"
	"  let c01 = textureLoad(src_tex, vec2i(ox + x0, oy + y1), 0);\n"
	"  let c11 = textureLoad(src_tex, vec2i(ox + x1, oy + y1), 0);\n"
	"  return mix(mix(c00, c10, t.x), mix(c01, c11, t.x), t.y);\n"
	"}\n"
	"\n"
	"@fragment fn fs_main(@location(0) uv: vec2f) -> @location(0) vec4f {\n"
	"  let pix = uv * p.texsize.xy;\n"
	"  let local = pix - p.rect.xy;\n"
	// Outside the object's own rect there is no FilterSource at all.
	"  if (local.x < 0.0 || local.x > p.rect.z\n"
	"      || local.y < 0.0 || local.y > p.rect.w) {\n"
	"    return vec4f(0.0, 0.0, 0.0, 0.0);\n"
	"  }\n"
	"  let vs = p.misc.xy;\n"
	// ruffle: map_uv = (source_pos - offset) / viewscale / map_size.
	"  let mu = (local.x - p.mapinfo.z) / vs.x / p.mapinfo.x;\n"
	"  let mv = (local.y - p.mapinfo.w) / vs.y / p.mapinfo.y;\n"
	"  var cv = vec2f(127.5, 127.5);\n"
	"  if (mu >= 0.0 && mu <= 1.0 && mv >= 0.0 && mv <= 1.0) {\n"
	"    let mw = i32(p.mapinfo.x);\n"
	"    let mh = i32(p.mapinfo.y);\n"
	"    let mx = clamp(i32(floor(mu * p.mapinfo.x)), 0, mw - 1);\n"
	"    let my = clamp(i32(floor(mv * p.mapinfo.y)), 0, mh - 1);\n"
	"    let mc = textureLoad(map_tex, vec2i(mx, my), 0) * 255.0;\n"
	"    cv = vec2f(get_component(mc, p.comp.x), get_component(mc, p.comp.y));\n"
	"  }\n"
	// ruffle displace_coordinates(), with scale already multiplied by viewscale.
	"  let sc = vec2f(p.misc.z, p.misc.w) * vs;\n"
	"  let d = local + (cv - vec2f(128.0, 128.0)) * sc / 256.0;\n"
	"  var duv = d / p.rect.zw;\n"
	"  let oob = duv.x < 0.0 || duv.x > 1.0 || duv.y < 0.0 || duv.y > 1.0;\n"
	"  let mode = p.comp.z;\n"
	"  if (mode == 1.0) {\n"                       // clamp
	"    duv = clamp(duv, vec2f(0.0, 0.0), vec2f(1.0, 1.0));\n"
	"  } else if (mode == 2.0 && oob) {\n"         // ignore
	"    duv = local / p.rect.zw;\n"
	"  }\n"
	"  var result = sample_src(duv * p.rect.zw);\n"
	"  if (mode == 3.0 && oob) {\n"                // color
	"    result = vec4f(p.color.rgb, 1.0) * p.color.a;\n"
	"  }\n"
	"  return result;\n"
	"}\n";

// Lazily built on the first displacement filter in the movie: an AVM2 movie
// that never uses one pays nothing (no pipeline, no map texture).
static void ensure_displace_resources(WebGPURenderContext* ctx)
{
	if (ctx->displace_resources_created) return;
	ctx->displace_resources_created = 1;

	WGPUBindGroupLayoutEntry entries[3] = {0};
	entries[0].binding = 0;
	entries[0].visibility = WGPUShaderStage_Fragment;
	entries[0].texture.sampleType = WGPUTextureSampleType_Float;
	entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
	entries[1].binding = 1;
	entries[1].visibility = WGPUShaderStage_Fragment;
	entries[1].texture.sampleType = WGPUTextureSampleType_Float;
	entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
	entries[2].binding = 2;
	entries[2].visibility = WGPUShaderStage_Fragment;
	entries[2].buffer.type = WGPUBufferBindingType_Uniform;
	entries[2].buffer.minBindingSize = 96;

	WGPUBindGroupLayoutDescriptor bgl_desc = {0};
	bgl_desc.entryCount = 3;
	bgl_desc.entries = entries;
	ctx->displace_bgl = wgpuDeviceCreateBindGroupLayout(ctx->device, &bgl_desc);

	WGPUPipelineLayoutDescriptor pl_desc = {0};
	pl_desc.bindGroupLayoutCount = 1;
	pl_desc.bindGroupLayouts = &ctx->displace_bgl;
	ctx->displace_pipeline_layout = wgpuDeviceCreatePipelineLayout(ctx->device, &pl_desc);

	WGPUShaderModule sm = create_shader(ctx->device, displace_wgsl, "displace_shader");

	WGPUColorTargetState ct = {0};
	ct.format = ctx->surface_format;
	ct.writeMask = WGPUColorWriteMask_All;
	WGPUBlendState blend = {0};
	blend.color.srcFactor = WGPUBlendFactor_One;
	blend.color.dstFactor = WGPUBlendFactor_Zero;
	blend.color.operation = WGPUBlendOperation_Add;
	blend.alpha.srcFactor = WGPUBlendFactor_One;
	blend.alpha.dstFactor = WGPUBlendFactor_Zero;
	blend.alpha.operation = WGPUBlendOperation_Add;
	ct.blend = &blend;

	WGPUFragmentState fs = {0};
	fs.module = sm;
	fs.entryPoint = WGPU_LABEL("fs_main");
	fs.targetCount = 1;
	fs.targets = &ct;

	WGPURenderPipelineDescriptor rpd = {0};
	rpd.label = WGPU_LABEL("displace_pipeline");
	rpd.layout = ctx->displace_pipeline_layout;
	rpd.vertex.module = sm;
	rpd.vertex.entryPoint = WGPU_LABEL("vs_main");
	rpd.fragment = &fs;
	rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	// The ping-pong targets (filter_tex_a/b) are single-sampled, exactly like
	// the blur pipeline next door; MSAA_SAMPLES belongs to the OFFSCREEN pass
	// that produced them, never to a filter pass.
	rpd.multisample.count = 1;
	rpd.multisample.mask = 0xFFFFFFFF;
	ctx->displace_pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &rpd);
	wgpuShaderModuleRelease(sm);
}

// Upload the filter's map BitmapData into a dedicated RGBA8 texture. Format is
// pinned to RGBA8Unorm (not ctx->surface_format) so the byte order below is the
// channel order the shader reads, whatever the swapchain happens to be. Pixels
// are premultiplied ARGB u32, exactly as Avm2BitmapDataExt stores them and as
// bd_displace_source_region reads them.
static void upload_displace_map(WebGPURenderContext* ctx,
	const uint32_t* argb_pixels, u32 map_w, u32 map_h)
{
	if (ctx->displace_map_tex == NULL
	    || ctx->displace_map_w != map_w || ctx->displace_map_h != map_h)
	{
		if (ctx->displace_map_view) wgpuTextureViewRelease(ctx->displace_map_view);
		if (ctx->displace_map_tex) wgpuTextureRelease(ctx->displace_map_tex);
		ctx->displace_map_view = NULL;
		ctx->displace_map_tex = NULL;
		WGPUTextureDescriptor td = {0};
		td.label = WGPU_LABEL("displace_map");
		td.dimension = WGPUTextureDimension_2D;
		td.size = (WGPUExtent3D){map_w, map_h, 1};
		td.format = WGPUTextureFormat_RGBA8Unorm;
		td.mipLevelCount = 1;
		td.sampleCount = 1;
		td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
		ctx->displace_map_tex = wgpuDeviceCreateTexture(ctx->device, &td);
		WGPUTextureViewDescriptor vd = {0};
		vd.mipLevelCount = 1;
		vd.arrayLayerCount = 1;
		ctx->displace_map_view = wgpuTextureCreateView(ctx->displace_map_tex, &vd);
		ctx->displace_map_w = map_w;
		ctx->displace_map_h = map_h;
	}

	size_t n = (size_t)map_w * (size_t)map_h;
	u8* temp = (u8*)malloc(n * 4);
	if (temp == NULL) return;
	u32* dst = (u32*)temp;
	for (size_t i = 0; i < n; i++)
	{
		u32 argb = argb_pixels[i];
		u32 a = (argb >> 24) & 0xFF;
		u32 r = (argb >> 16) & 0xFF;
		u32 g = (argb >> 8) & 0xFF;
		u32 b = argb & 0xFF;
		dst[i] = r | (g << 8) | (b << 16) | (a << 24);
	}
	WGPUTexelCopyTextureInfo dest = {0};
	dest.texture = ctx->displace_map_tex;
	dest.origin = (WGPUOrigin3D){0, 0, 0};
	WGPUTexelCopyBufferLayout layout = {0};
	layout.bytesPerRow = map_w * 4;
	layout.rowsPerImage = map_h;
	WGPUExtent3D extent = {map_w, map_h, 1};
	wgpuQueueWriteTexture(ctx->queue, &dest, temp, n * 4, &layout, &extent);
	free(temp);
}

// One DisplacementMapFilter pass over the offscreen layer: filter_tex_a ->
// filter_tex_b, then copied back so the ping-pong invariant ("the layer always
// ends up in filter_tex_a") holds and the pass chains with run_blur.
//
// rect_* are the filtered object's screen rect in RENDER-TARGET pixels
// (ruffle's cache-texture geometry); map_point is in STAGE pixels, as the AS
// property is; viewscale is the stage view matrix's scale, the same factor
// run_blur applies to blur sizes.
void render_webgpu_run_displacement(WebGPURenderContext* ctx,
	const uint32_t* map_pixels, u32 map_w, u32 map_h,
	float rect_x, float rect_y, float rect_w, float rect_h,
	float map_point_x, float map_point_y,
	float scale_x, float scale_y, float viewscale,
	int comp_x, int comp_y, int mode,
	float cr, float cg, float cb, float ca)
{
	if (!ctx->renderer_ok) return;
	if (map_pixels == NULL || map_w == 0 || map_h == 0) return;
	if (rect_w < 1.0f || rect_h < 1.0f) return;
	render_webgpu_ensure_filter_resources(ctx);
	ensure_displace_resources(ctx);
	if (ctx->displace_pipeline == NULL) return;
	upload_displace_map(ctx, map_pixels, map_w, map_h);
	if (ctx->displace_map_view == NULL) return;

	// 96 bytes: color, comp, rect, mapinfo, misc, texsize - six vec4f.
	float params[24] = {0};
	params[0] = cr; params[1] = cg; params[2] = cb; params[3] = ca;
	params[4] = (float)comp_x; params[5] = (float)comp_y; params[6] = (float)mode;
	params[8] = rect_x; params[9] = rect_y; params[10] = rect_w; params[11] = rect_h;
	params[12] = (float)map_w; params[13] = (float)map_h;
	params[14] = map_point_x; params[15] = map_point_y;
	params[16] = viewscale; params[17] = viewscale;
	params[18] = scale_x; params[19] = scale_y;
	params[20] = (float)ctx->width; params[21] = (float)ctx->height;
	u32 params_offset = filter_uniform_push(ctx, params, 96);

	WGPUBindGroupEntry bg_entries[3] = {0};
	bg_entries[0].binding = 0;
	bg_entries[0].textureView = ctx->filter_view_a;
	bg_entries[1].binding = 1;
	bg_entries[1].textureView = ctx->displace_map_view;
	bg_entries[2].binding = 2;
	bg_entries[2].buffer = ctx->filter_uniform_ring;
	bg_entries[2].offset = params_offset;
	bg_entries[2].size = 96;

	WGPUBindGroupDescriptor bg_desc = {0};
	bg_desc.layout = ctx->displace_bgl;
	bg_desc.entryCount = 3;
	bg_desc.entries = bg_entries;
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(ctx->device, &bg_desc);

	WGPURenderPassColorAttachment color_att = {0};
	color_att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
	color_att.view = ctx->filter_view_b;
	color_att.loadOp = WGPULoadOp_Clear;
	color_att.storeOp = WGPUStoreOp_Store;
	color_att.clearValue = (WGPUColor){0, 0, 0, 0};

	WGPURenderPassDescriptor rp_desc = {0};
	rp_desc.label = WGPU_LABEL("displace");
	rp_desc.colorAttachmentCount = 1;
	rp_desc.colorAttachments = &color_att;

	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(ctx->encoder, &rp_desc);
	wgpuRenderPassEncoderSetPipeline(pass, ctx->displace_pipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
	wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);
	wgpuBindGroupRelease(bg);

	// Back into filter_tex_a. Both ping-pong textures carry CopySrc|CopyDst
	// (see render_webgpu_ensure_filter_resources), and this runs with the main
	// pass suspended, so the copy is legal here for the same reason
	// render_webgpu_snapshot_filter_source's is.
	WGPUTexelCopyTextureInfo src = {0};
	src.texture = ctx->filter_tex_b;
	src.mipLevel = 0;
	src.origin = (WGPUOrigin3D){0, 0, 0};
	src.aspect = WGPUTextureAspect_All;
	WGPUTexelCopyTextureInfo dst = {0};
	dst.texture = ctx->filter_tex_a;
	dst.mipLevel = 0;
	dst.origin = (WGPUOrigin3D){0, 0, 0};
	dst.aspect = WGPUTextureAspect_All;
	WGPUExtent3D extent = {(u32)ctx->width, (u32)ctx->height, 1};
	wgpuCommandEncoderCopyTextureToTexture(ctx->encoder, &src, &dst, &extent);
}

// ---------------------------------------------------------------------------
// render_webgpu_free: release all GPU resources
// ---------------------------------------------------------------------------
void render_webgpu_free(SWFAppContext* app_context, WebGPURenderContext* ctx)
{
	(void)app_context;

	// Release pipelines
	if (ctx->render_pipeline)
		wgpuRenderPipelineRelease(ctx->render_pipeline);
	if (ctx->stencil_write_pipeline)
		wgpuRenderPipelineRelease(ctx->stencil_write_pipeline);
	if (ctx->stencil_test_pipeline)
		wgpuRenderPipelineRelease(ctx->stencil_test_pipeline);
	if (ctx->blend_premul_pipeline)
		wgpuRenderPipelineRelease(ctx->blend_premul_pipeline);
	for (int bm = 0; bm < 15; bm++)
	{
		if (ctx->blend_layer_pipeline[bm])
			wgpuRenderPipelineRelease(ctx->blend_layer_pipeline[bm]);
		if (ctx->blend_shader_pipeline[bm])
			wgpuRenderPipelineRelease(ctx->blend_shader_pipeline[bm]);
	}
	if (ctx->blend_shader_pl) wgpuPipelineLayoutRelease(ctx->blend_shader_pl);
	if (ctx->blend_shader_bgl) wgpuBindGroupLayoutRelease(ctx->blend_shader_bgl);
	if (ctx->blend_params_buf) wgpuBufferRelease(ctx->blend_params_buf);
	if (ctx->filter_ds_view) wgpuTextureViewRelease(ctx->filter_ds_view);
	if (ctx->filter_ds_texture) wgpuTextureRelease(ctx->filter_ds_texture);
	if (ctx->compute_pipeline)
		wgpuComputePipelineRelease(ctx->compute_pipeline);

	// Release bind groups
	if (ctx->vertex_storage_bg) wgpuBindGroupRelease(ctx->vertex_storage_bg);
	if (ctx->vertex_uniform_bg) wgpuBindGroupRelease(ctx->vertex_uniform_bg);
	if (ctx->fragment_sampler_bg) wgpuBindGroupRelease(ctx->fragment_sampler_bg);
	if (ctx->compute_read_bg) wgpuBindGroupRelease(ctx->compute_read_bg);
	if (ctx->compute_write_bg) wgpuBindGroupRelease(ctx->compute_write_bg);

	// Release bind group layouts
	if (ctx->vertex_storage_bgl) wgpuBindGroupLayoutRelease(ctx->vertex_storage_bgl);
	if (ctx->vertex_uniform_bgl) wgpuBindGroupLayoutRelease(ctx->vertex_uniform_bgl);
	if (ctx->fragment_sampler_bgl) wgpuBindGroupLayoutRelease(ctx->fragment_sampler_bgl);
	if (ctx->compute_read_bgl) wgpuBindGroupLayoutRelease(ctx->compute_read_bgl);
	if (ctx->compute_write_bgl) wgpuBindGroupLayoutRelease(ctx->compute_write_bgl);

	// Release pipeline layouts
	if (ctx->render_pipeline_layout) wgpuPipelineLayoutRelease(ctx->render_pipeline_layout);
	if (ctx->compute_pipeline_layout) wgpuPipelineLayoutRelease(ctx->compute_pipeline_layout);

	// Release buffers
	if (ctx->vertex_buffer) wgpuBufferRelease(ctx->vertex_buffer);
	if (ctx->xform_buffer) wgpuBufferRelease(ctx->xform_buffer);
	if (ctx->color_buffer) wgpuBufferRelease(ctx->color_buffer);
	if (ctx->uninv_mat_buffer) wgpuBufferRelease(ctx->uninv_mat_buffer);
	if (ctx->inv_mat_buffer) wgpuBufferRelease(ctx->inv_mat_buffer);
	if (ctx->bitmap_sizes_buffer) wgpuBufferRelease(ctx->bitmap_sizes_buffer);
	if (ctx->cxform_buffer) wgpuBufferRelease(ctx->cxform_buffer);
	if (ctx->stage_to_ndc_buf) wgpuBufferRelease(ctx->stage_to_ndc_buf);
	if (ctx->transform_id_buf) wgpuBufferRelease(ctx->transform_id_buf);
	if (ctx->extra_transform_id_buf) wgpuBufferRelease(ctx->extra_transform_id_buf);
	if (ctx->extra_transform_buf) wgpuBufferRelease(ctx->extra_transform_buf);
	if (ctx->cxform_id_buf) wgpuBufferRelease(ctx->cxform_id_buf);
	if (ctx->cxform_uniform_buf) wgpuBufferRelease(ctx->cxform_uniform_buf);

	// Release textures and views
	if (ctx->gradient_tex_view) wgpuTextureViewRelease(ctx->gradient_tex_view);
	if (ctx->gradient_tex) wgpuTextureRelease(ctx->gradient_tex);
	if (ctx->gradient_sampler) wgpuSamplerRelease(ctx->gradient_sampler);
	for (u32 i = 0; i < BITMAP_POOL_COUNT; i++)
	{
		if (ctx->bitmap_pools[i].view) wgpuTextureViewRelease(ctx->bitmap_pools[i].view);
		if (ctx->bitmap_pools[i].tex) wgpuTextureRelease(ctx->bitmap_pools[i].tex);
	}
	if (ctx->bitmap_sampler) wgpuSamplerRelease(ctx->bitmap_sampler);
	if (ctx->bitmap_sampler_linear) wgpuSamplerRelease(ctx->bitmap_sampler_linear);
	if (ctx->dummy_tex_view) wgpuTextureViewRelease(ctx->dummy_tex_view);
	if (ctx->dummy_tex_2d_view) wgpuTextureViewRelease(ctx->dummy_tex_2d_view);
	if (ctx->dummy_tex) wgpuTextureRelease(ctx->dummy_tex);
	if (ctx->dummy_sampler) wgpuSamplerRelease(ctx->dummy_sampler);
	if (ctx->msaa_view) wgpuTextureViewRelease(ctx->msaa_view);
	if (ctx->msaa_texture) wgpuTextureRelease(ctx->msaa_texture);
	if (ctx->depth_stencil_view) wgpuTextureViewRelease(ctx->depth_stencil_view);
	if (ctx->depth_stencil_texture) wgpuTextureRelease(ctx->depth_stencil_texture);

	// Release filter resources
	if (ctx->filter_msaa_view) wgpuTextureViewRelease(ctx->filter_msaa_view);
	if (ctx->filter_msaa_texture) wgpuTextureRelease(ctx->filter_msaa_texture);
	if (ctx->filter_view_a) wgpuTextureViewRelease(ctx->filter_view_a);
	if (ctx->filter_tex_a) wgpuTextureRelease(ctx->filter_tex_a);
	if (ctx->filter_view_b) wgpuTextureViewRelease(ctx->filter_view_b);
	if (ctx->filter_tex_b) wgpuTextureRelease(ctx->filter_tex_b);
	if (ctx->filter_sampler) wgpuSamplerRelease(ctx->filter_sampler);
	if (ctx->filter_uniform_ring) wgpuBufferRelease(ctx->filter_uniform_ring);
	if (ctx->blur_pipeline) wgpuRenderPipelineRelease(ctx->blur_pipeline);
	if (ctx->blur_bgl) wgpuBindGroupLayoutRelease(ctx->blur_bgl);
	if (ctx->blur_pipeline_layout) wgpuPipelineLayoutRelease(ctx->blur_pipeline_layout);
	if (ctx->composite_pipeline) wgpuRenderPipelineRelease(ctx->composite_pipeline);
	if (ctx->composite_bgl) wgpuBindGroupLayoutRelease(ctx->composite_bgl);
	if (ctx->composite_pipeline_layout) wgpuPipelineLayoutRelease(ctx->composite_pipeline_layout);
	if (ctx->filter_src_view) wgpuTextureViewRelease(ctx->filter_src_view);
	if (ctx->filter_src_tex) wgpuTextureRelease(ctx->filter_src_tex);
	if (ctx->alpha_mask_pipeline) wgpuRenderPipelineRelease(ctx->alpha_mask_pipeline);
	if (ctx->compose_pipeline) wgpuRenderPipelineRelease(ctx->compose_pipeline);
	if (ctx->compose_bgl) wgpuBindGroupLayoutRelease(ctx->compose_bgl);
	if (ctx->compose_pipeline_layout) wgpuPipelineLayoutRelease(ctx->compose_pipeline_layout);
	// DisplacementMapFilter (cut 2) — all NULL unless the movie used one.
	if (ctx->displace_map_view) wgpuTextureViewRelease(ctx->displace_map_view);
	if (ctx->displace_map_tex) wgpuTextureRelease(ctx->displace_map_tex);
	if (ctx->displace_pipeline) wgpuRenderPipelineRelease(ctx->displace_pipeline);
	if (ctx->displace_bgl) wgpuBindGroupLayoutRelease(ctx->displace_bgl);
	if (ctx->displace_pipeline_layout) wgpuPipelineLayoutRelease(ctx->displace_pipeline_layout);

	// Release surface
	if (ctx->surface) wgpuSurfaceRelease(ctx->surface);

	// Release device, adapter, instance
	if (ctx->queue) wgpuQueueRelease(ctx->queue);
	if (ctx->device) wgpuDeviceRelease(ctx->device);
	if (ctx->adapter) wgpuAdapterRelease(ctx->adapter);
	if (ctx->instance) wgpuInstanceRelease(ctx->instance);

	// Release heap-allocated data
	FREE(ctx->bitmap_sizes);

	// Destroy SDL window (native only)
#if !defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	if (ctx->window)
		SDL_DestroyWindow(ctx->window);
#endif

#ifdef OFFSCREEN_RENDER
	// Release headless resources
	if (ctx->offscreen_view) wgpuTextureViewRelease(ctx->offscreen_view);
	if (ctx->offscreen_texture) wgpuTextureRelease(ctx->offscreen_texture);
	if (ctx->readback_buffer) wgpuBufferRelease(ctx->readback_buffer);
#endif

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	if (ctx->browser_capture_view) wgpuTextureViewRelease(ctx->browser_capture_view);
	if (ctx->browser_capture_texture) wgpuTextureRelease(ctx->browser_capture_texture);
	if (ctx->browser_capture_readback) wgpuBufferRelease(ctx->browser_capture_readback);
	if (ctx->browser_capture_rgba) free(ctx->browser_capture_rgba);
#endif

	free(ctx);
}

// ---------------------------------------------------------------------------
// Headless framebuffer capture and PNG output
// ---------------------------------------------------------------------------
#ifdef OFFSCREEN_RENDER

// Synchronous callback state for buffer mapping
static volatile int g_map_done = 0;
static volatile WGPUMapAsyncStatus g_map_status;

static void on_map_callback(WGPUMapAsyncStatus status,
                            WGPUStringView message,
                            void* userdata1, void* userdata2)
{
	(void)message;
	(void)userdata1;
	(void)userdata2;
	g_map_status = status;
	g_map_done = 1;
}

void render_webgpu_request_capture(WebGPURenderContext* ctx)
{
	ctx->capture_requested = 1;
}

int render_webgpu_save_png(WebGPURenderContext* ctx, const char* path)
{
	if (!ctx->readback_buffer) return 0;

	// Map the staging buffer synchronously
	g_map_done = 0;
	WGPUBufferMapCallbackInfo map_info = {0};
	map_info.mode = WGPUCallbackMode_AllowProcessEvents;
	map_info.callback = on_map_callback;
	wgpuBufferMapAsync(ctx->readback_buffer, WGPUMapMode_Read, 0,
	                   ctx->readback_row_stride * ctx->height, map_info);

	// Poll until mapped (synchronous spin — fine for headless test runner)
	while (!g_map_done) {
		wgpuInstanceProcessEvents(ctx->instance);
	}

	if (g_map_status != WGPUMapAsyncStatus_Success) {
		fprintf(stderr, "render_webgpu_save_png: buffer map failed (status %d)\n",
		        (int)g_map_status);
		return 0;
	}

	const void* mapped = wgpuBufferGetConstMappedRange(ctx->readback_buffer, 0,
	                                                   ctx->readback_row_stride * ctx->height);
	if (!mapped) {
		wgpuBufferUnmap(ctx->readback_buffer);
		return 0;
	}

	// Convert BGRA8 (GPU format) to RGBA8 (PNG format), stripping row padding
	int w = ctx->width;
	int h = ctx->height;
	unsigned char* rgba = malloc(w * h * 4);
	if (!rgba) {
		wgpuBufferUnmap(ctx->readback_buffer);
		return 0;
	}

	for (int y = 0; y < h; y++) {
		const unsigned char* src_row = (const unsigned char*)mapped + y * ctx->readback_row_stride;
		unsigned char* dst_row = rgba + y * w * 4;
		for (int x = 0; x < w; x++) {
			dst_row[x * 4 + 0] = src_row[x * 4 + 2]; // R <- B
			dst_row[x * 4 + 1] = src_row[x * 4 + 1]; // G <- G
			dst_row[x * 4 + 2] = src_row[x * 4 + 0]; // B <- R
			dst_row[x * 4 + 3] = src_row[x * 4 + 3]; // A <- A
		}
	}

	wgpuBufferUnmap(ctx->readback_buffer);

	// Write PNG
	int ok = stbi_write_png(path, w, h, 4, rgba, w * 4);
	free(rgba);

	ctx->capture_requested = 0;
	return ok;
}

#endif // OFFSCREEN_RENDER

// ---------------------------------------------------------------------------
// Browser on-demand framebuffer capture (debug; HAS_DISPLAY_BRIDGE)
//
// copyTextureToBuffer on the rendered color target → mapAsync → CPU RGBA,
// returned to JS. This is a DIRECT GPU copy, not a present, so it bypasses the
// browser compositor and the software present queue that makes a busy board's
// page/CDP screenshot hang in WSL2 (Chrome WebGPU has no GPU path there). The
// whole thing is driven from render_webgpu_close_pass across frames so this
// code never calls emscripten_sleep — the render loop already owns the single
// ASYNCIFY suspended stack, and a second suspension from dbgCapturePNG would
// corrupt it. JS polls render_webgpu_browser_capture_ready() instead.
// ---------------------------------------------------------------------------
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)

static volatile int g_browser_map_done = 0;
static volatile WGPUMapAsyncStatus g_browser_map_status;

static void on_browser_map_callback(WGPUMapAsyncStatus status,
                                    WGPUStringView message,
                                    void* userdata1, void* userdata2)
{
	(void)message; (void)userdata1; (void)userdata2;
	g_browser_map_status = status;
	g_browser_map_done = 1;
}

// Lazily create the capture render target + readback staging buffer (once).
static int ensure_browser_capture_resources(WebGPURenderContext* ctx)
{
	if (!ctx->device || ctx->width <= 0 || ctx->height <= 0) return 0;
	if (ctx->browser_capture_texture && ctx->browser_capture_readback) return 1;

	WGPUTextureDescriptor tex_desc = {0};
	tex_desc.label = WGPU_LABEL("browser_capture_target");
	tex_desc.dimension = WGPUTextureDimension_2D;
	tex_desc.size = (WGPUExtent3D){(uint32_t)ctx->width, (uint32_t)ctx->height, 1};
	tex_desc.format = ctx->surface_format;   // BGRA8Unorm — matches the MSAA target
	tex_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
	tex_desc.mipLevelCount = 1;
	tex_desc.sampleCount = 1;                 // resolve target for the 4x MSAA pass
	ctx->browser_capture_texture = wgpuDeviceCreateTexture(ctx->device, &tex_desc);
	if (!ctx->browser_capture_texture) return 0;
	ctx->browser_capture_view = wgpuTextureCreateView(ctx->browser_capture_texture, NULL);

	size_t row_bytes = (size_t)ctx->width * 4;
	size_t aligned_row = (row_bytes + 255) & ~(size_t)255;   // WebGPU 256-align
	ctx->browser_capture_row_stride = aligned_row;

	WGPUBufferDescriptor buf_desc = {0};
	buf_desc.label = WGPU_LABEL("browser_capture_readback");
	buf_desc.size = aligned_row * (size_t)ctx->height;
	buf_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
	buf_desc.mappedAtCreation = false;
	ctx->browser_capture_readback = wgpuDeviceCreateBuffer(ctx->device, &buf_desc);
	if (!ctx->browser_capture_readback) return 0;

	if (!ctx->browser_capture_rgba)
		ctx->browser_capture_rgba = malloc((size_t)ctx->width * ctx->height * 4);

	return ctx->browser_capture_rgba != NULL;
}

// Map the readback buffer and block (via emscripten_sleep) until it completes,
// then harvest BGRA→RGBA into the CPU buffer (state → 3). Called from close_pass,
// i.e. from the main loop's ASYNCIFY stack, so the sleep pauses frame submission
// and lets the GPU backlog drain — without that pause the copy is perpetually
// queued behind present work on a busy board and the map never fires.
static void browser_capture_finish(WebGPURenderContext* ctx)
{
	ctx->browser_capture_state = 2;   // mapping
	g_browser_map_done = 0;
	WGPUBufferMapCallbackInfo map_info = {0};
	// AllowSpontaneous so the callback fires during emscripten_sleep event-loop
	// turns — the browser never calls wgpuInstanceProcessEvents, so
	// AllowProcessEvents would never fire. Matches the adapter/device requests.
	map_info.mode = WGPUCallbackMode_AllowSpontaneous;
	map_info.callback = on_browser_map_callback;
	wgpuBufferMapAsync(ctx->browser_capture_readback, WGPUMapMode_Read, 0,
	                   ctx->browser_capture_row_stride * (size_t)ctx->height, map_info);

	int guard = 0;
	while (!g_browser_map_done && !g_device_lost && guard++ < 2500)   // up to ~20s, then give up
		emscripten_sleep(8);

	if (!g_browser_map_done || g_browser_map_status != WGPUMapAsyncStatus_Success)
	{
		fprintf(stderr, "browser capture: map failed (done=%d status %d)\n",
		        g_browser_map_done, (int)g_browser_map_status);
		ctx->browser_capture_state = 0;   // give up; JS poll will time out
		return;
	}

	size_t map_size = ctx->browser_capture_row_stride * (size_t)ctx->height;
	const void* mapped = wgpuBufferGetConstMappedRange(ctx->browser_capture_readback, 0, map_size);
	if (mapped && ctx->browser_capture_rgba)
	{
		int w = ctx->width, h = ctx->height;
		for (int y = 0; y < h; y++)
		{
			const unsigned char* src_row = (const unsigned char*)mapped + (size_t)y * ctx->browser_capture_row_stride;
			unsigned char* dst_row = ctx->browser_capture_rgba + (size_t)y * w * 4;
			for (int x = 0; x < w; x++)
			{
				dst_row[x * 4 + 0] = src_row[x * 4 + 2]; // R <- B
				dst_row[x * 4 + 1] = src_row[x * 4 + 1]; // G <- G
				dst_row[x * 4 + 2] = src_row[x * 4 + 0]; // B <- R
				dst_row[x * 4 + 3] = 255;                // opaque stage screenshot
			}
		}
		ctx->browser_capture_state = 3;   // ready
	}
	else
	{
		ctx->browser_capture_state = 0;
	}
	wgpuBufferUnmap(ctx->browser_capture_readback);
}

void render_webgpu_request_browser_capture(WebGPURenderContext* ctx)
{
	if (!ctx || !ctx->renderer_ok || g_device_lost) return;
	ctx->browser_capture_state = 1;   // requested — open_pass picks it up next frame
}

int render_webgpu_browser_capture_ready(WebGPURenderContext* ctx)
{
	return (ctx && ctx->browser_capture_state == 3) ? 1 : 0;
}

unsigned char* render_webgpu_browser_capture_data(WebGPURenderContext* ctx)
{
	if (!ctx || ctx->browser_capture_state != 3) return NULL;
	return ctx->browser_capture_rgba;
}

#endif // __EMSCRIPTEN__ && !OFFSCREEN_RENDER
