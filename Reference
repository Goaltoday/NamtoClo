# NAM → CLO Conversion Algorithm

**Technical Reference for the Reconstructed GP-200 / CLO Conversion Pipeline**  
Version 1.0 — August 2026

> **Purpose.** This document describes the conversion algorithm implemented in the current NAM to CLO release so that other developers can reproduce, analyze, benchmark, and optimize it. It focuses on conversion mathematics and DSP rather than the GUI or USB uploader.
>
> **Status convention.** “Reconstructed / confirmed” means the behavior is directly represented in the current code and was reconstructed from the official GP-200 conversion path during the project. “Project-validated” refers to prior same-input comparisons against official CLO output. “Implementation choice” identifies optional features added by this project. “Open optimization area” is a hypothesis/research direction, not a confirmed property of the official converter.

Reference implementation:
- `src/native_converter.cpp`
- `src/stimulus.cpp`
- `src/corrective_ir.cpp`
- `src/clo_refiner.cpp`
- `src/common.cpp`

## 1. Scope and reconstruction status

The converter approximates a Neural Amp Modeler (NAM) model with the fixed signal structure supported by the CLO runtime. The problem is parameter identification inside a specific DSP topology rather than unconstrained neural-model distillation.

The current application exports a GP-200-compatible 1024-tap CLO. Internally, the native fitting algorithm first constructs the 128 + 2048 form and then serializes the GP-200-compatible compact 128 + 1024 form.

| Subsystem | Status | Notes |
|---|---|---|
| 70 s original stimulus | Reconstructed / confirmed | 50 s base + 20 s tail/reamp; 600 zero guard samples |
| NAM rendering scale | Reconstructed / confirmed | `0.31f` |
| NAM block maximum | Reconstructed / confirmed | 1024 |
| Detrend / latency | Reconstructed / confirmed | Float32 detrend; up to 600-sample search |
| P/K fit | Reconstructed / confirmed | 100 ms extrema, 5 s, branch slope seed + discrete K search |
| A128/B2048 fitting | Reconstructed / confirmed | Three phases, ten iterations |
| Final B refinement | Reconstructed / confirmed | Final 20 s, 256-tap correction |
| Compact B1024 output | Reconstructed / confirmed | First 1024 taps retained |
| Corrective IR | Implementation choice | Optional post-fit Block-B processing |
| Tone Match | Implementation choice | Optional final-20-s spectral Block-B refinement |

## 2. End-to-end model

```text
Input
 → PRE biquad
 → FIR A (128)
 → 2× interpolator
 → 2× interpolator
 → asymmetric exponential waveshaper @ 4×
 → 2× decimator
 → 2× decimator
 → POST biquad
 → FIR B (2048 during fitting, 1024 exported)
 → Output
```

### Waveshaper

For `x > 0`:

`y = P+ · (1 − exp(−K+·x))`

For `x ≤ 0`:

`y = P− · (exp(K−·x) − 1)`

## 3. Stimulus and NAM rendering

- 0–50 s: `nam_input_wav.wav`
- 50–70 s: original tail or recorded audio
- 600 zero samples: trainer guard/storage
- Original stimulus rate: 44.1 kHz
- NAM is rendered at its expected sample rate.
- Reset maximum block: 1024 samples.
- Target scaling: `NAM_target = 0.31f × NAM(input)`.
- A2 SlimmableContainer models use the submodel with the highest `max_value`.

## 4. Target preprocessing

First remove the mean, then fit/subtract a straight line with `x=1..N`:

```text
mean = Σy / N
slope = (NΣxy − ΣxΣy) / (NΣx² − (Σx)²)
intercept = (Σy − slopeΣx) / N
```

Latency is searched near the 6 s point over a 600-sample interval. The target is shifted left by the detected latency.

## 5. P/K identification

Use the first 5 s, divided into 100 ms windows:

```text
x_abs[w] = max |input|
y_pos[w] = max(target, 0)
y_neg[w] = min(target, 0)

P+ = max(y_pos)
P− = max(−y_neg)
```

Low-level branch slopes seed K:

```text
K+_0 = slope+ / P+
K−_0 = slope− / P−
```

Each K is searched with multipliers `0.80, 0.85, …, 1.20`, positive branch first, minimizing float32 SSE over the signed extrema dataset.

## 6. Trainer runtime model

The trainer biquad keeps the reconstructed numerical path:

```text
w0 = 1000·x − a1·w1 − a2·w2
yd = b0·w0 + b1·w1 + b2·w2
y  = float(float(yd) · 0.001)
```

POST at sample rate `Fs`:

```text
c  = 177.7158051
w2 = 15791.45215
D  = Fs² + cFs + w2

b0 = Fs²/D
b1 = −2b0
b2 = b0
a1 = −(2Fs² − 2w2)/D
a2 = (Fs² − cFs + w2)/D
```

FIR evaluation uses the reconstructed 64-sample partition / 128-point float FFT engine.

## 7. Spectral transfer estimator

```text
L ≈ round(0.125·Fs)
hop = 50%
FFT = 2048
```

For each Hamming-windowed, mean-removed frame:

```text
Sxx += |X|²
Sxy += X* · Y
R[k] = |Sxy[k]| / (Sxx[k] + ε)
ε = 1.1920928955078125e−7
```

## 8. Magnitude conditioning

```text
mel(f) = 2595 log10(1 + f/700)
f(mel) = 700(10^(mel/2595) − 1)
```

The routine converts magnitude to dB, Gaussian-smooths, interpolates on a uniform Mel grid, smooths again, changes point count if required, interpolates back to linear frequency, then converts dB to linear magnitude.

## 9. Minimum-phase reconstruction

1. Mirror positive magnitude to a full spectrum.
2. Floor magnitude at `max(|M|)·10^-5`.
3. Take log magnitude.
4. Inverse direct float DFT.
5. Apply causal cepstral lifter.
6. Forward direct float DFT.
7. Complex exponential.
8. Inverse direct float DFT.
9. Retain requested taps.
10. Remove mean.
11. Normalize retained energy to full impulse energy.

## 10. Initial A/B factorization

- Low-level region: 6–21 s → `lowSpec`
- Conditioned sweep: 23–28 s → `sweepSpec`

After smoothing / regularization:

```text
B_factor = sweepSpec
A_state  = lowSpec / sweepSpec
```

## 11. Iterative A/B fit

Phases:
- 23–28 s: 3 iterations
- 6–21 s: 2 iterations
- 30–50 s: 5 iterations

One iteration:

```text
corr_step = clamp(corr^(weight·step), 0.2, 5)
step *= 0.8999999761581421

conditioned = conditionMagnitude(conditionMagnitude(corr_step))
B_factor *= conditioned
A_state  /= conditioned
A_state = lowFrequencySequentialSmooth(A_state)

A = minimumPhase(conditionMagnitude(A_state, 128), 128)

preB  = render(PRE → A → NL4× → POST)
fresh = ratioSpectrum(preB, target)

next_corr = conditionMagnitude(fresh / B_factor)
B = minimumPhase(fresh, 2048)

residual = ratioSpectrum(render(full model), target)
loss = mean_512_mel_points_80_to_10000(|ln(residual + ε)|)
```

Acceptance:
- better loss → save best
- `loss > 1.2×best` → restore best and halve step
- otherwise keep current live state
- each phase exits on its best snapshot

## 12. Final B tail refinement

Using 50–70 s:

```text
corr = minimumPhase(conditionedTailRatio, 256)
B = truncate_2048(B * corr)
B -= mean(B)

gain = sqrt(E_target / E_model)
B *= gain
```

## 13. Serialization

Internal form: `VTSI`, A=128, B=2048, physical size `0x2288`.

Before storing:
- FIRs are resampled to 44.1 kHz when needed.
- **B is multiplied by 4.0 before serialization.**

GP-200 compact output:
- declared size `0x1288`
- payload `0x1200`
- `countB = 1024`
- only first 1024 B taps are retained
- CRC16/MODBUS is recalculated

## 14. Corrective IR (project feature)

```text
B_conv[n] = Σ B_old[n−k]·IR[k]
g_rms = RMS(B_old) / RMS(B_conv)
g_post = 10^(−6/20)
B_corrected = B_conv · g_rms · g_post
```

This is not claimed to be part of the official conversion algorithm.

## 15. Tone Match (project feature)

Final 20 s only:
- FFT 16384
- hop 4096
- 11 groups
- analysis 30–20000 Hz
- comparison 40–18000 Hz, 512 log-spaced points
- reject below −55 dB or clipping ≥0.999
- 5% smoothing
- synthesize 2048-sample minimum-phase correction IR
- convolve into B with 0 dB post gain

## 16. Numerical fidelity

The reconstruction deliberately preserves float/double boundaries, incremental linspace generation, custom interpolation operation order, Q15 FFT twiddles, direct-DFT minimum-phase reconstruction, discrete K candidate generation, separate signal/FIR SRC call patterns, and B×4 serialization scaling.

## 17. High-level pseudocode

```text
render fixed 70 s stimulus through NAM
→ scale by 0.31
→ detrend
→ latency-align
→ fit P/K on 0–5 s
→ initialize A/B from 6–21 s and conditioned 23–28 s
→ optimize A/B for 10 iterations across 3 regions
→ refine B on 50–70 s
→ serialize A128/B2048 at 44.1 kHz with B×4
→ optional Tone Match or Corrective IR
→ compact to A128/B1024 GP-200 CLO
```

## 18. Optimization opportunities (hypotheses)

These are not part of the reconstructed official algorithm:

- alternative multi-objective losses including transient, THD and IMD metrics;
- explicit regularization of A/B;
- tests that distinguish optimizer error from limits of the four-parameter memoryless nonlinearity;
- GP-200-specific optimization with B1024 truncation included inside the objective;
- evaluation on real DI material, level sweeps and two-tone tests instead of stationary spectral matching alone.

## Appendix A — key constants

| Constant | Value |
|---|---:|
| A taps | 128 |
| B fit taps | 2048 |
| GP-200 B taps | 1024 |
| Estimator FFT | 2048 |
| Positive bins | 1025 |
| NAM block max | 1024 |
| NAM scale | 0.31f |
| Guard | 600 samples |
| P/K region | 0–5 s |
| A/B phases | 23–28 / 6–21 / 30–50 s |
| B refinement | 50–70 s |
| Final B correction | 256 taps |
| CLO rate | 44.1 kHz |
| B serialization gain | ×4 |

## Appendix B — CLO layout

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | 4 bytes | `VTSI` |
| 0x18–0x38 | 5×float64 | PRE |
| 0x40–0x60 | 5×float64 | POST |
| 0x68 | float32 | P+ |
| 0x6C | float32 | P− |
| 0x70 | float32 | K+ |
| 0x74 | float32 | K− |
| 0x78 | uint32 | startA=0 |
| 0x7C | uint32 | countA=128 |
| 0x80 | uint32 | startB=128 |
| 0x84 | uint32 | countB=2048 internal / 1024 GP-200 |
| 0x88 | float32[] | A then B |

## Appendix C — all-pass coefficients

**2× interpolation stage 1**
- A: `0.0457281470, 0.3325011134, 0.6632020473, 0.9338558316`
- B: `0.1680875421, 0.5044857264, 0.8037808537`

**2× interpolation stage 2**
- A: `0.0542307794, 0.3987969756, 0.8629178405`
- B: `0.1996995807, 0.6210968494`

**2× decimation stage 1**
- A: `0.0707659498, 0.5131675601`
- B: `0.2578530908, 0.8173173666`

**2× decimation stage 2**
- A: `0.0542175248, 0.3830873370, 0.7487209439`
- B: `0.1967979670, 0.5731363893, 0.9142937064`

---

The recommended research workflow is to preserve the reconstructed native algorithm as an untouched regression baseline and develop perceptual improvements on a separate experimental branch.
