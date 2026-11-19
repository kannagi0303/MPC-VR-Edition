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

前五輪交接、設計及地圖保留在 notes/history/2026-11-19-edition-boundary-stage-*-*.md。第六輪開始快照在 _bin/native-settings-check/before/，測試／差異／官方 body 比對與 build 紀錄同目錄；更早快照在 _bin/minimal-boundary-check/、_bin/edition-refactor-check/、_bin/edition-refinement-check/。根目錄 handoff 與成品緩衝歷史未刪減。
