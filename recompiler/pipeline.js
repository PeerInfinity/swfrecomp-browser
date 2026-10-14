// SWFRecomp In-Browser Recompiler
//   1. SWFRecomp.wasm (Emscripten build of the recompiler) turns the dropped
//      SWF into C files inside MEMFS.
//   2. The generated C is zipped together with the runtime snapshot served
//      from ./bundle/ (see SWFRecomp/scripts/deploy_wasm_demo.sh) into a
//      self-contained build bundle: ./build.sh + Emscripten -> WebGPU WASM page.
//
// The in-browser C -> WASM step (graphics host + in-browser clang) is a later
// stage; see SWFRecompDocs/plans/in-browser-recompiler-refresh-assessment.md.

const FFLATE_URL = "https://unpkg.com/fflate@0.8.2/esm/browser.js";
const GENERATED_DIRS = ["RecompiledTags", "RecompiledScripts", "RecompiledABC"];

// --- UI helpers ---

const $ = (id) => document.getElementById(id);

function setStep(id, state, text) {
    const el = $(id);
    el.className = "status-line " + state;
    if (text) el.textContent = text;
}

function showError(message) {
    const box = $("errorBox");
    box.style.display = "block";
    box.textContent = message;
}

function clearError() {
    $("errorBox").style.display = "none";
}

function fmtSize(n) {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    return `${(n / 1024 / 1024).toFixed(2)} MB`;
}

function safeName(fileName) {
    const base = fileName.replace(/\.swf$/i, "").replace(/[^A-Za-z0-9_.-]+/g, "_");
    return base || "swf";
}

// --- Build info ---

let buildInfo = null;
fetch("build_info.json").then(r => r.ok ? r.json() : null).then(info => {
    if (!info) return;
    buildInfo = info;
    const d = new Date(info.built_at);
    const date = d.toLocaleString(undefined, { year: "numeric", month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });
    $("build-info").textContent = `Recompiler snapshot ${info.commit}, built ${date}`;
}).catch(() => {});

// --- Step 1: recompile SWF -> C (fresh module instance per run) ---

let factoryLoaded = false;

async function loadRecompilerFactory() {
    if (factoryLoaded) return;
    // SWFRecomp.js is an Emscripten MODULARIZE script exposing SWFRecompModule().
    await new Promise((resolve, reject) => {
        const script = document.createElement("script");
        script.src = "SWFRecomp.js";
        script.onload = resolve;
        script.onerror = () => reject(new Error("Failed to load SWFRecomp.js"));
        document.head.appendChild(script);
    });
    factoryLoaded = true;
}

function collectDir(FS, dir, prefix, out) {
    let entries;
    try {
        entries = FS.readdir(dir).filter(f => f !== "." && f !== "..");
    } catch {
        return;
    }
    for (const name of entries) {
        const full = `${dir}/${name}`;
        const mode = FS.stat(full).mode;
        if (FS.isDir(mode)) {
            collectDir(FS, full, `${prefix}${name}/`, out);
        } else {
            out[`${prefix}${name}`] = FS.readFile(full);   // Uint8Array
        }
    }
}

async function recompileSWF(swfBytes) {
    const logLines = [];
    const Module = await SWFRecompModule({
        print: (t) => logLines.push(t),
        printErr: (t) => logLines.push(t),
    });

    Module.FS.writeFile("input.swf", new Uint8Array(swfBytes));
    // try_helper: emit every try-bearing method (AVM2) and try body (AVM1) as
    // a lifted function the RUNTIME calls through its own setjmp. The guest
    // toolchain here cannot lower setjmp/longjmp at all — a plain setjmp in
    // generated code silently links the WASIX libc's stack_checkpoint version,
    // which the loader stubs — so without this a throw inside `try` kills the
    // movie. See SWFRecompDocs/plans/avm2-in-browser-assessment.md §4.1.
    const configToml = [
        "[input]",
        'path_to_swf = "input.swf"',
        'output_tags_folder = "RecompiledTags"',
        'output_scripts_folder = "RecompiledScripts"',
        "try_helper = true",
        // tu_split: emit the per-DoABC-tag method bodies as ~1.5 MB chunk TUs
        // instead of one giant one. In-browser clang time grows super-linearly
        // with TU size — Seedling's 12.9 MB abc1_methods.c is 399 s as one TU
        // and 182 s as nine chunks in the same clang call, at half the peak
        // memory; Snailiad's 29.7 MB TU never finished at all. §1.1/§1.3 of
        // SWFRecompDocs/plans/avm2-in-browser-assessment.md.
        "tu_split = 1500000",
        // skip_avm1_payload: an AS3 SWF stops emitting bitmap_data /
        // sound_data / video_data into draws.c. Those arrays are read only by
        // tagInit's defineBitmap / tagDefineSound and by the AVM1 frame
        // bodies' tagVideoFrame / tagSoundStreamBlock, and runSWF_avm2 reaches
        // none of them — AVM2 carries its own copies of the same bytes in
        // RecompiledABC/abc_timeline.c. For the original Seedling that is
        // 142.1 MB of C down to 3.4 MB, and 22.0 MB of static data down to
        // 1.3 MB. An AVM1 SWF is unaffected at any setting. §2.2 of
        // SWFRecompDocs/plans/avm2-in-browser-assessment.md.
        "skip_avm1_payload = true",
        "",
    ].join("\n");
    Module.FS.writeFile("config.toml", configToml);

    let exitCode = 0;
    let failure = null;
    try {
        exitCode = Module.callMain(["config.toml"]) || 0;
    } catch (e) {
        if (e && e.name === "ExitStatus") {
            exitCode = e.status;
        } else {
            failure = e;
        }
    }

    const files = {};
    for (const dir of GENERATED_DIRS) collectDir(Module.FS, dir, `${dir}/`, files);

    const log = logLines.join("\n");
    if (failure) {
        throw new Error(`Recompiler crashed: ${failure.message || failure}\n\n${log}`);
    }
    if (exitCode !== 0 || Object.keys(files).length === 0) {
        throw new Error(`Recompiler exited with code ${exitCode} and produced ${Object.keys(files).length} files.\n\n${log}`);
    }
    return { files, log, configToml };
}

// --- Step 2: zip bundles ---

let fflate = null;
async function loadFflate() {
    if (!fflate) fflate = await import(FFLATE_URL);
    return fflate;
}

let bundleManifest = null;
async function loadBundleManifest() {
    if (bundleManifest) return bundleManifest;
    const r = await fetch("bundle/manifest.json");
    if (!r.ok) throw new Error("bundle/manifest.json not found — the page was deployed without the runtime snapshot");
    bundleManifest = await r.json();
    return bundleManifest;
}

// Fetch every runtime snapshot file (with limited concurrency); returns
// {relativePath: Uint8Array}.
async function fetchBundleFiles(onProgress) {
    const manifest = await loadBundleManifest();
    const out = {};
    let done = 0;
    const queue = manifest.slice();
    const worker = async () => {
        while (queue.length) {
            const { path } = queue.shift();
            const r = await fetch(`bundle/${path}`);
            if (!r.ok) throw new Error(`Failed to fetch bundle/${path} (${r.status})`);
            out[path] = new Uint8Array(await r.arrayBuffer());
            done++;
            if (onProgress) onProgress(done, manifest.length);
        }
    };
    await Promise.all(Array.from({ length: 8 }, worker));
    return out;
}

function triggerDownload(bytes, fileName, mime = "application/zip") {
    const blob = new Blob([bytes], { type: mime });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = fileName;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10_000);
}

function zipEntries(entries) {
    // entries: {path: Uint8Array}; build.sh gets the executable bit.
    const tree = {};
    for (const [path, bytes] of Object.entries(entries)) {
        // os: 3 (Unix) makes unzip honour the mode in the external attrs, so
        // every entry needs one: 0644 for files, 0755 for the build script.
        const mode = path.endsWith("/build.sh") ? 0o755 : 0o644;
        tree[path] = [bytes, { level: 6, attrs: (0o100000 | mode) << 16 }];
    }
    return fflate.zipSync(tree, { os: 3 });
}

async function downloadSources(result) {
    await loadFflate();
    const root = `${result.name}/`;
    const entries = {};
    for (const [p, b] of Object.entries(result.files)) entries[root + p] = b;
    entries[root + `${result.name}.swf`] = new Uint8Array(result.swfBytes);
    entries[root + "config.toml"] = new TextEncoder().encode(result.configToml);
    entries[root + "recompiler.log"] = new TextEncoder().encode(result.log);
    triggerDownload(zipEntries(entries), `${result.name}-recompiled-c.zip`);
}

async function downloadBundle(result, setHint) {
    await loadFflate();
    const runtime = await fetchBundleFiles((d, n) => setHint(`Fetching runtime snapshot ${d}/${n}`));
    setHint("Zipping");
    const root = `${result.name}/`;
    const entries = {};
    for (const [p, b] of Object.entries(runtime)) {
        if (p === "manifest.json") continue;
        entries[root + p] = b;
    }
    for (const [p, b] of Object.entries(result.files)) entries[root + p] = b;
    entries[root + `${result.name}.swf`] = new Uint8Array(result.swfBytes);
    entries[root + "config.toml"] = new TextEncoder().encode(result.configToml);
    entries[root + "recompiler.log"] = new TextEncoder().encode(result.log);
    entries[root + "NAME"] = new TextEncoder().encode(result.name + "\n");
    entries[root + "BUNDLE_INFO.json"] = new TextEncoder().encode(JSON.stringify({
        name: result.name,
        source_swf: result.sourceLabel,
        avm2: Object.keys(result.files).some(p => p.startsWith("RecompiledABC/")),
        recompiler_snapshot: buildInfo,
        created_at: new Date().toISOString(),
    }, null, 2) + "\n");
    const zipped = zipEntries(entries);
    triggerDownload(zipped, `${result.name}-swfrecomp-bundle.zip`);
    setHint(`Bundle: ${fmtSize(zipped.length)} zipped`);
}

// --- Main flow ---

let currentResult = null;

function renderResult(result) {
    const rows = Object.entries(result.files).sort(([a], [b]) => a.localeCompare(b));
    let total = 0;
    $("fileRows").innerHTML = rows.map(([p, b]) => {
        total += b.length;
        return `<tr><td>${p}</td><td class="size">${fmtSize(b.length)}</td></tr>`;
    }).join("") + `<tr><td><b>${rows.length} files</b></td><td class="size"><b>${fmtSize(total)}</b></td></tr>`;
    $("log").textContent = result.log || "(no output)";
    $("dlHint").textContent = "";
    $("result").classList.add("visible");
}

async function processSwf(swfBytes, name, sourceLabel) {
    clearError();
    currentResult = null;
    $("result").classList.remove("visible");
    $("runStatus").style.display = "none";
    $("dlWasmBtn").style.display = "none";
    $("dlRunnableBtn").style.display = "none";
    lastGuest = null;
    $("status").classList.add("visible");
    setStep("step-load", "active", "Loading recompiler ...");
    setStep("step-recompile", "", "Recompiling SWF to C");
    try {
        await loadRecompilerFactory();
        setStep("step-load", "done", "Recompiler loaded");
        setStep("step-recompile", "active", "Recompiling SWF to C ...");
        const t0 = performance.now();
        const { files, log, configToml } = await recompileSWF(swfBytes);
        const ms = Math.round(performance.now() - t0);
        const isAvm2 = Object.keys(files).some(p => p.startsWith("RecompiledABC/"));
        setStep("step-recompile", "done",
            `Recompiled ${sourceLabel} to ${Object.keys(files).length} C files in ${ms} ms (${isAvm2 ? "AVM2 / AS3" : "AVM1"})`);
        currentResult = { files, log, configToml, swfBytes, name, sourceLabel };
        renderResult(currentResult);
    } catch (e) {
        console.error(e);
        const active = ["step-load", "step-recompile"].find(id => $(id).classList.contains("active"));
        if (active) setStep(active, "error");
        showError(e.message);
    }
}

$("dlBundleBtn").addEventListener("click", async () => {
    if (!currentResult) return;
    const btn = $("dlBundleBtn");
    btn.disabled = true;
    try {
        await downloadBundle(currentResult, (t) => { $("dlHint").textContent = t; });
    } catch (e) {
        console.error(e);
        showError(e.message);
    } finally {
        btn.disabled = false;
    }
});

let lastGuest = null;
$("dlWasmBtn").addEventListener("click", () => {
    if (!lastGuest) return;
    triggerDownload(lastGuest.bytes, `${lastGuest.name}-guest.wasm`, "application/wasm");
});

// Runnable zip: the guest module + the graphics host + the standalone loader +
// a player page + coi-serviceworker (cross-origin isolation for the host's
// shared memory). Serve the folder over HTTP and open it in Chrome 137+.
// AS3 guests ship with the AVM2 host variant (guest_loader.js picks it by name).
const runnableFiles = (avm2) => {
    const h = avm2 ? "graphics_host_avm2" : "graphics_host";
    return [
        [`host/${h}.js`, `${h}.js`],
        [`host/${h}.wasm`, `${h}.wasm`],
        ["guest_loader.js", "guest_loader.js"],
        ["coi-serviceworker.js", "coi-serviceworker.js"],
    ];
};
$("dlRunnableBtn").addEventListener("click", async () => {
    if (!lastGuest) return;
    const btn = $("dlRunnableBtn");
    btn.disabled = true;
    try {
        await loadFflate();
        const root = `${lastGuest.name}-runnable/`;
        const entries = {};
        $("runHint").textContent = "Fetching host files";
        await Promise.all(runnableFiles(lastGuest.avm2).map(async ([src, dst]) => {
            const r = await fetch(src);
            if (!r.ok) throw new Error(`Failed to fetch ${src} (${r.status})`);
            entries[root + dst] = new Uint8Array(await r.arrayBuffer());
        }));
        const tpl = await (await fetch("runner_template.html")).text();
        entries[root + "index.html"] = new TextEncoder().encode(tpl.replace(/{{NAME}}/g, lastGuest.name));
        entries[root + `${lastGuest.name}-guest.wasm`] = lastGuest.bytes;
        entries[root + "README.md"] = new TextEncoder().encode([
            `# ${lastGuest.name} — runnable build`, "",
            "Compiled by the SWFRecomp in-browser recompiler. No build step needed.", "",
            "    python3 -m http.server 8080      # in this folder", "",
            "then open http://localhost:8080/ in Chrome 137 or newer (WebGPU + JSPI required).",
            "It must be served over HTTP (any static host works); file:// does not, because",
            "coi-serviceworker.js has to enable cross-origin isolation for the shared-memory host.", "",
            "Files: index.html (player page), guest_loader.js (instantiates the guest against the",
            `host), ${lastGuest.name}-guest.wasm (this SWF's compiled code + data), ${lastGuest.avm2 ? "graphics_host_avm2" : "graphics_host"}.js/.wasm`,
            `(the SWFModernRuntime graphics runtime, snapshot ${buildInfo ? buildInfo.commit : "unknown"}), coi-serviceworker.js.`, "",
        ].join("\n"));
        $("runHint").textContent = "Zipping";
        const zipped = zipEntries(entries);
        triggerDownload(zipped, `${lastGuest.name}-runnable.zip`);
        $("runHint").textContent = `Runnable: ${fmtSize(zipped.length)} zipped — unzip, serve the folder over HTTP, open in Chrome 137+.`;
    } catch (e) {
        console.error(e);
        showError(e.message);
    } finally {
        btn.disabled = false;
    }
});

$("runBtn").addEventListener("click", async () => {
    if (!currentResult) return;
    const gfx = await import("./pipeline_graphics.js");
    if (gfx.hostAlreadyUsed()) { showError("Reload the page to run another SWF in the browser."); return; }
    const btn = $("runBtn");
    btn.disabled = true;
    clearError();
    $("runStatus").style.display = "block";
    for (const id of ["step-host", "step-guest", "step-run"]) setStep(id, "");
    const canvas = $("canvas");
    const runLog = [];
    const log = (t) => { runLog.push(t); console.log("[host] " + t); };
    try {
        setStep("step-guest", "active", "Compiling generated C in the browser ...");
        const guest = await gfx.compileGuest(currentResult, (t) => setStep("step-guest", "active", t + " ..."));
        setStep("step-guest", "done", `Compiled ${guest.fileCount} C files to a ${fmtSize(guest.bytes.length)} guest module in ${(guest.ms / 1000).toFixed(1)} s`);
        lastGuest = { bytes: guest.bytes, name: currentResult.name, avm2: !!guest.avm2 };
        $("dlWasmBtn").style.display = "";
        $("dlRunnableBtn").style.display = "";
        $("runHint").textContent = `The guest .wasm holds this SWF's compiled code and data; it runs only alongside the graphics host (host/${guest.avm2 ? "graphics_host_avm2" : "graphics_host"}.wasm), the way this page loads it.`;
        setStep("step-host", "active", `Loading ${guest.avm2 ? "AVM2 " : ""}graphics host ...`);
        await gfx.loadHost({ hostDir: "host/", canvas, log, avm2: !!guest.avm2 });
        setStep("step-host", "done", `${guest.avm2 ? "AVM2 " : ""}Graphics host loaded`);
        setStep("step-run", "active", "Instantiating guest ...");
        await gfx.runGuest({ guestBytes: guest.bytes, hostDir: "host/", canvas, log, setStatus: (t) => setStep("step-run", "active", t) });
        setStep("step-run", "done", "Finished");
    } catch (e) {
        console.error(e);
        const active = ["step-host", "step-guest", "step-run"].find(id => $(id).classList.contains("active"));
        if (active) setStep(active, "error");
        showError((e.stack && e.stack.includes(e.message) ? e.stack : e.message) + (runLog.length ? "\n\nHost log:\n" + runLog.slice(-20).join("\n") : ""));
        btn.disabled = false;
    }
});

$("dlSourcesBtn").addEventListener("click", async () => {
    if (!currentResult) return;
    try {
        await downloadSources(currentResult);
    } catch (e) {
        console.error(e);
        showError(e.message);
    }
});

// --- File drop / click handlers ---

const dropZone = $("dropZone");
const fileInput = $("fileInput");

dropZone.addEventListener("click", () => fileInput.click());
dropZone.addEventListener("dragover", (e) => { e.preventDefault(); dropZone.classList.add("dragover"); });
dropZone.addEventListener("dragleave", () => dropZone.classList.remove("dragover"));
dropZone.addEventListener("drop", (e) => {
    e.preventDefault();
    dropZone.classList.remove("dragover");
    const file = e.dataTransfer.files[0];
    if (file) handleFile(file);
});
fileInput.addEventListener("change", () => {
    const file = fileInput.files[0];
    if (file) handleFile(file);
});

function handleFile(file) {
    if (!file.name.toLowerCase().endsWith(".swf")) {
        showError("Please select a .swf file");
        return;
    }
    setListOpen(false);
    dropZone.querySelector("p").textContent = file.name;
    const reader = new FileReader();
    reader.onload = () => processSwf(reader.result, safeName(file.name), file.name);
    reader.readAsArrayBuffer(file);
}

// --- Example SWF search (shared catalog with the demo pages) ---

let demoList = [];     // [{name, kind, url}]
let selectedDemo = null;

fetch("../catalog.json").then(r => r.json()).then(data => {
    demoList = (data.tests || []).map(t => ({
        name: t.name,
        kind: t.type,
        url: t.type === "graphics" ? `../examples/graphics/${t.name}/test.swf` : `../examples/${t.name}/test.swf`,
    }));
    demoList.sort((a, b) => (a.kind === b.kind ? a.name.localeCompare(b.name) : a.kind === "graphics" ? -1 : 1));
    updateDemoSearch();
}).catch(() => {
    demoList = [{ name: "keyboard_input", kind: "graphics", url: "../examples/graphics/keyboard_input/test.swf" }];
    updateDemoSearch();
});

const demoSearch = $("demoSearch");
const demoResults = $("demoResults");
const browseBtn = $("demoBrowseBtn");

function updateDemoSearch() {
    demoSearch.placeholder = `Search example SWFs... (${demoList.length} available)`;
}

function highlightMatch(text, query) {
    if (!query) return text;
    const idx = text.toLowerCase().indexOf(query.toLowerCase());
    if (idx < 0) return text;
    return text.slice(0, idx) + `<span class="match">${text.slice(idx, idx + query.length)}</span>` + text.slice(idx + query.length);
}

function setListOpen(open) {
    if (open) {
        filterDemos();
    } else {
        demoResults.style.display = "none";
        browseBtn.style.borderColor = "#555";
        browseBtn.style.color = "#888";
    }
}

function matches(query) {
    const q = query.toLowerCase();
    return q ? demoList.filter(d => d.name.toLowerCase().includes(q)) : demoList;
}

function filterDemos() {
    const query = demoSearch.value.trim();
    const filtered = matches(query);
    demoResults.innerHTML = filtered.length === 0
        ? `<div style="padding: 0.4rem 0.8rem; color: #666; font-size: 0.85em;">No matches</div>`
        : filtered.map(d =>
            `<div class="demo-item${selectedDemo && d.name === selectedDemo.name && d.kind === selectedDemo.kind ? ' selected' : ''}" data-name="${d.name}" data-kind="${d.kind}">${highlightMatch(d.name, query)}<span class="kind">${d.kind}</span></div>`
        ).join("");
    demoResults.style.display = "block";
    browseBtn.style.borderColor = "#4ecca3";
    browseBtn.style.color = "#4ecca3";
}

function isListOpen() { return demoResults.style.display !== "none"; }

demoSearch.addEventListener("input", () => setListOpen(true));
demoSearch.addEventListener("focus", () => setListOpen(true));
demoSearch.addEventListener("blur", () => setTimeout(() => setListOpen(false), 200));
browseBtn.addEventListener("click", () => {
    if (isListOpen()) setListOpen(false);
    else { setListOpen(true); demoSearch.focus(); }
});
demoSearch.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
        const match = matches(demoSearch.value.trim())[0];
        if (match) {
            selectedDemo = match;
            demoSearch.value = match.name;
            demoResults.style.display = "none";
            runSelectedDemo();
        }
    }
});
demoResults.addEventListener("click", (e) => {
    const item = e.target.closest(".demo-item");
    if (!item) return;
    selectedDemo = demoList.find(d => d.name === item.dataset.name && d.kind === item.dataset.kind) || null;
    if (selectedDemo) demoSearch.value = selectedDemo.name;
    setListOpen(false);
});

async function runSelectedDemo() {
    if (!selectedDemo) return;
    const label = `${selectedDemo.kind}/${selectedDemo.name}/test.swf`;
    dropZone.querySelector("p").textContent = label;
    const resp = await fetch(selectedDemo.url);
    if (!resp.ok) {
        showError(`Could not load ${selectedDemo.url}`);
        return;
    }
    processSwf(await resp.arrayBuffer(), safeName(selectedDemo.name), label);
}

$("demoBtn").addEventListener("click", () => {
    setListOpen(false);
    if (!selectedDemo) {
        const match = matches(demoSearch.value.trim())[0];
        if (!match) return;
        selectedDemo = match;
        demoSearch.value = match.name;
    }
    runSelectedDemo();
});
