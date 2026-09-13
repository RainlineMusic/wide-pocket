# Wide Pocket - architecture

Mono-to-stereo vocal spatial widener. Everything happens in the Mid/Side
domain, which is what makes the mono fold-down safe by construction instead of
by correction.

## Signal flow (per sample)

```
in ---> M/S encode ---> analyser (Mid only)
          |
          +--> Natural / Smart : STFT all-pass decorrelation
          +--> Efficient       : sparse velvet noise FIR
                    |
                    +--> equal power crossfade between engines
                               |
             side shaping: Low Mono, Air, Transient Focus,
             Sibilance Guard, Width, Focus
                               |
delayed Mid ------------> M/S decode ---> Center Lock --->
             Correlation Guard ---> Auto Gain ---> Output
```

## Engines

| Engine | Method | Cost | Character |
| --- | --- | --- | --- |
| Natural | STFT per-bin phase rotation, magnitudes untouched | high | most transparent |
| Efficient | optimised velvet noise FIR (sparse +-1 taps) | low | slightly more diffuse |
| Smart | Natural plus per-band adaptive width from the analyser | high | widest, self-restraining |

The Smart controller exposes an `MlModel` seam. Version 0.1 ships no model and
never depends on one: if no model is attached, or it is not ready, or inference
fails, or it returns non-finite values, the deterministic path is used
unchanged.

## Invariants enforced by the test suite

- **Width = 0 is an exact null.** The dry Mid path is delayed by exactly the
  engine latency, so the output equals the latency-aligned input.
- **The mono sum never contains the synthesised Side signal.** `L + R = 2 * M`.
- **Latency is identical for all three engines** at a given Quality, so engine
  switching is click-free and never renegotiates latency with the host.
- **Low + high of the Low Mono crossover reconstructs the input** sample for
  sample, so there is no ripple and no phase error in the dry path.
- **No code path can emit NaN or Inf**, including NaN/Inf input.
- **Center Lock drives the inter-channel level difference to zero** by
  projecting Side onto the complement of Mid, so the image cannot drift and the
  mono sum stays untouched.

Latency: 1024 samples in Studio quality, 256 in Live.

## Running the tests locally

No JUCE and no test framework required:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -o wptests Tests/dsp_core_test.cpp
./wptests
```

CI runs the same suite through CTest (`WidePocketTests`) before the VST3 and
AAX targets are linked.

## Building the plugin

```sh
cmake -S . -B build -DJUCE_DIR=/path/to/JUCE-8.0.4
cmake --build build --config Release --target WidePocket_VST3 WidePocket_AAX
```

The AAX SDK is bundled with JUCE 8.0.4, so no separate Avid SDK download is
needed to configure or build the AAX target.

## AAX signing

CI produces unsigned developer builds. The workflow uploads both a Release and
a Debug AAX; Pro Tools will only load them after a local PACE signature:

```sh
wraptool sign --verbose \
  --account <iLok account> \
  --wcguid <your GUID> \
  --signid "Developer ID Application: ..." \
  --in "Wide Pocket.aaxplugin" \
  --out "Wide Pocket-signed.aaxplugin"
```
