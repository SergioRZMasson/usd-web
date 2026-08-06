/**
 * Babylon.js integration helpers.
 *
 * Kept in a separate entry point so the core package has no dependency on Babylon: import
 * from `usd-web-gltf/babylon` only if you use these.
 *
 * Babylon's glTF loader is the target renderer here — it consumes the converter's output
 * directly, so USD support costs nothing beyond the conversion itself.
 */

import { usdToGlb } from './index.js';
import type { BinaryInput, ConvertOptions } from './types.js';

/** Result of {@link loadUsdIntoScene}. */
export interface LoadedUsdAsset<TResult> {
    /** Whatever the Babylon loader returned. */
    result: TResult;
    /** Duration of the USD to GLB conversion, in milliseconds. */
    conversionMs: number;
    /** Size of the intermediate GLB, in bytes. */
    glbByteLength: number;
}

/** Options for {@link loadUsdIntoScene}. */
export interface LoadUsdOptions extends ConvertOptions {
    /**
     * The Babylon loader entry point to use — typically `AppendSceneAsync`,
     * `LoadAssetContainerAsync` or `ImportMeshAsync`.
     *
     * Passing it explicitly keeps this package decoupled from any particular Babylon
     * version or module layout, and lets the caller choose how the content is added.
     *
     * It receives the GLB bytes, the scene, and options carrying `pluginExtension`.
     */
    load: (
        source: Uint8Array,
        scene: unknown,
        options: { pluginExtension: string },
    ) => Promise<unknown>;
}

/**
 * Converts a USD asset and hands the resulting GLB to Babylon.
 *
 * The bytes are passed straight through: Babylon's `SceneSource` accepts an
 * `ArrayBufferView`, so no blob URL is created and nothing needs revoking.
 *
 * @example
 * ```ts
 * import { AppendSceneAsync } from '@babylonjs/core';
 * import '@babylonjs/loaders/glTF';
 * import { loadUsdIntoScene } from 'usd-web-gltf/babylon';
 *
 * await loadUsdIntoScene(bytes, scene, {
 *     fileName: 'chair.usdz',
 *     load: (glb, scene, options) => AppendSceneAsync(glb, scene as Scene, options),
 * });
 * ```
 *
 * @remarks
 * Meshes that carried only `primvars:displayColor` arrive with a `COLOR_0` attribute and
 * no glTF material, because Adobe's exporter only emits a material for prims with a
 * `material:binding`. Babylon's default material ignores vertex colours, so assign one
 * with `useVertexColors = true` to those meshes if you need the authored colour.
 */
export async function loadUsdIntoScene<TResult = unknown>(
    input: BinaryInput,
    scene: unknown,
    options: LoadUsdOptions,
): Promise<LoadedUsdAsset<TResult>> {
    const { load, ...convertOptions } = options;

    const started = Date.now();
    const glb = await usdToGlb(input, convertOptions);
    const conversionMs = Date.now() - started;

    // '.glb' is required: the loader has no filename to infer the format from.
    const result = (await load(glb, scene, { pluginExtension: '.glb' })) as TResult;

    return { result, conversionMs, glbByteLength: glb.byteLength };
}
