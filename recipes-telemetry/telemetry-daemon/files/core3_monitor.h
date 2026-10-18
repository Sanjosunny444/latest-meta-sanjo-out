#ifndef CORE3_MONITOR_H
#define CORE3_MONITOR_H

#include "shared_mem.h"

// Thread 1 from phase-7.md — the one real, fully-implemented thread.
// Reads the ring buffer, applies the 10-consecutive/~1-min debounce rule on
// core3_dead, auto-restarts core3_loop, escalates to a full device reset
// after 2 repeated failures (~20 min).
void *core3_monitor_thread(void *arg);

#endif // CORE3_MONITOR_H
