#pragma once

// SDK 1.1.4 and the DX11 fork have different host ABIs. Include the platform
// dependencies globally before wrapping the 1.1.4 implementation in a namespace.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <codecvt>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <forward_list>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <vector>

// C-linkage functions need distinct names even inside a C++ namespace.
#define ffxAssertReport framegenDx12AssertReport
#define ffxAssertSetPrintingCallback framegenDx12AssertSetPrintingCallback
#define ffxGetPermutationBlobByIndex framegenDx12GetPermutationBlobByIndex
#define ffxIsWave64 framegenDx12IsWave64
#define ffxOpticalflowContextCreate framegenDx12OpticalFlowCreate
#define ffxOpticalflowContextDestroy framegenDx12OpticalFlowDestroy
#define ffxOpticalflowContextDispatch framegenDx12OpticalFlowDispatch
#define ffxOpticalflowContextGetGpuMemoryUsage framegenDx12OpticalFlowGpuMemoryUsage
#define ffxOpticalflowGetEffectVersion framegenDx12OpticalFlowEffectVersion
#define ffxOpticalflowGetSharedResourceDescriptions framegenDx12OpticalFlowSharedDescriptions
#define ffxSafeReleaseCopyResource framegenDx12SafeReleaseCopyResource
#define ffxSafeReleasePipeline framegenDx12SafeReleasePipeline
#define ffxSafeReleaseResource framegenDx12SafeReleaseResource
