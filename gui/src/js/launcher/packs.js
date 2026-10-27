import { invoke, listen } from './bridge.js';
import { array, boolean, number, object, string } from './contracts.js';
import { brand, control, el, required, notice, errorMessage, run } from './ui.js';

const text = (/** @type {unknown} */ value) => (value === null ? null : string(value));

/** @param {unknown} item */
function parseReview(item) {
  const value = object(item);
  const update = value.update === null ? null : object(value.update);
  return {
    token: number(value.token, 0, Number.MAX_SAFE_INTEGER),
    id: string(value.id),
    name: string(value.name),
    version: string(value.version),
    description: string(value.description),
    authors: array(value.authors, string),
    origin: string(value.origin),
    verified: boolean(value.verified),
    signer: text(value.signer),
    publisher: text(value.publisher),
    newKey: boolean(value.newKey),
    runsCode: boolean(value.runsCode),
    managed: boolean(value.managed),
    server: text(value.server),
    localServer: boolean(value.localServer),
    args: array(value.args, string),
    patches: number(value.patches, 0, 100000),
    plugins: number(value.plugins, 0, 1000),
    fileCount: number(value.fileCount, 0, Number.MAX_SAFE_INTEGER),
    files: array(value.files, string),
    filesHost: text(value.filesHost),
    downloadBytes: value.downloadBytes === null ? null : number(value.downloadBytes, 0, Number.MAX_SAFE_INTEGER),
    warnings: array(value.warnings, string),
    update: update && {
      profile: string(update.profile),
      name: string(update.name),
      version: string(update.version),
      server: text(update.server),
      patches: number(update.patches, 0, 100000),
      filesAdded: number(update.filesAdded, 0, Number.MAX_SAFE_INTEGER),
      filesRemoved: number(update.filesRemoved, 0, Number.MAX_SAFE_INTEGER),
    },
  };
}
/** @typedef {ReturnType<typeof parseReview>} PackReview */

/** @param {PackReview} review */
function renderReview(review) {
  const body = required('pack-body');
  const parts = [];
  const head = el('div', 'pack-head');
  head.append(el('strong', '', brand(review.name)), el('span', 'pack-version', `version ${review.version}`));
  parts.push(head);
  if (review.description) parts.push(el('p', 'pack-description', brand(review.description)));

  const facts = el('dl', 'profile-facts');
  /** @param {string} term @param {string} value */
  const fact = (term, value) => facts.append(el('dt', '', term), el('dd', '', value));
  if (review.authors.length) fact('By', review.authors.join(', '));
  fact('From', review.origin);
  fact('Download', review.verified ? 'Checked against the published checksum' : 'Not checked (no published checksum)');
  fact(
    'Signature',
    !review.signer
      ? 'Not signed'
      : review.publisher
        ? `${brand(review.publisher)} (key ${review.signer})`
        : review.newKey
          ? `Key ${review.signer}, new: updates must be signed with it from now on`
          : `Key ${review.signer}, same as the installed version`,
  );
  const server = brand(review.server || 'Game default');
  fact('Server', review.update && review.update.server !== review.server ? `${brand(review.update.server || 'Game default')} → ${server}` : server);
  fact('Starts', review.localServer ? 'Game and local server' : 'Game only');
  let files = `${review.fileCount} file${review.fileCount === 1 ? '' : 's'}`;
  if (review.update) files += ` (${review.update.filesAdded} new, ${review.update.filesRemoved} removed)`;
  if (review.filesHost) {
    files += `, downloaded from ${review.filesHost}`;
    if (review.downloadBytes !== null) files += ` (${(review.downloadBytes / 1048576).toFixed(1)} MB)`;
  }
  fact('Replaces game files', files);
  if (review.patches || review.update?.patches)
    fact('Game patches', review.update ? `${review.update.patches} → ${review.patches}` : String(review.patches));
  if (review.plugins) fact('Native plugins', `${review.plugins} (run their own code in the game)`);
  if (review.args.length) fact('Game arguments', review.args.join(' '));
  if (review.managed) fact('Updates', 'Checked automatically from the server');
  parts.push(facts);

  if (review.files.length) {
    const details = document.createElement('details');
    details.className = 'pack-files';
    details.append(el('summary', '', 'Show replaced files'));
    const list = el('ul', '');
    for (const file of review.files) list.append(el('li', '', file));
    if (review.fileCount > review.files.length)
      list.append(el('li', 'muted', `… and ${review.fileCount - review.files.length} more`));
    details.append(list);
    parts.push(details);
  }
  if (review.warnings.length) {
    const list = el('ul', 'pack-warnings');
    for (const warning of review.warnings) list.append(el('li', '', brand(warning)));
    parts.push(list);
  }
  parts.push(
    el(
      'p',
      'pack-trust',
      review.runsCode
        ? 'This pack changes the game’s code as well as its files. A signature shows who made it, not that it is safe: only install packs from servers you trust.'
        : 'Packs change the files the game loads. Only install packs from servers you trust.',
    ),
  );
  body.replaceChildren(...parts);
  required('pack-title').textContent = review.update
    ? `Update ${brand(review.update.name)} from ${review.update.version}`
    : 'Install server pack';
  control('pack-install', HTMLButtonElement).textContent = review.update ? 'Update' : 'Install';
}

/**
 * Pack import, review, install and update checks.
 * @param {ReturnType<typeof import('./profiles.js').bindProfiles>} profiles
 * @param {() => boolean} locked true while the game is starting or running
 */
export async function bindPacks(profiles, locked) {
  const dialog = control('pack-dialog', HTMLDialogElement);
  const install = control('pack-install', HTMLButtonElement);
  const linkInput = control('pack-link', HTMLInputElement);
  const importButtons = [control('pack-link-import', HTMLButtonElement), control('pack-file-import', HTMLButtonElement)];
  /** @type {PackReview | null} */
  let review = null;
  let busy = false;

  const checkUpdates = async () => {
    const found = array(await invoke('check_pack_updates'), (item) => {
      const value = object(item);
      return { id: string(value.id), version: string(value.version), url: string(value.url) };
    });
    profiles.setUpdates(new Map(found.map((u) => [u.id, { version: u.version, url: u.url }])));
  };

  /** @param {string} source a path, https link or starforge:// link */
  const open = async (source) => {
    if (busy || !source.trim()) return;
    busy = true;
    importButtons.forEach((b) => (b.disabled = true));
    const remote = /^(https?|starforge):/i.test(source.trim());
    if (remote) notice('Downloading the server pack…');
    try {
      const next = parseReview(await invoke('inspect_pack', { source }));
      if (review) await invoke('discard_pack', { token: review.token });
      review = next;
      profiles.closeDialog();
      renderReview(review);
      install.disabled = false;
      if (!dialog.open) dialog.showModal();
    } catch (error) {
      notice(`Could not open the server pack: ${errorMessage(error)}`, 'error');
    } finally {
      busy = false;
      importButtons.forEach((b) => (b.disabled = false));
    }
  };

  dialog.addEventListener('close', () => {
    const pending = review;
    review = null;
    if (pending) run(() => invoke('discard_pack', { token: pending.token }));
  });
  install.addEventListener('click', () => {
    const target = review;
    if (!target) return;
    if (locked()) {
      notice('Close the game before installing or updating a pack.', 'error');
      return;
    }
    install.disabled = true;
    // Installing uses the review up; the download window takes over when the
    // pack has game files to fetch.
    const installing = invoke('install_pack', { token: target.token });
    review = null;
    dialog.close();
    run(async () => {
      try {
        const id = string(await installing);
        await profiles.refresh(id);
        notice(target.update ? `Updated ${target.name} to ${target.version}.` : `Installed ${target.name}.`, 'success');
        run(checkUpdates);
      } finally {
        install.disabled = false;
      }
    });
  });
  importButtons[0].addEventListener('click', () => run(() => open(linkInput.value)));
  linkInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      run(() => open(linkInput.value));
    }
  });
  importButtons[1].addEventListener('click', () =>
    run(async () => {
      const path = await invoke('pick_pack_file');
      if (typeof path === 'string') await open(path);
    }),
  );
  profiles.onReviewUpdate((url) => run(() => open(url)));

  // One-click starts for the community servers that ship with the launcher.
  const samples = required('sample-packs');
  for (const item of array(await invoke('list_sample_packs'), object)) {
    const button = el('button', 'button', brand(string(item.name)));
    button.type = 'button';
    const key = string(item.key);
    button.addEventListener('click', () => run(async () => open(string(await invoke('sample_pack_source', { key })))));
    samples.append(button);
  }
  samples.hidden = samples.children.length < 2;

  // starforge:// links: listen first, then collect one that arrived earlier.
  await listen('pack-link', (url) => {
    if (typeof url === 'string') run(() => open(url));
  });
  const pending = await invoke('take_pending_link');
  if (typeof pending === 'string') run(() => open(pending));
  run(checkUpdates);
}
