/**
 * usd-web demo — convert USD in the browser, render it with Babylon.js.
 *
 * Flow: USD bytes -> (wasm: OpenUSD + Adobe usdGltf plugin) -> GLB bytes -> Babylon.
 * The GLB is handed to Babylon as an ArrayBufferView, so no blob URL is involved.
 */

import { UsdConverter } from 'usd-web-gltf';

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

// Side-effect imports: the scene helpers back createDefaultSkybox, and the glTF registration
// is what lets AppendSceneAsync recognise the '.glb' the converter produces.
import '@babylonjs/core/Helpers/sceneHelpers.js';
import '@babylonjs/core/Loading/loadingScreen.js';
import '@babylonjs/loaders/glTF/index.js';

const els = {
    file: document.getElementById('file'),
    folder: document.getElementById('folder'),
    samples: document.getElementById('samples'),
    status: document.getElementById('status'),
    log: document.getElementById('log'),
    stats: document.getElementById('stats'),
    canvas: document.getElementById('canvas'),
    drop: document.getElementById('drop'),
};

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

setStatus('Loading OpenUSD (WebAssembly)…', true);

let converter;
try {
    converter = await UsdConverter.create({
        onLog: (m) => m.level !== 'info' && log(m.message, m.level),
    });
} catch (error) {
    setStatus('Failed to load the WebAssembly module.');
    log(String(error), 'error');
    if (!globalThis.crossOriginIsolated) {
        log(
            'crossOriginIsolated is false — the page needs COOP/COEP headers. ' +
                'Use `npm start`, which serves them.',
            'error',
        );
    }
    throw error;
}

const info = converter.info;
setStatus(`Ready — OpenUSD ${info.usdVersion}`);
log(`OpenUSD ${info.usdVersion} loaded. Writable formats: ${info.supportedOutputFormats.join(', ')}`);
log(`Adobe usdGltf plugin: ${info.gltfPluginAvailable ? 'registered' : 'MISSING'}`);
log(`Asset resolver: ${info.resolver}`);
setBusy(false);

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
    for (const button of els.samples.querySelectorAll('button')) button.disabled = busy;
}

async function loadUsd(bytes, fileName, additionalFiles = undefined) {
    const generation = ++loadGeneration;

    // Chain onto whatever is in flight so two loads never overlap, and keep the chain alive
    // if one of them fails.
    const run = pendingLoad.then(
        () => runLoad(generation, bytes, fileName, additionalFiles),
        () => runLoad(generation, bytes, fileName, additionalFiles),
    );
    pendingLoad = run.catch(() => undefined);
    return run;
}

async function runLoad(generation, bytes, fileName, additionalFiles) {
    // A newer request arrived while this one was queued, so this result would be thrown away
    // the moment it landed. Skip the work entirely.
    if (generation !== loadGeneration) return;

    els.log.replaceChildren();
    els.stats.replaceChildren();
    setStatus(`Converting ${fileName}…`, true);
    setBusy(true);

    try {
        const started = performance.now();
        const { data, durationMs, missingAssets } = await converter.convert(bytes, {
            fileName,
            additionalFiles,
        });
        const convertMs = performance.now() - started;

        if (generation !== loadGeneration) return;

        log(`Converted to GLB: ${formatBytes(data.byteLength)} in ${durationMs.toFixed(0)} ms`);

        if (missingAssets.length > 0) {
            log(
                `${missingAssets.length} referenced file(s) were never supplied, so that ` +
                    `content is missing: ${missingAssets.join(', ')}`,
                'warning',
            );
        }

        const loadStarted = performance.now();
        // Load into a container rather than straight into the scene: the container is the
        // record of what this asset added, which is what makes unloading it exact. Babylon
        // accepts an ArrayBufferView directly; pluginExtension tells it which loader to use,
        // since there is no filename to infer from.
        const container = await LoadAssetContainerAsync(data, scene, { pluginExtension: '.glb' });

        // Only release the old asset once the new one has loaded, so a failure part-way
        // through leaves the previous scene on screen rather than an empty one.
        if (generation !== loadGeneration) {
            container.dispose();
            return;
        }

        disposeLoadedAsset();
        container.addAllToScene();
        loadedContainer = container;
        const loadMs = performance.now() - loadStarted;

        const meshes = container.meshes.filter((m) => m.getTotalVertices() > 0);
        applyVertexColorMaterials(container, meshes);
        frameScene(meshes);

        const vertices = meshes.reduce((sum, m) => sum + m.getTotalVertices(), 0);
        const triangles = meshes.reduce((sum, m) => sum + (m.getTotalIndices() / 3 || 0), 0);

        renderStats([
            ['Source', `${fileName} · ${formatBytes(bytes.byteLength)}`],
            ['GLB', formatBytes(data.byteLength)],
            ['USD → GLB', `${durationMs.toFixed(0)} ms`],
            ['Babylon load', `${loadMs.toFixed(0)} ms`],
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

        // OpenUSD treats some malformed input as fatal, which terminates the wasm
        // runtime. The instance is unusable afterwards, so a fresh one is created.
        if (converter.aborted) {
            log('The WebAssembly runtime aborted. Reloading the module…', 'warning');
            setStatus('Reloading module…', true);
            try {
                converter = await UsdConverter.create({
                    onLog: (m) => m.level !== 'info' && log(m.message, m.level),
                });
                setStatus(`Ready — OpenUSD ${converter.info.usdVersion}`);
            } catch (reloadError) {
                setStatus('The WebAssembly module could not be reloaded. Refresh the page.');
                log(String(reloadError?.message ?? reloadError), 'error');
            }
        }
    } finally {
        // Leave the controls disabled if a newer load is already queued behind this one;
        // that load re-enables them when it settles.
        if (generation === loadGeneration) setBusy(false);
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

    // Prefer the shallowest USD file: in a published asset the root layer sits at the
    // top and its dependencies live in subdirectories.
    candidates.sort(
        (a, b) => a.path.split('/').length - b.path.split('/').length || a.path.localeCompare(b.path),
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

els.samples.addEventListener('click', async (event) => {
    const name = event.target?.dataset?.sample;
    if (!name) return;
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
});
