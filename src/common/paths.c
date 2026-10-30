#include "common/paths.h"

#include <stdio.h>
#include <stdlib.h>

#include "common/config.h"

int default_data_dir(char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return -1;
    int len = snprintf(out, n, "%s/%s", home, CFG_DATA_DIR_DEFAULT);
    return (len < 0 || (size_t)len >= n) ? -1 : 0;
}
