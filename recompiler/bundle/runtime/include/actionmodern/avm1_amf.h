#pragma once

// avm1_amf.h — AVM1 AMF0 serialization, SharedObject, and the
// NetConnection.call remoting wire.
//
// Three subsystems that all sit on one AMF0 codec, carved out of action.c the
// same way math.c / date.c / registered_class.c were:
//
//   * writer/reader — ActionVar <-> AMF0. Two writer modes: the WIRE mode used
//     by LocalConnection.send and NetConnection.call, and the LSO mode used for
//     SharedObject.getSize()/flush() byte counting. They differ only in how
//     arrays are shaped (see avm1_amf.c).
//   * SharedObject — a real same-run getLocal cache. No disk I/O: Ruffle's test
//     storage backend is an in-memory map that starts empty, so a shipped
//     `.sol` in a test directory is an expected *output*, never an input.
//   * NetConnection.call/addHeader — a per-connection message queue drained
//     once per tick into a single AMF0 packet (amf_packet.h does the framing),
//     reported through the Ruffle test-navigator fetch log; scripted response
//     packets come back from bundled data files.
//
// Byte-level behaviour is pinned by the AVM1 amf_* / netconnection_* /
// localconnection_top_level tests, whose expectations were recorded from Flash
// Player itself. Where Flash and Ruffle disagree (nested ECMAArray-vs-Flash's
// uniform array rule, nested typed objects, the reference-table counter) these
// follow Flash — those tests are `known_failure` upstream precisely because
// Ruffle does not match them.

#include <actionmodern/action_internal.h>

// ------------------------------------------------------------------
// Writer / reader
// ------------------------------------------------------------------

typedef enum
{
	AVM1_AMF_WIRE = 0,   // LocalConnection.send / NetConnection.call
	AVM1_AMF_LSO = 1,    // SharedObject .sol body (getSize / flush)
} Avm1AmfMode;

// Serialize one value. Returns a malloc'd buffer (NULL on failure) and writes
// its length to *out_len. Each call starts a fresh reference table, which is
// what Flash does per top-level value.
unsigned char* avm1AmfWriteValue(SWFAppContext* app_context, ActionVar* v,
                                 Avm1AmfMode mode, size_t* out_len);

// Serialize the `data` bag of a SharedObject as an LSO body (bare name/value
// pairs, no object header or terminator).
unsigned char* avm1AmfWriteLsoBody(SWFAppContext* app_context, ASObject* data,
                                   size_t* out_len);

// Deserialize one value. Malformed input yields UNDEFINED. The reference table
// starts empty per call, so a reference into an earlier sibling value resolves
// to undefined — Flash behaves the same way.
ActionVar avm1AmfReadValue(SWFAppContext* app_context,
                           const unsigned char* p, size_t n);

// ------------------------------------------------------------------
// SharedObject
// ------------------------------------------------------------------

// Install SharedObject.getLocal + the prototype methods that need real
// behaviour (flush / getSize / clear) onto an already-built prototype.
// `ctor` is action.c's SharedObject stub constructor.
void avm1AmfInitSharedObject(SWFAppContext* app_context, ASFunction* ctor);

// ------------------------------------------------------------------
// NetConnection.call / addHeader
// ------------------------------------------------------------------

// Install the real call/addHeader onto NetConnection.prototype.
void avm1AmfInitNetConnection(SWFAppContext* app_context, ASObject* proto);

// Forget the queue + response index for a connection that just (re)connected or
// closed. Called from builtin_nc_connect / builtin_nc_close.
void avm1AmfNetConnectionReset(ASObject* nc);

// Per-tick drain: build one packet per connection with queued messages, report
// it through the fetch log, then dispatch whatever the scripted response says.
// Called from the frame loops right before LocalConnection delivery.
void avm1AmfFlushNetConnections(SWFAppContext* app_context);

// 1 while any connection still has queued messages (keeps the exit gate open).
int avm1AmfNetConnectionsPending(void);

// ------------------------------------------------------------------
// LocalConnection argument marshalling (used by builtin_lc_send)
// ------------------------------------------------------------------

// LocalConnection is a wire channel: arguments are serialized at send() time
// and deserialized at delivery, so the receiver gets a deep copy with Flash's
// own type translations (display objects become undefined, functions become
// plain objects, and so on).
unsigned char* avm1AmfSerializeArg(SWFAppContext* app_context, ActionVar* v,
                                   size_t* out_len);
ActionVar avm1AmfDeserializeArg(SWFAppContext* app_context,
                                const unsigned char* p, size_t n);
