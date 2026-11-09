import { invoke } from './bridge.js';
import { boolean, object, choice, string } from './contracts.js';

export const factions = /** @type {const} */ (['starforge', 'starfury', 'empire', 'federation', 'jericho']);

/** @param {unknown} input */
export function parsePreferences(input) {
  const value = object(input);
  return {
    selectedProfile: string(value.selectedProfile),
    factionTheme: choice(value.factionTheme, factions),
    packBranding: boolean(value.packBranding),
    backgroundFolder: string(value.backgroundFolder),
    launchBehavior: choice(value.launchBehavior, ['tray', 'keepOpen', 'exit']),
    closeBehavior: choice(value.closeBehavior, ['tray', 'exit']),
    gameDirectory: string(value.gameDirectory),
    protonPath: string(value.protonPath),
    checkUpdates: boolean(value.checkUpdates),
  };
}
/** @typedef {ReturnType<typeof parsePreferences>} Preferences */

export async function loadPreferences() {
  let current = parsePreferences(await invoke('get_launcher_preferences'));
  /** @type {Set<(preferences: Preferences) => void>} */
  const listeners = new Set();
  let queue = Promise.resolve();
  return {
    get: () => current,
    async reload() {
      await queue;
      current = parsePreferences(await invoke('get_launcher_preferences'));
      listeners.forEach((callback) => callback(current));
    },
    /** @param {(preferences: Preferences) => void} callback */
    subscribe(callback) {
      listeners.add(callback);
      return () => listeners.delete(callback);
    },
    /** @template {keyof Preferences} K @param {K} key @param {Preferences[K]} value */
    update(key, value) {
      const pending = queue.then(async () => {
        current = parsePreferences(
          await invoke('update_launcher_preferences', { update: { key, value } }),
        );
        listeners.forEach((callback) => callback(current));
      });
      // Keep subsequent user edits usable after a failed write; caller still receives the rejection.
      queue = pending.catch(() => {});
      return pending;
    },
  };
}
/** @typedef {Awaited<ReturnType<typeof loadPreferences>>} PreferenceStore */
