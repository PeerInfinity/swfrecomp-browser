# swfrecomp-browser

The **in-browser SWF recompiler** of [SWFRecomp-CC](https://github.com/PeerInfinity/SWFRecomp-CC),
as a ready-to-serve page: drop a `.swf`, it is recompiled to C in the browser, the C is
compiled to WebAssembly by clang running in the browser, and the movie runs on a WebGPU
runtime. Nothing to install.

Open **`recompiler/index.html`** from any static web server (GitHub Pages, `python3 -m
http.server`). `file://` does not work.

This repository holds **one release**: the page built from the single SWFRecomp-CC commit
that `release.json` names. It carries no source code of its own; the code, its history and
its issues live in SWFRecomp-CC. [Archipelago-CC](https://github.com/PeerInfinity/Archipelago-CC)
consumes this repository as a git submodule.

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
  - **In an iframe, isolation needs the embedding page isolated too**, which a normal page is
    not. Inside an iframe the page still recompiles to C, but the in-browser compile fails.
    Open the page top-level or in its own tab.
- **Network at run time**, for the compile step only (recompiling to C is fully local):

  | What | From | Size |
  |---|---|---|
  | `@wasmer/sdk` 0.8.0-beta.1 | unpkg.com | 5.7 MB wasm + 50 KB js |
  | `clang/clang` 0.160000.1 | registry.wasmer.io (lookup), cdn.wasmer.io (the package) | 110.7 MB, cached by the browser |
  | `fflate` 0.8.2 (the zip downloads) | unpkg.com | small |

## What is in it

| Path | What |
|---|---|
| `recompiler/index.html`, `pipeline.js`, `pipeline_graphics.js`, `guest_loader.js`, `runner_template.html`, `coi-serviceworker.js` | the page (tracked in SWFRecomp-CC `docs/recompiler/`) |
| `recompiler/SWFRecomp.{js,wasm}` | the recompiler, built with Emscripten |
| `recompiler/host/` | the prebuilt WebGPU runtime hosts (`graphics_host` for AVM1, `graphics_host_avm2` for AS3) and the guest support sources the in-browser compile links against |
| `recompiler/bundle/` | the runtime source snapshot the "Download build bundle" zip carries (`manifest.json` lists it) |
| `recompiler/build_info.json` | the commit and build time the page footer shows |
| `catalog.json`, `examples/` | one example SWF for the page's example search, with its ActionScript source and expected trace (see below) |
| `release.json` | the release manifest: source commit and trees, toolchain, run-time fetches, and every file with its size and md5 |
| `LICENSE`, `licenses/` | see Licence |

### The example

`examples/graphics/avm2_graphics_runtime/` is a test from SWFRecomp-CC's own regression
suite (`ruffle-tests/tests/swfs/regression/`, which holds hand-written tests owned by that
project, not Ruffle's): a 1.2 KB AS3 movie that draws a rectangle, a triangle pair, a
gradient and a line with `flash.display.Graphics`, then traces seven pixel colours.
`output.txt` is the expected trace, `Test.as` the source and `build_swf.sh` the `mxmlc`
recipe that produced `test.swf`. Type its name into the page's example search to run it.

## Rebuilding a release

Recorded in `release.json` (`source.recipe`). In short: export the commit's `SWFRecomp`,
`SWFModernRuntime` and `docs/recompiler` trees, run
`SWFRecomp/scripts/deploy_wasm_demo.sh --build` with emsdk 5.0.0, and copy
`docs/recompiler/` to `recompiler/`. Then regenerate `release.json`.

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
| coi-serviceworker 0.1.7 | `recompiler/coi-serviceworker.js` | MIT |

Fetched at run time, not redistributed here: `@wasmer/sdk` (MIT), `fflate` (MIT), and the
wasmer registry package `clang/clang` (LLVM/clang and lld, Apache-2.0 WITH LLVM-exception,
plus a WASIX libc sysroot; the registry entry declares no licence).
