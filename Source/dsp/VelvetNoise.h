/*
    Wide Pocket - optimised velvet noise decorrelator (Efficient engine).
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    A velvet noise sequence is a sparse sequence of +-1 impulses, one impulse
    per grid interval, placed pseudo-randomly inside that interval. Convolving
    with it costs only a few additions per sample yet sounds smooth, because
    the sparse impulse response has a flat magnitude response and no audible
    comb pattern.

    "Optimised" velvet noise (OVN) here means:
      - logarithmically growing grid, so early reflections are dense and late
        ones sparse, which removes the metallic ring of uniform velvet noise,
      - a deterministic seed, so every build produces bit-identical output,
      - energy normalisation, so the decorrelated signal matches the input RMS.
*/

#pragma once

#include "WidePocketTypes.h"

#include <cstdint>
#include <vector>

namespace wp
{

/** Deterministic xorshift32, so the impulse response never changes. */
class DeterministicRandom
{
public:
    explicit DeterministicRandom (std::uint32_t seed = 0x9e3779b9u) : state (seed | 1u) {}

    std::uint32_t nextUInt() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    /** Uniform in [0, 1). */
    float nextFloat() noexcept
    {
        return (float) (nextUInt() >> 8) * (1.0f / 16777216.0f);
    }

    bool nextBool() noexcept { return (nextUInt() & 0x10000u) != 0u; }

private:
    std::uint32_t state;
};

struct VelvetTap
{
    int delaySamples = 0;
    float gain = 0.0f;
};

class VelvetNoiseDecorrelator
{
public:
    /**
        @param sampleRate   host sample rate
        @param lengthMs     impulse response length; longer means more diffuse
        @param density      impulses per second at the start of the response
        @param seed         deterministic seed, use a different one per channel
    */
    void prepare (double sampleRate, float lengthMs, float density, std::uint32_t seed)
    {
        const int lengthSamples = std::max (16, (int) std::round (sampleRate * (double) lengthMs * 0.001));

        buffer.assign ((std::size_t) nextPowerOfTwo (lengthSamples + 4), 0.0f);
        mask = (int) buffer.size() - 1;
        writeIndex = 0;

        buildTaps (sampleRate, lengthSamples, density, seed);
    }

    void reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    float process (float input) noexcept
    {
        buffer[(std::size_t) writeIndex] = sanitise (input);

        float sum = 0.0f;
        for (const auto& tap : taps)
            sum += tap.gain * buffer[(std::size_t) ((writeIndex - tap.delaySamples) & mask)];

        writeIndex = (writeIndex + 1) & mask;
        return sanitise (sum);
    }

    const std::vector<VelvetTap>& getTaps() const noexcept { return taps; }
    int getNumTaps() const noexcept { return (int) taps.size(); }

    /** Sum of squared tap gains. Equals 1 after normalisation. */
    float impulseEnergy() const noexcept
    {
        float energy = 0.0f;
        for (const auto& tap : taps)
            energy += tap.gain * tap.gain;
        return energy;
    }

    int maxDelaySamples() const noexcept
    {
        int longest = 0;
        for (const auto& tap : taps)
            longest = std::max (longest, tap.delaySamples);
        return longest;
    }

private:
    void buildTaps (double sampleRate, int lengthSamples, float density, std::uint32_t seed)
    {
        taps.clear();
        DeterministicRandom random (seed);

        const double safeDensity = (double) clampf (density, 200.0f, 8000.0f);
        double gridSamples = std::max (2.0, sampleRate / safeDensity);

        // Logarithmic grid growth: each interval is slightly longer than the
        // previous one, which decorrelates without the metallic tail.
        const double growth = 1.06;

        double position = 1.0;
        while (position < (double) lengthSamples)
        {
            const int jitterRange = std::max (1, (int) gridSamples - 1);
            const int offset = (int) (random.nextFloat() * (float) jitterRange);
            const int delay = std::min (lengthSamples, (int) position + offset);

            // Exponential decay keeps the late response from smearing transients.
            const double normalised = (double) delay / (double) lengthSamples;
            const float decay = (float) std::exp (-3.0 * normalised);

            taps.push_back ({ delay, random.nextBool() ? decay : -decay });

            position += gridSamples;
            gridSamples *= growth;
        }

        if (taps.empty())
            taps.push_back ({ 1, 1.0f });

        normalise();
    }

    void normalise() noexcept
    {
        float energy = 0.0f;
        for (const auto& tap : taps)
            energy += tap.gain * tap.gain;

        const float scale = 1.0f / std::sqrt (std::max (1.0e-12f, energy));
        for (auto& tap : taps)
            tap.gain *= scale;
    }

    static int nextPowerOfTwo (int value) noexcept
    {
        int result = 1;
        while (result < value)
            result <<= 1;
        return result;
    }

    std::vector<VelvetTap> taps;
    std::vector<float> buffer;
    int mask = 0;
    int writeIndex = 0;
};

} // namespace wp
