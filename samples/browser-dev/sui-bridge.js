// sui-bridge.js — browser stand-in for the in-game window.sui bridge.
//
// In-game the runtime injects the real bridge automatically. For developing a page
// in a normal browser, load this file BEFORE your page scripts and
// window.sui.send/on/request will work the same way.
//
//   <script src="sui-bridge.js"></script>
//   <script src="app.js"></script>
//
// Fake request replies per type (optional):
//   window.suiDev = {
//     'ped.scanClosest': function (payload) { return { name: 'Dev Ped', warrant: false }; },
//     'vehicle.lookup': async function (payload) { return { plate: payload.plate, stolen: false }; }
//   };
//
// Keep pages free of any runtime-specific code beyond window.sui: they then run
// unchanged in-game and in the browser.
(function () {
    "use strict";
    if (window.sui && window.sui._v === 1) return;

    var seq = 0;
    var pending = {};
    var listeners = {};

    function emit(type, payload) {
        var ls = listeners[type] || [];
        for (var i = 0; i < ls.length; i++) {
            try { ls[i](payload); } catch (e) { console.error("[sui] handler error", e); }
        }
        var all = listeners["*"] || [];
        for (var j = 0; j < all.length; j++) {
            try { all[j](payload, type); } catch (e) { console.error("[sui] handler error", e); }
        }
    }

    function resolve(id, payload) {
        var p = pending[id];
        if (!p) return;
        delete pending[id];
        p.resolve(payload);
    }

    function reject(id, error) {
        var p = pending[id];
        if (!p) return;
        delete pending[id];
        p.reject(error);
    }

    window.sui = {
        _v: 1,

        send: function (type, payload) {
            console.log("[sui.send]", type, payload);
            return Promise.resolve();
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
            console.log("[sui.request]", type, payload);
            return new Promise(function (resolveFn, rejectFn) {
                var id = ++seq;
                pending[id] = { resolve: resolveFn, reject: rejectFn };
                setTimeout(function () {
                    if (!pending[id]) return;
                    delete pending[id];
                    rejectFn("sui request timeout: " + type);
                }, timeoutMs || 5000);

                setTimeout(function () {
                    if (!pending[id]) return;
                    var dev = (window.suiDev || {})[type];
                    if (dev) {
                        try {
                            var result = dev(payload);
                            if (result && typeof result.then === "function") {
                                result.then(function (r) { resolve(id, r); },
                                            function (e) { reject(id, String(e)); });
                            } else {
                                resolve(id, result);
                            }
                        } catch (e) {
                            reject(id, String(e));
                        }
                    } else {
                        resolve(id, { echo: payload, type: type, note: "browser dev shim" });
                    }
                }, 150);
            });
        },

        // Helpers the in-game bridge exposes internally; handy for manual testing here.
        _resolve: resolve,
        _reject: reject,
        _emit: emit
    };
})();
