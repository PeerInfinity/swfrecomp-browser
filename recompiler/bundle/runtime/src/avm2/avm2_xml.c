// XML / XMLList classes — the E4X method surface, class-call conversions,
// settings statics, and the property-engine entry points wired into
// avm2_ops.c. Port of Ruffle globals/xml.rs, globals/xml_list.rs,
// globals/XML.as, globals/XMLList.as, object/xml_object.rs and
// object/xml_list_object.rs (see avm2_e4x.h / avm2_e4x.c for the tree).

#include <stdio.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_e4x.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_gc.h>
#include <memory/heap.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>

static Avm2XmlSettings g_xml_settings = { 1, 1, 1, 1, 2 };

Avm2XmlSettings* avm2_xml_settings(void)
{
	return &g_xml_settings;
}

const Avm2String* avm2_dxns_uri(Avm2Context* ctx)
{
	return ctx->dxns;
}

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------

Avm2XmlExt* avm2_xml_ext_of(Avm2Value v)
{
	if (v.kind == AVM2_VALUE_OBJECT && v.u.obj != NULL
	    && v.u.obj->cls == avm2_get_context()->builtins.xml_class)
	{
		return (Avm2XmlExt*) v.u.obj->native_ext;
	}
	return NULL;
}

Avm2XmlListExt* avm2_xmllist_ext_of(Avm2Value v)
{
	if (v.kind == AVM2_VALUE_OBJECT && v.u.obj != NULL
	    && v.u.obj->cls == avm2_get_context()->builtins.xml_list_class)
	{
		return (Avm2XmlListExt*) v.u.obj->native_ext;
	}
	return NULL;
}

int avm2_value_is_xmlish(Avm2Value v)
{
	return avm2_xml_ext_of(v) != NULL || avm2_xmllist_ext_of(v) != NULL;
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
	// GC: not pinned. A wrapper keeps its node tree alive and a live node keeps
	// its cached wrapper (node->obj) alive — avm2_xml_gc_trace_ext and
	// avm2_e4x_gc_mark_node — so the node->obj cache can never dangle. (It was
	// pinned while E4X nodes were immortal; that retained every parsed
	// document: Seedling's per-swap room XML, ~2.7 MB a swap.)
	return obj;
}

// GC ext tracer: an XML wrapper reaches its node's whole tree; an XMLList
// reaches each item's tree, its target object and its target name strings.
void avm2_xml_gc_trace_ext(Avm2Object* o)
{
	Avm2Context* ctx = avm2_get_context();
	if (o->cls == ctx->builtins.xml_class)
	{
		Avm2XmlExt* xe = (Avm2XmlExt*) o->native_ext;
		avm2_e4x_gc_mark_node(xe->node);
	}
	else if (o->cls == ctx->builtins.xml_list_class)
	{
		Avm2XmlListExt* le = (Avm2XmlListExt*) o->native_ext;
		for (uint32_t i = 0; i < le->count; i++) avm2_e4x_gc_mark_node(le->items[i]);
		avm2_gc_mark_object(le->target_object);
		if (le->has_target_prop)
		{
			avm2_gc_mark_string(le->target_prop.local);
			avm2_gc_mark_string(le->target_prop.single_uri);
		}
	}
}

// GC free hook: the XMLList item array is a separate allocation the list owns.
void avm2_xml_gc_free_ext(Avm2Context* ctx, Avm2Object* o)
{
	if (o->cls != ctx->builtins.xml_list_class) return;
	Avm2XmlListExt* le = (Avm2XmlListExt*) o->native_ext;
	if (le->items != NULL) heap_free(ctx->app, le->items);
	le->items = NULL;
}

Avm2Object* avm2_xml_object_for_node(Avm2Context* ctx, E4XNode* node)
{
	if (node->obj != NULL) return node->obj;
	Avm2Object* obj = alloc_instance(ctx, ctx->builtins.xml_class, sizeof(Avm2XmlExt));
	((Avm2XmlExt*) obj->native_ext)->node = node;
	node->obj = obj;
	return obj;
}

Avm2Object* avm2_xmllist_new(Avm2Context* ctx, Avm2Object* target_object,
                             const E4XName* target_prop)
{
	Avm2Object* obj = alloc_instance(ctx, ctx->builtins.xml_list_class,
	                                 sizeof(Avm2XmlListExt));
	Avm2XmlListExt* le = (Avm2XmlListExt*) obj->native_ext;
	le->target_object = target_object;
	if (target_prop != NULL)
	{
		le->has_target_prop = 1;
		le->target_prop = *target_prop;
	}
	return obj;
}

void avm2_xmllist_push(Avm2Context* ctx, Avm2XmlListExt* list, E4XNode* node)
{
	if (list->count == list->cap)
	{
		uint32_t ncap = list->cap ? list->cap * 2 : 4;
		E4XNode** ni = avm2_alloc(ctx, ncap * sizeof(E4XNode*));
		memcpy(ni, list->items, list->count * sizeof(E4XNode*));
		if (list->items != NULL) heap_free(ctx->app, list->items);  // list-owned
		list->items = ni;
		list->cap = ncap;
	}
	list->items[list->count++] = node;
}

static void list_remove_at(Avm2XmlListExt* list, uint32_t idx)
{
	if (idx >= list->count) return;
	memmove(&list->items[idx], &list->items[idx + 1],
	        (list->count - idx - 1) * sizeof(E4XNode*));
	list->count--;
}

// append (ECMA-357 9.2.1.6): appending a list copies its target linkage
// and clears the dirty flag; appending a single node sets it.
static void list_append_list(Avm2XmlListExt* dst, Avm2Context* ctx,
                             Avm2Object* src_obj)
{
	Avm2XmlListExt* src = (Avm2XmlListExt*) src_obj->native_ext;
	dst->target_dirty = 0;
	dst->target_object = src->target_object;
	dst->has_target_prop = src->has_target_prop;
	dst->target_prop = src->target_prop;
	for (uint32_t i = 0; i < src->count; i++)
	{
		avm2_xmllist_push(ctx, dst, src->items[i]);
	}
}

static void list_append_node(Avm2XmlListExt* dst, Avm2Context* ctx, E4XNode* node)
{
	dst->target_dirty = 1;
	avm2_xmllist_push(ctx, dst, node);
}

// ---------------------------------------------------------------------------
// this-extraction helpers for native methods
// ---------------------------------------------------------------------------

static _Noreturn void throw_1004(Avm2Context* ctx, const char* cls)
{
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1004: Method %s was invoked on an incompatible object.",
	                 cls);
}

static E4XNode* this_xml_node(Avm2Activation* act)
{
	Avm2XmlExt* xe = avm2_xml_ext_of(act->this_val);
	if (xe == NULL) throw_1004(act->ctx, "XML");
	return xe->node;
}

static Avm2XmlListExt* this_list(Avm2Activation* act)
{
	Avm2XmlListExt* le = avm2_xmllist_ext_of(act->this_val);
	if (le == NULL) throw_1004(act->ctx, "XMLList");
	return le;
}

static Avm2Value arg_or_undef(Avm2Activation* act, uint32_t i)
{
	return (i < act->argc) ? act->args[i] : avm2_undefined();
}

static const Avm2String* lit(Avm2Context* ctx, const char* s)
{
	return avm2_string_from_literal(ctx, s);
}

// name argument → E4XName with default "*" (descendants/elements/PI).
static E4XName name_arg_default_any(Avm2Activation* act, uint32_t i, int force_attr)
{
	if (i >= act->argc)
	{
		return avm2_e4x_name_from_string(act->ctx, lit(act->ctx, "*"), force_attr);
	}
	return avm2_e4x_name_from_value(act->ctx, act->args[i], force_attr);
}

// ---------------------------------------------------------------------------
// Core queries (get_child_list & co)
// ---------------------------------------------------------------------------

// XML get_child_list: filter children (or attributes) by name into a new
// XMLList with target linkage.
static Avm2Object* xml_get_child_list(Avm2Context* ctx, Avm2Object* xml_obj,
                                      E4XNode* node, const E4XName* name)
{
	Avm2Object* out = avm2_xmllist_new(ctx, xml_obj, name);
	Avm2XmlListExt* le = (Avm2XmlListExt*) out->native_ext;
	if (node->kind == E4X_ELEMENT)
	{
		E4XNode** arr = name->is_attribute ? node->attributes : node->children;
		uint32_t n = name->is_attribute ? node->attr_count : node->child_count;
		for (uint32_t i = 0; i < n; i++)
		{
			if (avm2_e4x_matches_name(arr[i], name))
			{
				avm2_xmllist_push(ctx, le, arr[i]);
			}
		}
	}
	if (le->count > 0) le->target_dirty = 1;
	return out;
}

// XMLList name query: union of the query over element items.
static Avm2Object* list_get_child_list(Avm2Context* ctx, Avm2Object* list_obj,
                                       const E4XName* name)
{
	Avm2XmlListExt* list = (Avm2XmlListExt*) list_obj->native_ext;
	Avm2Object* out = avm2_xmllist_new(ctx, list_obj, name);
	Avm2XmlListExt* oe = (Avm2XmlListExt*) out->native_ext;
	for (uint32_t i = 0; i < list->count; i++)
	{
		E4XNode* item = list->items[i];
		if (item->kind != E4X_ELEMENT) continue;
		Avm2Object* gq = xml_get_child_list(ctx, avm2_xml_object_for_node(ctx, item),
		                                    item, name);
		Avm2XmlListExt* ge = (Avm2XmlListExt*) gq->native_ext;
		if (ge->count > 0)
		{
			list_append_list(oe, ctx, gq);
		}
	}
	return out;
}

// Generic local get over an already-built E4XName.
static Avm2Value xmlish_get_local(Avm2Context* ctx, Avm2Value recv, const E4XName* name)
{
	Avm2XmlExt* xe = avm2_xml_ext_of(recv);
	if (xe != NULL)
	{
		return avm2_object_value(
			xml_get_child_list(ctx, recv.u.obj, xe->node, name));
	}
	return avm2_object_value(list_get_child_list(ctx, recv.u.obj, name));
}

// ---------------------------------------------------------------------------
// resolve_value / reevaluate_target (list append machinery)
// ---------------------------------------------------------------------------

static void list_reevaluate_target(Avm2Context* ctx, Avm2XmlListExt* list)
{
	if (!list->target_dirty || list->count == 0) return;
	E4XNode* last = list->items[list->count - 1];
	if (last->parent != NULL)
	{
		Avm2XmlExt* cur = (list->target_object != NULL)
			? avm2_xml_ext_of(avm2_object_value(list->target_object)) : NULL;
		if (cur == NULL || cur->node != last->parent)
		{
			list->target_object = avm2_xml_object_for_node(ctx, last->parent);
		}
	}
	else
	{
		list->target_object = NULL;
	}
	if (last->kind != E4X_PI && last->local != NULL)
	{
		E4XName n;
		memset(&n, 0, sizeof(n));
		n.local = last->local;
		n.single_uri = last->has_ns ? last->ns.uri : lit(ctx, "");
		n.single_is_real = 1;
		list->target_prop = n;
		list->has_target_prop = 1;
	}
	list->target_dirty = 0;
}

static void xml_set_local(Avm2Context* ctx, Avm2Object* xml_obj, E4XNode* node,
                          const E4XName* name, Avm2Value value);

// [[ResolveValue]] — returns the resolved XMLList/XML object or NULL.
static Avm2Object* list_resolve_value(Avm2Context* ctx, Avm2Object* list_obj)
{
	Avm2XmlListExt* list = (Avm2XmlListExt*) list_obj->native_ext;
	if (list->count > 0) return list_obj;
	list_reevaluate_target(ctx, list);
	if (list->target_object == NULL || !list->has_target_prop) return NULL;
	if (list->target_prop.is_attribute || list->target_prop.local == NULL) return NULL;

	Avm2Object* base = list->target_object;
	if (avm2_xmllist_ext_of(avm2_object_value(base)) != NULL)
	{
		base = list_resolve_value(ctx, base);
		if (base == NULL) return NULL;
	}
	Avm2Value target = xmlish_get_local(ctx, avm2_object_value(base),
	                                    &list->target_prop);
	Avm2XmlListExt* te = avm2_xmllist_ext_of(target);
	if (te == NULL) return NULL;
	if (te->count == 0)
	{
		Avm2XmlListExt* be = avm2_xmllist_ext_of(avm2_object_value(base));
		if (be != NULL && be->count > 1)
		{
			avm2_throw_error(ctx, ctx->builtins.type_error_class,
			                 "Error #1089: Assignment to lists with more than "
			                 "one item is not supported.");
		}
		// Create the empty property, then re-query.
		Avm2Value empty = avm2_string(lit(ctx, ""));
		Avm2XmlExt* bxe = avm2_xml_ext_of(avm2_object_value(base));
		if (bxe != NULL)
		{
			xml_set_local(ctx, base, bxe->node, &list->target_prop, empty);
		}
		else if (be != NULL && be->count == 1)
		{
			xml_set_local(ctx, avm2_xml_object_for_node(ctx, be->items[0]),
			              be->items[0], &list->target_prop, empty);
		}
		else
		{
			return NULL;
		}
		target = xmlish_get_local(ctx, avm2_object_value(base), &list->target_prop);
		te = avm2_xmllist_ext_of(target);
		if (te == NULL) return NULL;
	}
	return target.u.obj;
}

// ---------------------------------------------------------------------------
// XML [[Put]] (ECMA-357 9.1.1.2, Ruffle xml_object.rs set_property_local)
// ---------------------------------------------------------------------------

// namespace_for_multiname: the name's explicit ns, else dxns, else none.
static int name_namespace(Avm2Context* ctx, const E4XName* name, E4XNamespace* out)
{
	if (name->single_uri != NULL && name->single_uri->len > 0)
	{
		out->uri = name->single_uri;
		out->prefix = NULL;
		return 1;
	}
	const Avm2String* dxns = avm2_dxns_uri(ctx);
	if (dxns != NULL && dxns->len > 0)
	{
		out->uri = dxns;
		out->prefix = NULL;
		return 1;
	}
	return 0;
}

// Joins an XMLList's item strings with spaces (attribute-value coercion).
static const Avm2String* list_join_space(Avm2Context* ctx, Avm2XmlListExt* le)
{
	if (le->count == 0) return lit(ctx, "");
	const Avm2String* s = avm2_e4x_to_string(ctx, le->items[0]);
	for (uint32_t i = 1; i < le->count; i++)
	{
		s = avm2_string_concat(ctx, s, lit(ctx, " "));
		s = avm2_string_concat(ctx, s, avm2_e4x_to_string(ctx, le->items[i]));
	}
	return s;
}

static int name_parses_index(const E4XName* name, uint32_t* out)
{
	if (name->local == NULL || name->is_attribute) return 0;
	const Avm2String* s = name->local;
	if (s->len == 0 || s->len > 10) return 0;
	uint64_t v = 0;
	for (uint32_t i = 0; i < s->len; i++)
	{
		if (s->utf8[i] < '0' || s->utf8[i] > '9') return 0;
		v = v * 10 + (uint64_t) (s->utf8[i] - '0');
	}
	if (s->len > 1 && s->utf8[0] == '0') return 0;
	if (v >= 0xFFFFFFFFull) return 0;
	*out = (uint32_t) v;
	return 1;
}

static void xml_set_local(Avm2Context* ctx, Avm2Object* xml_obj, E4XNode* node,
                          const E4XName* name, Avm2Value value)
{
	(void) xml_obj;
	uint32_t idx;
	if (name_parses_index(name, &idx))
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1087: Assignment to indexed XML is not allowed.");
	}
	if (node->kind != E4X_ELEMENT) return;

	// Coerce the value (step 3).
	Avm2Value c = value;
	int c_is_string = 0;
	const Avm2String* c_str = NULL;
	{
		Avm2XmlExt* vx = avm2_xml_ext_of(value);
		Avm2XmlListExt* vl = avm2_xmllist_ext_of(value);
		if (vx != NULL)
		{
			if (vx->node->kind == E4X_ATTRIBUTE || vx->node->kind == E4X_TEXT)
			{
				c_is_string = 1;
				c_str = avm2_e4x_to_string(ctx, vx->node);
			}
			else
			{
				c = avm2_object_value(
					avm2_xml_object_for_node(ctx, avm2_e4x_deep_copy(ctx, vx->node)));
			}
		}
		else if (vl != NULL)
		{
			if (vl->count == 1 && (vl->items[0]->kind == E4X_ATTRIBUTE
			                       || vl->items[0]->kind == E4X_TEXT))
			{
				c_is_string = 1;
				c_str = avm2_e4x_to_string(ctx, vl->items[0]);
			}
			else
			{
				Avm2Object* copy = avm2_xmllist_new(ctx, NULL, NULL);
				Avm2XmlListExt* ce = (Avm2XmlListExt*) copy->native_ext;
				for (uint32_t i = 0; i < vl->count; i++)
				{
					avm2_xmllist_push(ctx, ce, avm2_e4x_deep_copy(ctx, vl->items[i]));
				}
				c = avm2_object_value(copy);
			}
		}
		else
		{
			c_is_string = 1;
			c_str = avm2_coerce_to_string(ctx, value);
		}
	}

	if (name->is_attribute)
	{
		// XMLList values join with spaces.
		Avm2XmlListExt* vl = avm2_xmllist_ext_of(c);
		const Avm2String* s = c_is_string ? c_str
		                     : (vl != NULL) ? list_join_space(ctx, vl)
		                     : avm2_coerce_to_string(ctx, c);
		uint32_t first_idx;
		E4XNode* first;
		if (avm2_e4x_remove_matching(ctx, node, name, 1, &first_idx, &first))
		{
			const Avm2String* old_value = first->text;
			first->text = s;
			first->parent = node;
			// Re-insert at the original index.
			avm2_e4x_append_attribute(ctx, node, first);
			for (uint32_t i = node->attr_count - 1; i > first_idx; i--)
			{
				E4XNode* t = node->attributes[i];
				node->attributes[i] = node->attributes[i - 1];
				node->attributes[i - 1] = t;
			}
			if (first->local != NULL)
			{
				avm2_xml_notify(ctx, node, "attributeChanged",
				                avm2_string(first->local), avm2_string(old_value));
			}
		}
		else if (name->local != NULL)
		{
			E4XNamespace ns;
			E4XNamespace* nsp = name_namespace(ctx, name, &ns) ? &ns : NULL;
			E4XNode* attr = avm2_e4x_attribute(ctx, nsp, name->local, s, node);
			avm2_e4x_append_attribute(ctx, node, attr);
			avm2_xml_notify(ctx, node, "attributeAdded",
			                avm2_string(name->local), avm2_string(s));
		}
		// An any-name (`x.@* = v`) can only REWRITE: ECMA-357 9.1.1.2 keeps the
		// first matching attribute, deletes the rest, and assigns to it — but
		// it never CREATES one, since there is no name to create. The read
		// half of the wildcard rule shipped in session 5; this is the write.
		return;
	}

	// Child assignment.
	int any_name = (name->local == NULL);
	if (!any_name && !avm2_e4x_is_xml_name(name->local))
	{
		return;
	}
	int primitive_assign = c_is_string && !any_name;

	uint32_t index;
	E4XNode* first;
	if (avm2_e4x_remove_matching(ctx, node, name, 0, &index, &first))
	{
		avm2_e4x_insert_at(ctx, node, index, first);
	}
	else
	{
		index = node->child_count;
		E4XNode* dummy = avm2_e4x_node_new(ctx, E4X_ELEMENT, NULL);
		avm2_e4x_insert_at(ctx, node, index, dummy);
		if (primitive_assign)
		{
			E4XNamespace ns;
			E4XNamespace* nsp = name_namespace(ctx, name, &ns) ? &ns : NULL;
			E4XNode* elem = avm2_e4x_element(ctx, nsp, name->local, node);
			elem->parent = node;
			node->children[index] = elem;
		}
	}

	if (primitive_assign)
	{
		E4XNode* slot = node->children[index];
		avm2_e4x_remove_all_children(slot);
		if (c_str->len > 0)
		{
			avm2_e4x_replace(ctx, slot, 0, avm2_string(c_str));
		}
	}
	else
	{
		Avm2Value cv = c_is_string ? avm2_string(c_str) : c;
		avm2_e4x_replace(ctx, node, index, cv);
	}
}

// ---------------------------------------------------------------------------
// XMLList [[Put]] (ECMA-357 9.2.1.2)
// ---------------------------------------------------------------------------

static void list_set_index(Avm2Context* ctx, Avm2Object* list_obj, uint32_t index,
                           Avm2Value value)
{
	Avm2XmlListExt* list = (Avm2XmlListExt*) list_obj->native_ext;
	list_reevaluate_target(ctx, list);

	Avm2Object* r = NULL;
	if (list->target_object != NULL)
	{
		if (avm2_xmllist_ext_of(avm2_object_value(list->target_object)) != NULL)
		{
			r = list_resolve_value(ctx, list->target_object);
		}
		else
		{
			r = list->target_object;
		}
		if (r == NULL) return;
	}

	if (index >= list->count)
	{
		// Appending: reduce r to a single element node.
		E4XNode* r_node = NULL;
		if (r != NULL)
		{
			Avm2XmlExt* rx = avm2_xml_ext_of(avm2_object_value(r));
			Avm2XmlListExt* rl = avm2_xmllist_ext_of(avm2_object_value(r));
			if (rx != NULL)
			{
				r_node = rx->node;
			}
			else if (rl != NULL)
			{
				if (rl->count != 1) return;
				r_node = rl->items[0];
			}
		}
		if (r_node != NULL && r_node->kind != E4X_ELEMENT) return;

		E4XNode* y;
		Avm2XmlExt* vx = avm2_xml_ext_of(value);
		if (list->has_target_prop && list->target_prop.is_attribute)
		{
			if (r_node != NULL)
			{
				Avm2Value existing = xmlish_get_local(
					ctx, avm2_object_value(avm2_xml_object_for_node(ctx, r_node)),
					&list->target_prop);
				Avm2XmlListExt* ee = avm2_xmllist_ext_of(existing);
				if (ee != NULL && ee->count > 0) return;
			}
			y = avm2_e4x_attribute(ctx, NULL, list->target_prop.local,
			                       lit(ctx, ""), r_node);
		}
		else if (!list->has_target_prop || list->target_prop.local == NULL
		         || (vx != NULL && (vx->node->kind == E4X_TEXT
		                            || vx->node->kind == E4X_ATTRIBUTE)))
		{
			y = avm2_e4x_text(ctx, lit(ctx, ""), r_node);
		}
		else
		{
			E4XNamespace ns;
			E4XNamespace* nsp =
				name_namespace(ctx, &list->target_prop, &ns) ? &ns : NULL;
			y = avm2_e4x_element(ctx, nsp, list->target_prop.local, r_node);
		}

		index = list->count;
		if (y->kind != E4X_ATTRIBUTE && r_node != NULL)
		{
			uint32_t j = r_node->child_count;
			if (list->count > 0)
			{
				int ci = avm2_e4x_child_index(list->items[list->count - 1]);
				if (ci >= 0) j = (uint32_t) ci + 1;
			}
			avm2_e4x_insert_at(ctx, r_node, j, y);
			if (vx != NULL && vx->node->local != NULL)
			{
				y->local = vx->node->local;
				y->has_ns = vx->node->has_ns;
				y->ns = vx->node->ns;
			}
			else
			{
				Avm2XmlListExt* vl = avm2_xmllist_ext_of(value);
				if (vl != NULL && vl->has_target_prop
				    && vl->target_prop.local != NULL)
				{
					y->local = vl->target_prop.local;
					if (vl->target_prop.single_uri != NULL
					    && vl->target_prop.single_uri->len > 0)
					{
						y->has_ns = 1;
						y->ns.uri = vl->target_prop.single_uri;
						y->ns.prefix = NULL;
					}
				}
			}
		}
		avm2_xmllist_push(ctx, list, y);
	}

	// Coerce V like XML put step 3 (leaf lists / leaf XML → string).
	{
		Avm2XmlListExt* vl = avm2_xmllist_ext_of(value);
		Avm2XmlExt* vx = avm2_xml_ext_of(value);
		if (vl != NULL && vl->count == 1
		    && (vl->items[0]->kind == E4X_ATTRIBUTE || vl->items[0]->kind == E4X_TEXT))
		{
			value = avm2_string(avm2_e4x_to_string(ctx, vl->items[0]));
		}
		else if (vx != NULL && (vx->node->kind == E4X_ATTRIBUTE
		                        || vx->node->kind == E4X_TEXT))
		{
			value = avm2_string(avm2_e4x_to_string(ctx, vx->node));
		}
	}

	E4XNode* child = list->items[index];
	Avm2XmlListExt* vl = avm2_xmllist_ext_of(value);
	Avm2XmlExt* vx = avm2_xml_ext_of(value);

	if (child->kind == E4X_ATTRIBUTE)
	{
		E4XNode* parent = child->parent;
		if (parent != NULL)
		{
			E4XName an;
			memset(&an, 0, sizeof(an));
			an.local = child->local;
			an.is_attribute = 1;
			an.single_uri = child->has_ns ? child->ns.uri : lit(ctx, "");
			an.single_is_real = 1;
			xml_set_local(ctx, avm2_xml_object_for_node(ctx, parent), parent,
			              &an, value);
		}
		return;
	}
	if (vl != NULL)
	{
		E4XNode* parent = child->parent;
		if (parent != NULL)
		{
			int q = avm2_e4x_child_index(child);
			if (q >= 0)
			{
				avm2_e4x_replace(ctx, parent, (uint32_t) q, value);
			}
		}
		// Splice the list contents in place of items[index].
		list_remove_at(list, index);
		for (uint32_t j = 0; j < vl->count; j++)
		{
			avm2_xmllist_push(ctx, list, NULL);
		}
		memmove(&list->items[index + vl->count], &list->items[index],
		        (list->count - vl->count - index) * sizeof(E4XNode*));
		for (uint32_t j = 0; j < vl->count; j++)
		{
			list->items[index + j] = vl->items[j];
		}
		return;
	}
	if (vx != NULL || child->kind != E4X_ELEMENT)
	{
		E4XNode* parent = child->parent;
		if (parent != NULL)
		{
			int q = avm2_e4x_child_index(child);
			if (q >= 0)
			{
				avm2_e4x_replace(ctx, parent, (uint32_t) q, value);
				list->items[index] = parent->children[q];
				return;
			}
		}
		if (vx != NULL)
		{
			list->items[index] = vx->node;
		}
		else
		{
			list->items[index] =
				avm2_e4x_text(ctx, avm2_coerce_to_string(ctx, value), NULL);
		}
		return;
	}
	// Plain element child, primitive value: child.*("*") = value.
	{
		E4XName star;
		memset(&star, 0, sizeof(star));
		star.any_ns = 1;
		xml_set_local(ctx, avm2_xml_object_for_node(ctx, child), child, &star, value);
	}
}

static void list_set_local(Avm2Context* ctx, Avm2Object* list_obj,
                           const E4XName* name, Avm2Value value)
{
	Avm2XmlListExt* list = (Avm2XmlListExt*) list_obj->native_ext;
	uint32_t idx;
	if (name_parses_index(name, &idx) && !name->is_attribute && name->local != NULL)
	{
		list_set_index(ctx, list_obj, idx, value);
		return;
	}
	if (list->count <= 1)
	{
		if (list->count == 0)
		{
			Avm2Object* r = list_resolve_value(ctx, list_obj);
			if (r == NULL) return;
			Avm2XmlListExt* re = avm2_xmllist_ext_of(avm2_object_value(r));
			if (re != NULL)
			{
				if (re->count != 1) return;
				list_append_node(list, ctx, re->items[0]);
				list->target_dirty = 0;
			}
			else
			{
				Avm2XmlExt* rx = avm2_xml_ext_of(avm2_object_value(r));
				if (rx == NULL) return;
				avm2_xmllist_push(ctx, list, rx->node);
			}
		}
		E4XNode* item = list->items[0];
		xml_set_local(ctx, avm2_xml_object_for_node(ctx, item), item, name, value);
		return;
	}
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1089: Assignment to lists with more than one item "
	                 "is not supported.");
}

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------

static void xml_delete_local(Avm2Context* ctx, E4XNode* node, const E4XName* name)
{
	uint32_t idx;
	if (name_parses_index(name, &idx)) return;  // no-op, returns true
	if (node->kind != E4X_ELEMENT) return;
	if (name->is_attribute)
	{
		uint32_t fi;
		E4XNode* fn;
		avm2_e4x_remove_matching(ctx, node, name, 1, &fi, &fn);
		return;
	}
	int any_name = (name->local == NULL && !name->is_qname);
	uint32_t w = 0;
	for (uint32_t i = 0; i < node->child_count; i++)
	{
		E4XNode* c = node->children[i];
		int drop = any_name
		           || (c->kind == E4X_ELEMENT && avm2_e4x_matches_name(c, name));
		if (drop)
		{
			c->parent = NULL;
		}
		else
		{
			node->children[w++] = c;
		}
	}
	node->child_count = w;
}

// ---------------------------------------------------------------------------
// has_property (in / hasOwnProperty)
// ---------------------------------------------------------------------------

static int xml_node_has_property(const E4XNode* node, const E4XName* name)
{
	if (node->kind != E4X_ELEMENT) return 0;
	const E4XNode* const* arr =
		name->is_attribute ? (const E4XNode* const*) node->attributes
		                   : (const E4XNode* const*) node->children;
	uint32_t n = name->is_attribute ? node->attr_count : node->child_count;
	for (uint32_t i = 0; i < n; i++)
	{
		if (avm2_e4x_matches_name(arr[i], name)) return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Property-engine entry points (avm2_ops.c)
// ---------------------------------------------------------------------------

// Should a static-multiname access fall through to the generic resolve?
// Only when a vtable trait matches AND the multiname has no public
// namespace (the avmplus Toplevel.cpp quirk: public method-trait hits
// reroute to E4X property access; explicit AS3::name binds the method).
static int fall_through_to_trait(Avm2Context* ctx, Avm2Value recv,
                                 const Avm2AbcFileData* data, uint32_t mn_idx)
{
	if (avm2_mn_has_public_ns(data, mn_idx)) return 0;
	const Avm2VTable* vt = avm2_value_vtable(ctx, recv);
	return avm2_vtable_find_mn(vt, data, mn_idx) != NULL;
}

int avm2_xml_get_mn(Avm2Context* ctx, Avm2Value recv, const Avm2AbcFileData* data,
                    uint32_t mn_idx, Avm2Value* out)
{
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
	// Numeric-index short-circuit, before handle_input.
	int is_attr_kind = (mn->kind == 0x0d || mn->kind == 0x10 || mn->kind == 0x12
	                    || mn->kind == 0x0e || mn->kind == 0x1c);
	if (!is_attr_kind && mn->name != 0)
	{
		E4XName probe;
		memset(&probe, 0, sizeof(probe));
		probe.local = &data->strings[mn->name];
		uint32_t idx;
		if (name_parses_index(&probe, &idx))
		{
			Avm2XmlExt* xe = avm2_xml_ext_of(recv);
			if (xe != NULL)
			{
				*out = (idx == 0) ? recv : avm2_undefined();
			}
			else
			{
				Avm2XmlListExt* le = avm2_xmllist_ext_of(recv);
				*out = (idx < le->count)
					? avm2_object_value(avm2_xml_object_for_node(ctx, le->items[idx]))
					: avm2_undefined();
			}
			return 1;
		}
	}
	if (fall_through_to_trait(ctx, recv, data, mn_idx)) return 0;
	E4XName name = avm2_e4x_name_from_mn(ctx, data, mn_idx);
	*out = xmlish_get_local(ctx, recv, &name);
	return 1;
}

int avm2_xml_get_name(Avm2Context* ctx, Avm2Value recv, const E4XName* name,
                      Avm2Value* out)
{
	uint32_t idx;
	if (name_parses_index(name, &idx))
	{
		Avm2XmlExt* xe = avm2_xml_ext_of(recv);
		if (xe != NULL)
		{
			*out = (idx == 0) ? recv : avm2_undefined();
		}
		else
		{
			Avm2XmlListExt* le = avm2_xmllist_ext_of(recv);
			*out = (idx < le->count)
				? avm2_object_value(avm2_xml_object_for_node(ctx, le->items[idx]))
				: avm2_undefined();
		}
		return 1;
	}
	*out = xmlish_get_local(ctx, recv, name);
	return 1;
}

int avm2_xml_set_mn(Avm2Context* ctx, Avm2Value recv, const Avm2AbcFileData* data,
                    uint32_t mn_idx, Avm2Value value)
{
	if (fall_through_to_trait(ctx, recv, data, mn_idx)) return 0;
	E4XName name = avm2_e4x_name_from_mn(ctx, data, mn_idx);
	return avm2_xml_set_name(ctx, recv, &name, value);
}

int avm2_xml_set_name(Avm2Context* ctx, Avm2Value recv, const E4XName* name,
                      Avm2Value value)
{
	Avm2XmlExt* xe = avm2_xml_ext_of(recv);
	if (xe != NULL)
	{
		xml_set_local(ctx, recv.u.obj, xe->node, name, value);
	}
	else
	{
		list_set_local(ctx, recv.u.obj, name, value);
	}
	return 1;
}

int avm2_xml_delete_mn(Avm2Context* ctx, Avm2Value recv, const Avm2AbcFileData* data,
                       uint32_t mn_idx, Avm2Value* out)
{
	if (fall_through_to_trait(ctx, recv, data, mn_idx)) return 0;
	E4XName name = avm2_e4x_name_from_mn(ctx, data, mn_idx);
	return avm2_xml_delete_name(ctx, recv, &name, out);
}

int avm2_xml_delete_name(Avm2Context* ctx, Avm2Value recv, const E4XName* name,
                         Avm2Value* out)
{
	Avm2XmlExt* xe = avm2_xml_ext_of(recv);
	if (xe != NULL)
	{
		xml_delete_local(ctx, xe->node, name);
		*out = avm2_bool(true);
		return 1;
	}
	Avm2XmlListExt* le = avm2_xmllist_ext_of(recv);
	uint32_t idx;
	if (name_parses_index(name, &idx) && !name->is_attribute)
	{
		if (idx < le->count)
		{
			E4XNode* node = le->items[idx];
			list_remove_at(le, idx);
			if (node->parent != NULL)
			{
				if (node->kind == E4X_ATTRIBUTE)
				{
					avm2_e4x_remove_attribute(node->parent, node);
				}
				else
				{
					avm2_e4x_remove_child(node->parent, node);
				}
				// `delete list[i]` DETACHES: the removed node's parent()
				// must read undefined and its childIndex() -1 (the name
				// paths already null it via avm2_e4x_remove_matching /
				// avm2_e4x_delete_by_index) — avm2/xml_list_delete_clear_parent.
				node->parent = NULL;
			}
		}
		*out = avm2_bool(true);
		return 1;
	}
	for (uint32_t i = 0; i < le->count; i++)
	{
		if (le->items[i]->kind == E4X_ELEMENT)
		{
			xml_delete_local(ctx, le->items[i], name);
		}
	}
	*out = avm2_bool(true);
	return 1;
}

Avm2Value avm2_xml_call_fallback(Avm2Context* ctx, Avm2Value recv,
                                 const char* name, uint32_t name_len,
                                 const Avm2Value* args, uint32_t argc)
{
	Avm2XmlExt* xe = avm2_xml_ext_of(recv);
	if (xe != NULL)
	{
		E4XName n = avm2_e4x_name_from_string(
			ctx, avm2_string_new(ctx, name, name_len), 0);
		Avm2Value q = xmlish_get_local(ctx, recv, &n);
		Avm2XmlListExt* qe = avm2_xmllist_ext_of(q);
		if (qe != NULL && qe->count == 0 && avm2_e4x_has_simple_content(xe->node))
		{
			Avm2Value sv = avm2_string(avm2_e4x_to_string(ctx, xe->node));
			return avm2_call_public_property(ctx, sv, name, name_len, args, argc);
		}
		avm2_throw_1006(ctx, name, name_len);
	}
	Avm2XmlListExt* le = avm2_xmllist_ext_of(recv);
	if (le != NULL && le->count == 1)
	{
		Avm2Value item =
			avm2_object_value(avm2_xml_object_for_node(ctx, le->items[0]));
		return avm2_call_public_property(ctx, item, name, name_len, args, argc);
	}
	avm2_throw_1006(ctx, name, name_len);
}

// Own dynamic props + prototype-chain dynamic props (the tail of the
// base has_property algorithm; prototype functions live as dyn props).
static int dyn_or_proto_has(Avm2Value recv, const Avm2String* name)
{
	if (recv.kind != AVM2_VALUE_OBJECT) return 0;
	if (avm2_object_find_dynamic(recv.u.obj, name->utf8, name->len) != NULL)
	{
		return 1;
	}
	for (Avm2Object* p = recv.u.obj->proto; p != NULL; p = p->proto)
	{
		if (avm2_object_find_dynamic(p, name->utf8, name->len) != NULL)
		{
			return 1;
		}
	}
	return 0;
}

int avm2_xml_has_property_via_in(Avm2Context* ctx, Avm2Value recv,
                                 const Avm2String* name)
{
	E4XName n = avm2_e4x_name_from_string(ctx, name, 0);
	uint32_t idx;
	Avm2XmlExt* xe = avm2_xml_ext_of(recv);
	if (xe != NULL)
	{
		if (name_parses_index(&n, &idx)) return idx == 0;
		if (xml_node_has_property(xe->node, &n)) return 1;
		return dyn_or_proto_has(recv, name);
	}
	Avm2XmlListExt* le = avm2_xmllist_ext_of(recv);
	if (name_parses_index(&n, &idx)) return idx < le->count;
	for (uint32_t i = 0; i < le->count; i++)
	{
		if (le->items[i]->kind == E4X_ELEMENT
		    && xml_node_has_property(le->items[i], &n))
		{
			return 1;
		}
	}
	return dyn_or_proto_has(recv, name);
}

int avm2_xml_has_own(Avm2Context* ctx, Avm2Value recv,
                     const char* name, uint32_t name_len)
{
	// hasOwnProperty: E4X property or an OWN dynamic prop — never the
	// prototype chain (unlike the `in` operator).
	const Avm2String* s = avm2_string_new(ctx, name, name_len);
	E4XName n = avm2_e4x_name_from_string(ctx, s, 0);
	uint32_t idx;
	Avm2XmlExt* xe = avm2_xml_ext_of(recv);
	if (xe != NULL)
	{
		if (name_parses_index(&n, &idx)) return idx == 0;
		if (xml_node_has_property(xe->node, &n)) return 1;
	}
	else
	{
		Avm2XmlListExt* le = avm2_xmllist_ext_of(recv);
		if (name_parses_index(&n, &idx)) return idx < le->count;
		for (uint32_t i = 0; i < le->count; i++)
		{
			if (le->items[i]->kind == E4X_ELEMENT
			    && xml_node_has_property(le->items[i], &n))
			{
				return 1;
			}
		}
	}
	return recv.kind == AVM2_VALUE_OBJECT
	       && avm2_object_find_dynamic(recv.u.obj, name, name_len) != NULL;
}

// ---------------------------------------------------------------------------
// Enumeration hooks
// ---------------------------------------------------------------------------

int avm2_xml_next_enumerant(Avm2Object* obj, uint32_t cur, uint32_t* out)
{
	Avm2Context* ctx = avm2_get_context();
	if (obj->cls == ctx->builtins.xml_class)
	{
		*out = (cur == 0) ? 1 : 0;
		return 1;
	}
	if (obj->cls == ctx->builtins.xml_list_class)
	{
		Avm2XmlListExt* le = (Avm2XmlListExt*) obj->native_ext;
		*out = (cur < le->count) ? cur + 1 : 0;
		return 1;
	}
	return 0;
}

int avm2_xml_enumerant_name(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                            Avm2Value* out)
{
	if (obj->cls == ctx->builtins.xml_class)
	{
		*out = (idx == 1) ? avm2_integer(0) : avm2_null();
		return 1;
	}
	if (obj->cls == ctx->builtins.xml_list_class)
	{
		Avm2XmlListExt* le = (Avm2XmlListExt*) obj->native_ext;
		*out = (idx >= 1 && idx <= le->count) ? avm2_integer((int32_t) (idx - 1))
		                                      : avm2_null();
		return 1;
	}
	return 0;
}

int avm2_xml_enumerant_value(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                             Avm2Value* out)
{
	if (obj->cls == ctx->builtins.xml_class)
	{
		*out = (idx == 1) ? avm2_object_value(obj) : avm2_undefined();
		return 1;
	}
	if (obj->cls == ctx->builtins.xml_list_class)
	{
		Avm2XmlListExt* le = (Avm2XmlListExt*) obj->native_ext;
		*out = (idx >= 1 && idx <= le->count)
			? avm2_object_value(avm2_xml_object_for_node(ctx, le->items[idx - 1]))
			: avm2_undefined();
		return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Abstract equality
// ---------------------------------------------------------------------------

static int xml_node_abstract_eq(Avm2Context* ctx, E4XNode* node, Avm2Value b)
{
	Avm2XmlExt* bx = avm2_xml_ext_of(b);
	Avm2XmlListExt* bl = avm2_xmllist_ext_of(b);
	if (bl != NULL)
	{
		// Symmetric: delegate to the list arm.
		int eq = 0;
		avm2_xml_abstract_eq(ctx, b,
			avm2_object_value(avm2_xml_object_for_node(ctx, node)), &eq);
		return eq;
	}
	if (bx != NULL)
	{
		E4XNode* other = bx->node;
		int self_leaf = (node->kind == E4X_TEXT || node->kind == E4X_CDATA
		                 || node->kind == E4X_ATTRIBUTE);
		int other_leaf = (other->kind == E4X_TEXT || other->kind == E4X_CDATA
		                  || other->kind == E4X_ATTRIBUTE);
		if ((self_leaf && avm2_e4x_has_simple_content(other))
		    || (other_leaf && avm2_e4x_has_simple_content(node)))
		{
			return avm2_string_equals(avm2_e4x_to_string(ctx, node),
			                          avm2_e4x_to_string(ctx, other));
		}
		return avm2_e4x_equals(node, other);
	}
	if (avm2_e4x_has_simple_content(node))
	{
		const Avm2String* s = avm2_e4x_to_string(ctx, node);
		return avm2_string_equals(s, avm2_coerce_to_string(ctx, b));
	}
	return 0;
}

int avm2_xml_abstract_eq(Avm2Context* ctx, Avm2Value a, Avm2Value b, int* eq)
{
	Avm2XmlListExt* al = avm2_xmllist_ext_of(a);
	if (al != NULL)
	{
		if (b.kind == AVM2_VALUE_UNDEFINED && al->count == 0)
		{
			*eq = 1;
			return 1;
		}
		Avm2XmlListExt* bl = avm2_xmllist_ext_of(b);
		if (bl != NULL)
		{
			if (al->count != bl->count)
			{
				*eq = 0;
				return 1;
			}
			for (uint32_t i = 0; i < al->count; i++)
			{
				if (!xml_node_abstract_eq(ctx, al->items[i],
				        avm2_object_value(
				            avm2_xml_object_for_node(ctx, bl->items[i]))))
				{
					*eq = 0;
					return 1;
				}
			}
			*eq = 1;
			return 1;
		}
		if (al->count == 1)
		{
			*eq = xml_node_abstract_eq(ctx, al->items[0], b);
			return 1;
		}
		*eq = 0;
		return 1;
	}
	Avm2XmlExt* ax = avm2_xml_ext_of(a);
	if (ax != NULL)
	{
		*eq = xml_node_abstract_eq(ctx, ax->node, b);
		return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Descendants
// ---------------------------------------------------------------------------

Avm2Value avm2_xml_descendants_value(Avm2Context* ctx, Avm2Value v, const E4XName* name)
{
	Avm2Object* out = avm2_xmllist_new(ctx, NULL, NULL);
	Avm2XmlListExt* oe = (Avm2XmlListExt*) out->native_ext;
	Avm2XmlExt* xe = avm2_xml_ext_of(v);
	Avm2XmlListExt* le = avm2_xmllist_ext_of(v);
	if (xe != NULL)
	{
		avm2_e4x_descendants(ctx, xe->node, name, oe);
	}
	else if (le != NULL)
	{
		for (uint32_t i = 0; i < le->count; i++)
		{
			avm2_e4x_descendants(ctx, le->items[i], name, oe);
		}
	}
	else
	{
		char cn[160];
		avm2_class_qname_buf(avm2_value_class(ctx, v), cn, sizeof(cn));
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1016: Descendants operator (..) not supported "
		                 "on type %s", cn);
	}
	if (oe->count > 0) oe->target_dirty = 1;
	return avm2_object_value(out);
}

// ---------------------------------------------------------------------------
// Constructors + class-call handlers
// ---------------------------------------------------------------------------

// XML.init: parse and pick the single root node (globals/xml.rs init).
static E4XNode* xml_init_node(Avm2Context* ctx, Avm2Value value)
{
	Avm2XmlListExt* vl = avm2_xmllist_ext_of(value);
	if (vl != NULL && vl->count != 1)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1088: The markup in the document following the "
		                 "root element must be well-formed.");
	}
	Avm2XmlSettings* st = avm2_xml_settings();
	uint32_t n = 0;
	E4XNode** nodes = avm2_e4x_parse(ctx, value, st->ignore_comments, st->ignore_pi,
	                                 st->ignore_white, &n);
	if (n == 0)
	{
		avm2_e4x_parse_free(ctx, nodes);
		return avm2_e4x_text(ctx, avm2_string_from_literal(ctx, ""), NULL);
	}
	if (n == 1)
	{
		E4XNode* only = nodes[0];
		avm2_e4x_parse_free(ctx, nodes);
		return only;
	}
	// Multiple top-level nodes: skip cdata/comments/PIs; whitespace-only
	// text allowed; exactly one element.
	E4XNode* element = NULL;
	for (uint32_t i = 0; i < n; i++)
	{
		E4XNode* node = nodes[i];
		if (node->kind == E4X_CDATA || node->kind == E4X_COMMENT
		    || node->kind == E4X_PI)
		{
			continue;
		}
		if (node->kind == E4X_TEXT)
		{
			int all_ws = 1;
			for (uint32_t j = 0; j < node->text->len && all_ws; j++)
			{
				char ch = node->text->utf8[j];
				all_ws = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
			}
			if (all_ws) continue;
			element = NULL;
			break;
		}
		if (node->kind == E4X_ELEMENT && element == NULL)
		{
			element = node;
			continue;
		}
		element = NULL;
		break;
	}
	avm2_e4x_parse_free(ctx, nodes);
	if (element == NULL)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1088: The markup in the document following the "
		                 "root element must be well-formed.");
	}
	return element;
}

static Avm2Value xml_construct(Avm2Context* ctx, Avm2Class* cls,
                               const Avm2Value* args, uint32_t argc)
{
	(void) cls;
	Avm2Value value = (argc >= 1) ? args[0] : avm2_undefined();
	E4XNode* node = xml_init_node(ctx, value);
	// A fresh wrapper unless the node is brand new (parse always creates
	// fresh nodes, so the cache is empty here).
	return avm2_object_value(avm2_xml_object_for_node(ctx, node));
}

// XML(...) call: XML passes through; 1-element XMLList unwraps; else new.
static Avm2Value xml_call(Avm2Context* ctx, Avm2Class* cls,
                          const Avm2Value* args, uint32_t argc)
{
	if (argc == 1)
	{
		if (avm2_xml_ext_of(args[0]) != NULL) return args[0];
		Avm2XmlListExt* le = avm2_xmllist_ext_of(args[0]);
		if (le != NULL)
		{
			if (le->count == 1)
			{
				return avm2_object_value(
					avm2_xml_object_for_node(ctx, le->items[0]));
			}
			avm2_throw_error(ctx, ctx->builtins.type_error_class,
			                 "Error #1088: The markup in the document following "
			                 "the root element must be well-formed.");
		}
	}
	return xml_construct(ctx, cls, args, argc);
}

static Avm2Value xmllist_construct(Avm2Context* ctx, Avm2Class* cls,
                                   const Avm2Value* args, uint32_t argc)
{
	(void) cls;
	Avm2Value value = (argc >= 1) ? args[0] : avm2_undefined();
	Avm2Object* obj = avm2_xmllist_new(ctx, NULL, NULL);
	Avm2XmlListExt* le = (Avm2XmlListExt*) obj->native_ext;
	Avm2XmlExt* vx = avm2_xml_ext_of(value);
	if (vx != NULL)
	{
		// new XMLList(xmlObj)[0] === xmlObj (identity preserved).
		avm2_xmllist_push(ctx, le, vx->node);
		return avm2_object_value(obj);
	}
	Avm2XmlSettings* st = avm2_xml_settings();
	uint32_t n = 0;
	E4XNode** nodes = avm2_e4x_parse(ctx, value, st->ignore_comments, st->ignore_pi,
	                                 st->ignore_white, &n);
	for (uint32_t i = 0; i < n; i++)
	{
		avm2_xmllist_push(ctx, le, nodes[i]);
	}
	avm2_e4x_parse_free(ctx, nodes);
	return avm2_object_value(obj);
}

static Avm2Value xmllist_call(Avm2Context* ctx, Avm2Class* cls,
                              const Avm2Value* args, uint32_t argc)
{
	if (argc == 1 && avm2_xmllist_ext_of(args[0]) != NULL)
	{
		return args[0];
	}
	return xmllist_construct(ctx, cls, args, argc);
}

// ---------------------------------------------------------------------------
// XML instance natives
// ---------------------------------------------------------------------------

static Avm2Value n_xml_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlExt* xe = avm2_xml_ext_of(act->this_val);
	if (xe == NULL)
	{
		// XML.prototype.toString() on the bare prototype returns "".
		return avm2_string(avm2_string_from_literal(ctx, ""));
	}
	return avm2_string(avm2_e4x_to_string(ctx, xe->node));
}

static Avm2Value n_xml_to_xml_string(Avm2Activation* act)
{
	return avm2_string(avm2_e4x_to_xml_string(act->ctx, this_xml_node(act)));
}

static Avm2Value n_xml_local_name(Avm2Activation* act)
{
	E4XNode* node = this_xml_node(act);
	if (node->local == NULL) return avm2_null();
	return avm2_string(node->local);
}

static Avm2Value n_xml_name(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	if (node->local == NULL) return avm2_null();
	const Avm2String* uri = node->has_ns ? node->ns.uri
	                                     : avm2_string_from_literal(ctx, "");
	Avm2Object* qn = avm2_qname_new(ctx, uri, node->local);
	((Avm2QNameExt*) qn->native_ext)->is_attribute =
		(node->kind == E4X_ATTRIBUTE);
	return avm2_object_value(qn);
}

static _Noreturn void throw_1117(Avm2Context* ctx, const Avm2String* name)
{
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1117: Invalid XML name: %.*s.",
	                 (int) name->len, name->utf8);
}

static Avm2Value n_xml_set_name(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	if (node->kind == E4X_TEXT || node->kind == E4X_CDATA
	    || node->kind == E4X_COMMENT)
	{
		return avm2_undefined();
	}
	Avm2Value name = arg_or_undef(act, 0);
	const Avm2QNameExt* q = avm2_qname_ext_of(name);
	const Avm2String* local;
	const Avm2String* uri = NULL;
	if (q != NULL && q->uri == NULL)
	{
		// Any-namespace QName: use its local name only.
		local = (q->local != NULL) ? q->local : avm2_string_from_literal(ctx, "*");
	}
	else if (q != NULL)
	{
		local = (q->local != NULL) ? q->local : avm2_string_from_literal(ctx, "*");
		uri = q->uri;
	}
	else if (name.kind == AVM2_VALUE_UNDEFINED)
	{
		// setName routes its argument through `new QName(name)`, and the
		// QName constructor maps a single UNDEFINED argument to the EMPTY
		// local name rather than the string "undefined" (Ruffle
		// globals/q_name.rs). An empty name is not an XML name, so this is
		// the whole of Error1117InvalidXmlName's `xml.setName(undefined)`.
		local = avm2_string_from_literal(ctx, "");
	}
	else
	{
		local = avm2_coerce_to_string(ctx, name);
	}
	if (!avm2_e4x_is_xml_name(local)) throw_1117(ctx, local);
	node->local = local;
	if (node->kind == E4X_PI || uri == NULL || uri->len == 0)
	{
		// A plain-string / empty-URI name CLEARS the namespace
		// (xml_set_name: setName("simpleName") drops the xmlns).
		node->has_ns = 0;
	}
	else
	{
		node->has_ns = 1;
		node->ns.uri = uri;
		node->ns.prefix = NULL;
		// add_in_scope_namespace no-ops for prefixless namespaces, so no
		// permanent declaration is recorded (the serializer prints the
		// transient xmlns as long as the name keeps the namespace).
		if (node->kind == E4X_ATTRIBUTE && node->parent != NULL)
		{
			E4XNamespace add = { uri, NULL };
			avm2_e4x_add_in_scope_namespace(ctx, node->parent, &add);
		}
		else if (node->kind == E4X_ELEMENT)
		{
			E4XNamespace add = { uri, NULL };
			avm2_e4x_add_in_scope_namespace(ctx, node, &add);
		}
	}
	avm2_xml_notify(ctx, node, "nameSet", name, avm2_string(local));
	return avm2_undefined();
}

static Avm2Value n_xml_set_local_name(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	if (node->kind == E4X_TEXT || node->kind == E4X_CDATA
	    || node->kind == E4X_COMMENT)
	{
		return avm2_undefined();
	}
	Avm2Value name = arg_or_undef(act, 0);
	const Avm2QNameExt* q = avm2_qname_ext_of(name);
	const Avm2String* local = (q != NULL && q->local != NULL)
		? q->local : avm2_coerce_to_string(ctx, name);
	if (!avm2_e4x_is_xml_name(local)) throw_1117(ctx, local);
	const Avm2String* previous = node->local;
	node->local = local;
	if (previous != NULL)
	{
		avm2_xml_notify(ctx, node, "nameSet",
		                avm2_string(local), avm2_string(previous));
	}
	return avm2_undefined();
}

// E4XNamespace -> Namespace object (Ruffle as_namespace_object): a
// prefixless empty-URI namespace gets prefix ""; a non-XML-name prefix is
// dropped; a non-empty prefix on an empty URI is error 1098.
static Avm2Object* ns_to_object(Avm2Context* ctx, const E4XNamespace* ns)
{
	const Avm2String* prefix = ns->prefix;
	if (prefix != NULL)
	{
		if (ns->uri->len == 0 && prefix->len > 0)
		{
			avm2_throw_error(ctx, ctx->builtins.type_error_class,
			                 "Error #1098: Illegal prefix %.*s for no namespace.",
			                 (int) prefix->len, prefix->utf8);
		}
		if (prefix->len > 0 && !avm2_e4x_is_xml_name(prefix))
		{
			prefix = NULL;
		}
	}
	else if (ns->uri->len == 0)
	{
		prefix = avm2_string_from_literal(ctx, "");
	}
	return avm2_namespace_new(ctx, ns->uri, prefix, 0x16);
}

// namespace() / namespace(prefix) — argc distinguishes the two.
static Avm2Value n_xml_namespace(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	E4XNamespace* in_scope;
	uint32_t n = avm2_e4x_in_scope_namespaces(ctx, node, &in_scope);
	Avm2Value out = avm2_undefined();
	if (act->argc == 0)
	{
		if (node->kind != E4X_ELEMENT && node->kind != E4X_ATTRIBUTE)
		{
			out = avm2_null();
		}
		else
		{
			E4XNamespace ns = avm2_e4x_get_namespace(ctx, node, in_scope, n);
			out = avm2_object_value(ns_to_object(ctx, &ns));
		}
		heap_free(ctx->app, in_scope);
		return out;
	}
	// (the coercion can run AS3 and throw: leaks only the scratch list)
	const Avm2String* prefix = avm2_coerce_to_string(ctx, act->args[0]);
	for (uint32_t i = 0; i < n; i++)
	{
		if (in_scope[i].prefix != NULL && in_scope[i].prefix->len == prefix->len
		    && memcmp(in_scope[i].prefix->utf8, prefix->utf8, prefix->len) == 0)
		{
			out = avm2_object_value(ns_to_object(ctx, &in_scope[i]));
			break;
		}
	}
	heap_free(ctx->app, in_scope);
	return out;
}

// Coerce an argument to a Namespace ext ({uri, prefix}).
static E4XNamespace ns_from_value(Avm2Context* ctx, Avm2Value v)
{
	E4XNamespace out;
	Avm2NamespaceExt* n = avm2_namespace_ext_of(v);
	if (n != NULL)
	{
		out.uri = n->uri;
		out.prefix = n->prefix;
		return out;
	}
	Avm2QNameExt* q = avm2_qname_ext_of(v);
	if (q != NULL && q->uri != NULL)
	{
		out.uri = q->uri;
		out.prefix = NULL;
		return out;
	}
	out.uri = avm2_coerce_to_string(ctx, v);
	out.prefix = NULL;
	return out;
}

static Avm2Value n_xml_set_namespace(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	if (node->kind != E4X_ELEMENT && node->kind != E4X_ATTRIBUTE)
	{
		return avm2_undefined();
	}
	E4XNamespace ns = ns_from_value(ctx, arg_or_undef(act, 0));
	node->has_ns = 1;
	node->ns = ns;
	if (node->kind == E4X_ATTRIBUTE && node->parent != NULL)
	{
		avm2_e4x_add_in_scope_namespace(ctx, node->parent, &ns);
	}
	else if (node->kind == E4X_ELEMENT)
	{
		avm2_e4x_add_in_scope_namespace(ctx, node, &ns);
	}
	return avm2_undefined();
}

static Avm2Value n_xml_add_namespace(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	// avmplus declares addNamespace(ns) with one REQUIRED parameter, so the
	// zero-argument call is an argument-count mismatch, not a call with
	// `undefined` (which is legal and is a no-op). e4x/XML/e13_4_4_17 only
	// checks that something throws; e13_4_4_2 grades the code.
	if (act->argc < 1)
	{
		avm2_throw_error(ctx, ctx->builtins.argument_error_class,
		                 "Error #1063: Argument count mismatch on "
		                 "XML/addNamespace(). Expected 1, got 0.");
	}
	E4XNode* node = this_xml_node(act);
	E4XNamespace ns = ns_from_value(ctx, arg_or_undef(act, 0));
	avm2_e4x_add_in_scope_namespace(ctx, node, &ns);
	return act->this_val;
}

static void remove_namespace_rec(Avm2Context* ctx, E4XNode* node,
                                 const E4XNamespace* ns)
{
	if (node->kind != E4X_ELEMENT) return;
	E4XNamespace* in_scope;
	uint32_t n = avm2_e4x_in_scope_namespaces(ctx, node, &in_scope);
	E4XNamespace own = avm2_e4x_get_namespace(ctx, node, in_scope, n);
	heap_free(ctx->app, in_scope);
	int ns_prefix_none = (ns->prefix == NULL);
	// The node's own namespace is never removed.
	int own_matches = ns_prefix_none
		? (own.uri->len == ns->uri->len
		   && memcmp(own.uri->utf8, ns->uri->utf8, ns->uri->len) == 0)
		: (own.uri->len == ns->uri->len
		   && memcmp(own.uri->utf8, ns->uri->utf8, ns->uri->len) == 0
		   && own.prefix != NULL && own.prefix->len == ns->prefix->len
		   && memcmp(own.prefix->utf8, ns->prefix->utf8, ns->prefix->len) == 0);
	if (own_matches) return;
	for (uint32_t i = 0; i < node->attr_count; i++)
	{
		E4XNode* a = node->attributes[i];
		if (a->has_ns && a->ns.uri->len == ns->uri->len
		    && memcmp(a->ns.uri->utf8, ns->uri->utf8, ns->uri->len) == 0)
		{
			return;  // in use by an attribute
		}
	}
	uint32_t w = 0;
	for (uint32_t i = 0; i < node->ns_count; i++)
	{
		E4XNamespace* d = &node->namespaces[i];
		int uri_eq = d->uri->len == ns->uri->len
		             && memcmp(d->uri->utf8, ns->uri->utf8, ns->uri->len) == 0;
		int drop;
		if (ns_prefix_none)
		{
			drop = uri_eq;
		}
		else
		{
			drop = uri_eq && d->prefix != NULL
			       && d->prefix->len == ns->prefix->len
			       && memcmp(d->prefix->utf8, ns->prefix->utf8, ns->prefix->len) == 0;
		}
		if (!drop) node->namespaces[w++] = *d;
	}
	node->ns_count = w;
	for (uint32_t i = 0; i < node->child_count; i++)
	{
		remove_namespace_rec(ctx, node->children[i], ns);
	}
}

static Avm2Value n_xml_remove_namespace(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	if (node->kind == E4X_ELEMENT)
	{
		E4XNamespace ns = ns_from_value(ctx, arg_or_undef(act, 0));
		remove_namespace_rec(ctx, node, &ns);
	}
	return act->this_val;
}

static Avm2Value n_xml_namespace_declarations(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	Avm2Object* arr = avm2_array_new(ctx, 0);
	if (node->kind != E4X_ELEMENT) return avm2_object_value(arr);
	E4XNamespace* anc = NULL;
	uint32_t anc_n = 0;
	if (node->parent != NULL)
	{
		anc_n = avm2_e4x_in_scope_namespaces(ctx, node->parent, &anc);
	}
	E4XNamespace* own;
	uint32_t own_n = avm2_e4x_in_scope_namespaces(ctx, node, &own);
	for (uint32_t i = 0; i < own_n; i++)
	{
		int in_anc = 0;
		for (uint32_t j = 0; j < anc_n && !in_anc; j++)
		{
			int peq = (anc[j].prefix == NULL) == (own[i].prefix == NULL)
			          && (anc[j].prefix == NULL
			              || (anc[j].prefix->len == own[i].prefix->len
			                  && memcmp(anc[j].prefix->utf8, own[i].prefix->utf8,
			                            anc[j].prefix->len) == 0));
			in_anc = peq && anc[j].uri->len == own[i].uri->len
			         && memcmp(anc[j].uri->utf8, own[i].uri->utf8,
			                   anc[j].uri->len) == 0;
		}
		if (!in_anc)
		{
			avm2_array_push(ctx, arr, avm2_object_value(ns_to_object(ctx, &own[i])));
		}
	}
	if (anc != NULL) heap_free(ctx->app, anc);
	heap_free(ctx->app, own);
	return avm2_object_value(arr);
}

static Avm2Value n_xml_in_scope_namespaces(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	Avm2Object* arr = avm2_array_new(ctx, 0);
	E4XNamespace* in_scope;
	uint32_t n = avm2_e4x_in_scope_namespaces(ctx, node, &in_scope);
	for (uint32_t i = 0; i < n; i++)
	{
		avm2_array_push(ctx, arr, avm2_object_value(ns_to_object(ctx, &in_scope[i])));
	}
	heap_free(ctx->app, in_scope);
	if (n == 0)
	{
		// avmplus: never an empty array — push the default namespace.
		avm2_array_push(ctx, arr, avm2_object_value(avm2_namespace_new(
			ctx, avm2_string_from_literal(ctx, ""),
			avm2_string_from_literal(ctx, ""), 0x16)));
	}
	return avm2_object_value(arr);
}

static Avm2Value n_xml_attribute(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	E4XName name = avm2_e4x_name_from_value(ctx, arg_or_undef(act, 0), 1);
	name.is_attribute = 1;
	return avm2_object_value(
		xml_get_child_list(ctx, act->this_val.u.obj, node, &name));
}

static Avm2Value n_xml_attributes(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	E4XName any;
	memset(&any, 0, sizeof(any));
	any.is_attribute = 1;
	any.any_ns = 1;
	return avm2_object_value(
		xml_get_child_list(ctx, act->this_val.u.obj, node, &any));
}

static Avm2Value n_xml_child(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	Avm2Value arg = arg_or_undef(act, 0);
	// Numeric child(n) indexes into children.
	uint32_t idx;
	int is_index = avm2_value_as_index(arg, &idx);
	if (!is_index && arg.kind == AVM2_VALUE_STRING)
	{
		E4XName p;
		memset(&p, 0, sizeof(p));
		p.local = arg.u.str;
		is_index = name_parses_index(&p, &idx);
	}
	if (is_index)
	{
		Avm2Object* out = avm2_xmllist_new(ctx, act->this_val.u.obj, NULL);
		Avm2XmlListExt* oe = (Avm2XmlListExt*) out->native_ext;
		if (node->kind == E4X_ELEMENT && idx < node->child_count)
		{
			avm2_xmllist_push(ctx, oe, node->children[idx]);
			oe->target_dirty = 1;
		}
		return avm2_object_value(out);
	}
	E4XName name = avm2_e4x_name_from_value(ctx, arg, 0);
	return avm2_object_value(
		xml_get_child_list(ctx, act->this_val.u.obj, node, &name));
}

static Avm2Value n_xml_children(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	E4XName any;
	memset(&any, 0, sizeof(any));
	any.any_ns = 1;
	return avm2_object_value(
		xml_get_child_list(ctx, act->this_val.u.obj, node, &any));
}

static Avm2Value n_xml_child_index(Avm2Activation* act)
{
	return avm2_integer(avm2_e4x_child_index(this_xml_node(act)));
}

static int str_eq_pi(const Avm2String* a, const Avm2String* b);

// Kind-filtered child lists (comments / text / processingInstructions).
static Avm2Value xml_kind_filter(Avm2Activation* act, uint8_t kind, int match_pi_name)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	Avm2Object* out = avm2_xmllist_new(ctx, act->this_val.u.obj, NULL);
	Avm2XmlListExt* oe = (Avm2XmlListExt*) out->native_ext;
	E4XName name;
	memset(&name, 0, sizeof(name));
	if (match_pi_name)
	{
		name = name_arg_default_any(act, 0, 0);
		// ToXMLName turns a leading '@' into an AttributeName, and an
		// attribute name can never match a processing instruction -- so
		// `processingInstructions('@xml-stylesheet')` is the EMPTY list, not
		// the PI whose target happens to be spelled the same
		// (e4x/XML/e13_4_4_28, e4x/XMLList/e13_5_4_17).
		if (name.is_attribute)
		{
			return avm2_object_value(out);
		}
	}
	if (node->kind == E4X_ELEMENT)
	{
		for (uint32_t i = 0; i < node->child_count; i++)
		{
			E4XNode* c = node->children[i];
			int k_ok = (kind == E4X_TEXT)
				? (c->kind == E4X_TEXT || c->kind == E4X_CDATA)
				: (c->kind == kind);
			if (!k_ok) continue;
			if (match_pi_name && name.local != NULL
			    && !str_eq_pi(c->local, name.local))
			{
				continue;
			}
			avm2_xmllist_push(ctx, oe, c);
		}
	}
	if (oe->count > 0) oe->target_dirty = 1;
	return avm2_object_value(out);
}

static int str_eq_pi(const Avm2String* a, const Avm2String* b)
{
	if (a == NULL || b == NULL) return 0;
	return a->len == b->len && memcmp(a->utf8, b->utf8, a->len) == 0;
}

static Avm2Value n_xml_comments(Avm2Activation* act)
{
	return xml_kind_filter(act, E4X_COMMENT, 0);
}

static Avm2Value n_xml_text(Avm2Activation* act)
{
	return xml_kind_filter(act, E4X_TEXT, 0);
}

static Avm2Value n_xml_processing_instructions(Avm2Activation* act)
{
	return xml_kind_filter(act, E4X_PI, 1);
}

static Avm2Value n_xml_contains(Avm2Activation* act)
{
	E4XNode* node = this_xml_node(act);
	Avm2XmlExt* ox = avm2_xml_ext_of(arg_or_undef(act, 0));
	if (ox == NULL) return avm2_bool(false);
	return avm2_bool(avm2_e4x_equals(node, ox->node) != 0);
}

static Avm2Value n_xml_copy(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* copy = avm2_e4x_deep_copy(ctx, this_xml_node(act));
	return avm2_object_value(avm2_xml_object_for_node(ctx, copy));
}

static Avm2Value n_xml_descendants(Avm2Activation* act)
{
	E4XName name = name_arg_default_any(act, 0, 0);
	return avm2_xml_descendants_value(act->ctx, act->this_val, &name);
}

static Avm2Value n_xml_elements(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	E4XName name = name_arg_default_any(act, 0, 0);
	Avm2Object* out = avm2_xmllist_new(ctx, act->this_val.u.obj, &name);
	Avm2XmlListExt* oe = (Avm2XmlListExt*) out->native_ext;
	if (node->kind == E4X_ELEMENT)
	{
		for (uint32_t i = 0; i < node->child_count; i++)
		{
			E4XNode* c = node->children[i];
			if (c->kind == E4X_ELEMENT && avm2_e4x_matches_name(c, &name))
			{
				avm2_xmllist_push(ctx, oe, c);
			}
		}
	}
	if (oe->count > 0) oe->target_dirty = 1;
	return avm2_object_value(out);
}

static Avm2Value n_xml_has_complex_content(Avm2Activation* act)
{
	return avm2_bool(avm2_e4x_has_complex_content(this_xml_node(act)) != 0);
}

static Avm2Value n_xml_has_simple_content(Avm2Activation* act)
{
	return avm2_bool(avm2_e4x_has_simple_content(this_xml_node(act)) != 0);
}

// Ruffle Activation::caller_movie_or_root().version(): the SWF version of the
// movie whose code called this native — NOT the root's. A native builtin's own
// `act->file` is NULL, so the caller is the nearest debug-callstack frame that
// HAS an ABC file; no such frame means the MAIN movie. (avm2/xml_appendchild_swf_v21
// is a v20 root that Loader-loads a v21 child: the >=21 detach rule is the
// CHILD's, and reading the global ctx->swf_version got the root's v20.)
static uint8_t xml_caller_swf_version(Avm2Activation* act)
{
	const Avm2AbcFileData* data = (act->file != NULL) ? act->file->data : NULL;
	if (data == NULL)
	{
		Avm2Context* c = act->ctx;
		for (uint32_t i = c->call_depth; i > 0 && data == NULL; i--)
		{
			Avm2AbcFileRt* f = c->call_frames[i - 1].method.file;
			if (f != NULL) data = f->data;
		}
	}
	const Avm2MovieTables* mv = avm2_display_movie_for_abc(data);
	return (mv != NULL) ? mv->swf_version : act->ctx->swf_version;
}

// maybe_escape_child (e4x.rs): SWF<=9 wraps non-XML args (ROOT swf version);
// caller-movie >=21 detaches an XML arg from its old parent.
static Avm2Value maybe_escape_child(Avm2Activation* act, Avm2Value child)
{
	Avm2Context* ctx = act->ctx;
	int is_xmlish = avm2_value_is_xmlish(child);
	if (ctx->swf_version <= 9)
	{
		if (is_xmlish) return child;
		const Avm2String* s = avm2_coerce_to_string(ctx, child);
		E4XNode* node = xml_init_node(ctx, avm2_string(s));
		return avm2_object_value(avm2_xml_object_for_node(ctx, node));
	}
	if (xml_caller_swf_version(act) >= 21)
	{
		Avm2XmlExt* xe = avm2_xml_ext_of(child);
		if (xe != NULL && xe->node->parent != NULL)
		{
			int ci = avm2_e4x_child_index(xe->node);
			if (ci >= 0)
			{
				avm2_e4x_delete_by_index(xe->node->parent, (uint32_t) ci);
			}
		}
	}
	return child;
}

static Avm2Value n_xml_append_child(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	Avm2Value child = maybe_escape_child(act, arg_or_undef(act, 0));
	// children = this.[[Get]]("*"); children.[[Put]](children.length, child)
	E4XName any;
	memset(&any, 0, sizeof(any));
	any.any_ns = 1;
	Avm2Object* children = xml_get_child_list(ctx, act->this_val.u.obj, node, &any);
	Avm2XmlListExt* ce = (Avm2XmlListExt*) children->native_ext;
	list_set_index(ctx, children, ce->count, child);
	return act->this_val;
}

static Avm2Value n_xml_prepend_child(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	Avm2Value child = maybe_escape_child(act, arg_or_undef(act, 0));
	avm2_e4x_insert_value(ctx, node, 0, child);
	return act->this_val;
}

// insertChildAfter / insertChildBefore.
static Avm2Value xml_insert_child(Avm2Activation* act, int after)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	Avm2Value child1 = arg_or_undef(act, 0);
	Avm2Value child2 = maybe_escape_child(act, arg_or_undef(act, 1));
	if (node->kind != E4X_ELEMENT) return avm2_undefined();

	E4XNode* ref = NULL;
	Avm2XmlExt* rx = avm2_xml_ext_of(child1);
	if (rx != NULL)
	{
		ref = rx->node;
	}
	else
	{
		Avm2XmlListExt* rl = avm2_xmllist_ext_of(child1);
		if (rl != NULL && rl->count == 1) ref = rl->items[0];
	}
	if (ref != NULL)
	{
		for (uint32_t i = 0; i < node->child_count; i++)
		{
			if (node->children[i] == ref)
			{
				avm2_e4x_insert_value(ctx, node, after ? i + 1 : i, child2);
				return act->this_val;
			}
		}
		return avm2_undefined();
	}
	if (child1.kind == AVM2_VALUE_NULL)
	{
		avm2_e4x_insert_value(ctx, node, after ? 0 : node->child_count, child2);
		return act->this_val;
	}
	return avm2_undefined();
}

static Avm2Value n_xml_insert_child_after(Avm2Activation* act)
{
	return xml_insert_child(act, 1);
}

static Avm2Value n_xml_insert_child_before(Avm2Activation* act)
{
	return xml_insert_child(act, 0);
}

static Avm2Value n_xml_length(Avm2Activation* act)
{
	(void) this_xml_node(act);
	return avm2_integer(1);
}

static Avm2Value n_xml_normalize(Avm2Activation* act)
{
	avm2_e4x_normalize(act->ctx, this_xml_node(act));
	return act->this_val;
}

static Avm2Value n_xml_node_kind(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const char* s;
	switch (this_xml_node(act)->kind)
	{
		case E4X_TEXT:
		case E4X_CDATA: s = "text"; break;
		case E4X_COMMENT: s = "comment"; break;
		case E4X_PI: s = "processing-instruction"; break;
		case E4X_ATTRIBUTE: s = "attribute"; break;
		default: s = "element"; break;
	}
	return avm2_string(avm2_string_from_literal(ctx, s));
}

static Avm2Value n_xml_parent(Avm2Activation* act)
{
	E4XNode* node = this_xml_node(act);
	if (node->parent == NULL) return avm2_undefined();
	return avm2_object_value(avm2_xml_object_for_node(act->ctx, node->parent));
}

static Avm2Value n_xml_replace(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	if (node->kind != E4X_ELEMENT) return act->this_val;
	Avm2Value prop = arg_or_undef(act, 0);
	Avm2Value value = arg_or_undef(act, 1);
	// Deep-copy XML/XMLList values.
	{
		Avm2XmlExt* vx = avm2_xml_ext_of(value);
		Avm2XmlListExt* vl = avm2_xmllist_ext_of(value);
		if (vx != NULL)
		{
			value = avm2_object_value(avm2_xml_object_for_node(
				ctx, avm2_e4x_deep_copy(ctx, vx->node)));
		}
		else if (vl != NULL)
		{
			Avm2Object* copy = avm2_xmllist_new(ctx, NULL, NULL);
			Avm2XmlListExt* ce = (Avm2XmlListExt*) copy->native_ext;
			for (uint32_t i = 0; i < vl->count; i++)
			{
				avm2_xmllist_push(ctx, ce, avm2_e4x_deep_copy(ctx, vl->items[i]));
			}
			value = avm2_object_value(copy);
		}
		else if (ctx->swf_version <= 9)
		{
			// avmplus XMLObject.cpp:1540 (Ruffle xml.rs `replace`): at ROOT
			// SWF <= 9 a non-XML value is run through the XML constructor
			// first, so markup PARSES ("<phone>1234567</phone>" becomes an
			// element, not a text node) — e4x/XML/e13_4_4_32/v9. From v10 on
			// the string is used verbatim (the /v10 sibling grades that).
			value = xml_construct(ctx, NULL, &value, 1);
		}
	}
	uint32_t idx;
	if (avm2_value_as_index(prop, &idx))
	{
		avm2_e4x_replace(ctx, node, idx, value);
		return act->this_val;
	}
	E4XName name = avm2_e4x_name_from_value(ctx, prop, 0);
	{
		E4XName probe = name;
		if (name_parses_index(&probe, &idx))
		{
			avm2_e4x_replace(ctx, node, idx, value);
			return act->this_val;
		}
	}
	uint32_t first_idx;
	E4XNode* first;
	if (avm2_e4x_remove_matching(ctx, node, &name, 0, &first_idx, &first))
	{
		E4XNode* dummy = avm2_e4x_node_new(ctx, E4X_ELEMENT, NULL);
		avm2_e4x_insert_at(ctx, node, first_idx, dummy);
		avm2_e4x_replace(ctx, node, first_idx, value);
	}
	return act->this_val;
}

static Avm2Value n_xml_set_children(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XNode* node = this_xml_node(act);
	E4XName star;
	memset(&star, 0, sizeof(star));
	star.any_ns = 1;
	xml_set_local(ctx, act->this_val.u.obj, node, &star, arg_or_undef(act, 0));
	return act->this_val;
}

static Avm2Value n_xml_value_of(Avm2Activation* act)
{
	return act->this_val;
}

static Avm2Value n_xml_to_json(Avm2Activation* act)
{
	return avm2_string(avm2_string_from_literal(act->ctx, "XML"));
}

// ---------------------------------------------------------------------------
// XMLList instance natives
// ---------------------------------------------------------------------------

static Avm2Value n_list_length(Avm2Activation* act)
{
	return avm2_integer((int32_t) this_list(act)->count);
}

static int list_has_simple_content(Avm2Context* ctx, Avm2XmlListExt* le)
{
	(void) ctx;
	if (le->count == 0) return 1;
	if (le->count == 1) return avm2_e4x_has_simple_content(le->items[0]);
	for (uint32_t i = 0; i < le->count; i++)
	{
		if (le->items[i]->kind == E4X_ELEMENT) return 0;
	}
	return 1;
}

static Avm2Value n_list_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlListExt* le = avm2_xmllist_ext_of(act->this_val);
	if (le == NULL)
	{
		return avm2_string(avm2_string_from_literal(ctx, ""));
	}
	if (list_has_simple_content(ctx, le))
	{
		const Avm2String* s = avm2_string_from_literal(ctx, "");
		for (uint32_t i = 0; i < le->count; i++)
		{
			E4XNode* c = le->items[i];
			if (c->kind == E4X_COMMENT || c->kind == E4X_PI) continue;
			s = avm2_string_concat(ctx, s, avm2_e4x_to_string(ctx, c));
		}
		return avm2_string(s);
	}
	// Complex: toXMLString.
	const Avm2String* s = avm2_string_from_literal(ctx, "");
	for (uint32_t i = 0; i < le->count; i++)
	{
		if (i > 0) s = avm2_string_concat(ctx, s, avm2_string_from_literal(ctx, "\n"));
		s = avm2_string_concat(ctx, s, avm2_e4x_to_xml_string(ctx, le->items[i]));
	}
	return avm2_string(s);
}

static Avm2Value n_list_to_xml_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlListExt* le = this_list(act);
	const Avm2String* s = avm2_string_from_literal(ctx, "");
	for (uint32_t i = 0; i < le->count; i++)
	{
		if (i > 0) s = avm2_string_concat(ctx, s, avm2_string_from_literal(ctx, "\n"));
		s = avm2_string_concat(ctx, s, avm2_e4x_to_xml_string(ctx, le->items[i]));
	}
	return avm2_string(s);
}

static Avm2Value n_list_child(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlListExt* le = this_list(act);
	Avm2Object* out = avm2_xmllist_new(ctx, act->this_val.u.obj, NULL);
	Avm2XmlListExt* oe = (Avm2XmlListExt*) out->native_ext;
	for (uint32_t i = 0; i < le->count; i++)
	{
		if (le->items[i]->kind != E4X_ELEMENT) continue;
		// Reuse the XML child() semantics per item.
		Avm2Value item = avm2_object_value(
			avm2_xml_object_for_node(ctx, le->items[i]));
		Avm2Value sub = avm2_call_public_property(ctx, item, "child", 5,
		                                          act->args, act->argc > 0 ? 1 : 0);
		Avm2XmlListExt* se = avm2_xmllist_ext_of(sub);
		if (se != NULL && se->count > 0)
		{
			list_append_list(oe, ctx, sub.u.obj);
		}
	}
	return avm2_object_value(out);
}

static Avm2Value n_list_children(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlListExt* le = this_list(act);
	E4XName any;
	memset(&any, 0, sizeof(any));
	any.any_ns = 1;
	Avm2Object* out = avm2_xmllist_new(ctx, act->this_val.u.obj, &any);
	Avm2XmlListExt* oe = (Avm2XmlListExt*) out->native_ext;
	for (uint32_t i = 0; i < le->count; i++)
	{
		E4XNode* item = le->items[i];
		if (item->kind != E4X_ELEMENT) continue;
		for (uint32_t j = 0; j < item->child_count; j++)
		{
			avm2_xmllist_push(ctx, oe, item->children[j]);
		}
	}
	return avm2_object_value(out);
}

static Avm2Value n_list_contains(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlListExt* le = this_list(act);
	Avm2Value v = arg_or_undef(act, 0);
	for (uint32_t i = 0; i < le->count; i++)
	{
		if (xml_node_abstract_eq(ctx, le->items[i], v)) return avm2_bool(true);
	}
	return avm2_bool(false);
}

static Avm2Value n_list_copy(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlListExt* le = this_list(act);
	list_reevaluate_target(ctx, le);
	Avm2Object* copy = avm2_xmllist_new(
		ctx, le->target_object, le->has_target_prop ? &le->target_prop : NULL);
	Avm2XmlListExt* ce = (Avm2XmlListExt*) copy->native_ext;
	for (uint32_t i = 0; i < le->count; i++)
	{
		avm2_xmllist_push(ctx, ce, avm2_e4x_deep_copy(ctx, le->items[i]));
	}
	return avm2_object_value(copy);
}

static Avm2Value n_list_attribute(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XName name = avm2_e4x_name_from_value(ctx, arg_or_undef(act, 0), 1);
	name.is_attribute = 1;
	return avm2_object_value(
		list_get_child_list(ctx, act->this_val.u.obj, &name));
}

static Avm2Value n_list_attributes(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	E4XName any;
	memset(&any, 0, sizeof(any));
	any.is_attribute = 1;
	any.any_ns = 1;
	return avm2_object_value(
		list_get_child_list(ctx, act->this_val.u.obj, &any));
}

static Avm2Value n_list_descendants(Avm2Activation* act)
{
	E4XName name = name_arg_default_any(act, 0, 0);
	return avm2_xml_descendants_value(act->ctx, act->this_val, &name);
}

// Kind filters over element items' children.
static Avm2Value list_kind_filter(Avm2Activation* act, uint8_t kind, int match_pi_name)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlListExt* le = this_list(act);
	Avm2Object* out = avm2_xmllist_new(ctx, act->this_val.u.obj, NULL);
	Avm2XmlListExt* oe = (Avm2XmlListExt*) out->native_ext;
	E4XName name;
	memset(&name, 0, sizeof(name));
	if (match_pi_name)
	{
		// Same AttributeName rule as xml_kind_filter (e4x/XMLList/e13_5_4_17).
		name = name_arg_default_any(act, 0, 0);
		if (name.is_attribute)
		{
			return avm2_object_value(out);
		}
	}
	for (uint32_t i = 0; i < le->count; i++)
	{
		E4XNode* item = le->items[i];
		if (item->kind != E4X_ELEMENT) continue;
		for (uint32_t j = 0; j < item->child_count; j++)
		{
			E4XNode* c = item->children[j];
			int k_ok = (kind == E4X_TEXT)
				? (c->kind == E4X_TEXT || c->kind == E4X_CDATA)
				: (c->kind == kind);
			if (!k_ok) continue;
			if (match_pi_name && name.local != NULL
			    && !str_eq_pi(c->local, name.local))
			{
				continue;
			}
			avm2_xmllist_push(ctx, oe, c);
		}
	}
	if (oe->count > 0) oe->target_dirty = 1;
	return avm2_object_value(out);
}

static Avm2Value n_list_text(Avm2Activation* act)
{
	return list_kind_filter(act, E4X_TEXT, 0);
}

static Avm2Value n_list_comments(Avm2Activation* act)
{
	return list_kind_filter(act, E4X_COMMENT, 0);
}

static Avm2Value n_list_processing_instructions(Avm2Activation* act)
{
	return list_kind_filter(act, E4X_PI, 1);
}

static Avm2Value n_list_elements(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlListExt* le = this_list(act);
	E4XName name = name_arg_default_any(act, 0, 0);
	Avm2Object* out = avm2_xmllist_new(ctx, act->this_val.u.obj, &name);
	Avm2XmlListExt* oe = (Avm2XmlListExt*) out->native_ext;
	for (uint32_t i = 0; i < le->count; i++)
	{
		E4XNode* item = le->items[i];
		if (item->kind != E4X_ELEMENT) continue;
		for (uint32_t j = 0; j < item->child_count; j++)
		{
			E4XNode* c = item->children[j];
			if (c->kind == E4X_ELEMENT && avm2_e4x_matches_name(c, &name))
			{
				avm2_xmllist_push(ctx, oe, c);
			}
		}
	}
	if (oe->count > 0) oe->target_dirty = 1;
	return avm2_object_value(out);
}

static Avm2Value n_list_parent(Avm2Activation* act)
{
	Avm2XmlListExt* le = this_list(act);
	if (le->count == 0) return avm2_undefined();
	E4XNode* parent = le->items[0]->parent;
	for (uint32_t i = 1; i < le->count; i++)
	{
		if (le->items[i]->parent != parent) return avm2_undefined();
	}
	if (parent == NULL) return avm2_undefined();
	return avm2_object_value(avm2_xml_object_for_node(act->ctx, parent));
}

static Avm2Value n_list_normalize(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlListExt* le = this_list(act);
	uint32_t i = 0;
	while (i < le->count)
	{
		E4XNode* c = le->items[i];
		if (c->kind == E4X_ELEMENT)
		{
			avm2_e4x_normalize(ctx, c);
			i++;
		}
		else if (c->kind == E4X_TEXT || c->kind == E4X_CDATA)
		{
			while (i + 1 < le->count
			       && (le->items[i + 1]->kind == E4X_TEXT
			           || le->items[i + 1]->kind == E4X_CDATA))
			{
				c->text = avm2_string_concat(ctx, c->text, le->items[i + 1]->text);
				// Remove from the list and the tree.
				E4XNode* victim = le->items[i + 1];
				list_remove_at(le, i + 1);
				if (victim->parent != NULL)
				{
					avm2_e4x_remove_child(victim->parent, victim);
				}
			}
			if (c->text->len == 0)
			{
				E4XNode* victim = le->items[i];
				list_remove_at(le, i);
				if (victim->parent != NULL)
				{
					avm2_e4x_remove_child(victim->parent, victim);
				}
			}
			else
			{
				i++;
			}
		}
		else
		{
			i++;
		}
	}
	return act->this_val;
}

static Avm2Value n_list_has_simple_content(Avm2Activation* act)
{
	return avm2_bool(list_has_simple_content(act->ctx, this_list(act)) != 0);
}

static Avm2Value n_list_has_complex_content(Avm2Activation* act)
{
	Avm2XmlListExt* le = this_list(act);
	if (le->count == 0) return avm2_bool(false);
	if (le->count == 1)
	{
		return avm2_bool(avm2_e4x_has_complex_content(le->items[0]) != 0);
	}
	for (uint32_t i = 0; i < le->count; i++)
	{
		if (le->items[i]->kind == E4X_ELEMENT) return avm2_bool(true);
	}
	return avm2_bool(false);
}

static Avm2Value n_list_to_json(Avm2Activation* act)
{
	return avm2_string(avm2_string_from_literal(act->ctx, "XMLList"));
}

// XMLList single-item proxy: forwards to the underlying XML instance
// method; 1086 otherwise. The method name is baked per wrapper.
static Avm2Value list_proxy(Avm2Activation* act, const char* mname)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlListExt* le = this_list(act);
	if (le->count != 1)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1086: The %s method only works on lists "
		                 "containing one item.", mname);
	}
	Avm2Value item = avm2_object_value(
		avm2_xml_object_for_node(ctx, le->items[0]));
	return avm2_call_public_property(ctx, item, mname, (uint32_t) strlen(mname),
	                                 act->args, act->argc);
}

#define LIST_PROXY(cname, asname) \
	static Avm2Value n_list_##cname(Avm2Activation* act) \
	{ \
		return list_proxy(act, asname); \
	}
LIST_PROXY(add_namespace, "addNamespace")
LIST_PROXY(append_child, "appendChild")
LIST_PROXY(child_index, "childIndex")
LIST_PROXY(in_scope_namespaces, "inScopeNamespaces")
LIST_PROXY(insert_child_after, "insertChildAfter")
LIST_PROXY(insert_child_before, "insertChildBefore")
LIST_PROXY(local_name, "localName")
LIST_PROXY(name, "name")
LIST_PROXY(namespace_declarations, "namespaceDeclarations")
LIST_PROXY(node_kind, "nodeKind")
LIST_PROXY(prepend_child, "prependChild")
LIST_PROXY(remove_namespace, "removeNamespace")
LIST_PROXY(replace, "replace")
LIST_PROXY(set_children, "setChildren")
LIST_PROXY(set_local_name, "setLocalName")
LIST_PROXY(set_name, "setName")
LIST_PROXY(set_namespace, "setNamespace")
LIST_PROXY(namespace_m, "namespace")
#undef LIST_PROXY

// ---------------------------------------------------------------------------
// Notification API (undocumented; Ruffle xml_object.rs trigger_notification)
// ---------------------------------------------------------------------------

void avm2_xml_notify(Avm2Context* ctx, E4XNode* target, const char* command,
                     Avm2Value value, Avm2Value detail)
{
	Avm2Value target_v = avm2_object_value(avm2_xml_object_for_node(ctx, target));
	for (E4XNode* cur = target; cur != NULL; cur = cur->parent)
	{
		if (cur->notify == NULL) continue;
		Avm2Value args[5];
		args[0] = avm2_object_value(avm2_xml_object_for_node(ctx, cur));
		args[1] = avm2_string(avm2_string_from_literal(ctx, command));
		args[2] = target_v;
		args[3] = value;
		args[4] = detail;
		avm2_call_value(ctx, avm2_object_value(cur->notify), avm2_null(), args, 5);
	}
}

static Avm2Value n_xml_set_notification(Avm2Activation* act)
{
	E4XNode* node = this_xml_node(act);
	Avm2Value f = arg_or_undef(act, 0);
	node->notify = (f.kind == AVM2_VALUE_OBJECT) ? f.u.obj : NULL;
	return avm2_undefined();
}

static Avm2Value n_xml_notification(Avm2Activation* act)
{
	E4XNode* node = this_xml_node(act);
	if (node->notify == NULL) return avm2_null();
	return avm2_object_value(node->notify);
}

// ---------------------------------------------------------------------------
// Settings statics
// ---------------------------------------------------------------------------

#define SETTING_BOOL(cname, field) \
	static Avm2Value n_get_##cname(Avm2Activation* act) \
	{ \
		(void) act; \
		return avm2_bool(g_xml_settings.field != 0); \
	} \
	static Avm2Value n_set_##cname(Avm2Activation* act) \
	{ \
		g_xml_settings.field = \
			(uint8_t) avm2_coerce_to_boolean(arg_or_undef(act, 0)); \
		return avm2_undefined(); \
	}
SETTING_BOOL(ignore_comments, ignore_comments)
SETTING_BOOL(ignore_pi, ignore_pi)
SETTING_BOOL(ignore_white, ignore_white)
SETTING_BOOL(pretty_printing, pretty_printing)
#undef SETTING_BOOL

static Avm2Value n_get_pretty_indent(Avm2Activation* act)
{
	(void) act;
	return avm2_integer(g_xml_settings.pretty_indent);
}

static Avm2Value n_set_pretty_indent(Avm2Activation* act)
{
	g_xml_settings.pretty_indent = avm2_coerce_to_i32(act->ctx, arg_or_undef(act, 0));
	return avm2_undefined();
}

static void settings_put(Avm2Context* ctx, Avm2Object* obj, const char* name,
                         Avm2Value v)
{
	avm2_object_set_dynamic(ctx, obj, name, (uint32_t) strlen(name), v);
}

static Avm2Value n_xml_settings(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* obj = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 0);
	obj->cls = ctx->builtins.object_class;
	obj->proto = ctx->builtins.object_class->prototype_obj;
	settings_put(ctx, obj, "ignoreComments",
	             avm2_bool(g_xml_settings.ignore_comments != 0));
	settings_put(ctx, obj, "ignoreProcessingInstructions",
	             avm2_bool(g_xml_settings.ignore_pi != 0));
	settings_put(ctx, obj, "ignoreWhitespace",
	             avm2_bool(g_xml_settings.ignore_white != 0));
	settings_put(ctx, obj, "prettyIndent",
	             avm2_integer(g_xml_settings.pretty_indent));
	settings_put(ctx, obj, "prettyPrinting",
	             avm2_bool(g_xml_settings.pretty_printing != 0));
	return avm2_object_value(obj);
}

static Avm2Value n_xml_default_settings(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* obj = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 0);
	obj->cls = ctx->builtins.object_class;
	obj->proto = ctx->builtins.object_class->prototype_obj;
	settings_put(ctx, obj, "ignoreComments", avm2_bool(true));
	settings_put(ctx, obj, "ignoreProcessingInstructions", avm2_bool(true));
	settings_put(ctx, obj, "ignoreWhitespace", avm2_bool(true));
	settings_put(ctx, obj, "prettyIndent", avm2_integer(2));
	settings_put(ctx, obj, "prettyPrinting", avm2_bool(true));
	return avm2_object_value(obj);
}

static Avm2Value n_xml_set_settings(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value s = arg_or_undef(act, 0);
	if (s.kind == AVM2_VALUE_NULL || s.kind == AVM2_VALUE_UNDEFINED)
	{
		g_xml_settings.ignore_comments = 1;
		g_xml_settings.ignore_pi = 1;
		g_xml_settings.ignore_white = 1;
		g_xml_settings.pretty_indent = 2;
		g_xml_settings.pretty_printing = 1;
		return avm2_undefined();
	}
	int found;
	Avm2Value v;
	if (avm2_has_public_property(ctx, s, "ignoreComments", 14))
	{
		v = avm2_get_public_property(ctx, s, "ignoreComments", 14, &found);
		g_xml_settings.ignore_comments = (uint8_t) avm2_coerce_to_boolean(v);
	}
	if (avm2_has_public_property(ctx, s, "ignoreProcessingInstructions", 28))
	{
		v = avm2_get_public_property(ctx, s, "ignoreProcessingInstructions", 28,
		                             &found);
		g_xml_settings.ignore_pi = (uint8_t) avm2_coerce_to_boolean(v);
	}
	if (avm2_has_public_property(ctx, s, "ignoreWhitespace", 16))
	{
		v = avm2_get_public_property(ctx, s, "ignoreWhitespace", 16, &found);
		g_xml_settings.ignore_white = (uint8_t) avm2_coerce_to_boolean(v);
	}
	if (avm2_has_public_property(ctx, s, "prettyIndent", 12))
	{
		v = avm2_get_public_property(ctx, s, "prettyIndent", 12, &found);
		g_xml_settings.pretty_indent = avm2_coerce_to_i32(ctx, v);
	}
	if (avm2_has_public_property(ctx, s, "prettyPrinting", 14))
	{
		v = avm2_get_public_property(ctx, s, "prettyPrinting", 14, &found);
		g_xml_settings.pretty_printing = (uint8_t) avm2_coerce_to_boolean(v);
	}
	return avm2_undefined();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static void add_xml_methods(Avm2Context* ctx, Avm2Class* cls)
{
	avm2_builtin_add_method(ctx, cls, "toString", n_xml_to_string);
	avm2_builtin_add_method(ctx, cls, "toXMLString", n_xml_to_xml_string);
	avm2_builtin_add_method(ctx, cls, "localName", n_xml_local_name);
	avm2_builtin_add_method(ctx, cls, "name", n_xml_name);
	avm2_builtin_add_method(ctx, cls, "setName", n_xml_set_name);
	avm2_builtin_add_method(ctx, cls, "setLocalName", n_xml_set_local_name);
	avm2_builtin_add_method(ctx, cls, "namespace", n_xml_namespace);
	avm2_builtin_add_method(ctx, cls, "setNamespace", n_xml_set_namespace);
	avm2_builtin_add_method(ctx, cls, "addNamespace", n_xml_add_namespace);
	avm2_builtin_add_method(ctx, cls, "removeNamespace", n_xml_remove_namespace);
	avm2_builtin_add_method(ctx, cls, "namespaceDeclarations",
	                        n_xml_namespace_declarations);
	avm2_builtin_add_method(ctx, cls, "inScopeNamespaces", n_xml_in_scope_namespaces);
	avm2_builtin_add_method(ctx, cls, "attribute", n_xml_attribute);
	avm2_builtin_add_method(ctx, cls, "attributes", n_xml_attributes);
	avm2_builtin_add_method(ctx, cls, "child", n_xml_child);
	avm2_builtin_add_method(ctx, cls, "children", n_xml_children);
	avm2_builtin_add_method(ctx, cls, "childIndex", n_xml_child_index);
	avm2_builtin_add_method(ctx, cls, "comments", n_xml_comments);
	avm2_builtin_add_method(ctx, cls, "contains", n_xml_contains);
	avm2_builtin_add_method(ctx, cls, "copy", n_xml_copy);
	avm2_builtin_add_method(ctx, cls, "descendants", n_xml_descendants);
	avm2_builtin_add_method(ctx, cls, "elements", n_xml_elements);
	avm2_builtin_add_method(ctx, cls, "hasComplexContent", n_xml_has_complex_content);
	avm2_builtin_add_method(ctx, cls, "hasSimpleContent", n_xml_has_simple_content);
	avm2_builtin_add_method(ctx, cls, "insertChildAfter", n_xml_insert_child_after);
	avm2_builtin_add_method(ctx, cls, "insertChildBefore", n_xml_insert_child_before);
	avm2_builtin_add_method(ctx, cls, "length", n_xml_length);
	avm2_builtin_add_method(ctx, cls, "normalize", n_xml_normalize);
	avm2_builtin_add_method(ctx, cls, "nodeKind", n_xml_node_kind);
	avm2_builtin_add_method(ctx, cls, "parent", n_xml_parent);
	avm2_builtin_add_method(ctx, cls, "prependChild", n_xml_prepend_child);
	avm2_builtin_add_method(ctx, cls, "appendChild", n_xml_append_child);
	avm2_builtin_add_method(ctx, cls, "processingInstructions",
	                        n_xml_processing_instructions);
	avm2_builtin_add_method(ctx, cls, "replace", n_xml_replace);
	avm2_builtin_add_method(ctx, cls, "setChildren", n_xml_set_children);
	avm2_builtin_add_method(ctx, cls, "text", n_xml_text);
	avm2_builtin_add_method(ctx, cls, "valueOf", n_xml_value_of);
	avm2_builtin_add_method(ctx, cls, "toJSON", n_xml_to_json);
	avm2_builtin_add_method(ctx, cls, "setNotification", n_xml_set_notification);
	avm2_builtin_add_method(ctx, cls, "notification", n_xml_notification);
}

static void add_list_methods(Avm2Context* ctx, Avm2Class* cls)
{
	avm2_builtin_add_method(ctx, cls, "length", n_list_length);
	avm2_builtin_add_method(ctx, cls, "toString", n_list_to_string);
	avm2_builtin_add_method(ctx, cls, "toXMLString", n_list_to_xml_string);
	avm2_builtin_add_method(ctx, cls, "child", n_list_child);
	avm2_builtin_add_method(ctx, cls, "children", n_list_children);
	avm2_builtin_add_method(ctx, cls, "contains", n_list_contains);
	avm2_builtin_add_method(ctx, cls, "copy", n_list_copy);
	avm2_builtin_add_method(ctx, cls, "attribute", n_list_attribute);
	avm2_builtin_add_method(ctx, cls, "attributes", n_list_attributes);
	avm2_builtin_add_method(ctx, cls, "descendants", n_list_descendants);
	avm2_builtin_add_method(ctx, cls, "text", n_list_text);
	avm2_builtin_add_method(ctx, cls, "comments", n_list_comments);
	avm2_builtin_add_method(ctx, cls, "processingInstructions",
	                        n_list_processing_instructions);
	avm2_builtin_add_method(ctx, cls, "elements", n_list_elements);
	avm2_builtin_add_method(ctx, cls, "parent", n_list_parent);
	avm2_builtin_add_method(ctx, cls, "normalize", n_list_normalize);
	avm2_builtin_add_method(ctx, cls, "hasSimpleContent", n_list_has_simple_content);
	avm2_builtin_add_method(ctx, cls, "hasComplexContent", n_list_has_complex_content);
	avm2_builtin_add_method(ctx, cls, "valueOf", n_xml_value_of);
	avm2_builtin_add_method(ctx, cls, "toJSON", n_list_to_json);
	avm2_builtin_add_method(ctx, cls, "addNamespace", n_list_add_namespace);
	avm2_builtin_add_method(ctx, cls, "appendChild", n_list_append_child);
	avm2_builtin_add_method(ctx, cls, "childIndex", n_list_child_index);
	avm2_builtin_add_method(ctx, cls, "inScopeNamespaces", n_list_in_scope_namespaces);
	avm2_builtin_add_method(ctx, cls, "insertChildAfter", n_list_insert_child_after);
	avm2_builtin_add_method(ctx, cls, "insertChildBefore", n_list_insert_child_before);
	avm2_builtin_add_method(ctx, cls, "localName", n_list_local_name);
	avm2_builtin_add_method(ctx, cls, "name", n_list_name);
	avm2_builtin_add_method(ctx, cls, "namespaceDeclarations",
	                        n_list_namespace_declarations);
	avm2_builtin_add_method(ctx, cls, "nodeKind", n_list_node_kind);
	avm2_builtin_add_method(ctx, cls, "prependChild", n_list_prepend_child);
	avm2_builtin_add_method(ctx, cls, "removeNamespace", n_list_remove_namespace);
	avm2_builtin_add_method(ctx, cls, "replace", n_list_replace);
	avm2_builtin_add_method(ctx, cls, "setChildren", n_list_set_children);
	avm2_builtin_add_method(ctx, cls, "setLocalName", n_list_set_local_name);
	avm2_builtin_add_method(ctx, cls, "setName", n_list_set_name);
	avm2_builtin_add_method(ctx, cls, "setNamespace", n_list_set_namespace);
	avm2_builtin_add_method(ctx, cls, "namespace", n_list_namespace_m);
}

void avm2_register_xml_legacy(Avm2Context* ctx);

void avm2_register_xml(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;

	Avm2Class* xml = avm2_builtin_class(ctx, "", "XML", b->object_class);
	b->xml_class = xml;
	xml->flags |= AVM2_CLASS_FLAG_FINAL;  // dynamic (not sealed)
	xml->native_construct = xml_construct;
	xml->native_call = xml_call;
	add_xml_methods(ctx, xml);
	avm2_builtin_add_static_getset(ctx, xml, "ignoreComments",
	                               n_get_ignore_comments, n_set_ignore_comments);
	avm2_builtin_add_static_getset(ctx, xml, "ignoreProcessingInstructions",
	                               n_get_ignore_pi, n_set_ignore_pi);
	avm2_builtin_add_static_getset(ctx, xml, "ignoreWhitespace",
	                               n_get_ignore_white, n_set_ignore_white);
	avm2_builtin_add_static_getset(ctx, xml, "prettyPrinting",
	                               n_get_pretty_printing, n_set_pretty_printing);
	avm2_builtin_add_static_getset(ctx, xml, "prettyIndent",
	                               n_get_pretty_indent, n_set_pretty_indent);
	avm2_builtin_add_static_method(ctx, xml, "settings", n_xml_settings);
	avm2_builtin_add_static_method(ctx, xml, "setSettings", n_xml_set_settings);
	avm2_builtin_add_static_method(ctx, xml, "defaultSettings",
	                               n_xml_default_settings);
	avm2_builtin_add_static_const(ctx, xml, "length", avm2_integer(1));

	// Prototype functions (dont_enum), reachable via .call on any receiver.
	Avm2Object* xp = xml->prototype_obj;
	avm2_proto_add_function(ctx, xp, "toString", n_xml_to_string);
	avm2_proto_add_function(ctx, xp, "toXMLString", n_xml_to_xml_string);
	avm2_proto_add_function(ctx, xp, "localName", n_xml_local_name);
	avm2_proto_add_function(ctx, xp, "name", n_xml_name);
	avm2_proto_add_function(ctx, xp, "valueOf", n_xml_value_of);
	avm2_proto_add_function(ctx, xp, "toJSON", n_xml_to_json);
	avm2_proto_add_function(ctx, xp, "nodeKind", n_xml_node_kind);
	avm2_proto_add_function(ctx, xp, "attributes", n_xml_attributes);
	avm2_proto_add_function(ctx, xp, "children", n_xml_children);
	avm2_proto_add_function(ctx, xp, "copy", n_xml_copy);
	avm2_proto_add_function(ctx, xp, "hasSimpleContent", n_xml_has_simple_content);
	avm2_proto_add_function(ctx, xp, "hasComplexContent", n_xml_has_complex_content);

	Avm2Class* xl = avm2_builtin_class(ctx, "", "XMLList", b->object_class);
	b->xml_list_class = xl;
	xl->flags |= AVM2_CLASS_FLAG_FINAL;
	xl->native_construct = xmllist_construct;
	xl->native_call = xmllist_call;
	add_list_methods(ctx, xl);
	avm2_builtin_add_static_const(ctx, xl, "length", avm2_integer(1));

	Avm2Object* lp = xl->prototype_obj;
	avm2_proto_add_function(ctx, lp, "toString", n_list_to_string);
	avm2_proto_add_function(ctx, lp, "toXMLString", n_list_to_xml_string);
	avm2_proto_add_function(ctx, lp, "valueOf", n_xml_value_of);
	avm2_proto_add_function(ctx, lp, "toJSON", n_list_to_json);
	avm2_proto_add_function(ctx, lp, "length", n_list_length);

	avm2_register_xml_legacy(ctx);
}

// ---------------------------------------------------------------------------
// Legacy flash.xml.XMLNode / XMLDocument / XMLNodeType (port of Ruffle's
// AS3 implementations in globals/flash/xml/*.as — an independent node
// model built by converting a parsed E4X tree).
// ---------------------------------------------------------------------------

enum
{
	XN_ELEMENT = 1,
	XN_TEXT = 3,
	XN_CDATA = 4,
	XN_PI = 7,
	XN_COMMENT = 8,
	XN_DOCTYPE = 10,
	XN_DECL = 13,
};

typedef struct Avm2XmlNodeExt
{
	Avm2Object* children;    // Array (identity-stable)
	Avm2Object* attributes;  // lazily-created plain object
} Avm2XmlNodeExt;

static Avm2Class* g_xmlnode_class;
static Avm2Class* g_xmldocument_class;

static Avm2XmlNodeExt* xmlnode_ext(Avm2Context* ctx, Avm2Value v)
{
	(void) ctx;
	if (v.kind != AVM2_VALUE_OBJECT || v.u.obj == NULL) return NULL;
	for (Avm2Class* c = v.u.obj->cls; c != NULL; c = c->super_class)
	{
		if (c == g_xmlnode_class) return (Avm2XmlNodeExt*) v.u.obj->native_ext;
	}
	return NULL;
}

static void xn_set(Avm2Context* ctx, Avm2Value node, const char* name, Avm2Value v)
{
	avm2_object_set_dynamic(ctx, node.u.obj, name, (uint32_t) strlen(name), v);
}

static Avm2Value xn_get(Avm2Context* ctx, Avm2Value node, const char* name)
{
	return avm2_get_public_property(ctx, node, name, (uint32_t) strlen(name), NULL);
}

static Avm2XmlNodeExt* xn_ensure_ext(Avm2Context* ctx, Avm2Object* obj)
{
	if (obj->native_ext == NULL)
	{
		obj->native_ext = avm2_alloc(ctx, sizeof(Avm2XmlNodeExt));
		memset(obj->native_ext, 0, sizeof(Avm2XmlNodeExt));
		// GC: children/attributes are direct in-blob object edges — the
		// conservative scan follows them once the span is recorded.
		obj->native_ext_size = sizeof(Avm2XmlNodeExt);
	}
	return (Avm2XmlNodeExt*) obj->native_ext;
}

static void xmlnode_init_common(Avm2Context* ctx, Avm2Value this_val,
                                uint32_t type, Avm2Value input)
{
	Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, this_val.u.obj);
	ext->children = avm2_array_new(ctx, 0);
	ext->attributes = NULL;
	xn_set(ctx, this_val, "nodeType", avm2_uint_value(type));
	if (type == XN_ELEMENT)
	{
		xn_set(ctx, this_val, "nodeName", input);
		xn_set(ctx, this_val, "nodeValue", avm2_null());
	}
	else
	{
		xn_set(ctx, this_val, "nodeName", avm2_null());
		xn_set(ctx, this_val, "nodeValue", input);
	}
	xn_set(ctx, this_val, "parentNode", avm2_null());
	xn_set(ctx, this_val, "firstChild", avm2_null());
	xn_set(ctx, this_val, "lastChild", avm2_null());
	xn_set(ctx, this_val, "previousSibling", avm2_null());
	xn_set(ctx, this_val, "nextSibling", avm2_null());
}

static Avm2Value xmlnode_ctor(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	uint32_t type = (act->argc > 0) ? avm2_coerce_to_u32(ctx, act->args[0]) : 0;
	Avm2Value input = arg_or_undef(act, 1);
	if (input.kind != AVM2_VALUE_NULL && input.kind != AVM2_VALUE_UNDEFINED)
	{
		input = avm2_string(avm2_coerce_to_string(ctx, input));
	}
	else
	{
		input = avm2_null();
	}
	xmlnode_init_common(ctx, act->this_val, type, input);
	return avm2_undefined();
}

static Avm2Value n_xn_get_attributes(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, act->this_val.u.obj);
	if (ext->attributes == NULL)
	{
		Avm2Object* o = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 0);
		o->cls = ctx->builtins.object_class;
		o->proto = ctx->builtins.object_class->prototype_obj;
		ext->attributes = o;
	}
	return avm2_object_value(ext->attributes);
}

static Avm2Value n_xn_set_attributes(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, act->this_val.u.obj);
	Avm2Value v = arg_or_undef(act, 0);
	ext->attributes = (v.kind == AVM2_VALUE_OBJECT) ? v.u.obj : NULL;
	return avm2_undefined();
}

static Avm2Value n_xn_get_child_nodes(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, act->this_val.u.obj);
	if (ext->children == NULL) ext->children = avm2_array_new(ctx, 0);
	return avm2_object_value(ext->children);
}

static Avm2Value n_xn_has_child_nodes(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, act->this_val.u.obj);
	return avm2_bool(ext->children != NULL
	                 && avm2_array_ext(ext->children)->length > 0);
}

static void xmlnode_append(Avm2Context* ctx, Avm2Value parent, Avm2Value node)
{
	Avm2Value cur_parent = xn_get(ctx, node, "parentNode");
	if (cur_parent.kind == AVM2_VALUE_OBJECT && cur_parent.u.obj == parent.u.obj)
	{
		return;
	}
	Avm2Value last = xn_get(ctx, parent, "lastChild");
	if (last.kind == AVM2_VALUE_OBJECT)
	{
		xn_set(ctx, last, "nextSibling", node);
		xn_set(ctx, node, "previousSibling", last);
	}
	else
	{
		xn_set(ctx, parent, "firstChild", node);
		xn_set(ctx, node, "previousSibling", avm2_null());
	}
	xn_set(ctx, node, "nextSibling", avm2_null());
	xn_set(ctx, node, "parentNode", parent);
	xn_set(ctx, parent, "lastChild", node);
	Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, parent.u.obj);
	if (ext->children == NULL) ext->children = avm2_array_new(ctx, 0);
	avm2_array_push(ctx, ext->children, node);
}

static Avm2Value n_xn_append_child(Avm2Activation* act)
{
	xmlnode_append(act->ctx, act->this_val, arg_or_undef(act, 0));
	return avm2_undefined();
}

static Avm2Value n_xn_insert_before(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value node = arg_or_undef(act, 0);
	Avm2Value before = arg_or_undef(act, 1);
	if (before.kind != AVM2_VALUE_OBJECT)
	{
		xmlnode_append(ctx, act->this_val, node);
		return avm2_undefined();
	}
	Avm2Value prev = xn_get(ctx, before, "previousSibling");
	Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, act->this_val.u.obj);
	if (prev.kind == AVM2_VALUE_OBJECT)
	{
		xn_set(ctx, prev, "nextSibling", node);
		Avm2ArrayExt* ae = avm2_array_ext(ext->children);
		for (uint32_t i = 0; i < ae->length; i++)
		{
			Avm2Value c = avm2_array_get(ext->children, i);
			if (c.kind == AVM2_VALUE_OBJECT && c.u.obj == before.u.obj)
			{
				// splice(i, 0, node)
				avm2_array_push(ctx, ext->children, avm2_undefined());
				for (uint32_t j = avm2_array_ext(ext->children)->length - 1;
				     j > i; j--)
				{
					avm2_array_set(ctx, ext->children, j,
					               avm2_array_get(ext->children, j - 1));
				}
				avm2_array_set(ctx, ext->children, i, node);
				break;
			}
		}
	}
	else
	{
		xn_set(ctx, act->this_val, "firstChild", node);
	}
	xn_set(ctx, node, "previousSibling", prev);
	xn_set(ctx, before, "previousSibling", node);
	xn_set(ctx, node, "nextSibling", before);
	xn_set(ctx, node, "parentNode", act->this_val);
	return avm2_undefined();
}

static Avm2Value n_xn_remove_node(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value self = act->this_val;
	Avm2Value parent = xn_get(ctx, self, "parentNode");
	Avm2Value prev = xn_get(ctx, self, "previousSibling");
	Avm2Value next = xn_get(ctx, self, "nextSibling");
	if (parent.kind == AVM2_VALUE_OBJECT)
	{
		Avm2Value pf = xn_get(ctx, parent, "firstChild");
		if (pf.kind == AVM2_VALUE_OBJECT && pf.u.obj == self.u.obj)
		{
			xn_set(ctx, parent, "firstChild", next);
		}
		Avm2Value pl = xn_get(ctx, parent, "lastChild");
		if (pl.kind == AVM2_VALUE_OBJECT && pl.u.obj == self.u.obj)
		{
			xn_set(ctx, parent, "lastChild", prev);
		}
		Avm2XmlNodeExt* pext = xn_ensure_ext(ctx, parent.u.obj);
		if (pext->children != NULL)
		{
			Avm2ArrayExt* ae = avm2_array_ext(pext->children);
			for (uint32_t i = 0; i < ae->length; i++)
			{
				Avm2Value c = avm2_array_get(pext->children, i);
				if (c.kind == AVM2_VALUE_OBJECT && c.u.obj == self.u.obj)
				{
					for (uint32_t j = i; j + 1 < ae->length; j++)
					{
						avm2_array_set(ctx, pext->children, j,
						               avm2_array_get(pext->children, j + 1));
					}
					avm2_array_set_length(ctx, pext->children, ae->length - 1);
					break;
				}
			}
		}
	}
	if (prev.kind == AVM2_VALUE_OBJECT) xn_set(ctx, prev, "nextSibling", next);
	if (next.kind == AVM2_VALUE_OBJECT) xn_set(ctx, next, "previousSibling", prev);
	xn_set(ctx, self, "parentNode", avm2_null());
	xn_set(ctx, self, "previousSibling", avm2_null());
	xn_set(ctx, self, "nextSibling", avm2_null());
	return avm2_undefined();
}

// nodeName split helpers (localName / prefix getters).
static Avm2Value n_xn_get_local_name(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value nn = xn_get(ctx, act->this_val, "nodeName");
	if (nn.kind != AVM2_VALUE_STRING) return avm2_null();
	const Avm2String* s = nn.u.str;
	for (uint32_t i = 0; i < s->len; i++)
	{
		if (s->utf8[i] == ':')
		{
			return avm2_string(avm2_string_new(ctx, s->utf8 + i + 1,
			                                   s->len - i - 1));
		}
	}
	return nn;
}

static Avm2Value n_xn_get_prefix(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value nn = xn_get(ctx, act->this_val, "nodeName");
	if (nn.kind != AVM2_VALUE_STRING) return avm2_null();
	const Avm2String* s = nn.u.str;
	for (uint32_t i = 0; i < s->len; i++)
	{
		if (s->utf8[i] == ':')
		{
			return avm2_string(avm2_string_new(ctx, s->utf8, i));
		}
	}
	return avm2_string(avm2_string_from_literal(ctx, ""));
}

// Attribute scan for xmlns declarations (getNamespaceForPrefix / URI).
static Avm2Value xn_ns_for_prefix(Avm2Context* ctx, Avm2Value node,
                                  const Avm2String* prefix)
{
	while (node.kind == AVM2_VALUE_OBJECT)
	{
		Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, node.u.obj);
		if (ext->attributes != NULL)
		{
			for (Avm2DynProp* p = ext->attributes->dyn_props; p != NULL; p = p->next)
			{
				if (p->dead) continue;
				if (p->name.len > 6 && memcmp(p->name.utf8, "xmlns:", 6) == 0
				    && p->name.len - 6 == prefix->len
				    && memcmp(p->name.utf8 + 6, prefix->utf8, prefix->len) == 0)
				{
					return p->value;
				}
			}
		}
		node = xn_get(ctx, node, "parentNode");
	}
	return avm2_null();
}

static Avm2Value n_xn_get_namespace_for_prefix(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* prefix = avm2_coerce_to_string(ctx, arg_or_undef(act, 0));
	return xn_ns_for_prefix(ctx, act->this_val, prefix);
}

static Avm2Value n_xn_get_prefix_for_namespace(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* uri = avm2_coerce_to_string(ctx, arg_or_undef(act, 0));
	Avm2Value node = act->this_val;
	while (node.kind == AVM2_VALUE_OBJECT)
	{
		Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, node.u.obj);
		if (ext->attributes != NULL)
		{
			for (Avm2DynProp* p = ext->attributes->dyn_props; p != NULL; p = p->next)
			{
				if (p->dead) continue;
				if (p->name.len > 6 && memcmp(p->name.utf8, "xmlns:", 6) == 0
				    && p->value.kind == AVM2_VALUE_STRING
				    && avm2_string_equals(p->value.u.str, uri))
				{
					return avm2_string(avm2_string_new(ctx, p->name.utf8 + 6,
					                                   p->name.len - 6));
				}
			}
		}
		node = xn_get(ctx, node, "parentNode");
	}
	return avm2_null();
}

static Avm2Value n_xn_get_namespace_uri(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value prefix = n_xn_get_prefix(act);
	if (prefix.kind == AVM2_VALUE_STRING && prefix.u.str->len > 0)
	{
		return xn_ns_for_prefix(ctx, act->this_val, prefix.u.str);
	}
	Avm2Value node = act->this_val;
	while (node.kind == AVM2_VALUE_OBJECT)
	{
		Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, node.u.obj);
		if (ext->attributes != NULL)
		{
			Avm2Value* v = avm2_object_find_dynamic(ext->attributes, "xmlns", 5);
			if (v != NULL && avm2_coerce_to_boolean(*v))
			{
				return *v;
			}
		}
		node = xn_get(ctx, node, "parentNode");
	}
	return avm2_null();
}

// Legacy _escapeXML: & < > " '.
static const Avm2String* xn_escape(Avm2Context* ctx, const Avm2String* s)
{
	uint32_t extra = 0;
	for (uint32_t i = 0; i < s->len; i++)
	{
		char c = s->utf8[i];
		if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') extra += 5;
	}
	if (extra == 0) return s;
	char* out = avm2_alloc(ctx, s->len + extra + 1);
	uint32_t n = 0;
	for (uint32_t i = 0; i < s->len; i++)
	{
		char c = s->utf8[i];
		const char* rep = NULL;
		if (c == '&') rep = "&amp;";
		else if (c == '<') rep = "&lt;";
		else if (c == '>') rep = "&gt;";
		else if (c == '"') rep = "&quot;";
		else if (c == '\'') rep = "&apos;";
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
	const Avm2String* r = avm2_string_new(ctx, out, n);  // copies
	heap_free(ctx->app, out);
	return r;
}

static const Avm2String* xmlnode_to_string(Avm2Context* ctx, Avm2Value node)
{
	Avm2Value nt = xn_get(ctx, node, "nodeType");
	uint32_t type = avm2_coerce_to_u32(ctx, nt);
	if (type != XN_ELEMENT)
	{
		Avm2Value nv = xn_get(ctx, node, "nodeValue");
		return xn_escape(ctx, avm2_coerce_to_string(ctx, nv));
	}
	Avm2Value nn = xn_get(ctx, node, "nodeName");
	int has_name = (nn.kind == AVM2_VALUE_STRING);
	const Avm2String* out = avm2_string_from_literal(ctx, "");
	if (has_name)
	{
		out = avm2_string_concat(ctx, avm2_string_from_literal(ctx, "<"), nn.u.str);
	}
	Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, node.u.obj);
	if (ext->attributes != NULL)
	{
		for (Avm2DynProp* p = ext->attributes->dyn_props; p != NULL; p = p->next)
		{
			if (p->dead || p->dont_enum) continue;
			out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, " "));
			out = avm2_string_concat(ctx, out,
			                         avm2_string_new(ctx, p->name.utf8, p->name.len));
			out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, "=\""));
			out = avm2_string_concat(ctx, out,
			                         xn_escape(ctx, avm2_coerce_to_string(ctx, p->value)));
			out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, "\""));
		}
	}
	uint32_t child_n = (ext->children != NULL)
		? avm2_array_ext(ext->children)->length : 0;
	if (child_n > 0)
	{
		if (has_name)
		{
			out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, ">"));
		}
		for (uint32_t i = 0; i < child_n; i++)
		{
			Avm2Value c = avm2_array_get(ext->children, i);
			if (c.kind == AVM2_VALUE_OBJECT)
			{
				out = avm2_string_concat(ctx, out, xmlnode_to_string(ctx, c));
			}
		}
		if (has_name)
		{
			out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, "</"));
			out = avm2_string_concat(ctx, out, nn.u.str);
			out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, ">"));
		}
	}
	else if (has_name)
	{
		out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, " />"));
	}
	return out;
}

static Avm2Value n_xn_to_string(Avm2Activation* act)
{
	return avm2_string(xmlnode_to_string(act->ctx, act->this_val));
}

static Avm2Value n_xn_clone_node(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	int deep = avm2_coerce_to_boolean(arg_or_undef(act, 0));
	Avm2Value self = act->this_val;
	uint32_t type = avm2_coerce_to_u32(ctx, xn_get(ctx, self, "nodeType"));
	Avm2Value input = (type == XN_ELEMENT) ? xn_get(ctx, self, "nodeName")
	                                       : xn_get(ctx, self, "nodeValue");
	Avm2Value args[2] = { avm2_uint_value(type), input };
	Avm2Value clone = avm2_class_construct(ctx, g_xmlnode_class, args, 2);
	Avm2XmlNodeExt* sext = xn_ensure_ext(ctx, self.u.obj);
	if (sext->attributes != NULL)
	{
		Avm2XmlNodeExt* cext = xn_ensure_ext(ctx, clone.u.obj);
		Avm2Object* ca = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 0);
		ca->cls = ctx->builtins.object_class;
		ca->proto = ctx->builtins.object_class->prototype_obj;
		for (Avm2DynProp* p = sext->attributes->dyn_props; p != NULL; p = p->next)
		{
			if (p->dead) continue;
			avm2_object_set_dynamic(ctx, ca, p->name.utf8, p->name.len, p->value);
		}
		cext->attributes = ca;
	}
	if (deep && sext->children != NULL)
	{
		uint32_t n = avm2_array_ext(sext->children)->length;
		for (uint32_t i = 0; i < n; i++)
		{
			Avm2Value c = avm2_array_get(sext->children, i);
			if (c.kind != AVM2_VALUE_OBJECT) continue;
			Avm2Value deep_arg = avm2_bool(true);
			Avm2Value cc = avm2_call_public_property(ctx, c, "cloneNode", 9,
			                                         &deep_arg, 1);
			xmlnode_append(ctx, clone, cc);
		}
	}
	return clone;
}

// XMLDocument.
static void xmldocument_parse(Avm2Context* ctx, Avm2Value this_val,
                              const Avm2String* input);

static Avm2Value xmldocument_ctor(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	xmlnode_init_common(ctx, act->this_val, XN_ELEMENT,
	                    avm2_string(avm2_string_from_literal(ctx, "")));
	xn_set(ctx, act->this_val, "nodeName", avm2_null());
	xn_set(ctx, act->this_val, "docTypeDecl", avm2_null());
	xn_set(ctx, act->this_val, "xmlDecl", avm2_null());
	xn_set(ctx, act->this_val, "ignoreWhite", avm2_bool(false));
	{
		Avm2Object* idmap = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 0);
		idmap->cls = ctx->builtins.object_class;
		idmap->proto = ctx->builtins.object_class->prototype_obj;
		xn_set(ctx, act->this_val, "idMap", avm2_object_value(idmap));
	}
	Avm2Value input = arg_or_undef(act, 0);
	if (input.kind != AVM2_VALUE_NULL && input.kind != AVM2_VALUE_UNDEFINED)
	{
		xmldocument_parse(ctx, act->this_val, avm2_coerce_to_string(ctx, input));
	}
	return avm2_undefined();
}

// Converts a parsed E4X node into an XMLNode tree (XMLDocument.as
// _convertXmlNode).
static Avm2Value xmldoc_convert(Avm2Context* ctx, E4XNode* node)
{
	uint32_t type;
	Avm2Value input;
	switch (node->kind)
	{
		case E4X_ELEMENT:
		{
			type = XN_ELEMENT;
			// Name with prefix when the resolved namespace has one.
			const Avm2String* nm = node->local;
			if (node->has_ns)
			{
				E4XNamespace* in_scope;
				uint32_t n = avm2_e4x_in_scope_namespaces(ctx, node, &in_scope);
				E4XNamespace ns = avm2_e4x_get_namespace(ctx, node, in_scope, n);
				heap_free(ctx->app, in_scope);
				if (ns.prefix != NULL && ns.prefix->len > 0)
				{
					nm = avm2_string_concat(ctx, ns.prefix,
					                        avm2_string_from_literal(ctx, ":"));
					nm = avm2_string_concat(ctx, nm, node->local);
				}
			}
			input = avm2_string(nm);
			break;
		}
		case E4X_COMMENT:
			type = XN_COMMENT;
			input = avm2_string(avm2_e4x_to_string(ctx, node));
			break;
		default:
			type = XN_TEXT;
			input = avm2_string(avm2_e4x_to_string(ctx, node));
			break;
	}
	Avm2Value args[2] = { avm2_uint_value(type), input };
	Avm2Value xn = avm2_class_construct(ctx, g_xmlnode_class, args, 2);
	for (uint32_t i = 0; i < node->child_count; i++)
	{
		xmlnode_append(ctx, xn, xmldoc_convert(ctx, node->children[i]));
	}
	if (node->kind == E4X_ELEMENT && (node->attr_count > 0 || node->ns_count > 0))
	{
		Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, xn.u.obj);
		Avm2Object* attrs = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 0);
		attrs->cls = ctx->builtins.object_class;
		attrs->proto = ctx->builtins.object_class->prototype_obj;
		for (uint32_t i = 0; i < node->attr_count; i++)
		{
			E4XNode* a = node->attributes[i];
			const Avm2String* an = a->local;
			if (a->has_ns && a->ns.prefix != NULL && a->ns.prefix->len > 0)
			{
				an = avm2_string_concat(ctx, a->ns.prefix,
				                        avm2_string_from_literal(ctx, ":"));
				an = avm2_string_concat(ctx, an, a->local);
			}
			avm2_object_set_dynamic(ctx, attrs, an->utf8, an->len,
			                        avm2_string(a->text));
		}
		for (uint32_t i = 0; i < node->ns_count; i++)
		{
			const Avm2String* name;
			if (node->namespaces[i].prefix != NULL
			    && node->namespaces[i].prefix->len > 0)
			{
				name = avm2_string_concat(ctx,
					avm2_string_from_literal(ctx, "xmlns:"),
					node->namespaces[i].prefix);
			}
			else
			{
				name = avm2_string_from_literal(ctx, "xmlns");
			}
			avm2_object_set_dynamic(ctx, attrs, name->utf8, name->len,
			                        avm2_string(node->namespaces[i].uri));
		}
		ext->attributes = attrs;
	}
	return xn;
}

static void xmldocument_parse(Avm2Context* ctx, Avm2Value this_val,
                              const Avm2String* input)
{
	// clear()
	Avm2XmlNodeExt* ext = xn_ensure_ext(ctx, this_val.u.obj);
	ext->children = avm2_array_new(ctx, 0);
	ext->attributes = NULL;
	xn_set(ctx, this_val, "parentNode", avm2_null());
	xn_set(ctx, this_val, "firstChild", avm2_null());
	xn_set(ctx, this_val, "lastChild", avm2_null());
	xn_set(ctx, this_val, "previousSibling", avm2_null());
	xn_set(ctx, this_val, "nextSibling", avm2_null());

	int ignore_white = avm2_coerce_to_boolean(
		xn_get(ctx, this_val, "ignoreWhite"));

	// "<xml>" + input + "</xml>" keeps top-level text/comments as children.
	const Avm2String* wrapped = avm2_string_concat(
		ctx, avm2_string_from_literal(ctx, "<xml>"), input);
	wrapped = avm2_string_concat(ctx, wrapped,
	                             avm2_string_from_literal(ctx, "</xml>"));
	uint32_t n = 0;
	E4XNode** nodes = avm2_e4x_parse(ctx, avm2_string(wrapped), 1, 1,
	                                 ignore_white, &n);
	for (uint32_t i = 0; i < n; i++)
	{
		if (nodes[i]->kind != E4X_ELEMENT) continue;
		for (uint32_t j = 0; j < nodes[i]->child_count; j++)
		{
			xmlnode_append(ctx, this_val,
			               xmldoc_convert(ctx, nodes[i]->children[j]));
		}
	}
	avm2_e4x_parse_free(ctx, nodes);
}

static Avm2Value n_xdoc_parse_xml(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	xmldocument_parse(ctx, act->this_val,
	                  avm2_coerce_to_string(ctx, arg_or_undef(act, 0)));
	return avm2_undefined();
}

static Avm2Value n_xdoc_create_element(Avm2Activation* act)
{
	Avm2Value args[2] = { avm2_uint_value(XN_ELEMENT), arg_or_undef(act, 0) };
	return avm2_class_construct(act->ctx, g_xmlnode_class, args, 2);
}

static Avm2Value n_xdoc_create_text_node(Avm2Activation* act)
{
	Avm2Value args[2] = { avm2_uint_value(XN_TEXT), arg_or_undef(act, 0) };
	return avm2_class_construct(act->ctx, g_xmlnode_class, args, 2);
}

void avm2_register_xml_legacy(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;

	Avm2Class* node = avm2_builtin_class(ctx, "flash.xml", "XMLNode",
	                                     b->object_class);
	g_xmlnode_class = node;
	node->native_ext_size = sizeof(Avm2XmlNodeExt);
	node->instance_init.fn = xmlnode_ctor;
	node->instance_init.debug_name = "XMLNode";
	avm2_builtin_add_getter(ctx, node, "attributes", n_xn_get_attributes);
	{
		// attributes is get/set: upgrade the entry to GETSET.
		Avm2VTable* vt = &node->ivtable;
		for (uint32_t i = 0; i < vt->count; i++)
		{
			if (vt->entries[i].key.name_len == 10
			    && memcmp(vt->entries[i].key.name, "attributes", 10) == 0)
			{
				vt->entries[i].kind = AVM2_PROP_GETSET;
				vt->entries[i].setter.fn = n_xn_set_attributes;
				vt->entries[i].setter.debug_name = "attributes";
			}
		}
	}
	avm2_builtin_add_getter(ctx, node, "childNodes", n_xn_get_child_nodes);
	avm2_builtin_add_getter(ctx, node, "localName", n_xn_get_local_name);
	avm2_builtin_add_getter(ctx, node, "prefix", n_xn_get_prefix);
	avm2_builtin_add_getter(ctx, node, "namespaceURI", n_xn_get_namespace_uri);
	avm2_builtin_add_method(ctx, node, "hasChildNodes", n_xn_has_child_nodes);
	avm2_builtin_add_method(ctx, node, "appendChild", n_xn_append_child);
	avm2_builtin_add_method(ctx, node, "insertBefore", n_xn_insert_before);
	avm2_builtin_add_method(ctx, node, "removeNode", n_xn_remove_node);
	avm2_builtin_add_method(ctx, node, "cloneNode", n_xn_clone_node);
	avm2_builtin_add_method(ctx, node, "toString", n_xn_to_string);
	avm2_builtin_add_method(ctx, node, "getNamespaceForPrefix",
	                        n_xn_get_namespace_for_prefix);
	avm2_builtin_add_method(ctx, node, "getPrefixForNamespace",
	                        n_xn_get_prefix_for_namespace);
	avm2_proto_add_function(ctx, node->prototype_obj, "toString", n_xn_to_string);

	Avm2Class* doc = avm2_builtin_class(ctx, "flash.xml", "XMLDocument", node);
	g_xmldocument_class = doc;
	doc->instance_init.fn = xmldocument_ctor;
	doc->instance_init.debug_name = "XMLDocument";
	avm2_builtin_add_method(ctx, doc, "parseXML", n_xdoc_parse_xml);
	avm2_builtin_add_method(ctx, doc, "createElement", n_xdoc_create_element);
	avm2_builtin_add_method(ctx, doc, "createTextNode", n_xdoc_create_text_node);
	// playerglobal declares `override public function toString()` ON
	// XMLDocument, so all_classes/xml/* grades
	// declaredBy="flash.xml::XMLDocument" on the factory's toString. The
	// implementation is the same one XMLNode carries, so re-point the COPIED
	// entry rather than appending a second one (avm2_vtable_append does not
	// de-duplicate, and a duplicate would emit two <method> elements).
	{
		Avm2VTable* vt = &doc->ivtable;
		for (uint32_t i = 0; i < vt->count; i++)
		{
			if (vt->entries[i].kind == AVM2_PROP_METHOD
			    && vt->entries[i].key.name_len == 8
			    && memcmp(vt->entries[i].key.name, "toString", 8) == 0)
			{
				vt->entries[i].defining_class = doc;
			}
		}
	}

	Avm2Class* nt = avm2_builtin_class(ctx, "flash.xml", "XMLNodeType",
	                                   b->object_class);
	nt->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	avm2_builtin_add_static_const(ctx, nt, "ELEMENT_NODE", avm2_uint_value(1));
	avm2_builtin_add_static_const(ctx, nt, "TEXT_NODE", avm2_uint_value(3));
	avm2_builtin_add_static_const(ctx, nt, "CDATA_NODE", avm2_uint_value(4));
	avm2_builtin_add_static_const(ctx, nt, "PROCESSING_INSTRUCTION_NODE",
	                              avm2_uint_value(7));
	avm2_builtin_add_static_const(ctx, nt, "COMMENT_NODE", avm2_uint_value(8));
	avm2_builtin_add_static_const(ctx, nt, "DOCUMENT_TYPE_NODE",
	                              avm2_uint_value(10));
	avm2_builtin_add_static_const(ctx, nt, "XML_DECLARATION", avm2_uint_value(13));
}
