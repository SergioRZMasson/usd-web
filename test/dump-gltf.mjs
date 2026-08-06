// Diagnostic: dumps the glTF JSON produced for a sample so material and attribute
// handling can be inspected without a browser.

import { UsdConverter } from '../js/dist/index.js';
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve, basename } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const input = process.argv[2] ?? resolve(here, '../demo/assets/cube.usda');

const converter = await UsdConverter.create();
const bytes = readFileSync(input);
const result = await converter.convert(bytes, { fileName: basename(input) });

const glb = result.data;
const dv = new DataView(glb.buffer, glb.byteOffset, glb.byteLength);
let offset = 12;
let json = null;
while (offset + 8 <= glb.byteLength) {
    const len = dv.getUint32(offset, true);
    const type = dv.getUint32(offset + 4, true);
    if (type === 0x4e4f534a) {
        json = JSON.parse(
            new TextDecoder().decode(new Uint8Array(glb.buffer, glb.byteOffset + offset + 8, len)),
        );
    }
    offset = offset + 8 + len + ((4 - (len % 4)) % 4);
}

const out = resolve(here, `${basename(input, '.usda')}.glb`);
writeFileSync(out, glb);

console.log(`input : ${input}`);
console.log(`output: ${out} (${glb.byteLength} bytes)\n`);
console.log('materials:', JSON.stringify(json.materials ?? [], null, 2));
console.log(
    '\nmeshes:',
    JSON.stringify(
        (json.meshes ?? []).map((m) => ({
            name: m.name,
            primitives: m.primitives.map((p) => ({
                attributes: Object.keys(p.attributes),
                material: p.material,
                mode: p.mode,
                indexCount: json.accessors[p.indices]?.count,
                vertexCount: json.accessors[p.attributes.POSITION]?.count,
            })),
        })),
        null,
        2,
    ),
);
console.log('\nnodes:', JSON.stringify(json.nodes ?? [], null, 2));
console.log(
    '\naccessors:',
    JSON.stringify(
        (json.accessors ?? []).map((a, i) => ({
            i,
            type: a.type,
            componentType: a.componentType,
            count: a.count,
            min: a.min,
            max: a.max,
        })),
        null,
        1,
    ),
);
