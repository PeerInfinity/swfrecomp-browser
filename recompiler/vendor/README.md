# Vendored run-time dependencies of the in-browser recompiler

Served next to the page so it needs no CDN for them. Each one is copied
unmodified from its npm package; the licence text is beside it.

| Directory | Package | Files taken | npm tarball sha256 | Licence |
|---|---|---|---|---|
| `wasmer-sdk/` | `@wasmer/sdk@0.8.0-beta.1` | `dist/index.mjs`, `dist/wasmer_js_bg.wasm`, `LICENSE` | `3955c4c0a5d65fc016aadb9040121e0866cec3843dda1bd272f1ff5e18127907` | MIT, Wasmer, Inc. |
| `fflate/` | `fflate@0.8.2` | `esm/browser.js`, `LICENSE` | `61fd5061e2fc8e5e3e3129f7f2fec7bd78a313e1bf4becbf1cc1cc9998d141dc` | MIT, Arjun Barrett |

To check a copy: `npm pack <package>@<version>`, compare the tarball's sha256
with the table, unpack it, and compare the files.

`index.mjs` loads `wasmer_js_bg.wasm` from its own directory
(`import.meta.url`), and the SDK's workers re-import `index.mjs` from the same
URL. So keep the two files together.

**Not vendored: the clang compiler package** (`clang/clang` 0.160000.1 from the
wasmer registry, 110,713,421 bytes). That is over GitHub's 100 MB file limit,
and its registry entry declares no licence. `pipeline_graphics.js` fetches it
from its content-addressed URL,
`https://cdn.wasmer.io/webcimages/<sha256>.webc`, and refuses it unless its
sha256 is `CLANG_WEBC_SHA256`. Recompiling to C works without it. The compile
step needs the network the first time; after that the browser cache serves it.
For a fully offline run, serve a copy yourself and pass `?clangWebc=<url>`. The
hash is still checked.
