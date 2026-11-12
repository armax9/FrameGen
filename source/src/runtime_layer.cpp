#include <deque>
#include "xrfg/private_openxr_runtime.hpp"
#include <openxr/openxr_loader_negotiation.h>
#include <atomic>
#include "color_flow_pipeline.hpp"
#include "host_timing.hpp"
#include "source_credit.hpp"
#include "source_release_phase.hpp"
#include "stock_ofxr_shader.hpp"
#include "xr_materializer.hpp"
#include "native_frame_path.hpp"
#include "shared_context_state.hpp"
#include "menu_framegen_profile.hpp"
#ifdef FRAMEGEN_NATIVE_D3D12
#include "d3d12_color_flow.hpp"
#include "d3d12_quad_compositor.hpp"
#include "native_d3d12_outputs.hpp"
#endif
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <wrl/client.h>
#include <thread>
#include <condition_variable>
#include <chrono>
using Microsoft::WRL::ComPtr;
namespace {
constexpr char layerName[]="XR_APILAYER_ARMAX_metalfx_study";
constexpr char colorLayerName[]="XR_APILAYER_ARMAX_amd_color_study";
constexpr char fixedLayerName[]="XR_APILAYER_ARMAX_amd_fixed_study";
struct Instance {
 XrInstance handle{};
 PFN_xrGetInstanceProcAddr gipa{};
 bool sourcePoseTags=false,passThrough=false,managed=false,openComposite=false;
};
using runtime_study::NativeFramePath;using runtime_study::cpuMicros;using runtime_study::addCpuMetric;
#ifdef FRAMEGEN_NATIVE_D3D12
struct NativeD3D12Generation {
 runtime_study::D3D12ColorFlow flow;
 runtime_study::NativeD3D12Outputs outputs;
 std::atomic<bool> primed{};
 explicit NativeD3D12Generation(NativeFramePath& endpoint):outputs(endpoint){}
};
#endif
enum class ProfileMode { full, capture, generate, native };
const char*profileName(ProfileMode mode){switch(mode){case ProfileMode::capture:return "capture";case ProfileMode::generate:return "generate";case ProfileMode::native:return "native";default:return "full";}}
bool parseProfile(const char*text,ProfileMode&mode){if(!strcmp(text,"capture"))mode=ProfileMode::capture;else if(!strcmp(text,"generate"))mode=ProfileMode::generate;else if(!strcmp(text,"native"))mode=ProfileMode::native;else if(!strcmp(text,"full"))mode=ProfileMode::full;else return false;return true;}
constexpr bool collectStudyDiagnostics=false;
constexpr bool envEnabled(const char*){return false;}
constexpr unsigned initialSourcePhase(){return 0;}
uint64_t studyClock(){if constexpr(collectStudyDiagnostics)return cpuMicros();return 0;}
struct Session{
 struct SourceJob{
  runtime_study::SourceCredit::Ticket sourceTicket;
  std::shared_ptr<NativeFramePath::LeasedFrame> frame;
  bool managedFrameReady=true;
 };
 std::mutex sourceMutex;std::condition_variable sourceChanged;
 std::deque<std::shared_ptr<SourceJob>> sourceQueue;std::thread sourceWorker;
 bool sourceStopping=true,sourceActive=false;std::atomic<bool>asyncSource=false,sourceSingle=false,nativeOne=false;std::atomic<DWORD>sourceWorkerId=0;
 std::atomic<XrResult>sourceFailure=XR_SUCCESS;
 runtime_study::SourceCredit sourceCredit;bool sourceCreditEnabled=envEnabled("MOLTEN_FG_WAIT_SOURCE_CREDIT");
 runtime_study::SourceReleasePhase sourcePhase;std::atomic<unsigned>sourcePhaseEighths=initialSourcePhase();std::atomic<uint64_t>sourcePhaseWaits=0,sourcePhaseWaitUs=0;
 bool sourceCadence=envEnabled("MOLTEN_FG_SOURCE_CADENCE");
 bool cadenceUniform=sourceCadence&&envEnabled("MOLTEN_FG_CADENCE_UNIFORM");
 const bool sharedForwardCadence=cadenceUniform&&envEnabled("MOLTEN_FG_SHARED_FORWARD_CADENCE");
 std::atomic<uint64_t> sharedPhaseStarts{};
 uint64_t lastSourceReleaseUs=0; // protected by sourceMutex
 std::atomic<uint64_t> sourceCadenceWaits=0,sourceCadenceWaitUs=0;
 std::atomic<uint64_t>sourceCreditWaits=0,sourceCreditEmpty=0,sourceCreditWaitUs=0,sourceCreditWaitMaxUs=0,sourceCreditCanceled=0;
 std::atomic<uint64_t>sourceQueueWaitUs=0,sourceQueueWaitMaxUs=0,sourceJobs=0;
 void startSourceWorker(bool requested=false);void stopSourceWorker();XrResult drainSourceWorker();XrResult enqueueSource(const XrFrameEndInfo*,std::int64_t applicationEndNs,bool managedFrameReady=true);
 XrResult waitExistingSourceCredit();
 std::atomic<ProfileMode> profile=ProfileMode::full;std::atomic<bool>skipPrepare=false;bool profileControl=false;std::atomic<bool>fastReplay=false,profileStages=false;ULONGLONG lastControlTick=0;
 bool expireSynthetic=envEnabled("MOLTEN_FG_EXPIRE_SYNTHETIC");
 std::atomic<uint64_t> expiredSynthetic{};
 bool directSynthetic=false,forwardPrediction=false;
 const bool liveToggle=envEnabled("MOLTEN_FG_LIVE_TOGGLE");
 const bool fixedSourceComparison=liveToggle&&envEnabled("MOLTEN_FG_TEST_FIXED_SOURCE");
 bool managed=false, managedRequested=false;
 bool managedD3D12=false;
 bool matchSourceColor=false;
 bool allowsQuadLayers()const noexcept {
  // The MoltenVR event publisher can only submit a single projection.
  // Stock presentation retains simple quads alongside the projection.
  return managedD3D12||matchSourceColor||!instance->sourcePoseTags;
 }
 std::atomic<runtime_study::menu_framegen::AdmissionIssue> managedIssue{};
 runtime_study::menu_framegen::AdmissionIssue managedInitIssue{};
 runtime_study::menu_framegen::SessionStatus managedStatus;
 std::atomic<bool> managedRuntimeAvailable{false},managedUnsupportedFrame{false};
 std::atomic<ULONGLONG> managedSyntheticTick{};
 std::atomic<bool> managedStereoReady{false};
 std::atomic<unsigned> managedIncompatibleFrames{};
 std::unordered_map<XrSwapchain,XrSwapchainCreateInfo> managedColorChains;
 const std::wstring comparisonModeFile=[this]{
  if(!fixedSourceComparison)return std::wstring{};
  wchar_t path[MAX_PATH]{};
  const auto size=GetEnvironmentVariableW(L"MOLTEN_FG_TEST_MODE_FILE",path,MAX_PATH);
  return size&&size<MAX_PATH?std::wstring(path,size):std::wstring{};
 }();
 ULONGLONG nextComparisonPoll{};
 std::atomic<bool> generationEnabled{true};
 std::mutex toggleMutex;
 bool toggleDown=false;
 XrResult pollGenerationToggle();
 void startManagedStatus(){
  if(!managed)return;
  managedStatus.start([this](bool requested,ULONGLONG now){
   using namespace runtime_study::menu_framegen;
   const auto enabled=generationEnabled.load();
   if(!requested&&!enabled)return StatusSnapshot{Status::off};
   if(!managedRuntimeAvailable.load())
    return StatusSnapshot{managedD3D12?Status::unsupported_d3d12:Status::unsupported,managedInitIssue};
   const auto failure=sourceFailure.load();
   if(XR_FAILED(failure))return StatusSnapshot{Status::unsupported,{BlockReason::source_failure,failure}};
   if(colorFault.load())return StatusSnapshot{Status::unsupported,{BlockReason::color_fault}};
   const auto issue=managedIssue.load();
   if(managedUnsupportedFrame.load()||(managedIncompatibleFrames.load()>=3&&issue.reason!=BlockReason::none))
    return StatusSnapshot{Status::unsupported,issue};
   const auto generated=managedSyntheticTick.load();
   if(enabled&&managedStereoReady.load()&&generated&&generated<=now&&now-generated<750)
    return StatusSnapshot{Status::active};
   return StatusSnapshot{Status::waiting};
  });
 }
 const bool nativeApplicationColor=envEnabled("MOLTEN_FG_NATIVE_APPLICATION_COLOR");
 std::atomic<uint64_t> forwardNativeOriginals{};
 std::atomic<uint64_t> directSyntheticFrames{},forwardSyntheticFrames{};
 std::atomic<uint64_t> repeatControlFrames{};
 enum class ForwardExpiry:unsigned { originalCopy, encoder, generation, conversion, admission, originalAdmission, count };
 std::array<std::atomic<uint64_t>,unsigned(ForwardExpiry::count)> forwardExpiry{};
 std::atomic<uint64_t> forwardOriginalCount{},forwardBeforeAdmissionUs{},forwardReservationUs{};
 std::atomic<uint64_t> forwardLateBeforeAdmissionUs{},forwardLateReservationUs{};
 void expireForward(ForwardExpiry stage){if constexpr(collectStudyDiagnostics){++expiredSynthetic;++forwardExpiry[unsigned(stage)];}}
 runtime_study::timing::HostTiming<> hostTiming;bool hostTimingEnabled=envEnabled("MOLTEN_FG_TIMING");
 void hostStage(XrTime tag,runtime_study::timing::HostStage stage){if(hostTimingEnabled||((expireSynthetic||forwardPrediction)&&stage==runtime_study::timing::HostStage::renderEnd))hostTiming.mark(tag,stage,cpuMicros());}
 std::atomic<uint64_t>deliveryCalls=0,deliveryUs=0,deliveryMaxUs=0,extraWaitUs=0,extraWaitMaxUs=0;
 std::mutex timingMutex;struct Timing{XrTime displayTime{};uint64_t at{};};std::array<Timing,8>timings{};std::array<Timing,32>outputTimings{};size_t timingCursor=0,outputTimingCursor=0;
 std::atomic<uint64_t>outputAgeSamples=0,outputAgeUs=0,outputAgeMaxUs=0;
 bool syntheticExpired(XrTime tag){
  const auto period=physicalPeriod.load();
  if(!expireSynthetic||period<2000000||period>50000000)return false;
  if(!hostTiming.renderEndExpired(tag,cpuMicros(),uint64_t(period)/1000))return false;
  if constexpr(collectStudyDiagnostics)++expiredSynthetic;
  return true;
 }
 void observeOutput(XrTime time){if constexpr(collectStudyDiagnostics){if(hostTimingEnabled)hostTiming.accepted(time,cpuMicros());++nativeEnds;uint64_t at=0;{std::lock_guard lock(timingMutex);for(auto&v:outputTimings)if(v.displayTime==time){at=v.at;break;}}if(at){++outputAgeSamples;addCpuMetric(outputAgeUs,outputAgeMaxUs,cpuMicros()-at);}}}
 std::atomic<uint64_t>applicationWorkSamples=0,applicationWorkUs=0,applicationWorkMaxUs=0;
 void markFrame(XrTime time){
  if(sourceCadence&&asyncSource&&sourceSingle&&profile==ProfileMode::full&&(generationEnabled||fixedSourceComparison)){
   std::lock_guard lock(sourceMutex);
   if(!sourceStopping)lastSourceReleaseUs=cpuMicros();
  }
  if constexpr(collectStudyDiagnostics){
   hostStage(time,runtime_study::timing::HostStage::frameStart);
   if(profile==ProfileMode::full&&!profileControl)return;
   std::lock_guard lock(timingMutex);auto&v=timings[timingCursor++%timings.size()];v={time,cpuMicros()};outputTimings[outputTimingCursor++%outputTimings.size()]=v;
  }
 }
 void measureFrame(XrTime time){if constexpr(collectStudyDiagnostics){if(profile==ProfileMode::full&&!profileControl)return;uint64_t at=0;{std::lock_guard lock(timingMutex);for(auto&v:timings)if(v.displayTime==time){at=v.at;v={};break;}}if(at){applicationWorkSamples++;addCpuMetric(applicationWorkUs,applicationWorkMaxUs,cpuMicros()-at);}}}
 XrResult deliver(const XrFrameEndInfo*frame){const auto at=studyClock();auto r=endpoint->end_frame(frame);if constexpr(collectStudyDiagnostics){deliveryCalls++;addCpuMetric(deliveryUs,deliveryMaxUs,cpuMicros()-at);}return r;}
 std::shared_ptr<Instance>instance;XrSession handle{};ComPtr<ID3D11Device>device;ComPtr<ID3D11DeviceContext>context;runtime_study::SharedContextLease contextState;
 std::recursive_mutex nativePoseMutex;
 std::unique_ptr<NativeFramePath>endpoint;
 std::atomic<uint64_t>readinessWaitUs=0,readinessWaitMaxUs=0,sourceHistoryResets=0,sequenceHistoryResets=0,projectionHistoryResets=0,newHistories=0;
 std::atomic<uint64_t>generationCalls=0,generationCpuUs=0,generationCpuMaxUs=0,materializeCalls=0,materializeCpuUs=0,materializeCpuMaxUs=0;
 std::atomic<uint64_t>captureEncodingScopes=0;
 std::atomic<uint64_t>applicationEnds=0;std::atomic<ULONGLONG>lastReportTick=0;bool liveReports=false;
 std::atomic<bool>running=false;std::atomic<int64_t>physicalPeriod{16'666'667};
 std::atomic<uint64_t>captureCandidates=0,missingGuides=0,rejectedGuides=0,generatedFrames=0,notReady=0,invalidDepth=0,preparedSources=0,prepareRejected=0,warmups=0,unavailable=0,geometrySkipped=0,outputSkipped=0,nativeEnds=0;
 std::unique_ptr<presentation_study::Materializer>materializer;
 std::unique_ptr<runtime_study::ColorFlowPipeline>colorPipeline;
#ifdef FRAMEGEN_NATIVE_D3D12
 std::unique_ptr<NativeD3D12Generation> d3d12;
#endif
 ComPtr<ID3D11DeviceContext4>colorContext;ComPtr<ID3D11Fence>colorSourceFence;uint64_t colorSourceSerial=0;std::atomic<bool>colorFault=false;
 struct ColorSourceHold{uint64_t fence;std::shared_ptr<NativeFramePath::LeasedFrame>source;};std::deque<ColorSourceHold>colorSources;
 std::mutex colorSourceMutex;std::condition_variable colorSourceChanged;std::thread colorRetirer;
 std::recursive_mutex colorOperationMutex;bool colorRetirerStopping=true;
 HANDLE colorRetirerEvent=nullptr;
 bool startColorRetirer(){
  if(colorRetirer.joinable())return true;if(!colorSourceFence)return false;
  if(!colorRetirerEvent)colorRetirerEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  if(!colorRetirerEvent)return false;
  {std::lock_guard lock(colorSourceMutex);colorRetirerStopping=false;}
  try{colorRetirer=std::thread([this]{
   for(;;){uint64_t marker=0;
    {std::unique_lock lock(colorSourceMutex);colorSourceChanged.wait(lock,[&]{return colorRetirerStopping||!colorSources.empty();});if(colorRetirerStopping)break;marker=colorSources.front().fence;}
    const auto completed=colorSourceFence->GetCompletedValue();
    if(completed==UINT64_MAX||marker==UINT64_MAX){colorFault=true;break;}
    if(completed>=marker){std::deque<ColorSourceHold>retired;
     {std::lock_guard lock(colorSourceMutex);while(!colorSources.empty()&&colorSources.front().fence!=UINT64_MAX&&colorSources.front().fence<=completed){retired.push_back(std::move(colorSources.front()));colorSources.pop_front();}}
     retired.clear();continue; // Destruct tokens outside our mutex and vendor locks.
    }
    if(FAILED(colorSourceFence->SetEventOnCompletion(marker,colorRetirerEvent))){colorFault=true;break;}
    auto waited=WaitForSingleObject(colorRetirerEvent,50);
    if(waited!=WAIT_OBJECT_0&&waited!=WAIT_TIMEOUT){colorFault=true;break;}
    {std::lock_guard lock(colorSourceMutex);if(colorRetirerStopping)break;}
   }
  });return true;}catch(...){std::lock_guard lock(colorSourceMutex);colorRetirerStopping=true;colorFault=true;return false;}
 }
 void stopColorRetirer(){
  {std::lock_guard lock(colorSourceMutex);colorRetirerStopping=true;}
  colorSourceChanged.notify_all();if(colorRetirer.joinable())colorRetirer.join();
 }
 HRESULT drainColorSources(){
  std::lock_guard operation(colorOperationMutex);if(!colorPipeline||!colorSourceFence)return S_OK;
  uint64_t marker=0;HRESULT result;
  {std::lock_guard gate(encoding());marker=++colorSourceSerial;result=colorContext->Signal(colorSourceFence.Get(),marker);if(SUCCEEDED(result))colorContext->Flush();}
  if(FAILED(result))return result;
  auto completed=colorSourceFence->GetCompletedValue();if(completed==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
  if(completed<marker){HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return HRESULT_FROM_WIN32(GetLastError());
   result=colorSourceFence->SetEventOnCompletion(marker,event);
   if(SUCCEEDED(result)){auto waited=WaitForSingleObject(event,2000);result=waited==WAIT_OBJECT_0?S_OK:waited==WAIT_TIMEOUT?HRESULT_FROM_WIN32(WAIT_TIMEOUT):HRESULT_FROM_WIN32(GetLastError());}
   CloseHandle(event);if(FAILED(result))return result;
  }
  std::deque<ColorSourceHold>retired;{std::lock_guard lock(colorSourceMutex);retired.swap(colorSources);}retired.clear();
  colorSourceChanged.notify_all();return S_OK;
 }
 uint64_t publication=0;

 HRESULT drainSourceReads(){
#ifdef FRAMEGEN_NATIVE_D3D12
  if(d3d12)return d3d12->flow.drainGpu();
#endif
  return drainColorSources();
 }

 xrfg::ContextEncodingGate&encoding(){return contextState->encoding;}
 void retireGpuOwners(){
  running=false;
  stopSourceWorker();stopColorRetirer();
  auto colorCompleted=drainSourceReads();
  if(endpoint)endpoint->stop();
  // Native stop proves only accepted publication work. A final producer marker
  // additionally covers work rejected before private queue admission.
  auto completed=colorCompleted;if(SUCCEEDED(completed)&&colorPipeline)completed=colorPipeline->drainGpu();
  if(FAILED(completed)){
   // Keep all unproven GPU owners and the encoder lease alive. GPU/device/event
   // failure is not a completion proof; freeing here would risk use-after-free.
   struct RetainedGpuOwners {
    runtime_study::SharedContextLease context;
    std::unique_ptr<presentation_study::Materializer>materializer;
    std::unique_ptr<NativeFramePath>endpoint;
    std::unique_ptr<runtime_study::ColorFlowPipeline>color;std::deque<ColorSourceHold>colorSources;
    ComPtr<ID3D11Fence>sourceFence;HANDLE retireEvent;
#ifdef FRAMEGEN_NATIVE_D3D12
    std::unique_ptr<NativeD3D12Generation>d3d12;
#endif
   };
   (void)new RetainedGpuOwners{contextState,std::move(materializer),std::move(endpoint),std::move(colorPipeline),std::move(colorSources),colorSourceFence,colorRetirerEvent
#ifdef FRAMEGEN_NATIVE_D3D12
    ,std::move(d3d12)
#endif
   };colorRetirerEvent=nullptr;
   std::fprintf(stderr,"FrameGen producer completion unproven (0x%08lx); GPU owners retained\n",(unsigned long)completed);
   return;
  }
  // Proxy destruction may wait for private ownership; never hold the encoder
  // gate across that protocol path. Its native calls take the shared gate.
  {std::deque<ColorSourceHold>retired;{std::lock_guard lock(colorSourceMutex);retired.swap(colorSources);}retired.clear();}
  if(colorRetirerEvent){CloseHandle(colorRetirerEvent);colorRetirerEvent=nullptr;}
  materializer.reset();
#ifdef FRAMEGEN_NATIVE_D3D12
  d3d12.reset();
#endif
  if(contextState){std::lock_guard lock(encoding());colorPipeline.reset();}else colorPipeline.reset();
  endpoint.reset();
 }
 ~Session(){if(managed)managedStatus.stop();retireGpuOwners();contextState.reset();}
};
bool startGeneration(Session&);
XrResult submitColorFrame(Session&,const XrFrameEndInfo*,std::shared_ptr<NativeFramePath::LeasedFrame> provided={},std::int64_t applicationEndNs=0,bool managedFrameReady=true);
#ifdef FRAMEGEN_NATIVE_D3D12
XrResult submitD3D12Frame(Session&,const std::shared_ptr<NativeFramePath::LeasedFrame>&,bool);
#endif

void writeReport(Session&){}
std::mutex mutex;std::unordered_map<XrInstance,std::shared_ptr<Instance>>instances;std::unordered_map<XrSession,std::shared_ptr<Session>>sessions;std::unordered_map<XrSwapchain,std::shared_ptr<Session>>chains;std::unordered_map<XrSpace,std::shared_ptr<Session>>spaceOwners;
template<class F>F proc(const Instance&i,const char*name){PFN_xrVoidFunction f=nullptr;return XR_SUCCEEDED(i.gipa(i.handle,name,&f))?reinterpret_cast<F>(f):nullptr;}
std::shared_ptr<Instance>instance(XrInstance handle){std::lock_guard l(mutex);auto i=instances.find(handle);return i==instances.end()?nullptr:i->second;}
std::shared_ptr<Session>session(XrSession handle){std::lock_guard l(mutex);auto i=sessions.find(handle);return i==sessions.end()?nullptr:i->second;}
std::shared_ptr<Session>chain(XrSwapchain handle){std::lock_guard l(mutex);auto i=chains.find(handle);return i==chains.end()?nullptr:i->second;}
XrResult XRAPI_CALL getProc(XrInstance,const char*,PFN_xrVoidFunction*);
#ifdef FRAMEGEN_NATIVE_D3D12
runtime_study::menu_framegen::AdmissionIssue initializeD3D12(Session& s,const XrGraphicsBindingD3D12KHR& binding){
 using runtime_study::menu_framegen::BlockReason;
 auto* device=binding.device;
 auto* queue=binding.queue;
 if(!device||!queue)return {BlockReason::dx12_queue,E_POINTER};
 const auto queueType=queue->GetDesc().Type;
 if(queueType!=D3D12_COMMAND_LIST_TYPE_DIRECT)return {BlockReason::dx12_queue,queueType};
 ComPtr<ID3D12Device> owner;
 auto hr=queue->GetDevice(IID_PPV_ARGS(&owner));
 if(FAILED(hr))return {BlockReason::dx12_owner,hr};
 if(!owner)return {BlockReason::dx12_owner,E_POINTER};
 // Identical typed pointers already prove ownership. D3DMetal's device can
 // return E_NOINTERFACE when asked for IUnknown even with a valid binding.
 if(owner.Get()!=device){
  ComPtr<IUnknown> deviceIdentity,ownerIdentity;
  hr=device->QueryInterface(IID_PPV_ARGS(&deviceIdentity));
  if(SUCCEEDED(hr))hr=owner.As(&ownerIdentity);
  if(FAILED(hr))return {BlockReason::dx12_owner,hr};
  if(!deviceIdentity||deviceIdentity.Get()!=ownerIdentity.Get())return {BlockReason::dx12_owner,E_INVALIDARG};
 }

 xrfg::PrivateOpenXrRuntime::Options options;
 options.real.instance=s.instance->handle;
 options.real.session=s.handle;
 options.device12=device;
 options.queue12=queue;
 options.producer_queue12=queue;
 options.capacity=2;
 options.director_enabled=false;
 options.uniform_output=true;
 options.source_frame_multiplier=1;
 options.reset_source_multiplier_on_start=true;
 options.preserve_source_display_time=s.instance->sourcePoseTags;
 options.native_wait_lookahead=s.instance->sourcePoseTags;
 options.accepted_synthetic_output=[session=&s]{session->managedSyntheticTick=GetTickCount64();};
 const auto& instance=*s.instance;
#define LOAD_D3D12(field,name) options.real.field=proc<PFN_##name>(instance,#name)
 LOAD_D3D12(wait,xrWaitFrame);
 LOAD_D3D12(begin,xrBeginFrame);
 LOAD_D3D12(end,xrEndFrame);
 LOAD_D3D12(create,xrCreateSwapchain);
 LOAD_D3D12(destroy,xrDestroySwapchain);
 LOAD_D3D12(enumerate,xrEnumerateSwapchainImages);
 LOAD_D3D12(acquire,xrAcquireSwapchainImage);
 LOAD_D3D12(wait_image,xrWaitSwapchainImage);
 LOAD_D3D12(release,xrReleaseSwapchainImage);
 LOAD_D3D12(to_qpc,xrConvertTimeToWin32PerformanceCounterKHR);
#undef LOAD_D3D12
 const auto& real=options.real;
 if(!real.wait||!real.begin||!real.end||!real.create||!real.destroy||!real.enumerate||
    !real.acquire||!real.wait_image||!real.release)return {BlockReason::dx12_dispatch};

 if(instance.sourcePoseTags){
  using QuadCompositor=runtime_study::D3D12QuadCompositor;
  using Image=xrfg::PrivateOpenXrRuntime::Options::D3D12LayerImage;
  const auto locate=proc<PFN_xrLocateSpace>(instance,"xrLocateSpace");
  auto compositor=std::make_shared<QuadCompositor>();
  if(locate&&SUCCEEDED(compositor->initialize(device,1))){
   options.compose_d3d12_quad=[compositor,locate,&s](ID3D12GraphicsCommandList* list,XrTime time,
       const XrCompositionLayerProjection& projection,const std::array<Image,2>& eyes,
       const XrCompositionLayerQuad& quad,const Image& overlay){
    auto pose=quad.pose;
    if(quad.space!=projection.space){
     constexpr auto required=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
     XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
     std::lock_guard lock(s.nativePoseMutex);
     if(XR_FAILED(locate(quad.space,projection.space,time,&location))||
        (location.locationFlags&required)!=required)return false;
     pose=QuadCompositor::composePose(location.pose,pose);
    }
    std::array<QuadCompositor::Eye,2> target;
    for(unsigned eye=0;eye<target.size();++eye){
     const auto& view=projection.views[eye];
     if(FAILED(compositor->prepareTargetFormat(eyes[eye].format)))return false;
     target[eye]={{eyes[eye].resource,eyes[eye].format,view.subImage.imageRect,view.subImage.imageArrayIndex},
                  view.pose,view.fov};
    }
    const QuadCompositor::Quad source{{overlay.resource,overlay.format,quad.subImage.imageRect,
        quad.subImage.imageArrayIndex},pose,quad.size,quad.eyeVisibility,quad.layerFlags,overlay.state};
    return compositor->record(list,0,source,target);
   };
  }
 }

 auto endpoint=std::make_unique<NativeFramePath>(
     std::move(options),proc<PFN_xrRequestExitSession>(instance,"xrRequestExitSession"),&s.nativePoseMutex);
 hr=endpoint->initialize(true);
 if(FAILED(hr)||!endpoint->metrics().asyncEnabled)return {BlockReason::dx12_presenter,hr};
 const auto capacity=endpoint->setBufferCapacity(1);
 if(capacity!=XR_SUCCESS)return {BlockReason::dx12_presenter,capacity};
 auto generation=std::make_unique<NativeD3D12Generation>(*endpoint);
 const auto issue=generation->flow.initialize(device,queue);
 if(issue.reason!=BlockReason::none)return issue;
 s.endpoint=std::move(endpoint);
 s.d3d12=std::move(generation);
 return {};
}
#endif
const XrBaseInStructure* sessionGraphicsBinding(const XrSessionCreateInfo& info) {
 auto binding=static_cast<const XrBaseInStructure*>(info.next);
 for(unsigned n=0;binding&&n<64;++n,binding=binding->next)
  if(binding->type==XR_TYPE_GRAPHICS_BINDING_D3D11_KHR||
     binding->type==XR_TYPE_GRAPHICS_BINDING_D3D12_KHR)return binding;
 return nullptr;
}
XrResult XRAPI_CALL createSession(XrInstance inst,const XrSessionCreateInfo*ci,XrSession*out){
 auto i=instance(inst);if(!i||!ci||!out)return XR_ERROR_HANDLE_INVALID;auto real=proc<PFN_xrCreateSession>(*i,"xrCreateSession");if(!real)return XR_ERROR_FUNCTION_UNSUPPORTED;auto r=real(inst,ci,out);if(XR_FAILED(r))return r;
 const auto* binding=sessionGraphicsBinding(*ci);
 const bool isD3D11=binding&&binding->type==XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
 const bool isD3D12=binding&&binding->type==XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
 auto* device11=isD3D11?reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(binding)->device:nullptr;
 bool canAttach=device11!=nullptr;
#ifdef FRAMEGEN_NATIVE_D3D12
 canAttach=canAttach||isD3D12;
#endif
 if(i->passThrough||(i->managed&&!canAttach)){
  auto s=std::make_shared<Session>();s->instance=i;s->handle=*out;s->managed=i->managed;
  s->managedD3D12=isD3D12;
  s->generationEnabled=false;
  s->managedInitIssue={runtime_study::menu_framegen::BlockReason::unsupported_graphics};
  std::lock_guard lock(mutex);sessions[*out]=std::move(s);
  return r;
 }
 auto s=std::make_shared<Session>();s->instance=i;s->handle=*out;
 if(i->managed){
  s->managed=true;
  s->managedD3D12=isD3D12;
  s->generationEnabled=false;
  s->profile=ProfileMode::full;s->profileControl=false;
  s->sourceSingle=true;s->nativeOne=true;s->sourceCreditEnabled=true;
  s->sourceCadence=true;s->cadenceUniform=true;s->expireSynthetic=true;s->hostTimingEnabled=false;
 }
#ifdef FRAMEGEN_NATIVE_D3D12
 if(s->managed&&isD3D12){
  try{s->managedInitIssue=initializeD3D12(*s,*reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(binding));}
  catch(...){s->managedInitIssue={runtime_study::menu_framegen::BlockReason::generation_init,E_UNEXPECTED};}
  std::lock_guard lock(mutex);sessions[*out]=std::move(s);
  return r;
 }
#endif
 if(s->profile==ProfileMode::native){char flag[8]{};if(device11&&GetEnvironmentVariableA("MOLTEN_FG_PROFILE_NATIVE_PROTECTION",flag,sizeof flag)&&flag[0]=='1'){s->device=device11;s->device->GetImmediateContext(&s->context);s->contextState=runtime_study::acquireContextState(s->context.Get());}std::lock_guard l(mutex);sessions[*out]=s;return r;}

 s->device=device11;
 if(s->device){
  s->device->GetImmediateContext(&s->context);
  s->contextState=runtime_study::acquireContextState(s->context.Get(),s->managed);
  if(!s->contextState||(s->managed&&!s->contextState->protection)){
   if(s->managed){
    using runtime_study::menu_framegen::BlockReason;
    auto reason=BlockReason::context_protection;
    if(!s->context)reason=BlockReason::context_missing;
    else if(!s->contextState)reason=BlockReason::context_identity;
    else{
     ComPtr<ID3D11Multithread> deviceProtection;
     if(SUCCEEDED(device11->QueryInterface(IID_PPV_ARGS(&deviceProtection))))
      reason=BlockReason::device_protection_only;
    }
    s->managedInitIssue={reason,int32_t(device11->GetCreationFlags())};
    s->contextState.reset();std::lock_guard l(mutex);sessions[*out]=s;return r;
   }
   if(auto cleanup=proc<PFN_xrDestroySession>(*i,"xrDestroySession"))cleanup(*out);
   return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  }
  xrfg::PrivateOpenXrRuntime::Options o;o.source_frame_multiplier=!s->managed&&s->profile==ProfileMode::full&&envEnabled("MOLTEN_FG_ENABLE")?2u:1u;o.uniform_output=!s->managed&&envEnabled("MOLTEN_FG_UNIFORM_OUTPUT");o.real.instance=inst;o.real.session=*out;o.device11=s->device.Get();o.context11=s->context.Get();o.capacity=2;o.director_enabled=false;o.d11_release_readiness=true;o.preserve_source_display_time=i->sourcePoseTags;o.native_wait_lookahead=i->sourcePoseTags;o.opaque_context_mutex=(i->sourcePoseTags||s->managed)?&s->encoding():nullptr;
  if(s->managed){
   o.uniform_output=s->cadenceUniform;
   o.reset_source_multiplier_on_start=true;
   o.accepted_synthetic_output=[raw=s.get()]{raw->managedSyntheticTick=GetTickCount64();};
  }
#define LOAD(field,name) o.real.field=proc<PFN_##name>(*i,#name)
  LOAD(wait,xrWaitFrame);LOAD(begin,xrBeginFrame);LOAD(end,xrEndFrame);LOAD(create,xrCreateSwapchain);LOAD(destroy,xrDestroySwapchain);LOAD(enumerate,xrEnumerateSwapchainImages);LOAD(acquire,xrAcquireSwapchainImage);LOAD(wait_image,xrWaitSwapchainImage);LOAD(release,xrReleaseSwapchainImage);LOAD(to_qpc,xrConvertTimeToWin32PerformanceCounterKHR);
#undef LOAD
  o.pace_synthetic_only=!s->managed&&s->cadenceUniform&&envEnabled("MOLTEN_FG_PACE_SYNTHETIC_ONLY");
  const bool earlyLinear=s->managed||envEnabled("MOLTEN_FG_EARLY_LINEAR_PUBLICATION");
  // OpenComposite switches composition between its menu and stereo frames.
  // Use the runtime's standard submission rather than bypassing its capture.
  const bool stockPublication=s->managed&&i->openComposite;
  o.molten_event_publication=!stockPublication&&i->sourcePoseTags&&s->cadenceUniform&&s->sourceSingle&&s->nativeOne&&
      !s->profileControl&&!o.pace_synthetic_only&&(s->managed||
      (!s->nativeApplicationColor&&envEnabled("MOLTEN_FG_ENABLE")&&envEnabled("MOLTEN_FG_FORWARD")&&
      (earlyLinear||envEnabled("MOLTEN_FG_EVENT_PUBLICATION"))));
  o.early_linear_publication=o.molten_event_publication&&earlyLinear;
  if(!s->managed&&s->fixedSourceComparison) {
   o.application_stage_stamps=[owner=s.get()](std::int64_t tag) {
    const auto stamps=owner->hostTiming.sourceStamps(tag);
    return xrfg::PrivateOpenXrRuntime::Options::ApplicationStageTiming{
     static_cast<std::int64_t>(stamps[0])*1000,static_cast<std::int64_t>(stamps[1])*1000};
   };
  }
  o.native_application_color=!s->managed&&s->nativeApplicationColor&&i->sourcePoseTags&&s->sourceSingle&&s->nativeOne&&
      !s->profileControl&&envEnabled("MOLTEN_FG_FORWARD")&&envEnabled("MOLTEN_FG_ENABLE");
  o.shared_forward_cadence=!s->managed&&s->sharedForwardCadence&&s->sourceCreditEnabled&&i->sourcePoseTags&&
      s->sourceSingle&&s->nativeOne&&!s->profileControl&&!s->nativeApplicationColor&&
      !o.pace_synthetic_only&&envEnabled("MOLTEN_FG_FORWARD")&&envEnabled("MOLTEN_FG_ENABLE");
  if(o.real.wait&&o.real.begin&&o.real.end&&o.real.create&&o.real.destroy&&o.real.enumerate&&o.real.acquire&&o.real.wait_image&&o.real.release){
   s->endpoint=std::make_unique<NativeFramePath>(std::move(o),proc<PFN_xrRequestExitSession>(*i,"xrRequestExitSession"),&s->nativePoseMutex);
   if(FAILED(s->endpoint->initialize(s->managed)))s->endpoint.reset();
  }
  if(s->managed){
   using runtime_study::menu_framegen::BlockReason;
   if(!s->endpoint||!s->endpoint->metrics().asyncEnabled||s->endpoint->setBufferCapacity(1)!=XR_SUCCESS)
    s->managedInitIssue={BlockReason::transport_init};
   else{
    // MoltenVR's stock capture rebuilds staging when the texture description
    // changes. Alternate only frames with matching color and image layout.
    s->matchSourceColor=i->sourcePoseTags&&!s->endpoint->eventPublicationEnabled();
    if(!startGeneration(*s))s->managedInitIssue={BlockReason::generation_init};
   }
   if(s->managedInitIssue.reason!=BlockReason::none){
    s->retireGpuOwners();
    s->contextState.reset();
   }
  }
 }
 {std::lock_guard l(mutex);sessions[*out]=s;}return r;
}
XrResult XRAPI_CALL beginSession(XrSession h,const XrSessionBeginInfo* info) {
 auto s=session(h);if(!s)return XR_ERROR_HANDLE_INVALID;
 auto real=proc<PFN_xrBeginSession>(*s->instance,"xrBeginSession");if(!real)return XR_ERROR_FUNCTION_UNSUPPORTED;
 const auto result=real(h,info);
 if(XR_FAILED(result))return result;
 if constexpr(collectStudyDiagnostics)s->liveReports=!s->managed&&envEnabled("MOLTEN_FG_LIVE_REPORT");
 if(s->managed){
  s->managedStereoReady=false;
  s->managedUnsupportedFrame=false;
  s->managedIssue.store({});
  s->managedIncompatibleFrames=0;
  s->managedSyntheticTick=0;
  s->managedRequested=false;
  s->managedRuntimeAvailable=false;
  s->generationEnabled=false;
 }
 if(!s->endpoint){s->running=true;s->startManagedStatus();return result;}
 const auto started=s->endpoint->start();
 if(XR_FAILED(started)){
  if(!s->managed)return started;
  {
   std::lock_guard lock(mutex);
   // The app may already own private swapchain images. Native rendering
   // cannot resume on those handles if the presenter itself failed to start.
   if(!s->managedColorChains.empty())return started;
  }
  s->managedInitIssue={runtime_study::menu_framegen::BlockReason::transport_start,started};
  s->retireGpuOwners();
  s->contextState.reset();
  s->running=true;
  s->startManagedStatus();
  return result;
 }
 s->running=true;
 if(s->managed){
  // The endpoint already runs at 1x. An unavailable optional source
  // worker or optical-flow pipeline leaves real frames on this endpoint.
  if(s->colorPipeline&&!s->colorFault){
   s->colorPipeline->requestReset();
   if(s->startColorRetirer())s->startSourceWorker(true);
  }
#ifdef FRAMEGEN_NATIVE_D3D12
  if(s->d3d12&&!s->colorFault){
   s->d3d12->flow.requestReset();
   s->d3d12->primed=false;
   s->startSourceWorker(true);
  }
#endif
  s->managedRuntimeAvailable=s->asyncSource.load();
  if(!s->managedRuntimeAvailable){
   s->managedInitIssue={runtime_study::menu_framegen::BlockReason::worker_start};
   s->sourceFailure=XR_SUCCESS;
  }
  s->startManagedStatus();
  return result;
 }
 if(s->nativeOne){
  if(s->profile!=ProfileMode::full||!s->endpoint->metrics().asyncEnabled)return XR_ERROR_FEATURE_UNSUPPORTED;
  auto mode=s->endpoint->setUniformOutput(s->cadenceUniform);if(mode!=XR_SUCCESS)return mode;
  mode=s->endpoint->setDirector(false);if(mode!=XR_SUCCESS)return mode;
  s->endpoint->setCopyOnly(false);
  mode=s->endpoint->setBufferCapacity(1);if(mode!=XR_SUCCESS)return mode;
 }
 if(envEnabled("MOLTEN_FG_ENABLE")){
  if(s->colorPipeline){
   if(s->colorFault)return XR_ERROR_RUNTIME_FAILURE;
   s->colorPipeline->requestReset();
   if(!s->startColorRetirer())return XR_ERROR_INITIALIZATION_FAILED;
  } else if(!startGeneration(*s)&&GetEnvironmentVariableA("MOLTEN_FG_BACKEND",nullptr,0)){
   return XR_ERROR_INITIALIZATION_FAILED;
  }
 }
 s->startSourceWorker(s->sourceSingle||s->nativeOne);
 return result;
}
XrResult XRAPI_CALL endSession(XrSession h){auto s=session(h);if(!s)return XR_ERROR_HANDLE_INVALID;if(s->running){s->running=false;if(s->managed)s->managedStatus.stop();s->stopSourceWorker();s->stopColorRetirer();if(FAILED(s->drainSourceReads()))s->colorFault=true;if(s->endpoint)s->endpoint->stop();writeReport(*s);}auto real=proc<PFN_xrEndSession>(*s->instance,"xrEndSession");return real?real(h):XR_ERROR_FUNCTION_UNSUPPORTED;}
XrResult XRAPI_CALL destroySession(XrSession h){auto s=session(h);if(!s)return XR_ERROR_HANDLE_INVALID;if(s->managed)s->managedStatus.stop();if(s->running||s->endpoint){s->running=false;s->stopSourceWorker();s->stopColorRetirer();writeReport(*s);s->retireGpuOwners();}auto real=proc<PFN_xrDestroySession>(*s->instance,"xrDestroySession");auto r=real?real(h):XR_ERROR_FUNCTION_UNSUPPORTED;if(XR_SUCCEEDED(r)){std::lock_guard l(mutex);for(auto it=chains.begin();it!=chains.end();)if(it->second==s)it=chains.erase(it);else ++it;for(auto it=spaceOwners.begin();it!=spaceOwners.end();)if(it->second==s)it=spaceOwners.erase(it);else ++it;sessions.erase(h);}return r;}
XrResult XRAPI_CALL waitFrame(XrSession h,const XrFrameWaitInfo*i,XrFrameState*out){auto s=session(h);if(!s)return XR_ERROR_HANDLE_INVALID;if(s->endpoint){if(XR_FAILED(s->sourceFailure.load()))return s->sourceFailure.load();const bool valid=out&&out->type==XR_TYPE_FRAME_STATE&&!out->next&&(!i||(i->type==XR_TYPE_FRAME_WAIT_INFO&&!i->next));if(valid){auto mode=s->pollGenerationToggle();if(mode!=XR_SUCCESS)return mode;auto credit=s->waitExistingSourceCredit();if(credit!=XR_SUCCESS)return credit;}auto r=s->endpoint->wait_frame(i,out);if(XR_SUCCEEDED(r)&&out){s->markFrame(out->predictedDisplayTime);if(out->predictedDisplayPeriod>0&&out->predictedDisplayPeriod<1'000'000'000)s->physicalPeriod=out->predictedDisplayPeriod/(std::max)(1u,s->endpoint->sourceMultiplier());}return r;}auto real=proc<PFN_xrWaitFrame>(*s->instance,"xrWaitFrame");auto r=real?real(h,i,out):XR_ERROR_FUNCTION_UNSUPPORTED;if(XR_SUCCEEDED(r)&&out)s->markFrame(out->predictedDisplayTime);return r;}
// MoltenVR's source-pose cache and native swapchain staging are shared state.
// Serialize source-pose queries with native delivery; native Wait stays separate.
XrResult XRAPI_CALL locateViews(XrSession h,const XrViewLocateInfo*i,XrViewState*state,uint32_t cap,uint32_t*count,XrView*out){auto s=session(h);if(!s)return XR_ERROR_HANDLE_INVALID;auto real=proc<PFN_xrLocateViews>(*s->instance,"xrLocateViews");if(!real)return XR_ERROR_FUNCTION_UNSUPPORTED;std::unique_lock<std::recursive_mutex> gate;if(s->contextState&&s->instance->sourcePoseTags)gate=std::unique_lock<std::recursive_mutex>(s->nativePoseMutex);auto result=real(h,i,state,cap,count,out);if(XR_SUCCEEDED(result)&&i)s->hostStage(i->displayTime,runtime_study::timing::HostStage::poseSample);return result;}
// Space handles referenced by queued layer metadata must outlive native End.
std::shared_ptr<Session>spaceSession(XrSpace h){std::lock_guard l(mutex);auto i=spaceOwners.find(h);return i==spaceOwners.end()?nullptr:i->second;}
XrResult XRAPI_CALL createReferenceSpace(XrSession h,const XrReferenceSpaceCreateInfo*i,XrSpace*out){auto s=session(h);if(!s)return XR_ERROR_HANDLE_INVALID;auto real=proc<PFN_xrCreateReferenceSpace>(*s->instance,"xrCreateReferenceSpace");if(!real)return XR_ERROR_FUNCTION_UNSUPPORTED;std::unique_lock<std::recursive_mutex> gate;if(s->contextState&&s->instance->sourcePoseTags)gate=std::unique_lock<std::recursive_mutex>(s->nativePoseMutex);auto r=real(h,i,out);gate=std::unique_lock<std::recursive_mutex>();if(XR_SUCCEEDED(r)&&out){std::lock_guard l(mutex);spaceOwners[*out]=s;}return r;}
XrResult XRAPI_CALL createActionSpace(XrSession h,const XrActionSpaceCreateInfo*i,XrSpace*out){auto s=session(h);if(!s)return XR_ERROR_HANDLE_INVALID;auto real=proc<PFN_xrCreateActionSpace>(*s->instance,"xrCreateActionSpace");if(!real)return XR_ERROR_FUNCTION_UNSUPPORTED;std::unique_lock<std::recursive_mutex> gate;if(s->contextState&&s->instance->sourcePoseTags)gate=std::unique_lock<std::recursive_mutex>(s->nativePoseMutex);auto r=real(h,i,out);gate=std::unique_lock<std::recursive_mutex>();if(XR_SUCCEEDED(r)&&out){std::lock_guard l(mutex);spaceOwners[*out]=s;}return r;}
XrResult XRAPI_CALL destroySpace(XrSpace h){auto s=spaceSession(h);if(!s)return XR_ERROR_HANDLE_INVALID;if(s->endpoint&&s->endpoint->metrics().asyncEnabled){auto jobs=s->drainSourceWorker();if(XR_FAILED(jobs))return jobs;auto gpu=s->drainSourceReads();if(FAILED(gpu))return XR_ERROR_RUNTIME_FAILURE;auto r=s->endpoint->drain();if(XR_FAILED(r))return r;}auto real=proc<PFN_xrDestroySpace>(*s->instance,"xrDestroySpace");if(!real)return XR_ERROR_FUNCTION_UNSUPPORTED;std::unique_lock<std::recursive_mutex> gate;if(s->contextState&&s->instance->sourcePoseTags)gate=std::unique_lock<std::recursive_mutex>(s->nativePoseMutex);auto r=real(h);gate=std::unique_lock<std::recursive_mutex>();if(XR_SUCCEEDED(r)){std::lock_guard l(mutex);spaceOwners.erase(h);}return r;}
XrResult XRAPI_CALL locateSpace(XrSpace h,XrSpace base,XrTime time,XrSpaceLocation*out){auto s=spaceSession(h);if(!s)return XR_ERROR_HANDLE_INVALID;auto real=proc<PFN_xrLocateSpace>(*s->instance,"xrLocateSpace");if(!real)return XR_ERROR_FUNCTION_UNSUPPORTED;std::unique_lock<std::recursive_mutex> gate;if(s->contextState&&s->instance->sourcePoseTags)gate=std::unique_lock<std::recursive_mutex>(s->nativePoseMutex);return real(h,base,time,out);}
XrResult XRAPI_CALL beginFrame(XrSession h,const XrFrameBeginInfo*i){auto s=session(h);if(!s)return XR_ERROR_HANDLE_INVALID;if(s->endpoint)return s->endpoint->begin_frame(i);auto real=proc<PFN_xrBeginFrame>(*s->instance,"xrBeginFrame");return real?real(h,i):XR_ERROR_FUNCTION_UNSUPPORTED;}
void Session::startSourceWorker(bool requested){
 bool ready=materializer&&colorPipeline;
#ifdef FRAMEGEN_NATIVE_D3D12
 ready=ready||bool(d3d12);
#endif
 if(sourceWorker.joinable()||!endpoint||!endpoint->metrics().asyncEnabled||!ready)return;
 char flag[8]{};if(!requested&&(!GetEnvironmentVariableA("MOLTEN_FG_ASYNC_SOURCE",flag,sizeof flag)||flag[0]!='1'))return;
 {std::lock_guard lock(sourceMutex);sourceCredit.start();sourcePhase.start(sourceCredit.epoch);lastSourceReleaseUs=0;sourceStopping=false;sourceActive=false;sourceFailure=XR_SUCCESS;endpoint->pauseDeferredAdmissions(false);}
 try{sourceWorker=std::thread([this]{
  sourceWorkerId=GetCurrentThreadId();
  for(;;){std::shared_ptr<SourceJob>job;runtime_study::SourceCredit::Ticket creditTicket;
   {std::unique_lock lock(sourceMutex);sourceChanged.wait(lock,[&]{return sourceStopping||!sourceQueue.empty();});if(sourceStopping)break;job=std::move(sourceQueue.front());sourceQueue.pop_front();sourceActive=true;creditTicket=job->sourceTicket;sourceCredit.activate(creditTicket);}
   sourceChanged.notify_all();XrResult result=XR_ERROR_RUNTIME_FAILURE;
   try{
#ifdef FRAMEGEN_NATIVE_D3D12
    if(d3d12)result=submitD3D12Frame(*this,job->frame,job->managedFrameReady);
    else
#endif
    result=submitColorFrame(*this,job->frame->info(),job->frame,0,job->managedFrameReady);
   }catch(...){result=XR_ERROR_RUNTIME_FAILURE;}
   // Dropping a source token may enter the vendor mutex. Never do it while
   // holding sourceMutex; drain observes idle only after this release finishes.
   job.reset();
   std::deque<std::shared_ptr<SourceJob>>abandoned;
   bool closing=false;{std::lock_guard lock(sourceMutex);sourceCredit.finish(creditTicket);sourcePhase.finish(creditTicket,cpuMicros());sourceActive=false;if(XR_FAILED(result)){sourceFailure=result;sourceStopping=true;abandoned.swap(sourceQueue);}closing=sourceStopping;}
   abandoned.clear();sourceChanged.notify_all();
   if constexpr(collectStudyDiagnostics)if(!closing&&liveReports){auto now=GetTickCount64();auto before=lastReportTick.load();if(now-before>=1000&&lastReportTick.compare_exchange_strong(before,now))writeReport(*this);}
   if(XR_FAILED(result))break;
  }
  {std::lock_guard lock(sourceMutex);sourceActive=false;}sourceWorkerId=0;sourceChanged.notify_all();
 });asyncSource=true;}catch(...){std::lock_guard lock(sourceMutex);sourceStopping=true;sourceActive=false;sourceFailure=XR_ERROR_RUNTIME_FAILURE;asyncSource=false;sourceChanged.notify_all();}
}
XrResult Session::drainSourceWorker(){
 // Internal Materializer chain resize is called by this worker. It must not
 // wait for itself; those chains are not application source handles in its queue.
 if(sourceWorkerId.load()==GetCurrentThreadId())return XR_SUCCESS;
 std::unique_lock lock(sourceMutex);sourceChanged.wait(lock,[&]{return XR_FAILED(sourceFailure.load())||(!sourceActive&&sourceQueue.empty());});
 return sourceFailure.load();
}
XrResult Session::pollGenerationToggle(){
 bool canGenerate=colorPipeline&&forwardPrediction;
#ifdef FRAMEGEN_NATIVE_D3D12
 canGenerate=canGenerate||bool(d3d12);
#endif
 if((!liveToggle&&!managed)||!endpoint||!canGenerate||!sourceCadence||!asyncSource)
  return XR_SUCCESS;
 std::lock_guard toggle(toggleMutex);
 const bool current=generationEnabled.load();
 bool next=!current;
 if(managed) {
  const auto requested=managedStatus.requested();
  if(requested!=managedRequested)managedIncompatibleFrames=0;
  managedRequested=requested;
  next=managedRequested&&managedStereoReady.load()&&!colorFault.load()&&managedIncompatibleFrames.load()<3;
#ifdef FRAMEGEN_NATIVE_D3D12
  if(d3d12)next=next&&d3d12->primed.load();
#endif
  if(next==current)return XR_SUCCESS;
 } else if(fixedSourceComparison) {
  const auto now=GetTickCount64();
  if(comparisonModeFile.empty()||now<nextComparisonPoll)return XR_SUCCESS;
  nextComparisonPoll=now+100;
  const auto file=CreateFileW(comparisonModeFile.c_str(),GENERIC_READ,
      FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)return XR_SUCCESS;
  char mode{};
  DWORD read{};
  const auto ok=ReadFile(file,&mode,1,&read,nullptr);
  CloseHandle(file);
  if(!ok||read!=1||(mode!='0'&&mode!='1')||(mode=='1')==current)return XR_SUCCESS;
 } else {
  DWORD foregroundProcess{};
  if(const auto window=GetForegroundWindow())GetWindowThreadProcessId(window,&foregroundProcess);
  const auto pressed=[](int key){return (GetAsyncKeyState(key)&0x8000)!=0;};
  const bool down=foregroundProcess==GetCurrentProcessId()&&pressed(VK_CONTROL)&&pressed(VK_SHIFT)&&pressed('G');
  const bool justPressed=down&&!toggleDown;
  toggleDown=down;
  if(!justPressed)return XR_SUCCESS;
 }

 // Preserve the one-slot publisher and finish old work before changing the
 // cadence. The first source after re-enabling starts fresh optical history.
 auto result=drainSourceWorker();if(result!=XR_SUCCESS)return result;
#ifdef FRAMEGEN_NATIVE_D3D12
 if(d3d12&&FAILED(drainSourceReads()))return XR_ERROR_RUNTIME_FAILURE;
#endif
 result=endpoint->drain();if(result!=XR_SUCCESS)return result;
 if(managed||!fixedSourceComparison){
  result=endpoint->setSourceMultiplier(next?2u:1u);if(result!=XR_SUCCESS)return result;
 }
 if(colorPipeline)colorPipeline->requestReset();
#ifdef FRAMEGEN_NATIVE_D3D12
 if(d3d12&&!next){d3d12->flow.requestReset();d3d12->primed=false;}
#endif
 {std::lock_guard lock(sourceMutex);lastSourceReleaseUs=0;}
 if(managed)managedSyntheticTick=0;
 generationEnabled=next;
 if(!managed){
  std::puts(next?"ETS2 AMD frame generation: ON":"ETS2 AMD frame generation: OFF");
  std::fflush(stdout);
 }
 return XR_SUCCESS;
}
void Session::stopSourceWorker(){
 std::deque<std::shared_ptr<SourceJob>>abandoned;
 {std::lock_guard lock(sourceMutex);sourceStopping=true;sourceCredit.cancel();if(endpoint)endpoint->pauseDeferredAdmissions(true);abandoned.swap(sourceQueue);asyncSource=false;}
 sourceChanged.notify_all();abandoned.clear();if(sourceWorker.joinable())sourceWorker.join();
}
XrResult Session::waitExistingSourceCredit(){
 if(!sourceCreditEnabled||!asyncSource||!sourceSingle||profile!=ProfileMode::full||!endpoint||endpoint->sourceMultiplier()!=2)return XR_SUCCESS;
 const auto phaseUnits=sourceCadence?0:sourcePhaseEighths.load();const auto at=studyClock();std::unique_lock lock(sourceMutex);
 const auto queued=sourceQueue.empty()?0:sourceQueue.back()->sourceTicket.ordinal;
 const auto target=sourceCredit.snapshot(queued);
 if(!target.ordinal){
  if constexpr(collectStudyDiagnostics)++sourceCreditEmpty;
  if(!sourceCadence)return XR_SUCCESS;
 }else{
  if constexpr(collectStudyDiagnostics)++sourceCreditWaits;
  sourceChanged.wait(lock,[&]{return sourceCredit.terminal(target,sourceStopping,XR_FAILED(sourceFailure.load()));});
  if constexpr(collectStudyDiagnostics)addCpuMetric(sourceCreditWaitUs,sourceCreditWaitMaxUs,cpuMicros()-at);
 }
 const auto stopped=[&]{return sourceStopping||XR_FAILED(sourceFailure.load())||target.epoch!=sourceCredit.epoch;};
 if(!stopped()){
  const auto p=physicalPeriod.load();const auto phaseAt=cpuMicros();
  const bool pacedSource=sourceCadence&&lastSourceReleaseUs&&p>=2'000'000&&p<=50'000'000;
  auto deadline=pacedSource?lastSourceReleaseUs+uint64_t(p)*2/1000
      :p>0?sourcePhase.deadline(target,uint64_t(p)/1000,phaseUnits):0;
  if(pacedSource&&sharedForwardCadence&&!managed) {
   const auto shared=endpoint->nextSourceReleaseNs();
   if(shared>0) {
    // Keep short corrections bounded when moving the producer onto the
    // presenter's phase. An elapsed deadline never holds a fresh render.
    const auto correction=(std::min)(uint64_t(p)/8000,uint64_t{1000});
    deadline=(std::max)(uint64_t(shared)/1000,deadline-correction);
    ++sharedPhaseStarts;
   }
  }
  if(deadline>phaseAt){
   if constexpr(collectStudyDiagnostics){if(sourceCadence)++sourceCadenceWaits;else ++sourcePhaseWaits;}
   while(!stopped()){
    const auto now=cpuMicros();
    if(now>=deadline)break;
    sourceChanged.wait_for(lock,std::chrono::microseconds(deadline-now));
   }
   if constexpr(collectStudyDiagnostics){
    if(sourceCadence)sourceCadenceWaitUs+=cpuMicros()-phaseAt;
    else sourcePhaseWaitUs+=cpuMicros()-phaseAt;
   }
  }
 }
 const auto result=XR_FAILED(sourceFailure.load())?sourceFailure.load():stopped()?XR_ERROR_SESSION_NOT_RUNNING:XR_SUCCESS;
 if constexpr(collectStudyDiagnostics)if(result!=XR_SUCCESS)++sourceCreditCanceled;
 return result;
}
XrResult Session::enqueueSource(const XrFrameEndInfo*frame,std::int64_t applicationEndNs,bool managedFrameReady){
 if(XR_FAILED(sourceFailure.load()))return sourceFailure.load();
 const bool single=sourceSingle.load();
 auto job=std::make_shared<SourceJob>();
 job->managedFrameReady=managedFrameReady;
 // Single-job opt-in owns the exact End-entry images/Begin before waiting.
 // A canceled admission destroys this local token outside sourceMutex.
 if(single){auto result=endpoint->defer_frame(frame,job->frame,applicationEndNs);if(result!=XR_SUCCESS)return result;}
 const auto at=studyClock();
 {std::unique_lock lock(sourceMutex);sourceChanged.wait(lock,[&]{return sourceStopping||(sourceQueue.empty()&&(!single||!sourceActive));});
  if constexpr(collectStudyDiagnostics)addCpuMetric(sourceQueueWaitUs,sourceQueueWaitMaxUs,cpuMicros()-at);
  if(sourceStopping)return XR_FAILED(sourceFailure.load())?sourceFailure.load():XR_ERROR_SESSION_NOT_RUNNING;
  // Claim the one worker slot under the same lock that passed admission.
  if(single){job->sourceTicket=sourceCredit.admit();sourceQueue.push_back(std::move(job));if constexpr(collectStudyDiagnostics){++sourceJobs;if(frame)hostStage(frame->displayTime,runtime_study::timing::HostStage::queueAdmission);}}}
 if(single){sourceChanged.notify_all();return XR_SUCCESS;}
 auto result=endpoint->defer_frame(frame,job->frame,applicationEndNs);if(XR_FAILED(result))return result;
 {std::lock_guard lock(sourceMutex);if(sourceStopping)return XR_FAILED(sourceFailure.load())?sourceFailure.load():XR_ERROR_SESSION_NOT_RUNNING;
  job->sourceTicket=sourceCredit.admit();sourceQueue.push_back(std::move(job));if constexpr(collectStudyDiagnostics){++sourceJobs;if(frame)hostStage(frame->displayTime,runtime_study::timing::HostStage::queueAdmission);}}
 sourceChanged.notify_all();return XR_SUCCESS;
}
// Color-only processing consumes Begin exactly once and retains the exact
// original images until its ordered GPU read marker completes.
XrResult submitColorFrame(Session&s,const XrFrameEndInfo*input,std::shared_ptr<NativeFramePath::LeasedFrame> provided,std::int64_t applicationEndNs,bool managedFrameReady){
 std::lock_guard operation(s.colorOperationMutex);
 auto forward=[&](const XrFrameEndInfo*f){
  if(!provided)return s.deliver(f);
  const auto at=studyClock();auto status=s.endpoint->end_deferred_output(f,*provided);
  if constexpr(collectStudyDiagnostics){++s.deliveryCalls;addCpuMetric(s.deliveryUs,s.deliveryMaxUs,cpuMicros()-at);}
  return status;
 };
 if(!s.colorPipeline||s.colorFault||s.profile!=ProfileMode::full||!s.generationEnabled)return forward(input);
 if(s.managed&&!managedFrameReady){
  s.colorPipeline->requestReset();
  return forward(input);
 }
 const XrCompositionLayerProjection*projection=nullptr;unsigned projections=0;
 if(input&&input->layers)for(unsigned i=0;i<input->layerCount;i++)if(input->layers[i]&&input->layers[i]->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION){projection=(const XrCompositionLayerProjection*)input->layers[i];++projections;}
 if(!input||input->next||projections!=1||!projection||projection->next||projection->viewCount!=2||!projection->views||projection->views[0].next||projection->views[1].next){s.colorPipeline->requestReset();return forward(input);}
 auto completed=s.colorSourceFence->GetCompletedValue();
 if(completed==UINT64_MAX){s.colorFault=true;return forward(input);}
 bool sourceCapacity=false;{std::lock_guard lock(s.colorSourceMutex);sourceCapacity=s.colorSources.size()<4;}
 if(!sourceCapacity){if constexpr(collectStudyDiagnostics)++s.notReady;s.colorPipeline->requestReset();return forward(input);}
 auto held=std::move(provided);XrResult result=XR_SUCCESS;if(!held)result=s.endpoint->defer_frame(input,held,applicationEndNs);
 if(XR_FAILED(result))return result;
 const auto*frame=held->info();projection=nullptr;for(unsigned i=0;i<frame->layerCount;i++)if(frame->layers[i]&&frame->layers[i]->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION)projection=(const XrCompositionLayerProjection*)frame->layers[i];
 auto deliver=[&](const XrFrameEndInfo*f,const std::shared_ptr<xrfg::FrameCopyReceipt>& receipt={},int64_t deadlineNs=0){
  const auto at=studyClock();auto status=s.endpoint->end_deferred_output(f,*held,receipt,deadlineNs);
  if constexpr(collectStudyDiagnostics){++s.deliveryCalls;addCpuMetric(s.deliveryUs,s.deliveryMaxUs,cpuMicros()-at);}
  return status;
 };
 bool originalAdmitted=false;
 auto finishOriginal=[&](){return originalAdmitted?XR_SUCCESS:deliver(frame);};
 auto fallback=[&](){
  if constexpr(collectStudyDiagnostics)++s.unavailable;
  if(s.managed){
   s.managedIssue.store({runtime_study::menu_framegen::BlockReason::color_source});
   ++s.managedIncompatibleFrames;
  }
  s.colorPipeline->requestReset();
  return finishOriginal();
 };
 if(!projection)return fallback();
 std::array<runtime_study::ColorFlowPipeline::Texture,2>source;
 std::array<int64_t,2> formats{};runtime_study::ColorFlowPipeline::Views views;
 const bool arrayStereo=s.managed&&s.instance->sourcePoseTags&&
     projection->views[0].subImage.swapchain==projection->views[1].subImage.swapchain;
 const bool matchSourceColor=s.matchSourceColor||arrayStereo;
 std::array<D3D11_TEXTURE2D_DESC,2> sourceDescriptions{};
 for(unsigned e=0;e<2;e++){
  views[e]=projection->views[e];
  source[e]=s.endpoint->source_image11(*held,views[e].subImage.swapchain,&formats[e]);
  if(!source[e])return fallback();
  // The presenter creates proxies from the runtime's original image desc.
  if(matchSourceColor)source[e]->GetDesc(&sourceDescriptions[e]);
 }
 uint64_t forwardDeadline=0,physicalPeriodUs=0;
 int64_t originalEnd=0;
 bool capturedSource=false,capturedNative=false;
 std::shared_ptr<xrfg::FrameCopyReceipt> originalCopy;
 if(s.forwardPrediction){
  const auto period=s.physicalPeriod.load();
  if(period<2000000||period>50000000)return fallback();
  physicalPeriodUs=uint64_t(period)/1000;
  forwardDeadline=s.hostTiming.renderEndDeadline(frame->displayTime,physicalPeriodUs);
  if(s.nativeApplicationColor){
   {
    std::lock_guard gate(s.encoding());
    capturedSource=s.colorPipeline->captureSource(source,views,frame->displayTime,projection->space,
        false,formats,int64_t(physicalPeriodUs)*1000);
   }
   if(!capturedSource)return fallback();
   // Include both history copies in the native release readiness marker.
   result=s.endpoint->protect_application_reads(*held,capturedNative);
   if(result!=XR_SUCCESS)return result;
  }
  originalCopy=std::make_shared<xrfg::FrameCopyReceipt>();
  const auto before=studyClock();
  result=deliver(frame,originalCopy);
  if(result!=XR_SUCCESS)return result;
  originalAdmitted=true;
  // The native copy must be submitted before flow can encode on the same
  // immediate context. Neither the encoding gate nor GPU completion is held here.
  const auto now=cpuMicros();
  if constexpr(collectStudyDiagnostics){
   const auto submitted=forwardDeadline>=physicalPeriodUs?forwardDeadline-physicalPeriodUs:0;
   const auto beforeAdmission=submitted&&before>=submitted?before-submitted:0;
   const auto reservation=uint64_t(originalCopy->reservation_wait_ns)/1000;
   ++s.forwardOriginalCount;
   if(capturedNative)++s.forwardNativeOriginals;
   s.forwardBeforeAdmissionUs+=beforeAdmission;
   s.forwardReservationUs+=reservation;
   if(now>=forwardDeadline){
    s.forwardLateBeforeAdmissionUs+=beforeAdmission;
    s.forwardLateReservationUs+=reservation;
   }
  }
  if(now>=forwardDeadline){
   s.expireForward(Session::ForwardExpiry::originalAdmission);return XR_SUCCESS;
  }
  if(capturedNative){
   const auto latest=forwardDeadline+physicalPeriodUs;
   originalEnd=originalCopy->wait_native_end_until(std::chrono::steady_clock::now()+
       std::chrono::microseconds(latest-now));
   if(!originalEnd){s.expireForward(Session::ForwardExpiry::originalCopy);return XR_SUCCESS;}
   forwardDeadline=(std::min)(latest,uint64_t(originalEnd)/1000+physicalPeriodUs);
  }else if(!originalCopy->wait_until(std::chrono::steady_clock::now()+
            std::chrono::microseconds(forwardDeadline-now))){
   s.expireForward(Session::ForwardExpiry::originalCopy);return XR_SUCCESS;
  }
 }
 if(matchSourceColor&&!s.materializer->selectSourceFormat(formats[0]))
  return finishOriginal();
 auto expired=[&](Session::ForwardExpiry stage){
  if(!s.forwardPrediction)return s.syntheticExpired(frame->displayTime);
  if(cpuMicros()<forwardDeadline)return false;
  s.expireForward(stage);return true;
 };
 if constexpr(collectStudyDiagnostics)++s.captureCandidates;
 runtime_study::ColorFlowPipeline::Result output;
 // Both policies submit in B's camera and preserve its source pose tag.
 auto target=views;bool generated=false,sourceSignalFailed=false;auto at=studyClock();
 {std::lock_guard gate(s.encoding());
  if(s.forwardPrediction&&expired(Session::ForwardExpiry::encoder))return XR_SUCCESS;
  if(capturedSource){
   generated=s.colorPipeline->dispatchCaptured(target,output);
  }else{
   generated=s.colorPipeline->generate(source,views,frame->displayTime,target,output,projection->space,false,formats,
       int64_t(physicalPeriodUs)*1000);
   auto marker=++s.colorSourceSerial;
   if(FAILED(s.colorContext->Signal(s.colorSourceFence.Get(),marker))){std::lock_guard lock(s.colorSourceMutex);s.colorSources.push_back({UINT64_MAX,held});s.colorFault=true;sourceSignalFailed=true;}
   else{std::lock_guard lock(s.colorSourceMutex);s.colorSources.push_back({marker,held});}s.colorContext->Flush();s.colorSourceChanged.notify_all();
  }
 }
 if constexpr(collectStudyDiagnostics){++s.generationCalls;addCpuMetric(s.generationCpuUs,s.generationCpuMaxUs,cpuMicros()-at);}
 if(sourceSignalFailed){
  if(s.managed)s.managedIncompatibleFrames=3;
  return finishOriginal();
 }
 if(!generated)return fallback();
 if(output.status==runtime_study::ColorFlowPipeline::Result::warmup){
  if constexpr(collectStudyDiagnostics)++s.warmups;
  if(s.forwardPrediction){
   std::array<UINT,2> widths{},heights{};
   for(unsigned eye=0;eye<2;eye++){
    widths[eye]=UINT(views[eye].subImage.imageRect.extent.width);
    heights[eye]=UINT(views[eye].subImage.imageRect.extent.height);
   }
   s.materializer->prime(widths,heights,arrayStereo);
  }
  return finishOriginal();
 }
 if(output.status!=runtime_study::ColorFlowPipeline::Result::generated)return fallback();
 if(expired(Session::ForwardExpiry::generation))return finishOriginal();
 auto storage=std::make_shared<presentation_study::GpuStorage>();for(unsigned e=0;e<2;e++)storage->texture[e]=output.textures[e];
 auto images=std::make_shared<presentation_study::GpuImages>();images->owner=storage;images->generation=++s.publication;images->orderedContext=s.context.Get();
 presentation_study::Plan plan;plan.sequence=s.publication;plan.kind=presentation_study::Kind::synthetic;plan.images=images;plan.content.space=(uintptr_t)projection->space;plan.content.sourceTime=frame->displayTime;
 for(unsigned e=0;e<2;e++){auto&v=target[e];plan.content.eyes[e].pose.q={v.pose.orientation.x,v.pose.orientation.y,v.pose.orientation.z,v.pose.orientation.w};plan.content.eyes[e].pose.p={v.pose.position.x,v.pose.position.y,v.pose.position.z};plan.content.eyes[e].fov={v.fov.angleLeft,v.fov.angleRight,v.fov.angleUp,v.fov.angleDown};}
 std::array<XrSwapchainSubImage,2>subimages;at=studyClock();
 const auto* expected=matchSourceColor?&sourceDescriptions:nullptr;
 bool made=s.materializer->materialize(plan,subimages,nullptr,nullptr,nullptr,nullptr,expected,arrayStereo);
 if constexpr(collectStudyDiagnostics){++s.materializeCalls;addCpuMetric(s.materializeCpuUs,s.materializeCpuMaxUs,cpuMicros()-at);}
 if(!made){
  if constexpr(collectStudyDiagnostics)++s.outputSkipped;
  if(s.managed){
   s.managedIssue.store({runtime_study::menu_framegen::BlockReason::color_output});
   ++s.managedIncompatibleFrames;
  }
  return finishOriginal();
 }
 if(s.managed)s.managedIncompatibleFrames=0;
 for(unsigned e=0;e<2;e++)target[e].subImage=subimages[e];
 XrCompositionLayerProjection layer=*projection;layer.views=target.data();std::vector<const XrCompositionLayerBaseHeader*>layers(frame->layers,frame->layers+frame->layerCount);
 for(auto&entry:layers)if(entry==(const XrCompositionLayerBaseHeader*)projection)entry=(const XrCompositionLayerBaseHeader*)&layer;
 if(expired(Session::ForwardExpiry::conversion))return finishOriginal();
 auto synthetic=*frame;synthetic.layers=layers.data();
 int64_t admissionDeadlineNs=0;
 if(s.forwardPrediction){
  // A ready forecast may still be waiting for the sole output slot while
  // real B is in native End. Use B's next refresh, capped at two periods
  // from source submission, so a real-frame stall never queues old pixels.
  const auto latest=capturedNative?forwardDeadline:forwardDeadline+physicalPeriodUs;
  const auto now=cpuMicros();
  if(!originalEnd&&now<latest)originalEnd=originalCopy->wait_native_end_until(
      std::chrono::steady_clock::now()+std::chrono::microseconds(latest-now));
  if(!originalEnd){s.expireForward(Session::ForwardExpiry::admission);return XR_SUCCESS;}
  constexpr int64_t preparationLeadNs=1'000'000;
  admissionDeadlineNs=(std::min)(int64_t(latest)*1000,
      originalEnd+int64_t(physicalPeriodUs)*1000-preparationLeadNs);
 }
 result=deliver(&synthetic,{},admissionDeadlineNs);
 if(s.forwardPrediction&&result==XR_TIMEOUT_EXPIRED){s.expireForward(Session::ForwardExpiry::admission);return XR_SUCCESS;}
 if(result!=XR_SUCCESS)return result;
 if constexpr(collectStudyDiagnostics){
  ++s.generatedFrames;
  if(s.forwardPrediction)++s.forwardSyntheticFrames;
  if(output.repeated)++s.repeatControlFrames;
  if(s.directSynthetic)++s.directSyntheticFrames;
 }
 return finishOriginal();
}
#ifdef FRAMEGEN_NATIVE_D3D12
XrResult submitD3D12Frame(Session& s,const std::shared_ptr<NativeFramePath::LeasedFrame>& held,
                        bool compatible){
 using runtime_study::menu_framegen::BlockReason;
 auto& generation=*s.d3d12;
 auto& endpoint=*s.endpoint;
 const auto* frame=held->info();
 generation.flow.reap();
 if(!compatible||!s.managedStatus.requested()||s.colorFault){
  generation.primed=false;
  generation.flow.requestReset();
  return endpoint.end_deferred_output(frame,*held);
 }

 // Submit the real frame first. Its copy must reach the binding queue before
 // optional synthesis can add GPU work on that same queue.
 const auto copy=std::make_shared<xrfg::FrameCopyReceipt>();
 const auto original=endpoint.end_deferred_output(frame,*held,copy);
 if(original!=XR_SUCCESS)return original;
 const auto skip=[&](BlockReason reason=BlockReason::none){
  s.managedIssue.store({reason});
  if(++s.managedIncompatibleFrames>=3)generation.primed=false;
  return XR_SUCCESS;
 };
 const auto physical=s.physicalPeriod.load();
 if(physical<2'000'000||physical>50'000'000)return skip();
 const auto periodUs=uint64_t(physical)/1000;
 const auto copyDeadline=s.hostTiming.renderEndDeadline(frame->displayTime,periodUs);
 const auto now=cpuMicros();
 if(now>=copyDeadline||!copy->wait_until(std::chrono::steady_clock::now()+
     std::chrono::microseconds(copyDeadline-now)))return skip();

 runtime_study::menu_framegen::AdmissionIssue issue;
 const auto* projection=runtime_study::menu_framegen::checkStereoProjection(frame,issue,s.allowsQuadLayers());
 if(!projection)return skip(issue.reason);
 std::array<xrfg::PrivateOpenXrRuntime::D3D12SourceImage,2> sources;
 std::array<XrCompositionLayerProjectionView,2> views;
 std::array<XrSwapchainSubImage,2> sourceSubImages;
 std::array<runtime_study::NativeD3D12Outputs::EyeDescription,2> descriptions;
 for(unsigned eye=0;eye<2;++eye){
  views[eye]=projection->views[eye];
  const auto& sub=views[eye].subImage;
  sourceSubImages[eye]=sub;
  auto source=endpoint.source_image12(held,sub.swapchain);
  if(!source)return skip(BlockReason::color_source);
  descriptions[eye]={uint32_t(sub.imageRect.extent.width),uint32_t(sub.imageRect.extent.height),
                     static_cast<DXGI_FORMAT>(source->declared_format)};
  sources[eye]=std::move(*source);
 }
 // MoltenVR's staging ring is keyed by the full resource descriptor.
 auto target=s.instance->sourcePoseTags?generation.outputs.acquire(descriptions,sourceSubImages):
     generation.outputs.acquire(descriptions);
 if(!target)return skip(BlockReason::color_output);
 std::array<runtime_study::D3D12ColorFlow::Output,2> outputs;
 for(unsigned eye=0;eye<2;++eye)
  outputs[eye]={target->eyes()[eye].texture.Get(),static_cast<DXGI_FORMAT>(target->eyes()[eye].declared_format),
                target->eyes()[eye].subImage};
 using Result=runtime_study::D3D12ColorFlow::Result;
 const auto result=generation.flow.generate(sources,views,frame->displayTime,projection->space,physical,outputs);
 if(result==Result::deviceLost){s.colorFault=true;generation.primed=false;return XR_SUCCESS;}
 if(result==Result::warmup){generation.primed=false;return XR_SUCCESS;}
 if(result!=Result::generated)return skip();
 generation.primed=true;
 // A successful 1× forecast primes history. The next WaitFrame may enter 2×.
 if(!s.generationEnabled){s.managedIncompatibleFrames=0;return XR_SUCCESS;}

 const auto latest=copyDeadline+periodUs;
 const auto afterGeneration=cpuMicros();
 if(afterGeneration>=latest)return skip();
 const auto originalEnd=copy->wait_native_end_until(std::chrono::steady_clock::now()+
     std::chrono::microseconds(latest-afterGeneration));
 if(!originalEnd)return skip();
 const auto deadline=(std::min)(int64_t(latest)*1000,originalEnd+physical);
 if(int64_t(cpuMicros())*1000>=deadline||!target->publish())return skip();
 for(unsigned eye=0;eye<2;++eye)
  views[eye].subImage=target->eyes()[eye].subImage;
 auto generated=*projection;
 generated.views=views.data();
 std::vector<const XrCompositionLayerBaseHeader*> layers(frame->layers,frame->layers+frame->layerCount);
 for(auto& layer:layers){
  if(layer==reinterpret_cast<const XrCompositionLayerBaseHeader*>(projection))
   layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&generated);
 }
 auto synthetic=*frame;
 synthetic.layers=layers.data();
 const auto submitted=endpoint.end_deferred_output(&synthetic,*held,{},deadline);
 if(submitted!=XR_SUCCESS)return skip();
 s.managedIncompatibleFrames=0;
 if constexpr(collectStudyDiagnostics)++s.generatedFrames;
 return XR_SUCCESS;
}
#endif
XrResult XRAPI_CALL endFrame(XrSession h,const XrFrameEndInfo*i){
 auto s=session(h);if(!s)return XR_ERROR_HANDLE_INVALID;
 bool managedFrameReady=true;
 if(s->managed&&s->endpoint){
  using namespace runtime_study::menu_framegen;
  AdmissionIssue issue;
  // An active event publisher fingerprints the MoltenVR build. Shared
  // stereo swapchains bypass it and use that build's stock RGB capture.
  const auto* projection=checkStereoProjection(i,issue,s->allowsQuadLayers(),
      !s->managedD3D12&&s->endpoint->eventPublicationEnabled());
  if(projection){
   const auto& left=projection->views[0].subImage;
   const auto& right=projection->views[1].subImage;
   const bool arrayStereo=!s->managedD3D12&&s->instance->sourcePoseTags&&left.swapchain==right.swapchain;
   const bool matchSourceColor=s->matchSourceColor||arrayStereo;
   std::lock_guard lock(mutex);
   int64_t firstEyeFormat=0;
   for(unsigned eye=0;eye<2;++eye){
    const auto& sub=projection->views[eye].subImage;
    const auto it=s->managedColorChains.find(sub.swapchain);
    if(it==s->managedColorChains.end()){
     issue={BlockReason::chain_missing,int32_t(eye)};
     break;
    }
    const auto& chain=it->second;
    if(!checkColor(chain,sub,issue))break;
    if(matchSourceColor){
     const auto& rect=sub.imageRect;
     if(!s->materializer->supportsSourceFormat(chain.format)||(eye&&chain.format!=firstEyeFormat)){
      issue={BlockReason::required_output,int32_t(chain.format)};
      break;
     }
     if(chain.mipCount!=1||chain.arraySize!=(arrayStereo?2u:1u)||
        (arrayStereo&&left.imageArrayIndex==right.imageArrayIndex)||
        rect.offset.x!=0||rect.offset.y!=0||
        rect.extent.width!=int32_t(chain.width)||rect.extent.height!=int32_t(chain.height)){
      issue={BlockReason::color_layout,int32_t(eye)};
      break;
     }
     firstEyeFormat=chain.format;
    }
   }
  }
  const bool supported=projection&&issue.reason==BlockReason::none;
  const auto previouslySupported=s->managedStereoReady.exchange(supported);
  if(supported&&!previouslySupported)s->managedIncompatibleFrames=0;
  if(!supported&&i&&i->layerCount)s->managedIssue=issue;
  s->managedUnsupportedFrame=i&&i->layerCount&&!supported;
  managedFrameReady=supported;
 }
 const auto applicationEndNs=s->endpoint?s->endpoint->application_end_stamp():0;
 if(i){s->hostStage(i->displayTime,runtime_study::timing::HostStage::renderEnd);s->measureFrame(i->displayTime);}
#ifdef FRAMEGEN_NATIVE_D3D12
 if(s->d3d12)return s->asyncSource?s->enqueueSource(i,applicationEndNs,managedFrameReady):s->endpoint->end_frame(i);
#endif
 if(s->endpoint&&s->colorPipeline)return s->asyncSource?s->enqueueSource(i,applicationEndNs,managedFrameReady):submitColorFrame(*s,i,{},applicationEndNs,managedFrameReady);
 if(s->endpoint)return s->asyncSource?s->enqueueSource(i,applicationEndNs):s->deliver(i);
 auto real=proc<PFN_xrEndFrame>(*s->instance,"xrEndFrame");const auto at=studyClock();auto r=real?real(h,i):XR_ERROR_FUNCTION_UNSUPPORTED;
 if constexpr(collectStudyDiagnostics){s->deliveryCalls++;addCpuMetric(s->deliveryUs,s->deliveryMaxUs,cpuMicros()-at);if(r==XR_SUCCESS)s->observeOutput(i?i->displayTime:0);}
 return r;
}
XrResult createChainImpl(XrSession h,const XrSwapchainCreateInfo*i,XrSwapchain*out,bool directSynthetic){auto s=session(h);if(!s||!out)return XR_ERROR_HANDLE_INVALID;XrResult r;if(s->endpoint)r=s->endpoint->create_swapchain(i,out,directSynthetic);else{auto real=proc<PFN_xrCreateSwapchain>(*s->instance,"xrCreateSwapchain");r=real?real(h,i,out):XR_ERROR_FUNCTION_UNSUPPORTED;}if(XR_SUCCEEDED(r)){if constexpr(collectStudyDiagnostics)if(i&&GetEnvironmentVariableA("MOLTEN_FG_LIVE_REPORT",nullptr,0))fprintf(stderr,"AMD-COLOR swapchain format=%lld extent=%ux%u array=%u samples=%u usage=%llu\n",(long long)i->format,i->width,i->height,i->arraySize,i->sampleCount,(unsigned long long)i->usageFlags);std::lock_guard l(mutex);chains[*out]=s;if(s->managed&&i){auto desc=*i;desc.next=nullptr;s->managedColorChains[*out]=desc;}}return r;}
XrResult XRAPI_CALL createChain(XrSession h,const XrSwapchainCreateInfo*i,XrSwapchain*out){return createChainImpl(h,i,out,false);}
XrResult XRAPI_CALL createDirectSyntheticChain(XrSession h,const XrSwapchainCreateInfo*i,XrSwapchain*out){return createChainImpl(h,i,out,true);}
XrResult XRAPI_CALL enumerateImages(XrSwapchain h,uint32_t cap,uint32_t*count,XrSwapchainImageBaseHeader*out){auto s=chain(h);if(!s)return XR_ERROR_HANDLE_INVALID;if(s->endpoint)return s->endpoint->enumerate_images(h,cap,count,out);auto real=proc<PFN_xrEnumerateSwapchainImages>(*s->instance,"xrEnumerateSwapchainImages");return real?real(h,cap,count,out):XR_ERROR_FUNCTION_UNSUPPORTED;}
XrResult XRAPI_CALL acquireImage(XrSwapchain h,const XrSwapchainImageAcquireInfo*i,uint32_t*out){auto s=chain(h);if(!s)return XR_ERROR_HANDLE_INVALID;if(s->endpoint)return s->endpoint->acquire_image(h,i,out);auto real=proc<PFN_xrAcquireSwapchainImage>(*s->instance,"xrAcquireSwapchainImage");return real?real(h,i,out):XR_ERROR_FUNCTION_UNSUPPORTED;}
XrResult XRAPI_CALL waitImage(XrSwapchain h,const XrSwapchainImageWaitInfo*i){auto s=chain(h);if(!s)return XR_ERROR_HANDLE_INVALID;if(s->endpoint)return s->endpoint->wait_image(h,i);auto real=proc<PFN_xrWaitSwapchainImage>(*s->instance,"xrWaitSwapchainImage");return real?real(h,i):XR_ERROR_FUNCTION_UNSUPPORTED;}
XrResult XRAPI_CALL releaseImage(XrSwapchain h,const XrSwapchainImageReleaseInfo*i){auto s=chain(h);if(!s)return XR_ERROR_HANDLE_INVALID;if(s->endpoint)return s->endpoint->release_image(h,i);auto real=proc<PFN_xrReleaseSwapchainImage>(*s->instance,"xrReleaseSwapchainImage");return real?real(h,i):XR_ERROR_FUNCTION_UNSUPPORTED;}
XrResult XRAPI_CALL destroyChain(XrSwapchain h){auto s=chain(h);if(!s)return XR_ERROR_HANDLE_INVALID;XrResult r;if(s->endpoint){auto jobs=s->drainSourceWorker();if(XR_FAILED(jobs))return jobs;if(FAILED(s->drainSourceReads()))return XR_ERROR_RUNTIME_FAILURE;r=s->endpoint->destroy_swapchain(h);}else{auto real=proc<PFN_xrDestroySwapchain>(*s->instance,"xrDestroySwapchain");r=real?real(h):XR_ERROR_FUNCTION_UNSUPPORTED;}if(XR_SUCCEEDED(r)){std::lock_guard l(mutex);chains.erase(h);s->managedColorChains.erase(h);}return r;}
bool startGeneration(Session&s){
 auto color=std::make_unique<runtime_study::ColorFlowPipeline>();
 std::string diagnostics;
 if(!color->initialize(s.device.Get(),s.context.Get(),stock_ofxr_shader,diagnostics,s.managed)){
  fprintf(stderr,"Color-only initialization failed: %s\n",diagnostics.c_str());
  return false;
 }
 ComPtr<ID3D11Device5>d5;
 if(FAILED(s.device.As(&d5))||FAILED(s.context.As(&s.colorContext))||
    FAILED(d5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.colorSourceFence))))return false;
 auto materializer=std::make_unique<presentation_study::Materializer>();presentation_study::ImageDispatch api;api.session=s.handle;api.formats=proc<PFN_xrEnumerateSwapchainFormats>(*s.instance,"xrEnumerateSwapchainFormats");
 s.directSynthetic=!s.managed&&s.sourceSingle&&s.nativeOne&&!s.profileControl&&envEnabled("MOLTEN_FG_DIRECT_SYNTH");
 if(s.managed||envEnabled("MOLTEN_FG_FORWARD")){
  if(!s.sourceSingle||!s.nativeOne||s.profileControl||s.directSynthetic){
   fprintf(stderr,"Forward prediction requires AMD, one source job, one native output, and direct output disabled\n");return false;
  }
  s.forwardPrediction=true;
 }
 api.create=s.directSynthetic?createDirectSyntheticChain:createChain;api.destroy=destroyChain;api.enumerate=enumerateImages;api.acquire=acquireImage;api.wait=waitImage;api.release=releaseImage;
 if(!materializer->initialize(s.device.Get(),s.context.Get(),api,&s.encoding(),s.matchSourceColor))return false;s.colorPipeline=std::move(color);s.materializer=std::move(materializer);if(!s.startColorRetirer()){s.colorFault=true;return false;}return true;
}
XrResult XRAPI_CALL destroyInstance(XrInstance h){auto i=instance(h);if(!i)return XR_ERROR_HANDLE_INVALID;auto real=proc<PFN_xrDestroyInstance>(*i,"xrDestroyInstance");if(!real)return XR_ERROR_FUNCTION_UNSUPPORTED;{std::lock_guard l(mutex);for(auto&[handle,s]:sessions)if(s->instance==i)return XR_ERROR_CALL_ORDER_INVALID;}auto r=real(h);if(XR_SUCCEEDED(r)){std::lock_guard l(mutex);instances.erase(h);}return r;}
XrResult XRAPI_CALL getProc(XrInstance h,const char*name,PFN_xrVoidFunction*out){if(!name||!out)return XR_ERROR_VALIDATION_FAILURE;*out=nullptr;
#define WRAP(api,fn) if(!strcmp(name,#api)){*out=(PFN_xrVoidFunction)fn;return XR_SUCCESS;}
 WRAP(xrGetInstanceProcAddr,getProc);
 auto i=instance(h);if(!i)return XR_ERROR_HANDLE_INVALID;
 WRAP(xrDestroyInstance,destroyInstance);
 if(i->passThrough){
  // Unsupported graphics still report an active XR session to the app.
  // Every rendering and input call continues directly to the next layer.
  WRAP(xrCreateSession,createSession);WRAP(xrBeginSession,beginSession);WRAP(xrEndSession,endSession);WRAP(xrDestroySession,destroySession);
  return i->gipa(h,name,out);
 }
 WRAP(xrCreateReferenceSpace,createReferenceSpace);WRAP(xrCreateActionSpace,createActionSpace);WRAP(xrDestroySpace,destroySpace);WRAP(xrLocateSpace,locateSpace);WRAP(xrLocateViews,locateViews);WRAP(xrCreateSession,createSession);WRAP(xrBeginSession,beginSession);WRAP(xrEndSession,endSession);WRAP(xrDestroySession,destroySession);WRAP(xrWaitFrame,waitFrame);WRAP(xrBeginFrame,beginFrame);WRAP(xrEndFrame,endFrame);WRAP(xrCreateSwapchain,createChain);WRAP(xrDestroySwapchain,destroyChain);WRAP(xrEnumerateSwapchainImages,enumerateImages);WRAP(xrAcquireSwapchainImage,acquireImage);WRAP(xrWaitSwapchainImage,waitImage);WRAP(xrReleaseSwapchainImage,releaseImage);
#undef WRAP
 return i->gipa(h,name,out);
}
XrResult XRAPI_CALL createInstance(const XrInstanceCreateInfo* info,const XrApiLayerCreateInfo* layer,XrInstance* out) {
 if(!info||!layer||!out||!layer->nextInfo||!layer->nextInfo->nextGetInstanceProcAddr||
     !layer->nextInfo->nextCreateApiLayerInstance)return XR_ERROR_INITIALIZATION_FAILED;
 auto next=*layer;next.nextInfo=layer->nextInfo->next;
 const auto result=layer->nextInfo->nextCreateApiLayerInstance(info,&next,out);
 if(XR_FAILED(result))return result;
 auto i=std::make_shared<Instance>();
 i->handle=*out;i->gipa=layer->nextInfo->nextGetInstanceProcAddr;
 i->managed=true;
 constexpr char openCompositeName[]="OpenComposite";
 i->openComposite=_strnicmp(info->applicationInfo.applicationName,openCompositeName,
                           sizeof(openCompositeName)-1)==0;
 i->passThrough=!runtime_study::menu_framegen::supportsTransport(*info);
 if(!i->passThrough){
  XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
  if(auto props=proc<PFN_xrGetInstanceProperties>(*i,"xrGetInstanceProperties");props&&XR_SUCCEEDED(props(*out,&properties)))
   i->sourcePoseTags=strstr(properties.runtimeName,"MoltenVR")!=nullptr;
 }
 std::lock_guard lock(mutex);instances[*out]=std::move(i);
 return result;
}
}
extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader,const char* name,XrNegotiateApiLayerRequest* out) {
 if(!loader||!name||!out||loader->structType!=XR_LOADER_INTERFACE_STRUCT_LOADER_INFO||
    loader->structVersion!=XR_LOADER_INFO_STRUCT_VERSION||loader->structSize!=sizeof(*loader)||
    out->structType!=XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST||
    out->structVersion!=XR_API_LAYER_INFO_STRUCT_VERSION||out->structSize!=sizeof(*out)||
    loader->minInterfaceVersion>1||loader->maxInterfaceVersion<1||
    loader->minApiVersion>XR_CURRENT_API_VERSION||loader->maxApiVersion<XR_MAKE_VERSION(1,0,0))
  return XR_ERROR_INITIALIZATION_FAILED;
 if(strcmp(name,runtime_study::menu_framegen::layerName))return XR_ERROR_INITIALIZATION_FAILED;
 out->layerInterfaceVersion=1;
 out->layerApiVersion=(std::min)(loader->maxApiVersion,XR_CURRENT_API_VERSION);
 out->getInstanceProcAddr=getProc;
 out->createApiLayerInstance=createInstance;
 return XR_SUCCESS;
}
