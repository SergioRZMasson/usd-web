// Parses the produced GLB and reports its contents, so we can confirm the conversion
// carried real geometry rather than just emitting a well-formed empty container.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const glb = readFileSync(resolve(here, 'cube.glb'));

const view = new DataView(glb.buffer, glb.byteOffset, glb.byteLength);

if (view.getUint32(0, true) !== 0x46546c67) {
    console.error('Not a GLB file.');
    process.exit(1);
}

// Walk the chunk table to find the JSON chunk (type 0x4e4f534a).
// Each chunk is an 8-byte header (uint32 length + uint32 type) followed by its
// 4-byte-aligned payload.
let offset = 12;
let json = null;
let binLength = 0;
while (offset + 8 <= glb.byteLength) {
    const chunkLength = view.getUint32(offset, true);
    const chunkType = view.getUint32(offset + 4, true);
    const dataStart = offset + 8;
    if (dataStart + chunkLength > glb.byteLength) {
        console.error(`Malformed chunk at ${offset}: declares ${chunkLength} bytes.`);
        process.exit(1);
    }
    const body = new Uint8Array(glb.buffer, glb.byteOffset + dataStart, chunkLength);
    if (chunkType === 0x4e4f534a) {
        json = JSON.parse(new TextDecoder().decode(body));
    } else if (chunkType === 0x004e4942) {
        binLength = chunkLength;
    }
    offset = dataStart + chunkLength + ((4 - (chunkLength % 4)) % 4);
}

if (!json) {
    console.error('GLB has no JSON chunk.');
    process.exit(1);
}

const count = (key) => (json[key] ? json[key].length : 0);

console.log('glTF asset      :', JSON.stringify(json.asset));
console.log('scenes          :', count('scenes'));
console.log('nodes           :', count('nodes'));
console.log('meshes          :', count('meshes'));
console.log('materials       :', count('materials'));
console.log('accessors       :', count('accessors'));
console.log('bufferViews     :', count('bufferViews'));
console.log('BIN chunk bytes :', binLength);

let totalPrimitives = 0;
let totalVertices = 0;
let totalIndices = 0;

for (const mesh of json.meshes ?? []) {
    for (const primitive of mesh.primitives ?? []) {
        totalPrimitives += 1;
        const positionIndex = primitive.attributes?.POSITION;
        if (positionIndex !== undefined) {
            totalVertices += json.accessors[positionIndex].count;
        }
        if (primitive.indices !== undefined) {
            totalIndices += json.accessors[primitive.indices].count;
        }
    }
}

console.log('\nprimitives      :', totalPrimitives);
console.log('vertices        :', totalVertices);
console.log('indices         :', totalIndices);

console.log('\nnode names      :', (json.nodes ?? []).map((n) => n.name ?? '<unnamed>').join(', '));

if (totalPrimitives === 0 || totalVertices === 0) {
    console.error('\nFAILED: the GLB contains no geometry.');
    process.exit(1);
}

// The source cube has 6 quads (24 corners -> 12 triangles) and the plane 1 quad
// (2 triangles), so a correct triangulation yields 14 triangles = 42 indices.
const expectedIndices = 42;
if (totalIndices !== expectedIndices) {
    console.warn(
        `\nWARNING: expected ${expectedIndices} indices from triangulating 7 quads, got ${totalIndices}.`,
    );
} else {
    console.log('\nTriangulation matches expectation (7 quads -> 14 triangles).');
}

console.log('\nPASS: GLB contains real geometry.');
