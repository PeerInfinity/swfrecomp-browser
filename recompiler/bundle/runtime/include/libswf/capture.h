#pragma once

// Frame-capture state machine used by swf.c (OFFSCREEN_RENDER).
// Reads CAPTURE_TRIGGERS / CAPTURE_OUTPUT_DIR
// env vars and writes rendered frames as PNGs via the renderer's readback
// buffer. The renderer-touching paths gate on HEADLESS_RENDER_ENABLED;
// without that define everything is a no-op (so non-image tests don't pay
// any cost beyond an env-var read).

#include <libswf/swf.h>

// One-shot init at swfStart: parse CAPTURE_TRIGGERS env into the table.
void parse_capture_triggers(void);

// Per-tick wrappers, called in order from the frame loop:
//   pre_frame    - before frame_funcs[current_frame] runs
//   after_events - after input_events_pump_tick + goto catch-up
//   post_frame   - save iteration/fscommand captures whose tick has arrived
void capture_tick_pre_frame(void);
void capture_tick_after_events(SWFAppContext* app_context);
void capture_tick_post_frame(void);

// End-of-loop: save any unsaved last_frame captures from the readback buffer.
void capture_save_last_frame(void);

// Called from actionFSCommand when fscommand("capture", ...) fires.
void capture_on_fscommand(void);

// Used by tagRerenderFrame to decide whether to request a capture pass.
int capture_has_pending(void);
