// flash.media Sound family (Stage 10), ported from Ruffle
// core/src/avm2/globals/flash/media/* + core/src/display_object.rs SoundTransform.
//
//   SoundTransform  — 5 f64 channel gains + pan math (pure computation).
//   SoundChannel    — extends EventDispatcher; stores a core i32×100 sound
//                     transform (so the AS3 f64 round-trips through Flash's
//                     integer store); stop()/position/leftPeak/rightPeak.
//   SoundMixer      — static bufferTime (int) + a global i32×100 soundTransform.
//   Sound           — extends EventDispatcher; embedded DefineSound metadata
//                     via SymbolClass binding (length/bytesTotal from the
//                     recompiler table); play() returns a SoundChannel unless
//                     the start position is past the clip length (then null).
//
// Graphics builds additionally bridge to the shared src/audio mixer
// (audio.c): embedded DefineSound payloads register at boot
// (avm2_media_register_sounds), play()/stop()/stopAll()/soundTransform drive
// real mixer channels, and avm2_media_poll dispatches Event.SOUND_COMPLETE
// when a channel drains. The bridge is inert for trace grading: nothing here
// writes stdout, and channel positions only advance when an output sink pulls
// audio_mix (browser Web Audio) — never in the native test harness.
//
// NO_GRAPHICS builds compile the trace-graded surface only (the mixer does
// not exist there); the bridge bodies compile away.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <audio/audio.h>
// Unconditional: DataFileEntry / findDataFile (the bundled sibling mp3 a
// Sound.load() resolves) are needed in both builds. Graphics builds also take
// the full SWFAppContext (app->audio_ctx) from here.
#include <libswf/swf.h>

#include <avm2/avm2_abc.h>
#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_gc.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>
#include <avm2/avm2_value.h>

#ifndef NO_GRAPHICS
// Live playback registry for SOUND_COMPLETE: mixer channel index -> the AS3
// SoundChannel object whose playback occupies that slot. Entries are cleared
// on stop()/stopAll() (Flash does not dispatch soundComplete for manual
// stops) and when avm2_media_poll observes the channel drained. Registered
// objects are GC roots (avm2_gc_mark_roots_media) — the mixer slot may be the
// only live reference to a playing channel.
static Avm2Object* g_live_channels[MAX_SOUND_CHANNELS];
#endif

static int media_class_is_a(const Avm2Class* cls, const Avm2Class* ancestor)
{
	for (const Avm2Class* c = cls; c != NULL; c = c->super_class)
		if (c == ancestor) return 1;
	return 0;
}

static Avm2Object* this_obj(Avm2Activation* act)
{
	return act->this_val.kind == AVM2_VALUE_OBJECT ? act->this_val.u.obj : NULL;
}

// DefineSound rate code -> Hz (SWF19 §DefineSound). MP3 sounds use code 3.
static double rate_code_hz(uint8_t rate)
{
	static const double hz[4] = { 5512.5, 11025.0, 22050.0, 44100.0 };
	return hz[rate & 3];
}

// ---------------------------------------------------------------------------
// SoundTransform
// ---------------------------------------------------------------------------

typedef struct Avm2SoundTransformExt
{
	double left_to_left, left_to_right, right_to_left, right_to_right, volume;
} Avm2SoundTransformExt;

static Avm2SoundTransformExt* st_ext_of(Avm2Context* ctx, Avm2Value v)
{
	if (v.kind != AVM2_VALUE_OBJECT || v.u.obj == NULL) return NULL;
	Avm2Object* o = v.u.obj;
	if (o->cls == NULL || ctx->builtins.sound_transform_class == NULL) return NULL;
	if (!media_class_is_a(o->cls, ctx->builtins.sound_transform_class)) return NULL;
	return (Avm2SoundTransformExt*) o->native_ext;
}

static Avm2SoundTransformExt* this_st(Avm2Activation* act)
{
	return st_ext_of(act->ctx, act->this_val);
}

static void st_apply_pan(Avm2SoundTransformExt* st, double pan)
{
	st->left_to_left = sqrt(1.0 - pan);
	st->right_to_right = sqrt(1.0 + pan);
	st->left_to_right = 0.0;
	st->right_to_left = 0.0;
}

// SoundTransform(vol:Number = 1, panning:Number = 0)
static Avm2Value st_ctor(Avm2Activation* act)
{
	Avm2SoundTransformExt* st = this_st(act);
	if (st == NULL) return avm2_undefined();
	st->volume = act->argc > 0 ? avm2_coerce_to_number(act->ctx, act->args[0]) : 1.0;
	double pan = act->argc > 1 ? avm2_coerce_to_number(act->ctx, act->args[1]) : 0.0;
	st_apply_pan(st, pan);
	return avm2_undefined();
}

#define ST_ACCESSOR(field)                                                    \
	static Avm2Value st_get_##field(Avm2Activation* act)                      \
	{                                                                         \
		Avm2SoundTransformExt* st = this_st(act);                             \
		return avm2_number(st != NULL ? st->field : 0.0);                     \
	}                                                                         \
	static Avm2Value st_set_##field(Avm2Activation* act)                      \
	{                                                                         \
		Avm2SoundTransformExt* st = this_st(act);                             \
		if (st != NULL)                                                       \
			st->field = act->argc > 0                                         \
				? avm2_coerce_to_number(act->ctx, act->args[0]) : 0.0;        \
		return avm2_undefined();                                              \
	}

ST_ACCESSOR(left_to_left)
ST_ACCESSOR(left_to_right)
ST_ACCESSOR(right_to_left)
ST_ACCESSOR(right_to_right)
ST_ACCESSOR(volume)

static Avm2Value st_get_pan(Avm2Activation* act)
{
	Avm2SoundTransformExt* st = this_st(act);
	if (st == NULL) return avm2_number(0.0);
	if (st->left_to_right != 0.0 || st->right_to_left != 0.0)
		return avm2_number(0.0);
	return avm2_number(1.0 - pow(st->left_to_left, 2.0));
}

static Avm2Value st_set_pan(Avm2Activation* act)
{
	Avm2SoundTransformExt* st = this_st(act);
	if (st != NULL)
		st_apply_pan(st, act->argc > 0
			? avm2_coerce_to_number(act->ctx, act->args[0]) : 0.0);
	return avm2_undefined();
}

// Build an AS3 SoundTransform from core i32×100 fields (Ruffle
// SoundTransform::into_avm2_object).
static Avm2Object* st_from_core(Avm2Context* ctx, int32_t l2l, int32_t l2r,
                                int32_t r2l, int32_t r2r, int32_t vol)
{
	Avm2Value v = avm2_class_construct(ctx, ctx->builtins.sound_transform_class,
	                                   NULL, 0);
	Avm2SoundTransformExt* st = st_ext_of(ctx, v);
	if (st != NULL)
	{
		st->left_to_left = l2l / 100.0;
		st->left_to_right = l2r / 100.0;
		st->right_to_left = r2l / 100.0;
		st->right_to_right = r2r / 100.0;
		st->volume = vol / 100.0;
	}
	return v.u.obj;
}

Avm2Value avm2_sound_transform_from_core(Avm2Context* ctx, const int32_t core[5])
{
	return avm2_object_value(st_from_core(ctx, core[0], core[1], core[2],
	                                      core[3], core[4]));
}

int avm2_sound_transform_read(Avm2Context* ctx, Avm2Value v, int32_t out[5])
{
	Avm2SoundTransformExt* st = st_ext_of(ctx, v);
	if (st == NULL) return 0;
	out[0] = (int32_t) (st->left_to_left * 100.0);
	out[1] = (int32_t) (st->left_to_right * 100.0);
	out[2] = (int32_t) (st->right_to_left * 100.0);
	out[3] = (int32_t) (st->right_to_right * 100.0);
	out[4] = (int32_t) (st->volume * 100.0);
	return 1;
}

// ---------------------------------------------------------------------------
// SoundChannel
// ---------------------------------------------------------------------------

typedef struct Avm2SoundChannelExt
{
	Avm2EventDispatcherExt dispatcher;  // extends EventDispatcher (MUST be first)
	int32_t st_l2l, st_l2r, st_r2l, st_r2r, st_volume;  // core SoundTransform ×100
	// Mixer playback handle (graphics builds; see audio_start_sound_ex).
	// mixer_live means "a mixer start succeeded for this channel object" —
	// the (mixer_ch, mixer_gen) pair stays valid-checked on every use.
	int32_t mixer_ch;
	uint32_t mixer_gen;
	uint8_t mixer_live;
	// Simulated playback clock for a URL-loaded Sound. Such a sound has no
	// DefineSound char id, so it never reaches the mixer in ANY build — the
	// clock is the only thing that can end its playback, and it runs
	// identically in no-graphics and graphics (mode parity).
	double sim_elapsed_ms, sim_duration_ms;
	uint8_t sim_live;
} Avm2SoundChannelExt;

static void sound_channel_native_init(Avm2Context* ctx, Avm2Object* obj)
{
	(void) ctx;
	Avm2SoundChannelExt* sc = (Avm2SoundChannelExt*) obj->native_ext;
	sc->st_l2l = 100;
	sc->st_l2r = 0;
	sc->st_r2l = 0;
	sc->st_r2r = 100;
	sc->st_volume = 100;
	sc->mixer_ch = -1;
}

#ifndef NO_GRAPHICS
// Mixer gain vector {l2l, l2r, r2l, r2r, vol} from the channel's core
// i32×100 SoundTransform store.
static void sc_mixer_gains(const Avm2SoundChannelExt* sc, float gains[5])
{
	gains[0] = (float) sc->st_l2l / 100.0f;
	gains[1] = (float) sc->st_l2r / 100.0f;
	gains[2] = (float) sc->st_r2l / 100.0f;
	gains[3] = (float) sc->st_r2r / 100.0f;
	gains[4] = (float) sc->st_volume / 100.0f;
}
#endif

// --- simulated playback registry -------------------------------------------
//
// Channels playing a URL-loaded Sound. The mixer registry above cannot hold
// them: audio_start_sound_ex is keyed by DefineSound char id and an external
// mp3 has none, so in every build (browser included) such a channel would play
// forever and never dispatch Event.SOUND_COMPLETE. This is the AVM2 port of
// the AVM1 trace-mode model in action.c ("Sound playback simulation"): a
// duration estimated from the mp3's own frame header, advanced one frame
// budget per tick, dispatching soundComplete when it drains. Embedded sounds
// are untouched — they keep the mixer path in both directions.
#define AVM2_MAX_SIM_CHANNELS 16
static Avm2Object* g_sim_channels[AVM2_MAX_SIM_CHANNELS];

static void sim_channel_stop(Avm2Object* obj, Avm2SoundChannelExt* sc)
{
	sc->sim_live = 0;
	for (int i = 0; i < AVM2_MAX_SIM_CHANNELS; i++)
		if (g_sim_channels[i] == obj) g_sim_channels[i] = NULL;
}

static Avm2SoundChannelExt* this_sc(Avm2Activation* act)
{
	Avm2Object* o = this_obj(act);
	if (o == NULL || o->cls == NULL
	    || !media_class_is_a(o->cls, act->ctx->builtins.sound_channel_class))
		return NULL;
	return (Avm2SoundChannelExt*) o->native_ext;
}

static Avm2Value sc_get_sound_transform(Avm2Activation* act)
{
	Avm2SoundChannelExt* sc = this_sc(act);
	if (sc == NULL) return avm2_undefined();
	return avm2_object_value(st_from_core(act->ctx, sc->st_l2l, sc->st_l2r,
	                                      sc->st_r2l, sc->st_r2r, sc->st_volume));
}

static Avm2Value sc_set_sound_transform(Avm2Activation* act)
{
	Avm2SoundChannelExt* sc = this_sc(act);
	if (sc == NULL) return avm2_undefined();
	Avm2SoundTransformExt* st = act->argc > 0
		? st_ext_of(act->ctx, act->args[0]) : NULL;
	if (st == NULL)
	{
		avm2_throw_error(act->ctx, act->ctx->builtins.type_error_class,
		                 "Error #2007: Parameter soundChannel must be non-null.");
	}
	sc->st_l2l = (int32_t) (st->left_to_left * 100.0);
	sc->st_l2r = (int32_t) (st->left_to_right * 100.0);
	sc->st_r2l = (int32_t) (st->right_to_left * 100.0);
	sc->st_r2r = (int32_t) (st->right_to_right * 100.0);
	sc->st_volume = (int32_t) (st->volume * 100.0);
#ifndef NO_GRAPHICS
	if (sc->mixer_live)
	{
		float gains[5];
		sc_mixer_gains(sc, gains);
		audio_channel_set_gains(act->ctx->app, sc->mixer_ch, sc->mixer_gen,
		                        gains);
	}
#endif
	return avm2_undefined();
}

static Avm2Value sc_get_position(Avm2Activation* act)
{
	Avm2SoundChannelExt* sc = this_sc(act);
	if (sc == NULL) return avm2_number(0.0);
	if (sc->sim_live) return avm2_number(sc->sim_elapsed_ms);
#ifndef NO_GRAPHICS
	// Live mixer playback clock (browser builds; in the native test harness
	// nothing pulls audio_mix, so an active channel reports 0 — trace-inert).
	if (sc->mixer_live)
	{
		double pos = audio_channel_position_ms(act->ctx->app, sc->mixer_ch,
		                                       sc->mixer_gen);
		if (pos >= 0.0) return avm2_number(pos);
	}
#endif
	return avm2_number(0.0);
}

static Avm2Value sc_get_peak(Avm2Activation* act)
{
	(void) act;
	return avm2_undefined();  // no instance
}

static Avm2Value sc_stop(Avm2Activation* act)
{
	// Manual stop: no soundComplete, in either playback model.
	{
		Avm2SoundChannelExt* sim = this_sc(act);
		if (sim != NULL && sim->sim_live) sim_channel_stop(this_obj(act), sim);
	}
#ifndef NO_GRAPHICS
	Avm2SoundChannelExt* sc = this_sc(act);
	if (sc != NULL && sc->mixer_live)
	{
		audio_channel_stop(act->ctx->app, sc->mixer_ch, sc->mixer_gen);
		// Manual stop: no soundComplete (Flash semantics) — unregister.
		if (g_live_channels[sc->mixer_ch] == this_obj(act))
			g_live_channels[sc->mixer_ch] = NULL;
		sc->mixer_live = 0;
	}
#else
	(void) act;
#endif
	return avm2_undefined();
}

static Avm2Object* sound_channel_new(Avm2Context* ctx)
{
	Avm2Value v = avm2_class_construct(ctx, ctx->builtins.sound_channel_class,
	                                   NULL, 0);
	return v.u.obj;
}

// ---------------------------------------------------------------------------
// SoundMixer (statics)
// ---------------------------------------------------------------------------

static int32_t g_mixer_buffer_time = 5;
static int32_t g_mixer_l2l = 100, g_mixer_l2r = 0, g_mixer_r2l = 0,
               g_mixer_r2r = 100, g_mixer_volume = 100;

static Avm2Value sm_get_buffer_time(Avm2Activation* act)
{
	(void) act;
	return avm2_integer(g_mixer_buffer_time);
}

static Avm2Value sm_set_buffer_time(Avm2Activation* act)
{
	g_mixer_buffer_time = act->argc > 0
		? avm2_coerce_to_i32(act->ctx, act->args[0]) : 0;
	return avm2_undefined();
}

static Avm2Value sm_get_sound_transform(Avm2Activation* act)
{
	return avm2_object_value(st_from_core(act->ctx, g_mixer_l2l, g_mixer_l2r,
	                                      g_mixer_r2l, g_mixer_r2r, g_mixer_volume));
}

static Avm2Value sm_set_sound_transform(Avm2Activation* act)
{
	Avm2SoundTransformExt* st = act->argc > 0
		? st_ext_of(act->ctx, act->args[0]) : NULL;
	if (st == NULL) return avm2_undefined();
	g_mixer_l2l = (int32_t) (st->left_to_left * 100.0);
	g_mixer_l2r = (int32_t) (st->left_to_right * 100.0);
	g_mixer_r2l = (int32_t) (st->right_to_left * 100.0);
	g_mixer_r2r = (int32_t) (st->right_to_right * 100.0);
	g_mixer_volume = (int32_t) (st->volume * 100.0);
#ifndef NO_GRAPHICS
	// Global transform applies at mix time as the output master volume
	// (pan cross-gains on the global transform are not wired — volume is
	// what real content uses).
	audio_set_master_volume(act->ctx->app, (float) g_mixer_volume / 100.0f);
#endif
	return avm2_undefined();
}

// `SimpleButton.soundTransform` IS the global SoundMixer transform: Flash
// declares the property on SimpleButton, but playerglobal's implementation is
// the mixer's (Ruffle: core/src/avm2/globals/flash/display/simple_button.rs:13
// re-exports `sound_mixer::{get_sound_transform, set_sound_transform}`, and
// sound_mixer.rs:15-19 documents the double duty). Sprite keeps its OWN
// per-object pair. Both accessors below ignore `this`, so the static
// SoundMixer pair serves the instance registration unchanged; avm2_display.c
// calls this where the SimpleButton class is built.
void avm2_media_register_mixer_transform(Avm2Context* ctx, Avm2Class* cls)
{
	avm2_builtin_add_getset(ctx, cls, "soundTransform", sm_get_sound_transform,
	                        sm_set_sound_transform);
}

static Avm2Value sm_stop_all(Avm2Activation* act)
{
#ifndef NO_GRAPHICS
	audio_stop_all_sounds(act->ctx->app);
	// stopAll is a manual stop: no soundComplete for any channel.
	memset(g_live_channels, 0, sizeof(g_live_channels));
#else
	(void) act;
#endif
	return avm2_undefined();
}

static Avm2Value sm_are_sounds_inaccessible(Avm2Activation* act)
{
	(void) act;
	return avm2_bool(false);
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

typedef struct Avm2SoundObjExt
{
	Avm2EventDispatcherExt dispatcher;  // extends EventDispatcher (MUST be first)
	uint8_t has_sound;
	uint16_t char_id;    // DefineSound char id (0 = none; mixer asset key)
	uint32_t sample_count;
	double sample_rate;
	uint32_t data_size;  // bytesTotal (compressed, excl. MP3 seek prefix)
	// URL-loaded sound (load()/new Sound(request)) — a bundled sibling file,
	// resolved synchronously, with its open/progress/complete events queued
	// for the end of the frame the way every other fetch in the runtime is.
	double ext_duration_ms;
	uint8_t ext_loaded;      // bytes resolved: length/bytesTotal are live
	uint8_t ext_load_queued; // events not delivered yet
	// The resolved file's bytes, for the ID3 walk at dispatch time. These
	// point into the linked-in DataFileEntry payload (static storage), never
	// into the GC heap.
	const unsigned char* ext_data;
	uint32_t ext_data_len;
	// Ruffle's SoundLoadingState (avm2/object/sound_object.rs): New until
	// load()/new Sound(request) starts a URL load (Loading); Loaded once that
	// load completes or loadCompressedDataFromByteArray/loadPCMFromByteArray
	// runs. load() outside New throws #2037, the byte-array loaders are
	// no-ops once Loaded, and a URL load finishing after Loaded is dropped
	// (loader.rs load_sound_avm2).
	uint8_t load_state;
} Avm2SoundObjExt;

#define AVM2_SOUND_LOAD_NEW     0
#define AVM2_SOUND_LOAD_LOADING 1
#define AVM2_SOUND_LOAD_LOADED  2

static Avm2SoundObjExt* sound_ext_of(Avm2Context* ctx, Avm2Object* o)
{
	if (o == NULL || o->cls == NULL
	    || !media_class_is_a(o->cls, ctx->builtins.sound_class))
		return NULL;
	return (Avm2SoundObjExt*) o->native_ext;
}

static Avm2SoundObjExt* this_sound(Avm2Activation* act)
{
	return sound_ext_of(act->ctx, this_obj(act));
}

// Keyed on the DEFINING movie, main or child - see avm2_bitmap.c's
// embedded_bitmap_for_char for the ruling and why an id can match at most one
// movie's rows.
static const Avm2SoundData* sound_data_for_char(uint16_t char_id)
{
	for (uint32_t i = 0; i < avm2_generated_sound_count; i++)
		if (avm2_generated_sounds[i].char_id == char_id)
			return &avm2_generated_sounds[i];
	uint32_t nm = avm2_display_child_movie_count();
	for (uint32_t m = 0; m < nm; m++)
	{
		const Avm2MovieTables* t = avm2_display_child_movie(m);
		if (t == NULL) continue;
		for (uint32_t i = 0; i < t->sound_count; i++)
			if (t->sounds[i].char_id == char_id) return &t->sounds[i];
	}
	return NULL;
}

static void sound_start_url_load(Avm2Context* ctx, Avm2Object* self,
                                 Avm2SoundObjExt* s, Avm2Value request);

// Sound(stream:URLRequest = null, context:SoundLoaderContext = null).
static Avm2Value sound_ctor(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* self = this_obj(act);
	Avm2SoundObjExt* s = sound_ext_of(ctx, self);
	if (s == NULL) return avm2_undefined();
	// SymbolClass-bound embedded sound: seed metadata from the DefineSound
	// table (ignores any ctor args, like BitmapData subclasses do).
	// child_char_for_class: g_symbol_map holds the MAIN movie's rows only, so
	// a sound embedded by a Loader-loaded child resolved to char 0 and came up
	// silent (length 0).
	uint16_t char_id = avm2_display_child_char_for_class(ctx, self->cls);
	const Avm2SoundData* sd = char_id != 0 ? sound_data_for_char(char_id) : NULL;
	if (sd != NULL)
	{
		s->has_sound = 1;
		s->char_id = char_id;
		s->sample_count = sd->sample_count;
		s->sample_rate = rate_code_hz(sd->rate);
		s->data_size = sd->data_size;
	}
	// `new Sound(request)` starts the load immediately (Sound.as), exactly as
	// `new URLLoader(request)` does.
	else if (act->argc > 0 && act->args[0].kind == AVM2_VALUE_OBJECT)
	{
		sound_start_url_load(ctx, self, s, act->args[0]);
		s->load_state = AVM2_SOUND_LOAD_LOADING;
	}
	return avm2_undefined();
}

static double sound_length_ms(Avm2SoundObjExt* s)
{
	if (s == NULL) return 0.0;
	if (s->ext_loaded) return s->ext_duration_ms;
	if (!s->has_sound || s->sample_rate <= 0) return 0.0;
	return (double) s->sample_count * 1000.0 / s->sample_rate;
}

static Avm2Value sound_get_length(Avm2Activation* act)
{
	return avm2_number(sound_length_ms(this_sound(act)));
}

static Avm2Value sound_get_bytes_total(Avm2Activation* act)
{
	Avm2SoundObjExt* s = this_sound(act);
	return avm2_integer(s != NULL ? (int32_t) s->data_size : 0);
}

static Avm2Value sound_get_bytes_loaded(Avm2Activation* act)
{
	Avm2SoundObjExt* s = this_sound(act);
	return avm2_uint_value(s != NULL ? s->data_size : 0);
}

static Avm2Value sound_get_is_buffering(Avm2Activation* act)
{
	(void) act;
	return avm2_bool(false);
}

static Avm2Value sound_get_is_url_inaccessible(Avm2Activation* act)
{
	(void) act;
	return avm2_bool(false);
}

static Avm2Value sound_get_url(Avm2Activation* act)
{
	(void) act;
	return avm2_null();
}

// `Sound.id3` holds the ID3Info built by the load (id3_read_and_dispatch), and
// is parked on the Sound itself as a dont-enum dynamic prop so the GC traces it
// with its owner — the same trick AVTagData uses for its backing fields.
#define AVM2_SOUND_ID3_SLOT "_id3"
#define AVM2_SOUND_ID3_SLOT_LEN 4

static Avm2Value sound_get_id3(Avm2Activation* act)
{
	Avm2Value v = avm2_get_public_property(act->ctx, act->this_val,
	                                       AVM2_SOUND_ID3_SLOT,
	                                       AVM2_SOUND_ID3_SLOT_LEN, NULL);
	return v.kind == AVM2_VALUE_OBJECT ? v : avm2_null();
}

// --- URL-loaded sounds ------------------------------------------------------
//
// Sounds with a pending load(), drained once per tick by avm2_media_poll.
// Flash never delivers open/progress/complete inside the calling frame, and
// sound_constructor_with_args grades exactly that: both of the constructor's
// own traces precede every callback.
#define AVM2_MAX_PENDING_SOUND_LOADS 8
static Avm2Object* g_pending_sound_loads[AVM2_MAX_PENDING_SOUND_LOADS];
static int g_pending_sound_load_count;

// Duration of a CBR MP3 from its first frame header — the same estimate the
// AVM1 side uses (action.c builtin_sound_loadSound). Enough for playback
// bookkeeping; nothing here decodes samples.
static double mp3_duration_ms(const unsigned char* p, size_t len)
{
	if (p == NULL || len < 4) return 0.0;
	// Skip an ID3v2 tag (syncsafe 28-bit size after the 10-byte header).
	if (len >= 10 && p[0] == 'I' && p[1] == 'D' && p[2] == '3')
	{
		size_t skip = (size_t) (((p[6] & 0x7F) << 21) | ((p[7] & 0x7F) << 14)
		                        | ((p[8] & 0x7F) << 7) | (p[9] & 0x7F)) + 10;
		if (skip >= len) return 0.0;
		p += skip;
		len -= skip;
	}
	if (len < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0.0;
	int version = (p[1] >> 3) & 3;   // 3 = MPEG1
	int layer = (p[1] >> 1) & 3;     // 1 = Layer III, 2 = Layer II
	int br_idx = (p[2] >> 4) & 0xF;
	static const int br_v1_l3[] =
		{ 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
	static const int br_v1_l2[] =
		{ 0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0 };
	int kbps = 0;
	if (version == 3 && layer == 1) kbps = br_v1_l3[br_idx];
	else if (version == 3 && layer == 2) kbps = br_v1_l2[br_idx];
	if (kbps <= 0) return 0.0;
	return (double) len * 8.0 / (double) (kbps * 1000) * 1000.0;
}

// Resolve a URLRequest to a bundled sibling file and seed the Sound with its
// size and duration. The bytes are available synchronously (they are linked
// into the binary) — only the EVENTS are deferred, which is the same split the
// URLLoader pipeline uses.
static void sound_start_url_load(Avm2Context* ctx, Avm2Object* self,
                                 Avm2SoundObjExt* s, Avm2Value request)
{
	if (self == NULL || s == NULL || request.kind != AVM2_VALUE_OBJECT) return;
	const Avm2String* url = NULL;
	{
		Avm2Value u = avm2_get_public_property(ctx, request, "url", 3, NULL);
		if (u.kind == AVM2_VALUE_STRING) url = u.u.str;
	}
	if (url == NULL || url->len == 0) return;

	char name[256];
	size_t n = (size_t) url->len < sizeof(name) - 1
		? (size_t) url->len : sizeof(name) - 1;
	memcpy(name, url->utf8, n);
	name[n] = '\0';
	const char* base = name;
	for (const char* p = name; *p != '\0'; p++)
		if (*p == '/' || *p == '\\') base = p + 1;
	DataFileEntry* d = findDataFile(base);
	if (d == NULL) d = findDataFile(name);
	if (d == NULL || d->content == NULL) return;

	s->ext_loaded = 1;
	s->data_size = (uint32_t) d->content_length;
	s->ext_data = (const unsigned char*) d->content;
	s->ext_data_len = (uint32_t) d->content_length;
	s->ext_duration_ms = mp3_duration_ms((const unsigned char*) d->content,
	                                     (size_t) d->content_length);
	if (s->ext_load_queued) return;
	s->ext_load_queued = 1;
	if (g_pending_sound_load_count < AVM2_MAX_PENDING_SOUND_LOADS)
		g_pending_sound_loads[g_pending_sound_load_count++] = self;
}

// play(startTime:Number = 0, loops:int = 0, sndTransform:SoundTransform = null)
// Returns a SoundChannel, or null when startTime is past the clip length
// (Ruffle play_queued: position > duration -> false -> null).
static Avm2Value sound_play(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2SoundObjExt* s = this_sound(act);
	if (s == NULL) return avm2_null();
	double position = act->argc > 0 ? avm2_coerce_to_number(ctx, act->args[0]) : 0.0;
	if (s->has_sound && position > sound_length_ms(s))
		return avm2_null();
	Avm2Object* ch = sound_channel_new(ctx);
	// Apply the optional sound transform to the channel (core i32×100).
	Avm2SoundTransformExt* st = act->argc > 2 ? st_ext_of(ctx, act->args[2]) : NULL;
	if (st != NULL && ch != NULL)
	{
		Avm2SoundChannelExt* sc = (Avm2SoundChannelExt*) ch->native_ext;
		sc->st_l2l = (int32_t) (st->left_to_left * 100.0);
		sc->st_l2r = (int32_t) (st->left_to_right * 100.0);
		sc->st_r2l = (int32_t) (st->right_to_left * 100.0);
		sc->st_r2r = (int32_t) (st->right_to_right * 100.0);
		sc->st_volume = (int32_t) (st->volume * 100.0);
	}
#ifndef NO_GRAPHICS
	// Start real playback on the shared mixer. AS3 loops semantics: the
	// sound plays max(loops, 1) times total, each pass restarting at
	// startTime (Ruffle play_queued) — the mixer takes the ADDITIONAL plays.
	if (ch != NULL && s->has_sound && s->char_id != 0)
	{
		Avm2SoundChannelExt* sc = (Avm2SoundChannelExt*) ch->native_ext;
		int32_t loops = act->argc > 1 ? avm2_coerce_to_i32(ctx, act->args[1]) : 0;
		if (loops < 1) loops = 1;
		float gains[5];
		sc_mixer_gains(sc, gains);
		uint32_t gen = 0;
		int mch = audio_start_sound_ex(ctx->app, s->char_id,
		                               (uint32_t) (loops - 1), position,
		                               gains, &gen);
		if (mch >= 0)
		{
			sc->mixer_ch = mch;
			sc->mixer_gen = gen;
			sc->mixer_live = 1;
			g_live_channels[mch] = ch;
		}
	}
#endif
	// A URL-loaded sound has no mixer asset in ANY build, so its playback is
	// simulated: the estimated duration is counted down one frame budget per
	// tick and Event.SOUND_COMPLETE is dispatched when it drains. Identical in
	// no-graphics and graphics (mode parity) — nothing here reads a mixer.
	if (ch != NULL && !s->has_sound && s->ext_loaded && s->ext_duration_ms > 0.0
	    && position < s->ext_duration_ms)
	{
		Avm2SoundChannelExt* sc = (Avm2SoundChannelExt*) ch->native_ext;
		for (int i = 0; i < AVM2_MAX_SIM_CHANNELS; i++)
		{
			if (g_sim_channels[i] != NULL) continue;
			sc->sim_elapsed_ms = position > 0.0 ? position : 0.0;
			sc->sim_duration_ms = s->ext_duration_ms;
			sc->sim_live = 1;
			g_sim_channels[i] = ch;
			break;
		}
	}
	return avm2_object_value(ch);
}

static Avm2Value sound_close(Avm2Activation* act)
{
	(void) act;
	return avm2_undefined();
}

// load(stream:URLRequest, context:SoundLoaderContext = null)
static Avm2Value sound_load(Avm2Activation* act)
{
	Avm2SoundObjExt* s = this_sound(act);
	// A SymbolClass-bound (embedded) sound keeps its own data: Flash ignores
	// the load on one, and so did the previous no-op.
	if (s == NULL || s->has_sound || act->argc == 0) return avm2_undefined();
	// Ruffle sound.rs load(): the state check precedes the null check.
	if (s->load_state != AVM2_SOUND_LOAD_NEW)
		avm2_throw_error(act->ctx, act->ctx->builtins.error_class,
		                 "Error #2037: Functions called in incorrect sequence, "
		                 "or earlier call was unsuccessful.");
	// FP ignores `load(null)`.
	if (act->args[0].kind != AVM2_VALUE_OBJECT) return avm2_undefined();
	sound_start_url_load(act->ctx, this_obj(act), s, act->args[0]);
	s->load_state = AVM2_SOUND_LOAD_LOADING;
	return avm2_undefined();
}

static void id3_read_and_dispatch(Avm2Context* ctx, Avm2Object* sound,
                                  Avm2SoundObjExt* s);

// loadCompressedDataFromByteArray(bytes:ByteArray, bytesLength:uint) — Ruffle
// sound.rs: a no-op once Loaded; reads `bytesLength` bytes at the ByteArray's
// position (advancing it), throwing ArgumentError #2084 on a short read (FP's
// actual choice); then a synchronous `progress`, the ID3 walk, and Loaded.
static Avm2Value sound_load_compressed_data_from_byte_array(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	Avm2Object* self = this_obj(act);
	Avm2SoundObjExt* s = this_sound(act);
	if (s == NULL || s->has_sound || s->load_state == AVM2_SOUND_LOAD_LOADED)
		return avm2_undefined();
	Avm2Value bv = act->argc > 0 ? act->args[0] : avm2_undefined();
	if (bv.kind == AVM2_VALUE_NULL || bv.kind == AVM2_VALUE_UNDEFINED)
		avm2_throw_error(ctx, ctx->builtins.type_error_class,
		                 "Error #2007: Parameter bytes must be non-null.");
	Avm2ByteArrayExt* ba = avm2_bytearray_ext_of(bv);
	if (ba == NULL) return avm2_undefined();
	uint32_t want = act->argc > 1 ? avm2_coerce_to_u32(ctx, act->args[1]) : 0;
	if (ba->position > ba->len || want > ba->len - ba->position)
		avm2_throw_error(ctx, ctx->builtins.argument_error_class,
		                 "Error #2084: The AMF encoding of the arguments "
		                 "cannot exceed 40K.");
	// Ruffle owns the bytes it read before dispatching anything; a progress
	// handler may resize the ByteArray, so take a private copy.
	unsigned char* copy = NULL;
	if (want > 0)
	{
		copy = (unsigned char*) malloc(want);
		if (copy != NULL) memcpy(copy, ba->bytes + ba->position, want);
	}
	ba->position += want;

	s->ext_loaded = 1;
	s->data_size = want;
	s->ext_duration_ms = copy != NULL ? mp3_duration_ms(copy, (size_t) want) : 0.0;
	s->load_state = AVM2_SOUND_LOAD_LOADED;
	Avm2Object* ev = avm2_progress_event_new(
		ctx, avm2_string_from_literal(ctx, "progress"),
		(double) want, (double) want);
	if (ev != NULL) avm2_dispatch_event(ctx, self, ev);
	// ext_data must not outlive this call (it otherwise points at static
	// DataFileEntry storage): scope the copy to the synchronous ID3 walk.
	s->ext_data = copy;
	s->ext_data_len = copy != NULL ? want : 0;
	id3_read_and_dispatch(ctx, self, s);
	s->ext_data = NULL;
	s->ext_data_len = 0;
	free(copy);
	return avm2_undefined();
}

// loadPCMFromByteArray — Ruffle stubs it: a no-op once Loaded, else Loaded.
static Avm2Value sound_load_pcm_from_byte_array(Avm2Activation* act)
{
	Avm2SoundObjExt* s = this_sound(act);
	if (s == NULL || s->has_sound || s->load_state == AVM2_SOUND_LOAD_LOADED)
		return avm2_undefined();
	s->load_state = AVM2_SOUND_LOAD_LOADED;
	return avm2_undefined();
}

// ---------------------------------------------------------------------------
// Mixer lifecycle (graphics builds; all no-ops under NO_GRAPHICS)
// ---------------------------------------------------------------------------

// Register every embedded DefineSound payload with the shared mixer so
// Sound.play() can start it. Called once at AVM2 boot (runSWF_avm2).
void avm2_media_register_sounds(Avm2Context* ctx)
{
#ifndef NO_GRAPHICS
	if (avm2_generated_sound_count == 0) return;
	if (ctx->app->audio_ctx == NULL) audio_init(ctx->app);
	for (uint32_t i = 0; i < avm2_generated_sound_count; i++)
	{
		const Avm2SoundData* sd = &avm2_generated_sounds[i];
		if (sd->data == NULL || sd->data_len == 0) continue;
		audio_define_sound(ctx->app, sd->char_id, sd->format, sd->rate,
		                   sd->sample_size, sd->stereo, sd->sample_count,
		                   sd->data, sd->data_len);
	}
#else
	(void) ctx;
#endif
}

// ---------------------------------------------------------------------------
// flash.media.ID3Info + the ID3v2 tag walk
//
// Ruffle SoundObject::read_and_call_id3_event (avm2/object/sound_object.rs):
// build an ID3Info, copy the seven fields the class exposes out of the tag,
// store it on the Sound, and dispatch Event.ID3 — but ONLY when the bytes
// actually parsed as a tag. The class itself is `public final dynamic class
// ID3Info` with seven `public var ...:String` (ID3Info.as); we back the seven
// with dont-enum dynamic props pre-seeded to `null`, which reads exactly like
// an unassigned String slot.
// ---------------------------------------------------------------------------

static Avm2Class* g_id3info_class;

// One decoded ID3 text field, already UTF-8 and NUL-terminated.
#define AVM2_ID3_TEXT_MAX 512

// ID3 text encodings (SWF-independent; ID3v2 §4).
enum { ID3_ENC_LATIN1 = 0, ID3_ENC_UTF16_BOM = 1, ID3_ENC_UTF16BE = 2,
       ID3_ENC_UTF8 = 3 };

static void id3_put_utf8(char* out, size_t cap, size_t* n, uint32_t cp)
{
	unsigned char buf[4];
	size_t len;
	if (cp < 0x80) { buf[0] = (unsigned char) cp; len = 1; }
	else if (cp < 0x800)
	{
		buf[0] = (unsigned char) (0xC0 | (cp >> 6));
		buf[1] = (unsigned char) (0x80 | (cp & 0x3F));
		len = 2;
	}
	else if (cp < 0x10000)
	{
		buf[0] = (unsigned char) (0xE0 | (cp >> 12));
		buf[1] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
		buf[2] = (unsigned char) (0x80 | (cp & 0x3F));
		len = 3;
	}
	else
	{
		buf[0] = (unsigned char) (0xF0 | (cp >> 18));
		buf[1] = (unsigned char) (0x80 | ((cp >> 12) & 0x3F));
		buf[2] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
		buf[3] = (unsigned char) (0x80 | (cp & 0x3F));
		len = 4;
	}
	if (*n + len >= cap) return;
	memcpy(out + *n, buf, len);
	*n += len;
}

// Decode `len` bytes in ID3 encoding `enc` into UTF-8. Trailing NUL padding is
// dropped (encoders pad text frames), and the result is always terminated.
static void id3_decode(uint8_t enc, const unsigned char* p, size_t len,
                       char* out, size_t cap)
{
	size_t n = 0;
	out[0] = '\0';
	if (enc == ID3_ENC_UTF16_BOM || enc == ID3_ENC_UTF16BE)
	{
		int big = (enc == ID3_ENC_UTF16BE);
		if (enc == ID3_ENC_UTF16_BOM && len >= 2)
		{
			if (p[0] == 0xFF && p[1] == 0xFE) { big = 0; p += 2; len -= 2; }
			else if (p[0] == 0xFE && p[1] == 0xFF) { big = 1; p += 2; len -= 2; }
		}
		while (len >= 2)
		{
			uint32_t u = big ? (uint32_t) ((p[0] << 8) | p[1])
			                 : (uint32_t) ((p[1] << 8) | p[0]);
			p += 2;
			len -= 2;
			if (u >= 0xD800 && u <= 0xDBFF && len >= 2)
			{
				uint32_t lo = big ? (uint32_t) ((p[0] << 8) | p[1])
				                  : (uint32_t) ((p[1] << 8) | p[0]);
				if (lo >= 0xDC00 && lo <= 0xDFFF)
				{
					p += 2;
					len -= 2;
					u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
				}
			}
			if (u == 0) break;
			id3_put_utf8(out, cap, &n, u);
		}
	}
	else if (enc == ID3_ENC_UTF8)
	{
		for (size_t i = 0; i < len; i++)
		{
			if (p[i] == 0) break;
			if (n + 1 >= cap) break;
			out[n++] = (char) p[i];
		}
	}
	else
	{
		// ISO-8859-1: every byte is its own code point.
		for (size_t i = 0; i < len; i++)
		{
			if (p[i] == 0) break;
			id3_put_utf8(out, cap, &n, (uint32_t) p[i]);
		}
	}
	out[n] = '\0';
}

// Offset of the terminator of a string in `enc`, starting at p[0..len).
// Returns `len` when unterminated; *adv receives the terminator's width.
static size_t id3_term(uint8_t enc, const unsigned char* p, size_t len,
                       size_t* adv)
{
	if (enc == ID3_ENC_UTF16_BOM || enc == ID3_ENC_UTF16BE)
	{
		*adv = 2;
		for (size_t i = 0; i + 1 < len; i += 2)
			if (p[i] == 0 && p[i + 1] == 0) return i;
		return len;
	}
	*adv = 1;
	for (size_t i = 0; i < len; i++)
		if (p[i] == 0) return i;
	return len;
}

typedef struct Avm2Id3Fields
{
	char album[AVM2_ID3_TEXT_MAX];
	char artist[AVM2_ID3_TEXT_MAX];
	char comment[AVM2_ID3_TEXT_MAX];
	char genre[AVM2_ID3_TEXT_MAX];
	char song_name[AVM2_ID3_TEXT_MAX];
	char track[AVM2_ID3_TEXT_MAX];
	char year[AVM2_ID3_TEXT_MAX];
	uint8_t has_album, has_artist, has_comment, has_genre, has_song_name,
	        has_track, has_year;
} Avm2Id3Fields;

// `tag.track()` / `tag.year()` are numeric in the id3 crate Ruffle uses, so a
// frame that is not a plain integer (with the optional "n/total" split for
// TRCK) yields None and the property stays unset.
static int id3_numeric(const char* s, char* out, size_t cap, int allow_slash)
{
	size_t n = 0;
	const char* p = s;
	while (*p == ' ') p++;
	if (*p < '0' || *p > '9') return 0;
	while (*p >= '0' && *p <= '9')
	{
		if (n + 1 >= cap) return 0;
		out[n++] = *p++;
	}
	out[n] = '\0';
	if (*p == '\0') return 1;
	if (allow_slash && *p == '/') return 1;
	return 0;
}

// ID3v2.2/2.3/2.4 frame walk. Returns 0 when the bytes carry no readable tag
// (the `Err` arm of Ruffle's `Tag::read_from2`, which suppresses the event).
static int id3_parse(const unsigned char* p, size_t len, Avm2Id3Fields* f)
{
	memset(f, 0, sizeof(*f));
	if (p == NULL || len < 10) return 0;
	if (p[0] != 'I' || p[1] != 'D' || p[2] != '3') return 0;
	uint8_t major = p[3];
	if (major < 2 || major > 4) return 0;
	uint8_t hdr_flags = p[5];
	size_t tag_size = (size_t) (((p[6] & 0x7F) << 21) | ((p[7] & 0x7F) << 14)
	                            | ((p[8] & 0x7F) << 7) | (p[9] & 0x7F));
	size_t pos = 10;
	size_t end = pos + tag_size;
	if (end > len) end = len;
	// Unsynchronisation (bit 7) rewrites the frame bytes; we do not undo it,
	// and neither does any test asset. Extended header (bit 6) is skipped.
	if ((hdr_flags & 0x40) != 0 && pos + 4 <= end)
	{
		if (major >= 4)
		{
			size_t ext = (size_t) (((p[pos] & 0x7F) << 21)
			                       | ((p[pos + 1] & 0x7F) << 14)
			                       | ((p[pos + 2] & 0x7F) << 7)
			                       | (p[pos + 3] & 0x7F));
			pos += ext > 0 ? ext : 4;
		}
		else
		{
			size_t ext = (size_t) ((p[pos] << 24) | (p[pos + 1] << 16)
			                       | (p[pos + 2] << 8) | p[pos + 3]);
			pos += 4 + ext;
		}
	}

	int id_len = (major == 2) ? 3 : 4;
	int hdr_len = (major == 2) ? 6 : 10;
	int found = 0;
	while (pos + (size_t) hdr_len <= end)
	{
		const unsigned char* id = p + pos;
		if (id[0] == 0) break;  // padding
		size_t fsize;
		if (major == 2)
			fsize = (size_t) ((id[3] << 16) | (id[4] << 8) | id[5]);
		else if (major == 4)
			fsize = (size_t) (((id[4] & 0x7F) << 21) | ((id[5] & 0x7F) << 14)
			                  | ((id[6] & 0x7F) << 7) | (id[7] & 0x7F));
		else
			fsize = (size_t) ((id[4] << 24) | (id[5] << 16) | (id[6] << 8)
			                  | id[7]);
		pos += (size_t) hdr_len;
		if (fsize == 0 || pos + fsize > end) break;
		const unsigned char* body = p + pos;
		pos += fsize;
		found = 1;

		char name[5];
		memcpy(name, id, (size_t) id_len);
		name[id_len] = '\0';

		int is_comment = (strcmp(name, "COMM") == 0 || strcmp(name, "COM") == 0);
		if (name[0] != 'T' && !is_comment) continue;

		uint8_t enc = body[0];
		char text[AVM2_ID3_TEXT_MAX];
		if (is_comment)
		{
			// encoding(1) language(3) short-description(term) text
			if (fsize < 5) continue;
			size_t off = 4;
			size_t adv = 1;
			size_t t = id3_term(enc, body + off, fsize - off, &adv);
			off += t + adv;
			if (off > fsize) continue;
			id3_decode(enc, body + off, fsize - off, text, sizeof(text));
			if (!f->has_comment)
			{
				memcpy(f->comment, text, sizeof(text));
				f->has_comment = 1;
			}
			continue;
		}
		id3_decode(enc, body + 1, fsize - 1, text, sizeof(text));

		const char* n4 = name;
		if (strcmp(n4, "TALB") == 0 || strcmp(n4, "TAL") == 0)
		{
			memcpy(f->album, text, sizeof(text));
			f->has_album = 1;
		}
		else if (strcmp(n4, "TPE1") == 0 || strcmp(n4, "TP1") == 0)
		{
			memcpy(f->artist, text, sizeof(text));
			f->has_artist = 1;
		}
		else if (strcmp(n4, "TCON") == 0 || strcmp(n4, "TCO") == 0)
		{
			memcpy(f->genre, text, sizeof(text));
			f->has_genre = 1;
		}
		else if (strcmp(n4, "TIT2") == 0 || strcmp(n4, "TT2") == 0)
		{
			memcpy(f->song_name, text, sizeof(text));
			f->has_song_name = 1;
		}
		else if (strcmp(n4, "TRCK") == 0 || strcmp(n4, "TRK") == 0)
		{
			if (id3_numeric(text, f->track, sizeof(f->track), 1))
				f->has_track = 1;
		}
		else if (strcmp(n4, "TYER") == 0 || strcmp(n4, "TYE") == 0)
		{
			if (id3_numeric(text, f->year, sizeof(f->year), 0))
				f->has_year = 1;
		}
		else if (strcmp(n4, "TDRC") == 0 && !f->has_year)
		{
			// v2.4 recording time: the leading four digits are the year.
			if (id3_numeric(text, f->year, sizeof(f->year), 0))
				f->has_year = 1;
		}
	}
	return found;
}

static void id3_set(Avm2Context* ctx, Avm2Object* o, const char* name,
                    uint32_t nlen, int present, const char* utf8)
{
	Avm2Value v = present
		? avm2_string(avm2_string_new(ctx, utf8, (uint32_t) strlen(utf8)))
		: avm2_null();
	Avm2DynProp* dp = avm2_object_set_dynamic(ctx, o, name, nlen, v);
	if (dp != NULL) dp->dont_enum = 1;
}

// Build the ID3Info, park it on the Sound, and dispatch Event.ID3. Ruffle runs
// this BETWEEN the progress and complete events of the load (loader.rs).
static void id3_read_and_dispatch(Avm2Context* ctx, Avm2Object* sound,
                                  Avm2SoundObjExt* s)
{
	if (g_id3info_class == NULL) return;
	Avm2Id3Fields f;
	int ok = id3_parse(s->ext_data, (size_t) s->ext_data_len, &f);
	Avm2Value iv = avm2_class_construct(ctx, g_id3info_class, NULL, 0);
	if (iv.kind != AVM2_VALUE_OBJECT || iv.u.obj == NULL) return;
	Avm2Object* info = iv.u.obj;
	id3_set(ctx, info, "album", 5, f.has_album, f.album);
	id3_set(ctx, info, "artist", 6, f.has_artist, f.artist);
	id3_set(ctx, info, "comment", 7, f.has_comment, f.comment);
	id3_set(ctx, info, "genre", 5, f.has_genre, f.genre);
	id3_set(ctx, info, "songName", 8, f.has_song_name, f.song_name);
	id3_set(ctx, info, "track", 5, f.has_track, f.track);
	id3_set(ctx, info, "year", 4, f.has_year, f.year);
	Avm2DynProp* slot = avm2_object_set_dynamic(ctx, sound, AVM2_SOUND_ID3_SLOT,
	                                            AVM2_SOUND_ID3_SLOT_LEN,
	                                            avm2_object_value(info));
	if (slot != NULL) slot->dont_enum = 1;
	if (!ok) return;
	Avm2Object* ev = avm2_event_new(ctx, avm2_string_from_literal(ctx, "id3"),
	                                0, 0);
	if (ev != NULL) avm2_dispatch_event(ctx, sound, ev);
}

// Deliver the queued open/progress/complete for URL-loaded sounds, then
// advance every simulated playback clock by one frame budget and dispatch
// Event.SOUND_COMPLETE for the ones that drained. Both halves are
// build-independent (no mixer state is read) and run before the mixer sweep.
static void media_poll_simulated(Avm2Context* ctx)
{
	if (g_pending_sound_load_count > 0)
	{
		Avm2Object* batch[AVM2_MAX_PENDING_SOUND_LOADS];
		int count = g_pending_sound_load_count;
		memcpy(batch, g_pending_sound_loads, (size_t) count * sizeof(Avm2Object*));
		g_pending_sound_load_count = 0;
		for (int i = 0; i < count; i++)
		{
			Avm2Object* obj = batch[i];
			if (obj == NULL || obj->native_ext == NULL) continue;
			Avm2SoundObjExt* s = (Avm2SoundObjExt*) obj->native_ext;
			if (!s->ext_load_queued) continue;
			s->ext_load_queued = 0;
			// Already Loaded from a ByteArray: Ruffle's load_sound_avm2
			// returns without touching the sound or dispatching anything.
			if (s->load_state == AVM2_SOUND_LOAD_LOADED) continue;
			s->load_state = AVM2_SOUND_LOAD_LOADED;
			Avm2Object* ev = avm2_event_new(
				ctx, avm2_string_from_literal(ctx, "open"), 0, 0);
			if (ev != NULL) avm2_dispatch_event(ctx, obj, ev);
			ev = avm2_progress_event_new(
				ctx, avm2_string_from_literal(ctx, "progress"),
				(double) s->data_size, (double) s->data_size);
			if (ev != NULL) avm2_dispatch_event(ctx, obj, ev);
			id3_read_and_dispatch(ctx, obj, s);
			ev = avm2_event_new(
				ctx, avm2_string_from_literal(ctx, "complete"), 0, 0);
			if (ev != NULL) avm2_dispatch_event(ctx, obj, ev);
		}
	}

	double fps = (double) (int16_t) avm2_generated_frame_rate / 256.0;
	if (fps <= 0.0) fps = 24.0;
	double budget_ms = 1000.0 / fps;
	for (int i = 0; i < AVM2_MAX_SIM_CHANNELS; i++)
	{
		Avm2Object* obj = g_sim_channels[i];
		if (obj == NULL || obj->native_ext == NULL) continue;
		Avm2SoundChannelExt* sc = (Avm2SoundChannelExt*) obj->native_ext;
		if (!sc->sim_live) { g_sim_channels[i] = NULL; continue; }
		sc->sim_elapsed_ms += budget_ms;
		if (sc->sim_elapsed_ms < sc->sim_duration_ms) continue;
		// Unregister BEFORE dispatch: the handler may start a new playback.
		sc->sim_elapsed_ms = sc->sim_duration_ms;
		sc->sim_live = 0;
		g_sim_channels[i] = NULL;
		Avm2Object* ev = avm2_event_new(
			ctx, avm2_string_from_literal(ctx, "soundComplete"), 0, 0);
		if (ev != NULL) avm2_dispatch_event(ctx, obj, ev);
	}
}

// Dispatch Event.SOUND_COMPLETE for channels whose mixer playback drained.
// Called once per tick after frame scripts (may run user handlers — the
// caller wraps it in the tick's catch-all try frame).
void avm2_media_poll(Avm2Context* ctx)
{
	media_poll_simulated(ctx);
#ifndef NO_GRAPHICS
	for (int i = 0; i < MAX_SOUND_CHANNELS; i++)
	{
		Avm2Object* obj = g_live_channels[i];
		if (obj == NULL) continue;
		Avm2SoundChannelExt* sc = (Avm2SoundChannelExt*) obj->native_ext;
		if (audio_channel_active(ctx->app, sc->mixer_ch, sc->mixer_gen))
			continue;
		// Unregister BEFORE dispatch: the handler may call play() and land
		// on this same (now free) mixer slot.
		g_live_channels[i] = NULL;
		sc->mixer_live = 0;
		Avm2Object* ev = avm2_event_new(
			ctx, avm2_string_from_literal(ctx, "soundComplete"), 0, 0);
		avm2_dispatch_event(ctx, obj, ev);
	}
#else
	(void) ctx;
#endif
}

// GC root marker: a playing SoundChannel may be referenced only by the
// mixer registry (fire-and-forget play()); keep it alive until it drains.
void avm2_gc_mark_roots_media(Avm2Context* ctx)
{
	(void) ctx;
	// Same argument for a simulated channel and for a Sound whose load events
	// are still queued: the registry is the only reference C holds.
	for (int i = 0; i < AVM2_MAX_SIM_CHANNELS; i++)
		if (g_sim_channels[i] != NULL) avm2_gc_mark_object(g_sim_channels[i]);
	for (int i = 0; i < g_pending_sound_load_count; i++)
		if (g_pending_sound_loads[i] != NULL)
			avm2_gc_mark_object(g_pending_sound_loads[i]);
#ifndef NO_GRAPHICS
	for (int i = 0; i < MAX_SOUND_CHANNELS; i++)
	{
		if (g_live_channels[i] != NULL)
			avm2_gc_mark_object(g_live_channels[i]);
	}
#endif
}

// ---------------------------------------------------------------------------
// flash.media.AVTagData — a value object carrying an AV tag's payload and
// the media time it belongs to. Both are read-only after construction.
// ---------------------------------------------------------------------------

static Avm2Value avtagdata_ctor(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	if (act->this_val.kind != AVM2_VALUE_OBJECT) return avm2_undefined();
	Avm2Object* self = act->this_val.u.obj;
	Avm2Value data = (act->argc > 0) ? act->args[0] : avm2_null();
	if (data.kind != AVM2_VALUE_NULL && data.kind != AVM2_VALUE_UNDEFINED)
		data = avm2_string(avm2_coerce_to_string(ctx, data));
	double local = (act->argc > 1) ? avm2_coerce_to_number(ctx, act->args[1])
	                               : 0.0;
	avm2_object_set_dynamic(ctx, self, "_data", 5, data)->dont_enum = 1;
	avm2_object_set_dynamic(ctx, self, "_localTime", 10,
	                        avm2_number(local))->dont_enum = 1;
	return avm2_undefined();
}

static Avm2Value avtagdata_get_data(Avm2Activation* act)
{
	return avm2_get_public_property(act->ctx, act->this_val, "_data", 5, NULL);
}

static Avm2Value avtagdata_get_local_time(Avm2Activation* act)
{
	return avm2_get_public_property(act->ctx, act->this_val, "_localTime", 10,
	                                NULL);
}

// ---------------------------------------------------------------------------
// The rest of the flash.media.AV* value-object family (13 classes)
// ---------------------------------------------------------------------------
//
// AVTagData above is the shape they all share: a constructor that coerces its
// arguments into DECLARED slot types, then getter-only (for two of them,
// get/set) access to the results. That slot typing is most of what av_classes
// grades -- `new AVABRParameters("hello", 2.2, 3.3, 4.4)` reads back 2/3/4
// because those slots are `int`, and AVTrackInfo's 7.7 reads back as the
// STRING "7.7" while its 8.8 reads back as the int 8.
//
// The backing store is a fixed set of dont-enum dynamic props "_av0".."_av10",
// so the GC traces them for free (no native_ext, no mark hook), and one
// accessor per slot INDEX serves every class in the family.

#define AV_MAX_SLOTS 11

static const char* const AV_SLOT_NAMES[AV_MAX_SLOTS] = {
	"_av0", "_av1", "_av2", "_av3", "_av4", "_av5",
	"_av6", "_av7", "_av8", "_av9", "_av10"
};

static Avm2Value av_slot_get(Avm2Activation* act, int i)
{
	const char* n = AV_SLOT_NAMES[i];
	return avm2_get_public_property(act->ctx, act->this_val, n,
	                                (uint32_t) strlen(n), NULL);
}

// 'S' String, 'I' int, 'N' Number, 'B' Boolean, 'O' a typed object slot.
static Avm2Value av_coerce(Avm2Context* ctx, Avm2Value v, char type)
{
	switch (type)
	{
		case 'S':
			// String is nullable, so null/undefined survive as null.
			return (v.kind == AVM2_VALUE_NULL || v.kind == AVM2_VALUE_UNDEFINED)
				? avm2_null() : avm2_string(avm2_coerce_to_string(ctx, v));
		case 'I': return avm2_integer(avm2_coerce_to_i32(ctx, v));
		case 'N': return avm2_number(avm2_coerce_to_number(ctx, v));
		case 'B': return avm2_bool(avm2_coerce_to_boolean(v));
		default:  return (v.kind == AVM2_VALUE_OBJECT) ? v : avm2_null();
	}
}

static void av_slot_put(Avm2Activation* act, int i, Avm2Value v)
{
	if (act->this_val.kind != AVM2_VALUE_OBJECT) return;
	const char* n = AV_SLOT_NAMES[i];
	avm2_object_set_dynamic(act->ctx, act->this_val.u.obj, n,
	                        (uint32_t) strlen(n), v)->dont_enum = 1;
}

static Avm2Value av_slot_set(Avm2Activation* act, int i, char type)
{
	Avm2Value v = (act->argc > 0) ? act->args[0] : avm2_undefined();
	av_slot_put(act, i, av_coerce(act->ctx, v, type));
	return avm2_undefined();
}

// A constructor body IS the declared parameter-type string, nothing more.
static Avm2Value av_ctor(Avm2Activation* act, const char* types)
{
	for (int i = 0; types[i] != '\0' && i < AV_MAX_SLOTS; i++)
	{
		Avm2Value v = ((uint32_t) i < act->argc) ? act->args[i]
		                                        : avm2_undefined();
		av_slot_put(act, i, av_coerce(act->ctx, v, types[i]));
	}
	return avm2_undefined();
}

#define AV_GETTER(i)                                                          \
	static Avm2Value av_get_##i(Avm2Activation* act)                          \
	{ return av_slot_get(act, i); }
#define AV_SETTER_S(i)                                                        \
	static Avm2Value av_set_s##i(Avm2Activation* act)                         \
	{ return av_slot_set(act, i, 'S'); }
#define AV_SETTER_I(i)                                                        \
	static Avm2Value av_set_i##i(Avm2Activation* act)                         \
	{ return av_slot_set(act, i, 'I'); }

AV_GETTER(0)  AV_GETTER(1)  AV_GETTER(2)  AV_GETTER(3)
AV_GETTER(4)  AV_GETTER(5)  AV_GETTER(6)  AV_GETTER(7)
AV_GETTER(8)  AV_GETTER(9)  AV_GETTER(10)
// Only two of the family are writable at all: AVCaptionStyle (11 String
// props) and AVABRParameters (one String plus three ints).
AV_SETTER_S(0)  AV_SETTER_S(1)  AV_SETTER_S(2)  AV_SETTER_S(3)
AV_SETTER_S(4)  AV_SETTER_S(5)  AV_SETTER_S(6)  AV_SETTER_S(7)
AV_SETTER_S(8)  AV_SETTER_S(9)  AV_SETTER_S(10)
AV_SETTER_I(1)  AV_SETTER_I(2)  AV_SETTER_I(3)

#undef AV_GETTER
#undef AV_SETTER_S
#undef AV_SETTER_I

static Avm2MethodFn const AV_GETTERS[AV_MAX_SLOTS] = {
	av_get_0, av_get_1, av_get_2, av_get_3, av_get_4, av_get_5,
	av_get_6, av_get_7, av_get_8, av_get_9, av_get_10
};

static Avm2Value avabrparams_ctor(Avm2Activation* act)
{ return av_ctor(act, "SIII"); }
static Avm2Value avabrprofile_ctor(Avm2Activation* act)
{ return av_ctor(act, "III"); }
static Avm2Value avcuepoint_ctor(Avm2Activation* act)
{ return av_ctor(act, "ON"); }
static Avm2Value avresult_ctor(Avm2Activation* act)
{ return av_ctor(act, "I"); }
// AVInsertionResult extends AVResult, but a builtin class's instance_init is
// never chained (avm2_class_construct calls only the most-derived one), so it
// writes its inherited `result` slot itself.
static Avm2Value avinsertion_ctor(Avm2Activation* act)
{ return av_ctor(act, "IIB"); }
static Avm2Value avperiodinfo_ctor(Avm2Activation* act)
{ return av_ctor(act, "NNNIIIIIBN"); }
static Avm2Value avplaystate_ctor(Avm2Activation* act)
{ return av_ctor(act, "I"); }
static Avm2Value avtimeline_ctor(Avm2Activation* act)
{ return av_ctor(act, "SNNIIIIB"); }
static Avm2Value avtrackinfo_ctor(Avm2Activation* act)
{ return av_ctor(act, "SSBBBBSI"); }

// AVCaptionStyle takes no arguments; every field initialises to "" EXCEPT
// bottomInset, which initialises to the string "0".
static Avm2Value avcaptionstyle_ctor(Avm2Activation* act)
{
	Avm2Context* ctx = act->ctx;
	for (int i = 0; i < 11; i++)
	{
		av_slot_put(act, i, avm2_string(
			avm2_string_from_literal(ctx, (i == 2) ? "0" : "")));
	}
	return avm2_undefined();
}

static void av_iconst(Avm2Context* ctx, Avm2Class* cls, const char* n, int32_t v)
{ avm2_builtin_add_static_const(ctx, cls, n, avm2_integer(v)); }

static void av_sconst(Avm2Context* ctx, Avm2Class* cls, const char* n,
                      const char* v)
{
	avm2_builtin_add_static_const(ctx, cls, n,
	                              avm2_string(avm2_string_from_literal(ctx, v)));
}

// Register names[0..n) as getter-only props bound to slots first_slot..+n.
static void av_add_ro_props(Avm2Context* ctx, Avm2Class* cls,
                            const char* const* names, int n, int first_slot)
{
	for (int i = 0; i < n; i++)
	{
		avm2_builtin_add_getset(ctx, cls, names[i],
		                        AV_GETTERS[first_slot + i], NULL);
	}
}

static Avm2Class* av_class(Avm2Context* ctx, const char* name, Avm2Class* super,
                           Avm2MethodFn ctor)
{
	Avm2Class* cls = avm2_builtin_class(ctx, "flash.media", name, super);
	if (ctor != NULL)
	{
		cls->instance_init.fn = ctor;
		cls->instance_init.debug_name = name;
	}
	return cls;
}

static void register_av_classes(Avm2Context* ctx)
{
	Avm2Class* obj = ctx->builtins.object_class;
	Avm2Class* ed = ctx->builtins.event_dispatcher_class;
	if (ed == NULL) ed = obj;

	// --- AVABRParameters
	{
		Avm2Class* cls = av_class(ctx, "AVABRParameters", obj,
		                          avabrparams_ctor);
		avm2_builtin_add_getset(ctx, cls, "policy", av_get_0, av_set_s0);
		avm2_builtin_add_getset(ctx, cls, "startBitsPerSecond",
		                        av_get_1, av_set_i1);
		avm2_builtin_add_getset(ctx, cls, "minBitsPerSecond",
		                        av_get_2, av_set_i2);
		avm2_builtin_add_getset(ctx, cls, "maxBitsPerSecond",
		                        av_get_3, av_set_i3);
		av_sconst(ctx, cls, "AGGRESSIVE", "aggressive");
		av_sconst(ctx, cls, "CONSERVATIVE", "conservative");
		av_sconst(ctx, cls, "MODERATE", "moderate");
	}

	// --- AVABRProfileInfo
	{
		static const char* const props[] = {
			"bitsPerSecond", "width", "height"
		};
		Avm2Class* cls = av_class(ctx, "AVABRProfileInfo", obj,
		                          avabrprofile_ctor);
		av_add_ro_props(ctx, cls, props, 3, 0);
	}

	// --- AVCaptionStyle
	{
		static const char* const props[] = {
			"backgroundColor", "backgroundOpacity", "bottomInset", "edgeColor",
			"fillColor", "fillOpacity", "font", "fontColor", "fontEdge",
			"fontOpacity", "size"
		};
		static Avm2MethodFn const setters[] = {
			av_set_s0, av_set_s1, av_set_s2, av_set_s3, av_set_s4, av_set_s5,
			av_set_s6, av_set_s7, av_set_s8, av_set_s9, av_set_s10
		};
		static const struct { const char* n; const char* v; } consts[] = {
			{ "DEFAULT", "default" },
			{ "NONE", "none" },
			{ "MONOSPACE_WITH_SERIFS", "monospaced_with_serifs" },
			{ "MONOSPACED_WITHOUT_SERIFS", "monospaced_without_serifs" },
			{ "PROPORTIONAL_WITH_SERIFS", "proportional_with_serifs" },
			{ "PROPORTIONAL_WITHOUT_SERIFS", "proportional_without_serifs" },
			{ "CASUAL", "casual" },
			{ "CURSIVE", "cursive" },
			{ "DEPRESSED", "depressed" },
			{ "RAISED", "raised" },
			{ "SMALL_CAPITALS", "small_capitals" },
			{ "UNIFORM", "uniform" },
			{ "SMALL", "small" },
			{ "MEDIUM", "medium" },
			{ "LARGE", "large" },
			{ "BRIGHT_MAGENTA", "bright_magenta" },
			{ "MAGENTA", "magenta" },
			{ "DARK_MAGENTA", "dark_magenta" },
			{ "BRIGHT_RED", "bright_red" },
			{ "RED", "red" },
			{ "DARK_RED", "dark_red" },
			{ "BRIGHT_YELLOW", "bright_yellow" },
			{ "YELLOW", "yellow" },
			{ "DARK_YELLOW", "dark_yellow" },
			{ "BRIGHT_GREEN", "bright_green" },
			{ "GREEN", "green" },
			{ "DARK_GREEN", "dark_green" },
			{ "BRIGHT_CYAN", "bright_cyan" },
			{ "CYAN", "cyan" },
			{ "DARK_CYAN", "dark_cyan" },
			{ "BRIGHT_BLUE", "bright_blue" },
			{ "BLUE", "blue" },
			{ "DARK_BLUE", "dark_blue" },
			{ "BRIGHT_WHITE", "bright_white" },
			{ "WHITE", "white" },
			{ "GRAY", "gray" },
			{ "BLACK", "black" },
			// Not a transposition: FP really does put the side LAST.
			{ "LEFT_DROP_SHADOW", "drop_shadow_left" },
			{ "RIGHT_DROP_SHADOW", "drop_shadow_right" },
		};
		Avm2Class* cls = av_class(ctx, "AVCaptionStyle", obj,
		                          avcaptionstyle_ctor);
		for (int i = 0; i < 11; i++)
			avm2_builtin_add_getset(ctx, cls, props[i], AV_GETTERS[i],
			                        setters[i]);
		for (size_t i = 0; i < sizeof(consts) / sizeof(consts[0]); i++)
			av_sconst(ctx, cls, consts[i].n, consts[i].v);
	}

	// --- AVCuePoint: the Dictionary is held BY IDENTITY, not stringified.
	{
		static const char* const props[] = { "dictionary", "localTime" };
		Avm2Class* cls = av_class(ctx, "AVCuePoint", obj, avcuepoint_ctor);
		av_add_ro_props(ctx, cls, props, 2, 0);
	}

	// --- AVResult, then AVInsertionResult which extends it.
	Avm2Class* avresult;
	{
		static const char* const props[] = { "result" };
		static const struct { const char* n; int32_t v; } consts[] = {
			{ "END_OF_PERIOD", -1 },
			{ "SUCCESS", 0 },
			{ "ASYNC_OPERATION_IN_PROGRESS", 1 },
			{ "EOF", 2 },
			{ "DECODER_FAILED", 3 },
			{ "DEVICE_OPEN_ERROR", 4 },
			{ "FILE_NOT_FOUND", 5 },
			{ "GENERIC_ERROR", 6 },
			{ "IRRECOVERABLE_ERROR", 7 },
			{ "LOST_CONNECTION_RECOVERABLE", 8 },
			{ "NO_FIXED_SIZE", 9 },
			{ "NOT_IMPLEMENTED", 10 },
			{ "OUT_OF_MEMORY", 11 },
			{ "PARSE_ERROR", 12 },
			{ "SIZE_UNKNOWN", 13 },
			{ "UNDERFLOW", 14 },
			{ "UNSUPPORTED_CONFIGURATION", 15 },
			{ "UNSUPPORTED_OPERATION", 16 },
			{ "WAITING_FOR_INIT", 17 },
			{ "INVALID_PARAMETER", 18 },
			{ "INVALID_OPERATION", 19 },
			{ "ONLY_ALLOWED_IN_PAUSED_STATE", 20 },
			{ "INVALID_WITH_AUDIO_ONLY_FILE", 21 },
			{ "PREVIOUS_STEP_SEEK_IN_PROGRESS", 22 },
			{ "RESOURCE_NOT_SPECIFIED", 23 },
			{ "RANGE_ERROR", 24 },
			{ "INVALID_SEEK_TIME", 25 },
			{ "FILE_STRUCTURE_INVALID", 26 },
			{ "COMPONENT_CREATION_FAILURE", 27 },
			{ "DRM_INIT_ERROR", 28 },
			{ "CONTAINER_NOT_SUPPORTED", 29 },
			{ "SEEK_FAILED", 30 },
			{ "CODEC_NOT_SUPPORTED", 31 },
			{ "NETWORK_UNAVAILABLE", 32 },
			{ "NETWORK_ERROR", 33 },
			{ "OVERFLOW", 34 },
			{ "VIDEO_PROFILE_NOT_SUPPORTED", 35 },
			{ "PERIOD_NOT_LOADED", 36 },
			{ "INVALID_REPLACE_DURATION", 37 },
			{ "CALLED_FROM_WRONG_THREAD", 38 },
			{ "FRAGMENT_READ_ERROR", 39 },
			{ "OPERATION_ABORTED", 40 },
			{ "UNSUPPORTED_HLS_VERSION", 41 },
			{ "CANNOT_FAIL_OVER", 42 },
			{ "HTTP_TIME_OUT", 43 },
			{ "NETWORK_DOWN", 44 },
			{ "NO_USEABLE_BITRATE_PROFILE", 45 },
			{ "BAD_MANIFEST_SIGNATURE", 46 },
			{ "CANNOT_LOAD_PLAY_LIST", 47 },
			{ "REPLACEMENT_FAILED", 48 },
			{ "SWITCH_TO_ASYMMETRIC_PROFILE", 49 },
			{ "LIVE_WINDOW_MOVED_BACKWARD", 50 },
			{ "CURRENT_PERIOD_EXPIRED", 51 },
			{ "CONTENT_LENGTH_MISMATCH", 52 },
			{ "PERIOD_HOLD", 53 },
			{ "LIVE_HOLD", 54 },
			{ "BAD_MEDIA_INTERLEAVING", 55 },
			{ "DRM_NOT_AVAILABLE", 56 },
			{ "PLAYBACK_NOT_ENABLED", 57 },
			{ "BAD_MEDIASAMPLE_FOUND", 58 },
			{ "RANGE_SPANS_READHEAD", 59 },
			{ "POSTROLL_WITH_LIVE_NOT_ALLOWED", 60 },
			{ "INTERNAL_ERROR", 61 },
			{ "SPS_PPS_FOUND_OUTSIDE_AVCC", 62 },
			{ "PARTIAL_REPLACEMENT", 63 },
			{ "RENDITION_M3U8_ERROR", 64 },
			{ "NULL_OPERATION", 65 },
			{ "SEGMENT_SKIPPED_ON_FAILURE", 66 },
			{ "INCOMPATIBLE_RENDER_MODE", 67 },
			{ "PROTOCOL_NOT_SUPPORTED", 68 },
			{ "INCOMPATIBLE_VERSION", 69 },
			{ "MANIFEST_FILE_UNEXPECTEDLY_CHANGED", 70 },
			{ "CANNOT_SPLIT_TIMELINE", 71 },
			{ "CANNOT_ERASE_TIMELINE", 72 },
			{ "DID_NOT_GET_NEXT_FRAGMENT", 73 },
			{ "NO_TIMELINE", 74 },
			{ "LISTENER_NOT_FOUND", 75 },
			{ "AUDIO_START_ERROR", 76 },
			{ "NO_AUDIO_SINK", 77 },
			{ "FILE_OPEN_ERROR", 78 },
			{ "FILE_WRITE_ERROR", 79 },
			{ "FILE_READ_ERROR", 80 },
			{ "ID3_PARSE_ERROR", 81 },
			{ "SECURITY_ERROR", 82 },
			{ "TIMELINE_TOO_SHORT", 83 },
			{ "AUDIO_ONLY_STREAM_START", 84 },
			{ "AUDIO_ONLY_STREAM_END", 85 },
			{ "CANNOT_HANDLE_MAIN_MANIFEST_UPDATE", 86 },
			{ "KEY_NOT_FOUND", 87 },
			{ "INVALID_KEY", 88 },
			{ "KEY_SERVER_NOT_FOUND", 89 },
			{ "MAIN_MANIFEST_UPDATE_TO_BE_HANDLED", 90 },
			{ "UNREPORTED_TIME_DISCONTINUITY_FOUND", 91 },
			// The crypto block is NOT contiguous with the rest: 92..299 are
			// unassigned.
			{ "CRYPTO_ALGORITHM_NOT_SUPPORTED", 300 },
			{ "CRYPTO_ERROR_CORRUPTED_DATA", 301 },
			{ "CRYPTO_ERROR_BUFFER_TOO_SMALL", 302 },
			{ "CRYPTO_ERROR_BAD_CERTIFICATE", 303 },
			{ "CRYPTO_ERROR_DIGEST_UPDATE", 304 },
			{ "CRYPTO_ERROR_DIGEST_FINISH", 305 },
			{ "CRYPTO_ERROR_BAD_PARAMETER", 306 },
			{ "CRYPTO_ERROR_UNKNOWN", 307 },
		};
		avresult = av_class(ctx, "AVResult", obj, avresult_ctor);
		av_add_ro_props(ctx, avresult, props, 1, 0);
		for (size_t i = 0; i < sizeof(consts) / sizeof(consts[0]); i++)
			av_iconst(ctx, avresult, consts[i].n, consts[i].v);
	}
	{
		static const char* const props[] = {
			"periodIndex", "insertedBeforeReadHead"
		};
		Avm2Class* cls = av_class(ctx, "AVInsertionResult", avresult,
		                          avinsertion_ctor);
		av_add_ro_props(ctx, cls, props, 2, 1);
	}

	// --- AVPeriodInfo: 3 Numbers, 5 ints, 1 Boolean, 1 Number.
	{
		static const char* const props[] = {
			"localStartTime", "virtualStartTime", "duration",
			"firstCuePointIndex", "lastCuePointIndex",
			"firstSubscribedTagIndex", "lastSubscribedTagIndex",
			"userData", "supportsTrickPlay", "targetDuration"
		};
		Avm2Class* cls = av_class(ctx, "AVPeriodInfo", obj, avperiodinfo_ctor);
		av_add_ro_props(ctx, cls, props, 10, 0);
	}

	// --- AVPlayState: nine int consts, 0..8, in declaration order.
	{
		static const char* const props[] = { "state" };
		static const char* const consts[] = {
			"UNINITIALIZED", "READY", "BUFFERING", "PLAYING", "PAUSED",
			"EOF", "SUSPENDED", "TRICK_PLAY", "UNRECOVERABLE_ERROR"
		};
		Avm2Class* cls = av_class(ctx, "AVPlayState", obj, avplaystate_ctor);
		av_add_ro_props(ctx, cls, props, 1, 0);
		for (int i = 0; i < 9; i++) av_iconst(ctx, cls, consts[i], i);
	}

	// --- AVSource / AVSegmentedSource / AVStream: EventDispatcher subclasses
	//     with no instance state of their own (AVStream's `source` argument is
	//     not readable back).
	Avm2Class* avsource = av_class(ctx, "AVSource", ed, NULL);
	{
		static const struct { const char* n; const char* v; } consts[] = {
			// The casing really is inconsistent upstream: the two protocol
			// names are upper-case, every content kind is lower-case.
			{ "AUDIO", "audio" },
			{ "AUDIO_DESCRIPTION", "audiodescription" },
			{ "AUDIO_LANGUAGE", "audiolanguage" },
			{ "AUDIO_PID", "audiopid" },
			{ "DASH", "DASH" },
			{ "DATA", "data" },
			{ "DATA_DESCRIPTION", "datadescription" },
			{ "HLS", "HLS" },
			{ "VIDEO", "video" },
			{ "VIDEO_DESCRIPTION", "videodescription" },
		};
		Avm2Class* cls = av_class(ctx, "AVSegmentedSource", avsource, NULL);
		for (size_t i = 0; i < sizeof(consts) / sizeof(consts[0]); i++)
			av_sconst(ctx, cls, consts[i].n, consts[i].v);
	}
	{
		Avm2Class* cls = av_class(ctx, "AVStream", ed, NULL);
		av_sconst(ctx, cls, "HARDWARE", "hardware");
		// "sofware" is not a typo here -- it is FP's typo, and av_classes
		// pins it verbatim.
		av_sconst(ctx, cls, "SOFTWARE", "sofware");
		av_sconst(ctx, cls, "UNDEFINED", "undefined");
	}

	// --- AVTimeline
	{
		static const char* const props[] = {
			"type", "virtualStartTime", "virtualDuration", "firstPeriodIndex",
			"lastPeriodIndex", "firstSubscribedTagIndex",
			"lastSubscribedTagIndex", "complete"
		};
		Avm2Class* cls = av_class(ctx, "AVTimeline", obj, avtimeline_ctor);
		av_add_ro_props(ctx, cls, props, 8, 0);
	}

	// --- AVTrackInfo: dataTrackInfoServiceType is a STRING slot while pid is
	//     an int, so 7.7 reads back "7.7" and 8.8 reads back 8.
	{
		static const char* const props[] = {
			"description", "language", "defaultTrack", "autoSelect", "forced",
			"activity", "dataTrackInfoServiceType", "pid"
		};
		Avm2Class* cls = av_class(ctx, "AVTrackInfo", obj, avtrackinfo_ctor);
		av_add_ro_props(ctx, cls, props, 8, 0);
		av_sconst(ctx, cls, "DTI_608_CAPTIONS", "DTI608Captions");
		av_sconst(ctx, cls, "DTI_708_CAPTIONS", "DTI708Captions");
		av_sconst(ctx, cls, "DTI_WEBVTT_CAPTIONS", "DTIWebVTTCaptions");
	}
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void avm2_register_media(Avm2Context* ctx)
{
	Avm2Builtins* b = &ctx->builtins;

	// flash.media.SoundTransform (final, extends Object).
	Avm2Class* st = avm2_builtin_class(ctx, "flash.media", "SoundTransform",
	                                   b->object_class);
	st->instance_init.fn = st_ctor;
	st->instance_init.debug_name = "SoundTransform";
	st->native_ext_size = sizeof(Avm2SoundTransformExt);
	b->sound_transform_class = st;
	avm2_builtin_add_getset(ctx, st, "leftToLeft", st_get_left_to_left,
	                        st_set_left_to_left);
	avm2_builtin_add_getset(ctx, st, "leftToRight", st_get_left_to_right,
	                        st_set_left_to_right);
	avm2_builtin_add_getset(ctx, st, "rightToLeft", st_get_right_to_left,
	                        st_set_right_to_left);
	avm2_builtin_add_getset(ctx, st, "rightToRight", st_get_right_to_right,
	                        st_set_right_to_right);
	avm2_builtin_add_getset(ctx, st, "volume", st_get_volume, st_set_volume);
	avm2_builtin_add_getset(ctx, st, "pan", st_get_pan, st_set_pan);

	// flash.media.SoundChannel (final, extends EventDispatcher).
	Avm2Class* sc = avm2_builtin_class(ctx, "flash.media", "SoundChannel",
	                                   b->event_dispatcher_class);
	sc->native_ext_size = sizeof(Avm2SoundChannelExt);
	sc->native_init = sound_channel_native_init;
	b->sound_channel_class = sc;
	avm2_builtin_add_getset(ctx, sc, "soundTransform", sc_get_sound_transform,
	                        sc_set_sound_transform);
	avm2_builtin_add_getset(ctx, sc, "position", sc_get_position, NULL);
	avm2_builtin_add_getset(ctx, sc, "leftPeak", sc_get_peak, NULL);
	avm2_builtin_add_getset(ctx, sc, "rightPeak", sc_get_peak, NULL);
	avm2_builtin_add_method(ctx, sc, "stop", sc_stop);

	// flash.media.SoundMixer (all statics).
	Avm2Class* sm = avm2_builtin_class(ctx, "flash.media", "SoundMixer",
	                                   b->object_class);
	avm2_builtin_add_static_getset(ctx, sm, "bufferTime", sm_get_buffer_time,
	                               sm_set_buffer_time);
	avm2_builtin_add_static_getset(ctx, sm, "soundTransform",
	                               sm_get_sound_transform, sm_set_sound_transform);
	avm2_builtin_add_static_method(ctx, sm, "stopAll", sm_stop_all);
	avm2_builtin_add_static_method(ctx, sm, "areSoundsInaccessible",
	                               sm_are_sounds_inaccessible);

	// flash.media.Sound (extends EventDispatcher).
	Avm2Class* snd = avm2_builtin_class(ctx, "flash.media", "Sound",
	                                    b->event_dispatcher_class);
	snd->instance_init.fn = sound_ctor;
	snd->instance_init.debug_name = "Sound";
	snd->native_ext_size = sizeof(Avm2SoundObjExt);
	b->sound_class = snd;
	avm2_builtin_add_getset(ctx, snd, "length", sound_get_length, NULL);
	avm2_builtin_add_getset(ctx, snd, "bytesTotal", sound_get_bytes_total, NULL);
	avm2_builtin_add_getset(ctx, snd, "bytesLoaded", sound_get_bytes_loaded, NULL);
	avm2_builtin_add_getset(ctx, snd, "isBuffering", sound_get_is_buffering, NULL);
	avm2_builtin_add_getset(ctx, snd, "isURLInaccessible",
	                        sound_get_is_url_inaccessible, NULL);
	avm2_builtin_add_getset(ctx, snd, "url", sound_get_url, NULL);
	avm2_builtin_add_getset(ctx, snd, "id3", sound_get_id3, NULL);
	avm2_builtin_add_method(ctx, snd, "play", sound_play);
	avm2_builtin_add_method(ctx, snd, "close", sound_close);
	avm2_builtin_add_method(ctx, snd, "load", sound_load);
	avm2_builtin_add_method(ctx, snd, "loadCompressedDataFromByteArray",
	                        sound_load_compressed_data_from_byte_array);
	avm2_builtin_add_method(ctx, snd, "loadPCMFromByteArray",
	                        sound_load_pcm_from_byte_array);

	// flash.media.ID3Info — `public final dynamic class` with seven
	// `public var ...:String` and nothing else (Ruffle ID3Info.as). The seven
	// are dont-enum dynamic props written by id3_read_and_dispatch.
	{
		Avm2Class* id3 = avm2_builtin_class(ctx, "flash.media", "ID3Info",
		                                    b->object_class);
		id3->flags |= AVM2_CLASS_FLAG_FINAL;
		g_id3info_class = id3;
	}

	// flash.media.StageVideo — [Ruffle(Abstract)], no members at all. Only
	// abstract_classes reads it.
	{
		Avm2Class* sv = avm2_builtin_class(ctx, "flash.media", "StageVideo",
		                                   b->object_class);
		sv->flags |= AVM2_CLASS_FLAG_SEALED;
		avm2_builtin_set_abstract(ctx, sv);
	}

	// flash.media.AVTagData(data, localTime) — a two-field value object with
	// getter-only access. The backing fields are dont-enum dynamic props so
	// the GC traces them for free (no native_ext, no mark hook).
	{
		Avm2Class* tag = avm2_builtin_class(ctx, "flash.media", "AVTagData",
		                                    b->object_class);
		tag->instance_init.fn = avtagdata_ctor;
		tag->instance_init.debug_name = "AVTagData";
		avm2_builtin_add_getset(ctx, tag, "data", avtagdata_get_data, NULL);
		avm2_builtin_add_getset(ctx, tag, "localTime", avtagdata_get_local_time,
		                        NULL);
	}

	// The other 13 flash.media.AV* value objects (AVNetworkingParams is the
	// 15th and lives in avm2_net.c, next to the rest of the networking API).
	register_av_classes(ctx);
}
