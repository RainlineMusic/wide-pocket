/*
    Wide Pocket - plugin processor.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.
*/

#pragma once

#include <JuceHeader.h>

#include "dsp/WidePocketEngine.h"

/** One snapshot of the analyser, handed to the editor without locks. */
struct WideTrace
{
    float correlation = 1.0f;
    float left = 0.0f;
    float right = 0.0f;
};

class WidePocketAudioProcessor final : public juce::AudioProcessor
{
public:
    WidePocketAudioProcessor();

    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override {}
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout&) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    juce::AudioProcessorParameter* getBypassParameter() const override { return parameters.getParameter ("bypass"); }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return (double) getLatencySamples() / juce::jmax (1.0, getSampleRate()); }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout layout();

    /** Pops one analyser snapshot. Returns false when the queue is empty. */
    bool popTrace (WideTrace&);

    juce::AudioProcessorValueTreeState parameters;

    std::atomic<bool> editorOpen { false };
    std::atomic<bool> displayBypass { false };
    std::atomic<int> editorWidth { 0 };
    std::atomic<float> outputPeak { 0.0f };

private:
    void processAudio (juce::AudioBuffer<float>&, bool hostBypass);
    void pushParameters (bool bypassed);

    wp::WidePocketEngine engine;

    std::atomic<float>* width = nullptr;
    std::atomic<float>* focus = nullptr;
    std::atomic<float>* air = nullptr;
    std::atomic<float>* stability = nullptr;
    std::atomic<float>* sibilanceGuard = nullptr;
    std::atomic<float>* transientFocus = nullptr;
    std::atomic<float>* outputGain = nullptr;
    std::atomic<float>* bypass = nullptr;

    // Dry path used while bypassed, so switching is click free and the
    // reported latency stays valid either way.
    juce::AudioBuffer<float> bypassDelayBuffer;
    juce::AudioBuffer<float> bypassWarmBuffer;
    int bypassWritePosition = 0;

    // The vector scope needs a fast, dense point stream, not one point per
    // audio block: at 60 fps one point per block would be ~10 points a frame
    // and the display would crawl. This queue carries ~12 000 points a
    // second, which is ~200 per displayed frame.
    static constexpr int fifoSize = 8192;
    juce::AbstractFifo fifo { fifoSize };
    std::array<WideTrace, (std::size_t) fifoSize> traces {};
    int captured = 0;
    int decimation = 4;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WidePocketAudioProcessor)
};
