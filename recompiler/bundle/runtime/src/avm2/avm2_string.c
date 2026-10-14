// String builtin class + methods (port of Ruffle globals/string.rs).
//
// Representation note: Avm2String holds canonical WTF-8 (see the long comment
// above avm2_string_concat in avm2_value.c) — UTF-8 extended so an unpaired
// UTF-16 surrogate can be stored as a 3-byte ED A0..BF xx sequence, with a
// high+low neighbour pair always folded into the 4-byte astral form. Every
// index and length below is in UTF-16 code units, decoded from those bytes.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <memory/heap.h>
#include <avm2/avm2_ops.h>

#include "unicode_case_tables.h"

static const Avm2String* this_string(Avm2Activation* act)
{
	return avm2_coerce_to_string(act->ctx, act->this_val);
}

static double arg_f64_def(Avm2Activation* act, uint32_t i, double def)
{
	// Defaults substitute only for MISSING args; an explicit undefined
	// coerces to NaN (slice(2, undefined) → "").
	if (i >= act->argc) return def;
	return avm2_coerce_to_number(act->ctx, act->args[i]);
}

// string_index (Ruffle): clamp to [0, len].
static uint32_t string_index(double i, uint32_t len)
{
	if (i == INFINITY) return len;
	if (i < 0.0 || isnan(i)) return 0;
	double v = trunc(i);
	return v > (double) len ? len : (uint32_t) v;
}

// string_wrapping_index (Ruffle): <= -1 counts back from len.
static uint32_t string_wrapping_index(double i, uint32_t len)
{
	if (i <= -1.0)
	{
		if (isinf(i)) return 0;
		double off = -trunc(i);
		if (off >= (double) len) return 0;
		return len - (uint32_t) off;
	}
	if (isinf(i)) return len;
	if (isnan(i) || i < 0.0) return 0;
	double v = trunc(i);
	return v > (double) len ? len : (uint32_t) v;
}

static Avm2Value make_str(Avm2Context* ctx, const char* s, uint32_t len)
{
	return avm2_string(avm2_string_new(ctx, s, len));
}


// UTF-16 view over the UTF-8 bytes: AVM2 string indexing/length is in
// UTF-16 code units. ASCII is the common fast path.
static uint32_t utf16_length(const Avm2String* s)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < s->len; )
	{
		unsigned char c = (unsigned char) s->utf8[i];
		if (c < 0x80) { i += 1; n += 1; }
		else if (c < 0xE0) { i += 2; n += 1; }
		else if (c < 0xF0) { i += 3; n += 1; }
		else { i += 4; n += 2; }
	}
	return n;
}

// Decode the UTF-16 code unit at `idx`; also reports the byte range of
// the codepoint containing it (for charAt). Returns -1 when out of range.
static int32_t utf16_unit_at(const Avm2String* s, uint32_t idx,
                             uint32_t* byte_start, uint32_t* byte_len)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < s->len; )
	{
		unsigned char c = (unsigned char) s->utf8[i];
		uint32_t cp;
		uint32_t clen;
		if (c < 0x80) { cp = c; clen = 1; }
		else if (c < 0xE0) { cp = c & 0x1F; clen = 2; }
		else if (c < 0xF0) { cp = c & 0x0F; clen = 3; }
		else { cp = c & 0x07; clen = 4; }
		for (uint32_t k = 1; k < clen && i + k < s->len; k++)
		{
			cp = (cp << 6) | ((unsigned char) s->utf8[i + k] & 0x3F);
		}
		uint32_t units = clen == 4 ? 2 : 1;
		if (idx < n + units)
		{
			if (byte_start != NULL) *byte_start = i;
			if (byte_len != NULL) *byte_len = clen;
			if (units == 1) return (int32_t) cp;
			// Surrogate pair halves.
			cp -= 0x10000;
			return idx == n ? (int32_t) (0xD800 + (cp >> 10))
			                : (int32_t) (0xDC00 + (cp & 0x3FF));
		}
		n += units;
		i += clen;
	}
	return -1;
}


// Map a UTF-16 index to a byte offset (clamped to s->len).
static uint32_t utf16_to_byte(const Avm2String* s, uint32_t u16idx)
{
	uint32_t n = 0;
	uint32_t i = 0;
	while (i < s->len && n < u16idx)
	{
		unsigned char c = (unsigned char) s->utf8[i];
		if (c < 0x80) { i += 1; n += 1; }
		else if (c < 0xE0) { i += 2; n += 1; }
		else if (c < 0xF0) { i += 3; n += 1; }
		else { i += 4; n += 2; }
	}
	return i > s->len ? s->len : i;
}

// Write one UTF-16 code unit as (W)TF-8: a surrogate becomes its 3-byte WTF-8
// form, everything else its ordinary UTF-8 form. Returns the byte count.
static uint32_t wtf8_put_unit(char* out, uint32_t unit)
{
	if (unit < 0x80) { out[0] = (char) unit; return 1; }
	if (unit < 0x800)
	{
		out[0] = (char) (0xC0 | (unit >> 6));
		out[1] = (char) (0x80 | (unit & 0x3F));
		return 2;
	}
	out[0] = (char) (0xE0 | (unit >> 12));
	out[1] = (char) (0x80 | ((unit >> 6) & 0x3F));
	out[2] = (char) (0x80 | (unit & 0x3F));
	return 3;
}

// Extract the UTF-16 code-unit range [start, end) as canonical WTF-8.
//
// The only interesting case is a range boundary that falls INSIDE an astral
// code point: `"\u{20B9F}".substring(0, 1)` is one lone high surrogate, not the
// whole character. Ranges that don't split anything are a plain byte copy.
static Avm2Value make_sub16(Avm2Context* ctx, const Avm2String* s,
                            uint32_t start, uint32_t end)
{
	if (end <= start) return make_str(ctx, "", 0);
	// A split astral pair costs at most 3 bytes per side against the 4 it
	// replaces, so the output is never longer than the input + 2.
	char* out = avm2_alloc(ctx, s->len + 3);
	uint32_t n = 0;
	uint32_t u = 0;  // code-unit index of the codepoint at byte i
	for (uint32_t i = 0; i < s->len && u < end; )
	{
		unsigned char c = (unsigned char) s->utf8[i];
		uint32_t clen;
		if (c < 0x80) clen = 1;
		else if (c < 0xE0) clen = 2;
		else if (c < 0xF0) clen = 3;
		else clen = 4;
		uint32_t units = clen == 4 ? 2 : 1;
		if (u + units > start)
		{
			if (units == 1 || (u >= start && u + units <= end))
			{
				memcpy(out + n, s->utf8 + i, clen);
				n += clen;
			}
			else
			{
				// Astral codepoint clipped on one side: emit the surviving
				// half as a lone surrogate.
				uint32_t cp = (uint32_t) (c & 0x07);
				for (uint32_t k = 1; k < clen && i + k < s->len; k++)
				{
					cp = (cp << 6) | ((unsigned char) s->utf8[i + k] & 0x3F);
				}
				cp -= 0x10000;
				uint32_t unit = u >= start ? (0xD800 + (cp >> 10))
				                           : (0xDC00 + (cp & 0x3FF));
				n += wtf8_put_unit(out + n, unit);
			}
		}
		u += units;
		i += clen;
	}
	{ Avm2Value r_ = make_str(ctx, out, n); heap_free(ctx->app, out); return r_; }  // copied: scratch
}

// Map a byte offset to a UTF-16 index.
static uint32_t byte_to_utf16(const Avm2String* s, uint32_t byteoff)
{
	uint32_t n = 0;
	uint32_t i = 0;
	while (i < s->len && i < byteoff)
	{
		unsigned char c = (unsigned char) s->utf8[i];
		if (c < 0x80) { i += 1; n += 1; }
		else if (c < 0xE0) { i += 2; n += 1; }
		else if (c < 0xF0) { i += 3; n += 1; }
		else { i += 4; n += 2; }
	}
	return n;
}

// ---------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------

static Avm2Value string_get_length(Avm2Activation* act)
{
	const Avm2String* s = this_string(act);
	return avm2_integer((int32_t) utf16_length(s));
}

static Avm2Value string_char_at(Avm2Activation* act)
{
	const Avm2String* s = this_string(act);
	double n = arg_f64_def(act, 0, 0.0);
	if (n < 0.0) return make_str(act->ctx, "", 0);
	uint32_t index = isnan(n) ? 0 : (uint32_t) n;
	uint32_t bs = 0;
	uint32_t bl = 0;
	int32_t unit = (n >= 4294967295.0) ? -1 : utf16_unit_at(s, index, &bs, &bl);
	if (unit < 0)
	{
		return make_str(act->ctx, "", 0);
	}
	if (bl == 4)
	{
		// Astral codepoint: charAt yields ONE code unit — the lone surrogate
		// half, stored in its WTF-8 form.
		char buf[4];
		uint32_t bn = wtf8_put_unit(buf, (uint32_t) unit);
		return make_str(act->ctx, buf, bn);
	}
	return make_str(act->ctx, s->utf8 + bs, bl);
}

static Avm2Value string_char_code_at(Avm2Activation* act)
{
	const Avm2String* s = this_string(act);
	double n = arg_f64_def(act, 0, 0.0);
	if (n < 0.0) return avm2_number(NAN);
	uint32_t index = isnan(n) ? 0 : (uint32_t) n;
	int32_t unit = (n >= 4294967295.0) ? -1 : utf16_unit_at(s, index, NULL, NULL);
	if (unit < 0) return avm2_number(NAN);
	return avm2_integer(unit);
}

static Avm2Value string_concat_method(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* ret = this_string(act);
	for (uint32_t i = 0; i < act->argc; i++)
	{
		ret = avm2_string_concat(ctx, ret, avm2_coerce_to_string(ctx, act->args[i]));
	}
	return avm2_string(ret);
}

static Avm2Value string_from_char_code(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	char* out = avm2_alloc(ctx, act->argc * 4 + 1);
	uint32_t n = 0;
	for (uint32_t i = 0; i < act->argc; i++)
	{
		uint32_t cp = (uint16_t) avm2_coerce_to_u32(ctx, act->args[i]);
		// Combine surrogate pairs into one code point.
		if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < act->argc)
		{
			uint16_t lo = (uint16_t) avm2_coerce_to_u32(ctx, act->args[i + 1]);
			if (lo >= 0xDC00 && lo <= 0xDFFF)
			{
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				i++;
			}
		}
		// A surrogate that found no partner stays a lone surrogate: it is a
		// legal AVM2 code unit and survives in the WTF-8 storage, so
		// `fromCharCode(0xDC00).charCodeAt(0) === 0xDC00`
		// (ecma3/String/e15_5_4_5_4) and `encodeURI` can still see it and
		// throw #1052 (as3/RuntimeErrors/Error1052InvalidUriPassed).
		if (cp < 0x80) out[n++] = (char) cp;
		else if (cp < 0x800)
		{
			out[n++] = (char) (0xC0 | (cp >> 6));
			out[n++] = (char) (0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000)
		{
			out[n++] = (char) (0xE0 | (cp >> 12));
			out[n++] = (char) (0x80 | ((cp >> 6) & 0x3F));
			out[n++] = (char) (0x80 | (cp & 0x3F));
		}
		else
		{
			out[n++] = (char) (0xF0 | (cp >> 18));
			out[n++] = (char) (0x80 | ((cp >> 12) & 0x3F));
			out[n++] = (char) (0x80 | ((cp >> 6) & 0x3F));
			out[n++] = (char) (0x80 | (cp & 0x3F));
		}
	}
	{ Avm2Value r_ = make_str(ctx, out, n); heap_free(ctx->app, out); return r_; }  // copied: scratch
}

static int64_t find_sub(const Avm2String* s, uint32_t from, const Avm2String* pat)
{
	if (pat->len > s->len) return -1;
	for (uint32_t i = from; i + pat->len <= s->len; i++)
	{
		if (memcmp(s->utf8 + i, pat->utf8, pat->len) == 0) return (int64_t) i;
	}
	return -1;
}

static Avm2Value string_index_of(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* s = this_string(act);
	if (act->argc == 0 || act->args[0].kind == AVM2_VALUE_NULL
	    || act->args[0].kind == AVM2_VALUE_UNDEFINED)
	{
		return avm2_integer(-1);
	}
	const Avm2String* pat = avm2_coerce_to_string(ctx, act->args[0]);
	uint32_t start16 = string_index(arg_f64_def(act, 1, 0.0), utf16_length(s));
	int64_t found = find_sub(s, utf16_to_byte(s, start16), pat);
	return avm2_integer(found < 0 ? -1
	                              : (int32_t) byte_to_utf16(s, (uint32_t) found));
}

static Avm2Value string_last_index_of(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* s = this_string(act);
	if (act->argc == 0 || act->args[0].kind == AVM2_VALUE_NULL
	    || act->args[0].kind == AVM2_VALUE_UNDEFINED)
	{
		return avm2_integer(-1);
	}
	const Avm2String* pat = avm2_coerce_to_string(ctx, act->args[0]);
	double startf = arg_f64_def(act, 1, NAN);
	uint32_t end;  // exclusive upper bound of the searched prefix
	if (isnan(startf) || startf == INFINITY)
	{
		end = s->len;
	}
	else if (startf == -INFINITY)
	{
		return avm2_integer(-1);
	}
	else
	{
		// Rust `as i32` saturates (lastIndexOf("x", 4294967300) → i32::MAX).
		int32_t si = startf >= 2147483647.0 ? 2147483647
		           : (startf <= -2147483648.0 ? (int32_t) -2147483648LL
		                                      : (int32_t) startf);
		if (si < 0) return avm2_integer(-1);
		uint64_t e = (uint64_t) utf16_to_byte(s, (uint32_t) si) + pat->len;
		end = e > s->len ? s->len : (uint32_t) e;
	}
	// rfind within s[..end].
	if (pat->len <= end)
	{
		for (int64_t i = (int64_t) (end - pat->len); i >= 0; i--)
		{
			if (memcmp(s->utf8 + i, pat->utf8, pat->len) == 0)
			{
				return avm2_integer((int32_t) byte_to_utf16(s, (uint32_t) i));
			}
		}
	}
	return avm2_integer(-1);
}

static Avm2Value string_locale_compare(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	// Unlike the variadic ES3 String methods, avmplus declares localeCompare
	// as `localeCompare(other:String):int` with no rest arg, so an extra
	// argument is an arity error rather than being ignored
	// (ecma3/String/localeCompare_rt calls it with two).
	if (act->argc > 1)
	{
		avm2_throw_error(ctx, ctx->builtins.argument_error_class,
		                 "Error #1063: Argument count mismatch on "
		                 "String/localeCompare(). Expected 1, got %u.",
		                 act->argc);
	}
	const Avm2String* s = this_string(act);
	Avm2Value other_v = act->argc > 0 ? act->args[0] : avm2_undefined();
	if (ctx->swf_version < 11
	    && (other_v.kind == AVM2_VALUE_NULL
	        || other_v.kind == AVM2_VALUE_UNDEFINED))
	{
		// avmplus bug 585791: before SWF11, a null/undefined argument is not
		// coerced to "null"/"undefined" at all — the comparison answers 1 for
		// an empty receiver and 0 for any other. Ruffle globals/string.rs
		// locale_compare gates the same quirk on version() < 11;
		// as3/String/localeCompare_585791/{v9,v12} pin both sides.
		return avm2_integer(s->len == 0 ? 1 : 0);
	}
	const Avm2String* other = avm2_coerce_to_string(ctx, other_v);
	uint32_t la = utf16_length(s);
	uint32_t lb = utf16_length(other);
	uint32_t n = la < lb ? la : lb;
	for (uint32_t i = 0; i < n; i++)
	{
		int32_t ua = utf16_unit_at(s, i, NULL, NULL);
		int32_t ub = utf16_unit_at(other, i, NULL, NULL);
		if (ua != ub) return avm2_integer(ua - ub);
	}
	if (la < lb) return avm2_integer(-1);
	if (la > lb) return avm2_integer(1);
	return avm2_integer(0);
}

static Avm2Value string_slice(Avm2Activation* act)
{
	const Avm2String* s = this_string(act);
	uint32_t len16 = utf16_length(s);
	uint32_t start = string_wrapping_index(arg_f64_def(act, 0, 0.0), len16);
	uint32_t end = string_wrapping_index(arg_f64_def(act, 1, (double) 0x7fffffff), len16);
	return make_sub16(act->ctx, s, start, end);
}

Avm2Value avm2_string_split_plain(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* s = this_string(act);
	Avm2Value delim_v = act->argc > 0 ? act->args[0] : avm2_undefined();
	uint32_t limit = 0xFFFFFFFFu;
	if (act->argc > 1 && act->args[1].kind != AVM2_VALUE_UNDEFINED)
	{
		limit = avm2_coerce_to_u32(ctx, act->args[1]);
	}
	Avm2Object* arr = avm2_array_new(ctx, 0);
	if (limit == 0)
	{
		return avm2_object_value(arr);
	}
	// ES3 15.5.4.14: only a *missing* separator returns the whole string.
	// An explicit `undefined` is ToString'd like any other value, so
	// "…".split(void 0) splits on the literal "undefined"
	// (ecma3/String/e15_5_4_8_2 pins this).
	if (act->argc == 0)
	{
		avm2_array_push(ctx, arr, avm2_string(s));
		return avm2_object_value(arr);
	}
	const Avm2String* delim = avm2_coerce_to_string(ctx, delim_v);
	if (delim->len == 0)
	{
		if (s->len == 0)
		{
			avm2_array_push(ctx, arr, avm2_string(avm2_string_from_literal(ctx, "")));
			return avm2_object_value(arr);
		}
		// One element per UTF-16 code unit, NOT per UTF-8 byte: mirrors
		// charAt above, including the astral case (a 4-byte codepoint is
		// two units, each a lone surrogate in WTF-8 form).
		uint32_t len16 = utf16_length(s);
		for (uint32_t i = 0; i < len16 && i < limit; i++)
		{
			uint32_t bs = 0;
			uint32_t bl = 0;
			int32_t unit = utf16_unit_at(s, i, &bs, &bl);
			if (unit < 0) break;
			if (bl == 4)
			{
				char buf[4];
				uint32_t bn = wtf8_put_unit(buf, (uint32_t) unit);
				avm2_array_push(ctx, arr, make_str(ctx, buf, bn));
			}
			else
			{
				avm2_array_push(ctx, arr, make_str(ctx, s->utf8 + bs, bl));
			}
		}
		return avm2_object_value(arr);
	}
	uint32_t pos = 0;
	uint32_t count = 0;
	while (count < limit)
	{
		int64_t next = find_sub(s, pos, delim);
		if (next < 0)
		{
			avm2_array_push(ctx, arr, make_str(ctx, s->utf8 + pos, s->len - pos));
			count++;
			break;
		}
		avm2_array_push(ctx, arr, make_str(ctx, s->utf8 + pos, (uint32_t) next - pos));
		count++;
		pos = (uint32_t) next + delim->len;
	}
	return avm2_object_value(arr);
}

static Avm2Value string_substr(Avm2Activation* act)
{
	const Avm2String* s = this_string(act);
	uint32_t start = string_wrapping_index(arg_f64_def(act, 0, 0.0), utf16_length(s));
	double lenf = arg_f64_def(act, 1, (double) 0x7fffffff);

	if (isnan(lenf)) lenf = 0.0;
	else if (lenf > (double) 0x7fffffff) lenf = (double) 0x7fffffff;

	if (lenf < 0.0)
	{
		if (isinf(lenf))
		{
			lenf = 0.0;
		}
		else if (lenf <= -1.0)
		{
			double wrapped = (double) utf16_length(s) + trunc(lenf);
			if (wrapped + (double) start >= (double) utf16_length(s) || wrapped < 0.0)
			{
				return make_str(act->ctx, "", 0);
			}
			lenf = wrapped;
		}
		else
		{
			lenf = 0.0;  // (-1, 0) truncates to 0
		}
	}
	uint64_t end64 = (uint64_t) start + (uint64_t) lenf;
	uint32_t len16 = utf16_length(s);
	uint32_t end = end64 > len16 ? len16 : (uint32_t) end64;
	return make_sub16(act->ctx, s, start, end);
}

static Avm2Value string_substring(Avm2Activation* act)
{
	const Avm2String* s = this_string(act);
	uint32_t len16 = utf16_length(s);
	uint32_t start = string_index(arg_f64_def(act, 0, 0.0), len16);
	uint32_t end = string_index(arg_f64_def(act, 1, (double) 0x7fffffff), len16);
	if (end < start)
	{
		uint32_t t = start;
		start = end;
		end = t;
	}
	return make_sub16(act->ctx, s, start, end);
}

// Case-map one BMP code point via the Flash-compatible tables
// (unicode_case_tables.h, shared with the AVM1 runtime).
static uint32_t case_map_cp(uint32_t cp, int to_lower)
{
	if (to_lower)
	{
		if (cp >= 'A' && cp <= 'Z') return cp + 32;
		if (cp < 0x80 || cp > 0xFFFF) return cp;
		uint32_t lo = 0;
		uint32_t hi = CASE_MAP_UPPER_TO_LOWER_COUNT;
		while (lo < hi)
		{
			uint32_t mid = (lo + hi) / 2;
			if (case_map_upper_to_lower[mid][0] < cp) lo = mid + 1;
			else hi = mid;
		}
		if (lo < CASE_MAP_UPPER_TO_LOWER_COUNT
		    && case_map_upper_to_lower[lo][0] == cp)
		{
			return case_map_upper_to_lower[lo][1];
		}
		return cp;
	}
	if (cp >= 'a' && cp <= 'z') return cp - 32;
	if (cp < 0x80 || cp > 0xFFFF) return cp;
	uint32_t lo = 0;
	uint32_t hi = CASE_MAP_LOWER_TO_UPPER_COUNT;
	while (lo < hi)
	{
		uint32_t mid = (lo + hi) / 2;
		if (case_map_lower_to_upper[mid][0] < cp) lo = mid + 1;
		else hi = mid;
	}
	if (lo < CASE_MAP_LOWER_TO_UPPER_COUNT
	    && case_map_lower_to_upper[lo][0] == cp)
	{
		return case_map_lower_to_upper[lo][1];
	}
	return cp;
}

static Avm2Value string_case_convert(Avm2Activation* act, int to_lower)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* s = this_string(act);
	char* out = avm2_alloc(ctx, s->len * 2 + 4);
	uint32_t n = 0;
	for (uint32_t i = 0; i < s->len; )
	{
		unsigned char c = (unsigned char) s->utf8[i];
		uint32_t cp;
		uint32_t clen;
		if (c < 0x80) { cp = c; clen = 1; }
		else if (c < 0xE0) { cp = c & 0x1F; clen = 2; }
		else if (c < 0xF0) { cp = c & 0x0F; clen = 3; }
		else { cp = c & 0x07; clen = 4; }
		for (uint32_t k = 1; k < clen && i + k < s->len; k++)
		{
			cp = (cp << 6) | ((unsigned char) s->utf8[i + k] & 0x3F);
		}
		cp = case_map_cp(cp, to_lower);
		if (cp < 0x80) out[n++] = (char) cp;
		else if (cp < 0x800)
		{
			out[n++] = (char) (0xC0 | (cp >> 6));
			out[n++] = (char) (0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000)
		{
			out[n++] = (char) (0xE0 | (cp >> 12));
			out[n++] = (char) (0x80 | ((cp >> 6) & 0x3F));
			out[n++] = (char) (0x80 | (cp & 0x3F));
		}
		else
		{
			out[n++] = (char) (0xF0 | (cp >> 18));
			out[n++] = (char) (0x80 | ((cp >> 12) & 0x3F));
			out[n++] = (char) (0x80 | ((cp >> 6) & 0x3F));
			out[n++] = (char) (0x80 | (cp & 0x3F));
		}
		i += clen;
	}
	{ Avm2Value r_ = make_str(ctx, out, n); heap_free(ctx->app, out); return r_; }  // copied: scratch
}

static Avm2Value string_to_lower_case(Avm2Activation* act)
{
	return string_case_convert(act, 1);
}

static Avm2Value string_to_upper_case(Avm2Activation* act)
{
	return string_case_convert(act, 0);
}

static Avm2Value string_to_string(Avm2Activation* act)
{
	return avm2_string(this_string(act));
}

// String.prototype.toString / .valueOf.
//
// Unlike the class method, the receiver here can be `String.prototype`
// ITSELF (`String.prototype.toString()` is a direct call in the ES3 tests).
// That is a bare Avm2Object with no primitive value, and coercing it would
// look up `toString` on it -- i.e. re-enter this very function -- and
// recurse until the stack dies. In avmplus `String.prototype` is a String
// object whose primitive value is "", and the tests assert exactly that
// (`String.prototype.toString()` -> ""), so a non-string receiver yields "".
// Any OTHER object receiver is an incompatible one -- avmplus reports #1004
// there (avm2/primitive_valueOf, ecma3/Exceptions/string_002_rt assign
// String.prototype.valueOf onto a plain Object and expect the call to throw).
static Avm2Value string_proto_receiver(Avm2Activation* act, const char* meth)
{
	if (act->this_val.kind == AVM2_VALUE_STRING) return act->this_val;
	Avm2Context* ctx = act->ctx;
	if (act->this_val.kind == AVM2_VALUE_OBJECT
	    && act->this_val.u.obj == ctx->builtins.string_class->prototype_obj)
	{
		return make_str(ctx, "", 0);
	}
	// AS3 playerglobal shim: FP's trace carries the throwError frame
	// (avm2/primitive_toString, primitive_valueOf). Per-site opt-in.
	avm2_callstack_push_throwerror(ctx);
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1004: Method String.prototype.%s was invoked on an "
	                 "incompatible object.", meth);
}

static Avm2Value string_proto_to_string(Avm2Activation* act)
{
	return string_proto_receiver(act, "toString");
}

static Avm2Value string_proto_value_of(Avm2Activation* act)
{
	return string_proto_receiver(act, "valueOf");
}

static Avm2Value string_construct(Avm2Context* ctx, Avm2Class* cls,
                                  const Avm2Value* args, uint32_t argc)
{
	(void) cls;
	if (argc == 0)
	{
		return avm2_string(avm2_string_from_literal(ctx, ""));
	}
	return avm2_string(avm2_coerce_to_string(ctx, args[0]));
}

void avm2_register_string(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;
	Avm2Class* cls = avm2_builtin_class(ctx, "", "String", b->object_class);
	b->string_class = cls;
	cls->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	cls->native_construct = string_construct;
	cls->native_call = string_construct;

	// `length` stays PUBLIC (avmplus has String::length in the public ns), but
	// every instance METHOD is AS3-keyed, which is avmplus's shape: only the
	// String.prototype copies below are public, so
	// `new String("JScript").hasOwnProperty("split")` is false
	// (from_avmplus ecma3/ObjectObjects/hasOwnProperty). Dispatch is
	// unaffected — avm2_propkey_matches folds AS3 onto public both ways.
	avm2_builtin_add_getter(ctx, cls, "length", string_get_length);
	avm2_builtin_add_method_as3(ctx, cls, "charAt", string_char_at);
	avm2_builtin_add_method_as3(ctx, cls, "charCodeAt", string_char_code_at);
	avm2_builtin_add_method_as3(ctx, cls, "concat", string_concat_method);
	avm2_builtin_add_method_as3(ctx, cls, "indexOf", string_index_of);
	avm2_builtin_add_method_as3(ctx, cls, "lastIndexOf", string_last_index_of);
	avm2_builtin_add_method_as3(ctx, cls, "localeCompare", string_locale_compare);
	avm2_builtin_add_method_as3(ctx, cls, "slice", string_slice);
	avm2_builtin_add_method_as3(ctx, cls, "split", avm2_string_split_plain);
	avm2_builtin_add_method_as3(ctx, cls, "substr", string_substr);
	avm2_builtin_add_method_as3(ctx, cls, "substring", string_substring);
	avm2_builtin_add_method_as3(ctx, cls, "toLowerCase", string_to_lower_case);
	avm2_builtin_add_method_as3(ctx, cls, "toLocaleLowerCase", string_to_lower_case);
	avm2_builtin_add_method_as3(ctx, cls, "toUpperCase", string_to_upper_case);
	avm2_builtin_add_method_as3(ctx, cls, "toLocaleUpperCase", string_to_upper_case);
	avm2_builtin_add_method_as3(ctx, cls, "toString", string_to_string);
	avm2_builtin_add_method_as3(ctx, cls, "valueOf", string_to_string);
	avm2_builtin_add_static_method(ctx, cls, "fromCharCode", string_from_char_code);

	// ES3-compat layer: the same methods also live on String.prototype as
	// plain functions (Ruffle globals/String.as). Tamarin's standard opening
	// assertion is `String.prototype.<m>.length`, and tests reassign them
	// onto other prototypes (`Number.prototype.split = String.prototype.split`),
	// so these must be real prototype properties. Every impl reads its
	// receiver through this_string(), which coerces, so a foreign `this`
	// works. match/replace/search are added in avm2_regexp.c alongside their
	// class methods.
	Avm2Object* proto = cls->prototype_obj;
	avm2_proto_add_function_n(ctx, proto, "charAt", string_char_at, 1);
	avm2_proto_add_function_n(ctx, proto, "charCodeAt", string_char_code_at, 1);
	avm2_proto_add_function_n(ctx, proto, "concat", string_concat_method, 0);
	avm2_proto_add_function_n(ctx, proto, "indexOf", string_index_of, 2);
	avm2_proto_add_function_n(ctx, proto, "lastIndexOf", string_last_index_of, 2);
	avm2_proto_add_function_n(ctx, proto, "localeCompare", string_locale_compare, 1);
	avm2_proto_add_function_n(ctx, proto, "slice", string_slice, 2);
	avm2_proto_add_function_n(ctx, proto, "split", avm2_string_split_plain, 2);
	avm2_proto_add_function_n(ctx, proto, "substr", string_substr, 2);
	avm2_proto_add_function_n(ctx, proto, "substring", string_substring, 2);
	avm2_proto_add_function_n(ctx, proto, "toLowerCase", string_to_lower_case, 0);
	avm2_proto_add_function_n(ctx, proto, "toLocaleLowerCase", string_to_lower_case, 0);
	avm2_proto_add_function_n(ctx, proto, "toUpperCase", string_to_upper_case, 0);
	avm2_proto_add_function_n(ctx, proto, "toLocaleUpperCase", string_to_upper_case, 0);
	avm2_proto_add_function_n(ctx, proto, "toString", string_proto_to_string, 0);
	avm2_proto_add_function_n(ctx, proto, "valueOf", string_proto_value_of, 0);
}
