// ============================================================
// shared_mem.h — single source of truth for the shared-RAM layout
// ============================================================
// Must byte-match bmp280_driver.c / core3_loop.c / read_ring.c exactly.
// Every thread in this daemon includes this ONE header instead of each
// re-declaring the struct — the previous version of this daemon (before the
// multi-file split) duplicated this struct inline, same as core3_loop.c and
// read_ring.c already do. That's now 4 independent copies across the project
// that all have to be kept in sync by hand — a real drift risk. This header
// at least stops a 5th copy from happening inside the daemon's own threads.
// ============================================================

#ifndef SHARED_MEM_H
#define SHARED_MEM_H

#include <stdint.h>
#include <signal.h>
#include <unistd.h>

#define BOOTSTRAP_MAGIC    0xDEADBEEFCAFEBABEULL
#define RING_ENTRY_COUNT   1000
#define BOOTSTRAP_SIZE     72
#define BUF_SIZE           16384   // PAGE_ALIGN(BOOTSTRAP_SIZE + RING_SIZE)

typedef struct {
    uint64_t magic;
    uint64_t ring_phys_addr;
    uint64_t ring_entry_count;
    uint64_t threshold_press;
    uint64_t threshold_temp;
    uint64_t flags;
    uint64_t write_idx;
    uint64_t heartbeat_counter;
    uint64_t core3_dead;
} __attribute__((packed)) bootstrap_cfg;

typedef struct {
    uint64_t timestamp;
    uint32_t pressure;
    int32_t  temp;
} __attribute__((packed)) ring_entry;

// Daemon-level health, as determined by core3_monitor's debounce/restart
// logic — written only by core3_monitor, read by mqtt_thread (Phase 10) to
// publish on the alerts topic. Added 2026-10-05: this state used to be
// trapped in core3_monitor_thread()'s own local variables, invisible to
// every other thread — see phase-10.md for the full reasoning.
typedef enum {
    DAEMON_HEALTH_OK       = 0,   // core3_dead is 0, or noise within debounce
    DAEMON_HEALTH_DEGRADED = 1,   // a restart attempt has fired, not given up yet
    DAEMON_HEALTH_GIVEN_UP = 2,   // MAX_RESTART_ATTEMPTS exhausted, needs a human
} daemon_health_t;

// Passed to every thread's entry point — one shared mmap, one shared
// shutdown flag, so no thread needs its own open()/mmap() of /dev/bmp280.
typedef struct {
    bootstrap_cfg          *boot;
    ring_entry             *ring;
    volatile sig_atomic_t  *running;
    volatile int           *health;   // daemon_health_t value — see above.
                                       // Plain volatile int, same risk/benefit
                                       // tradeoff this codebase already made
                                       // for `running` (sig_atomic_t) — not a
                                       // full C11 atomic, but a single aligned
                                       // int write/read is good enough here;
                                       // not worth a mutex for one field that
                                       // changes at most once a minute.
} thread_ctx;

// Sleeps in 1-second steps, checking *running each time, instead of one
// long blocking sleep(seconds). A plain sleep() doesn't wake up early just
// because someone else set the shutdown flag — every thread in this daemon
// uses this instead, so a SIGTERM is noticed within ~1s instead of however
// long the thread's own poll interval happens to be. Found this the hard
// way: without it, pthread_join() on a sleeping thread during shutdown
// could block for the thread's full sleep duration (confirmed on-device —
// shutdown took far longer than expected before this fix).
static inline void interruptible_sleep(int seconds, volatile sig_atomic_t *running)
{
    for (int i = 0; i < seconds && *running; i++)
        sleep(1);
}

#endif // SHARED_MEM_H
