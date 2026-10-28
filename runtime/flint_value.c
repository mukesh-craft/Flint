// flint_value.c — the shared dynamic-value model. Per flint_value.h.

#include "flint_value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---- growable buffers ----------------------------------------------------
// A tiny helper so arr/obj growth is not open-coded four times.
static void _grow(void** p, int64_t* cap, int64_t need, size_t elem) {
    if (need <= *cap) return;
    int64_t ncap = *cap ? *cap * 2 : 8;
    while (ncap < need) ncap *= 2;
    void* np = realloc(*p, (size_t)ncap * elem);
    if (!np) return;                     // caller keeps the old buffer
    *p = np;
    *cap = ncap;
}

FlintValue* flint_value_null(void) {
    FlintValue* v = (FlintValue*)calloc(1, sizeof(FlintValue));
    if (v) v->type = FLT_NULL;
    return v;
}

FlintValue* flint_value_bool(int64_t b) {
    FlintValue* v = flint_value_null();
    if (v) { v->type = FLT_BOOL; v->bool_val = b ? 1 : 0; }
    return v;
}

FlintValue* flint_value_i64(int64_t x) {
    FlintValue* v = flint_value_null();
    if (v) { v->type = FLT_I64; v->i64_val = x; }
    return v;
}

FlintValue* flint_value_f64(double x) {
    FlintValue* v = flint_value_null();
    if (v) { v->type = FLT_F64; v->f64_val = x; }
    return v;
}

FlintValue* flint_value_str(const char* s) {
    FlintValue* v = flint_value_null();
    if (!v) return NULL;
    v->type = FLT_STR;
    if (!s) s = "";
    size_t n = strlen(s);
    v->str_val = (char*)malloc(n + 1);
    if (!v->str_val) { free(v); return NULL; }
    memcpy(v->str_val, s, n + 1);
    return v;
}

FlintValue* flint_value_arr(void) {
    FlintValue* v = flint_value_null();
    if (v) v->type = FLT_ARR;
    return v;
}

FlintValue* flint_value_obj(void) {
    FlintValue* v = flint_value_null();
    if (v) v->type = FLT_OBJ;
    return v;
}

// ---- containers ----------------------------------------------------------
void flint_value_arr_add(FlintValue* arr, FlintValue* item) {
    if (!arr || arr->type != FLT_ARR || !item) return;
    _grow((void**)&arr->arr.items, &arr->arr.cap, arr->arr.len + 1, sizeof(FlintValue*));
    if (arr->arr.len < arr->arr.cap) arr->arr.items[arr->arr.len++] = item;
    else flint_value_free(item);          // allocation failed; do not leak
}

void flint_value_obj_set(FlintValue* obj, const char* key, FlintValue* val) {
    if (!obj || obj->type != FLT_OBJ || !key || !val) { flint_value_free(val); return; }
    // First match wins, so a later duplicate cannot silently shadow.
    for (int64_t i = 0; i < obj->obj.len; i++) {
        if (strcmp(obj->obj.keys[i], key) == 0) {
            flint_value_free(obj->obj.values[i]);
            obj->obj.values[i] = val;
            return;
        }
    }
    // keys and values are parallel arrays sharing ONE capacity counter, so each
    // needs its own counter: growing `keys` bumped `cap`, and the second _grow
    // then saw the requirement already met and never allocated `values` at all.
    // The first insert wrote through a NULL values pointer.
    _grow((void**)&obj->obj.keys, &obj->obj.key_cap, obj->obj.len + 1, sizeof(char*));
    _grow((void**)&obj->obj.values, &obj->obj.cap, obj->obj.len + 1, sizeof(FlintValue*));
    if (obj->obj.len >= obj->obj.cap || obj->obj.len >= obj->obj.key_cap) {
        flint_value_free(val);
        return;
    }
    obj->obj.keys[obj->obj.len] = strdup(key);
    if (!obj->obj.keys[obj->obj.len]) { flint_value_free(val); return; }
    obj->obj.values[obj->obj.len] = val;
    obj->obj.len++;
}

FlintValue* flint_value_obj_get(const FlintValue* obj, const char* key) {
    if (!obj || obj->type != FLT_OBJ || !key) return NULL;
    for (int64_t i = 0; i < obj->obj.len; i++)
        if (strcmp(obj->obj.keys[i], key) == 0) return obj->obj.values[i];
    return NULL;
}

int64_t flint_value_obj_has(const FlintValue* obj, const char* key) {
    return flint_value_obj_get(obj, key) != NULL;
}

int64_t flint_value_len(const FlintValue* v) {
    if (!v) return 0;
    if (v->type == FLT_ARR) return v->arr.len;
    if (v->type == FLT_OBJ) return v->obj.len;
    return 0;
}

FlintValue* flint_value_at(const FlintValue* arr, int64_t i) {
    if (!arr || arr->type != FLT_ARR || i < 0 || i >= arr->arr.len) return NULL;
    return arr->arr.items[i];
}

const char* flint_value_key_at(const FlintValue* obj, int64_t i) {
    if (!obj || obj->type != FLT_OBJ || i < 0 || i >= obj->obj.len) return NULL;
    return obj->obj.keys[i];
}

FlintValue* flint_value_value_at(const FlintValue* obj, int64_t i) {
    if (!obj || obj->type != FLT_OBJ || i < 0 || i >= obj->obj.len) return NULL;
    return obj->obj.values[i];
}

// ---- typed access --------------------------------------------------------
int64_t flint_value_as_i64(const FlintValue* v, int64_t dflt) {
    if (!v) return dflt;
    if (v->type == FLT_I64) return v->i64_val;
    if (v->type == FLT_BOOL) return v->bool_val;
    // A float that happens to be integral converts; one that does not is a
    // type error the caller should see, so it falls back to the default.
    if (v->type == FLT_F64 && v->f64_val == floor(v->f64_val)) return (int64_t)v->f64_val;
    return dflt;
}

double flint_value_as_f64(const FlintValue* v, double dflt) {
    if (!v) return dflt;
    if (v->type == FLT_F64) return v->f64_val;
    if (v->type == FLT_I64) return (double)v->i64_val;
    return dflt;
}

int64_t flint_value_as_bool(const FlintValue* v, int64_t dflt) {
    if (!v) return dflt;
    if (v->type == FLT_BOOL) return v->bool_val;
    return dflt;
}

const char* flint_value_as_str(const FlintValue* v, const char* dflt) {
    if (v && v->type == FLT_STR) return v->str_val;
    return dflt ? dflt : "";
}

// ---- path lookup ---------------------------------------------------------
FlintValue* flint_value_path(const FlintValue* root, const char* path) {
    if (!root || !path || !*path) return NULL;
    // Walk a copy of the path so we can split on '.' without mutating the
    // caller's string.
    size_t n = strlen(path);
    char* buf = (char*)malloc(n + 1);
    if (!buf) return NULL;
    memcpy(buf, path, n + 1);

    const FlintValue* cur = root;
    char* save = NULL;
    for (char* tok = strtok_r(buf, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) {
        if (cur->type == FLT_OBJ) {
            cur = flint_value_obj_get(cur, tok);
        } else if (cur->type == FLT_ARR) {
            // A numeric component indexes an array: "items.0.name".
            char* end = NULL;
            long idx = strtol(tok, &end, 10);
            cur = (end && *end == '\0') ? flint_value_at(cur, idx) : NULL;
        } else {
            cur = NULL;
        }
        if (!cur) break;
    }
    free(buf);
    return (FlintValue*)cur;
}

int64_t flint_value_path_i64(const FlintValue* root, const char* path, int64_t dflt) {
    return flint_value_as_i64(flint_value_path(root, path), dflt);
}

const char* flint_value_path_str(const FlintValue* root, const char* path, const char* dflt) {
    const char* s = flint_value_as_str(flint_value_path(root, path), dflt);
    return s ? s : "";
}

// ---- encoding ------------------------------------------------------------
typedef struct { char* buf; size_t len, cap; int oom; } SBuf;

static void _sb_putn(SBuf* s, const char* p, size_t n) {
    if (s->oom) return;
    if (s->len + n + 1 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 128;
        while (ncap < s->len + n + 1) ncap *= 2;
        char* nb = (char*)realloc(s->buf, ncap);
        if (!nb) { s->oom = 1; return; }
        s->buf = nb;
        s->cap = ncap;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void _sb_puts(SBuf* s, const char* p) { _sb_putn(s, p, strlen(p)); }
static void _sb_putc(SBuf* s, char c) { _sb_putn(s, &c, 1); }

// Escape per RFC 8259 / TOML basic strings: the two mandatory escapes, the
// short forms, and \uXXXX for everything else. Control characters go out as
// \u00XX because a literal newline inside a string is not representable.
static void _sb_quoted(SBuf* s, const char* str) {
    _sb_putc(s, '"');
    for (const unsigned char* p = (const unsigned char*)str; *p; p++) {
        switch (*p) {
            case '"':  _sb_puts(s, "\\\""); break;
            case '\\': _sb_puts(s, "\\\\"); break;
            case '\n': _sb_puts(s, "\\n"); break;
            case '\r': _sb_puts(s, "\\r"); break;
            case '\t': _sb_puts(s, "\\t"); break;
            case '\b': _sb_puts(s, "\\b"); break;
            case '\f': _sb_puts(s, "\\f"); break;
            default:
                if (*p < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", *p);
                    _sb_puts(s, buf);
                } else {
                    _sb_putc(s, (char)*p);
                }
        }
    }
    _sb_putc(s, '"');
}

static void _encode(SBuf* s, const FlintValue* v, int64_t indent, int depth) {
    char buf[64];
    switch (v->type) {
        case FLT_NULL: _sb_puts(s, "null"); break;
        case FLT_BOOL: _sb_puts(s, v->bool_val ? "true" : "false"); break;
        case FLT_I64:
            snprintf(buf, sizeof buf, "%lld", (long long)v->i64_val);
            _sb_puts(s, buf);
            break;
        case FLT_F64:
            // %.17g round-trips a double exactly; trim so 1.0 prints as 1.0
            // rather than 1.0000000000000000.
            snprintf(buf, sizeof buf, "%.17g", v->f64_val);
            _sb_puts(s, buf);
            break;
        case FLT_STR: _sb_quoted(s, v->str_val); break;
        case FLT_ARR:
            if (v->arr.len == 0) { _sb_puts(s, "[]"); break; }
            _sb_puts(s, "[");
            for (int64_t i = 0; i < v->arr.len; i++) {
                if (i) _sb_puts(s, indent >= 0 ? "," : ",");
                if (indent >= 0) { _sb_putc(s, '\n'); for (int64_t k = 0; k < (depth + 1) * indent; k++) _sb_putc(s, ' '); }
                _encode(s, v->arr.items[i], indent, depth + 1);
            }
            if (indent >= 0) { _sb_putc(s, '\n'); for (int64_t k = 0; k < depth * indent; k++) _sb_putc(s, ' '); }
            _sb_putc(s, ']');
            break;
        case FLT_OBJ:
            if (v->obj.len == 0) { _sb_puts(s, "{}"); break; }
            _sb_puts(s, "{");
            for (int64_t i = 0; i < v->obj.len; i++) {
                if (i) _sb_putc(s, ',');
                if (indent >= 0) { _sb_putc(s, '\n'); for (int64_t k = 0; k < (depth + 1) * indent; k++) _sb_putc(s, ' '); }
                _sb_quoted(s, v->obj.keys[i]);
                _sb_puts(s, indent >= 0 ? ": " : ":");
                _encode(s, v->obj.values[i], indent, depth + 1);
            }
            if (indent >= 0) { _sb_putc(s, '\n'); for (int64_t k = 0; k < depth * indent; k++) _sb_putc(s, ' '); }
            _sb_putc(s, '}');
            break;
    }
}

char* flint_value_encode(const FlintValue* v, int64_t indent) {
    if (!v) return NULL;
    SBuf s = { NULL, 0, 0, 0 };
    _encode(&s, v, indent, 0);
    if (s.oom) { free(s.buf); return NULL; }
    if (!s.buf) return strdup("null");
    return s.buf;
}

char* flint_value_to_string(const FlintValue* v) { return flint_value_encode(v, -1); }

void flint_value_free_string(char* s) { free(s); }

// ---- teardown ------------------------------------------------------------
void flint_value_free(FlintValue* v) {
    if (!v) return;
    // Iterative teardown with an explicit stack: a deeply nested config would
    // otherwise recurse once per level, and a hostile document can be deep.
    // Fixed cap with a growable fallback is simpler to audit than realloc churn.
    FlintValue** stack = NULL;
    int64_t sn = 0, scap = 0;
    #define PUSH(x) do { \
        if (sn == scap) { \
            int64_t nc = scap ? scap * 2 : 64; \
            FlintValue** nb = (FlintValue**)realloc(stack, (size_t)nc * sizeof(FlintValue*)); \
            if (!nb) { free(stack); return; } \
            stack = nb; scap = nc; \
        } \
        stack[sn++] = (x); \
    } while (0)

    PUSH(v);
    while (sn > 0) {
        FlintValue* cur = stack[--sn];
        if (!cur) continue;
        if (cur->type == FLT_ARR) {
            for (int64_t i = 0; i < cur->arr.len; i++) PUSH(cur->arr.items[i]);
            free(cur->arr.items);
        } else if (cur->type == FLT_OBJ) {
            for (int64_t i = 0; i < cur->obj.len; i++) {
                free(cur->obj.keys[i]);
                PUSH(cur->obj.values[i]);
            }
            free(cur->obj.keys);
            free(cur->obj.values);
        } else if (cur->type == FLT_STR) {
            free(cur->str_val);
        }
        free(cur);
    }
    free(stack);
    #undef PUSH
}

FlintValue* flint_value_clone(const FlintValue* v) {
    if (!v) return NULL;
    switch (v->type) {
        case FLT_NULL: return flint_value_null();
        case FLT_BOOL: return flint_value_bool(v->bool_val);
        case FLT_I64:  return flint_value_i64(v->i64_val);
        case FLT_F64:  return flint_value_f64(v->f64_val);
        case FLT_STR:  return flint_value_str(v->str_val);
        case FLT_ARR: {
            FlintValue* a = flint_value_arr();
            if (!a) return NULL;
            for (int64_t i = 0; i < v->arr.len; i++) {
                FlintValue* c = flint_value_clone(v->arr.items[i]);
                if (!c) { flint_value_free(a); return NULL; }
                flint_value_arr_add(a, c);
            }
            return a;
        }
        case FLT_OBJ: {
            FlintValue* o = flint_value_obj();
            if (!o) return NULL;
            for (int64_t i = 0; i < v->obj.len; i++) {
                FlintValue* c = flint_value_clone(v->obj.values[i]);
                if (!c) { flint_value_free(o); return NULL; }
                flint_value_obj_set(o, v->obj.keys[i], c);
            }
            return o;
        }
    }
    return flint_value_null();
}