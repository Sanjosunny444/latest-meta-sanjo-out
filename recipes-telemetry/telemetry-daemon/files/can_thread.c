#include "can_thread.h"
#include "logging.h"
#include <unistd.h>

void *can_thread(void *arg)
{
    thread_ctx *ctx = (thread_ctx *)arg;

    log_msg("[can] thread started (stub — Phase 8 not implemented yet)");

    // Placeholder loop — real version (Phase 8) will epoll on a CAN socket
    // instead of sleeping. Checking *ctx->running periodically here is what
    // lets this stub shut down cleanly with the rest of the daemon, same as
    // every other thread.
    while (*ctx->running) {
        interruptible_sleep(5, ctx->running);
    }

    log_msg("[can] thread stopping (shutdown requested)");
    return NULL;
}
