# 2026-11-24 — B2.1 Adaptive Luma Participation

Base: exact user archive `MPC-VR-Edition-for-GPT-20261124-145114-817.zip` plus delivered Reference Pipeline Correction overlay. The unshipped multi-constraint Source-Adaptive experiment is NOT included. No Git commit/push, no new model, runtime dependency or UI setting. Cumulative incremental overlay against the exact original user archive; safe also over the Reference Pipeline Correction alone. If other local changes exist, merge rather than overwrite them blindly.

## User decision and design

User prefers keeping the verified SVDLUT model and reference input/packing/temporal B1. The existing A separates Y' from chroma and is useful. B2.1 should make SVDLUT more adaptive by reducing its authority over brightness without jointly reducing color. This replaces the previous 3-zone multiplicative gains; it does not make A and B equivalent. W3C Compositing and Blending Level 1, nonseparable modes (`SetLum`, `ClipColor`, https://www.w3.org/TR/compositing-1/#blendingnonseparable) is the mature mathematical reference. Edition uses Rec.709 code-luma coefficients (.2126,.7152,.0722), not W3C's (.3,.59,.11), and custom adaptive participation; W3C does not establish the engineering parameters or video quality of this adaptation. Y' is not physical linear-light luminance or perceptual lightness.

- Let S be source RGB, G the B1 packed LUT's clamped RGB candidate, Ys and Yg its code luma. Choose Yt = Ys + clamp(alpha*(Yg-Ys), -4/255, +4/255). alpha in [0,.25] measured before draw. Chroma C = G-Yg. Output Yt+C*k where k is the largest scalar in [0,1] fitting SDR RGB gamut (SetLum/ClipColor-style radial compression). Preserve model chroma where feasible. No added image pass, texture, readback, inference or model mutation. Existing B strength blend is applied afterwards; off B2.1 uses alpha=1 sentinel bypass and existing B1/B2.2/R1/P1 behavior.
- CPU calibration uses the same existing 512 original thumbnail samples and actual selected B1 parameters. Alpha capped by largest sampled |delta Y'| and 256 neutral steps for each of four packed edge variants (contrast ratio envelope 0.5..1.5 at those probe points). All-neutral probes are safety checks, not observed content; no OCR or title-card classifier. Absolute per-pixel cap protects unsampled colors, but neutral ramp guarantees do not imply all RGB spatial gradients are preserved. No claim of optimal .25/4-code/0.5..1.5 engineering tolerances. These are conservative starting policy parameters. Shader and CPU use same formula.
- Reductions immediate, release time constant 180 ms same shot, no old constraint inherited across cuts. Exact cached LUT/source match reused. New unmeasured candidate under low buffer or missing samples uses alpha=0 rather than full brightness. Same-PTS revised LUT cannot increase alpha, paused replay only uses a previously valid B2.1 participation (not off sentinel). No modification to B1 scheduling or PTS/fence ownership. Existing B2.2/R1/M1/P1 continue their independent color logic; B2.1 gives them a calibrated reference. Independent P1 depth tone operation may still alter final brightness; the 4-code bound applies to B2.1 before downstream adjustments/dither, not every possible renderer output.
- Existing B2.1 checkbox and registry unchanged; tooltip and diagnostic text updated. The old estimator and sample reader remain in headers as reference and testing; production uses `SvdLutB2AdaptiveLuma.h`. Old `Tone.gains` array is reused as three copies of scalar participation for internal layout compatibility, no new member order. A/RAW shader math unchanged; shader compile helper prepends common B2.1 helper (unused there). Existing B2 shader compile failure fallback unchanged and visible in stats.

## Offline tests actually run

Linux g++ C++20 -O2: 100,000 random RGB composition cases (exact bypass at alpha=1; identity transform, zero luma, gamut, signed luma correction), 30 random packed LUTs x 4 variants x 256 gradient steps, missing data and asymmetric temporal updates. Also ASan/UBSan passed. Existing reference input test passed (196608 planar and 1536 B2 sample values, padded/unaligned rows). Actual supplied manifest-matching graph decoded using existing CPU reference PyTorch script. Candidate LUT packing verified against reference interior. 3 provided screenshots and 4 scikit-image natural photos; FP32 256x256 bilinear thumbnails; single-image LUT/full B strength, no B1 multi-frame, P1, MFG, actual GPU or display capture. C++ calibration result used in mirrored PyTorch output formula. Results in `notes/svdlut-b21-adaptive-luma-results.jsonl`.

| Case | RAW mean abs Y' code | old B2.1 | new B2.1 | alpha | tested near-neutral horizontal gradient reversals RAW -> new |
|---|---:|---:|---:|---:|---:|
| Bandai Namco | 40.56 | 23.35 | 3.14 | 7.75% | 43 -> 0 |
| Sunrise | 82.91 | 67.36 | 3.06 | 3.69% | 23 -> 0 |
| Gundam glow | 3.21 | 3.21 | 0.17 | 5.43% | 16 -> 0 |
| astronaut | 7.02 | 3.53 | .67 | 9.54% | 421 -> 0 |
| coffee | 9.96 | 6.06 | .90 | 9.03% | 115 -> 0 |
| chelsea | 31.28 | 15.87 | 3.01 | 9.62% | 0 -> 0 |
| rocket | 16.42 | 13.11 | 1.13 | 6.88% | 18 -> 0 |

Mean absolute chroma residual magnitude relative to RAW: natural photos 95.5–97.8%; not a similarity/quality score. Maximum RGB channel changes can remain large, especially Sunrise (~58.46 codes), because reducing Y' is deliberately not equivalent to suppressing chroma. No claim that color drift/visible banding completely resolved. Average calibration CPU ~0.24–0.27 ms here, not Windows/GPU timing. Gradient mask is near-neutral source, adjacent horizontal source difference 0.75–18 code; not full frame spatial quality metric.

Reproduce (from project root, developer-only Python dependencies numpy/torch/Pillow/scikit-image):
```
g++ -std=c++20 -O2 build-support/edition/tests/SvdLutAdaptiveLuma.cpp -o /tmp/b21
/tmp/b21
python build-support/edition/tests/SvdLutAdaptiveLumaModel.py /path/to/svdlut-fivek-srgb.dmlgraph /tmp/b21 /path/to/card1.png /path/to/card2.png /path/to/card3.png
```
Windows: existing `build-x64.cmd`; revised `SvdLutReferenceShaders.cpp` uses the exact common shader assembly and checks all variants' constant buffers/dither binding. Windows x64 Release, native shader compilation, PowerShell page tests and upstream boundary check cannot be executed in this Linux environment; no false success claim. No original checkpoint parity or actual DirectML GPU numeric comparison yet; previous reference-pipeline limitations remain. If testing live, compare Temporal B with B2.1 enabled, other optional color features off to isolate, then compare normal films and combined modes. No new settings or required dependencies.
