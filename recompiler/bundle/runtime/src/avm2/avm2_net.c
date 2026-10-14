// avm2_net.c — flash.net transport classes: Socket, NetConnection, Responder,
// NetStream, the FileReference family, and the constant-valued AIR/AV stubs.
//
// The URL half of flash.net (URLRequest/URLLoader/URLVariables/navigateToURL)
// lives in avm2_globals.c::register_net and avm2_display.c; this file owns the
// classes that model a *connection*.
//
// Tranche 1 of the net/socket arc (SWFRecompDocs/plans/net-socket-arc.md)
// deliberately implements the surface with NO transport behind it, because
// that is what the corpus grades:
//
//   * `Socket` — every read/write/flush/close on an unconnected socket throws
//     IOError #2002, which is the whole of `socket_errors` (56 lines).
//   * `NetConnection` — a two-variant protocol model (Ruffle
//     core/src/net_connection.rs): `connect(null)` opens a "local" connection
//     that reports a fixed set of constants, `connect("http(s)://…")` opens a
//     Flash Remoting connection that reports only its URI, and anything else
//     opens nothing. Every accessor unavailable in the current state throws
//     ArgumentError #2126. `netconnection_properties` grades all three states.
//   * The NetStatusEvent dispatch ORDER around connect/close, including the
//     second, empty-code event Flash emits when an explicit close tears down a
//     Flash Remoting connection — `netconnection_close` grades exactly that.
//
// Tranche 2 put a transport behind the socket half: a `Socket` now owns a
// ByteArray ext PAIR (inbound / outbound) and reuses avm2_bytearray.c's 28
// IDataInput/IDataOutput bodies verbatim through the direction-aware alt
// resolver, while `XMLSocket` sits on top as a NUL-framed DataEvent source.
// The bytes come from the scripted mock in src/utils.c (socket.json replay);
// with no script loaded connect() opens nothing and the #2002 surface above
// is exactly what remains.
//
// Tranche 3 added the file half — FileFilter, FileReference and
// FileReferenceList over the scripted file-dialog mock (dialog_events.h). It
// is not a *connection*, but it is flash.net and it shares this file's
// event-dispatch and per-instance-ext idioms; see the block above
// register_file_reference for the model.

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <avm2/avm2_abc.h>
#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_gc.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_value.h>
#include <amf_packet.h>
#include <dialog_events.h>
#include <libswf/swf.h>   // DataFileEntry / findDataFile (bundled response packets)
#include <socket_events.h>
#include <utils.h>        // swf_log_fetch_queue / SWF_LOG_FETCH_ENABLED

// ---------------------------------------------------------------------------
// shared helpers
// ---------------------------------------------------------------------------

static Avm2Value net2_str(Avm2Activation* act, const char* s)
{ return avm2_string(avm2_string_from_literal(act->ctx, s)); }

static Avm2Value net2_noop(Avm2Activation* act)
{ (void) act; return avm2_undefined(); }

static Avm2Object* net2_this(Avm2Activation* act)
{
	return (act->this_val.kind == AVM2_VALUE_OBJECT) ? act->this_val.u.obj : NULL;
}

static int net2_arg_bool(Avm2Activation* act, uint32_t i, int dflt)
{
	if (act->argc <= i) return dflt;
	return avm2_coerce_to_boolean(act->args[i]) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// flash.net.Socket
// ---------------------------------------------------------------------------
//
// Ruffle's Socket is an EventDispatcher over a pair of buffers plus a handle
// into the navigator's socket table. Every operation that would touch the
// wire first asserts the socket is open and raises IOError #2002 otherwise —
// Ruffle's socket.rs guards each native with `assert_socket_open!`, and Flash
// renders that as "Error: Error #2002: …" because Error's constructor stamps
// `this.name` from *Error*'s prototype, not the subclass's (see
// avm2_error.c::error_init).
//
// The two buffers are plain Avm2ByteArrayExts, so all 28 IDataInput /
// IDataOutput bodies are shared with flash.utils.ByteArray unchanged: reads
// resolve to the inbound buffer, writes to the outbound one. Ruffle drains
// its read buffer from the front; we advance `position` instead, which is
// behaviourally identical (bytesAvailable = len - position) and lets an
// inbound chunk simply append.

typedef struct Avm2SocketExt
{
	Avm2EventDispatcherExt dispatcher;  // extends EventDispatcher (MUST be first)
	Avm2Object* self;                   // the dispatch callback's target
	Avm2Object* xml_owner;              // set when this Socket backs an XMLSocket
	int handle;                         // mock handle; 0 = never connected
	Avm2ByteArrayExt in_buf;            // server -> player
	Avm2ByteArrayExt out_buf;           // player -> server, drained by flush()
	double timeout;
} Avm2SocketExt;

static Avm2Class* g_socket_class;

// Live sockets are GC roots: Ruffle's `Sockets` map holds a GC pointer to each
// target, so a socket whose only other reference is a closure over it must
// survive until the connection ends.
#define MAX_LIVE_SOCKETS 32
static Avm2Object* g_live_sockets[MAX_LIVE_SOCKETS];
static int g_live_socket_count;

static void socket_root(Avm2Object* o)
{
	if (o == NULL) return;
	for (int i = 0; i < g_live_socket_count; i++)
		if (g_live_sockets[i] == o) return;
	if (g_live_socket_count < MAX_LIVE_SOCKETS)
		g_live_sockets[g_live_socket_count++] = o;
}

static int socket_obj_is(Avm2Object* o)
{
	if (o == NULL || g_socket_class == NULL) return 0;
	for (Avm2Class* c = o->cls; c != NULL; c = c->super_class)
		if (c == g_socket_class) return 1;
	return 0;
}

static Avm2SocketExt* socket_ext(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	if (!socket_obj_is(o) || o->native_ext == NULL) return NULL;
	return (Avm2SocketExt*) o->native_ext;
}

static _Noreturn void socket_throw_invalid(Avm2Context* ctx)
{
	avm2_throw_error(ctx, ctx->builtins.io_error_class,
	                 "Error #2002: Operation attempted on invalid socket.");
}

static int socket_is_open(const Avm2SocketExt* e)
{
	return e != NULL && e->handle != 0 && swf_socket_is_connected(e->handle);
}

static Avm2SocketExt* socket_require_open(Avm2Activation* act)
{
	Avm2SocketExt* e = socket_ext(act);
	if (!socket_is_open(e)) socket_throw_invalid(act->ctx);
	return e;
}

// avm2_bytearray.c's alt resolver: hands the ByteArray bodies whichever of
// the two buffers matches the direction, after the same open-socket assert
// every Ruffle native performs.
static Avm2ByteArrayExt* socket_ba_resolve(Avm2Activation* act, int write_dir)
{
	Avm2Object* o = net2_this(act);
	if (!socket_obj_is(o) || o->native_ext == NULL) return NULL;
	Avm2SocketExt* e = (Avm2SocketExt*) o->native_ext;
	if (!socket_is_open(e)) socket_throw_invalid(act->ctx);
	return write_dir ? &e->out_buf : &e->in_buf;
}

static Avm2Value socket_init(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	Avm2SocketExt* e = socket_ext(act);
	if (e == NULL) return avm2_undefined();
	memset(&e->in_buf, 0, sizeof(e->in_buf));
	memset(&e->out_buf, 0, sizeof(e->out_buf));
	e->in_buf.object_encoding = 3;
	e->out_buf.object_encoding = 3;
	e->handle = 0;
	e->self = o;
	e->xml_owner = NULL;
	e->timeout = 20000;
	// Socket(host, port) with a non-null host connects immediately.
	if (act->argc > 0 && act->args[0].kind != AVM2_VALUE_NULL
	    && act->args[0].kind != AVM2_VALUE_UNDEFINED)
	{
		return avm2_call_public_property(act->ctx, act->this_val, "connect", 7,
		                                 act->args, act->argc);
	}
	return avm2_undefined();
}

// XMLSocket's private listeners on its inner Socket (see the XMLSocket block
// below); a no-op when this Socket is script-visible.
static void xmlsocket_relay(Avm2Context* ctx, Avm2Object* xml_obj,
                            Avm2SocketExt* sock, int action, size_t chunk_len);

// Server -> player, delivered by the mock at the tick boundary.
static void socket_dispatch(void* target, int action,
                            const unsigned char* data, size_t len)
{
	Avm2Object* o = (Avm2Object*) target;
	if (o == NULL || o->native_ext == NULL) return;
	Avm2SocketExt* e = (Avm2SocketExt*) o->native_ext;
	Avm2Context* ctx = avm2_get_context();

	switch (action)
	{
	case SWF_SOCKET_CONNECT:
	{
		Avm2Object* ev = avm2_event_new(ctx, avm2_string_from_literal(ctx, "connect"),
		                                0, 0);
		if (ev != NULL) avm2_dispatch_event(ctx, o, ev);
		break;
	}
	case SWF_SOCKET_DATA:
	{
		// Append at the end, leaving `position` (the read cursor) alone.
		uint32_t at = e->in_buf.len;
		avm2_bytearray_set_length_public(ctx, &e->in_buf, at + (uint32_t) len);
		if (len > 0) memcpy(e->in_buf.bytes + at, data, len);
		// bytesTotal is unused by socketData (Ruffle passes 0).
		Avm2Object* ev = avm2_progress_event_new(
			ctx, avm2_string_from_literal(ctx, "socketData"), (double) len, 0);
		if (ev != NULL) avm2_dispatch_event(ctx, o, ev);
		break;
	}
	case SWF_SOCKET_CLOSE:
	{
		e->in_buf.len = 0;
		e->in_buf.position = 0;
		e->out_buf.len = 0;
		e->out_buf.position = 0;
		Avm2Object* ev = avm2_event_new(ctx, avm2_string_from_literal(ctx, "close"),
		                                0, 0);
		if (ev != NULL) avm2_dispatch_event(ctx, o, ev);
		break;
	}
	default:
		break;
	}
	if (e->xml_owner != NULL) xmlsocket_relay(ctx, e->xml_owner, e, action, len);
}

static Avm2Value socket_connect(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2SocketExt* e = socket_ext(act);
	if (e == NULL) return avm2_undefined();

	char host[512];
	host[0] = '\0';
	if (act->argc > 0)
	{
		const Avm2String* s = avm2_coerce_to_string(ctx, act->args[0]);
		if (s != NULL)
		{
			size_t n = s->len < sizeof(host) - 1 ? s->len : sizeof(host) - 1;
			memcpy(host, s->utf8, n);
			host[n] = '\0';
		}
	}
	uint32_t port = act->argc > 1 ? avm2_coerce_to_u32(ctx, act->args[1]) : 0;
	if (port > 0xFFFF)
	{
		avm2_throw_error(ctx, ctx->builtins.range_error_class,
		                 "Error #2003: Invalid socket port number specified.");
	}

	socket_root(e->self);

	int prev = e->handle;
	// Flash's `host` is a C string and stops at a NUL (Ruffle sanitize_host);
	// the copy above already truncates there.
	e->handle = swf_socket_connect(host, (int) port, e->self, socket_dispatch);
	// "When a new connection is created the existing one is closed" (AS3 docs).
	if (prev != 0) swf_socket_close(prev);
	return avm2_undefined();
}

static Avm2Value socket_close(Avm2Activation* act)
{
	Avm2SocketExt* e = socket_require_open(act);
	swf_socket_close(e->handle);
	return avm2_undefined();
}

// One flush() = one message on the wire. The write buffer is drained whole,
// regardless of where `position` sits.
static Avm2Value socket_flush(Avm2Activation* act)
{
	Avm2SocketExt* e = socket_require_open(act);
	swf_socket_send(e->handle, e->out_buf.bytes, e->out_buf.len);
	e->out_buf.len = 0;
	e->out_buf.position = 0;
	return avm2_undefined();
}

static Avm2Value socket_get_connected(Avm2Activation* act)
{ return avm2_bool(socket_is_open(socket_ext(act))); }

static Avm2Value socket_get_bytes_available(Avm2Activation* act)
{
	Avm2SocketExt* e = socket_ext(act);
	if (e == NULL) return avm2_uint_value(0);
	uint32_t p = e->in_buf.position, n = e->in_buf.len;
	return avm2_uint_value(p <= n ? n - p : 0);
}

// bytesPending is a Ruffle stub_getter that returns 0 regardless.
static Avm2Value socket_get_bytes_pending(Avm2Activation* act)
{ (void) act; return avm2_uint_value(0); }

static Avm2Value socket_get_endian(Avm2Activation* act)
{
	Avm2SocketExt* e = socket_ext(act);
	return net2_str(act, (e != NULL && e->in_buf.endian_little) ? "littleEndian"
	                                                           : "bigEndian");
}
static Avm2Value socket_set_endian(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2SocketExt* e = socket_ext(act);
	if (e == NULL) return avm2_undefined();
	const Avm2String* s = act->argc > 0 ? avm2_coerce_to_string(ctx, act->args[0])
	                                    : NULL;
	int little;
	if (s != NULL && s->len == 12 && memcmp(s->utf8, "littleEndian", 12) == 0)
		little = 1;
	else if (s != NULL && s->len == 9 && memcmp(s->utf8, "bigEndian", 9) == 0)
		little = 0;
	else
	{
		avm2_throw_error(ctx, ctx->builtins.argument_error_class,
		                 "Error #2008: Parameter endian must be one of the "
		                 "accepted values.");
	}
	// Mirrored into both halves: the shared ByteArray bodies read it off
	// whichever ext the direction resolved to.
	e->in_buf.endian_little = (uint8_t) little;
	e->out_buf.endian_little = (uint8_t) little;
	return avm2_undefined();
}

static Avm2Value socket_get_object_encoding(Avm2Activation* act)
{
	Avm2SocketExt* e = socket_ext(act);
	return avm2_uint_value(e != NULL ? e->in_buf.object_encoding : 3);
}
static Avm2Value socket_set_object_encoding(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2SocketExt* e = socket_ext(act);
	if (e == NULL) return avm2_undefined();
	uint32_t enc = act->argc > 0 ? avm2_coerce_to_u32(ctx, act->args[0]) : 3;
	if (enc != 0 && enc != 3)
	{
		avm2_throw_error(ctx, ctx->builtins.argument_error_class,
		                 "Error #2008: Parameter objectEncoding must be one of "
		                 "the accepted values.");
	}
	e->in_buf.object_encoding = (uint8_t) enc;
	e->out_buf.object_encoding = (uint8_t) enc;
	return avm2_undefined();
}

// Socket.timeout clamps to a 250ms floor (Socket.as: `_timeout = value < 250 ?
// 250 : value`), which is what the XMLSocket test's "Timeout clamp" block
// grades.
static Avm2Value socket_get_timeout(Avm2Activation* act)
{ Avm2SocketExt* e = socket_ext(act); return avm2_uint_value(e ? (uint32_t) e->timeout : 20000); }
static Avm2Value socket_set_timeout(Avm2Activation* act)
{
	Avm2SocketExt* e = socket_ext(act);
	if (e != NULL && act->argc > 0)
	{
		uint32_t v = avm2_coerce_to_u32(act->ctx, act->args[0]);
		e->timeout = (v < 250) ? 250 : v;
	}
	return avm2_undefined();
}

// FileReference/FileReferenceList roots (the file half, defined below).
static void gc_mark_roots_file(void);
// NetConnection call-queue roots (the remoting half, defined below).
static void gc_mark_roots_nc_queue(void);
// LocalConnection channel-map + message-queue roots (defined below).
static void gc_mark_roots_lc(void);
// NetStream queued-onMetaData roots (defined below).
static void gc_mark_roots_netstream(void);

void avm2_gc_mark_roots_net(Avm2Context* ctx)
{
	(void) ctx;
	for (int i = 0; i < g_live_socket_count; i++)
	{
		Avm2Object* o = g_live_sockets[i];
		if (o == NULL) continue;
		avm2_gc_mark_object(o);
		// An XMLSocket's inner Socket is reachable only from C, and the
		// XMLSocket itself only from the inner Socket's back-pointer while
		// the connection lives — mark both halves of the pair.
		Avm2SocketExt* e = (Avm2SocketExt*) o->native_ext;
		if (e != NULL && e->xml_owner != NULL) avm2_gc_mark_object(e->xml_owner);
	}
	gc_mark_roots_file();
	gc_mark_roots_nc_queue();
	gc_mark_roots_lc();
	gc_mark_roots_netstream();
}

static void register_socket(Avm2Context* ctx)
{
	Avm2Class* cls = avm2_builtin_class(ctx, "flash.net", "Socket",
	                                    ctx->builtins.event_dispatcher_class);
	cls->instance_init.fn = socket_init;
	cls->instance_init.debug_name = "Socket";
	cls->native_ext_size = sizeof(Avm2SocketExt);
	g_socket_class = cls;
	avm2_bytearray_set_alt_resolver(socket_ba_resolve);

	avm2_builtin_add_method(ctx, cls, "connect", socket_connect);
	avm2_builtin_add_method(ctx, cls, "close", socket_close);
	avm2_builtin_add_method(ctx, cls, "flush", socket_flush);
	avm2_builtin_add_getter(ctx, cls, "connected", socket_get_connected);
	avm2_builtin_add_getter(ctx, cls, "bytesAvailable", socket_get_bytes_available);
	avm2_builtin_add_getter(ctx, cls, "bytesPending", socket_get_bytes_pending);
	avm2_builtin_add_getset(ctx, cls, "endian", socket_get_endian, socket_set_endian);
	avm2_builtin_add_getset(ctx, cls, "objectEncoding",
	                        socket_get_object_encoding, socket_set_object_encoding);
	avm2_builtin_add_getset(ctx, cls, "timeout",
	                        socket_get_timeout, socket_set_timeout);

	// The 28 IDataInput/IDataOutput bodies, shared verbatim with ByteArray.
	// Each one resolves its buffer through socket_ba_resolve, which is also
	// where the #2002 assert lives — that is the whole of `socket_errors`.
	avm2_bytearray_install_data_io(ctx, cls);
}

// ---------------------------------------------------------------------------
// flash.net.XMLSocket
// ---------------------------------------------------------------------------
//
// In Ruffle this is playerglobal AS3 (XMLSocket.as) wrapping a private
// flash.net.Socket; here it is the same wrapper written in C. Everything
// script-visible delegates to the inner Socket — including its errors, which
// is why close()/send() on an unconnected XMLSocket surface as IOError #2002
// and why `timeout` inherits Socket's 250ms floor.
//
// Inbound bytes are re-framed: XMLSocket reads the chunk one byte at a time,
// accumulating into a temp buffer and emitting a DataEvent at each NUL. The
// temp buffer persists across chunks, so a frame may span them; a trailing
// unterminated fragment is dropped by close().

typedef struct Avm2XmlSocketExt
{
	Avm2EventDispatcherExt dispatcher;  // extends EventDispatcher (MUST be first)
	Avm2Object* socket;                 // the private inner flash.net.Socket
	char* frame;                        // bytes since the last NUL
	size_t frame_len, frame_cap;
} Avm2XmlSocketExt;

static Avm2Class* g_xml_socket_class;

static Avm2XmlSocketExt* xmlsocket_ext(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	if (o == NULL || o->native_ext == NULL || g_xml_socket_class == NULL) return NULL;
	for (Avm2Class* c = o->cls; c != NULL; c = c->super_class)
		if (c == g_xml_socket_class) return (Avm2XmlSocketExt*) o->native_ext;
	return NULL;
}

static Avm2SocketExt* xmlsocket_inner(Avm2XmlSocketExt* x)
{
	if (x == NULL || x->socket == NULL) return NULL;
	return (Avm2SocketExt*) x->socket->native_ext;
}

static void xmlsocket_frame_push(Avm2XmlSocketExt* x, unsigned char b)
{
	if (x->frame_len == x->frame_cap)
	{
		size_t ncap = x->frame_cap ? x->frame_cap * 2 : 64;
		char* nf = (char*) realloc(x->frame, ncap);
		if (nf == NULL) return;
		x->frame = nf;
		x->frame_cap = ncap;
	}
	x->frame[x->frame_len++] = (char) b;
}

static void xmlsocket_relay(Avm2Context* ctx, Avm2Object* xml_obj,
                            Avm2SocketExt* sock, int action, size_t chunk_len)
{
	Avm2XmlSocketExt* x = (Avm2XmlSocketExt*) xml_obj->native_ext;
	if (x == NULL) return;

	if (action == SWF_SOCKET_CONNECT || action == SWF_SOCKET_CLOSE)
	{
		if (action == SWF_SOCKET_CLOSE) x->frame_len = 0;
		Avm2Object* ev = avm2_event_new(
			ctx, avm2_string_from_literal(
				ctx, action == SWF_SOCKET_CONNECT ? "connect" : "close"), 0, 0);
		if (ev != NULL) avm2_dispatch_event(ctx, xml_obj, ev);
		return;
	}
	if (action != SWF_SOCKET_DATA) return;

	// socketDataListener: exactly `bytesLoaded` readByte() calls, splitting at
	// every NUL. Re-read the buffer position each time — a DataEvent handler
	// may close the socket, and Ruffle's loop keeps reading its own chunk.
	for (size_t i = 0; i < chunk_len; i++)
	{
		if (sock->in_buf.position >= sock->in_buf.len) break;
		unsigned char b = sock->in_buf.bytes[sock->in_buf.position++];
		if (b != 0)
		{
			xmlsocket_frame_push(x, b);
			continue;
		}
		const Avm2String* s = avm2_string_new(
			ctx, x->frame != NULL ? x->frame : "", (uint32_t) x->frame_len);
		x->frame_len = 0;
		Avm2Object* ev = avm2_data_event_new(
			ctx, avm2_string_from_literal(ctx, "data"), 0, 0, s);
		if (ev != NULL) avm2_dispatch_event(ctx, xml_obj, ev);
	}
}

static Avm2Value xmlsocket_init(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* o = net2_this(act);
	Avm2XmlSocketExt* x = xmlsocket_ext(act);
	if (x == NULL) return avm2_undefined();
	x->frame = NULL;
	x->frame_len = 0;
	x->frame_cap = 0;

	Avm2Value sv = avm2_class_construct(ctx, g_socket_class, NULL, 0);
	x->socket = (sv.kind == AVM2_VALUE_OBJECT) ? sv.u.obj : NULL;
	Avm2SocketExt* s = xmlsocket_inner(x);
	if (s != NULL) s->xml_owner = o;
	// The inner Socket has no script-visible reference at all; root it now
	// rather than at connect() so an unconnected XMLSocket stays intact.
	socket_root(x->socket);

	if (act->argc > 0 && act->args[0].kind != AVM2_VALUE_NULL
	    && act->args[0].kind != AVM2_VALUE_UNDEFINED)
	{
		return avm2_call_public_property(ctx, act->this_val, "connect", 7,
		                                 act->args, act->argc);
	}
	return avm2_undefined();
}

static Avm2Value xmlsocket_connect(Avm2Activation* act)
{
	Avm2XmlSocketExt* x = xmlsocket_ext(act);
	if (x == NULL || x->socket == NULL) return avm2_undefined();
	// A null host falls back to the movie's domain, which for our file://
	// movies is "localhost" (Ruffle's XMLSocket.domain()).
	Avm2Value args[2];
	args[0] = (act->argc > 0 && act->args[0].kind != AVM2_VALUE_NULL)
		? act->args[0] : net2_str(act, "localhost");
	args[1] = act->argc > 1 ? act->args[1] : avm2_integer(0);
	return avm2_call_public_property(act->ctx, avm2_object_value(x->socket),
	                                 "connect", 7, args, 2);
}

static Avm2Value xmlsocket_close(Avm2Activation* act)
{
	Avm2XmlSocketExt* x = xmlsocket_ext(act);
	if (x == NULL || x->socket == NULL) return avm2_undefined();
	x->frame_len = 0;
	return avm2_call_public_property(act->ctx, avm2_object_value(x->socket),
	                                 "close", 5, NULL, 0);
}

static Avm2Value xmlsocket_send(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2XmlSocketExt* x = xmlsocket_ext(act);
	if (x == NULL || x->socket == NULL) return avm2_undefined();
	Avm2Value arg = act->argc > 0 ? act->args[0] : avm2_undefined();

	// XML / XMLList serialise with toXMLString(); everything else toString().
	// The distinction is load-bearing: <root>Hello!</root>.toString() is the
	// text node "Hello!", not the markup.
	const Avm2String* val;
	if (arg.kind == AVM2_VALUE_OBJECT && arg.u.obj != NULL
	    && (arg.u.obj->cls == ctx->builtins.xml_class
	        || arg.u.obj->cls == ctx->builtins.xml_list_class))
	{
		Avm2Value s = avm2_call_public_property(ctx, arg, "toXMLString", 11, NULL, 0);
		val = avm2_coerce_to_string(ctx, s);
	}
	else
	{
		val = avm2_coerce_to_string(ctx, arg);
	}

	Avm2Value sock = avm2_object_value(x->socket);
	Avm2Value a0 = avm2_string(val);
	avm2_call_public_property(ctx, sock, "writeUTFBytes", 13, &a0, 1);
	Avm2Value zero = avm2_integer(0);
	avm2_call_public_property(ctx, sock, "writeByte", 9, &zero, 1);
	avm2_call_public_property(ctx, sock, "flush", 5, NULL, 0);
	return avm2_undefined();
}

static Avm2Value xmlsocket_get_connected(Avm2Activation* act)
{
	Avm2SocketExt* s = xmlsocket_inner(xmlsocket_ext(act));
	return avm2_bool(socket_is_open(s));
}

static Avm2Value xmlsocket_get_timeout(Avm2Activation* act)
{
	Avm2SocketExt* s = xmlsocket_inner(xmlsocket_ext(act));
	return avm2_uint_value(s != NULL ? (uint32_t) s->timeout : 20000);
}

static Avm2Value xmlsocket_set_timeout(Avm2Activation* act)
{
	Avm2SocketExt* s = xmlsocket_inner(xmlsocket_ext(act));
	if (s == NULL) return avm2_undefined();
	// Delegates to Socket.timeout, so it inherits the 250ms floor — that is
	// what avm2/xml_socket's "Timeout clamp" block (20000, then 0 -> 250)
	// grades.
	uint32_t v = act->argc > 0 ? avm2_coerce_to_u32(act->ctx, act->args[0]) : 0;
	s->timeout = (v < 250) ? 250 : v;
	return avm2_undefined();
}

static void register_xml_socket(Avm2Context* ctx)
{
	Avm2Class* cls = avm2_builtin_class(ctx, "flash.net", "XMLSocket",
	                                    ctx->builtins.event_dispatcher_class);
	cls->instance_init.fn = xmlsocket_init;
	cls->instance_init.debug_name = "XMLSocket";
	cls->native_ext_size = sizeof(Avm2XmlSocketExt);
	g_xml_socket_class = cls;

	avm2_builtin_add_method(ctx, cls, "connect", xmlsocket_connect);
	avm2_builtin_add_method(ctx, cls, "close", xmlsocket_close);
	avm2_builtin_add_method(ctx, cls, "send", xmlsocket_send);
	avm2_builtin_add_getter(ctx, cls, "connected", xmlsocket_get_connected);
	avm2_builtin_add_getset(ctx, cls, "timeout",
	                        xmlsocket_get_timeout, xmlsocket_set_timeout);
}

// ---------------------------------------------------------------------------
// flash.net.NetConnection
// ---------------------------------------------------------------------------

enum
{
	NC_PROTO_NONE = 0,      // never connected, or closed
	NC_PROTO_LOCAL = 1,     // connect(null)
	NC_PROTO_REMOTING = 2,  // connect("http(s)://…")
};

typedef struct Avm2NetConnectionExt
{
	Avm2EventDispatcherExt dispatcher;  // extends EventDispatcher (MUST be first)
	Avm2Value client;                   // defaults to the connection itself
	const Avm2String* uri;              // NULL unless a connection is open
	const Avm2String* proxy_type;       // NULL == "none"
	uint32_t object_encoding;
	uint32_t max_peer_connections;
	uint8_t protocol;
} Avm2NetConnectionExt;

static Avm2Class* g_net_connection_class;
static uint32_t g_nc_default_object_encoding = 3;

static Avm2NetConnectionExt* nc_ext(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	return (o != NULL && o->native_ext != NULL)
		? (Avm2NetConnectionExt*) o->native_ext : NULL;
}

static _Noreturn void nc_throw_2126(Avm2Context* ctx)
{
	avm2_throw_error(ctx, ctx->builtins.argument_error_class,
	                 "Error #2126: NetConnection object must be connected.");
}

// Every accessor that is only meaningful on an open connection. Ruffle models
// this as `handle().and_then(|h| …)` returning None, and every None arm is
// make_error_2126 — except `uri`, which returns null instead (get_uri).
static Avm2NetConnectionExt* nc_require_open(Avm2Activation* act)
{
	Avm2NetConnectionExt* e = nc_ext(act);
	if (e == NULL || e->protocol == NC_PROTO_NONE) nc_throw_2126(act->ctx);
	return e;
}

// Only a "local" (connect(null)) connection answers the peer/protocol block;
// a Flash Remoting one reports None for all of it and so throws too.
static Avm2NetConnectionExt* nc_require_local(Avm2Activation* act)
{
	Avm2NetConnectionExt* e = nc_require_open(act);
	if (e->protocol != NC_PROTO_LOCAL) nc_throw_2126(act->ctx);
	return e;
}

static void nc_dispatch_status(Avm2Context* ctx, Avm2Object* self,
                               const char* code)
{
	static const char* const keys[2] = { "code", "level" };
	const char* values[2];
	values[0] = code;
	values[1] = "status";
	Avm2Object* ev = avm2_net_status_event_new(ctx, keys, values, 2);
	if (ev != NULL) avm2_dispatch_event(ctx, self, ev);
}

// [NA, Ruffle net_connection.rs] "I have no idea why, but a NetConnection
// receives a second and nonsensical event on close" — an explicit close() of a
// Flash Remoting connection fires a NetStatusEvent whose code, description and
// details are all the empty string.
static void nc_dispatch_empty_status(Avm2Context* ctx, Avm2Object* self)
{
	static const char* const keys[4] = { "code", "description", "details", "level" };
	static const char* const values[4] = { "", "", "", "status" };
	Avm2Object* ev = avm2_net_status_event_new(ctx, keys, values, 4);
	if (ev != NULL) avm2_dispatch_event(ctx, self, ev);
}

// A close (or a re-connect) drops the queued calls AND the accumulated headers:
// netconnection_send_remote's test 4 packet carries zero headers even though
// test 3 added two. Defined with the queue below.
static void nc_reset_conn(Avm2Object* nc);

// NetConnections::close. `is_explicit` distinguishes a user close() from the
// implicit teardown that a second connect() performs on the previous handle;
// only the explicit one gets the second event.
static void nc_close_open(Avm2Context* ctx, Avm2Object* self,
                          Avm2NetConnectionExt* e, int is_explicit)
{
	if (e->protocol == NC_PROTO_NONE) return;
	int was_remoting = (e->protocol == NC_PROTO_REMOTING);
	e->protocol = NC_PROTO_NONE;
	e->uri = NULL;
	nc_reset_conn(self);
	nc_dispatch_status(ctx, self, "NetConnection.Connect.Closed");
	if (is_explicit && was_remoting) nc_dispatch_empty_status(ctx, self);
}

static int nc_url_is_http(const Avm2String* s)
{
	// Ruffle lowercases the whole URL and then checks both prefixes.
	static const char http[7] = { 'h', 't', 't', 'p', ':', '/', '/' };
	static const char https[8] = { 'h', 't', 't', 'p', 's', ':', '/', '/' };
	for (int pass = 0; pass < 2; pass++)
	{
		const char* pfx = pass ? https : http;
		uint32_t n = pass ? 8u : 7u;
		if (s->len < n) continue;
		uint32_t i = 0;
		for (; i < n; i++)
		{
			char c = s->utf8[i];
			if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
			if (c != pfx[i]) break;
		}
		if (i == n) return 1;
	}
	return 0;
}

static Avm2Value nc_init(Avm2Activation* act)
{
	Avm2NetConnectionExt* e = nc_ext(act);
	if (e == NULL) return avm2_undefined();
	e->protocol = NC_PROTO_NONE;
	e->uri = NULL;
	e->proxy_type = NULL;
	e->object_encoding = g_nc_default_object_encoding;
	e->max_peer_connections = 8;
	// NetConnection.as: `private var _client:Object = this;`
	e->client = act->this_val;
	return avm2_undefined();
}

static Avm2Value nc_connect(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* self = net2_this(act);
	Avm2NetConnectionExt* e = nc_ext(act);
	if (self == NULL || e == NULL) return avm2_undefined();

	// try_get_string(0): null/undefined/absent all mean "no command".
	const Avm2String* url = NULL;
	if (act->argc > 0 && act->args[0].kind != AVM2_VALUE_NULL
	    && act->args[0].kind != AVM2_VALUE_UNDEFINED)
	{
		url = avm2_coerce_to_string(ctx, act->args[0]);
	}

	if (url == NULL)
	{
		// connect_to_local: the previous handle (if any) is closed
		// NON-explicitly, then the success event fires.
		nc_close_open(ctx, self, e, 0);
		e->protocol = NC_PROTO_LOCAL;
		e->uri = avm2_string_from_literal(ctx, "null");  // yes, the string
		nc_dispatch_status(ctx, self, "NetConnection.Connect.Success");
	}
	else if (nc_url_is_http(url))
	{
		// connect_to_flash_remoting: same non-explicit teardown, and
		// deliberately NO open event.
		nc_close_open(ctx, self, e, 0);
		e->protocol = NC_PROTO_REMOTING;
		e->uri = url;
	}
	// Any other command (rtmp://…, a bare name) is Ruffle's stub arm: no
	// connection is opened and no event fires.
	return avm2_undefined();
}

static Avm2Value nc_close(Avm2Activation* act)
{
	Avm2Object* self = net2_this(act);
	Avm2NetConnectionExt* e = nc_ext(act);
	if (self != NULL && e != NULL) nc_close_open(act->ctx, self, e, 1);
	return avm2_undefined();
}

static Avm2Value nc_get_connected(Avm2Activation* act)
{
	Avm2NetConnectionExt* e = nc_ext(act);
	return avm2_bool(e != NULL && e->protocol == NC_PROTO_LOCAL);
}

static Avm2Value nc_get_connected_proxy_type(Avm2Activation* act)
{ nc_require_local(act); return net2_str(act, "none"); }
static Avm2Value nc_get_far_id(Avm2Activation* act)
{ nc_require_local(act); return net2_str(act, ""); }
static Avm2Value nc_get_near_id(Avm2Activation* act)
{ nc_require_local(act); return net2_str(act, ""); }
static Avm2Value nc_get_far_nonce(Avm2Activation* act)
{
	nc_require_local(act);
	return net2_str(act,
		"0000000000000000000000000000000000000000000000000000000000000000");
}
static Avm2Value nc_get_near_nonce(Avm2Activation* act)
{
	nc_require_local(act);
	return net2_str(act,
		"0000000000000000000000000000000000000000000000000000000000000000");
}
static Avm2Value nc_get_protocol(Avm2Activation* act)
{ nc_require_local(act); return net2_str(act, "rtmp"); }
static Avm2Value nc_get_using_tls(Avm2Activation* act)
{ nc_require_local(act); return avm2_bool(0); }

// unconnectedPeerStreams is written in AS3 (NetConnection.as), so it is gated
// on `connected` — i.e. on the LOCAL protocol — not on "a handle exists".
static Avm2Value nc_get_unconnected_peer_streams(Avm2Activation* act)
{
	Avm2NetConnectionExt* e = nc_ext(act);
	if (e == NULL || e->protocol != NC_PROTO_LOCAL) nc_throw_2126(act->ctx);
	return avm2_object_value(avm2_array_new(act->ctx, 0));
}

// get_uri is the one accessor whose "no connection" arm is null, not #2126.
static Avm2Value nc_get_uri(Avm2Activation* act)
{
	Avm2NetConnectionExt* e = nc_ext(act);
	return (e != NULL && e->uri != NULL) ? avm2_string(e->uri) : avm2_null();
}

static Avm2Value nc_get_client(Avm2Activation* act)
{ Avm2NetConnectionExt* e = nc_ext(act); return e ? e->client : avm2_undefined(); }
static Avm2Value nc_set_client(Avm2Activation* act)
{
	Avm2NetConnectionExt* e = nc_ext(act);
	if (e != NULL && act->argc > 0) e->client = act->args[0];
	return avm2_undefined();
}

static Avm2Value nc_get_object_encoding(Avm2Activation* act)
{ Avm2NetConnectionExt* e = nc_ext(act); return avm2_uint_value(e ? e->object_encoding : 3); }
static Avm2Value nc_set_object_encoding(Avm2Activation* act)
{
	Avm2NetConnectionExt* e = nc_ext(act);
	if (e != NULL && act->argc > 0)
		e->object_encoding = avm2_coerce_to_u32(act->ctx, act->args[0]);
	return avm2_undefined();
}

static Avm2Value nc_get_max_peer_connections(Avm2Activation* act)
{ Avm2NetConnectionExt* e = nc_ext(act); return avm2_uint_value(e ? e->max_peer_connections : 8); }
static Avm2Value nc_set_max_peer_connections(Avm2Activation* act)
{
	Avm2NetConnectionExt* e = nc_ext(act);
	if (e != NULL && act->argc > 0)
		e->max_peer_connections = avm2_coerce_to_u32(act->ctx, act->args[0]);
	return avm2_undefined();
}

static Avm2Value nc_get_proxy_type(Avm2Activation* act)
{
	Avm2NetConnectionExt* e = nc_ext(act);
	return (e != NULL && e->proxy_type != NULL) ? avm2_string(e->proxy_type)
	                                            : net2_str(act, "none");
}
static Avm2Value nc_set_proxy_type(Avm2Activation* act)
{
	Avm2NetConnectionExt* e = nc_ext(act);
	if (e != NULL && act->argc > 0)
		e->proxy_type = avm2_coerce_to_string(act->ctx, act->args[0]);
	return avm2_undefined();
}

// ---------------------------------------------------------------------------
// NetConnection.call / addHeader — the Flash Remoting wire (tranche 8)
// ---------------------------------------------------------------------------
//
// The packet framing is src/amf_packet.c, shared verbatim with AVM1 (tranche 7)
// because it knows nothing about either VM: it frames byte ranges the caller has
// already serialized. The AVM2 differences from the AVM1 path are all at the
// edges:
//
//   * the arguments go through avm2_amf.c's writers, and an objectEncoding of
//     AMF3 sets the packet's version u16 to 3 and prefixes every argument with
//     the AMF0 0x11 avmplus escape (amf_array_serialization pins both bodies);
//   * a response dispatches to the Responder's result/status CLOSURE rather than
//     to an onResult/onStatus property;
//   * a failed fetch is a NetStatusEvent — code NetConnection.Call.Failed,
//     description "HTTP: Failed", details the URL, level "error" — not AVM1's
//     zero-argument onStatus.
//
// Queue drain: once per tick from avm2_display_run_tick, at the loader/executor
// drain point (Ruffle's harness polls the async executor there, so the fetch log
// and the response callbacks both land after the calling frame's traces).

#define NC_MAX_CONNS 16
#define NC_QUEUE 32
#define NC_HEADERS 16
#define NC_DRAIN_ROUNDS 8

typedef struct NcMessage
{
	char target[AMF_PACKET_MAX_NAME];
	unsigned char* body;     // AMF0 StrictArray of the call arguments
	size_t body_len;
	Avm2Object* responder;   // NULL when the call passed null/undefined
} NcMessage;

typedef struct NcHeader
{
	char name[AMF_PACKET_MAX_NAME];
	int must_understand;
	unsigned char* value;
	size_t value_len;
} NcHeader;

typedef struct NcConn
{
	Avm2Object* nc;
	NcHeader headers[NC_HEADERS];
	int header_count;
	NcMessage msgs[NC_QUEUE];
	int msg_count;
} NcConn;

static NcConn g_nc_conns[NC_MAX_CONNS];
static int g_nc_conn_count;

static NcConn* nc_conn_of(Avm2Object* nc, int create)
{
	for (int i = 0; i < g_nc_conn_count; i++)
		if (g_nc_conns[i].nc == nc) return &g_nc_conns[i];
	if (!create || g_nc_conn_count >= NC_MAX_CONNS) return NULL;
	NcConn* c = &g_nc_conns[g_nc_conn_count++];
	memset(c, 0, sizeof(*c));
	c->nc = nc;
	return c;
}

// The remoting endpoint, as a NUL-terminated copy. 0 when this connection
// cannot carry a call (never connected, or connect(null)).
static int nc_uri_of(Avm2NetConnectionExt* e, char* out, size_t cap)
{
	if (e == NULL || e->protocol != NC_PROTO_REMOTING || e->uri == NULL) return 0;
	if (e->uri->len + 1 > cap) return 0;
	memcpy(out, e->uri->utf8, e->uri->len);
	out[e->uri->len] = '\0';
	return 1;
}

// One call's arguments, as the synthetic AMF0 StrictArray Flash always wraps
// them in — even for a call with no arguments (`0A 00 00 00 00`). Each argument
// is serialized on its own, with its own reference tables.
static unsigned char* nc_build_args(Avm2Context* ctx, const Avm2Value* args,
                                    uint32_t first, uint32_t argc,
                                    int amf3, size_t* out_len)
{
	AmfBuf b;
	amf_buf_init(&b);
	uint32_t n = (argc > first) ? argc - first : 0;
	amf_buf_u8(&b, 0x0A);
	amf_buf_u32be(&b, n);
	for (uint32_t i = first; i < argc; i++)
	{
		size_t len = 0;
		unsigned char* one = amf3
			? avm2_amf3_write_value_tagged(ctx, args[i], &len)
			: avm2_amf0_write_value(ctx, args[i], &len);
		if (one != NULL) amf_buf_put(&b, one, len);
		free(one);
	}
	if (out_len != NULL) *out_len = b.len;
	return b.data;
}

static Avm2Value nc_call(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2NetConnectionExt* e = nc_require_open(act);
	Avm2Object* self = net2_this(act);
	if (self == NULL || act->argc < 1) return avm2_undefined();

	char url[512];
	if (!nc_uri_of(e, url, sizeof(url))) return avm2_undefined();

	const Avm2String* target = avm2_coerce_to_string(ctx, act->args[0]);
	NcConn* c = nc_conn_of(self, 1);
	if (c == NULL || c->msg_count >= NC_QUEUE) return avm2_undefined();

	NcMessage* m = &c->msgs[c->msg_count];
	uint32_t tn = target->len;
	if (tn >= sizeof(m->target)) tn = sizeof(m->target) - 1;
	memcpy(m->target, target->utf8, tn);
	m->target[tn] = '\0';
	m->responder = NULL;
	if (act->argc > 1 && act->args[1].kind == AVM2_VALUE_OBJECT)
		m->responder = act->args[1].u.obj;
	m->body = nc_build_args(ctx, act->args, 2, act->argc,
	                        e->object_encoding == 3, &m->body_len);
	c->msg_count++;
	return avm2_undefined();
}

static Avm2Value nc_add_header(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2NetConnectionExt* e = nc_ext(act);
	Avm2Object* self = net2_this(act);
	if (self == NULL || e == NULL || act->argc < 1) return avm2_undefined();

	const Avm2String* name = avm2_coerce_to_string(ctx, act->args[0]);
	NcConn* c = nc_conn_of(self, 1);
	if (c == NULL) return avm2_undefined();

	// addHeader(operation:String, mustUnderstand:Boolean = false,
	// param:Object = null) — the AS3 signature's default is FALSE, and
	// netconnection_send_remote's bare addHeader("Duplicate") puts a 00 in the
	// packet. (AVM1's own addHeader defaults the other way; tranche 7 pinned
	// that from Flash's bytes too.)
	int must = (act->argc > 1)
		? (avm2_coerce_to_boolean(act->args[1]) ? 1 : 0) : 0;

	// The value is serialized NOW, at addHeader time, and the header is then
	// re-sent with every packet until it is replaced. A MISSING value is null,
	// not undefined: netconnection_send_remote's bare `addHeader("Duplicate")`
	// puts an 0x05 in the packet.
	Avm2Value vv = (act->argc > 2) ? act->args[2] : avm2_null();
	size_t vlen = 0;
	unsigned char* val = (e->object_encoding == 3)
		? avm2_amf3_write_value_tagged(ctx, vv, &vlen)
		: avm2_amf0_write_value(ctx, vv, &vlen);

	char nbuf[AMF_PACKET_MAX_NAME];
	uint32_t nn = name->len;
	if (nn >= sizeof(nbuf)) nn = sizeof(nbuf) - 1;
	memcpy(nbuf, name->utf8, nn);
	nbuf[nn] = '\0';

	// One header per name, matched ASCII-case-insensitively and replaced in
	// place so the packet order does not change.
	NcHeader* slot = NULL;
	for (int i = 0; i < c->header_count; i++)
	{
		const char* a = c->headers[i].name;
		const char* b = nbuf;
		int same = 1;
		for (size_t k = 0;; k++)
		{
			char ca = a[k], cb = b[k];
			if (ca >= 'A' && ca <= 'Z') ca = (char) (ca - 'A' + 'a');
			if (cb >= 'A' && cb <= 'Z') cb = (char) (cb - 'A' + 'a');
			if (ca != cb) { same = 0; break; }
			if (ca == '\0') break;
		}
		if (same) { slot = &c->headers[i]; break; }
	}
	if (slot == NULL)
	{
		if (c->header_count >= NC_HEADERS) { free(val); return avm2_undefined(); }
		slot = &c->headers[c->header_count++];
		slot->value = NULL;
	}
	memcpy(slot->name, nbuf, nn + 1);
	slot->must_understand = must;
	free(slot->value);
	slot->value = val;
	slot->value_len = vlen;
	return avm2_undefined();
}

static Avm2Value nc_static_get_default_encoding(Avm2Activation* act)
{ (void) act; return avm2_uint_value(g_nc_default_object_encoding); }
static Avm2Value nc_static_set_default_encoding(Avm2Activation* act)
{
	if (act->argc > 0)
		g_nc_default_object_encoding = avm2_coerce_to_u32(act->ctx, act->args[0]);
	return avm2_undefined();
}

static void register_net_connection(Avm2Context* ctx)
{
	Avm2Class* cls = avm2_builtin_class(ctx, "flash.net", "NetConnection",
	                                    ctx->builtins.event_dispatcher_class);
	cls->instance_init.fn = nc_init;
	cls->instance_init.debug_name = "NetConnection";
	cls->native_ext_size = sizeof(Avm2NetConnectionExt);
	g_net_connection_class = cls;

	avm2_builtin_add_method(ctx, cls, "connect", nc_connect);
	avm2_builtin_add_method(ctx, cls, "close", nc_close);
	avm2_builtin_add_method(ctx, cls, "call", nc_call);
	avm2_builtin_add_method(ctx, cls, "addHeader", nc_add_header);

	avm2_builtin_add_getter(ctx, cls, "connected", nc_get_connected);
	avm2_builtin_add_getter(ctx, cls, "connectedProxyType",
	                        nc_get_connected_proxy_type);
	avm2_builtin_add_getter(ctx, cls, "farID", nc_get_far_id);
	avm2_builtin_add_getter(ctx, cls, "farNonce", nc_get_far_nonce);
	avm2_builtin_add_getter(ctx, cls, "nearID", nc_get_near_id);
	avm2_builtin_add_getter(ctx, cls, "nearNonce", nc_get_near_nonce);
	avm2_builtin_add_getter(ctx, cls, "protocol", nc_get_protocol);
	avm2_builtin_add_getter(ctx, cls, "usingTLS", nc_get_using_tls);
	avm2_builtin_add_getter(ctx, cls, "unconnectedPeerStreams",
	                        nc_get_unconnected_peer_streams);
	avm2_builtin_add_getter(ctx, cls, "uri", nc_get_uri);
	avm2_builtin_add_getset(ctx, cls, "client", nc_get_client, nc_set_client);
	avm2_builtin_add_getset(ctx, cls, "objectEncoding",
	                        nc_get_object_encoding, nc_set_object_encoding);
	avm2_builtin_add_getset(ctx, cls, "maxPeerConnections",
	                        nc_get_max_peer_connections, nc_set_max_peer_connections);
	avm2_builtin_add_getset(ctx, cls, "proxyType",
	                        nc_get_proxy_type, nc_set_proxy_type);
	avm2_builtin_add_static_getset(ctx, cls, "defaultObjectEncoding",
	                               nc_static_get_default_encoding,
	                               nc_static_set_default_encoding);
}

// ---------------------------------------------------------------------------
// flash.net.Responder
// ---------------------------------------------------------------------------
//
// Responder(result:Function, status:Function = null). The callbacks are
// reachable only from a NetConnection.call response: the packet's "/N/onResult"
// or "/N/onStatus" target picks which of the pair runs (see nc_flush_one).

typedef struct Avm2ResponderExt
{
	Avm2Value result_fn;
	Avm2Value status_fn;
} Avm2ResponderExt;

static Avm2Class* g_responder_class;

static Avm2Value responder_init(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	if (o == NULL || o->native_ext == NULL) return avm2_undefined();
	Avm2ResponderExt* e = (Avm2ResponderExt*) o->native_ext;
	e->result_fn = act->argc > 0 ? act->args[0] : avm2_null();
	e->status_fn = act->argc > 1 ? act->args[1] : avm2_null();
	return avm2_undefined();
}

// ---------------------------------------------------------------------------
// NetConnection queue drain (needs Avm2ResponderExt, hence its position here)
// ---------------------------------------------------------------------------

static void nc_clear_queue(NcConn* c)
{
	for (int i = 0; i < c->msg_count; i++)
	{
		free(c->msgs[i].body);
		c->msgs[i].body = NULL;
		c->msgs[i].responder = NULL;
	}
	c->msg_count = 0;
}

// The connection and every queued call's Responder are reachable only from this
// file's C table between the call and the drain.
static void gc_mark_roots_nc_queue(void)
{
	for (int i = 0; i < g_nc_conn_count; i++)
	{
		avm2_gc_mark_object(g_nc_conns[i].nc);
		for (int j = 0; j < g_nc_conns[i].msg_count; j++)
			avm2_gc_mark_object(g_nc_conns[i].msgs[j].responder);
	}
}

static void nc_reset_conn(Avm2Object* nc)
{
	NcConn* c = nc_conn_of(nc, 0);
	if (c == NULL) return;
	nc_clear_queue(c);
	for (int i = 0; i < c->header_count; i++)
	{
		free(c->headers[i].value);
		c->headers[i].value = NULL;
	}
	c->header_count = 0;
}

static Avm2ResponderExt* responder_ext_of(Avm2Object* o)
{
	if (o == NULL || o->native_ext == NULL || g_responder_class == NULL) return NULL;
	for (const Avm2Class* c = o->cls; c != NULL; c = c->super_class)
		if (c == g_responder_class) return (Avm2ResponderExt*) o->native_ext;
	return NULL;
}

typedef struct NcRespCtx
{
	Avm2Context* ctx;
	NcMessage* msgs;   // snapshot of the flushed batch
	int msg_count;
} NcRespCtx;

// A response target is "/<1-based responder index>/onResult" or ".../onStatus".
// Anything else (or an index with no responder behind it) is ignored, which is
// what Flash does with a bogus response.
static int nc_response_msg(const char* target, size_t target_len,
                           const unsigned char* body, size_t body_len,
                           void* user)
{
	NcRespCtx* rc = (NcRespCtx*) user;
	if (target == NULL || target_len < 4 || target[0] != '/') return 0;
	size_t i = 1;
	long idx = 0;
	if (target[i] < '0' || target[i] > '9') return 0;
	while (i < target_len && target[i] >= '0' && target[i] <= '9')
	{
		idx = idx * 10 + (target[i] - '0');
		i++;
	}
	if (i >= target_len || target[i] != '/') return 0;
	i++;
	size_t mlen = target_len - i;
	const char* method = target + i;
	int is_result = (mlen == 8 && memcmp(method, "onResult", 8) == 0);
	int is_status = (mlen == 8 && memcmp(method, "onStatus", 8) == 0);
	if (!is_result && !is_status) return 0;
	if (idx < 1 || idx > rc->msg_count) return 0;

	Avm2ResponderExt* re = responder_ext_of(rc->msgs[idx - 1].responder);
	if (re == NULL) return 0;
	Avm2Value fn = is_result ? re->result_fn : re->status_fn;
	if (fn.kind != AVM2_VALUE_OBJECT) return 0;

	Avm2Value arg = avm2_amf0_read_value(rc->ctx, body, body_len);
	avm2_call_value(rc->ctx, fn, avm2_null(), &arg, 1);
	return 0;
}

// Resolve a remoting URL to a bundled scripted response packet. Tests keep those
// beside the SWF (netconnection_send_remote's localhost/test1..3), and
// findDataFile is keyed by the bare filename as well as the relative path, so
// both spellings are worth trying.
static DataFileEntry* nc_resolve_response(const char* url)
{
	const char* scheme = strstr(url, "://");
	const char* rest = (scheme != NULL) ? scheme + 3 : url;
	const char* slash = strchr(rest, '/');
	if (slash == NULL || slash[1] == '\0') return NULL;

	// "localhost:8000/test1" -> try "localhost/test1" then "test1".
	char rel[512];
	size_t hostlen = (size_t) (slash - rest);
	const char* colon = memchr(rest, ':', hostlen);
	size_t hnlen = (colon != NULL) ? (size_t) (colon - rest) : hostlen;
	if (hnlen + 1 + strlen(slash + 1) + 1 <= sizeof(rel))
	{
		memcpy(rel, rest, hnlen);
		rel[hnlen] = '/';
		snprintf(rel + hnlen + 1, sizeof(rel) - hnlen - 1, "%s", slash + 1);
		DataFileEntry* d = findDataFile(rel);
		if (d != NULL && d->content != NULL) return d;
	}
	const char* base = strrchr(url, '/');
	base = (base != NULL) ? base + 1 : url;
	if (base[0] == '\0') return NULL;
	DataFileEntry* d = findDataFile(base);
	if (d != NULL && d->content != NULL) return d;
	return NULL;
}

// A fetch that resolves to nothing is a failed request. Ruffle's
// NetConnection::on_fetch_error path: code NetConnection.Call.Failed,
// description "HTTP: Failed", details the URL, level "error".
static void nc_dispatch_call_failed(Avm2Context* ctx, Avm2Object* self,
                                    const char* url)
{
	static const char* const keys[4] = { "code", "description", "details", "level" };
	const char* values[4];
	values[0] = "NetConnection.Call.Failed";
	values[1] = "HTTP: Failed";
	values[2] = url;
	values[3] = "error";
	Avm2Object* ev = avm2_net_status_event_new(ctx, keys, values, 4);
	if (ev != NULL) avm2_dispatch_event(ctx, self, ev);
}

static void nc_flush_one(Avm2Context* ctx, NcConn* c)
{
	Avm2NetConnectionExt* e = (c->nc != NULL && c->nc->native_ext != NULL)
		? (Avm2NetConnectionExt*) c->nc->native_ext : NULL;
	char url[512];
	if (!nc_uri_of(e, url, sizeof(url)))
	{
		// The connection closed between the call and the drain: Flash drops the
		// queued messages.
		nc_clear_queue(c);
		return;
	}

	// Take the batch out of the queue first: a responder callback may issue more
	// calls, and those belong to the NEXT packet.
	NcMessage batch[NC_QUEUE];
	int count = c->msg_count;
	memcpy(batch, c->msgs, (size_t) count * sizeof(NcMessage));
	c->msg_count = 0;

	AmfPacketHeader hdrs[NC_HEADERS];
	for (int i = 0; i < c->header_count; i++)
	{
		snprintf(hdrs[i].name, sizeof(hdrs[i].name), "%s", c->headers[i].name);
		hdrs[i].must_understand = c->headers[i].must_understand;
		hdrs[i].value = c->headers[i].value;
		hdrs[i].value_len = c->headers[i].value_len;
	}

	// Response URIs are "/1".."/N", numbered per FLUSH, not per connection
	// lifetime (netconnection_send_remote test 3 batches two calls as /1 and /2
	// after earlier flushes already used /1).
	AmfPacketMessage pmsgs[NC_QUEUE];
	for (int i = 0; i < count; i++)
	{
		snprintf(pmsgs[i].target, sizeof(pmsgs[i].target), "%s", batch[i].target);
		snprintf(pmsgs[i].response, sizeof(pmsgs[i].response), "/%d", i + 1);
		pmsgs[i].body = batch[i].body;
		pmsgs[i].body_len = batch[i].body_len;
	}

	AmfBuf packet;
	amf_buf_init(&packet);
	amf_packet_build(&packet, (e->object_encoding == 3) ? 3u : 0u,
	                 hdrs, (size_t) c->header_count, pmsgs, (size_t) count);

#if SWF_LOG_FETCH_ENABLED
	// Ruffle logs inside fetch(), i.e. before any of the response's events, so
	// the block is queued and flushed right here — ahead of the callbacks below.
	swf_log_fetch_queue(url, strlen(url), "POST", 4, NULL, 0,
	                    "application/x-amf", 17,
	                    packet.data, packet.len, 1, 0);
	swf_log_fetch_flush();
#endif

	DataFileEntry* resp = nc_resolve_response(url);
	if (resp != NULL)
	{
		NcRespCtx rc;
		rc.ctx = ctx;
		rc.msgs = batch;
		rc.msg_count = count;
		amf_packet_parse((const unsigned char*) resp->content,
		                 (size_t) resp->content_length, nc_response_msg, &rc);
	}
	else
	{
		nc_dispatch_call_failed(ctx, c->nc, url);
	}

	amf_buf_free(&packet);
	for (int i = 0; i < count; i++) free(batch[i].body);
}

// Per-tick drain, called from avm2_display_run_tick's loader/executor drain
// point. Synchronous and exhaustive, so nothing is ever left pending across a
// tick boundary (a responder callback that issues more calls gets its own
// packet, up to NC_DRAIN_ROUNDS).
void avm2_net_flush_connections(Avm2Context* ctx)
{
	for (int round = 0; round < NC_DRAIN_ROUNDS; round++)
	{
		int did = 0;
		for (int i = 0; i < g_nc_conn_count; i++)
		{
			if (g_nc_conns[i].msg_count == 0) continue;
			nc_flush_one(ctx, &g_nc_conns[i]);
			did = 1;
		}
		if (!did) return;
	}
}

// ---------------------------------------------------------------------------
// flash.net.LocalConnection (tranche 6)
// ---------------------------------------------------------------------------
//
// Ruffle's model (core/src/local_connection.rs +
// avm2/object/local_connection_object.rs), which is a per-PLAYER channel map,
// not any kind of IPC: every endpoint the corpus exercises lives in the same
// player (the main movie plus the child SWFs the Loader arc executes), so a
// send is a queued in-process message.
//
//   * The channel key is the connection name lowercased, prefixed with
//     "<superdomain>:" unless it starts with '_' (a global channel). A `connect`
//     name may not contain ':' at all; a `send` name may, and then carries its
//     own domain.
//   * `send` decides "is there a listener" TWICE: once now (no listener ->
//     queue a plain failure) and once at delivery (gone by then -> failure
//     too). Whatever holds the name at delivery is what receives.
//   * Delivery is next-tick, in queue order, and each message dispatches the
//     SENDER's StatusEvent (level "status"/"error", code always null) before
//     invoking the callee. A callee that throws produces an AsyncErrorEvent on
//     the RECEIVER — not on the sender (avm2/localconnection grades that
//     directly: "receiver received event AsyncErrorEvent.ASYNC_ERROR").
//   * Arguments cross as AMF0, serialized at send and deserialized at delivery,
//     so the callee gets a deep copy with Flash's type translations. That is the
//     same wire the NetConnection packet above uses.
//
// The AVM1 half of the registry lives in action.c, with its own channel map and
// its own AMF0 codec, and is BRIDGED here rather than merged: Flash's registry
// belongs to the player, not to a VM, so a send that finds no AVM2 listener
// consults the AVM1 map before it fails. Both key builders emit the same
// `superdomain:name` lowercased string and both halves put a bare AMF0 value in
// each argument buffer, so a message crosses untouched. mixed_avm/
// avm2_loads_avm1_v9 and _v10 grade the AVM2->AVM1 direction; the reverse enters
// through avm2_net_local_connection_deliver_from_avm1, at the end of this
// section.
extern int actionAvm1LocalConnectionHasChannel(const char* key);
extern int actionAvm1LocalConnectionDeliver(SWFAppContext* app_context,
                                            const char* key, const char* method,
                                            unsigned char* const* args,
                                            const size_t* arg_len, int argc);

#define LC_MAX_CHANNELS 32
#define LC_MAX_QUEUE 64
#define LC_MAX_ARGS 16
#define LC_MAX_NAME 256

typedef struct Avm2LocalConnectionExt
{
	Avm2EventDispatcherExt dispatcher;  // extends EventDispatcher (MUST be first)
	Avm2Value client;                   // defaults to the connection itself
	char key[LC_MAX_NAME];              // registered channel key ("" = closed)
} Avm2LocalConnectionExt;

typedef struct LcChannel
{
	char key[LC_MAX_NAME];
	Avm2Object* obj;
} LcChannel;

typedef struct LcMessage
{
	Avm2Object* sender;
	char key[LC_MAX_NAME];       // resolved channel key
	char method[LC_MAX_NAME];    // callee method name (UTF-8)
	unsigned char* args[LC_MAX_ARGS];
	size_t arg_len[LC_MAX_ARGS];
	int argc;
	int failure;                 // 1 = no listener at send time
} LcMessage;

static Avm2Class* g_local_connection_class;
static LcChannel g_lc_channels[LC_MAX_CHANNELS];
static int g_lc_channel_count;
static LcMessage g_lc_queue[LC_MAX_QUEUE];
static int g_lc_queue_count;

static Avm2LocalConnectionExt* lc_ext_of(Avm2Object* o)
{
	if (o == NULL || o->native_ext == NULL || g_local_connection_class == NULL)
		return NULL;
	for (const Avm2Class* c = o->cls; c != NULL; c = c->super_class)
		if (c == g_local_connection_class) return (Avm2LocalConnectionExt*) o->native_ext;
	return NULL;
}

static Avm2LocalConnectionExt* lc_ext(Avm2Activation* act)
{ return lc_ext_of(net2_this(act)); }

// The domain a name with no explicit one belongs to: everything after the LAST
// dot of the movie's domain (Ruffle get_superdomain).
static const char* lc_superdomain(void)
{
	const char* d = avm2_local_connection_domain();
	const char* dot = strrchr(d, '.');
	return (dot != NULL) ? dot + 1 : d;
}

static void lc_lower(char* s)
{
	for (; *s != '\0'; s++)
		if (*s >= 'A' && *s <= 'Z') *s = (char) (*s - 'A' + 'a');
}

// connect(): '_'-prefixed names are global, everything else is scoped to the
// superdomain. (A ':' is rejected before this by the caller.)
static void lc_connect_key(const Avm2String* name, char* out, size_t cap)
{
	if (name->len > 0 && name->utf8[0] == '_')
		snprintf(out, cap, "%.*s", (int) name->len, name->utf8);
	else
		snprintf(out, cap, "%s:%.*s", lc_superdomain(), (int) name->len, name->utf8);
	lc_lower(out);
}

// send(): a name that already carries a ':' is used as-is; otherwise the same
// rule as connect().
static void lc_send_key(const Avm2String* name, char* out, size_t cap)
{
	char buf[LC_MAX_NAME];
	snprintf(buf, sizeof(buf), "%.*s", (int) name->len, name->utf8);
	lc_lower(buf);
	if (strchr(buf, ':') != NULL || buf[0] == '_')
		snprintf(out, cap, "%s", buf);
	else
		snprintf(out, cap, "%s:%s", lc_superdomain(), buf);
	lc_lower(out);
}

static Avm2Object* lc_find_listener(const char* key)
{
	for (int i = 0; i < g_lc_channel_count; i++)
		if (strcmp(g_lc_channels[i].key, key) == 0) return g_lc_channels[i].obj;
	return NULL;
}

static void lc_unregister(const char* key)
{
	for (int i = 0; i < g_lc_channel_count; i++)
	{
		if (strcmp(g_lc_channels[i].key, key) != 0) continue;
		for (int j = i; j < g_lc_channel_count - 1; j++)
			g_lc_channels[j] = g_lc_channels[j + 1];
		g_lc_channel_count--;
		return;
	}
}

// The channel and queue hold the only references to a connection that is
// otherwise unreachable from script between send and delivery.
static void gc_mark_roots_lc(void)
{
	for (int i = 0; i < g_lc_channel_count; i++)
		avm2_gc_mark_object(g_lc_channels[i].obj);
	for (int i = 0; i < g_lc_queue_count; i++)
		avm2_gc_mark_object(g_lc_queue[i].sender);
}

static _Noreturn void lc_throw_2007(Avm2Context* ctx, const char* param)
{
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #2007: Parameter %s must be non-null.", param);
}

static _Noreturn void lc_throw_2085(Avm2Context* ctx, const char* param)
{
	avm2_throw_error(ctx, ctx->builtins.argument_error_class,
	                 "Error #2085: Parameter %s must be non-empty string.", param);
}

static _Noreturn void lc_throw_2004(Avm2Context* ctx, int type_error)
{
	avm2_throw_error(ctx,
	                 type_error ? ctx->builtins.type_error_class
	                            : ctx->builtins.argument_error_class,
	                 "Error #2004: One of the parameters is invalid.");
}

// A non-null String argument, Ruffle's ParametersExt::get_string_non_null.
static const Avm2String* lc_arg_string(Avm2Activation* act, uint32_t i,
                                       const char* param)
{
	Avm2Value v = (i < act->argc) ? act->args[i] : avm2_undefined();
	if (v.kind == AVM2_VALUE_NULL || v.kind == AVM2_VALUE_UNDEFINED)
		lc_throw_2007(act->ctx, param);
	return avm2_coerce_to_string(act->ctx, v);
}

static Avm2Value lc_init(Avm2Activation* act)
{
	Avm2LocalConnectionExt* e = lc_ext(act);
	if (e == NULL) return avm2_undefined();
	e->key[0] = '\0';
	// LocalConnection.as: the client defaults to the connection itself.
	e->client = act->this_val;
	return avm2_undefined();
}

static Avm2Value lc_connect(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* self = net2_this(act);
	Avm2LocalConnectionExt* e = lc_ext(act);
	const Avm2String* name = lc_arg_string(act, 0, "connectionName");
	if (name->len == 0) lc_throw_2085(ctx, "connectionName");
	if (memchr(name->utf8, ':', name->len) != NULL) lc_throw_2004(ctx, 0);
	if (e == NULL || self == NULL) return avm2_undefined();

	char key[LC_MAX_NAME];
	lc_connect_key(name, key, sizeof(key));
	// #2082 covers BOTH "this object is already connected" and "someone else
	// holds the name" — Ruffle notes the message is misleading in the latter case.
	if (e->key[0] != '\0' || lc_find_listener(key) != NULL
	    || g_lc_channel_count >= LC_MAX_CHANNELS)
	{
		avm2_throw_error(ctx, ctx->builtins.argument_error_class,
		                 "Error #2082: Connect failed because the object is "
		                 "already connected.");
	}
	snprintf(g_lc_channels[g_lc_channel_count].key,
	         sizeof(g_lc_channels[0].key), "%s", key);
	g_lc_channels[g_lc_channel_count].obj = self;
	g_lc_channel_count++;
	snprintf(e->key, sizeof(e->key), "%s", key);
	return avm2_undefined();
}

static Avm2Value lc_close(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2LocalConnectionExt* e = lc_ext(act);
	if (e == NULL) return avm2_undefined();
	if (e->key[0] == '\0')
	{
		avm2_throw_error(ctx, ctx->builtins.argument_error_class,
		                 "Error #2083: Close failed because the object is not "
		                 "connected.");
	}
	lc_unregister(e->key);
	e->key[0] = '\0';
	return avm2_undefined();
}

// The method names LocalConnection's own surface owns; a send naming one of them
// is an ArgumentError rather than a delivery.
static int lc_method_is_reserved(const Avm2String* m)
{
	static const char* const names[6] = {
		"send", "connect", "close", "allowDomain", "allowInsecureDomain", "domain"
	};
	for (int i = 0; i < 6; i++)
	{
		size_t n = strlen(names[i]);
		if (m->len == n && memcmp(m->utf8, names[i], n) == 0) return 1;
	}
	return 0;
}

static Avm2Value lc_send(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* self = net2_this(act);
	const Avm2String* cname = lc_arg_string(act, 0, "connectionName");
	const Avm2String* method = lc_arg_string(act, 1, "methodName");
	if (cname->len == 0) lc_throw_2085(ctx, "connectionName");
	if (method->len == 0) lc_throw_2085(ctx, "methodName");
	if (lc_method_is_reserved(method)) lc_throw_2004(ctx, 0);
	if (self == NULL || g_lc_queue_count >= LC_MAX_QUEUE) return avm2_undefined();

	LcMessage* m = &g_lc_queue[g_lc_queue_count];
	memset(m, 0, sizeof(*m));
	m->sender = self;
	lc_send_key(cname, m->key, sizeof(m->key));
	snprintf(m->method, sizeof(m->method), "%.*s", (int) method->len, method->utf8);
	// The arguments are serialized NOW: the callee must see a deep copy, and the
	// queue outlives the frame that built the values.
	uint32_t n = (act->argc > 2) ? act->argc - 2 : 0;
	if (n > LC_MAX_ARGS) n = LC_MAX_ARGS;
	for (uint32_t i = 0; i < n; i++)
	{
		size_t len = 0;
		m->args[i] = avm2_amf0_write_value(ctx, act->args[2 + i], &len);
		m->arg_len[i] = len;
	}
	m->argc = (int) n;
	m->failure = (lc_find_listener(m->key) == NULL
	              && !actionAvm1LocalConnectionHasChannel(m->key)) ? 1 : 0;
	g_lc_queue_count++;
	return avm2_undefined();
}

static Avm2Value lc_get_client(Avm2Activation* act)
{
	Avm2LocalConnectionExt* e = lc_ext(act);
	return (e != NULL) ? e->client : avm2_undefined();
}

static Avm2Value lc_set_client(Avm2Activation* act)
{
	Avm2LocalConnectionExt* e = lc_ext(act);
	Avm2Value v = (act->argc > 0) ? act->args[0] : avm2_undefined();
	// Anything that is not an object — null and undefined included — is #2004.
	if (v.kind != AVM2_VALUE_OBJECT) lc_throw_2004(act->ctx, 1);
	if (e != NULL) e->client = v;
	return avm2_undefined();
}

static Avm2Value lc_get_domain_prop(Avm2Activation* act)
{
	return avm2_string(avm2_string_from_literal(act->ctx,
	                                            avm2_local_connection_domain()));
}

static Avm2Value lc_noop(Avm2Activation* act)
{ (void) act; return avm2_undefined(); }

static Avm2Value lc_get_true(Avm2Activation* act)
{ (void) act; return avm2_bool(1); }

// --- delivery ---

static void lc_send_status(Avm2Context* ctx, Avm2Object* conn, const char* level)
{
	if (conn == NULL) return;
	Avm2Object* ev = avm2_status_event_new(ctx, NULL, level);
	if (ev != NULL) avm2_dispatch_event(ctx, conn, ev);
}

// Invoke the callee's method, catching whatever it throws: a thrown Error
// becomes an AsyncErrorEvent on the RECEIVER, carrying both the #2095 text and
// the error itself.
static void lc_run_method(Avm2Context* ctx, Avm2Object* receiver, LcMessage* m)
{
	Avm2LocalConnectionExt* e = lc_ext_of(receiver);
	if (e == NULL) return;
	Avm2Value args[LC_MAX_ARGS];
	for (int i = 0; i < m->argc; i++)
		args[i] = avm2_amf0_read_value(ctx, m->args[i], m->arg_len[i]);

	static const Avm2AbcException catch_any = { 0, 0xFFFFFFFF, 0, 0, 0, 1 };
	Avm2TryFrame tf;
	avm2_try_push_frame(ctx, &tf, &catch_any, 1, NULL);
	tf.op_index = 1;
	if (setjmp(tf.jb) == 0)
	{
		// Ruffle calls this as a script `client.method(...)` would, so a name a
		// SEALED client does not define raises the getproperty error #1069 (both
		// LocalConnection tests grade that text) while a dynamic one gets the
		// #1006 the call itself raises on undefined. Both come out of the shared
		// property path — LocalConnection is a sealed class, so a default client
		// (the connection itself) takes the first branch.
		uint32_t mlen = (uint32_t) strlen(m->method);
		avm2_call_public_property(ctx, e->client, m->method, mlen,
		                          m->argc > 0 ? args : NULL, (uint32_t) m->argc);
		avm2_try_pop_frame(&tf);
		return;
	}
	Avm2Value err = tf.exc;
	avm2_try_pop_frame(&tf);
	char text[LC_MAX_NAME + 96];
	snprintf(text, sizeof(text),
	         "Error #2095: flash.net.LocalConnection was unable to invoke "
	         "callback %s.", m->method);
	Avm2Object* ev = avm2_async_error_event_new(
		ctx, avm2_string_from_literal(ctx, text),
		(err.kind == AVM2_VALUE_OBJECT) ? err.u.obj : NULL);
	if (ev != NULL) avm2_dispatch_event(ctx, receiver, ev);
}

// Per-tick delivery, called from avm2_display_run_tick right after the frame's
// AVM2 phases — where Ruffle runs LocalConnections::update_connections, which is
// BEFORE its NetConnection flush (avm2/amf_array_serialization traces the
// delivered message ahead of both remoting packets).
void avm2_net_deliver_local_connections(Avm2Context* ctx)
{
	if (g_lc_queue_count == 0) return;
	// Take the whole queue first: a callee may send, and that belongs to the
	// next tick (Ruffle mem::take).
	LcMessage batch[LC_MAX_QUEUE];
	int count = g_lc_queue_count;
	memcpy(batch, g_lc_queue, (size_t) count * sizeof(LcMessage));
	g_lc_queue_count = 0;

	for (int i = 0; i < count; i++)
	{
		LcMessage* m = &batch[i];
		Avm2Object* receiver = m->failure ? NULL : lc_find_listener(m->key);
		if (receiver != NULL)
		{
			lc_send_status(ctx, m->sender, "status");
			lc_run_method(ctx, receiver, m);
		}
		else if (!m->failure && actionAvm1LocalConnectionHasChannel(m->key))
		{
			// Cross-VM: an AVM1 child holds the channel. Status first, exactly
			// as on the AVM2 path; the AVM1 half owns the callee dispatch and
			// reads these very AMF0 buffers with its own reader.
			lc_send_status(ctx, m->sender, "status");
			actionAvm1LocalConnectionDeliver(ctx->app, m->key, m->method,
			                                 m->args, m->arg_len, m->argc);
		}
		else
		{
			lc_send_status(ctx, m->sender, "error");
		}
		for (int a = 0; a < m->argc; a++)
		{
			free(m->args[a]);
			m->args[a] = NULL;
		}
	}
}

// The AVM2 half of the bridge, called from action.c's AVM1 queue drain when an
// AVM1 send() finds no AVM1 listener for the channel.
int avm2_net_local_connection_has_channel(const char* key)
{
	return (key != NULL && lc_find_listener(key) != NULL) ? 1 : 0;
}

// Deliver a message an AVM1 LocalConnection sent. `args` holds `argc` AMF0
// buffers owned by the AVM1 queue — lc_run_method reads them and never frees.
// The AVM1 side has already dispatched its sender's onStatus. Returns 1 if an
// AVM2 listener held the channel.
int avm2_net_local_connection_deliver_from_avm1(const char* key, const char* method,
                                                unsigned char* const* args,
                                                const size_t* arg_len, int argc)
{
	Avm2Context* ctx = avm2_get_context();
	Avm2Object* receiver = (ctx != NULL && key != NULL) ? lc_find_listener(key) : NULL;
	if (receiver == NULL || method == NULL) return 0;
	if (argc < 0) argc = 0;
	if (argc > LC_MAX_ARGS) argc = LC_MAX_ARGS;

	LcMessage m;
	memset(&m, 0, sizeof(m));
	snprintf(m.key, sizeof(m.key), "%s", key);
	snprintf(m.method, sizeof(m.method), "%s", method);
	for (int i = 0; i < argc; i++)
	{
		m.args[i] = args[i];
		m.arg_len[i] = arg_len[i];
	}
	m.argc = argc;
	lc_run_method(ctx, receiver, &m);
	return 1;
}

static void register_local_connection(Avm2Context* ctx)
{
	g_lc_channel_count = 0;
	g_lc_queue_count = 0;
	Avm2Class* cls = avm2_builtin_class(ctx, "flash.net", "LocalConnection",
	                                    ctx->builtins.event_dispatcher_class);
	cls->instance_init.fn = lc_init;
	cls->instance_init.debug_name = "LocalConnection";
	cls->native_ext_size = sizeof(Avm2LocalConnectionExt);
	// Sealed, like the AS3 class: a send naming a method the default client (the
	// connection itself) does not have must raise #1069, not silently find
	// undefined.
	cls->flags |= AVM2_CLASS_FLAG_SEALED;
	g_local_connection_class = cls;
	avm2_builtin_add_getter(ctx, cls, "domain", lc_get_domain_prop);
	avm2_builtin_add_method(ctx, cls, "connect", lc_connect);
	avm2_builtin_add_method(ctx, cls, "send", lc_send);
	avm2_builtin_add_method(ctx, cls, "close", lc_close);
	avm2_builtin_add_method(ctx, cls, "allowDomain", lc_noop);
	avm2_builtin_add_method(ctx, cls, "allowInsecureDomain", lc_noop);
	avm2_builtin_add_getset(ctx, cls, "client", lc_get_client, lc_set_client);
	avm2_builtin_add_getset(ctx, cls, "isPerUser", lc_get_true, lc_noop);
	// AIR-era static; true everywhere a player exists.
	avm2_builtin_add_static_const(ctx, cls, "isSupported", avm2_bool(1));
}

// ---------------------------------------------------------------------------
// flash.net.NetStream
// ---------------------------------------------------------------------------
//
// No media pipeline (no demuxer, no A/V decoder): the corpus tests that need
// one are dispositioned out of this arc (net-socket-arc.md bucket M). What IS
// graded is the `client` property, whose setter rejects every non-object with
// TypeError #2004 — including null and undefined, which is why it cannot be a
// plain slot — plus the SCRIPT-DATA half of playback, below.
//
// FLV script data is not media: an FLV's onMetaData tag is a plain AMF0 value
// sitting in a container we can walk with a length-prefixed tag loop, and
// Flash delivers it to `client.onMetaData` before a single sample is decoded.
// So `play()` on a bundled .flv parses the metadata tag and queues that one
// callback; everything downstream of it (frames, audio, `time`) stays absent.
// That is the whole of `avm2/netstream_flv_date` and the "cheapest re-entry
// point" bucket M named.

typedef struct Avm2NetStreamExt
{
	Avm2EventDispatcherExt dispatcher;  // extends EventDispatcher (MUST be first)
	Avm2Value client;                   // defaults to the stream itself
	Avm2Value pending_meta;             // queued onMetaData argument
	uint8_t has_pending_meta;
} Avm2NetStreamExt;

static Avm2NetStreamExt* ns_ext(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	return (o != NULL && o->native_ext != NULL) ? (Avm2NetStreamExt*) o->native_ext : NULL;
}

static Avm2Value ns_init(Avm2Activation* act)
{
	Avm2NetStreamExt* e = ns_ext(act);
	if (e == NULL) return avm2_undefined();
	e->client = act->this_val;
	return avm2_undefined();
}

static Avm2Value ns_get_client(Avm2Activation* act)
{ Avm2NetStreamExt* e = ns_ext(act); return e ? e->client : avm2_undefined(); }

static Avm2Value ns_set_client(Avm2Activation* act)
{
	Avm2NetStreamExt* e = ns_ext(act);
	Avm2Value v = act->argc > 0 ? act->args[0] : avm2_undefined();
	if (v.kind != AVM2_VALUE_OBJECT)
	{
		avm2_throw_error(act->ctx, act->ctx->builtins.type_error_class,
		                 "Error #2004: One of the parameters is invalid.");
	}
	if (e != NULL) e->client = v;
	return avm2_undefined();
}

static Avm2Value ns_get_zero(Avm2Activation* act)
{ (void) act; return avm2_number(0); }

// --- FLV script data ------------------------------------------------------
//
// Streams with a queued onMetaData. Small and fixed: a test movie plays one
// or two clips, and a stream whose callback has fired drops out immediately.
#define NS_MAX_PENDING_META 8
static Avm2Object* g_ns_pending_meta[NS_MAX_PENDING_META];
static int g_ns_pending_meta_count;

static void gc_mark_roots_netstream(void)
{
	for (int i = 0; i < g_ns_pending_meta_count; i++)
		avm2_gc_mark_object(g_ns_pending_meta[i]);
}

// Locate the AMF0 body of the first SCRIPTDATA tag (type 18) whose name is
// "onMetaData", and return a pointer to the VALUE that follows the name.
// FLV body layout: 9-byte header, then repeating [PreviousTagSize u32][tag
// header 11 bytes][tag data]. Returns NULL when the file is not an FLV, has no
// such tag, or is truncated — every one of which leaves play() silent, which
// is what Flash does with a stream that carries no metadata.
static const unsigned char* flv_find_metadata(const unsigned char* data,
                                              size_t len, size_t* out_len)
{
	if (data == NULL || len < 13) return NULL;
	if (data[0] != 'F' || data[1] != 'L' || data[2] != 'V') return NULL;
	size_t off = ((size_t) data[5] << 24) | ((size_t) data[6] << 16)
	           | ((size_t) data[7] << 8) | (size_t) data[8];   // DataOffset
	if (off < 9 || off > len) off = 9;
	off += 4;   // first PreviousTagSize
	while (off + 11 <= len)
	{
		unsigned int tag_type = data[off] & 0x1F;
		size_t tag_size = ((size_t) data[off + 1] << 16)
		                | ((size_t) data[off + 2] << 8) | (size_t) data[off + 3];
		size_t body = off + 11;
		if (body + tag_size > len) break;
		if (tag_type == 18 && tag_size > 3 && data[body] == 0x02)
		{
			size_t nlen = ((size_t) data[body + 1] << 8) | (size_t) data[body + 2];
			size_t name_end = body + 3 + nlen;
			if (name_end <= body + tag_size && nlen == 10
			    && memcmp(data + body + 3, "onMetaData", 10) == 0)
			{
				*out_len = body + tag_size - name_end;
				return data + name_end;
			}
		}
		off = body + tag_size + 4;   // skip the trailing PreviousTagSize
	}
	return NULL;
}

// play(name) — the script-data half only. A name that resolves to a bundled
// .flv with an onMetaData tag queues the callback for the end of the frame
// (Flash never delivers it inside the calling frame: netstream_flv_date traces
// "init finished" from the same constructor first). Anything else is a no-op.
static Avm2Value ns_play(Avm2Activation* act)
{
	Avm2Object* self = net2_this(act);
	Avm2NetStreamExt* e = ns_ext(act);
	if (self == NULL || e == NULL || act->argc == 0) return avm2_undefined();
	if (act->args[0].kind == AVM2_VALUE_NULL
	    || act->args[0].kind == AVM2_VALUE_UNDEFINED)
		return avm2_undefined();
	const Avm2String* url = avm2_coerce_to_string(act->ctx, act->args[0]);
	if (url == NULL || url->len == 0) return avm2_undefined();

	// findDataFile is keyed by the bare filename as well as the relative path.
	char name[256];
	size_t n = (size_t) url->len < sizeof(name) - 1
		? (size_t) url->len : sizeof(name) - 1;
	memcpy(name, url->utf8, n);
	name[n] = '\0';
	const char* base = name;
	for (const char* p = name; *p != '\0'; p++)
		if (*p == '/' || *p == '\\') base = p + 1;
	DataFileEntry* d = findDataFile(base);
	if (d == NULL) d = findDataFile(name);
	if (d == NULL || d->content == NULL) return avm2_undefined();

	size_t mlen = 0;
	const unsigned char* meta = flv_find_metadata(
		(const unsigned char*) d->content, (size_t) d->content_length, &mlen);
	if (meta == NULL) return avm2_undefined();

	e->pending_meta = avm2_amf0_read_value(act->ctx, meta, mlen);
	if (e->has_pending_meta) return avm2_undefined();   // already queued
	e->has_pending_meta = 1;
	if (g_ns_pending_meta_count < NS_MAX_PENDING_META)
		g_ns_pending_meta[g_ns_pending_meta_count++] = self;
	return avm2_undefined();
}

// Per-tick drain, called from avm2_display_run_tick's executor-drain point
// (beside the NetConnection flush). The client is whatever `client` holds at
// delivery time, and a client without an onMetaData property is silent —
// Flash raises no error for a missing script-data handler.
void avm2_net_deliver_netstream_meta(Avm2Context* ctx)
{
	if (g_ns_pending_meta_count == 0) return;
	Avm2Object* batch[NS_MAX_PENDING_META];
	int count = g_ns_pending_meta_count;
	memcpy(batch, g_ns_pending_meta, (size_t) count * sizeof(Avm2Object*));
	g_ns_pending_meta_count = 0;
	for (int i = 0; i < count; i++)
	{
		Avm2Object* ns = batch[i];
		if (ns == NULL || ns->native_ext == NULL) continue;
		Avm2NetStreamExt* e = (Avm2NetStreamExt*) ns->native_ext;
		if (!e->has_pending_meta) continue;
		e->has_pending_meta = 0;
		Avm2Value arg = e->pending_meta;
		e->pending_meta = avm2_undefined();
		Avm2Value client = e->client;
		if (client.kind != AVM2_VALUE_OBJECT) continue;
		if (!avm2_has_public_property(ctx, client, "onMetaData", 10)) continue;
		avm2_call_public_property(ctx, client, "onMetaData", 10, &arg, 1);
	}
}

// --- flash.net.NetStreamPlayOptions ---------------------------------------
//
// `public var` slots in NetStreamPlayOptions.as, modelled here as accessor
// pairs over an ext (indistinguishable from slots for every read and write a
// script makes) so the three numeric defaults — len -1, offset -1, start -2 —
// can be set in one place.

typedef struct Avm2NpoExt
{
	Avm2EventDispatcherExt dispatcher;  // extends EventDispatcher (MUST be first)
	double len, offset, start;
	Avm2Value old_stream_name, stream_name, transition;
} Avm2NpoExt;

static Avm2NpoExt* npo_ext(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	return (o != NULL && o->native_ext != NULL) ? (Avm2NpoExt*) o->native_ext : NULL;
}

static Avm2Value npo_init(Avm2Activation* act)
{
	Avm2NpoExt* e = npo_ext(act);
	if (e == NULL) return avm2_undefined();
	e->len = -1;
	e->offset = -1;
	e->start = -2;
	e->old_stream_name = avm2_null();
	e->stream_name = avm2_null();
	e->transition = avm2_null();
	return avm2_undefined();
}

#define NPO_NUM_ACCESSORS(field, get_name, set_name)                          \
	static Avm2Value get_name(Avm2Activation* act)                            \
	{ Avm2NpoExt* e = npo_ext(act); return avm2_number(e ? e->field : 0); }    \
	static Avm2Value set_name(Avm2Activation* act)                            \
	{                                                                         \
		Avm2NpoExt* e = npo_ext(act);                                         \
		if (e != NULL && act->argc > 0)                                       \
			e->field = avm2_coerce_to_number(act->ctx, act->args[0]);         \
		return avm2_undefined();                                              \
	}

NPO_NUM_ACCESSORS(len, npo_get_len, npo_set_len)
NPO_NUM_ACCESSORS(offset, npo_get_offset, npo_set_offset)
NPO_NUM_ACCESSORS(start, npo_get_start, npo_set_start)

// The three String vars: an unset one is null, an assigned one coerces (a
// null assignment stays null rather than becoming the string "null").
#define NPO_STR_ACCESSORS(field, get_name, set_name)                          \
	static Avm2Value get_name(Avm2Activation* act)                            \
	{ Avm2NpoExt* e = npo_ext(act); return e ? e->field : avm2_null(); }       \
	static Avm2Value set_name(Avm2Activation* act)                            \
	{                                                                         \
		Avm2NpoExt* e = npo_ext(act);                                         \
		if (e != NULL && act->argc > 0)                                       \
		{                                                                     \
			e->field = (act->args[0].kind == AVM2_VALUE_NULL                  \
			            || act->args[0].kind == AVM2_VALUE_UNDEFINED)         \
				? avm2_null()                                                 \
				: avm2_string(avm2_coerce_to_string(act->ctx, act->args[0])); \
		}                                                                     \
		return avm2_undefined();                                              \
	}

NPO_STR_ACCESSORS(old_stream_name, npo_get_old_name, npo_set_old_name)
NPO_STR_ACCESSORS(stream_name, npo_get_name, npo_set_name)
NPO_STR_ACCESSORS(transition, npo_get_transition, npo_set_transition)

static void register_net_stream(Avm2Context* ctx)
{
	Avm2Class* cls = avm2_builtin_class(ctx, "flash.net", "NetStream",
	                                    ctx->builtins.event_dispatcher_class);
	cls->instance_init.fn = ns_init;
	cls->instance_init.debug_name = "NetStream";
	cls->native_ext_size = sizeof(Avm2NetStreamExt);

	avm2_builtin_add_getset(ctx, cls, "client", ns_get_client, ns_set_client);
	avm2_builtin_add_getter(ctx, cls, "time", ns_get_zero);
	avm2_builtin_add_getter(ctx, cls, "bytesLoaded", ns_get_zero);
	avm2_builtin_add_getter(ctx, cls, "bytesTotal", ns_get_zero);
	avm2_builtin_add_getter(ctx, cls, "currentFPS", ns_get_zero);
	avm2_builtin_add_getter(ctx, cls, "bufferLength", ns_get_zero);
	avm2_builtin_add_method(ctx, cls, "play", ns_play);
	avm2_builtin_add_method(ctx, cls, "pause", net2_noop);
	avm2_builtin_add_method(ctx, cls, "resume", net2_noop);
	avm2_builtin_add_method(ctx, cls, "togglePause", net2_noop);
	avm2_builtin_add_method(ctx, cls, "seek", net2_noop);
	avm2_builtin_add_method(ctx, cls, "close", net2_noop);
	avm2_builtin_add_method(ctx, cls, "dispose", net2_noop);

	// flash.net.NetStreamPlayOptions — a bag of six defaults with no behaviour
	// (public vars in NetStreamPlayOptions.as; accessor pairs are
	// indistinguishable from slots for every read and write a script makes).
	Avm2Class* npo = avm2_builtin_class(ctx, "flash.net", "NetStreamPlayOptions",
	                                    ctx->builtins.event_dispatcher_class);
	npo->instance_init.fn = npo_init;
	npo->instance_init.debug_name = "NetStreamPlayOptions";
	npo->native_ext_size = sizeof(Avm2NpoExt);
	avm2_builtin_add_getset(ctx, npo, "len", npo_get_len, npo_set_len);
	avm2_builtin_add_getset(ctx, npo, "offset", npo_get_offset, npo_set_offset);
	avm2_builtin_add_getset(ctx, npo, "start", npo_get_start, npo_set_start);
	avm2_builtin_add_getset(ctx, npo, "oldStreamName",
	                        npo_get_old_name, npo_set_old_name);
	avm2_builtin_add_getset(ctx, npo, "streamName",
	                        npo_get_name, npo_set_name);
	avm2_builtin_add_getset(ctx, npo, "transition",
	                        npo_get_transition, npo_set_transition);
}

// ---------------------------------------------------------------------------
// flash.media.AVNetworkingParams
// ---------------------------------------------------------------------------

typedef struct Avm2AvnpExt
{
	uint8_t force_native, read_cookie, cookie_all;
	Avm2Value down_url, query_param;
} Avm2AvnpExt;

static Avm2AvnpExt* avnp_ext(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	return (o != NULL && o->native_ext != NULL) ? (Avm2AvnpExt*) o->native_ext : NULL;
}

static Avm2Value avnp_init(Avm2Activation* act)
{
	Avm2AvnpExt* e = avnp_ext(act);
	if (e == NULL) return avm2_undefined();
	// Only the middle default is true (AVNetworkingParams.as).
	e->force_native = (uint8_t) net2_arg_bool(act, 0, 0);
	e->read_cookie = (uint8_t) net2_arg_bool(act, 1, 1);
	e->cookie_all = (uint8_t) net2_arg_bool(act, 2, 0);
	e->down_url = (act->argc > 3)
		? avm2_string(avm2_coerce_to_string(act->ctx, act->args[3]))
		: avm2_string(avm2_string_from_literal(act->ctx, ""));
	// appendRandomQueryParameter takes no constructor argument, but it DOES
	// have an initializer -- AVNetworkingParams.as declares
	// `private var _appendRandomQueryParameter:String = "";` -- so it reads
	// back as the empty string, not null (av_classes grades both the
	// default-params and provided-params blocks on this).
	e->query_param = avm2_string(avm2_string_from_literal(act->ctx, ""));
	return avm2_undefined();
}

#define AVNP_BOOL_ACCESSORS(field, get_name, set_name)                        \
	static Avm2Value get_name(Avm2Activation* act)                            \
	{ Avm2AvnpExt* e = avnp_ext(act); return avm2_bool(e != NULL && e->field); } \
	static Avm2Value set_name(Avm2Activation* act)                            \
	{                                                                         \
		Avm2AvnpExt* e = avnp_ext(act);                                       \
		if (e != NULL && act->argc > 0)                                       \
			e->field = (uint8_t) (avm2_coerce_to_boolean(act->args[0]) ? 1 : 0); \
		return avm2_undefined();                                              \
	}

AVNP_BOOL_ACCESSORS(force_native, avnp_get_force, avnp_set_force)
AVNP_BOOL_ACCESSORS(read_cookie, avnp_get_cookie, avnp_set_cookie)
AVNP_BOOL_ACCESSORS(cookie_all, avnp_get_all_req, avnp_set_all_req)

#define AVNP_STR_ACCESSORS(field, get_name, set_name)                         \
	static Avm2Value get_name(Avm2Activation* act)                            \
	{ Avm2AvnpExt* e = avnp_ext(act); return e ? e->field : avm2_null(); }     \
	static Avm2Value set_name(Avm2Activation* act)                            \
	{                                                                         \
		Avm2AvnpExt* e = avnp_ext(act);                                       \
		if (e != NULL && act->argc > 0)                                       \
		{                                                                     \
			e->field = (act->args[0].kind == AVM2_VALUE_NULL                  \
			            || act->args[0].kind == AVM2_VALUE_UNDEFINED)         \
				? avm2_null()                                                 \
				: avm2_string(avm2_coerce_to_string(act->ctx, act->args[0])); \
		}                                                                     \
		return avm2_undefined();                                              \
	}

AVNP_STR_ACCESSORS(down_url, avnp_get_url, avnp_set_url)
AVNP_STR_ACCESSORS(query_param, avnp_get_qp, avnp_set_qp)

// ---------------------------------------------------------------------------
// flash.net.FileFilter / FileReference / FileReferenceList
// ---------------------------------------------------------------------------
//
// The file half of flash.net. There is no filesystem behind it and there does
// not need to be: the dialog is the scripted mock in src/utils.c
// (dialog_events.h), which decides select-vs-cancel from the filter
// description / file-name hint and always hands back the same 13-byte
// "Hello, World!" named test.txt.
//
// FileReference's state is Ruffle's two-variant enum (None | Selection) plus a
// separate `loaded` flag that only load() sets. Every accessor except `creator`
// and `data` throws #2037 in the None state; `creator` is always null (it was
// classic-macOS only) and `data` returns null instead of throwing, which is
// what filereference_uninitialized grades.
//
// The class is SEALED, and that is graded in the NEGATIVE: `extension` is
// [API("661")] AIR-only in Ruffle's FileReference.as, so reading it must raise
// ReferenceError #1069 rather than yield undefined. Registering the property
// "just in case" would silently turn two passing tests into failures.
//
// Where we deliberately differ from Ruffle: a save-derived selection reports
// `size` 0 until the save's COMPLETE fires, and load() on one throws #2037.
// Ruffle's mock pre-fills the destination with the same "Hello, World!" and
// lets load() succeed, which re-enters save's own complete handler forever —
// hence `known_failure = true` AND `ignore = true` on
// filereference_save_and_load upstream. Its output.txt is Flash's behaviour
// and Flash's behaviour is implementable, so we implement it and the test
// passes outright.

typedef struct Avm2FileFilterExt
{
	Avm2Value description, extension, mac_type;
} Avm2FileFilterExt;

static Avm2Class* g_filefilter_class;

static Avm2FileFilterExt* ff_ext(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	return (o != NULL && o->native_ext != NULL) ? (Avm2FileFilterExt*) o->native_ext : NULL;
}

static Avm2Value ff_arg_str(Avm2Activation* act, uint32_t i)
{
	if (act->argc <= i || act->args[i].kind == AVM2_VALUE_NULL
	    || act->args[i].kind == AVM2_VALUE_UNDEFINED)
		return avm2_null();
	return avm2_string(avm2_coerce_to_string(act->ctx, act->args[i]));
}

static Avm2Value ff_init(Avm2Activation* act)
{
	Avm2FileFilterExt* e = ff_ext(act);
	if (e == NULL) return avm2_undefined();
	e->description = ff_arg_str(act, 0);
	e->extension = ff_arg_str(act, 1);
	e->mac_type = ff_arg_str(act, 2);
	return avm2_undefined();
}

#define FF_ACCESSORS(field, get_name, set_name)                               \
	static Avm2Value get_name(Avm2Activation* act)                            \
	{ Avm2FileFilterExt* e = ff_ext(act); return e ? e->field : avm2_null(); } \
	static Avm2Value set_name(Avm2Activation* act)                            \
	{                                                                         \
		Avm2FileFilterExt* e = ff_ext(act);                                   \
		if (e != NULL) e->field = ff_arg_str(act, 0);                         \
		return avm2_undefined();                                              \
	}

FF_ACCESSORS(description, ff_get_desc, ff_set_desc)
FF_ACCESSORS(extension, ff_get_ext, ff_set_ext)
FF_ACCESSORS(mac_type, ff_get_mac, ff_set_mac)

// --- FileReference ---------------------------------------------------------

typedef struct Avm2FileRefExt
{
	Avm2EventDispatcherExt dispatcher;  // extends EventDispatcher (MUST be first)
	Avm2Object* self;
	uint8_t has_selection;   // FileReference::Selection vs ::None
	uint8_t loaded;          // set only by load(); gates the `data` getter
	uint8_t from_save;       // the selection came from save(), not browse()
	uint8_t save_written;    // the save's COMPLETE has fired
	char name[260];          // test.txt, or save()'s fileNameHint
	uint8_t* data;           // selection contents
	uint32_t data_len;
	uint8_t* save_data;      // bytes handed to save(), awaiting the dialog
	uint32_t save_len;
} Avm2FileRefExt;

static Avm2Class* g_fileref_class;

// A FileReference with a dialog in flight is reachable only from the mock's
// queue, so it has to be a GC root for as long as that dialog lives — the same
// reason Ruffle stashes the object handle in the future.
#define MAX_LIVE_FILEREFS 32
static Avm2Object* g_live_filerefs[MAX_LIVE_FILEREFS];
static int g_live_fileref_count;

static void fileref_root(Avm2Object* o)
{
	if (o == NULL) return;
	for (int i = 0; i < g_live_fileref_count; i++)
		if (g_live_filerefs[i] == o) return;
	if (g_live_fileref_count < MAX_LIVE_FILEREFS)
		g_live_filerefs[g_live_fileref_count++] = o;
}

static int fileref_obj_is(Avm2Object* o)
{
	if (o == NULL || g_fileref_class == NULL) return 0;
	for (Avm2Class* c = o->cls; c != NULL; c = c->super_class)
		if (c == g_fileref_class) return 1;
	return 0;
}

static Avm2FileRefExt* fileref_ext(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	if (!fileref_obj_is(o) || o->native_ext == NULL) return NULL;
	return (Avm2FileRefExt*) o->native_ext;
}

static _Noreturn void fileref_throw_2037(Avm2Context* ctx)
{
	avm2_throw_error(ctx, ctx->builtins.error_class,
	                 "Error #2037: Functions called in incorrect sequence, or "
	                 "earlier call was unsuccessful.");
}

// Every accessor but `creator` and `data` needs a selection first.
static Avm2FileRefExt* fileref_require_selection(Avm2Activation* act)
{
	Avm2FileRefExt* e = fileref_ext(act);
	if (e == NULL || !e->has_selection) fileref_throw_2037(act->ctx);
	return e;
}

static void fileref_set_data(Avm2FileRefExt* e, const uint8_t* bytes, uint32_t len)
{
	free(e->data);
	e->data = NULL;
	e->data_len = 0;
	if (len == 0) return;
	e->data = (uint8_t*) malloc(len);
	if (e->data == NULL) return;
	memcpy(e->data, bytes, len);
	e->data_len = len;
}

static Avm2Value fileref_init(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	Avm2FileRefExt* e = fileref_ext(act);
	if (e == NULL) return avm2_undefined();
	e->self = o;
	e->has_selection = 0;
	e->loaded = 0;
	e->from_save = 0;
	e->save_written = 0;
	e->name[0] = '\0';
	e->data = NULL;
	e->data_len = 0;
	e->save_data = NULL;
	e->save_len = 0;
	return avm2_undefined();
}

// The mock has no creation/modification time, so both dates read as null on a
// selection (filereference_browse_select pins that) and throw before one.
static Avm2Value fileref_get_date(Avm2Activation* act)
{
	fileref_require_selection(act);
	return avm2_null();
}

static Avm2Value fileref_get_creator(Avm2Activation* act)
{
	// Classic-macOS only, and deprecated: null in every state, never #2037.
	(void) act;
	return avm2_null();
}

static Avm2Value fileref_get_data(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2FileRefExt* e = fileref_ext(act);
	// Contrary to every other getter, `data` returns null instead of throwing
	// — and it stays null until load() has run, so a saved-but-not-loaded
	// FileReference reads null throughout (filereference_save).
	if (e == NULL || !e->has_selection || !e->loaded) return avm2_null();
	Avm2Value v = avm2_class_construct(ctx, ctx->builtins.bytearray_class, NULL, 0);
	if (v.kind != AVM2_VALUE_OBJECT) return avm2_null();
	Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(v);
	if (ba != NULL && e->data_len != 0)
	{
		avm2_bytearray_set_length_public(ctx, ba, e->data_len);
		if (ba->bytes != NULL && ba->len == e->data_len)
			memcpy(ba->bytes, e->data, e->data_len);
		ba->position = 0;
	}
	return v;
}

static Avm2Value fileref_get_name(Avm2Activation* act)
{
	Avm2FileRefExt* e = fileref_require_selection(act);
	return net2_str(act, e->name);
}

static Avm2Value fileref_get_size(Avm2Activation* act)
{
	Avm2FileRefExt* e = fileref_require_selection(act);
	// A save destination has no bytes on disk until the write completes:
	// Flash reports 0 for the whole select/open/progress run and the real
	// length only from COMPLETE onward (filereference_save_and_load).
	if (e->from_save && !e->save_written) return avm2_number(0);
	return avm2_number((double) e->data_len);
}

static Avm2Value fileref_get_type(Avm2Activation* act)
{
	fileref_require_selection(act);
	return net2_str(act, SWF_DIALOG_FILE_TYPE);
}

// browse()'s filter array. Ruffle rejects anything that is not a FileFilter
// with non-empty description AND extension with #2097; no corpus test grades
// that, so it stays a single guard rather than a validation subsystem. What IS
// graded is the magic description that flips the mock to "select".
static int fileref_filters_select(Avm2Activation* act, Avm2Value arg)
{
	Avm2Context* ctx = act->ctx;
	if (arg.kind != AVM2_VALUE_OBJECT || arg.u.obj == NULL) return 0;
	Avm2ArrayExt* ax = avm2_array_ext(arg.u.obj);
	if (ax == NULL) return 0;

	int select = 0;
	for (uint32_t i = 0; i < ax->length; i++)
	{
		Avm2Value el = avm2_array_get(arg.u.obj, i);
		Avm2FileFilterExt* fe = NULL;
		if (el.kind == AVM2_VALUE_OBJECT && el.u.obj != NULL
		    && el.u.obj->cls == g_filefilter_class)
			fe = (Avm2FileFilterExt*) el.u.obj->native_ext;
		if (fe == NULL || fe->description.kind != AVM2_VALUE_STRING
		    || fe->extension.kind != AVM2_VALUE_STRING
		    || fe->description.u.str->len == 0 || fe->extension.u.str->len == 0)
		{
			avm2_throw_error(ctx, ctx->builtins.argument_error_class,
			                 "Error #2097: The FileFilter Array is not in the "
			                 "correct format.");
		}
		if (fe->description.u.str->len == (uint32_t) strlen(SWF_DIALOG_MAGIC_FILTER)
		    && memcmp(fe->description.u.str->utf8, SWF_DIALOG_MAGIC_FILTER,
		              strlen(SWF_DIALOG_MAGIC_FILTER)) == 0)
			select = 1;
	}
	return select;
}

static void fileref_bare_event(Avm2Context* ctx, Avm2Object* o, const char* type)
{
	Avm2Object* ev = avm2_event_new(ctx, avm2_string_from_literal(ctx, type), 0, 0);
	if (ev != NULL) avm2_dispatch_event(ctx, o, ev);
}

static void fileref_progress_event(Avm2Context* ctx, Avm2Object* o,
                                   double loaded, double total)
{
	Avm2Object* ev = avm2_progress_event_new(
		ctx, avm2_string_from_literal(ctx, "progress"), loaded, total);
	if (ev != NULL) avm2_dispatch_event(ctx, o, ev);
}

// Adopt the mock's simulated file. Shared by browse() and by the list's
// per-file construction.
static void fileref_take_selection(Avm2FileRefExt* e, const char* name)
{
	e->has_selection = 1;
	e->loaded = 0;
	e->from_save = 0;
	e->save_written = 0;
	snprintf(e->name, sizeof(e->name), "%s", name);
	fileref_set_data(e, (const uint8_t*) SWF_DIALOG_CONTENTS, SWF_DIALOG_CONTENTS_LEN);
}

static void fileref_browse_resolve(void* target, int success)
{
	Avm2Object* o = (Avm2Object*) target;
	if (o == NULL || o->native_ext == NULL) return;
	Avm2FileRefExt* e = (Avm2FileRefExt*) o->native_ext;
	Avm2Context* ctx = avm2_get_context();
	if (!success)
	{
		fileref_bare_event(ctx, o, "cancel");
		return;
	}
	fileref_take_selection(e, swf_dialog_file_name(0));
	fileref_bare_event(ctx, o, "select");
}

static Avm2Value fileref_browse(Avm2Activation* act)
{
	Avm2FileRefExt* e = fileref_ext(act);
	if (e == NULL) return avm2_bool(0);
	int select = fileref_filters_select(act, act->argc > 0 ? act->args[0]
	                                                       : avm2_null());
	fileref_root(e->self);
	swf_dialog_queue(fileref_browse_resolve, e->self, select);
	return avm2_bool(1);
}

static Avm2Value fileref_load(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2FileRefExt* e = fileref_ext(act);
	// No selection at all, or a save destination: Flash refuses both with
	// #2037 (the FIXME on filereference_save_and_load upstream is exactly
	// "FP is more strict regarding the error 2037").
	if (e == NULL || !e->has_selection || e->from_save) fileref_throw_2037(ctx);

	// Nothing is actually fetched — browse() already has the bytes — but the
	// event sequence is a full round trip, and Flash emits open/progress
	// TWICE before completing.
	double size = (double) e->data_len;
	fileref_bare_event(ctx, e->self, "open");
	fileref_progress_event(ctx, e->self, 0, size);
	fileref_bare_event(ctx, e->self, "open");
	fileref_progress_event(ctx, e->self, size, size);
	e->loaded = 1;
	fileref_bare_event(ctx, e->self, "complete");
	return avm2_undefined();
}

static void fileref_save_resolve(void* target, int success)
{
	Avm2Object* o = (Avm2Object*) target;
	if (o == NULL || o->native_ext == NULL) return;
	Avm2FileRefExt* e = (Avm2FileRefExt*) o->native_ext;
	Avm2Context* ctx = avm2_get_context();
	uint8_t* data = e->save_data;
	uint32_t len = e->save_len;
	e->save_data = NULL;
	e->save_len = 0;
	if (!success)
	{
		free(data);
		fileref_bare_event(ctx, o, "cancel");
		return;
	}
	// The destination becomes the selection, named after the hint. Its bytes
	// are the ones being written, but they are not on disk until COMPLETE —
	// `size` reads 0 until then (fileref_get_size).
	e->has_selection = 1;
	e->loaded = 0;
	e->from_save = 1;
	e->save_written = 0;
	free(e->data);
	e->data = data;
	e->data_len = len;
	fileref_bare_event(ctx, o, "select");
	fileref_bare_event(ctx, o, "open");
	fileref_progress_event(ctx, o, (double) len, (double) len);
	e->save_written = 1;
	fileref_bare_event(ctx, o, "complete");
}

static Avm2Value fileref_save(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2FileRefExt* e = fileref_ext(act);
	if (e == NULL) return avm2_undefined();
	Avm2Value data = act->argc > 0 ? act->args[0] : avm2_undefined();
	if (data.kind == AVM2_VALUE_NULL || data.kind == AVM2_VALUE_UNDEFINED)
	{
		// Yes, this really is the error Flash throws (FileReference.as).
		avm2_throw_error(ctx, ctx->builtins.argument_error_class, "data");
	}

	// String / XML / ByteArray all become bytes; XML serialises as markup,
	// not as its text content.
	uint8_t* bytes = NULL;
	uint32_t len = 0;
	Avm2ByteArrayExt* src = avm2_bytearray_ext_of(data);
	if (src != NULL)
	{
		len = src->len;
		if (len != 0)
		{
			bytes = (uint8_t*) malloc(len);
			if (bytes != NULL) memcpy(bytes, src->bytes, len);
			else len = 0;
		}
	}
	else
	{
		const Avm2String* s;
		if (data.kind == AVM2_VALUE_OBJECT && data.u.obj != NULL
		    && (data.u.obj->cls == ctx->builtins.xml_class
		        || data.u.obj->cls == ctx->builtins.xml_list_class))
		{
			Avm2Value x = avm2_call_public_property(ctx, data, "toXMLString", 11,
			                                        NULL, 0);
			s = avm2_coerce_to_string(ctx, x);
		}
		else
		{
			s = avm2_coerce_to_string(ctx, data);
		}
		len = s->len;
		if (len != 0)
		{
			bytes = (uint8_t*) malloc(len);
			if (bytes != NULL) memcpy(bytes, s->utf8, len);
			else len = 0;
		}
	}

	char hint[260];
	if (act->argc > 1 && act->args[1].kind != AVM2_VALUE_NULL
	    && act->args[1].kind != AVM2_VALUE_UNDEFINED)
	{
		const Avm2String* h = avm2_coerce_to_string(ctx, act->args[1]);
		uint32_t n = h->len < sizeof(hint) - 1 ? h->len : (uint32_t) sizeof(hint) - 1;
		memcpy(hint, h->utf8, n);
		hint[n] = '\0';
	}
	else
	{
		hint[0] = '\0';
	}

	free(e->save_data);
	e->save_data = bytes;
	e->save_len = len;
	snprintf(e->name, sizeof(e->name), "%s", hint);
	fileref_root(e->self);
	swf_dialog_queue(fileref_save_resolve, e->self,
	                 strcmp(hint, SWF_DIALOG_MAGIC_SAVE) == 0);
	return avm2_undefined();
}

// --- FileReferenceList -----------------------------------------------------
//
// Ruffle implements this in AS as an EventDispatcher wrapping one private
// FileReference whose select/cancel it re-broadcasts; the list therefore uses
// the SINGLE-file dialog, so `fileList` holds exactly one entry named
// test.txt (the three-file mock belongs to AVM1's FileReferenceList, which
// calls display_file_open_dialog_multiple). `fileList` is null until the first
// browse() and an empty Array from the moment browse() is called — both states
// are graded.

typedef struct Avm2FileRefListExt
{
	Avm2EventDispatcherExt dispatcher;  // extends EventDispatcher (MUST be first)
	Avm2Object* self;
	Avm2Object* file_list;  // NULL until the first browse()
} Avm2FileRefListExt;

static Avm2Class* g_fileref_list_class;

static Avm2FileRefListExt* frl_ext(Avm2Activation* act)
{
	Avm2Object* o = net2_this(act);
	return (o != NULL && o->native_ext != NULL) ? (Avm2FileRefListExt*) o->native_ext : NULL;
}

static Avm2Value frl_init(Avm2Activation* act)
{
	Avm2FileRefListExt* e = frl_ext(act);
	if (e == NULL) return avm2_undefined();
	e->self = net2_this(act);
	e->file_list = NULL;
	return avm2_undefined();
}

static Avm2Value frl_get_file_list(Avm2Activation* act)
{
	Avm2FileRefListExt* e = frl_ext(act);
	if (e == NULL || e->file_list == NULL) return avm2_null();
	return avm2_object_value(e->file_list);
}

static void frl_browse_resolve(void* target, int success)
{
	Avm2Object* o = (Avm2Object*) target;
	if (o == NULL || o->native_ext == NULL) return;
	Avm2FileRefListExt* e = (Avm2FileRefListExt*) o->native_ext;
	Avm2Context* ctx = avm2_get_context();
	if (!success)
	{
		fileref_bare_event(ctx, o, "cancel");
		return;
	}
	Avm2Value fr = avm2_class_construct(ctx, g_fileref_class, NULL, 0);
	if (fr.kind == AVM2_VALUE_OBJECT && fr.u.obj != NULL
	    && fr.u.obj->native_ext != NULL && e->file_list != NULL)
	{
		fileref_take_selection((Avm2FileRefExt*) fr.u.obj->native_ext,
		                       swf_dialog_file_name(0));
		avm2_array_set(ctx, e->file_list, 0, fr);
	}
	fileref_bare_event(ctx, o, "select");
}

static Avm2Value frl_browse(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2FileRefListExt* e = frl_ext(act);
	if (e == NULL) return avm2_bool(0);
	int select = fileref_filters_select(act, act->argc > 0 ? act->args[0]
	                                                       : avm2_null());
	// Set SYNCHRONOUSLY, before the dialog resolves: `fileList` is already the
	// empty Array on the line right after browse() returns.
	e->file_list = avm2_array_new(ctx, 0);
	fileref_root(e->self);
	swf_dialog_queue(frl_browse_resolve, e->self, select);
	return avm2_bool(1);
}

// A FileReference or FileReferenceList that has ever opened a dialog stays
// rooted: while one is in flight the mock's queue is the only thing holding
// it, and afterwards its selection is still script-reachable state.
static void gc_mark_roots_file(void)
{
	for (int i = 0; i < g_live_fileref_count; i++)
	{
		Avm2Object* o = g_live_filerefs[i];
		if (o == NULL) continue;
		avm2_gc_mark_object(o);
		if (o->cls != NULL && o->cls == g_fileref_list_class
		    && o->native_ext != NULL)
		{
			Avm2FileRefListExt* e = (Avm2FileRefListExt*) o->native_ext;
			if (e->file_list != NULL) avm2_gc_mark_object(e->file_list);
		}
	}
}

static void register_file_reference(Avm2Context* ctx)
{
	// flash.net.FileFilter — final, three String slots behind get/set pairs.
	{
		Avm2Class* cls = avm2_builtin_class(ctx, "flash.net", "FileFilter",
		                                    ctx->builtins.object_class);
		cls->instance_init.fn = ff_init;
		cls->instance_init.debug_name = "FileFilter";
		cls->native_ext_size = sizeof(Avm2FileFilterExt);
		cls->flags |= AVM2_CLASS_FLAG_SEALED | AVM2_CLASS_FLAG_FINAL;
		avm2_builtin_add_getset(ctx, cls, "description", ff_get_desc, ff_set_desc);
		avm2_builtin_add_getset(ctx, cls, "extension", ff_get_ext, ff_set_ext);
		avm2_builtin_add_getset(ctx, cls, "macType", ff_get_mac, ff_set_mac);
		g_filefilter_class = cls;
	}

	// flash.net.FileReference. SEALED is load-bearing: `extension` is AIR-only
	// and must raise #1069, not read as undefined.
	{
		Avm2Class* cls = avm2_builtin_class(ctx, "flash.net", "FileReference",
		                                    ctx->builtins.event_dispatcher_class);
		cls->instance_init.fn = fileref_init;
		cls->instance_init.debug_name = "FileReference";
		cls->native_ext_size = sizeof(Avm2FileRefExt);
		cls->flags |= AVM2_CLASS_FLAG_SEALED;
		avm2_builtin_add_getter(ctx, cls, "creationDate", fileref_get_date);
		avm2_builtin_add_getter(ctx, cls, "modificationDate", fileref_get_date);
		avm2_builtin_add_getter(ctx, cls, "creator", fileref_get_creator);
		avm2_builtin_add_getter(ctx, cls, "data", fileref_get_data);
		avm2_builtin_add_getter(ctx, cls, "name", fileref_get_name);
		avm2_builtin_add_getter(ctx, cls, "size", fileref_get_size);
		avm2_builtin_add_getter(ctx, cls, "type", fileref_get_type);
		avm2_builtin_add_method(ctx, cls, "browse", fileref_browse);
		avm2_builtin_add_method(ctx, cls, "load", fileref_load);
		avm2_builtin_add_method(ctx, cls, "save", fileref_save);
		// The rest of the non-AIR surface is stubbed in Ruffle too; a sealed
		// class has to carry it or a call would raise #1069 instead of doing
		// nothing.
		avm2_builtin_add_method(ctx, cls, "cancel", net2_noop);
		avm2_builtin_add_method(ctx, cls, "download", net2_noop);
		avm2_builtin_add_method(ctx, cls, "upload", net2_noop);
		g_fileref_class = cls;
	}

	// flash.net.FileReferenceList.
	{
		Avm2Class* cls = avm2_builtin_class(ctx, "flash.net", "FileReferenceList",
		                                    ctx->builtins.event_dispatcher_class);
		cls->instance_init.fn = frl_init;
		cls->instance_init.debug_name = "FileReferenceList";
		cls->native_ext_size = sizeof(Avm2FileRefListExt);
		cls->flags |= AVM2_CLASS_FLAG_SEALED;
		avm2_builtin_add_getter(ctx, cls, "fileList", frl_get_file_list);
		avm2_builtin_add_method(ctx, cls, "browse", frl_browse);
		g_fileref_list_class = cls;
	}
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

void avm2_register_net_transport(Avm2Context* ctx)
{
	register_socket(ctx);
	register_xml_socket(ctx);
	register_net_connection(ctx);
	register_net_stream(ctx);
	register_file_reference(ctx);

	register_local_connection(ctx);

	// flash.net.Responder.
	{
		Avm2Class* cls = avm2_builtin_class(ctx, "flash.net", "Responder",
		                                    ctx->builtins.object_class);
		cls->instance_init.fn = responder_init;
		cls->instance_init.debug_name = "Responder";
		cls->native_ext_size = sizeof(Avm2ResponderExt);
		g_responder_class = cls;
	}

	// flash.net.DatagramSocket — `[API("668")] public class DatagramSocket {}`,
	// i.e. AIR 2.0 only. Its existence is the whole of what is observable, in
	// BOTH directions: air_datagram_socket (runtime = "AIR") traces the
	// instance, and air_hidden_lookup asserts that getDefinitionByName on it
	// THROWS under a plain Flash Player runtime. Registering it unconditionally
	// just trades one test for the other, so it is gated on the harness's
	// -DSWF_RUNTIME_AIR.
#ifdef SWF_RUNTIME_AIR
	avm2_builtin_class(ctx, "flash.net", "DatagramSocket",
	                   ctx->builtins.object_class);
#endif

	// flash.media.AVNetworkingParams — three Booleans whose only subtlety is
	// that readSetCookieHeader defaults to TRUE while the other two default
	// to false.
	{
		Avm2Class* cls = avm2_builtin_class(ctx, "flash.media",
		                                    "AVNetworkingParams",
		                                    ctx->builtins.object_class);
		cls->instance_init.fn = avnp_init;
		cls->instance_init.debug_name = "AVNetworkingParams";
		cls->native_ext_size = sizeof(Avm2AvnpExt);
		avm2_builtin_add_getset(ctx, cls, "forceNativeNetworking",
		                        avnp_get_force, avnp_set_force);
		avm2_builtin_add_getset(ctx, cls, "readSetCookieHeader",
		                        avnp_get_cookie, avnp_set_cookie);
		avm2_builtin_add_getset(ctx, cls, "useCookieHeaderForAllRequests",
		                        avnp_get_all_req, avnp_set_all_req);
		avm2_builtin_add_getset(ctx, cls, "networkDownVerificationUrl",
		                        avnp_get_url, avnp_set_url);
		avm2_builtin_add_getset(ctx, cls, "appendRandomQueryParameter",
		                        avnp_get_qp, avnp_set_qp);
	}
}
