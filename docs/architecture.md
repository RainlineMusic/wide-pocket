# Wide Pocket - architecture (v0.2)

## Why v0.1 wandered, and what changed

v0.1 built the Side signal by rotating every STFT bin by a pseudo-random
phase angle. That leaves a Mid-correlated component inside the Side, and the
inter-channel level difference of `L = M + S`, `R = M - S` is exactly

```
|L|^2 - |R|^2 = 4 * sum_k Re{ M[k] * conj(S[k]) }
```

so a random rotation produces a real, static image bias - the "leans left"
report. Every frequency dependent stage on top of it (the Air shelf, the
guards) biased the image differently per band, which is why sibilants pulled
right and vowels pulled left, and why the broadband Center Lock could only
replace a static offset with slow left-right movement.

v0.2 removes the cause instead of correcting the symptom:

```
S[k] = j * s[k] * g[k] * M[k],   s[k] in {-1, +1},  g[k] real
```

Every bin of the Side is exactly 90 degrees from the same bin of the Mid, so
`Re{M * conj(S)} = 0` in every bin and the level difference is identically
zero for *any* set of real band gains. Decorrelation comes from the frequency
dependent +-1 sign pattern (fixed seed, groups of three bins), which never
touches magnitudes.

## Signal flow

```
in -> M/S encode -> analyser (Mid only)
        |
        +-> Natural / Smart : STFT quadrature Side generator
        +-> Efficient       : sparse velvet noise FIR
                  |
                  +-> equal power crossfade between engines
                             |
                   real, slow scalar guards
              (Sibilance Guard, Transient Focus, Smart trim)
                             |
delayed Mid ----> M/S decode ---> output gain ---> out
             (centre alignment safety net in between)
```

- **Width, Focus, Air, spatial mask, low-end mono** are real per-band gains on
  the quadrature Side. None of them can move the image.
- **Focus** is now a real presence-band narrowing curve (~300 Hz - 3 kHz) with
  a top-end lift. In v0.1 the same mild curve was applied twice, in the engine
  and again in the Smart controller, which is why the knob did almost nothing.
- **Air** is no longer a minimum-phase shelf. A shelf on the Side is a
  frequency dependent phase shift, i.e. an image bias.
- **Sibilance Guard / Transient Focus / plosive duck** are one slow real
  scalar. They change how wide, never where.
- **Low mono** is an internal 150 Hz constant, not a user parameter.
- **CenterAlignment** is a six-band projection `S' = S - sum c_b * m_b` that
  only has work to do on the velvet (Efficient) path, whose shaping filters
  are minimum phase. With the quadrature engines its coefficients sit at zero.

## Removed switches

Mono Safe, Center Lock and Auto Gain are gone, and they are not hidden
parameters either:

- the Mid path is a pure delay, so the mono sum is always exactly the dry
  signal - "mono safe" is structural,
- the Side is quadrature to the Mid, so the image is centred by construction -
  there is nothing to lock and nothing to measure, therefore nothing to drift,
- the Side adds no broadband level to the sum, so there is nothing for an auto
  gain stage to chase.

## Invariants enforced by `Tests/dsp_core_test.cpp`

- zero band gain gives exact silence, so Width = 0 is a sample-exact null,
- Mid and Side are uncorrelated (`|corr| < 0.05`), hence no level difference,
- `L + R == 2 * Mid` at any setting,
- a steady tone keeps left and right within 0.5 dB of each other,
- latency is identical for all three engines at a given Quality,
- no code path can emit NaN or Inf, including NaN/Inf input.

## GUI (v0.2)

Design canvas 960 x 636, themes and drawing carried over from Phase Pocket
v0.8 (Neon / Solid Dark / Solid White, gear menu, blurred bypass).

- right column: **Width** and **Focus**, with a small **Output** between them,
- left: one wide Ozone-Imager-style stretched goniometer (`STEREO IMAGE`) with
  a correlation scale,
- under it: **Air**, **Stability**, **Sibilance**, **Transient**,
- arrow drawer (bottom left) holds the **Engine** selector and the four debug
  readouts,
- **Quality** lives only in the gear menu.

The scope reads a dense point stream (one point every `sampleRate / 12000`
samples, ~200 per displayed frame at 60 fps) instead of one point per audio
block, and its Side axis is `(R - L)`, so left-heavy material draws on the
left like every external meter. Both were wrong in v0.1.
