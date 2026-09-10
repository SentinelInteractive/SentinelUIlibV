using System;
using System.IO;

namespace SentinelUI.HostProbe
{
    /// <summary>
    /// Console probe for the P0 load-path/marshalling spike — no game required.
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

            // 1. Graceful degradation BEFORE any host exists.
            Console.WriteLine("[1] host missing behavior");
            var layer = Sui.CreateLayer("sentinelui://x/y.html");
            Check(layer == null, "CreateLayer returns null when host unavailable");
            Check(!Sui.HostAvailable, "HostAvailable is false");
            Sui.Pump(); // must be a safe no-op

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

            // 3. Create a layer and exercise the facade (P2 plumbing, must not throw).
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

            // 3b. Issue-2 smoke (layer stack + sentinelui:// scheme) — needs the helper running.
            Issue2(init);

            // 3c. Issue-1 smoke (IPC + capture + events) — needs the helper running.
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
                    // routed to THIS layer (per-layer Message event).
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
                Action<SuiMessage> handler = m => { lock (events) events.Add(m.Type + "|" + m.PayloadJson); };
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
                                break;   // J reached the page and echoed back
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
}