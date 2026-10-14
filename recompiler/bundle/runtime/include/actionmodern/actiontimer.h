#pragma once

// actiontimer.h — public API for the Timer subsystem
// (setInterval / setTimeout / clearInterval / clearTimeout).
//
// Carved out of action.c per
// SWFRecompDocs/plans/split-action-c-math-first.md (fourth subsystem,
// after Math / Date / registerClass).
//
// Note: the frame-loop integration hooks `processTimers` and
// `hasActiveTimers` live in <actionmodern/action.h> for ABI stability —
// swf_core.c / swf.c already call them via that path and
// haven't been touched by this carve-out.

#include <actionmodern/action_internal.h>  // ASFunction, ActionVar, SWFAppContext

// ------------------------------------------------------------------
// Timer creation / cancellation (called by action.c from both the
// inline actionCallFunction "setInterval"/"setTimeout" names and the
// aliased-function builtins g_setInterval_func.advanced_func etc.)
// ------------------------------------------------------------------

// Create a timer. is_interval = 1 for setInterval (repeating), 0 for
// setTimeout (one-shot). Accepts both function-form
// (func, delay [, args...]) and method-form
// (obj, methodName, delay [, args...]).
// Takes ownership of args (frees via FREE) on all exit paths, including
// validation-failure.
void actionTimerSetInterval(SWFAppContext* app_context, ActionVar* args, u32 num_args, int is_interval);

// Cancel a timer by id. Works for both setInterval- and setTimeout-
// produced ids (single registry).
void actionTimerClearInterval(SWFAppContext* app_context, ActionVar* args, u32 num_args);

// ------------------------------------------------------------------
// ASFunction accessors — used by action.c's initGlobalObject to
// register setInterval / clearInterval / setTimeout / clearTimeout on
// _global, and by the setupNativeFuncOwnProps pre-init pass.
// ------------------------------------------------------------------

// Ensure the 4 ASFunction globals are initialized (idempotent).
// Accessors below call this internally.
void initTimerFunctions(void);

ASFunction* actionTimerGetSetInterval(void);
ASFunction* actionTimerGetClearInterval(void);
ASFunction* actionTimerGetSetTimeout(void);
ASFunction* actionTimerGetClearTimeout(void);
