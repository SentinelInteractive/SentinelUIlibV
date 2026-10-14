// Badge layer script (sentinelui://sui/badge.js). Click-through panel: it paints
// the clock, the runtime link state and the game-process CPU, and binds no input.

var clockEl = document.getElementById('clock');
var cpuEl = document.getElementById('cpu');
var linkEl = document.getElementById('link');

function pad(n) { return String(n).padStart(2, '0'); }

function paintClock() {
  var d = new Date();
  clockEl.textContent = pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
}

// window.sui is injected before page scripts run, so its presence is the real
// link state; the 2s re-check also covers a page opened in a plain browser.
function linkLive() { return !!(window.sui && window.sui._v === 1); }

function paintLink() {
  var live = linkLive();
  linkEl.textContent = live ? 'LIVE' : 'OFFLINE';
  linkEl.dataset.state = live ? 'live' : 'offline';
  if (!live) cpuEl.textContent = 'CPU n/a';
}

// CPU of the GAME process, answered by the Bootstrap's `sys.stats` request: a page
// cannot read process stats itself. Task-Manager style, percent of all cores.
// "..." is the pending state (no reply yet), "n/a" means no answer at all.
var inFlight = false;
function pollStats() {
  if (inFlight || !linkLive()) return;
  inFlight = true;
  try {
    window.sui.request('sys.stats', null, 2000).then(function (s) {
      inFlight = false;
      cpuEl.textContent = (s && typeof s.cpu === 'number') ? 'CPU ' + s.cpu + '%' : 'CPU ...';
    }, function () {
      inFlight = false;
      cpuEl.textContent = 'CPU n/a';
    });
  } catch (e) {
    inFlight = false;
    cpuEl.textContent = 'CPU n/a';
  }
}

paintClock();
paintLink();
pollStats();
setInterval(paintClock, 1000);
setInterval(pollStats, 1000);
setInterval(paintLink, 2000);
