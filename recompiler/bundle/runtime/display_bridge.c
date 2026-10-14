/*
 * Display Bridge — JS-callable functions for reading/writing the display list.
 * Compiled when -DHAS_DISPLAY_BRIDGE is defined.
 */

#ifdef HAS_DISPLAY_BRIDGE

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define _USE_MATH_DEFINES
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <swf.h>
#include <action.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifndef USE_WEBGPU
/* Without WebGPU there is no renderer to sync to — stubs only. */
#endif

#ifdef USE_WEBGPU
#include <renderer.h>
#endif

/* Externs from the runtime */
extern DisplayObject* display_list;
extern size_t max_depth;
extern Character* dictionary;
extern SWFAppContext app_context;

#ifdef USE_WEBGPU
/* RenderContext* is a file-scope global in swf.c */
extern RenderContext* context;
#endif

static char json_buf[8192];

/* DEBUG: report root timeline state (Pacman play-button-bounce diagnosis). */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int dbgRootFrame(void)
{
    extern size_t current_frame;   /* the true root playhead (swf.c) */
    return (int)current_frame;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int dbgIsPlaying(void)
{
    extern int is_playing;
    return is_playing;
}

/* DEBUG: last key code seen by the runtime (keyboard-input diagnosis). */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int dbgLastKey(void)
{
    return app_context.keys.last_key_down;
}

/* DEBUG: read a clip's AS-state (x,y,as_set_flags) via the SAME lookup the
 * renderer uses (actionFindMovieClipByName). Format: "x,y,flags,hasDisplayObj".
 * Pacman gameplay diagnosis. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char* dbgClipInfo(const char* name)
{
    static char buf[128];
    extern MovieClip* actionFindMovieClipByName(const char* instance_name);
    MovieClip* mc = actionFindMovieClipByName(name);
    if (!mc) { snprintf(buf, sizeof(buf), "NOT_FOUND"); return buf; }
    snprintf(buf, sizeof(buf), "x=%.1f y=%.1f flags=%u dobj=%d",
             mc->x, mc->y, (unsigned)mc->as_set_flags, mc->display_obj ? 1 : 0);
    return buf;
}

/* DEBUG: dump an attached/child clip's state by instance name (ghost diagnosis):
 * "x,y parent=NAME ghX=.. ghY=.. ghDir=.. ghPos=..". Searches child_mc_cache. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char* dbgChildState(const char* name)
{
    static char buf[256];
    extern MovieClip* child_mc_cache[];
    extern int child_mc_count;
    MovieClip* mc = NULL;
    /* Prefer a match whose parent is the game "Ghost" container (the attract
     * demo's "G" container also holds clips named 1..4). */
    for (int i = 0; i < child_mc_count; i++) {
        MovieClip* c = child_mc_cache[i];
        if (c != NULL && c->name[0] != '\0' && strcmp(c->name, name) == 0) {
            if (c->parent != NULL && strcmp(c->parent->name, "Ghost") == 0) { mc = c; break; }
            if (mc == NULL) mc = c;  /* fallback */
        }
    }
    if (!mc) { snprintf(buf, sizeof(buf), "NOT_FOUND"); return buf; }
    double v[5] = {-9, -9, -9, -9, -9};
    const char* names[5] = {"ghX", "ghY", "ghDir", "ghPos", "gNum"};
    extern ActionVar* getProperty(void* obj, const char* name, unsigned name_length);
    extern double varToDoubleSWF(SWFAppContext*, ActionVar*, int);
    if (mc->dynamic_props != NULL) {
        for (int k = 0; k < 5; k++) {
            ActionVar* p = getProperty(mc->dynamic_props, names[k], (unsigned)strlen(names[k]));
            if (p) v[k] = varToDoubleSWF(&app_context, p, 6);
        }
    }
    const char* pn = (mc->parent && mc->parent->name[0]) ? mc->parent->name : "(root/none)";
    snprintf(buf, sizeof(buf), "x=%.1f y=%.1f parent=%s ghX=%.0f ghY=%.0f ghDir=%.0f ghPos=%.0f gNum=%.0f",
             mc->x, mc->y, pn, v[0], v[1], v[2], v[3], v[4]);
    return buf;
}

/* DEBUG: read a _root global variable as a number (Pacman gameplay diagnosis). */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double dbgRootVarNum(const char* name)
{
    extern double varToDoubleSWF(SWFAppContext* app_context, ActionVar* v, int swf_version);
    ActionVar* v = getVariable((char*)name, strlen(name));
    if (!v) return -999999.0;
    return varToDoubleSWF(&app_context, v, 6);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char* getDisplayListJSON(void)
{
    int pos = 0;
    pos += snprintf(json_buf + pos, sizeof(json_buf) - pos,
                    "{\"displayList\":[");

    int first = 1;
    for (size_t d = 1; d <= max_depth; d++) {
        DisplayObject* obj = &display_list[d];
        if (obj->char_id == 0) continue;

        const char* type_name = "unknown";
        Character* ch = &dictionary[obj->char_id];
        switch (ch->type) {
        case CHAR_TYPE_SHAPE:      type_name = "shape"; break;
        case CHAR_TYPE_MORPH_SHAPE: type_name = "morph"; break;
        case CHAR_TYPE_TEXT:        type_name = "text"; break;
        case CHAR_TYPE_SPRITE:     type_name = "sprite"; break;
        case CHAR_TYPE_BUTTON:     type_name = "button"; break;
        }

        /* Read transform matrix (16 floats per slot) */
        float* slot = (float*)app_context.transform_data + obj->transform_id * 16;
        float x = slot[12] / 20.0f;
        float y = slot[13] / 20.0f;
        float xscale = sqrtf(slot[0]*slot[0] + slot[1]*slot[1]) * 100.0f;
        float yscale = sqrtf(slot[4]*slot[4] + slot[5]*slot[5]) * 100.0f;
        float rotation = atan2f(slot[1], slot[0]) * 180.0f / (float)M_PI;

        if (!first) {
            pos += snprintf(json_buf + pos, sizeof(json_buf) - pos, ",");
        }
        first = 0;

        pos += snprintf(json_buf + pos, sizeof(json_buf) - pos,
            "{\"depth\":%zu,\"charId\":%zu,\"charType\":\"%s\","
            "\"transformId\":%u,"
            "\"x\":%.1f,\"y\":%.1f,"
            "\"xscale\":%.1f,\"yscale\":%.1f,"
            "\"rotation\":%.1f}",
            d, obj->char_id, type_name,
            obj->transform_id,
            x, y, xscale, yscale, rotation);

        if (pos >= (int)sizeof(json_buf) - 100) break; /* safety margin */
    }

    pos += snprintf(json_buf + pos, sizeof(json_buf) - pos, "]}");
    return json_buf;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char* getSpriteChildrenJSON(int root_depth)
{
    static char child_buf[8192];
    int pos = 0;
    pos += snprintf(child_buf + pos, sizeof(child_buf) - pos,
                    "{\"rootDepth\":%d,\"children\":[", root_depth);

    if (root_depth < 1 || (size_t)root_depth > max_depth) {
        pos += snprintf(child_buf + pos, sizeof(child_buf) - pos, "]}");
        return child_buf;
    }

    DisplayObject* parent = &display_list[root_depth];
    if (parent->char_id == 0 || parent->sprite_display_list == NULL) {
        pos += snprintf(child_buf + pos, sizeof(child_buf) - pos, "]}");
        return child_buf;
    }

    int first = 1;
    /* sprite_display_list is indexed by relative depth; iterate up to
     * sprite_max_depth and any AVM1-attached depths that may exceed it. */
    size_t scan = parent->sprite_max_depth;
    if (scan < 64) scan = 64; /* attachMovie depths start near 0 */
    if (scan > parent->sprite_dl_capacity) scan = parent->sprite_dl_capacity;
    for (size_t d = 0; d <= scan; d++) {
        DisplayObject* obj = &parent->sprite_display_list[d];
        if (obj->char_id == 0) continue;

        const char* type_name = "unknown";
        Character* ch = &dictionary[obj->char_id];
        switch (ch->type) {
        case CHAR_TYPE_SHAPE:       type_name = "shape"; break;
        case CHAR_TYPE_MORPH_SHAPE: type_name = "morph"; break;
        case CHAR_TYPE_TEXT:        type_name = "text"; break;
        case CHAR_TYPE_SPRITE:      type_name = "sprite"; break;
        case CHAR_TYPE_BUTTON:      type_name = "button"; break;
        }

        float* slot = (float*)app_context.transform_data + obj->transform_id * 16;
        float x = slot[12] / 20.0f;
        float y = slot[13] / 20.0f;

        if (!first) pos += snprintf(child_buf + pos, sizeof(child_buf) - pos, ",");
        first = 0;

        const char* name = obj->instance_name ? obj->instance_name : "";

        /* For attached MCs the entry's transform_id stays at 0 (identity);
         * compose_children builds the local matrix from mc->x/y in flight,
         * so the slot read above shows (0,0). Look up the MC by name and
         * report mc->x/y when available. */
        float mc_x = x, mc_y = y;
        if (name[0] != '\0') {
            extern MovieClip* child_mc_cache[];
            extern int child_mc_count;
            for (int i = 0; i < child_mc_count; i++) {
                MovieClip* mc = child_mc_cache[i];
                if (mc != NULL && mc->name[0] != '\0' && strcmp(mc->name, name) == 0) {
                    mc_x = mc->x; mc_y = mc->y; break;
                }
            }
        }
        pos += snprintf(child_buf + pos, sizeof(child_buf) - pos,
            "{\"depth\":%zu,\"charId\":%zu,\"type\":\"%s\","
            "\"name\":\"%s\",\"x\":%.1f,\"y\":%.1f}",
            d, obj->char_id, type_name, name, mc_x, mc_y);

        if (pos >= (int)sizeof(child_buf) - 100) break;
    }

    pos += snprintf(child_buf + pos, sizeof(child_buf) - pos, "]}");
    return child_buf;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void setObjectTransform(int depth, float x, float y,
                        float xscale, float yscale, float rotation)
{
    if (depth < 1 || (size_t)depth > max_depth) return;

    DisplayObject* obj = &display_list[depth];
    if (obj->char_id == 0) return;

    float* slot = (float*)app_context.transform_data + obj->transform_id * 16;

    float sx = xscale / 100.0f;
    float sy = yscale / 100.0f;
    float rad = rotation * (float)M_PI / 180.0f;
    float c = cosf(rad), s = sinf(rad);

    slot[0]  = sx * c;
    slot[1]  = sx * s;
    slot[4]  = -sy * s;
    slot[5]  = sy * c;
    slot[12] = roundf(x * 20.0f);
    slot[13] = roundf(y * 20.0f);

#ifdef USE_WEBGPU
    if (context) {
        renderer_write_transform(context, obj->transform_id, slot);
    }
#endif
}

// ---------------------------------------------------------------------------
// Browser framebuffer capture (debug)
//
// Reads the rendered frame back via a direct GPU copyTextureToBuffer (see
// render_webgpu.c), bypassing the software present queue that makes a busy
// board's Playwright/CDP screenshot hang in this WSL2. Async by design — JS
// requests, polls dbgCaptureReady, then reads the RGBA bytes at dbgCaptureData.
// A reusable async helper window.dbgCaptureBoard() (returns a PNG data URL) is
// installed on first request for console / harness use.
// ---------------------------------------------------------------------------
#if defined(USE_WEBGPU) && defined(__EMSCRIPTEN__)

EMSCRIPTEN_KEEPALIVE
void dbgCapturePNG(void)
{
    if (context) render_webgpu_request_browser_capture(context);
    // Install the JS convenience helper once (no-op on subsequent calls).
    static int helper_installed = 0;
    if (!helper_installed) {
        helper_installed = 1;
        EM_ASM({
            window.dbgCaptureBoard = async function() {
                Module.ccall('dbgCapturePNG', null, [], []);
                var ready = 0;
                for (var i = 0; i < 300; i++) {           // up to ~10s
                    ready = Module.ccall('dbgCaptureReady', 'number', [], []);
                    if (ready) break;
                    await new Promise(function(r){ setTimeout(r, 33); });
                }
                if (!ready) return null;
                var ptr = Module.ccall('dbgCaptureData', 'number', [], []);
                var w = Module.ccall('dbgCaptureWidth', 'number', [], []);
                var h = Module.ccall('dbgCaptureHeight', 'number', [], []);
                if (!ptr || !w || !h) return null;
                var bytes = Module.HEAPU8.subarray(ptr, ptr + w * h * 4);
                var cnv = document.createElement('canvas');
                cnv.width = w; cnv.height = h;
                var cx = cnv.getContext('2d');
                var img = cx.createImageData(w, h);
                img.data.set(bytes);
                cx.putImageData(img, 0, 0);
                return cnv.toDataURL('image/png');
            };
        });
    }
}

EMSCRIPTEN_KEEPALIVE
int dbgCaptureReady(void)
{
    return context ? render_webgpu_browser_capture_ready(context) : 0;
}

EMSCRIPTEN_KEEPALIVE
int dbgCaptureData(void)
{
    if (!context) return 0;
    return (int)(intptr_t)render_webgpu_browser_capture_data(context);
}

EMSCRIPTEN_KEEPALIVE
int dbgCaptureWidth(void)  { return context ? context->width : 0; }

EMSCRIPTEN_KEEPALIVE
int dbgCaptureHeight(void) { return context ? context->height : 0; }

// Throttle the render loop to at most ~(1000/ms) fps (0 = off). On WSL2 software
// WebGPU an uncapped busy board floods the present queue faster than it drains;
// the backlog makes the framebuffer readback (and Playwright input) stall for
// many seconds. Cap to ~6-10fps while capturing/driving the board.
EMSCRIPTEN_KEEPALIVE
void dbgSetFrameCapMs(int ms)
{
    extern int g_debug_frame_floor_ms;
    g_debug_frame_floor_ms = ms < 0 ? 0 : ms;
}

#endif /* USE_WEBGPU && __EMSCRIPTEN__ */

#endif /* HAS_DISPLAY_BRIDGE */
