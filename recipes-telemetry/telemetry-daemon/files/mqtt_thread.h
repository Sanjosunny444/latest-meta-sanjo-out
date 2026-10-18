#ifndef MQTT_THREAD_H
#define MQTT_THREAD_H

#include "shared_mem.h"

// Phase 10 — publishes sensor data + alerts + online/offline presence to an
// MQTT broker over WiFi. See phase-10.md for the full design.
void *mqtt_thread(void *arg);

#endif // MQTT_THREAD_H
