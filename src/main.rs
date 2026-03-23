// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

mod cli;
mod config;
mod error;

use clap::Parser;

use cli::Cli;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt::init();

    let cli = Cli::parse();
    cli.run().await
}
