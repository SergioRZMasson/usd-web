# @openusd-wasm/babylon

Direct, asynchronous USD loading for Babylon.js.

The package sends the source asset to a Web Worker. OpenUSD traverses the complete composed
stage in C++ and returns two transferable buffers:

- a versioned command queue describing Babylon nodes, materials, geometry, instances,
  skeletons, and animations;
- an aligned raw-data buffer containing strings, textures, vertex/index streams, matrices,
  skinning data, and animation samples.

JavaScript performs no per-prim OpenUSD calls and no intermediate GLB or JSON serialization.
The direct Wasm module traverses the stage with Pixar OpenUSD APIs and does not link Adobe
USD-Fileformat-plugins scene-reading or export code.

```ts
import { loadUsdIntoSceneAsync } from "@openusd-wasm/babylon";

const result = await loadUsdIntoSceneAsync(scene, await file.arrayBuffer(), {
    fileName: file.name,
    onProgress: ({ message }) => console.log(message),
});

console.log(result.timings, result.statistics);
```

`addToScene` defaults to `true`. Set it to `false` to receive an off-scene
`AssetContainer`.

Multi-file stages can supply their referenced layers and textures without placing anything
on a server:

```ts
await loadUsdIntoSceneAsync(scene, rootLayer, {
    fileName: "robot/root.usda",
    files: {
        "robot/parts/arm.usda": armLayer,
        "robot/textures/base_color.png": baseColor,
    },
});
```

The worker reports `initializing`, `staging`, `extracting`, and `materializing` progress.
`missingAssets` lists unresolved references. `resolveByFileName` defaults to `true` so
absolute paths authored on another machine can fall back to an unambiguous supplied file
with the same base name.

## Runtime representation

The little-endian protocol is versioned and validates every command payload and raw-data
range before constructing Babylon objects. It currently covers:

- composed transform hierarchies and root axis/unit conversion;
- PBR metallic-roughness materials, packed base-color/opacity and ORM textures, normal and
  emissive textures, opacity/ORM output-channel metadata, UV transforms, alpha modes, and
  double-sided materials;
- optimized indexed geometry, material subsets, vertex colors, and up to eight skinning
  influences;
- shared source meshes with Babylon instances;
- skeletons, node animation, and skeletal animation.

The direct path intentionally does not create a reusable GLB or `.babylon` file. Use
`usd-web-gltf` when the converted result must be cached, downloaded, or consumed by another
renderer.

This initial OpenUSD-only material implementation targets `UsdPreviewSurface`. It reports
unsupported surface shaders, separately authored metallic/roughness maps that cannot be
represented by Babylon's packed metallic texture, conflicting UV sets, and texture formats
that browsers cannot decode directly. Cameras, lights, blend shapes, and analytic gprim
tessellation are not yet part of the command protocol.
