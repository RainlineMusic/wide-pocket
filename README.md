# Wide Pocket

Natural mono-to-stereo vocal spatial widener. JUCE 8.0.4 plug-in by Rainline Music.

- **Product name:** Wide Pocket
- **Version:** 0.1.0
- **Formats:** VST3, AAX (developer/unsigned), Standalone
- **Platforms:** Windows x64, macOS universal (arm64 + x86_64, deployment target 11.0)

## What it does

Wide Pocket turns a mono or narrow vocal into a wide, stable stereo image without
a conventional Haas delay, pitch modulation or chorus. The plug-in now has one
focused high-quality engine: **Natural**.

Natural combines a short STFT with subband Schroeder allpasses, time/frequency
envelope reconstruction, transient protection and spectral centre locking. The
original Mid path is only delayed; widening is synthesised entirely in Side:

```
L = Mid + Side
R = Mid - Side
```

Consequently the mono sum is the delayed dry Mid at every Width setting. At
`Width = 0%`, mono and stereo inputs are reproduced sample-exactly after the
reported latency.

## Stable centre

Centre correction is performed in perceptual bands inside the STFT, before Side
is synthesised. It removes the in-phase projection that causes L/R energy bias.
This replaces the old moving six-band `CenterAlignment`, which could chase
phonemes and make the vocal wander.

The engine uses no random +/-j bin groups, no velvet-noise path and no adaptive
engine switching. It has a fixed latency of 256 samples and no runtime FFT
reconfiguration.

## Controls

- **Width** — generated Side amount
- **Focus** — gently anchors the vocal formant region
- **Air** — bounded extra width in the top bands
- **Stability** — speed of slow spatial adaptation
- **Sibilance Guard** — reduces excessive width on S/SH/T sounds
- **Transient Focus** — protects syllable attacks and plosives
- **Output** — -24 to +12 dB

There is no forced mono region below 150 Hz. Low content is allowed to widen
through the same bounded width law as the rest of the signal.

The former Engine & analysis drawer, Efficient and Smart algorithms, engine
selector and Quality switch have been removed.

## Buses

- mono in -> stereo out
- stereo in -> stereo out

## Building

See [docs/build-windows.md](docs/build-windows.md), [docs/build-macos.md](docs/build-macos.md)
and [docs/aax-setup.md](docs/aax-setup.md).

```bash
cmake -S . -B build -DJUCE_DIR=/path/to/JUCE-8.0.4
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

JUCE is not vendored. CI downloads the pinned JUCE 8.0.4 release.

## DSP tests

The JUCE-free test suite verifies exact mono folding, stereo identity at Width
zero, useful Side energy, short-window L/R stability, low-frequency widening,
transient protection, automation safety and finite output.

## Status

Experimental software under active development. Back up projects before use.
CI builds are unsigned/developer builds.
