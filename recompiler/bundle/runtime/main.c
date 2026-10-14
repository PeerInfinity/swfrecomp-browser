#include <recomp.h>
#include <swf.h>
#include <out.h>
#include <string.h>
#include "constants.h"

#ifndef FRAME_RATE
#define FRAME_RATE 12
#endif

#ifndef SWF_VERSION
#define SWF_VERSION 4
#endif

#ifndef SWF_USE_NETWORK
#define SWF_USE_NETWORK 0
#endif

#include "draws.h"

SWFAppContext app_context = {
    .frame_funcs = NULL,
    .frame_count = 0
};

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <string.h>
#include <actionmodern/action.h>
#include <actionmodern/stackvalue.h>

EMSCRIPTEN_KEEPALIVE
void runSWF() {
    printf("Starting SWF execution from JavaScript...\n");
#ifdef SWF_AVM2
    // AS3 SWF (RecompiledABC/ present): drive the AVM2 runtime. Stage 13a — the
    // AVM2 browser entry. runSWF_avm2 picks its browser loop shape (while(1) +
    // emscripten_sleep, ASYNCIFY) under __EMSCRIPTEN__ && !OFFSCREEN_RENDER.
    // app_context's graphics fields were populated in main() (the
    // !defined(NO_GRAPHICS) block runs in the browser build).
    extern void runSWF_avm2(SWFAppContext* app_context);
    runSWF_avm2(&app_context);
#else
    swfStart(&app_context);
#endif
}

// Host->AS inward bridge: synchronously invoke an ExternalInterface callback the
// SWF registered via ExternalInterface.addCallback(name, ...), passing one
// optional string arg, and return its string result as UTF-8. This is the
// host-initiated (addCallback) EI direction — the opposite of the cooperative
// __swfBridge pull (AS-initiated ExternalInterface.call -> window[name]) — and
// is what flashPanel's memory-poke style needs (configure(json) / readState()).
// The browser exposes this to JS via Module.ccall('swf_ei_call_internal', ...).
//
// Safe to call from a JS timer: the movie loop parks at emscripten_sleep between
// frames (ASYNCIFY), so this runs while no frame is mid-execution — the same
// quiescent window where the cooperative pull observes state. addCallback only
// registers when g_external_call_handler is installed (page exposed
// window.__swfBridge), so ExternalInterface.available must be true first.
extern uint16_t* utf8_to_u16(SWFAppContext* app_context, const char* utf8, u32 byte_len, u32* out_u16_len);

EMSCRIPTEN_KEEPALIVE
const char* swf_ei_call_internal(const char* name, const char* arg) {
    static char out_buf[262144];   // 256 KB — ample for a realistic readState JSON
    out_buf[0] = '\0';

    ActionVar av;
    memset(&av, 0, sizeof(av));
    int argc = 0;
    if (arg && arg[0]) {
        u32 n16 = 0;
        uint16_t* u16 = utf8_to_u16(&app_context, arg, (u32)strlen(arg), &n16);
        av.type = ACTION_STACK_VALUE_STRING;
        av.str_size = n16;
        av.data.string_data.heap_ptr = u16;
        av.data.string_data.owns_memory = true;
        argc = 1;
    } else {
        av.type = ACTION_STACK_VALUE_UNDEFINED;
    }

    ActionVar r = actionEI_callInternalInterface(&app_context, name, &av, argc);
    ei_actionvar_to_utf8(&r, out_buf, sizeof(out_buf));   // STRING -> UTF-8; else ""
    return out_buf;
}
#endif

#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
void input_events_load(const char* path);
#endif

int main(int argc, char* argv[]) {
    extern int g_swf_version;
    g_swf_version = SWF_VERSION;
    extern int g_main_movie_swf_version;
    g_main_movie_swf_version = SWF_VERSION;
    extern int g_use_network;
    g_use_network = SWF_USE_NETWORK;

    app_context.frame_funcs = frame_funcs;
    app_context.frame_count = FRAME_COUNT;
    app_context.fps = FRAME_RATE;

    app_context.shape_data = (char*)shape_data;
    app_context.shape_data_size = sizeof(shape_data);
    app_context.transform_data = (char*)transform_data;
    app_context.transform_data_size = sizeof(transform_data);

    // Static-text tables: all modes, not just graphics. The CPU glyph hit
    // tester and TextSnapshot read them in NO_GRAPHICS too, and
    // ng_buildMovieRenderTables needs the root's row counts to size the
    // combined tables a loaded child's indices are re-based onto.
    app_context.glyph_data = (u32*)glyph_data;
    app_context.glyph_data_size = sizeof(glyph_data);
    app_context.text_data = text_data;
    app_context.text_data_size = sizeof(text_data);
    app_context.text_char_codes = text_char_codes;
    app_context.text_char_codes_size = sizeof(text_char_codes);

    // Fill-style + morph END tables: all modes too. The AVM2 CPU raster
    // (BitmapData.draw, the CPU frame dump) and the AVM2 exact hit test read
    // them in NO_GRAPHICS, and ng_buildMovieRenderTables needs the root's row
    // counts there to combine a loaded child's rows onto them.
    app_context.color_data = (char*)color_data;
    app_context.color_data_size = sizeof(color_data);
    app_context.uninv_mat_data = (char*)uninv_mat_data;
    app_context.uninv_mat_data_size = sizeof(uninv_mat_data);
    app_context.gradient_data = (char*)gradient_data;
    app_context.gradient_data_size = sizeof(gradient_data);
    app_context.morph_end_shape_data = (char*)morph_end_shape_data;
    app_context.morph_end_shape_data_size = sizeof(morph_end_shape_data);
    app_context.morph_end_color_data = (char*)morph_end_color_data;
    app_context.morph_end_color_data_size = sizeof(morph_end_color_data);

#ifndef NO_GRAPHICS
    app_context.width = FRAME_WIDTH;
    app_context.height = FRAME_HEIGHT;
    app_context.stage_to_ndc = stage_to_ndc;
#if defined(VIEWPORT_WIDTH) && defined(VIEWPORT_HEIGHT)
    // The test harness declares a [player_options] viewport_dimensions and
    // Ruffle renders at THAT size, not at the SWF header stage size, fitting
    // the movie box into it with StageScaleMode::ShowAll (uniform min-fit) and
    // the default centred StageAlign. Reproduce both halves here: render at the
    // viewport size, and bake the fit into stage_to_ndc.
    //
    // Baking it into the projection (rather than calling SetViewport on the
    // fitted sub-rect) is deliberate: Ruffle's tests run with
    // should_letterbox() == false, so its view_bounds are *extended* into the
    // margins and content positioned outside the movie box is drawn there.
    // SetViewport would clip it instead. The margins are ordinary stage
    // background, not black bars.
    {
        static float fit_stage_to_ndc[16];
        float sx = (float) VIEWPORT_WIDTH / (float) FRAME_WIDTH;
        float sy = (float) VIEWPORT_HEIGHT / (float) FRAME_HEIGHT;
        float s = (sx < sy) ? sx : sy;                                  // ShowAll
        float kx = (float) FRAME_WIDTH  * s / (float) VIEWPORT_WIDTH;   // fitted w / VW
        float ky = (float) FRAME_HEIGHT * s / (float) VIEWPORT_HEIGHT;  // fitted h / VH

        app_context.render_width  = VIEWPORT_WIDTH;
        app_context.render_height = VIEWPORT_HEIGHT;
        app_context.stage_scale   = s;
        app_context.stage_fit_x   = kx;
        app_context.stage_fit_y   = ky;

        // Centred align collapses the correction to a pure scale, with no
        // translation term: with ox = (VW - nw)/2, NDC' = kx*NDC + (2*ox/VW +
        // kx - 1) and that bracket is identically zero. So scale row 0 of the
        // column-major matrix by kx and row 1 by ky. When the aspect ratios
        // match (every 2x test, and every test whose viewport equals its movie
        // box), kx == ky == 1 and the matrix is bit-identical to the original.
        memcpy(fit_stage_to_ndc, stage_to_ndc, sizeof(fit_stage_to_ndc));
        fit_stage_to_ndc[0]  *= kx;  fit_stage_to_ndc[4]  *= kx;
        fit_stage_to_ndc[8]  *= kx;  fit_stage_to_ndc[12] *= kx;
        fit_stage_to_ndc[1]  *= ky;  fit_stage_to_ndc[5]  *= ky;
        fit_stage_to_ndc[9]  *= ky;  fit_stage_to_ndc[13] *= ky;
        app_context.stage_to_ndc = fit_stage_to_ndc;
    }
#else
    // Browser / game / host builds never define VIEWPORT_*: render target ==
    // stage, fit is the identity, stage_to_ndc is untouched.
    app_context.render_width  = FRAME_WIDTH;
    app_context.render_height = FRAME_HEIGHT;
    app_context.stage_scale   = 1.0f;
    app_context.stage_fit_x   = 1.0f;
    app_context.stage_fit_y   = 1.0f;
#endif
    app_context.bitmap_count = BITMAP_COUNT;
    app_context.bitmap_highest_w = BITMAP_HIGHEST_W;
    app_context.bitmap_highest_h = BITMAP_HIGHEST_H;
    app_context.bitmap_data = (char*)bitmap_data;
    app_context.bitmap_data_size = sizeof(bitmap_data);
    app_context.cxform_data = (char*)cxform_data;
    app_context.cxform_data_size = sizeof(cxform_data);
#endif

#ifdef HAS_TEST_HARNESS
    {
        extern void test_harness_init(SWFAppContext*);
        test_harness_init(&app_context);
    }
#endif

#ifndef __EMSCRIPTEN__
    printf("SWF Runtime Loaded (Native Build)\n\n");
#ifdef SWF_AVM2
    // AS3 SWF (RecompiledABC/ present): dispatch to the AVM2 runtime.
    // AVM1 builds never define SWF_AVM2 and never link src/avm2/*.c.
    {
        extern void avm2_input_load(const char* path);
        extern void runSWF_avm2(SWFAppContext* app_context);
        if (argc > 1) {
            avm2_input_load(argv[1]);
        }
        runSWF_avm2(&app_context);
    }
#else
#if defined(NO_GRAPHICS) || defined(OFFSCREEN_RENDER)
    if (argc > 1) {
        input_events_load(argv[1]);
    }
#endif
    swfStart(&app_context);
#endif
#else
    printf("WASM SWF Runtime Loaded!\n");
    printf("This is a recompiled Flash SWF running in WebAssembly.\n\n");
    printf("Call runSWF() from JavaScript to execute the SWF.\n");
#endif

    return 0;
}
