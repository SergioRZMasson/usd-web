// Copies the Emscripten build output into dist/wasm so the published package is
// self-contained.
//
// Three artifacts matter:
//   usd-web-gltf.js    Emscripten glue (module factory)
//   usd-web-gltf.wasm  the compiled binary
//   usd-web-gltf.data  USD's plugInfo.json/schema resources, loaded into the virtual FS

import { cp, mkdir, stat } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..');

// The single-threaded build is shipped by default: it needs no COOP/COEP headers, which
// makes it deployable anywhere. Point USD_WEB_GLTF_BIN at ../build/bin for the pthreads
// build if you need it.
const sourceDir = process.env.USD_WEB_GLTF_BIN ?? resolve(root, "../build-st/bin");
const targetDir = resolve(root, 'dist/wasm');

const artifacts = ['usd-web-gltf.js', 'usd-web-gltf.wasm', 'usd-web-gltf.data'];

async function exists(path) {
    try {
        await stat(path);
        return true;
    } catch {
        return false;
    }
}

if (!(await exists(sourceDir))) {
    console.error(
        `Native build output not found at ${sourceDir}.\n` +
            'Build the WebAssembly target first, or set USD_WEB_GLTF_BIN.',
    );
    process.exit(1);
}

await mkdir(targetDir, { recursive: true });

for (const artifact of artifacts) {
    const from = resolve(sourceDir, artifact);
    if (!(await exists(from))) {
        console.error(`Missing required artifact: ${from}`);
        process.exit(1);
    }
    await cp(from, resolve(targetDir, artifact));
    const { size } = await stat(from);
    console.log(`  ${artifact.padEnd(22)} ${(size / 1024 / 1024).toFixed(2)} MB`);
}

console.log(`Copied WebAssembly artifacts into ${targetDir}.`);
