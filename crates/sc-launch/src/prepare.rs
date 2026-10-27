//! The steps before every launch, shared by `sc-launch run` and the launcher:
//! check the pack, ask its server about updates, refresh its game files,
//! check its patches, sync shared game settings, and build the command.

use crate::launch::{self, Setup};
use crate::packs::{self, Available};
use crate::remote::{self, SyncEvent};
use crate::trust::Trust;
use crate::{patching, profiles, state};
use sc_profile::Profile;
use std::path::{Path, PathBuf};
use std::process::Command;

/// What [`prepare`] reports on the way.
pub enum Event<'a> {
    /// Something the player should know; not an error.
    Note(String),
    /// The pack's server has a newer version (`required`: it no longer
    /// accepts this one, and the launch stops).
    Update(&'a Available),
    /// Game-file download progress (see [`remote::sync`]).
    Sync(SyncEvent<'a>),
    /// The game-file refresh finished, with its error if it failed.
    SyncDone(Option<&'a str>),
}

/// A profile ready to start.
pub struct Prepared {
    /// Starts sc-inject for the profile.
    pub command: Command,
    pub profile: Profile,
    pub trust: Trust,
    /// The shared copy of the game's settings; sync again after the game exits.
    pub shared: Option<PathBuf>,
}

/// Gets the profile in `dir` ready to start with `setup`. An error from
/// `events` stops it (say, a cancelled download).
pub fn prepare(
    setup: &Setup,
    dir: &Path,
    game_args: &[String],
    events: &mut dyn FnMut(Event<'_>) -> Result<(), String>,
) -> Result<Prepared, String> {
    let profile = profiles::load(dir)?;
    let trust = packs::verify_installed(dir)?;
    let pack = profiles::read_pack(dir)?;
    if let Some(warning) = packs::game_build_warning(&pack, &setup.game_exe()) {
        events(Event::Note(warning))?;
    }
    if pack.update.is_some() {
        match packs::check_update(&pack) {
            Ok(Some(found)) => {
                events(Event::Update(&found))?;
                if found.required {
                    return Err(format!(
                        "{}'s server needs pack version {} or newer (you have {}). Update the pack first.",
                        pack.name, found.version, pack.version
                    ));
                }
                events(Event::Note(format!("{} {} is available.", pack.name, found.version)))?;
            }
            Ok(None) => {}
            Err(e) => events(Event::Note(format!("Could not check {} for updates: {e}", pack.name)))?,
        }
    }
    let synced = remote::launch_sync(dir, &pack, trust.key().as_ref(), &mut |e| events(Event::Sync(e)));
    events(Event::SyncDone(synced.as_ref().err().map(String::as_str)))?;
    if let Some(note) = synced? {
        events(Event::Note(note))?;
    }
    for warning in patching::preflight(dir, &profile, &setup.game_exe())? {
        events(Event::Note(warning))?;
    }
    let shared = state::shared_local_appdata(setup);
    state::sync_shared(dir, &profile, shared.as_deref())?;
    let command = launch::command(setup, dir, game_args)?;
    Ok(Prepared { command, profile, trust, shared })
}
