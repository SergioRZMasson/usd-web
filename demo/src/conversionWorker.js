import { UsdConverter } from './usd-web.js';

const converters = new Map();
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

async function initialize(message) {
    const converter = await getConverter(message.backend);
    post('initialized', { info: converter.info });
}

async function convert(message) {
    if (message.asset) {
        storedAsset = message.asset;
    }
    if (!storedAsset) {
        throw new Error('No USD asset is available for conversion.');
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
