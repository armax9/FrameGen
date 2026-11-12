#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <mutex>

namespace runtime_study::timing {
// All host timestamps passed here are QPC converted to microseconds. A source
// tag is the original XrTime from application Wait/End, never a host timestamp.
using SourceTag=std::int64_t;
using HostMicros=std::uint64_t;
struct DurationStats {
    std::uint64_t count{},total{},maximum{};
    // Upper-bound histogram in milliseconds; bucket 255 includes >=255 ms.
    std::array<std::uint64_t,256> millisecondBuckets{};
    void add(HostMicros duration) noexcept {
        ++count;
        total=duration>std::numeric_limits<std::uint64_t>::max()-total?
            std::numeric_limits<std::uint64_t>::max():total+duration;
        if(duration>maximum)maximum=duration;
        ++millisecondBuckets[duration/1000<255?duration/1000:255];
    }
    double meanMicros() const noexcept {return count?double(total)/double(count):0.;}
};
enum class HostStage:unsigned { frameStart,poseSample,renderEnd,queueAdmission,count };
struct AcceptanceAge {
    SourceTag sourceTag{};
    std::uint64_t outputOrdinal{}; // 1=first accepted output for this source.
    HostMicros acceptedAt{};
    std::array<HostMicros,unsigned(HostStage::count)> age{};
    std::uint32_t validMask{};
};
struct HostTimingStats {
    std::uint64_t marks{},overwrites{},matchedOutputs{},unmatchedOutputs{},backwardsClock{};
    std::array<DurationStats,unsigned(HostStage::count)> allOutputs{},firstOutputs{},laterOutputs{};
};
template<std::size_t Capacity=128> class HostTiming {
    static_assert(Capacity>0);
    struct Entry {SourceTag tag{};bool occupied{};std::uint32_t mask{};
        std::array<HostMicros,unsigned(HostStage::count)> stamp{};std::uint64_t outputs{};};
    std::array<Entry,Capacity> entries_{};std::size_t cursor_{};
    mutable std::mutex mutex_;HostTimingStats stats_{};
public:
    // A repeated mark updates only that stage, leaving other stages unchanged.
    void mark(SourceTag tag,HostStage stage,HostMicros at) noexcept {
        if(unsigned(stage)>=unsigned(HostStage::count))return;
        std::lock_guard lock(mutex_);Entry*entry=nullptr;
        for(auto&v:entries_)if(v.occupied&&v.tag==tag){entry=&v;break;}
        if(!entry){entry=&entries_[cursor_++%Capacity];if(entry->occupied)++stats_.overwrites;*entry={};entry->occupied=true;entry->tag=tag;}
        entry->stamp[unsigned(stage)]=at;entry->mask|=1u<<unsigned(stage);++stats_.marks;
    }
    // Native acceptance is a host handoff, not GPU completion or photon time.
    // A source entry survives both generated and original output receipts.
    bool accepted(SourceTag originalSourceTag,HostMicros acceptedAt,AcceptanceAge*sample=nullptr) noexcept {
        std::lock_guard lock(mutex_);Entry*entry=nullptr;
        for(auto&v:entries_)if(v.occupied&&v.tag==originalSourceTag){entry=&v;break;}
        if(!entry){++stats_.unmatchedOutputs;return false;}
        AcceptanceAge result{};result.sourceTag=originalSourceTag;result.acceptedAt=acceptedAt;result.outputOrdinal=++entry->outputs;++stats_.matchedOutputs;
        for(unsigned i=0;i<unsigned(HostStage::count);++i)if(entry->mask&(1u<<i)){
            if(acceptedAt<entry->stamp[i]){++stats_.backwardsClock;continue;}
            result.age[i]=acceptedAt-entry->stamp[i];result.validMask|=1u<<i;
            stats_.allOutputs[i].add(result.age[i]);
            (result.outputOrdinal==1?stats_.firstOutputs:stats_.laterOutputs)[i].add(result.age[i]);
        }
        if(sample)*sample=result;return true;
    }
    HostMicros renderEndDeadline(SourceTag tag, HostMicros budget) const noexcept {
        if (!budget) return 0;
        std::lock_guard lock(mutex_);
        constexpr auto stage = unsigned(HostStage::renderEnd);
        for (const auto& entry : entries_) {
            if (entry.occupied && entry.tag == tag && (entry.mask & (1u << stage)))
                return entry.stamp[stage] + budget;
        }
        return 0;
    }
    bool renderEndExpired(SourceTag tag, HostMicros now, HostMicros budget) const noexcept {
        const auto deadline = renderEndDeadline(tag, budget);
        return deadline && now >= deadline;
    }
    std::array<HostMicros,2> sourceStamps(SourceTag tag) const noexcept {
        std::lock_guard lock(mutex_);
        for(const auto& entry:entries_)
            if(entry.occupied&&entry.tag==tag)
                return {entry.stamp[unsigned(HostStage::frameStart)],entry.stamp[unsigned(HostStage::poseSample)]};
        return {};
    }
    HostTimingStats snapshot()const noexcept{std::lock_guard lock(mutex_);return stats_;}
};

} // runtime_study::timing
