// No console window behind the launcher on Windows release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod downloads;
mod game;
mod lifecycle;
mod prefs;
mod profiles;
mod updater;

use std::ffi::OsStr;
use tauri::{Emitter, Manager};
#[cfg(not(target_os = "linux"))]
use tauri_plugin_opener::OpenerExt;

/// Sent to the interface as a number: 0 info, 1 success, 2 error.
#[derive(Clone, Copy)]
pub enum NotificationLevel {
    Info = 0,
    Error = 2,
}

pub fn notify(app: &tauri::AppHandle, message: String, level: NotificationLevel) {
    if let Err(e) = app.emit_to("main", "notification", (message, level as u8)) {
        eprintln!("could not show notification: {e}");
    }
}

/// Opens a folder, file or web link with the desktop's default program.
pub fn open_path(app: &tauri::AppHandle, path: &OsStr) -> Result<(), String> {
    // On Linux, start xdg-open ourselves so it gets the host environment, not
    // the AppImage's (see sc_launch::env::use_host_env).
    #[cfg(target_os = "linux")]
    {
        let _ = app;
        let mut cmd = std::process::Command::new("xdg-open");
        cmd.arg(path)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null());
        sc_launch::env::use_host_env(&mut cmd);
        let mut child = cmd.spawn().map_err(|e| format!("could not open {}: {e}", path.to_string_lossy()))?;
        std::thread::spawn(move || child.wait());
        Ok(())
    }
    #[cfg(not(target_os = "linux"))]
    app.opener()
        .open_path(path.to_string_lossy(), None::<&str>)
        .map_err(|e| format!("could not open {}: {e}", path.to_string_lossy()))
}

#[tauri::command]
fn app_version(app: tauri::AppHandle) -> String {
    app.package_info().version.to_string()
}

/// `starforge://install?url=...` links, from the command line, the desktop, or
/// a second launcher instance (which hands its link over and exits).
fn setup_pack_links(app: &tauri::AppHandle) {
    use tauri_plugin_deep_link::DeepLinkExt;
    sc_launch::packs::clear_staging();
    let handle = app.clone();
    app.deep_link().on_open_url(move |event| {
        for url in event.urls() {
            profiles::receive_link(&handle, url.to_string());
        }
    });
    if let Ok(Some(urls)) = app.deep_link().get_current() {
        for url in urls {
            profiles::receive_link(app, url.to_string());
        }
    }
    // Packages register the scheme when installed; a portable copy (AppImage,
    // development build) registers itself, which also points the link at
    // wherever it is now.
    let installed = cfg!(target_os = "linux")
        && std::env::var_os("APPIMAGE").is_none()
        && std::env::current_exe().is_ok_and(|exe| exe.starts_with("/usr"));
    if !installed {
        let handle = app.clone();
        std::thread::spawn(move || {
            if let Err(e) = handle.deep_link().register_all() {
                eprintln!("could not register {}:// links: {e}", sc_launch::packs::SCHEME);
            }
        });
    }
}

fn main() {
    sc_launch::env::load_dotenv();
    // WebKitGTK's DMA-BUF renderer shows a blank window on some Linux GPU
    // drivers; the fallback renderer is fine for a launcher.
    #[cfg(target_os = "linux")]
    if std::env::var_os("WEBKIT_DISABLE_DMABUF_RENDERER").is_none() {
        // SAFETY: single-threaded, before GTK/WebKit start.
        unsafe { std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1") };
    }

    let app = tauri::Builder::default()
        .manage(lifecycle::Lifecycle::default())
        .manage(prefs::PrefsState::default())
        .manage(game::GameState::default())
        .manage(downloads::Downloads::default())
        .manage(profiles::PackReviews::default())
        .manage(updater::UpdaterState::default())
        .plugin(tauri_plugin_single_instance::init(|app, _argv, _cwd| {
            lifecycle::restore(app);
        }))
        .plugin(tauri_plugin_deep_link::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(
            tauri_plugin_window_state::Builder::new()
                .with_denylist(&["splash"])
                .with_state_flags(
                    tauri_plugin_window_state::StateFlags::SIZE
                        | tauri_plugin_window_state::StateFlags::POSITION
                        | tauri_plugin_window_state::StateFlags::MAXIMIZED,
                )
                .build(),
        )
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_updater::Builder::new().build())
        .setup(|app| {
            prefs::load(app.handle());
            lifecycle::create_windows(app.handle())?;
            if let Some(window) = app.get_webview_window("main") {
                if let Err(e) = tauri::image::Image::from_bytes(include_bytes!("../icons/128x128.png"))
                    .and_then(|icon| window.set_icon(icon))
                {
                    eprintln!("could not set the window icon: {e}");
                }
            }
            lifecycle::setup(app.handle());
            setup_pack_links(app.handle());
            Ok(())
        })
        .on_window_event(|window, event| {
            if window.label() != "main" {
                return;
            }
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                if let Err(error) = lifecycle::close_main_window(window.app_handle()) {
                    notify(
                        window.app_handle(),
                        format!("Could not close the launcher: {error}"),
                        NotificationLevel::Error,
                    );
                }
            }
        })
        .invoke_handler(|invoke| {
            let window = invoke.message.webview_ref().label();
            if window != "main" && !(window == "splash" && invoke.message.command() == "splash_finished") {
                invoke.resolver.reject("This window cannot invoke launcher commands");
                return true;
            }
            let handler: fn(tauri::ipc::Invoke<tauri::Wry>) -> bool = tauri::generate_handler![
                lifecycle::main_ready,
                lifecycle::splash_finished,
                lifecycle::exit_launcher,
                prefs::get_launcher_preferences,
                prefs::update_launcher_preferences,
                game::get_launcher_snapshot,
                game::initialize_launcher,
                game::perform_launcher_action,
                game::detect_compat_tools,
                game::get_artwork,
                game::pick_background_folder,
                game::view_game_install,
                game::view_launch_log,
                downloads::cancel_download,
                profiles::list_profiles,
                profiles::save_profile,
                profiles::delete_profile,
                profiles::open_profile_folder,
                profiles::open_pack_link,
                profiles::inspect_pack,
                profiles::install_pack,
                profiles::discard_pack,
                profiles::pick_pack_file,
                profiles::list_sample_packs,
                profiles::sample_pack_source,
                profiles::check_pack_updates,
                profiles::take_pending_link,
                profiles::get_profile_branding,
                updater::check_launcher_update,
                updater::install_launcher_update,
                updater::open_launcher_release,
                app_version,
            ];
            handler(invoke)
        });

    if let Err(error) = app.run(tauri::generate_context!()) {
        eprintln!("STAR FORGE Launcher could not start: {error}");
        std::process::exit(1);
    }
}
