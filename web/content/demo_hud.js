// SentinelUI P2 demo HUD script (served via sentinelui://sui/demo_hud.js).

const clock = document.getElementById('clock');
function pad(n) { return String(n).padStart(2, '0'); }
setInterval(() => {
  const d = new Date();
  clock.textContent = pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
}, 1000);

// Visible on the ASI's host log as "2|console:NUI_HUD:hud-tick" once per minute,
// proving this layer's page is alive without stealing any input.
let ticks = 0;
setInterval(() => {
  ticks++;
  if (ticks % 60 === 0) console.log('NUI_HUD:hud-tick ' + ticks);
}, 1000);
