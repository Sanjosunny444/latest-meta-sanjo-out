#ifndef MODBUS_THREAD_H
#define MODBUS_THREAD_H

#include "shared_mem.h"

// Phase 9 (not started) fills this in — Modbus RTU slave over RS485/UART,
// exposes sensor registers for SCADA/HMI polling (libmodbus per the plan).
// Stub for now.
void *modbus_thread(void *arg);

#endif // MODBUS_THREAD_H
