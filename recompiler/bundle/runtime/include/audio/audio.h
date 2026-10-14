#pragma once

#include <common.h>

// Forward declaration
typedef struct SWFAppContext SWFAppContext;

// Sound format codes (from SWF spec)
#define SOUND_FORMAT_UNCOMPRESSED_NE  0  // Native-endian
#define SOUND_FORMAT_ADPCM            1
#define SOUND_FORMAT_MP3              2
#define SOUND_FORMAT_UNCOMPRESSED_LE  3
#define SOUND_FORMAT_NELLYMOSER_16K   4
#define SOUND_FORMAT_NELLYMOSER_8K    5
#define SOUND_FORMAT_NELLYMOSER       6
#define SOUND_FORMAT_SPEEX           11

// Sound rate codes
#define SOUND_RATE_5512   0
#define SOUND_RATE_11025  1
#define SOUND_RATE_22050  2
#define SOUND_RATE_44100  3

// Maximum concurrent sounds
#define MAX_SOUND_CHANNELS 32

#ifndef NO_GRAPHICS

// A defined sound asset (from DefineSound tag)
typedef struct SoundAsset {
	u16 sound_id;
	u8 format;           // SOUND_FORMAT_*
	u8 rate;             // SOUND_RATE_*
	u8 sample_size;      // 0 = 8-bit, 1 = 16-bit
	u8 stereo;           // 0 = mono, 1 = stereo
	u32 sample_count;
	const u8* data;      // Compressed sound data
	size_t data_size;
} SoundAsset;

// A currently-playing sound instance
typedef struct SoundChannel {
	int active;
	const SoundAsset* asset;
	float* pcm_data;      // Decoded PCM (float, interleaved)
	size_t pcm_samples;   // Total decoded samples (per channel)
	double pcm_position;  // Current playback position (fractional, resampled)
	int channels;         // 1=mono, 2=stereo
	int sample_rate;      // Actual sample rate in Hz
	u32 loop_count;       // 0 = no loop, >0 = remaining loops
	double loop_start;    // Sample index loops restart at (0 for AVM1 sounds)
	// Per-channel SoundTransform gains (AVM2 SoundChannel.soundTransform):
	// left_out  = vol * (l_in * l2l + r_in * r2l)
	// right_out = vol * (l_in * l2r + r_in * r2r)
	// AVM1 starts leave these at identity (l2l=r2r=vol=1, l2r=r2l=0), which
	// mixes byte-identically to the pre-transform code path.
	float gain_l2l, gain_l2r, gain_r2l, gain_r2r, gain_vol;
	// Bumped every time a sound starts on this slot; (index, generation)
	// pairs make AVM2 SoundChannel handles safe against slot reuse.
	u32 generation;
} SoundChannel;

// Streaming sound state
typedef struct StreamingSound {
	int active;
	u8 format;
	u8 rate;
	u8 sample_size;
	u8 stereo;
	float* pcm_buffer;    // Ring buffer of decoded streaming PCM
	size_t buffer_size;   // Total buffer capacity (samples)
	size_t write_pos;     // Next write position
	size_t read_pos;      // Next read position (integer source-rate sample index)
	double read_pos_frac; // Fractional remainder for resampling
	int sample_rate;
	int channels;
	size_t prebuffer;     // Samples to buffer before starting playback
	int started;          // 1 once prebuffer threshold reached
} StreamingSound;

// Audio context
typedef struct AudioContext {
	SoundAsset* assets;       // Array of defined sounds
	size_t asset_count;
	size_t asset_capacity;

	SoundChannel channels[MAX_SOUND_CHANNELS];
	StreamingSound stream;

	int initialized;
	float master_volume;
} AudioContext;

// Initialize audio subsystem
void audio_init(SWFAppContext* app_context);

// Define a sound asset
void audio_define_sound(SWFAppContext* app_context, u16 sound_id,
	u8 format, u8 rate, u8 sample_size, u8 stereo,
	u32 sample_count, const u8* data, size_t data_size);

// Start playing a sound
void audio_start_sound(SWFAppContext* app_context, u16 sound_id,
	int stop, u32 loop_count, u32 in_point, u32 out_point);

// Stop all sounds
void audio_stop_all_sounds(SWFAppContext* app_context);

// ---------------------------------------------------------------------------
// AVM2 SoundChannel-shaped API: start returns a (channel index, generation)
// handle so a specific playback instance can be stopped/adjusted/queried even
// after the slot is reused. gains[5] = {l2l, l2r, r2l, r2r, volume} (see
// SoundChannel). Loops restart at start_ms (AS3 play() semantics), unlike the
// AVM1 path which restarts at 0.
// ---------------------------------------------------------------------------

// Start a sound; returns channel index (>= 0) and writes its generation to
// *out_generation, or returns -1 (no asset / no free channel / decode failed).
int audio_start_sound_ex(SWFAppContext* app_context, u16 sound_id,
	u32 loop_count, double start_ms, const float gains[5],
	u32* out_generation);

// The handle-checked channel operations are no-ops / return defaults when the
// handle is stale (slot reused or drained).
int audio_channel_active(SWFAppContext* app_context, int ch, u32 generation);
void audio_channel_stop(SWFAppContext* app_context, int ch, u32 generation);
void audio_channel_set_gains(SWFAppContext* app_context, int ch,
	u32 generation, const float gains[5]);
double audio_channel_position_ms(SWFAppContext* app_context, int ch,
	u32 generation);

// Global output volume (AVM2 SoundMixer.soundTransform.volume; 1.0 default).
void audio_set_master_volume(SWFAppContext* app_context, float volume);

// Setup streaming sound header
void audio_stream_head(SWFAppContext* app_context,
	u8 format, u8 rate, u8 sample_size, u8 stereo,
	u16 avg_sample_count);

// Feed a streaming sound block
void audio_stream_block(SWFAppContext* app_context,
	const u8* data, size_t data_size);

// Mix audio for output callback (called by SDL)
void audio_mix(AudioContext* ctx, float* output, size_t frames, int out_channels, int out_rate);

// Shutdown audio
void audio_shutdown(SWFAppContext* app_context);

// Audio output (platform-specific)
void audio_output_init(SWFAppContext* app_context);
void audio_output_shutdown(void);

#endif
