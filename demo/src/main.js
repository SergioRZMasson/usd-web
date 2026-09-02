/**
 * usd-web demo — convert USD in the browser, render it with Babylon.js.
 *
 * Flow: USD bytes -> independently-linked GLB or .babylon wasm exporter -> Babylon.js.
 */

// Deep imports rather than the "@babylonjs/core" barrel: the barrel defeats tree shaking
// and roughly doubles the bundle, which matters because docs/ is committed.
import { Engine } from '@babylonjs/core/Engines/engine.js';
import { Scene } from '@babylonjs/core/scene.js';
import { ArcRotateCamera } from '@babylonjs/core/Cameras/arcRotateCamera.js';
import { HemisphericLight } from '@babylonjs/core/Lights/hemisphericLight.js';
import { Vector3 } from '@babylonjs/core/Maths/math.vector.js';
import { Color3, Color4 } from '@babylonjs/core/Maths/math.color.js';
import { CubeTexture } from '@babylonjs/core/Materials/Textures/cubeTexture.js';
import { PBRMaterial } from '@babylonjs/core/Materials/PBR/pbrMaterial.js';
import { VertexBuffer } from '@babylonjs/core/Buffers/buffer.js';
import { LoadAssetContainerAsync } from '@babylonjs/core/Loading/sceneLoader.js';
import { SceneLoaderFlags } from '@babylonjs/core/Loading/sceneLoaderFlags.js';
import { LoadAssetContainerFromSerializedScene } from '@babylonjs/core/Loading/Plugins/babylonFileLoader.js';
import { materializeCommandBuffers } from '@openusd-wasm/babylon';

// Side-effect imports required by the two intermediate-format loaders.
import '@babylonjs/core/Helpers/sceneHelpers.js';
import '@babylonjs/core/Loading/loadingScreen.js';
import '@babylonjs/core/Meshes/transformNode.js';
import '@babylonjs/core/Meshes/instancedMesh.js';
import '@babylonjs/core/Materials/PBR/pbrMetallicRoughnessMaterial.js';
import '@babylonjs/loaders/glTF/index.js';

SceneLoaderFlags.loggingLevel = 0;

const els = {
    file: document.getElementById('file'),
    folder: document.getElementById('folder'),
    intermediate: document.getElementById('intermediate'),
    samples: document.getElementById('samples'),
    status: document.getElementById('status'),
    log: document.getElementById('log'),
    stats: document.getElementById('stats'),
    canvas: document.getElementById('canvas'),
    drop: document.getElementById('drop'),
    loading: document.getElementById('loading'),
    loadingMessage: document.getElementById('loadingMessage'),
};
const requestedFormat = new URLSearchParams(location.search).get('format');
if (requestedFormat === 'glb' || requestedFormat === 'babylon' || requestedFormat === 'direct') {
    els.intermediate.value = requestedFormat;
}

const backendForFormat = (format) =>
    format === 'direct' ? 'direct' : format === 'babylon' ? 'babylon' : 'gltf';
const labelForBackend = (backend) =>
    backend === 'direct' ? 'Direct' : backend === 'gltf' ? 'GLB' : '.babylon';

// --- logging ---------------------------------------------------------------

function log(message, level = 'info') {
    const line = document.createElement('div');
    line.className = `line ${level}`;
    line.textContent = message;
    els.log.append(line);
    els.log.scrollTop = els.log.scrollHeight;
}

function setStatus(text, busy = false) {
    els.status.textContent = text;
    els.status.classList.toggle('busy', busy);
}

function showLoading(message) {
    els.loadingMessage.textContent = message;
    els.loading.setAttribute('aria-busy', 'true');
    els.loading.classList.add('active');
}

function updateLoading(message) {
    els.loadingMessage.textContent = message;
}

function hideLoading() {
    els.loading.classList.remove('active');
    els.loading.setAttribute('aria-busy', 'false');
}

const formatBytes = (n) =>
    n < 1024 ? `${n} B` : n < 1048576 ? `${(n / 1024).toFixed(1)} KB` : `${(n / 1048576).toFixed(2)} MB`;

// --- Babylon setup ---------------------------------------------------------

const engine = new Engine(els.canvas, true, { preserveDrawingBuffer: true, stencil: true });
const scene = new Scene(engine);
scene.clearColor = new Color4(0.09, 0.09, 0.11, 1);

const camera = new ArcRotateCamera('camera', -Math.PI / 2.2, Math.PI / 2.8, 6, Vector3.Zero(), scene);
camera.attachControl(els.canvas, true);
camera.wheelDeltaPercentage = 0.02;
camera.pinchDeltaPercentage = 0.02;
camera.minZ = 0.01;
// Auto-rotation is disabled by default: it fights with framing and makes screenshots
// non-deterministic. Toggle it from the UI instead.
camera.useAutoRotationBehavior = false;

// A fill light so untextured geometry is still legible; PBR materials additionally use
// the environment texture below.
const fill = new HemisphericLight('fill', new Vector3(0.3, 1, 0.2), scene);
fill.intensity = 0.9;

// Image-based lighting, so materials converted from USD (which are PBR) are lit
// plausibly rather than appearing flat black.
scene.environmentTexture = CubeTexture.CreateFromPrefilteredData(
    'https://assets.babylonjs.com/environments/environmentSpecular.env',
    scene,
);
scene.environmentIntensity = 1.0;

engine.runRenderLoop(() => scene.render());
addEventListener('resize', () => engine.resize());

// Exposed for debugging from the console / automated checks.
globalThis.__demo = { engine, scene, camera, getLoaded: () => loadedContainer };

// Everything the current asset added to the scene. Holding it in a container is what makes
// unloading exact: disposing it removes precisely the meshes, materials, textures, skeletons,
// animation groups and any lights or cameras the asset brought with it, and nothing else. The
// demo's own camera, fill light and environment texture are never part of it and so survive.
let loadedContainer = null;

/** Releases the previously loaded asset, leaving the demo's own camera and lighting intact. */
function disposeLoadedAsset() {
    if (!loadedContainer) return;

    // Animation groups keep running against targets that are about to disappear.
    for (const group of loadedContainer.animationGroups) group.stop();

    // A camera or light inside the asset can have been made active. Hand control back to the
    // demo's own camera first, because disposing the active camera detaches its controls.
    if (loadedContainer.cameras.includes(scene.activeCamera)) {
        scene.activeCamera = camera;
        camera.attachControl(els.canvas, true);
    }

    // Materials created here rather than by the loader are pushed into the container when
    // they are made, so disposing it releases them too.
    loadedContainer.dispose();
    loadedContainer = null;
}

/**
 * Adobe's exporter only writes a glTF material when the USD prim has a `material:binding`.
 * Meshes carrying just `primvars:displayColor` arrive with a COLOR_0 attribute and no
 * material, so Babylon assigns its default — which ignores vertex colours and shows a
 * white, fully-reflective surface instead of the authored colour.
 *
 * This gives those meshes a PBR material that actually consumes COLOR_0.
 * @param {import('@babylonjs/core').AssetContainer} container the asset that was just loaded
 * @param {import('@babylonjs/core').AbstractMesh[]} meshes the meshes to inspect
 */
function applyVertexColorMaterials(container, meshes) {
    for (const mesh of meshes) {
        if (mesh.getTotalVertices() === 0) continue;
        if (!mesh.isVerticesDataPresent(VertexBuffer.ColorKind)) continue;

        const existing = mesh.material;
        // Only replace the loader's default; a real converted material is left alone.
        if (existing && existing.name && existing.name !== '__GLTFLoader._default') continue;

        const material = new PBRMaterial(`vertexColor_${mesh.name}`, scene);
        material.albedoColor = Color3.White();
        material.metallic = 0;
        material.roughness = 0.75;
        material.useVertexColors = true;
        material.backFaceCulling = false;
        mesh.material = material;
        mesh.useVertexColors = true;
        // Hand ownership to the container so this material is released with the asset
        // rather than leaking one per mesh on every load.
        container.materials.push(material);
    }
}

/**
 * Frames the camera on the given meshes.
 * @param {import('@babylonjs/core').AbstractMesh[]} meshes the meshes to fit in view
 */
function frameScene(meshes) {
    if (meshes.length === 0) return;

    let min = null;
    let max = null;
    for (const mesh of meshes) {
        mesh.computeWorldMatrix(true);
        const { minimumWorld, maximumWorld } = mesh.getBoundingInfo().boundingBox;
        min = min ? Vector3.Minimize(min, minimumWorld) : minimumWorld.clone();
        max = max ? Vector3.Maximize(max, maximumWorld) : maximumWorld.clone();
    }

    const size = max.subtract(min);
    const center = min.add(max).scale(0.5);
    // Fit the bounding sphere to the narrower of the two frustum angles, so the model
    // fills the viewport on both axes regardless of canvas aspect ratio.
    const boundingRadius = Math.max(size.length() * 0.5, 1e-4);
    const aspect = engine.getAspectRatio(camera);
    const vertical = camera.fov;
    const horizontal = 2 * Math.atan(Math.tan(vertical / 2) * aspect);
    const fitting = Math.min(vertical, horizontal);

    camera.setTarget(center);
    camera.radius = (boundingRadius / Math.sin(fitting / 2)) * 1.15;
    camera.lowerRadiusLimit = boundingRadius * 0.05;
    camera.upperRadiusLimit = boundingRadius * 40;
    camera.minZ = boundingRadius / 500;
    camera.maxZ = boundingRadius * 200;
    camera.wheelPrecision = 60 / boundingRadius;

    // Reset to a consistent three-quarter view; auto-rotation otherwise leaves the
    // camera wherever the previous model left it, which can be edge-on.
    camera.alpha = -Math.PI / 2.2;
    camera.beta = Math.PI / 3;
}

// --- converter -------------------------------------------------------------

const initialBackend = backendForFormat(els.intermediate.value);
let conversionWorker = null;
let lastLoad = null;
const workerRequests = new Map();
let nextWorkerRequestId = 1;

function handleWorkerMessage(event) {
    const message = event.data;
    const request = workerRequests.get(message.requestId);
    if (!request) return;

    if (message.type === 'progress') {
        updateLoading(message.message);
        return;
    }
    if (message.type === 'log') {
        if (message.entry.level !== 'info') {
            log(message.entry.message, message.entry.level);
        }
        return;
    }

    workerRequests.delete(message.requestId);
    if (message.type === 'error') {
        const error = new Error(message.error.message);
        error.name = message.error.name;
        error.stack = message.error.stack;
        request.reject(error);
    } else {
        request.resolve(message);
    }
}

function handleWorkerFailure(event) {
    const error = event.error ?? new Error(event.message || 'Conversion worker failed.');
    for (const request of workerRequests.values()) {
        request.reject(error);
    }
    workerRequests.clear();
    conversionWorker?.terminate();
    conversionWorker = null;
    lastLoad = null;
    hideLoading();
    setBusy(false);
    setStatus('Conversion worker stopped. Select the asset again.');
    log(error.message, 'error');
}

function ensureWorker() {
    if (conversionWorker) return conversionWorker;

    conversionWorker = new Worker(new URL('./conversionWorker.js', import.meta.url), {
        type: 'module',
    });
    conversionWorker.addEventListener('message', handleWorkerMessage);
    conversionWorker.addEventListener('error', handleWorkerFailure);
    conversionWorker.addEventListener('messageerror', handleWorkerFailure);
    return conversionWorker;
}

addEventListener('beforeunload', () => conversionWorker?.terminate());

function requestWorker(type, payload, transfer = []) {
    const requestId = nextWorkerRequestId++;
    return new Promise((resolve, reject) => {
        workerRequests.set(requestId, { resolve, reject });
        try {
            ensureWorker().postMessage({ type, requestId, ...payload }, transfer);
        } catch (error) {
            workerRequests.delete(requestId);
            reject(error);
        }
    });
}

async function initializeBackend(backend) {
    const { info } = await requestWorker('initialize', { backend });
    const label = labelForBackend(backend);
    log(`${label} worker: OpenUSD ${info.usdVersion}; formats ${info.supportedOutputFormats.join(', ')}`);
    return info;
}

setStatus(`Loading ${labelForBackend(initialBackend)} exporter worker…`, true);
showLoading('Starting conversion worker...');

try {
    const info = await initializeBackend(initialBackend);
    setStatus(`Ready — OpenUSD ${info.usdVersion}`);
    log(`Asset resolver: ${info.resolver}`);
    hideLoading();
    setBusy(false);
} catch (error) {
    setStatus('Failed to load the conversion worker.');
    updateLoading('The conversion worker could not start.');
    log(String(error), 'error');
    hideLoading();
    setBusy(false);
}

// --- conversion ------------------------------------------------------------

// Loads are serialised and only the newest one is allowed to touch the scene. Without this a
// file dropped while another is still converting would interleave: the second load would clear
// the scene midway through the first one's population, leaving a half-built scene and stale
// statistics. Large assets take seconds to convert, so that window is easy to hit.
let loadGeneration = 0;
let pendingLoad = Promise.resolve();

function setBusy(busy) {
    els.file.disabled = busy;
    els.folder.disabled = busy;
    els.intermediate.disabled = busy;
    for (const button of els.samples.querySelectorAll('button')) button.disabled = busy;
}

async function loadUsd(bytes, fileName, additionalFiles = undefined) {
    const asset = {
        bytes,
        fileName,
        additionalFiles,
        sourceByteLength: bytes.byteLength,
    };
    lastLoad = { fileName, sourceByteLength: bytes.byteLength };
    const generation = ++loadGeneration;

    // Chain onto whatever is in flight so two loads never overlap, and keep the chain alive
    // if one of them fails.
    const run = pendingLoad.then(
        () => runLoad(generation, asset),
        () => runLoad(generation, asset),
    );
    pendingLoad = run.catch(() => undefined);
    return run;
}

async function reconvertLastUsd() {
    if (!lastLoad) return;
    const generation = ++loadGeneration;
    const run = pendingLoad.then(
        () => runLoad(generation, null),
        () => runLoad(generation, null),
    );
    pendingLoad = run.catch(() => undefined);
    return run;
}

async function runLoad(generation, asset) {
    // A newer request arrived while this one was queued, so this result would be thrown away
    // the moment it landed. Skip the work entirely.
    if (generation !== loadGeneration) return;

    els.log.replaceChildren();
    els.stats.replaceChildren();
    const fileName = asset?.fileName ?? lastLoad.fileName;
    const sourceByteLength = asset?.sourceByteLength ?? lastLoad.sourceByteLength;
    setStatus(`Converting ${fileName}…`, true);
    showLoading(`Preparing ${fileName}...`);
    setBusy(true);

    const intermediate = els.intermediate.value;
    const backend = backendForFormat(intermediate);
    try {
        const started = performance.now();
        const workerResult = (
            await requestWorker(
                'convert',
                {
                    backend,
                    format: intermediate,
                    asset: asset
                        ? {
                              bytes: asset.bytes,
                              fileName,
                              additionalFiles: asset.additionalFiles,
                          }
                        : undefined,
                },
                asset
                    ? [
                          ...new Set([
                              asset.bytes.buffer,
                              ...Object.values(asset.additionalFiles ?? {}).map(
                                  (file) => file.buffer,
                              ),
                          ]),
                      ]
                    : [],
            )
        ).result;
        if (asset) {
            lastLoad = {
                fileName: asset.fileName,
                sourceByteLength: asset.sourceByteLength,
            };
        }
        const convertMs = performance.now() - started;

        if (generation !== loadGeneration) return;

        let container;
        let durationMs;
        let intermediateByteLength;
        let intermediateLabel;
        let phaseRows;
        let missingAssets;
        let loadMs;

        if (workerResult.kind === 'direct') {
            const { commands, data, statistics, timings } = workerResult;
            durationMs = timings.durationMs;
            intermediateByteLength = commands.byteLength + data.byteLength;
            intermediateLabel = 'Command + data';
            missingAssets = workerResult.missingAssets;
            log(
                `Extracted ${formatBytes(commands.byteLength)} of commands and ` +
                    `${formatBytes(data.byteLength)} of raw data in ${durationMs.toFixed(0)} ms`,
            );
            updateLoading('Creating Babylon.js objects from command buffers...');
            const loadStarted = performance.now();
            const materialized = await materializeCommandBuffers(
                scene,
                commands.buffer,
                data.buffer,
                false,
            );
            container = materialized.container;
            loadMs = performance.now() - loadStarted;
            phaseRows = [
                ['OpenUSD open', `${timings.stageOpenMs.toFixed(0)} ms`],
                ['Direct stage read', `${timings.stageReadMs.toFixed(0)} ms`],
                ['Mesh preparation', `${timings.preparationMs.toFixed(0)} ms`],
                ['Command packing', `${timings.packingMs.toFixed(0)} ms`],
                ['Heap → JavaScript', `${timings.heapCopyMs.toFixed(0)} ms`],
                ['Command buffer', formatBytes(statistics.commandBytes)],
                ['Raw data buffer', formatBytes(statistics.dataBytes)],
            ];
        } else {
            const {
                data,
                exportDispatchMs,
                heapCopyMs,
                pluginReadMs,
                readbackMs,
                serializeMs,
                stageFlattenMs,
                stageOpenMs,
                transcodeMs,
            } = workerResult;
            durationMs = workerResult.durationMs;
            intermediateByteLength = data.byteLength;
            intermediateLabel = intermediate === 'babylon' ? '.babylon' : 'GLB';
            missingAssets = workerResult.missingAssets;
            log(
                `Converted to ${intermediateLabel}: ${formatBytes(data.byteLength)} in ` +
                    `${durationMs.toFixed(0)} ms`,
            );
            updateLoading(`Loading ${intermediateLabel} into Babylon.js...`);
            const loadStarted = performance.now();
            container =
                intermediate === 'babylon'
                    ? LoadAssetContainerFromSerializedScene(
                          scene,
                          new TextDecoder().decode(data),
                          '',
                      )
                    : await LoadAssetContainerAsync(data, scene, { pluginExtension: '.glb' });
            loadMs = performance.now() - loadStarted;
            phaseRows = [
                ['OpenUSD open', `${stageOpenMs.toFixed(0)} ms`],
                ['OpenUSD flatten', `${stageFlattenMs.toFixed(0)} ms`],
                ['Plugin USD read', `${pluginReadMs.toFixed(0)} ms`],
                ['Transcode / mesh prep', `${transcodeMs.toFixed(0)} ms`],
                ['Serialize', `${serializeMs.toFixed(0)} ms`],
                ['Export dispatch', `${exportDispatchMs.toFixed(0)} ms`],
                ['Wasm readback', `${readbackMs.toFixed(0)} ms`],
                ['Heap → JavaScript', `${heapCopyMs.toFixed(0)} ms`],
            ];
        }

        if (missingAssets.length > 0) {
            log(
                `${missingAssets.length} referenced file(s) were never supplied, so that ` +
                    `content is missing: ${missingAssets.join(', ')}`,
                'warning',
            );
        }

        // Only release the old asset once the new one has loaded, so a failure part-way
        // through leaves the previous scene on screen rather than an empty one.
        if (generation !== loadGeneration) {
            container.dispose();
            return;
        }

        disposeLoadedAsset();
        container.addAllToScene();
        loadedContainer = container;

        const meshes = container.meshes.filter((m) => m.getTotalVertices() > 0);
        applyVertexColorMaterials(container, meshes);
        frameScene(meshes);

        const vertices = meshes.reduce((sum, m) => sum + m.getTotalVertices(), 0);
        const triangles = meshes.reduce((sum, m) => sum + (m.getTotalIndices() / 3 || 0), 0);

        renderStats([
            ['Source', `${fileName} · ${formatBytes(sourceByteLength)}`],
            [intermediateLabel, formatBytes(intermediateByteLength)],
            ...phaseRows,
            [`USD → ${intermediateLabel}`, `${durationMs.toFixed(0)} ms`],
            [backend === 'direct' ? 'Babylon materialize' : 'Babylon load', `${loadMs.toFixed(0)} ms`],
            ['Total', `${(convertMs + loadMs).toFixed(0)} ms`],
            ['Meshes', String(meshes.length)],
            ['Vertices', vertices.toLocaleString()],
            ['Triangles', Math.round(triangles).toLocaleString()],
            ['Materials', String(container.materials.length)],
            ['Animations', String(container.animationGroups.length)],
            ['Skeletons', String(container.skeletons.length)],
        ]);

        for (const group of container.animationGroups) group.play(true);

        setStatus(`${fileName} — ${meshes.length} meshes, ${vertices.toLocaleString()} vertices`);
    } catch (error) {
        if (generation !== loadGeneration) return;

        setStatus('Conversion failed.');
        log(String(error?.message ?? error), 'error');
        console.error(error);
    } finally {
        // Leave the controls disabled if a newer load is already queued behind this one;
        // that load re-enables them when it settles.
        if (generation === loadGeneration) {
            hideLoading();
            setBusy(false);
        }
    }
}

function renderStats(rows) {
    els.stats.replaceChildren(
        ...rows.flatMap(([label, value]) => {
            const k = document.createElement('dt');
            k.textContent = label;
            const v = document.createElement('dd');
            v.textContent = value;
            return [k, v];
        }),
    );
}

els.intermediate.addEventListener('change', async () => {
    if (lastLoad) {
        await reconvertLastUsd();
    } else {
        const backend = backendForFormat(els.intermediate.value);
        const label = labelForBackend(backend);
        setStatus(`Loading ${label} exporter worker…`, true);
        showLoading(`Loading the ${label} exporter...`);
        setBusy(true);
        try {
            const info = await initializeBackend(backend);
            setStatus(`Ready — OpenUSD ${info.usdVersion}`);
        } catch (error) {
            setStatus('Failed to load the selected exporter.');
            log(String(error?.message ?? error), 'error');
        } finally {
            hideLoading();
            setBusy(false);
        }
    }
});

// --- input -----------------------------------------------------------------

/** Extensions the converter accepts as a root layer. */
const USD_EXTENSIONS = ['usd', 'usda', 'usdc', 'usdz'];

const extensionOf = (name) => name.slice(name.lastIndexOf('.') + 1).toLowerCase();
const isUsd = (name) => USD_EXTENSIONS.includes(extensionOf(name));

/**
 * Turns a set of picked files into a root layer plus its sibling assets.
 *
 * USD scenes are usually many files: the root layer references sublayers, payloads and
 * textures by relative path. Those references only resolve if the whole set is written
 * into the virtual filesystem with its directory structure intact, which is why the demo
 * accepts a folder rather than a single file.
 */
async function loadFileSet(files) {
    if (files.length === 0) return;

    // `webkitRelativePath` is populated for directory picks and preserves the tree.
    const entries = files.map((file) => ({
        file,
        path: (file.webkitRelativePath || file.name).replace(/\\/g, '/'),
    }));

    // Strip the common top-level folder so paths are relative to the asset root, which
    // is what the references inside the USD are anchored to.
    const segments = entries[0].path.split('/');
    const commonRoot = entries.length > 1 && segments.length > 1 ? `${segments[0]}/` : '';
    for (const entry of entries) {
        if (commonRoot && entry.path.startsWith(commonRoot)) {
            entry.path = entry.path.slice(commonRoot.length);
        }
    }

    const candidates = entries.filter((e) => isUsd(e.path));
    if (candidates.length === 0) {
        setStatus('No .usd/.usda/.usdc/.usdz file in that selection.');
        return;
    }

    const requestedRoot = new URLSearchParams(location.search).get('root');
    // Prefer an explicitly requested root for reproducible benchmarks, then the shallowest
    // USD file: in a published asset the root layer usually sits above its dependencies.
    candidates.sort(
        (a, b) =>
            Number(!(a.path === requestedRoot || a.path.endsWith(`/${requestedRoot}`))) -
                Number(!(b.path === requestedRoot || b.path.endsWith(`/${requestedRoot}`))) ||
            a.path.split('/').length - b.path.split('/').length ||
            a.path.localeCompare(b.path),
    );
    const root = candidates[0];

    const additionalFiles = {};
    for (const entry of entries) {
        if (entry === root) continue;
        additionalFiles[entry.path] = new Uint8Array(await entry.file.arrayBuffer());
    }

    const supporting = Object.keys(additionalFiles).length;
    if (supporting > 0) {
        log(`Staging ${supporting} supporting file${supporting === 1 ? '' : 's'} alongside ${root.path}`);
    }

    await loadUsd(new Uint8Array(await root.file.arrayBuffer()), root.path, additionalFiles);
}

/** Recursively collects files from a dropped directory entry. */
async function collectEntry(entry, prefix = '') {
    if (entry.isFile) {
        const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
        // Dropped files have no webkitRelativePath, so the traversal path is attached.
        Object.defineProperty(file, 'webkitRelativePath', {
            value: prefix + file.name,
            configurable: true,
        });
        return [file];
    }
    if (entry.isDirectory) {
        const reader = entry.createReader();
        const children = [];
        // readEntries returns at most 100 entries per call, so it is drained in a loop.
        for (;;) {
            const batch = await new Promise((resolve, reject) =>
                reader.readEntries(resolve, reject),
            );
            if (batch.length === 0) break;
            children.push(...batch);
        }
        const nested = await Promise.all(
            children.map((child) => collectEntry(child, `${prefix}${entry.name}/`)),
        );
        return nested.flat();
    }
    return [];
}

// A file input does not fire `change` when the same file is picked again, because its value
// is unchanged. Clearing it after each selection means re-picking the same asset reloads it.
async function handleInput(event) {
    const input = event.target;
    const files = [...input.files];
    input.value = '';
    await loadFileSet(files);
}

els.file.addEventListener('change', handleInput);
els.folder.addEventListener('change', handleInput);

for (const type of ['dragenter', 'dragover']) {
    document.addEventListener(type, (e) => {
        e.preventDefault();
        els.drop.classList.add('active');
    });
}
for (const type of ['dragleave', 'drop']) {
    document.addEventListener(type, (e) => {
        e.preventDefault();
        els.drop.classList.remove('active');
    });
}

document.addEventListener('drop', async (event) => {
    try {
        const items = [...(event.dataTransfer?.items ?? [])];
        const entries = items.map((item) => item.webkitGetAsEntry?.()).filter((entry) => entry != null);

        if (entries.length > 0) {
            setStatus('Reading dropped files…', true);
            const collected = await Promise.all(entries.map((entry) => collectEntry(entry)));
            await loadFileSet(collected.flat());
            return;
        }

        // Some sources hand over plain files with no filesystem entry. A dropped folder
        // arrives that way as a zero-byte placeholder, which cannot be read.
        const files = [...(event.dataTransfer?.files ?? [])].filter((file) => file.size > 0 || isUsd(file.name));
        await loadFileSet(files);
    } catch (error) {
        // Without this the controls would stay disabled and the app would look wedged.
        setStatus('Could not read the dropped files.');
        log(String(error?.message ?? error), 'error');
        console.error(error);
        setBusy(false);
    }
});

async function loadSample(name) {
    setStatus(`Fetching ${name}…`, true);
    try {
        const response = await fetch(`./assets/${name}`);
        if (!response.ok) {
            setStatus(`Could not fetch ${name}.`);
            return;
        }
        await loadUsd(new Uint8Array(await response.arrayBuffer()), name);
    } catch (error) {
        setStatus(`Could not fetch ${name}.`);
        log(String(error?.message ?? error), 'error');
        setBusy(false);
    }
}

els.samples.addEventListener('click', async (event) => {
    const name = event.target?.dataset?.sample;
    if (name) {
        await loadSample(name);
    }
});

const requestedSample = new URLSearchParams(location.search).get('sample');
if (requestedSample && [...els.samples.querySelectorAll('button')].some((button) => button.dataset.sample === requestedSample)) {
    await loadSample(requestedSample);
}
