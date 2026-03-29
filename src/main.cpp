// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "cli/cli.h"

int main(int argc, char** argv) {
    den::Cli cli;
    return cli.run(argc, argv);
}
