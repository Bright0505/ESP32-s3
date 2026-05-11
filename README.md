# ESP32-S3 AI Fortune Cat

基於 **Waveshare ESP32-S3-Touch-AMOLED-1.75** 的 AI 抽籤裝置，整合 Gemini API，以 ASCII 貓咪角色顯示今日運勢。

## 硬體

| 元件 | 規格 |
|------|------|
| 開發板 | Waveshare ESP32-S3-Touch-AMOLED-1.75 |
| 螢幕 | 1.75" AMOLED 466×466（圓形）, CO5300 驅動, QSPI 介面 |
| PMU | AXP2101 電源管理 |
| 儲存 | MicroSD（FAT32，存放字型檔） |

## 功能

- **按下 BOOT**：抽今日運勢（大吉 / 吉 / 中吉 / 小吉 / 末吉 / 凶 / 大凶）
- 貓咪 AI 大姐姐以繁體中文解讀運勢（霞鶩文楷 TTF）
- 待機時播放 ASCII 貓咪動畫，同時顯示 **比特幣挖礦統計** (Hashrate, Shares, etc.)
- **效能優化**：字體 PSRAM 預載入與 HTTP 長連線，解籤回應僅需 2-5 秒
- 結果顯示 1 分鐘後自動回到待機動畫
- WiFi 狀態圓點即時顯示（右上角，綠/黃/紅）
- 開機自檢畫面（SD / WiFi / Font 三步驟）

## 快速開始

### 1. 設定連線資訊 (二選一)

#### A. 使用 SD 卡 (推薦)
在 SD 卡根目錄建立 `config.txt`，內容參考 `GeminiAssistant/config.txt.example`：
```text
SSID="您的 WiFi 名稱"
PASS="您的 WiFi 密碼"
API_KEY="您的 Gemini API Key"
MODEL="gemini-1.5-flash"
BTC_ADDR="您的比特幣地址"
```

#### B. 硬寫入程式碼
直接編輯 `GeminiAssistant/config.h` 並填入對應資訊。

> API Key 申請：https://aistudio.google.com/apikey

### 2. 安裝依賴庫（Arduino IDE Library Manager）

- `GFX Library for Arduino` (moononournation)
- `ArduinoJson`
- `XPowersLib`
- `OpenFontRender`

### 3. 編譯並燒錄

**注意：** 本專案因整合 NerdMiner 與字體優化，必須使用 `PartitionScheme=huge_app` 分區表。

```bash
# 編譯
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,PartitionScheme=huge_app --output-dir build GeminiAssistant/

# 燒錄
arduino-cli upload --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,PartitionScheme=huge_app --port /dev/cu.usbmodem1101 --input-dir build GeminiAssistant/
```

### 4. Serial Monitor (除錯與監控)

燒錄後可使用內附的 `monitor.py` 觀察日誌與挖礦狀態。裝置開機後會等待 3 秒供 Serial 連接。

```bash
python3 monitor.py
```

### 5. 首次啟動

裝置會依序自檢：
1. **SD Card** — 需已插入並格式化為 FAT32
2. **Config** — 自動讀取 SD 卡或內部儲存的設定
3. **WiFi** — 連線至網路；成功後自動啟動背景挖礦
4. **Font** — 若 `/font.ttf` 不存在，自動下載並**載入至 PSRAM** 以加速渲染

## BLE 設定

WiFi 連線失敗時裝置自動進入 BLE 配對模式，使用手機 BLE UART app 傳送 JSON 填入設定：

```json
{"ssid":"MyWiFi","pass":"password","key":"AIzaSy...","model":"gemini-1.5-flash"}
```

設定儲存至 NVS，重啟後自動連線。

## AI 模型

透過 [Google Gemini API](https://ai.google.dev/) 生成內容，建議使用 `gemini-1.5-flash`。

## 致謝

貓咪 ASCII art 角色設計來自 [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)（Anthropic），依原始設計改編用於本專案的運勢及待機動畫。
