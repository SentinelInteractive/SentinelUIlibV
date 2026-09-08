using System;
using System.IO;
using System.Runtime.InteropServices;

namespace SentinelUI
{
    /// <summary>
    /// Low-level bridge to the native CEF host module (SentinelUI.asi).
    ///
    /// IMPORTANT (architecture decision D2/DC1): the ONLY supported way to load the host is the
    /// RPH Bootstrap plugin calling <see cref="NativeHost.Initialize"/> with the absolute path to
    /// &lt;game&gt;\SentinelUI\SentinelUI.asi. The module intentionally lives in a folder that ASI
    /// loaders never scan, so there is exactly one load path, one hook order and one init thread.
    ///
    /// LoadLibrary-by-path is used instead of static DllImport so we control resolution and can
    /// degrade gracefully (log + null object) when the runtime is not installed.
    /// </summary>
    internal static class NativeHost
    {
        private const string LogDirectory = "SentinelUI";

        // ---- host export table (mirrors native/host_exports.h, versioned by SuiGetApiVersion) ----
        // v2 planned exports:
        //   int    SuiGetApiVersion(void)
        //   const  SuiVersion* SuiGetVersion(void)
        //   int    SuiCreateLayer(const wchar_t* urlOrHtml)
        //   int    SuiCloseLayer(int layerId)
        //   void   SuiCloseAllLayers(void)
        //   void   SuiSetLayerVisible(int layerId, int visible)
        //   int    SuiIsLayerVisible(int layerId)
        //   void   SuiSetLayerZ(int layerId, int z)
        //   void   SuiBringToFront(int layerId) / SuiSendToBack(int layerId)
        //   void   SuiSetLayerClickThrough(int layerId, int enabled)
        //   int    SuiIsLayerClickThrough(int layerId)
        //   void   SuiSetLayerFocus(int layerId, int keyboard, int mouse)
        //   int    SuiIsLayerFocused(int layerId)
        //   void   SuiExecuteJs(int layerId, const char* jsUtf8)
        //   void   SuiSendData(int layerId, const char* jsonEnvelopeUtf8)
        //   uint64 SuiRegisterEventSink(SuiEventCallback cb, int* outEpoch)
        //   void   SuiUnregisterEventSink(uint64 token)
        //   void   SuiPumpEvents(void)                 // drains per-token queues (call on game thread)
        //   void   SuiShutdown(void)                    // Bootstrap unload ONLY, never PROCESS_DETACH
        //   int    SuiHealth(SuiHealth* outHealth)
        // Phase 0/1 skeleton only wires: SuiGetApiVersion, SuiPumpEvents, SuiShutdown.

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string lpFileName);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern int FreeLibrary(IntPtr hModule);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SuiGetApiVersionDelegate();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SuiPumpEventsDelegate();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SuiShutdownDelegate();

        private static IntPtr _module;
        private static int _apiVersion;
        private static SuiGetApiVersionDelegate _getApiVersion;
        private static SuiPumpEventsDelegate _pumpEvents;
        private static SuiShutdownDelegate _shutdown;
        private static bool _shutdownRequested;

        public static bool IsAvailable => _module != IntPtr.Zero && _apiVersion >= 1 && !_shutdownRequested;
        public static int ApiVersion => _apiVersion;
        public static IntPtr ModuleHandle => _module;

        public static bool Initialize(string hostPath)
        {
            if (_module != IntPtr.Zero) return true; // already loaded (GetModuleHandle guard lives here)
            if (string.IsNullOrWhiteSpace(hostPath) || !File.Exists(hostPath))
            {
                SuiLog.Warn($"Host module not found at '{hostPath}'. SentinelUI unavailable (graceful degradation).");
                return false;
            }

            try
            {
                _module = LoadLibraryW(Path.GetFullPath(hostPath));
                if (_module == IntPtr.Zero)
                {
                    SuiLog.Error($"LoadLibrary failed for '{hostPath}' (Win32 error {Marshal.GetLastWin32Error()}).");
                    return false;
                }

                _getApiVersion = LoadExport<SuiGetApiVersionDelegate>("SuiGetApiVersion");
                if (_getApiVersion == null)
                {
                    SuiLog.Error("Host module is missing SuiGetApiVersion export — version mismatch?");
                    FreeLibrary(_module);
                    _module = IntPtr.Zero;
                    return false;
                }

                _apiVersion = _getApiVersion();
                _pumpEvents = LoadExport<SuiPumpEventsDelegate>("SuiPumpEvents");
                _shutdown = LoadExport<SuiShutdownDelegate>("SuiShutdown");

                if (_apiVersion < 1)
                {
                    SuiLog.Error($"Host API version {_apiVersion} is not supported by this API assembly.");
                    FreeLibrary(_module);
                    _module = IntPtr.Zero;
                    return false;
                }

                // Per-domain lifetime: if this AppDomain (plugin) goes away while the host keeps
                // running, make sure we don't hold anything that would let the host call back into it.
                AppDomain.CurrentDomain.DomainUnload += OnDomainUnload;
                AppDomain.CurrentDomain.ProcessExit += OnProcessExit;

                SuiLog.Info($"Native host initialized: '{hostPath}' (API v{_apiVersion}).");
                return true;
            }
            catch (Exception ex)
            {
                SuiLog.Error("NativeHost.Initialize failed: " + ex);
                return false;
            }
        }

        private static T LoadExport<T>(string name) where T : Delegate
        {
            IntPtr proc = GetProcAddress(_module, name);
            return proc == IntPtr.Zero ? null : Marshal.GetDelegateForFunctionPointer<T>(proc);
        }

        /// <summary>Drains host event queues. MUST be called on the game thread (per-frame).</summary>
        public static void PumpEvents()
        {
            try { _pumpEvents?.Invoke(); }
            catch (Exception ex) { SuiLog.Error("PumpEvents failed: " + ex); }
        }

        /// <summary>Graceful host shutdown. Only ever called from the Bootstrap unload path.</summary>
        public static void Shutdown()
        {
            if (_shutdownRequested) return;
            _shutdownRequested = true;
            try
            {
                SuiLog.Info("Shutting down native host...");
                _shutdown?.Invoke();
            }
            catch (Exception ex)
            {
                SuiLog.Error("Host shutdown failed: " + ex);
            }
            finally
            {
                if (_module != IntPtr.Zero)
                {
                    FreeLibrary(_module);
                    _module = IntPtr.Zero;
                }
            }
        }

        private static void OnDomainUnload(object sender, EventArgs e) => Shutdown();
        private static void OnProcessExit(object sender, EventArgs e) => Shutdown();
    }
}
