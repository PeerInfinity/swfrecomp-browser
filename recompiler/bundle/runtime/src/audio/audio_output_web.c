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

// Called from JS: the AudioWorklet's "need" handler (or the legacy
// ScriptProcessorNode callback), once per 2048-frame quantum
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

			// One mix quantum. Both output paths mix exactly 2048 frames per
			// audio_fill_buffer call, so SoundChannel.position (pcm_position)
			// advances in the same steps at the same rate either way.
			var bufferSize = 2048;

			// Mix one quantum and return a copy of the interleaved stereo floats.
			var fillChunk = function(frames) {
				var chunk = new Float32Array(frames * 2);
				if (!Module._audio_fill_buffer) return chunk;
				var bufPtr = Module._audio_fill_buffer(frames);
				// Get float view of WASM memory (handle both exported HEAPF32 and direct access)
				var heap = Module.HEAPF32;
				if (!heap) {
					var mem = Module.wasmMemory || (Module.asm && Module.asm.memory);
					if (!mem) return chunk;
					heap = new Float32Array(mem.buffer);
				}
				var offset = bufPtr >> 2;
				chunk.set(heap.subarray(offset, offset + frames * 2));
				return chunk;
			};

			// Legacy path: ScriptProcessorNode. Its onaudioprocess runs on the
			// main thread, and when that thread stalls Chrome REPLAYS the node's
			// last output buffers in a loop until it is free again. Only used
			// where AudioWorklet is unavailable (insecure context, old browser)
			// or when window.SWF_AUDIO_SCRIPTPROCESSOR forces it for A/B tests.
			var startScriptProcessor = function() {
				var processor = ctx.createScriptProcessor(bufferSize, 0, 2);
				processor.onaudioprocess = function(e) {
					if (!Module._audio_fill_buffer) return;
					var outL = e.outputBuffer.getChannelData(0);
					var outR = e.outputBuffer.getChannelData(1);
					var frames = e.outputBuffer.length;
					var chunk = fillChunk(frames);
					for (var i = 0; i < frames; i++) {
						outL[i] = chunk[i * 2];
						outR[i] = chunk[i * 2 + 1];
					}
				};
				processor.connect(ctx.destination);
				window._swfAudioOut = processor;
				window._swfAudioPath = 'scriptprocessor';
			};

			// Default path: AudioWorkletNode fed by a FIFO of mixed chunks. The
			// worklet runs on the audio thread; when its FIFO is empty (main
			// thread stalled) it fades to SILENCE and never replays. It keeps
			// queued + requested frames at targetChunks quanta by posting one
			// {need} per missing chunk; the main thread answers each with one
			// audio_fill_buffer(2048) mix. Not a SharedArrayBuffer ring: that
			// needs COOP/COEP, which GitHub Pages cannot serve.
			//
			// Sound clock: the frames played as silence while starved are
			// reported back as {skip: k} whole quanta, and the main thread runs
			// k mixes and DISCARDS them. So audio_mix still runs once per 2048
			// frames of audio clock through a stall, and SoundChannel.position
			// (pcm_position) keeps pace with real time exactly as it did with
			// the ScriptProcessor (whose queued callbacks all ran after the
			// stall) and as in Flash (mixer on the audio thread); only the
			// stalled audio itself is not heard.
			var workletSrc =
				"class SwfAudioOut extends AudioWorkletProcessor {" +
				"  constructor(opts) {" +
				"    super();" +
				"    var o = (opts && opts.processorOptions) || {};" +
				"    this.chunk = o.chunkFrames;" +
				"    this.target = o.targetChunks;" +
				"    this.fadeLen = o.fadeFrames;" +
				"    this.q = []; this.off = 0; this.queued = 0; this.inflight = 0;" +
				"    this.lastL = 0; this.lastR = 0;" +
				"    this.starved = true; this.fadeOut = 0; this.fadeIn = 0;" +
				"    this.primed = false; this.skipped = 0;" +
				"    var self = this;" +
				"    this.port.onmessage = function(e) {" +
				"      var d = e.data;" +
				"      if (self.inflight > 0) self.inflight--;" +
				"      if (d && d.length) { self.q.push(d); self.queued += d.length >> 1; self.primed = true; }" +
				"      self.reportSkip();" +
				"    };" +
				"  }" +
				"  reportSkip() {" +
				"    if (this.skipped < this.chunk) return;" +
				"    var k = Math.floor(this.skipped / this.chunk);" +
				"    this.skipped -= k * this.chunk;" +
				"    this.port.postMessage({ skip: k, need: false });" +
				"  }" +
				"  process(inputs, outputs) {" +
				"    var out = outputs[0], L = out[0], R = out[1] || out[0], n = L.length;" +
				"    for (var i = 0; i < n; i++) {" +
				"      if (this.queued > 0) {" +
				"        var c = this.q[0], j = this.off * 2, l = c[j], r = c[j + 1];" +
				"        if (this.starved) { this.starved = false; this.fadeIn = 0; }" +
				"        if (this.fadeIn < this.fadeLen) {" +
				"          var g = (this.fadeIn + 1) / (this.fadeLen + 1); l *= g; r *= g; this.fadeIn++;" +
				"        }" +
				"        L[i] = l; R[i] = r; this.lastL = l; this.lastR = r;" +
				"        this.queued--;" +
				"        if (++this.off * 2 >= c.length) { this.q.shift(); this.off = 0; }" +
				"      } else {" +
				"        if (!this.starved) { this.starved = true; this.fadeOut = this.fadeLen; }" +
				"        if (this.primed) this.skipped++;" +
				"        if (this.fadeOut > 0) {" +
				"          var h = this.fadeOut / (this.fadeLen + 1);" +
				"          L[i] = this.lastL * h; R[i] = this.lastR * h; this.fadeOut--;" +
				"        } else { L[i] = 0; R[i] = 0; }" +
				"      }" +
				"    }" +
				"    while (this.queued + this.inflight * this.chunk < this.target * this.chunk) {" +
				"      this.inflight++; this.port.postMessage({ skip: 0, need: true });" +
				"    }" +
				"    return true;" +
				"  }" +
				"}" +
				"registerProcessor('swf-audio-out', SwfAudioOut);";

			var canWorklet = !window.SWF_AUDIO_SCRIPTPROCESSOR &&
				ctx.audioWorklet && typeof AudioWorkletNode !== 'undefined';
			if (!canWorklet) {
				startScriptProcessor();
			} else {
				var url = URL.createObjectURL(new Blob([workletSrc], { type: 'text/javascript' }));
				ctx.audioWorklet.addModule(url).then(function() {
					URL.revokeObjectURL(url);
					if (window._swfAudioCtx !== ctx) return;  // shut down meanwhile
					var node = new AudioWorkletNode(ctx, 'swf-audio-out', {
						numberOfInputs: 0,
						numberOfOutputs: 1,
						outputChannelCount: [2],
						processorOptions: {
							chunkFrames: bufferSize,
							// ~139 ms queued: a request leaves >= 2 quanta
							// (~93 ms) of margin, like the ScriptProcessor's
							// double buffering.
							targetChunks: 3,
							// ~2.9 ms fade on underrun / resume, avoids clicks.
							fadeFrames: 128
						}
					});
					node.port.onmessage = function(e) {
						var m = e.data;
						for (var k = 0; k < m.skip; k++) fillChunk(bufferSize);
						if (!m.need) return;
						var chunk = fillChunk(bufferSize);
						node.port.postMessage(chunk, [chunk.buffer]);
					};
					node.connect(ctx.destination);
					window._swfAudioOut = node;
					window._swfAudioPath = 'worklet';
				}).catch(function(err) {
					console.warn('[audio] AudioWorklet unavailable, using ScriptProcessor:', err);
					if (window._swfAudioCtx === ctx) startScriptProcessor();
				});
			}

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
			window._swfAudioOut = null;
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
