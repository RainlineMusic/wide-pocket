/*
    Wide Pocket - integer delay line.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    Used to align the dry Mid path with the latency of the active engine, which
    is what keeps the plugin phase coherent and lets Width = 0 be a true null.
*/

#pragma once

#include "WidePocketTypes.h"

#include <vector>

namespace wp
{

class DelayLine
{
public:
    void prepare (int maxDelaySamples)
    {
        int capacity = 1;
        while (capacity < maxDelaySamples + 2)
            capacity <<= 1;

        buffer.assign ((std::size_t) capacity, 0.0f);
        mask = capacity - 1;
        writeIndex = 0;
        delay = std::min (delay, maxDelaySamples);
    }

    void reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    void setDelay (int samples) noexcept
    {
        delay = std::max (0, std::min (samples, (int) buffer.size() - 1));
    }

    int getDelay() const noexcept { return delay; }

    float process (float input) noexcept
    {
        buffer[(std::size_t) writeIndex] = sanitise (input);
        const float output = buffer[(std::size_t) ((writeIndex - delay) & mask)];
        writeIndex = (writeIndex + 1) & mask;
        return output;
    }

private:
    std::vector<float> buffer { 0.0f, 0.0f };
    int mask = 1;
    int writeIndex = 0;
    int delay = 0;
};

} // namespace wp
