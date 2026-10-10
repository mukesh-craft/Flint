// Exercises the YAML parser: block mappings and sequences, the
// sequence-at-the-same-indent rule, flow collections, block scalars, quoting,
// typing, comments, and the error cases.
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
    "# top comment\n"
    "name: flint\n"
    "version: 3\n"
    "ratio: 0.5\n"
    "enabled: true\n"
    "disabled: false\n"
    "nothing: ~\n"
    "empty:\n"
    "hexnum: 0x1F\n"
    "quoted: \"a: b # not a comment\"\n"
    "single: 'it''s here'\n"
    "tagged: !!str 123\n"          /* tags unsupported -> must error */
    ;

static const char* DOC2 =
    "server:\n"
    "  host: localhost\n"
    "  port: 8080\n"
    "  tls:\n"
    "    enabled: true\n"
    "database:\n"
    "  url: postgres://localhost/db\n"
    "  pool: 10\n"
    ;

static const char* DOC3 =
    "steps:\n"
    "- name: build\n"
    "  run: make\n"
    "- name: test\n"
    "  run: make check\n"
    "- name: ship\n"
    "  run: make publish\n"
    ;

static const char* DOC4 =
    "list:\n"
    "- 1\n"
    "- 2\n"
    "- 3\n"
    "same_indent:\n"
    "- a\n"
    "- b\n"
    "nested:\n"
    "  - x: 1\n"
    "    y: 2\n"
    "  - x: 3\n"
    "    y: 4\n"
    "after: 1\n"
    ;

static const char* DOC5 =
    "flow_map: {a: 1, b: two}\n"
    "flow_seq: [1, 2, 3]\n"
    "flow_nested: {list: [1, {deep: true}]}\n"
    "literal: |\n"
    "  line one\n"
    "  line two\n"
    "folded: >\n"
    "  joined line\n"
    "  continues here\n"
    "after: done\n"
    ;

int main(void) {
    FlintParseError e = { 0, 0, NULL };

    // ---- scalars and typing (DOC has an unsupported tag, so expect failure)
    FlintValue* d1 = flint_yaml_parse(DOC, &e);
    ck("unsupported tag is an error, not a silent string", d1 == NULL && e.msg != NULL);
    flint_value_free(d1);

    // The same document without the tagged line must parse.
    {
        const char* ok =
            "name: flint\nversion: 3\nratio: 0.5\nenabled: true\n"
            "disabled: false\nnothing: ~\nempty:\nhexnum: 0x1F\n"
            "quoted: \"a: b # not a comment\"\nsingle: 'it''s here'\n";
        FlintParseError e2 = { 0, 0, NULL };
        FlintValue* d = flint_yaml_parse(ok, &e2);
        if (!d) { printf("FAIL scalar doc: %s at %lld:%lld\n", e2.msg, (long long)e2.line, (long long)e2.col); fails++; }
        else {
            ck("string scalar", strcmp(flint_value_as_str(flint_value_obj_get(d, "name"), ""), "flint") == 0);
            ck("int scalar", flint_value_as_i64(flint_value_obj_get(d, "version"), -1) == 3);
            ck("float scalar", fabs(flint_value_as_f64(flint_value_obj_get(d, "ratio"), 0) - 0.5) < 1e-12);
            ck("bool true", flint_value_as_bool(flint_value_obj_get(d, "enabled"), -1) == 1);
            ck("bool false", flint_value_as_bool(flint_value_obj_get(d, "disabled"), -1) == 0);
            ck("tilde is null", flint_value_obj_get(d, "nothing")->type == FLT_NULL);
            ck("empty value is null", flint_value_obj_get(d, "empty")->type == FLT_NULL);
            ck("hex int", flint_value_as_i64(flint_value_obj_get(d, "hexnum"), -1) == 31);
            ck("quoted keeps #", strcmp(flint_value_as_str(flint_value_obj_get(d, "quoted"), ""), "a: b # not a comment") == 0);
            ck("single-quote escape", strcmp(flint_value_as_str(flint_value_obj_get(d, "single"), ""), "it's here") == 0);
            flint_value_free(d);
        }
    }

    // ---- nested mappings
    {
        FlintParseError e2 = { 0, 0, NULL };
        FlintValue* d = flint_yaml_parse(DOC2, &e2);
        if (!d) { printf("FAIL nested doc: %s at %lld:%lld\n", e2.msg, (long long)e2.line, (long long)e2.col); fails++; }
        else {
            ck("nested host", strcmp(flint_value_path_str(d, "server.host", ""), "localhost") == 0);
            ck("nested port", flint_value_path_i64(d, "server.port", -1) == 8080);
            ck("two-level nest", flint_value_path_i64(d, "server.tls.enabled", -1) == 1);
            ck("sibling table", flint_value_path_i64(d, "database.pool", -1) == 10);
            flint_value_free(d);
        }
    }

    // ---- sequences of mappings
    {
        FlintParseError e2 = { 0, 0, NULL };
        FlintValue* d = flint_yaml_parse(DOC3, &e2);
        if (!d) { printf("FAIL steps doc: %s at %lld:%lld\n", e2.msg, (long long)e2.line, (long long)e2.col); fails++; }
        else {
            FlintValue* s = flint_value_obj_get(d, "steps");
            ck("steps is a sequence", s && s->type == FLT_ARR);
            ck("steps has 3", flint_value_len(s) == 3);
            if (flint_value_len(s) == 3) {
                ck("  step 0 name", strcmp(flint_value_as_str(flint_value_obj_get(flint_value_at(s, 0), "name"), ""), "build") == 0);
                ck("  step 2 run", strcmp(flint_value_as_str(flint_value_obj_get(flint_value_at(s, 2), "run"), ""), "make publish") == 0);
            }
            flint_value_free(d);
        }
    }

    // ---- the same-indent rule, and a sequence nested under a key
    {
        FlintParseError e2 = { 0, 0, NULL };
        FlintValue* d = flint_yaml_parse(DOC4, &e2);
        if (!d) { printf("FAIL list doc: %s at %lld:%lld\n", e2.msg, (long long)e2.line, (long long)e2.col); fails++; }
        else {
            ck("plain sequence", flint_value_len(flint_value_obj_get(d, "list")) == 3 &&
                                 flint_value_as_i64(flint_value_at(flint_value_obj_get(d, "list"), 2), 0) == 3);
            ck("sequence at the SAME indent as its key",
               flint_value_len(flint_value_obj_get(d, "same_indent")) == 2);
            ck("a key after a same-indent sequence is not swallowed",
               flint_value_as_i64(flint_value_obj_get(d, "after"), -1) == 1);
            ck("sequence of maps, nested under a key",
               flint_value_len(flint_value_obj_get(d, "nested")) == 2 &&
               flint_value_as_i64(flint_value_obj_get(flint_value_at(flint_value_obj_get(d, "nested"), 0), "y"), -1) == 2);
            flint_value_free(d);
        }
    }

    // ---- flow collections and block scalars
    {
        FlintParseError e2 = { 0, 0, NULL };
        FlintValue* d = flint_yaml_parse(DOC5, &e2);
        if (!d) { printf("FAIL flow doc: %s at %lld:%lld\n", e2.msg, (long long)e2.line, (long long)e2.col); fails++; }
        else {
            ck("flow map", flint_value_path_i64(d, "flow_map.a", -1) == 1 &&
                            strcmp(flint_value_path_str(d, "flow_map.b", ""), "two") == 0);
            ck("flow seq", flint_value_len(flint_value_obj_get(d, "flow_seq")) == 3);
            ck("flow nested", flint_value_path_i64(d, "flow_nested.list.1.deep", -1) == 1);
            const char* lit = flint_value_as_str(flint_value_obj_get(d, "literal"), "");
            ck("literal block keeps newlines", strchr(lit, '\n') != NULL && strstr(lit, "line one") != NULL);
            const char* fold = flint_value_as_str(flint_value_obj_get(d, "folded"), "");
            ck("folded block joins lines", strstr(fold, "joined line continues here") != NULL);
            ck("a key after a block scalar still parses",
               strcmp(flint_value_as_str(flint_value_obj_get(d, "after"), ""), "done") == 0);
            flint_value_free(d);
        }
    }

    // ---- document marker
    {
        FlintParseError e2 = { 0, 0, NULL };
        FlintValue* d = flint_yaml_parse("---\nkey: value\n", &e2);
        ck("--- document marker", d && strcmp(flint_value_as_str(flint_value_obj_get(d, "key"), ""), "value") == 0);
        flint_value_free(d);
    }

    // ---- errors
    {
        struct { const char* doc; const char* what; } bad[] = {
            { "\tkey: value\n",        "tab indentation is rejected" },
            { "key: [1, 2\n",         "unterminated flow sequence" },
            { "key: {a: 1\n",          "unterminated flow mapping" },
            { "key: \"unterminated\n",  "unterminated string" },
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            FlintParseError e2 = { 0, 0, NULL };
            FlintValue* r = flint_yaml_parse(bad[i].doc, &e2);
            ck(bad[i].what, r == NULL);
            flint_value_free(r);
        }
    }

    printf("yaml-check: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}