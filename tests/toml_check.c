// Exercises the TOML parser against the shapes a config file actually uses.
//
// Assertions are on the parsed tree, not on incidental representation, so a
// correct document is not failed by a detail that does not change meaning.
#include "flint_value.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
static void ck(const char* what, int ok) {
    if (ok) printf("PASS %s\n", what);
    else { printf("FAIL %s\n", what); fails++; }
}

static const char* DOC =
    "# a comment\n"
    "title = \"Flint config\"\n"
    "enabled = true\n"
    "disabled = false\n"
    "count = 42\n"
    "negative = -7\n"
    "big = 1_000_000\n"
    "hex = 0xFF\n"
    "octal = 0o755\n"
    "binary = 0b1010\n"
    "ratio = 3.14\n"
    "exponent = 1e3\n"
    "negative_exp = 2.5e-2\n"
    "infinite = inf\n"
    "notnum = nan\n"
    "\n"
    "[server]\n"
    "host = \"localhost\"\n"
    "port = 8080\n"
    "\n"
    "[server.tls]\n"
    "enabled = true\n"
    "\n"
    "[database]\n"
    "url = \"postgres://localhost/db\"\n"
    "pool = 10\n"
    "\n"
    "[[products]]\n"
    "name = \"hammer\"\n"
    "qty = 2\n"
    "\n"
    "[[products]]\n"
    "name = \"nail\"\n"
    "qty = 500\n"
    "\n"
    "[[products]]\n"
    "name = \"saw\"\n"
    "qty = 7\n"
    "\n"
    "# A key written here belongs to the LAST product, per the TOML spec:\n"
    "# everything after a [[table]] header is that table's body until the next\n"
    "# header. Wrap the rest in a real [table] so they land at the root.\n"
    "[options]\n"
    "literal = 'C:\\path\\no-escapes'\n"
    "multi = \"\"\"\nline one\nline two\"\"\"\n"
    "folded = \"\"\"\\\n   joined\"\"\"\n"
    "dotted.key = 5\n"
    "inline = { a = 1, b = \"two\" }\n"
    "mixed = [1, 2, 3]\n"
    "nested = [[1, 2], [3]]\n"
    "empties = {}\n"
    "empty_list = []\n"
    "when = 1979-05-27T07:32:00Z\n"
    "stamp = 1979-05-27\n"
    "clock = 07:32:00\n";

int main(void) {
    FlintParseError err = { 0, 0, NULL };
    FlintValue* d = flint_toml_parse(DOC, &err);
    if (!d) {
        printf("FAIL parse: %s at %lld:%lld\n", err.msg ? err.msg : "?",
               (long long)err.line, (long long)err.col);
        return 1;
    }
    ck("parsed to an object", d->type == FLT_OBJ);

    // ---- scalars
    ck("string", strcmp(flint_value_as_str(flint_value_obj_get(d, "title"), ""), "Flint config") == 0);
    ck("true",  flint_value_as_bool(flint_value_obj_get(d, "enabled"), -1) == 1);
    ck("false", flint_value_as_bool(flint_value_obj_get(d, "disabled"), -1) == 0);
    ck("int",   flint_value_as_i64(flint_value_obj_get(d, "count"), -1) == 42);
    ck("negative int", flint_value_as_i64(flint_value_obj_get(d, "negative"), 0) == -7);
    ck("underscores ignored", flint_value_as_i64(flint_value_obj_get(d, "big"), 0) == 1000000);
    ck("hex",     flint_value_as_i64(flint_value_obj_get(d, "hex"), 0) == 255);
    ck("octal",   flint_value_as_i64(flint_value_obj_get(d, "octal"), 0) == 0o755);
    ck("binary",  flint_value_as_i64(flint_value_obj_get(d, "binary"), 0) == 0b1010);
    ck("float",   fabs(flint_value_as_f64(flint_value_obj_get(d, "ratio"), 0) - 3.14) < 1e-9);
    ck("exponent float", flint_value_as_f64(flint_value_obj_get(d, "exponent"), 0) == 1000.0);
    ck("negative exponent", fabs(flint_value_as_f64(flint_value_obj_get(d, "negative_exp"), 0) - 0.025) < 1e-12);
    ck("inf", flint_value_as_f64(flint_value_obj_get(d, "infinite"), 0) > 1e308);
    ck("nan is nan", isnan(flint_value_as_f64(flint_value_obj_get(d, "notnum"), 0)));

    // ---- tables
    ck("[server] table", flint_value_path_i64(d, "server.port", -1) == 8080);
    ck("[server] host", strcmp(flint_value_path_str(d, "server.host", ""), "localhost") == 0);
    ck("nested table", flint_value_path_i64(d, "server.tls.enabled", -1) == 1);
    ck("[database] table", flint_value_path_i64(d, "database.pool", -1) == 10);

    // ---- array of tables: three entries must all survive
    FlintValue* prods = flint_value_obj_get(d, "products");
    ck("array-of-tables present", prods != NULL);
    ck("array-of-tables is an array", prods && prods->type == FLT_ARR);
    ck("array-of-tables has 3 entries", flint_value_len(prods) == 3);
    if (flint_value_len(prods) == 3) {
        ck("  entry 0", strcmp(flint_value_as_str(flint_value_obj_get(flint_value_at(prods, 0), "name"), ""), "hammer") == 0);
        ck("  entry 1", strcmp(flint_value_as_str(flint_value_obj_get(flint_value_at(prods, 1), "name"), ""), "nail") == 0);
        ck("  entry 2 qty", flint_value_as_i64(flint_value_obj_get(flint_value_at(prods, 2), "qty"), 0) == 7);
    }

    // ---- strings
    ck("literal string keeps backslashes",
       strcmp(flint_value_as_str(flint_value_obj_get(flint_value_obj_get(d, "options"), "literal"), ""), "C:\\path\\no-escapes") == 0);
    {
        const char* m = flint_value_as_str(flint_value_obj_get(flint_value_obj_get(d, "options"), "multi"), "");
        ck("multi-line string keeps the newline", strchr(m, '\n') != NULL);
        ck("multi-line content", strncmp(m, "line one\nline two", 17) == 0);
    }
    ck("folded multi-line joins lines",
       strcmp(flint_value_as_str(flint_value_obj_get(flint_value_obj_get(d, "options"), "folded"), ""), "joined") == 0);

    // ---- keys and containers
    ck("dotted key", flint_value_path_i64(d, "options.dotted.key", -1) == 5);
    ck("inline table", flint_value_path_i64(d, "options.inline.a", -1) == 1 &&
                       strcmp(flint_value_path_str(d, "options.inline.b", ""), "two") == 0);
    ck("array of ints", flint_value_len(flint_value_obj_get(flint_value_obj_get(d, "options"), "mixed")) == 3 &&
                         flint_value_as_i64(flint_value_at(flint_value_obj_get(flint_value_obj_get(d, "options"), "mixed"), 2), 0) == 3);
    ck("nested arrays", flint_value_len(flint_value_obj_get(flint_value_obj_get(d, "options"), "nested")) == 2 &&
                        flint_value_len(flint_value_at(flint_value_obj_get(flint_value_obj_get(d, "options"), "nested"), 0)) == 2);
    ck("empty inline table", flint_value_len(flint_value_obj_get(flint_value_obj_get(d, "options"), "empties")) == 0);
    ck("empty list", flint_value_len(flint_value_obj_get(flint_value_obj_get(d, "options"), "empty_list")) == 0);

    // ---- dates arrive as normalised strings
    ck("offset date-time", strcmp(flint_value_as_str(flint_value_obj_get(flint_value_obj_get(d, "options"), "when"), ""), "1979-05-27T07:32:00Z") == 0);
    ck("local date", strcmp(flint_value_as_str(flint_value_obj_get(flint_value_obj_get(d, "options"), "stamp"), ""), "1979-05-27") == 0);
    ck("local time", strcmp(flint_value_as_str(flint_value_obj_get(flint_value_obj_get(d, "options"), "clock"), ""), "07:32:00") == 0);

    // ---- round trip
    {
        char* enc = flint_value_encode(d, 2);
        ck("re-encodes", enc != NULL && strstr(enc, "title") != NULL);
        free(enc);
    }

    // ---- errors are reported, never silently accepted
    {
        struct { const char* doc; const char* what; } bad[] = {
            { "key = ",             "missing value" },
            { "key = \"unterminated", "unterminated string" },
            { "[unclosed\n",         "unclosed table" },
            { "= 5",                "missing key" },
            { "key = [1, 2",        "unterminated array" },
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            FlintParseError e2 = { 0, 0, NULL };
            FlintValue* r = flint_toml_parse(bad[i].doc, &e2);
            ck(bad[i].what, r == NULL && e2.msg != NULL);
            flint_value_free(r);
        }
    }

    flint_value_free(d);
    printf("toml-check: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}