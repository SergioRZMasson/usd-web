# Handoff: USD on the web, for Babylon.js

**Status at handoff:** working end to end. USD loads in the Babylon.js sandbox via drag-and-drop,
including multi-file scenes and `.usdz`. All work is committed and pushed.

This document is the complete context for picking the work up. It covers what we built, how it
works, *why* each decision was made, what we measured, and what is still open. Read
[`USD-TO-GLTF-TRANSLATION.md`](./USD-TO-GLTF-TRANSLATION.md) alongside it for the fidelity
analysis of Adobe's translator (which USD features survive the trip to glTF and which do not).

---

## 1. The goal and the strategy

Display USD content on the web, rendered by Babylon.js, without losing information that matters.

The USD specification is enormous and most of it is irrelevant to the web (Hydra render delegates,
USDZ ARKit specifics, layer composition tooling, Python bindings). Reimplementing a USD parser in
TypeScript would take years and would drift from the reference implementation.

**The strategy we chose:** run the *real* OpenUSD, plus Adobe's `usdGltf` file format plugin, inside
WebAssembly. USD parses the stage exactly as a desktop DCC would, Adobe's translator converts it to
glTF, and Babylon renders the glTF — which it already does extremely well.

glTF is used here as a **transport format**, not as an archival one. The conversion is lossy in
known, documented ways. The point of `USD-TO-GLTF-TRANSLATION.md` is to make that loss explicit so
nobody is surprised later.

**Why Adobe's plugin rather than our own translator:** it is the reference USD↔glTF implementation,
it is Apache-2.0, and it is actively maintained. We compile its sources **unmodified** (see §4.3 for
the single exception). That means our output matches what Adobe's desktop tooling produces, and we
inherit their bug fixes for free.

### What we rejected, and why

| Approach | Why we dropped it |
|---|---|
| Reimplement USD parsing in TypeScript | Years of work; would permanently lag the spec. |
| Port **BabylonPolymorph** (`E:\Github\BabylonPolymorph`, branch `usd-support`) to wasm | We got CoreUtils/Framework/Asset3D/PluginSDK/ImagingComponent compiling under Emscripten, but the library is deeply Windows-coupled (CanvasTex had to be stubbed out). We stopped: it duplicates code that OpenUSD already has, and would have to be maintained forever. The `JS/` bindings folder there is complete but the native module was never finished. **Treat that repo as a dead end unless requirements change.** |
| vcpkg's `usd` port | `vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)` — conflicts with the static `BUILD_SHARED_LIBS=OFF` that wasm requires. |

---

## 2. The four repositories

| Path | Role | State |
|---|---|---|
| `E:\Github\usd-web-gltf` | **The hub.** CMake + wasm build, npm package, demo. Remote is `SergioRZMasson/usd-web`. | Pushed, clean. Live at <https://sergiorzmasson.github.io/usd-web/> |
| `E:\Github\Babylon.js` | The USD loader + sandbox integration. Branch `sergio/usd-support`, remote `sergio` (`SergioRZMasson/Babylon.js`). | Pushed, clean. Latest USD commit `04e3a42124` |
| `E:\Github\USD-Fileformat-plugins` | Adobe's plugins. **Read-only reference** — we compile these sources in place, never edit them. | Untouched, at `6441b97` ("2026.07") |
| `E:\Github\BabylonPolymorph` | Abandoned approach, branch `usd-support`. | Not committed. See table above. |

> **Note:** `E:\Github\Babylon.js` is a shared checkout and may be on a different branch when you
> arrive (it was on `khr-interactivity-importer-v2` at handoff). The USD work is safe on
> `sergio/usd-support` both locally and on the remote. `git checkout sergio/usd-support` to resume.

### Build outputs on this machine

Not in git — these are large intermediate artifacts.

```
E:\emsdk                      Emscripten 6.0.5
E:\wasmdeps\OpenUSD26         OpenUSD v26.08 source
E:\wasmdeps\oneTBB            oneTBB v2022.2.0 source
E:\wasmdeps\tinygltf          tinygltf v2.8.21 source (header-only)
E:\wasmdeps\install-wasm      oneTBB install (multi-threaded)
E:\wasmdeps\install-wasm-st   oneTBB install (single-threaded)  <- the one we ship
E:\wasmdeps\usd26-wasm-st     OpenUSD install, single-threaded  <- the one we ship
E:\wasmdeps\usd26-wasm-os     OpenUSD install built -Os         (size experiment)
E:\wasmdeps\usd26-wasm-oz     OpenUSD install built -Oz         (size experiment)
```

`E:\Github\usd-web-gltf\build-st\bin\` holds the artifacts we ship. `build/`, `build-osfull/`,
`build-ozfull/` are the size experiments from §7.

---

## 3. The key finding: **USD plugins work in WebAssembly**

This was the question that decided the whole architecture, and the answer is **yes**. It is not
obvious, because USD's plugin system normally uses `dlopen` on a shared library named in
`plugInfo.json`, and Emscripten has no `dlopen` in a static build.

Under Emscripten, OpenUSD's `pxr_plugin` macro routes to `pxr_library(TYPE STATIC)`. The plugin
becomes a static archive and `plugInfo.json` carries an **empty `LibraryPath`**, which tells USD
"this plugin is already in the binary, just register it".

Three things must all be true or it silently fails:

1. **`pxr_plugin` builds STATIC under Emscripten.** Upstream OpenUSD 26.08 already does this —
   `cmake/macros/Public.cmake:458`:
   ```cmake
   macro(pxr_plugin NAME)
       if(EMSCRIPTEN)
           # Dynamic linking is not supported yet in the usd build toolchain
           pxr_library(${NAME} TYPE "STATIC" ${ARGN})
       else()
           pxr_library(${NAME} TYPE "PLUGIN" ${ARGN})
       endif()
   endmacro(pxr_plugin)
   ```
2. **`LibraryPath` is `""`** in the plugin's `plugInfo.json`. See
   `resources/usdGltf/resources/plugInfo.json`.
3. **`--whole-archive` on *both* the plugin and `libusd_m.a`.**

Point 3 is the subtle one and cost us the most time:

- The plugin archive needs it because the `TfType` registrations and the `SdfFileFormat` subclass
  are **static initialisers**. Nothing references them symbolically, so a normal link discards those
  objects and USD reports the file format as unknown.
- **`libusd_m.a` needs it too.** It contains `plug/initConfig.cpp`, which is what installs USD's
  default plugin search paths — again via a static initialiser nothing references. Omit it and you
  get `No plugin search paths` and a stage that will not even open.

```cmake
target_link_options(usd-web-gltf PRIVATE
    "SHELL:-Wl,--whole-archive $<TARGET_FILE:usdGltf> -Wl,--no-whole-archive"
    "SHELL:-Wl,--whole-archive ${PXR_CMAKE_DIR}/lib/libusd_m.a -Wl,--no-whole-archive"
)
```

**Consequence:** any other Adobe plugin (FBX, OBJ, PLY, STL, SPZ) can be added the same way. Only
glTF is wired up today.

---

## 4. Build configuration, and the reasons behind it

### 4.1 Pinned versions — do not bump casually

| Dependency | Pin | Why this exact version |
|---|---|---|
| **OpenUSD** | `v26.08` | **Hard minimum.** In 25.11 and earlier, `pxr/CMakeLists.txt` reads `if (NOT EMSCRIPTEN) add_subdirectory(usd) endif()` — so you get only `pxr/base`: no `UsdStage`, nothing that can open a file. 26.08 dropped that guard. We lost time on 25.11 before finding this. |
| **tinygltf** | `v2.8.21` | 2.9.x changed `WriteImageDataFunction` (added an `FsCallbacks` parameter). Adobe's `gltf.cpp` uses the old signature and will not compile against 2.9.x. |
| **oneTBB** | `v2022.2.0` | Required by OpenUSD. Built with `EMSCRIPTEN_WITHOUT_PTHREAD=ON` for the shipped build. |
| **Emscripten** | `6.0.5` | What we used. Nothing depends on this exact version. |
| **Babylon.js** | `9.19.1` | Pinned in the demo to match the Babylon checkout. |

### 4.2 OpenUSD CMake flags

Everything not needed to read a stage and write glTF is off, because each subsystem costs binary
size:

```
-DPXR_BUILD_MONOLITHIC=ON        one archive; what the wasm link expects
-DBUILD_SHARED_LIBS=OFF          wasm cannot link shared libs
-DPXR_BUILD_IMAGING=OFF          Hydra/GL are meaningless here
-DPXR_BUILD_USD_IMAGING=OFF
-DPXR_BUILD_USDVIEW=OFF
-DPXR_ENABLE_PYTHON_SUPPORT=OFF
-DPXR_BUILD_TESTS=OFF
-DPXR_BUILD_EXAMPLES=OFF
-DPXR_BUILD_TUTORIALS=OFF
-DPXR_BUILD_USD_TOOLS=OFF
-DPXR_BUILD_USD_VALIDATION=OFF
-DPXR_ENABLE_GL_SUPPORT=OFF
-DPXR_ENABLE_MATERIALX_SUPPORT=OFF
```

### 4.3 The one file we replaced, and why

`utils/src/images.cpp` in Adobe's tree pulls in **OpenImageIO** and `pxr/imaging/hio` — neither of
which we build for wasm. It is replaced by `src/imagesWasm.cpp`, which implements the same interface
using **stb_image / stb_image_write**.

This swap was only feasible because the dependency is **confined to that single file**: 39
references to OpenImageIO, all in `images.cpp`; the glTF plugin itself has **zero**. Everything else
— the entire USD→glTF translation — is Adobe's code, compiled unmodified.

The other deltas from upstream are build-rule only: Adobe builds `fileformatUtils` and `usdGltf` as
SHARED, and we need STATIC.

### 4.4 Two link-time traps we hit

**Windows command-line limit.** USD's exported CMake targets attach one `--embed-file` link option
*per resource file*. With the full schema set that blows past Windows' 32 KiB command line. We clear
those per-target `INTERFACE_LINK_OPTIONS` and stage every resource into one directory mounted with a
single `--preload-file`. Bonus: resources land in a separate `.data` file instead of inflating the
`.wasm`, and linking is faster.

**Emscripten link flags:**

```
--bind -sMODULARIZE=1 -sEXPORT_NAME=createUsdGltfModule
-sENVIRONMENT=web,worker,node -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB
-sFORCE_FILESYSTEM=1 -sEXPORTED_RUNTIME_METHODS=FS,HEAPU8 -sSTACK_SIZE=1MB
-sEXPORT_ES6=1
```

`-sEXPORT_ES6=1` matters: without it Emscripten emits CommonJS, which Node refuses to load from a
`"type": "module"` package.

### 4.5 Single-threaded, deliberately

We originally built with pthreads, which forces the hosting page to be **cross-origin isolated**
(`Cross-Origin-Opener-Policy: same-origin` + `Cross-Origin-Embedder-Policy: require-corp`) so that
`SharedArrayBuffer` is available. That is a heavy requirement to impose on every consumer — it
breaks third-party embeds and many CDN setups.

We rebuilt the entire stack single-threaded. **It cost 0.13 MB.** That is an excellent trade: the
module now runs anywhere, with no headers at all. This is the shipped default.

A pthreads build still exists for throughput on very large scenes, but nothing uses it today.

### 4.6 Building it

```powershell
cd E:\Github\usd-web-gltf
./scripts/build.ps1 -AdobePluginsDir E:/Github/USD-Fileformat-plugins
```

The script does the whole chain (oneTBB → OpenUSD → tinygltf → this project) and skips steps whose
output already exists. **Cold build is 30–60 minutes, almost all of it OpenUSD.**

Because `emsdk_env.bat` only affects the process it runs in, every build step must run activation
and the command in a *single* `cmd.exe` invocation. The script's `Invoke-Emscripten` helper does
this; keep that pattern if you add steps.

---

## 5. Asset resolution

This was the biggest real-world problem, and it is worth understanding before touching the loader.

Real USD scenes are **many files**: a root layer references sublayers, payloads and textures by
path. Two failure modes showed up immediately with a real customer asset:

1. **Absolute paths from the author's machine.**
   `@/Users/someone/OneDrive - Microsoft/Desktop/TravelDemo.usd@` — unresolvable anywhere else.
2. **Relative references that fail because only one file was supplied.** The reference is correct;
   the file was simply never handed to the converter.

`src/webResolver.cpp` subclasses `ArDefaultResolver` and adds a **fallback that matches by file
name** when normal resolution fails. It:

- **refuses ambiguous matches** — if two folders contain `checker.png` it will not guess;
- **logs every redirect** it makes, so surprises are visible;
- **reports unresolved references** through `missingAssets` on the result.

The fallback is a safety net, not the primary mechanism. The correct fix is to give the converter
the whole folder with its structure intact — which is what the loader now does (§6.3).

---

## 6. The Babylon.js side

Branch `sergio/usd-support`, remote `sergio`. Two commits: `76f8a42ca4` (the loader) and
`04e3a42124` (CDN hosting + drag-and-drop fixes).

### 6.1 Files

```
packages/dev/loaders/src/USD/
├── index.ts                    barrel
├── pure.ts                     side-effect-free entry
├── usdConverter.ts             the wasm wrapper  (modelled on DracoCodec)
├── usdFileLoader.metadata.ts   name + extensions, for lazy registration
├── usdFileLoader.pure.ts       the plugin itself
├── usdFileLoader.ts            registers on import
├── usdFileLoader.types.ts      type-only exports
└── usdLoadingOptions.ts        per-load options
```

Registered in `loaders/src/index.ts` and `loaders/src/dynamic.ts` (lazy — the 12 MB module is only
fetched when a USD file is actually loaded). `eslint.config.mjs` gained `"USD"` in its
**`abbreviations` allowlist** (top of the file, alongside `STL`, `PLY`, `BVH`) — that is how Babylon
handles `STLFileLoader`, `OBJFileLoader` etc. Do *not* add per-file eslint disables for the naming
rule.

### 6.2 How the loader works

`USDConverter` deliberately mirrors `DracoCodec` so it is familiar to Babylon maintainers:
`DefaultConfiguration` with URLs, `Tools.GetBabylonScriptURL` for CDN redirection, lazy first-use
download.

The plugin implements all three entry points — `importMeshAsync`, `loadAsync`,
`loadAssetContainerAsync` — and each one converts to GLB then **delegates to the glTF loader**.
No USD parsing happens in TypeScript.

Two robustness details worth keeping:

- **Conversions are serialised.** The native module holds global state (its virtual filesystem and
  asset index), so overlapping calls corrupt each other.
- **`ModuleAbortedError` + auto-recovery.** OpenUSD treats some malformed input as *fatal*, which
  terminates the wasm runtime permanently. We detect it and build a fresh instance instead of
  leaving the app wedged.

### 6.3 Hosting: the local CDN

`USDConverter.DefaultConfiguration` points at `https://cdn.babylonjs.com/usd/usd-web-gltf.{js,wasm,data}`.
**These are not on the real CDN yet** — that has to be arranged before this ships.

For development the artifacts are committed to
`packages/tools/babylonServer/public/usd/`, alongside draco, basis and twgsl.
`Tools.GetBabylonScriptURL` rewrites CDN URLs through `Tools.ScriptBaseUrl`, so the sandbox picks up
the local copy exactly the way it does for every other wasm dependency.

The Vite dev server never runs the CDN bootstrap (`public/index.js`) that sets `ScriptBaseUrl`, so
`sandbox/vite.config.ts` has a small plugin serving those same babylonServer files under `/usd/`
with the correct content types (`application/wasm` matters — Emscripten streams the instantiation).
Serving the babylonServer copy rather than staging a second one keeps the 12 MB binary in the tree
**once**.

> ⚠️ **The 12.9 MB of binaries are committed for local testing only.** Sergio's instruction was that
> this branch will be **squashed without them** before it reaches Babylon.js. Do not let them reach
> an upstream PR.

### 6.4 Two real bugs we found and fixed in drag-and-drop

Both were in the *sandbox/FilesInput* layer, not in USD.

**(a) The wrong root layer was chosen.** `FilesInput._processFiles` keeps the **last** file whose
extension has a registered loader. Every layer of a USD scene shares the same extensions, so
dropping a folder loaded an arbitrary sublayer — our test folder loaded `sub/robot.usda` instead of
`main.usda`.

`SelectUsdRootLayerAsync` in `renderingZone.tsx` now works it out using two signals:
a layer whose **name appears inside another layer** cannot be the root; among the survivors the
**shallowest path** wins. It reads the dropped files (asset paths survive as plain text in both the
ASCII `.usda` and the binary crate `.usdc` encodings, so a substring search finds references without
parsing either format), which cannot happen inside the synchronous `onProcessFileCallback` — so it
runs in the reload callback, before `filesInput.reload()`.

**(b) Sibling assets never resolved by their authored paths.** `FilesInputStore` keys carry each
file's path inside the dropped folder, but the scene loader hands the plugin only the root layer's
**base name**. Siblings were written into the virtual filesystem under their original folder paths
while the root sat at the top — nothing lined up, so *every* reference fell through to the file-name
fallback (which refuses to guess on duplicate names).

`ReadFilesInputStoreAsync` in `usdFileLoader.pure.ts` now rebases sibling paths onto the root
layer's directory, so `@./textures/checker.png@` resolves as authored.

### 6.5 A deliberate non-change

We did **not** add a missing-assets UI to the sandbox. The sandbox has only a *fatal* `errorZone`,
and a missing asset is non-fatal. The loader emits a clear `Logger.Warn` — consistent with how glTF
reports missing textures — and the folder-access prompt already addresses the common cause.
`onMissingAssets` is available on `USDLoadingOptions` if an application wants to surface it.

---

## 7. What we measured

### Optimisation level — **keep `-O3`**

We investigated shrinking the module. Raw and brotli sizes:

| Build | Raw | Brotli |
|---|---|---|
| `-O3` (shipped) | 12.00 MB | **1.87 MB** |
| `-Os` | 9.29 MB | 1.54 MB |
| `-Oz` | 7.06 MB | 1.34 MB |

Tempting — until we benchmarked a realistic 166k-triangle scene, which **inverted the
recommendation**:

| Build | Conversion time |
|---|---|
| `-O3` | **982 ms** |
| `-Os` | 2,972 ms |
| `-Oz` | 3,359 ms |

`-Oz` is **3.4× slower**. The size saving is fixed and paid once (and cached); the speed penalty is
paid on *every asset* and scales with scene size. **We kept `-O3`.** The benchmark harness is in
`test/bench.mjs` and `test/make-big-usd.mjs` if you want to re-run it.

Also measured:

- **Link-time `-Oz` alone is worthless**: raw 12.00 → 10.79 MB but brotli only 1.87 → 1.86 MB.
- **`-O2` was never tested** and may be the real sweet spot. Cheap experiment if size matters.
- Remaining "free" wins (no speed cost): selective linking ≈ 0.33 MB (measured ceiling —
  `--gc-sections` already strips most), `.data` pruning ≈ 0.15 MB. `usdUtils` is linked but never
  actually used by Adobe's code (comments only, zero undefined symbols).

**Serve with brotli.** 12 MB → 1.87 MB is the difference between usable and not.

### Load performance in Babylon

First load ~385 ms (module fetch + init), then 25–41 ms per conversion for small assets.

---

## 8. What we tested, and how

Everything below was verified **in a real browser** with `playwright-cli`, not just reasoned about.

**Babylon sandbox** (`packages/tools/sandbox`, `npm run serve`):

- Multi-folder USD (root layer + `sub/robot.usda` + `textures/checker.png`): correct root chosen,
  sublayer and texture both resolve with **zero fallback warnings**, texture confirmed loaded
  (64×64, `isReady() === true`).
- The same scene packaged as **`.usdz`** loads identically.
- Three sequential drops leave no residue, 0 console errors.
- All three plugin entry points work.

**usd-web demo** (`demo/`, `npm start` → <http://localhost:8080>):

- 10 sequential loads mixing meshes, materials, textures, lights and cameras.
- A deliberately malformed layer (fatal USD abort) followed by a good one — recovery works.
- Three overlapping drops (race conditions).
- Light/camera/geometry/material/texture counts return exactly to each asset's own totals, no drift.

### Test fixtures

The sandbox fixtures under `packages/tools/sandbox/public/usd-test/` were **deleted after
verification** — they were scratch. If you need them again, the shape was:

```
testscene/main.usda            root: 2 references to sub/robot.usda + a textured ground plane
testscene/sub/robot.usda       referenced sublayer, one cube with a PBR material
testscene/textures/checker.png 64x64 checker
```

Note `main.usda` must reference the texture as `@./textures/checker.png@`. An early version used
`@../textures/...@`, which escapes the scene root — that produced a fallback warning that looked
like a loader bug but was a **bad fixture**. Worth remembering when a resolution warning appears.

The demo's own samples are committed: `demo/assets/{cube,hierarchy,materials}.usda` and
`demo/assets/multifile/`.

---

## 9. Traps that will bite you

**Git silently corrupts Emscripten `.data` files.** The `.data` bundle has no NUL bytes near its
start, so git's heuristic classifies it as *text* and strips CR bytes on commit. We observed an
825,774-byte bundle stored as 821,440 bytes — a file that then fails at runtime with a confusing
error. `usd-web-gltf/.gitattributes` marks `*.wasm`/`*.data` as `binary`; **this is mandatory.**
Babylon.js is safe for a different reason: a global `* -crlf` in its own `.gitattributes`. Its
diffs still *display* `.data` as text (the commit stat shows "21376 +++++" rather than "Bin"), which
is cosmetic — we verified the committed blob hash equals `git hash-object` of the build artifact:

```powershell
git rev-parse "sergio/usd-support:packages/tools/babylonServer/public/usd/usd-web-gltf.data"
git hash-object "E:\Github\usd-web-gltf\build-st\bin\usd-web-gltf.data"
# both cf3fcd28bfed521a57f08f426856c6d4df4c21c0
```

**Always verify binaries after committing them.** If these ever move into Babylon.js proper, add a
`.gitattributes` entry marking them `binary`.

**GitHub Pages only accepts `/` or `/docs`** as a branch source — `/dist` is rejected with a 422.
That is why the demo builds into `docs/`. We tried the Actions-workflow route and abandoned it;
Pages is now pointed at `docs/` directly and is **live**.

**`docs/` is wiped by `demo/npm run build`.** Do not leave anything hand-written there.

**Known-harmless upstream noise** — do not chase these:

- `Invalid mesh topology: offset N into indices for face M...` — fires on well-formed quads.
- `Cannot set value for unknown field 'hide_in_stage_window'` / `'no_delete'` — Omniverse metadata
  that OpenUSD does not know.
- `Found material bindings ... but MaterialBindingAPI is not applied` — a USD authoring lint.

**`displayColor`-only meshes get no glTF material.** Adobe emits materials only for
`material:binding`, so those meshes arrive as `COLOR_0` with no material. Babylon's default material
ignores vertex colours and renders them white and shiny. The sandbox is fine (its IBL path handles
it); the standalone demo synthesises a `PBRMaterial` with `useVertexColors = true`.

**In Babylon dev mode there is no `BABYLON` global.** It only exists in production UMD builds. We
already fixed one pre-existing sandbox bug caused by this (`sandbox.tsx` line ~409). Use direct
module imports.

---

## 10. Open items / suggested next steps

Roughly in priority order.

1. **Publish the artifacts to the real Babylon CDN** (`cdn.babylonjs.com/usd/`). Everything is wired
   for it; the URLs are already correct. Until then the loader only works against a local CDN.
   Ensure **brotli** is enabled for them.
2. **Squash the branch without the 12.9 MB binaries** before opening an upstream PR (§6.3).
3. **Open the Babylon.js PR.** No PR exists yet. The branch is
   `SergioRZMasson/Babylon.js @ sergio/usd-support`.
4. **Add tests.** There are currently no automated tests for the loader on the Babylon side — all
   verification was manual browser testing. `packages/dev/loaders` uses `vitest`.
5. **Try `-O2`** (§7). Possibly a better size/speed point than `-O3`; untested.
6. **Consider more Adobe plugins.** FBX/OBJ/PLY/STL/SPZ can be linked exactly like glTF (§3). Note
   the FBX one carries an SDK dependency.
7. **Decide about lossy features.** `USD-TO-GLTF-TRANSLATION.md` lists what glTF cannot carry (blend
   shapes, curves, NURBS, implicit primitives, point-instancer animation, `purpose`). If any of these
   matter, glTF-as-transport needs supplementing — e.g. a side-channel extension.
8. **Surface `missingAssets` in application UI** where appropriate (§6.5 explains why the sandbox
   itself does not).

---

## 11. Quick reference

```powershell
# Build the wasm (30-60 min cold)
cd E:\Github\usd-web-gltf
./scripts/build.ps1 -AdobePluginsDir E:/Github/USD-Fileformat-plugins

# npm package
cd js;   npm install; npm run build

# Demo -> builds into ../docs, serves on :8080
cd demo; npm start

# Babylon sandbox (dev server picks its own port if 1339 is taken)
cd E:\Github\Babylon.js\packages\tools\sandbox; npm run serve

# Babylon: compile + lint the changed packages
cd E:\Github\Babylon.js\packages\dev\loaders; npm run compile
cd E:\Github\Babylon.js; npx eslint packages/dev/loaders/src/USD
```

If `tsc -b` fails in `packages/dev/core` with missing `ShadersWGSL/*.ts` modules, the generated
shader files are absent — regenerate with `cd packages/dev/core; npm run compile:assets`.

**Consuming the npm package:**

```js
import { UsdConverter } from 'usd-web-gltf';

const converter = await UsdConverter.create();
const { data, missingAssets } = await converter.convert(bytes, {
    fileName: 'scene.usdz',
    additionalFiles: { 'textures/wood.png': textureBytes },   // for multi-file scenes
});
// `data` is GLB bytes -> hand straight to Babylon
```

---

## 12. Further reading

- [`USD-TO-GLTF-TRANSLATION.md`](./USD-TO-GLTF-TRANSLATION.md) — full fidelity analysis with
  `file:line` citations into Adobe's source: what maps, what is dropped, what is degraded. **Read
  this before promising any USD feature to a partner.**
- [`README.md`](./README.md) — build instructions, JS API, supported/unsupported feature tables.
- OpenUSD API: <https://openusd.org/release/api/index.html>
- glTF 2.0 spec: <https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html>
- Adobe plugins: <https://github.com/adobe/USD-Fileformat-plugins>
