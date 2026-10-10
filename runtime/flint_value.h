// flint_value.h — the shared dynamic-value model.
//
// JSON already had one, but it was private to flint_serial.c, so every other
// format would have had to reinvent it. TOML, YAML and the config readers all
// parse into this, which also means a TOML document and a JSON document can be
// walked with the same code.
//
// Deliberately not a general-purpose object model: seven kinds, no cycles, no
// user-defined types. That covers every config and interchange format a
// systems language needs, and it keeps the ownership rule simple enough to
// audit: flint_value_free is the only way to release a tree.

#ifndef FLINT_VALUE_H
#define FLINT_VALUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FLT_NULL, FLT_BOOL, FLT_I64, FLT_F64, FLT_STR, FLT_ARR, FLT_OBJ
} FlintValueType;

typedef struct FlintValue FlintValue;

struct FlintValue {
    FlintValueType type;
    union {
        int64_t  bool_val;
        int64_t  i64_val;
        double   f64_val;
        char*    str_val;          // owned, NUL-terminated
        struct {
            FlintValue** items;
            int64_t len, cap;
        } arr;
        struct {
            char**       keys;     // owned
            FlintValue** values;
            int64_t len, cap;
            // Separate capacity for keys. Sharing one counter with values made
            // the first insert allocate keys but not values, then write
            // through NULL.
            int64_t key_cap;
        } obj;
    };
};

// Where a parse failed. line and col are 1-based; 0 means "not applicable"
// (as for a whole-string failure such as a bad encoding).
typedef struct {
    int64_t     line;
    int64_t     col;
    const char* msg;
} FlintParseError;

// ---- construction --------------------------------------------------------
FlintValue* flint_value_null(void);
FlintValue* flint_value_bool(int64_t b);
FlintValue* flint_value_i64(int64_t v);
FlintValue* flint_value_f64(double v);
FlintValue* flint_value_str(const char* s);          // NULL in, NULL out on OOM
FlintValue* flint_value_arr(void);
FlintValue* flint_value_obj(void);

// ---- containers ----------------------------------------------------------
void        flint_value_arr_add(FlintValue* arr, FlintValue* v);
void        flint_value_obj_set(FlintValue* obj, const char* key, FlintValue* v);
// First match wins, so a later duplicate cannot silently shadow an earlier one.
FlintValue* flint_value_obj_get(const FlintValue* obj, const char* key);
int64_t     flint_value_obj_has(const FlintValue* obj, const char* key);
int64_t     flint_value_len(const FlintValue* v);     // array/object length, else 0
FlintValue* flint_value_at(const FlintValue* arr, int64_t i);
const char* flint_value_key_at(const FlintValue* obj, int64_t i);
FlintValue* flint_value_value_at(const FlintValue* obj, int64_t i);

// ---- typed access with defaults -----------------------------------------
// Each returns `dflt` on a type mismatch rather than coercing, so reading a
// string where a number was expected is visible at the call site.
int64_t     flint_value_as_i64(const FlintValue* v, int64_t dflt);
double      flint_value_as_f64(const FlintValue* v, double dflt);
int64_t     flint_value_as_bool(const FlintValue* v, int64_t dflt);
// Always returns a valid string (possibly ""), never NULL.
const char* flint_value_as_str(const FlintValue* v, const char* dflt);

// ---- convenience readers -------------------------------------------------
// Follow "a.b.c", building objects for missing links, so
// flint_value_path(d, "server.http.port", 8080) works without walking.
int64_t     flint_value_path_i64(const FlintValue* root, const char* path, int64_t dflt);
const char* flint_value_path_str(const FlintValue* root, const char* path, const char* dflt);
FlintValue* flint_value_path(const FlintValue* root, const char* path);

// ---- text form -----------------------------------------------------------
// Encoding is owned by the caller (flint_value_free_string).
char*       flint_value_encode(const FlintValue* v, int64_t indent);
char*       flint_value_to_string(const FlintValue* v);   // compact
void        flint_value_free_string(char* s);
void        flint_value_free(FlintValue* v);

// Deep copy, so a caller can mutate a subtree without touching the original.
FlintValue* flint_value_clone(const FlintValue* v);

// ---- parsers ------------------------------------------------------------
// Each returns NULL and fills *err (if non-NULL) on failure. Input is
// NUL-terminated; every one of them handles a UTF-8 BOM.
FlintValue* flint_toml_parse(const char* text, FlintParseError* err);
FlintValue* flint_yaml_parse(const char* text, FlintParseError* err);
FlintValue* flint_json_parse(const char* text, FlintParseError* err);

#ifdef __cplusplus
}
#endif

#endif // FLINT_VALUE_H