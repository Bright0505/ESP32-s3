// Adapted from NerdMiner_v2 (https://github.com/BitMaker-hub/NerdMiner_v2)
#include "nerd_stratum.h"
#include "mbedtls/sha256.h"
#include <string.h>

#define CURRENT_VERSION "FortuneCat-1.0"

static StaticJsonDocument<BUFFER_JSON_DOC> doc;
static unsigned long s_id = 1;

unsigned long getNextId(unsigned long id) {
    return (id == ULONG_MAX) ? 1 : id + 1;
}

bool verifyPayload(String* line) {
    if (line->length() == 0) return false;
    line->trim();
    return !line->isEmpty();
}

static bool checkError(const StaticJsonDocument<BUFFER_JSON_DOC>& d) {
    if (!d.containsKey("error")) return false;
    if (d["error"].size() == 0) return false;
    Serial.printf("STRATUM ERROR: %d | %s\n", (int)d["error"][0], (const char*)d["error"][1]);
    return true;
}

mining_subscribe init_mining_subscribe(void) {
    mining_subscribe s;
    s.extranonce1 = "";
    s.extranonce2 = "";
    s.extranonce2_size = 0;
    s.sub_details = "";
    return s;
}

bool tx_mining_subscribe(WiFiClient& client, mining_subscribe& mSubscribe) {
    char payload[BUFFER] = {0};
    s_id = 1;
    snprintf(payload, sizeof(payload),
        "{\"id\":%lu,\"method\":\"mining.subscribe\",\"params\":[\"%s\"]}\n",
        s_id, CURRENT_VERSION);
    Serial.printf("[STRATUM] ==> subscribe\n");
    client.print(payload);
    vTaskDelay(200 / portTICK_PERIOD_MS);
    String line = client.readStringUntil('\n');
    return parse_mining_subscribe(line, mSubscribe);
}

bool parse_mining_subscribe(String line, mining_subscribe& mSubscribe) {
    if (!verifyPayload(&line)) return false;
    DeserializationError err = deserializeJson(doc, line);
    if (err || checkError(doc)) return false;
    if (!doc.containsKey("result")) return false;
    mSubscribe.sub_details      = String((const char*)doc["result"][0][0][1]);
    mSubscribe.extranonce1      = String((const char*)doc["result"][1]);
    mSubscribe.extranonce2_size = doc["result"][2];
    if (mSubscribe.extranonce1.length() == 0) return false;
    return true;
}

bool tx_mining_auth(WiFiClient& client, const char* user, const char* pass) {
    char payload[BUFFER] = {0};
    s_id = getNextId(s_id);
    snprintf(payload, sizeof(payload),
        "{\"params\":[\"%s\",\"%s\"],\"id\":%lu,\"method\":\"mining.authorize\"}\n",
        user, pass, s_id);
    client.print(payload);
    vTaskDelay(200 / portTICK_PERIOD_MS);
    return true;
}

stratum_method parse_mining_method(String line) {
    if (!verifyPayload(&line)) return STRATUM_PARSE_ERROR;
    DeserializationError err = deserializeJson(doc, line);
    if (err || checkError(doc)) return STRATUM_PARSE_ERROR;
    if (!doc.containsKey("method")) {
        return doc["error"].isNull() ? STRATUM_SUCCESS : STRATUM_UNKNOWN;
    }
    if (strcmp("mining.notify",         (const char*)doc["method"]) == 0) return MINING_NOTIFY;
    if (strcmp("mining.set_difficulty", (const char*)doc["method"]) == 0) return MINING_SET_DIFFICULTY;
    return STRATUM_UNKNOWN;
}

bool parse_mining_notify(String line, mining_job& mJob) {
    if (!verifyPayload(&line)) return false;
    DeserializationError err = deserializeJson(doc, line);
    if (err || !doc.containsKey("params")) return false;
    mJob.job_id          = String((const char*)doc["params"][0]);
    mJob.prev_block_hash = String((const char*)doc["params"][1]);
    mJob.coinb1          = String((const char*)doc["params"][2]);
    mJob.coinb2          = String((const char*)doc["params"][3]);
    mJob.merkle_branch   = doc["params"][4];
    mJob.version         = String((const char*)doc["params"][5]);
    mJob.nbits           = String((const char*)doc["params"][6]);
    mJob.ntime           = String((const char*)doc["params"][7]);
    mJob.clean_jobs      = doc["params"][8];
    return true;
}

bool tx_mining_submit(WiFiClient& client, mining_subscribe mWorker, mining_job mJob,
                      unsigned long nonce, unsigned long& submit_id) {
    char payload[BUFFER] = {0};
    s_id = getNextId(s_id);
    submit_id = s_id;
    snprintf(payload, sizeof(payload),
        "{\"id\":%lu,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}\n",
        s_id, mWorker.wName, mJob.job_id.c_str(),
        mWorker.extranonce2.c_str(), mJob.ntime.c_str(), String(nonce, HEX).c_str());
    client.print(payload);
    return true;
}

bool parse_mining_set_difficulty(String line, double& difficulty) {
    if (!verifyPayload(&line)) return false;
    DeserializationError err = deserializeJson(doc, line);
    if (err || !doc.containsKey("params")) return false;
    difficulty = (double)doc["params"][0];
    return true;
}

bool tx_suggest_difficulty(WiFiClient& client, double difficulty) {
    char payload[BUFFER] = {0};
    s_id = getNextId(s_id);
    snprintf(payload, sizeof(payload),
        "{\"id\":%lu,\"method\":\"mining.suggest_difficulty\",\"params\":[%.10g]}\n",
        s_id, difficulty);
    return client.print(payload);
}

unsigned long parse_extract_id(const String& line) {
    StaticJsonDocument<256> d;
    if (deserializeJson(d, line) || !d.containsKey("id")) return 0;
    return (unsigned long)d["id"];
}

// ── Utility functions (from utils.cpp) ─────────────────────
uint8_t hex_char(char ch) {
    uint8_t r = (ch > 57) ? (ch - 55) : (ch - 48);
    return r & 0x0F;
}

int to_byte_array(const char* in, size_t in_size, uint8_t* out) {
    int count = 0;
    while (*in && out) {
        *out++ = (hex_char(*in++) << 4) | hex_char(*in++);
        count++;
    }
    return count;
}

static const double truediffone =
    26959535291011309493156476344723991336010898738574164086137773096960.0;

double diff_from_target(void* target) {
    const uint64_t* d64;
    double dcut = 0;
    d64 = (const uint64_t*)((const uint8_t*)target + 24);
    dcut += *d64 * 6277101735386680763835789423207666416102355444464034512896.0;
    d64 = (const uint64_t*)((const uint8_t*)target + 16);
    dcut += *d64 * 340282366920938463463374607431768211456.0;
    d64 = (const uint64_t*)((const uint8_t*)target + 8);
    dcut += *d64 * 18446744073709551616.0;
    d64 = (const uint64_t*)target;
    dcut += *d64;
    if (!dcut) dcut = 1;
    return truediffone / dcut;
}

bool isSha256Valid(const void* sha256) {
    for (int i = 0; i < 8; ++i)
        if (((const uint32_t*)sha256)[i] != 0) return true;
    return false;
}

bool checkValid(unsigned char* hash, unsigned char* target) {
    for (int i = 31; i >= 0; --i) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true;
}

miner_data init_miner_data(void) {
    miner_data m;
    memset(&m, 0, sizeof(m));
    return m;
}

static void swap4bytes(uint8_t* data, size_t offset, size_t size) {
    for (size_t j = offset; j < offset + size / 2; j++) {
        uint8_t t = data[j];
        data[j] = data[2 * offset + size - 1 - j];
        data[2 * offset + size - 1 - j] = t;
    }
}

static void swap4byteWords(uint8_t* data, size_t offset, size_t total, size_t wordSize) {
    for (size_t i = 0; i < total / wordSize; i++) {
        swap4bytes(data, offset, wordSize);
        offset += wordSize;
    }
}

miner_data calculateMiningData(mining_subscribe& mWorker, mining_job mJob) {
    miner_data mMiner = init_miner_data();

    // Build target from nbits
    char target[TARGET_BUFFER_SIZE + 1];
    memset(target, '0', TARGET_BUFFER_SIZE);
    int zeros = (int)strtol(mJob.nbits.substring(0, 2).c_str(), 0, 16) - 3;
    memcpy(target + zeros - 2, mJob.nbits.substring(2).c_str(), mJob.nbits.length() - 2);
    target[TARGET_BUFFER_SIZE] = 0;
    to_byte_array(target, 32, mMiner.bytearray_target);
    // Reverse target
    for (int i = 0; i < 16; i++) {
        uint8_t t = mMiner.bytearray_target[i];
        mMiner.bytearray_target[i] = mMiner.bytearray_target[31 - i];
        mMiner.bytearray_target[31 - i] = t;
    }

    // extranonce2
    if      (mWorker.extranonce2_size == 2) mWorker.extranonce2 = "0001";
    else if (mWorker.extranonce2_size == 4) mWorker.extranonce2 = "00000001";
    else if (mWorker.extranonce2_size == 8) mWorker.extranonce2 = "0000000000000001";
    else                                    mWorker.extranonce2 = "00000001";

    // Coinbase double SHA256
    String coinbase = mJob.coinb1 + mWorker.extranonce1 + mWorker.extranonce2 + mJob.coinb2;
    size_t cbLen = coinbase.length() / 2;
    uint8_t* cbBytes = (uint8_t*)malloc(cbLen);
    if (!cbBytes) return mMiner;
    to_byte_array(coinbase.c_str(), cbLen * 2, cbBytes);

    mbedtls_sha256_context ctx;
    uint8_t inter[32];
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, cbBytes, cbLen);
    mbedtls_sha256_finish(&ctx, inter);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, inter, 32);
    mbedtls_sha256_finish(&ctx, mMiner.merkle_result);
    mbedtls_sha256_free(&ctx);
    free(cbBytes);

    // Merkle root
    uint8_t merkle_cat[64];
    for (size_t k = 0; k < mJob.merkle_branch.size(); k++) {
        const char* elem = (const char*)mJob.merkle_branch[k];
        uint8_t branch[32];
        to_byte_array(elem, 64, branch);
        memcpy(merkle_cat,      mMiner.merkle_result, 32);
        memcpy(merkle_cat + 32, branch, 32);
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, merkle_cat, 64);
        mbedtls_sha256_finish(&ctx, inter);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, inter, 32);
        mbedtls_sha256_finish(&ctx, mMiner.merkle_result);
        mbedtls_sha256_free(&ctx);
    }

    // Merkle root hex string
    char merkle_root[65];
    for (int i = 0; i < 32; i++)
        snprintf(&merkle_root[i * 2], 3, "%02x", mMiner.merkle_result[i]);
    merkle_root[64] = 0;

    // Assemble 80-byte block header
    String blockheader = mJob.version + mJob.prev_block_hash +
                         String(merkle_root) + mJob.ntime + mJob.nbits + "00000000";
    size_t bhLen = blockheader.length() / 2;
    to_byte_array(blockheader.c_str(), bhLen * 2, mMiner.bytearray_blockheader);

    // Byte-swap fields
    swap4bytes(mMiner.bytearray_blockheader, 0, 4);        // version
    swap4byteWords(mMiner.bytearray_blockheader, 4, 32, 4); // prev_hash (4-byte word swap)
    swap4bytes(mMiner.bytearray_blockheader, 68, 4);       // ntime
    swap4bytes(mMiner.bytearray_blockheader, 72, 4);       // nbits

    return mMiner;
}
