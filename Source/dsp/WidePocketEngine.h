/*
    Wide Pocket - single Natural vocal widening engine.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    The delayed original Mid is immutable. A DAFx23-style subband allpass
    decorrelator produces Side, restores its t/F envelope and removes its
    in-phase projection in perceptual bands. There is no time-domain centre
    servo, no velvet-noise path, no engine crossfade and no low-mono filter.
*/
#pragma once

#include "DelayLine.h"
#include "Guards.h"
#include "MidSideRenderer.h"
#include "StftDecorrelator.h"
#include "VocalAnalyzer.h"
#include "WidePocketTypes.h"

#include <array>

namespace wp
{

class WidePocketEngine
{
public:
    static constexpr int numBands = VocalAnalyzer::numMaskBands;

    void prepare (double newSampleRate, int /*maximumBlockSize*/)
    {
        sampleRate = std::max (8000.0, newSampleRate);
        analyzer.prepare (sampleRate);
        decorrelator.prepare (sampleRate, 8);
        latencySamples = decorrelator.getLatencySamples();
        midDelay.prepare (latencySamples + 8);
        sideDelay.prepare (latencySamples + 8);
        midDelay.setDelay (latencySamples);
        sideDelay.setDelay (latencySamples);
        renderer.prepare (sampleRate);
        correlationGuard.prepare (sampleRate);
        outputGainSmoother.reset (sampleRate, 20.0f, dbToGain (parameters.outputDb));
        guardSmoother.reset (sampleRate, 90.0f, 1.0f);
        reset();
    }

    void reset()
    {
        analyzer.reset();
        decorrelator.reset();
        midDelay.reset();
        sideDelay.reset();
        renderer.reset (1.0f, 1.0f);
        correlationGuard.reset();
        guardSmoother.snapTo (1.0f);
        snapshot = AnalyzerFrame {};
        bandWidths.fill (0.0f);
    }

    void setParameters (const Parameters& newParameters) noexcept
    {
        parameters = newParameters;
        outputGainSmoother.setTarget (dbToGain (clampf (parameters.outputDb, -24.0f, 12.0f)));
        const float stability = clamp01 (parameters.stability * 0.01f);
        decorrelator.setGainSmoothingMs (lerp (180.0f, 520.0f, stability));
        guardSmoother.setTime (sampleRate, lerp (70.0f, 160.0f, stability));
    }

    int getLatencySamples() const noexcept { return latencySamples; }
    AnalyzerFrame getAnalyzerSnapshot() const noexcept { return snapshot; }
    std::array<float, numBands> getBandWidths() const noexcept { return bandWidths; }

    void process (float* left, float* right, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float inLeft = sanitise (left[i]);
            const float inRight = sanitise (right[i]);
            const auto ms = encodeMidSide (inLeft, inRight);

            if (analyzer.pushSample (ms.mid))
                updatePerHop();

            const float synthesisedSide = decorrelator.process (ms.mid);
            const float delayedMid = midDelay.process (ms.mid);
            const float delayedOriginalSide = sideDelay.process (ms.side);
            const float guardedSide = guardSmoother.next() * synthesisedSide;

            // The direct Mid is never corrected; existing stereo Side is
            // preserved verbatim.
            const float side = delayedOriginalSide + guardedSide;
            renderer.setTargets (1.0f, 1.0f);
            const auto out = renderer.render (delayedMid, side);
            correlationGuard.measure (out.left, out.right);

            const float outputGain = outputGainSmoother.next();
            left[i] = sanitise (out.left * outputGain);
            right[i] = sanitise (out.right * outputGain);
        }

        snapshot.correlation = correlationGuard.getCorrelation();
        snapshot.appliedWidth = clamp01 (averageBandGain * guardSmoother.value());
    }

private:
    void updatePerHop() noexcept
    {
        const auto& frame = analyzer.getFrame();
        snapshot = frame;

        const float width = clamp01 (parameters.width * 0.01f);
        const float focus = clamp01 (parameters.focus * 0.01f);
        const float air = clamp01 (parameters.air * 0.01f);

        // A perceptual width law: full scale approaches Side/Mid ~= 0.8 while
        // the first half of the control remains easy to mix.
        const float base = 0.93f * std::pow (width, 0.82f);
        float weightedSum = 0.0f, weightTotal = 0.0f;

        for (int band = 0; band < numBands; ++band)
        {
            const float centre = bandCentreHz (band);
            const float presence = std::exp (-std::pow (std::log2 (centre / 1100.0f) / 1.7f, 2.0f));
            const float high = clamp01 (std::log2 (std::max (centre, 3500.0f) / 3500.0f) / 1.8f);

            // Focus never closes a band; it only anchors the formant region.
            // No low-mono curve is present: all bands may widen.
            const float focusGain = 1.0f - 0.42f * focus * presence;
            const float airGain = 1.0f + 0.38f * air * high;

            // The low bands still widen, but progressively. This is not the
            // removed 150 Hz mono switch: there is no cutoff and no band is
            // muted. It prevents sparse sub-fundamental bins from being louder
            // than the vocal itself when Width is at 100%.
            const float lowProgress = clamp01 (std::log2 (std::max (centre, 80.0f) / 80.0f) / 3.0f);
            const float lowProtection = lerp (0.32f, 1.0f, lowProgress * lowProgress * (3.0f - 2.0f * lowProgress));
            const float gain = clampf (base * focusGain * airGain * lowProtection,
                                       0.0f, StftDecorrelator::maxBandGain);
            bandWidths[(std::size_t) band] = gain;

            // Weight the displayed/applied amount towards the vocal range.
            const float weight = centre < 6000.0f ? 1.0f : 0.5f;
            weightedSum += gain * weight;
            weightTotal += weight;
        }
        decorrelator.setBandGains (bandWidths);
        averageBandGain = weightedSum / std::max (1.0f, weightTotal);

        // Slow, scalar protection cannot move the source. The STFT stage has
        // its own 8-frame transient hold; this extra guard gently controls
        // plosives and excessive sibilance without killing voiced sustain.
        const float sibilanceAmount = clamp01 (parameters.sibilanceGuard * 0.01f);
        const float transientAmount = clamp01 (parameters.transientFocus * 0.01f);
        const float sibilanceGain = 1.0f - 0.38f * sibilanceAmount * frame.sibilance;
        const float onsetGain = 1.0f - 0.55f * transientAmount * frame.transient;
        const float plosiveGain = 1.0f - 0.45f * frame.plosive;
        guardSmoother.setTarget (clampf (sibilanceGain * onsetGain * plosiveGain, 0.18f, 1.0f));
    }

    static float bandCentreHz (int band) noexcept
    {
        return VocalAnalyzer::bandLowEdgeHz (band) * std::pow (2.0f, 0.375f);
    }

    double sampleRate = 48000.0;
    int latencySamples = 256;
    Parameters parameters;
    VocalAnalyzer analyzer;
    StftDecorrelator decorrelator;
    DelayLine midDelay, sideDelay;
    MidSideRenderer renderer;
    CorrelationGuard correlationGuard;
    Smoother guardSmoother, outputGainSmoother;
    float averageBandGain = 0.0f;
    AnalyzerFrame snapshot;
    std::array<float, numBands> bandWidths {};
};

} // namespace wp
