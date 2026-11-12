#include "../src/source_credit.hpp"
#include "../vendor/ofxr/include/xrfg/private_output_distributor.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <stdexcept>
using Credit=runtime_study::SourceCredit;using namespace std::chrono_literals;
void check(bool x,const char*m){if(!x)throw std::runtime_error(m);}
struct Harness{std::mutex mutex;std::condition_variable changed;Credit credit;bool stopping=false,failed=false;
 struct Result{bool canceled;uint64_t active,completed;};
 auto wait(Credit::Ticket target){return std::async(std::launch::async,[this,target]{std::unique_lock lock(mutex);changed.wait(lock,[&]{return credit.terminal(target,stopping,failed);});return Result{credit.canceled(target,stopping,failed),credit.active,credit.completed};});}
};
template<class F>auto finish(F&f){check(f.wait_for(500ms)==std::future_status::ready,"bounded gate completion/cancel");return f.get();}
template<class F>void blocked(F&f){check(f.wait_for(15ms)==std::future_status::timeout,"existing unfinished owned job must block gate");}
void checkSharedForwardCadence(){
 using Schedule=xrfg::SharedForwardCadence;
 constexpr std::int64_t period=13'888'889, start=1'000'000'000;
 constexpr auto releaseAfterReal=period-period/2;
 {
  Schedule schedule;
  check(schedule.deadline(start,period,false)==start,"first real frame starts immediately");
  check(schedule.delivered(start,start,period,false)==start+releaseAfterReal,"real handoff seeds application release");
  const auto synth=schedule.deadline(start+period/2,period,true);
  check(synth==start+period,"synthetic owns next refresh");
  check(schedule.delivered(synth,synth,period,true)==start+releaseAfterReal,"synthetic preserves the next application release");
  const auto real=schedule.deadline(synth,period,false);
  check(real==start+2*period,"next real owns the following refresh");
  check(schedule.delivered(real,real,period,false)==real+releaseAfterReal,"real advances the shared grid by two refreshes");
 }
 {
  Schedule schedule;
  check(schedule.delivered(start,start,period,false)>0,"seed for missed synthetic");
  const auto real=schedule.deadline(start+period,period,false);
  check(real==start+2*period,"missing synthetic leaves its slot empty");
  check(schedule.delivered(real,real,period,false)==real+releaseAfterReal,"missing synthetic keeps application phase");
 }
 {
  Schedule schedule;
  check(schedule.delivered(start,start,period,false)>0,"seed for positive wakeup jitter");
  constexpr std::int64_t jitter=600'000, correction=100'000;
  const auto synth=start+period;
  check(schedule.delivered(synth,synth+jitter,period,true)==start+releaseAfterReal,"small wakeup jitter keeps the nominal grid");
  const auto real=schedule.deadline(synth+jitter,period,false);
  check(real==start+2*period+jitter-correction,"next output recovers at most 100 microseconds");
  check(schedule.delivered(real,real,period,false)==start+2*period+releaseAfterReal,"bounded correction keeps the real release grid");
  check(schedule.deadline(real,period,true)==start+3*period+jitter-2*correction,"correction continues without bunching outputs");
 }
 {
  Schedule schedule;
  check(schedule.delivered(start,start,period,false)>0,"seed before stall");
  const auto resumed=start+6*period;
  check(schedule.deadline(resumed,period,false)==resumed,"late real frame hands off immediately");
  check(schedule.delivered(resumed,resumed,period,false)==resumed+releaseAfterReal,"stall re-anchors application phase once");
  check(schedule.deadline(resumed,period,true)==resumed+period,"recovery maintains refresh spacing");
  const auto lateSynthetic=resumed+3*period;
  check(schedule.delivered(lateSynthetic,lateSynthetic,period,true)==lateSynthetic-period/2,"late synthetic keeps its odd position in the new pair");
  check(schedule.deadline(lateSynthetic,period,false)==lateSynthetic+period,"real following a late synthetic cannot burst");
 }
 {
  Schedule schedule;
  check(schedule.delivered(start,start,period,true)==0,"a synthetic handoff cannot seed the clock");
  const auto first=start+period;
  check(schedule.delivered(first,first,period,false)>0,"first real seeds after synthetic");
  constexpr std::int64_t period90=11'111'111;
  const auto transition=first+3*period;
  check(schedule.deadline(transition,period90,false)==transition,"refresh-rate change ignores the previous nominal grid");
  check(schedule.delivered(transition,transition,period90,false)==transition+period90-period90/2,"new refresh rate seeds from its first real handoff");
  check(schedule.deadline(transition,period90,true)==transition+period90,"new period sets the synthetic slot");
  check(schedule.delivered(transition,transition,0,false)==0,"invalid period clears the release target");
  check(schedule.deadline(transition,period90,false)==transition,"next valid frame starts a fresh clock");
 }
}
int main(){try{
 {Credit c;c.start();auto empty=c.snapshot();check(empty.ordinal==0&&c.terminal(empty,false,false),"empty entry is immediately terminal");auto later=c.admit();c.activate(later);check(c.terminal(empty,false,false)&&!c.canceled(empty,false,false),"job admitted after empty snapshot cannot make it wait");}
 {Harness h;h.credit.start();auto old=h.credit.admit();h.credit.activate(old);auto target=h.credit.snapshot();auto f=h.wait(target);blocked(f);
  // Complete captured N-2 and make N-1 ACTIVE in ONE critical section, before
  // the waiter is notified. Waiting for !active instead would fail this test.
  Credit::Ticket newer;{std::lock_guard l(h.mutex);h.credit.finish(old);newer=h.credit.admit();h.credit.activate(newer);}h.changed.notify_all();auto r=finish(f);check(!r.canceled&&r.active==newer.ordinal&&r.completed==old.ordinal,"handover releases only captured old credit, not newer active job");}
 {Harness h;h.credit.start();auto old=h.credit.admit();h.credit.activate(old);auto target=h.credit.snapshot();auto f=h.wait(target);blocked(f);{std::lock_guard l(h.mutex);h.credit.finish(old);}h.changed.notify_all();auto r=finish(f);check(!r.canceled,"captured N-2 finishes without any future End/N-1 admission");}
 {Credit c;c.start();auto old=c.admit();c.activate(old);auto queued=c.admit();auto target=c.snapshot(queued.ordinal);check(target.ordinal==queued.ordinal,"snapshot includes already owned queued job");c.finish(old);check(!c.terminal(target,false,false),"finishing active predecessor cannot cover captured queued job");c.activate(queued);c.finish(queued);check(c.terminal(target,false,false),"captured queued job completion releases gate");}
 for(bool fail:{false,true}){Harness h;h.credit.start();auto old=h.credit.admit();h.credit.activate(old);auto f=h.wait(h.credit.snapshot());blocked(f);{std::lock_guard l(h.mutex);if(fail)h.failed=true;else{h.stopping=true;h.credit.cancel();}}h.changed.notify_all();auto r=finish(f);check(r.canceled,fail?"source failure cancels gate without completion":"stop cancels gate before joining worker");}
 {Harness h;h.credit.start();auto old=h.credit.admit();h.credit.activate(old);auto f=h.wait(h.credit.snapshot());blocked(f);Credit::Ticket fresh;uint64_t completed;
  // Stop/start can happen before a canceled waiter reacquires the mutex. The
  // old wait must still cancel, not follow an ordinal in the restarted run.
  {std::lock_guard l(h.mutex);h.stopping=true;h.credit.cancel();h.credit.start();h.stopping=false;fresh=h.credit.admit();h.credit.activate(fresh);completed=h.credit.completed;h.credit.finish(old);check(h.credit.completed==completed&&h.credit.active==fresh.ordinal,"stale completion cannot retire restarted active work");}h.changed.notify_all();auto r=finish(f);check(r.canceled&&r.active==fresh.ordinal,"epoch defeats fast stop/restart ABA even while new job active");}
 checkSharedForwardCadence();
 std::puts("PASS SharedForwardCadence CPU contract: paired slots, missing synthetic, bounded jitter, stall recovery, refresh change.");
 std::puts("PASS actual SourceCredit CPU contract: empty/future job, old-job snapshot, handover while newer active, queued target, stop/failure cancellation, restart ABA and stale completion. No graphics device or GPU.");return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
