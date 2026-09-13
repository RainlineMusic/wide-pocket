/*
    Wide Pocket - plugin editor.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    The look is carried over from Phase Pocket v0.8 on purpose: same three
    themes, same dial and button drawing, same gear menu, same power bypass
    with the frozen blurred snapshot. Only the control set and the layout are
    new.
*/

#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"

enum class PocketTheme { Neon, SolidDark, SolidWhite };

class PocketLook final : public juce::LookAndFeel_V4
{
public:
    PocketTheme theme = PocketTheme::Neon;

    bool isDark() const { return theme != PocketTheme::SolidWhite; }
    bool isNeon() const { return theme == PocketTheme::Neon; }

    juce::Colour pick (juce::uint32 neon, juce::uint32 dark, juce::uint32 white) const;
    juce::Colour ink() const;
    juce::Colour muted() const;

    juce::Font getTextButtonFont (juce::TextButton&, int) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool, bool) override;
    void drawLinearSlider (juce::Graphics&, int, int, int, int, float, float, float,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool, bool) override;
};

/** Rotary dial, identical drawing to v0.8: 200 px design size, 100 px compact. */
class ModernDial final : public juce::Slider
{
public:
    ModernDial (PocketLook&, juce::String title, juce::String subtitle, juce::String unit,
                juce::uint32 accent, bool compact = false, int decimals = 0);

    void paint (juce::Graphics&) override;

private:
    PocketLook& look;
    juce::String title, subtitle, unit;
    juce::uint32 accent;
    bool compact;
    int decimals;
};

/** Three position Engine switch, drawn in the same style as the dials. */
class EngineSelector final : public juce::Component,
                            public juce::SettableTooltipClient
{
public:
    explicit EngineSelector (PocketLook&);

    std::function<void (int)> onChange;

    void setIndex (int newIndex, bool notify);
    int getIndex() const noexcept { return index; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    PocketLook& look;
    int index = 0;
    const juce::StringArray names { "Natural", "Efficient", "Smart" };
};

class WidePocketAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit WidePocketAudioProcessorEditor (WidePocketAudioProcessor&);
    ~WidePocketAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void timerCallback() override;
    void setTheme (PocketTheme, bool persist = true);
    void showSettingsMenu();
    void captureBlurSnapshot();
    void saveSize();

    void panel (juce::Graphics&, juce::Rectangle<float>);
    void vectorScope (juce::Graphics&, juce::Rectangle<float>);
    void drawer (juce::Graphics&, juce::Rectangle<float>);
    void readouts (juce::Graphics&, juce::Rectangle<float>);
    void setDrawerOpen (bool);
    juce::Rectangle<int> scaled (float, float, float, float) const;

    WidePocketAudioProcessor& audioProcessor;
    PocketLook look;

    ModernDial widthDial { look, "Width", "Stereo spread", "%", 0xff5987ff };
    ModernDial focusDial { look, "Focus", "Intelligibility", "%", 0xff32d4cb };
    ModernDial airDial { look, "Air", "Top octaves", "%", 0xfff1e84b, true };
    ModernDial stabilityDial { look, "Stability", "Adaptation", "%", 0xff5987ff, true };
    ModernDial sibilanceDial { look, "Sibilance", "Guard", "%", 0xfff1e84b, true };
    ModernDial transientDial { look, "Transient", "Focus", "%", 0xff5987ff, true };
    ModernDial outputDial { look, "Output", "dB", "dB", 0xfff1e84b, true, 2 };

    EngineSelector engineSelector { look };

    juce::TextButton settingsButton { "settings" }, bypassButton { "power" };

    // Engine and the analysis readouts live behind this arrow, the same way
    // the Sidechain drawer works in Phase Pocket. They are development
    // instruments, not everyday controls.
    juce::TextButton drawerButton { "Engine & analysis" };

    std::unique_ptr<SliderAttachment> widthAttach, focusAttach, airAttach, stabilityAttach,
        sibilanceAttach, transientAttach, outputAttach;
    std::unique_ptr<ButtonAttachment> bypassAttach;
    std::unique_ptr<juce::ParameterAttachment> engineAttach, qualityAttach;

    std::unique_ptr<juce::PropertiesFile> preferences;

    WideTrace latest {};
    std::array<float, wp::VocalAnalyzer::numMaskBands> smoothedBands {};

    // ~170 ms of history at the 12 kHz point rate: long enough to read as a
    // cloud, short enough to react immediately.
    std::array<juce::Point<float>, 2048> scatter {};
    int scatterCursor = 0, scatterFilled = 0;

    bool drawerOpen = false;
    bool qualityLive = false;

    bool ready = false, capturingBlur = false, bypassTarget = false;
    float bypassMix = 0.0f;
    double resizeStamp = 0.0;

    juce::Image blurredSnapshot;
    juce::Rectangle<int> blurArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WidePocketAudioProcessorEditor)
};
