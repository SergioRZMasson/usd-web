/**
 * Loads the Emscripten-generated glue code.
 *
 * The glue module is produced by the native CMake/Emscripten build and only exists in
 * `dist`, never in the source tree. The specifier is held in a variable so TypeScript and
 * bundlers treat it as an opaque runtime import rather than something to resolve.
 */

import type { UsdGltfModuleFactory } from './module.js';
import type { ConverterBackend } from './types.js';

/** Paths of the independently linked glue modules relative to the emitted bundle. */
const GLUE_SPECIFIERS: Record<ConverterBackend, string> = {
    gltf: './wasm/usd-web-gltf.js',
    babylon: './wasm/usd-web-babylon.js',
};

/**
 * Absolute URL of the directory holding the glue script, the `.wasm` binary and the
 * `.data` resource bundle, or `undefined` when the default resolution already works.
 *
 * Emscripten resolves the `.wasm` against the script directory but the `.data` package
 * against the *document*, so a browser page served from a different path than the
 * artifacts fails to find the resource bundle. Returning a base lets the converter
 * install a `locateFile` that resolves both consistently.
 *
 * Only http(s) bases are returned: under Node the glue resolves siblings against its own
 * path correctly, and handing it a `file://` URL would make it try to open that string
 * as a filesystem path.
 */
export function getGlueBaseUrl(backend: ConverterBackend): string | undefined {
    try {
        const specifier = GLUE_SPECIFIERS[backend];
        const url = new URL(specifier, import.meta.url);
        if (url.protocol !== 'http:' && url.protocol !== 'https:') {
            return undefined;
        }
        return url.href.slice(0, url.href.lastIndexOf('/') + 1);
    } catch {
        return undefined;
    }
}

/** Dynamically imports the Emscripten factory. */
export async function loadModuleFactory(
    backend: ConverterBackend,
): Promise<UsdGltfModuleFactory> {
    const specifier = GLUE_SPECIFIERS[backend];
    const glue = (await import(/* @vite-ignore */ /* webpackIgnore: true */ specifier)) as {
        default: UsdGltfModuleFactory;
    };

    if (typeof glue.default !== 'function') {
        throw new TypeError(
            `${specifier} did not export an Emscripten module factory as its default export.`,
        );
    }

    return glue.default;
}
