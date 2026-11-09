import { invoke } from './bridge.js';

// The splash is a still image on a transparent window. It is deliberately not
// animated: on Linux, WebKitGTK's fallback renderer (used where DMA-BUF is
// disabled, e.g. NVIDIA on Wayland) does not clear transparent pixels between
// frames, so anything that moves leaves a trail.
const SHOW_MS = 1800;

let finished = false;
const finish = () => {
  if (finished) return;
  finished = true;
  invoke('splash_finished').catch((error) => console.error('Splash completion failed', error));
};
const wordmark = document.getElementById('sting-wordmark');
if (!(wordmark instanceof HTMLImageElement)) {
  finish();
} else {
  wordmark.addEventListener('error', finish, { once: true });
  if (wordmark.complete && wordmark.naturalWidth === 0) finish();
  setTimeout(finish, SHOW_MS);
}
