using System;
using System.IO;
using System.Text;
using System.Threading;

namespace SentinelUI
{
    /// <summary>
    /// SentinelUI's own file logger. Policy: never spam RagePluginHook.log; everything of interest
    /// goes here (rotated file under &lt;game&gt;\SentinelUI\logs\). RPH's log only receives
    /// plugin lifecycle lines from the Bootstrap plugin.
    /// </summary>
    internal static class SuiLog
    {
        private static readonly object _gate = new object();
        private static string _file;
        private static long _maxBytes = 2 * 1024 * 1024;
        private static long _bytes;

        public static bool Verbose { get; set; }

        public static void Initialize(string directory)
        {
            lock (_gate)
            {
                if (_file != null) return;
                try
                {
                    Directory.CreateDirectory(directory);
                    _file = Path.Combine(directory, "sentinelui_api_" + DateTime.Now.ToString("yyyyMMdd") + ".log");
                }
                catch
                {
                    _file = null;
                }
            }
        }

        public static void Info(string message) => Write("INFO ", message);
        public static void Warn(string message) => Write("WARN ", message);
        public static void Error(string message) => Write("ERROR", message);
        public static void Debug(string message) { if (Verbose) Write("DEBUG", message); }

        private static void Write(string level, string message)
        {
            lock (_gate)
            {
                if (_file == null) return;
                try
                {
                    var line = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] [{level}] {message}{Environment.NewLine}";
                    var bytes = Encoding.UTF8.GetBytes(line);
                    if (_bytes + bytes.Length > _maxBytes && File.Exists(_file))
                    {
                        // Rotate-in-place: keep the most recent half of the log file.
                        var content = File.ReadAllBytes(_file); // bounded by _maxBytes (2 MB)
                        int keep = Math.Min(content.Length, (int)(_maxBytes / 2));
                        var tail = new byte[keep];
                        Array.Copy(content, content.Length - keep, tail, 0, keep);
                        File.WriteAllBytes(_file, tail);
                        _bytes = tail.Length;
                    }
                    using (var sw = new StreamWriter(_file, true, Encoding.UTF8))
                    {
                        sw.Write(line);
                    }
                    _bytes += bytes.Length;
                }
                catch
                {
                    // Logging must never crash the host process.
                }
            }
        }
    }
}
