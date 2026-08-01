# NativeDisc cold-build stress fixture

`write_native_disc_cold_build_stress.py` creates a public, deterministic and
retail-free three-track Dreamcast GDI using only the Python standard library.
The disc contains a synthetic SH-4 boot program, exact function boundaries,
real seed-wave declarations, a complete boot-partition plan and hash/extent
bound ExactOnly modules. It contains no synthetic cache-request table, fake
translation-unit plan or artificial head-of-line weight.

Two profiles are part of contract v2:

| Profile | Functions | Blocks | Roots | Seed waves | Exact modules |
| --- | ---: | ---: | ---: | ---: | ---: |
| `smoke` | 16 | 224 | 14 | 4 | 2 |
| `reference` | 1,600 | 22,400 | 1,400 | 8 | 32 |

The pinned v2 manifest digests are
`1b5a24bb35471d1aea0a09900c48dcd9ccb85a252c9073600a9484ee92a28359`
for `smoke` and
`f33809788bbea61e7b5f9934a244c4bca00a3b722f5a0a2defdb418530dc83ec`
for `reference`. Writer and independent verifier reject byte-level drift.

Generate and verify the quick profile:

```powershell
python -B tools/performance/write_native_disc_cold_build_stress.py `
  --profile smoke --output build/kr4974-smoke
python -B tools/performance/verify_native_disc_cold_build_stress.py `
  build/kr4974-smoke --profile smoke
```

Reproducibility is checked by generating a second empty directory and passing
it through `--compare`. `--katana-cli` additionally runs the built product's
real `disc-audit --json` path.

`katana-native-disc-cold-build-stress` consumes the fixture through Katana's
production paths. For every declared wave it grows the real
`FunctionBoundary` set and invokes FunctionValue analysis with the same
persistent session and real cached evaluation artifacts. A final identical
replay must be miss-free. The reported logical evaluations, cache lookups,
hits, misses and physical evaluations come only from live FVA progress and
session ledgers; no fixed hit count is part of the fixture contract.

The runner also byte-validates the complete `PARTS.JSN` plan against production
boot partitioning, proves every ExactOnly module/entry/source binding through
real latent analysis, exports the product port and binds
`PortExportResult.partitions` to the actually emitted `unit-v*.cpp` files and
their generated CMake source list. The CTest wrapper streams every subprocess,
builds that port with the configured compiler and worker budget, and finally
proves the generated-source target is linked into a non-empty host executable.

The focused smoke gate is:

```powershell
ctest --test-dir build --output-on-failure `
  -R '^katana-native-disc-cold-build-stress-tests$'
```

For a manual reference measurement, generate the `reference` profile, run the
stress executable with an empty port-output directory, and pass the intended
worker count. The terminal v2 JSON reports the observed real work and the
actual exported partition/TU counts; it makes no predetermined logical-work or
cache-hit claim. Generated fixture directories are build artifacts and are not
checked into the repository.
