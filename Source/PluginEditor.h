/*
    Wide Pocket - plugin editor.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    The look is carried over from Phase Pocket v0.8 on purpose: same three
    themes, same dial and button drawing, same gear menu, same power bypass
    with the frozen blurred snapshot. Only the control set and the layout are
    new.

    v0.4 UI: only Width, Air and Output are exposed. Focus, Stability,
    Sibilance and Transient stay at their defaults inside the engine, which is
    where they were measured to work best.
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

/** Compact round toggle carrying the polarity symbol, sized like the small
    Output dial so the two flank the big dials symmetrically. */
class PolarityButton final : public juce::Button
{
public:
    PolarityButton (PocketLook&, juce::uint32 accent);

    void paintButton (juce::Graphics&, bool, bool) override;

private:
    PocketLook& look;
    juce::uint32 accent;
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
    juce::Rectangle<int> scaled (float, float, float, float) const;

    WidePocketAudioProcessor& audioProcessor;
    PocketLook look;

    ModernDial widthDial { look, "Width", "Stereo spread", "%", 0xff5987ff };
    ModernDial airDial { look, "Air", "Dark to bright", "%", 0xff32d4cb };
    ModernDial outputDial { look, "Output", "dB", "", 0xfff1e84b, true, 2 };
    PolarityButton polarityButton { look, 0xff5987ff };

    juce::TextButton settingsButton { "settings" }, bypassButton { "power" };

    std::unique_ptr<SliderAttachment> widthAttach, airAttach, outputAttach;
    std::unique_ptr<ButtonAttachment> bypassAttach, polarityAttach;

    std::unique_ptr<juce::PropertiesFile> preferences;

    WideTrace latest {};

    // ~170 ms of history at the 12 kHz point rate: long enough to read as a
    // cloud, short enough to react immediately.
    std::array<juce::Point<float>, 2048> scatter {};
    int scatterCursor = 0, scatterFilled = 0;

    // Correlation trail: one sample per displayed frame, so 240 entries are
    // four seconds of history at 60 fps.
    std::array<float, 240> correlationTrail {};
    int correlationCursor = 0, correlationFilled = 0;

    bool ready = false, capturingBlur = false, bypassTarget = false;
    float bypassMix = 0.0f;
    double resizeStamp = 0.0;

    juce::Image blurredSnapshot;
    juce::Rectangle<int> blurArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WidePocketAudioProcessorEditor)
};
