// Multi-page variant of ../vite.config.ts: this example has three HTML pages (index, beacon, customer). Used by dev:store-presence and build:web.
import { fileURLToPath } from 'node:url';

const at = (p: string) => fileURLToPath(new URL(p, import.meta.url));

export default {
  base: './',
  worker: { format: 'es' },
  build: { target: 'es2022', rollupOptions: { input: { index: at('index.html'), beacon: at('beacon.html'), customer: at('customer.html') } } },
  server: { fs: { allow: ['..'] } },
};
