// flash.utils.ByteArray — port of Ruffle core/src/avm2/bytearray.rs
// (ByteArrayStorage), globals/flash/utils/byte_array.rs (natives), and
// ByteArray.as, plus the Endian / CompressionAlgorithm constant classes.
//
// Storage is a plain grow-on-write byte buffer with a read/write cursor.
// Reads past the end throw EOFError 2030; writes that would exceed u32
// range throw RangeError 2006. Strings read out of the buffer are
// UTF-8-validated with Rust's from_utf8_lossy "maximal subparts" rule so
// byte dumps match Ruffle exactly. readUTF/readUTFBytes strip a UTF-8 BOM
// and truncate at the first NUL (an FP bug we replicate); toString strips
// a UTF-8 BOM or decodes the whole buffer as UTF-16 on an FF FE / FE FF
// BOM, and does NOT truncate at NULs.
//
// compress/uncompress use the already-linked zlib ("zlib" = zlib wrapper,
// "deflate" = raw stream, level 9 = Ruffle's Compression::best); "lzma"
// goes through the vendored LZMA SDK (third_party/lzma) in the LZMA-alone
// container, which is what Ruffle's lzma_rs speaks — compress yields an
// empty buffer on encoder error, uncompress throws 2058 on any decode
// failure. readMultiByte/writeMultiByte
// handle utf-8 and utf-16le/be natively and delegate other charsets
// (shift-jis) to iconv; unknown labels fall back to UTF-8 exactly like
// encoding_rs's for_label().unwrap_or(UTF_8).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>
#ifndef __EMSCRIPTEN__
#include <errno.h>
#include <iconv.h>
#endif

#include <avm2/avm2_abc.h>
#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <memory/heap.h>

#include <avm2/avm2_gc.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>

#include "lzma_alone.h"

Avm2ByteArrayExt* avm2_bytearray_ext_of(Avm2Value v)
{
	if (v.kind != AVM2_VALUE_OBJECT || v.u.obj == NULL) return NULL;
	Avm2Context* ctx = avm2_get_context();
	if (ctx->builtins.bytearray_class == NULL) return NULL;
	for (Avm2Class* c = v.u.obj->cls; c != NULL; c = c->super_class)
	{
		if (c == ctx->builtins.bytearray_class)
		{
			return (Avm2ByteArrayExt*) v.u.obj->native_ext;
		}
	}
	return NULL;
}

// GC (Stage 11): free the backing byte buffer a swept ByteArray owns
// (avm2_alloc'd). No-op for non-ByteArrays / empty buffers.
void avm2_bytearray_gc_free_ext(Avm2Context* ctx, Avm2Object* o)
{
	Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(avm2_object_value(o));
	if (ba == NULL || ba->bytes == NULL) return;
	heap_free(ctx->app, ba->bytes);
	ba->bytes = NULL;
}

static _Noreturn void throw_eof(Avm2Context* ctx)
{
	avm2_throw_error(ctx, ctx->builtins.eof_error_class,
	                 "Error #2030: End of file was encountered.");
}

static _Noreturn void throw_2006(Avm2Context* ctx)
{
	avm2_throw_error(ctx, ctx->builtins.range_error_class,
	                 "Error #2006: The supplied index is out of bounds.");
}

static _Noreturn void throw_2007(Avm2Context* ctx, const char* param)
{
	avm2_throw_error(ctx, ctx->builtins.type_error_class,
	                 "Error #2007: Parameter %s must be non-null.", param);
}

static _Noreturn void throw_2008(Avm2Context* ctx, const char* param)
{
	avm2_throw_error(ctx, ctx->builtins.argument_error_class,
	                 "Error #2008: Parameter %s must be one of the accepted "
	                 "values.", param);
}

static _Noreturn void throw_2058(Avm2Context* ctx)
{
	avm2_throw_error(ctx, ctx->builtins.io_error_class,
	                 "Error #2058: There was an error decompressing the data.");
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

void avm2_bytearray_set_length_public(Avm2Context* ctx, Avm2ByteArrayExt* ba,
                                      uint32_t new_len);

// avmplus caps a ByteArray's backing store at MMgc's kMaxObjectSize. Both
// failure routes in tamarin-redux's ByteArrayGlue.cpp — mmfx_new_array_opt()
// returning null (:147, the 32-bit case) and the minimumCapacity check (:132)
// — surface to AS3 as Error #1000. The corpus pins the boundary at
// 0xFFFFC000 = 2^32 - 0x4000: as3/ByteArray/ByteArray_bug662851_{32,64}bit
// walk 0xFFFFC000..0xFFFFF000 and expect #1000 on every step. The check must
// come before any allocation: our capacity doubling below cannot represent
// such a size (and heap exhaustion is fatal, not catchable).
#define BA_MAX_LENGTH 0xFFFFC000u

static void ba_set_length(Avm2Context* ctx, Avm2ByteArrayExt* ba, uint32_t new_len)
{
	if (new_len >= BA_MAX_LENGTH)
	{
		avm2_throw_error(ctx, ctx->builtins.error_class,
		                 "Error #1000: The system is out of memory.");
	}
	if (new_len > ba->cap)
	{
		// Double until it fits. new_len < BA_MAX_LENGTH < 2^32 and the
		// doubling starts at 32, so new_cap tops out at 2^31 before the
		// guard below hands it the exact size — without that arm the
		// multiply overflows to 0 and the loop spins forever.
		uint32_t new_cap = ba->cap == 0 ? 32 : ba->cap;
		while (new_cap < new_len)
		{
			if (new_cap > 0x40000000u) { new_cap = new_len; break; }
			new_cap *= 2;
		}
		uint8_t* grown = avm2_alloc(ctx, new_cap);
		if (ba->len > 0) memcpy(grown, ba->bytes, ba->len);
		// Sole owner: every reader re-reads ba->bytes (domain memory too —
		// avm2_mops.c mops_window), and the self-copy paths (writeBytes of
		// itself) memmove AFTER the growth. It used to leak on every growth.
		if (ba->bytes != NULL) heap_free(ctx->app, ba->bytes);
		ba->bytes = grown;
		ba->cap = new_cap;
	}
	if (new_len > ba->len)
	{
		memset(ba->bytes + ba->len, 0, new_len - ba->len);
	}
	ba->len = new_len;
	if (ba->position > new_len) ba->position = new_len;
}

// Write at an arbitrary offset, growing (Ruffle write_at).
static void ba_write_at(Avm2Context* ctx, Avm2ByteArrayExt* ba, const uint8_t* buf,
                        uint32_t n, uint32_t offset)
{
	if ((uint64_t) offset + n > 0xFFFFFFFFull)
	{
		throw_2006(ctx);
	}
	uint32_t new_len = offset + n;
	if (ba->len < new_len) ba_set_length(ctx, ba, new_len);
	if (n > 0) memmove(ba->bytes + offset, buf, n);
}

static void ba_write_bytes(Avm2Context* ctx, Avm2ByteArrayExt* ba,
                           const uint8_t* buf, uint32_t n)
{
	ba_write_at(ctx, ba, buf, n, ba->position);
	ba->position += n;
}

// Read at an arbitrary offset (Ruffle read_at): EOF if out of range.
static const uint8_t* ba_read_at(Avm2Context* ctx, const Avm2ByteArrayExt* ba,
                                 uint32_t n, uint32_t offset)
{
	if ((uint64_t) offset + n > ba->len)
	{
		throw_eof(ctx);
	}
	return ba->bytes + offset;
}

static const uint8_t* ba_read_bytes(Avm2Context* ctx, Avm2ByteArrayExt* ba, uint32_t n)
{
	const uint8_t* p = ba_read_at(ctx, ba, n, ba->position);
	ba->position += n;
	return p;
}

static uint32_t ba_bytes_available(const Avm2ByteArrayExt* ba)
{
	return (ba->position <= ba->len) ? ba->len - ba->position : 0;
}

// Endian-aware scalar I/O.
static void ba_write_scalar(Avm2Context* ctx, Avm2ByteArrayExt* ba,
                            const uint8_t* be_bytes, uint32_t n)
{
	uint8_t buf[8];
	for (uint32_t i = 0; i < n; i++)
	{
		buf[i] = ba->endian_little ? be_bytes[n - 1 - i] : be_bytes[i];
	}
	ba_write_bytes(ctx, ba, buf, n);
}

static void ba_read_scalar(Avm2Context* ctx, Avm2ByteArrayExt* ba,
                           uint8_t* out_be, uint32_t n)
{
	const uint8_t* p = ba_read_bytes(ctx, ba, n);
	for (uint32_t i = 0; i < n; i++)
	{
		out_be[i] = ba->endian_little ? p[n - 1 - i] : p[i];
	}
}

static void ba_write_u16(Avm2Context* ctx, Avm2ByteArrayExt* ba, uint16_t v)
{
	uint8_t be[2] = { (uint8_t) (v >> 8), (uint8_t) v };
	ba_write_scalar(ctx, ba, be, 2);
}

static void ba_write_u32(Avm2Context* ctx, Avm2ByteArrayExt* ba, uint32_t v)
{
	uint8_t be[4] = { (uint8_t) (v >> 24), (uint8_t) (v >> 16),
	                  (uint8_t) (v >> 8), (uint8_t) v };
	ba_write_scalar(ctx, ba, be, 4);
}

static uint16_t ba_read_u16(Avm2Context* ctx, Avm2ByteArrayExt* ba)
{
	uint8_t be[2];
	ba_read_scalar(ctx, ba, be, 2);
	return (uint16_t) ((be[0] << 8) | be[1]);
}

static uint32_t ba_read_u32(Avm2Context* ctx, Avm2ByteArrayExt* ba)
{
	uint8_t be[4];
	ba_read_scalar(ctx, ba, be, 4);
	return ((uint32_t) be[0] << 24) | ((uint32_t) be[1] << 16)
	       | ((uint32_t) be[2] << 8) | be[3];
}

void avm2_bytearray_set_length_public(Avm2Context* ctx, Avm2ByteArrayExt* ba,
                                      uint32_t new_len)
{
	ba_set_length(ctx, ba, new_len);
}

// ---------------------------------------------------------------------------
// UTF-8 lossy validation (Rust String::from_utf8_lossy, maximal subparts)
// ---------------------------------------------------------------------------

static const char UTF8_REPLACEMENT[3] = { (char) 0xEF, (char) 0xBF, (char) 0xBD };

// Returns the length in bytes of the valid UTF-8 sequence at p (1-4), or 0
// if invalid; *consumed = bytes of the maximal valid subpart to skip.
static uint32_t utf8_seq_len(const uint8_t* p, uint32_t avail, uint32_t* consumed)
{
	uint8_t b0 = p[0];
	uint32_t need;
	uint8_t lo = 0x80, hi = 0xBF;
	if (b0 < 0x80) { *consumed = 1; return 1; }
	else if (b0 >= 0xC2 && b0 <= 0xDF) need = 1;
	else if (b0 == 0xE0) { need = 2; lo = 0xA0; }
	else if (b0 >= 0xE1 && b0 <= 0xEC) need = 2;
	else if (b0 == 0xED) { need = 2; hi = 0x9F; }
	else if (b0 >= 0xEE && b0 <= 0xEF) need = 2;
	else if (b0 == 0xF0) { need = 3; lo = 0x90; }
	else if (b0 >= 0xF1 && b0 <= 0xF3) need = 3;
	else if (b0 == 0xF4) { need = 3; hi = 0x8F; }
	else { *consumed = 1; return 0; }

	for (uint32_t i = 1; i <= need; i++)
	{
		if (i >= avail) { *consumed = i; return 0; }
		uint8_t b = p[i];
		// The special lo/hi range applies to the FIRST continuation only.
		uint8_t l = (i == 1) ? lo : 0x80;
		uint8_t h = (i == 1) ? hi : 0xBF;
		if (b < l || b > h) { *consumed = i; return 0; }
	}
	*consumed = need + 1;
	return need + 1;
}

static const Avm2String* utf8_lossy(Avm2Context* ctx, const uint8_t* bytes, uint32_t n)
{
	// Worst case: every byte invalid -> 3x expansion.
	char* buf = avm2_alloc(ctx, n * 3 + 1);
	uint32_t out = 0;
	uint32_t i = 0;
	while (i < n)
	{
		uint32_t consumed;
		uint32_t len = utf8_seq_len(bytes + i, n - i, &consumed);
		if (len > 0)
		{
			memcpy(buf + out, bytes + i, len);
			out += len;
			i += len;
		}
		else
		{
			memcpy(buf + out, UTF8_REPLACEMENT, 3);
			out += 3;
			i += consumed;
		}
	}
	buf[out] = '\0';
	{ const Avm2String* r_ = avm2_string_new(ctx, buf, out); heap_free(ctx->app, buf); return r_; }  // copied: the buffer is scratch
}

// avmplus's lenient UTF-8 decoder (Ruffle wstr DecodeAvmUtf8, used by
// AvmString::new_utf8_bytes): a lone continuation/invalid byte decodes to
// its own byte VALUE (0x89 -> U+0089), multibyte sequences take at most 3
// continuation bytes and fall back to the lead byte's value when malformed
// or overlong. readUTF/readUTFBytes/toString use this — NOT strict lossy.
static uint32_t avm_utf8_next(const uint8_t* p, uint32_t avail, uint32_t* consumed)
{
	uint8_t first = p[0];
	uint32_t ones = 0;
	while (ones < 8 && (first & (0x80u >> ones))) ones++;
	*consumed = 1;
	if (ones <= 1) return first;
	uint32_t mb = ones - 1;
	if (mb > 3) mb = 3;
	if (avail - 1 < mb) return first;
	uint32_t ch = (ones >= 8) ? 0 : (uint32_t) (first & (0xFFu >> ones));
	for (uint32_t i = 1; i <= mb; i++)
	{
		uint8_t b = p[i];
		if ((b & 0xC0) != 0x80) return first;
		ch = (ch << 6) | (b & 0x3F);
	}
	if (ch < 0x80) return first;
	*consumed = 1 + mb;
	return ch;
}

static uint32_t utf8_encode(uint32_t cp, char* out);

static const Avm2String* avm_utf8_lenient(Avm2Context* ctx, const uint8_t* bytes,
                                          uint32_t n)
{
	char* buf = avm2_alloc(ctx, n * 4 + 1);
	uint32_t out = 0;
	uint32_t i = 0;
	while (i < n)
	{
		uint32_t consumed;
		uint32_t cp = avm_utf8_next(bytes + i, n - i, &consumed);
		i += consumed;
		// CESU-8 in, canonical WTF-8 out: the decoder happily yields a bare
		// surrogate for an ED A0..BF sequence, and a high one immediately
		// followed by a low one is ONE astral character, not two units — so
		// fold it back into the 3 bytes just written. avm2/invalid_utf8
		// writes ED A0 BD ED B0 8C into a ByteArray and traces "🐌".
		if (cp >= 0xDC00 && cp <= 0xDFFF && out >= 3)
		{
			unsigned char* p = (unsigned char*) buf + out - 3;
			if (p[0] == 0xED && p[1] >= 0xA0 && p[1] <= 0xAF)
			{
				uint32_t hi = ((uint32_t) (p[0] & 0x0F) << 12)
				            | ((uint32_t) (p[1] & 0x3F) << 6)
				            | (uint32_t) (p[2] & 0x3F);
				out -= 3;
				cp = 0x10000 + ((hi - 0xD800) << 10) + (cp - 0xDC00);
			}
		}
		out += utf8_encode(cp, buf + out);
	}
	buf[out] = '\0';
	{ const Avm2String* r_ = avm2_string_new(ctx, buf, out); heap_free(ctx->app, buf); return r_; }  // copied: the buffer is scratch
}

// Encode one code point as UTF-8.
static uint32_t utf8_encode(uint32_t cp, char* out)
{
	if (cp < 0x80) { out[0] = (char) cp; return 1; }
	if (cp < 0x800)
	{
		out[0] = (char) (0xC0 | (cp >> 6));
		out[1] = (char) (0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000)
	{
		out[0] = (char) (0xE0 | (cp >> 12));
		out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char) (0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char) (0xF0 | (cp >> 18));
	out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char) (0x80 | (cp & 0x3F));
	return 4;
}

// Decode UTF-16 bytes (chunks of 2; a trailing odd byte is dropped) into a
// UTF-8 string. Unpaired surrogates become U+FFFD.
static const Avm2String* utf16_to_utf8(Avm2Context* ctx, const uint8_t* bytes,
                                       uint32_t n, int little)
{
	uint32_t units = n / 2;
	char* buf = avm2_alloc(ctx, units * 3 + 4);
	uint32_t out = 0;
	for (uint32_t i = 0; i < units; i++)
	{
		uint16_t u = little
			? (uint16_t) (bytes[i * 2] | (bytes[i * 2 + 1] << 8))
			: (uint16_t) ((bytes[i * 2] << 8) | bytes[i * 2 + 1]);
		uint32_t cp = u;
		if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units)
		{
			uint16_t u2 = little
				? (uint16_t) (bytes[(i + 1) * 2] | (bytes[(i + 1) * 2 + 1] << 8))
				: (uint16_t) ((bytes[(i + 1) * 2] << 8) | bytes[(i + 1) * 2 + 1]);
			if (u2 >= 0xDC00 && u2 <= 0xDFFF)
			{
				cp = 0x10000 + (((uint32_t) (u - 0xD800)) << 10) + (u2 - 0xDC00);
				i++;
			}
			else
			{
				cp = 0xFFFD;
			}
		}
		else if (u >= 0xD800 && u <= 0xDFFF)
		{
			cp = 0xFFFD;
		}
		out += utf8_encode(cp, buf + out);
	}
	buf[out] = '\0';
	{ const Avm2String* r_ = avm2_string_new(ctx, buf, out); heap_free(ctx->app, buf); return r_; }  // copied: the buffer is scratch
}

// readUTF/readUTFBytes semantics (Ruffle read_utf_bytes): strip a UTF-8
// BOM, truncate at the first NUL.
static const Avm2String* utf_bytes_to_string(Avm2Context* ctx, const uint8_t* bytes,
                                             uint32_t n)
{
	if (n >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
	{
		bytes += 3;
		n -= 3;
	}
	for (uint32_t i = 0; i < n; i++)
	{
		if (bytes[i] == '\0')
		{
			n = i;
			break;
		}
	}
	return avm_utf8_lenient(ctx, bytes, n);
}

// ---------------------------------------------------------------------------
// Charset conversion (readMultiByte / writeMultiByte)
// ---------------------------------------------------------------------------

enum { CS_UTF8, CS_UTF16LE, CS_UTF16BE, CS_ICONV };

// Map a WHATWG-ish charset label; unknown labels fall back to UTF-8
// (encoding_rs for_label().unwrap_or(UTF_8)). Returns the class and, for
// CS_ICONV, the iconv encoding name.
static int charset_classify(const Avm2String* label, const char** iconv_name)
{
	char low[64];
	uint32_t n = label->len < 63 ? label->len : 63;
	for (uint32_t i = 0; i < n; i++)
	{
		char c = label->utf8[i];
		low[i] = (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c;
	}
	low[n] = '\0';
	if (strcmp(low, "utf-16le") == 0 || strcmp(low, "utf-16") == 0
	    || strcmp(low, "unicode") == 0)
	{
		return CS_UTF16LE;
	}
	if (strcmp(low, "utf-16be") == 0) return CS_UTF16BE;
	if (strcmp(low, "shift-jis") == 0 || strcmp(low, "shift_jis") == 0
	    || strcmp(low, "sjis") == 0 || strcmp(low, "s-jis") == 0
	    || strcmp(low, "s_jis") == 0 || strcmp(low, "ms_kanji") == 0
	    || strcmp(low, "csshiftjis") == 0 || strcmp(low, "windows-31j") == 0
	    || strcmp(low, "x-sjis") == 0)
	{
		*iconv_name = "SHIFT_JIS";
		return CS_ICONV;
	}
	if (strcmp(low, "euc-jp") == 0) { *iconv_name = "EUC-JP"; return CS_ICONV; }
	if (strcmp(low, "big5") == 0) { *iconv_name = "BIG5"; return CS_ICONV; }
	if (strcmp(low, "gbk") == 0 || strcmp(low, "gb2312") == 0)
	{
		*iconv_name = "GBK";
		return CS_ICONV;
	}
	if (strcmp(low, "euc-kr") == 0) { *iconv_name = "EUC-KR"; return CS_ICONV; }
	if (strncmp(low, "iso-8859-", 9) == 0 || strncmp(low, "windows-12", 10) == 0)
	{
		// iconv understands these names directly (upper-case them back).
		static char name[64];
		for (uint32_t i = 0; i <= n; i++)
		{
			char c = low[i];
			name[i] = (c >= 'a' && c <= 'z') ? (char) (c - 32) : c;
		}
		*iconv_name = name;
		return CS_ICONV;
	}
	return CS_UTF8;  // incl. "utf-8", "", unknown labels
}

#ifndef __EMSCRIPTEN__
// iconv decode `from` -> UTF-8 with U+FFFD on invalid sequences.
static const Avm2String* iconv_decode(Avm2Context* ctx, const char* enc,
                                      const uint8_t* bytes, uint32_t n)
{
	iconv_t cd = iconv_open("UTF-8", enc);
	if (cd == (iconv_t) -1) return utf8_lossy(ctx, bytes, n);
	uint32_t cap = n * 4 + 8;
	char* buf = avm2_alloc(ctx, cap);
	char* in = (char*) bytes;
	size_t in_left = n;
	char* out = buf;
	size_t out_left = cap - 1;
	while (in_left > 0)
	{
		size_t rc = iconv(cd, &in, &in_left, &out, &out_left);
		if (rc != (size_t) -1) break;
		if (errno == EILSEQ || errno == EINVAL)
		{
			if (out_left < 3) break;
			memcpy(out, UTF8_REPLACEMENT, 3);
			out += 3;
			out_left -= 3;
			if (errno == EINVAL) break;  // truncated trailing sequence
			in++;
			in_left--;
		}
		else
		{
			break;
		}
	}
	iconv_close(cd);
	*out = '\0';
	{ const Avm2String* r_ = avm2_string_new(ctx, buf, (uint32_t) (out - buf)); heap_free(ctx->app, buf); return r_; }  // copied: the buffer is scratch
}

// iconv encode UTF-8 -> `to`; undecodable chars are dropped.
static uint32_t iconv_encode(Avm2Context* ctx, const char* enc, const Avm2String* s,
                             uint8_t** out_bytes)
{
	iconv_t cd = iconv_open(enc, "UTF-8");
	if (cd == (iconv_t) -1)
	{
		*out_bytes = (uint8_t*) s->utf8;
		return s->len;
	}
	uint32_t cap = s->len * 4 + 8;
	char* buf = avm2_alloc(ctx, cap);
	char* in = (char*) s->utf8;
	size_t in_left = s->len;
	char* out = buf;
	size_t out_left = cap;
	while (in_left > 0)
	{
		size_t rc = iconv(cd, &in, &in_left, &out, &out_left);
		if (rc != (size_t) -1) break;
		if (errno == EILSEQ || errno == EINVAL)
		{
			in++;
			in_left--;
		}
		else
		{
			break;
		}
	}
	iconv_close(cd);
	*out_bytes = (uint8_t*) buf;
	return (uint32_t) (out - buf);
}
#endif

// UTF-8 string -> UTF-16 bytes.
static uint32_t utf8_to_utf16(Avm2Context* ctx, const Avm2String* s, int little,
                              uint8_t** out_bytes)
{
	uint8_t* buf = avm2_alloc(ctx, s->len * 2 + 4);
	uint32_t out = 0;
	uint32_t i = 0;
	const uint8_t* b = (const uint8_t*) s->utf8;
	while (i < s->len)
	{
		uint32_t consumed;
		uint32_t len = utf8_seq_len(b + i, s->len - i, &consumed);
		uint32_t cp = 0xFFFD;
		if (len == 1) cp = b[i];
		else if (len == 2) cp = ((b[i] & 0x1Fu) << 6) | (b[i + 1] & 0x3F);
		else if (len == 3)
		{
			cp = ((b[i] & 0x0Fu) << 12) | ((b[i + 1] & 0x3Fu) << 6)
			     | (b[i + 2] & 0x3F);
		}
		else if (len == 4)
		{
			cp = ((b[i] & 0x07u) << 18) | ((b[i + 1] & 0x3Fu) << 12)
			     | ((b[i + 2] & 0x3Fu) << 6) | (b[i + 3] & 0x3F);
		}
		i += (len > 0) ? len : consumed;
		uint16_t units[2];
		uint32_t un = 1;
		if (cp >= 0x10000)
		{
			cp -= 0x10000;
			units[0] = (uint16_t) (0xD800 + (cp >> 10));
			units[1] = (uint16_t) (0xDC00 + (cp & 0x3FF));
			un = 2;
		}
		else
		{
			units[0] = (uint16_t) cp;
		}
		for (uint32_t k = 0; k < un; k++)
		{
			if (little)
			{
				buf[out++] = (uint8_t) units[k];
				buf[out++] = (uint8_t) (units[k] >> 8);
			}
			else
			{
				buf[out++] = (uint8_t) (units[k] >> 8);
				buf[out++] = (uint8_t) units[k];
			}
		}
	}
	*out_bytes = buf;
	return out;
}

// ---------------------------------------------------------------------------
// Natives
// ---------------------------------------------------------------------------

// flash.net.Socket is this same IDataInput/IDataOutput surface over a pair of
// buffers rather than one, so avm2_net.c registers the bodies below verbatim
// and supplies its own ext through this hook. `write_dir` picks the buffer:
// the read half backs every read*, the write half every write*. A ByteArray
// `this` resolves before the hook is ever consulted and is unaffected.
// More than one class borrows the bodies (Socket, URLStream), so the hook is a
// chain: each resolver claims only its own class and returns NULL otherwise,
// which makes the order immaterial.
#define MAX_BA_ALT_RESOLVERS 4
static Avm2BaAltResolver g_ba_alt_resolvers[MAX_BA_ALT_RESOLVERS];
static int g_ba_alt_resolver_count;

void avm2_bytearray_set_alt_resolver(Avm2BaAltResolver fn)
{
	if (fn == NULL) return;
	for (int i = 0; i < g_ba_alt_resolver_count; i++)
		if (g_ba_alt_resolvers[i] == fn) return;
	if (g_ba_alt_resolver_count < MAX_BA_ALT_RESOLVERS)
		g_ba_alt_resolvers[g_ba_alt_resolver_count++] = fn;
}

Avm2ByteArrayExt* avm2_bytearray_ext_dir(Avm2Activation* act, int write_dir)
{
	Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(act->this_val);
	if (ba != NULL) return ba;
	// A hook may throw on its own terms (an unconnected Socket raises
	// IOError #2002 before any buffer exists).
	for (int i = 0; i < g_ba_alt_resolver_count; i++)
	{
		ba = g_ba_alt_resolvers[i](act, write_dir);
		if (ba != NULL) return ba;
	}
	return NULL;
}

static Avm2ByteArrayExt* this_ba_dir(Avm2Activation* act, int write_dir)
{
	Avm2ByteArrayExt* ba = avm2_bytearray_ext_dir(act, write_dir);
	if (ba == NULL)
	{
		avm2_throw_error(act->ctx, act->ctx->builtins.type_error_class,
		                 "Error #1004: Method ByteArray was invoked on an "
		                 "incompatible object.");
	}
	return ba;
}

// Read direction is the default: every neutral accessor (position, length,
// endian, clear, compress, …) is ByteArray-only, and Socket never registers
// one of them.
static Avm2ByteArrayExt* this_ba(Avm2Activation* act)
{
	return this_ba_dir(act, 0);
}

static Avm2ByteArrayExt* this_ba_w(Avm2Activation* act)
{
	return this_ba_dir(act, 1);
}

static Avm2Value arg_or_undef(Avm2Activation* act, uint32_t i)
{
	return (i < act->argc) ? act->args[i] : avm2_undefined();
}

// get_string_non_null: null/undefined -> error 2007.
static const Avm2String* arg_string_non_null(Avm2Activation* act, uint32_t i,
                                             const char* pname)
{
	Avm2Value v = arg_or_undef(act, i);
	if (v.kind == AVM2_VALUE_NULL || v.kind == AVM2_VALUE_UNDEFINED)
	{
		throw_2007(act->ctx, pname);
	}
	return avm2_coerce_to_string(act->ctx, v);
}

static Avm2Value ba_get_length(Avm2Activation* act)
{
	return avm2_uint_value(this_ba(act)->len);
}

static Avm2Value ba_set_length_native(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba(act);
	ba_set_length(act->ctx, ba, avm2_coerce_to_u32(act->ctx, arg_or_undef(act, 0)));
	return avm2_undefined();
}

static Avm2Value ba_get_position(Avm2Activation* act)
{
	return avm2_uint_value(this_ba(act)->position);
}

static Avm2Value ba_set_position(Avm2Activation* act)
{
	this_ba(act)->position = avm2_coerce_to_u32(act->ctx, arg_or_undef(act, 0));
	return avm2_undefined();
}

static Avm2Value ba_get_bytes_available(Avm2Activation* act)
{
	return avm2_uint_value(ba_bytes_available(this_ba(act)));
}

static Avm2Value ba_get_endian(Avm2Activation* act)
{
	return avm2_string(avm2_string_from_literal(
		act->ctx, this_ba(act)->endian_little ? "littleEndian" : "bigEndian"));
}

static Avm2Value ba_set_endian(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba(act);
	const Avm2String* s = arg_string_non_null(act, 0, "endian");
	if (s->len == 9 && memcmp(s->utf8, "bigEndian", 9) == 0)
	{
		ba->endian_little = 0;
	}
	else if (s->len == 12 && memcmp(s->utf8, "littleEndian", 12) == 0)
	{
		ba->endian_little = 1;
	}
	else
	{
		throw_2008(act->ctx, "endian");
	}
	return avm2_undefined();
}

static Avm2Value ba_get_object_encoding(Avm2Activation* act)
{
	return avm2_uint_value(this_ba(act)->object_encoding);
}

static Avm2Value ba_set_object_encoding(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba(act);
	uint32_t enc = avm2_coerce_to_u32(act->ctx, arg_or_undef(act, 0));
	if (enc == 0 || enc == 3)
	{
		ba->object_encoding = (uint8_t) enc;
	}
	else
	{
		throw_2008(act->ctx, "objectEncoding");
	}
	return avm2_undefined();
}

static Avm2Value ba_static_get_default_encoding(Avm2Activation* act)
{
	(void) act;
	return avm2_uint_value(avm2_get_context()->bytearray_default_encoding);
}

static Avm2Value ba_static_set_default_encoding(Avm2Activation* act)
{
	uint32_t enc = avm2_coerce_to_u32(act->ctx, arg_or_undef(act, 0));
	if (enc == 0 || enc == 3)
	{
		avm2_get_context()->bytearray_default_encoding = (uint8_t) enc;
	}
	return avm2_undefined();
}

static Avm2Value ba_clear(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba(act);
	ba->len = 0;
	ba->position = 0;
	return avm2_undefined();
}

// Decode a byte run to a string honouring a leading byte-order mark: UTF-8
// (EF BB BF), UTF-16LE (FF FE) or UTF-16BE (FE FF); no BOM means lenient
// UTF-8. Ruffle's strip_bom + AvmString::new_utf8_bytes — shared so that
// ByteArray.toString and URLLoader/URLStream text decoding cannot drift.
const Avm2String* avm2_strip_bom(Avm2Context* ctx, const uint8_t* b, uint32_t n)
{
	if (b == NULL) n = 0;
	if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF)
	{
		return avm_utf8_lenient(ctx, b + 3, n - 3);
	}
	if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE)
	{
		return utf16_to_utf8(ctx, b + 2, n - 2, 1);
	}
	if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF)
	{
		return utf16_to_utf8(ctx, b + 2, n - 2, 0);
	}
	return avm_utf8_lenient(ctx, (n != 0) ? b : (const uint8_t*) "", n);
}

static Avm2Value ba_to_string(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba(act);
	return avm2_string(avm2_strip_bom(act->ctx, ba->bytes, ba->len));
}

// --- scalar reads ---

static Avm2Value ba_read_boolean(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba(act);
	const uint8_t* p = ba_read_bytes(act->ctx, ba, 1);
	return avm2_bool(p[0] != 0);
}

static Avm2Value ba_read_byte(Avm2Activation* act)
{
	const uint8_t* p = ba_read_bytes(act->ctx, this_ba(act), 1);
	return avm2_integer((int8_t) p[0]);
}

static Avm2Value ba_read_unsigned_byte(Avm2Activation* act)
{
	const uint8_t* p = ba_read_bytes(act->ctx, this_ba(act), 1);
	return avm2_integer(p[0]);
}

static Avm2Value ba_read_short(Avm2Activation* act)
{
	return avm2_integer((int16_t) ba_read_u16(act->ctx, this_ba(act)));
}

static Avm2Value ba_read_unsigned_short(Avm2Activation* act)
{
	return avm2_integer(ba_read_u16(act->ctx, this_ba(act)));
}

static Avm2Value ba_read_int(Avm2Activation* act)
{
	return avm2_integer((int32_t) ba_read_u32(act->ctx, this_ba(act)));
}

static Avm2Value ba_read_unsigned_int(Avm2Activation* act)
{
	return avm2_uint_value(ba_read_u32(act->ctx, this_ba(act)));
}

static Avm2Value ba_read_float(Avm2Activation* act)
{
	uint32_t bits = ba_read_u32(act->ctx, this_ba(act));
	float f;
	memcpy(&f, &bits, 4);
	return avm2_number((double) f);
}

static Avm2Value ba_read_double(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba(act);
	uint8_t be[8];
	ba_read_scalar(act->ctx, ba, be, 8);
	uint64_t bits = 0;
	for (int i = 0; i < 8; i++) bits = (bits << 8) | be[i];
	double d;
	memcpy(&d, &bits, 8);
	return avm2_number(d);
}

// --- scalar writes ---

static Avm2Value ba_write_boolean(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba_w(act);
	uint8_t v = avm2_coerce_to_boolean(arg_or_undef(act, 0)) ? 1 : 0;
	ba_write_bytes(act->ctx, ba, &v, 1);
	return avm2_undefined();
}

static Avm2Value ba_write_byte(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba_w(act);
	uint8_t v = (uint8_t) avm2_coerce_to_i32(act->ctx, arg_or_undef(act, 0));
	ba_write_bytes(act->ctx, ba, &v, 1);
	return avm2_undefined();
}

static Avm2Value ba_write_short(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba_w(act);
	ba_write_u16(act->ctx, ba,
	             (uint16_t) avm2_coerce_to_i32(act->ctx, arg_or_undef(act, 0)));
	return avm2_undefined();
}

static Avm2Value ba_write_int(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba_w(act);
	ba_write_u32(act->ctx, ba,
	             (uint32_t) avm2_coerce_to_i32(act->ctx, arg_or_undef(act, 0)));
	return avm2_undefined();
}

static Avm2Value ba_write_unsigned_int(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba_w(act);
	ba_write_u32(act->ctx, ba, avm2_coerce_to_u32(act->ctx, arg_or_undef(act, 0)));
	return avm2_undefined();
}

static Avm2Value ba_write_float(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba_w(act);
	float f = (float) avm2_coerce_to_number(act->ctx, arg_or_undef(act, 0));
	uint32_t bits;
	memcpy(&bits, &f, 4);
	ba_write_u32(act->ctx, ba, bits);
	return avm2_undefined();
}

static Avm2Value ba_write_double(Avm2Activation* act)
{
	Avm2ByteArrayExt* ba = this_ba_w(act);
	double d = avm2_coerce_to_number(act->ctx, arg_or_undef(act, 0));
	uint64_t bits;
	memcpy(&bits, &d, 8);
	uint8_t be[8];
	for (int i = 0; i < 8; i++) be[i] = (uint8_t) (bits >> (56 - i * 8));
	ba_write_scalar(act->ctx, ba, be, 8);
	return avm2_undefined();
}

// --- bulk transfer ---

static Avm2Value ba_write_bytes_native(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2ByteArrayExt* ba = this_ba_w(act);
	Avm2Value arg0 = arg_or_undef(act, 0);
	if (arg0.kind == AVM2_VALUE_NULL || arg0.kind == AVM2_VALUE_UNDEFINED)
	{
		throw_2007(ctx, "bytes");
	}
	Avm2ByteArrayExt* src = avm2_bytearray_ext_of(arg0);
	if (src == NULL)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1034: Type Coercion failed: cannot convert "
		                 "value to flash.utils.ByteArray.");
	}
	uint32_t offset = avm2_coerce_to_u32(ctx, arg_or_undef(act, 1));
	uint32_t length = avm2_coerce_to_u32(ctx, arg_or_undef(act, 2));
	uint32_t amnt = (length != 0)
		? length
		: (src->len > offset ? src->len - offset : 0);
	if (src == ba)
	{
		// Same array: validate the source range then memmove within.
		if ((uint64_t) offset + amnt > ba->len) throw_eof(ctx);
		uint32_t pos = ba->position;
		if ((uint64_t) pos + amnt > 0xFFFFFFFFull) throw_2006(ctx);
		if (ba->len < pos + amnt) ba_set_length(ctx, ba, pos + amnt);
		memmove(ba->bytes + pos, ba->bytes + offset, amnt);
		ba->position += amnt;
	}
	else
	{
		const uint8_t* p = ba_read_at(ctx, src, amnt, offset);
		ba_write_bytes(ctx, ba, p, amnt);
	}
	return avm2_undefined();
}

static Avm2Value ba_read_bytes_native(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2ByteArrayExt* ba = this_ba(act);
	Avm2Value arg0 = arg_or_undef(act, 0);
	if (arg0.kind == AVM2_VALUE_NULL || arg0.kind == AVM2_VALUE_UNDEFINED)
	{
		throw_2007(ctx, "bytes");
	}
	Avm2ByteArrayExt* dst = avm2_bytearray_ext_of(arg0);
	if (dst == NULL)
	{
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #1034: Type Coercion failed: cannot convert "
		                 "value to flash.utils.ByteArray.");
	}
	uint32_t offset = avm2_coerce_to_u32(ctx, arg_or_undef(act, 1));
	uint32_t length = avm2_coerce_to_u32(ctx, arg_or_undef(act, 2));
	uint32_t amnt = (length != 0) ? length : ba_bytes_available(ba);
	if (dst == ba)
	{
		uint32_t pos = ba->position;
		if ((uint64_t) pos + amnt > ba->len) throw_eof(ctx);
		if ((uint64_t) offset + amnt > 0xFFFFFFFFull) throw_2006(ctx);
		if (ba->len < offset + amnt) ba_set_length(ctx, ba, offset + amnt);
		memmove(ba->bytes + offset, ba->bytes + pos, amnt);
	}
	else
	{
		const uint8_t* p = ba_read_bytes(ctx, ba, amnt);
		ba_write_at(ctx, dst, p, amnt, offset);
	}
	return avm2_undefined();
}

// --- strings ---

static Avm2Value ba_write_utf(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2ByteArrayExt* ba = this_ba_w(act);
	const Avm2String* s = arg_string_non_null(act, 0, "value");
	if (s->len > 0xFFFF) throw_2006(ctx);
	ba_write_u16(ctx, ba, (uint16_t) s->len);
	ba_write_bytes(ctx, ba, (const uint8_t*) s->utf8, s->len);
	return avm2_undefined();
}

static Avm2Value ba_read_utf(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2ByteArrayExt* ba = this_ba(act);
	uint16_t len = ba_read_u16(ctx, ba);
	const uint8_t* p = ba_read_bytes(ctx, ba, len);
	return avm2_string(utf_bytes_to_string(ctx, p, len));
}

static Avm2Value ba_write_utf_bytes(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2ByteArrayExt* ba = this_ba_w(act);
	const Avm2String* s = arg_string_non_null(act, 0, "value");
	ba_write_bytes(ctx, ba, (const uint8_t*) s->utf8, s->len);
	return avm2_undefined();
}

static Avm2Value ba_read_utf_bytes(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2ByteArrayExt* ba = this_ba(act);
	uint32_t len = avm2_coerce_to_u32(ctx, arg_or_undef(act, 0));
	const uint8_t* p = ba_read_bytes(ctx, ba, len);
	return avm2_string(utf_bytes_to_string(ctx, p, len));
}

static Avm2Value ba_write_multi_byte(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2ByteArrayExt* ba = this_ba_w(act);
	const Avm2String* s = arg_string_non_null(act, 0, "value");
	const Avm2String* label = arg_string_non_null(act, 1, "charSet");
	const char* iconv_name = NULL;
	int cs = charset_classify(label, &iconv_name);
	uint8_t* bytes = (uint8_t*) s->utf8;
	uint32_t n = s->len;
	if (cs == CS_UTF16LE || cs == CS_UTF16BE)
	{
		n = utf8_to_utf16(ctx, s, cs == CS_UTF16LE, &bytes);
	}
#ifndef __EMSCRIPTEN__
	else if (cs == CS_ICONV)
	{
		n = iconv_encode(ctx, iconv_name, s, &bytes);
	}
#endif
	ba_write_bytes(ctx, ba, bytes, n);
	// A transcoded buffer is scratch (the passthrough case writes the
	// string's own bytes, which are not ours to free).
	if (bytes != (uint8_t*) s->utf8) heap_free(ctx->app, bytes);
	return avm2_undefined();
}

static Avm2Value ba_read_multi_byte(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2ByteArrayExt* ba = this_ba(act);
	uint32_t len = avm2_coerce_to_u32(ctx, arg_or_undef(act, 0));
	const Avm2String* label = arg_string_non_null(act, 1, "charSet");
	const uint8_t* p = ba_read_bytes(ctx, ba, len);
	const char* iconv_name = NULL;
	int cs = charset_classify(label, &iconv_name);
	const Avm2String* s;
	if (cs == CS_UTF16LE || cs == CS_UTF16BE)
	{
		s = utf16_to_utf8(ctx, p, len, cs == CS_UTF16LE);
	}
#ifndef __EMSCRIPTEN__
	else if (cs == CS_ICONV)
	{
		s = iconv_decode(ctx, iconv_name, p, len);
	}
#endif
	else
	{
		s = utf8_lossy(ctx, p, len);
	}
	// Flash truncates at the first NUL after checking the byte length.
	for (uint32_t i = 0; i < s->len; i++)
	{
		if (s->utf8[i] == '\0')
		{
			return avm2_string(avm2_string_new(ctx, s->utf8, i));
		}
	}
	return avm2_string(s);
}

// --- compression ---

enum { ALG_ZLIB, ALG_DEFLATE, ALG_LZMA };

static int parse_algorithm(Avm2Activation* act, const Avm2String* s)
{
	if (s->len == 4 && memcmp(s->utf8, "zlib", 4) == 0) return ALG_ZLIB;
	if (s->len == 7 && memcmp(s->utf8, "deflate", 7) == 0) return ALG_DEFLATE;
	if (s->len == 4 && memcmp(s->utf8, "lzma", 4) == 0) return ALG_LZMA;
	throw_2058(act->ctx);
}

static Avm2Value ba_compress(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2ByteArrayExt* ba = this_ba(act);
	const Avm2String* s = (act->argc > 0)
		? arg_string_non_null(act, 0, "algorithm")
		: avm2_string_from_literal(ctx, "zlib");
	int alg = parse_algorithm(act, s);
	uint8_t* out = NULL;
	uint32_t out_len = 0;
	if (alg == ALG_LZMA)
	{
		uint8_t* lz = NULL;
		size_t lz_len = 0;
		static const uint8_t empty = 0;
		if (swf_lzma_alone_compress(ba->bytes != NULL ? ba->bytes : &empty,
		                            ba->len, &lz, &lz_len))
		{
			out = avm2_alloc(ctx, (uint32_t) lz_len);
			memcpy(out, lz, lz_len);
			out_len = (uint32_t) lz_len;
			free(lz);
		}
		// Ruffle clears the buffer when the encoder errors.
	}
	else if (ba->len > 0)
	{
		z_stream strm;
		memset(&strm, 0, sizeof(strm));
		int wbits = (alg == ALG_ZLIB) ? 15 : -15;
		if (deflateInit2(&strm, 9, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) == Z_OK)
		{
			uLong bound = deflateBound(&strm, ba->len);
			out = avm2_alloc(ctx, (uint32_t) bound);
			strm.next_in = ba->bytes;
			strm.avail_in = ba->len;
			strm.next_out = out;
			strm.avail_out = (uInt) bound;
			if (deflate(&strm, Z_FINISH) == Z_STREAM_END)
			{
				out_len = (uint32_t) strm.total_out;
			}
			else
			{
				out_len = 0;  // Ruffle: empty buffer on error
			}
			deflateEnd(&strm);
		}
	}
	else
	{
		// Compressing an empty buffer still yields a valid stream.
		z_stream strm;
		memset(&strm, 0, sizeof(strm));
		int wbits = (alg == ALG_ZLIB) ? 15 : -15;
		if (deflateInit2(&strm, 9, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) == Z_OK)
		{
			out = avm2_alloc(ctx, 64);
			strm.next_out = out;
			strm.avail_out = 64;
			if (deflate(&strm, Z_FINISH) == Z_STREAM_END)
			{
				out_len = (uint32_t) strm.total_out;
			}
			deflateEnd(&strm);
		}
	}
	ba->len = 0;
	ba->position = 0;
	if (out_len > 0) ba_write_bytes(ctx, ba, out, out_len);
	if (out != NULL) heap_free(ctx->app, out);  // scratch: copied in above
	ba->position = ba->len;
	return avm2_undefined();
}

static Avm2Value ba_uncompress(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2ByteArrayExt* ba = this_ba(act);
	const Avm2String* s = (act->argc > 0)
		? arg_string_non_null(act, 0, "algorithm")
		: avm2_string_from_literal(ctx, "zlib");
	int alg = parse_algorithm(act, s);
	if (alg == ALG_LZMA)
	{
		uint8_t* raw = NULL;
		size_t raw_len = 0;
		// lzma_rs has no empty-input special case: a stream shorter than the
		// 13-byte header is simply a decode error.
		if (!swf_lzma_alone_decompress(ba->bytes, ba->len, &raw, &raw_len))
		{
			throw_2058(ctx);
		}
		ba->len = 0;
		ba->position = 0;
		if (raw_len > 0) ba_write_bytes(ctx, ba, raw, (uint32_t) raw_len);
		free(raw);
		ba->position = 0;
		return avm2_undefined();
	}
	// flate2 succeeds with empty output on an empty input stream.
	if (ba->len == 0)
	{
		ba->position = 0;
		return avm2_undefined();
	}
	z_stream strm;
	memset(&strm, 0, sizeof(strm));
	int wbits = (alg == ALG_ZLIB) ? 15 : -15;
	if (inflateInit2(&strm, wbits) != Z_OK) throw_2058(ctx);
	uint32_t cap = ba->len * 4 + 64;
	uint8_t* out = avm2_alloc(ctx, cap);
	uint32_t out_len = 0;
	strm.next_in = ba->bytes;
	strm.avail_in = ba->len;
	int ok = 0;
	for (;;)
	{
		strm.next_out = out + out_len;
		strm.avail_out = cap - out_len;
		int rc = inflate(&strm, Z_NO_FLUSH);
		out_len = (uint32_t) strm.total_out;
		if (rc == Z_STREAM_END)
		{
			ok = 1;
			break;
		}
		if (rc == Z_OK || rc == Z_BUF_ERROR)
		{
			if (strm.avail_out == 0)
			{
				uint8_t* grown = avm2_alloc(ctx, cap * 2);
				memcpy(grown, out, out_len);
				heap_free(ctx->app, out);
				out = grown;
				cap *= 2;
				continue;
			}
			// No progress possible (truncated stream).
			break;
		}
		break;
	}
	inflateEnd(&strm);
	if (!ok) { heap_free(ctx->app, out); throw_2058(ctx); }
	ba->len = 0;
	ba->position = 0;
	if (out_len > 0) ba_write_bytes(ctx, ba, out, out_len);
	heap_free(ctx->app, out);  // scratch: copied in above
	ba->position = 0;
	return avm2_undefined();
}

static Avm2Value ba_deflate(Avm2Activation* act)
{
	Avm2Value arg = avm2_string(avm2_string_from_literal(act->ctx, "deflate"));
	Avm2Activation sub = *act;
	sub.args = &arg;
	sub.argc = 1;
	return ba_compress(&sub);
}

static Avm2Value ba_inflate(Avm2Activation* act)
{
	Avm2Value arg = avm2_string(avm2_string_from_literal(act->ctx, "deflate"));
	Avm2Activation sub = *act;
	sub.args = &arg;
	sub.argc = 1;
	return ba_uncompress(&sub);
}

// readObject/writeObject (AMF) live in avm2_amf.c.
Avm2Value avm2_amf_read_object(Avm2Activation* act);
Avm2Value avm2_amf_write_object(Avm2Activation* act);

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// Which movie's character tables does `cls` belong to? Same class walk
// avm2_display_child_char_for_class takes (avm2_display.c), answering with the
// DEFINING file's tables. NULL = the main movie, whose chars are the
// avm2_generated_* globals. Needed because a SymbolClass char id is only
// meaningful inside the movie that carried the tag: two movies can define the
// same raw id as different character kinds.
static const Avm2CharInfo* ba_char_info_for_class(Avm2Class* cls, uint16_t char_id)
{
	const Avm2CharInfo* chars = avm2_generated_chars;
	uint32_t count = avm2_generated_char_count;
	for (Avm2Class* c = cls; c != NULL; c = c->super_class)
	{
		if (c->instance_init.file == NULL) continue;  // builtin
		const Avm2MovieTables* t =
			avm2_display_movie_for_abc(c->instance_init.file->data);
		if (t == NULL) continue;                      // the MAIN movie
		chars = t->chars;
		count = t->char_count;
		break;
	}
	for (uint32_t i = 0; i < count; i++)
		if (chars[i].char_id == char_id) return &chars[i];
	return NULL;
}

static void ba_native_init(Avm2Context* ctx, Avm2Object* obj)
{
	Avm2ByteArrayExt* ba = (Avm2ByteArrayExt*) obj->native_ext;
	ba->object_encoding = avm2_get_context()->bytearray_default_encoding;

	// A SymbolClass-bound DefineBinaryData subclass (Flex
	// `[Embed(mimeType="application/octet-stream")]` compiles the asset to a
	// class extending mx.core.ByteArrayAsset -> flash.utils.ByteArray): seed
	// the ByteArray with the embedded bytes, matching Ruffle's ByteArray
	// symbol association (data filled, position reset to 0). A plain
	// `new ByteArray()` (or any class with no binary binding) has char_id 0
	// and is left empty.
	// Both halves are child-aware, for the reason avm2_bitmap.c's
	// embedded_bitmap_for_char documents: g_symbol_map is built once from the
	// MAIN movie's SymbolClass rows, and the payload table read below is the
	// main movie's too, so a DefineBinaryData embedded by a Loader-loaded child
	// resolved to char 0 and seeded an EMPTY ByteArray. Keyed on the DEFINING
	// movie: the bytes live in the tables of the movie whose tag carried them.
	uint16_t char_id = avm2_display_child_char_for_class(ctx, obj->cls);
	if (char_id != 0)
	{
		const Avm2BinaryData* bin = NULL;
		for (uint32_t i = 0; i < avm2_generated_binary_count && bin == NULL; i++)
			if (avm2_generated_binaries[i].char_id == char_id)
				bin = &avm2_generated_binaries[i];
		uint32_t nm = avm2_display_child_movie_count();
		for (uint32_t m = 0; m < nm && bin == NULL; m++)
		{
			const Avm2MovieTables* t = avm2_display_child_movie(m);
			if (t == NULL) continue;
			for (uint32_t i = 0; i < t->binary_count && bin == NULL; i++)
				if (t->binaries[i].char_id == char_id) bin = &t->binaries[i];
		}
		if (bin != NULL && bin->len > 0 && bin->bytes != NULL)
		{
			ba_set_length(ctx, ba, bin->len);
			memcpy(ba->bytes, bin->bytes, bin->len);
			ba->position = 0;
		}
		else if (bin == NULL)
		{
			// The class IS SymbolClass-bound, but to something that is not a
			// DefineBinaryData. Ruffle's ByteArray allocator
			// (object/bytearray_object.rs) looks the bound character up and
			// raises #2136 ("The SWF file <url> contains invalid data.") for
			// any non-BinaryData kind — avm2/bytearray_bad_symbol_class binds
			// `MyBytes` to a DefineVideoStream and Flash throws there too.
			//
			// The predicate is deliberately "a KNOWN character of a non-binary
			// kind", not "no binary payload found": a DefineBinaryData whose
			// payload the emitter dropped (or a child movie whose binaries[]
			// table is empty) must keep seeding an empty ByteArray rather than
			// start throwing. abc_timeline.cpp records a kind-8 (OTHER)
			// CharInfo for every DefineBinaryData and for nothing else, so
			// kind 8 is exactly "binary" here; an id that names no character
			// at all (bin == NULL, ci == NULL) also stays silent.
			const Avm2CharInfo* ci = ba_char_info_for_class(obj->cls, char_id);
			if (ci != NULL && ci->kind != AVM2_CHAR_OTHER)
			{
#ifdef SWF_URL
				const char* url = SWF_URL;
#else
				const char* url = "";
#endif
				avm2_throw_error(ctx, NULL,
				                 "Error #2136: The SWF file %s contains "
				                 "invalid data.", url);
			}
		}
	}
}

static Avm2Value ba_proto_to_json(Avm2Activation* act)
{
	return avm2_string(avm2_string_from_literal(act->ctx, "ByteArray"));
}

static void ba_add_getset(Avm2Context* ctx, Avm2Class* cls, const char* name,
                          Avm2MethodFn getter, Avm2MethodFn setter)
{
	Avm2PropEntry e;
	memset(&e, 0, sizeof(e));
	e.key = avm2_public_key(name, (uint32_t) strlen(name));
	e.kind = AVM2_PROP_GETSET;
	e.method.fn = getter;
	e.method.debug_name = name;
	e.setter.fn = setter;
	// Flash names an accessor frame "Class/set prop()" — avm2/bytearray_oom
	// pins it for `ByteArray.length`. (Native accessors registered through
	// the shared avm2_builtin_add_getter/setter helpers elsewhere are still
	// unprefixed; nothing in the corpus observes those yet.)
	{
		size_t nlen = strlen(name);
		char* sname = avm2_alloc(ctx, (uint32_t) nlen + 5);
		memcpy(sname, "set ", 4);
		memcpy(sname + 4, name, nlen + 1);
		e.setter.debug_name = sname;
	}
	e.defining_class = cls;
	avm2_vtable_append(ctx, &cls->ivtable, &e);
}

// The IDataInput + IDataOutput method set, shared verbatim with
// flash.net.Socket (avm2_net.c) — the two classes differ only in which buffer
// each direction resolves to, which the alt resolver above decides.
//
// Every method is keyed TWICE: once public, once under the interface namespace
// ("flash.utils:IDataInput" / ":IDataOutput"). ASC compiles a call on an
// interface-typed reference to the interface namespace ALONE — never public —
// and builtin classes never run avm2_class.c's interface-alias pass, which is
// what supplies those keys for ABC classes. Without them, the IDataOutput
// handed to IExternalizable.writeExternal has no callable writeObject
// (from_avmplus as3/AMF/AMFSerializer).
static void ba_add_io(Avm2Context* ctx, Avm2Class* cls, const char* iface,
                      const char* name, Avm2MethodFn fn)
{
	avm2_builtin_add_method(ctx, cls, name, fn);
	Avm2PropEntry e;
	memset(&e, 0, sizeof(e));
	e.key.ns_kind = 0x16;
	e.key.ns_uri = iface;
	e.key.ns_len = (uint32_t) strlen(iface);
	e.key.name = name;
	e.key.name_len = (uint32_t) strlen(name);
	e.kind = AVM2_PROP_METHOD;
	e.method.fn = fn;
	e.method.debug_name = name;
	e.defining_class = cls;
	avm2_vtable_append(ctx, &cls->ivtable, &e);
}

void avm2_bytearray_install_data_input(Avm2Context* ctx, Avm2Class* cls)
{
	#define BA_IN(n, f) ba_add_io(ctx, cls, "flash.utils:IDataInput", n, f)
	BA_IN("readBoolean", ba_read_boolean);
	BA_IN("readByte", ba_read_byte);
	BA_IN("readUnsignedByte", ba_read_unsigned_byte);
	BA_IN("readShort", ba_read_short);
	BA_IN("readUnsignedShort", ba_read_unsigned_short);
	BA_IN("readInt", ba_read_int);
	BA_IN("readUnsignedInt", ba_read_unsigned_int);
	BA_IN("readFloat", ba_read_float);
	BA_IN("readDouble", ba_read_double);
	BA_IN("readBytes", ba_read_bytes_native);
	BA_IN("readUTF", ba_read_utf);
	BA_IN("readUTFBytes", ba_read_utf_bytes);
	BA_IN("readMultiByte", ba_read_multi_byte);
	BA_IN("readObject", avm2_amf_read_object);
	#undef BA_IN
}

void avm2_bytearray_install_data_output(Avm2Context* ctx, Avm2Class* cls)
{
	#define BA_OUT(n, f) ba_add_io(ctx, cls, "flash.utils:IDataOutput", n, f)
	BA_OUT("writeBoolean", ba_write_boolean);
	BA_OUT("writeByte", ba_write_byte);
	BA_OUT("writeShort", ba_write_short);
	BA_OUT("writeInt", ba_write_int);
	BA_OUT("writeUnsignedInt", ba_write_unsigned_int);
	BA_OUT("writeFloat", ba_write_float);
	BA_OUT("writeDouble", ba_write_double);
	BA_OUT("writeBytes", ba_write_bytes_native);
	BA_OUT("writeUTF", ba_write_utf);
	BA_OUT("writeUTFBytes", ba_write_utf_bytes);
	BA_OUT("writeMultiByte", ba_write_multi_byte);
	BA_OUT("writeObject", avm2_amf_write_object);
	#undef BA_OUT
}

void avm2_bytearray_install_data_io(Avm2Context* ctx, Avm2Class* cls)
{
	avm2_bytearray_install_data_input(ctx, cls);
	avm2_bytearray_install_data_output(ctx, cls);
}

void avm2_register_bytearray(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;
	ctx->bytearray_default_encoding = 3;  // AMF3 (SWF9+ default)

	Avm2Class* cls = avm2_builtin_class(ctx, "flash.utils", "ByteArray",
	                                    b->object_class);
	b->bytearray_class = cls;
	cls->flags |= AVM2_CLASS_FLAG_SEALED;
	cls->native_ext_size = sizeof(Avm2ByteArrayExt);
	cls->native_init = ba_native_init;

	ba_add_getset(ctx, cls, "length", ba_get_length, ba_set_length_native);
	ba_add_getset(ctx, cls, "position", ba_get_position, ba_set_position);
	ba_add_getset(ctx, cls, "endian", ba_get_endian, ba_set_endian);
	ba_add_getset(ctx, cls, "objectEncoding", ba_get_object_encoding,
	              ba_set_object_encoding);
	avm2_builtin_add_getter(ctx, cls, "bytesAvailable", ba_get_bytes_available);

	// Static defaultObjectEncoding accessor pair on the class object.
	{
		Avm2VTable* vt = (Avm2VTable*) cls->class_object->vtable;
		if (vt == NULL)
		{
			vt = avm2_alloc(ctx, sizeof(Avm2VTable));
			memset(vt, 0, sizeof(Avm2VTable));
			cls->class_object->vtable = vt;
		}
		Avm2PropEntry e;
		memset(&e, 0, sizeof(e));
		e.key = avm2_public_key("defaultObjectEncoding", 21);
		e.kind = AVM2_PROP_GETSET;
		e.method.fn = ba_static_get_default_encoding;
		e.method.debug_name = "get defaultObjectEncoding";
		e.setter.fn = ba_static_set_default_encoding;
		e.setter.debug_name = "set defaultObjectEncoding";
		e.defining_class = cls;
		avm2_vtable_append(ctx, vt, &e);
	}

	avm2_builtin_add_method(ctx, cls, "clear", ba_clear);
	avm2_builtin_add_method(ctx, cls, "compress", ba_compress);
	avm2_builtin_add_method(ctx, cls, "uncompress", ba_uncompress);
	avm2_builtin_add_method(ctx, cls, "deflate", ba_deflate);
	avm2_builtin_add_method(ctx, cls, "inflate", ba_inflate);
	avm2_builtin_add_method(ctx, cls, "toString", ba_to_string);
	avm2_bytearray_install_data_io(ctx, cls);

	avm2_proto_add_function(ctx, cls->prototype_obj, "toJSON", ba_proto_to_json);
	avm2_proto_add_function(ctx, cls->prototype_obj, "toString", ba_to_string);

	// flash.utils.Endian + flash.utils.CompressionAlgorithm constants.
	{
		Avm2Class* endian = avm2_builtin_class(ctx, "flash.utils", "Endian",
		                                       b->object_class);
		avm2_builtin_add_static_const(ctx, endian, "BIG_ENDIAN",
			avm2_string(avm2_string_from_literal(ctx, "bigEndian")));
		avm2_builtin_add_static_const(ctx, endian, "LITTLE_ENDIAN",
			avm2_string(avm2_string_from_literal(ctx, "littleEndian")));
		Avm2Class* ca = avm2_builtin_class(ctx, "flash.utils",
		                                   "CompressionAlgorithm", b->object_class);
		avm2_builtin_add_static_const(ctx, ca, "ZLIB",
			avm2_string(avm2_string_from_literal(ctx, "zlib")));
		avm2_builtin_add_static_const(ctx, ca, "DEFLATE",
			avm2_string(avm2_string_from_literal(ctx, "deflate")));
		avm2_builtin_add_static_const(ctx, ca, "LZMA",
			avm2_string(avm2_string_from_literal(ctx, "lzma")));
	}
}

// --- Public helpers for the BitmapData module (avm2_bitmap.c) ---
void avm2_bytearray_write_uint_public(Avm2Context* ctx, Avm2ByteArrayExt* ba,
                                      uint32_t v)
{
	ba_write_u32(ctx, ba, v);
}
uint32_t avm2_bytearray_read_uint_public(Avm2Context* ctx, Avm2ByteArrayExt* ba)
{
	return ba_read_u32(ctx, ba);
}
