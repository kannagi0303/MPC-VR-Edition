# 目前進度與接手

更新日期：2026-11-19。

## 目前狀態

使用者要求降低 Edition 與 MPC-VR 基底的耦合，並在第一批後同意分批重構。前三批完成後再要求「繼續」，目前第四批也完成：Render／Process 接點收斂、初始化／統計抽離及管線回歸測試。

採用獨立實作與具名接點；不改公開介面、設定佈局、演算法與持鎖契約。尚未升級官方基底，也不是可直接替換核心的外掛架構。

## 修改位置

- `Source/Edition/`：SR、深度顯示及前讀、單次影片管線、裝置／設定／Flush hooks、視窗呈現、功能狀態、DirectShow 排程與生命週期、設定遷移、設定頁與資源。
- 第四批新增 `DX11ProcessingSource.cpp`、`DX11Statistics.cpp`，並在既有 pipeline／lifecycle 模組實作 `PrepareEditionRender`、`InitializeEdition`、`BeginEditionShutdown`。基底 cpp 再減少 284 行；原生 HDR／轉色／縮放／字幕／Present 仍在原位置。
- `Source/DX11VideoProcessorJvr.cpp`：先前第一批抽出的 JVR。
- `build-support/edition/Edition.targets`：統一 Edition 建置項目、編譯參數、連結依賴與輸出名稱。
- `build-support/edition/tests/`：真實隱藏設定頁，以及直接擷取實際 C++ 方法的來源選擇／Render 準備測試；同一腳本可執行全部驗證。
- `build-support/edition/upstream-baseline.json`：已核對的官方參照 tag／commit；不代表 Edition ancestry 已驗證。
- 相關基底 cpp／h／rc／resource.h 保留必要接點。`Source/IVideoRenderer.h` 與 `Source/renbase2.cpp` 本輪完全未修改。
- `AGENTS.md` 為本機穩定協作規則，依既有 gitignore 不納入公開提交；內部交接與設計放 notes。

詳細入口、責任及持鎖契約：先讀 `notes/project-map.md`，再讀 `notes/upstream-integration.md`。

## 已驗證

- 三批增量建置及最後 x64 Release 完整方案 Rebuild 通過。MSVC 14.51.36231；紀錄 `_bin/edition-refactor-check/release-rebuild.log`。
- 第四批 x64 Release 增量方案建置與一鍵回歸腳本通過，紀錄 `_bin/edition-refinement-check/integration-tests.log`。
- 新增 1,024 組實際 `SelectEditionProcessingSource` 測試：來源優先順序、SR 開關、歷史重置、MFG 旁路／啟動策略、PTS／第二場與來源矩形。
- 新增 12 組實際 `PrepareEditionRender` 測試：optional 內的 S_OK 必須提前結束、縮放／目標失敗、已完成畫格重畫、presenter 擁有畫面、刷新率旁路、投影回退及成品幀 PTS。GPU／後端為替身，沒有執行真實 D3D11／NGX。
- 真實 `.ax` 設定頁測試：有／無 JVR 描述檔兩組通過，涵蓋預覽、Apply、Cancel、儲存失敗、重新啟用、無效選項、D3D11 gate 與隱藏 JVR 值保留。
- 新舊設定讀寫 12,000 組差異測試一致；86 種設定鍵，全部使用記憶體替身。
- 56 個抽離區塊比對、數字資源 ID、原始編譯項目 metadata 與輸出路徑檢查通過。
- 第四批另外六個抽離區塊，經明確的參數交接與縮排轉換比對一致。新快照在 `_bin/edition-refinement-check/before/`。
- `git diff --check` 通過。
- 完整建置仍有 2 個既有 C4018 比較警告及 vendor ONNX Runtime 標頭 C4819 字碼頁警告，未在純重構中改動這些行為。
- 沙箱內 MSVC 曾發生 C1902，取得工具允許後於沙箱外建置成功；沒有改專案工具鏈。

可重跑驗證：

```powershell
& .\build-support\edition\tests\run-page-tests.ps1
```

腳本使用現有本機依賴，建立隱藏視窗及隔離的 `.ax` 副本，並由實際 production 方法產生後端替身測試的 include；不下載、註冊、封裝或寫入真實設定。前三批差異測試與快照在 `_bin/edition-refactor-check/`，第四批驗證在 `_bin/edition-refinement-check/`。

## 尚待實機驗收與限制

- 未執行 MPC-BE 實播；一般 SDR／HDR、D3D9／D3D11、JVR／SR／MFG／ZipDepth／SVDLUT 組合，以及暫停、seek、EOS、縮放與影音同步仍待驗收。
- 官方參照 tag `0.11.1` 已唯讀核對，指向 `6be828bfc415aa72286862e6e262eb652abd9a53`；版本標頭為 0.11.1，本機宣告產品版號為 0.11.1.2636。Edition 的實際 ancestry 尚未核對，沒有升級基底或修改 remote。
- 共用類別與狀態仍在；更新上游時需合併既有 hooks 的位置及原生順序、截圖、`renbase2.cpp` 與建置基礎設定。Render 準備、Process 來源、初始化及統計已改用具名接點。不能只還原 Import 就覆蓋官方 Source。
- 未做 commit、push、部署、註冊、設定重設、外部依賴下載或全域 memory 更新。

## 歷史資料

- 第一批交接與設計已保留於 `notes/history/2026-11-19-edition-boundary-stage-one-handoff.md`、`notes/history/2026-11-19-edition-boundary-stage-one-design.md`。
- 前三批完成時的交接及設計另存於 `notes/history/2026-11-19-edition-boundary-stage-three-handoff.md`、`notes/history/2026-11-19-edition-boundary-stage-three-design.md`。
- 根目錄 `handoff.md` 保留既有功能交付歷史；`notes/completed-frame-buffer-plan.md` 保留成品緩衝的設計與過往實測。皆未刪減。
