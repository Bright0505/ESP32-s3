#ifndef CONFIG_H
#define CONFIG_H

// [說明] 本檔案為系統預設設定檔。
// 您可以直接在此修改連線資訊，或是（推薦）在 SD 卡根目錄使用 config.txt。
// 設定優先級：SD 卡 config.txt > NVS (BLE) > 本檔案預設值。

// WiFi 連線預設值
#define DEFAULT_SSID     "YOUR_WIFI_SSID"
#define DEFAULT_PASSWORD "YOUR_WIFI_PASSWORD"

// Gemini API 預設值
#define DEFAULT_API_KEY  "YOUR_GEMINI_API_KEY"
#define DEFAULT_MODEL    "gemini-1.5-flash"

// 比特幣挖礦預設值 (NerdMiner)
#define DEFAULT_BTC_ADDR "YOUR_BTC_ADDRESS"
#define DEFAULT_POOL     "public-pool.io"
#define DEFAULT_POOL_PORT "21496"

#endif
