# MPC-VR-Edition 成品畫面緩衝製作方案

## 本機成品

使用者指定直接交付。成品緩衝在本機逐行 NV12／SDR D3D11 檔案以正常倍速播放時自動運作。舊 ZipDepth 預取在本機仍設為零。原始幀和 MFG 生成幀經最終視訊處理後進入成品紋理池，並依 graph 時鐘呈現。字幕與即時 OSD 在呈現時合成。提前上限為 2 秒，受來源交付速度、GPU 完成速度和 DXGI 顯示記憶體預算限制。「Show statistics」的「Video buffer」顯示當下就緒毫秒數。

目前實作在上游交付執行緒進行原始畫面的預製，由另一執行緒呈現並完成對應的 MFG 小批次。兩者共同沿用 renderer lock 序列化 D3D11 操作。佇列滿時回壓發生在鎖外，保留 paused poster 和 EOS 握手。GPU fence 區分提交、完成和紋理可重用。視窗大小改變時，剩餘成品依新視窗幾何重映射，後續樣本依新尺寸製作。

x64 Release 建置通過。本機 DirectShow 單版本測試使用 30 fps 逐行 NV12 來源與 MFG，從 4K 輸出開始，途中暫停、縮放、繼續播放並正常收到 EOS。注入 401 ms 來源交付中斷，期間有 24 次成功的 Present 呼叫，最大呼叫間隔 17.7 ms。首幀走 preroll，其餘 209 張原始幀的 PTS 全部對上。尚待用真實 MPC-BE 音軌與實際背景負載確認長時間上限、A/V 同步及顯示掃描節奏。

縮放修正先釋放 back-buffer 引用再重建 DXGI buffers。播放器視窗無法立即取得 renderer lock 時，以訊息排定合併的幾何更新。與 GitHub v0.2.0（`9efbf30`）比對後，視窗 swap chain 恢復 `DXGI_SCALING_NONE`，拖曳時的視訊矩形在幾何更新取得 renderer lock 後套用，DXGI buffer 仍按既有約 140 ms 期限等尺寸穩定才重建。成品呈現沿用既有自適應 DXGI 提交提前量，PTS 與 graph 時鐘照原排程。

指定的 MP4 實測為固定 29.97 fps，全部 111,232 組相鄰 PTS 間隔皆為 33.3667 ms。離屏 DirectShow 測試把 Media Foundation 解碼輸出按 PTS 交付，從 4K 視窗開始連續送出 48 次尺寸變更。恢復 `DXGI_SCALING_NONE` 後，450 ms 拖曳期間有 27 次 Present 呼叫，最大呼叫間隔 19.5 ms，結束後 151 ms 重建一次並正常播完。289 張緩衝原始幀 PTS 對上。離屏視窗回傳 `DXGI_STATUS_OCCLUDED`，拖曳手感與實體上屏節奏須在 MPC-BE 確認。

切換後約兩秒的單幀尺寸閃回源於待處理 MFG 批次。批次已保留舊紋理和影片矩形，完成前變動的視窗矩形卻被合成端直接使用，使畫面像素與記錄矩形不符。現在 MFG 批次把保留的矩形交給所有輸出合成路徑，呈現端再映射到當前視窗。離屏 48 次 resize 測到七個跨矩形的在途批次，對應原始幀在 1.977–1.988 秒後才提交。289 張原始幀 PTS 全部按序，呈現呼叫未回傳錯誤。

小視窗切換 3840×2160 時還有另一個過渡情況。播放端會先給全螢幕影片矩形，DXGI buffer 尚待延後重建，因此可能把全螢幕影片畫進較小的成品紋理，當時即裁掉右側或下側。現在對超出成品紋理的矩形先按原比例縮入紋理，原始幀與 MFG 都使用這個儲存矩形，呈現時映射到即時的影片矩形。用合成測試圖跑 1920×1080 → 3840×2160。五個在途批次在切換後 1.997–2.130 秒提交，切換後 1.5–2.6 秒的 84 次右側像素讀回都保有內容。314 張緩衝原始幀 PTS 全部對上。測試屬於離屏 DirectShow，MPC-BE 實際上屏仍待確認。

## 成品緩衝資源實測

RTX 4060 Ti 16 GiB、指定的 1080p 29.97 fps 影片、3840×2160／60 Hz 輸出、離屏 DirectShow 視窗。附帶統計的本機建置使用相同的成品處理路徑。量測兩組設定，各取約 12 秒的採樣區間。

| 指標 | MFG | MFG + DLSS SR + ZipDepth |
| --- | ---: | ---: |
| 就緒水位中位數 | 1,966 ms | 1,949 ms |
| 就緒水位 P10 | 1,949 ms | 459 ms |
| 成品紋理池估算峰值 | 3,834 MiB | 3,834 MiB |
| DXGI 行程本地顯示記憶體中位數 | 4,551 MiB | 5,236 MiB |
| 整張 GPU 使用率 P95 | 39% | 71% |
| 平均 CPU 用量（邏輯核心） | 0.73 | 1.08 |
| Present 呼叫頻率 | 59.92 Hz | 60.06 Hz |

兩組在注入 401 ms 來源中斷時分別有 24／25 次 Present 呼叫，取樣最低水位約 1.66 秒。停止注入後約 0.44／0.69 秒回到各組中位數的 95%。進階組在累積初期的水位較低，ZipDepth 本次使用 mixed FP16。DXGI 預算約 14.98 GiB。離屏資料能量到處理與提交，實體掃出、實際多工與 MPC-BE 音訊同步仍須在播放器驗證。原始紀錄位於 `_bin/CompletedFrameCheck/usage/`。

以下保留原先研究與製作規劃，供後續擴充使用。

## 29.97 fps 來源的補幀診斷

本機第二支 1080p AVC 測試影片的 MP4 呈現時間戳有 105,676 個固定 33.3667 ms 間隔。用按 PTS 排序交付的 Media Foundation 解碼與 DirectShow 測試，片頭僅 MFG 約 60 次 Present／秒。32 分鐘附近開 MFG、DLSS SR、ZipDepth 的測試約為 56–60 次／秒，部分時段成品水位接近零。診斷版共量到 36 次生成幀跳過，其中 29 張 NGX 未趕上期限、6 張在完成合成前已到期、1 張送出時已過期。測試用 Media Foundation 解碼交付序列本身會倒序，測試端依 PTS 排序；MPC-BE 的實際解碼序列與使用者回報的 40 fps 仍待核對。

MFG 統計的輸入 FPS 已改用來源 PTS，輸出包含實際送出的原始、生成與回退幀。成品路徑補齊跳過計數，在統計顯示 `skipped/s`，並且在 opt-in cadence trace 中記錄生成幀未就緒、逾期及其他成品失敗原因。

## 目標與範圍

本機檔案在正常播放時，renderer 利用可用的 CPU、GPU 和解碼餘裕，提早完成可按 PTS 呈現的畫面。其他程序短暫佔用資源時，播放器先消耗已完成的畫面。保留原始畫格、原 PTS、既有 audio clock 和使用者設定的影音延遲。首幀與跳播後首幀依原有流程呈現，開始播放後才累積備援。

首版實驗以 MPC-BE、同裝置 D3D11 NV12、逐行 SDR 本機檔案、正常倍速為範圍，涵蓋 MFG 開關及現有 SR／VSR 路徑。HDR、隔行、直播、動態媒體格式與其他解碼器先沿用原有路徑，後續依獨立驗收開放。先用內部開關提供可回退的 A/B，通過實播驗收再決定是否預設開啟。緩衝策略與 ZipDepth 的模型及舊 `LookaheadMs` 設定獨立，A/B 期間停用舊深度預取。

## 查證結果與選型

- `Source/VideoRenderer.cpp` 的 `ReceiveScheduled` 先用 `WaitForRenderTime` 等播放時鐘，再走 `Render`。現有 `DecodedFrameLookahead` 把原始樣本交給同一條時鐘等待路徑，另一執行緒只做 `PrefetchZipDepth`。`CloneLookaheadSample` 依 D3D11 NV12、模型與色彩條件複製樣本，每張配置新紋理。其樣本脫離 decoder surface 的做法可參考，整條舊路徑直接加長仍只會提前解碼或推論。DirectShow upstream delivery 也會受 renderer 的 `Receive` 及 allocator 取樣阻塞影響。 [CBaseRenderer 排程](https://learn.microsoft.com/en-us/windows/win32/directshow/cbaserenderer) · [IMemAllocator::GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/strmif/nf-strmif-imemallocator-getbuffer)
- `Source/DX11VideoProcessorMfg.cpp` 已有 `RenderMfgVideo` 和 `FinishMfgPresent` 的分界。現行 `m_mfgPreparedTexture` 是單一暫存，`QueueMfg` 內的 NGX banks 是跨 D3D11／D3D12 的周轉資源，且既有排程包含即時期限、過期與淘汰判斷。光是把現有 MFG 佇列加長會同時碰到 bank 生命週期與即時排程。成品應在取得完成的 bank 後儘快轉入 renderer 自有紋理池，再依既有消費 fence 回收 bank。沿用現有 MFG 相位、VFR、重複幀和原始幀保留邏輯，將每對來源畫面的排程結果交給新路徑，跳過舊路徑的即時排程提交。
- LAV Splitter 每個輸出 pin 有 packet 佇列和交付執行緒，有軟水位、硬上限，其他 pin 缺資料時仍需照顧其交付。MPC-BE 的 splitter 也從 `DeliverPacket` 進入各輸出 pin 的 packet 佇列。這能吸收一部分讀檔與解碼供應波動，也說明 renderer 必須量實際收得到的未來畫面，特別是音訊分支一併運作時。 [LAV Splitter OutputPin](https://github.com/Nevcairiel/LAVFilters/blob/master/demuxer/LAVSplitter/OutputPin.cpp#L329) · [MPC-BE BaseSplitter](https://github.com/Aleksoid1978/MPC-BE/blob/master/src/filters/parser/BaseSplitter/BaseSplitter.cpp#L292)
- 首選 renderer 自有的 GPU 成品紋理池，保留實際輸出格式並按 PTS 呈現。擴大 DXGI Present queue 只會增加已提交給顯示端的工作，求穩時保持既有 swap chain 設定。CPU RAM 或壓縮可作為日後擴容研究，得先量讀回、解壓、上傳和失效後重建的成本。 [DXGI frame latency](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgidevice1-getmaximumframelatency) · [mpv 實作的畫面佇列取捨](https://github.com/mpv-player/mpv/blob/master/DOCS/man/vo.rst)

## 製作步驟

### 1. 量供應能力並保留播放基準

- 在 `VideoRenderer.cpp` 的 `Receive` 入口記錄 `qpc`、segment、sample PTS 和 graph stream time。另記錄解碼交付、等空位時間、首幀顯示與 audio-clock 同步偏移。`Mfg/CadenceTrace.h` 的 `source` 事件在 NGX 提交後，不能當作解碼交付時間。沿用既有 QPC 時基，新增事件先寫固定容量記憶體環形佇列，交由背景批次輸出，以免同步寫檔變成卡頓來源。
- 啟用小型非同步 ingress 試作。`Receive` 得到名額、擷取 sample 時戳與媒體資訊後，把輸入交給單一 D3D11 執行緒。若排入的是 decoder sample 參考，持有量須有小上限，完成 copy 後立刻釋放，避免耗光 allocator。只在需要提早歸還 decoder surface 時複製到可重用的暫存。佇列滿時回壓必須可取消，等待名額期間不得持有 renderer/interface lock 或任何 flush 需要的鎖。測 MPC-BE internal splitter 和 LAV Splitter 同時有音軌時各自能供應多少未來 PTS，並檢查音訊和字幕能否持續交付。
- 若上游供應不足，先定位 decoder surface、splitter 或 audio 分支的阻塞。renderer 可用可取得的成品水位正常運作。若連目標所需的未來畫格都收不到，將上游設定或 player 整合列為獨立工作，不把容量當成達成的秒數。

### 2. 把現有畫面處理和呈現拆開

- 在 `DX11VideoProcessor.cpp` 抽出可輸出至指定目標的原始幀處理路徑。MFG 從 `DX11VideoProcessorMfg.cpp` 共用畫面處理、相位與原始 PTS，於每個來源 pair 排定輸出後，趁 bank 尚受保護時完成生成幀的色彩、縮放和適用的 SR／VSR。ZipDepth 按來源 PTS 共用。每個原始幀在對應顯示時段仍有自己的輸出條目。重複畫面只有在渲染結果相同時才共用紋理。
- 以單一渲染執行緒管理 D3D11 immediate context 和 swap chain。把現有 MFG present worker 擴成成品路徑的執行緒，啟用時只有一個 presenter 提交 `Present`。D3D12 NGX 沿用跨 API fence。同一個執行緒優先處理已到呈現期限的工作，有餘裕才處理未來的一個小批次，避免把大量未來 GPU 工作排在下一次顯示前。現有 UI 的 repaint、截圖、resize 等 GPU 入口在此模式需轉成同一執行緒的請求，才可開放 A/B。 [Microsoft D3D11 執行緒與 DXGI](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro)
- 成品條目保存 `epoch`、PTS／時段、來源 serial、原始或生成類型、渲染設定代次、紋理引用和 producer fence。fence 完成才納入水位，使用 `GetCompletedValue` 或事件喚醒。送出呈現後，等最後讀取該紋理的 GPU copy fence 完成再回到池中。設定所需的小批次提交與 in-flight 上限須在第 1、2 階段實測，避免等待 fence 阻塞合法的呈現期限。 [D3D11 fence 完成值](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-getcompletedvalue)
- 成品包含已決定的視訊像素、成品尺寸和輸出色彩。`FinishMfgPresent` 保留成品 copy、由播放器在呈現時更新的字幕與 OSD，以及 `Present`。深度／光流除錯疊圖若開啟須取同 PTS 資料，首版可透過停用成品模式沿用既有除錯。字幕以該幀 PTS 請求，驗收包含字幕延遲、換軌與背景負載。若資料穩定且量出字幕合成是瓶頸，再把已就緒的字幕移入成品階段並依字幕代次失效。

### 3. 先測固定水位，再使用實測定參數

- **覆蓋時間**是距離當前 graph 播放時間的連續成品長度。每個計畫呈現時段要有對應且已完成的紋理。允許既定 cadence 有計畫地重複顯示同一紋理，另記錄因生成幀缺席而使用原始幀備援的時段。分別記錄完整品質與原始幀備援的覆蓋時間，不能把漏掉的原始幀、尚未完成的 GPU 工作或跨 PTS 缺口的未來紋理算入水位。
- 試作固定上限 `0 ms`（舊路徑）、`250 ms`、`500 ms`、`1,000 ms`、`2,000 ms`。2 秒只是測點，實際上限由上游供應、可配置成品紋理和補充速度決定。以相同影片和相同背景負載時間線擷取 trace。從接近滿水位的區間量「播放消耗時間減去同區間新完成的連續成品時間」的最大累積跌幅。耗盡後的真實跌幅至少等於耗盡前的水位，另標記耗盡長度，避免用截斷的資料選出過小的預設值。
- 同時量負載解除後的淨補充速率 `r - 1`，`r` 為每一秒實時間能新增的連續成品秒數，量測時佇列須低於上限且上游有料。例如兩波負載合計消耗 450 ms，若 `r = 1.25`，補回至少需 1.8 秒。若 1.5 秒都無新成品，500 ms 只能吸收前 500 ms，1 秒仍有 500 ms 缺口。若解除後 `r <= 1`，水位不會恢復，須找產出或上游瓶頸。
- 從能通過驗收的最小測點選初始目標水位。正式模式以已實測的跌幅和呈現餘裕設高水位，低水位至少保留足夠時間完成下一個來源 pair。兩者間留出足夠區間，避免一個畫格就來回切換。配額同時設 PTS 時限與位元組上限，按唯一紋理、實際格式、NGX／暫存需求和 DXGI `Budget`／`CurrentUsage` 限制池大小，預留呈現與配置空間。預算收縮時先回收空閒池並暫停遠端預製，不強迫把活躍成品移到 RAM。水位不足時先保住原始幀，允許 MFG 降級，回到高水位再恢復。恢復時按現有的 NGX 歷史重新暖機條件處理。 [DXGI `QueryVideoMemoryInfo`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo)

### 4. 處理播放狀態及中途失效

- 保留 `ReceiveScheduled` 目前的 paused poster／`Ready()` 握手，讓第一張畫面先出、GPU 資源依既有路徑暖機。首幀之後進入非同步模式，不往後移動影片或音訊時鐘。Pause 停止往前處理，resume 只使用仍對應相同 segment 與輸出設定的成品。逐幀、暫停跳播與 repaint 須用當下應顯示的 PTS，不能從成品尾端拿圖。
- `BeginFlush` 先取消當前 epoch、喚醒 `Receive` 和工作執行緒，再依基底類別完成 flush。`NewSegment`／stop／斷線作相同取消。換尺寸、全螢幕、輸出格式和畫面處理設定變更時使舊成品失效，讓 GPU fence 走完才回收。下一個可顯示畫面走當下設定的即時路徑。對只改畫面設定而無 graph seek 的情況，需有明確約束保住已收下的來源樣本。試作以有界來源重播池保留尚未呈現的原始輸入，在預算中列入這筆成本，從下一個原始 PTS 恢復，等待新 pair 才重新開 MFG。其成本太高時，先限制實驗的最大提前量。`EndOfStream` 用佇列尾標記，實際顯示完最後一張才送 `EC_COMPLETE`，flush 取消待送的通知。 [DirectShow Flush](https://learn.microsoft.com/en-us/windows/win32/directshow/flushing) · [DirectShow EOS](https://learn.microsoft.com/en-us/windows/win32/directshow/end-of-stream-notifications)

## 實播驗收與啟用條件

- 用 `build-x64.cmd quick` 建立實驗 AX，對照同一份建置用開關切換。先在無負載下測 4K 23.976／60 Hz、來源 4K60、VFR、MFG 開關、SR 或 VSR。A/B 使用相同影片、解碼器、MFG 模式、輸出尺寸、音訊裝置和顯示模式。固定時點施加 CPU、GPU 和磁碟負載，分別測單一 burst、間隔短於補回時間的重複 burst、持續負載，以及平常多工。這些壓力測試在隔離的測試時段執行。
- 最先做可確定因果的測試。在緩衝達到測點後暫停 renderer 的新畫面生產，呈現端照跑。對 `500 ms` 測點給 400 ms 的生產暫停，對實測達到 `2,000 ms` 的設定給 1,500 ms 的生產暫停。在顯示端仍可服務且過程中未跨設定變更的條件下，覆蓋範圍內應維持同一串輸出與 PTS，零 underrun。再做一個明確長於當下水位的中斷，確認 underrun 時點符合實測水位且備援策略生效。此測試證明成品確實可直接取用。真實 GPU 壓力另以顯示 trace 檢查交付是否阻塞在最後 copy／flip。
- 記錄原始幀交付和顯示順序、失約的呈現時段、MFG 遺失與品質降級、兩種覆蓋時間、補回時間、池位元組／DXGI 預算、首幀與 seek 首幀時間、A/V 偏移，以及實際顯示幀間隔 P95／P99。內部 QPC trace 與 PresentMon 的顯示事件對照，將 `Present` 成功回傳和實際顯示分開檢查。另測連續 seek、pause／resume、逐幀、字幕換軌、resize、全螢幕、播放完畢和 stop。 [PresentMon 顯示事件欄位](https://github.com/GameTechDev/PresentMon/blob/main/PresentMon/PresentMon/CsvOutput.cpp)
- 先通過功能門檻。無負載 A/B 不能新增原始幀遺失／亂序、首幀凍結、持續 A/V 偏移、EOS 提前或鎖死。再通過負載門檻。在顯示端仍按時工作的可重現情境中，緩衝開啟須減少實際逾時且能補回水位。確認壓力下的改善來自使用已完成的畫面，並記錄適用秒數及條件。若顯示本身被 GPU 或系統排程阻塞，記錄為 copy／flip 瓶頸。

目前完成程式路徑與參考實作查證，以及文件內的理論試算。上述固定水位、指標及故障注入是後續製作與驗收規格，尚未以這份方案進行建置或實播。

## 來源 PTS 亂序造成的幀率波動

- 本次影片的 MP4 時序表共有 105,677 張，按呈現時間排序後相鄰間隔一律為 `1001/30000` 秒。Media Foundation 測試以解碼回呼順序送幀時，480 張有 238 次 PTS 倒退。舊估算用最近 10 個間隔的首尾相減，可以重現 OSD 的 23.054、27.246 與 33.300 fps。
- `CFrameStats` 遇到近期 PTS 亂序時，按排序後的間隔檢查是否存在一致步長。480 張測試資料中，所有 470 個完整視窗都得到 29.970 fps。正常依序的來源沿用原本的幀率估算。D3D11 統計新增 `Source PTS` 倒退計數供 DirectShow 實播核對。
- 4K SR 後接 MFG 的獨立測試中，原始幀全數送出，亂序輸入仍發生補幀逾時或缺席。MPC-BE 的實際收幀順序及顯示幀間隔仍須確認。若 `Source PTS` 持續增加，優先查解碼器交付順序與 renderer 的輸入緩衝。若未出現該行，按補幀丟棄原因查成品生產與呈現期限。

## 成品水位與補幀輸出

- `Video buffer` 的 `ready` 是 GPU 完成的佇列前段最遠 PTS 距離播放時鐘的時間，`queued` 也算進已提交但 GPU 尚未完成的成品。兩者都是時間水位。若補幀在入列前掉落，後方仍有成品時水位可以很高。
- 同一支測試影片在 4K SR 後接 MFG。依序輸入時，7–21 秒、每約 250 ms 的 56 筆中有 17 筆抽到空佇列、7 筆已入列但 `ready` 顯示 0 ms。亂序輸入時，曾在 `ready` 72–303 ms 的一秒只送出 40 次 Present，並有 18 個補幀丟棄事件。先對照 `Source PTS` 和水位再判斷是前端供應、GPU 成品未就緒，還是補幀槽位缺失。
- 成品貼圖池保留仍被 GPU 使用的回收貼圖，完成後可供後續同尺寸畫格使用，避免在等待 fence 時直接移出池並反覆配置。

## 停用 ZipDepth 的成品追蹤

- 既有 `MPCVR_MFG_CADENCE_TRACE` 的 `duration_qpc` 記錄 `completed_produce`、`completed_mfg_submit`、`completed_compose` 和 `completed_present` 的 CPU 呼叫時間。將同 serial、PTS 的 MFG 送出時間與成品合成開始時間相減，得到觀察到的等待時間，其中也包含 GPU 佇列、跨 API 同步及執行緒排程。
- 同一影片以 4K SR 後接 MFG、兩處各 480 張來源測試。兩處 `ready` 水位分別最高約 0.23 秒與 1.98 秒，分別跳過 5 張與 4 張補幀，全部在補幀 deadline 當下 NGX 輸出仍未就緒。原始幀皆送出。MFG CPU 送出 P95 為 1.83/2.24 ms，Present 為 0.36/0.34 ms，送出到開始合成的觀察間隔 P95 為 31.09/20.86 ms。
- 在第二個時間點關閉 SR，MFG 轉為 1080p 內部運算。觀察間隔 P95 為 10.52 ms，跳過 1 張補幀。此對照同時變更了 SR 工作量及 MFG 解析度。尚未用 MPC-BE 捕捉到實播水位從高處掉光的區間。

## SR 順序與關閉 SR 的輸入尺寸

- 同一台機器、同一段、同樣 1967×1102 視窗與 4K 顯示，停用兩個 ZipDepth 選項，各送 900 張原始幀。先 MFG（1080p）再 DLSS SR 時，最高 ready 水位 198 ms，9 次取樣佇列空，跳過 201 張補幀。先做 DLSS SR 再做 MFG（4K）時，最高 ready 水位 1783 ms，跳過 6 張。後置 SR 對原始幀與生成幀各自運行，前置 SR 只處理原始幀。
- 播放中停用 SR，8-bit 路徑的 MFG 輸入從 4K 回到 1080p，強制 10-bit 內部格式則重現 1080p 來源在 SR 關閉後仍使用 4K MFG。SDR 原尺寸捷徑要求 BGRA8 與沒有額外後處理，10-bit 路徑使用顯示尺寸的處理紋理，以保留格式轉換與抖色。使用者播放器的實際回退條件仍待狀態資訊確認。
- 兩次即時切換都觀察到約 2 秒的單次 MFG 送出停頓，其後曾處於空水位並跳過約 120 張逾時補幀，最後水位重新接近 2 秒。穩態 SR 順序差異與切換的初始化停頓應分開查。

## 先放大再補幀的提交與回補

- 成品路徑先放大、非同步 MFG 時容許兩批 GPU 工作在途。依輸入順序合成及入列，批次保留各自的成品紋理、目標矩形和原始畫格。以 serial 和來源 PTS 配對生成紋理。待合成紋理計入 VRAM 配額，`ready` 水位仍只計已完成的成品。後置放大仍以單批提交。
- 同片段依序送入 900 張來源幀，ZipDepth 關閉、視窗 1967×1102。舊版在播放第 21–27 秒由約 1.77 秒降到 30 ms，最後一個完整秒送出 58 次 Present。新版同一段維持約 1.94–1.98 秒及每秒 60 次 Present。900 張原始幀均送出且保持順序。新版整段跳過 31 張，其中 30 張發生於媒體前兩秒，其餘一張在約第八秒。起播逾時與 MPC-BE 實際掃描輸出仍須確認。
- 3840×2160 成品尺寸測試同一段前 26 個完整秒各送出 60 次 Present，水位約在第 3 秒回補至 1.9 秒。第 27 秒附近呈現路徑出現一次約 249 ms 的 CPU 呼叫，之後水位短暫降到零。全程送出 900 張原始幀，跳過 23 張補幀。仍待拆分成品複製、OSD 與 DXGI Present 的時間，定位這次停頓。

## 補幀前修正來源 PTS 順序

- 以同一段 1080p、900 張來源，固定 4K MFG 工作尺寸、4K 成品、ZipDepth 關閉。依序送入時，開啟與關閉 SR 分別跳過 4 與 2 張，`ready` 水位約 1.98 秒。用解碼回呼順序重測開啟 SR，900 張有 447 次 PTS 倒退，耗損增至 502 次跳過，29 個完整秒平均 50.14 次 Present。MP4 呈現時序仍是固定 29.970 fps。
- `ReceiveOrderedSample` 在本機檔案的 MFG 成品路徑補齊 PTS 順序。依序畫格直接交付。提前抵達的 NV12 樣本只暫存必要部分，最多 6 張，時間戳缺口限 250 ms。硬體樣本複製至獨立 NV12 紋理，軟體樣本複製像素與時間欄位，避免佔滿解碼器 allocator。EOS 交付剩餘樣本，flush／stop／新 segment 清除舊狀態。
- 修正版接收同一亂序資料，900 張送進 MFG 的 PTS 全部依序，900 張原始幀都送出。跳過數 18，29 個完整秒平均 59.52 次 Present，其中 25 秒達到 60。最高水位 1.966 秒。此測試用軟體傳送 NV12 確認 renderer 排序與播放，MPC-BE 的硬體解碼交付順序及顯示端仍須確認。`Source PTS … reordered` 顯示已接收到並處理的倒退順序。
- 依序輸入的回歸測試送出 900 張原始幀、跳過 5 張。另啟用 SR 與兩個 ZipDepth 選項，用預設 mixed FP16 模型重播 480 張亂序輸入。所有 PTS 依序送進 MFG，480 張原始幀全數送出，尚有 28 次補幀跳過。最終未納入非阻塞 Present 與起播預熱的試作。

- MPC-BE 實播回報 1080p 片段出畫面後卡住，4K 來源仍正常。已撤回 `ReceiveOrderedSample`、樣本複製和 EOS 排序排空。上述改善只在獨立測試的亂序 NV12 交付中重現，尚未確認 MPC-BE 實播掉幀是否也由 PTS 亂序造成。保留 `Source PTS … out of order` 與成品階段追蹤，以實際播放的交付與呈現事件繼續定位。

- 撤回版能播放後，使用者確認 MPC-BE 的 `Source PTS … out of order` 持續增加。以 `IMediaSampleD3D11` 包裝 NV12 解碼紋理陣列重測，舊排序 AX 在第一張複製後送入 VP 的樣本回傳 `0x80070057` 中止。獨立測試亦證實只標記 `D3D11_BIND_SHADER_RESOURCE` 的 NV12 紋理建立 VP 輸入檢視會失敗，加入 `D3D11_BIND_RENDER_TARGET` 則成功。新排序限 D3D11 GPU 輸入，驗證複製紋理的 VP 相容性後才暫存，失敗時直接用既有交付路徑。 [D3D11 VP 輸入檢視文件](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videodevice-createvideoprocessorinputview)
- 同一段影片以硬體輸入介面交付亂序 NV12 陣列樣本，SR、兩個 ZipDepth 選項及 4K 輸出皆開啟。撤回版送進 MFG 的 480 張有 238 次倒退、301 次補幀跳過，平均 50.07 Presents/s。修正版完整播完，480 張 PTS 依序、446 張補幀送出、33 次跳過，15 個完整秒有 14 秒達 60，平均 58.67 Presents/s，最高 `ready` 1.939 秒。CPU NV12 依序路徑亦送出 480 張原始幀及 EOS。硬體測試為 D3D11 紋理交付測試，MPC-BE 本體全程實播仍待驗證。

## 驗證畫面順序與時間戳的對應

- MPC-BE 實播回報畫面順序錯誤。直接解析 MP4，視訊軌有 AVC B 幀，但沒有 `ctts` 組成時間偏移表。`stts` 的均勻間隔只足以說明 29.970 fps，無法證明每張解碼畫面取得正確 PTS。
- 開頭 120 張以 Media Foundation 取出 NV12 亮度採樣，對照 FFmpeg 的解碼畫面。118 張可唯一精確匹配的畫格依回呼順序沒有倒退，按回呼 PTS 排序後卻有 58 次畫面倒退。上述「480 張 PTS 依序」的測試只證明數字排序，不能作為畫面順序驗收。
- 撤回接收端的樣本排序及排空，沿用解碼畫面交付順序。保留 PTS 診斷與既有 NV12 複製紋理相容性檢查。後續處理 MFG 需要確認並修復時間戳與畫面的對應，避免按錯誤時間戳搬動畫格。

- MPC-BE 截圖顯示 1,465 張收到 724 次 PTS 倒退，MFG 輸出 32.6 fps，同時有 1,670 ms ready。`ProcessSample` 在成功接收解碼幀後處理，D3D12 生成等待 D3D11 輸入 fence。`QueueMfg` 與 `Session::Generate` 對倒退 PTS 重設歷史，未生成的候選不一定計入 `skipped/s`。`ready` 是最遠已完成的成品時間，無法反映補幀在此範圍是否齊全。
- 同一 480 張、同一 D3D11 NV12 送幀順序與 AX，保留 SR 與兩個 ZipDepth 選項。測試程式按已驗證畫面順序給予 29.970 fps 測試時間戳後，補幀從 260 張增到 463 張，15 個完整秒平均從 46.67 提升到 59.73 次 Present/s。時間戳只在測試程式調整，正式 renderer 未套用推算規則。

## MFG 單獨啟用與成品呈現期限

- 關閉 SR、VSR、ZipDepth 與疊圖，使用相同 480 張 D3D11 NV12 輸入及同一 AX，MFG 工作尺寸 1920×1080。原始 PTS 測得 236 次原始幀的呈現期限插入佇列時小於既有尾端期限。原本的水位算法採最大期限，即使中間有倒退也會顯示較高水位。修正版的 ready 只算到第一個非遞增期限或未完成 GPU fence，queued 仍顯示最遠的期限，OSD 標出目前佇列的倒退或重複期限數。這個水位仍無法代表每個刷新時段都有成品。
- MFG 右側使用固定至少一秒的 QPC 計數窗，分母是實際經過時間，分子是成功 Present 的原始、生成與備援幀數。修正成品路徑漏計未經 MFG 的備援原始幀。兩組共 30 個穩態 OSD 視窗均與獨立成功 Present 事件計數相符，起播的第一個統計窗有一張未被診斷事件覆蓋的備援幀。這項數值量 renderer 成功提交，不是顯示器掃描輸出。
- 同一批畫格只改測試程式的 PTS 對應。佇列期限倒退 236→0，補幀跳過 469→6，15 個穩態視窗平均 Present 次數 55.84→60.09/s。原始 PTS 組有一個視窗約 49.67/s，與實播低至 40 多 fps 的方向一致。時間戳修正只在測試程式，不在 AX。

## 正式 AX 的 AVC 呈現時序修正

- `AvcPresentationClock` 開檔時只讀取 MP4 容器標頭。適用於本機、單一 AVC 視訊軌、單一固定 `stts` 週期、沒有 `ctts` 的非片段 MP4，以及啟用 MFG 的 D3D11 逐行 NV12 正速播放。收到的時間戳和時長須符合檔案的幀格與有限重排序範圍。依解碼器交付的畫面順序，使用檔案的有理數週期計算 renderer 內部時間。原始樣本退出 Receive 時恢復原時間。切換 segment、flush 和 stop 重設相位。資訊不符合時沿用來源時間。
- 已驗證實片的 30000/1001 有理數週期，測試 discontinuity 重新定位、含 `ctts` 和多段 `stts` 的排除條件。正式 AX 接收原始解碼 PTS，未開啟測試程式的 PTS 代換。MFG 單獨啟用的 480 張起播與從 02:00 啟播測試，PTS 和佇列期限倒退皆為 0，平均 59.99、60.05 次 Present/s，最高 ready 1.993、1.968 秒，補幀跳過 7、4 次。起播加開 SR 和兩種 ZipDepth 用途時，480 張原始幀全數送出，佇列期限倒退 0，穩態平均 60.00 次 Present/s，最高 ready 1.958 秒，補幀跳過 29 次。
- 以上是獨立 D3D11 NV12 紋理交付測試。MPC-BE 的實際解碼 graph、中途跳播、音畫同步及掃描輸出仍須本機確認。
