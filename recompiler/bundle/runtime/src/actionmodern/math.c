// math.c — Math object, isNaN/isFinite, and avmplus-compatible RNG.
//
// Carved out of action.c per
// SWFRecompDocs/plans/split-action-c-math-first.md. The public surface
// is in actionmodern/actionmath.h; everything else here is file-static.

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <actionmodern/action_internal.h>
#include <actionmodern/actionmath.h>

// ============================================================================
// Math object state
// ============================================================================

// 18 methods: abs, sin, cos, tan, exp, log, sqrt, round, floor, ceil,
// atan, asin, acos, atan2, pow, min, max, random
static ASFunction g_math_funcs[18];
static ASObject* g_math_object = NULL;
static int g_math_init_done = 0;

// ============================================================================
// Math builtins
// ============================================================================

// --- Unary Math functions (abs, sin, cos, tan, exp, log, sqrt, round, floor, ceil, atan, asin, acos) ---

static ActionVar builtin_math_abs(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(fabs(mathArgToDouble(&args[0])));
}

static ActionVar builtin_math_sin(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(sin(mathArgToDouble(&args[0])));
}

static ActionVar builtin_math_cos(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(cos(mathArgToDouble(&args[0])));
}

static ActionVar builtin_math_tan(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(tan(mathArgToDouble(&args[0])));
}

static ActionVar builtin_math_exp(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(exp(mathArgToDouble(&args[0])));
}

static ActionVar builtin_math_log(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(log(mathArgToDouble(&args[0])));
}

static ActionVar builtin_math_sqrt(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(sqrt(mathArgToDouble(&args[0])));
}

static ActionVar builtin_math_round(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	double x = mathArgToDouble(&args[0]);
	// Flash rounds halves to +inf (not banker's rounding)
	return mathReturnDouble(floor(x + 0.5));
}

static ActionVar builtin_math_floor(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(floor(mathArgToDouble(&args[0])));
}

static ActionVar builtin_math_ceil(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(ceil(mathArgToDouble(&args[0])));
}

static ActionVar builtin_math_atan(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(atan(mathArgToDouble(&args[0])));
}

static ActionVar builtin_math_asin(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(asin(mathArgToDouble(&args[0])));
}

static ActionVar builtin_math_acos(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	return mathReturnDouble(acos(mathArgToDouble(&args[0])));
}

// --- Binary Math functions (atan2, pow, min, max) ---

static ActionVar builtin_math_atan2(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count < 2) return mathReturnDouble(NAN);
	double y = mathArgToDouble(&args[0]);
	double x = mathArgToDouble(&args[1]);
	return mathReturnDouble(atan2(y, x));
}

static ActionVar builtin_math_pow(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(NAN);
	double base = mathArgToDouble(&args[0]);
	// Missing exponent = NaN (not 0 or 1)
	double exponent = (arg_count >= 2) ? mathArgToDouble(&args[1]) : NAN;
	return mathReturnDouble(pow(base, exponent));
}

static ActionVar builtin_math_min(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(INFINITY);
	if (arg_count == 1) return mathReturnDouble(NAN);
	double a = mathArgToDouble(&args[0]);
	double b = mathArgToDouble(&args[1]);
	if (isnan(a) || isnan(b)) return mathReturnDouble(NAN);
	// Handle -0 vs +0: -0 < +0 in min (negative wins)
	if (a < b) return mathReturnDouble(a);
	if (b < a) return mathReturnDouble(b);
	// a == b (including 0 == -0): prefer negative
	return mathReturnDouble(signbit(a) ? a : b);
}

static ActionVar builtin_math_max(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	if (arg_count == 0) return mathReturnDouble(-INFINITY);
	if (arg_count == 1) return mathReturnDouble(NAN);
	double a = mathArgToDouble(&args[0]);
	double b = mathArgToDouble(&args[1]);
	if (isnan(a) || isnan(b)) return mathReturnDouble(NAN);
	if (a > b) return mathReturnDouble(a);
	if (b > a) return mathReturnDouble(b);
	// a == b (including 0 == -0): prefer positive
	return mathReturnDouble(signbit(a) ? b : a);
}

// --- ASnative (class 200 = Math) ---

// NaN stub: coerces args (calling valueOf on first two) but always returns NaN.
// Returned by ASnative(200, invalid_index).
static ActionVar builtin_math_nan_stub(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	return mathReturnDouble(NAN);
}

// ============================================================================
// avmplus-compatible Random Number Generator
// Based on Adobe's ActionScript VM (avmplus) implementation
// Source: https://github.com/adobe/avmplus/blob/master/core/MathUtils.cpp
// ============================================================================

typedef struct {
	uint32_t uValue;           // Random result and seed for next random result
	uint32_t uXorMask;         // XOR mask for generating the next random value
	uint32_t uSequenceLength;  // Number of values in the sequence
} TRandomFast;

#define kRandomPureMax 0x7FFFFFFFL

// XOR masks for random number generation (generates 2^n - 1 numbers)
static const uint32_t Random_Xor_Masks[31] = {
	0x00000003L, 0x00000006L, 0x0000000CL, 0x00000014L, 0x00000030L, 0x00000060L, 0x000000B8L, 0x00000110L,
	0x00000240L, 0x00000500L, 0x00000CA0L, 0x00001B00L, 0x00003500L, 0x00006000L, 0x0000B400L, 0x00012000L,
	0x00020400L, 0x00072000L, 0x00090000L, 0x00140000L, 0x00300000L, 0x00400000L, 0x00D80000L, 0x01200000L,
	0x03880000L, 0x07200000L, 0x09000000L, 0x14000000L, 0x32800000L, 0x48000000L, 0xA3000000L
};

// Global RNG state (initialized on first use or at startup)
static TRandomFast global_random_state = {0, 0, 0};

// Initialize the random number generator with a seed
static void RandomFastInit(TRandomFast *pRandomFast, uint32_t seed) {
	int32_t n = 31;
	pRandomFast->uValue = seed;
	pRandomFast->uSequenceLength = (1L << n) - 1L;
	pRandomFast->uXorMask = Random_Xor_Masks[n - 2];
}

// Generate next random value using XOR shift
static int32_t RandomFastNext(TRandomFast *pRandomFast) {
	if (pRandomFast->uValue & 1L) {
		pRandomFast->uValue = (pRandomFast->uValue >> 1L) ^ pRandomFast->uXorMask;
	} else {
		pRandomFast->uValue >>= 1L;
	}
	return (int32_t)pRandomFast->uValue;
}

// Hash function for additional randomness
static int32_t RandomPureHasher(int32_t iSeed) {
	const int32_t c1 = 1376312589L;
	const int32_t c2 = 789221L;
	const int32_t c3 = 15731L;

	iSeed = ((iSeed << 13) ^ iSeed) - (iSeed >> 21);
	int32_t iResult = (iSeed * (iSeed * iSeed * c3 + c2) + c1) & kRandomPureMax;
	iResult += iSeed;
	iResult = ((iResult << 13) ^ iResult) - (iResult >> 21);

	return iResult;
}

// Generate a random number (avmplus implementation)
static int32_t GenerateRandomNumber(TRandomFast *pRandomFast) {
	// Initialize if needed (first call or uninitialized)
	if (pRandomFast->uValue == 0) {
#ifdef MOCK_DATE_TIME
		// Deterministic seed matching Ruffle's test/deterministic mode:
		// Ruffle's avm_rng seeds from get_current_date_time().timestamp_micros()
		// as u32. MOCK_DATE_TIME is in milliseconds, so multiply by 1000.
		RandomFastInit(pRandomFast, (uint32_t)((int64_t)(MOCK_DATE_TIME) * 1000LL));
#else
		RandomFastInit(pRandomFast, (uint32_t)time(NULL));
#endif
	}

	int32_t aNum = RandomFastNext(pRandomFast);
	aNum = RandomPureHasher(aNum * 71L);
	return aNum & kRandomPureMax;
}

// AS2 random(max) function - returns integer in range [0, max)
static int32_t Random(int32_t range, TRandomFast *pRandomFast) {
	if (range <= 0) {
		return 0;
	}

	int32_t randomNumber = GenerateRandomNumber(pRandomFast);
	return randomNumber % range;
}

// --- Math.random (defined here after TRandomFast/RNG are available) ---

static ActionVar builtin_math_random(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	coerceMathArgs(app_context, args, arg_count, 2);
	// Generate [0, 1) using the avmplus-compatible RNG
	int32_t raw = GenerateRandomNumber(&global_random_state);
	double result = (double)(raw & 0x7FFFFFFF) / 2147483648.0;
	return mathReturnDouble(result);
}

// ============================================================================
// isNaN / isFinite as first-class function objects
// ============================================================================

static ASFunction g_isNaN_func;
static ASFunction g_isFinite_func;
static int g_isNaN_isFinite_init = 0;

static ActionVar builtin_isNaN(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	ActionVar result = {0};
	result.type = ACTION_STACK_VALUE_BOOLEAN;
	if (arg_count == 0) { result.data.numeric_value = 1; return result; }
	double val = varToDoubleSWF(app_context, &args[0], g_swf_version);
	result.data.numeric_value = (val != val) ? 1ULL : 0ULL;
	return result;
}

static ActionVar builtin_isFinite(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	ActionVar result = {0};
	result.type = ACTION_STACK_VALUE_BOOLEAN;
	if (arg_count == 0) { result.data.numeric_value = 0; return result; }
	double val = varToDoubleSWF(app_context, &args[0], g_swf_version);
	int is_finite = (val == val && val != INFINITY && val != -INFINITY);
	result.data.numeric_value = is_finite ? 1ULL : 0ULL;
	return result;
}

static void init_isNaN_isFinite(void)
{
	if (g_isNaN_isFinite_init) return;
	memset(&g_isNaN_func, 0, sizeof(ASFunction));
	strncpy(g_isNaN_func.name, "isNaN", 255);
	g_isNaN_func.function_type = 2;
	g_isNaN_func.advanced_func = (Function2Ptr)builtin_isNaN;

	memset(&g_isFinite_func, 0, sizeof(ASFunction));
	strncpy(g_isFinite_func.name, "isFinite", 255);
	g_isFinite_func.function_type = 2;
	g_isFinite_func.advanced_func = (Function2Ptr)builtin_isFinite;

	g_isNaN_isFinite_init = 1;
}

// ============================================================================
// Math object initialization
// ============================================================================

void initMathObject(SWFAppContext* app_context)
{
	if (g_math_init_done) return;

	g_math_object = allocObject(app_context, 32);
	retainObject(g_math_object);

	// Register constants (explicit values; M_PI etc. not available with -std=c17)
	ActionVar cv = {0};
	cv.type = ACTION_STACK_VALUE_F64;

	VAL(double, &cv.data.numeric_value) = 3.14159265358979323846;
	setProperty(app_context, g_math_object, "PI", 2, &cv);
	VAL(double, &cv.data.numeric_value) = 2.71828182845904523536;
	setProperty(app_context, g_math_object, "E", 1, &cv);
	VAL(double, &cv.data.numeric_value) = 0.69314718055994530942;
	setProperty(app_context, g_math_object, "LN2", 3, &cv);
	VAL(double, &cv.data.numeric_value) = 2.30258509299404568402;
	setProperty(app_context, g_math_object, "LN10", 4, &cv);
	VAL(double, &cv.data.numeric_value) = 1.44269504088896340736;
	setProperty(app_context, g_math_object, "LOG2E", 5, &cv);
	VAL(double, &cv.data.numeric_value) = 0.43429448190325182765;
	setProperty(app_context, g_math_object, "LOG10E", 6, &cv);
	VAL(double, &cv.data.numeric_value) = 1.41421356237309504880;
	setProperty(app_context, g_math_object, "SQRT2", 5, &cv);
	VAL(double, &cv.data.numeric_value) = 0.70710678118654752440;
	setProperty(app_context, g_math_object, "SQRT1_2", 7, &cv);

	// Register methods
	struct { const char* name; u32 name_len; Function2Ptr func; } math_methods[] = {
		{"abs",    3, (Function2Ptr)builtin_math_abs},
		{"sin",    3, (Function2Ptr)builtin_math_sin},
		{"cos",    3, (Function2Ptr)builtin_math_cos},
		{"tan",    3, (Function2Ptr)builtin_math_tan},
		{"exp",    3, (Function2Ptr)builtin_math_exp},
		{"log",    3, (Function2Ptr)builtin_math_log},
		{"sqrt",   4, (Function2Ptr)builtin_math_sqrt},
		{"round",  5, (Function2Ptr)builtin_math_round},
		{"floor",  5, (Function2Ptr)builtin_math_floor},
		{"ceil",   4, (Function2Ptr)builtin_math_ceil},
		{"atan",   4, (Function2Ptr)builtin_math_atan},
		{"asin",   4, (Function2Ptr)builtin_math_asin},
		{"acos",   4, (Function2Ptr)builtin_math_acos},
		{"atan2",  5, (Function2Ptr)builtin_math_atan2},
		{"pow",    3, (Function2Ptr)builtin_math_pow},
		{"min",    3, (Function2Ptr)builtin_math_min},
		{"max",    3, (Function2Ptr)builtin_math_max},
		{"random", 6, (Function2Ptr)builtin_math_random},
	};

	for (int i = 0; i < 18; i++)
	{
		memset(&g_math_funcs[i], 0, sizeof(ASFunction));
		strncpy(g_math_funcs[i].name, math_methods[i].name, 255);
		g_math_funcs[i].function_type = 2;
		g_math_funcs[i].param_count = 0;
		g_math_funcs[i].advanced_func = math_methods[i].func;
		setupNativeFuncOwnProps(app_context, &g_math_funcs[i]);

		ActionVar fv = {0};
		fv.type = ACTION_STACK_VALUE_FUNCTION;
		VAL(u64, &fv.data.numeric_value) = (u64)&g_math_funcs[i];
		setProperty(app_context, g_math_object, math_methods[i].name, math_methods[i].name_len, &fv);
	}

	// Math is a native static object — always returns "[object Object]" regardless of SWF version
	installNativeToString(app_context, g_math_object);

	// Math properties (constants + methods) are DONT_ENUM in Flash/Ruffle.
	// setProperty defaults to ENUMERABLE, so clear the flag after init.
	// Gnash Math-v6/v7/v8 tests expect `for (m in Math) count++; count == 0`.
	for (u32 i = 0; i < g_math_object->num_used; i++)
		g_math_object->properties[i].flags &= ~PROPERTY_FLAG_ENUMERABLE;

	g_math_init_done = 1;
}

// ============================================================================
// Public accessors
// ============================================================================

ASObject* actionMathGetObject(SWFAppContext* app_context)
{
	initMathObject(app_context);
	return g_math_object;
}

ASFunction* actionMathGetFunc(int index)
{
	if (index < 0 || index >= 18) return NULL;
	return &g_math_funcs[index];
}

ASFunction* actionMathGetNaNStub(void)
{
	static ASFunction g_nan_stub;
	static int g_nan_stub_init = 0;
	if (!g_nan_stub_init) {
		memset(&g_nan_stub, 0, sizeof(ASFunction));
		strncpy(g_nan_stub.name, "stub", 255);
		g_nan_stub.function_type = 2;
		g_nan_stub.advanced_func = (Function2Ptr)builtin_math_nan_stub;
		g_nan_stub_init = 1;
	}
	return &g_nan_stub;
}

ASFunction* actionMathGetIsNaN(void)
{
	init_isNaN_isFinite();
	return &g_isNaN_func;
}

ASFunction* actionMathGetIsFinite(void)
{
	init_isNaN_isFinite();
	return &g_isFinite_func;
}

int32_t actionRNGGenerateRange(int32_t range)
{
	return Random(range, &global_random_state);
}

// Stage 3 collector root marker: the Math singleton is read through this
// C static even if script deletes _global.Math.
void mathGcMarkRoots(void)
{
	swfGcMarkObject(g_math_object);
}
