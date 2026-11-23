# Contributing to den

Thank you for your interest in contributing to den!

## Building

den is C++23, built with CMake (3.24+) and Ninja. Everything compiles
into one `den_lib` static library, linked by the `den` binary and the
`den_tests` test runner.

```bash
git clone --recurse-submodules https://github.com/marcelocantos/den.git
cd den
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requirements:

- **macOS**: Xcode command-line tools, plus `cmake` and `ninja`.
- **Linux**: a C++23 compiler, `cmake`, `ninja-build`,
  `libcurl4-openssl-dev`, `zlib1g-dev`, `libbz2-dev` and `patchelf`.
- `clang-format` for the format check.

libarchive and its xz/zstd/lz4 codecs are built from pinned sources and
linked statically (`DEN_VENDOR_LIBARCHIVE=ON`, the default). The only
system library dependency is libcurl; `scripts/check-links.sh` checks
that the binary links nothing outside the base OS.

If you work in several git worktrees, see "Sharing the homebrew-core
test corpus across worktrees" in [README.md](README.md) to avoid one
1.4 GiB corpus clone per worktree.

## Local checks

`make bullseye` runs the same gates CI enforces: configure (if needed),
build, `ctest`, the `clang-format` check, and a clean working tree.
Run it before every push. `make format-fix` reformats the sources.

The Docker-based Linux smoke harness (`make harness-linux`) and the
macOS harnesses (`make harness-macos`, `make remote-check`) are not part
of `make bullseye`; CI runs the Linux harness on every push to master.

## Style

- Format C++ with the repository's `.clang-format` (`make format-fix`).
- Add tests (`tests/test_*.cpp`) for new behaviour.
- Keep commits focused, with descriptive messages.

## Maintainers: push straight to master

den uses a PR-free flow for maintainers. There are no pull requests per
fix; commits go straight to `master`:

1. Run `make bullseye` (or the cmake + ctest commands above) locally on
   macOS and make sure it passes.
2. `git push origin HEAD:master`.
3. Watch the CI runs that the push triggers (Check macos-15,
   Check ubuntu-latest, Format, Harness Linux) until they finish.
4. If master goes red, fix forward only: push a commit that fixes the
   failure (never force-push or rewrite master), and push nothing
   unrelated on top until master is green again.

## External contributors: pull requests welcome

If you don't have push access, fork the repository and open a pull
request against `master`. CI runs the same checks on pull requests.
Make sure `make bullseye` passes before you open it.

## Reporting issues

Use GitHub Issues for bug reports and feature requests. To report a
security vulnerability, see [SECURITY.md](SECURITY.md).

## Licence

By contributing, you agree that your contributions will be licensed
under the Apache License 2.0.
