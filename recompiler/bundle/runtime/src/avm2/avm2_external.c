// flash.external.ExternalInterface (AVM2) — the generic EI subset ruled for
// the Archipelago bridge (available / addCallback / call), serving both the
// injected BridgeGeneric games (Seedling, Robot Wants series) and any other
// cooperative AS3.
//
// Availability mirrors the AVM1 gate design (action.c swf_browser_external_call):
// the browser build is "available" ONLY when the page has opted in by exposing
// window.__swfBridge before the SWF first touches ExternalInterface (the page
// shim is a classic script loaded ahead of the wasm module, so ordering holds;
// the check is memoized on first read). Every other build — native trace,
// OFFSCREEN_RENDER graphics, browser pages without the shim — keeps
// available == false, and call/addCallback throw the FP Error #2067 exactly
// like Flash in a non-EI container (graded by the upstream
// avm2/missing_external_interface trace test).
//
// Marshaling is deliberately string-first (the __swfBridge contract's payload
// convention is JSON strings): outward call() arguments are encoded as a JSON
// array of scalars (strings escaped, bool/finite numbers as JSON literals,
// everything else via String() coercion), decoded with JSON.parse on the JS
// side so hosts receive real booleans/numbers (BridgeGeneric's
// stateChanged(pname, pvalue) passes typed scalars); return values coerce to
// an AS3 String or null. No object-graph marshaling.
//
// Inbound (host → AS3): addCallback stores name → function in a C registry
// (GC-rooted via avm2_gc_mark_roots_external) and notifies the page shim,
// which creates the window-visible wrapper; the wrapper funnels into the one
// exported dispatcher avm2_ei_dispatch(name, arg, has_arg) — string in,
// string out. The host must only call it while the VM is quiescent (the
// browser main loop parks at emscripten_sleep between frames — the same
// invariant the AVM1 swf_ei_call_internal relies on).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_gc.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_value.h>

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
#include <emscripten.h>
#endif

#define AVM2_EI_MAX_CALLBACKS 32

typedef struct Avm2EiCallback
{
	char* name;        // C-heap copy (not GC-managed)
	Avm2Value fn;      // GC-rooted via avm2_gc_mark_roots_external
	int in_use;
} Avm2EiCallback;

static Avm2EiCallback g_ei_callbacks[AVM2_EI_MAX_CALLBACKS];
static Avm2Context* g_ei_ctx = NULL;   // set at registration (one VM per process)
static bool g_marshall_exceptions = false;

// ---------------------------------------------------------------------------
// Availability gate
// ---------------------------------------------------------------------------

static int ei_available_now(void)
{
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	static int memo = -1;
	if (memo < 0)
	{
		memo = EM_ASM_INT({
			return (typeof window !== 'undefined' && window.__swfBridge) ? 1 : 0;
		});
	}
	return memo;
#else
	return 0;
#endif
}

_Noreturn static void ei_throw_2067(Avm2Context* ctx)
{
	avm2_throw_error(ctx, NULL,
		"Error #2067: The ExternalInterface is not available in this "
		"container. ExternalInterface requires Internet Explorer ActiveX, "
		"Firefox, Mozilla 1.7.5 and greater, or other browsers that support "
		"NPRuntime.");
}

// ---------------------------------------------------------------------------
// Outward marshaling: encode call() args as a JSON array (string-first)
// ---------------------------------------------------------------------------

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)

typedef struct EiJsonBuf
{
	char* buf;
	size_t len;
	size_t cap;
} EiJsonBuf;

static void ei_buf_put(EiJsonBuf* b, const char* s, size_t n)
{
	if (b->len + n + 1 > b->cap)
	{
		size_t cap = b->cap ? b->cap : 256;
		while (cap < b->len + n + 1) cap *= 2;
		b->buf = realloc(b->buf, cap);
		b->cap = cap;
	}
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = '\0';
}

static void ei_buf_put_json_string(EiJsonBuf* b, const char* s, uint32_t len)
{
	ei_buf_put(b, "\"", 1);
	for (uint32_t i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char) s[i];
		if (c == '"' || c == '\\')
		{
			char esc[2] = { '\\', (char) c };
			ei_buf_put(b, esc, 2);
		}
		else if (c < 0x20)
		{
			char esc[8];
			int n;
			switch (c)
			{
				case '\b': n = snprintf(esc, sizeof(esc), "\\b"); break;
				case '\f': n = snprintf(esc, sizeof(esc), "\\f"); break;
				case '\n': n = snprintf(esc, sizeof(esc), "\\n"); break;
				case '\r': n = snprintf(esc, sizeof(esc), "\\r"); break;
				case '\t': n = snprintf(esc, sizeof(esc), "\\t"); break;
				default:   n = snprintf(esc, sizeof(esc), "\\u%04x", c); break;
			}
			ei_buf_put(b, esc, (size_t) n);
		}
		else
		{
			ei_buf_put(b, (const char*) &s[i], 1);   // UTF-8 passes through
		}
	}
	ei_buf_put(b, "\"", 1);
}

// Append one AS3 value as a JSON scalar. String-first: objects (and anything
// else non-scalar) go through String() coercion, which may legitimately throw
// (a hostile toString) — that propagates to the AS3 caller like any error.
static void ei_buf_put_value(Avm2Context* ctx, EiJsonBuf* b, Avm2Value v)
{
	char num[40];
	switch (v.kind)
	{
		case AVM2_VALUE_UNDEFINED:
		case AVM2_VALUE_NULL:
			ei_buf_put(b, "null", 4);
			return;
		case AVM2_VALUE_BOOL:
			if (v.u.b) ei_buf_put(b, "true", 4);
			else       ei_buf_put(b, "false", 5);
			return;
		case AVM2_VALUE_INTEGER:
		{
			int n = snprintf(num, sizeof(num), "%d", v.u.i);
			ei_buf_put(b, num, (size_t) n);
			return;
		}
		case AVM2_VALUE_NUMBER:
		{
			double d = v.u.d;
			if (!isfinite(d))
			{
				ei_buf_put(b, "null", 4);   // NaN/Inf are not JSON
				return;
			}
			int n = avm2_format_number(num, sizeof(num), d);
			ei_buf_put(b, num, (size_t) n);
			return;
		}
		default:
		{
			const Avm2String* s = avm2_coerce_to_string(ctx, v);
			ei_buf_put_json_string(b, s->utf8, s->len);
			return;
		}
	}
}

// Forward ExternalInterface.call(name, ...) to the page. Resolves dotted
// names ("console.log") as a property path from window with the correct
// receiver (Flash evaluates the name as a JS expression; the page-visible
// host functions BridgeGeneric needs — stateChanged, getItemQueue — are plain
// window properties, console.log resolves through the path walk). Returns a
// malloc'd UTF-8 copy of the JS result coerced to String, or NULL when the
// function is absent, threw, or returned null/undefined.
static char* ei_browser_call(const char* name, const char* args_json)
{
	return (char*) (intptr_t) EM_ASM_INT({
		var name = UTF8ToString($0);
		var args;
		try { args = JSON.parse(UTF8ToString($1)); } catch (e) { args = []; }
		var recv = null;
		var fn = (typeof window !== 'undefined') ? window : null;
		var parts = name.split('.');
		for (var i = 0; i < parts.length && fn != null; i++)
		{
			recv = fn;
			fn = fn[parts[i]];
		}
		if (typeof fn !== 'function') return 0;
		var r;
		try { r = fn.apply(recv === window ? null : recv, args); }
		catch (e) { return 0; }
		if (r === undefined || r === null) return 0;
		if (typeof r !== 'string') r = String(r);
		var len = lengthBytesUTF8(r) + 1;
		var p = _malloc(len);
		stringToUTF8(r, p, len);
		return p;
	}, name, args_json);
}

// Tell the page shim a callback name became (un)available so it can create /
// remove the window-visible wrapper that funnels into avm2_ei_dispatch.
static void ei_notify_callback(const char* name, int registered)
{
	EM_ASM({
		var b = (typeof window !== 'undefined') ? window.__swfBridge : null;
		if (!b) return;
		var hook = $1 ? b.__registerCallback : b.__unregisterCallback;
		if (typeof hook === 'function') hook(UTF8ToString($0));
	}, name, registered);
}

#endif // __EMSCRIPTEN__ && !OFFSCREEN_RENDER

// ---------------------------------------------------------------------------
// Static methods / properties
// ---------------------------------------------------------------------------

static Avm2Value ei_get_available(Avm2Activation* act)
{
	(void) act;
	return avm2_bool(ei_available_now() != 0);
}

static Avm2Value ei_get_object_id(Avm2Activation* act)
{
	(void) act;
	return avm2_null();
}

static Avm2Value ei_get_marshall_exceptions(Avm2Activation* act)
{
	(void) act;
	return avm2_bool(g_marshall_exceptions);
}

static Avm2Value ei_set_marshall_exceptions(Avm2Activation* act)
{
	if (act->argc > 0) g_marshall_exceptions = avm2_coerce_to_boolean(act->args[0]);
	return avm2_undefined();
}

static Avm2EiCallback* ei_find_callback(const char* name)
{
	for (int i = 0; i < AVM2_EI_MAX_CALLBACKS; i++)
	{
		if (g_ei_callbacks[i].in_use && strcmp(g_ei_callbacks[i].name, name) == 0)
		{
			return &g_ei_callbacks[i];
		}
	}
	return NULL;
}

static Avm2Value ei_add_callback(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	if (!ei_available_now()) ei_throw_2067(ctx);
	if (act->argc < 1) return avm2_undefined();

	const Avm2String* name = avm2_coerce_to_string(ctx, act->args[0]);
	Avm2Value fn = (act->argc > 1) ? act->args[1] : avm2_null();

	Avm2EiCallback* cb = ei_find_callback(name->utf8);

	// null/undefined closure unregisters (Ruffle's external_interface test:
	// addCallback("removeMe", null) removes the callback).
	if (fn.kind == AVM2_VALUE_NULL || fn.kind == AVM2_VALUE_UNDEFINED)
	{
		if (cb != NULL)
		{
			free(cb->name);
			cb->name = NULL;
			cb->fn = avm2_undefined();
			cb->in_use = 0;
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
			ei_notify_callback(name->utf8, 0);
#endif
		}
		return avm2_undefined();
	}

	if (cb == NULL)
	{
		for (int i = 0; i < AVM2_EI_MAX_CALLBACKS; i++)
		{
			if (!g_ei_callbacks[i].in_use) { cb = &g_ei_callbacks[i]; break; }
		}
		if (cb == NULL) return avm2_undefined();   // registry full
		cb->name = strdup(name->utf8);
		cb->in_use = 1;
	}
	cb->fn = fn;
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	ei_notify_callback(name->utf8, 1);
#endif
	return avm2_undefined();
}

static Avm2Value ei_call(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	if (!ei_available_now()) ei_throw_2067(ctx);
	if (act->argc < 1) return avm2_null();

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	const Avm2String* name = avm2_coerce_to_string(ctx, act->args[0]);

	EiJsonBuf b = {0};
	ei_buf_put(&b, "[", 1);
	for (uint32_t i = 1; i < act->argc; i++)
	{
		if (i > 1) ei_buf_put(&b, ",", 1);
		ei_buf_put_value(ctx, &b, act->args[i]);
	}
	ei_buf_put(&b, "]", 1);

	char* ret = ei_browser_call(name->utf8, b.buf);
	free(b.buf);

	if (ret == NULL) return avm2_null();
	Avm2Value result = avm2_string(avm2_string_new(ctx, ret, (uint32_t) strlen(ret)));
	free(ret);
	return result;
#else
	// Unreachable (available is false without the browser bridge), but keep a
	// well-defined arm so the non-emscripten compile is self-contained.
	return avm2_null();
#endif
}

// ---------------------------------------------------------------------------
// Inbound dispatcher (host JS → registered AS3 callback)
// ---------------------------------------------------------------------------

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
// One string arg in, string out (the __swfBridge contract is string-first;
// BridgeGeneric's callbacks are wireCheck() / configure(json) / readState()).
// Returns NULL for: unknown name, callback threw, or a null/undefined result.
// The returned pointer is valid until the next dispatch (static buffer).
#ifdef HEAP_TRACK_SITES
extern int g_heap_track_on;
void heap_track_report(void);
void heap_track_reset(void);
// Diagnosis only: AVM2_EI_TRACK set → every heap_alloc inside a dispatch is
// recorded; avm2_ei_track_report() prints what is still outstanding.
EMSCRIPTEN_KEEPALIVE void avm2_ei_track_report(void) { heap_track_report(); }
EMSCRIPTEN_KEEPALIVE void avm2_ei_track_reset(void) { heap_track_reset(); }
EMSCRIPTEN_KEEPALIVE void avm2_ei_track_set(int on) { g_heap_track_on = on; }
#endif

EMSCRIPTEN_KEEPALIVE
const char* avm2_ei_dispatch(const char* name, const char* arg, int has_arg)
{
	static char* ret_buf = NULL;
#ifdef HEAP_TRACK_SITES
	static int track = -1;
	if (track < 0) track = getenv("AVM2_EI_TRACK") != NULL;
	if (track) g_heap_track_on = 1;
#endif

	Avm2Context* ctx = g_ei_ctx;
	if (ctx == NULL || name == NULL) return NULL;
	Avm2EiCallback* cb = ei_find_callback(name);
	if (cb == NULL) return NULL;

	const char* out = NULL;
	Avm2TryFrame top;
	avm2_try_push_catch_all(ctx, &top);
	if (setjmp(top.jb) == 0)
	{
		Avm2Value args[1];
		uint32_t argc = 0;
		if (has_arg && arg != NULL)
		{
			args[0] = avm2_string(avm2_string_new(ctx, arg, (uint32_t) strlen(arg)));
			argc = 1;
		}
		Avm2Value r = avm2_call_value(ctx, cb->fn, avm2_null(), args, argc);
		if (r.kind != AVM2_VALUE_UNDEFINED && r.kind != AVM2_VALUE_NULL)
		{
			const Avm2String* s = avm2_coerce_to_string(ctx, r);
			free(ret_buf);
			ret_buf = malloc((size_t) s->len + 1);
			memcpy(ret_buf, s->utf8, s->len);
			ret_buf[s->len] = '\0';
			out = ret_buf;
		}
	}
	avm2_try_pop_frame(&top);
#ifdef HEAP_TRACK_SITES
	if (track) g_heap_track_on = 0;
#endif
	return out;
}
#endif

// ---------------------------------------------------------------------------
// GC + registration
// ---------------------------------------------------------------------------

void avm2_gc_mark_roots_external(Avm2Context* ctx)
{
	(void) ctx;
	for (int i = 0; i < AVM2_EI_MAX_CALLBACKS; i++)
	{
		if (g_ei_callbacks[i].in_use) avm2_gc_mark_value(g_ei_callbacks[i].fn);
	}
}

void avm2_register_external(Avm2Context* ctx)
{
	g_ei_ctx = ctx;
	Avm2Class* cls = avm2_builtin_class(ctx, "flash.external", "ExternalInterface",
	                                    ctx->builtins.object_class);
	avm2_builtin_add_static_getset(ctx, cls, "available", ei_get_available, NULL);
	avm2_builtin_add_static_getset(ctx, cls, "objectID", ei_get_object_id, NULL);
	avm2_builtin_add_static_getset(ctx, cls, "marshallExceptions",
	                               ei_get_marshall_exceptions,
	                               ei_set_marshall_exceptions);
	avm2_builtin_add_static_method(ctx, cls, "addCallback", ei_add_callback);
	avm2_builtin_add_static_method(ctx, cls, "call", ei_call);
}
