/*
 * venture-formula.c - user formulas on the sandboxed suet VM
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The life of a formula:
 *
 *   1. its shape is checked (formula_check_shape): one expression, its
 *      brackets, strings and comments balanced, no `;` outside a block --
 *      so nothing in it can close the wrapper around it and declare a
 *      function, an import or a `main` of its own;
 *   2. it is wrapped (formula_wrap) in a module that imports `formula`
 *      (the accessors, src/formula/venture-formula.tlw) and
 *      `venture_kernels`, as the body of `pure fn venture_formula(r: str)`;
 *   3. a sandboxed VM compiles it (formula_compile): no capability, no
 *      GIR, the `formulas` limits; a diagnostic comes back with its line
 *      and column counted in the formula, not the wrapper; the program
 *      must then hold exactly the one function with exactly the expected
 *      signature, a second guard against escaping the wrapper;
 *   4. the VM is cached by (kind, text), at most `formulas.cache_size` of
 *      them, the least recently used going first; a failure to compile is
 *      cached too, so a broken formula is not compiled on every record;
 *   5. an evaluation borrows the entry (a reference, so eviction cannot
 *      free it underneath) and calls the VM under the entry's lock: a VM
 *      runs one call at a time, and the server has parallel workers.
 */

#include "venture.h"
#include "formula/venture-formula-private.h"
#include "plugin/venture-tallow-runtime-private.h"

#include <math.h>
#include <string.h>

/* the `formula` module, embedded by rules.mk */
#include "venture-formula-module.h"

/* the function the wrapper defines, and the file name diagnostics carry */
#define FORMULA_FUNCTION "venture_formula"
#define FORMULA_FILE "expression"

/* the line of the wrapper the formula's first line lands on */
#define FORMULA_FIRST_LINE (5)

/* defaults, as data/default-config.yaml has them */
#define FORMULA_DEFAULT_STEPS (100000)
#define FORMULA_DEFAULT_MEMORY (G_GINT64_CONSTANT(8) * 1024 * 1024)
#define FORMULA_DEFAULT_CACHE (256)
#define FORMULA_MAX_DEPTH (1000)

/* --- State ------------------------------------------------------------------ */

/*
 * One compiled formula. Reference counted: the cache holds one reference
 * and every evaluation in flight another, so an entry evicted while it
 * runs is freed by whoever finishes last.
 */
typedef struct
{
	gatomicrefcount	 ref;
	TallowSuet	*vm;		/* NULL when it did not compile */
	gchar		*refusal;	/* why it did not compile, else NULL */
	GMutex		 lock;		/* one call at a time on the VM */
	guint64		 used;		/* the cache's clock at its last use */
} FormulaEntry;

/* The settings and the cache: one per process, under one lock. */
typedef struct
{
	GMutex		 lock;
	gboolean	 configured;
	gboolean	 allow;
	gint64		 max_steps;
	gint64		 max_memory;
	guint		 cache_size;
	gchar		*std_dir;
	GHashTable	*cache;		/* "K\nTEXT" -> FormulaEntry */
	guint64		 clock;
	guint64		 compiles;
} FormulaState;

static FormulaState formula_state;

/* Compiling goes through the Tallow compiler, whose checker is not
 * documented as safe to run on two threads at once: one at a time. */
static GMutex formula_compile_lock;

static FormulaEntry *
formula_entry_ref(FormulaEntry *self)
{
	g_atomic_ref_count_inc(&self->ref);
	return self;
}

static void
formula_entry_unref(gpointer data)
{
	FormulaEntry *self;

	self = data;
	if (NULL == self || !g_atomic_ref_count_dec(&self->ref))
		return;
	g_clear_object(&self->vm);
	g_free(self->refusal);
	g_mutex_clear(&self->lock);
	g_free(self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FormulaEntry, formula_entry_unref)

/* the settings, read once on first use when nobody configured them */
static void
formula_state_ensure(void)
{
	if (formula_state.configured)
		return;

	formula_state.configured = TRUE;
	formula_state.allow = TRUE;
	formula_state.max_steps = FORMULA_DEFAULT_STEPS;
	formula_state.max_memory = FORMULA_DEFAULT_MEMORY;
	formula_state.cache_size = FORMULA_DEFAULT_CACHE;
	formula_state.std_dir = venture_tallow_std_dir(NULL);
	formula_state.cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                            formula_entry_unref);
}

void
venture_formula_configure(VentureConfig *config)
{
	gboolean allow;
	gint64 max_steps;
	gint64 max_memory;
	gint64 cache_size;

	/* the settings are gint64 (VC_INT); reading one as gint would leave
	 * half of it unset */
	allow = TRUE;
	max_steps = FORMULA_DEFAULT_STEPS;
	max_memory = FORMULA_DEFAULT_MEMORY;
	cache_size = FORMULA_DEFAULT_CACHE;
	if (NULL != config)
		g_object_get(config, "formulas-allow", &allow,
		             "formulas-max-steps", &max_steps,
		             "formulas-max-memory", &max_memory,
		             "formulas-cache-size", &cache_size, NULL);

	g_mutex_lock(&formula_state.lock);
	formula_state_ensure();
	formula_state.allow = allow;

	/* a formula is never unbounded: 0 or less means the default */
	formula_state.max_steps = (max_steps > 0) ? max_steps : FORMULA_DEFAULT_STEPS;
	formula_state.max_memory = (max_memory > 0) ? max_memory : FORMULA_DEFAULT_MEMORY;
	formula_state.cache_size = (guint)CLAMP(cache_size, 1, 65536);
	g_free(formula_state.std_dir);
	formula_state.std_dir = venture_tallow_std_dir(config);

	/* new limits, new VMs: whatever was compiled under the old ones goes */
	g_hash_table_remove_all(formula_state.cache);
	g_mutex_unlock(&formula_state.lock);
}

guint
venture_formula_cache_size(void)
{
	guint size;

	g_mutex_lock(&formula_state.lock);
	formula_state_ensure();
	size = g_hash_table_size(formula_state.cache);
	g_mutex_unlock(&formula_state.lock);
	return size;
}

guint64
venture_formula_compile_count(void)
{
	guint64 count;

	g_mutex_lock(&formula_state.lock);
	count = formula_state.compiles;
	g_mutex_unlock(&formula_state.lock);
	return count;
}

/* --- The shape of a formula ------------------------------------------------ */

/* the 1-based line and column of P in TEXT, in characters */
static void
formula_position(
	const gchar	*text,
	const gchar	*p,
	guint		*line,
	guint		*column
){
	const gchar *q;

	*line = 1;
	*column = 1;
	for (q = text; q < p && '\0' != *q; q = g_utf8_next_char(q))
	{
		if ('\n' == *q)
		{
			(*line)++;
			*column = 1;
		}
		else
			(*column)++;
	}
}

/* a refusal pointing at P */
static gboolean
formula_refuse_at(
	GError		**error,
	const gchar	 *text,
	const gchar	 *p,
	const gchar	 *what
){
	guint line;
	guint column;

	formula_position(text, p, &line, &column);
	venture_set_error_validation(error, "Expression", "%u:%u: %s", line, column, what);
	return FALSE;
}

/* the closing bracket of an opener on the shape stack */
static gchar
formula_closer(gchar opener)
{
	switch (opener)
	{
	case '(':
		return ')';
	case '[':
		return ']';
	default:
		return '}';
	}
}

/*
 * Checks that TEXT is one expression that stays inside the wrapper's
 * `return ( ... );`. A small lexer walks it with a stack of what is open:
 * `(`, `[`, `{`, a string `S`, and an interpolation `I` (`${` inside a
 * string, closed by `}`). Every closer must match the innermost opener, a
 * `;` may appear only inside a block, and everything must be closed at
 * the end -- so a `)` or `}` meant to end the wrapper's expression or
 * function is refused here, before the compiler sees it. Comments must
 * close and must not nest (whether the compiler would nest them is then
 * moot); a heredoc (`"""`), a command (`$(`) and a backtick are refused
 * outright: none belongs in a formula, and each has lexing rules of its
 * own this walk does not follow.
 */
static gboolean
formula_check_shape(
	const gchar	 *text,
	GError		**error
){
	g_autoptr(GString) open = NULL;
	const gchar *p;
	gboolean blank;

	if (NULL == text)
		return formula_refuse_at(error, "", "", "an expression is required");
	if (strlen(text) > VENTURE_FORMULA_MAX_LENGTH)
	{
		venture_set_error_validation(error, "Expression", "at most %d bytes",
		                             VENTURE_FORMULA_MAX_LENGTH);
		return FALSE;
	}
	if (!g_utf8_validate(text, -1, NULL))
		return formula_refuse_at(error, text, text, "the expression is not UTF-8 text");

	open = g_string_new(NULL);
	blank = TRUE;
	p = text;
	while ('\0' != *p)
	{
		gchar top;

		top = (open->len > 0) ? open->str[open->len - 1] : '\0';

		/* inside a string: escapes, its end, and interpolations */
		if ('S' == top)
		{
			if ('\\' == *p)
			{
				if ('\0' == p[1])
					break;
				p += 2;
			}
			else if ('"' == *p)
			{
				g_string_truncate(open, open->len - 1);
				p++;
			}
			else if ('$' == *p && '{' == p[1])
			{
				g_string_append_c(open, 'I');
				p += 2;
			}
			else
				p++;
			continue;
		}

		if (!g_ascii_isspace(*p))
			blank = FALSE;

		/* comments: closed, and not nested */
		if ('/' == *p && '*' == p[1])
		{
			const gchar *end;
			const gchar *inner;

			end = strstr(p + 2, "*/");
			inner = strstr(p + 2, "/*");
			if (NULL == end)
				return formula_refuse_at(error, text, p, "a comment is not closed");
			if (NULL != inner && inner < end)
				return formula_refuse_at(error, text, inner, "comments do not nest");
			p = end + 2;
			continue;
		}
		if ('/' == *p && '/' == p[1])
			return formula_refuse_at(error, text, p, "comments are /* ... */");

		switch (*p)
		{
		case '"':
			if ('"' == p[1] && '"' == p[2])
				return formula_refuse_at(error, text, p, "a heredoc has no place in a formula");
			g_string_append_c(open, 'S');
			p++;
			break;

		case '\'':
		{
			const gchar *q;

			/* a character literal: one character or one escape */
			q = p + 1;
			if ('\\' == *q)
				q += ('x' == q[1] && '\0' != q[2] && '\0' != q[3]) ? 4 : (('\0' != q[1]) ? 2 : 1);
			else if ('\0' != *q)
				q = g_utf8_next_char(q);
			if ('\'' != *q)
				return formula_refuse_at(error, text, p, "a character literal is not closed");
			p = q + 1;
			break;
		}

		case '`':
			return formula_refuse_at(error, text, p, "a backtick has no place in a formula");

		case '$':
			if ('(' == p[1])
				return formula_refuse_at(error, text, p, "a formula cannot run commands");
			p++;
			break;

		case '(':
		case '[':
		case '{':
			g_string_append_c(open, *p);
			p++;
			break;

		case ')':
		case ']':
		case '}':
			if ('}' == *p && 'I' == top)
			{
				/* the end of an interpolation: back into its string */
				g_string_truncate(open, open->len - 1);
				p++;
				break;
			}
			if ('\0' == top || 'I' == top || formula_closer(top) != *p)
				return formula_refuse_at(error, text, p,
					"this closes something the expression did not open");
			g_string_truncate(open, open->len - 1);
			p++;
			break;

		case ';':
			if (0 == open->len)
				return formula_refuse_at(error, text, p,
					"a formula is one expression: `;` only inside a { block }");
			p++;
			break;

		default:
			p++;
			break;
		}
	}

	if (open->len > 0)
	{
		gchar top;

		top = open->str[open->len - 1];
		return formula_refuse_at(error, text, p,
			('S' == top) ? "a string is not closed" :
			('I' == top) ? "an interpolation ${...} is not closed" :
			"a bracket is not closed");
	}
	if (blank)
		return formula_refuse_at(error, text, text, "an expression is required");

	return TRUE;
}

/* --- The wrapper ------------------------------------------------------------- */

/* the Tallow type a formula of KIND yields */
static const gchar *
formula_type(VentureFormulaKind kind)
{
	return (VENTURE_FORMULA_KIND_INT == kind) ? "i64" : "bool";
}

/*
 * The module a formula compiles as. The expression starts at column 1 of
 * line FORMULA_FIRST_LINE, so a diagnostic's column is already the
 * formula's and its line is off by a constant.
 */
static gchar *
formula_wrap(
	const gchar		*expression,
	VentureFormulaKind	 kind
){
	return g_strdup_printf("import formula;\n"
	                       "import venture_kernels;\n"
	                       "pure fn " FORMULA_FUNCTION "(r: str) -> %s {\n"
	                       "\treturn (\n"
	                       "%s\n"
	                       "\t);\n"
	                       "}\n", formula_type(kind), expression);
}

/* the number of lines in TEXT */
static guint
formula_line_count(const gchar *text)
{
	guint lines;

	for (lines = 1; '\0' != *text; text++)
	{
		if ('\n' == *text)
			lines++;
	}
	return lines;
}

typedef struct
{
	guint	lines;	/* how many lines the formula has */
} FormulaRemap;

/* `formula:LINE:COL` in the wrapper -> `LINE:COL` in the formula, or
 * `end` for a position on the wrapper's own lines (a type mismatch is
 * reported at the closing parenthesis) */
static gboolean
formula_remap_position(
	const GMatchInfo	*match,
	GString			*out,
	gpointer		 user_data
){
	FormulaRemap *remap;
	g_autofree gchar *line_text = NULL;
	g_autofree gchar *column_text = NULL;
	gint64 line;

	remap = user_data;
	line_text = g_match_info_fetch(match, 1);
	column_text = g_match_info_fetch(match, 2);
	line = g_ascii_strtoll(line_text, NULL, 10) - (FORMULA_FIRST_LINE - 1);

	if (line >= 1 && line <= (gint64)remap->lines)
		g_string_append_printf(out, "%" G_GINT64_FORMAT ":%s", line, column_text);
	else
		g_string_append(out, "end");
	return FALSE;
}

/*
 * The compiler's report, told about the formula instead of the wrapper:
 * positions moved, and the source excerpts (which quote wrapper lines a
 * person never wrote) dropped, keeping the diagnostics and their notes.
 */
static gchar *
formula_diagnostic(
	const gchar	*expression,
	const gchar	*message
){
	g_autoptr(GRegex) position = NULL;
	g_autoptr(GString) kept = NULL;
	g_auto(GStrv) lines = NULL;
	FormulaRemap remap;
	guint i;

	remap.lines = formula_line_count(expression);
	position = g_regex_new("(?:[^\\s:]*/)?" FORMULA_FILE "(?:\\.tlw)?:(\\d+):(\\d+)", 0, 0, NULL);

	/* the lines that say something: a diagnostic or a note, not an
	 * excerpt (`  12 | ...`) or a caret (`     ^^^`) */
	kept = g_string_new(NULL);
	lines = g_strsplit(message, "\n", -1);
	for (i = 0; NULL != lines[i]; i++)
	{
		g_autofree gchar *moved = NULL;
		const gchar *line;

		line = lines[i];
		if (NULL == strstr(line, FORMULA_FILE ":") && NULL == strstr(line, "error") &&
		    NULL == strstr(line, "note") && NULL == strstr(line, "help"))
			continue;
		moved = g_regex_replace_eval(position, line, -1, 0, 0, formula_remap_position,
		                             &remap, NULL);
		if (NULL == moved)
			continue;
		g_strstrip(moved);
		if ('\0' == *moved)
			continue;
		if (kept->len > 0)
			g_string_append_c(kept, '\n');
		g_string_append(kept, moved);
	}

	if (0 == kept->len)
		g_string_append(kept, message);
	return g_string_free(g_steal_pointer(&kept), FALSE);
}

/* --- The accessors (`formula` host functions) ------------------------------ */

/* the member NAME of the record ARGS[0], or NULL; *PARSER keeps it alive */
static JsonNode *
formula_member(
	const GValue	 *args,
	JsonParser	**out_parser
){
	g_autoptr(JsonParser) parser = NULL;
	const gchar *json;
	const gchar *name;
	JsonNode *root;
	JsonObject *object;
	JsonNode *member;

	json = g_value_get_string(&args[0]);
	name = g_value_get_string(&args[1]);
	if (NULL == json || NULL == name)
		return NULL;

	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, json, -1, NULL))
		return NULL;
	root = json_parser_get_root(parser);
	if (NULL == root || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;
	object = json_node_get_object(root);
	if (!json_object_has_member(object, name))
		return NULL;

	member = json_object_get_member(object, name);
	if (JSON_NODE_HOLDS_NULL(member))
		return NULL;
	*out_parser = g_steal_pointer(&parser);
	return member;
}

/* the text of a JSON scalar: a string's value, else as written */
static gchar *
formula_scalar_text(JsonNode *node)
{
	if (NULL == node || !JSON_NODE_HOLDS_VALUE(node))
		return NULL;
	if (G_TYPE_STRING == json_node_get_value_type(node))
		return g_strdup(json_node_get_string(node));
	return json_to_string(node, FALSE);
}

/* formula.field_str(r, name) -> own ?str */
static gboolean
formula_field_str(
	TallowSuet	*vm,
	const GValue	*args,
	guint		 n_args,
	GValue		*result,
	gpointer	 user_data,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;
	gchar *text;

	(void)vm;
	(void)user_data;
	(void)error;
	g_return_val_if_fail(2 == n_args, FALSE);

	text = formula_scalar_text(formula_member(args, &parser));
	if (NULL == text)
		return TRUE;
	g_value_init(result, G_TYPE_STRING);
	g_value_take_string(result, text);
	return TRUE;
}

/* formula.field_i64(r, name) -> ?i64: an integer, or a string of one */
static gboolean
formula_field_i64(
	TallowSuet	*vm,
	const GValue	*args,
	guint		 n_args,
	GValue		*result,
	gpointer	 user_data,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *member;
	gint64 value;

	(void)vm;
	(void)user_data;
	(void)error;
	g_return_val_if_fail(2 == n_args, FALSE);

	member = formula_member(args, &parser);
	if (NULL == member || !JSON_NODE_HOLDS_VALUE(member))
		return TRUE;

	if (G_TYPE_INT64 == json_node_get_value_type(member))
		value = json_node_get_int(member);
	else if (G_TYPE_STRING == json_node_get_value_type(member))
	{
		g_autofree gchar *text = g_strstrip(g_strdup(json_node_get_string(member)));

		if (!g_ascii_string_to_signed(text, 10, G_MININT64, G_MAXINT64, &value, NULL))
			return TRUE;
	}
	else
		return TRUE;

	g_value_init(result, G_TYPE_INT64);
	g_value_set_int64(result, value);
	return TRUE;
}

/* formula.field_f64(r, name) -> ?f64: a number, or a string of one */
static gboolean
formula_field_f64(
	TallowSuet	*vm,
	const GValue	*args,
	guint		 n_args,
	GValue		*result,
	gpointer	 user_data,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *member;
	gdouble value;
	GType type;

	(void)vm;
	(void)user_data;
	(void)error;
	g_return_val_if_fail(2 == n_args, FALSE);

	member = formula_member(args, &parser);
	if (NULL == member || !JSON_NODE_HOLDS_VALUE(member))
		return TRUE;

	type = json_node_get_value_type(member);
	if (G_TYPE_INT64 == type)
		value = (gdouble)json_node_get_int(member);
	else if (G_TYPE_DOUBLE == type)
		value = json_node_get_double(member);
	else if (G_TYPE_STRING == type)
	{
		g_autofree gchar *text = g_strstrip(g_strdup(json_node_get_string(member)));
		gchar *end;

		end = NULL;
		value = g_ascii_strtod(text, &end);
		if ('\0' == *text || NULL == end || '\0' != *end)
			return TRUE;
	}
	else
		return TRUE;

	if (!isfinite(value))
		return TRUE;
	g_value_init(result, G_TYPE_DOUBLE);
	g_value_set_double(result, value);
	return TRUE;
}

/* formula.field_bool(r, name) -> ?bool */
static gboolean
formula_field_bool(
	TallowSuet	*vm,
	const GValue	*args,
	guint		 n_args,
	GValue		*result,
	gpointer	 user_data,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *member;
	gboolean value;

	(void)vm;
	(void)user_data;
	(void)error;
	g_return_val_if_fail(2 == n_args, FALSE);

	member = formula_member(args, &parser);
	if (NULL == member || !JSON_NODE_HOLDS_VALUE(member))
		return TRUE;

	if (G_TYPE_BOOLEAN == json_node_get_value_type(member))
		value = json_node_get_boolean(member);
	else if (G_TYPE_STRING == json_node_get_value_type(member) &&
	         0 == g_strcmp0(json_node_get_string(member), "true"))
		value = TRUE;
	else if (G_TYPE_STRING == json_node_get_value_type(member) &&
	         0 == g_strcmp0(json_node_get_string(member), "false"))
		value = FALSE;
	else
		return TRUE;

	g_value_init(result, G_TYPE_BOOLEAN);
	g_value_set_boolean(result, value);
	return TRUE;
}

/* formula.has_field(r, name) -> bool: present and not empty */
static gboolean
formula_has_field(
	TallowSuet	*vm,
	const GValue	*args,
	guint		 n_args,
	GValue		*result,
	gpointer	 user_data,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *member;
	gboolean present;

	(void)vm;
	(void)user_data;
	(void)error;
	g_return_val_if_fail(2 == n_args, FALSE);

	member = formula_member(args, &parser);
	present = (NULL != member);
	if (present && JSON_NODE_HOLDS_VALUE(member) &&
	    G_TYPE_STRING == json_node_get_value_type(member))
		present = ('\0' != *json_node_get_string(member));
	else if (present && JSON_NODE_HOLDS_ARRAY(member))
		present = (json_array_get_length(json_node_get_array(member)) > 0);

	g_value_init(result, G_TYPE_BOOLEAN);
	g_value_set_boolean(result, present);
	return TRUE;
}

/* formula.field_has(r, name, choice) -> bool: the string, or a list
 * holding it */
static gboolean
formula_field_has(
	TallowSuet	*vm,
	const GValue	*args,
	guint		 n_args,
	GValue		*result,
	gpointer	 user_data,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *member;
	const gchar *choice;
	gboolean found;

	(void)vm;
	(void)user_data;
	(void)error;
	g_return_val_if_fail(3 == n_args, FALSE);

	member = formula_member(args, &parser);
	choice = g_value_get_string(&args[2]);
	found = FALSE;
	if (NULL != member && NULL != choice && JSON_NODE_HOLDS_ARRAY(member))
	{
		JsonArray *list;
		guint i;

		list = json_node_get_array(member);
		for (i = 0; !found && i < json_array_get_length(list); i++)
		{
			JsonNode *item = json_array_get_element(list, i);

			found = JSON_NODE_HOLDS_VALUE(item) &&
			        G_TYPE_STRING == json_node_get_value_type(item) &&
			        0 == g_strcmp0(json_node_get_string(item), choice);
		}
	}
	else if (NULL != member && NULL != choice && JSON_NODE_HOLDS_VALUE(member) &&
	         G_TYPE_STRING == json_node_get_value_type(member))
		found = (0 == g_strcmp0(json_node_get_string(member), choice));

	g_value_init(result, G_TYPE_BOOLEAN);
	g_value_set_boolean(result, found);
	return TRUE;
}

static const VentureTallowHostEntry formula_functions[] = {
	{ "venture_formula_field_str",  formula_field_str },
	{ "venture_formula_field_i64",  formula_field_i64 },
	{ "venture_formula_field_f64",  formula_field_f64 },
	{ "venture_formula_field_bool", formula_field_bool },
	{ "venture_formula_has_field",  formula_has_field },
	{ "venture_formula_field_has",  formula_field_has },
	{ NULL, NULL }
};

/* --- Compiling ----------------------------------------------------------------- */

/* the settings a VM is made under, copied out of the state's lock */
typedef struct
{
	gint64	 max_steps;
	gint64	 max_memory;
	gchar	*std_dir;
} FormulaLimits;

/*
 * The program must be exactly the wrapper: one callable function, named
 * as the wrapper names it, taking a `str` and yielding the kind's type.
 * The shape check already keeps a formula inside the wrapper; this is
 * the second lock on the same door.
 */
static gboolean
formula_check_program(
	TallowSuet		 *vm,
	VentureFormulaKind	  kind,
	GError			**error
){
	g_auto(GStrv) functions = NULL;
	g_auto(GStrv) param_types = NULL;
	g_autofree gchar *result_type = NULL;
	gboolean fallible;

	functions = tallow_suet_list_functions(vm);
	fallible = FALSE;
	if (NULL == functions || NULL == functions[0] || NULL != functions[1] ||
	    0 != g_strcmp0(functions[0], FORMULA_FUNCTION) ||
	    !tallow_suet_get_function_signature(vm, FORMULA_FUNCTION, NULL, &param_types,
	                                        &result_type, &fallible, NULL) ||
	    NULL == param_types || NULL == param_types[0] || NULL != param_types[1] ||
	    0 != g_strcmp0(param_types[0], "str") ||
	    0 != g_strcmp0(result_type, formula_type(kind)) || fallible)
	{
		venture_set_error_validation(error, "Expression",
			"a formula is one expression; it cannot declare anything");
		return FALSE;
	}

	return TRUE;
}

/*
 * A VM holding EXPRESSION compiled, or NULL with the reason in *REFUSAL
 * (already told about the formula, not the wrapper).
 */
static TallowSuet *
formula_compile(
	const gchar		 *expression,
	VentureFormulaKind	  kind,
	const FormulaLimits	 *limits,
	gchar			**refusal
){
	g_autoptr(TallowSuet) vm = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *source = NULL;
	const VentureTallowHostEntry *entry;
	gboolean loaded;

	*refusal = NULL;
	if (!formula_check_shape(expression, &local_error))
	{
		*refusal = g_strdup(local_error->message);
		return NULL;
	}

	/* the sandbox with nothing granted, no GIR at all, and the limits */
	vm = tallow_suet_new();
	tallow_suet_set_std_dir(vm, limits->std_dir);
	tallow_suet_set_sandbox(vm, TRUE);
	tallow_suet_set_restrict_gir(vm, TRUE);
	tallow_suet_set_max_steps(vm, limits->max_steps);
	tallow_suet_set_max_memory(vm, limits->max_memory);
	tallow_suet_set_max_depth(vm, FORMULA_MAX_DEPTH);

	/* the accessors, and the compiled kernels -- nothing that logs,
	 * registers or reaches the outside */
	tallow_suet_add_module_source(vm, "formula", venture_formula_module_source());
	tallow_suet_add_module_source(vm, "venture_kernels", venture_tallow_kernel_module_source());
	for (entry = formula_functions; NULL != entry->symbol; entry++)
		tallow_suet_add_host_function(vm, entry->symbol, entry->func, NULL, NULL);
	for (entry = venture_tallow_kernel_functions(); NULL != entry->symbol; entry++)
		tallow_suet_add_host_function(vm, entry->symbol, entry->func, NULL, NULL);

	source = formula_wrap(expression, kind);
	g_mutex_lock(&formula_compile_lock);
	loaded = tallow_suet_load_source(vm, FORMULA_FILE, source, &local_error);
	g_mutex_unlock(&formula_compile_lock);

	if (!loaded)
	{
		*refusal = formula_diagnostic(expression, local_error->message);
		return NULL;
	}
	if (!formula_check_program(vm, kind, &local_error))
	{
		*refusal = g_strdup(local_error->message);
		return NULL;
	}

	return g_steal_pointer(&vm);
}

/* --- The cache ------------------------------------------------------------------ */

/* the cache key: the kind, then the text exactly */
static gchar *
formula_key(
	const gchar		*expression,
	VentureFormulaKind	 kind
){
	return g_strdup_printf("%c\n%s", (VENTURE_FORMULA_KIND_INT == kind) ? 'i' : 'b', expression);
}

/* makes room for one more entry: the least recently used goes */
static void
formula_cache_evict(void)
{
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gpointer oldest;
	guint64 oldest_used;

	while (g_hash_table_size(formula_state.cache) >= formula_state.cache_size)
	{
		oldest = NULL;
		oldest_used = G_MAXUINT64;
		g_hash_table_iter_init(&iter, formula_state.cache);
		while (g_hash_table_iter_next(&iter, &key, &value))
		{
			if (((FormulaEntry *)value)->used < oldest_used)
			{
				oldest_used = ((FormulaEntry *)value)->used;
				oldest = key;
			}
		}
		if (NULL == oldest)
			return;
		g_hash_table_remove(formula_state.cache, oldest);
	}
}

/*
 * The entry for a formula, compiled now when the cache has none: a
 * reference the caller drops. Compiling happens outside the state's lock
 * (it takes milliseconds); when two threads compile the same formula at
 * once, the first to finish is kept and the other's VM is dropped.
 */
static FormulaEntry *
formula_lookup(
	const gchar		 *expression,
	VentureFormulaKind	  kind,
	GError			**error
){
	g_autofree gchar *key = NULL;
	g_autoptr(FormulaEntry) made = NULL;
	FormulaEntry *entry;
	FormulaLimits limits;
	gboolean allow;

	key = formula_key((NULL != expression) ? expression : "", kind);

	g_mutex_lock(&formula_state.lock);
	formula_state_ensure();
	allow = formula_state.allow;
	entry = g_hash_table_lookup(formula_state.cache, key);
	if (NULL != entry)
	{
		entry->used = ++formula_state.clock;
		formula_entry_ref(entry);
	}
	limits.max_steps = formula_state.max_steps;
	limits.max_memory = formula_state.max_memory;
	limits.std_dir = g_strdup(formula_state.std_dir);
	g_mutex_unlock(&formula_state.lock);

	if (!allow)
	{
		g_free(limits.std_dir);
		g_clear_pointer(&entry, formula_entry_unref);
		venture_set_error_validation(error, "Expression",
			"formulas are switched off on this server (formulas.allow)");
		return NULL;
	}
	if (NULL != entry)
	{
		g_free(limits.std_dir);
		return entry;
	}

	/* not cached: compile, outside the lock */
	made = g_new0(FormulaEntry, 1);
	g_atomic_ref_count_init(&made->ref);
	g_mutex_init(&made->lock);
	made->vm = formula_compile((NULL != expression) ? expression : "", kind, &limits,
	                           &made->refusal);
	g_free(limits.std_dir);

	g_mutex_lock(&formula_state.lock);
	formula_state.compiles++;
	entry = g_hash_table_lookup(formula_state.cache, key);
	if (NULL == entry)
	{
		formula_cache_evict();
		entry = made;
		g_hash_table_insert(formula_state.cache, g_steal_pointer(&key),
		                    formula_entry_ref(made));
	}
	entry->used = ++formula_state.clock;
	formula_entry_ref(entry);
	g_mutex_unlock(&formula_state.lock);

	return entry;
}

/* --- Checking and evaluating ------------------------------------------------- */

gboolean
venture_formula_check(
	const gchar		 *expression,
	VentureFormulaKind	  kind,
	GError			**error
){
	g_autoptr(FormulaEntry) entry = NULL;

	entry = formula_lookup(expression, kind, error);
	if (NULL == entry)
		return FALSE;
	if (NULL != entry->refusal)
	{
		venture_set_error_validation(error, "Expression", "%s", entry->refusal);
		return FALSE;
	}

	return TRUE;
}


/* --- The fields a formula reads ---------------------------------------------- */

/* the accessors of the `formula` module, which all take the field's name
 * right after the record */
static const gchar *const formula_accessors[] = {
	"field_str", "field_i64", "field_f64", "field_bool", "has_field",
	"field_has", "str_of", "i64_of", "f64_of", "bool_of", NULL
};

/* TRUE when C can continue an identifier */
static gboolean
formula_ident_char(gchar c)
{
	return g_ascii_isalnum(c) || ('_' == c);
}

/* P past white space and comments (the text has passed formula_check_shape,
 * so every comment closes) */
static const gchar *
formula_skip_blank(const gchar *p)
{
	for (;;)
	{
		if (g_ascii_isspace(*p))
			p++;
		else if (('/' == p[0]) && ('*' == p[1]))
			p = strstr(p + 2, "*/") + 2;
		else
			return p;
	}
}

/*
 * The end of one argument starting at P: the `,` or `)` that closes it at
 * its own depth. Strings, their interpolations, brackets and comments are
 * stepped over with a stack like formula_check_shape's. Returns NULL when
 * the text ends first, which a checked formula never does.
 */
static const gchar *
formula_argument_end(const gchar *p)
{
	g_autoptr(GString) open = NULL;

	open = g_string_new(NULL);
	while ('\0' != *p)
	{
		gchar top;

		top = (open->len > 0) ? open->str[open->len - 1] : '\0';
		if ('S' == top)
		{
			if (('\\' == *p) && ('\0' != p[1]))
				p += 2;
			else if ('"' == *p)
			{
				g_string_truncate(open, open->len - 1);
				p++;
			}
			else if (('$' == *p) && ('{' == p[1]))
			{
				g_string_append_c(open, 'I');
				p += 2;
			}
			else
				p++;
			continue;
		}
		if (('/' == p[0]) && ('*' == p[1]))
		{
			p = formula_skip_blank(p);
			continue;
		}
		if ((0 == open->len) && ((',' == *p) || (')' == *p)))
			return p;
		switch (*p)
		{
		case '"':
			g_string_append_c(open, 'S');
			break;
		case '(':
		case '[':
		case '{':
			g_string_append_c(open, *p);
			break;
		case ')':
		case ']':
		case '}':
			if (open->len > 0)
				g_string_truncate(open, open->len - 1);
			break;
		default:
			break;
		}
		p++;
	}

	return NULL;
}

/*
 * The field name an accessor call names, when it is a plain string
 * literal standing alone as its argument at P: `"seats"`, not `"se" + x`,
 * not `"${k}"`, not a variable. Returns NULL for anything else.
 */
static gchar *
formula_literal_argument(const gchar *p)
{
	const gchar *start;
	const gchar *q;

	p = formula_skip_blank(p);
	if ('"' != *p)
		return NULL;
	start = p + 1;
	for (q = start; ('"' != *q); q++)
	{
		if (('\0' == *q) || ('\\' == *q) || (('$' == *q) && ('{' == q[1])))
			return NULL;
	}
	p = formula_skip_blank(q + 1);
	if ((',' != *p) && (')' != *p))
		return NULL;

	return g_strndup(start, (gsize)(q - start));
}

/*
 * Walks TEXT for every call to an accessor and takes the field it names:
 * the argument after the record in `field_str(r, "seats")`, or the first
 * one in the method form `r.field_str("seats")`. A name that is not a
 * plain string literal sets *DYNAMIC. Strings are stepped over (only an
 * interpolation inside one is code), and so are comments: a key quoted in
 * a comment or compared as a value is not a field the formula reads.
 */
static void
formula_collect_fields(
	const gchar	*text,
	GPtrArray	*fields,
	gboolean	*dynamic
){
	g_autoptr(GString) open = NULL;
	const gchar *p;

	open = g_string_new(NULL);
	p = text;
	while ('\0' != *p)
	{
		gchar top;
		const gchar *start;
		g_autofree gchar *word = NULL;
		const gchar *argument;
		gchar *name;
		gboolean method;

		top = (open->len > 0) ? open->str[open->len - 1] : '\0';

		/* string text is data: only its interpolations are code */
		if ('S' == top)
		{
			if (('\\' == *p) && ('\0' != p[1]))
				p += 2;
			else if ('"' == *p)
			{
				g_string_truncate(open, open->len - 1);
				p++;
			}
			else if (('$' == *p) && ('{' == p[1]))
			{
				g_string_append_c(open, 'I');
				p += 2;
			}
			else
				p++;
			continue;
		}
		if (('/' == p[0]) && ('*' == p[1]))
		{
			p = formula_skip_blank(p);
			continue;
		}
		if ('"' == *p)
		{
			g_string_append_c(open, 'S');
			p++;
			continue;
		}
		if ('{' == *p)
		{
			g_string_append_c(open, '{');
			p++;
			continue;
		}
		if ('}' == *p)
		{
			/* closes a block, or an interpolation back into its string */
			if (open->len > 0)
				g_string_truncate(open, open->len - 1);
			p++;
			continue;
		}
		if (!(g_ascii_isalpha(*p) || ('_' == *p)) ||
		    ((p > text) && formula_ident_char(p[-1])))
		{
			p++;
			continue;
		}

		/* an identifier: is it an accessor being called? */
		start = p;
		while (formula_ident_char(*p))
			p++;
		word = g_strndup(start, (gsize)(p - start));
		if (!g_strv_contains(formula_accessors, word))
			continue;
		argument = formula_skip_blank(p);
		if ('(' != *argument)
			continue;
		argument++;

		/* the method form names the field first; the call form second */
		method = (start > text) && ('.' == start[-1]);
		if (!method)
		{
			argument = formula_argument_end(argument);
			if ((NULL == argument) || (',' != *argument))
			{
				*dynamic = TRUE;
				continue;
			}
			argument++;
		}
		name = formula_literal_argument(argument);
		if (NULL == name)
		{
			*dynamic = TRUE;
			continue;
		}
		if (g_ptr_array_find_with_equal_func(fields, name, g_str_equal, NULL))
			g_free(name);
		else
			g_ptr_array_add(fields, name);
	}
}

GPtrArray *
venture_formula_fields(
	const gchar	 *expression,
	gboolean	 *dynamic,
	GError		**error
){
	g_autoptr(GPtrArray) fields = NULL;
	gboolean any_dynamic;

	if (!formula_check_shape(expression, error))
		return NULL;

	fields = g_ptr_array_new_with_free_func(g_free);
	any_dynamic = FALSE;
	formula_collect_fields(expression, fields, &any_dynamic);
	if (NULL != dynamic)
		*dynamic = any_dynamic;

	return (GPtrArray *)g_steal_pointer(&fields);
}

/* runs a formula on a record; RESULT is set to the formula's value */
static gboolean
formula_eval(
	const gchar		 *expression,
	VentureFormulaKind	  kind,
	const gchar		 *record_json,
	GValue			 *result,
	GError			**error
){
	g_autoptr(FormulaEntry) entry = NULL;
	g_autoptr(GError) local_error = NULL;
	GValue arg = G_VALUE_INIT;
	gboolean ok;

	entry = formula_lookup(expression, kind, error);
	if (NULL == entry)
		return FALSE;
	if (NULL != entry->refusal)
	{
		venture_set_error_validation(error, "Expression", "%s", entry->refusal);
		return FALSE;
	}

	g_value_init(&arg, G_TYPE_STRING);
	g_value_set_string(&arg, (NULL != record_json) ? record_json : "{}");

	g_mutex_lock(&entry->lock);
	ok = tallow_suet_call(entry->vm, FORMULA_FUNCTION, &arg, 1, result, &local_error);
	g_mutex_unlock(&entry->lock);
	g_value_unset(&arg);

	/* a panic, a limit: the formula's failure, never the server's */
	if (!ok)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN, "formula: %s",
		            local_error->message);
		return FALSE;
	}

	return TRUE;
}

gboolean
venture_formula_eval_bool(
	const gchar	 *expression,
	const gchar	 *record_json,
	gboolean	 *result,
	GError		**error
){
	g_auto(GValue) value = G_VALUE_INIT;

	g_return_val_if_fail(NULL != result, FALSE);

	*result = FALSE;
	if (!formula_eval(expression, VENTURE_FORMULA_KIND_BOOL, record_json, &value, error))
		return FALSE;
	if (!G_VALUE_HOLDS_BOOLEAN(&value))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "formula: the condition yielded no bool");
		return FALSE;
	}

	*result = g_value_get_boolean(&value);
	return TRUE;
}

gboolean
venture_formula_eval_int(
	const gchar	 *expression,
	const gchar	 *record_json,
	gint64		 *result,
	GError		**error
){
	g_auto(GValue) value = G_VALUE_INIT;

	g_return_val_if_fail(NULL != result, FALSE);

	*result = 0;
	if (!formula_eval(expression, VENTURE_FORMULA_KIND_INT, record_json, &value, error))
		return FALSE;
	if (!G_VALUE_HOLDS_INT64(&value))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "formula: the score yielded no i64");
		return FALSE;
	}

	*result = g_value_get_int64(&value);
	return TRUE;
}

gboolean
venture_formula_eval_bool_object(
	const gchar	 *expression,
	JsonObject	 *record,
	gboolean	 *result,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *json = NULL;

	node = json_node_new(JSON_NODE_OBJECT);
	if (NULL != record)
		json_node_set_object(node, record);
	else
		json_node_take_object(node, json_object_new());
	json = json_to_string(node, FALSE);

	return venture_formula_eval_bool(expression, json, result, error);
}
