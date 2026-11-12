# Building FrameGen

Build on an Apple silicon Mac with Python 3, Swift 6 and a macOS SDK through Xcode or its Command Line Tools, and an LLVM-MinGW toolchain for `x86_64-w64-mingw32`. The Windows layer dependencies are in `source/vendor`.

From the repository root, run the following with the path to your LLVM-MinGW installation.

```sh
LLVM_MINGW_DIR="/path/to/llvm-mingw" ./menu-app/build.zsh
```

The script builds both Direct3D backends and the macOS app, then creates `build/menu-app/FrameGen.zip`. Pass an output path ending in `.app` to build an unpacked bundle or `.zip` to choose a different archive name. The toolchain root or its `bin` directory both work for `LLVM_MINGW_DIR`. You can also put the toolchain binaries on `PATH`.

The build script ad hoc signs the app and verifies the signature after packaging. For a notarized distribution, sign with your Developer ID and notarize the release. Distribute the corresponding source and bundled license notices with release binaries.
