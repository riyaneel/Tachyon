#!/bin/sh -eu

cmake -B build -S . \
	-DCMAKE_C_COMPILER="$CC" \
	-DCMAKE_CXX_COMPILER="$CXX" \
	-DCMAKE_C_FLAGS="$CFLAGS" \
	-DCMAKE_CXX_FLAGS="$CXXFLAGS" \
	-DTACHYON_LIB_FUZZING_ENGINE="$LIB_FUZZING_ENGINE" \
	-DTACHYON_ENABLE_FUZZING=ON \
	-DTACHYON_CFL_BUILD=ON \
	-DTACHYON_SANITIZER=none \
	-DCMAKE_BUILD_TYPE=Release \
	-G Ninja

cmake --build build --parallel "$(nproc)"

FUZZ_TARGETS="arena_rpc arena_rx arena_rx_batch arena_tx header_parser shm_attach toctou"

for target in $FUZZ_TARGETS; do
	cp "build/fuzz/tachyon_fuzz_$target" "$OUT/$target"

	cp "fuzz/dict/tachyon.dict" "$OUT/$target.dict"

	corpus_subdir=$target
	if [ -d "fuzz/corpus/$corpus_subdir" ]; then
		corpus_files=$(find "fuzz/corpus/$corpus_subdir" -maxdepth 1 -type f)
		if [ -n "$corpus_files" ]; then
			(cd "fuzz/corpus/$corpus_subdir" && zip -j "$OUT/${target}_seed_corpus.zip" ./*)
		fi
	fi
done
