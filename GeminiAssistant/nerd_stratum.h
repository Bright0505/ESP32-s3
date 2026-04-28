// Adapted from NerdMiner_v2 (https://github.com/BitMaker-hub/NerdMiner_v2)
// Stripped of NerdMiner-specific display/monitor/cJSON dependencies.
#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#define MAX_MERKLE_BRANCHES 32
#define BUFFER_JSON_DOC     4096
#define BUFFER              1024
#define TARGET_BUFFER_SIZE  64

typedef struct {
    String sub_details;
    String extranonce1;
    String extranonce2;
    int    extranonce2_size;
    char   wName[128];
    char   wPass[20];
} mining_subscribe;

typedef struct {
    String    job_id;
    String    prev_block_hash;
    String    coinb1;
    String    coinb2;
    String    nbits;
    JsonArray merkle_branch;
    String    version;
    uint32_t  target;
    String    ntime;
    bool      clean_jobs;
} mining_job;

typedef struct {
    uint8_t bytearray_target[32];
    uint8_t bytearray_pooltarget[32];
    uint8_t merkle_result[32];
    uint8_t bytearray_blockheader[128];
} miner_data;

typedef enum {
    STRATUM_SUCCESS,
    STRATUM_UNKNOWN,
    STRATUM_PARSE_ERROR,
    MINING_NOTIFY,
    MINING_SET_DIFFICULTY
} stratum_method;

#define DEFAULT_DIFFICULTY 0.00015
#define KEEPALIVE_TIME_ms       30000
#define POOLINACTIVITY_TIME_ms  60000

unsigned long getNextId(unsigned long id);
bool verifyPayload(String* line);

mining_subscribe init_mining_subscribe(void);
bool tx_mining_subscribe(WiFiClient& client, mining_subscribe& mSubscribe);
bool parse_mining_subscribe(String line, mining_subscribe& mSubscribe);

bool tx_mining_auth(WiFiClient& client, const char* user, const char* pass);
stratum_method parse_mining_method(String line);
bool parse_mining_notify(String line, mining_job& mJob);
bool tx_mining_submit(WiFiClient& client, mining_subscribe mWorker, mining_job mJob, unsigned long nonce, unsigned long& submit_id);
bool parse_mining_set_difficulty(String line, double& difficulty);
bool tx_suggest_difficulty(WiFiClient& client, double difficulty);
unsigned long parse_extract_id(const String& line);

// Utilities
uint8_t  hex_char(char ch);
int      to_byte_array(const char* in, size_t in_size, uint8_t* out);
double   diff_from_target(void* target);
bool     isSha256Valid(const void* sha256);
bool     checkValid(unsigned char* hash, unsigned char* target);
miner_data init_miner_data(void);
miner_data calculateMiningData(mining_subscribe& mWorker, mining_job mJob);
