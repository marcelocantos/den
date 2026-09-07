# Standing-invariants hook for /cv (bullseye_convergence).
#
# Mirrors the gates CI enforces: configure-if-needed, build, test,
# format check, and a clean working tree.

BUILD_DIR := build
CMAKE_FLAGS := -G Ninja -DCMAKE_BUILD_TYPE=Release

.PHONY: bullseye configure build test format format-fix clean-tree harness-linux harness-macos soak-macos remote-check bench bench-lock bench-gate bench-gate-loose

bullseye: configure build test format clean-tree

configure:
	@if [ ! -f $(BUILD_DIR)/build.ninja ]; then \
		cmake -B $(BUILD_DIR) $(CMAKE_FLAGS); \
	fi
	@echo "✓ configure"

build: configure
	@cmake --build $(BUILD_DIR) >/dev/null && echo "✓ build"

test: build
	@ctest --test-dir $(BUILD_DIR) --output-on-failure >/dev/null && echo "✓ tests"

# 🎯T81: benchmarks for the index load nine CLI callbacks pay for,
# locked both ways.
#
#   make bench             run them and print the results
#   make bench-lock        make this run the new baseline
#   make bench-gate        compare against docs/perf/baseline.txt;
#                          fails on a regression AND on an improvement,
#                          because an improvement means the baseline no
#                          longer describes the code and must be
#                          re-locked in the same commit
#   make bench-gate-loose  compare only the counted metrics
#
# Timings are only comparable on the machine the baseline was recorded
# on (docs/perf/baseline.md names it). Elsewhere, and in CI, use
# bench-gate-loose, which compares just the counted metrics — packages
# loaded and index bytes — which are identical everywhere.
bench: build
	@$(BUILD_DIR)/den_bench

bench-lock: build
	@$(BUILD_DIR)/den_bench --lock

bench-gate: build
	@$(BUILD_DIR)/den_bench --gate

bench-gate-loose: build
	@$(BUILD_DIR)/den_bench --gate --loose

# Mirror CMake's source-glob discipline: src/ is recursive, tests/ is
# top-level only — anything under tests/corpus/** (e.g. the
# homebrew-core submodule) is external and must not be reformatted.
FORMAT_FILES := $(shell find src \( -name '*.h' -o -name '*.cpp' \)) \
                $(shell find tests -maxdepth 1 \( -name '*.h' -o -name '*.cpp' \)) \
                $(shell find bench \( -name '*.h' -o -name '*.cpp' \))

format:
	@echo $(FORMAT_FILES) | xargs clang-format --dry-run -Werror >/dev/null 2>&1 \
		&& echo "✓ format" \
		|| (echo "✗ format — run: make format-fix"; exit 1)

format-fix:
	@echo $(FORMAT_FILES) | xargs clang-format -i && echo "✓ formatted $(words $(FORMAT_FILES)) files"

clean-tree:
	@test -z "$$(git status --porcelain)" \
		&& echo "✓ clean" \
		|| (echo "✗ dirty tree"; git status --short; exit 1)

# Docker-based Linux smoke harness — not part of bullseye (Docker is not a
# guaranteed local dependency; CI runs this separately).
harness-linux: build/den
	tests/harness/linux/run.sh --binary build/den

# SSH-based macOS smoke harness — not part of bullseye.
# Requires a configured SSH alias (den-test-mac by default).
# See tests/harness/macos/README.md for one-time setup.
# Not in CI — runs are dev-triggered only.
harness-macos: build/den
	tests/harness/macos/run.sh --binary build/den

# Real ~/.den soak on den-test-mac (migrate, install, run binaries, shell-env).
soak-macos: build/den
	tests/harness/macos/run-soak.sh --binary build/den

# Full remote conviction: isolated harness + real-home soak.
remote-check: build/den
	scripts/remote-check.sh

build/den: build
