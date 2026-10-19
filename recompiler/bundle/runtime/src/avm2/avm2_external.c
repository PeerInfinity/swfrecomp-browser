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
#include <avm2/avm2_external.h>
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
static Avm2EiProvider g_ei_provider = NULL;   // test harness only (avm2_external.h)

// ---------------------------------------------------------------------------
// Availability gate
// ---------------------------------------------------------------------------

static int ei_available_now(void)
{
	if (g_ei_provider != NULL) return 1;
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
// Structured values + test provider (avm2_external.h)
// ---------------------------------------------------------------------------
//
// Ruffle's external::Value with its from_avm2 / into_avm2 conversions
// (core/src/external.rs), used only when a test harness installed a provider.
// Trees are C-heap owned, so nothing here needs GC rooting; the transient
// AVM2 values built by into_avm2 live only within one call, and the GC only
// collects between ticks.

Avm2EiValue avm2_ei_undefined(void) { Avm2EiValue v = {0}; v.kind = AVM2_EI_UNDEFINED; return v; }
Avm2EiValue avm2_ei_null(void) { Avm2EiValue v = {0}; v.kind = AVM2_EI_NULL; return v; }
Avm2EiValue avm2_ei_bool(bool b) { Avm2EiValue v = {0}; v.kind = AVM2_EI_BOOL; v.b = b; return v; }
Avm2EiValue avm2_ei_number(double d) { Avm2EiValue v = {0}; v.kind = AVM2_EI_NUMBER; v.d = d; return v; }

static Avm2EiValue ei_string_n(const char* s, uint32_t len)
{
	Avm2EiValue v = {0};
	v.kind = AVM2_EI_STRING;
	v.str = malloc((size_t) len + 1);
	memcpy(v.str, s, len);
	v.str[len] = '\0';
	return v;
}

Avm2EiValue avm2_ei_string(const char* s) { return ei_string_n(s, (uint32_t) strlen(s)); }

Avm2EiValue avm2_ei_list(const Avm2EiValue* items, uint32_t n)
{
	Avm2EiValue v = {0};
	v.kind = AVM2_EI_LIST;
	v.count = n;
	if (n > 0)
	{
		v.items = malloc(n * sizeof(Avm2EiValue));
		memcpy(v.items, items, n * sizeof(Avm2EiValue));
	}
	return v;
}

// Insert key → value into a sorted map, replacing an existing key (BTreeMap
// insert). Takes ownership of `key` and `val`.
static void ei_map_insert(Avm2EiValue* m, uint32_t* cap, char* key, Avm2EiValue val)
{
	uint32_t pos = 0;
	while (pos < m->count && strcmp(m->keys[pos], key) < 0) pos++;
	if (pos < m->count && strcmp(m->keys[pos], key) == 0)
	{
		free(key);
		avm2_ei_value_free(&m->items[pos]);
		m->items[pos] = val;
		return;
	}
	if (m->count == *cap)
	{
		*cap = *cap ? *cap * 2 : 8;
		m->keys = realloc(m->keys, *cap * sizeof(char*));
		m->items = realloc(m->items, *cap * sizeof(Avm2EiValue));
	}
	memmove(&m->keys[pos + 1], &m->keys[pos], (m->count - pos) * sizeof(char*));
	memmove(&m->items[pos + 1], &m->items[pos], (m->count - pos) * sizeof(Avm2EiValue));
	m->keys[pos] = key;
	m->items[pos] = val;
	m->count++;
}

Avm2EiValue avm2_ei_object(char** keys, Avm2EiValue* values, uint32_t n)
{
	Avm2EiValue m = {0};
	m.kind = AVM2_EI_OBJECT;
	uint32_t cap = 0;
	for (uint32_t i = 0; i < n; i++) ei_map_insert(&m, &cap, keys[i], values[i]);
	free(keys);
	free(values);
	return m;
}

void avm2_ei_value_free(Avm2EiValue* v)
{
	if (v == NULL) return;
	free(v->str);
	for (uint32_t i = 0; i < v->count; i++)
	{
		if (v->items != NULL) avm2_ei_value_free(&v->items[i]);
		if (v->keys != NULL) free(v->keys[i]);
	}
	free(v->items);
	free(v->keys);
	memset(v, 0, sizeof(*v));
}

// Value::from_avm2. Arrays become List (holes read as Null); every other
// object becomes Object over its enumerable properties. A cyclic graph would
// recurse forever in Ruffle; cap the depth instead.
static Avm2EiValue ei_from_avm2(Avm2Context* ctx, Avm2Value v, int depth)
{
	switch (v.kind)
	{
		case AVM2_VALUE_UNDEFINED: return avm2_ei_undefined();
		case AVM2_VALUE_NULL:      return avm2_ei_null();
		case AVM2_VALUE_BOOL:      return avm2_ei_bool(v.u.b);
		case AVM2_VALUE_INTEGER:   return avm2_ei_number((double) v.u.i);
		case AVM2_VALUE_NUMBER:    return avm2_ei_number(v.u.d);
		case AVM2_VALUE_STRING:
		{
			const Avm2String* s = avm2_coerce_to_string(ctx, v);
			return ei_string_n(s->utf8, s->len);
		}
		case AVM2_VALUE_OBJECT:
		{
			Avm2Object* obj = v.u.obj;
			if (obj == NULL || depth > 256) return avm2_ei_null();
			if (obj->kind == AVM2_OBJ_ARRAY)
			{
				Avm2ArrayExt* ext = avm2_array_ext(obj);
				uint32_t n = ext ? ext->length : 0;
				Avm2EiValue l = {0};
				l.kind = AVM2_EI_LIST;
				l.count = n;
				if (n > 0) l.items = calloc(n, sizeof(Avm2EiValue));
				for (uint32_t i = 0; i < n; i++)
				{
					Avm2Value e = avm2_array_get(obj, i);
					l.items[i] = (e.kind == AVM2_VALUE_HOLE)
						? avm2_ei_null() : ei_from_avm2(ctx, e, depth + 1);
				}
				return l;
			}
			Avm2EiValue m = {0};
			m.kind = AVM2_EI_OBJECT;
			uint32_t cap = 0;
			for (uint32_t idx = avm2_object_next_enumerant(obj, 0); idx != 0;
			     idx = avm2_object_next_enumerant(obj, idx))
			{
				const Avm2String* name = avm2_coerce_to_string(ctx,
					avm2_object_enumerant_name(ctx, obj, idx));
				char* key = malloc((size_t) name->len + 1);
				memcpy(key, name->utf8, name->len);
				key[name->len] = '\0';
				Avm2Value ev = avm2_object_enumerant_value(ctx, obj, idx);
				ei_map_insert(&m, &cap, key, ei_from_avm2(ctx, ev, depth + 1));
			}
			return m;
		}
		default:
			return avm2_ei_null();
	}
}

// Value::into_avm2: Object → a fresh plain Object (dynamic props in key
// order), List → Array.
static Avm2Value ei_into_avm2(Avm2Context* ctx, const Avm2EiValue* v)
{
	switch (v->kind)
	{
		case AVM2_EI_NULL:   return avm2_null();
		case AVM2_EI_BOOL:   return avm2_bool(v->b);
		case AVM2_EI_NUMBER: return avm2_number(v->d);
		case AVM2_EI_STRING:
			return avm2_string(avm2_string_new(ctx, v->str, (uint32_t) strlen(v->str)));
		case AVM2_EI_LIST:
		{
			Avm2Object* arr = avm2_array_new(ctx, 0);
			for (uint32_t i = 0; i < v->count; i++)
			{
				avm2_array_push(ctx, arr, ei_into_avm2(ctx, &v->items[i]));
			}
			return avm2_object_value(arr);
		}
		case AVM2_EI_OBJECT:
		{
			Avm2Object* obj = avm2_object_alloc(ctx, AVM2_OBJ_SCRIPT, 0);
			obj->cls = ctx->builtins.object_class;
			obj->proto = ctx->builtins.object_class->prototype_obj;
			for (uint32_t i = 0; i < v->count; i++)
			{
				avm2_object_set_dynamic(ctx, obj, v->keys[i], (uint32_t) strlen(v->keys[i]),
				                        ei_into_avm2(ctx, &v->items[i]));
			}
			return avm2_object_value(obj);
		}
		default:
			return avm2_undefined();
	}
}

void avm2_ei_set_provider(Avm2EiProvider provider)
{
	g_ei_provider = provider;
}

// ExternalInterface.call under a provider (Ruffle external_interface.rs call).
static Avm2Value ei_provider_call(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	const Avm2String* name = avm2_coerce_to_string(ctx, act->args[0]);
	char* cname = malloc((size_t) name->len + 1);
	memcpy(cname, name->utf8, name->len);
	cname[name->len] = '\0';

	uint32_t n = act->argc - 1;
	Avm2EiValue* args = n ? calloc(n, sizeof(Avm2EiValue)) : NULL;
	for (uint32_t i = 0; i < n; i++) args[i] = ei_from_avm2(ctx, act->args[i + 1], 0);

	Avm2EiValue r = g_ei_provider(cname, args, n);

	for (uint32_t i = 0; i < n; i++) avm2_ei_value_free(&args[i]);
	free(args);
	free(cname);
	Avm2Value out = ei_into_avm2(ctx, &r);
	avm2_ei_value_free(&r);
	return out;
}

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

	const Avm2String* name_s = avm2_coerce_to_string(ctx, act->args[0]);
	const char* name = avm2_string_cstr(ctx, name_s);
	Avm2Value fn = (act->argc > 1) ? act->args[1] : avm2_null();

	Avm2EiCallback* cb = ei_find_callback(name);

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
			ei_notify_callback(name, 0);
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
		cb->name = strdup(name);
		cb->in_use = 1;
	}
	cb->fn = fn;
#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
	ei_notify_callback(name, 1);
#endif
	return avm2_undefined();
}

static Avm2Value ei_call(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	if (!ei_available_now()) ei_throw_2067(ctx);
	if (act->argc < 1) return avm2_null();

	if (g_ei_provider != NULL) return ei_provider_call(act);

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

	char* ret = ei_browser_call(avm2_string_cstr(ctx, name), b.buf);
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

// Host → AS3, structured (Ruffle Player::call_internal_interface +
// Callback::call): unknown name → Null; the callback runs with a null
// receiver; a throw is reported as uncaught and yields Null.
Avm2EiValue avm2_ei_call_internal(const char* name, const Avm2EiValue* args,
                                  uint32_t argc)
{
	Avm2Context* ctx = g_ei_ctx;
	Avm2EiCallback* cb = (ctx != NULL && name != NULL) ? ei_find_callback(name) : NULL;
	if (cb == NULL) return avm2_ei_null();

	Avm2EiValue out = avm2_ei_null();
	Avm2Value* vals = argc ? calloc(argc, sizeof(Avm2Value)) : NULL;
	Avm2TryFrame top;
	avm2_try_push_catch_all(ctx, &top);
	if (setjmp(top.jb) == 0)
	{
		for (uint32_t i = 0; i < argc; i++) vals[i] = ei_into_avm2(ctx, &args[i]);
		Avm2Value r = avm2_call_value(ctx, cb->fn, avm2_null(), vals, argc);
		out = ei_from_avm2(ctx, r, 0);
	}
	avm2_try_pop_frame(&top);
	free(vals);
	return out;
}

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
