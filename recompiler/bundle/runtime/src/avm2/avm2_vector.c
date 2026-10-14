// Vector builtin (__AS3__.vec::Vector) — port of Ruffle vector.rs
// (VectorStorage), object/vector_object.rs (index access + error
// semantics), object/class_object.rs apply/parametrize, and
// globals/vector.rs + VectorObject.as (method family).
//
// Model: one Avm2Class per distinct type parameter T (identity matters for
// is/as/coercion), cached in an application list keyed by the param class.
// The four builtin specializations (int/uint/Number/*) extend Object;
// runtime-created Vector.<T> classes extend Vector.<*> and inherit its
// vtable + prototype chain (Ruffle with_type_param).

#include <ctype.h>
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
#include <memory/heap.h>

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

Avm2VectorExt* avm2_vector_ext(Avm2Object* obj)
{
	if (obj == NULL || obj->kind != AVM2_OBJ_VECTOR) return NULL;
	return (Avm2VectorExt*) obj->native_ext;
}

static int vec_type_is_numeric(Avm2Context* ctx, Avm2Class* t)
{
	return t == ctx->builtins.int_class || t == ctx->builtins.uint_class
	       || t == ctx->builtins.number_class;
}

// Default element (Ruffle VectorStorage::default): Integer 0 for numeric
// element types, null otherwise (including Vector.<*>).
static Avm2Value vec_default(Avm2Context* ctx, const Avm2VectorExt* ext)
{
	if (ext->value_type != NULL && vec_type_is_numeric(ctx, ext->value_type))
	{
		return avm2_integer(0);
	}
	return avm2_null();
}

static void vec_reserve(Avm2Context* ctx, Avm2VectorExt* ext, uint32_t need)
{
	if (need <= ext->cap) return;
	uint32_t new_cap = ext->cap == 0 ? 8 : ext->cap;
	while (new_cap < need) new_cap *= 2;
	Avm2Value* grown = avm2_alloc(ctx, new_cap * sizeof(Avm2Value));
	if (ext->length > 0)
	{
		memcpy(grown, ext->elems, ext->length * sizeof(Avm2Value));
	}
	if (ext->elems != NULL) heap_free(ctx->app, ext->elems);  // sole owner (see array_reserve)
	ext->elems = grown;
	ext->cap = new_cap;
}

_Noreturn static void vec_throw_1126(Avm2Context* ctx)
{
	avm2_throw_error(ctx, ctx->builtins.range_error_class,
	                 "Error #1126: Cannot change the length of a fixed Vector.");
}

_Noreturn static void vec_throw_1125(Avm2Context* ctx, double index, uint32_t range)
{
	char ib[40];
	avm2_format_number(ib, sizeof(ib), index);
	avm2_throw_error(ctx, ctx->builtins.range_error_class,
	                 "Error #1125: The index %s is out of range %u.", ib, range);
}

static void vec_check_fixed(Avm2Context* ctx, const Avm2VectorExt* ext)
{
	if (ext->fixed) vec_throw_1126(ctx);
}

// Resize with defaults (fixed check included — Ruffle resize).
static void vec_resize(Avm2Context* ctx, Avm2VectorExt* ext, uint32_t new_len)
{
	vec_check_fixed(ctx, ext);
	if (new_len > ext->length)
	{
		vec_reserve(ctx, ext, new_len);
		Avm2Value d = vec_default(ctx, ext);
		for (uint32_t i = ext->length; i < new_len; i++)
		{
			ext->elems[i] = d;
		}
	}
	ext->length = new_len;
}

// Coerce a value to the element type (Ruffle coerce_to_type against
// value_type_for_coercion; Vector.<*> coerces against Object).
static Avm2Value vec_coerce(Avm2Context* ctx, const Avm2VectorExt* ext, Avm2Value v)
{
	Avm2Class* t = ext->value_type != NULL ? ext->value_type
	                                       : ctx->builtins.object_class;
	return avm2_coerce_to_class(ctx, t, v);
}

Avm2Object* avm2_vector_new(Avm2Context* ctx, Avm2Class* vec_class,
                            uint32_t length, int fixed)
{
	Avm2Object* obj = avm2_object_alloc(ctx, AVM2_OBJ_VECTOR, 1);
	obj->cls = vec_class;
	obj->vtable = &vec_class->ivtable;
	obj->proto = vec_class->prototype_obj;
	Avm2VectorExt* ext = avm2_alloc(ctx, sizeof(Avm2VectorExt));
	memset(ext, 0, sizeof(Avm2VectorExt));
	ext->value_type = vec_class->vector_param;
	obj->native_ext = ext;
	if (length > 0) vec_resize(ctx, ext, length);
	ext->fixed = fixed ? 1 : 0;
	return obj;
}

// ---------------------------------------------------------------------------
// Index access (Ruffle vector_object.rs)
// ---------------------------------------------------------------------------

Avm2Value avm2_vector_get_index(Avm2Context* ctx, Avm2Object* obj, uint32_t idx)
{
	Avm2VectorExt* ext = avm2_vector_ext(obj);
	if (idx < ext->length) return ext->elems[idx];
	vec_throw_1125(ctx, (double) idx, ext->length);
}

// set_element: coerce, undefined/null → default, append allowed at exactly
// length when not fixed.
void avm2_vector_set_index(Avm2Context* ctx, Avm2Object* obj, uint32_t idx, Avm2Value v)
{
	Avm2VectorExt* ext = avm2_vector_ext(obj);
	Avm2Value cv = vec_coerce(ctx, ext, v);
	if (cv.kind == AVM2_VALUE_UNDEFINED || cv.kind == AVM2_VALUE_NULL)
	{
		cv = vec_default(ctx, ext);
	}
	if (!ext->fixed && idx == ext->length)
	{
		vec_reserve(ctx, ext, idx + 1);
		ext->length = idx + 1;
	}
	if (idx < ext->length)
	{
		ext->elems[idx] = cv;
		return;
	}
	vec_throw_1125(ctx, (double) idx, ext->length);
}

// Parse a property name the way Ruffle's as_vector_index does (full f64
// parse). Returns 0 if not numeric.
static int vec_name_as_f64(const char* name, uint32_t len, double* out)
{
	char buf[40];
	if (len == 0 || len >= sizeof(buf)) return 0;
	// strtod is laxer than Rust's f64 parse: reject leading space and hex.
	if (isspace((unsigned char) name[0])) return 0;
	if (len > 1 && (name[1] == 'x' || name[1] == 'X')) return 0;
	memcpy(buf, name, len);
	buf[len] = '\0';
	char* end = NULL;
	double d = strtod(buf, &end);
	if (end != buf + len) return 0;
	*out = d;
	return 1;
}

int avm2_vector_name_access(Avm2Context* ctx, Avm2Object* obj, const char* name,
                            uint32_t name_len, Avm2Value* out, Avm2Value set_value)
{
	double d;
	if (!vec_name_as_f64(name, name_len, &d)) return 0;
	Avm2VectorExt* ext = avm2_vector_ext(obj);

	// Rust `as u32` saturating cast.
	uint32_t u;
	if (d != d) u = 0;
	else if (d <= 0.0) u = 0;
	else if (d >= 4294967295.0) u = 0xFFFFFFFFu;
	else u = (uint32_t) d;

	if ((double) u == d)
	{
		if (out != NULL)
		{
			if (u < ext->length)
			{
				*out = ext->elems[u];
				return 1;
			}
			// fail_read_error. A name that IS a valid u32 index is a
			// RangeError in every SWF version: avmplus routes an integer
			// index straight at getUintProperty, which range-checks. Ruffle
			// gates #1125 behind version >= 11 and answers #1069 below it,
			// but as3/Vector/nonindexproperty/v10 asserts #1125 for reads of
			// max_int28_m1..max_uint_m1 on a v10 movie. Only non-u32 names
			// (fractional, negative, non-numeric) keep the versioned
			// ReferenceError / proto-chain behavior, below.
			vec_throw_1125(ctx, d, ext->length);
		}
		avm2_vector_set_index(ctx, obj, u, set_value);
		return 1;
	}

	// Numeric but not a valid u32 index.
	if (out != NULL)
	{
		if (ctx->swf_version >= 11) vec_throw_1125(ctx, d, ext->length);
		if (d > 0.0) avm2_throw_1069(ctx, name, name_len, obj->cls);
		return 0;
	}
	if (ctx->swf_version >= 11) vec_throw_1125(ctx, d, ext->length);
	{
		char cn[160];
		avm2_class_qname_buf(obj->cls, cn, sizeof(cn));
		avm2_throw_error(ctx, ctx->builtins.reference_error_class,
		                 "Error #1056: Cannot create property %.*s on %s.",
		                 (int) name_len, name, cn);
	}
}

// ---------------------------------------------------------------------------
// Parameterized-class cache (Ruffle ClassObject::parametrize)
// ---------------------------------------------------------------------------

typedef struct VecApp
{
	struct VecApp* next;
	Avm2Class* param;  // NULL = *
	Avm2Class* cls;
} VecApp;

static VecApp* g_vec_apps;

static void vec_app_add(Avm2Context* ctx, Avm2Class* param, Avm2Class* cls)
{
	VecApp* a = avm2_alloc(ctx, sizeof(VecApp));
	a->param = param;
	a->cls = cls;
	a->next = g_vec_apps;
	g_vec_apps = a;
}

// "pkg::Name" or "Name" of a class, for Vector.<...> naming.
static int vec_qualified_name(const Avm2Class* cls, char* buf, int size)
{
	if (cls->name.ns_len > 0)
	{
		return snprintf(buf, size, "%.*s::%.*s",
		                (int) cls->name.ns_len, cls->name.ns_uri,
		                (int) cls->name.name_len, cls->name.name);
	}
	return snprintf(buf, size, "%.*s", (int) cls->name.name_len, cls->name.name);
}

static Avm2Value vector_construct(Avm2Context* ctx, Avm2Class* cls,
                                  const Avm2Value* args, uint32_t argc);
static Avm2Value vector_call(Avm2Context* ctx, Avm2Class* cls,
                             const Avm2Value* args, uint32_t argc);

static void vec_stamp_class(Avm2Class* cls, Avm2Class* param)
{
	cls->is_vector = 1;
	cls->vector_param = param;
	cls->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	cls->native_construct = vector_construct;
	cls->native_call = vector_call;
}

Avm2Class* avm2_vector_apply(Avm2Context* ctx, Avm2Class* param)
{
	for (VecApp* a = g_vec_apps; a != NULL; a = a->next)
	{
		if (a->param == param) return a->cls;
	}
	// Not int/uint/Number/* → a Vector.<*>-derived application
	// (Ruffle with_type_param). The name is built at its exact size rather
	// than through fixed buffers: applications nest without limit, and
	// as3/Vector/nested builds 500 levels (~4.5KB) and then compares the tail
	// of the class's toString.
	int pn = vec_qualified_name(param, NULL, 0);
	if (pn < 0) pn = 0;
	char* name = avm2_alloc(ctx, (uint32_t) pn + 10);  // "Vector.<" + p + ">" + NUL
	memcpy(name, "Vector.<", 8);
	vec_qualified_name(param, name + 8, pn + 1);
	name[8 + pn] = '>';
	name[9 + pn] = '\0';

	Avm2Class* cls = avm2_builtin_class(ctx, "__AS3__.vec", name,
	                                    ctx->builtins.vector_object_class);
	vec_stamp_class(cls, param);
	vec_app_add(ctx, param, cls);
	return cls;
}

Avm2Value avm2_op_applytype(Avm2Activation* act, Avm2Value base,
                            const Avm2Value* params, uint32_t num_params)
{
	Avm2Context* ctx = act->ctx;
	if (base.kind != AVM2_VALUE_OBJECT || base.u.obj == NULL
	    || base.u.obj->kind != AVM2_OBJ_CLASS
	    || !base.u.obj->class_ref->is_generic_vector)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1127: Type application attempted on a "
		                 "non-parameterized type.");
	}
	Avm2Class* generic = base.u.obj->class_ref;
	if (num_params != 1)
	{
		char cn[160];
		avm2_class_qname_buf(generic, cn, sizeof(cn));
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1128: Incorrect number of type parameters for "
		                 "%s. Expected 1, got %u.", cn, num_params);
	}
	Avm2Class* param = NULL;
	if (params[0].kind != AVM2_VALUE_NULL)
	{
		if (params[0].kind != AVM2_VALUE_OBJECT
		    || params[0].u.obj->kind != AVM2_OBJ_CLASS)
		{
			avm2_throw_error(ctx, ctx->builtins.verify_error_class,
			                 "Error #1107: The ABC data is corrupt, attempt "
			                 "to read out of bounds.");
		}
		param = params[0].u.obj->class_ref;
	}
	return avm2_object_value(avm2_vector_apply(ctx, param)->class_object);
}

// Name-based lookup ("Vector.<...>" with optional __AS3__.vec prefix), used
// by getDefinitionByName / ApplicationDomain (vector_class test): resolving
// by name creates the application on demand, exactly like avmplus.
Avm2Class* avm2_vector_class_by_name(Avm2Context* ctx, const char* s, uint32_t len)
{
	static const char NS_COLON[] = "__AS3__.vec::";
	static const char NS_DOT[] = "__AS3__.vec.";
	if (len > sizeof(NS_COLON) - 1 && memcmp(s, NS_COLON, sizeof(NS_COLON) - 1) == 0)
	{
		s += sizeof(NS_COLON) - 1;
		len -= sizeof(NS_COLON) - 1;
	}
	else if (len > sizeof(NS_DOT) - 1 && memcmp(s, NS_DOT, sizeof(NS_DOT) - 1) == 0
	         && len > sizeof(NS_DOT) - 1 + 7
	         && memcmp(s + sizeof(NS_DOT) - 1, "Vector.<", 8) == 0)
	{
		s += sizeof(NS_DOT) - 1;
		len -= sizeof(NS_DOT) - 1;
	}
	if (len < 10 || memcmp(s, "Vector.<", 8) != 0 || s[len - 1] != '>') return NULL;
	const char* inner = s + 8;
	uint32_t inner_len = len - 9;
	if (inner_len == 0) return NULL;

	if (inner_len == 1 && inner[0] == '*')
	{
		return ctx->builtins.vector_object_class;
	}
	// Nested vector name?
	Avm2Class* param = avm2_vector_class_by_name(ctx, inner, inner_len);
	if (param == NULL)
	{
		int found = 0;
		Avm2Value v = avm2_find_definition(ctx, inner, inner_len, &found);
		if (!found || v.kind != AVM2_VALUE_OBJECT
		    || v.u.obj->kind != AVM2_OBJ_CLASS)
		{
			return NULL;
		}
		param = v.u.obj->class_ref;
	}
	return avm2_vector_apply(ctx, param);
}

// ---------------------------------------------------------------------------
// Constructor + class call (conversion)
// ---------------------------------------------------------------------------

static Avm2Value vector_generic_construct(Avm2Context* ctx, Avm2Class* cls,
                                          const Avm2Value* args, uint32_t argc)
{
	(void) cls;
	(void) args;
	(void) argc;
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1007: Instantiation attempted on a non-constructor.");
}

static Avm2Value vector_construct(Avm2Context* ctx, Avm2Class* cls,
                                  const Avm2Value* args, uint32_t argc)
{
	uint32_t length = argc > 0 ? avm2_coerce_to_u32(ctx, args[0]) : 0;
	int fixed = argc > 1 ? avm2_coerce_to_boolean(args[1]) : 0;
	return avm2_object_value(avm2_vector_new(ctx, cls, length, fixed));
}

// Debug string for 1034 messages (mirrors avm2_coerce_to_class's format).
static void value_debug_str(Avm2Context* ctx, Avm2Value v, char* buf, int size)
{
	if (v.kind == AVM2_VALUE_STRING)
	{
		snprintf(buf, size, "\"%.*s\"",
		         v.u.str->len > 100 ? 100 : (int) v.u.str->len, v.u.str->utf8);
	}
	else if (v.kind == AVM2_VALUE_OBJECT)
	{
		char vq[128];
		avm2_class_qname_buf(avm2_value_class(ctx, v), vq, sizeof(vq));
		snprintf(buf, size, "%s@00000000000", vq);
	}
	else
	{
		const Avm2String* s = avm2_coerce_to_string(ctx, v);
		snprintf(buf, size, "%.*s", s->len > 100 ? 100 : (int) s->len, s->utf8);
	}
}

// Vector.<T>(other): conversion call (Ruffle globals/vector.rs call_handler).
static Avm2Value vector_call(Avm2Context* ctx, Avm2Class* cls,
                             const Avm2Value* args, uint32_t argc)
{
	if (argc != 1)
	{
		avm2_throw_error(ctx, ctx->builtins.argument_error_class,
		                 "Error #1112: Argument count mismatch on class coercion. "
		                 " Expected 1, got %u.", argc);
	}
	Avm2Value arg = args[0];
	if (arg.kind != AVM2_VALUE_OBJECT)
	{
		char dbg[128];
		char cq[192];
		value_debug_str(ctx, arg, dbg, sizeof(dbg));
		avm2_class_qname_buf(cls, cq, sizeof(cq));
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1034: Type Coercion failed: cannot convert %s to %s.",
		                 dbg, cq);
	}
	if (arg.u.obj->cls == cls) return arg;

	Avm2Value lenv = avm2_get_public_property(ctx, arg, "length", 6, NULL);
	int32_t length = avm2_coerce_to_i32(ctx, lenv);

	Avm2Object* out = avm2_vector_new(ctx, cls, 0, 0);
	Avm2VectorExt* oext = avm2_vector_ext(out);
	for (int32_t i = 0; i < length; i++)
	{
		char nb[16];
		int nl = snprintf(nb, sizeof(nb), "%d", i);
		Avm2Value item;
		Avm2Object* src = arg.u.obj;
		if (src->kind == AVM2_OBJ_VECTOR)
		{
			item = avm2_vector_get_index(ctx, src, (uint32_t) i);
		}
		else if (src->kind == AVM2_OBJ_ARRAY)
		{
			item = avm2_array_get(src, (uint32_t) i);
			if (item.kind == AVM2_VALUE_HOLE)
			{
				item = avm2_get_public_property(ctx, arg, nb, (uint32_t) nl, NULL);
			}
		}
		else
		{
			item = avm2_get_public_property(ctx, arg, nb, (uint32_t) nl, NULL);
		}
		Avm2Value cv = vec_coerce(ctx, oext, item);
		vec_reserve(ctx, oext, oext->length + 1);
		oext->elems[oext->length++] = cv;
	}
	return avm2_object_value(out);
}

// ---------------------------------------------------------------------------
// Methods (Ruffle globals/vector.rs + VectorObject.as)
// ---------------------------------------------------------------------------

static Avm2Object* this_vector(Avm2Activation* act)
{
	if (act->this_val.kind == AVM2_VALUE_OBJECT
	    && act->this_val.u.obj->kind == AVM2_OBJ_VECTOR)
	{
		return act->this_val.u.obj;
	}
	return NULL;
}

static Avm2Value arg_or_undef(Avm2Activation* act, uint32_t i)
{
	return i < act->argc ? act->args[i] : avm2_undefined();
}

// ArrayIter element read on a vector: reads past the CURRENT length throw
// 1125 (a callback shrank the vector mid-iteration).
static Avm2Value vec_iter_get(Avm2Context* ctx, Avm2Object* vec, uint32_t i)
{
	Avm2VectorExt* ext = avm2_vector_ext(vec);
	if (i >= ext->length) vec_throw_1125(ctx, (double) i, ext->length);
	return ext->elems[i];
}

static Avm2Value vec_get_length(Avm2Activation* act)
{
	Avm2Object* v = this_vector(act);
	return avm2_uint_value(v != NULL ? avm2_vector_ext(v)->length : 0);
}

static Avm2Value vec_set_length(Avm2Activation* act)
{
	Avm2Object* v = this_vector(act);
	if (v != NULL && act->argc > 0)
	{
		vec_resize(act->ctx, avm2_vector_ext(v),
		           avm2_coerce_to_u32(act->ctx, act->args[0]));
	}
	return avm2_undefined();
}

static Avm2Value vec_get_fixed(Avm2Activation* act)
{
	Avm2Object* v = this_vector(act);
	return avm2_bool(v != NULL && avm2_vector_ext(v)->fixed);
}

static Avm2Value vec_set_fixed(Avm2Activation* act)
{
	Avm2Object* v = this_vector(act);
	if (v != NULL && act->argc > 0)
	{
		avm2_vector_ext(v)->fixed = avm2_coerce_to_boolean(act->args[0]) ? 1 : 0;
	}
	return avm2_undefined();
}

// join: null/undefined elements render as "null" (Ruffle globals/vector.rs).
static const Avm2String* vec_join_inner(Avm2Context* ctx, Avm2Object* vec,
                                        const Avm2String* sep)
{
	Avm2VectorExt* ext = avm2_vector_ext(vec);
	const Avm2String* out = avm2_string_from_literal(ctx, "");
	for (uint32_t i = 0; i < ext->length; i++)
	{
		if (i > 0) out = avm2_string_concat(ctx, out, sep);
		Avm2Value v = ext->elems[i];
		if (v.kind == AVM2_VALUE_UNDEFINED || v.kind == AVM2_VALUE_NULL)
		{
			out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, "null"));
		}
		else
		{
			out = avm2_string_concat(ctx, out, avm2_coerce_to_string(ctx, v));
		}
	}
	return out;
}

static Avm2Value vec_join(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	const Avm2String* sep;
	if (act->argc > 0 && act->args[0].kind != AVM2_VALUE_UNDEFINED)
	{
		sep = avm2_coerce_to_string(ctx, act->args[0]);
	}
	else
	{
		sep = avm2_string_from_literal(ctx, ",");
	}
	return avm2_string(vec_join_inner(ctx, v, sep));
}

static Avm2Value vec_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	return avm2_string(vec_join_inner(ctx, v, avm2_string_from_literal(ctx, ",")));
}

// toLocaleString (VectorObject.as): undefined/null render via string concat
// ("undefined"/"null"); others call element.toLocaleString().
static Avm2Value vec_to_locale_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* vec = this_vector(act);
	if (vec == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(vec);
	const Avm2String* out = avm2_string_from_literal(ctx, "");
	for (uint32_t i = 0; i < ext->length; i++)
	{
		Avm2Value v = ext->elems[i];
		if (v.kind == AVM2_VALUE_UNDEFINED || v.kind == AVM2_VALUE_NULL)
		{
			out = avm2_string_concat(ctx, out, avm2_coerce_to_string(ctx, v));
		}
		else
		{
			Avm2Value s = avm2_call_public_property(ctx, v, "toLocaleString", 14,
			                                        NULL, 0);
			out = avm2_string_concat(ctx, out, avm2_coerce_to_string(ctx, s));
		}
		if (i != ext->length - 1)
		{
			out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, ","));
		}
	}
	return avm2_string(out);
}

static Avm2Value vec_push(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(v);
	// Pushing nothing still throws on a fixed vector.
	vec_check_fixed(ctx, ext);
	// Coerce first (side effects must not observe a partial push).
	Avm2Value coerced[64];
	Avm2Value* cs = act->argc <= 64 ? coerced
	                                : avm2_alloc(ctx, act->argc * sizeof(Avm2Value));
	for (uint32_t i = 0; i < act->argc; i++)
	{
		cs[i] = vec_coerce(ctx, ext, act->args[i]);
	}
	vec_check_fixed(ctx, ext);
	vec_reserve(ctx, ext, ext->length + act->argc);
	for (uint32_t i = 0; i < act->argc; i++)
	{
		ext->elems[ext->length++] = cs[i];
	}
	return avm2_uint_value(ext->length);
}

static Avm2Value vec_pop(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(v);
	vec_check_fixed(ctx, ext);
	if (ext->length == 0)
	{
		if (ext->value_type != NULL && vec_type_is_numeric(ctx, ext->value_type))
		{
			return avm2_integer(0);
		}
		return avm2_undefined();
	}
	return ext->elems[--ext->length];
}

static Avm2Value vec_shift(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(v);
	vec_check_fixed(ctx, ext);
	if (ext->length == 0)
	{
		if (ext->value_type != NULL && vec_type_is_numeric(ctx, ext->value_type))
		{
			return avm2_integer(0);
		}
		return avm2_undefined();
	}
	Avm2Value out = ext->elems[0];
	memmove(ext->elems, ext->elems + 1, (ext->length - 1) * sizeof(Avm2Value));
	ext->length--;
	return out;
}

static Avm2Value vec_unshift(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(v);
	Avm2Value coerced[64];
	Avm2Value* cs = act->argc <= 64 ? coerced
	                                : avm2_alloc(ctx, act->argc * sizeof(Avm2Value));
	for (uint32_t i = 0; i < act->argc; i++)
	{
		cs[i] = vec_coerce(ctx, ext, act->args[i]);
	}
	vec_check_fixed(ctx, ext);
	if (act->argc > 0)
	{
		vec_reserve(ctx, ext, ext->length + act->argc);
		memmove(ext->elems + act->argc, ext->elems, ext->length * sizeof(Avm2Value));
		for (uint32_t i = 0; i < act->argc; i++)
		{
			ext->elems[i] = cs[i];
		}
		ext->length += act->argc;
	}
	return avm2_uint_value(ext->length);
}

// clamp_parameter_index (Ruffle vector.rs).
static uint32_t vec_clamp_index(const Avm2VectorExt* ext, int32_t pos)
{
	if (pos < 0)
	{
		int64_t w = (int64_t) pos + (int64_t) ext->length;
		return w < 0 ? 0 : (uint32_t) w;
	}
	return (uint32_t) pos < ext->length ? (uint32_t) pos : ext->length;
}

static Avm2Value vec_insert_at(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(v);
	int32_t index = act->argc > 0 ? avm2_coerce_to_i32(ctx, act->args[0]) : 0;
	Avm2Value value = vec_coerce(ctx, ext, arg_or_undef(act, 1));
	vec_check_fixed(ctx, ext);
	uint32_t pos = vec_clamp_index(ext, index);
	vec_reserve(ctx, ext, ext->length + 1);
	memmove(ext->elems + pos + 1, ext->elems + pos,
	        (ext->length - pos) * sizeof(Avm2Value));
	ext->elems[pos] = value;
	ext->length++;
	return avm2_undefined();
}

static Avm2Value vec_remove_at(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(v);
	vec_check_fixed(ctx, ext);
	int32_t index = act->argc > 0 ? avm2_coerce_to_i32(ctx, act->args[0]) : 0;
	int64_t pos = index;
	if (pos < 0)
	{
		pos += (int64_t) ext->length;
		if (pos < 0) pos = 0;
	}
	if (pos >= (int64_t) ext->length)
	{
		vec_throw_1125(ctx, (double) pos, ext->length);
	}
	Avm2Value out = ext->elems[pos];
	memmove(ext->elems + pos, ext->elems + pos + 1,
	        (ext->length - (uint32_t) pos - 1) * sizeof(Avm2Value));
	ext->length--;
	return out;
}

static Avm2Value vec_reverse(Avm2Activation* act)
{
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(v);
	for (uint32_t i = 0; i < ext->length / 2; i++)
	{
		Avm2Value t = ext->elems[i];
		ext->elems[i] = ext->elems[ext->length - 1 - i];
		ext->elems[ext->length - 1 - i] = t;
	}
	return act->this_val;
}

static Avm2Value vec_concat(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(v);

	Avm2Object* out = avm2_vector_new(ctx, v->cls, 0, 0);
	Avm2VectorExt* oext = avm2_vector_ext(out);
	vec_reserve(ctx, oext, ext->length);
	memcpy(oext->elems, ext->elems, ext->length * sizeof(Avm2Value));
	oext->length = ext->length;
	uint32_t original_length = ext->length;

	// Args coerce to the receiver's BASE builtin class: the exact class for
	// int/uint/Number vectors, Vector.<*> for the whole object family
	// (Ruffle vector_object.rs concat passes class_defs().object_vector) —
	// custom Vector.<T> args pass because they extend Vector.<*>.
	Avm2Class* base = v->cls;
	if (base != ctx->builtins.vector_int_class
	    && base != ctx->builtins.vector_uint_class
	    && base != ctx->builtins.vector_double_class)
	{
		base = ctx->builtins.vector_object_class;
	}
	int swf10 = ctx->swf_version < 11;
	for (uint32_t a = 0; a < act->argc; a++)
	{
		Avm2Value arg = act->args[a];
		if (arg.kind == AVM2_VALUE_NULL || arg.kind == AVM2_VALUE_UNDEFINED)
		{
			avm2_throw_null_or_undefined(ctx, arg, NULL, 0);
		}
		Avm2Value coerced = avm2_coerce_to_class(ctx, base, arg);
		Avm2VectorExt* aext = avm2_vector_ext(coerced.u.obj);
		for (uint32_t i = 0; i < aext->length; i++)
		{
			Avm2Value cv = vec_coerce(ctx, oext, aext->elems[i]);
			if (swf10)
			{
				// Bugzilla 504525: SWF10 concat inserts at original_length+i.
				uint32_t pos = vec_clamp_index(oext, (int32_t) (original_length + i));
				vec_reserve(ctx, oext, oext->length + 1);
				memmove(oext->elems + pos + 1, oext->elems + pos,
				        (oext->length - pos) * sizeof(Avm2Value));
				oext->elems[pos] = cv;
				oext->length++;
			}
			else
			{
				vec_reserve(ctx, oext, oext->length + 1);
				oext->elems[oext->length++] = cv;
			}
		}
	}
	return avm2_object_value(out);
}

static Avm2Value vec_slice(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(v);
	int32_t from = act->argc > 0 ? avm2_coerce_to_i32(ctx, act->args[0]) : 0;
	int32_t to = (act->argc > 1 && act->args[1].kind != AVM2_VALUE_UNDEFINED)
		? avm2_coerce_to_i32(ctx, act->args[1]) : 2147483647;
	uint32_t f = vec_clamp_index(ext, from);
	uint32_t t = vec_clamp_index(ext, to);
	Avm2Object* out = avm2_vector_new(ctx, v->cls, 0, 0);
	Avm2VectorExt* oext = avm2_vector_ext(out);
	if (t > f)
	{
		vec_reserve(ctx, oext, t - f);
		memcpy(oext->elems, ext->elems + f, (t - f) * sizeof(Avm2Value));
		oext->length = t - f;
	}
	return avm2_object_value(out);
}

// splice: NO fixed check (avmplus bug compat —
// vector_splice_fixed_bug_compat).
static Avm2Value vec_splice(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(v);
	int32_t start_len = act->argc > 0 ? avm2_coerce_to_i32(ctx, act->args[0]) : 0;
	int32_t delete_len = act->argc > 1 ? avm2_coerce_to_i32(ctx, act->args[1]) : 0;
	uint32_t start = vec_clamp_index(ext, start_len);
	uint32_t end;
	if (delete_len < 0)
	{
		end = vec_clamp_index(ext, delete_len);
	}
	else
	{
		uint64_t e = (uint64_t) start + (uint64_t) delete_len;
		end = e > ext->length ? ext->length : (uint32_t) e;
	}
	if (end < start) end = start;

	uint32_t ins = act->argc > 2 ? act->argc - 2 : 0;
	Avm2Value coerced[64];
	Avm2Value* cs = ins <= 64 ? coerced : avm2_alloc(ctx, ins * sizeof(Avm2Value));
	for (uint32_t i = 0; i < ins; i++)
	{
		cs[i] = vec_coerce(ctx, ext, act->args[2 + i]);
	}

	uint32_t del = end - start;
	Avm2Object* removed = avm2_vector_new(ctx, v->cls, 0, 0);
	Avm2VectorExt* rext = avm2_vector_ext(removed);
	if (del > 0)
	{
		vec_reserve(ctx, rext, del);
		memcpy(rext->elems, ext->elems + start, del * sizeof(Avm2Value));
		rext->length = del;
	}
	uint32_t new_len = ext->length - del + ins;
	vec_reserve(ctx, ext, new_len);
	memmove(ext->elems + start + ins, ext->elems + end,
	        (ext->length - end) * sizeof(Avm2Value));
	for (uint32_t i = 0; i < ins; i++)
	{
		ext->elems[start + i] = cs[i];
	}
	ext->length = new_len;
	return avm2_object_value(removed);
}

static Avm2Value vec_index_of(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_integer(-1);
	Avm2VectorExt* ext = avm2_vector_ext(v);
	Avm2Value search = arg_or_undef(act, 0);
	double fromf = act->argc > 1 ? avm2_coerce_to_number(ctx, act->args[1]) : 0.0;
	uint32_t from;
	if (fromf < 0.0)
	{
		int64_t w = (int64_t) ext->length + (int64_t) fromf;
		from = w < 0 ? 0 : (uint32_t) w;
	}
	else
	{
		from = fromf >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t) fromf;
	}
	for (uint32_t i = from; i < ext->length; i++)
	{
		if (avm2_strict_eq(vec_iter_get(ctx, v, i), search))
		{
			return avm2_uint_value(i);
		}
	}
	return avm2_integer(-1);
}

static Avm2Value vec_last_index_of(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_integer(-1);
	Avm2VectorExt* ext = avm2_vector_ext(v);
	Avm2Value search = arg_or_undef(act, 0);
	double fromf = act->argc > 1 ? avm2_coerce_to_number(ctx, act->args[1])
	                             : 2147483647.0;
	uint32_t from;
	if (fromf < 0.0)
	{
		int64_t w = (int64_t) ext->length + (int64_t) fromf;
		from = w < 0 ? 0 : (uint32_t) w;
	}
	else
	{
		from = fromf >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t) fromf;
	}
	// ArrayIter::with_bounds(0, from): rev_index = min(from+1, length).
	uint64_t rev = (uint64_t) from + 1;
	if (rev > ext->length) rev = ext->length;
	for (int64_t i = (int64_t) rev - 1; i >= 0; i--)
	{
		if (avm2_strict_eq(vec_iter_get(ctx, v, (uint32_t) i), search))
		{
			return avm2_uint_value((uint32_t) i);
		}
	}
	return avm2_integer(-1);
}

// callback arg: null/undefined → not-a-callback (caller decides), callable
// object → itself, anything else → 1034 (the AS3 signatures declare
// callback:Function, so coercion fails there).
static int vec_callback_arg(Avm2Context* ctx, Avm2Value v, Avm2Value* out)
{
	if (v.kind == AVM2_VALUE_NULL || v.kind == AVM2_VALUE_UNDEFINED) return 0;
	if (v.kind == AVM2_VALUE_OBJECT && v.u.obj->kind == AVM2_OBJ_FUNCTION)
	{
		*out = v;
		return 1;
	}
	char dbg[128];
	value_debug_str(ctx, v, dbg, sizeof(dbg));
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1034: Type Coercion failed: cannot convert %s to "
	                 "Function.", dbg);
}

// The AS3 signatures declare `callback:Function` with no default, so avmplus
// rejects a zero-arg call on arity before it ever looks at the callback --
// as3/Vector/{some,map,filter,foreach} assert #1063 for `v.some()`. `every`
// has no corpus assertion but takes the same guard.
static void vec_check_cb_argc(Avm2Activation* act, const char* name)
{
	if (act->argc >= 1) return;
	avm2_throw_error(act->ctx, act->ctx->builtins.argument_error_class,
	                 "Error #1063: Argument count mismatch on Vector/%s(). "
	                 "Expected 1, got 0.", name);
}

static Avm2Value vec_call_cb(Avm2Activation* act, Avm2Value cb, Avm2Value recv,
                             Avm2Value item, uint32_t index)
{
	Avm2Value args[3];
	args[0] = item;
	args[1] = avm2_uint_value(index);
	args[2] = act->this_val;
	return avm2_call_value(act->ctx, cb, recv, args, 3);
}

static Avm2Value vec_every(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	vec_check_cb_argc(act, "every");
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_bool(true);
	Avm2Value cb;
	if (!vec_callback_arg(ctx, arg_or_undef(act, 0), &cb)) return avm2_bool(true);
	Avm2Value recv = arg_or_undef(act, 1);
	uint32_t bound = avm2_vector_ext(v)->length;
	for (uint32_t i = 0; i < bound; i++)
	{
		Avm2Value item = vec_iter_get(ctx, v, i);
		if (!avm2_coerce_to_boolean(vec_call_cb(act, cb, recv, item, i)))
		{
			return avm2_bool(false);
		}
	}
	return avm2_bool(true);
}

static Avm2Value vec_some(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	vec_check_cb_argc(act, "some");
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_bool(false);
	Avm2Value cb;
	if (!vec_callback_arg(ctx, arg_or_undef(act, 0), &cb)) return avm2_bool(false);
	Avm2Value recv = arg_or_undef(act, 1);
	uint32_t bound = avm2_vector_ext(v)->length;
	for (uint32_t i = 0; i < bound; i++)
	{
		Avm2Value item = vec_iter_get(ctx, v, i);
		if (avm2_coerce_to_boolean(vec_call_cb(act, cb, recv, item, i)))
		{
			return avm2_bool(true);
		}
	}
	return avm2_bool(false);
}

static Avm2Value vec_for_each(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	vec_check_cb_argc(act, "forEach");
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2Value cb;
	if (!vec_callback_arg(ctx, arg_or_undef(act, 0), &cb)) return avm2_undefined();
	Avm2Value recv = arg_or_undef(act, 1);
	uint32_t bound = avm2_vector_ext(v)->length;
	for (uint32_t i = 0; i < bound; i++)
	{
		vec_call_cb(act, cb, recv, vec_iter_get(ctx, v, i), i);
	}
	return avm2_undefined();
}

static Avm2Value vec_filter(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	vec_check_cb_argc(act, "filter");
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2Object* out = avm2_vector_new(ctx, v->cls, 0, 0);
	Avm2VectorExt* oext = avm2_vector_ext(out);
	Avm2Value cb;
	if (!vec_callback_arg(ctx, arg_or_undef(act, 0), &cb))
	{
		return avm2_object_value(out);
	}
	Avm2Value recv = arg_or_undef(act, 1);
	uint32_t bound = avm2_vector_ext(v)->length;
	for (uint32_t i = 0; i < bound; i++)
	{
		Avm2Value item = vec_iter_get(ctx, v, i);
		if (avm2_coerce_to_boolean(vec_call_cb(act, cb, recv, item, i)))
		{
			vec_reserve(ctx, oext, oext->length + 1);
			oext->elems[oext->length++] = item;
		}
	}
	return avm2_object_value(out);
}

static Avm2Value vec_map(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	vec_check_cb_argc(act, "map");
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2Object* out = avm2_vector_new(ctx, v->cls, 0, 0);
	Avm2VectorExt* oext = avm2_vector_ext(out);
	Avm2Value cb;
	if (!vec_callback_arg(ctx, arg_or_undef(act, 0), &cb))
	{
		// map(null) returns an empty vector resized to the receiver's length.
		vec_resize(ctx, oext, avm2_vector_ext(v)->length);
		return avm2_object_value(out);
	}
	Avm2Value recv = arg_or_undef(act, 1);
	uint32_t bound = avm2_vector_ext(v)->length;
	for (uint32_t i = 0; i < bound; i++)
	{
		Avm2Value item = vec_iter_get(ctx, v, i);
		Avm2Value nv = vec_call_cb(act, cb, recv, item, i);
		Avm2Value cv = vec_coerce(ctx, oext, nv);
		vec_reserve(ctx, oext, oext->length + 1);
		oext->elems[oext->length++] = cv;
	}
	return avm2_object_value(out);
}

// ---------------------------------------------------------------------------
// sort (Ruffle globals/vector.rs sort — always the slow numeric compare, no
// SWF-version quirks; UNIQUESORT discards on violation, RETURNINDEXEDARRAY
// discards the result entirely)
// ---------------------------------------------------------------------------

typedef struct VecSortCtx
{
	Avm2Activation* act;
	Avm2Value compare_fn;  // undefined = option-based compare
	uint32_t options;
	int unique_satisfied;
} VecSortCtx;

static int vec_sort_cmp(void* ud, const Avm2SortItem* pa, const Avm2SortItem* pb)
{
	VecSortCtx* sc = ud;
	Avm2Context* ctx = sc->act->ctx;
	int cmp;
	if (sc->compare_fn.kind != AVM2_VALUE_UNDEFINED)
	{
		Avm2Value args[2] = { pa->v, pb->v };
		Avm2Value r = avm2_call_value(ctx, sc->compare_fn, sc->act->this_val, args, 2);
		double order = avm2_coerce_to_number(ctx, r);
		cmp = order > 0.0 ? 1 : (order < 0.0 ? -1 : 0);
	}
	else if (sc->options & 16)  // NUMERIC
	{
		double x = avm2_coerce_to_number(ctx, pa->v);
		double y = avm2_coerce_to_number(ctx, pb->v);
		if (isnan(x) && isnan(y)) cmp = 0;
		else if (isnan(x)) cmp = 1;
		else if (isnan(y)) cmp = -1;
		else cmp = x < y ? -1 : (x > y ? 1 : 0);
	}
	else
	{
		const Avm2String* x = avm2_coerce_to_string(ctx, pa->v);
		const Avm2String* y = avm2_coerce_to_string(ctx, pb->v);
		uint32_t n = x->len < y->len ? x->len : y->len;
		cmp = 0;
		if (sc->options & 1)  // CASEINSENSITIVE
		{
			for (uint32_t i = 0; i < n && cmp == 0; i++)
			{
				char cx = x->utf8[i];
				char cy = y->utf8[i];
				if (cx >= 'A' && cx <= 'Z') cx += 32;
				if (cy >= 'A' && cy <= 'Z') cy += 32;
				if (cx != cy) cmp = cx < cy ? -1 : 1;
			}
		}
		else
		{
			int c = memcmp(x->utf8, y->utf8, n);
			if (c != 0) cmp = c < 0 ? -1 : 1;
		}
		if (cmp == 0)
		{
			cmp = x->len < y->len ? -1 : (x->len > y->len ? 1 : 0);
		}
	}
	if (cmp == 0)
	{
		sc->unique_satisfied = 0;
		return 0;
	}
	return (sc->options & 2) ? -cmp : cmp;  // DESCENDING
}

static Avm2Value vec_sort(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* v = this_vector(act);
	if (v == NULL) return avm2_undefined();
	Avm2VectorExt* ext = avm2_vector_ext(v);

	VecSortCtx sc;
	memset(&sc, 0, sizeof(sc));
	sc.act = act;
	sc.compare_fn = avm2_undefined();
	sc.unique_satisfied = 1;
	Avm2Value a0 = arg_or_undef(act, 0);
	if (a0.kind == AVM2_VALUE_OBJECT
	    && (a0.u.obj->kind == AVM2_OBJ_FUNCTION || a0.u.obj->kind == AVM2_OBJ_CLASS))
	{
		sc.compare_fn = a0;
	}
	else
	{
		sc.options = avm2_coerce_to_u32(ctx, a0) & 0xFF;
	}

	uint32_t len = ext->length;
	Avm2SortItem* items = avm2_alloc(ctx, (len + 1) * sizeof(Avm2SortItem));
	for (uint32_t i = 0; i < len; i++)
	{
		items[i].idx = i;
		items[i].v = ext->elems[i];
	}
	avm2_avmplus_qsort(&sc, vec_sort_cmp, items, len);

	// RETURNINDEXEDARRAY sorts but discards the result (Ruffle note).
	if (!(sc.options & 8) && (!(sc.options & 4) || sc.unique_satisfied))
	{
		for (uint32_t i = 0; i < len && i < ext->length; i++)
		{
			ext->elems[i] = items[i].v;
		}
	}
	heap_free(ctx->app, items);
	return act->this_val;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static void vec_register_methods(Avm2Context* ctx, Avm2Class* cls)
{
	{
		Avm2PropEntry e;
		memset(&e, 0, sizeof(e));
		e.key = avm2_public_key("length", 6);
		e.kind = AVM2_PROP_GETSET;
		e.method.fn = vec_get_length;
		e.method.debug_name = "get length";
		e.setter.fn = vec_set_length;
		e.setter.debug_name = "set length";
		e.defining_class = cls;
		avm2_vtable_append(ctx, &cls->ivtable, &e);
	}
	{
		Avm2PropEntry e;
		memset(&e, 0, sizeof(e));
		e.key = avm2_public_key("fixed", 5);
		e.kind = AVM2_PROP_GETSET;
		e.method.fn = vec_get_fixed;
		e.method.debug_name = "get fixed";
		e.setter.fn = vec_set_fixed;
		e.setter.debug_name = "set fixed";
		e.defining_class = cls;
		avm2_vtable_append(ctx, &cls->ivtable, &e);
	}
	avm2_builtin_add_method(ctx, cls, "join", vec_join);
	avm2_builtin_add_method(ctx, cls, "toString", vec_to_string);
	avm2_builtin_add_method(ctx, cls, "toLocaleString", vec_to_locale_string);
	avm2_builtin_add_method(ctx, cls, "push", vec_push);
	avm2_builtin_add_method(ctx, cls, "pop", vec_pop);
	avm2_builtin_add_method(ctx, cls, "shift", vec_shift);
	avm2_builtin_add_method(ctx, cls, "unshift", vec_unshift);
	avm2_builtin_add_method(ctx, cls, "insertAt", vec_insert_at);
	avm2_builtin_add_method(ctx, cls, "removeAt", vec_remove_at);
	avm2_builtin_add_method(ctx, cls, "reverse", vec_reverse);
	avm2_builtin_add_method(ctx, cls, "concat", vec_concat);
	avm2_builtin_add_method(ctx, cls, "slice", vec_slice);
	avm2_builtin_add_method(ctx, cls, "splice", vec_splice);
	avm2_builtin_add_method(ctx, cls, "indexOf", vec_index_of);
	avm2_builtin_add_method(ctx, cls, "lastIndexOf", vec_last_index_of);
	avm2_builtin_add_method(ctx, cls, "every", vec_every);
	avm2_builtin_add_method(ctx, cls, "some", vec_some);
	avm2_builtin_add_method(ctx, cls, "forEach", vec_for_each);
	avm2_builtin_add_method(ctx, cls, "filter", vec_filter);
	avm2_builtin_add_method(ctx, cls, "map", vec_map);
	avm2_builtin_add_method(ctx, cls, "sort", vec_sort);

	Avm2Object* proto = cls->prototype_obj;
	avm2_proto_add_function(ctx, proto, "join", vec_join);
	avm2_proto_add_function(ctx, proto, "toString", vec_to_string);
	avm2_proto_add_function(ctx, proto, "toLocaleString", vec_to_locale_string);
	avm2_proto_add_function(ctx, proto, "push", vec_push);
	avm2_proto_add_function(ctx, proto, "pop", vec_pop);
	avm2_proto_add_function(ctx, proto, "shift", vec_shift);
	avm2_proto_add_function(ctx, proto, "unshift", vec_unshift);
	avm2_proto_add_function(ctx, proto, "reverse", vec_reverse);
	avm2_proto_add_function(ctx, proto, "concat", vec_concat);
	avm2_proto_add_function(ctx, proto, "slice", vec_slice);
	avm2_proto_add_function(ctx, proto, "splice", vec_splice);
	avm2_proto_add_function(ctx, proto, "indexOf", vec_index_of);
	avm2_proto_add_function(ctx, proto, "lastIndexOf", vec_last_index_of);
	avm2_proto_add_function(ctx, proto, "every", vec_every);
	avm2_proto_add_function(ctx, proto, "some", vec_some);
	avm2_proto_add_function(ctx, proto, "forEach", vec_for_each);
	avm2_proto_add_function(ctx, proto, "filter", vec_filter);
	avm2_proto_add_function(ctx, proto, "map", vec_map);
	avm2_proto_add_function(ctx, proto, "sort", vec_sort);
}

void avm2_register_vector(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;

	Avm2Class* generic = avm2_builtin_class(ctx, "__AS3__.vec", "Vector",
	                                        b->object_class);
	generic->is_generic_vector = 1;
	generic->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	generic->native_construct = vector_generic_construct;
	b->vector_class = generic;

	// The four builtin specializations extend Object (playerglobal shape);
	// runtime applications extend Vector.<*> (Ruffle with_type_param).
	struct { const char* name; Avm2Class* param; Avm2Class** slot; } specs[4] = {
		{ "Vector.<*>", NULL, &b->vector_object_class },
		{ "Vector.<int>", b->int_class, &b->vector_int_class },
		{ "Vector.<uint>", b->uint_class, &b->vector_uint_class },
		{ "Vector.<Number>", b->number_class, &b->vector_double_class },
	};
	for (int i = 0; i < 4; i++)
	{
		Avm2Class* cls = avm2_builtin_class(ctx, "__AS3__.vec", specs[i].name,
		                                    b->object_class);
		vec_stamp_class(cls, specs[i].param);
		vec_register_methods(ctx, cls);
		vec_app_add(ctx, specs[i].param, cls);
		*specs[i].slot = cls;
	}

	// Legacy package-internal aliases (avmplus VectorClass.h internal
	// names; vector_legacy reaches them via PackageInternalNs).
	struct { const char* name; Avm2Class* cls; } aliases[4] = {
		{ "Vector$object", b->vector_object_class },
		{ "Vector$int", b->vector_int_class },
		{ "Vector$uint", b->vector_uint_class },
		{ "Vector$double", b->vector_double_class },
	};
	for (int i = 0; i < 4; i++)
	{
		Avm2PropKey key;
		key.name = aliases[i].name;
		key.name_len = (uint32_t) strlen(aliases[i].name);
		key.ns_kind = 0x17;  // PackageInternalNs
		key.ns_priv = NULL;
		key.ns_uri = "__AS3__.vec";
		key.ns_len = 11;
		avm2_builtin_define_alias(ctx, key,
		                          avm2_object_value(aliases[i].cls->class_object));
	}
}
