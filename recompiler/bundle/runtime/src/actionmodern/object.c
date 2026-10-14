#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <actionmodern/object.h>
#include <actionmodern/action.h>
#include <actionmodern/action_internal.h>  // ASFunction (Stage 3 collector traces prototype_obj/own_props/captured_scope)
#include <heap.h>
#include "unicode_case_tables.h"

// UTF-8 decode one codepoint, advance pointer
static uint32_t _obj_utf8_decode(const unsigned char** pp)
{
	const unsigned char* p = *pp;
	uint32_t c = *p;
	if (c < 0x80) { (*pp)++; return c; }
	if ((c & 0xE0) == 0xC0) {
		uint32_t r = (c & 0x1F) << 6;
		if ((p[1] & 0xC0) == 0x80) { r |= (p[1] & 0x3F); *pp += 2; return r; }
		(*pp)++; return 0xFFFD;
	}
	if ((c & 0xF0) == 0xE0) {
		uint32_t r = (c & 0x0F) << 12;
		if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
			r |= ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); *pp += 3; return r;
		}
		(*pp)++; return 0xFFFD;
	}
	if ((c & 0xF8) == 0xF0) {
		uint32_t r = (c & 0x07) << 18;
		if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
			r |= ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); *pp += 4; return r;
		}
		(*pp)++; return 0xFFFD;
	}
	(*pp)++; return 0xFFFD;
}

// Fold codepoint to lowercase for case-insensitive comparison
static uint32_t _obj_fold_lower(uint32_t c)
{
	if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
	if (c > 0x7F && c <= 0xFFFF) {
		int lo = 0, hi = CASE_MAP_UPPER_TO_LOWER_COUNT - 1;
		while (lo <= hi) {
			int mid = (lo + hi) / 2;
			if (case_map_upper_to_lower[mid][0] == (uint16_t)c) return case_map_upper_to_lower[mid][1];
			if (case_map_upper_to_lower[mid][0] < (uint16_t)c) lo = mid + 1; else hi = mid - 1;
		}
	}
	return c;
}

// SWF version-aware property name comparison
// For SWF <= 6: Unicode case-insensitive comparison
// For SWF >= 7: byte-exact comparison (strncmp)
static int prop_name_match(const char* a, u32 alen, const char* b, u32 blen)
{
	if (g_swf_version >= 7) {
		return alen == blen && strncmp(a, b, alen) == 0;
	}
	// SWF <= 6: Unicode case-insensitive
	const unsigned char* pa = (const unsigned char*)a;
	const unsigned char* ea = pa + alen;
	const unsigned char* pb = (const unsigned char*)b;
	const unsigned char* eb = pb + blen;
	while (pa < ea && pb < eb) {
		// ASCII fast path (property names are almost always pure ASCII): when
		// BOTH current bytes are ASCII, fold + compare at byte level without the
		// per-codepoint UTF-8 decode. For a <0x80 byte, _obj_utf8_decode returns
		// the byte unchanged and _obj_fold_lower only lowercases A-Z, so this is
		// bit-for-bit identical to the decode path below — just far cheaper.
		// Falls through to the full Unicode decode the moment either side is
		// non-ASCII, so multi-byte sequences still align correctly.
		unsigned char ba = *pa, bb = *pb;
		if (ba < 0x80 && bb < 0x80) {
			uint32_t ca = (ba >= 'A' && ba <= 'Z') ? (uint32_t)(ba + 32) : (uint32_t)ba;
			uint32_t cb = (bb >= 'A' && bb <= 'Z') ? (uint32_t)(bb + 32) : (uint32_t)bb;
			if (ca != cb) return 0;
			pa++; pb++;
			continue;
		}
		uint32_t ca = _obj_fold_lower(_obj_utf8_decode(&pa));
		uint32_t cb = _obj_fold_lower(_obj_utf8_decode(&pb));
		if (ca != cb) return 0;
	}
	return (pa >= ea && pb >= eb);
}

// Fast pre-filter hash for property-name lookups.
//
// Hashes the Unicode case-FOLDED codepoint sequence (the same fold used by
// prop_name_match's SWF<=6 path) so it is consistent with prop_name_match in
// BOTH version modes:
//   - SWF>=7 (exact match): byte-identical names fold identically => equal hash,
//     so a real match is never filtered out (no false negatives). Case-variant
//     names may collide, but prop_name_match's strncmp then rejects them.
//   - SWF<=6 (case-insensitive): names equal-under-fold hash equal, as required.
// It is ONLY a filter: a hash match is always confirmed by prop_name_match.
static u32 name_fold_hash(const char* name, u32 len)
{
	const unsigned char* p = (const unsigned char*)name;
	const unsigned char* e = p + len;
	u32 h = 2166136261u; // FNV-1a offset basis
	while (p < e)
	{
		unsigned char b = *p;
		if (b < 0x80)
		{
			// ASCII fast path (the overwhelmingly common case): lowercase
			// A-Z and mix one byte. Avoids the per-codepoint UTF-8 decode.
			uint32_t c = (b >= 'A' && b <= 'Z') ? (uint32_t)(b + 32) : (uint32_t)b;
			h = (h ^ c) * 16777619u;
			p++;
			continue;
		}
		// Non-ASCII: full Unicode decode + case fold (matches prop_name_match's
		// SWF<=6 path). Applied uniformly at insert and lookup, so equal-under-
		// match names always hash equal regardless of which path each char takes.
		uint32_t c = _obj_fold_lower(_obj_utf8_decode(&p));
		h = (h ^ (c & 0xff)) * 16777619u;
		h = (h ^ ((c >> 8) & 0xff)) * 16777619u;
		h = (h ^ ((c >> 16) & 0xff)) * 16777619u;
		h = (h ^ ((c >> 24) & 0xff)) * 16777619u;
	}
	return h;
}

// ---- Per-object property hash index ---------------------------------------
// Large objects (mainly prototypes and _global) get an open-addressing index
// mapping name_fold_hash -> slot in properties[], so lookups jump to the entry
// instead of scanning. Small objects keep the hash-gated linear scan (index
// stays NULL). The index stores slot indices into properties[]; since
// deleteProperty compacts the array (shifting indices), the index is rebuilt
// after a delete (deletes are rare). Array growth via realloc keeps slot
// indices valid (logical positions don't move), so it needs no index update.
#define PROP_HASH_EMPTY     0xFFFFFFFFu
#define PROP_HASH_THRESHOLD 12   // build an index once an object has >= this many props

static void prop_hash_insert(u32* table, u32 cap, u32 hash, u32 slot)
{
	u32 mask = cap - 1;
	u32 probe = hash & mask;
	while (table[probe] != PROP_HASH_EMPTY)
		probe = (probe + 1) & mask;
	table[probe] = slot;
}

// (Re)build the index sized for the current num_used. On allocation failure the
// index is left NULL and callers transparently fall back to the linear scan.
static void rebuildHashIndex(ASObject* obj)
{
	if (obj->hash_index)
	{
		free(obj->hash_index);
		obj->hash_index = NULL;
		obj->hash_capacity = 0;
	}
	u32 cap = 16;
	while (cap < obj->num_used * 2) cap <<= 1;   // keep load factor <= 0.5
	u32* table = (u32*) malloc(sizeof(u32) * cap);
	if (table == NULL) return;                    // fall back to linear scan
	for (u32 i = 0; i < cap; i++) table[i] = PROP_HASH_EMPTY;
	for (u32 i = 0; i < obj->num_used; i++)
	{
		if (obj->properties[i].name == NULL || (uintptr_t)obj->properties[i].name < 4096)
			continue;
		prop_hash_insert(table, cap, obj->properties[i].name_hash, i);
	}
	obj->hash_index = table;
	obj->hash_capacity = cap;
}

// Maintain the index after a new property was appended at `slot`. Builds the
// index when the object first crosses the size threshold, and grows it (rebuild)
// when the load factor would be exceeded.
static void hashIndexOnInsert(ASObject* obj, u32 slot)
{
	if (obj->hash_index != NULL)
	{
		if (obj->num_used * 2 > obj->hash_capacity)
			rebuildHashIndex(obj);   // grow + reinsert all (incl. the new slot)
		else
			prop_hash_insert(obj->hash_index, obj->hash_capacity,
			                 obj->properties[slot].name_hash, slot);
	}
	else if (obj->num_used >= PROP_HASH_THRESHOLD)
	{
		rebuildHashIndex(obj);
	}
}

// Core property lookup. Returns the slot index of the matching property or
// PROP_HASH_EMPTY. `qhash` must be name_fold_hash(name, name_length). Uses the
// hash index when present, else a hash-gated linear scan.
static u32 findPropertySlot(ASObject* obj, const char* name, u32 name_length, u32 qhash)
{
	if (obj->hash_index != NULL)
	{
		u32 mask = obj->hash_capacity - 1;
		for (u32 probe = qhash & mask; ; probe = (probe + 1) & mask)
		{
			u32 slot = obj->hash_index[probe];
			if (slot == PROP_HASH_EMPTY) return PROP_HASH_EMPTY;
			ASProperty* p = &obj->properties[slot];
			if (p->name_hash == qhash && p->name != NULL &&
			    prop_name_match(p->name, p->name_length, name, name_length))
				return slot;
		}
	}
	for (u32 i = 0; i < obj->num_used; i++)
	{
		if (obj->properties[i].name == NULL || (uintptr_t)obj->properties[i].name < 4096)
			continue;
		if (obj->properties[i].name_hash == qhash &&
		    prop_name_match(obj->properties[i].name, obj->properties[i].name_length, name, name_length))
			return i;
	}
	return PROP_HASH_EMPTY;
}

// Public hook: rebuild the index after code outside object.c has directly
// reordered/removed entries in properties[] (e.g. ensureBuiltinPrototypeProps).
// No-op when the object has no index. Must be called before any subsequent
// property lookup/insert on that object.
void objectRehashIndex(ASObject* obj)
{
	if (obj != NULL && obj->hash_index != NULL)
		rebuildHashIndex(obj);
}

// Null-terminated SWF version-aware name comparison (exposed for tag_stubs.c)
// Returns 1 if names match, 0 if they don't
int swf_name_match(const char* a, const char* b)
{
	if (g_swf_version >= 7) {
		return strcmp(a, b) == 0;
	}
	// SWF <= 6: Unicode case-insensitive
	const unsigned char* pa = (const unsigned char*)a;
	const unsigned char* pb = (const unsigned char*)b;
	while (*pa && *pb) {
		// ASCII fast path (see prop_name_match): byte-level fold when both sides
		// are ASCII — bit-identical to the decode path, far cheaper.
		unsigned char ba = *pa, bb = *pb;
		if (ba < 0x80 && bb < 0x80) {
			uint32_t ca = (ba >= 'A' && ba <= 'Z') ? (uint32_t)(ba + 32) : (uint32_t)ba;
			uint32_t cb = (bb >= 'A' && bb <= 'Z') ? (uint32_t)(bb + 32) : (uint32_t)bb;
			if (ca != cb) return 0;
			pa++; pb++;
			continue;
		}
		uint32_t ca = _obj_fold_lower(_obj_utf8_decode(&pa));
		uint32_t cb = _obj_fold_lower(_obj_utf8_decode(&pb));
		if (ca != cb) return 0;
	}
	return (*pa == 0 && *pb == 0);
}

// Version-based property hiding masks for ASSetPropFlags
// When (property->flash_flags & FLASH_HIDE_MASK) != 0, property is hidden from GetMember
static const u16 flash_hide_masks[] = {
	0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, // SWF 0-4
	0x7480, // SWF 5
	0x7500, // SWF 6
	0x7000, // SWF 7
	0x6000, // SWF 8
	0x4000, // SWF 9
	0x0000, // SWF 10+
};
#define FLASH_HIDE_MASK (g_swf_version <= 10 ? flash_hide_masks[g_swf_version] : 0)

// Check if a property with given flash_flags is hidden at the current SWF version
int isPropertyHiddenAtVersion(u16 flash_flags) {
	return (FLASH_HIDE_MASK != 0) && ((flash_flags & FLASH_HIDE_MASK) != 0);
}

/**
 * Memory-reclamation instrumentation (Stage 0 of
 * SWFRecompDocs/plans/memory-reclamation-plan.md)
 *
 * Always-on: total alloc/free counters plus an intrusive doubly-linked list
 * of every live ASObject/ASArray (two pointers per object, O(1) link/unlink).
 * The list doubles as the sweep infrastructure for the measurement-gated
 * Stage 3 collector. Reporting is opt-in via the SWF_MEM_REPORT env var
 * (stderr at exit, native builds), so default builds and CI output are
 * byte-identical.
 */
static u64 g_mt_obj_allocs = 0;
static u64 g_mt_obj_frees  = 0;
static u64 g_mt_arr_allocs = 0;
static u64 g_mt_arr_frees  = 0;
static ASObject* g_mt_obj_head = NULL;
static ASArray*  g_mt_arr_head = NULL;

u32 swfMemLiveObjects(void) { return (u32)(g_mt_obj_allocs - g_mt_obj_frees); }
u32 swfMemLiveArrays(void)  { return (u32)(g_mt_arr_allocs - g_mt_arr_frees); }

static void mtLinkObject(ASObject* obj)
{
	obj->mt_prev = NULL;
	obj->mt_next = g_mt_obj_head;
	if (g_mt_obj_head != NULL) g_mt_obj_head->mt_prev = obj;
	g_mt_obj_head = obj;
	g_mt_obj_allocs++;
}

static void mtUnlinkObject(ASObject* obj)
{
	if (obj->mt_prev != NULL) obj->mt_prev->mt_next = obj->mt_next;
	else g_mt_obj_head = obj->mt_next;
	if (obj->mt_next != NULL) obj->mt_next->mt_prev = obj->mt_prev;
	g_mt_obj_frees++;
}

static void mtLinkArray(ASArray* arr)
{
	arr->mt_prev = NULL;
	arr->mt_next = g_mt_arr_head;
	if (g_mt_arr_head != NULL) g_mt_arr_head->mt_prev = arr;
	g_mt_arr_head = arr;
	g_mt_arr_allocs++;
}

static void mtUnlinkArray(ASArray* arr)
{
	if (arr->mt_prev != NULL) arr->mt_prev->mt_next = arr->mt_next;
	else g_mt_arr_head = arr->mt_next;
	if (arr->mt_next != NULL) arr->mt_next->mt_prev = arr->mt_prev;
	g_mt_arr_frees++;
}

/**
 * Stage 3 collector: quarantine poison gate (SWF_GC=quarantine).
 *
 * Swept objects are poisoned (mt_kind = MT_KIND_GC_POISONED) and parked for a
 * few collections before the real free. Any runtime access to a poisoned
 * object means the collector swept something reachable — a missed GC root —
 * so the trap aborts loudly with the pointer and access site. The gate int
 * keeps the checks free when quarantine mode is off (one predictable branch).
 * Collector core is at the end of this file.
 */
int g_swf_gc_poison_active = 0;

void swfGcPoisonTrap(const void* p, const char* where)
{
	fprintf(stderr,
		"[swf-gc] FATAL: access to quarantined (swept) object %p via %s"
		" — missed GC root (memory-reclamation plan Stage 3 rollout)\n",
		p, where);
	fflush(stderr);
	abort();
}

#define GC_POISON_OBJ(o, where) \
	do { if (g_swf_gc_poison_active && (o) != NULL && (o)->mt_kind == MT_KIND_GC_POISONED) \
		swfGcPoisonTrap((o), (where)); } while (0)
#define GC_POISON_ARR(a, where) \
	do { if (g_swf_gc_poison_active && (a) != NULL && (a)->mt_kind == MT_KIND_GC_POISONED) \
		swfGcPoisonTrap((a), (where)); } while (0)

// Forward decl — GC stats line appended to swfMemReport (defined with the
// collector core at the end of this file).
static void gcReportLine(void);

// Forward decl — scrub borrowed C stashes (registers, timers, side tables)
// when refcounting frees an object, so the collector's root walk never reads
// freed memory through a dangling stash. No-op unless the collector is on.
static void gcNotifyRefcountFree(void* p);

/**
 * Classified live-set report (stderr).
 *
 * Crude attribution — it only needs to rank the three leak classes from the
 * plan: (a) detached dynamic_props, (b) arrays held only by object property
 * values (the borrowed-edge asymmetry), (c) everything else (cycle
 * candidates). "Attached" is computed against the MC registries at call time.
 */
void swfMemReport(void)
{
	// Mark pass: which live arrays are referenced by a live object's property
	// values, and which live objects are referenced by live object properties
	// / array elements (both directions only used for the summary).
	for (ASArray* a = g_mt_arr_head; a != NULL; a = a->mt_next) a->mt_mark = 0;
	for (ASObject* o = g_mt_obj_head; o != NULL; o = o->mt_next) o->mt_mark = 0;
	for (ASObject* o = g_mt_obj_head; o != NULL; o = o->mt_next)
	{
		for (u32 i = 0; i < o->num_used; i++)
		{
			if (o->properties == NULL) break;
			if (o->properties[i].value.type == ACTION_STACK_VALUE_ARRAY)
			{
				ASArray* a = (ASArray*) o->properties[i].value.data.numeric_value;
				if (a != NULL) a->mt_mark = 1;
			}
		}
	}

	// Attached dynamic_props set: root + levels + child MC registry.
	// (Uses mt_mark bit 2 on the object so we don't need a side table.)
	{
		extern MovieClip root_movieclip;
		extern MovieClip* child_mc_cache[];
		extern int child_mc_count;
		if (root_movieclip.dynamic_props != NULL)
			((ASObject*)root_movieclip.dynamic_props)->mt_mark |= 2;
		for (int i = 0; i < child_mc_count; i++)
		{
			if (child_mc_cache[i] != NULL && child_mc_cache[i]->dynamic_props != NULL)
				((ASObject*)child_mc_cache[i]->dynamic_props)->mt_mark |= 2;
		}
	}

	u32 live_obj = 0, dprops_attached = 0, dprops_detached = 0, plain_obj = 0;
	for (ASObject* o = g_mt_obj_head; o != NULL; o = o->mt_next)
	{
		live_obj++;
		if (o->mt_kind == MT_KIND_DPROPS)
		{
			if (o->mt_mark & 2) dprops_attached++;
			else dprops_detached++;
		}
		else plain_obj++;
	}

	u32 live_arr = 0, arr_as_obj_prop = 0, arr_other = 0;
	for (ASArray* a = g_mt_arr_head; a != NULL; a = a->mt_next)
	{
		live_arr++;
		if (a->mt_mark & 1) arr_as_obj_prop++;
		else arr_other++;
	}

	fprintf(stderr,
		"[swf-mem] objects: alloc=%llu free=%llu live=%u"
		" (dprops attached=%u detached=%u, plain=%u)\n",
		(unsigned long long)g_mt_obj_allocs, (unsigned long long)g_mt_obj_frees,
		live_obj, dprops_attached, dprops_detached, plain_obj);
	fprintf(stderr,
		"[swf-mem] arrays:  alloc=%llu free=%llu live=%u"
		" (held-as-obj-prop=%u other=%u)\n",
		(unsigned long long)g_mt_arr_allocs, (unsigned long long)g_mt_arr_frees,
		live_arr, arr_as_obj_prop, arr_other);
	gcReportLine();
}

// Env-gated wrapper for the end-of-run report. Called by the frame loops
// BEFORE heap_shutdown — the classifier walks the MC registry, and MovieClips
// live in the o1heap pool that heap_shutdown unmaps. An atexit FALLBACK
// covers abnormal exits (e.g. heap_alloc OOM calls exit(1) mid-run): it fires
// only if the loop-end report didn't run AND the pool is still mapped
// (heap_shutdown flips g_mt_walk_unsafe via swfMemMarkUnsafeToWalk).
static int g_mt_reported = 0;
static int g_mt_walk_unsafe = 0;

void swfMemMarkUnsafeToWalk(void)
{
	g_mt_walk_unsafe = 1;
}

#ifndef __EMSCRIPTEN__
static void mtAtexitFallbackReport(void)
{
	if (!g_mt_reported && !g_mt_walk_unsafe)
		swfMemReport();
}
#endif

void swfMemReportAtExitIfEnabled(void)
{
#ifndef __EMSCRIPTEN__
	if (getenv("SWF_MEM_REPORT") != NULL)
		swfMemReport();
	g_mt_reported = 1;
#endif
}

/**
 * Object Allocation
 *
 * Allocates a new ASObject with the specified initial capacity.
 * Returns object with refcount = 1 (caller owns the initial reference).
 */
ASObject* allocObject(SWFAppContext* app_context, u32 initial_capacity)
{
	ASObject* obj = (ASObject*) malloc(sizeof(ASObject));
	if (obj == NULL)
	{
		fprintf(stderr, "ERROR: Failed to allocate ASObject\n");
		return NULL;
	}

	obj->refcount = 1;  // Initial reference owned by caller
	obj->num_properties = initial_capacity;
	obj->num_used = 0;
	obj->hash_index = NULL;
	obj->hash_capacity = 0;

	// Initialize interface fields
	obj->interface_count = 0;
	obj->interfaces = NULL;

	// Initialize native type (NATIVE_NONE = pure ActionScript object)
	obj->native_type = NATIVE_NONE;

	// Allocate property array
	if (initial_capacity > 0)
	{
		obj->properties = (ASProperty*) malloc(sizeof(ASProperty) * initial_capacity);
		if (obj->properties == NULL)
		{
			fprintf(stderr, "ERROR: Failed to allocate property array\n");
			free(obj);
			return NULL;
		}

		// Initialize properties to zero
		memset(obj->properties, 0, sizeof(ASProperty) * initial_capacity);
	}
	else
	{
		obj->properties = NULL;
	}

	obj->mt_kind = MT_KIND_PLAIN;
	obj->mt_mark = 0;
	obj->mt_pending = 0;
	obj->mt_pending_idx = 0;
	mtLinkObject(obj);

#ifndef __EMSCRIPTEN__
	// Register the abnormal-exit report fallback once (env-gated; see above).
	{
		static int mt_exit_checked = 0;
		if (!mt_exit_checked) {
			mt_exit_checked = 1;
			if (getenv("SWF_MEM_REPORT") != NULL)
				atexit(mtAtexitFallbackReport);
		}
	}
#endif

#ifdef DEBUG
	printf("[DEBUG] allocObject: obj=%p, refcount=%u, capacity=%u\n",
		(void*)obj, obj->refcount, obj->num_properties);
#endif

	return obj;
}

// allocObject tagged as a MovieClip dynamic_props allocation (leak attribution).
ASObject* allocDynamicProps(SWFAppContext* app_context, u32 initial_capacity)
{
	ASObject* obj = allocObject(app_context, initial_capacity);
	if (obj != NULL) obj->mt_kind = MT_KIND_DPROPS;
	return obj;
}

/**
 * Retain Object
 *
 * Increments the reference count of an object.
 * Called when storing object in a variable, property, or array.
 */
void retainObject(ASObject* obj)
{
	if (obj == NULL)
	{
		return;
	}
	GC_POISON_OBJ(obj, "retainObject");

	obj->refcount++;

#ifdef DEBUG
	printf("[DEBUG] retainObject: obj=%p, refcount=%u -> %u\n",
		(void*)obj, obj->refcount - 1, obj->refcount);
#endif
}

// --- Deferred destruction (tick boundary) -----------------------------------
//
// AVM1 script-visible values borrow ASObject*/ASArray* WITHOUT holding a
// reference count: locals and registers live in `ActionVar regs[]` arrays that
// are plain C locals inside generated functions, so they are invisible to both
// refcounting and actionGcScrubStashes (which only reaches the four global
// g_registers). A runtime path that drops an object's last COUNTED reference
// mid-script therefore frees memory the script can still name. Flash keeps any
// object reachable from a live local alive, and draws no distinction between a
// built-in prototype and a user object, so the borrow must not be fatal.
//
// avm1/global_proto_decls is the corpus case — testDecls() save/restores every
// property it enumerates:
//
//     var old = object[key];   // borrows; refcount unchanged
//     object[key] = "OTHER";   // ONE statement drops BOTH runtime refs -> freed
//     object[key] = old;       // -> retainObject() on freed memory
//
// (TextFormat.prototype is held twice — ASFunction.prototype_obj and
// own_props["prototype"] — and actionSetMember's non-object-prototype branch
// releases both: action.c:47832 then setProperty at action.c:47839.)
//
// Fix: reaching refcount 0 queues the struct instead of tearing it down. The
// queue drains at the tick boundary, where the VM is quiescent — the same
// reasoning and the same moment as actionDeferDpropsRelease (a38bbe7ea), whose
// commit message states the rule: "script can still hold borrowed references
// when a detach happens mid-handler... At the tick boundary the VM is
// quiescent, and destruction is pure reclamation, so deferring is
// script-invisible." If script resurrects a queued object before the drain, its
// refcount is back above zero and the drain simply unparks it.
//
// Destruction during the drain stays INLINE-RECURSIVE, exactly as before this
// change. That is load-bearing for reference cycles: A->B->A with both at zero
// only survives because A is still allocated while B's teardown releases it.
// Queueing children instead would free A first and make B's teardown a UAF.
// The cost is that a child destroyed inline may already sit in the queue, so
// it tombstones its own slot (mt_pending_idx) on the way out.
typedef struct { void* p; u8 is_array; } PendingDestroy;
static PendingDestroy* g_pending_destroy = NULL;
static int g_pending_destroy_count = 0;
static int g_pending_destroy_capacity = 0;
static int g_in_destroy_drain = 0;

// Returns 1 if queued, 0 on OOM (caller destroys inline = pre-fix behavior).
static int pendingDestroyPush(void* p, u8 is_array, u32* out_idx)
{
	if (g_pending_destroy_count >= g_pending_destroy_capacity)
	{
		int ncap = g_pending_destroy_capacity ? g_pending_destroy_capacity * 2 : 64;
		PendingDestroy* nq = (PendingDestroy*) realloc(
			g_pending_destroy, (size_t)ncap * sizeof(PendingDestroy));
		if (nq == NULL) return 0;
		g_pending_destroy = nq;
		g_pending_destroy_capacity = ncap;
	}
	*out_idx = (u32) g_pending_destroy_count;
	g_pending_destroy[g_pending_destroy_count].p = p;
	g_pending_destroy[g_pending_destroy_count].is_array = is_array;
	g_pending_destroy_count++;
	return 1;
}

void swfDrainPendingDestroys(SWFAppContext* app_context)
{
	if (g_pending_destroy_count == 0) return;
	g_in_destroy_drain = 1;
	// Index walk, not a pop loop. Nothing is appended below — deferral is off
	// for the duration, so a child reaching zero is destroyed inline — but a
	// child destroyed that way may itself be queued, and it tombstones its own
	// slot on the way out. Walking by index (never removing slots) is what
	// keeps the recorded mt_pending_idx values valid.
	for (int i = 0; i < g_pending_destroy_count; i++)
	{
		PendingDestroy e = g_pending_destroy[i];
		if (e.p == NULL) continue;              // already destroyed as a child
		if (e.is_array)
		{
			ASArray* a = (ASArray*) e.p;
			a->mt_pending = 0;
			if (a->refcount != 0) continue;     // script resurrected it
			a->refcount = 1;                    // so the release below lands on 0
			releaseArray(app_context, a);
		}
		else
		{
			ASObject* o = (ASObject*) e.p;
			o->mt_pending = 0;
			if (o->refcount != 0) continue;     // script resurrected it
			o->refcount = 1;
			releaseObject(app_context, o);
		}
	}
	g_pending_destroy_count = 0;
	g_in_destroy_drain = 0;
}

/**
 * Release Object
 *
 * Decrements the reference count of an object.
 * When refcount reaches 0, queues the object for tick-boundary destruction
 * (see the deferred-destruction note above), which then frees it and all its
 * properties, recursively releasing any objects stored in properties.
 */
void releaseObject(SWFAppContext* app_context, ASObject* obj)
{
	if (obj == NULL)
	{
		return;
	}
	GC_POISON_OBJ(obj, "releaseObject");

#ifdef DEBUG
	printf("[DEBUG] releaseObject: obj=%p, refcount=%u -> %u\n",
		(void*)obj, obj->refcount, obj->refcount - 1);
#endif

	obj->refcount--;

	if (obj->refcount == 0)
	{
		// Outside the drain, park instead of destroying: a script local may
		// still name this object (see the deferred-destruction note above).
		if (!g_in_destroy_drain)
		{
			if (obj->mt_pending) return;
			u32 idx;
			if (pendingDestroyPush(obj, 0, &idx))
			{
				obj->mt_pending = 1;
				obj->mt_pending_idx = idx;
				return;
			}
			// Queue OOM: fall through and destroy inline (pre-fix behavior).
		}
		// Destroying inline while queued (cycle child): tombstone the slot so
		// the drain does not touch this struct after it is freed.
		if (obj->mt_pending)
		{
			g_pending_destroy[obj->mt_pending_idx].p = NULL;
			obj->mt_pending = 0;
		}
#ifdef DEBUG
		printf("[DEBUG] releaseObject: obj=%p reached refcount=0, freeing\n", (void*)obj);
#endif

		// Release all property values
		for (u32 i = 0; i < obj->num_used; i++)
		{
			// Free property name (always heap-allocated)
			if (obj->properties[i].name != NULL)
			{
				FREE(obj->properties[i].name);
			}

			// If property value is an object, release it recursively
			if (obj->properties[i].value.type == ACTION_STACK_VALUE_OBJECT)
			{
				ASObject* child_obj = (ASObject*) obj->properties[i].value.data.numeric_value;
				releaseObject(app_context, child_obj);
			}
			// If property value is an array, release it recursively (Stage 1b:
			// property stores retain arrays, so destruction must balance)
			else if (obj->properties[i].value.type == ACTION_STACK_VALUE_ARRAY)
			{
				ASArray* child_arr = (ASArray*) obj->properties[i].value.data.numeric_value;
				releaseArray(app_context, child_arr);
			}
			// If property value is a string that owns memory, free it
			else if (obj->properties[i].value.type == ACTION_STACK_VALUE_STRING &&
			         obj->properties[i].value.data.string_data.owns_memory)
			{
				free(obj->properties[i].value.data.string_data.heap_ptr);
			}
		}

		// Free property array
		if (obj->properties != NULL)
		{
			free(obj->properties);
		}

		// Free the property hash index
		if (obj->hash_index != NULL)
		{
			free(obj->hash_index);
		}

		// Release interface objects
		if (obj->interfaces != NULL)
		{
			for (u32 i = 0; i < obj->interface_count; i++)
			{
				releaseObject(app_context, obj->interfaces[i]);
			}
			free(obj->interfaces);
		}

		// Free object itself
		gcNotifyRefcountFree(obj);
		mtUnlinkObject(obj);
		free(obj);
	}
}

/**
 * Get Property
 *
 * Retrieves a property value by name.
 * Returns pointer to ActionVar, or NULL if property not found.
 */
// Find property struct by name (ignoring version hiding) - for ASSetPropFlags
ASProperty* findPropertyRaw(ASObject* obj, const char* name, u32 name_length)
{
	if (obj == NULL || name == NULL) return NULL;
	GC_POISON_OBJ(obj, "findPropertyRaw");
	if (obj->num_used > 16384 || (obj->num_used > 0 && obj->properties == NULL)) return NULL;
	u32 s = findPropertySlot(obj, name, name_length, name_fold_hash(name, name_length));
	return (s == PROP_HASH_EMPTY) ? NULL : &obj->properties[s];
}

// Like findPropertyRaw but takes a precomputed query hash, so prototype-chain
// walkers can hash the name once per access instead of once per chain level.
static ASProperty* findPropertyRawH(ASObject* obj, const char* name, u32 name_length, u32 qhash)
{
	if (obj == NULL || name == NULL) return NULL;
	if (obj->num_used > 16384 || (obj->num_used > 0 && obj->properties == NULL)) return NULL;
	u32 s = findPropertySlot(obj, name, name_length, qhash);
	return (s == PROP_HASH_EMPTY) ? NULL : &obj->properties[s];
}

bool hasPropertyRaw(ASObject* obj, const char* name, u32 name_length)
{
	return findPropertyRaw(obj, name, name_length) != NULL;
}

ActionVar* getProperty(ASObject* obj, const char* name, u32 name_length)
{
	if (obj == NULL || name == NULL)
	{
		return NULL;
	}
	GC_POISON_OBJ(obj, "getProperty");

	// Safety: reject obviously corrupt objects (garbage num_used or NULL properties with nonzero count)
	if (obj->num_used > 16384 || (obj->num_used > 0 && obj->properties == NULL))
	{
		return NULL;
	}

	u32 s = findPropertySlot(obj, name, name_length, name_fold_hash(name, name_length));
	if (s == PROP_HASH_EMPTY)
		return NULL;  // Property not found
	// Check version-based hiding (ASSetPropFlags)
	if (FLASH_HIDE_MASK && (obj->properties[s].flash_flags & FLASH_HIDE_MASK))
		return NULL;  // Property hidden by version flags
	return &obj->properties[s].value;
}

/**
 * Get Property With Prototype Chain
 *
 * Retrieves a property value by name, searching up the prototype chain via __proto__.
 * Returns pointer to ActionVar, or NULL if property not found in entire chain.
 *
 * This implements proper prototype-based inheritance for ActionScript.
 */
ActionVar* getPropertyWithPrototype(ASObject* obj, const char* name, u32 name_length)
{
	if (obj == NULL || name == NULL)
	{
		return NULL;
	}
	GC_POISON_OBJ(obj, "getPropertyWithPrototype");

	// Safety: reject obviously corrupt objects
	if (obj->num_used > 16384 || (obj->num_used > 0 && obj->properties == NULL))
		return NULL;

	ASObject* current = obj;
	int max_depth = 256;  // Prevent infinite loops in deep prototype chains
	int depth = 0;
	u32 qhash = name_fold_hash(name, name_length);   // hashed once for the whole walk
	u32 proto_h = name_fold_hash("__proto__", 9);

	while (current != NULL && depth < max_depth)
	{
		depth++;

		if (current->num_used > 16384 || (current->num_used > 0 && current->properties == NULL))
			break;

		// Search own properties first (mirrors getProperty, incl. version hiding)
		u32 s = findPropertySlot(current, name, name_length, qhash);
		if (s != PROP_HASH_EMPTY &&
		    !(FLASH_HIDE_MASK && (current->properties[s].flash_flags & FLASH_HIDE_MASK)))
		{
			return &current->properties[s].value;
		}

		// Property not found on this object - walk up to __proto__
		u32 ps = findPropertySlot(current, "__proto__", 9, proto_h);
		ActionVar* proto_var = NULL;
		if (ps != PROP_HASH_EMPTY &&
		    !(FLASH_HIDE_MASK && (current->properties[ps].flash_flags & FLASH_HIDE_MASK)))
			proto_var = &current->properties[ps].value;
		ASObject* next = resolveProtoVar(proto_var);
		if (next == NULL)
		{
			// No __proto__ property or not resolvable - end of chain
			break;
		}

		// Cycle detection: if we'd revisit the original object, it's circular
		if (next == obj)
		{
			g_execution_halted = 1;
			return NULL;
		}

		current = next;
	}

	// Depth limit exceeded — treat as recursion limit error (halt execution)
	if (depth >= max_depth)
	{
		g_execution_halted = 1;
	}

	return NULL;  // Property not found in entire prototype chain
}

/**
 * Find Property Struct With Prototype Chain
 *
 * Like getPropertyWithPrototype but returns the ASProperty struct instead of just the value.
 * This is needed to check for getter/setter (addProperty virtual properties).
 * Does NOT apply flash_flags hiding — that's handled by the caller.
 */
ASProperty* findPropertyStructWithPrototype(ASObject* obj, const char* name, u32 name_length)
{
	if (obj == NULL || name == NULL) return NULL;
	GC_POISON_OBJ(obj, "findPropertyStructWithPrototype");

	ASObject* current = obj;
	int max_depth = 256;
	int depth = 0;
	u32 qhash = name_fold_hash(name, name_length);   // hashed once for the whole walk
	u32 proto_h = name_fold_hash("__proto__", 9);

	while (current != NULL && depth < max_depth)
	{
		depth++;

		if (current->num_used > 16384 || (current->num_used > 0 && current->properties == NULL))
			break;

		// Search own properties
		u32 s = findPropertySlot(current, name, name_length, qhash);
		if (s != PROP_HASH_EMPTY)
		{
			ASProperty* prop = &current->properties[s];
			// Check version-based hiding
			if (FLASH_HIDE_MASK && (prop->flash_flags & FLASH_HIDE_MASK))
				return NULL;
			return prop;
		}

		// Walk up __proto__
		u32 ps = findPropertySlot(current, "__proto__", 9, proto_h);
		ActionVar* proto_var = NULL;
		if (ps != PROP_HASH_EMPTY &&
		    !(FLASH_HIDE_MASK && (current->properties[ps].flash_flags & FLASH_HIDE_MASK)))
			proto_var = &current->properties[ps].value;
		ASObject* next = resolveProtoVar(proto_var);
		if (next == NULL)
			break;
		if (next == obj)
		{
			g_execution_halted = 1;
			return NULL;
		}
		current = next;
	}

	if (depth >= max_depth) g_execution_halted = 1;
	return NULL;
}

/**
 * Set Property
 *
 * Sets a property value by name. Creates property if it doesn't exist.
 * Handles reference counting if value is an object.
 */
void setProperty(SWFAppContext* app_context, ASObject* obj, const char* name, u32 name_length, ActionVar* value)
{
	if (obj == NULL || name == NULL || value == NULL || name_length == 0)
	{
		return;
	}
	GC_POISON_OBJ(obj, "setProperty");

	// Check if property already exists
	u32 qhash = name_fold_hash(name, name_length);
	u32 found = findPropertySlot(obj, name, name_length, qhash);
	if (found != PROP_HASH_EMPTY)
	{
		ASProperty* p = &obj->properties[found];
		// Property exists - update value

		// Retain the new value BEFORE releasing the old one: self-assignment
		// (obj.a = obj.a with an array/object value) must not drop the count
		// to zero in between.
		if (value->type == ACTION_STACK_VALUE_OBJECT)
		{
			ASObject* new_obj = (ASObject*) value->data.numeric_value;
			retainObject(new_obj);
		}
		else if (value->type == ACTION_STACK_VALUE_ARRAY)
		{
			ASArray* new_arr = (ASArray*) value->data.numeric_value;
			retainArray(new_arr);
		}

		// Release old value if it was an object or array
		if (p->value.type == ACTION_STACK_VALUE_OBJECT)
		{
			ASObject* old_obj = (ASObject*) p->value.data.numeric_value;
			releaseObject(app_context, old_obj);
		}
		else if (p->value.type == ACTION_STACK_VALUE_ARRAY)
		{
			ASArray* old_arr = (ASArray*) p->value.data.numeric_value;
			releaseArray(app_context, old_arr);
		}
		// Free old string if it owned memory
		else if (p->value.type == ACTION_STACK_VALUE_STRING &&
		         p->value.data.string_data.owns_memory)
		{
			free(p->value.data.string_data.heap_ptr);
		}

		// Set new value and clear version-based hiding flags
		// (In Flash, setting a property via SetMember clears ASSetPropFlags visibility)
		p->value = *value;
		p->flash_flags = 0;

		return;
	}

	// Property doesn't exist - create new one

	// Check if we need to grow the property array
	if (obj->num_used >= obj->num_properties)
	{
		// Grow by 50% or at least 4 slots, ensuring at least +1
		u32 grow = (obj->num_properties * 3) / 2;
		u32 new_capacity = grow > obj->num_properties + 1 ? grow : obj->num_properties + 4;
		ASProperty* new_props = (ASProperty*) realloc(obj->properties,
		                                               sizeof(ASProperty) * new_capacity);
		if (new_props == NULL)
		{
			fprintf(stderr, "ERROR: Failed to grow property array\n");
			return;
		}

		obj->properties = new_props;
		obj->num_properties = new_capacity;

		// Zero out new slots
		memset(&obj->properties[obj->num_used], 0,
		       sizeof(ASProperty) * (new_capacity - obj->num_used));
	}

	// Add new property
	u32 index = obj->num_used;
	obj->num_used++;

	// Allocate and copy property name
	obj->properties[index].name = (char*) HALLOC(name_length + 1);
	if (obj->properties[index].name == NULL)
	{
		fprintf(stderr, "ERROR: Failed to allocate property name\n");
		obj->num_used--;
		return;
	}
	memcpy(obj->properties[index].name, name, name_length);
	obj->properties[index].name[name_length] = '\0';
	obj->properties[index].name_length = name_length;
	obj->properties[index].name_hash = name_fold_hash(name, name_length);

	// Set default property flags (enumerable, writable, configurable)
	obj->properties[index].flags = PROPERTY_FLAGS_DEFAULT;

	// __proto__ and constructor are DontEnum (not enumerable in for-in)
	if ((name_length == 9 && strncmp(name, "__proto__", 9) == 0) ||
	    (name_length == 11 && strncmp(name, "constructor", 11) == 0))
	{
		obj->properties[index].flags &= ~PROPERTY_FLAG_ENUMERABLE;
	}

	// Initialize getter/setter to NULL (no virtual property)
	obj->properties[index].getter = NULL;
	obj->properties[index].setter = NULL;

	// Set value
	obj->properties[index].value = *value;

	// Retain if value is an object or array
	if (value->type == ACTION_STACK_VALUE_OBJECT)
	{
		ASObject* new_obj = (ASObject*) value->data.numeric_value;
		retainObject(new_obj);
	}
	else if (value->type == ACTION_STACK_VALUE_ARRAY)
	{
		ASArray* new_arr = (ASArray*) value->data.numeric_value;
		retainArray(new_arr);
	}

	// Maintain the lookup index (builds it once the object grows large enough)
	hashIndexOnInsert(obj, index);

#ifdef DEBUG
	printf("[DEBUG] setProperty: obj=%p, created property '%.*s', num_used=%u\n",
		(void*)obj, name_length, name, obj->num_used);
#endif
}

/**
 * Set Property With Flags
 *
 * Like setProperty but allows specifying explicit property flags on creation.
 * If the property already exists, updates the value (does NOT change flags).
 */
void setPropertyWithFlags(SWFAppContext* app_context, ASObject* obj, const char* name, u32 name_length, ActionVar* value, u8 flags)
{
	if (obj == NULL || name == NULL || value == NULL || name_length == 0) return;
	GC_POISON_OBJ(obj, "setPropertyWithFlags");

	// If property already exists, just update the value
	u32 qhash = name_fold_hash(name, name_length);
	u32 found = findPropertySlot(obj, name, name_length, qhash);
	if (found != PROP_HASH_EMPTY)
	{
		ASProperty* p = &obj->properties[found];
		// Retain new before releasing old (self-assignment safety), then
		// release the old object/array value.
		if (value->type == ACTION_STACK_VALUE_OBJECT)
			retainObject((ASObject*) value->data.numeric_value);
		else if (value->type == ACTION_STACK_VALUE_ARRAY)
			retainArray((ASArray*) value->data.numeric_value);

		if (p->value.type == ACTION_STACK_VALUE_OBJECT)
			releaseObject(app_context, (ASObject*) p->value.data.numeric_value);
		else if (p->value.type == ACTION_STACK_VALUE_ARRAY)
			releaseArray(app_context, (ASArray*) p->value.data.numeric_value);
		else if (p->value.type == ACTION_STACK_VALUE_STRING &&
		         p->value.data.string_data.owns_memory)
			free(p->value.data.string_data.heap_ptr);

		p->value = *value;
		p->flash_flags = 0;
		return;
	}

	// Property doesn't exist — use setProperty to create it, then override flags
	setProperty(app_context, obj, name, name_length, value);

	// Override flags on the newly created property (last in array)
	if (obj->num_used > 0)
	{
		ASProperty* new_prop = &obj->properties[obj->num_used - 1];
		if (prop_name_match(new_prop->name, new_prop->name_length, name, name_length))
		{
			new_prop->flags = flags;
		}
	}
}

/**
 * Delete Property
 *
 * Deletes a property by name. Returns true if deleted or not found (Flash behavior).
 * Handles reference counting if value is an object/array.
 */
bool deleteProperty(SWFAppContext* app_context, ASObject* obj, const char* name, u32 name_length)
{
	if (obj == NULL || name == NULL)
	{
		return true;  // Flash behavior: delete on null returns true
	}
	GC_POISON_OBJ(obj, "deleteProperty");

	// Find property by name
	u32 qhash = name_fold_hash(name, name_length);
	u32 i = findPropertySlot(obj, name, name_length, qhash);
	if (i != PROP_HASH_EMPTY)
	{
		// Check if property is configurable (deletable)
		if (!(obj->properties[i].flags & PROPERTY_FLAG_CONFIGURABLE))
		{
			return false;  // Cannot delete non-configurable property
		}

		// Property found - delete it

		// 1. Release the property value if it's an object/array
		if (obj->properties[i].value.type == ACTION_STACK_VALUE_OBJECT)
		{
			ASObject* child_obj = (ASObject*) obj->properties[i].value.data.numeric_value;
			releaseObject(app_context, child_obj);
		}
		else if (obj->properties[i].value.type == ACTION_STACK_VALUE_ARRAY)
		{
			ASArray* child_arr = (ASArray*) obj->properties[i].value.data.numeric_value;
			releaseArray(app_context, child_arr);
		}
		// Free string if it owns memory
		else if (obj->properties[i].value.type == ACTION_STACK_VALUE_STRING &&
		         obj->properties[i].value.data.string_data.owns_memory)
		{
			free(obj->properties[i].value.data.string_data.heap_ptr);
		}

		// 2. Free the property name
		if (obj->properties[i].name != NULL)
		{
			FREE(obj->properties[i].name);
		}

		// 3. Shift remaining properties down to fill the gap
		for (u32 j = i; j < obj->num_used - 1; j++)
		{
			obj->properties[j] = obj->properties[j + 1];
		}

		// 4. Decrement the number of used slots
		obj->num_used--;

		// 5. Zero out the last slot
		memset(&obj->properties[obj->num_used], 0, sizeof(ASProperty));

		// 6. Compaction shifted slot indices — rebuild the lookup index.
		if (obj->hash_index != NULL)
			rebuildHashIndex(obj);

		return true;
	}

	// Property not found - Flash AS2 returns false for non-existent properties
#ifdef DEBUG
	printf("[DEBUG] deleteProperty: obj=%p, property '%.*s' not found (returning false)\n",
		(void*)obj, name_length, name);
#endif

	return false;
}

/**
 * Interface Management (ActionScript 2.0)
 */

/**
 * Set Interface List
 *
 * Sets the list of interfaces that a constructor implements.
 * Takes ownership of the interfaces array.
 * Called by ActionImplementsOp (0x2C).
 */
void setInterfaceList(SWFAppContext* app_context, ASObject* constructor, ASObject** interfaces, u32 count)
{
	if (constructor == NULL)
	{
		// Free interfaces array if constructor is NULL
		if (interfaces != NULL)
		{
			for (u32 i = 0; i < count; i++)
			{
				releaseObject(app_context, interfaces[i]);
			}
			free(interfaces);
		}
		return;
	}

	// Release old interfaces if they exist
	if (constructor->interfaces != NULL)
	{
		for (u32 i = 0; i < constructor->interface_count; i++)
		{
			releaseObject(app_context, constructor->interfaces[i]);
		}
		free(constructor->interfaces);
	}

	// Set new interfaces
	constructor->interfaces = interfaces;
	constructor->interface_count = count;

	// Retain each interface object
	if (interfaces != NULL)
	{
		for (u32 i = 0; i < count; i++)
		{
			retainObject(interfaces[i]);
		}
	}

#ifdef DEBUG
	printf("[DEBUG] setInterfaceList: constructor=%p, interface_count=%u\n",
		(void*)constructor, count);
#endif
}

/**
 * Implements Interface
 *
 * Check if an object implements a specific interface.
 * Returns 1 if the object's constructor implements the interface, 0 otherwise.
 * Performs recursive check for interface inheritance.
 */
int implementsInterface(ASObject* obj, ASObject* interface_ctor)
{
	if (obj == NULL || interface_ctor == NULL)
	{
		return 0;
	}

	// Get the object's constructor
	ASObject* obj_ctor = getConstructor(obj);
	if (obj_ctor == NULL)
	{
		return 0;
	}

	// Check if constructor implements the interface
	for (u32 i = 0; i < obj_ctor->interface_count; i++)
	{
		// Direct match
		if (obj_ctor->interfaces[i] == interface_ctor)
		{
			return 1;
		}

		// Recursive check for interface inheritance
		// (interfaces can extend other interfaces)
		if (implementsInterface(obj_ctor->interfaces[i], interface_ctor))
		{
			return 1;
		}
	}

	return 0;
}

/**
 * Get Constructor
 *
 * Get the constructor function for an object.
 * Returns the "constructor" property if it exists, NULL otherwise.
 */
ASObject* getConstructor(ASObject* obj)
{
	if (obj == NULL)
	{
		return NULL;
	}

	// Look for "constructor" property
	static const char* constructor_name = "constructor";
	ActionVar* constructor_var = getProperty(obj, constructor_name, strlen(constructor_name));

	if (constructor_var != NULL && constructor_var->type == ACTION_STACK_VALUE_OBJECT)
	{
		return (ASObject*) constructor_var->data.numeric_value;
	}

	return NULL;
}

/**
 * Debug Functions
 */

#ifdef DEBUG
void assertRefcount(ASObject* obj, u32 expected)
{
	if (obj == NULL)
	{
		fprintf(stderr, "ERROR: assertRefcount called with NULL object\n");
		assert(0);
	}

	if (obj->refcount != expected)
	{
		fprintf(stderr, "ERROR: refcount assertion failed: expected %u, got %u\n",
			expected, obj->refcount);
		assert(0);
	}

	printf("[DEBUG] assertRefcount: obj=%p, refcount=%u (OK)\n", (void*)obj, expected);
}

void printObject(ASObject* obj)
{
	if (obj == NULL)
	{
		printf("Object: NULL\n");
		return;
	}

	printf("Object: %p\n", (void*)obj);
	printf("  refcount: %u\n", obj->refcount);
	printf("  num_properties: %u\n", obj->num_properties);
	printf("  num_used: %u\n", obj->num_used);
	printf("  properties:\n");

	for (u32 i = 0; i < obj->num_used; i++)
	{
		printf("    [%u] '%.*s' = ",
			i, obj->properties[i].name_length, obj->properties[i].name);

		switch (obj->properties[i].value.type)
		{
			case ACTION_STACK_VALUE_F32:
				printf("%.15g (F32)\n", *((float*)&obj->properties[i].value.data.numeric_value));
				break;

			case ACTION_STACK_VALUE_F64:
				printf("%.15g (F64)\n", *((double*)&obj->properties[i].value.data.numeric_value));
				break;

			case ACTION_STACK_VALUE_STRING:
			{
				const char* str = obj->properties[i].value.data.string_data.owns_memory ?
					obj->properties[i].value.data.string_data.heap_ptr :
					(const char*)obj->properties[i].value.data.numeric_value;
				printf("'%.*s' (STRING)\n", obj->properties[i].value.str_size, str);
				break;
			}

			case ACTION_STACK_VALUE_OBJECT:
				printf("%p (OBJECT)\n", (void*)obj->properties[i].value.data.numeric_value);
				break;

			default:
				printf("(unknown type %d)\n", obj->properties[i].value.type);
				break;
		}
	}
}

void printArray(ASArray* arr)
{
	if (arr == NULL)
	{
		printf("Array: NULL\n");
		return;
	}

	printf("Array: %p\n", (void*)arr);
	printf("  refcount: %u\n", arr->refcount);
	printf("  length: %u\n", arr->length);
	printf("  capacity: %u\n", arr->capacity);
	printf("  elements:\n");

	for (u32 i = 0; i < arr->length; i++)
	{
		printf("    [%u] = ", i);

		switch (arr->elements[i].type)
		{
			case ACTION_STACK_VALUE_F32:
				printf("%.15g (F32)\n", *((float*)&arr->elements[i].data.numeric_value));
				break;

			case ACTION_STACK_VALUE_F64:
				printf("%.15g (F64)\n", *((double*)&arr->elements[i].data.numeric_value));
				break;

			case ACTION_STACK_VALUE_STRING:
			{
				const char* str = arr->elements[i].data.string_data.owns_memory ?
					arr->elements[i].data.string_data.heap_ptr :
					(const char*)arr->elements[i].data.numeric_value;
				printf("'%.*s' (STRING)\n", arr->elements[i].str_size, str);
				break;
			}

			case ACTION_STACK_VALUE_OBJECT:
				printf("%p (OBJECT)\n", (void*)arr->elements[i].data.numeric_value);
				break;

			case ACTION_STACK_VALUE_ARRAY:
				printf("%p (ARRAY)\n", (void*)arr->elements[i].data.numeric_value);
				break;

			default:
				printf("(unknown type %d)\n", arr->elements[i].type);
				break;
		}
	}
}
#endif

/**
 * Array Implementation
 */

// Track key insertion order for Flash-compatible for-in enumeration.
// Only adds the key if it's not already in the list (first-write wins).
void arrayTrackKey(ASArray* arr, const char* key, u32 key_len)
{
	// Check if already tracked
	for (u32 i = 0; i < arr->enum_count; i++)
	{
		if (strlen(arr->enum_keys[i]) == key_len &&
		    memcmp(arr->enum_keys[i], key, key_len) == 0)
			return;  // Already tracked
	}

	// Grow if needed
	if (arr->enum_count >= arr->enum_capacity)
	{
		u32 new_cap = arr->enum_capacity == 0 ? 8 : arr->enum_capacity * 2;
		char** new_keys = (char**) realloc(arr->enum_keys, sizeof(char*) * new_cap);
		if (new_keys == NULL) return;
		arr->enum_keys = new_keys;
		arr->enum_capacity = new_cap;
	}

	// Store copy of key
	char* key_copy = (char*) malloc(key_len + 1);
	if (key_copy == NULL) return;
	memcpy(key_copy, key, key_len);
	key_copy[key_len] = '\0';
	arr->enum_keys[arr->enum_count++] = key_copy;
}

// Remove a key from the array's enumeration order (freeing its copy) and
// compact the list. No-op if the key isn't tracked.
void arrayUntrackKey(ASArray* arr, const char* key, u32 key_len)
{
	if (arr->enum_keys == NULL) return;
	for (u32 i = 0; i < arr->enum_count; i++)
	{
		if (strlen(arr->enum_keys[i]) == key_len &&
		    memcmp(arr->enum_keys[i], key, key_len) == 0)
		{
			free(arr->enum_keys[i]);
			for (u32 j = i + 1; j < arr->enum_count; j++)
				arr->enum_keys[j - 1] = arr->enum_keys[j];
			arr->enum_count--;
			return;
		}
	}
}

// Move a key to the END of the array's enumeration order (Flash re-inserts a
// key when it is (re)assigned; for-in then yields it in reverse-insertion
// order). Removes any existing entry first, then appends.
void arrayReinsertKey(ASArray* arr, const char* key, u32 key_len)
{
	arrayUntrackKey(arr, key, key_len);
	arrayTrackKey(arr, key, key_len);
}

ASArray* allocArray(SWFAppContext* app_context, u32 initial_capacity)
{
	ASArray* arr = (ASArray*) malloc(sizeof(ASArray));
	if (arr == NULL)
	{
		fprintf(stderr, "ERROR: Failed to allocate ASArray\n");
		return NULL;
	}

	arr->refcount = 1;  // Initial reference owned by caller
	arr->length = 0;
	arr->capacity = initial_capacity > 0 ? initial_capacity : 4;

	// Allocate element array
	arr->elements = (ActionVar*) malloc(sizeof(ActionVar) * arr->capacity);
	if (arr->elements == NULL)
	{
		fprintf(stderr, "ERROR: Failed to allocate array elements\n");
		free(arr);
		return NULL;
	}

	// Initialize elements to HOLE (unset sentinel)
	for (u32 i = 0; i < arr->capacity; i++)
	{
		arr->elements[i].type = ACTION_STACK_VALUE_HOLE;
		arr->elements[i].str_size = 0;
		arr->elements[i].data.numeric_value = 0;
	}

	arr->props = NULL;  // Lazily allocated for non-index properties
	arr->enum_keys = NULL;
	arr->enum_count = 0;
	arr->enum_capacity = 0;

	arr->mt_mark = 0;
	arr->mt_kind = MT_KIND_PLAIN;
	arr->mt_pending = 0;
	arr->mt_pending_idx = 0;
	mtLinkArray(arr);

#ifdef DEBUG
	printf("[DEBUG] allocArray: arr=%p, refcount=%u, capacity=%u\n",
		(void*)arr, arr->refcount, arr->capacity);
#endif

	return arr;
}

void retainArray(ASArray* arr)
{
	if (arr == NULL)
	{
		return;
	}
	GC_POISON_ARR(arr, "retainArray");

	arr->refcount++;

#ifdef DEBUG
	printf("[DEBUG] retainArray: arr=%p, refcount=%u -> %u\n",
		(void*)arr, arr->refcount - 1, arr->refcount);
#endif
}

void releaseArray(SWFAppContext* app_context, ASArray* arr)
{
	if (arr == NULL)
	{
		return;
	}
	GC_POISON_ARR(arr, "releaseArray");

#ifdef DEBUG
	printf("[DEBUG] releaseArray: arr=%p, refcount=%u -> %u\n",
		(void*)arr, arr->refcount, arr->refcount - 1);
#endif

	arr->refcount--;

	if (arr->refcount == 0)
	{
		// Same deferral as releaseObject — see the note above it.
		if (!g_in_destroy_drain)
		{
			if (arr->mt_pending) return;
			u32 idx;
			if (pendingDestroyPush(arr, 1, &idx))
			{
				arr->mt_pending = 1;
				arr->mt_pending_idx = idx;
				return;
			}
			// Queue OOM: fall through and destroy inline (pre-fix behavior).
		}
		if (arr->mt_pending)
		{
			g_pending_destroy[arr->mt_pending_idx].p = NULL;
			arr->mt_pending = 0;
		}
#ifdef DEBUG
		printf("[DEBUG] releaseArray: arr=%p reached refcount=0, freeing\n", (void*)arr);
#endif

		// Release all element values
		for (u32 i = 0; i < arr->length; i++)
		{
			// If element is an object, release it recursively
			if (arr->elements[i].type == ACTION_STACK_VALUE_OBJECT)
			{
				ASObject* child_obj = (ASObject*) arr->elements[i].data.numeric_value;
				releaseObject(app_context, child_obj);
			}
			// If element is an array, release it recursively
			else if (arr->elements[i].type == ACTION_STACK_VALUE_ARRAY)
			{
				ASArray* child_arr = (ASArray*) arr->elements[i].data.numeric_value;
				releaseArray(app_context, child_arr);
			}
			// If element is a string that owns memory, free it
			else if (arr->elements[i].type == ACTION_STACK_VALUE_STRING &&
			         arr->elements[i].data.string_data.owns_memory)
			{
				free(arr->elements[i].data.string_data.heap_ptr);
			}
		}

		// Free element array
		if (arr->elements != NULL)
		{
			free(arr->elements);
		}

		// Free non-index properties object
		if (arr->props != NULL)
		{
			releaseObject(app_context, arr->props);
		}

		// Free enumeration key tracking
		if (arr->enum_keys != NULL)
		{
			for (u32 i = 0; i < arr->enum_count; i++)
				free(arr->enum_keys[i]);
			free(arr->enum_keys);
		}

		// Free array itself
		gcNotifyRefcountFree(arr);
		mtUnlinkArray(arr);
		free(arr);
	}
}

ActionVar* getArrayElement(ASArray* arr, u32 index)
{
	if (arr == NULL || index >= arr->length)
	{
		return NULL;
	}
	GC_POISON_ARR(arr, "getArrayElement");
	// Flash: negative signed length (e.g., arr.length = -1) makes all elements inaccessible
	if ((int32_t)arr->length < 0)
	{
		return NULL;
	}

	return &arr->elements[index];
}

void setArrayElement(SWFAppContext* app_context, ASArray* arr, u32 index, ActionVar* value)
{
	GC_POISON_ARR(arr, "setArrayElement");
	if (arr == NULL || value == NULL)
	{
		return;
	}

	// Grow array if needed. The capacity is computed in 64 bits: `(index + 1) * 2`
	// overflows u32 for any index at or past 0x7FFFFFFF, and a wrapped capacity of
	// 0 made realloc shrink the block while the write below still used the huge
	// index — an out-of-bounds write, not a failed allocation. Callers that can
	// see arbitrary indices (the AMF0 reader, script assignment) cap the index
	// themselves and store big ones as named properties; this is the backstop.
	if (index >= arr->capacity)
	{
		unsigned long long want = ((unsigned long long) index + 1) * 2;
		if (want > 0xFFFFFFFFull
		    || want > (unsigned long long) (SIZE_MAX / sizeof(ActionVar)))
		{
			fprintf(stderr, "ERROR: Failed to grow array\n");
			return;
		}
		u32 new_capacity = (u32) want;
		ActionVar* new_elements = (ActionVar*) realloc(arr->elements,
		                                                sizeof(ActionVar) * new_capacity);
		if (new_elements == NULL)
		{
			fprintf(stderr, "ERROR: Failed to grow array\n");
			return;
		}

		arr->elements = new_elements;

		// Initialize new slots to HOLE (unset)
		for (u32 i = arr->capacity; i < new_capacity; i++)
		{
			arr->elements[i].type = ACTION_STACK_VALUE_HOLE;
			arr->elements[i].str_size = 0;
			arr->elements[i].data.numeric_value = 0;
		}

		arr->capacity = new_capacity;
	}

	// Release old value if it exists and is an object/array
	if (index < arr->length)
	{
		if (arr->elements[index].type == ACTION_STACK_VALUE_OBJECT)
		{
			ASObject* old_obj = (ASObject*) arr->elements[index].data.numeric_value;
			releaseObject(app_context, old_obj);
		}
		else if (arr->elements[index].type == ACTION_STACK_VALUE_ARRAY)
		{
			ASArray* old_arr = (ASArray*) arr->elements[index].data.numeric_value;
			releaseArray(app_context, old_arr);
		}
		else if (arr->elements[index].type == ACTION_STACK_VALUE_STRING &&
		         arr->elements[index].data.string_data.owns_memory)
		{
			free(arr->elements[index].data.string_data.heap_ptr);
		}
	}

	// Fill any gap between the current length and the assigned index with
	// HOLEs. These slots may be within capacity but logically absent (e.g.
	// after a pop() shrank length without clearing the slot, or a sparse
	// assignment past the end). Without this, stale values from popped
	// elements leak back in (XML.as:760 — `arr[8]=x` after pop must leave
	// index 7 a hole, not the popped value). Don't free the stale contents:
	// a popped string's heap_ptr may still be referenced by the stack.
	if (index > arr->length)
	{
		for (u32 i = arr->length; i < index; i++)
		{
			arr->elements[i].type = ACTION_STACK_VALUE_HOLE;
			arr->elements[i].str_size = 0;
			arr->elements[i].data.numeric_value = 0;
		}
	}

	// Set new value
	arr->elements[index] = *value;

	// Track insertion order for enumeration
	{
		char idx_buf[12];
		int len = snprintf(idx_buf, sizeof(idx_buf), "%u", index);
		arrayTrackKey(arr, idx_buf, (u32)len);
	}

	// Update length if needed
	if (index >= arr->length)
	{
		arr->length = index + 1;
	}

	// Retain new value if it's an object or array
	if (value->type == ACTION_STACK_VALUE_OBJECT)
	{
		ASObject* new_obj = (ASObject*) value->data.numeric_value;
		retainObject(new_obj);
	}
	else if (value->type == ACTION_STACK_VALUE_ARRAY)
	{
		ASArray* new_arr = (ASArray*) value->data.numeric_value;
		retainArray(new_arr);
	}

#ifdef DEBUG
	printf("[DEBUG] setArrayElement: arr=%p, index=%u, length=%u\n",
		(void*)arr, index, arr->length);
#endif
}

/**
 * ===========================================================================
 * Stage 3 root-traced mark-sweep collector core
 * (SWFRecompDocs/plans/memory-reclamation-plan.md §Stage 3)
 * ===========================================================================
 *
 * Liveness = reachability ONLY. Refcounts are advisory here (stack ops,
 * var_map stores, scope captures, timers are all borrowed edges, and the
 * allocating +1 of script-created values floats forever), so the sweep frees
 * unmarked objects regardless of refcount — that floating +1 is exactly why
 * they leak. Conversely the two-pass teardown releases a doomed object's
 * retained edges into MARKED targets so live refcounts stay balanced.
 *
 * Runs only between frames (caller checks actionGcVmQuiescent) at a tick
 * cadence. Rollout modes (SWF_GC env var): count → quarantine → free.
 * Tracing rules (plan §Stage 3): follow OBJECT/ARRAY ActionVars in
 * properties/elements/arr->props/interfaces; FUNCTION values are traced via
 * their prototype_obj/own_props/captured_scope (functions are immortal);
 * MOVIECLIP values are non-owning and never dereferenced.
 */

// --- configuration + stats ---
#define SWF_GC_MODE_OFF        0
#define SWF_GC_MODE_COUNT      1
#define SWF_GC_MODE_QUARANTINE 2
#define SWF_GC_MODE_FREE       3
static u8  g_gc_mode = SWF_GC_MODE_OFF;
static int g_gc_verbose = 0;
static u32 g_gc_cadence = 60;          // ticks between collections
static u32 g_gc_quarantine_lag = 3;    // collections an object stays parked
static u64 g_gc_tick = 0;
static u32 g_gc_collections = 0;
static u64 g_gc_swept_obj_total = 0;
static u64 g_gc_swept_arr_total = 0;
static int g_gc_config_parsed = 0;

// --- mark worklist (explicit stack — the graph can be deep) ---
typedef struct { void* p; u8 is_array; } GcWorkItem;
static GcWorkItem* g_gc_wl = NULL;
static u32 g_gc_wl_count = 0;
static u32 g_gc_wl_cap = 0;
static int g_gc_mark_failed = 0;   // worklist OOM → abort collection, sweep nothing

// Function-mark epoch: a function is "already traced this collection" when
// gc_epoch == g_gc_fn_epoch. Epochs start at 1 so zero-initialized statics
// (BSS) read as unmarked without any clear pass.
static u32 g_gc_fn_epoch = 0;

static void gcPush(void* p, u8 is_array)
{
	if (g_gc_mark_failed) return;
	if (g_gc_wl_count >= g_gc_wl_cap) {
		u32 ncap = g_gc_wl_cap ? g_gc_wl_cap * 2 : 4096;
		GcWorkItem* nw = (GcWorkItem*) realloc(g_gc_wl, (size_t)ncap * sizeof(GcWorkItem));
		if (nw == NULL) { g_gc_mark_failed = 1; return; }
		g_gc_wl = nw;
		g_gc_wl_cap = ncap;
	}
	g_gc_wl[g_gc_wl_count].p = p;
	g_gc_wl[g_gc_wl_count].is_array = is_array;
	g_gc_wl_count++;
}

// Enrollment filter: the clear pass stamps bit 1 ("member of the live list
// this cycle") on every listed object/array; marking requires it. OBJECT/
// ARRAY-typed edges can dangle at freed memory (the runtime's own
// num_used>16384 corruption guards exist for the same reason), and a garbage
// pointer that merely LOOKS like an object must be neither traced nor swept
// (first observed: N title tick 60, a property whose ASProperty bytes were
// recycled heap garbage — tracing its "getter" segfaulted).
void swfGcMarkObject(ASObject* obj)
{
	if (obj == NULL || (obj->mt_mark & 2) == 0 || (obj->mt_mark & 1)) return;
	obj->mt_mark |= 1;
	gcPush(obj, 0);
}

void swfGcMarkArray(ASArray* arr)
{
	if (arr == NULL || (arr->mt_mark & 2) == 0 || (arr->mt_mark & 1)) return;
	arr->mt_mark |= 1;
	gcPush(arr, 1);
}

void swfGcMarkFunctionPtr(void* fp)
{
	ASFunction* f = (ASFunction*) fp;
	if (f == NULL || f->gc_epoch == g_gc_fn_epoch) return;
	f->gc_epoch = g_gc_fn_epoch;
	swfGcMarkObject(f->prototype_obj);
	swfGcMarkObject(f->own_props);
	u32 n = f->captured_scope_count;
	if (n > 8) n = 8;
	for (u32 i = 0; i < n; i++)
		swfGcMarkObject(f->captured_scope[i]);
}

void swfGcMarkVar(ActionVar* v)
{
	if (v == NULL) return;
	switch (v->type) {
		case ACTION_STACK_VALUE_OBJECT:
			swfGcMarkObject((ASObject*)(uintptr_t)v->data.numeric_value);
			break;
		case ACTION_STACK_VALUE_ARRAY:
			swfGcMarkArray((ASArray*)(uintptr_t)v->data.numeric_value);
			break;
		case ACTION_STACK_VALUE_FUNCTION:
			swfGcMarkFunctionPtr((void*)(uintptr_t)v->data.numeric_value);
			break;
		default:
			break;  // MOVIECLIP is non-owning (may dangle) — never traced
	}
}

// Trace one object's outgoing edges (same corruption guards as getProperty).
static void gcTraceObject(ASObject* o)
{
	if (o->num_used <= 16384 && o->properties != NULL) {
		for (u32 i = 0; i < o->num_used; i++) {
			swfGcMarkVar(&o->properties[i].value);
			if (o->properties[i].getter != NULL)
				swfGcMarkFunctionPtr(o->properties[i].getter);
			if (o->properties[i].setter != NULL)
				swfGcMarkFunctionPtr(o->properties[i].setter);
		}
	}
	if (o->interfaces != NULL) {
		for (u32 i = 0; i < o->interface_count; i++)
			swfGcMarkObject(o->interfaces[i]);
	}
}

// Trace one array's outgoing edges. Bounded by min(length, capacity): length
// can exceed capacity via script `arr.length = N`, and entries past a
// shrunken length may hold already-released values — [0, min) is the live
// window (enum_keys are name strings, not references).
static void gcTraceArray(ASArray* a)
{
	u32 n = a->length;
	if (n > a->capacity) n = a->capacity;
	if (a->elements != NULL) {
		for (u32 i = 0; i < n; i++)
			swfGcMarkVar(&a->elements[i]);
	}
	swfGcMarkObject(a->props);
}

static void gcDrainWorklist(void)
{
	while (g_gc_wl_count > 0 && !g_gc_mark_failed) {
		GcWorkItem it = g_gc_wl[--g_gc_wl_count];
		if (it.is_array) gcTraceArray((ASArray*)it.p);
		else             gcTraceObject((ASObject*)it.p);
	}
}

// --- two-pass teardown -----------------------------------------------------
// Pass 1 (neutralize): release each doomed object's retained edges into
// MARKED (live) targets, balancing the retain the doomed object held. Edges
// into other doomed objects are skipped — those are freed wholesale. If a
// live target's count hits 0 it is NOT freed here (borrowed roots may still
// reach it); it simply becomes collectable by a future sweep.
static void gcReleaseIfLiveObject(ASObject* o)
{
	if (o != NULL && (o->mt_mark & 1) && o->refcount > 0)
		o->refcount--;
}

static void gcReleaseIfLiveArray(ASArray* a)
{
	if (a != NULL && (a->mt_mark & 1) && a->refcount > 0)
		a->refcount--;
}

static void gcNeutralizeObject(ASObject* o)
{
	if (o->num_used <= 16384 && o->properties != NULL) {
		for (u32 i = 0; i < o->num_used; i++) {
			ActionVar* v = &o->properties[i].value;
			if (v->type == ACTION_STACK_VALUE_OBJECT)
				gcReleaseIfLiveObject((ASObject*)(uintptr_t)v->data.numeric_value);
			else if (v->type == ACTION_STACK_VALUE_ARRAY)
				gcReleaseIfLiveArray((ASArray*)(uintptr_t)v->data.numeric_value);
		}
	}
	if (o->interfaces != NULL) {
		for (u32 i = 0; i < o->interface_count; i++)
			gcReleaseIfLiveObject(o->interfaces[i]);
	}
}

static void gcNeutralizeArray(ASArray* a)
{
	u32 n = a->length;
	if (n > a->capacity) n = a->capacity;
	if (a->elements != NULL) {
		for (u32 i = 0; i < n; i++) {
			ActionVar* v = &a->elements[i];
			if (v->type == ACTION_STACK_VALUE_OBJECT)
				gcReleaseIfLiveObject((ASObject*)(uintptr_t)v->data.numeric_value);
			else if (v->type == ACTION_STACK_VALUE_ARRAY)
				gcReleaseIfLiveArray((ASArray*)(uintptr_t)v->data.numeric_value);
		}
	}
	gcReleaseIfLiveObject(a->props);
}

// Pass 2 (free innards): mirrors the release-at-zero teardown in
// releaseObject/releaseArray minus the recursive releases (pass 1 handled
// live edges; doomed edges are freed as their own sweep entries). Leaves the
// struct itself for the caller (free now, or poison + park in quarantine).
static void gcFreeObjectInnards(SWFAppContext* app_context, ASObject* o)
{
	if (o->num_used <= 16384 && o->properties != NULL) {
		for (u32 i = 0; i < o->num_used; i++) {
			if (o->properties[i].name != NULL)
				FREE(o->properties[i].name);
			if (o->properties[i].value.type == ACTION_STACK_VALUE_STRING &&
			    o->properties[i].value.data.string_data.owns_memory)
				free(o->properties[i].value.data.string_data.heap_ptr);
		}
	}
	if (o->properties != NULL) free(o->properties);
	if (o->hash_index != NULL) free(o->hash_index);
	if (o->interfaces != NULL) free(o->interfaces);
	o->properties = NULL;
	o->hash_index = NULL;
	o->interfaces = NULL;
	o->interface_count = 0;
	o->num_used = 0;
	o->num_properties = 0;
}

static void gcFreeArrayInnards(SWFAppContext* app_context, ASArray* a)
{
	(void)app_context;
	u32 n = a->length;
	if (n > a->capacity) n = a->capacity;
	if (a->elements != NULL) {
		for (u32 i = 0; i < n; i++) {
			if (a->elements[i].type == ACTION_STACK_VALUE_STRING &&
			    a->elements[i].data.string_data.owns_memory)
				free(a->elements[i].data.string_data.heap_ptr);
		}
		free(a->elements);
	}
	if (a->enum_keys != NULL) {
		for (u32 i = 0; i < a->enum_count; i++)
			free(a->enum_keys[i]);
		free(a->enum_keys);
	}
	a->elements = NULL;
	a->props = NULL;
	a->enum_keys = NULL;
	a->enum_count = 0;
	a->enum_capacity = 0;
	a->length = 0;
	a->capacity = 0;
}

// --- quarantine ------------------------------------------------------------
typedef struct { void* p; u8 is_array; u32 seq; } GcQuarantined;
static GcQuarantined* g_gc_q = NULL;
static u32 g_gc_q_count = 0;
static u32 g_gc_q_cap = 0;

static void gcQuarantinePark(void* p, u8 is_array)
{
	if (g_gc_q_count >= g_gc_q_cap) {
		u32 ncap = g_gc_q_cap ? g_gc_q_cap * 2 : 1024;
		GcQuarantined* nq = (GcQuarantined*) realloc(g_gc_q, (size_t)ncap * sizeof(GcQuarantined));
		if (nq == NULL) { free(p); return; }  // OOM: skip quarantine, free now
		g_gc_q = nq;
		g_gc_q_cap = ncap;
	}
	g_gc_q[g_gc_q_count].p = p;
	g_gc_q[g_gc_q_count].is_array = is_array;
	g_gc_q[g_gc_q_count].seq = g_gc_collections;
	g_gc_q_count++;
}

// Free entries that have aged out (or everything, when flush_all).
static void gcQuarantineFlush(int flush_all)
{
	u32 kept = 0;
	for (u32 i = 0; i < g_gc_q_count; i++) {
		if (flush_all || g_gc_collections - g_gc_q[i].seq >= g_gc_quarantine_lag)
			free(g_gc_q[i].p);
		else
			g_gc_q[kept++] = g_gc_q[i];
	}
	g_gc_q_count = kept;
}

// --- collection ------------------------------------------------------------
static void gcParseConfig(void)
{
	g_gc_config_parsed = 1;
	const char* mode = getenv("SWF_GC");
	if (mode != NULL && strcmp(mode, "0") == 0) return;  // explicit opt-out
	if (mode == NULL || mode[0] == '\0') {
		// DEFAULT-ON (real free, default cadence) as of 2026-07-04: gated
		// rollout complete — count/quarantine/free all byte-identical on the
		// game soak set, ASAN clean, and full CI green in both modes with
		// the collector forced to cadence 1 (see memory-reclamation results
		// doc §Stage 3). SWF_GC=0 disables.
		g_gc_mode = SWF_GC_MODE_FREE;
	}
	else if (strcmp(mode, "count") == 0)      g_gc_mode = SWF_GC_MODE_COUNT;
	else if (strcmp(mode, "quarantine") == 0) g_gc_mode = SWF_GC_MODE_QUARANTINE;
	else if (strcmp(mode, "1") == 0 || strcmp(mode, "free") == 0 || strcmp(mode, "on") == 0)
		g_gc_mode = SWF_GC_MODE_FREE;
	else {
		fprintf(stderr, "[swf-gc] unknown SWF_GC mode '%s' (want 0|count|quarantine|1) — collector off\n", mode);
		return;
	}
	const char* cad = getenv("SWF_GC_CADENCE");
	if (cad != NULL && atoi(cad) > 0) g_gc_cadence = (u32)atoi(cad);
	const char* lag = getenv("SWF_GC_QUARANTINE_LAG");
	if (lag != NULL && atoi(lag) > 0) g_gc_quarantine_lag = (u32)atoi(lag);
	if (getenv("SWF_GC_VERBOSE") != NULL) g_gc_verbose = 1;
	if (g_gc_mode == SWF_GC_MODE_QUARANTINE) g_swf_gc_poison_active = 1;
	if (g_gc_verbose)
		fprintf(stderr, "[swf-gc] mode=%s cadence=%u\n", mode, g_gc_cadence);
}

// AVM2 mark-roots participant (src/avm2/avm2_main.c registers it at
// runSWF_avm2 startup). A function pointer, not a direct call, so AVM1-only
// builds link object.c without the avm2 sources.
void (*g_avm2_gc_mark_roots)(void) = NULL;

static void gcCollect(SWFAppContext* app_context)
{
	g_gc_collections++;
	gcQuarantineFlush(0);

	// Clear marks + stamp the enrollment bit (bit 1) on the live sets; bump
	// the function epoch (skipping 0, which means "never marked" for
	// zero-initialized statics). Marking requires the enrollment bit, so
	// dangling edges at freed memory are ignored (see swfGcMarkObject).
	for (ASObject* o = g_mt_obj_head; o != NULL; o = o->mt_next) o->mt_mark = 2;
	for (ASArray* a = g_mt_arr_head; a != NULL; a = a->mt_next) a->mt_mark = 2;
	g_gc_fn_epoch++;
	if (g_gc_fn_epoch == 0) g_gc_fn_epoch = 1;
	g_gc_mark_failed = 0;
	g_gc_wl_count = 0;

	// Root set (see actionGcMarkRoots for the inventory), then trace.
	actionGcMarkRoots();
	variablesGcMarkRoots();
	timerGcMarkRoots();
	registeredClassGcMarkRoots();
	mathGcMarkRoots();
	dateGcMarkRoots();
	if (g_avm2_gc_mark_roots != NULL) g_avm2_gc_mark_roots();
	gcDrainWorklist();
	if (g_gc_mark_failed) {
		fprintf(stderr, "[swf-gc] mark worklist OOM — collection #%u aborted (nothing swept)\n",
			g_gc_collections);
		return;
	}

	// Doomed census (also the entire "count" mode).
	u32 doom_obj = 0, doom_dprops = 0, doom_arr = 0;
	for (ASObject* o = g_mt_obj_head; o != NULL; o = o->mt_next) {
		if (o->mt_mark & 1) continue;
		if (o->mt_kind == MT_KIND_DPROPS) doom_dprops++;
		doom_obj++;
	}
	for (ASArray* a = g_mt_arr_head; a != NULL; a = a->mt_next) {
		if (!(a->mt_mark & 1)) doom_arr++;
	}
	if (g_gc_verbose) {
		fprintf(stderr,
			"[swf-gc] #%u tick=%llu live=%u obj/%u arr — %s %u obj (%u dprops) + %u arr\n",
			g_gc_collections, (unsigned long long)g_gc_tick,
			swfMemLiveObjects(), swfMemLiveArrays(),
			g_gc_mode == SWF_GC_MODE_COUNT ? "would sweep" : "sweeping",
			doom_obj, doom_dprops, doom_arr);
	}
	if (g_gc_mode == SWF_GC_MODE_COUNT) return;
	if (doom_obj == 0 && doom_arr == 0) return;

	// Pass 1: neutralize every doomed object's retained edges into live
	// targets (must complete for the WHOLE doomed set before anything is
	// freed — releases must never touch freed memory).
	for (ASObject* o = g_mt_obj_head; o != NULL; o = o->mt_next)
		if (!(o->mt_mark & 1)) gcNeutralizeObject(o);
	for (ASArray* a = g_mt_arr_head; a != NULL; a = a->mt_next)
		if (!(a->mt_mark & 1)) gcNeutralizeArray(a);

	// Pass 2: free innards + free/park the structs.
	int park = (g_gc_mode == SWF_GC_MODE_QUARANTINE);
	for (ASObject* o = g_mt_obj_head; o != NULL; ) {
		ASObject* next = o->mt_next;
		if (!(o->mt_mark & 1)) {
			gcFreeObjectInnards(app_context, o);
			mtUnlinkObject(o);
			if (park) {
				o->mt_kind = MT_KIND_GC_POISONED;
				o->refcount = 0xDDDDDDDDu;
				o->mt_prev = NULL;
				o->mt_next = NULL;
				gcQuarantinePark(o, 0);
			} else {
				free(o);
			}
			g_gc_swept_obj_total++;
		}
		o = next;
	}
	for (ASArray* a = g_mt_arr_head; a != NULL; ) {
		ASArray* next = a->mt_next;
		if (!(a->mt_mark & 1)) {
			gcFreeArrayInnards(app_context, a);
			mtUnlinkArray(a);
			if (park) {
				a->mt_kind = MT_KIND_GC_POISONED;
				a->refcount = 0xDDDDDDDDu;
				a->mt_prev = NULL;
				a->mt_next = NULL;
				gcQuarantinePark(a, 1);
			} else {
				free(a);
			}
			g_gc_swept_arr_total++;
		}
		a = next;
	}
}

void swfGcMaybeCollect(SWFAppContext* app_context)
{
	// Deliberately FIRST, ahead of every early return below. This runs at the
	// tick boundary in all three frame loops, which makes it the one place that
	// is guaranteed to see each tick — and the drain must happen every tick,
	// not on the collector's cadence and not only when the collector is on.
	//
	// It must also precede the sweep. A parked struct is unreachable from the
	// roots, so the sweep would free it and leave the queue holding a dangling
	// pointer; worse, skipping parked structs in the sweep is not a fix either,
	// since their doomed outbound edges would be freed around them and the
	// later drain would release freed children. Draining first keeps the queue
	// empty whenever the sweep runs, so the two reclaimers never overlap.
	swfDrainPendingDestroys(app_context);

	if (!g_gc_config_parsed) gcParseConfig();
	if (g_gc_mode == SWF_GC_MODE_OFF) return;
	g_gc_tick++;
	if (g_gc_tick % g_gc_cadence != 0) return;
	if (!actionGcRootsEnumerable()) {
		fprintf(stderr, "[swf-gc] child_mc_cache overflowed — uncacheable MovieClips"
			" would be invisible roots; collector disabled for this run\n");
		g_gc_mode = SWF_GC_MODE_OFF;
		return;
	}
	if (!actionGcVmQuiescent(app_context)) {
		if (g_gc_verbose)
			fprintf(stderr, "[swf-gc] tick=%llu VM not quiescent — skipping\n",
				(unsigned long long)g_gc_tick);
		return;
	}
	gcCollect(app_context);
}

// GC stats line for swfMemReport (only when the collector ever ran).
static void gcReportLine(void)
{
	if (g_gc_collections == 0) return;
	fprintf(stderr,
		"[swf-mem] gc:      collections=%u swept=%llu obj + %llu arr (mode=%u cadence=%u quarantined=%u)\n",
		g_gc_collections,
		(unsigned long long)g_gc_swept_obj_total,
		(unsigned long long)g_gc_swept_arr_total,
		(unsigned)g_gc_mode, g_gc_cadence, g_gc_q_count);
}

// Scrub-on-free: when refcounting frees an object/array, clear any borrowed
// C stash still pointing at it (global registers, timer entries, watch table,
// LC/NetStream/Sound side tables). Those stashes are GC roots, and a stale
// entry would make the mark phase read freed memory. Pre-GC these stale
// entries were latent UAFs at their eventual use sites; scrubbing also fixes
// the pointer-reuse false-match hazard in the identity-keyed side tables.
// Gated on collector mode so default builds keep the exact old behavior.
static void gcNotifyRefcountFree(void* p)
{
	// Parse env config on the first free too — frees during init would
	// otherwise predate the first tick's parse and skip scrubbing.
	if (!g_gc_config_parsed) gcParseConfig();
	if (g_gc_mode == SWF_GC_MODE_OFF) return;
	actionGcScrubStashes(p);
	timerGcScrubStashes(p);
}
