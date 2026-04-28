// Simplified NerdMiner_v2 mining core — hardware SHA256 (ESP32-S3)
// Adapted from https://github.com/BitMaker-hub/NerdMiner_v2
#include "nerd_mining.h"
#include "nerd_stratum.h"
#include <WiFi.h>
#include <mutex>
#include <list>
#include <map>
#include <memory>
#include <esp_task_wdt.h>
#include <sha/sha_dma.h>
#include <hal/sha_hal.h>
#include <hal/sha_ll.h>

// ── Global stats ─────────────────────────────────────────
volatile uint32_t mineHashes   = 0;
volatile uint32_t mineMhashes  = 0;
volatile uint32_t mineShares   = 0;
volatile uint32_t mineValids   = 0;
volatile uint64_t mineUptime   = 0;
volatile double   mineBestDiff = 0.0;
volatile uint32_t mineKHs      = 0;
volatile bool     mineActive   = false;

char cfgBtcAddr[128] = "";
char cfgPool[128]    = "";
char cfgPoolPort[8]  = "21496";

// ── Job queue (Core 0 produce, miner consume) ────────────
struct JobRequest {
    uint32_t id;
    uint32_t nonce_start;
    uint32_t nonce_count;
    double   difficulty;
    uint8_t  sha_buffer[128];
    uint32_t midstate[8];  // HW midstate (sha_hal output)
};

struct JobResult {
    uint32_t id;
    uint32_t nonce;
    uint32_t nonce_count;
    double   difficulty;
    uint8_t  hash[32];
};

static std::mutex                            s_mtx;
static std::list<std::shared_ptr<JobRequest>> s_hw_queue;
static std::list<std::shared_ptr<JobResult>>  s_result_queue;
static volatile uint8_t s_cur_job_id = 0xFF;

#define NONCE_PER_JOB_HW (16 * 1024)

// ── Pool connection ──────────────────────────────────────
static WiFiClient       s_client;
static miner_data       s_mMiner;
static mining_subscribe s_mWorker;
static mining_job       s_mJob;
static volatile bool    s_subscribed  = false;
static unsigned long    s_lastTxPool  = 0;

static IPAddress s_poolIP(1, 1, 1, 1);

static bool checkPoolConn() {
    if (s_client.connected()) return true;
    s_subscribed = false;
    if (s_poolIP == IPAddress(1, 1, 1, 1))
        WiFi.hostByName(cfgPool, s_poolIP);
    int port = atoi(cfgPoolPort);
    if (!s_client.connect(s_poolIP, port)) {
        WiFi.hostByName(cfgPool, s_poolIP);
        return false;
    }
    return true;
}

static bool checkInactivity() {
    uint32_t now = millis();
    if (now - s_lastTxPool > KEEPALIVE_TIME_ms) {
        s_lastTxPool = now;
        tx_suggest_difficulty(s_client, DEFAULT_DIFFICULTY);
    }
    return false;
}

static void stopJobs(uint32_t& job_pool) {
    std::lock_guard<std::mutex> lock(s_mtx);
    s_hw_queue.clear();
    s_result_queue.clear();
    s_cur_job_id = 0xFF;
    job_pool = 0xFFFFFFFF;
}

// ── Hardware SHA256 helpers (ESP32-S3) ───────────────────
static inline void sha_ll_write_digest(void* dig) {
    uint32_t* w = (uint32_t*)dig;
    uint32_t* r = (uint32_t*)(SHA_H_BASE);
    for (int i = 0; i < 8; i++) REG_WRITE(&r[i], w[i]);
}

static inline void sha_fill_block_nonce(const void* buf, uint32_t nonce) {
    const uint32_t* d = (const uint32_t*)buf;
    uint32_t* r = (uint32_t*)(SHA_TEXT_BASE);
    REG_WRITE(&r[0], d[0]);
    REG_WRITE(&r[1], d[1]);
    REG_WRITE(&r[2], d[2]);
    REG_WRITE(&r[3], nonce);
    REG_WRITE(&r[4], 0x00000080);
    for (int i = 5; i <= 14; i++) REG_WRITE(&r[i], 0);
    REG_WRITE(&r[15], 0x80020000);
}

static inline void sha_fill_block_inter() {
    uint32_t* r = (uint32_t*)(SHA_TEXT_BASE);
    uint32_t* h = (uint32_t*)(SHA_H_BASE);
    DPORT_INTERRUPT_DISABLE();
    for (int i = 0; i < 8; i++)
        REG_WRITE(&r[i], DPORT_SEQUENCE_REG_READ(&h[i]));
    DPORT_INTERRUPT_RESTORE();
    REG_WRITE(&r[8],  0x00000080);
    for (int i = 9; i <= 14; i++) REG_WRITE(&r[i], 0);
    REG_WRITE(&r[15], 0x00010000);
}

static inline void sha_wait() {
    while (REG_READ(SHA_BUSY_REG)) {}
}

static inline bool sha_read_if(void* out) {
    DPORT_INTERRUPT_DISABLE();
    uint32_t last = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 7 * 4);
    if ((uint16_t)(last >> 16) != 0) { DPORT_INTERRUPT_RESTORE(); return false; }
    uint32_t* o = (uint32_t*)out;
    o[7] = last;
    for (int i = 0; i < 7; i++)
        o[i] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + i * 4);
    DPORT_INTERRUPT_RESTORE();
    return true;
}

// ── Hardware miner task (Core 0) ─────────────────────────
void minerWorkerHw(void* task_id) {
    Serial.printf("[MINER] HW worker started on core %d\n", xPortGetCoreID());

    std::shared_ptr<JobRequest> job;
    std::shared_ptr<JobResult>  result;
    uint8_t hash[32];
    uint32_t wdt = 0;

    while (true) {
        {
            std::lock_guard<std::mutex> lock(s_mtx);
            if (result) {
                if (s_result_queue.size() < 16)
                    s_result_queue.push_back(result);
                result.reset();
            }
            if (!s_hw_queue.empty()) {
                job = s_hw_queue.front(); s_hw_queue.pop_front();
            } else job.reset();
        }

        if (job) {
            result = std::make_shared<JobResult>();
            result->id          = job->id;
            result->nonce       = 0xFFFFFFFF;
            result->nonce_count = job->nonce_count;
            result->difficulty  = job->difficulty;
            uint8_t job_id = job->id & 0xFF;

            esp_sha_acquire_hardware();
            REG_WRITE(SHA_MODE_REG, SHA2_256);

            for (uint32_t n = job->nonce_start; n < job->nonce_start + job->nonce_count; ++n) {
                sha_ll_write_digest(job->midstate);
                sha_fill_block_nonce(job->sha_buffer + 64, n);
                REG_WRITE(SHA_CONTINUE_REG, 1);
                sha_ll_load(SHA2_256);
                sha_wait();
                sha_fill_block_inter();
                REG_WRITE(SHA_START_REG, 1);
                sha_ll_load(SHA2_256);
                sha_wait();

                if (sha_read_if(hash)) {
                    double diff = diff_from_target(hash);
                    if (diff > result->difficulty && isSha256Valid(hash)) {
                        result->difficulty = diff;
                        result->nonce      = n;
                        memcpy(result->hash, hash, 32);
                    }
                }
                if ((uint8_t)(n & 0xFF) == 0 && s_cur_job_id != job_id) {
                    result->nonce_count = n - job->nonce_start + 1;
                    break;
                }
            }
            esp_sha_release_hardware();
        } else {
            vTaskDelay(2 / portTICK_PERIOD_MS);
        }

        if (++wdt >= 8) { wdt = 0; esp_task_wdt_reset(); }
    }
}

// ── Stratum worker task (Core 0) ─────────────────────────
void runStratumWorker(void* name) {
    Serial.printf("[STRATUM] Worker started on core %d\n", xPortGetCoreID());

    double currentDiff = DEFAULT_DIFFICULTY;
    uint32_t nonce_pool = 0xDA54E700;
    uint32_t job_pool   = 0xFFFFFFFF;
    uint32_t lastJobTime = millis();
    uint32_t lastSecond  = millis();

    std::map<uint32_t, std::pair<double, bool>> submMap; // id → (diff, is32bit)

    // Start HW miner on Core 0 (same core, different task)
    xTaskCreatePinnedToCore(minerWorkerHw, "miner_hw", 4096, (void*)0, 5, nullptr, 0);

    while (true) {
        // Uptime + KHs update
        uint32_t now = millis();
        if (now - lastSecond >= 1000) {
            uint32_t elapsed = now - lastSecond;
            lastSecond = now;
            mineUptime++;
            uint32_t curKH = (mineMhashes * 1000) + mineHashes / 1000;
            static uint32_t prevKH = 0;
            mineKHs = (curKH - prevKH) * 1000 / elapsed;
            prevKH  = curKH;
        }

        if (WiFi.status() != WL_CONNECTED) {
            mineActive = false;
            stopJobs(job_pool);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        if (!checkPoolConn()) {
            mineActive = false;
            stopJobs(job_pool);
            vTaskDelay(((1 + rand() % 30) * 1000) / portTICK_PERIOD_MS);
            continue;
        }

        if (!s_subscribed) {
            s_mWorker = init_mining_subscribe();
            if (!tx_mining_subscribe(s_client, s_mWorker)) {
                s_client.stop();
                stopJobs(job_pool);
                continue;
            }
            strncpy(s_mWorker.wName, cfgBtcAddr, sizeof(s_mWorker.wName) - 1);
            strncpy(s_mWorker.wPass, "x",        sizeof(s_mWorker.wPass) - 1);
            tx_mining_auth(s_client, s_mWorker.wName, s_mWorker.wPass);
            tx_suggest_difficulty(s_client, currentDiff);
            s_subscribed  = true;
            s_lastTxPool  = millis();
            lastJobTime   = millis();
            mineActive    = true;
        }

        checkInactivity();

        // Timeout: 10 min without job
        if (millis() - lastJobTime > 10 * 60 * 1000) {
            s_client.stop(); s_subscribed = false;
            stopJobs(job_pool); continue;
        }

        // Read pool messages
        while (s_client.connected() && s_client.available()) {
            String line = s_client.readStringUntil('\n');
            stratum_method m = parse_mining_method(line);

            if (m == MINING_NOTIFY && parse_mining_notify(line, s_mJob)) {
                {
                    std::lock_guard<std::mutex> lock(s_mtx);
                    s_hw_queue.clear();
                }
                job_pool++;
                s_cur_job_id = job_pool & 0xFF;
                lastJobTime = millis();

                uint32_t mh = mineHashes / 1000000;
                mineMhashes += mh; mineHashes -= mh * 1000000;

                s_mMiner = calculateMiningData(s_mWorker, s_mJob);

                // Prepare padding for SHA256 midstate
                memset(s_mMiner.bytearray_blockheader + 80, 0, 128 - 80);
                s_mMiner.bytearray_blockheader[80]  = 0x80;
                s_mMiner.bytearray_blockheader[126] = 0x02;
                s_mMiner.bytearray_blockheader[127] = 0x80;

                // Compute HW midstate via esp_sha
                uint32_t hw_mid[8];
                esp_sha_acquire_hardware();
                sha_hal_hash_block(SHA2_256, s_mMiner.bytearray_blockheader, 64 / 4, true);
                sha_hal_read_digest(SHA2_256, hw_mid);
                esp_sha_release_hardware();

                nonce_pool = 0xDA54E700;
                {
                    std::lock_guard<std::mutex> lock(s_mtx);
                    for (int i = 0; i < 4; i++) {
                        auto job = std::make_shared<JobRequest>();
                        job->id          = job_pool;
                        job->nonce_start = nonce_pool;
                        job->nonce_count = NONCE_PER_JOB_HW;
                        job->difficulty  = currentDiff;
                        memcpy(job->sha_buffer, s_mMiner.bytearray_blockheader, 128);
                        memcpy(job->midstate,   hw_mid, 32);
                        s_hw_queue.push_back(job);
                        nonce_pool += NONCE_PER_JOB_HW;
                    }
                }
            } else if (m == MINING_SET_DIFFICULTY) {
                parse_mining_set_difficulty(line, currentDiff);
            } else if (m == STRATUM_SUCCESS) {
                unsigned long rid = parse_extract_id(line);
                auto it = submMap.find(rid);
                if (it != submMap.end()) {
                    if (it->second.first > mineBestDiff) mineBestDiff = it->second.first;
                    if (it->second.second) mineShares++;
                    submMap.erase(it);
                }
            }
        }

        // Refill job queue
        if (job_pool != 0xFFFFFFFF) {
            std::lock_guard<std::mutex> lock(s_mtx);

            while (s_hw_queue.size() < 4) {
                auto job = std::make_shared<JobRequest>();
                job->id          = job_pool;
                job->nonce_start = nonce_pool;
                job->nonce_count = NONCE_PER_JOB_HW;
                job->difficulty  = currentDiff;
                memcpy(job->sha_buffer, s_mMiner.bytearray_blockheader, 128);
                uint32_t hw_mid[8];
                // Reuse last midstate (already in s_mMiner context)
                esp_sha_acquire_hardware();
                sha_hal_hash_block(SHA2_256, s_mMiner.bytearray_blockheader, 64/4, true);
                sha_hal_read_digest(SHA2_256, hw_mid);
                esp_sha_release_hardware();
                memcpy(job->midstate, hw_mid, 32);
                s_hw_queue.push_back(job);
                nonce_pool += NONCE_PER_JOB_HW;
            }

            // Process results
            for (auto& res : s_result_queue) {
                mineHashes += res->nonce_count;
                if (res->difficulty > currentDiff && job_pool == res->id && res->nonce != 0xFFFFFFFF) {
                    unsigned long sid = 0;
                    tx_mining_submit(s_client, s_mWorker, s_mJob, res->nonce, sid);
                    bool is32 = (res->hash[29] == 0 && res->hash[28] == 0);
                    bool valid = is32 && checkValid(res->hash, s_mMiner.bytearray_target);
                    if (valid) { mineValids++; Serial.println("BLOCK FOUND!"); }
                    submMap[sid] = { res->difficulty, is32 };
                    if (submMap.size() > 32) submMap.erase(submMap.begin());
                    s_lastTxPool = millis();
                }
            }
            s_result_queue.clear();
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}
