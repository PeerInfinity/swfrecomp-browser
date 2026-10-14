#pragma once

// Scripted socket transport — the runtime half of Ruffle's `socket.json`
// replay (tests/socket-format + tests/framework/src/backends/navigator.rs).
//
// Both VMs share this because both own a socket class (AVM1 XMLSocket, AVM2
// Socket/XMLSocket) and the mock has to behave identically for each. It is
// deliberately VM-agnostic: a connection carries an opaque `target` pointer
// plus a dispatch callback supplied at connect() time, so there is no link
// dependency into src/avm2 (AVM1 tests never compile it).
//
// The implementation lives in src/utils.c, next to swf_log_fetch_queue /
// swf_log_navigate — the rest of the Ruffle test-navigator emulation. That
// keeps it in EVERY build's source list (five hand-maintained build scripts
// besides verify_output.py's) with no per-script wiring to rot.
//
// The contract, straight off Ruffle's mock:
//   * `connect()` succeeds — always and only — when a script file is loaded.
//     With no script the call is a silent no-op and the socket never opens.
//     (`socket_errors` grades exactly that absence.)
//   * The script runs per connection, in order, from a fresh cursor:
//       SEND          server -> player: queues a Data action
//       RECV <bytes>  blocks until the player flushes one message, then
//                     asserts byte equality
//       DISCONNECT    server closes: queues a Close action
//       WAITDISCONNECT blocks until the player closes; a message arriving
//                     instead is a script failure
//   * Actions are DELIVERED at the start of a tick hook and the script is
//     PUMPED right after, so an action queued during tick N is dispatched
//     after frame N+1 runs. That one-tick lag is what every socket test's
//     trace ordering encodes (Ruffle: player.tick -> update_sockets, then
//     executor.run() polls the mock).

#include <stddef.h>

enum
{
	SWF_SOCKET_CONNECT = 0,  // connection established
	SWF_SOCKET_DATA    = 1,  // `data`/`len` are inbound bytes
	SWF_SOCKET_CLOSE   = 2,  // server-side close
};

// Called with the opaque target the connection was opened with. `data`/`len`
// are meaningful only for SWF_SOCKET_DATA.
typedef void (*SwfSocketDispatchFn)(void* target, int action,
                                    const unsigned char* data, size_t len);

// Load the pre-processed script written by verify_output.py's
// preprocess_socket_json. Reading it (even when empty) is what makes
// connect() succeed.
void swf_socket_events_load(const char* path);
// Load from the SWF_SOCKET_SCRIPT environment variable, if set. Idempotent;
// called from both frame loops' startup and from the AVM2 entry point.
void swf_socket_events_init(void);
int  swf_socket_script_loaded(void);

// Open a connection. Returns a handle (>0) or 0 when no script is loaded.
// `host`/`port` are only used for the LOG_FETCH trace, which is emitted here
// on EVERY connect — including when no script is loaded.
int  swf_socket_connect(const char* host, int port, void* target,
                        SwfSocketDispatchFn dispatch);
int  swf_socket_is_connected(int handle);
// Player-initiated close. Queued actions for this handle are dropped.
void swf_socket_close(int handle);
// One flush() worth of bytes = one message on the wire.
void swf_socket_send(int handle, const unsigned char* data, size_t len);

// Deliver queued actions, then advance every connection's script. `owner`
// identifies the calling frame loop (1 = AVM1, 2 = AVM2); the first caller in
// the process wins, so a mixed-VM movie cannot double-pump a tick.
void swf_socket_tick(int owner);

// Nonzero while any connection could still deliver an event (an action is
// queued, or a connection is open). The AVM1 frame loop keeps ticking past the
// last frame while this holds — Ruffle's harness runs its full num_ticks
// unconditionally, ours exits as soon as nothing is asking to run.
int swf_socket_pending(void);
