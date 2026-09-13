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
    parameters.air = 40.0f;
    parameters.stability = 50.0f;
    parameters.sibilanceGuard = 60.0f;
    parameters.transientFocus = 60.0f;
    parameters.outputDb = 0.0f;
    parameters.engine = Engine::natural;
    parameters.quality = Quality::studio;
    return parameters;
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

    // A 50 Hz tone must survive in the low band, a 5 kHz tone must not.
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

        // Whatever we do to the Side signal, the mono sum must not move.
        const auto widened = decodeMidSide (ms.mid, ms.side + 3.0f * noise.next(), 1.0f, 1.0f);
        worstMonoSum = std::max (worstMonoSum,
                                 (double) std::abs ((widened.left + widened.right) - (left + right)));
    }

    checkNear (worstIdentity, 0.0, 1.0e-6, "decode(encode(L,R)) == (L,R)");
    checkNear (worstMonoSum, 0.0, 1.0e-6, "mono sum is independent of the Side signal");
}

void testVelvetNoise()
{
    beginCase ("Velvet noise: energy, decorrelation, stability");

    VelvetNoiseDecorrelator velvet;
    velvet.prepare (kSampleRate, 28.0f, 1800.0f, 0x1f2e3d4cu);

    checkGreater ((double) velvet.getNumTaps(), 20.0, "sparse impulse response has enough taps");
    checkLess ((double) velvet.getNumTaps(), 400.0, "impulse response stays sparse (cheap)");
    checkNear ((double) velvet.impulseEnergy(), 1.0, 1.0e-5, "tap gains are energy normalised");

    Noise noise (2024u);
    std::vector<float> input (48000), output (48000);
    for (std::size_t i = 0; i < input.size(); ++i)
    {
        input[i] = noise.next() * 0.5f;
        output[i] = velvet.process (input[i]);
    }

    checkTrue (allFinite (output), "output is finite");
    checkNear (rms (output, 4800) / std::max (1.0e-9, rms (input, 4800)), 1.0, 0.1,
               "RMS is preserved (energy neutral decorrelation)");
    checkLess (std::abs (correlation (input, output, 4800)), 0.25,
               "output is decorrelated from the input");

    // Determinism: a second instance with the same seed must be identical.
    VelvetNoiseDecorrelator twin;
    twin.prepare (kSampleRate, 28.0f, 1800.0f, 0x1f2e3d4cu);
    double worst = 0.0;
    for (std::size_t i = 0; i < input.size(); ++i)
        worst = std::max (worst, (double) std::abs (twin.process (input[i]) - output[i]));

    checkNear (worst, 0.0, 0.0, "same seed gives bit identical output");
}

void testStftIsSilentAtZeroGain()
{
    beginCase ("STFT: zero band gain produces exact silence");

    StftDecorrelator stft;
    stft.prepare (kSampleRate, 10);
    stft.setUniformGain (0.0f);

    const int latency = stft.getLatencySamples();
    checkNear ((double) latency, 1024.0, 0.0, "reported latency equals the frame size");

    auto input = makeVoiceLike (32768);
    double worst = 0.0;
    for (std::size_t i = 0; i < input.size(); ++i)
        worst = std::max (worst, (double) std::abs (stft.process (input[i])));

    // This is what makes Width = 0 an exact null: no Side is generated at all.
    checkNear (worst, 0.0, 1.0e-6, "no Side signal is generated");
}

void testStftSideIsQuadratureToMid()
{
    beginCase ("STFT: the Side is in quadrature to the Mid (structurally centred)");

    StftDecorrelator stft;
    stft.prepare (kSampleRate, 10);
    stft.setUniformGain (1.0f);

    const std::size_t latency = (std::size_t) stft.getLatencySamples();
    auto input = makeVoiceLike (131072);

    std::vector<float> side (input.size(), 0.0f);
    for (std::size_t i = 0; i < input.size(); ++i)
        side[i] = stft.process (input[i]);

    std::vector<float> mid (input.size(), 0.0f);
    for (std::size_t i = latency; i < input.size(); ++i)
        mid[i] = input[i - latency];

    // The inter-channel level difference of L = M + S, R = M - S is exactly
    // 4 * E[M * S], so a zero Mid/Side correlation means a centred image for
    // every possible band gain. This is the core invariant of the design.
    checkLess (std::abs (correlation (mid, side, latency * 3)), 0.05,
               "Mid and Side are uncorrelated, so there is no level difference");
}

void testStftPreservesSpectrum()
{
    beginCase ("STFT: the Side keeps the spectrum of the Mid");

    StftDecorrelator stft;
    stft.prepare (kSampleRate, 10);
    stft.setUniformGain (1.0f);

    auto input = makeVoiceLike (65536);
    std::vector<float> output (input.size(), 0.0f);
    for (std::size_t i = 0; i < input.size(); ++i)
        output[i] = stft.process (input[i]);

    checkTrue (allFinite (output), "output is finite");

    // Neighbouring frames carry independent sign patterns, so the overlap-add
    // sums incoherently; the fixed 3 dB makeup inside the stage compensates
    // that, which is why this bound can be tight.
    checkNear (rms (output, 8192) / std::max (1.0e-9, rms (input, 8192)), 1.0, 0.2,
               "the quadrature transform is close to energy neutral");

    std::vector<float> aligned (input.size(), 0.0f);
    for (std::size_t i = 1024; i < input.size(); ++i)
        aligned[i] = input[i - 1024];

    checkLess (std::abs (correlation (aligned, output, 8192)), 0.6,
               "the Side is decorrelated from the delayed Mid");
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

    // And the widening must genuinely be there.
    const auto side = correlation (output.left, output.right, latency * 3);
    checkLess (side, 0.999, "the two channels are not identical (widening happened)");
    checkGreater (side, 0.0, "the two channels stay positively correlated (no phasey mush)");
}

void testMonoCompatibilityWithAllGuards()
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
    const std::size_t from = 24000; // half a second, so every analysis window has settled

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

    checkNear (20.0 * std::log10 (rms (stereo, from) / dryRms), 0.0, 1.0,
               "stereo loudness stays within 1 dB of the dry signal");

    // The mono fold down sits slightly lower, because the Side energy
    // disappears when the channels are summed.
    checkNear (20.0 * std::log10 (rms (monoSum, from) / dryRms), 0.0, 2.0,
               "mono fold down stays within 2 dB of the dry signal");
    checkGreater (correlation (monoSum, dry, from), 0.99,
                  "mono sum is still the same signal, not a filtered version");
}

void testTonalStability()
{
    beginCase ("Engine: steady tone stays clean");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);

    auto parameters = defaultParameters();
    parameters.width = 100.0f;
    engine.setParameters (parameters);

    auto input = makeSine (65536, 440.0, 0.5);
    auto output = runEngine (engine, input);

    const std::size_t from = 24000;

    const double inputRms = rms (input, from);
    const double leftRms = rms (output.left, from);
    const double rightRms = rms (output.right, from);

    checkNear (20.0 * std::log10 (leftRms / inputRms), 0.0, 1.5, "left level within 1.5 dB");
    checkNear (20.0 * std::log10 (rightRms / inputRms), 0.0, 1.5, "right level within 1.5 dB");
    checkNear (20.0 * std::log10 (leftRms / std::max (1.0e-9, rightRms)), 0.0, 0.5,
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
    checkLess (peakAbs (output.left), 1.2 * 0.8, "no peak overshoot on the left channel");
    checkLess (peakAbs (output.right), 1.2 * 0.8, "no peak overshoot on the right channel");

    // The energy between the clicks must not blow up into a reverb tail.
    const std::size_t latency = (std::size_t) engine.getLatencySamples();
    const std::size_t tailStart = latency + 4096 / 2;
    double tail = 0.0;
    for (std::size_t i = tailStart; i < tailStart + 1024 && i < output.left.size(); ++i)
        tail = std::max (tail, (double) std::abs (output.left[i]));

    checkLess (tail, 0.25, "decay between transients stays low");
}

void testAllEnginesAndQualities()
{
    beginCase ("Engine: every engine and quality is finite and latency stable");

    const Engine engines[] = { Engine::natural, Engine::efficient, Engine::smart };
    const Quality qualities[] = { Quality::live, Quality::studio };

    for (auto quality : qualities)
    {
        int referenceLatency = -1;

        for (auto engineChoice : engines)
        {
            WidePocketEngine engine;
            engine.prepare (kSampleRate, 512);

            auto parameters = defaultParameters();
            parameters.engine = engineChoice;
            parameters.quality = quality;
            parameters.width = 85.0f;
            engine.setParameters (parameters);

            auto input = makeVoiceLike (32768);
            auto output = runEngine (engine, input);

            checkTrue (allFinite (output.left) && allFinite (output.right), "output is finite");
            checkLess (peakAbs (output.left), 4.0, "no runaway level");

            if (referenceLatency < 0)
                referenceLatency = engine.getLatencySamples();

            checkNear ((double) engine.getLatencySamples(), (double) referenceLatency, 0.0,
                       "latency is identical for all engines at a given quality");
        }
    }
}

void testEngineSwitchIsSmooth()
{
    beginCase ("Engine: switching engines does not click");

    WidePocketEngine engine;
    engine.prepare (kSampleRate, 512);

    auto parameters = defaultParameters();
    parameters.width = 100.0f;
    engine.setParameters (parameters);

    auto input = makeVoiceLike (49152);
    std::vector<float> left = input, right = input;

    const int blockSize = 256;
    int blockIndex = 0;

    for (std::size_t position = 0; position < left.size(); position += (std::size_t) blockSize, ++blockIndex)
    {
        if (blockIndex == 40)
        {
            parameters.engine = Engine::efficient;
            engine.setParameters (parameters);
        }
        else if (blockIndex == 80)
        {
            parameters.engine = Engine::smart;
            engine.setParameters (parameters);
        }

        const int count = (int) std::min ((std::size_t) blockSize, left.size() - position);
        engine.process (left.data() + position, right.data() + position, count);
    }

    // A click is a sample to sample step much larger than the steepest slope
    // the source signal itself contains, so the source sets the yardstick.
    double worstStep = 0.0, drySteepest = 0.0;
    for (std::size_t i = 2048; i < left.size(); ++i)
    {
        worstStep = std::max (worstStep, (double) std::abs (left[i] - left[i - 1]));
        drySteepest = std::max (drySteepest, (double) std::abs (input[i] - input[i - 1]));
    }

    checkTrue (allFinite (left) && allFinite (right), "output is finite across the switches");
    checkLess (worstStep, 3.0 * drySteepest, "no discontinuity at the engine crossfades");
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
        parameters.air = 50.0f + 50.0f * noise.next();
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

void testMlFallback()
{
    beginCase ("Smart: broken model falls back to the deterministic path");

    struct BrokenModel : MlModel
    {
        bool isReady() const override { return true; }

        bool infer (const MlFeatures&, MlDecision& decision) override
        {
            for (auto& value : decision.bandWidth)
                value = std::numeric_limits<float>::quiet_NaN();
            decision.globalTrim = std::numeric_limits<float>::infinity();
            return true;
        }
    };

    struct SilentModel : MlModel
    {
        bool isReady() const override { return false; }
        bool infer (const MlFeatures&, MlDecision&) override { return true; }
    };

    BrokenModel broken;
    SilentModel silent;

    for (MlModel* model : { (MlModel*) &broken, (MlModel*) &silent, (MlModel*) nullptr })
    {
        WidePocketEngine engine;
        engine.prepare (kSampleRate, 512);
        engine.setMlModel (model);

        auto parameters = defaultParameters();
        parameters.engine = Engine::smart;
        parameters.width = 90.0f;
        engine.setParameters (parameters);

        auto input = makeVoiceLike (32768);
        auto output = runEngine (engine, input);

        checkTrue (allFinite (output.left) && allFinite (output.right),
                   "output is finite regardless of model behaviour");
        checkGreater (rms (output.left, 4096), 0.001, "the plugin keeps passing audio");
    }
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
    testVelvetNoise();
    testStftIsSilentAtZeroGain();
    testStftSideIsQuadratureToMid();
    testStftPreservesSpectrum();
    testWidthZeroIsNull();
    testMonoCompatibility();
    testMonoCompatibilityWithAllGuards();
    testTonalStability();
    testTransientHandling();
    testAllEnginesAndQualities();
    testEngineSwitchIsSmooth();
    testParameterAutomation();
    testNanAndInfInput();
    testMlFallback();
    testAnalyzerFeatures();

    return summary();
}
