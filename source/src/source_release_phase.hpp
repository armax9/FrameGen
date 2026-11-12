#pragma once
#include "source_credit.hpp"
#include <array>
#include <cstdint>
#include <limits>

namespace runtime_study {
// Protected by the Session source mutex. A later completion cannot postpone
// an earlier captured ticket's deadline. Missing history means no extra hold.
class SourceReleasePhase {
    struct Receipt { SourceCredit::Ticket ticket{}; uint64_t at{}; };
    std::array<Receipt, 8> receipts_{};
    uint64_t epoch_{};
    unsigned cursor_{};
public:
    void start(uint64_t epoch) { epoch_=epoch; cursor_=0; receipts_={}; }
    void finish(SourceCredit::Ticket ticket,uint64_t at) {
        if(ticket.epoch!=epoch_ || !ticket.ordinal || !at)return;
        for(const auto& receipt:receipts_)
            if(receipt.ticket.epoch==ticket.epoch && receipt.ticket.ordinal==ticket.ordinal)return;
        receipts_[cursor_++%receipts_.size()]={ticket,at};
    }
    uint64_t deadline(SourceCredit::Ticket ticket,uint64_t periodUs,unsigned eighths)const {
        if(ticket.epoch!=epoch_ || !ticket.ordinal || !eighths || eighths>4 ||
           periodUs<2000 || periodUs>50000)return 0;
        for(const auto& receipt:receipts_)
            if(receipt.ticket.epoch==ticket.epoch && receipt.ticket.ordinal==ticket.ordinal && receipt.at) {
                const auto offset=periodUs*eighths/8;
                return receipt.at<=std::numeric_limits<uint64_t>::max()-offset?receipt.at+offset:0;
            }
        return 0;
    }
};
}
