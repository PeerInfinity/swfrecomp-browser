#pragma once

// action_internal.h — helpers shared across subsystem source files
// ($PROJECT/src/actionmodern/*.c). Not a public API.
//
// Added as part of the action.c carve-out effort (see
// SWFRecompDocs/plans/split-action-c-math-first.md). Grows with each new
// subsystem that moves out of action.c.

#include <actionmodern/action.h>     // MovieClip, Function2Ptr, g_swf_version, VAL
#include <actionmodern/object.h>     // ASObject, ASProperty
#include <actionmodern/variables.h>  // ActionVar, ActionStackValueType

// Simple-function pointer (Function2Ptr is already in action.h)
typedef void (*SimpleFunctionPtr)(SWFAppContext* app_context);

// Function-call recursion depth. Incremented on entry to a user-defined
// function and decremented on exit. Used by EFFECTIVE_SWF_VERSION() below
// and by various SWF4/5-gated code paths.
extern u32 g_call_depth;

// Effective SWF version (accounts for function context).
// In Flash, DefineFunction (SWF5 opcode) causes code inside functions to
// behave as SWF5+ even in a SWF4 file. When inside a function call
// (g_call_depth > 0) and g_swf_version < 5, we promote to 5.
#define EFFECTIVE_SWF_VERSION() ((g_swf_version < 5 && g_call_depth > 0) ? 5 : g_swf_version)

// ------------------------------------------------------------------
// ASFunction — built-in and user-defined function object.
// Used by all subsystem files; definition lives here so every .c in
// src/actionmodern/ can reference it.
// ------------------------------------------------------------------
typedef struct ASFunction {
	char name[256];           // Function name (can be empty for anonymous)
	u8 function_type;         // 1 = simple (DefineFunction), 2 = advanced (DefineFunction2)
	u32 param_count;          // Number of parameters

	// For DefineFunction (type 1)
	SimpleFunctionPtr simple_func;

	// For DefineFunction2 (type 2)
	Function2Ptr advanced_func;
	u8 register_count;
	u16 flags;

	// Prototype object (for constructor usage)
	ASObject* prototype_obj;  // Created lazily on first access

	// Own properties on the function object itself (e.g., toString override)
	ASObject* own_props;  // Created lazily when SetMember is called

	// Captured scope chain (scope entries at time of definition)
	u8 captured_scope_count;
	ASObject* captured_scope[8];   // scope objects
	MovieClip* captured_scope_mc[8]; // associated MovieClip (if any)
	u8 captured_scope_is_with[8]; // 1 = with scope, 0 = local scope

	// Closure context: the MovieClip where this function was defined
	// SWF6+: GotoFrame/GetProperty("") operate on base_clip, not caller's context
	// SWF5: base_clip is not used (functions execute in caller's context)
	MovieClip* base_clip;

	// SWF version at function definition time (Phase 3: per-function version tracking)
	// When called, g_swf_version is switched to this value so version-dependent
	// behavior matches the SWF where the function was originally defined.
	u16 swf_version;

	// Per-movie global index: reserved for future per-movie _global isolation.
	// Currently unused (always 0). Built-in functions are zero-initialized.
	u16 movie_global_idx;

	// If set, this function does not get a lazily-created .prototype object.
	// Used for native/built-in functions that don't have prototypes in Flash.
	u8 no_lazy_prototype;

	// --- Stage 3 mark-sweep collector (memory-reclamation plan §Stage 3) ---
	// Intrusive all-heap-functions list: function_registry deliberately
	// excludes anonymous functions, so the collector traces prototype_obj/
	// own_props/captured_scope from this list instead. Heap alloc sites link
	// via actionGcLinkFunction; static/BSS builtins are reached through
	// function_registry + FUNCTION-typed values in the object graph.
	// Functions themselves stay immortal (never swept).
	struct ASFunction* gc_next;
	// Mark epoch (== current collection's epoch → already traced). Epoch-based
	// so unlisted static functions reached via graph FUNCTION values need no
	// clear pass.
	u32 gc_epoch;
} ASFunction;

// Link a heap-allocated ASFunction into the collector's all-functions list.
// Call exactly once per heap allocation, after zeroing gc_next/gc_epoch
// (calloc does; malloc+memcpy copies like createConstructorCopy must reset
// them first). Defined in action.c.
void actionGcLinkFunction(ASFunction* fn);

// ------------------------------------------------------------------
// Stack / coercion helpers (defined in action.c)
// ------------------------------------------------------------------
void pushVar(SWFAppContext* app_context, ActionVar* var);
void popVar(SWFAppContext* app_context, ActionVar* var);
ActionStackValueType convertFloat(SWFAppContext* app_context);
double varToDoubleSWF(SWFAppContext* app_context, ActionVar* v, int swf_version);

// Simple SWF5-style number coercion (no hex/octal, no valueOf call).
// Used by Date argument coercion alongside varToDoubleSWF.
double varToDoubleSimple(ActionVar* v);

// UTF-8 → malloc-allocated UTF-16 conversion. Returned pointer owns
// its buffer (freed via ActionVar refcount semantics or explicit free).
// Uses plain malloc (not the heap arena) so long-lived strings don't
// exhaust it.
uint16_t* utf8_to_u16(SWFAppContext* app_context, const char* utf8, u32 byte_len, u32* out_u16_len);

// UTF-16 → UTF-8 into caller-provided buffer. Returns bytes written
// (not including NUL). Pair of utf8_to_u16.
int u16_to_utf8(const uint16_t* u16, u32 u16_len, char* out, int out_size);

// Return the u16 character pointer for an ActionVar of STRING type
// (handles both heap-owned and ID-based storage). Returns NULL for
// non-strings or empty/zero-size strings.
const uint16_t* varGetU16Ptr(ActionVar* v);

// Push an UNDEFINED ActionVar onto the VM stack.
void pushUndefined(SWFAppContext* app_context);

// Convert the top-of-stack value to STRING in place. Used whenever a
// builtin needs to read a string from whatever the caller passed
// (including objects with toString).
ActionStackValueType convertString(SWFAppContext* app_context, char* var_str);

// Coerce any value to a UTF-8 string through the VM stack (so objects run
// their own toString) and return it in a fresh malloc'd, NUL-terminated
// buffer. Writes the byte length (excluding the NUL) to *out_len when non-NULL.
// Caller frees. Returns NULL only on allocation failure.
char* actionVarToUtf8Alloc(SWFAppContext* app_context, ActionVar* v, u32* out_len);

// Build a STRING ActionVar from UTF-8 bytes (the exported form of action.c's
// makeStringActionVar, for subsystem files).
ActionVar actionMakeStringVar(SWFAppContext* app_context, const char* utf8, u32 len);

// Build an XML document object from UTF-8 markup — the equivalent of
// `new XML(source)`. Used by the AMF0 reader for the 0x0F XML marker.
ASObject* actionCreateXmlDocument(SWFAppContext* app_context, const char* utf8, u32 len);

// Flush a deferred onChanged handler queued by replaceSel, if any.
// Timer tick hook — called from processTimers after each timer fires.
void actionFlushPendingOnChanged(SWFAppContext* app_context);

// Coerce first min(arg_count,max_args) args to f64 via pushVar/convertFloat/popVar
// (calls valueOf on objects). Used by Math builtins, ASnative, and Date.
void coerceMathArgs(SWFAppContext* app_context, ActionVar* args, u32 arg_count, u32 max_args);

// Extract f64 from a coerced ActionVar (F32 or F64). Returns NaN for others.
double mathArgToDouble(ActionVar* v);

// Build an ActionVar holding the given f64. Used by Math + Date.
ActionVar mathReturnDouble(double val);

// ------------------------------------------------------------------
// Native function setup helpers (defined in action.c)
// ------------------------------------------------------------------

// Set up own_props on a native function with __proto__ + constructor (no prototype).
void setupNativeFuncOwnProps(SWFAppContext* app_context, ASFunction* func);

// Install a builtin toString that returns "[object Object]" on a native ASObject.
void installNativeToString(SWFAppContext* app_context, ASObject* obj);

// Set __proto__ → Object.prototype on a user-created object (WRITABLE).
void setObjectProto(SWFAppContext* app_context, ASObject* obj);

// Make __proto__ READ_ONLY on an object (after setObjectProto established it).
// Used for singleton globals (Accessibility, Key, Math, Mouse, Selection, etc.)
void makeProtoReadOnly(ASObject* obj);

// Register a native ASFunction in the global function_registry so
// lookupFunctionByName() can find it. Bounds-checked; silently no-ops
// if the registry is full. Used by subsystem files (date.c, etc.)
// carved out of action.c.
void registerNativeFunction(ASFunction* fn);

// ------------------------------------------------------------------
// Call-dispatch state (exposed so subsystem files carved out of
// action.c can inline call-dispatch patterns without duplicating the
// storage or the push/pop bookkeeping).
// ------------------------------------------------------------------

// WITH scope chain + function local scope
#define MAX_SCOPE_DEPTH 32
extern ASObject* scope_chain[MAX_SCOPE_DEPTH];
extern u8 scope_is_with[MAX_SCOPE_DEPTH];
extern MovieClip* scope_mc[MAX_SCOPE_DEPTH];
extern u32 scope_depth;

// Per-call-frame `this` binding stack (GetVariable("this") checks this
// before the scope chain).
#define MAX_THIS_DEPTH 64
extern ActionVar g_this_stack[MAX_THIS_DEPTH];
extern u32 g_this_depth;

// MovieClip receiver context for clip-event handlers; consumed by
// DefineFunction2 preload_this so generated code sees MC type.
extern MovieClip* g_event_this_mc;

// MovieClip currently executing (targetClip/base_clip interaction).
extern MovieClip* g_current_context;

// The clip a SWF6+ closure call enters: its live defining clip, else the
// clip re-resolved at its path, else `this_mc` (may be NULL), else the
// caller's g_current_context (removed-clip scope rule, action.c).
MovieClip* actionClosureBaseClip(SWFAppContext* app_context, MovieClip* base_clip,
                                 MovieClip* this_mc);

// Type tag of the thisArg passed through Function.prototype.call/apply
// (ACTION_STACK_VALUE_*). Lets builtin wrappers distinguish an ASArray
// receiver (this_obj is a raw ASArray*, NOT an ASObject*) from a real
// ASObject. Builtins that cast this_obj to ASObject* must check this
// first — see builtin_array_method / date_has_backing. 0 when not in a
// call/apply dispatch.
extern u8 g_call_this_type;

// Super context stack — (this, depth, mc) triple tracks prototype
// chain position for super() / super.method() resolution.
void pushSuperContextWithMC(void* this_obj, u8 depth, void* mc);
void popSuperContext(void);

// Per-function SWF-version switch (see ASFunction::swf_version). Swaps
// g_swf_version and the active version-group global (_global legacy vs
// modern) to the function's defining-movie values; restore reverses it.
// Wrappers around action.c's static-inline helpers.
void actionSwitchToFunctionVersion(ASFunction* func, int* saved_ver, ASObject** saved_global, int* saved_movie_idx);
void actionRestoreFunctionVersion(int saved_ver, ASObject* saved_global, int saved_movie_idx);

// ---------------------------------------------------------------------------
// Unified function-invocation core (Function-Dispatch Consolidation plan).
//
// One place that performs the ~11-step invocation ritual (depth guard, this-stack
// push, super context, captured/local scope setup, base_clip/version switch,
// event-this-mc, forward-order + clamp/pad type-1 marshalling, the single type-1
// cast, type-2 register setup, and symmetric teardown). Each of ~38 hand-rolled
// dispatchers used to perform a different subset of these steps; that variance
// is the structural cause of a many-times-shipped arg-marshalling bug class.
//
// Migration is behavior-preserving: a caller passes the flag subset that mirrors
// its current ritual. Turning a skipped step on ("normalization") is a separate,
// per-site, CI-gated decision — see the plan's Stage 4.
//
// The flag macros + InvokeOpts live here (not in action.c) so subsystem files
// carved out of action.c (timer.c) can drive the core through the exported
// actionInvokeFunctionValue wrapper below — same precedent as
// actionSwitchToFunctionVersion.
#define INV_DEPTH_GUARD          0x0001u  // g_call_depth++ guard at entry (accessor style)
#define INV_THIS_STACK           0x0002u  // push this_var onto g_this_stack for the call
#define INV_SUPER_CTX            0x0004u  // pushSuperContext(this_ptr, opts->super_depth)
#define INV_CAPTURED_SCOPE       0x0008u  // restore func's captured scope chain
#define INV_LOCAL_SCOPE          0x0010u  // alloc + push a fresh local scope frame
#define INV_BIND_THIS            0x0020u  // bind "this" on the local scope (needs INV_LOCAL_SCOPE)
#define INV_BASE_CLIP            0x0040u  // SWF6+: switch g_current_context to func->base_clip
#define INV_VERSION_SWITCH       0x0080u  // switch/restore per-function SWF version + _global group
#define INV_EVENT_THIS_MC        0x0100u  // set g_event_this_mc when the receiver is a MovieClip
#define INV_EXEC_FUNC            0x0200u  // save/restore g_current_executing_func / g_prev_executing_func
#define INV_FORCE_CAPTURED_WITH  0x0400u  // force is_with=1 on captured scopes (legacy resolve/EI quirk)
#define INV_MC_THIS_NULL_PTR     0x0800u  // pass NULL (not this_ptr) to advanced_func so preload_this uses g_event_this_mc
#define INV_LOCAL_SCOPE_MC       0x1000u  // associate the receiver MC with the local scope frame (scope_mc)
#define INV_RESET_THIS_DEPTH     0x2000u  // zero g_this_depth for the call (accessor-setter isolation)
#define INV_CTOR_CTX             0x4000u  // pushCtorContext(0)/popCtorContext around the callee body
#define INV_OVERRIDE_THIS        0x8000u  // manage g_override_this{,_set} for the call (see opts->override_this)
// Special-recursion guard (Ruffle ExecutionReason::Special, activation.rs:
// special_count == 65 -> SpecialRecursionLimit -> avm1.halt()): increment
// g_special_depth; at MAX_SPECIAL_DEPTH (66) decrement, HALT all script
// execution, return undefined. Unlike the legacy g_special_depth users
// (sound/LV/unload dispatch), which return non-fatally — a preserved
// divergence — this flag matches Ruffle's halting semantics. First users:
// the coercion paths (a recursive valueOf segfaulted before this;
// regression/coerce_recursion_guard).
#define INV_SPECIAL_GUARD        0x20000u
// 0x10000u was INV_LOCAL_SCOPE_UNDER_CAPTURED (push the fresh local frame
// BENEATH the captured scopes, letting a captured scope shadow the callee's
// own params on lookup). Removed by normalization pass (b), 2026-07-11: all
// seven users (the __resolve arm, EI, the enterFrame children arm,
// onLoad/onConstruct, soundFireCallback t1, the sort comparator t1) now take
// the standard order — locals are the innermost scope, per Flash/Ruffle.
// Deliberate lock flips: regression/resolve_type1_args ("h a=one") and
// regression/ei_closure_scope_order ("shadow a=ARG"). Do not reuse the bit
// without checking gates/check_dispatch_funnel.py.

// Callee activation (opts->act_flags): how the callee's *named* locals are bound
// on the fresh local-scope frame. A type-1 body has no DefineFunction2 flags word,
// so each bit means something slightly different per function_type:
//
//   bit             type 2 (DefineFunction2)                 type 1 (DefineFunction)
//   INV_ACT_THIS    !preload && !suppress: bind "this" on     push g_this_stack only
//                   the local scope AND push g_this_stack     (no scope binding)
//   INV_ACT_ARGS    !preload && !suppress: build `arguments`  unconditional; capacity
//                   with capacity num_args                    max(num_args, 1)
//   INV_ACT_SUPER   always bind "super" (SUPER value only     no-op
//                   when !preload && !suppress && in ctx)
//
// Read off actionCallFunction's two branches and the object-method arm. The
// capacity split is preserved, not normalized — Stage 3 is behavior-preserving.
//
// The type-1 column's two "no scope binding" cells are not an approximation: a
// type-1 body cannot observe either binding. GetVariable("this") resolves from the
// this-cell before any scope walk, and GetVariable("super") falls back to the live
// super context when the chain misses — so a name binding of either is dead
// whenever INV_ACT_THIS / INV_SUPER_CTX are in force. The string-primitive arm
// used to write both anyway (it read the callee's flags word without branching on
// function_type, and a DefineFunction's flags is always 0), which is why that arm
// needs no extra flag. See regression/string_prim_method_type1_args.
#define INV_ACT_THIS             0x01u
#define INV_ACT_ARGUMENTS        0x02u
#define INV_ACT_SUPER            0x04u

typedef struct InvokeOpts {
	u32 flags;
	u16 super_depth;   // super chain depth, applied when INV_SUPER_CTX is set
	u8  act_flags;     // INV_ACT_* — callee named-local bindings (needs INV_LOCAL_SCOPE)
	u8  has_this_ptr;  // 1: pass opts->this_ptr to advanced_func instead of deriving it

	// The ABI receiver handed to advanced_func, decoupled from `this_var` (the
	// *scriptable* receiver that `this` resolves to in the callee's scope chain).
	// actionCallFunction needs both to differ: `this` binds to the current-context
	// MovieClip while advanced_func receives NULL (or global_object when a SWF6+
	// function is called from SWF5 bytecode).
	void* this_ptr;

	// With INV_OVERRIDE_THIS: non-NULL installs *override_this into g_override_this
	// (+ g_override_this_set) for the call. The flag alone still clears
	// g_override_this_set afterwards — the safety net for C-implemented callees
	// that lack the generated preload-this code which normally consumes it.
	ActionVar* override_this;

	// When non-NULL (and INV_LOCAL_SCOPE is set), the core binds *super_bind as
	// "super" on the callee's fresh local frame for BOTH function types. This is
	// the type-1 super NAME-SHADOW a dispatcher needs when its own invocation
	// frame's `super` is still live on the scope chain: unlike the dead type-1
	// this/super binds (which the chain MISSES, falling to the this-cell / live
	// super context), here the chain HITS the outer frame's binding, so a type-1
	// callee's GetVariable("super") reads the DISPATCHER's super unless shadowed.
	// broadcastMessage is the canonical user (gnash AsBroadcaster.as:312 —
	// a listener's `super.add(o)` must see the LISTENER's super, not the
	// broadcaster's). The arm computes the value, including any preload/suppress
	// gate for type-2 callees. Bound AFTER buildActivationLocals, so it wins
	// over INV_ACT_SUPER — do not set both unless shadowing is the intent.
	ActionVar* super_bind;
} InvokeOpts;

// Exported wrapper over action.c's static invokeFunctionValue — the unified
// invocation core — for subsystem files carved out of action.c. Invokes `func`
// with `this_var` as the receiver and `args`/`num_args` as the arguments,
// performing the ritual steps selected by `opts->flags`. Returns the function's
// return value (undefined for type-1 functions that return nothing, or when
// func is NULL / execution is halted / the depth guard trips).
ActionVar actionInvokeFunctionValue(SWFAppContext* app_context, ASFunction* func,
                                    ActionVar* this_var, ActionVar* args, u32 num_args,
                                    const InvokeOpts* opts);

// Constructor context stack — marks whether the active call is a
// constructor invocation (changes return-value semantics).
void pushCtorContext(u8 is_constructor);
void popCtorContext(void);
