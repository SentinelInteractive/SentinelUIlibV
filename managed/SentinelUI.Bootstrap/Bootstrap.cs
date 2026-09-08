using System;
using System.IO;
using Rage;

// RPH plugin contract: assembly-level attribute + static entry point. RPH auto-loads this DLL
// from <game>\Plugins\. It is the ONLY supported loader of the native CEF host.
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

        /// <summary>Entry point, invoked by RPH on the game thread at plugin load.</summary>
        public static void Main()
        {
            try
            {
                Game.LogTrivial("[SentinelUI] Bootstrap loading (API 0.1.0)...");

                var hostPath = ResolveHostPath();
                if (hostPath == null)
                {
                    Game.LogTrivial("[SentinelUI] SentinelUI.asi not found — install the SentinelUI runtime " +
                                    "(SentinelUI\\SentinelUI.asi). Plugins will degrade gracefully.");
                    return;
                }

                if (!SentinelUI.Sui.Initialize(hostPath))
                {
                    Game.LogTrivial("[SentinelUI] Host initialization failed — see SentinelUI\\logs\\sentinelui_api_*.log.");
                    return;
                }

                Game.LogTrivial($"[SentinelUI] Native host ready (API v{SentinelUI.Sui.HostApiVersion}). Starting pump...");
                StartPump();
                Game.LogTrivial("[SentinelUI] Bootstrap ready.");
            }
            catch (Exception ex)
            {
                // Lifecycle failures must never take the game down silently — log, then keep going.
                Game.LogTrivial("[SentinelUI] Bootstrap.Main failed: " + ex);
            }
        }

        /// <summary>Exit point, invoked by RPH on unload/shutdown. Runs on the game thread.</summary>
        public static void OnUnload(bool isTerminating)
        {
            _running = false;

            if (_pumpFiber != null && _pumpFiber.IsAlive)
            {
                try { _pumpFiber.Abort(); } catch { /* fiber already gone */ }
                _pumpFiber = null;
            }

            // Graceful host shutdown — NEVER inside DllMain/PROCESS_DETACH; this path is safe.
            if (SentinelUI.Sui.HostAvailable)
            {
                Game.LogTrivial("[SentinelUI] Shutting down host...");
                SentinelUI.Sui.Shutdown();
            }
            Game.LogTrivial("[SentinelUI] Bootstrap unloaded.");
        }

        private static void StartPump()
        {
            _running = true;
            _pumpFiber = GameFiber.StartNew(() =>
            {
                while (_running)
                {
                    try { SentinelUI.Sui.Pump(); }
                    catch (Exception ex)
                    {
                        Game.LogTrivial("[SentinelUI] Pump error: " + ex);
                    }
                    GameFiber.Yield();
                }
            });
        }

        private static string ResolveHostPath()
        {
            var wd = Environment.CurrentDirectory; // game root when launched via RPH
            var baseDir = AppDomain.CurrentDomain.BaseDirectory; // <game>\Plugins\ when loaded by RPH

            string[] candidates =
            {
                Path.Combine(wd, "SentinelUI", "SentinelUI.asi"),
                Path.Combine(baseDir, "..", "SentinelUI", "SentinelUI.asi"),
                Path.Combine(baseDir, "SentinelUI", "SentinelUI.asi"),
                Path.Combine(wd, "SentinelUI.asi"), // dev convenience copy
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
