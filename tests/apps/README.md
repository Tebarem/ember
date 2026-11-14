# Framework apps

Small TodoMVC apps in five frameworks, a router app and a plain TypeScript app, built with Vite for the
`app://` scheme of Ember. The apps are built unmodified: each `dist/` is what `vite build` prints, with
`base: './'`, `build.target: 'es2022'` and minification on. The Ember tests load the `dist/` folders; the
sources are here so the builds can be reproduced.

| App | Framework | Versions (from `package-lock.json`) |
|-----|-----------|-------------------------------------|
| `preact-todomvc` | Preact, hooks | preact 11.2.0, @preact/preset-vite 2.10.6 |
| `react-todomvc` | React 19, `createRoot`, reducer | react 19.3.1, react-dom 19.3.1, @vitejs/plugin-react 6.3.0 |
| `vue-todomvc` | Vue 3 SFC, `<script setup>` | vue 3.5.45, @vitejs/plugin-vue 6.0.10 |
| `svelte-todomvc` | Svelte 5, runes, `fade`/`slide` transitions | svelte 5.57.3, @sveltejs/vite-plugin-svelte 7.3.1 |
| `solid-todomvc` | Solid, signals | solid-js 1.9.18, vite-plugin-solid 2.11.15 |
| `react-router` | React 19, react-router 8 data mode | react-router 8.5.1, react 19.3.1, @vitejs/plugin-react 6.3.0 |
| `vite-vanilla` | vanilla TypeScript (plain TypeScript) | typescript 7.0.2 (`tsc --noEmit`) |

All of them use vite 8.3.7. The versions are the current majors on npm on 2026-11-09 (Preact 11 and
react-router 8; Preact 10 and react-router 7 are the previous majors).

## Shared pieces

- `shared/todo.css`: the one stylesheet of the five TodoMVC apps (written for Ember, not copied from
  todomvc-app-css). The apps import it as `@shared/todo.css`; each `vite.config` aliases `@shared` to
  `../shared`, which is `tests/apps/shared` here and `$OUT/shared` when `tools/apps/build.sh` builds them.
- Every TodoMVC app has the same markup and class names (`.todoapp`, `.new-todo`, `.todo-list li.completed`,
  `.todo-count`, `.filters a.selected`, `.clear-completed`, ...), so one scenario drives all five.
- Storage: `localStorage` key `todos-<framework>`, the TodoMVC item shape `{id, title, completed}`. Filters
  are the hash routes `#/`, `#/active`, `#/completed`.
- `react-router` loads `data/users.json` with a `fetch` built from the document's base URL at start-up
  (relative fetches made after a `pushState` would resolve against the new path).

## Each app's folder

`package.json`, `package-lock.json` (the lockfile decides every version), `vite.config.*`, `index.html`,
`src/`, `public/` where there is one, `LICENSE` (the framework's MIT license, copied from node_modules), and
`dist/` (the build output). `node_modules` is never committed.

## Rebuilding

`tools/apps/build.sh` runs on a Linux build machine with Node 22 and npm. From a copy of this tree:

    [SRC=<tree>] [OUT=<work dir>] tools/apps/build.sh [app...]

It copies each app to `$OUT/<name>` (default `OUT=/opt/ember/apps`), runs `npm ci --ignore-scripts` (or `npm install
--ignore-scripts` for an app without a lockfile) and `npm run build` as user `nobody`, checks that no `dist/`
loads an external URL, and copies `dist/`, `package-lock.json` and `LICENSE` back into `SRC`. The builds are
deterministic: rerunning the script on the same lockfiles reproduces every `dist/` byte for byte.

## Sizes (dist/, bytes, as built)

| App | dist bytes |
|-----|-----------|
| preact-todomvc | 21696 |
| react-todomvc | 228019 |
| vue-todomvc | 74313 |
| svelte-todomvc | 50411 |
| solid-todomvc | 21353 |
| react-router | 318332 |
| vite-vanilla | 2955 |

## Canary

`tests/unit/embed/test_embed_apps.c` loads every built app from `app://local/` through an offscreen view and
prints a report (loaded, painted, errors, first error, missing APIs). It is strict by default: every app must be built, load, paint and log no error, and every scenario step must pass. `EMBER_APPS_STRICT=0` turns it into a report that always passes.
