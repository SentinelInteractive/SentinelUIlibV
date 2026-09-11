// SentinelUI Helper — sentinelui:// scheme implementation (issue #2).

#include "scheme.h"
#include "suilog.h"

#include "include/cef_request.h"
#include "include/cef_response.h"
#include "include/cef_parser.h"
#include "include/cef_urlrequest.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_closure_task.h"

#include <windows.h>

#include <algorithm>
#include <set>

namespace sui_helper {

// ---------------------------------------------------------------------------
// Content roots
// ---------------------------------------------------------------------------

ContentRoots& Roots() {
    static ContentRoots inst;
    return inst;
}

void ContentRoots::SetDefaultRoot(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(mu_);
    roots_["sui"] = dir;
}

bool ContentRoots::Register(const std::string& body) {
    size_t bar = body.find('|');
    if (bar == std::string::npos || bar == 0 || bar + 1 >= body.size()) return false;
    std::string plugin = body.substr(0, bar);
    std::string pathUtf8 = body.substr(bar + 1);

    // Plugin name: [A-Za-z0-9_-]+ only (it becomes a URL host component).
    for (char c : plugin) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    if (plugin.size() > 64) return false;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), (int)pathUtf8.size(), nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring path(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), (int)pathUtf8.size(), &path[0], wlen);

    // Absolute, existing directory only.
    wchar_t full[_MAX_PATH + 2];
    if (path.size() < 3 || path[1] != L':' ||
        _wfullpath(full, path.c_str(), _MAX_PATH + 1) == nullptr) {
        SUI_LOG_WARN("content root rejected (not absolute): %s", pathUtf8.c_str());
        return false;
    }
    std::wstring fullDir(full);
    DWORD attrs = GetFileAttributesW(fullDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        SUI_LOG_WARN("content root rejected (not a directory): %s", pathUtf8.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        roots_[plugin] = fullDir;
    }
    SUI_LOG_INFO("content root: sentinelui://%s/ -> %ls", plugin.c_str(), fullDir.c_str());
    return true;
}

namespace {

// Normalizes the URL path into root-relative segments; false on violation.
bool SafeRelPath(const std::string& urlPath, std::wstring& rel) {
    if (urlPath.empty() || urlPath[0] != '/') return false;
    if (urlPath.find('%') != std::string::npos) return false;  // no percent-encoding games

    std::vector<std::string> out;
    size_t start = 1;
    while (start <= urlPath.size()) {
        size_t end = urlPath.find('/', start);
        std::string seg = urlPath.substr(start, (end == std::string::npos ? urlPath.size() : end) - start);
        if (end == std::string::npos) start = urlPath.size() + 1;
        else start = end + 1;
        if (seg.empty() || seg == "." || seg == "..") return false;  // traversal refused
        for (char c : seg) {
            unsigned char u = (unsigned char)c;
            if (u < 0x20 || c == ':' || c == '\\' || c == '"' || c == '|' ||
                c == '?' || c == '*' || c == '<' || c == '>') {
                return false;  // Windows-reserved / control characters
            }
        }
        out.push_back(seg);
    }
    if (out.empty()) return false;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, out[0].c_str(), (int)out[0].size(), nullptr, 0);
    if (wlen <= 0) return false;
    rel = std::wstring(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, out[0].c_str(), (int)out[0].size(), &rel[0], wlen);
    for (size_t i = 1; i < out.size(); ++i) {
        rel += L"\\";
        int n = MultiByteToWideChar(CP_UTF8, 0, out[i].c_str(), (int)out[i].size(), nullptr, 0);
        if (n <= 0) return false;
        std::wstring part(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, out[i].c_str(), (int)out[i].size(), &part[0], n);
        rel += part;
    }
    return true;
}

std::string MimeForExtension(const std::wstring& path) {
    // NOTE: bare MIME types only — CefResponse::SetMimeType writes Content-Type
    // directly and a "type/subtype; charset=..." suffix yields an unparseable
    // type, which Chromium then renders as plain text (source view).
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return "application/octet-stream";
    std::wstring ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext == L"html" || ext == L"htm") return "text/html";
    if (ext == L"css") return "text/css";
    if (ext == L"js" || ext == L"mjs") return "text/javascript";
    if (ext == L"json") return "application/json";
    if (ext == L"png") return "image/png";
    if (ext == L"jpg" || ext == L"jpeg") return "image/jpeg";
    if (ext == L"gif") return "image/gif";
    if (ext == L"svg") return "image/svg+xml";
    if (ext == L"webp") return "image/webp";
    if (ext == L"ico") return "image/x-icon";
    if (ext == L"woff") return "font/woff";
    if (ext == L"woff2") return "font/woff2";
    if (ext == L"ttf") return "font/ttf";
    if (ext == L"txt") return "text/plain";
    if (ext == L"mp3") return "audio/mpeg";
    if (ext == L"ogg") return "audio/ogg";
    return "application/octet-stream";
}

// CSP applied to every HTML response (issue #2: CSP-tight content).
const char kContentCsp[] =
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
    "img-src 'self' data:; font-src 'self'; connect-src 'none'; "
    "object-src 'none'; frame-src 'none'; form-action 'none'";

}  // namespace

bool ContentRoots::Resolve(const std::string& pluginUtf8, const std::string& relPathUtf8,
                           std::wstring& outFile) const {
    std::wstring rel;
    if (!SafeRelPath(relPathUtf8, rel)) return false;

    std::wstring root;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = roots_.find(pluginUtf8);
        if (it == roots_.end()) return false;
        root = it->second;
    }

    // Join and re-verify containment (defense in depth against symlink/junction
    // style surprises is NOT implied; this blocks textual traversal).
    std::wstring candidate = root + L"\\" + rel;
    wchar_t full[_MAX_PATH + 2];
    if (_wfullpath(full, candidate.c_str(), _MAX_PATH + 1) == nullptr) return false;
    std::wstring resolved(full);
    // Root itself is already fully resolved; the file must live under it.
    if (resolved.size() < root.size() ||
        _wcsnicmp(resolved.c_str(), root.c_str(), root.size()) != 0 ||
        (resolved.size() > root.size() && resolved[root.size()] != L'\\')) {
        return false;
    }

    DWORD attrs = GetFileAttributesW(resolved.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;

    outFile = resolved;
    return true;
}

// ---------------------------------------------------------------------------
// Resource handler
// ---------------------------------------------------------------------------

namespace {

class SuiResourceHandler : public CefResourceHandler {
public:
    SuiResourceHandler() = default;

    bool Open(CefRefPtr<CefRequest> request, bool& handle_request,
              CefRefPtr<CefCallback> callback) override {
        handle_request = true;   // completed synchronously below
        ok_ = Handle(request);
        return true;
    }

    void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& response_length,
                            CefString& redirectUrl) override {
        if (!ok_) {
            response->SetError(ERR_FILE_NOT_FOUND);
            response_length = -1;
            return;
        }
        response->SetStatus(200);
        response->SetMimeType(mime_);
        CefResponse::HeaderMap headers;
        headers.insert({"Content-Security-Policy", kContentCsp});
        headers.insert({"Cache-Control", "no-store"});       // hot reload
        headers.insert({"X-Content-Type-Options", "nosniff"});
        // Local-only content, but data:-URL layers (CreateLayerFromHtml) have an
        // opaque origin: allow them to load scheme subresources (scripts/CSS)
        // with an explicit base URL.
        headers.insert({"Access-Control-Allow-Origin", "*"});
        response->SetHeaderMap(headers);
        response_length = (int64_t)data_.size();
    }

    bool Read(void* data_out, int bytes_to_read, int& bytes_read,
              CefRefPtr<CefResourceReadCallback> callback) override {
        if (!ok_ || bytes_to_read <= 0) {
            bytes_read = 0;
            return false;
        }
        int64_t remaining = (int64_t)data_.size() - offset_;
        bytes_read = (int)std::min<int64_t>(bytes_to_read, remaining);
        if (bytes_read <= 0) {
            bytes_read = 0;
            return false;
        }
        memcpy(data_out, data_.data() + offset_, (size_t)bytes_read);
        offset_ += bytes_read;
        return true;
    }

    void Cancel() override {}

private:
    bool Handle(CefRefPtr<CefRequest> request) {
        std::string url = request->GetURL().ToString();
        // Parse with CEF's URL parser (host = plugin, path = /relpath).
        CefURLParts parts;
        if (!CefParseURL(url, parts)) return false;
        std::string plugin = CefString(&parts.host).ToString();
        std::string path = CefString(&parts.path).ToString();
        if (plugin.empty()) return false;

        std::wstring file;
        if (!Roots().Resolve(plugin, path, file)) {
            SUI_LOG_WARN("scheme: refused %s (plugin=%s)", url.c_str(), plugin.c_str());
            return false;
        }

        // Hot reload: always read from disk, never cache.
        HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size;
        if (!GetFileSizeEx(h, &size) || size.QuadPart > (64 * 1024 * 1024)) {
            CloseHandle(h);
            return false;
        }
        data_.resize((size_t)size.QuadPart);
        DWORD got = 0;
        BOOL readOk = size.QuadPart == 0 ||
                      (ReadFile(h, data_.data(), (DWORD)size.QuadPart, &got, nullptr) &&
                       got == (DWORD)size.QuadPart);
        CloseHandle(h);
        if (!readOk) {
            data_.clear();
            return false;
        }
        mime_ = MimeForExtension(file);
        return true;
    }

    bool ok_ = false;
    int64_t offset_ = 0;
    std::vector<uint8_t> data_;
    std::string mime_ = "application/octet-stream";

    IMPLEMENT_REFCOUNTING(SuiResourceHandler);
};

class SuiSchemeFactory : public CefSchemeHandlerFactory {
public:
    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                         const CefString& scheme_name,
                                         CefRefPtr<CefRequest> request) override {
        return new SuiResourceHandler();
    }

private:
    IMPLEMENT_REFCOUNTING(SuiSchemeFactory);
};

}  // namespace

bool RegisterSentinelUiScheme() {
    static bool s_registered = false;
    if (s_registered) return true;
    s_registered = true;
    bool ok = CefRegisterSchemeHandlerFactory("sentinelui", "", new SuiSchemeFactory());
    if (!ok) {
        SUI_LOG_ERROR("CefRegisterSchemeHandlerFactory(sentinelui) failed.");
    } else {
        SUI_LOG_INFO("sentinelui:// scheme handler registered.");
    }
    return ok;
}

void LogSchemeRegistration(bool ok) {
    // Runs in every CEF process (no logger initialized in children). Derive the
    // log dir from the exe location — no command-line args needed.
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    if (n == 0) return;
    std::wstring dir(exe, n);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    dir.resize(slash);
    dir += L"\\SentinelUI\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\scheme_registration.log";

    // Process type: browser when absent, else the --type= switch value.
    std::wstring cmd = GetCommandLineW();
    std::wstring type = L"browser";
    size_t tp = cmd.find(L"--type=");
    if (tp != std::wstring::npos) {
        size_t start = tp + 7;
        size_t end = cmd.find(L' ', start);
        type = cmd.substr(start, (end == std::wstring::npos ? cmd.size() : end) - start);
    }

    char line[512];
    int len = wsprintfA(line, "pid=%lu type=%ls scheme=sentinelui ok=%d\r\n",
                        GetCurrentProcessId(), type.c_str(), ok ? 1 : 0);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line, (DWORD)len, &written, nullptr);
    CloseHandle(h);
}

}  // namespace sui_helper
