// Standalone loader for a guest module compiled by the in-browser recompiler.
//
// Used by the recompiler page (pipeline_graphics.js) AND shipped inside the
// "runnable" zip next to <name>-guest.wasm + graphics_host.{js,wasm}, so the
// two can never drift apart. No dependency on the Wasmer SDK.
//
//   const { runGuest } = await import("./guest_loader.js");
//   await runGuest({ guestBytes, hostDir: "./", canvas, log, setStatus });
//
// What it does (design: SWFRecompDocs/plans/in-browser-recompiler-refresh-assessment.md §7):
//   1. Loads the Emscripten graphics host (createGraphicsHost) and captures the
//      RAW instance exports in the instantiateWasm hook — under JSPI Emscripten
//      wraps every export in a JS function, and JSPI cannot suspend across JS
//      frames, so the guest must call the raw wasm functions.
//   2. Instantiates the guest against the host's shared memory, resolving its
//      imports from those raw exports (setjmp/longjmp and WASI get stubs).
//   3. Mirrors the guest's function table into the host's reserved low table
//      slots, so guest function pointers are valid host-side with no translation.
//   4. Hands the host the guest's data tables and frame functions, then runSWF().

// Must match host_main_graphics.c / build_graphics_host.sh (GUEST_ARENA_BASE,
// GUEST_ARENA_END); checked against the host's exports before running.
export const GUEST_ARENA_BASE = 65536;
export const GUEST_ARENA_END = 101 * 1024 * 1024;

let hostInstance = null;   // one host per page: the runtime has global state
let hostRawExports = null;
let hostUsed = false;
let hostVariant = null;    // "graphics_host" (AVM1) or "graphics_host_avm2"

// The AVM2 host variant (build_graphics_host.sh AVM2=1) carries the AVM2
// runtime and drives runSWF_avm2; a guest compiled from an AS3 SWF exports
// get_avm2_movie_tables and must run on it.
export function guestIsAvm2(mod) {
    return WebAssembly.Module.exports(mod).some(e => e.name === "get_avm2_movie_tables");
}

export function hostAlreadyUsed() { return hostUsed; }

export function checkBrowserSupport() {
    if (typeof WebAssembly.Suspending !== "function") {
        throw new Error("This browser lacks WebAssembly JS Promise Integration (JSPI), which the graphics host needs to suspend mid-frame across in-browser-compiled code. Use Chrome 137+.");
    }
    if (typeof SharedArrayBuffer !== "function" || !globalThis.crossOriginIsolated) {
        throw new Error("This page is not cross-origin isolated (SharedArrayBuffer unavailable). Serve it over HTTP with coi-serviceworker.js next to it, or with COOP/COEP headers; file:// does not work.");
    }
}

export async function loadHost({ hostDir = "host/", canvas, log = () => {}, avm2 = false }) {
    const variant = avm2 ? "graphics_host_avm2" : "graphics_host";
    if (hostInstance) {
        if (hostVariant !== variant) throw new Error(`The ${hostVariant} host is already loaded in this page; reload to run an ${avm2 ? "AS3" : "AVM1"} SWF.`);
        return hostInstance;
    }
    checkBrowserSupport();
    hostVariant = variant;
    if (typeof globalThis.createGraphicsHost !== "function") {
        await new Promise((resolve, reject) => {
            const s = document.createElement("script");
            s.src = hostDir + variant + ".js";
            s.onload = resolve;
            s.onerror = () => reject(new Error(`Failed to load ${hostDir}${variant}.js`));
            document.head.appendChild(s);
        });
    }
    hostInstance = await createGraphicsHost({
        canvas,
        locateFile: (p) => hostDir + p,
        print: (t) => log(t),
        printErr: (t) => log(t),
        instantiateWasm: (imports, onSuccess) => {
            WebAssembly.instantiateStreaming(fetch(hostDir + variant + ".wasm"), imports)
                .then(({ instance, module }) => { hostRawExports = instance.exports; onSuccess(instance, module); })
                .catch((e) => log(`[host] instantiate failed: ${e}`));
            return {};
        },
    });
    if (!hostRawExports) throw new Error("host instantiated without exposing raw exports");
    const arena = hostInstance._get_guest_arena(), size = hostInstance._get_guest_arena_size();
    if (arena !== GUEST_ARENA_BASE || arena + size !== GUEST_ARENA_END) {
        throw new Error(`host/loader arena mismatch: host [${arena}, ${arena + size}) vs loader [${GUEST_ARENA_BASE}, ${GUEST_ARENA_END})`);
    }
    return hostInstance;
}

export async function runGuest({ guestBytes, hostDir = "host/", canvas, log = () => {}, setStatus = () => {} }) {
    if (hostUsed) throw new Error("The graphics host can only run one SWF per page load — reload the page to run another.");
    const mod = await WebAssembly.compile(guestBytes);
    const avm2 = guestIsAvm2(mod);
    setStatus(`Loading ${avm2 ? "AVM2 " : ""}graphics host`);
    const host = await loadHost({ hostDir, canvas, log, avm2 });
    hostUsed = true;
    setStatus("Instantiating guest");

    const imports = WebAssembly.Module.imports(mod);
    const missing = [];
    const env = { memory: hostRawExports.memory };
    const stub = (name) => (...a) => {
        if (!stub[name]) { stub[name] = 1; log(`[guest] called unresolved runtime function: ${name}`); }
        return 0;
    };
    for (const imp of imports) {
        if (imp.module !== "env" || env[imp.name]) continue;
        if (imp.kind === "function") {
            if (imp.name === "setjmp" || imp.name === "__swf_guest_setjmp") {
                // AVM1 try/catch and AVM2 exception-table method bodies inline
                // setjmp into generated code and the in-browser clang cannot
                // lower it (the guest's setjmp.h shim makes it an import); run
                // the try body as if no exception can occur. A throw inside it
                // surfaces as an error.
                env[imp.name] = () => 0;
            } else if (imp.name === "longjmp") {
                env.longjmp = () => { throw new Error("AVM1 try/catch (longjmp) is not supported in the in-browser build; use the downloadable build bundle"); };
            } else {
                const f = hostRawExports[imp.name];
                if (f) env[imp.name] = f;
                else { missing.push(imp.name); env[imp.name] = stub(imp.name); }
            }
        } else if (imp.kind === "global") {
            env[imp.name] = new WebAssembly.Global({ value: "i32", mutable: true }, 0);
        } else if (imp.kind === "table") {
            env[imp.name] = new WebAssembly.Table({ initial: 1, element: "anyfunc" });
        }
    }
    if (missing.length) log(`[guest] ${missing.length} imports not exported by the host: ${missing.slice(0, 12).join(", ")}${missing.length > 12 ? " ..." : ""}`);
    const ENOSYS = 52, EBADF = 8;
    const wasiStubs = new Proxy({}, {
        get(_, p) {
            if (p === "fd_prestat_get" || p === "fd_prestat_dir_name") return () => EBADF;
            if (p === "proc_exit") return (c) => { throw new Error(`guest proc_exit(${c})`); };
            return () => ENOSYS;
        },
    });
    const instance = await WebAssembly.instantiate(mod, { env, wasi_snapshot_preview1: wasiStubs, wasix_32v1: wasiStubs });
    const g = instance.exports;

    // Layout check: runtime structs the guest allocates embed jmp_buf
    // (Avm2TryFrame); the guest's setjmp.h shim must match the host's size.
    if (g.get_guest_jmp_buf_size && host._get_jmp_buf_size) {
        const gs = g.get_guest_jmp_buf_size(), hs = host._get_jmp_buf_size();
        if (gs !== hs) throw new Error(`jmp_buf size mismatch: guest ${gs} vs host ${hs} bytes (update guest_setjmp_shim.h)`);
    }

    // Arena check: guest data + its shadow stack must fit below the host's data.
    const heapBase = g.__heap_base ? g.__heap_base.value : 0;
    if (heapBase && heapBase > GUEST_ARENA_END) {
        throw new Error(`Generated data (${((heapBase - GUEST_ARENA_BASE) / 1048576).toFixed(1)} MB) exceeds the ${((GUEST_ARENA_END - GUEST_ARENA_BASE) / 1048576).toFixed(0)} MB guest arena; use the downloadable build bundle for this SWF`);
    }

    // Mirror the guest's function table into the host's reserved low slots.
    const gt = g.__indirect_function_table;
    const base = host._get_host_table_base();
    if (!gt) throw new Error("guest exports no function table (link without --export-table?)");
    if (gt.length > base) throw new Error(`guest function table (${gt.length}) exceeds the host's reserved ${base} slots`);
    const ht = hostRawExports.__indirect_function_table;
    let mirrored = 0;
    for (let i = 1; i < gt.length; i++) {
        const f = gt.get(i);
        if (f) { ht.set(i, f); mirrored++; }
    }
    log(`[guest] function table: ${gt.length} entries, ${mirrored} mirrored into host slots < ${base}; data [${GUEST_ARENA_BASE}, ${heapBase}), frame_funcs[0] = ${new Uint32Array(hostRawExports.memory.buffer, g.get_frame_funcs(), 1)[0]}, tagInit = ${g.get_tagInit_ptr()}`);

    // Bridged runtime globals (bridge_globals.h): guest reads/writes host's copies.
    g.bridge_init(host._get_catch_up_mode_addr(), host._get_g_tag_skip_mode_addr(), host._get_quit_swf_addr(),
                  host._get_g_override_this_addr(), host._get_g_override_this_set_addr());

    const frameCount = g.get_frame_count(), fps = g.get_fps(), swfVersion = g.get_swf_version();
    const width = g.get_frame_width(), height = g.get_frame_height();
    canvas.width = width; canvas.height = height;
    host._configureAppContext(frameCount, fps, swfVersion, width, height);
    host._setFrameFuncs(g.get_frame_funcs());
    host._setTagInit(g.get_tagInit_ptr());
    host._setShapeData(g.get_shape_data(), g.get_shape_data_size());
    host._setTransformData(g.get_transform_data(), g.get_transform_data_size());
    host._setColorData(g.get_color_data(), g.get_color_data_size());
    host._setStageToNdc(g.get_stage_to_ndc());
    host._setBitmapInfo(g.get_bitmap_count(), g.get_bitmap_highest_w(), g.get_bitmap_highest_h());
    host._setUninvMatData(g.get_uninv_mat_data(), g.get_uninv_mat_data_size());
    host._setGradientData(g.get_gradient_data(), g.get_gradient_data_size());
    host._setBitmapData(g.get_bitmap_data(), g.get_bitmap_data_size());
    host._setGlyphData(g.get_glyph_data(), g.get_glyph_data_size());
    host._setTextData(g.get_text_data(), g.get_text_data_size(), g.get_text_char_codes ? g.get_text_char_codes() : 0);
    host._setCxformData(g.get_cxform_data(), g.get_cxform_data_size());
    host._setMorphData(g.get_morph_end_shape_data(), g.get_morph_end_shape_data_size(),
                       g.get_morph_end_color_data(), g.get_morph_end_color_data_size());
    host._setFrameLabels(g.get_frame_label_data(), g.get_frame_label_count());
    if (avm2) {
        host._setAvm2Tables(g.get_avm2_movie_tables(), g.get_avm2_abc_frames(), g.get_avm2_abc_lazy(),
                            g.get_avm2_symbol_class_frames(), g.get_avm2_device_fonts(), g.get_avm2_device_font_count());
        host._setAvm2DrawTables(g.get_color_data(), g.get_uninv_mat_data(), g.get_gradient_data(), g.get_morph_end_color_data());
    }

    // The JSPI frame loop resumes from promise continuations, so a trap inside a
    // frame surfaces as an uncaught error/rejection, not as a rejection of runSWF.
    const trapLog = (r) => { if (r && !/wasmer|panicked/.test(String(r.stack || r))) log(`[trap] ${(r.stack || String(r)).split("\n").slice(0, 14).join(" <- ")}`); };
    window.addEventListener("error", (ev) => trapLog(ev.error));
    window.addEventListener("unhandledrejection", (ev) => trapLog(ev.reason));

    setStatus(`Running ${width}x${height}, ${frameCount} frames @ ${fps} fps, SWF v${swfVersion}`);
    canvas.style.display = "block";
    canvas.focus();
    try {
        await host.ccall("runSWF", null, [], [], { async: true });   // resolves when the movie quits
    } catch (e) {
        // A throw inside a try body reaches a jmp_buf no real setjmp filled
        // (the guest's setjmp is stubbed, see guest_setjmp_shim.h): the host's
        // longjmp is a Wasm exception that no frame owns, so it escapes runSWF.
        if (typeof WebAssembly.Exception === "function" && e instanceof WebAssembly.Exception) {
            throw new Error("The movie threw an exception inside a try/catch block, which the in-browser build cannot catch yet (setjmp is stubbed; see the AVM2 in-browser assessment, §4). Use the downloadable build bundle for this SWF.");
        }
        throw e;
    }
}
