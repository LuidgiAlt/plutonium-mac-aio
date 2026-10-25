/* Result convention for every test program (tests/run.sh relies on it):
 * exit 0 PASS, 1 FAIL, 77 SKIP, and one line "PASS|FAIL|SKIP <name>: <detail>".
 * TEST_NAME is set by tests/Makefile. */
#include <stdarg.h>
#include <stdio.h>

#ifndef TEST_NAME
#define TEST_NAME "test"
#endif

static int report(int ok, const char *fmt, ...)
{
    va_list args;
    printf("%s %s: ", ok ? "PASS" : "FAIL", TEST_NAME);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
    return ok ? 0 : 1;
}

static int skip(const char *why)
{
    printf("SKIP %s: %s\n", TEST_NAME, why);
    fflush(stdout);
    return 77;
}

/* Each channel of an X8R8G8B8 pixel within 2 of the expected value. */
static int channel_near(unsigned long px, int shift, unsigned int want)
{
    int got = (int)((px >> shift) & 0xff), d = got - (int)want;
    return d >= -2 && d <= 2;
}
static int rgb_near(unsigned long px, unsigned int r, unsigned int g, unsigned int b)
{
    return channel_near(px, 16, r) && channel_near(px, 8, g) && channel_near(px, 0, b);
}
