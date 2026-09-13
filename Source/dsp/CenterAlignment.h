/*
    Wide Pocket - centre alignment (always on, no user control).
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    The Natural and Smart engines build the Side in exact quadrature to the
    Mid, so their image is centred by construction and this stage has nothing
    to do. Two things can still introduce a small Mid-correlated component:

      - the Efficient engine, whose sparse FIR is not quadrature,
      - any minimum-phase filter in the Side path.

    This stage is the safety net for those cases. It removes the part of the
    Side that is correlated with the Mid:

        S' = S - (E[M * S] / E[M * M]) * M

    The important detail - and the reason the old broadband "Center Lock"
    made the voice wander instead of holding it - is that the correlated part
    is *frequency dependent*. One broadband coefficient cannot cancel a bias
    that sits mostly above 4 kHz, so it over-corrected the body and
    under-corrected the sibilants: the image moved right on "s" sounds and
    left on vowels. Here the projection is done independently per band, using
    the same complementary filter bank for Mid and Side, so within a band both
    signals share the same phase response and the subtraction is exact.

    The coefficients move slowly (a few hundred milliseconds) because a fast
    coefficient *is* image movement. And because only the Side is touched, the
    mono sum (2 * Mid) is still untouched, bit for bit.
*/

#pragma once

#include "Filters.h"
#include "WidePocketTypes.h"

#include <array>

namespace wp
{

class CenterAlignment
{
public:
    static constexpr int numBands = 6;
    static constexpr int numSplits = numBands - 1;

    void prepare (double sampleRate, float windowMs = 250.0f)
    {
        const double tau = std::max (1.0, (double) windowMs) * 0.001;
        averaging = (float) (1.0 - std::exp (-1.0 / (std::max (1.0, sampleRate) * tau)));

        static constexpr std::array<float, numSplits> cutoffs { 150.0f, 500.0f, 1500.0f, 4000.0f, 9000.0f };

        for (int split = 0; split < numSplits; ++split)
        {
            const float hz = std::min (cutoffs[(std::size_t) split], (float) (sampleRate * 0.45));
            midSplit[(std::size_t) split].prepare (sampleRate);
            sideSplit[(std::size_t) split].prepare (sampleRate);
            midSplit[(std::size_t) split].setCutoff (hz);
            sideSplit[(std::size_t) split].setCutoff (hz);
        }

        for (auto& smoother : coefficient)
            smoother.reset (sampleRate, 200.0f, 0.0f);

        reset();
    }

    void reset() noexcept
    {
        for (int split = 0; split < numSplits; ++split)
        {
            midSplit[(std::size_t) split].reset();
            sideSplit[(std::size_t) split].reset();
        }

        for (auto& smoother : coefficient)
            smoother.snapTo (0.0f);

        crossSum.fill (0.0f);
        midPower.fill (0.0f);
    }

    /** Removes the Mid-correlated part of `side`, band by band. */
    void process (float mid, float& side) noexcept
    {
        std::array<float, numBands> midBand {};
        std::array<float, numBands> sideBand {};

        decompose (midSplit, mid, midBand);
        decompose (sideSplit, side, sideBand);

        float correction = 0.0f;
        float worst = 0.0f;

        for (int band = 0; band < numBands; ++band)
        {
            const auto index = (std::size_t) band;
            const float m = midBand[index];
            const float s = sideBand[index];

            crossSum[index] += averaging * (m * s - crossSum[index]);
            midPower[index] += averaging * (m * m - midPower[index]);

            const float target = midPower[index] > 1.0e-10f
                               ? clampf (crossSum[index] / midPower[index], -1.5f, 1.5f)
                               : 0.0f;

            coefficient[index].setTarget (target);
            const float value = coefficient[index].next();
            correction += value * m;
            worst = std::max (worst, std::abs (value));
        }

        largest = worst;
        side = sanitise (side - correction);
    }

    /** Largest projection coefficient in use. Near zero means "nothing to fix". */
    float getLargestCoefficient() const noexcept { return largest; }

private:
    static void decompose (std::array<ComplementaryCrossover, numSplits>& bank,
                           float input,
                           std::array<float, numBands>& bands) noexcept
    {
        float remaining = input;

        for (int split = 0; split < numSplits; ++split)
        {
            float low = 0.0f, high = 0.0f;
            bank[(std::size_t) split].process (remaining, low, high);
            bands[(std::size_t) split] = low;
            remaining = high;
        }

        bands[(std::size_t) (numBands - 1)] = remaining;
    }

    float averaging = 0.001f;
    float largest = 0.0f;

    std::array<ComplementaryCrossover, numSplits> midSplit {};
    std::array<ComplementaryCrossover, numSplits> sideSplit {};
    std::array<Smoother, numBands> coefficient {};
    std::array<float, numBands> crossSum {};
    std::array<float, numBands> midPower {};
};

} // namespace wp
