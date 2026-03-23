// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

mod api;
mod bottle;
mod cli;
mod config;
mod env;
mod error;
mod formula;
mod keg;
mod link;
mod platform;
mod tab;

use clap::Parser;

use cli::Cli;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt::init();

    let cli = Cli::parse();
    cli.run().await
}
