/*
    Wide Pocket - STFT quadrature Side generator (Natural / Smart engines).
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    This stage takes the Mid signal and synthesises the Side signal directly,
    in the frequency domain, with one hard structural rule:

        S[k] = j * s[k] * g[k] * M[k],   s[k] in {-1, +1},  g[k] real

    Every bin of the Side is exactly 90 degrees away from the same bin of the
    Mid. That single choice is what makes the image stable, and it is worth
    spelling out why:

        the inter-channel level difference of L = M + S, R = M - S is

            (|L|^2 - |R|^2) = 4 * sum_k Re{ M[k] * conj(S[k]) }

        and with S[k] = j * a_k * M[k] (a_k real) every term becomes

            Re{ M * conj(j * a_k * M) } = Re{ -j * a_k * |M|^2 } = 0.

    So the level difference is identically zero for *any* set of real per-bin
    gains. The gains carry Width, Focus, Air, the spatial mask and the low-end
    mono-ing, and none of them can pull the voice off centre. The previous
    design rotated each bin by a pseudo-random angle, which left a Mid
    correlated component in the Side (a real static IID: the "leans left"
    report) and made every later gain change move the image.

    Decorrelation still comes from frequency dependence: the +-1 sign pattern
    is drawn once per small bin group with a fixed seed, so the quadrature
    filter has a frequency dependent sign structure while staying exactly
    quadrature. Magnitudes are never altered, so the timbre of the Side is the
    timbre of the voice.

    Guaranteed by construction and covered by the tests:
      1. per-bin quadrature, hence zero inter-channel level difference,
      2. all gains at zero gives exact silence, so Width = 0 is an exact null,
      3. Hann analysis and synthesis with 75 % overlap (COLA), so there is no
         amplitude modulation,
      4. the delay is exactly fftSize samples, reported to the host.
*/

#pragma once

#include "Fft.h"
#include "VocalAnalyzer.h"
#include "WidePocketTypes.h"

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

namespace wp
{

class StftDecorrelator
{
public:
    static constexpr int numBands = VocalAnalyzer::numMaskBands;

    /** Band gains may exceed 1: Air lifts the top bands above unity. */
    static constexpr float maxBandGain = 2.5f;

    /**
        Neighbouring frames carry independent sign patterns, so the overlap-add
        sums incoherently and loses almost exactly 3 dB. This is a constant of
        the 75 % Hann overlap, not a property of the signal, so it is corrected
        with one fixed scalar. A real scalar cannot disturb the quadrature
        relationship, so the image stays centred.
    */
    static constexpr float overlapMakeup = 1.4142f;

    /** @param order log2 of the frame size: 8 = 256, 10 = 1024. */
    void prepare (double newSampleRate, int order)
    {
        sampleRate = std::max (8000.0, newSampleRate);
        fftSize = 1 << order;
        hopSize = fftSize / 4;
        numBins = fftSize / 2 + 1;

        fft.setOrder (order);
        makeHannWindow (window, (std::size_t) fftSize);
        normalisation = 1.0f / std::max (1.0e-6f, colaSum (window, (std::size_t) hopSize));

        inputFrame.assign ((std::size_t) fftSize, 0.0f);
        workFrame.assign ((std::size_t) fftSize, 0.0f);
        overlapBuffer.assign ((std::size_t) fftSize, 0.0f);
        spectrum.assign ((std::size_t) numBins, {});

        pendingOutput.assign ((std::size_t) hopSize, 0.0f);

        buildBinTable();
        buildSignTable();

        for (auto& smoother : bandGain)
            smoother.reset (sampleRate / (double) hopSize, 40.0f, 0.0f);

        reset();
    }

    void reset() noexcept
    {
        std::fill (inputFrame.begin(), inputFrame.end(), 0.0f);
        std::fill (overlapBuffer.begin(), overlapBuffer.end(), 0.0f);
        std::fill (pendingOutput.begin(), pendingOutput.end(), 0.0f);

        fill = 0;
        pendingIndex = hopSize; // nothing to hand out yet
        holdback = 0.0f;

        for (auto& smoother : bandGain)
            smoother.snapTo (smoother.value());
    }

    /**
        Per-band Side gain. 0 means the band stays mono, 1 means the full
        quadrature component of that band is used as Side.
    */
    void setBandGains (const std::array<float, numBands>& gains) noexcept
    {
        for (int band = 0; band < numBands; ++band)
            bandGain[(std::size_t) band].setTarget (clampf (gains[(std::size_t) band], 0.0f, maxBandGain));
    }

    void setUniformGain (float gain) noexcept
    {
        for (auto& smoother : bandGain)
            smoother.setTarget (clampf (gain, 0.0f, maxBandGain));
    }

    /** How quickly the band gains may move, in milliseconds. */
    void setGainSmoothingMs (float timeMs) noexcept
    {
        const double frameRate = sampleRate / (double) std::max (1, hopSize);
        for (auto& smoother : bandGain)
            smoother.setTime (frameRate, timeMs);
    }

    /** Processes one Mid sample and returns the Side sample, delayed by fftSize. */
    float process (float input) noexcept
    {
        inputFrame[(std::size_t) fill++] = sanitise (input);

        if (fill == fftSize)
            processFrame();

        const float popped = pendingIndex < hopSize ? pendingOutput[(std::size_t) pendingIndex++] : 0.0f;

        // One extra sample of hold-back makes the total delay exactly fftSize.
        const float output = holdback;
        holdback = popped;

        return sanitise (output);
    }

    int getLatencySamples() const noexcept { return fftSize; }
    int getFrameSize() const noexcept { return fftSize; }
    int getHopSize() const noexcept { return hopSize; }

private:
    void processFrame() noexcept
    {
        for (int i = 0; i < fftSize; ++i)
            workFrame[(std::size_t) i] = inputFrame[(std::size_t) i] * window[(std::size_t) i];

        fft.forwardReal (workFrame.data(), spectrum.data());

        // Advance the per-band smoothers once per hop.
        std::array<float, numBands> gain {};
        for (int band = 0; band < numBands; ++band)
            gain[(std::size_t) band] = bandGain[(std::size_t) band].next();

        // DC and Nyquist are real: they cannot carry a quadrature component,
        // and a Side contribution there would only unbalance the channels.
        spectrum[0] = {};
        spectrum[(std::size_t) (numBins - 1)] = {};

        for (int bin = 1; bin < numBins - 1; ++bin)
        {
            const float amount = overlapMakeup * interpolatedGain (bin, gain);

            if (amount <= 1.0e-6f)
            {
                spectrum[(std::size_t) bin] = {};
                continue;
            }

            // Multiply by +-j: exact quadrature, magnitude untouched.
            const auto value = spectrum[(std::size_t) bin];
            const float scale = amount * sign[(std::size_t) bin];
            spectrum[(std::size_t) bin] = std::complex<float> (-scale * value.imag(),
                                                                scale * value.real());
        }

        fft.inverseReal (spectrum.data(), workFrame.data());

        for (int i = 0; i < fftSize; ++i)
            overlapBuffer[(std::size_t) i] += workFrame[(std::size_t) i] * window[(std::size_t) i] * normalisation;

        std::copy (overlapBuffer.begin(), overlapBuffer.begin() + hopSize, pendingOutput.begin());
        pendingIndex = 0;

        std::copy (overlapBuffer.begin() + hopSize, overlapBuffer.end(), overlapBuffer.begin());
        std::fill (overlapBuffer.end() - hopSize, overlapBuffer.end(), 0.0f);

        std::copy (inputFrame.begin() + hopSize, inputFrame.end(), inputFrame.begin());
        fill = fftSize - hopSize;
    }

    /**
        Band gains are interpolated across bins in log frequency, so a band
        edge never becomes an audible step and a moving gain never produces a
        moving notch.
    */
    float interpolatedGain (int bin, const std::array<float, numBands>& gain) const noexcept
    {
        const int lower = binLowerBand[(std::size_t) bin];

        if (lower < 0)
            return 0.0f; // below the lowest band: the bottom end stays mono

        const int upper = std::min (lower + 1, numBands - 1);
        return lerp (gain[(std::size_t) lower], gain[(std::size_t) upper], binBandFraction[(std::size_t) bin]);
    }

    void buildBinTable()
    {
        binLowerBand.assign ((std::size_t) numBins, -1);
        binBandFraction.assign ((std::size_t) numBins, 0.0f);

        const float binHz = (float) (sampleRate / (double) fftSize);

        for (int bin = 0; bin < numBins; ++bin)
        {
            const float hz = (float) bin * binHz;

            if (hz < VocalAnalyzer::bandLowEdgeHz (0))
                continue;

            // Band index as a continuous position, matching the analyser grid.
            const float position = std::log2 (hz / VocalAnalyzer::bandLowEdgeHz (0)) / 0.75f;
            const float clamped = clampf (position, 0.0f, (float) (numBands - 1));
            const int lower = std::min ((int) std::floor (clamped), numBands - 1);

            binLowerBand[(std::size_t) bin] = lower;
            binBandFraction[(std::size_t) bin] = clamped - (float) lower;
        }
    }

    void buildSignTable()
    {
        // Deterministic +-1 pattern, drawn once per bin group. Grouping keeps
        // neighbouring bins coherent, which is what stops transients from
        // smearing; a fixed seed keeps every build bit identical.
        sign.assign ((std::size_t) numBins, 1.0f);

        std::uint32_t state = 0x5bd1e995u;
        constexpr int groupSize = 3;
        float groupSign = 1.0f;

        for (int bin = 0; bin < numBins; ++bin)
        {
            if (bin % groupSize == 0)
            {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                groupSign = (state & 0x10000u) != 0u ? 1.0f : -1.0f;
            }

            sign[(std::size_t) bin] = groupSign;
        }
    }

    double sampleRate = 48000.0;
    int fftSize = 1024;
    int hopSize = 256;
    int numBins = 513;
    float normalisation = 1.0f;

    Fft fft;
    std::vector<float> window;
    std::vector<float> inputFrame;
    std::vector<float> workFrame;
    std::vector<float> overlapBuffer;
    std::vector<std::complex<float>> spectrum;
    std::vector<float> sign;
    std::vector<int> binLowerBand;
    std::vector<float> binBandFraction;

    std::vector<float> pendingOutput;
    int pendingIndex = 0;
    float holdback = 0.0f;
    int fill = 0;

    std::array<Smoother, numBands> bandGain {};
};

} // namespace wp
