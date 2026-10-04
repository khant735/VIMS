#pragma once
#include <windows.h>
#include <string>

class D3D11CalibrationRenderer {
public:
    ~D3D11CalibrationRenderer();
    bool initialize(HWND hwnd, std::wstring& error);
    void shutdown();
    bool draw(float seconds);
    void setIdentity(const std::wstring& backend,const std::wstring& gpu){ identityBackend_=backend; identityGpu_=gpu; }
    bool ready() const { return device_ != nullptr; }
    const std::wstring& gpuName() const { return gpuName_; }
private:
    HWND hwnd_{};
    struct ID3D11Device* device_{};
    struct ID3D11DeviceContext* context_{};
    struct IDXGISwapChain* swap_{};
    struct ID3D11RenderTargetView* rtv_{};
    struct ID3D11DepthStencilView* dsv_{};
    struct ID3D11Buffer* vb_{};
    struct ID3D11Buffer* cb_{};
    struct ID3D11VertexShader* vs_{};
    struct ID3D11PixelShader* ps_{};
    struct ID3D11InputLayout* layout_{};
    std::wstring gpuName_;
    std::wstring identityBackend_, identityGpu_;
    bool createTargets();
};