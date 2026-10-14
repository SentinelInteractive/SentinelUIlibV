using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace SentinelUI
{
    /// <summary>Overall runtime health, as reported by <see cref="Sui.Diagnose"/> and the
    /// cheap <see cref="Sui.Status"/> probe.</summary>
    public enum SuiStatus
    {
        /// <summary>The UI cannot run in this AppDomain/session (host missing, export or
        /// version handshake failure).</summary>
        Unavailable = 0,

        /// <summary>The UI runs with reduced capability (older-but-supported host, missing
        /// optional exports, install-file warnings). See the report checks.</summary>
        Degraded = 1,

        /// <summary>Core path healthy: host attached, API v4+, layer model + capture available.</summary>
        Ready = 2,

        /// <summary>Intentionally off (host shut down, kill switch / safe mode). A session
        /// that shut the host down reports this.</summary>
        Disabled = 3,
    }

    /// <summary>One diagnostics check. <see cref="Required"/> failures gate
    /// <see cref="SuiStatus.Unavailable"/>; warning failures only degrade.</summary>
    public sealed class SuiCheck
    {
        internal SuiCheck(string name, bool ok, bool required, string detail)
        {
            Name = name;
            Ok = ok;
            Required = required;
            Detail = detail;
        }

        /// <summary>Stable check id (e.g. "host.module", "install.helper-exe").</summary>
        public string Name { get; }

        /// <summary>True when the check passed. False lines carry the diagnostic detail.</summary>
        public bool Ok { get; }

        /// <summary>Required checks gate the UI entirely; warnings only degrade it.</summary>
        public bool Required { get; }

        /// <summary>Human-readable state (path, version, what is missing and why it matters).</summary>
        public string Detail { get; }
    }

    /// <summary>Result of a full capability probe. Immutable snapshot; run
    /// <see cref="Sui.Diagnose"/> again for the current state.</summary>
    public sealed class SuiDiagnosticsReport
    {
        internal SuiDiagnosticsReport()
        {
            Checks = new List<SuiCheck>();
            TimestampUtc = DateTime.UtcNow;
        }

        /// <summary>Overall status derived from the checks.</summary>
        public SuiStatus Status { get; internal set; }

        /// <summary>Host export-table API version (0 when the host is absent).</summary>
        public int HostApiVersion { get; internal set; }

        /// <summary>Working directory the install-file checks ran against (game root in-game).</summary>
        public string GameDirectory { get; internal set; }

        public DateTime TimestampUtc { get; internal set; }

        public List<SuiCheck> Checks { get; }

        /// <summary>Number of failed checks (required and warning).</summary>
        public int Failures
        {
            get
            {
                int n = 0;
                foreach (var c in Checks) if (!c.Ok) n++;
                return n;
            }
        }

        /// <summary>Number of failed checks that are warnings only.</summary>
        public int Warnings
        {
            get
            {
                int n = 0;
                foreach (var c in Checks) if (!c.Ok && !c.Required) n++;
                return n;
            }
        }

        /// <summary>One-line summary for logs/console (e.g.
        /// "Degraded - 1 check(s) need attention (install.helper-exe)").</summary>
        public string Summary()
        {
            if (Failures == 0) return Status + " - all checks passed.";
            var names = new List<string>();
            foreach (var c in Checks) if (!c.Ok) names.Add(c.Name);
            return Status + " - " + names.Count + " check(s) need attention (" +
                   string.Join(", ", names.ToArray()) + ")";
        }

        /// <summary>Multi-line report for the console (`sui diag`). Never throws.</summary>
        public string ToText()
        {
            var sb = new StringBuilder();
            try
            {
                sb.Append("SentinelUI diagnostics - ").Append(Summary()).Append('\n');
                sb.Append("  time    ").Append(TimestampUtc.ToString("yyyy-MM-dd HH:mm:ss")).Append("Z\n");
                sb.Append("  game    ").Append(GameDirectory ?? "?").Append('\n');
                sb.Append("  host    ").Append(HostApiVersion > 0 ? "API v" + HostApiVersion : "not attached").Append('\n');
                sb.Append("  checks\n");
                foreach (var c in Checks)
                {
                    string mark = c.Ok ? "[ok]  " : (c.Required ? "[FAIL]" : "[warn]");
                    sb.Append("    ").Append(mark).Append(' ').Append(c.Name).Append(" - ").Append(c.Detail).Append('\n');
                }
            }
            catch (Exception ex)
            {
                sb.Append("\n  (report rendering failed: ").Append(ex.Message).Append(")\n");
            }
            return sb.ToString();
        }
    }

    /// <summary>
    /// Capability probe for the runtime. Managed-side only: it reports what this AppDomain
    /// can see, never attaches, never LoadLibrary's the host, and never touches the game.
    /// Required checks answer "can the UI run at all"; warning checks answer "is the
    /// install complete".
    /// </summary>
    public static class SuiDiagnostics
    {
        /// <summary>Minimum host export-table version this API assembly can work with
        /// (mirrors NativeHost's handshake).</summary>
        public const int RequiredHostApiVersion = 3;

        /// <summary>Host version that unlocks the full feature set (per-AppDomain event
        /// queues). Below this the runtime is usable but <see cref="SuiStatus.Degraded"/>.</summary>
        public const int PreferredHostApiVersion = 4;

        /// <summary>Runs the full probe. Passive: no side effects on host or layers.</summary>
        public static SuiDiagnosticsReport Run()
        {
            var report = new SuiDiagnosticsReport
            {
                GameDirectory = SafeCurrentDirectory(),
                HostApiVersion = NativeHost.ApiVersion,
            };

            // Host shutdown is the one state that overrides everything else: the session
            // deliberately disabled the UI, and no check result will change that.
            if (NativeHost.ShutdownRequested)
            {
                Add(report, "host.module", false, required: true,
                    detail: "host was shut down earlier this session (SentinelUI disabled)");
                report.Status = SuiStatus.Disabled;
                return report;
            }

            bool modulePresent = NativeHost.ModuleHandle != IntPtr.Zero;
            bool hostReady = NativeHost.IsAvailable;

            Add(report, "host.module", hostReady, required: true,
                detail: hostReady
                    ? $"attached (API v{NativeHost.ApiVersion})"
                    : modulePresent
                        ? "module present in-process, but this AppDomain is not attached (call Sui.Initialize/AttachClient)"
                        : "SentinelUI.asi is not loaded — the ASI loader (ScriptHookV) must load the root ASI at process start");

            Add(report, "host.api-version", hostReady && NativeHost.ApiVersion >= RequiredHostApiVersion, required: true,
                detail: hostReady
                    ? $"API v{NativeHost.ApiVersion} (needs v{RequiredHostApiVersion}+)"
                    : "unavailable (no host module)");

            Add(report, "host.layer-model", hostReady && NativeHost.SupportsLayerModel, required: true,
                detail: hostReady
                    ? (NativeHost.SupportsLayerModel
                        ? "SuiCreateLayer/SuiCloseLayer exports present"
                        : "layer-stack exports missing — update the SentinelUI runtime")
                    : "unavailable (no host module)");

            Add(report, "host.capture-input", hostReady && NativeHost.SupportsIssue1, required: false,
                detail: hostReady
                    ? (NativeHost.SupportsIssue1
                        ? "capture/execute-js exports present"
                        : "capture/execute-js exports missing — input capture and ExecuteJs disabled")
                    : "unavailable (no host module)");

            Add(report, "host.client-events", hostReady && NativeHost.SupportsClientEvents, required: false,
                detail: hostReady
                    ? (NativeHost.SupportsClientEvents
                        ? "per-AppDomain event queues available (host v4+)"
                        : $"API v{NativeHost.ApiVersion} has no per-AppDomain queues — plugin AppDomains receive no events")
                    : "unavailable (no host module)");

            string gameDir = report.GameDirectory;
            bool shvLoader = FileExistsIn(gameDir, "dinput8.dll");
            bool shv = FileExistsIn(gameDir, "ScriptHookV.dll");
            Add(report, "install.loader", shvLoader && shv, required: false,
                detail: shvLoader && shv
                    ? "dinput8.dll + ScriptHookV.dll present (load order: root ASI before RPH's Present-hook guard)"
                    : "dinput8.dll and/or ScriptHookV.dll missing in the game root — the root ASI must be " +
                      "ASI-loader-loaded at process start, or the UI degrades to a no-op");

            bool helperExe = FileExistsIn(gameDir, "SentinelUI_Host.exe");
            Add(report, "install.helper-exe", helperExe, required: false,
                detail: helperExe
                    ? "SentinelUI_Host.exe present"
                    : "SentinelUI_Host.exe missing — the host cannot start the CEF helper; UI stays blank");

            bool cefRuntime = FileExistsIn(gameDir, "libcef.dll");
            Add(report, "install.cef-runtime", cefRuntime, required: false,
                detail: cefRuntime
                    ? "libcef.dll present"
                    : "libcef.dll missing next to the game root — the helper cannot start CEF");

            bool contentDir = gameDir != null && Directory.Exists(Path.Combine(gameDir, "SentinelUI"));
            Add(report, "install.content-dir", contentDir, required: false,
                detail: contentDir ? "SentinelUI\\ (content/logs/cache) present" : "SentinelUI\\ missing (content/cache paths)");

            report.Status = DeriveStatus(report);
            return report;
        }

        private static SuiStatus DeriveStatus(SuiDiagnosticsReport report)
        {
            bool requiredFailure = false;
            bool anyFailure = false;
            foreach (var c in report.Checks)
            {
                if (c.Ok) continue;
                anyFailure = true;
                if (c.Required) requiredFailure = true;
            }
            if (requiredFailure) return SuiStatus.Unavailable;
            return anyFailure ? SuiStatus.Degraded : SuiStatus.Ready;
        }

        private static void Add(SuiDiagnosticsReport report, string name, bool ok, bool required, string detail)
        {
            report.Checks.Add(new SuiCheck(name, ok, required, detail));
        }

        private static bool FileExistsIn(string directory, params string[] names)
        {
            if (string.IsNullOrEmpty(directory)) return false;
            foreach (var name in names)
            {
                try
                {
                    if (File.Exists(Path.Combine(directory, name))) return true;
                }
                catch { /* malformed path: treat as missing */ }
            }
            return false;
        }

        private static string SafeCurrentDirectory()
        {
            try { return Environment.CurrentDirectory; }
            catch { return null; }
        }
    }
}
