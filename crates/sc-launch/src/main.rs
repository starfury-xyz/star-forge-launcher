use sc_launch::{launch, packs, profiles, signing, trust::Trust};
use std::path::PathBuf;
use std::process::ExitCode;

const USAGE: &str = "\
sc-launch - Star Conflict launcher with switchable profiles

USAGE:
  sc-launch [OPTIONS] list              List profiles
  sc-launch [OPTIONS] info              Show the detected game, tools and Proton setup
  sc-launch [OPTIONS] run <profile> [-- <game args>]
                                        Start the game with a profile (game args
                                        replace the profile's own)
  sc-launch [OPTIONS] install <zip|folder|url> [--yes]
                                        Install a server pack, or update the
                                        profile that already has it
  sc-launch [OPTIONS] updates           Check installed packs for new versions
  sc-launch [OPTIONS] update <profile> [--yes]
                                        Install the newest version of a profile's pack

FOR SERVER TEAMS:
  sc-launch pack check <zip|folder|url> Show what a pack would do and check it the
                                        way the launcher does, without installing
  sc-launch pack keygen <key file>      Make a pack signing key (keep it secret)
  sc-launch pack files <folder> <list> [--base-url <url>] [--key <key file>]
                                        Write a file list for the game files in
                                        <folder> (laid out like the game folder):
                                        <list> ending in .txt is SCMOD1 (needs
                                        --base-url), otherwise STAR FORGE JSON. With
                                        a key, also writes <list>.sig
  sc-launch pack sign <folder> --key <key file>
                                        Write manifest.json and manifest.sig for the
                                        pack in <folder>; the key can also come from
                                        STAR_FORGE_PACK_KEY (the key file's text)

OPTIONS:
  --profiles-dir <dir>   Profile folder (default: the per-user data folder)
  --game-dir <dir>       Star Conflict install (default: found through Steam)
  --tools-dir <dir>      Folder with sc-inject.exe and sc_agent.dll (default: next to sc-launch)
  --proton <dir>         Proton build to use (Linux; default: the one that last ran the game)
  --yes                  Install without asking for confirmation
  --key <file>           Pack signing key, for pack sign and pack files
  --base-url <url>       Where the files of a pack files list will be hosted

ENVIRONMENT (used when the matching option is not given):
  STAR_FORGE_GAME_DIR, STAR_FORGE_TOOLS_DIR, STAR_FORGE_PROTON, STAR_FORGE_DATA_DIR
  Development builds also read them from the repo's .env file.
";

struct Cli {
    opts: launch::Options,
    profiles_dir: Option<PathBuf>,
    command: Vec<String>,
    game_args: Vec<String>,
    yes: bool,
    key: Option<PathBuf>,
    base_url: Option<String>,
}

fn parse() -> Result<Cli, String> {
    let mut cli = Cli {
        opts: launch::Options::default(),
        profiles_dir: None,
        command: Vec::new(),
        game_args: Vec::new(),
        yes: false,
        key: None,
        base_url: None,
    };
    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        let mut value = |name: &str| it.next().map(PathBuf::from).ok_or(format!("{name} needs a value"));
        match a.as_str() {
            "--profiles-dir" => cli.profiles_dir = Some(value(&a)?),
            "--game-dir" => cli.opts.game_dir = Some(value(&a)?),
            "--tools-dir" => cli.opts.tools_dir = Some(value(&a)?),
            "--proton" => cli.opts.proton = Some(value(&a)?),
            "--key" => cli.key = Some(value(&a)?),
            "--base-url" => cli.base_url = Some(value(&a)?.to_string_lossy().into_owned()),
            "-h" | "--help" => cli.command = vec!["help".into()],
            "--yes" | "-y" => cli.yes = true,
            "--" => {
                cli.game_args = it.by_ref().collect();
            }
            _ if a.starts_with("--") => return Err(format!("unknown option {a}")),
            _ => cli.command.push(a),
        }
    }
    Ok(cli)
}

fn run() -> Result<u8, String> {
    let cli = parse()?;
    let profiles_dir = cli
        .profiles_dir
        .clone()
        .or_else(profiles::default_profiles_dir)
        .ok_or("cannot work out a profiles folder; pass --profiles-dir")?;

    match cli.command.iter().map(String::as_str).collect::<Vec<_>>().as_slice() {
        [] | ["help"] => {
            print!("{USAGE}");
            Ok(0)
        }
        ["list"] => {
            let entries = profiles::list(&profiles_dir);
            if entries.is_empty() {
                println!("no profiles in {}", profiles_dir.display());
            }
            for e in entries {
                match e.profile {
                    Ok(p) => println!(
                        "{:<20} {:<24} {:<13} {} patch(es)  {}",
                        e.name,
                        p.server.as_deref().unwrap_or("-"),
                        if p.local_server { "local server" } else { "client only" },
                        sc_launch::patching::summary(&e.dir, &p).patches,
                        p.name
                    ),
                    Err(err) => println!("{:<20} INVALID: {err}", e.name),
                }
            }
            Ok(0)
        }
        ["info"] => {
            println!("profiles: {}", profiles_dir.display());
            let setup = launch::resolve(&cli.opts)?;
            println!("game:     {}", setup.game_dir.display());
            println!("tools:    {}", setup.tools_dir.display());
            #[cfg(unix)]
            {
                let p = &setup.proton;
                println!("proton:   {}", p.dir.display());
                println!("runtime:  {} (Proton runs inside this container)", p.runtime.display());
                println!("prefix:   {}", p.compat_data.display());
            }
            Ok(0)
        }
        ["run", name] => {
            if !profiles::valid_name(name) {
                return Err(format!("{name:?} is not a valid profile name"));
            }
            let dir = profiles_dir.join(name);
            let profile = profiles::load(&dir)?;
            let trust = packs::verify_installed(&dir)?;
            let pack = profiles::read_pack(&dir)?;
            if let Some(warning) = packs::launch_check(&pack)? {
                eprintln!("note: {warning}");
            }
            if let Some(note) = sc_launch::remote::launch_sync(&dir, &pack, trust.key().as_ref(), &mut sc_launch::remote::quiet)? {
                eprintln!("note: {note}");
            }
            let setup = launch::resolve(&cli.opts)?;
            if let Some(warning) = profiles::read_pack(&dir).ok().and_then(|p| packs::game_build_warning(&p, &setup.game_exe())) {
                eprintln!("warning: {warning}");
            }
            for warning in sc_launch::patching::preflight(&dir, &profile, &setup.game_exe())? {
                eprintln!("warning: {warning}");
            }
            let shared = sc_launch::state::shared_local_appdata(&setup);
            sc_launch::state::sync_shared(&dir, &profile, shared.as_deref())?;
            let mut cmd = launch::command(&setup, &dir, &cli.game_args)?;
            println!(
                "starting {} with profile {name}{} ({})",
                setup.game_exe().display(),
                profile.server.as_deref().map(|s| format!(" (server {s})")).unwrap_or_default(),
                describe_trust(&trust)
            );
            let status = cmd.status().map_err(|e| format!("could not start the injector: {e}"))?;
            if let Err(e) = sc_launch::state::sync_shared(&dir, &profile, shared.as_deref()) {
                eprintln!("warning: could not save game settings back: {e}");
            }
            if let Some(e) = launch::inject_error(&dir).filter(|_| !status.success()) {
                eprintln!("sc-launch: the game did not start: {e}");
            }
            println!("agent log: {}", dir.join(sc_profile::AGENT_LOG).display());
            Ok(status.code().unwrap_or(1).clamp(0, 255) as u8)
        }
        ["install", source] => {
            let staged = packs::stage(&packs::Source::parse(source)?)?;
            install_reviewed(staged, &profiles_dir, cli.yes)
        }
        ["updates"] => {
            let mut any = false;
            for e in profiles::list(&profiles_dir) {
                let Some(pack) = e.pack else { continue };
                match packs::check_update(&pack) {
                    Ok(Some(a)) => {
                        any = true;
                        println!("{:<20} {} -> {}", e.name, pack.version, a.version);
                    }
                    Ok(None) => {}
                    Err(err) => eprintln!("{}: could not check for updates: {err}", e.name),
                }
            }
            if !any {
                println!("all packs are up to date");
            }
            Ok(0)
        }
        ["update", name] => {
            if !profiles::valid_name(name) {
                return Err(format!("{name:?} is not a valid profile name"));
            }
            let pack = profiles::read_pack(&profiles_dir.join(name))?;
            match packs::check_update(&pack)? {
                None if pack.update.is_none() => Err(format!("{name} has no update address")),
                None => {
                    println!("{name} is up to date ({})", pack.version);
                    Ok(0)
                }
                Some(a) => install_reviewed(packs::stage(&packs::Source::Url(a.url))?, &profiles_dir, cli.yes),
            }
        }
        ["pack", "check", source] => {
            let staged = packs::stage(&packs::Source::parse(source)?)?;
            let existing = profiles::find_pack(&profiles_dir, &staged.pack.id).and_then(|d| profiles::read_pack(&d).ok());
            print_review(&staged, existing.as_ref());
            let mut problems: Vec<String> = sc_launch::patching::load_sets(&staged.root).into_iter().filter_map(Result::err).collect();
            match launch::find_game(&cli.opts) {
                Ok(game_dir) => {
                    let profile = sc_profile::Profile::new(&staged.pack, &Default::default());
                    match sc_launch::patching::preflight(&staged.root, &profile, &game_dir.join("win64").join("game.exe")) {
                        Ok(warnings) => warnings.iter().for_each(|w| println!("  note: {w}")),
                        Err(e) => problems.push(e),
                    }
                }
                Err(e) => println!("  (patches not checked against the game: {e})"),
            }
            packs::discard(staged);
            for p in &problems {
                println!("  problem: {p}");
            }
            println!("{}", if problems.is_empty() { "pack OK" } else { "pack has problems" });
            Ok(u8::from(!problems.is_empty()))
        }
        ["pack", "keygen", file] => {
            let path = PathBuf::from(file);
            if path.exists() {
                return Err(format!("{file} already exists; not overwriting a key"));
            }
            let key = signing::SecretKey::generate()?;
            write_secret(&path, &key.to_text())?;
            println!("signing key written to {file}; keep it secret and backed up");
            println!("public key:  {}", key.public());
            println!("fingerprint: {}", key.public().fingerprint());
            Ok(0)
        }
        ["pack", "files", folder, out] => {
            let scmod1 = out.to_ascii_lowercase().ends_with(".txt");
            let text = sc_launch::remote::build_list(std::path::Path::new(folder), cli.base_url.as_deref(), scmod1)?;
            profiles::write_atomic(std::path::Path::new(out), &text)?;
            let count = text.lines().filter(|l| l.contains("sha256") || l.contains('\t')).count();
            println!("wrote {out}: {count} file(s), {}", if scmod1 { "SCMOD1" } else { "STAR FORGE list" });
            if let Some(key) = signing_key(&cli, false)? {
                profiles::write_atomic(std::path::Path::new(&format!("{out}.sig")), &signing::sign_bytes(&key, text.as_bytes()))?;
                println!("signed it with key {} ({out}.sig); upload both", key.public().fingerprint());
            }
            Ok(0)
        }
        ["pack", "sign", folder] => {
            let key = signing_key(&cli, true)?.expect("required");
            let manifest = signing::sign(std::path::Path::new(folder), &key)?;
            println!(
                "signed {} {} ({} files) with key {}",
                manifest.id,
                manifest.version,
                manifest.files.len(),
                key.public().fingerprint()
            );
            Ok(0)
        }
        _ => Err("unknown command; see sc-launch --help".into()),
    }
}

/// The signing key from --key or STAR_FORGE_PACK_KEY.
fn signing_key(cli: &Cli, required: bool) -> Result<Option<signing::SecretKey>, String> {
    let text = match (&cli.key, std::env::var("STAR_FORGE_PACK_KEY")) {
        (Some(path), _) => std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?,
        (None, Ok(text)) => text,
        (None, Err(_)) if required => return Err("this needs --key <file> or STAR_FORGE_PACK_KEY".into()),
        (None, Err(_)) => return Ok(None),
    };
    signing::SecretKey::parse(&text).map(Some)
}

/// Writes a secret file readable only by its owner where that's possible.
fn write_secret(path: &std::path::Path, text: &str) -> Result<(), String> {
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
    let mut f = options.open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    std::io::Write::write_all(&mut f, text.as_bytes()).map_err(|e| format!("{}: {e}", path.display()))
}

fn describe_trust(trust: &Trust) -> String {
    match trust {
        Trust::Unsigned => "not signed".into(),
        Trust::NewKey(k) => format!("signed with a new key {}", k.fingerprint()),
        Trust::Pinned(k) => format!("signed by its publisher, key {}", k.fingerprint()),
        Trust::Known { name, key } => format!("signed by {name}, key {}", key.fingerprint()),
    }
}

/// Shows what a pack will do and installs it once the user agrees.
fn install_reviewed(staged: packs::Staged, profiles_dir: &std::path::Path, yes: bool) -> Result<u8, String> {
    let existing = profiles::find_pack(profiles_dir, &staged.pack.id).and_then(|d| profiles::read_pack(&d).ok());
    print_review(&staged, existing.as_ref());
    if !yes {
        print!("Install? [y/N] ");
        std::io::Write::flush(&mut std::io::stdout()).ok();
        let mut answer = String::new();
        std::io::stdin().read_line(&mut answer).map_err(|e| e.to_string())?;
        if !matches!(answer.trim(), "y" | "Y" | "yes") {
            packs::discard(staged);
            println!("not installed");
            return Ok(1);
        }
    }
    let installed = packs::install(staged, profiles_dir, &mut sc_launch::remote::quiet)?;
    println!("installed as profile {}", installed.name);
    if let Some(w) = installed.warning {
        eprintln!("warning: {w}");
    }
    Ok(0)
}

fn print_review(staged: &packs::Staged, existing: Option<&sc_profile::Pack>) {
    let pack = &staged.pack;
    println!("{} {} ({})", pack.name, pack.version, pack.id);
    if let Some(old) = existing {
        println!("  updates the installed version {}", old.version);
    }
    if !pack.authors.is_empty() {
        println!("  by {}", pack.authors.join(", "));
    }
    println!("  from {}{}", staged.origin, if staged.verified { " (checksum verified)" } else { "" });
    println!("  {}", describe_trust(&staged.trust));
    println!("  server:  {}", pack.server.address.as_deref().unwrap_or("game default"));
    if pack.server.local {
        println!("  starts the local server with the game");
    }
    match &staged.remote {
        Some(list) => println!(
            "  replaces {} game file(s), downloaded from {}{}",
            staged.files.len(),
            list.host(),
            list.bytes().map(|b| format!(" ({:.1} MB)", b as f64 / 1048576.0)).unwrap_or_default()
        ),
        None => println!("  replaces {} game file(s)", staged.files.len()),
    }
    for f in staged.files.iter().take(20) {
        println!("    {f}");
    }
    if staged.files.len() > 20 {
        println!("    ... and {} more", staged.files.len() - 20);
    }
    let patches = sc_launch::patching::summary(&staged.root, &sc_profile::Profile::new(pack, &Default::default()));
    if !patches.modules.is_empty() {
        println!("  applies {} game patch(es) to {}", patches.patches, patches.modules.join(", "));
    }
    if patches.plugins > 0 {
        println!("  loads {} native plugin(s) into the game", patches.plugins);
    }
    if !pack.game.args.is_empty() {
        println!("  game arguments: {}", pack.game.args.join(" "));
    }
    for w in &staged.warnings {
        println!("  note: {w}");
    }
}

fn main() -> ExitCode {
    sc_launch::env::load_dotenv();
    match run() {
        Ok(code) => ExitCode::from(code),
        Err(e) => {
            eprintln!("sc-launch: {e}");
            ExitCode::FAILURE
        }
    }
}
