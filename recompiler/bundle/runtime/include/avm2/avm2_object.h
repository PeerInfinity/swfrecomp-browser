#ifndef AVM2_OBJECT_H
#define AVM2_OBJECT_H

// ScriptObject per Ruffle script_object.rs:61-79 (avm2-support-plan §4.2):
// fixed slot array for declared traits + dynamic property map for expando
// props + proto link + class/vtable pointers. Kept in its own small header
// (exposed architecture surface — see the plan's LittleCube note).
//
// NOT an ASObject/ASFunction — the AVM2 runtime never touches AVM1's
// ActionVar object model.

#include <avm2/avm2_abc.h>
#include <avm2/avm2_value.h>

typedef struct Avm2VTable Avm2VTable;
typedef struct Avm2Context Avm2Context;

typedef enum Avm2ObjectKind
{
	AVM2_OBJ_SCRIPT = 0,   // plain ScriptObject / class instance / script globals
	AVM2_OBJ_CLASS = 1,    // class object (holds class_ref + static traits)
	AVM2_OBJ_FUNCTION = 2, // function / bound-method closure
	AVM2_OBJ_ARRAY = 3,    // Array instance (native_ext = Avm2ArrayExt)
	AVM2_OBJ_VECTOR = 4,   // Vector.<T> instance (native_ext = Avm2VectorExt)
} Avm2ObjectKind;

typedef struct Avm2DynProp
{
	struct Avm2DynProp* next;
	Avm2String name;
	Avm2Value value;
	uint8_t dont_enum;
	// AS3 `public static const` semantics for a value carried in a dynamic
	// prop (avm2_builtin_add_static_const): ReadOnly *and* DontDelete, so an
	// AS-level write throws #1074 and `delete` reports false. Only the
	// ActionScript paths honour it — avm2_object_set_dynamic is the C-side
	// installer and still overwrites, so builtin registration is unaffected.
	uint8_t read_only;
	// Tombstone: deleted entries stay linked (dead=1) so an in-flight
	// enumeration cursor survives deletion, mirroring Ruffle's
	// dynamic_map.rs stable bucket indices (dictionary_iter_modify).
	uint8_t dead;
	// Dictionary object-space key (identity, no namespace). When set, the
	// entry lives in "object space" (Ruffle dictionary_object.rs) and
	// `name` is unused; string finders skip it. Interleaved in the same
	// list so for-in order stays insertion order across both key kinds.
	Avm2Object* key_obj;
} Avm2DynProp;

// Bound-method closure cache: obj.method === obj.method must hold (Ruffle
// caches bound methods per (receiver, trait)).
typedef struct Avm2BoundMethod
{
	struct Avm2BoundMethod* next;
	const void* entry;  // the Avm2PropEntry this closure was bound from
	Avm2Object* fn;
} Avm2BoundMethod;

// Method reference: emitted method fn (with its defining file) or native.
typedef struct Avm2MethodRef
{
	Avm2MethodFn fn;
	Avm2AbcFileRt* file;  // NULL for native builtins
	const char* debug_name;
	uint32_t method_index;  // index into file->data->methods (emitted only)
	uint32_t param_count;   // declared arity; NATIVE builtins only (file==NULL),
	                        // where there is no ABC method to read it from.
	                        // Backs Function.length — see fn_get_length.
} Avm2MethodRef;

struct Avm2Object
{
	uint8_t kind;             // Avm2ObjectKind
	// This object is some class's `prototype`. avmplus keeps prototypes
	// dynamic even when their class is sealed (ES3 code assigns onto
	// Boolean.prototype & co constantly, and our own bootstrap does too),
	// so object_is_dynamic() honours this bit ahead of the class flags.
	// Lives in the padding after `kind`: no sizeof or field-offset change.
	uint8_t is_prototype;
	// This object is the transparent box `avm2_op_pushscope` wraps a PRIMITIVE
	// scope in (`with (7)`). The primitive itself lives in slots[1] (a
	// GC-traced slot), and every property GET/SET/CALL entry point unboxes the
	// receiver before use, so a method found through the box runs with the
	// primitive as `this` (ecma3/Statements/e12_10: `with (7) x = valueOf()`
	// must yield a number). The box is created ONLY here and is reachable only
	// from a scope stack / Avm2ScopeChain, so unboxing cannot be observed
	// anywhere else. Lives in the padding after `kind`: no sizeof change.
	uint8_t is_prim_box;
	Avm2Class* cls;           // class this object is an instance of
	Avm2Object* proto;        // prototype link (ES3 chain, dynamic reads)
	const Avm2VTable* vtable; // instance vtable (usually &cls->ivtable) or own
	Avm2Value* slots;
	uint32_t slot_count;
	Avm2DynProp* dyn_props;   // insertion order (append at tail)
	Avm2DynProp* dyn_tail;
	// Enumeration cursor over the dyn-prop part (Ruffle dynamic_map.rs
	// public_index/real_index): survives delete-during-iteration.
	uint32_t dyn_enum_public;
	Avm2DynProp* dyn_enum_pos;
	Avm2BoundMethod* bound_methods;
	void* native_ext;         // builtin instance data (arrays, MovieClip, ...)
	// GC (avm2_gc.c): intrusive census list + mark byte + native_ext blob
	// size (for the conservative ext pointer-scan). Every avm2_object_alloc'd
	// object enrolls in the census; the collector marks from roots between
	// ticks and sweeps the unmarked. native_ext_size is 0 for objects with no
	// ext OR with a precisely-traced ext (ARRAY/VECTOR elems are walked, not
	// scanned); a non-zero value means "conservatively scan this many bytes of
	// native_ext for embedded object pointers".
	Avm2Object* gc_next;
	// bit 0 = pinned (immortal: class objects / prototypes / XML nodes);
	// bits 1.. = MARK EPOCH — the object is marked iff (gc_mark >> 1) equals
	// the collector's current epoch, which increments once per collect. That
	// makes "reset every object to white" O(1) instead of a walk of the whole
	// census (tier-2 lever 2; see avm2_gc.c). Widened from uint8_t into the
	// padding that already sat between it and native_ext_size — sizeof and
	// every field offset are unchanged (static_assert in avm2_gc.c).
	uint32_t gc_mark;
	uint32_t native_ext_size;

	// AVM2_OBJ_FUNCTION payload: a bound method closure.
	Avm2MethodRef fn_method;
	Avm2Value fn_receiver;
	uint8_t fn_has_receiver;  // bound method: receiver overrides call-site this
	Avm2ScopeChain* fn_scope;
	Avm2Class* fn_bound_class;
	Avm2Object* fn_prototype; // lazily created `prototype` object
	uint8_t fn_proto_nulled;  // prototype was explicitly set to null
	// Object.prototype.toString on a function reports "[object Function-N]"
	// (avmplus). N is an opaque per-function id, assigned lazily on first ask
	// so the counter only advances for functions that are actually asked —
	// keeping it out of the allocation path. Sits in the padding before
	// class_ref: no field offset and no sizeof change.
	uint32_t fn_tostring_id;

	// AVM2_OBJ_CLASS payload.
	Avm2Class* class_ref;
};

// Array storage (Ruffle array.rs ArrayStorage): a dense region with holes
// plus a sorted sparse overflow list for far indices (a[4294967294] must
// not allocate 4G slots — array_index_max).
typedef struct Avm2SparseElem
{
	struct Avm2SparseElem* next;  // ascending by idx
	uint32_t idx;
	Avm2Value v;
} Avm2SparseElem;

typedef struct Avm2ArrayExt
{
	uint32_t length;     // logical Array.length (max index + 1)
	uint32_t dense_len;  // valid entries in elems
	uint32_t cap;
	Avm2Value* elems;    // AVM2_VALUE_HOLE marks holes
	Avm2SparseElem* sparse;
} Avm2ArrayExt;

// Vector storage (Ruffle vector.rs VectorStorage): typed dense elements,
// coerce-on-write to value_type (NULL = Vector.<*>), optional fixed length.
typedef struct Avm2VectorExt
{
	Avm2Class* value_type;  // NULL = Vector.<*>
	uint8_t fixed;
	uint32_t length;
	uint32_t cap;
	Avm2Value* elems;
} Avm2VectorExt;

// Allocation (from the shared o1heap; see avm2GcMarkRoots in avm2_main.c
// for the immortality note).
Avm2Object* avm2_object_alloc(Avm2Context* ctx, uint8_t kind, uint32_t slot_count);

// May `obj` grow dynamic (expando) properties? False for instances of a
// sealed class — prototypes, globals and activations are always dynamic.
// (Defined in avm2_ops.c, next to the property paths that consult it.)
int avm2_object_is_dynamic(Avm2Object* obj);

// Dynamic (expando) properties.
Avm2Value* avm2_object_find_dynamic(Avm2Object* obj, const char* name, uint32_t name_len);
Avm2DynProp* avm2_object_find_dynamic_entry(Avm2Object* obj, const char* name,
                                            uint32_t name_len);
Avm2DynProp* avm2_object_set_dynamic(Avm2Context* ctx, Avm2Object* obj, const char* name,
                                     uint32_t name_len, Avm2Value value);
// 1 = existed and was removed, 0 = no such property,
// -1 = exists but is DontDelete (read_only) — the caller must report false.
int avm2_object_delete_dynamic(Avm2Object* obj, const char* name, uint32_t name_len);

// Dictionary object-space keys (identity semantics).
Avm2Value* avm2_object_find_dynamic_obj(Avm2Object* obj, Avm2Object* key);
void avm2_object_set_dynamic_obj(Avm2Context* ctx, Avm2Object* obj, Avm2Object* key,
                                 Avm2Value value);
int avm2_object_delete_dynamic_obj(Avm2Object* obj, Avm2Object* key);

// Enumeration (Ruffle get_next_enumerant / get_enumerant_name protocol):
// enumerants are numbered from 1; 0 means exhausted. Arrays enumerate
// dense non-hole indices first, then dynamic props (skipping dont_enum).
uint32_t avm2_object_next_enumerant(Avm2Object* obj, uint32_t cur);
Avm2Value avm2_object_enumerant_name(Avm2Context* ctx, Avm2Object* obj, uint32_t idx);
Avm2Value avm2_object_enumerant_value(Avm2Context* ctx, Avm2Object* obj, uint32_t idx);

// Arrays.
Avm2Object* avm2_array_new(Avm2Context* ctx, uint32_t length);  // length holes
Avm2Object* avm2_array_from_values(Avm2Context* ctx, const Avm2Value* vals, uint32_t n);
Avm2ArrayExt* avm2_array_ext(Avm2Object* obj);  // NULL if not an array
// Attach zeroed element storage to an already-allocated AVM2_OBJ_ARRAY object.
Avm2ArrayExt* avm2_array_ext_attach(Avm2Context* ctx, Avm2Object* obj);
// Element get: returns HOLE-kind value if out of range / a hole.
Avm2Value avm2_array_get(Avm2Object* arr, uint32_t idx);
// Resolve index `idx` through `arr`'s PROTOTYPE chain (what a hole falls back
// to). Checks each prototype's element storage as well as its dynamic props,
// because Array.prototype is itself an Array.
bool avm2_array_proto_index(Avm2Object* arr, uint32_t idx, Avm2Value* out);
void avm2_array_set(Avm2Context* ctx, Avm2Object* arr, uint32_t idx, Avm2Value v);
void avm2_array_set_length(Avm2Context* ctx, Avm2Object* arr, uint32_t new_len);
int avm2_array_delete(Avm2Object* arr, uint32_t idx);  // 1 if it became a hole
void avm2_array_push(Avm2Context* ctx, Avm2Object* arr, Avm2Value v);

#endif // AVM2_OBJECT_H
