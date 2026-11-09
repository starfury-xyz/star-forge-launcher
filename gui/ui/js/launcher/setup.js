import { invoke } from './bridge.js';
import { boolean, number, object, string } from './contracts.js';
import { control, errorMessage, notice, required, run } from './ui.js';

/** @param {unknown} input */
function parseInfo(input) {
  const value = object(input);
  const appimage = value.appimage === null ? null : object(value.appimage);
  return {
    needed: boolean(value.needed),
    gameDir: string(value.gameDir),
    gameDetected: boolean(value.gameDetected),
    profilesDir: string(value.profilesDir),
    standardProfilesDir: string(value.standardProfilesDir),
    dataDir: string(value.dataDir),
    profileCount: number(value.profileCount, 0, Number.MAX_SAFE_INTEGER),
    appimage: appimage && {
      path: string(appimage.path),
      suggestedDir: string(appimage.suggestedDir),
      inPlace: boolean(appimage.inPlace),
    },
  };
}
/** @typedef {ReturnType<typeof parseInfo>} SetupInfo */

/**
 * The first-run setup: Express takes the recommended folders, Custom lets the
 * player pick the game and profiles folders. An AppImage then offers to move
 * itself into the applications menu. Settings can run it again.
 * @param {import('./preferences.js').PreferenceStore} preferences
 * @param {{ refresh: () => Promise<void> }} profiles
 */
export async function bindSetup(preferences, profiles) {
  const dialog = control('setup-dialog', HTMLDialogElement);
  const title = required('setup-title');
  const steps = {
    choose: required('setup-choose'),
    custom: required('setup-custom'),
    appimage: required('setup-appimage'),
  };
  const game = control('setup-game', HTMLInputElement);
  const gameNote = required('setup-game-note');
  const profilesInput = control('setup-profiles', HTMLInputElement);
  const move = control('setup-move', HTMLInputElement);
  const moveRow = required('setup-move-row');
  const appimageDir = control('setup-appimage-dir', HTMLInputElement);
  const back = control('setup-back', HTMLButtonElement);
  const next = control('setup-next', HTMLButtonElement);
  const later = control('setup-cancel', HTMLButtonElement);
  /** @type {SetupInfo | null} */
  let info = null;
  /** @type {'choose' | 'custom' | 'appimage'} */
  let step = 'choose';
  /** First run: the player can't skip it. */
  let required_ = false;
  let busy = false;

  const show = (/** @type {typeof step} */ name) => {
    step = name;
    for (const [key, node] of Object.entries(steps)) node.hidden = key !== name;
    back.hidden = name !== 'custom';
    next.hidden = name === 'choose';
    next.textContent = name === 'appimage' ? 'Add to menu' : 'Finish setup';
    later.hidden = required_ && name !== 'appimage';
    later.textContent = name === 'appimage' ? 'Skip' : 'Cancel';
  };

  const checkGame = async () => {
    const path = game.value.trim();
    gameNote.dataset.state = '';
    if (!path) {
      gameNote.textContent = 'Star Conflict wasn’t found through Steam. Pick the folder it is installed in.';
      gameNote.dataset.state = 'error';
      return false;
    }
    try {
      await invoke('check_game_folder', { path });
      gameNote.textContent =
        info?.gameDetected && path === info.gameDir ? 'Found through Steam.' : 'Star Conflict found.';
      gameNote.dataset.state = 'ok';
      return true;
    } catch (error) {
      gameNote.textContent = errorMessage(error);
      gameNote.dataset.state = 'error';
      return false;
    }
  };

  const syncMove = () => {
    const changed = info !== null && profilesInput.value.trim() !== info.profilesDir;
    moveRow.hidden = !changed || !info?.profileCount;
    if (info) required('setup-move-label').textContent = `Move my ${info.profileCount} profile(s) there`;
  };

  /** @param {boolean} rerun from Settings, so it can be cancelled */
  const open = async (rerun) => {
    info = parseInfo(await invoke('get_setup_info'));
    required_ = !rerun;
    title.textContent = rerun ? 'Setup' : 'Welcome to STAR FORGE';
    game.value = info.gameDir;
    profilesInput.value = info.profilesDir;
    move.checked = true;
    required('setup-data').textContent = info.dataDir;
    required('setup-express-game').textContent = info.gameDir
      ? `Star Conflict: ${info.gameDir}`
      : 'Star Conflict: not found through Steam yet – you’ll pick its folder next';
    required('setup-express-profiles').textContent = `Profiles: ${info.profilesDir}`;
    if (info.appimage) appimageDir.value = info.appimage.suggestedDir;
    syncMove();
    show('choose');
    gameNote.textContent = '';
    if (!dialog.open) dialog.showModal();
  };

  const close = () => {
    dialog.close();
    info = null;
  };

  /** @param {string} gameDir @param {string} profilesDir @param {boolean} moveProfiles */
  const save = async (gameDir, profilesDir, moveProfiles) => {
    await invoke('finish_setup', { gameDir, profilesDir, moveProfiles });
    await preferences.reload();
    await profiles.refresh();
    if (info?.appimage && !info.appimage.inPlace) show('appimage');
    else {
      close();
      notice('Setup saved.', 'success');
    }
  };

  /** @param {() => Promise<void>} action */
  const guarded = (action) =>
    run(async () => {
      if (busy) return;
      busy = true;
      next.disabled = true;
      try {
        await action();
      } catch (error) {
        notice(errorMessage(error), 'error');
      } finally {
        busy = false;
        next.disabled = false;
      }
    });

  required('setup-express').addEventListener('click', () =>
    guarded(async () => {
      if (!info) return;
      if (!info.gameDir) {
        show('custom');
        await checkGame();
        return;
      }
      await save(info.gameDetected ? '' : info.gameDir, info.profilesDir, false);
    }),
  );
  required('setup-custom-choice').addEventListener('click', () => {
    show('custom');
    run(checkGame);
  });
  back.addEventListener('click', () => show('choose'));
  later.addEventListener('click', () => {
    if (step === 'appimage') {
      close();
      notice('Setup saved.', 'success');
    } else close();
  });
  // First run: Esc doesn't skip it.
  dialog.addEventListener('cancel', (event) => {
    if (required_ && step !== 'appimage') event.preventDefault();
  });
  game.addEventListener('change', () => run(checkGame));
  profilesInput.addEventListener('input', syncMove);
  required('setup-profiles-reset').addEventListener('click', () => {
    if (info) profilesInput.value = info.standardProfilesDir;
    syncMove();
  });
  /** @param {HTMLInputElement} input @param {string} label */
  const browse = (input, label) =>
    run(async () => {
      const dir = await invoke('pick_folder', { title: label, start: input.value.trim() || null });
      if (typeof dir === 'string') {
        input.value = dir;
        input.dispatchEvent(new Event('change'));
        input.dispatchEvent(new Event('input'));
      }
    });
  required('setup-game-browse').addEventListener('click', () => browse(game, 'Star Conflict folder'));
  required('setup-profiles-browse').addEventListener('click', () => browse(profilesInput, 'Profiles folder'));
  required('setup-appimage-browse').addEventListener('click', () => browse(appimageDir, 'Folder for the AppImage'));

  next.addEventListener('click', () =>
    guarded(async () => {
      if (step === 'custom') {
        if (!(await checkGame())) return;
        const gameDir = game.value.trim();
        await save(
          info?.gameDetected && gameDir === info.gameDir ? '' : gameDir,
          profilesInput.value.trim(),
          !moveRow.hidden && move.checked,
        );
      } else if (step === 'appimage') {
        await invoke('install_appimage', { dir: appimageDir.value.trim() });
        close();
        notice('STAR FORGE is in your applications menu.', 'success');
      }
    }),
  );

  document.addEventListener('run-setup', () => run(() => open(true)));
  const first = parseInfo(await invoke('get_setup_info'));
  if (first.needed) await open(false);
  return { open: () => open(true) };
}
