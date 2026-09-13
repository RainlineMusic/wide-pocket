/*
    Wide Pocket - shared DSP types and helpers.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    This header is intentionally free of JUCE dependencies so that the whole
    DSP core can be compiled and tested with a bare C++17 compiler.
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace wp
{

inline constexpr double kPi = 3.14159265358979323846;

/** User-facing parameter set, in user units. */
struct Parameters
{
    float width = 50.0f;           // 0 .. 100 %
    float focus = 50.0f;           // 0 .. 100 %
    float air = 40.0f;             // 0 .. 100 %
    float stability = 50.0f;       // 0 .. 100 %
    float sibilanceGuard = 60.0f;  // 0 .. 100 %
    float transientFocus = 60.0f;  // 0 .. 100 %
    float outputDb = 0.0f;         // -24 .. +12 dB
};

/*
    There are deliberately no Mono Safe / Center Lock / Auto Gain switches.

    Those were user-facing symptoms of a design problem, not features:

      - the Mid path is always a pure delay, so the mono sum is always exactly
        the dry signal. "Mono safe" is structural and cannot be switched off.
      - the synthesised Side is decorrelated and spectrally orthogonalised to
        the Mid before synthesis, so no time-domain panning servo is needed.
      - the Side never adds broadband level to the sum, so there is nothing
        for an auto gain stage to chase.
*/

/** Lock-free snapshot handed to the UI. Never read by the DSP. */
struct AnalyzerFrame
{
    float level = 0.0f;           // mid RMS, linear
    float voicing = 0.0f;         // 0 = unvoiced, 1 = strongly voiced
    float transient = 0.0f;       // 0 = steady, 1 = attack
    float harmonicity = 0.0f;     // harmonic / total energy
    float sibilance = 0.0f;       // S/SH energy dominance
    float plosive = 0.0f;         // low-frequency burst detector
    float flatness = 0.0f;        // spectral flatness, 0 tonal .. 1 noisy
    float flux = 0.0f;            // spectral flux, normalised
    float correlation = 0.0f;     // inter-channel correlation of the output
    float appliedWidth = 0.0f;    // width actually applied after all guards
};

inline float clampf (float value, float low, float high) noexcept
{
    return value < low ? low : (value > high ? high : value);
}

inline float clamp01 (float value) noexcept
{
    return clampf (value, 0.0f, 1.0f);
}

inline float dbToGain (float db) noexcept
{
    return std::pow (10.0f, db * 0.05f);
}

inline float gainToDb (float gain) noexcept
{
    return 20.0f * std::log10 (std::max (gain, 1.0e-9f));
}

inline float lerp (float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

/** Equal-power crossfade weights, used when switching engines. */
inline void equalPowerWeights (float position, float& fromWeight, float& toWeight) noexcept
{
    const float t = clamp01 (position);
    const float angle = (float) (0.5 * kPi) * t;
    fromWeight = std::cos (angle);
    toWeight = std::sin (angle);
}

/** Replaces NaN and Inf with zero. Used as the final real-time safety net. */
inline float sanitise (float value) noexcept
{
    return std::isfinite (value) ? value : 0.0f;
}

/** One-pole parameter smoother, time constant expressed in milliseconds. */
class Smoother
{
public:
    void reset (double sampleRate, float timeMs, float initialValue = 0.0f) noexcept
    {
        setTime (sampleRate, timeMs);
        current = initialValue;
        target = initialValue;
    }

    void setTime (double sampleRate, float timeMs) noexcept
    {
        const double tau = std::max (0.01, (double) timeMs) * 0.001;
        coefficient = (float) (1.0 - std::exp (-1.0 / (std::max (1.0, sampleRate) * tau)));
    }

    void setTarget (float value) noexcept { target = value; }
    void snapTo (float value) noexcept { target = current = value; }

    float next() noexcept
    {
        current += coefficient * (target - current);
        return current;
    }

    float value() const noexcept { return current; }

private:
    float coefficient = 1.0f;
    float current = 0.0f;
    float target = 0.0f;
};

} // namespace wp
