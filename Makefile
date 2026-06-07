# Standing-invariants hook for /cv (bullseye_convergence).
#
# Mirrors the gates CI enforces: configure-if-needed, build, test,
# format check, and a clean working tree.

BUILD_DIR := build
CMAKE_FLAGS := -G Ninja -DCMAKE_BUILD_TYPE=Release

.PHONY: bullseye configure build test format format-fix clean-tree harness-linux

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

# Mirror CMake's source-glob discipline: src/ is recursive, tests/ is
# top-level only — anything under tests/corpus/** (e.g. the
# homebrew-core submodule) is external and must not be reformatted.
FORMAT_FILES := $(shell find src \( -name '*.h' -o -name '*.cpp' \)) \
                $(shell find tests -maxdepth 1 \( -name '*.h' -o -name '*.cpp' \))

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

build/den: build
