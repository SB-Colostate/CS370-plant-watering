/* Step 5: strict parsing of Uno lines. Inputs are fixtures, not hardware readings. */
#include <string.h>

#include "sampler/parse.h"
#include "unit.h"

static int p(const char *s, struct reading *r) { return parse_line(s, strlen(s), r); }

static void test_good_lines(void)
{
    struct reading r;
    CHECK(p("SOIL,412,DIST,23.4", &r) == 0 && r.soil == 412 && r.dist_mm == 234 && !r.no_echo);
    CHECK(p("SOIL,0,DIST,0.0", &r) == 0 && r.soil == 0 && r.dist_mm == 0);
    CHECK(p("SOIL,1023,DIST,517.2", &r) == 0 && r.soil == 1023 && r.dist_mm == 5172);
}

static void test_no_echo(void)
{
    struct reading r;
    CHECK(p("SOIL,5,DIST,-1.0", &r) == 0 && r.no_echo && r.soil == 5);
    CHECK(p("SOIL,5,DIST,-1.00", &r) == -1);
    CHECK(p("SOIL,5,DIST,-2.0", &r) == -1);
}

static void test_rejects(void)
{
    struct reading r;
    const char *bad[] = {
        "", "SOIL", "SOIL,", "SOIL,1024,DIST,1.0", "SOIL,-1,DIST,1.0", "SOIL,12a,DIST,1.0",
        "SOIL,12,DIST,", "SOIL,12,DIST,1", "SOIL,12,DIST,1.", "SOIL,12,DIST,1.23",
        "SOIL,12,DIST,1.0 ", " SOIL,12,DIST,1.0", "SOIL,12,DIST,nan", "SOIL,12,DIST,inf",
        "OIL,12,DIST,1.0",   /* tail of a line cut by the Uno reset */
        "SOIL,12,DIST,1.0SOIL,13,DIST,2.0", "SOIL,00001,DIST,1.0", "SOIL,12,DIST,12345.0",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = p(bad[i], &r);
        CHECK(rc == -1);
        if (rc != -1)
            fprintf(stderr, "    accepted: \"%s\"\n", bad[i]);
    }
}

static void test_length_bounded(void)
{
    /* Only len bytes are read: a valid prefix followed by junk must use len. */
    struct reading r;
    const char *s = "SOIL,7,DIST,3.0XXXX";
    CHECK(parse_line(s, 15, &r) == 0 && r.soil == 7 && r.dist_mm == 30);
}

int main(void)
{
    RUN(test_good_lines);
    RUN(test_no_echo);
    RUN(test_rejects);
    RUN(test_length_bounded);
    return unit_done("test_parse");
}
