#pragma once
#include "xrfg/private_openxr_runtime.hpp"
#include <atomic>
#include <array>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include "molten_event_runtime.hpp"
#include "publication_timing_capture.hpp"
namespace runtime_study {
// Native frame transport. The Wait-only worker never touches D3D or EndFrame;
// the asynchronous OFXR delegate owns protected copies and native presentation.
// MOLTEN_FG_PREFETCH_WAIT=1 enables Wait-only prefetch.
// MOLTEN_FG_ASYNC_PRESENT=1 instead enables the leased OFXR presenter.
// Both unset/0 preserve synchronous native swapchains and frame forwarding.
inline uint64_t cpuMicros(){static const uint64_t frequency=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return (uint64_t)f.QuadPart;}();LARGE_INTEGER now{};QueryPerformanceCounter(&now);return (uint64_t)now.QuadPart/frequency*1000000ULL+(uint64_t)now.QuadPart%frequency*1000000ULL/frequency;}
inline void addCpuMetric(std::atomic<uint64_t>&total,std::atomic<uint64_t>&maximum,uint64_t value){total+=value;auto old=maximum.load();while(old<value&&!maximum.compare_exchange_weak(old,value)){};}
class NativeFramePath {
#ifdef FRAMEGEN_MENU_LAYER
 static constexpr bool collectStudyDiagnostics_=false;
#else
 static constexpr bool collectStudyDiagnostics_=true;
#endif
 using NativeAudit=void(__cdecl*)(bool);NativeAudit nativeAudit_=nullptr;std::recursive_mutex*poseMutex_=nullptr;std::atomic<bool>copyOnly_{false};
 xrfg::PrivateOpenXrRuntime::Options options_;PFN_xrRequestExitSession requestExit_{};
 std::unique_ptr<xrfg::PrivateOpenXrRuntime>async_;bool asyncRunning_=false,routerRegistered_=false;
 std::unique_ptr<MoltenEventRuntime>eventPublisher_;
 std::unique_ptr<PublicationTimingCapture>publicationTiming_;
 bool traceApplicationRelease_=false;
 std::atomic<bool>asyncEnabled_{false};
 inline static std::mutex routerMutex_;
 inline static std::unordered_map<XrSession,NativeFramePath*>routers_;
 static NativeFramePath*lookup(XrSession h){std::lock_guard lock(routerMutex_);auto i=routers_.find(h);return i==routers_.end()?nullptr:i->second;}
 bool registerRouter(){std::lock_guard lock(routerMutex_);auto [it,inserted]=routers_.emplace(options_.real.session,this);if(!inserted&&it->second!=this)return false;routerRegistered_=true;return true;}
 void unregisterRouter(){if(!routerRegistered_)return;std::lock_guard lock(routerMutex_);auto it=routers_.find(options_.real.session);if(it!=routers_.end()&&it->second==this)routers_.erase(it);routerRegistered_=false;}
 static XrResult XRAPI_CALL routeWait(XrSession h,const XrFrameWaitInfo*i,XrFrameState*out){auto*self=lookup(h);return self?self->timedWait(i,out):XR_ERROR_HANDLE_INVALID;}
 static XrResult XRAPI_CALL routeEnd(XrSession h,const XrFrameEndInfo*i){auto*self=lookup(h);return self?self->nativeEnd(i):XR_ERROR_HANDLE_INVALID;}
 std::mutex waitMutex_;std::condition_variable jobChanged_,waitChanged_;std::thread waitWorker_;
 bool stopping_=false,job_=false,active_=false,ready_=false,workerExited_=true,deliveredWait_=false,begun_=false;
 XrFrameState result_{XR_TYPE_FRAME_STATE};XrResult waitResult_=XR_SUCCESS,failure_=XR_SUCCESS;XrTime prediction_{},begunPrediction_{};
 std::atomic<bool>prefetchEnabled_{false};
 std::atomic<uint64_t>endGateUs_=0,endGateMaxUs_=0;
 std::atomic<int64_t>lastNativeEndEntryNs_=0;
 bool spacingDiagnostics_=false;
 std::atomic<uint64_t>endSpacingSamples_=0,endSpacingUs_=0,endSpacingMaxUs_=0,endSpacingMinUs_=UINT64_MAX;
 std::array<std::atomic<uint64_t>,4>endSpacingBuckets_{};
 std::atomic<uint64_t>nativeWaitCalls_=0,nativeWaitUs_=0,nativeWaitMaxUs_=0,nativeEndCalls_=0,nativeEndUs_=0,nativeEndMaxUs_=0,consumeCalls_=0,consumeWaitUs_=0,consumeWaitMaxUs_=0,readyHits_=0,stopStalls_=0;
 XrResult timedWait(const XrFrameWaitInfo*info,XrFrameState*out){
  const auto at=collectStudyDiagnostics_?cpuMicros():0;
  if constexpr(collectStudyDiagnostics_)if(options_.trace)options_.trace(100,nativeWaitCalls_.load()+1,at,0);
  XrResult result;
  try{result=options_.real.wait(options_.real.session,info,out);}catch(...){result=XR_ERROR_RUNTIME_FAILURE;}
  if constexpr(collectStudyDiagnostics_){
   if(options_.trace)options_.trace(101,nativeWaitCalls_.load()+1,cpuMicros(),result);
   ++nativeWaitCalls_;addCpuMetric(nativeWaitUs_,nativeWaitMaxUs_,cpuMicros()-at);
  }
  return result;
 }
 XrResult nativeWait(XrFrameState*out){XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};return timedWait(&info,out);}
 XrResult nativeEnd(const XrFrameEndInfo*info){
  std::unique_lock<std::recursive_mutex> pose;if(poseMutex_)pose=std::unique_lock<std::recursive_mutex>(*poseMutex_);
  const auto gateAt=collectStudyDiagnostics_?cpuMicros():0;
  std::unique_lock<xrfg::ContextEncodingGate> encoding;if(options_.opaque_context_mutex&&!copyOnly_.load())encoding=std::unique_lock<xrfg::ContextEncodingGate>(*options_.opaque_context_mutex);
  const auto at=cpuMicros();
  if constexpr(collectStudyDiagnostics_){
   const auto previousEntry=lastNativeEndEntryNs_.exchange(int64_t(at)*1000,std::memory_order_relaxed);
   if(spacingDiagnostics_&&previousEntry>0&&int64_t(at)*1000>=previousEntry){
    const auto spacing=uint64_t(int64_t(at)-previousEntry/1000);++endSpacingSamples_;
    addCpuMetric(endSpacingUs_,endSpacingMaxUs_,spacing);auto minimum=endSpacingMinUs_.load();
    while(minimum>spacing&&!endSpacingMinUs_.compare_exchange_weak(minimum,spacing)){}
    ++endSpacingBuckets_[spacing<8000?0:spacing<12000?1:spacing<13000?2:3];
   }
   addCpuMetric(endGateUs_,endGateMaxUs_,at-gateAt);
   if(options_.trace)options_.trace(200,nativeEndCalls_.load()+1,at,info?info->displayTime:0);
   if(nativeAudit_)nativeAudit_(true);
  }else{
   lastNativeEndEntryNs_.store(int64_t(at)*1000,std::memory_order_relaxed);
  }
  const auto result=options_.real.end(options_.real.session,info);
  if constexpr(collectStudyDiagnostics_){
   if(nativeAudit_)nativeAudit_(false);
   if(options_.trace)options_.trace(201,nativeEndCalls_.load()+1,cpuMicros(),result);
   ++nativeEndCalls_;addCpuMetric(nativeEndUs_,nativeEndMaxUs_,cpuMicros()-at);
  }
  return result;
 }
 uint64_t stageEvent(const XrFrameEndInfo& frame,const std::array<ID3D11Texture2D*,2>& eyes){
  std::scoped_lock pose(*poseMutex_);
  std::scoped_lock encode(*options_.opaque_context_mutex);
  return eventPublisher_->stage(frame,eyes);
 }
 XrResult endEvent(uint64_t token,const XrFrameEndInfo& frame,
                   xrfg::PrivateOpenXrRuntime::Options::RealPreparationTiming* timing,bool& contentSubmitted){
  if(options_.early_linear_publication){
   std::scoped_lock pose(*poseMutex_);
   std::scoped_lock encode(*options_.opaque_context_mutex);
   const auto preparation=token&&frame.layerCount
       ?eventPublisher_->earlyPreparation(token):MoltenEventWorker::Preparation{};
   if(timing&&preparation.finishedAt){
    timing->linear_fence_observed_ns=preparation.fenceAt*1000;
    timing->linear_ready_ns=preparation.finishedAt*1000;
    timing->linear_map_call_ns=preparation.mapCallUs*1000;
    timing->linear_map_gate_ns=preparation.mapGateUs*1000;
   }
   if(preparation.finishedAt){
    auto empty=frame;empty.layerCount=0;empty.layers=nullptr;
    const auto result=nativeEnd(&empty);
    contentSubmitted=result==XR_SUCCESS&&eventPublisher_->publishEarly(token);
    if(!contentSubmitted)eventPublisher_->finish(token,false,0);
    return result;
   }
   if(token)eventPublisher_->finish(token,false,0);
   eventPublisher_->resumeStock();
   return nativeEnd(&frame);
  }
  if(token&&frame.layerCount){
   auto empty=frame;empty.layerCount=0;empty.layers=nullptr;
   const auto result=nativeEnd(&empty);
   eventPublisher_->finish(token,result==XR_SUCCESS,uint64_t(lastNativeEndEntryNs_.load()/1000));
   return result;
  }
  if(token)eventPublisher_->finish(token,false,0);
  // Drain outside the driver and pose gates. The worker may need them
  // briefly to unmap its last image before stock becomes the sole writer.
  if(!eventPublisher_->drain())return XR_ERROR_RUNTIME_FAILURE;
  {
   std::scoped_lock pose(*poseMutex_);
   std::scoped_lock encode(*options_.opaque_context_mutex);
   eventPublisher_->resumeStock();
  }
  return nativeEnd(&frame);
 }
 void queueWaitLocked(){if(!stopping_&&!job_&&!active_&&!ready_&&XR_SUCCEEDED(failure_)){job_=true;jobChanged_.notify_one();}}
 void workerLoop()noexcept{
  for(;;){
   {std::unique_lock lock(waitMutex_);jobChanged_.wait(lock,[&]{return stopping_||job_;});if(stopping_)break;job_=false;active_=true;}
   XrFrameState frame{XR_TYPE_FRAME_STATE};auto r=nativeWait(&frame);
   {std::lock_guard lock(waitMutex_);active_=false;result_=frame;result_.next=nullptr;waitResult_=r;ready_=true;if(XR_FAILED(r))failure_=r;}
   waitChanged_.notify_all();
  }
  {std::lock_guard lock(waitMutex_);workerExited_=true;}waitChanged_.notify_all();
 }
 void closeBegunForStop(){XrTime time=0;{std::lock_guard lock(waitMutex_);if(!begun_)return;time=begunPrediction_;}XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};empty.displayTime=time;empty.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;auto r=nativeEnd(&empty);{std::lock_guard lock(waitMutex_);if(XR_SUCCEEDED(r))begun_=false;}}
public:
 struct Metrics{bool enabled,asyncEnabled,copyOnly;uint64_t waitCalls,waitUs,waitMaxUs,endCalls,endUs,endMaxUs,consumeCalls,consumeWaitUs,consumeWaitMaxUs,readyHits,stopStalls,endGateUs,endGateMaxUs,endSpacingSamples,endSpacingUs,endSpacingMinUs,endSpacingMaxUs;std::array<uint64_t,4>endSpacingBuckets;};
 Metrics metrics()const{return{prefetchEnabled_.load(),asyncEnabled_.load(),copyOnly_.load(),nativeWaitCalls_.load(),nativeWaitUs_.load(),nativeWaitMaxUs_.load(),nativeEndCalls_.load(),nativeEndUs_.load(),nativeEndMaxUs_.load(),consumeCalls_.load(),consumeWaitUs_.load(),consumeWaitMaxUs_.load(),readyHits_.load(),stopStalls_.load(),endGateUs_.load(),endGateMaxUs_.load(),endSpacingSamples_.load(),endSpacingUs_.load(),endSpacingMinUs_.load()==UINT64_MAX?0:endSpacingMinUs_.load(),endSpacingMaxUs_.load(),{endSpacingBuckets_[0].load(),endSpacingBuckets_[1].load(),endSpacingBuckets_[2].load(),endSpacingBuckets_[3].load()}};}
 explicit NativeFramePath(xrfg::PrivateOpenXrRuntime::Options o,PFN_xrRequestExitSession exit=nullptr,std::recursive_mutex*pose=nullptr):poseMutex_(pose),options_(std::move(o)),requestExit_(exit){
  if constexpr(collectStudyDiagnostics_){
   char spacing[8]{};spacingDiagnostics_=GetEnvironmentVariableA("MOLTEN_FG_END_SPACING",spacing,sizeof spacing)&&spacing[0]=='1';
   char flag[8]{};if(GetEnvironmentVariableA("MOLTEN_FG_PROFILE_NATIVE_AUDIT",flag,sizeof flag)&&flag[0]=='1'){wchar_t path[32768]{};if(GetEnvironmentVariableW(L"MOLTEN_FG_CAPTURE_DLL",path,32768)){auto module=GetModuleHandleW(path);nativeAudit_=module?(NativeAudit)GetProcAddress(module,"MFSourceNativePhaseV1"):nullptr;}}
  }
 }
 // Diagnostic opt-in only. Safe only for an audited runtime whose EndFrame
 // uses resource copies/readback, never pipeline binding or drawing. The caller
 // must retain the ID3D11Multithread lease and the independent pose-cache mutex.
 void setCopyOnly(bool enabled){copyOnly_=enabled&&poseMutex_&&options_.opaque_context_mutex;}
 ~NativeFramePath(){stop();unregisterRouter();}
 HRESULT initialize(bool managed=false){
  char flag[8]{};if(!managed&&(!GetEnvironmentVariableA("MOLTEN_FG_ASYNC_PRESENT",flag,sizeof flag)||flag[0]!='1'))return S_OK;
  auto configured=options_;configured.capacity=2;configured.director_enabled=false;configured.d11_release_readiness=true;
  // Molten associates source poses with application LocateViews timestamps.
  // The consumer still owns a genuine native clock; keep the owned packet's
  // source timestamp when the caller selected that runtime-specific policy.
  // Preserve the caller-selected source-time policy; other runtimes retain
  // their normal native-slot displayTime behavior.
  configured.real.wait=routeWait;configured.real.end=routeEnd;
  configured.native_handoff_at=[this]{return lastNativeEndEntryNs_.load(std::memory_order_relaxed);};
#ifndef FRAMEGEN_MENU_LAYER
  char publicationTrace[8]{};
  if(configured.preserve_source_display_time &&
     GetEnvironmentVariableA("MOLTEN_FG_PUBLICATION_TRACE",publicationTrace,sizeof publicationTrace)==1 && publicationTrace[0]=='1') {
   auto capture=std::make_unique<PublicationTimingCapture>();
   char sourceRelease[8]{};
   traceApplicationRelease_=GetEnvironmentVariableA("MOLTEN_FG_SOURCE_RELEASE_TRACE",sourceRelease,sizeof sourceRelease)==1 && sourceRelease[0]=='1';
   char realPreparation[8]{};
   configured.trace_real_preparation=traceApplicationRelease_ && configured.early_linear_publication &&
       GetEnvironmentVariableA("MOLTEN_FG_TRACE_REAL_PREPARATION",realPreparation,sizeof realPreparation)==1 &&
       realPreparation[0]=='1';
   if(!capture->open(traceApplicationRelease_,configured.trace_real_preparation,
                    configured.trace_real_preparation&&bool(configured.application_stage_stamps)))
    return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
   publicationTiming_=std::move(capture);
   configured.publication_timing=[this](const auto& timing){publicationTiming_->observe(timing);};
   configured.trace_application_release=traceApplicationRelease_;
  }
#endif
  if(configured.molten_event_publication&&configured.opaque_context_mutex&&poseMutex_){
   const auto linear=configured.early_linear_publication;
   // Loading the cabin can stall the GPU longer than the normal fence fault
   // budget. Only the fixed-source diagnostic extends resource retention.
   // The per-frame deadline and fallback still apply while that fence is pending.
   const auto fenceWaitBudgetUs=configured.trace_real_preparation&&configured.application_stage_stamps?200'000u:70'000u;
   auto publisher=std::make_unique<MoltenEventRuntime>(configured.device11,configured.context11,
       configured.opaque_context_mutex,linear,false,false,linear,linear,fenceWaitBudgetUs);
   if(publisher->initialize(options_.real.end)){
    eventPublisher_=std::move(publisher);
    configured.stage_event_before_native_copy=linear;
    configured.stage_event=[this](const auto& frame,const auto& eyes){return stageEvent(frame,eyes);};
    configured.end_event=[this](auto token,const auto& frame,auto* timing,bool& contentSubmitted){
     return endEvent(token,frame,timing,contentSubmitted);
    };
    configured.cancel_event=[this](auto token){eventPublisher_->finish(token,false,0);};
    if constexpr(collectStudyDiagnostics_)std::fprintf(stderr,"AMD event publication: pinned MoltenVR runtime connected\n");
   }else std::fprintf(stderr,"AMD event publication: runtime fingerprint or device refused; using stock\n");
  }
  async_=std::make_unique<xrfg::PrivateOpenXrRuntime>(std::move(configured));
  auto result=async_->initialize();
  if(FAILED(result)){async_.reset();std::fprintf(stderr,"FrameGen asynchronous presenter unavailable; using synchronous path\n");return options_.device12?result:S_OK;}
  asyncEnabled_=true;return S_OK;
 }
 XrResult start(){
  if(async_){if(asyncRunning_)return XR_SUCCESS;if(!registerRouter())return XR_ERROR_RUNTIME_FAILURE;XrResult r;try{r=async_->start();}catch(...){async_->stop();unregisterRouter();return XR_ERROR_RUNTIME_FAILURE;}if(XR_SUCCEEDED(r))asyncRunning_=true;else unregisterRouter();return r;}
  if(waitWorker_.joinable())return XR_SUCCESS;prefetchEnabled_=false;
  char flag[8]{};if(!GetEnvironmentVariableA("MOLTEN_FG_PREFETCH_WAIT",flag,sizeof flag)||flag[0]!='1')return XR_SUCCESS;
  {std::lock_guard lock(waitMutex_);if(waitWorker_.joinable())return XR_SUCCESS;stopping_=false;job_=active_=ready_=deliveredWait_=begun_=false;workerExited_=false;failure_=XR_SUCCESS;}
  try{waitWorker_=std::thread([this]{workerLoop();});prefetchEnabled_=true;}catch(...){std::lock_guard lock(waitMutex_);workerExited_=true;prefetchEnabled_=false;std::fprintf(stderr,"MetalFX native Wait prefetch unavailable; using synchronous path\n");}
  return XR_SUCCESS;
 }
 void stop(){
  if(async_){
   async_->stop();
   if(eventPublisher_){
    if(!eventPublisher_->drain())eventPublisher_->shutdown();
    std::scoped_lock pose(*poseMutex_);
    std::scoped_lock encode(*options_.opaque_context_mutex);
    eventPublisher_->resumeStock();
   }
   asyncRunning_=false;unregisterRouter();return;
  }
  if(!waitWorker_.joinable())return;
  {std::lock_guard lock(waitMutex_);stopping_=true;job_=false;}jobChanged_.notify_all();waitChanged_.notify_all();
  // A conforming runtime returns the outstanding Wait after the prior cycle closes.
  // On an interrupted application frame, close that Begin first so it cannot
  // hold the worker forever. Never detach a thread retaining session pointers.
  bool exited=false;{std::unique_lock lock(waitMutex_);exited=waitChanged_.wait_for(lock,std::chrono::milliseconds(250),[&]{return workerExited_;});}
  if(!exited){stopStalls_++;closeBegunForStop();{std::unique_lock lock(waitMutex_);exited=waitChanged_.wait_for(lock,std::chrono::milliseconds(100),[&]{return workerExited_;});}if(!exited&&requestExit_)requestExit_(options_.real.session);}
  waitWorker_.join();
  closeBegunForStop();
  bool pending=false;XrTime time=0;{std::lock_guard lock(waitMutex_);pending=deliveredWait_||(ready_&&XR_SUCCEEDED(waitResult_));time=ready_?result_.predictedDisplayTime:prediction_;}
  if(pending){XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};auto r=options_.real.begin(options_.real.session,&begin);if(XR_SUCCEEDED(r)){std::lock_guard lock(waitMutex_);begun_=true;prediction_=begunPrediction_=time;deliveredWait_=false;}}
  closeBegunForStop();
  {std::lock_guard lock(waitMutex_);job_=active_=ready_=deliveredWait_=begun_=false;}
 }
 XrResult drain(){return async_?async_->drain():XR_SUCCESS;}
 void pauseDeferredAdmissions(bool paused)noexcept{if(async_)async_->pause_deferred_admissions(paused);}
 // Optional final-output scheduler only. Does not alter source pose tags,
 // the application's Wait/Begin sequence, or producer queue capacity.
 XrResult setDirector(bool enabled){return async_?async_->set_director_enabled(enabled):(enabled?XR_ERROR_FUNCTION_UNSUPPORTED:XR_SUCCESS);}
 bool directorEnabled()const noexcept{return async_&&async_->director_enabled();}
 XrResult setSourceMultiplier(uint32_t multiplier){return async_?async_->set_source_multiplier(multiplier):XR_ERROR_FUNCTION_UNSUPPORTED;}
 uint32_t sourceMultiplier()const noexcept{return async_?async_->source_multiplier():1u;}
 XrResult setUniformOutput(bool enabled){return async_?async_->set_uniform_output(enabled):XR_ERROR_FUNCTION_UNSUPPORTED;}
 bool uniformOutputEnabled()const noexcept{return async_&&async_->uniform_output_enabled();}
 bool eventPublicationEnabled()const noexcept{return eventPublisher_!=nullptr;}
 std::array<uint64_t,5> eventCounters()const noexcept{return eventPublisher_?eventPublisher_->counters():std::array<uint64_t,5>{};}
 int64_t nextSourceReleaseNs()const noexcept{return async_?async_->next_source_release_ns():0;}
 XrResult setBufferCapacity(size_t capacity){return async_?async_->set_buffer_capacity(capacity):(capacity==2?XR_SUCCESS:XR_ERROR_FUNCTION_UNSUPPORTED);}
 size_t bufferCapacity()const noexcept{return async_?async_->buffer_capacity():0;}
 using LeasedFrame=xrfg::PrivateOpenXrRuntime::LeasedFrame;
 using D3D12SourceImage=xrfg::PrivateOpenXrRuntime::D3D12SourceImage;
 using D3D12OutputImages=xrfg::PrivateOpenXrRuntime::D3D12OutputImages;
 std::int64_t application_end_stamp() const noexcept {
  if(!traceApplicationRelease_)return 0;
  static const auto frequency=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return f.QuadPart;}();
  LARGE_INTEGER now{};QueryPerformanceCounter(&now);
  return now.QuadPart/frequency*1'000'000'000LL+now.QuadPart%frequency*1'000'000'000LL/frequency;
 }
 XrResult defer_frame(const XrFrameEndInfo*i,std::shared_ptr<LeasedFrame>&held,std::int64_t applicationEndNs=0){return async_?async_->defer_frame(i,held,applicationEndNs):XR_ERROR_FUNCTION_UNSUPPORTED;}
 Microsoft::WRL::ComPtr<ID3D11Texture2D> source_image11(const LeasedFrame&source,XrSwapchain h,std::int64_t*declared_format=nullptr)const{if(declared_format)*declared_format=0;return async_?async_->source_image11(source,h,declared_format):Microsoft::WRL::ComPtr<ID3D11Texture2D>{};}
 [[nodiscard]] std::optional<D3D12SourceImage> source_image12(
     const std::shared_ptr<const LeasedFrame>& source, XrSwapchain handle) const {
  return async_ ? async_->source_image12(source, handle) : std::nullopt;
 }
 XrResult create_output_swapchain12(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format, XrSwapchain* out) {
  return async_ ? async_->create_output_swapchain12(width, height, format, out) : XR_ERROR_FEATURE_UNSUPPORTED;
 }
 XrResult create_matching_output_swapchain12(XrSwapchain source, XrSwapchain* out) {
  return async_ ? async_->create_matching_output_swapchain12(source, out) : XR_ERROR_FEATURE_UNSUPPORTED;
 }
 [[nodiscard]] bool output_matches_source12(XrSwapchain output, XrSwapchain source) const {
  return async_ && async_->output_matches_source12(output, source);
 }
 XrResult try_destroy_output_swapchain12(XrSwapchain handle) {
  return async_ ? async_->try_destroy_output_swapchain12(handle) : XR_ERROR_FEATURE_UNSUPPORTED;
 }
 [[nodiscard]] std::optional<D3D12OutputImages> try_acquire_output_images12(
     const std::array<XrSwapchainSubImage, 2>& subImages) {
  return async_ ? async_->try_acquire_output_images12(subImages) : std::nullopt;
 }
 XrResult protect_application_reads(const LeasedFrame& source,bool& protectedNative){
  return async_?async_->protect_application_reads(source,protectedNative):XR_ERROR_FUNCTION_UNSUPPORTED;
 }
 XrResult end_deferred_output(const XrFrameEndInfo*i,const LeasedFrame&source,
   const std::shared_ptr<xrfg::FrameCopyReceipt>& copyReceipt={},int64_t admitBeforeNs=0){
  return async_?async_->end_deferred_output(i,source,copyReceipt,admitBeforeNs):XR_ERROR_FUNCTION_UNSUPPORTED;
 }

 XrResult wait_frame(const XrFrameWaitInfo*i,XrFrameState*out){
  if(async_)return async_->wait_frame(i,out);
  if(!prefetchEnabled_){auto at=cpuMicros();XrFrameWaitInfo def{XR_TYPE_FRAME_WAIT_INFO};auto r=options_.real.wait(options_.real.session,i?i:&def,out);nativeWaitCalls_++;addCpuMetric(nativeWaitUs_,nativeWaitMaxUs_,cpuMicros()-at);if(XR_SUCCEEDED(r)&&out)prediction_=out->predictedDisplayTime;return r;}
  if(!out||out->type!=XR_TYPE_FRAME_STATE||out->next||(i&&(i->type!=XR_TYPE_FRAME_WAIT_INFO||i->next)))return XR_ERROR_VALIDATION_FAILURE;
  auto at=cpuMicros();std::unique_lock lock(waitMutex_);if(deliveredWait_)return XR_ERROR_CALL_ORDER_INVALID;if(ready_)readyHits_++;queueWaitLocked();waitChanged_.wait(lock,[&]{return stopping_||ready_||XR_FAILED(failure_);});
  if(stopping_){consumeCalls_++;addCpuMetric(consumeWaitUs_,consumeWaitMaxUs_,cpuMicros()-at);return XR_ERROR_SESSION_NOT_RUNNING;}
  if(!ready_){consumeCalls_++;addCpuMetric(consumeWaitUs_,consumeWaitMaxUs_,cpuMicros()-at);return failure_;}
  auto frame=result_;auto r=waitResult_;ready_=false;if(XR_SUCCEEDED(r)){*out=frame;prediction_=frame.predictedDisplayTime;deliveredWait_=true;}lock.unlock();consumeCalls_++;addCpuMetric(consumeWaitUs_,consumeWaitMaxUs_,cpuMicros()-at);return r;
 }
 XrResult begin_frame(const XrFrameBeginInfo*i){if(async_)return async_->begin_frame(i);XrFrameBeginInfo def{XR_TYPE_FRAME_BEGIN_INFO};auto r=options_.real.begin(options_.real.session,i?i:&def);if(prefetchEnabled_&&XR_SUCCEEDED(r)){std::lock_guard lock(waitMutex_);deliveredWait_=false;begun_=true;begunPrediction_=prediction_;queueWaitLocked();}return r;}
 XrResult end_frame(const XrFrameEndInfo*i){if(async_)return async_->end_frame(i);if(!i)return XR_ERROR_VALIDATION_FAILURE;auto frame=*i;auto r=nativeEnd(&frame);if(prefetchEnabled_){std::lock_guard lock(waitMutex_);if(XR_SUCCEEDED(r))begun_=false;else{failure_=r;waitChanged_.notify_all();}}if(XR_SUCCEEDED(r)&&options_.accepted_output)options_.accepted_output(frame.displayTime);return r;}
 XrResult create_swapchain(const XrSwapchainCreateInfo*i,XrSwapchain*out,bool directSynthetic=false){
  if(async_)return async_->create_swapchain(i,out,D3D12_RESOURCE_STATE_RENDER_TARGET,3,XR_NULL_HANDLE,directSynthetic);
  if(directSynthetic)return XR_ERROR_FEATURE_UNSUPPORTED;
  return options_.real.create(options_.real.session,i,out);
 }
 XrResult destroy_swapchain(XrSwapchain h){if(async_)return async_->destroy_swapchain(h);return options_.real.destroy(h);}
 XrResult enumerate_images(XrSwapchain h,uint32_t cap,uint32_t*count,XrSwapchainImageBaseHeader*out){if(async_)return async_->enumerate_images(h,cap,count,out);return options_.real.enumerate(h,cap,count,out);}
 XrResult acquire_image(XrSwapchain h,const XrSwapchainImageAcquireInfo*i,uint32_t*out){if(async_)return async_->acquire_image(h,i,out);return options_.real.acquire(h,i,out);}
 XrResult wait_image(XrSwapchain h,const XrSwapchainImageWaitInfo*i){if(async_)return async_->wait_image(h,i);return options_.real.wait_image(h,i);}
 XrResult release_image(XrSwapchain h,const XrSwapchainImageReleaseInfo*i){if(async_)return async_->release_image(h,i);return options_.real.release(h,i);}
};

} // namespace runtime_study
