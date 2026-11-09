import { invoke } from './bridge.js';
import { object } from './contracts.js';
import { run } from './ui.js';

/** @param {unknown} value */
export function image(value) {
  if (value === null) return null;
  if (typeof value !== 'string' || !/^data:image\/(png|jpeg|webp);base64,[A-Za-z0-9+/=]+$/.test(value))
    throw new Error('Invalid artwork');
  return value;
}

/** The default background, and a server pack's background shown instead of it. */
let hero = /** @type {string | null} */ (null);
let override = /** @type {string | null} */ (null);

const paint = () => {
  const art = override || hero;
  for (const node of document.querySelectorAll('.artwork, .game-card-art'))
    if (node instanceof HTMLElement) node.style.backgroundImage = art ? `url("${art}")` : '';
};

/** @param {string | null} url a pack background, or null for the game's own */
export function setBackground(url) {
  override = url;
  paint();
}

/**
 * The default background: a random image from the player's background folder,
 * else Star Conflict's banner from the local Steam library cache.
 * @param {import('./preferences.js').PreferenceStore} preferences
 */
export async function bindArtwork(preferences) {
  const load = async () => {
    const art = object(await invoke('get_artwork'));
    hero = image(art.hero);
    paint();
  };
  let folder = preferences.get().backgroundFolder;
  preferences.subscribe((values) => {
    if (values.backgroundFolder === folder) return;
    folder = values.backgroundFolder;
    run(load);
  });
  await load();
}
