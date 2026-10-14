// LZMA "alone" container over the vendored LZMA SDK — see lzma_alone.h.
//
// Kept in its own translation unit so that no runtime source has to include
// 7zTypes.h (Byte / Bool / True / False would collide).

#include <stdlib.h>
#include <string.h>

#include "LzmaDec.h"
#include "LzmaEnc.h"
#include "lzma_alone.h"

#define ALONE_HEADER 13
#define ALONE_SIZE_UNKNOWN 0xFFFFFFFFFFFFFFFFull

// Refuse to honour an absurd declared size from an untrusted header before
// allocating for it; a real ByteArray cannot exceed 4 GB anyway.
#define ALONE_MAX_UNPACKED 0x40000000ull  // 1 GiB

// Growth step when the header declares an unknown size.
#define ALONE_CHUNK 65536

static void* alone_alloc(ISzAllocPtr p, size_t size)
{
	(void) p;
	return malloc(size);
}

static void alone_free(ISzAllocPtr p, void* addr)
{
	(void) p;
	free(addr);
}

static const ISzAlloc g_alone_alloc = { alone_alloc, alone_free };

static uint64_t rd_u64le(const uint8_t* p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
	return v;
}

static void wr_u64le(uint8_t* p, uint64_t v)
{
	for (int i = 0; i < 8; i++) p[i] = (uint8_t) (v >> (i * 8));
}

int swf_lzma_alone_compress(const uint8_t* in, size_t in_len,
                            uint8_t** out, size_t* out_len)
{
	*out = NULL;
	*out_len = 0;

	CLzmaEncProps props;
	LzmaEncProps_Init(&props);
	// lc=3 lp=0 pb=2 (properties byte 0x5D) is what lzma_rs and the LZMA.jar
	// blobs in the corpus use. reduceSize lets the encoder shrink its
	// dictionary — and so its working set — to the actual input.
	props.lc = 3;
	props.lp = 0;
	props.pb = 2;
	props.reduceSize = in_len;
	props.numThreads = 1;

	// Worst case for incompressible input is a shade over 1 byte per byte.
	size_t cap = in_len + in_len / 16 + 128;
	uint8_t* buf = (uint8_t*) malloc(ALONE_HEADER + cap);
	if (buf == NULL) return 0;

	uint8_t propsEncoded[LZMA_PROPS_SIZE];
	SizeT propsSize = LZMA_PROPS_SIZE;
	SizeT destLen = cap;
	SRes res = LzmaEncode(buf + ALONE_HEADER, &destLen, in, (SizeT) in_len,
	                      &props, propsEncoded, &propsSize, 0, NULL,
	                      &g_alone_alloc, &g_alone_alloc);
	if (res != SZ_OK || propsSize != LZMA_PROPS_SIZE)
	{
		free(buf);
		return 0;
	}
	memcpy(buf, propsEncoded, LZMA_PROPS_SIZE);
	wr_u64le(buf + LZMA_PROPS_SIZE, (uint64_t) in_len);
	*out = buf;
	*out_len = ALONE_HEADER + (size_t) destLen;
	return 1;
}

// Unknown declared size: decode incrementally into a growing buffer until the
// end marker.
static int alone_decompress_streaming(const uint8_t* src, size_t src_len,
                                      const uint8_t* props,
                                      uint8_t** out, size_t* out_len)
{
	CLzmaDec dec;
	LzmaDec_Construct(&dec);
	if (LzmaDec_Allocate(&dec, props, LZMA_PROPS_SIZE, &g_alone_alloc) != SZ_OK)
	{
		return 0;
	}
	LzmaDec_Init(&dec);

	size_t cap = ALONE_CHUNK;
	uint8_t* buf = (uint8_t*) malloc(cap);
	size_t have = 0;
	size_t used = 0;
	int ok = 0;
	while (buf != NULL)
	{
		if (have == cap)
		{
			if (cap >= ALONE_MAX_UNPACKED) break;
			size_t ncap = cap * 2;
			uint8_t* grown = (uint8_t*) realloc(buf, ncap);
			if (grown == NULL) break;
			buf = grown;
			cap = ncap;
		}
		SizeT destLen = cap - have;
		SizeT srcLen = src_len - used;
		ELzmaStatus status;
		SRes res = LzmaDec_DecodeToBuf(&dec, buf + have, &destLen,
		                               src + used, &srcLen,
		                               LZMA_FINISH_ANY, &status);
		have += destLen;
		used += srcLen;
		if (res != SZ_OK) break;
		if (status == LZMA_STATUS_FINISHED_WITH_MARK
		    || status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)
		{
			ok = 1;
			break;
		}
		if (destLen == 0 && srcLen == 0) break;  // no progress: truncated
	}
	LzmaDec_Free(&dec, &g_alone_alloc);
	if (!ok)
	{
		free(buf);
		return 0;
	}
	*out = buf;
	*out_len = have;
	return 1;
}

int swf_lzma_alone_decompress(const uint8_t* in, size_t in_len,
                              uint8_t** out, size_t* out_len)
{
	*out = NULL;
	*out_len = 0;
	if (in_len < ALONE_HEADER) return 0;

	const uint8_t* props = in;
	uint64_t unpacked = rd_u64le(in + LZMA_PROPS_SIZE);
	const uint8_t* src = in + ALONE_HEADER;
	size_t src_len = in_len - ALONE_HEADER;

	// Reject a bad properties byte here so a corrupt stream fails before we
	// size any buffer from its header.
	CLzmaProps p;
	if (LzmaProps_Decode(&p, props, LZMA_PROPS_SIZE) != SZ_OK) return 0;

	if (unpacked == ALONE_SIZE_UNKNOWN)
	{
		return alone_decompress_streaming(src, src_len, props, out, out_len);
	}
	if (unpacked > ALONE_MAX_UNPACKED) return 0;
	if (unpacked == 0)
	{
		// Nothing to decode; a bare header is a valid empty stream.
		uint8_t* buf = (uint8_t*) malloc(1);
		if (buf == NULL) return 0;
		*out = buf;
		*out_len = 0;
		return 1;
	}

	uint8_t* buf = (uint8_t*) malloc((size_t) unpacked);
	if (buf == NULL) return 0;
	SizeT destLen = (SizeT) unpacked;
	SizeT srcLen = (SizeT) src_len;
	ELzmaStatus status;
	SRes res = LzmaDecode(buf, &destLen, src, &srcLen, props, LZMA_PROPS_SIZE,
	                      LZMA_FINISH_END, &status, &g_alone_alloc);
	if (res != SZ_OK || destLen != (SizeT) unpacked)
	{
		free(buf);
		return 0;
	}
	*out = buf;
	*out_len = (size_t) destLen;
	return 1;
}
