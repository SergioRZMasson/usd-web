/**
 * The high-level converter API.
 */

import {
    ConversionError,
    DisposedError,
    InvalidInputError,
    ModuleAbortedError,
    ModuleLoadError,
} from './errors.js';
import {
    assertSupportedExtension,
    sanitizeVirtualPath,
    SUPPORTED_EXTENSIONS,
    toUint8Array,
    withExtension,
} from './internal.js';
import { loadModuleFactory, getGlueBaseUrl } from './glue.js';
import type { EmscriptenModuleOverrides, UsdGltfModule, UsdGltfModuleFactory } from './module.js';
import type {
    BinaryInput,
    ConvertOptions,
    ConvertResult,
    ConverterInfo,
    ConverterBackend,
    CreateConverterOptions,
    LogLevel,
    LogMessage,
} from './types.js';

/** Working directory inside the Emscripten virtual filesystem. */
const SCRATCH_DIR = '/work';

/** Maps the native log level enum onto its string form. */
const LOG_LEVELS: readonly LogLevel[] = ['info', 'warning', 'error'];

/**
 * Converts USD assets through an independently linked GLB or Babylon JSON exporter.
 *
 * The conversion runs inside OpenUSD itself. The selected module statically links exactly
 * one exporter plugin so download size and runtime cost can be compared independently.
 *
 * Instantiating the module is expensive (it loads a ~12 MB binary), so create one
 * converter and reuse it:
 *
 * ```ts
 * const converter = await UsdConverter.create();
 * const { data } = await converter.convert(bytes, { fileName: 'chair.usdz' });
 * ```
 */
export class UsdConverter {
    readonly #module: UsdGltfModule;
    readonly #backend: ConverterBackend;
    readonly #onLog: ((message: LogMessage) => void) | undefined;

    /** Diagnostics collected for the conversion currently in flight. */
    #activeLog: LogMessage[] = [];
    /** Per-conversion log sink, layered on top of the module-wide one. */
    #scopedLog: ((message: LogMessage) => void) | undefined;
    /** Serialises access to the single-threaded native module. */
    #queue: Promise<unknown> = Promise.resolve();
    #disposed = false;
    /** Populated by Emscripten's onAbort when the runtime terminates. */
    readonly #abortState: { reason?: string };

    private constructor(
        module: UsdGltfModule,
        backend: ConverterBackend,
        abortState: { reason?: string },
        onLog?: (message: LogMessage) => void,
    ) {
        this.#module = module;
        this.#backend = backend;
        this.#abortState = abortState;
        this.#onLog = onLog;

        module.setLogCallback((level: number, message: string) => {
            const entry: LogMessage = { level: LOG_LEVELS[level] ?? 'info', message };
            this.#activeLog.push(entry);
            this.#onLog?.(entry);
            this.#scopedLog?.(entry);
        });

        if (!module.FS.analyzePath(SCRATCH_DIR).exists) {
            module.FS.mkdir(SCRATCH_DIR);
        }
    }

    /**
     * Loads and instantiates the WebAssembly module.
     *
     * @throws {@link ModuleLoadError} if the binary cannot be fetched or instantiated.
     */
    static async create(options: CreateConverterOptions = {}): Promise<UsdConverter> {
        const { backend = 'gltf', wasmUrl, locateFile, wasmBinary, onLog } = options;

        let factory: UsdGltfModuleFactory;
        try {
            factory = await loadModuleFactory(backend);
        } catch (cause) {
            throw new ModuleLoadError(
                'the generated glue code could not be imported. Did you run the native build?',
                { cause },
            );
        }

        const overrides: Partial<EmscriptenModuleOverrides> = { noExitRuntime: true };

        // OpenUSD treats some malformed input as fatal, which aborts the wasm runtime.
        // The instance is dead afterwards, so the flag is captured here and surfaced as
        // a typed error rather than letting later calls fail incomprehensibly.
        const abortState: { reason?: string } = {};
        overrides.onAbort = (reason: unknown) => {
            abortState.reason = typeof reason === 'string' ? reason : String(reason ?? '');
        };

        if (wasmBinary) {
            overrides.wasmBinary =
                wasmBinary instanceof Uint8Array ? wasmBinary : new Uint8Array(wasmBinary);
        }

        if (locateFile) {
            overrides.locateFile = locateFile;
        } else {
            // In the browser, Emscripten resolves the .wasm against the script directory
            // but the .data resource bundle against the document, so a page served from
            // a different path than the artifacts cannot find the schemas. Anchoring
            // both to the glue script's own directory makes the package work wherever it
            // is mounted. Under Node the default resolution is already correct.
            const base = getGlueBaseUrl(backend);
            const wasmOverride = wasmUrl ? String(wasmUrl) : undefined;
            if (base || wasmOverride) {
                overrides.locateFile = (path: string, scriptDirectory: string) => {
                    if (wasmOverride && path.endsWith('.wasm')) return wasmOverride;
                    return (base ?? scriptDirectory) + path;
                };
            }
        }

        let module: UsdGltfModule;
        try {
            module = await factory(overrides);
        } catch (cause) {
            throw new ModuleLoadError(cause instanceof Error ? cause.message : String(cause), {
                cause,
            });
        }

        const pluginAvailable =
            backend === 'gltf'
                ? module.isGltfPluginAvailable()
                : module.isBabylonPluginAvailable();
        if (!pluginAvailable) {
            throw new ModuleLoadError(
                `the ${backend} file format plugin did not register. The build is missing its ` +
                    'plugInfo.json resources or was linked without --whole-archive.',
            );
        }

        return new UsdConverter(module, backend, abortState, onLog);
    }

    /** Metadata describing the loaded native module. */
    get info(): ConverterInfo {
        this.#assertUsable();
        return {
            backend: this.#backend,
            usdVersion: this.#module.getUsdVersion(),
            supportedOutputFormats: this.#module.getSupportedOutputFormats().split(','),
            gltfPluginAvailable: this.#module.isGltfPluginAvailable(),
            babylonPluginAvailable: this.#module.isBabylonPluginAvailable(),
            resolver: this.#module.getResolverName(),
        };
    }

    /** Extensions accepted by {@link convert}, without the leading dot. */
    static get supportedInputFormats(): readonly string[] {
        return SUPPORTED_EXTENSIONS;
    }

    /**
     * Converts a USD asset using this converter's linked exporter backend.
     *
     * @throws {@link InvalidInputError} when the input or file name is unusable.
     * @throws {@link ConversionError} when OpenUSD fails to open or export the stage.
     */
    async convert(input: BinaryInput, options: ConvertOptions = {}): Promise<ConvertResult> {
        this.#assertUsable();

        const fileName = options.fileName ?? 'input.usdz';
        assertSupportedExtension(fileName);

        const bytes = toUint8Array(input);
        if (bytes.byteLength === 0) {
            throw new InvalidInputError('The supplied USD payload is empty.');
        }

        const run = this.#queue.then(
            () => this.#convert(bytes, fileName, options),
            () => this.#convert(bytes, fileName, options),
        );
        this.#queue = run.catch(() => undefined);
        return run;
    }

    async #convert(
        bytes: Uint8Array,
        fileName: string,
        options: ConvertOptions,
    ): Promise<ConvertResult> {
        const {
            additionalFiles,
            embedTextures = true,
            format = this.#backend === 'babylon' ? 'babylon' : 'glb',
            meshoptCompression = true,
            optimizeMeshes = true,
            resolveByFileName = true,
        } = options;
        if (
            (this.#backend === 'babylon' && format !== 'babylon') ||
            (this.#backend === 'gltf' && format === 'babylon')
        ) {
            throw new InvalidInputError(
                `The ${this.#backend} WebAssembly module cannot export ${format}.`,
            );
        }

        const module = this.#module;
        const written: string[] = [];
        const inputPath = `${SCRATCH_DIR}/${sanitizeVirtualPath(fileName)}`;
        const outputPath = withExtension(inputPath, format);

        this.#activeLog = [];
        this.#scopedLog = options.onLog;

        try {
            this.#ensureParentDirectories(inputPath);
            module.FS.writeFile(inputPath, bytes);
            written.push(inputPath);

            if (additionalFiles) {
                for (const [rawPath, payload] of Object.entries(additionalFiles)) {
                    const target = `${SCRATCH_DIR}/${sanitizeVirtualPath(rawPath)}`;
                    this.#ensureParentDirectories(target);
                    module.FS.writeFile(
                        target,
                        toUint8Array(payload, `additionalFiles["${rawPath}"]`),
                    );
                    written.push(target);
                }
            }

            // Index what was just written so references that fail normal resolution can
            // fall back to a file-name match. Cleared first so a previous conversion's
            // files can never satisfy this one's references.
            module.clearAssetIndex();
            module.setAssetFallbackEnabled(resolveByFileName);
            module.registerAssetDirectory(SCRATCH_DIR);

            const native = module.convertWithOptions(
                inputPath,
                outputPath,
                optimizeMeshes,
                format === 'glb' && meshoptCompression,
                format === 'babylon' && embedTextures,
            );
            // A fatal USD error terminates the runtime from inside the call above, so
            // this is checked before touching any of the returned values.
            if (this.#abortState.reason !== undefined) {
                throw new ModuleAbortedError(this.#abortState.reason);
            }
            written.push(outputPath);

            try {
                if (!native.ok()) {
                    throw new ConversionError(native.error(), this.#activeLog);
                }
                // Copy out of the wasm heap before the native buffer is freed; memory
                // growth can also detach the previous HEAPU8 view.
                const ptr = native.dataPtr();
                const size = native.dataSize();
                const data = module.HEAPU8.slice(ptr, ptr + size);

                return {
                    data,
                    fileName: withExtension(fileName, format),
                    durationMs: native.durationMs(),
                    log: this.#activeLog,
                    missingAssets: module
                        .getUnresolvedAssets()
                        .split(',')
                        .filter((name) => name.length > 0),
                };
            } finally {
                native.delete();
            }
        } finally {
            this.#scopedLog = undefined;
            for (const path of written) {
                try {
                    module.FS.unlink(path);
                } catch {
                    // Already gone, or never created because the export failed.
                }
            }
            this.#activeLog = [];
        }
    }

    /** Creates any missing parent directories for a virtual file path. */
    #ensureParentDirectories(path: string): void {
        const segments = path.split('/').slice(1, -1);
        let current = '';
        for (const segment of segments) {
            current += `/${segment}`;
            if (!this.#module.FS.analyzePath(current).exists) {
                this.#module.FS.mkdir(current);
            }
        }
    }

    /** Releases the converter. The instance cannot be used afterwards. */
    dispose(): void {
        this.#disposed = true;
    }

    /** True once {@link dispose} has been called. */
    get disposed(): boolean {
        return this.#disposed;
    }

    #assertUsable(): void {
        if (this.#disposed) {
            throw new DisposedError();
        }
        if (this.#abortState.reason !== undefined) {
            throw new ModuleAbortedError(this.#abortState.reason);
        }
    }

    /**
     * True when the WebAssembly runtime has aborted.
     *
     * OpenUSD treats some malformed input as fatal, which terminates the runtime. Once
     * this is true the instance is unusable and a new converter must be created.
     */
    get aborted(): boolean {
        return this.#abortState.reason !== undefined;
    }
}
