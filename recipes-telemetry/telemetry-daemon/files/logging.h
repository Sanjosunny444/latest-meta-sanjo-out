#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>

// Shared across every thread — one log file, one FILE*, opened once by
// main() before any thread starts. log_msg() builds the full line (with
// timestamp and newline) in one buffer and writes it with a SINGLE fputs()
// call — that single-call property is what makes it safe across threads,
// via stdio's per-FILE* locking. An earlier version of this function issued
// three separate stdio calls per line (timestamp/message/newline) and
// assumed that was equally safe — it wasn't: confirmed on-device, two
// threads' log lines actually got mashed together mid-line with no
// separating newline. Keep log_msg() building one buffer + one write call;
// don't split it back into multiple stdio calls per line.
extern FILE *g_log;

void log_msg(const char *fmt, ...);

#endif // LOGGING_H
