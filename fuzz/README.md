# Fuzzing

Requires Clang. libFuzzer is not available under GCC.

## Build

```bash
cmake -S . -B cmake-build-fuzz \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DTACHYON_ENABLE_FUZZING=ON

cmake --build cmake-build-fuzz \
    --target tachyon_fuzz_arena_rx tachyon_fuzz_arena_rx_batch tachyon_fuzz_header_parser
```

## Run

```bash
cd cmake-build-fuzz

./fuzz/tachyon_fuzz_arena_rx ../../fuzz/corpus/arena_rx \
    -dict=../../fuzz/dict/tachyon.dict -max_total_time=300

./fuzz/tachyon_fuzz_arena_rx_batch ../../fuzz/corpus/arena_rx_batch \
    -dict=../../fuzz/dict/tachyon.dict -max_total_time=300

./fuzz/tachyon_fuzz_header_parser ../../fuzz/corpus/header_parser \
    -dict=../../fuzz/dict/tachyon.dict -max_total_time=300
```

## Regression mode

Replays the existing corpus only. No new inputs are generated. Use this in CI.

```bash
./fuzz/tachyon_fuzz_arena_rx       ../../fuzz/corpus/arena_rx       -runs=0
./fuzz/tachyon_fuzz_arena_rx_batch ../../fuzz/corpus/arena_rx_batch -runs=0
./fuzz/tachyon_fuzz_header_parser  ../../fuzz/corpus/header_parser  -runs=0
```

## Reproducing a crash

libFuzzer writes the crashing input to `fuzz/corpus/crashes/` (not tracked by git).

```bash
# Reproduce
./fuzz/tachyon_fuzz_arena_rx ../../fuzz/corpus/crashes/<file>

# Minimize
./fuzz/tachyon_fuzz_arena_rx -minimize_crash=1 \
    -exact_artifact_path=../../fuzz/corpus/crashes/<file>_min \
    ../../fuzz/corpus/crashes/<file>
```

## Targets

| Target                | Entry point                            | Notes                                                |
|-----------------------|----------------------------------------|------------------------------------------------------|
| `fuzz_arena_rx`       | `acquire_rx` / `commit_rx`             | Drain loop, SKIP_MARKER, wrap-around                 |
| `fuzz_arena_rx_batch` | `acquire_rx_batch` / `commit_rx_batch` | `current_tail` accumulation, `reserved_size=0` stall |
| `fuzz_header_parser`  | `acquire_rx` (single call)             | Isolated header parsing, double-SKIP path            |

`transport_uds.cpp`, `Arena::format()`, and language bindings are out of scope.

## Dictionary

libFuzzer prints recommended tokens at the end of a run. Add them in hex to `fuzz/dict/tachyon.dict` and commit.

## Adding a target

1. Write `fuzz/src/fuzz_<name>.cpp` with `LLVMFuzzerTestOneInput`.
2. Add `tachyon_fuzz_target(tachyon_fuzz_<name> src/fuzz_<name>.cpp)` to `fuzz/CMakeLists.txt`.
3. Create `fuzz/corpus/<name>/` and add seeds to `scripts/gen_fuzz_seeds.py`.
4. Commit corpus and updated dict.
