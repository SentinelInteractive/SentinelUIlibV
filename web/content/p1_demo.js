// Boot layer proof of life (sentinelui://sui/p1_demo.js). The page paints nothing;
// this one line reaches the host log as "1|console:NUI_TEST:p1-boot bridge=...",
// which is how the P1 gate sees that the base page loaded and got its bridge.
// window.sui is injected before page scripts run, so this read is not a race.
(function () {
  var linked = !!(window.sui && window.sui._v === 1);
  console.log('NUI_TEST:p1-boot layer=1 bridge=' + (linked ? 'live' : 'missing'));
})();
