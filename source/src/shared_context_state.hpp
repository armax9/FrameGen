#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include "xrfg/context_encoding_gate.hpp"
namespace runtime_study {
// A strong registry plus explicit lease count makes final restoration atomic
// with reacquisition. An expired weak_ptr alone cannot serialize those edges.
template<class State>class ContextStateRegistry {
 struct Entry {const void*key{};std::unique_ptr<State>state;std::size_t owners{};};
 inline static std::mutex registryMutex_;
 inline static std::unordered_map<const void*,std::shared_ptr<Entry>>entries_;
public:
 class Lease {
  friend class ContextStateRegistry;
  std::shared_ptr<Entry>entry_;
  explicit Lease(std::shared_ptr<Entry>entry):entry_(std::move(entry)){}
 public:
  Lease()=default;
  Lease(const Lease&other){std::lock_guard lock(registryMutex_);entry_=other.entry_;if(entry_)++entry_->owners;}
  Lease(Lease&&other)noexcept:entry_(std::move(other.entry_)){}
  Lease&operator=(const Lease&other){if(this!=&other){Lease copy(other);swap(copy);}return*this;}
  Lease&operator=(Lease&&other)noexcept{if(this!=&other){reset();entry_=std::move(other.entry_);}return*this;}
  ~Lease(){reset();}
  void swap(Lease&other)noexcept{entry_.swap(other.entry_);}
  void reset()noexcept{auto entry=std::move(entry_);if(!entry)return;std::lock_guard lock(registryMutex_);if(--entry->owners==0){entry->state->restore();auto it=entries_.find(entry->key);if(it!=entries_.end()&&it->second==entry)entries_.erase(it);}}
  State*get()const noexcept{return entry_?entry_->state.get():nullptr;}
  State*operator->()const noexcept{return get();}
  explicit operator bool()const noexcept{return bool(entry_);}
 };
 template<class Factory>static Lease acquire(const void*key,Factory&&factory){
  if(!key)return{};std::lock_guard lock(registryMutex_);auto found=entries_.find(key);
  if(found!=entries_.end()){++found->second->owners;return Lease(found->second);}
  auto entry=std::make_shared<Entry>();entry->key=key;entry->state=factory();if(!entry->state)return{};entry->owners=1;entries_.emplace(key,entry);return Lease(std::move(entry));
 }
};
struct DeviceContextState {
private:
 struct ProtectionSetup {
  Microsoft::WRL::ComPtr<ID3D11Multithread> protection;
  BOOL original=FALSE;bool driverTransactions=false;
 };
 static ProtectionSetup setUpProtection(ID3D11DeviceContext*context,bool managed){
  ProtectionSetup setup;
  if(SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&setup.protection)))){
   setup.original=setup.protection->GetMultithreadProtected();
   setup.protection->SetMultithreadProtected(TRUE);
   char flag[8]{};const auto n=GetEnvironmentVariableA("MOLTEN_FG_DRIVER_TRANSACTIONS",flag,sizeof flag);
   setup.driverTransactions=managed||(n==1&&flag[0]=='1');
  }
  return setup;
 }
 static void driverEnter(void*owner)noexcept{static_cast<ID3D11Multithread*>(owner)->Enter();}
 static void driverLeave(void*owner)noexcept{static_cast<ID3D11Multithread*>(owner)->Leave();}
 DeviceContextState(Microsoft::WRL::ComPtr<IUnknown>canonical,ProtectionSetup setup)
  :identity(std::move(canonical)),protection(std::move(setup.protection)),originalProtection(setup.original),
   encoding(setup.driverTransactions?protection.Get():nullptr,&driverEnter,&driverLeave){}
public:
 Microsoft::WRL::ComPtr<IUnknown>identity;
 Microsoft::WRL::ComPtr<ID3D11Multithread>protection;
 BOOL originalProtection=FALSE;bool restored=false;
 // Declared last: the driver binding is destroyed before its retained owner.
 xrfg::ContextEncodingGate encoding;
 DeviceContextState(Microsoft::WRL::ComPtr<IUnknown>canonical,ID3D11DeviceContext*context,bool managed=false)
  :DeviceContextState(std::move(canonical),setUpProtection(context,managed)){}
 ~DeviceContextState(){restore();}
 void restore()noexcept{if(!restored){restored=true;if(protection)protection->SetMultithreadProtected(originalProtection);}}
};
using SharedContextLease=ContextStateRegistry<DeviceContextState>::Lease;
inline SharedContextLease acquireContextState(ID3D11DeviceContext*context,bool managed=false){
 if(!context)return{};Microsoft::WRL::ComPtr<IUnknown>identity;if(FAILED(context->QueryInterface(IID_PPV_ARGS(&identity))))return{};
 return ContextStateRegistry<DeviceContextState>::acquire(identity.Get(),[identity,context,managed]{return std::make_unique<DeviceContextState>(identity,context,managed);});
}
} // namespace runtime_study
