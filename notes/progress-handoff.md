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
