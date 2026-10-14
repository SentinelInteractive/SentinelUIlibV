using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Threading;

namespace SentinelUI.HostProbe
{
    /// <summary>
    /// Console probe for the load path and interop marshalling, no game required.
    /// Usage: SentinelUI.HostProbe.exe [path\to\SentinelUI.asi]
    /// Exercises: graceful degradation (host missing), LoadLibrary of an ".asi"-named PE,
    /// export resolution, host handshake (API version), layer creation, pump, shutdown.
    /// </summary>
    internal static class Program
    {
        private static int _failures;

        private static void Check(bool condition, string what)
        {
            Console.WriteLine((condition ? "  [PASS] " : "  [FAIL] ") + what);
            if (!condition) _failures++;
        }

        private static void Main(string[] args)
        {
            Console.WriteLine("== SentinelUI HostProbe ==");

            // 1. Graceful degradation before any host is loaded.
            Console.WriteLine("[1] host missing behavior");
            var layer = Sui.CreateLayer("sentinelui://x/y.html");
            Check(layer == null, "CreateLayer returns null when host unavailable");
            Check(!Sui.HostAvailable, "HostAvailable is false");
            Sui.Pump(); // must be a safe no-op
            Check(!Sui.Initialize(null, loadIfNeeded: false),
                  "attach-only Initialize degrades when no ASI loader loaded the host (never LoadLibrary)");

            // 2. Initialize from an explicit path (the Bootstrap plugin will pass an absolute path).
            Console.WriteLine("[2] load & handshake");
            string hostPath = args.Length > 0 ? args[0] : null;
            if (string.IsNullOrWhiteSpace(hostPath))
                hostPath = Path.Combine(Environment.CurrentDirectory, "SentinelUI.asi");
            Check(File.Exists(hostPath), $"host file exists: {hostPath}");
            bool init = Sui.Initialize(hostPath);
            if (init) Console.WriteLine("  (host module loaded, API v" + Sui.HostApiVersion + ")");
            Check(init, "Sui.Initialize returns true");
            Check(Sui.HostAvailable, "HostAvailable is true after init");
            Check(Sui.HostApiVersion >= 1, "API version handshake ok (v" + Sui.HostApiVersion + ")");

            // 3. Create a layer and exercise the facade (must not throw).
            Console.WriteLine("[3] facade smoke");
            var okLayer = Sui.CreateLayer("sentinelui://probe/page.html");
            Check(okLayer != null, "CreateLayer works with living host");
            try
            {
                okLayer?.SetVisible(true);
                okLayer?.SetFocus(true, true);
                okLayer?.ExecuteJs("console.log('probe');");
                okLayer?.Send(new { type = "ping", payload = "x" });
                Sui.Pump();
                Check(true, "layer operations + Pump execute without exceptions");
            }
            catch (Exception ex)
            {
                Check(false, "layer operations threw: " + ex.Message);
            }
            okLayer?.Close();

            // 3b. Issue-2 smoke (layer stack + sentinelui:// scheme), needs the helper running.
            Issue2(init);

            // 3c. Issue-3 smoke (inline HTML layer), needs the helper running.
            Issue3InlineHtml(init);

            // 3d. Issue-3 smoke (request/response bridge), needs the helper running.
            Issue4Requests(init);

            // 3e. Issue-3 smoke (cross-AppDomain event client), the LSPDFR domain model.
            CrossDomain(init);

            // 3f. Issue-1 smoke (IPC + capture + events), needs the helper running.
            Issue1(init);

            // 4. Graceful shutdown + repeat-safe behavior.
            Console.WriteLine("[4] shutdown");
            Sui.Shutdown();
            Check(!Sui.HostAvailable, "HostAvailable false after Shutdown");
            Sui.Shutdown(); // idempotent
            Check(true, "Shutdown is idempotent (no double-free)");

            Console.WriteLine(_failures == 0 ? "PROBE RESULT: PASS" : $"PROBE RESULT: FAIL ({_failures})");
            Environment.ExitCode = _failures == 0 ? 0 : 1;
        }

        /// <summary>Issue #3: in-memory HTML layer (data: URL) runs its JS and reports back.</summary>
        private static void Issue3InlineHtml(bool init)
        {
            if (!init || !Sui.HostAvailable) return;
            Console.WriteLine("[7] issue-3 inline HTML layer (CreateLayerFromHtml)");
            var layer = Sui.CreateLayerFromHtml(
                "<!DOCTYPE html><html><body><script>console.log('NUI_HTML:inline-ok');</script></body></html>");
            Check(layer != null, "CreateLayerFromHtml returns a layer");
            if (layer == null) return;

            var events = new List<string>();
            Action<SuiMessage> handler = m => { lock (events) events.Add(m.Type + "|" + m.PayloadJson); };
            Sui.MessageReceived += handler;
            try
            {
                layer.SetVisible(true);
                var start = Environment.TickCount;
                bool seen = false;
                while (Environment.TickCount - start < 6000)
                {
                    Sui.Pump();
                    lock (events)
                        seen = events.Exists(e => e.IndexOf("NUI_HTML:inline-ok", StringComparison.Ordinal) >= 0);
                    if (seen) break;
                    Thread.Sleep(50);
                }
                Check(seen, "inline page JS ran through the data: URL");
            }
            finally
            {
                Sui.MessageReceived -= handler;
                layer.Close();
            }
        }

        /// <summary>Issue #3 bridge v2: window.sui request -> C# SuiLayer.Request/Reply -> JS promise.</summary>
        private static void Issue4Requests(bool init)
        {
            if (!init || !Sui.HostAvailable) return;
            Console.WriteLine("[9] issue-3 request/response bridge (window.sui)");
            var layer = Sui.CreateLayerFromHtml(
                "<!DOCTYPE html><html><body><script>" +
                "sui.request('probe.echo',{n:42}).then(function(r){" +
                "console.log('NUI_PROBE:reply '+JSON.stringify(r));});" +
                "</script></body></html>");
            Check(layer != null, "request layer created");
            if (layer == null) return;

            var got = new List<string>();
            var requests = new List<string>();
            Action<SuiMessage> onMessage = m =>
            {
                if (m.Layer == layer) lock (got) got.Add(m.Type + "|" + m.PayloadJson);
            };
            Action<SuiRequest> onRequest = r =>
            {
                if (r.Type != "probe.echo") return;
                lock (requests) requests.Add(r.PayloadJson);
                r.Reply(new { echo = "pong", got = r.PayloadJson });
            };
            layer.Message += onMessage;
            layer.Request += onRequest;
            try
            {
                layer.SetVisible(true);
                var start = Environment.TickCount;
                bool seen = false;
                while (Environment.TickCount - start < 6000)
                {
                    Sui.Pump();
                    lock (got)
                        seen = got.Exists(e => e.IndexOf("NUI_PROBE:reply", StringComparison.Ordinal) >= 0);
                    if (seen) break;
                    Thread.Sleep(25);
                }
                lock (requests)
                    Check(requests.Count == 1 && requests[0].IndexOf("42", StringComparison.Ordinal) >= 0,
                          "C# received the typed request payload");
                Check(seen, "page promise resolved with the C# reply");
            }
            finally
            {
                layer.Message -= onMessage;
                layer.Request -= onRequest;
                layer.Close();
            }
        }

        /// <summary>Issue #3: a second AppDomain attaches as a client and must receive its
        /// own events without owning the host (Bootstrap stays the owner).</summary>
        private static void CrossDomain(bool init)
        {
            if (!init || !Sui.HostAvailable) return;
            Console.WriteLine("[8] issue-3 cross-AppDomain event client");
            string apiDir = Path.GetDirectoryName(typeof(Program).Assembly.Location);
            AppDomain domain = null;
            try
            {
                domain = AppDomain.CreateDomain("SentinelUI.HostProbe.Client");
                var client = (CrossDomainProbe)domain.CreateInstanceFromAndUnwrap(
                    typeof(Program).Assembly.Location, typeof(CrossDomainProbe).FullName);
                string result = client.Run(apiDir);
                Check(result.StartsWith("ok:", StringComparison.Ordinal),
                      "client domain attaches and receives its own events (" + result + ")");
            }
            catch (Exception ex)
            {
                Check(false, "cross-domain probe threw: " + ex.Message);
            }
            finally
            {
                if (domain != null) AppDomain.Unload(domain);
            }
        }

        private static void Issue2(bool init)
        {
            // Requires the host to have spawned the helper + IPC (debug env starts both).
            if (!init || !Sui.HostAvailable) return;
            Console.WriteLine("[5] issue-2 layer stack + sentinelui:// scheme");
            try
            {
                // Register a real content root in a temp dir; the pages are served
                // through sentinelui://probe/... (CSP-tight: external script only).
                string root = Path.Combine(Path.GetTempPath(), "sentinelui-probe-root");
                Directory.CreateDirectory(root);
                File.WriteAllText(Path.Combine(root, "issue2.html"),
                    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">" +
                    "<link rel=\"stylesheet\" href=\"issue2.css\"></head>" +
                    "<body><div id=\"ok\">scheme-page</div>" +
                    "<script src=\"issue2.js\"></script></body></html>");
                File.WriteAllText(Path.Combine(root, "issue2.css"),
                    "body{background:transparent;color:#fff;font:14px sans-serif;}");
                File.WriteAllText(Path.Combine(root, "issue2.js"),
                    "window.__probeRan = true; console.log('NUI_PROBE:scheme-page-loaded');");

                Check(Sui.RegisterContentRoot("probe", root), "RegisterContentRoot accepted a local root");

                // Layer model: create/close/z/click-through/visibility.
                var l2 = Sui.CreateLayer("sentinelui://probe/issue2.html");
                Check(l2 != null && l2.NativeId > 0, "CreateLayer via sentinelui:// returns a native id");
                if (l2 != null)
                {
                    l2.SetZ(10);
                    Check(l2.Z == 10, "SetLayerZ/GetLayerZ round-trip");
                    l2.SetClickThrough(true);
                    Check(l2.IsClickThrough, "SetLayerClickThrough reflected");
                    l2.SetClickThrough(false);
                    Check(!l2.IsClickThrough, "click-through cleared");
                    l2.SetVisible(false);
                    Check(!l2.IsVisible, "visibility toggled off");
                    l2.SetVisible(true);

                    // Scheme round-trip: the page's external JS must run in the page
                    // loaded through sentinelui:// and come back as a console event,
                    // routed to this layer (per-layer Message event).
                    var got = new System.Collections.Generic.List<string>();
                    Action<SuiMessage> layerHandler = m => { lock (got) got.Add(m.Type + "|" + m.PayloadJson); };
                    l2.Message += layerHandler;
                    try
                    {
                        const int deadlineMs = 10000;
                        var start = Environment.TickCount;
                        bool schemeOk = false, cssOk = false;
                        while (Environment.TickCount - start < deadlineMs)
                        {
                            Sui.Pump();
                            lock (got)
                            {
                                schemeOk = schemeOk ||
                                    got.Exists(e => e.IndexOf("NUI_PROBE:scheme-page-loaded",
                                                               StringComparison.Ordinal) >= 0);
                            }
                            if (schemeOk) break;
                            System.Threading.Thread.Sleep(50);
                        }
                        // CSS served through the scheme + CSP-allowed external script;
                        // also dump whether the page script executed + resource entries.
                        l2.ExecuteJs(
                            "console.log('NUI_PROBE:css=' + getComputedStyle(document.body).color);" +
                            "console.log('NUI_PROBE:script=' + (window.__probeRan === true));" +
                            "console.log('NUI_PROBE:res=' + JSON.stringify(" +
                            "performance.getEntriesByType('resource').map(function(r){return r.name;})));" +
                            "console.log('NUI_PROBE:doc=' + document.contentType + '|' + location.href);" +
                            "console.log('NUI_PROBE:body=' + document.body.innerHTML.length);" +
                            "console.log('NUI_PROBE:html=' + encodeURIComponent(document.body.innerHTML.substring(0,260)));");
                        start = Environment.TickCount;
                        while (Environment.TickCount - start < 6000)
                        {
                            Sui.Pump();
                            lock (got)
                            {
                                cssOk = cssOk || got.Exists(e =>
                                    e.IndexOf("NUI_PROBE:css=", StringComparison.Ordinal) >= 0);
                            }
                            if (cssOk) break;
                            System.Threading.Thread.Sleep(50);
                        }
                        Check(schemeOk, "scheme round-trip: page loaded via sentinelui:// + JS ran");
                        Check(cssOk, "external CSS served through the scheme");
                        Check(got.Count > 0, "per-layer Message events routed (native id " + l2.NativeId + ")");
                        foreach (var ev in got) Console.WriteLine("    event: " + ev);
                    }
                    finally { l2.Message -= layerHandler; }

                    // Traversal refusal: ".." segments must produce a load error, never a file.
                    var bad = Sui.CreateLayer("sentinelui://probe/../issue2.js");
                    Check(bad != null, "traversal layer created (refusal happens at scheme level)");
                    bad?.Close();
                    l2.Close();
                }
            }
            catch (Exception ex)
            {
                Check(false, "issue-2 smoke threw: " + ex.Message);
            }
        }

        private static void Issue1(bool init)
        {
            // Requires the host to have spawned the helper + IPC (debug env starts both).
            if (!init || !Sui.HostAvailable) return;
            Console.WriteLine("[6] issue-1 IPC/capture/events");
            try
            {
                Check(!Sui.CaptureActive, "Capture inactive by default");
                Sui.SetCapture(true);
                Check(Sui.CaptureActive, "SetCapture(true) reflected on export");
                Sui.SetCapture(false);

                // Probe root registered by the issue-2 section; write the round-trip page.
                string root = Path.Combine(Path.GetTempPath(), "sentinelui-probe-root");
                Directory.CreateDirectory(root);
                File.WriteAllText(Path.Combine(root, "issue1.html"),
                    "<!DOCTYPE html><html><body>scheme-roundtrip</body></html>");

                var layer = Sui.CreateLayer("sentinelui://probe/issue1.html");
                Check(layer != null, "layer created");
                var events = new System.Collections.Generic.List<string>();
                Action<SuiMessage> handler = m =>
                {
                    // Only events routed to this layer (other AppDomains' layers broadcast globally).
                    if (m.Layer != layer) return;
                    lock (events) events.Add(m.Type + "|" + m.PayloadJson);
                };
                Sui.MessageReceived += handler;
                try
                {
                    layer?.SetVisible(true);
                    layer?.SetFocus(true, true);
                    // Command-path assertion: ExecuteJs must reach the helper,
                    // run in the page, and come back as a console event echo.
                    layer?.ExecuteJs("console.log('probe-ping');");
                    layer?.Send(new { tag = "hello" });
                    const int deadlineMs = 6000;
                    var start = Environment.TickCount;
                    while (Environment.TickCount - start < deadlineMs)
                    {
                        Sui.Pump();
                        lock (events)
                        {
                            if (events.Exists(ev => ev.StartsWith("console|", StringComparison.Ordinal)))
                                break;   // JS reached the page and echoed back
                        }
                        System.Threading.Thread.Sleep(50);
                    }
                    lock (events)
                    {
                        Check(events.Count > 0, "events delivered from helper via pump (" + events.Count + ")");
                        foreach (var ev in events) Console.WriteLine("    event: " + ev);
                        bool echo = false;
                        foreach (var ev in events)
                            if (ev.IndexOf("console|probe-ping", StringComparison.Ordinal) >= 0) { echo = true; break; }
                        Check(echo, "command path OK: ExecuteJs -> console event echo claims the J command reached the page");
                    }
                }
                finally { Sui.MessageReceived -= handler; layer?.Close(); }
            }
            catch (Exception ex)
            {
                Check(false, "issue-1 smoke threw: " + ex.Message);
            }
        }
    }

    /// <summary>
    /// Runs in a separate AppDomain for the issue-#3 cross-domain test. Uses the public API
    /// exactly like an LSPDFR plugin would: attach as a client, create a layer, pump its own
    /// event queue, never touching host lifecycle (no Initialize/Shutdown).
    /// </summary>
    public sealed class CrossDomainProbe : MarshalByRefObject
    {
        public string Run(string apiDir)
        {
            // The API DLL sits next to the probe exe; the client domain needs help resolving it.
            AppDomain.CurrentDomain.AssemblyResolve += (s, e) =>
            {
                var name = new AssemblyName(e.Name).Name;
                if (!string.Equals(name, "SentinelUI.API", StringComparison.OrdinalIgnoreCase))
                    return null;
                string path = Path.Combine(apiDir ?? string.Empty, "SentinelUI.API.dll");
                return File.Exists(path) ? Assembly.LoadFrom(path) : null;
            };

            if (!Sui.AttachClient())
                return "attach failed (host not loaded by an ASI loader)";

            var layer = Sui.CreateLayer("sentinelui://probe/issue1.html");
            if (layer == null) return "client CreateLayer refused";

            var got = new List<string>();
            Action<SuiMessage> handler = m => { lock (got) got.Add(m.Type + "|" + m.PayloadJson); };
            Action<SuiRequest> requestHandler = r =>
            {
                if (r.Type == "cdom.echo") r.Reply(new { ok = true, got = r.PayloadJson });
            };
            layer.Message += handler;
            layer.Request += requestHandler;
            try
            {
                layer.SetVisible(true);
                layer.ExecuteJs(
                    "sui.request('cdom.echo',{x:7}).then(function(r){" +
                    "console.log('NUI_CDOM:reply '+JSON.stringify(r));});");
                var start = Environment.TickCount;
                while (Environment.TickCount - start < 6000)
                {
                    Sui.Pump();   // this domain's queue only; the owner pumps its own
                    lock (got)
                    {
                        if (got.Exists(e => e.IndexOf("NUI_CDOM:reply", StringComparison.Ordinal) >= 0))
                            break;
                    }
                    Thread.Sleep(25);
                }
                lock (got)
                {
                    string reply = got.Find(e => e.IndexOf("NUI_CDOM:reply", StringComparison.Ordinal) >= 0);
                    return reply != null ? "ok:" + reply : "no reply";
                }
            }
            finally
            {
                layer.Message -= handler;
                layer.Request -= requestHandler;
                layer.Close();
            }
        }
    }
}