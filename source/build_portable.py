#!/usr/bin/env python3
"""Build the FrameGen OpenXR layer from the bundled source.

Requires Python3 and an llvm-mingw toolchain; does not install or run the DLL.
Set LLVM_MINGW_DIR to the toolchain root (or its bin directory), or put the
x86_64-w64-mingw32-clang++ compiler on PATH. No network/source checkout needed.
"""
from pathlib import Path
import argparse
import hashlib
import os
import shutil
import subprocess

ROOT=Path(__file__).resolve().parent
CPP_RUNTIME=["runtime_layer.cpp","color_flow_pipeline.cpp",
             "amd/amd_flow.cpp","amd/amd_shaders.cpp","amd/stock_camera_d11.cpp",
             "amd/ffx_opticalflow_history.cpp"]
CPP_AMD=["src/backends/dx11/ffx_dx11.cpp",
         "src/shared/ffx_assert.cpp","src/shared/ffx_object_management.cpp"]
C_AMD=["src/backends/dx11/DXBCChecksum.c","src/backends/dx11/md5.c"]

def toolchain_dir(value):
    if value:
        p=Path(value).expanduser().resolve()
        return p/"bin" if (p/"bin").is_dir() else p
    compiler=shutil.which("x86_64-w64-mingw32-clang++")
    if compiler:
        return Path(compiler).resolve().parent
    raise SystemExit("Set LLVM_MINGW_DIR or pass --toolchain pointing to llvm-mingw.")

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain",default=os.environ.get("LLVM_MINGW_DIR"))
    parser.add_argument("--output",type=Path,default=ROOT/"build"/"FrameGen.dll")
    parser.add_argument("--debug",action="store_true",help="Include debug symbols.")
    parser.add_argument("--menu-layer",action="store_true",help=argparse.SUPPRESS)
    parser.add_argument("--native-d3d12",action=argparse.BooleanOptionalAction,default=True,
                        help="Include native D3D12 support (enabled by default).")
    args=parser.parse_args()
    tc=toolchain_dir(args.toolchain)
    cc=tc/"x86_64-w64-mingw32-clang++"
    c=tc/"x86_64-w64-mingw32-clang"
    for compiler in [cc,c]:
        if not compiler.is_file():
            raise SystemExit(f"Missing compiler: {compiler}")
    amd=ROOT/"vendor"/"amd"
    includes=[ROOT/"src",ROOT/"vendor/ofxr/include",ROOT/"vendor/openxr/include",
              amd/"include",amd/"src/components",
              amd/"src/shared",amd/"src/backends/shared"]
    cpp_sources=[ROOT/"src"/x for x in CPP_RUNTIME]+[ROOT/"vendor/ofxr/private_openxr_runtime.cpp"]+[amd/x for x in CPP_AMD]
    c_sources=[amd/x for x in C_AMD]
    for source in cpp_sources+c_sources+[amd/"build_compat.hpp",ROOT/"src/amd/embedded_opticalflow.hpp"]:
        if not source.is_file():
            raise SystemExit(f"Snapshot incomplete: {source.relative_to(ROOT)}")
    output=args.output.expanduser().resolve();output.parent.mkdir(parents=True,exist_ok=True)
    objdir=output.parent/"portable-objects";objdir.mkdir(exist_ok=True)
    objects=[]
    for source in c_sources:
        obj=objdir/(source.stem+".o")
        command=[str(c),"-O2","-c",str(source),"-o",str(obj)]
        subprocess.run(command,check=True)
        objects.append(str(obj))
    common_flags=["-std=c++20","-O2","-DNOMINMAX","-DWIN32_LEAN_AND_MEAN",
                  f"-ffile-prefix-map={ROOT}=."]
    flags=[*common_flags,"-static","-shared",
           "-DFRAMEGEN_MENU_LAYER","-DXR_USE_PLATFORM_WIN32","-DXR_USE_GRAPHICS_API_D3D11",
           "-DXR_USE_GRAPHICS_API_D3D12",
           "-include",str(amd/"build_compat.hpp")]
    if args.debug:
        flags.append("-g")
    if args.native_d3d12:
        from build_amd_dx12 import build as build_amd_dx12
        flags.append("-DFRAMEGEN_NATIVE_D3D12")
        cpp_sources.append(ROOT/"src/d3d12_color_flow.cpp")
        objects.extend(map(str,build_amd_dx12(cc,objdir,common_flags)))
    command=[str(cc),*flags,*["-I"+str(p) for p in includes],*map(str,cpp_sources),*objects,
             "-o",str(output),"-ld3d11","-ld3d12","-ldxgi","-ld3dcompiler_47","-ldxguid"]
    subprocess.run(command,check=True)
    data=output.read_bytes()
    if data[:2]!=b"MZ":
        raise SystemExit("Compiler did not produce a PE DLL.")
    print(f"Built {output}")
    print(f"Size: {len(data)} bytes")
    print(f"SHA256: {hashlib.sha256(data).hexdigest()}")
    print("No installation or GPU execution performed.")

if __name__=="__main__":
    main()
