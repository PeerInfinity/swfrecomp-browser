// Bridge globals for dynamic guest modules.
// These pointers are initialized by bridge_init() to point into the host
// module's memory, allowing the guest to read/write shared globals.
//
// When DYNAMIC_GUEST is defined, this header also provides macros that
// redirect global variable accesses through these pointers.

#pragma once

#include <stdint.h>

extern int* _bridge_catch_up_mode;
extern int* _bridge_g_tag_skip_mode;
extern int* _bridge_quit_swf;
extern void* _bridge_g_override_this;     // Points to ActionVar in host memory
extern int* _bridge_g_override_this_set;

void bridge_init(int* catch_up_mode_ptr, int* g_tag_skip_mode_ptr,
                 int* quit_swf_ptr, void* g_override_this_ptr,
                 int* g_override_this_set_ptr);

#ifdef DYNAMIC_GUEST
// Redirect global accesses through bridge pointers.
// These macros are used instead of the extern declarations that are
// guarded by #ifndef DYNAMIC_GUEST in the runtime headers.
// Note: ActionVar must be defined before this point (included via action.h).
#include <actionmodern/stackvalue.h>
#define catch_up_mode       (*_bridge_catch_up_mode)
#define g_tag_skip_mode     (*_bridge_g_tag_skip_mode)
#define quit_swf            (*_bridge_quit_swf)
#define g_override_this     (*((ActionVar*)_bridge_g_override_this))
#define g_override_this_set (*_bridge_g_override_this_set)
#endif
