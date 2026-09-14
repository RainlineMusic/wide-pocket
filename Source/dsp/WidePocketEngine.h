/*
    Wide Pocket - single Natural vocal widening engine (v0.4).
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    Every control below is a real gain on the quadrature Side, so none of them
    can move the image and they can be made as strong as they need to be.
*/
#pragma once

#include "DelayLine.h"
#include "Guards.h"
#include "MidSideRenderer.h"
#include "QuadratureFir.h"
#include "VocalAnalyzer.h"
#include "WidePocketTypes.h"

#include <array>

namespace wp
{

class WidePocketEngine
{
public:
    static constexpr int numBands = VocalAnalyzer::numMaskBands;

    void prepare (double sr, int)
    {
        sampleRate = std::max (8000.0, sr);
        analyzer.prepare (sampleRate);
        decorrelator.prepare (sampleRate);
        latencySamples = decorrelator.getLatencySamples();
        midDelay.prepare (latencySamples + 8);
        sideDelay.prepare (latencySamples + 8);
        midDelay.setDelay (latencySamples);
        sideDelay.setDelay (latencySamples);
        renderer.prepare (sampleRate);
        renderer.setTargets (1.0f, 1.0f);
        correlationGuard.prepare (sampleRate);
        outputGainSmoother.reset (sampleRate, 20.0f, dbToGain (parameters.outputDb));
        guardSmoother.reset (sampleRate, 90.0f, 1.0f);
        polaritySmoother.reset (sampleRate, 12.0f, parameters.invertSide ? -1.0f : 1.0f);
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
        polaritySmoother.snapTo (parameters.invertSide ? -1.0f : 1.0f);
        snapshot = AnalyzerFrame {};
        bandWidths.fill (0.0f);
    }

    void setParameters (const Parameters& p) noexcept
    {
        parameters = p;
        outputGainSmoother.setTarget (dbToGain (clampf (parameters.outputDb, -24.0f, 12.0f)));

        // A 12 ms ramp through zero: fast enough to feel instant, slow enough
        // that flipping polarity while audio is running cannot click.
        polaritySmoother.setTarget (parameters.invertSide ? -1.0f : 1.0f);

        const float stability = clamp01 (parameters.stability * 0.01f);

        // Stability is the only control that is a time and not a gain: it sets
        // how quickly the per-band widths and the guards may move.
        decorrelator.setGainSmoothingMs (lerp (70.0f, 520.0f, stability));
        guardSmoother.setTime (sampleRate, lerp (45.0f, 160.0f, stability));
    }

    int getLatencySamples() const noexcept { return latencySamples; }
    AnalyzerFrame getAnalyzerSnapshot() const noexcept { return snapshot; }
    std::array<float, numBands> getBandWidths() const noexcept { return bandWidths; }

    void process (float* l, float* r, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const auto ms = encodeMidSide (sanitise (l[i]), sanitise (r[i]));

            if (analyzer.pushSample (ms.mid))
                updatePerHop();

            // The onset amount is a continuous, per-sample envelope feature,
            // so the duck moves like a compressor rather than switching.
            decorrelator.setTransientAmount (analyzer.getFrame().transient * 1.5f);
            const float generated = decorrelator.process (ms.mid);
            const float mid = midDelay.process (ms.mid);
            const float side = (sideDelay.process (ms.side) + guardSmoother.next() * generated)
                               * polaritySmoother.next();

            const auto out = renderer.render (mid, side);
            correlationGuard.measure (out.left, out.right);

            const float gain = outputGainSmoother.next();
            l[i] = sanitise (out.left * gain);
            r[i] = sanitise (out.right * gain);
        }

        snapshot.correlation = correlationGuard.getCorrelation();
        snapshot.appliedWidth = clamp01 (averageBandGain * guardSmoother.value() * decorrelator.getDuck());
    }

private:
    void updatePerHop() noexcept
    {
        const auto& frame = analyzer.getFrame();
        snapshot = frame;

        const float width = clamp01 (parameters.width * 0.01f);
        const float focus = clamp01 (parameters.focus * 0.01f);
        const float air = clampf (parameters.air * 0.01f, -1.0f, 1.0f);
        const float sibilanceAmount = clamp01 (parameters.sibilanceGuard * 0.01f);

        // The analyser detectors sit around 0.1 to 0.35 on normal vocal
        // material, so they are used expanded: otherwise a control with a 40 %
        // range ends up doing 1 dB and feels broken.
        const float sibilance = clamp01 (frame.sibilance * 2.2f);

        // The width law is flatter than before (0.55 instead of 0.82): the deep
        // Focus notch and the sibilance band gain take level out of the Side,
        // so the middle of the Width range has to start higher to stay as wide
        // as the old build felt.
        const float base = 1.28f * std::pow (width, 0.55f);
        const auto& mask = analyzer.getSpatialMask();

        float sum = 0.0f, weights = 0.0f;

        for (int b = 0; b < numBands; ++b)
        {
            const float centre = bandCentreHz (b);

            // Focus: how much narrower the intelligibility range stays than the
            // rest of the voice. Up to -16 dB of Side, about one octave wide
            // around 1.25 kHz, so it reads as the voice itself staying in the
            // middle rather than as a tone change.
            const float presence = std::exp (-std::pow (std::log2 (centre / 1250.0f) / 1.1f, 2.0f));
            const float focusGain = 1.0f - 0.85f * focus * presence;

            // Air is a bipolar tone tilt of the Side hinged at 1.2 kHz:
            // +100 % is about +7 dB on the top octaves, -100 % about -10 dB,
            // and 0 leaves the Side with the same tone as the Mid. This is what
            // makes a dark, close Side possible at all.
            const float tilt = clamp01 (std::log2 (std::max (centre, 1200.0f) / 1200.0f) / 2.4f);
            const float airGain = air >= 0.0f ? 1.0f + 1.25f * air * tilt
                                              : 1.0f / (1.0f + 2.2f * (-air) * tilt);

            // No hard cutoff, but the Side ramps in from 120 Hz so the low end
            // cannot turn into rumble.
            const float p = clamp01 (std::log2 (std::max (centre, 120.0f) / 120.0f) / 2.2f);
            const float low = lerp (0.02f, 1.0f, p * p * (3.0f - 2.0f * p));

            // Sibilance guard is a band gain now, not a broadband duck: only
            // the S/SH region above 4 kHz is held back, and only while the
            // analyser actually sees sibilance. Up to -14 dB there.
            const float sibWeight = clamp01 (std::log2 (std::max (centre, 4000.0f) / 4000.0f) / 1.2f);
            const float sibGain = 1.0f - 0.8f * sibilanceAmount * sibilance * sibWeight;

            // Sustained harmonics get slightly less Side than noisy or
            // consonant bands. Kept shallow (0.6 dB) because the deeper version
            // is part of why the Side measured brighter than the Mid.
            const float tonal = lerp (0.93f, 1.0f, clamp01 (mask[(size_t) b] * 3.0f));

            const float gain = clampf (base * focusGain * airGain * low * sibGain * tonal,
                                       0.0f, QuadratureFir::maxBandGain);
            bandWidths[(size_t) b] = gain;

            const float weight = centre < 6000.0f ? 1.0f : 0.5f;
            sum += gain * weight;
            weights += weight;
        }

        decorrelator.setBandGains (bandWidths);
        averageBandGain = sum / std::max (1.0f, weights);

        // Transient drives the smooth onset duck inside the FIR stage instead
        // of a fixed hard gate, so the control is audible and the Side no
        // longer stops dead.
        const float transientAmount = clamp01 (parameters.transientFocus * 0.01f);
        decorrelator.setDuckDepth (0.9f * transientAmount);

        // The only remaining broadband guard is the plosive one: a p or a
        // breath burst is the one case where a wide Side is always wrong.
        guardSmoother.setTarget (clampf (1.0f - 0.5f * frame.plosive, 0.4f, 1.0f));
    }

    static float bandCentreHz (int b) noexcept
    {
        return VocalAnalyzer::bandLowEdgeHz (b) * std::pow (2.0f, 0.375f);
    }

    double sampleRate = 48000.0;
    int latencySamples = 127;
    Parameters parameters;
    VocalAnalyzer analyzer;
    QuadratureFir decorrelator;
    DelayLine midDelay, sideDelay;
    MidSideRenderer renderer;
    CorrelationGuard correlationGuard;
    Smoother guardSmoother, outputGainSmoother, polaritySmoother;
    float averageBandGain = 0.0f;
    AnalyzerFrame snapshot;
    std::array<float, numBands> bandWidths {};
};

} // namespace wp
