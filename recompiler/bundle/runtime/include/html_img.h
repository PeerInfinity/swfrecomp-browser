#ifndef HTML_IMG_H
#define HTML_IMG_H

// Single reading of Flash's htmlText `<img>` rules, shared by the AVM1 parser
// (action.c tf_parse_html / tf_serialize_html) and the AVM2 parser
// (avm2_text.c spans_from_html / spans_to_html). Ported from Ruffle
// core/src/html/text_format.rs (8cbada89a..16b334374), with one Flash
// behaviour Ruffle still lacks (its TODO at the `img` arm): numeric attributes
// accept a numeric PREFIX ("40.9px" -> 40.9, serialised WIDTH="40";
// avm1/html_text_img_numeric_attributes is Flash's output for it).
//
// Rules (SWF >= 7; SWF <= 6 drops `<img>` entirely):
//   * an `<img>` without `src` is dropped; with one (even empty) it inserts a
//     single placeholder SPACE whose format is the current format with size 2,
//     and it does not touch the format stack;
//   * src/id are entity-decoded; align is "right" only for exactly "right"
//     (case-sensitive), anything else present is "left";
//   * width/height/hspace/vspace keep their numeric prefix and serialise
//     truncated toward zero; a value with no numeric prefix is dropped;
//   * checkPolicyFile is written only when its value is "true" (any case);
//   * two adjacent images with identical attributes merge into ONE span
//     (one IMG tag, two placeholder spaces). Images are interned in a small
//     per-TU pool, so attribute equality is index equality.
//
// Header-only (static) so it adds no TU to the build lists; each including TU
// owns its own pool (AVM1 runs and AVM2 spans never share images).

#include <stdio.h>
#include <string.h>
#include <math.h>

#define HTML_IMG_SRC_MAX 256
#define HTML_IMG_ID_MAX  128
#define HTML_IMG_POOL    1024

typedef struct HtmlImg
{
	char src[HTML_IMG_SRC_MAX];
	char id[HTML_IMG_ID_MAX];
	double width, height, hspace, vspace;
	unsigned char has_id, has_width, has_height, has_hspace, has_vspace;
	signed char align;  // -1 absent, 0 left, 1 right
	unsigned char check_policy_file;
} HtmlImg;

static HtmlImg g_html_img_pool[HTML_IMG_POOL];
static unsigned g_html_img_count = 0;

static void html_img_init(HtmlImg* img)
{
	memset(img, 0, sizeof(*img));
	img->align = -1;
}

// Numeric prefix: [+-]digits[.digits] (at least one digit). Returns 0 when
// the value has no numeric prefix at all (attribute dropped).
static int html_img_parse_number(const char* s, double* out)
{
	const char* p = s;
	int neg = 0;
	if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
	double v = 0.0;
	int digits = 0;
	while (*p >= '0' && *p <= '9') { v = v * 10.0 + (*p - '0'); p++; digits++; }
	if (*p == '.')
	{
		p++;
		double scale = 0.1;
		while (*p >= '0' && *p <= '9') { v += (*p - '0') * scale; scale *= 0.1; p++; digits++; }
	}
	if (digits == 0) return 0;
	*out = neg ? -v : v;
	return 1;
}

// Attribute setters: `value` is the raw (already entity-decoded for src/id)
// NUL-terminated attribute value.
static void html_img_set_attr(HtmlImg* img, const char* name, const char* value)
{
	if (strcasecmp(name, "src") == 0)
	{
		strncpy(img->src, value, HTML_IMG_SRC_MAX - 1);
		img->src[HTML_IMG_SRC_MAX - 1] = '\0';
	}
	else if (strcasecmp(name, "id") == 0)
	{
		strncpy(img->id, value, HTML_IMG_ID_MAX - 1);
		img->id[HTML_IMG_ID_MAX - 1] = '\0';
		img->has_id = 1;
	}
	else if (strcasecmp(name, "align") == 0)
	{
		img->align = (strcmp(value, "right") == 0) ? 1 : 0;
	}
	else if (strcasecmp(name, "checkpolicyfile") == 0)
	{
		img->check_policy_file = (strcasecmp(value, "true") == 0) ? 1 : 0;
	}
	else
	{
		double* dst = NULL;
		unsigned char* has = NULL;
		if (strcasecmp(name, "width") == 0) { dst = &img->width; has = &img->has_width; }
		else if (strcasecmp(name, "height") == 0) { dst = &img->height; has = &img->has_height; }
		else if (strcasecmp(name, "hspace") == 0) { dst = &img->hspace; has = &img->has_hspace; }
		else if (strcasecmp(name, "vspace") == 0) { dst = &img->vspace; has = &img->has_vspace; }
		if (dst != NULL) *has = (unsigned char) html_img_parse_number(value, dst);
	}
}

static int html_img_equal(const HtmlImg* a, const HtmlImg* b)
{
	return strcmp(a->src, b->src) == 0
	       && a->has_id == b->has_id && (!a->has_id || strcmp(a->id, b->id) == 0)
	       && a->has_width == b->has_width && (!a->has_width || a->width == b->width)
	       && a->has_height == b->has_height && (!a->has_height || a->height == b->height)
	       && a->has_hspace == b->has_hspace && (!a->has_hspace || a->hspace == b->hspace)
	       && a->has_vspace == b->has_vspace && (!a->has_vspace || a->vspace == b->vspace)
	       && a->align == b->align && a->check_policy_file == b->check_policy_file;
}

// Intern an image; returns its 1-based pool index (0 is "no image"). When the
// pool is full it is recycled from the start: stale indices then alias new
// images, which only matters for fields holding images across >1024 parses.
static unsigned short html_img_intern(const HtmlImg* img)
{
	for (unsigned i = 0; i < g_html_img_count; i++)
	{
		if (html_img_equal(&g_html_img_pool[i], img)) return (unsigned short) (i + 1);
	}
	if (g_html_img_count >= HTML_IMG_POOL) g_html_img_count = 0;
	g_html_img_pool[g_html_img_count] = *img;
	return (unsigned short) (++g_html_img_count);
}

static const HtmlImg* html_img_get(unsigned short idx)
{
	if (idx == 0 || idx > HTML_IMG_POOL) return NULL;
	return &g_html_img_pool[idx - 1];
}

// Writes `<IMG SRC=... >` into out (always NUL-terminated). src/id are
// written verbatim (Flash does not re-escape them).
static void html_img_serialize(const HtmlImg* img, char* out, size_t out_size)
{
	size_t n = 0;
#define HTML_IMG_PUT(...) do { \
		if (n < out_size) { \
			int _w = snprintf(out + n, out_size - n, __VA_ARGS__); \
			if (_w > 0) n += (size_t) _w; \
		} \
	} while (0)
	HTML_IMG_PUT("<IMG SRC=\"%s\"", img->src);
	if (img->has_width && isfinite(img->width)) HTML_IMG_PUT(" WIDTH=\"%lld\"", (long long) trunc(img->width));
	if (img->has_height && isfinite(img->height)) HTML_IMG_PUT(" HEIGHT=\"%lld\"", (long long) trunc(img->height));
	if (img->has_id) HTML_IMG_PUT(" ID=\"%s\"", img->id);
	if (img->align >= 0) HTML_IMG_PUT(" ALIGN=\"%s\"", img->align ? "right" : "left");
	if (img->has_vspace && isfinite(img->vspace)) HTML_IMG_PUT(" VSPACE=\"%lld\"", (long long) trunc(img->vspace));
	if (img->has_hspace && isfinite(img->hspace)) HTML_IMG_PUT(" HSPACE=\"%lld\"", (long long) trunc(img->hspace));
	if (img->check_policy_file) HTML_IMG_PUT(" CHECKPOLICYFILE=\"true\"");
	HTML_IMG_PUT(">");
#undef HTML_IMG_PUT
	if (out_size > 0 && n >= out_size) out[out_size - 1] = '\0';
}

#endif // HTML_IMG_H
