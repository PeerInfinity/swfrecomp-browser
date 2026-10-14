#ifndef AVM2_E4X_H
#define AVM2_E4X_H

// E4X / XML engine (ECMA-357), ported from Ruffle core/src/avm2/e4x.rs +
// object/xml_object.rs + object/xml_list_object.rs + globals/xml*.
//
// Two modules:
//   avm2_e4x.c — the node tree, the hand-written parser (Ruffle uses
//                quick-xml; we parse the same E4X subset), serialization
//                (toString / toXMLString with prettyPrinting), name
//                matching, and tree mutation.
//   avm2_xml.c — the XML / XMLList classes: method surface, class-call
//                conversions, settings statics, and the property-engine
//                entry points called from avm2_ops.c.
//
// GC: E4X nodes are non-census allocations (avm2_alloc) with their own
// collector — see E4XNode.gc_all_next.

#include <avm2/avm2_abc.h>
#include <avm2/avm2_class.h>
#include <avm2/avm2_value.h>

typedef struct Avm2Context Avm2Context;

// ---------------------------------------------------------------------------
// Node tree (Ruffle E4XNode / E4XNodeKind)
// ---------------------------------------------------------------------------

typedef enum E4XKind
{
	E4X_ELEMENT = 0,
	E4X_TEXT = 1,
	E4X_CDATA = 2,
	E4X_COMMENT = 3,
	E4X_PI = 4,        // target in `local`, value in `text`
	E4X_ATTRIBUTE = 5, // name in `local`/ns, value in `text`
} E4XKind;

// A namespace declaration ({uri, prefix}); prefix == NULL means "no
// explicit prefix" (a node's resolved namespace), prefix == "" marks the
// default declaration (xmlns="...").
typedef struct E4XNamespace
{
	const Avm2String* uri;     // never NULL
	const Avm2String* prefix;  // NULL = none
} E4XNamespace;

typedef struct E4XNode E4XNode;
struct E4XNode
{
	uint8_t kind;  // E4XKind
	E4XNode* parent;
	// The node's name namespace (Ruffle Option<E4XNamespace>).
	uint8_t has_ns;
	E4XNamespace ns;
	const Avm2String* local;  // NULL for text/cdata/comment
	const Avm2String* text;   // text/cdata/comment/PI value; attribute value
	// Element payload.
	E4XNode** children;
	uint32_t child_count, child_cap;
	E4XNode** attributes;
	uint32_t attr_count, attr_cap;
	E4XNamespace* namespaces;  // declarations made on THIS element
	uint32_t ns_count, ns_cap;
	// Cached XML wrapper object: every materialization of this node as an
	// AS value returns the same object (xml_basic: a.x[0] === a.x[0]).
	Avm2Object* obj;
	// setNotification callback (undocumented; xml_notification_bubbling).
	Avm2Object* notify;
	// GC: all-nodes registry link (avm2_e4x_node_new). Nodes are non-census
	// allocations owned by the E4X collector: a node is live while any XML /
	// XMLList wrapper reaches its tree (avm2_xml_gc_trace_ext marks the whole
	// connected tree — parent, children, attributes — plus each node's strings,
	// cached wrapper and notify closure); avm2_e4x_gc_sweep_nodes frees the
	// rest. Marking reads the CURRENT field values, so raw field assignments
	// anywhere in e4x/xml code stay safe.
	E4XNode* gc_all_next;
	uint32_t gc_mark;  // == the collector's node epoch when reached this cycle
};

// GC (avm2_gc.c drives these; see E4XNode.gc_all_next).
void avm2_e4x_gc_begin(void);
void avm2_e4x_gc_mark_node(E4XNode* n);
uint32_t avm2_e4x_gc_sweep_nodes(Avm2Context* ctx);

// ---------------------------------------------------------------------------
// Query names (Ruffle Multiname as E4X sees it, post handle_input_multiname)
// ---------------------------------------------------------------------------

typedef struct E4XName
{
	const Avm2String* local;  // NULL = any name
	uint8_t is_attribute;
	uint8_t is_qname;    // single explicit namespace (QName semantics)
	uint8_t any_ns;      // the any namespace
	// The namespace set, canonicalized: either a single URI or a static
	// ns-set from an ABC file. When both are unset the set is empty.
	const Avm2String* single_uri;   // may be "" (public); NULL when set-based
	uint8_t single_is_real;         // single_uri is a Namespace/Package kind
	const Avm2AbcFileData* data;    // set-based form (attribute names keep
	uint32_t ns_set;                // their original compiler set)
	// handle_input rebuild with no dxns: original set PLUS public — also
	// match empty-uri nodes even if the set lacks the public namespace.
	uint8_t also_public;
} E4XName;

// Builders (all apply the Ruffle handle_input_multiname semantics,
// including the default-XML-namespace rebuild).
E4XName avm2_e4x_name_from_mn(Avm2Context* ctx, const Avm2AbcFileData* data,
                              uint32_t mn_idx);
// string_to_multiname: "@*" / "@name" / "*" / plain (public or dxns).
E4XName avm2_e4x_name_from_string(Avm2Context* ctx, const Avm2String* s,
                                  int force_attribute);
// name_to_multiname over a runtime value (QName objects keep their own
// uri/local/attribute flag; null/undefined throws 1010).
E4XName avm2_e4x_name_from_value(Avm2Context* ctx, Avm2Value v, int force_attribute);

int avm2_e4x_name_is_public_like(const E4XName* n);  // matches empty-uri nodes
int avm2_e4x_matches_name(const E4XNode* node, const E4XName* name);

// ---------------------------------------------------------------------------
// Node tree API (avm2_e4x.c)
// ---------------------------------------------------------------------------

E4XNode* avm2_e4x_node_new(Avm2Context* ctx, uint8_t kind, E4XNode* parent);
E4XNode* avm2_e4x_text(Avm2Context* ctx, const Avm2String* s, E4XNode* parent);
E4XNode* avm2_e4x_element(Avm2Context* ctx, const E4XNamespace* ns,
                          const Avm2String* local, E4XNode* parent);
E4XNode* avm2_e4x_attribute(Avm2Context* ctx, const E4XNamespace* ns,
                            const Avm2String* local, const Avm2String* value,
                            E4XNode* parent);
E4XNode* avm2_e4x_deep_copy(Avm2Context* ctx, const E4XNode* node);

// Parse per the current XML settings flags. Returns the top-level node
// list (count in *out_count); throws typed parse errors (1085/1088/...).
// The array is scratch: release it with avm2_e4x_parse_free (the nodes are
// owned by the E4X collector, not by the array).
E4XNode** avm2_e4x_parse(Avm2Context* ctx, Avm2Value value,
                         int ignore_comments, int ignore_pi, int ignore_white,
                         uint32_t* out_count);
void avm2_e4x_parse_free(Avm2Context* ctx, E4XNode** nodes);

// Serialization.
const Avm2String* avm2_e4x_to_xml_string(Avm2Context* ctx, const E4XNode* node);
const Avm2String* avm2_e4x_to_string(Avm2Context* ctx, const E4XNode* node);

int avm2_e4x_has_simple_content(const E4XNode* node);
int avm2_e4x_has_complex_content(const E4XNode* node);
int avm2_e4x_is_xml_name(const Avm2String* s);
int avm2_e4x_equals(const E4XNode* a, const E4XNode* b);  // deep (==)
int avm2_e4x_is_ancestor(const E4XNode* maybe_ancestor, const E4XNode* node);

// Mutation (parent pointers maintained; cyclic inserts throw 1118).
void avm2_e4x_insert_at(Avm2Context* ctx, E4XNode* elem, uint32_t index, E4XNode* child);
void avm2_e4x_insert_value(Avm2Context* ctx, E4XNode* elem, uint32_t index, Avm2Value v);
void avm2_e4x_replace(Avm2Context* ctx, E4XNode* elem, uint32_t index, Avm2Value v);
void avm2_e4x_delete_by_index(E4XNode* elem, uint32_t index);
void avm2_e4x_remove_all_children(E4XNode* elem);
void avm2_e4x_remove_child(E4XNode* elem, E4XNode* child);
void avm2_e4x_remove_attribute(E4XNode* elem, E4XNode* attr);
// Removes ALL nodes matching `name`; reports the first removed one.
int avm2_e4x_remove_matching(Avm2Context* ctx, E4XNode* elem, const E4XName* name,
                             int attributes, uint32_t* out_index, E4XNode** out_node);
void avm2_e4x_append_attribute(Avm2Context* ctx, E4XNode* elem, E4XNode* attr);
void avm2_e4x_normalize(Avm2Context* ctx, E4XNode* elem);
int avm2_e4x_child_index(const E4XNode* node);  // -1 for attribute/detached

// Namespace machinery.
uint32_t avm2_e4x_in_scope_namespaces(Avm2Context* ctx, const E4XNode* node,
                                      E4XNamespace** out);  // allocates; caller heap_frees *out
E4XNamespace avm2_e4x_get_namespace(Avm2Context* ctx, const E4XNode* node,
                                    const E4XNamespace* in_scope, uint32_t n);
void avm2_e4x_add_in_scope_namespace(Avm2Context* ctx, E4XNode* elem,
                                     const E4XNamespace* ns);

// Descendants (`..`): pre-order walk collecting matches into a list ext.
struct Avm2XmlListExt;
void avm2_e4x_descendants(Avm2Context* ctx, const E4XNode* node, const E4XName* name,
                          struct Avm2XmlListExt* out);

// ---------------------------------------------------------------------------
// XML / XMLList objects (avm2_xml.c)
// ---------------------------------------------------------------------------

typedef struct Avm2XmlExt
{
	E4XNode* node;
} Avm2XmlExt;

typedef struct Avm2XmlListExt
{
	E4XNode** items;
	uint32_t count, cap;
	// Target linkage (ECMA-357 [[TargetObject]]/[[TargetProperty]]): the
	// query this list resulted from, used by resolve_value for appends.
	Avm2Object* target_object;  // XML or XMLList object; NULL = none
	uint8_t has_target_prop;
	E4XName target_prop;
	uint8_t target_dirty;
} Avm2XmlListExt;

// NULL when the value is not an XML / XMLList instance.
Avm2XmlExt* avm2_xml_ext_of(Avm2Value v);
Avm2XmlListExt* avm2_xmllist_ext_of(Avm2Value v);
int avm2_value_is_xmlish(Avm2Value v);  // XML or XMLList

// The cached wrapper for a node (creates + caches on first use).
Avm2Object* avm2_xml_object_for_node(Avm2Context* ctx, E4XNode* node);
Avm2Object* avm2_xmllist_new(Avm2Context* ctx, Avm2Object* target_object,
                             const E4XName* target_prop);
void avm2_xmllist_push(Avm2Context* ctx, Avm2XmlListExt* list, E4XNode* node);

// The current default XML namespace URI (Dxns/DxnsLate), or NULL.
// Stored on the context; saved/restored around calls by
// avm2_call_method_ref (methods with SET_DXNS start fresh).
const Avm2String* avm2_dxns_uri(Avm2Context* ctx);

// Global XML settings (XML.prettyPrinting & co; Ruffle XmlSettings).
typedef struct Avm2XmlSettings
{
	uint8_t ignore_comments;   // default 1
	uint8_t ignore_pi;         // default 1
	uint8_t ignore_white;      // default 1
	uint8_t pretty_printing;   // default 1
	int32_t pretty_indent;     // default 2; < 0 disables pretty printing
} Avm2XmlSettings;
Avm2XmlSettings* avm2_xml_settings(void);

// Notification dispatch (bubbles through ancestors' setNotification fns).
void avm2_xml_notify(Avm2Context* ctx, E4XNode* target, const char* command,
                     Avm2Value value, Avm2Value detail);

// Registration (replaces the Stage-2 stubs).
void avm2_register_xml(Avm2Context* ctx);

// --- Property-engine entry points (called from avm2_ops.c) ---
// Each returns 1 when the access was handled; 0 = fall through to the
// generic resolve path (only for non-public method-trait hits, e.g.
// a.AS3::name → bound method).
int avm2_xml_get_mn(Avm2Context* ctx, Avm2Value recv, const Avm2AbcFileData* data,
                    uint32_t mn_idx, Avm2Value* out);
int avm2_xml_get_name(Avm2Context* ctx, Avm2Value recv, const E4XName* name,
                      Avm2Value* out);
int avm2_xml_set_mn(Avm2Context* ctx, Avm2Value recv, const Avm2AbcFileData* data,
                    uint32_t mn_idx, Avm2Value value);
int avm2_xml_set_name(Avm2Context* ctx, Avm2Value recv, const E4XName* name,
                      Avm2Value value);
int avm2_xml_delete_mn(Avm2Context* ctx, Avm2Value recv, const Avm2AbcFileData* data,
                       uint32_t mn_idx, Avm2Value* out);
int avm2_xml_delete_name(Avm2Context* ctx, Avm2Value recv, const E4XName* name,
                         Avm2Value* out);
// Call fallback after the trait resolve missed (simple-content string
// delegation / one-element-list delegation). Always handles xmlish recv.
Avm2Value avm2_xml_call_fallback(Avm2Context* ctx, Avm2Value recv,
                                 const char* name, uint32_t name_len,
                                 const Avm2Value* args, uint32_t argc);
// `in` / hasOwnProperty.
int avm2_xml_has_property_via_in(Avm2Context* ctx, Avm2Value recv,
                                 const Avm2String* name);
int avm2_xml_has_own(Avm2Context* ctx, Avm2Value recv,
                     const char* name, uint32_t name_len);
// Enumeration hooks (avm2_object.c protocol); return 0 if not xmlish.
int avm2_xml_next_enumerant(Avm2Object* obj, uint32_t cur, uint32_t* out);
int avm2_xml_enumerant_name(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                            Avm2Value* out);
int avm2_xml_enumerant_value(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                             Avm2Value* out);
// Abstract equality arms (avm2_value.c). Handles the case where a is
// XML/XMLList (b arbitrary). Returns 1 if handled (result in *eq).
int avm2_xml_abstract_eq(Avm2Context* ctx, Avm2Value a, Avm2Value b, int* eq);
// GetDescendants over any value (throws 1016 on non-XML receivers).
Avm2Value avm2_xml_descendants_value(Avm2Context* ctx, Avm2Value v, const E4XName* name);
// Static-multiname has_property (with-scope finds in filter expressions).
int avm2_xml_has_property_mn(Avm2Context* ctx, Avm2Value recv,
                             const Avm2AbcFileData* data, uint32_t mn_idx);

#endif // AVM2_E4X_H
