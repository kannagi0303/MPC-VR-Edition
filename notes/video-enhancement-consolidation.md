# Video Enhancement consolidation — 2026-11-24 16:39:20 +08:00

## Baseline and scope
- User confirms B2.1 and B2.2 acceptable in playback and committed to GitHub. This patch is based on the exact local delivered B22 overlay (`mpc_b22`), NOT a newly fetched GitHub commit; check local diff if there were additional commits.
- One `Video Enhancement` checkbox, no A/RAW mode, B strength slider, B2.1/B2.2/R1 or independent Preferred/Skin controls. Temporal B always uses calibrated full blend 1.0 (former 200%); preferred colors + skin both active. Adaptive Tone is a separate future research item; not implemented here.
- Keep Settings_t ABI and native registry/preset compatibility slots (retired A strength no longer affects rendering). New global DWORD `VideoEnhancement` (0/1) is authoritative. If missing/invalid, migrate prior effective SVDLUT enable or either old perceptual enable; otherwise default off. Legacy keys are not live mode selectors. Apply/Cancel restore one live flag; toggle clears old PTS/LUT/backbuffer via existing fast stop. No automatic commit/push or registry write in this environment.
- Remove runtime A scene worker/capture/draw, synchronous RAW inference route, R1/M1/D1 experiment selection and state. Historical shader strings and legacy CPU helpers retained for reference tests; production B worker skips compiling A-only grade shader. The worker's actual graph, weight handling, FP32 input and LUT math are unchanged. Fully fused perceptual path is primary; if unavailable, calibrated B2.2/B2.1 fallback, never uncalibrated RAW; if no LUT/grade available, existing standalone perceptual fallback (SDR only), otherwise normal draw. PTS, fences, original-frame protection and scheduling untouched except the existing grade/cache gating is now the single flag.

## Code map (search tags)
| Tag | Responsibility | Main files |
|---|---|---|
| VE-00 | Single setting, preview and persistence | VideoRenderer.h/.cpp, Edition/RendererControls.cpp, Edition/PropPageExtended.cpp |
| VE-01 | Model/FP32 input | Edition/SvdLutRawMode.inc, DLSS/SvdLutModel.inc, DLSS/SvdLutShader.h |
| VE-02 | Temporal LUT statistics (B1) | Edition/SvdLutTemporalStatistics.h, Edition/SvdLutRawMode.inc |
| VE-03 | Adaptive Luma (B2.1) | Edition/SvdLutB2AdaptiveLuma.h, Edition/SvdLutRawMode.inc, DLSS/SvdLutShader.h |
| VE-04 | Adaptive Chroma (B2.2) | DLSS/SvdLutShader.h |
| VE-05 | Preferred Colors | DLSS/PerceptualColorShader.h |
| VE-06 | Skin Tones | DLSS/PerceptualColorShader.h |
| VE-07 | Fused composition / fallback | DX11VideoProcessorSvdLut.cpp, Edition/SvdLutRawMode.inc |

Date/time on section comments identifies this consolidation edit, not the original invention or validation date. For prior stages see notes/svdlut-b21-adaptive-luma.md, notes/svdlut-b22-adaptive-chroma.md and Git history.

## Verification actually executed in Linux
- `SvdLutReferenceInput.cpp`: 196608 planar values + 1536 sample values, padded/unaligned rows, invalid inputs: PASS.
- `SvdLutAdaptiveLuma.cpp`: 100000 cases, 30 random LUTs x 4 x 256 gradient steps, fallback and temporal release: PASS.
- `SvdLutAdaptiveChroma.cpp` using extracted actual HLSL helper: 100000 random, 256 neutrals, 4097 ray probes; max code-luma error 3.03984e-05 code values: PASS.
- Static checks: one visible VE checkbox, retired mutable flag names absent, shader and analysis math unchanged after stripping comments. Model graph file not replaced. Windows property-page integration test updated for preview/apply/cancel/reactivation/failed save and absence of retired controls, NOT run here.
- Windows x64 Release, actual DirectML/HLSL compilation and MPC-BE playback, page tests and upstream-boundary script NOT run (no Windows build environment). Do not claim complete acceptance.

## Next validation
Build x64 Release and run `build-support/edition/tests/run-page-tests.ps1` and `check-upstream-boundary.ps1` with fixed upstream reference. Verify checkbox Off/On/Apply/Cancel/reopen, paused redraw, seek, switch video, close, MFG/SR combinations, old settings migration; compare former B1+B2.1+B2.2 @200% + Preferred/Skin to new VE. New combined perceptual output may differ from B2 alone because both perceptual functions are now fixed on. Inspect video buffer. Once accepted, commit as checkpoint, then research Adaptive Tone independently.
