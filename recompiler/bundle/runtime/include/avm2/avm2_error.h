#ifndef AVM2_ERROR_H
#define AVM2_ERROR_H

// Typed exceptions: Throw + exception-table dispatch (plan §4.4). Runtime
// dispatch uses setjmp/longjmp (the AVM1 g_exception_state precedent, but
// implemented here in src/avm2/ — action.c is never touched).
//
// Every emitted method body whose exception table has an active entry
// pushes an Avm2TryFrame in its prologue; avm2_throw walks the frame stack
// looking for a covering, type-matching handler (Ruffle activation.rs
// handle_err: op_index in [from_op, to_op), typed match by is_of_type,
// type 0 = catch-all) and longjmps to it. avm2_main installs a catch-all
// frame around each top-level entry (script init, root construction,
// frame scripts) so uncaught errors abort that entry only, matching
// Flash/Ruffle (the movie keeps running).

#include <setjmp.h>

#include <avm2/avm2_abc.h>
#include <avm2/avm2_value.h>

typedef struct Avm2Class Avm2Class;
typedef struct Avm2CallFrame Avm2CallFrame;

typedef struct Avm2TryFrame
{
	struct Avm2TryFrame* prev;
	jmp_buf jb;
	// Body frames: the method's exception table + defining file.
	const Avm2AbcException* excs;
	uint32_t exc_count;
	Avm2AbcFileRt* file;
	uint32_t op_index;        // updated by the body before each op
	uint8_t catch_all;        // top-level frame: catches everything
	// A catch-all the RUNTIME installs to handle the error itself (the loaded
	// SWF's root constructor renders its own trace). Ruffle only routes an
	// error to Avm2::uncaught_error where the player loop discards it, so a
	// silent frame must not also go through print_uncaught — otherwise
	// loader_error_in_root_ctor's traced line appears twice.
	uint8_t silent;
	uint32_t saved_call_depth;  // debug call stack unwound to here on catch
	// Try-helper emission mode (see Avm2TryFrameStorage below): the generated
	// body writes its op index into runtime-owned storage the frame does not
	// contain, so the dispatch reads it through here when non-NULL.
	const uint32_t* op_index_ext;
	// Set by dispatch before the longjmp:
	Avm2Value exc;
	uint32_t handler_target;  // op index to resume at
} Avm2TryFrame;

// ---------------------------------------------------------------------------
// Try-helper emission mode (recompiler option `try_helper` / SWF_TRY_HELPER).
//
// Generated code must never hold a libc-typed object across the guest/host
// boundary: the in-browser guest toolchain's `jmp_buf` is a handful of bytes
// where the Emscripten host's is 156, so a guest-allocated Avm2TryFrame is
// smashed the moment the host writes it (assessment §3.1). In helper mode the
// generated method allocates only this opaque blob — sized by the RUNTIME, not
// by any libc type — and the runtime places its own Avm2TryFrame inside it.
// `op_index` sits outside the blob so the body can keep updating it with a
// plain store (it is written before every op).
#define AVM2_TRY_FRAME_STORAGE 512
typedef struct Avm2TryFrameStorage
{
	uint32_t op_index;   // written directly by the generated body
	uint32_t reserved;
	union
	{
		unsigned char bytes[AVM2_TRY_FRAME_STORAGE];
		// Widest alignment the frame's contents can need (jmp_buf saves
		// registers and wants 16-byte alignment on x86-64).
		long double align_ld;
		void* align_p;
		uint64_t align_u;
	} opaque;
} Avm2TryFrameStorage;

typedef struct Avm2ScopeEntry Avm2ScopeEntry;

// The generated method's frame, shared with its lifted body function: the
// value arrays stay in the OUTER frame so they survive re-entry after a catch
// (the body function's own frame is gone by then).
typedef struct Avm2TryEnv
{
	Avm2Activation* act;
	Avm2Value* loc;
	Avm2Value* stk;
	Avm2ScopeEntry* lscope;
	Avm2TryFrameStorage* tf;
	int32_t resume;     // -1 = first entry; else the handler's op index
	Avm2Value exc;      // thrown value, pushed by the body on re-entry
	Avm2Value ret;      // the body's return value
} Avm2TryEnv;

// Returns 0 (or the body's own non-negative exit code) when the body ran to
// completion, -1 when a throw was caught by this frame.
typedef int (*Avm2TryBodyFn)(void* env);

void avm2_try_push_frame_st(Avm2Context* ctx, Avm2TryFrameStorage* st,
                            const Avm2AbcException* excs, uint32_t exc_count,
                            Avm2AbcFileRt* file);
int avm2_try_run(Avm2Context* ctx, Avm2TryFrameStorage* st,
                 Avm2TryBodyFn fn, void* env);
void avm2_try_pop_frame_st(Avm2TryFrameStorage* st);
uint32_t avm2_try_handler_target(const Avm2TryFrameStorage* st);
Avm2Value avm2_try_exc(const Avm2TryFrameStorage* st);

// Native C-stack guard. avmplus bounds AS3 recursion by the real machine
// stack (AvmCore::stackLimit / interpreter.cpp's stackAvailable check), not by
// a frame count, and reports exhaustion as a catchable Error #1023 "Stack
// overflow occurred." — infinitely recursive scripts are expected to survive
// as a caught exception (ecma3/Exceptions/bug127913). avm2_stack_guard_init
// records the startup stack address and budget; avm2_stack_check throws when
// an invocation would run past it.
void avm2_stack_guard_init(Avm2Context* ctx);
void avm2_stack_check(Avm2Context* ctx);

// Debug call stack (avm2_class.c pushes/pops around every invocation).
void avm2_callstack_push(Avm2Context* ctx, const Avm2MethodRef* m, Avm2Class* bound_class);
void avm2_callstack_pop(Avm2Context* ctx);
// FP's playerglobal is AS3 code that raises through Error.throwError, so an
// "Error$/throwError()" frame sits atop the trace; its C++-implemented core
// does NOT. Call this immediately before avm2_throw_error at the sites whose
// FP counterpart is AS3 (avm2/system_exit's expected trace has no such frame —
// this must stay a PER-SITE OPT-IN and never move inside avm2_throw_error).
// No pop is needed: the longjmp unwinds call_depth to the catching frame.
void avm2_callstack_push_throwerror(Avm2Context* ctx);
// A native frame FP has no name for at all. It still occupies a line in the
// stack trace, but an EMPTY one (no "\tat"), which is how the E4X scanner's
// frame shows up (avm2/xml_list_ctor_errors). Same per-site opt-in and same
// no-pop-needed contract as above.
void avm2_callstack_push_unnamed(Avm2Context* ctx);
// Replace the innermost frame (if it is `own_fn`'s) with a synthetic native
// one named verbatim — how a class-side ("Class$/m()") or namespaced builtin
// frame is spelled, which the generic renderer cannot derive from a vtable
// entry. Also used to name a native accessor's frame ("Class/set p()").
void avm2_callstack_rename_frame(Avm2Context* ctx, Avm2MethodFn own_fn,
                                 const char* name);
// FP-style frame name ("Test()", "test_fla::MainTimeline/frame1()",
// "global/flash.utils::getDefinitionByName()", "Function/<anonymous>()").
void avm2_callstack_frame_name(Avm2Context* ctx, const Avm2CallFrame* f,
                               char* buf, int size);

void avm2_try_push_frame(Avm2Context* ctx, Avm2TryFrame* tf,
                         const Avm2AbcException* excs, uint32_t exc_count,
                         Avm2AbcFileRt* file);
void avm2_try_push_catch_all(Avm2Context* ctx, Avm2TryFrame* tf);
// As above, but the caller reports the error itself (see `silent`).
void avm2_try_push_catch_all_silent(Avm2Context* ctx, Avm2TryFrame* tf);
void avm2_try_pop_frame(Avm2TryFrame* tf);

// Throw a value: longjmp to the innermost matching handler; if only the
// top-level catch-all matches, it logs to stderr and resumes there. Aborts
// the process if no frame is installed at all.
_Noreturn void avm2_throw(Avm2Context* ctx, Avm2Value value);

// Render a thrown value the way the debug player traces an uncaught one
// (toString() + the construction-time "\n\tat X()" tail).
const Avm2String* avm2_error_stack_string(Avm2Context* ctx, Avm2Value v);

// Construct an Error-family instance (does not throw).
Avm2Value avm2_error_new(Avm2Context* ctx, Avm2Class* error_class, const char* message);
// printf-style construct + throw. `error_class` NULL = plain Error.
_Noreturn void avm2_throw_error(Avm2Context* ctx, Avm2Class* error_class,
                                const char* fmt, ...);

// Common error constructors (exact FP strings ported from Ruffle error.rs).
_Noreturn void avm2_throw_null_or_undefined(Avm2Context* ctx, Avm2Value v,
                                            const char* name, uint32_t name_len);
_Noreturn void avm2_throw_1006(Avm2Context* ctx, const char* name, uint32_t name_len);
_Noreturn void avm2_throw_1069(Avm2Context* ctx, const char* name, uint32_t name_len,
                               Avm2Class* cls);
_Noreturn void avm2_throw_1065(Avm2Context* ctx, const char* name, uint32_t name_len);

#endif // AVM2_ERROR_H
