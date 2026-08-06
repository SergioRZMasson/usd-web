// Bundles the Babylon USD loader test page.
//
// The dev packages emit bare "core/..." specifiers that only a bundler with the repo's
// path mapping can resolve, so `core` is aliased to the built core package here.

import { build } from "esbuild";
import { cp, mkdir, readdir } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const testRoot = resolve(here, "..");
const demoRoot = resolve(testRoot, "..");
const projectRoot = resolve(demoRoot, "..");
const babylonRoot = process.env.BABYLON_DEV_PACKAGES ?? "E:/Github/Babylon.js/packages/dev";
const publicDir = resolve(testRoot, "public");

await mkdir(publicDir, { recursive: true });

// The converter artifacts must sit together: the test points locateFile at this folder.
const wasmSource = resolve(projectRoot, "js/dist/wasm");
const wasmTarget = resolve(publicDir, "wasm");
await mkdir(wasmTarget, { recursive: true });
for (const file of await readdir(wasmSource)) {
    await cp(resolve(wasmSource, file), resolve(wasmTarget, file));
}

const assetsTarget = resolve(publicDir, "assets");
await mkdir(assetsTarget, { recursive: true });
for (const file of await readdir(resolve(demoRoot, "assets"), { withFileTypes: true })) {
    if (file.isFile()) {
        await cp(resolve(demoRoot, "assets", file.name), resolve(assetsTarget, file.name));
    }
}

await build({
    entryPoints: [resolve(testRoot, "src/main.js")],
    outfile: resolve(publicDir, "app.js"),
    bundle: true,
    format: "esm",
    target: "es2022",
    sourcemap: true,
    logLevel: "info",
    alias: {
        core: `${babylonRoot}/core/dist`,
        loaders: `${babylonRoot}/loaders/dist`,
    },
    // The Emscripten glue is fetched at runtime from ./wasm, never bundled.
    external: ["./wasm/usd-web-gltf.js"],
});

console.log("Babylon USD loader test bundled.");
