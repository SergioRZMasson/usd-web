// Builds the browser demo into the repository's `docs/` folder with webpack.
//
// `docs/` is committed and published by GitHub Pages, so partners can try the converter
// without building anything. GitHub Pages can only serve `/` or `/docs` when deploying from
// a branch, which is why the folder is named `docs` rather than `dist`.
//
// The output is self-contained: the bundled demo app (app.js), the WebAssembly artifacts
// staged under wasm/, the sample USD files under assets/, and index.html.

import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';
import webpack from 'webpack';
import CopyPlugin from 'copy-webpack-plugin';

const here = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(here, '..');
const siteDir = resolve(projectRoot, 'docs');
const converterBundle = resolve(projectRoot, 'js', 'dist', 'index.js');

// The Emscripten glue, the wasm binary and the resource bundle are produced by the js
// package's build (npm --prefix ../js run build). They must ship next to app.js: the glue
// resolves its siblings relative to its own URL.
const wasmSourceDir = resolve(projectRoot, 'js', 'dist', 'wasm');
if (!existsSync(wasmSourceDir)) {
    throw new Error(
        `WebAssembly artifacts not found at ${wasmSourceDir}.\n` +
            'Build the package first:  npm --prefix ../js run build',
    );
}
if (!existsSync(converterBundle)) {
    throw new Error(`Converter bundle not found at ${converterBundle}. Run npm --prefix ../js run build first.`);
}

// `--watch` rebuilds docs/ on change; serve.mjs serves it. A production build is minified
// and shipped without a source map (docs/ is committed — the map alone would add ~24 MB of
// history), while a watch build keeps the map for debugging.
const watch = process.argv.includes('--watch');

/** Emits an empty `.nojekyll` so GitHub Pages serves the folder verbatim rather than
 *  running it through Jekyll, which would skip files and folders beginning with an
 *  underscore. */
class EmitNojekyllPlugin {
    apply(compiler) {
        const { RawSource } = compiler.webpack.sources;
        compiler.hooks.thisCompilation.tap('EmitNojekyll', (compilation) => {
            compilation.hooks.processAssets.tap(
                {
                    name: 'EmitNojekyll',
                    stage: compiler.webpack.Compilation.PROCESS_ASSETS_STAGE_ADDITIONAL,
                },
                () => compilation.emitAsset('.nojekyll', new RawSource('')),
            );
        });
    }
}

export default {
    name: 'demo',
    mode: watch ? 'development' : 'production',
    target: ['web', 'es2022'],
    entry: resolve(here, 'src', 'main.js'),
    // index.html loads app.js with <script type="module">, so emit an ES module. This also
    // keeps import.meta.url meaningful at runtime (see the parser note below).
    experiments: { outputModule: true },
    output: {
        path: siteDir,
        filename: 'app.js',
        module: true,
        clean: true,
        environment: { dynamicImport: true, module: true },
    },
    module: {
        // The bundled usd-web-gltf package locates its glue with a webpackIgnore'd dynamic
        // import() and computes the .wasm/.data base with `new URL(spec, import.meta.url)`.
        // Webpack's defaults would rewrite the `new URL` into an emitted asset and inline
        // import.meta.url to a build path; disabling both leaves them as runtime values that
        // resolve against app.js's own URL — i.e. ./wasm next to the bundle.
        parser: { javascript: { url: false, importMeta: false } },
    },
    optimization: { minimize: !watch },
    devtool: watch ? 'source-map' : false,
    stats: 'minimal',
    performance: { hints: false },
    plugins: [
        // Babylon.js lazy-loads features through internal dynamic import(), which webpack
        // would otherwise split into ~160 separate chunk files. docs/ is committed, so keep
        // the output a single app.js (as the previous esbuild build did) to avoid churning
        // git history on every rebuild. The webpackIgnore'd glue import is external and
        // unaffected by this.
        new webpack.optimize.LimitChunkCountPlugin({ maxChunks: 1 }),
        new CopyPlugin({
            patterns: [
                { from: resolve(here, 'index.html'), to: 'index.html' },
                { from: resolve(here, 'src', 'conversionWorker.js'), to: 'conversionWorker.js' },
                { from: converterBundle, to: 'usd-web.js' },
                // Ship the prebuilt Emscripten artifacts verbatim. `info.minimized` stops
                // webpack's production Terser pass from re-minifying the glue .js, which is
                // already optimized by Emscripten and must not be altered.
                { from: wasmSourceDir, to: 'wasm', info: { minimized: true } },
                { from: resolve(here, 'assets'), to: 'assets' },
            ],
        }),
        new EmitNojekyllPlugin(),
    ],
};
