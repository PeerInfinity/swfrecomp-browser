# swfrecomp-browser

The **in-browser SWF recompiler** of [SWFRecomp-CC](https://github.com/PeerInfinity/SWFRecomp-CC),
as a ready-to-serve page: drop a `.swf`, it is recompiled to C in the browser, the C is
compiled to WebAssembly by clang running in the browser, and the movie runs on a WebGPU
runtime. Nothing to install.

Open **`recompiler/index.html`** from any static web server (GitHub Pages, `python3 -m
http.server`). `file://` does not work.

This repository holds **one release** of the page, from the SWFRecomp-CC commits that
`release.json` names (see "Two commits" below). It carries no source code of its own; the code, its history and
its issues live in SWFRecomp-CC. [Archipelago-CC](https://github.com/PeerInfinity/Archipelago-CC)
consumes this repository as a git submodule.

Other pages can drive it by messages (load, recompile, compile, run, read back): see
[Walk API](#walk-api).

## Requirements

- **Chrome or Chromium 137 or newer.** The runtime suspends the WebAssembly stack mid-frame
  with JSPI (`WebAssembly.Suspending`), which only Chromium ships. Firefox and Safari are not
  supported.
- **WebGPU.** The movie renders through it.
- **Cross-origin isolation** (`SharedArrayBuffer`): the in-browser clang runs in workers over
  shared memory, and the module it produces imports a shared memory.
  `recompiler/coi-serviceworker.js` provides it on hosts that cannot set COOP/COEP headers:
  the first visit registers a service worker and reloads once. Its scope is `recompiler/`, so
  it touches nothing outside that directory.
  - **In an iframe**, the service worker also sends `Document-Isolation-Policy:
    isolate-and-credentialless`, which isolates the frame inside a page that is not
    isolated. Chrome honours it (measured on 153); Playwright's headless *shell* does not.
    Give the `<iframe>` `allow="cross-origin-isolated"`. Where the frame stays
    un-isolated, the page still recompiles to C, says so, and links to itself in a new tab
    for the compile step.
- **Network at run time: one file, for the compile step only.** Recompiling to C, and the
  zip downloads, are fully local.

  | What | From | Size |
  |---|---|---|
  | `clang/clang` 0.160000.1 (the wasmer registry package) | `cdn.wasmer.io/webcimages/c127b7bf….webc`, a URL that is its own sha256; the page verifies the hash before use | 110.7 MB, cached by the browser |

  With no network, the compile step fails at once with an error naming that URL.
  `recompiler/index.html?clangWebc=<url>` fetches the package from elsewhere, such as a local
  copy for a fully offline run; the hash is still checked. The Wasmer SDK (`@wasmer/sdk`
  0.8.0-beta.1) and fflate 0.8.2 are vendored in `recompiler/vendor/`.

## What is in it

| Path | What |
|---|---|
| `recompiler/index.html`, `pipeline.js`, `pipeline_graphics.js`, `guest_loader.js`, `runner_template.html`, `coi-serviceworker.js`, `walk_api.js`, `WALK_API.md` | the page (tracked in SWFRecomp-CC `docs/recompiler/`) |
| `recompiler/vendor/` | the vendored Wasmer SDK and fflate, unmodified from npm (`vendor/README.md` gives the tarball hashes) |
| `recompiler/SWFRecomp.{js,wasm}` | the recompiler, built with Emscripten |
| `recompiler/host/` | the prebuilt WebGPU runtime hosts (`graphics_host` for AVM1, `graphics_host_avm2` for AS3) and the guest support sources the in-browser compile links against |
| `recompiler/bundle/` | the runtime source snapshot the "Download build bundle" zip carries (`manifest.json` lists it) |
| `recompiler/build_info.json` | the commit and build time the page footer shows |
| `catalog.json`, `examples/` | one example SWF for the page's example search, with its ActionScript source and expected trace (see below) |
| `release.json` | the release manifest: the build and page commits, the build-input check, the toolchain, the run-time fetch, the built files' sha256, and every file with its size, md5 and sha256 |
| `LICENSE`, `licenses/` | see Licence |

### The example

`examples/graphics/avm2_graphics_runtime/` is a test from SWFRecomp-CC's own regression
suite (`ruffle-tests/tests/swfs/regression/`, which holds hand-written tests owned by that
project, not Ruffle's): a 1.2 KB AS3 movie that draws a rectangle, a triangle pair, a
gradient and a line with `flash.display.Graphics`, then traces seven pixel colours.
`output.txt` is the expected trace, `Test.as` the source and `build_swf.sh` the `mxmlc`
recipe that produced `test.swf`. Type its name into the page's example search to run it.

## Walk API

`recompiler/walk_api.js` answers a small message protocol, `swfrecomp-walk` v1, over
BroadcastChannel (default name `swfrecomp-walk`, or `?walkChannel=<name>`) and over
`postMessage` from a parent frame or the opener. It accepts the same origin only. The
full reference is [`recompiler/WALK_API.md`](recompiler/WALK_API.md).

| Kind | Names |
|---|---|
| Commands | `hello`, `status`, `load`, `recompile`, `compile`, `run`, `pipeline`, `traces`, `readback`, `reload` |
| Events | `ready`, `stage`, `trace`, `runError`, `quit` |

```js
bc.postMessage({ proto: "swfrecomp-walk", v: 1, id: 1, cmd: "pipeline", example: "avm2_graphics_runtime" });
// → stage events …, trace lines "bg=ffffff" … "grad_mid=7c8300", then
//   { proto, v, re: 1, ok: true, result: { stages: { load, recompile, compile, run }, totalMs } }
```

Messages are needed because no isolated arrangement leaves the page DOM-scriptable from a
normal app:
- a new tab's opener handle is severed by COOP;
- a Document-Isolation-Policy frame is a separate agent cluster.

Both still take messages.

## Two commits, and rebuilding a release

`release.json` names two SWFRecomp-CC commits:

- **`source.commit`** built every **built** file: `SWFRecomp.{js,wasm}`, `host/`, `bundle/`
  and `build_info.json`. Their sha256 hashes are in `build.files`. The build inputs are
  `SWFRecomp/` and `SWFModernRuntime/`.
- **`source.page.commit`** is where the page files come from (`docs/recompiler/`).

A **page-only release** keeps the built files byte for byte (sha256-checked against the
previous release) and keeps `source.commit`. `source.buildInputsUnchanged` records the
measured `git diff --name-only` of the build inputs between the two commits. It may name
only files that are not build inputs, each listed literally with the reason.
[Archipelago-CC](https://github.com/PeerInfinity/Archipelago-CC) pins `source.commit`
against the recompiler of the Seedling wasm build it ships, so a release that needs new
binaries is a rebuild, and a rebuild moves that pin.

Both kinds are written by SWFRecomp-CC's `tools/browser-release/make_release.py`, which
refuses a page-only release whose built files or build inputs moved. The recipe for the
built files is in `release.json` (`source.recipe`). In short:
1. Export the commit's `SWFRecomp`, `SWFModernRuntime` and `docs/recompiler` trees.
2. Run `SWFRecomp/scripts/deploy_wasm_demo.sh --build` with emsdk 5.0.0.
3. Pass the result as `--rebuild --built-dir`.

## History

**Only the latest release is kept in the tree.** Each release is one commit that replaces the
previous release's files. The history grows by roughly the compressed size of the changed
`.wasm` files per release. When that becomes uncomfortable, the remedy is the one
[seedling-wasm](https://github.com/PeerInfinity/seedling-wasm) used: commit the current tree
as an orphan commit and force-push. That rewrite is the repository owner's decision, made
deliberately and never as part of a routine release.

## Licence

**MIT.** `LICENSE` is SWFRecomp-CC's root licence as of commit
[`7819368b1`](https://github.com/PeerInfinity/SWFRecomp-CC/commit/7819368b16aacddc4895aa8dcfa2018439f309ef):
SWFRecomp and SWFModernRuntime (Copyright (c) 2024 LittleCube) and the SWFRecomp-CC fork's
additions (Copyright (c) 2025-2026 PeerInfinity). Between the build commit `4c450f076` and it,
only docs, test results and licence files changed: the `SWFRecomp/` code and the
`SWFModernRuntime/` and `docs/recompiler/` trees are identical. Its `THIRD_PARTY_NOTICES.md` is the
source of the table below.

Third-party pieces compiled into the `.wasm` files or shipped as source in
`recompiler/bundle/` and `recompiler/host/`, each with its licence text in `licenses/`:

| Piece | Where | Licence |
|---|---|---|
| zlib | `SWFRecomp.wasm`; the AVM2 host (Emscripten port) | zlib |
| toml++ | `SWFRecomp.wasm` | MIT |
| earcut.hpp | `SWFRecomp.wasm` | ISC |
| stb_image | `SWFRecomp.wasm`, hosts, `bundle/` | MIT or public domain |
| Noto Sans (`NotoSans.ttf`, embedded device-font fallback) | `SWFRecomp.wasm` | SIL OFL 1.1 |
| libtess2 | hosts, `bundle/` | SGI Free Software License B 2.0 |
| c-hashmap | hosts, `bundle/` | BSD-3-Clause |
| o1heap | hosts, `bundle/` | MIT |
| QuickJS libregexp/libunicode | AVM2 host, `bundle/` | MIT |
| LZMA SDK 18.05 (subset) | AVM2 host, `bundle/` | public domain |
| Emscripten runtime and system libraries (musl, libc++, compiler-rt, dlmalloc) | `SWFRecomp.{js,wasm}`, hosts | MIT / University of Illinois NCSA (Emscripten); the system libraries under their own MIT / Apache-2.0-with-LLVM-exception terms |
| coi-serviceworker 0.1.7 | `recompiler/coi-serviceworker.js` (plus a `Document-Isolation-Policy` line) | MIT |
| `@wasmer/sdk` 0.8.0-beta.1 | `recompiler/vendor/wasmer-sdk/` | MIT (Wasmer, Inc.) |
| fflate 0.8.2 | `recompiler/vendor/fflate/` | MIT (Arjun Barrett) |

Fetched at run time, not redistributed here: the wasmer registry package `clang/clang`
(LLVM/clang and lld, Apache-2.0 WITH LLVM-exception, plus a WASIX libc sysroot; the registry
entry declares no licence).
