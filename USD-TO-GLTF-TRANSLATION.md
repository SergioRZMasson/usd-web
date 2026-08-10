# USD → glTF Translation Analysis
### Evaluating Adobe's `usdgltf` plugin as a fidelity reference for a Babylon.js USD importer

**Repo analysed:** `adobe/USD-Fileformat-plugins` @ `6441b97` (2026.07)
**Scope:** the *export* direction — USD stage → glTF 2.0 — because that is the direction that reveals what "web-friendly" costs you.
**Method:** full read of `utils/src/layerRead.cpp` (1621 lines), `utils/include/fileformatutils/usdData.h` (654 lines), `gltf/src/gltfExport.cpp` (~3145 lines), `utils/src/layerReadMaterial.cpp`, `utils/src/materials.cpp`, `utils/src/geometry.cpp`, `utils/src/images.cpp`. Every claim below is cited to `file:line`.

> ⚠️ **Terminology trap.** In this codebase "import"/"export" are relative to the **native format (glTF)**, not to USD. So `layerRead.cpp` — reading *from* USD — is the **USD→glTF export** path. Confirmed by `usdData.h:264` (`inverseBindTransforms; // used for export`) being populated in `layerRead.cpp:757`.

---

## 1. Executive summary

The plugin does **not** translate USD→glTF directly. It funnels everything through a narrow, glTF-shaped intermediate C++ struct called `UsdData` (`utils/include/fileformatutils/usdData.h`). **That struct is the hard ceiling on fidelity** — anything USD can express that has no field in `UsdData` is gone before glTF is even considered.

Three sequential lossy stages:

```
USD stage ──(A) layerRead.cpp──> UsdData ──(B) gltfExport.cpp──> tinygltf ──(C) gltf.cpp──> .gltf/.glb
   │                                │                                │
   │  drops: curves, NURBS,         │  drops: gsplats, dome lights,  │  drops: nothing
   │  implicit gprims, blendshapes, │  DoF, extra color sets,        │  (plumbing only)
   │  creases, purpose, kind,       │  customData, prim paths,       │
   │  variants, custom primvars,    │  static TRS, STEP/CUBICSPLINE  │
   │  vertex animation, orientation │                                │
```

**Headline verdict for the Babylon.js team:** this pipeline is an excellent, battle-tested reference for the *rich-material* problem (its OpenPBR→`KHR_materials_*` mapping is the most complete in the open-source world), but it is a **poor fidelity ceiling for geometry and scene structure**. If Babylon.js targets "display USD content without losing important information", you will need to read several categories of USD data **directly from the stage**, because they never reach glTF here.

---

## 2. The intermediate representation — your fidelity ceiling

`UsdData` (`usdData.h:563-607`) holds exactly:

| Container | Struct | Notable fields |
|---|---|---|
| `nodes[]` | `Node` (`usdData.h:69-100`) | name, displayName, markedInvisible, transform (4×4), worldTransform, T/R/S, animations[], parent, camera, ngp, light, nurbs[], staticMeshes[], skinnedMeshes[], curves[], children[], path, isJoint, customProperties |
| `meshes[]` | `Mesh` (`145-181`) | faces, indices, points, pointWidths, normals, tangents, bitangents, uvs, extraUVSets[], colors[], opacities[], joints, weights, material, subsets[], doubleSided, instanceable, asPoints, asGsplats, influenceCount, geomBindTransform, subdivisionScheme |
| `cameras[]` | `Camera` (`104-122`) | projection, f, h/vAperture, nearZ, farZ, fStop, focusDistance, fov, aspectRatio |
| `lights[]` | `Light` (`325-347`) | type (Disk/Rectangle/Sphere/Environment/Sun), color, length, intensity, radius, coneAngle, coneFalloff, angle, texture |
| `skeletons[]` | `Skeleton` (`255-276`) | joints, jointParents, restTransforms, bind/inverseBind, meshSkinningTargets, skeletonAnimations[] |
| `materials[]` / `openPbrMaterials[]` | `Material` (`437-471`) / `OpenPbrMaterial` (`484-555`) | ~30 / ~45 `Input` slots |
| `curves[]`, `nurbs[]`, `ngps[]`, `images[]` | | *(curves/nurbs are write-only — never populated from USD)* |
| stage-level | | `upAxis`, `metersPerUnit`, `doc`, `metadata`, `timeCodesPerSecond`, `animationTracks[]` |

**There is no field for:** blend shapes, creases/corners/holes, `purpose`, `kind`, `assetInfo`, variant sets, arbitrary primvars, velocities, point ids, orientation (winding), volumes (other than Adobe NGP), physics, or time-sampled points/topology.

---

## 3. What *does* translate — the mapping tables

### 3.1 Stage / scene metadata

| USD | glTF | Citation |
|---|---|---|
| `upAxis == "Z"` | synthetic `"correctionNode"` with rotation `(-0.7071,0,0,0.7071)` = −90° about X; all roots reparented under it | `gltfExport.cpp:113-141`, `3110-3130` |
| `upAxis == "Y"` | no node (glTF is implicitly Y-up) | `gltfExport.cpp:117` |
| `metersPerUnit` | uniform `scale` on the same correction node — **only if `!= 1 && > 0`** | `gltfExport.cpp:126-135` |
| root layer `customLayerData` | `asset.extras`, key-by-key; **only bool/int/float/string** survive, others logged "unsupported type not exported" | read `layerRead.cpp:1523`; write `gltfExport.cpp:77-110` |
| `timeCodesPerSecond` | used to convert skeleton animation times to seconds | `layerRead.cpp:1524`, `gltfExport.cpp:791-792` |
| `asset.generator/version/copyright` | **never authored** by the plugin (tinygltf defaults) | grep: 0 hits in `gltf/src` |

### 3.2 Node hierarchy & transforms

| USD | glTF | Citation |
|---|---|---|
| `UsdGeomXform` / `Scope` / unknown Xformable | `node` | `layerRead.cpp:1244-1266` |
| composed local xformOps | flattened to a single 4×4 `node.transform` at `EarliestTime()` | `layerRead.cpp:124-126` |
| static node | emitted as raw **`matrix`** (never TRS) | `gltfExport.cpp:455-456` |
| animated node | `GfMatrix4d::Factor` → separate `translation`/`rotation`/`scale` (glTF forbids `matrix` + channels) | `gltfExport.cpp:444-480` |
| prim name / display name | `node.name` (displayName wins if present) | `gltfExport.cpp:439`; `usdData.h:681-687` |
| **`SdfPath`** | **debug log only — not persisted** | `gltfExport.cpp:440-443` |

### 3.3 Meshes

| USD | glTF | Citation |
|---|---|---|
| `UsdGeomMesh` faceVertexCounts/Indices/points | fan-triangulated → `TRIANGLES` (hardcoded, the only mode) | `layerRead.cpp:424-426`, `geometry.cpp:509-585`, `gltfExport.cpp:2674` |
| indices | always `UNSIGNED_INT` (never downgraded to ushort) | `gltfExport.cpp:2653-2660` |
| POSITION/NORMAL/TANGENT/TEXCOORD/COLOR | always `FLOAT`, `normalized=false` hardcoded | `gltf.cpp:441` |
| `normals` primvar or `GetNormalsAttr` | `NORMAL`; **synthesised smooth normals if absent** | `layerRead.cpp:429-434`; `geometry.cpp:729-834`, `855-857` |
| `tangents`/`bitangents`/`binormals` primvars | `TANGENT` (VEC4) | `layerRead.cpp:436-475` |
| any `TexCoord2fArray`/`Float2Array` primvar | `TEXCOORD_n`; `"st"` forced to index 0; **V flipped (`v = 1−v`)**; unlimited sets | `layerRead.cpp:353-393`; `gltfExport.cpp:2814-2866` |
| `displayColor` + `displayOpacity` | merged into a **single `COLOR_0`** (VEC3 or VEC4) | `gltfExport.cpp:2867-2985` |
| `UsdGeomSubset` (face subsets) | one `primitive` per subset, sharing vertex accessors | `layerRead.cpp:543-556`; `gltfExport.cpp:3045-3062` |
| bound material (`ComputeBoundMaterial`) | `primitive.material` | `layerRead.cpp:527-529` |
| `doubleSided` | → material `doubleSided` | `usdData.h:169` |
| `geomBindTransform` | **baked into points/normals** | `gltfExport.cpp:2732` |
| extents/bounds | recomputed as accessor `min`/`max` (also for indices/normals/UVs/colors) | `gltf.cpp:355-373`, `447-471` |
| `UsdGeomPoints` | points + widths read into `UsdData`… but **TRIANGLES-only export means point clouds cannot be emitted** | `layerRead.cpp:479-489`; `gltfExport.cpp:2674` |

### 3.4 Skinning & skeletons

| USD | glTF | Citation |
|---|---|---|
| `UsdSkelRoot` + `UsdSkelBindingAPI` (via `UsdSkelCache`) | `skin` + joint node hierarchy | `layerRead.cpp:726-800`; `gltfExport.cpp:651-893` |
| `bindTransforms.GetInverse()` | `inverseBindMatrices` accessor | `layerRead.cpp:748-761`; `gltfExport.cpp:684-694` |
| `restTransforms` | joint node TRS (always decomposed, never matrix) | `gltfExport.cpp:704-705` |
| `ComputeJointInfluences`, N influences/vertex | **not clamped to 4** — padded to a multiple of 4 and split into `JOINTS_0/1/…`, `WEIGHTS_0/1/…` (`USHORT`/`FLOAT`) | `layerRead.cpp:641-664`; `gltfExport.cpp:2944-3041` |
| duplicate joints in a 4-block | merged, weights summed | `gltfExport.cpp:2953-2977` |
| `UsdSkelAnimation` joint transforms | per-sample `UsdSkelDecomposeTransform` → T/R/S channels | `layerRead.cpp:802-864` |

✅ *This is the strongest part of the pipeline.* Note `maxMeshInfluenceCount = -1` (unlimited) is set specifically for glTF (`fileFormat.cpp:265`) — the shared default is 4 (`layerRead.h:37`).

### 3.5 Animation

| USD | glTF | Citation |
|---|---|---|
| xformOp time samples | `translation`/`rotation`/`scale` channels | `gltfExport.cpp:581,610,639` |
| skeleton joint animation | same three channels per joint | `gltfExport.cpp:830-834` |
| interpolation | **always `"LINEAR"`** (6 hardcoded sites) | `gltfExport.cpp:575,604,633,821,824,827` |
| multiple animation tracks | fully supported, one `Animation` per track | `gltfExport.cpp:66-76` |

### 3.6 Cameras

| USD | glTF | Citation |
|---|---|---|
| `UsdGeomCamera` → `GfCamera` @ **time 0** | `perspective`/`orthographic` | `layerRead.cpp:1021-1060` |
| focal length + aperture | `yfov` via `GfCamera::GetFieldOfView(FOVVertical)` — math delegated to USD, not reimplemented | `gltfExport.cpp:174-178` |
| apertures (ortho) | `xmag`/`ymag = aperture * APERTURE_UNIT` | `gltfExport.cpp:179-185` |
| clippingRange | `znear`/`zfar` | `gltfExport.cpp:174-178` |

### 3.7 Lights (`KHR_lights_punctual`)

| USD light | glTF | Citation |
|---|---|---|
| `UsdLuxDiskLight` (+`ShapingAPI`) | `"spot"` (inner/outer cone from coneAngle/coneFalloff) | `gltfExport.cpp:226-271` |
| `UsdLuxDistantLight` | `"directional"` | ″ |
| `UsdLuxRectLight` | `"point"` — **shape lost** | ″ (`default:` case) |
| `UsdLuxSphereLight` | `"point"` — **shape lost** | ″ |
| `UsdLuxDomeLight` | ❌ **skipped**, `TF_WARN "not supported by glTF"`; a TODO cites `EXT_lights_image_based`, never emitted | `gltfExport.cpp:195-201` |
| intensity | empirical fudge factors: point `×0.225`, directional `×0.0000625`, spot `×1.0`, plus **surface-area multiplication** (πr² or l×w) since "glTF doesn't use lights that emit based on surface area" | `gltf.h:19-22`; `gltfExport.cpp:212-267` |

### 3.8 Materials — the strong suit

**Precedence on read** (`layerReadMaterial.cpp:773-808`) — exactly one wins, never merged:
1. **OpenPBR** (MaterialX graph ending in `ND_open_pbr_surface_surfaceshader`)
2. **ASM** (`adobeStandardMaterial` / `AdobeStandardMaterial_4_0`)
3. **UsdPreviewSurface** ("the universal fallback", comment at `:784`)

Core mapping:

| USD input | glTF | Citation / formula |
|---|---|---|
| `diffuseColor` / `base_color` | `baseColorFactor` / `baseColorTexture[rgb]` | `gltfExport.cpp:2074-2076`, `2265-2291` |
| `opacity` / `geometry_opacity` | `baseColor` **alpha**; `alphaMode="BLEND"` | `2221-2226`, `2265-2276` |
| `opacityThreshold` | `alphaMode="MASK"`, `alphaCutoff` (hardcoded `0.5` if textured — `TODO`) | `2519-2525` |
| `metallic` | `metallicFactor` / ORM **B** — default forced to **0.0** (USD default ≠ glTF's 1.0) | `2495-2503` |
| `roughness` | `roughnessFactor` / ORM **G** — default forced to **0.5** | `2505-2516` |
| `occlusion` | `occlusionTexture` / ORM **R**, `strength = scale[0]` | `2488-2491` |
| `emissiveColor` | `emissiveFactor` / `emissiveTexture` (+ `KHR_materials_emissive_strength` when >1) | `2081`, `2280-2284`, `2528` |
| `normal` | `normalTexture` + `.scale`; MaterialX normal maps decoded with OpenGL convention `scale=(2,2,2,1)`, `bias=(-1,-1,-1,0)` | `2286`, `2309`; `usdData.h` constants |
| `ior` | `KHR_materials_ior` (default 1.5, emitted only if different) | `2530`/`2544` |
| `clearcoat*` / `coat_*` | `KHR_materials_clearcoat` (+ `KHR_materials_coat` for advanced, + `ADOBE_materials_clearcoat_specular/_tint`) | `2565-2575` |
| anisotropy | `KHR_materials_anisotropy` — lossy reparameterisation, see §5 | `2320-2333` |
| transmission / volume / dispersion / subsurface | `KHR_materials_transmission`, `_volume`, `_volume_scatter`, `_diffuse_transmission`, `_dispersion` | `2535-2541` |
| sheen (ASM) vs fuzz (OpenPBR) | `KHR_materials_sheen` **legacy-only** (`2545`) / `KHR_materials_fuzz` **OpenPBR-only** (`2532`) — mutually exclusive by design | ″ |

**Textures:** `UsdPrimvarReader_float2` varname `st`/`st0`/`st1`… → `TEXCOORD_n` (`layerWriteShared.cpp:57-63`). `UsdTransform2d` → `KHR_texture_transform` (`gltfExport.cpp:1024-1063`). USD `black`/`useMetadata` wrap modes have no glTF equivalent → fall back to `REPEAT` with `TF_WARN` (`gltfExport.cpp:903-905`). Non-`{png,jpg,bmp,webp}` images are converted to PNG (`2596-2601`); WebP triggers `EXT_texture_webp`.

**Full extension list the exporter can write:** `KHR_lights_punctual`, `KHR_texture_transform`, `KHR_materials_{unlit, clearcoat, coat, emissive_strength, ior, sheen, fuzz, diffuse_roughness, iridescence, diffuse_transmission, dispersion, specular, transmission, volume, volume_scatter, anisotropy}`, `EXT_texture_webp`, `EXT_materials_specular_edge_color`, `ADOBE_materials_clearcoat_specular`, `ADOBE_materials_clearcoat_tint`, `ADOBE_nerf_asset`.

### 3.9 Instancing

Native USD instancing is resolved with `UsdTraverseInstanceProxies()` and prototype meshes are deduplicated by `GetPrimInPrototype().GetPath()` (`layerRead.cpp:670-696`). `UsdGeomPointInstancer` is **flattened**: one `Node` per instance with a baked transform, all pointing at the same mesh index (`layerRead.cpp:877-936`). On the glTF side this becomes ordinary node→mesh sharing (`gltfExport.cpp:519-534`) — **`EXT_mesh_gpu_instancing` is never emitted.**

---

## 4. What is NOT supported — the loss list

### 4.1 Never read out of USD at all (stage → `UsdData` loss)

| USD feature | Evidence |
|---|---|
| **`UsdGeomBasisCurves` / `UsdGeomNurbsCurves`** | `usd.curves`/`addCurve` have 0 hits in `layerRead.cpp`; `Curve` is write-path only (`layerWriteSdfData.cpp:946`) |
| **`UsdGeomNurbsPatch`** | `NurbData`/`addNurb` 0 hits in `layerRead.cpp` |
| **Implicit gprims — `Cube`, `Sphere`, `Cylinder`, `Cone`, `Capsule`** | No `IsA<>` case in the dispatch (`layerRead.cpp:1244-1266`); they fall to `readUnknown` and become **empty group nodes — geometry entirely lost** |
| **`UsdSkelBlendShape` + blendshape weights** | Zero occurrences of "BlendShape" anywhere under `utils/`. No morph path exists; no `"weights"` animation channel in `gltfExport.cpp` |
| **Standalone `UsdSkelSkeleton`** (not under a `SkelRoot`) | No `IsA<UsdSkelSkeleton>()` case; skeletons only found via `UsdSkelCache::ComputeSkelBindings` inside `readSkelRoot` |
| **Subdivision creases / corners / holes** | `GetCreaseIndicesAttr`, `GetCornerIndicesAttr`, `GetHoleIndicesAttr` — 0 references. Only the `subdivisionScheme` *token* is carried (`layerRead.cpp:427`), without the data needed to actually refine correctly |
| **`purpose` (render/proxy/guide)** | Never read; proxy and guide geometry are imported identically to render geometry |
| **Mesh `orientation` (leftHanded/rightHanded)** | No field on `Mesh`; `GetOrientationAttr` 0 references — winding always assumed as-authored |
| **Arbitrary / custom primvars** | No generic primvar container on `Mesh`. Only `normals`, `tangents`, `bitangents`, texcoord-typed, `displayColor`, `displayOpacity`, and the fixed gsplat tokens are read |
| **Half-precision UVs** (`TexCoord2hArray`/`Half2Array`) | `layerRead.cpp:359` — explicit `// TODO add support for` |
| **Velocities / accelerations / point `ids`** | `GetVelocitiesAttr`/`GetAccelerationsAttr`/`GetIdsAttr` — 0 references (no motion blur, no stable point identity) |
| **Variant sets** | No `GetVariantSets` in `utils/`; only the composed default selection is ever seen |
| **`kind` / `UsdGeomModelAPI` / `assetInfo`** | 0 references in `utils/` |
| **Per-prim `customData`** | `Node::customProperties` exists (`usdData.h:95`) but is **never assigned** on read — write-path only |
| **Inherited visibility** | Only the prim's *own* `visibility` attr is checked; `ComputeVisibility()` never called. Code comment admits it "may still inherit invisibility from a parent" (`layerRead.cpp:70-71`) |
| **`!resetXformStack!`** | Retrieved into a local (`layerRead.cpp:99-100`) then **never inspected** — reset semantics silently ignored |
| **Non-NGP volumes** (`UsdVolVolume`, OpenVDB) | `// Currently, we only support NGP volume.` (`layerRead.cpp:998`) |
| **Dome-light IBL texture** | `// TODO: Add support for texture` (`layerRead.cpp:1181`) |
| **Animated lights** | `readCommonLightAttributes` uses `Get()` with no time arg; no time-sample loop anywhere |
| **Animated camera intrinsics** | `usdCamera.GetCamera(0)` — hardcoded time 0 |
| **Time-sampled points / topology (vertex-cache animation)** | Every geometry `Get(..., 0)` uses literal time `0` (`layerRead.cpp:424-427, 432, 441, 461, 469, 481-486`) — collapsed to a single frame |
| **PointInstancer animation, `invisibleIds`, `ids`** | Single `EarliestTime()` sample only (`layerRead.cpp:891`); no mask/ids/velocity calls |
| **`UsdGeomSubset` family/elementType filtering** | Every `UsdGeomSubset` child is treated as a face/material subset without checking `familyName`/`elementType` |
| **Material binding *purpose*** | `ComputeBoundMaterial()` called with no purpose arg — only `allPurpose` bindings honoured (`layerRead.cpp:534-535`) |
| **Arbitrary MaterialX / UsdShade node graphs** | `_handleShader` (`layerReadMaterialUtils.cpp:62-113`) matches against fixed handler tables; a mismatch emits `TF_WARN` and **aborts that input entirely** — custom nodes are dropped, not approximated |
| **`UsdPreviewSurface.displacement`** | Read (`layerReadMaterial.cpp:204,237,757`) but **0 references in `gltfExport.cpp`** |
| **`useSpecularWorkflow`** | Parsed and stored, **never consulted** on export |
| **UDIM textures** | No UDIM handling anywhere in the repo |
| **Physics / `UsdPhysics`, `UsdMedia`, `UsdRender`, `UsdProc`, `UsdUI`** | No schema references at all |

### 4.2 In `UsdData` but dropped on glTF export

- **Gaussian splats** (`asGsplats`, `pointSHCoeffs`, `pointRotations`) — read from USD, written back to USD, but **0 references in `gltf/src`**; TRIANGLES-only mode rules out even a point fallback.
- **Point clouds** generally — same reason.
- **Dome/environment lights** and `Light.texture` — explicitly skipped with `TF_WARN`.
- **`Light.angle`** (sun angular diameter) — declared, never read.
- **Camera `fStop` / `focusDistance` / `f`** — depth-of-field lost.
- **Extra color/opacity sets** beyond index 0 — only `colors[0]`/`opacities[0]` → `COLOR_0`.
- **Non-vertex-interpolated `displayColor`/`displayOpacity`** — dropped with `TF_WARN` rather than expanded (`gltfExport.cpp:2917-2922`).
- **Original `SdfPath`** — debug logging only.
- **`Node.customProperties`, `UsdData.doc`, `Mesh.instanceable`, `Node.worldTransform`** — never read in `gltf/src`.
- **`emission_luminance`** (OpenPBR HDR emissive) — never referenced on export.
- **Textured OpenPBR `base_weight`** — explicit `TF_WARN "not applied to baseColor"` (`gltfExport.cpp:2054-2056`).
- **Draco** — import-only; the exporter never compresses.
- **`KHR_materials_variants`** — only a commented-out string in the import path (`gltfImport.cpp:4197`).
- **`KHR_mesh_quantization`, `KHR_texture_basisu`, `KHR_xmp_json_ld`, `KHR_animation_pointer`, `EXT_meshopt_compression`, `EXT_mesh_gpu_instancing`** — never emitted.

---

## 5. Lossy conversions (correct output, degraded data)

1. **Fan triangulation assumes convexity.** `fanTriangulate` (`geometry.cpp:509-585`) has no ear-clipping or convexity test — **concave n-gons triangulate incorrectly**. Always on for glTF (`fileFormat.cpp:262`).
2. **Full de-indexing / vertex explosion.** `forceVertexInterpolation` (`geometry.cpp:1079-1138`) expands *all* attributes to one-vertex-per-face-corner whenever any primvar isn't `vertex`-interpolated. The code's own comment calls it *"HIGHLY inefficient"* and *"crude"* (`geometry.cpp:1102-1106, 1121`). Indices degenerate to `std::iota`. **No welding exists anywhere in `gltf/src`.**
3. **`constant` primvars expanded to per-vertex**, losing the "one value for the whole mesh" semantic (`geometry.cpp:970-977`).
4. **Indexed primvars flattened** via `ComputeFlattened` — index buffer discarded (`geometry.cpp:960-967`).
5. **Xform animation only for 7 hardcoded op patterns.** Only exact op orders drawn from `{Translate, Orient, Scale}` yield channels (`layerRead.cpp:225-233`, marked `// TODO review if we covered xformOperation possibilites correctly`). `rotateXYZ`, matrix ops, pivots, or multiple translates ⇒ **zero animation extracted**, just a static matrix.
6. **Skeleton scale → half-precision.** `SkeletonAnimation::scales` is `VtArray<GfVec3h>` (`usdData.h:249`); shear is dropped by `UsdSkelDecomposeTransform`.
7. **All animation forced to LINEAR** — STEP and CUBICSPLINE never produced.
8. **Light intensity is empirical.** Multipliers `0.225` / `0.0000625` / `1.0` are literally commented "Experimentally found to result in similar brightnesses" (`gltf.h:19-22`) — not physically derived.
9. **HDR/EXR → 8-bit PNG.** Any image outside `{png,jpg,bmp,webp}` is force-converted (`gltfExport.cpp:2598-2601`) — irreversible for float/HDR sources.
10. **Anisotropy reparameterisation** (`gltfAnisotropyOpenPBR.cpp:82-99`): `om = 1−aniso`, `factor = √(2/(1+om²))`, `αt = rough²·factor`, `αb = αt·om`, `rough_out = √αb`, `strength = √((αt−αb)/(1−αb))` — lossy round-trip; ASM variant undefined and clamped when `roughness ≥ 1`.
11. **🐛 Silent texture loss on ORM packing.** `translateMix` requires identical source resolutions (`images.cpp:306-314` + `GUARD`, `common.h:361-366`), but **the call sites never check its return value** (`gltfExport.cpp:2265, 2365`). A resolution mismatch ⇒ empty `Input` ⇒ **no texture emitted at all, with no warning.**
12. **🐛 UV set silently forced to 0 on packing.** `translateMix` hardcodes `out.uvIndex = 0` (`materials.cpp:1919`). It warns on mismatched UV *transforms* but never on mismatched UV *set indices* — packing occlusion from `TEXCOORD_1` with roughness from `TEXCOORD_0` silently mislabels the result.
13. **Multi-root skeletons:** `skin.skeleton` is simply **omitted** (not approximated) when `rootCount != 1`, to dodge a validator warning (`gltfExport.cpp:739-743`).
14. **`Mesh::isRigid` never stored** — computed locally in `readSkinData` (`layerRead.cpp:645`) but never assigned to the struct field.

---

## 6. Implications for the Babylon.js USD importer

**Use this plugin as a reference for:**
- ✅ **Material translation.** The OpenPBR→`KHR_materials_*` mapping (§3.8) is the most complete open-source treatment available, including the exact default-value reconciliations (metallic 0.0, roughness 0.5) and the transmission→opacity fallback (`opacity.scale = −(transmission.scale × 0.75)`, `bias = 1 − transmission.bias`) for engines without transmission support.
- ✅ **Skinning.** The >4-influence splitting into `JOINTS_n/WEIGHTS_n` and duplicate-joint merging is directly reusable — Babylon.js supports 8 influences natively.
- ✅ **Camera math** — delegate to `GfCamera::GetFieldOfView`, don't reimplement.
- ✅ **Up-axis / metersPerUnit** — the correction-node approach maps cleanly onto a Babylon `TransformNode` root.

**Do NOT inherit its ceiling for:**
- ❌ **Implicit gprims** (`Cube`/`Sphere`/`Cylinder`/`Cone`/`Capsule`). These are extremely common in real USD content and are silently reduced to empty nodes here. Babylon has `MeshBuilder.CreateBox/Sphere/Cylinder` — tessellate them directly. **This is the single highest-value gap to close.**
- ❌ **Blend shapes.** `UsdSkelBlendShape` is completely absent. Babylon has `MorphTargetManager` — read it straight from the stage.
- ❌ **Vertex/topology animation.** Everything is frozen at time 0. If you want USD vertex caches, you must sample `points` across time yourself.
- ❌ **`purpose`.** Proxy and guide geometry currently render as if they were render geometry. A web viewer arguably wants the *proxy* representation preferentially — this is a genuine opportunity, not just a gap.
- ❌ **Variants.** A viewer that can switch variant sets is a major USD-specific differentiator glTF simply cannot express.
- ❌ **Curves / point clouds / Gaussian splats.** Babylon supports `LinesMesh`, `PointsCloudSystem`, and has a Gaussian-splatting implementation — all three are representable natively but unrepresentable in glTF.
- ❌ **Dome lights / IBL.** Skipped entirely. Babylon's `HDRCubeTexture`/environment pipeline handles this well.
- ❌ **Instancing.** Point instancers are flattened to N duplicated nodes. Babylon's `InstancedMesh`/thin instances map onto `UsdGeomPointInstancer` almost perfectly — flattening would be a large, avoidable perf regression.
- ❌ **Subdivision surfaces.** Only the scheme token survives; creases/corners/holes are dropped.

**Recommended architecture:** treat glTF as a *material transport format*, not a scene transport format. Port the material logic (`utils/src/materials.cpp` + `gltfExport.cpp:2036-2721`) as the conversion reference, but read geometry, instancing, variants, purpose, blend shapes, and time-varying data **directly from the USD stage** into Babylon primitives.

---

## 7. Cross-check against the plugin's own README

`gltf/README.md` self-reports `Mesh blend shapes ❌`, `Nurbs ❌`, `Node TRS ⚠️`, and `Mesh bounding box ✅` for export — consistent with the code. However the README **understates** the losses: it does not mention implicit gprims, `purpose`, variants, creases, custom primvars, inherited visibility, `resetXformStack`, animated cameras/lights, or point/gsplat export. Treat §4 above as the authoritative list.
