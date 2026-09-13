/*
    Wide Pocket - natural subband decorrelator.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    A 50%-overlapped 256-point sine-window STFT feeds four Schroeder
    allpasses in each complex subband (DAFx23: delays 1,2,3,5 frames,
    gamma 0.7). A per-bin t/F envelope shaper restores the direct spectral
    envelope. Finally a spectral centre lock removes the real projection of
    the processed signal onto the direct signal in broad perceptual bands.

    Unlike the former time-domain CenterAlignment servo, the centre lock acts
    in the same representation that creates Side. It cannot introduce an
    inter-band crossover error, and each processed frame is energy-balanced
    before synthesis. A small quadrature seed prevents bin-centred stationary
    partials from collapsing to mono after orthogonalisation.
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

class StftDecorrelator
{
public:
    static constexpr int numBands = VocalAnalyzer::numMaskBands;
    static constexpr float maxBandGain = 1.25f;

    void prepare (double newSampleRate, int /*unusedOrder*/ = 8)
    {
        sampleRate = std::max (8000.0, newSampleRate);
        fftSize = 256;
        hopSize = fftSize / 2;
        numBins = fftSize / 2 + 1;
        fft.setOrder (8);

        window.resize ((std::size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(std::size_t) i] = std::sin ((float) kPi * ((float) i + 0.5f) / (float) fftSize);

        inputFrame.assign ((std::size_t) fftSize, 0.0f);
        workFrame.assign ((std::size_t) fftSize, 0.0f);
        overlap.assign ((std::size_t) fftSize, 0.0f);
        pending.assign ((std::size_t) hopSize, 0.0f);
        directSpectrum.assign ((std::size_t) numBins, {});
        processedSpectrum.assign ((std::size_t) numBins, {});
        directEnergy.assign ((std::size_t) numBins, 0.0f);
        processedEnergy.assign ((std::size_t) numBins, 0.0f);
        binBand.assign ((std::size_t) numBins, 0);

        preDelay.assign ((std::size_t) preDelayFrames * (std::size_t) numBins, {});
        preWrite = 0;

        const int delays[numStages] = { 1, 2, 3, 5 };
        for (int stage = 0; stage < numStages; ++stage)
        {
            stages[(std::size_t) stage].delay = delays[stage];
            stages[(std::size_t) stage].write = 0;
            stages[(std::size_t) stage].xDelay.assign ((std::size_t) delays[stage] * (std::size_t) numBins, {});
            stages[(std::size_t) stage].yDelay.assign ((std::size_t) delays[stage] * (std::size_t) numBins, {});
        }

        buildBandTable();
        for (auto& smoother : bandGain)
            smoother.reset (sampleRate / (double) hopSize, 240.0f, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        std::fill (inputFrame.begin(), inputFrame.end(), 0.0f);
        std::fill (overlap.begin(), overlap.end(), 0.0f);
        std::fill (pending.begin(), pending.end(), 0.0f);
        std::fill (preDelay.begin(), preDelay.end(), std::complex<float> {});
        std::fill (directEnergy.begin(), directEnergy.end(), 0.0f);
        std::fill (processedEnergy.begin(), processedEnergy.end(), 0.0f);
        for (auto& stage : stages)
        {
            std::fill (stage.xDelay.begin(), stage.xDelay.end(), std::complex<float> {});
            std::fill (stage.yDelay.begin(), stage.yDelay.end(), std::complex<float> {});
            stage.write = 0;
        }
        fill = 0;
        pendingIndex = hopSize;
        preWrite = 0;
        previousTransientEnergy = 1.0e-9f;
        smoothedTransientEnergy = 1.0e-9f;
        holdFrames = inhibitFrames = 0;
        transientActive = false;
    }

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

    void setGainSmoothingMs (float timeMs) noexcept
    {
        const double frameRate = sampleRate / (double) hopSize;
        for (auto& smoother : bandGain)
            smoother.setTime (frameRate, clampf (timeMs, 120.0f, 600.0f));
    }

    float process (float input) noexcept
    {
        inputFrame[(std::size_t) fill++] = sanitise (input);
        if (fill == fftSize)
            processFrame();
        return pendingIndex < hopSize ? sanitise (pending[(std::size_t) pendingIndex++]) : 0.0f;
    }

    int getLatencySamples() const noexcept { return fftSize; }
    int getFrameSize() const noexcept { return fftSize; }
    int getHopSize() const noexcept { return hopSize; }
    bool isTransientActive() const noexcept { return transientActive; }

private:
    struct AllpassStage
    {
        int delay = 1, write = 0;
        std::vector<std::complex<float>> xDelay, yDelay;
    };

    void processFrame() noexcept
    {
        for (int i = 0; i < fftSize; ++i)
            workFrame[(std::size_t) i] = inputFrame[(std::size_t) i] * window[(std::size_t) i];
        fft.forwardReal (workFrame.data(), directSpectrum.data());

        updateTransientState();

        for (int bin = 0; bin < numBins; ++bin)
        {
            const auto delayed = preDelay[(std::size_t) preWrite * (std::size_t) numBins + (std::size_t) bin];
            // Do not inject detected onsets into the recursive allpass path.
            // Muting only its output would let the delayed transient reappear
            // after the hold window as a long, low-level tail.
            preDelay[(std::size_t) preWrite * (std::size_t) numBins + (std::size_t) bin]
                = transientActive ? std::complex<float> {} : directSpectrum[(std::size_t) bin];
            processedSpectrum[(std::size_t) bin] = delayed;
        }
        preWrite = (preWrite + 1) % preDelayFrames;

        for (auto& stage : stages)
        {
            for (int bin = 0; bin < numBins; ++bin)
            {
                const std::size_t index = (std::size_t) stage.write * (std::size_t) numBins + (std::size_t) bin;
                const auto x = processedSpectrum[(std::size_t) bin];
                const auto y = gamma * x + stage.xDelay[index] - gamma * stage.yDelay[index];
                stage.xDelay[index] = x;
                stage.yDelay[index] = y;
                processedSpectrum[(std::size_t) bin] = y;
            }
            stage.write = (stage.write + 1) % stage.delay;
        }

        std::array<float, numBands> gains {};
        for (int band = 0; band < numBands; ++band)
            gains[(std::size_t) band] = bandGain[(std::size_t) band].next();

        std::array<double, numBands> cross {}, directBand {}, processedBand {};
        for (int bin = 1; bin < numBins - 1; ++bin)
        {
            const auto x = directSpectrum[(std::size_t) bin];
            auto y = processedSpectrum[(std::size_t) bin];

            const float edNow = std::norm (x);
            const float epNow = std::norm (y);
            auto& ed = directEnergy[(std::size_t) bin];
            auto& ep = processedEnergy[(std::size_t) bin];
            ed = envelopeAlpha * edNow + (1.0f - envelopeAlpha) * ed;
            ep = envelopeAlpha * epNow + (1.0f - envelopeAlpha) * ep;

            float envelopeGain = 1.0f;
            if (ep > envelopeBeta * ed)
                envelopeGain = std::sqrt (envelopeBeta * ed / std::max (ep, 1.0e-12f));
            else if (ed > envelopeBeta * ep)
                envelopeGain = std::sqrt (ed / std::max (envelopeBeta * ep, 1.0e-12f));
            y *= 1.1f * clampf (envelopeGain, 0.35f, 2.5f);

            // A low-level quadrature floor makes stationary bin-centred
            // harmonics widen too, without the old random sign discontinuities.
            y += std::complex<float> (-quadratureFloor * x.imag(), quadratureFloor * x.real());
            processedSpectrum[(std::size_t) bin] = y;

            const int band = binBand[(std::size_t) bin];
            cross[(std::size_t) band] += (double) (x.real() * y.real() + x.imag() * y.imag());
            directBand[(std::size_t) band] += (double) std::norm (x);
        }

        // Spectral centre lock. Remove only the in-phase component per broad
        // perceptual band; unlike the old CenterAlignment this has no moving
        // crossover filters in the time-domain output.
        for (int bin = 1; bin < numBins - 1; ++bin)
        {
            const int band = binBand[(std::size_t) bin];
            const float projection = clampf ((float) (cross[(std::size_t) band]
                                                       / std::max (1.0e-12, directBand[(std::size_t) band])),
                                              -1.5f, 1.5f);
            processedSpectrum[(std::size_t) bin] -= projection * directSpectrum[(std::size_t) bin];
            processedBand[(std::size_t) band] += (double) std::norm (processedSpectrum[(std::size_t) bin]);
        }

        std::array<float, numBands> energyMatch {};
        for (int band = 0; band < numBands; ++band)
            energyMatch[(std::size_t) band] = clampf ((float) std::sqrt (directBand[(std::size_t) band]
                                                  / std::max (1.0e-12, processedBand[(std::size_t) band])),
                                                     0.35f, 2.0f);

        processedSpectrum[0] = {};
        processedSpectrum[(std::size_t) (numBins - 1)] = {};
        for (int bin = 1; bin < numBins - 1; ++bin)
        {
            const int band = binBand[(std::size_t) bin];
            const float transientGain = transientActive ? 0.0f : 1.0f;
            processedSpectrum[(std::size_t) bin] *= transientGain * gains[(std::size_t) band]
                                                   * energyMatch[(std::size_t) band];
        }

        fft.inverseReal (processedSpectrum.data(), workFrame.data());
        for (int i = 0; i < fftSize; ++i)
            overlap[(std::size_t) i] += workFrame[(std::size_t) i] * window[(std::size_t) i];

        std::copy (overlap.begin(), overlap.begin() + hopSize, pending.begin());
        pendingIndex = 0;
        std::copy (overlap.begin() + hopSize, overlap.end(), overlap.begin());
        std::fill (overlap.end() - hopSize, overlap.end(), 0.0f);
        std::copy (inputFrame.begin() + hopSize, inputFrame.end(), inputFrame.begin());
        fill = fftSize - hopSize;
    }

    void updateTransientState() noexcept
    {
        float energy = 0.0f;
        for (int bin = 4; bin < numBins; ++bin)
            energy += std::norm (directSpectrum[(std::size_t) bin]);

        previousTransientEnergy = smoothedTransientEnergy;
        smoothedTransientEnergy = 0.4f * energy + 0.6f * smoothedTransientEnergy;
        const bool onset = energy > 1.0e-8f && smoothedTransientEnergy > 2.8f * previousTransientEnergy;

        if (onset && inhibitFrames == 0)
        {
            holdFrames = 8;
            inhibitFrames = 56;
        }
        if (holdFrames > 0) --holdFrames;
        if (inhibitFrames > 0) --inhibitFrames;
        transientActive = holdFrames > 0;
    }

    void buildBandTable()
    {
        const float binHz = (float) sampleRate / (float) fftSize;
        for (int bin = 0; bin < numBins; ++bin)
        {
            const float hz = std::max (60.0f, (float) bin * binHz);
            const int band = (int) std::floor (std::log2 (hz / 60.0f) / 0.75f);
            binBand[(std::size_t) bin] = std::max (0, std::min (numBands - 1, band));
        }
    }

    static constexpr int numStages = 4;
    static constexpr int preDelayFrames = 4;
    static constexpr float gamma = 0.7f;
    static constexpr float envelopeAlpha = 0.4f;
    static constexpr float envelopeBeta = 1.5f;
    static constexpr float quadratureFloor = 0.18f;

    double sampleRate = 48000.0;
    int fftSize = 256, hopSize = 128, numBins = 129;
    Fft fft;
    std::vector<float> window, inputFrame, workFrame, overlap, pending;
    std::vector<std::complex<float>> directSpectrum, processedSpectrum, preDelay;
    std::vector<float> directEnergy, processedEnergy;
    std::vector<int> binBand;
    std::array<AllpassStage, numStages> stages {};
    std::array<Smoother, numBands> bandGain {};
    int fill = 0, pendingIndex = 0, preWrite = 0;
    float previousTransientEnergy = 1.0e-9f, smoothedTransientEnergy = 1.0e-9f;
    int holdFrames = 0, inhibitFrames = 0;
    bool transientActive = false;
};

} // namespace wp
