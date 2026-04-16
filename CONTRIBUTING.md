# Contributing to Tachyon

Thank you for your interest in contributing.

## Before opening a PR

- Open an issue first for non-trivial changes so we can align on a direction before you invest time writing code.
- For bug fixes, a minimal reproducer in the issue is required.

## Development setup

```bash
# Install dependencies
pipenv install --dev

# C++ core (Clang 21 + presets)
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --test-dir build/clang-debug/test --output-on-failure

# Python bindings
pipenv run pip install -e ./bindings/python/
pipenv run pytest bindings/python/tests/ -v

# Rust bindings
cd bindings/rust && cargo test

# Go bindings
cd bindings/go && bash ../../ci/vendor.sh go && go test ./tachyon/...
```

## Guidelines

**Code style**

- C++: `.clang-format` is enforced in CI. Run `clang-format -i` before committing.
- All other languages: formatters are enforced in CI (ruff for Python, prettier for Node.js, etc.).

**Commits**

- Follow [Conventional Commits](https://www.conventionalcommits.org): `feat(core):`, `fix(bindings/python):`, `docs:`,
  etc.
- The CHANGELOG is generated automatically from commit messages on release.

**Tests**

- Every bug fix must include a regression test.
- Every new API surface must include at least one unit test and one integration test.

**Benchmarks**

- Do not include benchmark numbers in PRs unless you ran on isolated hardware with `SCHED_FIFO` and core pinning.
  Uncontrolled numbers mislead more than they help.

**Bindings**

- A change to the C API (`tachyon.h`) requires updating all affected language bindings and their `.pyi` / GoDoc / type
  stubs in the same PR.
- ABI-breaking changes require a `TACHYON_VERSION` bump and an entry in `ABI.md`.

## License

By contributing, you agree that your contributions will be licensed under the [Apache 2.0 License](./LICENSE).
