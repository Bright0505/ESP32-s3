# ESP32-S3 Gemini AI AMOLED Assistant

這是一個基於 ESP32-S3 的 AI 助理專案，搭載 1.75 吋 AMOLED 螢幕，並整合 Google Gemini 3.1 AI。

## 硬體設備
- **開發板**: Waveshare ESP32-S3-Touch-AMOLED-1.75
- **螢幕**: 1.75" AMOLED (466x466), CO5300 驅動, QSPI 介面
- **PMU**: AXP2101 電源管理單元

## 功能
- 透過 QSPI 驅動高品質 AMOLED 螢幕。
- 整合 Google Gemini 3.1 Flash Lite API 進行智慧問答。
- 支援透過 Serial 埠輸入問題。
- 支援實體按鈕 (BOOT) 觸發預設問題。

## 如何開始
1. 進入 `GeminiAssistant` 資料夾。
2. 將 `config.h.example` 複製並更名為 `config.h`。
3. 在 `config.h` 中填入你的 Wi-Fi SSID、密碼與 Gemini API Key。
4. 使用 Arduino IDE 或 `arduino-cli` 編譯並燒錄。

## 依賴庫
- `GFX Library for Arduino`
- `ArduinoJson`
- `XPowersLib`
