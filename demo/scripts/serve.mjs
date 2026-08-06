// Static file server for the demo.
//
// Serves the built `dist/` folder — the same content GitHub Pages publishes — so what you
// test locally is what partners see.
//
// The shipped WebAssembly module is single-threaded, so no COOP/COEP headers are required
// and any static server will do. They are still set here because the optional pthreads
// build needs them, and it costs nothing to keep local parity with a cross-origin isolated
// deployment.

import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(process.env.ROOT ?? fileURLToPath(new URL('../../dist', import.meta.url)));
const port = Number(process.env.PORT ?? 8080);

const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.mjs': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.wasm': 'application/wasm',
    '.data': 'application/octet-stream',
    '.usd': 'model/vnd.usd',
    '.usda': 'model/vnd.usda',
    '.usdc': 'model/vnd.usdc',
    '.usdz': 'model/vnd.usdz+zip',
    '.glb': 'model/gltf-binary',
    '.map': 'application/json; charset=utf-8',
};

const server = createServer(async (req, res) => {
    res.setHeader('Cache-Control', 'no-store');

    const urlPath = decodeURIComponent((req.url ?? '/').split('?')[0]);
    // normalize() collapses any ".." so a request cannot escape the served root.
    const relative = normalize(urlPath === '/' ? '/index.html' : urlPath).replace(/^[/\\]+/, '');
    const filePath = join(root, relative);

    if (!filePath.startsWith(root)) {
        res.writeHead(403).end('Forbidden');
        return;
    }

    try {
        const info = await stat(filePath);
        if (!info.isFile()) throw new Error('not a file');

        const body = await readFile(filePath);
        res.writeHead(200, {
            'Content-Type': MIME[extname(filePath).toLowerCase()] ?? 'application/octet-stream',
            'Content-Length': body.length,
        });
        res.end(body);
    } catch {
        res.writeHead(404, { 'Content-Type': 'text/plain' }).end('Not found');
    }
});

server.listen(port, () => {
    console.log(`\n  usd-web demo -> http://localhost:${port}`);
    console.log(`  serving ${root}\n`);
    console.log('  Press Ctrl+C to stop.\n');
});
