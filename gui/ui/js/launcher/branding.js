import { invoke } from './bridge.js';
import { object } from './contracts.js';
import { image, setBackground } from './artwork.js';
import { required, run } from './ui.js';

const defaults = { logo: 'assets/starforge-logo.svg', icon: 'assets/starforge-emblem.svg' };

/** @param {unknown} input */
function parseBranding(input) {
  const value = object(input);
  const accent = value.accent;
  if (accent !== null && (typeof accent !== 'string' || !/^#[0-9a-fA-F]{6}$/.test(accent)))
    throw new Error('Invalid accent colour');
  return { logo: image(value.logo), icon: image(value.icon), background: image(value.background), accent };
}
/** @typedef {ReturnType<typeof parseBranding>} Branding */

/** @param {Branding | null} branding */
function paint(branding) {
  const wordmark = required('game-wordmark');
  if (wordmark instanceof HTMLImageElement) wordmark.src = branding?.logo || defaults.logo;
  for (const node of document.querySelectorAll('.game-emblem'))
    if (node instanceof HTMLImageElement) node.src = branding?.icon || defaults.icon;
  setBackground(branding?.background || null);
  // A pack's colour wins over the faction theme; text on it stays dark.
  const root = document.documentElement.style;
  if (branding?.accent) {
    root.setProperty('--accent', branding.accent);
    root.setProperty('--ink', `color-mix(in srgb, ${branding.accent} 12%, black)`);
  } else {
    root.removeProperty('--accent');
    root.removeProperty('--ink');
  }
}

/**
 * Shows the selected server pack's logo, icon, background and colour.
 * @param {ReturnType<typeof import('./profiles.js').bindProfiles>} profiles
 * @param {import('./preferences.js').PreferenceStore} preferences
 */
export function bindBranding(profiles, preferences) {
  /** Artwork by profile and pack version, so an updated pack is read again. @type {Map<string, Branding>} */
  const cache = new Map();
  let shown = '';

  const update = () => {
    const profile = profiles.current();
    const key = profile && !profile.error && preferences.get().packBranding ? `${profile.id}@${profile.version}` : '';
    if (key === shown) return;
    shown = key;
    if (!key || !profile) return paint(null);
    const cached = cache.get(key);
    if (cached) return paint(cached);
    run(async () => {
      try {
        const branding = parseBranding(await invoke('get_profile_branding', { id: profile.id }));
        cache.set(key, branding);
        if (shown === key) paint(branding);
      } catch (error) {
        if (shown === key) paint(null);
        throw error;
      }
    });
  };
  preferences.subscribe(update);
  return update;
}
