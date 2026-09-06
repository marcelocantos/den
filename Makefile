# Standing-invariants hook for /cv (bullseye_convergence).
#
# Mirrors the gates CI enforces: configure-if-needed, build, test,
# format check, and a clean working tree.

BUILD_DIR := build
CMAKE_FLAGS := -G Ninja -DCMAKE_BUILD_TYPE=Release

# macOS ships libarchive without headers, so CI passes Homebrew's prefix
# explicitly (.github/workflows/ci.yml). Do the same here, or `make gate`
# cannot configure a fresh tree on a Mac at all.
ifeq ($(shell uname -s),Darwin)
LIBARCHIVE_PREFIX := $(shell brew --prefix libarchive 2>/dev/null)
ifneq ($(LIBARCHIVE_PREFIX),)
CMAKE_FLAGS += -DCMAKE_PREFIX_PATH=$(LIBARCHIVE_PREFIX)
endif
endif

.PHONY: bullseye gate configure build test format format-fix clean-tree harness-linux harness-macos soak-macos remote-check

# The delivery gate: the repo's existing oracles on the shipped path.
# `scripts/hooks/pre-push` runs this before every push and refuses the push
# when it is red; CI runs the same steps after the fact.
gate: configure build test format

bullseye: gate clean-tree

configure:
	@if [ ! -f $(BUILD_DIR)/build.ninja ]; then \
		cmake -B $(BUILD_DIR) $(CMAKE_FLAGS); \
	fi
	@echo "✓ configure"

build: configure
	@cmake --build $(BUILD_DIR) >/dev/null && echo "✓ build"

# Quiet on success, but keep the failing output. Whoever acts on this exit
# code — /cv, CI, a pre-push gate — needs to know which test bit;
# "Errors while running CTest" on its own says nothing.
test: build
	@ctest --test-dir $(BUILD_DIR) --output-on-failure > $(BUILD_DIR)/ctest.log 2>&1 \
		&& echo "✓ tests" \
		|| (cat $(BUILD_DIR)/ctest.log; echo "✗ tests — see $(BUILD_DIR)/ctest.log"; exit 1)

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
