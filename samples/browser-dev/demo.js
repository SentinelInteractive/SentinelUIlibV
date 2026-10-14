// Browser-dev demo page. Loads after sui-bridge.js; nothing here is
// runtime-specific, so the same calls run unchanged in-game.

// Per-type answers for this page. The shim routes them back through
// window.sui._deliver, the entry point the runtime uses for host -> page.
window.suiDev = {
  'demo.echo': function (payload) {
    return { got: payload, note: 'answered by window.suiDev in demo.js' };
  },
  'demo.custom': function () {
    return { custom: true, at: new Date().toLocaleTimeString() };
  },
  'demo.slow': function () {
    return new Promise(function (resolve) { setTimeout(function () { resolve('late'); }, 3000); });
  }
};

var outEl = document.getElementById('out');
var emptyEl = document.getElementById('empty');
var linkEl = document.getElementById('link');
var noteEl = document.getElementById('linknote');

var MAX_ENTRIES = 12;

function bridge() {
  return (window.sui && window.sui._v === 1) ? window.sui : null;
}

function paintLink() {
  var live = !!bridge();
  linkEl.textContent = live ? 'LIVE' : 'OFFLINE';
  linkEl.dataset.state = live ? 'live' : 'offline';
  noteEl.textContent = live
    ? 'window.sui from sui-bridge.js, envelope v1'
    : 'window.sui missing: sui-bridge.js must load before demo.js';
}

// label is what happened (RESOLVED / REJECTED / SENT / EVENT); detail is the
// payload the call produced, printed as JSON unless it is already text.
function addEntry(label, kind, call, detail) {
  emptyEl.hidden = true;

  var li = document.createElement('li');
  li.className = 'entry';

  var head = document.createElement('div');
  head.className = 'entryhead';

  var tag = document.createElement('span');
  tag.className = 'tag';
  tag.dataset.kind = kind;
  tag.textContent = label;

  var callEl = document.createElement('span');
  callEl.className = 'call';
  callEl.textContent = call;

  var body = document.createElement('pre');
  body.textContent = (typeof detail === 'string') ? detail : JSON.stringify(detail, null, 2);

  head.appendChild(tag);
  head.appendChild(callEl);
  li.appendChild(head);
  li.appendChild(body);
  outEl.insertBefore(li, outEl.firstChild);

  while (outEl.children.length > MAX_ENTRIES) outEl.removeChild(outEl.lastChild);
}

// With no shim loaded, a click reports the missing bridge instead of throwing.
function suiCall(call, action) {
  var s = bridge();
  if (!s) {
    addEntry('REJECTED', 'err', call, 'window.sui is missing: load sui-bridge.js before demo.js.');
    return;
  }
  action(s);
}

function report(call, promise) {
  promise.then(function (data) {
    addEntry('RESOLVED', 'ok', call, data);
  }, function (err) {
    addEntry('REJECTED', 'err', call, String(err));
  });
}

document.getElementById('echo').addEventListener('click', function () {
  var call = "sui.request('demo.echo', { n: 42 })";
  suiCall(call, function (s) { report(call, s.request('demo.echo', { n: 42 })); });
});

document.getElementById('send').addEventListener('click', function () {
  var call = "sui.send('demo.event', { hello: 'host' })";
  suiCall(call, function (s) {
    s.send('demo.event', { hello: 'host' });
    addEntry('SENT', 'sent', call,
      'fire and forget: no reply is expected. The envelope is on the console at debug level.');
  });
});

document.getElementById('custom').addEventListener('click', function () {
  var call = "sui.request('demo.custom')";
  suiCall(call, function (s) { report(call, s.request('demo.custom', null)); });
});

// demo.slow answers after 3s while the call gives up at 1.2s, so the rejection is
// the expected result and the late answer is dropped by the shim.
document.getElementById('slow').addEventListener('click', function () {
  var call = "sui.request('demo.slow', 1200)";
  suiCall(call, function (s) { report(call, s.request('demo.slow', null, 1200)); });
});

document.getElementById('missing').addEventListener('click', function () {
  var call = "sui.request('demo.missing')";
  suiCall(call, function (s) { report(call, s.request('demo.missing', null)); });
});

document.getElementById('push').addEventListener('click', function () {
  var call = "sui._deliver({ kind: 'evt', type: 'demo.pushed' })";
  suiCall(call, function (s) {
    s._deliver({
      v: 1, kind: 'evt', type: 'demo.pushed',
      payload: { from: 'the page', at: new Date().toLocaleTimeString() }
    });
    addEntry('SENT', 'sent', call, 'in-game a plugin calls SuiLayer.Send; the handler below prints it.');
  });
});

var shim = bridge();
if (shim) {
  shim.on('demo.pushed', function (payload) {
    addEntry('EVENT', 'sent', "sui.on('demo.pushed')", payload);
  });
}

document.getElementById('clear').addEventListener('click', function () {
  outEl.textContent = '';
  emptyEl.hidden = false;
});

paintLink();
setInterval(paintLink, 2000);
