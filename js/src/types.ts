/**
 * Type definitions for the usd-web-gltf converter.
 */

/** Binary input accepted by the converter. */
export type BinaryInput = ArrayBuffer | ArrayBufferView | Uint8Array;

/** Sidecar assets keyed by the path referenced from the USD layer. */
export type VirtualFiles = Record<string, BinaryInput>;

/** Severity of a diagnostic emitted by OpenUSD. */
export type LogLevel = 'info' | 'warning' | 'error';

/** A diagnostic emitted while converting. */
export interface LogMessage {
    level: LogLevel;
    message: string;
}

/** Intermediate format produced before Babylon.js loads the scene. */
export type OutputFormat = 'glb' | 'gltf' | 'babylon';
/** Independently linked WebAssembly exporter module. */
export type ConverterBackend = 'gltf' | 'babylon';

/** Options for instantiating the WebAssembly module. */
export interface CreateConverterOptions {
    /** Exporter module to instantiate. Defaults to `"gltf"`. */
    backend?: ConverterBackend;

    /**
     * Explicit URL of the selected backend's `.wasm`. When omitted the file is resolved
     * relative to its generated glue script.
     */
    wasmUrl?: string | URL;

    /**
     * Overrides Emscripten's asset resolution. Takes precedence over {@link wasmUrl}.
     * Both the `.wasm` binary and the `.data` resource bundle are routed through this.
     */
    locateFile?: (path: string, scriptDirectory: string) => string;

    /** Pre-fetched WebAssembly binary, skipping the network request. */
    wasmBinary?: ArrayBuffer | Uint8Array;

    /** Receives diagnostics emitted by OpenUSD. */
    onLog?: (message: LogMessage) => void;
}

/** Options for a single conversion. */
export interface ConvertOptions {
    /**
     * Name of the primary file including extension. The extension selects the importer,
     * so it must be `.usd`, `.usda`, `.usdc` or `.usdz`. Defaults to `"input.usdz"`.
     */
    fileName?: string;

    /**
     * Additional assets (textures, referenced layers) made visible to USD's asset
     * resolver, keyed by the path used inside the USD layer.
     *
     * Preserve the original directory structure relative to the root layer — that is what
     * lets relative references such as `@sub/robot.usda@` resolve. Unnecessary for
     * self-contained `.usdz` archives.
     */
    additionalFiles?: VirtualFiles;

    /**
     * Resolve references that cannot be found by matching on file name. Defaults to
     * `true`.
     *
     * USD files exported from DCC tools frequently carry absolute paths from the machine
     * they were authored on (`@/Users/someone/Desktop/robot.usd@`). Those can never
     * resolve as written, so any supplied file with a matching name is used instead. Set
     * to `false` to require exact resolution.
     */
    resolveByFileName?: boolean;

    /** Output container. Defaults to `"glb"` for the glTF backend and `"babylon"` otherwise. */
    format?: OutputFormat;

    /**
     * Losslessly weld duplicate vertices and optimize triangle/vertex order for GPU caches.
     * Defaults to `true`.
     */
    optimizeMeshes?: boolean;

    /**
     * Emit `EXT_meshopt_compression` for GLB buffer views. This substantially reduces
     * download size and is decoded efficiently by Babylon.js and other meshopt-aware
     * glTF loaders. Defaults to `true` for GLB and is ignored for JSON glTF output.
     */
    meshoptCompression?: boolean;

    /**
     * Embed texture payloads as data URLs in `.babylon` output. Defaults to `true` and is
     * ignored for glTF/GLB output.
     */
    embedTextures?: boolean;

    /** Diagnostics for this conversion only. */
    onLog?: (message: LogMessage) => void;
}

/** Result of a successful conversion. */
export interface ConvertResult {
    /** The encoded GLB, glTF JSON, or Babylon JSON payload. */
    data: Uint8Array;

    /** Suggested output file name, derived from the input name. */
    fileName: string;

    /** Duration of the native conversion, in milliseconds. */
    durationMs: number;

    /** Diagnostics emitted during this conversion. */
    log: LogMessage[];

    /**
     * File names the asset referenced but that were never supplied.
     *
     * The conversion still succeeds — USD drops the unresolvable payload — so this is the
     * signal that content is missing from the output.
     */
    missingAssets: string[];
}

/** Details about the loaded native module. */
export interface ConverterInfo {
    /** Independently linked exporter module backing this converter. */
    backend: ConverterBackend;
    /** OpenUSD version the module was built from, e.g. `"0.26.8"`. */
    usdVersion: string;
    /** Extensions USD can write, as registered by the linked file format plugins. */
    supportedOutputFormats: readonly string[];
    /** Whether Adobe's glTF file format plugin registered successfully. */
    gltfPluginAvailable: boolean;
    /** Whether the export-only Babylon JSON file format plugin registered successfully. */
    babylonPluginAvailable: boolean;
    /** Asset resolver in use — `"WebResolver"` when name-fallback is available. */
    resolver: string;
}
