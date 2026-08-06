/**
 * Low-level typings for the Emscripten module produced by the native build.
 *
 * Must stay in sync with the EMSCRIPTEN_BINDINGS block in `src/bindings.cpp`.
 */

/** Native object owning wasm heap memory; must be explicitly freed. */
export interface EmbindObject {
    delete(): void;
}

/** Native result of a conversion. */
export interface NativeConvertResult extends EmbindObject {
    ok(): boolean;
    error(): string;
    dataPtr(): number;
    dataSize(): number;
    durationMs(): number;
}

/** The Emscripten module instance. */
export interface UsdGltfModule {
    HEAPU8: Uint8Array;

    FS: {
        writeFile(path: string, data: Uint8Array): void;
        unlink(path: string): void;
        mkdir(path: string): void;
        analyzePath(path: string): { exists: boolean };
    };

    /** Converts a file already present in the virtual filesystem. */
    convert(inputPath: string, outputPath: string): NativeConvertResult;

    /** Installs a callback invoked for every OpenUSD diagnostic. */
    setLogCallback(callback: (level: number, message: string) => void): void;

    /** Comma-separated list of writable extensions. */
    getSupportedOutputFormats(): string;

    /** True when Adobe's glTF file format plugin resolved. */
    isGltfPluginAvailable(): boolean;

    /** OpenUSD version, e.g. `"0.26.8"`. */
    getUsdVersion(): string;

    /** Indexes every file under `directory` for name-based fallback resolution. */
    registerAssetDirectory(directory: string): void;

    /** Drops the fallback index so one asset's files cannot satisfy another's. */
    clearAssetIndex(): void;

    /** Enables or disables name-based fallback resolution. */
    setAssetFallbackEnabled(enabled: boolean): void;

    /** Comma-separated file names that could not be resolved. */
    getUnresolvedAssets(): string;

    /** Name of the asset resolver Ar instantiated, e.g. `"WebResolver"`. */
    getResolverName(): string;
}

/** Factory signature exported by the Emscripten glue. */
export type UsdGltfModuleFactory = (
    moduleArg?: Partial<EmscriptenModuleOverrides>,
) => Promise<UsdGltfModule>;

/** The subset of Emscripten's overrides this package sets. */
export interface EmscriptenModuleOverrides {
    locateFile: (path: string, scriptDirectory: string) => string;
    wasmBinary: ArrayBuffer | Uint8Array;
    print: (message: string) => void;
    printErr: (message: string) => void;
    /** Invoked when the runtime terminates fatally; the instance is dead afterwards. */
    onAbort: (reason: unknown) => void;
    noExitRuntime: boolean;
}
