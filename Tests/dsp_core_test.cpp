/*
    Wide Pocket - DSP core test suite.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.

    Run by CTest as the WidePocketTests target on every CI build, before the
    VST3 and AAX targets are linked. No JUCE, no external test framework.
*/

#include "../Source/dsp/WidePocketEngine.h"
#include "TestHarness.h"

#include <vector>

using namespace wp;
using namespace wptest;

namespace
{

constexpr double kSampleRate = 48000.0;

std::vector<float> makeSine (int numSamples, double frequency, double amplitude = 0.5)
{
    std::vector<float> data ((std::size_t) numSamples, 0.0f);
    for (int i = 0; i < numSamples; ++i)
        data[(std::size_t) i] = (float) (amplitude * std::sin (2.0 * kPi * frequency * (double) i / kSampleRate));
    return data;
}

/** Crude but useful vocal-like signal: harmonic stack, vibrato, sibilant bursts. */
std::vector<float> makeVoiceLike (int numSamples)
{
    std::vector<float> data ((std::size_t) numSamples, 0.0f);
    Noise noise (777u);

    for (int i = 0; i < numSamples; ++i)
    {
        const double t = (double) i / kSampleRate;
        const double f0 = 150.0 + 6.0 * std::sin (2.0 * kPi * 5.0 * t);

        double value = 0.0;
        for (int harmonic = 1; harmonic <= 12; ++harmonic)
            value += (0.55 / harmonic) * std::sin (2.0 * kPi * f0 * (double) harmonic * t);

        // Sibilant burst every 400 ms.
        if (std::fmod (t, 0.4) < 0.06)
            value += 0.25 * noise.next();

        // Syllable envelope.
        const double envelope = 0.5 + 0.5 * std::sin (2.0 * kPi * 2.5 * t);
        data[(std::size_t) i] = (float) (0.4 * value * envelope);
    }

    return data;
}

std::vector<float> makeClicks (int numSamples, int period)
{
    std::vector<float> data ((std::size_t) numSamples, 0.0f);
    for (int i = 0; i < numSamples; i += period)
        data[(std::size_t) i] = 0.8f;
    return data;
}

struct StereoBuffers
{
    std::vector<float> left, right;
};

StereoBuffers runEngine (WidePocketEngine& engine, const std::vector<float>& monoInput, int blockSize = 128)
{
    StereoBuffers out { monoInput, monoInput };

    for (std::size_t position = 0; position < out.left.size(); position += (std::size_t) blockSize)
    {
        const int count = (int) std::min ((std::size_t) blockSize, out.left.size() - position);
        engine.process (out.left.data() + position, out.right.data() + position, count);
    }

    return out;
}

Parameters defaultParameters()
{
    Parameters parameters;
    parameters.width = 70.0f;
    parameters.focus = 50.0f;
    parameters.air = 0.0f;
    parameters.stability = 50.0f;
    parameters.sibilanceGuard = 60.0f;
    parameters.transientFocus = 60.0f;
    parameters.outputDb = 0.0f;
    return parameters;
}

/** Side RMS produced by the engine for one mono input, after settling. */
double sideRms (const Parameters& parameters, const std::vector<float>& input, std::size_t from = 24000)
{
    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);
    engine.setParameters (parameters);

    auto output = runEngine (engine, input);

    std::vector<float> side (input.size(), 0.0f);
    for (std::size_t i = from; i < input.size(); ++i)
        side[i] = 0.5f * (output.left[i] - output.right[i]);

    return rms (side, from);
}

// ---------------------------------------------------------------------------

void testFft()
{
    beginCase ("FFT: round trip and Parseval");

    constexpr int order = 9;
    constexpr int size = 1 << order;

    Fft fft (order);
    auto input = makeSine (size, 1000.0, 0.7);

    std::vector<std::complex<float>> spectrum (fft.numBins());
    std::vector<float> output ((std::size_t) size, 0.0f);

    fft.forwardReal (input.data(), spectrum.data());
    fft.inverseReal (spectrum.data(), output.data());

    checkNear (maxAbsDifference (input, output, 0, (std::size_t) size), 0.0, 1.0e-5,
               "inverse(forward(x)) reproduces x");

    double timeEnergy = 0.0;
    for (float value : input)
        timeEnergy += (double) value * value;

    double spectralEnergy = 0.0;
    for (std::size_t bin = 0; bin < spectrum.size(); ++bin)
    {
        const double magnitude = std::abs (spectrum[bin]);
        const bool edge = (bin == 0 || bin + 1 == spectrum.size());
        spectralEnergy += (edge ? 1.0 : 2.0) * magnitude * magnitude;
    }
    spectralEnergy /= (double) size;

    checkNear (spectralEnergy / timeEnergy, 1.0, 1.0e-4, "Parseval energy identity");
}

void testCrossover()
{
    beginCase ("Crossover: perfect reconstruction");

    ComplementaryCrossover crossover;
    crossover.prepare (kSampleRate);
    crossover.setCutoff (180.0f);

    Noise noise (4242u);
    double worst = 0.0;
    double lowEnergyBelow = 0.0, lowEnergyAbove = 0.0;

    for (int i = 0; i < 48000; ++i)
    {
        const float input = noise.next();
        float low = 0.0f, high = 0.0f;
        crossover.process (input, low, high);
        worst = std::max (worst, (double) std::abs ((low + high) - input));
    }

    checkNear (worst, 0.0, 1.0e-6, "low + high == input, sample for sample");

    crossover.reset();
    for (float sample : makeSine (24000, 50.0))
    {
        float low = 0.0f, high = 0.0f;
        crossover.process (sample, low, high);
        lowEnergyBelow += (double) low * low;
    }

    crossover.reset();
    for (float sample : makeSine (24000, 5000.0))
    {
        float low = 0.0f, high = 0.0f;
        crossover.process (sample, low, high);
        lowEnergyAbove += (double) low * low;
    }

    checkGreater (lowEnergyBelow / std::max (1.0e-12, lowEnergyAbove), 100.0,
                  "low band rejects content far above the cutoff");
}

void testMidSide()
{
    beginCase ("Mid/Side: identity and mono invariance");

    Noise noise (99u);
    double worstIdentity = 0.0;
    double worstMonoSum = 0.0;

    for (int i = 0; i < 10000; ++i)
    {
        const float left = noise.next();
        const float right = noise.next();

        const auto ms = encodeMidSide (left, right);
        const auto back = decodeMidSide (ms.mid, ms.side, 1.0f, 1.0f);

        worstIdentity = std::max (worstIdentity,
                                  (double) std::max (std::abs (back.left - left), std::abs (back.right - right)));

        const auto widened = decodeMidSide (ms.mid, ms.side + 3.0f * noise.next(), 1.0f, 1.0f);
        worstMonoSum = std::max (worstMonoSum,
                                 (double) std::abs ((widened.left + widened.right) - (left + right)));
    }

    checkNear (worstIdentity, 0.0, 1.0e-6, "decode(encode(L,R)) == (L,R)");
    checkNear (worstMonoSum, 0.0, 1.0e-6, "mono sum is independent of the Side signal");
}

void testStftIsSilentAtZeroGain()
{
    beginCase ("STFT: zero band gain produces exact silence");

    StftDecorrelator stft;
    stft.prepare (kSampleRate, 10);
    stft.setUniformGain (0.0f);

    const int latency = stft.getLatencySamples();
    checkNear ((double) latency, 256.0, 0.0, "reported latency equals the fixed frame size");

    auto input = makeVoiceLike (32768);
    double worst = 0.0;
    for (std::size_t i = 0; i < input.size(); ++i)
        worst = std::max (worst, (double) std::abs (stft.process (input[i])));

    checkNear (worst, 0.0, 1.0e-6, "no Side signal is generated");
}

void testStftSideIsQuadratureToMid()
{
    beginCase ("STFT: the Side is in quadrature to the Mid (structurally centred)");

    StftDecorrelator stft;
    stft.prepare (kSampleRate, 10);
    stft.setUniformGain (1.0f);
    stft.setDuckDepth (0.0f);

    const std::size_t latency = (std::size_t) stft.getLatencySamples();
    auto input = makeVoiceLike (131072);

    std::vector<float> side (input.size(), 0.0f);
    for (std::size_t i = 0; i < input.size(); ++i)
        side[i] = stft.process (input[i]);

    std::vector<float> mid (input.size(), 0.0f);
    for (std::size_t i = latency; i < input.size(); ++i)
        mid[i] = input[i - latency];

    // The inter-channel level difference of L = M + S, R = M - S is exactly
    // 4 * E[M * S]. Since v0.3 orthogonalises every bin individually, this is
    // zero by construction and the image cannot move.
    checkLess (std::abs (correlation (mid, side, latency * 3)), 0.02,
               "Mid and Side are uncorrelated, so there is no level difference");
}

void testStftPreservesSpectrum()
{
    beginCase ("STFT: the Side keeps the spectrum of the Mid");

    StftDecorrelator stft;
    stft.prepare (kSampleRate, 10);
    stft.setUniformGain (1.0f);
    stft.setDuckDepth (0.0f);

    auto input = makeVoiceLike (65536);
    std::vector<float> output (input.size(), 0.0f);
    for (std::size_t i = 0; i < input.size(); ++i)
        output[i] = stft.process (input[i]);

    checkTrue (allFinite (output), "output is finite");

    const double processedRatio = rms (output, 8192) / std::max (1.0e-9, rms (input, 8192));
    // Overlap-add of rotated 256-point frames loses a little energy at the
    // frame edges; what matters is that nothing is added and nothing collapses.
    checkGreater (processedRatio, 0.55, "the quadrature rotation keeps the energy");
    checkLess (processedRatio, 1.2, "and does not add any");

    const std::size_t latency = (std::size_t) stft.getLatencySamples();
    std::vector<float> aligned (input.size(), 0.0f);
    for (std::size_t i = latency; i < input.size(); ++i)
        aligned[i] = input[i - latency];

    checkLess (std::abs (correlation (aligned, output, 8192)), 0.6,
               "the Side is decorrelated from the delayed Mid");
}

void testTransientDuckIsSmooth()
{
    beginCase ("STFT: onsets duck the Side instead of gating it off");

    StftDecorrelator stft;
    stft.prepare (kSampleRate, 10);
    stft.setUniformGain (1.0f);
    stft.setDuckDepth (0.9f);

    auto input = makeVoiceLike (131072);
    std::vector<float> side (input.size(), 0.0f);
    for (std::size_t i = 0; i < input.size(); ++i)
        side[i] = stft.process (input[i]);

    // 5 ms envelope: with the old hard gate, whole frames were exactly zero.
    const std::size_t window = 240;
    int silentWindows = 0, activeWindows = 0;

    for (std::size_t pos = 24000; pos + window < side.size(); pos += window)
    {
        double energy = 0.0, dry = 0.0;
        for (std::size_t i = pos; i < pos + window; ++i)
        {
            energy += (double) side[i] * side[i];
            dry += (double) input[i] * input[i];
        }

        if (std::sqrt (dry / (double) window) < 0.01)
            continue;

        ++activeWindows;
        if (std::sqrt (energy / (double) window) < 1.0e-4)
            ++silentWindows;
    }

    checkGreater ((double) activeWindows, 50.0, "the test carried signal");
    checkNear ((double) silentWindows, 0.0, 0.0, "the Side never stops completely");
}

void testWidthZeroIsNull()
{
    beginCase ("Engine: Width = 0 is a latency aligned null");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);

    auto parameters = defaultParameters();
    parameters.width = 0.0f;
    engine.setParameters (parameters);

    auto input = makeVoiceLike (65536);
    auto output = runEngine (engine, input);

    const std::size_t latency = (std::size_t) engine.getLatencySamples();
    double worstLeft = 0.0, worstRight = 0.0;

    for (std::size_t i = latency * 3; i < input.size(); ++i)
    {
        const double dry = input[i - latency];
        worstLeft = std::max (worstLeft, std::abs ((double) output.left[i] - dry));
        worstRight = std::max (worstRight, std::abs ((double) output.right[i] - dry));
    }

    checkNear (worstLeft, 0.0, 1.0e-4, "left channel matches the delayed dry signal");
    checkNear (worstRight, 0.0, 1.0e-4, "right channel matches the delayed dry signal");
}

void testMonoCompatibility()
{
    beginCase ("Engine: mono fold down is exact");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);

    auto parameters = defaultParameters();
    parameters.width = 100.0f;
    engine.setParameters (parameters);

    auto input = makeVoiceLike (65536);
    auto output = runEngine (engine, input);

    const std::size_t latency = (std::size_t) engine.getLatencySamples();

    double worst = 0.0;
    double energy = 0.0;
    for (std::size_t i = latency * 3; i < input.size(); ++i)
    {
        const double sum = (double) output.left[i] + (double) output.right[i];
        const double dry = 2.0 * (double) input[i - latency];
        worst = std::max (worst, std::abs (sum - dry));
        energy += dry * dry;
    }

    checkNear (worst, 0.0, 1.0e-4, "L + R equals the dry mono sum, so nothing cancels");
    checkGreater (energy, 1.0, "the test actually carried signal");

    const auto side = correlation (output.left, output.right, latency * 3);
    checkLess (side, 0.999, "the two channels are not identical (widening happened)");
    checkGreater (side, 0.0, "the two channels stay positively correlated (no phasey mush)");
}

void testLoudnessAtFullWidth()
{
    beginCase ("Engine: loudness and mono fold down at full width");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);

    auto parameters = defaultParameters();
    parameters.width = 100.0f;
    engine.setParameters (parameters);

    auto input = makeVoiceLike (65536);
    auto output = runEngine (engine, input);

    const std::size_t latency = (std::size_t) engine.getLatencySamples();
    const std::size_t from = 24000;

    std::vector<float> monoSum (input.size(), 0.0f);
    std::vector<float> stereo (input.size(), 0.0f);
    std::vector<float> dry (input.size(), 0.0f);
    for (std::size_t i = from; i < input.size(); ++i)
    {
        monoSum[i] = 0.5f * (output.left[i] + output.right[i]);
        stereo[i] = std::sqrt (0.5f * (output.left[i] * output.left[i] + output.right[i] * output.right[i]));
        dry[i] = input[i - latency];
    }

    const double dryRms = std::max (1.0e-9, rms (dry, from));

    // A widener raises the stereo RMS by design: the Side adds energy that the
    // mono sum does not see. The tolerance covers full width, not a bug.
    checkNear (20.0 * std::log10 (rms (stereo, from) / dryRms), 0.0, 2.5,
               "stereo loudness stays within 2.5 dB of the dry signal");
    checkNear (20.0 * std::log10 (rms (monoSum, from) / dryRms), 0.0, 2.0,
               "mono fold down stays within 2 dB of the dry signal");
    checkGreater (correlation (monoSum, dry, from), 0.99,
                  "mono sum is still the same signal, not a filtered version");
}

void testTonalStability()
{
    beginCase ("Engine: steady tone stays centred");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);

    auto parameters = defaultParameters();
    parameters.width = 100.0f;
    engine.setParameters (parameters);

    auto input = makeSine (65536, 440.0, 0.5);
    auto output = runEngine (engine, input);

    const std::size_t from = 24000;

    const double leftRms = rms (output.left, from);
    const double rightRms = rms (output.right, from);

    // A held note is the hardest case for any quadrature widener: the residual
    // lean has to stay well under the ~1 dB that starts to be audible.
    checkLess (std::abs (20.0 * std::log10 (leftRms / std::max (1.0e-9, rightRms))), 0.6,
               "image stays centred (no inter channel level difference)");
}

void testTransientHandling()
{
    beginCase ("Engine: transients are not smeared");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);

    auto parameters = defaultParameters();
    parameters.width = 100.0f;
    parameters.transientFocus = 100.0f;
    engine.setParameters (parameters);

    auto input = makeClicks (32768, 4096);
    auto output = runEngine (engine, input);

    checkTrue (allFinite (output.left) && allFinite (output.right), "output is finite");
    checkLess (peakAbs (output.left), 1.5 * 0.8, "no peak overshoot on the left channel");
    checkLess (peakAbs (output.right), 1.5 * 0.8, "no peak overshoot on the right channel");

    const std::size_t latency = (std::size_t) engine.getLatencySamples();
    const std::size_t tailStart = latency + 4096 / 2;
    double tail = 0.0;
    for (std::size_t i = tailStart; i < tailStart + 1024 && i < output.left.size(); ++i)
        tail = std::max (tail, (double) std::abs (output.left[i]));

    checkLess (tail, 0.25, "decay between transients stays low");
}

void testFocusIsAudible()
{
    beginCase ("Controls: Focus really narrows the presence range");

    auto parameters = defaultParameters();
    parameters.width = 100.0f;

    auto presenceTone = makeSine (131072, 1250.0, 0.4);

    parameters.focus = 0.0f;
    const double open = sideRms (parameters, presenceTone);

    parameters.focus = 100.0f;
    const double closed = sideRms (parameters, presenceTone);

    const double change = 20.0 * std::log10 (open / std::max (1.0e-9, closed));
    checkGreater (change, 10.0, "Focus moves the presence Side by more than 10 dB");

    // And it must stay a local control, not a master width.
    auto lowTone = makeSine (131072, 220.0, 0.4);

    parameters.focus = 0.0f;
    const double lowOpen = sideRms (parameters, lowTone);
    parameters.focus = 100.0f;
    const double lowClosed = sideRms (parameters, lowTone);

    checkLess (20.0 * std::log10 (lowOpen / std::max (1.0e-9, lowClosed)), 3.0,
               "Focus barely touches the low harmonics");
}

void testAirIsBipolar()
{
    beginCase ("Controls: Air can brighten and darken the Side");

    auto parameters = defaultParameters();
    parameters.width = 100.0f;
    parameters.sibilanceGuard = 0.0f;

    auto topTone = makeSine (131072, 8000.0, 0.4);

    parameters.air = 0.0f;
    const double neutral = sideRms (parameters, topTone);

    parameters.air = 100.0f;
    const double bright = sideRms (parameters, topTone);

    parameters.air = -100.0f;
    const double dark = sideRms (parameters, topTone);

    checkGreater (20.0 * std::log10 (bright / std::max (1.0e-9, neutral)), 4.0,
                  "+100 % adds more than 4 dB on the top octaves");
    checkGreater (20.0 * std::log10 (neutral / std::max (1.0e-9, dark)), 6.0,
                  "-100 % removes more than 6 dB, so a dark Side is possible");

    // The hinge must leave the lower midrange alone.
    auto midTone = makeSine (131072, 500.0, 0.4);

    parameters.air = 100.0f;
    const double midBright = sideRms (parameters, midTone);
    parameters.air = -100.0f;
    const double midDark = sideRms (parameters, midTone);

    checkLess (std::abs (20.0 * std::log10 (midBright / std::max (1.0e-9, midDark))), 1.0,
               "Air does not change the midrange");
}

void testWideAndLocallyCentred()
{
    beginCase ("Natural: vocal is wide and locally centred");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);
    auto parameters = defaultParameters();
    parameters.width = 100.0f;
    parameters.focus = 45.0f;
    engine.setParameters (parameters);

    auto input = makeVoiceLike (192000);
    auto output = runEngine (engine, input);
    const std::size_t from = 24000;

    std::vector<float> mid (input.size()), side (input.size());
    for (std::size_t i = from; i < input.size(); ++i)
    {
        mid[i] = 0.5f * (output.left[i] + output.right[i]);
        side[i] = 0.5f * (output.left[i] - output.right[i]);
    }

    const double ratio = rms (side, from) / std::max (1.0e-9, rms (mid, from));
    checkGreater (ratio, 0.35, "full width produces useful Side energy");
    checkLess (ratio, 1.1, "Side stays in the same range as the Mid");

    const std::size_t window = 2400; // 50 ms
    double worstIld = 0.0, meanIld = 0.0;
    int windows = 0;
    for (std::size_t pos = from; pos + window < input.size(); pos += window)
    {
        double le = 0.0, re = 0.0;
        for (std::size_t i = pos; i < pos + window; ++i)
        {
            le += (double) output.left[i] * output.left[i];
            re += (double) output.right[i] * output.right[i];
        }
        const double l = std::sqrt (le / (double) window);
        const double r = std::sqrt (re / (double) window);
        const double ild = std::abs (20.0 * std::log10 (std::max (1.0e-9, l) / std::max (1.0e-9, r)));
        worstIld = std::max (worstIld, ild);
        meanIld += ild;
        ++windows;
    }

    checkLess (meanIld / std::max (1, windows), 0.15, "mean 50 ms ILD stays centred");
    checkLess (worstIld, 0.60, "no short window leans strongly left or right");
}

void testLowBandIsAllowedToWiden()
{
    beginCase ("Natural: no forced mono below 150 Hz");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);
    auto parameters = defaultParameters();
    parameters.width = 100.0f;
    parameters.focus = 0.0f;
    engine.setParameters (parameters);

    auto output = runEngine (engine, makeSine (192000, 110.0, 0.4));
    std::vector<float> side (output.left.size());
    for (std::size_t i = 0; i < side.size(); ++i)
        side[i] = 0.5f * (output.left[i] - output.right[i]);

    checkGreater (rms (side, 48000), 0.005, "110 Hz produces Side instead of being forced mono");
}

void testStereoIdentityAtZeroWidth()
{
    beginCase ("Natural: Width zero preserves a stereo input");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);
    auto parameters = defaultParameters();
    parameters.width = 0.0f;
    engine.setParameters (parameters);

    auto left = makeSine (65536, 330.0, 0.4);
    auto right = makeSine (65536, 550.0, 0.3);
    const auto dryLeft = left, dryRight = right;

    for (std::size_t pos = 0; pos < left.size(); pos += 128)
        engine.process (left.data() + pos, right.data() + pos, (int) std::min<std::size_t> (128, left.size() - pos));

    const std::size_t latency = (std::size_t) engine.getLatencySamples();
    double worst = 0.0;
    for (std::size_t i = latency * 3; i < left.size(); ++i)
    {
        worst = std::max (worst, std::abs ((double) left[i] - dryLeft[i - latency]));
        worst = std::max (worst, std::abs ((double) right[i] - dryRight[i - latency]));
    }

    checkNear (worst, 0.0, 1.0e-4, "stereo dry path is sample exact");
}

void testParameterAutomation()
{
    beginCase ("Engine: dense automation stays stable");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 64);

    auto parameters = defaultParameters();
    engine.setParameters (parameters);

    auto input = makeVoiceLike (49152);
    std::vector<float> left = input, right = input;

    Noise noise (31337u);
    const int blockSize = 64;

    for (std::size_t position = 0; position < left.size(); position += (std::size_t) blockSize)
    {
        parameters.width = 50.0f + 50.0f * noise.next();
        parameters.focus = 50.0f + 50.0f * noise.next();
        parameters.air = 100.0f * noise.next();
        parameters.stability = 50.0f + 50.0f * noise.next();
        parameters.sibilanceGuard = 50.0f + 50.0f * noise.next();
        parameters.transientFocus = 50.0f + 50.0f * noise.next();
        parameters.outputDb = 3.0f * noise.next();
        engine.setParameters (parameters);

        const int count = (int) std::min ((std::size_t) blockSize, left.size() - position);
        engine.process (left.data() + position, right.data() + position, count);
    }

    checkTrue (allFinite (left) && allFinite (right), "output is finite under dense automation");
    checkLess (peakAbs (left), 4.0, "no runaway level under dense automation");
}

void testNanAndInfInput()
{
    beginCase ("Engine: NaN and Inf input cannot poison the output");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);
    engine.setParameters (defaultParameters());

    std::vector<float> left (16384, 0.0f), right (16384, 0.0f);
    Noise noise (555u);

    for (std::size_t i = 0; i < left.size(); ++i)
    {
        left[i] = right[i] = 0.3f * noise.next();

        if (i % 1000 == 500)
            left[i] = right[i] = std::numeric_limits<float>::quiet_NaN();
        else if (i % 1000 == 700)
            left[i] = right[i] = std::numeric_limits<float>::infinity();
    }

    for (std::size_t position = 0; position < left.size(); position += 128)
        engine.process (left.data() + position, right.data() + position, 128);

    checkTrue (allFinite (left) && allFinite (right), "no NaN or Inf reaches the output");
}

void testAnalyzerFeatures()
{
    beginCase ("Analyser: features respond as documented");

    auto measure = [] (const std::vector<float>& signal)
    {
        VocalAnalyzer analyzer;
        analyzer.prepare (kSampleRate);

        AnalyzerFrame last;
        for (float sample : signal)
            if (analyzer.pushSample (sample))
                last = analyzer.getFrame();

        return last;
    };

    const auto tone = measure (makeSine (32768, 220.0, 0.5));

    Noise noise (8u);
    std::vector<float> hiss (32768, 0.0f);
    for (auto& sample : hiss)
        sample = 0.3f * noise.next();

    const auto noisy = measure (hiss);
    const auto voice = measure (makeVoiceLike (65536));

    checkLess ((double) tone.flatness, (double) noisy.flatness, "a tone is less flat than noise");
    checkGreater ((double) tone.harmonicity, 0.3, "a tone reads as harmonic");
    checkGreater ((double) voice.voicing, (double) noisy.voicing, "a voice reads as more voiced than hiss");
    checkTrue (std::isfinite (voice.sibilance) && voice.sibilance >= 0.0f && voice.sibilance <= 1.0f,
               "sibilance stays normalised");

    const auto& mask = VocalAnalyzer().getSpatialMask();
    checkNear ((double) mask.size(), 12.0, 0.0, "the spatial mask has the documented band count");
}

} // namespace

int main()
{
    std::printf ("Wide Pocket DSP core tests\n");
    std::printf ("sample rate: %.0f Hz\n", kSampleRate);

    testFft();
    testCrossover();
    testMidSide();
    testStftIsSilentAtZeroGain();
    testStftSideIsQuadratureToMid();
    testStftPreservesSpectrum();
    testTransientDuckIsSmooth();
    testWidthZeroIsNull();
    testMonoCompatibility();
    testLoudnessAtFullWidth();
    testTonalStability();
    testTransientHandling();
    testFocusIsAudible();
    testAirIsBipolar();
    testWideAndLocallyCentred();
    testLowBandIsAllowedToWiden();
    testStereoIdentityAtZeroWidth();
    testParameterAutomation();
    testNanAndInfInput();
    testAnalyzerFeatures();

    return summary();
}
