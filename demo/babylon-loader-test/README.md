# Babylon.js USD loader — integration test

Verifies the `USDFileLoader` added to Babylon.js at
`packages/dev/loaders/src/USD`, driving it through Babylon's `SceneLoader` exactly as an
application would.

Nothing here calls the converter directly. A `.usda` URL is handed to `AppendSceneAsync`,
which routes on extension to the USD plugin, which converts to GLB in WebAssembly and
delegates to the glTF loader. If the scene renders, plugin registration and delegation
both work.

## Run

```powershell
# 1. Build Babylon's core and loaders packages
cd E:\Github\Babylon.js\packages\dev\loaders
npm run compile

# 2. Build the converter's npm package (supplies the wasm artifacts)
cd E:\Github\usd-web\js
npm run build

# 3. Bundle and serve this test
cd E:\Github\usd-web\demo\babylon-loader-test
npx webpack --config ./webpack.config.mjs
cd ..;  $env:PORT="8082"; $env:ROOT="$PWD\babylon-loader-test\public"; node scripts/serve.mjs
```

Then open <http://localhost:8082>.

Set `BABYLON_DEV_PACKAGES` if your Babylon.js checkout is elsewhere:

```powershell
$env:BABYLON_DEV_PACKAGES = "D:/src/Babylon.js/packages/dev"
```

## What it checks

| Button | Plugin entry point |
|---|---|
| Cube / Hierarchy / Materials | `loadAsync` via `AppendSceneAsync` |
| Asset container | `loadAssetContainerAsync` via `LoadAssetContainerAsync` |

Observed results:

```
OpenUSD 0.26.8 via Babylon USD loader
materials.usda   6 meshes, 44 vertices, 6 materials   385 ms (first load, includes wasm init)
cube.usda        2 meshes, 40 vertices, 1 material     25 ms
hierarchy.usda   8 meshes, 60 vertices, 1 material     40 ms
materials.usda   7 meshes, 6 materials (asset container)
```

The first load pays module instantiation; later ones do not, because `USDConverter.Default`
is shared.

## Notes

- The shipped WebAssembly module is single-threaded, so no COOP/COEP headers are needed.
- `core/Loading/loadingScreen` must be imported when driving `SceneLoader` from a URL.
  That is a general Babylon requirement, not specific to this loader.
- Metallic materials look dark here because the test scene has only a hemispheric light
  and no environment texture; PBR metals need IBL to show anything.
