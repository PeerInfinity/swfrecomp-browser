# Walk API: driving the recompiler page by messages

`walk_api.js` lets another page load a SWF, recompile it to C, compile the C with the
in-browser clang, run the movie, and read back what the run printed and drew. All of it
works without touching this page's DOM. An app panel, a test, or a tutorial walk can use
it.

It is messages because the page has to be **cross-origin isolated** to compile, and every
isolated arrangement cuts DOM access:
- **A new tab:** `coi-serviceworker.js` gives the tab COOP `same-origin`, so the opener's
  window handle is severed. BroadcastChannel still reaches it, since it is the same
  origin and storage key.
- **An iframe:** isolation comes from `Document-Isolation-Policy: isolate-and-credentialless`,
  which the service worker adds. That puts the frame in its own agent cluster, so the parent
  cannot script it. `postMessage` still works.
  - **Where it works:** Chrome/Chromium with DIP support. Measured: Chrome 153, Chromium
    141 (full build).
  - **Where it does not:** the Playwright **headless shell** ignores DIP. There a frame
    stays un-isolated: it recompiles to C, and `compile` is refused with a reason.
  - **The `<iframe>` element** should carry `allow="cross-origin-isolated"`.
- **Top level:** the page itself, on its own; no channel is needed.

## Transports

| Transport | Walker side | Notes |
|---|---|---|
| BroadcastChannel | `new BroadcastChannel(name)`; the page listens on `swfrecomp-walk`, or on `?walkChannel=<name>` | Works to a COOP-severed tab. Use a unique `walkChannel` per walk, so two open pages never both answer. |
| postMessage | `frame.contentWindow.postMessage(msg, origin)`, or `window.opener` | Replies go to the sender's window; events go to every window that has sent a request, and to `window.parent` |

Only messages from the page's **own origin** are accepted, and replies and events are
posted to that origin.

## Message shapes

Every message carries `proto: "swfrecomp-walk"` and `v: 1`.

```
request : { proto, v, id, cmd, ...args }
reply   : { proto, v, re: id, ok: true,  result }
          { proto, v, re: id, ok: false, error, stage? }     // stage = where a pipeline failed
event   : { proto, v, event, ... }                           // unsolicited, no `re`
```

A reply with an unknown `v` says which version the page speaks. Bump `PROTOCOL_VERSION`
in `walk_api.js` on any incompatible change.

## Readiness

On a first visit the service worker reloads the page once. The API therefore installs its
listeners only once the page has **settled**, meaning any one of:
- it is isolated;
- it is a frame already controlled by the service worker (after a 1.5 s grace);
- no service worker can come;
- 10 s have passed.

Then it sends a `ready` event, which carries the same body as the `hello` reply.

The walker should send `hello` every half second until it gets an answer, and must also
watch for `ready`. **A `ready` with a new `instance` means a fresh page**: any request in
flight was lost, so resend it.

## Commands

| `cmd` | Args | `result` |
|---|---|---|
| `hello` | — | `{ protocol, version, instance, page, build, framed, crossOriginIsolated, jspi, webgpu, capabilities: {recompile, compile, run}, examples: [{name, kind}], uptimeMs }` |
| `status` | — | `{ instance, busy, source, stages: {load, recompile, compile, run}, traceLines, runError }` |
| `load` | **one of** `example` (a catalog name), `url` (resolved against the page), or `bytes` (ArrayBuffer or typed array) plus an optional `name` | `{ ms, name, label, bytes }` |
| `recompile` | — | `{ ms, recompileMs, avm2, fileCount, cBytes }` |
| `compile` | — | `{ ms, clangMs, totalMs, fileCount, guestBytes, avm2 }`. `ms` includes loading the SDK and fetching and verifying the clang package; `clangMs` is the clang run alone |
| `run` | — | `{ ms, status, width, height }`. Replies once the movie is **running**; most movies never quit |
| `pipeline` | the `load` args, plus `until` (`load`, `recompile`, `compile` or `run`; default `run`) | `{ stages: {load, recompile, compile, run}, totalMs }`, stopping at the first error (the reply's `stage` names it) |
| `traces` | `since` (an index, default 0) | `{ lines, next }`: every line the graphics host printed, including the movie's `trace()` output |
| `readback` | `points: [[x, y], …]` in canvas pixels (the canvas is the stage size), `png` (bool), `timeoutMs` (default 5000) | `{ width, height, pixels: [{x, y, rgba: "rrggbbaa"}], nonBlankPixels, png? }` |
| `reload` | — | `{ reloading: true }`, then the page reloads and a new `ready` follows. The graphics host runs **one SWF per page load** |

**Concurrency:** `load`, `recompile`, `compile`, `run`, `pipeline` and `reload` are
exclusive; a second one gets `busy with "<cmd>"`. `hello`, `status`, `traces` and
`readback` answer at any time.

**Readback:** a WebGPU canvas reads back transparent once a frame is presented
(`drawImage`, `createImageBitmap` and `toDataURL` were all measured blank). So `readback`
copies the canvas in the same task as the next frame's `GPUQueue.submit`, which needs the
movie to still be drawing; the runtime redraws every tick.

## Events

| `event` | Fields |
|---|---|
| `ready` | the `hello` body |
| `stage` | `stage`, `state` (`active` / `done` / `error`); `text` (progress, while active); the stage result (when done); `error` (on error) |
| `trace` | `index`, `line`: each host line as it is printed |
| `runError` | `error`: the movie failed after it had started |
| `quit` | the movie ended |

## Example

The S1 SWF, `graphics/avm2_graphics_runtime`, driven from a page on the same origin over
BroadcastChannel to a new tab:

```js
const chan = "walk-" + crypto.randomUUID().slice(0, 8);
const bc = new BroadcastChannel(chan);
window.open(`recompiler/index.html?walkChannel=${chan}`, "_blank");
// … wait for {event: "ready", crossOriginIsolated: true, capabilities: {compile: true, run: true}, …}
bc.postMessage({ proto: "swfrecomp-walk", v: 1, id: 1, cmd: "pipeline", example: "avm2_graphics_runtime" });
```

The events that follow, abridged:

```
{event:"stage", stage:"load", state:"done", ms:15, name:"avm2_graphics_runtime", label:"graphics/avm2_graphics_runtime/test.swf", bytes:1181}
{event:"stage", stage:"recompile", state:"done", ms:278, recompileMs:256, avm2:true, fileCount:14, cBytes:30666}
{event:"stage", stage:"compile", state:"active", text:"Downloading Clang (105.6 MB, …): 50%"}
{event:"stage", stage:"compile", state:"done", ms:20703, clangMs:12276, fileCount:11, guestBytes:103765, avm2:true}
{event:"stage", stage:"run", state:"done", ms:534, status:"Running 200x200, 1 frames @ 30 fps, SWF v38", width:200, height:200}
{event:"trace", index:4, line:"bg=ffffff"}  … "solid=ff0000" "triangles=ffff" "grad_start=ff0000" "grad_end=ff00" "stroke=ff" "grad_mid=7c8300"
```

Then the reply, `{re:1, ok:true, result:{stages:{…}, totalMs:21531}}`.

To check the run, send:

```
{id:2, cmd:"traces"}                        → lines ⊇ the 7 lines of the example's output.txt
{id:3, cmd:"readback", points:[[5,5],[30,30],[130,40],[90,100],[20,140],[180,140]]}
  → rgba ffffffff ff0000ff 00ffffff 0000ffff ff0000ff 00ff00ff
    (background, rectangle, triangles, line, gradient start, gradient end)
```

## Network

`compile` fetches one file, the clang package. It is 110.7 MB, fetched from
`cdn.wasmer.io/webcimages/<sha256>.webc`, and its sha256 is verified before use; the
browser caches it after that. The Wasmer SDK and fflate are vendored (`vendor/`).

**With no network,** everything up to `recompile` works, and `compile` fails at once with
an error naming the URL. `?clangWebc=<url>` fetches the package from somewhere else, such
as a local copy, for a fully offline run; the hash is still checked.
