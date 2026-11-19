# MPC-VR 基底更新與 Edition 邊界

更新日期：2026-11-19。內部設計及維護文件；入口地圖見 notes/project-map.md。

## 採用方案與停止條件

目標是降低接回新版 MPC-VR 的合併成本，保留現有功能與執行契約。

| 路線 | 判斷 |
| --- | --- |
| 獨立實作＋具名接點＋原位置宣告片段 | 已採用。讓差異可定位，保留類別、狀態與生命週期 |
| 複製核心／建置時文字覆寫 | 會把差異藏進副本或腳本，升級仍須維護兩份核心，不採用 |
| 完整外掛 API／PIMPL／獨立 renderer | 需改公開契約、GPU 所有權及析構順序，超過本次純重構範圍 |

大段 Edition 行為已移出官方檔案，留下必要執行接點、宣告及原生相容修正。繼續為每行建立包裝不會可靠降低更新成本；變更所有權或虛擬介面則超出純重構。這是目前限制下的合理最小邊界，不宣稱數學上的最少行數或可零修改更新。

## 第五輪收斂

- 設定值／Settings_t 擴充及預設集宣告、DX11／renderer 狀態與方法，改由 Edition 的 .h／.inc 在原位置引入。沿用原順序，沒有狀態副本或改 COM IID。ABI 契約固定 x64 大小及 94 個欄位 offset。
- RendererSettings／RendererPresets／RendererControls／RendererSourceConnection／RendererWindow 分別負責持久化、預設集、控制、來源連接及視窗事件。設定維持原呼叫端持鎖，依序正規化→Configure→UI／暫停重畫。
- VideoShaderExtensions、D3D11VideoProcessorBridge、DX11SubtitleCompatibility、VideoProcessorStatistics 收納既有擴充。CopySample 分析、樣本 discontinuity、processor info 使用具名接點。
- Edition ID 與設定 dialogs 集中；原生 .vcxproj.filters 還原官方版本，建置清單只維護 targets。
- 還原無關的 dormant TEST_SHADER 官方區塊。不更動演算法、設定鍵／ID、PTS、等待／取消、鎖範圍或 GPU fence 所有權。

## 官方參照與差異契約

官方 tag 0.11.1 的 annotated object：2d8d9de66b8c1d66987f9966fcc93a59991b0275，commit：6be828bfc415aa72286862e6e262eb652abd9a53。版本標頭為 0.11.1，本機記載完整產品版號 0.11.1.2636。已透過 GitHub 公開唯讀內容核對；Edition ancestry 尚未證實，沒有升級基底。

upstream-baseline.json 記版本來源；upstream-files.json 固定官方 Source 範圍 78 檔案的 Git blob。upstream-boundary.json 列出 31 個仍修改的檔案、原因及新增／刪除行數上限。數字忽略 CRLF／UTF-8 BOM、行尾空白及空行：

| 範圍 | 本輪開始 | 第五輪結束 |
| --- | --- | --- |
| 官方 Source 新增差異 | 3,952 | 1,048（減少約 73.5%） |
| 官方 Source 刪除差異 | 583 | 653 |
| 與官方一致／仍修改 | 46／32 | 47／31 |

這是官方檔案差異量，不是 repo 總行數或功能刪減比例。移出程式仍由 Edition.targets 編譯。每項殘留理由以 manifest 為準，主要是：

- DX11 樣本／處理／Render／Present／生命週期／統計接點及宣告，原生 D3D9、VP、格式與資源配置相容修正。
- Renderer 設定／來源／視窗接點，DirectShow 時間／排程／EOS 虛擬擴充及 input pin 互動。
- Settings／介面 include、UI 預覽接點、RC／ID include、binary identity 及建置 Import。
- 既有 shader／字幕／幾何／時序等原生修正。不能只靠搬移消除語義差異。

## 離線檢查

    & .\build-support\edition\check-upstream-boundary.ps1 -ReferenceRoot .\_bin\upstream-reference -ReportPath .\_bin\minimal-boundary-check\upstream-report.json

ReferenceRoot 下須有固定 commit 的 Source/... 官方來源。本機已保留所需參照檔，不納入 Git。若本機檔已與官方 blob 相同，不必另提供參照副本。檢查器不連網、不修改 production 或 .git；產物放 _bin/upstream-audit/。

先驗證 manifest commit 及參照 Git blob，才比較差異；拒絕缺失／錯誤參照、未審查路徑及超出上限。未涵蓋 Include、Shaders、build-support、vendor／external 等樹，不是全 repo 升級安全檢查。行數上限不能偵測所有同長度語義變更，仍須 review 與測試。

本輪曾觸發 VideoRenderer.cpp 上限失敗：視窗抽離讓 native 刪除差異增加，核對原流程及 7 個抽離區塊後才收緊為最終限額。新增功能應擴充 Edition 模組，不可自動提高上限通過。

## 更新基底步驟

1. 確認目前及目標官方 commit，在獨立分支／worktree 比較；未經要求不 commit／push。
2. 對照 project-map 及 boundary manifest，合併每個接點與原生修正。保留 targets Import、原位置 .inc、RC include、ABI／方法順序。
3. 特別審查 Receive／Schedule／Flush／Stop／EOS 的鎖外等待與取消，optional（S_OK／LRESULT 0 仍是有值），VP frame-order、PTS、完成幀及 GPU fence 所有權。
4. 同時審查 Source 清單外既有版本、shader、Include 介面、依賴及 build-support 變更；不能覆蓋 Source 後只恢復 Import。
5. x64 Release 完整 Rebuild（包含 converter），執行 build-support/edition/tests/run-page-tests.ps1。ABI 合法演進須先決策再更新契約，不可盲改斷言。
6. MPC-BE 實播 SDR／HDR、D3D9／D3D11、JVR／SR／MFG／ZipDepth／SVDLUT，涵蓋暫停、seek、EOS、縮放、切換功能及影音同步。
7. 審查後更新官方 manifest／限額與 notes，保留舊決策；不可將舊 commit 限額直接套在新 commit。

## 證據與限制

建置／回歸摘要見 progress-handoff.md。產物在 _bin/minimal-boundary-check/，設定差異測試在 _bin/edition-refactor-check/；過往實播歷史不是本輪實播證據。

已驗證最終 x64 Release 完整 Rebuild、真實隱藏設定頁、1,024 組來源路由、12 組 Render 準備、12,000 組記憶體設定讀寫、ABI／預設值、建置 metadata、資源 ID、抽離區塊及官方差異。GPU 測試用替身，未執行 MPC-BE 實播、影像比較或長時間時序／同步驗收。既有 C4018／vendor C4819 警告保留。
