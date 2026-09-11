using System;
using System.IO;
using System.Runtime.InteropServices;

namespace SentinelUI
{
    /// <summary>
    /// Low-level bridge to the native CEF host module (SentinelUI.asi).
    ///
    /// LOAD CONTRACT (2026-09-11): the ASI is loaded at process start by the ASI loader
    /// (ScriptHookV loads root *.asi) — it MUST precede RPH's Present hook; RPH's hook
    /// guard reverts foreign Present hooks installed later (instant crash, see
    /// docs/LESSONS-LEARNED.md §2). In-game the Bootstrap plugin calls
    /// Initialize(..., allowLoad:false) and this class only ATTACHES to the already-loaded
    /// module (GetModuleHandle). LoadLibrary-by-path remains for offline probes/tests
    /// (allowLoad default) and degrades gracefully when the host is absent.
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

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetModuleHandleW(string lpModuleName);

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

        // ---- issue #1 exports ----
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SuiCmdDelegate(int layerId, int value);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SuiCmdFocusDelegate(int layerId, int keyboard, int mouse);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SuiJsDelegate(int layerId, [MarshalAs(UnmanagedType.LPStr)] string utf8);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SuiCaptureDelegate(int enabled);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SuiSinkRegisterDelegate(IntPtr fnPtr);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SuiEventSinkNative([MarshalAs(UnmanagedType.LPStr)] string line);

        // ---- issue #2 exports (layer stack + content scheme) ----
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SuiCreateLayerDelegate([MarshalAs(UnmanagedType.LPStr)] string urlUtf8);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SuiCloseLayerDelegate(int layerId);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SuiRegisterRootDelegate(
            [MarshalAs(UnmanagedType.LPStr)] string pluginUtf8,
            [MarshalAs(UnmanagedType.LPStr)] string rootPathUtf8);

        // Layer-state queries: int(int) — native takes the layer id.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SuiLayerQueryDelegate(int layerId);

        private static SuiCmdDelegate _cmdVisible;
        private static SuiCmdFocusDelegate _cmdFocus;
        private static SuiJsDelegate _execJs;
        private static SuiJsDelegate _cmdNavigate;
        private static SuiCaptureDelegate _setCapture;
        private static SuiGetApiVersionDelegate _isCaptureActive;
        private static SuiSinkRegisterDelegate _registerSink;
        private static SuiEventSinkNative _sinkDelegate;   // kept alive (GC guard)

        private static SuiCreateLayerDelegate _createLayer;
        private static SuiCloseLayerDelegate _closeLayer;
        private static SuiCmdDelegate _setLayerVisible;
        private static SuiLayerQueryDelegate _isLayerVisible;
        private static SuiCmdDelegate _setLayerZ;
        private static SuiLayerQueryDelegate _getLayerZ;
        private static SuiCmdDelegate _setLayerClickThrough;
        private static SuiLayerQueryDelegate _isLayerClickThrough;
        private static SuiCloseLayerDelegate _reloadLayer;
        private static SuiRegisterRootDelegate _registerContentRoot;

        private static IntPtr _module;
        private static bool _ownsModule;   // true only when WE LoadLibrary'd it (probe/tests)
        private static int _apiVersion;
        private static SuiGetApiVersionDelegate _getApiVersion;
        private static SuiPumpEventsDelegate _pumpEvents;
        private static SuiShutdownDelegate _shutdown;
        private static bool _shutdownRequested;

        public static bool IsAvailable => _module != IntPtr.Zero && _apiVersion >= 1 && !_shutdownRequested;
        public static int ApiVersion => _apiVersion;
        public static IntPtr ModuleHandle => _module;

        public static bool Initialize(string hostPath, bool allowLoad = true)
        {
            if (_module != IntPtr.Zero) return true; // already attached

            try
            {
                // Attach-first: in-game the ASI loader (ScriptHookV) owns the module and
                // loaded it at process start, before RPH armed its Present-hook guard.
                // Never LoadLibrary it from here — a hook installed after RPH's guard
                // gets reverted and crashes the D3D chain (2026-09-11, LESSONS §2).
                _module = GetModuleHandleW("SentinelUI.asi");
                if (_module != IntPtr.Zero)
                {
                    _ownsModule = false;
                    SuiLog.Info("Attached to the ASI-loader-hosted SentinelUI.asi (no LoadLibrary).");
                }
                else if (allowLoad)
                {
                    // Offline probes/tests only (host not loaded by an ASI loader).
                    if (string.IsNullOrWhiteSpace(hostPath) || !File.Exists(hostPath))
                    {
                        SuiLog.Warn($"Host module not found at '{hostPath}'. SentinelUI unavailable (graceful degradation).");
                        return false;
                    }
                    _module = LoadLibraryW(Path.GetFullPath(hostPath));
                    if (_module == IntPtr.Zero)
                    {
                        SuiLog.Error($"LoadLibrary failed for '{hostPath}' (Win32 error {Marshal.GetLastWin32Error()}).");
                        return false;
                    }
                    _ownsModule = true;
                }
                else
                {
                    SuiLog.Error("SentinelUI host is not loaded in this process. The ASI loader " +
                                 "(ScriptHookV) must load the root SentinelUI.asi at startup; Bootstrap " +
                                 "attaches to it but never LoadLibrary's it (RPH Present-hook guard). " +
                                 "UI features are disabled for this session.");
                    return false;
                }

                _getApiVersion = LoadExport<SuiGetApiVersionDelegate>("SuiGetApiVersion");
                if (_getApiVersion == null)
                {
                    SuiLog.Error("Host module is missing SuiGetApiVersion export — version mismatch?");
                    Detach();
                    return false;
                }

                _apiVersion = _getApiVersion();
                _pumpEvents = LoadExport<SuiPumpEventsDelegate>("SuiPumpEvents");
                _shutdown = LoadExport<SuiShutdownDelegate>("SuiShutdown");

                // issue #1 exports (additive; absent on older hosts -> features no-op)
                _cmdVisible = LoadExport<SuiCmdDelegate>("SuiCmdVisible");
                _cmdFocus = LoadExport<SuiCmdFocusDelegate>("SuiCmdFocus");
                _execJs = LoadExport<SuiJsDelegate>("SuiExecuteJs");
                _cmdNavigate = LoadExport<SuiJsDelegate>("SuiCmdNavigate");
                _setCapture = LoadExport<SuiCaptureDelegate>("SuiSetCapture");
                _isCaptureActive = LoadExport<SuiGetApiVersionDelegate>("SuiIsCaptureActive");
                _registerSink = LoadExport<SuiSinkRegisterDelegate>("SuiRegisterEventSink");

                // issue #2 exports (additive; absent on older hosts -> layer model unavailable)
                _createLayer = LoadExport<SuiCreateLayerDelegate>("SuiCreateLayer");
                _closeLayer = LoadExport<SuiCloseLayerDelegate>("SuiCloseLayer");
                _setLayerVisible = LoadExport<SuiCmdDelegate>("SuiSetLayerVisible");
                _isLayerVisible = LoadExport<SuiLayerQueryDelegate>("SuiIsLayerVisible");
                _setLayerZ = LoadExport<SuiCmdDelegate>("SuiSetLayerZ");
                _getLayerZ = LoadExport<SuiLayerQueryDelegate>("SuiGetLayerZ");
                _setLayerClickThrough = LoadExport<SuiCmdDelegate>("SuiSetLayerClickThrough");
                _isLayerClickThrough = LoadExport<SuiLayerQueryDelegate>("SuiIsLayerClickThrough");
                _reloadLayer = LoadExport<SuiCloseLayerDelegate>("SuiReloadLayer");
                _registerContentRoot = LoadExport<SuiRegisterRootDelegate>("SuiRegisterContentRoot");

                if (_apiVersion < 1)
                {
                    SuiLog.Error($"Host API version {_apiVersion} is not supported by this API assembly.");
                    Detach();
                    return false;
                }

                // Per-domain lifetime: if this AppDomain (plugin) goes away while the host keeps
                // running, make sure we don't hold anything that would let the host call back into it.
                AppDomain.CurrentDomain.DomainUnload += OnDomainUnload;
                AppDomain.CurrentDomain.ProcessExit += OnProcessExit;

                SuiLog.Info($"Native host ready (API v{_apiVersion}; {(_ownsModule ? "loaded" : "attached")}).");
                return true;
            }
            catch (Exception ex)
            {
                SuiLog.Error("NativeHost.Initialize failed: " + ex);
                return false;
            }
        }

        /// <summary>Releases the module reference. Never FreeLibrary's a module the
        /// ASI loader owns (decrementing a refcount we never incremented would unload
        /// the host under the game process).</summary>
        private static void Detach()
        {
            if (_module == IntPtr.Zero) return;
            if (_ownsModule) FreeLibrary(_module);
            _module = IntPtr.Zero;
            _ownsModule = false;
        }

        private static T LoadExport<T>(string name) where T : Delegate
        {
            IntPtr proc = GetProcAddress(_module, name);
            return proc == IntPtr.Zero ? null : Marshal.GetDelegateForFunctionPointer<T>(proc);
        }

        // ---- issue #1 API (safe optional calls; host v2+ has them) ----
        public static bool SupportsIssue1 =>
            _cmdVisible != null && _execJs != null && _isCaptureActive != null;

        public static void CmdVisible(int layerId, bool visible) =>
            _cmdVisible?.Invoke(layerId, visible ? 1 : 0);

        public static void CmdFocus(int layerId, bool keyboard, bool mouse) =>
            _cmdFocus?.Invoke(layerId, keyboard ? 1 : 0, mouse ? 1 : 0);

        public static void ExecuteJs(int layerId, string jsUtf8) =>
            _execJs?.Invoke(layerId, jsUtf8);

        public static void CmdNavigate(int layerId, string urlUtf8) =>
            _cmdNavigate?.Invoke(layerId, urlUtf8);

        public static bool CaptureActive => _isCaptureActive != null && _isCaptureActive() != 0;

        public static void SetCapture(bool on) =>
            _setCapture?.Invoke(on ? 1 : 0);

        public static void RegisterEventSink(Action<string> handler)
        {
            if (_registerSink == null) return;
            _sinkDelegate = (line) => { try { handler(line); } catch { /* never crash host */ } };
            IntPtr fnPtr = Marshal.GetFunctionPointerForDelegate(_sinkDelegate);
            _registerSink(fnPtr);
        }

        // ---- issue #2 API (layer stack + content scheme; host v3+) ----
        public static bool SupportsLayerModel => _createLayer != null && _closeLayer != null;

        public static int CreateLayer(string url) => _createLayer != null ? _createLayer(url) : 0;

        public static void CloseLayer(int layerId) => _closeLayer?.Invoke(layerId);

        public static void SetLayerVisible(int layerId, bool visible) =>
            _setLayerVisible?.Invoke(layerId, visible ? 1 : 0);

        public static bool IsLayerVisible(int layerId) =>
            _isLayerVisible != null && _isLayerVisible(layerId) != 0;

        public static void SetLayerZ(int layerId, int z) => _setLayerZ?.Invoke(layerId, z);

        public static int GetLayerZ(int layerId) => _getLayerZ != null ? _getLayerZ(layerId) : 0;

        public static void SetLayerClickThrough(int layerId, bool enabled) =>
            _setLayerClickThrough?.Invoke(layerId, enabled ? 1 : 0);

        public static bool IsLayerClickThrough(int layerId) =>
            _isLayerClickThrough != null && _isLayerClickThrough(layerId) != 0;

        public static void ReloadLayer(int layerId) => _reloadLayer?.Invoke(layerId);

        public static bool RegisterContentRoot(string plugin, string rootPath) =>
            _registerContentRoot != null && _registerContentRoot(plugin, rootPath) != 0;

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
                Detach();
            }
        }

        private static void OnDomainUnload(object sender, EventArgs e) => Shutdown();
        private static void OnProcessExit(object sender, EventArgs e) => Shutdown();
    }
}
