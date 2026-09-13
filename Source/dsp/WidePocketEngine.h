/*
    Wide Pocket - top level DSP engine.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    Signal flow, per sample:

        in ---> M/S encode ---> analyser (Mid only)
                  |
                  +--> Natural / Smart : STFT all-pass decorrelation
                  +--> Efficient       : sparse velvet noise FIR
                            |
                            +--> equal power crossfade between engines
                                       |
                     side shaping: Low Mono, Air, Transient Focus,
                     Sibilance Guard, Width, Focus
                                       |
        delayed Mid ------------> M/S decode ---> Center Lock --->
                     Correlation Guard ---> Auto Gain ---> Output

    Invariants the test suite enforces:
      - the Mid path is delayed by exactly the engine latency, so Width = 0
        reproduces the dry signal sample for sample,
      - the mono sum never contains the synthesised Side signal,
      - the latency is identical for all three engines at a given Quality, so
        switching engines is click free and does not renegotiate latency,
      - no code path can emit NaN or Inf.
*/

#pragma once

#include "DelayLine.h"
#include "Filters.h"
#include "Guards.h"
#include "MidSideRenderer.h"
#include "SmartController.h"
#include "StftDecorrelator.h"
#include "VelvetNoise.h"
#include "VocalAnalyzer.h"
#include "WidePocketTypes.h"

#include <array>

namespace wp
{

class WidePocketEngine
{
public:
    static constexpr int numBands = VocalAnalyzer::numMaskBands;
    static constexpr float maxSideGain = 1.45f;

    void prepare (double newSampleRate, int /*maximumBlockSize*/)
    {
        sampleRate = std::max (8000.0, newSampleRate);

        analyzer.prepare (sampleRate);
        stft.prepare (sampleRate, frameOrderFor (parameters.quality));
        smart.prepare (sampleRate, stft.getHopSize());

        velvet.prepare (sampleRate, 28.0f, 1800.0f, 0x1f2e3d4cu);

        latencySamples = stft.getLatencySamples();
        midDelay.prepare (latencySamples + 8);
        sideDelay.prepare (latencySamples + 8);
        velvetDelay.prepare (latencySamples + 8);
        midDelay.setDelay (latencySamples);
        sideDelay.setDelay (latencySamples);
        velvetDelay.setDelay (latencySamples);

        renderer.prepare (sampleRate);
        lowMono.prepare (sampleRate);
        airSplit.prepare (sampleRate);
        airSplit.setCutoff (6000.0f);
        correlationGuard.prepare (sampleRate);
        autoGain.prepare (sampleRate);
        centerLock.prepare (sampleRate);

        sideGainSmoother.reset (sampleRate, 20.0f, 0.0f);
        outputGainSmoother.reset (sampleRate, 20.0f, dbToGain (parameters.outputDb));
        engineFade.reset (sampleRate, 80.0f, enginePosition (parameters.engine));

        reset();
    }

    void reset()
    {
        analyzer.reset();
        stft.reset();
        smart.reset();
        velvet.reset();
        midDelay.reset();
        sideDelay.reset();
        velvetDelay.reset();
        renderer.reset();
        lowMono.reset();
        airSplit.reset();
        correlationGuard.reset();
        autoGain.reset();
        centerLock.reset();
        sideGainSmoother.snapTo (0.0f);
        snapshot = AnalyzerFrame {};
    }

    void setParameters (const Parameters& newParameters)
    {
        const bool qualityChanged = newParameters.quality != parameters.quality;
        parameters = newParameters;

        if (qualityChanged)
        {
            stft.prepare (sampleRate, frameOrderFor (parameters.quality));
            smart.prepare (sampleRate, stft.getHopSize());
            latencySamples = stft.getLatencySamples();
            midDelay.prepare (latencySamples + 8);
            sideDelay.prepare (latencySamples + 8);
            velvetDelay.prepare (latencySamples + 8);
            midDelay.setDelay (latencySamples);
            sideDelay.setDelay (latencySamples);
            velvetDelay.setDelay (latencySamples);
        }

        lowMono.setCutoff (clampf (parameters.lowMonoHz, 80.0f, 600.0f));
        outputGainSmoother.setTarget (dbToGain (clampf (parameters.outputDb, -12.0f, 12.0f)));
        engineFade.setTarget (enginePosition (parameters.engine));

        const float stability = clamp01 (parameters.stability * 0.01f);
        sideGainSmoother.setTime (sampleRate, lerp (12.0f, 120.0f, stability));
    }

    void setMlModel (MlModel* model) noexcept { smart.setModel (model); }

    int getLatencySamples() const noexcept { return latencySamples; }

    AnalyzerFrame getAnalyzerSnapshot() const noexcept { return snapshot; }

    /** Per band applied width, 0 to 1. Used by the frequency display. */
    std::array<float, numBands> getBandWidths() const noexcept { return bandWidths; }

    /**
        Processes a stereo block in place.
        A mono source should be duplicated into both channels by the caller,
        which is what a mono to stereo plugin bus layout does anyway.
    */
    void process (float* left, float* right, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float inLeft = sanitise (left[i]);
            const float inRight = sanitise (right[i]);

            const auto ms = encodeMidSide (inLeft, inRight);

            if (analyzer.pushSample (ms.mid))
                updatePerHop();

            // Both decorrelators always run, so the engine crossfade never has
            // to wait for a cold start.
            const float stftSide = stft.process (ms.mid);
            const float velvetSide = velvetDelay.process (velvet.process (ms.mid));

            const float position = engineFade.next();
            const float synthesised = blendEngines (stftSide, velvetSide, position);

            const float midAligned = midDelay.process (ms.mid);
            const float sideAligned = sideDelay.process (ms.side);

            // Auto Gain must compare like with like: the reference is the dry
            // signal *after* latency alignment, not the incoming sample.
            autoGain.measureInput (midAligned);

            const float shaped = shapeSide (synthesised);
            const float sideGain = sideGainSmoother.next();

            float side = sideAligned + sideGain * shaped;

            // Both On means a hard centre: no inter-channel level difference.
            // Center Lock works on Side only, so it cannot disturb the mono sum.
            const bool hardCentre = parameters.centerLock && parameters.monoSafe;
            centerLock.process (midAligned, side, parameters.centerLock || hardCentre);

            // Unity M/S gains: the width is already baked into `side`, and a
            // unity Mid gain is what makes Width = 0 an exact null.
            renderer.setTargets (1.0f, 1.0f);
            auto out = renderer.render (midAligned, side);

            correlationGuard.measure (out.left, out.right);
            autoGain.measureOutput (out.left, out.right);

            const float makeUp = autoGain.nextGain (parameters.autoGain);
            const float outputGain = outputGainSmoother.next() * makeUp;

            left[i] = sanitise (out.left * outputGain);
            right[i] = sanitise (out.right * outputGain);
        }

        snapshot.correlation = correlationGuard.getCorrelation();
        snapshot.appliedWidth = sideGainSmoother.value() / maxSideGain;
    }

private:
    void updatePerHop()
    {
        const auto& frame = analyzer.getFrame();
        const auto& mask = analyzer.getSpatialMask();

        snapshot = frame;

        const float widthNorm = clamp01 (parameters.width * 0.01f);
        const float focusNorm = clamp01 (parameters.focus * 0.01f);

        smart.update (frame, mask, parameters);

        std::array<float, numBands> depths {};

        if (parameters.engine == Engine::smart)
        {
            depths = smart.getBandDepths();
            smartTrim = smart.getGlobalTrim();
        }
        else
        {
            // Natural and Efficient use the static spatial mask, weighted by
            // Focus. They do not adapt frame by frame beyond the mask itself.
            for (int band = 0; band < numBands; ++band)
                depths[(std::size_t) band] = clamp01 (widthNorm
                                                      * mask[(std::size_t) band]
                                                      * SmartController::focusWeight (band, focusNorm));
            smartTrim = 1.0f;
        }

        stft.setBandDepths (depths);
        bandWidths = depths;

        // Side level: Width sets the amount, the guards take it away again.
        const float sibilanceDuck = 1.0f - clamp01 (parameters.sibilanceGuard * 0.01f) * frame.sibilance;
        const float transientDuck = 1.0f - 0.8f * clamp01 (parameters.transientFocus * 0.01f) * frame.transient;
        const float correlationTrim = correlationGuard.sideGainMultiplier (parameters.monoSafe ? 0.3f : -0.2f);

        float target = maxSideGain * widthNorm * sibilanceDuck * transientDuck * smartTrim * correlationTrim;

        if (parameters.monoSafe)
            target *= 0.85f;

        sideGainSmoother.setTarget (clampf (target, 0.0f, maxSideGain));
    }

    float shapeSide (float side) noexcept
    {
        // Low Mono: the Side signal simply has no low frequency content, which
        // is the only way to keep the bottom end solid on any system.
        float low = 0.0f, high = 0.0f;
        lowMono.process (side, low, high);
        float shaped = high;

        // Air: gentle high shelf on the Side only, never on the Mid.
        float airLow = 0.0f, airHigh = 0.0f;
        airSplit.process (shaped, airLow, airHigh);
        const float airAmount = clamp01 (parameters.air * 0.01f);
        shaped = airLow + airHigh * (1.0f + 1.2f * airAmount);

        return sanitise (shaped);
    }

    static float blendEngines (float stftSide, float velvetSide, float position) noexcept
    {
        // position: 0 = Natural, 0.5 = Efficient, 1 = Smart.
        // Natural and Smart both come from the STFT path, Efficient from the
        // velvet path, so a single equal power crossfade covers all switches.
        const float velvetWeight = 1.0f - std::abs (position - 0.5f) * 2.0f;
        float fromWeight = 0.0f, toWeight = 0.0f;
        equalPowerWeights (clamp01 (velvetWeight), fromWeight, toWeight);
        return fromWeight * stftSide + toWeight * velvetSide;
    }

    static float enginePosition (Engine engine) noexcept
    {
        switch (engine)
        {
            case Engine::natural:   return 0.0f;
            case Engine::efficient: return 0.5f;
            case Engine::smart:     return 1.0f;
        }
        return 0.0f;
    }

    static int frameOrderFor (Quality quality) noexcept
    {
        return quality == Quality::live ? 8 : 10; // 256 or 1024 samples
    }

    double sampleRate = 48000.0;
    int latencySamples = 1024;

    Parameters parameters;

    VocalAnalyzer analyzer;
    StftDecorrelator stft;
    SmartController smart;
    VelvetNoiseDecorrelator velvet;

    DelayLine midDelay, sideDelay, velvetDelay;
    MidSideRenderer renderer;
    ComplementaryCrossover lowMono, airSplit;
    CorrelationGuard correlationGuard;
    AutoGain autoGain;
    CenterLock centerLock;

    Smoother sideGainSmoother;
    Smoother outputGainSmoother;
    Smoother engineFade;

    float smartTrim = 1.0f;
    AnalyzerFrame snapshot;
    std::array<float, numBands> bandWidths {};
};

} // namespace wp
