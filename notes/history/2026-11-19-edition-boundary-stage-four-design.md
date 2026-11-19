# MPC-VR 基底更新與 Edition 邊界

更新日期：2026-11-19。這份文件供 Codex 維護與交接使用。詳細入口見 `notes/project-map.md`。

## 目標與採用方案

使用者希望未來 MPC-VR 更新時，Edition 能以較小的人工合併成本接回。採用「獨立實作檔、少量明確接點」，保留原有演算法、設定及執行緒契約。

| 路線 | 取捨 |
| --- | --- |
| 維持混合實作 | 初始成本低，但每次更新都要在大型函式內辨識 Edition 修改 |
| 獨立實作與具名接點（已採用） | 可分批驗證、不改 COM 介面，更新時按生命週期審查接點 |
| 完整外掛介面或獨立 renderer | 需大改 GPU 資源所有權、執行緒及介面，本輪不採用 |

使用者在第一批後以「重購」回覆，依上下文視為同意所提的分批重構。前三批完成後，使用者再要求「繼續」，已完成第四批 Render／Process 接點收斂與管線回歸測試。這是程式碼與整合邊界重構，不是可任意替換核心的二進位外掛；各實作仍共用原有 renderer 類別與狀態。

## 本輪完成範圍

1. 建置及 JVR：獨立 `DX11VideoProcessorJvr.cpp`；renderer 以一個 Import 載入 `build-support/edition/Edition.targets`。Edition 清單、個別參數、NVIDIA 靜態連結與 x64 名稱集中在該檔。
2. 功能與設定：`Source/Edition/` 集中 SR、深度顯示、前讀、設定讀寫與遷移、Edition 設定頁生命週期、事件及資源。原生頁面透過明確方法呼叫。
3. 播放整合：抽出單次影片處理階段、設定／裝置／Flush hooks、視窗／處理尺寸、功能狀態、DirectShow 來源排程及 Run／Pause／Stop／切段接點。
4. 驗證與治理：加入真實隱藏設定頁測試、一鍵測試腳本、接點地圖與精簡本機 AGENTS 規則。舊交接內容保留於 `notes/history/`。
5. 第四批：以 `PrepareEditionRender` 集中刷新率旁路、成品重畫、投影及目標準備；以 `SelectEditionProcessingSource` 集中 SR／投影來源及切換歷史；抽離建構／解構中的 Edition 初始化／停止及統計文字。本批讓 DX11VideoProcessor.cpp 從 5,523 行降至 5,239 行，並移除三個不再需要的 Edition 標頭。
6. 直接擷取實際 C++ 方法做 1,024 組來源選擇及 12 組 Render 準備測試；補上經官方公開 tag 核對的 `upstream-baseline.json`。

`Source/DX11VideoProcessor.cpp`、`Source/VideoRenderer.cpp`、`Source/PropPage.cpp` 合計比本輪開始前減少 2,400 行以上；這個數字描述搬移規模，不代表與乾淨官方基底的差異已消失。

## 維護契約

- 新 Edition 功能優先擴充獨立模組，不新增混合用途的基底大函式。
- `Edition.targets` 在 renderer 的編譯預設設定之後、`Microsoft.Cpp.targets` 之前匯入。所有項目路徑相對於匯入它的 `Source/` 專案；converter 不匯入。
- `MPCVR_ENABLE_SVDLUT` 只套用 renderer 的 `DLSS/ZipDepthNative.cpp`。Converter 沿用自己的編譯設定，避免引用 renderer shader 而產生未解析符號。
- `Settings_t` 與 COM 宣告所在的 `Source/IVideoRenderer.h` 本輪逐位元未變；沒有新增或更換 IID、欄位及方法順序。
- 所有數字資源 ID、設定鍵名、遷移規則及原生 UI 佈局維持既有值。新 RC include 在原本位置展開。
- GPU 呼叫順序、PTS、容量等待、原始幀保護、renderer lock 範圍與 fence 所有權維持既有方式。移動程式碼不代表可以順手改鎖定策略。
- `RenderEditionVideo` 的 optional 回傳明確區分「停止此輪 Render」與「繼續原生合成」；HRESULT 以參考傳遞，避免遺失前面處理階段的結果。
- `PrepareEditionRender` 同樣使用 optional；`S_OK` 的數值雖是零，仍是「有值、立即結束」；只有空 optional 才繼續。原本 projection 起點的 QPC 量測位置保留。
- `SelectEditionProcessingSource` 只選借用來源及處理 Edition 切換，不接管 native VP／轉色；來源矩形只在取得 SR 輸出時改為其原生尺寸。
- Edition 影片處理後的字幕、alpha bitmap、VBlank 與 Present 仍在原生合成路徑。

## 基底版本與更新程序

`.github/README.md` 與 `Include/Version.h` 的本機記錄指向官方 `0.11.1.2636`。2026-11-19 已透過 GitHub 公開唯讀 API 核對 [官方 tag 0.11.1](https://github.com/Aleksoid1978/VideoRenderer/releases/tag/0.11.1)：annotated tag object 是 `2d8d9de66b8c1d66987f9966fcc93a59991b0275`，指向 [commit 6be828bfc415aa72286862e6e262eb652abd9a53](https://github.com/Aleksoid1978/VideoRenderer/commit/6be828bfc415aa72286862e6e262eb652abd9a53)。該 commit 的 `Include/Version.h` 定義版本為 0.11.1；本機宣告的完整產品版號仍記為 0.11.1.2636。

這些資料已記入 `build-support/edition/upstream-baseline.json`，但沒有核對 Edition 全部檔案的實際 ancestry，也沒有實際升級基底或更改 remote。本機 Git 的第一筆提交已包含 Edition，不可視為乾淨官方基底。

未來更新時：

1. 確認目前官方基底與目標版本的精確 commit，記錄於 notes；不要以 Edition release commit 代替。
2. 在獨立分支／工作樹比較上游，按 `notes/project-map.md` 的設定、來源交付、Render、Present、Flush／Stop 接點整合。上游若改變契約，先調整接點再重用 Edition 實作。
3. 保留 renderer 的 Edition.targets Import、相關私有宣告及 RC／resource.h include。確認編譯清單沒有重複項目。
4. 完整 Rebuild x64 Release 方案，包含 converter；執行 `build-support/edition/tests/run-page-tests.ps1`。
5. 在 MPC-BE 驗證一般 SDR／HDR 與 D3D9／D3D11，再測 JVR、SR、MFG、ZipDepth、SVDLUT 及組合。涵蓋暫停、seek、EOS、縮放、即時切換與音畫同步。
6. 更新 handoff 及接點地圖，保留歷史。

仍需人工審查的部分包括基底類別中的 Edition 成員、既有 hooks 的呼叫位置與原生操作順序、字幕／呈現後的通知及計數、截圖、既有 `renbase2.cpp` 修改、版本／shader／建置基礎設定。初始化、Render 準備、Process 來源選擇及統計已收斂成具名接點。不可直接覆蓋整份官方 Source 再只還原一行 Import。

## 驗證結果與限制

- x64 Release 完整方案 Rebuild 通過：renderer、NgxShim、converter 及依賴皆完成。MSVC 14.51.36231，`/m:1 /nr:false`。仍有 2 個搬移前即存在的 C4018 比較警告，以及 vendor ONNX Runtime 標頭的 C4819 字碼頁警告。
- 真實 `.ax` 的隱藏設定頁測試通過有／無 JVR 描述檔兩組。涵蓋預覽、Apply、Cancel、儲存失敗、重新啟用、無效選項、D3D11 gate 與隱藏 JVR 設定保留。使用記憶體 renderer，不寫真實登錄設定。
- 以本機重構前快照及實際新設定函式搭配記憶體登錄替身，12,000 組新舊讀取／寫回差異測試一致，涵蓋 86 種設定鍵。
- 56 個抽離區塊中，51 個逐字保留，5 個僅調整符號名稱或 linkage；另確認全部數字資源 ID、原始編譯項目 metadata、輸出路徑與設定／COM 標頭一致。
- `git diff --check` 通過。設定頁測試腳本可重跑；差異測試與當輪快照保留在 `_bin/edition-refactor-check/`。
- 第四批 x64 Release 增量方案建置及完整測試腳本通過。新增 1,024 組實際來源選擇函式狀態測試，及 12 組實際 Render 準備函式測試；GPU／後端使用替身。六個抽離區塊經明確的參數交接／縮排轉換比對一致。最新紀錄及快照保留在 `_bin/edition-refinement-check/`。
- 未做 MPC-BE 實播、D3D9／HDR 影像比較、MFG／SR GPU 時序、長時間負載或音畫同步驗收。以上通過不能替代實播。
- 沒有 commit、push、部署、註冊濾鏡、下載外部依賴或更新全域 Codex memory；只有唯讀查詢官方公開 tag 與版本標頭。
