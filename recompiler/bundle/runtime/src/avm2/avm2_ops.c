// Opcode helpers backing the generated C in RecompiledABC/. Semantics per
// Ruffle core/src/avm2/activation.rs (op impls) and value.rs (property
// dispatch): vtable traits (slots / methods / accessors) → own dynamic
// props → prototype chain, with array-index fast paths, primitive
// receivers via their builtin class vtables, and typed errors everywhere
// (avm2_error.h). Anything not implemented aborts loudly (honest failure).

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_e4x.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <memory/heap.h>
#include <avm2/avm2_ops.h>

// E4XName from a lazy QName value (uri NULL = any namespace).
static E4XName e4x_name_from_qext(const Avm2QNameExt* q, int force_attr)
{
	E4XName n;
	memset(&n, 0, sizeof(n));
	n.local = q->local;
	n.is_attribute = (uint8_t) (q->is_attribute || force_attr);
	if (q->uri == NULL)
	{
		n.any_ns = 1;
	}
	else
	{
		n.is_qname = 1;
		n.single_uri = q->uri;
		n.single_is_real = 1;
	}
	return n;
}

// E4XName from a popped RTQName namespace + a name string. Only the ANY
// namespace (kind 0 — `Namespace(new QName(null, x))`, pool index 0) is
// "any"; every other Namespace object is an EXPLICIT qualifier, INCLUDING
// `new Namespace()` / `new Namespace("")` whose URI is empty. An empty
// explicit URI is the NO-namespace, so `none::*` must match only
// unqualified nodes rather than everything, and it must NOT be rebuilt via
// string_to_multiname (which would re-qualify it onto the current default
// xml namespace) — e4x/Regress/regress-263935, e4x/Statements/e12_1 case 8.
static E4XName e4x_name_from_rtns(Avm2Context* ctx, Avm2Value ns_val,
                                  const Avm2String* local, int attr)
{
	Avm2NamespaceExt* n = avm2_namespace_ext_of(ns_val);
	if (n != NULL && n->uri != NULL && n->kind != 0)
	{
		E4XName name;
		memset(&name, 0, sizeof(name));
		// `local == NULL` is the E4X any-name (`ns::*` / `@ns::*`): the
		// caller passes NULL for multiname name index 0 rather than the
		// empty string, which would match no node at all
		// (e4x/Expressions/e11_1_1 `y1.@ns::*`).
		name.local = local;
		name.is_attribute = (uint8_t) attr;
		name.is_qname = 1;
		name.single_uri = n->uri;
		name.single_is_real = 1;
		return name;
	}
	if (local == NULL)
	{
		// `*::*` — the any namespace with the any name.
		E4XName name;
		memset(&name, 0, sizeof(name));
		name.any_ns = 1;
		name.is_attribute = (uint8_t) attr;
		return name;
	}
	return avm2_e4x_name_from_string(ctx, local, attr);
}

static int mn_is_attribute_kind(const Avm2AbcFileData* data, uint32_t mn_idx)
{
	uint8_t k = data->multinames[mn_idx].kind;
	return k == 0x0d || k == 0x10 || k == 0x12 || k == 0x0e || k == 0x1c;
}

// Is the site multiname the NAMESPACE-SET form (Multiname / MultinameA /
// MultinameL / MultinameLA) rather than a QName? This is exactly Ruffle's
// Multiname::has_multiple_ns() -- the HAS_MULTIPLE_NS flag is set for those
// four ABC kinds and for no other, regardless of how many namespaces the set
// actually holds (multiname.rs:239-260). It is the discriminator between
// #1081 and #1069 on a failed non-public read; see getproperty_common.
static int mn_is_nsset_kind(const Avm2AbcFileData* data, uint32_t mn_idx)
{
	uint8_t k = data->multinames[mn_idx].kind;
	return k == 0x09 || k == 0x0e || k == 0x1b || k == 0x1c;
}

// Lazy-name fill keeps the multiname's static ns set (handle_input then
// adds public): a plain rebuilt name gains the compiler set so
// use-namespace lookups still match (xml_explicit_use_namespace).
static void e4x_name_attach_mn_set(Avm2Activation* act, uint32_t mn_idx, E4XName* n)
{
	const Avm2AbcFileData* data = act->file->data;
	uint8_t k = data->multinames[mn_idx].kind;
	if (k != 0x09 && k != 0x0e && k != 0x1b && k != 0x1c) return;
	if (n->is_qname || n->any_ns || n->is_attribute || n->local == NULL) return;
	if (avm2_dxns_uri(act->ctx) != NULL) return;
	n->single_uri = NULL;
	n->single_is_real = 0;
	n->data = data;
	n->ns_set = data->multinames[mn_idx].ns_set;
	n->also_public = 1;
}

_Noreturn void avm2_fatal(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "AVM2 fatal: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	fflush(stdout);
	exit(1);
}

_Noreturn void avm2_unimplemented_op(Avm2Activation* act, const char* op_name, uint32_t op_index)
{
	(void) act;
	fflush(stdout);
	fprintf(stderr, "AVM2: unimplemented op %s at op index %u (Stage-3 surface; "
	        "see avm2-support-plan.md)\n", op_name, op_index);
	exit(1);
}

_Noreturn void avm2_verify_error_body(Avm2Activation* act, const char* message)
{
	// Reachable VerifyError bodies throw a catchable VerifyError.
	//
	// FP rejects the method at VERIFY time — before the frame that would name
	// it is pushed — so the error's getStackTrace() is empty. Our verify body
	// is emitted inside that very method, so the snapshot has to be suppressed
	// explicitly or an uncaught #1011 prints a frame FP never had
	// (avm2/verify_illegal_opcode expects the error line alone).
	act->ctx->suppress_stack_snapshot = 1;
	avm2_throw_error(act->ctx, act->ctx->builtins.verify_error_class, "%s", message);
}

// ---------------------------------------------------------------------------
// Property resolution engine
// ---------------------------------------------------------------------------

// How a property was resolved on a receiver.
typedef struct Resolved
{
	const Avm2PropEntry* entry;  // vtable trait
	Avm2DynProp* dyn;            // own dynamic prop (the entry, so writes can
	                             // see its read_only attribute)
	Avm2Object* proto_holder;    // proto-chain holder of `dyn`
	Avm2Object* proto_arr;       // proto-chain Array whose ELEMENT storage
	                             // holds the value (read-only; writes shadow)
	int is_array_elem;
	int is_vector_elem;
	int is_bytearray_elem;
	uint32_t arr_index;
	// Proxy receiver whose declared traits missed: route the access to the
	// flash_proxy hooks with QName(proxy_uri, proxy_local) — NULL = any
	// (Ruffle proxy_object.rs *_local overrides).
	int is_proxy;
	const Avm2String* proxy_uri;
	const Avm2String* proxy_local;
} Resolved;

#ifdef AVM2_HOTLOOP_PROF
// §7-rule-6 counters for hot AS3 loops (build -DAVM2_HOTLOOP_PROF; see the
// definitions at the end of this file). Dumped per tick by
// avm2_hotloop_prof_tick when a tick does enough property gets to matter.
enum { HL_HIT = 0, HL_MISS_NONOBJ, HL_MISS_NOVT, HL_MISS_COLD, HL_MISS_VT,
       HL_MISS_COUNT, HL_N };
static void hl_ic_note(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                       const Avm2InlineCache* ic, int outcome,
                       const Avm2PropEntry* hit_entry);
static void hl_add_note(Avm2Value a, Avm2Value b);
static void hl_coerce_note(Avm2Activation* act, Avm2Value v, uint32_t mn_idx);
#endif

static int value_is_null_like(Avm2Value v)
{
	return v.kind == AVM2_VALUE_UNDEFINED || v.kind == AVM2_VALUE_NULL;
}

// Unwrap the transparent primitive-scope box `avm2_op_pushscope` makes for
// `with (<primitive>)`. Applied to the RECEIVER at every property get/set/call
// entry point so a method found through the box runs with the primitive as
// `this` (`with (7) x = valueOf()` must give a number, not the box —
// from_avmplus/ecma3/Statements/e12_10). Resolution on the primitive is
// already equivalent (avm2_value_vtable -> class ivtable, avm2_value_proto ->
// class prototype), so this is pure receiver substitution. The flag can only
// be set by pushscope, and the box is unreachable outside a scope chain.
static Avm2Value unbox_scope_prim(Avm2Value v)
{
	if (v.kind == AVM2_VALUE_OBJECT && v.u.obj != NULL && v.u.obj->is_prim_box
	    && v.u.obj->slot_count > 1)
	{
		return v.u.obj->slots[1];
	}
	return v;
}

// Is `obj` allowed to grow dynamic props? Array kind is NOT automatically
// dynamic any more: a sealed `extends Array` subclass carries element storage
// below SWF 13 (avm2_class_instance_kind) and must still answer the sealed
// rules for property access. Array itself and every dynamic subclass have an
// unsealed class, so they land on the flag test below.
int avm2_object_is_dynamic(Avm2Object* obj)
{
	if (obj->kind == AVM2_OBJ_CLASS || obj->kind == AVM2_OBJ_FUNCTION)
	{
		return 1;
	}
	if (obj->is_prototype) return 1;
	if (obj->cls == NULL) return 1;
	return (obj->cls->flags & AVM2_CLASS_FLAG_SEALED) == 0;
}

static int object_is_dynamic(Avm2Object* obj)
{
	return avm2_object_is_dynamic(obj);
}

// Does index-keyed property ACCESS on `obj` reach element storage? Only on a
// dynamic array receiver. A sealed Array subclass keeps its storage for the
// Array methods, but `b[0]` / `b[0] = x` / `hasOwnProperty(0)` / `delete b[0]`
// go through the ordinary sealed-object rules — #1069 / #1056 / false / false
// (regress/bug_654807_swf12 "semisealed" columns). Enumeration is deliberately
// NOT gated: for-in on such an instance still yields its element indices.
static int array_index_access(Avm2Object* obj)
{
	return obj != NULL && obj->kind == AVM2_OBJ_ARRAY && object_is_dynamic(obj);
}

// Dictionary receivers route OBJECT-valued lazy names to object space
// (Ruffle op_get_property_fast Object arm).
static int dict_object_key(Avm2Value recv, Avm2Value name_val,
                           Avm2Object** out_dict, Avm2Object** out_key)
{
	if (recv.kind != AVM2_VALUE_OBJECT || name_val.kind != AVM2_VALUE_OBJECT)
	{
		return 0;
	}
	if (!avm2_is_dictionary(recv.u.obj)) return 0;
	*out_dict = recv.u.obj;
	*out_key = name_val.u.obj;
	return 1;
}

// QName components of a static multiname for Proxy hooks (Ruffle
// QNameObject::from_name): single-ns set -> that URI, multi-ns set -> "",
// the any namespace / any name -> NULL.
static void mn_qname_parts(Avm2Context* ctx, const Avm2AbcFileData* data,
                           uint32_t mn_idx, const Avm2String** uri,
                           const Avm2String** local)
{
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
	*local = (mn->name != 0) ? &data->strings[mn->name] : NULL;
	switch (mn->kind)
	{
		case 0x07: case 0x0d:  // QName / QNameA
			*uri = (mn->ns != 0) ? &data->strings[data->namespaces[mn->ns].name]
			                     : NULL;
			break;
		case 0x09: case 0x0e: case 0x1b: case 0x1c:  // Multiname(L)(A)
		{
			const Avm2AbcNsSet* set = &data->ns_sets[mn->ns_set];
			if (set->count == 1 && set->ns_indices[0] != 0)
			{
				*uri = &data->strings[data->namespaces[set->ns_indices[0]].name];
			}
			else if (set->count == 1)
			{
				*uri = NULL;
			}
			else
			{
				*uri = avm2_string_from_literal(ctx, "");
			}
			break;
		}
		default:
			*uri = avm2_string_from_literal(ctx, "");
			break;
	}
}

// Try to parse a property name as an array index ("0", "42").
static int name_as_index(const char* name, uint32_t len, uint32_t* out)
{
	if (len == 0 || len > 10) return 0;
	uint64_t v = 0;
	for (uint32_t i = 0; i < len; i++)
	{
		if (name[i] < '0' || name[i] > '9') return 0;
		v = v * 10 + (uint64_t) (name[i] - '0');
	}
	if (len > 1 && name[0] == '0') return 0;
	if (v >= 0xFFFFFFFFull) return 0;
	*out = (uint32_t) v;
	return 1;
}

// Resolve by name-key: vtable → array index → own dynamic → proto chain.
// `public_ok` gates the dynamic/proto part (dynamic props are public).
static int resolve_key(Avm2Context* ctx, Avm2Value recv, const Avm2PropKey* key,
                       int public_ok, Resolved* out)
{
	memset(out, 0, sizeof(*out));

	const Avm2VTable* vt = avm2_value_vtable(ctx, recv);
	const Avm2PropEntry* e = avm2_vtable_find(vt, key);
	if (e == NULL && recv.kind == AVM2_VALUE_OBJECT
	    && recv.u.obj->kind == AVM2_OBJ_CLASS
	    && ctx->builtins.class_class != NULL)
	{
		// Class objects carry their static traits; Class's own instance
		// members (the `prototype` getter) come from Class's ivtable.
		e = avm2_vtable_find(&ctx->builtins.class_class->ivtable, key);
	}
	if (e != NULL)
	{
		out->entry = e;
		return 1;
	}
	if (recv.kind == AVM2_VALUE_OBJECT && avm2_is_proxy(recv.u.obj))
	{
		// Proxies replace the whole local half (dynamic + proto) with the
		// flash_proxy hooks, for ANY namespace.
		out->is_proxy = 1;
		out->proxy_uri = avm2_string_new(ctx, key->ns_uri, key->ns_len);
		out->proxy_local = avm2_string_new(ctx, key->name, key->name_len);
		return 1;
	}
	if (!public_ok) return 0;

	if (recv.kind == AVM2_VALUE_OBJECT)
	{
		Avm2Object* obj = recv.u.obj;
		uint32_t idx;
		if (array_index_access(obj) && name_as_index(key->name, key->name_len, &idx))
		{
			Avm2Value v = avm2_array_get(obj, idx);
			if (v.kind != AVM2_VALUE_HOLE)
			{
				out->is_array_elem = 1;
				out->arr_index = idx;
				return 1;
			}
			// A hole / out-of-range falls through to dynamic + proto.
		}
		if (obj->kind == AVM2_OBJ_VECTOR
		    && name_as_index(key->name, key->name_len, &idx)
		    && idx < avm2_vector_ext(obj)->length)
		{
			out->is_vector_elem = 1;
			out->arr_index = idx;
			return 1;
		}
		if (avm2_bytearray_ext_of(recv) != NULL
		    && name_as_index(key->name, key->name_len, &idx))
		{
			// ByteArrays never forward index access (even out of bounds).
			out->is_bytearray_elem = 1;
			out->arr_index = idx;
			return 1;
		}
		out->dyn = avm2_object_find_dynamic_entry(obj, key->name, key->name_len);
		if (out->dyn != NULL) return 1;
	}

	// Prototype chain (reads only).
	// A prototype can itself be an Array -- `D.prototype = new Array` and
	// Array.prototype are both one -- and then an index write to it lands in
	// ELEMENT storage, which the by-name dyn-prop walk below cannot see. An
	// ARRAY receiver already resolves that through avm2_array_proto_index;
	// this covers every other receiver (regress/bug_687838's `Fcn .proto
	// ... array trans/immed` rows: a plain object whose prototype is a real
	// Array).
	uint32_t pidx;
	int proto_index_name =
		(recv.kind != AVM2_VALUE_OBJECT || recv.u.obj->kind != AVM2_OBJ_ARRAY)
		&& name_as_index(key->name, key->name_len, &pidx);
	Avm2Object* proto = avm2_value_proto(ctx, recv);
	while (proto != NULL)
	{
		if (proto_index_name && proto->kind == AVM2_OBJ_ARRAY)
		{
			Avm2Value pv = avm2_array_get(proto, pidx);
			if (pv.kind != AVM2_VALUE_HOLE)
			{
				out->proto_arr = proto;
				out->arr_index = pidx;
				return 1;
			}
		}
		Avm2DynProp* dv = avm2_object_find_dynamic_entry(proto, key->name,
		                                                 key->name_len);
		if (dv != NULL)
		{
			out->dyn = dv;
			out->proto_holder = proto;
			return 1;
		}
		proto = proto->proto;
	}
	return 0;
}

// Resolve a static multiname: vtable by multiname → dynamic/proto by name.
static int resolve_mn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx, Resolved* out)
{
	Avm2Context* ctx = act->ctx;
	const Avm2AbcFileData* data = act->file->data;
	memset(out, 0, sizeof(*out));

	const Avm2VTable* vt = avm2_value_vtable(ctx, recv);
	const Avm2PropEntry* e = avm2_vtable_find_mn(vt, data, mn_idx);
	if (e == NULL && recv.kind == AVM2_VALUE_OBJECT
	    && recv.u.obj->kind == AVM2_OBJ_CLASS
	    && ctx->builtins.class_class != NULL)
	{
		e = avm2_vtable_find_mn(&ctx->builtins.class_class->ivtable, data, mn_idx);
	}
	if (e != NULL)
	{
		out->entry = e;
		return 1;
	}
	if (recv.kind == AVM2_VALUE_OBJECT && avm2_is_proxy(recv.u.obj))
	{
		out->is_proxy = 1;
		mn_qname_parts(ctx, data, mn_idx, &out->proxy_uri, &out->proxy_local);
		return 1;
	}
	if (!avm2_mn_has_public_ns(data, mn_idx)) return 0;

	const char* name;
	uint32_t name_len;
	avm2_mn_name(data, mn_idx, &name, &name_len);
	Avm2PropKey key = avm2_public_key(name, name_len);
	if (recv.kind == AVM2_VALUE_OBJECT)
	{
		Avm2Object* obj = recv.u.obj;
		uint32_t idx;
		if (array_index_access(obj) && name_as_index(key.name, key.name_len, &idx))
		{
			Avm2Value v = avm2_array_get(obj, idx);
			if (v.kind != AVM2_VALUE_HOLE)
			{
				out->is_array_elem = 1;
				out->arr_index = idx;
				return 1;
			}
		}
		if (obj->kind == AVM2_OBJ_VECTOR
		    && name_as_index(key.name, key.name_len, &idx)
		    && idx < avm2_vector_ext(obj)->length)
		{
			out->is_vector_elem = 1;
			out->arr_index = idx;
			return 1;
		}
		if (avm2_bytearray_ext_of(recv) != NULL
		    && name_as_index(key.name, key.name_len, &idx))
		{
			out->is_bytearray_elem = 1;
			out->arr_index = idx;
			return 1;
		}
		out->dyn = avm2_object_find_dynamic_entry(obj, key.name, key.name_len);
		if (out->dyn != NULL) return 1;
	}
	// Element storage on a prototype that is itself an Array -- see the
	// matching walk in resolve_key.
	uint32_t pidx;
	int proto_index_name =
		(recv.kind != AVM2_VALUE_OBJECT || recv.u.obj->kind != AVM2_OBJ_ARRAY)
		&& name_as_index(key.name, key.name_len, &pidx);
	Avm2Object* proto = avm2_value_proto(ctx, recv);
	while (proto != NULL)
	{
		if (proto_index_name && proto->kind == AVM2_OBJ_ARRAY)
		{
			Avm2Value pv = avm2_array_get(proto, pidx);
			if (pv.kind != AVM2_VALUE_HOLE)
			{
				out->proto_arr = proto;
				out->arr_index = pidx;
				return 1;
			}
		}
		Avm2DynProp* dv = avm2_object_find_dynamic_entry(proto, key.name, key.name_len);
		if (dv != NULL)
		{
			out->dyn = dv;
			out->proto_holder = proto;
			return 1;
		}
		proto = proto->proto;
	}
	return 0;
}

// Bind a method trait into a bound-method closure, cached per receiver so
// obj.method === obj.method holds (Ruffle get_bound_method).
static Avm2Value bind_method_entry(Avm2Context* ctx, const Avm2PropEntry* e, Avm2Value recv)
{
	Avm2Object* holder = (recv.kind == AVM2_VALUE_OBJECT) ? recv.u.obj : NULL;
	if (holder != NULL)
	{
		for (Avm2BoundMethod* bm = holder->bound_methods; bm != NULL; bm = bm->next)
		{
			if (bm->entry == e) return avm2_object_value(bm->fn);
		}
	}
	Avm2Object* fnobj = avm2_function_new(ctx, &e->method, e->defining_class,
	                                      e->method_scope, recv, true);
	if (holder != NULL)
	{
		Avm2BoundMethod* bm = avm2_alloc(ctx, sizeof(Avm2BoundMethod));
		bm->entry = e;
		bm->fn = fnobj;
		bm->next = holder->bound_methods;
		holder->bound_methods = bm;
	}
	return avm2_object_value(fnobj);
}

// Qualified class name of a receiver, for error messages.
static const char* class_name_of(Avm2Context* ctx, Avm2Value recv, char* buf, int size)
{
	avm2_class_qname_buf(avm2_value_class(ctx, recv), buf, size);
	return buf;
}

// avmplus names a property in an error message by its full multiname:
// "Package1:ns1::classItem7" for a trait in a user namespace, and the bare
// local name for a public one (a public namespace has an empty URI).
// as3/Definitions/Variable/ConstVariables_custom1 asserts the qualified form
// in #1074.
static const char* prop_name_of(const Avm2PropKey* key, const char* name,
                                uint32_t name_len, char* buf, int size)
{
	if (key == NULL || key->ns_len == 0 || key->ns_uri == NULL)
	{
		snprintf(buf, (size_t) size, "%.*s", (int) name_len, name);
		return buf;
	}
	snprintf(buf, (size_t) size, "%.*s::%.*s", (int) key->ns_len, key->ns_uri,
	         (int) name_len, name);
	return buf;
}

// Read the resolved property's value with `recv` as the getter receiver.
static Avm2Value resolved_get(Avm2Context* ctx, Avm2Value recv, const Resolved* r,
                              const char* name, uint32_t name_len)
{
	if (r->is_proxy)
	{
		Avm2Value qn = avm2_object_value(
			avm2_qname_new(ctx, r->proxy_uri, r->proxy_local));
		return avm2_proxy_call_hook(ctx, recv.u.obj, "getProperty", &qn, 1);
	}
	if (r->proto_arr != NULL)
	{
		Avm2Value v = avm2_array_get(r->proto_arr, r->arr_index);
		return v.kind == AVM2_VALUE_HOLE ? avm2_undefined() : v;
	}
	if (r->is_array_elem)
	{
		Avm2Value v = avm2_array_get(recv.u.obj, r->arr_index);
		return v.kind == AVM2_VALUE_HOLE ? avm2_undefined() : v;
	}
	if (r->is_vector_elem)
	{
		return avm2_vector_get_index(ctx, recv.u.obj, r->arr_index);
	}
	if (r->is_bytearray_elem)
	{
		Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(recv);
		if (r->arr_index < ba->len)
		{
			return avm2_integer(ba->bytes[r->arr_index]);
		}
		return avm2_undefined();
	}
	if (r->dyn != NULL) return r->dyn->value;
	const Avm2PropEntry* e = r->entry;
	switch (e->kind)
	{
		case AVM2_PROP_SLOT:
			return recv.u.obj->slots[e->slot_index];
		case AVM2_PROP_METHOD:
			return bind_method_entry(ctx, e, recv);
		case AVM2_PROP_GETTER:
		case AVM2_PROP_GETSET:
			return avm2_call_method_ref(ctx, &e->method, e->defining_class,
			                            e->method_scope, recv, NULL, 0);
		default:
		{
			char cn[160];
			class_name_of(ctx, recv, cn, sizeof(cn));
			avm2_throw_error(ctx, ctx->builtins.reference_error_class,
			                 "Error #1077: Illegal read of write-only property "
			                 "%.*s on %s.", (int) name_len, name, cn);
		}
	}
}

// ---------------------------------------------------------------------------
// GetProperty / SetProperty / InitProperty / DeleteProperty
// ---------------------------------------------------------------------------

// avmplus qualifies a reference with its namespace URI in the #1056 / #1069 /
// #1077 / #1081 messages when the site multiname carries EXACTLY ONE namespace
// and that namespace has a non-empty URI (Ruffle Multiname::as_uri). Compiled
// `obj.name` sites carry a whole ns SET and print bare; an explicit
// `obj.ns::name` is a one-namespace QName and prints `uri::name`
// (avm2/core_exceptions lines 32/36). Returns NULL when the bare local name
// is what avmplus would print.
static const char* mn_display_name(const Avm2AbcFileData* data, uint32_t mn_idx,
                                   const char* name, uint32_t name_len,
                                   char* buf, size_t size)
{
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
	uint32_t ns_idx;
	switch (mn->kind)
	{
		case 0x07: case 0x0d:
			ns_idx = mn->ns;
			break;
		case 0x09: case 0x0e:
		{
			const Avm2AbcNsSet* set = &data->ns_sets[mn->ns_set];
			if (set->count != 1) return NULL;
			ns_idx = set->ns_indices[0];
			break;
		}
		default:
			return NULL;
	}
	const Avm2AbcNamespace* ns = &data->namespaces[ns_idx];
	const Avm2String* uri = &data->strings[ns->name];
	if (uri->len == 0) return NULL;
	snprintf(buf, size, "%.*s::%.*s", (int) uri->len, uri->utf8,
	         (int) name_len, name);
	return buf;
}

// A wildcard-name site (`obj.*` / `obj.ns::*`, ABC multiname name index 0).
// Ruffle script_object.rs get_dynamic_property: a wildcard that reaches the
// dynamic lookup is #1081 on an OBJECT receiver and #1069 on a PRIMITIVE one
// -- the sealed/dynamic split does not apply
// (as3/Expressions/QualifiedReferences/WildcardOperator asserts all three).
static int mn_is_wildcard_name(const Avm2AbcFileData* data, uint32_t mn_idx)
{
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
	switch (mn->kind)
	{
		case 0x07: case 0x0d: case 0x09: case 0x0e:
			return mn->name == 0;
		default:
			return 0;
	}
}

static Avm2Value getproperty_common(Avm2Activation* act, Avm2Value recv,
                                    const char* name, uint32_t name_len,
                                    int resolved_ok, const Resolved* r,
                                    int mn_public, int mn_attr, int mn_nsset)
{
	Avm2Context* ctx = act->ctx;
	if (resolved_ok)
	{
		return resolved_get(ctx, recv, r, name, name_len);
	}
	// Miss: dynamic receivers yield undefined for public names; a
	// non-public multiname can never match an expando prop (1081 on a
	// dynamic receiver). Sealed receivers throw 1069 -- except for an
	// ATTRIBUTE multiname, which avmplus routes to 1081 as well
	// (Toplevel: `isAttr() || !containsAnyPublicNamespace()`; as3/Vector/
	// bug_678952 reads `v.@attr` off a sealed Vector and asserts #1081).
	//
	// Do NOT widen this to plain `!mn_public`: a non-public QName miss on a
	// SEALED receiver is #1069, which avm2/catch_class (`AS3::hasOwnProperty`
	// on the catch scope object) and as3/Definitions/Classes/Ext/
	// AccStatPropViaSubClass (`obj.ns1::date`) both pin. That widening was
	// tried in the polish sweep and reverted -- it cost those two to win one.
	//
	// The discriminator is the multiname KIND, not the namespace. Ruffle's
	// script_object.rs get_dynamic_property picks InvalidNsRead (#1081) over
	// InvalidRead (#1069) on `!valid_dynamic_name()` exactly when
	// `has_multiple_ns()`, and HAS_MULTIPLE_NS is set for the ns-SET ABC
	// kinds (Multiname/MultinameA/MultinameL/MultinameLA) and nothing else.
	// So a non-public ns-SET miss is #1081 while a non-public QName miss
	// stays #1069:
	//   as3/RuntimeErrors/Error1081ReadSealedErrorNs reads `a.name` where
	//   `a:IClass`, and ASC compiles that to `mn {ns-set}::name` whose set
	//   holds the single non-public interface namespace `IClass` -- ns-set
	//   form, no public ns => #1081.
	//   catch_class's site is `http://adobe.com/AS3/2006/builtin::hasOwnProperty`
	//   and AccStatPropViaSubClass's is `ns1::date` -- both QNames => #1069.
	int dynamic = recv.kind == AVM2_VALUE_OBJECT && object_is_dynamic(recv.u.obj);
	if (dynamic && mn_public)
	{
		return avm2_undefined();
	}
	char cn[160];
	class_name_of(ctx, recv, cn, sizeof(cn));
	int ns_read = dynamic || mn_attr || (mn_nsset && !mn_public);
	avm2_throw_error(ctx, ctx->builtins.reference_error_class,
	                 "Error #%s: Property %.*s not found on %s and there is "
	                 "no default value.", ns_read ? "1081" : "1069",
	                 (int) name_len, name, cn);
}

static Avm2Value getproperty_static_impl(Avm2Activation* act, Avm2Value recv,
                                         uint32_t mn_idx, Avm2InlineCache* ic)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(act->ctx, recv, name, name_len);
	}
	int xmlish = avm2_value_is_xmlish(recv);
	if (xmlish)
	{
		Avm2Value out;
		if (avm2_xml_get_mn(act->ctx, recv, act->file->data, mn_idx, &out))
		{
			return out;
		}
	}
	if (recv.kind == AVM2_VALUE_OBJECT && recv.u.obj->kind == AVM2_OBJ_VECTOR
	    && avm2_mn_has_public_ns(act->file->data, mn_idx))
	{
		Avm2Value out;
		if (avm2_vector_name_access(act->ctx, recv.u.obj, name, name_len,
		                            &out, avm2_undefined()))
		{
			return out;
		}
	}
	// Populate the per-call-site inline cache for the common monomorphic case:
	// a plain-object receiver whose PRIMARY vtable find hits. XML is excluded —
	// its property set varies with content, so a vtable entry can't stand in.
	// (The vector fast path above already returned for any name it handles, and
	// static multinames never carry numeric-index names, so a reached-here
	// vector always resolves through its vtable — safe to cache.)
	if (ic != NULL && recv.kind == AVM2_VALUE_OBJECT && !xmlish)
	{
		const Avm2VTable* vt = avm2_value_vtable(act->ctx, recv);
		// Never cache the per-call newactivation/newcatch vtables (no_index):
		// they are GC'd, so a freed-then-reused address could false-hit a stale
		// entry. Long-lived class ivtables are never freed → safe to cache.
		if (vt != NULL && !vt->no_index)
		{
			const Avm2PropEntry* e = avm2_vtable_find_mn(vt, act->file->data, mn_idx);
			if (e != NULL)
			{
				ic->vt = vt;
				ic->vt_count = vt->count;
				ic->entry_index = (uint32_t) (e - vt->entries);
#ifndef AVM2_NO_IC_SLOT_INLINE
				ic->slot_plus1 = e->kind == AVM2_PROP_SLOT ? e->slot_index + 1 : 0;
#endif
			}
		}
	}
	Resolved r;
	int ok = resolve_mn(act, recv, mn_idx, &r);
	char dbuf[256];
	if (!ok)
	{
		// Only on the throw path: the qualified spelling costs an snprintf,
		// and every resolved read would pay it (the class_name_of lesson).
		const char* disp = mn_display_name(act->file->data, mn_idx, name, name_len,
		                                   dbuf, sizeof(dbuf));
		if (disp != NULL) { name = disp; name_len = (uint32_t) strlen(disp); }
		if (mn_is_wildcard_name(act->file->data, mn_idx))
		{
			char cn[160];
			class_name_of(act->ctx, recv, cn, sizeof(cn));
			avm2_throw_error(act->ctx, act->ctx->builtins.reference_error_class,
			                 "Error #%s: Property %.*s not found on %s and there "
			                 "is no default value.",
			                 recv.kind == AVM2_VALUE_OBJECT ? "1081" : "1069",
			                 (int) name_len, name, cn);
		}
	}
	return getproperty_common(act, recv, name, name_len, ok, &r,
	                          avm2_mn_has_public_ns(act->file->data, mn_idx),
	                          mn_is_attribute_kind(act->file->data, mn_idx),
	                          mn_is_nsset_kind(act->file->data, mn_idx));
}

Avm2Value avm2_op_getproperty_static(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx)
{
	recv = unbox_scope_prim(recv);
	return getproperty_static_impl(act, recv, mn_idx, NULL);
}

#ifdef AVM2_NO_IC_SLOT_INLINE
Avm2Value avm2_op_getproperty_static_ic(Avm2Activation* act, Avm2Value recv,
                                        uint32_t mn_idx, Avm2InlineCache* ic)
#else
// The inline slot hit lives in avm2_ops.h; this is everything else.
Avm2Value avm2_op_getproperty_static_ic_slow(Avm2Activation* act, Avm2Value recv,
                                             uint32_t mn_idx, Avm2InlineCache* ic)
#endif
{
	recv = unbox_scope_prim(recv);
#ifdef AVM2_HOTLOOP_PROF
	{
		int outcome;
		const Avm2PropEntry* he = NULL;
		if (recv.kind != AVM2_VALUE_OBJECT) outcome = HL_MISS_NONOBJ;
		else
		{
			const Avm2VTable* pvt = avm2_value_vtable(act->ctx, recv);
			if (pvt == NULL) outcome = HL_MISS_NOVT;
			else if (ic->vt == NULL) outcome = HL_MISS_COLD;
			else if (ic->vt != pvt) outcome = HL_MISS_VT;
			else if (ic->vt_count != pvt->count) outcome = HL_MISS_COUNT;
			else { outcome = HL_HIT; he = &pvt->entries[ic->entry_index]; }
		}
		hl_ic_note(act, recv, mn_idx, ic, outcome, he);
	}
#endif
	// Fast path: same receiver vtable (and unchanged entry count) as the cached
	// resolve → replay the resolved entry, skipping the multiname match. A
	// matching vt guarantees the receiver is a non-null, non-xmlish object whose
	// primary find hits this same entry, so this is byte-identical to the full
	// path. `ic->vt != NULL` is implied by `ic->vt == vt` with vt != NULL.
	if (recv.kind == AVM2_VALUE_OBJECT)
	{
		const Avm2VTable* vt = avm2_value_vtable(act->ctx, recv);
		if (vt != NULL && ic->vt == vt && ic->vt_count == vt->count)
		{
			Resolved r = {0};
			r.entry = &vt->entries[ic->entry_index];
			const char* name;
			uint32_t name_len;
			avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
			return resolved_get(act->ctx, recv, &r, name, name_len);
		}
	}
	return getproperty_static_impl(act, recv, mn_idx, ic);
}

#if defined(AVM2_IC_SLOT_VERIFY) && !defined(AVM2_NO_IC_SLOT_INLINE)
void avm2_ic_slot_verify_fail(Avm2Activation* act, uint32_t mn_idx, Avm2Value fast,
                              Avm2Value slow)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	avm2_fatal("AVM2 ic-slot-verify: %.*s — inline slot hit (kind=%u) != "
	           "out-of-line getproperty (kind=%u)", (int) name_len, name,
	           fast.kind, slow.kind);
}
#endif

#ifdef AVM2_SLOT_VERIFY
// Verify build: prove the recompiler's compile-time slot index matches what the
// full runtime resolve produces, for every specialized read that executes. Any
// mismatch (receiver not a plain object, slot out of range, name doesn't resolve
// to a slot, or resolves to a different index) aborts loudly with context.
Avm2Value avm2_op_getproperty_slot(Avm2Activation* act, Avm2Value recv,
                                   uint32_t slot, uint32_t mn_idx)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	if (recv.kind != AVM2_VALUE_OBJECT || recv.u.obj == NULL)
	{
		avm2_fatal("AVM2 slot-verify: receiver of %.*s is not an object (kind=%u)",
		           (int) name_len, name, recv.kind);
	}
	if (slot == 0 || slot >= recv.u.obj->slot_count)
	{
		avm2_fatal("AVM2 slot-verify: slot %u of %.*s out of range (slot_count=%u)",
		           slot, (int) name_len, name, recv.u.obj->slot_count);
	}
	Resolved r;
	int ok = resolve_mn(act, recv, mn_idx, &r);
	if (!ok || r.entry == NULL || r.entry->kind != AVM2_PROP_SLOT)
	{
		char cn[160];
		class_name_of(act->ctx, recv, cn, sizeof(cn));
		avm2_fatal("AVM2 slot-verify: %.*s on %s did not resolve to a slot "
		           "(ok=%d)", (int) name_len, name, cn, ok);
	}
	if (r.entry->slot_index != slot)
	{
		char cn[160];
		class_name_of(act->ctx, recv, cn, sizeof(cn));
		avm2_fatal("AVM2 slot-verify: %.*s on %s — compile-time slot %u != "
		           "runtime slot %u", (int) name_len, name, cn, slot,
		           r.entry->slot_index);
	}
	return recv.u.obj->slots[slot];
}
#endif

static Avm2Value getproperty_qname(Avm2Activation* act, Avm2Value recv,
                                   const Avm2QNameExt* q);

Avm2Value avm2_op_getproperty_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                  Avm2Value name_val, int interp)
{
	recv = unbox_scope_prim(recv);
	Avm2Context* ctx = act->ctx;
	(void) mn_idx;
	{
		// Dictionary receivers key objects by identity — but only on the
		// FAST path (public ns in the set, or an interpreter-mode body):
		// Ruffle op_get_property_fast; the slow path stringifies the name
		// (dictionary_access_no_pubns expects the 1056/1081 miss).
		Avm2Object* dict;
		Avm2Object* key;
		if ((interp || avm2_mn_has_public_ns(act->file->data, mn_idx))
		    && dict_object_key(recv, name_val, &dict, &key))
		{
			Avm2Value* v = avm2_object_find_dynamic_obj(dict, key);
			return (v != NULL) ? *v : avm2_undefined();
		}
		// A QName object as the lazy name resolves by its own uri::local
		// key (Ruffle fill_with_runtime_params — qname_indexing) — but NOT
		// on an XML receiver, where the QName is an E4X name and must reach
		// avm2_e4x_name_from_value below with the site's attribute flag
		// (`x1.bravo.@[q3]`, e4x/QName/e13_3_2 + e4x/Expressions/e11_1_1).
		const Avm2QNameExt* q = avm2_qname_ext_of(name_val);
		if (q != NULL && !avm2_value_is_xmlish(recv))
		{
			return getproperty_qname(act, recv, q);
		}
	}
	if (avm2_value_is_xmlish(recv))
	{
		int attr = mn_is_attribute_kind(act->file->data, mn_idx);
		E4XName n = avm2_e4x_name_from_value(ctx, name_val, attr);
		e4x_name_attach_mn_set(act, mn_idx, &n);
		Avm2Value out;
		if (avm2_xml_get_name(ctx, recv, &n, &out)) return out;
	}
	if (value_is_null_like(recv))
	{
		const Avm2String* ns = avm2_coerce_to_string(ctx, name_val);
		avm2_throw_null_or_undefined(ctx, recv, ns->utf8, ns->len);
	}
	// Array index fast path with a numeric name value (public names only,
	// except interpreter-mode bodies).
	uint32_t idx;
	if (recv.kind == AVM2_VALUE_OBJECT && array_index_access(recv.u.obj)
	    && avm2_value_as_index(name_val, &idx)
	    && (interp || avm2_mn_has_public_ns(act->file->data, mn_idx)))
	{
		Avm2Value v = avm2_array_get(recv.u.obj, idx);
		if (v.kind != AVM2_VALUE_HOLE) return v;
		// A prototype can itself be an Array -- Array.prototype is one -- and
		// then `Array.prototype[3] = x` lands in ELEMENT storage, which the
		// dyn-prop-by-name walk below cannot see (avm2/array_push & co set
		// that up and read it back through a hole in an instance).
		Avm2Value pv;
		if (avm2_array_proto_index(recv.u.obj, idx, &pv)) return pv;
		// fall through (holes may be shadowed by dyn/proto)
	}
	{
		Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(recv);
		if (ba != NULL && avm2_value_as_index(name_val, &idx)
		    && (interp || avm2_mn_has_public_ns(act->file->data, mn_idx)))
		{
			return (idx < ba->len) ? avm2_integer(ba->bytes[idx])
			                       : avm2_undefined();
		}
	}
	const Avm2String* ns = avm2_coerce_to_string(ctx, name_val);
	int mn_public = avm2_mn_has_public_ns(act->file->data, mn_idx);
	if (recv.kind == AVM2_VALUE_OBJECT && recv.u.obj->kind == AVM2_OBJ_VECTOR
	    && (interp || mn_public))
	{
		Avm2Value out;
		if (avm2_vector_name_access(ctx, recv.u.obj, ns->utf8, ns->len,
		                            &out, avm2_undefined()))
		{
			return out;
		}
	}
	{
		// A ns-set multiname matches a trait in ANY namespace of the set, and
		// a TRAIT beats a dynamic property (Ruffle VTable::get_trait runs
		// before the dynamic-prop probe). The set routinely contains the
		// public ns *alongside* the enclosing package/internal ones, so this
		// must not be gated on the set being non-public: `SuperProps` reads
		// an `internal var y` as `this["y"]` from a site whose set holds both,
		// and a public-key-only lookup misses the fixed property entirely.
		const Avm2PropEntry* e = avm2_vtable_find_mn_named(
			avm2_value_vtable(ctx, recv), act->file->data, mn_idx,
			ns->utf8, ns->len);
		if (e != NULL)
		{
			Resolved r2;
			memset(&r2, 0, sizeof(r2));
			r2.entry = e;
			return getproperty_common(act, recv, ns->utf8, ns->len, 1, &r2,
			                          mn_public,
			                          mn_is_attribute_kind(act->file->data, mn_idx),
			                          mn_is_nsset_kind(act->file->data, mn_idx));
		}
	}
	Avm2PropKey key = avm2_public_key(ns->utf8, ns->len);
	Resolved r;
	int ok = resolve_key(ctx, recv, &key, mn_public, &r);
	return getproperty_common(act, recv, ns->utf8, ns->len, ok, &r, mn_public,
	                          mn_is_attribute_kind(act->file->data, mn_idx),
	                          mn_is_nsset_kind(act->file->data, mn_idx));
}

static void setproperty_miss(Avm2Context* ctx, Avm2Value recv,
                             const char* name, uint32_t name_len, Avm2Value value);

// Common write path once resolution is done.
static void setproperty_resolved(Avm2Context* ctx, Avm2Value recv, const Resolved* r,
                                 const char* name, uint32_t name_len, Avm2Value value,
                                 int allow_const)
{
	if (r->is_proxy)
	{
		Avm2Value args[2];
		args[0] = avm2_object_value(
			avm2_qname_new(ctx, r->proxy_uri, r->proxy_local));
		args[1] = value;
		avm2_proxy_call_hook(ctx, recv.u.obj, "setProperty", args, 2);
		return;
	}
	if (r->proto_arr != NULL)
	{
		// Proto-chain element hit: a write shadows on the receiver, exactly
		// as a proto-chain dyn-prop hit does (and #1056 where it cannot).
		// Primitive receivers can reach the proto walk too -- see the dyn
		// branch below.
		if (recv.kind != AVM2_VALUE_OBJECT || !object_is_dynamic(recv.u.obj))
		{
			setproperty_miss(ctx, recv, name, name_len, value);
			return;
		}
		avm2_object_set_dynamic(ctx, recv.u.obj, name, name_len, value);
		return;
	}
	if (r->is_array_elem)
	{
		avm2_array_set(ctx, recv.u.obj, r->arr_index, value);
		return;
	}
	if (r->is_vector_elem)
	{
		avm2_vector_set_index(ctx, recv.u.obj, r->arr_index, value);
		return;
	}
	if (r->is_bytearray_elem)
	{
		Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(recv);
		uint8_t byte = (uint8_t) avm2_coerce_to_u32(ctx, value);
		if (r->arr_index >= ba->len)
		{
			avm2_bytearray_set_length_public(ctx, ba, r->arr_index + 1);
		}
		ba->bytes[r->arr_index] = byte;
		return;
	}
	if (r->dyn != NULL && r->proto_holder == NULL)
	{
		// A builtin `static const` carried in a dynamic prop is ReadOnly, and
		// reports it the same way a const SLOT trait does (below).
		if (r->dyn->read_only && !allow_const)
		{
			char cn[160];
			class_name_of(ctx, recv, cn, sizeof(cn));
			avm2_throw_error(ctx, ctx->builtins.reference_error_class,
			                 "Error #1074: Illegal write to read-only property "
			                 "%.*s on %s.", (int) name_len, name, cn);
		}
		r->dyn->value = value;
		return;
	}
	if (r->dyn != NULL)
	{
		// Proto-chain hit: writes shadow on the receiver itself -- but only
		// where the receiver can actually grow dynamic props. Finding the name
		// on the prototype does not make a sealed receiver writable:
		// as3/Vector/vectorIndexRangeExceptions sets Vector.<*>.prototype.bar
		// and then asserts #1056 for `v['bar'] = x` (while the READ still
		// resolves through the proto chain).
		// A PRIMITIVE receiver reaches here too -- `o:Number = new Number();
		// o.toLocaleString = f` finds the name on Number.prototype and nowhere
		// else -- and is never dynamic, so it must not read recv.u.obj
		// (ecma3/Number/toLocaleString_rt).
		if (recv.kind != AVM2_VALUE_OBJECT || !object_is_dynamic(recv.u.obj))
		{
			setproperty_miss(ctx, recv, name, name_len, value);
			return;
		}
		avm2_object_set_dynamic(ctx, recv.u.obj, name, name_len, value);
		return;
	}
	// `cn` (the receiver's class qname) is only needed on the throw branches;
	// building it eagerly ran an snprintf on EVERY slot write (~6% of Seedling's
	// frame self-time via avm2_class_qname_buf). Compute it lazily per throw.
	const Avm2PropEntry* e = r->entry;
	char cn[160];
	switch (e->kind)
	{
		case AVM2_PROP_SLOT:
		{
			if (e->is_const && !allow_const)
			{
				char pn[192];
				class_name_of(ctx, recv, cn, sizeof(cn));
				avm2_throw_error(ctx, ctx->builtins.reference_error_class,
				                 "Error #1074: Illegal write to read-only property "
				                 "%s on %s.",
				                 prop_name_of(&e->key, name, name_len, pn, sizeof(pn)),
				                 cn);
			}
			Avm2Value cv = value;
			if (e->type_mn != 0 && e->type_file != NULL)
			{
				cv = avm2_coerce_to_type_mn(ctx, e->type_file, e->type_mn, value);
			}
			recv.u.obj->slots[e->slot_index] = cv;
			return;
		}
		case AVM2_PROP_SETTER:
		case AVM2_PROP_GETSET:
			// The setter half may be declared by a different (deeper) class
			// than the getter — see setter_defining_class.
			avm2_call_method_ref(ctx, &e->setter,
			                     e->setter_defining_class != NULL
			                       ? e->setter_defining_class : e->defining_class,
			                     e->setter_defining_class != NULL
			                       ? e->setter_scope : e->method_scope,
			                     recv, &value, 1);
			return;
		case AVM2_PROP_GETTER:
		{
			char pn[192];
			class_name_of(ctx, recv, cn, sizeof(cn));
			avm2_throw_error(ctx, ctx->builtins.reference_error_class,
			                 "Error #1074: Illegal write to read-only property "
			                 "%s on %s.",
			                 prop_name_of(&e->key, name, name_len, pn, sizeof(pn)),
			                 cn);
		}
		case AVM2_PROP_METHOD:
		default:
			class_name_of(ctx, recv, cn, sizeof(cn));
			avm2_throw_error(ctx, ctx->builtins.reference_error_class,
			                 "Error #1037: Cannot assign to a method %.*s on %s.",
			                 (int) name_len, name, cn);
	}
}

// `disp` (may be NULL) is the namespace-qualified spelling used in the #1056
// message only -- the dynamic-property store always uses the LOCAL name.
static void setproperty_miss_disp(Avm2Context* ctx, Avm2Value recv,
                                  const char* name, uint32_t name_len,
                                  Avm2Value value,
                                  const Avm2AbcFileData* data, uint32_t mn_idx)
{
	if (recv.kind == AVM2_VALUE_OBJECT && object_is_dynamic(recv.u.obj))
	{
		avm2_object_set_dynamic(ctx, recv.u.obj, name, name_len, value);
		return;
	}
	char cn[160];
	class_name_of(ctx, recv, cn, sizeof(cn));
	char dbuf[256];
	const char* disp = (data != NULL)
		? mn_display_name(data, mn_idx, name, name_len, dbuf, sizeof(dbuf)) : NULL;
	if (disp != NULL) { name = disp; name_len = (uint32_t) strlen(disp); }
	avm2_throw_error(ctx, ctx->builtins.reference_error_class,
	                 "Error #1056: Cannot create property %.*s on %s.",
	                 (int) name_len, name, cn);
}

static void setproperty_miss(Avm2Context* ctx, Avm2Value recv,
                             const char* name, uint32_t name_len, Avm2Value value)
{
	setproperty_miss_disp(ctx, recv, name, name_len, value, NULL, 0);
}

static void setproperty_impl(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                             Avm2Value value, int allow_const, Avm2InlineCache* ic)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(act->ctx, recv, name, name_len);
	}
	if (avm2_value_is_xmlish(recv)
	    && avm2_xml_set_mn(act->ctx, recv, act->file->data, mn_idx, value))
	{
		return;
	}
	if (recv.kind == AVM2_VALUE_OBJECT && recv.u.obj->kind == AVM2_OBJ_VECTOR
	    && avm2_mn_has_public_ns(act->file->data, mn_idx)
	    && avm2_vector_name_access(act->ctx, recv.u.obj, name, name_len,
	                               NULL, value))
	{
		return;
	}
	// Populate the per-call-site inline cache (SetPropertyStatic only, via the
	// _ic entry point; ic == NULL for initproperty/allow_const). Same invariants
	// as getproperty: plain-object receiver whose PRIMARY vtable find hits, XML
	// and no_index vtables excluded. A static setproperty name is never numeric,
	// so the vector name-access fast path above is a no-op for it and the
	// reached-here resolve goes through the vtable → a matching entry replays
	// byte-identically.
	if (ic != NULL && recv.kind == AVM2_VALUE_OBJECT && !avm2_value_is_xmlish(recv))
	{
		const Avm2VTable* vt = avm2_value_vtable(act->ctx, recv);
		if (vt != NULL && !vt->no_index)
		{
			const Avm2PropEntry* e = avm2_vtable_find_mn(vt, act->file->data, mn_idx);
			if (e != NULL)
			{
				ic->vt = vt;
				ic->vt_count = vt->count;
				ic->entry_index = (uint32_t) (e - vt->entries);
			}
		}
	}
	Resolved r;
	if (resolve_mn(act, recv, mn_idx, &r))
	{
		// Proto-chain dynamic hits shadow onto the receiver; array holes
		// resolved as miss go to setproperty_miss (below) — resolve_mn
		// treats in-range holes as unresolved, but writes must still land
		// in storage, so handle arrays first.
		setproperty_resolved(act->ctx, recv, &r, name, name_len, value, allow_const);
		return;
	}
	uint32_t idx;
	if (recv.kind == AVM2_VALUE_OBJECT && array_index_access(recv.u.obj)
	    && name_as_index(name, name_len, &idx))
	{
		avm2_array_set(act->ctx, recv.u.obj, idx, value);
		return;
	}
	setproperty_miss_disp(act->ctx, recv, name, name_len, value,
	                      act->file->data, mn_idx);
}

void avm2_op_setproperty_static(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                Avm2Value value)
{
	recv = unbox_scope_prim(recv);
	setproperty_impl(act, recv, mn_idx, value, 0, NULL);
}

void avm2_op_setproperty_static_ic(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                   Avm2Value value, Avm2InlineCache* ic)
{
	recv = unbox_scope_prim(recv);
	// Fast path: cached receiver vtable (unchanged count) → replay the resolved
	// slot/setter entry directly. A matching vt is a non-null, non-xmlish object
	// whose primary find hits this entry, so this is byte-identical to the full
	// setproperty path (allow_const == 0, as for the non-cached static op).
	if (recv.kind == AVM2_VALUE_OBJECT)
	{
		const Avm2VTable* vt = avm2_value_vtable(act->ctx, recv);
		if (vt != NULL && ic->vt == vt && ic->vt_count == vt->count)
		{
			Resolved r = {0};
			r.entry = &vt->entries[ic->entry_index];
			const char* name;
			uint32_t name_len;
			avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
			setproperty_resolved(act->ctx, recv, &r, name, name_len, value, 0);
			return;
		}
	}
	setproperty_impl(act, recv, mn_idx, value, 0, ic);
}

void avm2_op_initproperty(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx, Avm2Value val)
{
	setproperty_impl(act, recv, mn_idx, val, 1, NULL);
}

// Non-object receiver of a compile-time slot-bound store (avm2_ops.h): route
// through the full generic path for the exact throw/no-op semantics.
void avm2_setproperty_slot_fallback(Avm2Activation* act, Avm2Value recv,
                                    uint32_t mn_idx, Avm2Value value, int is_init)
{
	setproperty_impl(act, recv, mn_idx, value, is_init ? 1 : 0, NULL);
}

#ifdef AVM2_SET_VERIFY
// Verify build for the recompiler's store-path slot specialization: resolve
// the multiname on the live receiver, prove the target is the compile-time
// slot, compute the reference coerced value from the RESOLVED entry's
// declared type, compare it against what the specialized path would store,
// and only then perform the (single) store. "Store both ways" is not an
// option — the first store would mask a divergent second one.
static int avm2_values_bitident(Avm2Value a, Avm2Value b)
{
	if (a.kind != b.kind) return 0;
	switch (a.kind)
	{
		case AVM2_VALUE_NUMBER:
		{
			uint64_t ab, bb;
			memcpy(&ab, &a.u.d, 8);
			memcpy(&bb, &b.u.d, 8);
			return ab == bb;
		}
		case AVM2_VALUE_INTEGER: return a.u.i == b.u.i;
		case AVM2_VALUE_BOOL: return a.u.b == b.u.b;
		case AVM2_VALUE_STRING: return a.u.str == b.u.str;
		case AVM2_VALUE_OBJECT: return a.u.obj == b.u.obj;
		default: return 1;   // null/undefined carry no payload
	}
}

static void setproperty_slot_verify(Avm2Activation* act, Avm2Value recv, uint32_t slot,
                                    uint32_t mn_idx, int has_type, uint32_t type_mn,
                                    Avm2Value value, int is_init)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	if (recv.kind != AVM2_VALUE_OBJECT || recv.u.obj == NULL)
	{
		// The specialized inline would take the generic fallback here.
		avm2_setproperty_slot_fallback(act, recv, mn_idx, value, is_init);
		return;
	}
	Resolved r;
	int ok = resolve_mn(act, recv, mn_idx, &r);
	if (!ok || r.entry == NULL || r.entry->kind != AVM2_PROP_SLOT)
	{
		avm2_fatal("[AVM2_SET_VERIFY] %.*s store did not resolve to a slot "
		           "(ok=%d)", (int) name_len, name, ok);
	}
	if (r.entry->slot_index != slot)
	{
		avm2_fatal("[AVM2_SET_VERIFY] %.*s: compile-time slot %u != runtime "
		           "slot %u", (int) name_len, name, slot, r.entry->slot_index);
	}
	if (r.entry->is_const && !is_init)
	{
		avm2_fatal("[AVM2_SET_VERIFY] %.*s: specialized SET of a const slot",
		           (int) name_len, name);
	}
	// Reference coerced value from the RESOLVED entry (what the generic
	// path would store).
	Avm2Value ref = value;
	if (r.entry->type_mn != 0 && r.entry->type_file != NULL)
	{
		ref = avm2_coerce_to_type_mn(act->ctx, r.entry->type_file,
		                             r.entry->type_mn, value);
	}
	// What the specialized path would store.
	Avm2Value ours = has_type
		? avm2_coerce_to_type_mn(act->ctx, act->file, type_mn, value)
		: value;
	if (!avm2_values_bitident(ref, ours))
	{
		avm2_fatal("[AVM2_SET_VERIFY] %.*s: coerced value mismatch "
		           "(ref kind=%u vs spec kind=%u, has_type=%d) — store-coerce "
		           "elision/type unsound at this site", (int) name_len, name,
		           ref.kind, ours.kind, has_type);
	}
	recv.u.obj->slots[slot] = ref;
}

void avm2_op_setproperty_slot(Avm2Activation* act, Avm2Value recv, uint32_t slot,
                              uint32_t mn_idx, Avm2Value value, int is_init)
{
	setproperty_slot_verify(act, recv, slot, mn_idx, 0, 0, value, is_init);
}

void avm2_op_setproperty_slot_c(Avm2Activation* act, Avm2Value recv, uint32_t slot,
                                uint32_t mn_idx, uint32_t type_mn, Avm2Value value,
                                int is_init)
{
	setproperty_slot_verify(act, recv, slot, mn_idx, 1, type_mn, value, is_init);
}
#endif

static void setproperty_qname(Avm2Activation* act, Avm2Value recv,
                              const Avm2QNameExt* q, Avm2Value value);

// Shared body of the runtime-named store ops. `allow_const` is the only
// difference between SetProperty and InitProperty (an init may write a const
// slot / a read-only trait — that is what the *Property init form is FOR).
static void setproperty_dyn_impl(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                 Avm2Value name_val, Avm2Value value, int interp,
                                 int allow_const)
{
	Avm2Context* ctx = act->ctx;
	{
		// FAST-path-only, like the get side (dictionary_access_no_pubns).
		Avm2Object* dict;
		Avm2Object* key;
		if ((interp || avm2_mn_has_public_ns(act->file->data, mn_idx))
		    && dict_object_key(recv, name_val, &dict, &key))
		{
			avm2_object_set_dynamic_obj(ctx, dict, key, value);
			return;
		}
		const Avm2QNameExt* q = avm2_qname_ext_of(name_val);
		if (q != NULL)
		{
			setproperty_qname(act, recv, q, value);
			return;
		}
	}
	if (avm2_value_is_xmlish(recv))
	{
		int attr = mn_is_attribute_kind(act->file->data, mn_idx);
		E4XName n = avm2_e4x_name_from_value(ctx, name_val, attr);
		e4x_name_attach_mn_set(act, mn_idx, &n);
		if (avm2_xml_set_name(ctx, recv, &n, value)) return;
	}
	if (value_is_null_like(recv))
	{
		const Avm2String* ns = avm2_coerce_to_string(ctx, name_val);
		avm2_throw_null_or_undefined(ctx, recv, ns->utf8, ns->len);
	}
	uint32_t idx;
	if (recv.kind == AVM2_VALUE_OBJECT && array_index_access(recv.u.obj)
	    && avm2_value_as_index(name_val, &idx)
	    && (interp || avm2_mn_has_public_ns(act->file->data, mn_idx)))
	{
		avm2_array_set(ctx, recv.u.obj, idx, value);
		return;
	}
	{
		Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(recv);
		if (ba != NULL && avm2_value_as_index(name_val, &idx)
		    && (interp || avm2_mn_has_public_ns(act->file->data, mn_idx)))
		{
			uint8_t byte = (uint8_t) avm2_coerce_to_u32(ctx, value);
			if (idx >= ba->len)
			{
				avm2_bytearray_set_length_public(ctx, ba, idx + 1);
			}
			ba->bytes[idx] = byte;
			return;
		}
	}
	const Avm2String* ns = avm2_coerce_to_string(ctx, name_val);
	int mn_public = avm2_mn_has_public_ns(act->file->data, mn_idx);
	if (recv.kind == AVM2_VALUE_OBJECT && recv.u.obj->kind == AVM2_OBJ_VECTOR
	    && (interp || mn_public)
	    && avm2_vector_name_access(ctx, recv.u.obj, ns->utf8, ns->len,
	                               NULL, value))
	{
		return;
	}
	{
		// Same ns-set-wide trait rule as the read side above: a write through
		// `obj[expr]` must hit the fixed property when one of the site's
		// namespaces declares it, rather than minting an expando.
		const Avm2PropEntry* e = avm2_vtable_find_mn_named(
			avm2_value_vtable(ctx, recv), act->file->data, mn_idx,
			ns->utf8, ns->len);
		if (e != NULL)
		{
			Resolved r2;
			memset(&r2, 0, sizeof(r2));
			r2.entry = e;
			setproperty_resolved(ctx, recv, &r2, ns->utf8, ns->len, value, allow_const);
			return;
		}
	}
	Avm2PropKey key = avm2_public_key(ns->utf8, ns->len);
	Resolved r;
	if (resolve_key(ctx, recv, &key, mn_public, &r))
	{
		setproperty_resolved(ctx, recv, &r, ns->utf8, ns->len, value, allow_const);
		return;
	}
	if (mn_public
	    && recv.kind == AVM2_VALUE_OBJECT && array_index_access(recv.u.obj)
	    && name_as_index(ns->utf8, ns->len, &idx))
	{
		avm2_array_set(ctx, recv.u.obj, idx, value);
		return;
	}
	if (!mn_public && recv.kind == AVM2_VALUE_OBJECT)
	{
		// Expando props are public-only: a non-public name can't create one.
		char cn[160];
		class_name_of(ctx, recv, cn, sizeof(cn));
		avm2_throw_error(ctx, ctx->builtins.reference_error_class,
		                 "Error #1056: Cannot create property %.*s on %s.",
		                 (int) ns->len, ns->utf8, cn);
	}
	setproperty_miss(ctx, recv, ns->utf8, ns->len, value);
}

void avm2_op_setproperty_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                             Avm2Value name_val, Avm2Value value, int interp)
{
	recv = unbox_scope_prim(recv);
	setproperty_dyn_impl(act, recv, mn_idx, name_val, value, interp, 0);
}

// InitProperty with a runtime name (MultinameL). ASC emits this for
// `this[expr] = v` inside a constructor and, in `misc/bug_508617` /
// `as3/Definitions/Super/SuperProps`, for the class-init store of a
// namespace-set-qualified slot whose local name is computed.
void avm2_op_initproperty_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                              Avm2Value name_val, Avm2Value value, int interp)
{
	setproperty_dyn_impl(act, recv, mn_idx, name_val, value, interp, 1);
}

// Declared trait visible to `delete` on this receiver. A class object's own
// `vtable` is its STATIC vtable, so Class's instance members (notably the
// `prototype` getter) need the same class_class fallback the resolve paths use
// — without it `delete(Number.prototype)` misses the trait and reports true
// (ecma3/Number/e15_7_3_1_1).
static const Avm2PropEntry* delete_trait_find(Avm2Context* ctx, Avm2Value recv,
                                              const Avm2VTable* vt,
                                              const Avm2PropKey* key)
{
	const Avm2PropEntry* e = avm2_vtable_find(vt, key);
	if (e == NULL && recv.kind == AVM2_VALUE_OBJECT
	    && recv.u.obj->kind == AVM2_OBJ_CLASS
	    && ctx->builtins.class_class != NULL)
	{
		e = avm2_vtable_find(&ctx->builtins.class_class->ivtable, key);
	}
	return e;
}

static Avm2Value deleteproperty_common(Avm2Activation* act, Avm2Value recv,
                                       const char* name, uint32_t name_len)
{
	Avm2Context* ctx = act->ctx;
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, name, name_len);
	}
	if (recv.kind != AVM2_VALUE_OBJECT)
	{
		// `delete` against a PRIMITIVE receiver throws; only objects ever
		// answer true/false (Ruffle value.rs Value::delete_property — the
		// non-Object arm is unconditionally ReferenceErrorCode::InvalidDelete).
		// `new String()` yields a String *value*, not an object, so
		// ecma3/String/e15_5_5_1's `delete s.length` lands here too.
		char cn[160];
		class_name_of(ctx, recv, cn, sizeof(cn));
		avm2_throw_error(ctx, ctx->builtins.reference_error_class,
		                 "Error #1120: Cannot delete property %.*s on %s.",
		                 (int) name_len, name, cn);
	}
	Avm2Object* obj = recv.u.obj;
	uint32_t idx;
	if (array_index_access(obj) && name_as_index(name, name_len, &idx))
	{
		avm2_array_delete(obj, idx);
		return avm2_bool(true);
	}
	if (obj->kind == AVM2_OBJ_VECTOR)
	{
		// FP never deletes vector elements, and reports true for them -- but a
		// name that resolves to a declared trait is still undeletable, same as
		// on any other object: as3/Vector/initializer_expressions asserts
		// `delete (new <int>[1,2,3]).length` === false.
		Avm2PropKey vkey = avm2_public_key(name, name_len);
		if (delete_trait_find(ctx, recv, obj->vtable, &vkey) != NULL)
		{
			return avm2_bool(false);
		}
		return avm2_bool(true);
	}
	// Declared traits can't be deleted.
	Avm2PropKey key = avm2_public_key(name, name_len);
	if (delete_trait_find(ctx, recv, obj->vtable, &key) != NULL)
	{
		return avm2_bool(false);
	}
	// -1 = present but DontDelete (a builtin `static const`),
	// 0 = no such property, 1 = removed.
	int removed = avm2_object_delete_dynamic(obj, name, name_len);
	if (removed < 0)
	{
		return avm2_bool(false);
	}
	if (removed == 0 && !object_is_dynamic(obj))
	{
		// A sealed (non-dynamic) class instance could never have held the
		// property, and avmplus reports that as false rather than the ES3
		// missing-property `true` (as3/ShellClasses/DictionarySubclass
		// "delete literal key from SealedDictionary" -- the write that would
		// have created it threw #1056 -- and regress/bug_654807 "delprop").
		// Dynamic receivers, prototypes, globals and activations keep the
		// ES3 answer; Dictionary object-space keys never reach here.
		return avm2_bool(false);
	}
	// Deleting a missing property returns true (ES3), same as a successful one.
	return avm2_bool(true);
}

Avm2Value avm2_op_deleteproperty(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx)
{
	Avm2Context* ctx = act->ctx;
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	if (avm2_value_is_xmlish(recv))
	{
		Avm2Value out;
		if (avm2_xml_delete_mn(ctx, recv, act->file->data, mn_idx, &out))
		{
			return out;
		}
	}
	if (recv.kind == AVM2_VALUE_OBJECT && avm2_is_proxy(recv.u.obj))
	{
		// Declared traits (in ANY ns of the multiname) can't be deleted;
		// everything else routes to the deleteProperty hook.
		if (avm2_vtable_find_mn(recv.u.obj->vtable, act->file->data, mn_idx))
		{
			return avm2_bool(false);
		}
		const Avm2String* uri;
		const Avm2String* local;
		mn_qname_parts(ctx, act->file->data, mn_idx, &uri, &local);
		Avm2Value qn = avm2_object_value(avm2_qname_new(ctx, uri, local));
		Avm2Value v = avm2_proxy_call_hook(ctx, recv.u.obj, "deleteProperty",
		                                   &qn, 1);
		return avm2_bool(avm2_coerce_to_boolean(v));
	}
	return deleteproperty_common(act, recv, name, name_len);
}

static Avm2Value deleteproperty_qname(Avm2Activation* act, Avm2Value recv,
                                      const Avm2QNameExt* q);

Avm2Value avm2_op_deleteproperty_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                     Avm2Value name_val)
{
	(void) mn_idx;
	// ECMA-357 11.3.1: `delete` with an XMLList operand is a TypeError. The
	// operand is the RUNTIME NAME, not the receiver — `delete
	// books.book.(@publisher == "Adobe")` compiles to a filter that leaves an
	// XMLList on the stack as the property name (Ruffle activation.rs
	// op_delete_property, the has_lazy_name arm).
	if (avm2_xmllist_ext_of(name_val) != NULL)
	{
		avm2_throw_error(act->ctx, act->ctx->builtins.type_error_class,
		                 "Error #1119: Delete operator is not supported with "
		                 "operand of type XMLList.");
	}
	{
		Avm2Object* dict;
		Avm2Object* key;
		if (dict_object_key(recv, name_val, &dict, &key))
		{
			avm2_object_delete_dynamic_obj(dict, key);
			return avm2_bool(true);
		}
		const Avm2QNameExt* q = avm2_qname_ext_of(name_val);
		if (q != NULL) return deleteproperty_qname(act, recv, q);
	}
	if (avm2_value_is_xmlish(recv))
	{
		Avm2Context* xctx = act->ctx;
		int attr = mn_is_attribute_kind(act->file->data, mn_idx);
		E4XName n = avm2_e4x_name_from_value(xctx, name_val, attr);
		e4x_name_attach_mn_set(act, mn_idx, &n);
		Avm2Value out;
		if (avm2_xml_delete_name(xctx, recv, &n, &out)) return out;
	}
	if (recv.kind == AVM2_VALUE_OBJECT && avm2_is_proxy(recv.u.obj))
	{
		Avm2Context* dctx = act->ctx;
		const Avm2String* nm = avm2_coerce_to_string(dctx, name_val);
		if (avm2_vtable_find_mn_named(recv.u.obj->vtable, act->file->data,
		                              mn_idx, nm->utf8, nm->len))
		{
			return avm2_bool(false);
		}
		const Avm2String* uri;
		const Avm2String* local;
		mn_qname_parts(dctx, act->file->data, mn_idx, &uri, &local);
		Avm2Value qn = avm2_object_value(avm2_qname_new(dctx, uri, nm));
		Avm2Value v = avm2_proxy_call_hook(dctx, recv.u.obj, "deleteProperty",
		                                   &qn, 1);
		return avm2_bool(avm2_coerce_to_boolean(v));
	}
	uint32_t idx;
	if (recv.kind == AVM2_VALUE_OBJECT && recv.u.obj != NULL
	    && array_index_access(recv.u.obj) && avm2_value_as_index(name_val, &idx))
	{
		avm2_array_delete(recv.u.obj, idx);
		return avm2_bool(true);
	}
	const Avm2String* ns = avm2_coerce_to_string(act->ctx, name_val);
	return deleteproperty_common(act, recv, ns->utf8, ns->len);
}

// ---------------------------------------------------------------------------
// Lazy-namespace (RTQName / RTQNameL) property ops + QName-valued lazy names
// ---------------------------------------------------------------------------

// Key from a popped Namespace value. Returns whether the namespace is the
// public (empty-URI) one, i.e. whether dynamic props are reachable.
static int key_from_ns_value(Avm2Context* ctx, Avm2Value ns_val,
                             const char* name, uint32_t name_len,
                             Avm2PropKey* key)
{
	Avm2NamespaceExt* n = avm2_namespace_ext_of(ns_val);
	if (n == NULL)
	{
		// The RTQName/RTQNameL namespace operand is verified LATE: avmplus
		// coerces it to Namespace at the op and reports a catchable
		// VerifyError #1058 when it isn't one (rtqname_not_namespace catches
		// it and prints e.name / e.errorID). It is not a fatal.
		char cn[160];
		class_name_of(ctx, ns_val, cn, sizeof(cn));
		avm2_throw_error(ctx, ctx->builtins.verify_error_class,
		                 "Error #1058: Illegal operand type: %s must be Namespace.",
		                 cn);
	}
	key->name = name;
	key->name_len = name_len;
	key->ns_kind = (n->kind != 0) ? n->kind : 0x16;
	key->ns_priv = NULL;
	key->ns_uri = n->uri->utf8;
	key->ns_len = n->uri->len;
	return avm2_propkey_is_public(key);
}

// Key from a QName object used as a lazy name. `*any_ns` reports the
// any-namespace QName (name-only matching).
static int key_from_qname_ext(const Avm2QNameExt* q, Avm2PropKey* key, int* any_ns)
{
	const char* name = (q->local != NULL) ? q->local->utf8 : "*";
	uint32_t name_len = (q->local != NULL) ? q->local->len : 1;
	*any_ns = (q->uri == NULL);
	if (q->uri == NULL)
	{
		*key = avm2_public_key(name, name_len);
		return 1;
	}
	key->name = name;
	key->name_len = name_len;
	key->ns_kind = 0x16;
	key->ns_priv = NULL;
	key->ns_uri = q->uri->utf8;
	key->ns_len = q->uri->len;
	return avm2_propkey_is_public(key);
}

// resolve_key plus the any-namespace fallback: a name-only vtable scan.
static int resolve_generic_key(Avm2Context* ctx, Avm2Value recv, const Avm2PropKey* key,
                               int public_ok, int any_ns, Resolved* out)
{
	if (resolve_key(ctx, recv, key, public_ok, out)) return 1;
	if (any_ns)
	{
		const Avm2VTable* vt = avm2_value_vtable(ctx, recv);
		if (vt != NULL)
		{
			for (uint32_t i = 0; i < vt->count; i++)
			{
				if (vt->entries[i].key.name_len == key->name_len
				    && memcmp(vt->entries[i].key.name, key->name, key->name_len) == 0)
				{
					memset(out, 0, sizeof(*out));
					out->entry = &vt->entries[i];
					return 1;
				}
			}
		}
	}
	return 0;
}

static Avm2Value getproperty_qname(Avm2Activation* act, Avm2Value recv,
                                   const Avm2QNameExt* q)
{
	Avm2Context* ctx = act->ctx;
	if (avm2_value_is_xmlish(recv))
	{
		E4XName n = e4x_name_from_qext(q, 0);
		Avm2Value out;
		if (avm2_xml_get_name(ctx, recv, &n, &out)) return out;
	}
	Avm2PropKey key;
	int any_ns;
	int pub = key_from_qname_ext(q, &key, &any_ns);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, key.name, key.name_len);
	}
	Resolved r;
	int ok = resolve_generic_key(ctx, recv, &key, pub, any_ns, &r);
	// A runtime QName is a QName: Ruffle sets IS_QNAME, never HAS_MULTIPLE_NS.
	return getproperty_common(act, recv, key.name, key.name_len, ok, &r, pub, 0, 0);
}

static void setproperty_nonpublic_miss(Avm2Context* ctx, Avm2Value recv,
                                       const char* name, uint32_t name_len)
{
	char cn[160];
	class_name_of(ctx, recv, cn, sizeof(cn));
	avm2_throw_error(ctx, ctx->builtins.reference_error_class,
	                 "Error #1056: Cannot create property %.*s on %s.",
	                 (int) name_len, name, cn);
}

static void setproperty_qname(Avm2Activation* act, Avm2Value recv,
                              const Avm2QNameExt* q, Avm2Value value)
{
	Avm2Context* ctx = act->ctx;
	if (avm2_value_is_xmlish(recv))
	{
		E4XName n = e4x_name_from_qext(q, 0);
		if (avm2_xml_set_name(ctx, recv, &n, value)) return;
	}
	Avm2PropKey key;
	int any_ns;
	int pub = key_from_qname_ext(q, &key, &any_ns);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, key.name, key.name_len);
	}
	Resolved r;
	if (resolve_generic_key(ctx, recv, &key, pub, any_ns, &r))
	{
		setproperty_resolved(ctx, recv, &r, key.name, key.name_len, value, 0);
		return;
	}
	if (!pub && recv.kind == AVM2_VALUE_OBJECT)
	{
		setproperty_nonpublic_miss(ctx, recv, key.name, key.name_len);
	}
	setproperty_miss(ctx, recv, key.name, key.name_len, value);
}

static Avm2Value deleteproperty_qname(Avm2Activation* act, Avm2Value recv,
                                      const Avm2QNameExt* q)
{
	Avm2Context* ctx = act->ctx;
	if (avm2_value_is_xmlish(recv))
	{
		E4XName n = e4x_name_from_qext(q, 0);
		Avm2Value out;
		if (avm2_xml_delete_name(ctx, recv, &n, &out)) return out;
	}
	Avm2PropKey key;
	int any_ns;
	int pub = key_from_qname_ext(q, &key, &any_ns);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, key.name, key.name_len);
	}
	if (recv.kind != AVM2_VALUE_OBJECT) return avm2_bool(false);
	Avm2Object* obj = recv.u.obj;
	Resolved r;
	if (resolve_generic_key(ctx, recv, &key, 0, any_ns, &r))
	{
		if (r.is_proxy)
		{
			Avm2Value qn = avm2_object_value(
				avm2_qname_new(ctx, q->uri, q->local));
			Avm2Value v = avm2_proxy_call_hook(ctx, obj, "deleteProperty",
			                                   &qn, 1);
			return avm2_bool(avm2_coerce_to_boolean(v));
		}
		return avm2_bool(false);  // declared traits can't be deleted
	}
	if (pub && avm2_object_delete_dynamic(obj, key.name, key.name_len) < 0)
	{
		return avm2_bool(false);  // present but DontDelete (`static const`)
	}
	return avm2_bool(true);  // deleting a missing property returns true
}

Avm2Value avm2_op_pushnamespace(Avm2Activation* act, uint32_t ns_idx)
{
	return avm2_object_value(avm2_namespace_from_pool(act->ctx, act->file, ns_idx));
}

Avm2Value avm2_op_getproperty_rtns(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                   Avm2Value ns_val)
{
	recv = unbox_scope_prim(recv);
	Avm2Context* ctx = act->ctx;
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, name, name_len);
	}
	if (avm2_value_is_xmlish(recv))
	{
		E4XName n = e4x_name_from_rtns(
			ctx, ns_val,
			act->file->data->multinames[mn_idx].name != 0
				? avm2_string_new(ctx, name, name_len) : NULL,
			mn_is_attribute_kind(act->file->data, mn_idx));
		Avm2Value out;
		if (avm2_xml_get_name(ctx, recv, &n, &out)) return out;
	}
	Avm2PropKey key;
	int pub = key_from_ns_value(act->ctx, ns_val, name, name_len, &key);
	Resolved r;
	int ok = resolve_key(ctx, recv, &key, pub, &r);
	return getproperty_common(act, recv, name, name_len, ok, &r, pub, 0, 0);
}

Avm2Value avm2_op_getproperty_rtns_l(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                     Avm2Value ns_val, Avm2Value name_val)
{
	recv = unbox_scope_prim(recv);
	Avm2Context* ctx = act->ctx;
	const Avm2QNameExt* q = avm2_qname_ext_of(name_val);
	if (q != NULL)
	{
		// A QName name overrides the popped namespace entirely.
		return getproperty_qname(act, recv, q);
	}
	const Avm2String* s = avm2_coerce_to_string(ctx, name_val);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, s->utf8, s->len);
	}
	if (avm2_value_is_xmlish(recv))
	{
		E4XName n = e4x_name_from_rtns(ctx, ns_val, s,
		                               mn_is_attribute_kind(act->file->data, mn_idx));
		Avm2Value out;
		if (avm2_xml_get_name(ctx, recv, &n, &out)) return out;
	}
	Avm2PropKey key;
	int pub = key_from_ns_value(act->ctx, ns_val, s->utf8, s->len, &key);
	Resolved r;
	int ok = resolve_key(ctx, recv, &key, pub, &r);
	return getproperty_common(act, recv, s->utf8, s->len, ok, &r, pub, 0, 0);
}

// `attr` / `any_name` are the two facts the READ path already derives from
// the multiname (attribute kind, name index 0). The write and delete paths
// used to hard-code 0 / non-NULL, so `x1.@ns::['v'] = '555'` built a plain
// element name and CREATED a `<ns:v>` child instead of setting the
// attribute (e4x/Expressions/e11_1_2).
static void setproperty_rtns_common(Avm2Activation* act, Avm2Value recv,
                                    Avm2Value ns_val, const char* name,
                                    uint32_t name_len, Avm2Value value,
                                    int attr, int any_name)
{
	Avm2Context* ctx = act->ctx;
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, name, name_len);
	}
	if (avm2_value_is_xmlish(recv))
	{
		E4XName n = e4x_name_from_rtns(
			ctx, ns_val,
			any_name ? NULL : avm2_string_new(ctx, name, name_len), attr);
		if (avm2_xml_set_name(ctx, recv, &n, value)) return;
	}
	Avm2PropKey key;
	int pub = key_from_ns_value(act->ctx, ns_val, name, name_len, &key);
	Resolved r;
	if (resolve_key(ctx, recv, &key, pub, &r))
	{
		setproperty_resolved(ctx, recv, &r, name, name_len, value, 0);
		return;
	}
	if (!pub && recv.kind == AVM2_VALUE_OBJECT)
	{
		setproperty_nonpublic_miss(ctx, recv, name, name_len);
	}
	setproperty_miss(ctx, recv, name, name_len, value);
}

void avm2_op_setproperty_rtns(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                              Avm2Value ns_val, Avm2Value value)
{
	recv = unbox_scope_prim(recv);
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	setproperty_rtns_common(act, recv, ns_val, name, name_len, value,
	                        mn_is_attribute_kind(act->file->data, mn_idx),
	                        act->file->data->multinames[mn_idx].name == 0);
}

void avm2_op_setproperty_rtns_l(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                Avm2Value ns_val, Avm2Value name_val, Avm2Value value)
{
	recv = unbox_scope_prim(recv);
	const Avm2QNameExt* q = avm2_qname_ext_of(name_val);
	if (q != NULL)
	{
		setproperty_qname(act, recv, q, value);
		return;
	}
	const Avm2String* s = avm2_coerce_to_string(act->ctx, name_val);
	setproperty_rtns_common(act, recv, ns_val, s->utf8, s->len, value,
	                        mn_is_attribute_kind(act->file->data, mn_idx), 0);
}

static Avm2Value deleteproperty_rtns_common(Avm2Activation* act, Avm2Value recv,
                                            Avm2Value ns_val, const char* name,
                                            uint32_t name_len, int attr,
                                            int any_name)
{
	Avm2Context* ctx = act->ctx;
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, name, name_len);
	}
	if (avm2_value_is_xmlish(recv))
	{
		E4XName n = e4x_name_from_rtns(
			ctx, ns_val,
			any_name ? NULL : avm2_string_new(ctx, name, name_len), attr);
		Avm2Value out;
		if (avm2_xml_delete_name(ctx, recv, &n, &out)) return out;
	}
	if (recv.kind != AVM2_VALUE_OBJECT) return avm2_bool(false);
	Avm2PropKey key;
	int pub = key_from_ns_value(act->ctx, ns_val, name, name_len, &key);
	const Avm2VTable* vt = avm2_value_vtable(ctx, recv);
	if (delete_trait_find(ctx, recv, vt, &key) != NULL)
	{
		return avm2_bool(false);
	}
	if (avm2_is_proxy(recv.u.obj))
	{
		Avm2Value qn = avm2_object_value(avm2_qname_new(
			ctx, avm2_string_new(ctx, key.ns_uri, key.ns_len),
			avm2_string_new(ctx, name, name_len)));
		Avm2Value v = avm2_proxy_call_hook(ctx, recv.u.obj, "deleteProperty",
		                                   &qn, 1);
		return avm2_bool(avm2_coerce_to_boolean(v));
	}
	if (pub && avm2_object_delete_dynamic(recv.u.obj, name, name_len) < 0)
	{
		return avm2_bool(false);  // present but DontDelete (`static const`)
	}
	return avm2_bool(true);
}

Avm2Value avm2_op_deleteproperty_rtns(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                      Avm2Value ns_val)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	return deleteproperty_rtns_common(
		act, recv, ns_val, name, name_len,
		mn_is_attribute_kind(act->file->data, mn_idx),
		act->file->data->multinames[mn_idx].name == 0);
}

Avm2Value avm2_op_deleteproperty_rtns_l(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                        Avm2Value ns_val, Avm2Value name_val)
{
	const Avm2QNameExt* q = avm2_qname_ext_of(name_val);
	if (q != NULL)
	{
		return deleteproperty_qname(act, recv, q);
	}
	const Avm2String* s = avm2_coerce_to_string(act->ctx, name_val);
	return deleteproperty_rtns_common(
		act, recv, ns_val, s->utf8, s->len,
		mn_is_attribute_kind(act->file->data, mn_idx), 0);
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

static const Avm2PropEntry* find_slot_entry(const Avm2VTable* vt, uint32_t slot_index)
{
	if (vt == NULL) return NULL;
	for (uint32_t i = 0; i < vt->count; i++)
	{
		if (vt->entries[i].kind == AVM2_PROP_SLOT
		    && vt->entries[i].slot_index == slot_index)
		{
			return &vt->entries[i];
		}
	}
	return NULL;
}

// A slot index past the object's slot array. The recompiler's static type
// lattice catches this at verify time whenever it can prove the base's class
// (abc_verifier.cpp, #1026), but the lattice is deliberately partial — it
// defaults to "we don't know" and never throws on an uninferred operand — so
// the op still has to handle the case. avmplus's own #1026 is a VerifyError,
// and a VerifyError is catchable; making this a fatal killed the process
// before any buffered trace output was flushed, losing the whole test.
// Slot arrays carry an unused element 0, so the avmplus-visible slotCount is
// one less than `slot_count`.
_Noreturn static void throw_slot_out_of_range(Avm2Activation* act, Avm2Value objv,
                                              const Avm2Object* obj, uint32_t slot)
{
	char cn[160];
	class_name_of(act->ctx, objv, cn, sizeof(cn));
	uint32_t declared = (obj != NULL && obj->slot_count > 0) ? obj->slot_count - 1 : 0;
	avm2_throw_error(act->ctx, act->ctx->builtins.verify_error_class,
	                 "Error #1026: Slot %u exceeds slotCount=%u of %s.",
	                 slot, declared, cn);
}

Avm2Value avm2_op_getslot(Avm2Activation* act, Avm2Value objv, uint32_t index0)
{
	if (value_is_null_like(objv))
	{
		avm2_throw_null_or_undefined(act->ctx, objv, NULL, 0);
	}
	Avm2Object* obj = objv.u.obj;
	uint32_t slot = index0 + 1;  // IR is 0-based; slot arrays are 1-based
	if (obj == NULL || slot >= obj->slot_count)
	{
		throw_slot_out_of_range(act, objv, obj, slot);
	}
	return obj->slots[slot];
}

void avm2_op_setslot(Avm2Activation* act, Avm2Value objv, uint32_t index0, Avm2Value value)
{
	Avm2Context* ctx = act->ctx;
	if (value_is_null_like(objv))
	{
		avm2_throw_null_or_undefined(ctx, objv, NULL, 0);
	}
	Avm2Object* obj = objv.u.obj;
	uint32_t slot = index0 + 1;
	if (obj == NULL || slot >= obj->slot_count)
	{
		throw_slot_out_of_range(act, objv, obj, slot);
	}
	const Avm2PropEntry* e = find_slot_entry(obj->vtable, slot);
	if (e != NULL && e->type_mn != 0 && e->type_file != NULL)
	{
		value = avm2_coerce_to_type_mn(ctx, e->type_file, e->type_mn, value);
	}
	obj->slots[slot] = value;
}

// ---------------------------------------------------------------------------
// Scopes and name lookup
// ---------------------------------------------------------------------------

Avm2Object* avm2_op_pushscope(Avm2Activation* act, Avm2Value v)
{
	if (v.kind == AVM2_VALUE_OBJECT && v.u.obj != NULL)
	{
		return v.u.obj;
	}
	if (value_is_null_like(v))
	{
		avm2_throw_null_or_undefined(act->ctx, v, NULL, 0);
	}
	// A primitive scope (p-code can push anything): box it — vtable and
	// prototype-chain lookups on the box behave like the primitive
	// (findprop_global_prototype pushes the number 4 as its global scope).
	// The box is TRANSPARENT: it keeps the primitive in slots[1] and every
	// property get/set/call entry point unboxes the receiver, so a method
	// resolved through the box runs with the primitive as `this`
	// (`with (7) x = valueOf()` — ecma3/Statements/e12_10). Ruffle's scope
	// entry holds a Value outright; keeping the box means Avm2ScopeEntry
	// (and ~20 op signatures plus the GetScopeObject emission) stay as they
	// are.
	Avm2Context* ctx = act->ctx;
	Avm2Object* box = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 2);
	box->cls = avm2_value_class(ctx, v);
	box->vtable = &box->cls->ivtable;
	box->proto = box->cls->prototype_obj;
	box->is_prim_box = 1;
	box->slots[1] = v;
	return box;
}

Avm2Object* avm2_op_getglobalscope(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                   uint32_t scope_n)
{
	if (act->outer != NULL && act->outer->count > 0)
	{
		return act->outer->entries[0].obj;
	}
	if (scope_n > 0)
	{
		return lscope[0].obj;
	}
	avm2_fatal("GetGlobalScope with an empty scope stack");
}

Avm2Object* avm2_op_getouterscope(Avm2Activation* act, uint32_t index)
{
	if (act->outer == NULL || index >= act->outer->count)
	{
		avm2_fatal("GetOuterScope %u out of range", index);
	}
	return act->outer->entries[index].obj;
}

// Does a scope object define the multiname? Plain scopes expose declared
// traits only; with-scopes expose everything (Ruffle scope.rs find).
static int scope_defines_mn(Avm2Activation* act, const Avm2ScopeEntry* se, uint32_t mn_idx)
{
	if (se->obj == NULL) return 0;
	if (!se->is_with)
	{
		if (avm2_vtable_find_mn(se->obj->vtable, act->file->data, mn_idx) != NULL)
		{
			return 1;
		}
		// Same fallback resolve_key already applies (see the AVM2_OBJ_CLASS arm
		// there): a class object's own vtable holds only its static traits;
		// `Class`'s instance members come from class_class->ivtable. A class
		// object sits on the scope chain of every one of its methods, so a bare
		// `prototype` inside a static method must find it there
		// (avm2/supercalls_weird). NOTE: class_class is built before Object's
		// hasOwnProperty/isPrototypeOf/propertyIsEnumerable are registered
		// (avm2_globals.c:3432 vs :3471) and avm2_builtin_class copies the super
		// ivtable at creation time, so this ivtable holds exactly one entry,
		// `prototype`. Reordering that init would silently widen this fallback.
		if (se->obj->kind == AVM2_OBJ_CLASS && act->ctx->builtins.class_class != NULL)
		{
			return avm2_vtable_find_mn(&act->ctx->builtins.class_class->ivtable,
			                           act->file->data, mn_idx) != NULL;
		}
		return 0;
	}
	Avm2Value sv = avm2_object_value(se->obj);
	if (avm2_value_is_xmlish(sv))
	{
		// E4X filter loops (with-scope per item): has_property semantics.
		return avm2_xml_has_property_mn(act->ctx, sv, act->file->data, mn_idx);
	}
	Resolved r;
	return resolve_mn(act, sv, mn_idx, &r);
}

// --- Runtime-named multinames (MultinameL / MultinameLA) -------------------
// A MultinameL carries a namespace SET but takes its local name off the
// operand stack, so none of the mn_idx-keyed lookups above can match it. The
// ONLY correct order is to test the whole ns set at EACH scope level, not one
// namespace across the whole chain: `scopes_dont_cache` pins exactly that —
// the set is {outer, inner} and the answer is the inner global purely because
// it sits higher on the scope stack. avm2_vtable_find_mn_named already walks
// the set against a single vtable, which is the per-level primitive.

// Is `mn_idx` a runtime-named multiname (name off the stack, ns set static)?
static int mn_is_lazy_name_set(const Avm2AbcFileData* data, uint32_t mn_idx)
{
	uint8_t k = data->multinames[mn_idx].kind;
	return k == 0x1b || k == 0x1c;   // MultinameL / MultinameLA
}

// scope_defines_mn for a MultinameL: the ns set applies at this one level.
static int scope_defines_named(Avm2Activation* act, const Avm2ScopeEntry* se,
                               uint32_t mn_idx, const char* name, uint32_t name_len)
{
	if (se->obj == NULL) return 0;
	if (!se->is_with)
	{
		if (avm2_vtable_find_mn_named(se->obj->vtable, act->file->data, mn_idx,
		                              name, name_len) != NULL)
		{
			return 1;
		}
		// Class-object fallback, same as scope_defines_mn above (kept in sync
		// deliberately: no corpus consumer today, but leaving the MultinameL
		// half behind is how the next `_dyn`-shaped bug gets written).
		if (se->obj->kind == AVM2_OBJ_CLASS && act->ctx->builtins.class_class != NULL)
		{
			return avm2_vtable_find_mn_named(&act->ctx->builtins.class_class->ivtable,
			                                 act->file->data, mn_idx,
			                                 name, name_len) != NULL;
		}
		return 0;
	}
	Avm2Value sv = avm2_object_value(se->obj);
	if (avm2_value_is_xmlish(sv))
	{
		return avm2_xml_has_property_via_in(
			act->ctx, sv, avm2_string_new(act->ctx, name, name_len));
	}
	// with-scopes expose dynamic props too, so try each namespace in the set
	// as a full key resolve.
	const Avm2AbcFileData* data = act->file->data;
	const Avm2AbcNsSet* set = &data->ns_sets[data->multinames[mn_idx].ns_set];
	for (uint32_t i = 0; i < set->count; i++)
	{
		Avm2PropKey key;
		key.name = name;
		key.name_len = name_len;
		avm2_key_ns_from_abc(&key, data, set->ns_indices[i]);
		Resolved r;
		if (resolve_key(act->ctx, sv, &key, avm2_propkey_is_public(&key), &r)) return 1;
	}
	return 0;
}

static Avm2Object* findproperty_scope_walk_named(Avm2Activation* act,
                                                 const Avm2ScopeEntry* lscope,
                                                 uint32_t scope_n, uint32_t mn_idx,
                                                 const char* name, uint32_t name_len)
{
	for (uint32_t i = scope_n; i > 0; i--)
	{
		if (scope_defines_named(act, &lscope[i - 1], mn_idx, name, name_len))
		{
			return lscope[i - 1].obj;
		}
	}
	if (act->outer != NULL)
	{
		for (uint32_t i = act->outer->count; i > 0; i--)
		{
			if (scope_defines_named(act, &act->outer->entries[i - 1], mn_idx,
			                        name, name_len))
			{
				return act->outer->entries[i - 1].obj;
			}
		}
	}
	return NULL;
}

// Domain phase for a MultinameL: the runtime name against each ns in the set.
static Avm2Object* findproperty_domain_find_named(Avm2Activation* act, uint32_t mn_idx,
                                                  const char* name, uint32_t name_len)
{
	const Avm2AbcFileData* data = act->file->data;
	const Avm2AbcNsSet* set = &data->ns_sets[data->multinames[mn_idx].ns_set];
	for (uint32_t i = 0; i < set->count; i++)
	{
		Avm2PropKey key;
		key.name = name;
		key.name_len = name_len;
		avm2_key_ns_from_abc(&key, data, set->ns_indices[i]);
		Avm2Object* g = avm2_domain_find(act->ctx, act->file->scope, &key);
		if (g != NULL) return g;
	}
	return NULL;
}

// --- FindProperty resolution, split into the three phases so the plain op and
// the per-call-site inline-cached op (avm2_op_findpropstrict_ic) share code. ---

// Scope walk (local scope stack, then captured outer chain), top → bottom.
// Returns the matching scope object or NULL. Plain (non-with) scope entries
// match on declared TRAITS only (avm2_vtable_find_mn) → the hit/miss for a
// given static multiname depends solely on the entry object's vtable (its
// class), so a with-free method's scope-walk result is invariant across
// activations at a fixed call site (the property the inline cache exploits).
// `out_outer_hit` reports whether the hit came from the OUTER chain, and
// `out_prefix_pure` whether every entry examined up to AND including the hit
// was a plain (non-with) scope — the two facts the findpropstrict scope-hit
// inline cache needs (with-scope matches are content-dependent, so any with
// entry in the checked prefix makes the walk result uncacheable).
static Avm2Object* findproperty_scope_walk_loc(Avm2Activation* act,
                                               const Avm2ScopeEntry* lscope,
                                               uint32_t scope_n, uint32_t mn_idx,
                                               int* out_outer_hit,
                                               int* out_prefix_pure)
{
	int pure = 1;
	*out_outer_hit = 0;
	for (uint32_t i = scope_n; i > 0; i--)
	{
		if (lscope[i - 1].is_with) pure = 0;
		if (scope_defines_mn(act, &lscope[i - 1], mn_idx))
		{
			*out_prefix_pure = pure;
			return lscope[i - 1].obj;
		}
	}
	if (act->outer != NULL)
	{
		for (uint32_t i = act->outer->count; i > 0; i--)
		{
			if (act->outer->entries[i - 1].is_with) pure = 0;
			if (scope_defines_mn(act, &act->outer->entries[i - 1], mn_idx))
			{
				*out_outer_hit = 1;
				*out_prefix_pure = pure;
				return act->outer->entries[i - 1].obj;
			}
		}
	}
	*out_prefix_pure = pure;
	return NULL;
}

static Avm2Object* findproperty_scope_walk(Avm2Activation* act,
                                           const Avm2ScopeEntry* lscope,
                                           uint32_t scope_n, uint32_t mn_idx)
{
	int outer_hit, prefix_pure;
	return findproperty_scope_walk_loc(act, lscope, scope_n, mn_idx,
	                                   &outer_hit, &prefix_pure);
}

// Domain of the defining file (lazy script init happens inside). Returns the
// resolved global/def object or NULL. `name`/`name_len` are only used for the
// lazy-name fallback key. The domain is append-only with stable object
// identity, and avm2_domain_find returns the first (lowest-index) match, so a
// NON-NULL result here is permanently valid for a given ctx → cacheable.
static Avm2Object* findproperty_domain_find(Avm2Activation* act, uint32_t mn_idx,
                                            const char* name, uint32_t name_len)
{
	const Avm2AbcFileData* data = act->file->data;
	Avm2Context* ctx = act->ctx;
	Avm2PropKey key;
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
	if (mn->kind == 0x07 || mn->kind == 0x0d)
	{
		if (avm2_propkey_from_qname(data, mn_idx, &key))
		{
			return avm2_domain_find(ctx, act->file->scope, &key);
		}
	}
	else if (mn->kind == 0x09 || mn->kind == 0x0e)
	{
		const Avm2AbcNsSet* set = &data->ns_sets[mn->ns_set];
		for (uint32_t i = 0; i < set->count; i++)
		{
			key.name = data->strings[mn->name].utf8;
			key.name_len = data->strings[mn->name].len;
			avm2_key_ns_from_abc(&key, data, set->ns_indices[i]);
			Avm2Object* g = avm2_domain_find(ctx, act->file->scope, &key);
			if (g != NULL) return g;
		}
	}
	else
	{
		// Lazy name resolved by the caller: public key lookup.
		key = avm2_public_key(name, name_len);
		return avm2_domain_find(ctx, act->file->scope, &key);
	}
	return NULL;
}

// Last resort (Ruffle find_definition): the global scope's own dynamic props
// and its prototype chain (findprop_global_prototype). Returns the global
// scope object or NULL. Dynamic → NOT cacheable.
static Avm2Object* findproperty_global_proto(Avm2Activation* act,
                                             const Avm2ScopeEntry* lscope,
                                             uint32_t scope_n, uint32_t mn_idx,
                                             const char* name, uint32_t name_len)
{
	if (!avm2_mn_has_public_ns(act->file->data, mn_idx)) return NULL;
	Avm2Object* global = NULL;
	if (act->outer != NULL && act->outer->count > 0)
	{
		global = act->outer->entries[0].obj;
	}
	else if (scope_n > 0)
	{
		global = lscope[0].obj;
	}
	for (Avm2Object* p = global; p != NULL; p = p->proto)
	{
		if (avm2_object_find_dynamic(p, name, name_len) != NULL)
		{
			return global;
		}
	}
	return NULL;
}

// Full resolution (scope walk → domain → global-prototype), NO throw: returns
// NULL on a total miss. The verify build cross-checks the inline-cached op
// against this.
static Avm2Object* findproperty_resolve(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                        uint32_t scope_n, uint32_t mn_idx,
                                        const char* name, uint32_t name_len)
{
	Avm2Object* g = findproperty_scope_walk(act, lscope, scope_n, mn_idx);
	if (g != NULL) return g;
	g = findproperty_domain_find(act, mn_idx, name, name_len);
	if (g != NULL) return g;
	return findproperty_global_proto(act, lscope, scope_n, mn_idx, name, name_len);
}

static Avm2Object* findproperty_impl(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                     uint32_t scope_n, uint32_t mn_idx,
                                     const char* name, uint32_t name_len, int strict)
{
	Avm2Object* g = findproperty_resolve(act, lscope, scope_n, mn_idx, name, name_len);
	if (g != NULL) return g;
	if (!strict)
	{
		return avm2_op_getglobalscope(act, lscope, scope_n);
	}
	avm2_throw_1065(act->ctx, name, name_len);
}

Avm2Object* avm2_op_findproperty(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                 uint32_t scope_n, uint32_t mn_idx, int strict)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	return findproperty_impl(act, lscope, scope_n, mn_idx, name, name_len, strict);
}

// Per-call-site domain inline cache for static-multiname FindProperty /
// FindPropStrict (the recompiler's getlex-global type-specialization lever).
// The scope walk varies per activation, so it is ALWAYS re-run to preserve
// exact semantics; the cache only elides the DOMAIN resolution — key
// construction + avm2_domain_find — which is stable per ctx (the domain is
// append-only with stable object identity, and a non-NULL hit is the first
// match ⟹ permanently valid). `scope_stable` (recompiler proved the method is
// with-free ⟹ every scope lookup is trait/vtable-based ⟹ the scope-walk
// hit/miss is invariant across activations at this site) additionally lets a
// populated domain cache skip the scope walk entirely — the largest saving.
// Only DOMAIN hits are ever cached; a scope hit or the dynamic global-proto
// fallback always takes the slow path. strict: throw 1065 on a total miss.
// Build -DAVM2_FIND_VERIFY to cross-check the returned object against the full
// uncached resolve on every call and abort on any mismatch.
Avm2Object* avm2_op_findpropstrict_ic(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                      uint32_t scope_n, uint32_t mn_idx, int strict,
                                      int scope_stable, Avm2FindCache* ic)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* g;
	if (scope_stable && ic->scope == act->file->scope && ic->scope_kind == 2
	    && scope_n == 1 && !lscope[0].is_with && lscope[0].obj != NULL
	    && lscope[0].obj->vtable == ic->l_vt0)
	{
		// Cached LOCAL hit at lscope[0] (the walk's very first probe, so no
		// prefix to re-validate): same vtable ⟹ vtable_find_mn re-hits ⟹
		// the walk would return exactly this activation's lscope[0].obj.
		g = lscope[0].obj;
	}
	else if (scope_stable && ic->scope == act->file->scope && ic->scope_kind == 1
	    && act->outer == ic->outer && scope_n == ic->l_n
	    && (scope_n == 0 || (!lscope[0].is_with && lscope[0].obj != NULL
	                         && lscope[0].obj->vtable == ic->l_vt0)))
	{
		// Cached OUTER-chain scope hit. Outer chains are never freed, so the
		// identity guard implies the exact entry objects (and their vtables)
		// the populate-time walk examined: the entries above the hit re-miss
		// (all non-with, trait-based ⟹ vtable-determined), the hit re-hits.
		// The local prefix is re-guarded directly (same vtable, non-with,
		// scope_n unchanged) — so the replay is byte-identical to a walk.
		g = ic->scope_obj;
	}
	else if (scope_stable && ic->scope == act->file->scope && ic->obj != NULL)
	{
		// With-free method + a prior domain hit ⟹ the scope walk is a proven
		// miss at this site: go straight to the cached def object.
		g = ic->obj;
	}
	else
	{
		int outer_hit = 0, prefix_pure = 0;
		g = findproperty_scope_walk_loc(act, lscope, scope_n, mn_idx,
		                                &outer_hit, &prefix_pure);
		if (g != NULL)
		{
			// Cache the hit LOCATION. Outer-chain hits (kind 1) replay the
			// fixed hit object; a hit at lscope[0] with scope_n==1 (kind 2)
			// replays "return the current lscope[0].obj" under a vtable
			// guard. With-tainted prefixes are content-dependent — never
			// cached (prefix_pure covers the hit entry itself).
			if (scope_stable && prefix_pure && scope_n <= 1
			    && (scope_n == 0 || (!lscope[0].is_with && lscope[0].obj != NULL)))
			{
				if (outer_hit)
				{
					ic->scope = act->file->scope;
					ic->outer = act->outer;
					ic->l_n = scope_n;
					ic->l_vt0 = (scope_n == 1) ? lscope[0].obj->vtable : NULL;
					ic->scope_obj = g;
					ic->scope_kind = 1;
				}
				else if (scope_n == 1 && g == lscope[0].obj)
				{
					ic->scope = act->file->scope;
					ic->l_vt0 = lscope[0].obj->vtable;
					ic->l_n = 1;
					ic->scope_kind = 2;
				}
			}
		}
		if (g == NULL)
		{
			if (ic->scope == act->file->scope && ic->obj != NULL)
			{
				g = ic->obj;   // cached domain hit (scope missed)
			}
			else
			{
				const char* name;
				uint32_t name_len;
				avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
				g = findproperty_domain_find(act, mn_idx, name, name_len);
				if (g != NULL)
				{
					ic->scope = act->file->scope;
					ic->obj = g;
				}
				else
				{
					g = findproperty_global_proto(act, lscope, scope_n, mn_idx,
					                               name, name_len);
				}
			}
		}
	}

#ifdef AVM2_FIND_VERIFY
	{
		const char* vname;
		uint32_t vname_len;
		avm2_mn_name(act->file->data, mn_idx, &vname, &vname_len);
		Avm2Object* ref = findproperty_resolve(act, lscope, scope_n, mn_idx,
		                                        vname, vname_len);
		if (g != ref)
		{
			avm2_fatal("[AVM2_FIND_VERIFY] findprop IC mismatch mn=%u strict=%d "
			           "stable=%d: ic=%p ref=%p", mn_idx, strict, scope_stable,
			           (void*) g, (void*) ref);
		}
	}
#endif

	if (g != NULL) return g;
	if (!strict)
	{
		return avm2_op_getglobalscope(act, lscope, scope_n);
	}
	{
		const char* name;
		uint32_t name_len;
		avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
		avm2_throw_1065(ctx, name, name_len);
	}
}

// --- find→own-class-static lever (avm2_ops.h Avm2StaticFindCache) ----------

// Shared slow path: full findpropstrict walk; populate the cache only when
// the resolution lands exactly on the enclosing class's class object AND the
// local scope has the canonical [this] shape the replay guards re-check.
// Returns the walk result unconditionally (exact FindPropStrict semantics).
static Avm2Object* findprop_ownstatic_resolve(Avm2Activation* act,
                                              const Avm2ScopeEntry* lscope,
                                              uint32_t scope_n, uint32_t mn_idx,
                                              uint32_t class_index,
                                              Avm2StaticFindCache* c)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	Avm2Object* g = findproperty_resolve(act, lscope, scope_n, mn_idx,
	                                      name, name_len);
	if (g == NULL)
	{
		avm2_throw_1065(act->ctx, name, name_len);
	}
	Avm2Class* cls = (act->file->classes != NULL)
		? act->file->classes[class_index] : NULL;
	if (cls != NULL && g == cls->class_object
	    && scope_n == 1 && !lscope[0].is_with && lscope[0].obj != NULL)
	{
		c->ctx = act->ctx;
		c->outer = act->outer;
		c->l_vt0 = lscope[0].obj->vtable;
		c->cls_obj = g;
	}
	return g;
}

Avm2Object* avm2_findprop_ownstatic_slow(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                         uint32_t scope_n, uint32_t mn_idx,
                                         uint32_t class_index, Avm2StaticFindCache* c)
{
	return findprop_ownstatic_resolve(act, lscope, scope_n, mn_idx, class_index, c);
}

Avm2Value avm2_getlex_ownstatic_slow(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                     uint32_t scope_n, uint32_t mn_idx,
                                     uint32_t class_index, uint32_t slot,
                                     Avm2StaticFindCache* c)
{
	Avm2Object* g = findprop_ownstatic_resolve(act, lscope, scope_n, mn_idx,
	                                           class_index, c);
	if (c->ctx == act->ctx && c->cls_obj == g)
	{
		// Populated (or re-confirmed) as the expected class object → the
		// compile-time static slot index applies (cvt-numbering mirror,
		// cross-checked under -DAVM2_SLOT_VERIFY).
		return g->slots[slot];
	}
	// Unexpected resolution (a shadow invisible at compile time): fully
	// generic read on whatever the walk returned — exact semantics.
	return avm2_op_getproperty_static(act, avm2_object_value(g), mn_idx);
}

#if defined(AVM2_FIND_VERIFY) || defined(AVM2_SLOT_VERIFY)
// Verify build: every fused getlex-ownstatic site re-runs the full resolve;
// a cache that WOULD have replayed must agree with the walk, and when the
// walk lands on the expected class object the compile-time slot index must
// match the runtime resolve exactly.
Avm2Value avm2_op_getlex_ownstatic(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                   uint32_t scope_n, uint32_t mn_idx,
                                   uint32_t class_index, uint32_t slot,
                                   Avm2StaticFindCache* c)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	int would_replay = c->ctx == act->ctx && act->outer == c->outer
		&& scope_n == 1 && !lscope[0].is_with && lscope[0].obj != NULL
		&& lscope[0].obj->vtable == c->l_vt0;
	Avm2Object* cached = would_replay ? c->cls_obj : NULL;
	Avm2Object* g = findprop_ownstatic_resolve(act, lscope, scope_n, mn_idx,
	                                           class_index, c);
	if (would_replay && cached != g)
	{
		avm2_fatal("[AVM2_FIND_VERIFY] getlex_ownstatic replay mismatch mn=%u "
		           "(%.*s): cached=%p walk=%p", mn_idx, (int) name_len, name,
		           (void*) cached, (void*) g);
	}
	Avm2Class* cls = (act->file->classes != NULL)
		? act->file->classes[class_index] : NULL;
	if (cls != NULL && g == cls->class_object)
	{
		Resolved r;
		int ok = resolve_mn(act, avm2_object_value(g), mn_idx, &r);
		if (!ok || r.entry == NULL || r.entry->kind != AVM2_PROP_SLOT
		    || r.entry->slot_index != slot)
		{
			avm2_fatal("[AVM2_SLOT_VERIFY] getlex_ownstatic %.*s: compile-time "
			           "static slot %u does not match runtime resolve "
			           "(ok=%d kind=%d slot=%u)", (int) name_len, name, slot,
			           ok, (ok && r.entry) ? (int) r.entry->kind : -1,
			           (ok && r.entry) ? r.entry->slot_index : 0);
		}
		return g->slots[slot];
	}
	fprintf(stderr, "[getlex_ownstatic] note: generic fallback engaged mn=%u "
	        "(%.*s)\n", mn_idx, (int) name_len, name);
	return avm2_op_getproperty_static(act, avm2_object_value(g), mn_idx);
}
#endif

#ifdef AVM2_FIND_VERIFY
// Verify build for the standalone guarded find: semantics are exact by
// construction (fallback returns the walk result), so verification checks
// that a cache that WOULD have replayed agrees with the full walk.
Avm2Object* avm2_op_findprop_ownstatic(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                       uint32_t scope_n, uint32_t mn_idx,
                                       uint32_t class_index, Avm2StaticFindCache* c)
{
	int would_replay = c->ctx == act->ctx && act->outer == c->outer
		&& scope_n == 1 && !lscope[0].is_with && lscope[0].obj != NULL
		&& lscope[0].obj->vtable == c->l_vt0;
	Avm2Object* cached = would_replay ? c->cls_obj : NULL;
	Avm2Object* g = findprop_ownstatic_resolve(act, lscope, scope_n, mn_idx,
	                                           class_index, c);
	if (would_replay && cached != g)
	{
		const char* name;
		uint32_t name_len;
		avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
		avm2_fatal("[AVM2_FIND_VERIFY] findprop_ownstatic replay mismatch mn=%u "
		           "(%.*s): cached=%p walk=%p", mn_idx, (int) name_len, name,
		           (void*) cached, (void*) g);
	}
	return g;
}
#endif

#ifdef AVM2_FIND_VERIFY
// Verify build for the recompiler's find→this lever (avm2_ops.h): every
// substituted find site re-runs the full resolve and aborts unless it lands
// exactly on `this`.
Avm2Value avm2_op_findprop_this(Avm2Activation* act, Avm2Value thisv,
                                const Avm2ScopeEntry* lscope, uint32_t scope_n,
                                uint32_t mn_idx)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	Avm2Object* ref = findproperty_resolve(act, lscope, scope_n, mn_idx,
	                                        name, name_len);
	if (thisv.kind != AVM2_VALUE_OBJECT || thisv.u.obj == NULL
	    || ref != thisv.u.obj)
	{
		avm2_fatal("[AVM2_FIND_VERIFY] findprop_this mismatch mn=%u (%.*s): "
		           "this=%p (kind=%u) ref=%p scope_n=%u", mn_idx,
		           (int) name_len, name,
		           (void*) (thisv.kind == AVM2_VALUE_OBJECT ? thisv.u.obj : NULL),
		           thisv.kind, (void*) ref, scope_n);
	}
	return thisv;
}
#endif

// Key-based scope search shared by the lazy-name and lazy-ns FindProperty
// variants. `public_ok` gates dynamic-prop matching (with-scopes and the
// global prototype-chain fallback).
static Avm2Object* findproperty_key(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                    uint32_t scope_n, const Avm2PropKey* key,
                                    int public_ok, int strict)
{
	Avm2Context* ctx = act->ctx;
	for (uint32_t i = scope_n; i > 0; i--)
	{
		Avm2Object* so = lscope[i - 1].obj;
		Resolved r;
		if (lscope[i - 1].is_with)
		{
			Avm2Value sv = avm2_object_value(so);
			if (avm2_value_is_xmlish(sv))
			{
				if (avm2_xml_has_property_via_in(ctx, sv,
				        avm2_string_new(ctx, key->name, key->name_len)))
				{
					return so;
				}
			}
			else if (resolve_key(ctx, sv, key, public_ok, &r))
			{
				return so;
			}
		}
		else if (avm2_vtable_find(so->vtable, key) != NULL)
		{
			return so;
		}
	}
	if (act->outer != NULL)
	{
		for (uint32_t i = act->outer->count; i > 0; i--)
		{
			Avm2Object* so = act->outer->entries[i - 1].obj;
			Resolved r;
			if (act->outer->entries[i - 1].is_with)
			{
				if (resolve_key(ctx, avm2_object_value(so), key, public_ok, &r)) return so;
			}
			else if (avm2_vtable_find(so->vtable, key) != NULL)
			{
				return so;
			}
		}
	}
	Avm2Object* g = avm2_domain_find(ctx, act->file->scope, key);
	if (g != NULL) return g;
	if (public_ok)
	{
		Avm2Object* global = NULL;
		if (act->outer != NULL && act->outer->count > 0)
		{
			global = act->outer->entries[0].obj;
		}
		else if (scope_n > 0)
		{
			global = lscope[0].obj;
		}
		for (Avm2Object* p = global; p != NULL; p = p->proto)
		{
			if (avm2_object_find_dynamic(p, key->name, key->name_len) != NULL)
			{
				return global;
			}
		}
	}
	if (!strict)
	{
		return avm2_op_getglobalscope(act, lscope, scope_n);
	}
	avm2_throw_1065(ctx, key->name, key->name_len);
}

Avm2Object* avm2_op_findproperty_dyn(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                     uint32_t scope_n, uint32_t mn_idx, Avm2Value name,
                                     int strict)
{
	// Resolve the runtime name to a string and search scope objects for a
	// public property of that name (with-scope semantics apply the same).
	Avm2Context* ctx = act->ctx;
	{
		const Avm2QNameExt* q = avm2_qname_ext_of(name);
		if (q != NULL)
		{
			Avm2PropKey key;
			int any_ns;
			int pub = key_from_qname_ext(q, &key, &any_ns);
			return findproperty_key(act, lscope, scope_n, &key, pub, strict);
		}
	}
	const Avm2String* ns = avm2_coerce_to_string(ctx, name);
	if (mn_is_lazy_name_set(act->file->data, mn_idx))
	{
		// MultinameL: the static half is a ns SET, tested at each scope level
		// with the runtime name. Run before the public-key search below so a
		// non-public trait at an inner scope wins over an outer public one.
		Avm2Object* g = findproperty_scope_walk_named(act, lscope, scope_n, mn_idx,
		                                              ns->utf8, ns->len);
		if (g == NULL) g = findproperty_domain_find_named(act, mn_idx, ns->utf8, ns->len);
		if (g != NULL) return g;
	}
	Avm2PropKey key = avm2_public_key(ns->utf8, ns->len);
	return findproperty_key(act, lscope, scope_n, &key, 1, strict);
}

Avm2Object* avm2_op_findproperty_rtns(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                      uint32_t scope_n, uint32_t mn_idx, Avm2Value ns_val,
                                      int strict)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	Avm2PropKey key;
	int pub = key_from_ns_value(act->ctx, ns_val, name, name_len, &key);
	return findproperty_key(act, lscope, scope_n, &key, pub, strict);
}

Avm2Object* avm2_op_findproperty_rtns_l(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                        uint32_t scope_n, uint32_t mn_idx, Avm2Value ns_val,
                                        Avm2Value name_val, int strict)
{
	(void) mn_idx;
	{
		const Avm2QNameExt* q = avm2_qname_ext_of(name_val);
		if (q != NULL)
		{
			Avm2PropKey key;
			int any_ns;
			int pub = key_from_qname_ext(q, &key, &any_ns);
			return findproperty_key(act, lscope, scope_n, &key, pub, strict);
		}
	}
	const Avm2String* s = avm2_coerce_to_string(act->ctx, name_val);
	Avm2PropKey key;
	int pub = key_from_ns_value(act->ctx, ns_val, s->utf8, s->len, &key);
	return findproperty_key(act, lscope, scope_n, &key, pub, strict);
}

Avm2Object* avm2_op_finddef(Avm2Activation* act, uint32_t mn_idx)
{
	const Avm2AbcFileData* data = act->file->data;
	Avm2PropKey key;
	if (avm2_propkey_from_qname(data, mn_idx, &key))
	{
		Avm2Object* g = avm2_domain_find(act->ctx, act->file->scope, &key);
		if (g != NULL) return g;
	}
	else if (data->multinames[mn_idx].kind == 0x09
	         || data->multinames[mn_idx].kind == 0x0e)
	{
		const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
		const Avm2AbcNsSet* set = &data->ns_sets[mn->ns_set];
		for (uint32_t i = 0; i < set->count; i++)
		{
			key.name = data->strings[mn->name].utf8;
			key.name_len = data->strings[mn->name].len;
			avm2_key_ns_from_abc(&key, data, set->ns_indices[i]);
			Avm2Object* g = avm2_domain_find(act->ctx, act->file->scope, &key);
			if (g != NULL) return g;
		}
	}
	const char* name;
	uint32_t name_len;
	avm2_mn_name(data, mn_idx, &name, &name_len);
	avm2_throw_1065(act->ctx, name, name_len);
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

static Avm2Value callproperty_common(Avm2Context* ctx, Avm2Value recv,
                                     const char* name, uint32_t name_len,
                                     int resolved_ok, const Resolved* r,
                                     const Avm2Value* args, uint32_t argc,
                                     Avm2Value call_recv)
{
	if (resolved_ok && r->is_proxy)
	{
		Avm2Value* a = avm2_alloc(ctx, (argc + 1) * sizeof(Avm2Value));
		a[0] = avm2_object_value(
			avm2_qname_new(ctx, r->proxy_uri, r->proxy_local));
		for (uint32_t i = 0; i < argc; i++) a[i + 1] = args[i];
		Avm2Value pr = avm2_proxy_call_hook(ctx, recv.u.obj, "callProperty", a, argc + 1);
		heap_free(ctx->app, a);  // scratch argument vector
		return pr;
	}
	if (resolved_ok && r->entry != NULL && r->entry->kind == AVM2_PROP_METHOD)
	{
		return avm2_call_method_ref(ctx, &r->entry->method,
		                            r->entry->defining_class, r->entry->method_scope,
		                            call_recv, args, argc);
	}
	if (resolved_ok)
	{
		Avm2Value v = resolved_get(ctx, recv, r, name, name_len);
		if (v.kind == AVM2_VALUE_OBJECT && v.u.obj != NULL)
		{
			return avm2_call_value(ctx, v, call_recv, args, argc);
		}
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1006: %.*s is not a function.",
		                 (int) name_len, name);
	}
	// Miss.
	if (recv.kind == AVM2_VALUE_OBJECT && !object_is_dynamic(recv.u.obj))
	{
		char cn[160];
		class_name_of(ctx, recv, cn, sizeof(cn));
		avm2_throw_error(ctx, ctx->builtins.reference_error_class,
		                 "Error #1069: Property %.*s not found on %s and there "
		                 "is no default value.", (int) name_len, name, cn);
	}
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1006: %.*s is not a function.", (int) name_len, name);
}

static Avm2Value callproperty_static_impl(Avm2Activation* act, Avm2Value recv,
                                          uint32_t mn_idx, const Avm2Value* args,
                                          uint32_t argc, Avm2InlineCache* ic)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(act->ctx, recv, name, name_len);
	}
	// Populate the per-call-site inline cache: a plain-object receiver whose
	// PRIMARY vtable find hits. Same invariants as getproperty_static_impl —
	// XML excluded (its property/method set varies with content) and the GC'd
	// no_index newactivation/newcatch vtables excluded (a freed-then-reused
	// address could false-hit a stale entry). A static callproperty resolves
	// purely through resolve_mn (no numeric-index fast path), so a matching
	// vtable entry is exactly what resolve_mn would return → byte-identical.
	if (ic != NULL && recv.kind == AVM2_VALUE_OBJECT && !avm2_value_is_xmlish(recv))
	{
		const Avm2VTable* vt = avm2_value_vtable(act->ctx, recv);
		if (vt != NULL && !vt->no_index)
		{
			const Avm2PropEntry* e = avm2_vtable_find_mn(vt, act->file->data, mn_idx);
			if (e != NULL)
			{
				ic->vt = vt;
				ic->vt_count = vt->count;
				ic->entry_index = (uint32_t) (e - vt->entries);
			}
		}
	}
	Resolved r;
	int ok = resolve_mn(act, recv, mn_idx, &r);
	if (!ok && avm2_value_is_xmlish(recv))
	{
		return avm2_xml_call_fallback(act->ctx, recv, name, name_len, args, argc);
	}
	return callproperty_common(act->ctx, recv, name, name_len, ok, &r, args, argc, recv);
}

Avm2Value avm2_op_callproperty(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                               const Avm2Value* args, uint32_t argc)
{
	recv = unbox_scope_prim(recv);
	return callproperty_static_impl(act, recv, mn_idx, args, argc, NULL);
}

Avm2Value avm2_op_callproperty_ic(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                  const Avm2Value* args, uint32_t argc, Avm2InlineCache* ic)
{
	recv = unbox_scope_prim(recv);
	// Fast path: same receiver vtable (and unchanged entry count) as the cached
	// resolve → replay the resolved entry, skipping the multiname match. A
	// matching vt guarantees a non-null, non-xmlish object whose primary find
	// hits this same entry, so this is byte-identical to the full path.
	if (recv.kind == AVM2_VALUE_OBJECT)
	{
		const Avm2VTable* vt = avm2_value_vtable(act->ctx, recv);
		if (vt != NULL && ic->vt == vt && ic->vt_count == vt->count)
		{
			Resolved r = {0};
			r.entry = &vt->entries[ic->entry_index];
			const char* name;
			uint32_t name_len;
			avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
			return callproperty_common(act->ctx, recv, name, name_len, 1, &r,
			                           args, argc, recv);
		}
	}
	return callproperty_static_impl(act, recv, mn_idx, args, argc, ic);
}

static Avm2Value callproperty_qname(Avm2Activation* act, Avm2Value recv,
                                    const Avm2QNameExt* q,
                                    const Avm2Value* args, uint32_t argc);

Avm2Value avm2_op_callproperty_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                   Avm2Value name_val, const Avm2Value* args, uint32_t argc)
{
	recv = unbox_scope_prim(recv);
	Avm2Context* ctx = act->ctx;
	(void) mn_idx;
	{
		const Avm2QNameExt* q = avm2_qname_ext_of(name_val);
		if (q != NULL) return callproperty_qname(act, recv, q, args, argc);
	}
	if (value_is_null_like(recv))
	{
		const Avm2String* ns = avm2_coerce_to_string(ctx, name_val);
		avm2_throw_null_or_undefined(ctx, recv, ns->utf8, ns->len);
	}
	uint32_t idx;
	if (recv.kind == AVM2_VALUE_OBJECT && array_index_access(recv.u.obj)
	    && avm2_value_as_index(name_val, &idx))
	{
		Avm2Value v = avm2_array_get(recv.u.obj, idx);
		if (v.kind != AVM2_VALUE_HOLE)
		{
			return avm2_call_value(ctx, v, recv, args, argc);
		}
	}
	if (recv.kind == AVM2_VALUE_OBJECT && recv.u.obj->kind == AVM2_OBJ_VECTOR
	    && avm2_value_as_index(name_val, &idx)
	    && idx < avm2_vector_ext(recv.u.obj)->length)
	{
		return avm2_call_value(ctx, avm2_vector_get_index(ctx, recv.u.obj, idx),
		                       recv, args, argc);
	}
	const Avm2String* ns = avm2_coerce_to_string(ctx, name_val);
	Avm2PropKey key = avm2_public_key(ns->utf8, ns->len);
	Resolved r;
	int ok = resolve_key(ctx, recv, &key, 1, &r);
	if (!ok && avm2_value_is_xmlish(recv))
	{
		return avm2_xml_call_fallback(ctx, recv, ns->utf8, ns->len, args, argc);
	}
	return callproperty_common(act->ctx, recv, ns->utf8, ns->len, ok, &r, args, argc, recv);
}

static Avm2Value callproperty_qname(Avm2Activation* act, Avm2Value recv,
                                    const Avm2QNameExt* q,
                                    const Avm2Value* args, uint32_t argc)
{
	Avm2Context* ctx = act->ctx;
	Avm2PropKey key;
	int any_ns;
	int pub = key_from_qname_ext(q, &key, &any_ns);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, key.name, key.name_len);
	}
	Resolved r;
	int ok = resolve_generic_key(ctx, recv, &key, pub, any_ns, &r);
	if (!ok && avm2_value_is_xmlish(recv))
	{
		return avm2_xml_call_fallback(ctx, recv, key.name, key.name_len, args, argc);
	}
	return callproperty_common(ctx, recv, key.name, key.name_len, ok, &r, args, argc, recv);
}

Avm2Value avm2_op_callproperty_rtns(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                    Avm2Value ns_val, const Avm2Value* args, uint32_t argc)
{
	recv = unbox_scope_prim(recv);
	Avm2Context* ctx = act->ctx;
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, name, name_len);
	}
	Avm2PropKey key;
	int pub = key_from_ns_value(act->ctx, ns_val, name, name_len, &key);
	Resolved r;
	int ok = resolve_key(ctx, recv, &key, pub, &r);
	if (!ok && avm2_value_is_xmlish(recv))
	{
		return avm2_xml_call_fallback(ctx, recv, name, name_len, args, argc);
	}
	return callproperty_common(ctx, recv, name, name_len, ok, &r, args, argc, recv);
}

Avm2Value avm2_op_callproperty_rtns_l(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                      Avm2Value ns_val, Avm2Value name_val,
                                      const Avm2Value* args, uint32_t argc)
{
	recv = unbox_scope_prim(recv);
	(void) mn_idx;
	Avm2Context* ctx = act->ctx;
	const Avm2QNameExt* q = avm2_qname_ext_of(name_val);
	if (q != NULL)
	{
		return callproperty_qname(act, recv, q, args, argc);
	}
	const Avm2String* s = avm2_coerce_to_string(ctx, name_val);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, s->utf8, s->len);
	}
	Avm2PropKey key;
	int pub = key_from_ns_value(act->ctx, ns_val, s->utf8, s->len, &key);
	Resolved r;
	int ok = resolve_key(ctx, recv, &key, pub, &r);
	if (!ok && avm2_value_is_xmlish(recv))
	{
		return avm2_xml_call_fallback(ctx, recv, s->utf8, s->len, args, argc);
	}
	return callproperty_common(ctx, recv, s->utf8, s->len, ok, &r, args, argc, recv);
}

Avm2Value avm2_op_callproplex(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                              const Avm2Value* args, uint32_t argc)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(act->ctx, recv, name, name_len);
	}
	Resolved r;
	int ok = resolve_mn(act, recv, mn_idx, &r);
	// Lex call: the callee receives null as `this`.
	return callproperty_common(act->ctx, recv, name, name_len, ok, &r, args, argc,
	                           avm2_null());
}

Avm2Value avm2_op_call(Avm2Activation* act, Avm2Value func, Avm2Value recv,
                       const Avm2Value* args, uint32_t argc)
{
	return avm2_call_value(act->ctx, func, recv, args, argc);
}

// CallStatic class-side receiver test: `recv` is a class object whose class
// definition is `env_class` (or a class object minted from the same ABC class:
// same defining file and the same instance initializer), or a subclass's.
static int callstatic_recv_is_class_side(const Avm2Class* env_class, Avm2Value recv)
{
	if (recv.kind != AVM2_VALUE_OBJECT || recv.u.obj == NULL)
		return 0;
	// Class object -> walk its class_ref chain (class-side call); instance ->
	// walk its own class chain (an instance of a SECOND class object minted
	// from the same ABC class is not `avm2_coerce_to_class`-compatible with
	// the first one's Avm2Class, yet Flash accepts it — 4th line of the test).
	const Avm2Class* start = (recv.u.obj->kind == AVM2_OBJ_CLASS)
	                       ? recv.u.obj->class_ref : recv.u.obj->cls;
	for (const Avm2Class* c = start; c != NULL; c = c->super_class)
	{
		if (c == env_class)
			return 1;
		if (c->instance_init.file != NULL
		    && c->instance_init.file == env_class->instance_init.file
		    && c->instance_init.method_index == env_class->instance_init.method_index)
			return 1;
	}
	return 0;
}

Avm2Value avm2_op_callstatic(Avm2Activation* act, uint32_t method_index, Avm2Value recv,
                             const Avm2Value* args, uint32_t argc)
{
	const Avm2AbcMethodData* m = &act->file->data->methods[method_index];
	Avm2MethodRef ref = { m->fn, act->file, m->debug_name, method_index };
	// avmplus dispatches CallStatic through the MethodInfo's MethodEnv, and a
	// MethodInfo has exactly ONE — the one from its FIRST binding. So the
	// callee runs on the scope captured where it was first installed in a
	// vtable, NOT on the caller's `act->outer` (which is what a naive reading
	// of "static call" gives and what threw #1065 here). Methods that were
	// never installed in a vtable have no env; fall back to the caller's.
	Avm2ScopeChain* env_scope = NULL;
	Avm2Class* env_class = NULL;
	if (act->file->method_env_scope != NULL
	    && method_index < act->file->data->method_count)
	{
		env_scope = act->file->method_env_scope[method_index];
		env_class = act->file->method_env_class[method_index];
	}
	// Ruffle activation.rs op_call_static: "Ensure receiver is of the correct
	// type" — the receiver is coerced to the callee's OWN bound class before
	// the call, so `CallStatic` with a foreign receiver raises TypeError #1034
	// rather than silently running the method on it
	// (avm2/method_association test3: `CallStatic Test/test1` on a bare
	// `{}` object). method_env_class is exactly Ruffle's `method.bound_class()`
	// — the declaring class of the method_info's first vtable binding.
	// ...but a STATIC method's bound class is the c_class, and
	// `coerce_to_type(c_class)` accepts the class OBJECT (any class object
	// minted from the same class definition — avm2/getouterscope_two_classobjects
	// runs `CallStatic Class1/staticFunc` on two distinct Class1 class objects
	// and Flash prints 50 for both). Coercing a class-object receiver to the
	// INSTANCE class would throw #1034 there, so detect the class-side case
	// by identity of the class definition (same defining file + iinit) and
	// leave the receiver alone.
	if (env_class != NULL && !callstatic_recv_is_class_side(env_class, recv))
	{
		recv = avm2_coerce_to_class(act->ctx, env_class, recv);
	}
	if (env_scope != NULL)
	{
		return avm2_call_method_ref(act->ctx, &ref, env_class, env_scope,
		                            recv, args, argc);
	}
	return avm2_call_method_ref(act->ctx, &ref, NULL, act->outer, recv, args, argc);
}

// Is this frame CLASS-SIDE (a static method or a class initializer)? Its
// receiver is the bound class's own class object. Ruffle models these as
// methods of the c_class, whose superclass is ALWAYS `Class` — flat, never the
// base class's c_class (class.rs:530-534, `class.super_class =
// Some(class_class)`), because AS3 does not inherit static traits.
static int frame_is_class_side(const Avm2Activation* act)
{
	return act->bound_class != NULL
	    && act->this_val.kind == AVM2_VALUE_OBJECT
	    && act->this_val.u.obj != NULL
	    && act->this_val.u.obj->kind == AVM2_OBJ_CLASS
	    && act->this_val.u.obj->class_ref == act->bound_class;
}

// Super dispatch: resolve on the bound class's SUPERCLASS vtable.
static Avm2Class* super_class_of(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	if (act->bound_class == NULL)
	{
		// A SCRIPT INIT is bound to the synthetic `global` class, whose base is
		// Object (Ruffle globals/global_scope.rs create_class + script.rs:630
		// MethodAssociation::classbound(global_class, true)). We keep
		// bound_class NULL there on purpose — avm2_callstack_frame_name prints
		// `global$init()` only on that fallthrough, and
		// avm2_op_constructsuper's bound_class == NULL early return is what
		// keeps avm2/array_access_interpreter passing — so the global base is
		// supplied here instead. avm2/supercalls_weird calls
		// super.AS3::hasOwnProperty on the script global.
		if (act->this_val.kind == AVM2_VALUE_OBJECT
		    && act->this_val.u.obj != NULL
		    && act->this_val.u.obj->cls == ctx->builtins.global_class)
		{
			return ctx->builtins.object_class;
		}
	}
	else if (frame_is_class_side(act))
	{
		return ctx->builtins.class_class;
	}
	if (act->bound_class == NULL || act->bound_class->super_class == NULL)
	{
		// avmplus VerifyError 1035 (catchable). NOTE: contrary to an older
		// comment here, avm2/array_access_interpreter does NOT pin this
		// condition — it has no get/set/callsuper at all, and its script-init
		// ConstructSuper returns early in avm2_op_constructsuper before
		// reaching this function.
		avm2_throw_error(ctx, ctx->builtins.verify_error_class,
		                 "Error #1035: Illegal super expression found in method.");
	}
	return act->bound_class->super_class;
}

// Ruffle activation.rs op_get_super / op_set_super / op_call_super all open
// with the SAME two steps, in this order:
//
//     let receiver = self.pop_stack()
//         .coerce_to_type(self, bound_superclass_object.inner_class_definition())?;
//     let receiver = receiver.null_check(self, Some(&multiname))?;
//
// i.e. the receiver is coerced to the BOUND SUPERCLASS first, so a receiver
// that is not an instance of it raises TypeError #1034 *before* any name
// lookup, arg-count check or read-only check can produce a different code
// (avm2/supercalls_coerce cases #1..#6, which otherwise surface as
// 1063/1077/1037/1070 or as no throw at all). The coercion also maps
// `undefined` to `null`, which is why case #7 (`super.func()` on an
// explicitly pushed `undefined`) must report #1009 and not #1010.
static Avm2Value super_coerce_receiver(Avm2Activation* act, Avm2Value recv,
                                       Avm2Class* super,
                                       const char* name, uint32_t name_len)
{
	Avm2Context* ctx = act->ctx;
	recv = avm2_coerce_to_class(ctx, super, recv);
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, name, name_len);
	}
	return recv;
}

Avm2Value avm2_op_callsuper(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                            const Avm2Value* args, uint32_t argc)
{
	Avm2Context* ctx = act->ctx;
	Avm2Class* super = super_class_of(act);
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	recv = super_coerce_receiver(act, recv, super, name, name_len);
	const Avm2PropEntry* e = avm2_vtable_find_mn(&super->ivtable, act->file->data, mn_idx);
	if (e == NULL)
	{
		avm2_throw_error(ctx, ctx->builtins.reference_error_class,
		                 "Error #1070: Method %.*s not found on %.*s",
		                 (int) name_len, name,
		                 (int) super->name.name_len, super->name.name);
	}
	switch (e->kind)
	{
		case AVM2_PROP_METHOD:
			return avm2_call_method_ref(ctx, &e->method, e->defining_class,
			                            e->method_scope, recv, args, argc);
		case AVM2_PROP_GETTER:
		case AVM2_PROP_GETSET:
		{
			Avm2Value v = avm2_call_method_ref(ctx, &e->method, e->defining_class,
			                                   e->method_scope, recv, NULL, 0);
			return avm2_call_value(ctx, v, recv, args, argc);
		}
		case AVM2_PROP_SLOT:
		{
			Avm2Value v = recv.u.obj->slots[e->slot_index];
			return avm2_call_value(ctx, v, recv, args, argc);
		}
		default:
			avm2_throw_1006(ctx, name, name_len);
	}
}

// Shared tails of Get/SetSuper. The static-multiname and MultinameL forms
// differ ONLY in how `name` and the super-vtable entry are obtained, so the
// null check, the #1069 throw and avmplus's setsuper leniency live here once.
static Avm2Value getsuper_common(Avm2Activation* act, Avm2Value recv, Avm2Class* super,
                                 const Avm2PropEntry* e,
                                 const char* name, uint32_t name_len)
{
	Avm2Context* ctx = act->ctx;
	recv = super_coerce_receiver(act, recv, super, name, name_len);
	if (e == NULL)
	{
		avm2_throw_1069(ctx, name, name_len, super);
	}
	Resolved r;
	memset(&r, 0, sizeof(r));
	r.entry = e;
	return resolved_get(ctx, recv, &r, name, name_len);
}

static void setsuper_common(Avm2Activation* act, Avm2Value recv, Avm2Class* super,
                            const Avm2PropEntry* e,
                            const char* name, uint32_t name_len, Avm2Value value)
{
	Avm2Context* ctx = act->ctx;
	recv = super_coerce_receiver(act, recv, super, name, name_len);
	if (e == NULL)
	{
		avm2_throw_1069(ctx, name, name_len, super);
	}
	// avmplus is lenient on setsuper: writes to const slots and getter-only
	// properties silently succeed / no-op (class_supercalls_errors e2/e5).
	if (e->kind == AVM2_PROP_GETTER)
	{
		return;
	}
	if (e->kind == AVM2_PROP_SLOT && e->is_const)
	{
		Avm2Value cv = value;
		if (e->type_mn != 0 && e->type_file != NULL)
		{
			cv = avm2_coerce_to_type_mn(ctx, e->type_file, e->type_mn, value);
		}
		recv.u.obj->slots[e->slot_index] = cv;
		return;
	}
	Resolved r;
	memset(&r, 0, sizeof(r));
	r.entry = e;
	setproperty_resolved(ctx, recv, &r, name, name_len, value, 0);
}

Avm2Value avm2_op_getsuper(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx)
{
	Avm2Class* super = super_class_of(act);
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	return getsuper_common(act, recv, super,
	                       avm2_vtable_find_mn(&super->ivtable, act->file->data, mn_idx),
	                       name, name_len);
}

void avm2_op_setsuper(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx, Avm2Value value)
{
	Avm2Class* super = super_class_of(act);
	const char* name;
	uint32_t name_len;
	avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
	setsuper_common(act, recv, super,
	                avm2_vtable_find_mn(&super->ivtable, act->file->data, mn_idx),
	                name, name_len, value);
}

// Get/SetSuper with a runtime name (`super[expr]`). The multiname's ns SET is
// still static, so the entry lookup is avm2_vtable_find_mn_named against the
// superclass ivtable — `as3/Definitions/Super/SuperInForLoop` writes
// `super[p] = v` inside a for-in over the property names.
Avm2Value avm2_op_getsuper_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                               Avm2Value name_val)
{
	Avm2Class* super = super_class_of(act);
	const Avm2String* s = avm2_coerce_to_string(act->ctx, name_val);
	return getsuper_common(act, recv, super,
	                       avm2_vtable_find_mn_named(&super->ivtable, act->file->data,
	                                                 mn_idx, s->utf8, s->len),
	                       s->utf8, s->len);
}

void avm2_op_setsuper_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                          Avm2Value name_val, Avm2Value value)
{
	Avm2Class* super = super_class_of(act);
	const Avm2String* s = avm2_coerce_to_string(act->ctx, name_val);
	setsuper_common(act, recv, super,
	                avm2_vtable_find_mn_named(&super->ivtable, act->file->data,
	                                          mn_idx, s->utf8, s->len),
	                s->utf8, s->len, value);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Avm2Value avm2_op_construct(Avm2Activation* act, Avm2Value ctor,
                            const Avm2Value* args, uint32_t argc)
{
	return avm2_construct_value(act->ctx, ctor, args, argc);
}

// `constructprop` on a value that is neither a Class nor a Function throws
// #1007 from avm2_construct_value — EXCEPT when the name resolved to a SLOT
// trait and the SWF's ROOT version is < 11, where avmplus's interpreter
// raises #1115 "%1 is not a constructor." instead (Ruffle value.rs
// Value::construct_prop, the `Property::Slot | ConstSlot` branch gated on
// `root_swf.version() < 11 && is_interpreter()`).
// `as3/RuntimeErrors/Error1115NotAConstructor/v10` grades this spelling and
// its v11 sibling grades the #1007 one. The slot restriction matters: a
// missing/dynamic property keeps #1007 at every version, and a method trait
// keeps #1064.
static void constructprop_v10_gate(Avm2Activation* act, Avm2Value recv,
                                   uint32_t mn_idx, Avm2Value ctor)
{
	Avm2Context* ctx = act->ctx;
	if (ctx->swf_version >= 11) return;
	if (ctor.kind == AVM2_VALUE_OBJECT && ctor.u.obj != NULL
	    && (ctor.u.obj->kind == AVM2_OBJ_CLASS
	        || ctor.u.obj->kind == AVM2_OBJ_FUNCTION))
	{
		return;
	}
	Resolved r;
	if (!resolve_mn(act, recv, mn_idx, &r)) return;
	if (r.entry == NULL || r.entry->kind != AVM2_PROP_SLOT) return;
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1115: value is not a constructor.");
}

Avm2Value avm2_op_constructprop(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                const Avm2Value* args, uint32_t argc)
{
	// Primitive receivers resolve the ctor LENIENTLY: a miss yields
	// undefined (→ TypeError 1007 in construct), not 1069 (Ruffle
	// Value::construct_prop; constructprop_dynamic_primitive).
	if (recv.kind != AVM2_VALUE_OBJECT && !value_is_null_like(recv))
	{
		const char* name;
		uint32_t name_len;
		avm2_mn_name(act->file->data, mn_idx, &name, &name_len);
		Resolved r;
		Avm2Value ctor = resolve_mn(act, recv, mn_idx, &r)
			? resolved_get(act->ctx, recv, &r, name, name_len)
			: avm2_undefined();
		return avm2_construct_value(act->ctx, ctor, args, argc);
	}
	Avm2Value ctor = avm2_op_getproperty_static(act, recv, mn_idx);
	return avm2_construct_value(act->ctx, ctor, args, argc);
}

// Same op, emitted only inside a class/script INITIALIZER body — the
// only methods avmplus runs in interpreter mode, and therefore the only
// place the SWF<11 #1115 spelling applies (avm2/construct_errors_swf10
// grades the #1007 spelling for the very same shape inside a normal
// method at SWF 10).
Avm2Value avm2_op_constructprop_init(Avm2Activation* act, Avm2Value recv,
                                     uint32_t mn_idx, const Avm2Value* args,
                                     uint32_t argc)
{
	if (act->ctx->swf_version < 11 && recv.kind == AVM2_VALUE_OBJECT)
	{
		Avm2Value ctor = avm2_op_getproperty_static(act, recv, mn_idx);
		constructprop_v10_gate(act, recv, mn_idx, ctor);
		return avm2_construct_value(act->ctx, ctor, args, argc);
	}
	return avm2_op_constructprop(act, recv, mn_idx, args, argc);
}

Avm2Value avm2_op_constructprop_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                    Avm2Value name, const Avm2Value* args, uint32_t argc)
{
	Avm2Value ctor = avm2_op_getproperty_dyn(act, recv, mn_idx, name, 0);
	return avm2_construct_value(act->ctx, ctor, args, argc);
}

void avm2_op_constructsuper(Avm2Activation* act, Avm2Value recv,
                            const Avm2Value* args, uint32_t argc)
{
	if (act->bound_class == NULL)
	{
		// Script initializers may constructsuper on the global object; the
		// global's superclass is Object, whose constructor is a no-op.
		(void) recv;
		(void) args;
		(void) argc;
		return;
	}
	// Inside a real constructor body the receiver is `this`, but the op is
	// reachable with an explicit null/undefined receiver (`PushNull; Coerce
	// Test; ConstructSuper`). avmplus runs the super constructor against the
	// receiver, so a null one is a null-object access: #1009 (#1010 for
	// undefined). Must come AFTER the script-initializer early return above,
	// whose receiver is the global object and is legitimately unbound.
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(act->ctx, recv, NULL, 0);
	}
	Avm2Class* super = super_class_of(act);
	// Builtin bases have a no-op instance_init stub — their real constructor
	// semantics live in native_construct, which allocates and so cannot run
	// on an already-allocated receiver. native_super_init is the in-place
	// form (Array: `super(length)` / `super(a, b, c)`).
	if (super->instance_init.fn == NULL && super->native_super_init != NULL
	    && recv.kind == AVM2_VALUE_OBJECT && recv.u.obj != NULL)
	{
		super->native_super_init(act->ctx, recv.u.obj, args, argc);
		return;
	}
	avm2_call_method_ref(act->ctx, &super->instance_init, super,
	                     super->iscope != NULL ? super->iscope : super->scope,
	                     recv, args, argc);
}

Avm2Value avm2_op_newclass(Avm2Activation* act, uint32_t class_idx, Avm2Value base,
                           const Avm2ScopeEntry* lscope, uint32_t scope_n)
{
	Avm2Context* ctx = act->ctx;
	// Ruffle activation.rs op_new_class, in order:
	//   1. coerce the base VALUE to Class -- a non-class, non-null base is a
	//      failed coercion (#1034), NOT a base-class mismatch;
	//   2. null base for a class that declares a superclass -> #1009;
	//   3. only then, a real class object that isn't the DECLARED super -> #1108.
	// (newclass_mismatched asserts exactly 1034 / 1009 / 1108 for those three.)
	base = avm2_coerce_to_class(ctx, ctx->builtins.class_class, base);
	Avm2Class* super = NULL;
	if (base.kind == AVM2_VALUE_OBJECT && base.u.obj != NULL
	    && base.u.obj->kind == AVM2_OBJ_CLASS)
	{
		super = base.u.obj->class_ref;
	}
	{
		// Compare by NAME, not object identity: two domains can hold distinct
		// class objects for one declared superclass and avmplus accepts that,
		// so identity would throw #1108 on valid programs.
		uint32_t super_mn = act->file->data->classes[class_idx].super_mn;
		if (super_mn != 0)
		{
			if (super == NULL)
			{
				avm2_throw_null_or_undefined(ctx, avm2_null(), NULL, 0);
			}
			if (!avm2_mn_match(act->file->data, super_mn, &super->name))
			{
				avm2_throw_error(ctx, ctx->builtins.verify_error_class,
				                 "Error #1108: The OP_newclass opcode was used "
				                 "with the incorrect base class.");
			}
		}
	}
	Avm2ScopeChain* scope = avm2_scope_capture(ctx, act->outer, lscope, scope_n);
	Avm2Class* cls = avm2_class_define(ctx, act->file, class_idx, super, scope);
	return avm2_object_value(cls->class_object);
}

Avm2Value avm2_op_newfunction(Avm2Activation* act, uint32_t method_index,
                              const Avm2ScopeEntry* lscope, uint32_t scope_n)
{
	Avm2Context* ctx = act->ctx;
	const Avm2AbcMethodData* m = &act->file->data->methods[method_index];
	Avm2ScopeChain* scope = avm2_scope_capture(ctx, act->outer, lscope, scope_n);
	Avm2MethodRef ref = { m->fn, act->file, m->debug_name, method_index };
	Avm2Object* fnobj = avm2_function_new(ctx, &ref, NULL, scope,
	                                      avm2_undefined(), false);
	return avm2_object_value(fnobj);
}

Avm2Value avm2_op_newactivation(Avm2Activation* act, uint32_t method_index)
{
	Avm2Context* ctx = act->ctx;
	const Avm2AbcMethodData* m = &act->file->data->methods[method_index];
	Avm2VTable* vt = avm2_alloc(ctx, sizeof(Avm2VTable));
	memset(vt, 0, sizeof(Avm2VTable));
	vt->no_index = 1;  // per-call activation vtable is GC'd; don't leak an index
	avm2_vtable_add_traits(ctx, vt, act->file, m->body_traits, m->body_trait_count,
	                       act->bound_class, act->outer);
	Avm2Object* obj = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, vt->slot_count + 1);
	// avmplus gives the activation object a synthetic SEALED + FINAL class
	// with NO superclass and NO prototype (Ruffle Class::for_activation +
	// verify.rs create_activation_class; op_new_activation passes `None` for
	// the proto). That single model produces all three of activation_class's
	// probes: a missing property READ is #1069 (sealed, not #1081), and a
	// dynamic WRITE is #1056. It also stops `toString` resolving through
	// Object's prototype, which is what makes probe 1 a #1069 too.
	//
	// The class is minted ONCE PER METHOD and cached on the file (avmplus
	// makes it at verify time): newactivation runs on every call of every
	// method with NEED_ACTIVATION, so a per-call mint would be a hot alloc.
	// The per-call VTABLE above stays per-call (no_index, GC'd with the
	// object); only the class is shared, and it is deliberately NOT flagged
	// AVM2_CLASS_FLAG_SYNTH_CATCH — that flag frees the class with the object.
	Avm2Class* acls = NULL;
	if (act->file->activation_classes != NULL
	    && method_index < act->file->data->method_count)
	{
		acls = act->file->activation_classes[method_index];
	}
	if (acls == NULL)
	{
		acls = avm2_alloc(ctx, sizeof(Avm2Class));
		memset(acls, 0, sizeof(Avm2Class));
		// Named after the method (Ruffle uses the method's pool name in the
		// public ns) so error text reads the method, not "Object".
		const char* nm = (m->debug_name != NULL) ? m->debug_name : "activation";
		acls->name.name = nm;
		acls->name.name_len = (uint32_t) strlen(nm);
		acls->name.ns_uri = "";
		acls->name.ns_len = 0;
		acls->flags = AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
		if (act->file->activation_classes != NULL
		    && method_index < act->file->data->method_count)
		{
			act->file->activation_classes[method_index] = acls;
		}
	}
	obj->cls = acls;
	obj->vtable = vt;
	avm2_slots_init_defaults(ctx, obj, vt);
	return avm2_object_value(obj);
}

Avm2Value avm2_op_newcatch(Avm2Activation* act, uint32_t method_index, uint32_t exc_index)
{
	Avm2Context* ctx = act->ctx;
	const Avm2AbcMethodData* m = &act->file->data->methods[method_index];
	if (exc_index >= m->exception_count)
	{
		avm2_fatal("NewCatch exception index %u out of range", exc_index);
	}
	const Avm2AbcException* e = &m->exceptions[exc_index];
	Avm2Object* obj = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 2);
	obj->cls = ctx->builtins.object_class;
	if (e->variable_mn != 0)
	{
		Avm2VTable* vt = avm2_alloc(ctx, sizeof(Avm2VTable));
		memset(vt, 0, sizeof(Avm2VTable));
		vt->no_index = 1;  // per-call catch vtable is GC'd; don't leak an index
		Avm2PropEntry pe;
		memset(&pe, 0, sizeof(pe));
		if (!avm2_propkey_from_qname(act->file->data, e->variable_mn, &pe.key))
		{
			avm2_fatal("NewCatch variable multiname %u is not a QName", e->variable_mn);
		}
		pe.kind = AVM2_PROP_SLOT;
		pe.slot_index = 1;
		// SetSlot into the catch variable coerces to the declared catch
		// type (catch_scope_slot expects the 1034 from a mismatched write).
		pe.type_mn = e->type_mn;
		pe.type_file = act->file;
		vt->slot_count = 1;
		avm2_vtable_append(ctx, vt, &pe);
		obj->vtable = vt;

		// avmplus gives the catch scope a synthetic SEALED class named
		// after the catch variable, with no prototype ("Property toString
		// not found on e" in catch_scope_slot).
		Avm2Class* cc = avm2_alloc(ctx, sizeof(Avm2Class));
		memset(cc, 0, sizeof(Avm2Class));
		cc->name = pe.key;
		cc->flags = AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL
		            | AVM2_CLASS_FLAG_SYNTH_CATCH;
		obj->cls = cc;
	}
	return avm2_object_value(obj);
}

Avm2Value avm2_op_newobject(Avm2Activation* act, const Avm2Value* name_value_pairs,
                            uint32_t num_pairs)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* obj = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 0);
	obj->cls = ctx->builtins.object_class;
	obj->proto = ctx->builtins.object_class->prototype_obj;
	// Ruffle pops pairs from the top of the stack, so the LAST pair is set
	// first (visible in enumeration order).
	for (uint32_t i = num_pairs; i > 0; i--)
	{
		Avm2Value name = name_value_pairs[(i - 1) * 2];
		Avm2Value value = name_value_pairs[(i - 1) * 2 + 1];
		const Avm2String* ns = avm2_coerce_to_string(ctx, name);
		avm2_object_set_dynamic(ctx, obj, ns->utf8, ns->len, value);
	}
	return avm2_object_value(obj);
}

Avm2Value avm2_op_newarray(Avm2Activation* act, const Avm2Value* values, uint32_t n)
{
	return avm2_object_value(avm2_array_from_values(act->ctx, values, n));
}

// ---------------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------------

Avm2Value avm2_op_add(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
#ifdef AVM2_HOTLOOP_PROF
	hl_add_note(a, b);
#endif
	return avm2_op_add_values(act->ctx, a, b);
}

Avm2Value avm2_op_subtract(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	double x = avm2_coerce_to_number(act->ctx, a);
	double y = avm2_coerce_to_number(act->ctx, b);
	return avm2_number(x - y);
}

Avm2Value avm2_op_multiply(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	double x = avm2_coerce_to_number(act->ctx, a);
	double y = avm2_coerce_to_number(act->ctx, b);
	return avm2_number(x * y);
}

Avm2Value avm2_op_divide(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	double x = avm2_coerce_to_number(act->ctx, a);
	double y = avm2_coerce_to_number(act->ctx, b);
	return avm2_number(x / y);
}

Avm2Value avm2_op_modulo(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	double x = avm2_coerce_to_number(act->ctx, a);
	double y = avm2_coerce_to_number(act->ctx, b);
	return avm2_number(fmod(x, y));
}

Avm2Value avm2_op_negate(Avm2Activation* act, Avm2Value a)
{
	return avm2_number(-avm2_coerce_to_number(act->ctx, a));
}

Avm2Value avm2_op_increment(Avm2Activation* act, Avm2Value a)
{
	return avm2_number(avm2_coerce_to_number(act->ctx, a) + 1.0);
}

Avm2Value avm2_op_decrement(Avm2Activation* act, Avm2Value a)
{
	return avm2_number(avm2_coerce_to_number(act->ctx, a) - 1.0);
}

Avm2Value avm2_op_add_i(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int32_t x = avm2_coerce_to_i32(act->ctx, a);
	int32_t y = avm2_coerce_to_i32(act->ctx, b);
	return avm2_integer((int32_t) ((uint32_t) x + (uint32_t) y));
}

Avm2Value avm2_op_subtract_i(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int32_t x = avm2_coerce_to_i32(act->ctx, a);
	int32_t y = avm2_coerce_to_i32(act->ctx, b);
	return avm2_integer((int32_t) ((uint32_t) x - (uint32_t) y));
}

Avm2Value avm2_op_multiply_i(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int32_t x = avm2_coerce_to_i32(act->ctx, a);
	int32_t y = avm2_coerce_to_i32(act->ctx, b);
	return avm2_integer((int32_t) ((uint32_t) x * (uint32_t) y));
}

Avm2Value avm2_op_negate_i(Avm2Activation* act, Avm2Value a)
{
	int32_t x = avm2_coerce_to_i32(act->ctx, a);
	return avm2_integer((int32_t) (0u - (uint32_t) x));
}

Avm2Value avm2_op_increment_i(Avm2Activation* act, Avm2Value a)
{
	int32_t x = avm2_coerce_to_i32(act->ctx, a);
	return avm2_integer((int32_t) ((uint32_t) x + 1u));
}

Avm2Value avm2_op_decrement_i(Avm2Activation* act, Avm2Value a)
{
	int32_t x = avm2_coerce_to_i32(act->ctx, a);
	return avm2_integer((int32_t) ((uint32_t) x - 1u));
}

Avm2Value avm2_op_bitand(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int32_t x = avm2_coerce_to_i32(act->ctx, a);
	int32_t y = avm2_coerce_to_i32(act->ctx, b);
	return avm2_integer(x & y);
}

Avm2Value avm2_op_bitor(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int32_t x = avm2_coerce_to_i32(act->ctx, a);
	int32_t y = avm2_coerce_to_i32(act->ctx, b);
	return avm2_integer(x | y);
}

Avm2Value avm2_op_bitxor(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int32_t x = avm2_coerce_to_i32(act->ctx, a);
	int32_t y = avm2_coerce_to_i32(act->ctx, b);
	return avm2_integer(x ^ y);
}

Avm2Value avm2_op_bitnot(Avm2Activation* act, Avm2Value a)
{
	return avm2_integer(~avm2_coerce_to_i32(act->ctx, a));
}

Avm2Value avm2_op_lshift(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int32_t x = avm2_coerce_to_i32(act->ctx, a);
	uint32_t y = avm2_coerce_to_u32(act->ctx, b) & 0x1f;
	return avm2_integer((int32_t) ((uint32_t) x << y));
}

Avm2Value avm2_op_rshift(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int32_t x = avm2_coerce_to_i32(act->ctx, a);
	uint32_t y = avm2_coerce_to_u32(act->ctx, b) & 0x1f;
	return avm2_integer(x >> y);
}

Avm2Value avm2_op_urshift(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	uint32_t x = avm2_coerce_to_u32(act->ctx, a);
	uint32_t y = avm2_coerce_to_u32(act->ctx, b) & 0x1f;
	return avm2_uint_value(x >> y);
}

Avm2Value avm2_op_equals(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	return avm2_bool(avm2_abstract_eq(act->ctx, a, b));
}

Avm2Value avm2_op_strictequals(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	(void) act;
	return avm2_bool(avm2_strict_eq(a, b));
}

Avm2Value avm2_op_lessthan(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int r = avm2_abstract_lt(act->ctx, a, b);
	return avm2_bool(r == 1);
}

Avm2Value avm2_op_lessequals(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int r = avm2_abstract_lt(act->ctx, b, a);
	return avm2_bool(r == 0);
}

Avm2Value avm2_op_greaterthan(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int r = avm2_abstract_lt(act->ctx, b, a);
	return avm2_bool(r == 1);
}

Avm2Value avm2_op_greaterequals(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	int r = avm2_abstract_lt(act->ctx, a, b);
	return avm2_bool(r == 0);
}

Avm2Value avm2_op_not(Avm2Activation* act, Avm2Value a)
{
	(void) act;
	return avm2_bool(!avm2_coerce_to_boolean(a));
}

Avm2Value avm2_op_typeof(Avm2Activation* act, Avm2Value a)
{
	Avm2Context* ctx = act->ctx;
	const char* s;
	switch (a.kind)
	{
		case AVM2_VALUE_UNDEFINED: s = "undefined"; break;
		case AVM2_VALUE_NULL: s = "object"; break;
		case AVM2_VALUE_BOOL: s = "boolean"; break;
		case AVM2_VALUE_INTEGER:
		case AVM2_VALUE_NUMBER: s = "number"; break;
		case AVM2_VALUE_STRING: s = "string"; break;
		case AVM2_VALUE_OBJECT:
			if (a.u.obj->kind == AVM2_OBJ_FUNCTION
			    && a.u.obj->cls == ctx->builtins.function_class)
			{
				s = "function";
			}
			else if (a.u.obj->cls == ctx->builtins.xml_class
			         || a.u.obj->cls == ctx->builtins.xml_list_class)
			{
				s = "xml";
			}
			else
			{
				s = "object";
			}
			break;
		default: s = "undefined"; break;
	}
	return avm2_string(avm2_string_from_literal(ctx, s));
}

Avm2Value avm2_op_in(Avm2Activation* act, Avm2Value name, Avm2Value obj)
{
	Avm2Context* ctx = act->ctx;
	if (value_is_null_like(obj))
	{
		avm2_throw_null_or_undefined(ctx, obj, NULL, 0);
	}
	{
		Avm2Object* dict;
		Avm2Object* key;
		if (dict_object_key(obj, name, &dict, &key))
		{
			return avm2_bool(avm2_object_find_dynamic_obj(dict, key) != NULL);
		}
	}
	if (obj.kind == AVM2_VALUE_OBJECT && avm2_is_proxy(obj.u.obj))
	{
		// `in` on a Proxy calls hasProperty with the STRING form of the
		// name (QNames stringify — proxy_hasproperty).
		Avm2Value arg = avm2_string(avm2_coerce_to_string(ctx, name));
		Avm2Value v = avm2_proxy_call_hook(ctx, obj.u.obj, "hasProperty", &arg, 1);
		return avm2_bool(avm2_coerce_to_boolean(v));
	}
	const Avm2String* ns = avm2_coerce_to_string(ctx, name);
	if (avm2_value_is_xmlish(obj))
	{
		return avm2_bool(avm2_xml_has_property_via_in(ctx, obj, ns) != 0);
	}
	return avm2_bool(avm2_has_public_property(ctx, obj, ns->utf8, ns->len) != 0);
}

Avm2Value avm2_op_instanceof(Avm2Activation* act, Avm2Value value, Avm2Value type)
{
	Avm2Context* ctx = act->ctx;
	if (type.kind != AVM2_VALUE_OBJECT || type.u.obj == NULL
	    || (type.u.obj->kind != AVM2_OBJ_CLASS && type.u.obj->kind != AVM2_OBJ_FUNCTION))
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1040: The right-hand side of instanceof must be a "
		                 "class or function.");
	}
	if (value.kind == AVM2_VALUE_UNDEFINED)
	{
		avm2_throw_null_or_undefined(ctx, value, NULL, 0);
	}
	if (value.kind == AVM2_VALUE_NULL)
	{
		return avm2_bool(false);
	}
	// ES3 proto-chain check.
	Avm2Object* type_proto = NULL;
	if (type.u.obj->kind == AVM2_OBJ_CLASS)
	{
		type_proto = type.u.obj->class_ref->prototype_obj;
	}
	else
	{
		Avm2Value pv = avm2_get_public_property(ctx, type, "prototype", 9, NULL);
		if (pv.kind == AVM2_VALUE_OBJECT) type_proto = pv.u.obj;
	}
	if (type_proto == NULL) return avm2_bool(false);
	for (Avm2Object* p = avm2_value_proto(ctx, value); p != NULL; p = p->proto)
	{
		if (p == type_proto) return avm2_bool(true);
	}
	return avm2_bool(false);
}

Avm2Value avm2_op_istype(Avm2Activation* act, Avm2Value value, uint32_t mn_idx)
{
	Avm2Class* cls = avm2_class_for_mn(act->ctx, act->file, mn_idx);
	if (cls == NULL) return avm2_bool(false);
	return avm2_bool(avm2_value_is_of_type(act->ctx, value, cls));
}

Avm2Value avm2_op_astype(Avm2Activation* act, Avm2Value value, uint32_t mn_idx)
{
	Avm2Class* cls = avm2_class_for_mn(act->ctx, act->file, mn_idx);
	if (cls == NULL) return avm2_null();
	return avm2_value_is_of_type(act->ctx, value, cls) ? value : avm2_null();
}

static Avm2Class* require_class_value(Avm2Context* ctx, Avm2Value type)
{
	if (type.kind == AVM2_VALUE_UNDEFINED)
	{
		avm2_throw_null_or_undefined(ctx, type, NULL, 0);
	}
	if (type.kind != AVM2_VALUE_OBJECT || type.u.obj == NULL)
	{
		avm2_throw_null_or_undefined(ctx, avm2_null(), NULL, 0);
	}
	if (type.u.obj->kind != AVM2_OBJ_CLASS)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1041: The right-hand side of operator must be a class.");
	}
	return type.u.obj->class_ref;
}

Avm2Value avm2_op_istypelate(Avm2Activation* act, Avm2Value value, Avm2Value type)
{
	Avm2Class* cls = require_class_value(act->ctx, type);
	return avm2_bool(avm2_value_is_of_type(act->ctx, value, cls));
}

Avm2Value avm2_op_astypelate(Avm2Activation* act, Avm2Value value, Avm2Value type)
{
	Avm2Class* cls = require_class_value(act->ctx, type);
	return avm2_value_is_of_type(act->ctx, value, cls) ? value : avm2_null();
}

// ---------------------------------------------------------------------------
// Coercion ops
// ---------------------------------------------------------------------------

Avm2Value avm2_op_coerce(Avm2Activation* act, Avm2Value v, uint32_t mn_idx)
{
#ifdef AVM2_HOTLOOP_PROF
	hl_coerce_note(act, v, mn_idx);
#endif
	return avm2_coerce_to_type_mn(act->ctx, act->file, mn_idx, v);
}

Avm2Value avm2_op_coerce_s(Avm2Activation* act, Avm2Value v)
{
	if (value_is_null_like(v)) return avm2_null();
	return avm2_string(avm2_coerce_to_string(act->ctx, v));
}

Avm2Value avm2_op_convert_s(Avm2Activation* act, Avm2Value v)
{
	return avm2_string(avm2_coerce_to_string(act->ctx, v));
}

Avm2Value avm2_op_coerce_o(Avm2Activation* act, Avm2Value v)
{
	(void) act;
	if (v.kind == AVM2_VALUE_UNDEFINED) return avm2_null();
	return v;
}

Avm2Value avm2_op_convert_o(Avm2Activation* act, Avm2Value v)
{
	if (value_is_null_like(v))
	{
		avm2_throw_null_or_undefined(act->ctx, v, NULL, 0);
	}
	return v;
}

// -------------------------------------------------------------------------
// Typed-value fast-path verify hook. Only compiled under -DAVM2_ARITH_VERIFY;
// the inline helpers in avm2_ops.h then run BOTH the specialized arm and the
// generic op on every execution and call this on any divergence.
#ifdef AVM2_ARITH_VERIFY
void avm2_arith_verify_fail(const char* what)
{
	avm2_fatal("[AVM2_ARITH_VERIFY] %s: specialized result differs from the "
	           "generic op", what);
}
#endif

// -------------------------------------------------------------------------
// Coerce-elision verify hooks (Step 4). Only compiled under
// -DAVM2_COERCE_VERIFY; the normal build uses the identity inlines in
// avm2_ops.h. Each hook runs the REAL coercion the recompiler elided and
// aborts if the coerced value differs from the original, proving the
// compile-time type-equality that justified the elision. It then returns the
// ORIGINAL value so the verify build's output stays byte-identical to the
// elided build (both keep `v`; a genuine mismatch aborts before returning).
#ifdef AVM2_COERCE_VERIFY
static int avm2_coerce_same_value(Avm2Value a, Avm2Value b)
{
	if (a.kind != b.kind) return 0;
	switch (a.kind)
	{
		case AVM2_VALUE_UNDEFINED:
		case AVM2_VALUE_NULL:    return 1;
		case AVM2_VALUE_BOOL:    return a.u.b == b.u.b;
		case AVM2_VALUE_INTEGER: return a.u.i == b.u.i;
		case AVM2_VALUE_NUMBER:
			// Bit-identical, with NaN==NaN (any NaN payload matches).
			if (a.u.d != a.u.d && b.u.d != b.u.d) return 1;
			return memcmp(&a.u.d, &b.u.d, sizeof(double)) == 0;
		case AVM2_VALUE_STRING:
			if (a.u.str == b.u.str) return 1;
			if (a.u.str == NULL || b.u.str == NULL) return 0;
			return a.u.str->len == b.u.str->len
			    && memcmp(a.u.str->utf8, b.u.str->utf8, a.u.str->len) == 0;
		default: return a.u.obj == b.u.obj;
	}
}

static Avm2Value avm2_coerce_verify_check(Avm2Value before, Avm2Value after, const char* site)
{
	if (!avm2_coerce_same_value(before, after))
	{
		fprintf(stderr,
		        "AVM2_COERCE_VERIFY: elided %s changed value "
		        "(before kind=%u -> after kind=%u)\n",
		        site, before.kind, after.kind);
		avm2_fatal("coerce elision mismatch");
	}
	return before;
}

Avm2Value avm2_coerce_verify_return(Avm2Activation* act, uint32_t method_index, Avm2Value v)
{
	return avm2_coerce_verify_check(v, avm2_op_coerce_return(act, method_index, v),
	                                "coerce_return");
}
Avm2Value avm2_coerce_verify_mn(Avm2Activation* act, Avm2Value v, uint32_t mn_idx)
{
	return avm2_coerce_verify_check(v, avm2_op_coerce(act, v, mn_idx), "coerce");
}
Avm2Value avm2_coerce_verify_d(Avm2Activation* act, Avm2Value v)
{
	return avm2_coerce_verify_check(v, avm2_number(avm2_coerce_to_number(act->ctx, v)),
	                                "coerce_d");
}
Avm2Value avm2_coerce_verify_i(Avm2Activation* act, Avm2Value v)
{
	return avm2_coerce_verify_check(v, avm2_integer(avm2_coerce_to_i32(act->ctx, v)),
	                                "coerce_i");
}
Avm2Value avm2_coerce_verify_u(Avm2Activation* act, Avm2Value v)
{
	return avm2_coerce_verify_check(v, avm2_uint_value(avm2_coerce_to_u32(act->ctx, v)),
	                                "coerce_u");
}
Avm2Value avm2_coerce_verify_b(Avm2Activation* act, Avm2Value v)
{
	return avm2_coerce_verify_check(v, avm2_bool(avm2_coerce_to_boolean(v)), "coerce_b");
}
Avm2Value avm2_coerce_verify_s(Avm2Activation* act, Avm2Value v)
{
	return avm2_coerce_verify_check(v, avm2_op_coerce_s(act, v), "coerce_s");
}
#endif  // AVM2_COERCE_VERIFY


// E4X escapes (ECMA-357 EscapeAttributeValue / EscapeElementValue).
static Avm2Value esc_xml(Avm2Activation* act, Avm2Value v, int attr)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* s = avm2_coerce_to_string(ctx, v);
	char* out = avm2_alloc(ctx, s->len * 6 + 1);
	uint32_t n = 0;
	for (uint32_t i = 0; i < s->len; i++)
	{
		char c = s->utf8[i];
		const char* rep = NULL;
		if (c == '&') rep = "&amp;";
		else if (c == '<') rep = "&lt;";
		else if (!attr && c == '>') rep = "&gt;";
		else if (attr && c == '"') rep = "&quot;";
		else if (attr && c == '\n') rep = "&#xA;";
		else if (attr && c == '\r') rep = "&#xD;";
		else if (attr && c == '\t') rep = "&#x9;";
		if (rep != NULL)
		{
			size_t rl = strlen(rep);
			memcpy(out + n, rep, rl);
			n += (uint32_t) rl;
		}
		else
		{
			out[n++] = c;
		}
	}
	{ const Avm2String* r_ = avm2_string_new(ctx, out, n); heap_free(ctx->app, out); return avm2_string(r_); }  // copied: the buffer is scratch
}

Avm2Value avm2_op_esc_xattr(Avm2Activation* act, Avm2Value v)
{
	return esc_xml(act, v, 1);
}

// ECMA-357 10.2.1.1 EscapeElementValue: an XML / XMLList operand contributes
// its MARKUP (ToXMLString), not its escaped string value. This is what makes
// `<a>{someXml}</a>` splice nodes into the literal instead of stringifying
// them -- an XML literal compiles to string concatenation of esc_xelem'd
// pieces followed by `new XML(...)`, so the splice IS this opcode.
// (Ruffle activation.rs op_esc_elem: XmlObject / XmlListObject -> as_xml_string,
// everything else -> escape_element_value(ToString). Deliberately asymmetric
// with esc_xattr, which always stringifies.)
Avm2Value avm2_op_esc_xelem(Avm2Activation* act, Avm2Value v)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlExt* xe = avm2_xml_ext_of(v);
	if (xe != NULL)
	{
		return avm2_string(avm2_e4x_to_xml_string(ctx, xe->node));
	}
	Avm2XmlListExt* le = avm2_xmllist_ext_of(v);
	if (le != NULL)
	{
		const Avm2String* s = avm2_string_from_literal(ctx, "");
		for (uint32_t i = 0; i < le->count; i++)
		{
			if (i > 0)
			{
				s = avm2_string_concat(ctx, s,
				                       avm2_string_from_literal(ctx, "\n"));
			}
			s = avm2_string_concat(ctx, s,
			                       avm2_e4x_to_xml_string(ctx, le->items[i]));
		}
		return avm2_string(s);
	}
	return esc_xml(act, v, 0);
}

// ---------------------------------------------------------------------------
// Enumeration (Ruffle activation.rs op_has_next_2 etc.)
// ---------------------------------------------------------------------------

Avm2Value avm2_op_hasnext2(Avm2Activation* act, Avm2Value* obj_reg, Avm2Value* idx_reg)
{
	Avm2Context* ctx = act->ctx;
	int32_t cur = avm2_coerce_to_i32(ctx, *idx_reg);
	if (cur < 0)
	{
		return avm2_bool(false);
	}
	uint32_t cur_index = (uint32_t) cur;
	Avm2Value result_value = *obj_reg;
	Avm2Object* object = NULL;

	if (value_is_null_like(*obj_reg))
	{
		cur_index = 0;
	}
	else if (obj_reg->kind == AVM2_VALUE_OBJECT)
	{
		object = obj_reg->u.obj->proto;
		cur_index = avm2_object_next_enumerant(obj_reg->u.obj, cur_index);
	}
	else
	{
		Avm2Object* proto = avm2_value_proto(ctx, *obj_reg);
		if (proto != NULL)
		{
			object = proto->proto;
			cur_index = avm2_object_next_enumerant(proto, cur_index);
		}
	}

	while (object != NULL && cur_index == 0)
	{
		Avm2Object* cur_object = object;
		cur_index = avm2_object_next_enumerant(cur_object, cur_index);
		result_value = avm2_object_value(cur_object);
		object = cur_object->proto;
	}

	if (cur_index == 0)
	{
		result_value = avm2_null();
	}
	*idx_reg = avm2_uint_value(cur_index);
	*obj_reg = result_value;
	return avm2_bool(cur_index != 0);
}

Avm2Value avm2_op_hasnext(Avm2Activation* act, Avm2Value obj, Avm2Value idx)
{
	Avm2Context* ctx = act->ctx;
	int32_t cur = avm2_coerce_to_i32(ctx, idx);
	if (cur < 0) return avm2_integer(0);
	Avm2Object* o = NULL;
	if (obj.kind == AVM2_VALUE_OBJECT) o = obj.u.obj;
	else if (!value_is_null_like(obj)) o = avm2_value_proto(ctx, obj);
	if (o == NULL) return avm2_integer(0);
	return avm2_uint_value(avm2_object_next_enumerant(o, (uint32_t) cur));
}

Avm2Value avm2_op_nextname(Avm2Activation* act, Avm2Value obj, Avm2Value idx)
{
	Avm2Context* ctx = act->ctx;
	int32_t cur = avm2_coerce_to_i32(ctx, idx);
	if (cur <= 0) return avm2_null();
	if (value_is_null_like(obj))
	{
		avm2_throw_null_or_undefined(ctx, obj, NULL, 0);
	}
	Avm2Object* o = (obj.kind == AVM2_VALUE_OBJECT) ? obj.u.obj
	                                                : avm2_value_proto(ctx, obj);
	return avm2_object_enumerant_name(ctx, o, (uint32_t) cur);
}

Avm2Value avm2_op_nextvalue(Avm2Activation* act, Avm2Value obj, Avm2Value idx)
{
	Avm2Context* ctx = act->ctx;
	int32_t cur = avm2_coerce_to_i32(ctx, idx);
	if (cur <= 0) return avm2_undefined();
	if (value_is_null_like(obj))
	{
		avm2_throw_null_or_undefined(ctx, obj, NULL, 0);
	}
	Avm2Object* o = (obj.kind == AVM2_VALUE_OBJECT) ? obj.u.obj
	                                                : avm2_value_proto(ctx, obj);
	return avm2_object_enumerant_value(ctx, o, (uint32_t) cur);
}

// ---------------------------------------------------------------------------
// Throw
// ---------------------------------------------------------------------------

_Noreturn void avm2_op_throw(Avm2Activation* act, Avm2Value v)
{
	avm2_throw(act->ctx, v);
}

// ---------------------------------------------------------------------------
// Name-based public property access (no multiname pools; used by
// coerce_to_primitive, builtins, `in`, and enumeration paths)
// ---------------------------------------------------------------------------

Avm2Value avm2_get_public_property(Avm2Context* ctx, Avm2Value recv,
                                   const char* name, uint32_t name_len, int* found)
{
	if (found != NULL) *found = 0;
	if (value_is_null_like(recv))
	{
		return avm2_undefined();
	}
	if (avm2_value_is_xmlish(recv))
	{
		E4XName n = avm2_e4x_name_from_string(
			ctx, avm2_string_new(ctx, name, name_len), 0);
		Avm2Value out;
		if (avm2_xml_get_name(ctx, recv, &n, &out))
		{
			if (found != NULL) *found = 1;
			return out;
		}
	}
	Avm2PropKey key = avm2_public_key(name, name_len);
	Resolved r;
	if (!resolve_key(ctx, recv, &key, 1, &r))
	{
		return avm2_undefined();
	}
	if (found != NULL) *found = 1;
	return resolved_get(ctx, recv, &r, name, name_len);
}

Avm2Value avm2_call_public_property(Avm2Context* ctx, Avm2Value recv,
                                    const char* name, uint32_t name_len,
                                    const Avm2Value* args, uint32_t argc)
{
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, name, name_len);
	}
	Avm2PropKey key = avm2_public_key(name, name_len);
	Resolved r;
	int ok = resolve_key(ctx, recv, &key, 1, &r);
	if (!ok && avm2_value_is_xmlish(recv))
	{
		return avm2_xml_call_fallback(ctx, recv, name, name_len, args, argc);
	}
	return callproperty_common(ctx, recv, name, name_len, ok, &r, args, argc, recv);
}

// Name-based public property WRITE (traits + setters + dynamic creation).
// Throws like SetProperty. `allow_const` is the initproperty distinction: the
// AMF deserializer is *constructing* an object, so it writes const slots the
// way the class's own initialiser would instead of reporting #1074 (a class
// with a `public const` member is serializable and round-trips in Flash —
// as3/AMF/AMFSerializer's "User Defined Final Class - Const").
static void set_public_property_impl(Avm2Context* ctx, Avm2Value recv,
                                     const char* name, uint32_t name_len,
                                     Avm2Value value, int allow_const)
{
	if (value_is_null_like(recv))
	{
		avm2_throw_null_or_undefined(ctx, recv, name, name_len);
	}
	if (avm2_value_is_xmlish(recv))
	{
		E4XName n = avm2_e4x_name_from_string(
			ctx, avm2_string_new(ctx, name, name_len), 0);
		if (avm2_xml_set_name(ctx, recv, &n, value)) return;
	}
	uint32_t idx;
	if (recv.kind == AVM2_VALUE_OBJECT && array_index_access(recv.u.obj)
	    && name_as_index(name, name_len, &idx))
	{
		avm2_array_set(ctx, recv.u.obj, idx, value);
		return;
	}
	Avm2PropKey key = avm2_public_key(name, name_len);
	Resolved r;
	if (resolve_key(ctx, recv, &key, 1, &r))
	{
		setproperty_resolved(ctx, recv, &r, name, name_len, value, allow_const);
		return;
	}
	setproperty_miss(ctx, recv, name, name_len, value);
}

void avm2_set_public_property(Avm2Context* ctx, Avm2Value recv,
                              const char* name, uint32_t name_len, Avm2Value value)
{
	set_public_property_impl(ctx, recv, name, name_len, value, 0);
}

void avm2_init_public_property(Avm2Context* ctx, Avm2Value recv,
                               const char* name, uint32_t name_len, Avm2Value value)
{
	set_public_property_impl(ctx, recv, name, name_len, value, 1);
}

int avm2_has_public_property(Avm2Context* ctx, Avm2Value recv,
                             const char* name, uint32_t name_len)
{
	if (value_is_null_like(recv)) return 0;
	if (avm2_value_is_xmlish(recv))
	{
		return avm2_xml_has_property_via_in(
			ctx, recv, avm2_string_new(ctx, name, name_len));
	}
	Avm2PropKey key = avm2_public_key(name, name_len);
	Resolved r;
	if (!resolve_key(ctx, recv, &key, 1, &r)) return 0;
	// A ByteArray index always resolves (index access never forwards to the
	// prototype, even out of bounds), but `in` answers false past the end --
	// as3/ByteArray's "hasAtomProperty false" row, `5 in ba` with length 3.
	if (r.is_bytearray_elem)
	{
		return r.arr_index < avm2_bytearray_ext_of(recv)->len;
	}
	return 1;
}

int avm2_has_own_public_property(Avm2Context* ctx, Avm2Value recv,
                                 const char* name, uint32_t name_len)
{
	if (value_is_null_like(recv)) return 0;
	if (avm2_value_is_xmlish(recv))
	{
		return avm2_xml_has_own(ctx, recv, name, name_len);
	}
	// STRICT publicness: hasOwnProperty must not see AS3-namespace traits
	// (hasownproperty_namespaces).
	const Avm2VTable* vt = avm2_value_vtable(ctx, recv);
	if (avm2_vtable_find_public(vt, name, name_len) != NULL) return 1;
	if (recv.kind != AVM2_VALUE_OBJECT) return 0;
	Avm2Object* obj = recv.u.obj;
	uint32_t idx;
	if (array_index_access(obj) && name_as_index(name, name_len, &idx))
	{
		Avm2Value v = avm2_array_get(obj, idx);
		if (v.kind != AVM2_VALUE_HOLE) return 1;
	}
	if (obj->kind == AVM2_OBJ_VECTOR && name_as_index(name, name_len, &idx))
	{
		return idx < avm2_vector_ext(obj)->length;
	}
	return avm2_object_find_dynamic(obj, name, name_len) != NULL;
}

// ---------------------------------------------------------------------------
// E4X ops: GetDescendants / CheckFilter / Dxns / DxnsLate
// ---------------------------------------------------------------------------

// Static-multiname has_property for with-scope finds (filter expressions).
int avm2_xml_has_property_mn(Avm2Context* ctx, Avm2Value recv,
                             const Avm2AbcFileData* data, uint32_t mn_idx)
{
	const char* name;
	uint32_t name_len;
	avm2_mn_name(data, mn_idx, &name, &name_len);
	E4XName n = avm2_e4x_name_from_mn(ctx, data, mn_idx);
	Avm2XmlExt* xe = avm2_xml_ext_of(recv);
	Avm2XmlListExt* le = avm2_xmllist_ext_of(recv);
	// Any-name / attribute forms have no string equivalent; probe directly.
	if (xe != NULL)
	{
		E4XNode* node = xe->node;
		if (node->kind != E4X_ELEMENT) return 0;
		E4XNode** arr = n.is_attribute ? node->attributes : node->children;
		uint32_t cnt = n.is_attribute ? node->attr_count : node->child_count;
		for (uint32_t i = 0; i < cnt; i++)
		{
			if (avm2_e4x_matches_name(arr[i], &n)) return 1;
		}
		return 0;
	}
	if (le != NULL)
	{
		for (uint32_t i = 0; i < le->count; i++)
		{
			Avm2Value item = avm2_object_value(
				avm2_xml_object_for_node(ctx, le->items[i]));
			if (avm2_xml_has_property_mn(ctx, item, data, mn_idx)) return 1;
		}
		return 0;
	}
	return 0;
}

Avm2Value avm2_op_getdescendants(Avm2Activation* act, Avm2Value v, uint32_t mn_idx)
{
	Avm2Context* ctx = act->ctx;
	if (value_is_null_like(v))
	{
		avm2_throw_null_or_undefined(ctx, v, NULL, 0);
	}
	E4XName name = avm2_e4x_name_from_mn(ctx, act->file->data, mn_idx);
	return avm2_xml_descendants_value(ctx, v, &name);
}

Avm2Value avm2_op_getdescendants_dyn(Avm2Activation* act, Avm2Value v, uint32_t mn_idx,
                                     Avm2Value name_val)
{
	Avm2Context* ctx = act->ctx;
	if (value_is_null_like(v))
	{
		avm2_throw_null_or_undefined(ctx, v, NULL, 0);
	}
	int attr = mn_is_attribute_kind(act->file->data, mn_idx);
	E4XName name = avm2_e4x_name_from_value(ctx, name_val, attr);
	return avm2_xml_descendants_value(ctx, v, &name);
}

Avm2Value avm2_op_getdescendants_rtns(Avm2Activation* act, Avm2Value v, uint32_t mn_idx,
                                       Avm2Value ns_val)
{
	Avm2Context* ctx = act->ctx;
	if (value_is_null_like(v))
	{
		avm2_throw_null_or_undefined(ctx, v, NULL, 0);
	}
	const char* nm;
	uint32_t nm_len;
	avm2_mn_name(act->file->data, mn_idx, &nm, &nm_len);
	E4XName name = e4x_name_from_rtns(ctx, ns_val, avm2_string_new(ctx, nm, nm_len),
	                                  mn_is_attribute_kind(act->file->data, mn_idx));
	return avm2_xml_descendants_value(ctx, v, &name);
}

Avm2Value avm2_op_getdescendants_rtns_l(Avm2Activation* act, Avm2Value v, uint32_t mn_idx,
                                        Avm2Value ns_val, Avm2Value name_val)
{
	Avm2Context* ctx = act->ctx;
	if (value_is_null_like(v))
	{
		avm2_throw_null_or_undefined(ctx, v, NULL, 0);
	}
	const Avm2QNameExt* q = avm2_qname_ext_of(name_val);
	if (q != NULL)
	{
		E4XName name = e4x_name_from_qext(
			q, mn_is_attribute_kind(act->file->data, mn_idx));
		return avm2_xml_descendants_value(ctx, v, &name);
	}
	const Avm2String* s = avm2_coerce_to_string(ctx, name_val);
	E4XName name = e4x_name_from_rtns(ctx, ns_val, s,
	                                  mn_is_attribute_kind(act->file->data, mn_idx));
	return avm2_xml_descendants_value(ctx, v, &name);
}

Avm2Value avm2_op_checkfilter(Avm2Activation* act, Avm2Value v)
{
	Avm2Context* ctx = act->ctx;
	if (value_is_null_like(v))
	{
		avm2_throw_null_or_undefined(ctx, v, NULL, 0);
	}
	if (avm2_value_is_xmlish(v)) return v;
	char cn[160];
	class_name_of(ctx, v, cn, sizeof(cn));
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1123: Filter operator not supported on type %s.", cn);
}

void avm2_op_dxns(Avm2Activation* act, uint32_t str_idx)
{
	act->ctx->dxns = &act->file->data->strings[str_idx];
}

void avm2_op_dxnslate(Avm2Activation* act, Avm2Value v)
{
	Avm2Context* ctx = act->ctx;
	Avm2NamespaceExt* n = avm2_namespace_ext_of(v);
	ctx->dxns = (n != NULL) ? n->uri : avm2_coerce_to_string(ctx, v);
}

#ifdef AVM2_HOTLOOP_PROF
// ---------------------------------------------------------------------------
// Hot-loop counters (§7 rule 6: name the input, not just the function).
// Per GetPropertyStatic IC site (keyed by the site's static cache address):
// hit / miss split by reason, the hit entry's kind, and the distinct receiver
// classes seen. Per Add: the operand-kind pair. Per Coerce site (mn): the
// value's kind and how far avm2_value_is_of_type walks to prove it.
// ---------------------------------------------------------------------------
#define HL_SITES 8192
#define HL_CLS 32
typedef struct
{
	const Avm2InlineCache* ic;
	const Avm2AbcFileData* data;
	uint32_t mn;
	uint64_t n[HL_N];
	uint64_t kind[8];                 // hit entry kind (Avm2PropKind)
	const Avm2Class* cls[HL_CLS];     // distinct receiver classes (first HL_CLS)
	uint32_t ncls;
} HlIcSite;
static HlIcSite g_hl_ic[HL_SITES];
static uint64_t g_hl_ic_calls;
static uint64_t g_hl_add[6];   // ii, nn, in, ni, str-involved, other
typedef struct
{
	const Avm2AbcFileData* data;
	uint32_t mn;
	uint64_t n, null_like, obj, other;
	uint64_t depth[6];                // chain steps to reach the target (5 = more / not found)
	const Avm2Class* cls[HL_CLS];
	uint32_t ncls;
} HlCoerceSite;
static HlCoerceSite g_hl_co[1024];

static void hl_cls_note(const Avm2Class** set, uint32_t* n, const Avm2Class* c)
{
	uint32_t m = *n < HL_CLS ? *n : HL_CLS;
	for (uint32_t i = 0; i < m; i++) if (set[i] == c) return;
	if (*n < HL_CLS) set[*n] = c;
	if (*n <= HL_CLS) (*n)++;   // HL_CLS + 1 = "more than HL_CLS distinct"
}

static void hl_ic_note(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                       const Avm2InlineCache* ic, int outcome,
                       const Avm2PropEntry* hit_entry)
{
	uintptr_t h = ((uintptr_t) ic >> 2) * 2654435761u;
	for (uint32_t k = 0; k < HL_SITES; k++)
	{
		HlIcSite* s = &g_hl_ic[(h + k) & (HL_SITES - 1)];
		if (s->ic != NULL && s->ic != ic) continue;
		if (s->ic == NULL) { s->ic = ic; s->data = act->file->data; s->mn = mn_idx; }
		s->n[outcome]++;
		if (hit_entry != NULL) s->kind[hit_entry->kind & 7]++;
		if (recv.kind == AVM2_VALUE_OBJECT) hl_cls_note(s->cls, &s->ncls, recv.u.obj->cls);
		g_hl_ic_calls++;
		return;
	}
}

static void hl_add_note(Avm2Value a, Avm2Value b)
{
	int ai = a.kind == AVM2_VALUE_INTEGER, an = a.kind == AVM2_VALUE_NUMBER;
	int bi = b.kind == AVM2_VALUE_INTEGER, bn = b.kind == AVM2_VALUE_NUMBER;
	if (ai && bi) g_hl_add[0]++;
	else if (an && bn) g_hl_add[1]++;
	else if (ai && bn) g_hl_add[2]++;
	else if (an && bi) g_hl_add[3]++;
	else if (a.kind == AVM2_VALUE_STRING || b.kind == AVM2_VALUE_STRING) g_hl_add[4]++;
	else g_hl_add[5]++;
}

static void hl_coerce_note(Avm2Activation* act, Avm2Value v, uint32_t mn_idx)
{
	const Avm2AbcFileData* data = act->file->data;
	uint32_t h = (mn_idx * 2654435761u) ^ (uint32_t) ((uintptr_t) data >> 3);
	for (uint32_t k = 0; k < 1024; k++)
	{
		HlCoerceSite* s = &g_hl_co[(h + k) & 1023];
		if (s->n != 0 && (s->mn != mn_idx || s->data != data)) continue;
		s->mn = mn_idx; s->data = data; s->n++;
		if (v.kind == AVM2_VALUE_NULL || v.kind == AVM2_VALUE_UNDEFINED) { s->null_like++; return; }
		if (v.kind != AVM2_VALUE_OBJECT) { s->other++; return; }
		s->obj++;
		hl_cls_note(s->cls, &s->ncls, v.u.obj->cls);
		Avm2Class* target = avm2_class_for_mn(act->ctx, act->file, mn_idx);
		uint32_t d = 0;
		Avm2Class* c = v.u.obj->cls;
		while (c != NULL && c != target && d < 5) { c = c->super_class; d++; }
		s->depth[(c == target) ? d : 5]++;
		return;
	}
}

static void hl_cls_print(const Avm2Class* const* set, uint32_t n)
{
	for (uint32_t i = 0; i < n && i < HL_CLS; i++)
	{
		printf(" %.*s", set[i] ? (int) set[i]->name.name_len : 4,
		       set[i] ? set[i]->name.name : "NULL");
	}
	if (n > HL_CLS) printf(" (+more)");
}

void avm2_hotloop_prof_tick(Avm2Context* ctx)
{
	static uint32_t tick;
	(void) ctx;
	tick++;
	if (g_hl_ic_calls >= 1000000)
	{
		printf("[HLPROF] tick=%u ic_calls=%llu add ii=%llu nn=%llu in=%llu ni=%llu str=%llu other=%llu\n",
		       tick, (unsigned long long) g_hl_ic_calls,
		       (unsigned long long) g_hl_add[0], (unsigned long long) g_hl_add[1],
		       (unsigned long long) g_hl_add[2], (unsigned long long) g_hl_add[3],
		       (unsigned long long) g_hl_add[4], (unsigned long long) g_hl_add[5]);
		for (uint32_t i = 0; i < HL_SITES; i++)
		{
			HlIcSite* s = &g_hl_ic[i];
			uint64_t tot = 0;
			for (int k = 0; k < HL_N; k++) tot += s->n[k];
			if (s->ic == NULL || tot < 10000) continue;
			const char* nm; uint32_t nl;
			avm2_mn_name(s->data, s->mn, &nm, &nl);
			printf("[HLPROF-IC] site=%p mn=%u %.*s calls=%llu hit=%llu nonobj=%llu novt=%llu cold=%llu vt=%llu count=%llu kind[slot=%llu method=%llu getter=%llu getset=%llu other=%llu] ncls=%u:",
			       (void*) s->ic, s->mn, (int) nl, nm, (unsigned long long) tot,
			       (unsigned long long) s->n[HL_HIT], (unsigned long long) s->n[HL_MISS_NONOBJ],
			       (unsigned long long) s->n[HL_MISS_NOVT], (unsigned long long) s->n[HL_MISS_COLD],
			       (unsigned long long) s->n[HL_MISS_VT], (unsigned long long) s->n[HL_MISS_COUNT],
			       (unsigned long long) s->kind[AVM2_PROP_SLOT], (unsigned long long) s->kind[AVM2_PROP_METHOD],
			       (unsigned long long) s->kind[AVM2_PROP_GETTER], (unsigned long long) s->kind[AVM2_PROP_GETSET],
			       (unsigned long long) (s->kind[3] + s->kind[5] + s->kind[6] + s->kind[7]),
			       s->ncls);
			hl_cls_print(s->cls, s->ncls);
			printf("\n");
		}
		for (uint32_t i = 0; i < 1024; i++)
		{
			HlCoerceSite* s = &g_hl_co[i];
			if (s->n < 10000) continue;
			const char* nm; uint32_t nl;
			avm2_mn_name(s->data, s->mn, &nm, &nl);
			printf("[HLPROF-CO] mn=%u %.*s n=%llu null=%llu obj=%llu other=%llu depth0..4,miss=%llu,%llu,%llu,%llu,%llu,%llu ncls=%u:",
			       s->mn, (int) nl, nm, (unsigned long long) s->n, (unsigned long long) s->null_like,
			       (unsigned long long) s->obj, (unsigned long long) s->other,
			       (unsigned long long) s->depth[0], (unsigned long long) s->depth[1],
			       (unsigned long long) s->depth[2], (unsigned long long) s->depth[3],
			       (unsigned long long) s->depth[4], (unsigned long long) s->depth[5], s->ncls);
			hl_cls_print(s->cls, s->ncls);
			printf("\n");
		}
		fflush(stdout);
	}
	memset(g_hl_ic, 0, sizeof(g_hl_ic));
	memset(g_hl_co, 0, sizeof(g_hl_co));
	memset(g_hl_add, 0, sizeof(g_hl_add));
	g_hl_ic_calls = 0;
}
#endif
