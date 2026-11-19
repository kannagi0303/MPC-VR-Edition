# MPC-VR 基底更新與 Edition 邊界

更新日期：2026-11-19。這份文件供 Codex 維護與交接使用。

## 目標與設計選擇

保留 MPC-VR 的裝置、轉色、縮放與呈現能力；Edition 功能在獨立實作檔維護，透過少量可定位的接點接回基底。優先減少需要人工合併的邏輯，而非只減少異動檔案數。

可行路線：

| 路線 | 取捨 |
| --- | --- |
| 保留現狀，更新時手動合併 | 初始成本最低，但渲染器、設定與排程的衝突持續累積 |
| 獨立實作檔與明確接點（推薦） | 可逐步完成，不改 COM 介面；仍需審查上游生命週期及排程變動 |
| 完整外掛介面或另建 renderer | 需要大幅調整資源所有權、執行緒與 GPU 同步，目前成本及回歸風險過高 |

僅拆成多個 CDX11VideoProcessor 成員函式實作檔，仍共用類別與狀態；這是減少合併衝突的第一步，不代表已完成可插拔架構。

## 已完成的局部調整

- `Source/DX11VideoProcessorJvr.cpp`：從基底渲染器原樣移出六個 JVR 函式，包含能力判斷、構圖、來源／呈現矩形、準備畫格與投影 shader。呼叫位置、宣告、資源所有權與演算法維持既有方式。
- `build-support/edition/Edition.targets`：集中原有 30 個 MFG／DLSS／SVDLUT 建置項目、個別編譯參數、NVIDIA 靜態程式庫與 x64 輸出名稱，並加入 JVR 實作與標頭。項目路徑相對於匯入它的 `Source/MpcVideoRenderer.vcxproj`。
- `Source/MpcVideoRenderer.vcxproj`：透過一行 Import 載入上述建置內容，位置在 `Microsoft.Cpp.targets` 前、專案預設編譯設定後。此 Import 只供 renderer 使用，勿套到 ZipDepth converter。
- `Source/MpcVideoRenderer.vcxproj.filters`：補上 JVR 的 IDE 顯示項目。

`MPCVR_ENABLE_SVDLUT` 必須繼續只套用於 renderer 的 `DLSS/ZipDepthNative.cpp`，否則 converter 會再次出現調色 shader 的未解析符號。

## 尚未抽離的接點

| 基底檔案 | Edition 耦合 | 後續處理 |
| --- | --- | --- |
| `Source/DX11VideoProcessor.cpp` | SR 準備／評估、深度顯示、Render／Process 接線、設定套用、資源釋放與統計 | 先搬移完整功能實作，再收斂每個生命週期的呼叫；保留既有 GPU 操作順序 |
| `Source/DX11VideoProcessor.h` | Edition 狀態與方法宣告 | 實作邊界穩定後才評估獨立狀態物件；避免為檔案搬移引入所有權改動 |
| `Source/VideoRenderer.cpp`、`Source/VideoRenderer.h`、`Source/renbase2.cpp` | 來源交付、成品緩衝、刷新率旁路、Flush／EOS／停止的執行緒交接 | 最後處理；必須確認 renderer lock、停止等待、原始幀 PTS 及 fence 的契約 |
| `Source/IVideoRenderer.h`、`Source/VideoRenderer.cpp` | 設定欄位、遷移及登錄保存 | 集中 Edition 的載入／保存函式，保持 Settings_t 佈局與既有介面 |
| `Source/PropPage.cpp`、`Source/PropPage.h`、`Source/MpcVideoRenderer.rc`、`Source/resource.h` | Edition 頁面、預覽、取消復原及資源 ID | 拆出 Edition 頁面實作與資源，保留資源 ID、Apply／Cancel 及即時預覽行為 |
| `MpcVideoRenderer.sln`、`build-support/common.props`、`Directory.Build.props`、`Shaders/`、`Include/` | 建置、依賴與版本整合 | 更新時另外檢查；Edition.targets 尚未涵蓋這些修改 |

## 後續實作順序與授權邊界

全面拆分將同時影響上述多個核心檔案，依使用者規則先取得「大量檔案重構」授權。建議分批進行，每批可獨立 build 與回退：

1. 將 SR 與深度顯示的完整函式移到具名 Edition 實作檔，維持原函式內容與呼叫順序；建置項目只加到 Edition.targets。
2. 將 Edition 設定讀寫、設定頁及資源抽離，保留設定遷移、結構佈局、COM 介面、預覽與取消行為。
3. 為來源交付、Render、Present、Flush／Stop 等生命週期整理明確接點，逐一驗證成品緩衝與 MFG 交接。不要為追求一行 hook 改變持鎖範圍。

若進一步需要改公開 API、設定佈局或執行緒所有權，需另行說明與確認。

## 基底版本與更新程序

目前 `.github/README.md` 與 `Include/Version.h` 記錄的上游參照版本為 `0.11.1.2636`。本輪只讀本機紀錄，沒有核對官方 tag 對應的精確 commit。本機 Git 的第一筆提交已包含 Edition，不能拿它當乾淨 MPC-VR 基底。

未來更新時：

1. 取得並記錄目前基底及目標官方版本的精確 commit。不要用 Edition 的 release commit 代替官方基底。
2. 在獨立分支／工作樹檢查上游差異，逐項整合上述接點；保留 Edition 專用檔案及 renderer 的 Edition.targets Import。
3. 若新上游也列入某個 Edition 編譯項目，移除重複清單；不可同時在 vcxproj 與 Edition.targets 編譯同一檔案。
4. 使用 x64 Release 完整方案建置，包含 converter；先檢查一般 SDR／HDR、D3D9，再測 JVR、SR、MFG、ZipDepth、SVDLUT 及功能組合。
5. 在 MPC-BE 驗證暫停、seek、Flush、EOS、視窗縮放、切換功能與長時間播放。建置通過不能取代實播。

現階段仍需要人工合併核心接點，不能直接覆蓋整份上游 Source 後只還原 Import。

## 本輪驗證

- 比對 MSBuild 評估前後的全部既有 ClCompile／ClInclude 項目，排除因搬移而必然改變的 DefiningProject* 來源資訊，其餘既有 metadata 一致；只新增 JVR 的一個編譯項目與一個標頭。
- x64 Release 的 TargetName、TargetExt、OutDir、IntDir 一致。
- JVR 六個函式與原始版本逐字比對，僅允許換行格式不同。
- 完整方案建置與最終結果見 `notes/progress-handoff.md`；本機紀錄位於 `_bin/edition-isolation-check/`，不交付公開文件。
