#include "tz_apply.h"

#include <stdlib.h>
#include <time.h>

const char *sync_tz_apply(const char *posix_tz, const char *fallback_tz) {
    const char *chosen = NULL;
    if (posix_tz && posix_tz[0] != '\0') {
        chosen = posix_tz;
    } else if (fallback_tz && fallback_tz[0] != '\0') {
        chosen = fallback_tz;
    } else {
        return NULL;
    }

    setenv("TZ", chosen, 1);
    tzset();
    return chosen;
}
