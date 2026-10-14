// Namespace + QName builtins — port of Ruffle
// core/src/avm2/object/{namespace,qname}_object.rs,
// globals/{namespace,q_name}.rs and globals/{Namespace,QName}.as.
//
// A Namespace VALUE (PushNamespace, `namespace n = "uri"` trait defaults,
// user `new Namespace(...)`) is an Avm2Object of namespace_class whose
// native_ext is Avm2NamespaceExt {uri, prefix, kind}. prefix == NULL means
// the undefined prefix. kind is the raw ABC namespace kind (folded only at
// match time); user-created namespaces are Package (0x16), the "any"
// namespace is kind 0 with an empty URI.
//
// A QName is an Avm2Object of qname_class whose native_ext is Avm2QNameExt
// {uri, local}: uri == NULL means "any namespace" (uri getter → null),
// local == NULL means the any name (localName getter → "*").

#include <stdio.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_e4x.h>
#include <memory/heap.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>

Avm2NamespaceExt* avm2_namespace_ext_of(Avm2Value v)
{
	if (v.kind == AVM2_VALUE_OBJECT && v.u.obj != NULL
	    && v.u.obj->cls == avm2_get_context()->builtins.namespace_class)
	{
		return (Avm2NamespaceExt*) v.u.obj->native_ext;
	}
	return NULL;
}

Avm2QNameExt* avm2_qname_ext_of(Avm2Value v)
{
	if (v.kind == AVM2_VALUE_OBJECT && v.u.obj != NULL
	    && v.u.obj->cls == avm2_get_context()->builtins.qname_class)
	{
		return (Avm2QNameExt*) v.u.obj->native_ext;
	}
	return NULL;
}

static Avm2Object* alloc_instance(Avm2Context* ctx, Avm2Class* cls, uint32_t ext_size)
{
	Avm2Object* obj = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 1);
	obj->cls = cls;
	obj->vtable = &cls->ivtable;
	obj->proto = cls->prototype_obj;
	obj->native_ext = avm2_alloc(ctx, ext_size);
	memset(obj->native_ext, 0, ext_size);
	obj->native_ext_size = ext_size;  // GC conservative-scan span
	return obj;
}

Avm2Object* avm2_namespace_new(Avm2Context* ctx, const Avm2String* uri,
                               const Avm2String* prefix, uint8_t kind)
{
	Avm2Object* obj = alloc_instance(ctx, ctx->builtins.namespace_class,
	                                 sizeof(Avm2NamespaceExt));
	Avm2NamespaceExt* ext = (Avm2NamespaceExt*) obj->native_ext;
	ext->uri = uri;
	ext->prefix = prefix;
	ext->kind = kind;
	return obj;
}

// PushNamespace / namespace-kind trait defaults: box a constant-pool
// namespace (Ruffle NamespaceObject::from_namespace — prefix "" iff the
// URI is empty).
Avm2Object* avm2_namespace_from_pool(Avm2Context* ctx, Avm2AbcFileRt* file, uint32_t ns_idx)
{
	const Avm2AbcNamespace* ns = &file->data->namespaces[ns_idx];
	const Avm2String* uri = &file->data->strings[ns->name];
	const Avm2String* prefix = (uri->len == 0) ? avm2_string_from_literal(ctx, "") : NULL;
	return avm2_namespace_new(ctx, uri, prefix, ns->kind);
}

Avm2Object* avm2_qname_new(Avm2Context* ctx, const Avm2String* uri,
                           const Avm2String* local)
{
	Avm2Object* obj = alloc_instance(ctx, ctx->builtins.qname_class,
	                                 sizeof(Avm2QNameExt));
	Avm2QNameExt* ext = (Avm2QNameExt*) obj->native_ext;
	ext->uri = uri;
	ext->local = local;
	return obj;
}

// QName.toString (Ruffle Multiname::as_uri): "*::local" for the any
// namespace, bare "local" for the public (empty-URI) namespace, else
// "uri::local"; the any name prints as "*".
const Avm2String* avm2_qname_to_string(Avm2Context* ctx, const Avm2QNameExt* ext)
{
	const char* local = (ext->local != NULL) ? ext->local->utf8 : "*";
	uint32_t local_len = (ext->local != NULL) ? ext->local->len : 1;
	const char* ns;
	uint32_t ns_len;
	if (ext->uri == NULL)
	{
		ns = "*";
		ns_len = 1;
	}
	else
	{
		ns = ext->uri->utf8;
		ns_len = ext->uri->len;
	}
	if (ns_len == 0)
	{
		return avm2_string_new(ctx, local, local_len);
	}
	char* buf = avm2_alloc(ctx, ns_len + 2 + local_len + 1);
	memcpy(buf, ns, ns_len);
	memcpy(buf + ns_len, "::", 2);
	memcpy(buf + ns_len + 2, local, local_len);
	buf[ns_len + 2 + local_len] = '\0';
	const Avm2String* s = avm2_string_new(ctx, buf, ns_len + 2 + local_len);
	heap_free(ctx->app, buf);  // avm2_string_new copied it
	return s;
}

// ---------------------------------------------------------------------------
// Namespace constructor (Ruffle globals/namespace.rs namespace_constructor)
// ---------------------------------------------------------------------------

static Avm2Value namespace_construct(Avm2Context* ctx, Avm2Class* cls,
                                     const Avm2Value* args, uint32_t argc)
{
	(void) cls;
	const Avm2String* empty = avm2_string_from_literal(ctx, "");
	const Avm2String* uri = empty;
	const Avm2String* prefix = NULL;
	uint8_t kind = 0x16;

	if (argc == 0)
	{
		prefix = empty;
	}
	else if (argc == 1)
	{
		Avm2QNameExt* q = avm2_qname_ext_of(args[0]);
		Avm2NamespaceExt* n = avm2_namespace_ext_of(args[0]);
		if (q != NULL)
		{
			if (q->uri != NULL)
			{
				uri = q->uri;
				prefix = (uri->len > 0) ? NULL : empty;
			}
			else
			{
				// Any-namespace QName → the any namespace.
				uri = empty;
				kind = 0;
				prefix = empty;
			}
		}
		else if (n != NULL)
		{
			uri = n->uri;
			prefix = n->prefix;
			kind = n->kind;
		}
		else
		{
			uri = avm2_coerce_to_string(ctx, args[0]);
			prefix = (uri->len == 0) ? empty : NULL;
		}
	}
	else
	{
		Avm2QNameExt* q = avm2_qname_ext_of(args[1]);
		if (q != NULL)
		{
			uri = (q->uri != NULL) ? q->uri : empty;
		}
		else
		{
			uri = avm2_coerce_to_string(ctx, args[1]);
		}
		// Coerce the prefix BEFORE the checks (side-effect order matches
		// Flash — Ruffle namespace.rs).
		const Avm2String* prefix_str = avm2_coerce_to_string(ctx, args[0]);
		int prefix_none = (args[0].kind == AVM2_VALUE_UNDEFINED
		                   || args[0].kind == AVM2_VALUE_NULL);
		prefix = prefix_none ? NULL : prefix_str;
		if (uri->len == 0 && !(prefix != NULL && prefix->len == 0))
		{
			avm2_throw_error(ctx, ctx->builtins.type_error_class,
			                 "Error #1098: Illegal prefix %.*s for no namespace.",
			                 (int) prefix_str->len, prefix_str->utf8);
		}
		if (prefix_str->len > 0 && !avm2_e4x_is_xml_name(prefix_str))
		{
			prefix = NULL;
		}
	}
	return avm2_object_value(avm2_namespace_new(ctx, uri, prefix, kind));
}

static Avm2NamespaceExt* this_namespace(Avm2Activation* act)
{
	Avm2NamespaceExt* ext = avm2_namespace_ext_of(act->this_val);
	if (ext == NULL)
	{
		avm2_throw_error(act->ctx, act->ctx->builtins.type_error_class,
		                 "Error #1004: Method Namespace was invoked on an "
		                 "incompatible object.");
	}
	return ext;
}

static Avm2Value namespace_get_prefix(Avm2Activation* act)
{
	Avm2NamespaceExt* ext = this_namespace(act);
	if (ext->prefix == NULL) return avm2_undefined();
	return avm2_string(ext->prefix);
}

static Avm2Value namespace_get_uri(Avm2Activation* act)
{
	return avm2_string(this_namespace(act)->uri);
}

static Avm2Value namespace_to_string(Avm2Activation* act)
{
	return avm2_string(this_namespace(act)->uri);
}

// ---------------------------------------------------------------------------
// QName constructor + call handler (Ruffle globals/q_name.rs)
// ---------------------------------------------------------------------------

static Avm2Value qname_construct(Avm2Context* ctx, Avm2Class* cls,
                                 const Avm2Value* args, uint32_t argc)
{
	(void) cls;
	const Avm2String* empty = avm2_string_from_literal(ctx, "");
	const Avm2String* uri = NULL;    // NULL = any namespace
	const Avm2String* local = NULL;  // NULL = any name

	if (argc >= 2)
	{
		Avm2Value ns_arg = args[0];
		Avm2Value local_arg = args[1];
		Avm2NamespaceExt* n = avm2_namespace_ext_of(ns_arg);
		Avm2QNameExt* nq = avm2_qname_ext_of(ns_arg);
		if (n != NULL)
		{
			uri = n->uri;
		}
		else if (nq != NULL)
		{
			uri = nq->uri;  // NULL propagates the any namespace
		}
		else if (ns_arg.kind == AVM2_VALUE_NULL)
		{
			uri = NULL;
		}
		else if (ns_arg.kind == AVM2_VALUE_UNDEFINED)
		{
			uri = empty;
		}
		else
		{
			uri = avm2_coerce_to_string(ctx, ns_arg);
		}

		Avm2QNameExt* lq = avm2_qname_ext_of(local_arg);
		if (lq != NULL)
		{
			local = (lq->local != NULL) ? lq->local
			                            : avm2_string_from_literal(ctx, "*");
		}
		else if (local_arg.kind == AVM2_VALUE_UNDEFINED)
		{
			local = empty;
		}
		else
		{
			local = avm2_coerce_to_string(ctx, local_arg);
		}
	}
	else
	{
		Avm2Value arg0 = (argc >= 1) ? args[0] : avm2_undefined();
		Avm2QNameExt* q = avm2_qname_ext_of(arg0);
		if (q != NULL)
		{
			return avm2_object_value(avm2_qname_new(ctx, q->uri, q->local));
		}
		const Avm2String* l = (arg0.kind == AVM2_VALUE_UNDEFINED)
		                          ? empty
		                          : avm2_coerce_to_string(ctx, arg0);
		if (!(l->len == 1 && l->utf8[0] == '*'))
		{
			// ECMA-357 13.3.2 step 4b: the one-argument form takes its
			// namespace from the DEFAULT XML NAMESPACE in effect at the call,
			// not from the public namespace. With no `default xml namespace`
			// statement the dxns is the unnamed namespace, so this still
			// yields "" in the overwhelmingly common case.
			const Avm2String* d = avm2_dxns_uri(ctx);
			uri = (d != NULL) ? d : empty;
			local = l;
		}
		// else: any namespace, any name
	}
	return avm2_object_value(avm2_qname_new(ctx, uri, local));
}

// QName(...) class call (Ruffle CallHandler): a lone QName argument passes
// through unchanged; anything else constructs.
static Avm2Value qname_call(Avm2Context* ctx, Avm2Class* cls,
                            const Avm2Value* args, uint32_t argc)
{
	if (argc == 1 && avm2_qname_ext_of(args[0]) != NULL)
	{
		return args[0];
	}
	return qname_construct(ctx, cls, args, argc);
}

static Avm2QNameExt* this_qname(Avm2Activation* act)
{
	Avm2QNameExt* ext = avm2_qname_ext_of(act->this_val);
	if (ext == NULL)
	{
		avm2_throw_error(act->ctx, act->ctx->builtins.type_error_class,
		                 "Error #1004: Method QName was invoked on an "
		                 "incompatible object.");
	}
	return ext;
}

static Avm2Value qname_get_local_name(Avm2Activation* act)
{
	Avm2QNameExt* ext = this_qname(act);
	if (ext->local == NULL)
	{
		return avm2_string(avm2_string_from_literal(act->ctx, "*"));
	}
	return avm2_string(ext->local);
}

static Avm2Value qname_get_uri(Avm2Activation* act)
{
	Avm2QNameExt* ext = this_qname(act);
	if (ext->uri == NULL) return avm2_null();
	return avm2_string(ext->uri);
}

static Avm2Value qname_to_string(Avm2Activation* act)
{
	return avm2_string(avm2_qname_to_string(act->ctx, this_qname(act)));
}

static Avm2Value qname_value_of(Avm2Activation* act)
{
	return act->this_val;
}

// ---------------------------------------------------------------------------
// Enumeration hooks (avm2_object.c): fixed two-enumerant layouts with the
// avmplus name/value order quirks.
// ---------------------------------------------------------------------------

int avm2_nsqname_next_enumerant(Avm2Object* obj, uint32_t cur, uint32_t* out)
{
	Avm2Context* ctx = avm2_get_context();
	if (obj->cls != ctx->builtins.namespace_class
	    && obj->cls != ctx->builtins.qname_class)
	{
		return 0;
	}
	*out = (cur < 2) ? cur + 1 : 0;
	return 1;
}

int avm2_nsqname_enumerant_name(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                                Avm2Value* out)
{
	if (obj->cls == ctx->builtins.namespace_class)
	{
		if (idx == 1) *out = avm2_string(avm2_string_from_literal(ctx, "uri"));
		else if (idx == 2) *out = avm2_string(avm2_string_from_literal(ctx, "prefix"));
		else *out = avm2_null();
		return 1;
	}
	if (obj->cls == ctx->builtins.qname_class)
	{
		// avmplus quirk: names run uri, localName — values run localName, uri.
		if (idx == 1) *out = avm2_string(avm2_string_from_literal(ctx, "uri"));
		else if (idx == 2) *out = avm2_string(avm2_string_from_literal(ctx, "localName"));
		else *out = avm2_null();
		return 1;
	}
	return 0;
}

int avm2_nsqname_enumerant_value(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                                 Avm2Value* out)
{
	if (obj->cls == ctx->builtins.namespace_class)
	{
		Avm2NamespaceExt* ext = (Avm2NamespaceExt*) obj->native_ext;
		if (idx == 1) *out = avm2_string(ext->uri);
		else if (idx == 2)
		{
			*out = (ext->prefix != NULL) ? avm2_string(ext->prefix)
			                             : avm2_undefined();
		}
		else *out = avm2_undefined();
		return 1;
	}
	if (obj->cls == ctx->builtins.qname_class)
	{
		Avm2QNameExt* ext = (Avm2QNameExt*) obj->native_ext;
		if (idx == 1)
		{
			*out = (ext->local != NULL)
			           ? avm2_string(ext->local)
			           : avm2_string(avm2_string_from_literal(ctx, "*"));
		}
		else if (idx == 2)
		{
			*out = (ext->uri != NULL) ? avm2_string(ext->uri)
			                          : avm2_string(avm2_string_from_literal(ctx, ""));
		}
		else *out = avm2_undefined();
		return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void avm2_register_nsqname(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;

	Avm2Class* ns = avm2_builtin_class(ctx, "", "Namespace", b->object_class);
	b->namespace_class = ns;
	ns->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	ns->native_construct = namespace_construct;
	ns->native_call = namespace_construct;  // [Ruffle(ConstructOnCall)]
	avm2_builtin_add_getter(ctx, ns, "prefix", namespace_get_prefix);
	avm2_builtin_add_getter(ctx, ns, "uri", namespace_get_uri);
	avm2_builtin_add_method(ctx, ns, "toString", namespace_to_string);
	avm2_builtin_add_method(ctx, ns, "valueOf", namespace_to_string);
	avm2_builtin_add_static_const(ctx, ns, "length", avm2_integer(2));
	avm2_proto_add_function(ctx, ns->prototype_obj, "toString", namespace_to_string);
	avm2_proto_add_function(ctx, ns->prototype_obj, "valueOf", namespace_to_string);

	Avm2Class* qn = avm2_builtin_class(ctx, "", "QName", b->object_class);
	b->qname_class = qn;
	qn->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	qn->native_construct = qname_construct;
	qn->native_call = qname_call;  // [Ruffle(CallHandler)]
	avm2_builtin_add_getter(ctx, qn, "localName", qname_get_local_name);
	avm2_builtin_add_getter(ctx, qn, "uri", qname_get_uri);
	avm2_builtin_add_method(ctx, qn, "toString", qname_to_string);
	avm2_builtin_add_method(ctx, qn, "valueOf", qname_value_of);
	avm2_builtin_add_static_const(ctx, qn, "length", avm2_integer(2));
	avm2_proto_add_function(ctx, qn->prototype_obj, "toString", qname_to_string);
}
