/*
    Wide Pocket - safety and compensation stages.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    Correlation Guard  keeps the inter-channel correlation above a floor, so
                       the result still folds down and still images as a single
                       source instead of a phasey cloud.
    Auto Gain          matches the output loudness to the input loudness.
    Center Lock        removes any inter-channel level difference, so the voice
                       cannot drift off centre.
*/

#pragma once

#include "WidePocketTypes.h"

namespace wp
{

/**
    Measures the running inter-channel correlation of the rendered output and
    returns a multiplier for the Side gain that keeps correlation above the
    requested floor.
*/
class CorrelationGuard
{
public:
    void prepare (double sampleRate, float windowMs = 120.0f)
    {
        const double tau = std::max (1.0, (double) windowMs) * 0.001;
        smoothing = (float) (1.0 - std::exp (-1.0 / (std::max (1.0, sampleRate) * tau)));
        release.reset (sampleRate, 60.0f, 1.0f);
        reset();
    }

    void reset() noexcept
    {
        crossSum = leftSum = rightSum = 0.0f;
        correlation = 1.0f;
        release.snapTo (1.0f);
    }

    /** Feeds one rendered output sample pair. */
    void measure (float left, float right) noexcept
    {
        crossSum += smoothing * (left * right - crossSum);
        leftSum += smoothing * (left * left - leftSum);
        rightSum += smoothing * (right * right - rightSum);

        const float denominator = std::sqrt (std::max (1.0e-12f, leftSum * rightSum));
        correlation = clampf (crossSum / denominator, -1.0f, 1.0f);
    }

    /**
        @param floorValue minimum acceptable correlation, 0 .. 0.95
        @returns a Side gain multiplier in [0, 1]
    */
    float sideGainMultiplier (float floorValue) noexcept
    {
        const float limit = clampf (floorValue, -0.5f, 0.95f);

        float target = 1.0f;
        if (correlation < limit)
        {
            // Scale back proportionally to how far we overshot the floor.
            const float overshoot = (limit - correlation) / std::max (0.05f, 1.0f + limit);
            target = clamp01 (1.0f - overshoot);
        }

        release.setTarget (target);
        return release.next();
    }

    float getCorrelation() const noexcept { return correlation; }

private:
    float smoothing = 0.01f;
    float crossSum = 0.0f, leftSum = 0.0f, rightSum = 0.0f;
    float correlation = 1.0f;
    Smoother release;
};

/**
    Loudness compensation. Compares input and output RMS over a slow window and
    returns a bounded make-up gain, so widening does not change perceived
    level. The correction is deliberately limited so it can never pump.
*/
class AutoGain
{
public:
    void prepare (double sampleRate, float windowMs = 300.0f, float maxCorrectionDb = 6.0f)
    {
        const double tau = std::max (1.0, (double) windowMs) * 0.001;
        smoothing = (float) (1.0 - std::exp (-1.0 / (std::max (1.0, sampleRate) * tau)));
        maxCorrection = maxCorrectionDb;
        gain.reset (sampleRate, 120.0f, 1.0f);
        reset();
    }

    void reset() noexcept
    {
        inputPower = outputPower = 0.0f;
        gain.snapTo (1.0f);
    }

    void measureInput (float monoInput) noexcept
    {
        inputPower += smoothing * (monoInput * monoInput - inputPower);
    }

    void measureOutput (float left, float right) noexcept
    {
        const float power = 0.5f * (left * left + right * right);
        outputPower += smoothing * (power - outputPower);
    }

    /** @returns the make-up gain to apply to the next sample. */
    float nextGain (bool enabled) noexcept
    {
        if (! enabled)
        {
            gain.setTarget (1.0f);
            return gain.next();
        }

        // Below the noise floor there is nothing meaningful to match.
        if (inputPower < 1.0e-9f || outputPower < 1.0e-9f)
        {
            gain.setTarget (1.0f);
            return gain.next();
        }

        const float ratioDb = gainToDb (std::sqrt (inputPower / outputPower));
        gain.setTarget (dbToGain (clampf (ratioDb, -maxCorrection, maxCorrection)));
        return gain.next();
    }

    float currentGain() const noexcept { return gain.value(); }

private:
    float smoothing = 0.001f;
    float maxCorrection = 6.0f;
    float inputPower = 0.0f;
    float outputPower = 0.0f;
    Smoother gain;
};

/**
    Center Lock, implemented in the M/S domain rather than as an L/R trim.

    An audible off-centre pull is an inter-channel level difference, and in M/S
    terms an IID is exactly the part of the Side signal that is correlated with
    the Mid signal:

        L/R power difference = 4 * E[M * S]

    So removing the Mid-correlated component of Side removes the IID exactly:

        S' = S - (E[M * S] / E[M * M]) * M

    This is the orthogonal projection of Side onto the complement of Mid. Two
    properties make it the right place to do it:

      - it drives the level difference to zero instead of chasing it with a
        servo, so the image cannot drift,
      - it only touches Side, so the mono sum (2 * M) is untouched, bit for
        bit. An L/R trim would have destroyed that.
*/
class CenterLock
{
public:
    void prepare (double sampleRate, float windowMs = 120.0f)
    {
        const double tau = std::max (1.0, (double) windowMs) * 0.001;
        smoothing = (float) (1.0 - std::exp (-1.0 / (std::max (1.0, sampleRate) * tau)));
        coefficient.reset (sampleRate, 40.0f, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        crossSum = 0.0f;
        midPower = 0.0f;
        coefficient.snapTo (0.0f);
    }

    /** Removes the Mid-correlated part of `side` when enabled. */
    void process (float mid, float& side, bool enabled) noexcept
    {
        crossSum += smoothing * (mid * side - crossSum);
        midPower += smoothing * (mid * mid - midPower);

        if (! enabled)
        {
            coefficient.setTarget (0.0f);
            coefficient.next();
            return;
        }

        const float target = midPower > 1.0e-10f ? clampf (crossSum / midPower, -2.0f, 2.0f) : 0.0f;
        coefficient.setTarget (target);
        side = sanitise (side - coefficient.next() * mid);
    }

    /**
        Residual inter-channel level difference in dB, derived from the
        measured Mid/Side correlation. Zero means perfectly centred.
    */
    float levelDifferenceDb() const noexcept
    {
        if (midPower < 1.0e-12f)
            return 0.0f;

        const float ratio = clampf (crossSum / midPower, -0.98f, 0.98f);
        return gainToDb ((1.0f + ratio) / (1.0f - ratio));
    }

private:
    float smoothing = 0.001f;
    float crossSum = 0.0f;
    float midPower = 0.0f;
    Smoother coefficient;
};

} // namespace wp
