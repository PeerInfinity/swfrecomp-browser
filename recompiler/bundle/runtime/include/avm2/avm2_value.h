#ifndef AVM2_VALUE_H
#define AVM2_VALUE_H

// Avm2Value — the AVM2 tagged value type (avm2-support-plan §4.2).
//
// 16 bytes, mirroring Ruffle's core/src/avm2/value.rs Value enum (Ruffle
// asserts the same size). Deliberately a DIFFERENT type from AVM1's
// ActionVar: the two VMs never share values except through interop shims
// (much later). Kept in its own small header so a late reshape after
// upstream (LittleCube) feedback stays cheap.
//
// Number/Integer duality (avmplus): i32-representable numbers canonicalize
// to AVM2_VALUE_INTEGER where the source pushes them that way (PushInt);
// the distinction is invisible at the AS3 level (both are "Number"), so
// arithmetic may produce either. Coercion helpers are named 1:1 with
// Ruffle's value.rs methods (coerce_to_string, ...).

#include <stdbool.h>
#include <stdint.h>

typedef struct Avm2Object Avm2Object;
typedef struct Avm2Context Avm2Context;

// Interned or heap string: UTF-8 bytes + explicit length (ABC strings may
// embed NULs). Static pool entries are emitted by the recompiler; runtime
// strings are heap-allocated. `utf8` is also NUL-terminated for printf
// convenience wherever the runtime creates one; pool entries emitted by
// the recompiler are NUL-terminated too (string literals).
//
// GC (avm2_gc.c string census): heap strings created by avm2_string_new /
// avm2_string_concat enroll in an intrusive census and are swept when
// unreachable. The GC fields are APPENDED after the {len, utf8} prefix so
// the recompiler's positional pool initializers `{ len, "..." }` zero them:
// gc_flags == 0 means "not heap-enrolled" (static pool / rodata — immortal,
// and the collector must never write to it).
typedef struct Avm2String
{
	uint32_t len;
	const char* utf8;
	struct Avm2String* gc_next;  // census link (heap strings only)
	uint32_t gc_flags;           // AVM2_STR_GC_* bits; 0 for static pool
} Avm2String;

#define AVM2_STR_GC_HEAP 1u  // heap-allocated + census-enrolled
#define AVM2_STR_GC_MARK 2u  // reachable this cycle (cleared each collect)

typedef enum Avm2ValueKind
{
	AVM2_VALUE_UNDEFINED = 0,
	AVM2_VALUE_NULL = 1,
	AVM2_VALUE_BOOL = 2,
	AVM2_VALUE_INTEGER = 3,
	AVM2_VALUE_NUMBER = 4,
	AVM2_VALUE_STRING = 5,
	AVM2_VALUE_OBJECT = 6,
	// Internal-only: an array-storage hole. Never appears on the operand
	// stack, in locals, or in any user-visible value.
	AVM2_VALUE_HOLE = 7,
} Avm2ValueKind;

typedef struct Avm2Value
{
	uint32_t kind;   // Avm2ValueKind
	uint32_t _pad;
	union
	{
		bool b;
		int32_t i;
		double d;
		const Avm2String* str;
		Avm2Object* obj;
	} u;
} Avm2Value;

// Constructors
Avm2Value avm2_undefined(void);
Avm2Value avm2_null(void);
Avm2Value avm2_bool(bool b);
Avm2Value avm2_integer(int32_t i);
Avm2Value avm2_number(double d);
Avm2Value avm2_uint_value(uint32_t u);  // Integer if it fits, else Number
Avm2Value avm2_string(const Avm2String* s);
Avm2Value avm2_object_value(Avm2Object* obj);

// Coercions (port of Ruffle value.rs semantics). coerce_to_string returns
// a heap or interned Avm2String, never NULL. Object arms call toString/
// valueOf via the public-property machinery and may throw AVM2 errors.
const Avm2String* avm2_coerce_to_string(Avm2Context* ctx, Avm2Value v);
bool avm2_coerce_to_boolean(Avm2Value v);
double avm2_coerce_to_number(Avm2Context* ctx, Avm2Value v);
int32_t avm2_coerce_to_i32(Avm2Context* ctx, Avm2Value v);
uint32_t avm2_coerce_to_u32(Avm2Context* ctx, Avm2Value v);
// ECMA ToPrimitive. hint: 0 = none/default, 1 = number, 2 = string.
Avm2Value avm2_coerce_to_primitive(Avm2Context* ctx, Avm2Value v, int hint);

// ECMA f64 -> i32/u32 wrapping conversion (Ruffle ecma_conversions).
int32_t avm2_f64_to_wrapping_i32(double d);
uint32_t avm2_f64_to_wrapping_u32(double d);

// String parsing (Ruffle value.rs string_to_int / string_to_f64).
double avm2_string_to_int(const char* s, uint32_t len, int32_t radix, bool strict);
// Returns false if unparseable (out_d untouched). `ctx` may be NULL; it is
// read only for ctx->swf_version (the SWF <= 10 double-dot bug compatibility,
// Ruffle value.rs / bugzilla 513018).
bool avm2_string_to_f64(Avm2Context* ctx, const char* s, uint32_t len, bool strict,
                        double* out_d);

// AVM2 Number.toString() formatting (Ruffle value.rs coerce_to_string
// Number arm: shortest round-trip decimal inside [1e-6, 1e21), otherwise
// MAX_PRECISION exponential). Returns length written to buf.
int avm2_format_number(char* buf, int buf_size, double d);

// Operators (Ruffle activation.rs op impls / value.rs abstract algorithms).
Avm2Value avm2_op_add_values(Avm2Context* ctx, Avm2Value a, Avm2Value b);
bool avm2_abstract_eq(Avm2Context* ctx, Avm2Value a, Avm2Value b);
bool avm2_strict_eq(Avm2Value a, Avm2Value b);
// abstract_lt: returns -1 for undefined (NaN), else 0/1.
int avm2_abstract_lt(Avm2Context* ctx, Avm2Value a, Avm2Value b);

// Number checks (Ruffle value.rs is_number / is_u32 / is_i32).
bool avm2_value_is_number(Avm2Value v);
bool avm2_value_is_u32(Avm2Value v);
bool avm2_value_is_i32(Avm2Value v);
// Index-like check: value is an unsigned integer < 0xFFFFFFFF. Returns
// false or sets *out_idx.
bool avm2_value_as_index(Avm2Value v, uint32_t* out_idx);

// Heap string helpers (copy bytes, NUL-terminate).
const Avm2String* avm2_string_new(Avm2Context* ctx, const char* utf8, uint32_t len);
const Avm2String* avm2_string_from_literal(Avm2Context* ctx, const char* lit);
const Avm2String* avm2_string_concat(Avm2Context* ctx, const Avm2String* a, const Avm2String* b);
bool avm2_string_equals(const Avm2String* a, const Avm2String* b);
// UTF-16 code-unit-order comparison (-1/0/1).
int avm2_string_utf16_cmp(const Avm2String* a, const Avm2String* b);

#endif // AVM2_VALUE_H
