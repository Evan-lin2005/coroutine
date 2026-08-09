#ifndef P0_DEBUG_LOG_H
#define P0_DEBUG_LOG_H

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <time.h>

/* Debug-mode NDJSON logger — external tests only; library untouched. */
static inline void p0_log(const char *hypothesis_id, const char *location,
                          const char *message, const char *data_json)
{
    FILE *f = fopen("debug-193e44.log", "a");
    if (!f)
        return;
    // #region agent log
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long ms = (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    fprintf(f,
            "{\"sessionId\":\"193e44\",\"hypothesisId\":\"%s\","
            "\"location\":\"%s\",\"message\":\"%s\",\"data\":%s,"
            "\"timestamp\":%lld,\"runId\":\"p0\"}\n",
            hypothesis_id, location, message,
            data_json ? data_json : "{}", ms);
    // #endregion
    fclose(f);
}

#endif /* P0_DEBUG_LOG_H */
