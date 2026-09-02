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
if (!['gltf', 'babylon'].includes(backend) || !rootFile || !assetDirectory) {
    console.error('Usage: node benchmark-wasm-formats.mjs <gltf|babylon> <root.usd> <asset-dir>');
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
const result = Module.convertWithOptions(input, output, true, true, true);
try {
    if (!result.ok()) {
        throw new Error(result.error());
    }
    console.log(
        JSON.stringify({
            backend,
            instantiateMs,
            conversionMs: result.durationMs(),
            outputBytes: result.dataSize(),
            stagedFiles: files.length,
            gltfPlugin: Module.isGltfPluginAvailable(),
            babylonPlugin: Module.isBabylonPluginAvailable(),
        }),
    );
} finally {
    result.delete();
}
