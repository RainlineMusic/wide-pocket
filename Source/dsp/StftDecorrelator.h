/* Wide Pocket - Natural v0.2 decorrelator: static quadrature rotation, no recursive tail. */
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

/*  Why this version has no memory at all
    -------------------------------------
    The previous build made the Side candidate with a cascade of four
    recursive Schroeder allpass stages running in the STFT bin domain, plus
    a four frame pre delay. Both stages store energy: the cascade feeds
    back over 1, 2, 3 and 5 frames (up to about 13 ms at 48 kHz) and the
    pre delay is a discrete ~10 ms echo. Summed back into L and R that is
    heard exactly as reported, a small metallic room around the voice, and
    it is worst on a male voice because the dense low harmonics keep
    re-exciting the tail.

    Here the Side candidate is the Mid spectrum multiplied by a frozen
    phase rotation. The plugin is then a pure static allpass: no feedback,
    no echo, group delay bounded to a few tens of samples, the mono sum
    untouched, and nothing left that can ring.

    The rotation sits on quadrature (+-90 degrees) and only wanders a
    bounded amount around it. That matters: the band orthogonalisation below
    removes whatever part of the candidate is in phase with the Mid, so a
    rotation that sits near 0 degrees in any region of the spectrum would
    leave almost no Side there. Quadrature is the point where the candidate
    is already orthogonal to the Mid, so the full magnitude survives.

    The sign of that quadrature alternates across small groups of bins.
    Using +90 degrees everywhere keeps the energy but makes the Side a plain
    Hilbert transform of the Mid, and a Hilbert copy with a consistent sign
    pushes the short term image off centre (it measured 0.6 dB of average
    50 ms level difference and 1.8 dB of gain on a held tone). Alternating
    the sign leaves every bin orthogonal to the Mid while cancelling that
    systematic lean, and grouping the flips (rather than flipping every bin)
    keeps neighbouring bins coherent enough that the Side does not cancel
    itself inside a band. */
class StftDecorrelator
{
public:
    static constexpr int numBands = VocalAnalyzer::numMaskBands;
    static constexpr float maxBandGain = 1.25f;

    void prepare (double sr, int = 8)
    {
        sampleRate = std::max (8000.0, sr);
        fftSize = 256;
        hopSize = 128;
        numBins = 129;
        fft.setOrder (8);

        window.resize (fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = std::sin ((float) kPi * ((float) i + 0.5f) / (float) fftSize);

        inputFrame.assign (fftSize, 0.0f);
        workFrame.assign (fftSize, 0.0f);
        overlap.assign (fftSize, 0.0f);
        pending.assign (hopSize, 0.0f);
        directSpectrum.assign (numBins, {});
        processedSpectrum.assign (numBins, {});
        binBand.assign (numBins, 0);

        buildRotationTable();
        buildBandTable();

        const double frameRate = sampleRate / hopSize;
        for (auto& x : bandGain)
            x.reset (frameRate, 240, 0);

        reset();
    }

    void reset() noexcept
    {
        std::fill (inputFrame.begin(), inputFrame.end(), 0.0f);
        std::fill (overlap.begin(), overlap.end(), 0.0f);
        std::fill (pending.begin(), pending.end(), 0.0f);
        fill = 0;
        pendingIndex = hopSize;
        previousTransientEnergy = smoothedTransientEnergy = 1e-9f;
        holdFrames = inhibitFrames = 0;
        transientActive = false;
    }

    void setBandGains (const std::array<float, numBands>& g) noexcept
    {
        for (int b = 0; b < numBands; ++b)
            bandGain[(size_t) b].setTarget (clampf (g[(size_t) b], 0.0f, maxBandGain));
    }

    void setUniformGain (float g) noexcept
    {
        for (auto& x : bandGain)
            x.setTarget (clampf (g, 0.0f, maxBandGain));
    }

    void setGainSmoothingMs (float ms) noexcept
    {
        for (auto& x : bandGain)
            x.setTime (sampleRate / hopSize, clampf (ms, 120.0f, 600.0f));
    }

    float process (float x) noexcept
    {
        inputFrame[(size_t) fill++] = sanitise (x);

        if (fill == fftSize)
            processFrame();

        return pendingIndex < hopSize ? sanitise (pending[(size_t) pendingIndex++]) : 0.0f;
    }

    int getLatencySamples() const noexcept { return fftSize; }
    int getFrameSize() const noexcept { return fftSize; }
    int getHopSize() const noexcept { return hopSize; }
    bool isTransientActive() const noexcept { return transientActive; }

private:
    void processFrame() noexcept
    {
        for (int i = 0; i < fftSize; ++i)
            workFrame[(size_t) i] = inputFrame[(size_t) i] * window[(size_t) i];

        fft.forwardReal (workFrame.data(), directSpectrum.data());
        updateTransientState();

        /* Static allpass: same rotation on every frame, so nothing is
           modulated and nothing decays. */
        for (int b = 0; b < numBins; ++b)
            processedSpectrum[(size_t) b] = directSpectrum[(size_t) b] * rotation[(size_t) b];

        std::array<float, numBands> g {};
        for (int b = 0; b < numBands; ++b)
            g[(size_t) b] = bandGain[(size_t) b].next();

        std::array<double, numBands> cross {}, direct {};
        for (int b = 1; b < numBins - 1; ++b)
        {
            const auto x = directSpectrum[(size_t) b];
            const auto y = processedSpectrum[(size_t) b];
            const int band = binBand[(size_t) b];
            cross[(size_t) band] += (double) (x.real() * y.real() + x.imag() * y.imag());
            direct[(size_t) band] += (double) std::norm (x);
        }

        /* Exact broad band orthogonalisation: keeps the centre locked without
           a time domain servo. Because the rotation is already close to
           quadrature, the removed in phase part is small and the Side keeps
           the spectrum of the Mid. */
        for (int b = 1; b < numBins - 1; ++b)
        {
            const int band = binBand[(size_t) b];
            const float p = clampf ((float) (cross[(size_t) band] / std::max (1e-12, direct[(size_t) band])), -1.5f, 1.5f);
            processedSpectrum[(size_t) b] -= p * directSpectrum[(size_t) b];
        }

        processedSpectrum[0] = {};
        processedSpectrum[(size_t) (numBins - 1)] = {};

        const float gate = transientActive ? 0.0f : 1.0f;
        for (int b = 1; b < numBins - 1; ++b)
            processedSpectrum[(size_t) b] *= gate * g[(size_t) binBand[(size_t) b]];

        fft.inverseReal (processedSpectrum.data(), workFrame.data());

        for (int i = 0; i < fftSize; ++i)
            overlap[(size_t) i] += workFrame[(size_t) i] * window[(size_t) i];

        std::copy (overlap.begin(), overlap.begin() + hopSize, pending.begin());
        pendingIndex = 0;
        std::copy (overlap.begin() + hopSize, overlap.end(), overlap.begin());
        std::fill (overlap.end() - hopSize, overlap.end(), 0.0f);
        std::copy (inputFrame.begin() + hopSize, inputFrame.end(), inputFrame.begin());
        fill = fftSize - hopSize;
    }

    void updateTransientState() noexcept
    {
        float e = 0.0f;
        for (int b = 4; b < numBins; ++b)
            e += std::norm (directSpectrum[(size_t) b]);

        previousTransientEnergy = smoothedTransientEnergy;
        smoothedTransientEnergy = 0.4f * e + 0.6f * smoothedTransientEnergy;
        const bool onset = e > 1e-8f && smoothedTransientEnergy > 2.8f * previousTransientEnergy;

        if (onset && inhibitFrames == 0)
        {
            holdFrames = 8;
            inhibitFrames = 56;
        }

        if (holdFrames > 0)
            --holdFrames;
        if (inhibitFrames > 0)
            --inhibitFrames;

        transientActive = holdFrames > 0;
    }

    /* A frozen bounded random walk around quadrature, with the sign of the
       quadrature re-drawn every signFlipBins bins. The per bin step and the
       total excursion are both bounded, so the equivalent impulse response
       stays inside a small part of the analysis window (a few tens of
       samples of group delay at 48 kHz) and cannot smear transients or build
       a tail, while still being different enough across frequency to
       decorrelate. */
    void buildRotationTable()
    {
        rotation.assign ((size_t) numBins, { 0.0f, 1.0f });
        std::uint32_t state = 0x9e3779b9u;
        float deviation = 0.0f;
        bool positive = true;

        for (int b = 0; b < numBins; ++b)
        {
            state = state * 1664525u + 1013904223u;
            const float uniform = (float) ((state >> 8) & 0xffffffu) / (float) 0xffffff - 0.5f;
            deviation = clampf (deviation + 2.0f * maxPhaseStep * uniform, -maxDeviation, maxDeviation);

            if (b % signFlipBins == 0)
                positive = ((state >> 23) & 1u) != 0u;

            const float sign = positive ? 1.0f : -1.0f;
            rotation[(size_t) b] = std::polar (1.0f, sign * (0.5f * (float) kPi + deviation));
        }
    }

    void buildBandTable()
    {
        const float binHz = (float) sampleRate / (float) fftSize;
        for (int b = 0; b < numBins; ++b)
        {
            const float hz = std::max (60.0f, (float) b * binHz);
            const int band = (int) std::floor (std::log2 (hz / 60.0f) / 0.75f);
            binBand[(size_t) b] = std::max (0, std::min (numBands - 1, band));
        }
    }

    static constexpr float maxPhaseStep = 0.45f;
    static constexpr float maxDeviation = 0.5f;
    static constexpr int signFlipBins = 7;

    double sampleRate = 48000.0;
    int fftSize = 256, hopSize = 128, numBins = 129;
    Fft fft;
    std::vector<float> window, inputFrame, workFrame, overlap, pending;
    std::vector<std::complex<float>> directSpectrum, processedSpectrum, rotation;
    std::vector<int> binBand;
    std::array<Smoother, numBands> bandGain {};
    int fill = 0, pendingIndex = 0;
    float previousTransientEnergy = 1e-9f, smoothedTransientEnergy = 1e-9f;
    int holdFrames = 0, inhibitFrames = 0;
    bool transientActive = false;
};

} // namespace wp
