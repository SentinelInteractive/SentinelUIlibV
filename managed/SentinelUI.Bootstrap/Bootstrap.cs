using System;
using System.Collections.Generic;
using System.IO;
using Rage;

// RPH plugin contract: assembly-level attribute + static entry point. RPH auto-loads this DLL
// from <game>\Plugins\. It is the only supported owner of the native host lifecycle.
[assembly: Rage.Attributes.Plugin("Sentinel UI",
    Author = "SentinelInteractive",
    Description = "SentinelUI host lifecycle owner + per-frame event pump (RPH + LSPDFR web UI library).",
    EntryPoint = "SentinelUI.Bootstrap.Bootstrap.Main",
    ExitPoint = "SentinelUI.Bootstrap.Bootstrap.OnUnload")]

namespace SentinelUI.Bootstrap
{
    public static class Bootstrap
    {
        private static GameFiber _pumpFiber;
        private static volatile bool _running;
        private static System.Reflection.Assembly _apiAssembly;
        private static readonly HashSet<string> _notifiedClasses = new HashSet<string>();
        private static int _pumpErrorCount;

        // RPH loads each plugin into its own AppDomain whose probing path is the
        // game root, not the plugin's folder. SentinelUI.API.dll may therefore
        // not resolve without help: resolve it explicitly from the known locations.
        static Bootstrap()
        {
            AppDomain.CurrentDomain.AssemblyResolve += (s, e) =>
            {
                var name = new System.Reflection.AssemblyName(e.Name).Name;
                if (!string.Equals(name, "SentinelUI.API", StringComparison.OrdinalIgnoreCase))
                    return null;
                if (_apiAssembly != null) return _apiAssembly;

                var pluginDir = Path.GetDirectoryName(typeof(Bootstrap).Assembly.Location);
                string[] candidates =
                {
                    Path.Combine(pluginDir ?? "", "SentinelUI.API.dll"),
                    Path.Combine(pluginDir ?? "", "..", "SentinelUI.API.dll"),
                    Path.Combine(Environment.CurrentDirectory, "SentinelUI.API.dll"),
                    Path.Combine(Environment.CurrentDirectory, "SentinelUI", "SentinelUI.API.dll"),
                };
                foreach (var candidate in candidates)
                {
                    try
                    {
                        if (File.Exists(candidate))
                            return _apiAssembly = System.Reflection.Assembly.LoadFrom(Path.GetFullPath(candidate));
                    }
                    catch { /* try next candidate */ }
                }
                return null;
            };
        }

        /// <summary>Entry point, invoked by RPH on the game thread at plugin load.</summary>
        public static void Main()
        {
            try
            {
                Game.LogTrivial("[SentinelUI] Bootstrap loading (API 0.5.0)...");

                // Console commands first: they are pure RPH and must work even when the
                // runtime is missing or broken (sui status / sui diag).
                TryRegisterConsoleCommands();

                var hostPath = ResolveHostPath();
                if (hostPath == null)
                {
                    Game.LogTrivial("[SentinelUI] SentinelUI.asi not found in the game root — install the " +
                                    "SentinelUI runtime (root ASI, loaded by ScriptHookV). Plugins will degrade gracefully.");
                    NotifyOnce("install", "runtime not installed - UI disabled (see log)");
                    return;
                }

                // Attach-only: the ASI must have been loaded by the ASI loader (ScriptHookV)
                // at process start. Bootstrap never LoadLibrary's it: RPH's Present-hook guard
                // reverts hooks installed after its own and crashes the D3D chain (2026-09-11).
                if (!SentinelUI.Sui.Initialize(hostPath, loadIfNeeded: false))
                {
                    Game.LogTrivial("[SentinelUI] Host not loaded — is ScriptHookV installed and SentinelUI.asi " +
                                    "in the game root? See SentinelUI\\logs\\sentinelui_api_*.log. UI disabled.");
                    NotifyOnce("attach", "host not loaded - UI disabled (see log)");
                    return;
                }

                Game.LogTrivial($"[SentinelUI] Native host ready (API v{SentinelUI.Sui.HostApiVersion}). Starting pump...");
                RunSelfCheck();
                StartPump();
                // Page requests (sui.request): the boot demo asks for process stats.
                SentinelUI.Sui.RequestReceived += TryHandlePageRequest;
                // Page events are already logged in SentinelUI\logs; also mirror
                // NUI_* messages to the RPH console line so in-game tests are visible.
                SentinelUI.Sui.MessageReceived += (m) =>
                {
                    try
                    {
                        if (string.Equals(m.Type, "console", StringComparison.Ordinal) &&
                            m.PayloadJson != null && m.PayloadJson.StartsWith("NUI_"))
                        {
                            Game.LogTrivial("[SentinelUI] page event: " + m.PayloadJson);
                        }
                    }
                    catch { /* never fail the pump */ }
                };
                Game.LogTrivial("[SentinelUI] Bootstrap ready.");
                TryCreateBadgeLayer();
            }
            catch (Exception ex)
            {
                // Lifecycle failures must never take the game down silently: log, then keep going.
                Game.LogTrivial("[SentinelUI] Bootstrap.Main failed: " + ex);
                NotifyOnce("startup", "startup failed - UI disabled (see log)");
            }
        }

        /// <summary>Registers the `sui` console command group. Failure is non-fatal:
        /// a command-name collision must never break the plugin load.</summary>
        private static void TryRegisterConsoleCommands()
        {
            try
            {
                Game.AddConsoleCommands(new[] { typeof(SuiConsole) });
                Game.LogTrivial("[SentinelUI] Console commands registered (sui status | sui diag).");
            }
            catch (Exception ex)
            {
                Game.LogTrivial("[SentinelUI] Console commands unavailable: " + ex.Message);
            }
        }

        /// <summary>Startup capability probe: diagnose, log the failures, and tell the user
        /// (one native notification per failure class). Runs even when the UI is only
        /// partially usable, so problems surface before a plugin trips on them.</summary>
        private static void RunSelfCheck()
        {
            try
            {
                var report = SentinelUI.Sui.Diagnose();
                Game.LogTrivial("[SentinelUI] self-check: " + report.Summary());
                foreach (var check in report.Checks)
                {
                    if (!check.Ok)
                        Game.LogTrivial($"[SentinelUI] self-check {check.Name} " +
                                        $"({(check.Required ? "required" : "warning")}): {check.Detail}");
                }
                if (report.Status == SentinelUI.SuiStatus.Unavailable)
                    NotifyOnce("runtime", "UI unavailable - type 'sui diag' in console (see log)");
                else if (report.Status == SentinelUI.SuiStatus.Degraded)
                    NotifyOnce("degraded", "running with warnings - type 'sui diag' in console");
            }
            catch (Exception ex)
            {
                Game.LogTrivial("[SentinelUI] self-check failed (non-fatal): " + ex);
            }
        }

        /// <summary>One native notification per failure class per session. Runs on pure RPH:
        /// works when the host, helper or CEF are dead. Never throws.</summary>
        private static void NotifyOnce(string failureClass, string text)
        {
            try
            {
                if (_notifiedClasses.Contains(failureClass)) return;
                _notifiedClasses.Add(failureClass);
                Game.DisplayNotification("~o~SentinelUI~s~ " + text);
            }
            catch { /* notifications are best-effort */ }
        }

        /// <summary>Exit point, invoked by RPH on unload/shutdown. Runs on the game thread.
        /// Everything here must be exception-safe: RPH reports but does not contain failures.</summary>
        public static void OnUnload(bool isTerminating)
        {
            try
            {
                _running = false;

                if (_pumpFiber != null && _pumpFiber.IsAlive)
                {
                    try { _pumpFiber.Abort(); } catch { /* fiber already gone */ }
                    _pumpFiber = null;
                }

                // Graceful host shutdown: safe on this path, never from DllMain/PROCESS_DETACH.
                if (SentinelUI.Sui.HostAvailable)
                {
                    Game.LogTrivial("[SentinelUI] Shutting down host...");
                    SentinelUI.Sui.Shutdown();
                }
                Game.LogTrivial("[SentinelUI] Bootstrap unloaded.");
            }
            catch (Exception ex)
            {
                Game.LogTrivial("[SentinelUI] OnUnload failed (non-fatal): " + ex);
            }
        }

        // Always-on-top badge layer.

        // The Sentinel Interactive brand/status panel is its own layer: never steals
        // input (click-through) and composites above plugin layers (z 9000; ascending-z
        // draw order, ties by layer id). F9 hides it with the rest of the overlay.
        private const int BadgeLayerZ = 9000;
        private static SentinelUI.SuiLayer _badgeLayer;

        private static void TryCreateBadgeLayer()
        {
            try
            {
                _badgeLayer = SentinelUI.Sui.CreateLayer("sentinelui://sui/badge.html");
                if (_badgeLayer == null)
                {
                    Game.LogTrivial("[SentinelUI] Badge layer unavailable (layer budget or host refused) - continuing without it.");
                    return;
                }
                _badgeLayer.SetClickThrough(true);
                _badgeLayer.SetZ(BadgeLayerZ);
                Game.LogTrivial($"[SentinelUI] Badge layer created (native id {_badgeLayer.NativeId}, " +
                                $"z {BadgeLayerZ}, click-through).");
            }
            catch (Exception ex)
            {
                Game.LogTrivial("[SentinelUI] Badge layer failed (non-fatal): " + ex.Message);
            }
        }

        // Page requests (sui.request).

        /// <summary>Answers page requests. The boot demo asks for `sys.stats`
        /// (process CPU/memory) to fill its side panel; unknown types are ignored so
        /// plugin-owned request types pass through untouched.</summary>
        private static void TryHandlePageRequest(SentinelUI.SuiRequest request)
        {
            try
            {
                if (request == null || !string.Equals(request.Type, "sys.stats", StringComparison.Ordinal))
                    return;
                request.Reply(SampleProcessStats());
            }
            catch (Exception ex)
            {
                Game.LogTrivial("[SentinelUI] sys.stats request failed: " + ex.Message);
            }
        }

        // Sampled between requests (the page polls ~1/s): Task-Manager-style percent
        // across all cores for this process (GTA5.exe incl. plugins, not the helper).
        private static readonly object _statsGate = new object();
        private static DateTime _statsLastWall = DateTime.MinValue;
        private static TimeSpan _statsLastCpu;

        private static object SampleProcessStats()
        {
            lock (_statsGate)
            {
                var proc = System.Diagnostics.Process.GetCurrentProcess();
                DateTime now = DateTime.UtcNow;
                TimeSpan cpuNow = proc.TotalProcessorTime;
                long memMb = proc.WorkingSet64 / (1024 * 1024);

                int? cpu = null;   // first sample has no baseline yet -> page shows "-"
                if (_statsLastWall != DateTime.MinValue)
                {
                    double wallMs = (now - _statsLastWall).TotalMilliseconds;
                    double cpuMs = (cpuNow - _statsLastCpu).TotalMilliseconds;
                    if (wallMs > 1)
                    {
                        double pct = cpuMs / (wallMs * Environment.ProcessorCount) * 100.0;
                        cpu = (int)Math.Round(Math.Max(0, Math.Min(100, pct)));
                    }
                }
                _statsLastWall = now;
                _statsLastCpu = cpuNow;

                return new { cpu, memMB = memMb, cores = Environment.ProcessorCount };
            }
        }

        private static void StartPump()
        {
            _running = true;
            _pumpFiber = GameFiber.StartNew(() =>
            {
                while (_running)
                {
                    try
                    {
                        SentinelUI.Sui.Pump();
                        // While a UI has input focus, the game must not eat the keys/mouse.
                        if (SentinelUI.Sui.CaptureActive) DisableCaptureControls();
                    }
                    catch (Exception ex)
                    {
                        // Throttle: a per-frame failure must not flood the RPH log. First 3
                        // errors in full, then every 300th frame.
                        _pumpErrorCount++;
                        if (_pumpErrorCount <= 3 || _pumpErrorCount % 300 == 0)
                            Game.LogTrivial($"[SentinelUI] Pump error #{_pumpErrorCount}: {ex.GetType().Name}: {ex.Message}");
                    }
                    GameFiber.Yield();
                }
            });
        }

        // DISABLE_CONTROL_ACTION (PAD namespace). While a UI has input focus the game must
        // not eat keys or the mouse: disable the entire default control group.
        private const ulong DisableControlAction = 0xFE99B66D079CF6BC;

        private static void DisableCaptureControls()
        {
            // 0..340 covers the standard control index space (movement, look, cursor, ...).
            for (int control = 0; control <= 340; control++)
            {
                try
                {
                    Rage.Native.NativeFunction.CallByHash<bool>(DisableControlAction, 0, control, true);
                }
                catch { /* native call fails gracefully on game-thread edge cases */ }
            }
        }

        private static string ResolveHostPath()
        {
            var wd = Environment.CurrentDirectory; // game root when launched via RPH
            var baseDir = AppDomain.CurrentDomain.BaseDirectory; // <game>\Plugins\ when loaded by RPH

            // The host ASI must live at the game root: ScriptHookV auto-loads it at
            // process start, before RPH installs its Present hook. A cold load later
            // would make RPH's hook guard revert our Present hook and crash the D3D
            // chain (2026-09-11).
            string[] candidates =
            {
                Path.Combine(wd, "SentinelUI", "SentinelUI.asi"),   // future split layout
                Path.Combine(baseDir, "..", "SentinelUI", "SentinelUI.asi"),
                Path.Combine(baseDir, "SentinelUI", "SentinelUI.asi"),
                Path.Combine(wd, "SentinelUI.asi"),                 // canonical (root, SHV-loaded)
            };

            foreach (var candidate in candidates)
            {
                try
                {
                    var full = Path.GetFullPath(candidate);
                    if (File.Exists(full)) return full;
                }
                catch { /* skip malformed candidates */ }
            }
            return null;
        }
    }
}
