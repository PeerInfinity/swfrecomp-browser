// Native C intrinsic for Flixel 2.21 org.flixel::FlxQuadTree +
// org.flixel.data::FlxList. See include/avm2/avm2_flixel.h for the rationale
// and the install contract.
//
// ============================================================================
// AS3 -> C MAP (source: jpexs output org/flixel/FlxQuadTree.as, 418 lines)
// ============================================================================
//   FlxQuadTree.as:8-20    protected static _ot/_or/_o/_oa/_oc/_ob/_ol
//                                                  -> g_ot/g_or/g_o/g_oa/g_oc/g_ob/g_ol
//   FlxQuadTree.as:22-26   MIN / A_LIST / B_LIST    -> FLXQT_MIN / FLXQT_A_LIST / FLXQT_B_LIST
//   FlxQuadTree.as:28-60   protected instance vars  -> struct FlxQTNode
//   FlxList.as:8-17        next / object            -> struct FlxQTList (+ list_new)
//   FlxQuadTree.as:62-115  FlxQuadTree()            -> qt_node_new()
//                          (root only)              -> qt_native_ctor()   [instance_init]
//   FlxQuadTree.as:117-198 addObject()              -> qt_add_object()
//   FlxQuadTree.as:200-243 addToList()              -> qt_add_to_list()
//   FlxQuadTree.as:245-326 overlap()                -> qt_overlap()
//                          (public entry)           -> qt_native_overlap() [vtable]
//   FlxQuadTree.as:328-371 add()                    -> qt_add()
//                          (public entry)           -> qt_native_add()     [vtable]
//   FlxQuadTree.as:373-415 overlapNode()            -> qt_overlap_node()
//   FlxU.as:19             roundingError = 1e-7     -> FLXU_ROUNDING_ERROR
//                          (internal static, assigned exactly once, never
//                           mutated — verified — so it is hoisted to a const)
//
// There are NO deviations from the source semantics. Everything below is a
// literal transliteration; where the C shape differs (recursion over C structs
// instead of AS3 virtual calls) the traversal order and short-circuiting are
// preserved exactly. The load-bearing quirks are called out inline as
// "QUIRK n" comments.
//
// Only the ROOT tree is an Avm2Object. Child FlxQuadTrees and every FlxList
// node are pure C structs bump-allocated out of the root's arena. This is safe
// because _nw/_ne/_se/_sw and _headA/_tailA/_headB/_tailB are `protected`, the
// class is never subclassed, and neither child trees nor FlxList instances ever
// escape the class (the only external callers are FlxU.overlap:48-61 and
// FlxU.collide:624-640, which touch nothing but add() and overlap()).
//
// FlxObject stays fully AS3: x/y/width/height/exists/solid/_group are read
// through vtable slot indices resolved LAZILY on first use (FlxObject's class
// may be defined after FlxQuadTree) and cached per Avm2Class*.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <avm2/avm2_abc.h>
#include <avm2/avm2_class.h>
#include <avm2/avm2_flixel.h>
#include <avm2/avm2_gc.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_value.h>
#include <memory/heap.h>
#include <stdio.h>
#include <stdlib.h>

// FlxQuadTree.as:22-26 — public static const (Flixel 2.21).
//
// 2.35 replaced this fixed constant with a `protected static var _min:uint`
// that the ROOT node computes as `(width + height) / (2 * FlxU.quadTreeDivisions)`
// and every child reuses (it is static). `quadTreeDivisions` defaults to 3 and
// neither RWF nor RWIC overrides it (both call FlxU.setWorldBounds without the
// 5th argument, and FlxU resets it to 3), so for the two whitelisted 2.35
// titles the divisor is a fixed 6. See the 2.35 variant in qt_node_new and the
// per-title fingerprints in SWFRecomp/src/abc/abc_emit.cpp.
#define FLXQT_MIN 48.0
#define FLXQT_DIVISIONS_235 3u   // FlxU.quadTreeDivisions default; verified for RWF/RWIC
#define FLXQT_A_LIST 0u
#define FLXQT_B_LIST 1u

// FlxU.as:19 — `internal static var roundingError:Number = 1e-7`, assigned
// exactly once at class init and never mutated anywhere in the title.
#define FLXU_ROUNDING_ERROR 1e-7

// ============================================================================
// Data structures
// ============================================================================

// org.flixel.data::FlxList (FlxList.as:5-18). The ctor nulls both fields;
// arena allocations are memset to 0, which is the same thing.
typedef struct FlxQTList
{
	struct FlxQTList* next;
	Avm2Object* object;
} FlxQTList;

// org.flixel::FlxQuadTree instance state (FlxQuadTree.as:28-60), plus the
// x/y/width/height it inherits from FlxRect. All AS3 `Number`, so IEEE double
// throughout — never float (trap 8).
typedef struct FlxQTNode
{
	double x, y, width, height;  // FlxRect base (set by super() at :66)
	double _l, _r, _t, _b, _hw, _hh, _mx, _my;
	int _canSubdivide;
	FlxQTList *_headA, *_tailA, *_headB, *_tailB;
	struct FlxQTNode *_nw, *_ne, *_se, *_sw;
} FlxQTNode;

// --- arena ------------------------------------------------------------------
// Plain bump allocator over avm2_alloc'd chunks, freed wholesale in the GC
// free-ext hook. Deliberately NOT a recycling/pooling scheme.
//
// A small inline bootstrap buffer lives inside the ext itself so the common
// tiny-tree case (root node + its two list nodes = ~190 bytes) allocates zero
// chunks. This matters: FlxU.collide() mints a fresh root per call, hundreds of
// times per frame, and those roots are only reclaimed at the next collection —
// a fixed 64 KB first chunk each would be tens of MB of float per tick.
#define FLXQT_INLINE_BYTES 1024u
#define FLXQT_CHUNK_MIN 4096u
#define FLXQT_CHUNK_MAX 65536u

typedef struct FlxQTChunk
{
	struct FlxQTChunk* next;
	uint32_t used;
	uint32_t cap;
	// payload follows immediately
} FlxQTChunk;

typedef struct FlxQTArena
{
	FlxQTChunk* chunks;  // most recent first
	uint32_t next_cap;
	uint32_t inline_used;
	unsigned char inline_buf[FLXQT_INLINE_BYTES];
} FlxQTArena;

typedef struct FlxQTExt
{
	FlxQTNode* root;
	FlxQTArena arena;
} FlxQTExt;

static void* arena_alloc(Avm2Context* ctx, FlxQTArena* a, uint32_t size)
{
	size = (size + 7u) & ~7u;
	if (a->inline_used + size <= FLXQT_INLINE_BYTES)
	{
		void* p = a->inline_buf + a->inline_used;
		a->inline_used += size;
		return p;
	}
	FlxQTChunk* c = a->chunks;
	if (c == NULL || c->used + size > c->cap)
	{
		uint32_t cap = a->next_cap;
		if (cap < FLXQT_CHUNK_MIN) cap = FLXQT_CHUNK_MIN;
		if (cap < size) cap = size;
		c = (FlxQTChunk*) avm2_alloc(ctx, (uint32_t) sizeof(FlxQTChunk) + cap);
		c->next = a->chunks;
		c->used = 0;
		c->cap = cap;
		a->chunks = c;
		a->next_cap = (cap < FLXQT_CHUNK_MAX) ? (cap * 2u) : FLXQT_CHUNK_MAX;
	}
	void* p = (unsigned char*) c + sizeof(FlxQTChunk) + c->used;
	c->used += size;
	return p;
}

static void arena_free(Avm2Context* ctx, FlxQTArena* a)
{
	FlxQTChunk* c = a->chunks;
	while (c != NULL)
	{
		FlxQTChunk* n = c->next;
		heap_free(ctx->app, c);
		c = n;
	}
	a->chunks = NULL;
	a->inline_used = 0;
}

// ============================================================================
// Class statics (FlxQuadTree.as:8-20)
// ============================================================================
// These are `protected static` and touched only by this class, so hoisting
// them to C file-scope globals preserves AS3 semantics exactly — INCLUDING the
// re-entrancy characteristics that QUIRK 1 depends on.
static double g_ol, g_or, g_ot, g_ob;
static Avm2Object* g_o;      // _o:FlxObject
static Avm2Value g_oc;       // _oc:Function (default null)
static uint32_t g_oa;        // _oa:uint

// ============================================================================
// Install state
// ============================================================================
static int g_no_intrinsics = -1;   // AVM2_NO_INTRINSICS, read once
static int g_flxlist_verified = 0; // intrinsic_id 2 seen
static Avm2Class* g_qt_pending;    // FlxQuadTree seen before FlxList
static int g_qt_pending_is_235;    // variant of the parked FlxQuadTree
static Avm2Class* g_qt_cls;        // installed FlxQuadTree class
static Avm2Class* g_tilemap_cls;   // class whose static arrayToCSV was substituted (id 4)

// Diagnostic call counters. Zero cost by default; AVM2_FLIXEL_STATS=1 prints
// them at exit. The intrinsic installs LAZILY (first FlxU.collide), so a
// boot-only check sees nothing — these counts are how the intrinsic-vs-fallback
// frame oracle proves the native path was actually exercised (non-vacuous).
static unsigned long g_stat_ctor, g_stat_add, g_stat_addobj,
                     g_stat_overlap, g_stat_overlapnode, g_stat_arraytocsv;

// Subdivision threshold. 2.21 (id 1) uses the fixed MIN=48 at every node; 2.35
// (id 3) has the ROOT compute a shared `_min` from its size, which every child
// reuses. `g_qt_is_235` selects the variant; `g_qt_min` holds the live value,
// set once per root in qt_node_new and read by all nodes of that tree.
static int g_qt_is_235 = 0;
static double g_qt_min = FLXQT_MIN;

// FlxRect slots on the root object (resolved at install time; 0 = fall back to
// the public-property path).
static uint32_t g_qt_s_x, g_qt_s_y, g_qt_s_w, g_qt_s_h;

// ============================================================================
// FlxObject field access
// ============================================================================
// Slot ids are 1-based (index 0 unused), so 0 doubles as "unresolved — use
// avm2_get_public_property". Resolution is lazy and cached per Avm2Class*; a
// different class pointer in the bucket triggers a re-resolve.

typedef struct FlxObjSlots
{
	Avm2Class* cls;
	uint32_t s_x, s_y, s_w, s_h, s_exists, s_solid, s_group;
} FlxObjSlots;

#define FLXOBJ_CACHE_N 16u
static FlxObjSlots g_slot_cache[FLXOBJ_CACHE_N];

// Public slot lookup; returns the slot id or 0 if absent / not a plain slot
// (a getter/setter falls back to the generic property path).
static uint32_t slot_of_public(const Avm2VTable* vt, const char* name, uint32_t len)
{
	const Avm2PropEntry* e = avm2_vtable_find_public(vt, name, len);
	if (e == NULL || e->kind != AVM2_PROP_SLOT) return 0;
	return e->slot_index;
}

// `internal var _group` is not in the public namespace, so find_public misses
// it — linear-scan the vtable by name only.
static uint32_t slot_of_any_name(const Avm2VTable* vt, const char* name, uint32_t len)
{
	for (uint32_t i = 0; i < vt->count; i++)
	{
		const Avm2PropEntry* e = &vt->entries[i];
		if (e->kind != AVM2_PROP_SLOT) continue;
		if (e->key.name_len == len && e->key.name != NULL
		    && memcmp(e->key.name, name, len) == 0)
		{
			return e->slot_index;
		}
	}
	return 0;
}

static const FlxObjSlots* flxobj_slots(Avm2Object* o)
{
	Avm2Class* cls = o->cls;
	uint32_t idx = (uint32_t) (((uintptr_t) cls >> 4) & (FLXOBJ_CACHE_N - 1u));
	FlxObjSlots* s = &g_slot_cache[idx];
	if (s->cls == cls) return s;
	const Avm2VTable* vt = (o->vtable != NULL) ? o->vtable : &cls->ivtable;
	s->cls = cls;
	s->s_x = slot_of_public(vt, "x", 1);
	s->s_y = slot_of_public(vt, "y", 1);
	s->s_w = slot_of_public(vt, "width", 5);
	s->s_h = slot_of_public(vt, "height", 6);
	s->s_exists = slot_of_public(vt, "exists", 6);
	s->s_solid = slot_of_public(vt, "solid", 5);
	s->s_group = slot_of_any_name(vt, "_group", 6);
	return s;
}

static Avm2Value fo_read(Avm2Context* ctx, Avm2Object* o, uint32_t slot,
                         const char* name, uint32_t len)
{
	if (slot != 0 && slot < o->slot_count) return o->slots[slot];
	return avm2_get_public_property(ctx, avm2_object_value(o), name, len, NULL);
}

// AS3 `Number`-typed slots. coerce_to_number on a Number/int slot is exact.
static double fo_x(Avm2Context* ctx, Avm2Object* o)
{
	return avm2_coerce_to_number(ctx, fo_read(ctx, o, flxobj_slots(o)->s_x, "x", 1));
}
static double fo_y(Avm2Context* ctx, Avm2Object* o)
{
	return avm2_coerce_to_number(ctx, fo_read(ctx, o, flxobj_slots(o)->s_y, "y", 1));
}
static double fo_w(Avm2Context* ctx, Avm2Object* o)
{
	return avm2_coerce_to_number(ctx, fo_read(ctx, o, flxobj_slots(o)->s_w, "width", 5));
}
static double fo_h(Avm2Context* ctx, Avm2Object* o)
{
	return avm2_coerce_to_number(ctx, fo_read(ctx, o, flxobj_slots(o)->s_h, "height", 6));
}
static int fo_exists(Avm2Context* ctx, Avm2Object* o)
{
	return avm2_coerce_to_boolean(fo_read(ctx, o, flxobj_slots(o)->s_exists, "exists", 6)) ? 1 : 0;
}
static int fo_solid(Avm2Context* ctx, Avm2Object* o)
{
	return avm2_coerce_to_boolean(fo_read(ctx, o, flxobj_slots(o)->s_solid, "solid", 5)) ? 1 : 0;
}
static int fo_group(Avm2Context* ctx, Avm2Object* o)
{
	return avm2_coerce_to_boolean(fo_read(ctx, o, flxobj_slots(o)->s_group, "_group", 6)) ? 1 : 0;
}

// `_loc4_[_loc6_] as FlxObject` (FlxQuadTree.as:342). Walk the class chain by
// qualified name — exact, and short (FlxObject chains are 3-6 deep). Yields
// NULL for non-objects and for objects that are not FlxObjects, matching `as`.
static Avm2Object* as_flxobject(Avm2Value v)
{
	if (v.kind != AVM2_VALUE_OBJECT || v.u.obj == NULL) return NULL;
	for (Avm2Class* c = v.u.obj->cls; c != NULL; c = c->super_class)
	{
		if (c->name.name_len == 9 && c->name.name != NULL
		    && memcmp(c->name.name, "FlxObject", 9) == 0)
		{
			return v.u.obj;
		}
	}
	return NULL;
}

// `members:Array` element/length access. Dense-first with a sparse walk, and a
// generic property fallback if the value is somehow not a native Array.
static uint32_t arr_length(Avm2Context* ctx, Avm2Value av)
{
	if (av.kind == AVM2_VALUE_OBJECT && av.u.obj != NULL
	    && av.u.obj->kind == AVM2_OBJ_ARRAY && av.u.obj->native_ext != NULL)
	{
		return ((Avm2ArrayExt*) av.u.obj->native_ext)->length;
	}
	return avm2_coerce_to_u32(ctx, avm2_get_public_property(ctx, av, "length", 6, NULL));
}

static Avm2Value arr_get(Avm2Context* ctx, Avm2Value av, uint32_t i)
{
	if (av.kind == AVM2_VALUE_OBJECT && av.u.obj != NULL
	    && av.u.obj->kind == AVM2_OBJ_ARRAY && av.u.obj->native_ext != NULL)
	{
		Avm2ArrayExt* e = (Avm2ArrayExt*) av.u.obj->native_ext;
		if (i < e->dense_len)
		{
			Avm2Value v = e->elems[i];
			return (v.kind == AVM2_VALUE_HOLE) ? avm2_undefined() : v;
		}
		for (Avm2SparseElem* s = e->sparse; s != NULL && s->idx <= i; s = s->next)
			if (s->idx == i) return s->v;
		return avm2_undefined();
	}
	char buf[24];
	int n = snprintf(buf, sizeof buf, "%u", (unsigned) i);
	return avm2_get_public_property(ctx, av, buf, (uint32_t) n, NULL);
}

// ============================================================================
// FlxList / FlxQuadTree construction
// ============================================================================

// FlxList.as:12-17 — `new FlxList()`: object = null, next = null.
static FlxQTList* list_new(Avm2Context* ctx, FlxQTExt* ext)
{
	FlxQTList* l = (FlxQTList*) arena_alloc(ctx, &ext->arena, (uint32_t) sizeof(FlxQTList));
	l->next = NULL;
	l->object = NULL;
	return l;
}

// FlxQuadTree.as:62-115 — the constructor.
static FlxQTNode* qt_node_new(Avm2Context* ctx, FlxQTExt* ext,
                              double param1, double param2, double param3, double param4,
                              FlxQTNode* param5)
{
	g_stat_ctor++;
	FlxQTList* _loc6_ = NULL;
	FlxQTList* _loc7_ = NULL;
	FlxQTNode* self = (FlxQTNode*) arena_alloc(ctx, &ext->arena, (uint32_t) sizeof(FlxQTNode));
	memset(self, 0, sizeof *self);
	// :66 super(param1,param2,param3,param4) — FlxRect(x,y,width,height)
	self->x = param1;
	self->y = param2;
	self->width = param3;
	self->height = param4;
	// :67 (2.21) / :113-114 (2.35) — the subdivision threshold.
	//  2.21 (id 1): fixed MIN=48 at every node.
	//  2.35 (id 3): the ROOT (param5==NULL) computes a shared `_min` from its
	//    own size and FlxU.quadTreeDivisions (=6 for RWF/RWIC, see FLXQT_MIN
	//    comment); the value is a uint (truncated), and every child reuses it
	//    because `_min` is static. We mirror that by setting g_qt_min once on
	//    the root and reading it on every node — faithful to the static.
	if (param5 == NULL)
	{
		g_qt_min = g_qt_is_235
			? (double) (uint32_t) ((param3 + param4) / (2.0 * FLXQT_DIVISIONS_235))
			: FLXQT_MIN;
	}
	self->_canSubdivide = (param3 > g_qt_min || param4 > g_qt_min) ? 1 : 0;
	// :68-69 — QUIRK 4: EVERY node, leaves included, allocates two list nodes.
	self->_headA = self->_tailA = list_new(ctx, ext);
	self->_headB = self->_tailB = list_new(ctx, ext);
	// :70-102 — QUIRK 3: the parent-list copy runs for BOTH A and B, each
	// guarded independently by `param5._headX.object != null`.
	if (param5 != NULL)
	{
		if (param5->_headA->object != NULL)          // :72
		{
			_loc6_ = param5->_headA;                 // :74
			while (_loc6_ != NULL)                   // :75
			{
				if (self->_tailA->object != NULL)    // :77
				{
					_loc7_ = self->_tailA;           // :79
					self->_tailA = list_new(ctx, ext); // :80
					_loc7_->next = self->_tailA;     // :81
				}
				self->_tailA->object = _loc6_->object; // :83
				_loc6_ = _loc6_->next;               // :84
			}
		}
		if (param5->_headB->object != NULL)          // :87
		{
			_loc6_ = param5->_headB;                 // :89
			while (_loc6_ != NULL)                   // :90
			{
				if (self->_tailB->object != NULL)    // :92
				{
					_loc7_ = self->_tailB;           // :94
					self->_tailB = list_new(ctx, ext); // :95
					_loc7_->next = self->_tailB;     // :96
				}
				self->_tailB->object = _loc6_->object; // :98
				_loc6_ = _loc6_->next;               // :99
			}
		}
	}
	self->_nw = NULL;                                // :103
	self->_ne = NULL;                                // :104
	self->_se = NULL;                                // :105
	self->_sw = NULL;                                // :106
	self->_l = self->x;                              // :107
	self->_r = self->x + self->width;                // :108
	self->_hw = self->width / 2;                     // :109
	self->_mx = self->_l + self->_hw;                // :110
	self->_t = self->y;                              // :111
	self->_b = self->y + self->height;               // :112
	self->_hh = self->height / 2;                    // :113
	self->_my = self->_t + self->_hh;                // :114
	return self;
}

// ============================================================================
// addToList / addObject
// ============================================================================

// FlxQuadTree.as:200-243
static void qt_add_to_list(Avm2Context* ctx, FlxQTExt* ext, FlxQTNode* self)
{
	FlxQTList* _loc1_ = NULL;
	if (g_oa == FLXQT_A_LIST)                        // :203
	{
		if (self->_tailA->object != NULL)            // :205
		{
			_loc1_ = self->_tailA;                   // :207
			self->_tailA = list_new(ctx, ext);       // :208
			_loc1_->next = self->_tailA;             // :209
		}
		self->_tailA->object = g_o;                  // :211
	}
	else
	{
		if (self->_tailB->object != NULL)            // :215
		{
			_loc1_ = self->_tailB;                   // :217
			self->_tailB = list_new(ctx, ext);       // :218
			_loc1_->next = self->_tailB;             // :219
		}
		self->_tailB->object = g_o;                  // :221
	}
	if (!self->_canSubdivide) return;                // :223-226
	// QUIRK 5: when _canSubdivide, recurse into ALL four existing children
	// unconditionally — AFTER having appended to this node's own list.
	if (self->_nw != NULL) qt_add_to_list(ctx, ext, self->_nw);  // :227-230
	if (self->_ne != NULL) qt_add_to_list(ctx, ext, self->_ne);  // :231-234
	if (self->_se != NULL) qt_add_to_list(ctx, ext, self->_se);  // :235-238
	if (self->_sw != NULL) qt_add_to_list(ctx, ext, self->_sw);  // :239-242
}

// FlxQuadTree.as:117-198
static void qt_add_object(Avm2Context* ctx, FlxQTExt* ext, FlxQTNode* self)
{
	g_stat_addobj++;
	// :119
	if (!self->_canSubdivide
	    || (self->_l >= g_ol && self->_r <= g_or && self->_t >= g_ot && self->_b <= g_ob))
	{
		qt_add_to_list(ctx, ext, self);              // :121
		return;                                      // :122
	}
	if (g_ol > self->_l && g_or < self->_mx)         // :124
	{
		if (g_ot > self->_t && g_ob < self->_my)     // :126
		{
			if (self->_nw == NULL)                   // :128
				self->_nw = qt_node_new(ctx, ext, self->_l, self->_t, self->_hw, self->_hh, self);  // :130
			qt_add_object(ctx, ext, self->_nw);      // :132
			return;                                  // :133
		}
		if (g_ot > self->_my && g_ob < self->_b)     // :135
		{
			if (self->_sw == NULL)                   // :137
				self->_sw = qt_node_new(ctx, ext, self->_l, self->_my, self->_hw, self->_hh, self); // :139
			qt_add_object(ctx, ext, self->_sw);      // :141
			return;                                  // :142
		}
	}
	if (g_ol > self->_mx && g_or < self->_r)         // :145
	{
		if (g_ot > self->_t && g_ob < self->_my)     // :147
		{
			if (self->_ne == NULL)                   // :149
				self->_ne = qt_node_new(ctx, ext, self->_mx, self->_t, self->_hw, self->_hh, self); // :151
			qt_add_object(ctx, ext, self->_ne);      // :153
			return;                                  // :154
		}
		if (g_ot > self->_my && g_ob < self->_b)     // :156
		{
			if (self->_se == NULL)                   // :158
				self->_se = qt_node_new(ctx, ext, self->_mx, self->_my, self->_hw, self->_hh, self); // :160
			qt_add_object(ctx, ext, self->_se);      // :162
			return;                                  // :163
		}
	}
	if (g_or > self->_l && g_ol < self->_mx && g_ob > self->_t && g_ot < self->_my)   // :166
	{
		if (self->_nw == NULL)                       // :168
			self->_nw = qt_node_new(ctx, ext, self->_l, self->_t, self->_hw, self->_hh, self);      // :170
		qt_add_object(ctx, ext, self->_nw);          // :172
	}
	if (g_or > self->_mx && g_ol < self->_r && g_ob > self->_t && g_ot < self->_my)   // :174
	{
		if (self->_ne == NULL)                       // :176
			self->_ne = qt_node_new(ctx, ext, self->_mx, self->_t, self->_hw, self->_hh, self);     // :178
		qt_add_object(ctx, ext, self->_ne);          // :180
	}
	if (g_or > self->_mx && g_ol < self->_r && g_ob > self->_my && g_ot < self->_b)   // :182
	{
		if (self->_se == NULL)                       // :184
			self->_se = qt_node_new(ctx, ext, self->_mx, self->_my, self->_hw, self->_hh, self);    // :186
		qt_add_object(ctx, ext, self->_se);          // :188
	}
	if (g_or > self->_l && g_ol < self->_mx && g_ob > self->_my && g_ot < self->_b)   // :190
	{
		if (self->_sw == NULL)                       // :192
			self->_sw = qt_node_new(ctx, ext, self->_l, self->_my, self->_hw, self->_hh, self);     // :194
		qt_add_object(ctx, ext, self->_sw);          // :196
	}
}

// ============================================================================
// overlapNode
// ============================================================================

// FlxQuadTree.as:373-415
static int qt_overlap_node(Avm2Context* ctx, FlxQTNode* self, FlxQTList* param1)
{
	g_stat_overlapnode++;
	Avm2Object* _loc3_ = NULL;                       // :375
	int _loc2_ = 0;                                  // :376
	FlxQTList* _loc4_ = param1;                      // :377
	// QUIRK 1 (receiving end): a null argument is indistinguishable from the
	// omitted default, so `overlapNode(_loc4_.next)` with a null `next` falls
	// into the head-of-list path using whatever _oa the LAST add() left behind.
	// This is a genuine Flixel quirk and it is load-bearing — do NOT "fix" it.
	if (_loc4_ == NULL)                              // :378
	{
		_loc4_ = (g_oa == FLXQT_A_LIST) ? self->_headA : self->_headB;  // :380-387
	}
	if (_loc4_->object != NULL)                      // :389
	{
		while (_loc4_ != NULL)                       // :391
		{
			_loc3_ = _loc4_->object;                 // :393
			// :394 — QUIRK 2: exact evaluation order and short-circuiting of
			// the whole `||` chain, identity test FIRST. The field reads are
			// deliberately NOT hoisted out of the chain.
			if (g_o == _loc3_
			    || !fo_exists(ctx, _loc3_)
			    || !fo_exists(ctx, g_o)
			    || !fo_solid(ctx, _loc3_)
			    || !fo_solid(ctx, g_o)
			    || fo_x(ctx, g_o) + fo_w(ctx, g_o) < fo_x(ctx, _loc3_) + FLXU_ROUNDING_ERROR
			    || fo_x(ctx, g_o) + FLXU_ROUNDING_ERROR > fo_x(ctx, _loc3_) + fo_w(ctx, _loc3_)
			    || fo_y(ctx, g_o) + fo_h(ctx, g_o) < fo_y(ctx, _loc3_) + FLXU_ROUNDING_ERROR
			    || fo_y(ctx, g_o) + FLXU_ROUNDING_ERROR > fo_y(ctx, _loc3_) + fo_h(ctx, _loc3_))
			{
				_loc4_ = _loc4_->next;               // :396
			}
			else
			{
				// :400 — `_oc == null` is AS3 loose equality: null OR undefined.
				if (g_oc.kind == AVM2_VALUE_NULL || g_oc.kind == AVM2_VALUE_UNDEFINED)
				{
					// :402-403 — kill() stays VIRTUAL: FlxGroup overrides it
					// (FlxGroup.as:322), FlxObject.as:131 is the base.
					avm2_call_public_property(ctx, avm2_object_value(g_o), "kill", 4, NULL, 0);
					avm2_call_public_property(ctx, avm2_object_value(_loc3_), "kill", 4, NULL, 0);
					_loc2_ = 1;                      // :404
				}
				else
				{
					Avm2Value cargs[2];              // :406 `_oc(_o,_loc3_)`
					cargs[0] = avm2_object_value(g_o);
					cargs[1] = avm2_object_value(_loc3_);
					Avm2Value r = avm2_call_value(ctx, g_oc, avm2_null(), cargs, 2);
					if (avm2_coerce_to_boolean(r)) _loc2_ = 1;  // :408
				}
				_loc4_ = _loc4_->next;               // :410
			}
		}
	}
	return _loc2_;                                   // :414
}

// ============================================================================
// overlap
// ============================================================================

// FlxQuadTree.as:245-326
static int qt_overlap(Avm2Context* ctx, FlxQTNode* self, int param1, Avm2Value param2)
{
	g_stat_overlap++;
	FlxQTList* _loc4_ = NULL;                        // :247
	// QUIRK 7: _oc is reassigned from param2 on entry at EVERY recursion level.
	g_oc = param2;                                   // :248
	int _loc3_ = 0;                                  // :249
	if (param1)                                      // :250
	{
		g_oa = FLXQT_B_LIST;                         // :252
		if (self->_headA->object != NULL)            // :253
		{
			_loc4_ = self->_headA;                   // :255
			while (_loc4_ != NULL)                   // :256
			{
				g_o = _loc4_->object;                // :258
				// :259 — overlapNode() with NO argument.
				if (fo_exists(ctx, g_o) && fo_solid(ctx, g_o)
				    && qt_overlap_node(ctx, self, NULL))
				{
					_loc3_ = 1;                      // :261
				}
				_loc4_ = _loc4_->next;               // :263
			}
		}
		g_oa = FLXQT_A_LIST;                         // :266
		if (self->_headB->object != NULL)            // :267
		{
			_loc4_ = self->_headB;                   // :269
			while (_loc4_ != NULL)                   // :270
			{
				g_o = _loc4_->object;                // :272
				if (fo_exists(ctx, g_o) && fo_solid(ctx, g_o))   // :273
				{
					if (self->_nw != NULL && qt_overlap_node(ctx, self->_nw, NULL)) _loc3_ = 1;  // :275-278
					if (self->_ne != NULL && qt_overlap_node(ctx, self->_ne, NULL)) _loc3_ = 1;  // :279-282
					if (self->_se != NULL && qt_overlap_node(ctx, self->_se, NULL)) _loc3_ = 1;  // :283-286
					if (self->_sw != NULL && qt_overlap_node(ctx, self->_sw, NULL)) _loc3_ = 1;  // :287-290
				}
				_loc4_ = _loc4_->next;               // :292
			}
		}
	}
	else if (self->_headA->object != NULL)           // :296
	{
		_loc4_ = self->_headA;                       // :298
		while (_loc4_ != NULL)                       // :299
		{
			g_o = _loc4_->object;                    // :301
			// :302 — QUIRK 1 (calling end): `_loc4_.next` CAN be null; passing
			// it is indistinguishable from omitting the arg. Preserved verbatim.
			if (fo_exists(ctx, g_o) && fo_solid(ctx, g_o)
			    && qt_overlap_node(ctx, self, _loc4_->next))
			{
				_loc3_ = 1;                          // :304
			}
			_loc4_ = _loc4_->next;                   // :306
		}
	}
	// :309-324 — each child call passes the CURRENT value of the _oc static
	// (read fresh at every call site, exactly as the bytecode does).
	if (self->_nw != NULL && qt_overlap(ctx, self->_nw, param1, g_oc)) _loc3_ = 1;
	if (self->_ne != NULL && qt_overlap(ctx, self->_ne, param1, g_oc)) _loc3_ = 1;
	if (self->_se != NULL && qt_overlap(ctx, self->_se, param1, g_oc)) _loc3_ = 1;
	if (self->_sw != NULL && qt_overlap(ctx, self->_sw, param1, g_oc)) _loc3_ = 1;
	return _loc3_;                                   // :325
}

// ============================================================================
// add
// ============================================================================

// FlxQuadTree.as:328-371. `add(_loc3_,param2)` at :347 is a virtual self-call
// on the same (root) node, so it becomes plain C recursion on `self`.
static void qt_add(Avm2Context* ctx, FlxQTExt* ext, FlxQTNode* self,
                   Avm2Object* param1, uint32_t param2)
{
	g_stat_add++;
	Avm2Object* _loc3_ = NULL;                       // :330
	g_oa = param2;                                   // :334
	if (fo_group(ctx, param1))                       // :335
	{
		// :337 `(param1 as FlxGroup).members`
		Avm2Value _loc4_ = avm2_get_public_property(ctx, avm2_object_value(param1),
		                                            "members", 7, NULL);
		uint32_t _loc5_ = arr_length(ctx, _loc4_);   // :338 — read ONCE, before the loop
		uint32_t _loc6_ = 0;                         // :339
		while (_loc6_ < _loc5_)                      // :340
		{
			_loc3_ = as_flxobject(arr_get(ctx, _loc4_, _loc6_));   // :342
			if (_loc3_ != NULL && fo_exists(ctx, _loc3_))          // :343
			{
				if (fo_group(ctx, _loc3_))           // :345
				{
					qt_add(ctx, ext, self, _loc3_, param2);        // :347
				}
				else if (fo_solid(ctx, _loc3_))      // :349
				{
					g_o = _loc3_;                    // :351
					g_ol = fo_x(ctx, g_o);           // :352
					g_ot = fo_y(ctx, g_o);           // :353
					g_or = fo_x(ctx, g_o) + fo_w(ctx, g_o);   // :354
					g_ob = fo_y(ctx, g_o) + fo_h(ctx, g_o);   // :355
					qt_add_object(ctx, ext, self);   // :356
				}
			}
			_loc6_++;                                // :359
		}
	}
	// QUIRK 6: this runs UNCONDITIONALLY after the group loop — a group whose
	// own `solid` is true is added as an object in its own right too.
	if (fo_solid(ctx, param1))                       // :362
	{
		g_o = param1;                                // :364
		g_ol = fo_x(ctx, g_o);                       // :365
		g_ot = fo_y(ctx, g_o);                       // :366
		g_or = fo_x(ctx, g_o) + fo_w(ctx, g_o);      // :367
		g_ob = fo_y(ctx, g_o) + fo_h(ctx, g_o);      // :368
		qt_add_object(ctx, ext, self);               // :369
	}
}

// ============================================================================
// Native method bodies (vtable / instance_init entry points)
// ============================================================================

static FlxQTExt* qt_ext_of(Avm2Value this_val)
{
	if (this_val.kind != AVM2_VALUE_OBJECT || this_val.u.obj == NULL) return NULL;
	Avm2Object* o = this_val.u.obj;
	if (g_qt_cls == NULL || o->cls != g_qt_cls) return NULL;
	return (FlxQTExt*) o->native_ext;
}

// FlxQuadTree.as:62 — instance_init for the ROOT object.
static Avm2Value qt_native_ctor(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* self = (act->this_val.kind == AVM2_VALUE_OBJECT) ? act->this_val.u.obj : NULL;
	if (self == NULL || self->native_ext == NULL) return avm2_undefined();
	FlxQTExt* ext = (FlxQTExt*) self->native_ext;

	// Declared signature is (Number, Number, Number, Number, FlxQuadTree=null);
	// no coercion or arg-count checking is done for us.
	double p1 = (act->argc > 0) ? avm2_coerce_to_number(ctx, act->args[0]) : NAN;
	double p2 = (act->argc > 1) ? avm2_coerce_to_number(ctx, act->args[1]) : NAN;
	double p3 = (act->argc > 2) ? avm2_coerce_to_number(ctx, act->args[2]) : NAN;
	double p4 = (act->argc > 3) ? avm2_coerce_to_number(ctx, act->args[3]) : NAN;
	FlxQTNode* parent = NULL;
	if (act->argc > 4)
	{
		FlxQTExt* pext = qt_ext_of(act->args[4]);
		if (pext != NULL) parent = pext->root;
	}

	ext->arena.chunks = NULL;
	ext->arena.inline_used = 0;
	ext->arena.next_cap = FLXQT_CHUNK_MIN;
	ext->root = qt_node_new(ctx, ext, p1, p2, p3, p4, parent);

	// Mirror super(x,y,width,height) onto the real FlxRect slots so any AS3
	// read of the root's rect sees what it would have seen.
	if (self->slots != NULL)
	{
		if (g_qt_s_x != 0 && g_qt_s_x < self->slot_count) self->slots[g_qt_s_x] = avm2_number(p1);
		if (g_qt_s_y != 0 && g_qt_s_y < self->slot_count) self->slots[g_qt_s_y] = avm2_number(p2);
		if (g_qt_s_w != 0 && g_qt_s_w < self->slot_count) self->slots[g_qt_s_w] = avm2_number(p3);
		if (g_qt_s_h != 0 && g_qt_s_h < self->slot_count) self->slots[g_qt_s_h] = avm2_number(p4);
	}
	return avm2_undefined();
}

// FlxQuadTree.as:245 `public function overlap(param1:Boolean = true, param2:Function = null)`
static Avm2Value qt_native_overlap(Avm2Activation* act)
{
	FlxQTExt* ext = qt_ext_of(act->this_val);
	if (ext == NULL || ext->root == NULL) return avm2_bool(false);
	int param1 = (act->argc > 0) ? (avm2_coerce_to_boolean(act->args[0]) ? 1 : 0) : 1;
	Avm2Value param2 = (act->argc > 1) ? act->args[1] : avm2_null();
	return avm2_bool(qt_overlap(act->ctx, ext->root, param1, param2) ? true : false);
}

// FlxQuadTree.as:328 `public function add(param1:FlxObject, param2:uint)`
static Avm2Value qt_native_add(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	FlxQTExt* ext = qt_ext_of(act->this_val);
	if (ext == NULL || ext->root == NULL) return avm2_undefined();
	Avm2Value a0 = (act->argc > 0) ? act->args[0] : avm2_undefined();
	if (a0.kind != AVM2_VALUE_OBJECT || a0.u.obj == NULL) return avm2_undefined();
	uint32_t param2 = (act->argc > 1) ? avm2_coerce_to_u32(ctx, act->args[1]) : 0u;
	qt_add(ctx, ext, ext->root, a0.u.obj, param2);
	return avm2_undefined();
}

// ============================================================================
// FlxTilemap.arrayToCSV (intrinsic id 4)
// ============================================================================
//
// AS3 source (jpexs org/flixel/FlxTilemap.as, Flixel 2.21 clean form; the two
// 2.35 titles carry opaque-predicate obfuscation that folds to this exact
// nest — see the per-title fingerprints in SWFRecomp/src/abc/abc_emit.cpp):
//
//   public static function arrayToCSV(param1:Array, param2:int) : String {
//      var _loc5_:String = null;
//      var _loc6_:int = param1.length / param2;
//      _loc3_ = 0;
//      while (_loc3_ < _loc6_) {                 // row
//         _loc4_ = 0;
//         while (_loc4_ < param2) {              // col
//            if (_loc4_ == 0) {
//               if (_loc3_ == 0) _loc5_ += param1[0];
//               else             _loc5_ += "\n" + param1[_loc3_ * param2];
//            } else               _loc5_ += ", " + param1[_loc3_ * param2 + _loc4_];
//            _loc4_++;
//         }
//         _loc3_++;
//      }
//      return _loc5_;
//   }
//
// The problem this replaces: `_loc5_ += ...` builds a fresh immutable String
// every iteration — O(n^2) total intermediate bytes. For RWIC's 188x84 x3
// tilemaps that is a measured ~1.7 GB single-tick transient (footprint plan
// §2). The native builder appends each cell's short string into ONE
// grow-realloc C scratch buffer (system malloc, freed before return, never
// touches the AVM2 arena) and materializes the final CSV once — O(n).
//
// BYTE-IDENTITY (the hard contract). Every cell's contribution is produced by
// the runtime's OWN coercion primitives, so the output matches the AS3 arm
// bit-for-bit by construction, not by re-deriving Number->String formatting:
//   * cell (0,0): `_loc5_` is null, so `null + param1[0]` is a NUMERIC add
//     (neither operand is a String) whose result is then string-coerced by the
//     typed-String `+=`. Reproduced with avm2_op_add_values(null, v) then
//     avm2_coerce_to_string — the exact interpreter path.
//   * every other cell: the literal ", "/"\n" separator is a String, so
//     `sep + param1[i]` is a string concat = sep ++ ToString(param1[i]).
//     Reproduced as append(sep) + append(avm2_coerce_to_string(v)).
// The AVM2_ARRAYTOCSV_SELFCHECK=1 env compares this fast builder against a
// literal O(n^2) transliteration of the loop nest above (using the same
// primitives) on every call and aborts on any mismatch — a self-contained
// byte-identity proof independent of any game.

// Grow-realloc byte buffer (transient C scratch, system allocator).
typedef struct { char* p; size_t n, cap; } CsvBuf;
static void csv_reserve(CsvBuf* b, size_t extra)
{
	if (b->n + extra <= b->cap) return;
	size_t nc = b->cap ? b->cap : 256;
	while (b->n + extra > nc) nc += nc / 2 + 1;
	b->p = (char*) realloc(b->p, nc);
	b->cap = nc;
}
static void csv_append(CsvBuf* b, const char* s, size_t len)
{
	if (len == 0) return;
	csv_reserve(b, len);
	memcpy(b->p + b->n, s, len);
	b->n += len;
}

// _loc6_:int = param1.length / param2. AS3 evaluates length/param2 as Number
// division and `int(...)` truncates toward zero. Guard the degenerate divisors
// exactly as AS3 would (int(±Inf)=0, int(NaN)=0), and note the loop body only
// runs — and _loc5_ is only ever assigned — when rows>=1 AND width>=1; when it
// never runs, `return _loc5_` returns null (NOT "").
static int32_t csv_rows(uint32_t len, int32_t width)
{
	if (width == 0) return 0;             // int(±Inf or NaN) = 0
	double q = (double) len / (double) width;
	if (q != q) return 0;                 // NaN
	return (int32_t) q;                   // trunc toward zero
}

// Append one cell's coerced-string contribution to `b`, faithfully.
static void csv_emit_cell(Avm2Context* ctx, CsvBuf* b, Avm2Value arr,
                          int32_t r, int32_t width, int32_t c)
{
	int64_t idx = (int64_t) r * (int64_t) width + (int64_t) c;
	Avm2Value v = arr_get(ctx, arr, (uint32_t) idx);
	if (r == 0 && c == 0)
	{
		// null + param1[0]: numeric add, then string-coerced by the typed +=.
		Avm2Value sum = avm2_op_add_values(ctx, avm2_null(), v);
		const Avm2String* s = avm2_coerce_to_string(ctx, sum);
		csv_append(b, s->utf8, s->len);
		return;
	}
	if (c == 0) csv_append(b, "\n", 1);
	else        csv_append(b, ", ", 2);
	const Avm2String* s = avm2_coerce_to_string(ctx, v);
	csv_append(b, s->utf8, s->len);
}

// The fast O(n) builder. Returns the final Avm2String (or NULL to signal the
// AS3 `null` return — loop never ran).
static const Avm2String* csv_build_fast(Avm2Context* ctx, Avm2Value arr,
                                        int32_t width, uint32_t len)
{
	int32_t rows = csv_rows(len, width);
	if (rows < 1 || width < 1) return NULL;   // _loc5_ stays null
	CsvBuf b = { NULL, 0, 0 };
	for (int32_t r = 0; r < rows; r++)
		for (int32_t c = 0; c < width; c++)
			csv_emit_cell(ctx, &b, arr, r, width, c);
	const Avm2String* out = avm2_string_new(ctx, b.p ? b.p : "", (uint32_t) b.n);
	free(b.p);
	return out;
}

// Literal O(n^2) transliteration of the AS3 loop nest — the byte-identity
// reference. `_loc5_ += X` is `_loc5_ = coerce_s(_loc5_ + X)`; coerce_s of
// null/undefined is null, else ToString. Used only under selfcheck.
static const Avm2String* csv_build_ref(Avm2Context* ctx, Avm2Value arr,
                                       int32_t width, uint32_t len)
{
	int32_t rows = csv_rows(len, width);
	if (rows < 1 || width < 1) return NULL;
	const Avm2String* nl = avm2_string_new(ctx, "\n", 1);
	const Avm2String* cm = avm2_string_new(ctx, ", ", 2);
	Avm2Value acc = avm2_null();
	for (int32_t r = 0; r < rows; r++)
	{
		for (int32_t c = 0; c < width; c++)
		{
			int64_t idx = (int64_t) r * (int64_t) width + (int64_t) c;
			Avm2Value v = arr_get(ctx, arr, (uint32_t) idx);
			Avm2Value rhs;
			if (r == 0 && c == 0)
				rhs = v;                                  // _loc5_ += param1[0]
			else
				rhs = avm2_op_add_values(ctx,
				          avm2_string(c == 0 ? nl : cm), v);   // sep + v
			Avm2Value tmp = avm2_op_add_values(ctx, acc, rhs); // _loc5_ + rhs
			// coerce_s: null/undef -> null, else ToString.
			acc = (tmp.kind == AVM2_VALUE_NULL || tmp.kind == AVM2_VALUE_UNDEFINED)
			      ? avm2_null()
			      : avm2_string(avm2_coerce_to_string(ctx, tmp));
		}
	}
	return (acc.kind == AVM2_VALUE_STRING) ? acc.u.str : NULL;
}

static int g_csv_selfcheck = -1;

// FlxTilemap.as — `public static function arrayToCSV(param1:Array, param2:int)`
static Avm2Value tilemap_native_arraytocsv(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value arr = (act->argc > 0) ? act->args[0] : avm2_undefined();
	int32_t width = (act->argc > 1) ? avm2_coerce_to_i32(ctx, act->args[1]) : 0;
	uint32_t len = arr_length(ctx, arr);

	const Avm2String* out = csv_build_fast(ctx, arr, width, len);

	if (g_csv_selfcheck < 0)
	{
		const char* e = getenv("AVM2_ARRAYTOCSV_SELFCHECK");
		g_csv_selfcheck = (e != NULL && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
	}
	if (g_csv_selfcheck)
	{
		const Avm2String* ref = csv_build_ref(ctx, arr, width, len);
		int ok = (out == NULL && ref == NULL) ||
		         (out != NULL && ref != NULL && avm2_string_equals(out, ref));
		fprintf(stderr, "[ARRAYTOCSV] selfcheck len=%u width=%d rows=%d "
		        "out=%u ref=%u %s\n", len, width, csv_rows(len, width),
		        out ? out->len : 0u, ref ? ref->len : 0u, ok ? "OK" : "MISMATCH");
		if (!ok) abort();
	}

	g_stat_arraytocsv++;
	return (out != NULL) ? avm2_string(out) : avm2_null();
}

// ============================================================================
// GC hooks
// ============================================================================

static void trace_node(FlxQTNode* n)
{
	// _tailA/_tailB always sit inside the _headA/_headB chains, so walking the
	// heads covers every list node.
	for (FlxQTList* l = n->_headA; l != NULL; l = l->next) avm2_gc_mark_object(l->object);
	for (FlxQTList* l = n->_headB; l != NULL; l = l->next) avm2_gc_mark_object(l->object);
	if (n->_nw != NULL) trace_node(n->_nw);
	if (n->_ne != NULL) trace_node(n->_ne);
	if (n->_se != NULL) trace_node(n->_se);
	if (n->_sw != NULL) trace_node(n->_sw);
}

void avm2_flixel_gc_trace_ext(Avm2Object* o)
{
	if (g_qt_cls == NULL || o == NULL || o->cls != g_qt_cls) return;
	FlxQTExt* ext = (FlxQTExt*) o->native_ext;
	if (ext == NULL || ext->root == NULL) return;
	trace_node(ext->root);
}

void avm2_flixel_gc_free_ext(Avm2Context* ctx, Avm2Object* o)
{
	if (g_qt_cls == NULL || o == NULL || o->cls != g_qt_cls) return;
	FlxQTExt* ext = (FlxQTExt*) o->native_ext;
	if (ext == NULL) return;
	arena_free(ctx, &ext->arena);
	ext->root = NULL;
}

void avm2_gc_mark_roots_flixel(Avm2Context* ctx)
{
	(void) ctx;
	if (g_qt_cls == NULL) return;
	avm2_gc_mark_object(g_o);
	avm2_gc_mark_value(g_oc);
}

// ============================================================================
// Install
// ============================================================================

// Mutable vtable lookup. avm2_vtable_append has no dedup and lookups are
// first-match-wins, so an override MUST mutate the existing entry in place —
// never append.
static Avm2PropEntry* vtable_find_public_mut(Avm2VTable* vt, const char* name, uint32_t len)
{
	const Avm2PropEntry* e = avm2_vtable_find_public(vt, name, len);
	return (Avm2PropEntry*) e;
}

// A native method is signalled by ref.file == NULL, NOT by fn == NULL. Leaving
// a stale non-NULL `file` behind would make dispatch index method_index into
// the GAME's method table.
// Resolve only — never mutate. Install is two-phase (resolve every target
// first, commit only once all of them resolved) so a partial override can
// never happen: a half-patched vtable would leave a native `overlap` running
// against g_qt_cls == NULL, which returns false and silently kills collision
// instead of falling back to the game's own code.
static Avm2PropEntry* resolve_method(Avm2VTable* vt, const char* name, uint32_t len)
{
	Avm2PropEntry* e = vtable_find_public_mut(vt, name, len);
	if (e == NULL || e->kind != AVM2_PROP_METHOD) return NULL;
	return e;
}

static void commit_method(Avm2PropEntry* e, Avm2MethodFn fn, const char* debug_name)
{
	e->method.fn = fn;
	e->method.file = NULL;
	e->method.method_index = 0;
	e->method.debug_name = debug_name;
}

static void qt_native_init(Avm2Context* ctx, Avm2Object* obj)
{
	(void) ctx;
	(void) obj;
	// The ext blob is already allocated and zeroed by avm2_class_construct;
	// the arena is armed in qt_native_ctor (instance_init), which always runs
	// immediately after this.
}

// AVM2_FLIXEL_STATS=1: dump the native call counters at exit, so the
// intrinsic-vs-fallback oracle can prove the native path actually engaged.
static void flixel_stats_atexit(void)
{
	fprintf(stderr,
	        "FLIXEL_STATS variant=%s ctor=%lu add=%lu addObject=%lu "
	        "overlap=%lu overlapNode=%lu arrayToCSV=%lu\n",
	        g_qt_is_235 ? "2.35" : "2.21", g_stat_ctor, g_stat_add,
	        g_stat_addobj, g_stat_overlap, g_stat_overlapnode, g_stat_arraytocsv);
}

static int install_quadtree(Avm2Context* ctx, Avm2Class* cls, int is_235)
{
	(void) ctx;
	g_qt_is_235 = is_235;
	{
		const char* s = getenv("AVM2_FLIXEL_STATS");
		if (s != NULL && s[0] != '\0' && strcmp(s, "0") != 0)
			atexit(flixel_stats_atexit);
	}
	Avm2VTable* vt = &cls->ivtable;

	// Phase 1 — resolve every target. Any miss aborts with the vtable untouched.
	Avm2PropEntry* e_overlap = resolve_method(vt, "overlap", 7);
	Avm2PropEntry* e_add     = resolve_method(vt, "add", 3);
	if (e_overlap == NULL || e_add == NULL) return 0;

	// Phase 2 — commit. Past this point installation cannot fail.
	commit_method(e_overlap, qt_native_overlap, "FlxQuadTree/overlap$native");
	commit_method(e_add, qt_native_add, "FlxQuadTree/add$native");

	// FlxRect slots on the root object.
	g_qt_s_x = slot_of_public(vt, "x", 1);
	g_qt_s_y = slot_of_public(vt, "y", 1);
	g_qt_s_w = slot_of_public(vt, "width", 5);
	g_qt_s_h = slot_of_public(vt, "height", 6);

	// NOT native_construct — that short-circuits object allocation entirely.
	// native_ext_size + native_init run first, then instance_init.
	cls->native_ext_size = (uint32_t) sizeof(FlxQTExt);
	cls->native_init = qt_native_init;
	cls->instance_init.fn = qt_native_ctor;
	cls->instance_init.file = NULL;
	cls->instance_init.method_index = 0;
	cls->instance_init.debug_name = "FlxQuadTree/ctor$native";

	// The protected addObject/addToList/overlapNode entries are deliberately
	// left as their original AS3 bodies: they are only ever reached from this
	// class's own code, every internal caller is now native, and leaving them
	// intact keeps the AS3 fallback arm functional.

	g_o = NULL;
	g_oc = avm2_null();
	g_oa = FLXQT_A_LIST;
	g_ol = g_or = g_ot = g_ob = 0.0;
	memset(g_slot_cache, 0, sizeof g_slot_cache);
	g_qt_cls = cls;
	return 1;
}

// Register the stats dump once, no matter how many intrinsics install.
static void maybe_register_stats_atexit(void)
{
	static int registered = 0;
	if (registered) return;
	const char* s = getenv("AVM2_FLIXEL_STATS");
	if (s != NULL && s[0] != '\0' && strcmp(s, "0") != 0)
	{
		atexit(flixel_stats_atexit);
		registered = 1;
	}
}

// id 4 — substitute FlxTilemap's static arrayToCSV with the native O(n) CSV
// builder. Stateless: no native_ext, no GC hooks, no roots — just a vtable
// method swap on the class object. Independent of the FlxQuadTree/FlxList
// coupling (a title may match this and not the collision classes, or vice
// versa). Returns 1 on success, 0 (fall back to the game's AS3) on any miss.
static int install_tilemap_arraytocsv(Avm2Context* ctx, Avm2Class* cls)
{
	(void) ctx;
	if (g_tilemap_cls != NULL) return 0;         // already installed
	if (cls->class_object == NULL) return 0;
	Avm2VTable* cvt = (Avm2VTable*) cls->class_object->vtable;
	if (cvt == NULL) return 0;
	Avm2PropEntry* e = resolve_method(cvt, "arrayToCSV", 10);
	if (e == NULL) return 0;                      // not the shape we expect -> fall back
	commit_method(e, tilemap_native_arraytocsv, "FlxTilemap/arrayToCSV$native");
	maybe_register_stats_atexit();
	g_tilemap_cls = cls;
	return 1;
}

int avm2_flixel_try_install(Avm2Context* ctx, Avm2Class* cls, uint32_t intrinsic_id)
{
	if (intrinsic_id == 0 || cls == NULL) return 0;
#ifdef AVM2_FORCE_NO_INTRINSICS
	// Compile-time kill switch. wasm has no environment to getenv the runtime
	// AVM2_NO_INTRINSICS switch, so a browser-side intrinsic-vs-fallback A/B
	// builds the fallback arm from the SAME recompiled game code with this -D.
	// (Placed after the null check but above the install path so install_quadtree
	// and the native methods stay referenced — no unused-function warnings.)
	return 0;
#endif
	if (g_no_intrinsics < 0)
	{
		const char* e = getenv("AVM2_NO_INTRINSICS");
		g_no_intrinsics = (e != NULL && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
	}
	if (g_no_intrinsics) return 0;

	if (intrinsic_id == 2)
	{
		// FlxList: nothing to install — its instances are C structs. The id
		// only certifies the shape (2.21 and 2.35 FlxList are byte-identical
		// source, so one native struct serves both). If FlxQuadTree already
		// linked and was parked, install it now with its recorded variant.
		g_flxlist_verified = 1;
		if (g_qt_pending != NULL && g_qt_cls == NULL)
		{
			Avm2Class* pending = g_qt_pending;
			g_qt_pending = NULL;
			install_quadtree(ctx, pending, g_qt_pending_is_235);
		}
		return 0;
	}

	// id 1 = FlxQuadTree 2.21 (fixed MIN=48); id 3 = FlxQuadTree 2.35 (dynamic
	// _min). Both couple to FlxList (id 2): install is refused/parked until
	// FlxList is fingerprint-verified, so a native FlxList can never exist under
	// an AS3 quadtree and vice versa — the pairing is all-or-nothing.
	if (intrinsic_id == 1 || intrinsic_id == 3)
	{
		int is_235 = (intrinsic_id == 3);
		if (g_qt_cls != NULL) return 0;  // already installed over a class
		if (!g_flxlist_verified)
		{
			// FlxList not fingerprint-verified yet: refuse for now, park.
			g_qt_pending = cls;
			g_qt_pending_is_235 = is_235;
			return 0;
		}
		return install_quadtree(ctx, cls, is_235);
	}

	// id 4 = FlxTilemap.arrayToCSV. Stateless static-method swap; no coupling
	// to the collision classes.
	if (intrinsic_id == 4)
	{
		return install_tilemap_arraytocsv(ctx, cls);
	}
	return 0;
}
