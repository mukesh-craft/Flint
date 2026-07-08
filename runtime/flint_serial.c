#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>

// =========================================================================
// Error handling — shared with runtime.c
// =========================================================================
extern int64_t flint_g_err;
extern void flint_set_err(int64_t err);

// =========================================================================
// Base64 tables
// =========================================================================
static const char b64_enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const uint8_t b64_dec[256] = {
    ['+']=62, ['/']=63,
    ['0']=52, ['1']=53, ['2']=54, ['3']=55, ['4']=56,
    ['5']=57, ['6']=58, ['7']=59, ['8']=60, ['9']=61,
    ['A']=0,  ['B']=1,  ['C']=2,  ['D']=3,  ['E']=4,
    ['F']=5,  ['G']=6,  ['H']=7,  ['I']=8,  ['J']=9,
    ['K']=10, ['L']=11, ['M']=12, ['N']=13, ['O']=14,
    ['P']=15, ['Q']=16, ['R']=17, ['S']=18, ['T']=19,
    ['U']=20, ['V']=21, ['W']=22, ['X']=23, ['Y']=24,
    ['Z']=25,
    ['a']=26, ['b']=27, ['c']=28, ['d']=29, ['e']=30,
    ['f']=31, ['g']=32, ['h']=33, ['i']=34, ['j']=35,
    ['k']=36, ['l']=37, ['m']=38, ['n']=39, ['o']=40,
    ['p']=41, ['q']=42, ['r']=43, ['s']=44, ['t']=45,
    ['u']=46, ['v']=47, ['w']=48, ['x']=49, ['y']=50,
    ['z']=51,
};

// =========================================================================
// Base64 encode/decode
// =========================================================================
char* flint_base64_encode(const char* data, int64_t len) {
    if (!data || len < 0) { flint_set_err(1); return NULL; }
    if (len == 0) {
        char* r = malloc(1);
        if (!r) return NULL;
        r[0] = '\0';
        return r;
    }
    int64_t out_len = ((len + 2) / 3) * 4;
    if (out_len < 0) { flint_set_err(1); return NULL; }
    char* out = malloc((size_t)(out_len + 1));
    if (!out) return NULL;

    int64_t i = 0, j = 0;
    while (i < len) {
        unsigned char a = (unsigned char)data[i];
        unsigned char b = (i + 1 < len) ? (unsigned char)data[i + 1] : 0;
        unsigned char c = (i + 2 < len) ? (unsigned char)data[i + 2] : 0;
        uint32_t triple = ((uint32_t)a << 16) | ((uint32_t)b << 8) | (uint32_t)c;

        out[j++] = b64_enc[(triple >> 18) & 0x3F];
        out[j++] = b64_enc[(triple >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64_enc[(triple >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? b64_enc[triple & 0x3F] : '=';
        i += 3;
    }
    out[out_len] = '\0';
    return out;
}

char* flint_base64_decode(const char* s) {
    if (!s) { flint_set_err(1); return NULL; }
    int64_t len = (int64_t)strlen(s);
    if (len < 0) { flint_set_err(1); return NULL; }
    if (len % 4 != 0) { flint_set_err(1); return NULL; }
    if (len == 0) {
        char* r = malloc(1);
        if (!r) return NULL;
        r[0] = '\0';
        return r;
    }

    int64_t pad = 0;
    if (len > 0 && s[len - 1] == '=') pad++;
    if (len > 1 && s[len - 2] == '=') pad++;
    int64_t out_len = (len / 4) * 3 - pad;
    if (out_len < 0) { flint_set_err(1); return NULL; }

    char* out = malloc((size_t)(out_len + 1));
    if (!out) return NULL;

    int64_t i = 0, j = 0;
    while (i < len) {
        uint32_t n = 0;
        for (int k = 0; k < 4; k++) {
            n <<= 6;
            char c = s[i++];
            if (c != '=') {
                if (b64_dec[(unsigned char)c] > 63) { free(out); flint_set_err(1); return NULL; }
                n |= b64_dec[(unsigned char)c];
            }
        }
        if (j < out_len) out[j++] = (char)((n >> 16) & 0xFF);
        if (j < out_len) out[j++] = (char)((n >> 8) & 0xFF);
        if (j < out_len) out[j++] = (char)(n & 0xFF);
    }
    out[out_len] = '\0';
    return out;
}

// =========================================================================
// Hex encode/decode
// =========================================================================
char* flint_hex_encode(const char* data, int64_t len) {
    if (!data || len < 0) { flint_set_err(1); return NULL; }
    if (len == 0) {
        char* r = malloc(1);
        if (!r) return NULL;
        r[0] = '\0';
        return r;
    }
    char* out = malloc((size_t)(len * 2 + 1));
    if (!out) return NULL;
    for (int64_t i = 0; i < len; i++) {
        sprintf(out + i * 2, "%02x", (unsigned char)data[i]);
    }
    out[len * 2] = '\0';
    return out;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char* flint_hex_decode(const char* s) {
    if (!s) { flint_set_err(1); return NULL; }
    int64_t len = (int64_t)strlen(s);
    if (len % 2 != 0) { flint_set_err(1); return NULL; }
    if (len == 0) {
        char* r = malloc(1);
        if (!r) return NULL;
        r[0] = '\0';
        return r;
    }
    int64_t out_len = len / 2;
    char* out = malloc((size_t)(out_len + 1));
    if (!out) return NULL;

    for (int64_t i = 0; i < out_len; i++) {
        int hi = hex_val(s[i * 2]);
        int lo = hex_val(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) { free(out); flint_set_err(1); return NULL; }
        out[i] = (char)((hi << 4) | lo);
    }
    out[out_len] = '\0';
    return out;
}

// =========================================================================
// JSON types
// =========================================================================
typedef enum {
    JSON_NULL, JSON_BOOL, JSON_I64, JSON_F64, JSON_STR, JSON_ARR, JSON_OBJ
} JsonType;

typedef struct JsonValue JsonValue;

struct JsonValue {
    JsonType type;
    union {
        int64_t  bool_val;
        int64_t  i64_val;
        double   f64_val;
        char*    str_val;
        struct {
            JsonValue** items;
            int64_t len;
            int64_t cap;
        } arr;
        struct {
            char** keys;
            JsonValue** values;
            int64_t len;
            int64_t cap;
        } obj;
    };
};

typedef struct {
    JsonValue* value;
    int64_t error_line;
    int64_t error_col;
    const char* error_msg;
} JsonResult;

// =========================================================================
// JSON internal helpers
// =========================================================================
static JsonValue* json_new(JsonType t) {
    JsonValue* v = calloc(1, sizeof(JsonValue));
    if (!v) return NULL;
    v->type = t;
    return v;
}

// =========================================================================
// JSON value constructors
// =========================================================================
JsonValue* flint_json_new_null(void) {
    return json_new(JSON_NULL);
}

JsonValue* flint_json_new_bool(int64_t b) {
    JsonValue* v = json_new(JSON_BOOL);
    if (v) v->bool_val = !!b;
    return v;
}

JsonValue* flint_json_new_i64(int64_t val) {
    JsonValue* v = json_new(JSON_I64);
    if (v) v->i64_val = val;
    return v;
}

JsonValue* flint_json_new_f64(double val) {
    JsonValue* v = json_new(JSON_F64);
    if (v) v->f64_val = val;
    return v;
}

JsonValue* flint_json_new_str(const char* s) {
    JsonValue* v = json_new(JSON_STR);
    if (!v) return NULL;
    if (s) {
        v->str_val = strdup(s);
        if (!v->str_val) { free(v); return NULL; }
    } else {
        v->str_val = strdup("");
        if (!v->str_val) { free(v); return NULL; }
    }
    return v;
}

JsonValue* flint_json_new_arr(void) {
    JsonValue* v = json_new(JSON_ARR);
    if (!v) return NULL;
    v->arr.items = NULL;
    v->arr.len = 0;
    v->arr.cap = 0;
    return v;
}

JsonValue* flint_json_new_obj(void) {
    JsonValue* v = json_new(JSON_OBJ);
    if (!v) return NULL;
    v->obj.keys = NULL;
    v->obj.values = NULL;
    v->obj.len = 0;
    v->obj.cap = 0;
    return v;
}

// =========================================================================
// JSON array/object manipulation
// =========================================================================
void flint_json_arr_add(JsonValue* arr, JsonValue* val) {
    if (!arr || !val || arr->type != JSON_ARR) {
        flint_set_err(1);
        return;
    }
    if (arr->arr.len >= arr->arr.cap) {
        int64_t new_cap = arr->arr.cap ? arr->arr.cap * 2 : 4;
        JsonValue** new_items = realloc(arr->arr.items, (size_t)(new_cap * sizeof(JsonValue*)));
        if (!new_items) { flint_set_err(1); return; }
        arr->arr.items = new_items;
        arr->arr.cap = new_cap;
    }
    arr->arr.items[arr->arr.len++] = val;
}

void flint_json_obj_set(JsonValue* obj, const char* key, JsonValue* val) {
    if (!obj || !key || !val || obj->type != JSON_OBJ) {
        flint_set_err(1);
        return;
    }
    if (obj->obj.len >= obj->obj.cap) {
        int64_t new_cap = obj->obj.cap ? obj->obj.cap * 2 : 4;
        char** new_keys = realloc(obj->obj.keys, (size_t)(new_cap * sizeof(char*)));
        if (!new_keys) { flint_set_err(1); return; }
        obj->obj.keys = new_keys;
        JsonValue** new_values = realloc(obj->obj.values, (size_t)(new_cap * sizeof(JsonValue*)));
        if (!new_values) { flint_set_err(1); return; }
        obj->obj.values = new_values;
        obj->obj.cap = new_cap;
    }
    obj->obj.keys[obj->obj.len] = strdup(key);
    if (!obj->obj.keys[obj->obj.len]) { flint_set_err(1); return; }
    obj->obj.values[obj->obj.len] = val;
    obj->obj.len++;
}

// =========================================================================
// JSON free
// =========================================================================
void flint_json_free(JsonValue* v) {
    if (!v) return;
    switch (v->type) {
        case JSON_STR:
            free(v->str_val);
            break;
        case JSON_ARR:
            for (int64_t i = 0; i < v->arr.len; i++)
                flint_json_free(v->arr.items[i]);
            free(v->arr.items);
            break;
        case JSON_OBJ:
            for (int64_t i = 0; i < v->obj.len; i++) {
                free(v->obj.keys[i]);
                flint_json_free(v->obj.values[i]);
            }
            free(v->obj.keys);
            free(v->obj.values);
            break;
        default:
            break;
    }
    free(v);
}

// =========================================================================
// JSON string buffer for encoding
// =========================================================================
typedef struct {
    char* buf;
    int64_t cap;
    int64_t len;
} JsonBuf;

static int jb_putc(JsonBuf* jb, char c) {
    if (jb->len >= jb->cap) {
        int64_t new_cap = jb->cap ? jb->cap * 2 : 128;
        if (new_cap < jb->len + 1) new_cap = jb->len + 1;
        char* new_buf = realloc(jb->buf, (size_t)new_cap);
        if (!new_buf) return 0;
        jb->buf = new_buf;
        jb->cap = new_cap;
    }
    jb->buf[jb->len++] = c;
    return 1;
}

static int jb_puts(JsonBuf* jb, const char* s) {
    if (!s) return 1;
    while (*s) {
        if (!jb_putc(jb, *s++)) return 0;
    }
    return 1;
}

static int jb_putn(JsonBuf* jb, const char* s, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        if (!jb_putc(jb, s[i])) return 0;
    }
    return 1;
}

static int jb_puti64(JsonBuf* jb, int64_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    if (n <= 0) return 0;
    return jb_putn(jb, tmp, (int64_t)n);
}

static int jb_putf64(JsonBuf* jb, double v) {
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "%.17g", v);
    if (n <= 0) return 0;
    return jb_putn(jb, tmp, (int64_t)n);
}

static int jb_indent(JsonBuf* jb, int spaces) {
    if (!jb_putc(jb, '\n')) return 0;
    for (int i = 0; i < spaces; i++) {
        if (!jb_putc(jb, ' ')) return 0;
    }
    return 1;
}

// =========================================================================
// JSON string encoder (escapes JSON string content)
// =========================================================================
static int jb_encode_str(JsonBuf* jb, const char* s) {
    if (!jb_putc(jb, '"')) return 0;
    while (s && *s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  if (!jb_puts(jb, "\\\"")) return 0; break;
            case '\\': if (!jb_puts(jb, "\\\\")) return 0; break;
            case '\b': if (!jb_puts(jb, "\\b"))  return 0; break;
            case '\f': if (!jb_puts(jb, "\\f"))  return 0; break;
            case '\n': if (!jb_puts(jb, "\\n"))  return 0; break;
            case '\r': if (!jb_puts(jb, "\\r"))  return 0; break;
            case '\t': if (!jb_puts(jb, "\\t"))  return 0; break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    if (!jb_puts(jb, esc)) return 0;
                } else {
                    if (!jb_putc(jb, (char)c)) return 0;
                }
                break;
        }
        s++;
    }
    return jb_putc(jb, '"');
}

// =========================================================================
// JSON value encoder (recursive)
// =========================================================================
static int jb_encode_value(JsonBuf* jb, const JsonValue* v, int indent_step, int depth) {
    if (!v) { return jb_puts(jb, "null"); }

    switch (v->type) {
        case JSON_NULL:
            return jb_puts(jb, "null");

        case JSON_BOOL:
            return jb_puts(jb, v->bool_val ? "true" : "false");

        case JSON_I64:
            return jb_puti64(jb, v->i64_val);

        case JSON_F64:
            return jb_putf64(jb, v->f64_val);

        case JSON_STR:
            return jb_encode_str(jb, v->str_val);

        case JSON_ARR: {
            if (v->arr.len == 0)
                return jb_puts(jb, "[]");
            if (!jb_putc(jb, '[')) return 0;
            for (int64_t i = 0; i < v->arr.len; i++) {
                if (indent_step > 0)
                    if (!jb_indent(jb, indent_step * (depth + 1))) return 0;
                if (!jb_encode_value(jb, v->arr.items[i], indent_step, depth + 1)) return 0;
                if (i + 1 < v->arr.len)
                    if (!jb_putc(jb, ',')) return 0;
            }
            if (indent_step > 0)
                if (!jb_indent(jb, indent_step * depth)) return 0;
            if (!jb_putc(jb, ']')) return 0;
            return 1;
        }

        case JSON_OBJ: {
            if (v->obj.len == 0)
                return jb_puts(jb, "{}");
            if (!jb_putc(jb, '{')) return 0;
            for (int64_t i = 0; i < v->obj.len; i++) {
                if (indent_step > 0)
                    if (!jb_indent(jb, indent_step * (depth + 1))) return 0;
                if (!jb_encode_str(jb, v->obj.keys[i])) return 0;
                if (indent_step > 0) {
                    if (!jb_puts(jb, ": ")) return 0;
                } else {
                    if (!jb_putc(jb, ':')) return 0;
                }
                if (!jb_encode_value(jb, v->obj.values[i], indent_step, depth + 1)) return 0;
                if (i + 1 < v->obj.len)
                    if (!jb_putc(jb, ',')) return 0;
            }
            if (indent_step > 0)
                if (!jb_indent(jb, indent_step * depth)) return 0;
            if (!jb_putc(jb, '}')) return 0;
            return 1;
        }

        default:
            return jb_puts(jb, "null");
    }
}

// =========================================================================
// JSON encode (public)
// =========================================================================
char* flint_json_encode(const JsonValue* v) {
    if (!v) { flint_set_err(1); return NULL; }
    JsonBuf jb;
    jb.buf = NULL;
    jb.cap = 0;
    jb.len = 0;
    if (!jb_encode_value(&jb, v, 0, 0)) {
        free(jb.buf);
        flint_set_err(1);
        return NULL;
    }
    if (!jb_putc(&jb, '\0')) {
        free(jb.buf);
        flint_set_err(1);
        return NULL;
    }
    return jb.buf;
}

char* flint_json_to_string(const JsonValue* v) {
    return flint_json_encode(v);
}

// =========================================================================
// JSON parser
// =========================================================================
typedef struct {
    const char* input;
    int64_t pos;
    int64_t len;
    int64_t line;
    int64_t col;
} JsonParser;

static void jp_skip_ws(JsonParser* p) {
    while (p->pos < p->len) {
        char c = p->input[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (c == '\n') { p->line++; p->col = 1; }
            else { p->col++; }
            p->pos++;
        } else {
            break;
        }
    }
}

static char jp_peek(JsonParser* p) {
    if (p->pos >= p->len) return '\0';
    return p->input[p->pos];
}

static char jp_advance(JsonParser* p) {
    if (p->pos >= p->len) return '\0';
    char c = p->input[p->pos++];
    if (c == '\n') { p->line++; p->col = 1; }
    else { p->col++; }
    return c;
}

static JsonResult jp_error(JsonParser* p, const char* msg) {
    JsonResult r = {NULL, p->line, p->col, msg};
    return r;
}

static JsonValue* jp_parse_value(JsonParser* p, JsonResult* r);

static JsonValue* jp_parse_string(JsonParser* p, JsonResult* r) {
    if (jp_peek(p) != '"') { *r = jp_error(p, "Expected '\"'"); return NULL; }
    jp_advance(p);

    int64_t cap = 64;
    int64_t len = 0;
    char* s = malloc((size_t)cap);
    if (!s) { *r = jp_error(p, "Out of memory"); return NULL; }

    while (p->pos < p->len) {
        char c = jp_advance(p);
        if (c == '"') {
            s[len] = '\0';
            JsonValue* v = json_new(JSON_STR);
            if (!v) { free(s); *r = jp_error(p, "Out of memory"); return NULL; }
            v->str_val = s;
            return v;
        }
        if (c == '\\') {
            if (p->pos >= p->len) { free(s); *r = jp_error(p, "Unexpected end in string escape"); return NULL; }
            char esc = jp_advance(p);
            char ch = 0;
            int known = 1;
            switch (esc) {
                case '"':  ch = '"';  break;
                case '\\': ch = '\\'; break;
                case '/':  ch = '/';  break;
                case 'b':  ch = '\b'; break;
                case 'f':  ch = '\f'; break;
                case 'n':  ch = '\n'; break;
                case 'r':  ch = '\r'; break;
                case 't':  ch = '\t'; break;
                case 'u': {
                    if (p->pos + 4 > p->len) { free(s); *r = jp_error(p, "Invalid unicode escape"); return NULL; }
                    char hex[5];
                    for (int k = 0; k < 4; k++) hex[k] = jp_advance(p);
                    hex[4] = '\0';
                    char* end;
                    long cp = strtol(hex, &end, 16);
                    if (*end != '\0') { free(s); *r = jp_error(p, "Invalid hex in unicode escape"); return NULL; }

                    uint32_t codepoint = (uint32_t)cp;

                    // Handle surrogate pairs
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        // Expect a low surrogate \uXXXX
                        if (p->pos >= p->len || jp_peek(p) != '\\') {
                            // Lone high surrogate - encode as replacement
                            codepoint = 0xFFFD;
                        } else {
                            // Peek ahead for \u
                            if (p->pos + 1 >= p->len || p->input[p->pos + 1] != 'u') {
                                codepoint = 0xFFFD;
                            } else {
                                jp_advance(p); // skip '\'
                                jp_advance(p); // skip 'u'
                                if (p->pos + 4 > p->len) { free(s); *r = jp_error(p, "Invalid unicode escape in surrogate pair"); return NULL; }
                                for (int k = 0; k < 4; k++) hex[k] = jp_advance(p);
                                hex[4] = '\0';
                                long lo = strtol(hex, &end, 16);
                                if (*end != '\0' || lo < 0xDC00 || lo > 0xDFFF) {
                                    codepoint = 0xFFFD;
                                } else {
                                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                                }
                            }
                        }
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        // Lone low surrogate
                        codepoint = 0xFFFD;
                    }

                    // Encode codepoint to UTF-8
                    char utf8[4];
                    int utf8_len;
                    if (codepoint <= 0x7F) {
                        utf8[0] = (char)codepoint;
                        utf8_len = 1;
                    } else if (codepoint <= 0x7FF) {
                        utf8[0] = (char)(0xC0 | (codepoint >> 6));
                        utf8[1] = (char)(0x80 | (codepoint & 0x3F));
                        utf8_len = 2;
                    } else if (codepoint <= 0xFFFF) {
                        utf8[0] = (char)(0xE0 | (codepoint >> 12));
                        utf8[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        utf8[2] = (char)(0x80 | (codepoint & 0x3F));
                        utf8_len = 3;
                    } else if (codepoint <= 0x10FFFF) {
                        utf8[0] = (char)(0xF0 | (codepoint >> 18));
                        utf8[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                        utf8[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        utf8[3] = (char)(0x80 | (codepoint & 0x3F));
                        utf8_len = 4;
                    } else {
                        codepoint = 0xFFFD;
                        utf8[0] = (char)(0xEF);
                        utf8[1] = (char)(0xBF);
                        utf8[2] = (char)(0xBD);
                        utf8_len = 3;
                    }

                    if (len + utf8_len >= cap) {
                        while (cap <= len + utf8_len) cap *= 2;
                        char* ns = realloc(s, (size_t)cap);
                        if (!ns) { free(s); *r = jp_error(p, "Out of memory"); return NULL; }
                        s = ns;
                    }
                    for (int k = 0; k < utf8_len; k++) s[len++] = utf8[k];
                    known = 0;
                    break;
                }
                default: known = 0; break;
            }
            if (!known) { free(s); *r = jp_error(p, "Invalid escape character"); return NULL; }
            if (len + 1 >= cap) {
                cap *= 2;
                char* ns = realloc(s, (size_t)cap);
                if (!ns) { free(s); *r = jp_error(p, "Out of memory"); return NULL; }
                s = ns;
            }
            s[len++] = ch;
        } else {
            if (len + 1 >= cap) {
                cap *= 2;
                char* ns = realloc(s, (size_t)cap);
                if (!ns) { free(s); *r = jp_error(p, "Out of memory"); return NULL; }
                s = ns;
            }
            s[len++] = c;
        }
    }
    free(s);
    *r = jp_error(p, "Unterminated string");
    return NULL;
}

static JsonValue* jp_parse_number(JsonParser* p, JsonResult* r) {
    int64_t start = p->pos;
    int is_float = 0;

    if (jp_peek(p) == '-') jp_advance(p);
    if (jp_peek(p) == '0') {
        jp_advance(p);
    } else if (jp_peek(p) >= '1' && jp_peek(p) <= '9') {
        while (jp_peek(p) >= '0' && jp_peek(p) <= '9') jp_advance(p);
    } else {
        *r = jp_error(p, "Invalid number");
        return NULL;
    }

    if (jp_peek(p) == '.') {
        is_float = 1;
        jp_advance(p);
        if (jp_peek(p) < '0' || jp_peek(p) > '9') {
            *r = jp_error(p, "Invalid number: expected digit after '.'");
            return NULL;
        }
        while (jp_peek(p) >= '0' && jp_peek(p) <= '9') jp_advance(p);
    }

    if (jp_peek(p) == 'e' || jp_peek(p) == 'E') {
        is_float = 1;
        jp_advance(p);
        if (jp_peek(p) == '+' || jp_peek(p) == '-') jp_advance(p);
        if (jp_peek(p) < '0' || jp_peek(p) > '9') {
            *r = jp_error(p, "Invalid number: expected digit in exponent");
            return NULL;
        }
        while (jp_peek(p) >= '0' && jp_peek(p) <= '9') jp_advance(p);
    }

    int64_t num_len = p->pos - start;
    char* num_str = malloc((size_t)(num_len + 1));
    if (!num_str) { *r = jp_error(p, "Out of memory"); return NULL; }
    memcpy(num_str, p->input + start, (size_t)num_len);
    num_str[num_len] = '\0';

    JsonValue* v = malloc(sizeof(JsonValue));
    if (!v) { free(num_str); *r = jp_error(p, "Out of memory"); return NULL; }

    if (is_float) {
        v->type = JSON_F64;
        v->f64_val = strtod(num_str, NULL);
    } else {
        errno = 0;
        int64_t val = (int64_t)strtoll(num_str, NULL, 10);
        if (errno == ERANGE) {
            v->type = JSON_F64;
            v->f64_val = strtod(num_str, NULL);
        } else {
            v->type = JSON_I64;
            v->i64_val = val;
        }
    }
    free(num_str);
    return v;
}

static JsonValue* jp_parse_array(JsonParser* p, JsonResult* r) {
    jp_advance(p);
    jp_skip_ws(p);
    JsonValue* arr = flint_json_new_arr();
    if (!arr) { *r = jp_error(p, "Out of memory"); return NULL; }

    if (jp_peek(p) == ']') { jp_advance(p); return arr; }

    while (1) {
        jp_skip_ws(p);
        JsonValue* item = jp_parse_value(p, r);
        if (!item) { flint_json_free(arr); return NULL; }
        flint_json_arr_add(arr, item);
        jp_skip_ws(p);
        char c = jp_peek(p);
        if (c == ',') { jp_advance(p); continue; }
        if (c == ']') { jp_advance(p); return arr; }
        flint_json_free(arr);
        *r = jp_error(p, "Expected ',' or ']' in array");
        return NULL;
    }
}

static JsonValue* jp_parse_object(JsonParser* p, JsonResult* r) {
    jp_advance(p);
    jp_skip_ws(p);
    JsonValue* obj = flint_json_new_obj();
    if (!obj) { *r = jp_error(p, "Out of memory"); return NULL; }

    if (jp_peek(p) == '}') { jp_advance(p); return obj; }

    while (1) {
        jp_skip_ws(p);
        if (jp_peek(p) != '"') { flint_json_free(obj); *r = jp_error(p, "Expected string key in object"); return NULL; }
        JsonValue* key_val = jp_parse_string(p, r);
        if (!key_val) { flint_json_free(obj); return NULL; }
        const char* key = key_val->str_val;

        jp_skip_ws(p);
        if (jp_peek(p) != ':') {
            free(key_val->str_val);
            free(key_val);
            flint_json_free(obj);
            *r = jp_error(p, "Expected ':' in object");
            return NULL;
        }
        jp_advance(p); // skip ':'
        jp_skip_ws(p);

        JsonValue* val = jp_parse_value(p, r);
        if (!val) {
            free(key_val->str_val);
            free(key_val);
            flint_json_free(obj);
            return NULL;
        }

        flint_json_obj_set(obj, key, val);
        free(key_val->str_val);
        free(key_val);

        jp_skip_ws(p);
        char c = jp_peek(p);
        if (c == ',') { jp_advance(p); continue; }
        if (c == '}') { jp_advance(p); return obj; }
        flint_json_free(obj);
        *r = jp_error(p, "Expected ',' or '}' in object");
        return NULL;
    }
}

static JsonValue* jp_parse_value(JsonParser* p, JsonResult* r) {
    jp_skip_ws(p);
    if (p->pos >= p->len) { *r = jp_error(p, "Unexpected end of input"); return NULL; }
    char c = jp_peek(p);

    switch (c) {
        case 'n':
            if (p->pos + 4 <= p->len && p->input[p->pos+1] == 'u' &&
                p->input[p->pos+2] == 'l' && p->input[p->pos+3] == 'l') {
                p->pos += 4; p->col += 4;
                return flint_json_new_null();
            }
            *r = jp_error(p, "Invalid value at 'n'");
            return NULL;

        case 't':
            if (p->pos + 4 <= p->len && p->input[p->pos+1] == 'r' &&
                p->input[p->pos+2] == 'u' && p->input[p->pos+3] == 'e') {
                p->pos += 4; p->col += 4;
                return flint_json_new_bool(1);
            }
            *r = jp_error(p, "Invalid value at 't'");
            return NULL;

        case 'f':
            if (p->pos + 5 <= p->len && p->input[p->pos+1] == 'a' &&
                p->input[p->pos+2] == 'l' && p->input[p->pos+3] == 's' &&
                p->input[p->pos+4] == 'e') {
                p->pos += 5; p->col += 5;
                return flint_json_new_bool(0);
            }
            *r = jp_error(p, "Invalid value at 'f'");
            return NULL;

        case '"':
            return jp_parse_string(p, r);

        case '[':
            return jp_parse_array(p, r);

        case '{':
            return jp_parse_object(p, r);

        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return jp_parse_number(p, r);

        default:
            *r = jp_error(p, "Unexpected character");
            return NULL;
    }
}

// =========================================================================
// JSON parse (public)
// =========================================================================
JsonResult flint_json_parse(const char* input) {
    JsonResult result = {NULL, 0, 0, NULL};
    if (!input) {
        result.error_msg = "Null input";
        result.error_line = 0;
        result.error_col = 0;
        flint_set_err(1);
        return result;
    }
    JsonParser parser;
    parser.input = input;
    parser.pos = 0;
    parser.len = (int64_t)strlen(input);
    parser.line = 1;
    parser.col = 1;

    JsonValue* v = jp_parse_value(&parser, &result);
    if (!v) {
        if (!result.error_msg) result.error_msg = "Parse error";
        return result;
    }
    jp_skip_ws(&parser);
    if (parser.pos < parser.len) {
        result.error_line = parser.line;
        result.error_col = parser.col;
        result.error_msg = "Trailing characters after JSON value";
        flint_json_free(v);
        return result;
    }
    result.value = v;
    return result;
}

// =========================================================================
// JSON accessor functions
// =========================================================================
int64_t flint_json_get_bool(const JsonValue* v) {
    if (!v || v->type != JSON_BOOL) { flint_set_err(1); return 0; }
    return v->bool_val;
}

int64_t flint_json_get_i64(const JsonValue* v) {
    if (!v || v->type != JSON_I64) { flint_set_err(1); return 0; }
    return v->i64_val;
}

double flint_json_get_f64(const JsonValue* v) {
    if (!v || v->type != JSON_F64) { flint_set_err(1); return 0.0; }
    return v->f64_val;
}

const char* flint_json_get_str(const JsonValue* v) {
    if (!v || v->type != JSON_STR) { flint_set_err(1); return NULL; }
    return v->str_val;
}

int64_t flint_json_arr_len(const JsonValue* v) {
    if (!v || v->type != JSON_ARR) { flint_set_err(1); return 0; }
    return v->arr.len;
}

const JsonValue* flint_json_arr_get(const JsonValue* v, int64_t i) {
    if (!v || v->type != JSON_ARR) { flint_set_err(1); return NULL; }
    if (i < 0 || i >= v->arr.len) { flint_set_err(1); return NULL; }
    return v->arr.items[i];
}

int64_t flint_json_obj_len(const JsonValue* v) {
    if (!v || v->type != JSON_OBJ) { flint_set_err(1); return 0; }
    return v->obj.len;
}

const char* flint_json_obj_key(const JsonValue* v, int64_t i) {
    if (!v || v->type != JSON_OBJ) { flint_set_err(1); return NULL; }
    if (i < 0 || i >= v->obj.len) { flint_set_err(1); return NULL; }
    return v->obj.keys[i];
}

const JsonValue* flint_json_obj_get(const JsonValue* v, const char* key) {
    if (!v || v->type != JSON_OBJ || !key) { flint_set_err(1); return NULL; }
    for (int64_t i = 0; i < v->obj.len; i++) {
        if (strcmp(v->obj.keys[i], key) == 0)
            return v->obj.values[i];
    }
    flint_set_err(1);
    return NULL;
}

// =========================================================================
// JSON high-level API
// =========================================================================
char* flint_json_decode(const char* input) {
    if (!input) { flint_set_err(1); return NULL; }
    JsonResult res = flint_json_parse(input);
    if (res.error_msg) { flint_set_err(1); return NULL; }

    JsonBuf jb;
    jb.buf = NULL;
    jb.cap = 0;
    jb.len = 0;
    if (!jb_encode_value(&jb, res.value, 2, 0)) {
        free(jb.buf);
        flint_json_free(res.value);
        flint_set_err(1);
        return NULL;
    }
    if (!jb_putc(&jb, '\0')) {
        free(jb.buf);
        flint_json_free(res.value);
        flint_set_err(1);
        return NULL;
    }
    flint_json_free(res.value);
    return jb.buf;
}
