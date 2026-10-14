// flash.utils.Proxy — port of Ruffle core/src/avm2/object/proxy_object.rs +
// globals/flash/utils/Proxy.as.
//
// A Proxy-derived receiver replaces the entire "local" half of property
// resolution: when a lookup misses the declared traits, the engine calls
// the flash_proxy-namespace hooks (getProperty/setProperty/deleteProperty/
// callProperty/hasProperty) with a QName built from the multiname, and
// enumeration routes to nextNameIndex/nextName/nextValue. Dynamic props
// and the prototype chain are never consulted on a Proxy — that is what
// makes coerce_to_primitive_side_effects observe callProperty("toString").
//
// The flash_proxy namespace ("http://www.adobe.com/2006/actionscript/
// flash/proxy") must NOT unify with public: hooks are registered under
// package-kind keys with that URI, so avm2_propkey_is_public stays false
// and public lookups miss them (proxy_getproperty's p.proxy_var probe).

#include <stdio.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>

static const char PROXY_NS[] = "http://www.adobe.com/2006/actionscript/flash/proxy";

int avm2_is_proxy(Avm2Object* obj)
{
	Avm2Context* ctx = avm2_get_context();
	if (ctx->builtins.proxy_class == NULL) return 0;
	for (Avm2Class* c = obj->cls; c != NULL; c = c->super_class)
	{
		if (c == ctx->builtins.proxy_class) return 1;
	}
	return 0;
}

Avm2Value avm2_proxy_call_hook(Avm2Context* ctx, Avm2Object* obj, const char* mname,
                               const Avm2Value* args, uint32_t argc)
{
	Avm2PropKey key;
	key.name = mname;
	key.name_len = (uint32_t) strlen(mname);
	key.ns_kind = 0x16;
	key.ns_priv = NULL;
	key.ns_uri = PROXY_NS;
	key.ns_len = sizeof(PROXY_NS) - 1;
	const Avm2PropEntry* e = avm2_vtable_find(obj->vtable, &key);
	if (e == NULL || (e->kind != AVM2_PROP_METHOD))
	{
		avm2_throw_error(ctx, ctx->builtins.reference_error_class,
		                 "Error #1069: Property %s not found on %.*s and there "
		                 "is no default value.", mname,
		                 (int) obj->cls->name.name_len, obj->cls->name.name);
	}
	return avm2_call_method_ref(ctx, &e->method, e->defining_class,
	                            e->method_scope, avm2_object_value(obj),
	                            args, argc);
}

// ---------------------------------------------------------------------------
// Enumeration hooks (avm2_object.c protocol; call user overrides)
// ---------------------------------------------------------------------------

int avm2_proxy_next_enumerant(Avm2Object* obj, uint32_t cur, uint32_t* out)
{
	Avm2Context* ctx = avm2_get_context();
	if (!avm2_is_proxy(obj)) return 0;
	Avm2Value arg = avm2_integer((int32_t) cur);
	Avm2Value v = avm2_proxy_call_hook(ctx, obj, "nextNameIndex", &arg, 1);
	*out = avm2_coerce_to_u32(ctx, v);
	return 1;
}

int avm2_proxy_enumerant_name(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                              Avm2Value* out)
{
	if (!avm2_is_proxy(obj)) return 0;
	Avm2Value arg = avm2_integer((int32_t) idx);
	*out = avm2_proxy_call_hook(ctx, obj, "nextName", &arg, 1);
	return 1;
}

int avm2_proxy_enumerant_value(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                               Avm2Value* out)
{
	if (!avm2_is_proxy(obj)) return 0;
	Avm2Value arg = avm2_integer((int32_t) idx);
	*out = avm2_proxy_call_hook(ctx, obj, "nextValue", &arg, 1);
	return 1;
}

// ---------------------------------------------------------------------------
// Default (un-overridden) hook bodies: IllegalOperationError 2088-family
// ---------------------------------------------------------------------------

static _Noreturn void proxy_unimplemented(Avm2Context* ctx, int code, const char* what)
{
	// Proxy.as is AS3 playerglobal, so FP's trace carries the throwError frame.
	avm2_callstack_push_throwerror(ctx);
	avm2_throw_error(ctx, ctx->builtins.illegal_operation_error_class,
	                 "Error #%d: The Proxy class does not implement %s. "
	                 "It must be overridden by a subclass.", code, what);
}

static Avm2Value proxy_default_get_property(Avm2Activation* act)
{
	proxy_unimplemented(act->ctx, 2088, "getProperty");
}
static Avm2Value proxy_default_set_property(Avm2Activation* act)
{
	proxy_unimplemented(act->ctx, 2089, "setProperty");
}
static Avm2Value proxy_default_call_property(Avm2Activation* act)
{
	proxy_unimplemented(act->ctx, 2090, "callProperty");
}
static Avm2Value proxy_default_has_property(Avm2Activation* act)
{
	proxy_unimplemented(act->ctx, 2091, "hasProperty");
}
static Avm2Value proxy_default_delete_property(Avm2Activation* act)
{
	proxy_unimplemented(act->ctx, 2092, "deleteProperty");
}
static Avm2Value proxy_default_get_descendants(Avm2Activation* act)
{
	proxy_unimplemented(act->ctx, 2093, "getDescendants");
}
static Avm2Value proxy_default_next_name_index(Avm2Activation* act)
{
	proxy_unimplemented(act->ctx, 2105, "nextNameIndex");
}
static Avm2Value proxy_default_next_name(Avm2Activation* act)
{
	proxy_unimplemented(act->ctx, 2106, "nextName");
}
static Avm2Value proxy_default_next_value(Avm2Activation* act)
{
	proxy_unimplemented(act->ctx, 2107, "nextValue");
}

static Avm2Value proxy_is_attribute(Avm2Activation* act)
{
	// Honors the QName ATTRIBUTE flag (set by E4X name() on attribute
	// nodes and by attribute multinames routed through the proxy hooks).
	Avm2QNameExt* q = (act->argc >= 1) ? avm2_qname_ext_of(act->args[0]) : NULL;
	return avm2_bool(q != NULL && q->is_attribute != 0);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static void proxy_add_ns_method(Avm2Context* ctx, Avm2Class* cls, const char* name,
                                Avm2MethodFn fn)
{
	Avm2PropEntry e;
	memset(&e, 0, sizeof(e));
	e.key.name = name;
	e.key.name_len = (uint32_t) strlen(name);
	e.key.ns_kind = 0x16;
	e.key.ns_uri = PROXY_NS;
	e.key.ns_len = sizeof(PROXY_NS) - 1;
	e.kind = AVM2_PROP_METHOD;
	e.method.fn = fn;
	// FP spells a non-public builtin method's frame with its namespace URI
	// ("flash.utils::Proxy/http://…/flash/proxy::getProperty()"), and a
	// native debug_name is printed verbatim by avm2_callstack_frame_name.
	{
		size_t nlen = strlen(name);
		char* qual = avm2_alloc(ctx, sizeof(PROXY_NS) + 2 + nlen);
		snprintf(qual, sizeof(PROXY_NS) + 2 + nlen, "%s::%s", PROXY_NS, name);
		e.method.debug_name = qual;
	}
	e.defining_class = cls;
	avm2_vtable_append(ctx, &cls->ivtable, &e);
}

void avm2_register_proxy(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;
	Avm2Class* cls = avm2_builtin_class(ctx, "flash.utils", "Proxy", b->object_class);
	b->proxy_class = cls;
	cls->flags |= AVM2_CLASS_FLAG_SEALED;

	proxy_add_ns_method(ctx, cls, "getProperty", proxy_default_get_property);
	proxy_add_ns_method(ctx, cls, "setProperty", proxy_default_set_property);
	proxy_add_ns_method(ctx, cls, "callProperty", proxy_default_call_property);
	proxy_add_ns_method(ctx, cls, "hasProperty", proxy_default_has_property);
	proxy_add_ns_method(ctx, cls, "deleteProperty", proxy_default_delete_property);
	proxy_add_ns_method(ctx, cls, "getDescendants", proxy_default_get_descendants);
	proxy_add_ns_method(ctx, cls, "nextNameIndex", proxy_default_next_name_index);
	proxy_add_ns_method(ctx, cls, "nextName", proxy_default_next_name);
	proxy_add_ns_method(ctx, cls, "nextValue", proxy_default_next_value);
	proxy_add_ns_method(ctx, cls, "isAttribute", proxy_is_attribute);

	// The flash.utils.flash_proxy namespace definition itself.
	{
		Avm2PropKey key;
		key.name = "flash_proxy";
		key.name_len = 11;
		key.ns_kind = 0x16;
		key.ns_priv = NULL;
		key.ns_uri = "flash.utils";
		key.ns_len = 11;
		Avm2Object* ns = avm2_namespace_new(
			ctx, avm2_string_from_literal(ctx, PROXY_NS), NULL, 0x16);
		avm2_builtin_define_alias(ctx, key, avm2_object_value(ns));
	}
}
