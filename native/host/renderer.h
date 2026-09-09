// D3D11 overlay renderer: uploads the latest CEF BGRA frame to a texture on the
// game's device and composites it as a fullscreen premultiplied-alpha quad in the
// Present hook (game thread). Single-device design — no cross-device sharing.

#pragma once
#ifndef SENTINEL_HOST_RENDERER_H
#define SENTINEL_HOST_RENDERER_H

#include <d3d11.h>
#include <windows.h>
#include <cstdint>
#include <vector>

namespace sui_host {

class Renderer {
public:
    Renderer() = default;

    // Initialize once from the game's swapchain (called inside the first Present).
    // Recreates device resources if the backbuffer changed.
    bool EnsureResources(IDXGISwapChain* swapChain);

    // Upload a new BGRA frame into the overlay texture (no-op if null/small).
    void UploadFrame(const uint8_t* data, int width, int height) {
        if (!ready_ || context_ == nullptr || data == nullptr || width <= 0 || height <= 0) return;
        D3D11_BOX box = {};
        box.left = 0;
        box.top = 0;
        box.right = (UINT)width;
        box.bottom = (UINT)height;
        box.front = 0;
        box.back = 1;
        context_->UpdateSubresource(frameTex_, 0, &box, data, (UINT)width * 4, 0);
    }

    // Draw the current overlay texture onto the swapchain backbuffer (game thread).
    void DrawFrame(IDXGISwapChain* swapChain);

    int width() const { return texWidth_; }
    int height() const { return texHeight_; }

    // Forget resources after ResizeBuffers (recreate lazily).
    void Invalidate() { ready_ = false; }

    bool ready() const { return ready_; }

private:
    bool CreateQuadResources(ID3D11Device* device, int width, int height);
    void ReleaseResources();

    bool ready_ = false;
    int texWidth_ = 0;
    int texHeight_ = 0;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* frameTex_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11Buffer* vbo_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    // Cached views: creating RTV/SRV per frame is driver churn (BSOD/D1 risk).
    ID3D11RenderTargetView* cachedRtv_ = nullptr;
    IUnknown* cachedRtvBack_ = nullptr;   // backbuffer this RTV belongs to
    ID3D11ShaderResourceView* cachedSrv_ = nullptr;
    uint64_t uploadedGeneration_ = 0;   // (frames carry generation in hooks.cpp)
    int lastW_ = 0;
    int lastH_ = 0;
};

}  // namespace sui_host

#endif