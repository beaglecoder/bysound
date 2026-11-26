// Standalone Vite config for this example: four pages, and /api proxied to the local backend (server.mjs on :8787).
// No import of 'vite': this folder has no node_modules of its own, it borrows ../../web's binary like the other examples.
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const backend = `http://127.0.0.1:${process.env.PORT ?? 8787}`;

export default {
  root: here,
  base: './',
  worker: { format: 'es' },
  build: {
    target: 'es2022',
    outDir: resolve(here, 'dist'),
    emptyOutDir: true,
    rollupOptions: { input: Object.fromEntries(['index', 'sender', 'receiver', 'rogue'].map((p) => [p, resolve(here, `${p}.html`)])) },
  },
  // The pages import ../../web/src and ../shared, outside this root.
  server: { fs: { allow: [resolve(here, '../..')] }, proxy: { '/api': { target: backend, rewrite: (p) => p.replace(/^\/api/, '') } } },
  preview: { proxy: { '/api': { target: backend, rewrite: (p) => p.replace(/^\/api/, '') } } },
};
