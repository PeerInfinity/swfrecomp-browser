#pragma once

#include <common.h>

// libswf
#include <swf.h>
#include <tag.h>

// actionmodern
#include <action.h>
#include <action_queue.h>
#include <variables.h>
#include <object.h>

// Goto catch-up mode flag.
// When set, generated frame functions skip their script_N() calls
// while tag functions (PlaceObject, RemoveObject, etc.) still execute.
// Defined in swf_core.c (NO_GRAPHICS) and swf.c (GRAPHICS).
#ifndef DYNAMIC_GUEST
extern int catch_up_mode;
// When set, tag functions are no-ops but scripts still run.
// Used for deferred goto script execution (tags already processed inline).
extern int g_tag_skip_mode;
#else
// In DYNAMIC_GUEST mode, globals are redirected to pointer dereferences
// into the host module's memory via bridge_globals.h macros.
#include "bridge_globals.h"
#endif