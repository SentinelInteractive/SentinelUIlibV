// sui-bridge.js: browser stand-in for the in-game window.sui bridge.
//
// In-game the runtime injects the real bridge before any page script runs, so a
// page never includes this file. In a plain browser, load it first and
// window.sui.send/on/request behave the same way:
//
//   <script src="sui-bridge.js"></script>
//   <script src="app.js"></script>
//
// Answers for request types are optional, per type:
//
//   window.suiDev = {
//     'ped.scanClosest': function (payload) { return { name: 'Dev Ped' }; },
//     'vehicle.lookup': async function (payload) { return { plate: payload.plate }; }
//   };
//
// A suiDev handler may return a value, return a promise, or throw; a request with
// no entry for its type rejects the way a host without a handler does, so a page's
// error state is reachable here.
//
// Wire shape (envelope v1), identical to the runtime:
//   page -> host   {v:1, kind:'evt'|'req', type, id, payload}
//   host -> page   {v:1, kind:'res', type, id, ok, payload}   or  {kind:'evt'}
(function () {
    "use strict";
    if (window.sui && window.sui._v === 1) return;

    var HOST_DELAY_MS = 150;   // simulated round trip, so loading states are visible
    var seq = 0;
    var pending = {};
    var listeners = {};

    function post(env) {
        // Call traffic is verbose on purpose; problems log at warn/error level.
        try { console.debug('[sui -> host]', env); } catch (e) {}
    }

    function emit(type, payload, env) {
        var typed = listeners[type] || [];
        for (var i = 0; i < typed.length; i++) {
            try { typed[i](payload, env); } catch (e) { console.error('[sui] handler error', e); }
        }
        var all = listeners['*'] || [];
        for (var j = 0; j < all.length; j++) {
            try { all[j](payload, env); } catch (e) { console.error('[sui] handler error', e); }
        }
    }

    // The runtime's host -> page entry point. Pages can call it to simulate a plugin
    // push, and request replies travel through it, so a reply takes the same path
    // in a browser as it does in-game (including the window.postMessage copy).
    function deliver(env) {
        try { window.postMessage(env, '*'); } catch (e) {}
        if (!env) return;
        if (env.kind === 'res' && env.id != null) {
            var p = pending[env.id];
            if (!p) return;
            delete pending[env.id];
            if (env.ok === false) p.reject(env.payload); else p.resolve(env.payload);
            return;
        }
        if (env.type) emit(env.type, env.payload, env);
    }

    function reply(type, id, ok, payload) {
        setTimeout(function () {
            if (!pending[id]) return;   // the caller timed out while the answer travelled
            deliver({ v: 1, kind: 'res', type: type, id: id, ok: ok, payload: payload });
        }, HOST_DELAY_MS);
    }

    function answerTo(type, payload, id) {
        var dev = (window.suiDev || {})[type];
        if (typeof dev === 'undefined') {
            console.warn('[sui] no window.suiDev entry for "' + type + '"');
            reply(type, id, false, 'browser dev shim: no handler for ' + type);
            return;
        }
        var result;
        try {
            result = (typeof dev === 'function') ? dev(payload) : dev;
        } catch (e) {
            reply(type, id, false, String(e));
            return;
        }
        if (result && typeof result.then === 'function') {
            result.then(
                function (value) { reply(type, id, true, value); },
                function (err) { reply(type, id, false, String(err)); }
            );
            return;
        }
        reply(type, id, true, result);
    }

    window.sui = {
        _v: 1,

        send: function (type, payload) {
            post({ v: 1, kind: 'evt', type: String(type), payload: payload });
        },

        on: function (type, handler) {
            var t = String(type);
            (listeners[t] = listeners[t] || []).push(handler);
            return function () {
                var a = listeners[t];
                if (!a) return;
                var i = a.indexOf(handler);
                if (i >= 0) a.splice(i, 1);
            };
        },

        request: function (type, payload, timeoutMs) {
            var t = String(type);
            return new Promise(function (resolve, reject) {
                var id = ++seq;
                pending[id] = { resolve: resolve, reject: reject };
                post({ v: 1, kind: 'req', type: t, id: id, payload: payload });
                answerTo(t, payload, id);
                if (timeoutMs) {
                    setTimeout(function () {
                        if (!pending[id]) return;
                        delete pending[id];
                        reject('sui request timeout: ' + t);
                    }, timeoutMs);
                }
            });
        },

        _deliver: deliver
    };
})();
