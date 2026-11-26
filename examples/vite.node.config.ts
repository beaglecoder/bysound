// Bundles the Node scripts (they import the TypeScript library from ../web/src) into plain .mjs files in dist-node/.
import { fileURLToPath } from 'node:url';

const at = (p: string) => fileURLToPath(new URL(p, import.meta.url));

export default {
  build: {
    ssr: true,
    target: 'node22',
    outDir: 'dist-node',
    emptyOutDir: true,
    rollupOptions: {
      input: { cli: at('node-cli/cli.ts'), 'music-variants': at('music-variants/music-variants.ts') },
      output: { entryFileNames: '[name].mjs', format: 'es' },
    },
  },
};
