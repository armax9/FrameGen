# FrameGen

FrameGen adds 2× frame generation to compatible Windows VR games running in CrossOver on Apple silicon. The macOS menu bar app installs an OpenXR layer in the bottle you select and lets you switch generation on or off while the game is running.

The layer uses AMD FidelityFX Optical Flow on the game's rendered stereo images. It supports Direct3D 11 and Direct3D 12 OpenXR sessions, including Direct3D 11 OpenVR games that use OpenComposite. At 72 Hz, the 2× mode aims for 36 game frames and 36 generated frames per second. The game still samples input at its own frame rate.

## Setup

1. Start with a working MoltenVR setup and a VR game that already runs through D3DMetal in a CrossOver or CrossOver Preview bottle.
2. Download [FrameGen 0.1.0](https://github.com/armax9/FrameGen/releases/tag/v0.1.0), extract the ZIP, and open `FrameGen.app`. Select the bottle that runs the game and choose **Install** or **Update** when offered. The app compares the installed layer with the version it bundles.
3. Launch the game through its usual Steam VR launch option. Use your existing OpenComposite setup for an OpenVR game. The first launch after installing or updating loads the new layer.
4. Turn **Frame Generation** on from the macOS menu bar. **Active** means the runtime has recently accepted a generated frame. **Waiting** means generation is enabled and a generated frame has yet to reach the runtime, which can happen during loading or on an incompatible scene. Check the app log when it reports an unsupported session.

One switch applies to compatible sessions in the selected bottle. The installer registers an implicit OpenXR layer, so games keep their usual launch options, runtime, and mods. Set `DISABLE_ARMAX_FRAMEGEN=1` in the launch environment to bypass the layer for that launch.

## Compatibility tested so far

These checks used CrossOver, D3DMetal, and MoltenVR. **Active** records acceptance of generated frames. Coverage varies by game and scene.

| Game | VR path | Verified and pending |
| --- | --- | --- |
| Euro Truck Simulator 2 | Direct3D 11, native OpenXR | Generation and both live toggle directions worked in the cab with the earlier standalone build. The final menu bar release still needs a fresh cab check. |
| DiRT Rally 2.0 | Direct3D 11, OpenVR through OpenComposite | Generation and live switching worked in the VR menu. Earlier in-race tests showed artifacts and headset interruptions. Race verification is pending. |
| Subnautica | Direct3D 11, OpenVR through OpenComposite | Generation reached Active in the menu with a local D3DMetal Unity compatibility repair. The tested build also had headset interruptions. Full gameplay verification is pending. |
| The Elder Scrolls V: Skyrim VR | Direct3D 11, OpenVR through OpenComposite | Generation reached Active in a short user test. |
| MADiSON VR | Direct3D 11, native OpenXR | Generation reached Active in a short VR test. |
| Escape Simulator 2 | Direct3D 11, native OpenXR | Generation reached Active with the two-eye texture array used by the Steam VR launch. A forced Direct3D 12 run fell back to Direct3D 11. |
| Red Matter 2 | Direct3D 12, native OpenXR | Generation reached Active in the menu at roughly 71–72 headset FPS and 36 game view queries per second. Turning generation off restored about 72 game view queries per second. Turning it back on stayed at Waiting in that run. Restart with generation enabled if that happens. |

In Red Matter 2, moving the menu quad to the GPU reduced the measured MoltenVR frame submission time from about 31 ms to about 3 ms in the tested menu. Performance and image quality can vary across scenes. Direct3D 12 verification currently covers Red Matter 2.

Some Unity games can hit a Direct3D 11 context limitation in D3DMetal. The Subnautica check used a local repair for Game Porting Toolkit 4.0 beta 3. That driver repair is separate from FrameGen. The release contains FrameGen and its redistributable dependencies. Obtain CrossOver, D3DMetal, MoltenVR, and games separately.

## Source and credits

FrameGen v0.1.0 is released under the GNU General Public License v3.0. Upstream components retain their own notices. Build instructions are in [BUILD.md](BUILD.md).

- The Windows layer builds on optical-flow synthesis and OpenXR work by tig3rmast3r and contributors. [OFXR Bridge](https://github.com/tig3rmast3r/OFXR-Bridge)
- Frame scheduling and GPU resource ownership were informed by the work of eregnier and contributors. [ReproVR](https://github.com/eregnier/repro-vr)
- The AMD optical-flow backend uses the FidelityFX SDK. [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
