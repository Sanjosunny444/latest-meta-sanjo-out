#ifndef CAN_THREAD_H
#define CAN_THREAD_H

#include "shared_mem.h"

// Phase 8 (not started) fills this in — SocketCAN input, parses commands
// from an external PLC/ECU, writes config (thresholds, sample rate) into
// the shared bootstrap_cfg region. Stub for now: just proves the thread
// starts, runs, and shuts down cleanly alongside the others.
void *can_thread(void *arg);

#endif // CAN_THREAD_H
