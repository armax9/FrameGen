"""Build isolated AMD FidelityFX 1.1.4 objects for the native DX12 preview."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
AMD = ROOT / "vendor/amd-dx12"
ADAPTER = ROOT / "src/amd"
SDK_UNITS = (
    "src/backends/dx12/ffx_dx12.cpp",
    "src/components/opticalflow/ffx_opticalflow.cpp",
    "src/shared/ffx_assert.cpp",
    "src/shared/ffx_breadcrumbs_list.cpp",
    "src/shared/ffx_object_management.cpp",
)


def build(cc, out_dir, common_flags) -> list[Path]:
    """Return COFF objects ready to link into FrameGen.dll."""
    out_dir = Path(out_dir) / "amd-dx12"
    out_dir.mkdir(parents=True, exist_ok=True)
    includes = (
        ADAPTER,
        AMD / "include",
        AMD / "src/backends/shared",
        AMD / "src/components",
        AMD / "src/shared",
    )
    flags = [str(cc), *common_flags, *[f"-I{path}" for path in includes]]
    units = [ADAPTER / "amd_flow12.cpp"]
    for relative in SDK_UNITS:
        source = AMD / relative
        wrapper = out_dir / source.name
        wrapper.write_text(
            '#include "dx12_sdk_platform.hpp"\n'
            'namespace framegen_amd_dx12 {\n'
            f'#include "{source.as_posix()}"\n'
            '}\n'
        )
        units.append(wrapper)

    objects = []
    for unit in units:
        output = out_dir / f"{unit.stem}.o"
        subprocess.run([*flags, "-c", str(unit), "-o", str(output)], check=True)
        objects.append(output)
    return objects
