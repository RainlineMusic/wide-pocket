/*
    Wide Pocket - filters.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    The complementary crossover is deliberately reconstructed: the high band
    is derived as (input - lowBand), so low + high reconstructs the input
    exactly, sample for sample, with no magnitude ripple and no phase error.
    That property is what lets Wide Pocket keep a bit-transparent dry path.
*/

#pragma once

#include "WidePocketTypes.h"

namespace wp
{

/** Topology preserving one-pole low-pass (Zavalishin TPT form). */
class OnePoleTpt
{
public:
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = std::max (8000.0, newSampleRate);
        setCutoff (cutoffHz);
        reset();
    }

    void reset() noexcept { state = 0.0f; }

    void setCutoff (float hz) noexcept
    {
        cutoffHz = clampf (hz, 10.0f, (float) (sampleRate * 0.45));
        const double warped = std::tan (kPi * (double) cutoffHz / sampleRate);
        coefficient = (float) (warped / (1.0 + warped));
    }

    float processLow (float input) noexcept
    {
        const float delta = coefficient * (input - state);
        const float low = state + delta;
        state = low + delta;
        return low;
    }

private:
    double sampleRate = 48000.0;
    float cutoffHz = 200.0f;
    float coefficient = 0.0f;
    float state = 0.0f;
};

/**
    Two cascaded one-poles for the low band, with the high band defined as the
    exact complement. 12 dB/oct low band, perfect reconstruction.
*/
class ComplementaryCrossover
{
public:
    void prepare (double sampleRate) noexcept
    {
        first.prepare (sampleRate);
        second.prepare (sampleRate);
        setCutoff (cutoffHz);
    }

    void reset() noexcept
    {
        first.reset();
        second.reset();
    }

    void setCutoff (float hz) noexcept
    {
        cutoffHz = hz;
        first.setCutoff (hz);
        second.setCutoff (hz);
    }

    void process (float input, float& low, float& high) noexcept
    {
        low = second.processLow (first.processLow (input));
        high = input - low;
    }

    float processHigh (float input) noexcept
    {
        float low = 0.0f, high = 0.0f;
        process (input, low, high);
        return high;
    }

private:
    OnePoleTpt first, second;
    float cutoffHz = 200.0f;
};

/** Transposed direct form II biquad, used for the analyser band splits. */
class Biquad
{
public:
    void reset() noexcept { z1 = z2 = 0.0f; }

    void setCoefficients (float newB0, float newB1, float newB2, float newA1, float newA2) noexcept
    {
        b0 = newB0; b1 = newB1; b2 = newB2; a1 = newA1; a2 = newA2;
    }

    float process (float input) noexcept
    {
        const float output = b0 * input + z1;
        z1 = b1 * input - a1 * output + z2;
        z2 = b2 * input - a2 * output;
        return output;
    }

    /** Second order Butterworth high-pass. */
    void setHighPass (double sampleRate, float hz)
    {
        const double w = 2.0 * kPi * (double) clampf (hz, 10.0f, (float) (sampleRate * 0.45)) / sampleRate;
        const double cosw = std::cos (w);
        const double alpha = std::sin (w) / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha;
        setCoefficients ((float) (((1.0 + cosw) * 0.5) / a0),
                         (float) ((-(1.0 + cosw)) / a0),
                         (float) (((1.0 + cosw) * 0.5) / a0),
                         (float) ((-2.0 * cosw) / a0),
                         (float) ((1.0 - alpha) / a0));
    }

    /** Second order Butterworth low-pass. */
    void setLowPass (double sampleRate, float hz)
    {
        const double w = 2.0 * kPi * (double) clampf (hz, 10.0f, (float) (sampleRate * 0.45)) / sampleRate;
        const double cosw = std::cos (w);
        const double alpha = std::sin (w) / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha;
        setCoefficients ((float) (((1.0 - cosw) * 0.5) / a0),
                         (float) ((1.0 - cosw) / a0),
                         (float) (((1.0 - cosw) * 0.5) / a0),
                         (float) ((-2.0 * cosw) / a0),
                         (float) ((1.0 - alpha) / a0));
    }

    /** Second order band-pass with unity peak gain. */
    void setBandPass (double sampleRate, float hz, float q)
    {
        const double w = 2.0 * kPi * (double) clampf (hz, 10.0f, (float) (sampleRate * 0.45)) / sampleRate;
        const double alpha = std::sin (w) / (2.0 * (double) std::max (0.1f, q));
        const double a0 = 1.0 + alpha;
        setCoefficients ((float) (alpha / a0),
                         0.0f,
                         (float) (-alpha / a0),
                         (float) ((-2.0 * std::cos (w)) / a0),
                         (float) ((1.0 - alpha) / a0));
    }

private:
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
};

/** Simple envelope follower with separate attack and release times. */
class EnvelopeFollower
{
public:
    void prepare (double sampleRate, float attackMs, float releaseMs) noexcept
    {
        attackCoeff = coefficientFor (sampleRate, attackMs);
        releaseCoeff = coefficientFor (sampleRate, releaseMs);
        envelope = 0.0f;
    }

    void reset() noexcept { envelope = 0.0f; }

    float process (float input) noexcept
    {
        const float magnitude = std::abs (input);
        const float coeff = magnitude > envelope ? attackCoeff : releaseCoeff;
        envelope += coeff * (magnitude - envelope);
        return envelope;
    }

    float value() const noexcept { return envelope; }

private:
    static float coefficientFor (double sampleRate, float timeMs) noexcept
    {
        const double tau = std::max (0.01, (double) timeMs) * 0.001;
        return (float) (1.0 - std::exp (-1.0 / (std::max (1.0, sampleRate) * tau)));
    }

    float attackCoeff = 1.0f;
    float releaseCoeff = 1.0f;
    float envelope = 0.0f;
};

} // namespace wp
