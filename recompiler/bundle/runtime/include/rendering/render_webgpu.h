#pragma once

// WebGPU rendering backend for SWFModernRuntime.
// Uses the standardized webgpu.h C API, which works for both:
//   - Native: via Dawn or wgpu-native (linked as libwebgpu)
//   - WASM:   via emdawnwebgpu (--use-port=emdawnwebgpu)
//
// The API surface matches flashbang.h so that swf.c and tag.c can
// switch between backends via renderer.h macros.

#include <webgpu/webgpu.h>
#include <common.h>

// Forward declaration
typedef struct SWFAppContext SWFAppContext;

// Bitmap texture pool: one texture_2d_array whose layers all share (w, h).
// Static bitmaps are grouped into BITMAP_STATIC_POOLS size classes, each pool
// sized to its members' bounding box; dynamic (attachBitmap / BitmapData /
// beginBitmapFill) uploads go to BITMAP_DYNAMIC_POOLS nominal-square pools
// created on first use and grown by reallocation. See "Bitmap texture pools"
// in render_webgpu.c.
typedef struct BitmapPool
{
	WGPUTexture tex;
	WGPUTextureView view;
	u32 w, h;        // layer size in texels (0 = pool not created)
	u32 layers;      // allocated array layers
	u32 used;        // static: members uploaded; dynamic: layers used this frame
	u32 cap;         // dynamic: growth ceiling in layers
} BitmapPool;
#define BITMAP_STATIC_POOLS 8
#define BITMAP_DYNAMIC_POOLS 4
#define BITMAP_POOL_COUNT (BITMAP_STATIC_POOLS + BITMAP_DYNAMIC_POOLS)

typedef struct WebGPURenderContext
{
	// Render-target size in device pixels (SWFAppContext::render_{width,height},
	// which is the viewport, not necessarily the movie box).
	int width;
	int height;
	// Uniform ShowAll fit factor from stage px to render px. 1.0 when the
	// render target is the stage size. Filter kernel radii are in target pixels
	// and have to be scaled by it.
	float stage_scale;

	const float* stage_to_ndc;

	// Bitmap metadata (same layout as FlashbangContext)
	size_t bitmap_count;
	size_t bitmap_highest_w;
	size_t bitmap_highest_h;
	size_t current_bitmap;
	// 8 u32 per bitmap SLOT: {content_w, content_h, padded_w, padded_h},
	// {pool, layer, 0, 0} (WGSL side: array<vec4u>, two entries per slot).
	// `content` is the bitmap's own size and is the repeat period for
	// 0x40/0x42 fills; `padded` is the layer size of the pool holding it
	// (members' bounding box, +1 edge-clamp row/col) and normalizes 0x41/0x43.
	// Slots [0, bitmap_count) are the recompiler's static bitmaps, followed by
	// MAX_DYNAMIC_BITMAP_SLOTS per-frame dynamic slots. The CPU copy covers the
	// static slots only.
	// NOTE: FlashbangContext's field of the same name is still 2 u32/layer.
	u32* bitmap_sizes;
	// Per static bitmap: an absolute pointer to its pixels. Not an offset —
	// a loaded child SWF defines its bitmaps out of its OWN bitmap_data array,
	// so the base has to be carried per entry (see defineBitmap in tag.h).
	const u8** bitmap_ptrs;
	int bitmap_static_built;  // static pools created + uploaded (finalize)
	// One past the highest slot filled by render_webgpu_predeclare_bitmap, i.e.
	// by a LOADED CHILD movie's static bitmaps (multi-SWF render slice). Those
	// slots sit ABOVE the root's sequential `current_bitmap` cursor, so
	// build_static_bitmap_pools has to size its walk off both. Stays 0 when no
	// child contributed bitmaps, which keeps the AVM2 case (tagInit never runs,
	// current_bitmap == 0, no pools at all) exactly as it was.
	u32 bitmap_predeclared_end;
	BitmapPool bitmap_pools[BITMAP_POOL_COUNT];
	// The DEVICE's texture limits, read back after device creation (the
	// adapter's maxima are requested, so on a real GPU these are typically
	// 2048 / 16384; SwiftShader and the WebGPU defaults give 256 / 8192).
	// max_texture_array_layers is additionally clamped by the test knob
	// SWF_GPU_MAX_TEXTURE_ARRAY_LAYERS. Every texture array the renderer
	// creates stays within both ("Bitmap texture pools" in render_webgpu.c).
	u32 max_texture_array_layers;
	u32 max_texture_dimension_2d;

	// CPU-side data pointers (populated by swf.c before init)
	char* shape_data;       size_t shape_data_size;
	char* transform_data;   size_t transform_data_size;
	char* color_data;       size_t color_data_size;
	char* uninv_mat_data;   size_t uninv_mat_data_size;
	char* gradient_data;    size_t gradient_data_size;
	char* bitmap_data;      size_t bitmap_data_size;
	char* cxform_data;      size_t cxform_data_size;

	// --- WebGPU core objects ---
	WGPUInstance instance;
	WGPUAdapter adapter;
	WGPUDevice device;
	WGPUQueue queue;
	WGPUSurface surface;
	WGPUTextureFormat surface_format;

	// --- GPU buffers ---
	WGPUBuffer vertex_buffer;
	WGPUBuffer xform_buffer;          // storage: transforms[]
	u32 xform_slot_count;             // total slots in xform_buffer (original + dynamic)
	WGPUBuffer color_buffer;          // storage: colors[]
	WGPUBuffer uninv_mat_buffer;      // storage: gradient matrices (input)
	WGPUBuffer inv_mat_buffer;        // storage: inverted gradient matrices (output)
	WGPUBuffer bitmap_sizes_buffer;   // storage: bitmap_sizes[]
	WGPUBuffer cxform_buffer;         // storage: cxform data
	u32 cxform_slot_count;            // total slots in cxform_buffer (original + dynamic)

	// Uniform buffers (written per-frame or per-draw)
	WGPUBuffer stage_to_ndc_buf;      // uniform: mat4
	WGPUBuffer transform_id_buf;      // uniform: u32 (per-draw)
	WGPUBuffer extra_transform_id_buf; // uniform: u32 padded to 16 bytes
	WGPUBuffer extra_transform_buf;   // uniform: mat4 = 64 bytes
	WGPUBuffer cxform_id_buf;         // uniform: u32 padded to 16 bytes
	WGPUBuffer cxform_uniform_buf;    // uniform: 20 floats (5x4 cxform), 80 bytes

	// --- Textures and samplers ---
	WGPUTexture gradient_tex;
	WGPUTextureView gradient_tex_view;
	WGPUSampler gradient_sampler;

	WGPUSampler bitmap_sampler;
	WGPUSampler bitmap_sampler_linear;  // smoothed bitmap fills (0x40/0x41)

	WGPUTexture dummy_tex;            // 1x1 fallback for empty gradient/bitmap
	WGPUTextureView dummy_tex_view;  // 2D-array view (bitmap fallback)
	WGPUTextureView dummy_tex_2d_view; // 2D view (gradient fallback)
	WGPUSampler dummy_sampler;

	WGPUTexture msaa_texture;
	WGPUTextureView msaa_view;

	WGPUTexture depth_stencil_texture;
	WGPUTextureView depth_stencil_view;

	// --- Pipelines ---
	WGPURenderPipeline render_pipeline;         // normal: no stencil test
	WGPURenderPipeline stencil_write_pipeline;  // raises the clip level, no color output
	WGPURenderPipeline stencil_test_pipeline;   // tests stencil, normal color output
	WGPURenderPipeline stencil_clear_pipeline;  // lowers the clip level, no color output
	WGPURenderPipeline blend_add_pipeline;      // blend mode 8: additive
	WGPURenderPipeline blend_lighten_pipeline;  // blend mode 5: lighten (max)
	WGPURenderPipeline blend_darken_pipeline;   // blend mode 6: darken (min)
	WGPURenderPipeline blend_subtract_pipeline; // blend mode 9: subtract
	WGPURenderPipeline blend_premul_pipeline;   // premultiplied alpha (for attached bitmaps)
	WGPUComputePipeline compute_pipeline;

	// --- Bind group layouts ---
	WGPUBindGroupLayout vertex_storage_bgl;   // group 0
	WGPUBindGroupLayout vertex_uniform_bgl;   // group 1
	WGPUBindGroupLayout fragment_sampler_bgl;  // group 2
	WGPUPipelineLayout render_pipeline_layout;

	WGPUBindGroupLayout compute_read_bgl;     // compute group 0
	WGPUBindGroupLayout compute_write_bgl;    // compute group 1
	WGPUPipelineLayout compute_pipeline_layout;

	// --- Bind groups ---
	WGPUBindGroup vertex_storage_bg;   // group 0: transforms, colors, inv_mats, bitmap_sizes
	WGPUBindGroup vertex_uniform_bg;   // group 1: stage_to_ndc, transform_id
	WGPUBindGroup fragment_sampler_bg; // group 2: gradient tex+samp, bitmap tex+samp

	WGPUBindGroup compute_read_bg;     // compute group 0: uninv_mat_buffer
	WGPUBindGroup compute_write_bg;    // compute group 1: inv_mat_buffer

	// --- Per-frame state ---
	WGPUCommandEncoder encoder;
	WGPURenderPassEncoder render_pass;
	WGPUTextureView surface_view;      // current frame's surface texture view

	// --- Filter resources (lazy-initialized on first filtered object) ---
	int filter_resources_created;
	WGPUTexture filter_tex_a;          // canvas-sized RGBA8 non-MSAA ping-pong A
	WGPUTexture filter_tex_b;          // canvas-sized RGBA8 non-MSAA ping-pong B
	WGPUTextureView filter_view_a;
	WGPUTextureView filter_view_b;
	WGPUTexture filter_msaa_texture;   // separate MSAA 4x texture for offscreen rendering
	WGPUTextureView filter_msaa_view;
	WGPUSampler filter_sampler;        // linear, clamp-to-edge
	WGPURenderPipeline blur_pipeline;
	WGPUBindGroupLayout blur_bgl;
	WGPUPipelineLayout blur_pipeline_layout;
	WGPURenderPipeline composite_pipeline;  // draws filtered result into MSAA main pass
	WGPUBindGroupLayout composite_bgl;
	WGPUPipelineLayout composite_pipeline_layout;
	// --- Filter COMPOSITION (inner / knockout / compositeSource) ---
	// Flash composes a glow/bevel against the UNBLURRED source, which the
	// filter_tex_a -> filter_tex_b ping-pong destroys, so the source is
	// snapshotted before the blur runs. See render_webgpu_compose_filter().
	WGPUTexture filter_src_tex;             // snapshot of the unfiltered source
	WGPUTextureView filter_src_view;
	WGPURenderPipeline compose_pipeline;
	WGPUBindGroupLayout compose_bgl;
	WGPUPipelineLayout compose_pipeline_layout;
	// --- Filter uniform RING ---
	// wgpuQueueWriteBuffer is ordered against Submit, NOT against the commands
	// already encoded on the pending encoder: every write issued during a frame
	// lands before ANY draw in that frame runs. A single re-written uniform
	// buffer therefore serves every filter draw in the frame with the LAST
	// value written. Each write takes its own 256-byte slot instead.
	WGPUBuffer filter_uniform_ring;
	u32 filter_uniform_cursor;             // next free slot, reset per frame

	// --- Blend-mode LAYER compositing (lazy, on the first non-trivial blend) ---
	// Flash blends a display object's whole rendered subtree against the backdrop
	// ONCE instead of per draw call; see render_webgpu_composite_blend().
	int blend_resources_created;
	int offscreen_depth;               // >0 while rendering into filter_tex_a
	WGPUTexture main_color_texture;    // this frame's RESOLVED colour target
	WGPUTexture filter_ds_texture;     // offscreen pass's OWN depth-stencil, so the
	WGPUTextureView filter_ds_view;    //   main pass's clip-mask stencil survives
	WGPUBuffer blend_params_buf;       // 32-byte all-zero uniform for composite_wgsl
	WGPURenderPipeline blend_layer_pipeline[15];   // trivial: screen / add / subtract
	WGPURenderPipeline blend_shader_pipeline[15];  // complex: multiply / lighten / ...
	WGPUBindGroupLayout blend_shader_bgl;
	WGPUPipelineLayout blend_shader_pl;

	// --- Clip-mask stencil nesting (ruffle's num_masks model) ---
	// `mask_ref` is the NESTING LEVEL of the active clip, not a per-mask id:
	// 0 = unclipped, N = N masks are open and content may paint only where all
	// N of them overlap. A mask draw is Equal(N-1)/IncrementClamp, so it can
	// raise only texels the enclosing mask already owns — that IS the
	// intersection, with no geometry algebra; content is Equal(N)/Keep; a pop
	// is a full-screen Equal(N)/DecrementClamp quad, which selects exactly the
	// texels the innermost mask raised and puts them back to N-1 (so a mask
	// leaves NO residue behind for the next one, which the old Replace model
	// relied on a fresh reference to survive). Ports ruffle
	// render/wgpu/src/pipelines.rs:376-414 + surface/commands.rs:399-433.
	// Pops must be LIFO — every caller restores monotonically downward.
	u32 mask_ref;             // nesting level of the ACTIVE clip (0 = none)
	u32 mask_capture_depth;   // >0 while writing mask geometry (nested sprite masks)
	int mask_save_sp;         // depth of the offscreen-pass save stack
	u32 mask_save_ref[8];     // {mask_ref, mask_capture_depth} parked across an
	u32 mask_save_cap[8];     //   offscreen pass, which owns a CLEARED stencil

	// --- SDL window (native only) ---
#if !defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	struct SDL_Window* window;
#endif

#ifdef OFFSCREEN_RENDER
	// --- Headless rendering resources ---
	WGPUTexture offscreen_texture;   // offscreen RGBA8 render target (resolve target)
	WGPUTextureView offscreen_view;  // persistent view of offscreen_texture
	WGPUBuffer readback_buffer;      // staging buffer for GPU→CPU readback
	size_t readback_row_stride;      // bytes per row in readback buffer (256-aligned)
	int capture_requested;           // 1 if next close_pass should copy to readback buffer
#endif

	// --- Browser on-demand framebuffer capture (debug; HAS_DISPLAY_BRIDGE) ---
	// Lets JS read back the rendered frame via copyTextureToBuffer (a DIRECT GPU
	// copy, NOT a present), bypassing the saturated software present queue that
	// makes Playwright/CDP page screenshots hang on a busy board in WSL2 (no dzn
	// Vulkan driver → Chrome WebGPU runs on software). Driven entirely from the
	// render loop so dbgCapturePNG never has to re-enter ASYNCIFY (the main loop
	// already owns the suspended stack). State machine: 0 idle → 1 requested →
	// 2 mapping → 3 ready. See render_webgpu_capture_browser_* and
	// SWFRecomp/wasm_wrappers/display_bridge.c (dbgCapturePNG).
	int browser_capture_state;             // 0=idle 1=requested 2=mapping 3=ready
	int browser_capture_active;            // this frame is resolving into the capture texture
	WGPUTexture browser_capture_texture;   // RenderAttachment|CopySrc resolve target
	WGPUTextureView browser_capture_view;
	WGPUBuffer browser_capture_readback;   // CopyDst|MapRead staging buffer
	size_t browser_capture_row_stride;     // 256-aligned bytes per row
	unsigned char* browser_capture_rgba;   // CPU RGBA result (width*height*4), tightly packed

	// Window background color
	u8 red;
	u8 green;
	u8 blue;

	// Dynamic rendering (text field backgrounds/borders, Drawing API)
	u32 dynamic_vertex_base;    // vertex offset (in vertices) where dynamic area starts
	u32 dynamic_color_base;     // color index where dynamic colors start
	u32 dynamic_rect_count;     // number of dynamic color slots used this frame
	u32 dynamic_vertex_used;    // total dynamic vertices used this frame

	// Per-frame CPU staging for the dynamic vertex/color regions. Each dynamic
	// draw (text glyph, drawing-API shape, bitmap quad) used to issue its OWN tiny
	// wgpuQueueWriteBuffer per shape — hundreds per frame, and the per-call
	// marshalling overhead dominated browser frame CPU (writeBuffer was ~88% on the
	// Minesweeper difficulty screen). Instead, draws now memcpy into these CPU
	// arrays and render_webgpu_close_pass flushes each as a SINGLE writeBuffer of
	// the used range. Same bytes, same offsets → pixel-identical output.
	u32* dyn_vtx_staging;       // [MAX_DYNAMIC_VERTICES * 4] u32 (mirror of dynamic vertex region)
	float* dyn_color_staging;   // [MAX_DYNAMIC_RECTS * 4] float (mirror of dynamic color region)

	// Retained-mode upload skip (browser only). Exact CPU copy of the LAST data
	// uploaded to the dynamic vertex/color regions; render_webgpu_close_pass skips
	// the writeBuffer when this frame's staging is byte-identical (the GPU buffer
	// still holds the retained data → pixel-identical, zero re-upload). NULL in
	// native/OFFSCREEN builds, where the skip is disabled (always upload).
	u32* prev_dyn_vtx;          // [MAX_DYNAMIC_VERTICES * 4] u32, or NULL
	float* prev_dyn_color;      // [MAX_DYNAMIC_RECTS * 4] float, or NULL
	u32 prev_dyn_vtx_used;      // vertex count of the last upload (0 = none/invalid)
	u32 prev_dyn_rect_count;    // color slot count of the last upload (0 = none/invalid)

	// Retained-mode transform-upload skip (browser only). compose_children
	// recomputes and re-uploads EVERY transform slot via render_webgpu_write_transform
	// each frame; a static screen re-uploads ~hundreds of byte-identical 64-byte
	// matrices (each a separate wgpuQueueWriteBuffer that serializes against present
	// on a software backend). xform_mirror holds the last bytes written to each
	// xform_buffer slot; write_transform skips the upload when the slot is unchanged.
	// Invariant: xform_mirror_valid[i] != 0  =>  GPU xform_buffer slot i == xform_mirror[i].
	// NULL in native/OFFSCREEN (skip disabled → graphics-native suite bit-for-bit).
	float* xform_mirror;        // [xform_slot_count * 16] float, or NULL
	u8* xform_mirror_valid;     // [xform_slot_count] u8 (0 = unknown/never written), or NULL

	// Dynamic gradient rendering (Drawing API beginGradientFill/lineGradientStyle)
	u32 static_gradient_count;   // number of gradients from recompiler (static)
	u32 dynamic_gradient_used;   // number of dynamic gradient layers used this frame
	u32 dynamic_gradient_capacity; // max dynamic gradient layers
	u32 static_mat_count;        // number of static gradient matrices

	// Dynamic bitmap rendering (attachBitmap)
	u32 dynamic_bitmap_base;       // first dynamic bitmap SLOT (= original bitmap_count)
	u32 dynamic_bitmap_used;       // dynamic slots used this frame (all pools)
	u32 dynamic_bitmap_capacity;   // per-frame dynamic slots (MAX_DYNAMIC_BITMAP_SLOTS)
	u32 dynamic_bitmap_max_w;      // largest dynamic source accepted (pixels)
	u32 dynamic_bitmap_max_h;      // largest dynamic source accepted (pixels)

	// Renderer initialization status (0 = not ready, 1 = fully initialized)
	int renderer_ok;

	// DisplacementMapFilter (filters cut 2). Built lazily on the first
	// displacement pass in the movie, so an AVM2 movie without one pays
	// nothing. The map texture is recreated only when the map's dimensions
	// change; it is RGBA8Unorm regardless of surface_format so the shader's
	// channel order is pinned.
	int displace_resources_created;
	WGPUBindGroupLayout displace_bgl;
	WGPUPipelineLayout displace_pipeline_layout;
	WGPURenderPipeline displace_pipeline;
	WGPUTexture displace_map_tex;
	WGPUTextureView displace_map_view;
	u32 displace_map_w;
	u32 displace_map_h;

	// Premultiplied-alpha bitmap blend behind the Equal-compare stencil test
	// (s17 P3). Bound instead of blend_premul_pipeline whenever a clip mask is
	// open, so a Bitmap / attachBitmap / beginBitmapFill draw is clipped by the
	// mask the way shape draws already were. See bind_premul_pipeline().
	WGPURenderPipeline blend_premul_stencil_pipeline;

	// Alpha-mask composite (s17 P3). Created alongside compose_pipeline in
	// render_webgpu_ensure_filter_resources and using the SAME compose_bgl /
	// compose_pipeline_layout: binding 0 = maskee layer, binding 3 = mask layer,
	// binding 2 = an unused uniform block the shared layout still demands.
	WGPURenderPipeline alpha_mask_pipeline;
	// One-shot integer texel shift for the NEXT render_webgpu_run_blur call
	// (set by render_webgpu_set_blur_shift, consumed and cleared by run_blur).
	int blur_shift_x;
	int blur_shift_y;
} WebGPURenderContext;

// SWF fill-style byte for a bitmap fill, and the ONLY place a caller should
// decide which bitmap sampler a draw gets. The SWF format spells the smoothing
// flag into the fill type itself (swf.hpp:115-118):
//
//     0x40 repeating, smoothed      0x42 repeating, NOT smoothed
//     0x41 clipped,   smoothed      0x43 clipped,   NOT smoothed
//
// and the recompiler already emits that raw byte as the vertex style type, so
// the fragment shader picks a Linear sampler for 0x40/0x41 and Nearest for
// 0x42/0x43 with no further plumbing. Dynamic (drawing-API / attachBitmap)
// draws build their style word here so they land on the same rule:
// beginBitmapFill(bmp, matrix, repeat, smooth) passes its own flag, and a path
// with NO smoothing information must pass smooth = 0 (Flash's default for
// Bitmap.smoothing and attachBitmap), which is also what keeps those draws
// byte-identical to the pre-2026-08 fixed-Nearest sampler.
static inline u32 render_webgpu_bitmap_fill_style_word(int repeat, int smooth)
{
	return (repeat ? 0x40u : 0x41u) | (smooth ? 0x0u : 0x2u);
}

// --- Public API (matches flashbang.h signatures) ---

WebGPURenderContext* render_webgpu_new(void);
void render_webgpu_init(SWFAppContext* app_context, WebGPURenderContext* context);
int render_webgpu_poll(SWFAppContext* app_context);
void render_webgpu_set_background(WebGPURenderContext* context, u8 r, u8 g, u8 b);
void render_webgpu_upload_bitmap(WebGPURenderContext* context, const u8* pixels, size_t size, u32 width, u32 height);
void render_webgpu_predeclare_bitmap(WebGPURenderContext* ctx, u32 slot,
                                     const u8* pixels, u32 width, u32 height);
void render_webgpu_finalize_bitmaps(WebGPURenderContext* context);
void render_webgpu_open_pass(WebGPURenderContext* context);
// Overwrite the per-pass stage_to_ndc uniform with a caller-composed matrix
// (used to apply _root._x / _root._xscale / etc. on top of the projection).
void render_webgpu_upload_stage_transform(WebGPURenderContext* context, const float matrix[16]);
void render_webgpu_upload_extra_transform_id(WebGPURenderContext* context, u32 transform_id);
void render_webgpu_upload_extra_transform(WebGPURenderContext* context, float* transform);
void render_webgpu_upload_cxform_id(WebGPURenderContext* context, u32 cxform_id);
void render_webgpu_upload_cxform(WebGPURenderContext* context, float* cxform);
void render_webgpu_draw_shape(WebGPURenderContext* context, size_t offset, size_t num_verts, u32 transform_id, u32 cxform_id);
void render_webgpu_draw_rect(WebGPURenderContext* context, float x, float y, float w, float h, float r, float g, float b, float a, u32 transform_id, u32 cxform_id);
void render_webgpu_draw_tris(WebGPURenderContext* context, const float* xy_pairs, u32 vertex_count, float r, float g, float b, float a, u32 transform_id, u32 cxform_id);
void render_webgpu_draw_gradient_tris(WebGPURenderContext* context, const float* xy_pairs, u32 vertex_count, u8 gradient_type, u8 spread_mode, u8 interpolation, float focal_ratio, const u8* gradient_ramp, const float* gradient_matrix, u32 transform_id, u32 cxform_id);
void render_webgpu_draw_bitmap_quad(WebGPURenderContext* context, const uint32_t* argb_pixels, u32 bmp_width, u32 bmp_height, float x_twips, float y_twips, u32 transform_id, u32 cxform_id);
void render_webgpu_draw_bitmap_quad_scaled(WebGPURenderContext* context, const uint32_t* argb_pixels, u32 src_w, u32 src_h, u32 dst_w, u32 dst_h, float x_twips, float y_twips, u32 transform_id, u32 cxform_id);
void render_webgpu_draw_bitmap_tris(WebGPURenderContext* context, const float* xy_pairs, u32 vertex_count, const uint32_t* argb_pixels, u32 src_w, u32 src_h, const float* user_matrix6, int repeat, int smooth, u32 transform_id, u32 cxform_id);
void render_webgpu_begin_clip_mask(WebGPURenderContext* context);
void render_webgpu_end_clip_mask(WebGPURenderContext* context);
void render_webgpu_end_clip(WebGPURenderContext* context);
// Save / restore the ACTIVE clip reference around a nested mask. The stencil
// texels an enclosing mask wrote are still in the buffer (nothing clears them
// mid-pass), so restoring an enclosing clip is just re-binding the stencil-test
// pipeline at its reference — no geometry replay. Used by the AVM2 render walk,
// where a masked subtree can contain another masked node.
u32  render_webgpu_clip_ref(WebGPURenderContext* context);
void render_webgpu_restore_clip(WebGPURenderContext* context, u32 ref);
void render_webgpu_close_pass(WebGPURenderContext* context);
void render_webgpu_compose_text_transforms(WebGPURenderContext* context, const char* transform_data, u32 place_transform_id, u32 glyph_start, size_t count);
void render_webgpu_compose_sprite_transform(WebGPURenderContext* context, const char* transform_data, u32 parent_transform_id, u32 child_transform_id);
void render_webgpu_write_transform(WebGPURenderContext* context, u32 transform_id, const float composed[16]);
void render_webgpu_write_cxform(WebGPURenderContext* context, u32 cxform_slot, const float cxform[20]);
void render_webgpu_update_vertices(WebGPURenderContext* context, size_t byte_offset, const void* data, size_t byte_size);
void render_webgpu_update_colors(WebGPURenderContext* context, size_t byte_offset, const void* data, size_t byte_size);
void render_webgpu_set_blend_mode(WebGPURenderContext* context, u8 blend_mode);
void render_webgpu_suspend_pass(WebGPURenderContext* context);
void render_webgpu_resume_pass(WebGPURenderContext* context);
void render_webgpu_begin_offscreen_pass(WebGPURenderContext* context);
void render_webgpu_end_offscreen_pass(WebGPURenderContext* context);
void render_webgpu_run_blur(WebGPURenderContext* context, float blur_x, float blur_y, u8 quality, float strength, float r, float g, float b, float a, int colorize);
// Make the next run_blur produce blur(x + (sx, sy)) texels instead of blur(x):
// a drop shadow's integer offset folded INTO the blur, so the offset is read
// from the source layer (whose content ends on-stage) rather than from the
// blurred layer (whose tail past the stage edge was never stored).
void render_webgpu_set_blur_shift(WebGPURenderContext* context, int sx, int sy);
void render_webgpu_composite_filtered(WebGPURenderContext* context, float offset_x, float offset_y, float tint_r, float tint_g, float tint_b, float tint_a);
// Copy the offscreen source out of filter_tex_a before the blur ping-pong
// overwrites it. Call with the main pass SUSPENDED, between
// render_webgpu_end_offscreen_pass() and render_webgpu_run_blur().
void render_webgpu_snapshot_filter_source(WebGPURenderContext* context);
// Compose a blurred filter (filter_tex_a) against the snapshotted source per
// Flash's inner/knockout/compositeSource rules. kind 0 = glow/drop-shadow
// (c1 = glow colour, variant = inner), kind 1 = bevel (c1 = highlight,
// c2 = shadow, variant = 0 outer / 1 inner / 2 full). blur_off_* is a UV-space
// offset added to the blurred sample (bevel also samples at -blur_off).
void render_webgpu_compose_filter(WebGPURenderContext* context, int kind,
	float blur_off_x, float blur_off_y,
	float c1r, float c1g, float c1b, float c1a,
	float c2r, float c2g, float c2b, float c2a,
	float strength, int variant, int knockout, int composite_source);
void render_webgpu_ensure_filter_resources(WebGPURenderContext* context);
// Composite an alpha mask (s17 P3): filter_tex_a = maskee layer, filter_src_tex
// = mask layer; draws maskee * mask.a over the RESUMED main pass. Ruffle's
// CommandList::render_alpha_mask (display_object.rs RenderMask::Alpha arm).
void render_webgpu_composite_alpha_mask(WebGPURenderContext* context);
// One DisplacementMapFilter pass over the offscreen layer (filters cut 2).
// Call with the main pass SUSPENDED, in the same slot as render_webgpu_run_blur
// — it consumes filter_tex_a and leaves the result there.
//
// Unlike every other filter pass this one is OBJECT-relative, because ruffle's
// DisplacementMapFilter defines wrap/clamp/ignore, the map offset and the
// out-of-bounds test on the SOURCE RECT. rect_* is the filtered object's screen
// rect in render-target pixels; map_point is in STAGE pixels; viewscale is the
// stage view matrix's scale (ctx->stage_scale), the same factor run_blur
// applies to blur sizes. comp_x/comp_y are BitmapDataChannel bits (1 R, 2 G,
// 4 B, 8 A; anything else = no displacement on that axis) and mode is
// 0 wrap / 1 clamp / 2 ignore / 3 color.
void render_webgpu_run_displacement(WebGPURenderContext* context,
	const uint32_t* map_pixels, u32 map_w, u32 map_h,
	float rect_x, float rect_y, float rect_w, float rect_h,
	float map_point_x, float map_point_y,
	float scale_x, float scale_y, float viewscale,
	int comp_x, int comp_y, int mode,
	float cr, float cg, float cb, float ca);
// Blend-mode layer compositing: 1 when this mode must be rendered to its own
// layer and composited once, 0 when the caller should keep the legacy per-draw
// pipeline (nested layers, or a backdrop we cannot sample).
int  render_webgpu_blend_mode_is_layered(WebGPURenderContext* context, u8 blend_mode);
// Snapshot the frame into filter_tex_b. Call while the main pass is SUSPENDED.
void render_webgpu_capture_backdrop(WebGPURenderContext* context, u8 blend_mode);
// Composite the layer in filter_tex_a into the resumed main pass.
void render_webgpu_composite_blend(WebGPURenderContext* context, u8 blend_mode, u32 stencil_ref);
void render_webgpu_free(SWFAppContext* app_context, WebGPURenderContext* context);

#ifdef OFFSCREEN_RENDER
// Headless rendering: framebuffer capture and PNG output
void render_webgpu_request_capture(WebGPURenderContext* context);
int render_webgpu_save_png(WebGPURenderContext* context, const char* path);
#endif

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
// Browser on-demand framebuffer capture (debug). Request a capture, poll for
// readiness, then read the tightly-packed RGBA result. Driven by the render
// loop (see the browser_capture_* fields). Used by display_bridge.c.
void render_webgpu_request_browser_capture(WebGPURenderContext* context);
int  render_webgpu_browser_capture_ready(WebGPURenderContext* context);   // 1 when RGBA ready
unsigned char* render_webgpu_browser_capture_data(WebGPURenderContext* context); // RGBA ptr or NULL

// Frame oracle for perf levers (-DSWF_FRAME_DIGEST, used only by
// render_webgpu.c's upload/draw macros): an FNV-1a digest of every byte a
// frame uploads (writeBuffer/writeTexture payloads + offsets) and every draw's
// arguments, pushed per submit to window.__swfDigest. The upload stream is
// issued even on a lost device (every call is a valid no-op), so the digest is
// available headless with no pixels. Lives here, not in render_webgpu.c, so the
// default build's assert() __LINE__ constants — and therefore its bytes — do
// not move.
#if defined(SWF_FRAME_DIGEST) && defined(__EMSCRIPTEN__)
#include <emscripten.h>
static uint64_t g_frame_digest = 1469598103934665603ULL;
static inline void frame_digest_bytes(const void* data, size_t n)
{
	const uint8_t* b = (const uint8_t*) data;
	uint64_t h = g_frame_digest;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	g_frame_digest = h;
}
static inline void frame_digest_u64(uint64_t v) { frame_digest_bytes(&v, sizeof(v)); }
#define FRAME_DIGEST_BYTES(d, n) frame_digest_bytes((d), (size_t) (n))
#define FRAME_DIGEST_U64(v) frame_digest_u64((uint64_t) (v))
#define FRAME_DIGEST_DRAW(vc, ic, fv, fi) \
	do { FRAME_DIGEST_U64(vc); FRAME_DIGEST_U64(ic); FRAME_DIGEST_U64(fv); FRAME_DIGEST_U64(fi); } while (0)
#define FRAME_DIGEST_FLUSH() \
	do { EM_ASM({ var a = globalThis.__swfDigest || (globalThis.__swfDigest = []); \
	              a.push(($0 >>> 0).toString(16).padStart(8, '0') + ($1 >>> 0).toString(16).padStart(8, '0')); }, \
	             (uint32_t) (g_frame_digest >> 32), (uint32_t) g_frame_digest); \
	     g_frame_digest = 1469598103934665603ULL; } while (0)
#else
#define FRAME_DIGEST_BYTES(d, n) ((void) 0)
#define FRAME_DIGEST_U64(v) ((void) 0)
#define FRAME_DIGEST_DRAW(vc, ic, fv, fi) ((void) 0)
#define FRAME_DIGEST_FLUSH() ((void) 0)
#endif

#endif
