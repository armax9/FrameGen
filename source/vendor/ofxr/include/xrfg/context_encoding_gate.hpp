#pragma once
#include <mutex>

namespace xrfg {
// Exactly one recursive exclusion domain. When a driver binding is supplied,
// native application calls and our complete encoding bursts use the same lock;
// a second user mutex must not be nested around that driver critical section.
// The binding is immutable and its owner must outlive this gate.
class ContextEncodingGate final {
public:
    using DriverAction = void (*)(void*) noexcept;
private:
    struct Binding { void* owner{}; DriverAction enter{},leave{}; };
    const Binding binding_;
    std::recursive_mutex fallback_;
public:
    ContextEncodingGate() = default;
    ContextEncodingGate(void* owner,DriverAction enter,DriverAction leave) noexcept
        : binding_{owner&&enter&&leave?owner:nullptr,enter,leave} {}
    ContextEncodingGate(const ContextEncodingGate&)=delete;
    ContextEncodingGate& operator=(const ContextEncodingGate&)=delete;
    void lock() { if(binding_.owner)binding_.enter(binding_.owner);else fallback_.lock(); }
    void unlock() noexcept { if(binding_.owner)binding_.leave(binding_.owner);else fallback_.unlock(); }
    bool uses_driver() const noexcept { return binding_.owner!=nullptr; }
};
} // namespace xrfg
