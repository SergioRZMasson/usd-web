// Exercises the Babylon.js USD file loader end to end.
//
// Nothing here calls the converter directly: a .usda is handed to Babylon's SceneLoader,
// which routes it to the USD plugin, which converts to GLB in WebAssembly and delegates to
// the glTF loader. If this renders, the plugin registration and delegation both work.

import { Engine } from "core/Engines/engine";
import { Scene } from "core/scene";
import { ArcRotateCamera } from "core/Cameras/arcRotateCamera";
import { HemisphericLight } from "core/Lights/hemisphericLight";
import { Vector3 } from "core/Maths/math.vector";
import { Color4 } from "core/Maths/math.color";
import { AppendSceneAsync, LoadAssetContainerAsync } from "core/Loading/sceneLoader";

// Side-effect import required whenever SceneLoader is driven from a URL.
import "core/Loading/loadingScreen";

// Side-effect import: registers the USD plugin (and the glTF plugin it delegates to).
import { USDConverter } from "loaders/USD/usdFileLoader";

const statusEl = document.getElementById("status");
const logEl = document.getElementById("log");

function log(message, level = "info") {
    const line = document.createElement("div");
    line.className = level;
    line.textContent = message;
    logEl.append(line);
    logEl.scrollTop = logEl.scrollHeight;
    // Surfaced for the automated check.
    (globalThis.__log ??= []).push(`${level}: ${message}`);
}

// Point the converter at the locally built artifacts instead of the CDN.
USDConverter.DefaultConfiguration = {
    wasmUrl: new URL("./wasm/usd-web-gltf.js", location.href).href,
    wasmBinaryUrl: new URL("./wasm/usd-web-gltf.wasm", location.href).href,
    dataUrl: new URL("./wasm/usd-web-gltf.data", location.href).href,
};

const engine = new Engine(document.getElementById("canvas"), true);
const scene = new Scene(engine);
scene.clearColor = new Color4(0.09, 0.09, 0.11, 1);

const camera = new ArcRotateCamera("camera", -Math.PI / 2.2, Math.PI / 3, 8, Vector3.Zero(), scene);
camera.attachControl(true);
new HemisphericLight("light", new Vector3(0.3, 1, 0.2), scene);

engine.runRenderLoop(() => scene.render());
addEventListener("resize", () => engine.resize());

globalThis.__demo = { engine, scene, camera };

/** Removes previously loaded content, leaving camera and light intact. */
function clearScene() {
    for (const mesh of [...scene.meshes]) mesh.dispose(false, true);
    for (const node of [...scene.transformNodes]) node.dispose();
    for (const material of [...scene.materials]) material.dispose(true, true);
}

async function loadSample(name) {
    clearScene();
    statusEl.textContent = `Loading ${name} through Babylon's SceneLoader…`;
    log(`--- ${name} ---`);

    try {
        const started = performance.now();
        // The extension routes to the USD plugin; no converter call appears here.
        await AppendSceneAsync(`./assets/${name}`, scene);
        const elapsed = performance.now() - started;

        const meshes = scene.meshes.filter((m) => m.getTotalVertices() > 0);
        const vertices = meshes.reduce((sum, m) => sum + m.getTotalVertices(), 0);

        camera.setTarget(Vector3.Zero());
        log(`loaded in ${elapsed.toFixed(0)} ms: ${meshes.length} meshes, ${vertices} vertices, ${scene.materials.length} materials`);
        statusEl.textContent = `${name} — ${meshes.length} meshes, ${vertices} vertices`;

        globalThis.__result = { name, meshes: meshes.length, vertices, materials: scene.materials.length };
    } catch (error) {
        statusEl.textContent = "Load failed.";
        log(String(error?.message ?? error), "error");
        globalThis.__result = { name, error: String(error?.message ?? error) };
        throw error;
    }
}

/** Also exercises the asset-container path, which is a separate plugin entry point. */
async function loadIntoContainer(name) {
    log(`--- ${name} (asset container) ---`);
    const container = await LoadAssetContainerAsync(`./assets/${name}`, scene);
    log(`container: ${container.meshes.length} meshes, ${container.materials.length} materials`);
    globalThis.__containerResult = {
        meshes: container.meshes.length,
        materials: container.materials.length,
    };
    container.dispose();
}

statusEl.textContent = "Ready.";
log(`OpenUSD ${await USDConverter.Default.getUsdVersionAsync()} via Babylon USD loader`);

for (const button of document.querySelectorAll("[data-sample]")) {
    button.addEventListener("click", () => loadSample(button.dataset.sample));
}
document.getElementById("container").addEventListener("click", () => loadIntoContainer("materials.usda"));

globalThis.__loadSample = loadSample;
globalThis.__loadIntoContainer = loadIntoContainer;
