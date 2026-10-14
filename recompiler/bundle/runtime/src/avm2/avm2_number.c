// Number / int / uint / Boolean wrapper classes + Math (port of Ruffle
// globals/{number,int,uint,boolean,math}.rs). AVM2 has no boxed numeric
// objects: constructing/calling these classes returns primitives; instance
// methods dispatch off the Number class ivtable for primitive receivers.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>

static double this_number(Avm2Activation* act)
{
	return avm2_coerce_to_number(act->ctx, act->this_val);
}

static double arg_f64(Avm2Activation* act, uint32_t i)
{
	return i < act->argc ? avm2_coerce_to_number(act->ctx, act->args[i])
	                     : NAN;
}

// ---------------------------------------------------------------------------
// Number methods
// ---------------------------------------------------------------------------

// FP's range check does not live in toFixed/toPrecision/toExponential: those
// three are thin AS3 wrappers around one static helper, `Number$/_convert`,
// and that helper is what raises #1002 -- so its frame sits INNERMOST in the
// trace (avm2/number_convert_errors pins 250 such lines). It is deliberately
// absent from the argument-coercion traces in the same test: the argument is
// coerced in the caller's own frame, BEFORE _convert is entered, so the push
// must stay here and not move up into the three entry points.
_Noreturn static void throw_1002(Avm2Context* ctx)
{
	static const Avm2MethodRef convert = { NULL, NULL, "Number$/_convert", 0, 0 };
	avm2_callstack_push(ctx, &convert, NULL);
	avm2_throw_error(ctx, ctx->builtins.range_error_class,
	                 "Error #1002: Number.toPrecision has a range of 1 to 21. "
	                 "Number.toFixed and Number.toExponential have a range of 0 to "
	                 "20. Specified value is not within expected range.");
}

// print_with_radix (Ruffle number.rs): radix 10 → normal coercion.
static const Avm2String* print_with_radix(Avm2Context* ctx, double number, int radix)
{
	char buf[420];
	if (radix == 10)
	{
		avm2_format_number(buf, sizeof(buf), number);
		return avm2_string_from_literal(ctx, buf);
	}
	if (isnan(number)) return avm2_string_from_literal(ctx, "NaN");
	if (isinf(number))
	{
		return avm2_string_from_literal(ctx, number < 0 ? "-Infinity" : "Infinity");
	}
	static const char DIGIT_CHARS[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	char digits[1200];
	int n = 0;
	int neg = number < 0.0;
	number = fabs(number);
	do
	{
		double digit = fmod(number, (double) radix);
		number /= (double) radix;
		digits[n++] = DIGIT_CHARS[(int) digit];
	} while (number >= 1.0 && n < (int) sizeof(digits) - 2);
	if (neg) digits[n++] = '-';
	// Reverse.
	char out[1204];
	for (int i = 0; i < n; i++) out[i] = digits[n - 1 - i];
	return avm2_string_new(ctx, out, (uint32_t) n);
}

static Avm2Value number_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	double number = this_number(act);
	int32_t radix = act->argc > 0 ? avm2_coerce_to_i32(ctx, act->args[0]) : 10;
	if (radix < 2 || radix > 36)
	{
		avm2_throw_error(ctx, ctx->builtins.range_error_class,
		                 "Error #1003: The radix argument must be between 2 and 36; "
		                 "got %d.", radix);
	}
	return avm2_string(print_with_radix(ctx, number, radix));
}

static Avm2Value number_value_of(Avm2Activation* act)
{
	return act->this_val;
}

// Number.prototype.toString / .valueOf / .toLocaleString.
//
// Same hazard as String.prototype.toString: the receiver can be
// `Number.prototype` itself, a bare object with no primitive value.
// this_number() would coerce it, which calls valueOf -> number_value_of,
// which hands the object straight back -> coerce again -> infinite
// recursion and a stack-overflow SIGSEGV. In avmplus `Number.prototype`
// is a Number object whose value is 0, and the tests assert exactly that
// (`Number.prototype.valueOf()` -> 0, `Number.prototype.toString()` ->
// "0"), so a non-numeric receiver is treated as 0.
static bool numeric_receiver(Avm2Value v)
{
	return v.kind == AVM2_VALUE_NUMBER || v.kind == AVM2_VALUE_INTEGER;
}

// ...but only for the prototype ITSELF. Any other object receiver is
// incompatible, and avmplus says so with #1004 (avm2/primitive_valueOf,
// ecma3/Exceptions/number_002_rt assign Number.prototype.valueOf onto a plain
// Object and expect the call to throw). The class name in the message is the
// prototype the shim was installed on, hence one pair per class.
static void number_proto_receiver_check(Avm2Activation* act, Avm2Class* cls,
                                        const char* cls_name, const char* meth)
{
	if (act->this_val.kind == AVM2_VALUE_OBJECT
	    && act->this_val.u.obj == cls->prototype_obj)
	{
		return;
	}
	// The prototype shims are AS3 playerglobal, so FP's trace carries the
	// "Error$/throwError()" frame (avm2/primitive_toString, primitive_valueOf).
	// Per-site opt-in — see avm2_error.h.
	avm2_callstack_push_throwerror(act->ctx);
	avm2_throw_error(act->ctx, act->ctx->builtins.type_error_class,
	                 "Error #1004: Method %s.prototype.%s was invoked on an "
	                 "incompatible object.", cls_name, meth);
}

// How strict each shim is about a NUMERIC receiver. FP's three prototypes do
// not agree, and avm2/primitive_{toString,valueOf} pin every cell of the
// table with `Cls.prototype.m.call(v)` over {1.5, uint(2), -2}:
//
//            | Number | int      | uint
//   toString | any    | integral | any        <- uint.toString(1.5) -> "1.5"
//   valueOf  | any    | integral | uint-exact <- uint.valueOf(-2) throws #1004
//
// (i.e. only uint's two halves disagree with each other — an avmplus
// implementation asymmetry, not a typo: both tests show it, for both signs.)
static bool numeric_receiver_ok(Avm2Value v, int strict)
{
	if (!numeric_receiver(v)) return false;
	if (strict == 0) return true;
	double d = (v.kind == AVM2_VALUE_INTEGER) ? (double) v.u.i : v.u.d;
	if (!isfinite(d) || d != (double) (int64_t) d) return false;
	return strict == 1 || d >= 0.0;
}

#define NUM_RECV_ANY 0
#define NUM_RECV_INT 1
#define NUM_RECV_UINT 2

#define DEFINE_NUMBER_PROTO_SHIMS(tag, cls_field, cls_name, ts_strict, vo_strict) \
	static Avm2Value tag##_proto_to_string(Avm2Activation* act)                \
	{                                                                          \
		if (numeric_receiver_ok(act->this_val, ts_strict))                     \
			return number_to_string(act);                                      \
		number_proto_receiver_check(act, act->ctx->builtins.cls_field,         \
		                            cls_name, "toString");                     \
		Avm2Activation zero = *act;                                            \
		zero.this_val = avm2_number(0.0);                                      \
		return number_to_string(&zero);                                        \
	}                                                                          \
	static Avm2Value tag##_proto_value_of(Avm2Activation* act)                 \
	{                                                                          \
		if (numeric_receiver_ok(act->this_val, vo_strict))                     \
			return act->this_val;                                              \
		number_proto_receiver_check(act, act->ctx->builtins.cls_field,         \
		                            cls_name, "valueOf");                      \
		return avm2_number(0.0);                                               \
	}

DEFINE_NUMBER_PROTO_SHIMS(number, number_class, "Number",
                          NUM_RECV_ANY, NUM_RECV_ANY)
DEFINE_NUMBER_PROTO_SHIMS(int, int_class, "int",
                          NUM_RECV_INT, NUM_RECV_INT)
DEFINE_NUMBER_PROTO_SHIMS(uint, uint_class, "uint",
                          NUM_RECV_ANY, NUM_RECV_UINT)

static Avm2Value number_to_fixed(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	double number = this_number(act);
	int32_t digits = act->argc > 0 ? avm2_coerce_to_i32(ctx, act->args[0]) : 0;
	if (digits < 0 || digits > 20) throw_1002(ctx);
	char buf[400];
	if (isnan(number)) return avm2_string(avm2_string_from_literal(ctx, "NaN"));
	if (isinf(number))
	{
		return avm2_string(avm2_string_from_literal(
			ctx, number < 0 ? "-Infinity" : "Infinity"));
	}
	// ECMA-262 toFixed only emits "-" for x < 0, and -0 is not < 0, so
	// (-0).toFixed(2) is "0.00". C's %f prints the sign bit, so normalize
	// negative zero away first (avm2/number_convert_edge_cases, whose -0 block
	// pins all eight toFixed rows).
	if (number == 0.0) number = 0.0;
	snprintf(buf, sizeof(buf), "%.*f", (int) digits, number);
	return avm2_string(avm2_string_from_literal(ctx, buf));
}

static Avm2Value number_to_exponential(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	double number = this_number(act);
	int32_t digits = act->argc > 0 ? avm2_coerce_to_i32(ctx, act->args[0]) : 0;
	if (digits < 0 || digits > 20) throw_1002(ctx);
	char buf[80];
	if (number == 0.0)
	{
		// Ruffle quirk-for-quirk: 0.toExponential(0) = "1e-15",
		// 0.toExponential(d) = "0.000...e-16".
		if (digits == 0)
		{
			return avm2_string(avm2_string_from_literal(ctx, "1e-15"));
		}
		int n = snprintf(buf, sizeof(buf), "0.");
		for (int i = 0; i < digits && n < (int) sizeof(buf) - 6; i++) buf[n++] = '0';
		snprintf(buf + n, sizeof(buf) - n, "e-16");
		return avm2_string(avm2_string_from_literal(ctx, buf));
	}
	if (isnan(number)) return avm2_string(avm2_string_from_literal(ctx, "NaN"));
	if (isinf(number))
	{
		return avm2_string(avm2_string_from_literal(
			ctx, number < 0 ? "-Infinity" : "Infinity"));
	}
	// C %.*e gives "d.ddde±XX" (2+ exponent digits); Rust gives "e±X" with
	// no zero padding and drops "e+0" entirely.
	char tmp[80];
	snprintf(tmp, sizeof(tmp), "%.*e", (int) digits, number);
	char* e = strchr(tmp, 'e');
	int exp_val = atoi(e + 1);
	*e = '\0';
	if (exp_val == 0)
	{
		snprintf(buf, sizeof(buf), "%s", tmp);
	}
	else
	{
		snprintf(buf, sizeof(buf), "%se%+d", tmp, exp_val);
	}
	return avm2_string(avm2_string_from_literal(ctx, buf));
}

// FP's Number$/_convert, precision form.
//
// This was a port of Ruffle's number.rs, which rounds with floor(x*scale)/scale
// and then prints the result with the SHORTEST round-trip formatter — so every
// trailing zero the precision asked for was dropped ("123".toPrecision(19) came
// out "123", not "123.0000000000000000") and zero came out "0" for every digit
// count. avm2/number_convert_edge_cases pins 18 such rows.
//
// The rule FP actually uses (`e` = the decimal exponent of the value already
// rounded to `wanted_digits` significant digits):
//   * non-finite            -> "NaN" / "±Infinity" (AFTER the range check:
//                              NaN.toPrecision(0) is still #1002)
//   * zero                  -> exactly what toFixed(p) prints, "0." + p zeros
//                              (ECMA would give "0" at p == 1; FP does not)
//   * wanted_digits <= e    -> exponential, wanted_digits-1 fraction digits
//   * otherwise             -> FIXED, wanted_digits-1-e fraction digits
// Note the last arm has no ECMA-style "e < -6 switches to exponential" escape:
// (1.2315e-8).toPrecision(1) is "0.00000001", not "1e-8" (avm2/number_toprecision).
static const Avm2String* print_with_precision(Avm2Context* ctx, double number,
                                              int wanted_digits)
{
	char buf[512];
	if (isnan(number)) return avm2_string_from_literal(ctx, "NaN");
	if (isinf(number))
	{
		return avm2_string_from_literal(ctx, number < 0 ? "-Infinity" : "Infinity");
	}
	if (number == 0.0)
	{
		snprintf(buf, sizeof(buf), "%.*f", wanted_digits, 0.0);
		return avm2_string_from_literal(ctx, buf);
	}
	// Read the exponent back out of %e so the rounding that decides
	// 9.99 -> 1e+1 is printf's, not a log10 approximation.
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%.*e", wanted_digits - 1, number);
	char* ep = strchr(tmp, 'e');
	int e = (ep != NULL) ? atoi(ep + 1) : 0;
	if (wanted_digits <= e)
	{
		if (ep != NULL) *ep = '\0';
		// FP spells the exponent unpadded and always signed ("1.2e+2"),
		// unlike C's two-digit "e+02".
		snprintf(buf, sizeof(buf), "%se%+d", tmp, e);
		return avm2_string_from_literal(ctx, buf);
	}
	int frac = wanted_digits - 1 - e;
	if (frac < 0) frac = 0;
	if (frac > 400) frac = 400;
	snprintf(buf, sizeof(buf), "%.*f", frac, number);
	return avm2_string_from_literal(ctx, buf);
}

static Avm2Value number_to_precision(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	double number = this_number(act);
	// An ABSENT precision means plain toString — and FP treats `null` as absent
	// here, not as 0: NaN.toPrecision(null) is "NaN" while NaN.toPrecision(0)
	// is #1002 (avm2/number_convert_edge_cases, six rows).
	if (act->argc == 0 || act->args[0].kind == AVM2_VALUE_UNDEFINED
	    || act->args[0].kind == AVM2_VALUE_NULL)
	{
		char buf[420];
		avm2_format_number(buf, sizeof(buf), number);
		return avm2_string(avm2_string_from_literal(ctx, buf));
	}
	int32_t digits = avm2_coerce_to_i32(ctx, act->args[0]);
	if (digits < 1 || digits > 21) throw_1002(ctx);
	return avm2_string(print_with_precision(ctx, number, digits));
}

static Avm2Value number_construct(Avm2Context* ctx, Avm2Class* cls,
                                  const Avm2Value* args, uint32_t argc)
{
	(void) cls;
	if (argc == 0) return avm2_number(0.0);
	return avm2_number(avm2_coerce_to_number(ctx, args[0]));
}

static Avm2Value int_construct(Avm2Context* ctx, Avm2Class* cls,
                               const Avm2Value* args, uint32_t argc)
{
	(void) cls;
	if (argc == 0) return avm2_integer(0);
	return avm2_integer(avm2_coerce_to_i32(ctx, args[0]));
}

static Avm2Value uint_construct(Avm2Context* ctx, Avm2Class* cls,
                                const Avm2Value* args, uint32_t argc)
{
	(void) cls;
	if (argc == 0) return avm2_integer(0);
	return avm2_uint_value(avm2_coerce_to_u32(ctx, args[0]));
}

static Avm2Value boolean_construct(Avm2Context* ctx, Avm2Class* cls,
                                   const Avm2Value* args, uint32_t argc)
{
	(void) ctx; (void) cls;
	if (argc == 0) return avm2_bool(false);
	return avm2_bool(avm2_coerce_to_boolean(args[0]));
}

static Avm2Value boolean_to_string(Avm2Activation* act)
{
	bool b = avm2_coerce_to_boolean(act->this_val);
	return avm2_string(avm2_string_from_literal(act->ctx, b ? "true" : "false"));
}

static Avm2Value boolean_value_of(Avm2Activation* act)
{
	return avm2_bool(avm2_coerce_to_boolean(act->this_val));
}

// Boolean.prototype.toString / .valueOf.
//
// Same shape as number_proto_to_string/value_of above: in avmplus
// `Boolean.prototype` is a Boolean object whose value is false, so the
// tests assert `Boolean.prototype.valueOf()` -> false and
// `String(Boolean.prototype)` -> "false". The class methods coerce their
// receiver, and an object coerces to true, so registering them directly
// on the prototype yields true for both. A non-boolean receiver is the
// prototype (or something equally value-less) and reads as false.
static Avm2Value boolean_proto_to_string(Avm2Activation* act)
{
	if (act->this_val.kind != AVM2_VALUE_BOOL)
	{
		number_proto_receiver_check(act, act->ctx->builtins.boolean_class,
		                            "Boolean", "toString");
	}
	bool b = act->this_val.kind == AVM2_VALUE_BOOL && act->this_val.u.b;
	return avm2_string(avm2_string_from_literal(act->ctx, b ? "true" : "false"));
}

static Avm2Value boolean_proto_value_of(Avm2Activation* act)
{
	if (act->this_val.kind == AVM2_VALUE_BOOL) return act->this_val;
	number_proto_receiver_check(act, act->ctx->builtins.boolean_class,
	                            "Boolean", "valueOf");
	return avm2_bool(false);
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

#define MATH1(name, expr) \
	static Avm2Value math_##name(Avm2Activation* act) \
	{ \
		double x = arg_f64(act, 0); \
		(void) x; \
		return avm2_number(expr); \
	}

MATH1(abs, fabs(x))
MATH1(acos, acos(x))
MATH1(asin, asin(x))
MATH1(atan, atan(x))
MATH1(ceil, ceil(x))
MATH1(cos, cos(x))
MATH1(exp, exp(x))
MATH1(floor, floor(x))
MATH1(log, log(x))
MATH1(sin, sin(x))
MATH1(sqrt, sqrt(x))
MATH1(tan, tan(x))
MATH1(round, floor(x + 0.5))
#undef MATH1

static Avm2Value math_atan2(Avm2Activation* act)
{
	return avm2_number(atan2(arg_f64(act, 0), arg_f64(act, 1)));
}

static Avm2Value math_pow(Avm2Activation* act)
{
	double n = arg_f64(act, 0);
	double p = arg_f64(act, 1);
	// Flash quirks (Ruffle math.rs pow).
	if (!isfinite(n) || !isfinite(p))
	{
		if (isnan(p)) return avm2_number(NAN);
		if ((n == 1.0 || n == -1.0) && isinf(p)) return avm2_number(NAN);
		if (n == -INFINITY && p < 0.0 && isfinite(p)
		    && p == floor(p) && fmod(p, 2.0) == 0.0)
		{
			return avm2_number(-0.0);
		}
	}
	return avm2_number(pow(n, p));
}

static Avm2Value math_max(Avm2Activation* act)
{
	double cur = -INFINITY;
	for (uint32_t i = 0; i < act->argc; i++)
	{
		double v = avm2_coerce_to_number(act->ctx, act->args[i]);
		if (isnan(v)) return avm2_number(NAN);
		// total order: +0 > -0.
		if (v > cur || (v == cur && !signbit(v) && signbit(cur))) cur = v;
	}
	return avm2_number(cur);
}

static Avm2Value math_min(Avm2Activation* act)
{
	double cur = INFINITY;
	for (uint32_t i = 0; i < act->argc; i++)
	{
		double v = avm2_coerce_to_number(act->ctx, act->args[i]);
		if (isnan(v)) return avm2_number(NAN);
		if (v < cur || (v == cur && signbit(v) && !signbit(cur))) cur = v;
	}
	return avm2_number(cur);
}

// avmplus-compatible RNG (same algorithm + MOCK_DATE_TIME seeding as
// actionmodern/math.c, whose state is file-static and unreachable here).
typedef struct { uint32_t uValue, uSequenceLength, uXorMask; } Avm2RandomFast;
static Avm2RandomFast g_avm2_rng;

static const uint32_t avm2_random_xor_masks[31] = {
	0x00000003, 0x00000006, 0x0000000C, 0x00000014, 0x00000030, 0x00000060,
	0x000000B8, 0x00000110, 0x00000240, 0x00000500, 0x00000CA0, 0x00001B00,
	0x00003500, 0x00006000, 0x0000B400, 0x00012000, 0x00020400, 0x00072000,
	0x00090000, 0x00140000, 0x00300000, 0x00400000, 0x00D80000, 0x01200000,
	0x03880000, 0x07200000, 0x09000000, 0x14000000, 0x32800000, 0x48000000,
	0xA3000000,
};

static int32_t avm2_random_pure_hasher(int32_t iSeed)
{
	const int32_t c1 = 1376312589L;
	const int32_t c2 = 789221L;
	const int32_t c3 = 15731L;
	iSeed = ((iSeed << 13) ^ iSeed) - (iSeed >> 21);
	int32_t iResult = (iSeed * (iSeed * iSeed * c3 + c2) + c1) & 0x7FFFFFFF;
	iResult += iSeed;
	iResult = ((iResult << 13) ^ iResult) - (iResult >> 21);
	return iResult;
}

// The build's own boot seed. MOCK_DATE_TIME is a -D and the artifact hash is
// what pins it; the reset hook below COMPOSES with it (seed 0 means "back to
// this build's boot state") rather than replacing it.
static uint32_t avm2_random_boot_seed(void)
{
#ifdef MOCK_DATE_TIME
	return (uint32_t) ((int64_t) (MOCK_DATE_TIME) * 1000LL);
#else
	return 987654321u;
#endif
}

static void avm2_random_seed(Avm2RandomFast* r, uint32_t seed)
{
	int n = 31;
	r->uValue = (seed == 0) ? avm2_random_boot_seed() : seed;
	r->uSequenceLength = (uint32_t) ((1ULL << n) - 1ULL);
	r->uXorMask = avm2_random_xor_masks[n - 2];
}

static int32_t avm2_random_next(Avm2RandomFast* r)
{
	if (r->uValue == 0) avm2_random_seed(r, 0);
	if (r->uValue & 1u)
	{
		r->uValue = (r->uValue >> 1) ^ r->uXorMask;
	}
	else
	{
		r->uValue >>= 1;
	}
	int32_t aNum = (int32_t) r->uValue;
	aNum = avm2_random_pure_hasher(aNum * 71L);
	return aNum & 0x7FFFFFFF;
}

// Draws taken from the MAIN stream. `Math.random()` is the only way to move
// that generator, so this counts exactly the numbers a replay would have to
// step past. It is deliberately NOT reset by setState: the point of a
// monotonic counter is that a per-tick sample can subtract two readings and
// get a draw COUNT, and a reset would silently make a window read as zero.
// `cosmetic()` runs a separate generator and is not counted -- counting it
// would defeat the whole reason the second stream exists.
static uint64_t g_avm2_rng_draws;

static int32_t avm2_generate_random_number(void)
{
	g_avm2_rng_draws++;
	return avm2_random_next(&g_avm2_rng);
}

static Avm2Value math_random(Avm2Activation* act)
{
	(void) act;
	int32_t raw = avm2_generate_random_number();
	return avm2_number((double) raw / 2147483648.0);
}

// ── swfmodern.Rng — the determinism hooks ────────────────────────────────
//
// `Math.random()` is one global 31-bit XOR-shift LFSR whose whole state is
// the single uint32 above, so a page's draw sequence is a pure function of
// its boot seed and of HOW MANY DRAWS HAVE HAPPENED. That second half is the
// problem this class exists for: a harness replaying a recorded input tape
// can prove the game REPRODUCES (same page, same tape, same rocks) without
// being able to PREDICT anything, because the stream position at the start of
// the interesting window is the whole page's history — every Tile and Enemy
// constructed since load, every sound index rolled, every frame of camera
// shake. No readout carries that number.
//
// `reset(seed)` deletes the question instead of answering it: after it, a
// model owes only the draws the window itself consumes. `state`/`setState`
// exist so an instrument can SAMPLE the stream (draw N values, then put the
// state back) without perturbing the run it is measuring.
//
// `cosmetic()` is the second half: a SEPARATE generator, so cooperative
// content can route draws whose values nothing gameplay-visible reads (sprite
// frames, particle jitter, sound indices) off the main stream, where they
// otherwise shift every gameplay draw that follows. Nothing routes itself —
// only the content knows which of its own draws are which — so this is inert
// until something calls it, and `Math.random()` is untouched either way.
//
// `draws()` closes the loop on the paragraph above: `state` says WHERE the
// stream is, `draws()` says HOW FAR it has come. With one seeded generator the
// values themselves are then recoverable offline (step the LFSR N times), so a
// per-tick sample of the pair describes the stream completely instead of
// merely proving it moved.
//
// Reachable from AS3 as flash.utils.getDefinitionByName("swfmodern.Rng"); a
// build without it throws #1065 there, which is a loud absence rather than a
// silent unseeded run.
static Avm2RandomFast g_avm2_rng_cosmetic;

static uint32_t rng_arg_u32(Avm2Activation* act, uint32_t i)
{
	if (act->argc <= i) return 0;
	double v = avm2_coerce_to_number(act->ctx, act->args[i]);
	if (!isfinite(v) || v < 0.0) return 0;
	return (uint32_t) fmod(v, 4294967296.0);
}

static Avm2Value rng_state(Avm2Activation* act)
{
	(void) act;
	return avm2_number((double) g_avm2_rng.uValue);
}

static Avm2Value rng_set_state(Avm2Activation* act)
{
	avm2_random_seed(&g_avm2_rng, rng_arg_u32(act, 0));
	return avm2_undefined();
}

static Avm2Value rng_cosmetic_state(Avm2Activation* act)
{
	(void) act;
	return avm2_number((double) g_avm2_rng_cosmetic.uValue);
}

static Avm2Value rng_set_cosmetic_state(Avm2Activation* act)
{
	avm2_random_seed(&g_avm2_rng_cosmetic, rng_arg_u32(act, 0));
	return avm2_undefined();
}

static Avm2Value rng_draws(Avm2Activation* act)
{
	(void) act;
	// A double carries every count below 2^53; the LFSR would have to be
	// stepped for millennia to reach that, so no precision note is needed.
	return avm2_number((double) g_avm2_rng_draws);
}

static Avm2Value rng_cosmetic(Avm2Activation* act)
{
	(void) act;
	int32_t raw = avm2_random_next(&g_avm2_rng_cosmetic);
	return avm2_number((double) raw / 2147483648.0);
}

// `Math` is a pure static namespace: avmplus gives it neither a call nor a
// construct behaviour and has dedicated errors for each attempt. Without
// these hooks `Math()` would fall through to the generic one-arg class
// coercion (#1112) and `new Math()` would happily hand back an instance.
// Only Math gets this -- `JSON()` really is #1112 (ecma3/JSON/e15_12_0).
static Avm2Value math_class_call(Avm2Context* ctx, Avm2Class* cls,
                                 const Avm2Value* args, uint32_t argc)
{
	(void) cls; (void) args; (void) argc;
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1075: Math is not a function.");
}

static Avm2Value math_class_construct(Avm2Context* ctx, Avm2Class* cls,
                                      const Avm2Value* args, uint32_t argc)
{
	(void) cls; (void) args; (void) argc;
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1076: Math is not a constructor.");
}

// ---------------------------------------------------------------------------
// Number static math (Flash 11.3 / API 680)
// ---------------------------------------------------------------------------
//
// `Number` re-exports the whole `Math` surface as statics. It is NOT a plain
// alias: these are *declared* natives, so the arg count is checked (avmplus
// throws ArgumentError #1063 when it does not match the signature) and each
// one's `.length` is the declared arity, which as3/Types/Number pins per
// method. `max`/`min` are variadic (`...rest`) with a declared length of 2, so
// they accept any count — as3/Types/Number/visibility/v16 calls both with none.
//
// The results still come back as plain doubles: getQualifiedClassName maps an
// integral double to "int", which is exactly what the tests assert
// (`Number.abs(1)` → "int", `Number.abs(3.14)` → "Number").

static void num_check_argc(Avm2Activation* act, const char* name, uint32_t expected)
{
	if (act->argc == expected) return;
	avm2_throw_error(act->ctx, act->ctx->builtins.argument_error_class,
	                 "Error #1063: Argument count mismatch on Number$/%s(). "
	                 "Expected %u, got %u.", name, expected, act->argc);
}

#define NUMSTATIC(name, arity) \
	static Avm2Value number_static_##name(Avm2Activation* act) \
	{ \
		num_check_argc(act, #name, arity); \
		return math_##name(act); \
	}

NUMSTATIC(abs, 1)
NUMSTATIC(acos, 1)
NUMSTATIC(asin, 1)
NUMSTATIC(atan, 1)
NUMSTATIC(ceil, 1)
NUMSTATIC(cos, 1)
NUMSTATIC(exp, 1)
NUMSTATIC(floor, 1)
NUMSTATIC(log, 1)
NUMSTATIC(round, 1)
NUMSTATIC(sin, 1)
NUMSTATIC(sqrt, 1)
NUMSTATIC(tan, 1)
NUMSTATIC(atan2, 2)
NUMSTATIC(pow, 2)
NUMSTATIC(random, 0)
#undef NUMSTATIC

// The eight constants must be read-only (`Number.E = 0` throws
// ReferenceError #1074), non-deletable and non-enumerable — as3/Types/Number/e
// asserts all four. A getter-only static trait gives all of that for free;
// `avm2_builtin_add_static_const` installs a dont-enum *dynamic* property,
// which is writable and deletable.
#define NUMCONST(fname, value) \
	static Avm2Value number_const_##fname(Avm2Activation* act) \
	{ \
		(void) act; \
		return avm2_number(value); \
	}

NUMCONST(e, 2.718281828459045)
NUMCONST(ln10, 2.302585092994046)
NUMCONST(ln2, 0.6931471805599453)
NUMCONST(log10e, 0.4342944819032518)
NUMCONST(log2e, 1.4426950408889634)
NUMCONST(pi, 3.141592653589793)
NUMCONST(sqrt1_2, 0.7071067811865476)
NUMCONST(sqrt2, 1.4142135623730951)
#undef NUMCONST

static void add_number_statics(Avm2Context* ctx, Avm2Class* cls)
{
	static const struct { const char* name; Avm2MethodFn fn; uint32_t arity; }
	methods[] = {
		{ "abs", number_static_abs, 1 },     { "acos", number_static_acos, 1 },
		{ "asin", number_static_asin, 1 },   { "atan", number_static_atan, 1 },
		{ "atan2", number_static_atan2, 2 }, { "ceil", number_static_ceil, 1 },
		{ "cos", number_static_cos, 1 },     { "exp", number_static_exp, 1 },
		{ "floor", number_static_floor, 1 }, { "log", number_static_log, 1 },
		{ "max", math_max, 2 },              { "min", math_min, 2 },
		{ "pow", number_static_pow, 2 },     { "random", number_static_random, 0 },
		{ "round", number_static_round, 1 }, { "sin", number_static_sin, 1 },
		{ "sqrt", number_static_sqrt, 1 },   { "tan", number_static_tan, 1 },
	};
	for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
	{
		avm2_builtin_add_static_method_n(ctx, cls, methods[i].name,
		                                 methods[i].fn, methods[i].arity);
	}

	static const struct { const char* name; Avm2MethodFn fn; } consts[] = {
		{ "E", number_const_e },           { "LN10", number_const_ln10 },
		{ "LN2", number_const_ln2 },       { "LOG10E", number_const_log10e },
		{ "LOG2E", number_const_log2e },   { "PI", number_const_pi },
		{ "SQRT1_2", number_const_sqrt1_2 }, { "SQRT2", number_const_sqrt2 },
	};
	for (size_t i = 0; i < sizeof(consts) / sizeof(consts[0]); i++)
	{
		avm2_builtin_add_static_getset(ctx, cls, consts[i].name, consts[i].fn, NULL);
	}
}

// ---------------------------------------------------------------------------
// Stack-frame spelling for the three convert methods
// ---------------------------------------------------------------------------
//
// toFixed / toExponential / toPrecision are declared in the AS3 builtin
// namespace in playerglobal, and FP spells a non-public trait's frame with its
// URI: "Number/http://adobe.com/AS3/2006/builtin::toFixed()". Same rule as
// "Function/http://adobe.com/AS3/2006/builtin::call()" (avm2_function.c) --
// only the frame name moves, dispatch keys stay public.
//
// The receiver class is always Number here even for an `int`/`uint` receiver:
// avm2_value_class maps AVM2_VALUE_INTEGER and AVM2_VALUE_NUMBER alike to
// Number, so a primitive never reaches int's or uint's own vtable. That is
// also why FP's SECOND frame for an int receiver ("int/...::toFixed()" outside
// "Number/...::toFixed()") is not reproduced -- it is a STATIC-type rule, not
// a value-kind one, and int and uint are the same runtime value here. See the
// session-18 w2-avmplus-numerics report.
#define AS3_BUILTIN_NS "http://adobe.com/AS3/2006/builtin::"

static Avm2Value convert_framed(Avm2Activation* act, Avm2MethodFn self,
                                Avm2MethodFn impl, const char* frame)
{
	Avm2Context* ctx = act->ctx;
	uint32_t depth0 = ctx->call_depth;
	avm2_callstack_rename_frame(ctx, self, frame);
	Avm2Value r = impl(act);
	// Restore exactly what we found; the throwing path longjmps past this and
	// the unwind resets call_depth for us.
	ctx->call_depth = depth0;
	return r;
}

#define DEFINE_CONVERT_WRAPPER(impl, meth)                                     \
	static Avm2Value impl##_framed(Avm2Activation* act)                        \
	{                                                                          \
		return convert_framed(act, impl##_framed, impl,                        \
		                      "Number/" AS3_BUILTIN_NS meth);                  \
	}

DEFINE_CONVERT_WRAPPER(number_to_fixed, "toFixed")
DEFINE_CONVERT_WRAPPER(number_to_exponential, "toExponential")
DEFINE_CONVERT_WRAPPER(number_to_precision, "toPrecision")

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static void add_number_methods(Avm2Context* ctx, Avm2Class* cls,
                               Avm2MethodFn proto_to_string,
                               Avm2MethodFn proto_value_of)
{
	avm2_builtin_add_method(ctx, cls, "toString", number_to_string);
	// No `toLocaleString` TRAIT. avmplus's Number/int/uint declare toString,
	// valueOf, toFixed, toExponential and toPrecision, but toLocaleString
	// exists only as a *prototype function* (ES3 compat) -- it is absent from
	// the AS3 Object and Number class surfaces alike. The difference is
	// observable: writing a method name that IS a trait reports #1037
	// (ecma3/Exceptions/number_003_rt `o.valueOf = ...`, number_004_rt
	// `o.toString = ...`, both on `new Number()`), while writing
	// toLocaleString finds no trait, so the sealed receiver reports #1056
	// (ecma3/Number/toLocaleString_rt). Ordinary `n.toLocaleString()` calls
	// still resolve through the prototype entry added below, whose numeric
	// arm is number_to_string.
	avm2_builtin_add_method(ctx, cls, "valueOf", number_value_of);
	// The three convert methods carry their AS3 namespace URI in a stack
	// frame; the ES3 prototype copies below deliberately do NOT (a prototype
	// call is a Function frame in FP, not a class-trait one).
	avm2_builtin_add_method(ctx, cls, "toFixed", number_to_fixed_framed);
	avm2_builtin_add_method(ctx, cls, "toExponential",
	                        number_to_exponential_framed);
	avm2_builtin_add_method(ctx, cls, "toPrecision", number_to_precision_framed);

	// ES3-compat layer on the prototype (Ruffle globals/Number.as).
	Avm2Object* proto = cls->prototype_obj;
	avm2_proto_add_function_n(ctx, proto, "toString", proto_to_string, 1);
	avm2_proto_add_function_n(ctx, proto, "toLocaleString", proto_to_string, 1);
	avm2_proto_add_function_n(ctx, proto, "valueOf", proto_value_of, 0);
	avm2_proto_add_function_n(ctx, proto, "toFixed", number_to_fixed, 1);
	avm2_proto_add_function_n(ctx, proto, "toExponential", number_to_exponential, 1);
	avm2_proto_add_function_n(ctx, proto, "toPrecision", number_to_precision, 1);
}

void avm2_register_number(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;

	b->number_class = avm2_builtin_class(ctx, "", "Number", b->object_class);
	b->number_class->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	b->number_class->native_construct = number_construct;
	b->number_class->native_call = number_construct;
	add_number_methods(ctx, b->number_class, number_proto_to_string,
	                   number_proto_value_of);
	avm2_builtin_add_static_const(ctx, b->number_class, "MAX_VALUE",
	                              avm2_number(1.7976931348623157e308));
	avm2_builtin_add_static_const(ctx, b->number_class, "MIN_VALUE",
	                              avm2_number(4.9406564584124654e-324));
	avm2_builtin_add_static_const(ctx, b->number_class, "NaN", avm2_number(NAN));
	avm2_builtin_add_static_const(ctx, b->number_class, "POSITIVE_INFINITY",
	                              avm2_number(INFINITY));
	avm2_builtin_add_static_const(ctx, b->number_class, "NEGATIVE_INFINITY",
	                              avm2_number(-INFINITY));
	// API 680: the Math surface on Number is SWF16+ only. SWF15 content must
	// still see `Number.abs` as undefined (as3/Types/Number/visibility/v15
	// asserts TypeError #1006 for every one of them).
	if (ctx->swf_version >= 16) add_number_statics(ctx, b->number_class);

	b->int_class = avm2_builtin_class(ctx, "", "int", b->object_class);
	b->int_class->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	b->int_class->native_construct = int_construct;
	b->int_class->native_call = int_construct;
	add_number_methods(ctx, b->int_class, int_proto_to_string,
	                   int_proto_value_of);
	avm2_builtin_add_static_const(ctx, b->int_class, "MAX_VALUE",
	                              avm2_integer(2147483647));
	avm2_builtin_add_static_const(ctx, b->int_class, "MIN_VALUE",
	                              avm2_integer((int32_t) -2147483648LL));

	b->uint_class = avm2_builtin_class(ctx, "", "uint", b->object_class);
	b->uint_class->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	b->uint_class->native_construct = uint_construct;
	b->uint_class->native_call = uint_construct;
	add_number_methods(ctx, b->uint_class, uint_proto_to_string,
	                   uint_proto_value_of);
	avm2_builtin_add_static_const(ctx, b->uint_class, "MAX_VALUE",
	                              avm2_number(4294967295.0));
	avm2_builtin_add_static_const(ctx, b->uint_class, "MIN_VALUE", avm2_integer(0));

	b->boolean_class = avm2_builtin_class(ctx, "", "Boolean", b->object_class);
	b->boolean_class->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	b->boolean_class->native_construct = boolean_construct;
	b->boolean_class->native_call = boolean_construct;
	avm2_builtin_add_method(ctx, b->boolean_class, "toString", boolean_to_string);
	avm2_builtin_add_method(ctx, b->boolean_class, "valueOf", boolean_value_of);
	avm2_proto_add_function(ctx, b->boolean_class->prototype_obj, "toString",
	                        boolean_proto_to_string);
	avm2_proto_add_function(ctx, b->boolean_class->prototype_obj, "valueOf",
	                        boolean_proto_value_of);

	Avm2Class* math = avm2_builtin_class(ctx, "", "Math", b->object_class);
	b->math_class = math;
	math->native_call = math_class_call;
	math->native_construct = math_class_construct;
	avm2_builtin_add_static_method(ctx, math, "abs", math_abs);
	avm2_builtin_add_static_method(ctx, math, "acos", math_acos);
	avm2_builtin_add_static_method(ctx, math, "asin", math_asin);
	avm2_builtin_add_static_method(ctx, math, "atan", math_atan);
	avm2_builtin_add_static_method(ctx, math, "atan2", math_atan2);
	avm2_builtin_add_static_method(ctx, math, "ceil", math_ceil);
	avm2_builtin_add_static_method(ctx, math, "cos", math_cos);
	avm2_builtin_add_static_method(ctx, math, "exp", math_exp);
	avm2_builtin_add_static_method(ctx, math, "floor", math_floor);
	avm2_builtin_add_static_method(ctx, math, "log", math_log);
	avm2_builtin_add_static_method(ctx, math, "max", math_max);
	avm2_builtin_add_static_method(ctx, math, "min", math_min);
	avm2_builtin_add_static_method(ctx, math, "pow", math_pow);
	avm2_builtin_add_static_method(ctx, math, "random", math_random);
	avm2_builtin_add_static_method(ctx, math, "round", math_round);
	avm2_builtin_add_static_method(ctx, math, "sin", math_sin);
	avm2_builtin_add_static_method(ctx, math, "sqrt", math_sqrt);
	avm2_builtin_add_static_method(ctx, math, "tan", math_tan);
	avm2_builtin_add_static_const(ctx, math, "E", avm2_number(2.718281828459045));
	avm2_builtin_add_static_const(ctx, math, "LN10", avm2_number(2.302585092994046));
	avm2_builtin_add_static_const(ctx, math, "LN2", avm2_number(0.6931471805599453));
	avm2_builtin_add_static_const(ctx, math, "LOG10E", avm2_number(0.4342944819032518));
	avm2_builtin_add_static_const(ctx, math, "LOG2E", avm2_number(1.4426950408889634));
	avm2_builtin_add_static_const(ctx, math, "PI", avm2_number(3.141592653589793));
	avm2_builtin_add_static_const(ctx, math, "SQRT1_2", avm2_number(0.7071067811865476));
	avm2_builtin_add_static_const(ctx, math, "SQRT2", avm2_number(1.4142135623730951));

	// The determinism hooks (see swfmodern.Rng above). Its own class in its
	// own package rather than four more statics on Math: Math's surface is
	// graded against avmplus and this is a runtime instrument, not ECMA.
	// ⛓ `setState` IS `reset` for this generator — the state is one uint32
	// and the seed is written straight into it, so "reset to seed s" and
	// "put the state back to s" are the same call. 0 means the build's own
	// boot seed (and is not a reachable state: the LFSR never enters 0).
	Avm2Class* rng = avm2_builtin_class(ctx, "swfmodern", "Rng", b->object_class);
	rng->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	avm2_builtin_add_static_method_n(ctx, rng, "state", rng_state, 0);
	avm2_builtin_add_static_method_n(ctx, rng, "setState", rng_set_state, 1);
	avm2_builtin_add_static_method_n(ctx, rng, "cosmeticState",
	                                 rng_cosmetic_state, 0);
	avm2_builtin_add_static_method_n(ctx, rng, "setCosmeticState",
	                                 rng_set_cosmetic_state, 1);
	avm2_builtin_add_static_method_n(ctx, rng, "cosmetic", rng_cosmetic, 0);
	// ⛓ `draws` is MONOTONIC for the life of the process — setState does not
	// zero it. A stream that reports only its state says the state moved; one
	// that also reports its draw count is self-describing (the values are
	// recoverable offline by stepping the generator that many times).
	avm2_builtin_add_static_method_n(ctx, rng, "draws", rng_draws, 0);
}
