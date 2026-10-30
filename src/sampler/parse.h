#ifndef PLANT_PARSE_H
#define PLANT_PARSE_H

#include <stddef.h>
#include <stdint.h>

struct reading {
    int32_t soil;    /* 0..1023 */
    int32_t dist_mm; /* valid only if !no_echo */
    int no_echo;     /* Uno sent DIST,-1.0 */
};

/* Strictly parses one Uno line, "SOIL,<0-1023>,DIST,<-1.0 | N.D>", with any trailing \r\n
 * already stripped. Returns 0 on success, -1 on anything else (out is then unspecified). */
int parse_line(const char *s, size_t len, struct reading *out);

#endif
