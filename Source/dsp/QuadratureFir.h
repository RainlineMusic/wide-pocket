/*
    Wide Pocket - Natural v0.4 side generator.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    Why this replaces the STFT decorrelator
    ---------------------------------------
    The v0.3 Side was synthesised by multiplying a 256 point spectrum by a
    per-bin sign table and a set of band gains, then overlap-adding. That is a
    time-varying, frame-rate-quantised filter, and it has two unavoidable
    problems that showed up straight away in a Plugin Doctor sweep:

      * the sign table has a long equivalent impulse response, much longer
        than the 256 point frame, so the circular convolution wraps around:
        energy from the end of the response folds back into the frame. On a
        steady sine that folded energy lands on frequencies that are not
        harmonically related to the input, which is what the comb of spurs
        around the fundamental (and the raised noise floor on the IMD test)
        actually is.
      * every frame the band gains and the duck step to a new value. A gain
        that steps once per hop modulates the signal at the frame rate
        (sampleRate / hop), producing sidebands spaced by that rate. Those
        are the evenly spaced lines either side of the tone.

    Neither is fixable by tuning: they are properties of doing the job with a
    short-frame STFT. So the Side is now produced by ordinary linear
    convolution with a single antisymmetric FIR, designed on the fly from the
    same band gains:

        H(f) = j * G(f),   G(f) real and non-negative

    An antisymmetric (odd) kernel has a purely imaginary frequency response,
    so every frequency of the Side is exactly 90 degrees from the Mid:

        Re{M(f) * conj(S(f))} = Re{M * conj(j G M)} = -G |M|^2 * Re{j} = 0

    The inter-channel level difference of L = M + S, R = M - S is proportional
    to exactly that quantity, so the image is centred by construction at every
    frequency, for any real G. No servo, no measurement, no drift.

    And because the filtering is a true linear convolution with a 255 tap
    kernel there is no wrap-around and no frame rate anywhere in the signal
    path: a sine in gives a sine out, at one frequency only.
*/

#pragma once

#include "Fft.h"
#include "VocalAnalyzer.h"
#include "WidePocketTypes.h"

#include <array>
#include <complex>
#include <vector>

namespace wp
{

class QuadratureFir
{
public:
    static constexpr int numBands = VocalAnalyzer::numMaskBands;
    static constexpr float maxBandGain = 2.5f;

    static constexpr int kernelLength = 255;             // odd, antisymmetric
    static constexpr int latency = (kernelLength - 1) / 2;
    static constexpr int designOrder = 9;
    static constexpr int designSize = 1 << designOrder;  // 512 point design grid
    static constexpr int designBins = designSize / 2 + 1;
    static constexpr int updateInterval = 128;           // kernel redesign rate
    static constexpr int ringSize = 512;                 // power of two, > kernelLength
    static constexpr int ringMask = ringSize - 1;

    void prepare (double sr)
    {
        sampleRate = std::max (8000.0, sr);

        fft.setOrder (designOrder);
        designSpectrum.assign ((std::size_t) designBins, {});
        designTime.assign ((std::size_t) designSize, 0.0f);

        currentKernel.assign ((std::size_t) kernelLength, 0.0f);
        targetKernel.assign ((std::size_t) kernelLength, 0.0f);
        history.assign ((std::size_t) ringSize, 0.0f);

        // Symmetric Hamming taper. A symmetric window keeps the kernel
        // antisymmetric, so the response stays purely imaginary and the
        // centring guarantee survives the truncation.
        window.resize ((std::size_t) kernelLength);
        for (int i = 0; i < kernelLength; ++i)
            window[(std::size_t) i] = 0.54f - 0.46f * (float) std::cos (2.0 * kPi * (double) i / (double) (kernelLength - 1));

        const double updateRate = sampleRate / (double) updateInterval;
        for (auto& smoother : bandGain)
            smoother.reset (updateRate, 240.0f, 0.0f);

        // The kernel itself is also crossfaded, so even a hard parameter jump
        // arrives as a continuous change of the filter rather than a step.
        kernelMorph = 1.0f - std::exp (-1.0f / (float) std::max (1.0, updateRate * 0.030));

        duckAttack = 1.0f - std::exp (-1.0f / (float) std::max (1.0, sampleRate * 0.005));
        duckRelease = 1.0f - std::exp (-1.0f / (float) std::max (1.0, sampleRate * 0.120));

        reset();
    }

    void reset() noexcept
    {
        std::fill (history.begin(), history.end(), 0.0f);
        writeIndex = 0;
        samplesUntilUpdate = 1;
        duck = 1.0f;
        transientAmount = 0.0f;
        std::fill (currentKernel.begin(), currentKernel.end(), 0.0f);
        std::fill (targetKernel.begin(), targetKernel.end(), 0.0f);
    }

    void setBandGains (const std::array<float, numBands>& g) noexcept
    {
        for (int b = 0; b < numBands; ++b)
            bandGain[(std::size_t) b].setTarget (clampf (g[(std::size_t) b], 0.0f, maxBandGain));
    }

    void setUniformGain (float g) noexcept
    {
        for (auto& smoother : bandGain)
            smoother.setTarget (clampf (g, 0.0f, maxBandGain));
    }

    void setGainSmoothingMs (float ms) noexcept
    {
        const double updateRate = sampleRate / (double) updateInterval;
        for (auto& smoother : bandGain)
            smoother.setTime (updateRate, clampf (ms, 40.0f, 600.0f));
    }

    /** How far the Side is pulled back on an onset. 0 = never, 1 = to silence. */
    void setDuckDepth (float depth) noexcept { duckDepth = clamp01 (depth); }

    /** Continuous onset amount from the analyser, 0 .. 1, set every sample. */
    void setTransientAmount (float amount) noexcept { transientAmount = clamp01 (amount); }

    float process (float x) noexcept
    {
        if (--samplesUntilUpdate <= 0)
        {
            samplesUntilUpdate = updateInterval;
            designTargetKernel();
            for (int i = 0; i < kernelLength; ++i)
                currentKernel[(std::size_t) i] += kernelMorph * (targetKernel[(std::size_t) i] - currentKernel[(std::size_t) i]);
        }

        writeIndex = (writeIndex + 1) & ringMask;
        history[(std::size_t) writeIndex] = sanitise (x);

        float sum = 0.0f;
        for (int k = 0; k < kernelLength; ++k)
            sum += currentKernel[(std::size_t) k] * history[(std::size_t) ((writeIndex - k) & ringMask)];

        // A single broadband, smoothly moving real gain. It cannot tilt the
        // image and, unlike the old gate, it never reaches zero abruptly.
        const float target = 1.0f - duckDepth * transientAmount;
        duck += (target < duck ? duckAttack : duckRelease) * (target - duck);
        duck = clampf (duck, 0.0f, 1.0f);

        return sanitise (sum * duck);
    }

    int getLatencySamples() const noexcept { return latency; }
    float getDuck() const noexcept { return duck; }

    /** Magnitude of the designed response at a frequency. Used by the tests. */
    float getDesignedGainAtHz (float hz) const noexcept { return gainAtHz (hz); }

private:
    void designTargetKernel() noexcept
    {
        std::array<float, numBands> g {};
        for (int b = 0; b < numBands; ++b)
            g[(std::size_t) b] = bandGain[(std::size_t) b].next();
        smoothedBandGain = g;

        const float binHz = (float) (sampleRate / (double) designSize);

        designSpectrum[0] = {};
        designSpectrum[(std::size_t) (designBins - 1)] = {};

        for (int k = 1; k < designBins - 1; ++k)
            designSpectrum[(std::size_t) k] = std::complex<float> (0.0f, gainAtHz ((float) k * binHz));

        fft.inverseReal (designSpectrum.data(), designTime.data());

        // The transform of a purely imaginary, conjugate-symmetric spectrum is
        // real and odd about n = 0. Rotating it by the latency and windowing
        // keeps it odd about the centre tap.
        for (int i = 0; i < kernelLength; ++i)
        {
            const int n = i - latency;
            const int index = (n + designSize) & (designSize - 1);
            targetKernel[(std::size_t) i] = designTime[(std::size_t) index] * window[(std::size_t) i];
        }
    }

    /** Band gains interpolated linearly in log frequency, so G(f) is smooth
        and the kernel stays short. Steps between bands would ring. */
    float gainAtHz (float hz) const noexcept
    {
        const float first = bandCentreHz (0);
        const float last = bandCentreHz (numBands - 1);

        if (hz <= first)
            return smoothedBandGain[0];
        if (hz >= last)
            return smoothedBandGain[(std::size_t) (numBands - 1)];

        const float position = std::log2 (hz / first) / 0.75f;
        const int lower = std::min (numBands - 2, (int) position);
        const float t = clamp01 (position - (float) lower);

        return lerp (smoothedBandGain[(std::size_t) lower], smoothedBandGain[(std::size_t) (lower + 1)], t);
    }

    static float bandCentreHz (int band) noexcept
    {
        return VocalAnalyzer::bandLowEdgeHz (band) * std::pow (2.0f, 0.375f);
    }

    double sampleRate = 48000.0;
    Fft fft;

    std::vector<std::complex<float>> designSpectrum;
    std::vector<float> designTime, currentKernel, targetKernel, history, window;
    std::array<Smoother, numBands> bandGain;
    std::array<float, numBands> smoothedBandGain {};

    int writeIndex = 0;
    int samplesUntilUpdate = 1;
    float kernelMorph = 0.2f;
    float duck = 1.0f, duckDepth = 0.0f, transientAmount = 0.0f;
    float duckAttack = 0.1f, duckRelease = 0.01f;
};

} // namespace wp
