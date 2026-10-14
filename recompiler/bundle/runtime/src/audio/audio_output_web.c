#ifndef NO_GRAPHICS

#include <audio/audio.h>
#include <swf.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Buffer shared between JS audio callback and C audio_mix
#define AUDIO_BUFFER_FRAMES 2048
#define AUDIO_CHANNELS 2
#define AUDIO_SAMPLE_RATE 44100

static float audio_buffer[AUDIO_BUFFER_FRAMES * AUDIO_CHANNELS];
static SWFAppContext* audio_app_ctx = NULL;

// Called from JS ScriptProcessorNode callback
EMSCRIPTEN_KEEPALIVE
float* audio_fill_buffer(int frames)
{
	AudioContext* ctx = audio_app_ctx ? (AudioContext*)audio_app_ctx->audio_ctx : NULL;
	if (!ctx)
	{
		memset(audio_buffer, 0, frames * AUDIO_CHANNELS * sizeof(float));
		return audio_buffer;
	}

	audio_mix(ctx, audio_buffer, (size_t)frames, AUDIO_CHANNELS, AUDIO_SAMPLE_RATE);
	return audio_buffer;
}

void audio_output_init(SWFAppContext* app_context)
{
	audio_app_ctx = app_context;

	EM_ASM({
		if (typeof window === 'undefined') return;

		// Create audio context on first user interaction (autoplay policy)
		var startAudio = function() {
			if (window._swfAudioStarted) {
				// Already initialized — but try to resume if suspended
				if (window._swfAudioCtx && window._swfAudioCtx.state === 'suspended') {
					window._swfAudioCtx.resume();
				}
				return;
			}
			window._swfAudioStarted = true;

			var AudioContext = window.AudioContext || window.webkitAudioContext;
			if (!AudioContext) return;

			var ctx = new AudioContext({ sampleRate: 44100 });
			window._swfAudioCtx = ctx;

			// Use ScriptProcessorNode (widely supported)
			var bufferSize = 2048;
			var processor = ctx.createScriptProcessor(bufferSize, 0, 2);

			processor.onaudioprocess = function(e) {
				if (!Module._audio_fill_buffer) return;
				var outL = e.outputBuffer.getChannelData(0);
				var outR = e.outputBuffer.getChannelData(1);
				var frames = e.outputBuffer.length;

				// Call C to fill the buffer
				var bufPtr = Module._audio_fill_buffer(frames);

				// Get float view of WASM memory (handle both exported HEAPF32 and direct access)
				var heap = Module.HEAPF32;
				if (!heap) {
					var mem = Module.wasmMemory || (Module.asm && Module.asm.memory);
					if (!mem) return;
					heap = new Float32Array(mem.buffer);
				}
				var offset = bufPtr >> 2;

				for (var i = 0; i < frames; i++) {
					outL[i] = heap[offset + i * 2];
					outR[i] = heap[offset + i * 2 + 1];
				}
			};

			processor.connect(ctx.destination);

			// Resume context if suspended
			if (ctx.state === 'suspended') {
				ctx.resume();
			}
		};

		// Try to start immediately, and also on user interaction
		try { startAudio(); } catch(e) {}
		document.addEventListener('click', startAudio, { once: false });
		document.addEventListener('keydown', startAudio, { once: false });
		document.addEventListener('touchstart', startAudio, { once: false });
	});
}

void audio_output_shutdown(void)
{
	EM_ASM({
		if (window._swfAudioCtx) {
			window._swfAudioCtx.close();
			window._swfAudioCtx = null;
			window._swfAudioStarted = false;
		}
	});
	audio_app_ctx = NULL;
}

#else
// Native builds - stub for now
void audio_output_init(SWFAppContext* app_context)
{
	(void)app_context;
}

void audio_output_shutdown(void)
{
}
#endif

#endif // NO_GRAPHICS
