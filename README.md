# Wide Pocket

Natural mono-to-stereo vocal spatial widener. JUCE 8.0.4 plug-in by Rainline Music.

- **Product name:** Wide Pocket
- **Version:** 0.1.0
- **Formats:** VST3, AAX (developer/unsigned), Standalone (local debugging only)
- **Platforms:** Windows x64, macOS universal (arm64 + x86_64, deployment target 11.0)
- **Plug-in code:** `WdPk` · **Manufacturer code:** `RnLn` · **Bundle ID:** `com.rainlinemusic.widepocket`

## What it does

Wide Pocket turns a mono or narrow vocal into a wide, phase-coherent stereo image
without the hollow center, comb filtering or mono-collapse artefacts of classic
Haas/chorus widening. All processing is performed in the Mid/Side domain:

```
L = g_M * M + g_S * S
R = g_M * M - g_S * S
```

The Mid path stays dry and latency-aligned; widening only synthesises the Side
component, so intelligibility of the center never drops as Width increases.
At `Width = 0%` the output is bit-transparent latency-aligned dry signal.

### Engines

| Engine | Method | Use |
| --- | --- | --- |
| **Natural** | STFT sub-band all-pass decorrelation with COLA-correct overlap-add | Highest quality, studio work |
| **Efficient** | Sparse velvet-noise (OVN) FIR decorrelation | Low CPU, many instances |
| **Smart** | Adaptive parametric stereo controller driven by the vocal analyser | Set-and-forget |

**Version 0.1.0 note:** `Smart` is a fully deterministic adaptive parametric
stereo controller. It is a real working algorithm, not a stub, and it contains
**no trained neural network**. The ML inference interface, feature schema,
output schema and safe fallback are implemented and documented so that a trained
model can be dropped in for v0.2.0 without changing the product surface.

### Safety systems

- **Mono Safe** — guarantees the mono sum stays artefact-free.
- **Center Lock** — pins the perceived center; with Mono Safe also on, IID is forced to 0.
- **Correlation Guard** — limits inter-channel correlation excursions.
- **Auto Gain** — loudness compensation so A/B comparisons stay level-matched.
- **Sibilance Guard** and **Transient Focus** — keep S/T sounds and consonants centered and dry.

## Controls

Large: **Width**, **Focus**. Compact: **Air**, **Stability**, **Low Mono**,
**Sibilance Guard**, **Transient Focus**, **Output**.
Switches: **Engine** (Natural / Efficient / Smart), **Quality** (Live / Studio),
**Mono Safe**, **Center Lock**, **Auto Gain**, plus three UI themes
(Neon / Solid Dark / Solid White) in the settings menu.

There is no separate Mix control: Width already scales the synthesised Side
component while the dry Mid is preserved, so a dry/wet knob would only duplicate it.

## Buses

- mono in -> stereo out
- stereo in -> stereo out (analysed center is `M = (L + R) / 2`)

No sidechain input. Wide Pocket is a new plug-in and is intentionally **not**
compatible with Phase Pocket sessions: new parameter IDs, plug-in code, bundle ID
and state namespace.

## Building

See [docs/build-windows.md](docs/build-windows.md), [docs/build-macos.md](docs/build-macos.md)
and [docs/aax-setup.md](docs/aax-setup.md).

```bash
cmake -S . -B build -DJUCE_DIR=/path/to/JUCE-8.0.4
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

JUCE is not vendored in this repository. Pass an existing checkout through
`-DJUCE_DIR=...`; CI downloads JUCE 8.0.4 from a pinned URL.

## Status

Experimental software under active development. Back up your projects before use.
macOS builds from CI are ad-hoc signed and not notarized; Windows builds are
unsigned; AAX builds are developer (unsigned) builds and must be signed locally
with PACE before Pro Tools will load them.
