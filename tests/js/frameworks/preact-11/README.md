# Preact 11.0.2 (ESM), vendored for the module-graph test

- Source: npm registry, `https://registry.npmjs.org/preact/-/preact-11.0.2.tgz`, downloaded 2026-11-08. The tarball's sha512 integrity matched the registry metadata.
- Files: `dist/preact.mjs` → `preact.mjs`, `hooks/dist/hooks.mjs` → `hooks/hooks.mjs`, `LICENSE` (MIT). They are unmodified.
- `hooks/hooks.mjs` imports the bare specifier `"preact"`, so the test's embedder loader must map `preact` to `preact.mjs`.
- `preact.mjs` contains regular-expression literals; the engine has its own RegExp, so the module test renders it.
- 11.0.2 was chosen over the newer 11.2.0 because 11.2.0 had been out for only two days.
