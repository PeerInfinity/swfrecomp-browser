#pragma once

// Scripted file dialogs — the runtime half of Ruffle's test UI backend
// (tests/framework/src/backends/ui.rs).
//
// Unlike the input-event and socket mocks, this one is keyed off the test's
// CONTENT rather than a config file, so there is nothing for verify_output.py
// to preprocess. The whole contract is three constants:
//
//   * `browse(filters)` succeeds when SOME filter's description is
//     "debug-select-success"; anything else (including no filters at all)
//     cancels.
//   * `save(data, hint)` succeeds when the hint is exactly
//     "debug-success.txt"; anything else cancels.
//   * The simulated file is always named test.txt (test1/2/3.txt for the
//     multi-select variant), holds "Hello, World!" (13 bytes), has type
//     ".txt", and has NO creation/modification time — which is why the corpus
//     expects `creationDate: null` rather than a date.
//
// Timing: Ruffle spawns the dialog as a future and the test harness polls the
// executor once the frame is over, so a dialog opened during frame N resolves
// in the SAME tick, right after that frame's scripts — and a dialog opened
// from a resolution callback resolves in the same drain
// (filereference_save_and_browse chains save -> browse -> load under
// `num_ticks = 1`). `swf_dialog_pump` therefore keeps going until the queue is
// empty, exactly like the loader drain it rides on.
//
// Both VMs share this (AVM1 FileReference/FileReferenceList, AVM2
// FileReference/FileReferenceList), so it is deliberately VM-agnostic: the
// caller decides success from the mock's rules and hands over an opaque target
// plus a resolution callback. Living in src/utils.c keeps it in EVERY build's
// source list with no per-script wiring to rot.

#include <stddef.h>

#define SWF_DIALOG_MAGIC_FILTER "debug-select-success"
#define SWF_DIALOG_MAGIC_SAVE   "debug-success.txt"
#define SWF_DIALOG_CONTENTS     "Hello, World!"
#define SWF_DIALOG_CONTENTS_LEN 13
#define SWF_DIALOG_FILE_TYPE    ".txt"

// `success` is 1 for a simulated selection, 0 for a simulated user cancel.
typedef void (*SwfDialogResolveFn)(void* target, int success);

// Queue a dialog. Resolution happens at the next pump, never inline.
void swf_dialog_queue(SwfDialogResolveFn fn, void* target, int success);

// Nonzero while a queued dialog has yet to resolve. The AVM1 frame loop keeps
// ticking while this holds, the same way it does for an open socket: Ruffle's
// harness runs its full num_ticks unconditionally, ours exits as soon as
// nothing is asking to run.
int swf_dialog_pending(void);

// Resolve every queued dialog, including ones a resolution callback queues.
void swf_dialog_pump(void);

// The simulated selection's name: index 0 is the single-select "test.txt",
// 1..3 are the multi-select "test1.txt".."test3.txt". The pointer is static.
const char* swf_dialog_file_name(int index);
