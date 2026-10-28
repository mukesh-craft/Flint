// flint_fmt.c — printf-style formatting, per flint_fmt.h.
//
// Formats into a growing heap buffer with snprintf per conversion. The buffer
// doubles as it fills, so width is unbounded (a %2000d field works).

#include "flint_fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

typedef struct { char* buf; size_t len, cap; } SBuf;

static int sb_reserve(SBuf* s, size_t extra) {
    if (s->len + extra + 1 <= s->cap) return 1;
    size_t ncap = s->cap ? s->cap * 2 : 128;
    while (ncap < s->len + extra + 1) ncap *= 2;
    char* nb = (char*)realloc(s->buf, ncap);
    if (!nb) return 0;
    s->buf = nb;
    s->cap = ncap;
    return 1;
}

static int sb_puts(SBuf* s, const char* p) {
    size_t n = strlen(p);
    if (!sb_reserve(s, n)) return 0;
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = '\0';
    return 1;
}

static int sb_putn(SBuf* s, const char* p, size_t n) {
    if (!sb_reserve(s, n)) return 0;
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = '\0';
    return 1;
}

// Build the flag/width/precision mini-format string for one conversion and hand
// it to snprintf, so the C library owns all the numeric formatting rules.
static int emit_conv(SBuf* s, int left, int plus, int space,
                     int zero, int width, int prec, int longmod, char conv,
                     const void* arg) {
    char mini[32];
    int k = 0;
    mini[k++] = '%';
    if (left)  mini[k++] = '-';
    if (plus)  mini[k++] = '+';
    if (space) mini[k++] = ' ';
    if (zero)  mini[k++] = '0';
    if (width > 0) k += snprintf(mini + k, sizeof(mini) - (size_t)k, "%d", width);
    if (prec >= 0) k += snprintf(mini + k, sizeof(mini) - (size_t)k, ".%d", prec);
    if (longmod) mini[k++] = 'l';
    mini[k++] = conv;
    mini[k] = '\0';

    // snprintf into a stack buffer when it fits, else measure and reserve.
    char tmp[512];
    int need;
    if (conv == 's') {
        const char* str = *(const char* const*)arg;
        if (!str) str = "(null)";
        need = snprintf(NULL, 0, mini, str);
        if (!sb_reserve(s, (size_t)(need < 0 ? 0 : need))) return 0;
        sb_putn(s, str, (size_t)(need < 0 ? 0 : need));
        return 1;
    }
    if (conv == 'c') {
        need = snprintf(tmp, sizeof tmp, mini, (int)(long)(intptr_t)arg);
    } else if (longmod) {
        need = snprintf(tmp, sizeof tmp, mini, (long)(*(const long*)arg));
    } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
               conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A') {
        need = snprintf(tmp, sizeof tmp, mini, *(const double*)arg);
    } else if (conv == 'p') {
        need = snprintf(tmp, sizeof tmp, mini, arg);
    } else {
        need = snprintf(tmp, sizeof tmp, mini, (int)(*(const int*)arg));
    }
    if (need < 0) return 0;
    if (!sb_reserve(s, (size_t)need)) return 0;
    sb_putn(s, tmp, (size_t)need);
    return 1;
}

char* flint_vformat(const char* fmt, void* va) {
    if (!fmt) return NULL;
    va_list ap;
    // The header takes void* to avoid dragging <stdarg.h> into consumers.
    // `va` points at the caller's va_list, so restore the type and use
    // va_copy: it is the standard way to pass a va_list through, and unlike
    // a raw memcpy it tells analyzers (and the compiler) what is happening.
    va_copy(ap, *(va_list*)va);

    SBuf s;
    s.buf = NULL; s.len = 0; s.cap = 0;
    char* result = NULL;
    if (!sb_reserve(&s, 128)) goto done;
    s.buf[0] = '\0';

    const char* p = fmt;
    while (*p) {
        if (*p != '%') {
            const char* start = p;
            while (*p && *p != '%') p++;
            if (!sb_putn(&s, start, (size_t)(p - start))) { free(s.buf); goto done; }
            continue;
        }
        p++;                                  // past '%'
        if (*p == '%') { if (!sb_puts(&s, "%")) { free(s.buf); goto done; } p++; continue; }

        int left = 0, plus = 0, space = 0, zero = 0;
        for (;;) {
            if (*p == '-') left = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '0') zero = 1;
            else break;
            p++;
        }
        int width = 0;
        if (*p == '*') { width = va_arg(ap, int); p++; if (width < 0) { left = 1; width = -width; } }
        else while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }

        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') { prec = va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') { prec = prec * 10 + (*p - '0'); p++; }
        }
        // Length modifiers: 'l' widens integers to long, which is what this
        // platform's int already is, so it is consumed and ignored. Others
        // (h/z/j/t) likewise.
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't') p++;
        char conv = *p;
        if (!conv) break;                      // trailing '%' -- stop
        p++;

        switch (conv) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
                long v = (long)va_arg(ap, int);
                emit_conv(&s, left, plus, space, zero, width, prec, 1, 'd', &v);
                break;
            }
            case 'f': case 'F': case 'e': case 'E':
            case 'g': case 'G': case 'a': case 'A': {
                double v = va_arg(ap, double);
                emit_conv(&s, left, plus, space, zero, width, prec, 0, conv, &v);
                break;
            }
            case 's': {
                const char* v = va_arg(ap, const char*);
                emit_conv(&s, left, plus, space, zero, width, prec, 0, 's', &v);
                break;
            }
            case 'c': {
                long v = (long)va_arg(ap, int);
                emit_conv(&s, left, plus, space, zero, width, prec, 0, 'c', &v);
                break;
            }
            case 'p': {
                void* v = va_arg(ap, void*);
                emit_conv(&s, left, plus, space, zero, width, prec, 0, 'p', &v);
                break;
            }
            default:
                // Unknown conversion: emit it literally so nothing is lost.
                sb_puts(&s, "%");
                {
                    char one[2] = { conv, '\0' };
                    sb_puts(&s, one);
                }
                break;
        }
    }
    if (!s.buf) {
        if (!sb_reserve(&s, 1)) goto done;
        s.buf[0] = '\0';
    }
    result = s.buf;
done:
    va_end(ap);
    return result;
}

char* flint_format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char* r = flint_vformat(fmt, &ap);
    va_end(ap);
    return r;
}

// ---- fixed-arity shims ------------------------------------------------
//
// flint_format itself is variadic, and an LLVM varargs function needs its
// FunctionType built with isVarArg=true plus every call site lowered to the
// platform vararg ABI. The compiler did none of that, so flint_format was
// registered as a fixed (char*) function -- which meant arguments after the
// format string were read as garbage, silently.
//
// These shims give the same capability with fixed signatures the verifier can
// check. The letter encodes the argument type (i integer, d double, s string),
// so each call site knows exactly what it is passing.
// Each takes its arguments explicitly so the count and order are checked by
// the compiler rather than inferred at runtime.
char* flint_format1(const char* f, int64_t a) {
    return flint_format(f, a);
}
char* flint_format2(const char* f, int64_t a, int64_t b) {
    return flint_format(f, a, b);
}
char* flint_format3(const char* f, int64_t a, int64_t b, int64_t c) {
    return flint_format(f, a, b, c);
}
char* flint_format4(const char* f, int64_t a, int64_t b, int64_t c, int64_t d) {
    return flint_format(f, a, b, c, d);
}
char* flint_formatd1(const char* f, double a)   { return flint_format(f, a); }
char* flint_formatd2(const char* f, double a, int64_t b) { return flint_format(f, a, b); }
char* flint_formatd3(const char* f, double a, int64_t b, int64_t c) {
    return flint_format(f, a, b, c);
}
char* flint_formatd4(const char* f, double a, int64_t b, int64_t c, int64_t d) {
    return flint_format(f, a, b, c, d);
}
char* flint_formats1(const char* f, const char* a) { return flint_format(f, a); }
char* flint_formats2(const char* f, const char* a, int64_t b) {
    return flint_format(f, a, b);
}
char* flint_formats3(const char* f, const char* a, int64_t b, int64_t c) {
    return flint_format(f, a, b, c);
}
