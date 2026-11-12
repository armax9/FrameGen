#include "xr_materializer.hpp"
#include <cstdio>
#include <stdexcept>

using namespace presentation_study;
namespace {
ComPtr<ID3D11Device> device;
constexpr UINT width=128, height=128;
constexpr int64_t formats[]={DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                             DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM};
unsigned acquired{},waited{},released{};
struct FakeChain { ComPtr<ID3D11Texture2D> texture; };
FakeChain& chain(XrSwapchain swapchain){return *reinterpret_cast<FakeChain*>(swapchain);}
XrResult XRAPI_CALL enumerateFormats(XrSession,uint32_t capacity,uint32_t* count,int64_t* values){
 *count=std::size(formats);
 if(capacity)std::copy(std::begin(formats),std::end(formats),values);
 return XR_SUCCESS;
}
XrResult XRAPI_CALL create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* result){
 auto next=std::make_unique<FakeChain>();
 D3D11_TEXTURE2D_DESC desc{};
 desc.Width=info->width;desc.Height=info->height;
 desc.MipLevels=info->mipCount;desc.ArraySize=info->arraySize;
 desc.SampleDesc.Count=info->sampleCount;desc.Format=DXGI_FORMAT(info->format);
 desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
 if(FAILED(device->CreateTexture2D(&desc,nullptr,&next->texture)))return XR_ERROR_RUNTIME_FAILURE;
 *result=reinterpret_cast<XrSwapchain>(next.release());
 return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy(XrSwapchain swapchain){delete &chain(swapchain);return XR_SUCCESS;}
XrResult XRAPI_CALL enumerate(XrSwapchain swapchain,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* images){
 *count=1;
 if(capacity)reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)->texture=chain(swapchain).texture.Get();
 return XR_SUCCESS;
}
XrResult XRAPI_CALL acquire(XrSwapchain,const XrSwapchainImageAcquireInfo*,uint32_t* index){++acquired;*index=0;return XR_SUCCESS;}
XrResult XRAPI_CALL waitImage(XrSwapchain,const XrSwapchainImageWaitInfo*){++waited;return XR_SUCCESS;}
XrResult XRAPI_CALL releaseImage(XrSwapchain,const XrSwapchainImageReleaseInfo*){++released;return XR_SUCCESS;}
void require(bool ok,const char* description){if(!ok)throw std::runtime_error(description);}
}
int main(){
 try{
  ComPtr<ID3D11DeviceContext> context;
  require(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,
          D3D11_SDK_VERSION,&device,nullptr,&context)),"D3D11 device");
  const ImageDispatch dispatch{reinterpret_cast<XrSession>(1),enumerateFormats,create,destroy,
                              enumerate,acquire,waitImage,releaseImage};
  Materializer materializer;
  require(materializer.initialize(device.Get(),context.Get(),dispatch,nullptr,true),"materializer init");
  auto storage=std::make_shared<GpuStorage>();
  auto images=std::make_shared<GpuImages>();
  images->gpuComplete=true;
  images->owner=storage;
  D3D11_TEXTURE2D_DESC input{};
  input.Width=width;input.Height=height;input.MipLevels=input.ArraySize=input.SampleDesc.Count=1;
  input.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;input.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  std::vector<std::array<uint16_t,4>> pixels(width*height,{0x3c00,0x3400,0,0x3c00});
  D3D11_SUBRESOURCE_DATA data{pixels.data(),width*sizeof(pixels[0]),0};
  for(unsigned eye=0;eye<2;++eye){
   if(eye)std::fill(pixels.begin(),pixels.end(),std::array<uint16_t,4>{0,0x3400,0x3c00,0x3c00});
   require(SUCCEEDED(device->CreateTexture2D(&input,&data,&storage->texture[eye])),"input texture");
  }
  D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION_PREDICATE,0};
  ComPtr<ID3D11Predicate> predicate;
  if(SUCCEEDED(device->CreatePredicate(&query,&predicate))){
   context->Begin(predicate.Get());context->End(predicate.Get());
  }
  Plan plan;
  plan.images=images;
  require(!materializer.selectSourceFormat(DXGI_FORMAT_UNKNOWN),"reject unsupported format");
  for(const auto format:formats)for(bool arrayStereo:{false,true}){
   require(materializer.selectSourceFormat(format),"switch source format");
   require(materializer.format()==format,"format selection");
   materializer.prime({width,width},{height,height},arrayStereo);
   images->generation=++plan.sequence;
   std::array<XrSwapchainSubImage,2> output{};
   auto description=input;
   description.Format=DXGI_FORMAT(format);
   description.ArraySize=arrayStereo?2:1;
   description.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
   std::array expected{description,description};
   acquired=waited=released=0;
   if(predicate)context->SetPredication(predicate.Get(),FALSE);
   require(materializer.materialize(plan,output,nullptr,nullptr,nullptr,nullptr,&expected,arrayStereo),"draw both eyes");
   const auto chainCount=arrayStereo?1u:2u;
   require(acquired==chainCount&&waited==chainCount&&released==chainCount,"one image cycle per swapchain");
   require((output[0].swapchain==output[1].swapchain)==arrayStereo,"expected stereo layout");
   ComPtr<ID3D11Predicate> restored;BOOL value=TRUE;
   context->GetPredication(&restored,&value);
   require(!predicate||(restored.Get()==predicate.Get()&&value==FALSE),"restore application predicate");
   context->SetPredication(nullptr,FALSE);
   for(unsigned index=0;index<output.size();++index){
    const auto& eye=output[index];
    require(eye.imageArrayIndex==(arrayStereo?index:0),"expected eye slice");
    auto& texture=chain(eye.swapchain).texture;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    require(desc.Format==format&&desc.Width==width&&desc.Height==height&&
            desc.ArraySize==description.ArraySize,"native texture description");
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&readback)),"readback texture");
    context->CopyResource(readback.Get(),texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(context->Map(readback.Get(),eye.imageArrayIndex,D3D11_MAP_READ,0,&mapped)),"readback map");
    const bool bgra=format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB||format==DXGI_FORMAT_B8G8R8A8_UNORM;
    const bool srgb=format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const auto* color=static_cast<const uint8_t*>(mapped.pData)+(height/2)*mapped.RowPitch+(width/2)*4;
    const bool correct=color[bgra?2:0]==(index?0:255)&&std::abs(int(color[1])-(srgb?137:64))<=1&&
                       color[bgra?0:2]==(index?255:0)&&color[3]==255;
    context->Unmap(readback.Get(),eye.imageArrayIndex);
    require(correct,"read back expected color");
   }
   expected[1].BindFlags=D3D11_BIND_SHADER_RESOURCE;
   require(!materializer.materialize(plan,output,nullptr,nullptr,nullptr,nullptr,&expected,arrayStereo),
           "reject mismatched native texture descriptor");
   context->Flush();
   const auto deadline=GetTickCount64()+2000;
   while(!materializer.idle()&&GetTickCount64()<deadline)Sleep(1);
   require(materializer.idle(),"wait for validation draws");
   std::printf("DXGI %lld (%s): both eye pixels verified\n",static_cast<long long>(format),
               arrayStereo?"array":"separate");
  }
  std::printf("Application predication %s\n",predicate?"preserved":"unavailable on this backend");
  return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
