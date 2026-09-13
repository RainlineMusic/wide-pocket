/*
    Wide Pocket - STFT sub-band all-pass decorrelator (Natural engine).
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    The Natural engine decorrelates by rotating the phase of each frequency bin
    while leaving its magnitude untouched. That is an all-pass operation, so the
    decorrelated signal has exactly the same spectrum as the input: no timbre
    change, no comb filtering, no chorusing. Only the phase relationship - and
    therefore the perceived spatial spread - changes.

    Two properties are guaranteed by construction and covered by the tests:

      1. With every rotation depth at zero the processor is a pure delay of
         fftSize samples. Perfect reconstruction, sample for sample.
      2. Analysis and synthesis both use a Hann window with 75 % overlap, whose
         squared overlap-add sum is constant (COLA), so the normalisation is
         exact and no amplitude modulation is introduced.
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

    /** @param order log2 of the frame size: 8 = 256, 9 = 512, 10 = 1024. */
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
        buildRotationTable();

        for (auto& smoother : bandDepth)
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

        for (auto& smoother : bandDepth)
            smoother.snapTo (smoother.value());
    }

    /** Per-band rotation depth, 0 = bypass (pure delay), 1 = full rotation. */
    void setBandDepths (const std::array<float, numBands>& depths) noexcept
    {
        for (int band = 0; band < numBands; ++band)
            bandDepth[(std::size_t) band].setTarget (clamp01 (depths[(std::size_t) band]));
    }

    void setUniformDepth (float depth) noexcept
    {
        for (auto& smoother : bandDepth)
            smoother.setTarget (clamp01 (depth));
    }

    /** Processes one sample. The returned sample is delayed by fftSize. */
    float process (float input) noexcept
    {
        inputFrame[(std::size_t) fill++] = sanitise (input);

        if (fill == fftSize)
            processFrame();

        // A completed frame hands over exactly hopSize samples, and exactly
        // hopSize input samples pass before the next frame completes, so this
        // queue can never underrun or overrun once running.
        const float popped = pendingIndex < hopSize ? pendingOutput[(std::size_t) pendingIndex++] : 0.0f;

        // One extra sample of hold-back makes the total delay exactly fftSize,
        // which is the value reported to the host.
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
        std::array<float, numBands> depth {};
        for (int band = 0; band < numBands; ++band)
            depth[(std::size_t) band] = bandDepth[(std::size_t) band].next();

        for (int bin = 1; bin < numBins - 1; ++bin)
        {
            const int band = binToBand[(std::size_t) bin];
            const float amount = band >= 0 ? depth[(std::size_t) band] : 0.0f;

            if (amount <= 1.0e-5f)
                continue;

            const float angle = amount * rotation[(std::size_t) bin];
            const std::complex<float> rotator (std::cos (angle), std::sin (angle));
            spectrum[(std::size_t) bin] *= rotator;
        }

        fft.inverseReal (spectrum.data(), workFrame.data());

        for (int i = 0; i < fftSize; ++i)
            overlapBuffer[(std::size_t) i] += workFrame[(std::size_t) i] * window[(std::size_t) i] * normalisation;

        // The first hop of the overlap buffer has received every contribution
        // it will ever get, so it is complete and can be handed out.
        std::copy (overlapBuffer.begin(), overlapBuffer.begin() + hopSize, pendingOutput.begin());
        pendingIndex = 0;

        std::copy (overlapBuffer.begin() + hopSize, overlapBuffer.end(), overlapBuffer.begin());
        std::fill (overlapBuffer.end() - hopSize, overlapBuffer.end(), 0.0f);

        std::copy (inputFrame.begin() + hopSize, inputFrame.end(), inputFrame.begin());
        fill = fftSize - hopSize;
    }

    void buildBinTable()
    {
        binToBand.assign ((std::size_t) numBins, -1);
        const float binHz = (float) (sampleRate / (double) fftSize);

        for (int bin = 0; bin < numBins; ++bin)
        {
            const float hz = (float) bin * binHz;
            if (hz < 60.0f)
                continue;

            const int band = (int) std::floor (std::log2 (hz / 60.0f) / 0.75f);
            binToBand[(std::size_t) bin] = band < numBands ? band : numBands - 1;
        }
    }

    void buildRotationTable()
    {
        // Deterministic per-bin phase offsets. Neighbouring bins must not be
        // fully independent, otherwise transients smear, so the angle is drawn
        // once per small bin group and dithered inside the group.
        rotation.assign ((std::size_t) numBins, 0.0f);
        DeterministicRandomState state { 0x5bd1e995u };

        constexpr int groupSize = 3;
        float groupAngle = 0.0f;

        for (int bin = 0; bin < numBins; ++bin)
        {
            if (bin % groupSize == 0)
                groupAngle = (nextFloat (state) * 2.0f - 1.0f) * (float) kPi;

            const float dither = (nextFloat (state) * 2.0f - 1.0f) * 0.25f;
            rotation[(std::size_t) bin] = groupAngle + dither;
        }

        rotation[0] = 0.0f;
        rotation[(std::size_t) (numBins - 1)] = 0.0f;
    }

    struct DeterministicRandomState { std::uint32_t value; };

    static float nextFloat (DeterministicRandomState& state) noexcept
    {
        state.value ^= state.value << 13;
        state.value ^= state.value >> 17;
        state.value ^= state.value << 5;
        return (float) (state.value >> 8) * (1.0f / 16777216.0f);
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
    std::vector<float> rotation;
    std::vector<int> binToBand;

    std::vector<float> pendingOutput;
    int pendingIndex = 0;
    float holdback = 0.0f;
    int fill = 0;

    std::array<Smoother, numBands> bandDepth {};
};

} // namespace wp
