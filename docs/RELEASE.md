# Release process (v1 track)

## Versioning (semver freeze)

- Versions live in `VERSION` (single line, `A.B.C`) and follow
  [semver](https://semver.org/spec/v2.0.0/): MAJOR = breaking IR/CLI/
  language change, MINOR = new feature, PATCH = fix.
- `CHANGELOG.md` (Keep a Changelog) is the source of truth for what
  changed; every PR adds an `Unreleased` entry. No changelog entry =
  no merge.
- `driver/VERSIONS` pins the LLVM toolchain + `RUNTIME_ABI` contract.
  Bumping either requires a MINOR version and a migration note.

## Signed tags

- Tags are `vA.B.C` and MUST be signed (`git tag -s vA.B.C -m "..."`).
  The tag message carries the `CHANGELOG.md` section for that version.
- Verify with `git tag -v vA.B.C`. CI publishes the SBOM
  (`python3 tools/sbom.py > sbom.json`) as a release artifact; the
  SBOM `documentVersion` must equal the tag.

## Release checklist

1. `bash tests/v1_gate_check.sh ./flintc --full` green (includes fmt,
   errors-catalog, decl-order, differential, driver, ladder 21,
   parse-gate/fixpoint/lexdiff corpora, tutorial).
2. Nightly green: perf-ratio gate within +10% (`bench/compile_time/`),
   100-seed fuzz with zero self-bugs (`fuzz/run.sh 1 100`).
3. `CHANGELOG.md`: move `Unreleased` to `[A.B.C] — YYYY-MM-DD`.
4. If codegen changed since the last release, bump the cache salt
   (`flintc-vNNN` in `src/main.cpp` fingerprint) so stale cached
   modules/binaries can never be reused.
5. Bump `VERSION`, regenerate `sbom.json`, attach both to the tag.
6. `git tag -s vA.B.C`, `git push --tags`.
