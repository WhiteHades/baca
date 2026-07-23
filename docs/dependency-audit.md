# dependency audit

reviewed 2026-07-23. review again by 2026-10-23 or before changing the pinned fff revision.

## reviewed inputs

- fff submodule: `05a35c6d4856455393a2dc8aadaeae4b2823ddf6`
- rust toolchain: `1.97.0`
- dependency lock: `vendor/fff/Cargo.lock`
- audit command: `cargo audit --file vendor/fff/Cargo.lock`
- reachability command: `cargo +1.97.0 tree --manifest-path vendor/fff/Cargo.toml -p fff-c --edges normal,build`

The upstream `main` branch was at the same fff revision when reviewed. No newer upstream revision was available.

## findings

`cargo audit` reports `RUSTSEC-2026-0176` and `RUSTSEC-2026-0177` for pyo3 0.24.2 in the workspace lockfile. pyo3 is not in the normal or build dependency graph of the shipped `fff-c` package, so neither advisory is reachable from `mereader-tui`.

The audit also reports these warnings:

- `RUSTSEC-2025-0141`: bincode 1.3.3 is unmaintained. It is present through heed. `mereader-tui` passes null fff frecency and query-history database paths, so fff does not open either heed-backed database.
- `RUSTSEC-2026-0183` and `RUSTSEC-2026-0184`: git2 0.20.4 has unsound `Remote::list()` and buffer-created blame APIs. Neither affected API is called by the production fff source.
- `RUSTSEC-2026-0097`: rand 0.8.5 can be unsound with a custom logger and `rand::rng()`. This version is build-only through phf, doxygen-rs, lmdb-master-sys, and heed; the affected runtime combination is absent.

No reported high- or critical-severity advisory is reachable from the shipped binary. The warnings above are accepted only for this pinned revision and must be reassessed by the review date.
