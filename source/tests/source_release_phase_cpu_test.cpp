// Actual production helpers, no graphics/runtime or system device calls.
#include "../src/source_release_phase.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <atomic>
using runtime_study::SourceCredit;using runtime_study::SourceReleasePhase;
void check(bool ok,const char*name){if(!ok){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
uint64_t us(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void values(){SourceCredit c;c.start();SourceReleasePhase p;p.start(c.epoch);auto target=c.admit();c.activate(target);check(p.deadline(target,13889,4)==0,"no timestamp invented before captured job completes");c.finish(target);p.finish(target,100000);
 for(unsigned rate:{72u,90u,120u}){const uint64_t P=((1000000000ull+rate/2)/rate)/1000;for(unsigned e=0;e<=4;++e)check(p.deadline(target,P,e)==(e?100000+P*e/8:0),"exact bounded eighth-period offset for72/90/120");}
 check(p.deadline(target,1999,4)==0&&p.deadline(target,50001,4)==0&&p.deadline(target,13889,5)==0,"invalid period/phase has zero extra hold");p.finish(target,999999);check(p.deadline(target,13889,4)==106944,"duplicate completion cannot postpone original deadline");auto later=c.admit();c.activate(later);c.finish(later);p.finish(later,500000);check(p.deadline(target,13889,4)==106944,"newer job completion cannot move captured earlier deadline");
 auto epoch=c.epoch;c.start();p.start(c.epoch);p.finish({epoch,target.ordinal},123456);check(p.deadline(target,13889,4)==0,"old epoch receipt cannot survive restart");auto fresh=c.admit();p.finish(fresh,1000);check(p.deadline(fresh,50000,4)==26000,"new epoch timestamp remains valid");SourceReleasePhase overflow;overflow.start(c.epoch);overflow.finish(fresh,UINT64_MAX-1);check(overflow.deadline(fresh,50000,4)==0,"overflow declines extra hold");std::puts("PASS exact deadlines, duplicate/newer/stale/overflow cases");}
void ring(){SourceCredit c;c.start();SourceReleasePhase p;p.start(c.epoch);auto oldest=c.admit();p.finish(oldest,1000);for(unsigned i=0;i<7;++i){auto t=c.admit();p.finish(t,2000+i);}check(p.deadline(oldest,10000,4)==6000,"eight receipts preserve exact oldest boundary");auto ninth=c.admit();p.finish(ninth,3000);check(p.deadline(oldest,10000,4)==0,"evicted receipt skips hold rather than using newer timestamp");for(unsigned i=0;i<32;++i){auto t=c.admit();p.finish(t,4000+i);check(p.deadline(t,10000,4)==9000+i,"bounded ring wrap preserves latest receipt");}std::puts("PASS eight-slot ring eviction/wrap");}
// Exercise the exact helper predicates across a real CV handover. The wrapper
// here is only scheduling glue; actual helper calculations select every target.
void cancellation(unsigned kind){SourceCredit c;c.start();SourceReleasePhase p;p.start(c.epoch);std::mutex m;std::condition_variable cv;bool stopping=false,failed=false;std::atomic_bool entered=false,done=false;bool canceled=false;auto target=c.admit();c.activate(target);c.finish(target);p.finish(target,us());const auto deadline=p.deadline(target,50000,4);check(deadline>us(),"test enters a real phase hold");
 std::thread waiter([&]{std::unique_lock l(m);entered=true;cv.notify_all();while(!c.canceled(target,stopping,failed)){auto now=us();if(now>=deadline)break;cv.wait_for(l,std::chrono::microseconds(deadline-now));}canceled=c.canceled(target,stopping,failed);done=true;});
 {std::unique_lock l(m);cv.wait(l,[&]{return entered.load();});if(kind==0)stopping=true;else if(kind==1)failed=true;else{c.cancel();c.start();p.start(c.epoch);auto newJob=c.admit();c.activate(newJob);}}cv.notify_all();waiter.join();check(done&&canceled,"stop/error/restart cancels an in-progress phase hold");check(us()<deadline,"cancel wake does not wait out old phase timer");}
void snapshot(){SourceCredit c;c.start();SourceReleasePhase p;p.start(c.epoch);auto empty=c.snapshot();auto future=c.admit();c.activate(future);check(empty.ordinal==0&&p.deadline(empty,13889,4)==0,"no-job entry never waits a future admitted source");auto captured=c.snapshot();c.finish(captured);p.finish(captured,10000);auto future2=c.admit();c.activate(future2);check(c.terminal(captured,false,false)&&p.deadline(captured,13889,4)==16944,"completed old target remains terminal while newer job stays active");std::puts("PASS empty-entry and active-newer ordinal handover");}
int main(){values();ring();snapshot();for(unsigned kind=0;kind<3;++kind)cancellation(kind);std::puts("PASS actual SourceReleasePhase/SourceCredit CPU proof; no GPU");}
