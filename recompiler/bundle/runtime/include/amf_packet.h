#pragma once

// amf_packet.h — AMF0 remoting-packet framing, deliberately VM-agnostic.
//
// The wire format below is the one Flash Player's NetConnection speaks to a
// Flash Remoting gateway. It carries AMF0 *values*, but knows nothing about how
// those values are produced: the caller hands over already-serialized byte
// ranges. That keeps this file usable from both VMs — AVM1's
// NetConnection.call (avm1_amf.c) and, later, AVM2's flash.net.NetConnection.
//
// Packet layout (all integers big-endian):
//
//   u16 version                        always 0
//   u16 header_count
//   per header:
//       u16 name_len, name bytes
//       u8  must_understand
//       u32 value_len                  EXACT length, never 0xFFFFFFFF
//       AMF0 value
//   u16 message_count
//   per message:
//       u16 target_len, target bytes   e.g. "test.method" out, "/1/onResult" in
//       u16 response_len, response      "/N" out (1-based per flush), "" in
//       u32 body_len                   EXACT length
//       AMF0 value                     outbound: always a StrictArray of args
//
// Ruffle's equivalents live in core/src/net_connection.rs (write side) and
// flash-lso's read side; Flash's own bytes are pinned by the AVM1
// netconnection_* tests' `log_fetch` expectations.

#include <stddef.h>

// ------------------------------------------------------------------
// Growable byte buffer (malloc-backed; free with amf_buf_free)
// ------------------------------------------------------------------

typedef struct AmfBuf
{
	unsigned char* data;
	size_t len;
	size_t cap;
} AmfBuf;

void amf_buf_init(AmfBuf* b);
void amf_buf_free(AmfBuf* b);
void amf_buf_put(AmfBuf* b, const void* p, size_t n);
void amf_buf_u8(AmfBuf* b, unsigned v);
void amf_buf_u16be(AmfBuf* b, unsigned v);
void amf_buf_u32be(AmfBuf* b, unsigned long v);
void amf_buf_f64be(AmfBuf* b, double d);
// u16 length prefix + bytes (the AMF0 short-string shape, also used for the
// header/target/response names in the packet framing).
void amf_buf_short_str(AmfBuf* b, const char* s, size_t n);

// ------------------------------------------------------------------
// Packet assembly
// ------------------------------------------------------------------

#define AMF_PACKET_MAX_NAME 256

typedef struct AmfPacketHeader
{
	char name[AMF_PACKET_MAX_NAME];
	int must_understand;
	const unsigned char* value;   // serialized AMF0 value
	size_t value_len;
} AmfPacketHeader;

typedef struct AmfPacketMessage
{
	char target[AMF_PACKET_MAX_NAME];
	char response[32];
	const unsigned char* body;    // serialized AMF0 value (StrictArray of args)
	size_t body_len;
} AmfPacketMessage;

// Append a complete packet to `out`. `version` is the leading u16: 0 for an
// AMF0 connection, 3 when objectEncoding is AMF3 (in which case the caller has
// already prefixed each value with the AMF0 0x11 avmplus escape).
void amf_packet_build(AmfBuf* out, unsigned version,
                      const AmfPacketHeader* headers, size_t header_count,
                      const AmfPacketMessage* messages, size_t message_count);

// ------------------------------------------------------------------
// Response parsing
// ------------------------------------------------------------------

// Called once per message in a response packet, in packet order. `body` points
// into the caller's buffer (no copy) and spans exactly the declared body
// length. Return non-zero to stop iterating.
typedef int (*AmfPacketMsgFn)(const char* target, size_t target_len,
                              const unsigned char* body, size_t body_len,
                              void* user);

// Walk a response packet. Returns 1 when the whole packet parsed cleanly, 0 on
// a malformed/truncated packet (Flash silently ignores those, so callers just
// drop the response). Headers are skipped: Flash ignores inbound headers, which
// is exactly what netconnection_send_remote's "Spookily ignored by flash!"
// response header asserts.
int amf_packet_parse(const unsigned char* p, size_t n,
                     AmfPacketMsgFn msg_fn, void* user);
