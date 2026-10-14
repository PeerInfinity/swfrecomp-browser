#ifndef AVM2_GLOBALS_H
#define AVM2_GLOBALS_H

// Builtins (avm2-support-plan §4.5): hand-written C implementations of the
// tranche-1 core (Object, Class, Function, String, Number/int/uint,
// Boolean, Array, Math, the Error family, toplevel functions) registered
// through the same vtable mechanism as SWF classes so the binding surface
// stays playerglobal-shaped. flash.display remains a stub chain
// (EventDispatcher → ... → MovieClip with addFrameScript).
//
// Also owns the "domain": the FindPropStrict last-resort lookup from a
// definition name to the globals object defining it (Ruffle Domain
// equivalent), including lazy script initialization.

#include <avm2/avm2_class.h>

typedef struct Avm2Context Avm2Context;

// One ApplicationDomain, as a resolution SCOPE (loader-arc tranche 8, §10b).
// Domains form a tree; a lookup checks the querying scope, then its ancestors,
// and finally the system domain — which is `scope == NULL`, the terminator of
// every chain and the home of the builtins.
//
// Entries stay in ONE flat array with ONE name index (below): a definition is
// exported only when the name is not already visible from the exporting scope
// (Ruffle `Domain::export_definition`), so at most one entry per name is ever
// visible from a given scope, and sibling domains are the only place two
// entries with the same name can coexist. That invariant is what keeps
// resolution a plain "first VISIBLE match" rather than a distance computation,
// and it is why a single-movie program's entry list is exactly what it was
// before domains existed.
typedef struct Avm2DomainScope
{
	const struct Avm2DomainScope* parent;  // NULL = the system domain
	struct Avm2Object* obj;   // the AS3 ApplicationDomain instance (lazy)
	struct Avm2DomainScope* next_all;  // every scope, for GC + reverse lookup
} Avm2DomainScope;

typedef struct Avm2DomainEntry
{
	Avm2PropKey key;
	Avm2AbcFileRt* file;    // NULL = builtin
	uint32_t script_index;  // valid when file != NULL
	const Avm2DomainScope* scope;  // NULL = system/builtin (visible to all)
} Avm2DomainEntry;

typedef struct Avm2Domain
{
	uint32_t count;
	uint32_t cap;
	Avm2DomainEntry* entries;
	// The main movie's domain, parented to the system domain. Every file in a
	// single-movie program shares it.
	Avm2DomainScope root;
	// The scope ABC files load into right now (set around a movie's
	// registration). Avoids threading a scope through avm2_abc_load.
	const Avm2DomainScope* loading;
	Avm2DomainScope* scopes;   // list head (root first)
	// Number of scopes ever created, `root` included. While it is 1 the
	// visibility test is skipped outright — the perf rail of §10b: a
	// single-movie program must pay nothing for domains it does not have.
	uint32_t scope_count;
	// Name-keyed lookup accelerator (avm2_globals.c, opaque), mirroring the
	// Avm2VTable index: avm2_domain_find is a per-findproperty linear scan of
	// every global class/function, so hashing by name makes it O(1)+small-
	// bucket. Lazily (re)built when `indexed_count != count`; holds only entry
	// indices (no GC pointers), malloc'd, GC-invisible.
	void* name_index;        // Avm2DomainIndex* (NULL until first index)
	uint32_t indexed_count;  // `count` at the last index build
} Avm2Domain;

// Creates builtin classes + the builtin globals object and seeds the domain.
void avm2_globals_init(Avm2Context* ctx);

// Domain registration (called during ABC load for every script trait).
// Exports into `ctx->domain.loading`, and does nothing when the name is
// already visible from there (Ruffle Domain::export_definition).
void avm2_domain_add(Avm2Context* ctx, const Avm2PropKey* key,
                     Avm2AbcFileRt* file, uint32_t script_index);

// Domain lookup: returns the globals object defining `key` (running the
// defining script's initializer lazily if needed), or NULL if undefined.
// `scope` is the querying code's domain — the calling file's `scope`, or NULL
// for a system-domain-only lookup.
Avm2Object* avm2_domain_find(Avm2Context* ctx, const Avm2DomainScope* scope,
                             const Avm2PropKey* key);

// A fresh domain whose parent chain is `parent` (NULL = the system domain).
Avm2DomainScope* avm2_domain_scope_new(Avm2Context* ctx,
                                       const Avm2DomainScope* parent);

// The main movie's domain. Also the fallback for any lookup with no file in
// hand (builtin helpers, the root SymbolClass binding).
Avm2DomainScope* avm2_domain_root_scope(Avm2Context* ctx);

// The AS3 ApplicationDomain instance for a scope, built on first use.
Avm2Object* avm2_domain_scope_object(Avm2Context* ctx,
                                     const Avm2DomainScope* scope);
// ...and the reverse: the scope an ApplicationDomain instance stands for, or
// NULL if the value is not one.
const Avm2DomainScope* avm2_domain_scope_of_object(Avm2Context* ctx,
                                                   Avm2Object* obj);

// Runs a script's initializer if it has not run yet (lazy or eager init).
void avm2_script_ensure_init(Avm2AbcFileRt* file, uint32_t script_index);

// Name-based public property access (no multiname pools required): used
// by coerce_to_primitive (toString/valueOf), builtins, and the `in`/
// enumeration paths. get sets *found (may be NULL) and returns undefined
// when missing.
Avm2Value avm2_get_public_property(Avm2Context* ctx, Avm2Value recv,
                                   const char* name, uint32_t name_len, int* found);
Avm2Value avm2_call_public_property(Avm2Context* ctx, Avm2Value recv,
                                    const char* name, uint32_t name_len,
                                    const Avm2Value* args, uint32_t argc);
int avm2_has_public_property(Avm2Context* ctx, Avm2Value recv,
                             const char* name, uint32_t name_len);
void avm2_set_public_property(Avm2Context* ctx, Avm2Value recv,
                              const char* name, uint32_t name_len, Avm2Value value);
// As above but with initproperty semantics (const slots writable) — for
// deserializers reconstructing an object, not for script-visible assignment.
void avm2_init_public_property(Avm2Context* ctx, Avm2Value recv,
                               const char* name, uint32_t name_len, Avm2Value value);
int avm2_has_own_public_property(Avm2Context* ctx, Avm2Value recv,
                                 const char* name, uint32_t name_len);

// Builtin class registration helpers (also used by avm2_string.c etc.).
Avm2Class* avm2_builtin_class(Avm2Context* ctx, const char* ns, const char* name,
                              Avm2Class* super);
// ...API-VERSION GATED. Ruffle annotates playerglobal classes `[API("N")]`
// and maps N-660 onto an ApiVersion ordinal (api_version.rs): the class is
// simply INVISIBLE to a movie older than that. This sibling builds the class
// identically but skips the two EXPOSURE calls (builtin_global_define_ro +
// avm2_domain_add) when ctx->swf_version < min_swf, so the name 1065s while
// the class itself still exists for the runtime to mint internally
// (Stage3D.requestContext3D hands back a Context3D at every SWF version).
// min_swf 0 = always exposed, i.e. identical to avm2_builtin_class.
Avm2Class* avm2_builtin_class_api(Avm2Context* ctx, const char* ns,
                                  const char* name, Avm2Class* super,
                                  uint8_t min_swf);
void avm2_builtin_add_method(Avm2Context* ctx, Avm2Class* cls, const char* name,
                             Avm2MethodFn fn);
// ...with a declared arity, which is what Function.length reports. Needed
// wherever a class's prototype is an instance of that class (Array), because
// then the ivtable trait -- not the ES3 prototype function -- is what
// `Cls.prototype.method.length` resolves to.
void avm2_builtin_add_method_n(Avm2Context* ctx, Avm2Class* cls, const char* name,
                               Avm2MethodFn fn, uint32_t param_count);
// ...keyed in the AS3 builtin namespace instead of public (avmplus's shape for
// a builtin class's INSTANCE methods; the public copies live on the
// prototype). Invisible to dispatch, visible to hasOwnProperty.
void avm2_builtin_add_method_as3(Avm2Context* ctx, Avm2Class* cls, const char* name,
                                 Avm2MethodFn fn);
// Override the stack-frame spelling of an already-registered public instance
// method. FP names a method declared in a non-public namespace with its URI
// ("Function/http://adobe.com/AS3/2006/builtin::call()"), but our keys stay
// public so dispatch is unaffected; only the debug_name moves. `debug_name`
// must be a string literal or another allocation that outlives the context.
void avm2_builtin_set_debug_name(Avm2Context* ctx, Avm2Class* cls,
                                 const char* name, const char* debug_name);
void avm2_builtin_add_getter(Avm2Context* ctx, Avm2Class* cls, const char* name,
                             Avm2MethodFn fn);
// Instance accessor pair (setter may be NULL for a getter-only prop).
void avm2_builtin_add_getset(Avm2Context* ctx, Avm2Class* cls, const char* name,
                             Avm2MethodFn getter, Avm2MethodFn setter);
void avm2_builtin_add_static_method(Avm2Context* ctx, Avm2Class* cls, const char* name,
                                    Avm2MethodFn fn);
// As above, declaring the arity that Function.length must report.
void avm2_builtin_add_static_method_n(Avm2Context* ctx, Avm2Class* cls, const char* name,
                                      Avm2MethodFn fn, uint32_t param_count);
void avm2_builtin_add_static_const(Avm2Context* ctx, Avm2Class* cls, const char* name,
                                   Avm2Value value);
// Make `new Cls()` throw "Error #2012: <Name>$ class cannot be instantiated."
// — the [Ruffle(Abstract)] gate, for a class with no display/FTE machinery
// of its own to hang it on.
void avm2_builtin_set_abstract(Avm2Context* ctx, Avm2Class* cls);
// As above, but only a script `new` is refused — for a class the runtime
// itself still mints (Graphics, Stage, the Stage3D resource family).
void avm2_builtin_set_abstract_script_only(Avm2Context* ctx, Avm2Class* cls);
// Static accessor pair on the class object (setter may be NULL).
void avm2_builtin_add_static_getset(Avm2Context* ctx, Avm2Class* cls, const char* name,
                                    Avm2MethodFn getter, Avm2MethodFn setter);
// Install a native function on a prototype object (dont_enum).
void avm2_proto_add_function(Avm2Context* ctx, Avm2Object* proto, const char* name,
                             Avm2MethodFn fn);
// As above, declaring the arity that Function.length must report.
void avm2_proto_add_function_n(Avm2Context* ctx, Avm2Object* proto, const char* name,
                               Avm2MethodFn fn, uint32_t param_count);
// Register a toplevel native function on the builtin globals + domain.
void avm2_builtin_add_global_fn(Avm2Context* ctx, const char* name, Avm2MethodFn fn);
// As above, declaring the arity that Function.length must report.
void avm2_builtin_add_global_fn_n(Avm2Context* ctx, const char* name, Avm2MethodFn fn,
                                  uint32_t param_count);
// Expose a value on the builtin globals + domain under an arbitrary key.
void avm2_builtin_define_alias(Avm2Context* ctx, Avm2PropKey key, Avm2Value value);

// Per-module builtin registration (called from avm2_globals_init).
void avm2_register_string(Avm2Context* ctx);
void avm2_register_number(Avm2Context* ctx);   // Number/int/uint/Boolean/Math
void avm2_register_array(Avm2Context* ctx);
void avm2_register_error(Avm2Context* ctx);
void avm2_register_toplevel(Avm2Context* ctx); // trace/isNaN/parseInt/...
void avm2_register_vector(Avm2Context* ctx);   // __AS3__.vec::Vector + specializations
void avm2_register_regexp(Avm2Context* ctx);   // RegExp + String regex paths
void avm2_register_json(Avm2Context* ctx);     // SWF13+ (caller gates)
void avm2_register_nsqname(Avm2Context* ctx);  // Namespace + QName
// swfmodern.Reflect (avm2_class.c) — the trait-table observation hook for
// injected code, sibling of swfmodern.Rng. Lives with the vtable machinery it
// walks; registered here so the whole swfmodern family is minted in one place.
void avm2_register_reflect(Avm2Context* ctx);
void avm2_register_dictionary(Avm2Context* ctx);  // flash.utils.Dictionary
// Is `obj` an instance of (a subclass of) flash.utils.Dictionary?
int avm2_is_dictionary(Avm2Object* obj);
void avm2_register_bytearray(Avm2Context* ctx);  // flash.utils.ByteArray (+Endian)
void avm2_register_amf(Avm2Context* ctx);  // flash.net alias fns
// Single-value AMF codec for the NetConnection wire (avm2_net.c). The returned
// buffers are MALLOC'd — free() them — because the call queue holds them across
// ticks and amf_packet.c reads them as plain byte ranges. Each call gets fresh
// reference tables, which is Flash's per-top-level-value framing.
unsigned char* avm2_amf0_write_value(Avm2Context* ctx, Avm2Value v, size_t* out_len);
// The AMF0 0x11 "avmplus object" escape + one AMF3 value (objectEncoding = AMF3).
unsigned char* avm2_amf3_write_value_tagged(Avm2Context* ctx, Avm2Value v,
                                            size_t* out_len);
Avm2Value avm2_amf0_read_value(Avm2Context* ctx, const unsigned char* p, size_t n);
void avm2_register_date(Avm2Context* ctx); // Date (ECMA-262 §15.9 + AS3)
void avm2_register_external(Avm2Context* ctx);  // flash.external.ExternalInterface

// Date instance state (avm2_date.c). `millis` is the ECMA time value: ms
// since the epoch in UTC, or NaN for an invalid Date.
typedef struct Avm2DateExt
{
	double millis;
} Avm2DateExt;
Avm2DateExt* avm2_date_ext_of(Avm2Value v);

// ByteArray instance state (avm2_bytearray.c).
typedef struct Avm2ByteArrayExt
{
	uint8_t* bytes;
	uint32_t len;
	uint32_t cap;
	uint32_t position;
	uint8_t endian_little;    // 0 = bigEndian (default)
	uint8_t object_encoding;  // 0 = AMF0, 3 = AMF3
} Avm2ByteArrayExt;

// NULL when the value is not a ByteArray (subclasses included).
Avm2ByteArrayExt* avm2_bytearray_ext_of(Avm2Value v);

// Decode a byte run to a string honouring a leading BOM (UTF-8 / UTF-16LE /
// UTF-16BE; no BOM means lenient UTF-8) — Ruffle's strip_bom. Shared by
// ByteArray.toString and URLLoader/URLStream text decoding. `bytes` may be
// NULL when `len` is 0.
const Avm2String* avm2_strip_bom(Avm2Context* ctx, const uint8_t* bytes,
                                 uint32_t len);

// flash.net.Socket reuses the whole IDataInput/IDataOutput body set over a
// buffer PAIR: reads drain the inbound buffer, writes fill the outbound one.
// avm2_net.c registers a resolver so those bodies can be shared verbatim.
// `write_dir` is 1 for write*, 0 for read*. Returning NULL means "not mine";
// the resolver may also throw (an unconnected Socket raises IOError #2002).
typedef Avm2ByteArrayExt* (*Avm2BaAltResolver)(Avm2Activation* act, int write_dir);
void avm2_bytearray_set_alt_resolver(Avm2BaAltResolver fn);
// ByteArray first, then the resolver. NULL when neither claims `this`.
Avm2ByteArrayExt* avm2_bytearray_ext_dir(Avm2Activation* act, int write_dir);
// Register the 28 IDataInput/IDataOutput natives on `cls`. The halves are also
// available separately: flash.net.URLStream implements IDataInput only.
void avm2_bytearray_install_data_input(Avm2Context* ctx, Avm2Class* cls);
void avm2_bytearray_install_data_output(Avm2Context* ctx, Avm2Class* cls);
void avm2_bytearray_install_data_io(Avm2Context* ctx, Avm2Class* cls);
// Grow/shrink storage (clamps position; used by the [] index write path).
void avm2_bytearray_set_length_public(Avm2Context* ctx, Avm2ByteArrayExt* ba,
                                      uint32_t new_len);
// write/readUnsignedInt at the current position, honoring endianness (used
// by BitmapData get/setPixels). The read throws EOFError on underflow.
void avm2_bytearray_write_uint_public(Avm2Context* ctx, Avm2ByteArrayExt* ba, uint32_t v);
uint32_t avm2_bytearray_read_uint_public(Avm2Context* ctx, Avm2ByteArrayExt* ba);

// Install ApplicationDomain.domainMemory + MIN_DOMAIN_MEMORY_LENGTH, the
// script-visible half of the Alchemy memory opcodes (avm2_mops.c).
void avm2_mops_register(Avm2Context* ctx, Avm2Class* appdomain_cls);

void avm2_register_proxy(Avm2Context* ctx);  // flash.utils.Proxy
// Is `obj` an instance of (a subclass of) flash.utils.Proxy?
int avm2_is_proxy(Avm2Object* obj);
// Invoke a flash_proxy-namespace hook (getProperty/setProperty/...) on a
// Proxy instance, honoring subclass overrides.
Avm2Value avm2_proxy_call_hook(Avm2Context* ctx, Avm2Object* obj, const char* mname,
                               const Avm2Value* args, uint32_t argc);
// Enumeration hooks (avm2_object.c protocol); return 0 if not a Proxy.
int avm2_proxy_next_enumerant(Avm2Object* obj, uint32_t cur, uint32_t* out);
int avm2_proxy_enumerant_name(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                              Avm2Value* out);
int avm2_proxy_enumerant_value(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                               Avm2Value* out);

// Namespace/QName instance state (avm2_nsqname.c). prefix == NULL is the
// undefined prefix; a QName uri == NULL is the any namespace and local ==
// NULL the any name. kind is the raw ABC namespace kind (0 = any).
typedef struct Avm2NamespaceExt
{
	const Avm2String* uri;
	const Avm2String* prefix;
	uint8_t kind;
} Avm2NamespaceExt;

typedef struct Avm2QNameExt
{
	const Avm2String* uri;
	const Avm2String* local;
	// The Ruffle Multiname ATTRIBUTE flag: set when the QName names an
	// attribute (e.g. xml.attributes()[0].name()); honored by E4X lazy
	// names and Proxy's flash_proxy::isAttribute.
	uint8_t is_attribute;
} Avm2QNameExt;

// NULL when the value is not a Namespace / QName instance.
Avm2NamespaceExt* avm2_namespace_ext_of(Avm2Value v);
Avm2QNameExt* avm2_qname_ext_of(Avm2Value v);
Avm2Object* avm2_namespace_new(Avm2Context* ctx, const Avm2String* uri,
                               const Avm2String* prefix, uint8_t kind);
// Box a constant-pool namespace (PushNamespace, namespace trait defaults).
Avm2Object* avm2_namespace_from_pool(Avm2Context* ctx, Avm2AbcFileRt* file,
                                     uint32_t ns_idx);
Avm2Object* avm2_qname_new(Avm2Context* ctx, const Avm2String* uri,
                           const Avm2String* local);
const Avm2String* avm2_qname_to_string(Avm2Context* ctx, const Avm2QNameExt* ext);
// Enumeration hooks (called from avm2_object.c); return 0 if `obj` is not
// a Namespace/QName.
int avm2_nsqname_next_enumerant(Avm2Object* obj, uint32_t cur, uint32_t* out);
int avm2_nsqname_enumerant_name(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                                Avm2Value* out);
int avm2_nsqname_enumerant_value(Avm2Context* ctx, Avm2Object* obj, uint32_t idx,
                                 Avm2Value* out);

// Plain (non-regex) String.split — the regex-aware split falls back to it.
Avm2Value avm2_string_split_plain(struct Avm2Activation* act);

// The current (root) ApplicationDomain as a value, or null if not yet built.
Avm2Value avm2_current_domain_value(Avm2Context* ctx);

// Definition lookup by dotted/:: name ("pkg::Name", "pkg.Name", "Name"),
// including on-demand "Vector.<...>" applications. Sets *found.
Avm2Value avm2_find_definition(Avm2Context* ctx, const char* s, uint32_t len,
                               int* found);
// ...resolved in a specific domain (getDefinitionByName runs in the CALLING
// file's; ApplicationDomain.getDefinition in the receiver's).
Avm2Value avm2_find_definition_in(Avm2Context* ctx, const Avm2DomainScope* scope,
                                  const char* s, uint32_t len, int* found);

// Vector machinery (avm2_vector.c).
Avm2VectorExt* avm2_vector_ext(Avm2Object* obj);  // NULL if not a Vector
// Resolve "Vector.<...>" (optional __AS3__.vec prefix) to the applied class.
Avm2Class* avm2_vector_class_by_name(Avm2Context* ctx, const char* s, uint32_t len);
// The parameterized class Vector.<param> (param NULL = *): cached per param.
Avm2Class* avm2_vector_apply(Avm2Context* ctx, Avm2Class* param);
// New Vector.<T> instance of the given PARAMETERIZED class.
Avm2Object* avm2_vector_new(Avm2Context* ctx, Avm2Class* vec_class,
                            uint32_t length, int fixed);
// Index access with Ruffle vector_object.rs error semantics (1125 on
// SWF>=11). set coerces to T; get throws on out-of-range.
Avm2Value avm2_vector_get_index(Avm2Context* ctx, Avm2Object* obj, uint32_t idx);
void avm2_vector_set_index(Avm2Context* ctx, Avm2Object* obj, uint32_t idx, Avm2Value v);
// Numeric-name access path (name parses as f64 but maybe not a valid u32
// index): returns 0 if the name is not numeric (caller falls through to the
// normal property path), 1 if handled; throws per version semantics.
// `out` NULL = write `set_value`, else read into *out. May "handle" a read
// by falling back (returns 0) for negative indices on SWF10.
int avm2_vector_name_access(Avm2Context* ctx, Avm2Object* obj, const char* name,
                            uint32_t name_len, Avm2Value* out, Avm2Value set_value);

// The avmplus QuickSort shared by Array.sort/sortOn and Vector.sort
// (Ruffle globals/array.rs qsort).
typedef struct Avm2SortItem { uint32_t idx; Avm2Value v; } Avm2SortItem;
typedef int (*Avm2SortCmp)(void* ud, const Avm2SortItem* a, const Avm2SortItem* b);
void avm2_avmplus_qsort(void* ud, Avm2SortCmp cmp, Avm2SortItem* s, uint32_t n);

// EventDispatcher instance state (Avm2Object.native_ext). ed_target is the
// EventDispatcher(target) constructor arg (the IEventDispatcher aggregation
// pattern); undefined/null = the dispatcher itself is the target.
typedef struct EDListener EDListener;
typedef struct Avm2EventDispatcherExt
{
	Avm2Value ed_target;
	EDListener* head;
} Avm2EventDispatcherExt;

// flash.events.Event instance state (Ruffle events.rs Event struct).
// Stage 8: the same struct also carries MouseEvent/KeyboardEvent/FocusEvent
// subclass fields (the display-ext "one struct for the whole ladder" pattern);
// Event subclasses inherit native_ext_size, so the base size below suffices.
typedef struct Avm2EventExt
{
	const Avm2String* type;      // NULL until init
	Avm2Object* target;
	Avm2Object* current_target;
	uint8_t bubbles;
	uint8_t cancelable;
	uint8_t cancelled;
	uint8_t propagation;         // 0 allow, 1 stop, 2 stop-immediate
	uint8_t phase;               // 1 capturing, 2 at-target, 3 bubbling

	// --- MouseEvent / KeyboardEvent / FocusEvent shared modifier flags ---
	uint8_t ctrl_key;
	uint8_t alt_key;
	uint8_t shift_key;
	uint8_t control_key;         // KeyboardEvent controlKey (Mac ctrl)
	uint8_t command_key;         // KeyboardEvent commandKey (Mac cmd)
	uint8_t button_down;         // MouseEvent buttonDown
	Avm2Object* related_object;  // MouseEvent/FocusEvent relatedObject
	// --- MouseEvent ---
	double local_x, local_y;     // NaN = unset
	int32_t delta;
	int32_t movement_x, movement_y;
	// --- KeyboardEvent ---
	uint32_t char_code;
	uint32_t key_code;
	uint32_t key_location;
	// --- FocusEvent ---
	const Avm2String* direction; // NULL = "none"
	uint8_t related_object_inaccessible;
	// --- TextEvent / ErrorEvent (text = error message) ---
	const Avm2String* text;
	// --- ProgressEvent ---
	double bytes_loaded, bytes_total;
	// --- ErrorEvent family ---
	int32_t error_id;
	// --- HTTPStatusEvent ---
	int32_t http_status;
	uint8_t redirected;
	const Avm2String* response_url;
	// --- StatusEvent ---
	const Avm2String* status_code;
	const Avm2String* status_level;
	// --- NetStatusEvent ---
	// The `info` bag. Reached by the GC through the conservative native_ext
	// scan, like every other Avm2Object* in this struct.
	Avm2Object* info;
	// --- AsyncErrorEvent ---
	// `error`, the Error instance the failed callback threw. AsyncErrorEvent's
	// 5th constructor argument is an Error OBJECT, not the errorID its
	// ErrorEvent base takes.
	Avm2Object* error_obj;
	// --- ContextMenuEvent ---
	// `mouseTarget` reuses `related_object` above (both are the InteractiveObject
	// the pointer was over); `contextMenuOwner` is the menu's own owner and needs
	// its own slot.
	Avm2Object* context_menu_owner;
} Avm2EventExt;

// DisplayObject instance state (avm2_display.c). One struct serves the
// whole display ladder (DisplayObject..MovieClip/Stage): per-class size
// juggling is not worth the bytes. The dispatcher ext MUST stay first:
// display classes inherit EventDispatcher's natives, which read native_ext
// as Avm2EventDispatcherExt.
typedef struct Avm2DepthEntry
{
	int32_t depth;
	Avm2Object* child;
} Avm2DepthEntry;

typedef struct Avm2DisplayObjectExt
{
	Avm2EventDispatcherExt dispatcher;

	// --- DisplayObject core ---
	Avm2Object* parent;          // display parent (NULL = not on a tree)
	const Avm2String* name;      // NULL only for the Stage
	uint8_t has_explicit_name;
	uint8_t parent_field_done;   // set_on_parent_field already ran (buttons
	                             // set the field BEFORE construction)
	uint8_t manual_frame_construct; // placed while the parent was still
	                             // unconstructed: ONLY Sprite.constructChildren
	                             // may construct it (Ruffle MANUAL_FRAME_CONSTRUCT
	                             // — movieclip_frameconstruct_skipped)
	uint8_t instantiated_by_timeline;
	uint8_t placed_by_avm2_script;
	uint8_t is_root;
	uint8_t is_stage;
	uint8_t visible;             // default 1
	uint8_t constructed;         // AVM2 constructor has run
	uint8_t skip_next_enter_frame;
	// Sprite.constructChildren is iterating this container (Ruffle
	// RUNNING_CONSTRUCT_FRAME): nested construct_frame passes skip its
	// still-unconstructed children (constructors_vs_timeline).
	uint8_t running_construct_frame;
	// Ruffle DisplayObjectFlags::TRANSFORMED_BY_SCRIPT. Sticky, never cleared:
	// once AS writes a transform attribute (x/y/rotation/scaleX/scaleY/alpha,
	// Transform.matrix=, Transform.colorTransform=, an EditText's width/height),
	// timeline PlaceObject tags stop applying to this object at all
	// (apply_place_object, Ruffle display_object.rs:2514).
	uint8_t transformed_by_script;
	uint16_t char_id;            // 0 = script-created (or root)
	// Renderable shape geometry (solid/stroke/gradient fills), resolved from
	// char_id at place-time (avm2_generated_shape_geom). shape_vert_count == 0
	// means "no renderable shape to draw" (script-created, sprite, bitmap-fill
	// shape, or unresolved) — the render walk then draws nothing for this node.
	uint32_t shape_vert_offset;
	uint32_t shape_vert_count;
	// T6 morph (DefineMorphShape): is_morph_shape routes this node through the
	// ratio-lerp runtime-tris path; morph_end_offset is the paired start index
	// into morph_end_shape_data; ratio (0..65535) is the placement interpolation
	// factor (op->ratio, updated each frame the timeline re-places the morph).
	uint32_t morph_end_offset;
	uint8_t  is_morph_shape;
	uint16_t ratio;
	int32_t depth;               // timeline depth
	int32_t clip_depth;
	int32_t place_frame;
	// Matrix: scale/rot f32, translate twips (Ruffle Matrix layout).
	float mtx_a, mtx_b, mtx_c, mtx_d;
	int32_t mtx_tx, mtx_ty;
	// Decomposed transform cache (Ruffle DisplayObjectBase).
	uint8_t scale_rot_cached;
	double rotation_deg;         // raw degrees (may be NaN/Inf)
	double scale_x, scale_y;     // unit values (may be NaN)
	double skew;                 // radians
	int16_t alpha_fixed8;        // 8.8 fixed a_multiply; default 256
	// The rest of the ColorTransform (Ruffle layout: Fixed8 multipliers,
	// i16 addends). aMultiplier lives in alpha_fixed8 above, which is what
	// keeps DisplayObject.alpha and transform.colorTransform.alphaMultiplier
	// the same storage. Defaults: multipliers 256, addends 0.
	int16_t cx_rm, cx_gm, cx_bm;
	int16_t cx_ra, cx_ga, cx_ba, cx_aa;
	// InteractiveObject.
	uint8_t mouse_enabled;       // default 1
	uint8_t double_click_enabled;
	uint8_t tab_enabled_set, tab_enabled_val;
	int32_t tab_index;           // -1 = unset
	uint8_t focus_rect_set, focus_rect_val;
	Avm2Value meta_data;         // metaData (undefined = unset)
	// scrollRect (whole-pixel-rounded twips; see avm2_display.c). When set it
	// REPLACES this object's bounds with a box of the rect's size at the
	// object's own origin.
	uint8_t has_scroll_rect;
	int32_t sr_xmin, sr_ymin, sr_xmax, sr_ymax;
	Avm2Object* mask;            // Ruffle masker(): the object masking THIS one
	// Ruffle maskee(): the object THIS one masks. Kept in sync by `set mask`.
	// A maskee is never itself a mouse target and is skipped by the pick walk.
	Avm2Object* maskee;

	// Drawing API (flash.display.Graphics stub): cached instance +
	// accumulated AABB of drawn geometry (twips), feeding self bounds.
	Avm2Object* graphics_obj;
	uint8_t draw_valid;
	int32_t draw_xmin, draw_xmax, draw_ymin, draw_ymax;
	// Ruffle Drawing keeps TWO boxes (core/src/drawing.rs): `shape_bounds`
	// grows by the active stroke's half-width, `edge_bounds` takes the raw
	// path point. draw_* above is the shape (stroke-inclusive) box that
	// getBounds / width / height / hitTestObject / pixelBounds read
	// (BoundsMode::Script); the edge_* box below is what AVM2
	// DisplayObject.getRect reads (BoundsMode::ScriptWithoutStrokes).
	uint8_t draw_edge_valid;
	int32_t draw_exmin, draw_exmax, draw_eymin, draw_eymax;

	// --- DisplayObjectContainer ---
	Avm2Object** render_list;
	uint32_t render_len, render_cap;
	Avm2DepthEntry* depth_list;  // sorted ascending by depth
	uint32_t depth_len, depth_cap;
	uint8_t mouse_children;      // default 1
	uint8_t tab_children;        // default 1
	uint8_t tab_children_set;    // tabChildren explicitly assigned
	uint8_t button_mode;         // Sprite.buttonMode
	uint8_t use_hand_cursor;     // Sprite.useHandCursor (default 1)

	// --- MovieClip / timeline ---
	const Avm2TimelineData* timeline;  // NULL = no timeline (plain Sprite etc.)
	uint16_t current_frame;      // 1-based; 0 = before frame 1
	uint8_t playing;             // default 1 for timeline clips
	uint8_t programmatically_played;
	uint8_t executing_frame_script;
	uint8_t loop_queued;
	uint8_t initialized;
	int32_t queued_goto_frame;   // -1 = none
	// The play/stop half of a queued goto (Ruffle GotoInfo.stop_or_play,
	// movie_clip.rs 7e8e2de8a): 1 = gotoAndStop, 0 = gotoAndPlay. Only
	// meaningful while queued_goto_frame >= 0.
	uint8_t queued_goto_stop;
	uint16_t queued_script_frame;
	int32_t last_queued_script_frame;  // -1 = none
	uint8_t has_pending_script;
	// Queued AS3 place/remove tags for this tick (per-depth, Ruffle
	// QueuedTagList): index into the current frame's op array, or -1.
	int32_t* queued_places;      // [frame op count] worth of op indices
	uint32_t queued_place_count;
	uint32_t frame_script_cap;
	Avm2Value* frame_scripts;    // indexed by 0-based frame; unset = undefined kind
	// Catch-up walk gate: 1 = this node AND its whole subtree are known
	// quiescent, so the construct / frame-script walks can skip them.
	// Zero-init means "must walk", so anything unaccounted for is walked
	// (fail-safe). Cleared up the ancestor chain by display_mark_frame_work
	// whenever a node acquires frame work; re-set only at the end of a
	// frame-script walk that found nothing to do. See avm2_display.c
	// "Catch-up walk gate".
	uint8_t walk_clean;
	// Number of direct children (render list + button states) that are not
	// walk_clean. Zero lets the walks skip the child loop outright, which is
	// what keeps a container with thousands of quiescent children cheap.
	// Only ever read as "is it zero"; it is recounted exactly at the end of
	// each frame-script walk, so an over-count merely costs one child scan.
	uint32_t dirty_kids;
	// Membership flag for the global orphan list (avm2_display.c), replacing
	// a linear scan on every insert.
	uint8_t in_orphan_list;

	// --- SimpleButton ---
	Avm2Object* btn_up;
	Avm2Object* btn_over;
	Avm2Object* btn_down;
	Avm2Object* btn_hit;
	uint8_t btn_states_created;
	uint8_t btn_weird_order;  // one-shot framescript order after construction
	// Ruffle avm2_button.rs `state: Cell<ButtonState>` — which state child is
	// currently painted / measured. 0 = Up (the memset-0 default, and the only
	// value a trace run without mouse input ever sees), 1 = Over, 2 = Down.
	uint8_t btn_state;

	// --- Bitmap (flash.display.Bitmap; avm2_bitmap.c) ---
	Avm2Object* bitmap_data;         // the BitmapData object (NULL = none)
	const Avm2String* pixel_snapping;  // NULL = default "auto"
	uint8_t smoothing;
	uint8_t is_bitmap;               // this display object is a flash.display.Bitmap
	// Cached width/height (pixels) captured when bitmapData is assigned —
	// Ruffle Bitmap keeps these even after the BitmapData is disposed.
	uint32_t bitmap_w, bitmap_h;

	// --- TextField ---
	const Avm2String* tf_text;   // NULL = default "" (mirror of edittext->text)
	// TextField/EditText engine state (avm2_text.c; NULL for non-TextFields).
	struct Avm2EditTextExt* edittext;
	// Static text (DefineText/2 -> StaticText): resolved glyph-placement range
	// from char_id at place-time (avm2_generated_statictexts). NULL for anything
	// that isn't a placed static-text character. The render walks feed it through
	// the same glyph raster/tessellation as edittext.
	const struct Avm2StaticTextData* statictext;
	// accessibilityImplementation/accessibilityProperties (stored verbatim).
	Avm2Value accessibility_impl;
	Avm2Value accessibility_props;

	// soundTransform: core i32×100 {l2l,l2r,r2l,r2r,volume}; unset = default.
	int32_t sound_transform[5];
	uint8_t sound_transform_set;
	// The LoaderInfo this object's MOVIE was loaded through (loader-arc
	// tranche 6). Set on a Loader-loaded child's ROOT only; every other
	// object reaches it by walking to its root, which is what makes
	// `loader.contentLoaderInfo === loader.content.loaderInfo` true.
	// NULL = the main movie, whose objects answer g_root_loader_info.
	Avm2Object* loader_info;
	// The ApplicationDomain of the movie INSTANCE this object's character came
	// from — Ruffle's DisplayObject::movie() -> library_for_movie(movie)
	// .avm2_domain(), which is where preload_symbol_class resolves the
	// SymbolClass names of every character that movie places. Set on a
	// Loader-loaded root (its LoaderInfo's scope) and inherited by each
	// timeline child from the parent that placed it. It cannot be derived
	// from the character's tables: the same SWF loaded twice shares one
	// Avm2MovieTables but loads into two domains (loader_duplicate_class).
	// NULL = the main movie (the root scope).
	const Avm2DomainScope* movie_scope;

	// DisplayObject.filters (filters arc F1). Stored as VALUES, never as the
	// AS objects the script handed us — which is what makes mutating an
	// assigned filter afterwards a no-op, and every getter call hand back
	// brand-new objects. A non-empty list also forces cacheAsBitmap on.
	struct Avm2FilterVal* filters;
	uint32_t filter_count;

	// DisplayObject.opaqueBackground (s16 P2). Ruffle display_object.rs
	// set_opaque_background: alpha is FORCED to 255 ("Only solid backgrounds
	// are supported"), and render_base paints a rect of the object's WORLD
	// render bounds beneath the object — outside its own mask/scrollRect.
	// `opaque_bg_set == 0` reads back as `null`, never `undefined`.
	uint8_t opaque_bg_set;
	uint32_t opaque_bg_rgb;      // 0xRRGGBB, alpha implicit 255

	// COMMITTED scrollRect. Ruffle keeps two rects per object: `next_scroll_
	// rect`, which is what AS just wrote and what the `scrollRect` getter
	// reports, and `scroll_rect`, which is what bounds, hit tests and the
	// local<->global matrices actually read. `DisplayObject::pre_render`
	// copies one to the other once per frame, just before the object is drawn
	// (display_object.rs:2426-2430) — so a rect assigned mid-frame does not
	// affect geometry until the next render. `has_scroll_rect` + `sr_*` above
	// are the `next` half; these are the committed half. Latched from the
	// frame loop (avm2_commit_scroll_rects), never from the renderer, so
	// NO_GRAPHICS and graphics builds agree.
	uint8_t sr_committed;
	int32_t csr_xmin, csr_ymin, csr_xmax, csr_ymax;

	// DisplayObject.blendMode as the SWF NUMERIC id (swf/src/types.rs
	// BlendMode; 0/1 = normal), cached at SET time so the render walk never
	// re-parses the `__blendMode` string it reads back. Written by
	// set_blend_mode_name() (PlaceObject3 + the goto rewind default) and by the
	// AS blendMode / blendShader setters; read only by avm2_render_node's blend
	// arm. "shader" has no SWF id and no backend, so it caches 0.
	uint8_t blend_mode_id;
} Avm2DisplayObjectExt;

// Compatibility alias: MovieClip state is the shared display ext.
typedef Avm2DisplayObjectExt Avm2MovieClipExt;

// Ruffle avm2_button.rs `get_state_child(self.state())`: the ONE child a
// SimpleButton paints / measures for its current state. NULL when that state
// has no records — Ruffle does NOT fall back to Up, and never paints the hit
// area. A SimpleButton is deliberately not a container (`render_len` is always
// 0 on one), so every walk that wants to see through a button has to come
// here; and the answer must be keyed off these fields, never off the child's
// `parent`, because assigning the same Shape to two state slots leaves its
// `parent` NULL while `btn_up` still points at it.
// Returns NULL on every non-button (btn_state is 0 and btn_up is NULL), so
// call sites need no class check — the cost is one load and one branch.
static inline Avm2Object* avm2_button_state_child(const Avm2DisplayObjectExt* ext)
{
	switch (ext->btn_state)
	{
		case 1:  return ext->btn_over;
		case 2:  return ext->btn_down;
		default: return ext->btn_up;
	}
}

// flash.events registration (avm2_events.c): Event (real internal state,
// clone/formatToString/preventDefault family), EventDispatcher (3-phase
// dispatch, priority buckets, capture split, broadcast registry),
// EventPhase, IEventDispatcher.
void avm2_register_events(Avm2Context* ctx);
// NULL when the value/object is not an Event / EventDispatcher-descendant.
Avm2EventExt* avm2_event_ext_of(Avm2Value v);
Avm2EventDispatcherExt* avm2_dispatcher_ext_of(Avm2Context* ctx, Avm2Object* obj);
// New base flash.events.Event instance (type interned by the caller).
Avm2Object* avm2_event_new(Avm2Context* ctx, const Avm2String* type,
                           int bubbles, int cancelable);
// Full 3-phase dispatch on a dispatcher object (Ruffle events.rs
// dispatch_event). Returns 1 if the event was handled (some listener ran).
int avm2_dispatch_event(Avm2Context* ctx, Avm2Object* dispatcher, Avm2Object* event);
// FP broadcast semantics: deliver to every dispatcher registered for this
// event type (registration order), no capture/bubble, ignoring propagation
// stops between targets. `filter_class` NULL = no class filter.
void avm2_broadcast_event(Avm2Context* ctx, Avm2Object* event, Avm2Class* filter_class);
// Stage 8: construct a MouseEvent/KeyboardEvent/FocusEvent with its native
// state filled directly (bypassing the AS3 ctor arg dance). `type` interned by
// the caller. The returned object is a fully-formed Event subclass instance.
Avm2Object* avm2_mouse_event_new(Avm2Context* ctx, const Avm2String* type,
                                 int bubbles, int cancelable, double local_x,
                                 double local_y, Avm2Object* related,
                                 int shift, int ctrl, int alt, int button_down,
                                 int32_t delta);
Avm2Object* avm2_keyboard_event_new(Avm2Context* ctx, const Avm2String* type,
                                    int bubbles, int cancelable,
                                    uint32_t char_code, uint32_t key_code,
                                    uint32_t key_location, int ctrl, int alt,
                                    int shift);
Avm2Object* avm2_focus_event_new(Avm2Context* ctx, const Avm2String* type,
                                 int bubbles, int cancelable,
                                 Avm2Object* related, int shift,
                                 uint32_t key_code, const char* direction);
// flash.events.TextEvent (Stage 8: TextField textInput). Returns the event;
// dispatch it and read ->cancelled to honor preventDefault.
Avm2Object* avm2_text_event_new(Avm2Context* ctx, const Avm2String* type,
                                int bubbles, int cancelable,
                                const Avm2String* text);
// flash.events.DataEvent (extends TextEvent) — one per NUL-delimited inbound
// XMLSocket frame. `data` is TextEvent's `text` under another name.
Avm2Object* avm2_data_event_new(Avm2Context* ctx, const Avm2String* type,
                                int bubbles, int cancelable,
                                const Avm2String* data);
// flash.events.ProgressEvent / IOErrorEvent / HTTPStatusEvent, dispatched from
// C by the Loader and URLLoader pipelines (avm2_display.c). All are
// non-bubbling and non-cancelable.
Avm2Object* avm2_progress_event_new(Avm2Context* ctx, const Avm2String* type,
                                    double bytes_loaded, double bytes_total);
Avm2Object* avm2_io_error_event_new(Avm2Context* ctx, const Avm2String* type,
                                    const Avm2String* text, int32_t error_id);
Avm2Object* avm2_http_status_event_new(Avm2Context* ctx, const Avm2String* type,
                                       int32_t status, int redirected);
// flash.events.NetStatusEvent("netStatus"), dispatched from C by the
// NetConnection state machine (avm2_net.c). `keys`/`values` build the `info`
// bag in order; both arrays hold `count` NUL-terminated C strings.
Avm2Object* avm2_net_status_event_new(Avm2Context* ctx,
                                      const char* const* keys,
                                      const char* const* values, int count);
// flash.events.StatusEvent("status"), dispatched from C by LocalConnection.
// `code` NULL means the null Flash itself passes there.
Avm2Object* avm2_status_event_new(Avm2Context* ctx, const Avm2String* code,
                                  const char* level);
// flash.events.AsyncErrorEvent("asyncError") — `error` is the thrown Error.
Avm2Object* avm2_async_error_event_new(Avm2Context* ctx, const Avm2String* text,
                                       Avm2Object* error_obj);
// Was the event's default prevented (cancelled)? NULL-safe.
int avm2_event_is_cancelled(Avm2Object* event);
// Display parent hook used for ancestor walks; reads the display ext.
Avm2Object* avm2_display_parent(Avm2Context* ctx, Avm2Object* obj);

// flash.utils.Timer / flash.events.TimerEvent (Stage 10). The Timer class +
// the shared setTimeout/setInterval/Timer priority list live in avm2_display.c
// (co-located with the tick); TimerEvent lives in avm2_events.c.
void avm2_register_timer_class(Avm2Context* ctx);   // avm2_display.c
Avm2Object* avm2_timer_event_new(Avm2Context* ctx, const Avm2String* type,
                                 int bubbles, int cancelable);
// getTimer() elapsed-time value (ms since start), read off the timer clock.
double avm2_timer_elapsed_ms(void);

// flash.net transport classes (avm2_net.c): Socket, NetConnection, NetStream,
// Responder, NetStreamPlayOptions, DatagramSocket, AVNetworkingParams. Runs
// after avm2_register_events, like register_net — three of these extend
// EventDispatcher and builtin classes snapshot their parent vtable at
// creation time.
void avm2_register_net_transport(Avm2Context* ctx);

// flash.display.Stage3D + the flash.display3D family + flash.geom.Matrix3D
// (avm2_stage3d.c). Runs AFTER avm2_register_display: it hangs the `stage3Ds`
// getter off the Stage class, and its Matrix3D replaces the shell that used to
// be minted in register_display.
void avm2_register_stage3d(Avm2Context* ctx);
// The four Stage3D singletons (created lazily, pinned, stable identity).
Avm2Object* avm2_stage3d_at(Avm2Context* ctx, uint32_t index);
// Ruffle Stage::check_requested_context3ds (frame_lifecycle.rs:104): create the
// Context3D for any Stage3D that asked for one this frame and dispatch
// `context3DCreate`. Called from avm2_display_run_tick at the END of the frame,
// after the exitFrame broadcast and the LoaderInfo exit-frame hook.
void avm2_stage3d_check_requested(Avm2Context* ctx);
// flash.geom.Vector3D, owned by avm2_display.c (see the note there).
Avm2Class* avm2_geom_vector3d_class(void);
// PlaceObject3 SurfaceFilterList -> a display object's stored filter list.
struct Avm2TagFilter;
void avm2_display_apply_tag_filters(Avm2Context* ctx, Avm2Object* obj,
                                    const struct Avm2TagFilter* tags, uint32_t count);
// PixelBender: the PBJ parser + flash.display Shader/ShaderData/
// ShaderParameter/ShaderInput/ShaderJob + flash.filters.ShaderFilter
// (avm2_pixelbender.c, shader/3d arc tranche P1). Runs AFTER
// avm2_register_filters (ShaderFilter extends its BitmapFilter base).
void avm2_register_pixelbender(Avm2Context* ctx);
// flash.filters (avm2_filters.h): the nine filter classes, the three constant
// bags, and the .filters conversion layer. avm2_filters.h also declares
// avm2_filters_bitmapfilter_class(), the accessor ShaderFilter shares.
// DisplayObject.blendShader's two gates (display_object.rs set_blend_shader):
// 0 = not a Shader at all, 1 = a Shader with a null `data` (#2007), 2 = usable.
int avm2_shader_blend_state(Avm2Value v);
// Drain every NetConnection's queued call()s into one Flash Remoting packet per
// connection, log the fetch, and dispatch the scripted response. Called once per
// tick from avm2_display_run_tick at the loader/executor drain point.
void avm2_net_flush_connections(Avm2Context* ctx);
// Deliver this tick's queued LocalConnection messages (sender StatusEvent, then
// the callee's method, then an AsyncErrorEvent on the receiver if it threw).
// Called from avm2_display_run_tick right after the frame's AVM2 phases.
void avm2_net_deliver_local_connections(Avm2Context* ctx);
// Deliver the onMetaData callback queued by NetStream.play() on a bundled FLV
// (the script-data half of playback — see avm2_net.c's NetStream section).
// Called from the same per-tick drain point as the NetConnection flush.
void avm2_net_deliver_netstream_meta(Avm2Context* ctx);
// The movie's LocalConnection domain (avm2_globals.c owns the URL parsing).
const char* avm2_local_connection_domain(void);

// flash.media Sound family (avm2_media.c — Stage 10).
void avm2_register_media(Avm2Context* ctx);
// Build an AS3 SoundTransform object from a core i32×100 transform, and read
// one back into a 5-slot core array {l2l,l2r,r2l,r2r,volume}. Used by
// SoundChannel/SoundMixer and DisplayObject.soundTransform. read returns 0 if
// `v` is not a SoundTransform.
Avm2Value avm2_sound_transform_from_core(Avm2Context* ctx, const int32_t core[5]);
int avm2_sound_transform_read(Avm2Context* ctx, Avm2Value v, int32_t out[5]);
// Mixer bridge (real audio output; both are no-ops under NO_GRAPHICS):
// register embedded DefineSound payloads with the shared audio mixer at boot,
// and per-tick dispatch Event.SOUND_COMPLETE for drained playbacks.
void avm2_media_register_sounds(Avm2Context* ctx);
void avm2_media_poll(Avm2Context* ctx);
// Registers the global SoundMixer `soundTransform` accessor pair as an
// INSTANCE property of `cls` — SimpleButton's `soundTransform` is the mixer's
// (see avm2_media.c). Called from avm2_display.c's SimpleButton class build.
void avm2_media_register_mixer_transform(Avm2Context* ctx, Avm2Class* cls);

// flash.text module (avm2_text.c — Stage 6): TextFormat/TextField engine.
void avm2_register_text(Avm2Context* ctx);
// Adds the full TextField property surface to the class shell created by
// avm2_display.c.
void avm2_text_init_textfield_class(Avm2Context* ctx, Avm2Class* textfield);
// flash.text.engine.TextLine instance state. A TextLine is a real
// DisplayObjectContainer — textline_name grades `name == "instance1"`, so the
// line takes a number from the shared instance counter — hence the display ext
// leads the blob, Loader-style. Everything here is inside the GC's
// conservative native_ext scan, so the object pointers need no extra marking.
typedef struct Avm2TextLineExt
{
	Avm2DisplayObjectExt display;   // extends DisplayObjectContainer (MUST be first)
	Avm2Object* text_block;
	Avm2Object* previous_line;
	Avm2Object* next_line;
	Avm2Object* block_chain;        // next line OF THE SAME BLOCK (invalidation walk)
	Avm2Value user_data;
	const Avm2String* validity;
	double specified_width;
	uint32_t raw_text_length;       // THIS line's substring, in UTF-16 units
	uint32_t begin_index;           // UTF-16 code-unit offset into the block text
	uint32_t end_index;             // UTF-16 code-unit offset into the block text
	uint32_t line_index;
	// validity = "static" hides the block from SCRIPT while the line stays in
	// the block's chain (still walkable, still releasable) — see avm2_text.c.
	uint8_t hide_block_from_script;
} Avm2TextLineExt;
// Wire the FTE surface onto the TextLine class shell (created by
// avm2_register_display so it gets the concrete display alloc hook).
void avm2_text_init_textline_class(Avm2Context* ctx, Avm2Class* textline);
// Alloc hook half for script-created TextFields (`new TextField()`).
void avm2_text_edittext_init(Avm2Context* ctx, Avm2Object* obj);
// Timeline instantiation: seed EditText state from the DefineEditText tag.
void avm2_text_seed_from_tag(Avm2Context* ctx, Avm2Object* obj, uint16_t char_id);
// EditText-owned bounds accessors (TextField width/height/x/y semantics —
// only valid when ext->edittext != NULL).
double avm2_text_get_width_px(Avm2Context* ctx, Avm2Object* obj);
double avm2_text_get_height_px(Avm2Context* ctx, Avm2Object* obj);
void avm2_text_set_width_px(Avm2Context* ctx, Avm2Object* obj, double value);
void avm2_text_set_height_px(Avm2Context* ctx, Avm2Object* obj, double value);
int32_t avm2_text_bounds_x_offset(Avm2Context* ctx, Avm2Object* obj, double scale_x);
void avm2_text_apply_pending_bounds(Avm2Context* ctx, Avm2Object* obj);
int32_t avm2_text_bounds_y_offset(Avm2Context* ctx, Avm2Object* obj, double scale_y);

// flash.display.BitmapData / Bitmap (avm2_bitmap.c — Stage 7).
void avm2_register_bitmap(Avm2Context* ctx);
// Wire the Bitmap class shell (created by avm2_register_display, so it gets
// display_native_init): adds the ctor + bitmapData/pixelSnapping/smoothing
// accessors. Called from avm2_register_display.
void avm2_bitmap_wire_bitmap(Avm2Context* ctx, Avm2Class* bitmap_cls);
// BitmapData instance state. Pixels are premultiplied ARGB, one uint32 per
// pixel in 0xAARRGGBB form (the value AS3 reads/writes), row-major.
typedef struct Avm2BitmapDataExt
{
	uint32_t width, height;
	uint8_t transparency;
	uint8_t disposed;
	uint32_t* pixels;  // width*height entries; NULL when 0-size / disposed
} Avm2BitmapDataExt;
// NULL when the value is not a BitmapData (subclasses included).
Avm2BitmapDataExt* avm2_bitmapdata_ext_of(Avm2Context* ctx, Avm2Value v);
// Bitmap self-bounds hook for avm2_display.c: 1 + w/h (pixels) if obj is a
// Bitmap with a live bitmapData, else 0.
int avm2_bitmap_self_dims(Avm2Context* ctx, Avm2Object* obj, uint32_t* w, uint32_t* h);
// Timeline placement of a bitmap character: seed `child` (a plain Bitmap
// display object) with a bitmapData. `bd_class` (a BitmapData subclass bound
// to the char, or NULL) is constructed with (1,1) — running its user ctor —
// else a plain BitmapData is seeded from the embedded asset for char_id.
void avm2_bitmap_seed_timeline(Avm2Context* ctx, Avm2Object* child,
                               uint16_t char_id, Avm2Class* bd_class);
// Decode PNG/JPEG/GIF bytes (stb_image) into a fresh Bitmap display object
// wrapping a BitmapData — flash.display.Loader's image content. `transparent`
// comes from the source channel count. A malformed-but-self-describing buffer
// yields an all-transparent bitmap at the header's size (Ruffle's behaviour);
// NULL means even the header was unreadable.
Avm2Object* avm2_bitmap_from_image_bytes(Avm2Context* ctx, const uint8_t* data,
                                         uint32_t len);

// Display module (avm2_display.c — Stage-5 tranche 2+).
void avm2_register_display(Avm2Context* ctx);
// Wire flash.net.URLLoader's ctor/methods/accessors. The class shell is created
// with the rest of flash.net in avm2_globals.c, but its load pipeline shares
// Loader's URL resolution, event dispatch and per-tick drain in avm2_display.c.
void avm2_display_wire_url_loader(Avm2Context* ctx, Avm2Class* ul);
// flash.events.UncaughtErrorEvents (avm2_events.c), or NULL below SWF 10 /
// before avm2_register_events has run. LoaderInfo.uncaughtErrorEvents mints
// one instance per LoaderInfo off it.
Avm2Class* avm2_uncaught_error_events_class(void);
// flash.events.FullScreenEvent (a shell class registered with the other 44),
// kept so Stage.displayState can dispatch a real instance of it.
Avm2Class* avm2_full_screen_event_class(void);
// flash.display.IBitmapDrawable, created with DisplayObject (its first
// implementor). avm2_register_bitmap runs later and declares it on BitmapData,
// the only other implementor. NULL before avm2_register_display has run.
Avm2Class* avm2_display_ibitmapdrawable(void);
void avm2_display_wire_url_stream(Avm2Context* ctx, Avm2Class* us);
// flash.net.URLVariables, or NULL before register_net has run. The URLLoader
// "variables" data format constructs one off the response body.
Avm2Class* avm2_url_variables_class(void);
// NULL when obj is not a DisplayObject descendant.
Avm2DisplayObjectExt* avm2_display_ext_of(Avm2Context* ctx, Avm2Object* obj);
// Build a flash.text.engine.TextLine. The ONLY sanctioned path: script
// `new TextLine()` is #2012 (the class is [Ruffle(Abstract)]), and going
// through the normal display allocation is what makes the line consume an
// instanceN name.
Avm2Object* avm2_display_new_textline(Avm2Context* ctx);
// T4 Part B — CPU-composite a node's recorded flash.display.Graphics geometry
// into a premultiplied-ARGB target (`w*` = shape-local twips -> target twips).
// No-op if the node has no recorded geometry. Used by BitmapData.draw (getPixel).
void avm2_graphics_cpu_composite(Avm2Context* ctx, Avm2Object* obj,
                                 double wa, double wb, double wc, double wd,
                                 double wtx, double wty, double alpha,
                                 uint32_t* buf, int W, int H, int transparent);
// SymbolClass char bound to a class (walks the class hierarchy); 0 = none.
uint16_t avm2_display_char_for_class(Avm2Class* cls);
// Same, but also across CHILD movies (whose SymbolClass rows never enter the
// main symbol map). Font.registerFont's lookup.
uint16_t avm2_display_child_char_for_class(Avm2Context* ctx, Avm2Class* cls);
// Build the stage + root (SymbolClass char 0 / bound placed symbols) and
// remember them on ctx. Called from runSWF_avm2 after script eager-init.
void avm2_display_build_stage(Avm2Context* ctx, const char* root_class_name);
// The CHILD movies loaded through flash.display.Loader, in load order. The
// MAIN movie is never in this list — it stays on the avm2_generated_* globals.
uint32_t avm2_display_child_movie_count(void);
const Avm2MovieTables* avm2_display_child_movie(uint32_t i);
// Which movie does this ABC file belong to? NULL = the MAIN movie (Ruffle
// Activation::caller_movie_or_root()'s `or_root` arm).
const Avm2MovieTables* avm2_display_movie_for_abc(const Avm2AbcFileData* f);
// Does the movie DEFINE this character? (Ruffle library.character_by_id.)
// A SymbolClass binding to an id that names nothing binds the ROOT class.
int avm2_display_char_is_defined(uint16_t char_id);
// The flat Avm2StaticGlyph table a DefineText character's glyph_start indexes:
// the DEFINING movie's, or avm2_generated_static_glyphs for the main movie.
struct Avm2StaticGlyph;
struct Avm2StaticTextData;
const struct Avm2StaticGlyph* avm2_display_static_glyphs_for(
	const struct Avm2StaticTextData* st);
// Load a CHILD movie's ABC files into the running context (loader-arc tranche
// 6): grows ctx->files, publishes each script's traits into the domain and
// eager-inits every file's last script. Idempotent per tables pointer — a
// second load of the same child re-uses the already-loaded scripts.
void avm2_abc_register_movie(Avm2Context* ctx, const Avm2MovieTables* tables,
                             const Avm2DomainScope* scope);
// One full frame tick: Enter -> Construct -> FrameScripts -> Exit
// (Ruffle frame_lifecycle.rs run_all_phases_avm2).
void avm2_display_run_tick(Avm2Context* ctx);

// Stage 8 input harness (avm2_display.c): parse input_events.txt (main.c) and
// pump one WAIT group per tick (called at the tail of avm2_display_run_tick).
void avm2_input_load(const char* path);
void avm2_input_pump_tick(Avm2Context* ctx);
// Live browser key injection (Stage 13c) — called from the emscripten keyboard
// callbacks to enqueue a keyDown/keyUp drained by avm2_input_pump_tick.
void avm2_input_inject_key(int is_down, int32_t key_code,
                           int32_t char_code, int32_t key_location);
// Live browser mouse injection (RWK-3) — same ring. kind 0=move/1=down/2=up,
// x/y stage pixels, button 0/1/2, click_count = DOM detail (>=2 → doubleClick).
void avm2_input_inject_mouse(int kind, float x, float y, int button,
                             int click_count);

// Text-editing bridge (avm2_text.c): route a physical key / typed char /
// text-control command to the focused TextField's EditText engine. `focus` may
// be NULL or a non-TextField (ignored). Returns nothing; a non-editable field
// still consumes selection/caret navigation keys.
void avm2_text_input_key(Avm2Context* ctx, Avm2Object* focus, int32_t key_code,
                         int32_t char_code, int shift);
void avm2_text_input_char(Avm2Context* ctx, Avm2Object* focus, int32_t codepoint);
void avm2_text_input_control(Avm2Context* ctx, Avm2Object* focus,
                             const char* ctrl, const char* clipboard);
// Mouse wheel over a TextField: scroll by `delta_lines` (Ruffle EditText
// event_dispatch(ClipEvent::MouseWheel)). Returns 1 if the field consumed it.
int avm2_text_mouse_wheel(Avm2Context* ctx, Avm2Object* obj, int32_t delta_lines);
// IME composition (Ruffle EditText::ime). `cursor_from`/`cursor_to` are UTF-8
// byte offsets into `text`, or -1 for Ruffle's `None`; an empty `text` clears
// the preedit. `commit_pending` is ensure_ime_committed, run on focus loss.
void avm2_text_ime_preedit(Avm2Context* ctx, Avm2Object* obj, const char* text,
                           int32_t cursor_from, int32_t cursor_to);
void avm2_text_ime_commit(Avm2Context* ctx, Avm2Object* obj, const char* text);
void avm2_text_ime_commit_pending(Avm2Context* ctx, Avm2Object* obj);
// EditText flag accessors for the mouse-pick path (NULL-safe on the ext ptr).
int avm2_text_is_selectable(struct Avm2EditTextExt* et);
int avm2_text_was_static(struct Avm2EditTextExt* et);
int avm2_text_is_editable(struct Avm2EditTextExt* et);

// Builtin class handles the runtime needs by identity.
typedef struct Avm2Builtins
{
	Avm2Class* object_class;
	Avm2Class* class_class;
	Avm2Class* function_class;
	Avm2Class* global_class;  // class of global objects ("[object global]")
	Avm2Class* number_class;
	Avm2Class* int_class;
	Avm2Class* uint_class;
	Avm2Class* boolean_class;
	Avm2Class* string_class;
	Avm2Class* array_class;
	Avm2Class* math_class;
	Avm2Class* error_class;
	Avm2Class* type_error_class;
	Avm2Class* reference_error_class;
	Avm2Class* argument_error_class;
	Avm2Class* range_error_class;
	Avm2Class* verify_error_class;
	Avm2Class* eval_error_class;
	Avm2Class* security_error_class;
	Avm2Class* definition_error_class;
	Avm2Class* uri_error_class;
	Avm2Class* syntax_error_class;
	Avm2Class* uninitialized_error_class;
	Avm2Class* io_error_class;
	Avm2Class* memory_error_class;
	Avm2Class* illegal_operation_error_class;
	Avm2Class* xml_class;       // stub: only typeof/is checks
	Avm2Class* xml_list_class;  // stub
	Avm2Class* movieclip_class;
	Avm2Class* event_class;
	Avm2Class* mouse_event_class;
	Avm2Class* keyboard_event_class;
	Avm2Class* focus_event_class;
	Avm2Class* text_event_class;
	// flash.events.ErrorEvent — the base of the four DRM/uncaught error
	// event shells registered in avm2_globals.c.
	Avm2Class* error_event_class;
	Avm2Class* timer_event_class;
	Avm2Class* timer_class;             // flash.utils.Timer
	Avm2Class* sound_class;             // flash.media.Sound
	Avm2Class* sound_channel_class;     // flash.media.SoundChannel
	Avm2Class* sound_transform_class;   // flash.media.SoundTransform
	Avm2Class* event_dispatcher_class;
	Avm2Class* ievent_dispatcher_class;  // interface
	Avm2Class* display_object_class;
	Avm2Class* loader_info_class;        // flash.display.LoaderInfo
	Avm2Class* loader_class;             // flash.display.Loader (stub)
	Avm2Class* interactive_object_class;
	Avm2Class* doc_class;                // DisplayObjectContainer
	Avm2Class* sprite_class;
	Avm2Class* shape_class;
	Avm2Class* bitmap_class;
	Avm2Class* bitmapdata_class;
	Avm2Class* stage_class;
	Avm2Class* simple_button_class;
	Avm2Class* regexp_class;
	Avm2Class* namespace_class;
	Avm2Class* qname_class;
	Avm2Class* dictionary_class;
	Avm2Class* proxy_class;
	Avm2Class* bytearray_class;
	Avm2Class* eof_error_class;
	Avm2Class* date_class;
	Avm2Class* vector_class;         // generic __AS3__.vec::Vector
	Avm2Class* vector_int_class;     // Vector.<int>
	Avm2Class* vector_uint_class;    // Vector.<uint>
	Avm2Class* vector_double_class;  // Vector.<Number>
	Avm2Class* vector_object_class;  // Vector.<*>
} Avm2Builtins;

#endif // AVM2_GLOBALS_H
