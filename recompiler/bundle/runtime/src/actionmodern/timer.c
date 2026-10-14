// timer.c — setInterval / setTimeout / clearInterval / clearTimeout
// implementation + per-frame tick (processTimers).
//
// Carved out of action.c per
// SWFRecompDocs/plans/split-action-c-math-first.md (fourth subsystem,
// after Math / Date / registerClass). Needed in both graphics and
// non-graphics modes.

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <heap.h>

#include <actionmodern/action.h>            // processTimers/hasActiveTimers ABI, actionFlushPendingOnLoads, g_execution_halted
#include <actionmodern/action_internal.h>
#include <actionmodern/actiontimer.h>

// ==================================================================
// Timer state
// ==================================================================

#define MAX_TIMERS 64

typedef struct {
	int active;                // 0 = empty slot, 1 = active
	int id;                    // sequential ID (1, 2, 3, ...)
	int is_interval;           // 1 = repeating (setInterval), 0 = one-shot (setTimeout)
	double delay_ms;           // delay in milliseconds
	double elapsed_ms;         // accumulated time since last fire (or since creation)

	// Callback — either function-form or method-form
	int is_method;             // 0 = function, 1 = method on object
	ActionVar func;            // function reference (function-form)
	ActionVar object;          // object reference (method-form)
	char method_name[256];     // method name string (method-form)

	// Extra arguments passed to callback
	ActionVar extra_args[8];   // inline array of extra args (max 8)
	int extra_arg_count;
} TimerEntry;

static TimerEntry g_timers[MAX_TIMERS];
static int g_next_timer_id = 1;

// ASFunction globals for the 4 top-level timer builtins. Populated by
// initTimerFunctions() on first accessor use.
static ASFunction g_setInterval_func;
static ASFunction g_clearInterval_func;
static ASFunction g_setTimeout_func;
static ASFunction g_clearTimeout_func;
static int g_timer_funcs_init = 0;

// ==================================================================
// Helpers
// ==================================================================

// Coerce an ActionVar to double using the stack (handles valueOf on objects)
static double timerCoerceToNumber(SWFAppContext* app_context, ActionVar* v)
{
	if (v->type == ACTION_STACK_VALUE_UNDEFINED) return NAN;
	if (v->type == ACTION_STACK_VALUE_NULL) return 0.0;
	if (v->type == ACTION_STACK_VALUE_F32) return (double) VAL(float, &v->data.numeric_value);
	if (v->type == ACTION_STACK_VALUE_F64) return VAL(double, &v->data.numeric_value);
	if (v->type == ACTION_STACK_VALUE_BOOLEAN) return v->data.numeric_value ? 1.0 : 0.0;
	// For objects (with valueOf), strings, etc. — use the stack-based convertFloat
	pushVar(app_context, v);
	convertFloat(app_context);
	ActionVar result;
	popVar(app_context, &result);
	if (result.type == ACTION_STACK_VALUE_F64) return VAL(double, &result.data.numeric_value);
	if (result.type == ACTION_STACK_VALUE_F32) return (double) VAL(float, &result.data.numeric_value);
	return NAN;
}

static int timerIsString(ActionVar* v)
{
	return v->type == ACTION_STACK_VALUE_STRING;
}

static int timerIsCallable(ActionVar* v)
{
	return v->type == ACTION_STACK_VALUE_FUNCTION;
}

static int timerIsObject(ActionVar* v)
{
	return v->type == ACTION_STACK_VALUE_OBJECT ||
	       v->type == ACTION_STACK_VALUE_MOVIECLIP;
}

// ==================================================================
// setInterval / setTimeout registration
// ==================================================================

// Called from actionCallFunction dispatch. args[] are the function arguments.
// is_interval: 1 = setInterval (repeating), 0 = setTimeout (one-shot)
void actionTimerSetInterval(SWFAppContext* app_context, ActionVar* args, u32 num_args, int is_interval)
{
	// Validation: need at least 2 args
	if (num_args < 2)
	{
		if (args != NULL) FREE(args);
		pushUndefined(app_context);
		return;
	}

	// Detect calling convention:
	// Function-form: setInterval(func, delay, ...extraArgs)
	//   - args[0] is a function
	// Method-form: setInterval(obj, methodName, delay, ...extraArgs)
	//   - args[0] is an object/movieclip, args[1] is a string (or coercible to string)
	//   - Flash uses method-form when: num_args >= 3, args[0] is obj/mc, and args[0] is NOT callable
	int is_method = 0;

	if (num_args >= 3 && timerIsObject(&args[0]) && !timerIsCallable(&args[0]))
	{
		is_method = 1;
	}

	if (is_method)
	{
		// Method-form: setInterval(obj, methodName, delay, ...extraArgs)
		// Need at least 3 args
		if (num_args < 3)
		{
			if (args != NULL) FREE(args);
			pushUndefined(app_context);
			return;
		}

		// Get method name — coerce args[1] to string via the stack
		char method_buf[256];
		method_buf[0] = '\0';
		if (timerIsString(&args[1]))
		{
			const uint16_t* u16_str = varGetU16Ptr(&args[1]);
			if (u16_str != NULL && args[1].str_size > 0)
			{
				int len = u16_to_utf8(u16_str, args[1].str_size, method_buf, sizeof(method_buf) - 1);
				method_buf[len] = '\0';
			}
		}
		else if (args[1].type != ACTION_STACK_VALUE_NULL && args[1].type != ACTION_STACK_VALUE_UNDEFINED)
		{
			// Coerce to string (handles objects with toString)
			pushVar(app_context, &args[1]);
			char _ts_buf[17];
			convertString(app_context, _ts_buf);
			const uint16_t* u16_str = (const uint16_t*)STACK_TOP_VALUE;
			u32 u16_len = STACK_TOP_N;
			if (u16_str != NULL && u16_len > 0)
			{
				int len = u16_to_utf8(u16_str, u16_len, method_buf, sizeof(method_buf) - 1);
				method_buf[len] = '\0';
			}
			POP();
		}
		else
		{
			// null or undefined method name — store "null" / "undefined"
			if (args[1].type == ACTION_STACK_VALUE_NULL)
				strcpy(method_buf, "null");
			else
				strcpy(method_buf, "undefined");
		}

		// Coerce delay (args[2]):
		// undefined delay → reject (return undefined), matching Flash behavior
		// Non-numeric string/object delays → coerce to 0 (Shumway/Ruffle behavior)
		if (args[2].type == ACTION_STACK_VALUE_UNDEFINED) {
			if (args != NULL) FREE(args);
			pushUndefined(app_context);
			return;
		}
		double delay = timerCoerceToNumber(app_context, &args[2]);
		if (isnan(delay)) delay = 0.0;

		// Find empty slot
		int slot = -1;
		for (int i = 0; i < MAX_TIMERS; i++)
		{
			if (!g_timers[i].active) { slot = i; break; }
		}
		if (slot == -1)
		{
			if (args != NULL) FREE(args);
			pushUndefined(app_context);
			return;
		}

		// Register timer
		TimerEntry* t = &g_timers[slot];
		memset(t, 0, sizeof(TimerEntry));
		t->active = 1;
		t->id = g_next_timer_id++;
		t->is_interval = is_interval;
		t->delay_ms = delay;
		t->elapsed_ms = 0.0;
		t->is_method = 1;
		t->object = args[0]; // shallow copy (object pointer)
		strncpy(t->method_name, method_buf, sizeof(t->method_name) - 1);
		t->method_name[sizeof(t->method_name) - 1] = '\0';

		// Copy extra args (args[3], args[4], ...)
		t->extra_arg_count = 0;
		for (u32 i = 3; i < num_args && t->extra_arg_count < 8; i++)
		{
			t->extra_args[t->extra_arg_count++] = args[i];
		}

		// Push timer ID
		double id_val = (double)t->id;
		PUSH(ACTION_STACK_VALUE_F64, VAL(u64, &id_val));
		if (args != NULL) FREE(args);
	}
	else
	{
		// Function-form: setInterval(func, delay, ...extraArgs)
		// Validate: args[0] must be a callable function
		if (!timerIsCallable(&args[0]))
		{
			if (args != NULL) FREE(args);
			pushUndefined(app_context);
			return;
		}

		// Coerce delay (args[1]):
		// undefined delay → reject (return undefined), matching Flash behavior
		// Non-numeric string/object delays → coerce to 0 (Shumway/Ruffle behavior)
		if (args[1].type == ACTION_STACK_VALUE_UNDEFINED) {
			if (args != NULL) FREE(args);
			pushUndefined(app_context);
			return;
		}
		double delay = timerCoerceToNumber(app_context, &args[1]);
		if (isnan(delay)) delay = 0.0;

		// Find empty slot
		int slot = -1;
		for (int i = 0; i < MAX_TIMERS; i++)
		{
			if (!g_timers[i].active) { slot = i; break; }
		}
		if (slot == -1)
		{
			if (args != NULL) FREE(args);
			pushUndefined(app_context);
			return;
		}

		// Register timer
		TimerEntry* t = &g_timers[slot];
		memset(t, 0, sizeof(TimerEntry));
		t->active = 1;
		t->id = g_next_timer_id++;
		t->is_interval = is_interval;
		t->delay_ms = delay;
		t->elapsed_ms = 0.0;
		t->is_method = 0;
		t->func = args[0]; // shallow copy (function pointer)

		// Copy extra args (args[2], args[3], ...)
		t->extra_arg_count = 0;
		for (u32 i = 2; i < num_args && t->extra_arg_count < 8; i++)
		{
			t->extra_args[t->extra_arg_count++] = args[i];
		}

		// Push timer ID
		double id_val = (double)t->id;
		PUSH(ACTION_STACK_VALUE_F64, VAL(u64, &id_val));
		if (args != NULL) FREE(args);
	}
}

// clearInterval / clearTimeout
void actionTimerClearInterval(SWFAppContext* app_context, ActionVar* args, u32 num_args)
{
	if (num_args == 0)
	{
		pushUndefined(app_context);
		return;
	}

	// Get timer ID from first arg
	double id_d = timerCoerceToNumber(app_context, &args[0]);
	int id = (int)id_d;

	// Find and deactivate the timer
	for (int i = 0; i < MAX_TIMERS; i++)
	{
		if (g_timers[i].active && g_timers[i].id == id)
		{
			g_timers[i].active = 0;
			break;
		}
	}

	pushUndefined(app_context);
}

// ==================================================================
// Callback dispatch
// ==================================================================

// Fire a timer callback (function-form or method-form)
static void fireTimerCallback(SWFAppContext* app_context, TimerEntry* t)
{
	if (g_execution_halted) return;

	if (t->is_method)
	{
		// Method-form: look up method on object at fire time, call with this=obj
		ASObject* obj = NULL;
		MovieClip* mc = NULL;

		if (t->object.type == ACTION_STACK_VALUE_OBJECT)
			obj = (ASObject*)(uintptr_t)t->object.data.numeric_value;
		else if (t->object.type == ACTION_STACK_VALUE_MOVIECLIP)
			mc = (MovieClip*)(uintptr_t)t->object.data.numeric_value;

		// Look up the method
		ASFunction* method_func = NULL;
		if (obj != NULL)
		{
			ActionVar* mv = getPropertyWithPrototype(obj, t->method_name, strlen(t->method_name));
			if (mv != NULL && mv->type == ACTION_STACK_VALUE_FUNCTION)
				method_func = (ASFunction*)(uintptr_t)mv->data.numeric_value;
		}
		else if (mc != NULL)
		{
			// Look up on MC's dynamic_props
			ASObject* dprops = (ASObject*)mc->dynamic_props;
			if (dprops != NULL)
			{
				ActionVar* mv = getPropertyWithPrototype(dprops, t->method_name, strlen(t->method_name));
				if (mv != NULL && mv->type == ACTION_STACK_VALUE_FUNCTION)
					method_func = (ASFunction*)(uintptr_t)mv->data.numeric_value;
			}
		}

		if (method_func == NULL) return; // method not found, skip silently

		// INV_VERSION_SWITCH: run the callback under its defining movie's SWF
		// version (and the matching version-group _global) — timers fire from
		// the host movie's frame loop, so a callback defined in a loaded child
		// SWF of a different version would otherwise get the host's semantics.
		// Safe inside the core for this form: neither arm ever switches base
		// clip, so there is no gate to compute against the switched version.
		//
		// this_var = &t->object: the core derives the same obj/mc ABI receiver
		// the arm used to compute by hand. INV_EVENT_THIS_MC on type-1 only —
		// the core's "set when the receiver is an MC" is exactly the old
		// `if (mc != NULL)`, and the type-2 arm never set it. No local scope
		// frame, no this-stack, no exec-func swap in either arm (preserved by
		// leaving those flags clear); depth bracket stays out here because the
		// path never had the g_max_call_depth halt check. The core additionally
		// clamps/pads type-1 args to param_count (the TYPE1_ARG_ORDER fix,
		// regression/timer_type1_args), bounds + copies scope_mc on the
		// captured-scope push (was stale), and NULL-checks the function
		// pointers (MovieClip.prototype stubs are type-1 with NULL pointers).
		u32 _tm_flags = (method_func->function_type == 2)
			? (INV_CAPTURED_SCOPE | INV_FORCE_CAPTURED_WITH | INV_VERSION_SWITCH)
			: (INV_EVENT_THIS_MC | INV_VERSION_SWITCH);
		InvokeOpts _tm_opts = { .flags = _tm_flags };
		g_call_depth++;
		actionInvokeFunctionValue(app_context, method_func, &t->object,
			t->extra_arg_count > 0 ? t->extra_args : NULL,
			t->extra_arg_count, &_tm_opts);
		g_call_depth--;
		// Return value discarded.
	}
	else
	{
		// Function-form: call the function directly
		ASFunction* func = (ASFunction*)(uintptr_t)t->func.data.numeric_value;
		if (func == NULL) return;

		// Base clip stays in the arm (never pair INV_BASE_CLIP with
		// INV_VERSION_SWITCH). CALLER-gated since normalization pass (b)
		// item 3 (was the callee-version accident of statement order):
		// Ruffle's Avm1Function::call decides is_closure from the CALLER's
		// activation version; a v5 ambient at fire time is pre-closure and
		// keeps the caller's context (the core's INV_VERSION_SWITCH v5 branch
		// handles the version side the same way).
		MovieClip* old_context = g_current_context;
		if (g_swf_version >= 6 && func->base_clip != NULL)
			g_current_context = actionClosureBaseClip(app_context, func->base_clip, NULL);

		// this_var = NULL: the core passes NULL as the ABI receiver, as both
		// arms always did. Type-1 keeps no captured scopes (a type-1 closure
		// loses its defining chain here — a normalization candidate, not a
		// migration change). Same silent fixes as the method-form above
		// (clamp/pad, bounded scope push, scope_mc, NULL-checked dispatch).
		u32 _tf_flags = (func->function_type == 2)
			? (INV_CAPTURED_SCOPE | INV_FORCE_CAPTURED_WITH | INV_VERSION_SWITCH)
			: INV_VERSION_SWITCH;
		InvokeOpts _tf_opts = { .flags = _tf_flags };
		g_call_depth++;
		actionInvokeFunctionValue(app_context, func, NULL,
			t->extra_arg_count > 0 ? t->extra_args : NULL,
			t->extra_arg_count, &_tf_opts);
		g_call_depth--;

		g_current_context = old_context;
		// Return value discarded.
	}
}

// ==================================================================
// Frame-loop hooks (declared in <actionmodern/action.h>)
// ==================================================================

// Process all active timers. Called from swf_core.c after each frame tick.
// frame_duration_ms = 1000.0 / fps
void processTimers(SWFAppContext* app_context, double frame_duration_ms)
{
	// Advance all timers by frame_duration_ms
	for (int i = 0; i < MAX_TIMERS; i++)
	{
		if (g_timers[i].active)
			g_timers[i].elapsed_ms += frame_duration_ms;
	}

	// Fire eligible timers. May fire multiple times per frame if delay is short.
	// Fire lowest ID first when multiple timers are ready at the same time.
	int fired_any;
	int iteration_limit = 10000; // safety limit to prevent infinite loops
	do {
		fired_any = 0;
		if (--iteration_limit <= 0) break;

		for (int i = 0; i < MAX_TIMERS; i++)
		{
			if (!g_timers[i].active) continue;

			// Deactivate method-form timers whose target MC has been removed
			if (g_timers[i].is_method && g_timers[i].object.type == ACTION_STACK_VALUE_MOVIECLIP) {
				MovieClip* tmc = (MovieClip*)(uintptr_t)g_timers[i].object.data.numeric_value;
				if (tmc == NULL || tmc->depth == INT_MIN) {
					g_timers[i].active = 0;
					continue;
				}
			}

			if (g_timers[i].elapsed_ms < g_timers[i].delay_ms) continue;

			// Fire this timer's callback
			fireTimerCallback(app_context, &g_timers[i]);
			fired_any = 1;

			// Fire deferred onChanged from replaceSel (if any)
			actionFlushPendingOnChanged(app_context);

			// Flush pending onLoads queued by attachMovie during this timer callback
			actionFlushPendingOnLoads(app_context);

			if (g_timers[i].is_interval)
			{
				// Repeating: subtract one delay period
				g_timers[i].elapsed_ms -= g_timers[i].delay_ms;
				// If delay is 0, prevent infinite loop
				if (g_timers[i].delay_ms <= 0) g_timers[i].active = 0;
			}
			else
			{
				// One-shot: deactivate
				g_timers[i].active = 0;
			}
			break; // Re-scan from beginning (firing may have cleared other timers)
		}
	} while (fired_any);
}

// Check if any timers are active
int hasActiveTimers(void)
{
	for (int i = 0; i < MAX_TIMERS; i++)
	{
		if (g_timers[i].active) return 1;
	}
	return 0;
}

// ==================================================================
// Top-level builtin wrappers + ASFunction globals
// ==================================================================

// Builtin setTimeout/setInterval that work when aliased (var f = setTimeout; f(...))
// Must copy args since actionTimerSetInterval frees them, but caller also frees the originals.
static ActionVar builtin_setTimeout_impl(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	ActionVar* args_copy = NULL;
	if (arg_count > 0) {
		args_copy = (ActionVar*)HALLOC(sizeof(ActionVar) * arg_count);
		for (u32 i = 0; i < arg_count; i++) args_copy[i] = args[i];
	}
	actionTimerSetInterval(app_context, args_copy, arg_count, 0);
	ActionVar r; popVar(app_context, &r); return r;
}

static ActionVar builtin_setInterval_impl(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	ActionVar* args_copy = NULL;
	if (arg_count > 0) {
		args_copy = (ActionVar*)HALLOC(sizeof(ActionVar) * arg_count);
		for (u32 i = 0; i < arg_count; i++) args_copy[i] = args[i];
	}
	actionTimerSetInterval(app_context, args_copy, arg_count, 1);
	ActionVar r; popVar(app_context, &r); return r;
}

static ActionVar builtin_clearInterval_impl(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj)
{
	(void)registers; (void)this_obj;
	actionTimerClearInterval(app_context, args, arg_count);
	ActionVar r = {0}; r.type = ACTION_STACK_VALUE_UNDEFINED; return r;
}

void initTimerFunctions(void)
{
	if (g_timer_funcs_init) return;
	struct { ASFunction* func; const char* name; Function2Ptr impl; } entries[] = {
		{&g_setInterval_func,   "setInterval",   (Function2Ptr)builtin_setInterval_impl},
		{&g_clearInterval_func, "clearInterval", (Function2Ptr)builtin_clearInterval_impl},
		{&g_setTimeout_func,    "setTimeout",    (Function2Ptr)builtin_setTimeout_impl},
		// clearTimeout shares the clearInterval impl — same semantics
		{&g_clearTimeout_func,  "clearTimeout",  (Function2Ptr)builtin_clearInterval_impl},
	};
	for (int i = 0; i < (int)(sizeof(entries)/sizeof(entries[0])); i++) {
		memset(entries[i].func, 0, sizeof(ASFunction));
		strncpy(entries[i].func->name, entries[i].name, 255);
		entries[i].func->function_type = 2;
		entries[i].func->advanced_func = entries[i].impl;
	}
	g_timer_funcs_init = 1;
}

ASFunction* actionTimerGetSetInterval(void)   { initTimerFunctions(); return &g_setInterval_func; }
ASFunction* actionTimerGetClearInterval(void) { initTimerFunctions(); return &g_clearInterval_func; }
ASFunction* actionTimerGetSetTimeout(void)    { initTimerFunctions(); return &g_setTimeout_func; }
ASFunction* actionTimerGetClearTimeout(void)  { initTimerFunctions(); return &g_clearTimeout_func; }

// Stage 3 collector root marker: active timer entries hold borrowed
// func/object/extra_args references invisible to the object graph.
// The 4 timer builtin ASFunctions' own_props are marked too (they are
// static but not necessarily in function_registry).
void timerGcMarkRoots(void)
{
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (!g_timers[i].active) continue;
		swfGcMarkVar(&g_timers[i].func);
		swfGcMarkVar(&g_timers[i].object);
		for (int j = 0; j < g_timers[i].extra_arg_count && j < 8; j++)
			swfGcMarkVar(&g_timers[i].extra_args[j]);
	}
	if (g_timer_funcs_init) {
		swfGcMarkFunctionPtr(&g_setInterval_func);
		swfGcMarkFunctionPtr(&g_clearInterval_func);
		swfGcMarkFunctionPtr(&g_setTimeout_func);
		swfGcMarkFunctionPtr(&g_clearTimeout_func);
	}
}

// Scrub-on-free (collector on only): clear timer-held borrowed refs to the
// freed object. A timer whose callback object died was a latent UAF at fire
// time before; firing with UNDEFINED is a deterministic no-op instead.
void timerGcScrubStashes(const void* p)
{
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (!g_timers[i].active) continue;
		ActionVar* vars[10];
		int n = 0;
		vars[n++] = &g_timers[i].func;
		vars[n++] = &g_timers[i].object;
		for (int j = 0; j < g_timers[i].extra_arg_count && j < 8; j++)
			vars[n++] = &g_timers[i].extra_args[j];
		for (int k = 0; k < n; k++) {
			ActionVar* v = vars[k];
			if ((v->type == ACTION_STACK_VALUE_OBJECT || v->type == ACTION_STACK_VALUE_ARRAY) &&
			    (void*)(uintptr_t)v->data.numeric_value == p)
			{
				v->type = ACTION_STACK_VALUE_UNDEFINED;
				v->data.numeric_value = 0;
				v->str_size = 0;
			}
		}
	}
}
