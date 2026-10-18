#ifndef SYSTEM_HEALTH_H
#define SYSTEM_HEALTH_H

// Gathers RPi board-level health (not sensor data) — RAM, storage, CPU
// temp, WiFi signal, uptime, load average — all read directly from /proc
// and /sys. No external library needed (unlike mqtt_thread.c's dependency
// on Paho) — kept separate anyway so this logic stays independently
// readable/testable, same reasoning as logging.h/shared_mem.h being split
// out of telemetry_daemon.c.
//
// Writes a JSON payload into `out` (must be at least `out_size` bytes).
// Any single field whose source is unavailable is written as a sentinel
// (-1 for values that are never legitimately negative) rather than failing
// the whole call — one missing /proc file shouldn't block every other
// field from being reported.
void system_health_json(char *out, int out_size);

#endif // SYSTEM_HEALTH_H
