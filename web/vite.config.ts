import { defineConfig } from 'vitest/config';

// index.html lives at web/; `npm run build` writes a static site to web/dist that works from any host or sub-path.
export default defineConfig({
  base: './',
  worker: { format: 'es' },
  build: { target: 'es2022' },
  test: { include: ['src/**/*.test.ts'] },
});
