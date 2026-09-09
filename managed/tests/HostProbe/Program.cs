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

            // 3b. Issue-1 smoke (IPC + capture + events) — needs the helper running.
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

        private static void Issue1(bool init)
        {
            // Requires the host to have spawned the helper + IPC (debug env starts both).
            if (!init || !Sui.HostAvailable) return;
            Console.WriteLine("[5] issue-1 IPC/capture/events");
            try
            {
                Check(!Sui.CaptureActive, "Capture inactive by default");
                Sui.SetCapture(true);
                Check(Sui.CaptureActive, "SetCapture(true) reflected on export");
                Sui.SetCapture(false);

                var layer = Sui.CreateLayer("sentinelui://probe/issue1.html");
                Check(layer != null, "layer created");
                var events = new System.Collections.Generic.List<string>();
                Action<SuiMessage> handler = m => { lock (events) events.Add(m.Type + "|" + m.PayloadJson); };
                Sui.MessageReceived += handler;
                try
                {
                    layer?.SetVisible(true);
                    layer?.SetFocus(true, true);
                    layer?.ExecuteJs("document.title='probe';");
                    layer?.Send(new { tag = "hello" });
                    const int deadlineMs = 6000;
                    var start = Environment.TickCount;
                    while (Environment.TickCount - start < deadlineMs)
                    {
                        Sui.Pump();
                        lock (events) { if (events.Count > 0) break; }
                        System.Threading.Thread.Sleep(50);
                    }
                    lock (events)
                    {
                        Check(events.Count > 0, "events delivered from helper via pump (" + events.Count + ")");
                        if (events.Count > 0) Console.WriteLine("  sample event: " + events[0]);
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