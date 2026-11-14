import { fileURLToPath, URL } from 'node:url';
import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

// Built with `base: './'` so the output loads from any directory (app://local/ in Ember).
export default defineConfig({
  base: './',
  plugins: [vue()],
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
