/*
    Wide Pocket - plugin processor.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

WidePocketAudioProcessor::WidePocketAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "PARAMETERS", layout())
{
    width = parameters.getRawParameterValue ("width");
    focus = parameters.getRawParameterValue ("focus");
    air = parameters.getRawParameterValue ("air");
    stability = parameters.getRawParameterValue ("stability");
    sibilanceGuard = parameters.getRawParameterValue ("sibilanceGuard");
    transientFocus = parameters.getRawParameterValue ("transientFocus");
    outputGain = parameters.getRawParameterValue ("output");
    invertSide = parameters.getRawParameterValue ("invertSide");
    bypass = parameters.getRawParameterValue ("bypass");
}

juce::AudioProcessorValueTreeState::ParameterLayout WidePocketAudioProcessor::layout()
{
    using Float = juce::AudioParameterFloat;
    using Bool = juce::AudioParameterBool;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

    p.push_back (std::make_unique<Float> (juce::ParameterID { "width", 1 }, "Width",
                                         juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));
    p.push_back (std::make_unique<Float> (juce::ParameterID { "focus", 1 }, "Focus",
                                         juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));
    // Air is a bipolar tone tilt of the Side now: negative darkens it,
    // positive opens the top octaves, zero keeps the tone of the Mid.
    p.push_back (std::make_unique<Float> (juce::ParameterID { "air", 1 }, "Air",
                                         juce::NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f));
    p.push_back (std::make_unique<Float> (juce::ParameterID { "stability", 1 }, "Stability",
                                         juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));
    p.push_back (std::make_unique<Float> (juce::ParameterID { "sibilanceGuard", 1 }, "Sibilance Guard",
                                         juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 60.0f));
    p.push_back (std::make_unique<Float> (juce::ParameterID { "transientFocus", 1 }, "Transient Focus",
                                         juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 60.0f));
    p.push_back (std::make_unique<Float> (juce::ParameterID { "output", 1 }, "Output",
                                         juce::NormalisableRange<float> (-24.0f, 12.0f, 0.01f), 0.0f));

    // No Mono Safe / Center Lock / Auto Gain parameters. The mono sum and the
    // centred image are structural properties of the engine now, so there is
    // nothing to switch and nothing for the user to get wrong.
    //
    // Side polarity is the one exception worth a switch. The mono sum does not
    // contain the Side at all, so this cannot have a mono consequence: it only
    // decides which ear leads in phase, so a widened mono source can be made
    // to lean the same way it is panned instead of fighting the pan.
    p.push_back (std::make_unique<Bool> (juce::ParameterID { "invertSide", 1 }, "Side Polarity", false));
    p.push_back (std::make_unique<Bool> (juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return { p.begin(), p.end() };
}

bool WidePocketAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::stereo())
        return false;

    // A widener is useful exactly in these two cases: a mono vocal placed into
    // stereo, and an already stereo bus that needs more spread.
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void WidePocketAudioProcessor::prepareToPlay (double sampleRate, int maximumBlockSize)
{
    engine.prepare (sampleRate, maximumBlockSize);
    pushParameters (bypass->load() > 0.5f);
    setLatencySamples (engine.getLatencySamples());

    bypassDelayBuffer.setSize (2, juce::jmax (1, engine.getLatencySamples() + 1), false, true, true);
    bypassDelayBuffer.clear();
    bypassWarmBuffer.setSize (2, juce::jmax (1, maximumBlockSize), false, true, true);
    bypassWarmBuffer.clear();
    bypassWritePosition = 0;

    decimation = juce::jmax (1, (int) (sampleRate / 12000.0));
    captured = 0;
}

void WidePocketAudioProcessor::reset()
{
    engine.reset();
    bypassDelayBuffer.clear();
    bypassWarmBuffer.clear();
    bypassWritePosition = 0;
    captured = 0;
}

void WidePocketAudioProcessor::pushParameters (bool bypassed)
{
    wp::Parameters p;
    p.width = bypassed ? 0.0f : width->load();
    p.focus = focus->load();
    p.air = air->load();
    p.stability = stability->load();
    p.sibilanceGuard = sibilanceGuard->load();
    p.transientFocus = transientFocus->load();
    p.outputDb = bypassed ? 0.0f : outputGain->load();
    p.invertSide = invertSide->load() > 0.5f;
    engine.setParameters (p);
}

void WidePocketAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    processAudio (buffer, false);
}

void WidePocketAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    processAudio (buffer, true);
}

void WidePocketAudioProcessor::processAudio (juce::AudioBuffer<float>& buffer, bool hostBypass)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numInputChannels = juce::jmax (1, getMainBusNumInputChannels());
    const int numOutputChannels = buffer.getNumChannels();

    if (numSamples <= 0 || numOutputChannels <= 0)
        return;

    const bool bypassed = hostBypass || bypass->load() > 0.5f;
    displayBypass.store (bypassed, std::memory_order_relaxed);

    // A mono input feeds both channels, which is the intended use case.
    if (numOutputChannels > 1 && numInputChannels == 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    if (bypassed)
    {
        // Keep the recursive decorrelator warm while returning latency-aligned
        // dry audio. The scratch buffer is allocated in prepareToPlay.
        if (bypassWarmBuffer.getNumSamples() >= numSamples)
        {
            bypassWarmBuffer.copyFrom (0, 0, buffer, 0, 0, numSamples);
            bypassWarmBuffer.copyFrom (1, 0, buffer, numOutputChannels > 1 ? 1 : 0, 0, numSamples);
            pushParameters (true);
            engine.process (bypassWarmBuffer.getWritePointer (0), bypassWarmBuffer.getWritePointer (1), numSamples);
        }

        // Hold the dry signal back by the reported latency so that engaging or
        // releasing bypass never shifts the timing against other tracks.
        const int delayLength = bypassDelayBuffer.getNumSamples();
        const int latency = engine.getLatencySamples();

        if (delayLength > 0 && latency > 0)
        {
            for (int n = 0; n < numSamples; ++n)
            {
                const int readPosition = (bypassWritePosition + delayLength - latency) % delayLength;

                for (int channel = 0; channel < juce::jmin (2, numOutputChannels); ++channel)
                {
                    const float dry = buffer.getSample (channel, n);
                    buffer.setSample (channel, n, bypassDelayBuffer.getSample (channel, readPosition));
                    bypassDelayBuffer.setSample (channel, bypassWritePosition, dry);
                }

                bypassWritePosition = (bypassWritePosition + 1) % delayLength;
            }
        }

        outputPeak.store (buffer.getMagnitude (0, numSamples));
        return;
    }

    pushParameters (false);

    // Prime bypass with the original dry input, never the processed output.
    const int delayLength = bypassDelayBuffer.getNumSamples();
    if (delayLength > 0)
    {
        for (int n = 0; n < numSamples; ++n)
        {
            bypassDelayBuffer.setSample (0, bypassWritePosition, buffer.getSample (0, n));
            bypassDelayBuffer.setSample (1, bypassWritePosition,
                                         buffer.getSample (numOutputChannels > 1 ? 1 : 0, n));
            bypassWritePosition = (bypassWritePosition + 1) % delayLength;
        }
    }

    auto* left = buffer.getWritePointer (0);
    auto* right = numOutputChannels > 1 ? buffer.getWritePointer (1) : left;

    engine.process (left, right, numSamples);

    outputPeak.store (buffer.getMagnitude (0, numSamples));

    if (! editorOpen.load (std::memory_order_relaxed))
    {
        captured = 0;
        return;
    }

    const auto snapshot = engine.getAnalyzerSnapshot();
    // One point every `decimation` samples, so the scope receives a dense,
    // evenly spaced stream instead of a single point per block.
    for (int n = 0; n < numSamples; ++n)
    {
        if (++captured < decimation)
            continue;

        captured = 0;

        WideTrace trace;
        trace.correlation = snapshot.correlation;
        trace.left = left[n];
        trace.right = right[n];

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (1, start1, size1, start2, size2);

        if (size1 <= 0)
            break; // the editor is not keeping up; drop the rest of the block

        traces[(std::size_t) start1] = trace;
        fifo.finishedWrite (1);
    }
}

bool WidePocketAudioProcessor::popTrace (WideTrace& value)
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToRead (1, start1, size1, start2, size2);

    if (size1 <= 0)
        return false;

    value = traces[(std::size_t) start1];
    fifo.finishedRead (1);
    return true;
}

void WidePocketAudioProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    auto state = parameters.copyState();
    state.setProperty ("uiWidth", editorWidth.load(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destination);
}

void WidePocketAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (parameters.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            editorWidth.store ((int) state.getProperty ("uiWidth", 0));
            parameters.replaceState (state);
        }
    }
}

juce::AudioProcessorEditor* WidePocketAudioProcessor::createEditor()
{
    return new WidePocketAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WidePocketAudioProcessor();
}
