import { defineConfig } from 'vite';

// The W7 exit app: the vanilla-ts template with fetch, localStorage, a CSS module and an image asset.
// Built with `base: './'` so the output loads from any directory (app://local/ in Ember).
export default defineConfig({
  base: './',
  build: {
    target: 'es2022',
    minify: true,
    assetsInlineLimit: 0,
    outDir: 'dist',
    emptyOutDir: true,
  },
});
