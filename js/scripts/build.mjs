// Bundles the TypeScript sources into ESM + CJS with esbuild.
// Type declarations are emitted separately by `tsc -p tsconfig.build.json`.

import { build } from 'esbuild';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..');

/** The Emscripten glue is resolved at runtime, never bundled. */
const external = ['./wasm/usd-web-gltf.js'];

const entryPoints = [resolve(root, 'src/index.ts'), resolve(root, 'src/babylon.ts')];

const shared = {
    entryPoints,
    bundle: true,
    platform: 'neutral',
    target: ['es2022', 'node18'],
    sourcemap: true,
    external,
    logLevel: 'info',
};

await Promise.all([
    build({ ...shared, format: 'esm', outdir: resolve(root, 'dist'), outExtension: { '.js': '.js' } }),
    build({ ...shared, format: 'cjs', outdir: resolve(root, 'dist'), outExtension: { '.js': '.cjs' } }),
]);

console.log('Bundled dist/{index,babylon}.{js,cjs}.');
