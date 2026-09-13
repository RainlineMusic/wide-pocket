# AAX build and signing

## No separate SDK download is required

JUCE 8.0.4 ships the AAX SDK inside its own distribution at:

```
JUCE/modules/juce_audio_plugin_client/AAX/SDK/
```

JUCE's CMake layer detects it automatically. `JUCEUtils.cmake` calls
`_juce_init_bundled_aax_sdk()` while configuring the `WidePocket_AAX` target,
which in turn calls `juce_set_aax_sdk_path()` on that bundled path. As a result
this project needs nothing more than:

```cmake
juce_add_plugin(WidePocket
    FORMATS VST3 AAX Standalone
    AAX_IDENTIFIER "com.rainlinemusic.widepocket"
    AAX_CATEGORY SoundField
    ...)
```

There is no `juce_set_aax_sdk_path()` call in this repository and no
`AAX_SDK_PATH` variable to set. If you use a JUCE distribution that does *not*
contain `modules/juce_audio_plugin_client/AAX/SDK`, configuration will fail with
a clear JUCE error; obtain the AAX SDK from Avid and call
`juce_set_aax_sdk_path("/path/to/AAX_SDK")` before `juce_add_plugin` in that case.

## Building the AAX target

```bash
# Release
cmake --build build --config Release --target WidePocket_AAX
# Debug
cmake --build build --config Debug --target WidePocket_AAX
```

Output:

- Windows: `build/WidePocket_artefacts/<Config>/AAX/Wide Pocket.aaxplugin`
- macOS: `build/WidePocket_artefacts/<Config>/AAX/Wide Pocket.aaxplugin` (bundle)

## Signing with PACE (required)

CI artefacts are **developer builds and are not signed**. Pro Tools refuses to
load an unsigned AAX plug-in, so signing happens locally with the PACE Eden /
`wraptool` toolchain and the Avid developer account credentials.

Typical invocation (adjust to your account, GUID and certificate):

```bash
wraptool sign \
  --verbose \
  --account   <pace-account> \
  --wcguid    <your-plugin-guid> \
  --signid    "Developer ID Application: Rainline Music (TEAMID)" \
  --in        "build/WidePocket_artefacts/Release/AAX/Wide Pocket.aaxplugin" \
  --out       "signed/Wide Pocket.aaxplugin"
```

On Windows use `--keyfile` / `--keypassword` instead of `--signid`.

Notes:

- Keep the `wcguid` stable across releases; changing it invalidates existing
  authorizations.
- Sign the macOS bundle with a Developer ID identity **before or via** wraptool,
  otherwise Gatekeeper will block it.
- Never commit certificates, `.p12` files or PACE credentials. `.gitignore`
  already excludes them.

## CI behaviour

The build workflow compiles the AAX target on both Windows and macOS and uploads
the result as `WidePocket-v<version>-<platform>-AAX-Developer`. It performs no
PACE signing and never claims the artefact is release-ready. pluginval validates
the VST3 only, because pluginval cannot load AAX.
