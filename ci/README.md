# CI

Infrastructure scripts for building, testing, releasing, and measuring Tachyon.

| Directory  | Contents                                             | When invoked                         |
|------------|------------------------------------------------------|--------------------------------------|
| `bench/`   | Benchmark runner, percentile comparator, PGO builder | Manually or via `workflow_dispatch`  |
| `fuzz/`    | Corpus seed generator                                | Manually before committing new seeds |
| `release/` | Changelog generator                                  | Release workflow on `v*` tags        |
| `vendor/`  | Core C++ vendoring per binding                       | CI build jobs before compilation     |

## Quick reference

```bash
# Run the full benchmark suite (intra + inter + ZeroMQ baseline)
bash ci/bench/run.sh [build_dir] [output_dir]

# Compare two or more JSON bench result files
python3 ci/bench/compare.py baseline.json target.json [target2.json ...]

# Two-phase PGO build
bash ci/bench/pgo.sh [build_dir] [parallelism]

# Regenerate fuzz corpus seeds
python3 ci/fuzz/gen_seeds.py

# Generate CHANGELOG section for a release tag
python3 ci/release/gen_changelog.py v0.2.0

# Vendor C++ core into a language binding
bash ci/vendor/go.sh
bash ci/vendor/rust.sh
```
