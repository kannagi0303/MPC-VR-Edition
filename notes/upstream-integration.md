# MPC-VR 基底更新與 Edition 邊界

更新日期：2026-11-19。內部維護文件；入口見 notes/project-map.md，當前驗證見 progress-handoff.md。

## 目標與設計判斷

目標是降低接回新版 MPC-VR 的合併負擔。第五輪完成大段實作抽離後，重新審視發現：把 native 設定讀寫搬進 Edition，雖能減少 Source 的新增差異，卻讓 Edition 承擔原生邏輯。第六輪修正這項取捨，不以最小行數當作最小維護成本。

| 方向 | 採用方式 |
| --- | --- |
| 既有 virtual／override | 繼續使用既有來源交付、排程、EOS 等覆寫；不另加公開 API 或大改 protected |
| 原生責任 | 設定鍵表／讀寫留在 native VideoRenderer.cpp，僅一份 helper；原生資源／時序相容修正留在管理它們的原處 |
| Edition 策略與功能 | 獨立模組負責功能、設定擴充、產品策略；需要原生方法中途介入時使用具名接點 |
| 整份核心副本／文字替換／完整 PIMPL | 不採用：會藏起差異或牽涉 GPU 所有權、析構與介面改造，超過純重構範圍 |

這是目前採用的維護邊界，不宣稱數學上的最少行數或零修改更新。

## 第六輪責任修正

- VideoRenderer.cpp 的 private static ReadSettings／WriteSettings 同時服務啟動、SaveSettings 與預設集。原生 body 與官方版本直接比對，僅參數名稱／縮排、Edition hook 與 startup tuning 分工不同。
- 原生三項 registry-only tuning 只在建構時讀，沒有加入預設集或寫回。SaveSettings 仍在 RendererLock 內取快照，再於鎖外寫入；原有儲存驗證／HRESULT 保留。
- Edition/SettingsRegistry.cpp 維護獨有鍵、遷移及三個既有原生值政策覆寫（legacy VSR、chroma／upscale 無效值預設）。不複製 native 表。RendererSettings.cpp 保留 preset 路徑與 Edition 驗證；舊 TakeSettingsOver 仍未啟用。
- ApplyEditionSuperResPolicy 負責 Intel 排除與 NVIDIA legacy Always。SetSuperRes 先保留原生 context gate，再以 optional 接策略，接著是原生尺寸判斷／dispatch。有值包括 S_OK 立即回傳。VP input history／decoder／UAV 修正不一起搬走。
- 類別宣告片段仍按原位置引入；不改 Settings_t 佈局、IID／COM 順序、ID、PTS、鎖與 GPU fence 所有權。
- 復原官方 dormant ps_TEST.hlsl，但不啟用 TEST_SHADER；既有 8 個缺少的範例 shader 依現況記錄，沒有在本輪刪檔。

## 參照與差異範圍

官方 tag 0.11.1，annotated object 2d8d9de66b8c1d66987f9966fcc93a59991b0275，commit 6be828bfc415aa72286862e6e262eb652abd9a53。版本標頭為 0.11.1，本機產品版記載 0.11.1.2636；Edition ancestry 未證實，尚未更新官方基底。

| 範圍 | 檔案 | 一致／差異 | 新增／刪除差異 |
| --- | --- | --- | --- |
| Source | 78 | 47／31 | +1,231/-590 |
| Integration | 84 | 71／13 | +69/-456 |
| All | 162 | 118／44 | +1,300/-1,046 |

第五輪 Source 為 +1,048/-653；第六輪 native helper 回到原生檔案後新增差異較多。這不是功能增加或刪除，也不以它宣稱降幅。數字忽略 EOL／BOM／行尾空白及空行，指的是官方檔案差異，不是 repo 總規模。

Source manifest／contract 沿用 upstream-files.json／upstream-boundary.json；新增 upstream-integration-files.json／upstream-integration-boundary.json 包含官方 Include、Shaders、MpcVideoRenderer.sln，以及搬至 build-support 的 common.props、platform.props、update_revision.cmd。localPath 明記官方與本地路徑，allowMissing 只核准 8 個既有範例缺檔；變更或缺檔都必須有正確官方 Git blob 參照。

Source contract 的 roles 與 reason 區分：edition-seam、native-compatibility、settings-contract、edition-identity、resource-integration、build-integration。VSR 廠牌選擇是產品策略；decoder view／input history／等待與資源配置是原生相容修正，升級時須分開判斷。

Integration 差異包括版本識別、NGX／converter solution 接線、shader 專案的 PQ／Jinc／範例配置、IDE metadata 及 revision 腳本搬移／無 Git 處理。共用 props 內容與官方一致。

## 離線檢查

    & .\build-support\edition\check-upstream-boundary.ps1 -ReferenceRoot .\_bin\upstream-reference

預設 All；可用 -Scope Source 或 Integration。需要固定 commit 的官方檔案，以官方路徑存於 ReferenceRoot；本機已保留所需參照，不納入 Git。與 blob 已相同的本機檔不必另供副本。檢查器不下載、不修改 production／.git，產物寫入 _bin/upstream-audit/。

先驗證 manifest commit、相對路徑、重複本地映射及 Git blob，再檢查未審查路徑、非預期缺檔、差異限額。曾確認可拒絕遭竄改的 Source 與 Include 參照。不能自動提高上限來通過；須先檢視責任與行為。行數上限不會辨識所有等長語義改動，也不檢查所有 Edition 新檔、整棵 build-support、vendor／external 或 root Directory.Build.props。

## 更新基底步驟

1. 確認現在與目標官方 commit；獨立分支／worktree 比較，未經要求不 commit／push。
2. 用 project-map 與 contract 的 roles／reason 合併接點和相容修正。上游若已修正同一問題，先對照行為與測試，再決定移除舊修補。
3. 原生新設定合併到 native ReadSettings／WriteSettings 單一來源；Edition 只調整自己的 policy。保留 tuning 不進 preset／寫回、snapshot 持鎖與存檔錯誤契約。
4. 審查 Receive／Schedule／Flush／Stop／EOS 鎖外等待／取消、VP frame-order、PTS、成品幀及 fence 所有權。保留 optional 的有值語義，不能將 S_OK／LRESULT 0 當未處理。
5. 合併 targets Import、原位置 .inc、RC include，以及 Source 外的 Include／shader／建置映射。沒有只覆蓋 Source 並恢復 Import 的捷徑。
6. 完整 Rebuild x64 Release（含 converter），執行 build-support/edition/tests/run-page-tests.ps1，再以 MPC-BE 驗收 SDR／HDR、D3D9／D3D11、功能組合、暫停／seek／EOS／resize／同步。
7. 審查後更新新 commit 的 blob、契約與 notes，保留歷史；不能套用舊 commit 的限額。

## 驗證限制

已建置並驗證：原生／Edition 設定真實 body、預設集 roundtrip、tuning 邊界、缺鍵／錯值／拒寫、12,000 組舊新差異、95 個 ABI 斷言、31 組 VSR、1,024 組路由、12 組 Render 準備、真實隱藏設定頁、metadata／ID／官方差異。driver／GPU 使用替身，沒有真正建立 renderer 做播放或寫真實設定。

使用者於 2026-11-19 回報「可正常播放」，本輪重構據此結案。Codex 未自行執行實播；回報未列影片格式及功能組合，因此不推定所有 GPU 影像／長時間時序／同步案例均通過。完整矩陣留作後續升級回歸。既有 C4018／vendor C4819 警告未改。驗證產物在 _bin/native-settings-check/；前五輪完整記錄在 notes/history/。
