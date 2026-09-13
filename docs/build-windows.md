# Building Wide Pocket on Windows

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 with the "Desktop development with C++" workload
- CMake 3.22 or newer
- JUCE 8.0.4 (exact version; do not substitute another release)

JUCE is not vendored in this repository. Download
`https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.4.zip`, unpack it,
and point `JUCE_DIR` at the unpacked folder.

## Configure and build

```powershell
cmake -S . -B build -A x64 -DJUCE_DIR="C:/SDK/JUCE-8.0.4"
cmake --build build --config Release
```

Targets:

| Target | Output |
| --- | --- |
| `WidePocket_VST3` | `build/WidePocket_artefacts/Release/VST3/Wide Pocket.vst3` |
| `WidePocket_AAX` | `build/WidePocket_artefacts/Release/AAX/Wide Pocket.aaxplugin` |
| `WidePocket_Standalone` | `build/WidePocket_artefacts/Release/Standalone/Wide Pocket.exe` |

Build a single format, for example:

```powershell
cmake --build build --config Release --target WidePocket_VST3
```

## Debug build

```powershell
cmake --build build --config Debug --target WidePocket_VST3 WidePocket_AAX
```

Debug AAX artefacts land in `build/WidePocket_artefacts/Debug/AAX/`. See
[aax-setup.md](aax-setup.md) for PACE signing, which is required before Pro Tools
will load them.

## Tests

The DSP tests are dependency-free C++ and do not require JUCE:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

They can also be compiled directly:

```powershell
cl /std:c++17 /O2 /EHsc Tests\dsp_core_test.cpp
```

## Install locally

Copy `Wide Pocket.vst3` to `C:\Program Files\Common Files\VST3\`.
Copy `Wide Pocket.aaxplugin` to
`C:\Program Files\Common Files\Avid\Audio\Plug-Ins\` after signing.
