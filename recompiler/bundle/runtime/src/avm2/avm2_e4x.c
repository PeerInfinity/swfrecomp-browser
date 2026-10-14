// E4X node tree, parser, serializer, and name matching — port of Ruffle
// core/src/avm2/e4x.rs (see avm2_e4x.h). Ruffle parses with quick-xml; we
// hand-write a parser for the same E4X subset: elements/attributes/text/
// CDATA/comments/PIs, the five standard entities + character refs, the
// ignoreWhitespace / ignoreComments / ignoreProcessingInstructions
// settings, and the numbered FP parse errors (1085/1088/1090/...).

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
#include <avm2/avm2_ops.h>
#include <memory/heap.h>

// Every live E4X node (the collector's registry; see E4XNode.gc_all_next).
static E4XNode* g_e4x_all_nodes = NULL;

// ---------------------------------------------------------------------------
// Small growable byte buffer
// ---------------------------------------------------------------------------

typedef struct Buf
{
	Avm2Context* ctx;
	char* p;
	uint32_t len, cap;
} Buf;

static void buf_init(Buf* b, Avm2Context* ctx)
{
	b->ctx = ctx;
	b->cap = 64;
	b->len = 0;
	b->p = avm2_alloc(ctx, b->cap);
}

static void buf_reserve(Buf* b, uint32_t extra)
{
	if (b->len + extra <= b->cap) return;
	uint32_t ncap = b->cap * 2;
	while (ncap < b->len + extra) ncap *= 2;
	char* np = avm2_alloc(b->ctx, ncap);
	memcpy(np, b->p, b->len);
	heap_free(b->ctx->app, b->p);  // scratch: the builder owns it alone
	b->p = np;
	b->cap = ncap;
}

static void buf_bytes(Buf* b, const char* s, uint32_t n)
{
	buf_reserve(b, n);
	memcpy(b->p + b->len, s, n);
	b->len += n;
}

static void buf_str(Buf* b, const Avm2String* s)
{
	buf_bytes(b, s->utf8, s->len);
}

static void buf_lit(Buf* b, const char* s)
{
	buf_bytes(b, s, (uint32_t) strlen(s));
}

static void buf_ch(Buf* b, char c)
{
	buf_reserve(b, 1);
	b->p[b->len++] = c;
}

// The string COPIES the bytes; the builder buffer is scratch and is freed
// here (every Buf ends in buf_take — it used to leak per serialization).
static const Avm2String* buf_take(Buf* b)
{
	const Avm2String* s = avm2_string_new(b->ctx, b->p, b->len);
	heap_free(b->ctx->app, b->p);
	b->p = NULL;
	b->cap = b->len = 0;
	return s;
}

static const Avm2String* str_empty(Avm2Context* ctx)
{
	return avm2_string_from_literal(ctx, "");
}

static int str_eq(const Avm2String* a, const Avm2String* b)
{
	if (a == b) return 1;
	if (a == NULL || b == NULL) return 0;
	return a->len == b->len && memcmp(a->utf8, b->utf8, a->len) == 0;
}

static int str_is_empty(const Avm2String* s)
{
	return s == NULL || s->len == 0;
}

// ---------------------------------------------------------------------------
// Node constructors + basic tree ops
// ---------------------------------------------------------------------------

E4XNode* avm2_e4x_node_new(Avm2Context* ctx, uint8_t kind, E4XNode* parent)
{
	E4XNode* n = avm2_alloc(ctx, sizeof(E4XNode));
	memset(n, 0, sizeof(*n));
	n->kind = kind;
	n->parent = parent;
	// Enroll in the all-nodes registry the E4X collector sweeps (see
	// gc_all_next); gc_mark 0 = not reached yet.
	n->gc_all_next = g_e4x_all_nodes;
	g_e4x_all_nodes = n;
	return n;
}

// --- collector ---------------------------------------------------------------
//
// Nodes used to be immortal (every node a root, every wrapper pinned), which
// retained every XML document a game ever parsed — Seedling re-parses its room
// XML on each world swap and ran out of arena after ~170 swaps. Now a node
// lives while a wrapper reaches its tree. A wrapper reaching ANY node keeps
// the whole connected tree (parent/children/attributes edges in both
// directions), matching what AS3 can still navigate to (parent(), children()).
// Marking runs inside the ordinary mark drain (avm2_xml_gc_trace_ext), so it
// may mark wrapper objects; the sweep runs once the drain is complete.

static uint32_t g_e4x_epoch = 1;
static E4XNode** g_e4x_stack = NULL;
static uint32_t g_e4x_stack_n = 0, g_e4x_stack_cap = 0;
static int g_e4x_mark_failed = 0;

void avm2_e4x_gc_begin(void)
{
	g_e4x_epoch++;
	if (g_e4x_epoch == 0) g_e4x_epoch = 1;  // 0 = never marked (node_new)
	g_e4x_mark_failed = 0;
}

static void e4x_stack_push(E4XNode* n)
{
	if (n == NULL || n->gc_mark == g_e4x_epoch) return;
	if (g_e4x_stack_n == g_e4x_stack_cap)
	{
		uint32_t nc = g_e4x_stack_cap == 0 ? 1024 : g_e4x_stack_cap * 2;
		E4XNode** grown = realloc(g_e4x_stack, nc * sizeof(E4XNode*));
		if (grown == NULL)
		{
			// Cannot finish this mark: the node sweep is skipped this cycle
			// (over-retain, never free a reachable node).
			g_e4x_mark_failed = 1;
			return;
		}
		g_e4x_stack = grown;
		g_e4x_stack_cap = nc;
	}
	g_e4x_stack[g_e4x_stack_n++] = n;
}

static void e4x_mark_ns(const E4XNamespace* ns)
{
	avm2_gc_mark_string(ns->uri);
	avm2_gc_mark_string(ns->prefix);
}

void avm2_e4x_gc_mark_node(E4XNode* root)
{
	e4x_stack_push(root);
	while (g_e4x_stack_n > 0)
	{
		E4XNode* n = g_e4x_stack[--g_e4x_stack_n];
		if (n->gc_mark == g_e4x_epoch) continue;
		n->gc_mark = g_e4x_epoch;
		avm2_gc_mark_string(n->local);
		avm2_gc_mark_string(n->text);
		if (n->has_ns) e4x_mark_ns(&n->ns);
		for (uint32_t i = 0; i < n->ns_count; i++) e4x_mark_ns(&n->namespaces[i]);
		// Object edges held only by the node: the cached wrapper (so a
		// node's identity — a.x[0] === a.x[0] — survives while it lives)
		// and the setNotification closure.
		avm2_gc_mark_object(n->obj);
		avm2_gc_mark_object(n->notify);
		e4x_stack_push(n->parent);
		for (uint32_t i = 0; i < n->child_count; i++) e4x_stack_push(n->children[i]);
		for (uint32_t i = 0; i < n->attr_count; i++) e4x_stack_push(n->attributes[i]);
	}
}

uint32_t avm2_e4x_gc_sweep_nodes(Avm2Context* ctx)
{
	if (g_e4x_mark_failed) return 0;
	uint32_t freed = 0;
	E4XNode** link = &g_e4x_all_nodes;
	for (E4XNode* n = g_e4x_all_nodes; n != NULL; )
	{
		E4XNode* next = n->gc_all_next;
		if (n->gc_mark == g_e4x_epoch)
		{
			link = &n->gc_all_next;
			n = next;
			continue;
		}
		// Unreached: no wrapper reaches its tree, and its own wrapper (n->obj)
		// was not marked either (a marked wrapper marks its node), so it dies
		// in this cycle's object sweep and never reads n again.
		*link = next;
		if (n->children != NULL) heap_free(ctx->app, n->children);
		if (n->attributes != NULL) heap_free(ctx->app, n->attributes);
		if (n->namespaces != NULL) heap_free(ctx->app, n->namespaces);
		heap_free(ctx->app, n);
		freed++;
		n = next;
	}
	return freed;
}

E4XNode* avm2_e4x_text(Avm2Context* ctx, const Avm2String* s, E4XNode* parent)
{
	E4XNode* n = avm2_e4x_node_new(ctx, E4X_TEXT, parent);
	n->text = s;
	return n;
}

E4XNode* avm2_e4x_element(Avm2Context* ctx, const E4XNamespace* ns,
                          const Avm2String* local, E4XNode* parent)
{
	E4XNode* n = avm2_e4x_node_new(ctx, E4X_ELEMENT, parent);
	if (ns != NULL)
	{
		n->has_ns = 1;
		n->ns = *ns;
	}
	n->local = local;
	return n;
}

E4XNode* avm2_e4x_attribute(Avm2Context* ctx, const E4XNamespace* ns,
                            const Avm2String* local, const Avm2String* value,
                            E4XNode* parent)
{
	E4XNode* n = avm2_e4x_node_new(ctx, E4X_ATTRIBUTE, parent);
	if (ns != NULL)
	{
		n->has_ns = 1;
		n->ns = *ns;
	}
	n->local = local;
	n->text = value;
	return n;
}

static void node_push_child(Avm2Context* ctx, E4XNode* elem, E4XNode* child)
{
	if (elem->child_count == elem->child_cap)
	{
		uint32_t ncap = elem->child_cap ? elem->child_cap * 2 : 4;
		E4XNode** np = avm2_alloc(ctx, ncap * sizeof(E4XNode*));
		memcpy(np, elem->children, elem->child_count * sizeof(E4XNode*));
		if (elem->children != NULL) heap_free(ctx->app, elem->children);  // node-owned
		elem->children = np;
		elem->child_cap = ncap;
	}
	elem->children[elem->child_count++] = child;
}

void avm2_e4x_append_attribute(Avm2Context* ctx, E4XNode* elem, E4XNode* attr)
{
	if (elem->attr_count == elem->attr_cap)
	{
		uint32_t ncap = elem->attr_cap ? elem->attr_cap * 2 : 4;
		E4XNode** np = avm2_alloc(ctx, ncap * sizeof(E4XNode*));
		memcpy(np, elem->attributes, elem->attr_count * sizeof(E4XNode*));
		if (elem->attributes != NULL) heap_free(ctx->app, elem->attributes);  // node-owned
		elem->attributes = np;
		elem->attr_cap = ncap;
	}
	elem->attributes[elem->attr_count++] = attr;
	attr->parent = elem;
}

static void node_push_ns(Avm2Context* ctx, E4XNode* elem, E4XNamespace ns)
{
	if (elem->ns_count == elem->ns_cap)
	{
		uint32_t ncap = elem->ns_cap ? elem->ns_cap * 2 : 2;
		E4XNamespace* np = avm2_alloc(ctx, ncap * sizeof(E4XNamespace));
		memcpy(np, elem->namespaces, elem->ns_count * sizeof(E4XNamespace));
		if (elem->namespaces != NULL) heap_free(ctx->app, elem->namespaces);  // node-owned
		elem->namespaces = np;
		elem->ns_cap = ncap;
	}
	elem->namespaces[elem->ns_count++] = ns;
}

void avm2_e4x_insert_at(Avm2Context* ctx, E4XNode* elem, uint32_t index, E4XNode* child)
{
	if (elem->kind != E4X_ELEMENT) return;
	child->parent = elem;
	node_push_child(ctx, elem, NULL);  // grow by one
	if (index > elem->child_count - 1) index = elem->child_count - 1;
	memmove(&elem->children[index + 1], &elem->children[index],
	        (elem->child_count - 1 - index) * sizeof(E4XNode*));
	elem->children[index] = child;
}

void avm2_e4x_delete_by_index(E4XNode* elem, uint32_t index)
{
	if (elem->kind != E4X_ELEMENT || index >= elem->child_count) return;
	elem->children[index]->parent = NULL;
	memmove(&elem->children[index], &elem->children[index + 1],
	        (elem->child_count - index - 1) * sizeof(E4XNode*));
	elem->child_count--;
}

void avm2_e4x_remove_all_children(E4XNode* elem)
{
	if (elem->kind != E4X_ELEMENT) return;
	for (uint32_t i = 0; i < elem->child_count; i++)
	{
		elem->children[i]->parent = NULL;
	}
	elem->child_count = 0;
}

void avm2_e4x_remove_child(E4XNode* elem, E4XNode* child)
{
	if (elem->kind != E4X_ELEMENT) return;
	uint32_t w = 0;
	for (uint32_t i = 0; i < elem->child_count; i++)
	{
		if (elem->children[i] != child) elem->children[w++] = elem->children[i];
	}
	elem->child_count = w;
}

void avm2_e4x_remove_attribute(E4XNode* elem, E4XNode* attr)
{
	if (elem->kind != E4X_ELEMENT) return;
	uint32_t w = 0;
	for (uint32_t i = 0; i < elem->attr_count; i++)
	{
		if (elem->attributes[i] != attr) elem->attributes[w++] = elem->attributes[i];
	}
	elem->attr_count = w;
}

int avm2_e4x_child_index(const E4XNode* node)
{
	if (node->kind == E4X_ATTRIBUTE || node->parent == NULL) return -1;
	const E4XNode* p = node->parent;
	for (uint32_t i = 0; i < p->child_count; i++)
	{
		if (p->children[i] == node) return (int) i;
	}
	return -1;
}

int avm2_e4x_is_ancestor(const E4XNode* maybe_ancestor, const E4XNode* node)
{
	for (const E4XNode* p = node; p != NULL; p = p->parent)
	{
		if (p == maybe_ancestor) return 1;
	}
	return 0;
}

E4XNode* avm2_e4x_deep_copy(Avm2Context* ctx, const E4XNode* node)
{
	E4XNode* n = avm2_e4x_node_new(ctx, node->kind, NULL);
	n->has_ns = node->has_ns;
	n->ns = node->ns;
	n->local = node->local;
	n->text = node->text;
	if (node->kind == E4X_ELEMENT)
	{
		for (uint32_t i = 0; i < node->attr_count; i++)
		{
			E4XNode* a = avm2_e4x_deep_copy(ctx, node->attributes[i]);
			avm2_e4x_append_attribute(ctx, n, a);
		}
		for (uint32_t i = 0; i < node->child_count; i++)
		{
			E4XNode* c = avm2_e4x_deep_copy(ctx, node->children[i]);
			c->parent = n;
			node_push_child(ctx, n, c);
		}
		for (uint32_t i = 0; i < node->ns_count; i++)
		{
			node_push_ns(ctx, n, node->namespaces[i]);
		}
	}
	return n;
}

// ---------------------------------------------------------------------------
// Content predicates / equality
// ---------------------------------------------------------------------------

int avm2_e4x_has_simple_content(const E4XNode* node)
{
	switch (node->kind)
	{
		case E4X_ELEMENT:
			for (uint32_t i = 0; i < node->child_count; i++)
			{
				if (node->children[i]->kind == E4X_ELEMENT) return 0;
			}
			return 1;
		case E4X_TEXT:
		case E4X_CDATA:
		case E4X_ATTRIBUTE:
			return 1;
		default:  // comment / PI
			return 0;
	}
}

int avm2_e4x_has_complex_content(const E4XNode* node)
{
	if (node->kind != E4X_ELEMENT) return 0;
	for (uint32_t i = 0; i < node->child_count; i++)
	{
		if (node->children[i]->kind == E4X_ELEMENT) return 1;
	}
	return 0;
}

static int node_is_text(const E4XNode* n)
{
	return n->kind == E4X_TEXT || n->kind == E4X_CDATA;
}

// Namespace URI for equality (None → NULL).
static const Avm2String* node_ns_uri(const E4XNode* n)
{
	return n->has_ns ? n->ns.uri : NULL;
}

int avm2_e4x_equals(const E4XNode* a, const E4XNode* b)
{
	if (a == b) return 1;
	if (!str_eq(a->local, b->local)) return 0;
	// URI only; prefixes are ignored. None != Some.
	{
		const Avm2String* ua = node_ns_uri(a);
		const Avm2String* ub = node_ns_uri(b);
		if ((ua == NULL) != (ub == NULL)) return 0;
		if (ua != NULL && !str_eq(ua, ub)) return 0;
	}
	if (node_is_text(a) && node_is_text(b))
	{
		return str_eq(a->text, b->text);
	}
	if (a->kind != b->kind) return 0;
	switch (a->kind)
	{
		case E4X_COMMENT:
		case E4X_PI:
		case E4X_ATTRIBUTE:
			return str_eq(a->text, b->text);
		case E4X_ELEMENT:
		{
			if (a->child_count != b->child_count) return 0;
			if (a->attr_count != b->attr_count) return 0;
			// Attributes order-independently.
			for (uint32_t i = 0; i < a->attr_count; i++)
			{
				int found = 0;
				for (uint32_t j = 0; j < b->attr_count && !found; j++)
				{
					found = avm2_e4x_equals(a->attributes[i], b->attributes[j]);
				}
				if (!found) return 0;
			}
			// Children in order.
			for (uint32_t i = 0; i < a->child_count; i++)
			{
				if (!avm2_e4x_equals(a->children[i], b->children[i])) return 0;
			}
			return 1;
		}
		default:
			return 0;
	}
}

// ---------------------------------------------------------------------------
// XML 1.0 name characters (isXMLName, XML child assignment, QName/Namespace)
// ---------------------------------------------------------------------------

// The XML 1.0 4th ed. BaseChar|Ideographic ("Letter"), Digit, CombiningChar
// and Extender productions
// (https://www.w3.org/TR/2004/REC-xml-20040204/#NT-Letter), as avmplus
// AvmCore.cpp:3478 and Ruffle core/src/avm2/e4x/is_xml_name.rs. Sorted,
// non-overlapping, and all <= 0xFFFF, so uint16_t pairs suffice (1304 bytes).
static const uint16_t E4X_LETTER[][2] =
{
	{0x0041, 0x005A}, {0x0061, 0x007A}, {0x00C0, 0x00D6}, {0x00D8, 0x00F6},
	{0x00F8, 0x00FF}, {0x0100, 0x0131}, {0x0134, 0x013E}, {0x0141, 0x0148},
	{0x014A, 0x017E}, {0x0180, 0x01C3}, {0x01CD, 0x01F0}, {0x01F4, 0x01F5},
	{0x01FA, 0x0217}, {0x0250, 0x02A8}, {0x02BB, 0x02C1}, {0x0386, 0x0386},
	{0x0388, 0x038A}, {0x038C, 0x038C}, {0x038E, 0x03A1}, {0x03A3, 0x03CE},
	{0x03D0, 0x03D6}, {0x03DA, 0x03DA}, {0x03DC, 0x03DC}, {0x03DE, 0x03DE},
	{0x03E0, 0x03E0}, {0x03E2, 0x03F3}, {0x0401, 0x040C}, {0x040E, 0x044F},
	{0x0451, 0x045C}, {0x045E, 0x0481}, {0x0490, 0x04C4}, {0x04C7, 0x04C8},
	{0x04CB, 0x04CC}, {0x04D0, 0x04EB}, {0x04EE, 0x04F5}, {0x04F8, 0x04F9},
	{0x0531, 0x0556}, {0x0559, 0x0559}, {0x0561, 0x0586}, {0x05D0, 0x05EA},
	{0x05F0, 0x05F2}, {0x0621, 0x063A}, {0x0641, 0x064A}, {0x0671, 0x06B7},
	{0x06BA, 0x06BE}, {0x06C0, 0x06CE}, {0x06D0, 0x06D3}, {0x06D5, 0x06D5},
	{0x06E5, 0x06E6}, {0x0905, 0x0939}, {0x093D, 0x093D}, {0x0958, 0x0961},
	{0x0985, 0x098C}, {0x098F, 0x0990}, {0x0993, 0x09A8}, {0x09AA, 0x09B0},
	{0x09B2, 0x09B2}, {0x09B6, 0x09B9}, {0x09DC, 0x09DD}, {0x09DF, 0x09E1},
	{0x09F0, 0x09F1}, {0x0A05, 0x0A0A}, {0x0A0F, 0x0A10}, {0x0A13, 0x0A28},
	{0x0A2A, 0x0A30}, {0x0A32, 0x0A33}, {0x0A35, 0x0A36}, {0x0A38, 0x0A39},
	{0x0A59, 0x0A5C}, {0x0A5E, 0x0A5E}, {0x0A72, 0x0A74}, {0x0A85, 0x0A8B},
	{0x0A8D, 0x0A8D}, {0x0A8F, 0x0A91}, {0x0A93, 0x0AA8}, {0x0AAA, 0x0AB0},
	{0x0AB2, 0x0AB3}, {0x0AB5, 0x0AB9}, {0x0ABD, 0x0ABD}, {0x0AE0, 0x0AE0},
	{0x0B05, 0x0B0C}, {0x0B0F, 0x0B10}, {0x0B13, 0x0B28}, {0x0B2A, 0x0B30},
	{0x0B32, 0x0B33}, {0x0B36, 0x0B39}, {0x0B3D, 0x0B3D}, {0x0B5C, 0x0B5D},
	{0x0B5F, 0x0B61}, {0x0B85, 0x0B8A}, {0x0B8E, 0x0B90}, {0x0B92, 0x0B95},
	{0x0B99, 0x0B9A}, {0x0B9C, 0x0B9C}, {0x0B9E, 0x0B9F}, {0x0BA3, 0x0BA4},
	{0x0BA8, 0x0BAA}, {0x0BAE, 0x0BB5}, {0x0BB7, 0x0BB9}, {0x0C05, 0x0C0C},
	{0x0C0E, 0x0C10}, {0x0C12, 0x0C28}, {0x0C2A, 0x0C33}, {0x0C35, 0x0C39},
	{0x0C60, 0x0C61}, {0x0C85, 0x0C8C}, {0x0C8E, 0x0C90}, {0x0C92, 0x0CA8},
	{0x0CAA, 0x0CB3}, {0x0CB5, 0x0CB9}, {0x0CDE, 0x0CDE}, {0x0CE0, 0x0CE1},
	{0x0D05, 0x0D0C}, {0x0D0E, 0x0D10}, {0x0D12, 0x0D28}, {0x0D2A, 0x0D39},
	{0x0D60, 0x0D61}, {0x0E01, 0x0E2E}, {0x0E30, 0x0E30}, {0x0E32, 0x0E33},
	{0x0E40, 0x0E45}, {0x0E81, 0x0E82}, {0x0E84, 0x0E84}, {0x0E87, 0x0E88},
	{0x0E8A, 0x0E8A}, {0x0E8D, 0x0E8D}, {0x0E94, 0x0E97}, {0x0E99, 0x0E9F},
	{0x0EA1, 0x0EA3}, {0x0EA5, 0x0EA5}, {0x0EA7, 0x0EA7}, {0x0EAA, 0x0EAB},
	{0x0EAD, 0x0EAE}, {0x0EB0, 0x0EB0}, {0x0EB2, 0x0EB3}, {0x0EBD, 0x0EBD},
	{0x0EC0, 0x0EC4}, {0x0F40, 0x0F47}, {0x0F49, 0x0F69}, {0x10A0, 0x10C5},
	{0x10D0, 0x10F6}, {0x1100, 0x1100}, {0x1102, 0x1103}, {0x1105, 0x1107},
	{0x1109, 0x1109}, {0x110B, 0x110C}, {0x110E, 0x1112}, {0x113C, 0x113C},
	{0x113E, 0x113E}, {0x1140, 0x1140}, {0x114C, 0x114C}, {0x114E, 0x114E},
	{0x1150, 0x1150}, {0x1154, 0x1155}, {0x1159, 0x1159}, {0x115F, 0x1161},
	{0x1163, 0x1163}, {0x1165, 0x1165}, {0x1167, 0x1167}, {0x1169, 0x1169},
	{0x116D, 0x116E}, {0x1172, 0x1173}, {0x1175, 0x1175}, {0x119E, 0x119E},
	{0x11A8, 0x11A8}, {0x11AB, 0x11AB}, {0x11AE, 0x11AF}, {0x11B7, 0x11B8},
	{0x11BA, 0x11BA}, {0x11BC, 0x11C2}, {0x11EB, 0x11EB}, {0x11F0, 0x11F0},
	{0x11F9, 0x11F9}, {0x1E00, 0x1E9B}, {0x1EA0, 0x1EF9}, {0x1F00, 0x1F15},
	{0x1F18, 0x1F1D}, {0x1F20, 0x1F45}, {0x1F48, 0x1F4D}, {0x1F50, 0x1F57},
	{0x1F59, 0x1F59}, {0x1F5B, 0x1F5B}, {0x1F5D, 0x1F5D}, {0x1F5F, 0x1F7D},
	{0x1F80, 0x1FB4}, {0x1FB6, 0x1FBC}, {0x1FBE, 0x1FBE}, {0x1FC2, 0x1FC4},
	{0x1FC6, 0x1FCC}, {0x1FD0, 0x1FD3}, {0x1FD6, 0x1FDB}, {0x1FE0, 0x1FEC},
	{0x1FF2, 0x1FF4}, {0x1FF6, 0x1FFC}, {0x2126, 0x2126}, {0x212A, 0x212B},
	{0x212E, 0x212E}, {0x2180, 0x2182}, {0x3007, 0x3007}, {0x3021, 0x3029},
	{0x3041, 0x3094}, {0x30A1, 0x30FA}, {0x3105, 0x312C}, {0x4E00, 0x9FA5},
	{0xAC00, 0xD7A3}
};

static const uint16_t E4X_DIGIT[][2] =
{
	{0x0030, 0x0039}, {0x0660, 0x0669}, {0x06F0, 0x06F9}, {0x0966, 0x096F},
	{0x09E6, 0x09EF}, {0x0A66, 0x0A6F}, {0x0AE6, 0x0AEF}, {0x0B66, 0x0B6F},
	{0x0BE7, 0x0BEF}, {0x0C66, 0x0C6F}, {0x0CE6, 0x0CEF}, {0x0D66, 0x0D6F},
	{0x0E50, 0x0E59}, {0x0ED0, 0x0ED9}, {0x0F20, 0x0F29}
};

static const uint16_t E4X_COMBINING[][2] =
{
	{0x0300, 0x0345}, {0x0360, 0x0361}, {0x0483, 0x0486}, {0x0591, 0x05A1},
	{0x05A3, 0x05B9}, {0x05BB, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2},
	{0x05C4, 0x05C4}, {0x064B, 0x0652}, {0x0670, 0x0670}, {0x06D6, 0x06DC},
	{0x06DD, 0x06DF}, {0x06E0, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED},
	{0x0901, 0x0903}, {0x093C, 0x093C}, {0x093E, 0x094C}, {0x094D, 0x094D},
	{0x0951, 0x0954}, {0x0962, 0x0963}, {0x0981, 0x0983}, {0x09BC, 0x09BC},
	{0x09BE, 0x09BE}, {0x09BF, 0x09BF}, {0x09C0, 0x09C4}, {0x09C7, 0x09C8},
	{0x09CB, 0x09CD}, {0x09D7, 0x09D7}, {0x09E2, 0x09E3}, {0x0A02, 0x0A02},
	{0x0A3C, 0x0A3C}, {0x0A3E, 0x0A3E}, {0x0A3F, 0x0A3F}, {0x0A40, 0x0A42},
	{0x0A47, 0x0A48}, {0x0A4B, 0x0A4D}, {0x0A70, 0x0A71}, {0x0A81, 0x0A83},
	{0x0ABC, 0x0ABC}, {0x0ABE, 0x0AC5}, {0x0AC7, 0x0AC9}, {0x0ACB, 0x0ACD},
	{0x0B01, 0x0B03}, {0x0B3C, 0x0B3C}, {0x0B3E, 0x0B43}, {0x0B47, 0x0B48},
	{0x0B4B, 0x0B4D}, {0x0B56, 0x0B57}, {0x0B82, 0x0B83}, {0x0BBE, 0x0BC2},
	{0x0BC6, 0x0BC8}, {0x0BCA, 0x0BCD}, {0x0BD7, 0x0BD7}, {0x0C01, 0x0C03},
	{0x0C3E, 0x0C44}, {0x0C46, 0x0C48}, {0x0C4A, 0x0C4D}, {0x0C55, 0x0C56},
	{0x0C82, 0x0C83}, {0x0CBE, 0x0CC4}, {0x0CC6, 0x0CC8}, {0x0CCA, 0x0CCD},
	{0x0CD5, 0x0CD6}, {0x0D02, 0x0D03}, {0x0D3E, 0x0D43}, {0x0D46, 0x0D48},
	{0x0D4A, 0x0D4D}, {0x0D57, 0x0D57}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A},
	{0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EB9}, {0x0EBB, 0x0EBC},
	{0x0EC8, 0x0ECD}, {0x0F18, 0x0F19}, {0x0F35, 0x0F35}, {0x0F37, 0x0F37},
	{0x0F39, 0x0F39}, {0x0F3E, 0x0F3E}, {0x0F3F, 0x0F3F}, {0x0F71, 0x0F84},
	{0x0F86, 0x0F8B}, {0x0F90, 0x0F95}, {0x0F97, 0x0F97}, {0x0F99, 0x0FAD},
	{0x0FB1, 0x0FB7}, {0x0FB9, 0x0FB9}, {0x20D0, 0x20DC}, {0x20E1, 0x20E1},
	{0x302A, 0x302F}, {0x3099, 0x3099}, {0x309A, 0x309A}
};

static const uint16_t E4X_EXTENDER[][2] =
{
	{0x00B7, 0x00B7}, {0x02D0, 0x02D0}, {0x02D1, 0x02D1}, {0x0387, 0x0387},
	{0x0640, 0x0640}, {0x0E46, 0x0E46}, {0x0EC6, 0x0EC6}, {0x3005, 0x3005},
	{0x3031, 0x3035}, {0x309D, 0x309E}, {0x30FC, 0x30FE}
};

// Binary search over a sorted, non-overlapping range table.
static int in_ranges(const uint16_t (*t)[2], size_t n, uint32_t c)
{
	if (c > 0xFFFF) return 0;   // no table reaches past the BMP
	size_t lo = 0, hi = n;
	while (lo < hi)
	{
		size_t mid = lo + (hi - lo) / 2;
		if (c < t[mid][0])      hi = mid;
		else if (c > t[mid][1]) lo = mid + 1;
		else                    return 1;
	}
	return 0;
}

#define E4X_IN(tbl, c) in_ranges((tbl), sizeof(tbl) / sizeof((tbl)[0]), (c))

// Decode one WTF-8 code point starting at *i; 0 on a malformed sequence.
// Lone surrogates are surfaced as 0xD800..0xDFFF rather than folded to
// U+FFFD -- String.fromCharCode stores them as 3-byte WTF-8 (avm2_string.c),
// and no table contains them, so they correctly answer "not a name
// character", matching Ruffle's chars() Err arm.
static int e4x_next_cp(const unsigned char* b, uint32_t len, uint32_t* i,
                       uint32_t* cp)
{
	uint32_t p = *i;
	if (p >= len) return 0;
	unsigned char c = b[p];
	uint32_t n, v;
	if (c < 0x80)                { *cp = c; *i = p + 1; return 1; }
	else if ((c & 0xE0) == 0xC0) { n = 1; v = (uint32_t) (c & 0x1F); }
	else if ((c & 0xF0) == 0xE0) { n = 2; v = (uint32_t) (c & 0x0F); }
	else if ((c & 0xF8) == 0xF0) { n = 3; v = (uint32_t) (c & 0x07); }
	else return 0;
	if (p + n >= len) return 0;
	for (uint32_t k = 1; k <= n; k++)
	{
		unsigned char cc = b[p + k];
		if ((cc & 0xC0) != 0x80) return 0;
		v = (v << 6) | (uint32_t) (cc & 0x3F);
	}
	*cp = v;
	*i = p + n + 1;
	return 1;
}

// avmplus isXMLName (AvmCore.cpp:3478) / Ruffle is_xml_name(): the first
// character must be a Letter or '_' (note ':' is NOT allowed); the rest may
// additionally be a Digit, CombiningChar, Extender, '.' or '-'.
//
// The ASCII arm is spelled out as a fast path. It is exactly equivalent to the
// tables below 0x80 -- Letter contributes only A-Z/a-z, Digit only 0-9, and
// neither CombiningChar (first range 0x0300) nor Extender (first 0x00B7)
// reaches under 0x80 -- and it keeps the E4X child-assignment hot path
// (avm2_xml.c) at its previous per-character cost.
int avm2_e4x_is_xml_name(const Avm2String* s)
{
	if (s == NULL || s->len == 0) return 0;
	const unsigned char* b = (const unsigned char*) s->utf8;
	uint32_t i = 0;
	uint32_t cp;

	if (!e4x_next_cp(b, s->len, &i, &cp)) return 0;
	if (cp < 0x80)
	{
		if (!((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_'))
		{
			return 0;
		}
	}
	else if (!E4X_IN(E4X_LETTER, cp))
	{
		return 0;
	}

	while (i < s->len)
	{
		if (!e4x_next_cp(b, s->len, &i, &cp)) return 0;
		if (cp < 0x80)
		{
			if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')
			    || (cp >= '0' && cp <= '9') || cp == '.' || cp == '-'
			    || cp == '_')
			{
				continue;
			}
			return 0;
		}
		if (E4X_IN(E4X_LETTER, cp) || E4X_IN(E4X_DIGIT, cp)
		    || E4X_IN(E4X_COMBINING, cp) || E4X_IN(E4X_EXTENDER, cp))
		{
			continue;
		}
		return 0;
	}
	return 1;
}

// ---------------------------------------------------------------------------
// Query names (handle_input_multiname / string_to_multiname / matches_name)
// ---------------------------------------------------------------------------

// Is a raw ABC namespace kind the folded Namespace/Package ("public-like")
// kind? (Ruffle folds 0x08 and 0x16 into NamespaceData::Namespace.)
static int ns_kind_is_real(uint8_t kind)
{
	return kind == 0x08 || kind == 0x16;
}

static E4XName name_any(void)
{
	E4XName n;
	memset(&n, 0, sizeof(n));
	n.any_ns = 1;  // Multiname::any(): any name, any namespace
	return n;
}

static E4XName name_public(Avm2Context* ctx, const Avm2String* local, int attr)
{
	E4XName n;
	memset(&n, 0, sizeof(n));
	n.local = local;
	n.is_attribute = (uint8_t) attr;
	// Public or the default XML namespace (handle_input_multiname).
	const Avm2String* dxns = avm2_dxns_uri(ctx);
	n.single_uri = (dxns != NULL) ? dxns : str_empty(ctx);
	n.single_is_real = 1;
	return n;
}

E4XName avm2_e4x_name_from_string(Avm2Context* ctx, const Avm2String* s,
                                  int force_attribute)
{
	E4XName n;
	if (s->len >= 1 && s->utf8[0] == '@')
	{
		if (s->len == 2 && s->utf8[1] == '*')
		{
			// @* — any attribute.
			memset(&n, 0, sizeof(n));
			n.is_attribute = 1;
			n.any_ns = 1;
			return n;
		}
		const Avm2String* local = avm2_string_new(ctx, s->utf8 + 1, s->len - 1);
		return name_public(ctx, local, 1);
	}
	if (s->len == 1 && s->utf8[0] == '*')
	{
		n = name_any();
		n.is_attribute = (uint8_t) force_attribute;
		return n;
	}
	n = name_public(ctx, s, force_attribute);
	return n;
}

E4XName avm2_e4x_name_from_value(Avm2Context* ctx, Avm2Value v, int force_attribute)
{
	if (v.kind == AVM2_VALUE_UNDEFINED || v.kind == AVM2_VALUE_NULL)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1010: A term is undefined and has no properties.");
	}
	const Avm2QNameExt* q = avm2_qname_ext_of(v);
	if (q != NULL)
	{
		E4XName n;
		memset(&n, 0, sizeof(n));
		n.local = q->local;  // NULL = any name
		n.is_attribute = (uint8_t) (q->is_attribute || force_attribute);
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
	const Avm2String* s = avm2_coerce_to_string(ctx, v);
	return avm2_e4x_name_from_string(ctx, s, force_attribute);
}

// From a static multiname, applying handle_input_multiname: names without
// an explicit namespace rebuild onto {public} or {dxns}; attribute names
// and any-forms keep the compiler's set.
E4XName avm2_e4x_name_from_mn(Avm2Context* ctx, const Avm2AbcFileData* data,
                              uint32_t mn_idx)
{
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
	E4XName n;
	memset(&n, 0, sizeof(n));

	switch (mn->kind)
	{
		case 0x0d: case 0x10: case 0x12: case 0x0e: case 0x1c:  // *A kinds
			n.is_attribute = 1;
			break;
		default:
			break;
	}
	n.local = (mn->name != 0) ? &data->strings[mn->name] : NULL;

	int explicit_ns = 0;
	switch (mn->kind)
	{
		case 0x07: case 0x0d:  // QName / QNameA
		{
			n.is_qname = 1;
			if (mn->ns == 0)
			{
				n.any_ns = 1;
			}
			else
			{
				const Avm2AbcNamespace* ns = &data->namespaces[mn->ns];
				n.single_uri = &data->strings[ns->name];
				n.single_is_real = (uint8_t) ns_kind_is_real(ns->kind);
				explicit_ns = ns_kind_is_real(ns->kind) && n.single_uri->len > 0;
			}
			break;
		}
		case 0x09: case 0x0e: case 0x1b: case 0x1c:  // Multiname(L)(A)
		{
			const Avm2AbcNsSet* set = &data->ns_sets[mn->ns_set];
			if (set->count == 1)
			{
				const Avm2AbcNamespace* ns = &data->namespaces[set->ns_indices[0]];
				if (set->ns_indices[0] == 0)
				{
					n.any_ns = 1;
					break;
				}
				n.single_uri = &data->strings[ns->name];
				n.single_is_real = (uint8_t) ns_kind_is_real(ns->kind);
				explicit_ns = ns_kind_is_real(ns->kind) && n.single_uri->len > 0;
			}
			else
			{
				n.data = data;
				n.ns_set = mn->ns_set;
			}
			break;
		}
		default:
			// RTQName kinds are handled by the caller (runtime ns).
			n.any_ns = 1;
			break;
	}

	// handle_input_multiname rebuild: dxns replaces the set exclusively;
	// otherwise the ORIGINAL set is kept and public added (use-namespace
	// names must still match their opened namespaces —
	// xml_explicit_use_namespace).
	if (!explicit_ns && !n.is_attribute && n.local != NULL && !n.any_ns)
	{
		E4XName r = avm2_e4x_name_from_string(ctx, n.local, 0);
		if (avm2_dxns_uri(ctx) == NULL && n.data != NULL)
		{
			r.single_uri = NULL;
			r.single_is_real = 0;
			r.data = n.data;
			r.ns_set = n.ns_set;
			r.also_public = 1;
		}
		return r;
	}
	return n;
}

int avm2_e4x_name_is_public_like(const E4XName* n)
{
	if (n->any_ns) return 1;
	if (n->also_public) return 1;
	if (n->single_uri != NULL) return n->single_uri->len == 0;
	if (n->data != NULL)
	{
		const Avm2AbcNsSet* set = &n->data->ns_sets[n->ns_set];
		for (uint32_t i = 0; i < set->count; i++)
		{
			const Avm2AbcNamespace* ns = &n->data->namespaces[set->ns_indices[i]];
			if (ns_kind_is_real(ns->kind) && n->data->strings[ns->name].len == 0)
			{
				return 1;
			}
		}
	}
	return 0;
}

// E4XNode::matches_name (see avm2_e4x.h header notes for the step list).
int avm2_e4x_matches_name(const E4XNode* node, const E4XName* name)
{
	int node_is_attr = (node->kind == E4X_ATTRIBUTE);
	if (node_is_attr != (name->is_attribute != 0)) return 0;

	int any_name = (name->local == NULL);
	if (any_name && !name->is_qname) return 1;
	if (!any_name && !str_eq(node->local, name->local)) return 0;
	if (node->local == NULL) return 0;
	if (name->any_ns) return 1;

	const Avm2String* self_uri = node_ns_uri(node);
	if (name->single_uri != NULL)
	{
		if (!name->single_is_real)
		{
			return self_uri == NULL || self_uri->len == 0;
		}
		if (self_uri == NULL) return name->single_uri->len == 0;
		return str_eq(name->single_uri, self_uri);
	}
	if (name->data != NULL)
	{
		const Avm2AbcNsSet* set = &name->data->ns_sets[name->ns_set];
		int has_real = (name->also_public != 0);
		for (uint32_t i = 0; i < set->count && !has_real; i++)
		{
			if (set->ns_indices[i] != 0
			    && ns_kind_is_real(name->data->namespaces[set->ns_indices[i]].kind))
			{
				has_real = 1;
			}
		}
		if (!has_real)
		{
			return self_uri == NULL || self_uri->len == 0;
		}
		if (name->also_public && (self_uri == NULL || self_uri->len == 0))
		{
			return 1;
		}
		for (uint32_t i = 0; i < set->count; i++)
		{
			if (set->ns_indices[i] == 0) continue;
			const Avm2String* uri =
				&name->data->strings[name->data->namespaces[set->ns_indices[i]].name];
			if (self_uri != NULL ? str_eq(uri, self_uri) : uri->len == 0)
			{
				return 1;
			}
		}
		return 0;
	}
	// Empty namespace set: match unnamespaced nodes.
	return self_uri == NULL || self_uri->len == 0;
}

// Removes ALL matching nodes from children/attributes; reports the first.
int avm2_e4x_remove_matching(Avm2Context* ctx, E4XNode* elem, const E4XName* name,
                             int attributes, uint32_t* out_index, E4XNode** out_node)
{
	(void) ctx;
	if (elem->kind != E4X_ELEMENT) return 0;
	E4XNode** arr = attributes ? elem->attributes : elem->children;
	uint32_t n = attributes ? elem->attr_count : elem->child_count;
	int any_name = (name->local == NULL && !name->is_qname);
	int found = 0;
	uint32_t w = 0;
	for (uint32_t i = 0; i < n; i++)
	{
		E4XNode* node = arr[i];
		int matches = any_name || avm2_e4x_matches_name(node, name);
		if (matches)
		{
			if (!found)
			{
				found = 1;
				*out_index = i;
				*out_node = node;
			}
			node->parent = NULL;
		}
		else
		{
			arr[w++] = node;
		}
	}
	if (attributes) elem->attr_count = w;
	else elem->child_count = w;
	return found;
}

// ---------------------------------------------------------------------------
// Namespace machinery
// ---------------------------------------------------------------------------

uint32_t avm2_e4x_in_scope_namespaces(Avm2Context* ctx, const E4XNode* node,
                                      E4XNamespace** out)
{
	uint32_t cap = 8, count = 0;
	E4XNamespace* list = avm2_alloc(ctx, cap * sizeof(E4XNamespace));
	for (const E4XNode* p = node; p != NULL; p = p->parent)
	{
		if (p->kind != E4X_ELEMENT) continue;
		for (uint32_t i = 0; i < p->ns_count; i++)
		{
			const E4XNamespace* cand = &p->namespaces[i];
			int present = 0;
			for (uint32_t j = 0; j < count && !present; j++)
			{
				if (cand->prefix != NULL)
				{
					present = list[j].prefix != NULL
					          && str_eq(list[j].prefix, cand->prefix);
				}
				else
				{
					present = str_eq(list[j].uri, cand->uri);
				}
			}
			if (present) continue;
			if (count == cap)
			{
				E4XNamespace* nl = avm2_alloc(ctx, cap * 2 * sizeof(E4XNamespace));
				memcpy(nl, list, count * sizeof(E4XNamespace));
				heap_free(ctx->app, list);
				list = nl;
				cap *= 2;
			}
			list[count++] = *cand;
		}
	}
	*out = list;
	return count;
}

E4XNamespace avm2_e4x_get_namespace(Avm2Context* ctx, const E4XNode* node,
                                    const E4XNamespace* in_scope, uint32_t n)
{
	E4XNamespace res;
	if (!node->has_ns)
	{
		res.uri = str_empty(ctx);
		res.prefix = NULL;
		return res;
	}
	for (uint32_t i = 0; i < n; i++)
	{
		if (str_eq(in_scope[i].uri, node->ns.uri)) return in_scope[i];
	}
	res.uri = node->ns.uri;
	res.prefix = NULL;
	return res;
}

void avm2_e4x_add_in_scope_namespace(Avm2Context* ctx, E4XNode* elem,
                                     const E4XNamespace* ns)
{
	if (elem->kind != E4X_ELEMENT || ns->prefix == NULL) return;
	if (ns->prefix->len == 0 && (!elem->has_ns || str_is_empty(elem->ns.uri)))
	{
		return;
	}
	// Replace an existing declaration with the same prefix (different uri).
	uint32_t w = 0;
	for (uint32_t i = 0; i < elem->ns_count; i++)
	{
		E4XNamespace* e = &elem->namespaces[i];
		if (e->prefix != NULL && str_eq(e->prefix, ns->prefix)
		    && !str_eq(e->uri, ns->uri))
		{
			continue;
		}
		elem->namespaces[w++] = *e;
	}
	elem->ns_count = w;
	node_push_ns(ctx, elem, *ns);
	// Strip a now-shadowed prefix from the element/attribute names (the
	// node keeps its OWN uri — only the prefix is dropped; ECMA 9.1.1.13
	// steps 2.f/2.g, Ruffle set_namespace(new_uri(self_ns.uri))).
	if (elem->has_ns && elem->ns.prefix != NULL && str_eq(elem->ns.prefix, ns->prefix))
	{
		elem->ns.prefix = NULL;
	}
	for (uint32_t i = 0; i < elem->attr_count; i++)
	{
		E4XNode* a = elem->attributes[i];
		if (a->has_ns && a->ns.prefix != NULL && str_eq(a->ns.prefix, ns->prefix))
		{
			a->ns.prefix = NULL;
		}
	}
}

// ---------------------------------------------------------------------------
// Mutation with values (replace / insert — ECMA-357 9.1.1.11/9.1.1.12)
// ---------------------------------------------------------------------------

static void throw_1118(Avm2Context* ctx)
{
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1118: Illegal cyclical loop between nodes.");
}

void avm2_e4x_replace(Avm2Context* ctx, E4XNode* elem, uint32_t index, Avm2Value v)
{
	if (elem->kind != E4X_ELEMENT) return;
	Avm2XmlExt* xe = avm2_xml_ext_of(v);
	Avm2XmlListExt* le = avm2_xmllist_ext_of(v);
	if (xe != NULL && xe->node->kind != E4X_ATTRIBUTE)
	{
		E4XNode* node = xe->node;
		if (node->kind == E4X_ELEMENT && avm2_e4x_is_ancestor(node, elem))
		{
			throw_1118(ctx);
		}
		node->parent = elem;
		if (index >= elem->child_count)
		{
			node_push_child(ctx, elem, node);
		}
		else
		{
			elem->children[index]->parent = NULL;
			elem->children[index] = node;
		}
		return;
	}
	if (le != NULL)
	{
		avm2_e4x_delete_by_index(elem, index);
		avm2_e4x_insert_value(ctx, elem, index, v);
		return;
	}
	const Avm2String* s = avm2_coerce_to_string(ctx, v);
	E4XNode* text = avm2_e4x_text(ctx, s, elem);
	if (index >= elem->child_count)
	{
		node_push_child(ctx, elem, text);
	}
	else
	{
		elem->children[index]->parent = NULL;
		elem->children[index] = text;
	}
}

void avm2_e4x_insert_value(Avm2Context* ctx, E4XNode* elem, uint32_t index, Avm2Value v)
{
	if (elem->kind != E4X_ELEMENT) return;
	Avm2XmlExt* xe = avm2_xml_ext_of(v);
	Avm2XmlListExt* le = avm2_xmllist_ext_of(v);
	if (xe != NULL)
	{
		if (xe->node->kind == E4X_ELEMENT && avm2_e4x_is_ancestor(xe->node, elem))
		{
			throw_1118(ctx);
		}
	}
	if (le != NULL)
	{
		for (uint32_t j = 0; j < le->count; j++)
		{
			avm2_e4x_insert_at(ctx, elem, index + j, le->items[j]);
		}
		return;
	}
	// Insert a dummy placeholder, then replace it.
	E4XNode* dummy = avm2_e4x_node_new(ctx, E4X_ELEMENT, NULL);
	avm2_e4x_insert_at(ctx, elem, index, dummy);
	avm2_e4x_replace(ctx, elem, index, v);
}

static int str_all_whitespace(const Avm2String* s)
{
	for (uint32_t i = 0; i < s->len; i++)
	{
		char c = s->utf8[i];
		if (c != '\t' && c != '\n' && c != '\r' && c != ' ') return 0;
	}
	return 1;
}

void avm2_e4x_normalize(Avm2Context* ctx, E4XNode* elem)
{
	if (elem->kind != E4X_ELEMENT) return;
	uint32_t i = 0;
	while (i < elem->child_count)
	{
		E4XNode* child = elem->children[i];
		if (child->kind == E4X_ELEMENT)
		{
			avm2_e4x_normalize(ctx, child);
			i++;
		}
		else if (node_is_text(child))
		{
			while (i + 1 < elem->child_count && node_is_text(elem->children[i + 1]))
			{
				child->text = avm2_string_concat(ctx, child->text,
				                                 elem->children[i + 1]->text);
				avm2_e4x_delete_by_index(elem, i + 1);
			}
			// avmplus: whitespace-only merged text is removed entirely.
			if (str_all_whitespace(child->text))
			{
				avm2_e4x_delete_by_index(elem, i);
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
}

// ---------------------------------------------------------------------------
// Descendants (document order, pre-order)
// ---------------------------------------------------------------------------

void avm2_e4x_descendants(Avm2Context* ctx, const E4XNode* node, const E4XName* name,
                          Avm2XmlListExt* out)
{
	if (node->kind != E4X_ELEMENT) return;
	if (name->is_attribute)
	{
		for (uint32_t i = 0; i < node->attr_count; i++)
		{
			if (avm2_e4x_matches_name(node->attributes[i], name))
			{
				avm2_xmllist_push(ctx, out, node->attributes[i]);
			}
		}
	}
	for (uint32_t i = 0; i < node->child_count; i++)
	{
		E4XNode* child = node->children[i];
		if (avm2_e4x_matches_name(child, name))
		{
			avm2_xmllist_push(ctx, out, child);
		}
		avm2_e4x_descendants(ctx, child, name, out);
	}
}

// ---------------------------------------------------------------------------
// Serialization (ECMA-357 10.2.1 ToXMLString)
// ---------------------------------------------------------------------------

static void esc_element_value(Buf* b, const Avm2String* s)
{
	for (uint32_t i = 0; i < s->len; i++)
	{
		char c = s->utf8[i];
		if (c == '<') buf_lit(b, "&lt;");
		else if (c == '>') buf_lit(b, "&gt;");
		else if (c == '&') buf_lit(b, "&amp;");
		else buf_ch(b, c);
	}
}

static void esc_attribute_value(Buf* b, const Avm2String* s)
{
	for (uint32_t i = 0; i < s->len; i++)
	{
		char c = s->utf8[i];
		if (c == '"') buf_lit(b, "&quot;");
		else if (c == '<') buf_lit(b, "&lt;");
		else if (c == '&') buf_lit(b, "&amp;");
		else if (c == '\n') buf_lit(b, "&#xA;");
		else if (c == '\r') buf_lit(b, "&#xD;");
		else if (c == '\t') buf_lit(b, "&#x9;");
		else buf_ch(b, c);
	}
}

// The pretty state: indent < 0 means pretty printing is off.
typedef struct AncNs
{
	const E4XNamespace* list;
	uint32_t count;
	const struct AncNs* prev;
} AncNs;

static const E4XNamespace* anc_find_uri(const AncNs* anc, const Avm2String* uri)
{
	for (const AncNs* a = anc; a != NULL; a = a->prev)
	{
		for (uint32_t i = 0; i < a->count; i++)
		{
			if (str_eq(a->list[i].uri, uri)) return &a->list[i];
		}
	}
	return NULL;
}

// Same lookup, OUTERMOST frame first. ECMA-357 13.3.5.4 [[GetNamespace]] step
// 2 lets an implementation pick arbitrarily when several in-scope namespaces
// share the uri; avmplus picks the ANCESTOR one (e4x/XML/e13_4_4_36 row 7:
// abc->zzz.com on <a>, def->zzz.com on <b> serializes as
// <abc:b xmlns:def="http://www.zzz.com">). Only the NAME->prefix resolution
// uses this; avm2_e4x_in_scope_namespaces must stay inner-first, since that
// order is what lets an inner prefix SHADOW an outer one (row 6).
static const E4XNamespace* anc_find_uri_outer(const AncNs* anc, const Avm2String* uri)
{
	if (anc == NULL) return NULL;
	const E4XNamespace* r = anc_find_uri_outer(anc->prev, uri);
	if (r != NULL) return r;
	for (uint32_t i = 0; i < anc->count; i++)
	{
		if (str_eq(anc->list[i].uri, uri)) return &anc->list[i];
	}
	return NULL;
}

// Is `p` bound as a prefix anywhere in the chain? A NULL prefix and a
// zero-length prefix are the same thing (both print as xmlns="...").
static int anc_prefix_taken(const AncNs* anc, const char* p, uint32_t len)
{
	for (const AncNs* a = anc; a != NULL; a = a->prev)
	{
		for (uint32_t i = 0; i < a->count; i++)
		{
			const Avm2String* q = a->list[i].prefix;
			uint32_t qlen = (q == NULL) ? 0 : q->len;
			if (qlen != len) continue;
			if (len == 0 || memcmp(q->utf8, p, len) == 0) return 1;
		}
	}
	return 0;
}

// ECMA-357 10.2.1 step 11: a namespace whose prefix is UNDEFINED gets an
// "arbitrary implementation defined" prefix at serialization time, chosen so
// that it collides with nothing in (AncestorNamespaces u namespaceDeclarations).
// avmplus takes the EMPTY prefix when it is free (so the decl prints as
// xmlns="...") and "aaa" otherwise — e4x/XML/e13_4_4_36 rows 4/5 pin exactly
// that, and Rhino's own comment in the same test says it mints "ns" instead,
// i.e. this is genuinely implementation-defined and must be matched to avmplus.
// Returning NULL for the empty case (rather than an interned "") keeps
// anc_contains_exact's NULL-vs-"" distinction untouched for descendants.
// Only the FIRST mint is observable anywhere in the corpus; the aaa -> aab ->
// ... odometer is an extrapolation from that single datapoint.
static const Avm2String* mint_prefix(Avm2Context* ctx, const AncNs* here)
{
	if (!anc_prefix_taken(here, "", 0)) return NULL;
	char buf[4] = { 'a', 'a', 'a', '\0' };
	for (;;)
	{
		if (!anc_prefix_taken(here, buf, 3)) return avm2_string_new(ctx, buf, 3);
		int k = 2;
		while (k >= 0 && buf[k] == 'z') { buf[k] = 'a'; k--; }
		if (k < 0) return avm2_string_new(ctx, "aaa", 3);
		buf[k]++;
	}
}

static int anc_contains_exact(const AncNs* anc, const E4XNamespace* ns)
{
	for (const AncNs* a = anc; a != NULL; a = a->prev)
	{
		for (uint32_t i = 0; i < a->count; i++)
		{
			int peq = (a->list[i].prefix == NULL && ns->prefix == NULL)
			          || (a->list[i].prefix != NULL && ns->prefix != NULL
			              && str_eq(a->list[i].prefix, ns->prefix));
			if (peq && str_eq(a->list[i].uri, ns->uri)) return 1;
		}
	}
	return 0;
}

static void to_xml_string_inner(Avm2Context* ctx, const E4XNode* node, Buf* b,
                                const AncNs* ancestors, int indent_level, int indent_step)
{
	if (indent_level >= 0)
	{
		for (int i = 0; i < indent_level; i++) buf_ch(b, ' ');
	}
	switch (node->kind)
	{
		case E4X_TEXT:
			esc_element_value(b, node->text);
			return;
		case E4X_CDATA:
			buf_lit(b, "<![CDATA[");
			buf_str(b, node->text);
			buf_lit(b, "]]>");
			return;
		case E4X_COMMENT:
			buf_lit(b, "<!--");
			buf_str(b, node->text);
			buf_lit(b, "-->");
			return;
		case E4X_PI:
			buf_lit(b, "<?");
			buf_str(b, node->local);
			if (node->text != NULL && node->text->len > 0)
			{
				buf_ch(b, ' ');
				buf_str(b, node->text);
			}
			buf_lit(b, "?>");
			return;
		case E4X_ATTRIBUTE:
			esc_attribute_value(b, node->text);
			return;
		default:
			break;
	}

	// Element: which namespace declarations must be printed here.
	E4XNamespace decls[64];
	uint32_t decl_count = 0;
	{
		E4XNamespace* in_scope;
		uint32_t n = avm2_e4x_in_scope_namespaces(ctx, node, &in_scope);
		for (uint32_t i = 0; i < n && decl_count < 64; i++)
		{
			if (!anc_contains_exact(ancestors, &in_scope[i]))
			{
				decls[decl_count++] = in_scope[i];
			}
		}
		heap_free(ctx->app, in_scope);  // per-element scratch
	}
	AncNs here = { decls, decl_count, ancestors };
	if (node->has_ns && anc_find_uri(&here, node->ns.uri) == NULL && decl_count < 64)
	{
		E4XNamespace d = node->ns;
		if (str_is_empty(d.prefix) && d.uri->len > 0) d.prefix = mint_prefix(ctx, &here);
		decls[decl_count++] = d;
		here.count = decl_count;
	}
	for (uint32_t i = 0; i < node->attr_count; i++)
	{
		const E4XNode* a = node->attributes[i];
		if (a->has_ns && anc_find_uri(&here, a->ns.uri) == NULL && decl_count < 64)
		{
			E4XNamespace d = a->ns;
			if (str_is_empty(d.prefix) && d.uri->len > 0) d.prefix = mint_prefix(ctx, &here);
			decls[decl_count++] = d;
			here.count = decl_count;
		}
	}

	// Prefix lookup: non-empty prefix of the namespace covering a uri.
	#define PREFIX_OF(nsvar, out_prefix) \
		do { \
			const E4XNamespace* _f = anc_find_uri_outer(&here, (nsvar).uri); \
			out_prefix = (_f != NULL && _f->prefix != NULL && _f->prefix->len > 0) \
			                 ? _f->prefix : NULL; \
		} while (0)

	buf_ch(b, '<');
	if (node->has_ns)
	{
		const Avm2String* prefix;
		PREFIX_OF(node->ns, prefix);
		if (prefix != NULL)
		{
			buf_str(b, prefix);
			buf_ch(b, ':');
		}
	}
	buf_str(b, node->local);

	for (uint32_t i = 0; i < node->attr_count; i++)
	{
		const E4XNode* a = node->attributes[i];
		buf_ch(b, ' ');
		if (a->has_ns)
		{
			const Avm2String* prefix;
			PREFIX_OF(a->ns, prefix);
			if (prefix != NULL)
			{
				buf_str(b, prefix);
				buf_ch(b, ':');
			}
		}
		buf_str(b, a->local);
		buf_lit(b, "=\"");
		esc_attribute_value(b, a->text);
		buf_ch(b, '"');
	}
	for (uint32_t i = 0; i < decl_count; i++)
	{
		buf_lit(b, " xmlns");
		if (decls[i].prefix != NULL && decls[i].prefix->len > 0)
		{
			buf_ch(b, ':');
			buf_str(b, decls[i].prefix);
		}
		buf_lit(b, "=\"");
		esc_attribute_value(b, decls[i].uri);
		buf_ch(b, '"');
	}
	#undef PREFIX_OF

	if (node->child_count == 0)
	{
		buf_lit(b, "/>");
		return;
	}
	buf_ch(b, '>');

	int indent_children = node->child_count > 1
	                      || (node->child_count == 1 && !node_is_text(node->children[0]));
	int child_level = -1;
	if (indent_level >= 0 && indent_children)
	{
		child_level = indent_level + indent_step;
	}
	for (uint32_t i = 0; i < node->child_count; i++)
	{
		if (indent_level >= 0 && indent_children) buf_ch(b, '\n');
		to_xml_string_inner(ctx, node->children[i], b, &here, child_level, indent_step);
	}
	if (indent_level >= 0 && indent_children)
	{
		buf_ch(b, '\n');
		for (int i = 0; i < indent_level; i++) buf_ch(b, ' ');
	}

	buf_lit(b, "</");
	if (node->has_ns)
	{
		const E4XNamespace* f = anc_find_uri_outer(&here, node->ns.uri);
		if (f != NULL && f->prefix != NULL && f->prefix->len > 0)
		{
			buf_str(b, f->prefix);
			buf_ch(b, ':');
		}
	}
	buf_str(b, node->local);
	buf_ch(b, '>');
}

const Avm2String* avm2_e4x_to_xml_string(Avm2Context* ctx, const E4XNode* node)
{
	Avm2XmlSettings* st = avm2_xml_settings();
	Buf b;
	buf_init(&b, ctx);
	int level = (st->pretty_printing && st->pretty_indent >= 0) ? 0 : -1;
	to_xml_string_inner(ctx, node, &b, NULL, level, st->pretty_indent);
	return buf_take(&b);
}

static void simple_content_to_string(Avm2Context* ctx, const E4XNode* node, Buf* b);

// xml_to_string: text/cdata/attribute → raw value; simple-content element
// → concatenated content; else toXMLString markup.
const Avm2String* avm2_e4x_to_string(Avm2Context* ctx, const E4XNode* node)
{
	switch (node->kind)
	{
		case E4X_TEXT:
		case E4X_CDATA:
		case E4X_ATTRIBUTE:
			return node->text;
		case E4X_ELEMENT:
			if (avm2_e4x_has_simple_content(node))
			{
				Buf b;
				buf_init(&b, ctx);
				simple_content_to_string(ctx, node, &b);
				return buf_take(&b);
			}
			return avm2_e4x_to_xml_string(ctx, node);
		default:
			return avm2_e4x_to_xml_string(ctx, node);
	}
}

static void simple_content_to_string(Avm2Context* ctx, const E4XNode* node, Buf* b)
{
	for (uint32_t i = 0; i < node->child_count; i++)
	{
		const E4XNode* c = node->children[i];
		if (c->kind == E4X_COMMENT || c->kind == E4X_PI) continue;
		const Avm2String* s = avm2_e4x_to_string(ctx, c);
		buf_str(b, s);
	}
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

typedef struct Parser
{
	Avm2Context* ctx;
	const char* p;
	uint32_t len;
	uint32_t pos;
	int ignore_comments, ignore_pi, ignore_white;
	// Open-element stack; raw_names holds the tag names exactly as written
	// (prefix:local) for end-tag matching.
	E4XNode** open;
	const Avm2String** open_raw;
	uint32_t open_count, open_cap;
	// Top-level output.
	E4XNode** top;
	uint32_t top_count, top_cap;
} Parser;

static _Noreturn void parse_error(Parser* ps, const char* fmt, const Avm2String* arg)
{
	Avm2Context* ctx = ps->ctx;
	// FP's E4X scanner frame has no name at all, and its stack trace shows it
	// as a bare empty line between the message and the first named frame
	// (avm2/xml_list_ctor_errors — the one non-parser error in that test, the
	// #1050, has no such line). See avm2_error.h's unnamed-frame convention.
	avm2_callstack_push_unnamed(ctx);
	if (arg != NULL)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class, fmt,
		                 (int) arg->len, arg->utf8, (int) arg->len, arg->utf8);
	}
	avm2_throw_error(ctx, ctx->builtins.type_error_class, "%s", fmt);
}

// #1104 needs TWO different names interpolated (the attribute and the
// element), which parse_error's single-arg-substituted-twice shape cannot do.
static _Noreturn void parse_error2(Parser* ps, const char* fmt,
                                   const Avm2String* a, const Avm2String* b)
{
	Avm2Context* ctx = ps->ctx;
	avm2_callstack_push_unnamed(ctx);  // same unnamed scanner frame as above
	avm2_throw_error(ctx, ctx->builtins.type_error_class, fmt,
	                 (int) a->len, a->utf8, (int) b->len, b->utf8);
}

static _Noreturn void err_1090(Parser* ps)
{
	parse_error(ps, "Error #1090: XML parser failure: element is malformed.", NULL);
}

static _Noreturn void err_1085(Parser* ps, const Avm2String* tag)
{
	parse_error(ps, "Error #1085: The element type \"%.*s\" must be terminated "
	            "by the matching end-tag \"</%.*s>\".", tag);
}

static int ws(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_ws(Parser* ps)
{
	while (ps->pos < ps->len && ws(ps->p[ps->pos])) ps->pos++;
}

static int starts_with(Parser* ps, const char* s)
{
	uint32_t n = (uint32_t) strlen(s);
	return ps->pos + n <= ps->len && memcmp(ps->p + ps->pos, s, n) == 0;
}

// Case-insensitive variant. avmplus's XML scanner matches the DOCTYPE
// keyword without regard to case (`<!DocType html>` parses like
// `<!DOCTYPE html>`); every other markup keyword stays case-sensitive.
static int starts_with_ci(Parser* ps, const char* s)
{
	uint32_t n = (uint32_t) strlen(s);
	if (ps->pos + n > ps->len) return 0;
	for (uint32_t i = 0; i < n; i++)
	{
		char a = ps->p[ps->pos + i];
		char b = s[i];
		if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
		if (a != b) return 0;
	}
	return 1;
}

// Entity decoding (Ruffle avm2_unescape): the five named entities +
// numeric character refs; undecodable entities are preserved verbatim.
static void decode_entities(Parser* ps, Buf* out, const char* s, uint32_t len)
{
	(void) ps;
	for (uint32_t i = 0; i < len; i++)
	{
		if (s[i] != '&')
		{
			buf_ch(out, s[i]);
			continue;
		}
		// Find the terminating ';' (Ruffle regex &[^;]*; — a second '&'
		// inside the span is part of the entity text and fails decoding).
		uint32_t j = i + 1;
		while (j < len && s[j] != ';') j++;
		if (j >= len)
		{
			buf_ch(out, s[i]);
			continue;
		}
		const char* ent = s + i + 1;
		uint32_t elen = j - (i + 1);
		if (elen == 3 && memcmp(ent, "amp", 3) == 0) buf_ch(out, '&');
		else if (elen == 2 && memcmp(ent, "lt", 2) == 0) buf_ch(out, '<');
		else if (elen == 2 && memcmp(ent, "gt", 2) == 0) buf_ch(out, '>');
		else if (elen == 4 && memcmp(ent, "quot", 4) == 0) buf_ch(out, '"');
		else if (elen == 4 && memcmp(ent, "apos", 4) == 0) buf_ch(out, '\'');
		else if (elen >= 2 && ent[0] == '#')
		{
			uint32_t cp = 0;
			int ok = 1;
			if (ent[1] == 'x' || ent[1] == 'X')
			{
				if (elen < 3) ok = 0;
				for (uint32_t k = 2; k < elen && ok; k++)
				{
					char c = ent[k];
					cp *= 16;
					if (c >= '0' && c <= '9') cp += (uint32_t) (c - '0');
					else if (c >= 'a' && c <= 'f') cp += (uint32_t) (c - 'a' + 10);
					else if (c >= 'A' && c <= 'F') cp += (uint32_t) (c - 'A' + 10);
					else ok = 0;
				}
			}
			else
			{
				for (uint32_t k = 1; k < elen && ok; k++)
				{
					char c = ent[k];
					if (c < '0' || c > '9') ok = 0;
					else cp = cp * 10 + (uint32_t) (c - '0');
				}
			}
			if (!ok || cp == 0 || cp > 0x10FFFF)
			{
				// Preserve the raw entity text.
				buf_bytes(out, s + i, j - i + 1);
			}
			else if (cp < 0x80)
			{
				buf_ch(out, (char) cp);
			}
			else if (cp < 0x800)
			{
				buf_ch(out, (char) (0xC0 | (cp >> 6)));
				buf_ch(out, (char) (0x80 | (cp & 0x3F)));
			}
			else if (cp < 0x10000)
			{
				buf_ch(out, (char) (0xE0 | (cp >> 12)));
				buf_ch(out, (char) (0x80 | ((cp >> 6) & 0x3F)));
				buf_ch(out, (char) (0x80 | (cp & 0x3F)));
			}
			else
			{
				buf_ch(out, (char) (0xF0 | (cp >> 18)));
				buf_ch(out, (char) (0x80 | ((cp >> 12) & 0x3F)));
				buf_ch(out, (char) (0x80 | ((cp >> 6) & 0x3F)));
				buf_ch(out, (char) (0x80 | (cp & 0x3F)));
			}
		}
		else
		{
			// Unknown entity: preserved verbatim.
			buf_bytes(out, s + i, j - i + 1);
		}
		i = j;
	}
}

static const Avm2String* decode_entities_str(Parser* ps, const char* s, uint32_t len)
{
	Buf b;
	buf_init(&b, ps->ctx);
	decode_entities(ps, &b, s, len);
	return buf_take(&b);
}

static void push_top(Parser* ps, E4XNode* node)
{
	if (ps->top_count == ps->top_cap)
	{
		uint32_t ncap = ps->top_cap ? ps->top_cap * 2 : 4;
		E4XNode** nt = avm2_alloc(ps->ctx, ncap * sizeof(E4XNode*));
		memcpy(nt, ps->top, ps->top_count * sizeof(E4XNode*));
		if (ps->top != NULL) heap_free(ps->ctx->app, ps->top);
		ps->top = nt;
		ps->top_cap = ncap;
	}
	ps->top[ps->top_count++] = node;
}

static void push_childless(Parser* ps, E4XNode* node)
{
	if (ps->open_count > 0)
	{
		E4XNode* parent = ps->open[ps->open_count - 1];
		node->parent = parent;
		node_push_child(ps->ctx, parent, node);
	}
	else
	{
		push_top(ps, node);
	}
}

static void handle_text_cdata(Parser* ps, const Avm2String* text, int is_text)
{
	int is_ws = str_all_whitespace(text);
	if (is_text && ps->ignore_white && is_ws) return;
	const Avm2String* val = text;
	if (is_text && ps->ignore_white)
	{
		// trim_ascii
		uint32_t a = 0, b = text->len;
		while (a < b && ws(text->utf8[a])) a++;
		while (b > a && ws(text->utf8[b - 1])) b--;
		val = avm2_string_new(ps->ctx, text->utf8 + a, b - a);
	}
	E4XNode* n = avm2_e4x_node_new(ps->ctx, is_text ? E4X_TEXT : E4X_CDATA, NULL);
	n->text = val;
	push_childless(ps, n);
}

// A name token: up to whitespace, '=', '/', '>'.
static const Avm2String* parse_name_token(Parser* ps)
{
	uint32_t start = ps->pos;
	while (ps->pos < ps->len)
	{
		char c = ps->p[ps->pos];
		if (ws(c) || c == '=' || c == '/' || c == '>') break;
		ps->pos++;
	}
	if (ps->pos == start) err_1090(ps);
	return avm2_string_new(ps->ctx, ps->p + start, ps->pos - start);
}

// Split "prefix:local"; empty prefix (leading ':') is malformed (1084 side).
static void split_qname(const Avm2String* raw, const Avm2String** prefix,
                        const Avm2String** local, Parser* ps)
{
	*prefix = NULL;
	for (uint32_t i = 0; i < raw->len; i++)
	{
		if (raw->utf8[i] == ':')
		{
			if (i == 0)
			{
				parse_error(ps, "Error #1084: Element or attribute (\":%.*s\") does "
				            "not match QName production: QName::=(NCName':')?NCName.",
				            avm2_string_new(ps->ctx, raw->utf8 + 1, raw->len - 1));
			}
			*prefix = avm2_string_new(ps->ctx, raw->utf8, i);
			*local = avm2_string_new(ps->ctx, raw->utf8 + i + 1, raw->len - i - 1);
			return;
		}
	}
	*local = raw;
}

// Resolve a prefix against the open-element declaration stack (the element
// being built is already ON the stack conceptually — caller passes it).
static const Avm2String* resolve_prefix(Parser* ps, E4XNode* current,
                                        const Avm2String* prefix)
{
	if (prefix->len == 3 && memcmp(prefix->utf8, "xml", 3) == 0)
	{
		return avm2_string_from_literal(ps->ctx, "http://www.w3.org/XML/1998/namespace");
	}
	E4XNode* chain[2] = { current, NULL };
	(void) chain;
	for (E4XNode* e = current; e != NULL; e = e->parent)
	{
		for (uint32_t i = 0; i < e->ns_count; i++)
		{
			if (e->namespaces[i].prefix != NULL
			    && str_eq(e->namespaces[i].prefix, prefix))
			{
				return e->namespaces[i].uri;
			}
		}
	}
	return NULL;  // unbound
}

// The nearest default-namespace declaration (xmlns="...").
static const Avm2String* resolve_default_ns(E4XNode* current)
{
	for (E4XNode* e = current; e != NULL; e = e->parent)
	{
		for (uint32_t i = 0; i < e->ns_count; i++)
		{
			if (e->namespaces[i].prefix != NULL && e->namespaces[i].prefix->len == 0)
			{
				return e->namespaces[i].uri;
			}
		}
	}
	return NULL;
}

// Parses a start tag after '<'; returns the element (self-closed handled
// by the caller via *out_self_closing).
static E4XNode* parse_start_tag(Parser* ps, const Avm2String** out_raw,
                                int* out_self_closing)
{
	Avm2Context* ctx = ps->ctx;
	const Avm2String* raw = parse_name_token(ps);
	*out_raw = raw;

	E4XNode* elem = avm2_e4x_node_new(ctx, E4X_ELEMENT, NULL);
	if (ps->open_count > 0)
	{
		elem->parent = ps->open[ps->open_count - 1];  // for ns resolution
	}

	// Raw attribute list: two passes (declarations first, then resolution).
	typedef struct RawAttr
	{
		const Avm2String* raw_name;
		const Avm2String* value;
	} RawAttr;
	RawAttr attrs[128];
	uint32_t attr_n = 0;

	for (;;)
	{
		skip_ws(ps);
		if (ps->pos >= ps->len) err_1090(ps);
		char c = ps->p[ps->pos];
		if (c == '>')
		{
			ps->pos++;
			*out_self_closing = 0;
			break;
		}
		if (c == '/')
		{
			ps->pos++;
			if (ps->pos >= ps->len || ps->p[ps->pos] != '>') err_1090(ps);
			ps->pos++;
			*out_self_closing = 1;
			break;
		}
		const Avm2String* aname = parse_name_token(ps);
		skip_ws(ps);
		if (ps->pos >= ps->len || ps->p[ps->pos] != '=') err_1090(ps);
		ps->pos++;
		skip_ws(ps);
		if (ps->pos >= ps->len) err_1090(ps);
		char quote = ps->p[ps->pos];
		if (quote != '"' && quote != '\'') err_1090(ps);
		ps->pos++;
		uint32_t vstart = ps->pos;
		while (ps->pos < ps->len && ps->p[ps->pos] != quote) ps->pos++;
		if (ps->pos >= ps->len) err_1090(ps);
		const Avm2String* value =
			decode_entities_str(ps, ps->p + vstart, ps->pos - vstart);
		ps->pos++;
		if (attr_n < 128)
		{
			attrs[attr_n].raw_name = aname;
			attrs[attr_n].value = value;
			attr_n++;
		}
	}

	// Duplicate raw attribute names → #1104. This runs only AFTER the start tag
	// has been terminated: quick_xml (and therefore ruffle e4x.rs
	// from_start_event) only sees the attribute list once the scanner has
	// produced a complete BytesStart, so an UNTERMINATED tag reports #1090 even
	// when it also carries a duplicate (`<root a="" a=""` with no `>`).
	for (uint32_t i = 1; i < attr_n; i++)
	{
		for (uint32_t j = 0; j < i; j++)
		{
			if (str_eq(attrs[j].raw_name, attrs[i].raw_name))
			{
				parse_error2(ps, "Error #1104: Attribute \"%.*s\" was already "
				             "specified for element \"%.*s\".",
				             attrs[i].raw_name, raw);
			}
		}
	}

	// Pass 1: xmlns declarations.
	for (uint32_t i = 0; i < attr_n; i++)
	{
		const Avm2String* an = attrs[i].raw_name;
		if (an->len == 5 && memcmp(an->utf8, "xmlns", 5) == 0)
		{
			E4XNamespace ns = { attrs[i].value, str_empty(ctx) };
			node_push_ns(ctx, elem, ns);
			attrs[i].raw_name = NULL;  // consumed
		}
		else if (an->len > 6 && memcmp(an->utf8, "xmlns:", 6) == 0)
		{
			E4XNamespace ns = { attrs[i].value,
			                    avm2_string_new(ctx, an->utf8 + 6, an->len - 6) };
			node_push_ns(ctx, elem, ns);
			attrs[i].raw_name = NULL;
		}
	}

	// Pass 2: resolve the element name.
	{
		const Avm2String* prefix;
		const Avm2String* local;
		split_qname(raw, &prefix, &local, ps);
		if (prefix != NULL)
		{
			const Avm2String* uri = resolve_prefix(ps, elem, prefix);
			if (uri == NULL)
			{
				avm2_throw_error(ps->ctx, ps->ctx->builtins.type_error_class,
				                 "Error #1083: The prefix \"%.*s\" for element "
				                 "\"%.*s\" is not bound.",
				                 (int) prefix->len, prefix->utf8,
				                 (int) local->len, local->utf8);
			}
			elem->has_ns = 1;
			elem->ns.uri = uri;
			elem->ns.prefix = prefix;
		}
		else
		{
			const Avm2String* def = resolve_default_ns(elem);
			if (def == NULL) def = avm2_dxns_uri(ctx);
			if (def != NULL && def->len > 0)
			{
				elem->has_ns = 1;
				elem->ns.uri = def;
				elem->ns.prefix = NULL;
			}
		}
		elem->local = local;
	}

	// Pass 3: resolve + attach attributes.
	for (uint32_t i = 0; i < attr_n; i++)
	{
		if (attrs[i].raw_name == NULL) continue;
		const Avm2String* prefix;
		const Avm2String* local;
		split_qname(attrs[i].raw_name, &prefix, &local, ps);
		E4XNamespace ns = { NULL, NULL };
		E4XNamespace* nsp = NULL;
		if (prefix != NULL)
		{
			const Avm2String* uri = resolve_prefix(ps, elem, prefix);
			if (uri == NULL)
			{
				avm2_throw_error(ps->ctx, ps->ctx->builtins.type_error_class,
				                 "Error #1083: The prefix \"%.*s\" for element "
				                 "\"%.*s\" is not bound.",
				                 (int) prefix->len, prefix->utf8,
				                 (int) local->len, local->utf8);
			}
			ns.uri = uri;
			ns.prefix = prefix;
			nsp = &ns;
		}
		E4XNode* a = avm2_e4x_attribute(ctx, nsp, local, attrs[i].value, elem);
		avm2_e4x_append_attribute(ctx, elem, a);
	}
	return elem;
}

static void push_open(Parser* ps, E4XNode* elem, const Avm2String* raw)
{
	if (ps->open_count == ps->open_cap)
	{
		uint32_t ncap = ps->open_cap ? ps->open_cap * 2 : 8;
		E4XNode** no = avm2_alloc(ps->ctx, ncap * sizeof(E4XNode*));
		const Avm2String** nr = avm2_alloc(ps->ctx, ncap * sizeof(Avm2String*));
		memcpy(no, ps->open, ps->open_count * sizeof(E4XNode*));
		memcpy(nr, ps->open_raw, ps->open_count * sizeof(Avm2String*));
		if (ps->open != NULL) heap_free(ps->ctx->app, ps->open);
		if (ps->open_raw != NULL) heap_free(ps->ctx->app, (void*) ps->open_raw);
		ps->open = no;
		ps->open_raw = nr;
		ps->open_cap = ncap;
	}
	ps->open[ps->open_count] = elem;
	ps->open_raw[ps->open_count] = raw;
	ps->open_count++;
}

E4XNode** avm2_e4x_parse(Avm2Context* ctx, Avm2Value value,
                         int ignore_comments, int ignore_pi, int ignore_white,
                         uint32_t* out_count)
{
	const Avm2String* src;
	if (value.kind == AVM2_VALUE_NULL || value.kind == AVM2_VALUE_UNDEFINED)
	{
		src = str_empty(ctx);
	}
	else if (avm2_value_is_xmlish(value))
	{
		Avm2Value s = avm2_call_public_property(ctx, value, "toXMLString", 11, NULL, 0);
		src = avm2_coerce_to_string(ctx, s);
	}
	else
	{
		src = avm2_coerce_to_string(ctx, value);
	}

	Parser ps;
	memset(&ps, 0, sizeof(ps));
	ps.ctx = ctx;
	ps.p = src->utf8;
	ps.len = src->len;
	ps.ignore_comments = ignore_comments;
	ps.ignore_pi = ignore_pi;
	ps.ignore_white = ignore_white;

	uint32_t text_start = 0;

	while (ps.pos < ps.len)
	{
		if (ps.p[ps.pos] != '<')
		{
			ps.pos++;
			continue;
		}
		// Flush pending text.
		if (ps.pos > text_start)
		{
			const Avm2String* t =
				decode_entities_str(&ps, ps.p + text_start, ps.pos - text_start);
			handle_text_cdata(&ps, t, 1);
		}

		if (starts_with(&ps, "<!--"))
		{
			ps.pos += 4;
			uint32_t start = ps.pos;
			for (;;)
			{
				if (ps.pos + 3 > ps.len)
				{
					parse_error(&ps, "Error #1094: XML parser failure: "
					            "Unterminated comment", NULL);
				}
				if (memcmp(ps.p + ps.pos, "-->", 3) == 0) break;
				ps.pos++;
			}
			if (!ps.ignore_comments)
			{
				E4XNode* n = avm2_e4x_node_new(ctx, E4X_COMMENT, NULL);
				n->text = decode_entities_str(&ps, ps.p + start, ps.pos - start);
				push_childless(&ps, n);
			}
			ps.pos += 3;
		}
		else if (starts_with(&ps, "<![CDATA["))
		{
			ps.pos += 9;
			uint32_t start = ps.pos;
			for (;;)
			{
				if (ps.pos + 3 > ps.len)
				{
					parse_error(&ps, "Error #1091: XML parser failure: "
					            "Unterminated CDATA section.", NULL);
				}
				if (memcmp(ps.p + ps.pos, "]]>", 3) == 0) break;
				ps.pos++;
			}
			const Avm2String* t = avm2_string_new(ctx, ps.p + start, ps.pos - start);
			handle_text_cdata(&ps, t, 0);
			ps.pos += 3;
		}
		else if (starts_with_ci(&ps, "<!DOCTYPE"))
		{
			// Skip to the matching '>' (with [..] internal subset).
			ps.pos += 9;
			int bracket = 0;
			for (;;)
			{
				if (ps.pos >= ps.len)
				{
					parse_error(&ps, "Error #1093: XML parser failure: "
					            "Unterminated DOCTYPE declaration.", NULL);
				}
				char c = ps.p[ps.pos++];
				if (c == '[') bracket = 1;
				else if (c == ']') bracket = 0;
				else if (c == '>' && !bracket) break;
			}
		}
		else if (starts_with(&ps, "<?"))
		{
			ps.pos += 2;
			uint32_t start = ps.pos;
			for (;;)
			{
				if (ps.pos + 2 > ps.len)
				{
					parse_error(&ps, "Error #1097: XML parser failure: "
					            "Unterminated processing instruction.", NULL);
				}
				if (memcmp(ps.p + ps.pos, "?>", 2) == 0) break;
				ps.pos++;
			}
			const Avm2String* body =
				decode_entities_str(&ps, ps.p + start, ps.pos - start);
			ps.pos += 2;
			// Split at the first space: target / value (value trim_start).
			uint32_t sp = 0;
			while (sp < body->len && body->utf8[sp] != ' ') sp++;
			const Avm2String* target = avm2_string_new(ctx, body->utf8, sp);
			uint32_t vst = sp;
			while (vst < body->len
			       && (body->utf8[vst] == ' ' || body->utf8[vst] == '\t'
			           || body->utf8[vst] == '\n' || body->utf8[vst] == '\r'))
			{
				vst++;
			}
			const Avm2String* pival =
				avm2_string_new(ctx, body->utf8 + vst, body->len - vst);
			// The XML declaration (<?xml ...?>) is dropped entirely.
			int is_decl = target->len == 3
			              && (target->utf8[0] == 'x' || target->utf8[0] == 'X')
			              && (target->utf8[1] == 'm' || target->utf8[1] == 'M')
			              && (target->utf8[2] == 'l' || target->utf8[2] == 'L');
			if (!is_decl && !ps.ignore_pi)
			{
				E4XNode* n = avm2_e4x_node_new(ctx, E4X_PI, NULL);
				n->local = target;
				n->text = pival;
				push_childless(&ps, n);
			}
		}
		else if (ps.pos + 1 < ps.len && ps.p[ps.pos + 1] == '/')
		{
			// End tag.
			ps.pos += 2;
			uint32_t start = ps.pos;
			while (ps.pos < ps.len && ps.p[ps.pos] != '>') ps.pos++;
			if (ps.pos >= ps.len) err_1090(&ps);
			const Avm2String* found =
				avm2_string_new(ctx, ps.p + start, ps.pos - start);
			ps.pos++;
			if (ps.open_count == 0)
			{
				parse_error(&ps, "Error #1088: The markup in the document "
				            "following the root element must be well-formed.", NULL);
			}
			const Avm2String* expected = ps.open_raw[ps.open_count - 1];
			int matches = str_eq(found, expected);
			if (!matches && found->len > expected->len
			    && memcmp(found->utf8, expected->utf8, expected->len) == 0)
			{
				// Lenient: </a/>, </a >, </a b="c">.
				char after = found->utf8[expected->len];
				matches = (after == ' ' || after == '\t' || after == '/');
			}
			if (!matches)
			{
				err_1085(&ps, expected);
			}
			ps.open_count--;
			if (ps.open_count == 0)
			{
				push_top(&ps, ps.open[0]);
			}
		}
		else
		{
			// Start tag.
			ps.pos++;
			if (ps.pos >= ps.len) err_1090(&ps);
			if (ps.p[ps.pos] == '!') err_1090(&ps);
			const Avm2String* raw;
			int self_closing;
			E4XNode* elem = parse_start_tag(&ps, &raw, &self_closing);
			if (self_closing)
			{
				elem->parent = NULL;
				push_childless(&ps, elem);
			}
			else
			{
				if (ps.open_count > 0)
				{
					E4XNode* parent = ps.open[ps.open_count - 1];
					elem->parent = parent;
					node_push_child(ctx, parent, elem);
				}
				else
				{
					elem->parent = NULL;
				}
				push_open(&ps, elem, raw);
			}
		}
		text_start = ps.pos;
	}

	// Trailing text.
	if (ps.pos > text_start)
	{
		const Avm2String* t =
			decode_entities_str(&ps, ps.p + text_start, ps.pos - text_start);
		handle_text_cdata(&ps, t, 1);
	}
	if (ps.open_count > 0)
	{
		err_1085(&ps, ps.open_raw[ps.open_count - 1]);
	}

	// The open-element stack is parse scratch; the top-level array is the
	// caller's to free (avm2_e4x_parse_free).
	if (ps.open != NULL) heap_free(ctx->app, ps.open);
	if (ps.open_raw != NULL) heap_free(ctx->app, (void*) ps.open_raw);
	*out_count = ps.top_count;
	return ps.top;
}

void avm2_e4x_parse_free(Avm2Context* ctx, E4XNode** nodes)
{
	if (nodes != NULL) heap_free(ctx->app, nodes);
}
