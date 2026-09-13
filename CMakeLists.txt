# Wide Pocket - build definition.
# Copyright (c) 2026 Rainline Music. All Rights Reserved.
#
# Local:
#   cmake -B build -DJUCE_DIR=/path/to/JUCE -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --target WidePocketTests
#   ctest --test-dir build --output-on-failure
#   cmake --build build --target WidePocket_VST3 WidePocket_AAX

cmake_minimum_required(VERSION 3.22)

project(WidePocket VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

# ---------------------------------------------------------------------------
# DSP core tests. Deliberately JUCE free, so they build and run anywhere,
# including a bare container with nothing but a compiler.
# ---------------------------------------------------------------------------

add_executable(WidePocketTests Tests/dsp_core_test.cpp)
target_compile_features(WidePocketTests PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(WidePocketTests PRIVATE /W4)
else()
    target_compile_options(WidePocketTests PRIVATE -Wall -Wextra)
endif()

add_test(NAME WidePocketDspCore COMMAND WidePocketTests)

# ---------------------------------------------------------------------------
# Plugin. Skipped when JUCE is not present, so the tests stay buildable alone.
# ---------------------------------------------------------------------------

set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" CACHE PATH "Path to the JUCE source tree")

if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(WARNING "JUCE not found at ${JUCE_DIR}: only the WidePocketTests target will be built.")
    return()
endif()

add_subdirectory("${JUCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

juce_add_plugin(WidePocket
    PRODUCT_NAME "Wide Pocket"
    COMPANY_NAME "Rainline Music"
    COMPANY_EMAIL "rainlinemusic@gmail.com"
    BUNDLE_ID com.rainlinemusic.widepocket
    PLUGIN_MANUFACTURER_CODE RnLn
    PLUGIN_CODE WdPk
    FORMATS VST3 AAX Standalone
    VST3_CATEGORIES Fx Spatial
    AAX_CATEGORY AAX_EPlugInCategory_SoundField
    AAX_IDENTIFIER com.rainlinemusic.widepocket
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

juce_generate_juce_header(WidePocket)

target_sources(WidePocket PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(WidePocket PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(WidePocket
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
