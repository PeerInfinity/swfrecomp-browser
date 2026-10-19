// avm2_jpegxr.c — JPEG-XR decode (jxrlib) for Loader content and Stage3D ATF.
//
// The only translation unit that includes jxrlib's headers: they define
// Bool / U8 / max / Call / FailIf and friends, so nothing else may see them.
// Without SWF_HAVE_LIBJXR (no-graphics, WASM, a box without libjxr-dev) every
// entry point is a stub that reports "not decoded", which is exactly the
// pre-JPEG-XR behaviour: Loader content stays null, ATF uploads stay blank.
//
// Ruffle's `jpegxr` crate (ruffle-rs/jpegxr @86cc1749) wraps the same jxrlib,
// so the pixel path is mirrored call for call:
//   * ImageDecode::with_reader      -> PKImageDecode_Create_WMP + Initialize
//   * ImageDecode::convert_to_tiff  -> uAlphaMode = 2, a PKFormatConverter to
//     the LOOKUP_BACKWARD_TIF twin of the source format, then
//     PKImageEncode_Transcode's Copy (the TIFF writer stores those rows
//     verbatim, so the TIFF round trip is skipped and the rows are read here)
//   * image::DynamicImage::to_rgba8 -> expand Gray8 / RGB8 / RGBA8 rows
//   * ImageDecode::copy_all         -> PKImageDecode Copy in the source format

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <avm2/avm2_jpegxr.h>

// One BC3 (DXT5) block -> 4x4 RGBA8, Mesa's util_format s3tc decode
// (texcompress_s3tc_tmp.h): 565 endpoints bit-replicated to 8 bits, colour
// thirds and alpha sevenths/fifths by truncating integer division. BC3 colour
// is always the four-colour mode. Outside SWF_HAVE_LIBJXR: ATF format 5
// (raw DXT5, avm2_stage3d.c) needs no JPEG-XR.
void avm2_bc3_decode_block(const uint8_t* b, uint8_t* dst, uint32_t dst_stride)
{
	uint8_t a[8];
	a[0] = b[0];
	a[1] = b[1];
	for (int c = 2; c < 8; c++)
	{
		if (a[0] > a[1])
			a[c] = (uint8_t) ((a[0] * (8 - c) + a[1] * (c - 1)) / 7);
		else if (c < 6)
			a[c] = (uint8_t) ((a[0] * (6 - c) + a[1] * (c - 1)) / 5);
		else
			a[c] = (c == 6) ? 0 : 255;
	}
	uint64_t abits = 0;
	for (int i = 0; i < 6; i++) abits |= (uint64_t) b[2 + i] << (8 * i);

	uint32_t c0 = b[8] | (b[9] << 8);
	uint32_t c1 = b[10] | (b[11] << 8);
	uint8_t col[4][3];
	col[0][0] = (uint8_t) (((c0 >> 8) & 0xF8) | ((c0 >> 13) & 0x7));
	col[0][1] = (uint8_t) (((c0 >> 3) & 0xFC) | ((c0 >> 9) & 0x3));
	col[0][2] = (uint8_t) (((c0 << 3) & 0xF8) | ((c0 >> 2) & 0x7));
	col[1][0] = (uint8_t) (((c1 >> 8) & 0xF8) | ((c1 >> 13) & 0x7));
	col[1][1] = (uint8_t) (((c1 >> 3) & 0xFC) | ((c1 >> 9) & 0x3));
	col[1][2] = (uint8_t) (((c1 << 3) & 0xF8) | ((c1 >> 2) & 0x7));
	for (int k = 0; k < 3; k++)
	{
		col[2][k] = (uint8_t) ((col[0][k] * 2 + col[1][k]) / 3);
		col[3][k] = (uint8_t) ((col[0][k] + col[1][k] * 2) / 3);
	}
	uint32_t cbits = b[12] | (b[13] << 8) | (b[14] << 16) | ((uint32_t) b[15] << 24);

	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++)
		{
			int p = j * 4 + i;
			uint8_t* o = dst + (size_t) j * dst_stride + (size_t) i * 4;
			int cc = (cbits >> (2 * p)) & 3;
			o[0] = col[cc][0];
			o[1] = col[cc][1];
			o[2] = col[cc][2];
			o[3] = a[(abits >> (3 * p)) & 7];
		}
}

#ifdef SWF_HAVE_LIBJXR

#include <lzma.h>
#include <JXRGlue.h>

// One decoder over an in-memory buffer. jxrlib's memory stream does not copy,
// so `data` must outlive the decoder.
static PKImageDecode* jxr_open(const uint8_t* data, size_t len)
{
	struct WMPStream* ws = NULL;
	PKImageDecode* dec = NULL;
	if (data == NULL || len == 0) return NULL;
	if (CreateWS_Memory(&ws, (void*) data, len) != WMP_errSuccess) return NULL;
	if (PKImageDecode_Create_WMP(&dec) != WMP_errSuccess)
	{
		ws->Close(&ws);
		return NULL;
	}
	if (dec->Initialize(dec, ws) != WMP_errSuccess)
	{
		dec->Release(&dec);
		ws->Close(&ws);
		return NULL;
	}
	// The decoder's Release closes the stream it was initialised with.
	dec->fStreamOwner = !0;
	return dec;
}

static void jxr_close(PKImageDecode* dec)
{
	if (dec != NULL) dec->Release(&dec);
}

static int jxr_lookup(const PKPixelFormatGUID* fmt, PKPixelInfo* pi)
{
	memset(pi, 0, sizeof(*pi));
	pi->pGUIDPixFmt = fmt;
	return PixelFormatLookup(pi, LOOKUP_FORWARD) == WMP_errSuccess;
}

// image::FromPrimitive<u16> for u8: round(c * 255 / 65535). jxrlib writes the
// samples in host (little-endian) order and the TIFF header says "II".
static uint8_t jxr_u16_to_u8(const uint8_t* s)
{
	uint32_t c = (uint32_t) s[0] | ((uint32_t) s[1] << 8);
	return (uint8_t) ((c + 128) / 257);
}

// convert_to_tiff + to_rgba8. Returns a malloc()ed w*h*4 RGBA8 buffer.
static uint8_t* jxr_decode_tiff_rgba(PKImageDecode* dec, int w, int h)
{
	PKPixelFormatGUID src_fmt;
	if (dec->GetPixelFormat(dec, &src_fmt) != WMP_errSuccess) return NULL;

	dec->WMP.wmiSCP.uAlphaMode = 2;
	PKPixelInfo info;
	if (!jxr_lookup(&src_fmt, &info)) return NULL;
	if (PixelFormatLookup(&info, LOOKUP_BACKWARD_TIF) != WMP_errSuccess)
		return NULL;
	PKPixelFormatGUID out_fmt = *info.pGUIDPixFmt;

	// The unsigned-integer Gray / RGB / RGBA layouts at 8 or 16 bits per
	// sample (ImageMagick's `-define jxr` writes 64bppRGBA).
	PKPixelInfo pfrom, pto;
	if (!jxr_lookup(&src_fmt, &pfrom) || !jxr_lookup(&out_fmt, &pto))
		return NULL;
	int wide;
	if (pto.bdBitDepth == BD_8 && pto.uBitsPerSample == 8) wide = 0;
	else if (pto.bdBitDepth == BD_16 && pto.uBitsPerSample == 16) wide = 1;
	else return NULL;
	uint32_t spp = pto.uSamplePerPixel;
	uint32_t bpp = (pto.cbitUnit + 7) >> 3;
	if ((spp != 1 && spp != 3 && spp != 4) || bpp != spp << wide) return NULL;

	PKCodecFactory* factory = NULL;
	PKFormatConverter* conv = NULL;
	uint8_t* rows = NULL;
	uint8_t* rgba = NULL;
	if (PKCreateCodecFactory(&factory, WMP_SDK_VERSION) != WMP_errSuccess)
		return NULL;
	if (factory->CreateFormatConverter(&conv) != WMP_errSuccess) goto done;
	if (conv->Initialize(conv, dec, (char*) ".tiff", out_fmt) != WMP_errSuccess)
		goto done;

	// PKImageEncode_Transcode's common stride: the wider of source and target.
	{
		uint32_t stride_from = (pfrom.bdBitDepth == BD_1)
			? ((pfrom.cbitUnit * (uint32_t) w + 7) >> 3)
			: (((pfrom.cbitUnit + 7) >> 3) * (uint32_t) w);
		uint32_t stride_to = bpp * (uint32_t) w;
		uint32_t stride = stride_from > stride_to ? stride_from : stride_to;
		rows = (uint8_t*) calloc((size_t) stride * (size_t) h + 128, 1);
		if (rows == NULL) goto done;
		PKRect rect = { 0, 0, w, h };
		if (conv->Copy(conv, &rect, rows, stride) != WMP_errSuccess) goto done;

		rgba = (uint8_t*) malloc((size_t) w * (size_t) h * 4);
		if (rgba == NULL) goto done;
		for (int y = 0; y < h; y++)
		{
			const uint8_t* s = rows + (size_t) y * stride;
			uint8_t* d = rgba + (size_t) y * w * 4;
			for (int x = 0; x < w; x++, s += bpp, d += 4)
			{
				uint8_t v[4];
				for (uint32_t k = 0; k < spp; k++)
					v[k] = wide ? jxr_u16_to_u8(s + 2 * k) : s[k];
				if (spp == 1)
				{
					d[0] = d[1] = d[2] = v[0];
					d[3] = 255;
				}
				else
				{
					d[0] = v[0];
					d[1] = v[1];
					d[2] = v[2];
					d[3] = (spp == 4) ? v[3] : 255;
				}
			}
		}
	}

done:
	free(rows);
	if (conv != NULL) conv->Release(&conv);
	if (factory != NULL) factory->Release(&factory);
	return rgba;
}

int avm2_jxr_decode_rgba(const uint8_t* data, size_t len,
                         uint32_t* out_w, uint32_t* out_h, uint8_t** out_rgba)
{
	*out_rgba = NULL;
	PKImageDecode* dec = jxr_open(data, len);
	if (dec == NULL) return 0;
	I32 w = 0, h = 0;
	uint8_t* rgba = NULL;
	if (dec->GetSize(dec, &w, &h) == WMP_errSuccess && w > 0 && h > 0
	    && (uint64_t) w * (uint64_t) h <= 0x0FFFFFFFull)
		rgba = jxr_decode_tiff_rgba(dec, w, h);
	jxr_close(dec);
	if (rgba == NULL) return 0;
	*out_w = (uint32_t) w;
	*out_h = (uint32_t) h;
	*out_rgba = rgba;
	return 1;
}

// --- ATF ---------------------------------------------------------------------

// jpegxr_to_tiff with Ruffle's size assertion (a mismatch is a decode failure
// here rather than a panic).
static uint8_t* atf_jxr_rgba(const uint8_t* d, size_t n, uint32_t w, uint32_t h)
{
	uint32_t jw = 0, jh = 0;
	uint8_t* rgba = NULL;
	if (!avm2_jxr_decode_rgba(d, n, &jw, &jh, &rgba)) return NULL;
	if (jw != w || jh != h)
	{
		free(rgba);
		return NULL;
	}
	return rgba;
}

// jpegxr_to_raw_pixels: the source pixel format, unconverted. `want` is the
// exact byte count the caller indexes (2 bytes per pixel for the BGR565 plane).
static uint8_t* atf_jxr_raw(const uint8_t* d, size_t n, uint32_t w, uint32_t h,
                            size_t want)
{
	PKImageDecode* dec = jxr_open(d, n);
	if (dec == NULL) return NULL;
	uint8_t* out = NULL;
	I32 jw = 0, jh = 0;
	PKPixelFormatGUID fmt;
	PKPixelInfo pi;
	if (dec->GetSize(dec, &jw, &jh) != WMP_errSuccess
	    || (uint32_t) jw != w || (uint32_t) jh != h
	    || dec->GetPixelFormat(dec, &fmt) != WMP_errSuccess
	    || !jxr_lookup(&fmt, &pi))
		goto done;
	{
		// PixelInfo::bits_per_pixel is cbitUnit; stride = width * bpp / 8.
		size_t stride = (size_t) w * pi.cbitUnit / 8;
		size_t size = stride * h;
		if (size < want) goto done;
		out = (uint8_t*) calloc(size + 128, 1);
		if (out == NULL) goto done;
		PKRect rect = { 0, 0, jw, jh };
		if (dec->Copy(dec, &rect, out, (U32) stride) != WMP_errSuccess)
		{
			free(out);
			out = NULL;
		}
	}
done:
	jxr_close(dec);
	return out;
}

// ATF's LZMA records are an "alone" stream with the 8-byte size field missing
// (adobe/dds2atf#5). Ruffle splices in u64::MAX (unknown size); we know the
// exact size of a DXT table, so stop once it is full.
static uint8_t* atf_lzma(const uint8_t* d, size_t n, size_t want)
{
	if (n < 5 || want == 0) return NULL;
	uint8_t* in = (uint8_t*) malloc(n + 8);
	uint8_t* out = (uint8_t*) malloc(want);
	if (in == NULL || out == NULL)
	{
		free(in);
		free(out);
		return NULL;
	}
	memcpy(in, d, 5);
	memset(in + 5, 0xFF, 8);
	memcpy(in + 13, d + 5, n - 5);

	lzma_stream strm = LZMA_STREAM_INIT;
	int ok = 0;
	if (lzma_alone_decoder(&strm, UINT64_MAX) == LZMA_OK)
	{
		strm.next_in = in;
		strm.avail_in = n + 8;
		strm.next_out = out;
		strm.avail_out = want;
		lzma_ret r = lzma_code(&strm, LZMA_RUN);
		if (r == LZMA_OK || r == LZMA_STREAM_END)
		{
			if (strm.avail_out != 0 && r == LZMA_OK)
				r = lzma_code(&strm, LZMA_FINISH);
			ok = (strm.avail_out == 0);
		}
	}
	lzma_end(&strm);
	free(in);
	if (!ok)
	{
		free(out);
		return NULL;
	}
	return out;
}

// ATFFormat::CompressedAlpha (4): rebuild the DXT5 stream exactly as
// atf_jpegxr.rs:49-144 does, then decode it.
static uint8_t* atf_compressed_alpha(const uint8_t* dxt1a_z, size_t dxt1a_n,
                                     const uint8_t* jxr_a, size_t jxr_a_n,
                                     const uint8_t* dxt5c_z, size_t dxt5c_n,
                                     const uint8_t* jxr_bgr, size_t jxr_bgr_n,
                                     uint32_t w, uint32_t h)
{
	if (w < 4 || h < 4) return NULL;
	size_t bw = w / 4, bh = h / 4;
	size_t blocks = bw * bh;
	uint8_t* alpha_tab = atf_lzma(dxt1a_z, dxt1a_n, blocks * 6);
	uint8_t* color_tab = atf_lzma(dxt5c_z, dxt5c_n, blocks * 4);
	// The endpoint planes are (w/4) x (h/2): the first endpoint of every block
	// in the upper half, the second in the lower half (dds2atf
	// pvr2atfcore.cpp:884).
	uint8_t* alpha = atf_jxr_rgba(jxr_a, jxr_a_n, w / 4, h / 2);
	uint8_t* bgr = atf_jxr_raw(jxr_bgr, jxr_bgr_n, w / 4, h / 2, blocks * 4);
	uint8_t* rgba = NULL;
	uint8_t blk[16];
	if (alpha_tab == NULL || color_tab == NULL || alpha == NULL || bgr == NULL)
		goto done;
	rgba = (uint8_t*) malloc((size_t) w * h * 4);
	if (rgba == NULL) goto done;
	for (size_t i = 0; i < blocks; i++)
	{
		// Ruffle reads the second alpha endpoint at BYTE offset
		// second_half + 4i of the RGBA-expanded plane (atf_jpegxr.rs:107) —
		// pixel second_half/4 + i, not second_half + i. The BGR plane below
		// is indexed in pixels. We follow Ruffle byte for byte.
		blk[0] = alpha[i * 4];
		blk[1] = alpha[blocks + i * 4];
		memcpy(blk + 2, alpha_tab + i * 6, 6);
		memcpy(blk + 8, bgr + i * 2, 2);
		memcpy(blk + 10, bgr + blocks * 2 + i * 2, 2);
		memcpy(blk + 12, color_tab + i * 4, 4);
		size_t bx = i % bw, by = i / bw;
		avm2_bc3_decode_block(blk, rgba + (by * 4 * w + bx * 4) * 4, w * 4);
	}
done:
	free(alpha_tab);
	free(color_tab);
	free(alpha);
	free(bgr);
	return rgba;
}

// render/src/atf.rs ATFTexture::from_bytes, reading face 0 / mip 0 only.
int avm2_atf_jxr_decode(const uint8_t* b, size_t len, uint32_t w, uint32_t h,
                        uint8_t** out_rgba)
{
	*out_rgba = NULL;
	if (b == NULL || len < 3 || memcmp(b, "ATF", 3) != 0) return 0;
	size_t p = 3;
	int version = 0;
	if (len - p < 4) return 0;
	if (b[p + 3] == 0xFF)
	{
		if (len - p < 9) return 0;
		version = b[p + 4];
		p += 9;
	}
	else
	{
		p += 3;
	}
	if (len - p < 4) return 0;
	uint8_t format = b[p] & 0x7F;
	if ((1u << b[p + 1]) != w || (1u << b[p + 2]) != h) return 0;
	p += 4;   // tdata, log2 width, log2 height, mip count

	// (u24 | u32be length, payload) records.
	const uint8_t* rec[4];
	size_t rec_n[4];
	int want = (format == 0 || format == 1) ? 1 : (format == 4) ? 4 : 0;
	if (want == 0) return 0;
	for (int r = 0; r < want; r++)
	{
		size_t n;
		if (version == 0)
		{
			if (len - p < 3) return 0;
			n = ((size_t) b[p] << 16) | ((size_t) b[p + 1] << 8) | b[p + 2];
			p += 3;
		}
		else
		{
			if (len - p < 4) return 0;
			n = ((size_t) b[p] << 24) | ((size_t) b[p + 1] << 16)
			  | ((size_t) b[p + 2] << 8) | b[p + 3];
			p += 4;
		}
		if (len - p < n) return 0;
		rec[r] = b + p;
		rec_n[r] = n;
		p += n;
	}

	uint8_t* rgba = (want == 1)
		? atf_jxr_rgba(rec[0], rec_n[0], w, h)
		: atf_compressed_alpha(rec[0], rec_n[0], rec[1], rec_n[1],
		                       rec[2], rec_n[2], rec[3], rec_n[3], w, h);
	if (rgba == NULL) return 0;
	*out_rgba = rgba;
	return 1;
}

#else // !SWF_HAVE_LIBJXR

int avm2_jxr_decode_rgba(const uint8_t* data, size_t len,
                         uint32_t* out_w, uint32_t* out_h, uint8_t** out_rgba)
{
	(void) data; (void) len; (void) out_w; (void) out_h;
	*out_rgba = NULL;
	return 0;
}

int avm2_atf_jxr_decode(const uint8_t* atf, size_t len, uint32_t w, uint32_t h,
                        uint8_t** out_rgba)
{
	(void) atf; (void) len; (void) w; (void) h;
	*out_rgba = NULL;
	return 0;
}

#endif
