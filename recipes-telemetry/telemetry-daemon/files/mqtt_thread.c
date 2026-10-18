// ============================================================
// mqtt_thread.c — Phase 10 (see phase-10.md for the full design history)
// ============================================================
// Publish-only. Never subscribes to anything.
//
// Four topics:
//   telemetry/sensor  — pressure, temp, flags, thresholds, measured sample
//                        rate — every ~30s
//   telemetry/system  — RPi board health (RAM, storage, CPU temp, WiFi
//                        signal, uptime, load avg) — every 10s. See
//                        system_health.c — deliberately NOT sensor data,
//                        this is about the board itself, not the BMP280.
//   telemetry/alerts  — daemon health (core3_monitor's debounce/restart
//                        state, via the shared `health` field in
//                        thread_ctx) — only published when it CHANGES
//   telemetry/status  — online/offline presence via MQTT Birth + Last Will:
//                        "online" published right after connecting, and an
//                        "offline" Last Will registered with the broker at
//                        connect time so the BROKER publishes it on our
//                        behalf if the connection drops ungracefully (we
//                        don't have to still be alive to say we're gone)
//
// Broker config below is a placeholder pointing at the local test broker
// (see ~/Downloads/mqtt-test-broker/) — swap MQTT_BROKER_URI etc. for the
// real broker's details when ready. Still an open question in phase-10.md
// whether this should become a config file instead of a #define.
// ============================================================

#include "mqtt_thread.h"
#include "logging.h"
#include "system_health.h"
#include "status_buf.h"
#include <MQTTClient.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// ============================================================
// Config
// ============================================================

#define MQTT_BROKER_URI   "tcp://192.168.1.6:1883"   // local test broker on the dev
                                                       // laptop's LAN IP — swap for the
                                                       // real broker later (and note:
                                                       // this IP isn't guaranteed stable,
                                                       // re-check if the laptop's DHCP
                                                       // lease ever changes)
#define MQTT_CLIENT_ID    "telemetry_daemon"
#define MQTT_QOS          1   // at-least-once — presence/alerts are infrequent
                               // and important enough to be worth the overhead

#define TOPIC_SENSOR   "telemetry/sensor"
#define TOPIC_SYSTEM   "telemetry/system"
#define TOPIC_ALERTS   "telemetry/alerts"
#define TOPIC_STATUS   "telemetry/status"

#define RECONNECT_RETRY_INTERVAL_SEC  10   // don't hammer reconnect attempts
                                            // every single second while the
                                            // broker/network is down — found
                                            // this gap for real: the original
                                            // version connected once at
                                            // startup and never recovered
                                            // from a dropped connection,
                                            // confirmed on-device (repeated
                                            // rc=-3 MQTTCLIENT_DISCONNECTED
                                            // publish failures, forever)
#define SENSOR_PUBLISH_INTERVAL_SEC   30
#define SYSTEM_PUBLISH_INTERVAL_SEC   10   // board health — cheap to gather
                                            // (just /proc and /sys reads), so
                                            // a shorter interval than sensor
                                            // data is fine
#define RATE_MEASURE_WINDOW_SEC        4   // window used to measure the real
                                            // sample rate (two heartbeat_counter
                                            // reads this many seconds apart) —
                                            // cheap: two shared-memory reads
                                            // plus simple arithmetic, nothing
                                            // heavier. NOTE: this window adds
                                            // to the publish cycle, so the real
                                            // sensor-publish period ends up
                                            // ~30+4=34s, not exactly 30 — close
                                            // enough for a "default ~30s"
                                            // requirement, not worth the extra
                                            // complexity of hiding the overlap.

// ============================================================
// Small helper — publish a string payload, log on failure
// ============================================================

static int publish_str(MQTTClient client, const char *topic,
                        const char *payload, int retained)
{
    MQTTClient_deliveryToken token;
    int rc = MQTTClient_publish(client, topic, (int)strlen(payload),
                                 payload, MQTT_QOS, retained, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        log_msg("[mqtt] publish to %s failed, rc=%d", topic, rc);
    } else {
        log_msg("[mqtt] published to %s: %s", topic, payload);
        g_status.mqtt.last_pub = time(NULL);
        strncpy(g_status.mqtt.last_topic, topic, sizeof(g_status.mqtt.last_topic) - 1);
        g_status.mqtt.pub_count++;
        char logbuf[STATUS_MSG_LEN];
        snprintf(logbuf, sizeof(logbuf), "pub %s", topic);
        status_log(g_status.mqtt_log, &g_status.mqtt_log_idx, logbuf);
    }
    return rc;
}

// Checks the connection and reconnects if needed, backed off to at most once
// every RECONNECT_RETRY_INTERVAL_SEC so a down broker/network doesn't get
// hammered with a connection attempt every single second. Re-publishes the
// Birth message on a successful reconnect, since a fresh MQTTClient_connect()
// call (even with cleansession=1) starts a new session from the broker's
// point of view.
static int ensure_connected(MQTTClient client, MQTTClient_connectOptions *conn_opts,
                             int *seconds_since_last_attempt)
{
    if (MQTTClient_isConnected(client)) {
        *seconds_since_last_attempt = RECONNECT_RETRY_INTERVAL_SEC;  // allow
                                                                       // an immediate
                                                                       // attempt the
                                                                       // next time it drops
        return 1;
    }

    if (*seconds_since_last_attempt < RECONNECT_RETRY_INTERVAL_SEC) {
        (*seconds_since_last_attempt)++;
        return 0;
    }
    *seconds_since_last_attempt = 0;

    log_msg("[mqtt] connection lost — attempting to reconnect to %s", MQTT_BROKER_URI);
    int rc = MQTTClient_connect(client, conn_opts);
    if (rc == MQTTCLIENT_SUCCESS) {
        log_msg("[mqtt] reconnected to %s", MQTT_BROKER_URI);
        publish_str(client, TOPIC_STATUS, "{\"status\":\"online\"}", 1);
        return 1;
    }
    log_msg("[mqtt] reconnect attempt failed, rc=%d — will retry in %ds",
            rc, RECONNECT_RETRY_INTERVAL_SEC);
    return 0;
}

static const char *health_name(int health)
{
    switch (health) {
        case DAEMON_HEALTH_OK:       return "healthy";
        case DAEMON_HEALTH_DEGRADED: return "degraded";
        case DAEMON_HEALTH_GIVEN_UP: return "given_up";
        default:                     return "unknown";
    }
}

// Found this one for real, on-device: MQTTClient_yield() MUST be called
// periodically for a single-threaded client like ours that never calls
// MQTTClient_receive() — otherwise Paho never gets a chance to send its own
// keepalive PINGREQ packets, and the BROKER's own keepalive timeout
// eventually disconnects us even though nothing about the network is
// actually wrong. Confirmed: broker logged "Client telemetry_daemon has
// exceeded timeout, disconnecting" ~45s after connecting, during a run
// where nothing else was interfering — root cause was this thread doing
// plain sleep() for its idle waiting, bypassing Paho's internals entirely.
// This replaces the shared interruptible_sleep() for THIS thread only —
// still checks *running the same way, just also pumps MQTTClient_yield()
// often (every 200ms) so it stays comfortably inside any reasonable
// keepAliveInterval regardless of which part of the loop is "idling".
static void mqtt_idle_wait(int seconds, volatile sig_atomic_t *running)
{
    int ticks = seconds * 5;   // 5 ticks/sec = one every 200ms
    for (int i = 0; i < ticks && *running; i++) {
        MQTTClient_yield();
        usleep(200000);
    }
}

// ============================================================
// Thread entry point
// ============================================================

void *mqtt_thread(void *arg)
{
    thread_ctx *ctx = (thread_ctx *)arg;
    bootstrap_cfg *boot = ctx->boot;
    ring_entry    *ring = ctx->ring;

    log_msg("[mqtt] thread started, broker=%s", MQTT_BROKER_URI);
    status_log(g_status.mqtt_log, &g_status.mqtt_log_idx, "thread started");

    MQTTClient client;
    int rc = MQTTClient_create(&client, MQTT_BROKER_URI, MQTT_CLIENT_ID,
                                MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        log_msg("[mqtt] FATAL: MQTTClient_create failed, rc=%d — thread "
                "exiting, no publishing will happen", rc);
        return NULL;
    }

    // Last Will — the broker publishes this on our behalf if we disconnect
    // ungracefully (crash, power loss, WiFi drop). retained=1 so a new
    // subscriber immediately sees current status instead of waiting for the
    // next change.
    MQTTClient_willOptions will = MQTTClient_willOptions_initializer;
    will.topicName = TOPIC_STATUS;
    will.message   = "{\"status\":\"offline\"}";
    will.retained  = 1;
    will.qos       = MQTT_QOS;

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    // keepAliveInterval left at the library default (60s, already set by the
    // initializer above) — not explicitly set here. 20s was tried first for
    // no strong reason and reverted; 60s is standard, and our own publish
    // cadence (~30-34s) is more frequent than the keepalive window anyway.
    conn_opts.cleansession = 1;
    conn_opts.will         = &will;

    rc = MQTTClient_connect(client, &conn_opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        log_msg("[mqtt] initial connect to %s failed, rc=%d — will keep "
                "retrying every %ds inside the main loop",
                MQTT_BROKER_URI, rc, RECONNECT_RETRY_INTERVAL_SEC);
        status_log(g_status.mqtt_log, &g_status.mqtt_log_idx,
                   "initial connect failed — retrying");
    } else {
        log_msg("[mqtt] connected to %s", MQTT_BROKER_URI);
        publish_str(client, TOPIC_STATUS, "{\"status\":\"online\"}", 1);
        g_status.mqtt.connected = 1;
        status_log(g_status.mqtt_log, &g_status.mqtt_log_idx, "connected");
    }

    int last_published_health = -1;   // force a first publish regardless of
                                       // the actual starting value
    int elapsed = 0;          // sensor-publish cycle (~30-34s)
    int elapsed_system = 0;   // system-health cycle (10s) — tracked separately
                               // since the two run on different intervals
    int seconds_since_last_reconnect_attempt = 0;  // try reconnect immediately
                                                    // on first loop iteration
                                                    // if initial connect failed

    while (*ctx->running) {
        int connected = ensure_connected(client, &conn_opts,
                                          &seconds_since_last_reconnect_attempt);
        g_status.mqtt.connected = connected;

        // --- sensor data, every ~30s (see RATE_MEASURE_WINDOW_SEC note) ---
        // Only publish if sensor is actually available (boot != NULL). If not
        // ready yet, still advance the timer — the moment boot becomes
        // available, we publish immediately without waiting another full cycle.
        // This lets MQTT start publishing real data as soon as the sensor is
        // ready, without any delay.
        if (connected && elapsed >= SENSOR_PUBLISH_INTERVAL_SEC) {
            elapsed = 0;

            if (boot) {
                // Sensor is available — publish real data
                uint64_t hb_start = boot->heartbeat_counter;
                mqtt_idle_wait(RATE_MEASURE_WINDOW_SEC, ctx->running);
                uint64_t hb_end = boot->heartbeat_counter;
                double rate_hz = (double)(hb_end - hb_start) / RATE_MEASURE_WINDOW_SEC;

                uint64_t wi = boot->write_idx;
                ring_entry latest = ring[wi == 0 ? RING_ENTRY_COUNT - 1 : wi - 1];

                char payload[256];
                snprintf(payload, sizeof(payload),
                    "{\"pressure\":%u,\"temp\":%.2f,\"flags\":%llu,"
                    "\"threshold_press\":%llu,\"threshold_temp\":%llu,"
                    "\"sample_rate_hz\":%.1f}",
                    latest.pressure, latest.temp / 100.0,
                    (unsigned long long)boot->flags,
                    (unsigned long long)boot->threshold_press,
                    (unsigned long long)boot->threshold_temp,
                    rate_hz);

                publish_str(client, TOPIC_SENSOR, payload, 0);
            } else {
                // Sensor not ready yet — publish sentinel values (all zeros)
                // so subscribers know we're alive but waiting for hardware
                char payload[256];
                snprintf(payload, sizeof(payload),
                    "{\"pressure\":0,\"temp\":0.0,\"flags\":0,"
                    "\"threshold_press\":0,\"threshold_temp\":0,"
                    "\"sample_rate_hz\":0.0}");

                publish_str(client, TOPIC_SENSOR, payload, 0);
            }
        }

        // --- board health, every 10s — cheap, independent of sensor cycle ---
        if (connected && elapsed_system >= SYSTEM_PUBLISH_INTERVAL_SEC) {
            elapsed_system = 0;
            char sys_payload[256];
            system_health_json(sys_payload, sizeof(sys_payload));
            publish_str(client, TOPIC_SYSTEM, sys_payload, 0);
        }

        // --- alerts — only on change, not every cycle ---
        // Deliberately does NOT update last_published_health while
        // disconnected, same reasoning as elapsed above: if health changed
        // while we had no connection, publish it the moment we're back
        // instead of silently missing that transition.
        int cur_health = *ctx->health;
        if (connected && cur_health != last_published_health) {
            char payload[128];
            snprintf(payload, sizeof(payload),
                "{\"health\":%d,\"status\":\"%s\"}",
                cur_health, health_name(cur_health));
            publish_str(client, TOPIC_ALERTS, payload, 0);
            last_published_health = cur_health;
        }

        mqtt_idle_wait(1, ctx->running);
        elapsed++;
        elapsed_system++;
    }

    // Graceful shutdown — publish an explicit offline status ourselves
    // (distinct from the broker-triggered Last Will, which only fires on an
    // UNGRACEFUL drop) so a clean stop is distinguishable in the log from a
    // real failure.
    publish_str(client, TOPIC_STATUS, "{\"status\":\"offline\"}", 1);
    MQTTClient_disconnect(client, 1000);
    MQTTClient_destroy(&client);

    log_msg("[mqtt] thread stopping (shutdown requested)");
    return NULL;
}
