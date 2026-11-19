# 目前進度與接手

更新日期：2026-11-19。

## 目前任務

使用者希望 Edition 盡量少改動 MPC-VR 基底，降低未來基底更新後的接回成本。採用獨立功能實作檔與明確整合接點，分批處理；全面重構涉及大量核心檔案，須依使用者規則先確認。

## 已完成

- 從 `Source/DX11VideoProcessor.cpp` 原樣抽出 JVR 六個函式（141 行）至 `Source/DX11VideoProcessorJvr.cpp`。
- 新增 `build-support/edition/Edition.targets`，集中 Edition 既有 30 個建置項目、個別編譯參數、NVIDIA 連結依賴與 x64 輸出名稱；補上 JVR 編譯與 IDE 清單。
- 原有編譯項目的 MSBuild metadata 與輸出路徑比對通過。
- 詳細設計選擇、剩餘接點與更新程序見 `notes/upstream-integration.md`。

## 驗證狀態

- x64 Release 完整方案 `Build` 通過，renderer、NgxShim、converter 與依賴均完成。這是增量方案建置，沒有執行乾淨 Rebuild。
- 沙箱內首次建置發生 MSVC PDB 管理員 `C1902`；同一建置命令在取得工具允許後於沙箱外成功。使用 MSVC 14.51.36231、`/m:1 /nr:false`，沒有調整專案工具鏈。
- 仍有兩個既有 C4018 正負號比較警告，位於未修改的統計繪製程式碼。成功紀錄：`_bin/edition-isolation-check/build-unsandboxed.log`。
- JVR 六個函式與 HEAD 原始內容逐字比對通過（正規化換行）。`git diff --check` 通過。
- 沒有進行播放器實播。
- 沒有 commit、push、部署、註冊 renderer 或下載外部依賴。

## 下一步與限制

取得全面重構授權後，依序處理 SR／深度顯示、Edition 設定與 UI、最後是來源交付及呈現生命週期。每批保持既有演算法、設定佈局及持鎖契約，分別建置與驗證。

目前只是建立可延伸的分離方式，仍有核心檔案耦合；尚不能宣稱可直接替換 MPC-VR 基底。官方基底精確 commit 也尚未核對。

## 歷史資料

既有 `handoff.md` 保留 JVR、SVDLUT、刷新率旁路等功能交付歷史；`notes/completed-frame-buffer-plan.md` 保留成品緩衝的設計及過往測試紀錄。這兩份沒有搬移或刪減。
