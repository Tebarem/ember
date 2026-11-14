import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

// Ember serves the build from app://local/, so the output uses relative URLs (base './').
// es2022 is what Ember's JavaScript engine targets.
export default defineConfig({
  base: './',
  plugins: [vue()],
  build: {
    target: 'es2022',
    outDir: 'dist',
    emptyOutDir: true,
  },
});
