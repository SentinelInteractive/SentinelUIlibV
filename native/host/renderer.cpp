#include "renderer.h"
#include "suilog.h"

#include <d3dcompiler.h>
#include <d3d11.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace sui_host {

namespace {

const char* kVsSrc = R"(struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
PSIn main(VSIn i) { PSIn o; o.pos = float4(i.pos.xy, 0, 1); o.uv = i.uv; return o; })";

const char* kPsSrc = R"(Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(PSIn i) : SV_Target {
  float4 c = tex0.Sample(samp0, i.uv);
  return float4(c.rgb * c.a, c.a);  // premultiplied alpha for ONE/INV_SRC_ALPHA
})";

HRESULT CompileShader(const char* src, const char* entry, const char* target,
                      ID3DBlob** blob) {
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
    if (FAILED(hr) && errors) {
        SUI_LOG_ERROR("shader compile %s failed: %s", entry,
                      (const char*)errors->GetBufferPointer());
    }
    if (errors) errors->Release();
    return hr;
}

}  // namespace

bool Renderer::CreateQuadResources(ID3D11Device* device, int width, int height) {
    if (texWidth_ == width && texHeight_ == height && ready_) return true;

    ReleaseResources();
    device_ = device;
    device_->GetImmediateContext(&context_);
    texWidth_ = width;
    texHeight_ = height;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &frameTex_))) {
        SUI_LOG_ERROR("CreateTexture2D failed (%dx%d).", width, height);
        return false;
    }

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (FAILED(CompileShader(kVsSrc, "main", "vs_4_0", &vsBlob)) ||
        FAILED(CompileShader(kPsSrc, "main", "ps_4_0", &psBlob))) {
        SUI_LOG_ERROR("shader compile failed.");
        return false;
    }
    device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_);

    float verts[3][4] = {
        {-1.0f, -1.0f, 0.0f, 1.0f},
        {-1.0f, 3.0f, 0.0f, -1.0f},
        {3.0f, -1.0f, 2.0f, 1.0f},
    };
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(verts);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {verts, 0, 0};
    device_->CreateBuffer(&bd, &init, &vbo_);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&sd, &sampler_);

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;            // premultiplied source
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&bld, &blend_);

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device_->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout_);

    if (vsBlob) vsBlob->Release();
    if (psBlob) psBlob->Release();

    ready_ = true;
    SUI_LOG_INFO("Renderer ready (%dx%d).", width, height);
    return true;
}

bool Renderer::EnsureResources(IDXGISwapChain* swapChain) {
    ID3D11Device* dev = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return false;

    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) {
        dev->Release();
        return false;
    }
    D3D11_TEXTURE2D_DESC desc;
    back->GetDesc(&desc);
    back->Release();

    if (!ready_ || texWidth_ != (int)desc.Width || texHeight_ != (int)desc.Height) {
        if (!CreateQuadResources(dev, (int)desc.Width, (int)desc.Height)) {
            dev->Release();
            return false;
        }
    }
    dev->Release();
    return ready_;
}

void Renderer::UploadFrame(const std::vector<uint8_t>& bgra, int width, int height) {
    if (!ready_ || context_ == nullptr || width <= 0 || height <= 0) return;
    if (bgra.size() < (size_t)width * height * 4) return;

    D3D11_BOX box = {};
    box.left = 0;
    box.top = 0;
    box.right = (UINT)width;
    box.bottom = (UINT)height;
    box.front = 0;
    box.back = 1;
    context_->UpdateSubresource(frameTex_, 0, &box, bgra.data(), (UINT)width * 4, 0);
}

void Renderer::DrawFrame(IDXGISwapChain* swapChain) {

    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;

    ID3D11RenderTargetView* rtv = nullptr;
    device_->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    if (!rtv) return;

    D3D11_VIEWPORT vp = {0, 0, (float)texWidth_, (float)texHeight_, 0, 1};

    ID3D11ShaderResourceView* srv = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(frameTex_, &srvDesc, &srv);

    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &vp);
    context_->IASetInputLayout(layout_);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = 16, offset = 0;
    context_->IASetVertexBuffers(0, 1, &vbo_, &stride, &offset);
    context_->VSSetShader(vs_, nullptr, 0);
    context_->PSSetShader(ps_, nullptr, 0);
    context_->PSSetShaderResources(0, 1, &srv);
    context_->PSSetSamplers(0, 1, &sampler_);
    const float blendFactor[4] = {1, 1, 1, 1};
    context_->OMSetBlendState(blend_, blendFactor, 0xffffffff);
    context_->Draw(3, 0);

    if (srv) srv->Release();
    rtv->Release();
}

void Renderer::ReleaseResources() {
    if (frameTex_) { frameTex_->Release(); frameTex_ = nullptr; }
    if (vs_) { vs_->Release(); vs_ = nullptr; }
    if (ps_) { ps_->Release(); ps_ = nullptr; }
    if (vbo_) { vbo_->Release(); vbo_ = nullptr; }
    if (sampler_) { sampler_->Release(); sampler_ = nullptr; }
    if (blend_) { blend_->Release(); blend_ = nullptr; }
    if (layout_) { layout_->Release(); layout_ = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    ready_ = false;
}

}  // namespace sui_host