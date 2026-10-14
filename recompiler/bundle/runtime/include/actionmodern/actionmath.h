#pragma once

// actionmath.h — public API for the Math subsystem.
//
// Named "actionmath" (not "math") so it does not shadow the C standard
// library's <math.h>; include/actionmodern/ is on the compiler include
// path.
//
// The Math object and its builtins were carved out of action.c per
// SWFRecompDocs/plans/split-action-c-math-first.md. All Math state lives
// in math.c; this header exposes only the accessors action.c (and future
// subsystems) need.

#include <actionmodern/action_internal.h>  // ASFunction

// Ensure Math object + function table are initialized (idempotent).
// Accessors below call this internally; call directly only if you need
// to force construction before the first accessor use.
void initMathObject(SWFAppContext* app_context);

// Return the singleton Math ASObject (initializes if needed).
ASObject* actionMathGetObject(SWFAppContext* app_context);

// Return the ASFunction for Math method index (0..17), or NULL if out
// of range. Indices correspond to ASnative(200, N) order:
//   0 abs   1 sin   2 cos   3 tan   4 exp   5 log
//   6 sqrt  7 round 8 floor 9 ceil  10 atan 11 asin
//   12 acos 13 atan2 14 pow 15 min  16 max  17 random
ASFunction* actionMathGetFunc(int index);

// NaN stub used for out-of-range ASnative(200, N). Lazily initialized.
// Does not register itself in action.c's function_registry — the caller
// should register on first use.
ASFunction* actionMathGetNaNStub(void);

// isNaN / isFinite as first-class function objects (accessible via
// GetVariable("isNaN") etc., not just inline call handlers).
ASFunction* actionMathGetIsNaN(void);
ASFunction* actionMathGetIsFinite(void);

// AS2 random(n) top-level builtin: returns an integer in [0, range)
// using the avmplus-compatible RNG. Returns 0 if range <= 0.
int32_t actionRNGGenerateRange(int32_t range);
