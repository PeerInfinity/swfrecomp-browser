// In-browser C -> WASM build (stage 2 of the in-browser recompiler).
//
//   1. Compile the generated C (the "guest") with clang running in the browser
//      (@wasmer/sdk + the clang/clang registry package), linked with
//      --import-memory and --global-base = GUEST_ARENA_BASE.
//   2. Hand the module to guest_loader.js, which loads the pre-built graphics
//      HOST (host/graphics_host.{js,wasm}), instantiates the guest against it
//      and runs the movie. The same loader ships inside the "runnable" zip.
//
// Design notes and the spike that fixed this shape:
// SWFRecompDocs/plans/in-browser-recompiler-refresh-assessment.md §7.

import { GUEST_ARENA_BASE, loadHost, runGuest, hostAlreadyUsed } from "./guest_loader.js";
export { loadHost, runGuest, hostAlreadyUsed };

const SDK_URL = "https://unpkg.com/@wasmer/sdk@0.8.0-beta.1/dist/index.mjs";
const HOST_DIR = "host/";

let sdk = null;
let clangPkg = null;

// --- Clang ---

export async function loadClang(setStatus) {
    if (clangPkg) return clangPkg;
    setStatus("Loading Wasmer SDK");
    sdk = await import(SDK_URL);
    await sdk.init({});
    setStatus("Loading Clang (~100 MB, cached by the browser after the first time)");
    clangPkg = await sdk.Wasmer.fromRegistry("clang/clang");
    return clangPkg;
}

async function fetchBytes(url) {
    const r = await fetch(url);
    if (!r.ok) throw new Error(`Failed to fetch ${url} (${r.status})`);
    return new Uint8Array(await r.arrayBuffer());
}

// Generated C referencing bridged runtime globals declares them `extern`
// inside function bodies, which clashes with the bridge_globals.h macro
// expansion; strip those. (Same treatment the original pipeline applied.)
function patchGeneratedSource(text) {
    return text
        .replace(/extern\s+ActionVar\s+g_override_this\s*;/g, "/* g_override_this: via bridge */")
        .replace(/extern\s+int\s+g_override_this_set\s*;/g, "/* g_override_this_set: via bridge */")
        .replace(/extern\s+int\s+quit_swf\s*;/g, "/* quit_swf: via bridge */")
        .replace(/extern\s+int\s+catch_up_mode\s*;/g, "/* catch_up_mode: via bridge */")
        .replace(/extern\s+int\s+g_tag_skip_mode\s*;/g, "/* g_tag_skip_mode: via bridge */")
        // clang 16 rejects a declaration directly after a label (C23 allows it,
        // gcc/newer clang accept it): give every bare `label_N:` a null statement.
        .replace(/^(\s*label_\w+:)\s*$/gm, "$1;");
}

export async function compileGuest(result, setStatus) {
    // AS3 (AVM2) SWFs compile against the AVM2 runtime headers and run on the
    // AVM2 host variant (guest_loader.js picks it from the guest's exports).
    // Their generated method bodies inline setjmp like AVM1 try/catch does;
    // the setjmp.h shim below turns it into a stubbed import.
    const isAvm2 = Object.keys(result.files).some(p => p.startsWith("RecompiledABC/"));
    const clang = await loadClang(setStatus);
    setStatus("Preparing sources");
    const project = new sdk.Directory();

    // Runtime headers + the few support headers generated code pulls in, from
    // the same snapshot the downloadable bundle uses.
    const manifest = await (await fetch("bundle/manifest.json")).json();
    const headers = manifest.filter(e => e.path.startsWith("runtime/include/") && e.path.endsWith(".h"));
    const dirs = new Set();
    for (const { path } of headers) {
        const rel = path.replace(/^runtime\//, "");        // include/...
        const d = rel.substring(0, rel.lastIndexOf("/"));
        const parts = d.split("/");
        for (let i = 1; i <= parts.length; i++) dirs.add(parts.slice(0, i).join("/"));
    }
    for (const d of [...dirs].sort()) await project.createDir(d);
    await Promise.all(headers.map(async ({ path }) => {
        await project.writeFile(path.replace(/^runtime\//, ""), await fetchBytes(`bundle/${path}`));
    }));
    for (const [src, dst] of [
        ["runtime/lib/c-hashmap/map.h", "map.h"],
        ["runtime/lib/o1heap/o1heap.h", "o1heap.h"],
        ["runtime/src/actionmodern/unicode_case_tables.h", "unicode_case_tables.h"],
    ]) await project.writeFile(dst, await fetchBytes(`bundle/${src}`));
    for (const f of ["guest_main_graphics.c", "bridge_globals.h", "bridge_globals.c"]) {
        await project.writeFile(f, await fetchBytes(HOST_DIR + f));
    }
    // setjmp.h shim ahead of the sysroot: host-compatible jmp_buf layout and an
    // importable (stubbed) setjmp — see guest_setjmp_shim.h for both reasons.
    await project.createDir("shim");
    await project.writeFile("shim/setjmp.h", await fetchBytes(HOST_DIR + "guest_setjmp_shim.h"));

    // Generated files, flattened (generated code includes them unqualified).
    const cFiles = ["guest_main_graphics.c", "bridge_globals.c"];
    const dec = new TextDecoder();
    for (const [path, bytes] of Object.entries(result.files)) {
        const name = path.substring(path.lastIndexOf("/") + 1);
        if (name.endsWith(".c") || name.endsWith(".h")) {
            await project.writeFile(name, new TextEncoder().encode(patchGeneratedSource(dec.decode(bytes))));
        } else {
            await project.writeFile(name, bytes);
        }
        if (name.endsWith(".c")) cFiles.push(name);
    }

    const arena = GUEST_ARENA_BASE;
    const args = [
        ...cFiles.map(f => `/project/${f}`),
        "-DDYNAMIC_GUEST",
        ...(isAvm2 ? ["-DSWF_AVM2", "-I/project/include/avm2"] : []),
        "-std=gnu2x",
        "-include", "string.h",
        "-include", "strings.h",
        "-I/project/shim", "-I/project", "-I/project/include", "-I/project/include/actionmodern",
        "-I/project/include/libswf", "-I/project/include/memory",
        "-O1", "-w",
        // The clang driver preset already adds --import-memory/--shared-memory.
        `-Wl,--export-table,--global-base=${arena},--allow-undefined,--export-all,--no-entry,-z,stack-size=8388608`,
        "-o", "/project/guest.wasm",
    ];
    setStatus(`Compiling ${cFiles.length} C files with in-browser clang`);
    const t0 = performance.now();
    // The SDK occasionally fails a run with "oneshot canceled" (a worker race
    // in @wasmer/sdk 0.8.0-beta.1); the same command succeeds on retry.
    let r;
    for (let attempt = 1; ; attempt++) {
        try {
            const inst = await clang.entrypoint.run({ args, mount: { "/project": project } });
            r = await inst.wait();
            break;
        } catch (e) {
            if (attempt >= 3 || !/oneshot|canceled|cancelled/i.test(String(e))) throw e;
            setStatus(`Compiler worker hiccup (${e}); retrying ${attempt}/2`);
            await new Promise(res => setTimeout(res, 500));
        }
    }
    const ms = Math.round(performance.now() - t0);
    if (!r.ok) {
        const err = (r.stderr || "").replace(/clang-16: warning: -Wl[^\n]*\n/g, "");
        throw new Error(`In-browser compile failed (code ${r.code}):\n${err || r.stdout || "(no output)"}`);
    }
    const bytes = await project.readFile("guest.wasm");
    return { bytes, ms, fileCount: cFiles.length, avm2: isAvm2 };
}
