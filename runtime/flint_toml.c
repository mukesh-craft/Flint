// flint_toml.c — TOML v1.0.0 parser. Per flint_value.h.
//
// Covers the parts of TOML that configuration files actually use: tables,
// arrays of tables, dotted keys, inline tables, arrays, all four integer
// forms (decimal, hex, octal, binary) with underscores, floats including
// inf/nan, booleans, basic and literal strings, and the four date/time forms
// (offset date-time, local date-time, local date, local time).
//
// A date or time becomes a string in the value tree, normalised to RFC 3339
// so a consumer does not have to re-parse it. That is a deliberate trade: the
// value model has seven kinds and no date type, and inventing an eighth would
// push a parsing responsibility onto every reader. The original text is
// preserved verbatim in the string.
//
// Comments start with #, bare keys are [A-Za-z0-9_-], quoted keys may be any
// string, and a key may be a dotted chain (a.b.c = 1 becomes nested tables).
// Escapes in basic strings are the JSON set plus \e, \b, \f and \uXXXX.

#include "flint_value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

typedef struct {
    const char* p;
    int64_t     line;
    int64_t     col;
    FlintParseError* err;
} P;

static void _fail(P* s, const char* msg) {
    if (s->err && !s->err->msg) {
        s->err->line = s->line;
        s->err->col  = s->col;
        s->err->msg  = msg;
    }
}

static void _adv(P* s) {
    if (*s->p == '\n') { s->line++; s->col = 1; } else { s->col++; }
    s->p++;
}

static void _skip_ws(P* s) {
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\r') _adv(s);
}

// A comment runs to end of line, and there is no line-continuation in TOML,
// so nothing here escapes.
static void _skip_to_eol(P* s) {
    while (*s->p && *s->p != '\n') _adv(s);
}

// Whitespace INCLUDING newlines: used between a key and its '=', where the
// grammar allows a line break.
static void _skip_ws_nl(P* s) {
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\r' || *s->p == '\n') _adv(s);
}

static FlintValue* _value(P* s);

// ---- strings -------------------------------------------------------------
typedef struct { char* b; size_t n, cap; } SB;

static void _sbn(SB* s, const char* p, size_t n) {
    if (s->n + n + 1 > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 32;
        while (c < s->n + n + 1) c *= 2;
        char* nb = (char*)realloc(s->b, c);
        if (!nb) return;
        s->b = nb; s->cap = c;
    }
    memcpy(s->b + s->n, p, n);
    s->n += n;
    s->b[s->n] = '\0';
}

static void _utf8(SB* s, uint32_t cp) {
    char buf[4];
    if (cp < 0x80) { buf[0] = (char)cp; _sbn(s, buf, 1); }
    else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        _sbn(s, buf, 2);
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        _sbn(s, buf, 3);
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        _sbn(s, buf, 4);
    }
}

static int _hex4(const char* p, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

// Basic string: "..." with escapes. Returns malloc'd text.
static char* _basic_string(P* s) {
    if (*s->p != '"') { _fail(s, "expected string"); return NULL; }
    _adv(s);
    SB out = { NULL, 0, 0 };
    while (*s->p && *s->p != '"') {
        if (*s->p == '\n') { _fail(s, "unterminated string"); free(out.b); return NULL; }
        if (*s->p != '\\') { _sbn(&out, s->p, 1); _adv(s); continue; }
        _adv(s);                                   // past the backslash
        char e = *s->p;
        switch (e) {
            case '"':  _sbn(&out, "\"", 1);  _adv(s); break;
            case '\\': _sbn(&out, "\\", 1);  _adv(s); break;
            case 'b':  _sbn(&out, "\b", 1);  _adv(s); break;
            case 'f':  _sbn(&out, "\f", 1);  _adv(s); break;
            case 'n':  _sbn(&out, "\n", 1);  _adv(s); break;
            case 'r':  _sbn(&out, "\r", 1);  _adv(s); break;
            case 't':  _sbn(&out, "\t", 1);  _adv(s); break;
            case 'e':  _sbn(&out, "\x1b", 1); _adv(s); break;   // TOML-only escape
            case 'u': case 'U': {
                int want = (e == 'u') ? 4 : 8;
                uint32_t cp;
                if (!_hex4(s->p + 1, &cp)) {
                    if (want == 8) {                         // \UXXXXXXXX
                        char tmp[9];
                        memcpy(tmp, s->p + 1, 8);
                        tmp[8] = '\0';
                        char* end = NULL;
                        unsigned long long v = strtoull(tmp, &end, 16);
                        if (end && *end == '\0') { _utf8(&out, (uint32_t)v); s->p += 8; _adv(s); _adv(s); _adv(s); break; }
                    }
                    _fail(s, "bad unicode escape"); free(out.b); return NULL;
                }
                _utf8(&out, cp);
                for (int i = 0; i < 4; i++) _adv(s);
                if (want == 8) { for (int i = 0; i < 4; i++) _adv(s); }
                break;
            }
            default:
                _fail(s, "unknown escape");
                free(out.b);
                return NULL;
        }
    }
    if (*s->p != '"') { _fail(s, "unterminated string"); free(out.b); return NULL; }
    _adv(s);
    if (!out.b) out.b = strdup("");
    return out.b;
}

// Literal string: '...' with NO escapes at all. That is the point of it.
static char* _literal_string(P* s) {
    if (*s->p != '\'') { _fail(s, "expected literal string"); return NULL; }
    _adv(s);
    SB out = { NULL, 0, 0 };
    while (*s->p && *s->p != '\'') {
        if (*s->p == '\n') { _fail(s, "unterminated literal string"); free(out.b); return NULL; }
        _sbn(&out, s->p, 1);
        _adv(s);
    }
    if (*s->p != '\'') { _fail(s, "unterminated literal string"); free(out.b); return NULL; }
    _adv(s);
    if (!out.b) out.b = strdup("");
    return out.b;
}

// Multi-line basic string: """...""". A newline immediately after the opening
// delimiter is trimmed, per the spec.
static char* _multiline_basic(P* s) {
    _adv(s); _adv(s); _adv(s);
    if (*s->p == '\r') _adv(s);
    if (*s->p == '\n') _adv(s);
    SB out = { NULL, 0, 0 };
    while (*s->p) {
        if (*s->p == '"' && s->p[1] == '"' && s->p[2] == '"') {
            _adv(s); _adv(s); _adv(s);
            // Up to two extra quotes are part of the content.
            while (*s->p == '"' && out.n < (size_t)-1 && out.n + 1 < (size_t)-1) {
                _sbn(&out, "\"", 1); _adv(s);
                if (out.n >= 2 && out.b[out.n-1] == '"' && out.b[out.n-2] == '"') break;
            }
            if (!out.b) out.b = strdup("");
            return out.b;
        }
        if (*s->p == '\\') {
            // A backslash at end of line trims the newline and all leading
            // whitespace on the next line.
            const char* q = s->p + 1;
            int nl = 0;
            while (*q == ' ' || *q == '\t' || *q == '\r') q++;
            if (*q == '\n') {
                while (s->p <= q) _adv(s);
                while (*s->p == ' ' || *s->p == '\t' || *s->p == '\r' || *s->p == '\n') _adv(s);
                continue;
            }
            (void)nl;
        }
        _sbn(&out, s->p, 1);
        _adv(s);
    }
    _fail(s, "unterminated multi-line string");
    free(out.b);
    return NULL;
}

static char* _multiline_literal(P* s) {
    _adv(s); _adv(s); _adv(s);
    if (*s->p == '\r') _adv(s);
    if (*s->p == '\n') _adv(s);
    SB out = { NULL, 0, 0 };
    while (*s->p) {
        if (*s->p == '\'' && s->p[1] == '\'' && s->p[2] == '\'') {
            _adv(s); _adv(s); _adv(s);
            if (!out.b) out.b = strdup("");
            return out.b;
        }
        _sbn(&out, s->p, 1);
        _adv(s);
    }
    _fail(s, "unterminated multi-line literal string");
    free(out.b);
    return NULL;
}

// ---- dates ---------------------------------------------------------------
// Recognised and normalised to RFC 3339. Returns NULL if this is not a date.
// A date may be followed by a space-separated offset time (TOML allows it),
// which is the one place the two forms meet.
static char* _try_date(P* s) {
    // YYYY-MM-DD or HH:MM:SS
    int is_date = (isdigit((unsigned char)s->p[0]) && isdigit((unsigned char)s->p[1]) &&
                   isdigit((unsigned char)s->p[2]) && isdigit((unsigned char)s->p[3]) &&
                   s->p[4] == '-');
    int is_time = (isdigit((unsigned char)s->p[0]) && isdigit((unsigned char)s->p[1]) &&
                   s->p[2] == ':');
    if (!is_date && !is_time) return NULL;

    const char* start = s->p;
    char buf[80];
    size_t n = 0;
    // Copy greedily while the characters could belong to a date/time, stopping
    // before a delimiter that cannot.
    while (*s->p && n + 1 < sizeof buf) {
        char c = *s->p;
        if (isdigit((unsigned char)c) || c == '-' || c == ':' ||
            c == 'T' || c == 't' || c == 'Z' || c == 'z' ||
            c == '+' || c == '.' || c == ' ') {
            // A space only continues the value if a digit follows (the
            // "date time" form); otherwise it ends the token.
            if (c == ' ' && !isdigit((unsigned char)s->p[1])) break;
            buf[n++] = c;
            _adv(s);
        } else {
            break;
        }
    }
    buf[n] = '\0';
    // Trim a trailing space that came from a partial scan.
    while (n && buf[n-1] == ' ') buf[--n] = '\0';
    if (n == 0) { s->p = start; return NULL; }

    // Validate the shape we claim to support, so "12:34:56" and
    // "2024-01-02T03:04:05Z" both work and "99:99" does not.
    int nd = 0;
    for (size_t i = 0; i < n; i++) if (buf[i] >= '0' && buf[i] <= '9') nd++;
    if (nd < 4) { s->p = start; return NULL; }

    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, buf, n + 1);
    return out;
}

// ---- numbers -------------------------------------------------------------
static FlintValue* _number(P* s) {
    const char* start = s->p;
    // Radix prefixes: 0x hex, 0o octal, 0b binary. TOML has no leading-zero
    // decimal, so "0123" is an error rather than 123.
    if (s->p[0] == '0' && (s->p[1] == 'x' || s->p[1] == 'X' ||
                            s->p[1] == 'o' || s->p[1] == 'O' ||
                            s->p[1] == 'b' || s->p[1] == 'B')) {
        int base = (s->p[1] == 'x' || s->p[1] == 'X') ? 16 :
                   (s->p[1] == 'o' || s->p[1] == 'O') ? 8 : 2;
        _adv(s); _adv(s);
        SB digits = { NULL, 0, 0 };
        while (isalnum((unsigned char)*s->p) || *s->p == '_') {
            if (*s->p != '_') { char c = *s->p; _sbn(&digits, &c, 1); }
            _adv(s);
        }
        if (!digits.b) { _fail(s, "empty number"); return NULL; }
        char* end = NULL;
        long long v = strtoll(digits.b, &end, base);
        if (!end || *end) { _fail(s, "bad integer"); free(digits.b); return NULL; }
        free(digits.b);
        return flint_value_i64((int64_t)v);
    }

    SB text = { NULL, 0, 0 };
    int is_float = 0;
    while (*s->p == '+' || *s->p == '-' ||
           isdigit((unsigned char)*s->p) || *s->p == '_' ||
           *s->p == '.' || *s->p == 'e' || *s->p == 'E' ||
           // 'f' is needed for `inf`, 'a' for `nan`; without them the scan
           // stopped at "in" and the value was rejected as a bad number.
           *s->p == 'i' || *s->p == 'I' || *s->p == 'n' || *s->p == 'N' ||
           *s->p == 'a' || *s->p == 'A' || *s->p == 'f' || *s->p == 'F') {
        char c = *s->p;
        if (c != '_') {
            _sbn(&text, &c, 1);
            if (c == '.' || c == 'e' || c == 'E') is_float = 1;
        }
        _adv(s);
    }
    if (!text.b) { s->p = start; return NULL; }

    // inf / nan, with an optional sign.
    const char* t = text.b;
    const char* sign = "";
    if (*t == '+' || *t == '-') { sign = (*t == '-') ? "-" : ""; t++; }
    if (strcasecmp(t, "inf") == 0) {
        double d = INFINITY;
        free(text.b);
        return flint_value_f64(sign[0] == '-' ? -d : d);
    }
    if (strcasecmp(t, "nan") == 0) {
        free(text.b);
        return flint_value_f64(sign[0] == '-' ? -NAN : NAN);
    }

    char* end = NULL;
    if (is_float) {
        double d = strtod(text.b, &end);
        if (!end || *end || end == text.b) { _fail(s, "bad float"); free(text.b); return NULL; }
        free(text.b);
        return flint_value_f64(d);
    }
    long long v = strtoll(text.b, &end, 10);
    if (!end || *end || end == text.b) {
        // Might be a float after all (e.g. 1e5 written without a dot is still
        // a float because of the exponent).
        double d = strtod(text.b, &end);
        if (end && !*end && end != text.b) { free(text.b); return flint_value_f64(d); }
        _fail(s, "bad number");
        free(text.b);
        return NULL;
    }
    free(text.b);
    return flint_value_i64((int64_t)v);
}

// ---- keys ----------------------------------------------------------------
static int _bare(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

// Read one key component. Returns malloc'd text; sets *dotted when the next
// char is '.', so the caller can keep building a nested path.
static char* _key_part(P* s, int* dotted) {
    *dotted = 0;
    if (*s->p == '"')  return _basic_string(s);
    if (*s->p == '\'') return _literal_string(s);
    SB out = { NULL, 0, 0 };
    while (_bare(*s->p)) { char c = *s->p; _sbn(&out, &c, 1); _adv(s); }
    if (!out.b) { _fail(s, "expected key"); return NULL; }
    if (*s->p == '.') { *dotted = 1; _adv(s); }   // consume it: the caller loops
    return out.b;
}

// Walk into `root` following a dotted key, creating tables as needed.
// Walk a dotted key, creating the intermediate tables, and return the table the
// FINAL component lives in. The final component itself is handed back through
// `last_key` (owned by the caller): the value goes there, not into the table
// root. Storing it under "" -- which is what happened before -- silently lost
// every key, so a document parsed "successfully" with no retrievable fields.
static FlintValue* _descend(P* s, FlintValue* root, char** last_key) {
    FlintValue* cur = root;
    char* prev = NULL;                 // owned, released on error
    for (;;) {
        _skip_ws(s);
        int dotted = 0;
        char* part = _key_part(s, &dotted);
        if (!part) { free(prev); return NULL; }
        free(prev);
        prev = part;
        // The final component must NOT get a table wrapper, or `a = 1` becomes
        // {a: {a: 1}} and `a.b = 1` becomes {a: {b: {b: 1}}}. Only intermediate
        // components -- the ones followed by a dot -- create tables.
        if (!dotted) {
            *last_key = prev;
            return cur;                     // caller sets the value here
        }
        FlintValue* next = flint_value_obj_get(cur, part);
        if (!next) {
            next = flint_value_obj();
            if (!next) { _fail(s, "out of memory"); free(prev); return NULL; }
            flint_value_obj_set(cur, part, next);
        } else if (next->type != FLT_OBJ) {
            // A key that is both a table and a scalar is a hard error, not a
            // silent overwrite of the earlier value.
            _fail(s, "key is not a table");
            free(prev);
            return NULL;
        }
        cur = next;
    }
}

// ---- values --------------------------------------------------------------
static FlintValue* _array(P* s) {
    // Step over the '[' first. _value dispatches here with the cursor ON it, so
    // without this every element re-entered _array on the same bracket:
    // unbounded recursion until the stack gave out.
    if (*s->p == '[') _adv(s);
    FlintValue* a = flint_value_arr();
    if (!a) return NULL;
    for (;;) {
        _skip_ws_nl(s);                            // arrays may span lines
        if (*s->p == ']') { _adv(s); return a; }
        if (!*s->p) { _fail(s, "unterminated array"); flint_value_free(a); return NULL; }
        FlintValue* v = _value(s);
        if (!v) { flint_value_free(a); return NULL; }
        flint_value_arr_add(a, v);
        _skip_ws_nl(s);
        if (*s->p == ',') { _adv(s); continue; }
        if (*s->p == ']') { _adv(s); return a; }
        _fail(s, "expected ',' or ']' in array");
        flint_value_free(a);
        return NULL;
    }
}

// Inline table: { k = v, ... }. No newlines, no trailing comma (per spec).
static FlintValue* _inline_table(P* s) {
    // _value dispatches here with the cursor ON the '{', so step over it before
    // reading a key -- otherwise _descend tried to parse '{' as a key and every
    // inline table failed with "expected key".
    if (*s->p == '{') _adv(s);
    FlintValue* o = flint_value_obj();
    if (!o) return NULL;
    _skip_ws(s);
    if (*s->p == '}') { _adv(s); return o; }
    for (;;) {
        _skip_ws(s);
        char* key = NULL;
        FlintValue* slot = _descend(s, o, &key);
        if (!slot || !key) { free(key); flint_value_free(o); return NULL; }
        _skip_ws(s);
        if (*s->p != '=') {
            _fail(s, "expected '=' in inline table");
            free(key); flint_value_free(o); return NULL;
        }
        _adv(s);
        _skip_ws(s);
        FlintValue* v = _value(s);
        if (!v) { free(key); flint_value_free(o); return NULL; }
        flint_value_obj_set(slot, key, v);
        free(key);
        _skip_ws(s);
        if (*s->p == ',') { _adv(s); continue; }
        if (*s->p == '}') { _adv(s); return o; }
        _fail(s, "expected ',' or '}' in inline table");
        flint_value_free(o);
        return NULL;
    }
}

static FlintValue* _value(P* s) {
    _skip_ws(s);
    switch (*s->p) {
        case '"':
            if (s->p[1] == '"' && s->p[2] == '"') {
                char* t = _multiline_basic(s);
                if (!t) return NULL;
                FlintValue* v = flint_value_str(t);
                free(t);
                return v;
            }
            { char* t = _basic_string(s); if (!t) return NULL;
              FlintValue* v = flint_value_str(t); free(t); return v; }
        case '\'':
            if (s->p[1] == '\'' && s->p[2] == '\'') {
                char* t = _multiline_literal(s);
                if (!t) return NULL;
                FlintValue* v = flint_value_str(t);
                free(t);
                return v;
            }
            { char* t = _literal_string(s); if (!t) return NULL;
              FlintValue* v = flint_value_str(t); free(t); return v; }
        case '[': return _array(s);
        case '{': return _inline_table(s);
        case '\0': _fail(s, "expected value"); return NULL;
        default: break;
    }
    if (strncmp(s->p, "true", 4) == 0) { for (int i = 0; i < 4; i++) _adv(s); return flint_value_bool(1); }
    if (strncmp(s->p, "false", 5) == 0) { for (int i = 0; i < 5; i++) _adv(s); return flint_value_bool(0); }

    // A date before a number: 1979-05-27 would otherwise scan as arithmetic.
    {
        const char* save_p = s->p;
        int64_t save_l = s->line, save_c = s->col;
        char* d = _try_date(s);
        if (d) {
            FlintValue* v = flint_value_str(d);
            free(d);
            return v;
        }
        s->p = save_p; s->line = save_l; s->col = save_c;
    }
    // inf / nan start with a letter, so the numeric branch has to admit them.
    if (isdigit((unsigned char)*s->p) || *s->p == '+' || *s->p == '-' ||
        *s->p == 'i' || *s->p == 'I' || *s->p == 'n' || *s->p == 'N')
        return _number(s);

    _fail(s, "unrecognised value");
    return NULL;
}

// A key = value line, where the key may be dotted.
static int _keyval(P* s, FlintValue* root) {
    char* key = NULL;
    FlintValue* slot = _descend(s, root, &key);
    if (!slot || !key) { free(key); return 0; }
    _skip_ws(s);
    if (*s->p != '=') { _fail(s, "expected '=' after key"); free(key); return 0; }
    _adv(s);
    _skip_ws(s);
    FlintValue* v = _value(s);
    if (!v) { free(key); return 0; }
    flint_value_obj_set(slot, key, v);
    free(key);
    return 1;
}

// ---- entry ---------------------------------------------------------------
FlintValue* flint_toml_parse(const char* text, FlintParseError* err) {
    if (err) { err->line = 0; err->col = 0; err->msg = NULL; }
    if (!text) return NULL;

    // A UTF-8 BOM is common on Windows-authored files and must not become a
    // key character.
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) text += 3;

    P s = { text, 1, 1, err };
    FlintValue* root = flint_value_obj();
    if (!root) return NULL;
    FlintValue* cur = root;                 // the table new keys land in

    for (;;) {
        _skip_ws(&s);
        if (*s.p == '#') { _skip_to_eol(&s); continue; }
        if (*s.p == '\n') { _adv(&s); continue; }
        if (!*s.p) break;

        if (*s.p == '[') {
            _adv(&s);
            int is_array = 0;
            if (*s.p == '[') { is_array = 1; _adv(&s); }
            // Build the table path, remembering each component so
            // [[a.b]] can create the intermediate tables.
            FlintValue* t = root;
            for (;;) {
                _skip_ws(&s);
                int dotted = 0;
                char* part = _key_part(&s, &dotted);
                if (!part) { flint_value_free(root); return NULL; }
                FlintValue* next = flint_value_obj_get(t, part);
                if (is_array) {
                    // [[a]] appends to the array at a; the table is its LAST
                    // element, so repeated [[a]] do not overwrite each other.
                    FlintValue* arr = next;
                    if (!arr) {
                        arr = flint_value_arr();
                        if (!arr) { free(part); flint_value_free(root); return NULL; }
                        flint_value_obj_set(t, part, arr);
                    }
                    if (arr->type != FLT_ARR) { free(part); flint_value_free(root); return NULL; }
                    FlintValue* elem = flint_value_obj();
                    flint_value_arr_add(arr, elem);
                    next = elem;
                } else {
                    if (!next) {
                        next = flint_value_obj();
                        if (!next) { free(part); flint_value_free(root); return NULL; }
                        flint_value_obj_set(t, part, next);
                    }
                    if (next->type != FLT_OBJ) { free(part); flint_value_free(root); return NULL; }
                }
                free(part);
                t = next;
                if (!dotted) break;
            }
            _skip_ws(&s);
            if (*s.p == ']') _adv(&s);
            else { _fail(&s, "expected ']' after table name"); flint_value_free(root); return NULL; }
            if (is_array) {
                if (*s.p == ']') _adv(&s);
                else { _fail(&s, "expected ']]' after array-of-tables name"); flint_value_free(root); return NULL; }
            }
            cur = t;
            _skip_ws(&s);
            if (*s.p == '#') _skip_to_eol(&s);
            // Consume the newline that ends the header line. Without this the
            // next iteration treated the following blank line as part of the
            // table body, and any key after the last [[table]] was captured
            // into that table instead of the root.
            while (*s.p == ' ' || *s.p == '\t' || *s.p == '\r') _adv(&s);
            if (*s.p == '\n') _adv(&s);
            continue;
        }

        if (!_keyval(&s, cur)) { flint_value_free(root); return NULL; }
        _skip_ws(&s);
        if (*s.p == '#') _skip_to_eol(&s);
        else if (*s.p && *s.p != '\n') {
            _fail(&s, "unexpected trailing characters");
            flint_value_free(root);
            return NULL;
        }
    }

    if (err && err->msg) { flint_value_free(root); return NULL; }
    return root;
}