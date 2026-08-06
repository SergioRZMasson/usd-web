// Builds the browser demo into the repository's `dist/` folder.
//
// `dist/` is committed and published by GitHub Pages (see .github/workflows/pages.yml), so
// partners can try the converter without building anything. It is self-contained: the demo
// app, the WebAssembly artifacts and the sample USD files.

import { build, context } from "esbuild";
import { cp, mkdir, readdir, rm, writeFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const demoRoot = resolve(here, "..");
const projectRoot = resolve(demoRoot, "..");
const distDir = resolve(projectRoot, "dist");
const watch = process.argv.includes("--watch");

await rm(distDir, { recursive: true, force: true });
await mkdir(distDir, { recursive: true });

// Tells GitHub Pages to serve the folder verbatim instead of running it through Jekyll,
// which would otherwise skip files and folders beginning with an underscore.
await writeFile(resolve(distDir, ".nojekyll"), "");

await cp(resolve(demoRoot, "index.html"), resolve(distDir, "index.html"));

// The Emscripten glue, the wasm binary and the resource bundle must ship together: the
// glue resolves its siblings relative to its own URL.
const wasmSource = resolve(projectRoot, "js/dist/wasm");
const wasmTarget = resolve(distDir, "wasm");
await mkdir(wasmTarget, { recursive: true });
for (const file of await readdir(wasmSource)) {
    await cp(resolve(wasmSource, file), resolve(wasmTarget, file));
}
console.log(`staged WebAssembly artifacts -> ${wasmTarget}`);

// Sample USD assets, including the multi-file scene.
await cp(resolve(demoRoot, "assets"), resolve(distDir, "assets"), { recursive: true });
console.log(`staged sample assets -> ${resolve(distDir, "assets")}`);

const options = {
    entryPoints: [resolve(demoRoot, "src/main.js")],
    outfile: resolve(distDir, "app.js"),
    bundle: true,
    format: "esm",
    target: "es2022",
    // dist/ is committed, so the bundle is minified and shipped without a source map.
    // Unminified it is ~14 MB (Babylon.js is bundled whole) versus ~4 MB minified, and the
    // map alone would add another ~24 MB of history for no benefit to a published demo.
    minify: !watch,
    sourcemap: watch,
    logLevel: "info",
    // The converter package resolves its glue at runtime; keep it out of the bundle so the
    // browser fetches ./wasm/usd-web-gltf.js as a real module.
    external: ["./wasm/usd-web-gltf.js"],
};

if (watch) {
    const ctx = await context(options);
    await ctx.watch();
    console.log("watching for changes...");
} else {
    await build(options);
    console.log(`demo built into ${distDir}`);
}
