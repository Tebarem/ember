# Framework builds for engine smoke tests

Unmodified files from the npm registry, MIT licensed (see the LICENSE files).

- `preact-10.29.8.umd.js`: `preact@10.29.8`, `dist/preact.umd.js` (Preact ships this build minified).
  Preact 11 ships ESM only: its build is in `preact-11/` and is tested as a module graph
  (test_js_frameworks_esm).
- `vue-reactivity-3.5.45.global.js`: `@vue/reactivity@3.5.45`, `dist/reactivity.global.js`
  (unminified IIFE, defines `VueReactivity`).
