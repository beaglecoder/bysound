// Shared by the browser examples: `vite <example-folder> -c vite.config.ts`. The examples import ../web/src, outside their root.
// (no import of 'vite': this folder has no node_modules of its own, it borrows web/'s binary)
export default {
  base: './',
  worker: { format: 'es' },
  build: { target: 'es2022' },
  server: { fs: { allow: ['..'] } },
};
