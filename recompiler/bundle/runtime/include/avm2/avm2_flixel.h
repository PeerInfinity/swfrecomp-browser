#ifndef AVM2_FLIXEL_H
#define AVM2_FLIXEL_H

// Native C intrinsics for Flixel 2.21 collision internals
// (org.flixel::FlxQuadTree + org.flixel.data::FlxList).
//
// Motivation: profiling of Robot Wants Kitty attributes ~70.8% of frame time
// to the Flixel quadtree (see avm2-quadtree-is-71pct-pooling-dead). The tree
// is rebuilt from scratch on EVERY FlxU.overlap/FlxU.collide call, so the AS3
// bytecode arm pays interpreter dispatch + object allocation for hundreds of
// nodes per frame. This module replaces the whole data structure with plain C
// structs bump-allocated from a per-root arena.
//
// HARD CONTRACT: order-identical to the decompiled AS3 source. A TAS oracle
// diffs frames byte-for-byte, so every traversal order, short-circuit, static
// re-entrancy quirk and IEEE-double arithmetic step is ported literally. See
// the block comment in avm2_flixel.c for the AS3-line -> C-function map.
//
// Only the ROOT FlxQuadTree stays an Avm2Object (FlxU.quadTree is a typed
// static that holds it). Its native_ext carries the C tree + arena. Child
// nodes and every FlxList node are pure C. FlxObject stays fully AS3 and is
// read through cached vtable slot indices.
//
// Kill switches: AVM2_NO_INTRINSICS=1 in the environment skips installation
// entirely and the game's own compiled AS3 runs untouched. The compile-time
// twin -DAVM2_FORCE_NO_INTRINSICS=1 does the same at build time — needed for a
// browser-side intrinsic-vs-fallback A/B, since wasm has no environment to
// getenv; both arms then build from the SAME recompiled game code.

#include <avm2/avm2_class.h>
#include <avm2/avm2_object.h>

// Install the native intrinsic over `cls` if `intrinsic_id` matches a class
// this module implements. The id is a fingerprint-gated marker supplied by
// the recompiler:
//   0 = no match  -> installs nothing, returns 0
//   1 = org.flixel::FlxQuadTree 2.21 (fixed MIN=48)
//   2 = org.flixel.data::FlxList  2.21/2.35 (byte-identical source)
//   3 = org.flixel::FlxQuadTree 2.35 (dynamic _min = (w+h)/(2*quadTreeDivisions))
//   4 = org.flixel::FlxTilemap.arrayToCSV — native O(n) CSV builder replacing
//       the O(n^2) string-concat that spikes ~1.7 GB of single-tick transient
//       per level load (see SWFRecompDocs/plans/avm2-browser-footprint.md).
//       METHOD-level gate (only that one static method's body is fingerprinted,
//       decoupled from the rest of the large FlxTilemap class) and STATELESS —
//       no native_ext/GC/roots, independent of the FlxQuadTree/FlxList coupling.
//
// FlxList instances are replaced by C structs entirely, so id 2 installs
// nothing and returns 0 — it exists purely so the fingerprint can VERIFY
// FlxList is the expected shape before FlxQuadTree's intrinsic is trusted.
// (Upstream Flixel FlxList is byte-identical between 2.21 and 2.35, so one
// native struct serves both; the per-title fingerprints differ only by the
// obfuscator's control-flow transforms and are all mapped to id 2.)
// FlxQuadTree's install (id 1 or 3) is refused unless FlxList was also
// fingerprint-verified; the two classes may link in either order and both
// orderings are handled (a FlxQuadTree seen first is parked as pending and
// installed when FlxList arrives). This coupling makes native FlxList +
// native FlxQuadTree strictly all-or-nothing.
//
// Returns 1 if the native intrinsic was installed over THIS class, else 0.
int avm2_flixel_try_install(Avm2Context* ctx, Avm2Class* cls, uint32_t intrinsic_id);

// --- GC hooks (invoked from avm2_gc.c alongside the other module hooks) ---

// Precise ext tracer. The conservative blob scan does not follow into the
// arena chunks, so walk the whole C tree and mark every FlxQTList.object
// edge. No-op unless `o` is an instance of the installed FlxQuadTree class.
void avm2_flixel_gc_trace_ext(Avm2Object* o);

// Sweep hook: free the arena chunks owned by a collected root's ext.
// No-op unless `o` is an instance of the installed FlxQuadTree class.
void avm2_flixel_gc_free_ext(Avm2Context* ctx, Avm2Object* o);

// Root marker: the AS3 `protected static var _o:FlxObject` / `_oc:Function`
// become C file-scope globals here and hold object references across the tick
// boundary exactly as the AS3 statics did. Mark them so liveness matches the
// AS3 arm (a static holding an object keeps it alive in Flash too).
void avm2_gc_mark_roots_flixel(Avm2Context* ctx);

#endif // AVM2_FLIXEL_H
