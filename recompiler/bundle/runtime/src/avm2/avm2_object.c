// ScriptObject allocation + dynamic (expando) properties + array storage +
// enumeration.
//
// GC note (Stage 11): AVM2 objects allocate from the shared o1heap and enroll
// in the AVM2 mark-sweep census (avm2_gc.c) — a SEPARATE census from AVM1's
// g_mt_obj_head / g_mt_arr_head (ASObject/ASArray). Every avm2_object_alloc'd
// object is collectable; the collector marks the reachable set from the
// persistent root graph between ticks and sweeps the rest. See avm2_gc.h.

#include <stdio.h>
#include <string.h>

#include <avm2/avm2_gc.h>
#include <avm2/avm2_object.h>
#include <memory/heap.h>
#include <avm2/avm2_class.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_value.h>

// INLINE SLOTS WERE TRIED HERE AND RULED OUT — see RWK_AB_STATUS.md. Carving
// the slot array out of the object's own allocation removed one alloc + one
// free per construction (o1heapAllocate 2.60% -> 1.48% Ir, -489M) but grew the
// live heap 22% (138 -> 169 MB at 1560 ticks) and cut GC=0 headroom ~17%.
// Cause: o1heap bins by `roundUpToPowerOf2(amount + O1HEAP_ALIGNMENT)` with
// ALIGNMENT 32, and sizeof(Avm2Object) is 208 -> a 256-byte fragment with only
// 16 bytes of slack. Combining crosses a power of 2 for every class with more
// than one slot: FlxList (2 slots, 64% of constructions) 320 -> 512 bytes,
// FlxQuadTree (21 slots, 30%) 768 -> 1024. It only pays above ~57 slots, i.e.
// on classes with negligible construction volume. Do not retry without first
// changing the allocator's size classes or shrinking Avm2Object.
Avm2Object* avm2_object_alloc(Avm2Context* ctx, uint8_t kind, uint32_t slot_count)
{
	Avm2Object* obj = avm2_alloc(ctx, sizeof(Avm2Object));
	memset(obj, 0, sizeof(Avm2Object));
	avm2_gc_enroll(ctx, obj);
	obj->kind = kind;
	obj->slot_count = slot_count;
	if (slot_count > 0)
	{
		obj->slots = avm2_alloc(ctx, slot_count * sizeof(Avm2Value));
		// `avm2_undefined()` is { 0, 0, { 0 } } — all 16 bytes zero — so this
		// is byte-exactly the old per-slot undefined fill.
		memset(obj->slots, 0, slot_count * sizeof(Avm2Value));
	}
	return obj;
}

Avm2DynProp* avm2_object_find_dynamic_entry(Avm2Object* obj, const char* name,
                                            uint32_t name_len)
{
	for (Avm2DynProp* p = obj->dyn_props; p != NULL; p = p->next)
	{
		if (!p->dead && p->key_obj == NULL && p->name.len == name_len
		    && memcmp(p->name.utf8, name, name_len) == 0)
		{
			return p;
		}
	}
	return NULL;
}

Avm2Value* avm2_object_find_dynamic(Avm2Object* obj, const char* name, uint32_t name_len)
{
	Avm2DynProp* p = avm2_object_find_dynamic_entry(obj, name, name_len);
	return (p != NULL) ? &p->value : NULL;
}

Avm2Value* avm2_object_find_dynamic_obj(Avm2Object* obj, Avm2Object* key)
{
	for (Avm2DynProp* p = obj->dyn_props; p != NULL; p = p->next)
	{
		if (!p->dead && p->key_obj == key) return &p->value;
	}
	return NULL;
}

void avm2_object_set_dynamic_obj(Avm2Context* ctx, Avm2Object* obj, Avm2Object* key,
                                 Avm2Value value)
{
	Avm2Value* v = avm2_object_find_dynamic_obj(obj, key);
	if (v != NULL)
	{
		*v = value;
		return;
	}
	Avm2DynProp* p = avm2_alloc(ctx, sizeof(Avm2DynProp));
	memset(p, 0, sizeof(*p));
	p->key_obj = key;
	p->value = value;
	if (obj->dyn_tail != NULL) obj->dyn_tail->next = p;
	else obj->dyn_props = p;
	obj->dyn_tail = p;
}

int avm2_object_delete_dynamic_obj(Avm2Object* obj, Avm2Object* key)
{
	for (Avm2DynProp* p = obj->dyn_props; p != NULL; p = p->next)
	{
		if (!p->dead && p->key_obj == key)
		{
			p->dead = 1;  // tombstone (see Avm2DynProp.dead)
			return 1;
		}
	}
	return 0;
}

// Is this name the CANONICAL spelling of a dynamic INTEGER key? ("0" yes, "00"
// and "-1" no — those round-trip as strings, so both players keep them
// string-keyed.)
//
// The ceiling is 2^28 - 1, NOT u32::MAX: "Due to a quirk in avmplus, only
// values that fit in 28 bits are considered to be integers. Other values remain
// strings." (ruffle core/src/avm2/object/script_object.rs::maybe_int_property,
// commit 45904c3ee; avmplus String::getIntAtom / String::parseIndex). avm2
// primitive_keys pins the boundary exactly: 268435455 -> number,
// 268435456 / 2147483646 / 4294967294 -> string. Array's OWN index parse is a
// different predicate with a u32::MAX - 1 ceiling (ruffle
// array_object.rs:111) — do not fold the two together.
#define AVM2_DYN_KEY_MAX_INDEX 0x0FFFFFFFull

static int dyn_key_index_value(const char* name, uint32_t name_len, uint32_t* out)
{
	if (name_len == 0 || name_len > 10) return 0;
	if (name_len > 1 && name[0] == '0') return 0;
	uint64_t v = 0;
	for (uint32_t i = 0; i < name_len; i++)
	{
		if (name[i] < '0' || name[i] > '9') return 0;
		v = v * 10 + (uint64_t) (name[i] - '0');
	}
	if (v > AVM2_DYN_KEY_MAX_INDEX) return 0;
	if (out != NULL) *out = (uint32_t) v;
	return 1;
}

static int dyn_key_is_index(const char* name, uint32_t name_len)
{
	return dyn_key_index_value(name, name_len, NULL);
}

Avm2DynProp* avm2_object_set_dynamic(Avm2Context* ctx, Avm2Object* obj, const char* name,
                                     uint32_t name_len, Avm2Value value)
{
	for (Avm2DynProp* p = obj->dyn_props; p != NULL; p = p->next)
	{
		if (!p->dead && p->key_obj == NULL && p->name.len == name_len
		    && memcmp(p->name.utf8, name, name_len) == 0)
		{
			p->value = value;
			return p;
		}
	}
	Avm2DynProp* p = avm2_alloc(ctx, sizeof(Avm2DynProp));
	memset(p, 0, sizeof(*p));  // key_obj/dead MUST start 0 — string finders skip
	                           // non-NULL key_obj, and the GC traces key_obj; an
	                           // uninitialized value (o1heap reuses freed memory
	                           // once the collector runs) breaks both.
	const Avm2String* s = avm2_string_new(ctx, name, name_len);
	p->name = *s;
	p->value = value;
	p->next = NULL;
	p->dont_enum = 0;
	// Enumeration order: integer-spelled keys come BEFORE string keys, with
	// insertion order holding inside each group.
	//
	// Both players key an integer-spelled dynamic property by its integer value
	// (Ruffle DynamicKey::Uint; avmplus interns it as an int atom) and enumerate
	// it ahead of the string keys. Two recorded outputs pin the pair of rules:
	// `{"0": "fake_0", "length": 1}` serializes "0" first (avm2
	// amf_array_serialization, Flash AND Ruffle bytes agree) even though
	// newobject sets the literal's pairs LAST-first, while the all-string
	// `{a: this.test, b: true}` traces as `{"b":true,"a":…}` — reverse of source
	// — from that same last-first insertion (avm2 json_stringify_function).
	// Appending at the tail satisfies only the second.
	if (dyn_key_is_index(name, name_len))
	{
		// Insert after the last integer-like entry: the list stays partitioned,
		// so that entry is always found by scanning from the head.
		Avm2DynProp* prev = NULL;
		for (Avm2DynProp* q = obj->dyn_props; q != NULL; q = q->next)
		{
			if (q->key_obj != NULL
			    || !dyn_key_is_index(q->name.utf8, q->name.len)) break;
			prev = q;
		}
		if (prev == NULL)
		{
			p->next = obj->dyn_props;
			obj->dyn_props = p;
		}
		else
		{
			p->next = prev->next;
			prev->next = p;
		}
		if (p->next == NULL) obj->dyn_tail = p;
		return p;
	}
	if (obj->dyn_tail != NULL)
	{
		obj->dyn_tail->next = p;
	}
	else
	{
		obj->dyn_props = p;
	}
	obj->dyn_tail = p;
	return p;
}

int avm2_object_delete_dynamic(Avm2Object* obj, const char* name, uint32_t name_len)
{
	for (Avm2DynProp* p = obj->dyn_props; p != NULL; p = p->next)
	{
		if (!p->dead && p->key_obj == NULL && p->name.len == name_len
		    && memcmp(p->name.utf8, name, name_len) == 0)
		{
			// A `static const` value prop is DontDelete (see read_only).
			if (p->read_only) return -1;
			p->dead = 1;  // tombstone (see Avm2DynProp.dead)
			return 1;
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Arrays
// ---------------------------------------------------------------------------

// Far writes go to the sorted sparse list instead of growing the dense
// region. Ruffle keeps arrays dense up to 2^28 entries; we cap at 2^22
// (64 MB of slots) — everything below stays dense (holes included), so the
// dense-based Array methods keep exact semantics; only truly huge indices
// (array_index_max) go sparse.
#define AVM2_ARRAY_MAX_DENSE (1u << 22)

Avm2ArrayExt* avm2_array_ext(Avm2Object* obj)
{
	if (obj == NULL || obj->kind != AVM2_OBJ_ARRAY) return NULL;
	return (Avm2ArrayExt*) obj->native_ext;
}

static void array_reserve(Avm2Context* ctx, Avm2ArrayExt* ext, uint32_t need)
{
	if (need <= ext->cap) return;
	uint32_t new_cap = ext->cap == 0 ? 8 : ext->cap;
	while (new_cap < need) new_cap *= 2;
	Avm2Value* grown = avm2_alloc(ctx, new_cap * sizeof(Avm2Value));
	if (ext->dense_len > 0)
	{
		memcpy(grown, ext->elems, ext->dense_len * sizeof(Avm2Value));
	}
	// The old buffer is owned by this ext alone (every reader re-reads
	// ext->elems; nothing caches the pointer across a call that can grow the
	// array), so it is freed here — it used to leak on every growth.
	if (ext->elems != NULL) heap_free(ctx->app, ext->elems);
	ext->elems = grown;
	ext->cap = new_cap;
}

// Attach empty element storage to an already-allocated AVM2_OBJ_ARRAY object.
// Used by avm2_array_new and by avm2_class_construct for `extends Array`
// subclass instances, which keep their own cls/vtable/proto.
// native_ext_size stays 0: the GC traces array ext precisely (trace_array).
Avm2ArrayExt* avm2_array_ext_attach(Avm2Context* ctx, Avm2Object* obj)
{
	Avm2ArrayExt* ext = avm2_alloc(ctx, sizeof(Avm2ArrayExt));
	memset(ext, 0, sizeof(Avm2ArrayExt));
	obj->native_ext = ext;
	return ext;
}

Avm2Object* avm2_array_new(Avm2Context* ctx, uint32_t length)
{
	Avm2Object* obj = avm2_object_alloc(ctx, AVM2_OBJ_ARRAY, 1);
	obj->cls = ctx->builtins.array_class;
	obj->vtable = &ctx->builtins.array_class->ivtable;
	obj->proto = ctx->builtins.array_class->prototype_obj;
	Avm2ArrayExt* ext = avm2_array_ext_attach(ctx, obj);
	if (length > 0)
	{
		if (length <= AVM2_ARRAY_MAX_DENSE)
		{
			array_reserve(ctx, ext, length);
			for (uint32_t i = 0; i < length; i++)
			{
				ext->elems[i].kind = AVM2_VALUE_HOLE;
			}
			ext->dense_len = length;
		}
		ext->length = length;
	}
	return obj;
}

Avm2Object* avm2_array_from_values(Avm2Context* ctx, const Avm2Value* vals, uint32_t n)
{
	Avm2Object* obj = avm2_array_new(ctx, 0);
	Avm2ArrayExt* ext = avm2_array_ext(obj);
	if (n > 0)
	{
		array_reserve(ctx, ext, n);
		memcpy(ext->elems, vals, n * sizeof(Avm2Value));
		ext->dense_len = n;
		ext->length = n;
	}
	return obj;
}

Avm2Value avm2_array_get(Avm2Object* arr, uint32_t idx)
{
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	Avm2Value hole = { AVM2_VALUE_HOLE, 0, { 0 } };
	if (ext == NULL) return hole;
	if (idx < ext->dense_len) return ext->elems[idx];
	for (Avm2SparseElem* s = ext->sparse; s != NULL && s->idx <= idx; s = s->next)
	{
		if (s->idx == idx) return s->v;
	}
	return hole;
}

void avm2_array_set(Avm2Context* ctx, Avm2Object* arr, uint32_t idx, Avm2Value v)
{
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	if (ext == NULL) return;
	if (idx < ext->dense_len)
	{
		ext->elems[idx] = v;
		return;
	}
	if (idx < AVM2_ARRAY_MAX_DENSE && ext->sparse == NULL)
	{
		array_reserve(ctx, ext, idx + 1);
		for (uint32_t i = ext->dense_len; i < idx; i++)
		{
			ext->elems[i].kind = AVM2_VALUE_HOLE;
		}
		ext->elems[idx] = v;
		ext->dense_len = idx + 1;
	}
	else
	{
		// Sorted insert into the sparse list.
		Avm2SparseElem** link = &ext->sparse;
		while (*link != NULL && (*link)->idx < idx) link = &(*link)->next;
		if (*link != NULL && (*link)->idx == idx)
		{
			(*link)->v = v;
		}
		else
		{
			Avm2SparseElem* n = avm2_alloc(ctx, sizeof(Avm2SparseElem));
			n->idx = idx;
			n->v = v;
			n->next = *link;
			*link = n;
		}
	}
	if (idx + 1 > ext->length && idx != 0xFFFFFFFFu)
	{
		ext->length = idx + 1;
	}
	else if (idx == 0xFFFFFFFFu)
	{
		// Index 2^32-1 can't extend length past 2^32-1.
		if (ext->length < 0xFFFFFFFFu) ext->length = 0xFFFFFFFFu;
	}
}

void avm2_array_set_length(Avm2Context* ctx, Avm2Object* arr, uint32_t new_len)
{
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	if (ext == NULL) return;
	if (new_len < ext->dense_len)
	{
		ext->dense_len = new_len;
	}
	else if (new_len > ext->dense_len && new_len <= AVM2_ARRAY_MAX_DENSE
	         && ext->sparse == NULL)
	{
		array_reserve(ctx, ext, new_len);
		for (uint32_t i = ext->dense_len; i < new_len; i++)
		{
			ext->elems[i].kind = AVM2_VALUE_HOLE;
		}
		ext->dense_len = new_len;
	}
	// Drop sparse entries at/after the new length.
	Avm2SparseElem** link = &ext->sparse;
	while (*link != NULL)
	{
		if ((*link)->idx >= new_len) *link = (*link)->next;
		else link = &(*link)->next;
	}
	ext->length = new_len;
}

int avm2_array_delete(Avm2Object* arr, uint32_t idx)
{
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	if (ext == NULL) return 0;
	if (idx < ext->dense_len)
	{
		if (ext->elems[idx].kind == AVM2_VALUE_HOLE) return 0;
		ext->elems[idx].kind = AVM2_VALUE_HOLE;
		return 1;
	}
	Avm2SparseElem** link = &ext->sparse;
	while (*link != NULL && (*link)->idx < idx) link = &(*link)->next;
	if (*link != NULL && (*link)->idx == idx)
	{
		*link = (*link)->next;
		return 1;
	}
	return 0;
}

void avm2_array_push(Avm2Context* ctx, Avm2Object* arr, Avm2Value v)
{
	Avm2ArrayExt* ext = avm2_array_ext(arr);
	avm2_array_set(ctx, arr, ext->length, v);
}

// ---------------------------------------------------------------------------
// Enumeration (Ruffle get_next_enumerant protocol; 1-based, 0 = done)
// ---------------------------------------------------------------------------

// Enumerant layout: arrays first expose their dense non-hole indices (in
// index order), then dynamic props (insertion order, skipping dont_enum).
// Plain objects expose dynamic props only (declared traits don't enumerate).

static uint32_t array_enum_count(Avm2Object* obj)
{
	Avm2ArrayExt* ext = avm2_array_ext(obj);
	if (ext == NULL) return 0;
	uint32_t n = 0;
	for (uint32_t i = 0; i < ext->dense_len; i++)
	{
		if (ext->elems[i].kind != AVM2_VALUE_HOLE) n++;
	}
	for (Avm2SparseElem* s = ext->sparse; s != NULL; s = s->next) n++;
	return n;
}

// Returns the index of the nth (1-based) non-hole element, or -1.
static int64_t array_nth_index(Avm2Object* obj, uint32_t nth)
{
	Avm2ArrayExt* ext = avm2_array_ext(obj);
	if (ext == NULL) return -1;
	uint32_t seen = 0;
	for (uint32_t i = 0; i < ext->dense_len; i++)
	{
		if (ext->elems[i].kind != AVM2_VALUE_HOLE)
		{
			seen++;
			if (seen == nth) return (int64_t) i;
		}
	}
	for (Avm2SparseElem* s = ext->sparse; s != NULL; s = s->next)
	{
		seen++;
		if (seen == nth) return (int64_t) s->idx;
	}
	return -1;
}

static Avm2DynProp* nth_enumerable_dyn(Avm2Object* obj, uint32_t nth)
{
	uint32_t seen = 0;
	for (Avm2DynProp* p = obj->dyn_props; p != NULL; p = p->next)
	{
		if (p->dead || p->dont_enum) continue;
		seen++;
		if (seen == nth) return p;
	}
	return NULL;
}

static uint32_t dyn_enum_count(Avm2Object* obj)
{
	uint32_t n = 0;
	for (Avm2DynProp* p = obj->dyn_props; p != NULL; p = p->next)
	{
		if (!p->dead && !p->dont_enum) n++;
	}
	return n;
}

uint32_t avm2_object_next_enumerant(Avm2Object* obj, uint32_t cur)
{
	if (obj->kind == AVM2_OBJ_VECTOR)
	{
		// Ruffle vector_object.rs: indices 0..length-1 (1-based enumerants).
		Avm2VectorExt* ext = (Avm2VectorExt*) obj->native_ext;
		return cur < ext->length ? cur + 1 : 0;
	}
	{
		uint32_t next;
		if (avm2_nsqname_next_enumerant(obj, cur, &next)) return next;
		if (avm2_proxy_next_enumerant(obj, cur, &next)) return next;
		if (avm2_xml_next_enumerant(obj, cur, &next)) return next;
	}
	uint32_t arr_n = array_enum_count(obj);
	if (cur < arr_n) return cur + 1;
	// Dyn-prop part: cursor-based (Ruffle dynamic_map.rs next) so entries
	// deleted mid-iteration don't shift positions under the iterator.
	uint32_t dpub = cur - arr_n;
	if (dpub == 0)
	{
		for (Avm2DynProp* p = obj->dyn_props; p != NULL; p = p->next)
		{
			if (!p->dead && !p->dont_enum)
			{
				obj->dyn_enum_pos = p;
				obj->dyn_enum_public = 1;
				return arr_n + 1;
			}
		}
		obj->dyn_enum_pos = NULL;
		obj->dyn_enum_public = 0;
		return 0;
	}
	if (obj->dyn_enum_public == 0 || dpub != obj->dyn_enum_public
	    || obj->dyn_enum_pos == NULL)
	{
		Avm2DynProp* p = nth_enumerable_dyn(obj, dpub);
		if (p == NULL)
		{
			obj->dyn_enum_pos = NULL;
			obj->dyn_enum_public = 0;
			return 0;
		}
		obj->dyn_enum_pos = p;
		obj->dyn_enum_public = dpub;
	}
	for (Avm2DynProp* p = obj->dyn_enum_pos->next; p != NULL; p = p->next)
	{
		if (!p->dead && !p->dont_enum)
		{
			obj->dyn_enum_pos = p;
			obj->dyn_enum_public = dpub + 1;
			return arr_n + dpub + 1;
		}
	}
	obj->dyn_enum_pos = NULL;
	obj->dyn_enum_public = 0;
	return 0;
}

Avm2Value avm2_object_enumerant_name(Avm2Context* ctx, Avm2Object* obj, uint32_t idx)
{
	if (obj->kind == AVM2_OBJ_VECTOR)
	{
		Avm2VectorExt* ext = (Avm2VectorExt*) obj->native_ext;
		if (idx >= 1 && idx <= ext->length) return avm2_uint_value(idx - 1);
		return avm2_null();
	}
	{
		Avm2Value v;
		if (avm2_nsqname_enumerant_name(ctx, obj, idx, &v)) return v;
		if (avm2_proxy_enumerant_name(ctx, obj, idx, &v)) return v;
		if (avm2_xml_enumerant_name(ctx, obj, idx, &v)) return v;
	}
	uint32_t arr_n = array_enum_count(obj);
	if (idx >= 1 && idx <= arr_n)
	{
		int64_t dense = array_nth_index(obj, idx);
		return avm2_uint_value((uint32_t) dense);
	}
	Avm2DynProp* p = (obj->dyn_enum_public == idx - arr_n
	                  && obj->dyn_enum_pos != NULL)
	                     ? obj->dyn_enum_pos
	                     : nth_enumerable_dyn(obj, idx - arr_n);
	if (p == NULL) return avm2_null();
	if (p->key_obj != NULL) return avm2_object_value(p->key_obj);
	// Names that are pure natural numbers (no sign, no leading zeros) and fit
	// in 28 bits enumerate as NUMBERS (Ruffle DynamicKey::Uint / avmplus int
	// atoms). avm2/primitive_keys is the fixture: it asserts typeof key ==
	// "number" below 2^28 and "string" from 2^28 up. Same predicate the
	// integer-keys-first partition uses, as in Ruffle.
	{
		uint32_t iv;
		if (dyn_key_index_value(p->name.utf8, p->name.len, &iv))
		{
			return avm2_uint_value(iv);
		}
	}
	return avm2_string(avm2_string_new(ctx, p->name.utf8, p->name.len));
}

Avm2Value avm2_object_enumerant_value(Avm2Context* ctx, Avm2Object* obj, uint32_t idx)
{
	if (obj->kind == AVM2_OBJ_VECTOR)
	{
		Avm2VectorExt* ext = (Avm2VectorExt*) obj->native_ext;
		if (idx >= 1 && idx <= ext->length) return ext->elems[idx - 1];
		return avm2_undefined();
	}
	{
		Avm2Value v;
		if (avm2_nsqname_enumerant_value(ctx, obj, idx, &v)) return v;
		if (avm2_proxy_enumerant_value(ctx, obj, idx, &v)) return v;
		if (avm2_xml_enumerant_value(ctx, obj, idx, &v)) return v;
	}
	uint32_t arr_n = array_enum_count(obj);
	if (idx >= 1 && idx <= arr_n)
	{
		int64_t dense = array_nth_index(obj, idx);
		Avm2Value v = avm2_array_get(obj, (uint32_t) dense);
		if (v.kind == AVM2_VALUE_HOLE) return avm2_undefined();
		return v;
	}
	Avm2DynProp* p = (obj->dyn_enum_public == idx - arr_n
	                  && obj->dyn_enum_pos != NULL)
	                     ? obj->dyn_enum_pos
	                     : nth_enumerable_dyn(obj, idx - arr_n);
	if (p == NULL) return avm2_undefined();
	return p->value;
}
