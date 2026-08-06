// Verifies that Adobe's usdGltf file format plugin registers inside WebAssembly and that
// a USD stage can be exported to GLB through USD's own SdfFileFormat dispatch.

import createUsdGltfModule from '../build/bin/usd-web-gltf.js';
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));

const Module = await createUsdGltfModule();

Module.setLogCallback((level, message) => {
    const label = ['info', 'warn', 'error'][level] ?? 'info';
    if (level >= 1) console.log(`  [${label}] ${message}`);
});

console.log('OpenUSD version :', Module.getUsdVersion());
console.log('Output formats  :', Module.getSupportedOutputFormats());

const pluginOk = Module.isGltfPluginAvailable();
console.log('glTF plugin     :', pluginOk ? 'REGISTERED' : 'MISSING');

if (!pluginOk) {
    console.error('\nFAILED: the usdGltf plugin did not register.');
    process.exit(1);
}

const input = readFileSync(resolve(here, 'cube.usda'));
Module.FS.mkdir('/work');
Module.FS.writeFile('/work/cube.usda', new Uint8Array(input));

console.log('\nConverting cube.usda -> cube.glb ...');
const result = Module.convert('/work/cube.usda', '/work/cube.glb');

try {
    if (!result.ok()) {
        console.error('FAILED:', result.error());
        process.exit(1);
    }

    const ptr = result.dataPtr();
    const size = result.dataSize();
    const glb = Module.HEAPU8.slice(ptr, ptr + size);

    // Validate the GLB container header: magic 'glTF', version 2, and a length that
    // matches the payload we got back.
    const view = new DataView(glb.buffer, glb.byteOffset, glb.byteLength);
    const magic = view.getUint32(0, true);
    const version = view.getUint32(4, true);
    const declaredLength = view.getUint32(8, true);

    console.log(`  bytes      : ${size}`);
    console.log(`  duration   : ${result.durationMs().toFixed(1)} ms`);
    console.log(`  magic      : 0x${magic.toString(16)} (expect 0x46546c67)`);
    console.log(`  version    : ${version}`);
    console.log(`  length     : ${declaredLength}`);

    if (magic !== 0x46546c67) {
        console.error('FAILED: output is not a GLB container.');
        process.exit(1);
    }
    if (version !== 2) {
        console.error(`FAILED: expected glTF 2.0, got version ${version}.`);
        process.exit(1);
    }
    if (declaredLength !== size) {
        console.error(`FAILED: header length ${declaredLength} != payload ${size}.`);
        process.exit(1);
    }

    writeFileSync(resolve(here, 'cube.glb'), glb);
    console.log('\nPASS: Adobe usdGltf plugin converted USD to GLB inside WebAssembly.');
} finally {
    result.delete();
}
