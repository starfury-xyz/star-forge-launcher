function tauri() {
  if (!window.__TAURI__) throw new Error('Open STAR FORGE Launcher itself to use this interface.');
  return window.__TAURI__;
}

/** @param {string} command @param {Record<string, unknown>} [args] */
export const invoke = (command, args) => tauri().core.invoke(command, args);
/** @param {string} event @param {(payload: unknown) => void} callback */
export const listen = (event, callback) =>
  tauri().event.listen(event, (value) => callback(value.payload));
export const launcherBridge = {
  /** @param {(snapshot: unknown) => void} callback */
  listen: (callback) => listen('launcher-state-changed', callback),
  getSnapshot: () => invoke('get_launcher_snapshot'),
  /** @param {string} action */
  performAction: async (action) => {
    await invoke('perform_launcher_action', { action });
  },
};

/** @param {'minimize' | 'toggleMaximize' | 'close'} action */
export const windowAction = (action) => tauri().window.getCurrentWindow()[action]();
