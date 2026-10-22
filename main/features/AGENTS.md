# Feature Packs

This subtree owns optional, compile-time feature packs. Before editing here:

1. Read `docs/custom/feature-development-guide.md` and `docs/custom/feature-packs.md`.
2. Keep each pack self-contained: implementation, `feature.json`, `README.md`, and pack-specific assets stay under `main/features/<id>/`.
3. Use `CONFIG_FEATURE_<ID>` and `#if CONFIG_...` guards. Host code must not reference a pack type unless the guard is active.
4. When a feature is disabled, do not compile its backend or create its pages. Keep shared navigation and provide stubs only for methods still called by core code.
5. Update all profiles explicitly (`base`, `english`, `stroke`, `keyboard`, `web_file_transfer`, `full`). `full` keeps every registered feature.
6. Build all profiles and verify symbols are absent on the disabled side and present on the enabled side.
7. Keep secrets, NVS dumps, generated assets, and build output out of Git.
8. Record behavior, size, symbol, and hardware-test results in `docs/custom/feature-packs.md`.
