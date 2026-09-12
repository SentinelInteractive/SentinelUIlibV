using System;
using System.IO;
using System.Reflection;

using LSPD_First_Response.Mod.API;
using Rage;

namespace SuiSample
{
    /// <summary>
    /// Pack entry point. LSPDFR scans <c>Plugins\LSPDFR\</c> and instantiates the RPH
    /// <see cref="Plugin"/> class found in each assembly — callouts are NOT auto-discovered.
    /// They must be registered explicitly with <see cref="Functions.RegisterCallout"/>,
    /// normally when the player goes on duty (same pattern as SentinelCalloutsV).
    /// </summary>
    public sealed class Main : Plugin
    {
        private static bool _registered;

        static Main()
        {
            ApiResolver.Ensure();
        }

        public override void Initialize()
        {
            Game.LogTrivial("[SuiSample] pack loading...");
            Functions.OnOnDutyStateChanged += OnOnDutyStateChanged;
            Game.LogTrivial("[SuiSample] loaded — go on duty to register the callout.");
        }

        public override void Finally()
        {
            Functions.OnOnDutyStateChanged -= OnOnDutyStateChanged;
            Game.LogTrivial("[SuiSample] pack unloaded.");
        }

        private static void OnOnDutyStateChanged(bool onDuty)
        {
            if (!onDuty || _registered) return;
            try
            {
                Functions.RegisterCallout(typeof(SuiSampleCallout));
                _registered = true;
                Game.LogTrivial("[SuiSample] registered callout with LSPDFR.");
                Game.DisplayNotification("~b~[SuiSample]~w~ callout registered — F4 console: ~y~ForceCallout SuiSample");
            }
            catch (Exception ex)
            {
                Game.LogTrivial("[SuiSample] RegisterCallout failed: " + ex);
            }
        }
    }

    /// <summary>
    /// Resolves SentinelUI.API.dll from the known deploy locations. Registered from a
    /// static constructor so it is active before any member touching SentinelUI types
    /// is JIT'd.
    /// </summary>
    internal static class ApiResolver
    {
        private static bool _installed;

        public static void Ensure()
        {
            if (_installed) return;
            _installed = true;
            AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        }

        private static Assembly Resolve(object sender, ResolveEventArgs args)
        {
            var name = new AssemblyName(args.Name).Name;
            if (!string.Equals(name, "SentinelUI.API", StringComparison.OrdinalIgnoreCase))
                return null;

            var packDir = Path.GetDirectoryName(typeof(ApiResolver).Assembly.Location);
            string[] candidates =
            {
                Path.Combine(packDir ?? "", "SentinelUI.API.dll"),
                Path.Combine(Environment.CurrentDirectory, "SentinelUI.API.dll"),
                Path.Combine(Environment.CurrentDirectory, "Plugins", "SentinelUI.API.dll"),
                Path.Combine(Environment.CurrentDirectory, "SentinelUI", "SentinelUI.API.dll"),
            };
            foreach (var candidate in candidates)
            {
                try
                {
                    if (File.Exists(candidate))
                        return Assembly.LoadFrom(Path.GetFullPath(candidate));
                }
                catch { /* try next */ }
            }
            return null;
        }
    }
}
