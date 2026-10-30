#include "sampler/parse.h"

#include <string.h>

/* Consumes 1..maxdigits decimal digits. */
static int digits(const char **p, const char *end, int maxdigits, long *out)
{
    long v = 0;
    int n = 0;
    while (*p < end && **p >= '0' && **p <= '9') {
        if (++n > maxdigits)
            return -1;
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    if (n == 0)
        return -1;
    *out = v;
    return 0;
}

static int literal(const char **p, const char *end, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(end - *p) < n || memcmp(*p, lit, n) != 0)
        return -1;
    *p += n;
    return 0;
}

int parse_line(const char *s, size_t len, struct reading *out)
{
    const char *p = s, *end = s + len;
    long soil, whole, tenth;

    if (literal(&p, end, "SOIL,") < 0 || digits(&p, end, 4, &soil) < 0 || soil > 1023)
        return -1;
    if (literal(&p, end, ",DIST,") < 0)
        return -1;
    out->soil = (int32_t)soil;

    if (literal(&p, end, "-1.0") == 0) {
        if (p != end)
            return -1;
        out->no_echo = 1;
        out->dist_mm = 0;
        return 0;
    }
    /* pulseIn's 30 ms timeout caps real readings near 517 cm; allow up to 9999.9. */
    if (digits(&p, end, 4, &whole) < 0 || literal(&p, end, ".") < 0 ||
        digits(&p, end, 1, &tenth) < 0 || p != end)
        return -1;
    out->no_echo = 0;
    out->dist_mm = (int32_t)(whole * 10 + tenth);
    return 0;
}
