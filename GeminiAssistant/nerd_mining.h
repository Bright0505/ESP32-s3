// Simplified NerdMiner_v2 mining core for FortuneCat integration
#pragma once
#include <stdint.h>
#include <Arduino.h>

// Global mining statistics (written by Core 0, read by Core 1 UI)
extern volatile uint32_t mineHashes;   // hashes in current Mhash window
extern volatile uint32_t mineMhashes;  // completed Mhash
extern volatile uint32_t mineShares;   // shares accepted (32-bit difficulty)
extern volatile uint32_t mineValids;   // valid blocks found
extern volatile uint64_t mineUptime;   // seconds since mining started
extern volatile double   mineBestDiff; // best difficulty seen
extern volatile uint32_t mineKHs;      // kH/s (updated every second)
extern volatile bool     mineActive;   // true = Stratum connected and mining

// Pool settings (read from cfgPool / cfgPoolPort / cfgBtcAddr in main ino)
extern char cfgBtcAddr[128];
extern char cfgPool[128];
extern char cfgPoolPort[8];

// FreeRTOS tasks — started from setup() on Core 0
void runStratumWorker(void* name);  // Stratum + job dispatch
void minerWorkerHw(void* task_id);  // hardware SHA256 miner (ESP32-S3)
