using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace SentinelUI
{
    /// <summary>
    /// SentinelUI public facade. Stateless by design: no cross-AppDomain singletons.
    /// Every AppDomain (Bootstrap plugin, LSPDFR plugins) that uses the API loads its own copy of
    /// this assembly and talks to the single native host through exported C functions.
    /// </summary>
    public static class Sui
    {
        private static readonly object _gate = new object();
        private static readonly List<SuiLayer> _layers = new List<SuiLayer>();
        private static readonly JavaScriptSerializer _json = new JavaScriptSerializer();
        private static bool _logReady;
        private static bool _attached;       // bridge attached in this AppDomain
        private static bool _isOwner;        // Bootstrap/probe: shutdown rights + legacy sink
        private static bool _attachFailed;   // suppress repeated "host missing" logs
        private static SuiDiagnosticsReport _lastDiagnostics;

        /// <summary>True when the native host is loaded and API-compatible.</summary>
        public static bool HostAvailable => NativeHost.IsAvailable;

        /// <summary>Host export-table API version (0 when unavailable).</summary>
        public static int HostApiVersion => NativeHost.ApiVersion;

        /// <summary>
        /// Cheap health probe (no I/O, no side effects): the last full report if
        /// <see cref="Diagnose"/> ever ran, otherwise host state only. For the full check list
        /// use <see cref="Diagnose"/> / the in-game <c>sui diag</c> console command.
        /// </summary>
        public static SuiStatus Status
        {
            get
            {
                lock (_gate) { if (_lastDiagnostics != null) return _lastDiagnostics.Status; }
                if (NativeHost.ShutdownRequested) return SuiStatus.Disabled;
                if (!NativeHost.IsAvailable) return SuiStatus.Unavailable;
                if (!NativeHost.SupportsLayerModel) return SuiStatus.Degraded;
                if (NativeHost.ApiVersion < SuiDiagnostics.PreferredHostApiVersion) return SuiStatus.Degraded; // v3: no per-domain queues
                return SuiStatus.Ready;
            }
        }

        /// <summary>Result of the last <see cref="Diagnose"/> call (null before the first run).</summary>
        public static SuiDiagnosticsReport LastDiagnostics
        {
            get { lock (_gate) { return _lastDiagnostics; } }
        }

        /// <summary>
        /// Runs the full capability probe: host attach, API version, export groups and
        /// install files. Passive: never attaches, never LoadLibrary's. Caches the report
        /// as <see cref="LastDiagnostics"/> and logs failures to
        /// SentinelUI\logs\sentinelui_api_*.log. Never throws.
        /// </summary>
        public static SuiDiagnosticsReport Diagnose()
        {
            SuiDiagnosticsReport report = SuiDiagnostics.Run();
            lock (_gate) { _lastDiagnostics = report; }
            try
            {
                SuiLog.Info("Diagnostics: " + report.Summary());
                foreach (var check in report.Checks)
                {
                    if (!check.Ok)
                        SuiLog.Warn($"  check {check.Name} ({(check.Required ? "required" : "warning")}): {check.Detail}");
                }
            }
            catch { /* diagnostics must never fail the caller */ }
            return report;
        }

        /// <summary>True when input capture is active (F10 or SetCapture): the game
        /// should hand mouse/keyboard to the UI. Consumers (Bootstrap) use this to
        /// disable game controls while typed/clicked UI is focused.</summary>
        public static bool CaptureActive => NativeHost.CaptureActive;

        /// <summary>Number of layers this AppDomain currently owns (visible or hidden).</summary>
        public static int LayerCount
        {
            get { lock (_gate) { return _layers.Count; } }
        }

        /// <summary>Raised for every event arriving from the UI (console/load/...).</summary>
        public static event Action<SuiMessage> MessageReceived;

        /// <summary>Raised for every page request (<c>sui.request</c> from any layer).
        /// Reply on the request object to resolve the page's promise.</summary>
        public static event Action<SuiRequest> RequestReceived;

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

            // Protocol envelope from window.sui (JSON after the "SUI:" prefix).
            if (string.Equals(kind, "console", StringComparison.Ordinal) &&
                body.StartsWith("SUI:", StringComparison.Ordinal))
            {
                if (DispatchSuiEnvelope(nativeId, body.Substring(4))) return;
                // Malformed envelope: fall through and treat it as a plain console message.
            }

            RaiseRawMessage(nativeId, kind, body);
        }

        private static void RaiseRawMessage(int nativeId, string type, string payloadJson)
        {
            SuiMessage msg = new SuiMessage { Type = type, PayloadJson = payloadJson };
            lock (_gate) { RouteMessageLocked(nativeId, msg); }
            MessageReceived?.Invoke(msg);
        }

        private static void RouteMessageLocked(int nativeId, SuiMessage msg)
        {
            if (nativeId > 0)
            {
                // Route to the owning layer. Unknown ids (e.g. layers owned by another
                // AppDomain) must not broadcast per-layer: only the global event fires.
                FindLayerLocked(nativeId)?.RaiseMessage(msg);
            }
            else
            {
                foreach (var layer in _layers) layer.RaiseMessage(msg);
            }
        }

        private static SuiLayer FindLayerLocked(int nativeId)
        {
            foreach (var l in _layers)
                if (l.NativeId == nativeId) return l;
            return null;
        }

        /// <summary>Handles a window.sui envelope; returns false when the JSON is unusable
        /// (the caller then treats the line as a plain console message).</summary>
        private static bool DispatchSuiEnvelope(int nativeId, string json)
        {
            Dictionary<string, object> env;
            try { env = _json.Deserialize<Dictionary<string, object>>(json); }
            catch { return false; }
            if (env == null) return false;
            if (!env.TryGetValue("type", out var typeObj) || !(typeObj is string type) || type.Length == 0)
                return false;

            string kind = env.TryGetValue("kind", out var k) ? k as string : "evt";
            string payloadJson = env.TryGetValue("payload", out var p) ? Serialize(p) : "null";

            if (string.Equals(kind, "req", StringComparison.Ordinal))
            {
                long id = 0;
                if (env.TryGetValue("id", out var idObj) && idObj != null)
                    long.TryParse(Convert.ToString(idObj, CultureInfo.InvariantCulture), out id);

                var request = new SuiRequest { Type = type, PayloadJson = payloadJson, Id = id };
                SuiLayer target;
                lock (_gate)
                {
                    target = nativeId > 0 ? FindLayerLocked(nativeId) : null;
                    if (target != null) request.Layer = target;
                }
                target?.RaiseRequest(request);
                RequestReceived?.Invoke(request);
                return true;
            }

            RaiseRawMessage(nativeId, type, payloadJson);
            return true;
        }

        internal static string Serialize(object value)
        {
            try { return _json.Serialize(value ?? null); }
            catch { return "null"; }
        }

        internal static T Deserialize<T>(string json)
        {
            try { return _json.Deserialize<T>(json ?? "null"); }
            catch { return default(T); }
        }

        /// <summary>Enables/disables input capture. While enabled the Bootstrap's pump
        /// fiber disables game controls so typing/clicks go to the UI.</summary>
        public static void SetCapture(bool on)
        {
            if (!EnsureAttached()) return;
            NativeHost.SetCapture(on);
        }

        /// <summary>
        /// Initializes the bridge as the session owner (Bootstrap / offline probes).
        /// In-game (<c>loadIfNeeded:false</c>) this only attaches to the ASI-loader-hosted
        /// module: ScriptHookV loads the root SentinelUI.asi at process start, before RPH
        /// arms its Present-hook guard (see NativeHost for the 2026-09-11 crash constraint).
        /// The default (<c>true</c>) additionally LoadLibrary's the host from
        /// <paramref name="explicitHostPath"/> or the standard locations, for offline probes/tests.
        /// Plugin AppDomains do not need to call this: the first API call attaches lazily as a
        /// non-owner client (per-domain event queue, no shutdown rights). Safe to call repeatedly.
        /// </summary>
        public static bool Initialize(string explicitHostPath = null, bool loadIfNeeded = true)
        {
            lock (_gate)
            {
                EnsureLog();
                if (_attached) return HostAvailable;

                if (!loadIfNeeded)
                {
                    // Attach-only: the path is irrelevant (GetModuleHandle by module name).
                    return AttachLocked(owner: true);
                }

                var candidates = new List<string>();
                if (!string.IsNullOrWhiteSpace(explicitHostPath)) candidates.Add(explicitHostPath);
                candidates.Add(Path.Combine(Environment.CurrentDirectory, "SentinelUI", "SentinelUI.asi"));
                // Canonical in-game location: the ASI is SHV-loaded from the game root
                // (must hook Present before RPH; see Bootstrap.ResolveHostPath).
                candidates.Add(Path.Combine(Environment.CurrentDirectory, "SentinelUI.asi")); // dev convenience

                foreach (var candidate in candidates)
                {
                    if (NativeHost.Initialize(candidate))
                    {
                        CompleteAttachLocked(owner: true);
                        return true;
                    }
                }

                SuiLog.Warn("SentinelUI host not found in any standard location. Install the " +
                            "SentinelUI runtime (root SentinelUI.asi loaded by ScriptHookV + " +
                            "Plugins\\SentinelUI.Bootstrap.dll).");
                return false;
            }
        }

        private static void EnsureLog()
        {
            if (_logReady) return;
            SuiLog.Initialize(Path.Combine(Environment.CurrentDirectory, "SentinelUI", "logs"));
            _logReady = true;
        }

        /// <summary>Lazy attach for client AppDomains (LSPDFR plugins etc.). Never
        /// LoadLibrary's the host in-game; returns false when it isn't loaded.</summary>
        private static bool EnsureAttached()
        {
            if (_attached && HostAvailable) return true;
            lock (_gate)
            {
                if (_attached && HostAvailable) return true;
                return AttachLocked(owner: false);
            }
        }

        /// <summary>Attaches this AppDomain as a non-owner event client. The first API use
        /// does this automatically; call it explicitly to fail fast. Plugin AppDomains use
        /// this: only Bootstrap owns the host lifecycle (see <see cref="Initialize"/>).</summary>
        public static bool AttachClient() => EnsureAttached();

        private static bool AttachLocked(bool owner)
        {
            EnsureLog();
            if (NativeHost.ShutdownRequested) return false;   // host already shut down this session
            if (!NativeHost.IsAvailable && !NativeHost.Initialize(null, allowLoad: false))
            {
                if (!_attachFailed)
                {
                    _attachFailed = true;
                    SuiLog.Warn("SentinelUI host is not loaded in this process (ASI loader missing?) — " +
                                "UI features are disabled for this AppDomain.");
                }
                return false;
            }
            CompleteAttachLocked(owner);
            return true;
        }

        private static void CompleteAttachLocked(bool owner)
        {
            if (_attached) return;
            _attached = true;
            _isOwner = owner;

            if (NativeHost.SupportsClientEvents)
            {
                // Per-domain queue: events are delivered by Pump() on this domain's thread.
                NativeHost.RegisterEventClient(DispatchRawEvent);
            }
            else if (owner)
            {
                // Host v3: single legacy sink. Only the owner may install it.
                NativeHost.RegisterEventSink(DispatchRawEvent);
            }
            else
            {
                SuiLog.Warn("Host is v3 (no per-domain queues): client AppDomains receive no events. " +
                            "Update the SentinelUI runtime for cross-domain event delivery.");
            }

            if (owner)
            {
                AppDomain.CurrentDomain.DomainUnload += OnOwnerDomainUnload;
                AppDomain.CurrentDomain.ProcessExit += OnOwnerProcessExit;
            }
            else
            {
                AppDomain.CurrentDomain.DomainUnload += OnClientDomainUnload;
            }
        }

        /// <summary>
        /// Creates a UI layer from a URL: <c>sentinelui://&lt;plugin&gt;/&lt;path&gt;</c> for
        /// plugin content (root registered via <see cref="RegisterContentRoot"/>), or an
        /// https:// URL if the plugin explicitly opted into remote content. The boot layer
        /// uses <c>sentinelui://sui/p1_demo.html</c> (shipped demo content).
        /// Returns null (with a logged warning) when the host is unavailable, the layer
        /// budget is exhausted, or the URL was refused: plugins must handle null so the
        /// game keeps running without the SentinelUI runtime installed.
        /// </summary>
        public static SuiLayer CreateLayer(string url)
        {
            if (!EnsureAttached()) return null;
            if (string.IsNullOrWhiteSpace(url)) { SuiLog.Warn("CreateLayer ignored: empty URL."); return null; }
            if (url.TrimStart().StartsWith("<", StringComparison.Ordinal))
            {
                SuiLog.Warn("CreateLayer: raw HTML needs CreateLayerFromHtml; refusing: " +
                            Truncate(url, 60));
                return null;
            }
            lock (_gate)
            {
                int nativeId = NativeHost.CreateLayer(url);
                if (nativeId <= 0)
                {
                    SuiLog.Warn($"CreateLayer refused by host (budget/bad URL): {Truncate(url, 80)}");
                    return null;
                }
                var layer = new SuiLayer(nativeId, url);
                _layers.Add(layer);
                SuiLog.Info($"Layer created (native id {layer.NativeId}): {Truncate(url, 80)}");
                return layer;
            }
        }

        /// <summary>
        /// Creates a layer from an in-memory HTML document (no content root needed).
        /// The page becomes a <c>data:text/html</c> URL. When <paramref name="baseUrl"/> is
        /// given, it is injected as &lt;base href&gt; so relative script/CSS URLs resolve
        /// against it (sentinelui:// responses are CORS-open for this case). Self-contained
        /// pages need no base URL. Keep documents small: they travel through the IPC pipe
        /// and live in the page URL.
        /// </summary>
        public static SuiLayer CreateLayerFromHtml(string html, string baseUrl = null)
        {
            if (string.IsNullOrWhiteSpace(html))
            {
                SuiLog.Warn("CreateLayerFromHtml ignored: empty HTML.");
                return null;
            }

            string page = html;
            if (!string.IsNullOrWhiteSpace(baseUrl))
            {
                string tag = "<base href=\"" + baseUrl.TrimEnd('/') + "/\">";
                int head = page.IndexOf("<head", StringComparison.OrdinalIgnoreCase);
                if (head >= 0)
                {
                    int gt = page.IndexOf('>', head);
                    if (gt > 0) page = page.Insert(gt + 1, tag);
                }
                else
                {
                    page = "<head>" + tag + "</head>" + page;
                }
            }

            string dataUrl = "data:text/html;charset=utf-8;base64," +
                             Convert.ToBase64String(Encoding.UTF8.GetBytes(page));
            SuiLayer layer = CreateLayer(dataUrl);
            if (layer != null) SuiLog.Info($"Layer created from inline HTML (native id {layer.NativeId}).");
            return layer;
        }

        /// <summary>
        /// Registers a plugin content root for the sentinelui:// scheme:
        /// <c>sentinelui://&lt;pluginName&gt;/&lt;file&gt;</c> serves files from <paramref name="rootPath"/>.
        /// Local-only: traversal outside the root is refused by the helper.
        /// </summary>
        public static bool RegisterContentRoot(string pluginName, string rootPath)
        {
            if (!EnsureAttached()) return false;
            bool ok = NativeHost.RegisterContentRoot(pluginName, rootPath);
            SuiLog.Info(ok
                ? $"Content root registered: sentinelui://{pluginName}/ -> {rootPath}"
                : $"Content root rejected: {pluginName} -> {rootPath}");
            return ok;
        }

        /// <summary>Drains queued UI events for this AppDomain. Call once per game frame
        /// from the game thread (Bootstrap does it automatically). Every AppDomain has its
        /// own queue, so plugin handlers may safely call game natives: delivery happens on
        /// the caller's thread.</summary>
        public static void Pump()
        {
            if (!EnsureAttached()) return;
            if (NativeHost.HasClientToken) NativeHost.PumpClientEvents();
            else if (_isOwner) NativeHost.PumpEvents();      // v3 host legacy sink
            // Non-owner on a v3 host: no event path (warned at attach).
        }

        /// <summary>Graceful host shutdown. Only the owner (Bootstrap unload path) may call
        /// this; in a client AppDomain it just detaches that domain's event queue. Never call
        /// from DllMain/PROCESS_DETACH contexts.</summary>
        public static void Shutdown()
        {
            if (!_isOwner)
            {
                lock (_gate)
                {
                    NativeHost.UnregisterEventClient();
                    _attached = false;
                }
                return;
            }
            NativeHost.Shutdown();
        }

        internal static void RemoveLayer(SuiLayer layer)
        {
            lock (_gate) { _layers.Remove(layer); }
        }

        private static void OnOwnerDomainUnload(object sender, EventArgs e) => Shutdown();
        private static void OnOwnerProcessExit(object sender, EventArgs e) => Shutdown();
        /// <summary>Client AppDomain teardown: close the layers it owns and release its
        /// event queue. Never touches the host lifecycle (Bootstrap owns that).</summary>
        private static void OnClientDomainUnload(object sender, EventArgs e)
        {
            try
            {
                lock (_gate)
                {
                    foreach (var layer in _layers.ToArray())
                    {
                        try { layer.Close(); } catch { /* domain is going away */ }
                    }
                    NativeHost.UnregisterEventClient();
                    _attached = false;
                }
            }
            catch { /* nothing to do at unload */ }
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

        /// <summary>Raised for messages posted from this layer's page (<c>sui.send</c> or
        /// legacy console lines).</summary>
        public event Action<SuiMessage> Message;

        /// <summary>Raised for requests from this layer's page (<c>sui.request</c>).
        /// Call <see cref="SuiRequest.Reply"/> to resolve the page's promise.</summary>
        public event Action<SuiRequest> Request;

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

        /// <summary>Shows the layer, focuses it and enables input capture: the usual
        /// "open an interactive UI" sequence in one call. Pair with <see cref="HideCaptured"/>
        /// (or release capture yourself when the UI closes, e.g. on a page "close" event).</summary>
        public void ShowCaptured()
        {
            SetVisible(true);
            SetFocus(true, true);
            Sui.SetCapture(true);
        }

        /// <summary>Releases input capture and hides the layer.</summary>
        public void HideCaptured()
        {
            Sui.SetCapture(false);
            SetVisible(false);
        }

        /// <summary>Stacking order. Higher z composites above lower z (ties: creation order).</summary>
        public void SetZ(int z) => NativeHost.SetLayerZ(NativeId, z);

        public int Z => NativeHost.GetLayerZ(NativeId);

        /// <summary>Click-through layers composite normally but never receive input;
        /// clicks "fall through" to the next interactive layer below.</summary>
        public void SetClickThrough(bool enabled) => NativeHost.SetLayerClickThrough(NativeId, enabled);

        public bool IsClickThrough => NativeHost.IsLayerClickThrough(NativeId);

        /// <summary>Re-navigates to <see cref="Source"/>: content is re-read from disk
        /// (hot reload; responses are served no-store).</summary>
        public void Reload() => NativeHost.ReloadLayer(NativeId);

        public void ExecuteJs(string javascript) => NativeHost.ExecuteJs(NativeId, javascript);

        /// <summary>Posts a protocol event to the page: <c>sui.on(type, handler)</c> receives
        /// it. Default type is "message".</summary>
        public void Send(object payload) => Send("message", payload);

        /// <summary>Posts a typed protocol event to the page.</summary>
        public void Send(string type, object payload)
        {
            var env = new Dictionary<string, object>
            {
                { "v", 1 },
                { "kind", "evt" },
                { "type", type ?? "message" },
                { "payload", payload },
            };
            string json = Sui.Serialize(env);
            NativeHost.ExecuteJs(NativeId, "window.sui&&window.sui._deliver(" + json + ")");
        }

        public void Close()
        {
            NativeHost.CloseLayer(NativeId);
            Sui.RemoveLayer(this);
        }

        internal void RaiseMessage(SuiMessage message)
        {
            message.Layer = this;
            Message?.Invoke(message);
        }

        internal void RaiseRequest(SuiRequest request)
        {
            request.Layer = this;
            Request?.Invoke(request);
        }

        internal void ReplyRequest(SuiRequest request, object payload, bool ok)
        {
            var env = new Dictionary<string, object>
            {
                { "v", 1 },
                { "kind", "res" },
                { "type", request.Type },
                { "id", request.Id },
                { "ok", ok },
                { "payload", payload },
            };
            string json = Sui.Serialize(env);
            NativeHost.ExecuteJs(NativeId, "window.sui&&window.sui._deliver(" + json + ")");
        }
    }

    /// <summary>A request from a page (<c>sui.request(type, payload)</c>). Reply on the game
    /// thread to resolve the page's promise; the handler runs from <c>Sui.Pump()</c>, so game
    /// natives are safe to call here.</summary>
    public sealed class SuiRequest
    {
        public SuiLayer Layer { get; internal set; }
        public string Type { get; internal set; }
        public string PayloadJson { get; internal set; }
        public long Id { get; internal set; }

        /// <summary>Deserializes <see cref="PayloadJson"/> into <typeparamref name="T"/>.</summary>
        public T Payload<T>() => Sui.Deserialize<T>(PayloadJson);

        /// <summary>Resolves the page's promise with <paramref name="payload"/>.</summary>
        public void Reply(object payload)
        {
            if (Layer == null)
            {
                SuiLog.Warn($"SuiRequest '{Type}' has no owning layer; reply dropped.");
                return;
            }
            Layer.ReplyRequest(this, payload, true);
        }

        /// <summary>Rejects the page's promise (the page's <c>catch</c> receives the message).</summary>
        public void ReplyError(string message)
        {
            if (Layer == null)
            {
                SuiLog.Warn($"SuiRequest '{Type}' has no owning layer; error reply dropped.");
                return;
            }
            Layer.ReplyRequest(this, message ?? "error", false);
        }
    }

    /// <summary>A message received from a page (or sent to it via <see cref="SuiLayer.Send(object)"/>).</summary>
    public sealed class SuiMessage
    {
        public SuiLayer Layer { get; internal set; }
        public string Type { get; internal set; }
        public string PayloadJson { get; internal set; }

        /// <summary>Deserializes <see cref="PayloadJson"/> into <typeparamref name="T"/>.</summary>
        public T Payload<T>() => Sui.Deserialize<T>(PayloadJson);
    }
}
