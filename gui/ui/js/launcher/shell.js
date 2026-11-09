import { windowAction } from './bridge.js';
import { closePanels, control, openPanel, required, run } from './ui.js';

/** @param {(page: string) => void} onPage */
export function bindShell(onPage) {
  const root = required('launcher');
  /** @param {string} page */
  const navigate = (page) => {
    if (!['home', 'overview'].includes(page)) return;
    root.dataset.page = page;
    document.querySelectorAll('.content-area > section').forEach((section) => {
      if (section instanceof HTMLElement) section.hidden = section.id !== `${page}-page`;
    });
    document.querySelectorAll('.launch-dock').forEach((item) => {
      if (item instanceof HTMLElement) item.hidden = page === 'home';
    });
    document.querySelectorAll('.rail-button[data-page]').forEach((button) => {
      if (button.getAttribute('data-page') === page) button.setAttribute('aria-current', 'page');
      else button.removeAttribute('aria-current');
    });
    onPage(page);
  };
  document.addEventListener('click', (event) => {
    if (!(event.target instanceof Element)) return;
    const button = event.target.closest('button');
    if (!button) return;
    if (button.dataset.page) {
      closePanels();
      navigate(button.dataset.page);
    }
    const action = button.dataset.window;
    if (action === 'close' || action === 'minimize' || action === 'toggleMaximize')
      run(() => windowAction(action));
    if (button.hasAttribute('data-close-dialog')) button.closest('dialog')?.close();
    if (button.id === 'open-settings') openPanel(control('settings-dialog', HTMLDialogElement));
  });
  navigate('home');
  return { navigate };
}
