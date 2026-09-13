/*
    Wide Pocket - tiny test harness.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    Dependency free on purpose: the DSP core must be testable with nothing but
    a C++17 compiler, both locally and in CI. Every failure prints the
    expected and the actual value, and the process exits non-zero.
*/

#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wptest
{

struct Results
{
    int checks = 0;
    int failures = 0;
    std::string currentCase;
};

inline Results& results()
{
    static Results instance;
    return instance;
}

inline void beginCase (const std::string& name)
{
    results().currentCase = name;
    std::printf ("\n[ case ] %s\n", name.c_str());
}

inline void fail (const char* what, const std::string& detail)
{
    ++results().failures;
    std::printf ("  FAIL  %s\n        %s\n", what, detail.c_str());
}

inline void pass (const char* what)
{
    std::printf ("  ok    %s\n", what);
}

inline void checkTrue (bool condition, const char* what, const std::string& detail = {})
{
    ++results().checks;
    if (condition)
        pass (what);
    else
        fail (what, detail.empty() ? "expected: true, actual: false" : detail);
}

inline void checkNear (double actual, double expected, double tolerance, const char* what)
{
    ++results().checks;
    const double difference = std::abs (actual - expected);

    if (std::isfinite (actual) && difference <= tolerance)
    {
        std::printf ("  ok    %s (expected %.9g, actual %.9g, tol %.3g)\n", what, expected, actual, tolerance);
        return;
    }

    char buffer[256];
    std::snprintf (buffer, sizeof (buffer),
                   "expected: %.9g +- %.3g, actual: %.9g, difference: %.6g",
                   expected, tolerance, actual, difference);
    fail (what, buffer);
}

inline void checkLess (double actual, double limit, const char* what)
{
    ++results().checks;
    if (std::isfinite (actual) && actual < limit)
    {
        std::printf ("  ok    %s (actual %.9g < %.9g)\n", what, actual, limit);
        return;
    }

    char buffer[256];
    std::snprintf (buffer, sizeof (buffer), "expected: < %.9g, actual: %.9g", limit, actual);
    fail (what, buffer);
}

inline void checkGreater (double actual, double limit, const char* what)
{
    ++results().checks;
    if (std::isfinite (actual) && actual > limit)
    {
        std::printf ("  ok    %s (actual %.9g > %.9g)\n", what, actual, limit);
        return;
    }

    char buffer[256];
    std::snprintf (buffer, sizeof (buffer), "expected: > %.9g, actual: %.9g", limit, actual);
    fail (what, buffer);
}

inline int summary()
{
    std::printf ("\n================================\n");
    std::printf ("checks: %d, failures: %d\n", results().checks, results().failures);
    std::printf ("%s\n", results().failures == 0 ? "RESULT: PASS" : "RESULT: FAIL");
    return results().failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Signal helpers
// ---------------------------------------------------------------------------

inline double rms (const std::vector<float>& data, std::size_t from = 0)
{
    if (from >= data.size())
        return 0.0;

    double sum = 0.0;
    for (std::size_t i = from; i < data.size(); ++i)
        sum += (double) data[i] * (double) data[i];

    return std::sqrt (sum / (double) (data.size() - from));
}

inline double maxAbsDifference (const std::vector<float>& a, const std::vector<float>& b,
                                std::size_t from, std::size_t count)
{
    double worst = 0.0;
    for (std::size_t i = from; i < from + count && i < a.size() && i < b.size(); ++i)
        worst = std::max (worst, (double) std::abs (a[i] - b[i]));
    return worst;
}

inline double correlation (const std::vector<float>& a, const std::vector<float>& b, std::size_t from = 0)
{
    double cross = 0.0, powerA = 0.0, powerB = 0.0;
    for (std::size_t i = from; i < a.size() && i < b.size(); ++i)
    {
        cross += (double) a[i] * (double) b[i];
        powerA += (double) a[i] * (double) a[i];
        powerB += (double) b[i] * (double) b[i];
    }
    return cross / std::sqrt (std::max (1.0e-20, powerA * powerB));
}

inline bool allFinite (const std::vector<float>& data)
{
    for (float value : data)
        if (! std::isfinite (value))
            return false;
    return true;
}

inline double peakAbs (const std::vector<float>& data)
{
    double peak = 0.0;
    for (float value : data)
        peak = std::max (peak, (double) std::abs (value));
    return peak;
}

/** Deterministic pseudo random noise, so test runs are reproducible. */
class Noise
{
public:
    explicit Noise (unsigned int seed = 12345u) : state (seed | 1u) {}

    float next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float) ((double) (state >> 8) / 16777216.0 * 2.0 - 1.0);
    }

private:
    unsigned int state;
};

} // namespace wptest
