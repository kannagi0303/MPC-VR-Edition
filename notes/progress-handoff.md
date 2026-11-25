# 目前進度與接手

更新日期：2026-11-19。

## 結果與範圍

結案狀態：使用者於 2026-11-19 回報「可正常播放」，本輪架構重構依此完成結案。此為使用者實播回報，沒有提供影片格式、功能開關或測試矩陣，不推定所有 HDR／GPU 功能組合皆已驗證。

使用者要求持續降低 Edition 對 MPC-VR 的介入，並詢問繼承是否更容易；已同意混合使用既有虛擬覆寫、獨立策略與必要接點。第六輪完成原生設定所有權修正、VSR 產品策略分離與官方檢查範圍擴充。未新增公開虛擬介面、改 protected 或改物件所有權。

原生設定鍵／讀寫回到 VideoRenderer.cpp，由 private static ReadSettings／WriteSettings 同時服務全域及預設集；Edition 只保留自己的鍵與三個既有值政策覆寫。這樣避免原生邏輯外移到 Edition 後形成第二個維護來源。原生 reader／writer 已與官方內容直接比對，只有參數名稱／縮排、Edition hook 與 startup-only tuning 的刻意分工不同。

- Source：78 檔，47 一致、31 修改，+1,231/-590 行。第五輪為 +1,048/-653；本輪把原生讀寫帶回原生檔案，所以新增差異增加，沒有用行數宣稱進一步降低合併成本。
- Integration：Include、Shaders、solution 與指定搬移建置檔共 84 檔，71 一致、13 差異，+69/-456 行。
- All 預設合計：162 檔，118 一致、44 差異，+1,300/-1,046 行。8 個既有缺少的範例 shader 以 allowMissing 明記；恢復官方 dormant ps_TEST.hlsl，沒有啟用 TEST_SHADER。
- 比較基準仍是官方 tag 0.11.1 → 6be828bfc415aa72286862e6e262eb652abd9a53；不是 Edition ancestry 證明，也未實際升級基底。

## 入口與契約

- notes/project-map.md：模組、接點及持鎖地圖。notes/upstream-integration.md：本輪取捨、殘留分類及更新程序。
- Source/VideoRenderer.cpp：原生鍵表／讀寫、startup-only tuning、SaveSettings 快照與寫入協調；預設集直接使用相同 static 方法。
- Source/Edition/SettingsRegistry.cpp：Edition 鍵、遷移及既有 VSR／chroma／upscale 覆寫。RendererSettings.cpp 僅保留路徑與存檔驗證，舊遷移函式仍未啟用。
- Source/Edition/D3D11VideoProcessorBridge.cpp::ApplyEditionSuperResPolicy：Intel 排除及 NVIDIA legacy Always。native SetSuperRes 保留原尺寸門檻及 dispatch，先檢查 context 再呼叫 hook；有值（包括 S_OK）即回傳。VP 歷史／decoder／UAV 相容修正仍在原處。
- build-support/edition/Edition.targets：Edition 建置清單；原生 filters 維持官方。既有 .inc 保持原位置、成員／虛擬方法與析構順序。
- upstream-files／upstream-boundary 管理 Source；upstream-integration-files／upstream-integration-boundary 管理另外 84 檔。localPath 明記建置檔搬移；Source roles 區分接點、原生相容、設定契約、識別、資源及建置。
- SettingsLayoutContract.h：95 個 x64 ABI 斷言。新增 SettingsPersistenceIntegration.cpp：直接編譯／擷取 production 設定實作，非維護第二份演算法。

## 已驗證

- 最終 x64 Release 完整方案 Rebuild 通過，0 errors；仍有既有兩個 C4018、vendor C4819。紀錄 _bin/native-settings-check/final-rebuild.log。
- 一鍵測試通過：真實 .ax 隱藏設定頁（有／無 JVR）、1,024 組來源路由、12 組 Render 準備、新增 31 組 native VSR／Edition policy，以及原生／Edition 設定、預設集 roundtrip、registry-only 排除、舊值遷移及缺鍵／錯值／拒寫驗證。
- 新舊設定 12,000 組比對一致；原生讀寫與官方 body 比對通過。
- 121 個既有 build item metadata／輸出路徑、406 個資源 ID、原生 filters、最後視窗抽離 7 區塊及 ABI 契約通過。
- All／Source／Integration 差異守門均通過；刻意竄改 Include/Version.h 參照會被 Git blob 驗證拒絕。
- git diff --check 通過。未執行 commit、push、部署、註冊、重設設定、新依賴下載或全域 memory 更新；官方參照僅唯讀取得。

可重跑：

    & .\build-support\edition\tests\run-page-tests.ps1
    & .\build-support\edition\check-upstream-boundary.ps1 -ReferenceRoot .\_bin\upstream-reference

第二項預設 All；加 -Scope Source 或 Integration 可單獨檢查。參照不隨 Git 交付，需要固定官方 commit 的檔案，搬移檔以官方原路徑提供；每個變更／缺檔參照都驗證 Git blob。腳本不連網或修改 production。

## 未完成驗收與風險

- 使用者已回報正常播放；Codex 未自行執行實播。SDR／HDR、D3D9／D3D11、JVR／SR／MFG／ZipDepth／SVDLUT、暫停／seek／EOS／resize／同步的完整矩陣沒有逐項紀錄，留作未來升級與回歸時的驗收項目，不再作為本輪重構結案的阻擋。
- 仍共享原生類別私有狀態；沒有建立完整外掛 API。未來上游新增／修改設定時，仍需合併到 native helper，但不需同步 Edition 的原生副本。
- 守門只有固定的 162 個官方檔案，不涵蓋整棵 vendor／external、所有 Edition 新檔、整棵 build-support 或 root 工具鏈設定。行數相同的語義變更仍須 review。
- 工作期間另偵測到 tracked tools/zipdepth-convert.exe 變更，來源未確認，未覆寫或還原。初始 status 沒有此項；它不屬於本輪原始碼重構，後續 review 請分開處理。
- 不能覆蓋官方 Source 後只恢復 Import，也不宣稱已達絕對最少修改。

## 歷史與產物

- 2026-11-24 新增 `Export-Project-For-GPT.cmd`，雙擊後由 `build-support/export-project-for-gpt.ps1` 掃描目前工作區，將 ZIP 存在專案根目錄。保留程式、附屬資源和 notes 的相對路徑，排除建置產物、發行包、快取及模型二進位。規則集中於 PowerShell 腳本，不依賴 Git。

前五輪交接、設計及地圖保留在 notes/history/2026-11-19-edition-boundary-stage-*-*.md。第六輪開始快照在 _bin/native-settings-check/before/，測試／差異／官方 body 比對與 build 紀錄同目錄；更早快照在 _bin/minimal-boundary-check/、_bin/edition-refactor-check/、_bin/edition-refinement-check/。根目錄 handoff 與成品緩衝歷史未刪減。

## 2026-11-24 — SVDLUT Reference Pipeline Correction

Current reference pipeline changes and tests: `notes/svdlut-reference-pipeline.md` (authoritative for this round). Based on exact user archive 20261124-145114-817 and matching actual graph. Fixed point-downsample/extra thumbnail quantization, checked external tensor layouts, fused native dither at grade UNORM write. Portable input/CPU graph tests passed; Windows shader and x64 Release unverified. Existing architecture and B2/temporal/MFG decisions retained. Root `handoff.md` records previous integrated calibration plus this update. No commit or deployment.

## 2026-11-24 — B2.1 Adaptive Luma Participation

Current incremental work: `notes/svdlut-b21-adaptive-luma.md` and adjacent results JSONL. Preserves the previously delivered SVDLUT reference pipeline, replaces three-zone B2.1 gains with independent source-conditioned luma participation and gamut-safe chroma recomposition. User's three cards plus four natural photos tested offline against actual graph; Windows build/shaders/page tests pending. Cumulative overlay against original 20261124-145114-817 archive, no unshipped multi-constraint experiment, no commit or deployment.

## 2026-11-24 — B2.2 Source-Adaptive Chroma candidate

User reports actual B2.1 playback feels good and more suitable than A. Preserve B2.1 unchanged. Optional B2.2 candidate implemented, source-relative soft knee at unchanged B2.1 code-luma. See `notes/svdlut-b22-adaptive-chroma.md` and results JSONL. Incremental overlay against already applied B21 package; no new runtime dependencies. Portable scalar/actual graph tests passed; Windows compile/shader/page tests and real playback pending. No commit or deployment.


## 2026-11-24 16:39:20 +08:00 — Video Enhancement consolidation

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

## 2026-11-24 17:09 +08:00 — VE-08 Startup Coordination & Seamless Activation
User confirms consolidated VE/MFG work; startup settling can be smoother. Critical regression: after resizing stops, Ready Video Buffer previously slowly declined to zero then refilled despite sufficient processing capacity. Current baseline fixed this; preserve existing B08 resize-only bank recovery and display pacing estimator guards. See `notes/video-enhancement-startup.md` for exact changes, portable tests, limits, and mandatory delayed resize regression checks. Worker-side shader prep, cold-only VE admission, single pre-draw per frame, 250 ms media-time final fade; no new waiting, no buffer target or MFG scheduling changes. Windows build and real playback unverified.

## 2026-11-24 17:28 +08:00 — MFG-B08 normal-first resize recovery
Current candidate incremental against VE-08. See `notes/mfg-b08-seamless-recovery.md`; ordinary compose first, defer pending synthetic while Ready >= 250 ms, retain original-priority escape for low reserve after real resize even if delayed, reset on timeline clear. Independent early-drop diagnostic, no new waits/targets or edits to estimator, VE and NGX. Preserve original frames and delayed nonzero buffer; 5 s startup skip still needs attribution. Portable policy/VE/color tests passed; Windows playback not verified.

## 2026-11-24 17:51 +08:00 — MFG-B09 geometry-independent producer admission
User reports extremely dense alternating resize still drains Video Buffer to zero; previous B08 improves robustness but is not a verified root fix. Exact B08 overlay used. Remove unconditional geometry-only overlap blocking for matching fixed video-only pending batches; preserve all actual bank/fence/deadline/budget restrictions and legacy/mismatched geometry guard. Presenter prioritizes imminent frame over deferred resize commit, with old-buffer mapping during pending resize; existing 140 ms coalescing and 500 ms display estimator/quality guard remain. B08 emergency reserve and VE-08 unchanged. Existing opt-in cadence trace measures resize commit duration and failures. Portable production-method extraction tests (100,000 cases, 500 geometry-transition opportunities, ASan/UBSan) and prior B08/VE/color regressions pass. Windows compilation, real driver timing and actual high-density/delayed-drain acceptance pending. See `notes/mfg-b09-geometry-decoupling.md` for detailed source evidence, tests and next diagnostic direction. No commit/push/deployment.

## 2026-11-24 23:56 +08:00 — MFG-B10 calculation verification (diagnostic-only)
B09 improved ordinary resizing. Dense 4–5 toggles in 1–2s still trigger slow drain to displayed 0 then refill; playback smooth; DXGI n/a appears associated, not confirmed. Actual B09 buffer functions reproduce identical qualitative drain with a single injected duplicate due, 121 queued/signaled throughout, instant rebound when pair leaves (not measured actual refill). 100k buffer cases + sanitizers, 100k DXGI classification equivalence, B09 regression PASS. Added only diagnostics (Signaled horizon, cutoff, DXGI reason, optional CSV launcher) with unchanged buffer math and all policies. See `notes/mfg-b10-buffer-calculation-audit.md` for exact trace mapping and next decisions. Windows build/runtime pending; no root fix claim.

## 2026-11-25 09:55 +08:00 — MFG-B11 Presentation / Producer Scheduling Isolation
B10 real CSV establishes sustained actual depletion rather than Ready-prefix miscalculation: 37.764783–39.201240 s without compose, 85 presents, 84 >= half refresh, two waiting batches, first subsequent compose 39.202223 s. Brief second empty during refill at 39.257527 s. New candidate provides normal one-batch service after measured slow successful present, bounded permit retry/one-credit source head-gate and actual source lock waiter handoff on existing timer. Retains existing lock scope, B08/B09, B10 diagnostics, VE and all PTS/budget/ownership guards; no forced retirements added. Portable extraction and conditional scheduling simulation PASS; Windows build/playback unverified. See `notes/mfg-b11-presentation-producer-fairness.md` for CSV details, chosen defaults, tests and next acceptance. No commit/push.

## 2026-11-25 10:11 +08:00 — MFG-B12 bounded settled flip expansion
User confirms B11 is the strongest resize stress result so far (almost no buffer decline); screenshot shows 93% ready, 10 s low 88%, 113/115 ready/queued frames, MFG 59 fps, DXGI disjoint while video/OSD occupy the upper-left part of fullscreen. A short visible delay remains. Keep B11 unchanged. B09's resize deferral plus DXGI_SCALING_NONE can leave the smaller committed backbuffer in the enlarged client when a due head repeatedly wins. B12 keeps 140 ms last-event coalescing and prefers the original safe gap, but after a further 50 ms a settled flip expansion becomes eligible even if the head is due. No new timer, queue, GPU pass, unlock or forced frame retirement; uses existing ApplyPendingWindowResize, failure retry and serialized ownership. This is a scheduling eligibility bound, NOT a guaranteed screen-update latency. See `notes/mfg-b12-bounded-flip-expansion.md` for source analysis and tests; verify on Windows that fullscreen fills promptly without restoring delayed drain or cadence regression.

## 2026-11-25 — AT-01 / WB-01
B12 accepted as stable by user (occasional short top-left transition tolerated); VE complete for now. Independent AT offline candidate and WB literature review in `notes/adaptive-tone-at01-research.md`, tests and CSV alongside. Research-only overlay, no runtime, no Windows validation. Preserve B11/B12; WB not yet selected or implemented.

## 2026-11-25 11:35 +08:00 — AT-03 / WB-03
AT-01 user compile pass, AT-02 research only. AT-03 first runtime candidate (default off, independent SDR analysis/shot anchor and final composition); WB-03 nonlinear mapping capacity research only. See `notes/adaptive-tone-at03-white-balance-wb03.md` for details. Portable regression tests pass; Windows/HLSL/page/upstream-boundary and actual playback pending. Preserve B12 and VE math, no commit/push. Keep known high-key guard and fallback limitations explicit; WB no pretrained candidate chosen.

## 2026-11-25 12:03 +08:00 — AT-05
AT03-FIX01 user real playback effective but too strong. User declines A/B comparison; adopt AT04-B and conservative SVDLUT/B2 -> AT -> joint Preferred/Skin order. VE-only preserved, independent fade, no new setting or GPU pass. Keep B12 stable and WB research-only. See `notes/adaptive-tone-at05-midtone-composition.md` for exact changes, executed tests and unresolved Windows/GPU validation. No commit/push.

## 2026-11-25 12:20 +08:00 — AT-06 Enhancement Continuity
User confirms AT-05 acceptable, sees staggered feature activation after seek/resize. Preserve AT-B and VE order. Known pressure sampling gap no longer resets established AT unless detected cut; explicit timeline reset remains. Optional bounded initial anchor lookahead from ready original frames, no extra waits; existing simple resize draw can apply AT in one pass on final target only. Keep B11/B12, buffer and original-frame protections. Production C++ shot/priming extraction and prior portable regressions pass; Windows compilation, actual HLSL and real playback pending. No guaranteed simultaneous activation. See `notes/adaptive-tone-at06-enhancement-continuity.md`. WB-03 research-only.

- 2026-11-25 12:58 +08:00: AT-06 user accepted; WB-06 first independent SDR AWB runtime candidate (Gray Edge + fixed shot anchor; user 0–100% strength; matching pre-VE color basis and LUT epoch). See `notes/white-balance-wb06-implementation.md` for all tests, caveats and next steps. B11/B12 untouched; Windows compile/runtime pending.

## 2026-11-25 14:09 +08:00 — FA-01 independent perceptual lifecycle
User observes late skin after seek (memory colors likely same), same-shot AWB jumps and late cuts; rejects cross-cut artificial smoothing. Separate stateless perceptual shader from model admission/readiness; one cached CPU compile independent of DirectML, LUT-only startup fade before AT and perceptual stage. Preserve full strength formula, existing scaled/legacy cold guards, B12/AT/WB. Portable routing and arithmetic tests pass; Windows build, HLSL and real playback pending. Shared analysis, context/shot-aware WB and AT are design only. See `notes/frame-analysis-fa01-perceptual-lifecycle.md`.

## 2026-11-25 — FA-02 current candidate
For the latest work since the older 11-19 checkpoint above, read root `handoff.md` and `notes/frame-analysis-fa02-adaptive-startup.md`. FA-01 baseline; user reports 7–8 s cold startup, wants adaptive resource allocation. CPU bytecode preparation/caching and optional-work arbitration implemented, existing GPU admission and color equations unchanged. Linux portable tests pass; Windows build/playback/startup times pending. Do not treat this as unified scene/analysis completion. Follow AGENTS.md current guards.

## 2026-11-25 — FA-03A Source-frame AT/WB (partial)

Actual source composition in independent AT/WB mode (VE off), Completed route; source-only no fixed AT fade, originals and generated descendants carry baked-colour marker. Existing VE and non-Completed path preserved until upstream lookahead can guarantee matching source signature/WB epoch/LUT. See `notes/frame-analysis-fa03a-source-tone.md`. Portable tests passed as detailed there; Windows build and real playback unverified. No commit/deployment. Source-stage settings affect future originals, not already baked buffers. Keep B11/B12 admission and original protection.

## 2026-11-25 15:12 +08:00 — FA-03B
FA-03A user compile/test accepted; startup deferred. Independent AT/WB with VE off now directly grades into existing native-size FP16 SR input on eligible Completed originals, replaces plain copy without added pass, preserves raw analysis/depth, propagates final color decision on actual SR success. VSR-first unchanged; existing MFG-first native path is already upstream. Combined VE remains old path until bounded raw-source lookahead is available. No scheduler/water target changes. Portable extracted SR (2048) and prior regressions passed; Windows/FXC/GPU pending. See `notes/frame-analysis-fa03b-source-before-sr.md`, `handoff.md`.

## 2026-11-25 — EXP-GEO-0 experimental post-resize guard removal
User accepts FA-03B, observes color recovery after resize and authorizes an experiment to remove only the 500 ms post-event geometry hold. `CompletedGeometryChanging()` now returns pending only; B08/B09/B11/B12/FA-02 and 140 ms debounce remain unchanged. All applicable portable policy tests passed, including actual predicate integration and B12 retry. Existing geometry tick retained for rollback. This changes display-pacing sample eligibility too; post-resize p95 inflation and delayed Ready drain are the main regression risks. Windows compile/real playback and boundary PowerShell not run. See root `handoff.md` EXP-GEO-0 for exact tests and acceptance; do not consider settled until real dense resize/cadence/color/fast-exit validation. No commit/push.

## 2026-11-25 — EXP-FADE-0 experimental fixed activation ramp removal
User reports EXP-GEO-0 has no problem, notices remaining fade, authorizes removal of artificial activation fades. SVDLUT fused and B2 fallback now bind 1.0 at first valid selected LUT, without waiting for first-present timestamp; activation bookkeeping and cold admission unchanged. AT source and legacy share binary eligibility, preserving active/locked/activation PTS and existing anchor/cut/reset policies. Source-only FA-03A/B already used this policy. No changes to WB, preferred/skin, LUT/B2 math, temporal selection, depth confidence, geometry or MFG scheduling. This is a separate incremental overlay against EXP-GEO-0; real GPU/visual acceptance pending. See root `handoff.md` for verification and rollback scope.

## 2026-11-25 — Edition 0.2.3 / UI and color consolidation

**Base:** user reports EXP-GEO-0 + EXP-FADE-0 playback excellent, WB accuracy only remaining problem; user elects to retire WB and approves separate Color Enhancement, Memory Color, Skin Tone, Adaptive Tone 0–100% (5% UI steps) sliders, two tabs, then asks implementation and a version bump. Edition version 0.2.2 -> 0.2.3; upstream 0.11.1.2636 unchanged.

- `CVRMainPPage` owns one settings transaction. Native Win32 tab control switches visibility of the two groups of sibling controls; no separate COM property page or settings snapshots. Dialog 352 x 302 DLU (was 352 x 468); original order preserved, Extended groups Processing / Video Enhancement / diagnostics / optional VR. Shared footer; Default scoped to current tab. Four strengths preview on slider release/keyboard/channel action, Apply saves, Cancel rolls back to last successful Apply. Existing global strengths stay outside Settings_t, ABI/layout unchanged. `IDC_VIDEO_ENHANCEMENT` and `IDC_ADAPTIVE_TONE` reused as trackbars, WB IDs retired.
- New DWORD keys ColorEnhancementStrength, MemoryColorStrength, SkinToneStrength, AdaptiveToneStrength. New keys take precedence; old VideoEnhancement bool migrates to all three color strengths 0 or 100, old AdaptiveTone bool to 0 or 100. Very old grade/preferred/skin migration retains former combined enable logic. Fresh/default settings 100 each. Legacy bool config APIs remain compatible; int channels accept 0..100, UI step 5. Old AutoWhiteBalance registry entry ignored (not automatically deleted, to preserve rollback).
- Runtime WB removed: no setting, UI, active estimator, deferred samples, gain, epoch reset, model input correction, shader constants, stats or WB scheduling priority. Original SDR samples feed model and AT. Historical WhiteBalance.h/WhiteBalanceShader.h and WB-only offline research/test files retained as unreferenced research material; not compiled or used by renderer. Overwrite ZIP does not delete historical files.
- Color grade strength applies after calibrated B1/B2.1/B2.2, before AT and preferred/skin. D1 subtle far-depth dim assigned to Color Enhancement, scaled with its percentage. Memory Color and Skin Tone use independent weights in existing joint shader (including 5%); neither needs SVDLUT. All zero bypasses color paths; Color=0 prevents model prefetch/start and zero crossing releases model via existing fast path. AT uses existing curve weight, no fade. Nonzero changes preserve model/shot state; only invalidate prepared backbuffer. Pure grade fallback uses calibrated B2.1/B2.2 without hidden legacy memory/skin, so disabled sliders stay disabled. AT-only FA-03A/B continues upstream only when all three other sliders off. Already baked AT frames remain in buffer until consumed; changing source-stage strength while paused may not recolor an already baked frame. Existing scaled standalone pressure guards remain.
- No change to B08–B12 policies, water target, original protection, present pacing, EXP-GEO-0 or fixed activation ramps. No extra full-size pass for independent controls. MFG legacy fallback observes AT/grade only on originals, preserving normal completed path.
- Portable verification: PerceptualLifecycle (1024 route cases), SourceToneRouting (512 plus independent exclusions/strength), SourceToneBeforeSR (2048), AdaptiveToneContinuity, AdaptiveToneComposition (100000 prior composition + 100000 strength/bypass plus hue examples), OptionalColorWork (4096 prime + 256 preparation), ResizeGeometryAdmission (100000 + B12), PresentationProducerFairness, CompletedBufferCalculation (100000), DxgiStatisticsAudit (100000); all run and passed. Real Win32 page integration and shader-reflection tests updated but NOT executed; Linux has no MSBuild/FXC/PowerShell. Upstream boundary script not run; review actual native diffs before adjusting thresholds if needed. Not a Windows build/GPU validation. No commit/push.
- Windows acceptance: build x64 Release, run `build-support/edition/tests/run-page-tests.ps1`, `check-upstream-boundary.ps1`, shader reflection harness; tab visibility/focus/DPI/footer/JVR with/without profiles, independent 0/5/50/100 and combinations, save/reopen/cancel/default per tab, compare 100% to WB-off stable base, pause/seek/cut, repeat dense resize and observe water level >=60 s, fast exit. GPU load and visual strength changes require real playback. If issue, preserve last accepted EXP-FADE-0 as rollback baseline.

## 2026-11-25 — Edition 0.2.3 FIX01 (Windows link)

User x64 Release build log reaches final renderer link; `PropPage.obj` reports LNK2001 `__imp_InitCommonControlsEx`, followed by LNK1120. New native tab initialization calls this Win32 API. `EditionPageIntegration.vcxproj` already links comctl32.lib, but the actual renderer did not. Add `comctl32.lib` to `build-support/edition/Edition.targets` Link/AdditionalDependencies for all configurations; preserve NVIDIA and inherited dependencies, no changes to upstream vcxproj. Also fix both C4018 warnings in `Source/Edition/DX11FeatureStatus.cpp` by checking positive signed processing dimensions before casting to UINT for VSR badge comparison. Normal valid-size semantics unchanged; invalid dimensions do not falsely indicate upscale. The vendor ONNX Runtime C4819 code-page warning is unrelated/nonfatal and left unchanged. Version remains 0.2.3. No UI, settings, rendering, MFG, shader or scheduling changes.

Actual local checks: MSBuild XML parses, real renderer import precedes Cpp.targets, dependency and inherited libraries present; extracted actual C++ VSR badge predicate compiled with g++ -Wall -Wextra -Wsign-compare -Werror and passed 9 valid/equal/downscale/zero/negative/large source-size assertions. Windows MSBuild/link, page tests, boundary script and runtime not executed in Linux; user should overlay FIX01 on existing 0.2.3 and rerun build-x64.cmd. No commit/push.

## 2026-11-25 — 0.2.3 FIX02
FIX01 user compile success and screenshots; approved native two-column layout, editable percent before slider, common background, master enable, diagnostic and VR groups. Implemented requested-vs-effective strengths with durable master, 1% inputs and preview/Apply/Cancel. No version bump or rendering algorithm change; existing zero-crossing model/AT lifecycle and baked source frame caveat remain. See root `handoff.md` FIX02 for exact implementation and actual tests. Windows build/page/DPI/visual acceptance and boundary script pending.

## 2026-11-25 — 0.2.3 FIX03
User approves true persistent child dialogs after FIX02 per-control tab repaint and notes upstream maintainability. Native and Edition panes now have separate dialog resources, whole-pane batched visibility, native background, common settings transaction, actual control-parent routing and message forwarding. No renderer changes. Portable checks pass; updated real Windows page harness includes 100 switches/unchanged HWNDs/no settings writes, not run in Linux. Windows build, visual/keyboard/DPI, page tests and upstream-boundary checks pending. See root handoff.md FIX03. No version bump or commit.

## 2026-11-25 — 0.2.3 FIX04
User reports overlapping tab paint on FIX03 (screenshots). Preserve persistent child dialogs and shared settings; sequential hide, raise/show selected pane, explicit full incoming-pane erase/redraw using system color. Tests for mutually exclusive visibility/z-order on every transition and memory-DC erase added to Windows harness; not yet run. Portable checks passed; real Win32 drawing/build and upstream boundary checks pending. See root handoff.md FIX04. No playback changes or commit.

## 2026-11-25 — 0.2.3 FIX05 (tab group-background clipping)

User reports FIX04 still overlaps and challenges irrelevant verification. FIX03/04 pane resource styles both include WS_CLIPCHILDREN. Group boxes are transparent sibling controls occupying large rectangles; a real pane paint DC excludes these rectangles when WS_CLIPCHILDREN is set, unlike FIX04's unclipped memory DC. This is a concrete resource/style defect consistent with screenshot distribution, not yet a confirmed complete Windows runtime diagnosis. Microsoft references: https://learn.microsoft.com/en-us/windows/win32/gdi/window-regions and https://devblogs.microsoft.com/oldnewthing/20181005-00/?p=99905%2F .

Production change: remove WS_CLIPCHILDREN from IDD_EDITION_RENDERER_PANE and IDD_EDITION_EXTENDED_PANE only; retain WS_CLIPSIBLINGS, DS_CONTROL, WS_EX_CONTROLPARENT, persistent HWNDs, outer page WS_CLIPCHILDREN and FIX04 sequential pane switching/erase. Clarify the erase handler comment. No control coordinates, IDs, layouts, settings, processing or rendering changes. Resource bytes verified against FIX03 (unchanged in FIX04) except exactly one flag deletion in each; old style fails new static contract; new style passes. Only static UI contract suffix executed, not unrelated MFG or color tests.

Removed the misleading 8x8 memory DC erase check. Added opt-in real visible property-page test: `powershell -ExecutionPolicy Bypass -File .\build-support\edition\tests\run-page-tests.ps1 -VisualTabsOnly`. Builds renderer and page harness only, opens an isolated visible topmost test page with in-memory renderer, sends actual mouse messages through native tab control 40 transitions per profile configuration, pumps WM_PAINT, checks exclusive pane visibility and stable HWNDs, acquires GetDCEx(DCX_USESTYLE) and probes unobstructed group interior pixels against system background and real clipping. Tests with/without VR profile; requires unlocked interactive Windows desktop, sufficient display work area, no overlay. It samples actual screen pixels, not full screenshot equality, and must not be presented as full visual coverage. No settings writes to user registry or filter registration. Native Win32 build, focused test and upstream boundary PowerShell not run in Linux (no MSBuild/Windows); user MPC-BE screenshots/visual acceptance still required. No claim problem solved until actual validation; if still broken, collect live HWND visibility, update/visible region and real painting trace before further changes. Do not reintroduce per-control tab switching or unrelated guards. Version stays 0.2.3; no commit/push.
