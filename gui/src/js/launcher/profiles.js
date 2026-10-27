import { invoke } from './bridge.js';
import { array, boolean, number, object, string } from './contracts.js';
import { brand, control, openPanel, required, notice, errorMessage, run } from './ui.js';

/** @param {unknown} item */
function parseProfile(item) {
  const value = object(item);
  return {
    id: string(value.id),
    name: string(value.name),
    server: value.server === null ? null : string(value.server),
    localServer: boolean(value.localServer),
    ownGameData: boolean(value.ownGameData),
    patches: number(value.patches, 0, 100000),
    error: value.error === null ? null : string(value.error),
    packId: value.packId === null ? null : string(value.packId),
    version: value.version === null ? null : string(value.version),
    description: string(value.description),
    managed: boolean(value.managed),
    links: array(value.links, (link) => {
      const item = object(link);
      return { key: string(item.key), label: string(item.label) };
    }),
  };
}
/** @typedef {ReturnType<typeof parseProfile>} ProfileInfo */

const isLocalAddress = (/** @type {string} */ server) =>
  /^(127\.\d+\.\d+\.\d+|localhost)(:\d+)?$/i.test(server.trim());

/** @param {import('./preferences.js').PreferenceStore} preferences @param {() => void} onChange */
export function bindProfiles(preferences, onChange) {
  const select = control('profile-select', HTMLSelectElement);
  const edit = control('summary-edit', HTMLButtonElement);
  const checkUpdate = control('summary-check-update', HTMLButtonElement);
  let checking = false;
  const dialog = control('profile-dialog', HTMLDialogElement);
  const form = control('profile-form', HTMLFormElement);
  const nameInput = control('profile-name', HTMLInputElement);
  const serverInput = control('profile-server', HTMLInputElement);
  const localInput = control('profile-local-server', HTMLInputElement);
  const ownDataInput = control('profile-own-data', HTMLInputElement);
  const formError = required('profile-error');
  const confirm = control('confirm-dialog', HTMLDialogElement);
  /** @type {ProfileInfo[]} */
  let profiles = [];
  let loaded = false;
  /** Profile being edited; null while adding. @type {ProfileInfo | null} */
  let editing = null;
  /** Whether the local-server box was set by hand in the open dialog. */
  let localTouched = false;
  /** Newer pack versions by profile id. @type {Map<string, { version: string, url: string }>} */
  let updates = new Map();
  /** @type {(url: string) => void} */
  let onReviewUpdate = () => {};

  const current = () => profiles.find((p) => p.id === select.value) || null;

  const summarize = () => {
    const profile = current();
    required('summary-name').textContent = profile ? brand(profile.name) : 'No profile selected';
    required('summary-server').textContent = profile
      ? profile.error
        ? brand(`Invalid profile: ${profile.error}`)
        : brand(profile.server || 'Game default')
      : '–';
    required('summary-mode').textContent = profile
      ? profile.localServer
        ? 'Game and local server'
        : 'Game only'
      : '–';
    required('summary-patches').textContent = profile ? String(profile.patches) : '–';
    required('summary-version').textContent = profile?.version
      ? `${profile.version}${profile.managed ? '' : ' (your own)'}`
      : '–';
    const description = required('summary-description');
    description.textContent = brand(profile?.description || '');
    description.hidden = !profile?.description;
    const links = required('summary-links');
    links.replaceChildren(
      ...(profile?.links || []).map((link) => {
        const button = document.createElement('button');
        button.className = 'button';
        button.textContent = link.label;
        button.addEventListener('click', () =>
          run(() => invoke('open_pack_link', { id: profile?.id, key: link.key })),
        );
        return button;
      }),
    );
    links.hidden = !profile?.links.length;
    const update = profile ? updates.get(profile.id) : undefined;
    required('summary-update-row').hidden = !update;
    required('summary-update-text').textContent = update
      ? `Version ${update.version} of this pack is available.`
      : '';
    control('summary-folder', HTMLButtonElement).disabled = !profile;
    edit.disabled = !profile;
    checkUpdate.hidden = !profile?.managed;
    checkUpdate.disabled = checking || Boolean(profile?.error);
    checkUpdate.textContent = checking ? 'Checking…' : 'Check for updates';
  };

  /** @param {string} [prefer] profile to select afterwards */
  const refresh = async (prefer) => {
    loaded = false;
    select.disabled = true;
    onChange();
    try {
      profiles = array(await invoke('list_profiles'), parseProfile);
      select.replaceChildren();
      if (!profiles.length) select.add(new Option('No profiles yet – add one', ''));
      for (const profile of profiles) {
        const option = new Option(
          brand(profile.error ? `${profile.name} (invalid)` : profile.name),
          profile.id,
        );
        select.add(option);
      }
      const saved = prefer ?? preferences.get().selectedProfile;
      const usable = profiles.filter((p) => !p.error);
      // Invalid profiles stay selectable so they can be edited or deleted; Play stays off.
      const choice = profiles.find((p) => p.id === saved) || usable[0];
      select.value = choice ? choice.id : '';
      if ((choice?.id || '') !== preferences.get().selectedProfile)
        await preferences.update('selectedProfile', choice?.id || '');
      loaded = true;
      select.disabled = !profiles.length;
      required('home-add-server').classList.toggle('primary', !profiles.length);
    } catch (error) {
      notice(`Could not load server profiles: ${errorMessage(error)}`, 'error');
    } finally {
      summarize();
      onChange();
    }
  };

  /** @param {ProfileInfo | null} profile */
  const open = (profile) => {
    editing = profile;
    localTouched = false;
    required('profile-dialog-title').textContent = profile ? 'Edit server profile' : 'Add server profile';
    nameInput.value = brand(profile?.name || '');
    serverInput.value = brand(profile?.server || '');
    localInput.checked = profile ? profile.localServer : false;
    ownDataInput.checked = profile ? profile.ownGameData : true;
    formError.hidden = true;
    required('pack-import').hidden = Boolean(profile);
    required('profile-managed-note').hidden = !profile?.managed;
    control('pack-link', HTMLInputElement).value = '';
    required('delete-profile').hidden = !profile;
    required('profile-folder').hidden = !profile;
    openPanel(dialog);
    nameInput.focus();
  };

  select.addEventListener('change', () => {
    summarize();
    loaded = false;
    onChange();
    run(async () => {
      try {
        await preferences.update('selectedProfile', select.value);
      } finally {
        loaded = true;
        onChange();
      }
    });
  });
  required('add-profile').addEventListener('click', () => open(null));
  required('home-add-server').addEventListener('click', () => {
    open(null);
    control('pack-link', HTMLInputElement).focus();
  });
  required('summary-update').addEventListener('click', () => {
    const update = updates.get(current()?.id || '');
    if (update) onReviewUpdate(update.url);
  });
  edit.addEventListener('click', () => open(current()));
  checkUpdate.addEventListener('click', () => {
    const profile = current();
    if (!profile || checking) return;
    checking = true;
    summarize();
    onChange();
    run(async () => {
      try {
        const found = await invoke('check_pack_update', { id: profile.id });
        const value = found === null ? null : object(found);
        if (value) {
          updates.set(profile.id, { version: string(value.version), url: string(value.url) });
          notice(`Version ${string(value.version)} of ${profile.name} is available.`);
        } else {
          updates.delete(profile.id);
          notice(`${profile.name} ${profile.version ?? ''} is up to date.`, 'success');
        }
      } catch (error) {
        notice(`Could not check ${profile.name} for updates: ${errorMessage(error)}`, 'error');
      } finally {
        checking = false;
        summarize();
        onChange();
      }
    });
  });
  required('summary-folder').addEventListener('click', () => {
    const profile = current();
    if (profile) run(() => invoke('open_profile_folder', { id: profile.id }));
  });
  required('profile-folder').addEventListener('click', () => {
    if (editing) run(() => invoke('open_profile_folder', { id: editing?.id }));
  });
  localInput.addEventListener('change', () => (localTouched = true));
  serverInput.addEventListener('input', () => {
    // A local address almost always means the bundled server should start too.
    if (!localTouched) localInput.checked = isLocalAddress(serverInput.value);
  });
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    const save = control('save-profile', HTMLButtonElement);
    save.disabled = true;
    formError.hidden = true;
    run(async () => {
      try {
        const id = string(
          await invoke('save_profile', {
            profile: {
              id: editing?.id ?? null,
              name: nameInput.value,
              server: serverInput.value,
              localServer: localInput.checked,
              ownGameData: ownDataInput.checked,
            },
          }),
        );
        dialog.close();
        await refresh(id);
        notice(editing ? 'Profile saved.' : 'Profile added.', 'success');
      } catch (error) {
        formError.textContent = brand(errorMessage(error));
        formError.hidden = false;
      } finally {
        save.disabled = false;
      }
    });
  });
  required('delete-profile').addEventListener('click', () => {
    if (!editing) return;
    required('confirm-text').textContent =
      `“${brand(editing.name)}” and everything in its folder (including any replacement game files) will be deleted.`;
    confirm.showModal();
  });
  required('confirm-delete').addEventListener('click', () => {
    const target = editing;
    confirm.close();
    if (!target) return;
    run(async () => {
      await invoke('delete_profile', { id: target.id });
      dialog.close();
      await refresh();
      notice(`Deleted “${target.name}”.`, 'success');
    });
  });

  return {
    refresh,
    ready: () => loaded && Boolean(current()) && !current()?.error,
    current,
    /** @param {Map<string, { version: string, url: string }>} found */
    setUpdates: (found) => {
      updates = found;
      summarize();
    },
    /** @param {(url: string) => void} handler */
    onReviewUpdate: (handler) => (onReviewUpdate = handler),
    closeDialog: () => dialog.close(),
  };
}
