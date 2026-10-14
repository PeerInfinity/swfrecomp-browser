#pragma once

// Renderer abstraction layer.
// Dispatches to either flashbang (SDL3 GPU) or WebGPU based on USE_WEBGPU flag.
// Used by swf.c and tag.c — these files include this header instead of
// flashbang.h or render_webgpu.h directly.

#include <common.h>

typedef struct SWFAppContext SWFAppContext;

#if defined(USE_WEBGPU)

#include <render_webgpu.h>
typedef WebGPURenderContext RenderContext;

#define renderer_new()                             render_webgpu_new()
#define renderer_init(app, ctx)                    render_webgpu_init(app, ctx)
#define renderer_poll(app)                         render_webgpu_poll(app)
#define renderer_set_background(ctx, r, g, b)      render_webgpu_set_background(ctx, r, g, b)
#define renderer_open_pass(ctx)                    render_webgpu_open_pass(ctx)
#define renderer_upload_stage_transform(ctx, m)    render_webgpu_upload_stage_transform(ctx, m)
#define renderer_draw_shape(ctx, o, n, t, c)       render_webgpu_draw_shape(ctx, o, n, t, c)
#define renderer_draw_rect(ctx, x, y, w, h, r, g, b, a, t, c) render_webgpu_draw_rect(ctx, x, y, w, h, r, g, b, a, t, c)
#define renderer_draw_tris(ctx, v, n, r, g, b, a, t, c) render_webgpu_draw_tris(ctx, v, n, r, g, b, a, t, c)
#define renderer_draw_gradient_tris(ctx, v, n, gt, sm, interp, fr, ramp, mat, t, c) render_webgpu_draw_gradient_tris(ctx, v, n, gt, sm, interp, fr, ramp, mat, t, c)
#define renderer_draw_bitmap_quad(ctx, px, w, h, x, y, t, c) render_webgpu_draw_bitmap_quad(ctx, px, w, h, x, y, t, c)
#define renderer_draw_bitmap_quad_scaled(ctx, px, sw, sh, dw, dh, x, y, t, c) render_webgpu_draw_bitmap_quad_scaled(ctx, px, sw, sh, dw, dh, x, y, t, c)
#define renderer_draw_bitmap_tris(ctx, v, n, px, sw, sh, m, rep, smo, t, c) render_webgpu_draw_bitmap_tris(ctx, v, n, px, sw, sh, m, rep, smo, t, c)
#define renderer_begin_clip_mask(ctx)              render_webgpu_begin_clip_mask(ctx)
#define renderer_end_clip_mask(ctx)                render_webgpu_end_clip_mask(ctx)
#define renderer_end_clip(ctx)                     render_webgpu_end_clip(ctx)
#define renderer_clip_ref(ctx)                     render_webgpu_clip_ref(ctx)
#define renderer_restore_clip(ctx, ref)            render_webgpu_restore_clip(ctx, ref)
#define renderer_close_pass(ctx)                   render_webgpu_close_pass(ctx)
#define renderer_free(app, ctx)                    render_webgpu_free(app, ctx)
#define renderer_upload_bitmap(ctx, px, s, w, h)   render_webgpu_upload_bitmap(ctx, px, s, w, h)
#define renderer_finalize_bitmaps(ctx)             render_webgpu_finalize_bitmaps(ctx)
// Fill one static bitmap slot ahead of the pools being built. Used for a
// loaded child movie's bitmaps, which the child's own defineBitmap would
// otherwise report far too late (multi-SWF render slice).
#define renderer_predeclare_bitmap(ctx, slot, px, w, h) render_webgpu_predeclare_bitmap(ctx, slot, px, w, h)
#define renderer_upload_extra_transform_id(ctx, t) render_webgpu_upload_extra_transform_id(ctx, t)
#define renderer_upload_extra_transform(ctx, xf)   render_webgpu_upload_extra_transform(ctx, xf)
#define renderer_upload_cxform_id(ctx, id)         render_webgpu_upload_cxform_id(ctx, id)
#define renderer_upload_cxform(ctx, cx)            render_webgpu_upload_cxform(ctx, cx)
#define renderer_compose_text_transforms(ctx, td, pt, gs, c) render_webgpu_compose_text_transforms(ctx, td, pt, gs, c)
#define renderer_compose_sprite_transform(ctx, td, pt, ct) render_webgpu_compose_sprite_transform(ctx, td, pt, ct)
#define renderer_write_transform(ctx, id, xf) render_webgpu_write_transform(ctx, id, xf)
#define renderer_write_cxform(ctx, slot, cx) render_webgpu_write_cxform(ctx, slot, cx)
#define renderer_update_vertices(ctx, off, data, sz)  render_webgpu_update_vertices(ctx, off, data, sz)
#define renderer_update_colors(ctx, off, data, sz)    render_webgpu_update_colors(ctx, off, data, sz)
#define renderer_set_blend_mode(ctx, mode)            render_webgpu_set_blend_mode(ctx, mode)
#define renderer_suspend_pass(ctx)                    render_webgpu_suspend_pass(ctx)
#define renderer_resume_pass(ctx)                     render_webgpu_resume_pass(ctx)
#define renderer_begin_offscreen_pass(ctx)            render_webgpu_begin_offscreen_pass(ctx)
#define renderer_end_offscreen_pass(ctx)              render_webgpu_end_offscreen_pass(ctx)
#define renderer_run_blur(ctx, bx, by, q, s, r, g, b, a, c) render_webgpu_run_blur(ctx, bx, by, q, s, r, g, b, a, c)
#define renderer_set_blur_shift(ctx, sx, sy)          render_webgpu_set_blur_shift(ctx, sx, sy)
#define renderer_composite_filtered(ctx, ox, oy, tr, tg, tb, ta) render_webgpu_composite_filtered(ctx, ox, oy, tr, tg, tb, ta)
#define renderer_snapshot_filter_source(ctx)           render_webgpu_snapshot_filter_source(ctx)
#define renderer_compose_filter(ctx, k, ox, oy, c1r, c1g, c1b, c1a, c2r, c2g, c2b, c2a, st, va, ko, cs) \
	render_webgpu_compose_filter(ctx, k, ox, oy, c1r, c1g, c1b, c1a, c2r, c2g, c2b, c2a, st, va, ko, cs)
#define renderer_ensure_filter_resources(ctx)          render_webgpu_ensure_filter_resources(ctx)
// s17 P3: composite the alpha-mask pair (maskee layer x mask layer alpha).
#define renderer_composite_alpha_mask(ctx)             render_webgpu_composite_alpha_mask(ctx)
#define renderer_run_displacement(ctx, mp, mw, mh, rx, ry, rw, rh, px, py, sx, sy, vs, cx, cy, md, cr, cg, cb, ca) \
	render_webgpu_run_displacement(ctx, mp, mw, mh, rx, ry, rw, rh, px, py, sx, sy, vs, cx, cy, md, cr, cg, cb, ca)
#define renderer_blend_mode_is_layered(ctx, m)         render_webgpu_blend_mode_is_layered(ctx, m)
#define renderer_capture_backdrop(ctx, m)              render_webgpu_capture_backdrop(ctx, m)
#define renderer_composite_blend(ctx, m, sr)           render_webgpu_composite_blend(ctx, m, sr)

#ifdef OFFSCREEN_RENDER
#define renderer_request_capture(ctx)                  render_webgpu_request_capture(ctx)
#define renderer_save_png(ctx, path)                   render_webgpu_save_png(ctx, path)
#endif

#else

#include <flashbang.h>
typedef FlashbangContext RenderContext;

#define renderer_new()                             flashbang_new()
#define renderer_init(app, ctx)                    flashbang_init(app, ctx)
#define renderer_poll(app)                         flashbang_poll(app)
#define renderer_set_background(ctx, r, g, b)      flashbang_set_window_background(ctx, r, g, b)
#define renderer_open_pass(ctx)                    flashbang_open_pass(ctx)
#define renderer_upload_stage_transform(ctx, m)    ((void)0)
#define renderer_draw_shape(ctx, o, n, t, c)       flashbang_draw_shape(ctx, o, n, t, c)
#define renderer_draw_rect(ctx, x, y, w, h, r, g, b, a, t, c) ((void)0)
#define renderer_draw_tris(ctx, v, n, r, g, b, a, t, c) ((void)0)
#define renderer_draw_gradient_tris(ctx, v, n, gt, sm, interp, fr, ramp, mat, t, c) ((void)0)
#define renderer_draw_bitmap_quad(ctx, px, w, h, x, y, t, c) ((void)0)
#define renderer_draw_bitmap_quad_scaled(ctx, px, sw, sh, dw, dh, x, y, t, c) ((void)0)
#define renderer_draw_bitmap_tris(ctx, v, n, px, sw, sh, m, rep, smo, t, c) ((void)0)
#define renderer_begin_clip_mask(ctx)              ((void)0)
#define renderer_end_clip_mask(ctx)                ((void)0)
#define renderer_end_clip(ctx)                     ((void)0)
#define renderer_clip_ref(ctx)                     (0u)
#define renderer_restore_clip(ctx, ref)            ((void)0)
#define renderer_close_pass(ctx)                   flashbang_close_pass(ctx)
#define renderer_free(app, ctx)                    flashbang_free(app, ctx)
#define renderer_upload_bitmap(ctx, px, s, w, h)   flashbang_upload_bitmap(ctx, px, s, w, h)
#define renderer_finalize_bitmaps(ctx)             flashbang_finalize_bitmaps(ctx)
#define renderer_predeclare_bitmap(ctx, slot, px, w, h) ((void)0)
#define renderer_upload_extra_transform_id(ctx, t) flashbang_upload_extra_transform_id(ctx, t)
#define renderer_upload_extra_transform(ctx, xf)   flashbang_upload_extra_transform(ctx, xf)
#define renderer_upload_cxform_id(ctx, id)         flashbang_upload_cxform_id(ctx, id)
#define renderer_upload_cxform(ctx, cx)            flashbang_upload_cxform(ctx, cx)
#define renderer_compose_text_transforms(ctx, td, pt, gs, c) ((void)0)
#define renderer_compose_sprite_transform(ctx, td, pt, ct) ((void)0)
#define renderer_write_transform(ctx, id, xf) ((void)0)
#define renderer_write_cxform(ctx, slot, cx) ((void)0)
#define renderer_update_vertices(ctx, off, data, sz)  ((void)0)
#define renderer_update_colors(ctx, off, data, sz)    ((void)0)
#define renderer_set_blend_mode(ctx, mode)            ((void)0)
#define renderer_suspend_pass(ctx)                    ((void)0)
#define renderer_resume_pass(ctx)                     ((void)0)
#define renderer_begin_offscreen_pass(ctx)            ((void)0)
#define renderer_end_offscreen_pass(ctx)              ((void)0)
#define renderer_run_blur(ctx, bx, by, q, s, r, g, b, a, c) ((void)0)
#define renderer_set_blur_shift(ctx, sx, sy)          ((void)0)
#define renderer_composite_filtered(ctx, ox, oy, tr, tg, tb, ta) ((void)0)
#define renderer_snapshot_filter_source(ctx)           ((void)0)
#define renderer_compose_filter(ctx, k, ox, oy, c1r, c1g, c1b, c1a, c2r, c2g, c2b, c2a, st, va, ko, cs) ((void)0)
#define renderer_ensure_filter_resources(ctx)          ((void)0)
#define renderer_composite_alpha_mask(ctx)             ((void)0)
#define renderer_run_displacement(ctx, mp, mw, mh, rx, ry, rw, rh, px, py, sx, sy, vs, cx, cy, md, cr, cg, cb, ca) ((void)0)
#define renderer_blend_mode_is_layered(ctx, m)         (0)
#define renderer_capture_backdrop(ctx, m)              ((void)0)
#define renderer_composite_blend(ctx, m, sr)           ((void)0)

#endif
