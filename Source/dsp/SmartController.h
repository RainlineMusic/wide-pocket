/*
    Wide Pocket - Smart engine controller.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    Smart is an adaptive parametric stereo controller: instead of applying one
    fixed decorrelation depth, it decides per band and per frame how much the
    signal may spread, from the analyser features.

    Version 0.1 is fully deterministic. No neural network is shipped, and the
    plugin never depends on one. The MlModel interface below is the seam where
    a trained model can be plugged in later: if a model is attached, ready and
    returns finite values, its output is blended with the deterministic
    decision; in every other case - no model, model not ready, inference
    failure, non-finite output - the deterministic path is used unchanged.
    That is the safe fallback, and it is covered by the test suite.
*/

#pragma once

#include "VocalAnalyzer.h"
#include "WidePocketTypes.h"

#include <array>

namespace wp
{

/** Feature vector handed to a model. Stable, documented layout. */
struct MlFeatures
{
    static constexpr int size = 10;

    // 0 level, 1 voicing, 2 transient, 3 harmonicity, 4 sibilance,
    // 5 plosive, 6 flatness, 7 flux, 8 width setting, 9 focus setting
    std::array<float, size> data {};
};

/** Model output: per-band width scalers plus a global trim. */
struct MlDecision
{
    static constexpr int numBands = VocalAnalyzer::numMaskBands;

    std::array<float, numBands> bandWidth {};
    float globalTrim = 1.0f;
};

class MlModel
{
public:
    virtual ~MlModel() = default;

    /** Must be cheap and real-time safe. */
    virtual bool isReady() const = 0;

    /** Real-time safe inference. Return false to fall back to the DSP path. */
    virtual bool infer (const MlFeatures& features, MlDecision& decision) = 0;

    /** How much the model is trusted, 0 .. 1. */
    virtual float blendAmount() const { return 1.0f; }
};

class SmartController
{
public:
    static constexpr int numBands = VocalAnalyzer::numMaskBands;

    void prepare (double sampleRate, int hopSize)
    {
        frameRate = std::max (1.0, sampleRate / (double) std::max (1, hopSize));
        for (auto& smoother : bandSmoothers)
            smoother.reset (frameRate, 60.0f, 0.0f);
        trim.reset (frameRate, 80.0f, 1.0f);
        modelActive = false;
    }

    void reset()
    {
        for (auto& smoother : bandSmoothers)
            smoother.snapTo (0.0f);
        trim.snapTo (1.0f);
        modelActive = false;
    }

    void setModel (MlModel* newModel) noexcept { model = newModel; }

    /** Called once per analysis hop. */
    void update (const AnalyzerFrame& frame,
                 const std::array<float, numBands>& spatialMask,
                 const Parameters& parameters)
    {
        const float widthNorm = clamp01 (parameters.width * 0.01f);
        const float focusNorm = clamp01 (parameters.focus * 0.01f);
        const float stability = clamp01 (parameters.stability * 0.01f);

        // Deterministic decision.
        MlDecision decision;
        for (int band = 0; band < numBands; ++band)
        {
            const float permission = clamp01 (spatialMask[(std::size_t) band]);

            // A strongly voiced, tonal passage is widened conservatively; a
            // breathy or consonant passage can take much more.
            const float voiceRestraint = 1.0f - 0.55f * frame.voicing * (1.0f - frame.flatness);
            const float transientRestraint = 1.0f - 0.85f * frame.transient
                                                 * clamp01 (parameters.transientFocus * 0.01f);

            // Focus is applied by the engine as a real band gain, so it is
            // deliberately not duplicated here: two mild curves multiplied
            // together is exactly why Focus used to do almost nothing.
            decision.bandWidth[(std::size_t) band] =
                clamp01 (widthNorm * permission * voiceRestraint * transientRestraint);
        }

        decision.globalTrim = 1.0f - 0.5f * frame.plosive;

        // Optional model blend, with a strict safety contract.
        modelActive = false;
        if (model != nullptr && model->isReady())
        {
            MlFeatures features;
            features.data = { frame.level, frame.voicing, frame.transient, frame.harmonicity,
                              frame.sibilance, frame.plosive, frame.flatness, frame.flux,
                              widthNorm, focusNorm };

            MlDecision modelDecision;
            if (model->infer (features, modelDecision) && isDecisionUsable (modelDecision))
            {
                const float blend = clamp01 (model->blendAmount());
                for (int band = 0; band < numBands; ++band)
                    decision.bandWidth[(std::size_t) band] =
                        lerp (decision.bandWidth[(std::size_t) band],
                              clamp01 (modelDecision.bandWidth[(std::size_t) band]),
                              blend);

                decision.globalTrim = lerp (decision.globalTrim,
                                            clampf (modelDecision.globalTrim, 0.0f, 2.0f),
                                            blend);
                modelActive = true;
            }
        }

        // Stability slows the adaptation down, from snappy to glue-like.
        const float smoothingMs = lerp (25.0f, 400.0f, stability);
        for (int band = 0; band < numBands; ++band)
        {
            auto& smoother = bandSmoothers[(std::size_t) band];
            smoother.setTime (frameRate, smoothingMs);
            smoother.setTarget (decision.bandWidth[(std::size_t) band]);
            depths[(std::size_t) band] = smoother.next();
        }

        trim.setTime (frameRate, smoothingMs);
        trim.setTarget (decision.globalTrim);
        trim.next();
    }

    const std::array<float, numBands>& getBandDepths() const noexcept { return depths; }
    float getGlobalTrim() const noexcept { return trim.value(); }
    bool isModelActive() const noexcept { return modelActive; }

private:
    static bool isDecisionUsable (const MlDecision& decision) noexcept
    {
        if (! std::isfinite (decision.globalTrim))
            return false;

        for (float value : decision.bandWidth)
            if (! std::isfinite (value))
                return false;

        return true;
    }

    MlModel* model = nullptr;
    bool modelActive = false;

    double frameRate = 187.5;
    std::array<Smoother, numBands> bandSmoothers {};
    std::array<float, numBands> depths {};
    Smoother trim;
};

} // namespace wp
