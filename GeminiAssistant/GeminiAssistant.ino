#include <Arduino_GFX_Library.h>
#include <Adafruit_GFX.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <mbedtls/base64.h>
#include "OpenFontRender.h"
#include "ofrfs/M5Stack_SD_Preset.h"
#include "XPowersLib.h"
#include "config.h"

/* ── 麥克風（ES7210 + I2S）─────────────────────────────── */
#define MIC_BCLK        9
#define MIC_WS         45
#define MIC_DIN        10
#define MIC_MCLK       42
#define ES7210_ADDR  0x40
#define I2S_PORT    I2S_NUM_0
#define SAMPLE_RATE 16000
#define MAX_REC_SEC     4
#define AUDIO_BUF_SIZE  (MAX_REC_SEC * SAMPLE_RATE * 2)   // 128KB mono 16-bit
#define WAV_HDR_SIZE   44

/* Display (QSPI) */
#define LCD_SDIO0  4
#define LCD_SDIO1  5
#define LCD_SDIO2  6
#define LCD_SDIO3  7
#define LCD_SCLK  38
#define LCD_CS    12
#define LCD_RESET 39
#define IIC_SDA   15
#define IIC_SCL   14

/* 畫面旋轉：0=原始 1=90°CW 2=180° 3=270°CW
   gfx driver 控制 boot screen；CANVAS_ROT 控制主畫面 canvas blit */
#define GFX_ROT     0
#define CANVAS_ROT  0    // ← 調這個讓主畫面旋轉

/* SD Card — Waveshare ESP32-S3-Touch-AMOLED-1.75 官方腳位 */
#define SD_MOSI   1
#define SD_MISO   3
#define SD_SCK    2
#define SD_CS    41

#define DISPLAY_W  466
#define DISPLAY_H  466
#define FONT_SIZE   30
#define FONT_PATH  "/font.ttf"
#define FONT_URL   "https://cdn.jsdelivr.net/gh/lxgw/LxgwWenKai@1.330/fonts/TTF/LXGWWenKaiTC-Regular.ttf"

XPowersAXP2101 power;
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Arduino_CO5300(bus, LCD_RESET, GFX_ROT, DISPLAY_W, DISPLAY_H, 6, 0, 0, 0);

OpenFontRender render;
SPIClass sdSPI(HSPI);
bool fontLoaded = false;
GFXcanvas16 *msgCanvas = nullptr;

// ── 開機狀態面板（全置中版面）──────────────────────────
// textSize=2: 12×16px；每行佔 60px（label+detail+padding）
// 版面（466×466）:
//   y= 35  標題
//   y= 58  分隔線
//   y= 76  SD   行 / y= 96 詳細
//   y=136  WiFi 行 / y=156 詳細
//   y=196  Font 行 / y=216 詳細
//   y=248  分隔線
//   y=268  訊息區

#define ROW_SD      76
#define ROW_WIFI   136
#define ROW_FONT   196
#define ROW_MSG    268
#define CHAR_W2     12
#define CHAR_H2     16

enum BootStatus { BS_PENDING, BS_CHECKING, BS_OK, BS_FAIL };

// 水平置中輔助
int cx(const char* s, int ts = 2) {
    return max(0, (DISPLAY_W - (int)strlen(s) * 6 * ts) / 2);
}

void drawBootScreen() {
    gfx->setRotation(CANVAS_ROT);
    gfx->fillScreen(0x0000);

    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(cx("System Boot Check"), 35);
    gfx->print("System Boot Check");

    gfx->drawFastHLine(30, 58, DISPLAY_W - 60, 0x8410);

    const char* labels[] = { "1. SD Card", "2. WiFi", "3. Font" };
    int rows[] = { ROW_SD, ROW_WIFI, ROW_FONT };
    gfx->setTextColor(0xC618);
    for (int i = 0; i < 3; i++) {
        int lx = (DISPLAY_W - strlen(labels[i]) * CHAR_W2 - 80) / 2;
        gfx->setCursor(lx, rows[i]);
        gfx->print(labels[i]);
    }

    gfx->drawFastHLine(30, 248, DISPLAY_W - 60, 0x8410);
}

void setRowStatus(int row, BootStatus s, const char* detail = "") {
    uint16_t color;
    const char* label;
    switch (s) {
        case BS_CHECKING: color = 0xFFE0; label = "...";  break;
        case BS_OK:       color = 0x07E0; label = "OK";   break;
        case BS_FAIL:     color = 0xF800; label = "FAIL"; break;
        default:          color = 0x7BEF; label = "---";  break;
    }

    // 計算 label 的 x（同 drawBootScreen 邏輯），status 緊接其後
    const char* rowLabels[] = { "1. SD Card", "2. WiFi", "3. Font" };
    int idx = (row == ROW_SD) ? 0 : (row == ROW_WIFI) ? 1 : 2;
    int lw = strlen(rowLabels[idx]) * CHAR_W2;
    int lx = (DISPLAY_W - lw - 80) / 2;
    int sx = lx + lw + 12;   // status 緊接 label 後 12px

    // 清除 status 區域後重繪
    gfx->fillRect(sx, row - 2, DISPLAY_W - sx - 10, CHAR_H2 + 4, 0x0000);
    gfx->setTextColor(color);
    gfx->setTextSize(2);
    gfx->setCursor(sx, row);
    gfx->print(label);

    // 詳細文字（textSize=2，置中）
    int dy = row + CHAR_H2 + 4;
    gfx->fillRect(20, dy, DISPLAY_W - 40, CHAR_H2, 0x0000);
    if (strlen(detail) > 0) {
        char buf[36]; strncpy(buf, detail, 35); buf[35] = '\0';
        gfx->setTextColor(0xAD55);
        gfx->setTextSize(2);
        gfx->setCursor(cx(buf), dy);
        gfx->print(buf);
    }
}

void setProgress(const char* msg) {
    gfx->fillRect(10, ROW_MSG, DISPLAY_W - 20, DISPLAY_H - ROW_MSG - 10, 0x0000);
    gfx->setTextColor(0x07FF);
    gfx->setTextSize(2);
    gfx->setCursor(cx(msg), ROW_MSG);
    gfx->print(msg);
}

void setProgress2(const char* line1, const char* line2) {
    gfx->fillRect(10, ROW_MSG, DISPLAY_W - 20, DISPLAY_H - ROW_MSG - 10, 0x0000);
    gfx->setTextColor(0x07FF);
    gfx->setTextSize(2);
    gfx->setCursor(cx(line1), ROW_MSG);
    gfx->print(line1);
    gfx->setCursor(cx(line2), ROW_MSG + 24);
    gfx->print(line2);
}

// ── 字型下載（WiFi 已連線才呼叫）───────────────────────
bool downloadFont() {
    // 依序嘗試多個來源（正確檔名：LXGWWenKai-Regular.ttf，無 TC）
    const char* urls[] = {
        "https://github.com/lxgw/LxgwWenKai/releases/download/v1.522/LXGWWenKai-Regular.ttf",
        "https://github.com/lxgw/LxgwWenKai/releases/download/v1.500/LXGWWenKai-Regular.ttf",
        "https://github.com/lxgw/LxgwWenKai/releases/download/v1.330/LXGWWenKai-Regular.ttf",
    };

    int code = 0;
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30);

    for (int u = 0; u < 3; u++) {
        setProgress2(urls[u] + 8, "Connecting...");  // 跳過 "https://"
        Serial.printf("Try %d: %s\n", u+1, urls[u]);
        http.begin(client, urls[u]);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setTimeout(60000);
        code = http.GET();
        Serial.printf("HTTP %d  size=%d\n", code, http.getSize());
        if (code == 200) break;
        // 顯示錯誤碼（大字）
        gfx->fillRect(0, 350, DISPLAY_W, 100, 0x0000);
        gfx->setTextColor(0xF800); gfx->setTextSize(3);
        gfx->setCursor(20, 360);
        gfx->printf("HTTP %d  src%d/3", code, u+1);
        http.end();
        delay(1000);
    }

    if (code != 200) {
        setRowStatus(ROW_FONT, BS_FAIL, "All sources failed");
        return false;
    }

    int total = http.getSize();
    if (SD.exists(FONT_PATH)) SD.remove(FONT_PATH);
    File f = SD.open(FONT_PATH, FILE_WRITE);
    if (!f) {
        setRowStatus(ROW_FONT, BS_FAIL, "SD write error");
        http.end(); return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    static uint8_t buf[4096];
    int downloaded = 0;
    uint32_t lastDraw = 0;

    while (http.connected() || stream->available()) {
        int avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
            f.write(buf, n);
            downloaded += n;
            if (millis() - lastDraw > 400) {
                lastDraw = millis();
                char prog[48];
                if (total > 0)
                    snprintf(prog, sizeof(prog), "%d%% (%dKB / %dKB)", downloaded*100/total, downloaded/1024, total/1024);
                else
                    snprintf(prog, sizeof(prog), "%dKB downloaded", downloaded/1024);
                setProgress2("Downloading font...", prog);
                Serial.println(prog);
            }
        } else {
            if (!http.connected()) break;
            delay(10);
        }
    }
    f.close();
    http.end();
    Serial.printf("Downloaded %d bytes\n", downloaded);

    if (downloaded < 1000) {
        SD.remove(FONT_PATH);
        setRowStatus(ROW_FONT, BS_FAIL, "Incomplete download");
        return false;
    }
    return true;
}

// 快速狀態文字（內建字型，無 OFR，瞬間顯示）
void quickMsg(const char* msg, uint16_t color = 0x07FF) {
    gfx->fillScreen(0x0000);
    gfx->setTextColor(color);
    gfx->setTextSize(3);
    int x = max(0, (DISPLAY_W - (int)strlen(msg) * 18) / 2);
    gfx->setCursor(x, 213);
    gfx->print(msg);
    drawWifiIcon();
}

// ── 主顯示函式（canvas buffer → blit）──────────────────
void displayMessage(String text, uint16_t color) {
    // 不在開頭清屏，避免黑屏閃爍；canvas 本身是黑底，blit 時一次替換
    if (!fontLoaded) { Serial.println(text); return; }

    // 第一次使用時建立全螢幕 canvas（PSRAM 分配）
    if (!msgCanvas) {
        msgCanvas = new GFXcanvas16(DISPLAY_W, DISPLAY_H);
        if (!msgCanvas->getBuffer()) { delete msgCanvas; msgCanvas = nullptr; return; }
    }
    msgCanvas->fillScreen(0x0000);

    render.setDrawer(*msgCanvas);
    render.setFontSize(FONT_SIZE);

    int lineCount = 1;
    for (char c : text) if (c == '\n') lineCount++;
    int lineH = FONT_SIZE + 12;
    int y = max(FONT_SIZE, (DISPLAY_H - lineCount * lineH) / 2 + FONT_SIZE);
    int lineStart = 0;

    for (int i = 0; i <= (int)text.length(); i++) {
        if (i == (int)text.length() || text[i] == '\n') {
            String line = text.substring(lineStart, i);
            if (line.length() > 0) {
                int32_t lw = render.getTextWidth("%s", line.c_str());
                int32_t x = max((int32_t)0, ((int32_t)DISPLAY_W - lw) / 2);
                render.setCursor(x, y);
                render.setFontColor(color, 0x0000);
                render.setAlignment(Align::TopLeft);
                render.printf("%s", line.c_str());
            }
            y += lineH;
            lineStart = i + 1;
        }
    }

    // Blit canvas（含軟體旋轉）
    blitCanvas();
    drawWifiIcon();
}

void blitCanvas() {
    uint16_t* src = msgCanvas->getBuffer();
    const int N = DISPLAY_W * DISPLAY_H;

    if (CANVAS_ROT == 0) {
        gfx->draw16bitRGBBitmap(0, 0, src, DISPLAY_W, DISPLAY_H);
        return;
    }
    if (CANVAS_ROT == 2) {
        // 180°：原地反轉再 blit，blit 後還原
        for (int i = 0; i < N / 2; i++) {
            uint16_t t = src[i]; src[i] = src[N-1-i]; src[N-1-i] = t;
        }
        gfx->draw16bitRGBBitmap(0, 0, src, DISPLAY_W, DISPLAY_H);
        for (int i = 0; i < N / 2; i++) {
            uint16_t t = src[i]; src[i] = src[N-1-i]; src[N-1-i] = t;
        }
        return;
    }
    // 90° 或 270°：需要暫存緩衝區
    uint16_t* dst = (uint16_t*)heap_caps_malloc(N * 2, MALLOC_CAP_SPIRAM);
    if (!dst) { gfx->draw16bitRGBBitmap(0, 0, src, DISPLAY_W, DISPLAY_H); return; }
    const int W = DISPLAY_W, H = DISPLAY_H;
    if (CANVAS_ROT == 1) {           // 90° CW
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[x * H + (H-1-y)] = src[y * W + x];
    } else {                          // 270° CW
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[(W-1-x) * H + y] = src[y * W + x];
    }
    gfx->draw16bitRGBBitmap(0, 0, dst, DISPLAY_W, DISPLAY_H);
    heap_caps_free(dst);
}

// ── WiFi 狀態圖示 ────────────────────────────────────────
void drawWifiIcon() {
    const int X0 = 328, Y0 = 95;
    const int BW = 7, GAP = 4;
    const int HEIGHTS[] = { 8, 14, 20, 26 };

    wl_status_t status = WiFi.status();
    bool connected   = (status == WL_CONNECTED);
    bool connecting  = (status == WL_DISCONNECTED || status == WL_IDLE_STATUS);

    int bars = 0;
    if (connected) {
        int32_t rssi = WiFi.RSSI();
        if      (rssi >= -55) bars = 4;
        else if (rssi >= -65) bars = 3;
        else if (rssi >= -75) bars = 2;
        else                  bars = 1;
    }

    gfx->fillRect(X0 - 2, Y0 - 30, (BW + GAP) * 4 + 4, 34, 0x0000);

    for (int i = 0; i < 4; i++) {
        int bx = X0 + i * (BW + GAP);
        int bh = HEIGHTS[i];
        int by = Y0 - bh;
        uint16_t color;
        if (connecting)        color = (millis() / 400 % 2) ? 0xFFE0 : 0x2945; // 黃色閃爍
        else if (!connected)   color = 0xF800;                                   // 紅：斷線
        else if (i < bars)     color = (bars >= 3) ? 0x07E0 : 0xFFE0;           // 綠/黃
        else                   color = 0x2945;                                   // 暗灰
        gfx->fillRect(bx, by, BW, bh, color);
    }
}

// ── ES7210 & I2S 麥克風 ──────────────────────────────────
static void es7210_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES7210_ADDR);
    Wire.write(reg); Wire.write(val);
    Wire.endTransmission();
}

void initES7210() {
    es7210_reg(0x00, 0xFF); delay(20);   // Reset
    es7210_reg(0x00, 0x32);              // Normal
    es7210_reg(0x01, 0x20);              // I2S 16-bit
    es7210_reg(0x02, 0x04);              // MCLK = 256*LRCK
    es7210_reg(0x03, 0x00);
    es7210_reg(0x06, 0x00);
    es7210_reg(0x07, 0x20);
    es7210_reg(0x08, 0x10);
    es7210_reg(0x11, 0xFF);
    es7210_reg(0x12, 0x68);
    es7210_reg(0x40, 0x43);              // MIC1 gain +30dB
    es7210_reg(0x41, 0x43);              // MIC2 gain +30dB
    es7210_reg(0x43, 0x10);
    es7210_reg(0x44, 0x10);
    es7210_reg(0x45, 0x10);
    es7210_reg(0x46, 0x10);
    Serial.println("ES7210 init done");
}

void initMicI2S() {
    i2s_config_t cfg = {
        .mode             = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate      = SAMPLE_RATE,
        .bits_per_sample  = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format   = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count    = 8,
        .dma_buf_len      = 512,
        .use_apll         = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk       = SAMPLE_RATE * 256
    };
    i2s_pin_config_t pins = {
        .mck_io_num    = MIC_MCLK,
        .bck_io_num    = MIC_BCLK,
        .ws_io_num     = MIC_WS,
        .data_out_num  = I2S_PIN_NO_CHANGE,
        .data_in_num   = MIC_DIN
    };
    i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_PORT, &pins);
    Serial.println("I2S mic init done");
}

void buildWAVHeader(uint8_t* h, uint32_t audioBytes) {
    uint32_t fileSize = audioBytes + 36;
    uint32_t sr       = SAMPLE_RATE;
    uint32_t byteRate = sr * 2;
    uint16_t ch=1, fmt=1, align=2, bps=16, fmtSz=16;
    memcpy(h,    "RIFF", 4); memcpy(h+4,  &fileSize, 4);
    memcpy(h+8,  "WAVE", 4); memcpy(h+12, "fmt ", 4);
    memcpy(h+16, &fmtSz, 4); memcpy(h+20, &fmt,   2);
    memcpy(h+22, &ch,    2);  memcpy(h+24, &sr,    4);
    memcpy(h+28, &byteRate,4);memcpy(h+32, &align, 2);
    memcpy(h+34, &bps,   2);  memcpy(h+36, "data", 4);
    memcpy(h+40, &audioBytes, 4);
}

void askGeminiVoice() {
    quickMsg("Recording...", 0xF800);

    // 分配錄音緩衝區（純 PCM，不含 WAV header）
    uint8_t* pcm = (uint8_t*)heap_caps_malloc(AUDIO_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!pcm) { quickMsg("PSRAM error", 0xF800); return; }

    size_t total = 0, got = 0;
    while (digitalRead(0) == LOW && total < AUDIO_BUF_SIZE) {
        i2s_read(I2S_PORT, pcm + total,
                 min((size_t)1024, AUDIO_BUF_SIZE - total), &got, 100);
        total += got;
    }
    Serial.printf("Recorded %u bytes PCM\n", total);

    if (total < 2000) {
        heap_caps_free(pcm);
        quickMsg("Too short!", 0xF800);
        delay(2000);
        return;
    }

    quickMsg("Processing...", 0x07FF);

    // Base64 編碼（PSRAM）
    size_t b64Sz = ((total + 2) / 3) * 4 + 1;
    uint8_t* b64 = (uint8_t*)heap_caps_malloc(b64Sz, MALLOC_CAP_SPIRAM);
    if (!b64) { heap_caps_free(pcm); quickMsg("PSRAM error", 0xF800); return; }
    size_t b64Len = 0;
    mbedtls_base64_encode(b64, b64Sz, &b64Len, pcm, total);
    heap_caps_free(pcm);
    b64[b64Len] = '\0';

    // 組 JSON：使用 audio/l16 raw PCM（不需要 WAV header）
    const char* pre = "{\"contents\":[{\"parts\":[{\"inline_data\":{\"mime_type\":\"audio/l16;rate=16000\",\"data\":\"";
    const char* suf = "\"}},{\"text\":\"請理解語音內容並用繁體中文回答，100字內\"}]}]}";
    size_t jsonSz = strlen(pre) + b64Len + strlen(suf) + 1;
    char* json = (char*)heap_caps_malloc(jsonSz, MALLOC_CAP_SPIRAM);
    if (!json) { heap_caps_free(b64); quickMsg("PSRAM error", 0xF800); return; }
    strcpy(json, pre);
    strcat(json, (char*)b64);
    strcat(json, suf);
    heap_caps_free(b64);

    WiFiClientSecure client; client.setInsecure(); client.setTimeout(30);
    HTTPClient http;
    http.begin(client, "https://generativelanguage.googleapis.com/v1beta/models/gemini-3-flash-preview:generateContent?key=" + String(apiKey));
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(30000);

    int code = http.POST((uint8_t*)json, strlen(json));
    heap_caps_free(json);
    Serial.printf("Voice API: %d\n", code);

    if (code == 200) {
        String resp = http.getString();
        DynamicJsonDocument doc(32768);
        if (!deserializeJson(doc, resp)) {
            displayMessage(String(doc["candidates"][0]["content"]["parts"][0]["text"].as<const char*>()), 0xFFFF);
        } else {
            displayMessage("解析錯誤", 0xF800);
        }
    } else {
        // 顯示詳細錯誤
        String body = http.getString();
        Serial.println(body);
        // 從 error.message 擷取
        DynamicJsonDocument errDoc(2048);
        String errMsg = "HTTP " + String(code);
        if (!deserializeJson(errDoc, body) && errDoc.containsKey("error"))
            errMsg = errDoc["error"]["message"].as<String>().substring(0, 40);
        displayMessage("語音錯誤\n" + errMsg, 0xF800);
    }
    http.end();
    drawWifiIcon();
}

// ── Setup ───────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    Wire.begin(IIC_SDA, IIC_SCL);
    Wire.beginTransmission(0x34); Wire.write(0x90); Wire.write(0xBF); Wire.endTransmission();
    Wire.beginTransmission(0x34); Wire.write(0x92); Wire.write(0x1C); Wire.endTransmission();
    Wire.beginTransmission(0x34); Wire.write(0x93); Wire.write(0x1C); Wire.endTransmission();
    Wire.beginTransmission(0x34); Wire.write(0x94); Wire.write(0x1C); Wire.endTransmission();
    delay(500);

    gfx->begin();
    gfx->fillScreen(0x0000);
    drawBootScreen();

    // ── Step 1: SD Card ──
    setRowStatus(ROW_SD, BS_CHECKING);
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI);
    delay(200);
    bool sdOk = SD.begin(SD_CS, sdSPI, 4000000);
    if (sdOk) {
        char info[32];
        snprintf(info, sizeof(info), "%lluMB", SD.cardSize() / (1024*1024));
        setRowStatus(ROW_SD, BS_OK, info);
        Serial.printf("SD OK  %s\n", info);
    } else {
        setRowStatus(ROW_SD, BS_FAIL, "Check wiring & format");
        Serial.println("SD FAIL");
        while (true) delay(1000);  // 無法繼續，停在這裡
    }

    // ── Step 2: WiFi ──
    setRowStatus(ROW_WIFI, BS_CHECKING);
    WiFi.begin(ssid, password);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries++ < 60) {  // 30秒
        delay(500);
        if (tries % 10 == 0) {  // 每5秒顯示進度
            char prog[20]; snprintf(prog, sizeof(prog), "Trying %d/30s", tries/2);
            setRowStatus(ROW_WIFI, BS_CHECKING, prog);
        }
    }
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    if (wifiOk) {
        setRowStatus(ROW_WIFI, BS_OK, WiFi.localIP().toString().c_str());
        Serial.printf("WiFi OK  %s\n", WiFi.localIP().toString().c_str());
    } else {
        setRowStatus(ROW_WIFI, BS_FAIL, "Check SSID/Password");
        Serial.println("WiFi FAIL");
        // WiFi 失敗但 SD 有字型仍可嘗試載入
    }

    // ── Step 3: Font ──
    setRowStatus(ROW_FONT, BS_CHECKING);
    if (!SD.exists(FONT_PATH)) {
        if (!wifiOk) {
            setRowStatus(ROW_FONT, BS_FAIL, "Need WiFi to download");
            while (true) delay(1000);
        }
        bool dl = downloadFont();
        if (!dl) {
            setProgress("Press BOOT to retry");
            while (digitalRead(0) != LOW) delay(100);
            ESP.restart();
        }
    }

    // 顯示字型檔資訊
    File fi = SD.open(FONT_PATH, FILE_READ);
    uint32_t fsize = fi ? fi.size() : 0;
    if (fi) fi.close();
    char finfo[48];
    snprintf(finfo, sizeof(finfo), "File: %luKB", fsize / 1024);
    setProgress(finfo);
    Serial.printf("Font file size: %lu bytes\n", fsize);
    delay(1000);

    render.setDrawer(*gfx);
    int fontErr = render.loadFont(FONT_PATH);
    if (fontErr != 0) {
        Serial.printf("Font load failed FT error: %d, deleting and re-downloading\n", fontErr);
        SD.remove(FONT_PATH);
        setRowStatus(ROW_FONT, BS_FAIL, "Bad font, re-downloading...");
        delay(1000);
        if (!downloadFont()) {
            setProgress("Press BOOT to retry");
            while (digitalRead(0) != LOW) delay(100);
            ESP.restart();
        }
        fontErr = render.loadFont(FONT_PATH);
        if (fontErr != 0) {
            char errMsg[40];
            snprintf(errMsg, sizeof(errMsg), "FT error: %d (%luKB)", fontErr, fsize/1024);
            setRowStatus(ROW_FONT, BS_FAIL, errMsg);
            Serial.printf("Font load failed again: %d\n", fontErr);
            while (true) delay(1000);
        }
    }
    fontLoaded = true;
    setRowStatus(ROW_FONT, BS_OK, "LXGWWenKaiTC");
    setProgress("Boot complete!");
    Serial.println("Font OK — boot complete");
    delay(1500);

    // ── 初始化麥克風 ──
    initES7210();
    initMicI2S();

    // ── 正常啟動 ──
    displayMessage("連線成功！\n短按：文字  長按：語音", 0xFFFF);
}

// ── Loop ────────────────────────────────────────────────
void loop() {
    // 每秒更新圖示；每 10 秒嘗試重連
    static uint32_t lastIconUpdate = 0;
    static uint32_t lastReconnect  = 0;
    if (millis() - lastIconUpdate > 1000) {
        lastIconUpdate = millis();
        drawWifiIcon();
    }
    if (WiFi.status() != WL_CONNECTED && millis() - lastReconnect > 10000) {
        lastReconnect = millis();
        Serial.println("WiFi lost, reconnecting...");
        WiFi.disconnect();
        delay(100);
        WiFi.begin(ssid, password);
    }

    if (digitalRead(0) == LOW) {
        delay(50);
        if (digitalRead(0) == LOW) {
            uint32_t t0 = millis();
            while (digitalRead(0) == LOW && millis() - t0 < 700) delay(10);

            if (digitalRead(0) == LOW) {
                // 長按（> 700ms）→ 語音輸入
                askGeminiVoice();
                while (digitalRead(0) == LOW) delay(10);
            } else {
                // 短按 → 抽運勢卡
                drawFortuneCard();
            }
        }
    }
}

// ── 運勢卡牌（由 Gemini 生成）────────────────────────────
void drawFortuneCard() {
    askGemini(
        "請幫我抽一張今日運勢卡片。"
        "格式如下，嚴格按照此格式輸出，不要多餘說明：\n"
        "第一行：運勢等級（從「大吉/吉/中吉/小吉/末吉/凶/大凶」擇一）\n"
        "第二行：一句話運勢描述（15字內）\n"
        "第三行：今日建議（15字內）"
    );
}

void askGemini(String prompt) {
    quickMsg("Thinking...", 0x07FF);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);

    HTTPClient http;
    String url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-3-flash-preview:generateContent?key=" + String(apiKey);
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);
    String payload = "{\"contents\":[{\"parts\":[{\"text\":\"" + prompt + " (請用繁體中文回答，100字內)\"}]}]}";

    int code = http.POST(payload);
    Serial.printf("Gemini HTTP %d\n", code);

    if (code == 200) {
        String resp = http.getString();
        DynamicJsonDocument doc(32768);
        DeserializationError err = deserializeJson(doc, resp);
        if (!err) {
            const char* result = doc["candidates"][0]["content"]["parts"][0]["text"];
            displayMessage(String(result), 0xFFFF);
        } else {
            displayMessage("解析錯誤", 0xF800);
        }
    } else {
        String errBody = http.getString();
        Serial.println(errBody);
        displayMessage("API 錯誤\n錯誤碼: " + String(code), 0xF800);
    }
    http.end();
}
