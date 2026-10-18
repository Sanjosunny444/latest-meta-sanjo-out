// ============================================================
// system_health.c — RPi board-level health, for Phase 10's telemetry/system
// topic. See system_health.h for the design reasoning.
// ============================================================

#include "system_health.h"
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

// ============================================================
// Individual field readers — each one fails independently, sentinel value
// (-1) on failure so one missing source doesn't block the others
// ============================================================

static void read_meminfo(long *total_kb, long *avail_kb)
{
    *total_kb = -1;
    *avail_kb = -1;

    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %ld kB", total_kb) == 1) continue;
        if (sscanf(line, "MemAvailable: %ld kB", avail_kb) == 1) continue;
    }
    fclose(f);
}

static void read_storage(long *total_mb, long *avail_mb)
{
    *total_mb = -1;
    *avail_mb = -1;

    struct statvfs vfs;
    if (statvfs("/", &vfs) != 0) return;

    // f_frsize is the fragment size in bytes — the correct multiplier for
    // block counts (f_bsize can differ from f_frsize on some filesystems)
    *total_mb = (long)((unsigned long long)vfs.f_blocks * vfs.f_frsize / (1024 * 1024));
    *avail_mb = (long)((unsigned long long)vfs.f_bavail * vfs.f_frsize / (1024 * 1024));
}

static double read_cpu_temp(void)
{
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1.0;

    int millideg;
    int got = fscanf(f, "%d", &millideg);
    fclose(f);
    if (got != 1) return -1.0;

    return millideg / 1000.0;
}

// /proc/net/wireless format (fixed columns, wlan0 is this board's only
// wireless interface):
//   wlan0: 0000   46.  -64.  -256        0      0      0      9      0        0
// link quality, signal level (dBm) — noise level is unused/placeholder
// (-256) on this driver, not reported.
static void read_wifi(int *quality, int *signal_dbm)
{
    *quality    = -1;
    *signal_dbm = 1;   // sentinel for dBm is +1, not -1 — real dBm values
                        // are always negative, so -1 could be a real
                        // (if unusually strong) reading

    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "wlan0:")) {
            char iface[16];
            unsigned status;
            double qual, level;
            // NOTE: no literal '.' after each %lf — found the hard way that
            // %lf already greedily consumes a trailing decimal point as
            // part of valid float syntax ("44." parses whole as 44.0), so
            // an explicit '.' here finds nothing left to match and halts
            // the entire scan right after the first field. Confirmed via a
            // standalone test against the real /proc/net/wireless line.
            if (sscanf(line, "%15s %x %lf %lf", iface, &status, &qual, &level) == 4) {
                *quality    = (int)qual;
                *signal_dbm = (int)level;
            }
            break;
        }
    }
    fclose(f);
}

static double read_uptime(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return -1.0;

    double up;
    int got = fscanf(f, "%lf", &up);
    fclose(f);
    return got == 1 ? up : -1.0;
}

static double read_loadavg(void)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return -1.0;

    double load1;
    int got = fscanf(f, "%lf", &load1);
    fclose(f);
    return got == 1 ? load1 : -1.0;
}

// ============================================================
// Public entry point
// ============================================================

void system_health_json(char *out, int out_size)
{
    long ram_total_kb, ram_avail_kb;
    read_meminfo(&ram_total_kb, &ram_avail_kb);

    long storage_total_mb, storage_avail_mb;
    read_storage(&storage_total_mb, &storage_avail_mb);

    double cpu_temp_c = read_cpu_temp();

    int wifi_quality, wifi_signal_dbm;
    read_wifi(&wifi_quality, &wifi_signal_dbm);

    double uptime_sec  = read_uptime();
    double load_avg_1m = read_loadavg();

    snprintf(out, out_size,
        "{\"ram_avail_kb\":%ld,\"ram_total_kb\":%ld,"
        "\"storage_avail_mb\":%ld,\"storage_total_mb\":%ld,"
        "\"cpu_temp_c\":%.1f,"
        "\"wifi_signal_dbm\":%d,\"wifi_quality\":%d,"
        "\"uptime_sec\":%.0f,\"load_avg_1min\":%.2f}",
        ram_avail_kb, ram_total_kb,
        storage_avail_mb, storage_total_mb,
        cpu_temp_c,
        wifi_signal_dbm, wifi_quality,
        uptime_sec, load_avg_1m);
}
