// LZMA "alone" container over the vendored LZMA SDK.
//
// The alone format is a 13-byte header — 1 properties byte, u32le dictionary
// size, u64le uncompressed size (all-ones = unknown, terminated by an end
// marker) — followed by the raw LZMA stream. That is what Ruffle's
// lzma_rs::lzma_{compress,decompress} produce and consume, and so what
// ByteArray.compress("lzma") / uncompress("lzma") must speak.
//
// This header deliberately exposes nothing from 7zTypes.h (which defines
// Byte / Bool / True / False and would collide with runtime code).

#ifndef SWF_LZMA_ALONE_H
#define SWF_LZMA_ALONE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Both calls malloc() the output buffer; the caller frees it with free().
// Return 1 on success (*out owned by the caller, *out_len its size), 0 on
// failure (*out NULL). An empty input is legal for both: compressing yields a
// bare header, decompressing a header that declares zero bytes yields zero
// bytes.
int swf_lzma_alone_compress(const uint8_t* in, size_t in_len,
                            uint8_t** out, size_t* out_len);
int swf_lzma_alone_decompress(const uint8_t* in, size_t in_len,
                              uint8_t** out, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif
