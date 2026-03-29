// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "core/config.h"

#include <spdlog/spdlog.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    spdlog::info("den {} — not yet implemented", DEN_VERSION);
    return 0;
}
