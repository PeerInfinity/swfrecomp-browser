// Walk API: drive the recompiler page by messages instead of clicks.
//
// Another page (an app panel, a test, a tutorial walk) can make this page load
// a SWF, recompile it, compile it with the in-browser clang, run it, and read
// back what the run printed and drew. It cannot script this page's DOM: an
// isolated page in a new tab has a dead opener handle (COOP), and an isolated
// iframe (Document-Isolation-Policy) is a different agent cluster. Messages
// still cross both, so the API is messages.
//
// Transports (both carry the same messages):
//   - BroadcastChannel, name "swfrecomp-walk", or `?walkChannel=<name>` in this
//     page's URL to keep concurrent walkers apart. Works to a COOP-severed tab.
//   - window.postMessage from the embedding parent or the opener. Replies go
//     back to the message's source window.
// Only messages from this page's own origin are accepted.
//
// Messages are plain objects tagged { proto: "swfrecomp-walk", v: 1 }.
//   request : { proto, v, id, cmd, ...args }
//   reply   : { proto, v, re: id, ok: true, result } | { proto, v, re: id, ok: false, error, stage? }
//   event   : { proto, v, event, ... }      (no `re`; broadcast on every transport)
// Commands, events, readiness and an example exchange: WALK_API.md (next to
// this file). Bump PROTOCOL_VERSION on any incompatible change.

export const PROTOCOL = "swfrecomp-walk";
export const PROTOCOL_VERSION = 1;

const STAGES = ["load", "recompile", "compile", "run"];

export function installWalkApi(page) {
    const params = new URLSearchParams(location.search);
    const channelName = params.get("walkChannel") || PROTOCOL;
    const instance = Math.random().toString(36).slice(2, 10);
    const bc = typeof BroadcastChannel === "function" ? new BroadcastChannel(channelName) : null;
    const t0 = performance.now();

    const state = {
        busy: null,                    // the command in flight
        source: null,                  // { name, label, bytes: ArrayBuffer }
        stages: {},                    // stage -> { state, ms, ...result } (latest)
        runError: null,                // a failure after the movie started
    };
    const peers = new Set();           // windows that have sent us a request

    const tag = (o) => ({ proto: PROTOCOL, v: PROTOCOL_VERSION, ...o });
    const sendEvent = (o) => {
        const m = tag(o);
        if (bc) bc.postMessage(m);
        for (const w of peers) { try { w.postMessage(m, location.origin); } catch { peers.delete(w); } }
        if (window.parent !== window && !peers.has(window.parent)) {
            try { window.parent.postMessage(m, location.origin); } catch { /* cross-origin parent: not ours */ }
        }
    };

    const isolated = () => !!globalThis.crossOriginIsolated;
    const jspi = () => typeof WebAssembly.Suspending === "function";
    const capabilities = () => ({
        recompile: true,
        compile: isolated(),
        run: isolated() && jspi() && !!navigator.gpu,
    });
    const hello = async () => ({
        protocol: PROTOCOL, version: PROTOCOL_VERSION, instance,
        page: location.href,
        build: page.buildInfo(),
        framed: window.top !== window,
        crossOriginIsolated: isolated(),
        jspi: jspi(),
        webgpu: !!navigator.gpu,
        capabilities: capabilities(),
        examples: await page.examples(),
        uptimeMs: Math.round(performance.now() - t0),
    });
    const status = () => ({
        instance,
        busy: state.busy,
        source: state.source ? { name: state.source.name, label: state.source.label, bytes: state.source.bytes.byteLength } : null,
        stages: state.stages,
        traceLines: page.runLog.length,
        runError: state.runError,
    });

    // Each stage: emit `stage` events around it and keep its latest outcome.
    async function stage(name, fn) {
        const begin = performance.now();
        state.stages[name] = { state: "active" };
        sendEvent({ event: "stage", stage: name, state: "active" });
        try {
            const result = await fn((text) => sendEvent({ event: "stage", stage: name, state: "active", text }));
            const out = { ms: Math.round(performance.now() - begin), ...result };
            state.stages[name] = { state: "done", ...out };
            sendEvent({ event: "stage", stage: name, state: "done", ...out });
            return out;
        } catch (e) {
            const error = String(e && e.message || e);
            state.stages[name] = { state: "error", ms: Math.round(performance.now() - begin), error };
            sendEvent({ event: "stage", stage: name, state: "error", error });
            e.walkStage = name;
            throw e;
        }
    }

    function toBuffer(b) {
        if (b instanceof ArrayBuffer) return b;
        if (ArrayBuffer.isView(b)) return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
        throw new Error("`bytes` must be an ArrayBuffer or a typed array");
    }

    const load = (args) => stage("load", async () => {
        let bytes, name, label;
        if (args.example) {
            const ex = await page.exampleUrl(args.example);
            if (!ex) throw new Error(`No example named "${args.example}" in catalog.json`);
            const r = await fetch(ex.url);
            if (!r.ok) throw new Error(`Could not fetch ${ex.url} (${r.status})`);
            bytes = await r.arrayBuffer(); name = ex.name; label = ex.label;
        } else if (args.url) {
            const r = await fetch(new URL(args.url, location.href));
            if (!r.ok) throw new Error(`Could not fetch ${args.url} (${r.status})`);
            bytes = await r.arrayBuffer();
            label = String(args.url).split("/").pop() || "input.swf";
            name = args.name || label.replace(/\.swf$/i, "");
        } else if (args.bytes) {
            bytes = toBuffer(args.bytes); name = args.name || "input"; label = args.name ? `${args.name}.swf` : "input.swf";
        } else {
            throw new Error("load needs one of `example`, `url` or `bytes`");
        }
        state.source = { name, label, bytes };
        page.showSource(label);
        return { name, label, bytes: bytes.byteLength };
    });

    const recompile = () => stage("recompile", async () => {
        if (!state.source) throw new Error("Nothing loaded: send `load` first");
        const r = await page.recompile(state.source.bytes, state.source.name, state.source.label);
        const files = Object.entries(r.files);
        return {
            recompileMs: r.ms, avm2: r.avm2, fileCount: files.length,
            cBytes: files.reduce((n, [, b]) => n + b.length, 0),
        };
    });

    const compile = () => stage("compile", async (progress) => {
        if (!capabilities().compile) throw new Error("This page is not cross-origin isolated here, so the in-browser compile cannot run. Open it top-level or in a new tab (or in an iframe on a browser that honours Document-Isolation-Policy).");
        const g = await page.compile({ onStatus: progress });
        return { clangMs: g.ms, totalMs: g.totalMs, fileCount: g.fileCount, guestBytes: g.bytes.length, avm2: g.avm2 };
    });

    const run = () => stage("run", async (progress) => {
        if (!capabilities().run) throw new Error(`The movie cannot run here: ${!isolated() ? "not cross-origin isolated" : !jspi() ? "no WebAssembly JSPI (Chrome 137+)" : "no WebGPU"}`);
        const r = await page.run({ onStatus: progress });
        r.done.then(
            () => sendEvent({ event: "quit" }),
            (e) => {
                state.runError = String(e && e.message || e);
                sendEvent({ event: "runError", error: state.runError });
            });
        return { status: r.status, width: r.width, height: r.height };
    });

    // Pixel readback. A WebGPU canvas reads back as transparent once its frame
    // is presented (drawImage, createImageBitmap and toDataURL all measured
    // blank), so the copy is taken in the same task as a frame's GPU submit,
    // before the browser presents it: GPUQueue.submit is wrapped for the one
    // frame. The movie must still be drawing frames (the runtime redraws every
    // tick). `points` are canvas pixels, [x, y] pairs; the canvas is the stage
    // size.
    function captureNextFrame(src, timeoutMs) {
        const c = document.createElement("canvas");
        c.width = src.width; c.height = src.height;
        const ctx = c.getContext("2d", { willReadFrequently: true });
        if (typeof GPUQueue !== "function") {
            ctx.drawImage(src, 0, 0);
            return Promise.resolve(c);
        }
        const proto = GPUQueue.prototype, orig = proto.submit;
        return new Promise((resolve, reject) => {
            const restore = () => { if (proto.submit === hooked) proto.submit = orig; };
            const timer = setTimeout(() => { restore(); reject(new Error(`no frame was drawn within ${timeoutMs} ms (the movie stopped rendering?)`)); }, timeoutMs);
            function hooked(...a) {
                const r = orig.apply(this, a);
                restore();
                clearTimeout(timer);
                try { ctx.drawImage(src, 0, 0); resolve(c); } catch (e) { reject(e); }
                return r;
            }
            proto.submit = hooked;
        });
    }

    async function readback(args) {
        const src = page.canvas();
        if (!state.stages.run || state.stages.run.state !== "done") throw new Error("Nothing is running: send `run` first");
        const c = await captureNextFrame(src, args.timeoutMs || 5000);
        const ctx = c.getContext("2d");
        const data = ctx.getImageData(0, 0, c.width, c.height).data;
        const hex = (i) => Array.from(data.subarray(i, i + 4), (b) => b.toString(16).padStart(2, "0")).join("");
        const pixels = (args.points || []).map(([x, y]) => {
            const xi = Math.floor(x), yi = Math.floor(y);
            if (xi < 0 || yi < 0 || xi >= c.width || yi >= c.height) return { x, y, rgba: null };
            return { x, y, rgba: hex((yi * c.width + xi) * 4) };
        });
        let nonBlank = 0;
        for (let i = 0; i < data.length; i += 4) if (data[i + 3] && (data[i] | data[i + 1] | data[i + 2])) nonBlank++;
        const out = { width: c.width, height: c.height, pixels, nonBlankPixels: nonBlank };
        if (args.png) out.png = c.toDataURL("image/png");
        return out;
    }

    const commands = {
        hello,
        status: async () => status(),
        load,
        recompile,
        compile,
        run,
        // load + recompile + compile + run (or up to `until`), stopping at the first error.
        pipeline: async (args) => {
            const until = args.until || "run";
            if (!STAGES.includes(until)) throw new Error(`until must be one of ${STAGES.join(", ")}`);
            const out = {};
            const begin = performance.now();
            for (const s of STAGES) {
                out[s] = s === "load" ? await load(args) : await commands[s](args);
                if (s === until) break;
            }
            return { stages: out, totalMs: Math.round(performance.now() - begin) };
        },
        // The host's printed lines (trace() output among them), from index `since`.
        traces: async (args) => {
            const since = Math.max(0, args.since | 0);
            return { lines: page.runLog.slice(since), next: page.runLog.length };
        },
        readback,
        // Reload the page (the graphics host runs one SWF per page load). The
        // reply goes out first; a new `ready` with a new `instance` follows.
        reload: async () => { setTimeout(() => location.reload(), 50); return { reloading: true }; },
    };
    // Mutating commands; `hello`, `status`, `traces` and `readback` may run alongside one.
    const exclusive = new Set(["load", "recompile", "compile", "run", "pipeline", "reload"]);

    async function handle(msg, reply) {
        if (!msg || msg.proto !== PROTOCOL || msg.re !== undefined || msg.event !== undefined) return;
        const id = msg.id;
        if (msg.v !== PROTOCOL_VERSION) {
            reply(tag({ re: id, ok: false, error: `protocol version ${msg.v} not supported; this page speaks ${PROTOCOL_VERSION}` }));
            return;
        }
        const fn = commands[msg.cmd];
        if (!fn) { reply(tag({ re: id, ok: false, error: `unknown command "${msg.cmd}"; known: ${Object.keys(commands).join(", ")}` })); return; }
        if (exclusive.has(msg.cmd)) {
            if (state.busy) { reply(tag({ re: id, ok: false, error: `busy with "${state.busy}"` })); return; }
            state.busy = msg.cmd;
        }
        try {
            const { proto, v, id: _id, cmd, ...args } = msg;
            const result = await fn(args);
            reply(tag({ re: id, ok: true, result }));
        } catch (e) {
            reply(tag({ re: id, ok: false, error: String(e && e.message || e), ...(e && e.walkStage ? { stage: e.walkStage } : {}) }));
        } finally {
            if (exclusive.has(msg.cmd)) state.busy = null;
        }
    }

    // Stream the host's lines as they arrive.
    page.onRunLog((line, index) => sendEvent({ event: "trace", index, line }));

    // Wait until the page has SETTLED before listening: on a first visit
    // coi-serviceworker reloads the page once, and a command accepted before
    // that reload would be lost. Settled = isolated already, or controlled by
    // the service worker (isolated or not: a frame without DIP stays
    // un-isolated), or no service worker is coming at all. Otherwise the
    // reload is on its way; if it never comes (registration failed), answer
    // after a grace period, un-isolated. A walker that sees a `ready` with a
    // new `instance` must treat it as a fresh page.
    // Measured: on a first visit an un-isolated instance can already be
    // controlled and is still reloaded, so "controlled" alone settles only a
    // frame (where no DIP means it stays un-isolated), and only after a grace
    // period in which a pending reload would have unloaded it.
    const settled = new Promise((resolve) => {
        const sw = navigator.serviceWorker;
        if (isolated() || !sw || !window.isSecureContext) return resolve("now");
        if (sw.controller && window.top !== window) return setTimeout(() => resolve("framed"), 1500);
        setTimeout(() => resolve("timeout"), 10000);
    });
    settled.then(async () => {
        if (bc) bc.onmessage = (ev) => handle(ev.data, (m) => bc.postMessage(m));
        window.addEventListener("message", (ev) => {
            if (ev.origin !== location.origin || !ev.source || ev.source === window) return;
            if (!ev.data || ev.data.proto !== PROTOCOL) return;
            peers.add(ev.source);
            handle(ev.data, (m) => ev.source.postMessage(m, location.origin));
        });
        if (window.opener) { try { peers.add(window.opener); } catch { /* severed */ } }
        sendEvent({ event: "ready", ...(await hello()) });
    });
}
