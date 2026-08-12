// Bundles the TypeScript sources with webpack.
//
// Two library builds are produced from the same two entry points (index, babylon):
//   * an ES module build   -> dist/{index,babylon}.js
//   * a CommonJS build      -> dist/{index,babylon}.cjs
// Type declarations are emitted separately by `tsc -p tsconfig.build.json` (npm run
// build:types), which the `build` script runs before webpack.
//
// The Emscripten glue (./wasm/usd-web-gltf.js) is never bundled: it is imported at runtime
// through a dynamic import() carrying a `webpackIgnore` magic comment (see src/glue.ts), so
// webpack leaves that import verbatim. The accompanying .wasm/.data artifacts are copied out
// of the native build folder by CopyWebpackPlugin below.

import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';
import webpack from 'webpack';
import CopyPlugin from 'copy-webpack-plugin';

const here = dirname(fileURLToPath(import.meta.url));
const src = (file) => resolve(here, 'src', file);

// The WebAssembly artifacts produced by the `wasm` CMake preset. Webpack copies these into
// dist/wasm so the published package is self-contained. Point USD_WEB_GLTF_BIN at a
// different directory to bundle a build from elsewhere.
const wasmSourceDir = process.env.USD_WEB_GLTF_BIN
    ? resolve(process.env.USD_WEB_GLTF_BIN)
    : resolve(here, '..', 'build', 'wasm', 'bin');

const WASM_ARTIFACTS = ['usd-web-gltf.js', 'usd-web-gltf.wasm', 'usd-web-gltf.data'];

if (!existsSync(wasmSourceDir)) {
    throw new Error(
        `WebAssembly build output not found at ${wasmSourceDir}.\n` +
            'Build the wasm module first — from the repository root run:\n' +
            '  cmake --workflow --preset wasm\n' +
            'or set USD_WEB_GLTF_BIN to a directory holding usd-web-gltf.{js,wasm,data}.',
    );
}

const entry = {
    index: src('index.ts'),
    babylon: src('babylon.ts'),
};

/**
 * Shared configuration for both library formats.
 *
 * @param {'module' | 'commonjs2'} libraryType
 * @param {string} ext output file extension (without the dot)
 */
function makeConfig(libraryType, ext) {
    const isModule = libraryType === 'module';
    return {
        name: isModule ? 'esm' : 'cjs',
        mode: 'production',
        // Neutral-ish: the package runs in browsers and in Node. Nothing here uses a
        // platform-specific global, and the only dynamic import is left external.
        target: ['web', 'es2022'],
        entry,
        experiments: isModule ? { outputModule: true } : {},
        output: {
            path: resolve(here, 'dist'),
            filename: `[name].${ext}`,
            module: isModule,
            library: { type: libraryType },
            // Keep native import() in the output so the webpackIgnore'd glue import survives.
            environment: { dynamicImport: true, module: isModule },
        },
        resolve: {
            extensions: ['.ts', '.js'],
            // Sources reference siblings as "./x.js" (TS Bundler/NodeNext style); map those
            // specifiers back onto the .ts files on disk.
            extensionAlias: { '.js': ['.ts', '.js'] },
        },
        module: {
            // getGlueBaseUrl() uses `new URL(spec, import.meta.url)` to compute the glue's
            // directory at runtime. Two webpack defaults would break that:
            //   * `url` parsing rewrites the pattern into an asset-module request; and
            //   * `importMeta` parsing inlines import.meta.url to the *build-time* source
            //     path, which both leaks the build machine's path and defeats the runtime
            //     resolution the browser needs.
            // Disabling both leaves the expression intact. In the ES module output that is a
            // valid runtime `import.meta.url` (the bundle's own URL); in the CommonJS output
            // `import.meta` is illegal, so it is replaced with `undefined` by DefinePlugin
            // below — getGlueBaseUrl then safely returns undefined and Node falls back to
            // resolving the glue's siblings against the script directory.
            parser: { javascript: { url: false, importMeta: false } },
            rules: [
                {
                    test: /\.ts$/,
                    loader: 'ts-loader',
                    options: {
                        // Type checking and .d.ts emission are handled by `tsc` in the
                        // build:types step, so here webpack only transpiles.
                        transpileOnly: true,
                        compilerOptions: { declaration: false, declarationMap: false },
                    },
                },
            ],
        },
        // Preserve readable, debuggable output (matches the previous esbuild build) while
        // still benefiting from production-mode tree shaking.
        optimization: { minimize: false },
        devtool: 'source-map',
        stats: 'minimal',
        // The wasm/.data files are large copied runtime assets, not bundled code, so the
        // bundle-size performance hints don't apply.
        performance: { hints: false },
    };
}

const esm = makeConfig('module', 'js');
// Copy the native artifacts once, from the ESM compiler only, to avoid a double write.
esm.plugins = [
    new CopyPlugin({
        patterns: WASM_ARTIFACTS.map((name) => ({
            from: resolve(wasmSourceDir, name),
            to: `wasm/${name}`,
        })),
    }),
];

const cjs = makeConfig('commonjs2', 'cjs');
// `import.meta` is not valid in a CommonJS module, and the parser is configured to leave it
// untouched, so replace `import.meta.url` with `undefined` here. getGlueBaseUrl() catches the
// resulting error and returns undefined, which is the correct behaviour for the CJS/Node
// consumer (Emscripten resolves the glue's siblings against the script directory itself).
cjs.plugins = [new webpack.DefinePlugin({ 'import.meta.url': 'undefined' })];

export default [esm, cjs];
