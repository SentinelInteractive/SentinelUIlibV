using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;

namespace SentinelUI
{
    /// <summary>
    /// SentinelUI public facade. Stateless by design: no cross-AppDomain singletons.
    /// Every AppDomain (Bootstrap plugin, LSPDFR plugins) that uses the API loads its own copy of
    /// this assembly and talks to the single native host through exported C functions.
    /// </summary>
    public static class Sui
    {
        private static readonly object Gate = new object();
        private static readonly List<SuiLayer> Layers = new List<SuiLayer>();
        private static bool _logReady;

        /// <summary>True when the native host is loaded and API-compatible.</summary>
        public static bool HostAvailable => NativeHost.IsAvailable;

        /// <summary>Host export-table API version (0 when unavailable).</summary>
        public static int HostApiVersion => NativeHost.ApiVersion;

        /// <summary>True when input capture is active (F10 or SetCapture) — the game
        /// should hand mouse/keyboard to the UI. Consumers (Bootstrap) use this to
        /// disable game controls while typed/clicked UI is focused.</summary>
        public static bool CaptureActive => NativeHost.CaptureActive;

        /// <summary>Raised for every event arriving from the UI (console/load/...).</summary>
        public static event Action<SuiMessage> MessageReceived;

        internal static void DispatchRawEvent(string line)
        {
            // line format: "<kind>:<body>" from the helper (e.g. console:NUI_...)
            int colon = line.IndexOf(':');
            string kind = colon < 0 ? line : line.Substring(0, colon);
            string body = colon < 0 ? string.Empty : line.Substring(colon + 1);

            SuiMessage msg = new SuiMessage { Type = kind, PayloadJson = body };
            lock (Gate)
            {
                foreach (var layer in Layers) layer.RaiseMessage(msg);
            }
            MessageReceived?.Invoke(msg);
        }

        /// <summary>Enables/disables input capture. While enabled the Bootstrap's pump
        /// fiber disables game controls so typing/clicks go to the UI.</summary>
        public static void SetCapture(bool on) => NativeHost.SetCapture(on);

        /// <summary>
        /// Initializes the bridge. Searches common locations for the host module:
        /// current directory (game root when launched by RPH) and the &lt;game&gt;\SentinelUI\ folder.
        /// Safe to call multiple times.
        /// </summary>
        public static bool Initialize(string explicitHostPath = null)
        {
            lock (Gate)
            {
                if (!_logReady)
                {
                    SuiLog.Initialize(Path.Combine(Environment.CurrentDirectory, "SentinelUI", "logs"));
                    _logReady = true;
                }

                if (HostAvailable) return true;

                var candidates = new List<string>();
                if (!string.IsNullOrWhiteSpace(explicitHostPath)) candidates.Add(explicitHostPath);
                candidates.Add(Path.Combine(Environment.CurrentDirectory, "SentinelUI", "SentinelUI.asi"));
                candidates.Add(Path.Combine(Environment.CurrentDirectory, "SentinelUI.asi")); // dev convenience

                foreach (var candidate in candidates)
                {
                    if (NativeHost.Initialize(candidate))
                    {
                        // Wire the native event sink once (game thread pump -> here).
                        NativeHost.RegisterEventSink(DispatchRawEvent);
                        return true;
                    }
                }

                SuiLog.Warn("SentinelUI host not found in any standard location. " +
                            "Install the SentinelUI runtime (SentinelUI\\SentinelUI.asi + Plugins\\SentinelUI.Bootstrap.dll).");
                return false;
            }
        }

        /// <summary>
        /// Creates a UI layer from a URL (sentinelui://plugin/page.html, or an https:// URL if the
        /// plugin explicitly opted into remote content) or from raw HTML markup.
        /// Returns null (with a logged warning) when the host is unavailable — plugins must handle
        /// null so the game keeps running without the SentinelUI runtime installed.
        /// </summary>
        public static SuiLayer CreateLayer(string urlOrHtml)
        {
            if (!HostAvailable) { SuiLog.Warn("CreateLayer ignored: host unavailable."); return null; }
            lock (Gate)
            {
                var layer = new SuiLayer(urlOrHtml);
                Layers.Add(layer);
                SuiLog.Info($"Layer created (managed id {layer.Id}): {Truncate(urlOrHtml, 80)}");
                // TODO(P2): NativeHost.SuiCreateLayer(...) and map native layer id.
                return layer;
            }
        }

        /// <summary>Drains host event queues. Call once per game frame from the game thread
        /// (Bootstrap does this automatically; LSPDFR plugins call it from their per-frame code,
        /// or use the pump fiber helper once P3 ships).</summary>
        public static void Pump()
        {
            if (HostAvailable) NativeHost.PumpEvents();
        }

        /// <summary>Graceful host shutdown. Only the Bootstrap plugin's unload path should call this.
        /// Never call from DllMain/PROCESS_DETACH contexts.</summary>
        public static void Shutdown()
        {
            NativeHost.Shutdown();
        }

        internal static void RemoveLayer(SuiLayer layer)
        {
            lock (Gate) { Layers.Remove(layer); }
        }

        private static string Truncate(string value, int max)
        {
            if (value == null) return string.Empty;
            return value.Length <= max ? value : value.Substring(0, max) + "...";
        }
    }

    /// <summary>One web UI surface on the host layer stack.</summary>
    public sealed class SuiLayer
    {
        private static long _nextId;

        /// <summary>Managed-side handle (native layer id mapping lands in P2).</summary>
        public long Id { get; }

        /// <summary>The URL or raw HTML this layer was created from.</summary>
        public string Source { get; }

        /// <summary>Raised for messages posted from the page (JSON envelope payload).</summary>
        public event Action<SuiMessage> Message;

        internal SuiLayer(string source)
        {
            Id = Interlocked.Increment(ref _nextId);
            Source = source;
        }

        public void SetVisible(bool visible)
        {
            NativeHost.CmdVisible((int)Id, visible);
        }

        public void SetFocus(bool keyboard, bool mouse)
        {
            NativeHost.CmdFocus((int)Id, keyboard, mouse);
        }

        public void SetClickThrough(bool enabled)
        {
            // P2: click-through per layer arrives with the layer stack.
        }

        public void ExecuteJs(string javascript)
        {
            NativeHost.ExecuteJs((int)Id, javascript);
        }

        public void Send(object payload)
        {
            // Interim: JS postMessage bridge (JSON envelope arrives with the scheme in P2).
            string json;
            try
            {
                json = new System.Web.Script.Serialization.JavaScriptSerializer().Serialize(payload);
            }
            catch
            {
                json = null;
            }
            if (json != null)
                NativeHost.ExecuteJs((int)Id, "window.postMessage(" + json + ", '*')");
        }

        public void Close()
        {
            // TODO(P2): NativeHost.SuiCloseLayer(...)
            Sui.RemoveLayer(this);
        }

        internal void RaiseMessage(SuiMessage message) => Message?.Invoke(message);
    }

    /// <summary>A message received from a page (or sent to it via <see cref="SuiLayer.Send"/>).</summary>
    public sealed class SuiMessage
    {
        public SuiLayer Layer { get; internal set; }
        public string Type { get; internal set; }
        public string PayloadJson { get; internal set; }
    }
}
