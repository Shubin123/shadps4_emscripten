#!/usr/bin/env node
'use strict';

// Minimal static file server for web/. Cross-Origin-Opener-Policy/Cross-Origin-Embedder-Policy
// are set on every response because SharedArrayBuffer (needed for any future pthreads-enabled
// wasm build) requires the page to be cross-origin isolated - setting these now, even though
// there is no wasm module to load yet, avoids relearning this requirement later.
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = __dirname;
const PORT = process.env.PORT ? Number(process.env.PORT) : 8766;

const CONTENT_TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.cjs': 'text/javascript; charset=utf-8',
    '.mjs': 'text/javascript; charset=utf-8',
    '.wasm': 'application/wasm',
    '.json': 'application/json; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.svg': 'image/svg+xml',
    '.png': 'image/png',
};

const ISOLATION_HEADERS = {
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
};

const server = http.createServer((req, res) => {
    let urlPath = decodeURIComponent(req.url.split('?')[0]);
    if (urlPath === '/') {
        urlPath = '/index.html';
    }
    const filePath = path.normalize(path.join(ROOT, urlPath));
    if (!filePath.startsWith(ROOT)) {
        res.writeHead(403, {'Content-Type': 'text/plain', ...ISOLATION_HEADERS});
        res.end('Forbidden');
        return;
    }
    fs.readFile(filePath, (err, data) => {
        if (err) {
            res.writeHead(404, {'Content-Type': 'text/plain', ...ISOLATION_HEADERS});
            res.end('Not found');
            return;
        }
        const ext = path.extname(filePath);
        res.writeHead(200, {
            'Content-Type': CONTENT_TYPES[ext] || 'application/octet-stream',
            ...ISOLATION_HEADERS,
        });
        res.end(data);
    });
});

server.listen(PORT, () => {
    console.log(`shadps4_web dev server: http://localhost:${PORT}`);
});
