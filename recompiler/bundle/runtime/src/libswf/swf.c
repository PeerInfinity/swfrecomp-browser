#ifndef NO_GRAPHICS

#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <swf.h>
#include <tag.h>
#include <action.h>
#include <object.h>
#include <variables.h>
#include <renderer.h>
#include <utils.h>
#include <dialog_events.h>
#include <socket_events.h>
#include <heap.h>
#include <audio/audio.h>
#ifdef OFFSCREEN_RENDER
#include <libswf/capture.h>
#endif

// Generated per-test; supplies VIDEO_HIGHEST_W/H (largest DefineVideoStream
// declared dimensions in this SWF, 0 when the movie has no embedded video).
// Guarded so a stale RecompiledTags/ from before that emission still builds.
#if defined(__has_include)
#  if __has_include("constants.h")
#    include "constants.h"
#  endif
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// In-browser performance HUD + uncapped benchmark (Phase 2 of the WASM-game
// performance plan). Called once per rendered frame with the frame's CPU time
// (ms) and the per-frame budget (1000/fps). Records a rolling window. Visibility
// is OFF by default and driven externally by the host page's "Toggle HUD" button
// via window.__swfHudOn; the URL ?perfhud=1 / ?perfbench=1 also enable it (a
// standalone fallback for pages without that button). When shown it renders a
// stats panel (frame CPU + delivered frame-time + late count + max FPS) plus an
// "Uncapped" button that skips the frame-pacing sleep to measure max sustainable
// FPS. Returns whether uncapped is on.
EM_JS(int, swf_perf_report, (double elapsed_ms, double budget_ms, double present_ms, int live_obj, int live_arr), {
	var S = globalThis.__swfPerf;
	if (!S) {
		S = globalThis.__swfPerf = { cpu: [], iv: [], rp: [], bad: [], cap: 120, i: 0, frames: 0,
			uncapped: false, ui: null, pre: null, bU: null, last: 0, lastT: 0,
			// Steady-state filtering: drop the first `warmup` frames (one-time
			// startup work — shader/pipeline compile, the ~4MB initial texture
			// upload) and any frame whose delivered interval exceeds `throttleMs`
			// (Chrome RAF-throttles a non-foreground tab to ~1Hz; the catch-up
			// frame then does huge-dt work). Folding those into the rolling mean is
			// what made the HUD read 100-200%+ when steady state is well under
			// budget — see tools/divergence/perf/WINDOWS_REALGPU_RESULTS.md.
			warmup: 30, throttleMs: 250 };
		try { if (location.search.indexOf('perfbench=1') >= 0) S.uncapped = true; } catch (e) {}
	}
	// `present_ms` = time spent in renderer_poll (swap/present + async-GPU yield);
	// `elapsed_ms - present_ms` ≈ AVM + render-submit (mostly CPU). Lets us tell a
	// CPU-bound frame from one that's parked on the SwiftShader/GPU present.
	// Standalone fallback: if no host page set the visibility flag, derive the
	// initial state from the URL so ?perfhud=1 / ?perfbench=1 still work.
	if (typeof window !== 'undefined' && window.__swfHudOn === undefined) {
		try {
			var q = location.search;
			window.__swfHudOn = (q.indexOf('perfhud=1') >= 0 || q.indexOf('perfbench=1') >= 0);
		} catch (e) { window.__swfHudOn = false; }
	}
	var on = (typeof window !== 'undefined' && window.__swfHudOn) ? true : false;

	var nowT = (typeof performance !== 'undefined') ? performance.now() : 0;
	// Wall-clock gap since the previous frame's report = the delivered frame
	// period (includes the pacing sleep + any browser scheduling/jank).
	var interval = (S.lastT > 0) ? (nowT - S.lastT) : 0;
	S.lastT = nowT;

	// Mark a frame "tainted" (excluded from the steady-state stats below) if it's
	// in the warmup window, was delivered after a throttle/stall gap, the tab was
	// hidden, or it's the first frame (no valid delivered period yet). Kept in a
	// `bad` ring buffer aligned 1:1 with cpu/iv/rp so the stats can filter it out.
	var tainted = (S.frames < S.warmup
		|| interval <= 0 || interval > S.throttleMs
		|| (typeof document !== 'undefined' && document.hidden)) ? 1 : 0;
	if (S.cpu.length < S.cap) { S.cpu.push(elapsed_ms); S.rp.push(present_ms); S.iv.push(interval); S.bad.push(tainted); }
	else { S.cpu[S.i] = elapsed_ms; S.rp[S.i] = present_ms; S.iv[S.i] = interval; S.bad[S.i] = tainted; S.i = (S.i + 1) % S.cap; }
	S.frames++;

	// Build the panel lazily the first time it is shown (default: hidden).
	if (on && !S.ui && typeof document !== 'undefined' && document.createElement && document.body) {
		var box = document.createElement('div');
		box.style.cssText = 'position:fixed;top:6px;left:6px;z-index:99999;'
			+ 'font:11px/1.35 monospace;color:#7CFC00;background:rgba(0,0,0,.72);'
			+ 'padding:6px 8px;border-radius:4px;pointer-events:auto';
		var bU = document.createElement('button');
		bU.textContent = 'Uncapped: ' + (S.uncapped ? 'ON' : 'OFF');
		bU.style.cssText = 'font:11px monospace;margin:0 0 4px;padding:2px 6px;cursor:pointer;'
			+ 'border:1px solid #7CFC00;background:#111;color:#7CFC00;border-radius:3px';
		bU.onclick = function() {
			S.uncapped = !S.uncapped;
			S.bU.textContent = 'Uncapped: ' + (S.uncapped ? 'ON' : 'OFF');
		};
		var pre = document.createElement('pre');
		pre.style.cssText = 'margin:0;white-space:pre';
		box.appendChild(bU); box.appendChild(pre);
		document.body.appendChild(box);
		S.ui = box; S.pre = pre; S.bU = bU;
	}
	if (S.ui) S.ui.style.display = on ? 'block' : 'none';

	if (on && S.pre && nowT - S.last >= 200) {   // throttle DOM updates to ~5 Hz
		S.last = nowT;
		// stat over the window; when clean=true, include only untainted frames
		// (steady state) using the index-aligned S.bad flags.
		var stat = function(arr, clean) {
			var b = [];
			for (var k = 0; k < arr.length; k++) if (!clean || !S.bad[k]) b.push(arr[k]);
			b.sort(function(x, y) { return x - y; });
			var n = b.length, s = 0;
			for (var k2 = 0; k2 < n; k2++) s += b[k2];
			return { n: n, mean: n ? s / n : 0,
				p95: n ? b[Math.min(n - 1, Math.floor(n * 0.95))] : 0,
				max: n ? b[n - 1] : 0 };
		};
		var total = S.cpu.length, nbad = 0;
		for (var jb = 0; jb < total; jb++) if (S.bad[jb]) nbad++;
		// Headline = steady-state (clean) stats. If EVERY frame is tainted (e.g.
		// the tab is persistently throttled/hidden), fall back to raw so the panel
		// still shows something, flagged THROTTLED so the number isn't trusted.
		var throttled = (total > 0 && nbad === total);
		var c = stat(S.cpu, !throttled), v = stat(S.iv, !throttled), rp = stat(S.rp, !throttled);
		var cRaw = stat(S.cpu, false);
		var cpuOnly = Math.max(0, c.mean - rp.mean);   // AVM + render-submit
		var head = budget_ms > 0 ? (c.mean / budget_ms * 100) : 0;
		var susFps = c.mean > 0 ? (1000 / c.mean) : 0;
		var capFps = budget_ms > 0 ? (1000 / budget_ms) : 0;
		var devMean = v.n ? (v.mean - budget_ms) : 0;       // avg drift from target
		S.pre.textContent =
			'SWF perf  ' + (S.uncapped ? '[UNCAPPED]' : '[capped ' + capFps.toFixed(0) + 'fps]')
				+ (throttled ? '  [THROTTLED]' : '') + '\n'
			+ 'frame CPU   mean ' + c.mean.toFixed(2) + '  p95 ' + c.p95.toFixed(2) + '  max ' + c.max.toFixed(2) + ' ms  (' + head.toFixed(0) + '% budget)\n'
			+ '  avm+submit ' + cpuOnly.toFixed(2) + '   present ' + rp.mean.toFixed(2) + '  p95 ' + rp.p95.toFixed(2) + ' ms\n'
			+ 'frame time  mean ' + v.mean.toFixed(1) + '  p95 ' + v.p95.toFixed(1) + '  max ' + v.max.toFixed(1) + ' ms  (target ' + budget_ms.toFixed(1) + ', ' + (devMean >= 0 ? '+' : '') + devMean.toFixed(1) + ')\n'
			+ 'steady-state: ' + c.n + ' / ' + total + ' frames  (excl ' + nbad + ' warmup/throttle; raw max ' + cRaw.max.toFixed(1) + ')\n'
			+ 'max sustainable ~' + susFps.toFixed(0) + ' fps\n'
			// Live AS heap objects (memory-reclamation plan Stage 0): a count
			// that climbs without bound on clip-churn gameplay = a leak.
			// Slot 2 is VM-dependent — AVM1 passes live arrays, AVM2 passes
			// live strings (avm2_gc_live_strings) — hence the neutral label.
			+ 'live AS objs ' + live_obj + '  arrays/strings ' + live_arr;
	}
	return S.uncapped ? 1 : 0;
});
#endif

int quit_swf;
int is_playing = 1;
int bad_poll;
size_t current_frame;
size_t next_frame;
int manual_next_frame;
ActionVar* temp_val;

// Global frame access for ActionCall opcode
frame_func* g_frame_funcs = NULL;
size_t g_frame_count = 0;

// Drag state tracking
int is_dragging = 0;
char* dragged_target = NULL;
float g_drag_virt_x = 0.0f;
float g_drag_virt_y = 0.0f;
char g_drag_target_name[256] = "";

// Frame execution state — needed by tag.c sprite advancement
int catch_up_mode = 0;
int g_tag_skip_mode = 0;

// Debug: minimum per-frame wall-clock budget in ms (0 = off). When set (via the
// display-bridge dbgSetFrameCapMs from JS), the browser render loop sleeps to at
// least this long per frame, throttling the frame rate. On a software-WebGPU
// backend (WSL2) an uncapped busy board floods the present queue faster than it
// drains; the backlog makes screenshots and the debug framebuffer readback stall
// for many seconds. Capping to ~6-10fps keeps the queue drained so captures
// (and Playwright input) stay responsive. No effect on native/headless builds.
int g_debug_frame_floor_ms = 0;

// Goto-catch-up state and executors — ported from swf_core.c. This
// duplication between the two surviving frame loops is still outstanding.
int goto_from_action = 0;
int g_deferred_root_goto = 0;
int g_skip_inline_target_script = 0;
int g_goto_inlined_in_caller_frame = 0;
// See swf_core.c for the rationale. Set to 1 for exactly the frame_0 re-run
// that follows a natural backward timeline wrap; consulted by tagPlaceObject2.
int g_loopback_replay = 0;
int g_loopback_replay_armed = 0;
#ifdef OFFSCREEN_RENDER
// Phase 4 (TRANSFORMED_BY_SCRIPT_WRAP_BACK) — DEFERRED BY ONE TICK.
// The natural end-of-movie wrap is a *next-tick* event: tick N still displays
// the last frame, tick N+1 displays frame 0. Applying the promoted goto inside
// tick N (which is what setting goto_from_action there used to do) ran
// frame_funcs[0] before the tick's captures, so the last frame was never
// displayed and every later capture was one tick early. Key test:
// from_gnash/misc-ming.all/loop/simple_loop_test (period 3 instead of 4).
// See the consumption site at the top of the tick loop.
static int g_pending_natural_wrap_promote = 0;
// Set for exactly the tick that replays frame 0 under the deferred promotion,
// so the close-out half (catch_up_backward reset + unsurvivor sweep) runs once.
static int g_wrap_promote_active = 0;
#endif

extern void ng_display_clear_after(SWFAppContext* app_context, size_t target_frame);
extern void ng_display_cleanup_unplaced_after(SWFAppContext* app_context, size_t target_frame);
extern void actionRewindCleanup(SWFAppContext* app_context);
// actionDrainActionQueueByKind comes from action_queue.h below
extern void actionDrainSuppressEnter(void);
extern void actionDrainSuppressLeave(void);
extern void actionGotoCatchupEnter(void);
extern void actionGotoCatchupLeave(void);
extern int  ng_swapToRootDL(DisplayObject** saved_dl, size_t* saved_max, size_t* saved_cap);
extern void ng_restoreFromRootDL(DisplayObject* saved_dl, size_t saved_max, size_t saved_cap);
extern void ng_run_deferred_sprite_init_before(SWFAppContext*, size_t);
extern void ng_run_deferred_sprite_init_on_or_after(SWFAppContext*, size_t);
extern int g_defer_sprite_init;
extern size_t g_place_gen;
extern int catch_up_backward;
extern size_t catch_up_target;
#include <actionmodern/action_queue.h>

void ng_executeGotoCatchUp(SWFAppContext* app_context)
{
	if (!goto_from_action || !manual_next_frame) return;

	DisplayObject* saved_sprite_dl = NULL;
	size_t saved_sprite_max = 0, saved_sprite_cap = 0;
	int swapped = ng_swapToRootDL(&saved_sprite_dl, &saved_sprite_max, &saved_sprite_cap);

	g_place_gen++;

	frame_func* funcs = g_frame_funcs;
	size_t original_frame = current_frame;
	size_t target = next_frame;

	// Self-goto guard (ported from swf_core.c): a goto targeting the frame whose
	// own script is currently executing (e.g. GotoFrame2 play=1 /
	// gotoAndPlay(_currentframe) issued from the running frame) must NOT replay
	// tags or re-run funcs[target]. The playhead is already on `target`; the
	// inline funcs[target] call at the end of this function re-queues that same
	// script (via the recompiler-emitted actionQueueScript under g_tag_skip_mode),
	// whose drain re-issues the goto → unbounded inline frame replay within a
	// single tick (the script FIFO never drains, MAX_FRAMES can't bound it). The
	// NO_GRAPHICS path has had this guard; the OFFSCREEN_RENDER (graphics) path
	// lacked it, so it spun forever where no-graphics terminates and passes.
	// Flash does not re-execute the current frame's actions on a same-frame goto
	// within the tick — it just confirms the playhead and lets is_playing (set by
	// any trailing Stop/Play in the same script) decide whether the NEXT tick
	// advances. Consume the goto request and return so the main loop's natural
	// advance (current_frame++ when is_playing) still applies. Key test:
	// from_gnash/misc-ming.all/gotoFrame2Test (GotoFrame2 play=1 to the current
	// frame, then Stop()).
	if (target == original_frame) {
		if (swapped)
			ng_restoreFromRootDL(saved_sprite_dl, saved_sprite_max, saved_sprite_cap);
		goto_from_action = 0;
		manual_next_frame = 0;
		current_frame = target;
		return;
	}

	ng_display_clear_after(app_context, target);

	int saved_tag_skip = g_tag_skip_mode;
	int saved_defer_sprite = g_defer_sprite_init;
	g_tag_skip_mode = 0;
	g_defer_sprite_init = 1;
	catch_up_mode = 1;
	actionGotoCatchupEnter();
	if (target <= original_frame)
	{
		actionRewindCleanup(app_context);
		catch_up_backward = 1;
		catch_up_target = target;
		for (size_t f = 0; f <= target && f < g_frame_count; f++)
		{
			current_frame = f;
			if (funcs[f]) funcs[f](app_context);
		}
		catch_up_backward = 0;
		ng_display_cleanup_unplaced_after(app_context, target);
	}
	else
	{
		for (size_t f = original_frame + 1; f <= target && f < g_frame_count; f++)
		{
			current_frame = f;
			if (funcs[f]) funcs[f](app_context);
		}
	}
	actionGotoCatchupLeave();
	catch_up_mode = 0;
	g_tag_skip_mode = saved_tag_skip;
	(void)saved_defer_sprite;
	current_frame = target;

	actionDrainActionQueueByKind(app_context, AQ_KIND_CLIP_INIT);
	actionDrainActionQueueByKind(app_context, AQ_KIND_REGISTER_CTOR);

	if (swapped)
		ng_restoreFromRootDL(saved_sprite_dl, saved_sprite_max, saved_sprite_cap);

	goto_from_action = 0;
	manual_next_frame = 0;

	int skip_inline = g_skip_inline_target_script;
	g_skip_inline_target_script = 0;
	if (!skip_inline && target < g_frame_count && funcs[target])
	{
		int saved_defer_phase_f = g_defer_sprite_init;
		g_defer_sprite_init = 0;
		ng_run_deferred_sprite_init_before(app_context, target);
		int saved_tag_skip_phase_e = g_tag_skip_mode;
		g_tag_skip_mode = 1;
		actionDrainSuppressEnter();
		funcs[target](app_context);
		actionDrainSuppressLeave();
		g_tag_skip_mode = saved_tag_skip_phase_e;
		ng_run_deferred_sprite_init_on_or_after(app_context, target);
		g_defer_sprite_init = saved_defer_phase_f;
		g_goto_inlined_in_caller_frame = 1;
	}
}

void ng_executeGotoTagsOnly(SWFAppContext* app_context)
{
	if (!goto_from_action || !manual_next_frame) return;

	g_place_gen++;

	frame_func* funcs = g_frame_funcs;
	size_t original_frame = current_frame;
	size_t target = next_frame;

	ng_display_clear_after(app_context, target);

	int saved_tag_skip = g_tag_skip_mode;
	int saved_defer_sprite = g_defer_sprite_init;
	g_tag_skip_mode = 0;
	g_defer_sprite_init = 1;
	catch_up_mode = 1;
	if (target <= original_frame)
	{
		actionRewindCleanup(app_context);
		catch_up_backward = 1;
		catch_up_target = target;
		for (size_t f = 0; f <= target && f < g_frame_count; f++)
		{
			current_frame = f;
			if (funcs[f]) funcs[f](app_context);
		}
		catch_up_backward = 0;
		ng_display_cleanup_unplaced_after(app_context, target);
	}
	else
	{
		for (size_t f = original_frame + 1; f <= target && f < g_frame_count; f++)
		{
			current_frame = f;
			if (funcs[f]) funcs[f](app_context);
		}
	}
	catch_up_mode = 0;
	g_tag_skip_mode = saved_tag_skip;
	g_defer_sprite_init = saved_defer_sprite;

	current_frame = original_frame;

	g_deferred_root_goto = 1;
}

Character* dictionary = NULL;

DisplayObject* display_list = NULL;
size_t max_depth = 0;

// Dummy sprite object pointer for the wasm browser graphics build only —
// in --mode=graphics native (OFFSCREEN_RENDER), tag.c's widened
// (NO_GRAPHICS || OFFSCREEN_RENDER) arm provides the real one and defining
// it here would conflict.
#ifndef OFFSCREEN_RENDER
DisplayObject* g_current_sprite_obj = NULL;
#endif

RenderContext* context;

void tagMain(SWFAppContext* app_context)
{
	frame_func* frame_funcs = app_context->frame_funcs;
	u32 frame_ms = app_context->fps > 0 ? 1000 / app_context->fps : 83;
#ifdef __EMSCRIPTEN__
	// Wall-clock frame pacing. Sleeping (budget - work) each frame lets the
	// per-frame sleep/timer overhead accumulate, so the movie runs slightly slow
	// and drifts out of sync with real time (and with Ruffle/Flash, which pace
	// off the wall clock). Instead, track the absolute time each frame is *due*
	// (anchor + N*budget) and sleep until then, so overhead never compounds and
	// the long-run rate is exactly app_context->fps. Use a double budget so
	// fractional rates (e.g. 30 -> 33.333 ms) don't round-drift either.
	double frame_budget_ms = app_context->fps > 0 ? 1000.0 / (double)app_context->fps : 83.0;
	double next_due_ms = 0.0;  // 0 = not yet anchored (set on the first frame)
#endif

#ifdef MAX_FRAMES
	// Test-mode termination: bound the loop. Mirrors swf_core.c's max_ticks.
	const size_t max_ticks = MAX_FRAMES;
	size_t tick_count = 0;
#endif

#ifdef OFFSCREEN_RENDER
	// Execution timeout (mirrors swf_core.c): bound any single script's
	// wall-clock time so an infinite loop in AS doesn't hang the test
	// runner. Tests opt-in via `max_execution_duration` in test.toml →
	// `-DMAX_EXECUTION_MS=N` from verify_output.py. On timeout,
	// actionCheckExecutionTimeout longjmps back here. Key test:
	// avm1/timeout (infinite-loop try/catch).
#ifdef MAX_EXECUTION_MS
	actionSetMaxExecutionDuration(MAX_EXECUTION_MS);
#endif
	actionResetExecutionTimer();
	jmp_buf timeout_jmp;
	if (setjmp(timeout_jmp) != 0)
	{
		// Returned here via longjmp — execution was halted by timeout.
		goto frame_loop_exit;
	}
	actionSetTimeoutJmp(&timeout_jmp);
#endif

	while (1)
	{
#ifdef MAX_FRAMES
		if (tick_count >= max_ticks) break;
		tick_count++;
#endif
		// Bump global tick counter so DisplayObject.placed_at_tick stamps
		// from this tick can be distinguished from earlier ticks. See
		// SWFRecompDocs/plans/defer-newly-placed-sprite-advance-plan.md.
		{ extern size_t g_tick_count; g_tick_count++; }

		// Tick boundary: promote last tick's freshly-initialized children of
		// attachMovie'd clips (no-op outside the CI modes). Mirrors swf_core.c.
		{
			extern void ng_upgrade_attached_standalone_initialized(void);
			ng_upgrade_attached_standalone_initialized();
		}

		// Tick boundary = VM quiescent: release dynamic_props detached last
		// tick (memory-reclamation plan Stage 1; see actionDeferDpropsRelease).
		actionDrainDpropsReleases(app_context);

		// Mirror swf_core.c's exit condition (around line 1056). The
		// recompiler-emits `quit_swf = 1` at the end of the last root
		// frame, but multi-frame sprites placed by that frame still need
		// to advance through their own timelines. Without this widened
		// check, the loop exits after one root tick and any sprite that
		// hasn't reached its own last frame is silently abandoned. Key
		// test: avm1/tell_target (script_3 lives on sprite_6 frame 1, so
		// it never runs without continued ticking after quit_swf=1).
		// Also gate on pending loadMovie / loadClip queues — otherwise
		// loadMovie from the last root frame exits before
		// actionFirePendingDirectLoads / actionFirePendingLoadInits get
		// to fire the child's init+frame0 (drops the "Child movie loaded!"
		// trace line in avm1/loadmovie and the loadmovie/loadmovienum
		// cluster). Mirrors swf_core.c (line ~1064).
		//
		// This applies to BOTH the headless graphics-native (OFFSCREEN_RENDER)
		// build AND the browser-WASM build. Browser-WASM previously used a bare
		// `if (quit_swf) break;`, which terminated the whole game loop the tick
		// after a non-looping (e.g. single-frame) root set quit_swf — killing
		// `_root.onEnterFrame`-driven games (Metanet's "N": frame_1 sets
		// `_root.onEnterFrame = RunApp` then quit_swf, so the demo + all menu
		// input died after ~2 ticks). Keeping the loop alive while a handler,
		// sprite, timer, sound, or pending load is still live is exactly the
		// headless behavior, so the two paths are now unified.
		{
			extern int hasPlayingSounds(void);
			extern int hasActiveNetStreams(void);
			extern int hasPlayingLevels(void);
			extern int g_pending_mcl_load_count;
			extern int g_pending_direct_load_count;
			// Keep the loop alive while pre-loaded input events remain so
			// post-quit_swf MouseMove / KeyDown / etc. still fire their AS2
			// handlers. swf_core.c (line ~1056) and swf_headless.c (line
			// ~1023) both gate on this. Without it, a test whose root
			// frame_0 sets quit_swf=1 and stops all sprites exits the loop
			// before input_events_pump_tick gets to deliver the queued
			// events. Key test: avm1/hittest_morph_input — onRollOver
			// queued for MouseMove (180,160) never fired.
			//
			// The preloaded-event queue (g_event_count/g_event_pos) only exists
			// in the headless graphics-native build; browser-WASM delivers mouse/
			// key events live via the render_webgpu.c emscripten callbacks, so
			// there is no queue to drain (treat as already drained).
#ifdef OFFSCREEN_RENDER
			extern size_t g_event_count;
			extern size_t g_event_pos;
			int events_drained = (g_event_pos >= g_event_count);
#else
			int events_drained = 1;
#endif
			// An open socket (or an undelivered socket action) keeps the loop
			// alive the same way an active NetStream does — the socket tick
			// further down is what would deliver it. Mirrors swf_core.c.
			if (quit_swf
			    && events_drained
			    && !actionHasEnterFrameHandlers()
			    && !hasPlayingSprites()
			    && !hasActiveTimers()
			    && !hasPlayingSounds()
			    && !hasActiveNetStreams()
			    && !swf_socket_pending()
			    && !swf_dialog_pending()
			    && !hasPlayingLevels()
			    && !hasClipEnterFrameHandlers()
			    && g_pending_mcl_load_count == 0
			    && !actionHasPendingLoadInits()
			    && g_pending_direct_load_count == 0) break;

			// FSCommand:quit was called in a prior tick — exit as soon as
			// nothing is still asking to run. Without this, a SWF that
			// FSCommand:quits from a non-last frame but still has the natural
			// end-of-movie loopback (manual_next_frame=1, next_frame=0) keeps
			// restarting frame 0 forever, because hasPlayingSprites() stays
			// true. `g_force_quit` is set only by FSCommand:quit (not by the
			// recompiler-emitted end-of-movie quit_swf=1), so this bypasses
			// hasPlayingSprites / hasPlayingLevels / pending-load checks that
			// the regular quit_swf gate insists on. Mirrors swf_core.c
			// (line ~906). Key test: from_shumway/timeline/timeline_as2_1
			// (frame 5 calls FSCommand:quit but frame 4 loops back to
			// frame 1, so the test re-cycles 3× before max_ticks hits).
			{
				extern int g_force_quit;
				if (g_force_quit
				    && events_drained
				    && !actionHasEnterFrameHandlers()
				    && !hasActiveTimers()
				    && !hasPlayingSounds()
				    && !hasActiveNetStreams()
				    && !swf_socket_pending()
				    && !swf_dialog_pending()
				    && !hasClipEnterFrameHandlers()) break;
			}
		}

#ifdef __EMSCRIPTEN__
		double frame_start = emscripten_get_now();
#endif
		// Flash clears the action stack at each frame boundary — see swf_core.c
		// for rationale. Key test: misc-swfc/stackscope.
		app_context->sp = INITIAL_SP;
		app_context->oldSP = 0;

		// Stage 3 mark-sweep collector (env-gated, default off) — after the
		// dprops drain + stack reset, where the VM is quiescent.
		swfGcMaybeCollect(app_context);

		current_frame = next_frame;
#ifdef OFFSCREEN_RENDER
		// Phase 4 (TRANSFORMED_BY_SCRIPT_WRAP_BACK), *deferred half*.
		// The previous tick's end-of-movie wrap asked for survives_rewind
		// cleanup semantics; apply the cleanup here, at the top of the tick
		// that actually replays frame 0, and let this tick's own
		// frame_funcs[current_frame] be the replay. This mirrors the
		// `target <= original_frame` arm of the catch-up loop below with
		// target = 0 (clear_after -> rewindCleanup -> catch_up_backward=1 ->
		// frame_funcs[0] -> catch_up_backward=0 -> cleanup_unplaced_after),
		// minus the in-tick frame execution that used to steal a tick.
		if (g_pending_natural_wrap_promote)
		{
			g_pending_natural_wrap_promote = 0;
			ng_display_clear_after(app_context, current_frame);
			actionRewindCleanup(app_context);
			extern int g_natural_wrap_cleanup_pending;
			g_natural_wrap_cleanup_pending = 1;
			catch_up_backward = 1;
			catch_up_target = current_frame;
			g_wrap_promote_active = 1;
		}

		// Capture scheduling: request a readback before this tick's frame
		// renders. With HEADLESS_RENDER_ENABLED on, this is a no-op unless
		// CAPTURE_TRIGGERS asks for the current tick or any last_frame.
		// Mirrors swf_headless.c line ~904.
		capture_tick_pre_frame();
#endif  // OFFSCREEN_RENDER — the _currentframe sync below is deliberately outside it

		// NOT gated on OFFSCREEN_RENDER. This block used to sit inside the
		// tick's `#ifdef OFFSCREEN_RENDER`, so browser-WASM and the native
		// windowed player — the two swf.c builds that define neither
		// NO_GRAPHICS nor OFFSCREEN_RENDER — never ran it and parked
		// `_root._currentframe` at 1 for the life of the movie however far the
		// playhead advanced. There is no parallel arm: `cpp -P -DUSE_WEBGPU`
		// counts ZERO `root_movieclip.currentframe =` writes in this file
		// against OFFSCREEN's one. It is pure computation over `current_frame`
		// and `g_frame_count`, so hoisting it leaves OFFSCREEN's statement
		// order untouched. Graded by
		// regression/avm1_root_identity_and_playhead's `cf:` column, which only
		// a browser build can flip (see
		// SWFRecompDocs/status/browser-root-side-gaps.md).
		//
		// Keep root _currentframe in sync with the natural advance. Mirrors
		// swf_core.c lines ~1425/1431 (which update at the END of each tick).
		// Without this, scripts running on frame N read a stale value left
		// behind by the most recent explicit goto, and pure natural advance
		// never updates _currentframe at all. Key test: avm1/goto_frame2 —
		// after gotoAndPlay(2) on frame 1 the natural advance to frames 2
		// and 3 leaves root_movieclip.currentframe stuck at 2 until the
		// next explicit goto.
		{
			extern MovieClip root_movieclip;
			// Hold _currentframe at the last frame once the playhead runs
			// past the timeline. A single-frame (or non-looping, quit_swf)
			// root with no stop() keeps "playing" — the funcs gate
			// (current_frame < g_frame_count) already stops frame scripts
			// re-running, but current_frame itself keeps incrementing, so
			// without this clamp _currentframe drifts (2,3,4,...) where Flash
			// holds it at the last frame. Multi-frame looping movies never
			// reach here: their last frame sets manual_next_frame (recompiler
			// swf.cpp:733) and wraps current_frame, so this clamp is a no-op
			// for them. Key game: Shopping Cart Hero (1-frame root, no stop();
			// Ruffle holds _currentframe=1 while swfrecomp drifted 2,3,4,...).
			size_t _disp_frame = (g_frame_count > 0 && current_frame >= g_frame_count)
			                   ? g_frame_count - 1 : current_frame;
			root_movieclip.currentframe = (int)_disp_frame + 1;
		}

#ifdef OFFSCREEN_RENDER  // resumes the tick block opened above

		// Clear g_defer_sprite_init at tick boundary. ng_executeGotoCatchUp
		// intentionally leaves it set so the calling frame's tagShowFrame
		// continues to defer sprite init for the rest of THAT tick. Without
		// a tick-boundary clear, a leak from a regular-frame-script
		// gotoAndPlay/gotoAndStop persists into subsequent frames, where it
		// suppresses process_sprite_needs_init for newly-placed sprites
		// (sprite_initialized stays 0 → onEnterFrame clip-actions never
		// dispatch). Mirrors swf_core.c (line ~951). Key test:
		// from_gnash/misc-ming.all/timeline_var_test — frame 1's
		// gotoAndPlay leaves the flag set, so the sprite placed on frame 3
		// never inits and its setTarget DoAction never pushes "setTarget"
		// into the ar array.
		{ extern int g_defer_sprite_init; g_defer_sprite_init = 0; }

		// Process deferred unloadMovie state (MC properties change on
		// next frame). Mirrors swf_core.c line ~918. Runs before
		// pending-removal finalize so any properties cleared by
		// unloadMovie are visible to the upcoming frame scripts.
		{
			extern void actionProcessDeferredUnloads(void);
			actionProcessDeferredUnloads();
		}

		// Finalize MCs marked for pending removal in the previous frame.
		// Mirrors swf_core.c's frame-start hook (around line 955). Without
		// this, AS-level removeMovieClip / tag RemoveObject2 with onUnload
		// handlers leave dynamic_props and var_map bindings live, so
		// `typeof(_root.clipN)` reports 'movieclip' on the next frame
		// instead of 'undefined'. Key test: avm1/unload.
		{
			extern void actionFinalizePendingRemovals(SWFAppContext* app_context);
			actionFinalizePendingRemovals(app_context);
		}

		// Reclaim dead child_mc_cache slots (mirrors swf_core.c / the
		// browser-WASM pass below; must follow the finalize cascade — see
		// actionReclaimDeadChildMCSlots).
		{
			extern void actionReclaimDeadChildMCSlots(void);
			actionReclaimDeadChildMCSlots();
		}

		// Promote pending MCL loads queued during the previous tick into
		// the "fire this tick" bucket. Mirrors swf_core.c line ~961.
		// Without this, MovieClipLoader.loadClip events queued for the
		// next tick never advance into the dispatch bucket and the
		// onLoadStart / onLoadComplete / onLoadInit listeners never
		// fire. Affects avm1/loadmovie_method, mcl_getprogress, etc.
		{
			extern void actionPromotePendingMCLLoads(SWFAppContext* app_context);
			actionPromotePendingMCLLoads(app_context);
		}
#endif

#if defined(__EMSCRIPTEN__) && !defined(OFFSCREEN_RENDER)
		// Browser-WASM: run the same pending-removal finalize + dead-child cascade
		// the OFFSCREEN block above runs. Without it, nested timeline-placed
		// children (auto-named "instanceN") of clips removed via removeMovieClip
		// are never invalidated — they orphan in child_mc_cache (parent depth =
		// INT_MIN but the child still live) and accumulate every frame. Metanet
		// "N"'s title demo continuously spawns + removes objects/particles whose
		// sub-clips leaked this way: the "instance" clip count climbed unbounded,
		// ratcheting per-frame render/scan cost until the game froze. The cascade
		// keys off parent->depth == INT_MIN, which removeMovieClip already sets, so
		// it reaches every orphan + grandchild. (Mirrors swf_core.c / the OFFSCREEN
		// path, which already call this each tick.)
		{
			extern void actionFinalizePendingRemovals(SWFAppContext* app_context);
			actionFinalizePendingRemovals(app_context);
		}

		// Browser-WASM dead-slot reclaim (see actionReclaimDeadChildMCSlots for
		// the full rationale — the pass originated here for the Metanet "N"
		// instanceN leak and is now shared with the CI modes).
		{
			extern void actionReclaimDeadChildMCSlots(void);
			actionReclaimDeadChildMCSlots();
		}
#endif

#ifndef OFFSCREEN_RENDER
		// Per-frame AS2 input dispatch.
		// In NO_GRAPHICS mode swf_core.c dispatches these per event; here we
		// dispatch per frame based on the flags + state set by render_webgpu.c's
		// callbacks. Runs BEFORE clearing the per-frame flags and BEFORE the
		// frame func so the transitions are visible to user scripts.
		//
		// Gated off in OFFSCREEN_RENDER because input_events_pump_tick (line
		// ~542) is the canonical event source there — it dispatches synchronously
		// AND sets mouse.clicked / .released, so a top-of-tick re-read here
		// would double-fire every mouse and key event. Key test: avm1/click_block
		// (last click was duplicated because the final EV_MOUSE_DOWN_LEFT in tick N
		// set mouse.clicked=1, and tick N+1's top dispatched MouseDown again).
		{
			// --- Mouse ---
			static float prev_stage_x = 0.0f;
			static float prev_stage_y = 0.0f;
			static int   prev_mouse_initialized = 0;
			float mx = app_context->mouse.stage_x;
			float my = app_context->mouse.stage_y;
			int moved = !prev_mouse_initialized || (mx != prev_stage_x) || (my != prev_stage_y);
			prev_stage_x = mx;
			prev_stage_y = my;
			prev_mouse_initialized = 1;
			if (moved) {
				// CLIP_EVENT_MOUSE_MOVE and CLIP_EVENT_ROLL_*/DRAG_* clip-action
				// dispatch. Matches swf_core.c's EV_MOUSE_MOVE handler (line ~501-503).
				// Without these, sprites with PlaceObject2 onClipEvent(mouseMove) /
				// onClipEvent(rollOver/Out/dragOver/Out) handlers never fire in
				// browser-WASM. Doodle Jump's button cursor and roll handlers
				// rely on dispatch_clip_event_roll.
				dispatch_clip_event_flag(app_context, CLIP_EVENT_MOUSE_MOVE);
				dispatch_clip_event_roll(app_context);
				actionEndVirtualHoverOnMouse(app_context);
				actionDispatchMCMouseMove(app_context);
				actionDispatchMCMouseMoveGlobal(app_context);
				actionResetHighlightForEvent(0); // 0=mouse_move
				// Extend textfield drag selection if mouse is moved while button
				// is held (matches swf_core.c EV_MOUSE_MOVE behavior).
				if (app_context->mouse.button_down) {
					extern void actionTextFieldDragSelect(SWFAppContext*);
					actionTextFieldDragSelect(app_context);
				}
			}
			if (app_context->mouse.clicked) {
				// CLIP_EVENT_MOUSE_DOWN / PRESS clip-action dispatch. Matches
				// swf_core.c's EV_MOUSE_DOWN_LEFT (line ~538/547/549). Without
				// these, PlaceObject2 onClipEvent(mouseDown)/onClipEvent(press)
				// handlers never fire in browser-WASM. Doodle Jump's button
				// CLIP_EVENT_RELEASE handlers also depend on PRESS being
				// dispatched first so clip_mc_pressed is set, gating the
				// subsequent RELEASE dispatch on mouseup.
				dispatch_clip_event_flag(app_context, CLIP_EVENT_MOUSE_DOWN);
				actionDispatchMouseDown(app_context);       // Mouse listener broadcast
				actionDispatchMCMouseDown(app_context);      // Per-MC AS2 dispatch
				actionMouseClickFocus(app_context);          // Focus acquisition
				dispatch_clip_event_press(app_context);      // onClipEvent(press)
				dispatch_clip_event_roll(app_context);       // ROLL→DRAG transition
				actionDispatchMCPress(app_context);          // onPress
				actionResetHighlightForEvent(1);             // 1=left_down
				actionClearVirtualHover();
			}
			if (app_context->mouse.released) {
				// CLIP_EVENT_MOUSE_UP / RELEASE clip-action dispatch. Matches
				// swf_core.c's EV_MOUSE_UP_LEFT (line ~566/573/575). The DJ
				// button handlers (clip_action_17/19/21/23) are
				// CLIP_EVENT_RELEASE actions that change game state on click.
				dispatch_clip_event_flag(app_context, CLIP_EVENT_MOUSE_UP);
				extern void actionTextFieldDragEnd(SWFAppContext*);
				actionTextFieldDragEnd(app_context);         // Finalize drag selection
				actionDispatchMouseUp(app_context);          // Mouse listener broadcast
				actionDispatchMCMouseUp(app_context);        // Per-MC AS2 dispatch
				dispatch_clip_event_release(app_context);    // onClipEvent(release)
				dispatch_clip_event_roll(app_context);       // DRAG→ROLL transition
				actionDispatchMCRelease(app_context);        // onRelease/onReleaseOutside
				actionResetHighlightForEvent(2);             // 2=left_up
			}

			// --- Keyboard ---
			// Detect key down/up transitions by comparing current keys.down[] to
			// the previous frame's snapshot. Each transition fires the focused-MC
			// dispatcher AND the global Key listener broadcast.
			static u8 prev_keys_down[256];
			static int prev_keys_initialized = 0;
			if (!prev_keys_initialized) {
				memset(prev_keys_down, 0, sizeof(prev_keys_down));
				prev_keys_initialized = 1;
			}
			int ctrl_held = (app_context->keys.down[17] != 0);
			extern int  dispatch_button_key_actions(SWFAppContext*, int code);
			for (int code = 0; code < 256; code++) {
				u8 cur = app_context->keys.down[code];
				u8 prev = prev_keys_down[code];
				// Edge latch from DOM callbacks lets us catch keys that were
				// pressed and released inside one 60Hz tick (e.g. Playwright's
				// keyboard.press) — the snapshot alone would miss them.
				u8 edge = app_context->keys.edge_down[code];
				app_context->keys.edge_down[code] = 0;
				if ((cur && !prev) || edge) {
					app_context->keys.last_key_down = code;
					app_context->keys.last_key_ascii = (code >= 32 && code <= 126) ? code : 0;
					// onClipEvent(keyDown) dispatch. Matches swf_core.c
					// EV_KEY_DOWN line ~616. Without it, sprites with
					// PlaceObject2 onClipEvent(keyDown) handlers never fire
					// under browser-WASM.
					dispatch_clip_event_flag(app_context, CLIP_EVENT_KEY_DOWN);
					actionDispatchKeyDownToFocused(app_context, code);
					actionDispatchKeyDown(app_context);
					// SWF5-era on(keyPress "X") button conditions. Walks the
					// display list, fires the first visible button whose
					// keyPress condition matches `code`. Snake (SWF5) steers
					// the snake via this path — without it the snake walks
					// straight into the wall. swf_core.c calls this from
					// EV_KEY_DOWN (line ~672/682); browser-WASM was missing it.
					// First button to handle stops propagation; only then do
					// we fall back to the focused-MC simulated press (Enter/Space).
					int key_press_handled = dispatch_button_key_actions(app_context, code);
					if (!key_press_handled)
						actionDispatchKeyPressToFocused(app_context, code);
					if (code == 9) { // Tab
						int shift_held = (app_context->keys.down[16] != 0);
						actionAdvanceTabFocus(app_context, shift_held);
					}
					// Text-control shortcuts. Equivalent to swf_core.c's
					// EV_TEXT_CONTROL events. Each handler internally checks
					// for a focused textfield and no-ops otherwise.
					if (ctrl_held) {
						switch (code) {
							case 65: actionTextControlSelectAll(app_context); break; // Ctrl+A
							case 67: actionTextControlCopy(app_context);      break; // Ctrl+C
							case 86: actionTextControlPaste(app_context);     break; // Ctrl+V
							case 88: actionTextControlCut(app_context);       break; // Ctrl+X
						}
					} else {
						switch (code) {
							case 8:  actionTextControlBackspace(app_context); break; // Backspace
							case 13: actionTextControlEnter(app_context);     break; // Enter
							case 46: actionTextControlDelete(app_context);    break; // Delete (forward)
							case 35: actionTextControlMoveEnd(app_context);   break; // End
							case 36: actionTextControlMoveHome(app_context);  break; // Home
							case 37: actionTextControlMoveLeft(app_context);  break; // Left arrow
							case 39: actionTextControlMoveRight(app_context); break; // Right arrow
						}
					}
				} else {
					u8 edge_up = app_context->keys.edge_up[code];
					app_context->keys.edge_up[code] = 0;
					if ((!cur && prev) || edge_up) {
						app_context->keys.last_key_down = code;
						app_context->keys.last_key_ascii = (code >= 32 && code <= 126) ? code : 0;
						// onClipEvent(keyUp) dispatch — symmetric to KEY_DOWN above.
						dispatch_clip_event_flag(app_context, CLIP_EVENT_KEY_UP);
						actionDispatchKeyUpToFocused(app_context, code);
						actionDispatchKeyUp(app_context);
					}
				}
				prev_keys_down[code] = cur;
			}

			// --- Text input (typed characters from emscripten keypress callback) ---
			// Drain the ring buffer populated by render_webgpu.c's on_keypress.
			extern int g_text_input_ring[];
			extern int g_text_input_ring_head;
			extern int g_text_input_ring_tail;
			while (g_text_input_ring_tail != g_text_input_ring_head) {
				int cp = g_text_input_ring[g_text_input_ring_tail];
				g_text_input_ring_tail = (g_text_input_ring_tail + 1) % 64;
				// Suppress text input for control characters that are also dispatched
				// via the text-control shortcuts above (Backspace, Tab, Enter, Esc).
				if (cp == 8 || cp == 9 || cp == 13 || cp == 27) continue;
				if (ctrl_held) continue; // Ctrl+letter shortcuts shouldn't also type the letter
				actionTextFieldInput(app_context, cp);
			}

			// --- Window focus lost ---
			extern int g_window_focus_lost;
			if (g_window_focus_lost) {
				g_window_focus_lost = 0;
				actionWindowFocusLost(app_context);
			}

			// --- IME compose / commit ---
			// Driven by JS listeners in render_webgpu.c (compositionupdate /
			// compositionend on the canvas). The compose call places the
			// composing text in the focused field with caret at the end;
			// the commit call finalizes it.
			extern char g_ime_compose_text[];
			extern char g_ime_commit_text[];
			extern int  g_ime_compose_pending;
			extern int  g_ime_commit_pending;
			extern void actionTextFieldImeCompose(SWFAppContext*, const char*, int, int);
			extern void actionTextFieldImeCommit(SWFAppContext*, const char*);
			if (g_ime_compose_pending) {
				g_ime_compose_pending = 0;
				int len = (int)strlen(g_ime_compose_text);
				actionTextFieldImeCompose(app_context, g_ime_compose_text, len, len);
			}
			if (g_ime_commit_pending) {
				g_ime_commit_pending = 0;
				actionTextFieldImeCommit(app_context, g_ime_commit_text);
			}

			// --- Focus highlight tick ---
			actionUpdateHighlightState();

			// --- Deferred roll-event flush ---
			// AS2 handlers fired above (e.g. Selection.setFocus from a Key
			// listener) can queue rollOver/rollOut events; drain them so they
			// fire before the next frame's events.
			actionFlushDeferredRollEvents(app_context);
		}

		app_context->mouse.clicked = 0;
		app_context->mouse.released = 0;
#endif

#ifdef OFFSCREEN_RENDER
		// Advance root-level sprites BEFORE root frame_func runs, mirroring
		// swf_core.c's two-phase order. Each sprite's frame_func queues its
		// script via actionQueueSpriteScript; the recompiler-emitted
		// actionDrainAllInPriorityOrder at the top of root frame_funcs[current_frame]
		// drains those scripts before the root's own DoAction. Without this
		// call, sprite frame_funcs (frame > 0) never run.
		{
			extern void advance_sprite_frames(SWFAppContext* app_context);
			extern int g_advance_defer_nested;
			g_advance_defer_nested = 1;
			advance_sprite_frames(app_context);
			g_advance_defer_nested = 0;
		}

		// Auto-advance attachMovie'd multi-frame clips (PROGRESS #15). Phase 1,
		// before the root frame func reads _currentframe. Skips clips attached
		// this tick (not yet promoted) and stopped clips. Mirrors swf_core.c.
		{
			extern void ng_advance_attached_clip_playheads(SWFAppContext* app_context);
			ng_advance_attached_clip_playheads(app_context);
		}

		// Pre-sync the AS-visible _currentframe of deferred nested sprites
		// (advanced in Phase 3 below) so a clip whose onEnterFrame fires in the
		// flush reads their post-advance value, matching Ruffle's
		// instantiation-ordered exec list (#10a). Writes only mc->currentframe;
		// the playhead + frame scripts stay in Phase 3 (advance_nested_sprite_frames).
		{
			extern void presync_nested_sprite_currentframe(SWFAppContext* app_context);
			presync_nested_sprite_currentframe(app_context);
		}
#endif

		// Mark ENTER_FRAME dispatch pending. tagFlushPendingEnterFrame is
		// called by the recompiler-emitted code right before each DoAction
		// (after RemoveObject) and dispatches clip event ENTER_FRAME +
		// AS2 onEnterFrame handlers. Mirrors swf_core.c line ~985.
		// Unguarded so browser-WASM also fires CLIP_EVENT_ENTER_FRAME
		// handlers — without this Doodle Jump's hero never bounces because
		// clip_action_26 (its physics handler) never dispatches.
		{
			extern int g_enterframe_flush_pending;
			g_enterframe_flush_pending = 1;
		}

		if (current_frame < g_frame_count && frame_funcs[current_frame] != NULL
#ifdef OFFSCREEN_RENDER
		    && (is_playing || manual_next_frame)
#endif
		    )
		{
			g_loopback_replay = g_loopback_replay_armed;
			g_loopback_replay_armed = 0;
#ifndef OFFSCREEN_RENDER
			// Browser-WASM Flash-accurate frame-entry gate. This loop re-runs
			// frame_funcs[current_frame] every tick (no is_playing gate — that
			// gate is OFFSCREEN_RENDER-only) because tagShowFrame, which pumps
			// rendering + sprite advance + button hover, lives inside the
			// frame_func body. But a frame's root DoAction must run only ONCE,
			// when the playhead ENTERS the frame — not on every parked re-run.
			// Otherwise a Stop() on a stopped frame re-fires each tick and
			// clobbers a sprite-driven _root.play() (Pong court sprite_54's
			// script_11 _root.play() vs frame_3's script_10 Stop; identical to
			// the preloader→title transition where sprite_9's _root.play()
			// races frame_0's script_0 Stop). We suppress only the
			// recompiler-emitted actionQueueScript (root DoAction) when the
			// playhead has not advanced since the last tick; tags + tagShowFrame
			// still run every tick so the sprite pump and renderer keep ticking.
			// Mirrors swf_core.c / OFFSCREEN_RENDER, which simply don't run the
			// frame_func at all while parked. See [[browser-wasm-frame-func-rerun]].
			{
				extern int g_suppress_root_doaction;
				static long s_wasm_last_doaction_frame = -1;
				g_suppress_root_doaction =
				    ((long)current_frame == s_wasm_last_doaction_frame) ? 1 : 0;
				s_wasm_last_doaction_frame = (long)current_frame;
			}
#endif
			frame_funcs[current_frame](app_context);
#ifndef OFFSCREEN_RENDER
			{ extern int g_suppress_root_doaction; g_suppress_root_doaction = 0; }
#endif
			g_loopback_replay = 0;
#ifdef OFFSCREEN_RENDER
			// Phase 4 deferred promotion, close-out half — mirrors the
			// catch-up loop's `catch_up_backward = 0;
			// ng_display_cleanup_unplaced_after(target)` tail.
			if (g_wrap_promote_active)
			{
				g_wrap_promote_active = 0;
				catch_up_backward = 0;
				ng_display_cleanup_unplaced_after(app_context, current_frame);
			}
#endif
#ifdef OFFSCREEN_RENDER
			// If a goto inside the script inlined the target frame's body
			// AND the recompiler-emitted last-frame wrap-back fired
			// afterward (signature: next_frame=0; manual_next_frame=1),
			// undo the wrap-back so the natural advance moves to
			// current_frame+1 (gotoAndPlay) or stays in place
			// (gotoAndStop). Without this, gotoAndPlay/Stop from inside
			// the last frame's script loops back to 0. Mirrors swf_core.c
			// (line ~1013). swf.c's next_frame-based natural advance also
			// needs next_frame resynced to current_frame: the wrap-back set
			// next_frame=0, so the loop-bottom `next_frame += 1` would land
			// on frame 1 (re-running the target script) instead of
			// current_frame+1. Setting next_frame=current_frame turns the
			// natural advance into current_frame+1 (gotoAndPlay) or stay
			// (gotoAndStop, is_playing=0). Key test:
			// from_shumway/timeline/timeline_as2_1 — frame_4's script
			// gotoAndPlay(1) inlines script_1 via ng_executeGotoCatchUp,
			// the wrap-back then fires, and without the undo the timeline
			// loops to frame 0 instead of advancing to frame 2's
			// FSCommand:quit.
			if (g_goto_inlined_in_caller_frame)
			{
				extern int g_deferred_root_goto;
				g_goto_inlined_in_caller_frame = 0;
				if (manual_next_frame && next_frame == 0 && !g_deferred_root_goto) {
					manual_next_frame = 0;
					next_frame = current_frame;
				}
			}
#endif
		}

#ifdef OFFSCREEN_RENDER
		// Safety net: if the frame_func gate above declined to run (root
		// stopped / past-last-frame), the deferred promotion's close-out
		// never fired. Never leave catch_up_backward latched across a tick.
		if (g_wrap_promote_active)
		{
			g_wrap_promote_active = 0;
			catch_up_backward = 0;
			ng_display_cleanup_unplaced_after(app_context, current_frame);
		}

		// Fallback flush: if frame_func didn't run (root stopped,
		// past-last-frame, ...), flush enter_frame here so clip event
		// ENTER_FRAME and onEnterFrame handlers still fire.
		{
			extern int g_enterframe_flush_pending;
			extern void tagFlushPendingEnterFrame(SWFAppContext*);
			if (g_enterframe_flush_pending)
				tagFlushPendingEnterFrame(app_context);
		}

		// Mark dynamic MCs (createEmptyMovieClip, etc. — no display_obj)
		// as eligible for the NEXT tick's enterFrame dispatch. Mirrors
		// swf_core.c line ~1100. Without this, MCs created by DoAction
		// scripts during this tick are never marked, so
		// actionDispatchEnterFrameHandlers skips them on subsequent
		// ticks (it checks mc_enterframe_eligible for MCs with NULL
		// display_obj). Symptom: create_empty_movie_clip's
		// mc2.onEnterFrame never fires from the main loop, was only
		// firing via the post-quit drain (and firing too many times
		// there). Key test: avm1/create_empty_movie_clip.
		{
			extern void actionMarkDynamicMCsEnterFrameEligible(void);
			actionMarkDynamicMCsEnterFrameEligible();
		}

		// Promote this tick's freshly-attached multi-frame clips to active so the
		// pump advances them STARTING NEXT tick (PROGRESS #15). After the deferred
		// attach-init drain (inside frame_func) so a clip's frame-1 this.stop()
		// has applied before it is ever advanced. Mirrors swf_core.c.
		{
			extern void ng_promote_attached_playheads(void);
			ng_promote_attached_playheads();
		}

		// Phase 3: advance nested sprite children (deferred from Phase 1
		// via g_advance_defer_nested above).
		{
			extern void advance_nested_sprite_frames(SWFAppContext* app_context);
			advance_nested_sprite_frames(app_context);
		}
		// Drain any sprite SCRIPT entries queued during advance_*_sprite_frames
		// that the recompiler-emitted in-frame drain didn't cover.
		actionDrainActionQueueByKind(app_context, AQ_KIND_SCRIPT);

		// Apply any sprite-frame-script gotoAndPlay that targeted its OWN sprite
		// WITHIN this tick (#10). Sprite scripts only run in the drain above, so
		// the manual flag is set too late for the advance passes; applying it here
		// (rebuild to target, resume at target+1) avoids the one-tick-late goto +
		// stutter the deferred top-of-loop path produces. Mirrors swf_core.c.
		// OFFSCREEN_RENDER only (the graphics-native test mode); browser-WASM keeps
		// the established deferred path — it is not covered by the CI suites and
		// has its own delicate sprite-timing paths.
#if defined(OFFSCREEN_RENDER)
		{
			extern void ng_apply_pending_sprite_self_gotos(SWFAppContext* app_context);
			ng_apply_pending_sprite_self_gotos(app_context);
		}
#endif

		// Flush deferred rollOver/rollOut events from Selection.setFocus()
		// calls that occurred during frame scripts. These fire
		// asynchronously (after script completes) but before input events
		// are processed. Mirrors swf_core.c (line ~1105) / swf_headless.c
		// (line ~1062). input_events_pump_tick also flushes between
		// queued events, but tests with no input.json need this call to
		// drain rolls queued by frame-script Selection.setFocus.
		// Key test: avm1/selection_handlers — Selection.setFocus calls
		// from script_0 queue rollOver/rollOut on text/button/clip that
		// never fired without this.
		actionFlushDeferredRollEvents(app_context);

		// Drive file-driven test input (KEY_DOWN, MOUSE_MOVE, etc.) AFTER
		// frame scripts have set up their listeners. Mirrors swf_core.c's
		// placement (around line 1109). No-op when no event file is loaded.
		{
			extern void input_events_pump_tick(SWFAppContext* app_context);
			input_events_pump_tick(app_context);
		}

		// Sockets: same cadence as swf_core.c (~line 1240) — deliver the
		// actions queued last tick, then pump the socket.json script. No-op
		// with no script loaded.
		{
			extern void swf_socket_tick(int owner);
			swf_socket_tick(1);
			// File dialogs resolve in the SAME tick as the frame that
			// opened them (Ruffle polls the executor once the frame is
			// over), unlike socket actions, which queue for the next.
			swf_dialog_pump();
		}

		// Goto catch-up: when a script-initiated goto (e.g. mc.gotoAndStop("/:N")
		// with a force_root path) called ng_executeGotoTagsOnly during this
		// tick, it left goto_from_action + manual_next_frame + g_deferred_root_goto
		// set so the main loop runs the target frame's scripts. Without this
		// block, manual_next_frame gets cleared at end-of-tick (line ~613) and
		// the next tick sees is_playing=0 (from gotoAndStop), so funcs[target]
		// never runs. Mirrors swf_core.c outer catch-up loop (line ~1195+).
		// Key test: avm1/goto_frame_number.
#ifdef OFFSCREEN_RENDER
		// Phase 4 (TRANSFORMED_BY_SCRIPT_WRAP_BACK), ported from swf_core.c
		// (~line 1257): promote a natural backward wrap that leaves stale
		// later-frame display-list entries into an implicit backward goto so
		// the catch-up loop below applies survives_rewind semantics
		// (tagPlaceObject2 modify-vs-replace, unsurvivor cleanup via
		// g_natural_wrap_cleanup_pending in actionDrainOnloadAndScript).
		// Without this, the light wrap path (~line 1257) re-places
		// non-surviving entries fresh, resetting script-written transforms
		// and skipping depth renames. Key test:
		// place_and_remove_object_insane_test (the last strict no-graphics /
		// graphics parity gap). OFFSCREEN_RENDER-only for now: browser-WASM
		// keeps the light wrap path its restart flows were tuned on (see the
		// Snake note above ng_display_clear_after) pending probe coverage.
		extern int g_force_quit;
		if (manual_next_frame && !goto_from_action && next_frame < current_frame
		    && current_frame + 1 == g_frame_count
		    && !g_force_quit)
		{
			int has_stale = 0;
			for (size_t d = 1; d <= max_depth && d < 16384; d++) {
				if (display_list[d].char_id != 0
				    && display_list[d].placed_at_frame > next_frame) {
					has_stale = 1;
					break;
				}
			}
			if (has_stale) {
				// DEFERRED BY ONE TICK (see g_pending_natural_wrap_promote).
				// Setting goto_from_action here made the catch-up loop below
				// run frame_funcs[0] inside THIS tick, before
				// capture_tick_after_events / capture_tick_post_frame — so the
				// movie's last frame was never displayed and every later
				// capture was one tick early (simple_loop_test rendered with
				// period 3 where Flash/Ruffle have period 4). The promotion
				// exists for the survives_rewind *cleanup* semantics, not to
				// re-execute the frame early, so hand the cleanup to the top
				// of the next tick and let that tick's own frame_funcs[0] be
				// the replay. The light wrap path below is suppressed for the
				// same reason (it would clear the display list the deferred
				// half still wants to diff against).
				g_pending_natural_wrap_promote = 1;
			}
		}
#endif
		{
			int _goto_retry_limit = 16;
			while (goto_from_action && manual_next_frame && _goto_retry_limit-- > 0)
			{
				size_t original_frame = current_frame;
				size_t target = next_frame;
				manual_next_frame = 0;
				goto_from_action = 0;

				ng_display_clear_after(app_context, target);

				catch_up_mode = 1;
				if (target <= original_frame)
				{
					actionRewindCleanup(app_context);
					catch_up_backward = 1;
					catch_up_target = target;
					for (size_t f = 0; f < target && f < g_frame_count; f++)
					{
						current_frame = f;
						if (frame_funcs[f]) frame_funcs[f](app_context);
					}
					catch_up_mode = 0;
					if (target < g_frame_count)
					{
						current_frame = target;
						if (frame_funcs[target]) frame_funcs[target](app_context);
					}
					catch_up_backward = 0;
					ng_display_cleanup_unplaced_after(app_context, target);
				}
				else
				{
					for (size_t f = original_frame + 1; f < target && f < g_frame_count; f++)
					{
						current_frame = f;
						if (frame_funcs[f]) frame_funcs[f](app_context);
					}
					catch_up_mode = 0;
					if (target < g_frame_count)
					{
						current_frame = target;
						if (frame_funcs[target]) frame_funcs[target](app_context);
					}
				}
				current_frame = target;
				g_deferred_root_goto = 0;
			}
		}

		// Process deferred failed-load state and direct loadMovie inits
		// queued by this tick's frame/sprite scripts. Mirrors swf_core.c
		// (lines ~1282-1300). Without actionFirePendingDirectLoads, a
		// loadMovie call from frame_0 never runs the child's init+frame0
		// — drops the "Child movie loaded!" trace line in loadmovie /
		// loadmovie_flashvars / loadmovie_method / loadmovie_registerclass
		// / loadmovie_replace_root / loadmovie_var_persistence and the
		// loadmovienum cluster. Also unblocks the "After load movie:"
		// tail of focusrect_property_swf{5,6,7}.
		{
			extern void actionProcessDeferredFailedLoads(void);
			extern void actionFirePendingDirectLoads(SWFAppContext*);
			extern int g_pending_direct_load_count;
			actionProcessDeferredFailedLoads();
			int dl_guard = 0;
			while (g_pending_direct_load_count > 0 && dl_guard++ < 32)
				actionFirePendingDirectLoads(app_context);
		}

		// Advance multi-frame _levelN loads (loadMovieNum into a level slot
		// not in display_list — advance_sprite_frames doesn't reach those).
		// Mirrors swf_core.c line ~1299.
		{
			// The MCL Phase-3 slot, mirroring swf_core.c: after this tick's
			// enterFrame broadcast, before the loaded movie's own advance.
			// See actionDrainPendingLoadInits / regression/avm1_mcl_load_tick.
			actionDrainPendingLoadInits(app_context);
			extern void actionAdvancePlayingLevels(SWFAppContext*);
			actionAdvancePlayingLevels(app_context);
		}

		// Mirror swf_headless.c (line ~1208) / swf_core.c (line ~1309): fire
		// AS2 setInterval/setTimeout callbacks, LoadVars onData, and end-of-frame
		// hooks. Without processTimers, setInterval-driven callbacks never run
		// in graphics-native, so any test relying on them (Dejagnu's
		// setInterval(checkIt, ...) — every actionscript.all test) gets stuck
		// because the only checkIt invocation path left (onEnterFrame) bails
		// inside enterFrame dispatch via the g_inside_enterframe_dispatch guard
		// in actionGotoFrame.
		{
			extern void processTimers(SWFAppContext*, double);
			extern void processLoadVarsLoads(SWFAppContext*);
			extern void processSoundPlayback(SWFAppContext*, double);
			extern void processNetStreams(SWFAppContext*, double);
			extern void avm1AmfFlushNetConnections(SWFAppContext*);
			extern void processLocalConnectionMessages(SWFAppContext*);
			extern void actionFlushPendingOnLoads(SWFAppContext*);
			double frame_duration_ms = (app_context->fps > 0) ? (1000.0 / app_context->fps) : 83.33;
			actionFlushPendingOnLoads(app_context);
			processTimers(app_context, frame_duration_ms);
			processLoadVarsLoads(app_context);
			processSoundPlayback(app_context, frame_duration_ms);
			processNetStreams(app_context, frame_duration_ms);
			// Mirrors swf_core.c: NetConnection packet drain before LC delivery.
			avm1AmfFlushNetConnections(app_context);
			processLocalConnectionMessages(app_context);
			actionFlushPendingOnLoads(app_context);
		}

		// Drain MCL pending loads queued during frame scripts / timers /
		// onLoadInit chains. Mirrors swf_core.c lines ~1336-1345.
		{
			extern void actionFirePendingLoadInits(SWFAppContext*);
			extern int g_pending_mcl_load_count_this_tick;
			int mcl_guard = 0;
			while (g_pending_mcl_load_count_this_tick > 0 && mcl_guard++ < 32)
				actionFirePendingLoadInits(app_context);
		}

		// Last-tick MCL drain: when this is the final tick (tick_count
		// reaches max_ticks), promote any _next_tick loads and drain so
		// listeners get to run. Mirrors swf_core.c lines ~1353-1358.
#ifdef MAX_FRAMES
		if (tick_count >= max_ticks)
		{
			extern void actionPromotePendingMCLLoads(SWFAppContext*);
			extern void actionFirePendingLoadInits(SWFAppContext*);
			extern int g_pending_mcl_load_count_this_tick;
			extern int g_pending_mcl_load_count_next_tick;
			if (g_pending_mcl_load_count_next_tick > 0) {
				actionPromotePendingMCLLoads(app_context);
				int mcl_guard = 0;
				while (g_pending_mcl_load_count_this_tick > 0 && mcl_guard++ < 32)
					actionFirePendingLoadInits(app_context);
			}
			// …and the deferred Phase 3, which has no next tick to fire in.
			actionDrainPendingLoadInitsFinal(app_context);
		}
#endif

		// Re-render after events so iteration captures reflect post-event
		// display state (Tab focus, button hover, etc.). Mirrors
		// swf_headless.c line ~1249.
		capture_tick_after_events(app_context);

		// Save captures whose tick has arrived (iteration matches; pending
		// fscommand). last_frame captures wait until end-of-loop. Mirrors
		// swf_headless.c line ~1267.
		capture_tick_post_frame();
#endif

#if !defined(OFFSCREEN_RENDER) && defined(__EMSCRIPTEN__)
		// Browser-WASM per-tick timer pump. The OFFSCREEN_RENDER block above
		// runs processTimers inside its #ifdef; browser-WASM never did, so
		// AS2 setInterval / setTimeout callbacks never fired here — e.g.
		// Tetris's `timeout = setInterval(down, speed)` falling-piece driver,
		// which is the game's entire fall mechanic (without it the board never
		// animates and the game is unplayable). Sprite advance, enterFrame
		// dispatch (tagFlushPendingEnterFrame), and LOAD drains already run per
		// tick inside tagShowFrame (called from each frame_func), so we add ONLY
		// the timer-side pumps that have no browser-WASM equivalent. Bracketed
		// by actionFlushPendingOnLoads like the OFFSCREEN cluster so onLoad
		// chains queued by a timer callback drain the same tick.
		//
		// The LOAD drains referred to above are tagShowFrame's AQ_KIND_LOAD
		// queue (clip-event onLoad) — NOT the loaded-MOVIE drains. Those live
		// in the OFFSCREEN_RENDER block above and in tagShowFrame arms gated
		// `NO_GRAPHICS || OFFSCREEN_RENDER`, so browser-WASM had none of them:
		// a `holder.loadMovie("child.swf")` queued a pending direct load that
		// nothing ever fired, and a loaded movie's playhead was never advanced.
		// Measured 2026-09-04 on a browser build of
		// regression/avm1_child_timeline_advance: no child frame ever ran
		// (`_currentframe` 0, `_totalframes` 1) where native traces frames
		// 1..5 and a wrap. The three drains below mirror the OFFSCREEN cluster
		// (and swf_core.c ~1455/1463/1517) in the same order and with the same
		// guards; they are strict no-ops when no load is pending.
		{
			extern void actionProcessDeferredFailedLoads(void);
			extern void actionFirePendingDirectLoads(SWFAppContext*);
			extern int g_pending_direct_load_count;
			actionProcessDeferredFailedLoads();
			int dl_guard = 0;
			while (g_pending_direct_load_count > 0 && dl_guard++ < 32)
				actionFirePendingDirectLoads(app_context);
		}
		{
			// The MCL Phase-3 slot, mirroring swf_core.c: after this tick's
			// enterFrame broadcast, before the loaded movie's own advance.
			// See actionDrainPendingLoadInits / regression/avm1_mcl_load_tick.
			actionDrainPendingLoadInits(app_context);
			extern void actionAdvancePlayingLevels(SWFAppContext*);
			actionAdvancePlayingLevels(app_context);
		}
		{
			extern void processTimers(SWFAppContext*, double);
			extern void actionFlushPendingOnLoads(SWFAppContext*);
			double timer_dur_ms = (app_context->fps > 0) ? (1000.0 / app_context->fps) : 83.33;
			actionFlushPendingOnLoads(app_context);
			processTimers(app_context, timer_dur_ms);
			actionFlushPendingOnLoads(app_context);
		}
		// MovieClipLoader.loadClip: loadClip() only queues;
		// actionFirePendingLoadInits is what fires onLoadStart/Progress/
		// Complete/Init and links the movie in. Bounded like the OFFSCREEN and
		// swf_core.c drains — a chain of loadClips can re-queue.
		{
			extern void actionFirePendingLoadInits(SWFAppContext*);
			extern int g_pending_mcl_load_count_this_tick;
			int mcl_guard = 0;
			while (g_pending_mcl_load_count_this_tick > 0 && mcl_guard++ < 32)
				actionFirePendingLoadInits(app_context);
		}
#endif
		// A goto issued from a timer/event callback (setInterval, etc.) queues the
		// target frame's DoAction via ng_executeGotoCatchUp's drain-suppressed
		// funcs[target] call. Unlike a goto from a frame script — where the
		// caller's SHOW_FRAME drain runs the target script this tick — a
		// timer-callback goto leaves it orphaned in the queue. A gotoAndPlay's
		// is_playing=1 then survives into the natural advance below before the
		// target frame's own stop() runs, over-advancing one frame. Drain any
		// orphaned ONLOAD/SCRIPT entries now so the target frame's actions
		// (e.g. stop()) settle is_playing before the advance. g_aq_count is
		// normally 0 here (frame funcs drain at SHOW_FRAME), so this is a no-op
		// on the common path. Mirrors swf_core.c (~line 1533-1548) — this is
		// the graphics/OFFSCREEN_RENDER + browser-WASM twin of that drain, and
		// it is deliberately OUTSIDE the mode #ifdefs so both loops get it.
		// Key test: from_gnash/misc-swfc.all/gotoFrameFromInterval2 (the last
		// graphics/no-graphics parity gap in the corpus).
		{
			extern size_t actionActionQueuePending(void);
			if (actionActionQueuePending() > 0)
				actionDrainAllInPriorityOrder(app_context);
		}
		if (manual_next_frame)
		{
			// Natural backward wrap (recompiler-emitted at end of last frame
			// when total frame count > 1): invalidate cached MCs and clear
			// display entries placed at frames > target so names placed only
			// at later frames don't bleed into the next loop's frame 0.
			// Mirrors swf_core.c (line ~1395). Goto-from-action wraps go
			// through ng_executeGotoCatchUp / the outer catch-up loop above,
			// which handle their own cleanup. Key test (OFFSCREEN_RENDER):
			// avm1/default_names — without this, the second-iteration
			// auto-instance counter is short by the number of stale-depth
			// modifies that would have been fresh placements. Key
			// browser-WASM scenario: Snake game-over → menu → restart. The
			// game-over button's Play() advances the timeline; frame 56
			// natural-wraps to frame 0; without this cleanup the gameplay
			// display-list entries (Snake/Food at depths 6/8 plus the
			// SCORE/LEVEL textfields at 3/4/5) survive into the menu —
			// "SCORE:" lingers on the title screen and a stale-depth black
			// square paints over the countdown. After this widened cleanup
			// the menu re-renders correctly. (A separate clone-rendering
			// issue may still leave the snake invisible on restart even
			// after this cleanup — tracked as a follow-up.)
			//
			// OFFSCREEN_RENDER skip on the last tick of the run: the
			// wrap-back is preparation for the next tick's frame_0 which
			// will re-place the cleared entries. If no next tick is going
			// to run, the cleanup just destroys the display_list and leaves
			// last_frame captures rendering blank. Key test:
			// from_shumway/avm1/text-bind. (Per-iteration captures happen
			// *before* this wrap-back so they're unaffected.) Browser-WASM
			// runs forever — no "last tick" concern there.
#if defined(OFFSCREEN_RENDER) && defined(MAX_FRAMES)
			int _wrap_is_last_tick = (tick_count >= max_ticks);
#else
			int _wrap_is_last_tick = 0;
#endif
#ifdef OFFSCREEN_RENDER
			// The Phase-4 deferred promotion owns this wrap — it will run the
			// survives_rewind cleanup at the top of the next tick. Running the
			// light path here as well would clear the display list a tick
			// early (the tick's captures already happened, but the deferred
			// half's modify-vs-replace decisions read the pre-wrap entries).
			if (g_pending_natural_wrap_promote) _wrap_is_last_tick = 1;
#endif
			if (!goto_from_action && next_frame < current_frame && !_wrap_is_last_tick)
			{
				extern void actionInvalidateCachedMovieClip(SWFAppContext*, const char*, int);
				for (size_t d = 1; d <= max_depth && d < 16384; d++)
				{
					if (display_list[d].char_id != 0 &&
					    display_list[d].placed_at_frame > next_frame &&
					    display_list[d].instance_name != NULL)
					{
						actionInvalidateCachedMovieClip(app_context,
						    display_list[d].instance_name, (int)d);
					}
				}
				ng_display_clear_after(app_context, next_frame);
				ng_display_cleanup_unplaced_after(app_context, next_frame);
				// The upcoming re-run of frame_funcs[next_frame] is a rewind
				// (loop wrap); arm the flag so tagPlaceObject2 modifies rather
				// than refuses a Place on a swapDepths-occupied depth.
				g_loopback_replay_armed = 1;
			}
			// Goto/play command set next_frame directly
			manual_next_frame = 0;
		}
		else if (is_playing)
		{
			next_frame += 1;
			// Wrap around when reaching the end. SWFs loop by default in
			// real playback; but in test mode (OFFSCREEN_RENDER), the
			// recompiler emits its own natural-wrap as
			// `manual_next_frame=1; next_frame=0` at the end of the last
			// frame and pairs it with `quit_swf=1`. Auto-wrapping here on
			// top of that re-runs frame 0 every tick whenever something
			// else is keeping the loop alive (a playing child sprite,
			// pending MCL load, etc.), producing duplicated trace output
			// — the loadmovie_var_persistence cluster of failures.
#ifndef OFFSCREEN_RENDER
			if (next_frame >= g_frame_count)
				next_frame = 0;
#endif
		}
		// else: stopped — stay on current frame
#ifdef __EMSCRIPTEN__
		double render_poll_start = emscripten_get_now();
#endif
		bad_poll |= renderer_poll(app_context);
#ifdef __EMSCRIPTEN__
		double render_poll_ms = emscripten_get_now() - render_poll_start;
#endif

		// After-tick hook (for test harness / display bridge)
		{
			typedef void (*AfterTickHandler)(SWFAppContext*, int);
			extern AfterTickHandler g_after_tick_handler;
			static int _tick_count = 0;
			_tick_count++;
			if (g_after_tick_handler)
				g_after_tick_handler(app_context, _tick_count);
		}

#ifdef __EMSCRIPTEN__
		double now_ms = emscripten_get_now();
		double elapsed = now_ms - frame_start;
		double eff_budget_ms = frame_budget_ms;
		if (g_debug_frame_floor_ms > eff_budget_ms) eff_budget_ms = g_debug_frame_floor_ms;
		int perf_uncapped = swf_perf_report(elapsed, eff_budget_ms, render_poll_ms,
		                                    (int)swfMemLiveObjects(), (int)swfMemLiveArrays());
		if (next_due_ms == 0.0) next_due_ms = frame_start;  // anchor to first frame
		next_due_ms += eff_budget_ms;                        // when this frame is due to end
		if (perf_uncapped) {
			emscripten_sleep(0);
			next_due_ms = now_ms;                            // don't bank credit while uncapped
		} else {
			double remain_ms = next_due_ms - now_ms;
			if (remain_ms > 0.0) {
				emscripten_sleep((u32)(remain_ms + 0.5));   // round to nearest ms; deadline self-corrects
			} else {
				emscripten_sleep(0);
				// More than a full frame behind (heavy frame / backgrounded tab):
				// resync so we don't burst a catch-up storm.
				if (remain_ms < -eff_budget_ms) next_due_ms = now_ms;
			}
		}
#endif
		quit_swf |= bad_poll;
	}

#ifdef OFFSCREEN_RENDER
frame_loop_exit:
	// Deactivate timeout longjmp (jmp_buf is stack-local, must not be used after return).
	actionSetTimeoutJmp(NULL);
#endif

	if (bad_poll)
	{
		return;
	}

#ifndef OFFSCREEN_RENDER
	// Post-quit drain loop: in browser/emscripten mode the SWF stays "alive"
	// after its timeline ends — onEnterFrame handlers and sprite timelines
	// keep firing until the user closes the window. renderer_poll returns
	// nonzero on window close.
	//
	// Skipped in OFFSCREEN_RENDER (test mode): swf_core.c has no equivalent
	// drain loop. The main loop's quit_swf gate already waits on all pending
	// work (sprites, timers, MCL loads, onEnterFrame handlers), and now that
	// actionMarkDynamicMCsEnterFrameEligible runs at each tick boundary,
	// dynamic-MC handlers fire from the main loop too. Running the drain
	// here on top caused duplicate onEnterFrame dispatches (e.g.
	// create_empty_movie_clip's mc2.onEnterFrame firing twice).
	while (!renderer_poll(app_context))
	{
#ifdef __EMSCRIPTEN__
		double frame_start2 = emscripten_get_now();
#endif
		// Tick boundary = VM quiescent (see main loop).
		actionDrainDpropsReleases(app_context);

		// Stage 3 mark-sweep collector (env-gated, default off). The
		// quiescence check inside bails if this loop ever runs with leftover
		// eval-stack state.
		swfGcMaybeCollect(app_context);

		// Set enterframe_eligible on all initialized sprites so
		// actionDispatchEnterFrameHandlers doesn't skip them.
		{
			extern void set_enterframe_eligible_recursive(DisplayObject*, size_t);
			set_enterframe_eligible_recursive(display_list, max_depth);
		}

		// Dispatch AS2 onEnterFrame handlers (child MCs + root dynamic_props)
		actionDispatchEnterFrameHandlers(app_context);
		// Dispatch root onEnterFrame from var_map (DefineFunction/SetVariable path)
		actionDispatchRootVarMapEnterFrame(app_context);

		tagShowFrame(app_context);
#ifdef __EMSCRIPTEN__
		double now2_ms = emscripten_get_now();
		double elapsed2 = now2_ms - frame_start2;
		double eff_budget2_ms = frame_budget_ms;
		if (g_debug_frame_floor_ms > eff_budget2_ms) eff_budget2_ms = g_debug_frame_floor_ms;
		int perf_uncapped2 = swf_perf_report(elapsed2, eff_budget2_ms, 0.0,  // drain loop: present folded into renderer_poll gate
		                                     (int)swfMemLiveObjects(), (int)swfMemLiveArrays());
		if (next_due_ms == 0.0) next_due_ms = frame_start2;  // continue the main-loop schedule
		next_due_ms += eff_budget2_ms;
		if (perf_uncapped2) {
			emscripten_sleep(0);
			next_due_ms = now2_ms;
		} else {
			double remain2_ms = next_due_ms - now2_ms;
			if (remain2_ms > 0.0) {
				emscripten_sleep((u32)(remain2_ms + 0.5));
			} else {
				emscripten_sleep(0);
				if (remain2_ms < -eff_budget2_ms) next_due_ms = now2_ms;
			}
		}
#endif
	}
#endif
}

// Pre-declare every child movie's static bitmaps into the renderer's slot
// table, so build_static_bitmap_pools can dimension the size-class pools for
// all of them at the ROOT's finalizeBitmaps and never has to be re-entered.
//
// A child's own defineBitmap calls arrive at loadMovie time, long after the
// pools are latched; making the pools growable and the finalize re-entrant was
// the alternative, and it is a much riskier shape (destroying textures a
// recorded draw still references). The descriptors are static data the
// recompiler already knows -- see `bitmap_descs` in draws.c -- so the whole
// question can be answered before the first frame.
//
// Cost: a bundled child's bitmaps are resident even if the child is never
// loaded. That is the same trade the root's own bitmaps already make.
void ng_predeclareChildBitmaps(void)
{
	for (int i = 0; ; i++) {
		MovieEntry* e = getMovieEntryAt(i);
		if (e == NULL) break;
		if (e->movie_id == 0 || e->bitmap_count == 0) continue;
		if (e->bitmap_descs_ptr == NULL || e->bitmap_data_ptr == NULL) continue;
		for (size_t k = 0; k < e->bitmap_count; k++) {
			const u32* d = e->bitmap_descs_ptr + k * 4;
			if (d[2] == 0 || d[3] == 0) continue;   // degenerate: keeps its slot
			renderer_predeclare_bitmap(context, e->bitmap_base + (u32) k,
			                           e->bitmap_data_ptr + d[0],
			                           d[2], d[3]);
		}
	}
}

void swfStart(SWFAppContext* app_context)
{
	// Multi-SWF render slice: fold every linked child movie's geometry and
	// style arrays into the root's, so what the renderer uploads below covers
	// all of them. No-op unless a child movie carries render tables.
	ng_buildMovieRenderTables(app_context);

	context = renderer_new();

	// The renderer's width/height are the RENDER TARGET, which is the declared
	// viewport when the harness gave us one and the stage size otherwise.
	// app_context->width/height stay stage pixels — see SWFAppContext.
	context->width = app_context->render_width > 0
		? app_context->render_width : app_context->width;
	context->height = app_context->render_height > 0
		? app_context->render_height : app_context->height;
	context->stage_scale = app_context->stage_scale > 0.0f
		? app_context->stage_scale : 1.0f;

	context->stage_to_ndc = app_context->stage_to_ndc;

	context->bitmap_count = app_context->bitmap_count;
	context->bitmap_highest_w = app_context->bitmap_highest_w;
	context->bitmap_highest_h = app_context->bitmap_highest_h;

	context->shape_data = app_context->shape_data;
	context->shape_data_size = app_context->shape_data_size;
	context->transform_data = app_context->transform_data;
	context->transform_data_size = app_context->transform_data_size;
	context->color_data = app_context->color_data;
	context->color_data_size = app_context->color_data_size;
	context->uninv_mat_data = app_context->uninv_mat_data;
	context->uninv_mat_data_size = app_context->uninv_mat_data_size;
	context->gradient_data = app_context->gradient_data;
	context->gradient_data_size = app_context->gradient_data_size;
	context->bitmap_data = app_context->bitmap_data;
	context->bitmap_data_size = app_context->bitmap_data_size;
	context->cxform_data = app_context->cxform_data;
	context->cxform_data_size = app_context->cxform_data_size;

	// Allocate stack into app_context (use system malloc, not heap - stack is allocated before heap_init)
	app_context->stack = (char*) malloc(INITIAL_STACK_SIZE);
	app_context->sp = INITIAL_SP;
	app_context->oldSP = 0;

	quit_swf = 0;
	bad_poll = 0;
	next_frame = 0;

	// Store frame info globally for ActionCall opcode
	g_frame_funcs = app_context->frame_funcs;
	g_frame_count = app_context->frame_count;

	initTime(app_context);
	initMap();

	// Initialize heap allocator (must be before renderer_init which uses HALLOC)
	if (!heap_init(app_context, 0)) {  // 0 = use default size (64 MB)
		fprintf(stderr, "Failed to initialize heap allocator\n");
		return;
	}

	// Allocate display state via HCALLOC so grow_ptr's HALLOC + FREE pair works.
	// Must use HCALLOC (heap allocator) so grow_ptr can FREE them without
	// passing a system-malloc'd pointer to heap_free. Mirrors swf_core.c
	// (line ~807) / swf_headless.c (line ~845). With system malloc here, the
	// first display_list/dictionary growth during execution called HALLOC for
	// the new buffer and FREE on the old malloc'd one, leaving an "invalid
	// pointer" entry that heap_shutdown traversal hits at exit — surfaces as
	// "SIGSEGV (output matches)" on any test that grows the display list.
	dictionary = HCALLOC(INITIAL_DICTIONARY_CAPACITY, sizeof(Character));
	display_list = HCALLOC(INITIAL_DISPLAYLIST_CAPACITY, sizeof(DisplayObject));
	if (!dictionary || !display_list) {
		fprintf(stderr, "Failed to allocate display state\n");
		return;
	}

	// audio_output_init MUST run before renderer_init so the Web Audio
	// AudioContext is created while the user gesture (click) is still active.
	// renderer_init calls emscripten_sleep() which consumes the gesture.
	audio_output_init(app_context);

	// Size the dynamic bitmap texture array large enough for any bundled
	// image the test may load via MovieClipLoader.loadClip("foo.png" / .gif /
	// .jpg). Defaults to 256×256 inside the renderer; bump up here when the
	// data registry reports a larger image (capped at 2048 to keep VRAM use
	// reasonable). The renderer preserves a pre-set value (see
	// create_buffers_and_upload in render_webgpu.c).
	{
		int max_w = 0, max_h = 0;
		getDataFilesMaxImageDims(&max_w, &max_h);
		if (max_w > 2048) max_w = 2048;
		if (max_h > 2048) max_h = 2048;
		if ((u32)max_w > context->dynamic_bitmap_max_w)
			context->dynamic_bitmap_max_w = (u32)max_w;
		if ((u32)max_h > context->dynamic_bitmap_max_h)
			context->dynamic_bitmap_max_h = (u32)max_h;
	}

	// Embedded video (DefineVideoStream + VideoFrame) draws each decoded frame
	// through renderer_draw_bitmap_quad_scaled, which drops any source larger
	// than dynamic_bitmap_max_{w,h} (render_webgpu.c) — and the AVM1 default is
	// 256×256, so a 640×480 video frame is silently discarded and the stage
	// stays blank.
	//
	// VIDEO_HIGHEST_W/H are recompiler-emitted per-SWF (0 when the movie has no
	// DefineVideoStream), so this raise is GATED ON VIDEO PRESENCE and cannot
	// move a non-video test. That gate is load-bearing: dynamic_bitmap_max
	// feeds the bitmap texture array's padded dimensions
	// (render_webgpu.c create_buffers_and_upload), which the shader's UV
	// normalisation divides by — raising it unconditionally would shift every
	// bitmap render in the corpus.
#if defined(VIDEO_HIGHEST_W) && defined(VIDEO_HIGHEST_H)
	if (VIDEO_HIGHEST_W > 0 && VIDEO_HIGHEST_H > 0) {
		u32 vw = (u32)VIDEO_HIGHEST_W, vh = (u32)VIDEO_HIGHEST_H;
		if (vw > 2048) vw = 2048;
		if (vh > 2048) vh = 2048;
		if (vw > context->dynamic_bitmap_max_w) context->dynamic_bitmap_max_w = vw;
		if (vh > context->dynamic_bitmap_max_h) context->dynamic_bitmap_max_h = vh;
	}
#endif

	renderer_init(app_context, context);

	// Child movies' static bitmaps go into their combined slots now, while the
	// size-class pools are still unbuilt: the root's tagInit finalizes them a
	// few lines below and a child's own defineBitmap arrives at loadMovie time,
	// far too late. See ng_predeclareChildBitmaps.
	ng_predeclareChildBitmaps();

#ifdef OFFSCREEN_RENDER
	// Parse CAPTURE_TRIGGERS / CAPTURE_OUTPUT_DIR env vars so the tick-loop
	// capture wrappers know which frames to save. Mirrors swf_headless.c.
	parse_capture_triggers();
#endif

#ifdef OFFSCREEN_RENDER
	// Initialize root display sentinel and set root_movieclip.display_obj.
	// Mirrors swf_core.c (line ~815) and swf_headless.c (line ~862). Without
	// this, root_movieclip.display_obj is NULL when actionImportAssets runs
	// from frame_0, and the imported SWF's init code that resolves through
	// _root or g_current_context misses the root sentinel. Key cluster:
	// from_gnash/actionscript.all (Dejagnu.swf-based tests).
	{
		extern void ng_sync_root_display_obj(void);
		extern void* ng_get_root_display_obj(void);
		extern MovieClip root_movieclip;
		ng_sync_root_display_obj();
		root_movieclip.display_obj = ng_get_root_display_obj();
	}

	// Set root url and byte_size from compile-time SWF metadata. Mirrors
	// swf_core.c (line ~821-827). Without these, root.getBytesLoaded() /
	// getBytesTotal() return 0 instead of the actual SWF size, and
	// _url / loadVariables-relative URL construction sees an empty base.
	// Key test: avm1/get_bytes_total.
#ifdef SWF_URL
	{
		extern MovieClip root_movieclip;
		strncpy(root_movieclip.url, SWF_URL, sizeof(root_movieclip.url) - 1);
		root_movieclip.url[sizeof(root_movieclip.url) - 1] = '\0';
	}
#endif
#ifdef SWF_FILE_SIZE
	{
		extern MovieClip root_movieclip;
		root_movieclip.byte_size = SWF_FILE_SIZE;
	}
#endif
#endif

	tagInit(app_context);

#ifdef OFFSCREEN_RENDER
	// Set root movieclip as default execution context (for 'this' resolution).
	// Mirrors swf_core.c / swf_headless.c. Without this g_current_context is
	// NULL when frame_0 runs, breaking actions that resolve through the
	// current MC (e.g., DefineFunction / SetVariable inside imported SWFs).
	{
		extern MovieClip root_movieclip;
		actionSetCurrentContext(&root_movieclip);
	}
#endif

	tagMain(app_context);

#ifdef OFFSCREEN_RENDER
	// Refresh the readback buffer with the final scene state before saving
	// last_frame captures. Without this, tests whose only meaningful state
	// change happens AFTER the last tagShowFrame (e.g. mc.loadMovie on a
	// single-frame SWF: tagShowFrame runs before actionFirePendingDirectLoads
	// attaches the decoded pixels, and no later tick re-renders) capture an
	// out-of-date snapshot. Key test: avm1/movieclip_methods_with_loaded_image.
	{
		extern void tagRerenderFrame(SWFAppContext* app_context);
		if (context != NULL && context->renderer_ok) {
			renderer_request_capture(context);
			tagRerenderFrame(app_context);
		}
	}
	// Save any unsaved last_frame captures from the readback buffer before
	// renderer teardown. Mirrors swf_headless.c line ~1332.
	capture_save_last_frame();
#endif

	audio_output_shutdown();
	audio_shutdown(app_context);

	renderer_free(app_context, context);

	// Release any dynamic_props detached in the final tick, then the
	// env-gated (SWF_MEM_REPORT) leak-tracking summary; both must precede
	// heap_shutdown, which unmaps the pool the MC registry lives in.
	actionDrainDpropsReleases(app_context);
	// ...and destroy whatever that release (or the last tick's script) parked,
	// so the exit-time leak report counts reclaimable structs as reclaimed.
	swfDrainPendingDestroys(app_context);
	swfMemReportAtExitIfEnabled();

	heap_shutdown(app_context);
	freeMap();

	free(app_context->stack);
}

// Focus rect stub — wasm graphics only. action.c's widened arm provides
// the real impl in OFFSCREEN_RENDER (graphics-native).
#ifndef OFFSCREEN_RENDER
int actionGetFocusRectInfo(FocusRectInfo* out) {
	(void)out;
	return 0;
}
#endif

// Default findMovieEntry stub when no child movies are linked
#ifndef HAS_CHILD_MOVIES
MovieEntry* findMovieEntry(const char* filename) {
	(void)filename;
	return NULL;
}
MovieEntry* getMovieEntryAt(int idx) {
	(void)idx;
	return NULL;
}
#endif

// Default findDataFile stub when no data files are linked
#ifndef HAS_DATA_FILES
DataFileEntry* findDataFile(const char* filename) {
	(void)filename;
	return NULL;
}
void getDataFilesMaxImageDims(int* out_w, int* out_h) {
	if (out_w) *out_w = 0;
	if (out_h) *out_h = 0;
}
#endif

#endif // !NO_GRAPHICS