import { UsdConverter } from './usd-web.js';

const converters = new Map();
let directModulePromise;
let activeRequestId = 0;
let storedAsset = null;
let queue = Promise.resolve();

function post(type, payload = {}) {
    self.postMessage({ type, requestId: activeRequestId, ...payload });
}

async function getConverter(backend) {
    if (converters.has(backend)) {
        return await converters.get(backend);
    }

    post('progress', { message: `Loading the ${backend === 'gltf' ? 'GLB' : '.babylon'} exporter...` });
    const pending = UsdConverter.create({
        backend,
        onLog: (entry) => post('log', { entry }),
    });
    converters.set(backend, pending);
    try {
        const converter = await pending;
        converters.set(backend, converter);
        return converter;
    } catch (error) {
        converters.delete(backend);
        throw error;
    }
}

async function getDirectModule() {
    if (!directModulePromise) {
        post('progress', { message: 'Loading the direct OpenUSD bridge...' });
        directModulePromise = import('./wasm/openusd-babylon.js')
            .then(({ default: createModule }) =>
                createModule({
                    noExitRuntime: true,
                    locateFile: (path) => new URL(`./wasm/${path}`, import.meta.url).href,
                }),
            )
            .catch((error) => {
                directModulePromise = undefined;
                throw error;
            });
    }
    return await directModulePromise;
}

function ensureDirectory(module, path, created) {
    const parts = path.split('/').filter(Boolean);
    let current = '';
    for (const part of parts) {
        current += `/${part}`;
        if (!module.FS.analyzePath(current).exists) {
            module.FS.mkdir(current);
            created?.push(current);
        }
    }
}

function parentPath(path) {
    const slash = path.lastIndexOf('/');
    return slash > 0 ? path.slice(0, slash) : '/';
}

function normalizedVirtualPath(path) {
    const normalized = path.replace(/\\/g, '/');
    const parts = normalized.split('/').filter((part) => part && part !== '.');
    if (normalized.startsWith('/') || parts.length === 0 || parts.includes('..')) {
        throw new Error(`Invalid virtual asset path: ${path}`);
    }
    return parts.join('/');
}

async function initialize(message) {
    if (message.backend === 'direct') {
        const module = await getDirectModule();
        post('initialized', {
            info: {
                backend: 'direct',
                usdVersion: module.getUsdVersion(),
                supportedOutputFormats: ['command-buffer'],
                resolver: module.getResolverName(),
            },
        });
        return;
    }
    const converter = await getConverter(message.backend);
    post('initialized', { info: converter.info });
}

async function convertDirect(message) {
    const module = await getDirectModule();
    module.setLogCallback((level, text) => {
        post('log', {
            entry: {
                level: level >= 2 ? 'error' : level === 1 ? 'warning' : 'info',
                message: text,
            },
        });
    });
    const root = `/direct/${message.requestId}`;
    const written = [];
    const writtenPaths = new Set();
    const createdDirectories = [];
    ensureDirectory(module, root, createdDirectories);
    try {
        const inputPath = `${root}/${normalizedVirtualPath(storedAsset.fileName)}`;
        ensureDirectory(module, parentPath(inputPath), createdDirectories);
        module.FS.writeFile(inputPath, storedAsset.bytes);
        written.push(inputPath);
        writtenPaths.add(inputPath);
        for (const [relativePath, bytes] of Object.entries(storedAsset.additionalFiles ?? {})) {
            const path = `${root}/${normalizedVirtualPath(relativePath)}`;
            if (writtenPaths.has(path)) {
                throw new Error(`Duplicate virtual asset path: ${relativePath}`);
            }
            ensureDirectory(module, parentPath(path), createdDirectories);
            module.FS.writeFile(path, bytes);
            written.push(path);
            writtenPaths.add(path);
        }
        module.clearAssetIndex();
        module.setAssetFallbackEnabled(true);
        module.registerAssetDirectory(root);
        post('progress', { message: `Extracting ${storedAsset.fileName} directly...` });
        const result = module.extract(inputPath);
        try {
            if (!result.ok()) throw new Error(result.error());
            const copyStarted = performance.now();
            const commands = module.HEAPU8.slice(
                result.commandPtr(),
                result.commandPtr() + result.commandSize(),
            );
            const data = module.HEAPU8.slice(
                result.dataPtr(),
                result.dataPtr() + result.dataSize(),
            );
            const heapCopyMs = performance.now() - copyStarted;
            self.postMessage(
                {
                    type: 'result',
                    requestId: activeRequestId,
                    result: {
                        kind: 'direct',
                        commands,
                        data,
                        timings: {
                            durationMs: result.totalMs() + heapCopyMs,
                            nativeDurationMs: result.totalMs(),
                            stageOpenMs: result.stageOpenMs(),
                            stageReadMs: result.stageReadMs(),
                            preparationMs: result.preparationMs(),
                            packingMs: result.packingMs(),
                            heapCopyMs,
                        },
                        statistics: {
                            nodes: result.nodeCount(),
                            meshes: result.meshCount(),
                            instances: result.instanceCount(),
                            materials: result.materialCount(),
                            vertices: result.vertexCount(),
                            triangles: result.triangleCount(),
                            commandBytes: result.commandSize(),
                            dataBytes: result.dataSize(),
                        },
                        missingAssets: module.getUnresolvedAssets().split(',').filter(Boolean),
                    },
                },
                [commands.buffer, data.buffer],
            );
        } finally {
            result.delete();
        }
    } finally {
        for (const path of written.reverse()) {
            try {
                module.FS.unlink(path);
            } catch {
                // Request directories are unique.
            }
        }
        for (const path of createdDirectories.reverse()) {
            try {
                module.FS.rmdir(path);
            } catch {
                // Only request-owned directories are considered.
            }
        }
    }
}

async function convert(message) {
    if (message.asset) {
        storedAsset = message.asset;
    }
    if (!storedAsset) {
        throw new Error('No USD asset is available for conversion.');
    }
    if (message.backend === 'direct') {
        await convertDirect(message);
        return;
    }

    const converter = await getConverter(message.backend);
    post('progress', { message: `Converting ${storedAsset.fileName}...` });

    try {
        const result = await converter.convert(storedAsset.bytes, {
            additionalFiles: storedAsset.additionalFiles,
            fileName: storedAsset.fileName,
            format: message.format,
        });
        const { data, ...metrics } = result;
        self.postMessage(
            {
                type: 'result',
                requestId: activeRequestId,
                result: { ...metrics, data },
            },
            [data.buffer],
        );
    } catch (error) {
        if (converter.aborted) {
            converters.delete(message.backend);
            converter.dispose();
        }
        throw error;
    }
}

async function handle(message) {
    activeRequestId = message.requestId;
    try {
        if (message.type === 'initialize') {
            await initialize(message);
        } else if (message.type === 'convert') {
            await convert(message);
        } else {
            throw new Error(`Unknown worker request: ${message.type}`);
        }
    } catch (error) {
        post('error', {
            error: {
                message: error instanceof Error ? error.message : String(error),
                name: error instanceof Error ? error.name : 'Error',
                stack: error instanceof Error ? error.stack : undefined,
            },
        });
    }
}

self.addEventListener('message', (event) => {
    queue = queue.then(() => handle(event.data));
});
