# Edition 整合地圖

更新日期：2026-11-19。以函式名稱搜尋，避免依賴容易漂移的行號。

## 建置入口

- `Source/MpcVideoRenderer.vcxproj`：載入 `build-support/edition/Edition.targets`，位置在專案編譯預設值後、`Microsoft.Cpp.targets` 前。
- `build-support/edition/Edition.targets`：Edition 編譯清單、個別旗標、x64 濾鏡名稱及 NVIDIA 連結依賴。Converter 不匯入它。
- `Source/MpcVideoRenderer.vcxproj.filters`：保持官方內容。Edition 檔案由 targets 匯入；不維護第二份分類清單，IDE 可能顯示為未分類，尚未視覺驗收。
- `build-support/edition/upstream-baseline.json`：已核對的官方參照 tag／commit。只確認官方參照，Edition 的實際 Git ancestry 尚未驗證。

## 影像與裝置

| 入口／接點 | Edition 實作 | 契約 |
| --- | --- | --- |
| 建構 → `InitializeEdition`、解構 → `BeginEditionShutdown` | `Source/Edition/DX11Lifecycle.cpp` | 建構只正規化及初始化 Edition 欄位，不做 runtime 切換；解構仍先停 presenter 再復原 HDR／釋放裝置 |
| `Configure` → `ConfigureEdition` | `Source/Edition/DX11Lifecycle.cpp` | 先正規化設定，再套用原生 VP／HDR 設定；回傳本次 SR／JVR 變更資訊 |
| `ReleaseVP` → `ResetEditionVideoProcessor` | 同上 | 在原本位置重置深度與 SR；不改裝置清理順序 |
| `ReleaseDevice` → `StopEditionDevice`、`ReleaseEditionProjectionResources`、`ReleaseEditionOverlayResources` | 同上 | 先停止工作，再在原本位置釋放資源；不越過 native bitmap／sampler 的清理次序 |
| `Flush` → `FlushEdition` | 同上 | 在原生 VP frame-order 重置前清空成品幀與功能歷史 |
| `Render` → `RenderEditionVideo` | `Source/Edition/DX11VideoPipeline.cpp` | 呼叫者持有原 renderer lock；有回傳值代表已排入佇列、完成或失敗，空值才繼續原生字幕與 Present |
| `Render` → `PrepareEditionRender` | 同上 | 先處理刷新率旁路、成品重畫、JVR 來源及輸出目標，再交回 native 清除／HDR 路徑；有回傳值（包括 S_OK）立即結束 Render |
| `Process` → `SelectEditionProcessingSource` | `Source/Edition/DX11ProcessingSource.cpp` | SR 優先，其次是已準備的投影來源；nullptr 才回到 native VP／轉色。保留來源切換時的 MFG 歷史與原始幀交接 |
| `edition::VideoFrame` | `Source/Edition/VideoFrame.h` | 單次處理輸入；沒有持久狀態或 GPU 所有權。HRESULT 以參考傳遞，沿用呼叫端結果 |
| SR 能力判斷、準備及評估 | `Source/Edition/DX11SuperResolution.cpp` | 使用既有 `CDX11VideoProcessor` 狀態，保留 JVR 準備來源與 PTS |
| 深度／光流疊圖與 3D 研究預覽 | `Source/Edition/DX11DepthOverlay.cpp` | 共用 renderer 的私有靜態 blit／vertex-buffer 函式，避免複製繪製邏輯 |
| 解碼畫格複製與深度前讀 | `Source/Edition/DX11DepthPrefetch.cpp` | 沿用樣本、裝置與媒體格式檢查 |
| `ProcessingRect`、`UpdateProcessingResolution`、`SetWindowRect`、`ApplyPendingWindowResize` | `Source/Edition/DX11Presentation.cpp` | 沿用延後 resize 及成品幀幾何；保留原覆寫與方法宣告 |
| 功能狀態標籤 | `Source/Edition/DX11FeatureStatus.cpp` | `DrawFeatureStatus` 仍在原有合成位置 |
| `DrawStats` → `AppendEditionStatistics`、`InsertEditionStatsSeparator` | `Source/Edition/DX11Statistics.cpp` | 先附加 Edition 統計，再由原生 layout 測量寬度，最後插入分隔線；保持格式及取樣／鎖定次序 |
| JVR 投影 | `Source/DX11VideoProcessorJvr.cpp`、`Source/JvrProjection.h` | 原有來源準備與幾何算法 |
| MFG 與成品佇列 | `Source/DX11VideoProcessorMfg.cpp`、`Source/DX11VideoProcessorCompletedFrames.cpp`、`Source/Mfg/` | 本輪保留既有實作與所有權 |
| SVDLUT | `Source/DX11VideoProcessorSvdLut.cpp`、`Source/DLSS/SvdLut*` | 本輪保留既有模型、工作執行緒與 shader |

## DirectShow 來源交付與生命週期

- `VideoRenderer.cpp::Receive` 只委派至 `ReceiveEditionSample`。
- `Source/Edition/VideoRendererScheduling.cpp` 集中 `ReceiveEditionSample`、來源時間修正、`ScheduleSample`、成品緩衝交付、前讀、時鐘等待、`ReceiveScheduled` 與 EOS 覆寫。搬移時保留原函式內容，僅將 `Receive` 內部方法更名。
- `Source/Edition/VideoRendererLifecycle.cpp` 實作 `BeginEditionSegment`、`BeginEditionFlush`、Run／Pause 前後接點、`BeginEditionStop`、startup overlay 及 `OnVideoPresented`。
- `Run` 的 InterfaceLock 仍在基底呼叫端。各 hook 只保留原有的內部 RendererLock；不可順手擴大持鎖範圍。
- `ReceiveCompletedSample`、`ReceiveScheduled` 的容量／交接等待仍在鎖外；Flush／Stop 必須能取消與喚醒等待。EOS 仍由已完成畫格真正清空後回報。
- `Source/renbase2.cpp` 本輪完全未變動；既有虛擬覆寫與基底互動仍須在更新時審查。

## 設定與 UI

- `Edition/RendererSettings.cpp` 集中 native 與 Edition 的讀寫組合、載入及預設集路徑；原生 renderer 只保留載入／保存接點。原本未呼叫的遷移函式仍未啟用。
- `Source/Edition/SettingsRegistry.cpp`：MFG、SR、ZipDepth、SVDLUT、JVR 與既有 Edition／legacy 欄位的遷移與保存。兩組函式同時服務全域設定與預設集。
- `Source/Edition/SettingsRegistry.h`：設定讀寫接點及共用 ZipDepth mode 轉換。
- `Source/Edition/PropPageExtended.cpp`：Edition 控制項初始化、啟用條件、通知處理、驗證與預覽交易。`PropPage.cpp` 在原生頁面生命週期呼叫這些方法。
- SDR nits 與 JVR 的原有合併預覽復原保留為同一交易；SVDLUT 的獨立預覽保留原 Apply／Cancel／重啟頁面行為。
- `Source/Edition/ExtendedControls.rcinc` 與 `Dialogs.rcinc` 由原 RC 在原位置 include；`ResourceIds.h` 由原 resource.h include。尺寸、座標、ID 與 tab 順序維持既有值。
- `Source/IVideoRenderer.h` 由 `SettingsValues.h`、`SettingsExtension.inc`、`SettingsPresets.inc` 引入 Edition 宣告。欄位順序／佈局、COM IID 與方法順序沿用；`SettingsLayoutContract.h` 固定 x64 的大小與 94 個欄位 offset。

## 最後一輪新增邊界

| 責任 | 搜尋入口 | 契約 |
| --- | --- | --- |
| Renderer 預設集／快捷鍵 | `Edition/RendererPresets.cpp` | 保存仍使用相同 key；鍵盤 hook 的執行緒及移除順序不變 |
| Edition Flt 控制 | `Edition/RendererControls.cpp` | optional 有值即已處理；原生參數／空指標檢查仍在呼叫端 |
| 設定套用 | `NormalizeEditionSettings`、`OnEditionSettingsConfigured` | 原呼叫端持 RendererLock；依序正規化、native Configure、UI hook／暫停重畫 |
| 來源連接 | `Edition/RendererSourceConnection.cpp` | CompleteConnect／BreakConnect／投影需求覆寫沿用原流程 |
| 視窗事件與幾何 | `Edition/RendererWindow.cpp` | `HandleEditionWindowMessage` 回傳空值才交還原生事件；LRESULT 0 仍代表已處理。幾何 timer／TryLock／延後更新不變 |
| 來源與 UI 關閉 | `ShutdownEditionSource`、`ReleaseEditionUiState` | 解構內原先順序不變，先取消前讀再移除 UI hook |
| 解碼分析 | `AnalyzeEditionCpuNv12`、`AnalyzeEditionDecodedFrame` | CopySample 在原位置呼叫，CPU 已比較旗標不變 |
| Shader 擴充 | `Edition/VideoShaderExtensions.cpp`、`ShaderExtensions.h` | Jinc／RGB 轉換與採樣 helper 集中；原生 ShaderGetPixels 仍由基底實作 |
| VP bridge | `Edition/D3D11VideoProcessorBridge.cpp` | 輸入驗證、HoldFrame／RestoreFrame／ProcessExternal；沿用既有 VP 資源與 frame-order |
| 字幕相容 | `Edition/DX11SubtitleCompatibility.cpp` | PQ 字幕 shader 準備在 native bitmap 流程之前；SetPGSSubtitles 覆寫保持 |
| 統計 | `Edition/VideoProcessorStatistics.cpp`、`DX11Statistics.cpp` | layout、processor info、MFG status 集中；原生取樣／繪製位置保留 |
| 設定頁資源 | `Edition/SettingsDialogs.rcinc`、`ResourceIds.h` | 從原位置 include，ID／幾何／tab 順序不變 |
| 類別宣告 | `DX11*State.inc`、`DX11*Methods.inc`、`Renderer*State.inc`、`VideoProcessorInterface.inc` | 刻意依原位置分片；不是可獨立編譯的標頭，不改成員、析構或虛擬方法順序 |

## 官方差異守門

`build-support/edition/upstream-files.json` 固定官方 Source 的 78 個 Git blob；`upstream-boundary.json` 列出 31 個殘留修改及理由，並限制新增／刪除行數。執行方式、停止重構的判斷及基底更新步驟見 `notes/upstream-integration.md`。清單不涵蓋 Include／shader／external／build-support 等樹。

## 驗證入口

```powershell
& .\build-support\edition\tests\run-page-tests.ps1
```

腳本先建置 x64 Release 方案與測試，再載入隔離副本的真實 `.ax`，分別測有／無 JVR 描述檔。只使用本機依賴，沒有註冊、下載、封裝或真實設定寫入。測試產物保留在 `_bin/edition-tests/`。

同一腳本也擷取實際 `SelectEditionProcessingSource` 與 `PrepareEditionRender` 方法，使用 GPU／後端替身編譯執行 1,024 組來源選擇與狀態切換，以及 12 組 Render 準備測試。特別檢查 optional 內的 S_OK 仍代表提前結束、投影失敗回退、目標取得失敗及完成幀 PTS。這些測試不執行真實 D3D11／NGX 工作。

GPU 播放驗收仍需 MPC-BE：一般 SDR／HDR、D3D9／D3D11，再到 JVR／SR／MFG／ZipDepth／SVDLUT 組合，以及暫停、seek、EOS、縮放、切換功能與音畫同步。
