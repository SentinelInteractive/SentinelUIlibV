#include "helper_app.h"
#include "ipc.h"
#include "scheme.h"
#include "suilog.h"

#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "include/cef_parser.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>

namespace sui_helper {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

LayerClient::LayerClient(int layerId)
    : layerId_(layerId) {}

LayerClient::~LayerClient() {
    shm_.Close();
}

bool LayerClient::OpenShm() {
    if (shm_.Header()) return true;
    const std::wstring& base = LayerManager::Instance().ShmBaseName();
    std::wstring name = layerId_ == 1 ? base : base + L"_L" + std::to_wstring(layerId_);
    if (!shm_.Open(name.c_str(), kMaxFrameW, kMaxFrameH)) {
        SUI_LOG_ERROR("layer %d: shm open failed (%ls).", layerId_, name.c_str());
        return false;
    }
    SUI_LOG_INFO("layer %d shm ready (%ls).", layerId_, name.c_str());
    return true;
}

void LayerClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    browser_ = browser;
}

void LayerClient::OnBeforeClose(CefRefPtr<CefBrowser>) {
    LayerManager::Instance().OnLayerBrowserClosed(layerId_);
}

bool LayerClient::OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t,
                                   const CefString& message, const CefString&, int) {
    // JS->host channel: log locally + forward to the ASI (layer-tagged).
    std::string msg = message.ToString();
    LogConsole(msg);
    SendEvent("console", msg);
    return true;
}

void LayerClient::OnLoadingStateChange(CefRefPtr<CefBrowser>, bool isLoading, bool, bool) {
    LogConsole(isLoading ? std::string("[load] loading") : std::string("[load] done"));
    SendEvent("load", isLoading ? "loading" : "done");
    if (!isLoading) {
        // Page committed: queued JS (incl. the software cursor) may now run.
        jsReady_ = true;
        loadRetries_ = 0;          // successful load clears the retry budget
        retryPending_ = false;
        // Inject the page scripts once loaded: protocol bridge first (page
        // scripts may use window.sui immediately), then the cursor/badge, then
        // the current capture state.
        QueueJs(kSuiJs);
        QueueJs(kCursorJs);
        QueueJs(captureOn_ ? kCaptureStateJsOn : kCaptureStateJsOff);
    }
}

void LayerClient::OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, ErrorCode code,
                              const CefString& text, const CefString& url) {
    std::string body = text.ToString() + " url=" + url.ToString();
    LogConsole("[load-error] " + body);
    SendEvent("load-error", body);
    // Error pages still provide a runnable frame context; don't wedge queued JS.
    jsReady_ = true;

    // First-navigation race insurance: retry sentinelui:// loads that fail with
    // ERR_UNKNOWN_URL_SCHEME (child-process scheme registration not live yet).
    std::string urlStr = url.ToString();
    if (code == ERR_UNKNOWN_URL_SCHEME &&
        urlStr.compare(0, 13, "sentinelui://") == 0 &&
        loadRetries_ < kMaxLoadRetries) {
        loadRetries_++;
        retryPending_ = true;
        retryAtTick_ = GetTickCount64() + 400;
        retryUrl_ = urlStr;
        SUI_LOG_WARN("layer %d: ERR_UNKNOWN_URL_SCHEME; retry %d/%d in 400ms.",
                     layerId_, loadRetries_, kMaxLoadRetries);
    }
}

void LayerClient::LogConsole(const std::string& msg) {
    SUI_LOG_INFO("layer %d: %s", layerId_, msg.c_str());
}

void LayerClient::SendEvent(const std::string& kind, const std::string& body) {
    IpcSendEvent(std::to_string(layerId_) + "|" + kind + ":" + body);
}

void LayerClient::SetVisible(bool visible) {
    visible_ = visible;
    ApplyVisibility();
}

void LayerClient::SetMasterVisible(bool visible) {
    masterVisible_ = visible;
    ApplyVisibility();
}

void LayerClient::SetCaptureIndicator(bool on) {
    captureOn_ = on;
    // Only queue the state when the page already has the injected script. Before
    // load commit, OnLoadingStateChange queues kCursorJs + the current state in
    // the right order itself.
    if (jsReady_) QueueJs(on ? kCaptureStateJsOn : kCaptureStateJsOff);
}

void LayerClient::ApplyVisibility() {
    if (!browser_) return;
    bool effective = visible_ && masterVisible_;
    browser_->GetHost()->WasHidden(!effective);
    if (effective) browser_->GetHost()->Invalidate(PET_VIEW);
}

void LayerClient::SetFocused(bool keyboard, bool mouse) {
    if (!browser_) return;
    browser_->GetHost()->SetFocus(keyboard);
    (void)mouse;
}

void LayerClient::SetViewport(int width, int height) {
    if (width <= 0 || height <= 0) return;
    viewWidth_ = width;
    viewHeight_ = height;
    SUI_LOG_INFO("layer %d viewport -> %dx%d", layerId_, width, height);
    if (browser_) {
        browser_->GetHost()->WasResized();
        browser_->GetHost()->NotifyMoveOrResizeStarted();
    }
}

void LayerClient::Navigate(const std::string& urlUtf8) {
    if (!browser_) return;
    jsReady_ = false;   // queued JS waits for the next commit
    browser_->GetMainFrame()->LoadURL(urlUtf8);
}

void LayerClient::Reload() {
    if (!browser_) return;
    jsReady_ = false;
    browser_->Reload();
}

void LayerClient::ExecuteJs(const std::string& javascript) {
    if (!browser_) return;
    browser_->GetMainFrame()->ExecuteJavaScript(javascript, "sentinelui://", 0);
}

// Protocol bridge injected into every page: window.sui with
//   sui.send(type, payload)           fire-and-forget event to C#/plugins
//   sui.on(type, handler)             receive events/replies (returns unsubscribe)
//   sui.request(type, payload[, ms])  Promise request -> SuiLayer.Request/Reply
// JS->C# travels as console lines prefixed "SUI:" (JSON envelope v1); C# -> JS
// is delivered through window.sui._deliver (ExecuteJs), which also re-posts the
// envelope on window.postMessage for pages that prefer raw listeners.
const char* kSuiJs =
    "(function(){"
    "if(window.sui&&window.sui._v===1)return;"
    "var seq=0,pending={},listeners={};"
    "function post(m){try{console.log('SUI:'+JSON.stringify(m));}catch(e){}}"
    "function emit(type,payload,env){"
    "var ls=listeners[type]||[];for(var i=0;i<ls.length;i++){try{ls[i](payload,env);}catch(e){}}"
    "var all=listeners['*']||[];for(var j=0;j<all.length;j++){try{all[j](payload,env);}catch(e){}}"
    "}"
    "function deliver(env){"
    "try{window.postMessage(env,'*');}catch(e){}"
    "if(!env)return;"
    "if(env.kind==='res'&&env.id!=null){"
    "var p=pending[env.id];if(!p)return;delete pending[env.id];"
    "if(env.ok===false)p.reject(env.payload);else p.resolve(env.payload);"
    "return;"
    "}"
    "if(env.type)emit(env.type,env.payload,env);"
    "}"
    "window.sui={_v:1,"
    "send:function(type,payload){post({v:1,kind:'evt',type:String(type),payload:payload});},"
    "on:function(type,fn){var t=String(type);(listeners[t]=listeners[t]||[]).push(fn);"
    "return function(){var a=listeners[t];if(!a)return;var i=a.indexOf(fn);if(i>=0)a.splice(i,1);};},"
    "request:function(type,payload,timeoutMs){return new Promise(function(resolve,reject){"
    "var id=++seq;pending[id]={resolve:resolve,reject:reject};"
    "post({v:1,kind:'req',type:String(type),id:id,payload:payload});"
    "if(timeoutMs)setTimeout(function(){if(pending[id]){delete pending[id];"
    "reject('sui request timeout: '+type);}},timeoutMs);});},"
    "_deliver:deliver};"
    "})();";

// Software cursor: GTA V hides the OS cursor and OSR draws none, so the page
// renders its own arrow. Visible only while input capture is on
// (window.__suiSetCapture); a plain white/black arrow with a drop shadow replaces
// the old mix-blend-mode:difference one (it inverted whatever was under it).
// Moves via translate3d for smoothness and scales slightly on press. The same
// injected script carries the input-capture badge. Deliberately no CSS `cursor:`
// property: OSR does not draw one.
const char* kCursorJs =
    "(function(){"
    "if(!document.getElementById('sui-cursor')){"
    "var c=document.createElement('div');"
    "c.id='sui-cursor';"
    "c.style.cssText='position:fixed;left:0;top:0;width:22px;height:22px;pointer-events:none;"
    "z-index:2147483647;display:none;transform:translate3d(-100px,-100px,0);"
    "will-change:transform;transform-origin:1px 1px;"
    "filter:drop-shadow(0 1.5px 2px rgba(0,0,0,.55));';"
    "c.innerHTML='<svg width=\"22\" height=\"22\" viewBox=\"0 0 22 22\">"
    "<path d=\"M1 1 L1 17.6 L5.6 13.4 L8.5 20 L11.5 18.6 L8.6 12.1 L14.6 11.7 Z\" "
    "fill=\"#ffffff\" stroke=\"#141414\" stroke-width=\"1.5\" "
    "stroke-linejoin=\"round\" stroke-linecap=\"round\"/></svg>';"
    "document.body.appendChild(c);"
    "var x=-100,y=-100,pressed=false;"
    "var apply=function(){c.style.transform='translate3d('+x+'px,'+y+'px,0)'+(pressed?' scale(.86)':'');};"
    "document.addEventListener('mousemove',function(e){x=e.clientX;y=e.clientY;apply();},true);"
    "document.addEventListener('mousedown',function(){pressed=true;apply();},true);"
    "document.addEventListener('mouseup',function(){pressed=false;apply();},true);"
    "}"
    "if(!document.getElementById('sui-capture')){"
    "var b=document.createElement('div');"
    "b.id='sui-capture';"
    "b.style.cssText='position:fixed;right:12px;top:10px;padding:4px 10px;border-radius:4px;"
    "background:rgba(217,37,52,.88);color:#fff;font:600 11px/1.6 Consolas,monospace;"
    "letter-spacing:.08em;pointer-events:none;z-index:2147483647;display:none;';"
    "b.textContent='INPUT CAPTURE \\u2022 F10 RELEASE';"
    "document.body.appendChild(b);"
    "}"
    "window.__suiSetCapture=function(on){"
    "var c=document.getElementById('sui-cursor');if(c)c.style.display=on?'block':'none';"
    "var b=document.getElementById('sui-capture');if(b)b.style.display=on?'block':'none';"
    "};"
    "})();";

const char* kCaptureStateJsOn =
    "window.__suiSetCapture&&window.__suiSetCapture(true);";
const char* kCaptureStateJsOff =
    "window.__suiSetCapture&&window.__suiSetCapture(false);";

void LayerClient::QueueJs(const std::string& javascript) {
    std::lock_guard<std::mutex> lock(jsMutex_);
    jsQueue_.push_back(javascript);
}

void LayerClient::FlushJsQueue() {
    if (!browser_ || !jsReady_) return;   // queued JS waits until the page committed
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lock(jsMutex_);
        if (jsQueue_.empty()) return;
        batch.swap(jsQueue_);
    }
    for (const auto& js : batch) ExecuteJs(js);
}

void LayerClient::Tick() {
    if (!retryPending_ || !browser_) return;
    if (GetTickCount64() < retryAtTick_) return;
    retryPending_ = false;
    jsReady_ = false;
    SUI_LOG_INFO("layer %d: retrying navigation to %s", layerId_, retryUrl_.c_str());
    browser_->GetMainFrame()->LoadURL(retryUrl_);
}

void LayerClient::DispatchKey(bool down, int vk, int scancode, bool extended, int modifiers) {
    if (!browser_) return;
    CefKeyEvent ev;
    ev.type = down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
    ev.windows_key_code = vk;
    ev.native_key_code = (extended ? 0x100 : 0) | (scancode << 16);
    ev.is_system_key = false;
    // Host modifier bits (1 shift, 2 ctrl, 4 alt) -> CEF event flags.
    int cefMods = 0;
    if (modifiers & 1) cefMods |= EVENTFLAG_SHIFT_DOWN;
    if (modifiers & 2) cefMods |= EVENTFLAG_CONTROL_DOWN;
    if (modifiers & 4) cefMods |= EVENTFLAG_ALT_DOWN;
    ev.modifiers = cefMods;
    browser_->GetHost()->SendKeyEvent(ev);
}

void LayerClient::DispatchChar(int codePoint) {
    if (!browser_) return;
    CefKeyEvent ev;
    ev.type = KEYEVENT_CHAR;
    ev.windows_key_code = codePoint;
    ev.native_key_code = 0;
    browser_->GetHost()->SendKeyEvent(ev);
}

void LayerClient::DispatchMouseMove(int x, int y) {
    if (!browser_) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    browser_->GetHost()->SendMouseMoveEvent(ev, false);
}

void LayerClient::DispatchMouseButton(int button, bool down, int x, int y) {
    if (!browser_) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    CefBrowserHost::MouseButtonType type = MBT_LEFT;
    if (button == 1) type = MBT_MIDDLE;
    if (button == 2) type = MBT_RIGHT;
    // CEF's third parameter is |mouseUp|, not mouseDown. Passing the raw down flag
    // inverted every click (up before down), so Chromium only completed a button
    // press on every second click (the "buttons are buggy" field report 2026-09-11).
    browser_->GetHost()->SendMouseClickEvent(ev, type, /*mouseUp=*/!down, 1);
}

void LayerClient::DispatchMouseWheel(int delta, int x, int y) {
    if (!browser_) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    browser_->GetHost()->SendMouseWheelEvent(ev, 0, delta);
}

LayerManager& LayerManager::Instance() {
    static LayerManager inst;
    return inst;
}

bool LayerManager::CreateLayerNow(int layerId, const std::string& urlUtf8) {
    CEF_REQUIRE_UI_THREAD();
    if (layers_.count(layerId)) return true;
    auto client = CefRefPtr<LayerClient>(new LayerClient(layerId));
    // A layer created after the game viewport is known must start at that size,
    // otherwise it renders at 1280x720 and gets upscaled (pixelated).
    client->SetInitialViewport(lastViewportW_, lastViewportH_);
    if (!client->OpenShm()) return false;

    CefWindowInfo info;
    info.SetAsWindowless(kNullWindowHandle);
    CefBrowserSettings browserSettings;
    browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);

    CefRefPtr<CefBrowser> browser =
        CefBrowserHost::CreateBrowserSync(info, client.get(), urlUtf8, browserSettings, nullptr, nullptr);
    if (!browser) {
        SUI_LOG_ERROR("layer %d: CreateBrowserSync failed.", layerId);
        return false;
    }
    // OnAfterCreated stored the browser; keep the client referenced in the map.
    layers_[layerId] = client;
    client->SetMasterVisible(masterVisible_);
    client->SetCaptureIndicator(captureOn_);
    if (focusedId_ == 0) focusedId_ = layerId;
    SUI_LOG_INFO("layer %d browser created: %s", layerId, urlUtf8.c_str());
    return true;
}

void LayerManager::CreateLayer(int layerId, const std::string& urlUtf8) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId, urlUtf8]() {
        if (layers_.count(layerId)) {
            SUI_LOG_WARN("layer %d already exists; create ignored.", layerId);
            return;
        }
        CreateLayerNow(layerId, urlUtf8);
    });
}

void LayerManager::CloseLayer(int layerId) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId]() {
        auto it = layers_.find(layerId);
        if (it == layers_.end()) return;
        if (focusedId_ == layerId) focusedId_ = 0;
        it->second->browser()->GetHost()->CloseBrowser(true);
        SUI_LOG_INFO("layer %d close requested.", layerId);
    });
}

void LayerManager::SetVisible(int layerId, bool visible) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId, visible]() {
        auto it = layers_.find(layerId);
        if (it != layers_.end()) it->second->SetVisible(visible);
    });
}

void LayerManager::SetMasterVisible(bool visible) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, visible]() {
        masterVisible_ = visible;
        for (auto& kv : layers_) kv.second->SetMasterVisible(visible);
    });
}

void LayerManager::SetFocus(int layerId, bool keyboard, bool mouse) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId, keyboard, mouse]() {
        if (focusedId_ != 0 && focusedId_ != layerId) {
            auto prev = layers_.find(focusedId_);
            if (prev != layers_.end()) prev->second->SetFocused(false, false);
        }
        focusedId_ = keyboard ? layerId : 0;
        auto it = layers_.find(layerId);
        if (it != layers_.end()) it->second->SetFocused(keyboard, mouse);
    });
}

void LayerManager::ExecuteJs(int layerId, const std::string& js) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId, js]() {
        auto it = layers_.find(layerId);
        if (it != layers_.end()) it->second->QueueJs(js);
    });
}

void LayerManager::Navigate(int layerId, const std::string& urlUtf8) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId, urlUtf8]() {
        auto it = layers_.find(layerId);
        if (it != layers_.end()) it->second->Navigate(urlUtf8);
    });
}

void LayerManager::Reload(int layerId) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId]() {
        auto it = layers_.find(layerId);
        if (it != layers_.end()) it->second->Reload();
    });
}

void LayerManager::SetViewportAll(int width, int height) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, width, height]() {
        lastViewportW_ = width;
        lastViewportH_ = height;
        for (auto& kv : layers_) kv.second->SetViewport(width, height);
    });
}

void LayerManager::DispatchKey(int layerId, bool down, int vk, int scancode, bool extended,
                               int modifiers) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId, down, vk, scancode, extended, modifiers]() {
        auto it = layers_.find(layerId);
        if (it != layers_.end()) it->second->DispatchKey(down, vk, scancode, extended, modifiers);
    });
}

void LayerManager::DispatchChar(int layerId, int codePoint) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId, codePoint]() {
        auto it = layers_.find(layerId);
        if (it != layers_.end()) it->second->DispatchChar(codePoint);
    });
}

void LayerManager::DispatchMouseMove(int layerId, int x, int y) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId, x, y]() {
        auto it = layers_.find(layerId);
        if (it != layers_.end()) it->second->DispatchMouseMove(x, y);
    });
}

void LayerManager::DispatchMouseButton(int layerId, int button, bool down, int x, int y) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId, button, down, x, y]() {
        auto it = layers_.find(layerId);
        if (it != layers_.end()) it->second->DispatchMouseButton(button, down, x, y);
    });
}

void LayerManager::DispatchMouseWheel(int layerId, int delta, int x, int y) {
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, layerId, delta, x, y]() {
        auto it = layers_.find(layerId);
        if (it != layers_.end()) it->second->DispatchMouseWheel(delta, x, y);
    });
}

void LayerManager::SetCaptureIndicator(bool on) {
    captureOn_ = on;
    std::lock_guard<std::mutex> lock(opsMutex_);
    ops_.push_back([this, on]() {
        for (auto& kv : layers_) kv.second->SetCaptureIndicator(on);
    });
}

void LayerManager::PumpOps() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(opsMutex_);
        if (ops_.empty()) return;
        batch.swap(ops_);
    }
    for (auto& op : batch) op();
}

void LayerManager::FlushJsQueues() {
    for (auto& kv : layers_) kv.second->FlushJsQueue();
}

void LayerManager::Tick() {
    for (auto& kv : layers_) kv.second->Tick();
}

void LayerManager::CloseAll() {
    for (auto& kv : layers_) kv.second->browser()->GetHost()->CloseBrowser(true);
}

bool LayerManager::AnyBrowserAlive() const {
    return !layers_.empty();
}

LayerClient* LayerManager::Get(int layerId) const {
    auto it = layers_.find(layerId);
    return it == layers_.end() ? nullptr : it->second.get();
}

void LayerManager::OnLayerBrowserClosed(int layerId) {
    auto it = layers_.find(layerId);
    if (it == layers_.end()) return;
    if (focusedId_ == layerId) focusedId_ = 0;
    layers_.erase(it);   // last LayerClient reference drops; shm released
    SUI_LOG_INFO("layer %d closed.", layerId);
}

// IPC command dispatch (reader thread -> LayerManager queue)

namespace {
// Parses "<digits> <rest>" starting at pos; returns false when malformed.
bool ParseIdThenBody(const std::string& line, size_t pos, int& id, size_t& bodyPos) {
    size_t p = pos;
    while (p < line.size() && isdigit((unsigned char)line[p])) p++;
    if (p == pos || p >= line.size() || line[p] != ' ') return false;
    id = atoi(line.substr(pos, p - pos).c_str());
    bodyPos = p + 1;
    return true;
}

bool ParseLenBody(const std::string& line, size_t pos, std::string& out) {
    size_t colon = line.find(':', pos);
    if (colon == std::string::npos) return false;
    int len = atoi(line.substr(pos, colon - pos).c_str());
    if (len < 0 || colon + 1 + (size_t)len > line.size()) return false;
    out = line.substr(colon + 1, (size_t)len);
    return true;
}

LayerManager& LM() { return LayerManager::Instance(); }

void OnIpcCommand(const std::string& line) {
    if (line.empty()) return;
    // One-shot per command type so the command path is visible in logs without
    // spamming the hot M path.
    static bool s_seen[256] = {};
    unsigned char c = (unsigned char)line[0];
    if (!s_seen[c]) {
        s_seen[c] = true;
        SUI_LOG_INFO("cmd recv: first '%c' (%u bytes)", (char)c, (unsigned)line.size());
    }

    // Legacy forms (no layer id) target layer 1.
    const int kBase = 1;
    int id = kBase;
    size_t pos = 1;
    switch (line[0]) {
        case 'C': {  // C<id> <len>:<url> (create a layer browser)
            // Bare "C1"/"C0" is the input-capture badge toggle (SetCaptureState);
            // the create form always has a space after the id.
            if (line.size() == 2 && (line[1] == '0' || line[1] == '1')) {
                LM().SetCaptureIndicator(line[1] == '1');
                break;
            }
            std::string url;
            if (ParseIdThenBody(line, 1, id, pos) && ParseLenBody(line, pos, url))
                LM().CreateLayer(id, url);
            break;
        }
        case 'X': {  // X<id> (close a layer)
            int xid = atoi(line.c_str() + 1);
            LM().CloseLayer(xid > 0 ? xid : kBase);
            break;
        }
        case 'S': case 'H': {
            // S<id> / H<id> (legacy: bare "S"/"H" -> layer 1)
            if (line.size() > 1) {
                int sid = atoi(line.c_str() + 1);
                if (sid > 0) id = sid;
            }
            LM().SetVisible(id, line[0] == 'S');
            break;
        }
        case 'F': {  // F<id><kb><mouse> (legacy: F<kb><mouse> -> layer 1)
            if (line.size() > 1 && isdigit((unsigned char)line[1])) {
                size_t p = 1;
                while (p < line.size() && isdigit((unsigned char)line[p])) p++;
                id = atoi(line.substr(1, p - 1).c_str());
                pos = p;
            }
            bool k = line.size() > pos && line[pos] == '1';
            bool m = line.size() > pos + 1 && line[pos + 1] == '1';
            LM().SetFocus(id, k, m);
            break;
        }
        case 'J': case 'N': {  // J<id> <len>:<js> / N<id> <len>:<url> (legacy: J<len>:<js>)
            std::string body;
            if (isdigit((unsigned char)line[1])) {
                if (!ParseIdThenBody(line, 1, id, pos) || !ParseLenBody(line, pos, body)) return;
            } else if (!ParseLenBody(line, 1, body)) {
                return;
            }
            if (line[0] == 'J') LM().ExecuteJs(id, body);
            else LM().Navigate(id, body);
            break;
        }
        case 'L': {  // L<id> (hot reload)
            int lid = atoi(line.c_str() + 1);
            LM().Reload(lid > 0 ? lid : kBase);
            break;
        }
        case 'A': {  // A1/A0 (master show/hide, F9 pause/resume all pages)
            bool visible = line.size() > 1 && line[1] == '1';
            LM().SetMasterVisible(visible);
            break;
        }
        case 'R': {  // R<len>:<plugin>|<path> (register a content root)
            std::string body;
            if (ParseLenBody(line, 1, body)) Roots().Register(body);
            break;
        }
        case 'V': {  // V<w> <h> (global viewport sync, all layers)
            int w = 0, h = 0;
            if (sscanf_s(line.c_str() + 1, "%d %d", &w, &h) == 2) LM().SetViewportAll(w, h);
            break;
        }
        case 'K': {  // K<id> <down> <vk> <sc> <ext> [mods]
            int d = 0, vk = 0, sc = 0, ext = 0, mods = 0;
            int n = sscanf_s(line.c_str() + 1, "%d %d %d %d %d %d", &id, &d, &vk, &sc, &ext, &mods);
            if (n >= 5) LM().DispatchKey(id, d != 0, vk, sc, ext != 0, mods);
            break;
        }
        case 'U': {  // U<id> <char>
            int cp = 0;
            if (sscanf_s(line.c_str() + 1, "%d %d", &id, &cp) == 2) LM().DispatchChar(id, cp);
            break;
        }
        case 'M': {  // M<id> <x> <y>
            int x = 0, y = 0;
            if (sscanf_s(line.c_str() + 1, "%d %d %d", &id, &x, &y) == 3)
                LM().DispatchMouseMove(id, x, y);
            break;
        }
        case 'B': {  // B<id> <button> <down> <x> <y>
            int b = 0, d = 0, x = 0, y = 0;
            if (sscanf_s(line.c_str() + 1, "%d %d %d %d %d", &id, &b, &d, &x, &y) == 5)
                LM().DispatchMouseButton(id, b, d != 0, x, y);
            break;
        }
        case 'W': {  // W<id> <delta> <x> <y>
            int dd = 0, x = 0, y = 0;
            if (sscanf_s(line.c_str() + 1, "%d %d %d %d", &id, &dd, &x, &y) == 4)
                LM().DispatchMouseWheel(id, dd, x, y);
            break;
        }
        case 'Q': StopIpc(); break;
        default: break;
    }
}
}  // namespace

void SetupIpcCommands() { SetCommandHandler(&OnIpcCommand); }

// SuiHelperApp: browser-process scheme registration

void SuiHelperApp::OnContextInitialized() {
    CEF_REQUIRE_UI_THREAD();
    // Factory registration is callable from any thread in the browser process
    // and applies to the global request context. RunBrowserProcess also calls
    // this before creating the first browser (idempotent).
    RegisterSentinelUiScheme();
}

// Browser-process run loop

int RunBrowserProcess(const HelperConfig& cfg) {
    std::string pageUrlUtf8 = ToUtf8(cfg.contentFile);
    if (pageUrlUtf8.find("://") == std::string::npos) {
        // Plain filesystem path (selftest/harness compatibility): file:// URL.
        std::wstring pageUrl = L"file:///" + cfg.contentFile;
        std::string::size_type p = 0;
        while ((p = pageUrl.find(L'\\', p)) != std::wstring::npos) pageUrl.replace(p, 1, L"/");
        pageUrlUtf8 = ToUtf8(pageUrl);
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exePathW(exePath);

    CefMainArgs mainArgs(GetModuleHandleW(nullptr));
    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.log_severity = LOGSEVERITY_WARNING;
    CefString(&settings.root_cache_path) = ToUtf8(cfg.cacheDir);
    CefString(&settings.browser_subprocess_path) = ToUtf8(exePathW);   // self-spawn (EXE)

    CefRefPtr<SuiHelperApp> app(new SuiHelperApp());

    if (!CefInitialize(mainArgs, settings, app.get(), nullptr)) {
        SUI_LOG_ERROR("CefInitialize failed.");
        return 1;
    }
    SUI_LOG_INFO("CefInitialize OK w/ view %dx%d; boot layer: %s", cfg.viewWidth, cfg.viewHeight,
                 pageUrlUtf8.c_str());

    // Register the scheme factory before ANY browser exists (also done from
    // OnContextInitialized on the IO thread; this call removes all scheduling
    // doubt). Then give CEF a brief settle window so child processes (network
    // service/utility) complete their own scheme registration before the first
    // sentinelui:// navigation. Without this, a busy in-game boot can race the
    // first request and fail with ERR_UNKNOWN_URL_SCHEME.
    RegisterSentinelUiScheme();
    for (int i = 0; i < 40; ++i) {   // ~400ms
        CefDoMessageLoopWork();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    LayerManager& lm = LayerManager::Instance();
    lm.SetShmBaseName(cfg.shmName);

    // Boot layer (id 1): create its browser + shm right here on the UI thread.
    if (!lm.CreateLayerNow(1, pageUrlUtf8)) {
        SUI_LOG_ERROR("Boot layer creation failed.");
        CefShutdown();
        return 2;
    }
    FrameShm* bootShm = lm.Get(1)->shm();

    // Pump loop (dedicated-thread CefDoMessageLoopWork, harness-proven).
    unsigned __int64 endTick = cfg.timeoutSeconds ? GetTickCount64() + (unsigned __int64)cfg.timeoutSeconds * 1000 : 0;
    bool quitting = false;
    uint64_t ipcDownSince = 0;
    while (!quitting) {
        CefDoMessageLoopWork();
        lm.PumpOps();
        lm.FlushJsQueues();
        lm.Tick();
        // Orphan guard: the parent-pid watchdog exits immediately when the game
        // process dies (kill/crash); the pipe-down timer stays as a fallback for
        // hosts that spawned us without --parent-pid.
        if (!cfg.selftest) {
            if (cfg.parentPid && ParentProcessGone()) {
                quitting = true;
            } else if (IpcConnected()) {
                ipcDownSince = 0;
            } else {
                if (!ipcDownSince) ipcDownSince = GetTickCount64();
                else if (GetTickCount64() - ipcDownSince > 30000) {
                    SUI_LOG_WARN("IPC down for 30s (host gone?); shutting down.");
                    quitting = true;
                }
            }
        }
        if (cfg.timeoutSeconds && GetTickCount64() >= endTick) quitting = true;
        if (cfg.selftest && lm.Get(1) && lm.Get(1)->paintCount >= cfg.selftestPaints) quitting = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // Self-test: verify the boot layer's shared-memory frame contains content.
    if (cfg.selftest) {
        const FrameHeader* h = bootShm->Header();
        const uint8_t* px = bootShm->Pixels();
        if (!h || h->state == 0 || h->generation == 0 || !px) {
            SUI_LOG_ERROR("SELFTEST FAIL: no frame published.");
            lm.CloseAll();
            CefShutdown();
            return 3;
        }
        auto alphaAt = [&](int x, int y) {
            size_t off = (size_t)y * h->width * 4 + (size_t)x * 4;
            return (int)px[off + 3];
        };
        int aBg = alphaAt(10, 10);
        int aTile = alphaAt(80, 60);
        SUI_LOG_INFO("SELFTEST: gen=%llu %ux%u alpha(10,10)=%d alpha(80,60)=%d",
                     (unsigned long long)h->generation, h->width, h->height, aBg, aTile);
        bool ok = (h->generation > 0) && ((int)h->width == cfg.viewWidth) && (aBg < 40) && (aTile > 100);
        SUI_LOG_INFO(ok ? "SELFTEST PASS" : "SELFTEST FAIL");
        lm.CloseAll();
        CefShutdown();
        return ok ? 0 : 4;
    }

    SUI_LOG_INFO("Helper running (timeout=%d).", cfg.timeoutSeconds);

    // Graceful shutdown: close all layer browsers, pump until they're gone
    // (OnBeforeClose erases them), then CefShutdown.
    lm.CloseAll();
    for (int i = 0; i < 600 && lm.AnyBrowserAlive(); ++i) {
        CefDoMessageLoopWork();
        lm.PumpOps();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    if (lm.AnyBrowserAlive()) SUI_LOG_WARN("browsers still alive at CefShutdown.");
    CefShutdown();
    return 0;
}

}  // namespace sui_helper
