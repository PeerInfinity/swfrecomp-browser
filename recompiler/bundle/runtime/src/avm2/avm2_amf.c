// AMF3/AMF0 serialization — ByteArray.readObject/writeObject,
// flash.net.registerClassAlias/getClassByAlias/ObjectEncoding.
//
// (The Date class used to be bolted on here as a three-method stub, purely so
// AMF round-trips could carry a timestamp; it now lives in avm2_date.c. This
// file still reaches into Avm2DateExt via avm2_date_ext_of() to read the
// timestamp off the wire.)
//
// The wire format is a byte-exact port of the flash-lso crate
// (amf3/write.rs + length.rs + element_cache.rs, amf0/write.rs, read.rs)
// as DRIVEN BY Ruffle's bridge (core/src/avm2/amf.rs), quirks included:
//
// - EXCEPTION (deliberate divergence from flash-lso/Ruffle, see
//   w3_ref_or_store): every repeated complex value collapses to an AMF3
//   object reference, as Flash does. flash-lso hardcodes Length::Size for
//   objects/arrays and Ruffle's get_or_create_value only registers an object
//   AFTER serializing it, so a self-referential graph recurses forever there
//   (Ruffle even stubs "with same Object used multiple times"). Flash writes
//   references, and as3/AMF/AMFSerializer's "Objects with Circular Reference"
//   round-trip depends on it.
// - Static (trait) properties are the class's public slots and full
//   get/set accessor pairs, SORTED by name (Ruffle sorts for stable
//   output); dynamic props follow in enumeration order. Function-valued
//   entries are skipped in the dynamic part only.
// - AMF0 writes class_def-less objects (marker 0x03) even for aliased
//   classes, and Integers widen to Numbers; vectors/dictionaries/
//   bytearrays become 0x0D Unsupported (flash-lso fallback arm).
// - Values normalize before dispatch (Ruffle Value::normalize): Numbers
//   with exact-i32 bit patterns inside ±2^28 become Integers and vice
//   versa.
//
// Deserialization builds AVM2 values directly, inserting containers into
// the reference table before their children (circular refs). Property
// sets on typed objects that throw are caught, stringified, and TRACED
// (Ruffle read_object error handling — amf_setter_error); errors thrown
// by the stringification itself propagate.

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_e4x.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_gc.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <memory/heap.h>
#include <avm2/avm2_ops.h>

// ---------------------------------------------------------------------------
// Class alias registry (flash.net.registerClassAlias)
// ---------------------------------------------------------------------------

typedef struct AliasEntry
{
	struct AliasEntry* next;
	const Avm2String* alias;
	Avm2Class* cls;
} AliasEntry;

static AliasEntry* g_aliases;

static Avm2Class* alias_to_class(const char* name, uint32_t len)
{
	for (AliasEntry* a = g_aliases; a != NULL; a = a->next)
	{
		if (a->alias->len == len && memcmp(a->alias->utf8, name, len) == 0)
		{
			return a->cls;
		}
	}
	return NULL;
}

static const Avm2String* class_to_alias(Avm2Context* ctx, Avm2Class* cls)
{
	for (AliasEntry* a = g_aliases; a != NULL; a = a->next)
	{
		if (a->cls == cls) return a->alias;
	}
	return avm2_string_from_literal(ctx, "");
}

// ---------------------------------------------------------------------------
// flash.utils.IExternalizable + ObjectEncoding.dynamicPropertyWriter
// ---------------------------------------------------------------------------

static Avm2Class* g_externalizable_iface;   // flash.utils.IExternalizable
static Avm2Class* g_dyn_prop_output_class;  // the IDynamicPropertyOutput impl
static Avm2Value g_dyn_prop_writer;         // ObjectEncoding.dynamicPropertyWriter

static int value_is_externalizable(Avm2Context* ctx, Avm2Value v)
{
	if (v.kind != AVM2_VALUE_OBJECT || v.u.obj == NULL) return 0;
	if (g_externalizable_iface == NULL || v.u.obj->cls == NULL) return 0;
	return avm2_class_has_interface(ctx, v.u.obj->cls, g_externalizable_iface)
	       ? 1 : 0;
}

// A fresh AMF3 ByteArray seeded with `n` bytes — the IDataOutput handed to
// writeExternal (empty) and the IDataInput handed to readExternal (the rest of
// the stream; its final `position` is how many bytes the body consumed).
static Avm2Value ext_new_bytearray(Avm2Context* ctx, const uint8_t* src, uint32_t n)
{
	Avm2Value v = avm2_class_construct(ctx, ctx->builtins.bytearray_class, NULL, 0);
	Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(v);
	if (ba != NULL)
	{
		ba->object_encoding = 3;
		if (n > 0)
		{
			avm2_bytearray_set_length_public(ctx, ba, n);
			memcpy(ba->bytes, src, n);
		}
		ba->position = 0;
	}
	return v;
}

// ---------------------------------------------------------------------------
// Growable output buffer
// ---------------------------------------------------------------------------

typedef struct AmfBuf
{
	Avm2Context* ctx;
	uint8_t* b;
	uint32_t len;
	uint32_t cap;
} AmfBuf;

static void buf_put(AmfBuf* w, const void* p, uint32_t n)
{
	if (w->len + n > w->cap)
	{
		uint32_t new_cap = w->cap == 0 ? 64 : w->cap;
		while (new_cap < w->len + n) new_cap *= 2;
		uint8_t* grown = avm2_alloc(w->ctx, new_cap);
		if (w->len > 0) memcpy(grown, w->b, w->len);
		if (w->b != NULL) heap_free(w->ctx->app, w->b);
		w->b = grown;
		w->cap = new_cap;
	}
	memcpy(w->b + w->len, p, n);
	w->len += n;
}

static void buf_u8(AmfBuf* w, uint8_t v) { buf_put(w, &v, 1); }

static void buf_u16be(AmfBuf* w, uint16_t v)
{
	uint8_t b[2] = { (uint8_t) (v >> 8), (uint8_t) v };
	buf_put(w, b, 2);
}

static void buf_u32be(AmfBuf* w, uint32_t v)
{
	uint8_t b[4] = { (uint8_t) (v >> 24), (uint8_t) (v >> 16),
	                 (uint8_t) (v >> 8), (uint8_t) v };
	buf_put(w, b, 4);
}

static void buf_f64be(AmfBuf* w, double d)
{
	uint64_t bits;
	memcpy(&bits, &d, 8);
	uint8_t b[8];
	for (int i = 0; i < 8; i++) b[i] = (uint8_t) (bits >> (56 - i * 8));
	buf_put(w, b, 8);
}

// ---------------------------------------------------------------------------
// AMF3 writer
// ---------------------------------------------------------------------------

enum
{
	M3_UNDEF = 0x00, M3_NULL = 0x01, M3_FALSE = 0x02, M3_TRUE = 0x03,
	M3_INT = 0x04, M3_NUM = 0x05, M3_STR = 0x06, M3_XMLDOC = 0x07,
	M3_DATE = 0x08, M3_ARRAY = 0x09, M3_OBJECT = 0x0A, M3_XMLSTR = 0x0B,
	M3_BYTEARRAY = 0x0C, M3_VECINT = 0x0D, M3_VECUINT = 0x0E,
	M3_VECDOUBLE = 0x0F, M3_VECOBJ = 0x10, M3_DICT = 0x11,
};

typedef struct StrEntry
{
	const char* p;
	uint32_t n;
} StrEntry;

typedef struct TraitEntry
{
	const Avm2String* name;
	int dynamic;
	int externalizable;   // trait bit 0x04: the body is opaque IExternalizable data
	uint32_t static_count;
	const Avm2String** statics;
} TraitEntry;

// Object-table entry: identity of the source object (bridge-level Rc
// identity approximated by Avm2Object identity).
typedef struct ObjEntry
{
	Avm2Object* src;
} ObjEntry;

typedef struct Amf3Wr
{
	Avm2Context* ctx;
	AmfBuf out;
	StrEntry* strings;
	uint32_t str_count, str_cap;
	TraitEntry* traits;
	uint32_t trait_count, trait_cap;
	ObjEntry* objs;
	uint32_t obj_count, obj_cap;
	// AMF0 reference table (a separate index space from the AMF3 one).
	ObjEntry* objs0;
	uint32_t obj0_count, obj0_cap;
	// AMF0 only: is this a WIRE channel (NetConnection packet / LocalConnection
	// send) rather than a local serialization (ByteArray.writeObject)? Only the
	// wire promotes a dense array to a StrictArray — see w0_value's array arm.
	int wire_mode;
} Amf3Wr;

static void w3_u29(Amf3Wr* w, int32_t i)
{
	int32_t n = (i < 0) ? i + 0x20000000 : i;
	if (n > 0x1fffff)
	{
		buf_u8(&w->out, (uint8_t) ((n >> 22) | 0x80));
		buf_u8(&w->out, (uint8_t) ((n >> 15) | 0x80));
		buf_u8(&w->out, (uint8_t) ((n >> 8) | 0x80));
		buf_u8(&w->out, (uint8_t) (n & 0xFF));
	}
	else if (n > 0x3fff)
	{
		buf_u8(&w->out, (uint8_t) ((n >> 14) | 0x80));
		buf_u8(&w->out, (uint8_t) ((n >> 7) | 0x80));
		buf_u8(&w->out, (uint8_t) (n & 0x7F));
	}
	else if (n > 0x7f)
	{
		buf_u8(&w->out, (uint8_t) ((n >> 7) | 0x80));
		buf_u8(&w->out, (uint8_t) (n & 0x7F));
	}
	else
	{
		buf_u8(&w->out, (uint8_t) (n & 0x7F));
	}
}

// Byte string with the AMF3 string reference table (empty never cached).
static void w3_str(Amf3Wr* w, const char* p, uint32_t n)
{
	if (n > 0)
	{
		for (uint32_t i = 0; i < w->str_count; i++)
		{
			if (w->strings[i].n == n && memcmp(w->strings[i].p, p, n) == 0)
			{
				w3_u29(w, (int32_t) (i << 1));
				return;
			}
		}
		if (w->str_count == w->str_cap)
		{
			uint32_t nc = w->str_cap == 0 ? 16 : w->str_cap * 2;
			StrEntry* g = avm2_alloc(w->ctx, nc * sizeof(StrEntry));
			memcpy(g, w->strings, w->str_count * sizeof(StrEntry));
			if (w->strings != NULL) heap_free(w->ctx->app, (void*) w->strings);
			w->strings = g;
			w->str_cap = nc;
		}
		w->strings[w->str_count].p = p;
		w->strings[w->str_count].n = n;
		w->str_count++;
	}
	w3_u29(w, (int32_t) ((n << 1) | 1));
	buf_put(&w->out, p, n);
}

// Object reference table (identity). Returns index or -1.
static int32_t w3_obj_find(Amf3Wr* w, Avm2Object* src)
{
	for (uint32_t i = 0; i < w->obj_count; i++)
	{
		if (w->objs[i].src == src) return (int32_t) i;
	}
	return -1;
}

static void w3_obj_store(Amf3Wr* w, Avm2Object* src)
{
	if (w3_obj_find(w, src) >= 0) return;
	if (w->obj_count == w->obj_cap)
	{
		uint32_t nc = w->obj_cap == 0 ? 16 : w->obj_cap * 2;
		ObjEntry* g = avm2_alloc(w->ctx, nc * sizeof(ObjEntry));
		memcpy(g, w->objs, w->obj_count * sizeof(ObjEntry));
		if (w->objs != NULL) heap_free(w->ctx->app, (void*) w->objs);
		w->objs = g;
		w->obj_cap = nc;
	}
	w->objs[w->obj_count++].src = src;
}

// AMF3 object-reference gate, applied to EVERY complex value: a value already
// written in this packet is replaced by `marker` + u29 with the low bit clear
// (its table index), and a first sighting is registered BEFORE its children are
// written so self-referential graphs terminate.
//
// Returns 1 when a reference was emitted (nothing else to write).
//
// The set and order of what lands in this table is exactly what the reader's
// rd3_obj_reserve materialises, which is what keeps the two index spaces in
// step — a writer that stores fewer kinds than the reader reserves would emit
// references that resolve to the wrong object.
static int w3_ref_or_store(Amf3Wr* w, Avm2Object* obj, uint8_t marker)
{
	int32_t r = w3_obj_find(w, obj);
	if (r >= 0)
	{
		buf_u8(&w->out, marker);
		w3_u29(w, r << 1);
		return 1;
	}
	w3_obj_store(w, obj);
	return 0;
}

static void w3_value(Amf3Wr* w, Avm2Value v);
static void w0_value(Amf3Wr* w, Avm2Value v);

// Ruffle Value::normalize.
static Avm2Value amf_normalize(Avm2Value v)
{
	if (v.kind == AVM2_VALUE_NUMBER)
	{
		int32_t i = (int32_t) v.u.d;
		double rt = (double) i;
		uint64_t a, b;
		memcpy(&a, &v.u.d, 8);
		memcpy(&b, &rt, 8);
		if (a == b && i < (1 << 28) && i >= -(1 << 28))
		{
			return avm2_integer(i);
		}
	}
	else if (v.kind == AVM2_VALUE_INTEGER)
	{
		if (!(v.u.i < (1 << 28) && v.u.i >= -(1 << 28)))
		{
			return avm2_number((double) v.u.i);
		}
	}
	return v;
}

// The class's serializable "static" properties: public slots + full
// accessor pairs, sorted by name (UTF-16 order). Returns count.
static uint32_t collect_static_props(Avm2Context* ctx, Avm2Object* obj,
                                     const Avm2String*** out_names)
{
	const Avm2VTable* vt = obj->vtable;
	uint32_t cap = (vt != NULL) ? vt->count : 0;
	const Avm2String** names = avm2_alloc(ctx, (cap + 1) * sizeof(Avm2String*));
	uint32_t n = 0;
	for (uint32_t i = 0; vt != NULL && i < vt->count; i++)
	{
		const Avm2PropEntry* e = &vt->entries[i];
		if (!avm2_propkey_is_public(&e->key)) continue;
		if (e->kind == AVM2_PROP_METHOD) continue;
		if (e->kind == AVM2_PROP_GETTER || e->kind == AVM2_PROP_SETTER) continue;
		// duplicate keys (alias entries) — skip repeats by name
		int dup = 0;
		for (uint32_t j = 0; j < n; j++)
		{
			if (names[j]->len == e->key.name_len
			    && memcmp(names[j]->utf8, e->key.name, e->key.name_len) == 0)
			{
				dup = 1;
				break;
			}
		}
		if (dup) continue;
		names[n++] = avm2_string_new(ctx, e->key.name, e->key.name_len);
	}
	// Insertion sort by UTF-16 order.
	for (uint32_t i = 1; i < n; i++)
	{
		const Avm2String* key = names[i];
		uint32_t j = i;
		while (j > 0 && avm2_string_utf16_cmp(names[j - 1], key) > 0)
		{
			names[j] = names[j - 1];
			j--;
		}
		names[j] = key;
	}
	*out_names = names;
	return n;
}

static int value_is_function(Avm2Value v)
{
	return v.kind == AVM2_VALUE_OBJECT && v.u.obj != NULL
	       && v.u.obj->kind == AVM2_OBJ_FUNCTION;
}

// The writer whose stream an IDynamicPropertyOutput.writeDynamicProperty call
// appends to. Set only for the duration of a writeDynamicProperties callback,
// saved/restored so a nested dynamic object (whose own body re-enters the hook)
// cannot retarget an outer one.
static Amf3Wr* g_dpo_wr;

static Avm2Value dpo_write_dynamic_property(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Amf3Wr* w = g_dpo_wr;
	if (w == NULL) return avm2_undefined();
	const Avm2String* name = (act->argc > 0)
		? avm2_coerce_to_string(ctx, act->args[0])
		: avm2_string_from_literal(ctx, "");
	Avm2Value val = (act->argc > 1) ? act->args[1] : avm2_undefined();
	w3_str(w, name->utf8, name->len);
	w3_value(w, val);
	return avm2_undefined();
}

// Generic object body: static values (sorted) then, when dynamic, the
// name/value expando pairs and a terminator (flash-lso write_object_full
// tail shared with write_trait_reference).
static void w3_object_body(Amf3Wr* w, Avm2Object* obj, int dynamic,
                           const Avm2String** statics, uint32_t static_count)
{
	Avm2Context* ctx = w->ctx;
	Avm2Value recv = avm2_object_value(obj);
	for (uint32_t i = 0; i < static_count; i++)
	{
		Avm2Value pv = avm2_get_public_property(ctx, recv, statics[i]->utf8,
		                                        statics[i]->len, NULL);
		w3_value(w, pv);
	}
	if (dynamic)
	{
		// ObjectEncoding.dynamicPropertyWriter, when set, REPLACES the dynamic
		// half: the hook decides which expandos reach the stream (and under
		// which names), so no filtering of our own happens on this path — not
		// even the function-valued skip below. AMF3 only, which is the only
		// encoding ObjectEncoding's hook is documented against.
		if (g_dyn_prop_writer.kind == AVM2_VALUE_OBJECT
		    && g_dyn_prop_output_class != NULL)
		{
			Avm2Value out = avm2_class_construct(ctx, g_dyn_prop_output_class,
			                                     NULL, 0);
			Avm2Value args[2] = { recv, out };
			Amf3Wr* saved = g_dpo_wr;
			g_dpo_wr = w;
			avm2_call_public_property(ctx, g_dyn_prop_writer,
			                          "writeDynamicProperties", 22, args, 2);
			g_dpo_wr = saved;
		}
		else
		{
			uint32_t idx = avm2_object_next_enumerant(obj, 0);
			while (idx != 0)
			{
				Avm2Value name = avm2_object_enumerant_name(ctx, obj, idx);
				Avm2Value val = avm2_object_enumerant_value(ctx, obj, idx);
				if (!value_is_function(val))
				{
					const Avm2String* ns = avm2_coerce_to_string(ctx, name);
					w3_str(w, ns->utf8, ns->len);
					w3_value(w, val);
				}
				idx = avm2_object_next_enumerant(obj, idx);
			}
		}
		w3_str(w, "", 0);
	}
}

// An IExternalizable instance: trait bit 0x04, the class name, then an OPAQUE
// body that writeExternal(IDataOutput) produces. Neither Ruffle nor flash-lso
// writes this shape at all. Adobe's serializer shares the enclosing stream's
// string/object reference tables with the body; we hand writeExternal a FRESH
// ByteArray instead, which is self-consistent with the reader below and
// byte-identical for any body that does not reference the outer graph.
static void w3_object_external(Amf3Wr* w, Avm2Object* obj, const Avm2String* alias)
{
	Avm2Context* ctx = w->ctx;
	buf_u8(&w->out, M3_OBJECT);

	int32_t trait_idx = -1;
	for (uint32_t i = 0; i < w->trait_count; i++)
	{
		TraitEntry* t = &w->traits[i];
		if (!t->externalizable) continue;
		if (!avm2_string_equals(t->name, alias)) continue;
		trait_idx = (int32_t) i;
		break;
	}
	if (trait_idx >= 0)
	{
		w3_u29(w, (int32_t) ((((uint32_t) trait_idx << 1) << 1) | 1));
	}
	else
	{
		if (w->trait_count == w->trait_cap)
		{
			uint32_t nc = w->trait_cap == 0 ? 8 : w->trait_cap * 2;
			TraitEntry* g = avm2_alloc(ctx, nc * sizeof(TraitEntry));
			memcpy(g, w->traits, w->trait_count * sizeof(TraitEntry));
			if (w->traits != NULL) heap_free(ctx->app, w->traits);
			w->traits = g;
			w->trait_cap = nc;
		}
		TraitEntry* t = &w->traits[w->trait_count++];
		memset(t, 0, sizeof(*t));
		t->name = alias;
		t->externalizable = 1;
		// (0 statics << 4) | (externalizable << 2) | 3
		w3_u29(w, 0x07);
		w3_str(w, alias->utf8, alias->len);
	}

	Avm2Value out = ext_new_bytearray(ctx, NULL, 0);
	Avm2Value objv = avm2_object_value(obj);
	avm2_call_public_property(ctx, objv, "writeExternal", 13, &out, 1);
	Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(out);
	if (ba != NULL && ba->len > 0) buf_put(&w->out, ba->bytes, ba->len);
}

static void w3_object(Amf3Wr* w, Avm2Object* obj)
{
	Avm2Context* ctx = w->ctx;
	if (w3_ref_or_store(w, obj, M3_OBJECT)) return;

	const Avm2String* alias = class_to_alias(ctx, avm2_value_class(
		ctx, avm2_object_value(obj)));
	if (value_is_externalizable(ctx, avm2_object_value(obj)))
	{
		w3_object_external(w, obj, alias);
		return;
	}
	int dynamic = 1;
	Avm2Class* cls = obj->cls;
	if (cls != NULL && (cls->flags & AVM2_CLASS_FLAG_SEALED)) dynamic = 0;

	const Avm2String** statics;
	uint32_t static_count = collect_static_props(ctx, obj, &statics);

	// Trait table lookup: name + dynamic + static list.
	int32_t trait_idx = -1;
	for (uint32_t i = 0; i < w->trait_count; i++)
	{
		TraitEntry* t = &w->traits[i];
		if (t->externalizable) continue;
		if (t->dynamic != dynamic) continue;
		if (!avm2_string_equals(t->name, alias)) continue;
		if (t->static_count != static_count) continue;
		int same = 1;
		for (uint32_t j = 0; j < static_count; j++)
		{
			if (!avm2_string_equals(t->statics[j], statics[j]))
			{
				same = 0;
				break;
			}
		}
		if (!same) continue;
		trait_idx = (int32_t) i;
		break;
	}

	buf_u8(&w->out, M3_OBJECT);
	if (trait_idx >= 0)
	{
		w3_u29(w, (int32_t) ((((uint32_t) trait_idx << 1) << 1) | 1));
	}
	else
	{
		if (w->trait_count == w->trait_cap)
		{
			uint32_t nc = w->trait_cap == 0 ? 8 : w->trait_cap * 2;
			TraitEntry* g = avm2_alloc(ctx, nc * sizeof(TraitEntry));
			memcpy(g, w->traits, w->trait_count * sizeof(TraitEntry));
			if (w->traits != NULL) heap_free(ctx->app, w->traits);
			w->traits = g;
			w->trait_cap = nc;
		}
		TraitEntry* t = &w->traits[w->trait_count++];
		t->name = alias;
		t->dynamic = dynamic;
		t->externalizable = 0;
		t->static_count = static_count;
		t->statics = statics;

		uint32_t encoding = dynamic ? 0x2 : 0x0;
		uint32_t size = ((((static_count << 2) | encoding) << 1 | 1) << 1) | 1;
		w3_u29(w, (int32_t) size);
		w3_str(w, alias->utf8, alias->len);
		for (uint32_t i = 0; i < static_count; i++)
		{
			w3_str(w, statics[i]->utf8, statics[i]->len);
		}
	}
	w3_object_body(w, obj, dynamic, statics, static_count);
	// A new trait adopted `statics`; a matched one left it scratch.
	if (trait_idx >= 0 && statics != NULL) heap_free(ctx->app, (void*) statics);
}

static void w3_array(Amf3Wr* w, Avm2Object* arr)
{
	Avm2Context* ctx = w->ctx;
	if (w3_ref_or_store(w, arr, M3_ARRAY)) return;

	// Ruffle amf.rs: enumerate everything, splitting entries whose name
	// matches its running position into the dense part.
	uint32_t cap = 8;
	Avm2Value* dense = avm2_alloc(ctx, cap * sizeof(Avm2Value));
	uint32_t dense_n = 0;
	Avm2Value* sp_names = avm2_alloc(ctx, cap * sizeof(Avm2Value));
	Avm2Value* sp_vals = avm2_alloc(ctx, cap * sizeof(Avm2Value));
	uint32_t sp_n = 0;
	uint32_t pos = 0;
	uint32_t idx = avm2_object_next_enumerant(arr, 0);
	while (idx != 0)
	{
		Avm2Value name = avm2_object_enumerant_name(ctx, arr, idx);
		Avm2Value val = avm2_object_enumerant_value(ctx, arr, idx);
		if (dense_n >= cap || sp_n >= cap)
		{
			uint32_t nc = cap * 2;
			Avm2Value* d2 = avm2_alloc(ctx, nc * sizeof(Avm2Value));
			memcpy(d2, dense, dense_n * sizeof(Avm2Value));
			Avm2Value* n2 = avm2_alloc(ctx, nc * sizeof(Avm2Value));
			memcpy(n2, sp_names, sp_n * sizeof(Avm2Value));
			Avm2Value* v2 = avm2_alloc(ctx, nc * sizeof(Avm2Value));
			memcpy(v2, sp_vals, sp_n * sizeof(Avm2Value));
			heap_free(ctx->app, dense);
			heap_free(ctx->app, sp_names);
			heap_free(ctx->app, sp_vals);
			dense = d2;
			sp_names = n2;
			sp_vals = v2;
			cap = nc;
		}
		// Function-valued entries are skipped BEFORE the dense/sparse
		// position split (Ruffle recursive_serialize filters them out of
		// `values`, and the enumerate() index runs over the filtered list).
		if (value_is_function(val))
		{
			idx = avm2_object_next_enumerant(arr, idx);
			continue;
		}
		const Avm2String* ns = avm2_coerce_to_string(ctx, name);
		char posbuf[16];
		int pn = snprintf(posbuf, sizeof(posbuf), "%u", pos);
		if ((uint32_t) pn == ns->len && memcmp(posbuf, ns->utf8, pn) == 0)
		{
			dense[dense_n++] = val;
		}
		else
		{
			sp_names[sp_n] = avm2_string(ns);
			sp_vals[sp_n] = val;
			sp_n++;
		}
		pos++;
		idx = avm2_object_next_enumerant(arr, idx);
	}

	buf_u8(&w->out, M3_ARRAY);
	w3_u29(w, (int32_t) ((dense_n << 1) | 1));
	for (uint32_t i = 0; i < sp_n; i++)
	{
		const Avm2String* ns = sp_names[i].u.str;
		w3_str(w, ns->utf8, ns->len);
		w3_value(w, sp_vals[i]);
	}
	w3_str(w, "", 0);
	for (uint32_t i = 0; i < dense_n; i++)
	{
		w3_value(w, dense[i]);
	}
	// The split is scratch (the values were written out above).
	heap_free(ctx->app, dense);
	heap_free(ctx->app, sp_names);
	heap_free(ctx->app, sp_vals);
}

static void w3_vector(Amf3Wr* w, Avm2Object* vec)
{
	Avm2Context* ctx = w->ctx;
	Avm2Builtins* b = &ctx->builtins;
	Avm2VectorExt* ext = avm2_vector_ext(vec);
	Avm2Class* t = ext->value_type;
	if (t == b->int_class || t == b->uint_class || t == b->number_class)
	{
		uint8_t marker = (t == b->int_class) ? M3_VECINT
		                 : (t == b->uint_class) ? M3_VECUINT
		                                        : M3_VECDOUBLE;
		if (w3_ref_or_store(w, vec, marker)) return;
		buf_u8(&w->out, marker);
		w3_u29(w, (int32_t) ((ext->length << 1) | 1));
		buf_u8(&w->out, ext->fixed ? 1 : 0);
		for (uint32_t i = 0; i < ext->length; i++)
		{
			if (t == b->number_class)
			{
				buf_f64be(&w->out, avm2_coerce_to_number(ctx, ext->elems[i]));
			}
			else
			{
				buf_u32be(&w->out, (uint32_t) avm2_coerce_to_i32(ctx, ext->elems[i]));
			}
		}
		return;
	}
	if (w3_ref_or_store(w, vec, M3_VECOBJ)) return;
	buf_u8(&w->out, M3_VECOBJ);
	const Avm2String* tname = class_to_alias(
		ctx, (t != NULL) ? t : b->object_class);
	w3_u29(w, (int32_t) ((ext->length << 1) | 1));
	buf_u8(&w->out, ext->fixed ? 1 : 0);
	w3_str(w, tname->utf8, tname->len);
	for (uint32_t i = 0; i < ext->length; i++)
	{
		w3_value(w, ext->elems[i]);
	}
}

static void w3_dictionary(Amf3Wr* w, Avm2Object* dict)
{
	Avm2Context* ctx = w->ctx;
	if (w3_ref_or_store(w, dict, M3_DICT)) return;
	buf_u8(&w->out, M3_DICT);
	uint32_t count = 0;
	uint32_t idx = avm2_object_next_enumerant(dict, 0);
	while (idx != 0)
	{
		count++;
		idx = avm2_object_next_enumerant(dict, idx);
	}
	w3_u29(w, (int32_t) ((count << 1) | 1));
	buf_u8(&w->out, 0);  // weak_keys (unobservable; always strong)
	idx = avm2_object_next_enumerant(dict, 0);
	while (idx != 0)
	{
		w3_value(w, avm2_object_enumerant_name(ctx, dict, idx));
		w3_value(w, avm2_object_enumerant_value(ctx, dict, idx));
		idx = avm2_object_next_enumerant(dict, idx);
	}
}

static void w3_value(Amf3Wr* w, Avm2Value v)
{
	Avm2Context* ctx = w->ctx;
	v = amf_normalize(v);
	switch (v.kind)
	{
		case AVM2_VALUE_UNDEFINED: buf_u8(&w->out, M3_UNDEF); return;
		case AVM2_VALUE_NULL: buf_u8(&w->out, M3_NULL); return;
		case AVM2_VALUE_BOOL: buf_u8(&w->out, v.u.b ? M3_TRUE : M3_FALSE); return;
		case AVM2_VALUE_INTEGER:
			buf_u8(&w->out, M3_INT);
			w3_u29(w, v.u.i);
			return;
		case AVM2_VALUE_NUMBER:
			buf_u8(&w->out, M3_NUM);
			buf_f64be(&w->out, v.u.d);
			return;
		case AVM2_VALUE_STRING:
			buf_u8(&w->out, M3_STR);
			w3_str(w, v.u.str->utf8, v.u.str->len);
			return;
	}
	Avm2Object* obj = v.u.obj;
	if (obj == NULL || obj->kind == AVM2_OBJ_FUNCTION
	    || obj->kind == AVM2_OBJ_CLASS)
	{
		buf_u8(&w->out, M3_UNDEF);
		return;
	}
	if (obj->kind == AVM2_OBJ_ARRAY)
	{
		w3_array(w, obj);
		return;
	}
	if (obj->kind == AVM2_OBJ_VECTOR)
	{
		w3_vector(w, obj);
		return;
	}
	Avm2DateExt* date = avm2_date_ext_of(v);
	if (date != NULL)
	{
		if (w3_ref_or_store(w, obj, M3_DATE)) return;
		buf_u8(&w->out, M3_DATE);
		w3_u29(w, 1);  // Size(0)
		buf_f64be(&w->out, date->millis);
		return;
	}
	Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(v);
	if (ba != NULL)
	{
		if (w3_ref_or_store(w, obj, M3_BYTEARRAY)) return;
		buf_u8(&w->out, M3_BYTEARRAY);
		w3_u29(w, (int32_t) ((ba->len << 1) | 1));
		buf_put(&w->out, ba->bytes, ba->len);
		return;
	}
	if (avm2_is_dictionary(obj))
	{
		w3_dictionary(w, obj);
		return;
	}
	{
		// XML (not XMLList) writes as XmlString: toXMLString() bytes,
		// prettyPrinting-sensitive (Ruffle amf.rs as_xml_object arm).
		Avm2XmlExt* xe = avm2_xml_ext_of(v);
		if (xe != NULL)
		{
			if (w3_ref_or_store(w, obj, M3_XMLSTR)) return;
			const Avm2String* xs = avm2_e4x_to_xml_string(w->ctx, xe->node);
			buf_u8(&w->out, M3_XMLSTR);
			w3_u29(w, (int32_t) ((xs->len << 1) | 1));
			buf_put(&w->out, (const uint8_t*) xs->utf8, xs->len);
			return;
		}
	}
	w3_object(w, obj);
}

// ---------------------------------------------------------------------------
// AMF0 writer — Flash Player's behaviour, not flash-lso's
// ---------------------------------------------------------------------------
//
// Ported from the AVM1 implementation (avm1_amf.c, net-socket-arc.md §7.2,
// pinned against Flash-recorded bytes). It differs from flash-lso/Ruffle in
// four ways, each of which some `known_failure` test in the corpus grades:
//
//   1. Arrays, on a WIRE channel only: a native Array whose every own
//      enumerable key is an index is a StrictArray 0x0A, densified to
//      max(length, maxIndex+1) with 0x06 for the holes, at EVERY nesting level.
//      One non-index key demotes the whole value to an ECMAArray 0x08 whose u32
//      is `length`, entries in enumeration order. Ruffle always writes
//      ECMAArray, on every channel.
//   2. An aliased class serializes as a typed object 0x10 carrying the alias,
//      nested as well as at top level (Ruffle passes class_def None always).
//   3. XML writes as 0x0F (u32 length + markup).
//   4. A reference table: every repeated REFERENCEABLE value (object, typed
//      object, either array shape, Date, XML) collapses to 0x07 + u16 index,
//      which is also what stops a cyclic graph from recursing until the C stack
//      runs out. Primitives never take a slot. The reader reserves in the same
//      order, which is what keeps the two index spaces in step.

// The array rule is CHANNEL-dependent, exactly as it is in AVM1 (there between
// the wire and an LSO body): a NetConnection/LocalConnection packet promotes a
// dense array to a StrictArray, while ByteArray.writeObject keeps the ECMAArray
// form for every array shape. from_shumway/encoding1 pins the ByteArray side
// (a dense ["a", {}] is `08 00000002` with "0"/"1" keys) and
// netconnection_serialize_arrays the wire side (the same shape is `0A`).
//
// A StrictArray is densified, so `a[2147483647] = x` would emit two gigabytes
// of 0x06. Flash really would; we write a (bounded) ECMAArray instead rather
// than hang. Nothing in the corpus comes within four orders of magnitude.
#define W0_MAX_STRICT 65536

enum
{
	M0_NUM = 0x00, M0_BOOL = 0x01, M0_STR = 0x02, M0_OBJECT = 0x03,
	M0_NULL = 0x05, M0_UNDEF = 0x06, M0_REF = 0x07, M0_ECMA = 0x08,
	M0_OBJEND = 0x09, M0_STRICT = 0x0A, M0_DATE = 0x0B, M0_LONGSTR = 0x0C,
	M0_UNSUPPORTED = 0x0D, M0_XML = 0x0F, M0_TYPED = 0x10, M0_AMF3 = 0x11,
};

static void w0_short_str(Amf3Wr* w, const char* p, uint32_t n)
{
	buf_u16be(&w->out, (uint16_t) n);
	buf_put(&w->out, p, n);
}

// An AMF0 index key: pure ASCII digits, no sign, in u32 range. Flash's own
// bytes confirm `-1` and `2.5` count as non-numeric and so demote an array.
static int w0_key_is_index(const char* name, uint32_t len, uint32_t* out)
{
	if (len == 0 || len > 10) return 0;
	uint64_t v = 0;
	for (uint32_t i = 0; i < len; i++)
	{
		if (name[i] < '0' || name[i] > '9') return 0;
		v = v * 10 + (uint64_t) (name[i] - '0');
	}
	if (v > 4294967294ULL) return 0;
	if (out != NULL) *out = (uint32_t) v;
	return 1;
}

// AMF0 reference gate: a repeated referenceable value becomes 0x07 + u16 index.
// Returns 1 when a reference was emitted (nothing else to write).
static int w0_ref_or_store(Amf3Wr* w, Avm2Object* obj)
{
	for (uint32_t i = 0; i < w->obj0_count; i++)
	{
		if (w->objs0[i].src == obj)
		{
			buf_u8(&w->out, M0_REF);
			buf_u16be(&w->out, (uint16_t) i);
			return 1;
		}
	}
	if (w->obj0_count == w->obj0_cap)
	{
		uint32_t nc = w->obj0_cap == 0 ? 16 : w->obj0_cap * 2;
		ObjEntry* g = avm2_alloc(w->ctx, nc * sizeof(ObjEntry));
		memcpy(g, w->objs0, w->obj0_count * sizeof(ObjEntry));
		if (w->objs0 != NULL) heap_free(w->ctx->app, (void*) w->objs0);
		w->objs0 = g;
		w->obj0_cap = nc;
	}
	w->objs0[w->obj0_count++].src = obj;
	return 0;
}

static void w0_object_body(Amf3Wr* w, Avm2Object* obj)
{
	Avm2Context* ctx = w->ctx;
	Avm2Value recv = avm2_object_value(obj);
	const Avm2String** statics;
	uint32_t static_count = collect_static_props(ctx, obj, &statics);
	for (uint32_t i = 0; i < static_count; i++)
	{
		Avm2Value pv = avm2_get_public_property(ctx, recv, statics[i]->utf8,
		                                        statics[i]->len, NULL);
		w0_short_str(w, statics[i]->utf8, statics[i]->len);
		w0_value(w, pv);
	}
	uint32_t idx = avm2_object_next_enumerant(obj, 0);
	while (idx != 0)
	{
		Avm2Value name = avm2_object_enumerant_name(ctx, obj, idx);
		Avm2Value val = avm2_object_enumerant_value(ctx, obj, idx);
		if (!value_is_function(val))
		{
			const Avm2String* ns = avm2_coerce_to_string(ctx, name);
			w0_short_str(w, ns->utf8, ns->len);
			w0_value(w, val);
		}
		idx = avm2_object_next_enumerant(obj, idx);
	}
	buf_u16be(&w->out, 0);
	buf_u8(&w->out, M0_OBJEND);
}

static void w0_value(Amf3Wr* w, Avm2Value v)
{
	Avm2Context* ctx = w->ctx;
	v = amf_normalize(v);
	switch (v.kind)
	{
		case AVM2_VALUE_UNDEFINED: buf_u8(&w->out, M0_UNDEF); return;
		case AVM2_VALUE_NULL: buf_u8(&w->out, M0_NULL); return;
		case AVM2_VALUE_BOOL:
			buf_u8(&w->out, M0_BOOL);
			buf_u8(&w->out, v.u.b ? 1 : 0);
			return;
		case AVM2_VALUE_INTEGER:
			buf_u8(&w->out, M0_NUM);
			buf_f64be(&w->out, (double) v.u.i);
			return;
		case AVM2_VALUE_NUMBER:
			buf_u8(&w->out, M0_NUM);
			buf_f64be(&w->out, v.u.d);
			return;
		case AVM2_VALUE_STRING:
			if (v.u.str->len > 65535)
			{
				buf_u8(&w->out, M0_LONGSTR);
				buf_u32be(&w->out, v.u.str->len);
				buf_put(&w->out, v.u.str->utf8, v.u.str->len);
			}
			else
			{
				buf_u8(&w->out, M0_STR);
				w0_short_str(w, v.u.str->utf8, v.u.str->len);
			}
			return;
	}
	Avm2Object* obj = v.u.obj;
	if (obj == NULL || obj->kind == AVM2_OBJ_FUNCTION
	    || obj->kind == AVM2_OBJ_CLASS)
	{
		buf_u8(&w->out, M0_UNDEF);
		return;
	}
	if (obj->kind == AVM2_OBJ_ARRAY)
	{
		if (w0_ref_or_store(w, obj)) return;
		// The Flash array rule (see the header comment): on a WIRE channel,
		// StrictArray unless some own enumerable key is not an index.
		Avm2ArrayExt* ext = avm2_array_ext(obj);
		uint32_t length = (ext != NULL) ? ext->length : 0;
		int has_non_index = 0;
		int64_t max_index = -1;
		uint32_t idx = avm2_object_next_enumerant(obj, 0);
		while (idx != 0)
		{
			Avm2Value name = avm2_object_enumerant_name(ctx, obj, idx);
			Avm2Value val = avm2_object_enumerant_value(ctx, obj, idx);
			// A function-valued property is omitted from the stream entirely, so
			// it cannot demote the array either.
			if (!value_is_function(val))
			{
				const Avm2String* ns = avm2_coerce_to_string(ctx, name);
				uint32_t k;
				if (w0_key_is_index(ns->utf8, ns->len, &k))
				{
					if ((int64_t) k > max_index) max_index = (int64_t) k;
				}
				else
				{
					has_non_index = 1;
				}
			}
			idx = avm2_object_next_enumerant(obj, idx);
		}
		int64_t count = (max_index >= 0) ? max_index + 1 : 0;
		if ((int64_t) length > count) count = (int64_t) length;
		if (w->wire_mode && !has_non_index && count <= W0_MAX_STRICT)
		{
			buf_u8(&w->out, M0_STRICT);
			buf_u32be(&w->out, (uint32_t) count);
			for (int64_t i = 0; i < count; i++)
			{
				Avm2Value ev = avm2_array_get(obj, (uint32_t) i);
				if (ev.kind == AVM2_VALUE_HOLE) buf_u8(&w->out, M0_UNDEF);
				else w0_value(w, ev);
			}
			return;
		}
		// ECMA array: u32 = Array.length, every entry as a named element (dense
		// indices become string names).
		buf_u8(&w->out, M0_ECMA);
		buf_u32be(&w->out, length);
		idx = avm2_object_next_enumerant(obj, 0);
		while (idx != 0)
		{
			Avm2Value name = avm2_object_enumerant_name(ctx, obj, idx);
			Avm2Value val = avm2_object_enumerant_value(ctx, obj, idx);
			if (!value_is_function(val))
			{
				const Avm2String* ns = avm2_coerce_to_string(ctx, name);
				w0_short_str(w, ns->utf8, ns->len);
				w0_value(w, val);
			}
			idx = avm2_object_next_enumerant(obj, idx);
		}
		buf_u16be(&w->out, 0);
		buf_u8(&w->out, M0_OBJEND);
		return;
	}
	Avm2DateExt* date = avm2_date_ext_of(v);
	if (date != NULL)
	{
		if (w0_ref_or_store(w, obj)) return;
		buf_u8(&w->out, M0_DATE);
		buf_f64be(&w->out, date->millis);
		buf_u16be(&w->out, 0);
		return;
	}
	{
		// XML (not XMLList): 0x0F + u32 length + the markup, same string the
		// AMF3 XmlString arm writes.
		Avm2XmlExt* xe = avm2_xml_ext_of(v);
		if (xe != NULL)
		{
			if (w0_ref_or_store(w, obj)) return;
			const Avm2String* xs = avm2_e4x_to_xml_string(ctx, xe->node);
			buf_u8(&w->out, M0_XML);
			buf_u32be(&w->out, xs->len);
			buf_put(&w->out, xs->utf8, xs->len);
			return;
		}
	}
	if (obj->kind == AVM2_OBJ_VECTOR || avm2_bytearray_ext_of(v) != NULL
	    || avm2_is_dictionary(obj))
	{
		// flash-lso amf0 fallback arm.
		buf_u8(&w->out, M0_UNSUPPORTED);
		return;
	}
	if (w0_ref_or_store(w, obj)) return;
	// A registerClassAlias'd class writes as a typed object carrying the alias
	// (Flash does this nested too); everything else is anonymous.
	const Avm2String* alias = class_to_alias(ctx, avm2_value_class(ctx, v));
	if (alias != NULL && alias->len > 0)
	{
		buf_u8(&w->out, M0_TYPED);
		w0_short_str(w, alias->utf8, alias->len);
	}
	else
	{
		buf_u8(&w->out, M0_OBJECT);
	}
	w0_object_body(w, obj);
}

// ---------------------------------------------------------------------------
// AMF3 reader
// ---------------------------------------------------------------------------

typedef struct Rd
{
	Avm2Context* ctx;
	const uint8_t* p;
	uint32_t n;
	uint32_t pos;
	// AMF3 tables.
	const Avm2String** strings;
	uint32_t str_count, str_cap;
	TraitEntry* traits;
	uint32_t trait_count, trait_cap;
	Avm2Value* objs;
	uint32_t obj_count, obj_cap;
	// AMF0 reference table.
	Avm2Value* refs0;
	uint32_t ref0_count, ref0_cap;
} Rd;

// Release a writer's / reader's tables (every entry point's are per-call
// scratch; they used to leak on every writeObject/readObject and wire value).
static void amf_wr_free(Amf3Wr* w)
{
	SWFAppContext* app = w->ctx->app;
	if (w->out.b != NULL) heap_free(app, w->out.b);
	if (w->strings != NULL) heap_free(app, w->strings);
	for (uint32_t i = 0; i < w->trait_count; i++)
		if (w->traits[i].statics != NULL) heap_free(app, (void*) w->traits[i].statics);
	if (w->traits != NULL) heap_free(app, w->traits);
	if (w->objs != NULL) heap_free(app, w->objs);
	if (w->objs0 != NULL) heap_free(app, w->objs0);
	memset(w, 0, sizeof(*w));
}

static void amf_rd_free(Rd* r)
{
	SWFAppContext* app = r->ctx->app;
	if (r->strings != NULL) heap_free(app, (void*) r->strings);
	for (uint32_t i = 0; i < r->trait_count; i++)
		if (r->traits[i].statics != NULL) heap_free(app, (void*) r->traits[i].statics);
	if (r->traits != NULL) heap_free(app, r->traits);
	if (r->objs != NULL) heap_free(app, r->objs);
	if (r->refs0 != NULL) heap_free(app, r->refs0);
}


static _Noreturn void rd_fail(Rd* r)
{
	avm2_throw_error(r->ctx, r->ctx->builtins.error_class, "Error: Invalid object");
}

static uint8_t rd_u8(Rd* r)
{
	if (r->pos >= r->n) rd_fail(r);
	return r->p[r->pos++];
}

static const uint8_t* rd_bytes(Rd* r, uint32_t n)
{
	if ((uint64_t) r->pos + n > r->n) rd_fail(r);
	const uint8_t* p = r->p + r->pos;
	r->pos += n;
	return p;
}

static uint16_t rd_u16be(Rd* r)
{
	const uint8_t* p = rd_bytes(r, 2);
	return (uint16_t) ((p[0] << 8) | p[1]);
}

static uint32_t rd_u32be(Rd* r)
{
	const uint8_t* p = rd_bytes(r, 4);
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
	       | ((uint32_t) p[2] << 8) | p[3];
}

static double rd_f64be(Rd* r)
{
	const uint8_t* p = rd_bytes(r, 8);
	uint64_t bits = 0;
	for (int i = 0; i < 8; i++) bits = (bits << 8) | p[i];
	double d;
	memcpy(&d, &bits, 8);
	return d;
}

static uint32_t rd_u29(Rd* r)
{
	uint32_t v = 0;
	for (int i = 0; i < 3; i++)
	{
		uint8_t b = rd_u8(r);
		if (b & 0x80)
		{
			v = (v << 7) | (b & 0x7F);
		}
		else
		{
			return (v << 7) | b;
		}
	}
	return (v << 8) | rd_u8(r);
}

static const Avm2String* rd3_str(Rd* r)
{
	uint32_t u = rd_u29(r);
	if (!(u & 1))
	{
		uint32_t idx = u >> 1;
		if (idx >= r->str_count) rd_fail(r);
		return r->strings[idx];
	}
	uint32_t len = u >> 1;
	const uint8_t* p = rd_bytes(r, len);
	const Avm2String* s = avm2_string_new(r->ctx, (const char*) p, len);
	if (len > 0)
	{
		if (r->str_count == r->str_cap)
		{
			uint32_t nc = r->str_cap == 0 ? 16 : r->str_cap * 2;
			const Avm2String** g = avm2_alloc(r->ctx, nc * sizeof(Avm2String*));
			memcpy(g, r->strings, r->str_count * sizeof(Avm2String*));
			if (r->strings != NULL) heap_free(r->ctx->app, (void*) r->strings);
			r->strings = g;
			r->str_cap = nc;
		}
		r->strings[r->str_count++] = s;
	}
	return s;
}

static uint32_t rd3_obj_reserve(Rd* r, Avm2Value v)
{
	if (r->obj_count == r->obj_cap)
	{
		uint32_t nc = r->obj_cap == 0 ? 16 : r->obj_cap * 2;
		Avm2Value* g = avm2_alloc(r->ctx, nc * sizeof(Avm2Value));
		memcpy(g, r->objs, r->obj_count * sizeof(Avm2Value));
		if (r->objs != NULL) heap_free(r->ctx->app, (void*) r->objs);
		r->objs = g;
		r->obj_cap = nc;
	}
	r->objs[r->obj_count] = v;
	return r->obj_count++;
}

static Avm2Value rd3_value(Rd* r);

// Set a deserialized property, catching + tracing errors from setters
// (Ruffle read_object; errors from the STRINGIFICATION propagate).
static void rd_set_prop_guarded(Rd* r, Avm2Value recv, const Avm2String* name,
                                Avm2Value value)
{
	Avm2Context* ctx = r->ctx;
	static const Avm2AbcException catch_any = { 0, 0xFFFFFFFF, 0, 0, 0, 1 };
	Avm2TryFrame tf;
	avm2_try_push_frame(ctx, &tf, &catch_any, 1, NULL);
	tf.op_index = 1;
	if (setjmp(tf.jb) == 0)
	{
		avm2_init_public_property(ctx, recv, name->utf8, name->len, value);
		avm2_try_pop_frame(&tf);
	}
	else
	{
		Avm2Value err = tf.exc;
		avm2_try_pop_frame(&tf);
		const Avm2String* s = avm2_coerce_to_string(ctx, err);
		printf("%.*s\n", (int) s->len, s->utf8);
	}
}

static Avm2Value rd3_read_object(Rd* r)
{
	Avm2Context* ctx = r->ctx;
	uint32_t u = rd_u29(r);
	if (!(u & 1))
	{
		uint32_t idx = u >> 1;
		if (idx >= r->obj_count) rd_fail(r);
		return r->objs[idx];
	}
	TraitEntry* trait;
	if (!(u & 2))
	{
		uint32_t idx = u >> 2;
		if (idx >= r->trait_count) rd_fail(r);
		trait = &r->traits[idx];
	}
	else
	{
		if (r->trait_count == r->trait_cap)
		{
			uint32_t nc = r->trait_cap == 0 ? 8 : r->trait_cap * 2;
			TraitEntry* g = avm2_alloc(ctx, nc * sizeof(TraitEntry));
			memcpy(g, r->traits, r->trait_count * sizeof(TraitEntry));
			if (r->traits != NULL) heap_free(r->ctx->app, (void*) r->traits);
			r->traits = g;
			r->trait_cap = nc;
		}
		trait = &r->traits[r->trait_count++];
		memset(trait, 0, sizeof(*trait));
		trait->externalizable = (u & 4) ? 1 : 0;
		if (trait->externalizable)
		{
			// Trait bit 0x04: no dynamic flag, no static names — just the
			// class name, then a body only readExternal can interpret.
			trait->name = rd3_str(r);
		}
		else
		{
			trait->dynamic = (u & 8) ? 1 : 0;
			trait->static_count = u >> 4;
			trait->name = rd3_str(r);
			trait->statics = avm2_alloc(ctx, (trait->static_count + 1)
			                                     * sizeof(Avm2String*));
			for (uint32_t i = 0; i < trait->static_count; i++)
			{
				trait->statics[i] = rd3_str(r);
			}
		}
	}

	// The nested reads below can add traits and MOVE the table (its old
	// array is freed on growth): work from a copy, never through `trait`.
	const TraitEntry tr = *trait;
	trait = NULL;
	Avm2Class* cls = (tr.name->len > 0)
		? alias_to_class(tr.name->utf8, tr.name->len)
		: NULL;
	if (tr.externalizable)
	{
		// The alias MUST resolve to a class implementing IExternalizable —
		// there is no other way to interpret an opaque body.
		if (cls == NULL || g_externalizable_iface == NULL
		    || !avm2_class_has_interface(ctx, cls, g_externalizable_iface))
		{
			avm2_throw_error(ctx, ctx->builtins.error_class,
			                 "Error #2173: Unable to read object in stream.  "
			                 "The class %.*s does not implement "
			                 "flash.utils.IExternalizable but is aliased to an "
			                 "externalizable class.",
			                 (int) tr.name->len, tr.name->utf8);
		}
		Avm2Value objv = avm2_class_construct(ctx, cls, NULL, 0);
		rd3_obj_reserve(r, objv);
		// Fresh reference tables inside the body, mirroring w3_object_external.
		Avm2Value in = ext_new_bytearray(ctx, r->p + r->pos, r->n - r->pos);
		avm2_call_public_property(ctx, objv, "readExternal", 12, &in, 1);
		Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(in);
		if (ba != NULL)
		{
			uint32_t used = ba->position;
			if ((uint64_t) r->pos + used > r->n) rd_fail(r);
			r->pos += used;
		}
		return objv;
	}
	if (cls == NULL) cls = ctx->builtins.object_class;
	Avm2Value objv = avm2_class_construct(ctx, cls, NULL, 0);
	rd3_obj_reserve(r, objv);

	for (uint32_t i = 0; i < tr.static_count; i++)
	{
		Avm2Value v = rd3_value(r);
		rd_set_prop_guarded(r, objv, tr.statics[i], v);
	}
	if (tr.dynamic)
	{
		for (;;)
		{
			const Avm2String* name = rd3_str(r);
			if (name->len == 0) break;
			Avm2Value v = rd3_value(r);
			rd_set_prop_guarded(r, objv, name, v);
		}
	}
	return objv;
}

static Avm2Value rd3_value(Rd* r)
{
	Avm2Context* ctx = r->ctx;
	uint8_t marker = rd_u8(r);
	switch (marker)
	{
		case M3_UNDEF: return avm2_undefined();
		case M3_NULL: return avm2_null();
		case M3_FALSE: return avm2_bool(false);
		case M3_TRUE: return avm2_bool(true);
		case M3_INT:
		{
			uint32_t u = rd_u29(r);
			int32_t v = (int32_t) u;
			if (u & 0x10000000) v -= 0x20000000;
			return avm2_integer(v);
		}
		case M3_NUM: return avm2_number(rd_f64be(r));
		case M3_STR: return avm2_string(rd3_str(r));
		case M3_XMLDOC:
		case M3_XMLSTR:
		{
			uint32_t u = rd_u29(r);
			if (!(u & 1))
			{
				uint32_t idx = u >> 1;
				if (idx >= r->obj_count) rd_fail(r);
				return r->objs[idx];
			}
			uint32_t len = u >> 1;
			const uint8_t* p = rd_bytes(r, len);
			Avm2Value arg = avm2_string(
				avm2_string_new(ctx, (const char*) p, len));
			Avm2Value xml = avm2_class_construct(ctx, ctx->builtins.xml_class,
			                                     &arg, 1);
			rd3_obj_reserve(r, xml);
			return xml;
		}
		case M3_DATE:
		{
			uint32_t u = rd_u29(r);
			if (!(u & 1))
			{
				uint32_t idx = u >> 1;
				if (idx >= r->obj_count) rd_fail(r);
				return r->objs[idx];
			}
			double millis = rd_f64be(r);
			Avm2Value arg = avm2_number(millis);
			Avm2Value d = avm2_class_construct(ctx, ctx->builtins.date_class,
			                                   &arg, 1);
			rd3_obj_reserve(r, d);
			return d;
		}
		case M3_ARRAY:
		{
			uint32_t u = rd_u29(r);
			if (!(u & 1))
			{
				uint32_t idx = u >> 1;
				if (idx >= r->obj_count) rd_fail(r);
				return r->objs[idx];
			}
			uint32_t dense_n = u >> 1;
			Avm2Object* arr = avm2_array_new(ctx, 0);
			Avm2Value arrv = avm2_object_value(arr);
			rd3_obj_reserve(r, arrv);
			// Assoc part first (name/value pairs until the empty name).
			for (;;)
			{
				const Avm2String* name = rd3_str(r);
				if (name->len == 0) break;
				Avm2Value v = rd3_value(r);
				avm2_init_public_property(ctx, arrv, name->utf8, name->len, v);
			}
			for (uint32_t i = 0; i < dense_n; i++)
			{
				avm2_array_set(ctx, arr, i, rd3_value(r));
			}
			return arrv;
		}
		case M3_OBJECT: return rd3_read_object(r);
		case M3_BYTEARRAY:
		{
			uint32_t u = rd_u29(r);
			if (!(u & 1))
			{
				uint32_t idx = u >> 1;
				if (idx >= r->obj_count) rd_fail(r);
				return r->objs[idx];
			}
			uint32_t len = u >> 1;
			const uint8_t* p = rd_bytes(r, len);
			Avm2Value bav = avm2_class_construct(
				ctx, ctx->builtins.bytearray_class, NULL, 0);
			Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(bav);
			avm2_bytearray_set_length_public(ctx, ba, len);
			memcpy(ba->bytes, p, len);
			ba->position = 0;
			rd3_obj_reserve(r, bav);
			return bav;
		}
		case M3_VECINT:
		case M3_VECUINT:
		case M3_VECDOUBLE:
		{
			uint32_t u = rd_u29(r);
			if (!(u & 1))
			{
				uint32_t idx = u >> 1;
				if (idx >= r->obj_count) rd_fail(r);
				return r->objs[idx];
			}
			uint32_t count = u >> 1;
			int fixed = rd_u8(r) != 0;
			Avm2Class* t = (marker == M3_VECINT) ? ctx->builtins.int_class
			               : (marker == M3_VECUINT)
			                   ? ctx->builtins.uint_class
			                   : ctx->builtins.number_class;
			Avm2Class* vc = avm2_vector_apply(ctx, t);
			Avm2Object* vec = avm2_vector_new(ctx, vc, count, 0);
			Avm2Value vv = avm2_object_value(vec);
			rd3_obj_reserve(r, vv);
			Avm2VectorExt* ext = avm2_vector_ext(vec);
			for (uint32_t i = 0; i < count; i++)
			{
				if (marker == M3_VECDOUBLE)
				{
					ext->elems[i] = avm2_number(rd_f64be(r));
				}
				else if (marker == M3_VECUINT)
				{
					ext->elems[i] = avm2_uint_value(rd_u32be(r));
				}
				else
				{
					ext->elems[i] = avm2_integer((int32_t) rd_u32be(r));
				}
			}
			ext->fixed = fixed ? 1 : 0;
			return vv;
		}
		case M3_VECOBJ:
		{
			uint32_t u = rd_u29(r);
			if (!(u & 1))
			{
				uint32_t idx = u >> 1;
				if (idx >= r->obj_count) rd_fail(r);
				return r->objs[idx];
			}
			uint32_t count = u >> 1;
			int fixed = rd_u8(r) != 0;
			const Avm2String* tname = rd3_str(r);
			Avm2Class* t = (tname->len > 0)
				? alias_to_class(tname->utf8, tname->len)
				: NULL;
			if (t == NULL) t = ctx->builtins.object_class;
			Avm2Class* vc = avm2_vector_apply(ctx, t);
			Avm2Object* vec = avm2_vector_new(ctx, vc, count, 0);
			Avm2Value vv = avm2_object_value(vec);
			rd3_obj_reserve(r, vv);
			Avm2VectorExt* ext = avm2_vector_ext(vec);
			for (uint32_t i = 0; i < count; i++)
			{
				Avm2Value v = rd3_value(r);
				// No Vector.<void>: Undefined items become Null.
				if (v.kind == AVM2_VALUE_UNDEFINED) v = avm2_null();
				ext->elems[i] = v;
			}
			ext->fixed = fixed ? 1 : 0;
			return vv;
		}
		case M3_DICT:
		{
			uint32_t u = rd_u29(r);
			if (!(u & 1))
			{
				uint32_t idx = u >> 1;
				if (idx >= r->obj_count) rd_fail(r);
				return r->objs[idx];
			}
			uint32_t count = u >> 1;
			Avm2Value weak = avm2_bool(rd_u8(r) != 0);
			Avm2Value dv = avm2_class_construct(
				ctx, ctx->builtins.dictionary_class, &weak, 1);
			rd3_obj_reserve(r, dv);
			for (uint32_t i = 0; i < count; i++)
			{
				Avm2Value key = rd3_value(r);
				Avm2Value val = rd3_value(r);
				if (key.kind == AVM2_VALUE_OBJECT)
				{
					avm2_object_set_dynamic_obj(ctx, dv.u.obj, key.u.obj, val);
				}
				else
				{
					const Avm2String* ks = avm2_coerce_to_string(ctx, key);
					avm2_object_set_dynamic(ctx, dv.u.obj, ks->utf8, ks->len,
					                        val);
				}
			}
			return dv;
		}
		default:
			rd_fail(r);
	}
}

// ---------------------------------------------------------------------------
// AMF0 reader
// ---------------------------------------------------------------------------

static Avm2Value rd0_value(Rd* r);

static uint32_t rd0_ref_reserve(Rd* r, Avm2Value v)
{
	if (r->ref0_count == r->ref0_cap)
	{
		uint32_t nc = r->ref0_cap == 0 ? 16 : r->ref0_cap * 2;
		Avm2Value* g = avm2_alloc(r->ctx, nc * sizeof(Avm2Value));
		memcpy(g, r->refs0, r->ref0_count * sizeof(Avm2Value));
		if (r->refs0 != NULL) heap_free(r->ctx->app, (void*) r->refs0);
		r->refs0 = g;
		r->ref0_cap = nc;
	}
	r->refs0[r->ref0_count] = v;
	return r->ref0_count++;
}

static const Avm2String* rd0_short_str(Rd* r)
{
	uint16_t len = rd_u16be(r);
	const uint8_t* p = rd_bytes(r, len);
	return avm2_string_new(r->ctx, (const char*) p, len);
}

// name/value pairs terminated by "" + ObjectEnd, applied to recv.
static void rd0_pairs_into(Rd* r, Avm2Value recv)
{
	Avm2Context* ctx = r->ctx;
	(void) ctx;
	for (;;)
	{
		const Avm2String* name = rd0_short_str(r);
		if (name->len == 0)
		{
			uint8_t end = rd_u8(r);
			if (end != M0_OBJEND) rd_fail(r);
			break;
		}
		Avm2Value v = rd0_value(r);
		rd_set_prop_guarded(r, recv, name, v);
	}
}

static Avm2Value rd0_value(Rd* r)
{
	Avm2Context* ctx = r->ctx;
	uint8_t marker = rd_u8(r);
	switch (marker)
	{
		case M0_NUM: return avm2_number(rd_f64be(r));
		case M0_BOOL: return avm2_bool(rd_u8(r) != 0);
		case M0_STR: return avm2_string(rd0_short_str(r));
		case M0_LONGSTR:
		{
			uint32_t len = rd_u32be(r);
			const uint8_t* p = rd_bytes(r, len);
			return avm2_string(avm2_string_new(ctx, (const char*) p, len));
		}
		case M0_XML:
		{
			// Read as a String (Ruffle amf0 read), but the slot is still
			// reserved: XML is referenceable on the WRITE side, so skipping it
			// here would shift every later index in a Flash-produced stream.
			uint32_t len = rd_u32be(r);
			const uint8_t* p = rd_bytes(r, len);
			Avm2Value s = avm2_string(avm2_string_new(ctx, (const char*) p, len));
			rd0_ref_reserve(r, s);
			return s;
		}
		case M0_NULL: return avm2_null();
		case M0_UNDEF: return avm2_undefined();
		case M0_REF:
		{
			uint16_t idx = rd_u16be(r);
			if (idx >= r->ref0_count) rd_fail(r);
			return r->refs0[idx];
		}
		case M0_OBJECT:
		{
			Avm2Value obj = avm2_class_construct(
				ctx, ctx->builtins.object_class, NULL, 0);
			rd0_ref_reserve(r, obj);
			rd0_pairs_into(r, obj);
			return obj;
		}
		case M0_TYPED:
		{
			const Avm2String* name = rd0_short_str(r);
			Avm2Class* cls = (name->len > 0)
				? alias_to_class(name->utf8, name->len)
				: NULL;
			if (cls == NULL) cls = ctx->builtins.object_class;
			Avm2Value obj = avm2_class_construct(ctx, cls, NULL, 0);
			rd0_ref_reserve(r, obj);
			rd0_pairs_into(r, obj);
			return obj;
		}
		case M0_ECMA:
		{
			(void) rd_u32be(r);  // declared length (ignored on read)
			Avm2Object* arr = avm2_array_new(ctx, 0);
			Avm2Value arrv = avm2_object_value(arr);
			rd0_ref_reserve(r, arrv);
			for (;;)
			{
				const Avm2String* name = rd0_short_str(r);
				if (name->len == 0)
				{
					uint8_t end = rd_u8(r);
					if (end != M0_OBJEND) rd_fail(r);
					break;
				}
				Avm2Value v = rd0_value(r);
				avm2_init_public_property(ctx, arrv, name->utf8, name->len, v);
			}
			return arrv;
		}
		case M0_STRICT:
		{
			uint32_t count = rd_u32be(r);
			Avm2Object* arr = avm2_array_new(ctx, 0);
			Avm2Value arrv = avm2_object_value(arr);
			rd0_ref_reserve(r, arrv);
			for (uint32_t i = 0; i < count; i++)
			{
				avm2_array_set(ctx, arr, i, rd0_value(r));
			}
			return arrv;
		}
		case M0_DATE:
		{
			double millis = rd_f64be(r);
			(void) rd_u16be(r);  // timezone
			Avm2Value arg = avm2_number(millis);
			Avm2Value d = avm2_class_construct(ctx, ctx->builtins.date_class,
			                                   &arg, 1);
			rd0_ref_reserve(r, d);  // referenceable, like the writer's table
			return d;
		}
		case M0_UNSUPPORTED: return avm2_undefined();
		case M0_AMF3: return rd3_value(r);
		default:
			rd_fail(r);
	}
}

// ---------------------------------------------------------------------------
// ByteArray natives
// ---------------------------------------------------------------------------

Avm2Value avm2_amf_write_object(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	// Direction-aware: on a Socket this resolves to the outbound buffer.
	Avm2ByteArrayExt* ba = avm2_bytearray_ext_dir(act, 1);
	if (ba == NULL) return avm2_undefined();
	Avm2Value obj = (act->argc > 0) ? act->args[0] : avm2_undefined();

	Amf3Wr w;
	memset(&w, 0, sizeof(w));
	w.ctx = ctx;
	w.out.ctx = ctx;
	if (ba->object_encoding == 0)
	{
		w0_value(&w, obj);
	}
	else
	{
		w3_value(&w, obj);
	}
	// Append at the cursor like any other write.
	extern void avm2_bytearray_set_length_public(Avm2Context*, Avm2ByteArrayExt*, uint32_t);
	uint32_t pos = ba->position;
	if (ba->len < pos + w.out.len)
	{
		avm2_bytearray_set_length_public(ctx, ba, pos + w.out.len);
	}
	memcpy(ba->bytes + pos, w.out.b, w.out.len);
	ba->position = pos + w.out.len;
	amf_wr_free(&w);
	return avm2_undefined();
}

// ---------------------------------------------------------------------------
// Single-value codec entry points for the NetConnection wire (avm2_net.c)
// ---------------------------------------------------------------------------
//
// The buffers are malloc'd, not GC-allocated: they live in amf_packet.c's
// AmfBuf world (the queue holds them across ticks, and amf_packet_build reads
// them as plain byte ranges), so the caller free()s them. Each call gets FRESH
// reference/string/trait tables, which is exactly the per-argument framing Flash
// uses — one table per top-level value, not one per packet.
static unsigned char* amf_copy_out(Amf3Wr* w, size_t* out_len)
{
	unsigned char* p = (unsigned char*) malloc(w->out.len > 0 ? w->out.len : 1);
	if (p != NULL && w->out.len > 0) memcpy(p, w->out.b, w->out.len);
	if (out_len != NULL) *out_len = (p != NULL) ? w->out.len : 0;
	amf_wr_free(w);
	return p;
}

unsigned char* avm2_amf0_write_value(Avm2Context* ctx, Avm2Value v, size_t* out_len)
{
	Amf3Wr w;
	memset(&w, 0, sizeof(w));
	w.ctx = ctx;
	w.out.ctx = ctx;
	w.wire_mode = 1;   // NetConnection / LocalConnection: arrays promote
	w0_value(&w, v);
	return amf_copy_out(&w, out_len);
}

// The AMF0 "avmplus object" escape (0x11) followed by one AMF3 value — how an
// objectEncoding = AMF3 NetConnection carries each call argument.
unsigned char* avm2_amf3_write_value_tagged(Avm2Context* ctx, Avm2Value v,
                                            size_t* out_len)
{
	Amf3Wr w;
	memset(&w, 0, sizeof(w));
	w.ctx = ctx;
	w.out.ctx = ctx;
	buf_u8(&w.out, M0_AMF3);
	w3_value(&w, v);
	return amf_copy_out(&w, out_len);
}

Avm2Value avm2_amf0_read_value(Avm2Context* ctx, const unsigned char* p, size_t n)
{
	Rd r;
	memset(&r, 0, sizeof(r));
	r.ctx = ctx;
	r.p = p;
	r.n = (uint32_t) n;
	r.pos = 0;
	Avm2Value v = rd0_value(&r);
	amf_rd_free(&r);
	return v;
}

Avm2Value avm2_amf_read_object(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2ByteArrayExt* ba = avm2_bytearray_ext_dir(act, 0);
	if (ba == NULL) return avm2_undefined();
	Rd r;
	memset(&r, 0, sizeof(r));
	r.ctx = ctx;
	r.p = ba->bytes;
	r.n = ba->len;
	r.pos = ba->position;
	Avm2Value v = (ba->object_encoding == 0) ? rd0_value(&r) : rd3_value(&r);
	ba->position = r.pos;
	amf_rd_free(&r);
	return v;
}

// ---------------------------------------------------------------------------
// flash.net toplevel + registration
// ---------------------------------------------------------------------------

// Append a method entry keyed in an INTERFACE namespace (`ns` is the
// "package:Iface" URI ASC emits, kind Namespace/Package — avm2_ns_fold treats
// 0x08 and 0x16 alike). `fn` NULL means a declaration-only interface entry.
static void amf_add_iface_method(Avm2Context* ctx, Avm2Class* cls,
                                 const char* ns, const char* name,
                                 Avm2MethodFn fn)
{
	Avm2PropEntry e;
	memset(&e, 0, sizeof(e));
	e.key.ns_kind = 0x16;
	e.key.ns_uri = ns;
	e.key.ns_len = (uint32_t) strlen(ns);
	e.key.name = name;
	e.key.name_len = (uint32_t) strlen(name);
	e.kind = AVM2_PROP_METHOD;
	e.method.fn = fn;
	e.method.debug_name = name;
	e.defining_class = cls;
	avm2_vtable_append(ctx, &cls->ivtable, &e);
}

static Avm2Value oe_get_dyn_prop_writer(Avm2Activation* act)
{
	(void) act;
	return g_dyn_prop_writer;
}

static Avm2Value oe_set_dyn_prop_writer(Avm2Activation* act)
{
	Avm2Value v = (act->argc > 0) ? act->args[0] : avm2_null();
	g_dyn_prop_writer = (v.kind == AVM2_VALUE_OBJECT) ? v : avm2_null();
	return avm2_undefined();
}

static Avm2Value net_register_class_alias(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	if (act->argc < 2 || act->args[0].kind == AVM2_VALUE_NULL
	    || act->args[0].kind == AVM2_VALUE_UNDEFINED)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #2007: Parameter aliasName must be non-null.");
	}
	const Avm2String* alias = avm2_coerce_to_string(ctx, act->args[0]);
	if (act->args[1].kind != AVM2_VALUE_OBJECT
	    || act->args[1].u.obj->kind != AVM2_OBJ_CLASS)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #2007: Parameter classObject must be non-null.");
	}
	Avm2Class* cls = act->args[1].u.obj->class_ref;
	// Re-registering an alias replaces the class.
	for (AliasEntry* a = g_aliases; a != NULL; a = a->next)
	{
		if (avm2_string_equals(a->alias, alias))
		{
			a->cls = cls;
			return avm2_undefined();
		}
	}
	AliasEntry* a = avm2_alloc(ctx, sizeof(AliasEntry));
	a->alias = alias;
	a->cls = cls;
	a->next = g_aliases;
	g_aliases = a;
	return avm2_undefined();
}

static Avm2Value net_get_class_by_alias(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	if (act->argc < 1 || act->args[0].kind == AVM2_VALUE_NULL
	    || act->args[0].kind == AVM2_VALUE_UNDEFINED)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #2007: Parameter aliasName must be non-null.");
	}
	const Avm2String* alias = avm2_coerce_to_string(ctx, act->args[0]);
	Avm2Class* cls = alias_to_class(alias->utf8, alias->len);
	if (cls == NULL)
	{
		avm2_throw_error(ctx, ctx->builtins.reference_error_class,
		                 "Error #1014: Class %.*s could not be found.",
		                 (int) alias->len, alias->utf8);
	}
	return avm2_object_value(cls->class_object);
}

// ---------------------------------------------------------------------------
// flash.net.SharedObject (Stage 10). Minimal in-process model: getLocal(name)
// returns a per-name SharedObject whose `data` is a dynamic Object. Persistence
// to a .sol file + AMF `size` accounting are NOT modeled (the full shared_object
// test needs the two-run .sol harness); flush reports "flushed" so the common
// getLocal/data/flush path works (shared_object_no_root).
// ---------------------------------------------------------------------------

typedef struct Avm2SharedObjectExt
{
	Avm2EventDispatcherExt dispatcher;  // extends EventDispatcher (MUST be first)
	Avm2Object* data;                   // dynamic Object
} Avm2SharedObjectExt;

static Avm2Class* g_shared_object_class;

static Avm2Object* this_obj_amf(Avm2Activation* act)
{
	return act->this_val.kind == AVM2_VALUE_OBJECT ? act->this_val.u.obj : NULL;
}

typedef struct SoCacheEntry { char* name; Avm2Object* obj; } SoCacheEntry;
static SoCacheEntry g_so_cache[64];
static uint32_t g_so_cache_count;

// GC root marker (Stage 11): the per-name SharedObject cache. Each entry's
// SharedObject instance (and its dynamic `data` object, reached by tracing the
// instance) persists across ticks reachable only from here.
void avm2_gc_mark_roots_amf(Avm2Context* ctx)
{
	(void) ctx;
	for (uint32_t i = 0; i < g_so_cache_count; i++) avm2_gc_mark_object(g_so_cache[i].obj);
	// registerClassAlias strings live only in this C-static registry.
	for (AliasEntry* a = g_aliases; a != NULL; a = a->next) avm2_gc_mark_string(a->alias);
	// ObjectEncoding.dynamicPropertyWriter is a script object held only here.
	if (g_dyn_prop_writer.kind == AVM2_VALUE_OBJECT)
	{
		avm2_gc_mark_object(g_dyn_prop_writer.u.obj);
	}
}

static Avm2SharedObjectExt* so_ext_of(Avm2Context* ctx, Avm2Object* o)
{
	if (o == NULL || o->cls == NULL || g_shared_object_class == NULL) return NULL;
	for (const Avm2Class* c = o->cls; c != NULL; c = c->super_class)
		if (c == g_shared_object_class) return (Avm2SharedObjectExt*) o->native_ext;
	return NULL;
}

static Avm2Object* so_new_data(Avm2Context* ctx)
{
	Avm2Value v = avm2_class_construct(ctx, ctx->builtins.object_class, NULL, 0);
	return v.u.obj;
}

static Avm2Value so_get_local(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* name = act->argc > 0
		? avm2_coerce_to_string(ctx, act->args[0])
		: avm2_string_from_literal(ctx, "");
	// Same-run identity: return the cached instance for this name.
	for (uint32_t i = 0; i < g_so_cache_count; i++)
	{
		if (strncmp(g_so_cache[i].name, name->utf8, name->len) == 0
		    && g_so_cache[i].name[name->len] == '\0')
			return avm2_object_value(g_so_cache[i].obj);
	}
	Avm2Value v = avm2_class_construct(ctx, g_shared_object_class, NULL, 0);
	Avm2SharedObjectExt* ext = so_ext_of(ctx, v.u.obj);
	if (ext != NULL && ext->data == NULL) ext->data = so_new_data(ctx);
	if (g_so_cache_count < 64)
	{
		char* copy = avm2_alloc(ctx, name->len + 1);
		memcpy(copy, name->utf8, name->len);
		copy[name->len] = '\0';
		g_so_cache[g_so_cache_count].name = copy;
		g_so_cache[g_so_cache_count].obj = v.u.obj;
		g_so_cache_count++;
	}
	return v;
}

static Avm2Value so_get_data(Avm2Activation* act)
{
	Avm2SharedObjectExt* ext = so_ext_of(act->ctx, this_obj_amf(act));
	if (ext == NULL) return avm2_undefined();
	if (ext->data == NULL) ext->data = so_new_data(act->ctx);
	return avm2_object_value(ext->data);
}

static Avm2Value so_get_size(Avm2Activation* act)
{
	(void) act;
	return avm2_uint_value(0);
}

static Avm2Value so_flush(Avm2Activation* act)
{
	// SharedObjectFlushStatus.FLUSHED
	return avm2_string(avm2_string_from_literal(act->ctx, "flushed"));
}

static Avm2Value so_clear(Avm2Activation* act)
{
	Avm2SharedObjectExt* ext = so_ext_of(act->ctx, this_obj_amf(act));
	if (ext != NULL) ext->data = so_new_data(act->ctx);
	return avm2_undefined();
}

static Avm2Value so_noop(Avm2Activation* act)
{
	(void) act;
	return avm2_undefined();
}

void avm2_register_amf(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;
	g_aliases = NULL;

	// flash.net toplevel functions (package-qualified keys).
	{
		Avm2PropKey key;
		key.ns_kind = 0x16;
		key.ns_priv = NULL;
		key.ns_uri = "flash.net";
		key.ns_len = 9;
		key.name = "registerClassAlias";
		key.name_len = 18;
		Avm2Object* fn = avm2_function_new(
			ctx, &(Avm2MethodRef) { net_register_class_alias, NULL,
			                        "registerClassAlias", 0 },
			NULL, NULL, avm2_undefined(), false);
		avm2_builtin_define_alias(ctx, key, avm2_object_value(fn));
		key.name = "getClassByAlias";
		key.name_len = 15;
		fn = avm2_function_new(
			ctx, &(Avm2MethodRef) { net_get_class_by_alias, NULL,
			                        "getClassByAlias", 0 },
			NULL, NULL, avm2_undefined(), false);
		avm2_builtin_define_alias(ctx, key, avm2_object_value(fn));
	}

	// flash.utils.IExternalizable (interface). Instances of implementors
	// serialize through writeExternal/readExternal and the AMF3 0x04 trait bit;
	// the vtable entries carry the interface namespace so implementor classes
	// get call-through aliases (avm2_class.c add_iface_aliases_from) and a call
	// on an IExternalizable-typed reference resolves.
	{
		Avm2Class* ie = avm2_builtin_class(ctx, "flash.utils",
		                                   "IExternalizable", NULL);
		ie->flags |= AVM2_CLASS_FLAG_INTERFACE;
		g_externalizable_iface = ie;
		amf_add_iface_method(ctx, ie, "flash.utils:IExternalizable",
		                     "writeExternal", NULL);
		amf_add_iface_method(ctx, ie, "flash.utils:IExternalizable",
		                     "readExternal", NULL);
	}

	// flash.net.IDynamicPropertyWriter / IDynamicPropertyOutput (interfaces)
	// plus the internal implementation of the latter handed to
	// writeDynamicProperties (Flash's own is flash.net::DynamicPropertyOutput).
	// writeDynamicProperty appends straight into the serializer that is running.
	{
		Avm2Class* ipw = avm2_builtin_class(ctx, "flash.net",
		                                    "IDynamicPropertyWriter", NULL);
		ipw->flags |= AVM2_CLASS_FLAG_INTERFACE;
		amf_add_iface_method(ctx, ipw, "flash.net:IDynamicPropertyWriter",
		                     "writeDynamicProperties", NULL);

		Avm2Class* ipo = avm2_builtin_class(ctx, "flash.net",
		                                    "IDynamicPropertyOutput", NULL);
		ipo->flags |= AVM2_CLASS_FLAG_INTERFACE;
		amf_add_iface_method(ctx, ipo, "flash.net:IDynamicPropertyOutput",
		                     "writeDynamicProperty", NULL);

		Avm2Class* dpo = avm2_builtin_class(ctx, "flash.net",
		                                    "DynamicPropertyOutput",
		                                    b->object_class);
		dpo->interface_count = 1;
		dpo->interfaces = avm2_alloc(ctx, sizeof(Avm2Class*));
		dpo->interfaces[0] = ipo;
		avm2_builtin_add_method(ctx, dpo, "writeDynamicProperty",
		                        dpo_write_dynamic_property);
		// Builtin classes never run avm2_class.c's interface-alias pass, so the
		// call-through key an IDynamicPropertyOutput-typed reference uses (and
		// that is the ONLY namespace ASC emits for it) is added by hand.
		amf_add_iface_method(ctx, dpo, "flash.net:IDynamicPropertyOutput",
		                     "writeDynamicProperty", dpo_write_dynamic_property);
		g_dyn_prop_output_class = dpo;
	}

	// flash.net.ObjectEncoding constants + the dynamicPropertyWriter hook.
	{
		g_dyn_prop_writer = avm2_null();
		Avm2Class* oe = avm2_builtin_class(ctx, "flash.net", "ObjectEncoding",
		                                   b->object_class);
		avm2_builtin_add_static_const(ctx, oe, "AMF0", avm2_uint_value(0));
		avm2_builtin_add_static_const(ctx, oe, "AMF3", avm2_uint_value(3));
		avm2_builtin_add_static_const(ctx, oe, "DEFAULT", avm2_uint_value(3));
		avm2_builtin_add_static_getset(ctx, oe, "dynamicPropertyWriter",
		                               oe_get_dyn_prop_writer,
		                               oe_set_dyn_prop_writer);
	}

	// flash.net.SharedObject (extends EventDispatcher).
	{
		g_so_cache_count = 0;
		Avm2Class* so = avm2_builtin_class(ctx, "flash.net", "SharedObject",
		                                   b->event_dispatcher_class);
		so->native_ext_size = sizeof(Avm2SharedObjectExt);
		g_shared_object_class = so;
		avm2_builtin_add_static_method(ctx, so, "getLocal", so_get_local);
		avm2_builtin_add_static_method(ctx, so, "getRemote", so_get_local);
		avm2_builtin_add_getset(ctx, so, "data", so_get_data, NULL);
		avm2_builtin_add_getset(ctx, so, "size", so_get_size, NULL);
		avm2_builtin_add_method(ctx, so, "flush", so_flush);
		avm2_builtin_add_method(ctx, so, "clear", so_clear);
		avm2_builtin_add_method(ctx, so, "close", so_noop);
		avm2_builtin_add_method(ctx, so, "setProperty", so_noop);
	}

	// flash.net.SharedObjectFlushStatus constants (FlxSave compares
	// flush()'s return against FLUSHED).
	{
		Avm2Class* fs = avm2_builtin_class(ctx, "flash.net",
		                                   "SharedObjectFlushStatus",
		                                   b->object_class);
		avm2_builtin_add_static_const(ctx, fs, "FLUSHED",
			avm2_string(avm2_string_from_literal(ctx, "flushed")));
		avm2_builtin_add_static_const(ctx, fs, "PENDING",
			avm2_string(avm2_string_from_literal(ctx, "pending")));
	}
}
