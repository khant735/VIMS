#include "D3D11CalibrationRenderer.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <cwctype>

struct V { float x,y,z,r,g,b; };
struct C { float m[16]; };
static void rel(IUnknown*&p){if(p){p->Release();p=nullptr;}}
static void addTextBars(std::vector<V>& out,const std::wstring& text,float x,float y,float scale){
    static const unsigned short glyphs[37]={
        0x7B6F,0x2492,0x73E7,0x73CF,0x5BC9,0x79CF,0x79EF,0x7249,0x7BEF,0x7BCF,
        0x7BED,0x6BAE,0x7927,0x6B6E,0x79E7,0x79E4,0x792F,0x5BED,0x7249,0x124E,0x5AAD,0x4927,0x5F6D,0x5B6D,0x7B6F,0x7BE4,0x7B6B,0x7BEA,0x79CF,0x7248,0x5B6F,0x5B6A,0x5F7D,0x5AAD,0x5AA4,0x72E7,0};
    auto bit=[&](wchar_t ch,int r,int col)->bool{int idx=-1;if(ch>=L'0'&&ch<=L'9')idx=ch-L'0';else if(ch>=L'A'&&ch<=L'Z')idx=10+ch-L'A';if(idx<0)return false;return ((glyphs[idx]>>(14-(r*3+col)))&1)!=0;};
    float px=x;for(wchar_t raw:text){wchar_t ch=(wchar_t)towupper(raw);if(ch==L' '){px+=scale*2.5f;continue;}if(ch==L':'||ch==L'-'||ch==L'/'){px+=scale*2;continue;}
        for(int r=0;r<5;r++)for(int col=0;col<3;col++)if(bit(ch,r,col)){float x0=px+col*scale,y0=y-r*scale,x1=x0+scale*.82f,y1=y0-scale*.82f;float z=.5f;V a{x0,y0,z,1,1,1},b{x1,y0,z,1,1,1},cc{x1,y1,z,1,1,1},d{x0,y1,z,1,1,1};out.insert(out.end(),{a,b,cc,a,cc,d});}px+=scale*3.8f;}
}
D3D11CalibrationRenderer::~D3D11CalibrationRenderer(){shutdown();}
bool D3D11CalibrationRenderer::createTargets(){
    ID3D11Texture2D* back=nullptr;if(FAILED(swap_->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&back)))return false;
    HRESULT hr=device_->CreateRenderTargetView(back,nullptr,&rtv_);D3D11_TEXTURE2D_DESC d{};back->GetDesc(&d);back->Release();if(FAILED(hr))return false;
    D3D11_TEXTURE2D_DESC dd{};dd.Width=d.Width;dd.Height=d.Height;dd.MipLevels=1;dd.ArraySize=1;dd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dd.SampleDesc.Count=1;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depth=nullptr;if(FAILED(device_->CreateTexture2D(&dd,nullptr,&depth)))return false;hr=device_->CreateDepthStencilView(depth,nullptr,&dsv_);depth->Release();return SUCCEEDED(hr);
}
bool D3D11CalibrationRenderer::initialize(HWND hwnd,std::wstring& error){
    hwnd_=hwnd;RECT r{};GetClientRect(hwnd,&r);DXGI_SWAP_CHAIN_DESC sd{};sd.BufferCount=2;sd.BufferDesc.Width=std::max(1L,r.right);sd.BufferDesc.Height=std::max(1L,r.bottom);sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.OutputWindow=hwnd;sd.SampleDesc.Count=1;sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;sd.Flags=DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    D3D_FEATURE_LEVEL fl{};if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&swap_,&device_,&fl,&context_))){error=L"Direct3D 11 hardware device creation failed.";return false;}
    IDXGIDevice* xd=nullptr;IDXGIAdapter* a=nullptr;DXGI_ADAPTER_DESC ad{};if(SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice),(void**)&xd))&&SUCCEEDED(xd->GetAdapter(&a))){a->GetDesc(&ad);gpuName_=ad.Description;a->Release();}if(xd)xd->Release();
    if(!createTargets()){error=L"Direct3D 11 render-target creation failed.";shutdown();return false;}
    const char* shader="cbuffer C:register(b0){float4x4 m;} struct I{float3 p:POSITION;float3 c:COLOR;};struct O{float4 p:SV_POSITION;float3 c:COLOR;};O vs(I i){O o;o.p=mul(float4(i.p,1),m);o.c=i.c;return o;}float4 ps(O i):SV_TARGET{return float4(i.c,1);}";
    ID3DBlob *v=nullptr,*p=nullptr,*e=nullptr;if(FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_4_0",0,0,&v,&e))||FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_4_0",0,0,&p,&e))){if(e)e->Release();error=L"Direct3D shader compilation failed.";shutdown();return false;}
    device_->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs_);device_->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps_);
    D3D11_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};device_->CreateInputLayout(il,2,v->GetBufferPointer(),v->GetBufferSize(),&layout_);v->Release();p->Release();if(e)e->Release();
    std::vector<V> verts;
    auto gear=[&](float cx,float cy,float radius,int teeth,float rr,float gg,float bb){
        const int seg=teeth*2;const float inner=radius*.78f;
        for(int i=0;i<seg;++i){float a0=6.2831853f*i/seg,a1=6.2831853f*(i+1)/seg;float r0=(i&1)?inner:radius,r1=((i+1)&1)?inner:radius;
            verts.push_back({cx,cy,.10f,rr,gg,bb});verts.push_back({cx+cosf(a0)*r0,cy+sinf(a0)*r0,.10f,rr,gg,bb});verts.push_back({cx+cosf(a1)*r1,cy+sinf(a1)*r1,.10f,rr,gg,bb});}
    };
    auto disc=[&](float cx,float cy,float radius,float rr,float gg,float bb){for(int i=0;i<32;++i){float a0=6.2831853f*i/32,a1=6.2831853f*(i+1)/32;verts.push_back({cx,cy,.05f,rr,gg,bb});verts.push_back({cx+cosf(a0)*radius,cy+sinf(a0)*radius,.05f,rr,gg,bb});verts.push_back({cx+cosf(a1)*radius,cy+sinf(a1)*radius,.05f,rr,gg,bb});}};
    gear(-.38f,.02f,.48f,16,.72f,.10f,.045f);disc(-.38f,.02f,.14f,.72f,.74f,.77f);
    gear(.48f,.02f,.32f,8,.045f,.20f,.78f);disc(.48f,.02f,.10f,.72f,.74f,.77f);
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=(UINT)(verts.size()*sizeof(V));bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA init{verts.data()};device_->CreateBuffer(&bd,&init,&vb_);bd.ByteWidth=sizeof(C);bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;device_->CreateBuffer(&bd,nullptr,&cb_);return true;
}
bool D3D11CalibrationRenderer::draw(float seconds){
    if(!device_||!swap_||!context_||!rtv_)return false;RECT r{};GetClientRect(hwnd_,&r);if(r.right<=0||r.bottom<=0)return false;
    // Reassert DXGI ownership/visibility after the Vulkan swapchain was destroyed.
    // This avoids Present succeeding while DWM continues displaying Vulkan's last frame.
    if(!IsWindowVisible(hwnd_))ShowWindow(hwnd_,SW_SHOW);
    float clear[]={.025f,.035f,.055f,1};context_->OMSetRenderTargets(1,&rtv_,dsv_);context_->ClearRenderTargetView(rtv_,clear);context_->ClearDepthStencilView(dsv_,D3D11_CLEAR_DEPTH,1,0);
    D3D11_VIEWPORT vp{0,0,(float)r.right,(float)r.bottom,0,1};context_->RSSetViewports(1,&vp);UINT stride=sizeof(V),off=0;context_->IASetVertexBuffers(0,1,&vb_,&stride,&off);context_->IASetInputLayout(layout_);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context_->VSSetShader(vs_,nullptr,0);context_->PSSetShader(ps_,nullptr,0);
    float c=cosf(seconds),s=sinf(seconds);C x{{c,s,0,0,-s,c,0,0,0,0,1,0,0,0,0,1}};D3D11_MAPPED_SUBRESOURCE map{};context_->Map(cb_,0,D3D11_MAP_WRITE_DISCARD,0,&map);memcpy(map.pData,&x,sizeof(x));context_->Unmap(cb_,0);context_->VSSetConstantBuffers(0,1,&cb_);context_->Draw(336,0);
    if(!identityBackend_.empty()){
        // Draw the identity as framebuffer geometry after the gears. Disable depth
        // so the HUD cannot be rejected by the scene depth buffer.
        std::wstring label=identityBackend_+L"  "+identityGpu_;std::vector<V> tv;addTextBars(tv,label,-.94f,.92f,.010f);
        if(!tv.empty()){ID3D11Buffer* tb=nullptr;D3D11_BUFFER_DESC td{};td.ByteWidth=(UINT)(tv.size()*sizeof(V));td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA ti{tv.data()};
            if(SUCCEEDED(device_->CreateBuffer(&td,&ti,&tb))){ID3D11DepthStencilState* noDepth=nullptr;D3D11_DEPTH_STENCIL_DESC nd{};nd.DepthEnable=FALSE;nd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;nd.DepthFunc=D3D11_COMPARISON_ALWAYS;device_->CreateDepthStencilState(&nd,&noDepth);context_->OMSetDepthStencilState(noDepth,0);
                // The scene vertex shader uses mul(rowVector, matrix), so put clip-space
                // identity on the constant buffer in the same layout as the scene.
                UINT ts=sizeof(V),to=0;context_->IASetVertexBuffers(0,1,&tb,&ts,&to);C id{{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}};D3D11_MAPPED_SUBRESOURCE tm{};if(SUCCEEDED(context_->Map(cb_,0,D3D11_MAP_WRITE_DISCARD,0,&tm))){memcpy(tm.pData,&id,sizeof(id));context_->Unmap(cb_,0);context_->Draw((UINT)tv.size(),0);}context_->OMSetDepthStencilState(nullptr,0);if(noDepth)noDepth->Release();tb->Release();}
        }
    }
    HRESULT phr=swap_->Present(1,0);if(SUCCEEDED(phr)){DwmFlush();}return SUCCEEDED(phr);
}
void D3D11CalibrationRenderer::shutdown(){if(context_)context_->ClearState();IUnknown* p=nullptr;
#define R(x) p=(IUnknown*)x;rel(p);x=nullptr
R(cb_);R(vb_);R(layout_);R(ps_);R(vs_);R(dsv_);R(rtv_);R(swap_);R(context_);R(device_);
#undef R
}