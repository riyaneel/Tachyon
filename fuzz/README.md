# Fuzzing

Requires Clang. libFuzzer is not available under GCC.

## Build

```bash
cmake --preset fuzz
cmake --build --preset fuzz --parallel
```

## Seed generation

```bash
python ci/fuzz/gen_seeds.py
```

## Run

```bash
./build/fuzz/fuzz/tachyon_fuzz_arena_rpc fuzz/corpus/arena_rpc -dict=fuzz/dict/tachyon.dict -max_total_time=300
./build/fuzz/fuzz/tachyon_fuzz_arena_rx fuzz/corpus/arena_rx -dict=fuzz/dict/tachyon.dict -max_total_time=300
./build/fuzz/fuzz/tachyon_fuzz_arena_rx_batch fuzz/corpus/arena_rx_batch -dict=fuzz/dict/tachyon.dict -max_total_time=300
./build/fuzz/fuzz/tachyon_fuzz_arena_tx fuzz/corpus/arena_tx -dict=fuzz/dict/tachyon.dict -max_total_time=300
./build/fuzz/fuzz/tachyon_fuzz_header_parser fuzz/corpus/header_parser -dict=fuzz/dict/tachyon.dict -max_total_time=300
./build/fuzz/fuzz/tachyon_fuzz_shm_attach fuzz/corpus/shm_attach -dict=fuzz/dict/tachyon.dict -max_total_time=300
./build/fuzz/fuzz/tachyon_fuzz_star fuzz/corpus/star -dict=fuzz/dict/tachyon.dict -max_total_time=300
./build/fuzz/fuzz/tachyon_fuzz_toctou fuzz/corpus/toctou -dict=fuzz/dict/tachyon.dict -max_total_time=300
```

## Regression mode

Replays the existing corpus only. No new inputs are generated. Use this in CI.

```bash
./build/fuzz/fuzz/tachyon_fuzz_arena_rpc fuzz/corpus/arena_rpc -runs=0
./build/fuzz/fuzz/tachyon_fuzz_arena_rx fuzz/corpus/arena_rx -runs=0
./build/fuzz/fuzz/tachyon_fuzz_arena_rx_batch fuzz/corpus/arena_rx_batch -runs=0
./build/fuzz/fuzz/tachyon_fuzz_arena_tx fuzz/corpus/arena_tx -runs=0
./build/fuzz/fuzz/tachyon_fuzz_header_parser fuzz/corpus/header_parser -runs=0
./build/fuzz/fuzz/tachyon_fuzz_shm_attach fuzz/corpus/shm_attach -runs=0
./build/fuzz/fuzz/tachyon_fuzz_star fuzz/corpus/star -runs=0
./build/fuzz/fuzz/tachyon_fuzz_toctou fuzz/corpus/toctou -runs=0
```

## Reproducing a crash

libFuzzer writes the crashing input to the current directory as `crash-<sha1>` (not tracked by git).
Move it to `fuzz/corpus/crashes/` before minimizing.

```bash
# Reproduce
./build/fuzz/fuzz/tachyon_fuzz_<target> fuzz/corpus/crashes/<file>

# Minimize
./build/fuzz/fuzz/tachyon_fuzz_<target> -minimize_crash=1 \
    -exact_artifact_path=fuzz/corpus/crashes/<file>_min \
    fuzz/corpus/crashes/<file>
```

## Targets

| Target                | Entry point                                | Notes                                                           |
|-----------------------|--------------------------------------------|-----------------------------------------------------------------|
| `fuzz_arena_rpc`      | `commit_tx_rpc` / `acquire_rx_rpc`         | RpcPackedMeta offsets, `correlation_id` edge cases              |
| `fuzz_arena_rx`       | `acquire_rx` / `commit_rx`                 | Drain loop, SKIP_MARKER, wrap-around                            |
| `fuzz_arena_rx_batch` | `acquire_rx_batch` / `commit_rx_batch`     | `current_tail` accumulation, `reserved_size=0` stall            |
| `fuzz_arena_tx`       | `acquire_tx` / `commit_tx` / `rollback_tx` | Corrupted `tail` index, integer overflow on space calc          |
| `fuzz_header_parser`  | `acquire_rx` (single call)                 | Isolated header parsing, double-SKIP path                       |
| `fuzz_shm_attach`     | `Arena::attach` / `acquire_rx`             | Header validation, `capacity_mask_` near-end OOB                |
| `fuzz_star`           | `StarBus::create` / `poll`                 | N-spoke round-robin drain, TX path OOB, refcount teardown order |
| `fuzz_toctou`         | `acquire_rx` with `__builtin_trap`         | Integer underflow, misalignment, bounds invariants              |

`transport_uds.cpp`, `Arena::format()`, and language bindings are out of scope.

## Dictionary

libFuzzer prints recommended tokens at the end of a run. Add them in hex to `fuzz/dict/tachyon.dict` and commit.

## Adding a target

1. Write `fuzz/src/fuzz_<name>.cpp` with `LLVMFuzzerTestOneInput`.
2. Add `tachyon_fuzz_target(tachyon_fuzz_<name> src/fuzz_<name>.cpp)` to `fuzz/CMakeLists.txt`.
3. Create `fuzz/corpus/<name>/` and add seeds to `ci/fuzz/gen_seeds.py`.
4. Run `python3 ci/fuzz/gen_seeds.py` from the repo root to generate seed files.
5. Commit corpus and updated dict.

---

## ClusterFuzzLite

ClusterFuzzLite (CFL) runs the same libFuzzer harnesses in GitHub Actions using the OSS-Fuzz infrastructure. The
configuration lives in `.clusterfuzzlite/`.

### How CI fuzzing differs from regression mode

The regular CI job (`fuzz.yml`) runs the harnesses with `-runs=0`: it replays the committed corpus and fails if any seed
crashes. It never generates new inputs and never stores state between runs.

CFL is different. It maintains a **persistent corpus** stored as a GitHub Actions cache keyed by sanitizer. Each run
loads the previous corpus, fuzzes for a bounded time window, and writes new interesting inputs back. Crashes are
surfaced as workflow failures with the reproducer uploaded as an artifact.

### Workflow files

| File                  | Trigger                                  | Purpose                                                     |
|-----------------------|------------------------------------------|-------------------------------------------------------------|
| `cfl_batch.yml`       | push to `development`, schedule every 6h | Fuzzes all 3 sanitizers for 600s each                       |
| `cfl_ci.yml`          | pull_request to `main` / `development`   | Fuzzes all 3 sanitizers for 300s each in `code-change` mode |
| `cfl_maintenance.yml` | schedule Sunday 03:00 + 04:00 UTC        | Prunes corpus (03:00) and generates coverage report (04:00) |

### Sanitizers

Three sanitizers run in parallel via a matrix:

| Sanitizer   | Detects                                                                |
|-------------|------------------------------------------------------------------------|
| `address`   | Buffer overflows, use-after-free, heap/stack OOB                       |
| `memory`    | Uninitialized reads (requires MSan-instrumented libc++)                |
| `undefined` | UB: signed overflow, shift, null deref, misaligned access, vptr errors |

Each sanitizer maintains its own independent corpus cache.

### Modes

- **`batch`**: primary fuzzing mode. Runs on every push to `development` and every 6 hours via schedule. Generates new
  corpus entries and saves them. Cancel-in-progress is disabled, so a push mid-fuzz does not abort the current batch.

- **`code-change`**: runs on pull requests. The same mechanism as batch but shorter (300s) and cancel-in-progress is
  enabled, so a force-push cancels the stale run immediately.

- **`prune`**: reduces the corpus by removing redundant inputs that cover the same edges. Runs weekly (Sunday 03:00
  UTC). Should not be canceled mid-run to avoid corpus corruption.

- **`coverage`**: builds with `-fsanitize=coverage`, fuzzes, and generates an HTML coverage report uploaded as a GitHub
  Actions artifact (`coverage-report`, retained 7 days). Runs weekly (Sunday 04:00 UTC) after the prune has completed.

### Retrieving a crash from CI

When CFL detects a crash, the workflow fails and the reproducer is uploaded as a GitHub Actions artifact named
`crash-<target>-<sanitizer>` on the failing run.

```
GitHub Actions -> failing run -> Summary -> Artifacts -> crash-<target>-<sanitizer>.zip
```

Download, extract, then reproduce locally:

```bash
# Build the fuzz target locally with the same sanitizer
cmake --preset fuzz # ASan by default (presets: fuzz / fuzz-ubsan / fuzz-msan / fuzz-tsan) 
cmake --build --preset fuzz --parallel

# Reproduce
./build/fuzz/fuzz/tachyon_fuzz_<target> <crash-file>

# Minimize before committing as a regression seed
./build/fuzz/fuzz/tachyon_fuzz_<target> -minimize_crash=1 \
    -exact_artifact_path=fuzz/corpus/crashes/<crash-file>_min \
    <crash-file>
```

### Adding a seed after fixing a crash

Once the bug is fixed, add the minimized reproducer as a permanent regression seed so CFL never
regresses on it:

```bash
# Copy the minimized input into the target's corpus directory
cp fuzz/corpus/crashes/<crash-file>_min fuzz/corpus/<target>/

# Verify it no longer crashes
./build/fuzz/fuzz/tachyon_fuzz_<target> fuzz/corpus/<target>/<crash-file>_min

# Commit
git add fuzz/corpus/<target>/<crash-file>_min
git commit -m "fuzz: add regression seed for <bug description>"
```

The seed will be included in `<target>_seed_corpus.zip` on the next CFL build and replayed in every subsequent run in
regression mode.

### Testing locally with infra/helper.py

Local testing uses `infra/helper.py` from the OSS-Fuzz repository with the `--external` flag for CFL projects.

```bash
# Clone OSS-Fuzz once
git clone --depth=1 https://github.com/google/oss-fuzz.git
cd oss-fuzz

# Build the Docker image (reads .clusterfuzzlite/Dockerfile)
python3 infra/helper.py build_image --external /path/to/tachyon

# Build fuzz targets for a given sanitizer
python3 infra/helper.py build_fuzzers --external /path/to/tachyon --sanitizer address
python3 infra/helper.py build_fuzzers --external /path/to/tachyon --sanitizer memory
python3 infra/helper.py build_fuzzers --external /path/to/tachyon --sanitizer undefined

# Validate the build
python3 infra/helper.py check_build --external /path/to/tachyon --sanitizer address

# Run a single fuzzer
python3 infra/helper.py run_fuzzer --external /path/to/tachyon --sanitizer address arena_rpc

# Reproduce a crash
python3 infra/helper.py reproduce --external /path/to/tachyon arena_rpc /path/to/crash-file
```

Built binaries land in `oss-fuzz/build/out/tachyon/`. The `$PROJECT_NAME` is derived from the root directory name of
your checkout.

**Note on memory sanitizer**: MSan requires a libc++ instrumented with `-fsanitize=memory`. The CFL base image provides
this automatically. Locally, build the instrumented libc++ first:

```bash
bash ci/build_msan_libcxx.sh 21
```

Then use `cmake --preset fuzz-msan`.
