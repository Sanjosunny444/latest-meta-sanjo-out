// ============================================================
// fdd_thread.c — Phase 11: Edge Fault Detection & Diagnostics
// ============================================================
// 8 algorithms running on the RPi itself — no cloud, no ML library.
// All pure C math. Each algorithm catches a different fault type.
//
// Algorithm stack (in order of execution per sample):
//   1. Welford    — running mean + stddev (baseline for everything else)
//   2. EWMA       — smoothed value (fed into Z-score/CUSUM to reduce noise)
//   3. Z-score    — sudden spike detection
//   4. CUSUM      — slow drift detection
//   5. dT/dt      — rate of change (fastest alarm)
//   6. IQR        — robust outlier detection (no Gaussian assumption)
//   7. Holt       — level + trend tracking (predictive)
//   8. Mahalanobis— pressure AND temp together (multivariate)
//
// Outputs:
//   - ctx->health updated (→ Modbus register 5, MQTT telemetry/alerts)
//   - SQLite /root/bmp280/fdd.db — one row per fault event (FIFO, max 10000)
//   - MQTT telemetry/report every 10 min (summary only, not raw rows)
//   - MQTT on-demand: subscribes to telemetry/cmd, responds with last 20 rows
// ============================================================

#include "fdd_thread.h"
#include "logging.h"
#include "status_buf.h"
#include <sqlite3.h>
#include <MQTTClient.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include <unistd.h>

// ============================================================
// Config
// ============================================================

#define DB_PATH              "/root/bmp280/fdd.db"
#define MAX_FDD_EVENTS       10000   // FIFO row cap — oldest deleted when exceeded
                                      // 10000 × ~100 bytes = ~1MB max on disk

#define WARMUP_SAMPLES       100     // first N samples only build baseline, no alarms
                                      // ~1.7 min at 1 sample/sec — lets Welford stabilise

#define ZSCORE_THRESHOLD     3.0     // flag if |z| > 3σ (~0.3% false positive rate)
                                      // industry standard starting point — tune on real data
#define CUSUM_K              0.5     // slack — ignores deviations below 0.5σ (noise)
#define CUSUM_H              5.0     // alarm threshold — fires after sustained drift
#define DTDT_THRESHOLD_PRESS 500.0   // Pa/sec — sudden pressure rate change alarm
#define DTDT_THRESHOLD_TEMP  2.0     // °C/sec — sudden temperature rate change alarm
#define IQR_WINDOW           50      // sliding window size for IQR calculation
#define IQR_MULTIPLIER       1.5     // standard box-plot outlier multiplier
#define EWMA_ALPHA           0.1     // smoothing factor: 0=slow/smooth, 1=fast/raw
#define HOLT_ALPHA           0.2     // level smoothing
#define HOLT_BETA            0.1     // trend smoothing
#define MAHAL_THRESHOLD      9.21    // chi-squared, 2 DOF, 99% confidence

#define REPORT_INTERVAL_SEC  600     // publish telemetry/report every 10 minutes

#define MQTT_BROKER_URI      "tcp://192.168.1.6:1883"
#define MQTT_CLIENT_ID       "telemetry_fdd"
#define TOPIC_ALERTS         "telemetry/alerts"
#define TOPIC_REPORT         "telemetry/report"
#define TOPIC_CMD            "telemetry/cmd"

// ============================================================
// Per-field state — one instance for pressure, one for temp
// ============================================================

typedef struct {
    // Welford online mean/variance
    double   mean;
    double   M2;        // sum of squared deviations (Welford accumulator)
    double   stddev;
    uint64_t n;         // sample count

    // EWMA smoothed value
    double   ewma;

    // CUSUM accumulators
    double   cusum_pos;   // upward drift
    double   cusum_neg;   // downward drift

    // Previous sample (for dT/dt)
    double   prev_sample;
    time_t   prev_time;

    // IQR sliding window (circular buffer)
    double   iqr_window[IQR_WINDOW];
    int      iqr_idx;
    int      iqr_full;   // 1 once window has been filled once

    // Holt's double exponential smoothing
    double   holt_level;
    double   holt_trend;

} field_state;

// ============================================================
// Mahalanobis — tracks covariance between pressure and temp
// ============================================================

typedef struct {
    double cov_sum;    // sum of (press_dev * temp_dev) for covariance
    uint64_t n;
} mahal_state;

// ============================================================
// Welford update — call every sample
// ============================================================

static void welford_update(field_state *s, double x)
{
    s->n++;
    double delta  = x - s->mean;
    s->mean      += delta / s->n;
    double delta2 = x - s->mean;
    s->M2        += delta * delta2;
    // stddev only valid once n > 1
    s->stddev = (s->n > 1) ? sqrt(s->M2 / (s->n - 1)) : 0.0;
}

// ============================================================
// EWMA update — smoothed value, reduces noise before Z/CUSUM
// ============================================================

static void ewma_update(field_state *s, double x)
{
    if (s->n == 1)
        s->ewma = x;   // seed with first sample
    else
        s->ewma = EWMA_ALPHA * x + (1.0 - EWMA_ALPHA) * s->ewma;
}

// ============================================================
// Holt's double exponential smoothing
// ============================================================

static void holt_update(field_state *s, double x)
{
    if (s->n == 1) {
        s->holt_level = x;
        s->holt_trend = 0.0;
        return;
    }
    double prev_level = s->holt_level;
    s->holt_level = HOLT_ALPHA * x + (1.0 - HOLT_ALPHA) * (s->holt_level + s->holt_trend);
    s->holt_trend = HOLT_BETA  * (s->holt_level - prev_level) +
                    (1.0 - HOLT_BETA) * s->holt_trend;
}

// ============================================================
// IQR: add sample to window, return 1 if outlier
// ============================================================

static int cmp_double(const void *a, const void *b)
{
    double da = *(double *)a, db = *(double *)b;
    return (da > db) - (da < db);
}

static int iqr_check(field_state *s, double x)
{
    // Add to circular window
    s->iqr_window[s->iqr_idx] = x;
    s->iqr_idx = (s->iqr_idx + 1) % IQR_WINDOW;
    if (s->iqr_idx == 0) s->iqr_full = 1;

    if (!s->iqr_full) return 0;   // window not full yet

    // Sort a copy and find Q1, Q3
    double sorted[IQR_WINDOW];
    memcpy(sorted, s->iqr_window, sizeof(sorted));
    qsort(sorted, IQR_WINDOW, sizeof(double), cmp_double);

    double Q1  = sorted[IQR_WINDOW / 4];
    double Q3  = sorted[3 * IQR_WINDOW / 4];
    double IQR = Q3 - Q1;

    return (x < Q1 - IQR_MULTIPLIER * IQR || x > Q3 + IQR_MULTIPLIER * IQR);
}

// ============================================================
// SQLite helpers
// ============================================================

static void db_insert_event(sqlite3 *db, const char *algorithm,
                             const char *sensor, double value,
                             double score, const char *fault_label)
{
    char sql[512];
    time_t now = time(NULL);
    char ts[32];
    struct tm *tm = localtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    snprintf(sql, sizeof(sql),
        "INSERT INTO events(ts, algorithm, sensor, value, score, fault_label) "
        "VALUES('%s','%s','%s',%.2f,%.2f,'%s');",
        ts, algorithm, sensor, value, score, fault_label);
    sqlite3_exec(db, sql, NULL, NULL, NULL);

    // FIFO trim — delete oldest rows beyond MAX_FDD_EVENTS
    char trim_sql[128];
    snprintf(trim_sql, sizeof(trim_sql),
        "DELETE FROM events WHERE id NOT IN "
        "(SELECT id FROM events ORDER BY id DESC LIMIT %d);",
        MAX_FDD_EVENTS);
    sqlite3_exec(db, trim_sql, NULL, NULL, NULL);
}

static sqlite3 *db_open(void)
{
    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        log_msg("[fdd] FATAL: cannot open SQLite DB %s: %s",
                DB_PATH, sqlite3_errmsg(db));
        return NULL;
    }
    // Create table if it doesn't exist
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS events ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts           TEXT,"
        "  algorithm    TEXT,"
        "  sensor       TEXT,"
        "  value        REAL,"
        "  score        REAL,"
        "  fault_label  TEXT"
        ");",
        NULL, NULL, NULL);
    return db;
}

// ============================================================
// MQTT publish helper (fire-and-forget, no reconnect here —
// fdd_thread doesn't own the MQTT connection, uses its own
// simple client just for publishing reports/alerts)
// ============================================================

static void fdd_publish(MQTTClient client, const char *topic, const char *payload)
{
    if (!MQTTClient_isConnected(client)) return;
    MQTTClient_deliveryToken token;
    MQTTClient_publish(client, topic, (int)strlen(payload),
                       payload, 1, 0, &token);
}

// ============================================================
// Run all algorithms on one sample, log/alert on fault
// ============================================================

static void run_algorithms(field_state *fs_press, field_state *fs_temp,
                            mahal_state *ms,
                            double press, double temp,
                            sqlite3 *db, MQTTClient client,
                            volatile int *health, uint64_t sample_n)
{
    int in_warmup = (sample_n <= WARMUP_SAMPLES);

    // --- 1. Welford (always runs, even in warmup) ---
    welford_update(fs_press, press);
    welford_update(fs_temp,  temp);

    // --- 2. EWMA ---
    ewma_update(fs_press, press);
    ewma_update(fs_temp,  temp);

    // --- 3. Holt ---
    holt_update(fs_press, press);
    holt_update(fs_temp,  temp);

    // Update Mahalanobis covariance
    if (sample_n > 1) {
        ms->cov_sum += (press - fs_press->mean) * (temp - fs_temp->mean);
        ms->n++;
    }

    if (in_warmup) return;   // don't alarm during warmup

    char payload[256];
    double z_press = 0, z_temp = 0;

    // --- 4. Z-score (on EWMA-smoothed values) ---
    if (fs_press->stddev > 0) {
        z_press = (fs_press->ewma - fs_press->mean) / fs_press->stddev;
        if (fabs(z_press) > ZSCORE_THRESHOLD) {
            snprintf(payload, sizeof(payload),
                "{\"algorithm\":\"zscore\",\"sensor\":\"pressure\","
                "\"value\":%.1f,\"score\":%.2f,\"fault\":\"spike\"}",
                press, z_press);
            fdd_publish(client, TOPIC_ALERTS, payload);
            db_insert_event(db, "zscore", "pressure", press, z_press, "spike");
            *health = DAEMON_HEALTH_DEGRADED;
            log_msg("[fdd] Z-score PRESSURE alert: z=%.2f", z_press);
            g_status.fdd.fault_count_total++;
            strncpy(g_status.fdd.last_fault_algo, "zscore", sizeof(g_status.fdd.last_fault_algo)-1);
            strncpy(g_status.fdd.last_fault_label, "pressure spike", sizeof(g_status.fdd.last_fault_label)-1);
            g_status.fdd.last_fault_ts = time(NULL);
            status_log(g_status.fdd_log, &g_status.fdd_log_idx, "zscore pressure spike");
        }
    }
    if (fs_temp->stddev > 0) {
        z_temp = (fs_temp->ewma - fs_temp->mean) / fs_temp->stddev;
        if (fabs(z_temp) > ZSCORE_THRESHOLD) {
            snprintf(payload, sizeof(payload),
                "{\"algorithm\":\"zscore\",\"sensor\":\"temp\","
                "\"value\":%.2f,\"score\":%.2f,\"fault\":\"spike\"}",
                temp, z_temp);
            fdd_publish(client, TOPIC_ALERTS, payload);
            db_insert_event(db, "zscore", "temp", temp, z_temp, "spike");
            *health = DAEMON_HEALTH_DEGRADED;
            log_msg("[fdd] Z-score TEMP alert: z=%.2f", z_temp);
            g_status.fdd.fault_count_total++;
            strncpy(g_status.fdd.last_fault_algo, "zscore", sizeof(g_status.fdd.last_fault_algo)-1);
            strncpy(g_status.fdd.last_fault_label, "temp spike", sizeof(g_status.fdd.last_fault_label)-1);
            g_status.fdd.last_fault_ts = time(NULL);
            status_log(g_status.fdd_log, &g_status.fdd_log_idx, "zscore temp spike");
        }
    }

    // --- 5. CUSUM (on z-scores from above) ---
    if (fs_press->stddev > 0) {
        fs_press->cusum_pos = fmax(0, fs_press->cusum_pos + z_press - CUSUM_K);
        fs_press->cusum_neg = fmax(0, fs_press->cusum_neg - z_press - CUSUM_K);
        if (fs_press->cusum_pos > CUSUM_H) {
            snprintf(payload, sizeof(payload),
                "{\"algorithm\":\"cusum\",\"sensor\":\"pressure\","
                "\"value\":%.1f,\"score\":%.2f,\"fault\":\"drift_up\"}",
                press, fs_press->cusum_pos);
            fdd_publish(client, TOPIC_ALERTS, payload);
            db_insert_event(db, "cusum", "pressure", press, fs_press->cusum_pos, "drift_up");
            *health = DAEMON_HEALTH_DEGRADED;
            log_msg("[fdd] CUSUM pressure drift UP: S+=%.2f", fs_press->cusum_pos);
            g_status.fdd.fault_count_total++;
            strncpy(g_status.fdd.last_fault_algo, "cusum", sizeof(g_status.fdd.last_fault_algo)-1);
            strncpy(g_status.fdd.last_fault_label, "pressure drift_up", sizeof(g_status.fdd.last_fault_label)-1);
            g_status.fdd.last_fault_ts = time(NULL);
            status_log(g_status.fdd_log, &g_status.fdd_log_idx, "cusum pressure drift_up");
            fs_press->cusum_pos = 0;   // reset after alarm
        }
        if (fs_press->cusum_neg > CUSUM_H) {
            snprintf(payload, sizeof(payload),
                "{\"algorithm\":\"cusum\",\"sensor\":\"pressure\","
                "\"value\":%.1f,\"score\":%.2f,\"fault\":\"drift_down\"}",
                press, fs_press->cusum_neg);
            fdd_publish(client, TOPIC_ALERTS, payload);
            db_insert_event(db, "cusum", "pressure", press, fs_press->cusum_neg, "drift_down");
            *health = DAEMON_HEALTH_DEGRADED;
            log_msg("[fdd] CUSUM pressure drift DOWN: S-=%.2f", fs_press->cusum_neg);
            g_status.fdd.fault_count_total++;
            strncpy(g_status.fdd.last_fault_algo, "cusum", sizeof(g_status.fdd.last_fault_algo)-1);
            strncpy(g_status.fdd.last_fault_label, "pressure drift_down", sizeof(g_status.fdd.last_fault_label)-1);
            g_status.fdd.last_fault_ts = time(NULL);
            status_log(g_status.fdd_log, &g_status.fdd_log_idx, "cusum pressure drift_down");
            fs_press->cusum_neg = 0;
        }
    }
    if (fs_temp->stddev > 0) {
        fs_temp->cusum_pos = fmax(0, fs_temp->cusum_pos + z_temp - CUSUM_K);
        fs_temp->cusum_neg = fmax(0, fs_temp->cusum_neg - z_temp - CUSUM_K);
        if (fs_temp->cusum_pos > CUSUM_H) {
            snprintf(payload, sizeof(payload),
                "{\"algorithm\":\"cusum\",\"sensor\":\"temp\","
                "\"value\":%.2f,\"score\":%.2f,\"fault\":\"drift_up\"}",
                temp, fs_temp->cusum_pos);
            fdd_publish(client, TOPIC_ALERTS, payload);
            db_insert_event(db, "cusum", "temp", temp, fs_temp->cusum_pos, "drift_up");
            *health = DAEMON_HEALTH_DEGRADED;
            log_msg("[fdd] CUSUM temp drift UP: S+=%.2f", fs_temp->cusum_pos);
            g_status.fdd.fault_count_total++;
            strncpy(g_status.fdd.last_fault_algo, "cusum", sizeof(g_status.fdd.last_fault_algo)-1);
            strncpy(g_status.fdd.last_fault_label, "temp drift_up", sizeof(g_status.fdd.last_fault_label)-1);
            g_status.fdd.last_fault_ts = time(NULL);
            status_log(g_status.fdd_log, &g_status.fdd_log_idx, "cusum temp drift_up");
            fs_temp->cusum_pos = 0;
        }
        if (fs_temp->cusum_neg > CUSUM_H) {
            snprintf(payload, sizeof(payload),
                "{\"algorithm\":\"cusum\",\"sensor\":\"temp\","
                "\"value\":%.2f,\"score\":%.2f,\"fault\":\"drift_down\"}",
                temp, fs_temp->cusum_neg);
            fdd_publish(client, TOPIC_ALERTS, payload);
            db_insert_event(db, "cusum", "temp", temp, fs_temp->cusum_neg, "drift_down");
            *health = DAEMON_HEALTH_DEGRADED;
            log_msg("[fdd] CUSUM temp drift DOWN: S-=%.2f", fs_temp->cusum_neg);
            g_status.fdd.fault_count_total++;
            strncpy(g_status.fdd.last_fault_algo, "cusum", sizeof(g_status.fdd.last_fault_algo)-1);
            strncpy(g_status.fdd.last_fault_label, "temp drift_down", sizeof(g_status.fdd.last_fault_label)-1);
            g_status.fdd.last_fault_ts = time(NULL);
            status_log(g_status.fdd_log, &g_status.fdd_log_idx, "cusum temp drift_down");
            fs_temp->cusum_neg = 0;
        }
    }

    // --- 6. dT/dt — rate of change (fastest alarm) ---
    time_t now = time(NULL);
    if (fs_press->prev_time > 0) {
        double dt = difftime(now, fs_press->prev_time);
        if (dt > 0) {
            double rate_press = (press - fs_press->prev_sample) / dt;
            double rate_temp  = (temp  - fs_temp->prev_sample)  / dt;

            if (fabs(rate_press) > DTDT_THRESHOLD_PRESS) {
                snprintf(payload, sizeof(payload),
                    "{\"algorithm\":\"dtdt\",\"sensor\":\"pressure\","
                    "\"value\":%.1f,\"score\":%.1f,\"fault\":\"%s\"}",
                    press, rate_press,
                    rate_press > 0 ? "sudden_rise" : "sudden_drop");
                fdd_publish(client, TOPIC_ALERTS, payload);
                db_insert_event(db, "dtdt", "pressure", press, rate_press,
                                rate_press > 0 ? "sudden_rise" : "sudden_drop");
                *health = DAEMON_HEALTH_DEGRADED;
                log_msg("[fdd] dT/dt pressure: %.1f Pa/s", rate_press);
                g_status.fdd.fault_count_total++;
                strncpy(g_status.fdd.last_fault_algo, "dtdt", sizeof(g_status.fdd.last_fault_algo)-1);
                strncpy(g_status.fdd.last_fault_label, rate_press > 0 ? "pressure sudden_rise" : "pressure sudden_drop", sizeof(g_status.fdd.last_fault_label)-1);
                g_status.fdd.last_fault_ts = time(NULL);
                status_log(g_status.fdd_log, &g_status.fdd_log_idx, "dtdt pressure rate alarm");
            }
            if (fabs(rate_temp) > DTDT_THRESHOLD_TEMP) {
                snprintf(payload, sizeof(payload),
                    "{\"algorithm\":\"dtdt\",\"sensor\":\"temp\","
                    "\"value\":%.2f,\"score\":%.2f,\"fault\":\"%s\"}",
                    temp, rate_temp,
                    rate_temp > 0 ? "sudden_rise" : "sudden_drop");
                fdd_publish(client, TOPIC_ALERTS, payload);
                db_insert_event(db, "dtdt", "temp", temp, rate_temp,
                                rate_temp > 0 ? "sudden_rise" : "sudden_drop");
                *health = DAEMON_HEALTH_DEGRADED;
                log_msg("[fdd] dT/dt temp: %.2f °C/s", rate_temp);
                g_status.fdd.fault_count_total++;
                strncpy(g_status.fdd.last_fault_algo, "dtdt", sizeof(g_status.fdd.last_fault_algo)-1);
                strncpy(g_status.fdd.last_fault_label, rate_temp > 0 ? "temp sudden_rise" : "temp sudden_drop", sizeof(g_status.fdd.last_fault_label)-1);
                g_status.fdd.last_fault_ts = time(NULL);
                status_log(g_status.fdd_log, &g_status.fdd_log_idx, "dtdt temp rate alarm");
            }
        }
    }
    fs_press->prev_sample = press;
    fs_temp->prev_sample  = temp;
    fs_press->prev_time   = now;
    fs_temp->prev_time    = now;

    // --- 7. IQR ---
    if (iqr_check(fs_press, press)) {
        snprintf(payload, sizeof(payload),
            "{\"algorithm\":\"iqr\",\"sensor\":\"pressure\","
            "\"value\":%.1f,\"score\":0,\"fault\":\"outlier\"}",
            press);
        fdd_publish(client, TOPIC_ALERTS, payload);
        db_insert_event(db, "iqr", "pressure", press, 0, "outlier");
        *health = DAEMON_HEALTH_DEGRADED;
        log_msg("[fdd] IQR pressure outlier: %.1f Pa", press);
        g_status.fdd.fault_count_total++;
        strncpy(g_status.fdd.last_fault_algo, "iqr", sizeof(g_status.fdd.last_fault_algo)-1);
        strncpy(g_status.fdd.last_fault_label, "pressure outlier", sizeof(g_status.fdd.last_fault_label)-1);
        g_status.fdd.last_fault_ts = time(NULL);
        status_log(g_status.fdd_log, &g_status.fdd_log_idx, "iqr pressure outlier");
    }
    if (iqr_check(fs_temp, temp)) {
        snprintf(payload, sizeof(payload),
            "{\"algorithm\":\"iqr\",\"sensor\":\"temp\","
            "\"value\":%.2f,\"score\":0,\"fault\":\"outlier\"}",
            temp);
        fdd_publish(client, TOPIC_ALERTS, payload);
        db_insert_event(db, "iqr", "temp", temp, 0, "outlier");
        *health = DAEMON_HEALTH_DEGRADED;
        log_msg("[fdd] IQR temp outlier: %.2f °C", temp);
        g_status.fdd.fault_count_total++;
        strncpy(g_status.fdd.last_fault_algo, "iqr", sizeof(g_status.fdd.last_fault_algo)-1);
        strncpy(g_status.fdd.last_fault_label, "temp outlier", sizeof(g_status.fdd.last_fault_label)-1);
        g_status.fdd.last_fault_ts = time(NULL);
        status_log(g_status.fdd_log, &g_status.fdd_log_idx, "iqr temp outlier");
    }

    // --- 8. Mahalanobis (pressure + temp together) ---
    if (ms->n > 10 && fs_press->stddev > 0 && fs_temp->stddev > 0) {
        double cov     = ms->cov_sum / ms->n;
        double var_p   = fs_press->stddev * fs_press->stddev;
        double var_t   = fs_temp->stddev  * fs_temp->stddev;
        double det     = var_p * var_t - cov * cov;

        if (fabs(det) > 1e-10) {
            // 2×2 matrix inverse: Σ⁻¹ = (1/det) * [[var_t, -cov], [-cov, var_p]]
            double dp = press - fs_press->mean;
            double dt_val = temp - fs_temp->mean;
            double D2 = (var_t * dp * dp - 2 * cov * dp * dt_val + var_p * dt_val * dt_val) / det;

            if (D2 > MAHAL_THRESHOLD) {
                snprintf(payload, sizeof(payload),
                    "{\"algorithm\":\"mahalanobis\",\"sensor\":\"both\","
                    "\"value\":%.1f,\"score\":%.2f,\"fault\":\"multivariate_anomaly\","
                    "\"pressure\":%.1f,\"temp\":%.2f}",
                    D2, D2, press, temp);
                fdd_publish(client, TOPIC_ALERTS, payload);
                db_insert_event(db, "mahalanobis", "both", D2, D2, "multivariate_anomaly");
                *health = DAEMON_HEALTH_DEGRADED;
                log_msg("[fdd] Mahalanobis anomaly: D²=%.2f (press=%.1f temp=%.2f)",
                        D2, press, temp);
                g_status.fdd.fault_count_total++;
                strncpy(g_status.fdd.last_fault_algo, "mahalanobis", sizeof(g_status.fdd.last_fault_algo)-1);
                strncpy(g_status.fdd.last_fault_label, "multivariate anomaly", sizeof(g_status.fdd.last_fault_label)-1);
                g_status.fdd.last_fault_ts = time(NULL);
                status_log(g_status.fdd_log, &g_status.fdd_log_idx, "mahalanobis anomaly");
            }
        }
    }

    // --- Holt trend alert (sustained trend over threshold) ---
    // Flag if trend is consistently strong (e.g. pressure dropping > 10 Pa/sample)
    if (fabs(fs_press->holt_trend) > 10.0) {
        snprintf(payload, sizeof(payload),
            "{\"algorithm\":\"holt\",\"sensor\":\"pressure\","
            "\"value\":%.1f,\"score\":%.2f,\"fault\":\"%s\"}",
            press, fs_press->holt_trend,
            fs_press->holt_trend > 0 ? "trend_rising" : "trend_falling");
        fdd_publish(client, TOPIC_ALERTS, payload);
        db_insert_event(db, "holt", "pressure", press, fs_press->holt_trend,
                        fs_press->holt_trend > 0 ? "trend_rising" : "trend_falling");
        log_msg("[fdd] Holt pressure trend: %.2f Pa/sample", fs_press->holt_trend);
        g_status.fdd.fault_count_total++;
        strncpy(g_status.fdd.last_fault_algo, "holt", sizeof(g_status.fdd.last_fault_algo)-1);
        strncpy(g_status.fdd.last_fault_label,
                fs_press->holt_trend > 0 ? "pressure trend_rising" : "pressure trend_falling",
                sizeof(g_status.fdd.last_fault_label)-1);
        g_status.fdd.last_fault_ts = time(NULL);
        status_log(g_status.fdd_log, &g_status.fdd_log_idx, "holt pressure trend alarm");
    }
}

// ============================================================
// Publish periodic report (summary, not raw rows)
// ============================================================

static void publish_report(MQTTClient client, sqlite3 *db,
                            field_state *fs_press, field_state *fs_temp)
{
    // Count fault events in last 10 minutes from SQLite
    int fault_count = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM events WHERE ts >= datetime('now','-10 minutes');",
            -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            fault_count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"period_min\":10,"
        "\"pressure\":{\"mean\":%.1f,\"stddev\":%.1f,\"trend\":%.2f},"
        "\"temp\":{\"mean\":%.2f,\"stddev\":%.2f,\"trend\":%.2f},"
        "\"fault_events\":%d,\"samples\":%" PRIu64 "}",
        fs_press->mean, fs_press->stddev, fs_press->holt_trend,
        fs_temp->mean,  fs_temp->stddev,  fs_temp->holt_trend,
        fault_count, fs_press->n);

    if (MQTTClient_isConnected(client))
        fdd_publish(client, TOPIC_REPORT, payload);

    log_msg("[fdd] report published: press_mean=%.1f temp_mean=%.2f faults=%d",
            fs_press->mean, fs_temp->mean, fault_count);
}

// ============================================================
// Thread entry point
// ============================================================

void *fdd_thread(void *arg)
{
    thread_ctx *ctx = (thread_ctx *)arg;

    log_msg("[fdd] thread started");

    // Wait for sensor to be ready (same pattern as core3_monitor)
    if (!ctx->boot) {
        log_msg("[fdd] waiting for sensor...");
        while (*ctx->running && !ctx->boot)
            interruptible_sleep(2, ctx->running);
        if (!*ctx->running) return NULL;
        log_msg("[fdd] sensor ready, starting FDD");
    }

    // Open SQLite DB
    sqlite3 *db = db_open();
    if (!db) return NULL;

    // Create MQTT client (separate from mqtt_thread's client)
    MQTTClient client;
    MQTTClient_connectOptions conn = MQTTClient_connectOptions_initializer;
    conn.cleansession = 1;
    int mqtt_ok = 0;

    if (MQTTClient_create(&client, MQTT_BROKER_URI, MQTT_CLIENT_ID,
                           MQTTCLIENT_PERSISTENCE_NONE, NULL) == MQTTCLIENT_SUCCESS) {
        if (MQTTClient_connect(client, &conn) == MQTTCLIENT_SUCCESS) {
            mqtt_ok = 1;
            log_msg("[fdd] MQTT connected");
        } else {
            log_msg("[fdd] MQTT connect failed — alerts/reports will log to SQLite only");
        }
    }

    // Per-field state — zeroed out (all algorithms start from zero)
    field_state fs_press = {0};
    field_state fs_temp  = {0};
    mahal_state ms       = {0};

    time_t last_report = time(NULL);
    uint64_t sample_n  = 0;

    log_msg("[fdd] warmup: collecting %d samples before alarming...", WARMUP_SAMPLES);

    while (*ctx->running) {
        bootstrap_cfg *boot = ctx->boot;
        ring_entry    *ring = ctx->ring;

        if (!boot || !ring) {
            interruptible_sleep(1, ctx->running);
            continue;
        }

        // Read latest sample from ring buffer
        uint64_t wi = boot->write_idx;
        ring_entry latest = ring[wi == 0 ? RING_ENTRY_COUNT - 1 : wi - 1];

        double press = (double)latest.pressure;
        double temp  = latest.temp / 100.0;   // stored as int × 100

        sample_n++;
        g_status.fdd.sample_n  = sample_n;
        g_status.fdd.in_warmup = (sample_n <= WARMUP_SAMPLES);

        if (sample_n == WARMUP_SAMPLES) {
            log_msg("[fdd] warmup complete — alarms now active");
            status_log(g_status.fdd_log, &g_status.fdd_log_idx,
                       "warmup complete — alarms active");
        }

        run_algorithms(&fs_press, &fs_temp, &ms,
                       press, temp, db, client,
                       ctx->health, sample_n);

        // Publish report every 10 minutes
        if (difftime(time(NULL), last_report) >= REPORT_INTERVAL_SEC) {
            publish_report(client, db, &fs_press, &fs_temp);
            last_report = time(NULL);
        }

        interruptible_sleep(1, ctx->running);   // 1 sample per second
    }

    if (mqtt_ok) {
        MQTTClient_disconnect(client, 1000);
        MQTTClient_destroy(&client);
    }
    sqlite3_close(db);

    log_msg("[fdd] thread stopping (shutdown requested)");
    return NULL;
}
