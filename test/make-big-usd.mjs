// Generates a larger synthetic USD scene for benchmarking.
//
// The sample assets are tiny (a handful of quads), which makes conversion time dominated
// by fixed overhead. Optimisation level mostly affects the per-primitive work, so a
// realistic mesh count is needed to compare -O3/-Os/-Oz honestly.
//
// Usage: node make-big-usd.mjs [gridSize] [out]

import { writeFileSync } from "node:fs";

const grid = Number(process.argv[2] ?? 24); // grid x grid subdivided planes
const out = process.argv[3] ?? "big.usda";
const segments = 12; // subdivisions per plane edge -> segments^2 quads each

const parts = [
    `#usda 1.0`,
    `(`,
    `    defaultPrim = "World"`,
    `    metersPerUnit = 1`,
    `    upAxis = "Y"`,
    `)`,
    ``,
    `def Xform "World"`,
    `{`,
];

let totalQuads = 0;

for (let gx = 0; gx < grid; gx++) {
    for (let gz = 0; gz < grid; gz++) {
        const points = [];
        const counts = [];
        const indices = [];
        const uvs = [];

        for (let i = 0; i <= segments; i++) {
            for (let j = 0; j <= segments; j++) {
                const u = i / segments;
                const v = j / segments;
                // A gentle dome so normals and positions vary rather than being degenerate.
                const y = Math.sin(u * Math.PI) * Math.sin(v * Math.PI) * 0.4;
                points.push(`(${(u - 0.5).toFixed(4)}, ${y.toFixed(4)}, ${(v - 0.5).toFixed(4)})`);
                uvs.push(`(${u.toFixed(4)}, ${v.toFixed(4)})`);
            }
        }

        const stride = segments + 1;
        for (let i = 0; i < segments; i++) {
            for (let j = 0; j < segments; j++) {
                const a = i * stride + j;
                counts.push(4);
                indices.push(a, a + 1, a + stride + 1, a + stride);
                totalQuads++;
            }
        }

        parts.push(
            `    def Mesh "Tile_${gx}_${gz}"`,
            `    {`,
            `        int[] faceVertexCounts = [${counts.join(", ")}]`,
            `        int[] faceVertexIndices = [${indices.join(", ")}]`,
            `        point3f[] points = [${points.join(", ")}]`,
            `        texCoord2f[] primvars:st = [${uvs.join(", ")}] (interpolation = "vertex")`,
            `        color3f[] primvars:displayColor = [(${(gx / grid).toFixed(3)}, 0.5, ${(gz / grid).toFixed(3)})]`,
            `        double3 xformOp:translate = (${(gx - grid / 2) * 1.2}, 0, ${(gz - grid / 2) * 1.2})`,
            `        uniform token[] xformOpOrder = ["xformOp:translate"]`,
            `    }`
        );
    }
}

parts.push(`}`, ``);

writeFileSync(out, parts.join("\n"));

const meshes = grid * grid;
console.log(`${out}: ${meshes} meshes, ${totalQuads} quads (${totalQuads * 2} triangles)`);
