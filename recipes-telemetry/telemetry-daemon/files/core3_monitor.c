// ============================================================
// core3_monitor.c — Thread 1 (the real one, see phase-7.md)
// ============================================================

#include "core3_monitor.h"
#include "logging.h"
#include "status_buf.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
// interruptible_sleep() comes from shared_mem.h (included via core3_monitor.h)

// ============================================================
// Config — the numbers decided on together (see phase-7.md)
// ============================================================

#define POLL_INTERVAL_SEC     60   // check core3_dead ~once a minute
#define DEBOUNCE_COUNT        10   // 10 consecutive dead reads = genuinely dead
                                    // (10 * 60s = 600s = exactly 10 min)
#define MAX_RESTART_ATTEMPTS   2   // give up after 2 failed restart cycles (~20 min)
#define HEALTHY_RESET_MIN     60   // 60 consecutive healthy polls (~1 hour) before
                                    // forgiving past attempts and clearing given_up

// Decision (2026-10-04): no device-reboot escalation. restart_attempts is a
// plain in-memory counter — it resets to 0 every time this process restarts,
// which a reboot itself causes. If Core 3's problem were something a reboot
// genuinely can't fix, rebooting on a loop would be strictly worse than the
// problem it was meant to solve (the board would reboot forever, every
// ~30 min, with no memory of having already tried that). So instead: after
// MAX_RESTART_ATTEMPTS failed process-restarts, just STOP — log it once,
// take no further automatic action, wait for manual intervention. No
// reboot, no infinite retry either. Alerts/indicators for this "given up"
// state are planned for later, not built yet — for now a log line is
// enough.

static void restart_core3(void)
{
    log_msg("[core3_monitor] restarting core3_loop (self-healing)");
    if (system("killall core3_loop 2>/dev/null") != 0)
        log_msg("[core3_monitor] note: killall found nothing to kill "
                "(core3_loop was likely already dead — expected)");
    sleep(1);
    if (system("cd /root/bmp280 && taskset -c 3 ./core3_loop "
               ">/tmp/core3_loop.log 2>&1 &") != 0)
        log_msg("[core3_monitor] WARNING: relaunch command itself failed to "
                "run — core3_loop may still be down, will retry next poll");
}

void *core3_monitor_thread(void *arg)
{
    thread_ctx *ctx = (thread_ctx *)arg;
    bootstrap_cfg *boot = ctx->boot;
    ring_entry    *ring = ctx->ring;

    log_msg("[core3_monitor] thread started");

    if (!boot) {
        log_msg("[core3_monitor] no sensor — idling until /dev/bmp280 ready");
        while (*ctx->running && !ctx->boot)
            interruptible_sleep(POLL_INTERVAL_SEC, ctx->running);
        if (!*ctx->running)
            return NULL;
        boot = ctx->boot;
        ring = ctx->ring;
        log_msg("[core3_monitor] sensor ready, starting monitoring");
    }

    int consecutive_dead = 0;
    int consecutive_healthy = 0;
    int restart_attempts = 0;
    int given_up = 0;   // true once MAX_RESTART_ATTEMPTS is exhausted —
                        // stops all further automatic action until a long
                        // healthy stretch (or a manual fix) clears it

    while (*ctx->running) {
        uint64_t dead = boot->core3_dead;
        uint64_t wi   = boot->write_idx;
        ring_entry latest = ring[wi == 0 ? RING_ENTRY_COUNT - 1 : wi - 1];

        if (dead) {
            consecutive_dead++;
            consecutive_healthy = 0;
        } else {
            consecutive_dead = 0;
            consecutive_healthy++;

            if ((restart_attempts > 0 || given_up) &&
                consecutive_healthy >= HEALTHY_RESET_MIN) {
                log_msg("[core3_monitor] sustained healthy for %d polls — "
                        "resetting restart attempt counter (was %d)%s",
                        consecutive_healthy, restart_attempts,
                        given_up ? " and clearing given-up state" : "");
                restart_attempts = 0;
                given_up = 0;
                consecutive_healthy = 0;
                *ctx->health = DAEMON_HEALTH_OK;
            }
        }

        // logged after the counters above are updated, so this line
        // reflects the state resulting from *this* poll
        log_msg("[core3_monitor] poll: core3_dead=%llu  write_idx=%llu  "
                "last_sample: press=%u Pa temp=%d (%.2fC)  "
                "consecutive_dead=%d  restart_attempts=%d  given_up=%d",
                (unsigned long long)dead, (unsigned long long)wi,
                latest.pressure, latest.temp, latest.temp / 100.0,
                consecutive_dead, restart_attempts, given_up);

        g_status.core3.core3_dead        = (int)dead;
        g_status.core3.consecutive_dead  = consecutive_dead;
        g_status.core3.restart_attempts  = restart_attempts;
        g_status.core3.given_up          = given_up;
        g_status.core3.health            = *ctx->health;

        char logbuf[STATUS_MSG_LEN];
        snprintf(logbuf, sizeof(logbuf), "dead=%llu consec=%d press=%uPa temp=%.1fC",
                 (unsigned long long)dead, consecutive_dead,
                 latest.pressure, latest.temp / 100.0);
        status_log(g_status.core3_log, &g_status.core3_log_idx, logbuf);

        // given_up gates this whole block — once set, nothing below runs
        // again until the healthy-reset above clears it. consecutive_dead
        // is left to keep climbing past DEBOUNCE_COUNT harmlessly in that
        // state; it's informative in the log ("how long has this been
        // broken") and costs nothing since no action depends on it anymore.
        if (consecutive_dead >= DEBOUNCE_COUNT && !given_up) {
            log_msg("[core3_monitor] core3_dead confirmed (%d consecutive "
                     "checks, ~%d min) — not noise, taking action",
                     consecutive_dead, (DEBOUNCE_COUNT * POLL_INTERVAL_SEC) / 60);

            restart_attempts++;
            if (restart_attempts > MAX_RESTART_ATTEMPTS) {
                given_up = 1;
                *ctx->health = DAEMON_HEALTH_GIVEN_UP;
                log_msg("[core3_monitor] CRITICAL: core3_loop still dead "
                        "after %d restart attempts — giving up, no further "
                        "automatic action will be taken. Manual "
                        "intervention needed. (now published on "
                        "telemetry/alerts via mqtt_thread, Phase 10)",
                        MAX_RESTART_ATTEMPTS);
            } else {
                *ctx->health = DAEMON_HEALTH_DEGRADED;
                restart_core3();
            }
            consecutive_dead = 0;   // fresh debounce window after acting
        }

        interruptible_sleep(POLL_INTERVAL_SEC, ctx->running);
    }

    log_msg("[core3_monitor] thread stopping (shutdown requested)");
    return NULL;
}
