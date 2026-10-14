#ifndef AVM2_FILTERS_H
#define AVM2_FILTERS_H

// flash.filters (avm2_filters.c) — the nine filter value classes, the three
// constant bags, and the ONE conversion layer that owns every quantization
// rule.
//
// The structural key (Ruffle core/src/avm2/filters.rs): the AS classes do ZERO
// coercion. They are sealed value bags whose constructors just assign. Every
// clamp/quantize behaviour the corpus grades happens in the
// DisplayObject.filters ROUND TRIP: AS object -> Avm2FilterVal (this file's
// native form, bit-identical to swf::Filter) -> a FRESHLY constructed AS
// object. A script never reads back the object it handed us.

#include <avm2/avm2_abc.h>
#include <avm2/avm2_class.h>
#include <avm2/avm2_value.h>

// 1..8 deliberately match the SWF-tag filter ordering used by Avm2TagFilter
// (and by the AVM1 tag.c side-tables), so a tag import is a straight copy.
typedef enum
{
	AVM2_FILTER_NONE = 0,
	AVM2_FILTER_BLUR = 1,
	AVM2_FILTER_DROP_SHADOW = 2,
	AVM2_FILTER_GLOW = 3,
	AVM2_FILTER_BEVEL = 4,
	AVM2_FILTER_CONVOLUTION = 5,
	AVM2_FILTER_COLOR_MATRIX = 6,
	AVM2_FILTER_GRADIENT_GLOW = 7,
	AVM2_FILTER_GRADIENT_BEVEL = 8,
	AVM2_FILTER_DISPLACEMENT_MAP = 9,
	AVM2_FILTER_SHADER = 10
} Avm2FilterKind;

// Flash keeps at most 16 gradient stops; the SWF tag side-tables cap there too.
#define AVM2_FILTER_MAX_GRAD 16

// The quantized native filter, in swf::Filter's own units — Fixed16 bits for
// blur/angle/distance, Fixed8 bits for strength, a u8 alpha byte, an RGB
// triple. Keeping the fixed-point BITS (rather than a re-derived double) is
// what makes a tag-authored filter and a script-authored one read back
// identically: both reach the AS side through exactly one division.
typedef struct Avm2FilterVal
{
	uint8_t kind;                       // Avm2FilterKind
	int32_t blur_x, blur_y;             // Fixed16
	int32_t angle, distance;            // Fixed16 (radians / pixels)
	int16_t strength;                   // Fixed8
	uint8_t quality;                    // passes, 0..15
	uint8_t inner, knockout, on_top, hide_object;
	uint32_t color;  uint8_t alpha;     // shadow / glow / convolution default
	uint32_t color2; uint8_t alpha2;    // bevel highlight
	// ColorMatrixFilter: always exactly 20 f32s.
	float cm[20];
	// ConvolutionFilter: matrix resized to cols*rows (grows AND truncates).
	uint8_t conv_cols, conv_rows;
	float* conv_matrix; uint32_t conv_len;
	float divisor, bias;
	uint8_t preserve_alpha, clamp;
	// DisplacementMapFilter (mapBitmap always round-trips to null, like Ruffle).
	//
	// `map_bitmap` is kept BY IDENTITY, exactly like ShaderFilter's `shader`,
	// and is deliberately NOT part of the AS round trip: avm2_filter_to_object
	// still writes `mapBitmap = null`, so nothing script-visible changes. The
	// render-time DisplacementMapFilter arm (avm2_display.c) needs the real
	// BitmapData to upload as the map texture, and unlike BitmapData.applyFilter
	// it has no AS filter object in hand — only the stored Avm2FilterVal.
	// GC: marked by avm2_filter_gc_mark for this kind.
	int32_t map_x, map_y;
	Avm2Value map_bitmap;
	uint8_t comp_x, comp_y, dm_mode;    // dm_mode: 0 wrap, 1 clamp, 2 ignore, 3 color
	float scale_x, scale_y;
	// Gradient filters.
	uint8_t grad_count;
	uint32_t grad_colors[AVM2_FILTER_MAX_GRAD];
	uint8_t grad_alphas[AVM2_FILTER_MAX_GRAD];
	uint8_t grad_ratios[AVM2_FILTER_MAX_GRAD];
	// ShaderFilter: the Shader is kept BY IDENTITY across the round trip.
	Avm2Value shader;
	int32_t ext_left, ext_right, ext_top, ext_bottom;
} Avm2FilterVal;

// flash.filters registration. MUST run before avm2_register_pixelbender —
// ShaderFilter extends the BitmapFilter base minted here.
void avm2_register_filters(Avm2Context* ctx);

// The flash.filters.BitmapFilter base. avm2_builtin_class always MINTS (it
// never looks a class up), so any subclass registered in another translation
// unit has to reach the base through this accessor or it silently forks it.
Avm2Class* avm2_filters_bitmapfilter_class(void);

// 1 when `v` is an instance of any BitmapFilter subclass we implement.
int avm2_filter_is_filter(Avm2Context* ctx, Avm2Value v);

// AS object -> quantized native value. Returns 0 if `obj` is not a filter
// (the caller raises #2005); may throw #2008 for a bad DisplacementMapFilter
// mode string. `out` is fully initialised on success.
int avm2_filter_from_object(Avm2Context* ctx, Avm2Object* obj, Avm2FilterVal* out);

// A DisplacementMapFilter's mapBitmap. Avm2FilterVal deliberately does NOT
// carry it (it round-trips to null, exactly like ruffle), but the one consumer
// that needs the real BitmapData -- BitmapData.applyFilter -- has the AS filter
// object in hand, so it reads the map straight off that. Returns null for any
// other filter kind.
Avm2Value avm2_filter_map_bitmap(Avm2Context* ctx, Avm2Object* obj);

// Quantized native value -> a BRAND NEW AS filter object (fresh every call:
// `o.filters === o.filters` is false, and mutating what you assigned has no
// effect on what you read back).
Avm2Value avm2_filter_to_object(Avm2Context* ctx, const Avm2FilterVal* v);

// PlaceObject3 SurfaceFilterList entry -> native value. Pure bit copying: the
// tag already stores the quantized form.
void avm2_filter_from_tag(const Avm2TagFilter* tag, Avm2FilterVal* out);

// Release a native filter's out-of-line convolution matrix.
void avm2_filter_release(Avm2Context* ctx, Avm2FilterVal* v);
// Deep-copy `src` into `dst` (duplicating the convolution matrix).
void avm2_filter_copy(Avm2Context* ctx, Avm2FilterVal* dst, const Avm2FilterVal* src);
// GC: mark the object references a stored filter list holds.
void avm2_filter_gc_mark(const Avm2FilterVal* v, uint32_t count);

// Ruffle bitmap_data.rs generate_filter_rect: every filter kind except
// ShaderFilter is a stub that returns the source rect unchanged; ShaderFilter
// returns the TARGET BitmapData's own rect, ignoring sourceRect entirely.
// Returns 1 when the filter is a ShaderFilter.
int avm2_filter_is_shader_filter(Avm2Context* ctx, Avm2Value v);

#endif // AVM2_FILTERS_H
