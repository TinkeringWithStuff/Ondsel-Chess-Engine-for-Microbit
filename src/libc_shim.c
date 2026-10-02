// The engine core (board.c) uses exactly two libc functions: memset (to
// zero a Board struct) and strchr (to look up a FEN piece character). With
// -nostdlib there's no libc to link against, so these are trivial hand
// implementations rather than anything performance-critical -- board_clear
// and FEN parsing both run once, never in perft's hot path.
#include <stddef.h>

void *memset(void *dst, int val, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    unsigned char v = (unsigned char)val;
    while (n--) {
        *p++ = v;
    }
    return dst;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) {
            return (char *)s;
        }
        s++;
    }
    return (ch == '\0') ? (char *)s : (char *)0;
}
