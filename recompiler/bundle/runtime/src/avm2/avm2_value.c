// Avm2Value constructors + coercions + abstract operators (port of Ruffle
// core/src/avm2/value.rs; helper names 1:1 with Ruffle's methods).
// Number-to-string follows value.rs's coerce_to_string Number arm:
// shortest round-trip decimal inside [1e-6, 1e21), MAX_PRECISION (15)
// exponential outside it.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <avm2/avm2_value.h>
#include <avm2/avm2_e4x.h>
#include <avm2/avm2_gc.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_ops.h>

_Static_assert(sizeof(Avm2Value) == 16, "Avm2Value must be 16 bytes (plan §4.2)");

Avm2Value avm2_undefined(void)
{
	Avm2Value v = { AVM2_VALUE_UNDEFINED, 0, { 0 } };
	return v;
}

Avm2Value avm2_null(void)
{
	Avm2Value v = { AVM2_VALUE_NULL, 0, { 0 } };
	return v;
}

Avm2Value avm2_bool(bool b)
{
	Avm2Value v = { AVM2_VALUE_BOOL, 0, { 0 } };
	v.u.b = b;
	return v;
}

Avm2Value avm2_integer(int32_t i)
{
	Avm2Value v = { AVM2_VALUE_INTEGER, 0, { 0 } };
	v.u.i = i;
	return v;
}

Avm2Value avm2_number(double d)
{
	Avm2Value v = { AVM2_VALUE_NUMBER, 0, { 0 } };
	v.u.d = d;
	return v;
}

Avm2Value avm2_uint_value(uint32_t u)
{
	if (u <= 0x7fffffffu)
	{
		return avm2_integer((int32_t) u);
	}
	return avm2_number((double) u);
}

Avm2Value avm2_string(const Avm2String* s)
{
	Avm2Value v = { AVM2_VALUE_STRING, 0, { 0 } };
	v.u.str = s;
	return v;
}

Avm2Value avm2_object_value(Avm2Object* obj)
{
	Avm2Value v = { AVM2_VALUE_OBJECT, 0, { 0 } };
	v.u.obj = obj;
	return v;
}

const Avm2String* avm2_string_new(Avm2Context* ctx, const char* utf8, uint32_t len)
{
	Avm2String* s = avm2_alloc(ctx, sizeof(Avm2String) + len + 1);
	char* bytes = (char*) (s + 1);
	memcpy(bytes, utf8, len);
	bytes[len] = '\0';
	s->len = len;
	s->utf8 = bytes;
	avm2_gc_enroll_string(s);
	return s;
}

const Avm2String* avm2_string_from_literal(Avm2Context* ctx, const char* lit)
{
	return avm2_string_new(ctx, lit, (uint32_t) strlen(lit));
}

// --- canonical WTF-8 -------------------------------------------------------
//
// AVM2 strings are sequences of UTF-16 code units and an unpaired surrogate is
// a perfectly ordinary value (`String.fromCharCode(0xDC00)`), which strict
// UTF-8 cannot represent. Storage is therefore WTF-8: UTF-8 extended so that
// U+D800..U+DFFF may appear as the 3-byte sequence ED A0 80 .. ED BF BF (never
// valid UTF-8, so nothing else can collide with it).
//
// The form is CANONICAL: a high surrogate immediately followed by a low one is
// ALWAYS stored as the 4-byte UTF-8 of the combined astral code point, never as
// the 6-byte CESU-8 pair. That invariant is what keeps `avm2_string_equals`
// (a memcmp) equivalent to code-unit equality — `"\u{20B9F}"` and
// `fromCharCode(0xD842) + fromCharCode(0xDF9F)` must compare equal
// (ecma3/Unicode/utf8count).
//
// Concatenation is the only operation that can bring an unpaired high and an
// unpaired low together, so it re-normalizes the seam. (This is exactly the
// WTF-8 spec's concatenation rule.) Every other producer emits canonical form
// directly.

// s ends with an unpaired high surrogate (ED A0..AF xx)?
static bool wtf8_ends_with_high_surrogate(const Avm2String* s)
{
	if (s->len < 3) return false;
	const unsigned char* p = (const unsigned char*) s->utf8 + s->len - 3;
	return p[0] == 0xED && p[1] >= 0xA0 && p[1] <= 0xAF
	    && (p[2] & 0xC0) == 0x80;
}

// s starts with an unpaired low surrogate (ED B0..BF xx)?
static bool wtf8_starts_with_low_surrogate(const Avm2String* s)
{
	if (s->len < 3) return false;
	const unsigned char* p = (const unsigned char*) s->utf8;
	return p[0] == 0xED && p[1] >= 0xB0 && p[1] <= 0xBF
	    && (p[2] & 0xC0) == 0x80;
}

static uint32_t wtf8_decode3(const unsigned char* p)
{
	return ((uint32_t) (p[0] & 0x0F) << 12) | ((uint32_t) (p[1] & 0x3F) << 6)
	     | (uint32_t) (p[2] & 0x3F);
}

const Avm2String* avm2_string_concat(Avm2Context* ctx, const Avm2String* a, const Avm2String* b)
{
	// WTF-8 seam: an unpaired high surrogate at the end of `a` followed by an
	// unpaired low surrogate at the start of `b` re-forms one astral code
	// point, shrinking the result by two bytes (3 + 3 -> 4).
	bool join = wtf8_ends_with_high_surrogate(a) && wtf8_starts_with_low_surrogate(b);
	uint32_t total = a->len + b->len - (join ? 2u : 0u);
	Avm2String* s = avm2_alloc(ctx, sizeof(Avm2String) + total + 1);
	char* bytes = (char*) (s + 1);
	if (join)
	{
		uint32_t hi = wtf8_decode3((const unsigned char*) a->utf8 + a->len - 3);
		uint32_t lo = wtf8_decode3((const unsigned char*) b->utf8);
		uint32_t cp = 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
		memcpy(bytes, a->utf8, a->len - 3);
		char* w = bytes + (a->len - 3);
		w[0] = (char) (0xF0 | (cp >> 18));
		w[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
		w[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
		w[3] = (char) (0x80 | (cp & 0x3F));
		memcpy(w + 4, b->utf8 + 3, b->len - 3);
	}
	else
	{
		memcpy(bytes, a->utf8, a->len);
		memcpy(bytes + a->len, b->utf8, b->len);
	}
	bytes[total] = '\0';
	s->len = total;
	s->utf8 = bytes;
	avm2_gc_enroll_string(s);
	return s;
}

bool avm2_string_equals(const Avm2String* a, const Avm2String* b)
{
	if (a == b) return true;
	if (a == NULL || b == NULL) return false;
	return a->len == b->len && memcmp(a->utf8, b->utf8, a->len) == 0;
}

// ---------------------------------------------------------------------------
// ECMA f64 → int conversions (Ruffle ecma_conversions.rs)
// ---------------------------------------------------------------------------

uint32_t avm2_f64_to_wrapping_u32(double n)
{
	if (!isfinite(n)) return 0;
	double t = trunc(n);
	double r = fmod(t, 4294967296.0);
	if (r < 0.0) r += 4294967296.0;
	return (uint32_t) r;
}

int32_t avm2_f64_to_wrapping_i32(double n)
{
	return (int32_t) avm2_f64_to_wrapping_u32(n);
}

// ---------------------------------------------------------------------------
// String → number parsing (Ruffle value.rs string_to_int / string_to_f64)
// ---------------------------------------------------------------------------

static bool is_space_cp(unsigned char c)
{
	return c == 0x20 || c == 0x09 || c == 0x0d || c == 0x0a || c == 0x0c || c == 0x0b;
}

// Ruffle's leading-whitespace set is NOT ASCII-only: 0x2000..=0x200b, 0x2028,
// 0x2029, 0x205f and 0x3000 count too, and the corpus does reach them —
// `parseInt(" 1234 ")` is 1234, not NaN, in both avmplus and
// Ruffle. Returns the UTF-8 length of the space at `p`, or 0 if it is not one.
static uint32_t space_len_utf8(const char* p, const char* end)
{
	unsigned char b = (unsigned char) *p;
	if (b < 0x80) return is_space_cp(b) ? 1u : 0u;
	// Every non-ASCII space in the set is U+2000..U+3000, i.e. 3 UTF-8 bytes.
	if ((b & 0xF0) != 0xE0 || end - p < 3) return 0;
	uint32_t cp = ((uint32_t) (b & 0x0F) << 12)
	            | ((uint32_t) ((unsigned char) p[1] & 0x3F) << 6)
	            | ((uint32_t) ((unsigned char) p[2] & 0x3F));
	if ((cp >= 0x2000 && cp <= 0x200b) || cp == 0x2028 || cp == 0x2029
	    || cp == 0x205f || cp == 0x3000)
	{
		return 3u;
	}
	return 0u;
}

static void skip_spaces(const char** s, const char* end)
{
	for (;;)
	{
		if (*s >= end) return;
		uint32_t n = space_len_utf8(*s, end);
		if (n == 0) return;
		*s += n;
	}
}

static bool parse_sign(const char** s, const char* end)
{
	if (*s < end && **s == '-') { (*s)++; return true; }
	if (*s < end && **s == '+') { (*s)++; return false; }
	return false;
}

static int digit_value(char c, int radix)
{
	int d;
	if (c >= '0' && c <= '9') d = c - '0';
	else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
	else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
	else return -1;
	return d < radix ? d : -1;
}

double avm2_string_to_int(const char* s, uint32_t len, int32_t radix, bool strict)
{
	const char* p = s;
	const char* end = s + len;
	skip_spaces(&p, end);
	bool neg = parse_sign(&p, end);

	if (radix == 16 || radix == 0)
	{
		if (end - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		{
			p += 2;
			radix = 16;
		}
		else if (radix == 0)
		{
			radix = 10;
		}
	}
	if (radix < 2 || radix > 36 || p >= end) return NAN;

	// avmplus is exact for a POWER-OF-TWO radix and runs Ruffle's
	// `result = result * radix + digit` double chain for every other one, and
	// its .out files are these tests' oracle. All three graded values say so:
	// `parseInt("0x1000000000000081")` is 1152921504606847200, the double
	// nearest the true ...105, where the chain drifts to ...847000; the
	// 60-digit base-2 string likewise wants the exact ...724 over the chain's
	// ...720. But `parseInt("123456789012345678")` — radix 10, and comfortably
	// inside 64 bits — wants ...345700, which is the CHAIN's answer, not the
	// exact ...345680. So the split is the radix, not the width; a
	// power-of-two radix is just digit-shifting and stays exact for free.
	// (Ruffle ships an output.ruffle.txt for both parseInt tests because it
	// runs the chain throughout.)
	//
	// Past 2^64 the exact half stops contributing and `result` has carried the
	// same chain from the first digit, so long inputs are unaffected either way.
	double result = 0.0;
	uint64_t exact = 0;
	bool exact_ok = (radix & (radix - 1)) == 0;
	const char* start = p;
	while (p < end)
	{
		int d = digit_value(*p, radix);
		if (d < 0) break;
		result = result * (double) radix + (double) d;
		if (exact_ok)
		{
			uint64_t r = (uint64_t) radix;
			if (exact > (UINT64_MAX - (uint64_t) d) / r) exact_ok = false;
			else exact = exact * r + (uint64_t) d;
		}
		p++;
	}
	if (p == start) return NAN;
	if (exact_ok) result = (double) exact;

	if (strict)
	{
		skip_spaces(&p, end);
		if (p < end) return NAN;
	}
	return neg ? -result : result;
}


// LLVM __powidf2 (f64::powi): binary exponentiation, NOT correctly
// rounded — Ruffle's string_to_f64 uses powi, and the 1-ulp differences
// are observable (coerce_string_precision).
static double f64_powi(double a, int32_t b)
{
	const int recip = b < 0;
	double r = 1.0;
	uint32_t ub = (uint32_t) (recip ? -(int64_t) b : b);
	for (;;)
	{
		if (ub & 1u) r *= a;
		ub >>= 1;
		if (ub == 0) break;
		a *= a;
	}
	return recip ? 1.0 / r : r;
}

bool avm2_string_to_f64(Avm2Context* ctx, const char* s, uint32_t len, bool strict,
                        double* out_d)
{
	const char* p = s;
	const char* end = s + len;
	skip_spaces(&p, end);

	if (p >= end)
	{
		if (strict) { *out_d = 0.0; return true; }
		return false;
	}

	bool neg = parse_sign(&p, end);
	const char* after_sign = p;

	// Digits before the decimal point.
	while (p < end && *p >= '0' && *p <= '9') p++;
	int total_digits = (int) (p - after_sign);

	// Digits after the decimal point.
	if (p < end && *p == '.')
	{
		p++;
		const char* frac_start = p;
		while (p < end && *p >= '0' && *p <= '9') p++;
		total_digits += (int) (p - frac_start);
	}

	// Exponent.
	int32_t exponent = 0;
	if (p < end && (*p == 'e' || *p == 'E'))
	{
		p++;
		bool exp_neg = parse_sign(&p, end);
		if (exp_neg && p >= end) return false;
		while (p < end && *p >= '0' && *p <= '9')
		{
			exponent = exponent * 10 + (*p - '0');
			p++;
		}
		if (exp_neg) exponent = -exponent;
	}

	skip_spaces(&p, end);

	if (total_digits == 0)
	{
		// Infinity / -Infinity.
		if (end - p >= 8 && memcmp(p, "Infinity", 8) == 0)
		{
			const char* q = p + 8;
			if (q < end)
			{
				const char* q2 = q;
				skip_spaces(&q2, end);
				if (q2 == q) return false;  // trailing garbage, no space
			}
			*out_d = neg ? -INFINITY : INFINITY;
			return true;
		}
		return false;
	}

	if (strict && p < end && *p != '\0') return false;

	// Bug compatibility (Ruffle value.rs string_to_f64, bugzilla 513018): SWF
	// >= 11 accumulates over the SCANNED prefix only, but SWF <= 10 re-walks
	// the WHOLE remainder, so a SECOND '.' RESETS the fractional-digit count
	// instead of ending the number — parseFloat("1.2345.678") is 12345.678 and
	// parseFloat("1.2345.6e50") is 12345.6 (avm2/parse_float_swf10). Both
	// accumulator arms below break on the first byte that is neither a digit
	// nor '.', exactly like Ruffle's two loops, so this one line covers both.
	// Inert on the `strict` path: strict already rejected trailing garbage
	// above, so p == end there.
	const char* span_end = (ctx == NULL || ctx->swf_version >= 11) ? p : end;

	double result = 0.0;
	int decimal_digits = -1;
	if (total_digits > 15)
	{
		// avmplus builds the digit string as an exact integer (BigInt in
		// Ruffle) and converts once; strtod of the digits WITHOUT the dot
		// reproduces that single rounding, then the decimal shift below
		// applies in float exactly like Ruffle.
		char tmp[400];
		const char* q = after_sign;
		int ti = 0;
		while (q < span_end
		       && ((*q >= '0' && *q <= '9') || *q == '.'))
		{
			if (*q == '.')
			{
				decimal_digits = 0;
			}
			else
			{
				if (decimal_digits >= 0) decimal_digits++;
				if (ti < 380) tmp[ti++] = *q;
			}
			q++;
		}
		tmp[ti] = '\0';
		result = strtod(tmp, NULL);
	}
	else
	{
		const char* q = after_sign;
		while (q < span_end)
		{
			if (*q >= '0' && *q <= '9')
			{
				if (decimal_digits != -1) decimal_digits++;
				result = result * 10.0 + (double) (*q - '0');
			}
			else if (*q == '.')
			{
				decimal_digits = 0;
			}
			else
			{
				break;
			}
			q++;
		}
	}

	if (decimal_digits > 0)
	{
		exponent -= decimal_digits;
	}
	if (exponent > 0)
	{
		result *= f64_powi(10.0, exponent);
	}
	if (exponent < 0)
	{
		if (exponent < -307)
		{
			int diff = exponent + 307;
			result /= f64_powi(10.0, -diff);
			exponent = -307;
		}
		result /= f64_powi(10.0, -exponent);
	}

	*out_d = neg ? -result : result;
	return true;
}

// ---------------------------------------------------------------------------
// Number → string (Ruffle value.rs coerce_to_string Number arm)
// ---------------------------------------------------------------------------

// Exact-midpoint tie-break, Rust-style. glibc's printf is correctly rounded
// and resolves an exact decimal tie TO-EVEN; Rust's f64 Display (which Ruffle
// prints through) resolves it AWAY FROM ZERO. They disagree only when the
// double's exact decimal expansion terminates exactly one digit past the
// shortest round-tripping precision, with that digit a '5'
// (e.g. 966.67828369140625 -> glibc "966.6782836914062", Rust
// "966.6782836914063"). Rewrites digs[0..prec] / *exp10 in place and returns
// 1 when it re-rounds. `d` is > 0 and finite; only called for prec >= 13, so
// the big exact-expansion buffer never lands on the hot path.
#define AVM2_EXACT_PREC 767
static int tie_round_away_from_zero(double d, int prec, char* digs, int* exp10)
{
	// Stage 0 (arithmetic filter, no formatting): a tie needs d's EXACT decimal
	// expansion to terminate at prec+2 significant digits. Writing d = m * 2^p
	// with m an odd integer, that expansion is m*5^-p (p < 0) or m*2^p (p >= 0),
	// so its digit count is computable with one log10. Almost every double is
	// far too long and exits here, which keeps the two snprintf stages below off
	// the hot path. One digit of slack absorbs any log10 rounding; stage 2 is
	// still the authority, so a false accept costs nothing but time.
	{
		int e2;
		double fr = frexp(d, &e2);
		uint64_t m = (uint64_t) ldexp(fr, 53);
		int p = e2 - 53;
		while (m != 0 && (m & 1u) == 0) { m >>= 1; p++; }
		double lg = log10((double) m)
		            + (p < 0 ? (-p) * 0.69897000433601880   // log10(5)
		                     :   p  * 0.30102999566398120); // log10(2)
		if ((int) floor(lg) + 1 > prec + 3) return 0;
	}

	// Stage 1 (cheap filter): print one extra significant digit. A true tie
	// always shows a '5' there; the overwhelming majority of values do not.
	char probe[64];
	snprintf(probe, sizeof(probe), "%.*e", prec + 1, d);
	int seen = 0;
	char last = 0;
	for (const char* p = probe; *p != '\0' && *p != 'e' && *p != 'E'; p++)
	{
		if (*p >= '0' && *p <= '9') { last = *p; seen++; }
	}
	if (seen != prec + 2 || last != '5') return 0;

	// Stage 2: glibc prints a double's EXACT decimal expansion at any
	// precision, zero-padded. 768 significant digits covers every finite f64
	// (the longest, 2^-1074, needs 751), so this is exact, not rounded.
	char ex[AVM2_EXACT_PREC + 32];
	snprintf(ex, sizeof(ex), "%.*e", AVM2_EXACT_PREC, d);
	char dig[AVM2_EXACT_PREC + 4];
	int m = 0;
	for (const char* q = ex; *q != '\0'; q++)
	{
		if (*q >= '0' && *q <= '9') dig[m++] = *q;
		else if (*q == 'e' || *q == 'E') break;
	}
	if (m < prec + 2 || dig[prec + 1] != '5') return 0;
	for (int i = prec + 2; i < m; i++) if (dig[i] != '0') return 0;

	// Exact midpoint. Away from zero, and d > 0, so round up.
	char up[24];
	for (int i = 0; i <= prec; i++) up[i] = dig[i];
	int carry = 1;
	for (int i = prec; i >= 0 && carry != 0; i--)
	{
		if (up[i] == '9') up[i] = '0';
		else { up[i]++; carry = 0; }
	}
	int e = *exp10;
	if (carry != 0)
	{
		// 99..9 -> 100..0: shift and bump the exponent.
		for (int i = prec; i > 0; i--) up[i] = up[i - 1];
		up[0] = '1';
		e++;
	}

	// Safety net: Rust only ever emits a representation that round-trips, so
	// if the away-from-zero neighbour does not, keep glibc's answer.
	char chk[48];
	int cn = 0;
	chk[cn++] = up[0];
	if (prec > 0)
	{
		chk[cn++] = '.';
		for (int i = 1; i <= prec; i++) chk[cn++] = up[i];
	}
	snprintf(chk + cn, sizeof(chk) - (size_t) cn, "e%d", e);
	if (strtod(chk, NULL) != d) return 0;

	for (int i = 0; i <= prec; i++) digs[i] = up[i];
	*exp10 = e;
	return 1;
}
#undef AVM2_EXACT_PREC

// Shortest decimal digit string that round-trips `d` (> 0, finite).
// Writes the significant digits (no dot) to digs (cap >= 20) and the
// power-of-ten of the first digit to *exp10. Mirrors Rust's f64 Display.
static int shortest_digits(double d, char* digs, int* exp10)
{
	char tmp[48];
	for (int prec = 0; prec <= 17; prec++)
	{
		snprintf(tmp, sizeof(tmp), "%.*e", prec, d);
		if (strtod(tmp, NULL) == d)
		{
			int n = 0;
			const char* q = tmp;
			int e = 0;
			for (; *q != '\0'; q++)
			{
				if (*q >= '0' && *q <= '9')
				{
					digs[n++] = *q;
				}
				else if (*q == 'e' || *q == 'E')
				{
					e = atoi(q + 1);
					break;
				}
			}
			// glibc broke an exact decimal tie to-even; Rust (and so Ruffle)
			// breaks it away from zero. Re-round when they disagree.
			if (prec >= 13 && n == prec + 1)
			{
				tie_round_away_from_zero(d, prec, digs, &e);
			}
			// Trim trailing zeros (keep at least one digit).
			while (n > 1 && digs[n - 1] == '0') n--;
			digs[n] = '\0';
			*exp10 = e;
			return n;
		}
	}
	// Unreachable: 17 significant digits always round-trip.
	digs[0] = '0';
	digs[1] = '\0';
	*exp10 = 0;
	return 1;
}

// Plain (exponent-free) decimal rendering of a positive double, Rust
// Display style. Takes the shortest round-tripping digits from the caller
// (which needs them anyway to pick the notation), so the expensive
// shortest_digits probe loop runs once per format, not twice.
static int format_plain(char* buf, int buf_size, const char* digs, int n, int e)
{
	int pos = 0;

#define PUTC(c) do { if (pos < buf_size - 1) buf[pos] = (c); pos++; } while (0)
	if (e >= 0)
	{
		if (e + 1 >= n)
		{
			for (int i = 0; i < n; i++) PUTC(digs[i]);
			for (int i = 0; i < e + 1 - n; i++) PUTC('0');
		}
		else
		{
			for (int i = 0; i <= e; i++) PUTC(digs[i]);
			PUTC('.');
			for (int i = e + 1; i < n; i++) PUTC(digs[i]);
		}
	}
	else
	{
		PUTC('0');
		PUTC('.');
		for (int i = 0; i < -e - 1; i++) PUTC('0');
		for (int i = 0; i < n; i++) PUTC(digs[i]);
	}
#undef PUTC
	int written = pos < buf_size - 1 ? pos : buf_size - 1;
	buf[written] = '\0';
	return written;
}

int avm2_format_number(char* buf, int buf_size, double d)
{
	if (isnan(d)) return snprintf(buf, buf_size, "NaN");
	if (d == 0.0) return snprintf(buf, buf_size, "0");
	if (d < 0.0)
	{
		int n = snprintf(buf, buf_size, "-");
		return n + avm2_format_number(buf + 1, buf_size - 1, -d);
	}
	if (isinf(d)) return snprintf(buf, buf_size, "Infinity");

	// Fast path: exact positive integers below 1e15 render as a plain decimal
	// (the shortest repr of such a value has n <= 15 <= 21, so the fixed branch
	// below would apply anyway), but
	// skip shortest_digits' up-to-18 snprintf("%.*e")/strtod round-trip probes —
	// number formatting is ~10% of Seedling's frame self-time and dominated by
	// integer coordinates/counters. Integers < 1e15 are exact in f64 (< 2^53),
	// and this is byte-identical to format_plain (verified over 0..2e6 + edges).
	if (d < 1e15 && d == floor(d))
	{
		uint64_t u = (uint64_t) d;
		char tmp[20];
		int n = 0;
		do { tmp[n++] = (char) ('0' + (u % 10)); u /= 10; } while (u != 0);
		int w = 0;
		while (n > 0 && w < buf_size - 1) buf[w++] = tmp[--n];
		buf[w] = '\0';
		return w;
	}

	// ECMA-262 7.1.12.1 (Number::toString) picks the notation from the decimal
	// exponent `n` of the SHORTEST round-tripping representation -- the value is
	// digits x 10^(n-k) -- and uses fixed notation iff -6 < n <= 21.
	// `floor(log10(d))` is NOT that quantity and disagrees on both boundaries:
	//   * log10(9.999999999999997e-7) rounds to exactly -6.0 in f64, so the old
	//     test took the fixed branch where ECMA (and Flash, and Ruffle) take
	//     exponential -- 10 lines of avm2/number_to_string;
	//   * 9.999999999999999e20 has n = 21, i.e. fixed ("999999999999999900000"),
	//     while floor(log10) reports 21, which the old `>= 21.0` test sent to
	//     exponential -- 2 more lines of the same test.
	// The whole behavioural delta is 36 doubles: the 5 representable values
	// immediately below 1e-6 and the 31 immediately below 1e21 (enumerated by
	// ULP walk over both boundaries; a 600k-value random sweep finds no others).
	// shortest_digits' exp10 is the power of ten of the FIRST digit, i.e. n-1,
	// so the test is e10 < -6 || e10 > 20.
	char digs[24];
	int e10 = 0;
	int nd = shortest_digits(d, digs, &e10);

	if (e10 < -6 || e10 > 20)
	{
		// Exponential form, Ruffle value.rs::f64_to_string_finite_nonzero:
		// take the SHORTEST round-tripping decimal and, for a POSITIVE
		// exponent only, cut the mantissa TEXT to 16 characters ("Flash shows
		// large numbers with smaller precision for some reason"). A negative
		// exponent keeps every digit.
		//
		// The arithmetic 15-significant-digit truncation this replaced was
		// the OLD Ruffle rule and it does not survive a round trip: dividing
		// the truncated value by 10^digits reintroduces error, so 1e40 came
		// out as the malformed "0.9999999999999999e+40" and 3e50 as
		// "2.9999999999999996e+50" (avm2/matrix3d_precision grades both).
		char mant[40];
		int mp = 0;
		mant[mp++] = digs[0];
		if (nd > 1)
		{
			mant[mp++] = '.';
			for (int i = 1; i < nd && mp < (int) sizeof(mant) - 1; i++)
				mant[mp++] = digs[i];
		}
		mant[mp] = '\0';
		if (e10 > 0)
		{
			// Two doubles print as "1e+308" in Flash while their immediate
			// neighbours print normally (Ruffle carries the same pair).
			uint64_t bits;
			memcpy(&bits, &d, sizeof(bits));
			if (bits == 0x7fe0000000000000ull || bits == 0x7fd0000000000000ull)
				return snprintf(buf, buf_size, "1e+308");
			if (mp > 16) { mant[16] = '\0'; mp = 16; }
		}
		return snprintf(buf, buf_size, "%se%c%d", mant,
		                e10 < 0 ? '-' : '+', e10 < 0 ? -e10 : e10);
	}

	return format_plain(buf, buf_size, digs, nd, e10);
}

// ---------------------------------------------------------------------------
// Coercions
// ---------------------------------------------------------------------------

bool avm2_coerce_to_boolean(Avm2Value v)
{
	switch (v.kind)
	{
		case AVM2_VALUE_UNDEFINED:
		case AVM2_VALUE_NULL:
			return false;
		case AVM2_VALUE_BOOL:
			return v.u.b;
		case AVM2_VALUE_INTEGER:
			return v.u.i != 0;
		case AVM2_VALUE_NUMBER:
			return !isnan(v.u.d) && v.u.d != 0.0;
		case AVM2_VALUE_STRING:
			return v.u.str != NULL && v.u.str->len > 0;
		case AVM2_VALUE_OBJECT:
			return true;
	}
	return false;
}

Avm2Value avm2_coerce_to_primitive(Avm2Context* ctx, Avm2Value v, int hint)
{
	if (v.kind != AVM2_VALUE_OBJECT)
	{
		return v;
	}
	if (hint == 0)
	{
		// ES3 8.6.2.6 [[DefaultValue]] with no hint: Number for every object
		// except Date, whose [[DefaultValue]] uses the String hint (15.9.6),
		// so `date + x` concatenates rather than adding milliseconds.
		// ecma3/Expressions/e11_6_1_3 pins exactly that. Only the `add`
		// operator passes hint 0, so the arm is confined to `+`.
		hint = 1;
		for (Avm2Class* c = (v.u.obj != NULL) ? v.u.obj->cls : NULL;
		     c != NULL; c = c->super_class)
		{
			if (c == ctx->builtins.date_class)
			{
				hint = 2;
				break;
			}
		}
	}
	const char* first = (hint == 2) ? "toString" : "valueOf";
	const char* second = (hint == 2) ? "valueOf" : "toString";

	Avm2Value prim = avm2_call_public_property(ctx, v, first, (uint32_t) strlen(first), NULL, 0);
	if (prim.kind != AVM2_VALUE_OBJECT) return prim;
	prim = avm2_call_public_property(ctx, v, second, (uint32_t) strlen(second), NULL, 0);
	if (prim.kind != AVM2_VALUE_OBJECT) return prim;

	Avm2Class* cls = avm2_value_class(ctx, v);
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1050: Cannot convert %.*s to primitive.",
	                 (int) cls->name.name_len, cls->name.name);
}

double avm2_coerce_to_number(Avm2Context* ctx, Avm2Value v)
{
	switch (v.kind)
	{
		case AVM2_VALUE_NUMBER:
			return v.u.d;
		case AVM2_VALUE_INTEGER:
			return (double) v.u.i;
		case AVM2_VALUE_UNDEFINED:
			return NAN;
		case AVM2_VALUE_NULL:
			return 0.0;
		case AVM2_VALUE_BOOL:
			return v.u.b ? 1.0 : 0.0;
		case AVM2_VALUE_STRING:
		{
			double d;
			if (avm2_string_to_f64(ctx, v.u.str->utf8, v.u.str->len, true, &d))
			{
				return d;
			}
			return avm2_string_to_int(v.u.str->utf8, v.u.str->len, 0, true);
		}
		case AVM2_VALUE_OBJECT:
			return avm2_coerce_to_number(ctx, avm2_coerce_to_primitive(ctx, v, 1));
	}
	return NAN;
}

uint32_t avm2_coerce_to_u32(Avm2Context* ctx, Avm2Value v)
{
	switch (v.kind)
	{
		case AVM2_VALUE_NUMBER:
			return avm2_f64_to_wrapping_u32(v.u.d);
		case AVM2_VALUE_INTEGER:
			return (uint32_t) v.u.i;
		case AVM2_VALUE_BOOL:
			return v.u.b ? 1u : 0u;
		case AVM2_VALUE_UNDEFINED:
		case AVM2_VALUE_NULL:
			return 0;
		default:
			return avm2_f64_to_wrapping_u32(avm2_coerce_to_number(ctx, v));
	}
}

int32_t avm2_coerce_to_i32(Avm2Context* ctx, Avm2Value v)
{
	switch (v.kind)
	{
		case AVM2_VALUE_NUMBER:
			return avm2_f64_to_wrapping_i32(v.u.d);
		case AVM2_VALUE_INTEGER:
			return v.u.i;
		case AVM2_VALUE_BOOL:
			return v.u.b ? 1 : 0;
		case AVM2_VALUE_UNDEFINED:
		case AVM2_VALUE_NULL:
			return 0;
		default:
			return avm2_f64_to_wrapping_i32(avm2_coerce_to_number(ctx, v));
	}
}

// Signed 32-bit int -> decimal, no printf. Byte-identical to snprintf("%d"),
// including INT32_MIN (magnitude taken in uint32 to avoid -INT32_MIN UB).
// Integer stringification (counters, coordinates, String(int), concat) is a
// hot path; snprintf's printf_core dominated ~10% of Seedling's frame.
static int write_i32_decimal(char* buf, int32_t v)
{
	uint32_t mag = (v < 0) ? (uint32_t) -(int64_t) v : (uint32_t) v;
	char tmp[10];
	int n = 0;
	do { tmp[n++] = (char) ('0' + (mag % 10)); mag /= 10; } while (mag != 0);
	int w = 0;
	if (v < 0) buf[w++] = '-';
	while (n > 0) buf[w++] = tmp[--n];
	buf[w] = '\0';
	return w;
}

const Avm2String* avm2_coerce_to_string(Avm2Context* ctx, Avm2Value v)
{
	char buf[420];
	switch (v.kind)
	{
		case AVM2_VALUE_UNDEFINED:
			return avm2_string_from_literal(ctx, "undefined");
		case AVM2_VALUE_NULL:
			return avm2_string_from_literal(ctx, "null");
		case AVM2_VALUE_BOOL:
			return avm2_string_from_literal(ctx, v.u.b ? "true" : "false");
		case AVM2_VALUE_INTEGER:
			write_i32_decimal(buf, v.u.i);
			return avm2_string_from_literal(ctx, buf);
		case AVM2_VALUE_NUMBER:
			avm2_format_number(buf, sizeof(buf), v.u.d);
			return avm2_string_from_literal(ctx, buf);
		case AVM2_VALUE_STRING:
			return v.u.str;
		case AVM2_VALUE_OBJECT:
			return avm2_coerce_to_string(ctx, avm2_coerce_to_primitive(ctx, v, 2));
	}
	return avm2_string_from_literal(ctx, "undefined");
}

// ---------------------------------------------------------------------------
// Numeric predicates
// ---------------------------------------------------------------------------

bool avm2_value_is_number(Avm2Value v)
{
	return v.kind == AVM2_VALUE_NUMBER || v.kind == AVM2_VALUE_INTEGER;
}

bool avm2_value_is_u32(Avm2Value v)
{
	if (v.kind == AVM2_VALUE_INTEGER) return v.u.i >= 0;
	if (v.kind == AVM2_VALUE_NUMBER)
	{
		return v.u.d == (double) (uint32_t) v.u.d;
	}
	return false;
}

bool avm2_value_is_i32(Avm2Value v)
{
	if (v.kind == AVM2_VALUE_INTEGER) return true;
	if (v.kind == AVM2_VALUE_NUMBER)
	{
		return v.u.d == (double) (int32_t) v.u.d;
	}
	return false;
}

bool avm2_value_as_index(Avm2Value v, uint32_t* out_idx)
{
	if (v.kind == AVM2_VALUE_INTEGER && v.u.i >= 0)
	{
		*out_idx = (uint32_t) v.u.i;
		return true;
	}
	if (v.kind == AVM2_VALUE_NUMBER && avm2_value_is_u32(v)
	    && v.u.d < 4294967295.0)
	{
		*out_idx = (uint32_t) v.u.d;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Abstract operators (value.rs / activation.rs)
// ---------------------------------------------------------------------------

Avm2Value avm2_op_add_values(Avm2Context* ctx, Avm2Value a, Avm2Value b)
{
	if (a.kind == AVM2_VALUE_INTEGER && b.kind == AVM2_VALUE_INTEGER)
	{
		int64_t sum = (int64_t) a.u.i + (int64_t) b.u.i;
		if (sum >= INT32_MIN && sum <= INT32_MAX)
		{
			return avm2_integer((int32_t) sum);
		}
		return avm2_number((double) sum);
	}
	if (a.kind == AVM2_VALUE_NUMBER && b.kind == AVM2_VALUE_NUMBER)
	{
		return avm2_number(a.u.d + b.u.d);
	}
	if (avm2_value_is_xmlish(a) && avm2_value_is_xmlish(b))
	{
		// E4X: XML/XMLList + XML/XMLList concatenates into a new XMLList
		// (Ruffle activation.rs op_add; xml_list_concat).
		Avm2Object* out = avm2_xmllist_new(ctx, NULL, NULL);
		Avm2XmlListExt* oe = (Avm2XmlListExt*) out->native_ext;
		Avm2Value halves[2] = { a, b };
		for (int h = 0; h < 2; h++)
		{
			Avm2XmlListExt* le = avm2_xmllist_ext_of(halves[h]);
			if (le != NULL)
			{
				// append(list): copy target linkage, clear dirty.
				oe->target_dirty = 0;
				oe->target_object = le->target_object;
				oe->has_target_prop = le->has_target_prop;
				oe->target_prop = le->target_prop;
				for (uint32_t i = 0; i < le->count; i++)
				{
					avm2_xmllist_push(ctx, oe, le->items[i]);
				}
			}
			else
			{
				Avm2XmlExt* xe = avm2_xml_ext_of(halves[h]);
				oe->target_dirty = 1;
				avm2_xmllist_push(ctx, oe, xe->node);
			}
		}
		return avm2_object_value(out);
	}
	if (a.kind == AVM2_VALUE_STRING)
	{
		return avm2_string(avm2_string_concat(ctx, a.u.str, avm2_coerce_to_string(ctx, b)));
	}
	if (b.kind == AVM2_VALUE_STRING)
	{
		return avm2_string(avm2_string_concat(ctx, avm2_coerce_to_string(ctx, a), b.u.str));
	}

	Avm2Value pa = avm2_coerce_to_primitive(ctx, a, 0);
	Avm2Value pb = avm2_coerce_to_primitive(ctx, b, 0);
	if (pa.kind == AVM2_VALUE_STRING)
	{
		return avm2_string(avm2_string_concat(ctx, pa.u.str, avm2_coerce_to_string(ctx, pb)));
	}
	if (pb.kind == AVM2_VALUE_STRING)
	{
		return avm2_string(avm2_string_concat(ctx, avm2_coerce_to_string(ctx, pa), pb.u.str));
	}
	return avm2_number(avm2_coerce_to_number(ctx, pa) + avm2_coerce_to_number(ctx, pb));
}

bool avm2_strict_eq(Avm2Value a, Avm2Value b)
{
	switch (a.kind)
	{
		case AVM2_VALUE_UNDEFINED:
			return b.kind == AVM2_VALUE_UNDEFINED;
		case AVM2_VALUE_NULL:
			return b.kind == AVM2_VALUE_NULL;
		case AVM2_VALUE_BOOL:
			return b.kind == AVM2_VALUE_BOOL && a.u.b == b.u.b;
		case AVM2_VALUE_INTEGER:
			if (b.kind == AVM2_VALUE_INTEGER) return a.u.i == b.u.i;
			if (b.kind == AVM2_VALUE_NUMBER) return (double) a.u.i == b.u.d;
			return false;
		case AVM2_VALUE_NUMBER:
			if (b.kind == AVM2_VALUE_INTEGER) return a.u.d == (double) b.u.i;
			if (b.kind == AVM2_VALUE_NUMBER) return a.u.d == b.u.d;
			return false;
		case AVM2_VALUE_STRING:
			return b.kind == AVM2_VALUE_STRING && avm2_string_equals(a.u.str, b.u.str);
		case AVM2_VALUE_OBJECT:
			return b.kind == AVM2_VALUE_OBJECT && a.u.obj == b.u.obj;
	}
	return false;
}

bool avm2_abstract_eq(Avm2Context* ctx, Avm2Value a, Avm2Value b)
{
	// E4X arms come first (Ruffle value.rs: XML/XMLList/QName before the
	// ECMA algorithm — an empty XMLList == undefined).
	//
	// The object-kind gate below is EXACT, not heuristic: every probe in this
	// block (avm2_xml_ext_of / avm2_xmllist_ext_of / avm2_qname_ext_of) tests
	// `v.kind == AVM2_VALUE_OBJECT` first and returns NULL otherwise, so
	//   - avm2_xml_abstract_eq(ctx, a, b, ..) can only return nonzero if `a`
	//     is an OBJECT (it dispatches solely on a's XML/XMLList ext),
	//   - the mirrored call likewise needs `b` to be an OBJECT,
	//   - the QName arm needs BOTH to be OBJECTs.
	// So when neither operand is an object the whole block is a proven no-op.
	// The asymmetric cases the Ruffle ordering comment protects (empty XMLList
	// == undefined, XML == "string") keep one OBJECT operand and still enter.
	// Motivation: this block was ~2.6% of all instructions in a game with zero
	// E4X — five ext probes (each an avm2_get_context() + class compare) on
	// every numeric/string equality.
	if (a.kind == AVM2_VALUE_OBJECT || b.kind == AVM2_VALUE_OBJECT)
	{
		int eq;
		if (avm2_xml_abstract_eq(ctx, a, b, &eq)) return eq != 0;
		if (avm2_xml_abstract_eq(ctx, b, a, &eq)) return eq != 0;
		Avm2QNameExt* qa = avm2_qname_ext_of(a);
		Avm2QNameExt* qb = avm2_qname_ext_of(b);
		if (qa != NULL && qb != NULL)
		{
			int uri_eq = (qa->uri == NULL && qb->uri == NULL)
			             || (qa->uri != NULL && qb->uri != NULL
			                 && avm2_string_equals(qa->uri, qb->uri));
			int local_eq = (qa->local == NULL && qb->local == NULL)
			               || (qa->local != NULL && qb->local != NULL
			                   && avm2_string_equals(qa->local, qb->local));
			return uri_eq && local_eq;
		}
	}

	// ECMA-262 3rd ed. 11.9.3 (value.rs abstract_eq).
	bool a_num = avm2_value_is_number(a);
	bool b_num = avm2_value_is_number(b);

	if (a.kind == AVM2_VALUE_UNDEFINED || a.kind == AVM2_VALUE_NULL)
	{
		return b.kind == AVM2_VALUE_UNDEFINED || b.kind == AVM2_VALUE_NULL;
	}
	if (b.kind == AVM2_VALUE_UNDEFINED || b.kind == AVM2_VALUE_NULL)
	{
		return false;
	}
	if (a_num && b_num)
	{
		return avm2_coerce_to_number(ctx, a) == avm2_coerce_to_number(ctx, b);
	}
	if (a.kind == AVM2_VALUE_STRING && b.kind == AVM2_VALUE_STRING)
	{
		return avm2_string_equals(a.u.str, b.u.str);
	}
	if (a.kind == AVM2_VALUE_BOOL && b.kind == AVM2_VALUE_BOOL)
	{
		return a.u.b == b.u.b;
	}
	if (a.kind == AVM2_VALUE_OBJECT && b.kind == AVM2_VALUE_OBJECT)
	{
		// Two Namespaces compare by URI (Ruffle value.rs abstract_eq).
		Avm2NamespaceExt* na = avm2_namespace_ext_of(a);
		Avm2NamespaceExt* nb = avm2_namespace_ext_of(b);
		if (na != NULL && nb != NULL)
		{
			return avm2_string_equals(na->uri, nb->uri);
		}
		return a.u.obj == b.u.obj;
	}
	if (a_num && b.kind == AVM2_VALUE_STRING)
	{
		return avm2_abstract_eq(ctx, a, avm2_number(avm2_coerce_to_number(ctx, b)));
	}
	if (a.kind == AVM2_VALUE_STRING && b_num)
	{
		return avm2_abstract_eq(ctx, avm2_number(avm2_coerce_to_number(ctx, a)), b);
	}
	if (a.kind == AVM2_VALUE_BOOL)
	{
		return avm2_abstract_eq(ctx, avm2_number(avm2_coerce_to_number(ctx, a)), b);
	}
	if (b.kind == AVM2_VALUE_BOOL)
	{
		return avm2_abstract_eq(ctx, a, avm2_number(avm2_coerce_to_number(ctx, b)));
	}
	if ((a.kind == AVM2_VALUE_STRING || a_num) && b.kind == AVM2_VALUE_OBJECT)
	{
		return avm2_abstract_eq(ctx, a, avm2_coerce_to_primitive(ctx, b, 1));
	}
	if (a.kind == AVM2_VALUE_OBJECT && (b.kind == AVM2_VALUE_STRING || b_num))
	{
		return avm2_abstract_eq(ctx, avm2_coerce_to_primitive(ctx, a, 1), b);
	}
	return false;
}


// Lexicographic comparison in UTF-16 code-unit order (WStr order): astral
// codepoints order by their HIGH surrogate, which sorts below U+E000+.
static uint32_t next_unit_key(const Avm2String* s, uint32_t* i)
{
	unsigned char c = (unsigned char) s->utf8[*i];
	uint32_t cp;
	uint32_t clen;
	if (c < 0x80) { cp = c; clen = 1; }
	else if (c < 0xE0) { cp = c & 0x1F; clen = 2; }
	else if (c < 0xF0) { cp = c & 0x0F; clen = 3; }
	else { cp = c & 0x07; clen = 4; }
	for (uint32_t k = 1; k < clen && *i + k < s->len; k++)
	{
		cp = (cp << 6) | ((unsigned char) s->utf8[*i + k] & 0x3F);
	}
	*i += clen;
	if (cp >= 0x10000)
	{
		return 0xD800 + ((cp - 0x10000) >> 10);
	}
	return cp;
}

int avm2_string_utf16_cmp(const Avm2String* a, const Avm2String* b)
{
	uint32_t ia = 0;
	uint32_t ib = 0;
	while (ia < a->len && ib < b->len)
	{
		uint32_t ua = next_unit_key(a, &ia);
		uint32_t ub = next_unit_key(b, &ib);
		if (ua != ub) return ua < ub ? -1 : 1;
	}
	if (ia < a->len) return 1;
	if (ib < b->len) return -1;
	return 0;
}

int avm2_abstract_lt(Avm2Context* ctx, Avm2Value a, Avm2Value b)
{
	if (a.kind == AVM2_VALUE_INTEGER && b.kind == AVM2_VALUE_INTEGER)
	{
		return a.u.i < b.u.i ? 1 : 0;
	}
	// Mixed/double numeric fast path, exactly equivalent to the generic tail
	// below: avm2_coerce_to_primitive is the identity on non-objects, two
	// numbers are never both strings, and avm2_coerce_to_number on a numeric
	// value is just the widening below. Saves two calls per compare on the
	// dominant shape (Number vs int / Number vs Number).
	if (avm2_value_is_number(a) && avm2_value_is_number(b))
	{
		double na = a.kind == AVM2_VALUE_INTEGER ? (double) a.u.i : a.u.d;
		double nb = b.kind == AVM2_VALUE_INTEGER ? (double) b.u.i : b.u.d;
		if (isnan(na) || isnan(nb)) return -1;
		return na < nb ? 1 : 0;
	}
	Avm2Value pa = avm2_coerce_to_primitive(ctx, a, 1);
	Avm2Value pb = avm2_coerce_to_primitive(ctx, b, 1);

	if (pa.kind == AVM2_VALUE_STRING && pb.kind == AVM2_VALUE_STRING)
	{
		return avm2_string_utf16_cmp(pa.u.str, pb.u.str) < 0 ? 1 : 0;
	}

	double na = avm2_coerce_to_number(ctx, pa);
	double nb = avm2_coerce_to_number(ctx, pb);
	if (isnan(na) || isnan(nb)) return -1;
	return na < nb ? 1 : 0;
}
