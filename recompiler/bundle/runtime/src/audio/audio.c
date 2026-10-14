#ifndef NO_GRAPHICS

#include <audio/audio.h>
#include <swf.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include <audio/minimp3.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static int rate_code_to_hz(u8 rate)
{
	switch (rate)
	{
		case SOUND_RATE_5512:  return 5512;
		case SOUND_RATE_11025: return 11025;
		case SOUND_RATE_22050: return 22050;
		case SOUND_RATE_44100: return 44100;
		default: return 44100;
	}
}

static AudioContext* get_audio_ctx(SWFAppContext* app_context)
{
	return (AudioContext*)app_context->audio_ctx;
}

// ---------------------------------------------------------------------------
// Decode MP3 data into float PCM
// ---------------------------------------------------------------------------
static float* decode_mp3(const u8* data, size_t data_size,
	size_t* out_samples, int* out_channels, int* out_rate)
{
	mp3dec_t dec;
	mp3dec_init(&dec);

	// First pass: count total samples
	size_t total_samples = 0;
	size_t offset = 0;
	mp3dec_frame_info_t info;
	short pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];

	while (offset < data_size)
	{
		int samples = mp3dec_decode_frame(&dec, data + offset,
			(int)(data_size - offset), pcm_buf, &info);
		if (info.frame_bytes == 0) break;
		total_samples += samples;
		offset += info.frame_bytes;
	}

	if (total_samples == 0)
	{
		*out_samples = 0;
		*out_channels = 1;
		*out_rate = 44100;
		return NULL;
	}

	// Second pass: decode into float buffer
	mp3dec_init(&dec);
	int channels = info.channels;
	float* result = (float*)malloc(total_samples * channels * sizeof(float));
	size_t write_pos = 0;
	offset = 0;

	while (offset < data_size)
	{
		int samples = mp3dec_decode_frame(&dec, data + offset,
			(int)(data_size - offset), pcm_buf, &info);
		if (info.frame_bytes == 0) break;

		for (int i = 0; i < samples * channels; i++)
		{
			result[write_pos++] = pcm_buf[i] / 32768.0f;
		}
		offset += info.frame_bytes;
	}

	*out_samples = total_samples;
	*out_channels = channels;
	*out_rate = info.hz;
	return result;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void audio_init(SWFAppContext* app_context)
{
	AudioContext* ctx = (AudioContext*)calloc(1, sizeof(AudioContext));
	ctx->asset_capacity = 64;
	ctx->assets = (SoundAsset*)calloc(ctx->asset_capacity, sizeof(SoundAsset));
	ctx->asset_count = 0;
	ctx->master_volume = 1.0f;
	ctx->initialized = 1;
	memset(&ctx->stream, 0, sizeof(StreamingSound));
	app_context->audio_ctx = ctx;
}

void audio_define_sound(SWFAppContext* app_context, u16 sound_id,
	u8 format, u8 rate, u8 sample_size, u8 stereo,
	u32 sample_count, const u8* data, size_t data_size)
{
	AudioContext* ctx = get_audio_ctx(app_context);
	if (!ctx) return;

	// Grow array if needed
	if (ctx->asset_count >= ctx->asset_capacity)
	{
		ctx->asset_capacity *= 2;
		ctx->assets = (SoundAsset*)realloc(ctx->assets,
			ctx->asset_capacity * sizeof(SoundAsset));
	}

	SoundAsset* asset = &ctx->assets[ctx->asset_count++];
	asset->sound_id = sound_id;
	asset->format = format;
	asset->rate = rate;
	asset->sample_size = sample_size;
	asset->stereo = stereo;
	asset->sample_count = sample_count;
	asset->data = data;
	asset->data_size = data_size;
}

static const SoundAsset* find_asset(AudioContext* ctx, u16 sound_id)
{
	for (size_t i = 0; i < ctx->asset_count; i++)
	{
		if (ctx->assets[i].sound_id == sound_id)
			return &ctx->assets[i];
	}
	return NULL;
}

// Decode an asset and start it on a free channel with default state
// (position 0, loop restart at 0, identity gains). Returns the channel
// index, or -1 (no free channel / unsupported format / decode failed).
// Callers adjust position/loop_start/gains afterwards.
static int start_sound_core(AudioContext* ctx, const SoundAsset* asset,
	u32 loop_count)
{
	// Find free channel
	int ch = -1;
	for (int i = 0; i < MAX_SOUND_CHANNELS; i++)
	{
		if (!ctx->channels[i].active)
		{
			ch = i;
			break;
		}
	}
	if (ch < 0) return -1; // All channels busy

	// Decode
	float* pcm = NULL;
	size_t pcm_samples = 0;
	int channels = 1;
	int sample_rate = rate_code_to_hz(asset->rate);

	if (asset->format == SOUND_FORMAT_MP3)
	{
		// MP3 data in DefineSound has a 2-byte SeekSamples field before the MP3 frames
		if (asset->data_size < 2) return -1;
		const u8* mp3_data = asset->data + 2;
		size_t mp3_size = asset->data_size - 2;
		pcm = decode_mp3(mp3_data, mp3_size, &pcm_samples, &channels, &sample_rate);
	}
	else if (asset->format == SOUND_FORMAT_UNCOMPRESSED_NE ||
			 asset->format == SOUND_FORMAT_UNCOMPRESSED_LE)
	{
		channels = asset->stereo ? 2 : 1;
		if (asset->sample_size == 1) // 16-bit
		{
			pcm_samples = asset->data_size / (2 * channels);
			pcm = (float*)malloc(pcm_samples * channels * sizeof(float));
			const s16* src = (const s16*)asset->data;
			for (size_t i = 0; i < pcm_samples * (size_t)channels; i++)
				pcm[i] = src[i] / 32768.0f;
		}
		else // 8-bit
		{
			pcm_samples = asset->data_size / channels;
			pcm = (float*)malloc(pcm_samples * channels * sizeof(float));
			for (size_t i = 0; i < pcm_samples * (size_t)channels; i++)
				pcm[i] = (asset->data[i] - 128) / 128.0f;
		}
	}

	if (!pcm) return -1;

	SoundChannel* channel = &ctx->channels[ch];
	channel->active = 1;
	channel->asset = asset;
	channel->pcm_data = pcm;
	channel->pcm_samples = pcm_samples;
	channel->pcm_position = 0.0;
	channel->channels = channels;
	channel->sample_rate = sample_rate;
	channel->loop_count = loop_count;
	channel->loop_start = 0.0;
	channel->gain_l2l = 1.0f;
	channel->gain_l2r = 0.0f;
	channel->gain_r2l = 0.0f;
	channel->gain_r2r = 1.0f;
	channel->gain_vol = 1.0f;
	channel->generation++;
	return ch;
}

void audio_start_sound(SWFAppContext* app_context, u16 sound_id,
	int stop, u32 loop_count, u32 in_point, u32 out_point)
{
	AudioContext* ctx = get_audio_ctx(app_context);
	if (!ctx) return;

	// If stop flag, stop any playing instances of this sound
	if (stop)
	{
		for (int i = 0; i < MAX_SOUND_CHANNELS; i++)
		{
			if (ctx->channels[i].active && ctx->channels[i].asset &&
				ctx->channels[i].asset->sound_id == sound_id)
			{
				ctx->channels[i].active = 0;
				if (ctx->channels[i].pcm_data)
				{
					free(ctx->channels[i].pcm_data);
					ctx->channels[i].pcm_data = NULL;
				}
			}
		}
		return;
	}

	const SoundAsset* asset = find_asset(ctx, sound_id);
	if (!asset) return;

	int ch = start_sound_core(ctx, asset, loop_count);
	if (ch < 0) return;

	SoundChannel* channel = &ctx->channels[ch];
	channel->pcm_position =
		(in_point < channel->pcm_samples) ? (double)in_point : 0.0;
}

void audio_stop_all_sounds(SWFAppContext* app_context)
{
	AudioContext* ctx = get_audio_ctx(app_context);
	if (!ctx) return;

	for (int i = 0; i < MAX_SOUND_CHANNELS; i++)
	{
		if (ctx->channels[i].active)
		{
			ctx->channels[i].active = 0;
			if (ctx->channels[i].pcm_data)
			{
				free(ctx->channels[i].pcm_data);
				ctx->channels[i].pcm_data = NULL;
			}
		}
	}
}

void audio_stream_head(SWFAppContext* app_context,
	u8 format, u8 rate, u8 sample_size, u8 stereo,
	u16 avg_sample_count)
{
	AudioContext* ctx = get_audio_ctx(app_context);
	if (!ctx) return;

	ctx->stream.active = 1;
	ctx->stream.format = format;
	ctx->stream.rate = rate;
	ctx->stream.sample_size = sample_size;
	ctx->stream.stereo = stereo;
	ctx->stream.sample_rate = rate_code_to_hz(rate);
	ctx->stream.channels = stereo ? 2 : 1;

	// Pre-buffer 0.5 seconds of audio before starting playback.
	// emscripten_sleep() has inherent overhead making frames run slightly slower
	// than real-time, so the audio callback consumes data faster than frames
	// produce it. A large pre-buffer absorbs this drift for many seconds.
	ctx->stream.prebuffer = (size_t)ctx->stream.sample_rate / 2;
	ctx->stream.started = 0;

	// Allocate a streaming buffer (enough for ~2 seconds)
	size_t buf_samples = (size_t)ctx->stream.sample_rate * 2;
	ctx->stream.buffer_size = buf_samples;
	ctx->stream.pcm_buffer = (float*)calloc(buf_samples * ctx->stream.channels, sizeof(float));
	ctx->stream.write_pos = 0;
	ctx->stream.read_pos = 0;
	ctx->stream.read_pos_frac = 0.0;
}

void audio_stream_block(SWFAppContext* app_context,
	const u8* data, size_t data_size)
{
	AudioContext* ctx = get_audio_ctx(app_context);
	if (!ctx || !ctx->stream.active) return;

	if (ctx->stream.format == SOUND_FORMAT_MP3)
	{
		// MP3 streaming blocks have: SampleCount(UI16) + SeekSamples(SI16) + MP3Frames
		if (data_size < 4) return;
		const u8* mp3_data = data + 4;
		size_t mp3_size = data_size - 4;

		size_t pcm_samples;
		int channels, rate;
		float* pcm = decode_mp3(mp3_data, mp3_size, &pcm_samples, &channels, &rate);
		if (!pcm) return;

		// Write into ring buffer
		int ch = ctx->stream.channels;
		for (size_t i = 0; i < pcm_samples; i++)
		{
			size_t wp = ctx->stream.write_pos % ctx->stream.buffer_size;
			for (int c = 0; c < ch && c < channels; c++)
				ctx->stream.pcm_buffer[wp * ch + c] = pcm[i * channels + c];
			ctx->stream.write_pos++;
		}

		free(pcm);
	}
	else if (ctx->stream.format == SOUND_FORMAT_UNCOMPRESSED_NE ||
			 ctx->stream.format == SOUND_FORMAT_UNCOMPRESSED_LE)
	{
		int ch = ctx->stream.channels;
		if (ctx->stream.sample_size == 1) // 16-bit
		{
			size_t pcm_samples = data_size / (2 * ch);
			const s16* src = (const s16*)data;
			for (size_t i = 0; i < pcm_samples; i++)
			{
				size_t wp = ctx->stream.write_pos % ctx->stream.buffer_size;
				for (int c = 0; c < ch; c++)
					ctx->stream.pcm_buffer[wp * ch + c] = src[i * ch + c] / 32768.0f;
				ctx->stream.write_pos++;
			}
		}
		else // 8-bit
		{
			size_t pcm_samples = data_size / ch;
			for (size_t i = 0; i < pcm_samples; i++)
			{
				size_t wp = ctx->stream.write_pos % ctx->stream.buffer_size;
				for (int c = 0; c < ch; c++)
					ctx->stream.pcm_buffer[wp * ch + c] = (data[i * ch + c] - 128) / 128.0f;
				ctx->stream.write_pos++;
			}
		}
	}
}

void audio_mix(AudioContext* ctx, float* output, size_t frames, int out_channels, int out_rate)
{
	memset(output, 0, frames * out_channels * sizeof(float));

	// Mix event sounds
	for (int i = 0; i < MAX_SOUND_CHANNELS; i++)
	{
		SoundChannel* ch = &ctx->channels[i];
		if (!ch->active) continue;

		// Resample source → output rate. Without this, sources at
		// 22050 Hz / 11025 Hz play 2× / 4× faster + higher-pitched at
		// the 44100 Hz hardware output (classic chipmunk effect).
		// Nearest-neighbor is adequate for game SFX and avoids the
		// complexity of linear interpolation.
		double rate_ratio = (out_rate > 0)
			? ((double)ch->sample_rate / (double)out_rate)
			: 1.0;

		for (size_t f = 0; f < frames; f++)
		{
			size_t int_pos = (size_t)ch->pcm_position;
			if (int_pos >= ch->pcm_samples)
			{
				if (ch->loop_count > 0)
				{
					// AVM2 play(startTime, loops) restarts each loop at
					// startTime (loop_start); AVM1 starts leave it at 0.
					ch->loop_count--;
					ch->pcm_position = ch->loop_start;
					int_pos = (size_t)ch->pcm_position;
					if (int_pos >= ch->pcm_samples)
					{
						ch->active = 0;
						free(ch->pcm_data);
						ch->pcm_data = NULL;
						break;
					}
				}
				else
				{
					ch->active = 0;
					free(ch->pcm_data);
					ch->pcm_data = NULL;
					break;
				}
			}

			// Per-channel SoundTransform (identity for AVM1 sounds, which
			// reduces to the plain src*master_volume mix bit-for-bit).
			float in_l = ch->pcm_data[int_pos * ch->channels];
			float in_r = (ch->channels > 1)
				? ch->pcm_data[int_pos * ch->channels + 1] : in_l;
			float out_l = ch->gain_vol
				* (in_l * ch->gain_l2l + in_r * ch->gain_r2l) * ctx->master_volume;
			float out_r = ch->gain_vol
				* (in_l * ch->gain_l2r + in_r * ch->gain_r2r) * ctx->master_volume;
			output[f * out_channels] += out_l;
			if (out_channels > 1)
				output[f * out_channels + 1] += out_r;
			ch->pcm_position += rate_ratio;
		}
	}

	// Mix streaming sound
	if (ctx->stream.active && ctx->stream.pcm_buffer)
	{
		// Wait until enough data is buffered before starting playback.
		// Also re-triggers after underrun to accumulate a fresh buffer.
		if (!ctx->stream.started)
		{
			size_t available = ctx->stream.write_pos - ctx->stream.read_pos;
			if (available >= ctx->stream.prebuffer)
				ctx->stream.started = 1;
		}

		if (ctx->stream.started)
		{
			int ch = ctx->stream.channels;

			// Resample stream rate → output rate. Same chipmunk-effect bug
			// as event sounds: a 22050 Hz stream against a 44100 Hz output
			// played 2× too fast / pitched without this. read_pos stays an
			// integer source-sample index (so buffer-occupancy and underrun
			// checks against write_pos remain correct); read_pos_frac
			// accumulates the fractional advance and promotes whole steps.
			double rate_ratio = (out_rate > 0)
				? ((double)ctx->stream.sample_rate / (double)out_rate)
				: 1.0;

			for (size_t f = 0; f < frames; f++)
			{
				if (ctx->stream.read_pos >= ctx->stream.write_pos)
				{
					// Buffer underrun — stop reading and rebuffer
					ctx->stream.started = 0;
					break;
				}

				size_t rp = ctx->stream.read_pos % ctx->stream.buffer_size;
				for (int c = 0; c < out_channels; c++)
				{
					int src_c = (c < ch) ? c : 0;
					output[f * out_channels + c] +=
						ctx->stream.pcm_buffer[rp * ch + src_c] * ctx->master_volume;
				}
				ctx->stream.read_pos_frac += rate_ratio;
				size_t step = (size_t)ctx->stream.read_pos_frac;
				ctx->stream.read_pos += step;
				ctx->stream.read_pos_frac -= (double)step;
			}
		}
	}

	// Clamp output
	for (size_t i = 0; i < frames * (size_t)out_channels; i++)
	{
		if (output[i] > 1.0f) output[i] = 1.0f;
		if (output[i] < -1.0f) output[i] = -1.0f;
	}
}

void audio_shutdown(SWFAppContext* app_context)
{
	AudioContext* ctx = get_audio_ctx(app_context);
	if (!ctx) return;

	audio_stop_all_sounds(app_context);

	if (ctx->stream.pcm_buffer)
	{
		free(ctx->stream.pcm_buffer);
		ctx->stream.pcm_buffer = NULL;
	}

	free(ctx->assets);
	free(ctx);
	app_context->audio_ctx = NULL;
}

// ---------------------------------------------------------------------------
// AVM2 SoundChannel-shaped API (see audio.h). Handles are (index, generation)
// pairs; every operation validates the generation so a stale AS3 SoundChannel
// can never affect a reused slot.
// ---------------------------------------------------------------------------

static SoundChannel* channel_for_handle(SWFAppContext* app_context, int ch,
	u32 generation)
{
	AudioContext* ctx = get_audio_ctx(app_context);
	if (!ctx || ch < 0 || ch >= MAX_SOUND_CHANNELS) return NULL;
	SoundChannel* channel = &ctx->channels[ch];
	if (!channel->active || channel->generation != generation) return NULL;
	return channel;
}

int audio_start_sound_ex(SWFAppContext* app_context, u16 sound_id,
	u32 loop_count, double start_ms, const float gains[5],
	u32* out_generation)
{
	AudioContext* ctx = get_audio_ctx(app_context);
	if (!ctx) return -1;

	const SoundAsset* asset = find_asset(ctx, sound_id);
	if (!asset) return -1;

	int ch = start_sound_core(ctx, asset, loop_count);
	if (ch < 0) return -1;

	SoundChannel* channel = &ctx->channels[ch];
	if (start_ms > 0.0)
	{
		double start_sample = start_ms / 1000.0 * (double)channel->sample_rate;
		if (start_sample < (double)channel->pcm_samples)
		{
			channel->pcm_position = start_sample;
			channel->loop_start = start_sample;
		}
	}
	if (gains != NULL)
	{
		channel->gain_l2l = gains[0];
		channel->gain_l2r = gains[1];
		channel->gain_r2l = gains[2];
		channel->gain_r2r = gains[3];
		channel->gain_vol = gains[4];
	}
	if (out_generation) *out_generation = channel->generation;
	return ch;
}

int audio_channel_active(SWFAppContext* app_context, int ch, u32 generation)
{
	return channel_for_handle(app_context, ch, generation) != NULL;
}

void audio_channel_stop(SWFAppContext* app_context, int ch, u32 generation)
{
	SoundChannel* channel = channel_for_handle(app_context, ch, generation);
	if (!channel) return;
	channel->active = 0;
	if (channel->pcm_data)
	{
		free(channel->pcm_data);
		channel->pcm_data = NULL;
	}
}

void audio_channel_set_gains(SWFAppContext* app_context, int ch,
	u32 generation, const float gains[5])
{
	SoundChannel* channel = channel_for_handle(app_context, ch, generation);
	if (!channel || gains == NULL) return;
	channel->gain_l2l = gains[0];
	channel->gain_l2r = gains[1];
	channel->gain_r2l = gains[2];
	channel->gain_r2r = gains[3];
	channel->gain_vol = gains[4];
}

double audio_channel_position_ms(SWFAppContext* app_context, int ch,
	u32 generation)
{
	SoundChannel* channel = channel_for_handle(app_context, ch, generation);
	if (!channel || channel->sample_rate <= 0) return -1.0;
	return channel->pcm_position / (double)channel->sample_rate * 1000.0;
}

void audio_set_master_volume(SWFAppContext* app_context, float volume)
{
	AudioContext* ctx = get_audio_ctx(app_context);
	if (!ctx) return;
	ctx->master_volume = volume;
}

// ---------------------------------------------------------------------------
// Tag wrapper functions (called from generated code)
// ---------------------------------------------------------------------------

void tagDefineSound(SWFAppContext* app_context, u16 sound_id,
	u8 format, u8 rate, u8 sample_size, u8 stereo,
	u32 sample_count, const u8* data, size_t data_size)
{
	if (!app_context->audio_ctx)
		audio_init(app_context);

	audio_define_sound(app_context, sound_id,
		format, rate, sample_size, stereo,
		sample_count, data, data_size);

	extern void ng_registerSoundMetadata(u16 sound_id, u8 rate, u32 sample_count);
	ng_registerSoundMetadata(sound_id, rate, sample_count);
}

void tagStartSound(SWFAppContext* app_context, u16 sound_id,
	int stop, u32 loop_count, u32 in_point, u32 out_point)
{
	if (!app_context->audio_ctx)
		audio_init(app_context);

	// Browser-WASM swf.c re-runs frame_funcs[current_frame] every tick
	// (no is_playing gate outside OFFSCREEN_RENDER — see [[browser-wasm-frame-func-rerun]]).
	// Without dedup, a stopped frame holding a StartSound tag re-triggers the
	// sound every tick → audible cacophony (Snake frame_56 game-over SFX).
	// SWF semantics: StartSound is a frame tag and fires once per frame entry.
	// Dedup by (sound_id, current_frame): suppress repeats until current_frame
	// changes. Stop=1 passes through unconditionally.
	if (!stop)
	{
		extern size_t current_frame;
		static size_t s_dedup_frame = (size_t)-1;
		static u16    s_dedup_ids[32];
		static int    s_dedup_count = 0;
		if (current_frame != s_dedup_frame)
		{
			s_dedup_frame = current_frame;
			s_dedup_count = 0;
		}
		for (int i = 0; i < s_dedup_count; i++)
		{
			if (s_dedup_ids[i] == sound_id) return;
		}
		if (s_dedup_count < (int)(sizeof(s_dedup_ids)/sizeof(s_dedup_ids[0])))
			s_dedup_ids[s_dedup_count++] = sound_id;
	}

	audio_start_sound(app_context, sound_id, stop, loop_count, in_point, out_point);
}

void tagSoundStreamHead(SWFAppContext* app_context,
	u8 format, u8 rate, u8 sample_size, u8 stereo,
	u16 avg_sample_count)
{
	if (!app_context->audio_ctx)
		audio_init(app_context);

	audio_stream_head(app_context, format, rate, sample_size, stereo, avg_sample_count);
}

void tagSoundStreamBlock(SWFAppContext* app_context,
	const u8* data, size_t data_size)
{
	if (!app_context->audio_ctx)
		audio_init(app_context);

	audio_stream_block(app_context, data, data_size);
}

void tagStopAllSounds(SWFAppContext* app_context)
{
	if (!app_context->audio_ctx) return;
	audio_stop_all_sounds(app_context);
}

#endif
