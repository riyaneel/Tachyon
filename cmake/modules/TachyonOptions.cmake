include_guard(GLOBAL)

# LLVM
set(LLVM_VERSION "21" CACHE STRING "LLVM version to use")
set(TACHYON_MSAN_LLVM_VERSION "21" CACHE STRING "LLVM version used for MSan libc++")
set(TACHYON_MSAN_LIBCXX_DIR "${CMAKE_SOURCE_DIR}/.msan_toolchain/llvm-${TACHYON_MSAN_LLVM_VERSION}")

# GCC
set(GCC_VERSION "16" CACHE STRING "GCC version to use")

# Sanitizers
set(TACHYON_SANITIZER "asan_ubsan" CACHE STRING "Sanitizer preset: none | asan_ubsan | tsan | msan")
set_property(CACHE TACHYON_SANITIZER PROPERTY STRINGS none asan_ubsan tsan msan)

# Build
option(TACHYON_PORTABLE_BUILD "Disable -march=native for redistributable binaries" OFF)
option(TACHYON_ENABLE_TESTS "Enable tests" ON)
option(TACHYON_ENABLE_BENCH "Build benchmarks" ON)
option(TACHYON_ENABLE_FUZZING "Build libFuzzer harnesses (Clang only)" OFF)
option(TACHYON_ENABLE_TOP "Build tachyon-top CLI" OFF)
option(TACHYON_ENABLE_SECCOMP "Build seccomp BPF generators (Linux + libseccomp)" OFF)
set(TACHYON_LTO "fat" CACHE STRING "LTO mode: fat | thin | off")
