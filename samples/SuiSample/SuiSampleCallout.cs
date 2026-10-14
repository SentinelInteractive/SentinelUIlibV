using System;
using System.Globalization;
using System.Windows.Forms;

using LSPD_First_Response.Mod.Callouts;
using Rage;
using SentinelUI;

namespace SuiSample
{
    /// <summary>
    /// Quick LSPDFR callout that opens a SentinelUI panel from LSPDFR's AppDomain.
    ///
    /// Cross-domain client path: the callout does not own the host (Bootstrap does).
    /// It attaches as a client, creates an inline-HTML layer, pumps its own event queue
    /// in Process() and closes the layer in End(). Registered on duty by <see cref="Main"/>;
    /// trigger with the RPH console: ForceCallout SuiSample (it can also spawn on its own).
    /// </summary>
    [CalloutInfo("SuiSample", CalloutProbability.Low)]
    public sealed class SuiSampleCallout : Callout
    {
        private SuiLayer _ui;
        private bool _acknowledged;

        static SuiSampleCallout()
        {
            ApiResolver.Ensure();
        }

        public override bool OnBeforeCalloutDisplayed()
        {
            CalloutMessage = "SentinelUI sample";
            CalloutPosition = Game.LocalPlayer.Character.Position;
            ShowCalloutAreaBlipBeforeAccepting(CalloutPosition, 30f);
            return base.OnBeforeCalloutDisplayed();
        }

        public override bool OnCalloutAccepted()
        {
            try
            {
                Game.LogTrivial("[SuiSample] callout accepted; attaching SentinelUI client...");
                if (!Sui.AttachClient())
                {
                    Game.LogTrivial("[SuiSample] SentinelUI host not loaded (ScriptHookV/ASI missing); ending.");
                    End();
                    return true;
                }

                _ui = Sui.CreateLayerFromHtml(SampleHtml);
                if (_ui == null)
                {
                    Game.LogTrivial("[SuiSample] CreateLayerFromHtml refused; ending.");
                    End();
                    return true;
                }

                _ui.Message += OnUiMessage;
                _ui.Request += OnUiRequest;
                _ui.SetVisible(true);
                _ui.SetZ(50);
                Game.DisplayNotification("~b~SentinelUI~w~ sample panel open. Press ~y~F10~w~ to click, ~y~End~w~ to close.");
                Game.LogTrivial("[SuiSample] layer created (native id " + _ui.NativeId + ").");
            }
            catch (Exception ex)
            {
                Game.LogTrivial("[SuiSample] OnCalloutAccepted failed: " + ex);
                End();
            }
            return base.OnCalloutAccepted();
        }

        public override void Process()
        {
            base.Process();
            try
            {
                Sui.Pump();   // this domain's event queue; handlers run on this (game) thread
                if (Game.IsKeyDown(Keys.End)) End();
            }
            catch (Exception ex)
            {
                Game.LogTrivial("[SuiSample] Process failed: " + ex);
                End();
            }
        }

        public override void End()
        {
            try
            {
                if (_ui != null)
                {
                    _ui.Message -= OnUiMessage;
                    _ui.Request -= OnUiRequest;
                    _ui.Close();
                    _ui = null;
                    Game.LogTrivial("[SuiSample] layer closed.");
                }
            }
            catch (Exception ex)
            {
                Game.LogTrivial("[SuiSample] End cleanup failed: " + ex);
            }
            base.End();
        }

        private void OnUiMessage(SuiMessage message)
        {
            // Handlers run from Sui.Pump() in Process(): game thread, natives are legal here.
            if (message == null || message.Type == null) return;
            Game.LogTrivial("[SuiSample] page event: " + message.Type + " " + (message.PayloadJson ?? "null"));

            if (message.Type == "sample.close" && _ui != null) End();
        }

        private void OnUiRequest(SuiRequest request)
        {
            Game.LogTrivial("[SuiSample] page request: " + request.Type);
            switch (request.Type)
            {
                case "sample.scene":
                    var position = Game.LocalPlayer.Character.Position;
                    request.Reply(new
                    {
                        street = World.GetStreetName(position),
                        x = position.X,
                        y = position.Y,
                        z = position.Z,
                    });
                    break;

                case "sample.ack":
                    if (!_acknowledged)
                    {
                        _acknowledged = true;
                        Game.DisplayNotification("~g~Acknowledged~w~: SentinelUI request reached the callout.");
                    }
                    request.Reply(new { at = DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture) });
                    break;

                default:
                    request.ReplyError("unknown request: " + request.Type);
                    break;
            }
        }

        private const string SampleHtml = @"<!DOCTYPE html>
<html lang=""en"">
<head><meta charset=""utf-8""><title>SuiSample panel</title><style>
  :root{--base:#10141b;--panel:#171d27;--line:#2b3543;--hair:#222b37;--edge:#5c6a80;
        --text:#e7e5df;--muted:#9ba3ae;--accent:#e8a13d;--ok:#4cc38a;--err:#ef6b6b;}
  *{box-sizing:border-box;}
  body{margin:0;background:transparent;color:var(--text);
       font:13px/1.45 'Segoe UI',system-ui,sans-serif;}
  .panel{position:fixed;left:clamp(12px,3vw,48px);top:clamp(12px,6vh,96px);
         width:min(390px,calc(100vw - 24px));max-height:calc(100vh - 24px);overflow-y:auto;
         background:var(--base);border:1px solid var(--line);border-radius:6px;}
  .strip{display:flex;align-items:baseline;gap:10px;padding:7px 14px;background:var(--panel);
         border-bottom:1px solid var(--line);font-size:11px;letter-spacing:.06em;
         text-transform:uppercase;}
  .strip .state{color:var(--ok);}
  .strip .state.off{color:var(--err);}
  .strip .clock{margin-left:auto;font-family:'Cascadia Mono',Consolas,monospace;
                font-size:11px;letter-spacing:0;color:var(--muted);}
  .body{padding:14px 14px 16px;}
  h1{font-size:14px;font-weight:600;margin:0 0 2px;}
  .sub{margin:0;font-size:12px;color:var(--muted);}
  .section{margin-top:14px;padding-top:12px;border-top:1px solid var(--hair);}
  .label{font-size:11px;letter-spacing:.06em;text-transform:uppercase;color:var(--muted);
         margin:0 0 8px;}
  dl{display:grid;grid-template-columns:92px 1fr;gap:6px 10px;margin:0;}
  dt{font-size:11px;letter-spacing:.04em;text-transform:uppercase;color:var(--muted);padding-top:2px;}
  dd{margin:0;font-size:13px;}
  .mono{font-family:'Cascadia Mono',Consolas,monospace;font-size:12px;}
  .note{margin:0;font-size:12px;color:var(--muted);}
  .note.err{color:var(--err);}
  .retry{margin-top:10px;}
  .actions{display:flex;flex-wrap:wrap;gap:10px;margin-top:16px;}
  .btn{min-height:44px;padding:0 16px;font:inherit;font-size:13px;border-radius:4px;
       border:1px solid var(--edge);background:#1b2230;color:var(--text);
       transition:background-color .12s linear;}
  .btn:hover:not(:disabled){background:#232c3b;}
  .btn:focus-visible{outline:2px solid var(--accent);outline-offset:2px;}
  .btn:disabled{color:var(--muted);background:#151a23;border-color:#3a4557;}
  .btn-primary{border-color:var(--accent);background:transparent;color:var(--accent);}
  .btn-primary:hover:not(:disabled){background:rgba(232,161,61,.14);}
  .status{margin:12px 0 0;font-size:12px;color:var(--muted);}
  .status.ok{color:var(--ok);}
  .status.err{color:var(--err);}
  @media (max-width:430px){
    .panel{left:12px;right:12px;width:auto;}
    dl{grid-template-columns:1fr;gap:2px;}
    dd{margin-bottom:6px;}
    .actions .btn{flex:1 1 100%;}
  }
  @media (prefers-reduced-motion:reduce){.btn{transition:none;}}
</style></head><body>
<div class=""panel"">
  <div class=""strip"">
    <span>SENTINEL</span>
    <span class=""state"" id=""link"">LIVE</span>
    <span class=""clock"" id=""clock"">--:--:--</span>
  </div>
  <div class=""body"">
    <h1>SuiSample callout</h1>
    <p class=""sub"">Inline HTML panel, opened from LSPDFR's AppDomain.</p>
    <div class=""section"">
      <p class=""label"">Scene data</p>
      <div id=""scene"" aria-live=""polite"" aria-busy=""true"">
        <p class=""note"">Reading scene data...</p>
      </div>
    </div>
    <div class=""actions"">
      <button type=""button"" class=""btn btn-primary"" id=""ack"">Acknowledge</button>
      <button type=""button"" class=""btn"" id=""close"">Close callout</button>
    </div>
    <p class=""status"" id=""status"" role=""status"">Not acknowledged</p>
  </div>
</div>
<script>
(function () {
  var bridge = (window.sui && window.sui._v === 1) ? window.sui : null;
  var sceneEl = document.getElementById('scene');
  var statusEl = document.getElementById('status');
  var ackBtn = document.getElementById('ack');
  var closeBtn = document.getElementById('close');

  function pad(n) { return (n < 10 ? '0' : '') + n; }
  function tick() {
    var d = new Date();
    document.getElementById('clock').textContent =
      pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
  }
  tick();
  setInterval(tick, 1000);

  function setStatus(kind, text) {
    statusEl.className = kind ? 'status ' + kind : 'status';
    statusEl.textContent = text;
  }

  function sceneNote(text, isError) {
    var p = document.createElement('p');
    p.className = isError ? 'note err' : 'note';
    p.textContent = text;
    sceneEl.replaceChildren(p);
  }

  function sceneRows(data) {
    var dl = document.createElement('dl');
    var rows = [
      ['Street', (data && data.street) ? String(data.street) : 'No street data at this position', false],
      ['Position', (data && typeof data.x === 'number')
        ? data.x.toFixed(1) + ', ' + data.y.toFixed(1) + ', ' + data.z.toFixed(1)
        : 'Unavailable', true]
    ];
    for (var i = 0; i < rows.length; i++) {
      var dt = document.createElement('dt');
      dt.textContent = rows[i][0];
      var dd = document.createElement('dd');
      if (rows[i][2]) dd.className = 'mono';
      dd.textContent = rows[i][1];
      dl.appendChild(dt);
      dl.appendChild(dd);
    }
    sceneEl.replaceChildren(dl);
  }

  function sceneError() {
    var p = document.createElement('p');
    p.className = 'note err';
    p.textContent = 'No reply from the callout.';
    var retry = document.createElement('button');
    retry.type = 'button';
    retry.className = 'btn retry';
    retry.textContent = 'Retry scene request';
    retry.addEventListener('click', loadScene);
    sceneEl.replaceChildren(p, retry);
  }

  function loadScene() {
    sceneEl.setAttribute('aria-busy', 'true');
    sceneNote('Reading scene data...');
    bridge.request('sample.scene', null, 4000).then(function (data) {
      sceneEl.setAttribute('aria-busy', 'false');
      sceneRows(data);
    }, function () {
      sceneEl.setAttribute('aria-busy', 'false');
      sceneError();
    });
  }

  function closeCallout() {
    setStatus('', 'Close request sent.');
    bridge.send('sample.close', null);
  }

  if (bridge) {
    ackBtn.addEventListener('click', function () {
      ackBtn.disabled = true;
      setStatus('', 'Waiting for the callout to confirm...');
      bridge.request('sample.ack', null, 3000).then(function (data) {
        ackBtn.textContent = 'Acknowledged';
        setStatus('ok', 'Acknowledged by the callout' +
          (data && data.at ? ' at ' + data.at : '') + '.');
      }, function () {
        ackBtn.disabled = false;
        setStatus('err', 'No confirmation from the callout. Try again.');
      });
    });
    closeBtn.addEventListener('click', closeCallout);
    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape') closeCallout();
    });
    loadScene();
  } else {
    document.getElementById('link').className = 'state off';
    document.getElementById('link').textContent = 'OFFLINE';
    sceneEl.setAttribute('aria-busy', 'false');
    sceneNote('The window.sui bridge is not available on this page.', true);
    ackBtn.disabled = true;
    closeBtn.disabled = true;
    setStatus('err', 'Open this page through SentinelUI or with the browser dev shim.');
  }
})();
</script>
</body></html>";
    }
}
