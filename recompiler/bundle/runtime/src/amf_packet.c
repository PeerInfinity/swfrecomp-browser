// amf_packet.c — AMF0 remoting-packet framing (see amf_packet.h).
//
// No VM types appear here on purpose: AVM1's NetConnection.call (avm1_amf.c)
// serializes its own AMF0 values and hands the byte ranges over, and AVM2's
// NetConnection will do the same.

#include <stdlib.h>
#include <string.h>

#include <amf_packet.h>

void amf_buf_init(AmfBuf* b)
{
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

void amf_buf_free(AmfBuf* b)
{
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

void amf_buf_put(AmfBuf* b, const void* p, size_t n)
{
	if (n == 0) return;
	if (b->len + n > b->cap)
	{
		size_t nc = b->cap ? b->cap * 2 : 64;
		while (nc < b->len + n) nc *= 2;
		unsigned char* g = (unsigned char*) realloc(b->data, nc);
		if (g == NULL) return;   // out of memory: drop the append, stay consistent
		b->data = g;
		b->cap = nc;
	}
	memcpy(b->data + b->len, p, n);
	b->len += n;
}

void amf_buf_u8(AmfBuf* b, unsigned v)
{
	unsigned char c = (unsigned char) (v & 0xFF);
	amf_buf_put(b, &c, 1);
}

void amf_buf_u16be(AmfBuf* b, unsigned v)
{
	unsigned char c[2];
	c[0] = (unsigned char) ((v >> 8) & 0xFF);
	c[1] = (unsigned char) (v & 0xFF);
	amf_buf_put(b, c, 2);
}

void amf_buf_u32be(AmfBuf* b, unsigned long v)
{
	unsigned char c[4];
	c[0] = (unsigned char) ((v >> 24) & 0xFF);
	c[1] = (unsigned char) ((v >> 16) & 0xFF);
	c[2] = (unsigned char) ((v >> 8) & 0xFF);
	c[3] = (unsigned char) (v & 0xFF);
	amf_buf_put(b, c, 4);
}

void amf_buf_f64be(AmfBuf* b, double d)
{
	unsigned char c[8];
	unsigned long long bits;
	memcpy(&bits, &d, 8);
	for (int i = 0; i < 8; i++)
		c[i] = (unsigned char) ((bits >> (56 - 8 * i)) & 0xFF);
	amf_buf_put(b, c, 8);
}

void amf_buf_short_str(AmfBuf* b, const char* s, size_t n)
{
	if (n > 65535) n = 65535;
	amf_buf_u16be(b, (unsigned) n);
	amf_buf_put(b, s, n);
}

void amf_packet_build(AmfBuf* out, unsigned version,
                      const AmfPacketHeader* headers, size_t header_count,
                      const AmfPacketMessage* messages, size_t message_count)
{
	amf_buf_u16be(out, version);
	amf_buf_u16be(out, (unsigned) header_count);
	for (size_t i = 0; i < header_count; i++)
	{
		amf_buf_short_str(out, headers[i].name, strlen(headers[i].name));
		amf_buf_u8(out, headers[i].must_understand ? 1 : 0);
		amf_buf_u32be(out, (unsigned long) headers[i].value_len);
		amf_buf_put(out, headers[i].value, headers[i].value_len);
	}
	amf_buf_u16be(out, (unsigned) message_count);
	for (size_t i = 0; i < message_count; i++)
	{
		amf_buf_short_str(out, messages[i].target, strlen(messages[i].target));
		amf_buf_short_str(out, messages[i].response, strlen(messages[i].response));
		amf_buf_u32be(out, (unsigned long) messages[i].body_len);
		amf_buf_put(out, messages[i].body, messages[i].body_len);
	}
}

// --- response parsing ---

typedef struct
{
	const unsigned char* p;
	size_t n;
	size_t pos;
	int bad;
} PktRd;

static unsigned pk_u16(PktRd* r)
{
	if (r->pos + 2 > r->n) { r->bad = 1; return 0; }
	unsigned v = ((unsigned) r->p[r->pos] << 8) | r->p[r->pos + 1];
	r->pos += 2;
	return v;
}

static unsigned long pk_u32(PktRd* r)
{
	if (r->pos + 4 > r->n) { r->bad = 1; return 0; }
	unsigned long v = ((unsigned long) r->p[r->pos] << 24)
	                | ((unsigned long) r->p[r->pos + 1] << 16)
	                | ((unsigned long) r->p[r->pos + 2] << 8)
	                | (unsigned long) r->p[r->pos + 3];
	r->pos += 4;
	return v;
}

static const unsigned char* pk_bytes(PktRd* r, size_t n)
{
	if (r->pos + n > r->n) { r->bad = 1; return NULL; }
	const unsigned char* q = r->p + r->pos;
	r->pos += n;
	return q;
}

int amf_packet_parse(const unsigned char* p, size_t n,
                     AmfPacketMsgFn msg_fn, void* user)
{
	PktRd r;
	r.p = p; r.n = n; r.pos = 0; r.bad = 0;

	(void) pk_u16(&r);                 // version
	unsigned hcount = pk_u16(&r);
	for (unsigned i = 0; i < hcount && !r.bad; i++)
	{
		unsigned name_len = pk_u16(&r);
		(void) pk_bytes(&r, name_len);
		if (r.pos + 1 > r.n) { r.bad = 1; break; }
		r.pos += 1;                    // must_understand
		unsigned long vlen = pk_u32(&r);
		(void) pk_bytes(&r, (size_t) vlen);
	}
	if (r.bad) return 0;

	unsigned mcount = pk_u16(&r);
	for (unsigned i = 0; i < mcount && !r.bad; i++)
	{
		unsigned tlen = pk_u16(&r);
		const unsigned char* target = pk_bytes(&r, tlen);
		unsigned rlen = pk_u16(&r);
		(void) pk_bytes(&r, rlen);
		unsigned long blen = pk_u32(&r);
		const unsigned char* body = pk_bytes(&r, (size_t) blen);
		if (r.bad) break;
		if (msg_fn != NULL)
		{
			if (msg_fn((const char*) target, tlen, body, (size_t) blen, user))
				return 1;
		}
	}
	return r.bad ? 0 : 1;
}
