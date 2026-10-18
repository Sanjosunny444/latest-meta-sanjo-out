// ============================================================
// telemetry_daemon.c — Phase 7 skeleton, main/orchestration
// ============================================================
// Actual multithreaded skeleton now (see phase-7.md): one real thread
// (core3_monitor) plus four stub threads (can, modbus, mqtt, fdd) that each
// start, run, and shut down cleanly — just doing nothing meaningful yet.
// Each later phase (8-11) fills in its own file without touching this one
// or any other thread's file.
//
// main()'s job: daemonize, open the one shared mmap of /dev/bmp280, launch
// every thread, wait for a shutdown signal, tell every thread to stop, join
// them all, clean up. Phase 6's watchdog kick (deferred) and epoll-based CAN
// handling from the original plan both still have a natural home here later
// if/when Phase 6 gets built — not added yet since it's deferred.
// ============================================================

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "shared_mem.h"
#include "logging.h"
#include "core3_monitor.h"
#include "can_thread.h"
#include "modbus_thread.h"
#include "mqtt_thread.h"
#include "fdd_thread.h"
#include "status_writer.h"

#define LOG_PATH   "/root/bmp280/telemetry_daemon.log"
#define PIDFILE    "/var/run/telemetry_daemon.pid"
// TODO(Phase 13): move LOG_PATH to /data/logs/ once that partition exists —
// this rootfs path works today but breaks once Phase 12's read-only rootfs
// lands. PIDFILE is fine as-is: /var/run is tmpfs, correctly wiped every
// boot (confirmed live on-device), which is the right behavior for a pid
// file — a pid from a previous boot is meaningless anyway.

static volatile sig_atomic_t g_running = 1;
static volatile int          g_health  = DAEMON_HEALTH_OK;   // see shared_mem.h

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);

    if (setsid() < 0) { perror("setsid"); exit(1); }

    pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);

    umask(0);
    if (chdir("/") < 0) { perror("chdir"); exit(1); }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);

    FILE *pf = fopen(PIDFILE, "w");
    if (pf) {
        fprintf(pf, "%d\n", getpid());
        fclose(pf);
    }
}

int main(void)
{
    daemonize();

    g_log = fopen(LOG_PATH, "a");

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    log_msg("telemetry_daemon started (pid=%d)", getpid());

    // Try to open /dev/bmp280, but DON'T BLOCK startup if it's not ready yet.
    // MQTT will start immediately and publish system health regardless. Sensor
    // data waits independently in mqtt_thread (publishes zeros/sentinel until
    // real data is available). This way MQTT is resilient to slow driver
    // initialization — the daemon serves useful data (board temp, RAM, WiFi)
    // immediately, and adds pressure/temp readings once the sensor is ready.
    int fd = open("/dev/bmp280", O_RDONLY);
    int sensor_ready = (fd >= 0);

    if (!sensor_ready) {
        log_msg("/dev/bmp280 not ready at startup — sensor readings will be "
                "unavailable until the driver loads. Starting daemon anyway; "
                "MQTT will publish system health immediately.");
    }

    void *buf = NULL;
    bootstrap_cfg *boot = NULL;
    ring_entry *ring = NULL;

    if (sensor_ready) {
        buf = mmap(NULL, BUF_SIZE, PROT_READ, MAP_SHARED, fd, 0);
        if (buf == MAP_FAILED) {
            log_msg("WARNING: mmap /dev/bmp280 failed: %s — continuing without "
                    "sensor data", strerror(errno));
            close(fd);
            fd = -1;
            buf = NULL;
            sensor_ready = 0;
        } else {
            boot = (bootstrap_cfg *)buf;
            ring = (ring_entry *)((uint8_t *)buf + BOOTSTRAP_SIZE);

            if (boot->magic != BOOTSTRAP_MAGIC) {
                log_msg("WARNING: bad magic 0x%016llX in /dev/bmp280 — "
                        "is bmp280_driver.ko loaded correctly? Continuing "
                        "without sensor data for now.",
                        (unsigned long long)boot->magic);
                munmap(buf, BUF_SIZE);
                close(fd);
                fd = -1;
                buf = NULL;
                boot = NULL;
                ring = NULL;
                sensor_ready = 0;
            }
        }
    }

    // One shared context — every thread gets a pointer to the SAME data.
    // Note: boot and ring are NULL if sensor isn't ready, and threads handle
    // this gracefully (mqtt publishes zeros, core3_monitor doesn't run, etc.)
    thread_ctx ctx = {
        .boot    = boot,
        .ring    = ring,
        .running = &g_running,
        .health  = &g_health,
    };

    // ------------------------------------------------------------
    // Launch every thread — the real one + all four stubs
    // ------------------------------------------------------------
    pthread_t th_core3, th_can, th_modbus, th_mqtt, th_fdd, th_status;

    pthread_create(&th_status, NULL, status_writer_thread,  &ctx);
    pthread_create(&th_core3,  NULL, core3_monitor_thread,  &ctx);
    pthread_create(&th_can,    NULL, can_thread,             &ctx);
    pthread_create(&th_modbus, NULL, modbus_thread,          &ctx);
    pthread_create(&th_mqtt,   NULL, mqtt_thread,            &ctx);
    pthread_create(&th_fdd,    NULL, fdd_thread,             &ctx);

    log_msg("all threads launched (core3_monitor + 4 stubs + status_writer)");

    // main thread just waits for a shutdown signal — handle_signal() flips
    // g_running, every thread's own loop notices *ctx.running == 0 and
    // returns on its own next iteration.
    while (g_running) {
        sleep(1);
    }

    log_msg("shutdown requested — waiting for all threads to stop");

    pthread_join(th_core3,  NULL);
    pthread_join(th_can,    NULL);
    pthread_join(th_modbus, NULL);
    pthread_join(th_mqtt,   NULL);
    pthread_join(th_fdd,    NULL);
    pthread_join(th_status, NULL);

    log_msg("telemetry_daemon shutdown complete");

    if (buf) munmap(buf, BUF_SIZE);
    if (fd >= 0) close(fd);
    if (g_log) fclose(g_log);
    unlink(PIDFILE);
    return 0;
}
