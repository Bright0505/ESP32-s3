#include <Arduino_GFX_Library.h>
#include <vector>
#include <Adafruit_GFX.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SD.h>
#include <SPI.h>
#include "OpenFontRender.h"
#include "ofrfs/M5Stack_SD_Preset.h"
#include "XPowersLib.h"
#include "config.h"

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

// 待機貓咪動畫
static int      gCatFrame     = 0;
static uint32_t gLastCatFrame = 0;
static bool     gIdleMode     = true;   // false = 正在顯示結果，暫停動畫
static uint32_t gResultTime   = 0;      // 上次顯示結果的時間
#define IDLE_TIMEOUT_MS 60000           // 1 分鐘後回到待機動畫

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
    gfx->fillRect(0, ROW_MSG, DISPLAY_W, DISPLAY_H - ROW_MSG - 10, 0x0000);
    gfx->setTextColor(0x07FF);
    gfx->setTextSize(2);
    gfx->setCursor(cx(msg), ROW_MSG);
    gfx->print(msg);
}

void setProgress2(const char* line1, const char* line2) {
    gfx->fillRect(0, ROW_MSG, DISPLAY_W, DISPLAY_H - ROW_MSG - 10, 0x0000);
    gfx->setTextColor(0x07FF);
    gfx->setTextSize(2);
    gfx->setCursor(cx(line1), ROW_MSG);
    gfx->print(line1);
    gfx->setCursor(cx(line2), ROW_MSG + 24);
    gfx->print(line2);
}

// ── 字型下載（WiFi 已連線才呼叫）───────────────────────
bool downloadFont() {
    // PSRAM 8MB，字型檔需 ≤7MB（OpenFontRender 會將整個字型載入 PSRAM）
    // FreeType error 0x40 = Out of Memory
    const char* urls[] = {
        // WenQuanYi Micro Hei (~3.5MB)，繁簡中文皆支援，最小且可靠
        "https://github.com/anthonyfok/fonts-wqy-microhei/raw/master/wqy-microhei.ttc",
        // LXGW WenKai TC 備用（若未來有 ≤7MB 版本）
        "https://cdn.jsdelivr.net/gh/lxgw/LxgwWenKai@1.330/fonts/TTF/LXGWWenKaiTC-Regular.ttf",
        "https://raw.githubusercontent.com/lxgw/LxgwWenKai/1.330/fonts/TTF/LXGWWenKaiTC-Regular.ttf",
    };

    int code = 0;
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20);  // TLS 握手上限 20 秒

    int urlCount = sizeof(urls) / sizeof(urls[0]);
    for (int u = 0; u < urlCount; u++) {
        char connMsg[24];
        snprintf(connMsg, sizeof(connMsg), "Connecting %d/%d...", u + 1, urlCount);
        setProgress2(urls[u] + 8, connMsg);
        Serial.printf("Try %d: %s\n", u+1, urls[u]);
        http.begin(client, urls[u]);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setTimeout(30000);
        code = http.GET();
        int sz = http.getSize();
        Serial.printf("HTTP %d  size=%d\n", code, sz);
        if (code == 200) {
            if (sz > 7 * 1024 * 1024) {
                Serial.printf("Font too large (%dMB > 7MB PSRAM limit), skipping\n", sz / 1024 / 1024);
                http.end();
                code = 0;
                continue;
            }
            break;
        }
        gfx->fillRect(0, 350, DISPLAY_W, 100, 0x0000);
        gfx->setTextColor(0xF800); gfx->setTextSize(3);
        gfx->setCursor(20, 360);
        gfx->printf("HTTP %d  src%d/%d", code, u+1, urlCount);
        http.end();
        delay(1000);
    }

    if (code != 200) {
        setRowStatus(ROW_FONT, BS_FAIL, "All sources failed");
        return false;
    }

    int total = http.getSize();

    // SD 空間確認
    uint64_t freeBytes = SD.totalBytes() - SD.usedBytes();
    Serial.printf("SD free: %llu bytes, need: %d bytes\n", freeBytes, total);
    if (total > 0 && freeBytes < (uint64_t)total) {
        char msg[48];
        snprintf(msg, sizeof(msg), "SD full: %lluMB free", freeBytes / (1024*1024));
        setRowStatus(ROW_FONT, BS_FAIL, msg);
        http.end(); return false;
    }

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
    uint32_t lastRecv = millis();

    while (true) {
        if (total > 0 && downloaded >= total) break;
        int avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
            if (n > 0) {
                size_t written = f.write(buf, n);
                if ((int)written != n) {
                    Serial.printf("SD write failed: wrote %u of %d (disk full?)\n", written, n);
                    f.close(); http.end();
                    setRowStatus(ROW_FONT, BS_FAIL, "SD write failed");
                    return false;
                }
                downloaded += n;
                lastRecv = millis();
                if (millis() - lastDraw > 400) {
                    lastDraw = millis();
                    char prog[48];
                    if (total > 0)
                        snprintf(prog, sizeof(prog), "%d%% (%dKB / %dKB)", (int)((int64_t)downloaded*100/total), downloaded/1024, total/1024);
                    else
                        snprintf(prog, sizeof(prog), "%dKB downloaded", downloaded/1024);
                    setProgress2("Downloading font...", prog);
                    Serial.println(prog);
                }
            }
        } else {
            if (millis() - lastRecv > 30000) { Serial.println("Download stall 30s, giving up"); break; }
            delay(10);
        }
    }
    f.flush();
    f.close();
    http.end();
    setProgress2("Downloading font...", "100% - Verifying...");
    Serial.printf("Downloaded %d bytes\n", downloaded);

    // 驗證 SD 實際寫入大小，避免 flush 不完整留下空檔案
    File verify = SD.open(FONT_PATH, FILE_READ);
    uint32_t actualSize = verify ? verify.size() : 0;
    if (verify) verify.close();
    Serial.printf("SD verify size: %u bytes\n", actualSize);

    // 下載不完整視為失敗（Content-Length 未知時要求至少 1MB）
    bool incomplete = (total > 0) ? (actualSize < (uint32_t)(total * 0.99)) : (actualSize < 1024 * 1024);
    if (incomplete) {
        Serial.printf("Incomplete: SD=%u expected=%d\n", actualSize, total);
        SD.remove(FONT_PATH);
        setRowStatus(ROW_FONT, BS_FAIL, "Incomplete download");
        return false;
    }
    return true;
}

// ── 待機貓咪動畫 ──────────────────────────────────────────
// textSize=3 → 每字 18×24px；11字 × 18 = 198px wide；catX=(466-198)/2=134
void drawIdleCat(int frame) {
    // 無網路：全螢幕顯示離去訊息（bitmap 字型，不需 TTF）
    if (WiFi.status() != WL_CONNECTED) {
        if (!msgCanvas) {
            msgCanvas = new GFXcanvas16(DISPLAY_W, DISPLAY_H);
            if (!msgCanvas->getBuffer()) { delete msgCanvas; msgCanvas = nullptr; return; }
        }
        msgCanvas->fillScreen(0x0000);
        msgCanvas->setTextColor(0xF800, 0x0000);
        msgCanvas->setTextWrap(true);
        const char* line1 = "Without your care,";
        const char* line2 = "I had to drift";
        const char* line3 = "away sadly...";
        msgCanvas->setTextSize(2);
        msgCanvas->setCursor((DISPLAY_W - strlen(line1)*12)/2, 180);
        msgCanvas->print(line1);
        msgCanvas->setCursor((DISPLAY_W - strlen(line2)*12)/2, 210);
        msgCanvas->print(line2);
        msgCanvas->setCursor((DISPLAY_W - strlen(line3)*12)/2, 240);
        msgCanvas->print(line3);
        blitCanvas();
        return;
    }

    // 10 幀循環：REST / BLINK / REST / EAR_L / REST / EAR_R / REST / GROOM / REST / TAIL
    static const char* FRAMES[10][5] = {
        { "           ", "   /\\_/\\   ", "  ( o o )  ", "  (  w  )  ", "  (\")_(\")" },  // 0 REST
        { "           ", "   /\\_/\\   ", "  ( - - )  ", "  (  w  )  ", "  (\")_(\")" },  // 1 BLINK
        { "           ", "   /\\_/\\   ", "  ( o o )  ", "  (  w  )  ", "  (\")_(\")" },  // 2 REST
        { "           ", "   <\\_/\\   ", "  ( o o )  ", "  (  w  )  ", "  (\")_(\")" },  // 3 EAR_L
        { "           ", "   /\\_/\\   ", "  ( o o )  ", "  (  w  )  ", "  (\")_(\")" },  // 4 REST
        { "           ", "   /\\_/>   ", "  ( o o )  ", "  (  w  )  ", "  (\")_(\")" },  // 5 EAR_R
        { "           ", "   /\\_/\\   ", "  ( o o )  ", "  (  w  )  ", "  (\")_(\")" },  // 6 REST
        { "           ", "   /\\_/\\   ", "  ( ^ ^ )  ", "  (  P  )  ", "  (\")_(\")" },  // 7 GROOM
        { "           ", "   /\\_/\\   ", "  ( o o )  ", "  (  w  )  ", "  (\")_(\")" },  // 8 REST
        { "           ", "   /\\_/\\   ", "  ( o o )  ", "  (  w  )  ", " ~(\")_(\")" },  // 9 TAIL
    };

    if (!msgCanvas) {
        msgCanvas = new GFXcanvas16(DISPLAY_W, DISPLAY_H);
        if (!msgCanvas->getBuffer()) { delete msgCanvas; msgCanvas = nullptr; return; }
    }
    msgCanvas->fillScreen(0x0000);

    const int CAT_CHAR_W = 18, CAT_CHAR_H = 24, CAT_COLS = 11, CAT_ROWS = 5;
    int catX = (DISPLAY_W - CAT_CHAR_W * CAT_COLS) / 2; // 134
    int catY = 155;

    msgCanvas->setTextSize(3);
    msgCanvas->setTextColor(0xFD20); // 橘黃
    msgCanvas->setTextWrap(false);
    for (int i = 0; i < CAT_ROWS; i++) {
        msgCanvas->setCursor(catX, catY + i * CAT_CHAR_H);
        msgCanvas->print(FRAMES[frame][i]);
    }

    // 底部提示（textSize=2，白色，置中）
    const char* hint = "Press BOOT to draw";
    int hx = (DISPLAY_W - (int)strlen(hint) * 12) / 2;
    msgCanvas->setTextSize(2);
    msgCanvas->setTextColor(0x8410); // 暗灰
    msgCanvas->setCursor(hx, 310);
    msgCanvas->print(hint);

    renderWifiToCanvas();
    blitCanvas();
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
// 將一段文字依照顯示寬度拆成多行（UTF-8 aware）
// Returns true if the 3-byte UTF-8 sequence at seg[j] is a line-start-forbidden punctuation
// (避頭禁則：。、！？）」』】…～)
static bool isKinsoku(const String& seg, int j) {
    if (j + 2 >= (int)seg.length()) return false;
    uint8_t b0 = seg[j], b1 = seg[j+1], b2 = seg[j+2];
    if (b0 == 0xE3) {
        if (b1 == 0x80 && (b2 == 0x82 || b2 == 0x81)) return true; // 。、
        if (b1 == 0x80 && (b2 == 0x8D || b2 == 0x8F || b2 == 0x8B)) return true; // 」』】
        if (b1 == 0x80 && b2 == 0xBB) return true; // …
    }
    if (b0 == 0xEF && b1 == 0xBC) {
        if (b2 == 0x81 || b2 == 0x9F || b2 == 0x89) return true; // ！？）
    }
    if (b0 == 0xEF && b1 == 0xBD && b2 == 0x9E) return true; // ～
    return false;
}

void wrapSegment(const String& seg, std::vector<String>& out, int maxW = DISPLAY_W - 10) {
    if (seg.length() == 0) { out.push_back(""); return; }
    String cur = "";
    int j = 0;
    while (j < (int)seg.length()) {
        uint8_t c = (uint8_t)seg[j];
        int cl = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        String next = cur + seg.substring(j, j + cl);
        if (render.getTextWidth("%s", next.c_str()) > maxW && cur.length() > 0) {
            // Kinsoku: don't leave sentence-end punctuation at start of next line
            if (cl == 3 && isKinsoku(seg, j)) {
                out.push_back(next); // attach punctuation to current line even if slightly wide
                cur = "";
            } else {
                out.push_back(cur);
                cur = seg.substring(j, j + cl);
            }
        } else {
            cur = next;
        }
        j += cl;
    }
    if (cur.length() > 0) out.push_back(cur);
}

void displayMessage(String text, uint16_t color) {
    if (!fontLoaded) { Serial.println(text); return; }

    // Merge punctuation that Gemini placed on its own line back to the previous line
    text.replace("\n。", "。");
    text.replace("\n、", "、");
    text.replace("\n！", "！");
    text.replace("\n？", "？");
    text.replace("\n）", "）");
    text.replace("\n」", "」");
    text.replace("\n』", "』");

    if (!msgCanvas) {
        msgCanvas = new GFXcanvas16(DISPLAY_W, DISPLAY_H);
        if (!msgCanvas->getBuffer()) { delete msgCanvas; msgCanvas = nullptr; return; }
    }
    msgCanvas->fillScreen(0x0000);
    render.setDrawer(*msgCanvas);
    render.setFontSize(FONT_SIZE);

    // 先拆行（\n 顯式換行 + 自動換行）
    std::vector<String> lines;
    int start = 0;
    for (int i = 0; i <= (int)text.length(); i++) {
        if (i == (int)text.length() || text[i] == '\n') {
            wrapSegment(text.substring(start, i), lines);
            start = i + 1;
        }
    }

    int lineH = FONT_SIZE + 12;
    int y = max(FONT_SIZE, (DISPLAY_H - (int)lines.size() * lineH) / 2);

    for (auto& line : lines) {
        if (line.length() > 0) {
            int32_t lw = render.getTextWidth("%s", line.c_str());
            int32_t x = max((int32_t)0, ((int32_t)DISPLAY_W - lw) / 2);
            render.setCursor(x, y);
            render.setFontColor(color, 0x0000);
            render.setAlignment(Align::TopLeft);
            render.printf("%s", line.c_str());
        }
        y += lineH;
    }

    renderWifiToCanvas();
    blitCanvas();
}

// ── Buddy 對話框顯示 ─────────────────────────────────────
// cat: 5 行 ASCII art（等寬，每字 12px，每行 12 字）
void displayBuddyMessage(const char* cat[5], String text) {
    if (!fontLoaded) { Serial.println(text); return; }

    if (!msgCanvas) {
        msgCanvas = new GFXcanvas16(DISPLAY_W, DISPLAY_H);
        if (!msgCanvas->getBuffer()) { delete msgCanvas; msgCanvas = nullptr; return; }
    }
    msgCanvas->fillScreen(0x0000);
    render.setDrawer(*msgCanvas);

    // ① 上方：ASCII 貓咪（Adafruit GFX bitmap 等寬字型，textSize=2 → 12×16px/字）
    // 字串實際 11 字元寬，以此計算置中 x
    const int CAT_CHAR_W = 12, CAT_CHAR_H = 16, CAT_COLS = 11, CAT_ROWS = 5;
    int catX = (DISPLAY_W - CAT_CHAR_W * CAT_COLS) / 2; // (466-132)/2 = 167
    int catY = 82;
    msgCanvas->setTextSize(2);
    msgCanvas->setTextColor(0xFD20); // 橘黃色
    msgCanvas->setTextWrap(false);
    for (int i = 0; i < CAT_ROWS; i++) {
        msgCanvas->setCursor(catX, catY + i * CAT_CHAR_H);
        msgCanvas->print(cat[i]);
    }

    // ② 下方：對話框（3 行文字）
    const int BOX_X = 18, BOX_Y = 175, BOX_W = 430, BOX_H = 155, BOX_R = 18;
    const int TEXT_X = BOX_X + 18, TEXT_MAX_W = BOX_W - 36;
    const int BUDDY_FONT = 28;
    render.setFontSize(BUDDY_FONT);

    // 合併孤立標點
    text.replace("\n。", "。"); text.replace("\n、", "、");
    text.replace("\n！", "！"); text.replace("\n？", "？");

    // 折行
    std::vector<String> lines;
    int start = 0;
    for (int i = 0; i <= (int)text.length(); i++) {
        if (i == (int)text.length() || text[i] == '\n') {
            wrapSegment(text.substring(start, i), lines, TEXT_MAX_W);
            start = i + 1;
        }
    }

    // 對話框框線（白色）
    msgCanvas->drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, 0xFFFF);
    msgCanvas->drawRoundRect(BOX_X+1, BOX_Y+1, BOX_W-2, BOX_H-2, BOX_R, 0xC618); // 內層淡色

    // 文字渲染（靠左對齊、整體垂直置中於對話框）
    int lineH = BUDDY_FONT + 10;
    // 計算可顯示行數，再由此決定垂直起始位置
    int visibleLines = 0;
    for (auto& line : lines) {
        if (BOX_Y + 10 + (visibleLines + 1) * lineH > BOX_Y + BOX_H - 10) break;
        visibleLines++;
    }
    int totalTextH = visibleLines * lineH;
    int y = BOX_Y + (BOX_H - totalTextH) / 2;
    int rendered = 0;
    for (auto& line : lines) {
        if (rendered >= visibleLines) break;
        if (line.length() > 0) {
            render.setCursor(TEXT_X, y);
            render.setFontColor(0xFFFF, 0x0000);
            render.setAlignment(Align::TopLeft);
            render.printf("%s", line.c_str());
        }
        y += lineH;
        rendered++;
    }

    renderWifiToCanvas();
    blitCanvas();
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

// ── WiFi 狀態圖示（畫進 canvas 避免閃爍，textSize=4 放大版）──
// 5 字 24×32px，水平置中；在 blitCanvas 前呼叫
static void renderWifiToCanvas() {
    if (!msgCanvas) return;

    const int TS = 4;
    const int CW = 6 * TS;   // 24px
    const int CH = 8 * TS;   // 32px
    const int SP = CW + 4;   // 28px 間距
    const int N  = 5;
    const int Y  = 38;
    const int X0 = (DISPLAY_W - ((N-1)*SP + CW)) / 2; // 165

    wl_status_t status = WiFi.status();
    bool connected  = (status == WL_CONNECTED);
    bool connecting = (status == WL_DISCONNECTED || status == WL_IDLE_STATUS);

    if (!connected && !connecting) return; // 無訊號由 drawIdleCat 負責全螢幕訊息

    int count = 0;
    uint16_t faceColor;
    if (connecting) {
        if (!(millis() / 500 % 2)) return;
        count = 1;
        faceColor = 0xFFE0;
    } else {
        int32_t rssi = WiFi.RSSI();
        if      (rssi >= -55) count = 5;
        else if (rssi >= -65) count = 4;
        else if (rssi >= -75) count = 3;
        else if (rssi >= -85) count = 2;
        else                  count = 1;
        faceColor = (count >= 4) ? 0x07E0 : (count >= 2 ? 0xFFE0 : 0xFD20);
    }

    for (int i = 0; i < N; i++) {
        int x = X0 + i * SP;
        if (i < count)
            msgCanvas->drawChar(x, Y, 0x02, faceColor, 0x0000, TS); // ☻
        else
            msgCanvas->drawChar(x, Y, 0x01, 0xC618,    0x0000, TS); // ☺ 灰
    }
}

// 給 quickMsg 等不用 canvas 的場合：簡易圓點
void drawWifiIcon() {
    bool ok = (WiFi.status() == WL_CONNECTED);
    gfx->fillCircle(448, 20, 5, 0x0000);
    gfx->fillCircle(448, 20, 5, ok ? 0x07E0 : 0xF800);
}

// ── Setup ───────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    // 等待 USB-CDC terminal 連線（最多 3 秒），確保 Serial 輸出不遺失
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);

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

    // 顯示字型檔資訊，零位元組直接刪除重下（避免 loadFont 卡死）
    File fi = SD.open(FONT_PATH, FILE_READ);
    uint32_t fsize = fi ? fi.size() : 0;
    if (fi) fi.close();
    Serial.printf("Font file size: %lu bytes\n", fsize);
    Serial.flush();

    if (fsize < 1000) {
        SD.remove(FONT_PATH);
        setRowStatus(ROW_FONT, BS_FAIL, "Bad font, re-downloading...");
        Serial.println("Font too small, re-downloading");
        if (!wifiOk) { while (true) delay(1000); }
        if (!downloadFont()) {
            setProgress("Press BOOT to retry");
            while (digitalRead(0) != LOW) delay(100);
            ESP.restart();
        }
        fi = SD.open(FONT_PATH, FILE_READ);
        fsize = fi ? fi.size() : 0;
        if (fi) fi.close();
        Serial.printf("Re-downloaded font size: %lu bytes\n", fsize);
        Serial.flush();
    }

    char finfo[48];
    snprintf(finfo, sizeof(finfo), "File: %luKB", fsize / 1024);
    setProgress(finfo);
    delay(500);

    render.setDrawer(*gfx);
    Serial.printf("Free internal heap: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Serial.printf("Free PSRAM:         %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    Serial.printf("Loading font from SD (%luKB)...\n", fsize / 1024);
    Serial.flush();
    int fontErr = render.loadFont(FONT_PATH);
    Serial.printf("render.loadFont returned: %d\n", fontErr);
    Serial.flush();
    if (fontErr != 0) {
        // 字型載入失敗：刪除後讓用戶手動按 BOOT 重啟重下
        // 不自動重試，避免無限下載循環
        char errMsg[48];
        snprintf(errMsg, sizeof(errMsg), "FT err %d  %luKB", fontErr, fsize / 1024);
        setRowStatus(ROW_FONT, BS_FAIL, errMsg);
        setProgress("Delete & BOOT to retry");
        Serial.printf("Font load failed: FT error %d, file %lu bytes\n", fontErr, fsize);
        Serial.flush();
        SD.remove(FONT_PATH);  // 刪掉，下次開機重下
        while (digitalRead(0) != LOW) delay(100);
        ESP.restart();
    }
    fontLoaded = true;
    setRowStatus(ROW_FONT, BS_OK, "LXGWWenKaiTC");
    setProgress("Press BOOT to start");
    Serial.println("Font OK — waiting for BOOT button");
    while (digitalRead(0) != LOW) delay(50);
    while (digitalRead(0) == LOW)  delay(50);  // 等放開

    // ── 立即顯示主待機畫面（按鍵說明）──
    drawIdleCat(0);
}

// ── Loop ────────────────────────────────────────────────
void loop() {
    // 結果顯示中：1 分鐘無操作後回到待機動畫
    if (!gIdleMode && millis() - gResultTime > IDLE_TIMEOUT_MS) {
        gIdleMode = true;
        gCatFrame = 0;
        gLastCatFrame = millis();
        drawIdleCat(0);
    }

    // 待機動畫：每 600ms 換幀（僅 idle 模式）
    if (gIdleMode && millis() - gLastCatFrame > 600) {
        gLastCatFrame = millis();
        gCatFrame = (gCatFrame + 1) % 10;
        drawIdleCat(gCatFrame);
    }

    // 每 10 秒嘗試重連
    static uint32_t lastReconnect = 0;
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
            while (digitalRead(0) == LOW) delay(10);
            drawFortuneCard();
            // 抽籤完畢：暫停動畫，記錄時間，1 分鐘後自動恢復
            gIdleMode   = false;
            gResultTime = millis();
        }
    }
}

// ── 運勢卡牌（Buddy 貓咪大姐姐版）──────────────────────
void drawFortuneCard() {
    // ASCII art 貓咪（來自 claude-desktop-buddy cat.cpp），依運勢等級選情緒
    static const char* CAT_ARTS[7][5] = {
        // 0 大吉 — celebrate JUMP  (eyes/body centered at col 5, 1 inner space)
        { "  \\^   ^/  ", "   /\\_/\\   ", "  ( ^ ^ )  ", "  (  W  )  ", "  (\")_(\")" },
        // 1 吉 — heart DREAMY
        { "           ", "   /\\_/\\   ", "  ( ^ ^ )  ", "  (  u  )  ", "  (\")_(\")~" },
        // 2 中吉 — idle REST
        { "           ", "   /\\_/\\   ", "  ( o o )  ", "  (  w  )  ", "  (\")_(\")" },
        // 3 小吉 — idle BLINK
        { "           ", "   /\\_/\\   ", "  ( - - )  ", "  (  w  )  ", "  (\")_(\")" },
        // 4 末吉 — idle SLOW_BL
        { "           ", "   /\\-/\\   ", "  ( _ _ )  ", "  (  w  )  ", "  (\")_(\")" },
        // 5 凶 — dizzy WOOZY
        { "           ", "   /\\_/\\   ", "  ( x @ )  ", "  (  v  )  ", "  (\")_(\")~" },
        // 6 大凶 — dizzy SPLAT
        { "           ", "   /\\_/\\   ", "  ( @ @ )  ", "  (  -  )  ", " (\")_(\")~" },
    };

    const char* levels[] = { "大吉", "吉", "中吉", "小吉", "末吉", "凶", "大凶" };
    int idx = (int)(esp_random() % 7);
    const char* level = levels[idx];

    quickMsg("Thinking...", 0x07FF);

    char prompt[400];
    snprintf(prompt, sizeof(prompt),
        "你是一隻溫柔的貓咪大姐姐。"
        "請用溫柔、關心、帶點可愛的語氣，幫我解讀今日「%s」的運勢。"
        "一段話，40字內，繁體中文，句尾可以加「喵～」。",
        level);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);
    HTTPClient http;
    String url = "https://generativelanguage.googleapis.com/v1beta/models/" + String(model) + ":generateContent?key=" + String(apiKey);
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);
    String payload = "{\"contents\":[{\"parts\":[{\"text\":\"" + String(prompt) + "\"}]}]}";

    int code = http.POST(payload);
    Serial.printf("Fortune HTTP %d\n", code);

    if (code == 200) {
        String resp = http.getString();
        DynamicJsonDocument doc(32768);
        if (!deserializeJson(doc, resp)) {
            const char* result = doc["candidates"][0]["content"]["parts"][0]["text"];
            displayBuddyMessage(CAT_ARTS[idx], String(result ? result : "喵～"));
        } else {
            displayMessage("解析錯誤", 0xF800);
        }
    } else {
        Serial.println(http.getString());
        displayMessage("API 錯誤\n" + String(code), 0xF800);
    }
    http.end();
}

