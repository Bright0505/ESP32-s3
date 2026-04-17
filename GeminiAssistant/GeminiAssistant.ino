#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "XPowersLib.h"
#include "config.h"

/* 官方確定的 1.75" AMOLED 腳位 */
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39
#define LCD_WIDTH 466
#define LCD_HEIGHT 466
#define IIC_SDA 15
#define IIC_SCL 14

XPowersAXP2101 power;
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

void setup() {
    Serial.begin(115200);
    pinMode(0, INPUT_PULLUP);
    
    // 1. 強啟電源序列 (增加延時)
    Wire.begin(IIC_SDA, IIC_SCL);
    if (!power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 Fail!");
    } else {
        Serial.println("AXP2101 Online. Stabilizing...");
        power.setALDO1Voltage(1800); power.enableALDO1();
        power.setALDO2Voltage(3300); power.enableALDO2();
        power.setDLDO1Voltage(3300); power.enableDLDO1();
        delay(500); // 關鍵：等待電壓穩定
    }
    
    // 2. 啟動顯示 (不載入重型庫)
    if (!gfx->begin()) {
        Serial.println("GFX Fail!");
    }
    gfx->fillScreen(0x0000);
    
    // 暫時使用標準字體確認螢幕是否亮起
    gfx->setTextColor(0x07E0);
    gfx->setTextSize(4);
    gfx->setCursor(20, 150);
    gfx->println("RECOVERING...");
    
    // 3. WiFi
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    
    gfx->fillScreen(0x0000);
    gfx->setCursor(20, 100);
    gfx->println("SYSTEM READY");
}

void loop() {
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input.length() > 0) {
            askGemini(input);
        }
    }
    if (digitalRead(0) == LOW) {
        delay(50);
        if (digitalRead(0) == LOW) {
            askGemini("Hello Gemini, please introduce yourself.");
            while(digitalRead(0) == LOW);
        }
    }
}

void askGemini(String prompt) {
    gfx->fillScreen(0x0000);
    gfx->setCursor(20, 50);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07FF);
    gfx->println("Processing...");

    HTTPClient http;
    String url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.1-flash-lite-preview:generateContent?key=" + String(apiKey);
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    String payload = "{\"contents\":[{\"parts\":[{\"text\":\"" + prompt + "\"}]}]}";
    
    int code = http.POST(payload);
    if (code == 200) {
        String resp = http.getString();
        DynamicJsonDocument doc(4096);
        deserializeJson(doc, resp);
        const char* result = doc["candidates"][0]["content"]["parts"][0]["text"];
        
        gfx->fillScreen(0x0000);
        gfx->setCursor(10, 40);
        gfx->setTextColor(0xFFFF);
        gfx->setTextWrap(true);
        gfx->println(result); // 如果是中文，此版可能暫時顯示亂碼
        Serial.println(result);
    }
    http.end();
}
