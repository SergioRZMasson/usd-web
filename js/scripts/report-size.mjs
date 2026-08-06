// Reports the download cost of the shipped WebAssembly artifacts, both raw and
// compressed, so regressions in binary size are visible in CI.

import { readdir, readFile, stat } from 'node:fs/promises';
import { brotliCompressSync, gzipSync, constants } from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..');
const wasmDir = process.env.POLYMORPH_WASM_DIR ?? resolve(root, 'dist/wasm');

const mib = (bytes) => `${(bytes / 1024 / 1024).toFixed(2)} MB`;

let files;
try {
    files = await readdir(wasmDir);
} catch {
    console.error(`No artifacts found at ${wasmDir}. Run "npm run build" first.`);
    process.exit(1);
}

const rows = [];
let totals = { raw: 0, gzip: 0, brotli: 0 };

for (const name of files.sort()) {
    const path = resolve(wasmDir, name);
    if (!(await stat(path)).isFile()) continue;

    const bytes = await readFile(path);
    const gzip = gzipSync(bytes, { level: 9 }).length;
    const brotli = brotliCompressSync(bytes, {
        params: { [constants.BROTLI_PARAM_QUALITY]: 11 },
    }).length;

    rows.push({ name, raw: bytes.length, gzip, brotli });
    totals.raw += bytes.length;
    totals.gzip += gzip;
    totals.brotli += brotli;
}

const width = Math.max(...rows.map((r) => r.name.length), 5);
console.log('\nWebAssembly payload size\n');
console.log(`${'File'.padEnd(width)}  ${'Raw'.padStart(10)}  ${'gzip'.padStart(10)}  ${'brotli'.padStart(10)}`);
console.log('-'.repeat(width + 38));
for (const row of rows) {
    console.log(
        `${row.name.padEnd(width)}  ${mib(row.raw).padStart(10)}  ${mib(row.gzip).padStart(10)}  ${mib(row.brotli).padStart(10)}`,
    );
}
console.log('-'.repeat(width + 38));
console.log(
    `${'TOTAL'.padEnd(width)}  ${mib(totals.raw).padStart(10)}  ${mib(totals.gzip).padStart(10)}  ${mib(totals.brotli).padStart(10)}\n`,
);
