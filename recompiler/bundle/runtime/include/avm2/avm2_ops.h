#ifndef AVM2_OPS_H
#define AVM2_OPS_H

// Opcode helpers called by the generated C in RecompiledABC/. One function
// per non-trivial IR op; trivial ops (GetLocal, Pop, Dup, PushInt, ...)
// are emitted inline. See SWFRecomp/src/abc/abc_emit.cpp for the emission
// patterns. Semantics ported from Ruffle core/src/avm2/activation.rs.
//
// Binary operators are helpers (not inline expressions) so operand
// coercion side effects (valueOf/toString) run in ECMA order.

#include <avm2/avm2_abc.h>
#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_value.h>

// Locals setup: loc[0] = this, loc[1..] = args coerced to the declared
// param types, missing trailing args filled from optional defaults, plus
// the `arguments`/rest array per method flags. Throws ArgumentError 1063
// on arg-count mismatch (Ruffle parameters.rs).
void avm2_setup_locals(Avm2Value* loc, uint32_t num_locals, Avm2Activation* act,
                       uint32_t method_index);

// ReturnValue coercion to the method's declared return type.
Avm2Value avm2_op_coerce_return(Avm2Activation* act, uint32_t method_index, Avm2Value v);

// PushScope/PushWith entry: null/undefined throws TypeError 1009/1010.
Avm2Object* avm2_op_pushscope(Avm2Activation* act, Avm2Value v);

// FindPropStrict/FindProperty: local scope top→bottom (with-scopes match
// any property, plain scopes declared traits only, the bottom global scope
// also matches dynamics), captured outer chain top→bottom, then the
// defining file's domain (lazy script init). strict: throw ReferenceError
// 1065 if unresolved; non-strict: return the global scope.
Avm2Object* avm2_op_findproperty(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                 uint32_t scope_n, uint32_t mn_idx, int strict);
Avm2Object* avm2_op_findproperty_dyn(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                     uint32_t scope_n, uint32_t mn_idx, Avm2Value name,
                                     int strict);
// FindDef: domain lookup only.
Avm2Object* avm2_op_finddef(Avm2Activation* act, uint32_t mn_idx);

// Per-call-site domain inline cache for static-multiname FindProperty /
// FindPropStrict (the getlex-global type-specialization lever). Caches only the
// DOMAIN resolution (stable per ctx: the domain is append-only with stable
// object identity). The generated C declares one (block-scoped, zero-init =
// empty) per find site and threads its address in. See avm2_op_findpropstrict_ic
// in avm2_ops.c for the full soundness argument.
typedef struct Avm2FindCache
{
	// The DOMAIN this entry was resolved against (loader-arc tranche 8). It
	// used to be the Avm2Context, back when there was exactly one domain per
	// context; a scope is strictly finer and costs the same single compare.
	// The same generated call site can now run under two Avm2AbcFileRt of the
	// same ABC in different domains (a self-load, or one child SWF loaded into
	// two domains), and those must not share a cached def object.
	const struct Avm2DomainScope* scope;
	Avm2Object* obj;    // cached domain-resolved def object (NULL = empty)
	// Scope-walk HIT cache (avm2_op_findpropstrict_ic): populated only when
	// the hit landed on the OUTER chain with a with-free checked prefix and
	// scope_n <= 1. Outer chains are allocated once and never freed, so the
	// chain-identity guard cannot alias; the same chain ⟹ the same entry
	// objects ⟹ the same trait-based misses above the hit.
	const Avm2ScopeChain* outer; // outer-chain identity guard
	const Avm2VTable* l_vt0;     // lscope[0] vtable guard (NULL ⟺ cached scope_n==0)
	Avm2Object* scope_obj;       // cached outer-chain hit object
	uint32_t l_n;                // scope_n at populate time (0 or 1)
	// scope_kind: 0 = no scope cache, 1 = OUTER-chain hit (replay returns
	// scope_obj), 2 = hit at lscope[0] with scope_n==1 (replay returns the
	// CURRENT activation's lscope[0].obj — the hit entry is the very first
	// thing the walk examines, so vtable identity alone determines the hit;
	// no prefix assumptions at all).
	uint32_t scope_kind;
} Avm2FindCache;

// Inline-cached FindProperty/FindPropStrict. Identical semantics to
// avm2_op_findproperty; `scope_stable` (recompiler proved the method with-free)
// additionally allows a cached domain hit to skip the scope walk.
Avm2Object* avm2_op_findpropstrict_ic(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                      uint32_t scope_n, uint32_t mn_idx, int strict,
                                      int scope_stable, Avm2FindCache* ic);
// GetGlobalScope: outer chain bottom, else local scope bottom.
Avm2Object* avm2_op_getglobalscope(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                   uint32_t scope_n);
Avm2Object* avm2_op_getouterscope(Avm2Activation* act, uint32_t index);

// Per-call-site monomorphic inline cache for GetPropertyStatic. The generated
// C declares one (block-scoped `static`) per getproperty call site and threads
// its address in; a repeat call whose receiver has the same vtable identity
// (and unchanged entry count) replays the resolved vtable entry directly,
// skipping multiname matching. Zero-initialized (vt == NULL) = empty; only
// plain-object receivers whose PRIMARY vtable find hit are ever cached, and a
// matching vt is byte-identical to the full resolve path (the vtable pointer
// alone determines not-null/not-xmlish/not-vector). See avm2_class.c's vtable
// name index for the miss-path accelerator this sits on top of.
typedef struct Avm2InlineCache
{
	const Avm2VTable* vt;    // receiver vtable this entry was resolved against
	uint32_t vt_count;       // vt->count at resolve time (guards realloc/growth)
	uint32_t entry_index;    // index into vt->entries (stable across realloc)
#ifndef AVM2_NO_IC_SLOT_INLINE
	// GetPropertyStatic only: the cached entry is a SLOT trait → its
	// slot_index + 1 (0 = not a slot, or the cache is empty). Written together
	// with vt/vt_count/entry_index, so the four always describe one resolve.
	uint32_t slot_plus1;
#endif
} Avm2InlineCache;

#ifdef AVM2_HOTLOOP_PROF
// Per-tick dump + reset of the hot-loop counters (avm2_ops.c).
void avm2_hotloop_prof_tick(Avm2Context* ctx);
#endif

// Property access. *_dyn variants take the lazy runtime name.
Avm2Value avm2_op_getproperty_static(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx);
// Inline-cached variant: identical semantics, threads a per-call-site cache.
#ifdef AVM2_NO_IC_SLOT_INLINE
Avm2Value avm2_op_getproperty_static_ic(Avm2Activation* act, Avm2Value recv,
                                        uint32_t mn_idx, Avm2InlineCache* ic);
#else
// Everything except the inline slot hit below: the out-of-line IC (its own
// vtable-replay hit for non-slot entries, then the full resolve + populate).
Avm2Value avm2_op_getproperty_static_ic_slow(Avm2Activation* act, Avm2Value recv,
                                             uint32_t mn_idx, Avm2InlineCache* ic);
#ifdef AVM2_IC_SLOT_VERIFY
void avm2_ic_slot_verify_fail(Avm2Activation* act, uint32_t mn_idx, Avm2Value fast,
                              Avm2Value slow);
#endif
// Seedling hot-loop lever (SWFRecompDocs/status/seedling-collide-hotloop.md):
// the out-of-line IC hit — two calls, a zeroed Resolved and resolved_get's
// branch chain before `slots[i]` — cost ~60 % of a FlashPunk collide pass, and
// 99.97 % of its 54 M gets per frame were hits on SLOT entries. Here, a hit
// on a cached SLOT entry is the bare load, inlined into the generated code.
// Equivalent to the out-of-line hit for all inputs: the receiver is exactly
// the one `unbox_scope_prim` leaves alone (an OBJECT that is not a primitive
// scope box), `vt` is computed as avm2_value_vtable does, and the out-of-line
// hit with a SLOT entry reads `recv.u.obj->slots[entry->slot_index]`. Build
// -DAVM2_IC_SLOT_VERIFY to run the out-of-line op on every inline hit and
// abort on any difference; -DAVM2_NO_IC_SLOT_INLINE removes the lever.
static inline Avm2Value avm2_op_getproperty_static_ic(Avm2Activation* act, Avm2Value recv,
                                                      uint32_t mn_idx, Avm2InlineCache* ic)
{
	if (recv.kind == AVM2_VALUE_OBJECT && ic->slot_plus1 != 0)
	{
		Avm2Object* o = recv.u.obj;
		const Avm2VTable* vt = o->vtable != NULL ? o->vtable
		                     : (o->cls != NULL ? &o->cls->ivtable : NULL);
		if (vt == ic->vt && vt->count == ic->vt_count && !o->is_prim_box)
		{
			Avm2Value v = o->slots[ic->slot_plus1 - 1];
#ifdef AVM2_IC_SLOT_VERIFY
			Avm2Value g = avm2_op_getproperty_static_ic_slow(act, recv, mn_idx, ic);
			if (v.kind != g.kind || memcmp(&v.u, &g.u, sizeof(v.u)) != 0)
			{
				avm2_ic_slot_verify_fail(act, mn_idx, v, g);
			}
#endif
			return v;
		}
	}
	return avm2_op_getproperty_static_ic_slow(act, recv, mn_idx, ic);
}
#endif
// Compile-time slot-bound read (recompiler's type-specialization pass): the
// recompiler proved the receiver is `this` — an instance of a sealed ABC class
// whose full superclass chain to Object is ABC-defined — and the accessed name
// resolves to a SLOT/CONST trait at compile-time slot index `slot` (numbered
// identically to avm2_class.c's ivtable assignment). `this` is never null in a
// verified instance method and slots aren't shadowable on a sealed class, so the
// read is a bare `slots[slot]` load — no null check, no vtable fetch, no
// multiname match, no resolved_get dispatch. `mn_idx` is carried only for the
// verify build. Build with -DAVM2_SLOT_VERIFY to cross-check `slot` against the
// full runtime resolve on every call and abort on any mismatch (validates the
// compile-time numbering); the default build is the pure inline load below.
#ifdef AVM2_SLOT_VERIFY
Avm2Value avm2_op_getproperty_slot(Avm2Activation* act, Avm2Value recv,
                                   uint32_t slot, uint32_t mn_idx);
#else
static inline Avm2Value avm2_op_getproperty_slot(Avm2Activation* act, Avm2Value recv,
                                                 uint32_t slot, uint32_t mn_idx)
{
	(void) act; (void) mn_idx;
	return recv.u.obj->slots[slot];
}
#endif
// Compile-time-resolved FindProperty/FindPropStrict (recompiler find→this
// lever): the recompiler proved the scope walk at this site always hits
// `this` — the enclosing instance method has the canonical GetLocal0+PushScope
// preamble as its only scope ops (no with, no activation, no active
// exceptions, no branch into the preamble), local 0 is never rewritten, and
// the multiname statically matches a declared instance trait of the enclosing
// class (ns-set aware, mirroring avm2_mn_match). Every runtime receiver (the
// class or any subclass) declares that trait, and plain-scope probes match
// declared traits only (scope_defines_mn), so the walk's FIRST probe
// (lscope[0] == this) always hits. The op is an identity move of loc[0];
// build -DAVM2_FIND_VERIFY to cross-check against the full resolve.
#ifdef AVM2_FIND_VERIFY
Avm2Value avm2_op_findprop_this(Avm2Activation* act, Avm2Value thisv,
                                const Avm2ScopeEntry* lscope, uint32_t scope_n,
                                uint32_t mn_idx);
#else
static inline Avm2Value avm2_op_findprop_this(Avm2Activation* act, Avm2Value thisv,
                                              const Avm2ScopeEntry* lscope,
                                              uint32_t scope_n, uint32_t mn_idx)
{
	(void) act; (void) lscope; (void) scope_n; (void) mn_idx;
	return thisv;
}
#endif
// Per-call-site cache for the recompiler's find→own-class-static lever: the
// site's multiname statically resolves to a declared STATIC trait of the
// ENCLOSING class, the method has the canonical GetLocal0+PushScope preamble
// as its only scope ops (same gate as find→this), and the enclosing
// instance chain does NOT declare the name — so the scope walk provably
// probes `this` (miss) and then the class object at the top of the outer
// chain (hit). Soundness is RUNTIME-guarded, not assumed: the cache is
// populated only after a full walk whose result equals the enclosing class's
// class object (act->file->classes[class_index]->class_object), and replays
// only under context + outer-chain identity + lscope[0] (this) vtable
// identity — the exact facts that walk depended on (plain scopes match
// declared traits only, so a fixed this-vtable re-misses and the fixed outer
// chain re-hits at the same entry). Anything else (cross-file subclass
// shadowing, native-ancestor traits invisible to the recompiler) falls back
// to the full walk with exact semantics. Zero-init = empty.
typedef struct Avm2StaticFindCache
{
	Avm2Context* ctx;            // context identity (restart guard)
	const Avm2ScopeChain* outer; // outer-chain identity guard
	const Avm2VTable* l_vt0;     // lscope[0] (`this`) vtable guard
	Avm2Object* cls_obj;         // proven hit: the enclosing class's class object
} Avm2StaticFindCache;

// Fused FindPropStrict+GetPropertyStatic of the same multiname (the getlex-
// ownstatic read pattern old ASC emits for unqualified own-class-static
// reads, e.g. FlxQuadTree._min): on a cache hit returns the static SLOT
// directly (slot index compile-time mirrored from the runtime cvt numbering,
// computeStaticSlotIndex; -DAVM2_SLOT_VERIFY cross-checks). On miss: full
// findpropstrict walk; if it lands on the expected class object, populate +
// slot read, else a fully generic getproperty on whatever it returned.
Avm2Value avm2_getlex_ownstatic_slow(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                     uint32_t scope_n, uint32_t mn_idx,
                                     uint32_t class_index, uint32_t slot,
                                     Avm2StaticFindCache* c);
#if defined(AVM2_FIND_VERIFY) || defined(AVM2_SLOT_VERIFY)
Avm2Value avm2_op_getlex_ownstatic(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                   uint32_t scope_n, uint32_t mn_idx,
                                   uint32_t class_index, uint32_t slot,
                                   Avm2StaticFindCache* c);
#else
static inline Avm2Value avm2_op_getlex_ownstatic(Avm2Activation* act,
                                                 const Avm2ScopeEntry* lscope,
                                                 uint32_t scope_n, uint32_t mn_idx,
                                                 uint32_t class_index, uint32_t slot,
                                                 Avm2StaticFindCache* c)
{
	if (c->ctx == act->ctx && act->outer == c->outer && scope_n == 1
	    && !lscope[0].is_with && lscope[0].obj != NULL
	    && lscope[0].obj->vtable == c->l_vt0)
	{
		return c->cls_obj->slots[slot];
	}
	return avm2_getlex_ownstatic_slow(act, lscope, scope_n, mn_idx, class_index,
	                                  slot, c);
}
#endif

// Standalone guarded find for own-class-static sites whose consumer is NOT
// an adjacent same-mn read (e.g. static stores: findprop; <value>; setprop).
// Identical semantics to FindPropStrict — the fallback returns whatever the
// full walk resolves — so the consumer op stays fully generic.
Avm2Object* avm2_findprop_ownstatic_slow(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                         uint32_t scope_n, uint32_t mn_idx,
                                         uint32_t class_index, Avm2StaticFindCache* c);
#ifdef AVM2_FIND_VERIFY
Avm2Object* avm2_op_findprop_ownstatic(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                       uint32_t scope_n, uint32_t mn_idx,
                                       uint32_t class_index, Avm2StaticFindCache* c);
#else
static inline Avm2Object* avm2_op_findprop_ownstatic(Avm2Activation* act,
                                                     const Avm2ScopeEntry* lscope,
                                                     uint32_t scope_n, uint32_t mn_idx,
                                                     uint32_t class_index,
                                                     Avm2StaticFindCache* c)
{
	if (c->ctx == act->ctx && act->outer == c->outer && scope_n == 1
	    && !lscope[0].is_with && lscope[0].obj != NULL
	    && lscope[0].obj->vtable == c->l_vt0)
	{
		return c->cls_obj;
	}
	return avm2_findprop_ownstatic_slow(act, lscope, scope_n, mn_idx, class_index, c);
}
#endif

// `interp`: body is a class/script initializer — avmplus's interpreter
// takes the index fast path regardless of the ns set
// (class_init_interpreter_mode / array_access_interpreter).
Avm2Value avm2_op_getproperty_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                  Avm2Value name, int interp);
void avm2_op_setproperty_static(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                Avm2Value value);
// Inline-cached SetPropertyStatic: identical semantics, threads a per-call-site
// cache. Same monomorphic-vtable design as getproperty (only plain-object
// receivers whose primary vtable find hits are cached — the resolved slot/setter
// entry is stable, so a matching vt replays it byte-identically).
void avm2_op_setproperty_static_ic(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                   Avm2Value value, Avm2InlineCache* ic);
void avm2_op_setproperty_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                             Avm2Value name, Avm2Value value, int interp);
void avm2_op_initproperty(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx, Avm2Value val);
// InitProperty with a runtime name (MultinameL): same resolution as
// avm2_op_setproperty_dyn, but an init may write a const slot.
void avm2_op_initproperty_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                              Avm2Value name, Avm2Value value, int interp);

// Compile-time slot-bound STORE (recompiler store-path specialization): the
// recompiler proved the receiver's static class is a sealed ABC class whose
// chain to Object is ABC-defined, the name resolves uniquely (ns-aware,
// mirroring avm2_mn_match) to a SLOT trait at compile-time index `slot` (no
// subclass redeclares it; const slots only via the initproperty form), so
// the write is `slots[slot] = coerce(value)` with no resolve. Two forms:
// the bare op is emitted when the operand's static type provably satisfies
// the declared slot type (the store coerce is a value no-op — Step-4 elide
// rules extended with null literals); the `_c` op keeps the declared-type
// coerce with a compile-time type multiname. A non-object receiver (null/
// undefined — a typed local can be null) routes through the full generic
// path for the exact throw. `is_init` selects the InitProperty fallback
// (const stores legal). Build -DAVM2_SET_VERIFY to resolve + cross-check
// the target slot and the coerced value BEFORE the store on every call.
void avm2_setproperty_slot_fallback(Avm2Activation* act, Avm2Value recv,
                                    uint32_t mn_idx, Avm2Value value, int is_init);
#ifdef AVM2_SET_VERIFY
void avm2_op_setproperty_slot(Avm2Activation* act, Avm2Value recv, uint32_t slot,
                              uint32_t mn_idx, Avm2Value value, int is_init);
void avm2_op_setproperty_slot_c(Avm2Activation* act, Avm2Value recv, uint32_t slot,
                                uint32_t mn_idx, uint32_t type_mn, Avm2Value value,
                                int is_init);
#else
static inline void avm2_op_setproperty_slot(Avm2Activation* act, Avm2Value recv,
                                            uint32_t slot, uint32_t mn_idx,
                                            Avm2Value value, int is_init)
{
	if (recv.kind == AVM2_VALUE_OBJECT && recv.u.obj != NULL)
	{
		recv.u.obj->slots[slot] = value;
		return;
	}
	avm2_setproperty_slot_fallback(act, recv, mn_idx, value, is_init);
}
static inline void avm2_op_setproperty_slot_c(Avm2Activation* act, Avm2Value recv,
                                              uint32_t slot, uint32_t mn_idx,
                                              uint32_t type_mn, Avm2Value value,
                                              int is_init)
{
	if (recv.kind == AVM2_VALUE_OBJECT && recv.u.obj != NULL)
	{
		recv.u.obj->slots[slot] =
			avm2_coerce_to_type_mn(act->ctx, act->file, type_mn, value);
		return;
	}
	avm2_setproperty_slot_fallback(act, recv, mn_idx, value, is_init);
}
#endif
Avm2Value avm2_op_deleteproperty(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx);
Avm2Value avm2_op_deleteproperty_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                     Avm2Value name);

// Lazy-namespace (RTQName/RTQNameL) property ops: `ns_val` is the popped
// Namespace VALUE; the *_l variants also pop the runtime name (a QName
// object name overrides the popped namespace, per Ruffle
// fill_with_runtime_params).
Avm2Value avm2_op_pushnamespace(Avm2Activation* act, uint32_t ns_idx);
Avm2Value avm2_op_getproperty_rtns(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                   Avm2Value ns_val);
Avm2Value avm2_op_getproperty_rtns_l(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                     Avm2Value ns_val, Avm2Value name_val);
void avm2_op_setproperty_rtns(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                              Avm2Value ns_val, Avm2Value value);
void avm2_op_setproperty_rtns_l(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                Avm2Value ns_val, Avm2Value name_val, Avm2Value value);
Avm2Value avm2_op_deleteproperty_rtns(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                      Avm2Value ns_val);
Avm2Value avm2_op_deleteproperty_rtns_l(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                        Avm2Value ns_val, Avm2Value name_val);
Avm2Value avm2_op_callproperty_rtns(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                    Avm2Value ns_val, const Avm2Value* args, uint32_t argc);
Avm2Value avm2_op_callproperty_rtns_l(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                      Avm2Value ns_val, Avm2Value name_val,
                                      const Avm2Value* args, uint32_t argc);
Avm2Object* avm2_op_findproperty_rtns(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                      uint32_t scope_n, uint32_t mn_idx, Avm2Value ns_val,
                                      int strict);
Avm2Object* avm2_op_findproperty_rtns_l(Avm2Activation* act, const Avm2ScopeEntry* lscope,
                                        uint32_t scope_n, uint32_t mn_idx, Avm2Value ns_val,
                                        Avm2Value name_val, int strict);

// Slots (IR indices are 0-based; runtime slot arrays are 1-based).
Avm2Value avm2_op_getslot(Avm2Activation* act, Avm2Value obj, uint32_t index0);
void avm2_op_setslot(Avm2Activation* act, Avm2Value obj, uint32_t index0, Avm2Value value);

// Calls. `opts` bit 1 = lex (receiver is null for the callee).
Avm2Value avm2_op_callproperty(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                               const Avm2Value* args, uint32_t argc);
// Inline-cached CallProperty/CallPropVoid (static multiname): identical
// semantics, threads a per-call-site cache. A repeat call whose receiver has the
// same vtable identity replays the resolved method/property entry, skipping the
// multiname match. Only plain-object receivers whose primary vtable find hit are
// cached (xmlish + no_index excluded), so a matching vt is byte-identical.
Avm2Value avm2_op_callproperty_ic(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                  const Avm2Value* args, uint32_t argc, Avm2InlineCache* ic);
Avm2Value avm2_op_callproperty_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                   Avm2Value name, const Avm2Value* args, uint32_t argc);
Avm2Value avm2_op_callproplex(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                              const Avm2Value* args, uint32_t argc);
Avm2Value avm2_op_call(Avm2Activation* act, Avm2Value func, Avm2Value recv,
                       const Avm2Value* args, uint32_t argc);
Avm2Value avm2_op_callstatic(Avm2Activation* act, uint32_t method_index, Avm2Value recv,
                             const Avm2Value* args, uint32_t argc);
Avm2Value avm2_op_callsuper(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                            const Avm2Value* args, uint32_t argc);
Avm2Value avm2_op_getsuper(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx);
void avm2_op_setsuper(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx, Avm2Value value);
// `super[expr]` — runtime local name, static ns set.
Avm2Value avm2_op_getsuper_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                               Avm2Value name);
void avm2_op_setsuper_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                          Avm2Value name, Avm2Value value);

// Construction.
Avm2Value avm2_op_construct(Avm2Activation* act, Avm2Value ctor,
                            const Avm2Value* args, uint32_t argc);
Avm2Value avm2_op_constructprop(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                const Avm2Value* args, uint32_t argc);
Avm2Value avm2_op_constructprop_init(Avm2Activation* act, Avm2Value recv,
                                     uint32_t mn_idx, const Avm2Value* args,
                                     uint32_t argc);
Avm2Value avm2_op_constructprop_dyn(Avm2Activation* act, Avm2Value recv, uint32_t mn_idx,
                                    Avm2Value name, const Avm2Value* args, uint32_t argc);
void avm2_op_constructsuper(Avm2Activation* act, Avm2Value recv,
                            const Avm2Value* args, uint32_t argc);

// ApplyType (Vector.<T>): pop type args, produce the parameterized class.
Avm2Value avm2_op_applytype(Avm2Activation* act, Avm2Value base,
                            const Avm2Value* params, uint32_t num_params);

// Object model.
Avm2Value avm2_op_newclass(Avm2Activation* act, uint32_t class_idx, Avm2Value base,
                           const Avm2ScopeEntry* lscope, uint32_t scope_n);
Avm2Value avm2_op_newfunction(Avm2Activation* act, uint32_t method_index,
                              const Avm2ScopeEntry* lscope, uint32_t scope_n);
Avm2Value avm2_op_newactivation(Avm2Activation* act, uint32_t method_index);
Avm2Value avm2_op_newcatch(Avm2Activation* act, uint32_t method_index, uint32_t exc_index);
Avm2Value avm2_op_newobject(Avm2Activation* act, const Avm2Value* name_value_pairs,
                            uint32_t num_pairs);
Avm2Value avm2_op_newarray(Avm2Activation* act, const Avm2Value* values, uint32_t n);

// Operators (value pairs; ECMA evaluation order preserved by the caller
// having already materialized both operands).
Avm2Value avm2_op_add(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_subtract(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_multiply(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_divide(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_modulo(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_negate(Avm2Activation* act, Avm2Value a);
Avm2Value avm2_op_increment(Avm2Activation* act, Avm2Value a);
Avm2Value avm2_op_decrement(Avm2Activation* act, Avm2Value a);
Avm2Value avm2_op_add_i(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_subtract_i(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_multiply_i(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_negate_i(Avm2Activation* act, Avm2Value a);
Avm2Value avm2_op_increment_i(Avm2Activation* act, Avm2Value a);
Avm2Value avm2_op_decrement_i(Avm2Activation* act, Avm2Value a);
Avm2Value avm2_op_bitand(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_bitor(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_bitxor(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_bitnot(Avm2Activation* act, Avm2Value a);
Avm2Value avm2_op_lshift(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_rshift(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_urshift(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_equals(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_strictequals(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_lessthan(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_lessequals(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_greaterthan(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_greaterequals(Avm2Activation* act, Avm2Value a, Avm2Value b);
Avm2Value avm2_op_not(Avm2Activation* act, Avm2Value a);
Avm2Value avm2_op_typeof(Avm2Activation* act, Avm2Value a);
Avm2Value avm2_op_in(Avm2Activation* act, Avm2Value name, Avm2Value obj);
Avm2Value avm2_op_instanceof(Avm2Activation* act, Avm2Value value, Avm2Value type);
Avm2Value avm2_op_istype(Avm2Activation* act, Avm2Value value, uint32_t mn_idx);
Avm2Value avm2_op_astype(Avm2Activation* act, Avm2Value value, uint32_t mn_idx);
Avm2Value avm2_op_istypelate(Avm2Activation* act, Avm2Value value, Avm2Value type);
Avm2Value avm2_op_astypelate(Avm2Activation* act, Avm2Value value, Avm2Value type);

// -------------------------------------------------------------------------
// Typed-value fast paths (inlined into the generated ABC TUs).
//
// Every helper here is EXACTLY equivalent to the out-of-line op it shortcuts
// FOR ALL INPUTS — the fast arm only decides the kinds it can decide locally,
// and every other kind falls through to the same generic implementation. They
// are therefore NOT gated on the compile-time type lattice and need no
// soundness argument beyond the per-helper equivalence noted below; the
// recompiler's static types only decide where emitting them is worthwhile.
//
// The `_test` variants return a plain int instead of a boxed Avm2Value: the
// ABC verifier always splits compare-and-branch bytecodes (iflt/ifnlt/...)
// into a compare op followed immediately by IfTrue/IfFalse, so the recompiler
// fuses that pair into one `if (avm2_op_*_test(...)) goto`, removing both the
// avm2_bool() boxing and the avm2_coerce_to_boolean() call on the hottest
// shape in the codebase.
//
// Build -DAVM2_ARITH_VERIFY to cross-check every fast arm against the generic
// op on every execution (the FIND/SLOT/SET/COERCE precedent).
#ifdef AVM2_ARITH_VERIFY
// Aborts with the op name; defined in avm2_ops.c.
void avm2_arith_verify_fail(const char* what);
// Bit-exact "same value" for the verify compare: NaN counts as equal to NaN
// (both paths compute the same NaN result) and -0 is distinguished from +0 so
// a sign-losing specialization cannot slip through.
static inline int avm2_arith_same(Avm2Value x, Avm2Value y)
{
	if (x.kind != y.kind) return 0;
	if (x.kind == AVM2_VALUE_NUMBER)
	{
		if (x.u.d != x.u.d && y.u.d != y.u.d) return 1;      // NaN vs NaN
		if (x.u.d == 0.0 && y.u.d == 0.0)                    // ±0 must match
			return (1.0 / x.u.d) == (1.0 / y.u.d);
		return x.u.d == y.u.d;
	}
	if (x.kind == AVM2_VALUE_INTEGER) return x.u.i == y.u.i;
	if (x.kind == AVM2_VALUE_BOOL) return x.u.b == y.u.b;
	return 1;
}
#define AVM2_ARITH_CHECK_INT(what, fast, generic) \
	do { if ((int) (fast) != (int) (generic)) avm2_arith_verify_fail(what); } while (0)
#define AVM2_ARITH_CHECK_VAL(what, fast, generic) \
	do { if (!avm2_arith_same((fast), (generic))) avm2_arith_verify_fail(what); } while (0)
#else
#define AVM2_ARITH_CHECK_INT(what, fast, generic) ((void) 0)
#define AVM2_ARITH_CHECK_VAL(what, fast, generic) ((void) 0)
#endif

// Boolean test. Equivalent: avm2_coerce_to_boolean's BOOL arm returns v.u.b.
static inline int avm2_to_boolean_fast(Avm2Value v)
{
	return v.kind == AVM2_VALUE_BOOL ? (int) v.u.b : (int) avm2_coerce_to_boolean(v);
}

// Numeric widening. Equivalent: avm2_coerce_to_number's INTEGER/NUMBER arms
// are exactly these widenings; every other kind (including OBJECT, whose
// valueOf may run user code or throw) goes to the generic function.
static inline double avm2_to_number_fast(Avm2Context* ctx, Avm2Value v)
{
	if (v.kind == AVM2_VALUE_NUMBER) return v.u.d;
	if (v.kind == AVM2_VALUE_INTEGER) return (double) v.u.i;
	return avm2_coerce_to_number(ctx, v);
}

// Both-numeric predicate: the kinds for which the arithmetic/comparison fast
// arms below are decidable without any coercion.
static inline int avm2_both_numeric(Avm2Value a, Avm2Value b)
{
	return (a.kind == AVM2_VALUE_INTEGER || a.kind == AVM2_VALUE_NUMBER)
	    && (b.kind == AVM2_VALUE_INTEGER || b.kind == AVM2_VALUE_NUMBER);
}

// Comparison tests. NaN semantics are load-bearing and match the generic ops
// exactly (avm2_abstract_lt returns -1 for NaN, so lessthan/greaterthan are
// false and lessequals/greaterequals — which test `r == 0` — are ALSO false):
// every C operator below is likewise false when either operand is NaN. Note
// the generic lessequals/greaterequals swap their operands; the direct forms
// here are the algebraic equivalents on non-NaN doubles.
#define AVM2_DEFINE_CMP_TEST(name, cop)                                       \
	static inline int avm2_op_##name##_test(Avm2Activation* act,              \
	                                        Avm2Value a, Avm2Value b)         \
	{                                                                         \
		if (avm2_both_numeric(a, b))                                          \
		{                                                                     \
			double x = a.kind == AVM2_VALUE_INTEGER ? (double) a.u.i : a.u.d;  \
			double y = b.kind == AVM2_VALUE_INTEGER ? (double) b.u.i : b.u.d;  \
			int r = x cop y;                                                  \
			AVM2_ARITH_CHECK_INT(#name, r,                                    \
				avm2_coerce_to_boolean(avm2_op_##name(act, a, b)));           \
			return r;                                                         \
		}                                                                     \
		return avm2_coerce_to_boolean(avm2_op_##name(act, a, b));             \
	}
AVM2_DEFINE_CMP_TEST(lessthan, <)
AVM2_DEFINE_CMP_TEST(lessequals, <=)
AVM2_DEFINE_CMP_TEST(greaterthan, >)
AVM2_DEFINE_CMP_TEST(greaterequals, >=)
#undef AVM2_DEFINE_CMP_TEST

// equals: the generic abstract_eq's both-numeric arm is `to_number(a) ==
// to_number(b)`, i.e. exactly this (-0 == +0 and NaN != NaN both fall out of
// the IEEE compare, as they do generically).
static inline int avm2_op_equals_test(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	if (avm2_both_numeric(a, b))
	{
		double x = a.kind == AVM2_VALUE_INTEGER ? (double) a.u.i : a.u.d;
		double y = b.kind == AVM2_VALUE_INTEGER ? (double) b.u.i : b.u.d;
		int r = x == y;
		AVM2_ARITH_CHECK_INT("equals", r,
			avm2_coerce_to_boolean(avm2_op_equals(act, a, b)));
		return r;
	}
	return avm2_coerce_to_boolean(avm2_op_equals(act, a, b));
}

// strictequals: avm2_strict_eq's INTEGER/NUMBER arms already promote across
// the two numeric kinds, so the both-numeric case is the same IEEE compare.
static inline int avm2_op_strictequals_test(Avm2Activation* act, Avm2Value a, Avm2Value b)
{
	if (avm2_both_numeric(a, b))
	{
		double x = a.kind == AVM2_VALUE_INTEGER ? (double) a.u.i : a.u.d;
		double y = b.kind == AVM2_VALUE_INTEGER ? (double) b.u.i : b.u.d;
		int r = x == y;
		AVM2_ARITH_CHECK_INT("strictequals", r, avm2_strict_eq(a, b));
		return r;
	}
	(void) act;
	return (int) avm2_strict_eq(a, b);
}

// Arithmetic. subtract/multiply/divide/modulo/increment/decrement/negate are
// to_number-of-both by spec (never string-concatenating, never dispatching),
// so inlining the numeric widening is equivalent for all inputs. `add` is NOT
// here: it may concatenate or call valueOf, so it keeps its generic op.
#define AVM2_DEFINE_ARITH(name, aop)                                          \
	static inline Avm2Value avm2_op_##name##_fast(Avm2Activation* act,        \
	                                              Avm2Value a, Avm2Value b)   \
	{                                                                         \
		if (avm2_both_numeric(a, b))                                          \
		{                                                                     \
			double x = a.kind == AVM2_VALUE_INTEGER ? (double) a.u.i : a.u.d;  \
			double y = b.kind == AVM2_VALUE_INTEGER ? (double) b.u.i : b.u.d;  \
			Avm2Value r = avm2_number(x aop y);                               \
			AVM2_ARITH_CHECK_VAL(#name, r, avm2_op_##name(act, a, b));        \
			return r;                                                         \
		}                                                                     \
		return avm2_op_##name(act, a, b);                                     \
	}
AVM2_DEFINE_ARITH(subtract, -)
AVM2_DEFINE_ARITH(multiply, *)
AVM2_DEFINE_ARITH(divide, /)
#undef AVM2_DEFINE_ARITH

#define AVM2_DEFINE_UNARY_ARITH(name, aop)                                    \
	static inline Avm2Value avm2_op_##name##_fast(Avm2Activation* act,        \
	                                              Avm2Value a)                \
	{                                                                         \
		if (a.kind == AVM2_VALUE_NUMBER || a.kind == AVM2_VALUE_INTEGER)      \
		{                                                                     \
			double x = a.kind == AVM2_VALUE_INTEGER ? (double) a.u.i : a.u.d;  \
			Avm2Value r = avm2_number(x aop 1.0);                             \
			AVM2_ARITH_CHECK_VAL(#name, r, avm2_op_##name(act, a));           \
			return r;                                                         \
		}                                                                     \
		return avm2_op_##name(act, a);                                        \
	}
AVM2_DEFINE_UNARY_ARITH(increment, +)
AVM2_DEFINE_UNARY_ARITH(decrement, -)
#undef AVM2_DEFINE_UNARY_ARITH

// Boolean negation (`not`): coerce_to_boolean of the operand, inverted.
static inline Avm2Value avm2_op_not_fast(Avm2Activation* act, Avm2Value a)
{
	if (a.kind == AVM2_VALUE_BOOL)
	{
		Avm2Value r = avm2_bool(!a.u.b);
		AVM2_ARITH_CHECK_VAL("not", r, avm2_op_not(act, a));
		return r;
	}
	return avm2_op_not(act, a);
}

// Coercions.
Avm2Value avm2_op_coerce(Avm2Activation* act, Avm2Value v, uint32_t mn_idx);
Avm2Value avm2_op_coerce_s(Avm2Activation* act, Avm2Value v);   // null/undef → null
Avm2Value avm2_op_convert_s(Avm2Activation* act, Avm2Value v);  // always string
Avm2Value avm2_op_coerce_o(Avm2Activation* act, Avm2Value v);   // undef → null
Avm2Value avm2_op_convert_o(Avm2Activation* act, Avm2Value v);  // null check

// -------------------------------------------------------------------------
// Coerce-elision verify hooks (Step 4, compile-time type specialization).
// The recompiler proves, via forward abstract-interpretation over the ABC,
// that a coerce_* site's operand already has the target static type, so the
// coercion is a value no-op and can be dropped. At every such elided site the
// emitter emits one of these hooks in place of the real coerce:
//   * normal build   — a `static inline` identity the optimizer removes,
//                       so the coercion truly disappears (the perf win).
//   * -DAVM2_COERCE_VERIFY — a real function that runs the ACTUAL coercion and
//                       aborts if it would have changed the value, then returns
//                       the ORIGINAL value (so output stays identical to the
//                       elided build). This proves every elision is sound
//                       across the whole test suite, exactly like
//                       -DAVM2_SLOT_VERIFY does for slot specialization.
#ifdef AVM2_COERCE_VERIFY
Avm2Value avm2_coerce_verify_return(Avm2Activation* act, uint32_t method_index, Avm2Value v);
Avm2Value avm2_coerce_verify_mn(Avm2Activation* act, Avm2Value v, uint32_t mn_idx);
Avm2Value avm2_coerce_verify_d(Avm2Activation* act, Avm2Value v);
Avm2Value avm2_coerce_verify_i(Avm2Activation* act, Avm2Value v);
Avm2Value avm2_coerce_verify_u(Avm2Activation* act, Avm2Value v);
Avm2Value avm2_coerce_verify_b(Avm2Activation* act, Avm2Value v);
Avm2Value avm2_coerce_verify_s(Avm2Activation* act, Avm2Value v);
#else
static inline Avm2Value avm2_coerce_verify_return(Avm2Activation* act, uint32_t method_index, Avm2Value v)
{ (void) act; (void) method_index; return v; }
static inline Avm2Value avm2_coerce_verify_mn(Avm2Activation* act, Avm2Value v, uint32_t mn_idx)
{ (void) act; (void) mn_idx; return v; }
static inline Avm2Value avm2_coerce_verify_d(Avm2Activation* act, Avm2Value v) { (void) act; return v; }
static inline Avm2Value avm2_coerce_verify_i(Avm2Activation* act, Avm2Value v) { (void) act; return v; }
static inline Avm2Value avm2_coerce_verify_u(Avm2Activation* act, Avm2Value v) { (void) act; return v; }
static inline Avm2Value avm2_coerce_verify_b(Avm2Activation* act, Avm2Value v) { (void) act; return v; }
static inline Avm2Value avm2_coerce_verify_s(Avm2Activation* act, Avm2Value v) { (void) act; return v; }
#endif
// E4X ops (GetDescendants / CheckFilter / Dxns / DxnsLate).
Avm2Value avm2_op_getdescendants(Avm2Activation* act, Avm2Value v, uint32_t mn_idx);
Avm2Value avm2_op_getdescendants_dyn(Avm2Activation* act, Avm2Value v, uint32_t mn_idx,
                                     Avm2Value name_val);
Avm2Value avm2_op_getdescendants_rtns(Avm2Activation* act, Avm2Value v, uint32_t mn_idx,
                                       Avm2Value ns_val);
Avm2Value avm2_op_getdescendants_rtns_l(Avm2Activation* act, Avm2Value v, uint32_t mn_idx,
                                        Avm2Value ns_val, Avm2Value name_val);
Avm2Value avm2_op_checkfilter(Avm2Activation* act, Avm2Value v);
void avm2_op_dxns(Avm2Activation* act, uint32_t str_idx);
void avm2_op_dxnslate(Avm2Activation* act, Avm2Value v);
Avm2Value avm2_op_esc_xattr(Avm2Activation* act, Avm2Value v);  // XML attr escape
Avm2Value avm2_op_esc_xelem(Avm2Activation* act, Avm2Value v);  // XML elem escape

// Alchemy/CrossBridge memory opcodes (avm2_mops.c). Loads/stores go
// through ApplicationDomain.domainMemory (or the 1024-byte scratch when
// unset); an out-of-range address throws RangeError 1506. Stores take the
// operands in push order: value first, address second.
Avm2Value avm2_op_li8(Avm2Activation* act, Avm2Value addr);
Avm2Value avm2_op_li16(Avm2Activation* act, Avm2Value addr);
Avm2Value avm2_op_li32(Avm2Activation* act, Avm2Value addr);
Avm2Value avm2_op_lf32(Avm2Activation* act, Avm2Value addr);
Avm2Value avm2_op_lf64(Avm2Activation* act, Avm2Value addr);
void avm2_op_si8(Avm2Activation* act, Avm2Value value, Avm2Value addr);
void avm2_op_si16(Avm2Activation* act, Avm2Value value, Avm2Value addr);
void avm2_op_si32(Avm2Activation* act, Avm2Value value, Avm2Value addr);
void avm2_op_sf32(Avm2Activation* act, Avm2Value value, Avm2Value addr);
void avm2_op_sf64(Avm2Activation* act, Avm2Value value, Avm2Value addr);
Avm2Value avm2_op_sxi1(Avm2Activation* act, Avm2Value v);
Avm2Value avm2_op_sxi8(Avm2Activation* act, Avm2Value v);
Avm2Value avm2_op_sxi16(Avm2Activation* act, Avm2Value v);

// Enumeration.
Avm2Value avm2_op_hasnext2(Avm2Activation* act, Avm2Value* obj_reg, Avm2Value* idx_reg);
Avm2Value avm2_op_hasnext(Avm2Activation* act, Avm2Value obj, Avm2Value idx);
Avm2Value avm2_op_nextname(Avm2Activation* act, Avm2Value obj, Avm2Value idx);
Avm2Value avm2_op_nextvalue(Avm2Activation* act, Avm2Value obj, Avm2Value idx);

// Throw.
_Noreturn void avm2_op_throw(Avm2Activation* act, Avm2Value v);

// Honest failure paths: print a clear diagnostic to stderr and exit
// non-zero so unsupported avm2 tests fail as runtime_error, not silently.
_Noreturn void avm2_unimplemented_op(Avm2Activation* act, const char* op_name, uint32_t op_index);
// VerifyError bodies throw a catchable VerifyError (verify_* tests).
_Noreturn void avm2_verify_error_body(Avm2Activation* act, const char* message);
_Noreturn void avm2_fatal(const char* fmt, ...);

#endif // AVM2_OPS_H
