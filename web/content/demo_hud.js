// F11 HUD strip (sentinelui://sui/demo_hud.js). Click-through layer: this script
// only reads state and writes text, it never binds input.

var linkEl = document.getElementById('link');
var clockEl = document.getElementById('clock');

function pad(n) { return String(n).padStart(2, '0'); }

function paintClock() {
  var d = new Date();
  clockEl.textContent = pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
}

// window.sui is injected before page scripts run, so its presence is the real
// link state; the 2s re-check also covers a page opened in a plain browser.
function paintLink() {
  var live = !!(window.sui && window.sui._v === 1);
  linkEl.textContent = live ? 'LIVE' : 'OFFLINE';
  linkEl.dataset.state = live ? 'live' : 'offline';
}

paintLink();
paintClock();
setInterval(paintLink, 2000);

// One "NUI_HUD:hud-tick" line a minute reaches the host log ("<layer>|console:...")
// and proves this page is alive without taking input from any layer.
var ticks = 0;
setInterval(function () {
  paintClock();
  ticks++;
  if (ticks % 60 === 0) console.log('NUI_HUD:hud-tick ' + ticks);
}, 1000);
