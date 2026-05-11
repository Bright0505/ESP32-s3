#ifndef CONFIG_H
#define CONFIG_H

// [說明] 本檔案為系統預設設定檔。
// 您可以直接在此修改連線資訊，或是（推薦）在 SD 卡根目錄使用 config.txt。
// 設定優先級：SD 卡 config.txt > 本檔案預設值。

const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* apiKey   = "YOUR_GEMINI_API_KEY";
const char* model    = "gemini-1.5-flash";

#endif
