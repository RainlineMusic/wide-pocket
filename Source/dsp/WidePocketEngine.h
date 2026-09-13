/*
    Wide Pocket - top level DSP engine.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    Signal flow, per sample:

        in ---> M/S encode ---> analyser (Mid only)
                  |
                  +--> Natural / Smart : STFT quadrature Side generator
                  +--> Efficient       : sparse velvet noise FIR
                            |
                            +--> equal power crossfade between engines
                                       |
                          real, slow scalar guards
                     (Sibilance Guard, Transient Focus, Smart trim)
                                       |
        delayed Mid ------> M/S decode ---> output gain ---> out
                    (centre alignment safety net in between)

    Where the spatial shaping happens, and why it matters:

      - Width, Focus, Air, the spatial mask and the low-end mono-ing are all
        *real per-band gains* handed to the STFT stage. Real gains on a
        quadrature Side cannot create an inter-channel level difference, so
        none of them can move the image.
      - Sibilance Guard and Transient Focus are *real scalar gains* on the
        Side. Same argument: they change how wide the voice is, never where it
        is. Previously Air was a minimum-phase shelf and the guards fought a
        broadband correction stage, which is exactly why sibilants pulled
        right and vowels pulled left.

    Invariants the test suite enforces:
      - the Mid path is a pure delay of exactly the reported latency, so
        Width = 0 reproduces the dry signal sample for sample,
      - the mono sum is always 2 * Mid: the Side can never reach it,
      - the inter-channel level difference stays at zero for every parameter
        setting, including full Air, full Sibilance Guard and full Transient
        Focus, and on breath, sibilant and vowel material,
      - the latency is identical for all three engines at a given Quality,
      - no code path can emit NaN or Inf.
*/

#pragma once

#include "CenterAlignment.h"
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
        velvetLowCut.prepare (sampleRate);
        velvetLowCut.setCutoff (lowMonoHz);
        velvetAirSplit.prepare (sampleRate);
        velvetAirSplit.setCutoff (6000.0f);

        correlationGuard.prepare (sampleRate);
        alignment.prepare (sampleRate);

        guardSmoother.reset (sampleRate, 40.0f, 1.0f);
        velvetGainSmoother.reset (sampleRate, 40.0f, 0.0f);
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
        velvetLowCut.reset();
        velvetAirSplit.reset();
        correlationGuard.reset();
        alignment.reset();
        guardSmoother.snapTo (1.0f);
        velvetGainSmoother.snapTo (0.0f);
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

        outputGainSmoother.setTarget (dbToGain (clampf (parameters.outputDb, -12.0f, 12.0f)));
        engineFade.setTarget (enginePosition (parameters.engine));

        // Stability decides how fast anything spatial is allowed to move. It
        // is the only control over the time behaviour of the image.
        const float stability = clamp01 (parameters.stability * 0.01f);
        const float bandMs = lerp (40.0f, 400.0f, stability);
        stft.setGainSmoothingMs (bandMs);
        guardSmoother.setTime (sampleRate, lerp (15.0f, 90.0f, stability));
        velvetGainSmoother.setTime (sampleRate, bandMs);
    }

    void setMlModel (MlModel* model) noexcept { smart.setModel (model); }

    int getLatencySamples() const noexcept { return latencySamples; }

    AnalyzerFrame getAnalyzerSnapshot() const noexcept { return snapshot; }

    /** Per band applied width. Used by the width-by-frequency display. */
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
            const float velvetSide = shapeVelvet (velvetDelay.process (velvet.process (ms.mid)));

            const float position = engineFade.next();
            const float synthesised = blendEngines (stftSide, velvetSide, position);

            const float midAligned = midDelay.process (ms.mid);
            const float sideAligned = sideDelay.process (ms.side);

            // A real, slowly moving scalar: it scales the Side, it cannot
            // rotate it, so the centre is unaffected.
            const float guard = guardSmoother.next();

            float side = sideAligned + guard * synthesised;

            // Safety net only. With the quadrature engines its coefficients
            // sit at zero and it is a no-op.
            alignment.process (midAligned, side);

            // Unity M/S gains: the width is already baked into `side`, and a
            // unity Mid gain is what makes Width = 0 an exact null.
            renderer.setTargets (1.0f, 1.0f);
            auto out = renderer.render (midAligned, side);

            correlationGuard.measure (out.left, out.right);

            const float outputGain = outputGainSmoother.next();

            left[i] = sanitise (out.left * outputGain);
            right[i] = sanitise (out.right * outputGain);
        }

        snapshot.correlation = correlationGuard.getCorrelation();
        snapshot.appliedWidth = clamp01 (averageBandGain * guardSmoother.value());
    }

private:
    /** Low edge of the region that always stays mono. */
    static constexpr float lowMonoHz = 150.0f;

    void updatePerHop()
    {
        const auto& frame = analyzer.getFrame();
        const auto& mask = analyzer.getSpatialMask();

        snapshot = frame;

        const float widthNorm = clamp01 (parameters.width * 0.01f);
        const float focusNorm = clamp01 (parameters.focus * 0.01f);
        const float airNorm = clamp01 (parameters.air * 0.01f);

        smart.update (frame, mask, parameters);

        const bool useSmart = parameters.engine == Engine::smart;
        const auto& smartDepths = smart.getBandDepths();
        const float smartTrim = useSmart ? smart.getGlobalTrim() : 1.0f;

        std::array<float, numBands> gains {};
        float sum = 0.0f;

        for (int band = 0; band < numBands; ++band)
        {
            const auto index = (std::size_t) band;

            // Smart decides per band and per frame; Natural and Efficient use
            // the static spatial mask.
            const float permission = useSmart ? smartDepths[index]
                                              : clamp01 (widthNorm * mask[index]);

            gains[index] = clampf (permission
                                   * focusShape (band, focusNorm)
                                   * airShape (band, airNorm)
                                   * lowShape (band)
                                   * smartTrim,
                                   0.0f,
                                   StftDecorrelator::maxBandGain);

            sum += gains[index];
        }

        stft.setBandGains (gains);
        bandWidths = gains;
        averageBandGain = sum / (float) numBands;

        // The velvet path has no band resolution, so it takes the average.
        velvetGainSmoother.setTarget (averageBandGain);

        // Sibilance Guard and Transient Focus: real scalar gains, so they
        // trade width for stability without ever moving the image.
        const float sibilanceDuck = 1.0f - 0.9f * clamp01 (parameters.sibilanceGuard * 0.01f) * frame.sibilance;
        const float transientDuck = 1.0f - 0.85f * clamp01 (parameters.transientFocus * 0.01f) * frame.transient;
        const float plosiveDuck = 1.0f - 0.6f * frame.plosive;

        guardSmoother.setTarget (clamp01 (sibilanceDuck * transientDuck * plosiveDuck));
    }

    /**
        Focus: how tightly the voice holds the centre of the image.

        At 0 the whole spectrum is allowed to spread. Turning it up pulls the
        presence region - roughly 300 Hz to 3 kHz, where intelligibility and
        the sense of "one performer in front of me" live - back towards mono,
        while leaving the top end wide. That is an audible, monotonic change
        across the whole range, which the previous gentle weighting curve was
        not.
    */
    static float focusShape (int band, float focusNorm) noexcept
    {
        constexpr float presenceCentre = 5.3f; // ~ 1 kHz on the analyser grid
        constexpr float presenceSpread = 2.3f;

        const float distance = ((float) band - presenceCentre) / presenceSpread;
        const float presence = std::exp (-distance * distance);

        const float narrowed = 1.0f - 0.92f * focusNorm * presence;
        const float topLift = 1.0f + 0.35f * focusNorm * highWeight (band);

        return clampf (narrowed * topLift, 0.0f, 1.5f);
    }

    /** Air: extra Side width in the top octaves only, as a real gain. */
    static float airShape (int band, float airNorm) noexcept
    {
        return 1.0f + 1.6f * airNorm * highWeight (band);
    }

    /** Everything below ~150 Hz stays mono, with a smooth handover. */
    static float lowShape (int band) noexcept
    {
        switch (band)
        {
            case 0:  return 0.0f;  //  60 - 101 Hz
            case 1:  return 0.0f;  // 101 - 170 Hz
            case 2:  return 0.45f; // 170 - 286 Hz
            default: return 1.0f;
        }
    }

    /** 0 below ~4 kHz, rising to 1 above ~10 kHz. */
    static float highWeight (int band) noexcept
    {
        return clamp01 (((float) band - 7.0f) / 3.0f);
    }

    /**
        Shaping for the Efficient path. The velvet FIR has no band structure,
        so it gets a broadband low cut and a gentle top lift, then the average
        band gain. These are minimum-phase filters, which is precisely what
        the centre alignment safety net exists for.
    */
    float shapeVelvet (float side) noexcept
    {
        float low = 0.0f, high = 0.0f;
        velvetLowCut.process (side, low, high);

        float airLow = 0.0f, airHigh = 0.0f;
        velvetAirSplit.process (high, airLow, airHigh);

        const float airAmount = clamp01 (parameters.air * 0.01f);
        const float shaped = airLow + airHigh * (1.0f + 1.2f * airAmount);

        return sanitise (shaped * velvetGainSmoother.next());
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
    ComplementaryCrossover velvetLowCut, velvetAirSplit;
    CorrelationGuard correlationGuard;
    CenterAlignment alignment;

    Smoother guardSmoother;
    Smoother velvetGainSmoother;
    Smoother outputGainSmoother;
    Smoother engineFade;

    float averageBandGain = 0.0f;
    AnalyzerFrame snapshot;
    std::array<float, numBands> bandWidths {};
};

} // namespace wp
