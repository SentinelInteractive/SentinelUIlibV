// SentinelUI P1/P2 demo page script (served via sentinelui://sui/p1_demo.js).
// The scheme's CSP allows external scripts only — inline <script> is blocked.

const clock = document.getElementById('clock');
const logline = document.getElementById('logline');
const panel = document.querySelector('.panel');

function pad(n) { return String(n).padStart(2, '0'); }
setInterval(() => {
  const d = new Date();
  clock.textContent = pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
}, 1000);

document.getElementById('ping').addEventListener('click', () => {
  // JS -> host bridge: forwarded as a layer-tagged console event
  // ("1|console:NUI_TEST:...") to the ASI, then to Sui.MessageReceived.
  console.log('NUI_TEST:p1-button-clicked');
  logline.textContent = 'clock is live (JS repaint works)';
});

document.getElementById('togglebg').addEventListener('click', () => {
  const bg = getComputedStyle(panel).backgroundColor;
  panel.style.background = bg.indexOf('0.72') !== -1 ? 'rgba(10,14,22,0.96)' : 'rgba(10,14,22,0.72)';
});

// Envelope receive demo (SuiLayer.Send posts window messages at the page).
window.addEventListener('message', (e) => {
  if (e.data && e.data.type === 'ping') {
    console.log('NUI_TEST:envelope-received ' + JSON.stringify(e.data));
  }
});
