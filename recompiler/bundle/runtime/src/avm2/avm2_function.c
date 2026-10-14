// Function-call plumbing: locals setup (parameter coercion, optional
// defaults, `arguments`/rest, ArgumentError 1063), return-type coercion,
// and the Function builtin class (call/apply/length/prototype).
// Semantics per Ruffle parameters.rs / activation.rs init + globals/function.rs.

#include <stdio.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_gc.h>
#include <avm2/avm2_object.h>
#include <memory/heap.h>
#include <avm2/avm2_ops.h>

// Best-effort port of Ruffle function.rs display_function for error 1063.
static void display_function(char* buf, size_t buf_size, Avm2Activation* act,
                             const Avm2AbcMethodData* md, uint32_t method_index)
{
	if (act->bound_class != NULL)
	{
		// Ruffle prints the class as to_qualified_name_no_mc ("pkg::Class"),
		// then the method TRAIT's local name — not the ABC debug name, which
		// mxmlc emits pre-qualified with single colons
		// ("com.ruffle:MyClass/myMethod"). Same normalization as
		// avm2_callstack_frame_name: keep only the last segment.
		char cq[128];
		avm2_class_qname_colons_buf(act->bound_class, cq, sizeof(cq));
		if (act->bound_class->instance_init.file == act->file
		    && act->bound_class->instance_init.method_index == method_index
		    && act->bound_class->instance_init.fn == md->fn)
		{
			snprintf(buf, buf_size, "%s()", cq);
			return;
		}
		if (md->debug_name != NULL && md->debug_name[0] != '\0')
		{
			const char* dn = md->debug_name;
			const char* slash = strrchr(dn, '/');
			if (slash != NULL) dn = slash + 1;
			const char* colon = strrchr(dn, ':');
			if (colon != NULL) dn = colon + 1;
			snprintf(buf, buf_size, "%s/%s()", cq, dn);
			return;
		}
		snprintf(buf, buf_size, "%s/MethodInfo-%u()", cq, method_index);
		return;
	}
	if (md->is_function)
	{
		snprintf(buf, buf_size, "Function/%s()",
		         (md->debug_name != NULL && md->debug_name[0] != '\0'
		          && strchr(md->debug_name, '/') == NULL)
		         ? md->debug_name : "<anonymous>");
		return;
	}
	if (md->debug_name != NULL && md->debug_name[0] != '\0')
	{
		if (strchr(md->debug_name, '/') != NULL)
		{
			snprintf(buf, buf_size, "%s()", md->debug_name);
			return;
		}
		snprintf(buf, buf_size, "Function/%s()", md->debug_name);
		return;
	}
	snprintf(buf, buf_size, "MethodInfo-%u()", method_index);
}

_Noreturn static void throw_1063(Avm2Activation* act, const Avm2AbcMethodData* md,
                                 uint32_t method_index)
{
	uint32_t required = 0;
	for (uint32_t i = 0; i < md->param_count; i++)
	{
		if (md->optionals == NULL || !md->optionals[i].has_value) required++;
	}
	char fname[160];
	display_function(fname, sizeof(fname), act, md, method_index);
	// The arg-count check conceptually happens in the CALLER (the callee
	// frame is excluded from the error's stack snapshot).
	avm2_callstack_pop(act->ctx);
	avm2_throw_error(act->ctx, act->ctx->builtins.argument_error_class,
	                 "Error #1063: Argument count mismatch on %s. Expected %u, "
	                 "got %u.", fname, required, act->argc);
}

void avm2_setup_locals(Avm2Value* loc, uint32_t num_locals, Avm2Activation* act,
                       uint32_t method_index)
{
	Avm2Context* ctx = act->ctx;
	if (num_locals == 0) return;
	// `avm2_undefined()` is `{ AVM2_VALUE_UNDEFINED, 0, { 0 } }` — all 16
	// bytes zero — so the per-local undefined fill is BYTE-EXACTLY a memset.
	// It is not a small loop: the RWK plan_k TAS runs 50.7M setup_locals
	// calls / 1560 ticks at ~3.9 locals each = 197M scalar 16-byte stores.
#ifdef AVM2_NO_LOCALS_MEMSET
	for (uint32_t i = 0; i < num_locals; i++)
	{
		loc[i] = avm2_undefined();
	}
#else
	memset(loc, 0, (size_t) num_locals * sizeof(Avm2Value));
#endif
	loc[0] = act->this_val;

	const Avm2AbcMethodData* md = &act->file->data->methods[method_index];
	uint32_t pc = md->param_count;

	// "Unchecked" (Ruffle method.rs): function closures whose params are
	// all untyped with no defaults skip both arg-count checks.
	int unchecked = md->is_function;
	for (uint32_t i = 0; i < pc && unchecked; i++)
	{
		if ((md->param_types != NULL && md->param_types[i] != 0)
		    || (md->optionals != NULL && md->optionals[i].has_value))
		{
			unchecked = 0;
		}
	}
	int variadic = (md->flags & (AVM2_METHOD_NEED_REST | AVM2_METHOD_NEED_ARGUMENTS
	                             | AVM2_METHOD_IGNORE_REST)) != 0;
	if (act->argc > pc && !variadic && !unchecked)
	{
		throw_1063(act, md, method_index);
	}

	for (uint32_t i = 0; i < pc && i + 1 < num_locals; i++)
	{
		Avm2Value v;
		if (i < act->argc)
		{
			v = act->args[i];
		}
		else if (md->optionals != NULL && md->optionals[i].has_value)
		{
			v = avm2_default_value(ctx, act->file, &md->optionals[i]);
		}
		else if (unchecked)
		{
			v = avm2_undefined();
		}
		else
		{
			throw_1063(act, md, method_index);
		}
		if (md->param_types != NULL && md->param_types[i] != 0)
		{
			// Ruffle coerces the whole signature in init_from_method, which
			// runs BEFORE the callee's call-stack frame is pushed — so a
			// #1034 here is attributed to the CALLER, exactly like the #1063
			// above (loader_method's uncaught coercion error prints only
			// `at Test/onFrame()`, never `at Test/load()`). The frame is put
			// back if the coercion succeeds; if it throws, the longjmp
			// unwinds past it anyway.
			int popped = ctx->call_depth > 0;
			Avm2CallFrame saved;
			if (popped)
			{
				saved = ctx->call_frames[ctx->call_depth - 1];
				avm2_callstack_pop(ctx);
			}
			v = avm2_coerce_to_type_mn(ctx, act->file, md->param_types[i], v);
			if (popped)
			{
				avm2_callstack_push(ctx, &saved.method, saved.bound_class);
			}
		}
		loc[i + 1] = v;
	}

	if ((md->flags & AVM2_METHOD_NEED_REST) && pc + 1 < num_locals)
	{
		uint32_t extra = act->argc > pc ? act->argc - pc : 0;
		loc[pc + 1] = avm2_object_value(
			avm2_array_from_values(ctx, extra ? &act->args[pc] : NULL, extra));
	}
	else if ((md->flags & AVM2_METHOD_NEED_ARGUMENTS) && pc + 1 < num_locals)
	{
		if (act->callee == NULL)
		{
			// Method-trait invocation without a closure: synthesize one so
			// arguments.callee is callable, cached per (receiver, method)
			// so repeated calls return the identical closure.
			Avm2Object* holder = (act->this_val.kind == AVM2_VALUE_OBJECT)
				? act->this_val.u.obj : NULL;
			if (holder != NULL)
			{
				for (Avm2BoundMethod* bm = holder->bound_methods; bm != NULL;
				     bm = bm->next)
				{
					if (bm->entry == (const void*) md)
					{
						act->callee = bm->fn;
						break;
					}
				}
			}
			if (act->callee == NULL)
			{
				Avm2MethodRef ref = { md->fn, act->file, md->debug_name, method_index };
				act->callee = avm2_function_new(ctx, &ref, act->bound_class,
				                                act->outer, act->this_val, true);
				if (holder != NULL)
				{
					Avm2BoundMethod* bm = avm2_alloc(ctx, sizeof(Avm2BoundMethod));
					bm->entry = (const void*) md;
					bm->fn = act->callee;
					bm->next = holder->bound_methods;
					holder->bound_methods = bm;
				}
			}
		}
		Avm2Object* args_arr = avm2_array_from_values(ctx, act->args, act->argc);
		Avm2DynProp* p = avm2_object_set_dynamic(ctx, args_arr, "callee", 6,
		                                         avm2_object_value(act->callee));
		p->dont_enum = 1;
		loc[pc + 1] = avm2_object_value(args_arr);
	}
}

Avm2Value avm2_op_coerce_return(Avm2Activation* act, uint32_t method_index, Avm2Value v)
{
	const Avm2AbcMethodData* md = &act->file->data->methods[method_index];
	return avm2_coerce_to_type_mn(act->ctx, act->file, md->return_type_mn, v);
}

// ---------------------------------------------------------------------------
// Function builtins
// ---------------------------------------------------------------------------

static Avm2Object* require_function_this(Avm2Activation* act)
{
	if (act->this_val.kind != AVM2_VALUE_OBJECT
	    || act->this_val.u.obj->kind != AVM2_OBJ_FUNCTION)
	{
		avm2_throw_error(act->ctx, act->ctx->builtins.type_error_class,
		                 "Error #1006: value is not a function.");
	}
	return act->this_val.u.obj;
}

// Function.prototype.toString / .toLocaleString: every function stringifies as
// the same generic source text (avm2/function_to_string). This has to live on
// Function.prototype rather than being folded into Object.prototype.toString,
// because that one reports functions as "[object Function-N]" — the ES3
// classification the ecma3/FunctionObjects tests probe by reassigning
// `fn.toString = Object.prototype.toString`.
static Avm2Value fn_proto_to_string(Avm2Activation* act)
{
	require_function_this(act);
	return avm2_string(avm2_string_from_literal(act->ctx, "function Function() {}"));
}

static Avm2Value fn_call(Avm2Activation* act)
{
	Avm2Object* fn = require_function_this(act);
	Avm2Value recv = act->argc > 0 ? act->args[0] : avm2_null();
	const Avm2Value* rest = act->argc > 1 ? &act->args[1] : NULL;
	uint32_t restc = act->argc > 1 ? act->argc - 1 : 0;
	return avm2_call_function_obj(act->ctx, fn, recv, rest, restc);
}

static Avm2Value fn_apply(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* fn = require_function_this(act);
	Avm2Value recv = act->argc > 0 ? act->args[0] : avm2_null();
	if (act->argc < 2 || act->args[1].kind == AVM2_VALUE_NULL
	    || act->args[1].kind == AVM2_VALUE_UNDEFINED)
	{
		return avm2_call_function_obj(ctx, fn, recv, NULL, 0);
	}
	Avm2ArrayExt* ext = (act->args[1].kind == AVM2_VALUE_OBJECT)
		? avm2_array_ext(act->args[1].u.obj) : NULL;
	if (ext == NULL)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1116: second argument to Function.prototype.apply "
		                 "must be an array.");
	}
	// Holes resolve through the array's prototype chain (Array.prototype
	// indices are visible to apply).
	Avm2Value* args = NULL;
	Avm2Object* arr_obj = act->args[1].u.obj;
	uint32_t n_args = ext->dense_len;
	if (n_args > 0)
	{
		args = avm2_alloc(ctx, n_args * sizeof(Avm2Value));
		for (uint32_t i = 0; i < n_args; i++)
		{
			Avm2Value v = ext->elems[i];
			if (v.kind == AVM2_VALUE_HOLE)
			{
				Avm2Value pv;
				v = avm2_array_proto_index(arr_obj, i, &pv) ? pv : avm2_undefined();
			}
			args[i] = v;
		}
	}
	Avm2Value r = avm2_call_function_obj(ctx, fn, recv, args, n_args);
	// The argument vector is scratch (callees copy what they keep: arguments,
	// ...rest); it used to leak per apply() call.
	if (args != NULL) heap_free(ctx->app, args);
	return r;
}

static Avm2Value fn_get_length(Avm2Activation* act)
{
	Avm2Object* fn = require_function_this(act);
	if (fn->fn_method.file != NULL)
	{
		const Avm2AbcMethodData* md =
			&fn->fn_method.file->data->methods[fn->fn_method.method_index];
		return avm2_integer((int32_t) md->param_count);
	}
	// Native builtin: no ABC method to read, so the arity is carried on the
	// method ref itself (declared at registration).
	return avm2_integer((int32_t) fn->fn_method.param_count);
}

static Avm2Value fn_get_prototype(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* fn = require_function_this(act);
	if (fn->fn_proto_nulled)
	{
		return avm2_undefined();
	}
	if (fn->fn_prototype == NULL)
	{
		// Lazily create: a plain dynamic object with constructor = fn.
		Avm2Object* proto = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 0);
		proto->cls = ctx->builtins.object_class;
		proto->proto = ctx->builtins.object_class->prototype_obj;
		Avm2DynProp* p = avm2_object_set_dynamic(ctx, proto, "constructor", 11,
		                                         avm2_object_value(fn));
		p->dont_enum = 1;
		fn->fn_prototype = proto;
	}
	return avm2_object_value(fn->fn_prototype);
}

static Avm2Value fn_set_prototype(Avm2Activation* act)
{
	Avm2Object* fn = require_function_this(act);
	if (act->argc > 0 && act->args[0].kind == AVM2_VALUE_OBJECT)
	{
		fn->fn_prototype = act->args[0].u.obj;
		fn->fn_proto_nulled = 0;
	}
	else
	{
		fn->fn_prototype = NULL;
		fn->fn_proto_nulled = 1;
	}
	return avm2_undefined();
}

// Function() / new Function(): a fresh no-op function (Ruffle matches).
//
// With ANY argument it is the ES3 `Function('function body')` form, which
// avmplus does not implement — there is no eval, so it throws EvalError
// #1066 rather than compiling the body (ecma3/FunctionObjects e15_3_1_1_2_rt
// and friends). Both the call and construct forms land here.
static Avm2Value function_construct(Avm2Context* ctx, Avm2Class* cls,
                                    const Avm2Value* args, uint32_t argc)
{
	(void) cls; (void) args;
	if (argc > 0)
	{
		avm2_throw_error(ctx, ctx->builtins.eval_error_class,
		                 "Error #1066: The form function('function body') "
		                 "is not supported.");
	}
	Avm2MethodRef ref = { NULL, NULL, "Function", 0 };
	return avm2_object_value(avm2_function_new(ctx, &ref, NULL, NULL,
	                                           avm2_undefined(), false));
}

void avm2_register_function_builtins(Avm2Context* ctx)
{
	Avm2Class* cls = ctx->builtins.function_class;

	// Function.prototype is itself a callable no-op function in avmplus
	// (Function.prototype is Function → true).
	{
		Avm2MethodRef ref = { NULL, NULL, "Function.prototype", 0 };
		Avm2Object* fproto = avm2_function_new(ctx, &ref, NULL, NULL,
		                                       avm2_undefined(), false);
		fproto->proto = ctx->builtins.object_class->prototype_obj;
		if (cls->prototype_obj != NULL)
		{
			// Preserve the constructor link created by builtin_class.
			fproto->dyn_props = cls->prototype_obj->dyn_props;
			fproto->dyn_tail = cls->prototype_obj->dyn_tail;
		}
		cls->prototype_obj = fproto;
		avm2_gc_pin(fproto);  // GC: class prototypes are immortal (structural)
	}
	// The declared arities matter: Function.prototype is a real Function
	// instance, so `Function.prototype.apply.length` resolves `apply`
	// through the ivtable trait (which shadows the ES3 prototype function),
	// and it is THIS method ref's param_count that fn_get_length reports.
	// ecma3/Function/e15_3_4_3_1 pins 2 and e15_3_4_4_1_rt pins 1.
	avm2_builtin_add_method_n(ctx, cls, "call", fn_call, 1);
	avm2_builtin_add_method_n(ctx, cls, "apply", fn_apply, 2);
	// Both traits live in the AS3 BUILTIN namespace in playerglobal, and FP
	// spells a non-public method's frame with its namespace URI
	// ("Function/http://adobe.com/AS3/2006/builtin::call()" —
	// avm2/primitive_toString, avm2/error_throwerror). Our keys stay public
	// (dispatch folds AS3<->public); only the frame name is corrected here.
	avm2_builtin_set_debug_name(ctx, cls, "call",
	                            "http://adobe.com/AS3/2006/builtin::call");
	avm2_builtin_set_debug_name(ctx, cls, "apply",
	                            "http://adobe.com/AS3/2006/builtin::apply");
	avm2_builtin_add_getter(ctx, cls, "length", fn_get_length);
	// ES3-compat layer on Function.prototype (Ruffle globals/Function.as).
	avm2_proto_add_function_n(ctx, cls->prototype_obj, "call", fn_call, 1);
	avm2_proto_add_function_n(ctx, cls->prototype_obj, "apply", fn_apply, 2);
	avm2_proto_add_function(ctx, cls->prototype_obj, "toString", fn_proto_to_string);
	avm2_proto_add_function(ctx, cls->prototype_obj, "toLocaleString",
	                        fn_proto_to_string);
	// NOT valueOf: e15_3_4__1_rt asserts Function.prototype.valueOf really is
	// Object.prototype.valueOf (identity, not just equivalent behavior).
	// prototype is a getter/setter pair.
	{
		Avm2PropEntry e;
		memset(&e, 0, sizeof(e));
		e.key = avm2_public_key("prototype", 9);
		e.kind = AVM2_PROP_GETSET;
		e.method.fn = fn_get_prototype;
		e.method.debug_name = "get prototype";
		e.setter.fn = fn_set_prototype;
		e.setter.debug_name = "set prototype";
		avm2_vtable_append(ctx, &cls->ivtable, &e);
	}
	cls->native_construct = function_construct;
	cls->native_call = function_construct;
}
