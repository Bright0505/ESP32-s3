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
- 待機時播放 ASCII 貓咪動畫（眨眼、耳朵動、舔爪、尾巴擺動）
- 結果顯示 1 分鐘後自動回到待機動畫
- WiFi 狀態圓點即時顯示（右上角，綠/黃/紅）
- 開機自檢畫面（SD / WiFi / Font 三步驟）

## 快速開始

### 1. 設定 config.h

```bash
cp GeminiAssistant/config.h.example GeminiAssistant/config.h
```

編輯 `GeminiAssistant/config.h`，填入：

```cpp
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* apiKey   = "YOUR_GEMINI_API_KEY";
```

> API Key 申請：https://aistudio.google.com/apikey

### 2. 安裝依賴庫（Arduino IDE Library Manager）

- `GFX Library for Arduino` (moononournation)
- `ArduinoJson`
- `XPowersLib`
- `OpenFontRender`

### 3. 編譯並燒錄

```bash
# 編譯（FQBN 需包含 CDCOnBoot=cdc，否則 Serial 無輸出）
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi --output-dir build GeminiAssistant/

# 燒錄
arduino-cli upload --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi --port /dev/cu.usbmodem1101 --input-dir build GeminiAssistant/
```

> 若上傳失敗，請先按住 BOOT → 按 RESET → 放開 RESET → 放開 BOOT 進入燒錄模式。

### 4. 首次啟動

裝置會依序自檢：
1. **SD Card** — 需已插入並格式化為 FAT32
2. **WiFi** — 連線至 config.h 中設定的 AP
3. **Font** — 若 `/font.ttf` 不存在或損毀，自動從 GitHub 下載霞鶩文楷（約 5MB）

自檢完成後進入貓咪待機動畫，按 BOOT 即可抽籤。

## AI 模型

透過 [Google Gemini API](https://ai.google.dev/) 生成運勢內容，預設使用 `gemini-3-flash-preview`。
可在 `GeminiAssistant/config.h` 中修改 `model` 欄位替換為其他 Gemini 模型。

## 致謝

貓咪 ASCII art 角色設計來自 [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)（Anthropic），依原始設計改編用於本專案的運勢及待機動畫。
