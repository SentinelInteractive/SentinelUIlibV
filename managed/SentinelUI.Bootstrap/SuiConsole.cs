using System;
using Rage;
using Rage.Attributes;

namespace SentinelUI.Bootstrap
{
    /// <summary>
    /// RPH console command group. Runs in the Bootstrap AppDomain: pure RPH, no host/CEF
    /// dependency, so it stays usable exactly when the UI is broken and the user needs to
    /// know why.
    /// </summary>
    public static class SuiConsole
    {
        [ConsoleCommand("SentinelUI: status | diag", Name = "sui")]
        public static void Command_Sui([ConsoleCommandParameter("status or diag")] string action)
        {
            try
            {
                switch ((action ?? "status").Trim().ToLowerInvariant())
                {
                    case "status":
                        PrintStatus();
                        break;
                    case "diag":
                        PrintDiagnostics();
                        break;
                    default:
                        Game.Console.Print("[SentinelUI] usage: sui status | sui diag");
                        break;
                }
            }
            catch (Exception ex)
            {
                // A console command must never throw into RPH.
                Game.Console.Print("[SentinelUI] command failed: " + ex.Message);
                Game.LogTrivial("[SentinelUI] console command failed: " + ex);
            }
        }

        private static void PrintStatus()
        {
            var last = SentinelUI.Sui.LastDiagnostics;
            Game.Console.Print("[SentinelUI] status: " + SentinelUI.Sui.Status);
            Game.Console.Print("[SentinelUI] host API v" + SentinelUI.Sui.HostApiVersion +
                               ", capture " + (SentinelUI.Sui.CaptureActive ? "on" : "off") +
                               ", layers " + SentinelUI.Sui.LayerCount);
            if (last != null)
                Game.Console.Print("[SentinelUI] last check: " + last.Summary() +
                                   " (" + last.TimestampUtc.ToString("HH:mm:ss") + "Z)");
            if (SentinelUI.Sui.Status != SuiStatus.Ready)
                Game.Console.Print("[SentinelUI] run 'sui diag' for the full check list; details in SentinelUI\\logs.");
        }

        private static void PrintDiagnostics()
        {
            var report = SentinelUI.Sui.Diagnose();
            foreach (var line in report.ToText().Replace("\r\n", "\n").Split('\n'))
                Game.Console.Print(line.Length > 0 ? line : " ");
            Game.LogTrivial("[SentinelUI] sui diag: " + report.Summary());
        }
    }
}
