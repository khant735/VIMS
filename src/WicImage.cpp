#include "WicImage.h"
#include <windows.h>
#include <wincodec.h>
#include <stdexcept>
#include <sstream>
static void ck(HRESULT h,const char* s){if(FAILED(h))throw std::runtime_error(s);} 
ImageRGBA loadImageWic(const std::filesystem::path& p){
 CoInitializeEx(nullptr,COINIT_MULTITHREADED); IWICImagingFactory* f=nullptr; ck(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)),"WIC factory failed");
 IWICBitmapDecoder*d=nullptr; ck(f->CreateDecoderFromFilename(p.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&d),"Could not decode image"); IWICBitmapFrameDecode*fr=nullptr; ck(d->GetFrame(0,&fr),"No image frame"); UINT w=0,h=0; fr->GetSize(&w,&h); IWICFormatConverter*c=nullptr; ck(f->CreateFormatConverter(&c),"WIC converter failed"); ck(c->Initialize(fr,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"WIC RGBA conversion failed"); ImageRGBA out;out.width=w;out.height=h;out.pixels.resize(size_t(w)*h*4);ck(c->CopyPixels(nullptr,w*4,(UINT)out.pixels.size(),out.pixels.data()),"WIC pixel copy failed"); c->Release();fr->Release();d->Release();f->Release();return out;}
void saveMaskPngWic(const std::filesystem::path&p,const Mask&m){CoInitializeEx(nullptr,COINIT_MULTITHREADED);IWICImagingFactory*f=nullptr;ck(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)),"WIC factory failed");IWICStream*s=nullptr;ck(f->CreateStream(&s),"WIC stream failed");auto full=std::filesystem::absolute(p).wstring();if(full.rfind(L"\\\\?\\",0)!=0){if(full.rfind(L"\\\\",0)==0)full=L"\\\\?\\UNC\\"+full.substr(2);else full=L"\\\\?\\"+full;}HRESULT opened=s->InitializeFromFilename(full.c_str(),GENERIC_WRITE);if(FAILED(opened)){std::ostringstream error;error<<"Could not create PNG (HRESULT 0x"<<std::hex<<static_cast<unsigned>(opened)<<", path length "<<std::dec<<full.size()<<"): "<<p.string()<<". Check folder write permission and free disk space.";throw std::runtime_error(error.str());}IWICBitmapEncoder*e=nullptr;ck(f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&e),"PNG encoder failed");ck(e->Initialize(s,WICBitmapEncoderNoCache),"PNG encoder init failed");IWICBitmapFrameEncode*fr=nullptr;IPropertyBag2*b=nullptr;ck(e->CreateNewFrame(&fr,&b),"PNG frame failed");ck(fr->Initialize(b),"PNG frame init failed");fr->SetSize(m.width,m.height);WICPixelFormatGUID fmt=GUID_WICPixelFormat8bppGray;fr->SetPixelFormat(&fmt);ck(fr->WritePixels(m.height,m.width,(UINT)m.pixels.size(),const_cast<BYTE*>(m.pixels.data())),"PNG write failed");fr->Commit();e->Commit();if(b)b->Release();fr->Release();e->Release();s->Release();f->Release();}

void saveCutoutPngWic(const std::filesystem::path& p,const ImageRGBA& image,const Mask& mask){
 if(image.empty()||mask.empty()||image.width!=mask.width||image.height!=mask.height)
  throw std::runtime_error("Cutout source and mask dimensions do not match");
 const size_t count=mask.pixels.size();
 if(count>UINT32_MAX/4) throw std::runtime_error("Cutout image exceeds WIC PNG size limit");
 std::vector<BYTE> bgra(count*4);
 for(size_t i=0;i<count;++i){
  const size_t j=i*4;
  bgra[j]=image.pixels[j+2]; bgra[j+1]=image.pixels[j+1]; bgra[j+2]=image.pixels[j];
  bgra[j+3]=static_cast<BYTE>((unsigned(image.pixels[j+3])*unsigned(mask.pixels[i])+127)/255);
 }
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 IWICImagingFactory* f=nullptr;
 ck(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)),"WIC factory failed");
 IWICStream* stream=nullptr; IWICBitmapEncoder* encoder=nullptr;
 IWICBitmapFrameEncode* frame=nullptr; IPropertyBag2* bag=nullptr;
 try {
  ck(f->CreateStream(&stream),"WIC stream failed");
  // WIC inherits Win32 MAX_PATH restrictions on ordinary paths. Use the
  // extended absolute path for deeply nested user-selected application folders.
  auto full=std::filesystem::absolute(p).wstring();
  if(full.rfind(L"\\\\?\\",0)!=0){
    if(full.rfind(L"\\\\",0)==0)full=L"\\\\?\\UNC\\"+full.substr(2);
    else full=L"\\\\?\\"+full;
  }
  const HRESULT opened=stream->InitializeFromFilename(full.c_str(),GENERIC_WRITE);
  if(FAILED(opened)){std::ostringstream error;error<<"Could not create cutout PNG (HRESULT 0x"<<std::hex<<static_cast<unsigned>(opened)<<", path length "<<std::dec<<full.size()<<"). Check folder write permission and free disk space.";throw std::runtime_error(error.str());}
  ck(f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder failed");
  ck(encoder->Initialize(stream,WICBitmapEncoderNoCache),"PNG encoder init failed");
  ck(encoder->CreateNewFrame(&frame,&bag),"PNG frame failed");
  ck(frame->Initialize(bag),"PNG frame init failed");
  ck(frame->SetSize(image.width,image.height),"PNG dimensions failed");
  WICPixelFormatGUID fmt=GUID_WICPixelFormat32bppBGRA;
  ck(frame->SetPixelFormat(&fmt),"PNG BGRA format failed");
  if(!IsEqualGUID(fmt,GUID_WICPixelFormat32bppBGRA)) throw std::runtime_error("PNG encoder cannot preserve transparency");
  ck(frame->WritePixels(image.height,image.width*4,static_cast<UINT>(bgra.size()),bgra.data()),"Cutout PNG write failed");
  ck(frame->Commit(),"Cutout PNG frame commit failed");
  ck(encoder->Commit(),"Cutout PNG commit failed");
 } catch(...) {
  if(bag)bag->Release(); if(frame)frame->Release(); if(encoder)encoder->Release();
  if(stream)stream->Release(); f->Release(); throw;
 }
 if(bag)bag->Release(); frame->Release(); encoder->Release(); stream->Release(); f->Release();
}
