// Benchmarks one independently linked wasm exporter against a local USD asset directory.
//
// Usage:
//   node test/benchmark-wasm-formats.mjs gltf /path/to/root.usd /path/to/assets
//   node test/benchmark-wasm-formats.mjs babylon /path/to/root.usd /path/to/assets

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const backend = process.argv[2];
const rootFile = process.argv[3] && resolve(process.argv[3]);
const assetDirectory = process.argv[4] && resolve(process.argv[4]);
const iterations = Number(process.argv[5] ?? 3);
if (
    !['gltf', 'babylon'].includes(backend) ||
    !rootFile ||
    !assetDirectory ||
    !Number.isInteger(iterations) ||
    iterations < 1
) {
    console.error(
        'Usage: node benchmark-wasm-formats.mjs <gltf|babylon> <root.usd> <asset-dir> [iterations]',
    );
    process.exit(1);
}

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const binDirectory = resolve(projectRoot, 'build', 'wasm', 'bin');
const moduleName = backend === 'gltf' ? 'usd-web-gltf.js' : 'usd-web-babylon.js';
const { default: createModule } = await import(pathToFileURL(join(binDirectory, moduleName)));

// Emscripten's Node data-package loader resolves the .data file against cwd.
process.chdir(binDirectory);
const instantiateStarted = performance.now();
const Module = await createModule();
const instantiateMs = performance.now() - instantiateStarted;
Module.setLogCallback(() => {});

const madeDirectories = new Set(['/work']);
Module.FS.mkdir('/work');
const ensureDirectory = (path) => {
    const parts = path.split('/').filter(Boolean);
    let current = '';
    for (const part of parts) {
        current += `/${part}`;
        if (!madeDirectories.has(current)) {
            Module.FS.mkdir(current);
            madeDirectories.add(current);
        }
    }
};

const files = [];
const walk = (directory) => {
    for (const name of readdirSync(directory)) {
        const path = join(directory, name);
        if (statSync(path).isDirectory()) {
            walk(path);
        } else {
            files.push(path);
        }
    }
};
walk(assetDirectory);

for (const file of files) {
    const relativePath = relative(assetDirectory, file).split(sep).join('/');
    const target = `/work/${relativePath}`;
    ensureDirectory(dirname(target));
    Module.FS.writeFile(target, new Uint8Array(readFileSync(file)));
}
Module.registerAssetDirectory('/work');

const input = `/work/${relative(assetDirectory, rootFile).split(sep).join('/')}`;
const extension = backend === 'gltf' ? 'glb' : 'babylon';
const output = `/work/output.${extension}`;
const parseGltfStats = (payload) => {
        const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
        const jsonLength = view.getUint32(12, true);
        const json = JSON.parse(
            new TextDecoder().decode(payload.subarray(20, 20 + jsonLength)),
        );
        const positions = new Set();
        let triangles = 0;
        let primitives = 0;
        for (const mesh of json.meshes ?? []) {
            for (const primitive of mesh.primitives ?? []) {
                primitives++;
                positions.add(primitive.attributes.POSITION);
                triangles += json.accessors[primitive.indices].count / 3;
            }
        }
        return {
            meshes: json.meshes?.length ?? 0,
            primitives,
            vertices: [...positions].reduce(
                (total, accessor) => total + json.accessors[accessor].count,
                0,
            ),
            triangles,
        };
};
const parseBabylonStats = (payload) => {
        const json = JSON.parse(new TextDecoder().decode(payload));
        const geometries = json.geometries?.vertexData ?? [];
        return {
            meshes: json.meshes?.length ?? 0,
            instances: (json.meshes ?? []).reduce(
                (total, mesh) => total + (mesh.instances?.length ?? 0),
                0,
            ),
            vertices: geometries.reduce(
                (total, geometry) => total + (geometry.positions?.length ?? 0) / 3,
                0,
            ),
            triangles: geometries.reduce(
                (total, geometry) => total + (geometry.indices?.length ?? 0) / 3,
                0,
            ),
        };
};

const samples = [];
let payload;
let outputBytes = 0;
for (let iteration = -1; iteration < iterations; ++iteration) {
    const result = Module.convertWithOptions(input, output, true, true, true);
    try {
        if (!result.ok()) {
            throw new Error(result.error());
        }
        const heapCopyStarted = performance.now();
        const copiedPayload = Module.HEAPU8.slice(
            result.dataPtr(),
            result.dataPtr() + result.dataSize(),
        );
        const heapCopyMs = performance.now() - heapCopyStarted;
        if (iteration >= 0) {
            samples.push({
                conversionMs: result.durationMs() + heapCopyMs,
                nativeDurationMs: result.durationMs(),
                stageOpenMs: result.stageOpenMs(),
                stageFlattenMs: result.stageFlattenMs(),
                exportDispatchMs: result.exportDispatchMs(),
                pluginReadMs: result.pluginReadMs(),
                transcodeMs: result.transcodeMs(),
                serializeMs: result.serializeMs(),
                readbackMs: result.readbackMs(),
                heapCopyMs,
            });
        }
        if (iteration === iterations - 1) {
            outputBytes = result.dataSize();
            payload = copiedPayload;
        }
    } finally {
        result.delete();
    }
}

const average = (name) =>
    samples.reduce((total, sample) => total + sample[name], 0) / samples.length;
console.log(
    JSON.stringify({
        backend,
        iterations,
        instantiateMs,
        conversionMs: average('conversionMs'),
        nativeDurationMs: average('nativeDurationMs'),
        stageOpenMs: average('stageOpenMs'),
        stageFlattenMs: average('stageFlattenMs'),
        exportDispatchMs: average('exportDispatchMs'),
        pluginReadMs: average('pluginReadMs'),
        transcodeMs: average('transcodeMs'),
        serializeMs: average('serializeMs'),
        readbackMs: average('readbackMs'),
        heapCopyMs: average('heapCopyMs'),
        outputBytes,
        stagedFiles: files.length,
        gltfPlugin: Module.isGltfPluginAvailable(),
        babylonPlugin: Module.isBabylonPluginAvailable(),
        ...(backend === 'gltf' ? parseGltfStats(payload) : parseBabylonStats(payload)),
    }),
);
