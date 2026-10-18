#ifndef STATUS_BUF_H
#define STATUS_BUF_H

// ============================================================
// status_buf.h — shared in-RAM status structure (Phase 12)
// ============================================================
// Every thread writes its own section here. A dedicated
// status_writer thread flushes this to /var/run/telemetry_status.json
// every 20 seconds. /var/run is tmpfs — zero SD card writes.
//
// Web UI reads the JSON file every 10 seconds.
// ============================================================

#include <time.h>
#include <stdint.h>
#include <pthread.h>

#define STATUS_LOG_DEPTH  20    // log entries per thread ring
#define STATUS_JSON_PATH  "/var/run/telemetry_status.json"
#define STATUS_WRITE_INTERVAL_SEC  20
#define STATUS_MSG_LEN    120

// One timestamped log entry — web UI shows [HH:MM:SS] msg
typedef struct {
    time_t ts;
    char   msg[STATUS_MSG_LEN];
} log_entry;

typedef struct {

    // --- System info (written by status_writer thread) ---
    char  ip[32];
    char  mac[32];
    char  hostname[64];
    char  kernel[64];
    float cpu_temp_c;
    int   ram_avail_kb;
    int   ram_total_kb;
    int   storage_avail_mb;
    int   uptime_sec;
    float load_avg;
    int   wifi_signal_dbm;

    // --- MQTT thread ---
    struct {
        int    connected;
        time_t last_pub;
        char   last_topic[64];
        char   last_payload[256];
        int    pub_count;
    } mqtt;

    // --- Modbus thread ---
    struct {
        int      listening;
        int      client_connected;
        time_t   last_poll;
        uint16_t regs[6];
    } modbus;

    // --- Core3 monitor thread ---
    struct {
        int core3_dead;
        int consecutive_dead;
        int restart_attempts;
        int given_up;
        int health;   // 0=ok 1=degraded 2=given_up
    } core3;

    // --- FDD thread ---
    struct {
        uint64_t sample_n;
        int      in_warmup;
        int      fault_count_total;
        char     last_fault_algo[32];
        char     last_fault_label[64];
        time_t   last_fault_ts;
    } fdd;

    // --- Per-thread log rings (FIFO, each thread owns its own) ---
    log_entry mqtt_log  [STATUS_LOG_DEPTH];
    log_entry modbus_log[STATUS_LOG_DEPTH];
    log_entry core3_log [STATUS_LOG_DEPTH];
    log_entry fdd_log   [STATUS_LOG_DEPTH];

    // Circular write indices
    int mqtt_log_idx;
    int modbus_log_idx;
    int core3_log_idx;
    int fdd_log_idx;

    // Mutex — held only during the 20s JSON flush (very brief)
    pthread_mutex_t lock;

} status_buf;

// Global instance — defined in status_writer.c, extern everywhere else
extern status_buf g_status;

// Helper — each thread calls this to append to its own log ring
void status_log(log_entry *ring, int *idx, const char *msg);

#endif // STATUS_BUF_H
