#ifndef AVM2_MAIN_H
#define AVM2_MAIN_H

// AVM2 entry point + context. runSWF_avm2() mirrors swf_core.c::runSWF's
// tick cadence and exit conditions (MAX_FRAMES) but dispatches the AVM2
// load ordering (avm2-support-plan §4.4 / Ruffle movie_clip.rs:4246-4255):
// load all ABC (no script inits) → process SymbolClass → eager-init each
// ABC's LAST script → construct the root class → per-tick frame scripts.
//
// swf_core.c is never edited to reference this module: AVM1 builds link
// swf_core.c without src/avm2/*.c. The shared wasm_wrappers/main.c calls
// runSWF_avm2() only under -DSWF_AVM2 (defined by verify_output.py when
// the recompiler produced RecompiledABC/).

#include <stddef.h>

#include <avm2/avm2_abc.h>
#include <avm2/avm2_globals.h>

typedef struct SWFAppContext SWFAppContext;

// One entry of the debug call stack (getStackTrace / error 1063 naming).
typedef struct Avm2CallFrame
{
	Avm2MethodRef method;
	Avm2Class* bound_class;
} Avm2CallFrame;

struct Avm2Context
{
	SWFAppContext* app;   // for heap_alloc/heap_calloc
	Avm2Domain domain;
	Avm2Object* builtin_globals;
	Avm2Builtins builtins;
	Avm2AbcFileRt** files;
	uint32_t file_count;
	Avm2Object* root;     // SymbolClass char-0 instance
	Avm2Object* stage;    // flash.display.Stage singleton (avm2_display.c)
	// Frame lifecycle (Ruffle frame_lifecycle.rs FramePhase):
	// 0 Idle, 1 Enter, 2 Construct, 3 FrameScripts, 4 Exit.
	uint8_t frame_phase;
	uint32_t instance_counter;  // auto instance names ("instanceN")
	// Opaque ids for "[object Function-N]" (Avm2Object.fn_tostring_id). Starts
	// at 0 so the first id is 1 — 0 doubles as "not yet assigned".
	uint32_t fn_tostring_next_id;
	struct Avm2TryFrame* try_top;  // innermost exception frame (avm2_error.h)
	uint8_t swf_version;  // for string_to_f64 bug compatibility
	uint8_t bytearray_default_encoding;  // ByteArray.defaultObjectEncoding
	// The default XML namespace URI (Dxns/DxnsLate), or NULL. Dynamically
	// scoped: avm2_call_method_ref AND avm2_call_function_obj (the runtime's
	// only two Avm2Activation construction sites) save/restore it around
	// every call and reset it for methods with the AVM2_METHOD_SET_DXNS flag
	// (Ruffle activation.rs default_xml_namespace propagation). NOTE: an
	// exception unwind does NOT restore it — there is no try-frame snapshot.
	const Avm2String* dxns;
	// Alchemy/CrossBridge domain memory (avm2_mops.c). `domain_memory` is
	// the ByteArray object assigned to ApplicationDomain.domainMemory, or
	// NULL when the default scratch buffer is in use. It is a GC root
	// (marked from avm2_gc_mark_roots_globals). NEVER cache its bytes
	// pointer: ByteArray.length reallocs the buffer.
	Avm2Object* domain_memory;
	// Lazily allocated zero-filled MIN_DOMAIN_MEMORY_LENGTH scratch used
	// while domain_memory is NULL. Persistent across assignments.
	uint8_t* domain_scratch;
	// Debug call stack (FP debug-player getStackTrace).
	Avm2CallFrame* call_frames;
	uint32_t call_depth;
	uint32_t call_cap;
	// Native C-stack guard (avm2_error.c). AS3 recursion is bounded by the
	// real machine stack, exactly as in avmplus (AvmCore::stackLimit) — a
	// frame count would be wrong because generated method frames vary in size
	// by orders of magnitude. `stack_base` is the deepest-known-safe address
	// captured at startup; exceeding `stack_budget` bytes below it throws
	// Error #1023.
	char* stack_base;
	size_t stack_budget;
	// Set while an Error #1023 is being built and dispatched: constructing the
	// Error is itself an AS3 invocation, so the guard must stand down until the
	// throw lands or it recurses on its own diagnosis. The reserved headroom
	// below stack_budget is what that construction runs in.
	uint8_t stack_overflow_pending;
	// Set for the duration of ONE Error construction whose stack tail must be
	// empty. FP verifies a method BEFORE its frame exists, so a VerifyError's
	// getStackTrace() has no frames at all — but our verify bodies are emitted
	// INSIDE the method they reject, so error_init would snapshot the very
	// frame FP never pushed (avm2/verify_illegal_opcode expects the error line
	// alone). Cleared by error_init.
	uint8_t suppress_stack_snapshot;
};

void runSWF_avm2(SWFAppContext* app_context);

// The (single) runtime context.
Avm2Context* avm2_get_context(void);

// Heap helpers bound to the shared o1heap allocator.
void* avm2_alloc(Avm2Context* ctx, uint32_t size);

#endif // AVM2_MAIN_H
