// Verifies asset resolution: that WebResolver is selected, that relative references
// resolve when sibling files are supplied, and that an absolute path baked in on another
// machine is recovered by the file-name fallback.

import createUsdGltfModule from '../build/bin/usd-web-gltf.js';

const Module = await createUsdGltfModule();

const warnings = [];
Module.setLogCallback((level, message) => {
    if (level >= 1) warnings.push(message);
});

console.log('resolver:', Module.getResolverName());

const enc = (s) => new TextEncoder().encode(s);

// A child layer holding one mesh.
const child = `#usda 1.0
def Mesh "Child"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
}
`;

// Root layer with two references:
//   - a relative one, which should resolve against the root layer's directory
//   - an absolute one from a different machine, which only the fallback can satisfy
const root = `#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def "Relative" (prepend references = @sub/child.usda@</Child>)
    {
    }

    def "AbsoluteFromAnotherMachine" (prepend references = @/Users/someone/Desktop/child.usda@</Child>)
    {
    }
}
`;

Module.FS.mkdir('/work');
Module.FS.mkdir('/work/sub');
Module.FS.writeFile('/work/sub/child.usda', enc(child));
Module.FS.writeFile('/work/root.usda', enc(root));

// Index the supplied files so unresolvable references can fall back to a name match.
Module.registerAssetDirectory('/work');

const result = Module.convert('/work/root.usda', '/work/root.glb');

try {
    if (!result.ok()) {
        console.error('FAILED:', result.error());
        process.exit(1);
    }

    const glb = Module.HEAPU8.slice(result.dataPtr(), result.dataPtr() + result.dataSize());
    const view = new DataView(glb.buffer, glb.byteOffset, glb.byteLength);

    // Parse the JSON chunk to count what actually made it through.
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

    const meshCount = json.meshes?.length ?? 0;
    console.log(`meshes in output: ${meshCount}`);
    console.log('unresolved:', Module.getUnresolvedAssets() || '(none)');
    console.log('\nwarnings:');
    for (const w of warnings) console.log('  -', w);

    // Both references must have produced geometry: the relative one through normal
    // resolution, the absolute one through the fallback.
    if (meshCount !== 2) {
        console.error(`\nFAILED: expected 2 meshes (relative + fallback), got ${meshCount}.`);
        process.exit(1);
    }

    const usedFallback = warnings.some((w) => w.includes('by file name'));
    if (!usedFallback) {
        console.error('\nFAILED: the absolute reference did not go through the name fallback.');
        process.exit(1);
    }

    console.log('\nPASS: relative references resolve, and absolute paths from another machine');
    console.log('      are recovered by file-name fallback.');
} finally {
    result.delete();
}
