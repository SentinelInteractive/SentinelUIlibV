using System;
using System.Windows.Forms;

using LSPD_First_Response.Mod.Callouts;
using Rage;
using SentinelUI;

namespace SuiSample
{
    /// <summary>
    /// Quick LSPDFR callout that opens a SentinelUI panel from the LSPDFR AppDomain.
    ///
    /// This is the cross-domain client path: the callout does NOT own the host
    /// (Bootstrap does). It attaches as a client, creates an inline-HTML layer,
    /// pumps its own per-domain event queue in Process(), and closes the layer when
    /// the callout ends.
    ///
    /// Registered by <see cref="Main"/> when going on duty; trigger with the RPH
    /// console (F4): ForceCallout SuiSample (probability is High, it can also spawn
    /// on its own). End with the End key or the panel button.
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
                    Game.LogTrivial("[SuiSample] SentinelUI host not loaded (ScriptHookV/ASI missing) — ending.");
                    End();
                    return true;
                }

                _ui = Sui.CreateLayerFromHtml(SampleHtml);
                if (_ui == null)
                {
                    Game.LogTrivial("[SuiSample] CreateLayerFromHtml refused — ending.");
                    End();
                    return true;
                }

                _ui.Message += OnUiMessage;
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
            // Called from Sui.Pump() in Process() — game thread, natives are legal here.
            if (message == null || message.Type != "console" || message.PayloadJson == null) return;
            Game.LogTrivial("[SuiSample] page event: " + message.PayloadJson);

            if (message.PayloadJson.StartsWith("NUI_SAMPLE:ack", StringComparison.Ordinal))
            {
                if (_acknowledged) return;
                _acknowledged = true;
                Game.DisplayNotification("~g~Acknowledged~w~ — SentinelUI event reached the callout.");
            }
            else if (message.PayloadJson.StartsWith("NUI_SAMPLE:close", StringComparison.Ordinal))
            {
                End();
            }
        }

        private const string SampleHtml = @"<!DOCTYPE html>
<html><head><meta charset=""utf-8""><style>
  html,body{margin:0;background:transparent;font-family:'Segoe UI',sans-serif;color:#e8eef7;}
  .panel{position:absolute;left:48px;top:96px;width:380px;background:rgba(9,13,20,.93);
         border:1px solid #2d4059;border-radius:8px;box-shadow:0 12px 40px rgba(0,0,0,.55);
         padding:16px 18px;}
  h1{font-size:14px;letter-spacing:.14em;text-transform:uppercase;color:#6ec1ff;margin:0 0 10px;}
  .meta{font-size:12px;color:#8fa3bd;margin:0 0 8px;}
  p{margin:6px 0;font-size:13px;color:#c6d2e2;}
  button{margin:12px 8px 0 0;padding:8px 14px;border:0;border-radius:5px;background:#1f6feb;
         color:#fff;font-size:13px;cursor:pointer;}
  button.ghost{background:#243447;color:#cfe1ff;}
</style></head><body>
  <div class=""panel"">
    <h1>SentinelUI &mdash; LSPDFR Sample</h1>
    <div class=""meta"">inline HTML layer from an LSPDFR callout AppDomain</div>
    <p id=""status"">Press F10 (capture), then click Acknowledge.</p>
    <button id=""ack"">Acknowledge</button>
    <button class=""ghost"" id=""close"">Close callout</button>
  </div>
  <script>
    document.getElementById('ack').addEventListener('click', function () {
      console.log('NUI_SAMPLE:ack');
      document.getElementById('status').textContent = 'Acknowledged at ' + new Date().toLocaleTimeString();
    });
    document.getElementById('close').addEventListener('click', function () {
      console.log('NUI_SAMPLE:close');
    });
  </script>
</body></html>";
    }
}
