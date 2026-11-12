#include "menu_framegen_profile.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace runtime_study::menu_framegen;

static void require(bool condition,const char* message) {
    if (condition) return;
    std::fprintf(stderr,"%s\n",message);
    std::exit(1);
}

int main() {
    std::array<XrCompositionLayerProjectionView,2> views{};
    XrCompositionLayerProjection stereo{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    stereo.viewCount=static_cast<uint32_t>(views.size());
    stereo.views=views.data();
    XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad panel{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const auto* projection=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&stereo);
    const auto* hudLayer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud);
    const auto* panelLayer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel);
    std::array layers{hudLayer,projection,panelLayer};
    XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
    frame.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    frame.layerCount=static_cast<uint32_t>(layers.size());
    frame.layers=layers.data();
    AdmissionIssue issue;

    require(checkStereoProjection(&frame,issue,true)==&stereo,"accept quads on both sides of stereo");
    require(issue.reason==BlockReason::none,"no issue for supported layers");
    require(!checkStereoProjection(&frame,issue,false)&&issue.reason==BlockReason::layer_count,
            "reject multiple layers where only one can be carried");

    layers[2]=projection;
    require(!checkStereoProjection(&frame,issue,true)&&issue.reason==BlockReason::layer_type,
            "reject duplicate projections");
    layers[2]=panelLayer;

    XrCompositionLayerColorScaleBiasKHR extension{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
    hud.next=&extension;
    require(!checkStereoProjection(&frame,issue,true)&&issue.reason==BlockReason::layer_extensions,
            "reject extended quads");
    hud.next=nullptr;
    frame.layerCount=1;
    require(!checkStereoProjection(&frame,issue,true)&&issue.reason==BlockReason::layer_type,
            "bypass a quad-only menu");
    frame.layers=layers.data()+1;
    require(checkStereoProjection(&frame,issue,false)==&stereo,"recover the original stereo path");
    stereo.layerFlags=XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    require(checkStereoProjection(&frame,issue,false)==&stereo,"allow alpha encoding flag with opaque projection");
    stereo.layerFlags|=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    require(!checkStereoProjection(&frame,issue,false)&&issue.reason==BlockReason::layer_flags,
            "reject projection that really blends source alpha");

    const auto sharedChain=reinterpret_cast<XrSwapchain>(1);
    views[0].subImage.swapchain=sharedChain;
    views[1].subImage.swapchain=sharedChain;
    views[1].subImage.imageArrayIndex=1;
    require(!checkStereoProjection(&frame,issue,false)&&issue.reason==BlockReason::layer_flags,
            "default runtime must still reject source alpha for shared stereo");
    require(checkStereoProjection(&frame,issue,false,true)==&stereo,
            "allow shared stereo source alpha only on the verified stock path");
    frame.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    require(!checkStereoProjection(&frame,issue,false,true)&&issue.reason==BlockReason::blend_mode,
            "reject alpha-blended environments even on the verified path");
    frame.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    views[1].subImage.imageArrayIndex=0;
    require(!checkStereoProjection(&frame,issue,false,true)&&issue.reason==BlockReason::layer_flags,
            "reject shared projections without distinct stereo slices");
    views[1].subImage.imageArrayIndex=1;
    stereo.layerFlags|=XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT;
    require(!checkStereoProjection(&frame,issue,false,true)&&issue.reason==BlockReason::layer_flags,
            "reject extra alpha modes even on the verified path");
    stereo.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    require(!checkStereoProjection(&frame,issue,false,true)&&issue.reason==BlockReason::layer_flags,
            "reject premultiplied source alpha");
    stereo.layerFlags|=XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    views[1].subImage.swapchain=reinterpret_cast<XrSwapchain>(2);
    require(!checkStereoProjection(&frame,issue,false,true)&&issue.reason==BlockReason::layer_flags,
            "reject source alpha on split stereo");
    views[1].subImage.swapchain=sharedChain;
    frame.layers=layers.data();
    frame.layerCount=static_cast<uint32_t>(layers.size());
    require(!checkStereoProjection(&frame,issue,true,true)&&issue.reason==BlockReason::layer_flags,
            "reject source alpha when there are other layers to blend");

    std::puts("PASS: stereo layer admission, quads, and scoped shared-stereo alpha compatibility");
}
