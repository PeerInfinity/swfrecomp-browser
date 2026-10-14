// Class realization, flattened vtables (Ruffle vtable.rs model), scope
// chains, method invocation, type checks, and function objects. SWF-defined
// classes are built by NewClass from the emitted static data; builtins
// (avm2_globals.c and friends) register through the same vtable mechanism.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_flixel.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_gc.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>

int avm2_class_qname_buf(const Avm2Class* cls, char* buf, int size)
{
	if (cls == NULL)
	{
		return snprintf(buf, size, "Object");
	}
	if (cls->name.ns_len > 0)
	{
		return snprintf(buf, size, "%.*s.%.*s",
		                (int) cls->name.ns_len, cls->name.ns_uri,
		                (int) cls->name.name_len, cls->name.name);
	}
	return snprintf(buf, size, "%.*s", (int) cls->name.name_len, cls->name.name);
}

int avm2_class_qname_colons_buf(const Avm2Class* cls, char* buf, int size)
{
	if (cls == NULL)
	{
		return snprintf(buf, size, "Object");
	}
	if (cls->name.ns_len > 0)
	{
		return snprintf(buf, size, "%.*s::%.*s",
		                (int) cls->name.ns_len, cls->name.ns_uri,
		                (int) cls->name.name_len, cls->name.name);
	}
	return snprintf(buf, size, "%.*s", (int) cls->name.name_len, cls->name.name);
}

uint8_t avm2_ns_fold(uint8_t kind)
{
	// Namespace (0x08) and Package (0x16) are both "public" (Ruffle
	// namespace.rs is_public folds them).
	return (kind == 0x08) ? 0x16 : kind;
}

// The AS3 builtin namespace: methods invoked as obj.AS3::meth() by
// strict-mode compilers. Our builtins register public keys only, so both
// fold to public for matching purposes.
static const char AS3_NS[] = "http://adobe.com/AS3/2006/builtin";

static int key_ns_is_public(uint8_t folded_kind, const char* uri, uint32_t len)
{
	if (folded_kind != 0x16) return 0;
	if (len == 0) return 1;
	return len == sizeof(AS3_NS) - 1 && memcmp(uri, AS3_NS, len) == 0;
}

int avm2_propkey_is_public(const Avm2PropKey* k)
{
	// STRICT: kind public with an empty URI. (The AS3 builtin ns unifies
	// with public only inside propkey_matches, for method dispatch.)
	return avm2_ns_fold(k->ns_kind) == 0x16 && k->ns_len == 0;
}

Avm2PropKey avm2_public_key(const char* name, uint32_t name_len)
{
	Avm2PropKey k;
	k.name = name;
	k.name_len = name_len;
	k.ns_kind = 0x16;
	k.ns_uri = "";
	k.ns_len = 0;
	k.ns_priv = NULL;
	return k;
}

void avm2_key_ns_from_abc(Avm2PropKey* k, const Avm2AbcFileData* data, uint32_t ns_idx)
{
	const Avm2AbcNamespace* ns = &data->namespaces[ns_idx];
	k->ns_kind = ns->kind;
	k->ns_uri = data->strings[ns->name].utf8;
	k->ns_len = data->strings[ns->name].len;
	// The pool record's address is the private namespace's identity.
	k->ns_priv = (ns->kind == 0x05) ? (const void*) ns : NULL;
}

int avm2_propkey_matches(const Avm2PropKey* a, const Avm2PropKey* b)
{
	if (a->name_len != b->name_len) return 0;
	if (memcmp(a->name, b->name, a->name_len) != 0) return 0;
	uint8_t ka = avm2_ns_fold(a->ns_kind);
	uint8_t kb = avm2_ns_fold(b->ns_kind);
	// AS3-builtin ns unifies with public (both directions).
	int pa = key_ns_is_public(ka, a->ns_uri, a->ns_len);
	int pb = key_ns_is_public(kb, b->ns_uri, b->ns_len);
	if (pa || pb) return pa && pb;
	if (ka != kb) return 0;
	// Private namespaces are compared by pool-entry identity (Ruffle
	// namespace.rs: "private namespaces are always compared by pointer
	// identity"). Their URIs are all empty, so a URI-only compare would let
	// Class2 read Class1's `private var`.
	if (ka == 0x05) return a->ns_priv == b->ns_priv;
	if (a->ns_len != b->ns_len) return 0;
	if (a->ns_len > 0 && memcmp(a->ns_uri, b->ns_uri, a->ns_len) != 0) return 0;
	return 1;
}

static void propkey_from_parts(const Avm2AbcFileData* data, uint32_t ns_idx,
                               uint32_t name_idx, Avm2PropKey* out)
{
	out->name = data->strings[name_idx].utf8;
	out->name_len = data->strings[name_idx].len;
	avm2_key_ns_from_abc(out, data, ns_idx);
}

int avm2_propkey_from_qname(const Avm2AbcFileData* data, uint32_t mn_idx, Avm2PropKey* out)
{
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
	if (mn->kind == 0x07 || mn->kind == 0x0d)  // QName / QNameA
	{
		propkey_from_parts(data, mn->ns, mn->name, out);
		return 1;
	}
	if (mn->kind == 0x09 || mn->kind == 0x0e)  // Multiname with a 1-ns set
	{
		const Avm2AbcNsSet* set = &data->ns_sets[mn->ns_set];
		if (set->count == 1)
		{
			propkey_from_parts(data, set->ns_indices[0], mn->name, out);
			return 1;
		}
	}
	return 0;
}

void avm2_mn_name(const Avm2AbcFileData* data, uint32_t mn_idx,
                  const char** name, uint32_t* name_len)
{
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
	*name = data->strings[mn->name].utf8;
	*name_len = data->strings[mn->name].len;
}

int avm2_mn_has_public_ns(const Avm2AbcFileData* data, uint32_t mn_idx)
{
	// STRICT publicness (empty-URI Namespace/Package): the AS3 builtin ns
	// does NOT enable dynamic-property access (array_access_interpreter's
	// JIT probe expects 1081 with an AS3-only ns set).
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
	switch (mn->kind)
	{
		case 0x07:
		case 0x0d:
		{
			const Avm2AbcNamespace* ns = &data->namespaces[mn->ns];
			return avm2_ns_fold(ns->kind) == 0x16
			       && data->strings[ns->name].len == 0;
		}
		case 0x09:
		case 0x0e:
		case 0x1b:
		case 0x1c:
		{
			const Avm2AbcNsSet* set = &data->ns_sets[mn->ns_set];
			for (uint32_t i = 0; i < set->count; i++)
			{
				const Avm2AbcNamespace* ns = &data->namespaces[set->ns_indices[i]];
				if (avm2_ns_fold(ns->kind) == 0x16
				    && data->strings[ns->name].len == 0)
				{
					return 1;
				}
			}
			return 0;
		}
		default:
			// Lazy-name kinds without a set: RTQName* — treat as public.
			return 1;
	}
}

int avm2_mn_match(const Avm2AbcFileData* data, uint32_t mn_idx, const Avm2PropKey* key)
{
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
	switch (mn->kind)
	{
		case 0x07:  // QName
		case 0x0d:  // QNameA
		{
			Avm2PropKey k;
			propkey_from_parts(data, mn->ns, mn->name, &k);
			return avm2_propkey_matches(&k, key);
		}
		case 0x09:  // Multiname (static name + ns set)
		case 0x0e:  // MultinameA
		{
			const Avm2String* name = &data->strings[mn->name];
			if (name->len != key->name_len
			    || memcmp(name->utf8, key->name, name->len) != 0)
			{
				return 0;
			}
			const Avm2AbcNsSet* set = &data->ns_sets[mn->ns_set];
			for (uint32_t i = 0; i < set->count; i++)
			{
				Avm2PropKey k;
				propkey_from_parts(data, set->ns_indices[i], mn->name, &k);
				if (avm2_propkey_matches(&k, key)) return 1;
			}
			return 0;
		}
		default:
			// Lazy (runtime) multinames are resolved by the *_dyn op
			// helpers before matching.
			return 0;
	}
}

// ---------------------------------------------------------------------------
// VTable
// ---------------------------------------------------------------------------

void avm2_vtable_append(Avm2Context* ctx, Avm2VTable* vt, const Avm2PropEntry* e)
{
	if (vt->count == vt->cap)
	{
		uint32_t new_cap = vt->cap == 0 ? 8 : vt->cap * 2;
		Avm2PropEntry* grown = avm2_alloc(ctx, new_cap * sizeof(Avm2PropEntry));
		if (vt->count > 0)
		{
			memcpy(grown, vt->entries, vt->count * sizeof(Avm2PropEntry));
		}
		vt->entries = grown;
		vt->cap = new_cap;
	}
	vt->entries[vt->count++] = *e;
}

// ---------------------------------------------------------------------------
// VTable name-hash accelerator
//
// Each find matches on a full predicate (avm2_propkey_matches / avm2_mn_match)
// that ALWAYS requires the entry's local name to equal the query name. So a
// name-keyed hash lets us visit only entries whose name matches, shrinking the
// O(count) linear scan to O(bucket) (~1). The bucket chain is built by walking
// entries HIGH->LOW and prepending, so iteration yields ASCENDING entry index
// — identical to a forward linear scan, preserving "first match wins" order
// (matters for multiname ns-set lookups where several namespaces could hold
// the name). The index is a pure derived cache: it stores entry indices only
// (GC-invisible), and the same match predicate still runs on each candidate,
// so behavior is byte-identical to the linear scan.
// ---------------------------------------------------------------------------

#define VT_INDEX_MIN 8u  // below this, a linear scan is already cheap

typedef struct VtIndexNode
{
	uint32_t hash;
	uint32_t entry;
	int32_t next;
} VtIndexNode;

typedef struct Avm2VTableIndex
{
	uint32_t mask;      // nbuckets - 1 (power of two)
	int32_t* buckets;   // [mask+1]; -1 = empty, else head node index
	VtIndexNode* nodes; // [count]
} Avm2VTableIndex;

static uint32_t vt_name_hash(const char* s, uint32_t n)
{
	uint32_t h = 2166136261u;  // FNV-1a
	for (uint32_t i = 0; i < n; i++)
	{
		h ^= (uint8_t) s[i];
		h *= 16777619u;
	}
	return h;
}

static void vt_index_build(Avm2VTable* vt)
{
	Avm2VTableIndex* ix = (Avm2VTableIndex*) vt->name_index;
	if (ix != NULL)
	{
		free(ix->buckets);
		free(ix->nodes);
		free(ix);
	}
	uint32_t nb = 8;
	while (nb < vt->count * 2) nb <<= 1;
	ix = (Avm2VTableIndex*) malloc(sizeof(Avm2VTableIndex));
	ix->mask = nb - 1;
	ix->buckets = (int32_t*) malloc(nb * sizeof(int32_t));
	ix->nodes = (VtIndexNode*) malloc((vt->count ? vt->count : 1) * sizeof(VtIndexNode));
	for (uint32_t b = 0; b < nb; b++) ix->buckets[b] = -1;
	// Walk high->low so equal-name entries chain in ascending index order.
	for (uint32_t i = vt->count; i-- > 0; )
	{
		const Avm2PropKey* k = &vt->entries[i].key;
		uint32_t h = vt_name_hash(k->name, k->name_len);
		uint32_t b = h & ix->mask;
		ix->nodes[i].hash = h;
		ix->nodes[i].entry = i;
		ix->nodes[i].next = ix->buckets[b];
		ix->buckets[b] = (int32_t) i;
	}
	vt->name_index = ix;
	vt->indexed_count = vt->count;
}

// Get the (lazily (re)built) index, or NULL if this vtable should stay linear.
static const Avm2VTableIndex* vt_index_get(const Avm2VTable* vt)
{
	if (vt->no_index || vt->count < VT_INDEX_MIN) return NULL;
	Avm2VTable* mvt = (Avm2VTable*) vt;  // mutable cache on a const object
	if (mvt->name_index == NULL || mvt->indexed_count != mvt->count)
	{
		vt_index_build(mvt);
	}
	return (const Avm2VTableIndex*) mvt->name_index;
}

const Avm2PropEntry* avm2_vtable_find(const Avm2VTable* vt, const Avm2PropKey* key)
{
	if (vt == NULL) return NULL;
	const Avm2VTableIndex* ix = vt_index_get(vt);
	if (ix != NULL)
	{
		uint32_t h = vt_name_hash(key->name, key->name_len);
		for (int32_t n = ix->buckets[h & ix->mask]; n >= 0; n = ix->nodes[n].next)
		{
			if (ix->nodes[n].hash != h) continue;
			const Avm2PropEntry* e = &vt->entries[ix->nodes[n].entry];
			if (avm2_propkey_matches(&e->key, key)) return e;
		}
		return NULL;
	}
	for (uint32_t i = 0; i < vt->count; i++)
	{
		if (avm2_propkey_matches(&vt->entries[i].key, key))
		{
			return &vt->entries[i];
		}
	}
	return NULL;
}

// A multiname carrying a namespace SET can match SEVERAL vtable entries at
// once, because a class may declare the same NAME in two different namespaces
// that the site's set both contains. Flash resolves that PER VTABLE, walking
// the class chain most-derived first; our vtable is flattened, so the tie is
// broken on the entry's DEFINING CLASS instead. Two corpus tests pin the two
// directions and only this rule satisfies both:
//
//   property_priority_chained          base: field1 internal / field2 public
//                                      sub:  field1 public   / field2 internal
//     -> the SUBCLASS declaration wins, even though the base's entry comes
//        first in the flattened order.
//   getter_different_namespace_setter  base: getter virtualprop ns A
//                                            setter virtualprop ns B
//                                      sub:  getter virtualprop ns A (override)
//     -> a WRITE must find the subclass's getter-only entry and throw #1074,
//        NOT the base's setter in the other namespace. Ruffle gets this one
//        wrong (`known_failure = true`), and its test.toml names the reason:
//        "correct handling requires per-vtable lookup; with flattened vtables,
//        overridden traits would need to be hackily promoted ahead of
//        inherited traits" — i.e. plain "last entry wins" is the hack, and it
//        breaks exactly this case.
//
// So: keep the match whose defining class is most derived, and on a TIE keep
// the FIRST — which leaves every single-match lookup (the overwhelming
// majority, and all builtins, whose entries carry no defining class) behaving
// exactly as before. Exact-key lookups (avm2_vtable_find) never multi-match.
static int mn_kind_is_ns_set(const Avm2AbcFileData* data, uint32_t mn_idx)
{
	uint8_t k = data->multinames[mn_idx].kind;
	return k == 0x09 || k == 0x0e || k == 0x1b || k == 0x1c;
}

// Depth of `cls` below the root (Object = 0). NULL (a builtin//native entry
// with no declaring ABC class) sorts lowest so it never displaces a real one.
static int class_derive_depth(const Avm2Class* cls)
{
	int d = 0;
	for (const Avm2Class* c = cls; c != NULL; c = c->super_class)
	{
		if (++d > 256) break;   // cycle guard
	}
	return d;
}

// Should `cand` displace the currently-held `cur` for a ns-set match?
static int entry_more_derived(const Avm2PropEntry* cand, const Avm2PropEntry* cur)
{
	if (cur == NULL) return 1;
	return class_derive_depth(cand->defining_class)
	     > class_derive_depth(cur->defining_class);
}

const Avm2PropEntry* avm2_vtable_find_mn(const Avm2VTable* vt, const Avm2AbcFileData* data,
                                         uint32_t mn_idx)
{
	if (vt == NULL) return NULL;
	const int multi = mn_kind_is_ns_set(data, mn_idx);
	const Avm2PropEntry* found = NULL;
	const Avm2VTableIndex* ix = vt_index_get(vt);
	if (ix != NULL)
	{
		const char* name;
		uint32_t name_len;
		avm2_mn_name(data, mn_idx, &name, &name_len);
		uint32_t h = vt_name_hash(name, name_len);
		for (int32_t n = ix->buckets[h & ix->mask]; n >= 0; n = ix->nodes[n].next)
		{
			if (ix->nodes[n].hash != h) continue;
			const Avm2PropEntry* e = &vt->entries[ix->nodes[n].entry];
			if (!avm2_mn_match(data, mn_idx, &e->key)) continue;
			if (!multi) return e;
			if (entry_more_derived(e, found)) found = e;
		}
		return found;
	}
	for (uint32_t i = 0; i < vt->count; i++)
	{
		if (!avm2_mn_match(data, mn_idx, &vt->entries[i].key)) continue;
		if (!multi) return &vt->entries[i];
		if (entry_more_derived(&vt->entries[i], found)) found = &vt->entries[i];
	}
	return found;
}

const Avm2PropEntry* avm2_vtable_find_public(const Avm2VTable* vt,
                                             const char* name, uint32_t name_len)
{
	if (vt == NULL) return NULL;
	const Avm2VTableIndex* ix = vt_index_get(vt);
	if (ix != NULL)
	{
		uint32_t h = vt_name_hash(name, name_len);
		for (int32_t n = ix->buckets[h & ix->mask]; n >= 0; n = ix->nodes[n].next)
		{
			if (ix->nodes[n].hash != h) continue;
			const Avm2PropEntry* e = &vt->entries[ix->nodes[n].entry];
			if (e->key.name_len == name_len
			    && memcmp(e->key.name, name, name_len) == 0
			    && avm2_propkey_is_public(&e->key))
			{
				return e;
			}
		}
		return NULL;
	}
	for (uint32_t i = 0; i < vt->count; i++)
	{
		const Avm2PropEntry* e = &vt->entries[i];
		if (e->key.name_len == name_len
		    && memcmp(e->key.name, name, name_len) == 0
		    && avm2_propkey_is_public(&e->key))
		{
			return e;
		}
	}
	return NULL;
}

const Avm2PropEntry* avm2_vtable_find_mn_named(const Avm2VTable* vt,
                                               const Avm2AbcFileData* data,
                                               uint32_t mn_idx,
                                               const char* name, uint32_t name_len)
{
	if (vt == NULL) return NULL;
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];
	uint32_t single_ns = 0;
	const Avm2AbcNsSet* set = NULL;
	switch (mn->kind)
	{
		case 0x1b: case 0x1c:  // MultinameL / MultinameLA
			set = &data->ns_sets[mn->ns_set];
			break;
		case 0x07: case 0x0d:  // QName kinds (name replaced at runtime)
			single_ns = mn->ns;
			break;
		default:
			return NULL;
	}
	uint32_t count = (set != NULL) ? set->count : 1;
	// Hash the (single, runtime) name ONCE and walk its bucket once, testing
	// each same-name entry against the whole ns set — the ns sets ASC emits
	// for `obj[expr]` inside a class run to 7-9 namespaces, and this op sits
	// on the dynamic-property fast path, so a find-per-namespace would mean
	// 9 hashes + 9 bucket walks for what is one bucket's worth of work.
	// Set matches take the LAST hit (most-derived wins) — see the comment on
	// avm2_vtable_find_mn.
	const int multi = (set != NULL);
	const Avm2PropEntry* found = NULL;
	const Avm2VTableIndex* ix = vt_index_get(vt);
	if (ix != NULL)
	{
		uint32_t h = vt_name_hash(name, name_len);
		for (int32_t n = ix->buckets[h & ix->mask]; n >= 0; n = ix->nodes[n].next)
		{
			if (ix->nodes[n].hash != h) continue;
			const Avm2PropEntry* e = &vt->entries[ix->nodes[n].entry];
			if (e->key.name_len != name_len
			    || memcmp(e->key.name, name, name_len) != 0) continue;
			for (uint32_t i = 0; i < count; i++)
			{
				uint32_t ns_idx = (set != NULL) ? set->ns_indices[i] : single_ns;
				Avm2PropKey key;
				key.name = name;
				key.name_len = name_len;
				avm2_key_ns_from_abc(&key, data, ns_idx);
				if (avm2_propkey_matches(&e->key, &key))
				{
					if (!multi) return e;
					if (entry_more_derived(e, found)) found = e;
					break;
				}
			}
		}
		return found;
	}
	for (uint32_t i = 0; i < vt->count; i++)
	{
		const Avm2PropEntry* e = &vt->entries[i];
		if (e->key.name_len != name_len
		    || memcmp(e->key.name, name, name_len) != 0) continue;
		for (uint32_t j = 0; j < count; j++)
		{
			uint32_t ns_idx = (set != NULL) ? set->ns_indices[j] : single_ns;
			Avm2PropKey key;
			key.name = name;
			key.name_len = name_len;
			avm2_key_ns_from_abc(&key, data, ns_idx);
			if (avm2_propkey_matches(&e->key, &key))
			{
				if (!multi) return e;
				if (entry_more_derived(e, found)) found = e;
				break;
			}
		}
	}
	return found;
}

static Avm2PropEntry* vtable_find_mut(Avm2VTable* vt, const Avm2PropKey* key)
{
	for (uint32_t i = 0; i < vt->count; i++)
	{
		if (avm2_propkey_matches(&vt->entries[i].key, key))
		{
			return &vt->entries[i];
		}
	}
	return NULL;
}

// Record slot metadata for slot id `idx` (grows the table).
static void vtable_set_slot_meta(Avm2Context* ctx, Avm2VTable* vt, uint32_t idx,
                                 const Avm2SlotMeta* meta)
{
	if (idx >= vt->meta_cap)
	{
		uint32_t new_cap = vt->meta_cap == 0 ? 8 : vt->meta_cap;
		while (new_cap <= idx) new_cap *= 2;
		Avm2SlotMeta* grown = avm2_alloc(ctx, new_cap * sizeof(Avm2SlotMeta));
		memset(grown, 0, new_cap * sizeof(Avm2SlotMeta));
		if (vt->meta_cap > 0)
		{
			memcpy(grown, vt->metas, vt->meta_cap * sizeof(Avm2SlotMeta));
		}
		vt->metas = grown;
		vt->meta_cap = new_cap;
	}
	vt->metas[idx] = *meta;
}

void avm2_vtable_add_traits(Avm2Context* ctx, Avm2VTable* vt, Avm2AbcFileRt* file,
                            const Avm2AbcTrait* traits, uint32_t trait_count,
                            Avm2Class* defining_class, Avm2ScopeChain* scope)
{
	for (uint32_t i = 0; i < trait_count; i++)
	{
		const Avm2AbcTrait* t = &traits[i];
		Avm2PropEntry e;
		memset(&e, 0, sizeof(e));
		if (!avm2_propkey_from_qname(file->data, t->name_mn, &e.key))
		{
			avm2_fatal("AVM2: trait name multiname %u is not a QName", t->name_mn);
		}
		e.defining_class = defining_class;
		e.method_scope = scope;
		// Trait metadata rides along for describeType. The two accessor
		// merge paths below re-route it onto the surviving entry's matching
		// half; the override path copies the whole entry, so an override's
		// metadata correctly REPLACES the parent's.
		e.metadata = t->metadata;
		e.metadata_count = t->metadata_count;
		e.metadata_file = (t->metadata_count > 0) ? file : NULL;
		if (t->kind == 3)  // Setter: its metadata is the setter half's
		{
			e.setter_metadata = t->metadata;
			e.setter_metadata_count = t->metadata_count;
			e.setter_metadata_file = e.metadata_file;
			e.metadata = NULL;
			e.metadata_count = 0;
			e.metadata_file = NULL;
		}

		switch (t->kind)
		{
			case 0:  // Slot
			case 4:  // Class
			case 5:  // Function
			case 6:  // Const
			{
				e.kind = AVM2_PROP_SLOT;
				// Class traits are READ-ONLY, like Const (Ruffle vtable.rs:
				// `TraitKind::Const | TraitKind::Class => new_const_slot`).
				// Without this, `Object = new Object()` at script scope
				// silently overwrites the global's class slot instead of
				// throwing #1074 -- and then every later script init that
				// pushes `Object` as a newclass base gets a plain object.
				e.is_const = (t->kind == 6 || t->kind == 4);
				uint32_t slot_id = t->slot_or_disp_id;
				if (slot_id == 0)
				{
					slot_id = vt->slot_count + 1;  // auto-assign
				}
				e.slot_index = slot_id;
				e.type_mn = t->type_mn;
				e.type_file = file;
				e.value = t->value;
				{
					Avm2SlotMeta meta;
					memset(&meta, 0, sizeof(meta));
					meta.used = 1;
					meta.is_class_trait = (t->kind == 4);
					meta.type_mn = t->type_mn;
					meta.type_file = file;
					meta.value = t->value;
					meta.defining_class = defining_class;
					meta.method_scope = scope;
					if (t->kind == 5)
					{
						const Avm2AbcMethodData* fm = &file->data->methods[t->method_or_class];
						meta.is_function_trait = 1;
						meta.fn_method.fn = fm->fn;
						meta.fn_method.file = file;
						meta.fn_method.debug_name = fm->debug_name;
						meta.fn_method.method_index = t->method_or_class;
					}
					vtable_set_slot_meta(ctx, vt, slot_id, &meta);
				}
				if (t->kind == 5)
				{
					// Function trait: the slot's default is a closure over
					// the entry's scope (realized in slots_init_defaults).
					const Avm2AbcMethodData* m = &file->data->methods[t->method_or_class];
					e.is_function_trait = 1;
					e.method.fn = m->fn;
					e.method.file = file;
					e.method.debug_name = m->debug_name;
					e.method.method_index = t->method_or_class;
				}
				if (slot_id > vt->slot_count) vt->slot_count = slot_id;
				break;
			}
			case 1:  // Method
			case 2:  // Getter
			case 3:  // Setter
			{
				const Avm2AbcMethodData* m = &file->data->methods[t->method_or_class];
				// FIRST-BINDING record (avmplus MethodEnv). A MethodInfo gets
				// exactly one MethodEnv — the one made where it was first
				// bound — and `callstatic` dispatches through it, so the
				// callee's scope is the one captured HERE, not the caller's.
				// First writer wins: when a class trait is NewClass'ed twice,
				// both class objects share the FIRST one's captured scope
				// (avm2/getouterscope_two_classobjects prints 50 four times).
				if (scope != NULL && file->method_env_scope != NULL
				    && t->method_or_class < file->data->method_count
				    && file->method_env_scope[t->method_or_class] == NULL)
				{
					file->method_env_scope[t->method_or_class] = scope;
					file->method_env_class[t->method_or_class] = defining_class;
				}
				Avm2MethodRef ref;
				ref.fn = m->fn;
				ref.file = file;
				ref.debug_name = m->debug_name;
				ref.method_index = t->method_or_class;
				// avmplus names a method by its defining trait. ASC (unlike
				// mxmlc) emits no method_info debug name, so without this the
				// frame reads "Class/<anonymous>()"; fall back to the trait
				// QName (null-terminated for %s), with the "get "/"set "
				// prefix FP uses for accessors.
				if ((ref.debug_name == NULL || ref.debug_name[0] == '\0')
				    && e.key.name != NULL && e.key.name_len > 0)
				{
					const char* pfx = (t->kind == 2) ? "get "
					                  : (t->kind == 3) ? "set " : "";
					size_t pl = strlen(pfx);
					// A trait declared in a USER namespace (ABC kind 0x08,
					// `namespace my_ns = "uri"`) is named by its QUALIFIED
					// name — "Test/uri::f()", not "Test/f()" (Ruffle
					// function.rs: `method_trait.name().namespace()
					// .is_namespace()` -> to_qualified_name). Package /
					// package-internal / protected / private traits (0x16-
					// 0x1a, 0x05) keep the local name.
					const char* uri = NULL;
					size_t ul = 0;
					if (e.key.ns_kind == 0x08 && e.key.ns_uri != NULL
					    && e.key.ns_len > 0)
					{
						uri = e.key.ns_uri;
						ul = e.key.ns_len;
					}
					size_t sep = (ul > 0) ? 2 : 0;
					char* nm = avm2_alloc(ctx, pl + ul + sep + e.key.name_len + 1);
					memcpy(nm, pfx, pl);
					if (ul > 0)
					{
						memcpy(nm + pl, uri, ul);
						nm[pl + ul] = ':';
						nm[pl + ul + 1] = ':';
					}
					memcpy(nm + pl + ul + sep, e.key.name, e.key.name_len);
					nm[pl + ul + sep + e.key.name_len] = '\0';
					ref.debug_name = nm;
				}

				Avm2PropEntry* existing = vtable_find_mut(vt, &e.key);
				if (t->kind == 1)
				{
					e.kind = AVM2_PROP_METHOD;
					e.method = ref;
				}
				else if (t->kind == 2)
				{
					// Merge with an inherited/sibling setter.
					if (existing != NULL && (existing->kind == AVM2_PROP_SETTER
					                         || existing->kind == AVM2_PROP_GETSET))
					{
						// The setter half we are merging onto keeps ITS
						// declaring class: this getter override does not
						// re-bind it.
						if (existing->setter_defining_class == NULL)
						{
							existing->setter_defining_class = existing->defining_class;
							existing->setter_scope = existing->method_scope;
						}
						existing->kind = AVM2_PROP_GETSET;
						existing->method = ref;
						existing->defining_class = defining_class;
						existing->method_scope = scope;
						existing->metadata = e.metadata;
						existing->metadata_count = e.metadata_count;
						existing->metadata_file = e.metadata_file;
						continue;
					}
					e.kind = AVM2_PROP_GETTER;
					e.method = ref;
				}
				else
				{
					if (existing != NULL && (existing->kind == AVM2_PROP_GETTER
					                         || existing->kind == AVM2_PROP_GETSET))
					{
						existing->kind = AVM2_PROP_GETSET;
						existing->setter = ref;
						// This half is ours even when the getter half
						// stays the inherited one.
						existing->setter_defining_class = defining_class;
						existing->setter_scope = scope;
						existing->setter_metadata = e.setter_metadata;
						existing->setter_metadata_count = e.setter_metadata_count;
						existing->setter_metadata_file = e.setter_metadata_file;
						continue;
					}
					e.kind = AVM2_PROP_SETTER;
					e.setter = ref;
				}
				break;
			}
			default:
				avm2_fatal("AVM2: unknown trait kind %u", t->kind);
		}

		// Overrides replace the inherited entry, keeping the parent's slot
		// index (slot_disp_id_shared_numbering).
		Avm2PropEntry* existing = vtable_find_mut(vt, &e.key);
		if (existing != NULL)
		{
			// NOTE: a redeclared var/const/class slot keeps its NEW slot
			// index (Ruffle vtable.rs always appends) — the parent's slot
			// storage survives for GetSuper/SetSuper shadowing
			// (class_supercalls_errors).
			Avm2PropEntry old = *existing;
			Avm2PropKey saved_key = existing->key;
			*existing = e;
			existing->key = saved_key;
			// Protected/interface members are aliased under other
			// namespaces (ancestor protected ns, interface ns); an
			// override must update every alias that shared the old
			// implementation.
			for (uint32_t j = 0; j < vt->count; j++)
			{
				Avm2PropEntry* o = &vt->entries[j];
				if (o == existing) continue;
				if (o->key.name_len != e.key.name_len) continue;
				if (memcmp(o->key.name, e.key.name, e.key.name_len) != 0) continue;
				if (o->kind != old.kind) continue;
				if (o->method.fn == old.method.fn
				    && o->method.file == old.method.file
				    && o->setter.fn == old.setter.fn)
				{
					Avm2PropKey k2 = o->key;
					*o = e;
					o->key = k2;
				}
			}
		}
		else
		{
			avm2_vtable_append(ctx, vt, &e);
		}
	}
}

// Slot defaults: trait value if present, else the type's default.
static Avm2Value slot_type_default(Avm2Context* ctx, const Avm2PropEntry* e)
{
	if (e->type_mn == 0 || e->type_file == NULL)
	{
		return avm2_undefined();
	}
	const char* tn;
	uint32_t tn_len;
	avm2_mn_name(e->type_file->data, e->type_mn, &tn, &tn_len);
	if (tn_len == 3 && memcmp(tn, "int", 3) == 0) return avm2_integer(0);
	if (tn_len == 4 && memcmp(tn, "uint", 4) == 0) return avm2_integer(0);
	if (tn_len == 6 && memcmp(tn, "Number", 6) == 0) return avm2_number(__builtin_nan(""));
	if (tn_len == 7 && memcmp(tn, "Boolean", 7) == 0) return avm2_bool(false);
	if (tn_len == 1 && tn[0] == '*') return avm2_undefined();
	return avm2_null();
}

Avm2Value avm2_default_value(Avm2Context* ctx, Avm2AbcFileRt* file, const Avm2AbcDefault* d)
{
	const Avm2AbcFileData* data = file->data;
	switch (d->kind)
	{
		case 0x01: return avm2_string(&data->strings[d->index]);
		case 0x03: return avm2_integer(data->ints[d->index]);
		case 0x04: return avm2_uint_value(data->uints[d->index]);
		case 0x06: return avm2_number(data->doubles[d->index]);
		case 0x0a: return avm2_bool(false);
		case 0x0b: return avm2_bool(true);
		case 0x0c: return avm2_null();
		case 0x00: return avm2_undefined();
		// Namespace kinds (`namespace n = "uri"` consts): box the pool
		// namespace as a Namespace object.
		case 0x05: case 0x08: case 0x16: case 0x17: case 0x18: case 0x19:
		case 0x1a:
			return avm2_object_value(avm2_namespace_from_pool(ctx, file, d->index));
		default:
			return avm2_undefined();
	}
}

// The default value one slot's meta produces. Factored out of the old
// per-construction loop so the template builder and the patch loop share it
// verbatim — there is exactly one definition of "what does this slot default
// to", which is what makes the template provably equivalent.
static Avm2Value slot_default_for(Avm2Context* ctx, const Avm2SlotMeta* m)
{
	if (m->value.has_value && m->type_file != NULL)
	{
		Avm2Value dv = avm2_default_value(ctx, m->type_file, &m->value);
		// Coerce the constant to the slot's declared type (int slots with a
		// double initializer, etc.).
		if (m->type_mn != 0)
		{
			dv = avm2_coerce_to_type_mn(ctx, m->type_file, m->type_mn, dv);
		}
		return dv;
	}
	if (m->is_function_trait)
	{
		return avm2_object_value(
			avm2_function_new(ctx, &m->fn_method, m->defining_class,
			                  m->method_scope, avm2_undefined(), false));
	}
	if (m->type_mn != 0 && m->type_file != NULL)
	{
		Avm2PropEntry tmp;
		memset(&tmp, 0, sizeof(tmp));
		tmp.type_mn = m->type_mn;
		tmp.type_file = m->type_file;
		return slot_type_default(ctx, &tmp);
	}
	// A Class trait's slot holds `null` until its script init NewClass'es into
	// it (Ruffle vtable.rs `TraitKind::Class { .. } => Value::Null`). It has no
	// declared type, so it would otherwise land on `undefined` below.
	if (m->is_class_trait)
	{
		return avm2_null();
	}
	return avm2_undefined();
}

#ifndef AVM2_NO_SLOT_TPL
// Does this slot have to be realized per object rather than templated?
// Decided on the META, never by computing the value, because computing a
// function-trait or namespace default has SIDE EFFECTS (it allocates a
// closure / Namespace object) that must happen once per object, not once per
// class.
static int slot_needs_realization(const Avm2SlotMeta* m)
{
	if (m->is_function_trait) return 1;
	if (m->value.has_value && m->type_file != NULL)
	{
		uint8_t k = m->value.kind;
		// 0x01 string (static pool) and the namespace kinds are the
		// pointer-valued constant defaults. Strings are provably immortal
		// rodata and could in principle be templated, but the census says
		// they are 314 boot-time slot inits out of 96.7M — zero upside for
		// a weaker invariant, so they stay in the patch loop and the
		// template's "no pointers, ever" rule stays absolute.
		if (k == 0x01 || k == 0x05 || k == 0x08 || (k >= 0x16 && k <= 0x1a))
		{
			return 1;
		}
	}
	return 0;
}

static void slot_tpl_build(Avm2Context* ctx, Avm2VTable* vt)
{
	uint32_t n = vt->slot_count;
	Avm2Value* tpl = avm2_alloc(ctx, (n + 1) * (uint32_t) sizeof(Avm2Value));
	uint32_t* patch = avm2_alloc(ctx, (n + 1) * (uint32_t) sizeof(uint32_t));
	// Zero == undefined, which is also what an unused/absent meta leaves.
	memset(tpl, 0, (n + 1) * sizeof(Avm2Value));
	uint32_t pn = 0;
	for (uint32_t s = 1; s <= n; s++)
	{
		if (s >= vt->meta_cap || !vt->metas[s].used) continue;
		const Avm2SlotMeta* m = &vt->metas[s];
		if (slot_needs_realization(m))
		{
			patch[pn++] = s;
			continue;
		}
		Avm2Value dv = slot_default_for(ctx, m);
		// Backstop for the meta-side classification: if a default somehow
		// still produced a traced pointer, refuse to template it.
		if (dv.kind == AVM2_VALUE_STRING || dv.kind == AVM2_VALUE_OBJECT)
		{
			patch[pn++] = s;
			continue;
		}
		tpl[s] = dv;
	}
	vt->slot_tpl = tpl;
	vt->slot_patch = patch;
	vt->slot_patch_n = pn;
	vt->tpl_slot_n = n;
	vt->tpl_built = 1;
}
#endif

void avm2_slots_init_defaults(Avm2Context* ctx, Avm2Object* obj, const Avm2VTable* vt)
{
	if (vt == NULL) return;
	// The slot-meta table covers every slot ever allocated, including
	// parent slots shadowed by a subclass redeclaration.
	uint32_t n = vt->slot_count;
	if (obj->slot_count > 0 && n > obj->slot_count - 1) n = obj->slot_count - 1;
	if (n == 0) return;

#ifndef AVM2_NO_SLOT_TPL
	// Shared class vtables only: a no_index vtable belongs to a single
	// newactivation/newcatch object, so a template would be built and thrown
	// away once per call.
	if (!vt->no_index)
	{
		Avm2VTable* mvt = (Avm2VTable*) vt;
		// Rebuilt if the vtable grew slots since the template was built
		// (class define appends traits; constructions only start after).
		if (!mvt->tpl_built || mvt->tpl_slot_n != vt->slot_count)
		{
			slot_tpl_build(ctx, mvt);
		}
		memcpy(&obj->slots[1], &vt->slot_tpl[1], n * sizeof(Avm2Value));
		for (uint32_t i = 0; i < vt->slot_patch_n; i++)
		{
			uint32_t s = vt->slot_patch[i];
			if (s > n) continue;
			obj->slots[s] = slot_default_for(ctx, &vt->metas[s]);
		}
#ifdef AVM2_SLOTTPL_VERIFY
		// Cross-check the templated image against the reference loop for
		// every construction. The reference is re-run into scratch AFTER
		// the fast path so realization side effects (fresh closures) are
		// compared by kind, not identity.
		for (uint32_t s = 1; s <= n; s++)
		{
			Avm2Value want = (s < vt->meta_cap && vt->metas[s].used)
				? slot_default_for(ctx, &vt->metas[s]) : avm2_undefined();
			Avm2Value got = obj->slots[s];
			int ok = (want.kind == got.kind);
			if (ok && want.kind != AVM2_VALUE_OBJECT
			    && memcmp(&want, &got, sizeof(Avm2Value)) != 0)
			{
				ok = 0;
			}
			if (!ok)
			{
				avm2_fatal("AVM2_SLOTTPL_VERIFY: slot %u kind %u != %u",
				           s, got.kind, want.kind);
			}
		}
#endif
		return;
	}
#endif

	for (uint32_t s = 1; s <= n; s++)
	{
		if (s >= vt->meta_cap || !vt->metas[s].used) continue;
		obj->slots[s] = slot_default_for(ctx, &vt->metas[s]);
	}
}

// ---------------------------------------------------------------------------
// Scope chains
// ---------------------------------------------------------------------------

Avm2ScopeChain* avm2_scope_capture(Avm2Context* ctx, const Avm2ScopeChain* outer,
                                   const Avm2ScopeEntry* lscope, uint32_t scope_n)
{
	uint32_t outer_n = (outer != NULL) ? outer->count : 0;
	Avm2ScopeChain* chain = avm2_alloc(ctx, sizeof(Avm2ScopeChain));
	chain->count = outer_n + scope_n;
	chain->entries = NULL;
	if (chain->count > 0)
	{
		chain->entries = avm2_alloc(ctx, chain->count * sizeof(Avm2ScopeEntry));
		for (uint32_t i = 0; i < outer_n; i++) chain->entries[i] = outer->entries[i];
		for (uint32_t i = 0; i < scope_n; i++) chain->entries[outer_n + i] = lscope[i];
	}
	return chain;
}

// ---------------------------------------------------------------------------
// Invocation
// ---------------------------------------------------------------------------

Avm2Value avm2_call_method_ref(Avm2Context* ctx, const Avm2MethodRef* m,
                               Avm2Class* bound_class, Avm2ScopeChain* outer,
                               Avm2Value this_val, const Avm2Value* args, uint32_t argc)
{
	if (m->fn == NULL)
	{
		if (m->file == NULL)
		{
			// Builtin stub with no native body (e.g. Object/Sprite
			// constructors) — deliberate no-op.
			return avm2_undefined();
		}
		// A declared-but-bodyless ABC method: avmplus reports this LATE, at
		// the call, as a catchable VerifyError #1001 (method_without_body
		// catches it and prints e / e.errorID). Not a fatal. The method is
		// named the way a stack frame names it ("Class/method").
		const char* dn = (m->debug_name != NULL && m->debug_name[0] != '\0')
		                 ? m->debug_name : "<anonymous>";
		char qual[224];
		if (bound_class != NULL)
		{
			char cq[128];
			avm2_class_qname_colons_buf(bound_class, cq, sizeof(cq));
			snprintf(qual, sizeof(qual), "%s/%s", cq, dn);
		}
		else
		{
			snprintf(qual, sizeof(qual), "%s", dn);
		}
		avm2_throw_error(ctx, ctx->builtins.verify_error_class,
		                 "Error #1001: The method %s() is not implemented.", qual);
	}
	avm2_stack_check(ctx);
	Avm2Activation act;
	act.ctx = ctx;
	act.file = m->file;
	act.bound_class = bound_class;
	act.outer = outer;
	act.this_val = this_val;
	act.args = args;
	act.argc = argc;
	act.callee = NULL;
	// Default XML namespace propagation (Ruffle activation.rs): callees
	// inherit the caller's dxns; SET_DXNS methods start fresh. Restored on
	// return.
	const Avm2String* saved_dxns = ctx->dxns;
	if (m->file != NULL
	    && (m->file->data->methods[m->method_index].flags & AVM2_METHOD_SET_DXNS) != 0)
	{
		ctx->dxns = NULL;
	}
	avm2_callstack_push(ctx, m, bound_class);
	Avm2Value result = m->fn(&act);
	avm2_callstack_pop(ctx);
	ctx->dxns = saved_dxns;
	return result;
}

Avm2Object* avm2_function_new(Avm2Context* ctx, const Avm2MethodRef* method,
                              Avm2Class* bound_class, Avm2ScopeChain* scope,
                              Avm2Value receiver, bool has_receiver)
{
	Avm2Object* fnobj = avm2_object_alloc(ctx, AVM2_OBJ_FUNCTION, 0);
	fnobj->cls = ctx->builtins.function_class;
	fnobj->vtable = &ctx->builtins.function_class->ivtable;
	fnobj->proto = ctx->builtins.function_class->prototype_obj;
	fnobj->fn_method = *method;
	fnobj->fn_receiver = receiver;
	fnobj->fn_has_receiver = has_receiver;
	fnobj->fn_scope = scope;
	fnobj->fn_bound_class = bound_class;
	return fnobj;
}

Avm2Value avm2_call_function_obj(Avm2Context* ctx, Avm2Object* fnobj,
                                 Avm2Value receiver, const Avm2Value* args, uint32_t argc)
{
	if (fnobj == NULL || fnobj->kind != AVM2_OBJ_FUNCTION)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1006: value is not a function.");
	}
	Avm2Value this_val = fnobj->fn_has_receiver ? fnobj->fn_receiver : receiver;
	if (this_val.kind == AVM2_VALUE_NULL || this_val.kind == AVM2_VALUE_UNDEFINED)
	{
		// A null/undefined `this` binds to the function's global object
		// (the bottom of its captured scope chain).
		if (fnobj->fn_scope != NULL && fnobj->fn_scope->count > 0)
		{
			this_val = avm2_object_value(fnobj->fn_scope->entries[0].obj);
		}
		else
		{
			this_val = avm2_object_value(ctx->builtin_globals);
		}
	}
	if (fnobj->fn_method.fn == NULL && fnobj->fn_method.file == NULL)
	{
		return avm2_undefined();
	}
	avm2_stack_check(ctx);
	Avm2Activation act;
	act.ctx = ctx;
	act.file = fnobj->fn_method.file;
	act.bound_class = fnobj->fn_bound_class;
	act.outer = fnobj->fn_scope;
	act.this_val = this_val;
	act.args = args;
	act.argc = argc;
	act.callee = fnobj;
	// Same default-XML-namespace rule as avm2_call_method_ref above. This is
	// the OTHER of the runtime's two Avm2Activation construction sites, and
	// it is the one script-level `function` declarations and closures are
	// invoked through, so without this a `default xml namespace` statement in
	// such a function would leak out to its caller.
	const Avm2String* saved_dxns = ctx->dxns;
	if (fnobj->fn_method.file != NULL
	    && (fnobj->fn_method.file->data->methods[fnobj->fn_method.method_index].flags
	        & AVM2_METHOD_SET_DXNS) != 0)
	{
		ctx->dxns = NULL;
	}
	avm2_callstack_push(ctx, &fnobj->fn_method, fnobj->fn_bound_class);
	Avm2Value result = fnobj->fn_method.fn(&act);
	avm2_callstack_pop(ctx);
	ctx->dxns = saved_dxns;
	return result;
}

Avm2Value avm2_call_value(Avm2Context* ctx, Avm2Value func, Avm2Value receiver,
                          const Avm2Value* args, uint32_t argc)
{
	if (func.kind == AVM2_VALUE_OBJECT && func.u.obj != NULL)
	{
		Avm2Object* o = func.u.obj;
		if (o->kind == AVM2_OBJ_FUNCTION)
		{
			return avm2_call_function_obj(ctx, o, receiver, args, argc);
		}
		if (o->kind == AVM2_OBJ_CLASS)
		{
			// Class call = coercion (int(x), String(x), ...).
			Avm2Class* cls = o->class_ref;
			if (cls->native_call != NULL)
			{
				return cls->native_call(ctx, cls, args, argc);
			}
			// Calling a class that has no call handler of its own IS the
			// coercion C(x), which takes exactly one argument. avmplus
			// reports any other count as #1112 rather than coercing
			// undefined / ignoring the extras (ecma3/JSON/e15_12_0's
			// `JSON()`, regress/bug_420755's `MyDynamicArray(1,2,3)`).
			// Classes for which a 0- or n-arg call is meaningful --
			// Array, String, Number, Error, Date, RegExp, Vector, XML --
			// all install native_call above and never reach here.
			if (argc != 1)
			{
				avm2_throw_error(ctx, ctx->builtins.argument_error_class,
				                 "Error #1112: Argument count mismatch on class "
				                 "coercion.  Expected 1, got %u.",
				                 argc);
			}
			return avm2_coerce_to_class(ctx, cls, args[0]);
		}
	}
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1006: value is not a function.");
}

// ---------------------------------------------------------------------------
// Class definition + construction
// ---------------------------------------------------------------------------

static void resolve_interfaces(Avm2Context* ctx, Avm2Class* cls);

// Interface-namespace aliases (Ruffle vtable.rs): a call through an
// interface-typed reference uses the interface's namespace, so every
// implemented interface's members must appear on the class vtable under
// the interface namespace, pointing at the class's public implementation.
static void add_iface_aliases_from(Avm2Context* ctx, Avm2Class* cls, Avm2Class* iface)
{
	if (iface == NULL) return;
	for (uint32_t i = 0; i < iface->ivtable.count; i++)
	{
		const Avm2PropEntry* ie = &iface->ivtable.entries[i];
		if (avm2_vtable_find(&cls->ivtable, &ie->key) != NULL) continue;
		Avm2PropKey pub = avm2_public_key(ie->key.name, ie->key.name_len);
		const Avm2PropEntry* own = avm2_vtable_find(&cls->ivtable, &pub);
		if (own == NULL) continue;
		Avm2PropEntry alias = *own;
		alias.key = ie->key;
		alias.is_iface_alias = 1;  // dispatch-only; never enumerated as a trait
		avm2_vtable_append(ctx, &cls->ivtable, &alias);
	}
	resolve_interfaces(ctx, iface);
	for (uint32_t i = 0; i < iface->interface_count; i++)
	{
		if (iface->interfaces != NULL)
		{
			add_iface_aliases_from(ctx, cls, iface->interfaces[i]);
		}
	}
}

static void class_add_interface_aliases(Avm2Context* ctx, Avm2Class* cls)
{
	if (cls->interface_count == 0) return;
	resolve_interfaces(ctx, cls);
	for (uint32_t i = 0; i < cls->interface_count; i++)
	{
		add_iface_aliases_from(ctx, cls, cls->interfaces[i]);
	}
}

static void class_setup_prototype(Avm2Context* ctx, Avm2Class* cls)
{
	Avm2Object* proto = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 0);
	proto->cls = ctx->builtins.object_class;
	proto->is_prototype = 1;  // dynamic even for sealed classes
	if (cls->super_class != NULL)
	{
		proto->proto = cls->super_class->prototype_obj;
	}
	cls->prototype_obj = proto;
	avm2_gc_pin(proto);  // GC: class prototypes are immortal (structural)
	if (cls->class_object != NULL)
	{
		Avm2DynProp* p = avm2_object_set_dynamic(ctx, proto, "constructor", 11,
		                                         avm2_object_value(cls->class_object));
		p->dont_enum = 1;
	}
}

Avm2Class* avm2_class_define(Avm2Context* ctx, Avm2AbcFileRt* file, uint32_t class_idx,
                             Avm2Class* super_class, Avm2ScopeChain* scope)
{
	const Avm2AbcClassData* cd = &file->data->classes[class_idx];

	Avm2Class* cls = avm2_alloc(ctx, sizeof(Avm2Class));
	memset(cls, 0, sizeof(Avm2Class));
	if (!avm2_propkey_from_qname(file->data, cd->name_mn, &cls->name))
	{
		avm2_fatal("AVM2: class name multiname %u is not a QName", cd->name_mn);
	}
	cls->super_class = super_class;
	cls->flags = cd->flags;
	cls->scope = scope;
	// Instance scope = class scope + [class object] (Ruffle
	// ClassObject::from_class): instance methods and the constructor see the
	// class object on their outer chain, so FindProperty reaches static
	// traits (`INSTANCE = this` inside a ctor). The entry is patched with
	// the real class object once it exists below.
	{
		uint32_t outer_n = scope != NULL ? scope->count : 0;
		Avm2ScopeChain* iscope = avm2_alloc(ctx, sizeof(Avm2ScopeChain));
		iscope->count = outer_n + 1;
		iscope->entries = avm2_alloc(ctx, iscope->count * sizeof(Avm2ScopeEntry));
		for (uint32_t i = 0; i < outer_n; i++) iscope->entries[i] = scope->entries[i];
		iscope->entries[outer_n].obj = NULL;
		iscope->entries[outer_n].is_with = 0;
		cls->iscope = iscope;
	}
	cls->interface_count = cd->interface_count;
	cls->interface_mns = cd->interface_mns;
	cls->iface_file = file;

	if (cd->has_protected_ns)
	{
		cls->has_protected_ns = 1;
		avm2_key_ns_from_abc(&cls->protected_key, file->data, cd->protected_ns);
	}

	const Avm2AbcMethodData* iinit = &file->data->methods[cd->instance_init];
	cls->instance_init.fn = iinit->fn;
	cls->instance_init.file = file;
	cls->instance_init.debug_name = iinit->debug_name;
	cls->instance_init.method_index = cd->instance_init;

	// Remember the static initializer too — a cinit frame is named after the
	// class ("Test$cinit()"), which the frame renderer cannot derive from the
	// method alone.
	const Avm2AbcMethodData* cinitd = &file->data->methods[cd->class_init];
	cls->class_init.fn = cinitd->fn;
	cls->class_init.file = file;
	cls->class_init.debug_name = cinitd->debug_name;
	cls->class_init.method_index = cd->class_init;

	// Inherit the flattened instance vtable, then append own traits
	// (vtable.rs model). Inherited protected-ns entries are re-keyed onto
	// this class's protected namespace so subclass protected access
	// resolves (es4_protected_inheritance).
	if (super_class != NULL)
	{
		cls->ivtable.slot_count = super_class->ivtable.slot_count;
		for (uint32_t i = 0; i < super_class->ivtable.count; i++)
		{
			avm2_vtable_append(ctx, &cls->ivtable, &super_class->ivtable.entries[i]);
		}
		for (uint32_t s = 1; s < super_class->ivtable.meta_cap; s++)
		{
			if (super_class->ivtable.metas[s].used)
			{
				vtable_set_slot_meta(ctx, &cls->ivtable, s,
				                     &super_class->ivtable.metas[s]);
			}
		}
		// Protected traits are ALSO visible under this class's protected
		// namespace (Ruffle vtable.rs keeps the original key and inserts a
		// copy under the child's ns; overrides sync both — see
		// vtable_add_traits).
		if (cls->has_protected_ns && super_class->has_protected_ns)
		{
			uint32_t inherited = cls->ivtable.count;
			for (uint32_t i = 0; i < inherited; i++)
			{
				Avm2PropEntry e = cls->ivtable.entries[i];
				if (avm2_ns_fold(e.key.ns_kind) == 0x18
				    && e.key.ns_len == super_class->protected_key.ns_len
				    && memcmp(e.key.ns_uri, super_class->protected_key.ns_uri,
				              e.key.ns_len) == 0)
				{
					e.key.ns_kind = cls->protected_key.ns_kind;
					e.key.ns_uri = cls->protected_key.ns_uri;
					e.key.ns_len = cls->protected_key.ns_len;
					e.key.ns_priv = cls->protected_key.ns_priv;
					if (avm2_vtable_find(&cls->ivtable, &e.key) == NULL)
					{
						avm2_vtable_append(ctx, &cls->ivtable, &e);
					}
				}
			}
		}
		cls->native_ext_size = super_class->native_ext_size;
		cls->native_init = super_class->native_init;
		// Builtin-container kind IS inherited: `class MyList extends Array`
		// allocates element storage (avm2_class_construct). The chain is
		// complete here — builtins register at boot and a SWF class defines
		// before any instance of it exists.
		cls->instance_kind = super_class->instance_kind;
		cls->native_super_init = super_class->native_super_init;
		// native_construct is deliberately NOT inherited: a SWF subclass of
		// Object/Error/... constructs through the standard alloc+init path
		// (its own constructor chains to super via ConstructSuper).
	}
	avm2_vtable_add_traits(ctx, &cls->ivtable, file,
	                       cd->instance_traits, cd->instance_trait_count, cls, cls->iscope);
	if ((cd->flags & AVM2_CLASS_FLAG_INTERFACE) == 0)
	{
		class_add_interface_aliases(ctx, cls);
	}

	// Class object: holds the static (class) traits.
	Avm2VTable* cvt = avm2_alloc(ctx, sizeof(Avm2VTable));
	memset(cvt, 0, sizeof(Avm2VTable));
	avm2_vtable_add_traits(ctx, cvt, file, cd->class_traits, cd->class_trait_count,
	                       cls, scope);
	Avm2Object* cobj = avm2_object_alloc(ctx, AVM2_OBJ_CLASS, cvt->slot_count + 1);
	cobj->vtable = cvt;
	cobj->class_ref = cls;
	cobj->cls = ctx->builtins.class_class;
	cobj->proto = ctx->builtins.class_class->prototype_obj;
	cls->class_object = cobj;
	avm2_gc_pin(cobj);  // GC: class objects are immortal (structural)
	cls->iscope->entries[cls->iscope->count - 1].obj = cobj;
	avm2_slots_init_defaults(ctx, cobj, cvt);

	class_setup_prototype(ctx, cls);

	// Run the static initializer now (Ruffle runs it during NewClass).
	Avm2MethodRef cinit_ref = cls->class_init;
	avm2_call_method_ref(ctx, &cinit_ref, cls, scope,
	                     avm2_object_value(cobj), NULL, 0);

	// Native intrinsics: the recompiler fingerprints each class's normalized
	// method bodies + trait layout and stamps a marker on an exact match. A
	// zero marker (any mismatch at all) means the game's own compiled code
	// runs untouched — the fallback is non-negotiable.
	if (cd->intrinsic_id != 0)
	{
		avm2_flixel_try_install(ctx, cls, cd->intrinsic_id);
	}

	file->classes[class_idx] = cls;
	return cls;
}

// Object kind for instances of `cls`: the kind inherited from the builtin
// ancestor, minus avmplus's version-gated rule for SEALED subclasses of Array
// (bugzilla 654807, regress/bug_654807_swf12 vs _swf13 -- same source, SWF 12
// and 13, opposite expectations).
//
//   SWF >= 13: a sealed Array subclass gets NO element storage. Its Array
//     methods take their generic (property-based) paths, so a method that
//     writes reports #1056 and `length` stays 0 -- the "sealed" columns.
//   SWF <= 12: it keeps element storage, so the dense-path methods work,
//     but index property access still follows the sealed-object rules
//     (#1069 / #1056 / false) -- the inconsistent "semisealed" behaviour
//     that the bug fixed.
//
// The gate reads the LEAF class's own dynamic bit, never an inherited one:
// `dynamic class D extends SealedArray` is a plain dynamic array in both
// versions, and `class S extends DynamicArray` is gated in both.
uint8_t avm2_class_instance_kind(Avm2Context* ctx, const Avm2Class* cls)
{
	uint8_t kind = cls->instance_kind;
	if (kind == AVM2_OBJ_ARRAY && (cls->flags & AVM2_CLASS_FLAG_SEALED)
	    && ctx->swf_version >= 13)
	{
		return AVM2_OBJ_SCRIPT;
	}
	return kind;
}

// Was THIS allocation a script `new`, or an internal C mint?
//
// Many playerglobal classes are [Ruffle(Abstract)] — `new Graphics()` is
// #2012 — while the runtime itself mints them constantly (DisplayObject's
// `graphics` getter, requestContext3D, SharedObject.getLocal, ...). Both go
// through avm2_class_construct, so an unconditional native_init gate would
// break the internal mints; TextLine and MorphShape each carry their own
// ad-hoc flag for exactly this reason.
//
// The discriminator is free: avm2_construct_value is called ONLY from
// avm2_ops.c's construct* opcodes, i.e. only for a script `new`. It arms the
// flag; the first alloc_instance to see it consumes it, so a constructor body
// that internally mints an abstract class is (correctly) not a script `new`.
static int g_script_construct_armed;

void avm2_class_arm_script_construct(void)
{
	g_script_construct_armed = 1;
}

static int g_alloc_is_script_new;

int avm2_class_alloc_is_script_new(void)
{
	return g_alloc_is_script_new;
}

Avm2Value avm2_class_construct(Avm2Context* ctx, Avm2Class* cls,
                               const Avm2Value* args, uint32_t argc)
{
	int script_new = g_script_construct_armed;
	g_script_construct_armed = 0;
	if (cls->flags & AVM2_CLASS_FLAG_INTERFACE)
	{
		// avmplus names the missing implementation after the interface's own
		// constructor slot -- `new ITest()` reports "The method ITest() is
		// not implemented." (construct_interface).
		avm2_throw_error(ctx, ctx->builtins.verify_error_class,
		                 "Error #1001: The method %.*s() is not implemented.",
		                 (int) cls->name.name_len,
		                 cls->name.name != NULL ? cls->name.name : "undefined");
	}
	if (cls->native_construct != NULL)
	{
		return cls->native_construct(ctx, cls, args, argc);
	}
	uint8_t kind = avm2_class_instance_kind(ctx, cls);
	Avm2Object* obj = avm2_object_alloc(ctx, kind, cls->ivtable.slot_count + 1);
	obj->cls = cls;
	obj->vtable = &cls->ivtable;
	obj->proto = cls->prototype_obj;
	avm2_slots_init_defaults(ctx, obj, &cls->ivtable);
	if (kind == AVM2_OBJ_ARRAY)
	{
		avm2_array_ext_attach(ctx, obj);
	}
	else if (cls->native_ext_size > 0)
	{
		obj->native_ext = avm2_alloc(ctx, cls->native_ext_size);
		memset(obj->native_ext, 0, cls->native_ext_size);
		obj->native_ext_size = cls->native_ext_size;  // GC conservative-scan span
	}
	if (cls->native_init != NULL)
	{
		int saved = g_alloc_is_script_new;
		g_alloc_is_script_new = script_new;
		cls->native_init(ctx, obj);
		g_alloc_is_script_new = saved;
	}
	Avm2Value this_val = avm2_object_value(obj);
	avm2_call_method_ref(ctx, &cls->instance_init, cls,
	                     cls->iscope != NULL ? cls->iscope : cls->scope,
	                     this_val, args, argc);
	return this_val;
}

Avm2Value avm2_construct_value(Avm2Context* ctx, Avm2Value ctor,
                               const Avm2Value* args, uint32_t argc)
{
	if (ctor.kind == AVM2_VALUE_OBJECT && ctor.u.obj != NULL)
	{
		Avm2Object* o = ctor.u.obj;
		if (o->kind == AVM2_OBJ_CLASS)
		{
			avm2_class_arm_script_construct();
			return avm2_class_construct(ctx, o->class_ref, args, argc);
		}
		if (o->kind == AVM2_OBJ_FUNCTION)
		{
			if (o->fn_bound_class != NULL)
			{
				// A METHOD (class-bound) can never be constructed, however it
				// was obtained — a method closure off an instance, a prototype
				// method, `arguments.callee` inside a method (Ruffle
				// FunctionObject::construct, the `method.bound_class()` gate).
				// Only `newfunction` closures reach the ES3 path below.
				const char* fname = o->fn_method.debug_name;
				avm2_throw_error(ctx, ctx->builtins.type_error_class,
				                 "Error #1064: Cannot call method %s as "
				                 "constructor.",
				                 fname != NULL ? fname : "?");
			}
			// ES3 constructor call: new object, proto = fn.prototype,
			// call with this = the new object; an object return value
			// replaces the new object.
			Avm2Object* inst = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 0);
			inst->cls = ctx->builtins.object_class;
			inst->vtable = &ctx->builtins.object_class->ivtable;
			Avm2Value protov = avm2_get_public_property(
				ctx, ctor, "prototype", 9, NULL);
			if (protov.kind == AVM2_VALUE_OBJECT)
			{
				inst->proto = protov.u.obj;
			}
			else
			{
				// A nulled fn.prototype resets to Object.prototype on
				// construction (Ruffle FunctionObject::construct;
				// prototype_set_null).
				o->fn_prototype = ctx->builtins.object_class->prototype_obj;
				o->fn_proto_nulled = 0;
				inst->proto = o->fn_prototype;
			}
			Avm2Value result = avm2_call_function_obj(ctx, o, avm2_object_value(inst),
			                                          args, argc);
			if (result.kind == AVM2_VALUE_OBJECT)
			{
				return result;
			}
			return avm2_object_value(inst);
		}
	}
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1007: Instantiation attempted on a non-constructor.");
}

// ---------------------------------------------------------------------------
// Types / classes of values
// ---------------------------------------------------------------------------

Avm2Class* avm2_value_class(Avm2Context* ctx, Avm2Value v)
{
	switch (v.kind)
	{
		case AVM2_VALUE_BOOL: return ctx->builtins.boolean_class;
		case AVM2_VALUE_INTEGER:
		case AVM2_VALUE_NUMBER: return ctx->builtins.number_class;
		case AVM2_VALUE_STRING: return ctx->builtins.string_class;
		case AVM2_VALUE_OBJECT:
			return v.u.obj->cls != NULL ? v.u.obj->cls : ctx->builtins.object_class;
		default:
			avm2_fatal("avm2_value_class on null/undefined");
	}
}

const Avm2VTable* avm2_value_vtable(Avm2Context* ctx, Avm2Value v)
{
	if (v.kind == AVM2_VALUE_OBJECT)
	{
		if (v.u.obj->vtable != NULL)
		{
			return v.u.obj->vtable;
		}
		// Plain objects (NewObject, ES3-constructed instances, JSON) carry
		// no own vtable: their class's instance vtable applies (Object's
		// hasOwnProperty & co resolve through the AS3 namespace).
		return v.u.obj->cls != NULL ? &v.u.obj->cls->ivtable : NULL;
	}
	Avm2Class* cls = avm2_value_class(ctx, v);
	return &cls->ivtable;
}

Avm2Object* avm2_value_proto(Avm2Context* ctx, Avm2Value v)
{
	if (v.kind == AVM2_VALUE_OBJECT)
	{
		return v.u.obj->proto;
	}
	Avm2Class* cls = avm2_value_class(ctx, v);
	return cls->prototype_obj;
}

static void resolve_interfaces(Avm2Context* ctx, Avm2Class* cls)
{
	if (cls->interface_count == 0) return;
	if (cls->interfaces == NULL)
	{
		uint32_t bytes = cls->interface_count * (uint32_t) sizeof(Avm2Class*);
		cls->interfaces = avm2_alloc(ctx, bytes);
		memset(cls->interfaces, 0, bytes);
	}
	// Cache only NON-NULL answers, exactly as avm2_class_for_mn does. A class
	// is set up (newclass) before the script's later classes exist, so an
	// interface declared AFTER its implementor in the same script resolves to
	// NULL at that moment; freezing that NULL made `implements X` invisible
	// forever (superinterface_call: `var v: BaseInterface = concrete` threw
	// #1034). Re-resolve the misses on each call — once a name resolves, the
	// domain's append-only identity makes the answer permanent.
	if (cls->interface_mns == NULL) return;
	int resolved_now = 0;
	for (uint32_t i = 0; i < cls->interface_count; i++)
	{
		if (cls->interfaces[i] != NULL) continue;
		cls->interfaces[i] = avm2_class_for_mn(ctx, cls->iface_file,
		                                       cls->interface_mns[i]);
		if (cls->interfaces[i] != NULL) resolved_now = 1;
	}
	// The interface-namespace method aliases (add_iface_aliases_from) are
	// installed at newclass time off this same list, so an interface that
	// only resolves now never got its aliases: `iface_typed.method()` would
	// raise #1069. Install them the moment the interface becomes visible.
	if (resolved_now && (cls->flags & AVM2_CLASS_FLAG_INTERFACE) == 0)
	{
		for (uint32_t i = 0; i < cls->interface_count; i++)
		{
			if (cls->interfaces[i] != NULL)
			{
				add_iface_aliases_from(ctx, cls, cls->interfaces[i]);
			}
		}
	}
}

void avm2_class_resolve_interfaces(Avm2Context* ctx, Avm2Class* cls)
{
	if (cls != NULL) resolve_interfaces(ctx, cls);
}

// Does `cls` implement `iface` directly or transitively?
bool avm2_class_has_interface(Avm2Context* ctx, Avm2Class* cls, Avm2Class* iface)
{
	for (Avm2Class* c = cls; c != NULL; c = c->super_class)
	{
		if (c->interface_count == 0) continue;
		resolve_interfaces(ctx, c);
		for (uint32_t i = 0; i < c->interface_count; i++)
		{
			Avm2Class* di = c->interfaces[i];
			if (di == NULL) continue;
			if (di == iface) return true;
			// Super-interfaces are declared as the interface's own
			// interface list.
			if (avm2_class_has_interface(ctx, di, iface)) return true;
		}
	}
	return false;
}

bool avm2_value_is_of_type(Avm2Context* ctx, Avm2Value v, Avm2Class* type_class)
{
	Avm2Builtins* b = &ctx->builtins;
	if (type_class == b->number_class) return avm2_value_is_number(v);
	if (type_class == b->uint_class) return avm2_value_is_u32(v);
	if (type_class == b->int_class) return avm2_value_is_i32(v);
	if (type_class == b->boolean_class) return v.kind == AVM2_VALUE_BOOL;
	if (type_class == b->string_class) return v.kind == AVM2_VALUE_STRING;
	if (type_class == b->object_class)
	{
		return v.kind != AVM2_VALUE_UNDEFINED && v.kind != AVM2_VALUE_NULL;
	}
	if (v.kind == AVM2_VALUE_UNDEFINED || v.kind == AVM2_VALUE_NULL)
	{
		return false;
	}
	if (v.kind != AVM2_VALUE_OBJECT)
	{
		// Primitives are instances of their builtin classes only (handled
		// above); Function/Class subclass checks need an object.
		Avm2Class* vc = avm2_value_class(ctx, v);
		for (Avm2Class* c = vc; c != NULL; c = c->super_class)
		{
			if (c == type_class) return true;
		}
		return false;
	}
	Avm2Class* vc = v.u.obj->cls;
	for (Avm2Class* c = vc; c != NULL; c = c->super_class)
	{
		if (c == type_class) return true;
	}
	if (type_class->flags & AVM2_CLASS_FLAG_INTERFACE)
	{
		return avm2_class_has_interface(ctx, vc, type_class);
	}
	return false;
}

static Avm2Class* avm2_class_for_mn_resolve(Avm2Context* ctx, Avm2AbcFileRt* file,
                                            uint32_t mn_idx)
{
	if (mn_idx == 0 || file == NULL) return NULL;
	const Avm2AbcFileData* data = file->data;
	const Avm2AbcMultiname* mn = &data->multinames[mn_idx];

	if (mn->kind == 0x1d)  // TypeName: Vector.<T>
	{
		Avm2Class* base = avm2_class_for_mn(ctx, file, mn->base_type);
		if (base == NULL || !base->is_generic_vector) return NULL;
		Avm2Class* param = NULL;
		if (mn->type_param_count >= 1 && mn->type_params[0] != 0)
		{
			// A param naming "*" also means Vector.<*>.
			const char* pn;
			uint32_t pn_len;
			avm2_mn_name(data, mn->type_params[0], &pn, &pn_len);
			if (!(pn_len == 1 && pn[0] == '*'))
			{
				param = avm2_class_for_mn(ctx, file, mn->type_params[0]);
				if (param == NULL) return NULL;
			}
		}
		return avm2_vector_apply(ctx, param);
	}

	Avm2PropKey key;
	Avm2Object* globals = NULL;
	if (mn->kind == 0x07 || mn->kind == 0x0d)
	{
		if (!avm2_propkey_from_qname(data, mn_idx, &key)) return NULL;
		globals = avm2_domain_find(ctx, file->scope, &key);
	}
	else if (mn->kind == 0x09 || mn->kind == 0x0e)
	{
		const Avm2AbcNsSet* set = &data->ns_sets[mn->ns_set];
		for (uint32_t i = 0; i < set->count && globals == NULL; i++)
		{
			key.name = data->strings[mn->name].utf8;
			key.name_len = data->strings[mn->name].len;
			avm2_key_ns_from_abc(&key, data, set->ns_indices[i]);
			globals = avm2_domain_find(ctx, file->scope, &key);
		}
	}
	if (globals == NULL) return NULL;

	// Fetch the class value off the globals (slot or dynamic).
	const Avm2PropEntry* entry = avm2_vtable_find(globals->vtable, &key);
	Avm2Value cv = avm2_undefined();
	if (entry != NULL && entry->kind == AVM2_PROP_SLOT)
	{
		cv = globals->slots[entry->slot_index];
	}
	else
	{
		Avm2Value* dyn = avm2_object_find_dynamic(globals, key.name, key.name_len);
		if (dyn != NULL) cv = *dyn;
	}
	if (cv.kind == AVM2_VALUE_OBJECT && cv.u.obj != NULL
	    && cv.u.obj->kind == AVM2_OBJ_CLASS)
	{
		return cv.u.obj->class_ref;
	}
	return NULL;
}

// Resolve an mn_idx (a compile-time constant at each coerce / setup_locals /
// setproperty / coerce_return site) to its Class, memoized per file.
//
// The (file, mn_idx) -> Class* mapping is a pure function of append-only state:
// the class is installed into a script global exactly once and the domain is
// append-only with stable object identity, so once resolution yields a non-NULL
// Class that answer is permanent. We therefore cache only NON-NULL results —
// during a class's own cinit its type name (and forward references) resolve to
// NULL, and that transient miss must NOT be frozen. Vector.<T> and every
// recursive base_type / type_param resolution route back through this wrapper,
// so they are cached too.
//
// Build -DAVM2_CLASS_MEMO_VERIFY to re-run the full resolve on every memo HIT
// and abort on any divergence; build -DSWF_NO_CLASS_MEMO to disable the cache
// entirely (A/B baseline).
Avm2Class* avm2_class_for_mn(Avm2Context* ctx, Avm2AbcFileRt* file, uint32_t mn_idx)
{
	if (mn_idx == 0 || file == NULL) return NULL;
#ifndef SWF_NO_CLASS_MEMO
	Avm2Class** memo = file->coerce_class_memo;
	if (memo != NULL && mn_idx < file->data->multiname_count)
	{
		Avm2Class* hit = memo[mn_idx];
		if (hit != NULL)
		{
#ifdef AVM2_CLASS_MEMO_VERIFY
			Avm2Class* fresh = avm2_class_for_mn_resolve(ctx, file, mn_idx);
			if (fresh != hit)
			{
				avm2_fatal("[AVM2_CLASS_MEMO_VERIFY] class-memo mismatch mn=%u: "
				           "memo=%p fresh=%p", mn_idx, (void*) hit, (void*) fresh);
			}
#endif
			return hit;
		}
		Avm2Class* cls = avm2_class_for_mn_resolve(ctx, file, mn_idx);
		if (cls != NULL) memo[mn_idx] = cls;
		return cls;
	}
#endif
	return avm2_class_for_mn_resolve(ctx, file, mn_idx);
}

// Coercion by class (Ruffle value.rs coerce_to_type).
Avm2Value avm2_coerce_to_class(Avm2Context* ctx, Avm2Class* cls, Avm2Value v)
{
	Avm2Builtins* b = &ctx->builtins;
	if (cls == NULL) return v;
	if (cls == b->int_class) return avm2_integer(avm2_coerce_to_i32(ctx, v));
	if (cls == b->uint_class) return avm2_uint_value(avm2_coerce_to_u32(ctx, v));
	if (cls == b->number_class) return avm2_number(avm2_coerce_to_number(ctx, v));
	if (cls == b->boolean_class) return avm2_bool(avm2_coerce_to_boolean(v));
	if (v.kind == AVM2_VALUE_UNDEFINED || v.kind == AVM2_VALUE_NULL)
	{
		return avm2_null();
	}
	if (cls == b->string_class) return avm2_string(avm2_coerce_to_string(ctx, v));
	if (cls == b->object_class) return v;
	if (avm2_value_is_of_type(ctx, v, cls))
	{
		return v;
	}
	// Error #1034 with a debug string for the value.
	char dbg[128];
	if (v.kind == AVM2_VALUE_STRING)
	{
		snprintf(dbg, sizeof(dbg), "\"%.*s\"",
		         v.u.str->len > 100 ? 100 : (int) v.u.str->len, v.u.str->utf8);
	}
	else if (v.kind == AVM2_VALUE_OBJECT)
	{
		// FP prints the VALUE's type ns::name (colons) but the TARGET
		// class dotted (vector_coercion).
		Avm2Class* vc = avm2_value_class(ctx, v);
		if (vc->name.ns_len > 0)
		{
			snprintf(dbg, sizeof(dbg), "%.*s::%.*s@00000000000",
			         (int) vc->name.ns_len, vc->name.ns_uri,
			         (int) vc->name.name_len, vc->name.name);
		}
		else
		{
			snprintf(dbg, sizeof(dbg), "%.*s@00000000000",
			         (int) vc->name.name_len, vc->name.name);
		}
	}
	else
	{
		const Avm2String* s = avm2_coerce_to_string(ctx, v);
		snprintf(dbg, sizeof(dbg), "%.*s",
		         s->len > 100 ? 100 : (int) s->len, s->utf8);
	}
	char cq[160];
	avm2_class_qname_buf(cls, cq, sizeof(cq));
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1034: Type Coercion failed: cannot convert %s to %s.",
	                 dbg, cq);
}

Avm2Value avm2_coerce_to_type_mn(Avm2Context* ctx, Avm2AbcFileRt* file,
                                 uint32_t mn_idx, Avm2Value v)
{
	if (mn_idx == 0) return v;  // any
	const char* tn;
	uint32_t tn_len;
	avm2_mn_name(file->data, mn_idx, &tn, &tn_len);
	if (tn_len == 1 && tn[0] == '*') return v;
	if (tn_len == 4 && memcmp(tn, "void", 4) == 0)
	{
		return (v.kind == AVM2_VALUE_UNDEFINED || v.kind == AVM2_VALUE_NULL)
		       ? avm2_undefined() : v;
	}
	Avm2Class* cls = avm2_class_for_mn(ctx, file, mn_idx);
	if (cls == NULL)
	{
		// Unresolvable type name (e.g. the class's own slot during its
		// cinit): null/undefined still coerce to null; other values are
		// left alone.
		if (v.kind == AVM2_VALUE_UNDEFINED || v.kind == AVM2_VALUE_NULL)
		{
			return avm2_null();
		}
		return v;
	}
	return avm2_coerce_to_class(ctx, cls, v);
}

// ---------------------------------------------------------------------------
// swfmodern.Reflect — namespace-blind trait access for injected code
// ---------------------------------------------------------------------------
//
// Sibling of swfmodern.Rng (avm2_number.c): a runtime INSTRUMENT, not an ECMA
// or playerglobal surface. It exists for code injected into a recompiled,
// source-less SWF — a recorder that must read a game object's `protected var
// dashTime` without being able to recompile the game.
//
// Why the ordinary route cannot work. `getproperty` with a runtime-built name
// resolves in the PUBLIC namespace, and a script cannot construct a key in any
// other one:
//   - PRIVATE (ABC kind 0x05) is compared by POOL-ENTRY IDENTITY, never by URI
//     (avm2_propkey_matches above; every ASC PrivateNamespace carries the same
//     empty name). No `new Namespace(uri)` can ever equal one.
//   - PROTECTED (0x18) IS compared by (kind, URI) — it is not identity-keyed,
//     which is a real difference from private. It is still unreachable from
//     script, for a different reason: `new Namespace(...)` always yields kind
//     0x16 (avm2_nsqname.c namespace_construct), and 0x16 vs 0x18 fails the
//     kind check before the URI is ever looked at. The only way to a 0x18 key
//     is a namespace CONSTANT in an ABC that declares it.
// So the hook resolves against the class's trait table directly, and takes the
// declaring class as a REQUIRED argument.
//
// Why the OWNER argument is required rather than optional: a base-class
// `private var x` and a subclass `protected var x` are genuinely different
// slots (private members are not virtual — Base's method reads BASE's slot),
// so a name-only lookup would have to pick one by a rule the caller could only
// trust. Naming the owner removes the ambiguity by construction. Callers get
// the owner from getQualifiedClassName, so BOTH spellings are accepted:
// "pkg.Class" and "pkg::Class".
//
// Why the RUNTIME vtable and not the Avm2AbcTrait arrays: a trait's
// `slot_or_disp_id` is frequently 0 ("auto-assign"), so the static table alone
// does not say which slot a trait owns — you would have to replay the whole
// vtable build to find out. The flattened ivtable already holds the answer,
// already carries `defining_class` on every entry (stamped in
// avm2_vtable_add_traits, and preserved when a subclass inherits the entry),
// and already keeps a shadowed base slot alongside the subclass's own. Class
// (static) traits live on the class object's own vtable with the same stamp,
// so passing a class object reads statics through the same code.
//
// Every failure THROWS with a distinguishing message. A missing trait must
// never come back as null: null is exactly what the broken path returns, and
// an instrument that cannot tell "no such trait" from "trait unreadable" from
// "object not built yet" has not fixed anything.

static Avm2Object* reflect_receiver(Avm2Activation* act, uint32_t argc_min)
{
	Avm2Context* ctx = act->ctx;
	if (act->argc < argc_min)
	{
		avm2_throw_error(ctx, ctx->builtins.argument_error_class,
		                 "swfmodern.Reflect: expected %u arguments, got %u.",
		                 argc_min, act->argc);
	}
	Avm2Value v = act->args[0];
	if (v.kind == AVM2_VALUE_NULL || v.kind == AVM2_VALUE_UNDEFINED)
	{
		avm2_throw_error(ctx, ctx->builtins.reference_error_class,
		                 "swfmodern.Reflect: receiver is null or undefined "
		                 "(the object does not exist yet).");
	}
	if (v.kind != AVM2_VALUE_OBJECT || v.u.obj == NULL)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "swfmodern.Reflect: receiver is a primitive, which "
		                 "declares no traits.");
	}
	return v.u.obj;
}

// Does `cls` answer to the caller's spelling of its qualified name? Accepts
// the dot form ("pkg.Class") and the getQualifiedClassName form
// ("pkg::Class"), since callers get the string from the latter.
static int reflect_owner_matches(const Avm2Class* cls,
                                 const char* want, uint32_t want_len)
{
	char buf[256];
	int n = avm2_class_qname_buf(cls, buf, sizeof(buf));
	if (n > 0 && (size_t) n < sizeof(buf) && (uint32_t) n == want_len
	    && memcmp(buf, want, want_len) == 0)
	{
		return 1;
	}
	n = avm2_class_qname_colons_buf(cls, buf, sizeof(buf));
	if (n > 0 && (size_t) n < sizeof(buf) && (uint32_t) n == want_len
	    && memcmp(buf, want, want_len) == 0)
	{
		return 1;
	}
	return 0;
}

// Find the entry named `name` DECLARED BY the class the caller named. Interface
// aliases are skipped: they are our dispatch machinery, not traits anyone
// declared. `*owner_seen` reports whether the named class contributed anything
// at all to this receiver, so a wrong class name and a wrong member name are
// distinguishable failures.
static const Avm2PropEntry* reflect_find(const Avm2VTable* vt,
                                         const char* owner, uint32_t owner_len,
                                         const char* name, uint32_t name_len,
                                         int* owner_seen)
{
	*owner_seen = 0;
	if (vt == NULL) return NULL;
	const Avm2Class* last_owner = NULL;
	int last_match = 0;
	for (uint32_t i = 0; i < vt->count; i++)
	{
		const Avm2PropEntry* e = &vt->entries[i];
		if (e->is_iface_alias || e->defining_class == NULL) continue;
		// The owner test is an snprintf; the entry list is walked per call, so
		// cache the last verdict (runs of entries share a defining class).
		if (e->defining_class != last_owner)
		{
			last_owner = e->defining_class;
			last_match = reflect_owner_matches(e->defining_class, owner, owner_len);
		}
		if (!last_match) continue;
		*owner_seen = 1;
		if (e->key.name_len == name_len
		    && memcmp(e->key.name, name, name_len) == 0)
		{
			return e;
		}
	}
	return NULL;
}

static _Noreturn void reflect_no_trait(Avm2Context* ctx, int owner_seen,
                                       const char* owner, uint32_t owner_len,
                                       const char* name, uint32_t name_len)
{
	if (!owner_seen)
	{
		avm2_throw_error(ctx, ctx->builtins.reference_error_class,
		                 "swfmodern.Reflect: no class %.*s in this object's "
		                 "hierarchy.", (int) owner_len, owner);
	}
	avm2_throw_error(ctx, ctx->builtins.reference_error_class,
	                 "swfmodern.Reflect: class %.*s declares no trait %.*s.",
	                 (int) owner_len, owner, (int) name_len, name);
}

static Avm2Value reflect_get_trait(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* obj = reflect_receiver(act, 3);
	const Avm2String* owner = avm2_coerce_to_string(ctx, act->args[1]);
	const Avm2String* name = avm2_coerce_to_string(ctx, act->args[2]);
	int owner_seen = 0;
	const Avm2PropEntry* e = reflect_find(obj->vtable, owner->utf8, owner->len,
	                                      name->utf8, name->len, &owner_seen);
	if (e == NULL)
	{
		reflect_no_trait(ctx, owner_seen, owner->utf8, owner->len,
		                 name->utf8, name->len);
	}
	switch (e->kind)
	{
		case AVM2_PROP_SLOT:
			if (e->slot_index == 0 || e->slot_index >= obj->slot_count)
			{
				avm2_throw_error(ctx, ctx->builtins.reference_error_class,
				                 "swfmodern.Reflect: trait %.*s has no slot on "
				                 "this object (it is not fully constructed).",
				                 (int) name->len, name->utf8);
			}
			return obj->slots[e->slot_index];
		case AVM2_PROP_GETTER:
		case AVM2_PROP_GETSET:
			return avm2_call_method_ref(ctx, &e->method, e->defining_class,
			                            e->method_scope, act->args[0], NULL, 0);
		default:
			break;
	}
	avm2_throw_error(ctx, ctx->builtins.reference_error_class,
	                 "swfmodern.Reflect: trait %.*s is not readable (it is a %s).",
	                 (int) name->len, name->utf8,
	                 e->kind == AVM2_PROP_METHOD ? "method" : "write-only accessor");
}

static Avm2Value reflect_set_trait(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* obj = reflect_receiver(act, 4);
	const Avm2String* owner = avm2_coerce_to_string(ctx, act->args[1]);
	const Avm2String* name = avm2_coerce_to_string(ctx, act->args[2]);
	Avm2Value value = act->args[3];
	int owner_seen = 0;
	const Avm2PropEntry* e = reflect_find(obj->vtable, owner->utf8, owner->len,
	                                      name->utf8, name->len, &owner_seen);
	if (e == NULL)
	{
		reflect_no_trait(ctx, owner_seen, owner->utf8, owner->len,
		                 name->utf8, name->len);
	}
	switch (e->kind)
	{
		case AVM2_PROP_SLOT:
		{
			if (e->is_const)
			{
				avm2_throw_error(ctx, ctx->builtins.reference_error_class,
				                 "swfmodern.Reflect: trait %.*s is a const.",
				                 (int) name->len, name->utf8);
			}
			if (e->slot_index == 0 || e->slot_index >= obj->slot_count)
			{
				avm2_throw_error(ctx, ctx->builtins.reference_error_class,
				                 "swfmodern.Reflect: trait %.*s has no slot on "
				                 "this object (it is not fully constructed).",
				                 (int) name->len, name->utf8);
			}
			// Declared-type coercion, exactly as a slot write through the
			// ordinary path does it (avm2_op_setslot): an instrument must not
			// be able to park a String in an int slot.
			if (e->type_mn != 0 && e->type_file != NULL)
			{
				value = avm2_coerce_to_type_mn(ctx, e->type_file, e->type_mn, value);
			}
			obj->slots[e->slot_index] = value;
			return avm2_undefined();
		}
		case AVM2_PROP_SETTER:
		case AVM2_PROP_GETSET:
			// The setter half can be declared by a deeper class than the
			// getter (see Avm2PropEntry::setter_defining_class).
			avm2_call_method_ref(ctx, &e->setter,
			                     e->setter_defining_class != NULL
			                       ? e->setter_defining_class : e->defining_class,
			                     e->setter_scope != NULL
			                       ? e->setter_scope : e->method_scope,
			                     act->args[0], &value, 1);
			return avm2_undefined();
		default:
			break;
	}
	avm2_throw_error(ctx, ctx->builtins.reference_error_class,
	                 "swfmodern.Reflect: trait %.*s is not writable (it is a %s).",
	                 (int) name->len, name->utf8,
	                 e->kind == AVM2_PROP_METHOD ? "method" : "read-only accessor");
}

void avm2_register_reflect(Avm2Context* ctx)
{
	Avm2Class* refl = avm2_builtin_class(ctx, "swfmodern", "Reflect",
	                                     ctx->builtins.object_class);
	refl->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
	avm2_builtin_add_static_method_n(ctx, refl, "getTrait", reflect_get_trait, 3);
	avm2_builtin_add_static_method_n(ctx, refl, "setTrait", reflect_set_trait, 4);
}
