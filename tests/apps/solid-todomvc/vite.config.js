import { fileURLToPath, URL } from 'node:url';
import { defineConfig } from 'vite';
import solid from 'vite-plugin-solid';

// Built with `base: './'` so the output loads from any directory (app://local/ in Ember).
export default defineConfig({
  base: './',
  plugins: [solid()],
  resolve: {
    alias: { '@shared': fileURLToPath(new URL('../shared', import.meta.url)) },
  },
  build: {
    target: 'es2022',
    minify: true,
    outDir: 'dist',
    emptyOutDir: true,
  },
});
