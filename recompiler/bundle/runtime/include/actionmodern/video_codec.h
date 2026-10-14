#ifndef SWFMODERN_VIDEO_CODEC_H
#define SWFMODERN_VIDEO_CODEC_H

#ifdef __cplusplus
extern "C" {
#endif

// FLV / DefineVideoStream codec ids (Adobe SWF spec).
//   2 = Sorenson Spark (H.263 variant)
//   3 = ScreenVideo
//   4 = VP6 (no alpha)
//   5 = VP6 alpha
//   6 = ScreenVideo 2
//   7 = H.264 / AVC

// Returns 1 if this build has a working decoder for the given codec id.
int video_codec_supported(int codec_id);

// DefineVideoStream `VideoFlagsDeblocking` (SWF spec UB[3], and the swf
// crate's `VideoDeblocking`). Only codec 2 (Sorenson Spark) has a deblocking
// filter; every other codec ignores the setting.
//
// Ruffle applies its H.263 Annex J post-filter for LEVEL1, or for
// USE_PACKET_VALUE when the picture header's own deblocker bit is set
// (video/software/src/decoder/h263.rs:80-90). LEVEL2..4 are parsed but never
// act — that asymmetry is Ruffle's, and we reproduce it.
#define VIDEO_DEBLOCK_USE_PACKET_VALUE 0
#define VIDEO_DEBLOCK_NONE             1
#define VIDEO_DEBLOCK_LEVEL1           2
#define VIDEO_DEBLOCK_LEVEL2           3
#define VIDEO_DEBLOCK_LEVEL3           4
#define VIDEO_DEBLOCK_LEVEL4           5

// Decode a single FLV/DefineVideoStream video payload.
//
// `payload` is the codec payload *after* the FLV tag's leading
// frame_type/codec_id byte (caller is responsible for stripping that
// byte and supplying frame_type/codec_id separately).
//
// `frame_type`: 1=keyframe, 2=inter, 3=disposable inter. Codecs without
// inter-frame support ignore it.
//
// On success returns 1 and fills *out_w, *out_h, *out_rgba. Caller owns
// *out_rgba and must release it via free(). Layout is packed RGBA8
// (R=byte0, G=byte1, B=byte2, A=byte3), top-to-bottom.
//
// `deblocking`: one of the VIDEO_DEBLOCK_* values above. NetStream/FLV
// callers pass VIDEO_DEBLOCK_USE_PACKET_VALUE (Ruffle core/src/streams.rs
// hardcodes that for every FLV stream); embedded DefineVideoStream callers
// pass the tag's own flags field.
//
// Returns 0 on any failure (unsupported codec, decode error, OOM).
int video_decode_one_frame(int codec_id, int frame_type,
                           const unsigned char* payload, int payload_len,
                           int deblocking,
                           int* out_w, int* out_h,
                           unsigned char** out_rgba);

// Persistent per-stream decoder. Inter-frame codecs (Sorenson Spark, VP6,
// H.264) need the prior keyframe's reference state when decoding subsequent
// inter-frames; the one-shot `video_decode_one_frame` allocates a fresh
// AVCodecContext per call and so cannot decode anything past the keyframe.
// SWF embedded video (DefineVideoStream + VideoFrame tags) needs this path.
typedef struct VideoDecoderCtx VideoDecoderCtx;

// Create a persistent decoder for the given codec id, or NULL if the codec
// is unsupported in this build. `deblocking` is one of VIDEO_DEBLOCK_*.
VideoDecoderCtx* video_decoder_create(int codec_id, int deblocking);

// Decode one VideoFrame payload through the persistent context. Same arg
// semantics as `video_decode_one_frame`. Returns 1 on success, 0 otherwise.
int video_decoder_decode(VideoDecoderCtx* ctx, int frame_type,
                         const unsigned char* payload, int payload_len,
                         int* out_w, int* out_h,
                         unsigned char** out_rgba);

// Release a persistent decoder. Safe to pass NULL.
void video_decoder_destroy(VideoDecoderCtx* ctx);

// Resample a packed RGBA8 buffer (R=byte0, G=byte1, B=byte2, A=byte3) to new
// dimensions. Uses libswscale when SWF_HAVE_LIBAVCODEC is defined (bilinear);
// otherwise falls back to nearest-neighbour. Returns 1 on success and fills
// *out_rgba (caller frees via free()). Returns 0 on OOM / bad inputs.
int video_resample_rgba(const unsigned char* src_rgba, int src_w, int src_h,
                        unsigned char** out_rgba, int dst_w, int dst_h);

#ifdef __cplusplus
}
#endif

#endif
