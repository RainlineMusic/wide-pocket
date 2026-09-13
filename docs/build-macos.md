# Building Wide Pocket on macOS

## Requirements

- macOS 13 or newer
- Xcode 15 or newer with command line tools
- CMake 3.22 or newer
- JUCE 8.0.4 (exact version)

Download `https://github.com/juce-framework/JUCE/releases/download/8.0.4/juce-8.0.4-osx.zip`,
unpack it, and point `JUCE_DIR` at the unpacked `JUCE` folder.

## Universal build

```bash
cmake -S . -B build -G Xcode \
  -DJUCE_DIR="$HOME/SDK/JUCE" \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build --config Release --target WidePocket_VST3 WidePocket_AAX
```

Verify both slices are present:

```bash
lipo "build/WidePocket_artefacts/Release/VST3/Wide Pocket.vst3/Contents/MacOS/Wide Pocket" -verify_arch arm64 x86_64
```

## Debug build

```bash
cmake --build build --config Debug --target WidePocket_AAX
```

For a native-only debug build, omit `CMAKE_OSX_ARCHITECTURES` so the build is
single-architecture and much faster.

## Signing

CI produces ad-hoc signed, non-notarized artefacts. For local use:

```bash
codesign --force --deep --sign - "build/WidePocket_artefacts/Release/VST3/Wide Pocket.vst3"
```

For distribution, sign with a Developer ID Application identity and notarize.
AAX requires PACE signing instead; see [aax-setup.md](aax-setup.md).

## Tests

```bash
ctest --test-dir build -C Release --output-on-failure
```

Or directly, without JUCE or CMake:

```bash
c++ -std=c++17 -O2 Tests/dsp_core_test.cpp -o dsp_core_test && ./dsp_core_test
```

## Install locally

- VST3: `~/Library/Audio/Plug-Ins/VST3/`
- AAX: `/Library/Application Support/Avid/Audio/Plug-Ins/` (after signing)
