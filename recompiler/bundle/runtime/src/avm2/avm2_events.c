// flash.events — the real event machinery (Stage 5 tranche 1).
//
// Event carries internal state (Ruffle events.rs Event struct) surfaced
// through const getters; EventDispatcher implements Ruffle's DispatchList
// semantics (per-type priority buckets, capture split, dedup by
// (listener, useCapture)) and the 3-phase DOM dispatch algorithm
// (events.rs dispatch_event), plus the FP broadcast-event registry
// (avm2.rs BROADCAST_WHITELIST / broadcast_event) that the frame
// lifecycle (tranche 3) drives.
//
// Sources of truth: Ruffle core/src/avm2/events.rs,
// globals/flash/events/{Event,EventDispatcher}.as + event*.rs.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <memory/heap.h>

#include <avm2/avm2_gc.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>
#include <avm2/avm2_value.h>

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static Avm2PropKey events_key(const char* ns, const char* name)
{
	Avm2PropKey k;
	k.name = name;
	k.name_len = (uint32_t) strlen(name);
	k.ns_kind = 0x16;
	k.ns_priv = NULL;
	k.ns_uri = ns;
	k.ns_len = (uint32_t) strlen(ns);
	return k;
}

static int class_is_a(const Avm2Class* cls, const Avm2Class* ancestor)
{
	for (const Avm2Class* c = cls; c != NULL; c = c->super_class)
	{
		if (c == ancestor) return 1;
	}
	return 0;
}

Avm2EventExt* avm2_event_ext_of(Avm2Value v)
{
	if (v.kind != AVM2_VALUE_OBJECT || v.u.obj == NULL) return NULL;
	Avm2Object* o = v.u.obj;
	Avm2Context* ctx = avm2_get_context();
	if (o->cls == NULL || !class_is_a(o->cls, ctx->builtins.event_class)) return NULL;
	return (Avm2EventExt*) o->native_ext;
}

Avm2EventDispatcherExt* avm2_dispatcher_ext_of(Avm2Context* ctx, Avm2Object* obj)
{
	if (obj == NULL || obj->cls == NULL) return NULL;
	if (!class_is_a(obj->cls, ctx->builtins.event_dispatcher_class)) return NULL;
	return (Avm2EventDispatcherExt*) obj->native_ext;
}

Avm2Object* avm2_display_parent(Avm2Context* ctx, Avm2Object* obj)
{
	if (obj == NULL || obj->cls == NULL || obj->native_ext == NULL) return NULL;
	if (!class_is_a(obj->cls, ctx->builtins.display_object_class)) return NULL;
	return ((Avm2DisplayObjectExt*) obj->native_ext)->parent;
}

// ---------------------------------------------------------------------------
// flash.events.Event
// ---------------------------------------------------------------------------

enum
{
	EVT_PROP_ALLOW = 0,
	EVT_PROP_STOP = 1,
	EVT_PROP_STOP_IMMEDIATE = 2,
};

enum
{
	EVT_PHASE_CAPTURING = 1,
	EVT_PHASE_AT_TARGET = 2,
	EVT_PHASE_BUBBLING = 3,
};

static void event_native_init(Avm2Context* ctx, Avm2Object* obj)
{
	(void) ctx;
	Avm2EventExt* ext = obj->native_ext;
	ext->phase = EVT_PHASE_AT_TARGET;
}

static Avm2EventExt* this_event(Avm2Activation* act)
{
	Avm2EventExt* ext = avm2_event_ext_of(act->this_val);
	if (ext == NULL)
	{
		avm2_fatal("flash.events.Event native on a non-Event receiver");
	}
	return ext;
}

static Avm2Value event_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = act->argc > 1 && avm2_coerce_to_boolean(act->args[1]);
	ext->cancelable = act->argc > 2 && avm2_coerce_to_boolean(act->args[2]);
	return avm2_undefined();
}

static Avm2Value event_get_type(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	return ext->type != NULL ? avm2_string(ext->type) : avm2_null();
}

static Avm2Value event_get_bubbles(Avm2Activation* act)
{
	return avm2_bool(this_event(act)->bubbles != 0);
}

static Avm2Value event_get_cancelable(Avm2Activation* act)
{
	return avm2_bool(this_event(act)->cancelable != 0);
}

static Avm2Value event_get_target(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	return ext->target != NULL ? avm2_object_value(ext->target) : avm2_null();
}

static Avm2Value event_get_current_target(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	return ext->current_target != NULL ? avm2_object_value(ext->current_target)
	                                   : avm2_null();
}

static Avm2Value event_get_event_phase(Avm2Activation* act)
{
	return avm2_uint_value(this_event(act)->phase);
}

static Avm2Value event_is_default_prevented(Avm2Activation* act)
{
	return avm2_bool(this_event(act)->cancelled != 0);
}

static Avm2Value event_prevent_default(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	if (ext->cancelable) ext->cancelled = 1;
	return avm2_undefined();
}

static Avm2Value event_stop_propagation(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	if (ext->propagation != EVT_PROP_STOP_IMMEDIATE)
	{
		ext->propagation = EVT_PROP_STOP;
	}
	return avm2_undefined();
}

static Avm2Value event_stop_immediate_propagation(Avm2Activation* act)
{
	this_event(act)->propagation = EVT_PROP_STOP_IMMEDIATE;
	return avm2_undefined();
}

static Avm2Value event_clone(Avm2Activation* act)
{
	// Event.as: return new Event(this.type, this.bubbles, this.cancelable)
	// — always the BASE Event class (a subclass that fails to override
	// clone loses its type — event_clone), reading the props virtually.
	Avm2Context* ctx = act->ctx;
	Avm2Value args[3];
	args[0] = avm2_get_public_property(ctx, act->this_val, "type", 4, NULL);
	args[1] = avm2_get_public_property(ctx, act->this_val, "bubbles", 7, NULL);
	args[2] = avm2_get_public_property(ctx, act->this_val, "cancelable", 10, NULL);
	return avm2_class_construct(ctx, ctx->builtins.event_class, args, 3);
}

static Avm2Value event_format_to_string(Avm2Activation* act)
{
	// Event.as formatToString(className, ...arguments): each key reads
	// `this[key]` virtually; String values print quoted.
	Avm2Context* ctx = act->ctx;
	const Avm2String* cname = act->argc > 0
		? avm2_coerce_to_string(ctx, act->args[0])
		: avm2_string_from_literal(ctx, "undefined");
	// Build into a growing heap buffer.
	uint32_t cap = 64 + cname->len;
	char* buf = avm2_alloc(ctx, cap);
	uint32_t len = 0;
	buf[len++] = '[';
	memcpy(buf + len, cname->utf8, cname->len);
	len += cname->len;
	for (uint32_t i = 1; i < act->argc; i++)
	{
		// The rest params are typed String in Event.as: undefined/null
		// coerce to null, which concatenates (and property-reads) as
		// "null" — `formatToString("C", undefined)` prints null=undefined.
		const Avm2String* key =
			(act->args[i].kind == AVM2_VALUE_UNDEFINED
			 || act->args[i].kind == AVM2_VALUE_NULL)
				? avm2_string_from_literal(ctx, "null")
				: avm2_coerce_to_string(ctx, act->args[i]);
		Avm2Value val = avm2_get_public_property(ctx, act->this_val,
		                                         key->utf8, key->len, NULL);
		int quoted = (val.kind == AVM2_VALUE_STRING);
		const Avm2String* vs = avm2_coerce_to_string(ctx, val);
		uint32_t need = len + 1 + key->len + 1 + vs->len + 4;
		if (need > cap)
		{
			uint32_t new_cap = need * 2;
			char* grown = avm2_alloc(ctx, new_cap);
			memcpy(grown, buf, len);
			heap_free(ctx->app, buf);
			buf = grown;
			cap = new_cap;
		}
		buf[len++] = ' ';
		memcpy(buf + len, key->utf8, key->len);
		len += key->len;
		buf[len++] = '=';
		if (quoted) buf[len++] = '"';
		memcpy(buf + len, vs->utf8, vs->len);
		len += vs->len;
		if (quoted) buf[len++] = '"';
	}
	if (len + 2 > cap)
	{
		char* grown = avm2_alloc(ctx, len + 2);
		memcpy(grown, buf, len);
		heap_free(ctx->app, buf);
		buf = grown;
	}
	buf[len++] = ']';
	{ const Avm2String* r_ = avm2_string_new(ctx, buf, len); heap_free(ctx->app, buf); return avm2_string(r_); }  // copied: the buffer is scratch
}

static Avm2Value event_to_string(Avm2Activation* act)
{
	// Event.as: this.formatToString("Event","type","bubbles","cancelable",
	// "eventPhase") — a virtual call (subclasses may override).
	Avm2Context* ctx = act->ctx;
	Avm2Value args[5];
	args[0] = avm2_string(avm2_string_from_literal(ctx, "Event"));
	args[1] = avm2_string(avm2_string_from_literal(ctx, "type"));
	args[2] = avm2_string(avm2_string_from_literal(ctx, "bubbles"));
	args[3] = avm2_string(avm2_string_from_literal(ctx, "cancelable"));
	args[4] = avm2_string(avm2_string_from_literal(ctx, "eventPhase"));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 5);
}

Avm2Object* avm2_event_new(Avm2Context* ctx, const Avm2String* type,
                           int bubbles, int cancelable)
{
	Avm2Value args[3];
	args[0] = avm2_string(type);
	args[1] = avm2_bool(bubbles != 0);
	args[2] = avm2_bool(cancelable != 0);
	Avm2Value v = avm2_class_construct(ctx, ctx->builtins.event_class, args, 3);
	return v.u.obj;
}

// ---------------------------------------------------------------------------
// EventDispatcher listener list (Ruffle events.rs DispatchList)
// ---------------------------------------------------------------------------

struct EDListener
{
	EDListener* next;
	const Avm2String* type;
	Avm2Value fn;
	int32_t priority;
	uint8_t use_capture;
};

// Any registration with the same (type, listener, useCapture) already
// present (at ANY priority) makes addEventListener a silent no-op.
static int ed_list_contains(EDListener* head, const Avm2String* type,
                            Avm2Value fn, int use_capture)
{
	for (EDListener* l = head; l != NULL; l = l->next)
	{
		if (l->use_capture == use_capture && avm2_string_equals(l->type, type)
		    && avm2_strict_eq(l->fn, fn))
		{
			return 1;
		}
	}
	return 0;
}

static void ed_list_add(Avm2Context* ctx, Avm2EventDispatcherExt* ext,
                        const Avm2String* type, Avm2Value fn, int use_capture,
                        int32_t priority)
{
	if (ed_list_contains(ext->head, type, fn, use_capture)) return;
	EDListener* nl = avm2_alloc(ctx, sizeof(EDListener));
	nl->type = type;
	nl->fn = fn;
	nl->priority = priority;
	nl->use_capture = (uint8_t) use_capture;
	// Insertion point: before the first same-type node with a strictly
	// lower priority (higher priorities run first; equal priorities run in
	// insertion order — eventdispatcher_dispatchevent_handlerorder).
	EDListener** link = &ext->head;
	while (*link != NULL)
	{
		EDListener* cur = *link;
		if (avm2_string_equals(cur->type, type) && cur->priority < priority)
		{
			break;
		}
		link = &cur->next;
	}
	nl->next = *link;
	*link = nl;
}

static void ed_list_remove(Avm2EventDispatcherExt* ext, const Avm2String* type,
                           Avm2Value fn, int use_capture)
{
	for (EDListener** link = &ext->head; *link != NULL; )
	{
		EDListener* l = *link;
		if (l->use_capture == (uint8_t) use_capture
		    && avm2_string_equals(l->type, type) && avm2_strict_eq(l->fn, fn))
		{
			*link = l->next;
		}
		else
		{
			link = &l->next;
		}
	}
}

static int ed_list_has_type(EDListener* head, const Avm2String* type)
{
	for (EDListener* l = head; l != NULL; l = l->next)
	{
		if (avm2_string_equals(l->type, type)) return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Broadcast registry (Ruffle avm2.rs BROADCAST_WHITELIST / broadcast_list)
// ---------------------------------------------------------------------------

static const char* const k_broadcast_whitelist[4] = {
	"enterFrame", "exitFrame", "frameConstructed", "render"
};

typedef struct BroadcastBucket
{
	uint32_t count;
	uint32_t cap;
	Avm2Object** objs;  // registration order; dedup by identity
} BroadcastBucket;

static BroadcastBucket g_broadcast[4];

// --- GC roots + tracing (Stage 11) ------------------------------------------

// Root marker: the broadcast registries (enterFrame/exitFrame/frameConstructed/
// render) hold live listener objects by identity.
void avm2_gc_mark_roots_events(Avm2Context* ctx)
{
	(void) ctx;
	for (int i = 0; i < 4; i++)
	{
		BroadcastBucket* b = &g_broadcast[i];
		for (uint32_t j = 0; j < b->count; j++) avm2_gc_mark_object(b->objs[j]);
	}
}

// Ext tracer: the EventDispatcher listener list (ext->head) is a chain of
// separately-allocated EDListener nodes hung off the ext, so the conservative
// blob scan cannot reach the listener closures — mark them precisely. Covers
// every EventDispatcher-derived object (all display objects, Timer, Sound,
// SharedObject, plain EventDispatcher).
void avm2_events_gc_trace_ext(Avm2Object* o)
{
	Avm2Context* ctx = avm2_get_context();
	Avm2EventDispatcherExt* ext = avm2_dispatcher_ext_of(ctx, o);
	if (ext == NULL) return;
	for (EDListener* l = ext->head; l != NULL; l = l->next)
	{
		avm2_gc_mark_value(l->fn);
		avm2_gc_mark_string(l->type);
	}
}

// GC free hook: free the EDListener node chain a swept EventDispatcher owns
// (avm2_alloc'd, per-dispatcher). No-op for non-dispatchers.
void avm2_events_gc_free_ext(Avm2Context* ctx, Avm2Object* o)
{
	Avm2EventDispatcherExt* ext = avm2_dispatcher_ext_of(ctx, o);
	if (ext == NULL) return;
	for (EDListener* l = ext->head; l != NULL; )
	{
		EDListener* next = l->next;
		heap_free(ctx->app, l);
		l = next;
	}
	ext->head = NULL;
}

static int broadcast_index(const Avm2String* type)
{
	for (int i = 0; i < 4; i++)
	{
		size_t wl = strlen(k_broadcast_whitelist[i]);
		if (type->len == wl && memcmp(type->utf8, k_broadcast_whitelist[i], wl) == 0)
		{
			return i;
		}
	}
	return -1;
}

static void broadcast_register(Avm2Context* ctx, Avm2Object* obj,
                               const Avm2String* type)
{
	int bi = broadcast_index(type);
	if (bi < 0) return;
	BroadcastBucket* b = &g_broadcast[bi];
	for (uint32_t i = 0; i < b->count; i++)
	{
		if (b->objs[i] == obj) return;
	}
	if (b->count == b->cap)
	{
		uint32_t new_cap = b->cap > 0 ? b->cap * 2 : 16;
		Avm2Object** grown = avm2_alloc(ctx, new_cap * sizeof(Avm2Object*));
		memcpy(grown, b->objs, b->count * sizeof(Avm2Object*));
		b->objs = grown;
		b->cap = new_cap;
	}
	b->objs[b->count++] = obj;
}

// ---------------------------------------------------------------------------
// Dispatch (Ruffle events.rs dispatch_event_to_target / dispatch_event)
// ---------------------------------------------------------------------------

// Call every matching handler on `dispatcher` for the event's current
// phase. target/currentTarget are only stamped when at least one handler
// matched (that is FP's "handled" signal — event_target_set).
static void dispatch_event_to_target(Avm2Context* ctx, Avm2Object* dispatcher,
                                     Avm2Object* real_target,
                                     Avm2Object* current_target, Avm2Object* event)
{
	Avm2EventDispatcherExt* ext = avm2_dispatcher_ext_of(ctx, dispatcher);
	Avm2EventExt* evt = (Avm2EventExt*) event->native_ext;
	if (ext == NULL || evt == NULL || evt->type == NULL) return;

	int use_capture = (evt->phase == EVT_PHASE_CAPTURING);

	// Snapshot the handlers before calling any of them: listeners added or
	// removed by a handler don't affect the in-flight dispatch.
	uint32_t n = 0;
	for (EDListener* l = ext->head; l != NULL; l = l->next)
	{
		if (l->use_capture == use_capture && avm2_string_equals(l->type, evt->type))
		{
			n++;
		}
	}
	if (n == 0) return;
	Avm2Value stack_fns[16];
	Avm2Value* fns = (n <= 16) ? stack_fns : avm2_alloc(ctx, n * sizeof(Avm2Value));
	uint32_t k = 0;
	for (EDListener* l = ext->head; l != NULL; l = l->next)
	{
		if (l->use_capture == use_capture && avm2_string_equals(l->type, evt->type))
		{
			fns[k++] = l->fn;
		}
	}

	evt->target = real_target;
	evt->current_target = current_target;

	// Handlers run with the toplevel global as `this` (function_unbound_this)
	// and an uncaught error aborts only that handler, not the dispatch
	// (event_handler_exception).
	Avm2Value recv = avm2_object_value(ctx->builtin_globals);
	for (uint32_t i = 0; i < n; i++)
	{
		if (evt->propagation == EVT_PROP_STOP_IMMEDIATE) break;
		Avm2TryFrame top;
		avm2_try_push_catch_all(ctx, &top);
		if (setjmp(top.jb) == 0)
		{
			Avm2Value args[1] = { avm2_object_value(event) };
			avm2_call_value(ctx, fns[i], recv, args, 1);
		}
		avm2_try_pop_frame(&top);
	}
}

int avm2_dispatch_event(Avm2Context* ctx, Avm2Object* dispatcher, Avm2Object* event)
{
	Avm2EventDispatcherExt* ext = avm2_dispatcher_ext_of(ctx, dispatcher);
	Avm2EventExt* evt = (Avm2EventExt*) event->native_ext;
	if (evt == NULL)
	{
		avm2_fatal("avm2_dispatch_event on a non-Event object");
	}

	Avm2Object* target = dispatcher;
	if (ext != NULL && ext->ed_target.kind == AVM2_VALUE_OBJECT
	    && ext->ed_target.u.obj != NULL)
	{
		target = ext->ed_target.u.obj;
	}

	// Ancestor chain: nearest parent first (Ruffle builds the same list).
	uint32_t anc_count = 0;
	Avm2Object* anc_stack[64];
	for (Avm2Object* p = avm2_display_parent(ctx, target);
	     p != NULL && anc_count < 64; p = avm2_display_parent(ctx, p))
	{
		anc_stack[anc_count++] = p;
	}

	evt->phase = EVT_PHASE_CAPTURING;
	for (uint32_t i = anc_count; i > 0; i--)
	{
		if (evt->propagation != EVT_PROP_ALLOW) break;
		Avm2Object* anc = anc_stack[i - 1];
		dispatch_event_to_target(ctx, anc, target, anc, event);
	}

	evt->phase = EVT_PHASE_AT_TARGET;
	if (evt->propagation == EVT_PROP_ALLOW)
	{
		dispatch_event_to_target(ctx, dispatcher, target, target, event);
	}

	evt->phase = EVT_PHASE_BUBBLING;
	if (evt->bubbles)
	{
		for (uint32_t i = 0; i < anc_count; i++)
		{
			if (evt->propagation != EVT_PROP_ALLOW) break;
			dispatch_event_to_target(ctx, anc_stack[i], target, anc_stack[i], event);
		}
	}

	return evt->target != NULL;
}

void avm2_broadcast_event(Avm2Context* ctx, Avm2Object* event, Avm2Class* filter_class)
{
	Avm2EventExt* evt = (Avm2EventExt*) event->native_ext;
	if (evt == NULL || evt->type == NULL) return;
	int bi = broadcast_index(evt->type);
	if (bi < 0) return;
	BroadcastBucket* b = &g_broadcast[bi];
	// Ruffle captures the list length BEFORE iterating: dispatchers
	// registered DURING the broadcast wait until the next one
	// (movieclip_displayevents_enterframesymbol).
	uint32_t n = b->count;
	for (uint32_t i = 0; i < n; i++)
	{
		Avm2Object* obj = b->objs[i];
		if (filter_class != NULL
		    && (obj->cls == NULL || !class_is_a(obj->cls, filter_class)))
		{
			continue;
		}
		Avm2EventDispatcherExt* ext = avm2_dispatcher_ext_of(ctx, obj);
		Avm2Object* target = obj;
		if (ext != NULL && ext->ed_target.kind == AVM2_VALUE_OBJECT
		    && ext->ed_target.u.obj != NULL)
		{
			target = ext->ed_target.u.obj;
		}
		evt->phase = EVT_PHASE_AT_TARGET;
		evt->propagation = EVT_PROP_ALLOW;
		dispatch_event_to_target(ctx, obj, target, target, event);
	}
}

// ---------------------------------------------------------------------------
// EventDispatcher natives
// ---------------------------------------------------------------------------

static Avm2EventDispatcherExt* this_dispatcher(Avm2Activation* act)
{
	if (act->this_val.kind != AVM2_VALUE_OBJECT) return NULL;
	return avm2_dispatcher_ext_of(act->ctx, act->this_val.u.obj);
}

static Avm2Class* g_uncaught_error_events_class;

static Avm2Value ed_init(Avm2Activation* act)
{
	// EventDispatcher(target:IEventDispatcher = null).
	Avm2EventDispatcherExt* ext = this_dispatcher(act);
	if (ext != NULL && act->argc > 0)
	{
		ext->ed_target = act->args[0];
	}
	return avm2_undefined();
}

static Avm2Value ed_add_event_listener(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2EventDispatcherExt* ext = this_dispatcher(act);
	if (ext == NULL) return avm2_undefined();
	Avm2Value listener = act->argc > 1 ? act->args[1] : avm2_null();
	if (listener.kind == AVM2_VALUE_NULL || listener.kind == AVM2_VALUE_UNDEFINED)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #2007: Parameter listener must be non-null.");
	}
	const Avm2String* type = act->argc > 0
		? avm2_coerce_to_string(ctx, act->args[0])
		: avm2_string_from_literal(ctx, "null");
	int use_capture = act->argc > 2 && avm2_coerce_to_boolean(act->args[2]);
	int32_t priority = act->argc > 3 ? avm2_coerce_to_i32(ctx, act->args[3]) : 0;
	ed_list_add(ctx, ext, type, listener, use_capture, priority);
	broadcast_register(ctx, act->this_val.u.obj, type);
	return avm2_undefined();
}

static Avm2Value ed_remove_event_listener(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2EventDispatcherExt* ext = this_dispatcher(act);
	if (ext == NULL || act->argc < 2) return avm2_undefined();
	const Avm2String* type = avm2_coerce_to_string(ctx, act->args[0]);
	int use_capture = act->argc > 2 && avm2_coerce_to_boolean(act->args[2]);
	ed_list_remove(ext, type, act->args[1], use_capture);
	return avm2_undefined();
}

static Avm2Value ed_has_event_listener(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2EventDispatcherExt* ext = this_dispatcher(act);
	if (ext == NULL || act->argc < 1) return avm2_bool(false);
	const Avm2String* type = avm2_coerce_to_string(ctx, act->args[0]);
	return avm2_bool(ed_list_has_type(ext->head, type) != 0);
}

static Avm2Value ed_will_trigger(Avm2Activation* act)
{
	// Ruffle event_dispatcher.rs will_trigger: own list, else recurse on
	// the display parent of the aggregation target.
	Avm2Context* ctx = act->ctx;
	if (act->argc < 1 || act->this_val.kind != AVM2_VALUE_OBJECT)
	{
		return avm2_bool(false);
	}
	const Avm2String* type = avm2_coerce_to_string(ctx, act->args[0]);
	Avm2Object* cur = act->this_val.u.obj;
	while (cur != NULL)
	{
		Avm2EventDispatcherExt* ext = avm2_dispatcher_ext_of(ctx, cur);
		if (ext != NULL && ed_list_has_type(ext->head, type))
		{
			return avm2_bool(true);
		}
		Avm2Object* target = cur;
		if (ext != NULL && ext->ed_target.kind == AVM2_VALUE_OBJECT
		    && ext->ed_target.u.obj != NULL)
		{
			target = ext->ed_target.u.obj;
		}
		cur = avm2_display_parent(ctx, target);
	}
	return avm2_bool(false);
}

static Avm2Value ed_dispatch_event(Avm2Activation* act)
{
	// EventDispatcher.as dispatchEvent: a redispatch (event.target already
	// set — read via the PUBLIC getter) dispatches event.clone() (also a
	// virtual call); a null clone throws 2007 (event_clone_error_redispatch).
	Avm2Context* ctx = act->ctx;
	Avm2Value event_val = act->argc > 0 ? act->args[0] : avm2_null();
	if (event_val.kind != AVM2_VALUE_OBJECT || event_val.u.obj == NULL)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #2007: Parameter event must be non-null.");
	}
	Avm2Value tv = avm2_get_public_property(ctx, event_val, "target", 6, NULL);
	if (avm2_coerce_to_boolean(tv))
	{
		event_val = avm2_call_public_property(ctx, event_val, "clone", 5, NULL, 0);
		if (event_val.kind != AVM2_VALUE_OBJECT || event_val.u.obj == NULL)
		{
			avm2_throw_error(ctx, ctx->builtins.type_error_class,
			                 "Error #2007: Parameter event must be non-null.");
		}
	}
	Avm2EventExt* evt = avm2_event_ext_of(event_val);
	if (evt == NULL)
	{
		avm2_fatal("dispatchEvent on a non-Event argument (unimplemented coercion)");
	}
	if (act->this_val.kind != AVM2_VALUE_OBJECT)
	{
		return avm2_bool(false);
	}
	// FP's EventDispatcher.dispatchEvent is AS3 playerglobal code whose body
	// is `return dispatchEventFunction(event)` (Flash) /
	// `dispatchEventInternal(event)` (Ruffle's playerglobal,
	// EventDispatcher.as), so a listener that throws reports TWO frames for
	// the dispatch, not one: the inner native worker under the outer AS3
	// method. Ours is a single native method, so push the worker's frame
	// explicitly around the dispatch (avm2/event_handler_exception grades
	// exactly this line). Only the AS3 entry point does it — the runtime's
	// own broadcasts (enterFrame &c.) call avm2_dispatch_event directly and
	// FP shows no dispatch frames for those, which the same fixture's first
	// trace pins.
	static const Avm2MethodRef dispatch_internal = {
		NULL, NULL, "flash.events::EventDispatcher/dispatchEventInternal", 0, 0
	};
	avm2_callstack_push(ctx, &dispatch_internal, NULL);
	avm2_dispatch_event(ctx, act->this_val.u.obj, event_val.u.obj);
	avm2_callstack_pop(ctx);
	return avm2_bool(evt->cancelled == 0);
}

static Avm2Value ed_to_string(Avm2Activation* act)
{
	// EventDispatcher.as: Object.prototype.toString.call(this) — the
	// CURRENT prototype function, looked up dynamically
	// (eventdispatcher_tostring replaces it and expects the shim to run).
	Avm2Context* ctx = act->ctx;
	Avm2Object* oproto = ctx->builtins.object_class->prototype_obj;
	Avm2Value* ts = avm2_object_find_dynamic(oproto, "toString", 8);
	if (ts != NULL)
	{
		return avm2_call_value(ctx, *ts, act->this_val, NULL, 0);
	}
	return avm2_string(avm2_string_from_literal(ctx, "[object Object]"));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// flash.events.MouseEvent / KeyboardEvent / FocusEvent (Stage 8)
// ---------------------------------------------------------------------------

static double arg_num_default_nan(Avm2Activation* act, uint32_t i)
{
	if (i >= act->argc) return NAN;
	return avm2_coerce_to_number(act->ctx, act->args[i]);
}

static int arg_bool(Avm2Activation* act, uint32_t i)
{
	return i < act->argc && avm2_coerce_to_boolean(act->args[i]);
}

static int arg_bool_true_default(Avm2Activation* act, uint32_t i)
{
	// bubbles defaults to true for these subclasses.
	return i >= act->argc ? 1 : (avm2_coerce_to_boolean(act->args[i]) ? 1 : 0);
}

static uint32_t arg_u32(Avm2Activation* act, uint32_t i)
{
	return i < act->argc ? avm2_coerce_to_u32(act->ctx, act->args[i]) : 0;
}

static int32_t arg_i32(Avm2Activation* act, uint32_t i)
{
	return i < act->argc ? avm2_coerce_to_i32(act->ctx, act->args[i]) : 0;
}

static Avm2Object* arg_obj(Avm2Activation* act, uint32_t i)
{
	if (i < act->argc && act->args[i].kind == AVM2_VALUE_OBJECT)
		return act->args[i].u.obj;
	return NULL;
}

// MouseEvent(type, bubbles=true, cancelable=false, localX=NaN, localY=NaN,
//   relatedObject=null, ctrlKey=false, altKey=false, shiftKey=false,
//   buttonDown=false, delta=0, commandKey=false, clickCount=0)
static Avm2Value mouse_event_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool_true_default(act, 1);
	ext->cancelable = arg_bool(act, 2);
	ext->local_x = arg_num_default_nan(act, 3);
	ext->local_y = arg_num_default_nan(act, 4);
	ext->related_object = arg_obj(act, 5);
	ext->ctrl_key = arg_bool(act, 6);
	ext->alt_key = arg_bool(act, 7);
	ext->shift_key = arg_bool(act, 8);
	ext->button_down = arg_bool(act, 9);
	ext->delta = arg_i32(act, 10);
	ext->command_key = arg_bool(act, 11);
	return avm2_undefined();
}

// localX/localY are plain writable Numbers (Ruffle MouseEvent.as backs them
// with `_localX`/`_localY` slots and a get/set pair). They are NOT rounded:
// the twips snap happens only on the way through local_to_global for
// stageX/stageY, which is where the 0.05-px expected values come from.
static Avm2Value me_get_local_x(Avm2Activation* act)
{
	return avm2_number(this_event(act)->local_x);
}
static Avm2Value me_set_local_x(Avm2Activation* act)
{
	this_event(act)->local_x = arg_num_default_nan(act, 0);
	return avm2_undefined();
}

static Avm2Value me_get_local_y(Avm2Activation* act)
{
	return avm2_number(this_event(act)->local_y);
}
static Avm2Value me_set_local_y(Avm2Activation* act)
{
	this_event(act)->local_y = arg_num_default_nan(act, 0);
	return avm2_undefined();
}

static Avm2Value me_get_rel_inaccessible(Avm2Activation* act)
{ return avm2_bool(this_event(act)->related_object_inaccessible != 0); }
static Avm2Value me_set_rel_inaccessible(Avm2Activation* act)
{
	this_event(act)->related_object_inaccessible = arg_bool(act, 0);
	return avm2_undefined();
}

static Avm2Value me_get_related(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	return ext->related_object != NULL ? avm2_object_value(ext->related_object)
	                                   : avm2_null();
}

static Avm2Value me_set_related(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->related_object = arg_obj(act, 0);
	return avm2_undefined();
}

static Avm2Value me_get_ctrl(Avm2Activation* act)
{ return avm2_bool(this_event(act)->ctrl_key != 0); }
static Avm2Value me_set_ctrl(Avm2Activation* act)
{ this_event(act)->ctrl_key = arg_bool(act, 0); return avm2_undefined(); }
static Avm2Value me_get_alt(Avm2Activation* act)
{ return avm2_bool(this_event(act)->alt_key != 0); }
static Avm2Value me_set_alt(Avm2Activation* act)
{ this_event(act)->alt_key = arg_bool(act, 0); return avm2_undefined(); }
static Avm2Value me_get_shift(Avm2Activation* act)
{ return avm2_bool(this_event(act)->shift_key != 0); }
static Avm2Value me_set_shift(Avm2Activation* act)
{ this_event(act)->shift_key = arg_bool(act, 0); return avm2_undefined(); }
static Avm2Value me_get_button_down(Avm2Activation* act)
{ return avm2_bool(this_event(act)->button_down != 0); }
static Avm2Value me_set_button_down(Avm2Activation* act)
{ this_event(act)->button_down = arg_bool(act, 0); return avm2_undefined(); }
static Avm2Value me_get_delta(Avm2Activation* act)
{ return avm2_integer(this_event(act)->delta); }
static Avm2Value me_set_delta(Avm2Activation* act)
{ this_event(act)->delta = arg_i32(act, 0); return avm2_undefined(); }
static Avm2Value me_get_command(Avm2Activation* act)
{ return avm2_bool(this_event(act)->command_key != 0); }
static Avm2Value me_set_command(Avm2Activation* act)
{ this_event(act)->command_key = arg_bool(act, 0); return avm2_undefined(); }
static Avm2Value me_get_movement_x(Avm2Activation* act)
{ return avm2_number(this_event(act)->movement_x); }
static Avm2Value me_set_movement_x(Avm2Activation* act)
{ this_event(act)->movement_x = arg_i32(act, 0); return avm2_undefined(); }
static Avm2Value me_get_movement_y(Avm2Activation* act)
{ return avm2_number(this_event(act)->movement_y); }
static Avm2Value me_set_movement_y(Avm2Activation* act)
{ this_event(act)->movement_y = arg_i32(act, 0); return avm2_undefined(); }

// stageX/stageY (Ruffle mouse_event.rs::local_to_stage_{x,y}), three branches:
//   * either local coordinate NaN      -> that axis's local value, unmapped
//   * a display-object target          -> local_to_global through twips
//   * anything else (no target at all) -> `local * 0.0` — signed zero, NOT the
//     local value. A `new MouseEvent(...)` that was never dispatched therefore
//     reads stageX == stageY == 0 however large its localX/localY are.
extern int avm2_display_event_stage_coords(Avm2Context* ctx, Avm2Object* target,
                                           double lx, double ly,
                                           double* sx, double* sy);
static Avm2Value me_get_stage_x(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	double sx = ext->local_x, sy = ext->local_y;
	if (avm2_display_event_stage_coords(act->ctx, ext->target, ext->local_x,
	                                    ext->local_y, &sx, &sy))
		return avm2_number(sx);
	if (isnan(ext->local_x) || isnan(ext->local_y))
		return avm2_number(ext->local_x);
	return avm2_number(ext->local_x * 0.0);
}
static Avm2Value me_get_stage_y(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	double sx = ext->local_x, sy = ext->local_y;
	if (avm2_display_event_stage_coords(act->ctx, ext->target, ext->local_x,
	                                    ext->local_y, &sx, &sy))
		return avm2_number(sy);
	if (isnan(ext->local_x) || isnan(ext->local_y))
		return avm2_number(ext->local_y);
	return avm2_number(ext->local_y * 0.0);
}

static Avm2Value me_update_after_event(Avm2Activation* act)
{
	(void) act;
	return avm2_undefined();
}

static Avm2Value timer_event_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"TimerEvent", "type", "bubbles", "cancelable", "eventPhase"
	};
	Avm2Value args[5];
	for (int i = 0; i < 5; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 5);
}

static Avm2Value me_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"MouseEvent", "type", "bubbles", "cancelable", "eventPhase", "localX",
		"localY", "stageX", "stageY", "relatedObject", "ctrlKey", "altKey",
		"shiftKey", "buttonDown", "delta"
	};
	Avm2Value args[15];
	for (int i = 0; i < 15; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 15);
}

// KeyboardEvent(type, bubbles=true, cancelable=false, charCodeValue=0,
//   keyCodeValue=0, keyLocationValue=0, ctrlKeyValue=false, altKeyValue=false,
//   shiftKeyValue=false, controlKeyValue=false, commandKeyValue=false)
static Avm2Value keyboard_event_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool_true_default(act, 1);
	ext->cancelable = arg_bool(act, 2);
	ext->char_code = arg_u32(act, 3);
	ext->key_code = arg_u32(act, 4);
	ext->key_location = arg_u32(act, 5);
	ext->ctrl_key = arg_bool(act, 6);
	ext->alt_key = arg_bool(act, 7);
	ext->shift_key = arg_bool(act, 8);
	ext->control_key = arg_bool(act, 9);
	ext->command_key = arg_bool(act, 10);
	return avm2_undefined();
}

static Avm2Value ke_get_char_code(Avm2Activation* act)
{ return avm2_uint_value(this_event(act)->char_code); }
static Avm2Value ke_set_char_code(Avm2Activation* act)
{ this_event(act)->char_code = arg_u32(act, 0); return avm2_undefined(); }
static Avm2Value ke_get_key_code(Avm2Activation* act)
{ return avm2_uint_value(this_event(act)->key_code); }
static Avm2Value ke_set_key_code(Avm2Activation* act)
{ this_event(act)->key_code = arg_u32(act, 0); return avm2_undefined(); }
static Avm2Value ke_get_key_location(Avm2Activation* act)
{ return avm2_uint_value(this_event(act)->key_location); }
static Avm2Value ke_set_key_location(Avm2Activation* act)
{ this_event(act)->key_location = arg_u32(act, 0); return avm2_undefined(); }
static Avm2Value ke_get_control(Avm2Activation* act)
{ return avm2_bool(this_event(act)->control_key != 0); }
static Avm2Value ke_set_control(Avm2Activation* act)
{ this_event(act)->control_key = arg_bool(act, 0); return avm2_undefined(); }

static Avm2Value ke_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"KeyboardEvent", "type", "bubbles", "cancelable", "eventPhase",
		"charCode", "keyCode", "keyLocation", "ctrlKey", "altKey", "shiftKey"
	};
	Avm2Value args[11];
	for (int i = 0; i < 11; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 11);
}

// FocusEvent(type, bubbles=true, cancelable=false, relatedObject=null,
//   shiftKey=false, keyCode=0, direction="none", isRelatedObjectInaccessible=false)
static Avm2Value focus_event_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool_true_default(act, 1);
	ext->cancelable = arg_bool(act, 2);
	ext->related_object = arg_obj(act, 3);
	ext->shift_key = arg_bool(act, 4);
	ext->key_code = arg_u32(act, 5);
	ext->direction = act->argc > 6
		? avm2_coerce_to_string(act->ctx, act->args[6]) : NULL;
	ext->related_object_inaccessible = arg_bool(act, 7);
	return avm2_undefined();
}

static Avm2Value fe_get_related(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	return ext->related_object != NULL ? avm2_object_value(ext->related_object)
	                                   : avm2_null();
}
static Avm2Value fe_set_related(Avm2Activation* act)
{ this_event(act)->related_object = arg_obj(act, 0); return avm2_undefined(); }
static Avm2Value fe_get_shift(Avm2Activation* act)
{ return avm2_bool(this_event(act)->shift_key != 0); }
static Avm2Value fe_set_shift(Avm2Activation* act)
{ this_event(act)->shift_key = arg_bool(act, 0); return avm2_undefined(); }
static Avm2Value fe_get_key_code(Avm2Activation* act)
{ return avm2_uint_value(this_event(act)->key_code); }
static Avm2Value fe_set_key_code(Avm2Activation* act)
{ this_event(act)->key_code = arg_u32(act, 0); return avm2_undefined(); }
static Avm2Value fe_get_direction(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	return avm2_string(ext->direction != NULL ? ext->direction
		: avm2_string_from_literal(act->ctx, "none"));
}
static Avm2Value fe_set_direction(Avm2Activation* act)
{
	this_event(act)->direction = act->argc > 0
		? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	return avm2_undefined();
}
static Avm2Value fe_get_rel_inaccessible(Avm2Activation* act)
{ return avm2_bool(this_event(act)->related_object_inaccessible != 0); }
static Avm2Value fe_set_rel_inaccessible(Avm2Activation* act)
{ this_event(act)->related_object_inaccessible = arg_bool(act, 0); return avm2_undefined(); }

static Avm2Value fe_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"FocusEvent", "type", "bubbles", "cancelable", "eventPhase",
		"relatedObject", "shiftKey", "keyCode"
	};
	Avm2Value args[8];
	for (int i = 0; i < 8; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 8);
}

// --- flash.events.ContextMenuEvent -----------------------------------------
//
// Ruffle ContextMenuEvent.as. Headless never raises one, but any movie that
// builds a right-click menu getlexes the class (away3d's SimpleGUI does, and a
// missing class is a #1065 that kills the whole script). `mouseTarget` shares
// the ext's `related_object` slot — MouseEvent's relatedObject is the same
// "InteractiveObject under the pointer" — and `contextMenuOwner` has its own.
static Avm2Value cme_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool(act, 1);
	ext->cancelable = arg_bool(act, 2);
	ext->related_object = arg_obj(act, 3);
	ext->context_menu_owner = arg_obj(act, 4);
	return avm2_undefined();
}

static Avm2Value cme_get_owner(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	return ext->context_menu_owner != NULL
		? avm2_object_value(ext->context_menu_owner) : avm2_null();
}
static Avm2Value cme_set_owner(Avm2Activation* act)
{ this_event(act)->context_menu_owner = arg_obj(act, 0); return avm2_undefined(); }

static Avm2Value cme_get_mouse_target_inaccessible(Avm2Activation* act)
{ return avm2_bool(this_event(act)->related_object_inaccessible != 0); }
static Avm2Value cme_set_mouse_target_inaccessible(Avm2Activation* act)
{
	this_event(act)->related_object_inaccessible = arg_bool(act, 0);
	return avm2_undefined();
}

static Avm2Value cme_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"ContextMenuEvent", "type", "bubbles", "cancelable", "eventPhase",
		"mouseTarget", "contextMenuOwner"
	};
	Avm2Value args[7];
	for (int i = 0; i < 7; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 7);
}

static Avm2Class* g_context_menu_event_class;

static Avm2Value cme_clone(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2EventExt* ext = this_event(act);
	Avm2Value args[5];
	args[0] = avm2_string(ext->type != NULL ? ext->type
		: avm2_string_from_literal(ctx, ""));
	args[1] = avm2_bool(ext->bubbles != 0);
	args[2] = avm2_bool(ext->cancelable != 0);
	args[3] = ext->related_object != NULL
		? avm2_object_value(ext->related_object) : avm2_null();
	args[4] = ext->context_menu_owner != NULL
		? avm2_object_value(ext->context_menu_owner) : avm2_null();
	return avm2_class_construct(ctx, g_context_menu_event_class, args, 5);
}

Avm2Object* avm2_mouse_event_new(Avm2Context* ctx, const Avm2String* type,
                                 int bubbles, int cancelable, double local_x,
                                 double local_y, Avm2Object* related,
                                 int shift, int ctrl, int alt, int button_down,
                                 int32_t delta)
{
	Avm2Value args[11];
	args[0] = avm2_string(type);
	args[1] = avm2_bool(bubbles != 0);
	args[2] = avm2_bool(cancelable != 0);
	args[3] = avm2_number(local_x);
	args[4] = avm2_number(local_y);
	args[5] = related != NULL ? avm2_object_value(related) : avm2_null();
	args[6] = avm2_bool(ctrl != 0);
	args[7] = avm2_bool(alt != 0);
	args[8] = avm2_bool(shift != 0);
	args[9] = avm2_bool(button_down != 0);
	args[10] = avm2_integer(delta);
	Avm2Value v = avm2_class_construct(ctx, ctx->builtins.mouse_event_class, args, 11);
	return v.u.obj;
}

Avm2Object* avm2_keyboard_event_new(Avm2Context* ctx, const Avm2String* type,
                                    int bubbles, int cancelable,
                                    uint32_t char_code, uint32_t key_code,
                                    uint32_t key_location, int ctrl, int alt,
                                    int shift)
{
	Avm2Value args[9];
	args[0] = avm2_string(type);
	args[1] = avm2_bool(bubbles != 0);
	args[2] = avm2_bool(cancelable != 0);
	args[3] = avm2_uint_value(char_code);
	args[4] = avm2_uint_value(key_code);
	args[5] = avm2_uint_value(key_location);
	args[6] = avm2_bool(ctrl != 0);
	args[7] = avm2_bool(alt != 0);
	args[8] = avm2_bool(shift != 0);
	Avm2Value v = avm2_class_construct(ctx, ctx->builtins.keyboard_event_class,
	                                   args, 9);
	return v.u.obj;
}

Avm2Object* avm2_focus_event_new(Avm2Context* ctx, const Avm2String* type,
                                 int bubbles, int cancelable,
                                 Avm2Object* related, int shift,
                                 uint32_t key_code, const char* direction)
{
	Avm2Value args[7];
	args[0] = avm2_string(type);
	args[1] = avm2_bool(bubbles != 0);
	args[2] = avm2_bool(cancelable != 0);
	args[3] = related != NULL ? avm2_object_value(related) : avm2_null();
	args[4] = avm2_bool(shift != 0);
	args[5] = avm2_uint_value(key_code);
	args[6] = avm2_string(avm2_string_from_literal(ctx,
		direction != NULL ? direction : "none"));
	Avm2Value v = avm2_class_construct(ctx, ctx->builtins.focus_event_class,
	                                   args, 7);
	return v.u.obj;
}

// flash.events.TextEvent(type, bubbles=false, cancelable=false, text="")
static Avm2Value text_event_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool(act, 1);
	ext->cancelable = arg_bool(act, 2);
	ext->text = act->argc > 3
		? avm2_coerce_to_string(act->ctx, act->args[3])
		: avm2_string_from_literal(act->ctx, "");
	return avm2_undefined();
}

static Avm2Value te_get_text(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	return avm2_string(ext->text != NULL ? ext->text
		: avm2_string_from_literal(act->ctx, ""));
}
static Avm2Value te_set_text(Avm2Activation* act)
{
	this_event(act)->text = act->argc > 0
		? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	return avm2_undefined();
}

static Avm2Value te_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"TextEvent", "type", "bubbles", "cancelable", "eventPhase", "text"
	};
	Avm2Value args[6];
	for (int i = 0; i < 6; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 6);
}

// flash.events.DataEvent extends TextEvent; `data` is TextEvent's `text`
// under another name (Ruffle's DataEvent.as delegates both accessors to
// super). Dispatched by XMLSocket for each NUL-delimited inbound frame.
static Avm2Value de_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"DataEvent", "type", "bubbles", "cancelable", "eventPhase", "data"
	};
	Avm2Value args[6];
	for (int i = 0; i < 6; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 6);
}

static Avm2Class* g_data_event_class;

Avm2Object* avm2_data_event_new(Avm2Context* ctx, const Avm2String* type,
                                int bubbles, int cancelable, const Avm2String* data)
{
	if (g_data_event_class == NULL) return NULL;
	Avm2Value args[4];
	args[0] = avm2_string(type);
	args[1] = avm2_bool(bubbles != 0);
	args[2] = avm2_bool(cancelable != 0);
	args[3] = avm2_string(data != NULL ? data : avm2_string_from_literal(ctx, ""));
	Avm2Value v = avm2_class_construct(ctx, g_data_event_class, args, 4);
	return v.kind == AVM2_VALUE_OBJECT ? v.u.obj : NULL;
}

Avm2Object* avm2_text_event_new(Avm2Context* ctx, const Avm2String* type,
                                int bubbles, int cancelable, const Avm2String* text)
{
	Avm2Value args[4];
	args[0] = avm2_string(type);
	args[1] = avm2_bool(bubbles != 0);
	args[2] = avm2_bool(cancelable != 0);
	args[3] = avm2_string(text != NULL ? text : avm2_string_from_literal(ctx, ""));
	Avm2Value v = avm2_class_construct(ctx, ctx->builtins.text_event_class, args, 4);
	return v.u.obj;
}

int avm2_event_is_cancelled(Avm2Object* event)
{
	if (event == NULL || event->native_ext == NULL) return 0;
	return ((Avm2EventExt*) event->native_ext)->cancelled != 0;
}

// Replace an inherited public method in-place (the ivtable copies the super's
// entries, and avm2_vtable_find returns the FIRST match, so a plain append
// would be shadowed by the parent's method — e.g. Event.toString).
static void event_override_method(Avm2Context* ctx, Avm2Class* cls,
                                  const char* name, Avm2MethodFn fn)
{
	uint32_t nlen = (uint32_t) strlen(name);
	for (uint32_t i = 0; i < cls->ivtable.count; i++)
	{
		Avm2PropEntry* e = &cls->ivtable.entries[i];
		if (e->key.name_len == nlen && e->key.ns_len == 0
		    && memcmp(e->key.name, name, nlen) == 0
		    && e->kind == AVM2_PROP_METHOD)
		{
			e->method.fn = fn;
			e->method.debug_name = name;
			e->defining_class = cls;
			return;
		}
	}
	avm2_builtin_add_method(ctx, cls, name, fn);
}

// --- ProgressEvent / ErrorEvent family / HTTPStatusEvent / StatusEvent ---
// These flash.events subclasses are dispatched on URLLoader/Loader/Sound
// during network I/O. The native/headless runtime has no network layer, so
// they are rarely dispatched, but the classes must exist (and expose their
// static event-type constants) — the Newgrounds-API preloader wraps Seedling
// and registers ProgressEvent/IOErrorEvent/SecurityErrorEvent listeners.

static Avm2Value progress_event_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool(act, 1);
	ext->cancelable = arg_bool(act, 2);
	ext->bytes_loaded = act->argc > 3 ? avm2_coerce_to_number(act->ctx, act->args[3]) : 0;
	ext->bytes_total = act->argc > 4 ? avm2_coerce_to_number(act->ctx, act->args[4]) : 0;
	return avm2_undefined();
}
static Avm2Value pe_get_loaded(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); return avm2_number(e ? e->bytes_loaded : 0); }
static Avm2Value pe_set_loaded(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); if (e && act->argc > 0) e->bytes_loaded = avm2_coerce_to_number(act->ctx, act->args[0]); return avm2_undefined(); }
static Avm2Value pe_get_total(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); return avm2_number(e ? e->bytes_total : 0); }
static Avm2Value pe_set_total(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); if (e && act->argc > 0) e->bytes_total = avm2_coerce_to_number(act->ctx, act->args[0]); return avm2_undefined(); }

// ProgressEvent.as / ErrorEvent.as toString(): each subclass names itself and
// adds its own fields to Event's four. ErrorEvent and friends inherit
// TextEvent's toString otherwise and would print "[TextEvent ...]".
static Avm2Value pe_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"ProgressEvent", "type", "bubbles", "cancelable", "eventPhase",
		"bytesLoaded", "bytesTotal"
	};
	Avm2Value args[7];
	for (int i = 0; i < 7; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 7);
}

static Avm2Value error_event_to_string_named(Avm2Activation* act,
                                             const char* cname)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"type", "bubbles", "cancelable", "eventPhase", "text"
	};
	Avm2Value args[6];
	args[0] = avm2_string(avm2_string_from_literal(ctx, cname));
	for (int i = 0; i < 5; i++)
		args[i + 1] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 6);
}
static Avm2Value ee_to_string(Avm2Activation* act)
{ return error_event_to_string_named(act, "ErrorEvent"); }
static Avm2Value ioe_to_string(Avm2Activation* act)
{ return error_event_to_string_named(act, "IOErrorEvent"); }
static Avm2Value sece_to_string(Avm2Activation* act)
{ return error_event_to_string_named(act, "SecurityErrorEvent"); }
static Avm2Value asye_to_string(Avm2Activation* act)
{ return error_event_to_string_named(act, "AsyncErrorEvent"); }

static Avm2Value error_event_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool(act, 1);
	ext->cancelable = arg_bool(act, 2);
	ext->text = act->argc > 3 ? avm2_coerce_to_string(act->ctx, act->args[3])
		: avm2_string_from_literal(act->ctx, "");
	ext->error_id = act->argc > 4 ? (int32_t) avm2_coerce_to_number(act->ctx, act->args[4]) : 0;
	return avm2_undefined();
}
static Avm2Value ee_get_error_id(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); return avm2_integer(e ? e->error_id : 0); }

// AsyncErrorEvent(type, bubbles, cancelable, text, error:Error = null): the 5th
// argument is an Error OBJECT where its ErrorEvent base takes an errorID, so it
// needs its own init. from_shumway/localconnection traces `event.error`.
static Avm2Value async_error_event_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool(act, 1);
	ext->cancelable = arg_bool(act, 2);
	ext->text = act->argc > 3 ? avm2_coerce_to_string(act->ctx, act->args[3])
		: avm2_string_from_literal(act->ctx, "");
	ext->error_obj = (act->argc > 4 && act->args[4].kind == AVM2_VALUE_OBJECT)
		? act->args[4].u.obj : NULL;
	return avm2_undefined();
}
static Avm2Value asye_get_error(Avm2Activation* act)
{
	Avm2EventExt* e = this_event(act);
	return (e != NULL && e->error_obj != NULL)
		? avm2_object_value(e->error_obj) : avm2_null();
}

static Avm2Value http_status_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool(act, 1);
	ext->cancelable = arg_bool(act, 2);
	ext->http_status = act->argc > 3 ? (int32_t) avm2_coerce_to_number(act->ctx, act->args[3]) : 0;
	ext->response_url = NULL;
	return avm2_undefined();
}
static Avm2Value hse_get_status(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); return avm2_integer(e ? e->http_status : 0); }
static Avm2Value hse_get_redirected(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); return avm2_bool(e && e->redirected); }
static Avm2Value hse_set_redirected(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); if (e && act->argc > 0) e->redirected = avm2_coerce_to_boolean(act->args[0]) ? 1 : 0; return avm2_undefined(); }
static Avm2Value hse_get_response_url(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); return (e && e->response_url) ? avm2_string(e->response_url) : avm2_null(); }

// HTTPStatusEvent.as toString() — url_loader traces the whole event, so the
// three extra fields must be there or it prints Event's four alone.
static Avm2Value hse_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"HTTPStatusEvent", "type", "bubbles", "cancelable", "eventPhase",
		"status", "redirected", "responseURL"
	};
	Avm2Value args[8];
	for (int i = 0; i < 8; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 8);
}

static Avm2Value status_event_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool(act, 1);
	ext->cancelable = arg_bool(act, 2);
	// `code` and `level` are String-typed, so null/undefined coerce to NULL —
	// not to the string "null". localconnection_send traces `code=null`.
	ext->status_code = (act->argc > 3 && act->args[3].kind != AVM2_VALUE_NULL
	                    && act->args[3].kind != AVM2_VALUE_UNDEFINED)
		? avm2_coerce_to_string(act->ctx, act->args[3]) : NULL;
	ext->status_level = (act->argc > 4 && act->args[4].kind != AVM2_VALUE_NULL
	                     && act->args[4].kind != AVM2_VALUE_UNDEFINED)
		? avm2_coerce_to_string(act->ctx, act->args[4]) : NULL;
	return avm2_undefined();
}

// StatusEvent.as toString() — localconnection_send traces the whole event, so
// the two extra fields must be there or it prints Event's four alone.
static Avm2Value ste_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"StatusEvent", "type", "bubbles", "cancelable", "eventPhase",
		"code", "level"
	};
	Avm2Value args[7];
	for (int i = 0; i < 7; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 7);
}
static Avm2Value se_get_code(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); return (e && e->status_code) ? avm2_string(e->status_code) : avm2_null(); }
static Avm2Value se_set_code(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); if (e && act->argc > 0) e->status_code = avm2_coerce_to_string(act->ctx, act->args[0]); return avm2_undefined(); }
static Avm2Value se_get_level(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); return (e && e->status_level) ? avm2_string(e->status_level) : avm2_null(); }
static Avm2Value se_set_level(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); if (e && act->argc > 0) e->status_level = avm2_coerce_to_string(act->ctx, act->args[0]); return avm2_undefined(); }

// flash.events.NetStatusEvent — Event plus one Object field. Its `info` bag is
// the whole payload of the NetConnection/NetStream status protocol, and
// netconnection_close grades both the field values and the dispatch order.
static Avm2Value net_status_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool(act, 1);
	ext->cancelable = arg_bool(act, 2);
	ext->info = (act->argc > 3 && act->args[3].kind == AVM2_VALUE_OBJECT)
		? act->args[3].u.obj : NULL;
	return avm2_undefined();
}
static Avm2Value nse_get_info(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act); return (e && e->info) ? avm2_object_value(e->info) : avm2_null(); }
static Avm2Value nse_set_info(Avm2Activation* act)
{
	Avm2EventExt* e = this_event(act);
	if (e && act->argc > 0)
		e->info = (act->args[0].kind == AVM2_VALUE_OBJECT) ? act->args[0].u.obj : NULL;
	return avm2_undefined();
}
static Avm2Value nse_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"NetStatusEvent", "type", "bubbles", "cancelable", "eventPhase", "info"
	};
	Avm2Value args[6];
	for (int i = 0; i < 6; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 6);
}

static void sconst(Avm2Context* ctx, Avm2Class* cls, const char* n, const char* v)
{ avm2_builtin_add_static_const(ctx, cls, n, avm2_string(avm2_string_from_literal(ctx, v))); }

// flash.events.NetFilterEvent — Event plus two ByteArray public VARS, held by
// identity (netfilterevent grades `clone.header === a`). They live in
// dont-enum dynamic props rather than the shared Avm2EventExt so the GC traces
// them for free and no other event pays for the two extra fields.
static Avm2Class* g_net_filter_event_class;

static Avm2Value nfe_field(Avm2Activation* act, const char* n, uint32_t nlen)
{ return avm2_get_public_property(act->ctx, act->this_val, n, nlen, NULL); }

static void nfe_put(Avm2Activation* act, const char* n, uint32_t nlen,
                    Avm2Value v)
{
	if (act->this_val.kind != AVM2_VALUE_OBJECT) return;
	avm2_object_set_dynamic(act->ctx, act->this_val.u.obj, n, nlen,
	                        v.kind == AVM2_VALUE_OBJECT ? v : avm2_null())
		->dont_enum = 1;
}

static Avm2Value net_filter_init(Avm2Activation* act)
{
	Avm2EventExt* ext = this_event(act);
	ext->type = act->argc > 0 ? avm2_coerce_to_string(act->ctx, act->args[0]) : NULL;
	ext->bubbles = arg_bool(act, 1);
	ext->cancelable = arg_bool(act, 2);
	nfe_put(act, "_header", 7, act->argc > 3 ? act->args[3] : avm2_null());
	nfe_put(act, "_data", 5, act->argc > 4 ? act->args[4] : avm2_null());
	return avm2_undefined();
}

static Avm2Value nfe_get_header(Avm2Activation* act)
{ return nfe_field(act, "_header", 7); }
static Avm2Value nfe_set_header(Avm2Activation* act)
{
	if (act->argc > 0) nfe_put(act, "_header", 7, act->args[0]);
	return avm2_undefined();
}
static Avm2Value nfe_get_data(Avm2Activation* act)
{ return nfe_field(act, "_data", 5); }
static Avm2Value nfe_set_data(Avm2Activation* act)
{
	if (act->argc > 0) nfe_put(act, "_data", 5, act->args[0]);
	return avm2_undefined();
}

static Avm2Value nfe_clone(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2EventExt* ext = this_event(act);
	Avm2Value args[5];
	args[0] = avm2_string(ext->type != NULL ? ext->type
		: avm2_string_from_literal(ctx, ""));
	args[1] = avm2_bool(ext->bubbles != 0);
	args[2] = avm2_bool(ext->cancelable != 0);
	args[3] = nfe_field(act, "_header", 7);
	args[4] = nfe_field(act, "_data", 5);
	return avm2_class_construct(ctx, g_net_filter_event_class, args, 5);
}

static Avm2Value nfe_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	// The class name in the rendered string really is NetTransformEvent —
	// an FP naming leftover that Ruffle replicates and this test pins.
	static const char* const fields[] = {
		"NetTransformEvent", "type", "bubbles", "cancelable", "eventPhase",
		"header", "data"
	};
	Avm2Value args[7];
	for (int i = 0; i < 7; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 7);
}

// Cached for the C-side constructors below (avm2_progress_event_new /
// avm2_io_error_event_new), which the Loader pipeline dispatches from.
static Avm2Class* g_progress_event_class;
static Avm2Class* g_io_error_event_class;
static Avm2Class* g_status_event_class;
static Avm2Class* g_async_error_event_class;
static Avm2Class* g_http_status_event_class;
static Avm2Class* g_net_status_event_class;

static void register_net_events(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;

	// flash.events.ProgressEvent (extends Event).
	Avm2Class* pe = avm2_builtin_class(ctx, "flash.events", "ProgressEvent",
	                                   b->event_class);
	pe->instance_init.fn = progress_event_init;
	pe->instance_init.debug_name = "ProgressEvent";
	g_progress_event_class = pe;
	avm2_builtin_add_getset(ctx, pe, "bytesLoaded", pe_get_loaded, pe_set_loaded);
	avm2_builtin_add_getset(ctx, pe, "bytesTotal", pe_get_total, pe_set_total);
	event_override_method(ctx, pe, "toString", pe_to_string);
	sconst(ctx, pe, "PROGRESS", "progress");
	sconst(ctx, pe, "SOCKET_DATA", "socketData");

	// flash.events.ErrorEvent (extends TextEvent) + IOErrorEvent /
	// SecurityErrorEvent / AsyncErrorEvent / UncaughtErrorEvent (extend it).
	Avm2Class* er = avm2_builtin_class(ctx, "flash.events", "ErrorEvent",
	                                   b->text_event_class);
	er->instance_init.fn = error_event_init;
	er->instance_init.debug_name = "ErrorEvent";
	b->error_event_class = er;
	avm2_builtin_add_getter(ctx, er, "errorID", ee_get_error_id);
	event_override_method(ctx, er, "toString", ee_to_string);
	sconst(ctx, er, "ERROR", "error");

	Avm2Class* ioe = avm2_builtin_class(ctx, "flash.events", "IOErrorEvent", er);
	ioe->instance_init.fn = error_event_init;
	ioe->instance_init.debug_name = "IOErrorEvent";
	g_io_error_event_class = ioe;
	event_override_method(ctx, ioe, "toString", ioe_to_string);
	sconst(ctx, ioe, "IO_ERROR", "ioError");
	sconst(ctx, ioe, "NETWORK_ERROR", "networkError");
	sconst(ctx, ioe, "DISK_ERROR", "diskError");
	sconst(ctx, ioe, "VERIFY_ERROR", "verifyError");
	sconst(ctx, ioe, "STANDARD_ERROR_IO_ERROR", "ioError");

	Avm2Class* sec = avm2_builtin_class(ctx, "flash.events",
	                                    "SecurityErrorEvent", er);
	sec->instance_init.fn = error_event_init;
	sec->instance_init.debug_name = "SecurityErrorEvent";
	event_override_method(ctx, sec, "toString", sece_to_string);
	sconst(ctx, sec, "SECURITY_ERROR", "securityError");

	Avm2Class* ase = avm2_builtin_class(ctx, "flash.events",
	                                    "AsyncErrorEvent", er);
	ase->instance_init.fn = async_error_event_init;
	ase->instance_init.debug_name = "AsyncErrorEvent";
	event_override_method(ctx, ase, "toString", asye_to_string);
	avm2_builtin_add_getter(ctx, ase, "error", asye_get_error);
	sconst(ctx, ase, "ASYNC_ERROR", "asyncError");
	g_async_error_event_class = ase;

	// flash.events.HTTPStatusEvent (extends Event).
	Avm2Class* hse = avm2_builtin_class(ctx, "flash.events", "HTTPStatusEvent",
	                                    b->event_class);
	hse->instance_init.fn = http_status_init;
	hse->instance_init.debug_name = "HTTPStatusEvent";
	avm2_builtin_add_getter(ctx, hse, "status", hse_get_status);
	avm2_builtin_add_getset(ctx, hse, "redirected", hse_get_redirected, hse_set_redirected);
	avm2_builtin_add_getter(ctx, hse, "responseURL", hse_get_response_url);
	event_override_method(ctx, hse, "toString", hse_to_string);
	g_http_status_event_class = hse;
	sconst(ctx, hse, "HTTP_STATUS", "httpStatus");
	sconst(ctx, hse, "HTTP_RESPONSE_STATUS", "httpResponseStatus");

	// flash.events.StatusEvent (extends Event).
	Avm2Class* ste = avm2_builtin_class(ctx, "flash.events", "StatusEvent",
	                                    b->event_class);
	ste->instance_init.fn = status_event_init;
	ste->instance_init.debug_name = "StatusEvent";
	avm2_builtin_add_getset(ctx, ste, "code", se_get_code, se_set_code);
	avm2_builtin_add_getset(ctx, ste, "level", se_get_level, se_set_level);
	event_override_method(ctx, ste, "toString", ste_to_string);
	sconst(ctx, ste, "STATUS", "status");
	g_status_event_class = ste;

	// flash.events.NetStatusEvent (extends Event).
	Avm2Class* nse = avm2_builtin_class(ctx, "flash.events", "NetStatusEvent",
	                                    b->event_class);
	nse->instance_init.fn = net_status_init;
	nse->instance_init.debug_name = "NetStatusEvent";
	g_net_status_event_class = nse;
	avm2_builtin_add_getset(ctx, nse, "info", nse_get_info, nse_set_info);
	event_override_method(ctx, nse, "toString", nse_to_string);
	sconst(ctx, nse, "NET_STATUS", "netStatus");

	// flash.events.NetFilterEvent (extends Event). No static const of its own
	// upstream — the type string is whatever the RTMP filter chain passes.
	Avm2Class* nfe = avm2_builtin_class(ctx, "flash.events", "NetFilterEvent",
	                                    b->event_class);
	nfe->instance_init.fn = net_filter_init;
	nfe->instance_init.debug_name = "NetFilterEvent";
	g_net_filter_event_class = nfe;
	avm2_builtin_add_getset(ctx, nfe, "header", nfe_get_header, nfe_set_header);
	avm2_builtin_add_getset(ctx, nfe, "data", nfe_get_data, nfe_set_data);
	event_override_method(ctx, nfe, "clone", nfe_clone);
	event_override_method(ctx, nfe, "toString", nfe_to_string);
}

// The Loader pipeline (avm2_display.c) dispatches these from C; both events
// are non-bubbling and non-cancelable in Ruffle's loader.
Avm2Object* avm2_progress_event_new(Avm2Context* ctx, const Avm2String* type,
                                    double bytes_loaded, double bytes_total)
{
	if (g_progress_event_class == NULL) return NULL;
	Avm2Value args[5];
	args[0] = avm2_string(type);
	args[1] = avm2_bool(0);
	args[2] = avm2_bool(0);
	args[3] = avm2_number(bytes_loaded);
	args[4] = avm2_number(bytes_total);
	Avm2Value v = avm2_class_construct(ctx, g_progress_event_class, args, 5);
	return v.kind == AVM2_VALUE_OBJECT ? v.u.obj : NULL;
}

// NetStatusEvent built from a NUL-terminated key/value list, mirroring Ruffle's
// EventObject::net_status_event: the `info` bag is a plain dynamic Object whose
// properties are set in the order given (netconnection_close sorts them itself,
// but netstream_connect reads them straight back).
Avm2Object* avm2_net_status_event_new(Avm2Context* ctx,
                                      const char* const* keys,
                                      const char* const* values, int count)
{
	if (g_net_status_event_class == NULL) return NULL;
	Avm2Value info = avm2_class_construct(ctx, ctx->builtins.object_class, NULL, 0);
	for (int i = 0; i < count; i++)
	{
		avm2_set_public_property(ctx, info, keys[i], (uint32_t) strlen(keys[i]),
			avm2_string(avm2_string_from_literal(ctx, values[i])));
	}
	Avm2Value args[4];
	args[0] = avm2_string(avm2_string_from_literal(ctx, "netStatus"));
	args[1] = avm2_bool(0);
	args[2] = avm2_bool(0);
	args[3] = info;
	Avm2Value v = avm2_class_construct(ctx, g_net_status_event_class, args, 4);
	return v.kind == AVM2_VALUE_OBJECT ? v.u.obj : NULL;
}

// StatusEvent / AsyncErrorEvent built from C by LocalConnection delivery
// (avm2_net.c), matching Ruffle's LocalConnectionObject::{send_status,run_method}
// argument lists exactly: both events are non-bubbling and non-cancelable, the
// status event's `code` is null, and the async error carries the thrown Error.
Avm2Object* avm2_status_event_new(Avm2Context* ctx, const Avm2String* code,
                                  const char* level)
{
	if (g_status_event_class == NULL) return NULL;
	Avm2Value args[5];
	args[0] = avm2_string(avm2_string_from_literal(ctx, "status"));
	args[1] = avm2_bool(0);
	args[2] = avm2_bool(0);
	args[3] = (code != NULL) ? avm2_string(code) : avm2_null();
	args[4] = avm2_string(avm2_string_from_literal(ctx, level));
	Avm2Value v = avm2_class_construct(ctx, g_status_event_class, args, 5);
	return v.kind == AVM2_VALUE_OBJECT ? v.u.obj : NULL;
}

Avm2Object* avm2_async_error_event_new(Avm2Context* ctx, const Avm2String* text,
                                       Avm2Object* error_obj)
{
	if (g_async_error_event_class == NULL) return NULL;
	Avm2Value args[5];
	args[0] = avm2_string(avm2_string_from_literal(ctx, "asyncError"));
	args[1] = avm2_bool(0);
	args[2] = avm2_bool(0);
	args[3] = avm2_string(text);
	args[4] = (error_obj != NULL) ? avm2_object_value(error_obj) : avm2_null();
	Avm2Value v = avm2_class_construct(ctx, g_async_error_event_class, args, 5);
	return v.kind == AVM2_VALUE_OBJECT ? v.u.obj : NULL;
}

Avm2Object* avm2_io_error_event_new(Avm2Context* ctx, const Avm2String* type,
                                    const Avm2String* text, int32_t error_id)
{
	if (g_io_error_event_class == NULL) return NULL;
	Avm2Value args[5];
	args[0] = avm2_string(type);
	args[1] = avm2_bool(0);
	args[2] = avm2_bool(0);
	args[3] = avm2_string(text);
	args[4] = avm2_integer(error_id);
	Avm2Value v = avm2_class_construct(ctx, g_io_error_event_class, args, 5);
	return v.kind == AVM2_VALUE_OBJECT ? v.u.obj : NULL;
}

Avm2Object* avm2_http_status_event_new(Avm2Context* ctx, const Avm2String* type,
                                      int32_t status, int redirected)
{
	if (g_http_status_event_class == NULL) return NULL;
	Avm2Value args[4];
	args[0] = avm2_string(type);
	args[1] = avm2_bool(0);
	args[2] = avm2_bool(0);
	args[3] = avm2_integer(status);
	Avm2Value v = avm2_class_construct(ctx, g_http_status_event_class, args, 4);
	if (v.kind != AVM2_VALUE_OBJECT) return NULL;
	// `redirected` is not a ctor parameter (Ruffle sets it after construction).
	Avm2EventExt* ext = (Avm2EventExt*) v.u.obj->native_ext;
	if (ext != NULL) ext->redirected = redirected ? 1 : 0;
	return v.u.obj;
}

static void register_input_events(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;

	// flash.events.TextEvent.
	Avm2Class* te = avm2_builtin_class(ctx, "flash.events", "TextEvent",
	                                   b->event_class);
	te->instance_init.fn = text_event_init;
	te->instance_init.debug_name = "TextEvent";
	b->text_event_class = te;
	avm2_builtin_add_getset(ctx, te, "text", te_get_text, te_set_text);
	event_override_method(ctx, te, "toString", te_to_string);
	avm2_builtin_add_static_const(ctx, te, "LINK",
		avm2_string(avm2_string_from_literal(ctx, "link")));
	avm2_builtin_add_static_const(ctx, te, "TEXT_INPUT",
		avm2_string(avm2_string_from_literal(ctx, "textInput")));

	// flash.events.DataEvent (extends TextEvent) — XMLSocket's inbound frames.
	Avm2Class* de = avm2_builtin_class(ctx, "flash.events", "DataEvent", te);
	de->instance_init.fn = text_event_init;
	de->instance_init.debug_name = "DataEvent";
	g_data_event_class = de;
	avm2_builtin_add_getset(ctx, de, "data", te_get_text, te_set_text);
	event_override_method(ctx, de, "toString", de_to_string);
	avm2_builtin_add_static_const(ctx, de, "DATA",
		avm2_string(avm2_string_from_literal(ctx, "data")));
	avm2_builtin_add_static_const(ctx, de, "UPLOAD_COMPLETE_DATA",
		avm2_string(avm2_string_from_literal(ctx, "uploadCompleteData")));

	// flash.events.MouseEvent.
	Avm2Class* me = avm2_builtin_class(ctx, "flash.events", "MouseEvent",
	                                   b->event_class);
	me->instance_init.fn = mouse_event_init;
	me->instance_init.debug_name = "MouseEvent";
	b->mouse_event_class = me;
	avm2_builtin_add_getset(ctx, me, "localX", me_get_local_x, me_set_local_x);
	avm2_builtin_add_getset(ctx, me, "localY", me_get_local_y, me_set_local_y);
	avm2_builtin_add_getset(ctx, me, "isRelatedObjectInaccessible",
	                        me_get_rel_inaccessible, me_set_rel_inaccessible);
	avm2_builtin_add_getter(ctx, me, "stageX", me_get_stage_x);
	avm2_builtin_add_getter(ctx, me, "stageY", me_get_stage_y);
	avm2_builtin_add_getset(ctx, me, "relatedObject", me_get_related, me_set_related);
	avm2_builtin_add_getset(ctx, me, "ctrlKey", me_get_ctrl, me_set_ctrl);
	avm2_builtin_add_getset(ctx, me, "altKey", me_get_alt, me_set_alt);
	avm2_builtin_add_getset(ctx, me, "shiftKey", me_get_shift, me_set_shift);
	avm2_builtin_add_getset(ctx, me, "buttonDown", me_get_button_down, me_set_button_down);
	avm2_builtin_add_getset(ctx, me, "delta", me_get_delta, me_set_delta);
	avm2_builtin_add_getset(ctx, me, "commandKey", me_get_command, me_set_command);
	avm2_builtin_add_getset(ctx, me, "movementX", me_get_movement_x, me_set_movement_x);
	avm2_builtin_add_getset(ctx, me, "movementY", me_get_movement_y, me_set_movement_y);
	avm2_builtin_add_method(ctx, me, "updateAfterEvent", me_update_after_event);
	event_override_method(ctx, me, "toString", me_to_string);
	{
		static const struct { const char* n; const char* v; } mc[] = {
			{ "CLICK", "click" }, { "DOUBLE_CLICK", "doubleClick" },
			{ "MOUSE_DOWN", "mouseDown" }, { "MOUSE_MOVE", "mouseMove" },
			{ "MOUSE_OUT", "mouseOut" }, { "MOUSE_OVER", "mouseOver" },
			{ "MOUSE_UP", "mouseUp" }, { "MOUSE_WHEEL", "mouseWheel" },
			{ "ROLL_OUT", "rollOut" }, { "ROLL_OVER", "rollOver" },
			{ "MIDDLE_CLICK", "middleClick" },
			{ "MIDDLE_MOUSE_DOWN", "middleMouseDown" },
			{ "MIDDLE_MOUSE_UP", "middleMouseUp" },
			{ "RIGHT_CLICK", "rightClick" },
			{ "RIGHT_MOUSE_DOWN", "rightMouseDown" },
			{ "RIGHT_MOUSE_UP", "rightMouseUp" },
			{ "CONTEXT_MENU", "contextMenu" },
			{ "RELEASE_OUTSIDE", "releaseOutside" },
			{ "MOUSE_LEAVE", "mouseLeave" },
		};
		for (size_t i = 0; i < sizeof(mc) / sizeof(mc[0]); i++)
			avm2_builtin_add_static_const(ctx, me, mc[i].n,
				avm2_string(avm2_string_from_literal(ctx, mc[i].v)));
	}

	// flash.events.TouchEvent. Headless never generates one, but a movie that
	// feature-checks for touch getlexes the class at class-init time (away3d's
	// demo does), so the constant bag has to exist. Its instance surface is
	// MouseEvent's plus the touch ids, which is what the .as declares.
	{
		// [API("667")] = SWF 10 in Ruffle's TouchEvent.as: a SWF 9 movie
		// cannot see the name at all (all_classes/events/swf9 lists it as
		// "not accessible"), while the class object still exists for the
		// runtime to mint internally.
		Avm2Class* te = avm2_builtin_class_api(ctx, "flash.events",
		                                       "TouchEvent", b->event_class,
		                                       10);
		te->instance_init.fn = mouse_event_init;
		te->instance_init.debug_name = "TouchEvent";
		avm2_builtin_add_getset(ctx, te, "localX", me_get_local_x, me_set_local_x);
		avm2_builtin_add_getset(ctx, te, "localY", me_get_local_y, me_set_local_y);
		avm2_builtin_add_getter(ctx, te, "stageX", me_get_stage_x);
		avm2_builtin_add_getter(ctx, te, "stageY", me_get_stage_y);
		avm2_builtin_add_getset(ctx, te, "ctrlKey", me_get_ctrl, me_set_ctrl);
		avm2_builtin_add_getset(ctx, te, "altKey", me_get_alt, me_set_alt);
		avm2_builtin_add_getset(ctx, te, "shiftKey", me_get_shift, me_set_shift);
		avm2_builtin_add_method(ctx, te, "updateAfterEvent", me_update_after_event);
		static const struct { const char* n; const char* v; } tc[] = {
			{ "TOUCH_BEGIN", "touchBegin" }, { "TOUCH_END", "touchEnd" },
			{ "TOUCH_MOVE", "touchMove" }, { "TOUCH_OUT", "touchOut" },
			{ "TOUCH_OVER", "touchOver" }, { "TOUCH_ROLL_OUT", "touchRollOut" },
			{ "TOUCH_ROLL_OVER", "touchRollOver" }, { "TOUCH_TAP", "touchTap" },
			{ "PROXIMITY_BEGIN", "proximityBegin" },
			{ "PROXIMITY_END", "proximityEnd" },
			{ "PROXIMITY_MOVE", "proximityMove" },
			{ "PROXIMITY_OUT", "proximityOut" },
			{ "PROXIMITY_OVER", "proximityOver" },
			{ "PROXIMITY_ROLL_OUT", "proximityRollOut" },
			{ "PROXIMITY_ROLL_OVER", "proximityRollOver" },
		};
		for (size_t i = 0; i < sizeof(tc) / sizeof(tc[0]); i++)
			avm2_builtin_add_static_const(ctx, te, tc[i].n,
				avm2_string(avm2_string_from_literal(ctx, tc[i].v)));
	}

	// flash.events.ContextMenuEvent.
	{
		Avm2Class* cme = avm2_builtin_class(ctx, "flash.events",
		                                    "ContextMenuEvent", b->event_class);
		cme->instance_init.fn = cme_init;
		cme->instance_init.debug_name = "ContextMenuEvent";
		g_context_menu_event_class = cme;
		avm2_builtin_add_getset(ctx, cme, "mouseTarget",
		                        me_get_related, me_set_related);
		avm2_builtin_add_getset(ctx, cme, "contextMenuOwner",
		                        cme_get_owner, cme_set_owner);
		avm2_builtin_add_getset(ctx, cme, "isMouseTargetInaccessible",
		                        cme_get_mouse_target_inaccessible,
		                        cme_set_mouse_target_inaccessible);
		event_override_method(ctx, cme, "toString", cme_to_string);
		event_override_method(ctx, cme, "clone", cme_clone);
		avm2_builtin_add_static_const(ctx, cme, "MENU_ITEM_SELECT",
			avm2_string(avm2_string_from_literal(ctx, "menuItemSelect")));
		avm2_builtin_add_static_const(ctx, cme, "MENU_SELECT",
			avm2_string(avm2_string_from_literal(ctx, "menuSelect")));
	}

	// flash.events.KeyboardEvent.
	Avm2Class* ke = avm2_builtin_class(ctx, "flash.events", "KeyboardEvent",
	                                   b->event_class);
	ke->instance_init.fn = keyboard_event_init;
	ke->instance_init.debug_name = "KeyboardEvent";
	b->keyboard_event_class = ke;
	avm2_builtin_add_getset(ctx, ke, "charCode", ke_get_char_code, ke_set_char_code);
	avm2_builtin_add_getset(ctx, ke, "keyCode", ke_get_key_code, ke_set_key_code);
	avm2_builtin_add_getset(ctx, ke, "keyLocation", ke_get_key_location, ke_set_key_location);
	avm2_builtin_add_getset(ctx, ke, "ctrlKey", me_get_ctrl, me_set_ctrl);
	avm2_builtin_add_getset(ctx, ke, "altKey", me_get_alt, me_set_alt);
	avm2_builtin_add_getset(ctx, ke, "shiftKey", me_get_shift, me_set_shift);
	avm2_builtin_add_getset(ctx, ke, "controlKey", ke_get_control, ke_set_control);
	avm2_builtin_add_getset(ctx, ke, "commandKey", me_get_command, me_set_command);
	avm2_builtin_add_method(ctx, ke, "updateAfterEvent", me_update_after_event);
	event_override_method(ctx, ke, "toString", ke_to_string);
	{
		static const struct { const char* n; const char* v; } kc[] = {
			{ "KEY_DOWN", "keyDown" }, { "KEY_UP", "keyUp" },
		};
		for (size_t i = 0; i < sizeof(kc) / sizeof(kc[0]); i++)
			avm2_builtin_add_static_const(ctx, ke, kc[i].n,
				avm2_string(avm2_string_from_literal(ctx, kc[i].v)));
	}

	// flash.events.FocusEvent.
	Avm2Class* fe = avm2_builtin_class(ctx, "flash.events", "FocusEvent",
	                                   b->event_class);
	fe->instance_init.fn = focus_event_init;
	fe->instance_init.debug_name = "FocusEvent";
	b->focus_event_class = fe;
	avm2_builtin_add_getset(ctx, fe, "relatedObject", fe_get_related, fe_set_related);
	avm2_builtin_add_getset(ctx, fe, "shiftKey", fe_get_shift, fe_set_shift);
	avm2_builtin_add_getset(ctx, fe, "keyCode", fe_get_key_code, fe_set_key_code);
	avm2_builtin_add_getset(ctx, fe, "direction", fe_get_direction, fe_set_direction);
	avm2_builtin_add_getset(ctx, fe, "isRelatedObjectInaccessible",
	                        fe_get_rel_inaccessible, fe_set_rel_inaccessible);
	event_override_method(ctx, fe, "toString", fe_to_string);
	{
		static const struct { const char* n; const char* v; } fc[] = {
			{ "FOCUS_IN", "focusIn" }, { "FOCUS_OUT", "focusOut" },
			{ "KEY_FOCUS_CHANGE", "keyFocusChange" },
			{ "MOUSE_FOCUS_CHANGE", "mouseFocusChange" },
		};
		for (size_t i = 0; i < sizeof(fc) / sizeof(fc[0]); i++)
			avm2_builtin_add_static_const(ctx, fe, fc[i].n,
				avm2_string(avm2_string_from_literal(ctx, fc[i].v)));
	}
}

// ---------------------------------------------------------------------------
// flash.automation  (AutomationAction / KeyboardAutomationAction /
// MouseAutomationAction / StageCapture / StageCaptureEvent)
// ---------------------------------------------------------------------------
//
// Ported from Ruffle's playerglobal stubs (core/src/avm2/globals/flash/
// automation/*.as) and graded by avm2/automation_classes (a SWF 9 fixture).
//
// VERSION GATE: none of the five carries an `[API]` annotation, so all five
// exist at every SWF version -- registered with plain avm2_builtin_class, not
// the _api variant. No all_classes fixture covers flash.automation, so there
// is no describeType surface to keep in step.

typedef struct Avm2AutomationActionExt
{
	const Avm2String* type;    // AutomationAction._type (defaults to "")
	double stage_x, stage_y;   // MouseAutomationAction
	int32_t delta;             // MouseAutomationAction._delta is int
	uint32_t key_code;         // KeyboardAutomationAction._keyCode is uint
} Avm2AutomationActionExt;

static Avm2Class* g_automation_action_class;
static Avm2Class* g_stage_capture_event_class;

static Avm2AutomationActionExt* auto_ext(Avm2Activation* act)
{
	Avm2Object* o = act->this_val.kind == AVM2_VALUE_OBJECT
		? act->this_val.u.obj : NULL;
	return o != NULL ? (Avm2AutomationActionExt*) o->native_ext : NULL;
}

// Builtin constructors in this package are arity-checked: the fixture opens
// each class with a deliberately wrong call and prints the #1063 text. Our
// native builtins do not enforce declared arity generally, so the check is
// local (min = required params, max = total params).
static void auto_check_argc(Avm2Activation* act, const char* qname,
                            uint32_t min_args, uint32_t max_args)
{
	uint32_t expected;
	if (act->argc < min_args) expected = min_args;
	else if (act->argc > max_args) expected = max_args;
	else return;
	char buf[224];
	snprintf(buf, sizeof(buf),
	         "Error #1063: Argument count mismatch on %s(). Expected %u, "
	         "got %u.", qname, expected, act->argc);
	avm2_throw_error(act->ctx, act->ctx->builtins.argument_error_class, buf);
}

static const Avm2String* auto_arg_str(Avm2Activation* act, uint32_t i)
{
	if (act->argc > i)
		return avm2_coerce_to_string(act->ctx, act->args[i]);
	return avm2_string_from_literal(act->ctx, "");
}

static Avm2Value auto_action_init(Avm2Activation* act)
{
	auto_check_argc(act, "flash.automation::AutomationAction", 0, 0);
	Avm2AutomationActionExt* e = auto_ext(act);
	if (e != NULL) e->type = avm2_string_from_literal(act->ctx, "");
	return avm2_undefined();
}

static Avm2Value auto_get_type(Avm2Activation* act)
{
	Avm2AutomationActionExt* e = auto_ext(act);
	if (e == NULL || e->type == NULL) return avm2_null();
	return avm2_string(e->type);
}

static Avm2Value auto_set_type(Avm2Activation* act)
{
	Avm2AutomationActionExt* e = auto_ext(act);
	if (e != NULL) e->type = auto_arg_str(act, 0);
	return avm2_undefined();
}

// KeyboardAutomationAction(type:String, keyCode:int = 0)
static Avm2Value auto_keyboard_init(Avm2Activation* act)
{
	auto_check_argc(act, "flash.automation::KeyboardAutomationAction", 1, 2);
	Avm2AutomationActionExt* e = auto_ext(act);
	if (e == NULL) return avm2_undefined();
	e->type = auto_arg_str(act, 0);
	// `keyCode:int` coerces first, then lands in a uint field.
	e->key_code = act->argc > 1
		? (uint32_t) avm2_coerce_to_i32(act->ctx, act->args[1]) : 0;
	return avm2_undefined();
}

static Avm2Value auto_get_key_code(Avm2Activation* act)
{
	Avm2AutomationActionExt* e = auto_ext(act);
	return avm2_uint_value(e != NULL ? e->key_code : 0);
}

static Avm2Value auto_set_key_code(Avm2Activation* act)
{
	Avm2AutomationActionExt* e = auto_ext(act);
	if (e != NULL && act->argc > 0)
		e->key_code = avm2_coerce_to_u32(act->ctx, act->args[0]);
	return avm2_undefined();
}

// MouseAutomationAction(type:String, stageX=0, stageY=0, delta=0)
static Avm2Value auto_mouse_init(Avm2Activation* act)
{
	auto_check_argc(act, "flash.automation::MouseAutomationAction", 1, 4);
	Avm2AutomationActionExt* e = auto_ext(act);
	if (e == NULL) return avm2_undefined();
	Avm2Context* ctx = act->ctx;
	e->type = auto_arg_str(act, 0);
	e->stage_x = act->argc > 1 ? avm2_coerce_to_number(ctx, act->args[1]) : 0.0;
	e->stage_y = act->argc > 2 ? avm2_coerce_to_number(ctx, act->args[2]) : 0.0;
	// The 4th parameter is declared Number but the backing field is int.
	e->delta = act->argc > 3 ? avm2_coerce_to_i32(ctx, act->args[3]) : 0;
	return avm2_undefined();
}

static Avm2Value auto_get_stage_x(Avm2Activation* act)
{ Avm2AutomationActionExt* e = auto_ext(act);
  return avm2_number(e != NULL ? e->stage_x : 0.0); }
static Avm2Value auto_set_stage_x(Avm2Activation* act)
{ Avm2AutomationActionExt* e = auto_ext(act);
  if (e != NULL && act->argc > 0)
      e->stage_x = avm2_coerce_to_number(act->ctx, act->args[0]);
  return avm2_undefined(); }
static Avm2Value auto_get_stage_y(Avm2Activation* act)
{ Avm2AutomationActionExt* e = auto_ext(act);
  return avm2_number(e != NULL ? e->stage_y : 0.0); }
static Avm2Value auto_set_stage_y(Avm2Activation* act)
{ Avm2AutomationActionExt* e = auto_ext(act);
  if (e != NULL && act->argc > 0)
      e->stage_y = avm2_coerce_to_number(act->ctx, act->args[0]);
  return avm2_undefined(); }
static Avm2Value auto_get_delta(Avm2Activation* act)
{ Avm2AutomationActionExt* e = auto_ext(act);
  return avm2_integer(e != NULL ? e->delta : 0); }
static Avm2Value auto_set_delta(Avm2Activation* act)
{ Avm2AutomationActionExt* e = auto_ext(act);
  if (e != NULL && act->argc > 0)
      e->delta = avm2_coerce_to_i32(act->ctx, act->args[0]);
  return avm2_undefined(); }

// --- StageCaptureEvent -----------------------------------------------------
//
// Its three own fields borrow slots already present in the shared Avm2EventExt
// (the "one struct for the whole event ladder" pattern the header documents):
// `response_url` (HTTPStatusEvent) carries `url`, `key_code` (KeyboardEvent)
// carries the uint `checksum`, and `bytes_loaded` (ProgressEvent) carries
// `pts`. StageCaptureEvent extends Event DIRECTLY, so none of those three
// donors is reachable through its own vtable and no aliasing is observable.

static Avm2Value sce_init(Avm2Activation* act)
{
	auto_check_argc(act, "flash.automation::StageCaptureEvent", 1, 6);
	Avm2Context* ctx = act->ctx;
	Avm2EventExt* e = this_event(act);
	if (e == NULL) return avm2_undefined();
	e->type = avm2_coerce_to_string(ctx, act->args[0]);
	e->bubbles = (uint8_t) (act->argc > 1 && avm2_coerce_to_boolean(act->args[1]));
	e->cancelable = (uint8_t) (act->argc > 2
	                           && avm2_coerce_to_boolean(act->args[2]));
	e->response_url = act->argc > 3
		? avm2_coerce_to_string(ctx, act->args[3])
		: avm2_string_from_literal(ctx, "");
	e->key_code = act->argc > 4 ? avm2_coerce_to_u32(ctx, act->args[4]) : 0;
	e->bytes_loaded = act->argc > 5
		? avm2_coerce_to_number(ctx, act->args[5]) : 0.0;
	return avm2_undefined();
}

static Avm2Value sce_get_url(Avm2Activation* act)
{
	Avm2EventExt* e = this_event(act);
	if (e == NULL || e->response_url == NULL) return avm2_null();
	return avm2_string(e->response_url);
}
static Avm2Value sce_get_checksum(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act);
  return avm2_uint_value(e != NULL ? e->key_code : 0); }
static Avm2Value sce_get_pts(Avm2Activation* act)
{ Avm2EventExt* e = this_event(act);
  return avm2_number(e != NULL ? e->bytes_loaded : 0.0); }

// clone() forwards only FIVE arguments -- `pts` is dropped and comes back 0.
// That is playerglobal's own behaviour and the fixture grades it.
static Avm2Value sce_clone(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Value args[5];
	args[0] = avm2_get_public_property(ctx, act->this_val, "type", 4, NULL);
	args[1] = avm2_get_public_property(ctx, act->this_val, "bubbles", 7, NULL);
	args[2] = avm2_get_public_property(ctx, act->this_val, "cancelable", 10, NULL);
	args[3] = avm2_get_public_property(ctx, act->this_val, "url", 3, NULL);
	args[4] = avm2_get_public_property(ctx, act->this_val, "checksum", 8, NULL);
	if (g_stage_capture_event_class == NULL) return avm2_null();
	return avm2_class_construct(ctx, g_stage_capture_event_class, args, 5);
}

static Avm2Value sce_to_string(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	static const char* const fields[] = {
		"StageCaptureEvent", "type", "bubbles", "cancelable", "eventPhase",
		"url", "checksum"
	};
	Avm2Value args[7];
	for (int i = 0; i < 7; i++)
		args[i] = avm2_string(avm2_string_from_literal(ctx, fields[i]));
	return avm2_call_public_property(ctx, act->this_val, "formatToString", 14,
	                                 args, 7);
}

static void register_automation_classes(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;

	Avm2Class* aa = avm2_builtin_class(ctx, "flash.automation",
	                                   "AutomationAction", b->object_class);
	g_automation_action_class = aa;
	aa->native_ext_size = sizeof(Avm2AutomationActionExt);
	aa->instance_init.fn = auto_action_init;
	aa->instance_init.debug_name = "AutomationAction";
	avm2_builtin_add_getset(ctx, aa, "type", auto_get_type, auto_set_type);

	Avm2Class* ka = avm2_builtin_class(ctx, "flash.automation",
	                                   "KeyboardAutomationAction", aa);
	ka->native_ext_size = sizeof(Avm2AutomationActionExt);
	ka->instance_init.fn = auto_keyboard_init;
	ka->instance_init.debug_name = "KeyboardAutomationAction";
	avm2_builtin_add_getset(ctx, ka, "keyCode", auto_get_key_code,
	                        auto_set_key_code);
	sconst(ctx, ka, "KEY_DOWN", "keyDown");
	sconst(ctx, ka, "KEY_UP", "keyUp");

	Avm2Class* ma = avm2_builtin_class(ctx, "flash.automation",
	                                   "MouseAutomationAction", aa);
	ma->native_ext_size = sizeof(Avm2AutomationActionExt);
	ma->instance_init.fn = auto_mouse_init;
	ma->instance_init.debug_name = "MouseAutomationAction";
	avm2_builtin_add_getset(ctx, ma, "stageX", auto_get_stage_x,
	                        auto_set_stage_x);
	avm2_builtin_add_getset(ctx, ma, "stageY", auto_get_stage_y,
	                        auto_set_stage_y);
	avm2_builtin_add_getset(ctx, ma, "delta", auto_get_delta, auto_set_delta);
	sconst(ctx, ma, "MIDDLE_MOUSE_DOWN", "middleMouseDown");
	sconst(ctx, ma, "MIDDLE_MOUSE_UP", "middleMouseUp");
	sconst(ctx, ma, "MOUSE_DOWN", "mouseDown");
	sconst(ctx, ma, "MOUSE_MOVE", "mouseMove");
	sconst(ctx, ma, "MOUSE_UP", "mouseUp");
	sconst(ctx, ma, "MOUSE_WHEEL", "mouseWheel");
	sconst(ctx, ma, "RIGHT_MOUSE_DOWN", "rightMouseDown");
	sconst(ctx, ma, "RIGHT_MOUSE_UP", "rightMouseUp");

	Avm2Class* sc = avm2_builtin_class(ctx, "flash.automation", "StageCapture",
	                                   b->event_dispatcher_class);
	sconst(ctx, sc, "CURRENT", "current");
	sconst(ctx, sc, "MULTIPLE", "multiple");
	sconst(ctx, sc, "NEXT", "next");
	sconst(ctx, sc, "RASTER", "raster");
	sconst(ctx, sc, "SCREEN", "screen");
	sconst(ctx, sc, "STAGE", "stage");

	Avm2Class* sce = avm2_builtin_class(ctx, "flash.automation",
	                                    "StageCaptureEvent", b->event_class);
	g_stage_capture_event_class = sce;
	sce->instance_init.fn = sce_init;
	sce->instance_init.debug_name = "StageCaptureEvent";
	avm2_builtin_add_getter(ctx, sce, "url", sce_get_url);
	avm2_builtin_add_getter(ctx, sce, "checksum", sce_get_checksum);
	avm2_builtin_add_getter(ctx, sce, "pts", sce_get_pts);
	event_override_method(ctx, sce, "clone", sce_clone);
	event_override_method(ctx, sce, "toString", sce_to_string);
	sconst(ctx, sce, "CAPTURE", "capture");
}

void avm2_register_events(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;

	// flash.events.Event.
	Avm2Class* event = avm2_builtin_class(ctx, "flash.events", "Event",
	                                      b->object_class);
	event->instance_init.fn = event_init;
	event->instance_init.debug_name = "Event";
	event->native_ext_size = sizeof(Avm2EventExt);
	event->native_init = event_native_init;
	b->event_class = event;
	avm2_builtin_add_getter(ctx, event, "type", event_get_type);
	avm2_builtin_add_getter(ctx, event, "bubbles", event_get_bubbles);
	avm2_builtin_add_getter(ctx, event, "cancelable", event_get_cancelable);
	avm2_builtin_add_getter(ctx, event, "target", event_get_target);
	avm2_builtin_add_getter(ctx, event, "currentTarget", event_get_current_target);
	avm2_builtin_add_getter(ctx, event, "eventPhase", event_get_event_phase);
	avm2_builtin_add_method(ctx, event, "clone", event_clone);
	avm2_builtin_add_method(ctx, event, "toString", event_to_string);
	avm2_builtin_add_method(ctx, event, "formatToString", event_format_to_string);
	avm2_builtin_add_method(ctx, event, "isDefaultPrevented", event_is_default_prevented);
	avm2_builtin_add_method(ctx, event, "preventDefault", event_prevent_default);
	avm2_builtin_add_method(ctx, event, "stopPropagation", event_stop_propagation);
	avm2_builtin_add_method(ctx, event, "stopImmediatePropagation",
	                        event_stop_immediate_propagation);
	// Static const inventory from Ruffle Event.as.
	static const struct { const char* name; const char* v; } consts[] = {
		{ "ACTIVATE", "activate" }, { "ADDED", "added" },
		{ "ADDED_TO_STAGE", "addedToStage" },
		{ "BROWSER_ZOOM_CHANGE", "browserZoomChange" }, { "CANCEL", "cancel" },
		{ "CHANGE", "change" }, { "CLEAR", "clear" }, { "CLOSE", "close" },
		{ "CLOSING", "closing" }, { "COMPLETE", "complete" },
		{ "CONNECT", "connect" }, { "COPY", "copy" }, { "CUT", "cut" },
		{ "DEACTIVATE", "deactivate" }, { "ENTER_FRAME", "enterFrame" },
		{ "FRAME_CONSTRUCTED", "frameConstructed" }, { "EXITING", "exiting" },
		{ "EXIT_FRAME", "exitFrame" }, { "FRAME_LABEL", "frameLabel" },
		{ "ID3", "id3" }, { "INIT", "init" }, { "MOUSE_LEAVE", "mouseLeave" },
		{ "OPEN", "open" }, { "PASTE", "paste" }, { "REMOVED", "removed" },
		{ "REMOVED_FROM_STAGE", "removedFromStage" }, { "RENDER", "render" },
		{ "RESIZE", "resize" }, { "SCROLL", "scroll" },
		{ "TEXT_INTERACTION_MODE_CHANGE", "textInteractionModeChange" },
		{ "SELECT", "select" }, { "SELECT_ALL", "selectAll" },
		{ "SOUND_COMPLETE", "soundComplete" },
		{ "TAB_CHILDREN_CHANGE", "tabChildrenChange" },
		{ "TAB_ENABLED_CHANGE", "tabEnabledChange" },
		{ "TAB_INDEX_CHANGE", "tabIndexChange" }, { "UNLOAD", "unload" },
		{ "FULLSCREEN", "fullScreen" },
		{ "CONTEXT3D_CREATE", "context3DCreate" },
		{ "TEXTURE_READY", "textureReady" },
		{ "SUSPEND", "suspend" },
	};
	for (size_t i = 0; i < sizeof(consts) / sizeof(consts[0]); i++)
	{
		avm2_builtin_add_static_const(
			ctx, event, consts[i].name,
			avm2_string(avm2_string_from_literal(ctx, consts[i].v)));
	}
	// API-versioned surface. Ruffle Event.as tags these `[API("682")]`, which
	// is avmplus api-version 682-660 = 22 = SWF_17, so they only exist for
	// SWF 17+ content. The gate is on the ROOT movie's version, not the
	// defining SWF's: avm2/cross_api_version_call_older (root v43, child v12)
	// sees `workerState`, while cross_api_version_call_newer (root v12, child
	// v43) must see `undefined`. avm2/all_classes/events/swf{9,10,11,12}
	// likewise omit all four; swf30 lists them.
	// (Same idiom as avm2_number.c's API-680 `swf_version >= 16` Math-on-
	// Number gate and avm2_globals.c's JSON-at-13 gate.)
	if (ctx->swf_version >= 17)
	{
		static const struct { const char* name; const char* v; } api682[] = {
			{ "VIDEO_FRAME", "videoFrame" },
			{ "CHANNEL_MESSAGE", "channelMessage" },
			{ "CHANNEL_STATE", "channelState" },
			{ "WORKER_STATE", "workerState" },
		};
		for (size_t i = 0; i < sizeof(api682) / sizeof(api682[0]); i++)
		{
			avm2_builtin_add_static_const(
				ctx, event, api682[i].name,
				avm2_string(avm2_string_from_literal(ctx, api682[i].v)));
		}
	}

	// flash.events.EventPhase.
	{
		Avm2Class* phase = avm2_builtin_class(ctx, "flash.events", "EventPhase",
		                                      b->object_class);
		avm2_builtin_add_static_const(ctx, phase, "CAPTURING_PHASE", avm2_uint_value(1));
		avm2_builtin_add_static_const(ctx, phase, "AT_TARGET", avm2_uint_value(2));
		avm2_builtin_add_static_const(ctx, phase, "BUBBLING_PHASE", avm2_uint_value(3));
	}

	// flash.events.IEventDispatcher (interface): vtable entries carry the
	// interface namespace so implementor classes get call-through aliases
	// (avm2_class.c add_iface_aliases_from).
	Avm2Class* iface = avm2_builtin_class(ctx, "flash.events", "IEventDispatcher", NULL);
	iface->flags |= AVM2_CLASS_FLAG_INTERFACE;
	b->ievent_dispatcher_class = iface;
	{
		static const char* const inames[5] = {
			"addEventListener", "removeEventListener", "dispatchEvent",
			"hasEventListener", "willTrigger"
		};
		for (int i = 0; i < 5; i++)
		{
			Avm2PropEntry e;
			memset(&e, 0, sizeof(e));
			e.key = events_key("flash.events:IEventDispatcher", inames[i]);
			e.kind = AVM2_PROP_METHOD;
			e.method.debug_name = inames[i];
			e.defining_class = iface;
			avm2_vtable_append(ctx, &iface->ivtable, &e);
		}
	}

	// flash.events.EventDispatcher.
	Avm2Class* ed = avm2_builtin_class(ctx, "flash.events", "EventDispatcher",
	                                   b->object_class);
	ed->instance_init.fn = ed_init;
	ed->instance_init.debug_name = "EventDispatcher";
	ed->native_ext_size = sizeof(Avm2EventDispatcherExt);
	b->event_dispatcher_class = ed;
	avm2_builtin_add_method(ctx, ed, "addEventListener", ed_add_event_listener);
	avm2_builtin_add_method(ctx, ed, "removeEventListener", ed_remove_event_listener);
	avm2_builtin_add_method(ctx, ed, "hasEventListener", ed_has_event_listener);
	avm2_builtin_add_method(ctx, ed, "willTrigger", ed_will_trigger);
	avm2_builtin_add_method(ctx, ed, "dispatchEvent", ed_dispatch_event);
	avm2_builtin_add_method(ctx, ed, "toString", ed_to_string);
	// Declare + alias the interface (emitted classes get the same aliases
	// from class_add_interface_aliases).
	ed->interface_count = 1;
	ed->interfaces = avm2_alloc(ctx, sizeof(Avm2Class*));
	ed->interfaces[0] = iface;
	for (uint32_t i = 0; i < iface->ivtable.count; i++)
	{
		const Avm2PropEntry* ie = &iface->ivtable.entries[i];
		Avm2PropKey pub = avm2_public_key(ie->key.name, ie->key.name_len);
		const Avm2PropEntry* own = avm2_vtable_find(&ed->ivtable, &pub);
		if (own == NULL) continue;
		Avm2PropEntry alias = *own;
		alias.key = ie->key;
		// Dispatch machinery, not a trait avmplus has: describeType must not
		// report EventDispatcher's five methods a SECOND time carrying
		// uri="flash.events:IEventDispatcher" (avm2_class.c:1263 sets the
		// same flag on the ABC-side aliases). Subclasses never showed the
		// duplicate because HIDE_NSURI_METHODS suppresses a namespace their
		// SUPERCLASS already declares — EventDispatcher, whose superclass is
		// Object, is the one class the shadow rule cannot cover, and
		// all_classes/events/swf* is the first fixture to describe it.
		alias.is_iface_alias = 1;
		avm2_vtable_append(ctx, &ed->ivtable, &alias);
	}

	// flash.events.MouseEvent / KeyboardEvent / FocusEvent (Stage 8).
	register_input_events(ctx);

	// flash.events.ProgressEvent / ErrorEvent family / HTTPStatusEvent /
	// StatusEvent — network/loader events (after TextEvent: ErrorEvent
	// extends it).
	register_net_events(ctx);

	// flash.events.TimerEvent (Stage 10). Base Event ctor + a toString that
	// lists the standard 5 fields under the "TimerEvent" class name.
	Avm2Class* tev = avm2_builtin_class(ctx, "flash.events", "TimerEvent",
	                                    b->event_class);
	tev->instance_init.fn = event_init;
	tev->instance_init.debug_name = "TimerEvent";
	b->timer_event_class = tev;
	event_override_method(ctx, tev, "toString", timer_event_to_string);
	avm2_builtin_add_method(ctx, tev, "updateAfterEvent", me_update_after_event);
	avm2_builtin_add_static_const(ctx, tev, "TIMER",
		avm2_string(avm2_string_from_literal(ctx, "timer")));
	avm2_builtin_add_static_const(ctx, tev, "TIMER_COMPLETE",
		avm2_string(avm2_string_from_literal(ctx, "timerComplete")));

	// flash.events.UncaughtErrorEvents — [API("667")] in Ruffle
	// (globals/flash/events/UncaughtErrorEvents.as), i.e. SWF 10+; an empty
	// EventDispatcher subclass with a do-nothing constructor. LoaderInfo's
	// `uncaughtErrorEvents` getter mints one per LoaderInfo.
	g_uncaught_error_events_class =
		avm2_builtin_class_api(ctx, "flash.events", "UncaughtErrorEvents",
		                       b->event_dispatcher_class, 10);
	g_uncaught_error_events_class->instance_init.fn = ed_init;
	g_uncaught_error_events_class->instance_init.debug_name =
		"UncaughtErrorEvents";

	// flash.automation (avm2/automation_classes) -- after the Event ladder,
	// StageCaptureEvent extends Event and StageCapture extends EventDispatcher.
	register_automation_classes(ctx);
}

Avm2Class* avm2_uncaught_error_events_class(void)
{
	return g_uncaught_error_events_class;
}

Avm2Object* avm2_timer_event_new(Avm2Context* ctx, const Avm2String* type,
                                 int bubbles, int cancelable)
{
	Avm2Value args[3];
	args[0] = avm2_string(type);
	args[1] = avm2_bool(bubbles != 0);
	args[2] = avm2_bool(cancelable != 0);
	Avm2Value v = avm2_class_construct(ctx, ctx->builtins.timer_event_class, args, 3);
	return v.u.obj;
}
