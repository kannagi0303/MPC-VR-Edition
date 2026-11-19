# 目前進度與接手

更新日期：2026-11-19。

## 結果與範圍

使用者要求「繼續重構，直到架構對原本介入最少」。第五輪已完成：以精確官方 Source 比對收斂，將設定／預設集、控制、來源連接、視窗事件、shader、VP bridge、字幕相容及類別宣告抽至具名 Edition 模組。採用獨立實作＋明確接點，沒有建立整份核心的影子副本。

在保留行為、ABI、鎖與 GPU 所有權的限制下，已達本輪可合理收斂的邊界。不是零差異外掛，尚未實際升級官方基底。

- 官方 Source 78 個檔案：47 個一致、31 個有理由的修改；新增／刪除差異為 +1,048/-653 行。
- 本輪開始的同範圍為 +3,952/-583 行；新增差異減少約 73.5%。只計官方 Source，不把搬出的程式算作消失，不代表整個 repo 的降幅。
- upstream-files.json 固定官方 Git blob；upstream-boundary.json 記錄每個殘留差異理由及行數上限；檢查器拒絕未審查檔案、上限擴大及錯誤參照。
- 原生 .vcxproj.filters 恢復官方內容；Edition 編譯項目只維護 Edition.targets。IDE 匯入項目可能顯示未分類，未做視覺驗收。

## 入口

- notes/project-map.md：模組、接點及持鎖地圖。
- notes/upstream-integration.md：設計判斷、殘留差異及更新步驟。
- Source/Edition/：功能、設定／控制／視窗／連接、宣告片段、UI 資源及 shader／VP 擴充。
- Source/DX11VideoProcessorJvr.cpp 及既有 Mfg／DLSS／成品緩衝模組保留具名獨立實作。
- build-support/edition/Edition.targets：全部 Edition 建置增量；不匯入 converter。
- build-support/edition/tests/SettingsLayoutContract.h：95 個靜態斷言，固定既有 x64 Settings_t 大小及 94 個欄位 offset。
- 本機 AGENTS.md 為 gitignored 穩定規則；內部交接放 notes，沒有寫入公開 docs。

## 已驗證

- 最終 x64 Release 完整方案 Rebuild 通過；紀錄 _bin/minimal-boundary-check/final-rebuild.log。MSVC 14.51.36231，/m:1 /nr:false。
- 真實 .ax 隱藏設定頁：有／無 JVR 描述檔兩組，涵蓋預覽、Apply、Cancel、儲存失敗、重啟、無效選項、D3D11 gate 及隱藏 JVR 值保留。
- 實際 C++ 方法搭配 GPU／後端替身：1,024 組來源選擇及 12 組 Render 準備通過；檢查 optional 內 S_OK、來源回退、PTS 及失敗分支。
- 設定 ABI／預設值快照比對通過；新舊設定讀寫 12,000 組一致，全程記憶體登錄替身。
- 121 個既有編譯／標頭項目的 metadata、輸出路徑、406 個數字資源 ID、原生 filters 及最後 7 個抽離區塊一致性檢查通過。
- 官方 Source 差異守門通過；曾實際攔下最後抽離造成的未審查行數變化，核對後才更新限額。另以刻意竄改的參照副本確認 Git blob 驗證會拒絕，production 未變動。
- git diff --check 通過。既有 2 個 C4018 及 vendor C4819 警告保留。
- 未做 commit、push、部署、註冊、重設設定、下載新依賴或更新全域 memory；僅唯讀取得官方參照原始碼。

可重跑：

    & .\build-support\edition\tests\run-page-tests.ps1
    & .\build-support\edition\check-upstream-boundary.ps1 -ReferenceRoot .\_bin\upstream-reference

第二項需要固定 commit 的官方來源；本機參照在 _bin/upstream-reference/Source/，不隨 Git 交付。檢查器離線驗證 Git blob，不下載或修改 production。設定差異測試與快照為當輪暫存驗證，位於 _bin/edition-refactor-check/；第五輪產物在 _bin/minimal-boundary-check/。長期入口是 build-support 下腳本及 ABI 契約。

## 尚待驗收與風險

- 未執行 MPC-BE 真實 GPU 播放：一般 SDR／HDR、D3D9／D3D11、JVR／SR／MFG／ZipDepth／SVDLUT，以及暫停、seek、EOS、縮放與影音同步仍待驗收。
- 官方參照 tag 0.11.1 → 6be828bfc415aa72286862e6e262eb652abd9a53 已核對；Edition ancestry 未證實。
- 類別共用狀態仍存在；.inc 必須在原位置展開，不可重排欄位、虛擬方法或析構順序。未更換 COM IID／方法順序。
- 殘留修改含原生 D3D11／D3D9／DirectShow 相容修正、時序及幾何；不能覆蓋官方 Source 再只恢復 Import。
- Source 守門不涵蓋 Include／Shaders／build-support／external 等整棵 repo。未來仍須審查這些既有版本／shader／依賴變更。

## 歷史

前四輪交接、設計及第四輪地圖已保留在 notes/history/2026-11-19-edition-boundary-stage-*-*.md。本輪開始快照在 _bin/minimal-boundary-check/before/，較早快照在 _bin/edition-refactor-check/、_bin/edition-refinement-check/。根目錄 handoff 與成品緩衝計畫未刪減。
