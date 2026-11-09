#pragma once

// Port of lifecycle.rs's StartupGate: the main window is shown once the native side and the
// splash are done and the interface reports ready (or a watchdog gives up waiting for it).

namespace sf::app {

struct StartupGate {
    bool native_ready_ = false, ready_ = false, sting_finished_ = false, watchdog_elapsed_ = false, revealed = false;

    // Each returns whether the window should be shown now (once).
    bool reveal() {
        if (native_ready_ && (ready_ || watchdog_elapsed_) && sting_finished_ && !revealed) {
            revealed = true;
            return true;
        }
        return false;
    }
    bool ready() {
        ready_ = true;
        return reveal();
    }
    bool finish_sting() {
        sting_finished_ = true;
        return reveal();
    }
    bool native_ready() {
        native_ready_ = true;
        return reveal();
    }
    bool watchdog() {
        watchdog_elapsed_ = true;
        return finish_sting();
    }
};

}  // namespace sf::app
