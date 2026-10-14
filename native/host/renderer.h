// D3D11 overlay renderer: per-layer textures + z-ordered composite.
// Each layer's newest BGRA frame (from its own shared-memory mapping) is
// uploaded into a texture on the game's device; DrawLayers composites the
// visible stack back-to-front as fullscreen premultiplied-alpha quads in the
// Present hook (game thread). Single-device design: no cross-device sharing.

#pragma once

#include <d3d11.h>
#include <windows.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sui_host {

class Renderer {
public:
    Renderer() = default;

    // Initialize once from the game's swapchain (called inside the first Present).
    // Creates the shared quad/shader state; per-layer textures arrive with
    // UploadLayer. Recreates everything if the backbuffer changed.
    bool EnsureResources(IDXGISwapChain* swapChain);

    // Upload a layer's BGRA frame into its texture (creating/recreating it when
    // the frame size changed). No-op when the data is null/small.
    void UploadLayer(int layerId, const uint8_t* data, int width, int height);

    // Drop a layer's texture (layer closed).
    void RemoveLayer(int layerId);

    // Composite the given layers (back-to-front order) onto the backbuffer.
    void DrawLayers(IDXGISwapChain* swapChain, const std::vector<int>& layerIds);

    // Forget all resources after ResizeBuffers/device loss (recreated lazily).
    void Invalidate();

    bool ready() const { return ready_; }
    int width() const { return backWidth_; }
    int height() const { return backHeight_; }

private:
    bool CreateSharedResources(ID3D11Device* device, int backWidth, int backHeight);
    void ReleaseResources();
    ID3D11ShaderResourceView* LayerSrv(int layerId);

    struct LayerTex {
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int width = 0;
        int height = 0;
    };

    bool ready_ = false;
    int backWidth_ = 0;
    int backHeight_ = 0;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11Buffer* vbo_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    // Cached RTV: creating it per frame is driver churn (BSOD/D1 risk).
    ID3D11RenderTargetView* cachedRtv_ = nullptr;
    IUnknown* cachedRtvBack_ = nullptr;   // backbuffer this RTV belongs to

    std::unordered_map<int, LayerTex> layers_;
};

}  // namespace sui_host
