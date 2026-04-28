# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 編譯與燒錄

FQBN 必須包含 `CDCOnBoot=cdc`，否則 `Serial.printf` 不會透過 USB 輸出。

加入 NerdMiner 後 sketch 超過 1.25MB 預設分區，**必須加 `PartitionScheme=huge_app`**（3MB app，無 OTA）。

```bash
# 編譯
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,PartitionScheme=huge_app --output-dir build GeminiAssistant/

# 燒錄
arduino-cli upload --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,PartitionScheme=huge_app --port /dev/cu.usbmodem1101 --input-dir build GeminiAssistant/
```

## Serial Monitor

燒錄完畢後立即連接（裝置開機後會等待最多 3 秒讓 terminal 接上）：

```bash
python3 monitor.py   # 同時輸出至 log.txt，執行 600 秒
```

> 不可同時開多個 serial monitor，會互相衝突導致無輸出。

## 硬體

**Waveshare ESP32-S3-Touch-AMOLED-1.75**
- 顯示：466×466 QSPI CO5300 AMOLED（圓形，角落有裁切）
- PSRAM：8MB OPI（字型整個載入 PSRAM，上限 7MB）
- 電源管理：AXP2101（I2C addr 0x34）
- 按鈕：GPIO 0（Boot 鍵）

## 程式架構

單一 sketch `GeminiAssistant/GeminiAssistant.ino`，credentials 存在 `GeminiAssistant/config.h`（不進 git）。

### 開機流程（setup）
1. Serial → Wire/AXP2101 電源初始化 → display 初始化
2. SD 卡掛載（失敗則停機）
3. WiFi 連線；連線成功後在 Core 0 啟動 `runStratumWorker`（Bitcoin 挖礦）
4. 字型下載/驗證（存 SD `/font.ttf`，PSRAM 載入）
5. 顯示 `drawIdleCat(0)` — 貓咪動畫開始

### 按鈕操作
| 操作 | 觸發 |
|---|---|
| 按下 Boot | `drawFortuneCard()` — 抽運勢，顯示 Buddy 貓咪 + 籤詩，結束後自動回到動畫 |

### 主要函式
| 函式 | 說明 |
|---|---|
| `drawIdleCat(frame)` | 待機動畫，10 幀循環（REST/BLINK/EAR_L/EAR_R/GROOM/TAIL），每幀 600ms；貓咪下方每幀同時顯示挖礦統計 |
| `drawFortuneCard()` | 亂數決定運勢等級，選對應 ASCII 貓咪，送溫柔大姐姐 prompt 給 Gemini |
| `displayBuddyMessage(cat, text)` | 上方渲染 ASCII 貓咪 + 下方對話框顯示文字 |
| `displayMessage(text, color)` | TTF 渲染文字至 canvas，支援自動換行與避頭禁則 |
| `wrapSegment(seg, out, maxW)` | UTF-8 aware 斷行，含避頭禁則；maxW 預設 `DISPLAY_W-10` |
| `drawWifiIcon()` | 右上角小圓點（綠/黃/紅）顯示 WiFi 狀態 |

### NerdMiner 比特幣挖礦（nerd_mining / nerd_stratum）

`runStratumWorker`（Core 0，priority 3）：連接 Stratum pool → 派發 job → 啟動 `minerWorkerHw` task 做 SHA256 硬體運算。

全域統計變數（Core 0 寫，Core 1 UI 讀）：

| 變數 | 說明 |
|---|---|
| `mineKHs` | 當前 hash rate（kH/s） |
| `mineShares` | 提交 share 數 |
| `mineValids` | 有效區塊數 |
| `mineUptime` | 挖礦秒數 |
| `mineBestDiff` | 見過的最佳 difficulty |
| `mineActive` | Stratum 連線狀態 |

Pool 設定（`cfgPool`、`cfgPoolPort`、`cfgBtcAddr`）來自 `config.h` 或 BLE provisioning 存入 NVS。

### 顯示系統

三種渲染路徑：

| 用途 | 方法 |
|---|---|
| 開機狀態面板 | `gfx->print()` 直接寫入（textSize=2，12×16px）|
| 待機動畫、Buddy 貓咪 | `msgCanvas->print()` — Adafruit GFX bitmap 等寬字型 |
| 中文文字、對話框內文 | `OpenFontRender` TTF（LXGWWenKai，從 SD 載入 PSRAM）|

- `GFXcanvas16` canvas 做離屏渲染，完成後 `blitCanvas()` 一次輸出
- `displayMessage()` 與 `displayBuddyMessage()` 都會自動合併 Gemini 回傳的孤立標點行（`\n。` → `。`）
- `CANVAS_ROT` 控制主畫面旋轉（0/2 已實作，1/3 需額外轉置邏輯）

### 待機動畫（drawIdleCat）

```
textSize=3（18×24px/字）
catX = (466 - 11×18) / 2 = 134
catY = 155，貓咪區 y=155~275
提示文字 y=310（textSize=2，灰色）
```

10 幀循環（每幀 600ms，全循環 6 秒）：REST → BLINK → REST → EAR_L → REST → EAR_R → REST → GROOM → REST → TAIL

全域狀態：`gCatFrame`、`gLastCatFrame`，在 `loop()` 計時驅動。

### Buddy 貓咪版面（displayBuddyMessage）

```
y=82~162   ASCII 貓咪（5 行，textSize=2，catX=167）
y=175~330  對話框（圓角矩形，白框，最多 3 行中文，垂直置中）
```

貓咪 ASCII art 來自 [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) `cat.cpp`，
7 種情緒對應 7 個運勢等級（大吉=celebrate、凶=dizzy、大凶=splat 等）。
等寬字型關鍵：每字串必須維持 **11 字元寬**，`CAT_COLS=11`，眼睛/身體行左右對稱（1 個內部空格）。

### WiFi 狀態圖示

右上角圓點 (x=448, y=20, r=7)：
- 🟢 綠（rssi ≥ -65 dBm）
- 🟡 黃（連線但訊號弱，或連線中閃爍）
- 🔴 紅（斷線）

## 注意事項

- 字型檔超過 7MB 會在 `downloadFont()` 被跳過（FreeType error 0x40 = OOM）
- `WiFiClientSecure` 使用 `setInsecure()`（不驗證 TLS 憑證）
- Gemini model 與 API endpoint 硬寫在 `drawFortuneCard()` 內
- `ArduinoJson` 7.x API（`DynamicJsonDocument` 仍支援但為相容層）
- 圓形螢幕邊角會裁切：x=18 的 UI 元素在 y<143 或 y>323 時會被遮住
- 不要在待機畫面使用特定 AI 模型名稱（未來可能換模型）
- Sketch 約 1.5MB，超出預設 1.25MB 分區，必須用 `PartitionScheme=huge_app`（無 OTA）
