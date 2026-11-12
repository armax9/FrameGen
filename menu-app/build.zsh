#!/bin/zsh
set -eu

here=${0:A:h}
root=${here:h}
# Older local build commands can still pass the DX12 preview flag.
if [[ ${1:-} == --native-d3d12 ]]; then shift; fi
(( $# <= 1 )) || { print -u2 'Usage: build.zsh [output.app or output.zip]'; exit 2; }
output=${1:-"$root/build/menu-app/FrameGen.zip"}
output=${output:A}
[[ "$output" == *.app || "$output" == *.zip ]] || { print -u2 'The output must end in .app or .zip.'; exit 2; }
if [[ "$output" == *.app && -e "$output" ]]; then
  installed_id=$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$output/Contents/Info.plist" 2>/dev/null || true)
  [[ "$installed_id" == com.armax.framegen ]] || { print -u2 'The destination contains another app.'; exit 2; }
fi

mkdir -p -- "${output:h}"
stage=$(mktemp -d "${output:h}/.framegen-build.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
app="$stage/FrameGen.app"
resources="$app/Contents/Resources"
mkdir -p "$app/Contents/MacOS" "$resources/Licenses"

nice -n 15 python3 -B "$root/source/build_portable.py" --menu-layer --native-d3d12 --output "$stage/FrameGen.dll"
install -m 0644 "$stage/FrameGen.dll" "$resources/FrameGen.dll"
install -m 0644 "$here/FrameGen.json" "$resources/FrameGen.json"
install -m 0755 "$here/FrameGenCtl" "$resources/FrameGenCtl"
install -m 0644 "$here/Info.plist" "$app/Contents/Info.plist"
install -m 0644 "$here/FrameGen.icns" "$resources/FrameGen.icns"
install -m 0644 "$root/LICENSE" "$resources/Licenses/FrameGen-and-OFXR-GPL-3.0.txt"
install -m 0644 "$root/THIRD_PARTY_NOTICES.md" "$resources/Licenses/THIRD_PARTY_NOTICES.md"
install -m 0644 "$root/LICENSES/ReproVR-LGPL-3.0.txt" "$resources/Licenses/ReproVR-LGPL-3.0.txt"
install -m 0644 "$root/source/vendor/amd/LICENSE.txt" "$resources/Licenses/AMD-D3D11.txt"
install -m 0644 "$root/source/vendor/amd/Third_party_notices.txt" "$resources/Licenses/AMD-D3D11-notices.txt"
install -m 0644 "$root/source/vendor/amd-dx12/LICENSE.txt" "$resources/Licenses/AMD-D3D12.txt"
install -m 0644 "$root/source/vendor/amd-dx12/include/FidelityFX/host/backends/dx12/license.txt" "$resources/Licenses/Microsoft-D3DX12.txt"
install -m 0644 "$root/source/vendor/openxr/LICENSES/MIT.txt" "$resources/Licenses/OpenXR-MIT.txt"
install -m 0644 "$root/source/vendor/openxr/LICENSES/Apache-2.0.txt" "$resources/Licenses/OpenXR-Apache-2.0.txt"
nice -n 15 xcrun swiftc -swift-version 6 -O -target arm64-apple-macos13.0 \
  -module-cache-path "$stage/swift-cache" "$here/FrameGenMenu.swift" -o "$app/Contents/MacOS/FrameGenMenu"

/usr/bin/plutil -lint "$app/Contents/Info.plist"
/bin/zsh -n "$resources/FrameGenCtl"
/usr/bin/xattr -cr "$app"
/usr/bin/codesign --force --sign - "$app"
/usr/bin/codesign --verify --strict "$app"
if [[ "$output" == *.zip ]]; then
  archive="$stage/FrameGen.zip"
  /usr/bin/ditto -c -k --norsrc --keepParent "$app" "$archive"
  mkdir "$stage/unpacked"
  /usr/bin/ditto -x -k "$archive" "$stage/unpacked"
  /usr/bin/codesign --verify --strict "$stage/unpacked/FrameGen.app"
  mv -f -- "$archive" "$output"
  print -r -- "$output"
  exit 0
fi
if [[ -e "$output" ]]; then mv -- "$output" "$stage/previous.app"; fi
if ! mv -- "$app" "$output"; then
  if [[ -d "$stage/previous.app" ]]; then mv -- "$stage/previous.app" "$output"; fi
  exit 1
fi
# A File Provider destination can attach Finder metadata during the move.
if ! /usr/bin/xattr -cr "$output" || ! /usr/bin/codesign --verify --strict "$output"; then
  mv -- "$output" "$stage/failed.app"
  if [[ -d "$stage/previous.app" ]]; then mv -- "$stage/previous.app" "$output"; fi
  exit 1
fi
print -r -- "$output"
