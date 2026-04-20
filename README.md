# ESP32-S3 Gemini AI AMOLED Assistant

基於 **Waveshare ESP32-S3-Touch-AMOLED-1.75** 的 AI 語音助理，整合 Google Gemini API，透過 AMOLED 螢幕顯示回應。

## 硬體

| 元件 | 規格 |
|------|------|
| 開發板 | Waveshare ESP32-S3-Touch-AMOLED-1.75 |
| 螢幕 | 1.75" AMOLED 466×466, CO5300 驅動, QSPI 介面 |
| 麥克風 | ES7210 + I2S（MIC1/2 +30dB gain） |
| PMU | AXP2101 電源管理 |
| 儲存 | MicroSD（FAT32，存放字型檔） |

## 功能

- **短按 BOOT**：由 Gemini 生成今日運勢卡（大吉 / 吉 / ... / 大凶）
- **長按 BOOT**：錄音（最長 4 秒）並送至 Gemini 語音理解，以繁體中文回答
- AMOLED 顯示繁體中文回應（霞鶩文楷 TTF，首次使用自動從 GitHub 下載至 SD 卡）
- WiFi 訊號強度圖示即時顯示（右上角）
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

> Gemini API Key 申請：https://aistudio.google.com/apikey

### 2. 安裝依賴庫（Arduino IDE Library Manager）

- `GFX Library for Arduino` (moononournation)
- `ArduinoJson`
- `XPowersLib`
- `OpenFontRender`

### 3. 編譯並燒錄

**Arduino IDE**：開啟 `GeminiAssistant/GeminiAssistant.ino`，選擇 `ESP32S3 Dev Module`，上傳。

**arduino-cli**：
```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 GeminiAssistant
arduino-cli upload  --fqbn esp32:esp32:esp32s3 --port /dev/cu.usbmodem1101 GeminiAssistant
```

> 若上傳失敗，請先按住 BOOT → 按 RESET → 放開 RESET → 放開 BOOT 進入燒錄模式。

### 4. 首次啟動

裝置會依序自檢：
1. SD Card — 需已插入並格式化為 FAT32
2. WiFi — 連線至 config.h 中設定的 AP
3. Font — 若 `/font.ttf` 不存在，自動從 GitHub 下載霞鶩文楷（約 6MB）

## AI 模型

使用 `gemini-3-flash-preview`，透過 Gemini REST API (`generateContent`) 處理文字與語音（audio/l16）輸入。