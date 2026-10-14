// Bridge globals for dynamic guest modules.
// Provides pointer indirection so guest code can access host globals
// through shared memory.

#include "bridge_globals.h"

int* _bridge_catch_up_mode = 0;
int* _bridge_g_tag_skip_mode = 0;
int* _bridge_quit_swf = 0;
void* _bridge_g_override_this = 0;
int* _bridge_g_override_this_set = 0;

void bridge_init(int* catch_up_mode_ptr, int* g_tag_skip_mode_ptr,
                 int* quit_swf_ptr, void* g_override_this_ptr,
                 int* g_override_this_set_ptr) {
    _bridge_catch_up_mode = catch_up_mode_ptr;
    _bridge_g_tag_skip_mode = g_tag_skip_mode_ptr;
    _bridge_quit_swf = quit_swf_ptr;
    _bridge_g_override_this = g_override_this_ptr;
    _bridge_g_override_this_set = g_override_this_set_ptr;
}
