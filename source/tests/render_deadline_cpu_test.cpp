#include "../src/host_timing.hpp"
#include <cassert>

using runtime_study::timing::HostStage;
using runtime_study::timing::HostTiming;

int main() {
    for (auto period : {13888ULL, 11111ULL}) {
        HostTiming<4> timing;
        constexpr auto origin = 100000ULL;
        assert(!timing.renderEndExpired(1, origin + period, period));
        timing.mark(1, HostStage::poseSample, origin - period);
        assert(!timing.renderEndExpired(1, origin + period, period));
        timing.mark(1, HostStage::renderEnd, origin);
        assert(!timing.renderEndExpired(1, origin + period - 1, period));
        assert(timing.renderEndExpired(1, origin + period, period));
        assert(timing.renderEndExpired(1, origin + 5 * period, period));
        for (int tag = 2; tag <= 4; ++tag) {
            const auto next = origin + (5 + tag) * period;
            timing.mark(tag, HostStage::renderEnd, next);
            assert(!timing.renderEndExpired(tag, next + period / 2, period));
        }
        assert(!timing.renderEndExpired(1, origin - 1, period));
        assert(!timing.renderEndExpired(1, origin + 10 * period, 0));
        timing.mark(5, HostStage::renderEnd, origin + 10 * period);
        assert(!timing.renderEndExpired(1, origin + 12 * period, period));
    }
}
