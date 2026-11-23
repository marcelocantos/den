# Standing-invariants hook for /cv (bullseye_convergence).
#
# Mirrors the gates CI enforces: configure-if-needed, build, test,
# format check, and a clean working tree.

BUILD_DIR := build
CMAKE_FLAGS := -G Ninja -DCMAKE_BUILD_TYPE=Release

.PHONY: bullseye configure build test format format-fix clean-tree harness-linux harness-macos soak-macos remote-check verify-release

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

# Formatting is pinned to one exact clang-format release, recorded in
# .clang-format-version (CI's Format job installs that version from PyPI:
# pip install clang-format==<version>). Other majors format differently,
# so with a missing binary or a different major the format targets warn
# and skip (exit 0) rather than fail or reformat: CI stays the authority.
# Point at another binary with: make format CLANG_FORMAT=/path/to/clang-format
CLANG_FORMAT_VERSION := $(shell cat .clang-format-version)
CLANG_FORMAT_PIN_MAJOR := $(firstword $(subst ., ,$(CLANG_FORMAT_VERSION)))
CLANG_FORMAT ?= clang-format
CLANG_FORMAT_MAJOR = $$($(CLANG_FORMAT) --version 2>/dev/null \
	| sed -n 's/.*clang-format version \([0-9][0-9]*\).*/\1/p' | head -n 1)
define clang_format_pinned
major=$(CLANG_FORMAT_MAJOR); \
if [ "$$major" != "$(CLANG_FORMAT_PIN_MAJOR)" ]; then \
	echo "⚠ $@ skipped — need clang-format $(CLANG_FORMAT_PIN_MAJOR) (pinned $(CLANG_FORMAT_VERSION) in .clang-format-version), '$(CLANG_FORMAT)' is $${major:+version }$${major:-not found}; CI checks formatting (set CLANG_FORMAT=/path/to/clang-format)"; \
	exit 0; \
fi
endef

format:
	@$(clang_format_pinned); \
	echo $(FORMAT_FILES) | xargs $(CLANG_FORMAT) --dry-run -Werror >/dev/null 2>&1 \
		&& echo "✓ format" \
		|| (echo "✗ format — run: make format-fix"; exit 1)

format-fix:
	@$(clang_format_pinned); \
	echo $(FORMAT_FILES) | xargs $(CLANG_FORMAT) -i && echo "✓ formatted $(words $(FORMAT_FILES)) files"

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

# End-to-end battery against a published release, GA or RC
# (RELEASE_TAG=vX.Y.Z or vX.Y.Z-rc.N; default: v<CMake version>), installed
# by install.sh into a scratch HOME/DEN_HOME/DEN_PREFIX with Homebrew off
# PATH. The LaunchAgent check uses a uniquely labelled agent that is removed
# afterwards. VR_* knobs: see scripts/verify-release.sh.
verify-release:
	scripts/verify-release.sh $(RELEASE_TAG)
