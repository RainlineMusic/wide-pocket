# Wide Pocket - Natural architecture

## Product decision

Wide Pocket now has one widening engine: Natural. Efficient/velvet noise,
Smart/ML scaffolding, the engine selector, the analysis drawer, runtime Quality
switching and the forced mono region below 150 Hz have been removed.

## Signal flow

```
input -> M/S encode
          |\
          | +-> immutable 256-sample delay -------------------- Mid
          |
          +-> 256-point sine-window STFT, 50% overlap
                -> 4-frame pre-delay
                -> four complex subband Schroeder allpasses
                   (gamma 0.7, delays 1/2/3/5 frames)
                -> per-bin t/F envelope reconstruction
                -> perceptual-band spectral centre lock
                -> transient hold / recursive-path exclusion
                -> slow real Width/Focus/Air gains ----------- Side

L = Mid + original Side + generated Side
R = Mid - original Side - generated Side
```

## Stable centre

The old six-band time-domain CenterAlignment was a feedback correction after the
image had already been formed. Its overlapping crossover bands could chase
phonemes independently and turn a static bias into image movement.

Natural instead performs centre correction before synthesis, in the STFT domain
where Side is created. For every perceptual band it removes only the in-phase
projection of the processed spectrum onto the direct spectrum:

```
c_b = sum Re{X[k] conj(Y[k])} / sum |X[k]|^2
Y_locked[k] = Y[k] - c_b X[k]
```

The remaining band is energy-normalised. This controls the cause of L/R level
bias without moving separate formants with time-domain crossover filters. The
original Mid is never modified and existing stereo Side is preserved.

## Decorrelator

The core follows the DAFx23/MPEG-I structure:

- 256 samples, 50% overlap, sine analysis/synthesis window;
- four-frame pre-delay;
- four Schroeder allpasses with gamma 0.7 and frame delays 1, 2, 3 and 5;
- per-bin direct/processed energy followers with alpha 0.4;
- bounded t/F envelope reconstruction with beta 1.5;
- a small coherent quadrature floor for stationary bin-centred harmonics.

There are no random +/-j bin groups. Phase evolution comes from the allpass
states and remains smooth across the source instead of assigning neighbouring
vocal partials to arbitrary sides.

## Transients

An onset detector uses the DAFx23 energy ratio (threshold 2.8), an eight-frame
hold and a 56-frame inhibit period. During a detected onset the processed Side
is muted and the transient frame is not written into the recursive pre-delay.
The latter prevents the onset from reappearing later as an allpass tail.

The vocal analyser additionally applies a slow, scalar plosive/sibilance guard.
A real broadband scalar changes width but cannot pan the source.

## Controls

- Width maps perceptually to generated Side energy and reaches useful width at
  full scale while keeping Side below Mid.
- Focus gently anchors the formant region but never closes a band.
- Air adds bounded high-band Side gain.
- Stability controls only slow width adaptation (180-520 ms).
- Sibilance and Transient are guards, not separate engines.
- Output is -24 to +12 dB.

No band is forced mono. Content below 150 Hz follows the same bounded width law.

## Fixed latency and real-time behaviour

Natural has a fixed reported latency of 256 samples. There is no runtime FFT
reconfiguration or allocation in `setParameters`. Bypass stores the original
dry input rather than processed output and keeps the recursive engine warm in a
preallocated scratch buffer.

## Automated invariants

`Tests/dsp_core_test.cpp` verifies:

- Width 0 is sample-exact for mono and stereo input after latency;
- L+R is the delayed dry mono sum at full width;
- generated Side has useful energy and stays below Mid;
- mean 50 ms ILD on vocal-like material is below 0.3 dB and no window exceeds
  0.8 dB;
- 110 Hz is allowed to widen;
- transient clicks do not create a decorrelator tail;
- dense automation and NaN/Inf input remain finite.
