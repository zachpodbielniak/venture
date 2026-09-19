/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The mapped payload, parsed once into typed rows. Included by
 * venture-cutover-service.c before the importers, reconcile and rollback.
 *
 * Nothing here writes. Every row problem is collected with its location --
 * "open_ap[12] bill-9: amount ..." -- instead of stopping at the first one:
 * preview stores the whole list on its rows, and import refuses with it
 * before its first write. A migration that fails on row 40 after writing 39
 * leaves an operator nothing to fix; one that names all eleven bad rows at
 * once is a spreadsheet afternoon.
 */

/* The generic CSV importer's limit. Past it a single transaction holds the
 * database lock long enough to stall every other writer. */
#define CUTOVER_ROW_LIMIT 5000
#define CUTOVER_CLEARING_CODE "3900"

typedef struct
{
	gchar *section;
	gint position;
	gchar *source_id;
	gchar *message;
	gboolean warning;
} Problem;

static void
problem_free(gpointer data)
{
	Problem *problem = data;
	g_free(problem->section);
	g_free(problem->source_id);
	g_free(problem->message);
	g_free(problem);
}

static gchar *
problem_format(const Problem *problem)
{
	GString *text = g_string_new(problem->section);
	if (problem->position >= 0)
		g_string_append_printf(text, "[%d]", problem->position);
	if (problem->source_id != NULL && *problem->source_id != '\0')
		g_string_append_printf(text, " %s", problem->source_id);
	g_string_append_printf(text, ": %s", problem->message);
	return g_string_free(text, FALSE);
}

/* One bill line of an open_ap row. */
typedef struct
{
	gchar *description;
	gchar *account_code;
	gchar *category;
	VentureMoney *amount;
	VentureMoney *tax;
} BillLine;

static void
bill_line_free(gpointer data)
{
	BillLine *line = data;
	g_free(line->description);
	g_free(line->account_code);
	g_free(line->category);
	g_clear_pointer(&line->amount, venture_money_free);
	g_clear_pointer(&line->tax, venture_money_free);
	g_free(line);
}

/*
 * One parsed source row of any section. Only the members a section uses are
 * set; the rest stay NULL or zero. @open is what the subledger must hold for
 * the row at the cutover: an invoice or bill's total less what was paid
 * before it, a credit's amount, or an asset's net book value.
 */
typedef struct
{
	const gchar *section;
	gint position;
	gchar *source_id;
	gboolean ok;
	gboolean vendor;
	gboolean prepayment;
	gboolean default_account;
	gchar *party_source;
	gchar *party_name;
	gchar *number;
	gchar *name;
	gchar *memo;
	gchar *terms;
	gchar *tag;
	gchar *category;
	gchar *method;
	gchar *account_code;
	gchar *accumulated_code;
	gchar *expense_code;
	gchar *tax_exempt_reason;
	gboolean tax_exempt;
	GDateTime *date;
	GDateTime *due;
	GDateTime *acquired;
	gint64 life_months;
	gint64 quantity;
	VentureMoney *total;
	VentureMoney *net;
	VentureMoney *tax;
	VentureMoney *paid;
	VentureMoney *open;
	VentureMoney *salvage;
	VentureMoney *accumulated;
	GPtrArray *lines;
} Row;

static void
row_free(gpointer data)
{
	Row *row = data;
	g_free(row->source_id);
	g_free(row->party_source);
	g_free(row->party_name);
	g_free(row->number);
	g_free(row->name);
	g_free(row->memo);
	g_free(row->terms);
	g_free(row->tag);
	g_free(row->category);
	g_free(row->method);
	g_free(row->account_code);
	g_free(row->accumulated_code);
	g_free(row->expense_code);
	g_free(row->tax_exempt_reason);
	g_clear_pointer(&row->date, g_date_time_unref);
	g_clear_pointer(&row->due, g_date_time_unref);
	g_clear_pointer(&row->acquired, g_date_time_unref);
	g_clear_pointer(&row->total, venture_money_free);
	g_clear_pointer(&row->net, venture_money_free);
	g_clear_pointer(&row->tax, venture_money_free);
	g_clear_pointer(&row->paid, venture_money_free);
	g_clear_pointer(&row->open, venture_money_free);
	g_clear_pointer(&row->salvage, venture_money_free);
	g_clear_pointer(&row->accumulated, venture_money_free);
	g_clear_pointer(&row->lines, g_ptr_array_unref);
	g_free(row);
}

typedef struct
{
	gchar *code;
	gchar *name;
	VentureAccountKind kind;
	gboolean active;
} ChartRow;

static void
chart_row_free(gpointer data)
{
	ChartRow *row = data;
	g_free(row->code);
	g_free(row->name);
	g_free(row);
}

typedef struct
{
	VentureCutoverService *self;
	JsonObject *payload;
	gint64 org;
	gint64 cutover_id;
	gchar *source;
	GDateTime *cutoff;
	gchar *currency;
	gchar decimal_separator;
	gchar thousands_separator;
	gchar *clearing_code;
	JsonObject *account_map;
	GPtrArray *chart;
	GHashTable *chart_codes;
	GHashTable *customers;
	GHashTable *vendors;
	GPtrArray *open_ar;
	GPtrArray *open_ap;
	GPtrArray *credits;
	GPtrArray *bank;
	GPtrArray *assets;
	GPtrArray *trial_balance;
	GPtrArray *inventory;
	GPtrArray *problems;
	guint items;
} Plan;

static void
plan_free(Plan *plan)
{
	if (plan == NULL)
		return;
	g_free(plan->source);
	g_clear_pointer(&plan->cutoff, g_date_time_unref);
	g_free(plan->currency);
	g_free(plan->clearing_code);
	g_clear_pointer(&plan->account_map, json_object_unref);
	g_clear_pointer(&plan->payload, json_object_unref);
	g_clear_pointer(&plan->chart_codes, g_hash_table_unref);
	g_clear_pointer(&plan->chart, g_ptr_array_unref);
	g_clear_pointer(&plan->customers, g_hash_table_unref);
	g_clear_pointer(&plan->vendors, g_hash_table_unref);
	g_clear_pointer(&plan->open_ar, g_ptr_array_unref);
	g_clear_pointer(&plan->open_ap, g_ptr_array_unref);
	g_clear_pointer(&plan->credits, g_ptr_array_unref);
	g_clear_pointer(&plan->bank, g_ptr_array_unref);
	g_clear_pointer(&plan->assets, g_ptr_array_unref);
	g_clear_pointer(&plan->trial_balance, g_ptr_array_unref);
	g_clear_pointer(&plan->inventory, g_ptr_array_unref);
	g_clear_pointer(&plan->problems, g_ptr_array_unref);
	g_free(plan);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Plan, plan_free)

static void plan_note(Plan *plan, const gchar *section, gint position, const gchar *source_id,
	gboolean warning, const gchar *format, ...) G_GNUC_PRINTF(6, 7);

static void
plan_note(Plan *plan, const gchar *section, gint position, const gchar *source_id,
	gboolean warning, const gchar *format, ...)
{
	Problem *problem = g_new0(Problem, 1);
	va_list args;
	va_start(args, format);
	problem->message = g_strdup_vprintf(format, args);
	va_end(args);
	problem->section = g_strdup(section);
	problem->position = position;
	problem->source_id = g_strdup(source_id);
	problem->warning = warning;
	g_ptr_array_add(plan->problems, problem);
}

static void row_error(Plan *plan, Row *row, const gchar *format, ...) G_GNUC_PRINTF(3, 4);

static void
row_error(Plan *plan, Row *row, const gchar *format, ...)
{
	g_autofree gchar *message = NULL;
	va_list args;
	va_start(args, format);
	message = g_strdup_vprintf(format, args);
	va_end(args);
	row->ok = FALSE;
	plan_note(plan, row->section, row->position, row->source_id, FALSE, "%s", message);
}

static void row_warning(Plan *plan, Row *row, const gchar *format, ...) G_GNUC_PRINTF(3, 4);

static void
row_warning(Plan *plan, Row *row, const gchar *format, ...)
{
	g_autofree gchar *message = NULL;
	va_list args;
	va_start(args, format);
	message = g_strdup_vprintf(format, args);
	va_end(args);
	plan_note(plan, row->section, row->position, row->source_id, TRUE, "%s", message);
}

static guint
plan_error_count(Plan *plan)
{
	guint i, count = 0;
	for (i = 0; i < plan->problems->len; i++)
		if (!((Problem *)g_ptr_array_index(plan->problems, i))->warning)
			count++;
	return count;
}

/* The refusal import returns: every located error, capped so a hopeless
 * payload still produces a readable message. */
static gboolean
plan_refuse_errors(Plan *plan, GError **error)
{
	g_autoptr(GString) text = g_string_new(NULL);
	guint i, shown = 0, count = plan_error_count(plan);
	if (count == 0)
		return TRUE;
	g_string_append_printf(text, "VentureCutoverService: %u row problem%s; nothing was imported:",
		count, count == 1 ? "" : "s");
	for (i = 0; i < plan->problems->len && shown < 20; i++)
	{
		Problem *problem = g_ptr_array_index(plan->problems, i);
		g_autofree gchar *line = NULL;
		if (problem->warning)
			continue;
		line = problem_format(problem);
		g_string_append_printf(text, "\n- %s", line);
		shown++;
	}
	if (count > shown)
		g_string_append_printf(text, "\n- ... and %u more", count - shown);
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, text->str);
	return FALSE;
}

/* --- Typed member access ---------------------------------------------------- */

static JsonNode *
member(JsonObject *object, const gchar *name)
{
	JsonNode *node;
	if (object == NULL || !json_object_has_member(object, name))
		return NULL;
	node = json_object_get_member(object, name);
	return JSON_NODE_HOLDS_NULL(node) ? NULL : node;
}

static const gchar *
json_kind(JsonNode *node)
{
	if (JSON_NODE_HOLDS_OBJECT(node))
		return "an object";
	if (JSON_NODE_HOLDS_ARRAY(node))
		return "an array";
	if (json_node_get_value_type(node) == G_TYPE_BOOLEAN)
		return "true/false";
	return "a number";
}

/*
 * Text from a string, or from a JSON integer written by a spreadsheet export
 * ("number": 1001). An empty string counts as absent. @prefix locates a
 * nested member such as "lines[2] ".
 */
static gchar *
field_text(Plan *plan, Row *row, JsonObject *object, const gchar *prefix, const gchar *name, gboolean required)
{
	JsonNode *node = member(object, name);
	GType type;
	if (node == NULL)
	{
		if (required)
			row_error(plan, row, "%s%s is required", prefix, name);
		return NULL;
	}
	if (!JSON_NODE_HOLDS_VALUE(node))
	{
		row_error(plan, row, "%s%s must be text, not %s", prefix, name, json_kind(node));
		return NULL;
	}
	type = json_node_get_value_type(node);
	if (type == G_TYPE_STRING)
	{
		const gchar *text = json_node_get_string(node);
		if (text == NULL || *text == '\0')
		{
			if (required)
				row_error(plan, row, "%s%s is required", prefix, name);
			return NULL;
		}
		return g_strdup(text);
	}
	if (type == G_TYPE_INT64)
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
	if (type == G_TYPE_DOUBLE)
	{
		gdouble value = json_node_get_double(node);
		if (value == (gdouble)(gint64)value && value > -9007199254740992.0 && value < 9007199254740992.0)
			return g_strdup_printf("%" G_GINT64_FORMAT, (gint64)value);
	}
	row_error(plan, row, "%s%s must be text, not %s", prefix, name, json_kind(node));
	return NULL;
}

/* An integer from a JSON integer, an integral JSON number or a string of
 * digits ("useful_life_months": "36"). */
static gboolean
field_int(Plan *plan, Row *row, JsonObject *object, const gchar *name, gboolean required, gint64 *out)
{
	JsonNode *node = member(object, name);
	*out = 0;
	if (node == NULL)
	{
		if (required)
			row_error(plan, row, "%s is required", name);
		return !required;
	}
	if (JSON_NODE_HOLDS_VALUE(node))
	{
		GType type = json_node_get_value_type(node);
		if (type == G_TYPE_INT64)
		{
			*out = json_node_get_int(node);
			return TRUE;
		}
		if (type == G_TYPE_DOUBLE)
		{
			gdouble value = json_node_get_double(node);
			if (value == (gdouble)(gint64)value && value > -9007199254740992.0 && value < 9007199254740992.0)
			{
				*out = (gint64)value;
				return TRUE;
			}
		}
		if (type == G_TYPE_STRING)
		{
			const gchar *text = json_node_get_string(node);
			gchar *end = NULL;
			gint64 value;
			if (text != NULL && *text != '\0')
			{
				errno = 0;
				value = g_ascii_strtoll(text, &end, 10);
				if (errno == 0 && end != NULL && *end == '\0')
				{
					*out = value;
					return TRUE;
				}
			}
		}
	}
	row_error(plan, row, "%s must be a whole number", name);
	return FALSE;
}

static gboolean
field_bool(Plan *plan, Row *row, JsonObject *object, const gchar *name, gboolean *out)
{
	JsonNode *node = member(object, name);
	*out = FALSE;
	if (node == NULL)
		return TRUE;
	if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN)
	{
		*out = json_node_get_boolean(node);
		return TRUE;
	}
	if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
	{
		const gchar *text = json_node_get_string(node);
		if (g_ascii_strcasecmp(text, "true") == 0 || g_ascii_strcasecmp(text, "yes") == 0 || g_strcmp0(text, "1") == 0)
		{
			*out = TRUE;
			return TRUE;
		}
		if (g_ascii_strcasecmp(text, "false") == 0 || g_ascii_strcasecmp(text, "no") == 0 ||
			g_strcmp0(text, "0") == 0 || *text == '\0')
			return TRUE;
	}
	row_error(plan, row, "%s must be true or false", name);
	return FALSE;
}

/* ISO dates only. venture_time_from_string() also accepts "now" and
 * "today", which in a migration file would silently date history at the
 * moment somebody pressed Import. */
static GDateTime *
field_date(Plan *plan, Row *row, JsonObject *object, const gchar *name, gboolean required)
{
	g_autofree gchar *text = field_text(plan, row, object, "", name, required);
	GDateTime *date;
	if (text == NULL)
		return NULL;
	date = g_ascii_isdigit(text[0]) ? venture_time_from_string(text, NULL) : NULL;
	if (date == NULL)
		row_error(plan, row, "%s \"%s\" is not an ISO date such as 2025-12-31", name, text);
	return date;
}

/*
 * The strict money grammar for a migration payload:
 *
 *     [-]DIGITS[.FRACTION][ CUR]
 *
 * where the payload may rename the decimal separator and allow a thousands
 * separator, and CUR is three uppercase letters, taken from the row or the
 * payload when the text omits it. The fraction may not be longer than the
 * currency's minor unit.
 *
 * venture_money_from_string() is deliberately forgiving because it reads
 * marketplace exports: it drops symbols, commas and a second decimal point.
 * For opening balances that tolerance is a silent misread -- "1.234,56 EUR"
 * became 1.23456 and "1,50 EUR" became 150.00 -- so this grammar refuses
 * everything it does not fully understand. The global parser is unchanged.
 */
static VentureMoney *
parse_strict_money(const gchar *text, const gchar *currency, gchar decimal, gchar thousands, gchar **why)
{
	g_autofree gchar *working = g_strstrip(g_strdup(text != NULL ? text : ""));
	g_autoptr(GString) digits = g_string_new(NULL);
	g_autoptr(GString) fraction = g_string_new(NULL);
	g_autofree gchar *normalized = NULL;
	gchar code[4] = { 0 };
	gsize length = strlen(working), i = 0, group = 0;
	gboolean negative = FALSE, in_fraction = FALSE, grouped = FALSE, first_group = TRUE;
	guint exponent;

	if (length > 4 && working[length - 4] == ' ' && g_ascii_isalpha(working[length - 3]) &&
		g_ascii_isalpha(working[length - 2]) && g_ascii_isalpha(working[length - 1]))
	{
		if (!g_ascii_isupper(working[length - 3]) || !g_ascii_isupper(working[length - 2]) ||
			!g_ascii_isupper(working[length - 1]))
		{
			*why = g_strdup_printf("\"%s\" names its currency in lowercase; write it as three uppercase letters", text);
			return NULL;
		}
		memcpy(code, working + length - 3, 3);
		working[length - 4] = '\0';
		length -= 4;
	}
	else if (currency != NULL && *currency != '\0')
		g_strlcpy(code, currency, sizeof(code));
	else
	{
		*why = g_strdup_printf("\"%s\" must include an ISO currency, as in \"105.00 USD\", or the row or payload must name one", text);
		return NULL;
	}
	if (!venture_currency_is_valid(code) || !g_ascii_isupper(code[0]) || !g_ascii_isupper(code[1]) || !g_ascii_isupper(code[2]))
	{
		*why = g_strdup_printf("\"%s\" has no valid ISO currency code", text);
		return NULL;
	}
	if (length > 0 && working[0] == '-')
	{
		negative = TRUE;
		i = 1;
	}
	if (i >= length || !g_ascii_isdigit(working[i]))
	{
		*why = g_strdup_printf("\"%s\" is not an amount of the form [-]1234.56 CUR", text);
		return NULL;
	}
	for (; i < length; i++)
	{
		gchar c = working[i];
		if (g_ascii_isdigit(c))
		{
			g_string_append_c(in_fraction ? fraction : digits, c);
			if (!in_fraction)
				group++;
			continue;
		}
		if (!in_fraction && thousands != '\0' && c == thousands)
		{
			/* Groups after the first are exactly three digits. */
			if (group == 0 || (first_group ? group > 3 : group != 3))
				break;
			first_group = FALSE;
			grouped = TRUE;
			group = 0;
			continue;
		}
		if (!in_fraction && c == decimal)
		{
			if (grouped && group != 3)
				break;
			in_fraction = TRUE;
			continue;
		}
		break;
	}
	if (i < length || (in_fraction && fraction->len == 0) || (grouped && !in_fraction && group != 3))
	{
		*why = g_strdup_printf("\"%s\" is not an amount of the form [-]1234%c56 CUR%s", text, decimal,
			thousands != '\0' ? " (with optional thousands separators)" : "; set thousands_separator to use grouping");
		return NULL;
	}
	exponent = venture_currency_get_exponent(code);
	if (fraction->len > exponent)
	{
		*why = g_strdup_printf("\"%s\" has %u decimal places but %s has %u", text, (guint)fraction->len, code, exponent);
		return NULL;
	}
	normalized = g_strdup_printf("%s%s%s%s %s", negative ? "-" : "", digits->str,
		fraction->len > 0 ? "." : "", fraction->str, code);
	{
		g_autoptr(GError) local = NULL;
		VentureMoney *money = venture_money_from_string(normalized, NULL, &local);
		if (money == NULL)
			*why = g_strdup_printf("\"%s\": %s", text, local != NULL ? local->message : "unreadable amount");
		return money;
	}
}

/*
 * Money from a strict string, the serialised money object, or a JSON number.
 * A JSON number is binary floating point: it is accepted only when the
 * currency's minor units print it exactly, so 105.0 is 105.00 and 0.1 is
 * 0.10, but 1.005 in a two-decimal currency is refused rather than rounded.
 */
static VentureMoney *
field_money(Plan *plan, Row *row, JsonObject *object, const gchar *prefix, const gchar *name,
	const gchar *currency, gboolean required)
{
	JsonNode *node = member(object, name);
	g_autofree gchar *why = NULL;
	VentureMoney *money = NULL;
	if (node == NULL)
	{
		if (required)
			row_error(plan, row, "%s%s is required", prefix, name);
		return NULL;
	}
	if (JSON_NODE_HOLDS_OBJECT(node) && member(json_node_get_object(node), "currency") == NULL &&
		(currency == NULL || *currency == '\0'))
		/* venture_money_from_json() would fall back to the process
		 * default currency, which is a guess. */
		why = g_strdup("is a money object without a currency, and the row and payload name none");
	else if (JSON_NODE_HOLDS_OBJECT(node))
	{
		g_autoptr(GError) local = NULL;
		money = venture_money_from_json(node, currency, &local);
		if (money == NULL)
			why = g_strdup(local != NULL ? local->message : "unreadable money object");
		else if (!venture_currency_is_valid(venture_money_get_currency(money)))
		{
			why = g_strdup("the money object has no valid currency");
			g_clear_pointer(&money, venture_money_free);
		}
	}
	else if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
		money = parse_strict_money(json_node_get_string(node), currency, plan->decimal_separator,
			plan->thousands_separator, &why);
	else if (JSON_NODE_HOLDS_VALUE(node) && (json_node_get_value_type(node) == G_TYPE_INT64 ||
		json_node_get_value_type(node) == G_TYPE_DOUBLE))
	{
		guint exponent = venture_currency_get_exponent(currency);
		if (currency == NULL || *currency == '\0')
			why = g_strdup("a JSON number carries no currency; name one on the row or the payload");
		else if (json_node_get_value_type(node) == G_TYPE_INT64)
		{
			g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
			money = parse_strict_money(text, currency, '.', '\0', &why);
		}
		else
		{
			gdouble value = json_node_get_double(node);
			gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
			gdouble limit = 9007199254740992.0;
			guint e;
			for (e = 0; e < exponent; e++)
				limit /= 10.0;
			g_ascii_formatd(buffer, sizeof(buffer), exponent == 0 ? "%.0f" :
				(exponent == 1 ? "%.1f" : (exponent == 2 ? "%.2f" : (exponent == 3 ? "%.3f" : "%.4f"))), value);
			if (value >= limit || value <= -limit || g_ascii_strtod(buffer, NULL) != value)
				why = g_strdup_printf("the JSON number %.17g is not exact in %s minor units; write it as a string such as \"105.00 %s\"",
					value, currency, currency);
			else
				money = parse_strict_money(buffer, currency, '.', '\0', &why);
		}
	}
	else
		why = g_strdup_printf("must be an amount such as \"105.00 USD\", not %s", json_kind(node));
	if (money == NULL)
		row_error(plan, row, "%s%s %s", prefix, name, why != NULL ? why : "is not a valid amount");
	return money;
}

static gboolean
same_currency(const VentureMoney *a, const VentureMoney *b)
{
	return a == NULL || b == NULL || g_strcmp0(venture_money_get_currency(a), venture_money_get_currency(b)) == 0;
}

static VentureMoney *
money_sum(const VentureMoney *a, const VentureMoney *b)
{
	if (a == NULL)
		return b != NULL ? venture_money_copy((VentureMoney *)b) : NULL;
	if (b == NULL)
		return venture_money_copy((VentureMoney *)a);
	return venture_money_add(a, b, NULL);
}

static VentureMoney *
money_difference(const VentureMoney *a, const VentureMoney *b)
{
	if (b == NULL)
		return a != NULL ? venture_money_copy((VentureMoney *)a) : NULL;
	if (a == NULL)
		return venture_money_negate(b);
	return venture_money_subtract(a, b, NULL);
}

/* --- Accounts ---------------------------------------------------------------- */

/* A source code through account_map. The result is borrowed from the map or
 * is @code itself. */
static const gchar *
plan_map_code(Plan *plan, const gchar *code)
{
	JsonNode *node;
	if (code == NULL || plan->account_map == NULL)
		return code;
	node = member(plan->account_map, code);
	if (node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
		return json_node_get_string(node);
	return code;
}

static VentureEntity *
account_with_code(VentureCutoverService *self, gint64 org, const gchar *code, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	venture_query_set_organization(query, org);
	if (code == NULL || !venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
		return NULL;
	accounts = venture_database_find(self->database, query, error);
	if (accounts == NULL || accounts->len == 0)
		return NULL;
	return g_object_ref(g_ptr_array_index(accounts, 0));
}

/* Resolves and checks a mapped account code for @row. It exists when the
 * chart already has it or the payload's chart will create it. The mapped
 * code is returned for storage on the row. */
static gchar *
plan_account(Plan *plan, Row *row, const gchar *prefix, const gchar *name, const gchar *code)
{
	const gchar *mapped = plan_map_code(plan, code);
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(GError) local = NULL;
	if (mapped == NULL)
		return NULL;
	if (plan->clearing_code != NULL && g_strcmp0(mapped, plan->clearing_code) == 0)
	{
		row_error(plan, row, "%s%s %s is the opening balance clearing account; map the source account to a real one", prefix, name, mapped);
		return NULL;
	}
	if (g_hash_table_contains(plan->chart_codes, mapped))
		return g_strdup(mapped);
	account = account_with_code(plan->self, plan->org, mapped, &local);
	if (account == NULL)
	{
		if (g_strcmp0(mapped, code) != 0)
			row_error(plan, row, "%s%s %s (mapped from %s) is missing from the chart; add it under chart or fix account_map",
				prefix, name, mapped, code);
		else
			row_error(plan, row, "%s%s %s is missing from the chart; add it under chart or map it with account_map", prefix, name, mapped);
		return NULL;
	}
	return g_strdup(mapped);
}

/* --- Sections ---------------------------------------------------------------- */

static JsonArray *
plan_section(Plan *plan, const gchar *name)
{
	JsonNode *node = member(plan->payload, name);
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
		return NULL;
	return json_node_get_array(node);
}

static Row *
plan_row(Plan *plan, const gchar *section, JsonArray *array, guint position, JsonObject **object)
{
	JsonNode *node = json_array_get_element(array, position);
	Row *row = g_new0(Row, 1);
	row->section = section;
	row->position = (gint)position;
	row->ok = TRUE;
	*object = NULL;
	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
	{
		row_error(plan, row, "must be an object, not %s", node != NULL && !JSON_NODE_HOLDS_NULL(node) ? json_kind(node) : "null");
		return row;
	}
	*object = json_node_get_object(node);
	row->source_id = field_text(plan, row, *object, "", "source_id", FALSE);
	return row;
}

static const gchar *
row_currency(JsonObject *object, Plan *plan)
{
	JsonNode *node = member(object, "currency");
	if (node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
		return json_node_get_string(node);
	return plan->currency;
}

static void
require_before_cutoff(Plan *plan, Row *row, const gchar *name, GDateTime *date)
{
	/* Opening balances are the source's position at the end of the day
	 * before the cutoff. A document dated at or after the cutoff instant is
	 * post-cutover activity: record it in VENTURE, not as an opening. */
	if (date != NULL && g_date_time_compare(date, plan->cutoff) >= 0)
	{
		g_autofree gchar *text = g_date_time_format_iso8601(plan->cutoff);
		row_error(plan, row, "%s must be before the cutoff %s; later documents are ordinary activity, not opening balances",
			name, text);
	}
}

/* The rule code a row's party problem carries: one per section, so the
 * refusal names the row and the field it needs (issue #84). */
static const gchar *
party_rule(Row *row)
{
	if (g_strcmp0(row->section, "open_ar") == 0)
		return "open-ar-customer";
	if (g_strcmp0(row->section, "open_ap") == 0)
		return "open-ap-vendor";
	return "credit-party";
}

static void
parse_party(Plan *plan, Row *row, JsonObject *object, gboolean vendor)
{
	row->vendor = vendor;
	row->party_source = field_text(plan, row, object, "", vendor ? "vendor_source_id" : "customer_source_id", FALSE);
	row->party_name = field_text(plan, row, object, "", vendor ? "vendor_name" : "customer_name", FALSE);
	if (row->party_source == NULL && row->party_name == NULL)
		row_error(plan, row, "needs %s or %s (rule: %s)", vendor ? "vendor_source_id" : "customer_source_id",
			vendor ? "vendor_name" : "customer_name", party_rule(row));
}

/* One total rule for import and reconcile. amount is the gross document
 * total; net is income before tax. With all three present they must agree,
 * because a disagreement means the mapping read the wrong column. */
static void
parse_open_ar(Plan *plan, JsonArray *array)
{
	guint i;
	for (i = 0; array != NULL && i < json_array_get_length(array); i++)
	{
		JsonObject *object = NULL;
		Row *row = plan_row(plan, "open_ar", array, i, &object);
		const gchar *currency;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) balance = NULL;
		g_ptr_array_add(plan->open_ar, row);
		if (object == NULL)
			continue;
		currency = row_currency(object, plan);
		if (row->source_id == NULL)
			row_error(plan, row, "source_id is required");
		parse_party(plan, row, object, FALSE);
		row->number = field_text(plan, row, object, "", "number", TRUE);
		/* An undated invoice would be issued as of today, moving history
		 * into the current period. */
		row->date = field_date(plan, row, object, "date", TRUE);
		row->due = field_date(plan, row, object, "due_date", FALSE);
		row->terms = field_text(plan, row, object, "", "terms", FALSE);
		row->memo = field_text(plan, row, object, "", "memo", FALSE);
		field_bool(plan, row, object, "tax_exempt", &row->tax_exempt);
		row->tax_exempt_reason = field_text(plan, row, object, "", "tax_exempt_reason", FALSE);
		require_before_cutoff(plan, row, "date", row->date);
		if (row->date != NULL && row->due != NULL && g_date_time_compare(row->due, row->date) < 0)
			row_warning(plan, row, "due_date precedes date");
		amount = field_money(plan, row, object, "", "amount", currency, FALSE);
		row->net = field_money(plan, row, object, "", "net", currency, FALSE);
		row->tax = field_money(plan, row, object, "", "tax", currency, FALSE);
		row->paid = field_money(plan, row, object, "", "paid", currency, FALSE);
		balance = field_money(plan, row, object, "", "balance", currency, FALSE);
		if (!row->ok)
			continue;
		if (amount == NULL && row->net == NULL)
		{
			row_error(plan, row, "needs amount (the gross total) or net");
			continue;
		}
		if (!same_currency(amount, row->net) || !same_currency(amount, row->tax) || !same_currency(row->net, row->tax) ||
			!same_currency(amount != NULL ? amount : row->net, row->paid) || !same_currency(amount != NULL ? amount : row->net, balance))
		{
			row_error(plan, row, "mixes currencies between amount, net, tax, paid and balance");
			continue;
		}
		if (row->net != NULL)
		{
			row->total = money_sum(row->net, row->tax);
			if (amount != NULL && !venture_money_equal(amount, row->total))
			{
				g_autofree gchar *a = venture_money_to_string(amount);
				g_autofree gchar *t = venture_money_to_string(row->total);
				row_error(plan, row, "amount %s is not net plus tax (%s); amount is the gross total", a, t);
				continue;
			}
		}
		else
		{
			row->total = venture_money_copy(amount);
			row->net = money_difference(amount, row->tax);
		}
		if (row->tax == NULL)
			row->tax = venture_money_new_zero(venture_money_get_currency(row->total));
		if (venture_money_is_zero(row->total))
		{
			row_error(plan, row, "has a zero total; nothing is open");
			continue;
		}
		if (venture_money_get_amount(row->total) < 0)
		{
			/* A negative receivable is money owed back to the customer:
			 * it imports as an unapplied opening credit note. */
			if (row->paid != NULL || !venture_money_is_zero(row->tax))
			{
				row_error(plan, row, "is negative, so it imports as a customer credit; it cannot also carry paid or tax");
				continue;
			}
			row->open = venture_money_copy(row->total);
			continue;
		}
		if (venture_money_get_amount(row->tax) < 0 || venture_money_get_amount(row->net) < 0)
		{
			row_error(plan, row, "has a negative net or tax on a positive invoice");
			continue;
		}
		if (row->paid == NULL)
			row->paid = venture_money_new_zero(venture_money_get_currency(row->total));
		row->open = money_difference(row->total, row->paid);
		if (row->open == NULL || venture_money_get_amount(row->paid) < 0 || venture_money_get_amount(row->open) <= 0)
		{
			row_error(plan, row, "must have a nonnegative paid amount below its total; a settled invoice is not open (rule: open-ar-balance)");
			continue;
		}
		if (balance != NULL && !venture_money_equal(balance, row->open))
			row_error(plan, row, "declares a balance that is not its total less the paid amount (rule: open-ar-balance)");
	}
}

static void
parse_open_ap(Plan *plan, JsonArray *array)
{
	guint i, l;
	for (i = 0; array != NULL && i < json_array_get_length(array); i++)
	{
		JsonObject *object = NULL;
		Row *row = plan_row(plan, "open_ap", array, i, &object);
		JsonNode *lines_node;
		const gchar *currency;
		g_autoptr(VentureMoney) balance = NULL;
		g_ptr_array_add(plan->open_ap, row);
		if (object == NULL)
			continue;
		currency = row_currency(object, plan);
		if (row->source_id == NULL)
			row_error(plan, row, "source_id is required");
		parse_party(plan, row, object, TRUE);
		row->number = field_text(plan, row, object, "", "number", TRUE);
		row->date = field_date(plan, row, object, "date", TRUE);
		row->due = field_date(plan, row, object, "due_date", FALSE);
		row->memo = field_text(plan, row, object, "", "memo", FALSE);
		require_before_cutoff(plan, row, "date", row->date);
		row->paid = field_money(plan, row, object, "", "paid", currency, FALSE);
		balance = field_money(plan, row, object, "", "balance", currency, FALSE);
		row->lines = g_ptr_array_new_with_free_func(bill_line_free);
		lines_node = member(object, "lines");
		if (lines_node == NULL || !JSON_NODE_HOLDS_ARRAY(lines_node) || json_array_get_length(json_node_get_array(lines_node)) == 0)
		{
			row_error(plan, row, "needs at least one line (rule: open-ap-lines)");
			continue;
		}
		for (l = 0; l < json_array_get_length(json_node_get_array(lines_node)); l++)
		{
			JsonNode *line_node = json_array_get_element(json_node_get_array(lines_node), l);
			g_autofree gchar *prefix = g_strdup_printf("lines[%u] ", l);
			g_autofree gchar *code = NULL;
			JsonObject *line_object;
			BillLine *line;
			if (!JSON_NODE_HOLDS_OBJECT(line_node))
			{
				row_error(plan, row, "%smust be an object", prefix);
				continue;
			}
			line_object = json_node_get_object(line_node);
			line = g_new0(BillLine, 1);
			g_ptr_array_add(row->lines, line);
			line->description = field_text(plan, row, line_object, prefix, "description", FALSE);
			line->category = field_text(plan, row, line_object, prefix, "category", FALSE);
			line->amount = field_money(plan, row, line_object, prefix, "amount", currency, TRUE);
			line->tax = field_money(plan, row, line_object, prefix, "tax", currency, FALSE);
			code = field_text(plan, row, line_object, prefix, "account_code", FALSE);
			if (code != NULL)
				line->account_code = plan_account(plan, row, prefix, "account_code", code);
			if (line->amount != NULL && venture_money_get_amount(line->amount) <= 0)
				row_error(plan, row, "%samount must be positive; record a supplier credit under credits with kind vendor", prefix);
			if (line->tax != NULL && venture_money_get_amount(line->tax) < 0)
				row_error(plan, row, "%stax must not be negative", prefix);
			if (line->amount != NULL && line->tax == NULL)
				line->tax = venture_money_new_zero(venture_money_get_currency(line->amount));
			if (line->amount != NULL)
			{
				VentureMoney *next;
				if ((row->total != NULL && !same_currency(row->total, line->amount)) || !same_currency(line->amount, line->tax))
				{
					row_error(plan, row, "%smixes currencies; a bill has one currency", prefix);
					continue;
				}
				next = money_sum(row->total, line->amount);
				g_clear_pointer(&row->total, venture_money_free);
				row->total = money_sum(next, line->tax);
				venture_money_free(next);
				next = money_sum(row->tax, line->tax);
				g_clear_pointer(&row->tax, venture_money_free);
				row->tax = next;
			}
		}
		if (!row->ok || row->total == NULL)
			continue;
		if (!same_currency(row->total, row->paid) || !same_currency(row->total, balance))
		{
			row_error(plan, row, "paid or balance is not in the bill's currency");
			continue;
		}
		if (row->paid == NULL)
			row->paid = venture_money_new_zero(venture_money_get_currency(row->total));
		row->open = money_difference(row->total, row->paid);
		if (row->open == NULL || venture_money_get_amount(row->paid) < 0 || venture_money_get_amount(row->open) <= 0)
		{
			row_error(plan, row, "must have a nonnegative paid amount below its total; a settled bill is not open (rule: open-ap-balance)");
			continue;
		}
		if (balance != NULL && !venture_money_equal(balance, row->open))
			row_error(plan, row, "declares a balance that is not its lines less the paid amount (rule: open-ap-balance)");
	}
}

static void
parse_credits(Plan *plan, JsonArray *array)
{
	guint i;
	for (i = 0; array != NULL && i < json_array_get_length(array); i++)
	{
		JsonObject *object = NULL;
		Row *row = plan_row(plan, "credits", array, i, &object);
		g_autofree gchar *kind = NULL;
		g_autofree gchar *type = NULL;
		g_ptr_array_add(plan->credits, row);
		if (object == NULL)
			continue;
		if (row->source_id == NULL)
			row_error(plan, row, "source_id is required (rule: credit-identity)");
		kind = field_text(plan, row, object, "", "kind", FALSE);
		type = field_text(plan, row, object, "", "type", FALSE);
		if (kind != NULL && g_strcmp0(kind, "customer") != 0 && g_strcmp0(kind, "vendor") != 0)
		{
			row_error(plan, row, "kind must be customer or vendor (rule: credit-identity)");
			continue;
		}
		if (type != NULL && g_strcmp0(type, "credit_note") != 0 && g_strcmp0(type, "prepayment") != 0)
		{
			row_error(plan, row, "type must be credit_note or prepayment");
			continue;
		}
		row->prepayment = g_strcmp0(type, "prepayment") == 0;
		parse_party(plan, row, object,
			g_strcmp0(kind, "vendor") == 0 || (kind == NULL && member(object, "vendor_source_id") != NULL));
		row->number = field_text(plan, row, object, "", "number", FALSE);
		row->date = field_date(plan, row, object, "date", TRUE);
		require_before_cutoff(plan, row, "date", row->date);
		row->total = field_money(plan, row, object, "", "amount", row_currency(object, plan), TRUE);
		if (member(object, "tax") != NULL)
			row_error(plan, row, "an opening credit carries no tax; the source system already reported it");
		if (row->total != NULL && venture_money_get_amount(row->total) <= 0)
			row_error(plan, row, "amount must be positive");
		if (row->ok)
			row->open = venture_money_copy(row->total);
	}
}

static void
parse_bank(Plan *plan, JsonArray *array)
{
	guint i;
	for (i = 0; array != NULL && i < json_array_get_length(array); i++)
	{
		JsonObject *object = NULL;
		Row *row = plan_row(plan, "bank_balances", array, i, &object);
		g_autofree gchar *code = NULL;
		g_ptr_array_add(plan->bank, row);
		if (object == NULL)
			continue;
		if (row->source_id == NULL)
			row_error(plan, row, "source_id is required");
		row->name = field_text(plan, row, object, "", "name", TRUE);
		code = field_text(plan, row, object, "", "account_code", FALSE);
		row->account_code = plan_account(plan, row, "", "account_code", code != NULL ? code : "1000");
		row->default_account = code == NULL;
		/* A negative balance is a credit card or an overdraft: it credits
		 * the account instead of debiting it. */
		row->total = field_money(plan, row, object, "", "amount", row_currency(object, plan), TRUE);
		if (row->ok)
			row->open = venture_money_copy(row->total);
	}
}

static void
parse_assets(Plan *plan, JsonArray *array)
{
	guint i;
	for (i = 0; array != NULL && i < json_array_get_length(array); i++)
	{
		JsonObject *object = NULL;
		Row *row = plan_row(plan, "assets", array, i, &object);
		const gchar *currency;
		g_autofree gchar *code = NULL;
		g_ptr_array_add(plan->assets, row);
		if (object == NULL)
			continue;
		currency = row_currency(object, plan);
		if (row->source_id == NULL)
			row_error(plan, row, "source_id is required (rule: asset-identity)");
		row->tag = field_text(plan, row, object, "", "tag", FALSE);
		if (row->tag == NULL)
			row->tag = g_strdup(row->source_id);
		row->name = field_text(plan, row, object, "", "name", FALSE);
		if (row->name == NULL)
			row->name = g_strdup(row->source_id);
		row->category = field_text(plan, row, object, "", "category", FALSE);
		row->method = field_text(plan, row, object, "", "method", FALSE);
		if (row->method == NULL)
			row->method = g_strdup("straight_line");
		if (g_strcmp0(row->method, "straight_line") != 0 && g_strcmp0(row->method, "declining_balance") != 0 &&
			g_strcmp0(row->method, "none") != 0)
			row_error(plan, row, "method must be straight_line, declining_balance or none");
		row->date = field_date(plan, row, object, "in_service_at", TRUE);
		row->acquired = field_date(plan, row, object, "acquired_at", FALSE);
		require_before_cutoff(plan, row, "in_service_at", row->date);
		if (row->acquired == NULL && row->date != NULL)
			row->acquired = g_date_time_ref(row->date);
		if (row->acquired != NULL && row->date != NULL && g_date_time_compare(row->acquired, row->date) > 0)
			row_error(plan, row, "acquired_at is after in_service_at");
		if (field_int(plan, row, object, "useful_life_months", TRUE, &row->life_months) &&
			(row->life_months < 1 || row->life_months > 1200))
			row_error(plan, row, "useful_life_months must be 1 to 1200 (rule: asset-identity)");
		row->total = field_money(plan, row, object, "", "cost", currency, TRUE);
		row->salvage = field_money(plan, row, object, "", "salvage", currency, FALSE);
		row->accumulated = field_money(plan, row, object, "", "accumulated_depreciation", currency, FALSE);
		code = field_text(plan, row, object, "", "asset_account_code", TRUE);
		row->account_code = plan_account(plan, row, "", "asset_account_code", code);
		g_clear_pointer(&code, g_free);
		code = field_text(plan, row, object, "", "accumulated_depreciation_account_code", TRUE);
		row->accumulated_code = plan_account(plan, row, "", "accumulated_depreciation_account_code", code);
		g_clear_pointer(&code, g_free);
		code = field_text(plan, row, object, "", "depreciation_expense_account_code", TRUE);
		row->expense_code = plan_account(plan, row, "", "depreciation_expense_account_code", code);
		if (row->total == NULL)
			continue;
		if (row->salvage == NULL)
			row->salvage = venture_money_new_zero(venture_money_get_currency(row->total));
		if (row->accumulated == NULL)
			row->accumulated = venture_money_new_zero(venture_money_get_currency(row->total));
		if (!same_currency(row->total, row->salvage) || !same_currency(row->total, row->accumulated))
		{
			row_error(plan, row, "cost, salvage and accumulated_depreciation must share a currency");
			continue;
		}
		if (venture_money_get_amount(row->total) < 0 || venture_money_get_amount(row->salvage) < 0 ||
			venture_money_compare(row->salvage, row->total) > 0)
			row_error(plan, row, "needs a nonnegative cost with salvage between zero and cost");
		else
		{
			g_autoptr(VentureMoney) basis = venture_money_subtract(row->total, row->salvage, NULL);
			if (venture_money_get_amount(row->accumulated) < 0 || venture_money_compare(row->accumulated, basis) > 0)
				row_error(plan, row, "accumulated depreciation must lie between zero and cost less salvage (rule: opening-accumulated-within-basis)");
		}
		if (row->ok)
			row->open = venture_money_subtract(row->total, row->accumulated, NULL);
	}
}

static void
parse_trial_balance(Plan *plan, JsonArray *array)
{
	guint i;
	for (i = 0; array != NULL && i < json_array_get_length(array); i++)
	{
		JsonObject *object = NULL;
		Row *row = plan_row(plan, "trial_balance", array, i, &object);
		const gchar *currency;
		g_autofree gchar *code = NULL;
		g_autoptr(VentureMoney) debit = NULL;
		g_autoptr(VentureMoney) credit = NULL;
		g_ptr_array_add(plan->trial_balance, row);
		if (object == NULL)
			continue;
		currency = row_currency(object, plan);
		code = field_text(plan, row, object, "", "account_code", TRUE);
		if (row->source_id == NULL && code != NULL)
			row->source_id = g_strdup(code);
		row->account_code = plan_account(plan, row, "", "account_code", code);
		row->name = field_text(plan, row, object, "", "name", FALSE);
		debit = field_money(plan, row, object, "", "debit", currency, FALSE);
		credit = field_money(plan, row, object, "", "credit", currency, FALSE);
		if (debit == NULL && credit == NULL && row->ok)
		{
			row_error(plan, row, "needs debit or credit");
			continue;
		}
		if ((debit != NULL && venture_money_get_amount(debit) < 0) || (credit != NULL && venture_money_get_amount(credit) < 0) ||
			!same_currency(debit, credit))
		{
			row_error(plan, row, "debit and credit must be nonnegative amounts in one currency; a credit balance goes under credit");
			continue;
		}
		if (row->ok)
			row->total = money_difference(debit, credit);
	}
}

/* Stock on hand at the cutoff, received into an inventory item that already
 * exists: items belong to products, and products are not opening balances. */
static void
parse_inventory(Plan *plan, JsonArray *array)
{
	guint i;
	for (i = 0; array != NULL && i < json_array_get_length(array); i++)
	{
		JsonObject *object = NULL;
		Row *row = plan_row(plan, "inventory", array, i, &object);
		g_ptr_array_add(plan->inventory, row);
		if (object == NULL)
			continue;
		if (row->source_id == NULL)
			row_error(plan, row, "source_id is required");
		row->tag = field_text(plan, row, object, "", "sku", TRUE);
		row->category = field_text(plan, row, object, "", "location", FALSE);
		if (field_int(plan, row, object, "quantity", TRUE, &row->quantity) && row->quantity <= 0)
			row_error(plan, row, "quantity must be a positive whole number");
		row->net = field_money(plan, row, object, "", "unit_cost", row_currency(object, plan), TRUE);
		if (row->net != NULL && venture_money_get_amount(row->net) <= 0)
			row_error(plan, row, "unit_cost must be positive");
		if (!row->ok)
			continue;
		row->total = venture_money_multiply_int(row->net, row->quantity, NULL);
		if (row->total == NULL)
			row_error(plan, row, "quantity times unit_cost overflows");
		else
			row->open = venture_money_copy(row->total);
	}
}

static gboolean
parse_kind(const gchar *text, VentureAccountKind *kind)
{
	static const struct { const gchar *name; VentureAccountKind kind; } kinds[] = {
		{ "asset", VENTURE_ACCOUNT_KIND_ASSET }, { "liability", VENTURE_ACCOUNT_KIND_LIABILITY },
		{ "equity", VENTURE_ACCOUNT_KIND_EQUITY }, { "income", VENTURE_ACCOUNT_KIND_INCOME },
		{ "expense", VENTURE_ACCOUNT_KIND_EXPENSE }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
		if (g_strcmp0(text, kinds[i].name) == 0)
		{
			*kind = kinds[i].kind;
			return TRUE;
		}
	return FALSE;
}

static void
parse_chart(Plan *plan, JsonArray *array)
{
	guint i;
	for (i = 0; array != NULL && i < json_array_get_length(array); i++)
	{
		JsonObject *object = NULL;
		g_autoptr(GPtrArray) holder = g_ptr_array_new_with_free_func(row_free);
		Row *row = plan_row(plan, "chart", array, i, &object);
		g_autofree gchar *code = NULL;
		g_autofree gchar *kind = NULL;
		const gchar *mapped;
		ChartRow *chart;
		gboolean active = TRUE;
		g_ptr_array_add(holder, row);
		if (object == NULL)
			continue;
		code = field_text(plan, row, object, "", "code", TRUE);
		chart = g_new0(ChartRow, 1);
		chart->name = field_text(plan, row, object, "", "name", TRUE);
		kind = field_text(plan, row, object, "", "kind", TRUE);
		if (member(object, "active") != NULL)
			field_bool(plan, row, object, "active", &active);
		chart->active = active;
		if (kind != NULL && !parse_kind(kind, &chart->kind))
			row_error(plan, row, "kind must be asset, liability, equity, income or expense");
		mapped = plan_map_code(plan, code);
		if (mapped != NULL && g_strcmp0(mapped, plan->clearing_code) == 0)
			row_error(plan, row, "code %s is reserved for opening balance clearing", mapped);
		if (!row->ok || mapped == NULL)
		{
			chart_row_free(chart);
			continue;
		}
		chart->code = g_strdup(mapped);
		if (g_hash_table_contains(plan->chart_codes, chart->code))
		{
			row_error(plan, row, "code %s appears twice in chart", chart->code);
			chart_row_free(chart);
			continue;
		}
		{
			g_autoptr(VentureEntity) existing = account_with_code(plan->self, plan->org, chart->code, NULL);
			gint existing_kind = 0;
			if (existing != NULL)
			{
				g_object_get(existing, "kind", &existing_kind, NULL);
				if (existing_kind != (gint)chart->kind)
					row_warning(plan, row, "account %s already exists with another kind; the existing account is kept", chart->code);
			}
		}
		g_ptr_array_add(plan->chart, chart);
		g_hash_table_insert(plan->chart_codes, chart->code, chart);
	}
}

static void
parse_parties(Plan *plan, const gchar *section, GHashTable *index)
{
	JsonArray *array = plan_section(plan, section);
	guint i;
	for (i = 0; array != NULL && i < json_array_get_length(array); i++)
	{
		JsonObject *object = NULL;
		g_autoptr(GPtrArray) holder = g_ptr_array_new_with_free_func(row_free);
		Row *row = plan_row(plan, section, array, i, &object);
		g_ptr_array_add(holder, row);
		if (object == NULL)
			continue;
		if (row->source_id == NULL)
		{
			row_error(plan, row, "source_id is required");
			continue;
		}
		if (g_hash_table_contains(index, row->source_id))
		{
			row_error(plan, row, "source_id appears twice in %s", section);
			continue;
		}
		g_hash_table_insert(index, g_strdup(row->source_id), json_object_ref(object));
	}
}

/* --- Whole-payload checks ------------------------------------------------------ */

static void
check_duplicate_ids(Plan *plan, GPtrArray *rows)
{
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	guint i;
	for (i = 0; i < rows->len; i++)
	{
		Row *row = g_ptr_array_index(rows, i);
		Row *earlier;
		if (row->source_id == NULL)
			continue;
		earlier = g_hash_table_lookup(seen, row->source_id);
		if (earlier != NULL)
			/* Silently skipping the second row made a payload with a
			 * copy-pasted id import one document and reconcile against
			 * two. */
			row_error(plan, row, "source_id also used by %s[%d]; every source row needs its own id",
				earlier->section, earlier->position);
		else
			g_hash_table_insert(seen, row->source_id, row);
	}
}

/* Numbers and tags are unique per organization, not per vendor, so two
 * suppliers' bill "1001" cannot both be stored as 1001. */
static void
check_duplicate_numbers(Plan *plan, GPtrArray *rows, gboolean tags)
{
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	guint i;
	for (i = 0; i < rows->len; i++)
	{
		Row *row = g_ptr_array_index(rows, i);
		const gchar *key = tags ? row->tag : row->number;
		Row *earlier;
		if (key == NULL || !row->ok)
			continue;
		earlier = g_hash_table_lookup(seen, key);
		if (earlier == NULL)
		{
			g_hash_table_insert(seen, (gpointer)key, row);
			continue;
		}
		if (tags)
			row_error(plan, row, "tag %s is also used by assets[%d]; asset tags are unique per organization", key, earlier->position);
		else if (row->vendor)
			row_error(plan, row, "number %s is also used by open_ap[%d] (vendor %s); bill numbers are unique per organization, so make one distinct, for example %s-%s",
				key, earlier->position, earlier->party_source != NULL ? earlier->party_source : earlier->party_name,
				key, row->party_source != NULL ? row->party_source : "2");
		else
			row_error(plan, row, "number %s is also used by open_ar[%d]; invoice numbers are unique per organization", key, earlier->position);
	}
}

static void
check_trial_balance(Plan *plan)
{
	g_autoptr(GHashTable) debits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(GHashTable) accounts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GHashTableIter iter;
	gpointer key, value;
	guint i;
	for (i = 0; i < plan->trial_balance->len; i++)
	{
		Row *row = g_ptr_array_index(plan->trial_balance, i);
		g_autofree gchar *slot = NULL;
		VentureMoney *sum;
		const gchar *currency;
		if (!row->ok || row->total == NULL)
			continue;
		currency = venture_money_get_currency(row->total);
		slot = g_strdup_printf("%s|%s", row->account_code, currency);
		if (g_hash_table_contains(accounts, slot))
		{
			row_error(plan, row, "account %s appears twice in trial_balance for %s", row->account_code, currency);
			continue;
		}
		g_hash_table_insert(accounts, g_steal_pointer(&slot), GINT_TO_POINTER(1));
		sum = money_sum(g_hash_table_lookup(debits, currency), row->total);
		g_hash_table_insert(debits, g_strdup(currency), sum);
	}
	g_hash_table_iter_init(&iter, debits);
	while (g_hash_table_iter_next(&iter, &key, &value))
		if (!venture_money_is_zero(value))
		{
			g_autofree gchar *text = venture_money_to_string(value);
			plan_note(plan, "trial_balance", -1, NULL, FALSE,
				"debits exceed credits by %s in %s; a trial balance must balance", text, (const gchar *)key);
		}
}

static void
check_bank(Plan *plan)
{
	g_autoptr(GHashTable) names = g_hash_table_new(g_str_hash, g_str_equal);
	Row *defaulted = NULL;
	guint i;
	for (i = 0; i < plan->bank->len; i++)
	{
		Row *row = g_ptr_array_index(plan->bank, i);
		if (row->name != NULL)
		{
			Row *earlier = g_hash_table_lookup(names, row->name);
			if (earlier != NULL)
				row_error(plan, row, "name %s is also used by bank_balances[%d]", row->name, earlier->position);
			else
				g_hash_table_insert(names, row->name, row);
		}
		/* Two accounts silently defaulting to 1000 would reconcile as one
		 * total; with explicit codes they are compared per GL account. */
		if (row->default_account)
		{
			if (defaulted != NULL)
				row_error(plan, row, "has no account_code, like bank_balances[%d]; name the GL account for each bank", defaulted->position);
			else
				defaulted = row;
		}
	}
}

/*
 * Builds the typed plan. A GError is only for a payload that cannot be read
 * at all -- no source, no cutoff, a module switched off, over the row limit;
 * everything row-shaped goes into plan->problems.
 */
static Plan *
plan_build(VentureCutoverService *self, gint64 org, JsonObject *payload, GError **error)
{
	static const gchar *const sections[] = {
		"open_ar", "open_ap", "credits", "bank_balances", "assets", "trial_balance", "inventory", "chart", "customers", "vendors", "items"
	};
	g_autoptr(Plan) plan = g_new0(Plan, 1);
	g_autoptr(GDateTime) now = venture_time_now();
	JsonNode *node;
	guint i;
	if (payload == NULL)
	{
		refuse(error, "mapped source payload is required");
		return NULL;
	}
	plan->self = self;
	plan->payload = json_object_ref(payload);
	plan->org = org;
	plan->problems = g_ptr_array_new_with_free_func(problem_free);
	plan->chart = g_ptr_array_new_with_free_func(chart_row_free);
	plan->chart_codes = g_hash_table_new(g_str_hash, g_str_equal);
	plan->customers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_object_unref);
	plan->vendors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_object_unref);
	plan->open_ar = g_ptr_array_new_with_free_func(row_free);
	plan->open_ap = g_ptr_array_new_with_free_func(row_free);
	plan->credits = g_ptr_array_new_with_free_func(row_free);
	plan->bank = g_ptr_array_new_with_free_func(row_free);
	plan->assets = g_ptr_array_new_with_free_func(row_free);
	plan->trial_balance = g_ptr_array_new_with_free_func(row_free);
	plan->inventory = g_ptr_array_new_with_free_func(row_free);
	node = member(payload, "source");
	plan->source = node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
		? g_strdup(json_node_get_string(node)) : NULL;
	if (g_strcmp0(plan->source, "zoho_books") != 0 && g_strcmp0(plan->source, "quickbooks") != 0 &&
		g_strcmp0(plan->source, "xero") != 0 && g_strcmp0(plan->source, "generic") != 0)
	{
		refuse(error, "source must be zoho_books, quickbooks, xero or generic");
		return NULL;
	}
	node = member(payload, "cutoff");
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING ||
		!g_ascii_isdigit(json_node_get_string(node)[0]))
	{
		refuse(error, "cutoff must be an ISO date such as 2026-01-01");
		return NULL;
	}
	plan->cutoff = venture_time_from_string(json_node_get_string(node), error);
	if (plan->cutoff == NULL)
		return NULL;
	/* Opening documents and payments are dated at the cutoff, and no
	 * financial event may be dated in the future. */
	if (g_date_time_compare(plan->cutoff, now) > 0)
	{
		refuse(error, "cutoff is in the future; import opening balances once the cutoff has passed");
		return NULL;
	}
	node = member(payload, "currency");
	if (node != NULL)
	{
		const gchar *code = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
			? json_node_get_string(node) : NULL;
		if (code == NULL || !venture_currency_is_valid(code) || !g_ascii_isupper(code[0]) ||
			!g_ascii_isupper(code[1]) || !g_ascii_isupper(code[2]))
		{
			refuse(error, "currency must be an uppercase ISO 4217 code");
			return NULL;
		}
		plan->currency = g_strdup(code);
	}
	plan->decimal_separator = '.';
	for (i = 0; i < 2; i++)
	{
		const gchar *name = i == 0 ? "decimal_separator" : "thousands_separator";
		const gchar *text;
		node = member(payload, name);
		if (node == NULL)
			continue;
		text = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
		if (text == NULL || strlen(text) != 1 || strchr(i == 0 ? ".," : ".,' ", text[0]) == NULL)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureCutoverService: %s must be one of %s", name, i == 0 ? "\".\" or \",\"" : "\".\", \",\", \"'\" or \" \"");
			return NULL;
		}
		if (i == 0)
			plan->decimal_separator = text[0];
		else
			plan->thousands_separator = text[0];
	}
	if (plan->thousands_separator == plan->decimal_separator)
	{
		refuse(error, "decimal_separator and thousands_separator must differ");
		return NULL;
	}
	node = member(payload, "clearing_account_code");
	plan->clearing_code = g_strdup(node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
		json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : CUTOVER_CLEARING_CODE);
	node = member(payload, "account_map");
	if (node != NULL)
	{
		GList *names, *l;
		if (!JSON_NODE_HOLDS_OBJECT(node))
		{
			refuse(error, "account_map must be an object of source code to VENTURE code");
			return NULL;
		}
		names = json_object_get_members(json_node_get_object(node));
		for (l = names; l != NULL; l = l->next)
		{
			JsonNode *target = json_object_get_member(json_node_get_object(node), l->data);
			if (!JSON_NODE_HOLDS_VALUE(target) || json_node_get_value_type(target) != G_TYPE_STRING)
			{
				g_list_free(names);
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
					"VentureCutoverService: account_map member %s must map to a code string", (const gchar *)l->data);
				return NULL;
			}
		}
		g_list_free(names);
		plan->account_map = json_object_ref(json_node_get_object(node));
	}
	for (i = 0; i < G_N_ELEMENTS(sections); i++)
	{
		node = member(payload, sections[i]);
		if (node == NULL)
			continue;
		if (!JSON_NODE_HOLDS_ARRAY(node))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureCutoverService: %s must be an array of rows", sections[i]);
			return NULL;
		}
		if (g_strcmp0(sections[i], "items") != 0)
			plan->items += json_array_get_length(json_node_get_array(node));
	}
	if (plan->items > CUTOVER_ROW_LIMIT)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureCutoverService: %u rows is over the %d-row limit for one batch; split the migration into batches, "
			"for example customers and open_ar in one payload and vendors and open_ap in another", plan->items, CUTOVER_ROW_LIMIT);
		return NULL;
	}
	if (!refuse_unimported_sections(payload, error))
		return NULL;
	parse_chart(plan, plan_section(plan, "chart"));
	parse_parties(plan, "customers", plan->customers);
	parse_parties(plan, "vendors", plan->vendors);
	parse_open_ar(plan, plan_section(plan, "open_ar"));
	parse_open_ap(plan, plan_section(plan, "open_ap"));
	parse_credits(plan, plan_section(plan, "credits"));
	parse_bank(plan, plan_section(plan, "bank_balances"));
	parse_assets(plan, plan_section(plan, "assets"));
	parse_trial_balance(plan, plan_section(plan, "trial_balance"));
	parse_inventory(plan, plan_section(plan, "inventory"));
	check_duplicate_ids(plan, plan->open_ar);
	check_duplicate_ids(plan, plan->inventory);
	check_duplicate_ids(plan, plan->open_ap);
	check_duplicate_ids(plan, plan->credits);
	check_duplicate_ids(plan, plan->bank);
	check_duplicate_ids(plan, plan->assets);
	check_duplicate_numbers(plan, plan->open_ar, FALSE);
	check_duplicate_numbers(plan, plan->open_ap, FALSE);
	check_duplicate_numbers(plan, plan->assets, TRUE);
	check_trial_balance(plan);
	check_bank(plan);
	{
		JsonArray *items = plan_section(plan, "items");
		if (items != NULL && json_array_get_length(items) > 0)
			plan_note(plan, "items", -1, NULL, TRUE,
				"%u items are not imported; products and services are not opening balances", json_array_get_length(items));
	}
	return g_steal_pointer(&plan);
}
