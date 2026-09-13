/*
    Wide Pocket - minimal radix-2 FFT.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    JUCE ships a perfectly good FFT, but the DSP core must be testable without
    JUCE, so the transform lives here. It is allocation-free once prepared.
*/

#pragma once

#include "WidePocketTypes.h"

#include <complex>
#include <utility>
#include <vector>

namespace wp
{

class Fft
{
public:
    Fft() = default;
    explicit Fft (int order) { setOrder (order); }

    void setOrder (int newOrder)
    {
        order = newOrder;
        length = std::size_t (1) << order;

        twiddles.resize (length / 2);
        for (std::size_t i = 0; i < length / 2; ++i)
        {
            const double angle = -2.0 * kPi * (double) i / (double) length;
            twiddles[i] = std::complex<float> ((float) std::cos (angle), (float) std::sin (angle));
        }

        reversed.resize (length);
        for (std::size_t i = 0; i < length; ++i)
            reversed[i] = reverseBits (i, order);

        scratch.assign (length, std::complex<float> {});
    }

    int getOrder() const noexcept { return order; }
    std::size_t size() const noexcept { return length; }
    std::size_t numBins() const noexcept { return length / 2 + 1; }

    void forward (std::complex<float>* data) const { run (data, false); }
    void inverse (std::complex<float>* data) const { run (data, true); }

    /** Real forward transform. `spectrum` must hold numBins() elements. */
    void forwardReal (const float* input, std::complex<float>* spectrum) const
    {
        auto* work = scratch.data();
        for (std::size_t i = 0; i < length; ++i)
            work[i] = std::complex<float> (input[i], 0.0f);

        run (work, false);

        for (std::size_t i = 0; i < numBins(); ++i)
            spectrum[i] = work[i];
    }

    /** Real inverse transform from a half spectrum of numBins() elements. */
    void inverseReal (const std::complex<float>* spectrum, float* output) const
    {
        auto* work = scratch.data();
        for (std::size_t i = 0; i < numBins(); ++i)
            work[i] = spectrum[i];

        for (std::size_t i = numBins(); i < length; ++i)
            work[i] = std::conj (spectrum[length - i]);

        run (work, true);

        for (std::size_t i = 0; i < length; ++i)
            output[i] = work[i].real();
    }

private:
    void run (std::complex<float>* data, bool invert) const
    {
        for (std::size_t i = 0; i < length; ++i)
        {
            const std::size_t j = reversed[i];
            if (j > i)
                std::swap (data[i], data[j]);
        }

        for (std::size_t span = 2; span <= length; span <<= 1)
        {
            const std::size_t half = span >> 1;
            const std::size_t step = length / span;

            for (std::size_t base = 0; base < length; base += span)
            {
                for (std::size_t k = 0; k < half; ++k)
                {
                    auto w = twiddles[k * step];
                    if (invert)
                        w = std::conj (w);

                    const auto upper = data[base + k];
                    const auto lower = data[base + k + half] * w;

                    data[base + k] = upper + lower;
                    data[base + k + half] = upper - lower;
                }
            }
        }

        if (invert)
        {
            const float scale = 1.0f / (float) length;
            for (std::size_t i = 0; i < length; ++i)
                data[i] *= scale;
        }
    }

    static std::size_t reverseBits (std::size_t value, int bits) noexcept
    {
        std::size_t result = 0;
        for (int i = 0; i < bits; ++i)
        {
            result = (result << 1) | (value & 1);
            value >>= 1;
        }
        return result;
    }

    int order = 0;
    std::size_t length = 1;
    std::vector<std::complex<float>> twiddles;
    std::vector<std::size_t> reversed;
    mutable std::vector<std::complex<float>> scratch;
};

/** Periodic Hann window. With 50 % or 75 % hop this satisfies COLA. */
inline void makeHannWindow (std::vector<float>& window, std::size_t size)
{
    window.resize (size);
    for (std::size_t i = 0; i < size; ++i)
        window[i] = 0.5f - 0.5f * (float) std::cos (2.0 * kPi * (double) i / (double) size);
}

/** Sum of squared, hop-shifted windows. Used to normalise overlap-add exactly. */
inline float colaSum (const std::vector<float>& window, std::size_t hop)
{
    const std::size_t size = window.size();
    float sum = 0.0f;
    for (std::size_t offset = 0; offset < size; offset += hop)
        sum += window[offset] * window[offset];
    return sum;
}

} // namespace wp
