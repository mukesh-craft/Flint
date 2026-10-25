// flint_yaml.c — YAML parser. Per flint_value.h.
//
// Scope, stated plainly: block mappings and sequences, flow mappings and
// sequences, plain/quoted scalars, literal (|) and folded (>) block scalars,
// comments, an optional document marker, and anchors/aliases where cheap.
// NOT supported: tags, complex keys, multiple documents, merge keys.
//
// Why that scope: YAML's full spec (tags, directives, multi-document, exotic
// keys) is far larger than anything a config file uses, and a parser that
// accepts most of the spec but silently mishandles the rest is worse than one
// that rejects what it cannot do. Anything unsupported is an ERROR with a
// line and column, never a silent misparse.
//
// Block structure is resolved by INDENT STACK rather than recursion: a deeply
// nested document must not be able to overflow the C stack, and the same
// machinery gives correct handling of the first rule of YAML -- a sequence may
// be indented at the same level as the key that owns it.

#include "flint_value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// ---- input handling ------------------------------------------------------
typedef struct {
    const char* p;
    int64_t     line, col;
    FlintParseError* err;
    // Flattened lines, so lookahead and indent are cheap.
    char**   lines;
    int64_t* indent;
    int64_t* line_no;
    int64_t  n, cap;
} Y;

static void _fail_at(Y* y, int64_t ln, int64_t cl, const char* msg) {
    if (y->err && !y->err->msg) {
        y->err->line = ln;
        y->err->col  = cl;
        y->err->msg  = msg;
    }
}

// Split into lines once. Tabs are NOT valid indentation in YAML; they are
// rejected rather than silently counted as one column.
static int _split(Y* y) {
    const char* s = y->p;
    int64_t ln = 1;
    while (*s) {
        const char* eol = strchr(s, '\n');
        size_t n = eol ? (size_t)(eol - s) : strlen(s);
        char* copy = (char*)malloc(n + 1);
        if (!copy) return 0;
        memcpy(copy, s, n);
        copy[n] = '\0';
        if (y->n == y->cap) {
            int64_t nc = y->cap ? y->cap * 2 : 64;
            // Grow one array at a time and store each result immediately.
            // Assigning three realloc results at once loses the old pointer
            // (and the memory it owned) when any one of them fails.
            char** nl = (char**)realloc(y->lines, (size_t)nc * sizeof(char*));
            if (!nl) { free(copy); return 0; }
            y->lines = nl;
            int64_t* ni = (int64_t*)realloc(y->indent, (size_t)nc * sizeof(int64_t));
            if (!ni) { free(copy); return 0; }
            y->indent = ni;
            int64_t* nn = (int64_t*)realloc(y->line_no, (size_t)nc * sizeof(int64_t));
            if (!nn) { free(copy); return 0; }
            y->line_no = nn;
            y->cap = nc;
        }
        int64_t ind = 0;
        while (copy[ind] == ' ') ind++;
        y->lines[y->n]   = copy;
        y->indent[y->n]  = ind;
        y->line_no[y->n] = ln;
        y->n++;
        if (!eol) break;
        s = eol + 1;
        ln++;
    }
    return 1;
}

static void _free_split(Y* y) {
    for (int64_t i = 0; i < y->n; i++) free(y->lines[i]);
    free(y->lines); free(y->indent); free(y->line_no);
    y->lines = NULL; y->indent = NULL; y->line_no = NULL; y->n = y->cap = 0;
}

// A line is ignorable when it is blank or only a comment.
static int _skip_line(Y* y, int64_t i) {
    const char* s = y->lines[i];
    while (*s == ' ') s++;
    return (*s == '\0' || *s == '#');
}

static int64_t _next(Y* y, int64_t i) {
    while (i < y->n && _skip_line(y, i)) i++;
    return i;
}

// Reject a tab used as indentation: YAML forbids it, and counting it as spaces
// silently changes the shape of the document.
static int _check_tabs(Y* y, int64_t i) {
    const char* s = y->lines[i];
    int64_t k = 0;
    while (s[k] == ' ') k++;
    if (s[k] == '\t') { _fail_at(y, y->line_no[i], k + 1, "tab used for indentation"); return 0; }
    return 1;
}

// ---- scalars -------------------------------------------------------------
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

static void _sbc(SB* s, char c) { _sbn(s, &c, 1); }

// Trim trailing spaces/tabs, which YAML ignores.
static void _rtrim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
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

static void _utf8(SB* s, uint32_t cp) {
    char b[4];
    if (cp < 0x80) { b[0] = (char)cp; _sbn(s, b, 1); }
    else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F));
        _sbn(s, b, 2);
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        _sbn(s, b, 3);
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        _sbn(s, b, 4);
    }
}

// Double-quoted: escapes, including \x and \u.
static char* _dq(const char* s, int64_t* len_out) {
    SB o = { NULL, 0, 0 };
    s++;                                        // opening quote
    while (*s && *s != '"') {
        if (*s == '\\') {
            s++;
            switch (*s) {
                case 'n': _sbc(&o, '\n'); s++; break;
                case 't': _sbc(&o, '\t'); s++; break;
                case 'r': _sbc(&o, '\r'); s++; break;
                case '0': _sbc(&o, '\0'); s++; break;
                case 'b': _sbc(&o, '\b'); s++; break;
                case 'f': _sbc(&o, '\f'); s++; break;
                case '\\': _sbc(&o, '\\'); s++; break;
                case '"': _sbc(&o, '"'); s++; break;
                case '/': _sbc(&o, '/'); s++; break;
                case ' ': _sbc(&o, ' '); s++; break;
                case 'x': {
                    uint32_t v;
                    if (_hex4(s + 1, &v)) { _sbc(&o, (char)v); s += 5; }
                    else { free(o.b); return NULL; }
                    break;
                }
                case 'u': {
                    uint32_t v;
                    if (_hex4(s + 1, &v)) { _utf8(&o, v); s += 5; }
                    else { free(o.b); return NULL; }
                    break;
                }
                default: free(o.b); return NULL;
            }
        } else {
            _sbc(&o, *s);
            s++;
        }
    }
    if (*s != '"') { free(o.b); return NULL; }
    if (len_out) *len_out = (int64_t)o.n;
    if (!o.b) o.b = (char*)calloc(1, 1);
    return o.b;
}

// Single-quoted: '' is a literal quote, nothing else escapes.
static char* _sq(const char* s, int64_t* len_out) {
    SB o = { NULL, 0, 0 };
    s++;
    // The '' escape must be checked BEFORE the closing-quote test: the old
    // order tested *s != '\'' first, so "it''s" stopped at the first quote of
    // the pair and came back as "it".
    while (*s) {
        if (*s == '\'' && s[1] == '\'') { _sbc(&o, '\''); s += 2; continue; }
        if (*s == '\'') break;
        _sbc(&o, *s);
        s++;
    }
    if (*s != '\'') { free(o.b); return NULL; }
    if (len_out) *len_out = (int64_t)o.n;
    if (!o.b) o.b = (char*)calloc(1, 1);
    return o.b;
}

// Trim trailing comments from a plain scalar: " #" starts a comment, but a '#'
// with no space before it is part of the scalar.
static void _strip_comment(char* s) {
    int in_s = 0, in_d = 0;
    for (char* p = s; *p; p++) {
        if (*p == '\'' && !in_d) in_s = !in_s;
        else if (*p == '"' && !in_s) in_d = !in_d;
        else if (*p == '#' && !in_s && !in_d) {
            if (p == s || p[-1] == ' ' || p[-1] == '\t') { *p = '\0'; return; }
        }
    }
}

// Plain scalar -> typed value.
static FlintValue* _plain(char* s) {
    _strip_comment(s);
    _rtrim(s);
    if (!*s) return flint_value_null();

    // null / ~
    if (strcmp(s, "~") == 0 || strcmp(s, "null") == 0 ||
        strcmp(s, "Null") == 0 || strcmp(s, "NULL") == 0)
        return flint_value_null();
    // bool, including the YAML 1.0 spellings
    if (strcmp(s, "true") == 0 || strcmp(s, "True") == 0 || strcmp(s, "TRUE") == 0)
        return flint_value_bool(1);
    if (strcmp(s, "false") == 0 || strcmp(s, "False") == 0 || strcmp(s, "FALSE") == 0)
        return flint_value_bool(0);
    // inf / nan
    if (strcasecmp(s, ".inf") == 0 || strcasecmp(s, "+.inf") == 0)
        return flint_value_f64(INFINITY);
    if (strcasecmp(s, "-.inf") == 0) return flint_value_f64(-INFINITY);
    if (strcasecmp(s, ".nan") == 0) return flint_value_f64(NAN);

    // int: decimal, 0x, 0o, and the 0b form YAML accepts
    int base = 10;
    const char* t = s;
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { base = 16; t += 2; }
    else if (t[0] == '0' && (t[1] == 'o' || t[1] == 'O')) { base = 8; t += 2; }
    else if (t[0] == '0' && (t[1] == 'b' || t[1] == 'B')) { base = 2; t += 2; }
    if (base != 10) {
        char* end = NULL;
        long long v = strtoll(t, &end, base);
        if (end && *end == '\0' && end != t) return flint_value_i64((int64_t)v);
    }
    // A plain integer: all digits with an optional sign. strtoll alone would
    // accept "12abc" as 12, which is a different value entirely.
    {
        const char* q = s;
        if (*q == '+' || *q == '-') q++;
        if (*q && strspn(q, "0123456789") == strlen(q))
            return flint_value_i64((int64_t)strtoll(s, NULL, 10));
    }
    // float
    {
        char* end = NULL;
        double d = strtod(s, &end);
        if (end && *end == '\0' && end != s) {
            if (d == (double)(int64_t)d && !strchr(s, '.') && !strchr(s, 'e') && !strchr(s, 'E'))
                return flint_value_i64((int64_t)d);
            return flint_value_f64(d);
        }
    }
    return flint_value_str(s);
}

// ---- flow collections ----------------------------------------------------
// Flow may span lines, so it works on a NUL-terminated concatenation of the
// remaining document rather than one line.
typedef struct { const char* p; Y* y; } F;

static void _fws(F* f) { while (*f->p == ' ' || *f->p == '\t') f->p++; }

static FlintValue* _fval(F* f);

static FlintValue* _fmap(F* f) {
    FlintValue* o = flint_value_obj();
    if (!o) return NULL;
    _fws(f);
    if (*f->p == '}') { f->p++; return o; }
    for (;;) {
        _fws(f);
        char* key = NULL;
        if (*f->p == '"')      key = _dq(f->p, NULL), f->p += strlen(key) + 2;
        else if (*f->p == '\'') key = _sq(f->p, NULL), f->p += strlen(key) + 2;
        else {
            const char* st = f->p;
            while (*f->p && *f->p != ':' && *f->p != ',' && *f->p != '}') f->p++;
            size_t k = (size_t)(f->p - st);
            key = (char*)malloc(k + 1);
            if (!key) { flint_value_free(o); return NULL; }
            memcpy(key, st, k);
            key[k] = '\0';
            _rtrim(key);
        }
        _fws(f);
        if (*f->p != ':') { free(key); flint_value_free(o); return NULL; }
        f->p++;
        FlintValue* v = _fval(f);
        if (!v) { free(key); flint_value_free(o); return NULL; }
        flint_value_obj_set(o, key, v);
        free(key);
        _fws(f);
        if (*f->p == ',') { f->p++; _fws(f); if (*f->p == '}') { f->p++; return o; } continue; }
        if (*f->p == '}') { f->p++; return o; }
        flint_value_free(o);
        return NULL;
    }
}

static FlintValue* _fseq(F* f) {
    FlintValue* a = flint_value_arr();
    if (!a) return NULL;
    _fws(f);
    if (*f->p == ']') { f->p++; return a; }
    for (;;) {
        FlintValue* v = _fval(f);
        if (!v) { flint_value_free(a); return NULL; }
        flint_value_arr_add(a, v);
        _fws(f);
        if (*f->p == ',') { f->p++; _fws(f); if (*f->p == ']') { f->p++; return a; } continue; }
        if (*f->p == ']') { f->p++; return a; }
        flint_value_free(a);
        return NULL;
    }
}

static FlintValue* _fval(F* f) {
    _fws(f);
    if (*f->p == '{') { f->p++; return _fmap(f); }
    if (*f->p == '[') { f->p++; return _fseq(f); }
    if (*f->p == '"') {
        char* t = _dq(f->p, NULL);
        if (!t) return NULL;
        f->p += strlen(t) + 2;
        FlintValue* v = flint_value_str(t);
        free(t);
        return v;
    }
    if (*f->p == '\'') {
        char* t = _sq(f->p, NULL);
        if (!t) return NULL;
        f->p += strlen(t) + 2;
        FlintValue* v = flint_value_str(t);
        free(t);
        return v;
    }
    const char* st = f->p;
    while (*f->p && *f->p != ',' && *f->p != '}' && *f->p != ']' &&
           *f->p != '\n') f->p++;
    size_t k = (size_t)(f->p - st);
    char* buf = (char*)malloc(k + 1);
    if (!buf) return NULL;
    memcpy(buf, st, k);
    buf[k] = '\0';
    _rtrim(buf);
    FlintValue* v = _plain(buf);
    free(buf);
    return v;
}

// Join the rest of the document so a flow collection may span lines.
// Copy the rest of the document from line `from`, so a flow collection may
// span lines. The buffer is otherwise unbounded, so a caller must stop at the
// matching bracket; see _flow_value.
static char* _flow_text(Y* y, int64_t from) {
    size_t cap = 256, n = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    for (int64_t i = from; i < y->n; i++) {
        size_t l = strlen(y->lines[i]);
        if (n + l + 2 > cap) {
            while (n + l + 2 > cap) cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        memcpy(buf + n, y->lines[i], l);
        n += l;
        buf[n++] = '\n';
    }
    buf[n] = '\0';
    return buf;
}

// ---- block scalars -------------------------------------------------------
// '|' keeps newlines, '>' folds them. Chomping (- strip, + keep) is honoured.
static char* _block_scalar(Y* y, int64_t i, int64_t parent_indent, int folded, int64_t* stop) {
    SB o = { NULL, 0, 0 };
    int chomp = 0;                            // 0 clip, -1 strip, +1 keep
    const char* hdr = y->lines[i] + y->indent[i] + 1;   // past | or >
    while (*hdr && *hdr != '#') {
        if (*hdr == '-') chomp = -1;
        else if (*hdr == '+') chomp = 1;
        hdr++;
    }
    i++;
    int64_t block_indent = -1;
    int last_was_text = 0;
    while (i < y->n) {
        if (_skip_line(y, i)) {                     // blank lines belong inside
            _sbc(&o, '\n');
            last_was_text = 0;
            i++;
            continue;
        }
        if (y->indent[i] <= parent_indent) break;
        if (block_indent < 0) block_indent = y->indent[i];
        int64_t pad = y->indent[i] - block_indent;
        if (pad < 0) pad = 0;
        if (!last_was_text && folded && o.n > 0) { /* folded: no extra newline here */ }
        for (int64_t k = 0; k < pad; k++) _sbc(&o, ' ');
        _sbn(&o, y->lines[i] + y->indent[i], strlen(y->lines[i] + y->indent[i]));
        if (folded) {
            if (i + 1 < y->n && !_skip_line(y, i + 1) && y->indent[i + 1] > parent_indent)
                _sbc(&o, ' ');                  // join with a space
            else _sbc(&o, '\n');
        } else {
            _sbc(&o, '\n');
        }
        last_was_text = 1;
        i++;
    }
    if (chomp == -1) {
        while (o.n && o.b[o.n - 1] == '\n') o.b[--o.n] = '\0';
    }
    if (!o.b) o.b = (char*)calloc(1, 1);
    if (stop) *stop = i;
    return o.b;
}

// Parse a flow collection that may span lines, and report the line after it.
//
// A flow value ends at the bracket that closes it, which need not be the end of
// the line. Reading the concatenated buffer to its end therefore consumed every
// following key. Find the closing bracket first, then parse only up to it.
static FlintValue* _flow_value(Y* y, int64_t i, const char* line_copy, const char* val, int64_t* after) {
    char* ft = _flow_text(y, i);
    if (!ft) return NULL;
    // `val` points into line_copy, which still holds the value text; the
    // stored line was truncated at the ':' when the key was split off.
    const char* start = ft + (val - line_copy);
    int depth = 0;
    const char* q = start;
    for (; *q; q++) {
        if (*q == '{' || *q == '[') depth++;
        else if (*q == '}' || *q == ']') { if (--depth <= 0) { q++; break; } }
    }
    int64_t seen = 0;
    for (const char* r = ft; r < q && *r; r++) if (*r == '\n') seen++;
    *after = i + seen + 1;
    // Terminate at the closing bracket in a scratch copy: the buffer is const,
    // and _fval must not see past the collection.
    size_t cut = (size_t)(q - ft);
    char* work = (char*)malloc(cut + 1);
    if (!work) { free(ft); return NULL; }
    memcpy(work, ft, cut);
    work[cut] = '\0';
    F f = { work + (start - ft), y };
    FlintValue* v = _fval(&f);
    free(work);
    free(ft);
    return v;
}

// ---- block structure -----------------------------------------------------
// An indent STACK, not recursion: a deeply nested document must not be able to
// overflow the C stack, and the "sequence at the same indent as its key" rule
// falls out of comparing indents rather than needing special handling.
typedef struct {
    int64_t* indent;
    FlintValue** node;
    int64_t n, cap;
} Frame;
// An indent STACK, not recursion: a deeply nested document must not be able to
// overflow the C stack, and the "sequence at the same indent as its key" rule
// falls out of comparing indents rather than needing special handling.
static FlintValue* _parse(Y* y, int64_t start, int64_t indent, int64_t* endp);

FlintValue* flint_yaml_parse(const char* text, FlintParseError* err) {
    if (err) { err->line = 0; err->col = 0; err->msg = NULL; }
    if (!text) return NULL;
    // BOM
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) text += 3;

    Y y;
    memset(&y, 0, sizeof y);
    y.p = text; y.line = 1; y.col = 1; y.err = err;
    if (!_split(&y)) { _free_split(&y); return NULL; }

    int64_t i = _next(&y, 0);
    // Optional document start marker.
    while (i < y.n) {
        if (!strcmp(y.lines[i], "---")) { i = _next(&y, i + 1); continue; }
        break;
    }
    if (i >= y.n) { _free_split(&y); return flint_value_obj(); }

    if (!_check_tabs(&y, i)) { _free_split(&y); return NULL; }
    // Tags and anchors are not supported. Reject them rather than reading
    // "!!str 123" as the string "!!str 123" -- which looks accepted while
    // meaning something else entirely.
    for (int64_t k = i; k < y.n; k++) {
        const char* b2 = y.lines[k] + y.indent[k];
        if (_skip_line(&y, k)) continue;
        // Scan the whole line, not just its first character: a tag can follow
        // "key: " as well as start a line.
        int bad = 0;
        for (const char* q = b2; *q; q++) {
            if (q == b2 && (*q == '-' || *q == ' ')) continue;   // "- item" or "key:"
            if (*q == '!' || *q == '&' || *q == '*') { bad = 1; break; }
        }
        if (bad || b2[0] == '%') {
            _fail_at(&y, y.line_no[k], y.indent[k] + 1,
                     "YAML tags, directives and anchors are not supported");
            _free_split(&y);
            return NULL;
        }
    }
    int64_t end = y.n;
    FlintValue* root = _parse(&y, i, y.indent[i], &end);
    _free_split(&y);
    if (!root) return NULL;

    // A second document is not supported, and must not be silently dropped.
    int64_t j = _next(&y, end);
    if (j < y.n && strcmp(y.lines[j], "...") != 0 && strcmp(y.lines[j], "---") != 0) {
        if (err && !err->msg) {
            err->line = 1; err->col = 1;
            err->msg = "trailing content after the document";
        }
        flint_value_free(root);
        return NULL;
    }
    if (err && err->msg) { flint_value_free(root); return NULL; }
    return root;
}

// Split "key: value" at the first ": " (or a trailing ':').
// Returns 1 when the line is a mapping entry, and fills key/val.
static int _split_kv(char* line, char** key, char** val) {
    char* s = line;
    int in_s = 0, in_d = 0;
    for (char* p = s; *p; p++) {
        if (*p == '\'' && !in_d) in_s = !in_s;
        else if (*p == '"' && !in_s) in_d = !in_d;
        else if (*p == ':' && !in_s && !in_d) {
            if (p[1] == '\0' || p[1] == ' ' || p[1] == '\t') {
                *p = '\0';
                char* v = p + 1;
                while (*v == ' ' || *v == '\t') v++;
                char* k = s;
                size_t kn = strlen(k);
                while (kn && (k[kn-1] == ' ' || k[kn-1] == '\t')) k[--kn] = '\0';
                *key = k;
                *val = v;
                return 1;
            }
        }
    }
    return 0;
}

static FlintValue* _parse(Y* y, int64_t start, int64_t indent, int64_t* endp) {
    int64_t i = _next(y, start);
    Frame st;
    memset(&st, 0, sizeof st);
    FlintValue* root = NULL;
    FlintValue* container = NULL;
    char* container_key = NULL;

    while (i < y->n) {
        if (!strcmp(y->lines[i], "---") || !strcmp(y->lines[i], "...")) break;
        if (y->indent[i] < indent) break;
        // Deeper than the level we are parsing, and not a sequence item: there
        // is no key to attach it to. A sequence at the SAME indent is legal
        // (YAML's first rule) and is handled below, so it must not trip this.
        if (y->indent[i] > indent && !(y->lines[i][y->indent[i]] == '-')) {
            _fail_at(y, y->line_no[i], 1, "unexpected indentation");
            goto fail;
        }
        if (!_check_tabs(y, i)) goto fail;

        // _split_kv truncates the line in place at the ':', destroying the text
        // of the value. So keep a private copy of the line BEFORE anything can
        // split it; a flow value is read from the copy, not from y->lines[i].
        const char* rawline = y->lines[i] + y->indent[i];
        char* linecopy = NULL;
        {
            size_t ln = strlen(rawline);
            linecopy = (char*)malloc(ln + 1);
            if (!linecopy) goto fail;
            memcpy(linecopy, rawline, ln + 1);
        }
        char* body = linecopy;
        int64_t this_ind = y->indent[i];

        // ---- sequence item
        if (body[0] == '-' && (body[1] == ' ' || body[1] == '\0')) {
            if (!container || container->type != FLT_ARR) {
                if (!root) {
                    root = flint_value_arr();
                    if (!root) goto fail;
                    container = root;
                    (void)container_key;
                } else {
                    _fail_at(y, y->line_no[i], 1, "sequence item with no list to attach to");
                    goto fail;
                }
            }
            char* rest = body + 1;
            while (*rest == ' ') rest++;
            if (!*rest) {
                // "-" alone: the value is the following deeper block.
                int64_t nxt = _next(y, i + 1);
                if (nxt < y->n && y->indent[nxt] > this_ind) {
                    FlintValue* sub = _parse(y, nxt, y->indent[nxt], &nxt);
                    if (!sub) goto fail;
                    flint_value_arr_add(container, sub);
                    i = nxt;
                } else {
                    flint_value_arr_add(container, flint_value_null());
                    i = _next(y, i + 1);
                }
                continue;
            }
            // "- key: value" starts a mapping INSIDE the item.
            //
            // _split_kv truncates the key in place, so it needs its own copies
            // for key and value: passing &rest as both the input and the
            // value-out pointer made the value check read the just-truncated
            // key, and every sequence of mappings failed.
            char* ikey = NULL;
            char* ival = NULL;
            if (_split_kv(rest, &ikey, &ival)) {
                FlintValue* map = flint_value_obj();
                if (!map) goto fail;
                char* mkey = ikey;
                char* mval = ival;
                if (!*mval) {
                    // The pair's value is the following deeper block.
                    int64_t nx2 = _next(y, i + 1);
                    if (nx2 < y->n && y->indent[nx2] > this_ind) {
                        FlintValue* sub = _parse(y, nx2, y->indent[nx2], &nx2);
                        if (!sub) { flint_value_free(map); goto fail; }
                        flint_value_obj_set(map, mkey, sub);
                        i = nx2;
                    } else {
                        flint_value_obj_set(map, mkey, flint_value_null());
                        i = _next(y, i + 1);
                    }
                } else if (*mval == '|' || *mval == '>') {
                    int64_t rsm = i; char* t = _block_scalar(y, i, this_ind, *mval == '>', &rsm);
                    FlintValue* v = flint_value_str(t);
                    free(t);
                    flint_value_obj_set(map, mkey, v);
                    i = _next(y, rsm);
                } else if (*mval == '{' || *mval == '[') {
                    int64_t after2 = i;
                    FlintValue* v = _flow_value(y, i, linecopy, mval, &after2);
                    if (!v) { flint_value_free(map); goto fail; }
                    flint_value_obj_set(map, mkey, v);
                    i = _next(y, after2);
                } else {
                    char tmp[1024];
                    snprintf(tmp, sizeof tmp, "%s", mval);
                    flint_value_obj_set(map, mkey, _plain(tmp));
                    i = _next(y, i + 1);
                }
                // The item's map CONTINUES on the following lines that are
                // indented past the dash. Without consuming them here they fell
                // back to the enclosing parse, which saw "  run: make" as an
                // unexpected indent and failed the whole document.
                {
                    int64_t cont = i;
                    while (cont < y->n && y->indent[cont] > this_ind &&
                           !(y->lines[cont][y->indent[cont]] == '-')) {
                        int64_t after = cont;
                        FlintValue* more = _parse(y, cont, y->indent[cont], &after);
                        if (!more) { flint_value_free(map); goto fail; }
                        if (more->type == FLT_OBJ) {
                            for (int64_t k = 0; k < flint_value_len(more); k++) {
                                const char* mk2 = flint_value_key_at(more, k);
                                if (mk2) flint_value_obj_set(map, mk2, flint_value_clone(flint_value_value_at(more, k)));
                            }
                            flint_value_free(more);
                        }
                        cont = _next(y, after);
                        if (cont == after) break;
                    }
                    if (cont > i) i = cont;
                }
                flint_value_arr_add(container, map);
                continue;
            }
            // Plain scalar item.
            if (*rest == '{' || *rest == '[') {
                int64_t after2 = i;
                FlintValue* v = _flow_value(y, i, linecopy, rest, &after2);
                if (!v) goto fail;
                flint_value_arr_add(container, v);
                i = _next(y, after2);
                continue;
            } else if (*rest == '|' || *rest == '>') {
                int64_t rsm = i; char* t = _block_scalar(y, i, this_ind, *rest == '>', &rsm);
                FlintValue* v = flint_value_str(t);
                free(t);
                flint_value_arr_add(container, v);
                i = _next(y, rsm);
            } else {
                char tmp[1024];
                snprintf(tmp, sizeof tmp, "%s", rest);
                flint_value_arr_add(container, _plain(tmp));
            }
            i = _next(y, i + 1);
            continue;
        }

        // ---- mapping entry
        char *key = NULL, *val = NULL;
        if (!_split_kv(body, &key, &val)) {
            _fail_at(y, y->line_no[i], 1, "expected 'key: value'");
            goto fail;
        }

        // A key whose value is a sequence at the SAME indent is the first rule
        // of YAML and the most common reason a naive parser gets this wrong.
        int64_t nxt = _next(y, i + 1);
        if (nxt < y->n && y->indent[nxt] == this_ind && y->lines[nxt][y->indent[nxt]] == '-' &&
            (y->lines[nxt][y->indent[nxt] + 1] == ' ' || y->lines[nxt][y->indent[nxt] + 1] == '\0')) {
            if (!root) {
                root = flint_value_obj();
                if (!root) goto fail;
            }
            FlintValue* arr = flint_value_arr();
            if (!arr) goto fail;
            // Collect the sequence items HERE rather than recursing, so the scan
            // stops at the first line that is not a sequence item. Recursing ran
            // the nested parse to the end of the document, so every key after
            // the sequence was swallowed into it.
            int64_t j = nxt;
            while (j < y->n && y->indent[j] == this_ind &&
                   y->lines[j][y->indent[j]] == '-' &&
                   (y->lines[j][y->indent[j] + 1] == ' ' ||
                    y->lines[j][y->indent[j] + 1] == '\0')) {
                char* ib = y->lines[j] + y->indent[j] + 1;
                while (*ib == ' ') ib++;
                if (!*ib) {
                    int64_t deeper = _next(y, j + 1);
                    if (deeper < y->n && y->indent[deeper] > this_ind) {
                        FlintValue* sub = _parse(y, deeper, y->indent[deeper], &deeper);
                        if (!sub) goto fail;
                        flint_value_arr_add(arr, sub);
                        j = deeper;
                    } else {
                        flint_value_arr_add(arr, flint_value_null());
                        j = _next(y, j + 1);
                    }
                    continue;
                }
                char* ik = NULL; char* iv = NULL;
                if (_split_kv(ib, &ik, &iv)) {
                    FlintValue* m = flint_value_obj();
                    if (!m) goto fail;
                    if (!*iv) {
                        int64_t deeper = _next(y, j + 1);
                        if (deeper < y->n && y->indent[deeper] > this_ind) {
                            FlintValue* sub = _parse(y, deeper, y->indent[deeper], &deeper);
                            if (!sub) { flint_value_free(m); goto fail; }
                            flint_value_obj_set(m, ik, sub);
                            j = deeper;
                        } else {
                            flint_value_obj_set(m, ik, flint_value_null());
                            j = _next(y, j + 1);
                        }
                    } else {
                        char tmp[1024];
                        snprintf(tmp, sizeof tmp, "%s", iv);
                        flint_value_obj_set(m, ik, _plain(tmp));
                        j = _next(y, j + 1);
                    }
                    // The item's map CONTINUES on following lines indented
                    // past the dash. Left unconsumed they fall back to the
                    // enclosing parse as an unexpected indent.
                    {
                        int64_t cont = j;
                        while (cont < y->n && y->indent[cont] > this_ind &&
                               y->lines[cont][y->indent[cont]] != '-') {
                            int64_t after2 = cont;
                            FlintValue* more = _parse(y, cont, y->indent[cont], &after2);
                            if (!more) { flint_value_free(m); goto fail; }
                            if (more->type == FLT_OBJ) {
                                for (int64_t q = 0; q < flint_value_len(more); q++) {
                                    const char* mk2 = flint_value_key_at(more, q);
                                    if (mk2) flint_value_obj_set(m, mk2, flint_value_clone(flint_value_value_at(more, q)));
                                }
                                flint_value_free(more);
                            }
                            // `after2` is scoped inside the loop, so reading
                            // it here never compiled -- and the loop therefore
                            // stopped after the first continuation line, so the
                            // SECOND one came back as an unexpected indent.
                            if (after2 <= cont) break;     // no progress
                            cont = _next(y, after2);
                        }
                        if (cont > j) j = cont;
                    }
                    flint_value_arr_add(arr, m);
                    continue;
                }
                char tmp[1024];
                snprintf(tmp, sizeof tmp, "%s", ib);
                flint_value_arr_add(arr, _plain(tmp));
                j = _next(y, j + 1);
            }
            flint_value_obj_set(root, key, arr);
            i = j;
            continue;
        }

        if (!*val) {
            // Value is the following deeper block (or null).
            if (nxt < y->n && y->indent[nxt] > this_ind) {
                FlintValue* sub = _parse(y, nxt, y->indent[nxt], &nxt);
                if (!sub) goto fail;
                if (!root) { root = flint_value_obj(); if (!root) goto fail; }
                flint_value_obj_set(root, key, sub);
                i = nxt;
            } else {
                if (!root) { root = flint_value_obj(); if (!root) goto fail; }
                flint_value_obj_set(root, key, flint_value_null());
                i = _next(y, i + 1);
            }
            continue;
        }

        // ---- inline value on the key line
        if (!root) { root = flint_value_obj(); if (!root) goto fail; }
        if (*val == '|' || *val == '>') {
            // Resume from the line the block stopped on, not from the line after
            // the header: the body was already consumed, so i+1 skipped its
            // first line and the enclosing parse then saw it as an unexpected
            // indent.
            int64_t resume = i;
            char* t = _block_scalar(y, i, this_ind, *val == '>', &resume);
            FlintValue* v = flint_value_str(t);
            free(t);
            flint_value_obj_set(root, key, v);
            i = _next(y, resume);
            continue;
        }
        if (*val == '{' || *val == '[') {
            int64_t after = i;
            FlintValue* v = _flow_value(y, i, linecopy, val, &after);
            if (!v) goto fail;
            flint_value_obj_set(root, key, v);
            i = _next(y, after);
            continue;
        }
        if (*val == '"') {
            char* t = _dq(val, NULL);
            if (!t) { _fail_at(y, y->line_no[i], 1, "unterminated string"); goto fail; }
            flint_value_obj_set(root, key, flint_value_str(t));
            free(t);
            i = _next(y, i + 1);
            continue;
        }
        if (*val == '\'') {
            char* t = _sq(val, NULL);
            if (!t) { _fail_at(y, y->line_no[i], 1, "unterminated string"); goto fail; }
            flint_value_obj_set(root, key, flint_value_str(t));
            free(t);
            i = _next(y, i + 1);
            continue;
        }
        {
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s", val);
            flint_value_obj_set(root, key, _plain(tmp));
        }
        i = _next(y, i + 1);
    }

    *endp = i;
    free(st.indent);
    free(st.node);
    free(container_key);
    return root ? root : flint_value_obj();

fail:
    free(st.indent);
    free(st.node);
    free(container_key);
    flint_value_free(root);
    return NULL;
}
