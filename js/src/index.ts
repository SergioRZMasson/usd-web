/**
 * usd-web-gltf — OpenUSD and Adobe's usdGltf plugin, compiled to WebAssembly.
 *
 * Converts USD (`.usd`, `.usda`, `.usdc`, `.usdz`) to glTF/GLB entirely in the browser,
 * so it can be handed straight to a glTF-capable renderer such as Babylon.js.
 *
 * @example One-liner
 * ```ts
 * import { usdToGlb } from 'usd-web-gltf';
 *
 * const glb = await usdToGlb(await file.arrayBuffer(), { fileName: file.name });
 * ```
 *
 * @example Load into a Babylon.js scene
 * ```ts
 * import { loadUsdIntoScene } from 'usd-web-gltf/babylon';
 *
 * await loadUsdIntoScene(bytes, scene, { fileName: 'chair.usdz' });
 * ```
 *
 * @packageDocumentation
 */

export { UsdConverter } from './converter.js';

export {
    PolymorphError as UsdWebGltfError,
    ModuleLoadError,
    ModuleAbortedError,
    InvalidInputError,
    ConversionError,
    ConversionAbortedError,
    DisposedError,
} from './errors.js';

export type {
    BinaryInput,
    ConvertOptions,
    ConvertResult,
    ConverterBackend,
    ConverterInfo,
    CreateConverterOptions,
    LogLevel,
    LogMessage,
    OutputFormat,
    VirtualFiles,
} from './types.js';

import { UsdConverter } from './converter.js';
import type {
    BinaryInput,
    ConvertOptions,
    ConverterBackend,
    CreateConverterOptions,
} from './types.js';

/** Lazily-created converters, kept separate so wasm size/runtime comparisons stay isolated. */
const sharedConverters: Partial<Record<ConverterBackend, Promise<UsdConverter>>> = {};

/**
 * Returns a process-wide shared {@link UsdConverter}, instantiating the WebAssembly
 * module on first use.
 *
 * Prefer this over constructing a converter per call: instantiation dominates the cost of
 * a conversion. If a previous conversion aborted the runtime, a fresh module is created
 * transparently.
 */
export function getSharedConverter(options?: CreateConverterOptions): Promise<UsdConverter> {
    const backend = options?.backend ?? 'gltf';
    sharedConverters[backend] = (
        sharedConverters[backend] ?? create({ ...options, backend })
    ).then((converter) => {
        // A fatal USD error terminates the runtime; the instance cannot be reused.
        if (converter.aborted) {
            sharedConverters[backend] = create({ ...options, backend });
            return sharedConverters[backend];
        }
        return converter;
    });
    return sharedConverters[backend];
}

function create(options?: CreateConverterOptions): Promise<UsdConverter> {
    return UsdConverter.create(options).catch((error: unknown) => {
        // Don't cache a failed instantiation; a later call may succeed once the caller
        // corrects the wasm URL.
        sharedConverters[options?.backend ?? 'gltf'] = undefined;
        throw error;
    });
}

/**
 * Converts a USD asset to GLB bytes using the shared converter.
 *
 * @param input   The `.usd`/`.usda`/`.usdc`/`.usdz` payload.
 * @param options Conversion options; `fileName` selects the importer.
 * @returns The GLB payload.
 */
export async function usdToGlb(
    input: BinaryInput,
    options?: ConvertOptions,
): Promise<Uint8Array> {
    const converter = await getSharedConverter({ backend: 'gltf' });
    const result = await converter.convert(input, { ...options, format: 'glb' });
    return result.data;
}

/**
 * Converts a USD asset to Babylon.js' native JSON scene format.
 */
export async function usdToBabylon(
    input: BinaryInput,
    options?: ConvertOptions,
): Promise<Uint8Array> {
    const converter = await getSharedConverter({ backend: 'babylon' });
    const result = await converter.convert(input, { ...options, format: 'babylon' });
    return result.data;
}

/**
 * Converts a USD asset and returns an object URL pointing at the resulting GLB.
 *
 * Remember to `URL.revokeObjectURL` the result when the asset is no longer needed.
 * Browser only.
 */
export async function usdToGlbObjectUrl(
    input: BinaryInput,
    options?: ConvertOptions,
): Promise<string> {
    const glb = await usdToGlb(input, options);
    // Copy into a fresh ArrayBuffer: the view may be backed by wasm memory that later
    // grows, which would detach it.
    const blob = new Blob([glb.slice()], { type: 'model/gltf-binary' });
    return URL.createObjectURL(blob);
}
