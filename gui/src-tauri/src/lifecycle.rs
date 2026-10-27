//! Splash, main window reveal, tray and exit.

use crate::Locked;
use crate::prefs::{self, CloseBehavior};
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;
use tauri::{AppHandle, Manager};

/// The main window is shown once the native side and the splash are done and
/// the interface reports ready (or a watchdog gives up waiting for it).
#[derive(Default)]
struct StartupGate {
    native_ready: bool,
    ready: bool,
    sting_finished: bool,
    watchdog_elapsed: bool,
    revealed: bool,
}
impl StartupGate {
    fn reveal(&mut self) -> bool {
        if self.native_ready && (self.ready || self.watchdog_elapsed) && self.sting_finished && !self.revealed {
            self.revealed = true;
            true
        } else {
            false
        }
    }
    fn ready(&mut self) -> bool {
        self.ready = true;
        self.reveal()
    }
    fn finish_sting(&mut self) -> bool {
        self.sting_finished = true;
        self.reveal()
    }
    fn native_ready(&mut self) -> bool {
        self.native_ready = true;
        self.reveal()
    }
    fn watchdog(&mut self) -> bool {
        self.watchdog_elapsed = true;
        self.finish_sting()
    }
}

#[derive(Default)]
pub struct Lifecycle {
    startup: Mutex<StartupGate>,
    tray_available: AtomicBool,
}

pub fn create_windows(app: &AppHandle) -> tauri::Result<()> {
    for config in &app.config().app.windows {
        tauri::WebviewWindowBuilder::from_config(app, config)?.build()?;
    }
    Ok(())
}

fn show(app: &AppHandle) {
    if let Some(splash) = app.get_webview_window("splash") {
        let _ = splash.close();
    }
    if let Some(window) = app.get_webview_window("main") {
        if let Err(error) = fit_to_monitor(&window) {
            eprintln!("could not fit the launcher to the monitor: {error}");
        }
        if let Err(error) = window.show().and_then(|_| window.unminimize()).and_then(|_| window.set_focus()) {
            eprintln!("could not show the launcher: {error}");
        }
    }
}

fn fit_to_monitor(window: &tauri::WebviewWindow) -> tauri::Result<()> {
    if window.is_maximized()? {
        return Ok(());
    }
    let Some(monitor) = window.current_monitor()?.or(window.primary_monitor()?) else {
        return Ok(());
    };
    let area = monitor.work_area();
    let old = window.outer_size()?;
    let size = tauri::PhysicalSize::new(old.width.min(area.size.width), old.height.min(area.size.height));
    if size != old {
        window.set_size(size)?;
    }
    let position = window.outer_position()?;
    let x = position.x.clamp(area.position.x, area.position.x + (area.size.width - size.width) as i32);
    let y = position.y.clamp(area.position.y, area.position.y + (area.size.height - size.height) as i32);
    if x != position.x || y != position.y {
        window.set_position(tauri::PhysicalPosition::new(x, y))?;
    }
    Ok(())
}

/// Brings the launcher back (tray click, second instance, game exit).
pub fn restore(app: &AppHandle) {
    let Some(lifecycle) = app.try_state::<Lifecycle>() else {
        return;
    };
    let can_show = {
        let mut gate = lifecycle.startup.locked();
        if gate.revealed { true } else { gate.finish_sting() }
    };
    if can_show {
        show(app);
    }
}

pub fn hide_to_tray(app: &AppHandle) -> Result<(), String> {
    let window = app.get_webview_window("main").ok_or("Launcher window unavailable")?;
    if app.state::<Lifecycle>().tray_available.load(Ordering::Relaxed) {
        window.hide().map_err(|e| e.to_string())
    } else {
        window.minimize().map_err(|e| e.to_string())?;
        crate::notify(
            app,
            "System tray is unavailable. Restore STAR FORGE Launcher from the taskbar.".into(),
            crate::NotificationLevel::Info,
        );
        Ok(())
    }
}

pub fn close_main_window(app: &AppHandle) -> Result<(), String> {
    match prefs::get(app).close_behavior {
        CloseBehavior::Tray => hide_to_tray(app),
        CloseBehavior::Exit => {
            exit_launcher(app.clone());
            Ok(())
        }
    }
}

#[tauri::command]
pub fn main_ready(app: AppHandle) {
    if app.state::<Lifecycle>().startup.locked().ready() {
        show(&app);
    }
}

#[tauri::command]
pub fn splash_finished(app: AppHandle) {
    if app.state::<Lifecycle>().startup.locked().finish_sting() {
        show(&app);
    }
}

#[tauri::command]
pub fn exit_launcher(app: AppHandle) {
    // A running game is not tied to the launcher, so exiting is always safe.
    app.exit(0);
}

/// Linux tray icons need an AppIndicator library at runtime, and the tray code
/// panics instead of failing when it is missing, so check first.
#[cfg(target_os = "linux")]
fn tray_supported() -> bool {
    [
        c"libayatana-appindicator3.so.1",
        c"libappindicator3.so.1",
        c"libayatana-appindicator3.so",
        c"libappindicator3.so",
    ]
    .iter()
    .any(|name| {
        // SAFETY: dlopen/dlclose on a library name; the handle is not used otherwise.
        unsafe {
            let handle = libc::dlopen(name.as_ptr(), libc::RTLD_LAZY | libc::RTLD_LOCAL);
            if handle.is_null() {
                false
            } else {
                libc::dlclose(handle);
                true
            }
        }
    })
}

#[cfg(not(target_os = "linux"))]
fn tray_supported() -> bool {
    true
}

pub fn setup(app: &AppHandle) {
    let create_tray = || -> tauri::Result<()> {
        use tauri::menu::{Menu, MenuItem};
        let show_item = MenuItem::with_id(app, "show", "Show STAR FORGE Launcher", true, None::<&str>)?;
        let exit_item = MenuItem::with_id(app, "exit", "Exit", true, None::<&str>)?;
        let menu = Menu::with_items(app, &[&show_item, &exit_item])?;
        tauri::tray::TrayIconBuilder::with_id("star-forge")
            .icon(tauri::image::Image::from_bytes(include_bytes!("../icons/128x128.png"))?)
            .tooltip("STAR FORGE Launcher")
            .menu(&menu)
            .show_menu_on_left_click(true)
            .on_menu_event(|app, event| match event.id.as_ref() {
                "show" => restore(app),
                "exit" => exit_launcher(app.clone()),
                _ => {}
            })
            .build(app)?;
        Ok(())
    };
    if !tray_supported() {
        eprintln!("system tray unavailable (install libayatana-appindicator); the taskbar still works");
    } else {
        match create_tray() {
            Ok(()) => app.state::<Lifecycle>().tray_available.store(true, Ordering::Release),
            Err(error) => eprintln!("system tray unavailable; the taskbar still works: {error}"),
        }
    }
    if let Some(splash) = app.get_webview_window("splash") {
        let _ = splash.set_ignore_cursor_events(true);
    }
    if app.state::<Lifecycle>().startup.locked().native_ready() {
        show(app);
    }
    let app = app.clone();
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_secs(5));
        let reveal = app.state::<Lifecycle>().startup.locked().watchdog();
        if reveal {
            eprintln!("startup watchdog revealed the launcher before the interface reported ready");
            show(&app);
        }
    });
}

#[cfg(test)]
mod tests {
    use super::StartupGate;

    #[test]
    fn waits_for_ready_and_sting_in_either_order() {
        let mut gate = StartupGate::default();
        assert!(!gate.native_ready());
        assert!(!gate.ready());
        assert!(gate.finish_sting());
        assert!(!gate.finish_sting());

        let mut gate = StartupGate::default();
        gate.native_ready();
        assert!(!gate.finish_sting());
        assert!(gate.ready());
    }

    #[test]
    fn watchdog_reveals_without_ui_ready() {
        let mut gate = StartupGate::default();
        gate.native_ready();
        assert!(gate.watchdog());
    }
}
