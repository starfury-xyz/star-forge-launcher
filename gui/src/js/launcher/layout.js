import { required, renderIcons } from './ui.js';

export function mountLayout() {
  required('launcher').innerHTML = `
    <dialog id="startup-dialog" aria-labelledby="startup-title"><h2 id="startup-title">Launcher could not finish starting</h2><p id="startup-error"></p><div class="setting-buttons"><button id="retry-startup" class="button">Retry startup</button><button id="exit-startup" class="button">Exit launcher</button></div></dialog>
    <nav class="rail" aria-label="Launcher navigation">
      <button class="brand-button" data-page="home" aria-label="STAR FORGE Launcher home"><img src="assets/starforge-emblem.svg" alt="" /></button>
      <button class="rail-button" data-page="home" aria-current="page"><span data-icon="home"></span><span>Home</span></button>
      <button class="rail-button" data-page="overview" aria-label="Star Conflict"><img class="game-emblem" src="assets/starforge-emblem.svg" alt="" /><span>Star Conflict</span></button>
      <span class="rail-spacer"></span>
      <button id="open-downloads" class="rail-button download-rail" aria-label="Show download progress" hidden><span class="download-ring"><svg viewBox="0 0 36 36" aria-hidden="true"><circle class="ring-track" cx="18" cy="18" r="15"/><circle id="download-ring" class="ring-value" cx="18" cy="18" r="15" pathLength="100" stroke-dasharray="0 100"/></svg><span data-icon="download"></span></span><span id="download-rail-label">0%</span></button>
      <button id="open-settings" class="rail-button"><span data-icon="settings"></span><span>Settings</span></button>
    </nav>
    <main class="main-surface">
      <div class="artwork" aria-hidden="true"></div>
      <div class="drag-area" data-tauri-drag-region></div>
      <div class="window-controls" aria-label="Window controls">
        <button data-window="minimize" aria-label="Minimize"><span data-icon="minus"></span></button>
        <button data-window="toggleMaximize" aria-label="Maximize or restore"><span data-icon="square"></span></button>
        <button data-window="close" aria-label="Close"><span data-icon="close"></span></button>
      </div>
      <img id="game-wordmark" class="game-wordmark" src="assets/starforge-logo.svg" alt="STAR FORGE" />
      <div class="content-area">
        <section id="home-page" class="home-page">
          <h1>Home</h1><div class="section-heading"><h2>Your games</h2><span>1 game</span></div>
          <button class="game-card" data-page="overview" aria-label="Open Star Conflict">
            <span class="game-card-art"></span><span class="game-card-info"><img class="game-emblem" src="assets/starforge-emblem.svg" alt="" /><span><strong>Star Conflict</strong><small>Open game</small></span><span data-icon="arrow"></span></span>
          </button>
        </section>
        <section id="overview-page" class="overview-page" hidden>
          <div class="profile-summary"><span class="eyebrow">Server profile</span><h1 id="summary-name">No profile selected</h1>
            <p id="summary-description" class="summary-description" hidden></p>
            <dl class="profile-facts"><dt>Server</dt><dd id="summary-server">–</dd><dt>Starts</dt><dd id="summary-mode">–</dd><dt>Game patches</dt><dd id="summary-patches">–</dd><dt>Pack version</dt><dd id="summary-version">–</dd></dl>
            <div id="summary-update-row" class="update-row" hidden><span id="summary-update-text"></span><button class="button primary" id="summary-update">Review update</button></div>
            <div id="summary-links" class="setting-buttons summary-links" hidden></div>
            <div class="setting-buttons"><button class="button" id="summary-edit">Edit profile</button><button class="button" id="summary-folder">Open profile folder</button></div>
          </div>
        </section>
        <div id="launcher-update" class="update-row" hidden><span id="launcher-update-text"></span><span class="spacer"></span><button class="button" id="launcher-update-notes" hidden>Release notes</button><button class="button primary" id="launcher-update-install">Update and restart</button></div>
      </div>
      <div id="notices" class="notices" aria-live="polite" aria-relevant="additions"></div>
      <footer class="launch-dock" hidden>
        <button id="play-button" class="primary-button" disabled><span id="play-icon" data-icon="play"></span><span id="play-label">Checking</span></button>
        <div class="launch-details"><span id="launch-status" role="status">Checking launcher…</span><div class="server-row"><span data-icon="globe"></span><select id="profile-select" aria-label="Server profile" disabled><option value="">Loading profiles…</option></select><button id="add-profile" class="icon-button" aria-label="Add server profile" title="Add server profile"><span data-icon="plus"></span></button><button id="edit-profile" class="icon-button" aria-label="Edit server profile" title="Edit server profile" disabled><span data-icon="edit"></span></button></div></div>
      </footer>
    </main>`;
  const dialogs = document.createElement('div');
  dialogs.innerHTML = `<dialog id="settings-dialog" class="settings-dialog framed"><header><h1>Settings</h1><button class="icon-button" data-close-dialog aria-label="Close settings"><span data-icon="close"></span></button></header><nav class="settings-tabs" aria-label="Settings"><button data-settings-tab="launcher" aria-current="page">Launcher</button><button data-settings-tab="game">Star Conflict</button></nav><div class="dialog-body"><div id="settings-content"></div><footer id="launcher-version"></footer></div></dialog>
    <dialog id="profile-dialog" class="profile-dialog framed" aria-labelledby="profile-dialog-title"><header><h1 id="profile-dialog-title">Add server profile</h1><button class="icon-button" data-close-dialog aria-label="Close"><span data-icon="close"></span></button></header><div class="dialog-body">
      <section id="pack-import" class="pack-import" aria-labelledby="pack-import-title">
        <h2 id="pack-import-title">Import a server pack</h2>
        <p>Got a pack link or zip from a server? It brings the server address, files and look in one go.</p>
        <div class="pack-import-row"><input id="pack-link" type="text" placeholder="https://… or starforge://…" spellcheck="false" autocomplete="off" aria-label="Pack link" /><button type="button" class="button" id="pack-link-import">Import</button><button type="button" class="button" id="pack-file-import">Choose zip…</button></div>
        <div id="sample-packs" class="sample-packs" hidden><span>Community servers</span></div>
        <div class="divider"><span>or set one up yourself</span></div>
      </section>
      <form id="profile-form" method="dialog">
        <p id="profile-managed-note" class="field-note" hidden>This profile comes from a server pack. Changes you make here are kept when the pack updates.</p>
        <label class="field">Name<input id="profile-name" type="text" maxlength="64" required placeholder="STARFURY community server" /></label>
        <label class="field">Server address<input id="profile-server" type="text" maxlength="255" placeholder="STARFURY.XYZ:3801" spellcheck="false" autocomplete="off" /><small>Filled in on the login screen. Leave empty to keep the game's own setting.</small></label>
        <label class="check-field"><input id="profile-local-server" type="checkbox" /><span>Start the local server with the game<small>Runs the bundled MongoDB and Hub, as Steam's default launch does. Leave off for profiles that play on someone else's server.</small></span></label>
        <label class="check-field"><input id="profile-own-data" type="checkbox" /><span>Keep this server's game data separate<small>The game's store cache, notes and local server database stay with this profile, so other servers never see them. Graphics settings and key bindings are shared by all profiles. Turn off to use the game's normal data folder, where any existing local server data is.</small></span></label>
        <p id="profile-error" class="form-error" hidden></p>
        <div class="dialog-actions"><button type="button" class="button danger" id="delete-profile" hidden>Delete</button><button type="button" class="button" id="profile-folder" hidden>Open folder</button><span class="spacer"></span><button type="button" class="button" data-close-dialog>Cancel</button><button type="submit" class="button primary" id="save-profile">Save</button></div>
      </form></div></dialog>
    <dialog id="pack-dialog" class="pack-dialog framed" aria-labelledby="pack-title"><header><h1 id="pack-title">Install server pack</h1><button class="icon-button" data-close-dialog aria-label="Close"><span data-icon="close"></span></button></header>
      <div id="pack-body" class="pack-body dialog-body"></div>
      <div class="dialog-actions"><span class="spacer"></span><button class="button" data-close-dialog>Cancel</button><button class="button primary" id="pack-install">Install</button></div></dialog>
    <dialog id="download-dialog" class="download-dialog framed" aria-labelledby="download-title" data-state="running"><header><div><span id="download-eyebrow" class="eyebrow">Downloading</span><h1 id="download-title">–</h1></div><button class="icon-button" data-close-dialog aria-label="Hide download window" title="Hide (keeps downloading)"><span data-icon="close"></span></button></header>
      <div class="dialog-body download-body">
        <div class="download-readout"><span id="download-percent" class="download-percent">0%</span><span id="download-state" class="download-state">Live</span><span class="spacer"></span><span id="download-amount" class="download-amount"></span></div>
        <div id="download-bar" class="download-bar" role="progressbar" aria-labelledby="download-title" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0"><span class="download-bar-fill"></span></div>
        <div class="download-current"><span id="download-file" class="download-file"></span><span id="download-count" class="download-count"></span></div>
        <dl class="download-stats"><div><dt>Speed</dt><dd id="download-speed">–</dd></div><div><dt>ETA</dt><dd id="download-eta">–</dd></div><div><dt>Elapsed</dt><dd id="download-elapsed">0:00</dd></div><div class="download-graph" title="Throughput, last 30 seconds"><svg viewBox="0 0 60 20" preserveAspectRatio="none" aria-hidden="true"><polygon id="download-graph-fill"/><polyline id="download-graph-line"/></svg></div></dl>
        <div id="download-console" class="download-console" role="log" aria-label="Download log"></div>
      </div>
      <div class="dialog-actions"><span class="spacer"></span><button class="button" id="download-hide" data-close-dialog>Hide</button><button class="button danger" id="download-cancel">Cancel download</button></div></dialog>
    <dialog id="confirm-dialog" aria-labelledby="confirm-title"><h2 id="confirm-title">Delete profile?</h2><p id="confirm-text"></p><div class="dialog-actions"><span class="spacer"></span><button class="button" data-close-dialog>Cancel</button><button class="button danger" id="confirm-delete">Delete</button></div></dialog>`;
  document.body.append(dialogs);
  renderIcons();
}
