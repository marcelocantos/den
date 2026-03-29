// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

namespace den {

class Cli {
  public:
    Cli();
    ~Cli();

    // Parse argv and run the matched command. Returns exit code.
    int run(int argc, char** argv);

  private:
    struct M;
    std::unique_ptr<M> m;
};

} // namespace den
