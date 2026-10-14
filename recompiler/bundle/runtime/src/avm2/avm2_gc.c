// AVM2 mark-sweep garbage collector (Stage 11). See avm2_gc.h for the design
// rationale (separate census from AVM1; collect only between ticks because
// method-body operand stacks/locals live on the C stack; conservative ext
// scan as the missed-edge safety net).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <memory/heap.h>

#include <avm2/avm2_abc.h>
#include <avm2/avm2_class.h>
#include <avm2/avm2_e4x.h>
#include <avm2/avm2_flixel.h>
#include <avm2/avm2_gc.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_value.h>

// --- census -----------------------------------------------------------------

static Avm2Object* g_gc_head = NULL;   // intrusive census (all Avm2Objects)
static uint32_t g_gc_live_objects = 0; // enrolled minus swept (net live count)
static uint64_t g_gc_total_alloc_bytes = 0;   // monotonic: every avm2_alloc byte
static uint64_t g_gc_bytes_since_collect = 0; // reset each collect (watermark)

// String census: every heap Avm2String (avm2_string_new / avm2_string_concat)
// enrolls here and is swept when unreachable. Static pool strings never
// enroll (gc_flags == 0) and are never written to (rodata).
static Avm2String* g_gc_str_head = NULL;
static uint32_t g_gc_live_strings = 0;
static uint64_t g_gc_str_swept_total = 0;

// --- config -----------------------------------------------------------------

static int g_gc_configured = 0;
static int g_gc_enabled = 1;              // AVM2_GC=0 disables
static int g_gc_strings_enabled = 1;      // AVM2_GC_STRINGS=0 disables the
                                          // string sweep only (A/B + kill switch)
static uint64_t g_gc_watermark = 4u * 1024 * 1024; // bytes allocated between collects
// Adaptive watermark (measured on RWK gameplay, 2026-07-18): the watermark
// counts GROSS bytes allocated since the last collect, and Flixel's per-frame
// churn (FlxQuadTree/FlxList rebuilt from scratch every tick — ~9k objects,
// ~3.3 MB gross per tick) crossed the fixed 4 MB default every ~2 ticks,
// putting a ~55 ms stop-the-world collect on nearly every other frame (~50%
// GC duty cycle; "unplayably slow with a rhythmic pause" in browser wasm).
// After each completed collect the watermark is retargeted to the LIVE heap
// size — collect frequency then tracks retention, not churn:
//   watermark = clamp(live_allocated_bytes, base, min(256 MB, headroom/4))
// The headroom/4 clamp keeps garbage accumulation from OOMing a
// near-capacity arena (rwic's live set boots at ~1.7 GB of a 1770 MB heap);
// the 256 MB ceiling bounds the worst-case sweep pause. Deterministic: the
// post-collect o1heap 'allocated' value is a pure function of the (already
// deterministic) allocation sequence. AVM2_GC_WATERMARK (absolute override)
// and AVM2_GC_STRESS (watermark 0) both disable adaptation.
static uint64_t g_gc_watermark_base = 4u * 1024 * 1024;
static int g_gc_adaptive = 1;
// Companion trigger: a collect's snap+sweep cost is proportional to CENSUS
// ENTRIES (objects + strings), not bytes — a big-live-set game whose byte
// watermark allows long intervals could otherwise accumulate millions of
// tiny dead FlxList-class objects and pay a multi-hundred-ms pause. Collect
// when either trigger fires. 200k new entries ≈ ~100 ms of snap+sweep on
// the 2026 reference box (AVM2_GC_MAX_ENROLL overrides; 0 disables).
static uint64_t g_gc_enroll_cap = 200000;
static uint64_t g_gc_enrolled_since_collect = 0;
static uint64_t g_gc_sweep_budget = 25000;  // census entries per sweep slice
                                            // (≈2 ms on the 2026 reference box)
static int g_gc_verbose = 0;
static int g_gc_time = 0;                 // AVM2_GC_TIME=1: per-collect phase timings
static uint32_t g_gc_collections = 0;
static uint64_t g_gc_swept_total = 0;

// --- retention diagnosis (AVM2_GC_WHY=<class-name substring>) ---------------
// Records, during the ordinary mark, the FIRST marker of every object (its
// parent object + the edge kind), then after the drain prints the path to a
// root for each live instance whose class qname contains the target. Off (and
// free) unless the env var is set; never changes what is marked.
static const char* g_why_target = NULL;
static int g_hist_on = 0;
static Avm2Object* g_why_parent = NULL;
static const char* g_why_edge = NULL;
static uint32_t g_why_idx = 0;
typedef struct WhyEntry { Avm2Object* obj; Avm2Object* parent; const char* edge; uint32_t idx; } WhyEntry;
static WhyEntry* g_why_tab = NULL;
static uint32_t g_why_cap = 0;   // power of two
static uint32_t g_why_used = 0;
#define WHY_SET(e, i) do { g_why_edge = (e); g_why_idx = (i); } while (0)
static uint32_t why_hash(const void* p) { uintptr_t x = (uintptr_t) p; x ^= x >> 16; x *= 0x45d9f3bu; x ^= x >> 16; return (uint32_t) x; }
static void why_record(Avm2Object* o)
{
	if (g_why_used * 2 >= g_why_cap) return;  // table full this cycle: lose precision, never block
	uint32_t m = g_why_cap - 1, h = why_hash(o) & m;
	while (g_why_tab[h].obj != NULL) h = (h + 1) & m;
	g_why_tab[h].obj = o; g_why_tab[h].parent = g_why_parent; g_why_tab[h].edge = g_why_edge; g_why_tab[h].idx = g_why_idx;
	g_why_used++;
}
static const WhyEntry* why_find(const Avm2Object* o)
{
	uint32_t m = g_why_cap - 1, h = why_hash(o) & m;
	while (g_why_tab[h].obj != NULL) { if (g_why_tab[h].obj == o) return &g_why_tab[h]; h = (h + 1) & m; }
	return NULL;
}

static void gc_configure(void)
{
	g_gc_configured = 1;
	const char* e = getenv("AVM2_GC");
	if (e != NULL && strcmp(e, "0") == 0) g_gc_enabled = 0;
	const char* es = getenv("AVM2_GC_STRINGS");
	if (es != NULL && strcmp(es, "0") == 0) g_gc_strings_enabled = 0;
	const char* w = getenv("AVM2_GC_WATERMARK");
	if (w != NULL)
	{
		long long v = atoll(w);
		if (v > 0)
		{
			g_gc_watermark = (uint64_t) v;
			g_gc_watermark_base = (uint64_t) v;
			g_gc_adaptive = 0;  // explicit absolute request wins
		}
	}
	const char* ad = getenv("AVM2_GC_ADAPTIVE");
	if (ad != NULL && strcmp(ad, "0") == 0) g_gc_adaptive = 0;
	const char* ec = getenv("AVM2_GC_MAX_ENROLL");
	if (ec != NULL && ec[0] != '\0') g_gc_enroll_cap = (uint64_t) atoll(ec);
	// Stress mode (CI correctness gate, the AVM1 swf_gc=1/cadence-1 precedent):
	// collect between EVERY tick regardless of allocation volume, so short
	// trace tests actually exercise mark-sweep. A collection that frees a live
	// object surfaces as a wrong trace — the honest-failure tripwire. Treat
	// empty / "0" as off (CI sets the var to "" when the input is blank).
	const char* stress = getenv("AVM2_GC_STRESS");
	if (stress != NULL && stress[0] != '\0' && strcmp(stress, "0") != 0)
	{
		g_gc_watermark = 0;
		g_gc_adaptive = 0;  // stress means collect EVERY tick — never retarget
		g_gc_sweep_budget = 0;  // ...and sweep completely, so every tick is a
		                        // full mark-sweep (the honest-failure tripwire)
	}
	const char* sb = getenv("AVM2_GC_SWEEP_BUDGET");
	if (sb != NULL && sb[0] != '\0') g_gc_sweep_budget = (uint64_t) atoll(sb);
	if (getenv("AVM2_GC_VERBOSE") != NULL) g_gc_verbose = 1;
	if (getenv("AVM2_GC_TIME") != NULL) g_gc_time = 1;
	g_why_target = getenv("AVM2_GC_WHY");
	g_hist_on = getenv("AVM2_GC_HIST") != NULL;
	if (g_why_target != NULL && g_why_target[0] == '\0') g_why_target = NULL;
}

// Per-collect phase timing (AVM2_GC_TIME=1). One clock read per phase per
// collect — the collector's own cost model (the tier-2 lever ledger in
// SWFRecompDocs/prompts/avm2-gc-collector-cost.md) is measured with this, so
// it lives here rather than being re-patched in each session.
static double gc_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

// --- enrollment + accounting ------------------------------------------------

// --- arena membership bitmap (tier-2 lever 1) --------------------------------
//
// The conservative ext scan needs one question answered per scanned word: does
// a census object START at this address? That used to be a binary search over
// a sorted snapshot of every census address, REBUILT (walk + qsort) at the top
// of every collect — measured at HALF the collect pause on RWK gameplay
// (36 ms of a 73 ms pause over a 284k-entry census).
//
// The bitmap answers it in O(1) with no per-collect work: 1 bit per
// allocation-alignment cell of the heap arena, SET on enroll and CLEARED on
// sweep, so it is always exactly the census. Every heap_alloc result is
// alignment-aligned, so distinct live objects never share a cell; a cell whose
// object was freed and whose memory now backs a different object reads as
// "yes" for that new object — over-retention at worst, never a missed edge.
//
// Sizing: span/ALIGN/8 = 16 MB for the 4 GB native arena, 15.5 MB for the
// 1984 MB wasm one. calloc'd, so native pages materialize only where objects
// actually live (o1heap allocates from the arena front).
//
// When the heap reports no arena (HEAP_PASSTHROUGH — sanitizer builds route to
// system malloc), g_bm stays NULL and the collector falls back to the sorted
// snapshot below.
static uint8_t* g_bm = NULL;
static const char* g_bm_base = NULL;
static size_t g_bm_span = 0;
static size_t g_bm_cell = 0;
static int g_bm_tried = 0;

static void bm_init(Avm2Context* ctx)
{
	g_bm_tried = 1;
	if (ctx == NULL || ctx->app == NULL) return;
	const char* base = (const char*) heap_arena_base(ctx->app);
	size_t span = heap_arena_span(ctx->app);
	size_t cell = heap_arena_alignment();
	if (base == NULL || span == 0 || cell == 0) return;
	size_t bytes = (span / cell + 7) / 8 + 1;
	uint8_t* bm = (uint8_t*) calloc(bytes, 1);
	if (bm == NULL) return;  // fall back to the snapshot path
	g_bm = bm;
	g_bm_base = base;
	g_bm_span = span;
	g_bm_cell = cell;
}

static inline void bm_set(const void* p)
{
	size_t i = ((const char*) p - g_bm_base) / g_bm_cell;
	g_bm[i >> 3] |= (uint8_t) (1u << (i & 7));
}

static inline void bm_clear(const void* p)
{
	size_t i = ((const char*) p - g_bm_base) / g_bm_cell;
	g_bm[i >> 3] &= (uint8_t) ~(1u << (i & 7));
}

static inline int bm_test(const void* p)
{
	const char* c = (const char*) p;
	if (c < g_bm_base || c >= g_bm_base + g_bm_span) return 0;
	size_t off = (size_t) (c - g_bm_base);
	if ((off & (g_bm_cell - 1)) != 0) return 0;  // not an allocation start
	size_t i = off / g_bm_cell;
	return (g_bm[i >> 3] >> (i & 7)) & 1;
}

// Sweep state (tier-2 lever 3, defined below): objects enrolled while a sweep
// is in flight go to a nursery instead of the census list the sweep cursor is
// walking, and are born BLACK.
static int g_sweeping;
static uint32_t g_sweep_epoch;
static Avm2Object* g_nursery_head;
static Avm2Object* g_nursery_tail;

void avm2_gc_enroll(Avm2Context* ctx, Avm2Object* obj)
{
	if (!g_bm_tried) bm_init(ctx);
	if (g_sweeping)
	{
		// Allocate black, off to the side: the cursor must not meet objects
		// that did not exist when the mark ran, and prepending to the census
		// under the cursor's `link` would splice them out on the next unlink.
		// The nursery joins the census when the sweep finishes.
		obj->gc_next = g_nursery_head;
		obj->gc_mark = g_sweep_epoch << 1;
		if (g_nursery_head == NULL) g_nursery_tail = obj;
		g_nursery_head = obj;
	}
	else
	{
		obj->gc_next = g_gc_head;
		obj->gc_mark = 0;
		g_gc_head = obj;
	}
	g_gc_live_objects++;
	g_gc_enrolled_since_collect++;
	// An object outside the arena can only happen under HEAP_PASSTHROUGH (no
	// bitmap at all); with an arena, avm2_alloc always lands inside it.
	if (g_bm != NULL) bm_set(obj);
}

void avm2_gc_note_alloc(uint32_t bytes)
{
	g_gc_total_alloc_bytes += bytes;
	g_gc_bytes_since_collect += bytes;
}

// --- pins + mark epoch (tier-2 lever 2) --------------------------------------
//
// Pinned objects (class objects, prototypes, XML nodes) are structurally
// immortal. They used to be found by walking the whole census at the top of
// each collect — the same walk that reset every object to white. With epoch
// marks there is no reset walk left, so pins live in their own append-only
// array and seed the worklist directly. Pins are never removed (nothing
// unpins, and a pinned object is never swept because it is always marked).
static Avm2Object** g_pinned = NULL;
static uint32_t g_pinned_count = 0, g_pinned_cap = 0;
static uint32_t g_gc_epoch = 1;  // 0 = "never marked" (enroll state)

_Static_assert(sizeof(((Avm2Object*) 0)->gc_mark) == 4, "gc_mark is the epoch word");

void avm2_gc_pin(Avm2Object* obj)
{
	if (obj == NULL) return;
	if (obj->gc_mark & 1) return;  // already pinned (and already in the array)
	if (g_pinned_count == g_pinned_cap)
	{
		uint32_t nc = g_pinned_cap == 0 ? 1024 : g_pinned_cap * 2;
		Avm2Object** grown = realloc(g_pinned, nc * sizeof(Avm2Object*));
		// Can't record the pin: leave the object UNPINNED rather than silently
		// half-pinning it. It is still reachable from wherever it is stored
		// (class tables are marked by the root markers), so this is a
		// precision loss under OOM, not a UAF.
		if (grown == NULL) return;
		g_pinned = grown;
		g_pinned_cap = nc;
	}
	obj->gc_mark |= 1;
	g_pinned[g_pinned_count++] = obj;
}

int avm2_gc_is_marked(const Avm2Object* obj)
{
	return obj != NULL && (obj->gc_mark >> 1) == g_gc_epoch;
}

void avm2_gc_enroll_string(Avm2String* s)
{
	s->gc_next = g_gc_str_head;
	s->gc_flags = AVM2_STR_GC_HEAP;
	g_gc_str_head = s;
	g_gc_live_strings++;
	g_gc_enrolled_since_collect++;
}


uint32_t avm2_gc_live_objects(void) { return g_gc_live_objects; }
uint32_t avm2_gc_live_strings(void) { return g_gc_live_strings; }
uint64_t avm2_gc_live_bytes(void) { return g_gc_total_alloc_bytes; }

// --- worklist ---------------------------------------------------------------

static Avm2Object** g_wl = NULL;
static uint32_t g_wl_count = 0, g_wl_cap = 0;
static int g_gc_mark_failed = 0;  // worklist grow OOM this cycle → abort sweep

static void wl_push(Avm2Object* o)
{
	if (g_wl_count == g_wl_cap)
	{
		uint32_t nc = g_wl_cap == 0 ? 4096 : g_wl_cap * 2;
		Avm2Object** grown = realloc(g_wl, nc * sizeof(Avm2Object*));
		if (grown == NULL)
		{
			// Can't grow the worklist: dropping queued objects would leave
			// their children untraced → swept-while-live (UAF). Flag the
			// failure; gc_collect aborts the sweep entirely (over-retain,
			// never free). Drop THIS push (o stays marked; a marked object
			// that never gets traced is fine when nothing is swept).
			g_gc_mark_failed = 1;
			return;
		}
		g_wl = grown;
		g_wl_cap = nc;
	}
	g_wl[g_wl_count++] = o;
}

void avm2_gc_mark_object(Avm2Object* obj)
{
	if (obj == NULL) return;
	if ((obj->gc_mark >> 1) == g_gc_epoch) return;  // already marked this cycle
	obj->gc_mark = (g_gc_epoch << 1) | (obj->gc_mark & 1);
	if (g_why_tab != NULL) why_record(obj);
	wl_push(obj);
}

void avm2_gc_mark_value(Avm2Value v)
{
	if (v.kind == AVM2_VALUE_OBJECT) avm2_gc_mark_object(v.u.obj);
	else if (v.kind == AVM2_VALUE_STRING) avm2_gc_mark_string(v.u.str);
}

void avm2_gc_mark_scope(const Avm2ScopeChain* scope)
{
	if (scope == NULL) return;
	for (uint32_t i = 0; i < scope->count; i++)
	{
		avm2_gc_mark_object(scope->entries[i].obj);
	}
}

// --- conservative census-membership test ------------------------------------
//
// A sorted snapshot of every census-object address, rebuilt each cycle during
// the clear pass. The conservative ext scan binary-searches it: any ext word
// equal to a census-object start is treated as a live edge. Over-retention
// (a non-pointer word that happens to equal an address) is safe; the only
// forbidden outcome — freeing a still-referenced object — cannot occur because
// every raw object pointer in a live ext is caught here.

static Avm2Object** g_census_sorted = NULL;
static uint32_t g_census_sorted_count = 0, g_census_sorted_cap = 0;

// Sorted snapshot of the STRING census, rebuilt each cycle. Unlike objects,
// string lookups are RANGE-based: a census string's allocation spans
// [s, s + sizeof(Avm2String) + s->len + 1) (header + inline bytes), and live
// references may point at the header (Avm2Value.u.str), at the bytes (a
// by-value Avm2String copy's utf8, e.g. Avm2DynProp.name), or anywhere inside
// (conservative ext-scan words). Any pointer into the span marks the string.
static Avm2String** g_str_census_sorted = NULL;
static uint32_t g_str_census_count = 0, g_str_census_cap = 0;
// O(1) address-bounds gate for the range lookup, set once per cycle after the
// snapshot qsort. Most conservatively-scanned ext words are numbers/flags;
// rejecting them before the binary search keeps the scan near its old cost.
static const char* g_str_census_lo = NULL;
static const char* g_str_census_hi = NULL;

static int str_census_ptr_cmp(const void* a, const void* b)
{
	Avm2String* pa = *(Avm2String* const*) a;
	Avm2String* pb = *(Avm2String* const*) b;
	if (pa < pb) return -1;
	if (pa > pb) return 1;
	return 0;
}

void avm2_gc_mark_string_bytes(const void* p)
{
	if (p == NULL || g_str_census_count == 0) return;
	if ((const char*) p < g_str_census_lo || (const char*) p >= g_str_census_hi) return;
	// Greatest census string whose start <= p.
	uint32_t lo = 0, hi = g_str_census_count;
	while (lo < hi)
	{
		uint32_t mid = lo + (hi - lo) / 2;
		if ((const void*) g_str_census_sorted[mid] <= p) lo = mid + 1;
		else hi = mid;
	}
	if (lo == 0) return;
	Avm2String* s = g_str_census_sorted[lo - 1];
	if ((const char*) p < (const char*) s + sizeof(Avm2String) + s->len + 1)
	{
		s->gc_flags |= AVM2_STR_GC_MARK;
	}
}

void avm2_gc_mark_string(const Avm2String* s)
{
	if (s == NULL) return;
	if ((s->gc_flags & AVM2_STR_GC_HEAP) && s->utf8 == (const char*) (s + 1))
	{
		// The census header itself (every string minted by avm2_string_new /
		// avm2_string_concat has its bytes inline) — O(1).
		((Avm2String*) s)->gc_flags |= AVM2_STR_GC_MARK;
		return;
	}
	// A by-value copy (its utf8 points into some census string's bytes) or a
	// static pool string / literal (lookup misses; no-op).
	avm2_gc_mark_string_bytes(s->utf8);
}

static int census_ptr_cmp(const void* a, const void* b)
{
	Avm2Object* pa = *(Avm2Object* const*) a;
	Avm2Object* pb = *(Avm2Object* const*) b;
	if (pa < pb) return -1;
	if (pa > pb) return 1;
	return 0;
}

static int census_contains(Avm2Object* p)
{
	if (g_bm != NULL) return bm_test(p);
	// Fallback (no arena — HEAP_PASSTHROUGH): binary search the per-cycle
	// sorted snapshot.
	uint32_t lo = 0, hi = g_census_sorted_count;
	while (lo < hi)
	{
		uint32_t mid = lo + (hi - lo) / 2;
		if (g_census_sorted[mid] < p) lo = mid + 1;
		else hi = mid;
	}
	return lo < g_census_sorted_count && g_census_sorted[lo] == p;
}

static void conservative_scan(Avm2Object* o)
{
	if (o->native_ext == NULL || o->native_ext_size == 0) return;
	// native_ext blobs are O1HEAP_ALIGNMENT-aligned; pointer fields are
	// word-aligned. Scan word by word for embedded census-object starts.
	char* base = (char*) o->native_ext;
	uint32_t nwords = o->native_ext_size / sizeof(void*);
	for (uint32_t i = 0; i < nwords; i++)
	{
		Avm2Object* cand;
		memcpy(&cand, base + i * sizeof(void*), sizeof(void*));
		if (cand == NULL) continue;
		if (census_contains(cand)) { avm2_gc_mark_object(cand); continue; }
		// Not an object: it may be a census STRING edge — either an
		// Avm2String* header (Avm2EventExt.type, DisplayObjectExt.name,
		// RegExpExt.source, QName/Namespace ext, ...) or a bare utf8 byte
		// pointer. The range lookup covers both; over-retention only.
		avm2_gc_mark_string_bytes(cand);
	}
}

// --- precise edge tracing ---------------------------------------------------

static void trace_array(Avm2Object* o)
{
	Avm2ArrayExt* ext = (Avm2ArrayExt*) o->native_ext;
	if (ext == NULL) return;
	for (uint32_t i = 0; i < ext->dense_len; i++) { WHY_SET("elem", i); avm2_gc_mark_value(ext->elems[i]); }
	WHY_SET("sparse", 0);
	for (Avm2SparseElem* s = ext->sparse; s != NULL; s = s->next) avm2_gc_mark_value(s->v);
}

static void trace_vector(Avm2Object* o)
{
	Avm2VectorExt* ext = (Avm2VectorExt*) o->native_ext;
	if (ext == NULL) return;
	for (uint32_t i = 0; i < ext->length; i++) { WHY_SET("velem", i); avm2_gc_mark_value(ext->elems[i]); }
}

static void trace_object(Avm2Object* o)
{
	g_why_parent = o;
	WHY_SET("proto", 0);
	avm2_gc_mark_object(o->proto);
	for (uint32_t i = 0; i < o->slot_count; i++) { WHY_SET("slot", i); avm2_gc_mark_value(o->slots[i]); }
	uint32_t why_dyn = 0;
	for (Avm2DynProp* p = o->dyn_props; p != NULL; p = p->next)
	{
		WHY_SET("dyn", why_dyn++);
		avm2_gc_mark_value(p->value);
		avm2_gc_mark_object(p->key_obj);
		// The prop name is a by-value COPY of a heap string (set_dynamic:
		// p->name = *avm2_string_new(...)); its utf8 points into that census
		// string's inline bytes, which must survive as long as the prop does.
		if (p->name.utf8 != NULL) avm2_gc_mark_string_bytes(p->name.utf8);
	}
	WHY_SET("bound_method", 0);
	for (Avm2BoundMethod* bm = o->bound_methods; bm != NULL; bm = bm->next)
	{
		avm2_gc_mark_object(bm->fn);
	}
	// Function-closure payload.
	WHY_SET("fn_receiver", 0);
	avm2_gc_mark_value(o->fn_receiver);
	WHY_SET("fn_prototype", 0);
	avm2_gc_mark_object(o->fn_prototype);
	WHY_SET("fn_scope", 0);
	avm2_gc_mark_scope(o->fn_scope);
	WHY_SET("class_payload", 0);
	if (o->fn_bound_class != NULL) avm2_gc_mark_object(o->fn_bound_class->class_object);
	// Class object payload: keep the class's proto + class object alive, and
	// trace the class's captured scope chains. A class scope can hold arbitrary
	// objects (avmplus captures `new C()` instances as scope bases — see
	// supercall_two_classobjects), and the object is reachable ONLY through the
	// class object's scope. Because every class object is pinned (hence traced
	// each cycle), tracing scopes here covers ALL class objects — including
	// ones overwritten in file->classes when the ABC reuses a class index.
	if (o->class_ref != NULL)
	{
		Avm2Class* c = o->class_ref;
		avm2_gc_mark_object(c->prototype_obj);
		avm2_gc_mark_object(c->class_object);
		avm2_gc_mark_scope(c->scope);
		avm2_gc_mark_scope(c->iscope);
		for (uint32_t i = 0; i < c->ivtable.count; i++)
			avm2_gc_mark_scope(c->ivtable.entries[i].method_scope);
		for (uint32_t i = 0; i < c->ivtable.meta_cap; i++)
			if (c->ivtable.metas != NULL && c->ivtable.metas[i].used)
				avm2_gc_mark_scope(c->ivtable.metas[i].method_scope);
	}
	// native_ext: precise for the two element-storage kinds; for every other
	// ext blob, run the two module tracers that follow indirect edges the
	// conservative scan cannot (dispatcher listeners, display child lists),
	// then conservatively scan the blob for any remaining direct object
	// pointers (event target, transform target, sharedobject data, ...).
	if (o->kind == AVM2_OBJ_ARRAY) { trace_array(o); return; }
	if (o->kind == AVM2_OBJ_VECTOR) { trace_vector(o); return; }
	if (o->native_ext != NULL)
	{
		WHY_SET("ext:listeners", 0);
		avm2_events_gc_trace_ext(o);
		WHY_SET("ext:display", 0);
		avm2_display_gc_trace_ext(o);
		WHY_SET("ext:text", 0);
		avm2_text_gc_trace_ext(o);
		WHY_SET("ext:xml", 0);
		avm2_xml_gc_trace_ext(o);
		// Flixel quadtree intrinsic: the FlxList object edges hang off arena
		// chunks the conservative blob scan cannot follow into.
		WHY_SET("ext:flixel", 0);
		avm2_flixel_gc_trace_ext(o);
		WHY_SET("ext:conservative", 0);
		conservative_scan(o);
	}
}

// --- sweep ------------------------------------------------------------------

// Free a live object's tombstoned dyn-prop nodes. Tombstones exist only for
// in-flight enumeration cursors (avm2_object.c next_enumerant), and
// collections run strictly between ticks when no for-in can be mid-flight
// (hasnext2 loops live inside method bodies, which complete within a tick) —
// so at sweep time the only reference to a dead node is the object's cached
// cursor, which is reset here (the cursor cache re-derives its position from
// the public index on the next enumeration call; dead nodes are never
// counted, so the enumerable sequence is unchanged). Without this purge, a
// delete→re-add cycle on a long-lived object grows its chain unboundedly
// (the set path skips dead nodes and appends a fresh node + name string).
static void purge_dead_dyn_props(Avm2Context* ctx, Avm2Object* o)
{
	Avm2DynProp** link = &o->dyn_props;
	Avm2DynProp* tail = NULL;
	int purged = 0;
	for (Avm2DynProp* p = o->dyn_props; p != NULL; )
	{
		Avm2DynProp* next = p->next;
		if (p->dead)
		{
			*link = next;
			heap_free(ctx->app, p);
			purged = 1;
		}
		else
		{
			tail = p;
			link = &p->next;
		}
		p = next;
	}
	if (purged)
	{
		o->dyn_tail = tail;
		o->dyn_enum_pos = NULL;
		o->dyn_enum_public = 0;
	}
}

static void free_innards(Avm2Context* ctx, Avm2Object* o)
{
	SWFAppContext* app = ctx->app;
	if (o->slots != NULL) heap_free(app, o->slots);
	for (Avm2DynProp* p = o->dyn_props; p != NULL; )
	{
		Avm2DynProp* next = p->next;
		heap_free(app, p);
		p = next;
	}
	// The bound-method cache nodes (obj.method === obj.method) are owned by
	// the receiver; the closures they point at are census objects of their
	// own. Leaked one node per cached method per dead receiver (FlashPunk's
	// Mask caches three per instance — Seedling made ~1k a room swap).
	for (Avm2BoundMethod* bm = o->bound_methods; bm != NULL; )
	{
		Avm2BoundMethod* next = bm->next;
		heap_free(app, bm);
		bm = next;
	}
	if (o->kind == AVM2_OBJ_ARRAY && o->native_ext != NULL)
	{
		Avm2ArrayExt* ext = (Avm2ArrayExt*) o->native_ext;
		if (ext->elems != NULL) heap_free(app, ext->elems);
		for (Avm2SparseElem* s = ext->sparse; s != NULL; )
		{
			Avm2SparseElem* n = s->next;
			heap_free(app, s);
			s = n;
		}
		heap_free(app, ext);
	}
	else if (o->kind == AVM2_OBJ_VECTOR && o->native_ext != NULL)
	{
		Avm2VectorExt* ext = (Avm2VectorExt*) o->native_ext;
		if (ext->elems != NULL) heap_free(app, ext->elems);
		heap_free(app, ext);
	}
	else if (o->native_ext != NULL)
	{
		// Generic ext blob. First let each module free the further heap
		// allocations its ext owns (pixel buffer, byte buffer, child arrays,
		// listener nodes, style entries) — freeing the blob alone would leak
		// them (a native-heap leak invisible to the object census). Each hook
		// no-ops unless the object is its type. Then free the blob.
		avm2_bitmap_gc_free_ext(ctx, o);
		avm2_bytearray_gc_free_ext(ctx, o);
		avm2_display_gc_free_ext(ctx, o);
		avm2_events_gc_free_ext(ctx, o);
		avm2_text_gc_free_ext(ctx, o);
		avm2_xml_gc_free_ext(ctx, o);
		avm2_flixel_gc_free_ext(ctx, o);
		avm2_stage3d_gc_free_ext(ctx, o);
		heap_free(app, o->native_ext);
	}
	// Per-object vtables: only newactivation/newcatch allocate a vtable owned
	// by a single object, and they are exactly the `no_index` ones (every
	// other object shares its class's embedded ivtable — never no_index, and
	// never freed here). Entry keys point into ABC static data; method_scope
	// chains are shared (not owned); name_index is never built for no_index
	// vtables.
	if (o->vtable != NULL && o->vtable->no_index)
	{
		if (o->vtable->entries != NULL) heap_free(app, o->vtable->entries);
		if (o->vtable->metas != NULL) heap_free(app, o->vtable->metas);
		heap_free(app, (void*) o->vtable);
	}
	// Synthetic catch-scope class (avm2_op_newcatch): owned exclusively by
	// this object; nothing else can reach it (never registered in any
	// file/domain table, has no class_object/prototype).
	if (o->cls != NULL && (o->cls->flags & AVM2_CLASS_FLAG_SYNTH_CATCH))
	{
		heap_free(app, o->cls);
	}
}

// --- incremental sweep (tier-2 lever 3) --------------------------------------
//
// The mark phase must be atomic (it runs between ticks, off the C stack), but
// the sweep only touches objects the completed mark already proved dead —
// nothing can reach them, so freeing them can be spread over the ticks that
// follow. That converts the whole `census × per-object free` term (the largest
// remaining slice of the pause: ~24 ms of a ~40 ms RWK collect) from one
// stop-the-world hit into a bounded slice per tick.
//
// The invariants that make it safe:
//  - objects allocated during the sweep never meet the cursor: they enroll in
//    a nursery (avm2_gc_enroll) and are born black, and the nursery is spliced
//    into the census only once the cursor reaches the end;
//  - no marking happens while a sweep is in flight — avm2_gc_maybe_collect
//    finishes the sweep before it will start another cycle, so mark state is
//    stable for the cursor's whole traversal;
//  - a dead object cannot be resurrected mid-sweep: if the mutator could reach
//    it, the mark would have marked it.
//
// Budget is per slice, in census entries (AVM2_GC_SWEEP_BUDGET; 0 = sweep
// everything in one go, the pre-lever behaviour and the A/B kill switch).
// Stress mode forces 0 so the CI gate keeps collecting completely each tick.
static Avm2Object* g_sweep_cur;
static Avm2Object** g_sweep_link;

static uint32_t gc_sweep_slice(Avm2Context* ctx, uint64_t budget)
{
	uint32_t swept = 0;
	uint64_t visited = 0;
	while (g_sweep_cur != NULL && (budget == 0 || visited < budget))
	{
		Avm2Object* o = g_sweep_cur;
		Avm2Object* next = o->gc_next;
		visited++;
		if ((o->gc_mark >> 1) == g_sweep_epoch)
		{
			purge_dead_dyn_props(ctx, o);
			g_sweep_link = &o->gc_next;
		}
		else
		{
			// Unlink + free. The membership bit must drop with the object: the
			// cell can be handed to a different allocation immediately.
			*g_sweep_link = next;
			if (g_bm != NULL) bm_clear(o);
			free_innards(ctx, o);
			heap_free(ctx->app, o);
			g_gc_live_objects--;
			swept++;
		}
		g_sweep_cur = next;
	}
	g_gc_swept_total += swept;
	if (g_sweep_cur == NULL)
	{
		// Done: splice the nursery (objects born during this sweep) back onto
		// the front of the census and reopen marking.
		if (g_nursery_head != NULL)
		{
			g_nursery_tail->gc_next = g_gc_head;
			g_gc_head = g_nursery_head;
			g_nursery_head = g_nursery_tail = NULL;
		}
		g_sweeping = 0;
		g_sweep_link = NULL;
	}
	return swept;
}

// --- collection -------------------------------------------------------------

static const char* why_name(const Avm2Object* o, char* buf, int size)
{
	if (o->kind == AVM2_OBJ_FUNCTION)
	{
		snprintf(buf, size, "fn:%s", o->fn_method.debug_name ? o->fn_method.debug_name : "?");
		return buf;
	}
	if (o->kind == AVM2_OBJ_CLASS && o->class_ref != NULL)
	{
		int n = snprintf(buf, size, "class:");
		avm2_class_qname_buf(o->class_ref, buf + n, size - n);
		return buf;
	}
	if (o->cls == NULL) { snprintf(buf, size, "<nocls k%u>", o->kind); return buf; }
	avm2_class_qname_buf(o->cls, buf, size);
	return buf;
}

// The slot's trait name: the vtable entry whose SLOT index is i.
static const char* why_slot_name(const Avm2Object* parent, uint32_t i, char* buf, int size)
{
	buf[0] = '\0';
	const Avm2VTable* vt = parent->vtable;
	if (vt == NULL) return buf;
	for (uint32_t e = 0; e < vt->count; e++)
	{
		const Avm2PropEntry* pe = &vt->entries[e];
		if (pe->kind == AVM2_PROP_SLOT && pe->slot_index == i && pe->key.name != NULL)
		{
			snprintf(buf, size, "%.*s", (int) pe->key.name_len, pe->key.name);
			return buf;
		}
	}
	return buf;
}

static const char* why_dyn_name(const Avm2Object* parent, uint32_t i, char* buf, int size)
{
	buf[0] = '\0';
	uint32_t k = 0;
	for (Avm2DynProp* p = parent->dyn_props; p != NULL; p = p->next, k++)
		if (k == i) { snprintf(buf, size, "%.*s", (int) p->name.len, p->name.utf8 ? p->name.utf8 : ""); break; }
	return buf;
}

static void why_report(void)
{
	char nb[256], eb[160], pb[256];
	uint32_t total = 0, shown = 0;
	for (Avm2Object* o = g_gc_head; o != NULL; o = o->gc_next)
	{
		if ((o->gc_mark >> 1) != g_gc_epoch) continue;
		why_name(o, nb, sizeof nb);
		if (strstr(nb, g_why_target) == NULL) continue;
		// Exact-name match after the last '.' or ':' so "Game" does not also match "GameText".
		const char* leaf = nb;
		for (const char* c = nb; *c; c++) if (*c == '.' || *c == ':') leaf = c + 1;
		if (strcmp(leaf, g_why_target) != 0) continue;
		total++;
		if (shown >= 8) continue;
		shown++;
		fprintf(stderr, "[avm2-gc-why] #%u %s@%p\n", g_gc_collections, nb, (void*) o);
		const Avm2Object* cur = o;
		for (int depth = 0; depth < 64 && cur != NULL; depth++)
		{
			const WhyEntry* w = why_find(cur);
			if (w == NULL) { fprintf(stderr, "[avm2-gc-why]   (no record)\n"); break; }
			eb[0] = '\0';
			if (w->parent != NULL && w->edge != NULL && strcmp(w->edge, "slot") == 0) why_slot_name(w->parent, w->idx, eb, sizeof eb);
			else if (w->parent != NULL && w->edge != NULL && strcmp(w->edge, "dyn") == 0) why_dyn_name(w->parent, w->idx, eb, sizeof eb);
			if (w->parent == NULL)
			{
				fprintf(stderr, "[avm2-gc-why]   <- %s\n", w->edge ? w->edge : "?");
				break;
			}
			why_name(w->parent, pb, sizeof pb);
			fprintf(stderr, "[avm2-gc-why]   <- %s[%u%s%s] of %s@%p\n", w->edge ? w->edge : "?", w->idx,
			        eb[0] ? " " : "", eb, pb, (void*) w->parent);
			cur = w->parent;
		}
	}
	fprintf(stderr, "[avm2-gc-why] #%u live %s instances: %u (recorded %u marks)\n", g_gc_collections, g_why_target, total, g_why_used);
}

// AVM2_GC_HIST=1: after the mark, the live (marked) census by class, top 40.
typedef struct HistEnt { const void* key; uint8_t kind; uint32_t n; const Avm2Object* sample; } HistEnt;
static int hist_cmp(const void* a, const void* b)
{
	uint32_t x = ((const HistEnt*) a)->n, y = ((const HistEnt*) b)->n;
	return x < y ? 1 : x > y ? -1 : 0;
}
static void hist_report(void)
{
	enum { CAP = 1 << 14 };
	HistEnt* t = calloc(CAP, sizeof(HistEnt));
	if (t == NULL) return;
	uint32_t total = 0;
	for (Avm2Object* o = g_gc_head; o != NULL; o = o->gc_next)
	{
		if ((o->gc_mark >> 1) != g_gc_epoch) continue;
		total++;
		const void* key = o->kind == AVM2_OBJ_FUNCTION ? (const void*) o->fn_method.debug_name
		                : o->kind == AVM2_OBJ_CLASS ? (const void*) o->class_ref : (const void*) o->cls;
		uint32_t h = (uint32_t) (((uintptr_t) key * 2654435761u) ^ o->kind) & (CAP - 1);
		while (t[h].sample != NULL && !(t[h].key == key && t[h].kind == o->kind)) h = (h + 1) & (CAP - 1);
		if (t[h].sample == NULL) { t[h].key = key; t[h].kind = o->kind; t[h].sample = o; }
		t[h].n++;
	}
	qsort(t, CAP, sizeof(HistEnt), hist_cmp);
	char nb[256];
	fprintf(stderr, "[avm2-gc-hist] #%u live=%u\n", g_gc_collections, total);
	for (int i = 0; i < 40 && t[i].n > 0; i++)
		fprintf(stderr, "[avm2-gc-hist] #%u %7u %s\n", g_gc_collections, t[i].n, why_name(t[i].sample, nb, sizeof nb));
	free(t);
}

static void gc_collect(Avm2Context* ctx)
{
	g_gc_collections++;
	double t_start = 0, t_snap = 0, t_strsnap = 0, t_trace = 0, t_sweep = 0, t_strsweep = 0;
	if (g_gc_time) t_start = gc_now_ms();
	const uint32_t census_n = g_gc_live_objects;  // census size (pre-sweep)
	// NOTE: the watermark (g_gc_bytes_since_collect) is reset only after both
	// census snapshots are built — a snapshot realloc OOM returns early, and
	// resetting first would silently degrade the collector to a permanent
	// no-op under exactly the memory pressure it exists to relieve (the next
	// avm2_gc_maybe_collect must retry immediately).

	// Clear pass: O(1) — bumping the epoch makes every object white at once
	// (nothing carries the new epoch yet). Pinned objects are seeded from the
	// pin array. The only remaining reason to walk the census here is the
	// fallback membership snapshot, built only when there is no arena bitmap.
	g_gc_epoch++;
	if (g_gc_epoch > 0x7FFFFFFFu) g_gc_epoch = 1;  // (2^31 collects; wrap only over-retains)
	g_census_sorted_count = 0;
	g_wl_count = 0;
	g_gc_mark_failed = 0;
	if (g_bm == NULL)
	{
		for (Avm2Object* o = g_gc_head; o != NULL; o = o->gc_next)
		{
			if (g_census_sorted_count == g_census_sorted_cap)
			{
				uint32_t nc = g_census_sorted_cap == 0 ? 8192 : g_census_sorted_cap * 2;
				Avm2Object** grown = realloc(g_census_sorted, nc * sizeof(Avm2Object*));
				if (grown == NULL) { fprintf(stderr, "[avm2-gc] census snapshot OOM — skip collect\n"); return; }
				g_census_sorted = grown;
				g_census_sorted_cap = nc;
			}
			g_census_sorted[g_census_sorted_count++] = o;
		}
		qsort(g_census_sorted, g_census_sorted_count, sizeof(Avm2Object*), census_ptr_cmp);
	}
	if (g_why_target != NULL)
	{
		uint32_t want = 1024;
		while (want < (g_gc_live_objects + 1024) * 2) want <<= 1;
		if (want != g_why_cap) { free(g_why_tab); g_why_tab = malloc(want * sizeof(WhyEntry)); g_why_cap = g_why_tab ? want : 0; }
		if (g_why_tab) memset(g_why_tab, 0, g_why_cap * sizeof(WhyEntry));
		g_why_used = 0;
	}
	// E4X nodes: open this cycle's node epoch before anything can mark one.
	avm2_e4x_gc_begin();
	g_why_parent = NULL;
	WHY_SET("root:pinned", 0);
	for (uint32_t i = 0; i < g_pinned_count; i++) avm2_gc_mark_object(g_pinned[i]);
	if (g_gc_time) t_snap = gc_now_ms();

	// String clear pass: reset white + build the sorted range-lookup snapshot.
	// Strings have no outgoing edges, so nothing is seeded to the worklist.
	g_str_census_count = 0;
	for (Avm2String* s = g_gc_str_head; s != NULL; s = s->gc_next)
	{
		if (g_str_census_count == g_str_census_cap)
		{
			uint32_t nc = g_str_census_cap == 0 ? 8192 : g_str_census_cap * 2;
			Avm2String** grown = realloc(g_str_census_sorted, nc * sizeof(Avm2String*));
			if (grown == NULL) { fprintf(stderr, "[avm2-gc] string snapshot OOM — skip collect\n"); g_str_census_count = 0; return; }
			g_str_census_sorted = grown;
			g_str_census_cap = nc;
		}
		g_str_census_sorted[g_str_census_count++] = s;
		s->gc_flags &= ~AVM2_STR_GC_MARK;
	}
	qsort(g_str_census_sorted, g_str_census_count, sizeof(Avm2String*), str_census_ptr_cmp);
	// O(1) reject bounds for the range lookup (most conservatively-scanned
	// ext words are numbers/flags, not string pointers).
	if (g_str_census_count > 0)
	{
		Avm2String* last = g_str_census_sorted[g_str_census_count - 1];
		g_str_census_lo = (const char*) g_str_census_sorted[0];
		g_str_census_hi = (const char*) last + sizeof(Avm2String) + last->len + 1;
	}

	if (g_gc_time) t_strsnap = gc_now_ms();

	// Watermark reset: both snapshots built, the cycle will complete.
	g_gc_bytes_since_collect = 0;
	g_gc_enrolled_since_collect = 0;

	// Root markers (per module).
	g_why_parent = NULL; WHY_SET("root:main", 0);
	avm2_gc_mark_roots_main(ctx);
	g_why_parent = NULL; WHY_SET("root:globals", 0);
	avm2_gc_mark_roots_globals(ctx);
	g_why_parent = NULL; WHY_SET("root:display", 0);
	avm2_gc_mark_roots_display(ctx);
	g_why_parent = NULL; WHY_SET("root:events", 0);
	avm2_gc_mark_roots_events(ctx);
	g_why_parent = NULL; WHY_SET("root:amf", 0);
	avm2_gc_mark_roots_amf(ctx);
	g_why_parent = NULL; WHY_SET("root:media", 0);
	avm2_gc_mark_roots_media(ctx);
	g_why_parent = NULL; WHY_SET("root:net", 0);
	avm2_gc_mark_roots_net(ctx);
	g_why_parent = NULL; WHY_SET("root:external", 0);
	avm2_gc_mark_roots_external(ctx);
	// Flixel quadtree intrinsic: the AS3 `protected static _o / _oc` live as C
	// globals and hold object refs across the tick boundary, as they did in AS3.
	g_why_parent = NULL; WHY_SET("root:flixel", 0);
	avm2_gc_mark_roots_flixel(ctx);

	// Drain: trace every marked object's edges (may mark more).
	while (g_wl_count > 0)
	{
		Avm2Object* o = g_wl[--g_wl_count];
		trace_object(o);
	}

	if (g_gc_time) t_trace = gc_now_ms();
	if (g_why_tab != NULL) why_report();
	if (g_hist_on) hist_report();
	g_why_parent = NULL;

	// If the worklist couldn't grow at any point this cycle, the mark is
	// incomplete — abort the sweep (over-retain, never free a live object).
	if (g_gc_mark_failed)
	{
		if (g_gc_verbose)
			fprintf(stderr, "[avm2-gc] #%u worklist OOM — sweep aborted\n", g_gc_collections);
		return;
	}

	// Weak registries: the orphan list must drop entries for objects the sweep
	// below will free (it intentionally holds weak references — see
	// avm2_display_gc_prune_dead_orphans). Runs only on a completed mark.
	avm2_display_gc_prune_dead_orphans();

	// E4X nodes no wrapper reached die now (their wrappers die in the object
	// sweep below and never read them). Only on a completed mark.
	uint32_t e4x_freed = avm2_e4x_gc_sweep_nodes(ctx);

	// Sweep: free every unmarked (white) census object. Marking is atomic
	// between ticks, but the sweep is resumable — see gc_sweep_slice.
	g_sweeping = 1;
	g_sweep_epoch = g_gc_epoch;
	g_sweep_cur = g_gc_head;
	g_sweep_link = &g_gc_head;
	// Deferring frees costs headroom: the cycle's garbage stays allocated for
	// the ~10 ticks the slices take. A game whose live set nearly fills the
	// arena (rwic boots at ~1.7 GB of a 1770 MB heap) cannot afford that — an
	// allocation failure is fatal, a long pause is not. Below 1/8 headroom,
	// sweep the whole census right here.
	uint64_t budget = g_gc_sweep_budget;
	{
		uint64_t cap = heap_capacity_bytes(ctx->app);
		uint64_t used = heap_allocated_bytes(ctx->app);
		if (cap > 0 && used > cap - cap / 8) budget = 0;
	}
	uint32_t swept = gc_sweep_slice(ctx, budget);
	if (g_gc_time) t_sweep = gc_now_ms();

	// String sweep: free every unmarked census string (header + inline bytes
	// are one allocation). AVM2_GC_STRINGS=0 skips it (exact pre-string-GC
	// behavior for A/B leak measurement, and a kill switch).
	uint32_t str_swept = 0;
	if (g_gc_strings_enabled)
	{
		Avm2String** slink = &g_gc_str_head;
		for (Avm2String* s = g_gc_str_head; s != NULL; )
		{
			Avm2String* snext = s->gc_next;
			if (s->gc_flags & AVM2_STR_GC_MARK)
			{
				slink = &s->gc_next;
				s = snext;
				continue;
			}
			*slink = snext;
			heap_free(ctx->app, s);
			g_gc_live_strings--;
			str_swept++;
			s = snext;
		}
		g_gc_str_swept_total += str_swept;
	}

	if (g_gc_time) t_strsweep = gc_now_ms();
	const uint32_t str_census_n = g_str_census_count;

	// Drop the per-cycle snapshot: it now holds dangling pointers to swept
	// strings, and an out-of-cycle mark call must be a safe no-op, never a
	// read of freed memory.
	g_str_census_count = 0;
	g_str_census_lo = g_str_census_hi = NULL;

	// Retarget the watermark to the post-collect live heap (see the field
	// comment). Runs only on a completed cycle — aborted cycles (snapshot
	// OOM, worklist OOM) keep the old watermark so the next maybe_collect
	// retries immediately.
	if (g_gc_adaptive)
	{
		uint64_t live = heap_allocated_bytes(ctx->app);
		if (live > 0)
		{
			uint64_t w = live;
			uint64_t cap = heap_capacity_bytes(ctx->app);
			if (cap > live && w > (cap - live) / 4) w = (cap - live) / 4;
			if (w > 256u * 1024 * 1024) w = 256u * 1024 * 1024;
			if (w < g_gc_watermark_base) w = g_gc_watermark_base;
			g_gc_watermark = w;
		}
	}

	if (g_gc_time)
	{
		fprintf(stderr, "[avm2-gc-time] #%u total=%.2f snap=%.2f strsnap=%.2f trace=%.2f sweep=%.2f strsweep=%.2f census=%u str=%u swept=%u\n",
		        g_gc_collections, t_strsweep - t_start, t_snap - t_start,
		        t_strsnap - t_snap, t_trace - t_strsnap, t_sweep - t_trace,
		        t_strsweep - t_sweep, census_n, str_census_n, swept);
	}

	if (g_gc_verbose)
	{
		fprintf(stderr, "[avm2-gc] #%u live=%u swept=%u strings live=%u swept=%u (total alloc %.1f MB, heap %.2f MB)\n",
		        g_gc_collections, g_gc_live_objects, swept,
		        g_gc_live_strings, str_swept,
		        (double) g_gc_total_alloc_bytes / (1024.0 * 1024.0),
		        (double) heap_allocated_bytes(ctx->app) / (1024.0 * 1024.0));
		if (e4x_freed > 0) fprintf(stderr, "[avm2-gc-e4x] #%u nodes swept=%u\n", g_gc_collections, e4x_freed);
	}
}

void avm2_gc_collect_now(Avm2Context* ctx)
{
	if (!g_gc_configured) gc_configure();
	if (!g_gc_enabled) return;
	if (g_sweeping) gc_sweep_slice(ctx, 0);  // finish the in-flight sweep first
	gc_collect(ctx);
	if (g_sweeping) gc_sweep_slice(ctx, 0);  // "now" means fully collected
}

void avm2_gc_maybe_collect(Avm2Context* ctx)
{
	if (!g_gc_configured) gc_configure();
	if (!g_gc_enabled) return;
	if (g_sweeping)
	{
		// A cycle is already in flight — retire another slice of it. Marking
		// cannot start until the cursor finishes.
		gc_sweep_slice(ctx, g_gc_sweep_budget);
		return;
	}
	if (g_gc_bytes_since_collect < g_gc_watermark
	    && (g_gc_enroll_cap == 0 || g_gc_enrolled_since_collect < g_gc_enroll_cap))
		return;
	gc_collect(ctx);
}

// --- soak harness (AVM2_GC_SOAK=<ticks>) ------------------------------------
//
// Drives `ticks` synthetic frames on the real, fully-constructed context, each
// allocating a batch of UNREACHABLE arrays (per-frame garbage, the dominant
// leak class for a blitting game). With the collector on, live-object N goes
// flat; with AVM2_GC=0 it climbs linearly — the before/after picture the
// Stage-11 soak must show. Logs to stderr; does not touch stdout (trace
// output stays byte-identical). Runs only when AVM2_GC_SOAK is set.

void avm2_gc_soak(Avm2Context* ctx, uint64_t ticks)
{
	if (!g_gc_configured) gc_configure();
	const uint32_t per_tick = 200;
	fprintf(stderr, "[avm2-gc-soak] start ticks=%llu per_tick=%u gc=%s watermark=%lluB\n",
	        (unsigned long long) ticks, per_tick, g_gc_enabled ? "on" : "off",
	        (unsigned long long) g_gc_watermark);
	uint32_t base_live = g_gc_live_objects;
	for (uint64_t t = 0; t < ticks; t++)
	{
		for (uint32_t i = 0; i < per_tick; i++)
		{
			Avm2Object* a = avm2_array_new(ctx, 0);
			avm2_array_push(ctx, a, avm2_integer((int32_t) i));
			avm2_array_push(ctx, a, avm2_object_value(avm2_array_new(ctx, 0)));
			(void) a;  // never stored → unreachable garbage
		}
		avm2_gc_maybe_collect(ctx);
		if (t % 500 == 0 || t == ticks - 1)
		{
			fprintf(stderr, "[avm2-gc-soak] tick=%-6llu live=%-8u (delta %+d) total_alloc=%.1fMB collections=%u\n",
			        (unsigned long long) t, g_gc_live_objects,
			        (int) g_gc_live_objects - (int) base_live,
			        (double) g_gc_total_alloc_bytes / (1024.0 * 1024.0),
			        g_gc_collections);
		}
	}
	fprintf(stderr, "[avm2-gc-soak] END live=%u swept_total=%llu strings_live=%u strings_swept_total=%llu collections=%u\n",
	        g_gc_live_objects, (unsigned long long) g_gc_swept_total,
	        g_gc_live_strings, (unsigned long long) g_gc_str_swept_total,
	        g_gc_collections);
}
