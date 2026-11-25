/*
 * The few C library functions LVGL, FreeRTOS and the compiler need. Byte-wise
 * and therefore alignment-safe: with the MMU off every access is strongly
 * ordered and must be aligned.
 */
#include <stddef.h>

#define NO_LOOP_TO_CALL __attribute__((optimize("no-tree-loop-distribute-patterns")))

NO_LOOP_TO_CALL void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

NO_LOOP_TO_CALL void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        while (n--) {
            d[n] = s[n];
        }
    }
    return dst;
}

NO_LOOP_TO_CALL void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;

    for (; n; n--, x++, y++) {
        if (*x != *y) {
            return *x - *y;
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}
