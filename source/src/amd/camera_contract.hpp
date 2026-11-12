#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>
namespace ofxr_color_d11 {
struct CameraMapping {
    std::array<float,4> target_to_source_rotation{},source_tangents{},target_tangents{},source_rect{},target_rect{};
};
struct alignas(16) Parameters {
    uint32_t width{},height{},array_size{},packed_width{};
    uint32_t packed_height{},packed_eye_stride{},flow_width{},flow_height{};
    uint32_t flow_block_size{8},view_index{},use_game_motion{},game_motion_padding{};
    std::array<float,4> game_motion_rect{};
    std::array<float,2> game_motion_scale{},game_motion_jitter_delta{};
    std::array<CameraMapping,2> previous_mappings{};
    uint32_t slice{},synthesis_flags{};
    std::array<uint32_t,2> output_origin{};
};
static_assert(sizeof(CameraMapping)==80);
static_assert(sizeof(Parameters)==256);
static_assert(offsetof(Parameters,slice)==240);
struct View {
    std::array<float,4> orientation{0,0,0,1}; // OpenXR quaternion x,y,z,w; camera->world.
    std::array<float,4> fov{}; // left,right,up,down in radians.
    std::array<float,4> rect{}; // pixel x,y,width,height; must be resolved/nonempty.
};
namespace {
inline bool unit(std::array<float,4>& q) noexcept {float n=0;for(auto v:q){if(!std::isfinite(v))return false;n+=v*v;}if(!std::isfinite(n)||n<1e-12f)return false;n=1/std::sqrt(n);for(auto&v:q)v*=n;return true;}
inline std::array<float,4> multiply(const std::array<float,4>&a,const std::array<float,4>&b)noexcept{return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}

}
inline bool make_mapping(const View&s,const View&t,CameraMapping&out)noexcept{
 auto a=s.orientation,b=t.orientation;if(!unit(a)||!unit(b))return false;for(unsigned k=0;k<3;k++)a[k]=-a[k];auto q=multiply(a,b);if(!unit(q))return false;CameraMapping m;m.target_to_source_rotation=q;
 for(unsigned k=0;k<4;k++){if(!std::isfinite(s.fov[k])||!std::isfinite(t.fov[k])||std::abs(s.fov[k])>=1.5707f||std::abs(t.fov[k])>=1.5707f||!std::isfinite(s.rect[k])||!std::isfinite(t.rect[k]))return false;m.source_tangents[k]=std::tan(s.fov[k]);m.target_tangents[k]=std::tan(t.fov[k]);}
 if(s.fov[0]>=s.fov[1]||s.fov[3]>=s.fov[2]||t.fov[0]>=t.fov[1]||t.fov[3]>=t.fov[2]||s.rect[0]<0||s.rect[1]<0||t.rect[0]<0||t.rect[1]<0||s.rect[2]<=0||s.rect[3]<=0||t.rect[2]<=0||t.rect[3]<=0)return false;m.source_rect=s.rect;m.target_rect=t.rect;out=m;return true;
}
inline uint32_t fraction_flags(float f)noexcept{if(!std::isfinite(f))return 0;auto i=static_cast<uint32_t>(std::clamp(f,0.f,1.f)*65535.f+.5f);return std::max(i,1u)<<8;}
inline bool set_packed_extent(Parameters&p,uint32_t w,uint32_t h,uint32_t n,uint32_t div=1)noexcept{if(!w||!h||w>16384||h>16384||!n||n>2||!div||div>4)return false;uint64_t sw=(uint64_t(w)+div-1)/div,sh=(uint64_t(h)+div-1)/div;auto align=[](uint64_t x){return (x+7)/8*8;};auto stride=align(sh+64),pw=std::max<uint64_t>(64,align(sw)),ph=std::max<uint64_t>(64,align(stride*(n-1)+sh));if(pw>16384||ph>16384)return false;p.width=w;p.height=h;p.array_size=n;p.packed_width=uint32_t(pw);p.packed_height=uint32_t(ph);p.packed_eye_stride=uint32_t(stride);p.flow_block_size=8;return true;}

}
