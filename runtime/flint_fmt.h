// flint_fmt.h — printf-style string formatting.
//
// Flint had no format function at all, so every program that wanted
// "x=%d" concatenated with flint_i64_to_string. This is the single most-used
// piece of stdlib ergonomics that was missing.
//
// Supported conversions:
//   %d %i   int64_t          %u    unsigned
//   %f %F   double            %e %E scientific
//   %g %G   shortest of %f/%e  %a    hex float
//   %s      const char*       %c    int as a character
//   %p      pointer
//   %%      literal percent
// Flags: - (left), + (sign), space, 0 (zero pad). Width and precision, e.g.
// %8.3f, %-5d, %.2e.  A `*` width/precision reads an int argument.
//
// flint_format allocates the result and returns it; the caller owns it and
// releases it with flint_str_free.  NULL returns NULL.

#ifndef FLINT_FMT_H
#define FLINT_FMT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

char* flint_format(const char* fmt, ...);

// Same, but taking an explicit va_list so wrappers can forward.
struct __va_list_tag;
char* flint_vformat(const char* fmt, void* va);

#ifdef __cplusplus
}
#endif

#endif // FLINT_FMT_H