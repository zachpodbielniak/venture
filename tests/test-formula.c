/*
 * test-formula.c - user formulas on the sandboxed suet VM
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The claims, each with a way to be quietly false:
 *
 *   - a formula that compiles is accepted at save time, and one that does
 *     not is refused with the compiler's diagnostic, its line and column
 *     counted in the formula as the person typed it;
 *   - a formula reads its record through the accessors and yields what
 *     the expression says, for every type the accessors know;
 *   - a runaway formula (steps, memory, a panic) is an error for that
 *     evaluation, and the evaluator keeps working afterwards;
 *   - nothing a person types can leave the expression: no declaration,
 *     no closing of the wrapper, no file, no environment;
 *   - a formula is compiled once, the cache stays inside its bound, and
 *     one formula is safe to evaluate from several threads at once.
 */

#include <venture.h>

#include <string.h>

#include "formula/venture-formula-private.h"

/* every test starts from the defaults and an empty cache */
static void
reset(void)
{
	venture_formula_configure(NULL);
}

/* a condition that must evaluate, and its value */
static gboolean
eval_bool(
	const gchar	*expression,
	const gchar	*record
){
	g_autoptr(GError) error = NULL;
	gboolean result;
	gboolean ok;

	result = FALSE;
	ok = venture_formula_eval_bool(expression, record, &result, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	return result;
}

/* a score that must evaluate, and its value */
static gint64
eval_int(
	const gchar	*expression,
	const gchar	*record
){
	g_autoptr(GError) error = NULL;
	gint64 result;
	gboolean ok;

	result = -1;
	ok = venture_formula_eval_int(expression, record, &result, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	return result;
}

/* --- Compiling ------------------------------------------------------------- */

static void
test_check_ok(void)
{
	g_autoptr(GError) error = NULL;

	reset();
	g_assert_true(venture_formula_check("i64_of(r, \"amount\") > 100", VENTURE_FORMULA_KIND_BOOL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_formula_check("when(has_field(r, \"email\"), 10) + i64_of(r, \"employees\") / 100",
	                                    VENTURE_FORMULA_KIND_INT, &error));
	g_assert_no_error(error);

	/* several lines, a comment, a match and an interpolation are fine */
	g_assert_true(venture_formula_check("/* big, or flagged */\n"
	                                    "match field_str(r, \"tier\") {\n"
	                                    "\tsome t => t == \"gold\" || \"${t}!\" == \"vip!\",\n"
	                                    "\tnone => bool_of(r, \"flagged\"),\n"
	                                    "}", VENTURE_FORMULA_KIND_BOOL, &error));
	g_assert_no_error(error);
}

static void
test_check_error_position(void)
{
	g_autoptr(GError) error = NULL;

	reset();

	/* an unknown name on the second line, third column */
	g_assert_false(venture_formula_check("has_field(r, \"a\") &&\n  zork", VENTURE_FORMULA_KIND_BOOL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "2:3:"));
	g_assert_nonnull(strstr(error->message, "zork"));
	g_assert_null(strstr(error->message, "venture_formula"));
	g_clear_error(&error);

	/* the precedence trap: `??` binds loosest, so this compares 0 > 3 */
	g_assert_false(venture_formula_check("field_i64(r, \"n\") ?? 0 > 3", VENTURE_FORMULA_KIND_BOOL, &error));
	g_assert_nonnull(strstr(error->message, "1:"));
	g_assert_nonnull(strstr(error->message, "E0401"));
	g_clear_error(&error);

	/* a score is not a condition, nor the other way round */
	g_assert_false(venture_formula_check("i64_of(r, \"n\")", VENTURE_FORMULA_KIND_BOOL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_true(venture_formula_check("i64_of(r, \"n\")", VENTURE_FORMULA_KIND_INT, &error));
	g_assert_no_error(error);

	/* an empty formula */
	g_assert_false(venture_formula_check("  \n ", VENTURE_FORMULA_KIND_BOOL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* evaluating a formula that does not compile is the same refusal */
	{
		gboolean result = TRUE;

		g_assert_false(venture_formula_eval_bool("nope(r)", "{}", &result, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_false(result);
	}
}

/* --- Evaluating ------------------------------------------------------------ */

static void
test_eval_bool(void)
{
	const gchar *record;

	reset();
	record = "{\"amount\": 250, \"name\": \"Acme\", \"rate\": 1.5, \"open\": true,"
	         " \"count\": \"42\", \"colors\": [\"red\", \"blue\"], \"empty\": \"\","
	         " \"none\": null, \"nested\": {\"a\": 1}}";

	g_assert_true(eval_bool("i64_of(r, \"amount\") > 100", record));
	g_assert_false(eval_bool("i64_of(r, \"amount\") > 1000", record));
	g_assert_true(eval_bool("str_of(r, \"name\") == \"Acme\"", record));
	g_assert_true(eval_bool("str_of(r, \"name\").lower().starts_with(\"ac\")", record));
	g_assert_true(eval_bool("f64_of(r, \"rate\") > 1.25", record));
	g_assert_true(eval_bool("bool_of(r, \"open\")", record));
	g_assert_true(eval_bool("i64_of(r, \"count\") == 42", record));
	g_assert_true(eval_bool("field_has(r, \"colors\", \"blue\")", record));
	g_assert_false(eval_bool("field_has(r, \"colors\", \"green\")", record));
	g_assert_true(eval_bool("field_has(r, \"name\", \"Acme\")", record));

	/* absent, null, empty and of another type */
	g_assert_false(eval_bool("has_field(r, \"missing\")", record));
	g_assert_false(eval_bool("has_field(r, \"none\")", record));
	g_assert_false(eval_bool("has_field(r, \"empty\")", record));
	g_assert_true(eval_bool("has_field(r, \"colors\")", record));
	g_assert_true(eval_bool("field_i64(r, \"name\") == none", record));
	g_assert_true(eval_bool("field_str(r, \"nested\") == none", record));
	g_assert_true(eval_bool("field_bool(r, \"amount\") == none", record));
	g_assert_true(eval_bool("i64_of(r, \"missing\") == 0", record));

	/* a record that is not an object reads as empty */
	g_assert_false(eval_bool("has_field(r, \"a\")", "[1, 2]"));
	g_assert_false(eval_bool("has_field(r, \"a\")", "not json"));
}

static void
test_eval_score(void)
{
	reset();
	g_assert_cmpint(eval_int("when(str_of(r, \"source\") == \"web\", 15)", "{\"source\": \"web\"}"), ==, 15);
	g_assert_cmpint(eval_int("when(str_of(r, \"source\") == \"web\", 15)", "{\"source\": \"ad\"}"), ==, 0);
	g_assert_cmpint(eval_int("i64_of(r, \"employees\") / 100 - 3", "{\"employees\": 1250}"), ==, 9);
	g_assert_cmpint(eval_int("-20", "{}"), ==, -20);
}

static void
test_eval_kernels(void)
{
	/* a compiled kernel through `venture_kernels`, as a plugin calls it */
	reset();
	g_assert_true(eval_bool("billing_rate_bp(i64_of(r, \"won\"), i64_of(r, \"total\")).is_some()",
	                        "{\"won\": 1, \"total\": 4}"));
}

/* --- Limits ------------------------------------------------------------------ */

static void
test_limit_steps(void)
{
	g_autoptr(GError) error = NULL;
	gboolean result;

	reset();

	/* a block in a match arm may loop; the step cap ends it */
	result = TRUE;
	g_assert_false(venture_formula_eval_bool(
		"match true {\n"
		"\ttrue => { var i = 0; while i >= 0 { i += 1; } i > 0 },\n"
		"\tfalse => false,\n"
		"}", "{}", &result, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_test_message("steps: %s", error->message);
	g_assert_nonnull(strstr(error->message, "instructions"));
	g_assert_false(result);
	g_clear_error(&error);

	/* and the evaluator goes on */
	g_assert_true(eval_bool("i64_of(r, \"n\") == 1", "{\"n\": 1}"));
}

static void
test_limit_memory(void)
{
	g_autoptr(GError) error = NULL;
	gint64 result;

	reset();

	/* a string doubling until the heap cap stops it, inside the step cap */
	result = 1;
	g_assert_false(venture_formula_eval_int(
		"match true {\n"
		"\ttrue => { var s: own str = \"xxxxxxxxxxxxxxxx\".dup(); var n = 0;\n"
		"\t\twhile n < 64 { s = \"${s}${s}\"; n += 1; } s.len() },\n"
		"\tfalse => 0,\n"
		"}", "{}", &result, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_test_message("memory: %s", error->message);
	g_assert_nonnull(strstr(error->message, "memory"));
	g_assert_cmpint(result, ==, 0);
	g_clear_error(&error);

	g_assert_cmpint(eval_int("7", "{}"), ==, 7);
}

static void
test_panic(void)
{
	g_autoptr(GError) error = NULL;
	gboolean result;

	reset();
	result = TRUE;
	g_assert_false(venture_formula_eval_bool("field_i64(r, \"missing\").unwrap() > 0", "{}", &result, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_test_message("panic: %s", error->message);
	g_clear_error(&error);

	/* the same formula on a record that has the field is fine */
	g_assert_true(eval_bool("field_i64(r, \"missing\").unwrap() > 0", "{\"missing\": 3}"));
}

/* --- Escaping ------------------------------------------------------------------ */

static void
test_injection(void)
{
	/* each tries to leave the expression, or to reach the outside */
	static const gchar *const hostile[] = {
		"true); } fn main() { print(\"x\"); } fn other() -> bool { return (true",
		"true }",
		"true; false",
		"true)",
		"(true",
		"\"open string",
		"true /* never closed",
		"true /* nested /* */ */",
		"true // line comment",
		"$(cat /etc/passwd).len() > 0",
		"\"\"\"heredoc\"\"\" == \"\"",
		"`id` == \"\"",
		"\"${ \"x\" \" == \"\"",
		"'ab' == 'a'",
		"read_file(\"/etc/passwd\").len() > 0",
		"getenv(\"HOME\") == none",
		"run_command(\"id\").len() > 0",
		NULL
	};
	guint i;

	reset();
	for (i = 0; NULL != hostile[i]; i++)
	{
		g_autoptr(GError) error = NULL;

		if (venture_formula_check(hostile[i], VENTURE_FORMULA_KIND_BOOL, &error))
			g_error("accepted a formula that leaves the expression: %s", hostile[i]);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}

	/* a closing brace inside a string, or an interpolation, is just text */
	g_assert_true(eval_bool("\"}\" == \"}\" && \"${\"}\"}\" == \"}\"", "{}"));

	/* and a block may hold statements: the `;` is inside its braces */
	g_assert_true(eval_bool("match true { true => { let x = 2; x > 1 }, false => false }", "{}"));
}

/* --- The cache ------------------------------------------------------------------ */

static void
test_cache_reuse(void)
{
	g_autoptr(GError) error = NULL;
	guint64 compiles;

	reset();
	g_assert_cmpuint(venture_formula_cache_size(), ==, 0);
	compiles = venture_formula_compile_count();

	/* checked at save, then evaluated: one compile */
	g_assert_true(venture_formula_check("i64_of(r, \"a\") > 1", VENTURE_FORMULA_KIND_BOOL, &error));
	g_assert_no_error(error);
	g_assert_true(eval_bool("i64_of(r, \"a\") > 1", "{\"a\": 2}"));
	g_assert_false(eval_bool("i64_of(r, \"a\") > 1", "{\"a\": 0}"));
	g_assert_cmpuint(venture_formula_compile_count(), ==, compiles + 1);
	g_assert_cmpuint(venture_formula_cache_size(), ==, 1);

	/* the same text as a score is another program */
	g_assert_false(venture_formula_check("i64_of(r, \"a\") > 1", VENTURE_FORMULA_KIND_INT, &error));
	g_clear_error(&error);
	g_assert_cmpuint(venture_formula_compile_count(), ==, compiles + 2);

	/* a refusal is cached too */
	g_assert_false(venture_formula_check("i64_of(r, \"a\") > 1", VENTURE_FORMULA_KIND_INT, &error));
	g_clear_error(&error);
	g_assert_cmpuint(venture_formula_compile_count(), ==, compiles + 2);
}

static void
test_cache_bound(void)
{
	g_autoptr(VentureConfig) config = NULL;
	guint i;

	config = venture_config_new();
	g_object_set(config, "formulas-cache-size", (gint64)3, NULL);
	venture_formula_configure(config);

	for (i = 0; i < 10; i++)
	{
		g_autofree gchar *text = g_strdup_printf("i64_of(r, \"a\") > %u", i);

		g_assert_true(eval_bool(text, "{\"a\": 100}"));
		g_assert_cmpuint(venture_formula_cache_size(), <=, 3);
	}
	g_assert_cmpuint(venture_formula_cache_size(), ==, 3);
	reset();
}

static void
test_allow_off(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	gboolean result;

	config = venture_config_new();
	g_object_set(config, "formulas-allow", FALSE, NULL);
	venture_formula_configure(config);

	g_assert_false(venture_formula_check("true", VENTURE_FORMULA_KIND_BOOL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "formulas.allow"));
	g_clear_error(&error);
	g_assert_false(venture_formula_eval_bool("true", "{}", &result, &error));
	g_clear_error(&error);
	reset();
}

static void
test_configured_steps(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *loop;
	gint64 result;

	/* a loop of a few thousand steps passes the default cap and not a
	 * tiny configured one (a gint64 setting, read as one) */
	loop = "match true { true => { var i = 0; while i < 500 { i += 1; } i }, false => 0 }";
	reset();
	g_assert_cmpint(eval_int(loop, "{}"), ==, 500);

	config = venture_config_new();
	g_object_set(config, "formulas-max-steps", (gint64)200, NULL);
	venture_formula_configure(config);
	g_assert_false(venture_formula_eval_int(loop, "{}", &result, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	reset();
}

/* --- Threads ------------------------------------------------------------------ */

typedef struct
{
	guint	seed;
	guint	wrong;
} Worker;

static gpointer
worker_run(gpointer data)
{
	Worker *worker;
	guint i;

	worker = data;
	for (i = 0; i < 200; i++)
	{
		g_autofree gchar *record = NULL;
		gint64 n;
		gint64 score;
		gboolean big;

		n = (gint64)((worker->seed * 7919u + i * 31u) % 1000u);
		record = g_strdup_printf("{\"n\": %" G_GINT64_FORMAT "}", n);
		big = FALSE;
		score = -1;
		if (!venture_formula_eval_bool("i64_of(r, \"n\") >= 500", record, &big, NULL) ||
		    big != (n >= 500))
			worker->wrong++;
		if (!venture_formula_eval_int("i64_of(r, \"n\") * 2 + 1", record, &score, NULL) ||
		    score != n * 2 + 1)
			worker->wrong++;
	}
	return NULL;
}

static void
test_threads(void)
{
	GThread *threads[8];
	Worker workers[8];
	guint64 compiles;
	guint i;

	reset();
	compiles = venture_formula_compile_count();
	for (i = 0; i < G_N_ELEMENTS(threads); i++)
	{
		workers[i].seed = i + 1;
		workers[i].wrong = 0;
		threads[i] = g_thread_new("formula", worker_run, &workers[i]);
	}
	for (i = 0; i < G_N_ELEMENTS(threads); i++)
	{
		g_thread_join(threads[i]);
		g_assert_cmpuint(workers[i].wrong, ==, 0);
	}

	/* two formulas; racing threads may each compile one, but the cache
	 * keeps one of each */
	g_assert_cmpuint(venture_formula_cache_size(), ==, 2);
	g_assert_cmpuint(venture_formula_compile_count() - compiles, <=, 2 * G_N_ELEMENTS(threads));
}

/* The fields a formula reads are the literal names its accessors are
 * called with: a key compared as a value, quoted in a comment or written
 * as plain string text is not one; an interpolation is code; a name built
 * at run time makes the set unknowable. */
static void
test_fields(void)
{
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(GError) error = NULL;
	gboolean dynamic;

	dynamic = TRUE;
	fields = venture_formula_fields(
		"i64_of(r, \"seats\") > 10 && r.str_of(\"kind\") == \"plan\" /* field_str(r, \"comment\") */\n"
		"&& \"${str_of(r, \"team\")}\".len() > 0 && \"field_str(r, \\\"text\\\")\" != \"\"\n"
		"&& field_has(r, \"seats\", \"many\") && when(has_field(r, \"team\"), 1) > 0",
		&dynamic, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fields);
	g_assert_false(dynamic);
	g_assert_cmpuint(fields->len, ==, 3);
	g_assert_cmpstr(g_ptr_array_index(fields, 0), ==, "seats");
	g_assert_cmpstr(g_ptr_array_index(fields, 1), ==, "kind");
	g_assert_cmpstr(g_ptr_array_index(fields, 2), ==, "team");
	g_clear_pointer(&fields, g_ptr_array_unref);

	/* a parenthesised name is not a plain literal: only a bare string counts */
	dynamic = FALSE;
	fields = venture_formula_fields("has_field(r, (\"nested\"))", &dynamic, &error);
	g_assert_no_error(error);
	g_assert_true(dynamic);
	g_clear_pointer(&fields, g_ptr_array_unref);

	dynamic = FALSE;
	fields = venture_formula_fields("str_of(r, \"ki\" + \"nd\") == \"a\"", &dynamic, &error);
	g_assert_no_error(error);
	g_assert_true(dynamic);
	g_assert_cmpuint(fields->len, ==, 0);
	g_clear_pointer(&fields, g_ptr_array_unref);

	dynamic = FALSE;
	fields = venture_formula_fields("str_of(r, \"${1}\") == \"a\"", &dynamic, &error);
	g_assert_no_error(error);
	g_assert_true(dynamic);
	g_clear_pointer(&fields, g_ptr_array_unref);

	/* not shaped as a formula: refused like a save would be */
	fields = venture_formula_fields("true) }", NULL, &error);
	g_assert_null(fields);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/formula/check/ok", test_check_ok);
	g_test_add_func("/formula/check/error-position", test_check_error_position);
	g_test_add_func("/formula/eval/bool", test_eval_bool);
	g_test_add_func("/formula/eval/score", test_eval_score);
	g_test_add_func("/formula/eval/kernels", test_eval_kernels);
	g_test_add_func("/formula/limit/steps", test_limit_steps);
	g_test_add_func("/formula/limit/memory", test_limit_memory);
	g_test_add_func("/formula/limit/panic", test_panic);
	g_test_add_func("/formula/limit/configured-steps", test_configured_steps);
	g_test_add_func("/formula/injection", test_injection);
	g_test_add_func("/formula/cache/reuse", test_cache_reuse);
	g_test_add_func("/formula/cache/bound", test_cache_bound);
	g_test_add_func("/formula/allow-off", test_allow_off);
	g_test_add_func("/formula/threads", test_threads);
	g_test_add_func("/formula/fields", test_fields);

	return g_test_run();
}
