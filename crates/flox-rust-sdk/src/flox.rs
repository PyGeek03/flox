use std::{marker::PhantomData, path::PathBuf};

use crate::{
    actions::package::Package,
    environment::{self, build_flox_env},
    nix::{command_line::NixCommandLine, EvaluationArgs, FlakeArgs, NixApi, NixCommonArgs},
    prelude::Installable,
};
use anyhow::Result;

use derive_builder::Builder;
use runix::NixConfigBuilder;

/// The main API struct for our flox implementation
///
/// A [Flox] instance serves as the context for nix invocations
/// and possibly other tools such as git.
/// As a CLI application one invocation of `flox` would run on the same instance
/// but may call different methods.
///
/// [Flox] will provide a preconfigured instance of the Nix API.
/// By default this nix API uses the nix CLI.
/// Preconfiguration includes environment variables and flox specific arguments.
#[derive(Builder)]
pub struct Flox<Nix: NixApiExt> {
    /// The directory pointing to the users flox configuration
    ///
    /// TODO: set a default in the lib or CLI?
    config_dir: PathBuf,
    cache_dir: PathBuf,
    data_dir: PathBuf,

    /// Whether to collect metrics of any kind
    /// (yet to be made use of)
    #[builder(default)]
    collect_metrics: bool,

    /// Additional `nix` arguments
    ///
    /// TODO: Implementation detail, should go along with the nix Configurator
    #[builder(default)]
    extra_nix_args: Vec<String>,

    #[builder(setter(skip))]
    #[builder(default)]
    nix_marker: PhantomData<Nix>,
}

pub type DefaultFlox = Flox<NixCommandLine>;
pub type DefaultFloxBuilder = FloxBuilder<NixCommandLine>;

impl<Nix: NixApiExt> Flox<Nix> {
    pub fn package(&self, installable: Installable) -> Package<Nix> {
        Package::new(self, installable)
    }

    pub fn nix(&self) -> Result<Nix> {
        Nix::instance(self)
    }
}

pub trait NixApiExt: NixApi {
    fn instance(flox: &Flox<Self>) -> Result<Self>
    where
        Self: Sized;
}

impl NixApiExt for NixCommandLine {
    fn instance(_flox: &Flox<Self>) -> Result<Self> {
        let nix_config = NixConfigBuilder::default()
            .accept_flake_config(true.into())
            // .netrc_file() TODO
            .warn_dirty(false.into())
            .extra_experimental_features(
                ["nix-command", "flakes"].map(String::from).to_vec().into(),
            )
            .extra_substituters(
                ["https://cache.floxdev.com?trusted=1"]
                    .map(String::from)
                    .to_vec()
                    .into(),
            )
            .build()?;

        Ok(NixCommandLine::new(
            Some(environment::NIX_BIN.to_string()),
            build_flox_env()?,
            NixCommonArgs::default(),
            FlakeArgs::default(),
            EvaluationArgs::default(),
            nix_config,
        ))
    }
}
