// ============================================================
// status_writer.c — Phase 12: RAM status buffer + JSON flush
// ============================================================
// Flushes g_status to /var/run/telemetry_status.json every 20s.
// /var/run is tmpfs — zero SD card wear.
// Also gathers system info (IP, MAC, CPU temp, RAM, etc.)
// ============================================================

#include "status_writer.h"
#include "status_buf.h"
#include "logging.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/statvfs.h>

// Global instance — every thread writes to this
status_buf g_status = {0};

// Helper — each thread calls this to append to its own log ring
void status_log(log_entry *ring, int *idx, const char *msg)
{
    pthread_mutex_lock(&g_status.lock);
    ring[*idx].ts = time(NULL);
    strncpy(ring[*idx].msg, msg, STATUS_MSG_LEN - 1);
    ring[*idx].msg[STATUS_MSG_LEN - 1] = '\0';
    *idx = (*idx + 1) % STATUS_LOG_DEPTH;
    pthread_mutex_unlock(&g_status.lock);
}

// Read system info from /proc, /sys, ip command etc.
static void gather_system_info(void)
{
    // Hostname
    FILE *f = fopen("/etc/hostname", "r");
    if (f) {
        if (fgets(g_status.hostname, sizeof(g_status.hostname), f))
            g_status.hostname[strcspn(g_status.hostname, "\n")] = '\0';
        fclose(f);
    }

    // Kernel version
    f = fopen("/proc/version", "r");
    if (f) {
        char buf[256];
        if (fgets(buf, sizeof(buf), f)) {
            // Extract just "Linux version X.Y.Z"
            char *space = strchr(buf, ' ');
            if (space) space = strchr(space + 1, ' ');
            if (space) {
                char *end = strchr(space + 1, ' ');
                if (end) *end = '\0';
                strncpy(g_status.kernel, space + 1, sizeof(g_status.kernel) - 1);
            }
        }
        fclose(f);
    }

    // CPU temp
    f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (f) {
        int millideg = 0;
        if (fscanf(f, "%d", &millideg) == 1)
            g_status.cpu_temp_c = millideg / 1000.0f;
        fclose(f);
    }

    // RAM from /proc/meminfo
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            int val;
            if (sscanf(line, "MemTotal: %d kB", &val) == 1)
                g_status.ram_total_kb = val;
            else if (sscanf(line, "MemAvailable: %d kB", &val) == 1)
                g_status.ram_avail_kb = val;
        }
        fclose(f);
    }

    // Storage
    struct statvfs sv;
    if (statvfs("/", &sv) == 0) {
        g_status.storage_avail_mb = (int)((sv.f_bavail * sv.f_frsize) / (1024 * 1024));
    }

    // Uptime
    f = fopen("/proc/uptime", "r");
    if (f) {
        double up = 0;
        if (fscanf(f, "%lf", &up) == 1)
            g_status.uptime_sec = (int)up;
        fclose(f);
    }

    // Load average
    f = fopen("/proc/loadavg", "r");
    if (f) {
        float la = 0;
        if (fscanf(f, "%f", &la) == 1)
            g_status.load_avg = la;
        fclose(f);
    }

    // WiFi signal from /proc/net/wireless
    f = fopen("/proc/net/wireless", "r");
    if (f) {
        char line[256];
        int linenum = 0;
        while (fgets(line, sizeof(line), f)) {
            linenum++;
            if (linenum <= 2) continue;   // skip header lines
            char iface[16];
            int status;
            double quality, signal;
            if (sscanf(line, "%15s %x %lf %lf", iface, &status,
                       &quality, &signal) >= 4)
                g_status.wifi_signal_dbm = (int)signal;
        }
        fclose(f);
    }

    // IP address — parse from /proc/net/if_inet6 or run ip command
    // Simple approach: read from a pipe
    f = popen("ip -4 addr show wlan0 2>/dev/null | grep 'inet ' | awk '{print $2}' | cut -d/ -f1", "r");
    if (f) {
        if (fgets(g_status.ip, sizeof(g_status.ip), f))
            g_status.ip[strcspn(g_status.ip, "\n")] = '\0';
        pclose(f);
    }

    // MAC address
    f = fopen("/sys/class/net/wlan0/address", "r");
    if (f) {
        if (fgets(g_status.mac, sizeof(g_status.mac), f))
            g_status.mac[strcspn(g_status.mac, "\n")] = '\0';
        fclose(f);
    }
}

// Write one log ring as a JSON array of {"ts":"HH:MM:SS","msg":"..."}
static void write_log_ring(FILE *f, const char *name, log_entry *ring,
                            int idx, int first)
{
    if (!first) fprintf(f, ",");
    fprintf(f, "\"%s\":[", name);
    int written = 0;
    for (int i = 0; i < STATUS_LOG_DEPTH; i++) {
        // Read from oldest to newest: start at idx (oldest), wrap around
        int j = (idx + i) % STATUS_LOG_DEPTH;
        if (ring[j].ts == 0) continue;   // empty slot
        if (written > 0) fprintf(f, ",");
        struct tm *tm = localtime(&ring[j].ts);
        char ts_str[16];
        strftime(ts_str, sizeof(ts_str), "%H:%M:%S", tm);
        // Escape quotes in msg
        fprintf(f, "{\"ts\":\"%s\",\"msg\":\"", ts_str);
        for (const char *c = ring[j].msg; *c; c++) {
            if (*c == '"') fprintf(f, "\\\"");
            else if (*c == '\\') fprintf(f, "\\\\");
            else if (*c == '\n') fprintf(f, "\\n");
            else fputc(*c, f);
        }
        fprintf(f, "\"}");
        written++;
    }
    fprintf(f, "]");
}

// Flush the entire status_buf to JSON
static void flush_json(void)
{
    pthread_mutex_lock(&g_status.lock);

    FILE *f = fopen(STATUS_JSON_PATH ".tmp", "w");
    if (!f) {
        pthread_mutex_unlock(&g_status.lock);
        return;
    }

    fprintf(f, "{");

    // System
    fprintf(f, "\"system\":{\"ip\":\"%s\",\"mac\":\"%s\",\"hostname\":\"%s\","
               "\"kernel\":\"%s\",\"cpu_temp_c\":%.1f,"
               "\"ram_avail_kb\":%d,\"ram_total_kb\":%d,"
               "\"storage_avail_mb\":%d,\"uptime_sec\":%d,"
               "\"load_avg\":%.2f,\"wifi_signal_dbm\":%d}",
               g_status.ip, g_status.mac, g_status.hostname,
               g_status.kernel, g_status.cpu_temp_c,
               g_status.ram_avail_kb, g_status.ram_total_kb,
               g_status.storage_avail_mb, g_status.uptime_sec,
               g_status.load_avg, g_status.wifi_signal_dbm);

    // MQTT
    fprintf(f, ",\"mqtt\":{\"connected\":%d,\"last_pub\":%ld,"
               "\"last_topic\":\"%s\",\"pub_count\":%d}",
               g_status.mqtt.connected, (long)g_status.mqtt.last_pub,
               g_status.mqtt.last_topic, g_status.mqtt.pub_count);

    // Modbus
    fprintf(f, ",\"modbus\":{\"listening\":%d,\"client_connected\":%d,"
               "\"last_poll\":%ld,\"regs\":[%u,%u,%u,%u,%u,%u]}",
               g_status.modbus.listening, g_status.modbus.client_connected,
               (long)g_status.modbus.last_poll,
               g_status.modbus.regs[0], g_status.modbus.regs[1],
               g_status.modbus.regs[2], g_status.modbus.regs[3],
               g_status.modbus.regs[4], g_status.modbus.regs[5]);

    // Core3
    fprintf(f, ",\"core3\":{\"core3_dead\":%d,\"consecutive_dead\":%d,"
               "\"restart_attempts\":%d,\"given_up\":%d,\"health\":%d}",
               g_status.core3.core3_dead, g_status.core3.consecutive_dead,
               g_status.core3.restart_attempts, g_status.core3.given_up,
               g_status.core3.health);

    // FDD
    fprintf(f, ",\"fdd\":{\"sample_n\":%llu,\"in_warmup\":%d,"
               "\"fault_count_total\":%d,\"last_fault_algo\":\"%s\","
               "\"last_fault_label\":\"%s\",\"last_fault_ts\":%ld}",
               (unsigned long long)g_status.fdd.sample_n,
               g_status.fdd.in_warmup,
               g_status.fdd.fault_count_total,
               g_status.fdd.last_fault_algo,
               g_status.fdd.last_fault_label,
               (long)g_status.fdd.last_fault_ts);

    // Log rings
    fprintf(f, ",\"logs\":{");
    write_log_ring(f, "mqtt",   g_status.mqtt_log,   g_status.mqtt_log_idx,   1);
    write_log_ring(f, "modbus", g_status.modbus_log, g_status.modbus_log_idx, 0);
    write_log_ring(f, "core3",  g_status.core3_log,  g_status.core3_log_idx,  0);
    write_log_ring(f, "fdd",    g_status.fdd_log,    g_status.fdd_log_idx,    0);
    fprintf(f, "}");

    fprintf(f, "}");
    fclose(f);

    // Atomic rename — readers always see a complete file
    rename(STATUS_JSON_PATH ".tmp", STATUS_JSON_PATH);

    pthread_mutex_unlock(&g_status.lock);
}

void *status_writer_thread(void *arg)
{
    thread_ctx *ctx = (thread_ctx *)arg;

    pthread_mutex_init(&g_status.lock, NULL);

    log_msg("[status_writer] thread started, writing to %s every %ds",
            STATUS_JSON_PATH, STATUS_WRITE_INTERVAL_SEC);

    while (*ctx->running) {
        gather_system_info();
        flush_json();
        interruptible_sleep(STATUS_WRITE_INTERVAL_SEC, ctx->running);
    }

    // Final flush on shutdown
    gather_system_info();
    flush_json();

    pthread_mutex_destroy(&g_status.lock);
    log_msg("[status_writer] thread stopping");
    return NULL;
}
