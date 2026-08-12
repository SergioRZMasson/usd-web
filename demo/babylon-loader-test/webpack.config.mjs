// Bundles the Babylon USD loader integration test with webpack.
//
// This drives Babylon's own USDFileLoader (from a Babylon.js dev checkout) rather than the
// converter package directly, so `core` and `loaders` are aliased to the built Babylon dev
// packages. Set BABYLON_DEV_PACKAGES if your checkout is not at the default path.
//
// Note: this test needs a Babylon.js source checkout to resolve `core/...` and
// `loaders/...`; it cannot be built without one. webpack itself is resolved from the demo
// package's node_modules, so run it from this directory after `npm install` in ../ (demo).

import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';
import webpack from 'webpack';
import CopyPlugin from 'copy-webpack-plugin';

const here = dirname(fileURLToPath(import.meta.url));
const demoRoot = resolve(here, '..');
const projectRoot = resolve(demoRoot, '..');
const publicDir = resolve(here, 'public');

// The dev packages emit bare "core/..." / "loaders/..." specifiers that only resolve
// against a Babylon.js checkout's built dist folders.
const babylonRoot = process.env.BABYLON_DEV_PACKAGES ?? 'E:/Github/Babylon.js/packages/dev';

// The converter artifacts must sit together next to app.js; the test points the loader's
// wasmUrl/dataUrl at this folder.
const wasmSourceDir = resolve(projectRoot, 'js', 'dist', 'wasm');
if (!existsSync(wasmSourceDir)) {
    throw new Error(
        `WebAssembly artifacts not found at ${wasmSourceDir}.\n` +
            'Build the package first:  npm --prefix ../../js run build',
    );
}

const watch = process.argv.includes('--watch');

export default {
    name: 'babylon-loader-test',
    mode: watch ? 'development' : 'production',
    target: ['web', 'es2022'],
    entry: resolve(here, 'src', 'main.js'),
    experiments: { outputModule: true },
    output: {
        path: publicDir,
        filename: 'app.js',
        module: true,
        clean: true,
        environment: { dynamicImport: true, module: true },
    },
    resolve: {
        // Extensionless "core/scene" style imports; the Babylon dev packages are aliased to
        // their built dist trees.
        extensions: ['.js'],
        alias: {
            core: `${babylonRoot}/core/dist`,
            loaders: `${babylonRoot}/loaders/dist`,
        },
    },
    module: {
        // The Babylon loader loads the Emscripten glue through a webpackIgnore'd dynamic
        // import(); keep webpack from rewriting new URL()/import.meta.url so those stay as
        // runtime values (the test also sets wasmUrl/dataUrl against location.href).
        parser: { javascript: { url: false, importMeta: false } },
    },
    optimization: { minimize: !watch },
    devtool: 'source-map',
    stats: 'minimal',
    performance: { hints: false },
    plugins: [
        // Babylon lazy-loads features through internal dynamic import(); keep the output a
        // single app.js.
        new webpack.optimize.LimitChunkCountPlugin({ maxChunks: 1 }),
        new CopyPlugin({
            patterns: [
                // Prebuilt Emscripten artifacts ship verbatim (info.minimized stops Terser).
                { from: wasmSourceDir, to: 'wasm', info: { minimized: true } },
                { from: resolve(demoRoot, 'assets'), to: 'assets' },
            ],
        }),
    ],
};
