#pragma once
#include <cstdint>
#include <algorithm>
namespace runtime_study {
// Admission/completion accounting only. The Session sourceMutex owns every
// access; these ordinals never authorize image reuse or GPU resource release.
struct SourceCredit {
 struct Ticket {uint64_t epoch{},ordinal{};};
 uint64_t epoch=1,next=0,completed=0,active=0;
 void start(){++epoch;completed=next;active=0;}
 void cancel(){++epoch;}
 Ticket admit(){return {epoch,++next};}
 void activate(Ticket t){if(t.epoch==epoch)active=t.ordinal;}
 void finish(Ticket t){if(t.epoch==epoch){completed=(std::max)(completed,t.ordinal);if(active==t.ordinal)active=0;}}
 Ticket snapshot(uint64_t queued=0)const{return {epoch,(std::max)(active,queued)};}
 bool terminal(Ticket t,bool stopping,bool failed)const{return stopping||failed||t.epoch!=epoch||completed>=t.ordinal;}
 bool canceled(Ticket t,bool stopping,bool failed)const{return stopping||failed||t.epoch!=epoch;}
};
}
