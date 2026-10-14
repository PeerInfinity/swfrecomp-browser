#ifndef CURVE_FLATTEN_H
#define CURVE_FLATTEN_H

// Raph Levien parabola-integral flattening for quadratic Beziers, ported from
// lyon_geom 1.0.6 `src/quadratic_bezier.rs` (`FlatteningParameters::new` and
// `t_at_iteration`) — the flattener Ruffle's tessellator actually runs, via
// `FillOptions::DEFAULT_TOLERANCE / scale` (`ruffle/render/src/tessellator.rs`).
//
// Coordinates are in PIXELS, exactly like Ruffle's lyon path (its `point()`
// helper converts Twips -> pixels before handing the path to lyon).
//
// Why this exists: the runtime drawing-API tessellators used to pick a chord
// count from a four-bucket flatness ladder (1 / 4 / 8 / 16 segments), which is
// far finer than lyon's on mid-size arcs and far coarser on large ones. Since
// our chords are inscribed, "finer" means the filled polygon is systematically
// LARGER than Ruffle's, which shows up as one-directional edge pixels at
// MSAA 1. See SWFRecompDocs/plans/session15-fanout-reports/w2-gfx-flatten-report.md.

#include <math.h>

#define CURVE_FLATTEN_TOLERANCE 0.1f   // lyon FillOptions::DEFAULT_TOLERANCE
#define CURVE_FLATTEN_MAX_SEGS  256

typedef struct
{
	int   count;                   // lyon's `count`; 0 => emit one straight chord
	float integral_from;
	float integral_step;
	float inv_integral_from;
	float div_inv_integral_diff;
} CurveFlatten;

// integral (1 + 4x^2) ^ -0.25 dx  (lyon `approx_parabola_integral`)
static inline float curve_flatten_pint(float x)
{
	const float d = 0.67f;
	return x / (1.0f - d + sqrtf(sqrtf(d*d*d*d + 0.25f*x*x)));
}

// inverse of the above (lyon `approx_parabola_inv_integral`)
static inline float curve_flatten_pinv(float x)
{
	const float b = 0.39f;
	return x * (1.0f - b + sqrtf(b*b + 0.25f*x*x));
}

static inline CurveFlatten curve_flatten_init(float x0, float y0,
                                              float cx, float cy,
                                              float x1, float y1,
                                              float tolerance)
{
	CurveFlatten p;
	p.count = 0;
	p.integral_from = 0.0f;
	p.integral_step = 0.0f;
	p.inv_integral_from = 0.0f;
	p.div_inv_integral_diff = 0.0f;

	// lyon `is_linear`: degenerate, or control within 2*tolerance of the chord.
	if (x0 == x1 && y0 == y1)
	{
		return p;
	}
	{
		float bx = x1 - x0, by = y1 - y0;
		float len2 = bx*bx + by*by;
		float cross = (cx - x0) * by - (cy - y0) * bx;
		if (len2 > 0.0f && (cross*cross) / len2 <= tolerance * tolerance * 4.0f)
		{
			return p;
		}
	}

	float ddx = 2.0f*cx - x0 - x1;
	float ddy = 2.0f*cy - y0 - y1;
	float crs = (x1 - x0) * ddy - (y1 - y0) * ddx;
	if (crs == 0.0f)
	{
		return p;
	}
	float inv_crs = 1.0f / crs;
	float parabola_from = ((cx - x0) * ddx + (cy - y0) * ddy) * inv_crs;
	float parabola_to   = ((x1 - cx) * ddx + (y1 - cy) * ddy) * inv_crs;
	float scale = fabsf(crs) / (sqrtf(ddx*ddx + ddy*ddy) * fabsf(parabola_to - parabola_from));

	float integral_from = curve_flatten_pint(parabola_from);
	float integral_to   = curve_flatten_pint(parabola_to);
	float integral_diff = integral_to - integral_from;

	float inv_integral_from = curve_flatten_pinv(integral_from);
	float inv_integral_to   = curve_flatten_pinv(integral_to);

	float count = ceilf(0.5f * fabsf(integral_diff) * sqrtf(scale / tolerance));
	if (!isfinite(count) || count < 1.0f)
	{
		return p;
	}
	if (count > (float) CURVE_FLATTEN_MAX_SEGS)
	{
		count = (float) CURVE_FLATTEN_MAX_SEGS;
	}

	p.count = (int) count;
	p.integral_from = integral_from;
	p.integral_step = integral_diff / count;
	p.inv_integral_from = inv_integral_from;
	p.div_inv_integral_diff = 1.0f / (inv_integral_to - inv_integral_from);
	return p;
}

// lyon `t_at_iteration`. Valid for i in [1, count-1]; the final point is the
// curve's own end point, never a sample.
static inline float curve_flatten_t(const CurveFlatten* p, int i)
{
	float u = curve_flatten_pinv(p->integral_from + p->integral_step * (float) i);
	return (u - p->inv_integral_from) * p->div_inv_integral_diff;
}

// Convenience: number of chords lyon would emit (always >= 1).
static inline int curve_flatten_segs(const CurveFlatten* p)
{
	return (p->count < 1) ? 1 : p->count;
}

#endif // CURVE_FLATTEN_H
