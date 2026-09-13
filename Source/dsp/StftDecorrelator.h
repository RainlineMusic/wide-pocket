/* Wide Pocket - Natural v0.3 decorrelator: static per-bin quadrature, smooth ducking. */
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

/*  What changed in v0.3
    --------------------
    1. Orthogonalisation is now per bin instead of per band. Summing the
       in-phase part over a whole band only guarantees that the *average*
       level difference of the band is zero; inside the band individual bins
       keep a residual in-phase component, and as the spectrum moves that
       residual wanders. Measured on a real vocal take that was a -0.13 dB
       mean and up to 1.4 dB short-term lean. Removing the in-phase part bin
       by bin makes every bin exactly quadrature to the Mid, so
       Re{M * conj(S)} = 0 everywhere and the image cannot move at all.

       Decorrelation then comes entirely from the sign of the quadrature,
       which is re-drawn every signFlipBins bins. That is enough: both signs
       are orthogonal to the Mid, so flipping them costs nothing in centring
       but breaks the Hilbert relationship that would otherwise make the Side
       a predictable copy of the Mid.

    2. The transient handling is no longer a hard gate. Zeroing the Side for
       eight frames made the widening stop dead for ~21 ms on every onset,
       which is exactly the "sudden stops in the Side" that was reported
       (0.9 % of all active frames had no Side at all). It is now a smooth
       duck: the depth is set by the Transient control, the attack is a few
       milliseconds and the release is slow, so the width dips and recovers
       instead of switching off.
*/
class StftDecorrelator
{
public:
    static constexpr int numBands = VocalAnalyzer::numMaskBands;
    static constexpr float maxBandGain = 2.5f;

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

        buildSignTable();
        buildBandTable();

        const double frameRate = sampleRate / hopSize;
        for (auto& x : bandGain)
            x.reset (frameRate, 240, 0);

        duckAttack = 1.0f - std::exp (-1.0f / (float) std::max (1.0, frameRate * 0.004));  // ~4 ms
        duckRelease = 1.0f - std::exp (-1.0f / (float) std::max (1.0, frameRate * 0.090)); // ~90 ms

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
        duck = 1.0f;
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
            x.setTime (sampleRate / hopSize, clampf (ms, 60.0f, 600.0f));
    }

    /** How much the Side is ducked on an onset. 0 = no ducking, 1 = silent. */
    void setDuckDepth (float depth) noexcept { duckDepth = clamp01 (depth); }

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
    float getDuck() const noexcept { return duck; }

private:
    void processFrame() noexcept
    {
        for (int i = 0; i < fftSize; ++i)
            workFrame[(size_t) i] = inputFrame[(size_t) i] * window[(size_t) i];

        fft.forwardReal (workFrame.data(), directSpectrum.data());
        updateTransientState();

        std::array<float, numBands> g {};
        for (int b = 0; b < numBands; ++b)
            g[(size_t) b] = bandGain[(size_t) b].next();

        processedSpectrum[0] = {};
        processedSpectrum[(size_t) (numBins - 1)] = {};

        /* Exact per-bin quadrature: S[k] = j * s[k] * M[k], with s[k] = +-1.
           Re{M[k] * conj(S[k])} is then identically zero in every bin, and
           since the inter-channel level difference of L = M + S, R = M - S is
           exactly 4 * sum_k Re{M * conj(S)}, the image is centred for any set
           of real band gains. Nothing here can move the image. */
        for (int b = 1; b < numBins - 1; ++b)
        {
            const auto m = directSpectrum[(size_t) b];
            const float s = sign[(size_t) b];
            const float gain = duck * g[(size_t) binBand[(size_t) b]];
            processedSpectrum[(size_t) b] = std::complex<float> (-s * m.imag(), s * m.real()) * gain;
        }

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
            holdFrames = 6;
            inhibitFrames = 40;
        }

        if (holdFrames > 0)
            --holdFrames;
        if (inhibitFrames > 0)
            --inhibitFrames;

        transientActive = holdFrames > 0;

        /* Smooth duck instead of a hard gate: fast in, slow out, and never
           deeper than the Transient control asks for. */
        const float target = transientActive ? 1.0f - duckDepth : 1.0f;
        const float coefficient = target < duck ? duckAttack : duckRelease;
        duck += coefficient * (target - duck);
        duck = clampf (duck, 0.0f, 1.0f);
    }

    /* A frozen +-1 sign per group of bins. Magnitudes are untouched, so the
       Side keeps the spectrum of the Mid exactly, and the equivalent impulse
       response stays short: no tail, no echo, no smearing. */
    void buildSignTable()
    {
        sign.assign ((size_t) numBins, 1.0f);
        std::uint32_t state = 0x9e3779b9u;
        float current = 1.0f;

        for (int b = 0; b < numBins; ++b)
        {
            state = state * 1664525u + 1013904223u;

            if (b % signFlipBins == 0)
                current = (((state >> 23) & 1u) != 0u) ? 1.0f : -1.0f;

            sign[(size_t) b] = current;
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

    // Five bins per sign group measured best: wider groups approach a plain
    // Hilbert transform (which correlates with the Mid again), narrower ones
    // put too many sign boundaries inside the leakage of a single partial.
    static constexpr int signFlipBins = 5;

    double sampleRate = 48000.0;
    int fftSize = 256, hopSize = 128, numBins = 129;
    Fft fft;
    std::vector<float> window, inputFrame, workFrame, overlap, pending, sign;
    std::vector<std::complex<float>> directSpectrum, processedSpectrum;
    std::vector<int> binBand;
    std::array<Smoother, numBands> bandGain {};
    int fill = 0, pendingIndex = 0;
    float previousTransientEnergy = 1e-9f, smoothedTransientEnergy = 1e-9f;
    int holdFrames = 0, inhibitFrames = 0;
    bool transientActive = false;
    float duck = 1.0f, duckDepth = 0.5f, duckAttack = 0.5f, duckRelease = 0.05f;
};

} // namespace wp
