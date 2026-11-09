import { invoke } from './bridge.js';
import { control, required, errorMessage, run } from './ui.js';

/** @template T @param {Promise<T>} pending @param {string} label @param {number} [timeout] */
export async function startupStep(pending, label, timeout = 15000) {
  /** @type {ReturnType<typeof setTimeout> | undefined} */
  let timer;
  try {
    return await Promise.race([
      pending,
      new Promise((_, reject) => {
        timer = setTimeout(
          () => reject(new Error(`${label} is taking longer than expected. Retry startup.`)),
          timeout,
        );
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

/** @param {unknown} error */
export async function showStartupFailure(error) {
  const dialog = control('startup-dialog', HTMLDialogElement);
  required('startup-error').textContent = errorMessage(error);
  required('retry-startup').addEventListener('click', () => window.location.reload());
  required('exit-startup').addEventListener('click', () => run(() => invoke('exit_launcher')));
  dialog.addEventListener('cancel', (event) => event.preventDefault());
  dialog.showModal();
  await invoke('main_ready');
}
