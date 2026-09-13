/*
    Wide Pocket - vocal analyser.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    The analyser is the part that makes the widener "know" it is looking at a
    voice. It runs on the Mid signal only, on a hop grid, and produces a set of
    normalised features that every engine consumes:

      voicing       harmonic, pitched content is present
      transient     an attack is happening right now
      harmonicity   how much of the energy sits on harmonic peaks
      sibilance     S / SH / T energy dominance in the 5-12 kHz region
      plosive       low frequency burst (P, B, popped mic)
      flatness      spectral flatness, tonal (0) to noise-like (1)
      flux          how fast the spectrum is changing

    From those features it derives the spatial residual mask: the per-band
    permission to widen. Bands that carry the intelligibility of the voice
    (fundamental, first formants) keep their permission low, noise-like and
    upper-formant bands are allowed to spread.
*/

#pragma once

#include "Fft.h"
#include "Filters.h"
#include "WidePocketTypes.h"

#include <array>
#include <complex>
#include <vector>

namespace wp
{

class VocalAnalyzer
{
public:
    static constexpr int fftOrder = 10;                 // 1024 point analysis
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int hopSize = fftSize / 4;         // 75 % overlap
    static constexpr int numBins = fftSize / 2 + 1;

    void prepare (double newSampleRate)
    {
        sampleRate = std::max (8000.0, newSampleRate);

        fft.setOrder (fftOrder);
        makeHannWindow (window, fftSize);

        inputRing.assign (fftSize, 0.0f);
        frame.assign (fftSize, 0.0f);
        spectrum.assign (numBins, {});
        magnitude.assign (numBins, 0.0f);
        previousMagnitude.assign (numBins, 0.0f);
        mask.fill (0.0f);

        writeIndex = 0;
        samplesUntilFrame = hopSize;

        transientEnvelope.prepare (sampleRate, 1.0f, 80.0f);
        levelEnvelope.prepare (sampleRate, 10.0f, 200.0f);

        current = AnalyzerFrame {};
        smoothedFlux = 0.0f;
    }

    void reset()
    {
        std::fill (inputRing.begin(), inputRing.end(), 0.0f);
        std::fill (previousMagnitude.begin(), previousMagnitude.end(), 0.0f);
        mask.fill (0.0f);
        writeIndex = 0;
        samplesUntilFrame = hopSize;
        transientEnvelope.reset();
        levelEnvelope.reset();
        current = AnalyzerFrame {};
        smoothedFlux = 0.0f;
    }

    /** Feeds one Mid sample. Returns true when a new analysis frame was produced. */
    bool pushSample (float mid) noexcept
    {
        inputRing[(std::size_t) writeIndex] = sanitise (mid);
        writeIndex = (writeIndex + 1) % fftSize;

        current.level = levelEnvelope.process (mid);
        const float fast = transientEnvelope.process (mid);
        const float slow = levelEnvelope.value();
        current.transient = clamp01 ((fast - slow) / std::max (1.0e-4f, slow + 1.0e-3f));

        if (--samplesUntilFrame > 0)
            return false;

        samplesUntilFrame = hopSize;
        analyseFrame();
        return true;
    }

    const AnalyzerFrame& getFrame() const noexcept { return current; }

    /** Number of logarithmic bands used by the spatial residual mask. */
    static constexpr int numMaskBands = 12;

    /** Per-band permission to widen, 0 = keep centred, 1 = free to spread. */
    const std::array<float, numMaskBands>& getSpatialMask() const noexcept { return mask; }

    /** Lower edge of a mask band, in Hz. */
    static float bandLowEdgeHz (int band) noexcept
    {
        return 60.0f * std::pow (2.0f, (float) band * 0.75f);
    }

private:
    void analyseFrame()
    {
        for (int i = 0; i < fftSize; ++i)
        {
            const int index = (writeIndex + i) % fftSize;
            frame[(std::size_t) i] = inputRing[(std::size_t) index] * window[(std::size_t) i];
        }

        fft.forwardReal (frame.data(), spectrum.data());

        float total = 0.0f;
        float logSum = 0.0f;
        float flux = 0.0f;
        float lowEnergy = 0.0f;   // below 150 Hz, plosives
        float voiceEnergy = 0.0f; // 150 Hz - 4 kHz, fundamental and formants
        float sibilantEnergy = 0.0f; // 5 kHz - 12 kHz
        float peakEnergy = 0.0f;

        const float binHz = (float) (sampleRate / (double) fftSize);

        for (int bin = 0; bin < numBins; ++bin)
        {
            const float mag = std::abs (spectrum[(std::size_t) bin]);
            magnitude[(std::size_t) bin] = mag;

            const float power = mag * mag;
            total += power;
            logSum += std::log (power + 1.0e-12f);

            const float difference = mag - previousMagnitude[(std::size_t) bin];
            if (difference > 0.0f)
                flux += difference;

            const float hz = (float) bin * binHz;
            if (hz < 150.0f)
                lowEnergy += power;
            else if (hz < 4000.0f)
                voiceEnergy += power;

            if (hz >= 5000.0f && hz <= 12000.0f)
                sibilantEnergy += power;
        }

        // Harmonic peak energy: a bin counts as a peak when it dominates both
        // neighbours. For a pitched voice the peaks carry most of the energy.
        for (int bin = 2; bin < numBins - 2; ++bin)
        {
            const float mag = magnitude[(std::size_t) bin];
            if (mag > magnitude[(std::size_t) (bin - 1)] && mag > magnitude[(std::size_t) (bin + 1)]
                && mag > magnitude[(std::size_t) (bin - 2)] && mag > magnitude[(std::size_t) (bin + 2)])
                peakEnergy += mag * mag;
        }

        const float safeTotal = std::max (1.0e-12f, total);

        // Geometric mean over arithmetic mean, i.e. Wiener entropy.
        const float geometricMean = std::exp (logSum / (float) numBins);
        const float arithmeticMean = safeTotal / (float) numBins;
        current.flatness = clamp01 (geometricMean / std::max (1.0e-12f, arithmeticMean));

        const float normalisedFlux = flux / (float) numBins / std::max (1.0e-6f, std::sqrt (arithmeticMean));
        smoothedFlux += 0.3f * (clamp01 (normalisedFlux * 0.25f) - smoothedFlux);
        current.flux = smoothedFlux;

        current.harmonicity = clamp01 (peakEnergy / safeTotal);
        current.sibilance = clamp01 (3.0f * sibilantEnergy / safeTotal);
        current.plosive = clamp01 (4.0f * lowEnergy / safeTotal);

        // A voice is present when energy concentrates in the speech range and
        // the spectrum is harmonic rather than flat.
        const float speechRatio = voiceEnergy / safeTotal;
        current.voicing = clamp01 (1.4f * speechRatio * (1.0f - current.flatness) * (0.4f + current.harmonicity));

        updateMask (binHz);

        previousMagnitude = magnitude;
    }

    void updateMask (float binHz)
    {
        // Energy per logarithmic band, then the residual: how much of the band
        // is *not* explained by the harmonic/intelligibility content.
        std::array<float, numMaskBands> bandEnergy {};
        std::array<float, numMaskBands> bandPeak {};
        bandEnergy.fill (0.0f);
        bandPeak.fill (0.0f);

        for (int bin = 1; bin < numBins; ++bin)
        {
            const float hz = (float) bin * binHz;
            const int band = bandForHz (hz);
            if (band < 0)
                continue;

            const float power = magnitude[(std::size_t) bin] * magnitude[(std::size_t) bin];
            bandEnergy[(std::size_t) band] += power;
            bandPeak[(std::size_t) band] = std::max (bandPeak[(std::size_t) band], power);
        }

        for (int band = 0; band < numMaskBands; ++band)
        {
            const float energy = bandEnergy[(std::size_t) band];
            const float peak = bandPeak[(std::size_t) band];

            // Tonal bands have their energy concentrated in a single bin, so a
            // high peak-to-total ratio means "protect this band".
            const float tonality = clamp01 (peak / std::max (1.0e-12f, energy) * 4.0f);
            const float residual = 1.0f - tonality;

            const float lowHz = bandLowEdgeHz (band);

            // Intelligibility weighting: the fundamental and the first two
            // formants stay centred, everything above spreads more freely.
            float permission = residual;
            if (lowHz < 200.0f)
                permission *= 0.1f;
            else if (lowHz < 800.0f)
                permission *= 0.45f;
            else if (lowHz < 2500.0f)
                permission *= 0.75f;

            // Sibilance and plosives are actively suppressed.
            if (lowHz >= 5000.0f)
                permission *= 1.0f - 0.8f * current.sibilance;
            if (lowHz < 200.0f)
                permission *= 1.0f - 0.9f * current.plosive;

            auto& value = mask[(std::size_t) band];
            value += 0.25f * (clamp01 (permission) - value);
        }
    }

    static int bandForHz (float hz) noexcept
    {
        if (hz < 60.0f)
            return -1;

        const int band = (int) std::floor (std::log2 (hz / 60.0f) / 0.75f);
        return band < numMaskBands ? band : -1;
    }

    double sampleRate = 48000.0;

    Fft fft;
    std::vector<float> window;
    std::vector<float> inputRing;
    std::vector<float> frame;
    std::vector<std::complex<float>> spectrum;
    std::vector<float> magnitude;
    std::vector<float> previousMagnitude;

    std::array<float, numMaskBands> mask {};

    EnvelopeFollower transientEnvelope;
    EnvelopeFollower levelEnvelope;

    AnalyzerFrame current;
    float smoothedFlux = 0.0f;

    int writeIndex = 0;
    int samplesUntilFrame = hopSize;
};

} // namespace wp
