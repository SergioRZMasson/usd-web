// End-to-end test of the published package surface: imports the built dist bundle,
// converts a USD asset and validates the resulting GLB.

import { UsdConverter } from '../dist/index.js';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));

const converter = await UsdConverter.create();

console.log('info:', JSON.stringify(converter.info, null, 2));

const bytes = readFileSync(resolve(here, '../../test/cube.usda'));

const result = await converter.convert(bytes, { fileName: 'cube.usda' });

console.log(`\nfileName   : ${result.fileName}`);
console.log(`bytes      : ${result.data.byteLength}`);
console.log(`durationMs : ${result.durationMs.toFixed(1)}`);
console.log(`log entries: ${result.log.length}`);

const view = new DataView(result.data.buffer, result.data.byteOffset, result.data.byteLength);
if (view.getUint32(0, true) !== 0x46546c67) {
    console.error('FAILED: not a GLB.');
    process.exit(1);
}
if (result.fileName !== 'cube.glb') {
    console.error(`FAILED: expected cube.glb, got ${result.fileName}`);
    process.exit(1);
}

// A second conversion must work on the same instance: this exercises the scratch-file
// cleanup and the serialisation queue.
const again = await converter.convert(bytes, { fileName: 'cube.usda' });
if (again.data.byteLength !== result.data.byteLength) {
    console.error('FAILED: repeat conversion produced a different result.');
    process.exit(1);
}

// Unsupported extensions must be rejected before reaching the native layer.
let rejected = false;
try {
    await converter.convert(bytes, { fileName: 'model.fbx' });
} catch (error) {
    rejected = error?.name === 'InvalidInputError';
}
if (!rejected) {
    console.error('FAILED: .fbx should have been rejected with InvalidInputError.');
    process.exit(1);
}

console.log('\nPASS: package API converts USD to GLB, is reusable, and validates input.');
