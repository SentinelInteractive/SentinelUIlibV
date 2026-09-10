// SentinelUI Helper — sentinelui:// content scheme (issue #2).
//
//   sentinelui://<plugin>/<relpath>  ->  <registered root>\<relpath>
//
// Rules (issue #2 acceptance):
//  * local-only: every request is resolved against a plugin-registered local
//    content root; the scheme never touches the network and is registered as a
//    standard scheme handled entirely in the browser process.
//  * traversal-safe: path segments are validated (no "..", no drive/UNC parts,
//    no percent-encoding games); the resolved file must stay under the root.
//  * CSP-tight: HTML responses carry a strict Content-Security-Policy
//    (external scripts only — inline <script> is blocked by design).
//  * hot reload: files are read from disk on EVERY request and responses are
//    marked no-store, so editing a page + reload is enough.

#pragma once
#ifndef SENTINEL_HELPER_SCHEME_H
#define SENTINEL_HELPER_SCHEME_H

#include <map>
#include <mutex>
#include <string>

#include "include/cef_scheme.h"

namespace sui_helper {

// Thread-safe registry of plugin content roots (plugin name -> absolute dir).
class ContentRoots {
public:
    // Default root shipped with the runtime: plugin "sui" -> <dir>.
    void SetDefaultRoot(const std::wstring& dir);
    // From the ASI ('R' command): "R<len>:<plugin>|<path>" body. Returns false
    // when the plugin name/path are malformed.
    bool Register(const std::string& body);

    // Resolves plugin + relPath to an existing file under its root.
    // Returns false (and leaves outFile untouched) on any policy violation.
    bool Resolve(const std::string& pluginUtf8, const std::string& relPathUtf8,
                 std::wstring& outFile) const;

private:
    mutable std::mutex mu_;
    std::map<std::string, std::wstring> roots_;
};

ContentRoots& Roots();

// Registers the sentinelui:// scheme handler factory with the global request
// context. Safe to call from any thread in the browser process (CEF dispatches
// internally); also called right after CefInitialize and again from
// OnContextInitialized, so the factory is live before any layer navigates.
// Returns the CEF result of the (first) registration.
bool RegisterSentinelUiScheme();

// Called from CefApp::OnRegisterCustomSchemes in EVERY process (browser,
// renderer, GPU, utility/network service). Appends one diagnostic line to
// <exe dir>\SentinelUI\logs\scheme_registration.log so field issues
// (ERR_UNKNOWN_URL_SCHEME) can be traced to the process that failed to
// register the scheme. |ok| is the AddCustomScheme result.
void LogSchemeRegistration(bool ok);

}  // namespace sui_helper

#endif
