// Alchemy / CrossBridge "mops" — the indirect memory-access opcodes
// li8/li16/li32/lf32/lf64 (0x35-0x39), si8/si16/si32/sf32/sf64
// (0x3a-0x3e) and sxi1/sxi8/sxi16 (0x50-0x52), plus the
// flash.system.ApplicationDomain surface they read through
// (domainMemory, MIN_DOMAIN_MEMORY_LENGTH).
//
// Memory model: domain memory IS the assigned ByteArray's buffer -- no
// copy, no separate allocation. The context holds the ByteArray *object*
// and every access re-fetches ext->bytes/ext->len through it. That is
// what makes ByteArray interop free (mops/li8 writes with
// ByteArray.writeInt and reads back with li8 on the same bytes), and it
// is why the bytes pointer must NEVER be cached across calls:
// ByteArray.length reallocs the buffer, and mops_basics grows
// domainMemory from 1024 to 16384 mid-test. Fetch-per-access is the
// correctness baseline; a cached base+len invalidated from set_length is
// a separate, perf-motivated change.
//
// With no ByteArray assigned, accesses land on a lazily allocated,
// zero-filled scratch buffer of exactly MIN_DOMAIN_MEMORY_LENGTH (1024)
// bytes. The scratch is persistent, not a view of anything: mops_basics
// writes a pattern into the min-sized ByteArray, assigns null, and
// requires the scratch to still read back as *different* memory (and
// avm2/domain_memory writes 42 to the scratch, swaps a ByteArray in and
// out, and reads 42 back). The domainMemory getter answers null the
// whole time the scratch is in use -- the scratch is not scriptable.
//
// Byte order is always little-endian regardless of ByteArray.endian: the
// avmplus tests say "memory opcodes use native endianness" and set the
// ByteArray to LITTLE_ENDIAN only so writeInt/readInt agree with the
// opcodes. We assume a little-endian host (x86-64, aarch64, wasm) and
// memcpy raw; a big-endian port would need byte swaps here.

#include <string.h>

#include <avm2/avm2_abc.h>
#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>
#include <avm2/avm2_value.h>
#include <memory/heap.h>

#define AVM2_MIN_DOMAIN_MEMORY_LENGTH 1024u

static _Noreturn void throw_1506(Avm2Context* ctx)
{
	avm2_throw_error(ctx, ctx->builtins.range_error_class,
	                 "Error #1506: The specified range is invalid.");
}

// Current addressable window. Re-read on every access -- see the header
// comment; caching this is a use-after-free waiting for the first
// `domainMemory.length = N`.
static void mops_window(Avm2Context* ctx, uint8_t** out_bytes, uint32_t* out_len)
{
	if (ctx->domain_memory != NULL)
	{
		Avm2ByteArrayExt* ba =
			avm2_bytearray_ext_of(avm2_object_value(ctx->domain_memory));
		if (ba != NULL)
		{
			*out_bytes = ba->bytes;
			*out_len = ba->len;
			return;
		}
	}
	if (ctx->domain_scratch == NULL)
	{
		ctx->domain_scratch = heap_calloc(ctx->app,
		                                  AVM2_MIN_DOMAIN_MEMORY_LENGTH, 1);
	}
	*out_bytes = ctx->domain_scratch;
	*out_len = AVM2_MIN_DOMAIN_MEMORY_LENGTH;
}

// Resolve addr to a pointer, or throw RangeError 1506. Valid addresses
// are 0 <= addr <= len - size; mops_basics pins that boundary exactly by
// counting the failures over [-10000, len + 10000).
static uint8_t* mops_addr(Avm2Activation* act, Avm2Value addr_val, uint32_t size)
{
	Avm2Context* ctx = act->ctx;
	int32_t addr = avm2_coerce_to_i32(ctx, addr_val);
	uint8_t* base;
	uint32_t len;
	mops_window(ctx, &base, &len);
	// int64 sum: addr + size must not wrap for addresses near UINT32_MAX.
	if (addr < 0 || (int64_t) (uint32_t) addr + (int64_t) size > (int64_t) len)
	{
		throw_1506(ctx);
	}
	return base + (uint32_t) addr;
}

// --- loads. li8/li16 zero-extend, li32 is a signed int32, lf32 widens
// float to double (avm2/domain_memory pins lf32 == 1234.765380859375 for
// the float nearest 1234.7654321). ---

Avm2Value avm2_op_li8(Avm2Activation* act, Avm2Value addr)
{
	const uint8_t* p = mops_addr(act, addr, 1);
	return avm2_integer((int32_t) *p);
}

Avm2Value avm2_op_li16(Avm2Activation* act, Avm2Value addr)
{
	const uint8_t* p = mops_addr(act, addr, 2);
	uint16_t v;
	memcpy(&v, p, 2);
	return avm2_integer((int32_t) v);
}

Avm2Value avm2_op_li32(Avm2Activation* act, Avm2Value addr)
{
	const uint8_t* p = mops_addr(act, addr, 4);
	int32_t v;
	memcpy(&v, p, 4);
	return avm2_integer(v);
}

Avm2Value avm2_op_lf32(Avm2Activation* act, Avm2Value addr)
{
	const uint8_t* p = mops_addr(act, addr, 4);
	float f;
	memcpy(&f, p, 4);
	return avm2_number((double) f);
}

Avm2Value avm2_op_lf64(Avm2Activation* act, Avm2Value addr)
{
	const uint8_t* p = mops_addr(act, addr, 8);
	double d;
	memcpy(&d, p, 8);
	return avm2_number(d);
}

// --- stores. The integer forms truncate the coerced i32 (si8 keeps the
// low byte: domain_memory stores 0xFFFFFFFF via si8 and reads 255 back);
// sf32 narrows the coerced double to float. The value operand is coerced
// before the address is range-checked, matching the operand push order
// (value first, address on top). ---

void avm2_op_si8(Avm2Activation* act, Avm2Value value, Avm2Value addr)
{
	int32_t v = avm2_coerce_to_i32(act->ctx, value);
	uint8_t* p = mops_addr(act, addr, 1);
	*p = (uint8_t) v;
}

void avm2_op_si16(Avm2Activation* act, Avm2Value value, Avm2Value addr)
{
	int32_t v = avm2_coerce_to_i32(act->ctx, value);
	uint8_t* p = mops_addr(act, addr, 2);
	uint16_t w = (uint16_t) v;
	memcpy(p, &w, 2);
}

void avm2_op_si32(Avm2Activation* act, Avm2Value value, Avm2Value addr)
{
	int32_t v = avm2_coerce_to_i32(act->ctx, value);
	uint8_t* p = mops_addr(act, addr, 4);
	memcpy(p, &v, 4);
}

void avm2_op_sf32(Avm2Activation* act, Avm2Value value, Avm2Value addr)
{
	double d = avm2_coerce_to_number(act->ctx, value);
	uint8_t* p = mops_addr(act, addr, 4);
	float f = (float) d;
	memcpy(p, &f, 4);
}

void avm2_op_sf64(Avm2Activation* act, Avm2Value value, Avm2Value addr)
{
	double d = avm2_coerce_to_number(act->ctx, value);
	uint8_t* p = mops_addr(act, addr, 8);
	memcpy(p, &d, 8);
}

// --- sign extension. Pure integer ops, no memory access. ---

static int32_t sign_extend(int32_t v, unsigned bits)
{
	unsigned shift = 32u - bits;
	return (int32_t) ((uint32_t) v << shift) >> shift;
}

Avm2Value avm2_op_sxi1(Avm2Activation* act, Avm2Value v)
{
	return avm2_integer(sign_extend(avm2_coerce_to_i32(act->ctx, v), 1));
}

Avm2Value avm2_op_sxi8(Avm2Activation* act, Avm2Value v)
{
	return avm2_integer(sign_extend(avm2_coerce_to_i32(act->ctx, v), 8));
}

Avm2Value avm2_op_sxi16(Avm2Activation* act, Avm2Value v)
{
	return avm2_integer(sign_extend(avm2_coerce_to_i32(act->ctx, v), 16));
}

// --- flash.system.ApplicationDomain.domainMemory ---

static Avm2Value appdomain_get_domain_memory(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	return ctx->domain_memory != NULL ? avm2_object_value(ctx->domain_memory)
	                                  : avm2_null();
}

static Avm2Value appdomain_set_domain_memory(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value v = act->argc > 0 ? act->args[0] : avm2_undefined();
	// The declared parameter type is ByteArray, so an Array / Object /
	// Function / user instance is a #1034 coercion failure (mops_basics
	// requires each of those four to throw), and null coerces to null.
	if (ctx->builtins.bytearray_class != NULL)
	{
		v = avm2_coerce_to_class(ctx, ctx->builtins.bytearray_class, v);
	}
	if (v.kind == AVM2_VALUE_NULL || v.kind == AVM2_VALUE_UNDEFINED)
	{
		ctx->domain_memory = NULL;   // back to the scratch buffer
		return avm2_undefined();
	}
	Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(v);
	if (ba == NULL || ba->len < AVM2_MIN_DOMAIN_MEMORY_LENGTH)
	{
		// avmplus throws kEndOfFileError for a too-short buffer, which is
		// a plain Error (not EOFError) -- avm2/domain_memory traces
		// "Error: Error #1504: End of file." for a 1023-byte ByteArray.
		avm2_throw_error(ctx, ctx->builtins.error_class,
		                 "Error #1504: End of file.");
	}
	ctx->domain_memory = v.u.obj;
	return avm2_undefined();
}

void avm2_mops_register(Avm2Context* ctx, Avm2Class* appdomain_cls)
{
	avm2_builtin_add_getset(ctx, appdomain_cls, "domainMemory",
	                        appdomain_get_domain_memory,
	                        appdomain_set_domain_memory);
	avm2_builtin_add_static_const(ctx, appdomain_cls,
	                              "MIN_DOMAIN_MEMORY_LENGTH",
	                              avm2_integer(AVM2_MIN_DOMAIN_MEMORY_LENGTH));
}

// avm2.intrinsics.memory::casi32(address:int, expectedValue:int,
// newValue:int):int and ::mfence():void -- the two package-level natives
// of the intrinsics package that are NOT compiled to opcodes (li32 & co.
// are). Registered as globals in avm2_globals.c. Semantics per Ruffle
// core/src/avm2/globals/concurrent.rs and avm2/casi32: the address is
// taken as unsigned (negatives land far out of range), must satisfy
// addr <= len - 4 AND addr % 4 == 0, else RangeError #1506; the old value
// is returned and newValue is stored only when old == expectedValue. We
// have no Workers, so "atomic" is trivially satisfied.
Avm2Value avm2_intrinsic_casi32(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value a0 = act->argc > 0 ? act->args[0] : avm2_undefined();
	Avm2Value a1 = act->argc > 1 ? act->args[1] : avm2_undefined();
	Avm2Value a2 = act->argc > 2 ? act->args[2] : avm2_undefined();
	uint32_t addr = (uint32_t) avm2_coerce_to_i32(ctx, a0);
	int32_t expected = avm2_coerce_to_i32(ctx, a1);
	int32_t update = avm2_coerce_to_i32(ctx, a2);
	uint8_t* base;
	uint32_t len;
	mops_window(ctx, &base, &len);
	if (len < 4 || addr > len - 4 || (addr & 3u) != 0)
	{
		throw_1506(ctx);
	}
	int32_t old;
	memcpy(&old, base + addr, 4);
	if (old == expected)
	{
		memcpy(base + addr, &update, 4);
	}
	return avm2_integer(old);
}

Avm2Value avm2_intrinsic_mfence(Avm2Activation* act)
{
	(void) act;
	return avm2_undefined();
}
