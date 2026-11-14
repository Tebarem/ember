# Shipping a Vue app with Ember

This example ships a Vue 3 + [TanStack Query](https://tanstack.com/query) notes app as one executable. The same
steps work for any Vite app: React, Svelte, Solid, TanStack Router, or plain TypeScript.

```
examples/vue-app/
  web/                  the web app, an ordinary Vite project
    src/App.vue         useQuery / useMutation against the host's API
    src/api.js          fetch('api/notes') ... (relative URLs, no server)
    dist/               the build output (committed, so the C build needs no npm)
  main.c                the host: serves dist/ and the API, opens the window
cmake/EmberEmbedAssets.cmake   bakes dist/ into the executable with C23 #embed
```

## How it fits together

- **The page is served from `app://local/`.** There is no web server. Every `app://` URL the page asks for
  (`index.html`, the JS and CSS, and `fetch('api/...')` calls) goes to the resource handler that `main.c` registers
  for the `app` scheme. To call a real backend as well, allow its host in `main.c` and fetch it by absolute URL:

  ```c
  emb_runtime_net_allow(rt, "api.example.com");   /* nothing else on the network is reachable */
  ```

  ```js
  const res = await fetch('https://api.example.com/v1/items', { credentials: 'include' });
  ```

  There is no CORS check: an allowed host is called like a native app would call it.
- **The build is inside the binary.** `ember_embed_assets(vue_app web/dist)` writes a table of every file in `dist/`,
  each one pulled in with `#embed`. The handler finds a path in the table with `bsearch`. Nothing is read from disk
  at run time.
- **The native API is an ordinary `fetch`.** Requests to `app://local/api/...` are answered in C: `GET api/system`,
  `GET api/notes`, `POST api/notes`, `DELETE api/notes/<id>`. The handler sees the method, headers and body. The page
  uses plain `fetch`, so TanStack Query's caching, refetching and invalidation work exactly as on the web, and the
  same code runs against a real HTTP backend in a browser.
- **Persistence:**
  - The host saves the notes in the user's preference folder (`SDL_GetPrefPath`).
  - `emb_runtime_set_storage` keeps the page's `localStorage` across restarts.

## Building and shipping

1. Build the web app (needs Node 20+):

   ```sh
   cd examples/vue-app/web
   npm ci
   npm run build          # writes dist/
   ```

2. Build the executable:

   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DEMBER_MIN_SIZE=ON -DEMBER_TESTS=OFF
   cmake --build build --target vue_app
   ```

   `EMBER_MIN_SIZE` trades some speed for size (see the main README); leave it out for the fastest build.

   CMake picks up new or renamed files in `dist/` on the next build; Vite hashes its file names.

3. Ship `build/examples/vue_app` (`vue_app.exe` on Windows).
   - On Windows it needs only system DLLs.
   - Stripped (`strip vue_app.exe`) it is about 5.5 MB with `EMBER_MIN_SIZE`, or 10 MB without. About 108 KB of
     that is the app.
   - SDL3 is linked statically.

## Developing

Run `npm run watch` in `web/`, then start the host on the directory instead of the embedded copy:

```sh
build/examples/vue_app --dev examples/vue-app/web/dist
```

In `--dev` mode each request reads the file from disk. There is no hot reload yet, so restart the app after a
rebuild.

## Doing the same in your app

1. Make the Vite build use relative URLs (`base: './'`) and target `es2022` (see `web/vite.config.js`).
2. In CMake:

   ```cmake
   include(path/to/ember/cmake/EmberEmbedAssets.cmake)
   ember_embed_assets(my_app web/dist)
   ```

   Then include `my_app_assets.h` and serve `my_app_assets[]` from your handler (`serve()` in `main.c`).
3. Register the handler with `emb_runtime_set_resource_handler_v2(rt, "app", serve, ctx)` and load
   `app://local/index.html`.
4. Add API routes to the handler as needed.
   - For slow work (disk, network, a database), use `emb_runtime_set_resource_handler_async`: complete each request
     from a worker thread with `emb_resource_complete`, so the UI never blocks.
   - To push data into the page, use `emb_view_eval` (for example, to dispatch a `CustomEvent` on `window`), or
     `emb_view_post_task` from another thread.
