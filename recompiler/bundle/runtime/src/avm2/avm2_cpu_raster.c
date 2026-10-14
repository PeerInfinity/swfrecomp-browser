// AVM2 CPU shape rasterizer (T5) — see avm2_cpu_raster.h.
//
// Ported by COPY (never call) from action.c's rasterizeMovieClipToBitmap:
//   - INVERT_2D_AFFINE  (action.c:13314) — verbatim
//   - RASTER_TRI edge-coverage math (action.c:13354) — verbatim (pixel centres
//     at +0.5, sign-normalised barycentric, inside test w0,w1,w2 >= 0)
// The SOLID path reuses that coverage directly. The GRADIENT path reuses the
// coverage + barycentric-weight structure but replaces action.c's Drawing-API
// gradient math (raw +/-16384 twips space) with the shape_data convention the
// WGSL shader uses: the forward gradient matrix (uninv_mat_data) already bakes
// the [0,1] UV normalisation, so inverting its 2D affine and applying it to the
// shape-local vertex gives inv_pos.xy in [0,1] — exactly render_webgpu.c's
// vs_main. Ramp sampling, spread, and the focal formula mirror fs_main /
// sample_gradient (t*256-0.5 sampler phase). Timeline-shape ramps are stored
// sRGB-encoded (swf.cpp linearRgbLerp bakes linear->sRGB back), and the
// per-shape style never sets the is_linear_rgb bit, so NO linear->sRGB pass is
// applied here — matching the shader for timeline shapes.
//
// STORE PARITY (the tranche's central risk): final pixels are premultiplied via
// the SAME forward premul avm2_bitmap.c::premul() uses ((ch*a+127)/255), then
// source-over-blended, so BitmapData.getPixel (which un-premultiplies with
// Flash's brute-forced table) round-trips exactly. We do NOT use action.c's
// premultiplyAlpha.

#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include "avm2/avm2_cpu_raster.h"

// Resident recompiler geometry tables, defined in the generated draws.c and
// linked into every build (both graphics and no-graphics — see verify_output.py
// DRAWS_ARRAY_NAMES). Read directly: NO_GRAPHICS builds have no render
// `context`, and the GPU dispatch reads the very same rows.
// Declared through libswf/generated_data.h so the in-browser graphics HOST
// (-DDYNAMIC_HOST, no generated code linked) sees them as pointers.
#include <libswf/generated_data.h>
GEN_EXTERN_SHAPE_DATA;            // {x_bits(float), y_bits(float), style_packed, style_index}
GEN_EXTERN_COLOR_DATA;            // {r,g,b,a} straight, 0..1
GEN_EXTERN_UNINV_MAT_DATA;        // 16 floats/gradient: forward UV[0,1]->twips (2D affine in 4x4)
GEN_EXTERN_GRADIENT_DATA;         // 256 rows/gradient: {r,g,b,a} u8, sRGB-encoded
// T6 (DefineMorphShape): END vertices paired 1:1 with the shape_data START
// range, and END solid-fill colours indexed by the high 16 bits of style_index.
GEN_EXTERN_MORPH_END_SHAPE_DATA;  // {x,y} twips, END positions
GEN_EXTERN_MORPH_END_COLOR_DATA;  // {r,g,b,a} straight 0..1, END colours

// …and the COMBINED tables those symbols become once a loaded child movie
// contributes geometry (ng_shared.c ng_buildMovieRenderTables). The generated
// symbol then names only the ROOT's PREFIX of the table, while the vertex
// offsets and style ids reaching this file are COMBINED-table indices
// (avm2_display.c resolve_shape_geom / ng_rebase_style_word) — so reading the
// symbol directly would be an out-of-bounds read, not merely a wrong one.
// Each accessor returns NULL when nothing was combined, in which case the
// generated symbol IS the whole table and every read is exactly what it was.
extern const uint32_t (*ng_combinedShapeData(void))[4];
extern const float (*ng_combinedColorData(void))[4];
extern const float (*ng_combinedMorphEndShapeData(void))[2];
extern const float (*ng_combinedMorphEndColorData(void))[4];
extern const uint8_t (*ng_combinedGradientData(void))[4];
extern const float* ng_combinedUninvMatData(void);

static inline const uint32_t (*cpu_shape_table(void))[4]
{
	const uint32_t (*t)[4] = ng_combinedShapeData();
	return t != NULL ? t : (const uint32_t (*)[4]) shape_data;
}
static inline const float (*cpu_color_table(void))[4]
{
	const float (*t)[4] = ng_combinedColorData();
	return t != NULL ? t : (const float (*)[4]) color_data;
}
static inline const float (*cpu_morph_end_shape_table(void))[2]
{
	const float (*t)[2] = ng_combinedMorphEndShapeData();
	return t != NULL ? t : (const float (*)[2]) morph_end_shape_data;
}
static inline const float (*cpu_morph_end_color_table(void))[4]
{
	const float (*t)[4] = ng_combinedMorphEndColorData();
	return t != NULL ? t : (const float (*)[4]) morph_end_color_data;
}
static inline const uint8_t (*cpu_gradient_table(void))[4]
{
	const uint8_t (*t)[4] = ng_combinedGradientData();
	return t != NULL ? t : (const uint8_t (*)[4]) gradient_data;
}
static inline const float* cpu_uninv_mat_table(void)
{
	const float* t = ng_combinedUninvMatData();
	return t != NULL ? t : (const float*) uninv_mat_data;
}

// ---------------------------------------------------------------------------
// Store helpers — must match avm2_bitmap.c premul()/blend_over() exactly.
// ---------------------------------------------------------------------------

// Flash forward premultiply with the TRUE source alpha (avm2_bitmap.c premul()
// with transparency=1). Its inverse is the FLASH_PREMUL_FACTOR table getPixel
// uses, so an opaque pixel round-trips identically.
static inline uint32_t cpu_premul(uint32_t c)
{
	uint32_t a = (c >> 24) & 0xFF;
	uint32_t r = (((c >> 16) & 0xFF) * a + 127) / 255;
	uint32_t g = (((c >>  8) & 0xFF) * a + 127) / 255;
	uint32_t b = (( c        & 0xFF) * a + 127) / 255;
	return (a << 24) | (r << 16) | (g << 8) | b;
}

// Premultiplied source-over (avm2_bitmap.c blend_over): src on top of dest.
static inline uint32_t cpu_blend_over(uint32_t dest, uint32_t src)
{
	uint32_t sa = (src >> 24) & 0xFF, inv = 255 - sa;
	uint32_t r = ((src >> 16) & 0xFF) + (((dest >> 16) & 0xFF) * inv) / 255;
	uint32_t g = ((src >>  8) & 0xFF) + (((dest >>  8) & 0xFF) * inv) / 255;
	uint32_t b = (( src        & 0xFF)) + ((( dest        & 0xFF)) * inv) / 255;
	uint32_t a = sa + (((dest >> 24) & 0xFF) * inv) / 255;
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	if (a > 255) a = 255;
	return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline uint32_t f2b(double x)
{
	if (x <= 0.0) return 0;
	if (x >= 1.0) return 255;
	return (uint32_t) (x * 255.0 + 0.5);
}

static inline float bits_to_f(uint32_t u)
{
	float f;
	__builtin_memcpy(&f, &u, sizeof(f));
	return f;
}

// ---------------------------------------------------------------------------
// Gradient shading — mirrors render_webgpu.c fs_main / sample_gradient exactly.
// ---------------------------------------------------------------------------

// apply_spread: 0 = pad (clamp), 1 = reflect (triangle wave), 2 = repeat (fract).
static inline double grad_spread(double t, uint32_t mode)
{
	if (mode == 1)
	{
		double p = t - 2.0 * floor(t / 2.0);
		return p <= 1.0 ? p : 2.0 - p;
	}
	if (mode == 2) return t - floor(t);
	if (t < 0.0) return 0.0;
	if (t > 1.0) return 1.0;
	return t;
}

// Sample ramp `row` at t in [0,1], matching the shader's hardware-sampler phase
// (u = t*256 - 0.5, clamp-to-edge, manual lerp along U). Returns straight ARGB.
static uint32_t grad_sample(uint32_t row, double t)
{
	if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
	double u = t * 256.0 - 0.5;
	if (u < 0.0) u = 0.0; else if (u > 255.0) u = 255.0;
	int u0 = (int) floor(u);
	int u1 = u0 + 1; if (u1 > 255) u1 = 255;
	double f = u - (double) u0;
	const uint8_t (*grad)[4] = cpu_gradient_table();
	const uint8_t* c0 = grad[row * 256 + u0];
	const uint8_t* c1 = grad[row * 256 + u1];
	uint32_t r = (uint32_t) ((c0[0] * (1.0 - f) + c1[0] * f) + 0.5);
	uint32_t g = (uint32_t) ((c0[1] * (1.0 - f) + c1[1] * f) + 0.5);
	uint32_t b = (uint32_t) ((c0[2] * (1.0 - f) + c1[2] * f) + 0.5);
	uint32_t a = (uint32_t) ((c0[3] * (1.0 - f) + c1[3] * f) + 0.5);
	return (a << 24) | (r << 16) | (g << 8) | b;
}

// gradient parameter t from normalised UV (gu,gv in [0,1]) per fill type.
static double grad_t(uint32_t style_type, double gu, double gv,
                     double focal_z, uint32_t spread)
{
	if (style_type == 0x10)          // linear
		return grad_spread(gu, spread);
	if (style_type == 0x12)          // radial
	{
		double x = gu * 2.0 - 1.0, y = gv * 2.0 - 1.0;
		return grad_spread(sqrt(x * x + y * y), spread);
	}
	// focal (0x13) — Ruffle's focal formula (render_webgpu.c focal_radial_t).
	double fx = focal_z;
	double ux = gu * 2.0 - 1.0, uy = gv * 2.0 - 1.0;
	double dx = fx - ux, dy = 0.0 - uy;
	double l = sqrt(dx * dx + dy * dy);
	if (l < 0.00001) return 0.0;
	dx /= l; dy /= l;
	double denom = sqrt(fmax(1.0 - fx * fx * dy * dy, 0.0)) + fx * dx;
	if (fabs(denom) < 0.00001) return 0.0;
	return grad_spread(l / denom, spread);
}

// ---------------------------------------------------------------------------
// Triangle coverage (ported verbatim from action.c RASTER_TRI). Calls back into
// a shader lambda per covered pixel. `solid_argb` non-zero-flag path avoids the
// per-pixel barycentric for the common solid/stroke case.
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t* buf;
	int W, H;
	// gradient params (unused for solid)
	uint32_t style_type, style_id, spread;
	double focal_z, node_alpha;
	// inverted 2D affine (shape twips -> UV[0,1])
	double ia, ib, ic, id, itx, ity;
	// shape-local vertices (for gradient barycentric interp)
	double sx0, sy0, sx1, sy1, sx2, sy2;
	// solid colour (straight ARGB, node-alpha already applied)
	uint32_t solid_argb;
	int is_gradient;
	// Runtime gradient (T4): when `ramp` is non-NULL the gradient is shaded from
	// this 256*4 RGBA8 buffer instead of the static gradient_data row; inv2d is
	// pre-normalised (shape-local twips -> UV[0,1]); linear_out converts the
	// sampled linear ramp back to sRGB (linearRGB interpolation).
	const uint8_t* ramp;
	int linear_out;
} RasterCtx;

// sRGB EOTF^-1 for linearRGB gradient output (mirror render_webgpu.c fs_main).
static inline double cpu_linear_to_srgb(double c)
{
	if (c <= 0.0031308) return c * 12.92;
	return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// Sample a runtime 256-row RGBA8 ramp at t in [0,1] (hardware-sampler phase,
// clamp-to-edge, manual lerp), optionally converting linear->sRGB. Straight ARGB.
static uint32_t grad_sample_ramp(const uint8_t* ramp, double t, int linear_out)
{
	if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
	double u = t * 256.0 - 0.5;
	if (u < 0.0) u = 0.0; else if (u > 255.0) u = 255.0;
	int u0 = (int) floor(u), u1 = u0 + 1; if (u1 > 255) u1 = 255;
	double f = u - (double) u0;
	const uint8_t* c0 = ramp + u0 * 4;
	const uint8_t* c1 = ramp + u1 * 4;
	double r = c0[0] * (1.0 - f) + c1[0] * f;
	double g = c0[1] * (1.0 - f) + c1[1] * f;
	double b = c0[2] * (1.0 - f) + c1[2] * f;
	double a = c0[3] * (1.0 - f) + c1[3] * f;
	if (linear_out)
	{
		r = cpu_linear_to_srgb(r / 255.0) * 255.0;
		g = cpu_linear_to_srgb(g / 255.0) * 255.0;
		b = cpu_linear_to_srgb(b / 255.0) * 255.0;
	}
	uint32_t ri = (uint32_t) (r + 0.5), gi = (uint32_t) (g + 0.5);
	uint32_t bi = (uint32_t) (b + 0.5), ai = (uint32_t) (a + 0.5);
	if (ri > 255) ri = 255; if (gi > 255) gi = 255;
	if (bi > 255) bi = 255; if (ai > 255) ai = 255;
	return (ai << 24) | (ri << 16) | (gi << 8) | bi;
}

static void raster_tri(RasterCtx* R,
                       double ax, double ay, double bx, double by,
                       double cx, double cy)
{
	uint32_t* buf = R->buf;
	int W = R->W, H = R->H;
	double tx_min = ax < bx ? ax : bx; if (cx < tx_min) tx_min = cx;
	double tx_max = ax > bx ? ax : bx; if (cx > tx_max) tx_max = cx;
	double ty_min = ay < by ? ay : by; if (cy < ty_min) ty_min = cy;
	double ty_max = ay > by ? ay : by; if (cy > ty_max) ty_max = cy;
	int xlo = (int) floor(tx_min - 0.5); if (xlo < 0) xlo = 0;
	int xhi = (int) ceil (tx_max - 0.5); if (xhi > W - 1) xhi = W - 1;
	int ylo = (int) floor(ty_min - 0.5); if (ylo < 0) ylo = 0;
	int yhi = (int) ceil (ty_max - 0.5); if (yhi > H - 1) yhi = H - 1;
	if (xhi < xlo || yhi < ylo) return;
	double sgnd = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
	if (sgnd == 0.0) return;
	double abs_area = sgnd > 0 ? sgnd : -sgnd;
	double sgn = sgnd > 0 ? 1.0 : -1.0;
	double inv_area = 1.0 / abs_area;

	for (int py = ylo; py <= yhi; py++)
	{
		double qy = (double) py + 0.5;
		uint32_t* row = buf + (size_t) py * W;
		for (int px = xlo; px <= xhi; px++)
		{
			double qx = (double) px + 0.5;
			double w0 = ((bx - ax) * (qy - ay) - (by - ay) * (qx - ax)) * sgn;
			double w1 = ((cx - bx) * (qy - by) - (cy - by) * (qx - bx)) * sgn;
			double w2 = ((ax - cx) * (qy - cy) - (ay - cy) * (qx - cx)) * sgn;
			if (w0 < 0 || w1 < 0 || w2 < 0) continue;

			uint32_t straight;
			if (!R->is_gradient)
			{
				straight = R->solid_argb;
			}
			else
			{
				// Barycentric: alphaA = w1, alphaB = w2, alphaC = w0 (action.c
				// RASTER_TRI_GRADIENT ordering). Interpolate shape-local coords,
				// map through the inverted gradient matrix to UV[0,1].
				double aA = w1 * inv_area, aB = w2 * inv_area, aC = w0 * inv_area;
				double sx = aA * R->sx0 + aB * R->sx1 + aC * R->sx2;
				double sy = aA * R->sy0 + aB * R->sy1 + aC * R->sy2;
				double gu = R->ia * sx + R->ic * sy + R->itx;
				double gv = R->ib * sx + R->id * sy + R->ity;
				double t = grad_t(R->style_type, gu, gv, R->focal_z, R->spread);
				uint32_t sampled = R->ramp != NULL
					? grad_sample_ramp(R->ramp, t, R->linear_out)
					: grad_sample(R->style_id, t);
				if (R->node_alpha < 0.999)
				{
					uint32_t sa = (uint32_t) (((sampled >> 24) & 0xFF)
					                          * R->node_alpha + 0.5);
					sampled = (sampled & 0x00FFFFFFu) | (sa << 24);
				}
				straight = sampled;
			}
			row[px] = cpu_blend_over(row[px], cpu_premul(straight));
		}
	}
}

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------
void avm2_cpu_raster_shape(uint32_t* buf, int W, int H, int transparent,
                           uint32_t vert_offset, uint32_t vert_count,
                           double wa, double wb, double wc, double wd,
                           double wtx, double wty, double node_alpha)
{
	(void) transparent;  // blending uses true source alpha; see header note.
	if (buf == NULL || W <= 0 || H <= 0 || vert_count < 3) return;
	if (node_alpha <= 0.0) return;

	const uint32_t (*sd)[4] = cpu_shape_table();
	const float (*cd)[4] = cpu_color_table();
	for (uint32_t t = 0; t + 3 <= vert_count; t += 3)
	{
		uint32_t i0 = vert_offset + t;
		uint32_t style_packed = sd[i0][2];
		uint32_t style_index  = sd[i0][3];
		uint32_t style_type   = style_packed & 0xFFu;

		// Only solid (0x00, incl. strokes) and gradients (0x10/0x12/0x13) this
		// tranche. Bitmap fills (0x40-0x43) and anything else are skipped.
		int is_grad = ((style_type & 0xF0u) == 0x10u);
		if (style_type != 0x00u && !is_grad) continue;

		// Shape-local twips vertices.
		double sx[3], sy[3], dX[3], dY[3];
		for (int k = 0; k < 3; k++)
		{
			uint32_t j = i0 + (uint32_t) k;
			sx[k] = (double) bits_to_f(sd[j][0]);
			sy[k] = (double) bits_to_f(sd[j][1]);
			// world (local twips -> target twips), then /20 -> device pixels.
			dX[k] = (wa * sx[k] + wc * sy[k] + wtx) / 20.0;
			dY[k] = (wb * sx[k] + wd * sy[k] + wty) / 20.0;
		}

		RasterCtx R;
		R.buf = buf; R.W = W; R.H = H;
		R.node_alpha = node_alpha;
		R.ramp = NULL; R.linear_out = 0;   // static path: use gradient_data row

		if (!is_grad)
		{
			uint32_t id = style_index & 0xFFFFu;
			double r = cd[id][0], g = cd[id][1];
			double b = cd[id][2], a = cd[id][3] * node_alpha;
			R.is_gradient = 0;
			R.solid_argb = (f2b(a) << 24) | (f2b(r) << 16) | (f2b(g) << 8) | f2b(b);
			raster_tri(&R, dX[0], dY[0], dX[1], dY[1], dX[2], dY[2]);
			continue;
		}

		// Gradient.
		uint32_t id = style_index & 0xFFFFu;
		uint32_t style_upper = (style_index >> 16) & 0xFFFFu;
		R.is_gradient = 1;
		R.style_type = style_type;
		R.style_id = id;
		R.spread = (style_packed >> 8) & 0x3u;
		R.focal_z = (style_type == 0x13u)
		            ? ((double) style_upper - 32768.0) / 16384.0 : 0.0;
		R.sx0 = sx[0]; R.sy0 = sy[0];
		R.sx1 = sx[1]; R.sy1 = sy[1];
		R.sx2 = sx[2]; R.sy2 = sy[2];

		// INVERT_2D_AFFINE(uninv_mat_data + id*16) — verbatim from action.c.
		const float* m = cpu_uninv_mat_table() + (size_t) id * 16;
		double _ia = m[0],  _ib = m[1];
		double _ic = m[4],  _id = m[5];
		double _itx = m[12], _ity = m[13];
		double _det = _ia * _id - _ib * _ic;
		if (_det == 0.0)
		{
			R.ia = R.ib = R.ic = R.id = R.itx = R.ity = 0.0;
		}
		else
		{
			double _inv = 1.0 / _det;
			R.ia =  _id * _inv;  R.ib = -_ib * _inv;
			R.ic = -_ic * _inv;  R.id =  _ia * _inv;
			R.itx = (_ic * _ity - _id * _itx) * _inv;
			R.ity = (_ib * _itx - _ia * _ity) * _inv;
		}
		raster_tri(&R, dX[0], dY[0], dX[1], dY[1], dX[2], dY[2]);
	}
}

// T4 Part B — rasterize an explicit runtime triangle list with a fill
// descriptor (see avm2_cpu_raster.h). Solid or runtime-ramp gradient; reuses the
// shared raster_tri coverage + shading.
void avm2_cpu_raster_tris(uint32_t* buf, int W, int H, int transparent,
                          const float* xy_twips, uint32_t vert_count,
                          const Avm2GfxFill* fill,
                          double wa, double wb, double wc, double wd,
                          double wtx, double wty, double node_alpha)
{
	(void) transparent;
	if (buf == NULL || W <= 0 || H <= 0 || xy_twips == NULL || fill == NULL) return;
	if (vert_count < 3 || node_alpha <= 0.0) return;

	RasterCtx R;
	R.buf = buf; R.W = W; R.H = H;
	R.node_alpha = node_alpha;
	R.ramp = NULL; R.linear_out = 0;

	if (fill->kind == 2)
	{
		R.is_gradient = 1;
		R.style_type = fill->grad_type;
		R.style_id = 0;
		R.spread = fill->spread;
		R.focal_z = (fill->grad_type == 0x13) ? fill->focal : 0.0;
		R.ramp = fill->ramp;
		R.linear_out = fill->interp == 1;
		R.ia = fill->inv2d[0]; R.ib = fill->inv2d[1];
		R.ic = fill->inv2d[2]; R.id = fill->inv2d[3];
		R.itx = fill->inv2d[4]; R.ity = fill->inv2d[5];
	}
	else
	{
		double a = (double) fill->a * node_alpha;
		R.is_gradient = 0;
		R.solid_argb = (f2b(a) << 24) | (f2b(fill->r) << 16)
		             | (f2b(fill->g) << 8) | f2b(fill->b);
	}

	for (uint32_t t = 0; t + 3 <= vert_count; t += 3)
	{
		double sx[3], sy[3], dX[3], dY[3];
		for (int k = 0; k < 3; k++)
		{
			sx[k] = xy_twips[(t + k) * 2];
			sy[k] = xy_twips[(t + k) * 2 + 1];
			dX[k] = (wa * sx[k] + wc * sy[k] + wtx) / 20.0;
			dY[k] = (wb * sx[k] + wd * sy[k] + wty) / 20.0;
		}
		if (R.is_gradient)
		{
			R.sx0 = sx[0]; R.sy0 = sy[0]; R.sx1 = sx[1]; R.sy1 = sy[1];
			R.sx2 = sx[2]; R.sy2 = sy[2];
		}
		raster_tri(&R, dX[0], dY[0], dX[1], dY[1], dX[2], dY[2]);
	}
}

// T6 — rasterize a DefineMorphShape at placement `ratio` (0..1). Each START
// vertex (shape_data[vert_offset + n]) is paired 1:1 with its END vertex
// (morph_end_shape_data[morph_end_offset + n]); position = lerp(start, end,
// ratio). SOLID fills lerp colour too: color_data[low16] (start) ->
// morph_end_color_data[high16] (end). Gradient / stroke morph is deferred (see
// avm2-vector-rendering-plan.md T6 RESULT) — those triangles are skipped.
// Mirrors avm2_cpu_raster_shape's coverage + store path (the CPU twin of the
// GPU avm2_render_morph).
void avm2_cpu_raster_morph(uint32_t* buf, int W, int H, int transparent,
                           uint32_t vert_offset, uint32_t vert_count,
                           uint32_t morph_end_offset, double ratio,
                           double wa, double wb, double wc, double wd,
                           double wtx, double wty, double node_alpha)
{
	(void) transparent;
	if (buf == NULL || W <= 0 || H <= 0 || vert_count < 3) return;
	if (node_alpha <= 0.0) return;
	if (ratio < 0.0) ratio = 0.0; else if (ratio > 1.0) ratio = 1.0;
	// Ruffle morph_shape.rs weights: b weights END, a weights START (a+b=1).
	double b_w = ratio, a_w = 1.0 - ratio;

	const uint32_t (*sd)[4] = cpu_shape_table();
	const float (*cd)[4] = cpu_color_table();
	const float (*med)[2] = cpu_morph_end_shape_table();
	const float (*mecd)[4] = cpu_morph_end_color_table();
	for (uint32_t t = 0; t + 3 <= vert_count; t += 3)
	{
		uint32_t i0 = vert_offset + t;
		uint32_t style_packed = sd[i0][2];
		uint32_t style_index  = sd[i0][3];
		uint32_t style_type   = style_packed & 0xFFu;

		// Solid morph only this tranche (gradient/stroke morph deferred).
		if (style_type != 0x00u) continue;

		double dX[3], dY[3];
		for (int k = 0; k < 3; k++)
		{
			uint32_t sj = i0 + (uint32_t) k;
			uint32_t ej = morph_end_offset + t + (uint32_t) k;
			double sxs = (double) bits_to_f(sd[sj][0]);
			double sys = (double) bits_to_f(sd[sj][1]);
			double sxe = (double) med[ej][0];
			double sye = (double) med[ej][1];
			// Ruffle lerp_twips: round(start*a + end*b) to integer twips.
			double sx = floor(sxs * a_w + sxe * b_w + 0.5);
			double sy = floor(sys * a_w + sye * b_w + 0.5);
			dX[k] = (wa * sx + wc * sy + wtx) / 20.0;
			dY[k] = (wb * sx + wd * sy + wty) / 20.0;
		}

		// Ruffle lerp_color: (a*start + b*end) as u8 — TRUNCATION, per channel.
		// color_data / morph_end_color_data hold the u8 SWF colour / 255.
		uint32_t sid = style_index & 0xFFFFu;
		uint32_t eid = (style_index >> 16) & 0xFFFFu;
		uint32_t sr = f2b(cd[sid][0]), er = f2b(mecd[eid][0]);
		uint32_t sg = f2b(cd[sid][1]), eg = f2b(mecd[eid][1]);
		uint32_t sb = f2b(cd[sid][2]), eb = f2b(mecd[eid][2]);
		uint32_t sa = f2b(cd[sid][3]), ea = f2b(mecd[eid][3]);
		uint32_t cr = (uint32_t) (a_w * sr + b_w * er);
		uint32_t cg = (uint32_t) (a_w * sg + b_w * eg);
		uint32_t cb = (uint32_t) (a_w * sb + b_w * eb);
		uint32_t ca = (uint32_t) (a_w * sa + b_w * ea);
		// node_alpha scales the (straight) alpha, matching the solid shape path.
		uint32_t a_out = f2b((double) ca / 255.0 * node_alpha);

		RasterCtx R;
		R.buf = buf; R.W = W; R.H = H;
		R.node_alpha = node_alpha;
		R.ramp = NULL; R.linear_out = 0;
		R.is_gradient = 0;
		R.solid_argb = (a_out << 24) | (cr << 16) | (cg << 8) | cb;
		raster_tri(&R, dX[0], dY[0], dX[1], dY[1], dX[2], dY[2]);
	}
}
