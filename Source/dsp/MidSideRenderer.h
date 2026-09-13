/*
    Wide Pocket - Mid/Side renderer.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    All widening happens in the M/S domain:

        L = gM * M + gS * S
        R = gM * M - gS * S

    The mono sum is 2 * gM * M and therefore completely independent of the
    synthesised Side signal, which is what makes the widening mono safe by
    construction rather than by correction.
*/

#pragma once

#include "WidePocketTypes.h"

namespace wp
{

struct MidSide
{
    float mid = 0.0f;
    float side = 0.0f;
};

struct StereoSample
{
    float left = 0.0f;
    float right = 0.0f;
};

inline MidSide encodeMidSide (float left, float right) noexcept
{
    return { 0.5f * (left + right), 0.5f * (left - right) };
}

inline StereoSample decodeMidSide (float mid, float side, float midGain, float sideGain) noexcept
{
    const float m = midGain * mid;
    const float s = sideGain * side;
    return { m + s, m - s };
}

/**
    Renders M/S back to L/R with smoothed gains.

    The Mid gain stays at unity unless Auto Gain asks for loudness
    compensation, so at width 0 the output is the latency aligned dry signal
    sample for sample.
*/
class MidSideRenderer
{
public:
    void prepare (double sampleRate, float smoothingMs = 15.0f)
    {
        midGain.reset (sampleRate, smoothingMs, 1.0f);
        sideGain.reset (sampleRate, smoothingMs, 0.0f);
    }

    void reset (float midValue = 1.0f, float sideValue = 0.0f)
    {
        midGain.snapTo (midValue);
        sideGain.snapTo (sideValue);
    }

    void setTargets (float mid, float side) noexcept
    {
        midGain.setTarget (mid);
        sideGain.setTarget (side);
    }

    StereoSample render (float mid, float side) noexcept
    {
        const float m = midGain.next();
        const float s = sideGain.next();
        auto out = decodeMidSide (mid, side, m, s);
        out.left = sanitise (out.left);
        out.right = sanitise (out.right);
        return out;
    }

    float currentMidGain() const noexcept { return midGain.value(); }
    float currentSideGain() const noexcept { return sideGain.value(); }

private:
    Smoother midGain;
    Smoother sideGain;
};

} // namespace wp
