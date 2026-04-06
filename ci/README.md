# CI

Infrastructure scripts for building, testing, releasing, and measuring Tachyon.

| Directory/File         | Contents                                           | When invoked                                        |
|------------------------|----------------------------------------------------|-----------------------------------------------------|
| `bench/`               | Benchmark runner, percentile comparator, PGO build | Manually                                            |
| `fuzz/`                | Corpus seed generator                              | Manually before committing new seeds                |
| `release/`             | Changelog generator                                | Release workflow on `v*` tags                       |
| `build_msan_libcxx.sh` | Build MSan instrumented libcxx                     | Development or CI build jobs before compilation     | 
| `install_llvm.sh`      | Install llvm toolchain                             | Development cfg or CI build jobs before compilation |
| `vendor.sh`            | Core C++ vendoring per binding                     | CI build jobs before compilation                    |

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
bash ci/vendor.sh <target> # targets: go, java, rust

# Build MSan instrumented libcxx
bash ci/build_msan_libcxx.sh [version] # default: 21

# Install llvm toolchain
bash ci/install_llvm.sh [version] # default: 21
```
