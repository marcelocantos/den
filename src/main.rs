// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

mod api;
mod bottle;
mod cask;
mod cli;
mod config;
mod daemon;
mod deps;
mod env;
mod error;
mod formula;
mod keg;
mod link;
mod manifest;
mod platform;
mod service;
mod settings;
mod tab;

use clap::Parser;

use cli::Cli;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt::init();

    let cli = Cli::parse();
    cli.run().await
}
