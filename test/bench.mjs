// Benchmarks conversion throughput for a given build, so the size/speed trade-off of
// different optimisation levels can be compared on equal terms.
//
// Usage (from the directory holding usd-web-gltf.js/.wasm/.data):
//   node bench.mjs [iterations]

import createUsdGltfModule from "./usd-web-gltf.js";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const iterations = Number(process.argv[2] ?? 12);

// Resolve the sample relative to this script so it can be copied next to any build.
const assetPath = process.env.BENCH_ASSET ?? resolve(here, "materials.usda");
const asset = readFileSync(assetPath);

const instantiateStart = performance.now();
const Module = await createUsdGltfModule();
const instantiateMs = performance.now() - instantiateStart;

// Silence diagnostics so logging cost does not pollute the measurement.
Module.setLogCallback(() => {});

Module.FS.mkdir("/work");
Module.FS.writeFile("/work/in.usda", new Uint8Array(asset));

const samples = [];
let bytes = 0;

for (let i = 0; i < iterations; i++) {
    const start = performance.now();
    const result = Module.convert("/work/in.usda", `/work/out${i}.glb`);
    const elapsed = performance.now() - start;
    if (!result.ok()) {
        console.error("FAILED:", result.error());
        process.exit(1);
    }
    bytes = result.dataSize();
    result.delete();
    Module.FS.unlink(`/work/out${i}.glb`);
    samples.push(elapsed);
}

samples.sort((a, b) => a - b);
const median = samples[Math.floor(samples.length / 2)];
const mean = samples.reduce((a, b) => a + b, 0) / samples.length;

console.log(`instantiate : ${instantiateMs.toFixed(0)} ms`);
console.log(`iterations  : ${iterations}`);
console.log(`output      : ${bytes} bytes`);
console.log(`min / med   : ${samples[0].toFixed(1)} / ${median.toFixed(1)} ms`);
console.log(`mean / max  : ${mean.toFixed(1)} / ${samples[samples.length - 1].toFixed(1)} ms`);
