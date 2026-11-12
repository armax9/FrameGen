# AMD FidelityFX Optical Flow for DirectX 12

Subset of [AMD FidelityFX SDK v1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4), used under its MIT license. The AMD source files retain their license notices. The included D3DX12 utility retains its own license.

Only the native DX12 backend, Optical Flow component, shared host support and their compile-time headers are retained. PIX is disabled. The optional history-only path and error handling are local adaptations. The DX12 SDK symbols and types are isolated from the existing DX11 fork.

The seven SDR Shader Model 6.2 DXIL programs in `source/src/amd/embedded_opticalflow12.hpp` come from the tag’s `PrebuiltSignedDLL/amd_fidelityfx_dx12.dll`. Resource names, descriptor slots, counts and spaces are copied from the SDK-generated reflection tables beside each shader. The programs use the SDK’s default wave size.

SHA-256 of the source DLL: `12a5081257ec95b0b53ad51b4a87fb3c03f97fe0bbb59f9496968f8d50ef93a6`

| Pass | DLL file offset | Bytes | SHA-256 |
| --- | --- | --- | --- |
| prepareLuma | `0x637c70` | 4944 | `4027f26a3b0f84e9723d3f53da5d814768e47565b43468b35de7932469d4a546` |
| generateLumaPyramid | `0x614c50` | 8240 | `88484dac6e2b0a351f5bcf2541660764d7ef1e86b47563e8ee94b691bd90804f` |
| generateSceneHistogram | `0x632930` | 4932 | `084f7363a9cef8d623cbf6bfc1730367b991ecf4ef230827d49a8eedbf7bc8c7` |
| computeSceneDivergence | `0x626c70` | 7184 | `646f60fcb36be4298f2cb7a3fb1bcacb227c28290c91f730b88553ffc324c435` |
| computeOpticalFlow | `0x61d3a0` | 8568 | `19f6dbaebea5bbbfd8edd662e03e3e23ab0547c6da1512511084b2bd79fecc05` |
| filterOpticalFlow | `0x62e220` | 4128 | `d1a0125f015462ce8a9de18c5802fb8e31301e73a7cefcfa3b7f8ce2d5e406d5` |
| scaleOpticalFlow | `0x63cef0` | 6796 | `0701f9193a74c9784cbb20703709cd2e0605c049ff2e9925489ce649c614c974` |
