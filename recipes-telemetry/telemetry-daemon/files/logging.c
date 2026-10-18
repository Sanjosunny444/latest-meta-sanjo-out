#include "logging.h"
#include <stdarg.h>
#include <time.h>

FILE *g_log = NULL;

void log_msg(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

    // Build the ENTIRE line (timestamp + message + newline) in one buffer
    // first, then write it with a single fputs() call. This matters because
    // multiple threads call log_msg() concurrently — stdio's per-FILE* lock
    // makes any ONE call atomic, but log_msg() used to make three separate
    // fprintf() calls per line (timestamp, message, newline), and another
    // thread's log_msg() could interleave in the gaps between those three
    // calls. Confirmed this happening for real on-device (two threads'
    // lines got mashed onto one line, no newline between them) before this
    // fix — a single fputs() call is what actually gets the atomicity the
    // old three-call version only claimed to have.
    char line[512];
    int prefix_len = snprintf(line, sizeof(line), "[%s] ", ts);

    va_list ap;
    va_start(ap, fmt);
    int msg_len = vsnprintf(line + prefix_len, sizeof(line) - prefix_len, fmt, ap);
    va_end(ap);

    int pos = prefix_len + (msg_len > 0 ? msg_len : 0);
    if (pos > (int)sizeof(line) - 2) pos = (int)sizeof(line) - 2;  // truncated, still room for \n
    line[pos]     = '\n';
    line[pos + 1] = '\0';

    if (g_log) {
        fputs(line, g_log);
        fflush(g_log);
    }
}
