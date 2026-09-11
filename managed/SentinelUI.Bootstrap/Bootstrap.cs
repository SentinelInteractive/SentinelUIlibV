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
        private static System.Reflection.Assembly _apiAssembly;

        // RPH loads each plugin into its own AppDomain whose probing path is the
        // GAME ROOT, not the plugin's folder. SentinelUI.API.dll may therefore
        // not resolve without help — resolve it explicitly from the known locations.
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
                Game.LogTrivial("[SentinelUI] Bootstrap loading (API 0.3.0)...");

                var hostPath = ResolveHostPath();
                if (hostPath == null)
                {
                    Game.LogTrivial("[SentinelUI] SentinelUI.asi not found in the game root — install the " +
                                    "SentinelUI runtime (root ASI, loaded by ScriptHookV). Plugins will degrade gracefully.");
                    return;
                }

                // Attach-only: the ASI must have been loaded by the ASI loader (ScriptHookV)
                // at process start. Bootstrap never LoadLibrary's it — RPH's Present-hook guard
                // reverts hooks installed after its own and crashes the D3D chain (2026-09-11).
                if (!SentinelUI.Sui.Initialize(hostPath, loadIfNeeded: false))
                {
                    Game.LogTrivial("[SentinelUI] Host not loaded — is ScriptHookV installed and SentinelUI.asi " +
                                    "in the game root? See SentinelUI\\logs\\sentinelui_api_*.log. UI disabled.");
                    return;
                }

                Game.LogTrivial($"[SentinelUI] Native host ready (API v{SentinelUI.Sui.HostApiVersion}). Starting pump...");
                StartPump();
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
            }
            catch (Exception ex)
            {
                // Lifecycle failures must never take the game down silently — log, then keep going.
                Game.LogTrivial("[SentinelUI] Bootstrap.Main failed: " + ex);
            }
        }

        /// <summary>Exit point, invoked by RPH on unload/shutdown. Runs on the game thread.
        /// Everything here must be exception-safe — RPH reports but does not contain failures.</summary>
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

                // Graceful host shutdown — NEVER inside DllMain/PROCESS_DETACH; this path is safe.
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
                        Game.LogTrivial("[SentinelUI] Pump error: " + ex);
                    }
                    GameFiber.Yield();
                }
            });
        }

        // DISABLE_CONTROL_ACTION (from natives.json PAD). While a UI has input focus the
        // game must not eat keys OR the mouse: disable the entire default control group.
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

            // The host ASI must live at the GAME ROOT: ScriptHookV auto-loads it at
            // process start, BEFORE RPH installs its Present hook. Bootstrap then
            // attaches to the already-loaded module (LoadLibrary of the same path
            // returns the existing handle). Cold-loading later makes RPH's hook
            // guard revert our Present hook and crash the D3D chain (2026-09-11).
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
