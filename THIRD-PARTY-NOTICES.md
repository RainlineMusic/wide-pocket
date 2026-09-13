# Third-party notices

Wide Pocket links against third-party components that are licensed separately
from the proprietary Wide Pocket source code. None of these components are
vendored in this repository; they are fetched at build time.

## JUCE 8.0.4

Copyright (c) Raw Material Software Limited.

Wide Pocket is built with the JUCE framework (version 8.0.4, pinned).
JUCE is dual-licensed: the JUCE modules used here (`juce_core`, `juce_events`,
`juce_graphics`, `juce_gui_basics`, `juce_gui_extra`, `juce_audio_basics`,
`juce_audio_processors`, `juce_audio_plugin_client`, `juce_audio_utils`,
`juce_dsp`) are used under a commercial JUCE license held by Rainline Music, or
under the AGPLv3 where applicable.

The full JUCE license text ships with the JUCE distribution
(`JUCE/LICENSE.md`) and applies unmodified. JUCE end-user license terms:
https://juce.com/legal/juce-8-licence/

## Steinberg VST 3 SDK

VST is a trademark of Steinberg Media Technologies GmbH, registered in Europe
and other countries. The VST 3 interfaces used through
`juce_audio_plugin_client` are governed by the Steinberg VST 3 SDK licensing
terms (dual GPLv3 / proprietary Steinberg VST 3 license). Rainline Music is
registered as a VST 3 plug-in vendor with Steinberg.

## Avid AAX SDK

The AAX SDK is redistributed inside the JUCE distribution at
`JUCE/modules/juce_audio_plugin_client/AAX/SDK/` and is used under the Avid AAX
SDK license agreement. Pro Tools, AAX and Avid are trademarks of Avid
Technology, Inc. AAX binaries must be digitally signed with PACE Anti-Piracy
tooling before Pro Tools will load them; unsigned AAX artefacts produced by CI
are developer builds only.

## pluginval

Copyright (c) Tracktion Software Corporation. Licensed under the GPLv3.
pluginval is used as an external validation executable in CI and is neither
linked into nor distributed with Wide Pocket.

## GitHub Actions

CI uses `actions/checkout`, `actions/cache` and `actions/upload-artifact`,
licensed under the MIT License by GitHub, Inc.
