#ifndef AVM2_CLASS_H
#define AVM2_CLASS_H

// Per-class flattened vtable + class metadata, per Ruffle vtable.rs
// (avm2-support-plan §4.2): built at class-definition time by inheriting
// the superclass's table and appending own traits. Native builtins
// register through the same mechanism so the binding surface stays
// playerglobal-shaped (plan §4.5). Kept in its own small header (exposed
// architecture surface — see the plan's LittleCube note).

#include <avm2/avm2_abc.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_value.h>

typedef struct Avm2Context Avm2Context;

// A resolved property name: UTF-8 name + namespace. ns_kind is stored
// FOLDED (Namespace 0x08 → Package 0x16, i.e. "public") so comparisons
// are byte-wise; see avm2_ns_fold().
typedef struct Avm2PropKey
{
	const char* name;
	uint32_t name_len;
	uint8_t ns_kind;
	const char* ns_uri;
	uint32_t ns_len;
	// PrivateNamespace (kind 0x05) identity. avmplus/Ruffle compare private
	// namespaces by the identity of the constant-pool entry, never by URI
	// (ASC emits one Private entry per class, all with the SAME empty name),
	// so a URI compare makes every class's privates alias every other's.
	// This is the address of the owning Avm2AbcNamespace record — unique per
	// (ABC file, pool index) and stable for the process. NULL for every
	// non-private key; only consulted when BOTH keys are private.
	const void* ns_priv;
} Avm2PropKey;

typedef enum Avm2PropKind
{
	AVM2_PROP_SLOT = 0,   // Slot/Const/Class/Function traits: value in slots[slot_index]
	AVM2_PROP_METHOD = 1,
	AVM2_PROP_GETTER = 2,
	AVM2_PROP_SETTER = 3,
	AVM2_PROP_GETSET = 4, // both accessors defined
} Avm2PropKind;

typedef struct Avm2PropEntry
{
	Avm2PropKey key;
	uint8_t kind;  // Avm2PropKind
	uint8_t is_const;               // Const trait: writable only via InitProperty
	uint8_t is_function_trait;      // Function trait: slot defaults to a closure
	// Interface-namespace ALIAS (avm2_class.c add_iface_aliases_from): a copy
	// of a public implementation re-keyed into an implemented interface's
	// namespace so an interface-typed call site resolves. It is our dispatch
	// machinery, not a trait avmplus has, so trait ENUMERATION
	// (avmplus.describeTypeJSON) must skip it or every implemented member is
	// reported twice.
	uint8_t is_iface_alias;
	uint32_t slot_index;
	uint32_t type_mn;               // slot type multiname (coerce on write); 0 = any
	Avm2AbcFileRt* type_file;       // file whose pools type_mn/value index (NULL = none)
	Avm2AbcDefault value;           // slot default
	Avm2MethodRef method;           // METHOD/GETTER (or getter half of GETSET)
	Avm2MethodRef setter;           // SETTER half
	Avm2Class* defining_class;      // bound_class for method dispatch
	Avm2ScopeChain* method_scope;   // captured outer scope for the method
	// A GETSET's two halves can be declared by DIFFERENT classes: AS3 lets a
	// subclass override only the setter (or only the getter) and inherit the
	// other half. The setter then has to run bound to ITS own class, or a
	// `super.prop = v` inside it resolves against the wrong grandparent
	// (away3d Entity overrides arcane::implicitPartition's setter alone and
	// calls super — bound to ObjectContainer3D it looked past it to Object3D
	// and threw #1069). NULL = the setter shares `defining_class` above.
	Avm2Class* setter_defining_class;
	Avm2ScopeChain* setter_scope;
	// ABC trait METADATA, for avmplus.describeType(INCLUDE_METADATA).
	// Two halves for the same reason the two method refs are two halves:
	// Ruffle reports an accessor's metadata as the UNION of the getter's and
	// the setter's, and either half can be inherited. `*_file` names the ABC
	// whose string pool the key/value indices belong to.
	const Avm2AbcMetadata* metadata;         // slot / method / getter half
	uint32_t metadata_count;
	Avm2AbcFileRt* metadata_file;
	const Avm2AbcMetadata* setter_metadata;  // setter half
	uint32_t setter_metadata_count;
	Avm2AbcFileRt* setter_metadata_file;
} Avm2PropEntry;

// Per-slot metadata (Ruffle's slot_table): survives name shadowing, so a
// redeclared var's PARENT slot still gets its own type/default.
typedef struct Avm2SlotMeta
{
	uint8_t used;
	uint8_t is_function_trait;
	// ABC trait kind 4 (Class). Such a slot carries NO declared type (type_mn
	// is 0), so without this bit it would default to `undefined`; avmplus and
	// Ruffle default it to `null` (Ruffle vtable.rs
	// `TraitKind::Class { .. } => Value::Null`). Observable whenever a class
	// trait's slot is read before its defining script assigns it
	// (avm2/getouterscope_two_classobjects line 1).
	uint8_t is_class_trait;
	uint32_t type_mn;
	Avm2AbcFileRt* type_file;
	Avm2AbcDefault value;
	Avm2MethodRef fn_method;       // Function-trait default closure
	Avm2Class* defining_class;
	Avm2ScopeChain* method_scope;
} Avm2SlotMeta;

struct Avm2VTable
{
	uint32_t count;
	uint32_t cap;
	Avm2PropEntry* entries;
	uint32_t slot_count;  // slots consumed by SLOT-kind entries (1-based ids; index 0 unused)
	Avm2SlotMeta* metas;  // [meta_cap]; index = slot id
	uint32_t meta_cap;
	// --- name-keyed lookup accelerator (avm2_class.c, opaque) ---
	// Lazily built on the first find once `count` is stable, so the O(count)
	// linear scans of avm2_vtable_find[_mn/_public] become O(1)+small-bucket
	// (the AVM2 property-lookup hot path — ~60% of Seedling frame self-time).
	// Holds only entry indices (no GC pointers): malloc'd, invisible to the
	// GC, rebuilt when `indexed_count != count`, freed on rebuild. `no_index`
	// opts a vtable out (set on the per-call newactivation/newcatch vtables,
	// which are GC'd and would otherwise leak their index).
	void* name_index;        // Avm2VTableIndex* (NULL until first index)
	uint32_t indexed_count;  // `count` at the last index build
	// --- default-slot template (avm2_slots_init_defaults fast path) ---
	// Precomputed image of every slot default that is a NON-POINTER value,
	// so construction is a memcpy plus a short patch loop over the slots
	// that must be realized per object (function-trait closures, namespace
	// and string defaults). GC HAZARD RULE: the template must never hold a
	// STRING or OBJECT pointer — it is plain malloc'd memory the collector
	// neither traces nor sweeps, so a pointer parked here would be an
	// untraced root (see the collectable-strings work). Built lazily on the
	// first construction, and rebuilt if `slot_count` grew since; never
	// built for `no_index` (per-call newactivation/newcatch) vtables, whose
	// objects would pay the build cost once per call.
	Avm2Value* slot_tpl;      // [tpl_slot_n + 1]; index 0 unused
	uint32_t* slot_patch;     // slot ids needing per-object realization
	uint32_t slot_patch_n;
	uint32_t tpl_slot_n;      // `slot_count` the template was built for
	uint8_t tpl_built;
	uint8_t no_index;        // 1 = never index (short-lived per-call vtable)
};

// Native construct/call hooks for builtins (String/Number/Array/... whose
// `new`/call return primitives or special objects).
typedef Avm2Value (*Avm2NativeCtor)(Avm2Context* ctx, struct Avm2Class* cls,
                                    const Avm2Value* args, uint32_t argc);

struct Avm2Class
{
	Avm2PropKey name;
	Avm2Class* super_class;
	uint8_t flags;
	Avm2VTable ivtable;            // instance vtable (inherited + own)
	Avm2MethodRef instance_init;
	// The static initializer (ABC class_info.cinit). Ruffle models the static
	// side as a separate c_class whose instance_init IS this method, which is
	// how a cinit frame gets spelled "Test$cinit()" (function.rs::
	// display_function). We keep one class per ABC class, so the ref lives
	// here. Zeroed for builtins (they have no ABC cinit).
	Avm2MethodRef class_init;
	Avm2ScopeChain* scope;         // class scope: captured at NewClass (statics/cinit)
	Avm2ScopeChain* iscope;        // instance scope: class scope + [class object]
	                               // (instance traits + instance_init outer chain)
	Avm2Object* class_object;
	Avm2Object* prototype_obj;     // ES3 prototype object (chain mirrors super)
	// Protected namespace (per-class URI); inherited protected traits are
	// re-keyed onto the subclass's protected ns at class-define time.
	uint8_t has_protected_ns;
	Avm2PropKey protected_key;     // ns_kind/ns_uri/ns_len meaningful only
	// Declared interfaces (direct); resolution is lazy (mn indices against
	// the defining file). Builtins have resolved==direct==NULL.
	uint32_t interface_count;
	const uint32_t* interface_mns;
	Avm2AbcFileRt* iface_file;
	Avm2Class** interfaces;        // resolved cache (NULL until first use)
	// Builtins: bytes of native instance state to allocate into
	// Avm2Object.native_ext at construction (nearest ancestor wins), plus
	// an optional native constructor body.
	uint32_t native_ext_size;
	void (*native_init)(Avm2Context* ctx, Avm2Object* obj);
	// Builtin-container inheritance: the object KIND that instances of this
	// class get (0 = AVM2_OBJ_SCRIPT). Resolved once per class at define
	// time by walking the super chain, so `class MyList extends Array`
	// instances allocate real element storage and every Array method sees
	// it (regress/bug_420755, regress/bug_654807_*). Separate from
	// native_ext_size on purpose — Array's is 0 (avm2_array_new attaches
	// the Avm2ArrayExt by hand) and ByteArray already owns that mechanism.
	uint8_t instance_kind;
	// Constructor semantics a SWF subclass's ConstructSuper must run on the
	// already-allocated receiver, for builtin bases whose `new` lives in
	// native_construct (which is deliberately not inherited). Array's is
	// `super(length)` / `super(a, b, c)` — as3/Array/length_mods's Subarray.
	void (*native_super_init)(Avm2Context* ctx, Avm2Object* obj,
	                          const Avm2Value* args, uint32_t argc);
	// If set, `new C(...)` returns this instead of the standard
	// allocate + run-instance-init path (String/Number/Boolean/int/uint/
	// Array/Function). Also used for coercion-style class calls when
	// native_call is unset.
	Avm2NativeCtor native_construct;
	// If set, `C(...)` (class call) uses this; default is coerce-arg0.
	Avm2NativeCtor native_call;
	// Vector machinery (avm2_vector.c): the generic Vector class accepts
	// ApplyType; parameterized Vector.<T> classes carry their element type.
	uint8_t is_generic_vector;
	uint8_t is_vector;           // parameterized Vector.<T> instance class
	Avm2Class* vector_param;     // T (NULL = * — only meaningful when is_vector)
};

// One local-scope stack entry in an emitted method body.
typedef struct Avm2ScopeEntry
{
	Avm2Object* obj;
	uint8_t is_with;
} Avm2ScopeEntry;

// Immutable captured scope chain.
struct Avm2ScopeChain
{
	uint32_t count;
	Avm2ScopeEntry* entries;  // bottom (0) → top (count-1)
};

// Dot-qualified class name for error messages ("pkg.Class" / "Class").
int avm2_class_qname_buf(const Avm2Class* cls, char* buf, int size);

// "::"-qualified class name for stack frames and function display names
// ("pkg::Class" / "Class") — Ruffle to_qualified_name_no_mc.
int avm2_class_qname_colons_buf(const Avm2Class* cls, char* buf, int size);

// Avm2ObjectKind instances of `cls` are allocated with (see instance_kind).
uint8_t avm2_class_instance_kind(Avm2Context* ctx, const Avm2Class* cls);

// Namespace kind folding for comparisons (public kinds unify).
uint8_t avm2_ns_fold(uint8_t kind);
int avm2_propkey_matches(const Avm2PropKey* a, const Avm2PropKey* b);
// Public-namespace check (kind public + empty URI). The AS3 builtin
// namespace (http://adobe.com/AS3/2006/builtin) also counts: our builtins
// register public-keyed methods only.
int avm2_propkey_is_public(const Avm2PropKey* k);
Avm2PropKey avm2_public_key(const char* name, uint32_t name_len);
// Fill a key's namespace half from an ABC constant-pool namespace index
// (kind + URI + the private-identity token). Use this everywhere a key is
// built from `data->namespaces[i]` — a hand-rolled copy silently drops
// ns_priv and reintroduces private-namespace aliasing.
void avm2_key_ns_from_abc(Avm2PropKey* k, const Avm2AbcFileData* data, uint32_t ns_idx);

// Multiname resolution against a file's pools. propkey_from_qname fails
// (returns 0) on lazy multinames; mn_match handles QName* and static
// Multiname (ns-set) kinds. mn_is_lazy_name reports MultinameL kinds
// (name popped from the stack at runtime).
int avm2_propkey_from_qname(const Avm2AbcFileData* data, uint32_t mn_idx, Avm2PropKey* out);
int avm2_mn_match(const Avm2AbcFileData* data, uint32_t mn_idx, const Avm2PropKey* key);
void avm2_mn_name(const Avm2AbcFileData* data, uint32_t mn_idx,
                  const char** name, uint32_t* name_len);
// Does the multiname's namespace (set) contain the public namespace?
int avm2_mn_has_public_ns(const Avm2AbcFileData* data, uint32_t mn_idx);

// VTable build/lookup.
void avm2_vtable_append(Avm2Context* ctx, Avm2VTable* vt, const Avm2PropEntry* e);
const Avm2PropEntry* avm2_vtable_find(const Avm2VTable* vt, const Avm2PropKey* key);
const Avm2PropEntry* avm2_vtable_find_mn(const Avm2VTable* vt, const Avm2AbcFileData* data,
                                         uint32_t mn_idx);
// Name-only public lookup (for public-property calls by name).
const Avm2PropEntry* avm2_vtable_find_public(const Avm2VTable* vt,
                                             const char* name, uint32_t name_len);
// Lazy-name lookup honoring the multiname's static ns set: match `name`
// against entries whose namespace is in mn's set (MultinameL with a
// non-public set, e.g. dict.test::["test1"]).
const Avm2PropEntry* avm2_vtable_find_mn_named(const Avm2VTable* vt,
                                               const Avm2AbcFileData* data,
                                               uint32_t mn_idx,
                                               const char* name, uint32_t name_len);

// Add trait entries from static data onto a vtable (used for class
// instance/static traits, script globals, and activation objects). `file`
// provides pools and the method fns; `defining_class`/`scope` are stamped
// on method entries.
void avm2_vtable_add_traits(Avm2Context* ctx, Avm2VTable* vt, Avm2AbcFileRt* file,
                            const Avm2AbcTrait* traits, uint32_t trait_count,
                            Avm2Class* defining_class, Avm2ScopeChain* scope);

// Initialize an object's slots from its vtable's slot defaults (trait
// value if present, else the type's default: int/uint 0, Number NaN,
// Boolean false, any undefined, other null).
void avm2_slots_init_defaults(Avm2Context* ctx, Avm2Object* obj, const Avm2VTable* vt);

// Scope chains.
Avm2ScopeChain* avm2_scope_capture(Avm2Context* ctx, const Avm2ScopeChain* outer,
                                   const Avm2ScopeEntry* lscope, uint32_t scope_n);

// Class realization (NewClass) and construction (ConstructProp / root).
Avm2Class* avm2_class_define(Avm2Context* ctx, Avm2AbcFileRt* file, uint32_t class_idx,
                             Avm2Class* super_class, Avm2ScopeChain* scope);
// True while a native_init hook is running for a SCRIPT `new`, false for an
// internal C mint. See avm2_class.c for why the two must be distinguished.
int avm2_class_alloc_is_script_new(void);
void avm2_class_arm_script_construct(void);

Avm2Value avm2_class_construct(Avm2Context* ctx, Avm2Class* cls,
                               const Avm2Value* args, uint32_t argc);

// ES4 type check (Ruffle value.rs is_of_type): class chain + interfaces,
// with the numeric-duality special cases for the builtin primitive classes.
bool avm2_value_is_of_type(Avm2Context* ctx, Avm2Value v, Avm2Class* type_class);
bool avm2_class_has_interface(Avm2Context* ctx, Avm2Class* cls, Avm2Class* iface);
// Fill `cls->interfaces` from `cls->interface_mns` (idempotent; misses are
// re-tried on every call). Exposed for describeType's all_interfaces walk.
void avm2_class_resolve_interfaces(Avm2Context* ctx, Avm2Class* cls);

// The class of a value (primitives map to their builtin classes; panics on
// null/undefined — callers null-check first).
Avm2Class* avm2_value_class(Avm2Context* ctx, Avm2Value v);
// The vtable of a value (primitive receivers use their class ivtable).
const Avm2VTable* avm2_value_vtable(Avm2Context* ctx, Avm2Value v);
// The prototype object of a value's type (for proto-chain reads).
Avm2Object* avm2_value_proto(Avm2Context* ctx, Avm2Value v);

// Invoke a method reference (emitted or native).
Avm2Value avm2_call_method_ref(Avm2Context* ctx, const Avm2MethodRef* m,
                               Avm2Class* bound_class, Avm2ScopeChain* outer,
                               Avm2Value this_val, const Avm2Value* args, uint32_t argc);

// Invoke a function object (AVM2_OBJ_FUNCTION closure).
Avm2Value avm2_call_function_obj(Avm2Context* ctx, Avm2Object* fnobj,
                                 Avm2Value receiver, const Avm2Value* args, uint32_t argc);

// Call any callable value with an explicit receiver (Op::Call semantics).
Avm2Value avm2_call_value(Avm2Context* ctx, Avm2Value func, Avm2Value receiver,
                          const Avm2Value* args, uint32_t argc);

// `new` on any value (class object, function object) — Op::Construct.
Avm2Value avm2_construct_value(Avm2Context* ctx, Avm2Value ctor,
                               const Avm2Value* args, uint32_t argc);

// Coerce a value to the type named by a multiname (Ruffle coerce_to_type).
// mn_idx 0 = any (no coercion). Throws TypeError 1034 on mismatch.
Avm2Value avm2_coerce_to_type_mn(Avm2Context* ctx, Avm2AbcFileRt* file,
                                 uint32_t mn_idx, Avm2Value v);
// Coercion by resolved class (NULL class = no coercion).
Avm2Value avm2_coerce_to_class(Avm2Context* ctx, Avm2Class* cls, Avm2Value v);
// Resolve a multiname to a class (via file classes/domain/builtins);
// NULL if not resolvable.
Avm2Class* avm2_class_for_mn(Avm2Context* ctx, Avm2AbcFileRt* file, uint32_t mn_idx);

// Decode an ABC default value against a file's pools.
Avm2Value avm2_default_value(Avm2Context* ctx, Avm2AbcFileRt* file, const Avm2AbcDefault* d);

// Function objects.
Avm2Object* avm2_function_new(Avm2Context* ctx, const Avm2MethodRef* method,
                              Avm2Class* bound_class, Avm2ScopeChain* scope,
                              Avm2Value receiver, bool has_receiver);

#endif // AVM2_CLASS_H
