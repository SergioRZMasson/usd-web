// Exercises the public API the way the demo does when a folder is picked: a root layer
// plus its sibling files, including a payload with an absolute path from another machine.

import { UsdConverter } from '../dist/index.js';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const assets = resolve(here, '../../demo/assets/multifile');

const converter = await UsdConverter.create();
console.log('resolver:', converter.info.resolver);

const result = await converter.convert(readFileSync(resolve(assets, 'scene.usda')), {
    fileName: 'scene.usda',
    // Keyed by the path the root layer references, exactly as the demo stages a folder.
    additionalFiles: {
        'parts/robot.usda': readFileSync(resolve(assets, 'parts/robot.usda')),
    },
});

// Count meshes so we can tell whether both payloads actually resolved.
const glb = result.data;
const view = new DataView(glb.buffer, glb.byteOffset, glb.byteLength);
let offset = 12;
let json = null;
while (offset + 8 <= glb.byteLength) {
    const len = view.getUint32(offset, true);
    const type = view.getUint32(offset + 4, true);
    if (type === 0x4e4f534a) {
        json = JSON.parse(
            new TextDecoder().decode(new Uint8Array(glb.buffer, glb.byteOffset + offset + 8, len)),
        );
    }
    offset = offset + 8 + len + ((4 - (len % 4)) % 4);
}

const meshes = json.meshes?.length ?? 0;
console.log(`meshes        : ${meshes}`);
console.log(`missingAssets : ${result.missingAssets.length ? result.missingAssets.join(', ') : '(none)'}`);
console.log('\nwarnings:');
for (const m of result.log.filter((m) => m.level !== 'info')) console.log('  -', m.message);

if (meshes !== 2) {
    console.error(`\nFAILED: expected 2 meshes (relative + absolute payload), got ${meshes}.`);
    process.exit(1);
}

// With the fallback disabled the absolute payload must be reported as missing, proving
// the option is actually honoured rather than always-on.
const strict = await converter.convert(readFileSync(resolve(assets, 'scene.usda')), {
    fileName: 'scene.usda',
    additionalFiles: {
        'parts/robot.usda': readFileSync(resolve(assets, 'parts/robot.usda')),
    },
    resolveByFileName: false,
});

console.log(`\nstrict mode missingAssets: ${strict.missingAssets.join(', ') || '(none)'}`);
if (strict.missingAssets.length === 0) {
    console.error('FAILED: strict mode should report the unresolvable absolute payload.');
    process.exit(1);
}

console.log('\nPASS: relative references resolve from additionalFiles, absolute paths are');
console.log('      recovered by name, and resolveByFileName:false restores strict behaviour.');
