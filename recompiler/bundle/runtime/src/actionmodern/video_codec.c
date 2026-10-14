#include "actionmodern/video_codec.h"

#include <stdlib.h>
#include <string.h>

#ifdef SWF_HAVE_LIBAVCODEC
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#endif

// Slack bytes appended to every sws_scale destination plane.
//
// libswscale converts in SIMD-sized chunks and can write past the end of the
// last output row. Measured, not assumed: valgrind on visual/video_deblocking
// reports two `Invalid write of size 8` at +0 and +16 past a 246,016-byte
// (w*h*4) malloc'd block, from sws_scale. glibc detects the resulting arena
// damage at the NEXT free — `free(): corrupted unsorted chunks` inside
// av_frame_free — which is why the abort's backtrace pointed at cleanup code
// and not at the writer. ASan does not see it either: the write comes from
// libswscale.so, which is uninstrumented.
//
// FFmpeg's own av_image_alloc pads for exactly this reason. We keep plain
// malloc (callers free() the buffer) and add the same slack; 64 is
// AV_INPUT_BUFFER_PADDING_SIZE, FFmpeg's standard margin.
//
// Found by the CI crash-capture instrument, run 30309564809 shard 30
// (SWFRecompDocs/guides/ci-crash-capture.md).
#define SWS_DST_SLACK 64

int video_codec_supported(int codec_id)
{
	switch (codec_id) {
		case 3: // ScreenVideo — hand-rolled decoder in action.c
			return 1;
#ifdef SWF_HAVE_LIBAVCODEC
		case 2: // Sorenson Spark
			return avcodec_find_decoder(AV_CODEC_ID_FLV1) != NULL;
		case 4: // On2 VP6
			return avcodec_find_decoder(AV_CODEC_ID_VP6F) != NULL;
		case 5: // On2 VP6 with alpha
			return avcodec_find_decoder(AV_CODEC_ID_VP6A) != NULL;
#endif
		default:
			return 0;
	}
}

#ifdef SWF_HAVE_LIBAVCODEC

// ---------------------------------------------------------------------------
// BT.601 YUV 4:2:0 → RGBA, byte-for-byte Ruffle's h263-rs-yuv `bt601`
// (yuv/src/bt601.rs) as used by ruffle_render's Bitmap::to_rgba for BOTH the
// Spark and VP6 decoders.
//
// This deliberately replaces libswscale. sws_scale interpolates chroma and
// rounds differently, which put ~48% of the channels of every embedded-video
// frame 1-3 levels off the Ruffle golden — enough to fail at tolerance 0 while
// looking visually identical. libavcodec's VP6F/VP6A decode is bit-identical
// to Ruffle's nihav VP6 decode, so with this conversion the VP6 tests come out
// byte-exact.
//
// The two properties that matter and that swscale does NOT have:
//   * chroma is NEAREST-NEIGHBOUR — one Cb/Cr sample is reused verbatim for
//     all four corresponding luma pixels. bt601.rs: "This is not the most
//     correct, or nicest, but it's what Flash Player does."
//   * 16.16 fixed point with a +32768 round and the reduced→full range
//     expansion folded into the coefficients.
//
// Alpha (VP6A / AV_PIX_FMT_YUVA420P) comes from plane 3 at full resolution,
// and RGB is clamped to it (bitmap.rs: "RGB components need to be clamped to
// alpha to avoid invalid premultiplied colors") — the renderer's bitmap quad
// path expects premultiplied ARGB.
// ---------------------------------------------------------------------------
static int yuv420_frame_is_supported(int pix_fmt)
{
	return pix_fmt == AV_PIX_FMT_YUV420P || pix_fmt == AV_PIX_FMT_YUVA420P;
}

static void yuv420_planes_to_rgba_bt601(const uint8_t* yp,  int yls,
                                        const uint8_t* cbp, int cbls,
                                        const uint8_t* crp, int crls,
                                        const uint8_t* ap,  int als,
                                        int w, int h, unsigned char* rgba)
{
	for (int y = 0; y < h; y++) {
		const uint8_t* yrow  = yp  + (size_t)y * (size_t)yls;
		const uint8_t* cbrow = cbp + (size_t)(y >> 1) * (size_t)cbls;
		const uint8_t* crrow = crp + (size_t)(y >> 1) * (size_t)crls;
		const uint8_t* arow  = ap ? ap + (size_t)y * (size_t)als : NULL;
		unsigned char* out   = rgba + (size_t)y * (size_t)w * 4;

		for (int x = 0; x < w; x++) {
			// 76309   == round((255/219) * 65536)
			// 104597  == round((255/224) * 1.402 * 65536)
			// -53279  == round(-(255/224) * 1.402 * (0.299/0.587) * 65536)
			// -25675  == round(-(255/224) * 1.772 * (0.114/0.587) * 65536)
			// 132201  == round((255/224) * 1.772 * 65536)
			int32_t gray = ((int32_t)yrow[x] - 16) * 76309;
			int32_t cb   = (int32_t)cbrow[x >> 1] - 128;   // nearest chroma
			int32_t cr   = (int32_t)crrow[x >> 1] - 128;

			int32_t r = gray + cr * 104597 + 32768;
			int32_t g = gray + cr * -53279 + cb * -25675 + 32768;
			int32_t b = gray + cb * 132201 + 32768;

			// clamp(v >> 16, 0, 255). Shifting only non-negative values keeps
			// this free of C's implementation-defined `>>` on negatives; a
			// negative numerator can only ever clamp to 0 anyway.
			r = (r < 0) ? 0 : (r >> 16); if (r > 255) r = 255;
			g = (g < 0) ? 0 : (g >> 16); if (g > 255) g = 255;
			b = (b < 0) ? 0 : (b >> 16); if (b > 255) b = 255;

			int32_t a = 255;
			if (arow) {
				a = (int32_t)arow[x];
				if (r > a) r = a;
				if (g > a) g = a;
				if (b > a) b = a;
			}

			out[x * 4 + 0] = (unsigned char)r;
			out[x * 4 + 1] = (unsigned char)g;
			out[x * 4 + 2] = (unsigned char)b;
			out[x * 4 + 3] = (unsigned char)a;
		}
	}
}

static void yuv420_frame_to_rgba_bt601(const AVFrame* frame, int w, int h,
                                       unsigned char* rgba)
{
	const uint8_t* ap = (frame->format == AV_PIX_FMT_YUVA420P) ? frame->data[3] : NULL;
	yuv420_planes_to_rgba_bt601(frame->data[0], frame->linesize[0],
	                            frame->data[1], frame->linesize[1],
	                            frame->data[2], frame->linesize[2],
	                            ap, ap ? frame->linesize[3] : 0,
	                            w, h, rgba);
}

// ---------------------------------------------------------------------------
// H.263 Annex J post-decode deblocking filter.
//
// Line-for-line port of Ruffle's `h263-rs-deblock`
// (deblock/src/deblock.rs), which `video/software/src/decoder/h263.rs`
// runs over the decoded Y/Cb/Cr planes BEFORE the BT.601 conversion whenever
// the stream asks for it. libavcodec's FLV1 decoder has no equivalent, so
// without this every deblocked Spark stream renders with visible 8x8 blocking
// against a Ruffle-exported golden that has none.
//
// Two properties of the Rust original that a "clean" C rewrite would get
// wrong, and that decide byte-exactness:
//
//  1. **The scalar and SIMD lanes are NOT the same function.** Rust's
//     `scalar_impl::process` divides (`/8`, `/4`, `/2`, truncating toward
//     zero); `simd_impl::process_simd` shifts (`.shr(3)`, `.shr(2)`,
//     `.shr(1)`, flooring). They disagree on every negative intermediate
//     (-3/2 == -1 but -3>>1 == -2). Ruffle's chunking decides which pixel
//     gets which: horizontally, columns [0, 8*floor(w/8)) shift and the
//     <=7-column remainder divides; vertically, rows [0, 8*floor(h/8))
//     shift and the <=7-row remainder divides. Reproduced exactly.
//  2. **A and D wrap, B and C clamp.** `(a16 - d2) as u8` is a truncating
//     cast in Rust; only B and C get `.clamp(0, 255)`.
//
// The port is validated against all three `test_deblock` vectors (11x17,
// strengths 4/8/12 — an image sized to exercise both lanes in both
// directions) and the 37-row `test_process` table from deblock.rs.
//
// Deblocking is display-only: Ruffle filters a COPY and leaves the decoder's
// reference picture untouched, so inter-frame prediction is unaffected. We do
// the same by filtering scratch planes, never `frame->data` (which libavcodec
// may still hold as a reference frame).
// ---------------------------------------------------------------------------

// Table J.2/H.263 — QUANT → filter STRENGTH. Index 0 is never used.
static const unsigned char VIDEO_QUANT_TO_STRENGTH[32] = {
	0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 7,
	7, 8, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11, 12, 12, 12
};

// Arithmetic (flooring) right shift, spelled without relying on C's
// implementation-defined `>>` for negative operands.
static int deblock_floor_shr(int x, int n)
{
	return (x < 0) ? -(((-x) + ((1 << n) - 1)) >> n) : (x >> n);
}

// Figure J.2/H.263 — parameter d1 as a function of d.
static int deblock_up_down_ramp(int x, int strength)
{
	int ax  = (x < 0) ? -x : x;
	int sgn = (x > 0) - (x < 0);
	int t   = 2 * (ax - strength);
	if (t < 0) t = 0;
	int v = ax - t;
	if (v < 0) v = 0;
	return sgn * v;
}

// Clips x to +/- abs(lim).
static int deblock_clipd1(int x, int lim)
{
	int l = (lim < 0) ? -lim : lim;
	if (x < -l) return -l;
	if (x >  l) return  l;
	return x;
}

// Rust `scalar_impl::process` — truncating division.
static void deblock_process_div(unsigned char* A, unsigned char* B,
                                unsigned char* C, unsigned char* D, int strength)
{
	int a = *A, b = *B, c = *C, d = *D;
	int dv = (a - 4 * b + 4 * c - d) / 8;
	int d1 = deblock_up_down_ramp(dv, strength);
	int d2 = deblock_clipd1((a - d) / 4, d1 / 2);
	int rb = b + d1; if (rb < 0) rb = 0; else if (rb > 255) rb = 255;
	int rc = c - d1; if (rc < 0) rc = 0; else if (rc > 255) rc = 255;
	*A = (unsigned char)(a - d2);
	*B = (unsigned char)rb;
	*C = (unsigned char)rc;
	*D = (unsigned char)(d + d2);
}

// Rust `simd_impl::process_simd` — flooring shift. See note 1 above.
static void deblock_process_shr(unsigned char* A, unsigned char* B,
                                unsigned char* C, unsigned char* D, int strength)
{
	int a = *A, b = *B, c = *C, d = *D;
	int dv = deblock_floor_shr(a - 4 * b + 4 * c - d, 3);
	int d1 = deblock_up_down_ramp(dv, strength);
	int d2 = deblock_clipd1(deblock_floor_shr(a - d, 2), deblock_floor_shr(d1, 1));
	int rb = b + d1; if (rb < 0) rb = 0; else if (rb > 255) rb = 255;
	int rc = c - d1; if (rc < 0) rc = 0; else if (rc > 255) rc = 255;
	*A = (unsigned char)(a - d2);
	*B = (unsigned char)rb;
	*C = (unsigned char)rc;
	*D = (unsigned char)(d + d2);
}

// Horizontal block edges: rows edge_y-2 .. edge_y+1 for every edge_y = 8, 16,
// ... while edge_y+2 <= height (Rust: `while edge_y <= height - 2`).
static void deblock_horiz(unsigned char* p, int width, int height, int stride, int strength)
{
	int simd_cols = (width / 8) * 8;
	for (int edge_y = 8; edge_y + 2 <= height; edge_y += 8) {
		unsigned char* ra = p + (size_t)(edge_y - 2) * (size_t)stride;
		unsigned char* rb = ra + stride;
		unsigned char* rc = rb + stride;
		unsigned char* rd = rc + stride;
		for (int x = 0; x < simd_cols; x++)
			deblock_process_shr(&ra[x], &rb[x], &rc[x], &rd[x], strength);
		for (int x = simd_cols; x < width; x++)
			deblock_process_div(&ra[x], &rb[x], &rc[x], &rd[x], strength);
	}
}

// Vertical block edges. Rust iterates `row[2..].chunks_exact_mut(8)` and
// touches indices 4..7 of each chunk, i.e. columns 8k+6 .. 8k+9 — the edge
// itself sits at column 8k+8. `width >= 10` is Rust's own guard for the
// `[2..]` slice.
static void deblock_vert(unsigned char* p, int width, int height, int stride, int strength)
{
	if (width < 10) return;
	int nchunks   = (width - 2) / 8;
	int simd_rows = (height / 8) * 8;
	for (int y = 0; y < height; y++) {
		unsigned char* row = p + (size_t)y * (size_t)stride;
		int use_shr = (y < simd_rows);
		for (int k = 0; k < nchunks; k++) {
			unsigned char* q = row + 2 + 8 * k;
			if (use_shr) deblock_process_shr(&q[4], &q[5], &q[6], &q[7], strength);
			else         deblock_process_div(&q[4], &q[5], &q[6], &q[7], strength);
		}
	}
}

// Rust `deblock()` — horizontal edges first, per the spec, then vertical.
static void deblock_plane(unsigned char* p, int width, int height, int stride, int strength)
{
	if (!p || width <= 0 || height <= 0 || strength <= 0) return;
	deblock_horiz(p, width, height, stride, strength);
	deblock_vert(p, width, height, stride, strength);
}

// ---------------------------------------------------------------------------
// Sorenson Spark picture header — just enough of it to recover the deblocker
// bit and PQUANT, which libavcodec does not expose on the AVFrame.
//
// Layout (h263-rs parser/picture.rs `decode_picture` +
// `decode_sorenson_ptype`, MSB-first): PSC[17] == 1, version[5],
// temporal_reference[8], source_format[3] (+ 8+8 or 16+16 custom dims when
// the code is 0 or 1), picture_type[2], USE_DEBLOCKER[1], PQUANT[5].
// ---------------------------------------------------------------------------
typedef struct { const unsigned char* d; int nbits; int pos; } SparkBits;

static int spark_read_bits(SparkBits* r, int n, unsigned* out)
{
	unsigned v = 0;
	if (r->pos + n > r->nbits) return 0;
	for (int i = 0; i < n; i++) {
		unsigned bit = (r->d[r->pos >> 3] >> (7 - (r->pos & 7))) & 1u;
		v = (v << 1) | bit;
		r->pos++;
	}
	*out = v;
	return 1;
}

static int spark_parse_header(const unsigned char* payload, int len,
                              int* out_use_deblocker, int* out_quantizer)
{
	SparkBits r = { payload, len * 8, 0 };
	unsigned v;
	if (!spark_read_bits(&r, 17, &v) || v != 1) return 0;   // picture start code
	if (!spark_read_bits(&r, 5, &v)) return 0;              // version
	if (!spark_read_bits(&r, 8, &v)) return 0;              // temporal reference
	if (!spark_read_bits(&r, 3, &v)) return 0;              // source format
	if (v == 0) {
		if (!spark_read_bits(&r, 8, &v)) return 0;
		if (!spark_read_bits(&r, 8, &v)) return 0;
	} else if (v == 1) {
		if (!spark_read_bits(&r, 16, &v)) return 0;
		if (!spark_read_bits(&r, 16, &v)) return 0;
	}
	if (!spark_read_bits(&r, 2, &v)) return 0;              // picture type
	if (!spark_read_bits(&r, 1, &v)) return 0;              // USE_DEBLOCKER
	*out_use_deblocker = (int)v;
	if (!spark_read_bits(&r, 5, &v)) return 0;              // PQUANT
	*out_quantizer = (int)v;
	return 1;
}

// Ruffle's gate, verbatim (h263.rs:80-90): filter for LEVEL1 unconditionally,
// or for USE_PACKET_VALUE when the picture header says so. Returns the
// Table J.2 strength, or 0 for "don't filter".
static int video_deblock_strength(int codec_id, int deblocking,
                                  const unsigned char* payload, int payload_len)
{
	if (codec_id != 2) return 0;
	if (deblocking != VIDEO_DEBLOCK_USE_PACKET_VALUE &&
	    deblocking != VIDEO_DEBLOCK_LEVEL1) return 0;

	int use_deblocker = 0, quantizer = 0;
	if (!spark_parse_header(payload, payload_len, &use_deblocker, &quantizer)) return 0;
	if (deblocking == VIDEO_DEBLOCK_USE_PACKET_VALUE && !use_deblocker) return 0;
	if (quantizer < 0 || quantizer > 31) return 0;
	return (int)VIDEO_QUANT_TO_STRENGTH[quantizer];
}

// Deblock a decoded 4:2:0 frame into scratch planes and convert those.
// Returns 1 if the deblocked path ran, 0 if the caller should fall back to
// converting `frame` directly (OOM only).
//
// The scratch planes are tightly packed at the picture's own width, which is
// exactly h263-rs's `DecodedPicture` layout (luma w*h, chroma
// ceil(w/2)*ceil(h/2)) — the geometry `deblock()` assumes.
static int yuv420_deblock_and_convert(const AVFrame* frame, int w, int h,
                                      int strength, unsigned char* rgba)
{
	int cw = (w + 1) / 2, ch = (h + 1) / 2;
	unsigned char* yb  = (unsigned char*)malloc((size_t)w * (size_t)h);
	unsigned char* cbb = (unsigned char*)malloc((size_t)cw * (size_t)ch);
	unsigned char* crb = (unsigned char*)malloc((size_t)cw * (size_t)ch);
	if (!yb || !cbb || !crb) { free(yb); free(cbb); free(crb); return 0; }

	for (int y = 0; y < h; y++)
		memcpy(yb + (size_t)y * w, frame->data[0] + (size_t)y * frame->linesize[0], (size_t)w);
	for (int y = 0; y < ch; y++) {
		memcpy(cbb + (size_t)y * cw, frame->data[1] + (size_t)y * frame->linesize[1], (size_t)cw);
		memcpy(crb + (size_t)y * cw, frame->data[2] + (size_t)y * frame->linesize[2], (size_t)cw);
	}

	deblock_plane(yb,  w,  h,  w,  strength);
	deblock_plane(cbb, cw, ch, cw, strength);
	deblock_plane(crb, cw, ch, cw, strength);

	// Codec 2 is never YUVA420P, so there is no alpha plane on this path.
	yuv420_planes_to_rgba_bt601(yb, w, cbb, cw, crb, cw, NULL, 0, w, h, rgba);

	free(yb); free(cbb); free(crb);
	return 1;
}

static enum AVCodecID map_flv_codec_id(int codec_id)
{
	switch (codec_id) {
		case 2: return AV_CODEC_ID_FLV1;
		case 4: return AV_CODEC_ID_VP6F;
		case 5: return AV_CODEC_ID_VP6A;
		case 7: return AV_CODEC_ID_H264;
		default: return AV_CODEC_ID_NONE;
	}
}

static int decode_via_libavcodec(int codec_id,
                                 const unsigned char* payload, int payload_len,
                                 int deblocking,
                                 int* out_w, int* out_h,
                                 unsigned char** out_rgba)
{
	enum AVCodecID avid = map_flv_codec_id(codec_id);
	if (avid == AV_CODEC_ID_NONE) return 0;

	const AVCodec* codec = avcodec_find_decoder(avid);
	if (!codec) return 0;

	AVCodecContext* cctx = avcodec_alloc_context3(codec);
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	struct SwsContext* sws = NULL;
	unsigned char* rgba = NULL;
	int ok = 0;

	if (!cctx || !pkt || !frame) goto done;
	if (avcodec_open2(cctx, codec, NULL) < 0) goto done;

	pkt->data = (uint8_t*)payload;
	pkt->size = payload_len;

	if (avcodec_send_packet(cctx, pkt) < 0) goto done;
	if (avcodec_receive_frame(cctx, frame) < 0) goto done;

	int w = frame->width;
	int h = frame->height;
	if (w <= 0 || h <= 0) goto done;

	rgba = (unsigned char*)malloc((size_t)w * (size_t)h * 4 + SWS_DST_SLACK);
	if (!rgba) goto done;

	if (yuv420_frame_is_supported(frame->format)) {
		// Exact Ruffle conversion — see yuv420_frame_to_rgba_bt601, with the
		// H.263 Annex J post-filter in front of it when the stream asks.
		int strength = video_deblock_strength(codec_id, deblocking, payload, payload_len);
		if (strength <= 0 || !yuv420_deblock_and_convert(frame, w, h, strength, rgba))
			yuv420_frame_to_rgba_bt601(frame, w, h, rgba);
	} else {
		// Any other decoded pixel format (e.g. an H.264 profile that isn't
		// 8-bit 4:2:0) still goes through libswscale.
		sws = sws_getContext(w, h, (enum AVPixelFormat)frame->format,
		                     w, h, AV_PIX_FMT_RGBA,
		                     SWS_BILINEAR, NULL, NULL, NULL);
		if (!sws) { free(rgba); rgba = NULL; goto done; }
		{
			const int* inv_table = sws_getCoefficients(SWS_CS_ITU601);
			const int* table     = sws_getCoefficients(SWS_CS_DEFAULT);
			sws_setColorspaceDetails(sws,
			                          inv_table, /*srcRange=*/0,   // limited / TV
			                          table,     /*dstRange=*/1,   // full
			                          /*brightness=*/0,
			                          /*contrast=*/1 << 16,
			                          /*saturation=*/1 << 16);
		}
		uint8_t* dst_data[4] = { rgba, NULL, NULL, NULL };
		int dst_linesize[4] = { w * 4, 0, 0, 0 };
		if (sws_scale(sws, (const uint8_t* const*)frame->data, frame->linesize,
		              0, h, dst_data, dst_linesize) <= 0) {
			free(rgba); rgba = NULL;
			goto done;
		}
	}

	*out_w = w;
	*out_h = h;
	*out_rgba = rgba;
	ok = 1;

done:
	if (sws) sws_freeContext(sws);
	if (frame) av_frame_free(&frame);
	if (pkt) av_packet_free(&pkt);
	if (cctx) avcodec_free_context(&cctx);
	return ok;
}

#endif // SWF_HAVE_LIBAVCODEC

int video_resample_rgba(const unsigned char* src_rgba, int src_w, int src_h,
                        unsigned char** out_rgba, int dst_w, int dst_h)
{
	if (!src_rgba || !out_rgba || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
		return 0;

	unsigned char* dst = (unsigned char*)malloc((size_t)dst_w * (size_t)dst_h * 4 + SWS_DST_SLACK);
	if (!dst) return 0;

#ifdef SWF_HAVE_LIBAVCODEC
	struct SwsContext* sws = sws_getContext(src_w, src_h, AV_PIX_FMT_RGBA,
	                                        dst_w, dst_h, AV_PIX_FMT_RGBA,
	                                        SWS_BILINEAR, NULL, NULL, NULL);
	if (sws) {
		const uint8_t* src_data[4] = { src_rgba, NULL, NULL, NULL };
		int src_linesize[4] = { src_w * 4, 0, 0, 0 };
		uint8_t* dst_data[4] = { dst, NULL, NULL, NULL };
		int dst_linesize[4] = { dst_w * 4, 0, 0, 0 };
		int rv = sws_scale(sws, src_data, src_linesize, 0, src_h,
		                   dst_data, dst_linesize);
		sws_freeContext(sws);
		if (rv > 0) { *out_rgba = dst; return 1; }
	}
	// libswscale failed — fall through to nearest neighbour.
#endif

	for (int y = 0; y < dst_h; y++) {
		int sy = (y * src_h) / dst_h;
		if (sy >= src_h) sy = src_h - 1;
		for (int x = 0; x < dst_w; x++) {
			int sx = (x * src_w) / dst_w;
			if (sx >= src_w) sx = src_w - 1;
			const unsigned char* sp = src_rgba + (sy * src_w + sx) * 4;
			unsigned char* dp = dst + (y * dst_w + x) * 4;
			dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
		}
	}
	*out_rgba = dst;
	return 1;
}

int video_decode_one_frame(int codec_id, int frame_type,
                           const unsigned char* payload, int payload_len,
                           int deblocking,
                           int* out_w, int* out_h,
                           unsigned char** out_rgba)
{
	(void)frame_type;
	if (!payload || payload_len <= 0 || !out_w || !out_h || !out_rgba) return 0;

#ifdef SWF_HAVE_LIBAVCODEC
	switch (codec_id) {
		case 2: // Sorenson Spark
		case 4: // VP6
		case 5: // VP6 alpha
		case 7: // H.264
			return decode_via_libavcodec(codec_id, payload, payload_len,
			                              deblocking, out_w, out_h, out_rgba);
		default:
			break;
	}
#endif

	// Codec 3 (ScreenVideo) is handled by the legacy decoder in action.c;
	// callers route it there directly.
	(void)codec_id; (void)deblocking;
	return 0;
}

// ---------------------------------------------------------------------------
// Persistent decoder
// ---------------------------------------------------------------------------
struct VideoDecoderCtx {
	int codec_id;
	int deblocking;   // VIDEO_DEBLOCK_* from the stream's DefineVideoStream
#ifdef SWF_HAVE_LIBAVCODEC
	AVCodecContext* cctx;
	struct SwsContext* sws;
	int sws_w, sws_h;
	int sws_pixfmt;
#else
	int unused;
#endif
};

VideoDecoderCtx* video_decoder_create(int codec_id, int deblocking)
{
	if (!video_codec_supported(codec_id)) return NULL;

	VideoDecoderCtx* ctx = (VideoDecoderCtx*)calloc(1, sizeof(VideoDecoderCtx));
	if (!ctx) return NULL;
	ctx->codec_id = codec_id;
	ctx->deblocking = deblocking;

#ifdef SWF_HAVE_LIBAVCODEC
	enum AVCodecID avid = map_flv_codec_id(codec_id);
	if (avid == AV_CODEC_ID_NONE) { free(ctx); return NULL; }

	const AVCodec* codec = avcodec_find_decoder(avid);
	if (!codec) { free(ctx); return NULL; }

	ctx->cctx = avcodec_alloc_context3(codec);
	if (!ctx->cctx) { free(ctx); return NULL; }

	if (avcodec_open2(ctx->cctx, codec, NULL) < 0) {
		avcodec_free_context(&ctx->cctx);
		free(ctx);
		return NULL;
	}
#endif
	return ctx;
}

void video_decoder_destroy(VideoDecoderCtx* ctx)
{
	if (!ctx) return;
#ifdef SWF_HAVE_LIBAVCODEC
	if (ctx->sws) sws_freeContext(ctx->sws);
	if (ctx->cctx) avcodec_free_context(&ctx->cctx);
#endif
	free(ctx);
}

int video_decoder_decode(VideoDecoderCtx* ctx, int frame_type,
                         const unsigned char* payload, int payload_len,
                         int* out_w, int* out_h,
                         unsigned char** out_rgba)
{
	(void)frame_type;
	if (!ctx || !payload || payload_len <= 0 || !out_w || !out_h || !out_rgba)
		return 0;

#ifdef SWF_HAVE_LIBAVCODEC
	if (!ctx->cctx) return 0;

	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	unsigned char* rgba = NULL;
	int ok = 0;

	if (!pkt || !frame) goto done;

	pkt->data = (uint8_t*)payload;
	pkt->size = payload_len;

	if (avcodec_send_packet(ctx->cctx, pkt) < 0) goto done;
	if (avcodec_receive_frame(ctx->cctx, frame) < 0) goto done;

	int w = frame->width;
	int h = frame->height;
	if (w <= 0 || h <= 0) goto done;

	rgba = (unsigned char*)malloc((size_t)w * (size_t)h * 4 + SWS_DST_SLACK);
	if (!rgba) goto done;

	if (yuv420_frame_is_supported(frame->format)) {
		// Exact Ruffle conversion — see yuv420_frame_to_rgba_bt601. This is
		// the path every embedded DefineVideoStream (Spark / VP6 / VP6A) and
		// every FLV NetStream frame takes. Spark streams may additionally ask
		// for the Annex J deblocking post-filter.
		int strength = video_deblock_strength(ctx->codec_id, ctx->deblocking,
		                                      payload, payload_len);
		if (strength <= 0 || !yuv420_deblock_and_convert(frame, w, h, strength, rgba))
			yuv420_frame_to_rgba_bt601(frame, w, h, rgba);
	} else {
		// (Re)build sws context if dimensions or pixel format changed.
		if (!ctx->sws || ctx->sws_w != w || ctx->sws_h != h ||
		    ctx->sws_pixfmt != frame->format) {
			if (ctx->sws) { sws_freeContext(ctx->sws); ctx->sws = NULL; }
			ctx->sws = sws_getContext(w, h, (enum AVPixelFormat)frame->format,
			                          w, h, AV_PIX_FMT_RGBA,
			                          SWS_BILINEAR, NULL, NULL, NULL);
			if (!ctx->sws) { free(rgba); rgba = NULL; goto done; }
			ctx->sws_w = w; ctx->sws_h = h; ctx->sws_pixfmt = frame->format;

			const int* inv_table = sws_getCoefficients(SWS_CS_ITU601);
			const int* table     = sws_getCoefficients(SWS_CS_DEFAULT);
			sws_setColorspaceDetails(ctx->sws,
			                          inv_table, /*srcRange=*/0,
			                          table,     /*dstRange=*/1,
			                          /*brightness=*/0,
			                          /*contrast=*/1 << 16,
			                          /*saturation=*/1 << 16);
		}

		uint8_t* dst_data[4] = { rgba, NULL, NULL, NULL };
		int dst_linesize[4] = { w * 4, 0, 0, 0 };
		if (sws_scale(ctx->sws, (const uint8_t* const*)frame->data, frame->linesize,
		              0, h, dst_data, dst_linesize) <= 0) {
			free(rgba); rgba = NULL;
			goto done;
		}
	}

	*out_w = w;
	*out_h = h;
	*out_rgba = rgba;
	ok = 1;

done:
	if (frame) av_frame_free(&frame);
	if (pkt) av_packet_free(&pkt);
	return ok;
#else
	(void)payload_len;
	return 0;
#endif
}
