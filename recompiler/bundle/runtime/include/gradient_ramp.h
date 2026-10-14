#ifndef GRADIENT_RAMP_H
#define GRADIENT_RAMP_H

// Single source of truth for the 256-row RGBA8 gradient ramp WALK, ported from
// Ruffle's `render/wgpu/src/mesh.rs::CommonGradient::new` (GRADIENT_SIZE = 256).
//
// Why this exists: we used to have three near-identical *segment-walking*
// builders (recompiler `SWFRecomp/src/swf.cpp::parseFillStyles`, AVM1
// `action.c::drawingGenerateGradientRamp`, AVM2 `avm2_display.c::gfx_gen_ramp`),
// each of which iterated `for ratio in [stop[s-1].ratio, stop[s].ratio]`. That
// is only equivalent to Ruffle for a STRICTLY INCREASING ratio list. SWF does
// not require that, and Ruffle never sorts the records, so:
//
//   * Duplicate ratios: Ruffle's cursor advances at most ONE record per texel
//     and pins the interpolation factor to 0 when `last.ratio == next.ratio`,
//     so k stops sharing a ratio render as k consecutive 1-pixel bands and
//     every later segment starts k-1 texels further along. The segment walkers
//     emitted zero rows (recompiler) or one wrong-coloured row (runtime).
//   * Decreasing ratios: Ruffle's cursor still advances one step and the
//     factor pins to 1.0, producing a plateau of the NEXT record's colour. The
//     segment walkers emitted nothing at all — and the recompiler's row count
//     then no longer summed to 256, which silently shifted every later
//     gradient off its own texture row (`gradient_nonsequential_ratios` emitted
//     336 rows into a 256-row slot).
//
// Building the whole ramp with a single texel-indexed loop makes the "exactly
// 256 rows per gradient" invariant structural instead of a property that has to
// be re-argued at every call site.
//
// SCOPE NOTE: this header deliberately reproduces each call site's EXISTING
// per-channel arithmetic (see the space/rounding modes below) so that for a
// strictly-increasing ratio list every builder is byte-for-byte unchanged. The
// two known per-channel divergences from Ruffle (the recompiler quantising
// linearRGB back to sRGB, and the runtime's +0.5 rounding / u8-quantised linear
// endpoints) are separate legs with their own blast radius.
//
// See SWFRecompDocs/plans/session16-fanout-reports/w2-gfx-gradient-report.md.

#include <math.h>
#include <string.h>

typedef struct
{
	unsigned char ratio;
	unsigned char r, g, b, a;
} GradientRampStop;

// Interpolation-space handling for the three COLOUR channels.
#define GRADIENT_RAMP_SRGB           0 // no conversion (both VMs + recompiler, mode 0)
#define GRADIENT_RAMP_LINEAR         1 // Ruffle exact: float linear endpoints, truncate
#define GRADIENT_RAMP_LINEAR_TO_SRGB 2 // recompiler legacy: lerp linear, back to sRGB, round
#define GRADIENT_RAMP_LINEAR_U8      3 // runtime legacy: u8-quantised linear endpoints, truncate

// Rounding for the SRGB colour path and for the alpha channel (alpha is never
// colour-space converted — Ruffle lerps `color.a` raw).
#define GRADIENT_RAMP_TRUNC 0
#define GRADIENT_RAMP_ROUND 1

static float gradient_ramp_srgb_to_linear(float c)
{
	if (c <= 0.04045f) return c / 12.92f;
	return powf((c + 0.055f) / 1.055f, 2.4f);
}

static float gradient_ramp_linear_to_srgb(float c)
{
	if (c <= 0.0031308f) return c * 12.92f;
	return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static unsigned char gradient_ramp_clamp_u8(float v)
{
	if (v <= 0.0f) return 0;
	if (v >= 255.0f) return 255;
	return (unsigned char) v;
}

static unsigned char gradient_ramp_chan(unsigned char start, unsigned char end,
                                        float t, int space, int rounding)
{
	float s, e, v;

	switch (space)
	{
		case GRADIENT_RAMP_LINEAR:
			// Ruffle: convert = |c| srgb_to_linear(c / 255.0) * 255.0, lerp in
			// that space, `as u8`. The texture stays linear; the shader (or the
			// CPU rasteriser) converts back per pixel.
			s = gradient_ramp_srgb_to_linear(start / 255.0f) * 255.0f;
			e = gradient_ramp_srgb_to_linear(end / 255.0f) * 255.0f;
			return gradient_ramp_clamp_u8(s + t * (e - s));

		case GRADIENT_RAMP_LINEAR_TO_SRGB:
			// swf.cpp::linearRgbLerp — lerp in normalised linear, convert back
			// to sRGB, round. (Always rounds; that is the legacy behaviour.)
			s = gradient_ramp_srgb_to_linear(start / 255.0f);
			e = gradient_ramp_srgb_to_linear(end / 255.0f);
			v = gradient_ramp_linear_to_srgb(s + t * (e - s)) * 255.0f + 0.5f;
			return gradient_ramp_clamp_u8(v);

		case GRADIENT_RAMP_LINEAR_U8:
			// action.c::drawingSrgbToLinearU8 + drawingLinearRgbLerp — endpoints
			// are quantised to u8 linear first, then lerped and truncated.
			s = (float) gradient_ramp_clamp_u8(
				gradient_ramp_srgb_to_linear(start / 255.0f) * 255.0f);
			e = (float) gradient_ramp_clamp_u8(
				gradient_ramp_srgb_to_linear(end / 255.0f) * 255.0f);
			return gradient_ramp_clamp_u8(s + t * (e - s));

		default:
			v = (float) start + t * ((float) end - (float) start);
			if (rounding == GRADIENT_RAMP_ROUND) v += 0.5f;
			return gradient_ramp_clamp_u8(v);
	}
}

// Build the 256-row RGBA8 ramp for `stops[0..n)` into `out` (256 * 4 bytes).
// `stops` is used in FILE ORDER — never sort it, Ruffle does not.
static void gradient_ramp_build(const GradientRampStop* stops, int n,
                                int space, int rounding, unsigned char* out)
{
	int t;
	int last = 0;

	if (n <= 0)
	{
		memset(out, 0, 256 * 4);
		return;
	}

	for (t = 0; t < 256; ++t)
	{
		const GradientRampStop* ls;
		const GradientRampStop* ns;
		int next;
		float a;

		// Ruffle advances the cursor by at most one record per texel.
		if (last + 1 < n && t > (int) stops[last + 1].ratio) last += 1;
		next = last + 1;
		if (next > n - 1) next = n - 1;

		ls = &stops[last];
		ns = &stops[next];

		if (t <= (int) ls->ratio || ls->ratio == ns->ratio)
		{
			// Before the first record, or a duplicate ratio: a full stop of
			// this record's colour for exactly one texel.
			a = 0.0f;
		}
		else if (t > (int) ns->ratio)
		{
			// Past the last record (or a decreasing ratio).
			a = 1.0f;
		}
		else
		{
			a = ((float) t - (float) ls->ratio)
			    / ((float) ns->ratio - (float) ls->ratio);
		}

		out[t * 4 + 0] = gradient_ramp_chan(ls->r, ns->r, a, space, rounding);
		out[t * 4 + 1] = gradient_ramp_chan(ls->g, ns->g, a, space, rounding);
		out[t * 4 + 2] = gradient_ramp_chan(ls->b, ns->b, a, space, rounding);
		out[t * 4 + 3] = gradient_ramp_chan(ls->a, ns->a, a,
		                                    GRADIENT_RAMP_SRGB, rounding);
	}
}

#endif // GRADIENT_RAMP_H
