#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <algorithm>
namespace presentation_study {
using Time=int64_t;
struct V {double x=0,y=0,z=0; V operator+(V b)const{return{x+b.x,y+b.y,z+b.z};} V operator-(V b)const{return{x-b.x,y-b.y,z-b.z};} V operator*(double k)const{return{x*k,y*k,z*k};}};
struct Q {double x=0,y=0,z=0,w=1;};
inline double dot(Q a,Q b){return a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;}
inline Q conjugate(Q a){return{-a.x,-a.y,-a.z,a.w};}
inline Q mul(Q a,Q b){return{a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
inline V rotate(Q q,V v){Q t=mul(mul(q,{v.x,v.y,v.z,0}),conjugate(q));return{t.x,t.y,t.z};}
inline bool valid(Q q){return std::isfinite(dot(q,q))&&std::fabs(dot(q,q)-1)<1e-5;}
inline bool valid(V v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
inline Q halfway(Q a,Q b){if(dot(a,b)<0)b={-b.x,-b.y,-b.z,-b.w};Q q{a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w};double n=std::sqrt(dot(q,q));return{q.x/n,q.y/n,q.z/n,q.w/n};}
inline double distance(V v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);}
struct Pose{Q q;V p;};
struct Eye {Pose pose;std::array<float,4>fov{};};
struct Stereo {uint64_t space=0;Time sourceTime=0;std::array<Eye,2>eyes;};
inline bool valid(const Stereo&s){if(!s.space||s.sourceTime<=0)return false;for(auto&e:s.eyes){if(!valid(e.pose.q)||!valid(e.pose.p))return false;for(auto f:e.fov)if(!std::isfinite(f)||std::fabs(f)>=1.5707f)return false;if(e.fov[0]>=e.fov[1]||e.fov[3]>=e.fov[2])return false;}return true;}
// Interpolate one rigid stereo rig, not two independently moving eye positions.
inline bool midpoint(const Stereo&a,const Stereo&b,Stereo&out){
 if(!valid(a)||!valid(b)||a.space!=b.space||a.sourceTime>=b.sourceTime)return false;
 for(int i=0;i<2;i++)for(int k=0;k<4;k++)if(std::fabs(a.eyes[i].fov[k]-b.eyes[i].fov[k])>1e-6f)return false;
 Q aq=a.eyes[0].pose.q,bq=b.eyes[0].pose.q,mq=halfway(aq,bq);
 V ac=(a.eyes[0].pose.p+a.eyes[1].pose.p)*.5,bc=(b.eyes[0].pose.p+b.eyes[1].pose.p)*.5,mc=(ac+bc)*.5;
 out={};out.space=a.space;out.sourceTime=a.sourceTime+(b.sourceTime-a.sourceTime)/2;
 for(int i=0;i<2;i++){
  V al=rotate(conjugate(aq),a.eyes[i].pose.p-ac),bl=rotate(conjugate(bq),b.eyes[i].pose.p-bc);
  Q ar=mul(conjugate(aq),a.eyes[i].pose.q),br=mul(conjugate(bq),b.eyes[i].pose.q);
  if(distance(al-bl)>1e-5||std::fabs(dot(ar,br))<1-1e-6)return false;
  out.eyes[i].pose={mul(mq,ar),mc+rotate(mq,al)};out.eyes[i].fov=a.eyes[i].fov;
 }
 return true;
}
// Abstract GPU ownership only: an actual bridge must hold texture references and
// an observed completion proof for these exact publication generations.
struct Images {uint64_t generation=0;bool gpuComplete=false;virtual ~Images()=default;};
struct Pair {uint64_t epoch=0,sequence=0;Stereo previous,current;std::shared_ptr<const Images>synthetic,real;};
enum class Kind {empty,synthetic,real,repeat};
struct Plan {uint64_t token=0,epoch=0,sequence=0;Kind kind=Kind::empty;Time displayTime=0;Stereo content;std::shared_ptr<const Images>images;std::shared_ptr<const Pair>publication;};
// Pure single-owner policy. It performs no waits, D3D calls or OpenXR calls.
// A real presenter must consume one plan within one real Wait/Begin/End cycle.
class Policy {
 uint64_t epoch_=0,lastSequence_=0,token_=0;Time attemptedTime_=0,lastContentTime_=0,lastPublishedTime_=0;
 std::shared_ptr<const Pair>latest_,protectedPair_;Plan last_;bool outstanding_=false;
public:
 void reset(uint64_t epoch){epoch_=epoch;lastSequence_=0;attemptedTime_=lastContentTime_=lastPublishedTime_=0;latest_.reset();protectedPair_.reset();last_={};outstanding_=false;++token_;}
 bool publish(std::shared_ptr<const Pair>p){if(!p||p->epoch!=epoch_||p->sequence<=lastSequence_||!valid(p->current)||!p->real||!p->real->gpuComplete||p->real->generation!=p->sequence||p->current.sourceTime<=lastContentTime_||p->current.sourceTime<=lastPublishedTime_)return false;latest_=std::move(p);lastSequence_=latest_->sequence;lastPublishedTime_=latest_->current.sourceTime;return true;}
 Plan prepare(Time prediction,Time period,bool shouldRender){
  if(outstanding_||prediction<=attemptedTime_||period<=0||period>1'000'000'000)return{};
  outstanding_=true;attemptedTime_=prediction;Plan p;p.token=++token_;p.epoch=epoch_;p.displayTime=prediction;
  if(!shouldRender)return p;
  std::shared_ptr<const Pair>pair=protectedPair_?protectedPair_:latest_;
  if(!pair){if(last_.images){p=last_;p.token=token_;p.displayTime=prediction;p.kind=Kind::repeat;}return p;}
  // Do not add a FIFO backlog after a stall: promote a newer real frame.
  bool catchup=protectedPair_&&latest_&&latest_->sequence>protectedPair_->sequence&&latest_->current.sourceTime-protectedPair_->current.sourceTime>period*2;
  if(catchup)pair=latest_;
  p.publication=pair;p.sequence=pair->sequence;p.content=pair->current;p.images=pair->real;p.kind=Kind::real;
  Stereo mid;
  bool gapOK=pair->current.sourceTime>pair->previous.sourceTime&&pair->current.sourceTime-pair->previous.sourceTime<=period*4;
  bool syntheticOK=!protectedPair_&&!catchup&&gapOK&&pair->synthetic&&pair->synthetic->gpuComplete&&pair->synthetic->generation==pair->sequence&&midpoint(pair->previous,pair->current,mid)&&mid.sourceTime>lastContentTime_;
  if(syntheticOK){p.kind=Kind::synthetic;p.content=mid;p.images=pair->synthetic;}
  if(p.content.sourceTime<lastContentTime_){p={};p.token=token_;p.epoch=epoch_;p.displayTime=prediction;}
  return p;
 }
 bool finish(const Plan&p,bool success){
  if(!outstanding_||p.token!=token_||p.epoch!=epoch_)return false;outstanding_=false;
  if(!success){protectedPair_.reset();return false;}
  if(p.kind==Kind::empty)return true;
  lastContentTime_=p.content.sourceTime;last_=p;
  if(p.kind==Kind::synthetic){protectedPair_=p.publication;}
  else if(p.kind==Kind::real){if(latest_&&latest_->sequence<=p.sequence)latest_.reset();protectedPair_.reset();}
  return true;
 }
};
}
