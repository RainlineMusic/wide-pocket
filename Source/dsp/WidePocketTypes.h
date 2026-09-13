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

enum class Engine
{
    natural = 0,   // STFT sub-band all-pass decorrelation
    efficient = 1, // sparse velvet-noise FIR decorrelation
    smart = 2      // adaptive parametric stereo controller
};

enum class Quality
{
    live = 0,  // short STFT frames, low latency
    studio = 1 // long STFT frames, highest quality
};

/** User-facing parameter set, in user units. */
struct Parameters
{
    float width = 50.0f;           // 0 .. 100 %
    float focus = 50.0f;           // 0 .. 100 %
    float air = 40.0f;             // 0 .. 100 %
    float stability = 50.0f;       // 0 .. 100 %
    float lowMonoHz = 180.0f;      // 80 .. 600 Hz
    float sibilanceGuard = 60.0f;  // 0 .. 100 %
    float transientFocus = 60.0f;  // 0 .. 100 %
    float outputDb = 0.0f;         // -12 .. +12 dB

    Engine engine = Engine::natural;
    Quality quality = Quality::studio;

    bool monoSafe = true;
    bool centerLock = true;
    bool autoGain = true;
};

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
