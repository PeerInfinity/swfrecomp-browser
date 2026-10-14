// Typed exceptions (avm2_error.h): try-frame stack, throw dispatch, and
// the Error class family. Dispatch semantics per Ruffle activation.rs
// handle_err; error message strings per Ruffle error.rs (exact FP text —
// many tests assert them).

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/stack.h>
#else
#include <sys/resource.h>
#endif

// ---------------------------------------------------------------------------
// Native C-stack guard
// ---------------------------------------------------------------------------
//
// Every AS3 invocation consumes a real C frame (the generated method body plus
// the runtime's own dispatch frames), so unbounded AS recursion is a segfault
// unless something stops it. avmplus stops it by comparing the stack pointer
// against a limit derived from the thread's stack (AvmCore::stackLimit) and
// throwing a *catchable* Error #1023; scripts that recurse forever inside a
// try/catch are expected to keep running. We do the same. A fixed call-depth
// cap would be the wrong instrument: generated frames range from a few dozen
// bytes to several KB, so no single count is both safe and non-restrictive.

// Headroom kept below the limit so the throw itself — snprintf, Error
// construction, the unwind — always has room. Capped so a small wasm stack
// still gets most of its depth.
#define AVM2_STACK_RESERVE_MAX (1u * 1024u * 1024u)
// Assumed usable stack when the platform will not say (RLIM_INFINITY).
#define AVM2_STACK_DEFAULT_TOTAL (8u * 1024u * 1024u)

void avm2_stack_guard_init(Avm2Context* ctx)
{
	size_t total;
#if defined(__EMSCRIPTEN__)
	ctx->stack_base = (char*) emscripten_stack_get_base();
	total = (size_t) (emscripten_stack_get_base() - emscripten_stack_get_end());
#else
	char here;
	ctx->stack_base = &here;
	total = AVM2_STACK_DEFAULT_TOTAL;
	struct rlimit rl;
	if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
	    && rl.rlim_cur > 0)
	{
		total = (size_t) rl.rlim_cur;
		// Ridiculous values (some CI images set 512 MB) would defeat the
		// guard's purpose: the process would thrash long before we throw.
		if (total > 64u * 1024u * 1024u) total = 64u * 1024u * 1024u;
	}
#endif
	size_t reserve = total / 8;
	if (reserve > AVM2_STACK_RESERVE_MAX) reserve = AVM2_STACK_RESERVE_MAX;
	ctx->stack_budget = (total > reserve) ? total - reserve : total / 2;
}

void avm2_stack_check(Avm2Context* ctx)
{
	if (ctx->stack_base == NULL) return;  // guard not initialised (unit tests)
	if (ctx->stack_overflow_pending) return;
	char probe;
	// The stack grows down on every target we build for; a negative delta
	// (a callback entered on some other stack) simply reads as "plenty left".
	ptrdiff_t used = ctx->stack_base - &probe;
	if (used > 0 && (size_t) used > ctx->stack_budget)
	{
		ctx->stack_overflow_pending = 1;  // cleared by avm2_throw's longjmp
		avm2_throw_error(ctx, NULL, "Error #1023: Stack overflow occurred.");
	}
}

// ---------------------------------------------------------------------------
// Debug call stack
// ---------------------------------------------------------------------------

void avm2_callstack_push(Avm2Context* ctx, const Avm2MethodRef* m, Avm2Class* bound_class)
{
	if (ctx->call_depth == ctx->call_cap)
	{
		uint32_t new_cap = ctx->call_cap == 0 ? 64 : ctx->call_cap * 2;
		Avm2CallFrame* grown = avm2_alloc(ctx, new_cap * sizeof(Avm2CallFrame));
		if (ctx->call_depth > 0)
		{
			memcpy(grown, ctx->call_frames, ctx->call_depth * sizeof(Avm2CallFrame));
		}
		ctx->call_frames = grown;
		ctx->call_cap = new_cap;
	}
	ctx->call_frames[ctx->call_depth].method = *m;
	ctx->call_frames[ctx->call_depth].bound_class = bound_class;
	ctx->call_depth++;
}

void avm2_callstack_pop(Avm2Context* ctx)
{
	if (ctx->call_depth > 0) ctx->call_depth--;
}

// See avm2_error.h: the synthetic "Error$/throwError()" frame FP's AS3
// playerglobal contributes. PER-SITE OPT-IN — avm2/system_exit's expected
// trace proves a blanket push inside avm2_throw_error would be wrong.
void avm2_callstack_push_throwerror(Avm2Context* ctx)
{
	static const Avm2MethodRef throwerror = { NULL, NULL, "Error$/throwError", 0 };
	avm2_callstack_push(ctx, &throwerror, NULL);
}

// See avm2_error.h: a nameless native frame, rendered as an empty line.
void avm2_callstack_push_unnamed(Avm2Context* ctx)
{
	static const Avm2MethodRef unnamed = { NULL, NULL, "", 0 };
	avm2_callstack_push(ctx, &unnamed, NULL);
}

// See avm2_error.h. `name` must outlive the frame (a string literal, or a
// registration-time allocation).
void avm2_callstack_rename_frame(Avm2Context* ctx, Avm2MethodFn own_fn,
                                 const char* name)
{
	if (ctx->call_depth > 0
	    && ctx->call_frames[ctx->call_depth - 1].method.fn == own_fn)
	{
		avm2_callstack_pop(ctx);
	}
	Avm2MethodRef ref;
	memset(&ref, 0, sizeof(ref));
	ref.debug_name = name;
	avm2_callstack_push(ctx, &ref, NULL);
}

// Is this frame's method a CLASS (static) trait of `cls`? Ruffle binds a
// static method to the class's c_class ("Test$"), whose traits are exactly the
// entries of our class object's vtable — so a hit here is what makes the frame
// read "Test$/class_method()" instead of "Test/class_method()".
static int frame_is_class_trait(const Avm2Class* cls, const Avm2MethodRef* m)
{
	if (cls == NULL || cls->class_object == NULL) return 0;
	// A NATIVE builtin (file == NULL) has no ABC identity, so its C function
	// pointer IS its identity and the file/method_index halves of the compare
	// below are trivially equal. Bailing on `file == NULL` used to strip the
	// `$` off EVERY native class-side frame ("JSON/parse()" for FP's
	// "JSON$/parse()"), which is why five call sites carry a hand-written
	// "Cls$/m" literal today. A NULL fn identifies nothing — reject that.
	if (m->file == NULL && m->fn == NULL) return 0;
	const Avm2VTable* vt = cls->class_object->vtable;
	if (vt == NULL) return 0;
	for (uint32_t i = 0; i < vt->count; i++)
	{
		const Avm2PropEntry* e = &vt->entries[i];
		if (e->kind == AVM2_PROP_SLOT) continue;
		if (e->method.file == m->file
		    && e->method.method_index == m->method_index
		    && e->method.fn == m->fn)
			return 1;
		if ((e->kind == AVM2_PROP_SETTER || e->kind == AVM2_PROP_GETSET)
		    && e->setter.file == m->file
		    && e->setter.method_index == m->method_index
		    && e->setter.fn == m->fn)
			return 1;
	}
	return 0;
}

void avm2_callstack_frame_name(Avm2Context* ctx, const Avm2CallFrame* f,
                               char* buf, int size)
{
	(void) ctx;
	const Avm2MethodRef* m = &f->method;
	if (f->bound_class != NULL)
	{
		char cq[128];
		avm2_class_qname_colons_buf(f->bound_class, cq, sizeof(cq));
		int is_iinit = f->bound_class->instance_init.fn == m->fn
		               && f->bound_class->instance_init.file == m->file
		               && f->bound_class->instance_init.method_index == m->method_index;
		// The STATIC initializer is the c_class's instance init in Ruffle's
		// model, and a c_class initializer prints "<QName>$cinit"
		// (function.rs:317-325). Ours is a field on the same class.
		int is_cinit = !is_iinit && m->file != NULL
		               && f->bound_class->class_init.file == m->file
		               && f->bound_class->class_init.method_index == m->method_index
		               && f->bound_class->class_init.fn == m->fn;
		if (is_cinit)
		{
			snprintf(buf, size, "%s$cinit()", cq);
		}
		else if (is_iinit)
		{
			snprintf(buf, size, "%s()", cq);
		}
		else
		{
			// mxmlc debug names are qualified ("test_fla:MainTimeline/
			// test_fla:frame1"); keep only the final segment. A NATIVE
			// method's debug_name is built here, not by a compiler, so it is
			// printed verbatim — that is what lets a builtin registered in a
			// non-public namespace carry its URI ("flash.utils::Proxy/
			// http://www.adobe.com/2006/actionscript/flash/proxy::getProperty()").
			const char* dn = (m->debug_name != NULL && m->debug_name[0] != '\0')
				? m->debug_name : "<anonymous>";
			if (m->file != NULL)
			{
				const char* slash = strrchr(dn, '/');
				if (slash != NULL) dn = slash + 1;
				// "uri::f" is FP's own spelling of a user-namespaced trait
				// (built in avm2_vtable_add_traits) and must survive; the
				// mxmlc package prefix this strip exists for uses a SINGLE
				// colon ("test_fla:frame1").
				if (strstr(dn, "::") == NULL)
				{
					const char* colon = strrchr(dn, ':');
					if (colon != NULL) dn = colon + 1;
				}
			}
			snprintf(buf, size, frame_is_class_trait(f->bound_class, m)
			                    ? "%s$/%s()" : "%s/%s()", cq, dn);
		}
		return;
	}
	if (m->file == NULL)
	{
		// Native: debug_name is prebuilt (globals register the full
		// "global/ns::name" form). An EMPTY debug_name is the deliberate
		// "unnamed frame" marker (avm2_callstack_push_unnamed) — it prints
		// as nothing, and callstack_snapshot drops the "\tat " with it.
		if (m->debug_name != NULL && m->debug_name[0] == '\0')
		{
			if (size > 0) buf[0] = '\0';
			return;
		}
		snprintf(buf, size, "%s()", m->debug_name != NULL ? m->debug_name : "<native>");
		return;
	}
	const Avm2AbcMethodData* md = &m->file->data->methods[m->method_index];
	if (md->is_function)
	{
		// Ruffle method.rs::method_name: `method_info.name == 0` means the
		// method has NO name (None) and the frame is spelled by its ABC
		// method index; a name index that merely points at the empty string
		// is Some("") and prints "<anonymous>". The recompiler preserves the
		// difference by emitting a NULL debug_name for index 0 (ASC/Flash-IDE
		// builds name every method 0; mxmlc builds point at "").
		if (md->debug_name == NULL && m->debug_name == NULL)
		{
			snprintf(buf, size, "MethodInfo-%u()", m->method_index);
			return;
		}
		snprintf(buf, size, "Function/%s()",
		         (m->debug_name != NULL && m->debug_name[0] != '\0')
		         ? m->debug_name : "<anonymous>");
		return;
	}
	if (m->debug_name != NULL && m->debug_name[0] != '\0')
	{
		snprintf(buf, size, "global/%s()", m->debug_name);
		return;
	}
	snprintf(buf, size, "global$init()");
}

// Is `cls` in the Error family (its constructor frames are excluded from
// stack snapshots)?
static int class_is_error_family(Avm2Context* ctx, Avm2Class* cls)
{
	for (Avm2Class* c = cls; c != NULL; c = c->super_class)
	{
		if (c == ctx->builtins.error_class) return 1;
	}
	return 0;
}

// Snapshot "\n\tat X()" lines for the current stack (innermost first),
// skipping the error-constructor frames on top.
static const Avm2String* callstack_snapshot(Avm2Context* ctx)
{
	char buf[4096];
	uint32_t n = 0;
	int64_t top = (int64_t) ctx->call_depth - 1;
	while (top >= 0)
	{
		Avm2CallFrame* f = &ctx->call_frames[top];
		int is_error_ctor = f->bound_class != NULL
			&& class_is_error_family(ctx, f->bound_class)
			&& f->bound_class->instance_init.fn == f->method.fn;
		if (!is_error_ctor) break;
		top--;
	}
	for (int64_t i = top; i >= 0 && n + 200 < sizeof(buf); i--)
	{
		char name[176];
		avm2_callstack_frame_name(ctx, &ctx->call_frames[i], name, sizeof(name));
		n += (uint32_t) (name[0] == '\0'
			? snprintf(buf + n, sizeof(buf) - n, "\n")
			: snprintf(buf + n, sizeof(buf) - n, "\n\tat %s", name));
	}
	return avm2_string_new(ctx, buf, n);
}

// ---------------------------------------------------------------------------
// Try frames
// ---------------------------------------------------------------------------

void avm2_try_push_frame(Avm2Context* ctx, Avm2TryFrame* tf,
                         const Avm2AbcException* excs, uint32_t exc_count,
                         Avm2AbcFileRt* file)
{
	memset(tf, 0, sizeof(*tf));
	tf->excs = excs;
	tf->exc_count = exc_count;
	tf->file = file;
	tf->saved_call_depth = ctx->call_depth;
	tf->prev = ctx->try_top;
	ctx->try_top = tf;
}

void avm2_try_push_catch_all(Avm2Context* ctx, Avm2TryFrame* tf)
{
	memset(tf, 0, sizeof(*tf));
	tf->catch_all = 1;
	tf->saved_call_depth = ctx->call_depth;
	tf->prev = ctx->try_top;
	ctx->try_top = tf;
}

void avm2_try_push_catch_all_silent(Avm2Context* ctx, Avm2TryFrame* tf)
{
	avm2_try_push_catch_all(ctx, tf);
	tf->silent = 1;
}

void avm2_try_pop_frame(Avm2TryFrame* tf)
{
	// Frames above tf (from callees that longjmp'd past their pop) are
	// dropped along with it.
	avm2_get_context()->try_top = tf->prev;
}

// --- try-helper emission mode (avm2_error.h) --------------------------------
//
// The generated method owns only the opaque Avm2TryFrameStorage; the frame the
// runtime longjmps into lives inside it, and the setjmp itself is executed HERE
// (avm2_try_run), so no generated frame ever holds a jmp_buf.

static Avm2TryFrame* try_frame_of(Avm2TryFrameStorage* st)
{
	return (Avm2TryFrame*) (void*) st->opaque.bytes;
}

void avm2_try_push_frame_st(Avm2Context* ctx, Avm2TryFrameStorage* st,
                            const Avm2AbcException* excs, uint32_t exc_count,
                            Avm2AbcFileRt* file)
{
	_Static_assert(sizeof(Avm2TryFrame) <= AVM2_TRY_FRAME_STORAGE,
	               "AVM2_TRY_FRAME_STORAGE too small for Avm2TryFrame");
	Avm2TryFrame* tf = try_frame_of(st);
	st->op_index = 0;
	st->reserved = 0;
	avm2_try_push_frame(ctx, tf, excs, exc_count, file);
	tf->op_index_ext = &st->op_index;
}

int avm2_try_run(Avm2Context* ctx, Avm2TryFrameStorage* st,
                 Avm2TryBodyFn fn, void* env)
{
	Avm2TryFrame* tf = try_frame_of(st);
	(void) ctx;
	// The frame stays installed across a catch (avm2_throw leaves try_top at
	// it for nested/rethrow), so re-arming is just another setjmp on it.
	if (setjmp(tf->jb) != 0)
	{
		return -1;
	}
	return fn(env);
}

void avm2_try_pop_frame_st(Avm2TryFrameStorage* st)
{
	avm2_try_pop_frame(try_frame_of(st));
}

uint32_t avm2_try_handler_target(const Avm2TryFrameStorage* st)
{
	return ((const Avm2TryFrame*) (const void*) st->opaque.bytes)->handler_target;
}

Avm2Value avm2_try_exc(const Avm2TryFrameStorage* st)
{
	return ((const Avm2TryFrame*) (const void*) st->opaque.bytes)->exc;
}

static void print_uncaught(Avm2Context* ctx, Avm2Value v)
{
	// Ruffle Avm2::uncaught_error (avm2.rs): in Debug player mode — what the
	// test harness runs — Flash Player TRACES the error that escaped, so the
	// line lands in the graded output. The text is Error::to_string(): the
	// value coerced to a string, followed by the call stack the Error captured
	// when it was built (the same "\n\tat X()" tail getStackTrace prints).
	// A thrown non-Error value has no tail and prints on its own.
	//
	// Only frames the PLAYER LOOP discards reach here — a catch-all the
	// runtime installs to render the error itself is marked `silent`
	// (loader_error_in_root_ctor would otherwise print its line twice).
	const Avm2String* s = avm2_coerce_to_string(ctx, v);
	const Avm2String* tail = NULL;
	if (v.kind == AVM2_VALUE_OBJECT && v.u.obj != NULL)
	{
		Avm2Value* t = avm2_object_find_dynamic(v.u.obj, "__stacktrace_tail",
		                                        17);
		if (t != NULL && t->kind == AVM2_VALUE_STRING) tail = t->u.str;
	}
	if (tail == NULL)
	{
		// A thrown NON-Error value carries no snapshot, but FP still names the
		// frame the throw escaped from ("undefined\n\tat global$init()" —
		// ecma3/Exceptions/exception_011_rt). print_uncaught runs before the
		// longjmp unwinds, so the current stack IS the throw site's.
		tail = callstack_snapshot(ctx);
	}
	printf("%.*s", (int) s->len, s->utf8);
	if (tail != NULL) printf("%.*s", (int) tail->len, tail->utf8);
	putchar('\n');
	fflush(stdout);
	fprintf(stderr, "AVM2 uncaught error: %.*s\n", (int) s->len, s->utf8);
}

_Noreturn void avm2_throw(Avm2Context* ctx, Avm2Value value)
{
	// stack_overflow_pending (if set) stays set across the search and the
	// uncaught-error printout — both can re-enter AS3 (type checks, toString)
	// while the stack is still past its budget — and is cleared at each exit
	// below, where the longjmp hands the consumed headroom back.
	for (Avm2TryFrame* tf = ctx->try_top; tf != NULL; tf = tf->prev)
	{
		if (tf->catch_all)
		{
			if (!tf->silent) print_uncaught(ctx, value);
			ctx->try_top = tf;  // drop frames above
			ctx->call_depth = tf->saved_call_depth;
			tf->exc = value;
			ctx->stack_overflow_pending = 0;
			longjmp(tf->jb, 1);
		}
		for (uint32_t i = 0; i < tf->exc_count; i++)
		{
			const Avm2AbcException* e = &tf->excs[i];
			if (!e->active) continue;
			uint32_t op_index = tf->op_index_ext != NULL
				? *tf->op_index_ext : tf->op_index;
			if (op_index < e->from_op || op_index >= e->to_op) continue;
			if (e->type_mn != 0)
			{
				Avm2Class* cls = avm2_class_for_mn(ctx, tf->file, e->type_mn);
				if (cls == NULL) continue;
				if (!avm2_value_is_of_type(ctx, value, cls)) continue;
			}
			ctx->try_top = tf;  // keep this frame installed (nested catches)
			ctx->call_depth = tf->saved_call_depth;
			tf->exc = value;
			tf->handler_target = e->target_op;
			ctx->stack_overflow_pending = 0;
			longjmp(tf->jb, 1);
		}
	}
	// No frame at all (shouldn't happen: avm2_main installs a catch-all).
	ctx->stack_overflow_pending = 0;
	print_uncaught(ctx, value);
	exit(1);
}

// ---------------------------------------------------------------------------
// Error construction
// ---------------------------------------------------------------------------

Avm2Value avm2_error_new(Avm2Context* ctx, Avm2Class* error_class, const char* message)
{
	if (error_class == NULL) error_class = ctx->builtins.error_class;
	Avm2Value args[2];
	args[0] = avm2_string(avm2_string_from_literal(ctx, message));
	// Runtime error messages carry their FP code as "Error #NNNN: ..." —
	// surface it as errorID (Ruffle constructs errors with the code).
	int32_t code = 0;
	const char* hash = strchr(message, '#');
	if (hash != NULL)
	{
		code = (int32_t) strtol(hash + 1, NULL, 10);
	}
	args[1] = avm2_integer(code);
	return avm2_class_construct(ctx, error_class, args, 2);
}

_Noreturn void avm2_throw_error(Avm2Context* ctx, Avm2Class* error_class,
                                const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	avm2_throw(ctx, avm2_error_new(ctx, error_class, buf));
}

_Noreturn void avm2_throw_null_or_undefined(Avm2Context* ctx, Avm2Value v,
                                            const char* name, uint32_t name_len)
{
	const char* base = (v.kind == AVM2_VALUE_UNDEFINED)
		? "Error #1010: A term is undefined and has no properties."
		: "Error #1009: Cannot access a property or method of a null object reference.";
	if (name != NULL && name_len > 0)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "%s (accessing field: %.*s)", base, (int) name_len, name);
	}
	avm2_throw_error(ctx, ctx->builtins.type_error_class, "%s", base);
}

_Noreturn void avm2_throw_1006(Avm2Context* ctx, const char* name, uint32_t name_len)
{
	(void) name; (void) name_len;
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #1006: value is not a function.");
}

_Noreturn void avm2_throw_1069(Avm2Context* ctx, const char* name, uint32_t name_len,
                               Avm2Class* cls)
{
	char cn[160];
	avm2_class_qname_buf(cls, cn, sizeof(cn));
	avm2_throw_error(ctx, ctx->builtins.reference_error_class,
	                 "Error #1069: Property %.*s not found on %s and there is "
	                 "no default value.", (int) name_len, name, cn);
}

_Noreturn void avm2_throw_1065(Avm2Context* ctx, const char* name, uint32_t name_len)
{
	avm2_throw_error(ctx, ctx->builtins.reference_error_class,
	                 "Error #1065: Variable %.*s is not defined.",
	                 (int) name_len, name);
}

// ---------------------------------------------------------------------------
// Error classes
// ---------------------------------------------------------------------------

// name/message are instance SLOTS (Ruffle Error.as `public var name` /
// `public var message`): direct reads see them, but prototype-CHAIN reads
// don't (only dynamic props delegate — error_prototype).
enum
{
	ERROR_SLOT_NAME = 1,
	ERROR_SLOT_MESSAGE = 2,
};

// Error constructor body: this.message = args[0] (default ""), this.name =
// <constructing class>.prototype.name, errorID = args[1] (dont-enum
// dynamic — the getter reads it back).
static Avm2Value error_init(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* self = act->this_val.u.obj;
	Avm2Value msg = (act->argc > 0)
		? avm2_string(avm2_coerce_to_string(ctx, act->args[0]))
		: avm2_string(avm2_string_from_literal(ctx, ""));
	// Core error classes re-set `this.name = prototype.name` in their own
	// constructors (Ruffle TypeError.as etc.); playerglobal flash.errors
	// subclasses do NOT — their instances keep the "Error" that Error's
	// constructor assigned (EOFError traces as "Error: Error #2030...").
	Avm2Value name = avm2_string(avm2_string_from_literal(ctx, "Error"));
	if (act->bound_class != NULL && act->bound_class->prototype_obj != NULL
	    && !(act->bound_class->name.ns_len == 12
	         && memcmp(act->bound_class->name.ns_uri, "flash.errors", 12) == 0))
	{
		Avm2Value pn = avm2_get_public_property(
			ctx, avm2_object_value(act->bound_class->prototype_obj), "name", 4, NULL);
		if (pn.kind != AVM2_VALUE_UNDEFINED) name = pn;
	}
	if (self->slot_count > ERROR_SLOT_MESSAGE)
	{
		self->slots[ERROR_SLOT_NAME] = name;
		self->slots[ERROR_SLOT_MESSAGE] = msg;
	}
	Avm2Value eid = (act->argc > 1)
		? avm2_integer(avm2_coerce_to_i32(ctx, act->args[1]))
		: avm2_integer(0);
	Avm2DynProp* p = avm2_object_set_dynamic(ctx, self, "errorID", 7, eid);
	p->dont_enum = 1;
	// FP debug player snapshots the call stack at construction — except for a
	// verifier-raised error, which FP builds before the rejected method's frame
	// exists (see Avm2Context.suppress_stack_snapshot).
	const Avm2String* tail;
	if (ctx->suppress_stack_snapshot)
	{
		ctx->suppress_stack_snapshot = 0;
		tail = avm2_string_from_literal(ctx, "");
	}
	else
	{
		tail = callstack_snapshot(ctx);
	}
	p = avm2_object_set_dynamic(ctx, self, "__stacktrace_tail", 17,
	                            avm2_string(tail));
	p->dont_enum = 1;
	return avm2_undefined();
}

// DRMManagerError(message, id, subErrorID) — Error's two-parameter ctor plus
// a third field, stored dont-enum beside errorID.
static Avm2Value drm_manager_error_init(Avm2Activation* act)
{
	error_init(act);
	Avm2Context* ctx = act->ctx;
	if (act->this_val.kind != AVM2_VALUE_OBJECT) return avm2_undefined();
	Avm2Value sub = (act->argc > 2)
		? avm2_integer(avm2_coerce_to_i32(ctx, act->args[2]))
		: avm2_integer(0);
	Avm2DynProp* p = avm2_object_set_dynamic(ctx, act->this_val.u.obj,
	                                         "_subErrorID", 11, sub);
	p->dont_enum = 1;
	return avm2_undefined();
}

static Avm2Value drm_get_sub_error_id(Avm2Activation* act)
{
	if (act->this_val.kind == AVM2_VALUE_OBJECT)
	{
		Avm2Value* v = avm2_object_find_dynamic(act->this_val.u.obj,
		                                        "_subErrorID", 11);
		if (v != NULL) return *v;
	}
	return avm2_integer(0);
}

// "DRMManagerError: 'msg', error ID:'42', subErrorID:'10'" — nothing like
// Error.prototype.toString's "name: message".
static Avm2Value drm_error_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value msg = avm2_get_public_property(ctx, act->this_val, "message", 7, NULL);
	Avm2Value eid = avm2_get_public_property(ctx, act->this_val, "errorID", 7, NULL);
	Avm2Value sub = drm_get_sub_error_id(act);
	char buf[64];
	const Avm2String* out = avm2_string_from_literal(ctx, "DRMManagerError: '");
	out = avm2_string_concat(ctx, out, avm2_coerce_to_string(ctx, msg));
	out = avm2_string_concat(ctx, out,
	                         avm2_string_from_literal(ctx, "', error ID:'"));
	snprintf(buf, sizeof(buf), "%d", (int) avm2_coerce_to_i32(ctx, eid));
	out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, buf));
	out = avm2_string_concat(ctx, out,
	                         avm2_string_from_literal(ctx, "', subErrorID:'"));
	snprintf(buf, sizeof(buf), "%d", (int) avm2_coerce_to_i32(ctx, sub));
	out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, buf));
	out = avm2_string_concat(ctx, out, avm2_string_from_literal(ctx, "'"));
	return avm2_string(out);
}

// Error.prototype.toString: "" message → name, else "name: message".
static Avm2Value error_proto_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value name = avm2_get_public_property(ctx, act->this_val, "name", 4, NULL);
	Avm2Value msg = avm2_get_public_property(ctx, act->this_val, "message", 7, NULL);
	const Avm2String* name_s = avm2_coerce_to_string(ctx, name);
	const Avm2String* msg_s = avm2_coerce_to_string(ctx, msg);
	if (msg_s->len == 0)
	{
		return avm2_string(name_s);
	}
	const Avm2String* sep = avm2_string_from_literal(ctx, ": ");
	return avm2_string(avm2_string_concat(ctx, avm2_string_concat(ctx, name_s, sep), msg_s));
}

static Avm2Value error_get_error_id(Avm2Activation* act)
{
	// Read the dynamic slot directly (the vtable getter would recurse).
	if (act->this_val.kind == AVM2_VALUE_OBJECT)
	{
		Avm2Value* v = avm2_object_find_dynamic(act->this_val.u.obj, "errorID", 7);
		if (v != NULL) return *v;
	}
	return avm2_integer(0);
}

// The FP debug player's rendering of an uncaught error: its toString()
// followed by the "\n\tat X()" tail snapshotted when the Error was
// constructed — the same pair Error.getStackTrace() returns. A non-Error throw
// (or an Error built before the snapshot existed) renders as just the head.
const Avm2String* avm2_error_stack_string(Avm2Context* ctx, Avm2Value v)
{
	const Avm2String* head = avm2_coerce_to_string(ctx, v);
	if (v.kind != AVM2_VALUE_OBJECT) return head;
	Avm2Value* tail = avm2_object_find_dynamic(v.u.obj, "__stacktrace_tail", 17);
	if (tail == NULL || tail->kind != AVM2_VALUE_STRING) return head;
	return avm2_string_concat(ctx, head, tail->u.str);
}

// The debug/release player split (Ruffle globals/error.rs get_stack_trace).
// Flash Player 11.5 (player version 18) made stack traces available in the
// RELEASE player too, but only for SWF>=18; a SWF<18 (or an older player)
// gets a trace in flashplayerdebugger and `null` in flashplayer. We always
// emulate a modern player (>= 18 — Ruffle's DEFAULT_PLAYER_VERSION is 32 and
// no test overrides it), so Ruffle's two-term gate
//   (player_version >= 18 && caller_movie.version() >= 18) || mode == Debug
// reduces to `caller SWF >= 18 || debug player`.
//
// The player mode is a harness knob, not an SWF flag: Ruffle's test framework
// assumes flashplayerdebugger unless test.toml sets
// `[player_options] mode = "Release"`, which verify_output.py mirrors as
// -DSWF_PLAYER_MODE_RELEASE. Only the four avm2/error_stack_trace_{debug,
// release}_swf1{7,8} tests set it; `release_swf17` is the one combination
// that must return null (its stage renders red instead of blue).
#ifdef SWF_PLAYER_MODE_RELEASE
#define AVM2_PLAYER_IS_DEBUG 0
#else
#define AVM2_PLAYER_IS_DEBUG 1
#endif

// Ruffle Activation::caller_movie_or_root().version(): the SWF version of the
// movie whose code called this native, NOT the root's. Same walk as
// avm2_xml.c's xml_caller_swf_version — a native builtin's own `act->file` is
// NULL, so the caller is the nearest debug-callstack frame that HAS an ABC
// file; no such frame means the main movie.
static uint8_t error_caller_swf_version(Avm2Activation* act)
{
	const Avm2AbcFileData* data = (act->file != NULL) ? act->file->data : NULL;
	if (data == NULL)
	{
		Avm2Context* c = act->ctx;
		for (uint32_t i = c->call_depth; i > 0 && data == NULL; i--)
		{
			Avm2AbcFileRt* f = c->call_frames[i - 1].method.file;
			if (f != NULL) data = f->data;
		}
	}
	const Avm2MovieTables* mv = (data != NULL) ? avm2_display_movie_for_abc(data)
	                                           : NULL;
	return (mv != NULL) ? mv->swf_version : (uint8_t) act->ctx->swf_version;
}

static Avm2Value error_get_stack_trace(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	if (!AVM2_PLAYER_IS_DEBUG && error_caller_swf_version(act) < 18)
	{
		return avm2_null();
	}
	if (act->this_val.kind != AVM2_VALUE_OBJECT) return avm2_null();
	Avm2Value* tail = avm2_object_find_dynamic(act->this_val.u.obj,
	                                           "__stacktrace_tail", 17);
	if (tail == NULL) return avm2_null();
	// First line is the error's toString() — a REAL call, so a script that
	// replaces Error.prototype.toString sees its side effects and its return
	// value here (avm2/error_stack_trace_edge_cases). A non-string return is
	// coerced, so an override returning null yields the literal "null".
	Avm2Value head = avm2_call_public_property(ctx, act->this_val,
	                                           "toString", 8, NULL, 0);
	return avm2_string(avm2_string_concat(ctx, avm2_coerce_to_string(ctx, head),
	                                      tail->u.str));
}

// Error-message table for Error.getErrorMessage, generated from Ruffle
// core/src/avm2/error_messages.rs and verified line-for-line against
// avm2/error_geterrormessage's output.txt (all 705 codes are covered by
// that test). Codes are ascending — lookup binary-searches.
typedef struct
{
	uint16_t code;
	const char* msg;
} Avm2ErrorMessageEntry;

// Longest message body is 204 chars.
static const Avm2ErrorMessageEntry ERROR_MESSAGES[] = {
	{ 1000, "The system is out of memory." },
	{ 1001, "The method %1 is not implemented." },
	{ 1002, "Number.toPrecision has a range of 1 to 21. Number.toFixed and Number.toExponential have a range of 0 to 20. Specified value is not within expected range." },
	{ 1003, "The radix argument must be between 2 and 36; got %1." },
	{ 1004, "Method %1 was invoked on an incompatible object." },
	{ 1005, "Array index is not a positive integer (%1)." },
	{ 1006, "%1 is not a function." },
	{ 1007, "Instantiation attempted on a non-constructor." },
	{ 1008, "%1 is ambiguous; Found more than one matching binding." },
	{ 1009, "Cannot access a property or method of a null object reference." },
	{ 1010, "A term is undefined and has no properties." },
	{ 1011, "Method %1 contained illegal opcode %2 at offset %3." },
	{ 1012, "The last instruction exceeded code size." },
	{ 1013, "Cannot call OP_findproperty when scopeDepth is 0." },
	{ 1014, "Class %1 could not be found." },
	{ 1015, "Method %1 cannot set default xml namespace" },
	{ 1016, "Descendants operator (..) not supported on type %1." },
	{ 1017, "Scope stack overflow occurred." },
	{ 1018, "Scope stack underflow occurred." },
	{ 1019, "Getscopeobject %1 is out of bounds." },
	{ 1020, "Code cannot fall off the end of a method." },
	{ 1021, "At least one branch target was not on a valid instruction in the method." },
	{ 1022, "Type void may only be used as a function return type." },
	{ 1023, "Stack overflow occurred." },
	{ 1024, "Stack underflow occurred." },
	{ 1025, "An invalid register %1 was accessed." },
	{ 1026, "Slot %1 exceeds slotCount=%2 of %3." },
	{ 1027, "Method_info %1 exceeds method_count=%2." },
	{ 1028, "Disp_id %1 exceeds max_disp_id=%2 of %3." },
	{ 1029, "Disp_id %1 is undefined on %2." },
	{ 1030, "Stack depth is unbalanced. %1 != %2." },
	{ 1031, "Scope depth is unbalanced. %1 != %2." },
	{ 1032, "Cpool index %1 is out of range %2." },
	{ 1033, "Cpool entry %1 is wrong type." },
	{ 1034, "Type Coercion failed: cannot convert %1 to %2." },
	{ 1035, "Illegal super expression found in method %1." },
	{ 1037, "Cannot assign to a method %1 on %2." },
	{ 1038, "%1 is already defined." },
	{ 1039, "Cannot verify method until it is referenced." },
	{ 1040, "The right-hand side of instanceof must be a class or function." },
	{ 1041, "The right-hand side of operator must be a class." },
	{ 1042, "Not an ABC file.  major_version=%1 minor_version=%2." },
	{ 1043, "Invalid code_length=%1." },
	{ 1044, "MethodInfo-%1 unsupported flags=%2." },
	{ 1045, "Unsupported traits kind=%1." },
	{ 1046, "MethodInfo-%1 referenced before definition." },
	{ 1047, "No entry point was found." },
	{ 1049, "Prototype objects must be vanilla Objects." },
	{ 1050, "Cannot convert %1 to primitive." },
	{ 1051, "Illegal early binding access to %1." },
	{ 1052, "Invalid URI passed to %1 function." },
	{ 1053, "Illegal override of %1 in %2." },
	{ 1054, "Illegal range or target offsets in exception handler." },
	{ 1056, "Cannot create property %1 on %2." },
	{ 1057, "%1 can only contain methods." },
	{ 1058, "Illegal operand type: %1 must be %2." },
	{ 1059, "ClassInfo-%1 is referenced before definition." },
	{ 1060, "ClassInfo %1 exceeds class_count=%2." },
	{ 1061, "The value %1 cannot be converted to %2 without losing precision." },
	{ 1063, "Argument count mismatch on %1. Expected %2, got %3." },
	{ 1064, "Cannot call method %1 as constructor." },
	{ 1065, "Variable %1 is not defined." },
	{ 1066, "The form function('function body') is not supported." },
	{ 1067, "Native method %1 has illegal method body." },
	{ 1068, "%1 and %2 cannot be reconciled." },
	{ 1069, "Property %1 not found on %2 and there is no default value." },
	{ 1070, "Method %1 not found on %2" },
	{ 1071, "Function %1 has already been bound to %2." },
	{ 1072, "Disp_id 0 is illegal." },
	{ 1073, "Non-override method %1 replaced because of duplicate disp_id %2." },
	{ 1074, "Illegal write to read-only property %1 on %2." },
	{ 1075, "Math is not a function." },
	{ 1076, "Math is not a constructor." },
	{ 1077, "Illegal read of write-only property %1 on %2." },
	{ 1078, "Illegal opcode/multiname combination: %1<%2>." },
	{ 1079, "Native methods are not allowed in loaded code." },
	{ 1080, "Illegal value for namespace." },
	{ 1081, "Property %1 not found on %2 and there is no default value." },
	{ 1082, "No default namespace has been set." },
	{ 1083, "The prefix \"%1\" for element \"%2\" is not bound." },
	{ 1084, "Element or attribute (\"%1\") does not match QName production: QName::=(NCName':')?NCName." },
	{ 1085, "The element type \"%1\" must be terminated by the matching end-tag \"</%2>\"." },
	{ 1086, "The %1 method only works on lists containing one item." },
	{ 1087, "Assignment to indexed XML is not allowed." },
	{ 1088, "The markup in the document following the root element must be well-formed." },
	{ 1089, "Assignment to lists with more than one item is not supported." },
	{ 1090, "XML parser failure: element is malformed." },
	{ 1091, "XML parser failure: Unterminated CDATA section." },
	{ 1092, "XML parser failure: Unterminated XML declaration." },
	{ 1093, "XML parser failure: Unterminated DOCTYPE declaration." },
	{ 1094, "XML parser failure: Unterminated comment." },
	{ 1095, "XML parser failure: Unterminated attribute." },
	{ 1096, "XML parser failure: Unterminated element." },
	{ 1097, "XML parser failure: Unterminated processing instruction." },
	{ 1098, "Illegal prefix %1 for no namespace." },
	{ 1100, "Cannot supply flags when constructing one RegExp from another." },
	{ 1101, "Cannot verify method %1 with unknown scope." },
	{ 1102, "Illegal default value for type %1." },
	{ 1103, "Class %1 cannot extend final base class." },
	{ 1104, "Attribute \"%1\" was already specified for element \"%2\"." },
	{ 1107, "The ABC data is corrupt, attempt to read out of bounds." },
	{ 1108, "The OP_newclass opcode was used with the incorrect base class." },
	{ 1109, "Attempt to directly call unbound function %1 from method %2." },
	{ 1110, "%1 cannot extend %2." },
	{ 1111, "%1 cannot implement %2." },
	{ 1112, "Argument count mismatch on class coercion.  Expected 1, got %1." },
	{ 1113, "OP_newactivation used in method without NEED_ACTIVATION flag." },
	{ 1114, "OP_getglobalslot or OP_setglobalslot used with no global scope." },
	{ 1115, "%1 is not a constructor." },
	{ 1116, "second argument to Function.prototype.apply must be an array." },
	{ 1117, "Invalid XML name: %1." },
	{ 1118, "Illegal cyclical loop between nodes." },
	{ 1119, "Delete operator is not supported with operand of type %1." },
	{ 1120, "Cannot delete property %1 on %2." },
	{ 1121, "Method %1 has a duplicate method body." },
	{ 1122, "Interface method %1 has illegal method body." },
	{ 1123, "Filter operator not supported on type %1." },
	{ 1124, "OP_hasnext2 requires object and index to be distinct registers." },
	{ 1125, "The index %1 is out of range %2." },
	{ 1126, "Cannot change the length of a fixed Vector." },
	{ 1127, "Type application attempted on a non-parameterized type." },
	{ 1128, "Incorrect number of type parameters for %1. Expected %2, got %3." },
	{ 1129, "Cyclic structure cannot be converted to JSON string." },
	{ 1131, "Replacer argument to JSON stringifier must be an array or a two parameter function." },
	{ 1132, "Invalid JSON parse input." },
	{ 1500, "Error occurred opening file %1." },
	{ 1501, "Error occurred writing to file %1." },
	{ 1502, "A script has executed for longer than the default timeout period of 15 seconds." },
	{ 1503, "A script failed to exit after 30 seconds and was terminated." },
	{ 1504, "End of file." },
	{ 1505, "The string index %1 is out of bounds; must be in range %2 to %3." },
	{ 1506, "The specified range is invalid." },
	{ 1507, "Argument %1 cannot be null." },
	{ 1508, "The value specified for argument %1 is invalid." },
	{ 1510, "When the callback argument is a method of a class, the optional this argument must be null." },
	{ 1511, "Worker is already started." },
	{ 1512, "Starting a worker that already failed is not supported." },
	{ 1513, "Worker has terminated.\"" },
	{ 1514, "unlock() with no preceding matching lock()." },
	{ 1515, "Invalid condition timeout value: %1." },
	{ 1516, "Condition cannot notify if associated mutex is not owned." },
	{ 1517, "Condition cannot notifyAll if associated mutex is not owned." },
	{ 1518, "Condition cannot wait if associated mutex is not owned." },
	{ 1519, "Condition cannot be initialized." },
	{ 1520, "Mutex cannot be initialized." },
	{ 1521, "Only the worker's parent may call start." },
	{ 2000, "No active security context." },
	{ 2001, "Too few arguments were specified; got %1, %2 expected." },
	{ 2002, "Operation attempted on invalid socket." },
	{ 2003, "Invalid socket port number specified." },
	{ 2004, "One of the parameters is invalid." },
	{ 2005, "Parameter %1 is of the incorrect type. Should be type %2." },
	{ 2006, "The supplied index is out of bounds." },
	{ 2007, "Parameter %1 must be non-null." },
	{ 2008, "Parameter %1 must be one of the accepted values." },
	{ 2009, "This method cannot be used on a text field with a style sheet." },
	{ 2010, "Local-with-filesystem SWF files are not permitted to use sockets." },
	{ 2011, "Socket connection failed to %1:%2." },
	{ 2012, "%1 class cannot be instantiated." },
	{ 2013, "Feature can only be used in Flash Authoring." },
	{ 2014, "Feature is not available at this time." },
	{ 2015, "Invalid BitmapData." },
	{ 2017, "Only trusted local files may cause the Flash Player to exit." },
	{ 2018, "System.exit is only available in the standalone Flash Player." },
	{ 2019, "Depth specified is invalid." },
	{ 2020, "MovieClips objects with different parents cannot be swapped." },
	{ 2021, "Object creation failed." },
	{ 2022, "Class %1 must inherit from DisplayObject to link to a symbol." },
	{ 2023, "Class %1 must inherit from Sprite to link to the root." },
	{ 2024, "An object cannot be added as a child of itself." },
	{ 2025, "The supplied DisplayObject must be a child of the caller." },
	{ 2026, "An error occurred navigating to the URL %1." },
	{ 2027, "Parameter %1 must be a non-negative number; got %2." },
	{ 2028, "Local-with-filesystem SWF file %1 cannot access Internet URL %2." },
	{ 2029, "This URLStream object does not have a stream opened." },
	{ 2030, "End of file was encountered." },
	{ 2031, "Socket Error." },
	{ 2032, "Stream Error." },
	{ 2033, "Key Generation Failed." },
	{ 2034, "An invalid digest was supplied." },
	{ 2035, "URL Not Found." },
	{ 2036, "Load Never Completed." },
	{ 2037, "Functions called in incorrect sequence, or earlier call was unsuccessful." },
	{ 2038, "File I/O Error." },
	{ 2039, "Invalid remote URL protocol. The remote URL protocol must be HTTP or HTTPS." },
	{ 2041, "Only one file browsing session may be performed at a time." },
	{ 2042, "The digest property is not supported by this load operation." },
	{ 2044, "Unhandled %1:." },
	{ 2046, "The loaded file did not have a valid signature." },
	{ 2047, "Security sandbox violation: %1: %2 cannot access %3." },
	{ 2048, "Security sandbox violation: %1 cannot load data from %2." },
	{ 2049, "Security sandbox violation: %1 cannot upload data to %2." },
	{ 2051, "Security sandbox violation: %1 cannot evaluate scripting URLs within %2 (allowScriptAccess is %3). Attempted URL was %4." },
	{ 2052, "Only String arguments are permitted for allowDomain and allowInsecureDomain." },
	{ 2053, "Security sandbox violation: %1 cannot clear an interval timer set by %2." },
	{ 2054, "The value of Security.exactSettings cannot be changed after it has been used." },
	{ 2055, "The print job could not be started." },
	{ 2056, "The print job could not be sent to the printer." },
	{ 2057, "The page could not be added to the print job." },
	{ 2058, "There was an error decompressing the data." },
	{ 2059, "Security sandbox violation: %1 cannot overwrite an ExternalInterface callback added by %2." },
	{ 2060, "Security sandbox violation: ExternalInterface caller %1 cannot access %2." },
	{ 2061, "No ExternalInterface callback %1 registered." },
	{ 2062, "Children of Event must override clone() {return new MyEventClass (...);}." },
	{ 2063, "Error attempting to execute IME command." },
	{ 2065, "The focus cannot be set for this target." },
	{ 2066, "The Timer delay specified is out of range." },
	{ 2067, "The ExternalInterface is not available in this container. ExternalInterface requires Internet Explorer ActiveX, Firefox, Mozilla 1.7.5 and greater, or other browsers that support NPRuntime." },
	{ 2068, "Invalid sound." },
	{ 2069, "The Loader class does not implement this method." },
	{ 2070, "Security sandbox violation: caller %1 cannot access Stage owned by %2." },
	{ 2071, "The Stage class does not implement this property or method." },
	{ 2073, "There was a problem saving the application to disk." },
	{ 2074, "The stage is too small to fit the download ui." },
	{ 2075, "The downloaded file is invalid." },
	{ 2077, "This filter operation cannot be performed with the specified input parameters." },
	{ 2078, "The name property of a Timeline-placed object cannot be modified." },
	{ 2079, "Classes derived from Bitmap can only be associated with defineBits characters (bitmaps)." },
	{ 2082, "Connect failed because the object is already connected." },
	{ 2083, "Close failed because the object is not connected." },
	{ 2084, "The AMF encoding of the arguments cannot exceed 40K." },
	{ 2085, "Parameter %1 must be non-empty string." },
	{ 2086, "A setting in the mms.cfg file prohibits this FileReference request." },
	{ 2087, "The FileReference.download() file name contains prohibited characters." },
	{ 2088, "The Proxy class does not implement getProperty. It must be overridden by a subclass." },
	{ 2089, "The Proxy class does not implement setProperty. It must be overridden by a subclass." },
	{ 2090, "The Proxy class does not implement callProperty. It must be overridden by a subclass." },
	{ 2091, "The Proxy class does not implement hasProperty. It must be overridden by a subclass." },
	{ 2092, "The Proxy class does not implement deleteProperty. It must be overridden by a subclass." },
	{ 2093, "The Proxy class does not implement getDescendants. It must be overridden by a subclass." },
	{ 2094, "Event dispatch recursion overflow." },
	{ 2095, "%1 was unable to invoke callback %2." },
	{ 2096, "The HTTP request header %1 cannot be set via ActionScript." },
	{ 2097, "The FileFilter Array is not in the correct format." },
	{ 2098, "The loading object is not a .swf file, you cannot request SWF properties from it." },
	{ 2099, "The loading object is not sufficiently loaded to provide this information." },
	{ 2100, "The ByteArray parameter in Loader.loadBytes() must have length greater than 0." },
	{ 2101, "The String passed to URLVariables.decode() must be a URL-encoded query string containing name/value pairs." },
	{ 2102, "The before XMLNode parameter must be a child of the caller." },
	{ 2103, "XML recursion failure: new child would create infinite loop." },
	{ 2105, "The Proxy class does not implement nextNameIndex. It must be overridden by a subclass." },
	{ 2106, "The Proxy class does not implement nextName. It must be overridden by a subclass." },
	{ 2107, "The Proxy class does not implement nextValue. It must be overridden by a subclass." },
	{ 2108, "Scene %1 was not found." },
	{ 2109, "Frame label %1 not found in scene %2." },
	{ 2110, "The value of Security.disableAVM1Loading cannot be set unless the caller can access the stage and is in an ActionScript 3.0 SWF file." },
	{ 2111, "Security.disableAVM1Loading is true so the current load of the ActionScript 1.0/2.0 SWF file has been blocked." },
	{ 2112, "Provided parameter LoaderContext.ApplicationDomain is from a disallowed domain." },
	{ 2113, "Provided parameter LoaderContext.SecurityDomain is from a disallowed domain." },
	{ 2114, "Parameter %1 must be null." },
	{ 2115, "Parameter %1 must be false." },
	{ 2116, "Parameter %1 must be true." },
	{ 2118, "The LoaderInfo class does not implement this method." },
	{ 2119, "Security sandbox violation: caller %1 cannot access LoaderInfo.applicationDomain owned by %2." },
	{ 2121, "Security sandbox violation: %1: %2 cannot access %3. This may be worked around by calling Security.allowDomain." },
	{ 2122, "Security sandbox violation: %1: %2 cannot access %3. A policy file is required, but the checkPolicyFile flag was not set when this media was loaded." },
	{ 2123, "Security sandbox violation: %1: %2 cannot access %3. No policy files granted access." },
	{ 2124, "Loaded file is an unknown type." },
	{ 2125, "Security sandbox violation: %1 cannot use Runtime Shared Library %2 because crossing the boundary between ActionScript 3.0 and ActionScript 1.0/2.0 objects is not allowed." },
	{ 2126, "NetConnection object must be connected." },
	{ 2127, "FileReference POST data cannot be type ByteArray." },
	{ 2129, "Connection to %1 failed." },
	{ 2130, "Unable to flush SharedObject." },
	{ 2131, "Definition %1 cannot be found." },
	{ 2132, "NetConnection.connect cannot be called from a netStatus event handler." },
	{ 2133, "Callback %1 is not registered." },
	{ 2134, "Cannot create SharedObject." },
	{ 2136, "The SWF file %1 contains invalid data." },
	{ 2137, "Security sandbox violation: %1 cannot navigate window %2 within %3 (allowScriptAccess is %4). Attempted URL was %5." },
	{ 2138, "Rich text XML could not be parsed." },
	{ 2139, "SharedObject could not connect." },
	{ 2140, "Security sandbox violation: %1 cannot load %2. Local-with-filesystem and local-with-networking SWF files cannot load each other." },
	{ 2141, "Only one PrintJob may be in use at a time." },
	{ 2142, "Security sandbox violation: local SWF files cannot use the LoaderContext.securityDomain property. %1 was attempting to load %2." },
	{ 2143, "AccessibilityImplementation.get_accRole() must be overridden from its default." },
	{ 2144, "AccessibilityImplementation.get_accState() must be overridden from its default." },
	{ 2145, "Cumulative length of requestHeaders must be less than 8192 characters." },
	{ 2146, "Security sandbox violation: %1 cannot call %2 because the HTML/container parameter allowNetworking has the value %3." },
	{ 2147, "Forbidden protocol in URL %1." },
	{ 2148, "SWF file %1 cannot access local resource %2. Only local-with-filesystem and trusted local SWF files may access local resources." },
	{ 2149, "Security sandbox violation: %1 cannot make fscommand calls to %2 (allowScriptAccess is %3)." },
	{ 2150, "An object cannot be added as a child to one of it's children (or children's children, etc.)." },
	{ 2151, "You cannot enter full screen mode when the settings dialog is visible." },
	{ 2152, "Full screen mode is not allowed." },
	{ 2153, "The URLRequest.requestHeaders array must contain only non-NULL URLRequestHeader objects." },
	{ 2154, "The NetStream Object is invalid.  This may be due to a failed NetConnection." },
	{ 2155, "The ExternalInterface.call functionName parameter is invalid.  Only alphanumeric characters are supported." },
	{ 2156, "Port %1 may not be accessed using protocol %2. Calling SWF was %3." },
	{ 2157, "Rejecting URL %1 because the 'asfunction:' protocol may only be used for link targets, not for networking APIs." },
	{ 2158, "The NetConnection Object is invalid.  This may be due to a dropped NetConnection." },
	{ 2159, "The SharedObject Object is invalid." },
	{ 2160, "The TextLine is INVALID and cannot be used to access the current state of the TextBlock." },
	{ 2161, "An internal error occured while laying out the text." },
	{ 2162, "The Shader output type is not compatible for this operation." },
	{ 2163, "The Shader input type %1 is not compatible for this operation." },
	{ 2164, "The Shader input %1 is missing or an unsupported type." },
	{ 2165, "The Shader input %1 does not have enough data." },
	{ 2166, "The Shader input %1 lacks valid dimensions." },
	{ 2167, "The Shader does not have the required number of inputs for this operation." },
	{ 2168, "Static text lines have no atoms and no reference to a text block." },
	{ 2169, "The method %1 may not be used for browser scripting.  The URL %2 requested by %3 is being ignored.  If you intend to call browser script, use navigateToURL instead." },
	{ 2170, "Security sandbox violation: %1 cannot send HTTP headers to %2." },
	{ 2171, "The Shader object contains no byte code to execute." },
	{ 2172, "The ShaderJob is already running or finished." },
	{ 2173, "Unable to read object in stream.  The class %1 does not implement flash.utils.IExternalizable but is aliased to an externalizable class." },
	{ 2174, "Only one download, upload, load or save operation can be active at a time on each FileReference." },
	{ 2175, "One or more elements of the content of the TextBlock has a null ElementFormat." },
	{ 2176, "Certain actions, such as those that display a pop-up window, may only be invoked upon user interaction, for example by a mouse click or button press." },
	{ 2177, "The Shader input %1 is too large." },
	{ 2178, "The Clipboard.generalClipboard object must be used instead of creating a new Clipboard." },
	{ 2179, "The Clipboard.generalClipboard object may only be read while processing a flash.events.Event.PASTE event." },
	{ 2180, "It is illegal to move AVM1 content (AS1 or AS2) to a different part of the displayList when it has been loaded into AVM2 (AS3) content." },
	{ 2181, "The TextLine class does not implement this property or method." },
	{ 2182, "Invalid fieldOfView value.  The value must be greater than 0 and less than 180." },
	{ 2183, "Scale values must not be zero." },
	{ 2184, "The ElementFormat object is locked and cannot be modified." },
	{ 2185, "The FontDescription object is locked and cannot be modified." },
	{ 2186, "Invalid focalLength %1." },
	{ 2187, "Invalid orientation style %1.  Value must be one of 'Orientation3D.EULER_ANGLES', 'Orientation3D.AXIS_ANGLE', or 'Orientation3D.QUATERNION'." },
	{ 2188, "Invalid raw matrix. Matrix must be invertible." },
	{ 2189, "A Matrix3D can not be assigned to more than one DisplayObject." },
	{ 2190, "The attempted load of %1 failed as it had a Content-Disposition of attachment set." },
	{ 2191, "The Clipboard.generalClipboard object may only be written to as the result of user interaction, for example by a mouse click or button press." },
	{ 2192, "An unpaired Unicode surrogate was encountered in the input." },
	{ 2193, "Security sandbox violation: %1: %2 cannot access %3." },
	{ 2194, "Parameter %1 cannot be a Loader." },
	{ 2195, "Error thrown as Loader called %1." },
	{ 2196, "Parameter %1 must be an Object with only String values." },
	{ 2200, "The SystemUpdater class is not supported by this player." },
	{ 2201, "The requested update type is not supported on this operating system." },
	{ 2202, "Only one SystemUpdater action is allowed at a time." },
	{ 2203, "The requested SystemUpdater action cannot be completed." },
	{ 2204, "This operation cannot be canceled because it is waiting for user interaction." },
	{ 2205, "Invalid update type %1." },
	{ 2500, "An error occurred decrypting the signed swf file. The swf will not be loaded." },
	{ 2501, "This property can only be accessed during screen sharing." },
	{ 2502, "This property can only be accessed if sharing the entire screen." },
	{ 3000, "Illegal path name." },
	{ 3001, "File or directory access denied." },
	{ 3002, "File or directory exists." },
	{ 3003, "File or directory does not exist." },
	{ 3004, "Insufficient file space." },
	{ 3005, "Insufficient system resources." },
	{ 3006, "Not a file." },
	{ 3007, "Not a directory." },
	{ 3008, "Read-only or write-protected media." },
	{ 3009, "Cannot move file or directory to a different device." },
	{ 3010, "Directory is not empty." },
	{ 3011, "Move or copy destination already exists." },
	{ 3012, "Cannot delete file or directory." },
	{ 3013, "File or directory is in use." },
	{ 3014, "Cannot copy or move a file or directory to overwrite a containing directory." },
	{ 3015, "Loader.loadBytes() is not permitted to load content with executable code." },
	{ 3016, "No application was found that can open this file." },
	{ 3100, "A SQLConnection cannot be closed while statements are still executing." },
	{ 3101, "Database connection is already open." },
	{ 3102, "Name argument specified was invalid. It must not be null or empty." },
	{ 3103, "Operation cannot be performed while there is an open transaction on this connection." },
	{ 3104, "A SQLConnection must be open to perform this operation." },
	{ 3105, "Operation is only allowed if a connection has an open transaction." },
	{ 3106, "Property cannot be changed while SQLStatement.executing is true." },
	{ 3107, "%1 may not be called unless SQLResult.complete is false." },
	{ 3108, "Operation is not permitted when the SQLStatement.text property is not set." },
	{ 3109, "Operation is not permitted when the SQLStatement.sqlConnection property is not set." },
	{ 3110, "Operation cannot be performed while SQLStatement.executing is true." },
	{ 3111, "An invalid schema type was specified." },
	{ 3112, "An invalid transaction lock type was specified." },
	{ 3113, "Reference specified is not of type File." },
	{ 3114, "An invalid open mode was specified." },
	{ 3115, "SQL Error." },
	{ 3116, "An internal logic error occurred." },
	{ 3117, "Access permission denied." },
	{ 3118, "Operation aborted." },
	{ 3119, "Database file is currently locked." },
	{ 3120, "Table is locked." },
	{ 3121, "Out of memory." },
	{ 3122, "Attempt to write a readonly database." },
	{ 3123, "Database disk image is malformed." },
	{ 3124, "Insertion failed because database is full." },
	{ 3125, "Unable to open the database file." },
	{ 3126, "Database lock protocol error." },
	{ 3127, "Database is empty." },
	{ 3128, "Disk I/O error occurred." },
	{ 3129, "The database schema changed." },
	{ 3130, "Too much data for one row of a table." },
	{ 3131, "Abort due to constraint violation." },
	{ 3132, "Data type mismatch." },
	{ 3133, "An internal error occurred." },
	{ 3134, "Feature not supported on this operating system." },
	{ 3135, "Authorization denied." },
	{ 3136, "Auxiliary database format error." },
	{ 3137, "An index specified for a parameter was out of range." },
	{ 3138, "File opened is not a database file." },
	{ 3139, "The page size specified was not valid for this operation." },
	{ 3140, "The encryption key size specified was not valid for this operation. Keys must be exactly 16 bytes in length" },
	{ 3141, "The requested database configuration is not supported." },
	{ 3143, "Unencrypted databases may not be reencrypted." },
	{ 3200, "Cannot perform operation on closed window." },
	{ 3201, "Adobe Reader cannot be found." },
	{ 3202, "Adobe Reader 8.1 or later cannot be found." },
	{ 3203, "Default Adobe Reader must be version 8.1 or later." },
	{ 3204, "An error ocurred trying to load Adobe Reader." },
	{ 3205, "Only application-sandbox content can access this feature." },
	{ 3206, "Caller %1 cannot set LoaderInfo property %2." },
	{ 3207, "Application-sandbox content cannot access this feature." },
	{ 3208, "Attempt to access invalid clipboard." },
	{ 3209, "Attempt to access dead clipboard." },
	{ 3210, "The application attempted to reference a JavaScript object in a HTML page that is no longer loaded." },
	{ 3211, "Drag and Drop File Promise error: %1" },
	{ 3212, "Cannot perform operation on a NativeProcess that is not running." },
	{ 3213, "Cannot perform operation on a NativeProcess that is already running." },
	{ 3214, "NativeProcessStartupInfo.executable does not specify a valid executable file." },
	{ 3215, "NativeProcessStartupInfo.workingDirectory does not specify a valid directory." },
	{ 3216, "Error while reading data from NativeProcess.standardOutput." },
	{ 3217, "Error while reading data from NativeProcess.standardError." },
	{ 3218, "Error while writing data to NativeProcess.standardInput." },
	{ 3219, "The NativeProcess could not be started. '%1'" },
	{ 3220, "Action '%1' not allowed in current security context '%2'." },
	{ 3221, "Adobe Flash Player cannot be found." },
	{ 3222, "The installed version of Adobe Flash Player is too old." },
	{ 3223, "DNS lookup error: platform error %1" },
	{ 3224, "Socket message too long" },
	{ 3225, "Cannot send data to a location when connected." },
	{ 3226, "Cannot import a SWF file when LoaderContext.allowCodeImport is false." },
	{ 3227, "Cannot launch another application from background." },
	{ 3228, "StageWebView encountered an error during the load operation." },
	{ 3229, "The protocol is not supported.:" },
	{ 3230, "The browse operation is unsupported." },
	{ 3300, "Voucher is invalid." },
	{ 3301, "User authentication failed." },
	{ 3302, "Flash Access server does not support SSL." },
	{ 3303, "Content expired." },
	{ 3304, "User authorization failed (for example, the user has not purchased the content)." },
	{ 3305, "Can't connect to the server." },
	{ 3306, "Client update required (Flash Access server requires new client)." },
	{ 3307, "Generic internal Flash Access failure." },
	{ 3308, "Wrong voucher key." },
	{ 3309, "Video content is corrupted." },
	{ 3310, "The AIR application or Flash Player SWF does not match the one specified in the DRM policy." },
	{ 3311, "The version of the application does not match the one specified in the DRM policy." },
	{ 3312, "Verification of voucher failed." },
	{ 3313, "Write to the file system failed." },
	{ 3314, "Verification of FLV/F4V header file failed." },
	{ 3315, "The current security context does not allow this operation." },
	{ 3316, "The value of LocalConnection.isPerUser cannot be changed because it has already been locked by a call to LocalConnection.connect, .send, or .close." },
	{ 3317, "Failed to load Flash Access module." },
	{ 3318, "Incompatible version of Flash Access module found." },
	{ 3319, "Missing Flash Access module API entry point." },
	{ 3320, "Generic internal Flash Access failure." },
	{ 3321, "Individualization failed." },
	{ 3322, "Device binding failed." },
	{ 3323, "The internal stores are corrupted." },
	{ 3324, "Reset license files and the client will fetch a new machine token." },
	{ 3325, "Internal stores are corrupt." },
	{ 3326, "Call customer support." },
	{ 3327, "Clock tampering detected." },
	{ 3328, "Server error; retry the request." },
	{ 3329, "Error in application-specific namespace." },
	{ 3330, "Need to authenticate the user and reacquire the voucher." },
	{ 3331, "Content is not yet valid." },
	{ 3332, "Cached voucher has expired. Reacquire the voucher from the server." },
	{ 3333, "The playback window for this policy has expired." },
	{ 3334, "This platform is not allowed to play this content." },
	{ 3335, "Invalid version of Flash Access module. Upgrade AIR or Flash Access module for the Flash Player." },
	{ 3336, "This platform is not allowed to play this content." },
	{ 3337, "Upgrade Flash Player or AIR  and retry playback." },
	{ 3338, "Unknown connection type." },
	{ 3339, "Can't play back on analog device. Connect to a digital device." },
	{ 3340, "Can't play back because connected analog device doesn't have the correct capabilities." },
	{ 3341, "Can't play back on digital device." },
	{ 3342, "The connected digital device doesn't have the correct capabilities." },
	{ 3343, "Internal Error." },
	{ 3344, "Missing Flash Access module." },
	{ 3345, "This operation is not permitted with content protected using Flash Access." },
	{ 3346, "Failed migrating local DRM data, all locally cached DRM vouchers are lost." },
	{ 3347, "The device does not meet the Flash Access server's playback device constraints." },
	{ 3348, "This protected content is expired." },
	{ 3349, "The Flash Access server is running at a version that's higher than the max supported by this runtime." },
	{ 3350, "The Flash Access server is running at a version that's lower than the min supported by this runtime." },
	{ 3351, "Device Group registration token is corrupted, please refresh the token by registering again to the DRMDeviceGroup." },
	{ 3352, "The server is using a newer version of the registration token for this Device Group. Please refresh the token by registering again to the DRMDeviceGroup." },
	{ 3353, "the server is using an older version of the registration token for this Device Group." },
	{ 3354, "Device Group registration is expired, please refresh the token by registering again to the DRMDeviceGroup." },
	{ 3355, "The server denied this Device Group registration request." },
	{ 3356, "The root voucher for this content's DRMVoucher was not found." },
	{ 3357, "The DRMContentData provides no valid embedded voucher and no Flash Access server url to acquire the voucher from." },
	{ 3358, "ACP protection is not available on the device but required to playback the content." },
	{ 3359, "CGMSA protection is not available on the device but required to playback the content." },
	{ 3360, "Device Group registration is required before doing this operation." },
	{ 3361, "The device is not registered to this Device Group." },
	{ 3362, "Asynchronous operation took longer than maxOperationTimeout." },
	{ 3363, "The M3U8 playlist passed in had unsupported content." },
	{ 3364, "The framework requested the device ID, but the returned value was empty." },
	{ 3365, "This browser/platform combination does not allow DRM protected playback when in incognito mode." },
	{ 3366, "The host runtime called the Access library with a bad parameter." },
	{ 3367, "M3U8 manifest signing failed." },
	{ 3368, "The user cancelled the operation, or has entered settings that disallow access to the system." },
	{ 3369, "A required browser interface is not available." },
	{ 3370, "The user has disabled the \"Allow identifiers for protected content\" setting." },
	{ 3400, "An error occured while executing JavaScript code." },
	{ 3401, "Security sandbox violation: An object with this name has already been registered from another security domain." },
	{ 3402, "Security sandbox violation: Bridge caller %1 cannot access %2." },
	{ 3500, "The extension context does not have a method with the name %1." },
	{ 3501, "The extension context has already been disposed." },
	{ 3502, "The extension returned an invalid value." },
	{ 3503, "The extension was left in an invalid state." },
	{ 3600, "No valid program set." },
	{ 3601, "No valid index buffer set." },
	{ 3602, "Sanity check on parameters failed, %1 triangles and %2 index offset." },
	{ 3603, "Not enough indices in this buffer. %1 triangles at offset %2, but there are only %3 indices in buffer." },
	{ 3604, "Sampler %1 binds a texture that is also bound for render to texture." },
	{ 3605, "Sampler %1 binds an invalid texture." },
	{ 3606, "Sampler %1 format does not match texture format." },
	{ 3607, "Stream %1 is set but not used by the current vertex program." },
	{ 3608, "Stream %1 is invalid." },
	{ 3609, "Stream %1 does not have enough vertices." },
	{ 3610, "Stream %1 vertex offset is out of bounds" },
	{ 3611, "Stream %1 is read by the current vertex program but not set." },
	{ 3612, "Programs must be in little endian format." },
	{ 3613, "The native shader compilation failed." },
	{ 3614, "The native shader compilation failed.enGL specific: %1" },
	{ 3615, "AGAL validation failed: Program size below minimum length for %1 program." },
	{ 3616, "AGAL validation failed: Not an AGAL program. Wrong magic byte for %1 program." },
	{ 3617, "AGAL validation failed: Bad AGAL version for %1 program. Current version is %2." },
	{ 3618, "AGAL validation failed: Bad AGAL program type identifier for %1 program." },
	{ 3619, "AGAL validation failed: Shader type must be either fragment or vertex for %1 program." },
	{ 3620, "AGAL validation failed: Invalid opcode, value out of range: %2 at token %3 of %1 program." },
	{ 3621, "AGAL validation failed: Invalid opcode, %2 is not implemented in this version at token %3 of %1 program." },
	{ 3622, "AGAL validation failed: Opcode %2 only allowed in fragment programs at token %3 of %1 program." },
	{ 3623, "AGAL validation failed: Block nesting underflow - EIF without opening IF condition. At token %3 of %1 program." },
	{ 3624, "AGAL validation failed: Block nesting overflow. Too many nested IF blocks. At token %3 of %1 program." },
	{ 3625, "AGAL validation failed: Bad AGAL source operands. Both are constants (this must be precomputed) at token %2 of %1 program." },
	{ 3626, "AGAL validation failed: Opcode %2, both operands are indirect reads at token %3 of %1 program." },
	{ 3627, "AGAL validation failed: Opcode %2 destination operand must be all zero at token %3 of %1 program." },
	{ 3628, "AGAL validation failed: The destination operand for the %2 instruction must mask w (use .xyz or less) at token %3 of %1 program." },
	{ 3629, "AGAL validation failed: Too many tokens (%2) for %1 program." },
	{ 3630, "Fragment shader type is not fragment." },
	{ 3631, "Vertex shader type is not vertex." },
	{ 3632, "AGAL linkage: Varying %1 is read in the fragment shader but not written to by the vertex shader." },
	{ 3633, "AGAL linkage: Varying %1 is only partially written to. Must write all four components." },
	{ 3634, "AGAL linkage: Fragment output needs to write to all components." },
	{ 3635, "AGAL linkage: Vertex output needs to write to all components." },
	{ 3636, "AGAL validation failed: Unused operand is not set to zero for %2 at token %3 of %1 program." },
	{ 3637, "AGAL validation failed: Sampler registers only allowed in fragment programs for %2 at token %3 of %1 program." },
	{ 3638, "AGAL validation failed: Sampler register only allowed as second operand in texture instructions for %2 at token %3 of %1 program." },
	{ 3639, "AGAL validation failed: Indirect addressing only allowed in vertex programs for %2 at token %3 of %1 program." },
	{ 3640, "AGAL validation failed: Indirect addressing only allowed into constant registers for %2 at token %3 of %1 program." },
	{ 3641, "AGAL validation failed: Indirect addressing not allowed for this operand in this instruction for %2 at token %3 of %1 program." },
	{ 3642, "AGAL validation failed: Indirect source type must be attribute, constant or temporary for %2 at token %3 of %1 program." },
	{ 3643, "AGAL validation failed: Indirect addressing fields must be zero for direct addressing for %2 at token %3 of %1 program." },
	{ 3644, "AGAL validation failed: Varying registers can only be read in fragment programs for %2 at token %3 of %1 program." },
	{ 3645, "AGAL validation failed: Attribute registers can only be read in vertex programs for %2 at token %3 of %1 program." },
	{ 3646, "AGAL validation failed: Can not read from output register for %2 at token %3 of %1 program." },
	{ 3647, "AGAL validation failed: Temporary register read without being written to for %2 at token %3 of %1 program." },
	{ 3648, "AGAL validation failed: Temporary register component read without being written to for %2 at token %3 of %1 program." },
	{ 3649, "AGAL validation failed: Sampler registers can not be written to for %2 at token %3 of %1 program." },
	{ 3650, "AGAL validation failed: Varying registers can only be written in vertex programs for %2 at token %3 of %1 program." },
	{ 3651, "AGAL validation failed: Attribute registers can not be written to for %2 at token %3 of %1 program." },
	{ 3652, "AGAL validation failed: Constant registers can not be written to for %2 at token %3 of %1 program." },
	{ 3653, "AGAL validation failed: Destination writemask is zero for %2 at token %3 of %1 program." },
	{ 3654, "AGAL validation failed: Reserve bits should be zero for %2 at token %3 of %1 program." },
	{ 3655, "AGAL validation failed: Unknown register type for %2 at token %3 of %1 program." },
	{ 3656, "AGAL validation failed: Sampler register index out of bounds for %2 at token %3 of %1 program." },
	{ 3657, "AGAL validation failed: Varying register index out of bounds for %2 at token %3 of %1 program." },
	{ 3658, "AGAL validation failed: Attribute register index out of bounds for %2 at token %3 of %1 program." },
	{ 3659, "AGAL validation failed: Constant register index out of bounds for %2 at token %3 of %1 program." },
	{ 3660, "AGAL validation failed: Output register index out of bounds for %2 at token %3 of %1 program." },
	{ 3661, "AGAL validation failed: Temporary register index out of bounds for %2 at token %3 of %1 program." },
	{ 3662, "AGAL validation failed: Cube map samplers must set wrapping to clamp mode for %2 at token %3 of %1 program." },
	{ 3663, "Sampler %1 binds an undefined texture." },
	{ 3664, "AGAL validation failed: Unknown sampler dimension %4 for %2 at token %3 of %1 program." },
	{ 3665, "AGAL validation failed: Unknown filter mode in sampler: %4 for %2 at token %3 of %1 program." },
	{ 3666, "AGAL validation failed: Unknown mipmap mode in sampler: %4 for %2 at token %3 of %1 program." },
	{ 3667, "AGAL validation failed: Unknown wrapping mode in sampler: %4 for %2 at token %3 of %1 program." },
	{ 3668, "AGAL validation failed: Unknown special flag used in sampler: %4 for %2 at token %3 of %1 program." },
	{ 3669, "Bad input size." },
	{ 3670, "Buffer too big." },
	{ 3671, "Buffer has zero size." },
	{ 3672, "Buffer creation failed. Internal error." },
	{ 3673, "Cube side must be [0..5]." },
	{ 3674, "Miplevel too large." },
	{ 3675, "Texture format mismatch." },
	{ 3676, "Platform does not support desired texture format." },
	{ 3677, "Texture decoding failed. Internal error." },
	{ 3678, "Texture needs to be square." },
	{ 3679, "Texture size does not match." },
	{ 3680, "Depth texture not implemented yet." },
	{ 3681, "Texture size is zero." },
	{ 3682, "Texture size not a power of two." },
	{ 3683, "Texture too big (max is %1x%1)." },
	{ 3684, "Texture creation failed. Internal error." },
	{ 3685, "Could not create renderer." },
	{ 3686, "'disabled' format only valid with a null vertex buffer." },
	{ 3687, "Null vertex buffers require the 'disabled' format." },
	{ 3688, "You must add an event listener for the context3DCreate event before requesting a new Context3D." },
	{ 3689, "You can not swizzle second operand for %2 at token %3 of %1 program." },
	{ 3690, "Too many draw calls before calling present." },
	{ 3691, "Resource limit for this resource type exceeded." },
	{ 3692, "All buffers need to be cleared every frame before drawing." },
	{ 3693, "AGAL validation failed: Sampler register must be used for second operand in texture instructions for %2 at token %3 of %1 program." },
	{ 3694, "The object was disposed by an earlier call of dispose() on it." },
	{ 3695, "A texture can only be bound to multiple samplers if the samplers also have the exact same properties. Mismatch at samplers %1 and %2." },
	{ 3696, "AGAL validation failed: Second use of sampler register needs to specify the exact same properties. At token %3 of %1 program." },
	{ 3697, "A texture is bound on sampler %1 but not used by the fragment program." },
	{ 3698, "The back buffer is not configured." },
	{ 3699, "Requested Operation failed to complete" },
	{ 3700, "A texture sampler binds an incomplete texture. Make sure to upload(). All miplevels are required when mipmapping is enabled." },
	{ 3701, "The output color register can not use a write mask. All components must be written." },
	{ 3702, "Context3D not available." },
	{ 3703, "AGAL validation failed: Source swizzle must be scalar (one of: xxxx, yyyy, zzzz, wwww) for %2 at token %3 of %1 program." },
	{ 3704, "AGAL validation failed: Cube map samplers must enable mipmapping for %2 at token %3 of %1 program." },
	{ 3705, "Cubemap texture too big (max is 1024x1024)." },
	{ 3706, "Scissor rectangle is set but does not intersect the framebuffer." },
	{ 3707, "Property can not be set in non full screen mode." },
	{ 3708, "Feature not available on this platform." },
	{ 3709, "The depthAndStencil flag in the application descriptor must match the enableDepthAndStencil Boolean passed to configureBackBuffer on the Context3D object." },
	{ 3710, "Requested Stage3D Operation failed to complete." },
	{ 3711, "The streaming levels is too large." },
	{ 3712, "Rendering to streaming textures is not allowed." },
	{ 3713, "Incomplete streaming texture (base level not uploaded) used with no mip sampling." },
	{ 3714, "ApplicationDomain.domainMemory is not available." },
	{ 3715, "Too many instructions used in native shader. Detected %3 but can only support %2 for %1 program." },
	{ 3716, "Too many ALU instructions in native shader. Detected %3 but can only support %2 for %1 program." },
	{ 3717, "Too many texture instructions in native shader. Detected %3 but can only support %2 for %1 program." },
	{ 3718, "Too many constants used in native shader. Detected %3 but can only support %2 for %1 program." },
	{ 3719, "Too many temporary registers used in native shader. Detected %3 but can only support %2 for %1 program." },
	{ 3720, "Too many varying registers used in native shader. Detected %3 but can only support %2 for %1 program." },
	{ 3721, "Too many indirect texture reads in native shader. Detected %3 but can only support %2 for %1 program." },
	{ 3722, "Event.FRAME_LABEL event can only be registered with FrameLabel object." },
	{ 3723, "Invalid Context3D bounds. Context3D instance bounds must be contained within Stage bounds in constrained mode. Requested Context3D bounds were (%1,%2,%3,%4), stage bounds are (%5,%6,%7,%8)." },
	{ 3724, "This call requires a Context3D that is created with the standard profile." },
	{ 3725, "The requested AGAL version (%2) is not valid under the Context3D profile. For example AGAL version 2 requires standard profile." },
	{ 3726, "AGAL validation failed: Opcode %2 requires AGAL version to be at least 2, at token %3 of %1 program." },
	{ 3727, "Failed to obtain authorization token." },
	{ 3728, "When rendering to multiple textures slot 0 must be active. When rendering to the back buffer all render to texture slots must be disabled." },
	{ 3729, "When rendering to multiple textures all textures must have the same dimension and render settings." },
	{ 3730, "When rendering to multiple textures the same texture (or cube map face) may not be bound into multiple slots." },
	{ 3731, "This feature is not available within this context." },
	{ 3732, "Worker.terminate is only available for background workers." },
	{ 3735, "This API cannot accept shared ByteArrays." },
	{ 3736, "MessageChannel is not a sender." },
	{ 3737, "MessageChannel is not a receiver." },
	{ 3738, "MessageChannel is closed." },
	{ 3739, "AGAL validation failed: Open conditional block at end of %1 program." },
	{ 3740, "AGAL validation failed: Texture samplers used in the TED instruction can not specify a lod bias. At token %3 of %1 program." },
	{ 3741, "AGAL validation failed: TEX instructions in an if block can not use computed texture coordinates. Either use interpolated texture coordinates or use the TED instruction instead. At token %3 of %1 program." },
	{ 3742, "AGAL validation failed: DDX and DDY opcodes are not allowed inside conditional blocks. At token %3 of %1 program." },
	{ 3743, "AGAL validation failed: The TED opcode must enable mip mapping. At token %3 of %1 program." },
	{ 3744, "AGAL validation failed: Color output written to multiple times. At token %3 of %1 program." },
	{ 3745, "Compressed texture size is too small. The minimum size for compressed textures is 4x4." },
	{ 3746, "Rendering to compressed textures is not allowed." },
	{ 3747, "Multiple application domains are not supported on this operating system." },
	{ 3748, "AGAL validation failed: Empty conditional branch in AGAL of %1 program." },
	{ 3749, "AGAL validation failed: Depth output register index out of bounds for %2 at token %3 of %1 program." },
	{ 3750, "AGAL validation failed: Depth output register is only available in fragment programs." },
	{ 3751, "AGAL validation failed: Output registers can not be written inside conditionals." },
	{ 3752, "AGAL validation failed: Broken else chain." },
	{ 3753, "Rectangle or cube textures require textures sampling to be set to clamp." },
	{ 3754, "Texture sampler dimensions mismatch. The AGAL declaration has to match the texture used." },
	{ 3755, "Rectangle textures have to disable mip mapping and can not have a lod bias set." },
	{ 3756, "AGAL validation failed: Depth output must set only x as a write mask. At token %3 of %1 program." },
	{ 3757, "AGAL validation failed: Vertex and fragment program need to have the same version." },
	{ 3758, "AGAL validation failed: Conditional source are exactly the same, condition is constant. At token %3 of %1 program." },
	{ 3759, "The selected texture format is not valid in this profile." },
	{ 3760, "The color output index is out of range." },
	{ 3761, "The bit depth of all textures used for render to texture must be exactly the same." },
	{ 3762, "This texture format is not supported for rectangle textures." },
	{ 3763, "Sampler %1 binds a texture that that does not match the read mode specified in AGAL. Reading compressed or single/dual channel textures must be explicitly declared." },
	{ 3764, "Reloading a SWF is not supported on this operating system." },
	{ 3765, "This call requires a Context3D that is created with the baseline or baselineExtended profile." },
	{ 3766, "RectangleTexture too big (max is the larger of %1x%1 or the size of the backbuffer)." },
	{ 3767, "The argument samples is too big. More than 1800 seconds of audio data is not permitted in a single call of loadPCMFromByteArray." },
	{ 3768, "The Stage3D API may not be used during background execution on this operating system." },
	{ 3769, "Security sandbox violation: Only simple headers can be used with navigateToUrl() or sendToUrl()." },
	{ 3770, "ColorOutputIndex must be in the range [0..3]." },
	{ 3771, "2D textures need to have surfaceSelector = 0." },
	{ 3772, "Cube textures need to have surfaceSelector [0..5]." },
	{ 3773, "Rectangle textures need to have surfaceSelector = 0." },
	{ 3774, "All the assigned render targets should match the outputs in the fragment program." },
	{ 3775, "AGAL validation failed: Non-consecutive slots are not allowed." },
	{ 3776, "Depth output in fragment program requires depthAndStencil = true." },
	{ 3777, "Buffers need to be cleared before first draw." },
	{ 3778, "Video textures have to disable mip mapping and can not have a LOD bias set." },
	{ 3779, "This call requires a Context3D that is created with the standard profile or above" },
	{ 3780, "Requested width of backbuffer is not in allowed range %1 to %2." },
	{ 3781, "Requested height of backbuffer is not in allowed range %1 to %2." },
	{ 3782, "This call requires a Context3D that is created with the baseline profile or above." },
	{ 3783, "A Stage object cannot be added as the child of another object." },
	{ 3784, "The number of instances per element should be greater than 0." },
	{ 3785, "Vertex buffer stream %1 does not contain enough elements for number of instances." },
	{ 3786, "AGAL validation failed: Instance id register can not be written to for %2 at token %3 of %1 program." },
	{ 3787, "This call requires a Context3D that is created with the standard extended profile or above." },
	{ 3788, "Instance id register can only be read in the vertex shader." },
	{ 3789, "AGAL validation failed: Instance id register is supported in Agal version 3 and above." },
	{ 3790, "Texture upload failed." },
	{ 3791, "Asynchronous upload is available for miplevel 0 only." },
	{ 3792, "Vertex buffer stream %1 for instances is improperly set at first index." },
	{ 3800, "This call requires %1 permission." },
	{ 3801, "Another permission request is in progress." },
	{ 3802, "Offset outside stage coordinate bound." },
	{ 3803, "AGAL validation failed: Opcode %2 only allowed in vertex programs at token %3 of %1 program." },
	{ 3804, "AGAL validation failed: Anistropic Filter is not allowed in Vertex Texture Sampler." },
	{ 3805, "AGAL validation failed: Vertex Texture Fetch is supported in Agal version 4 and above." },
	{ 3806, "ByteArray.shareable is no longer supported. Learn more at https://www.adobe.com/go/fp-spectre" },
	{ 3807, "Worker.start has been disabled by the user. Learn more at https://www.adobe.com/go/fp-spectre" },
};

static const char* raw_error_message(uint32_t code)
{
	uint32_t lo = 0;
	uint32_t hi = (uint32_t) (sizeof(ERROR_MESSAGES) / sizeof(ERROR_MESSAGES[0]));
	while (lo < hi)
	{
		uint32_t mid = lo + (hi - lo) / 2;
		uint32_t c = ERROR_MESSAGES[mid].code;
		if (c == code) return ERROR_MESSAGES[mid].msg;
		if (c < code) lo = mid + 1;
		else hi = mid;
	}
	return NULL;
}

// Error.getErrorMessage(id): "Error #N: <template>", or a bare "Error #N"
// when the id has no message (Ruffle globals/error.rs get_error_message —
// the id is coerced to int, so negatives never match). The template keeps
// its %1..%9 placeholders; Error.throwError is what substitutes them.
static Avm2Value error_get_error_message(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	int32_t id = (act->argc > 0) ? avm2_coerce_to_i32(ctx, act->args[0]) : 0;
	const char* msg = (id >= 0) ? raw_error_message((uint32_t) id) : NULL;
	char buf[320];
	int n = (msg != NULL)
		? snprintf(buf, sizeof(buf), "Error #%d: %s", (int) id, msg)
		: snprintf(buf, sizeof(buf), "Error #%d", (int) id);
	if (n < 0) n = 0;
	if (n > (int) sizeof(buf) - 1) n = (int) sizeof(buf) - 1;
	return avm2_string(avm2_string_new(ctx, buf, (uint32_t) n));
}

// Error.throwError(type:Class, index:uint, ...rest) — the entry point FP's AS3
// playerglobal raises every one of its own errors through, and which scripts
// may call directly (avm2/error_throwerror). It takes getErrorMessage(index),
// substitutes %1..%9 from `rest` in a SINGLE pass (so a replacement that is
// itself "%1" stays literal), and throws `new type(message, index)`.
//
// Missing arguments substitute as the empty string — "Error #1044: MethodInfo-
// unsupported flags=." is the real FP text for throwError(Error, 1044).
#define AVM2_THROWERROR_MAX_ARGS 6
static Avm2Value error_throw_error(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	// FP spells a class-side frame with a `$` on the class, which the generic
	// renderer cannot derive from a static vtable entry (same idiom as
	// system_exit). This call never returns, so the frame needs no pop.
	avm2_callstack_rename_frame(ctx, error_throw_error, "Error$/throwError");

	Avm2Class* cls = NULL;
	if (act->argc > 0 && act->args[0].kind == AVM2_VALUE_OBJECT
	    && act->args[0].u.obj != NULL)
	{
		cls = act->args[0].u.obj->class_ref;
	}
	if (cls == NULL) cls = ctx->builtins.error_class;
	double index = (act->argc > 1) ? avm2_coerce_to_number(ctx, act->args[1]) : 0;
	int32_t id = (int32_t) index;
	const char* tmpl = (id >= 0) ? raw_error_message((uint32_t) id) : NULL;

	char buf[1024];
	int n = snprintf(buf, sizeof(buf), "Error #%d", id);
	if (tmpl != NULL)
	{
		n += snprintf(buf + n, sizeof(buf) - (size_t) n, ": ");
		// The substitution runs inside String.replace with a closure
		// replacer, and each argument is coerced to String there — so an
		// argument whose toString() throws (avm2/error_throwerror's
		// TrojanHorse) surfaces with those intermediate frames on the stack.
		static const Avm2MethodRef anon = { NULL, NULL, "Function/<anonymous>", 0 };
		static const Avm2MethodRef repl = { NULL, NULL, "String$/_replace", 0 };
		avm2_callstack_push(ctx, &anon, NULL);
		avm2_callstack_push(ctx, &repl, NULL);
		for (const char* p = tmpl; *p != '\0' && n < (int) sizeof(buf) - 1; )
		{
			if (p[0] == '%' && p[1] >= '1' && p[1] <= '9')
			{
				uint32_t idx = (uint32_t) (p[1] - '1') + 2;
				// FP's throwError declares six OPTIONAL arguments, not a rest
				// parameter: throwError(Error, 3723, "a".."j") substitutes
				// %1..%6 and leaves %7/%8 empty (avm2/error_throwerror).
				if (idx < act->argc && idx < 2 + AVM2_THROWERROR_MAX_ARGS)
				{
					const Avm2String* s =
						avm2_coerce_to_string(ctx, act->args[idx]);
					n += snprintf(buf + n, sizeof(buf) - (size_t) n, "%.*s",
					              (int) s->len, s->utf8);
				}
				p += 2;
				continue;
			}
			buf[n++] = *p++;
			buf[n] = '\0';
		}
		avm2_callstack_pop(ctx);
		avm2_callstack_pop(ctx);
	}
	if (n < 0) n = 0;
	if (n > (int) sizeof(buf) - 1) n = (int) sizeof(buf) - 1;

	Avm2Value args[2];
	args[0] = avm2_string(avm2_string_new(ctx, buf, (uint32_t) n));
	// The id reaches the constructor as a Number, not an int — CustomError's
	// `arg.constructor` trace in avm2/error_throwerror pins "[class Number]".
	args[1] = avm2_number(index);
	avm2_throw(ctx, avm2_class_construct(ctx, cls, args, 2));
}

// Error(...) call = new Error(...) (Ruffle globals/error.rs call handler).
static Avm2Value error_call(Avm2Context* ctx, Avm2Class* cls,
                            const Avm2Value* args, uint32_t argc)
{
	return avm2_class_construct(ctx, cls, args, argc);
}

static void error_add_slot(Avm2Context* ctx, Avm2Class* cls, const char* name,
                           uint32_t slot_id)
{
	Avm2PropEntry e;
	memset(&e, 0, sizeof(e));
	e.key = avm2_public_key(name, (uint32_t) strlen(name));
	e.kind = AVM2_PROP_SLOT;
	e.slot_index = slot_id;
	e.defining_class = cls;
	avm2_vtable_append(ctx, &cls->ivtable, &e);
	if (slot_id > cls->ivtable.slot_count) cls->ivtable.slot_count = slot_id;
}

static Avm2Class* make_error_class(Avm2Context* ctx, const char* name, Avm2Class* super)
{
	Avm2Class* cls = avm2_builtin_class(ctx, "", name, super);
	cls->native_call = error_call;
	// Every error class runs the Error constructor body (subclass SWF
	// constructors reach it via constructsuper).
	cls->instance_init.fn = error_init;
	cls->instance_init.debug_name = name;
	if (super == ctx->builtins.object_class)
	{
		error_add_slot(ctx, cls, "name", ERROR_SLOT_NAME);
		error_add_slot(ctx, cls, "message", ERROR_SLOT_MESSAGE);
		avm2_builtin_add_method(ctx, cls, "getStackTrace", error_get_stack_trace);
		avm2_builtin_add_static_method_n(ctx, cls, "getErrorMessage",
		                                 error_get_error_message, 1);
		avm2_builtin_add_static_method_n(ctx, cls, "throwError",
		                                 error_throw_error, 2);
		avm2_builtin_add_getter(ctx, cls, "errorID", error_get_error_id);

		// Error.prototype is itself an ERROR instance (avmplus
		// initCustomPrototype): name/message land in its slots (invisible
		// to prototype-CHAIN reads), toString stays dynamic (delegated).
		Avm2Object* proto = cls->prototype_obj;
		proto->cls = cls;
		proto->vtable = &cls->ivtable;
		proto->slot_count = cls->ivtable.slot_count + 1;
		proto->slots = avm2_alloc(ctx, proto->slot_count * sizeof(Avm2Value));
		for (uint32_t i = 0; i < proto->slot_count; i++)
		{
			proto->slots[i] = avm2_undefined();
		}
		proto->slots[ERROR_SLOT_NAME] =
			avm2_string(avm2_string_from_literal(ctx, "Error"));
		proto->slots[ERROR_SLOT_MESSAGE] =
			avm2_string(avm2_string_from_literal(ctx, "Error"));
		avm2_proto_add_function(ctx, proto, "toString", error_proto_to_string);
	}
	else
	{
		// Subclass prototypes are plain objects carrying only a dynamic
		// `name`; toString delegates to Error.prototype's (identity matters:
		// TypeError.prototype.toString === Error.prototype.toString).
		avm2_object_set_dynamic(ctx, cls->prototype_obj, "name", 4,
			avm2_string(avm2_string_from_literal(ctx, name)))->dont_enum = 1;
	}
	return cls;
}

void avm2_register_error(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;
	b->error_class = make_error_class(ctx, "Error", b->object_class);
	b->type_error_class = make_error_class(ctx, "TypeError", b->error_class);
	b->reference_error_class = make_error_class(ctx, "ReferenceError", b->error_class);
	b->argument_error_class = make_error_class(ctx, "ArgumentError", b->error_class);
	b->range_error_class = make_error_class(ctx, "RangeError", b->error_class);
	b->verify_error_class = make_error_class(ctx, "VerifyError", b->error_class);
	b->eval_error_class = make_error_class(ctx, "EvalError", b->error_class);
	b->security_error_class = make_error_class(ctx, "SecurityError", b->error_class);
	b->definition_error_class = make_error_class(ctx, "DefinitionError", b->error_class);
	b->uri_error_class = make_error_class(ctx, "URIError", b->error_class);
	b->syntax_error_class = make_error_class(ctx, "SyntaxError", b->error_class);
	b->uninitialized_error_class = make_error_class(ctx, "UninitializedError", b->error_class);

	// flash.errors family (playerglobal AS3 classes extending Error).
	{
		static const char* const names[7] = {
			"IOError", "EOFError", "MemoryError", "IllegalOperationError",
			"InvalidSWFError", "ScriptTimeoutError", "StackOverflowError",
		};
		Avm2Class* made[7];
		for (int i = 0; i < 7; i++)
		{
			// EOFError is `public dynamic class EOFError extends IOError`
			// in both playerglobal and Ruffle -- `eofError is IOError` is
			// TRUE in Flash -- and all_classes/errors/* grades the extra
			// <extendsClass type="flash.errors::IOError"/> line. IOError is
			// made[0], created one iteration earlier.
			Avm2Class* super = (i == 1) ? made[0] : b->error_class;
			Avm2Class* cls = avm2_builtin_class(ctx, "flash.errors", names[i],
			                                    super);
			cls->native_call = error_call;
			cls->instance_init.fn = error_init;
			cls->instance_init.debug_name = names[i];
			avm2_object_set_dynamic(ctx, cls->prototype_obj, "name", 4,
				avm2_string(avm2_string_from_literal(ctx, names[i])))->dont_enum = 1;
			made[i] = cls;
		}
		b->io_error_class = made[0];
		b->eof_error_class = made[1];
		b->memory_error_class = made[2];
		b->illegal_operation_error_class = made[3];
	}
	{
		// DRMManagerError uniquely sets NO prototype.name (Ruffle
		// DRMManagerError.as note; error_prototype asserts undefined). It is
		// also the only flash.errors class with a THIRD constructor parameter
		// and its own toString, both of which error_tostring_more grades.
		// [API("667")] = ApiVersion::FP_10_1 — invisible below SWF 10
		// (all_classes/errors/swf9 expects "DRMManagerError not accessible").
		Avm2Class* cls = avm2_builtin_class_api(ctx, "flash.errors",
		                                        "DRMManagerError",
		                                        b->error_class, 10);
		cls->native_call = error_call;
		cls->instance_init.fn = drm_manager_error_init;
		cls->instance_init.debug_name = "DRMManagerError";
		avm2_builtin_add_getter(ctx, cls, "subErrorID", drm_get_sub_error_id);
		avm2_builtin_add_method(ctx, cls, "toString", drm_error_to_string);
	}
}
