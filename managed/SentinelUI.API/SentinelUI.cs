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
            // Line format (v3): "<nativeLayerId>|<kind>:<body>" (e.g. "1|console:NUI_...").
            // Legacy two-part lines (no id prefix) broadcast to every layer.
            int bar = line.IndexOf('|');
            int nativeId = 0;
            string rest = line;
            if (bar > 0 && int.TryParse(line.Substring(0, bar), out nativeId))
                rest = line.Substring(bar + 1);

            int colon = rest.IndexOf(':');
            string kind = colon < 0 ? rest : rest.Substring(0, colon);
            string body = colon < 0 ? string.Empty : rest.Substring(colon + 1);

            SuiMessage msg = new SuiMessage { Type = kind, PayloadJson = body };
            lock (Gate)
            {
                if (nativeId > 0)
                {
                    // Route to the owning layer (fall back to broadcast when unknown).
                    SuiLayer target = null;
                    foreach (var l in Layers)
                        if (l.NativeId == nativeId) { target = l; break; }
                    if (target != null) target.RaiseMessage(msg);
                    else foreach (var layer in Layers) layer.RaiseMessage(msg);
                }
                else
                {
                    foreach (var layer in Layers) layer.RaiseMessage(msg);
                }
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
        /// Creates a UI layer from a URL — <c>sentinelui://&lt;plugin&gt;/&lt;path&gt;</c> for
        /// plugin content (root registered via <see cref="RegisterContentRoot"/>), or an
        /// https:// URL if the plugin explicitly opted into remote content. The boot layer
        /// uses <c>sentinelui://sui/p1_demo.html</c> (shipped demo content).
        /// Returns null (with a logged warning) when the host is unavailable, the layer
        /// budget is exhausted, or the URL was refused — plugins must handle null so the
        /// game keeps running without the SentinelUI runtime installed.
        /// Raw HTML (<c>CreateLayerFromHtml</c>) lands with the P3 managed API work.
        /// </summary>
        public static SuiLayer CreateLayer(string url)
        {
            if (!HostAvailable) { SuiLog.Warn("CreateLayer ignored: host unavailable."); return null; }
            if (string.IsNullOrWhiteSpace(url)) { SuiLog.Warn("CreateLayer ignored: empty URL."); return null; }
            if (url.TrimStart().StartsWith("<", StringComparison.Ordinal))
            {
                SuiLog.Warn("CreateLayer: raw HTML needs CreateLayerFromHtml (P3); refusing: " +
                            Truncate(url, 60));
                return null;
            }
            lock (Gate)
            {
                int nativeId = NativeHost.CreateLayer(url);
                if (nativeId <= 0)
                {
                    SuiLog.Warn($"CreateLayer refused by host (budget/bad URL): {Truncate(url, 80)}");
                    return null;
                }
                var layer = new SuiLayer(nativeId, url);
                Layers.Add(layer);
                SuiLog.Info($"Layer created (native id {layer.NativeId}): {Truncate(url, 80)}");
                return layer;
            }
        }

        /// <summary>
        /// Registers a plugin content root for the sentinelui:// scheme:
        /// <c>sentinelui://&lt;pluginName&gt;/&lt;file&gt;</c> serves files from <paramref name="rootPath"/>.
        /// Local-only: traversal outside the root is refused by the helper.
        /// </summary>
        public static bool RegisterContentRoot(string pluginName, string rootPath)
        {
            if (!HostAvailable)
            {
                SuiLog.Warn("RegisterContentRoot ignored: host unavailable.");
                return false;
            }
            bool ok = NativeHost.RegisterContentRoot(pluginName, rootPath);
            SuiLog.Info(ok
                ? $"Content root registered: sentinelui://{pluginName}/ -> {rootPath}"
                : $"Content root rejected: {pluginName} -> {rootPath}");
            return ok;
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
        private static long _nextManagedId;

        /// <summary>Managed-side handle (stable across sessions, for dictionary keys).</summary>
        public long Id { get; }

        /// <summary>Native layer id (what the host exports take). Native ids start at 1
        /// (the boot layer) and are never reused within a session.</summary>
        public int NativeId { get; }

        /// <summary>The URL this layer was created from.</summary>
        public string Source { get; }

        /// <summary>Raised for messages posted from this layer's page.</summary>
        public event Action<SuiMessage> Message;

        internal SuiLayer(int nativeId, string source)
        {
            Id = Interlocked.Increment(ref _nextManagedId);
            NativeId = nativeId;
            Source = source;
        }

        public void SetVisible(bool visible) => NativeHost.SetLayerVisible(NativeId, visible);

        public bool IsVisible => NativeHost.IsLayerVisible(NativeId);

        public void SetFocus(bool keyboard, bool mouse) =>
            NativeHost.CmdFocus(NativeId, keyboard, mouse);

        /// <summary>Stacking order. Higher z composites above lower z (ties: creation order).</summary>
        public void SetZ(int z) => NativeHost.SetLayerZ(NativeId, z);

        public int Z => NativeHost.GetLayerZ(NativeId);

        /// <summary>Click-through layers composite normally but never receive input;
        /// clicks "fall through" to the next interactive layer below.</summary>
        public void SetClickThrough(bool enabled) => NativeHost.SetLayerClickThrough(NativeId, enabled);

        public bool IsClickThrough => NativeHost.IsLayerClickThrough(NativeId);

        /// <summary>Re-navigates to <see cref="Source"/> — content is re-read from disk
        /// (hot reload; responses are served no-store).</summary>
        public void Reload() => NativeHost.ReloadLayer(NativeId);

        public void ExecuteJs(string javascript) => NativeHost.ExecuteJs(NativeId, javascript);

        public void Send(object payload)
        {
            // JS postMessage bridge: the page receives the JSON envelope on window.message.
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
                NativeHost.ExecuteJs(NativeId, "window.postMessage(" + json + ", '*')");
        }

        public void Close()
        {
            NativeHost.CloseLayer(NativeId);
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
