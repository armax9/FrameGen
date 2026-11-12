# Third-party notices

FrameGen is a macOS and CrossOver adaptation of portions of OFXR Bridge.
The Windows layer, build system, and macOS app were adapted for Apple
silicon, MoltenVR, and CrossOver by armax9. The adaptation adds the
bottle installer, live controls, MoltenVR-compatible stereo swapchains,
and Direct3D 12 GPU composition for supported quad overlays.

- [OFXR Bridge](https://github.com/tig3rmast3r/OFXR-Bridge) by tig3rmast3r
  and contributors is GPL-3.0. FrameGen retains its license and notices.
- [ReproVR](https://github.com/eregnier/repro-vr) by eregnier and
  contributors is LGPL-3.0. Its scheduling and GPU ownership work
  informed this adaptation. The upstream license text is in
  `LICENSES/ReproVR-LGPL-3.0.txt`.
- [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
  provides the Optical Flow implementation. The Direct3D 11 and
  Direct3D 12 subsets include their respective MIT notices and provenance.
- [Khronos OpenXR](https://github.com/KhronosGroup/OpenXR-SDK)
  provides the API headers. Their license texts are in
  `source/vendor/openxr/LICENSES`.

FrameGen is independent of MoltenVR, CodeWeavers, and Apple.
