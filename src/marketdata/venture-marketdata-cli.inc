/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-marketdata-cli.inc - venturectl market quote|promote|browse|
 * deals|venues|instrument|watchlist|alerts
 *
 * Thin calls on /api/v1/market. The records need no verbs of their own
 * -- `list instrument`, `create watchlist_entry ...` -- these are what is
 * not a record: asking the price oracle, promoting a store's venue or
 * instrument into a record, and the Trading pages' JSON twins. Each verb
 * asks exactly the question its page asks, under the page's own query
 * names, so the CLI and the page cannot answer differently. Included by
 * src/cli/main.c, which builds without the server.
 *
 * The table helpers at the top are shared with the arbitrage and plugins
 * verbs (included later in main.c): an answer that is not a list of
 * records has no generic table, so each verb names its columns.
 */

/* --- Tables of answers ------------------------------------------------------ */

typedef enum
{
	VENTURE_CLI_CELL_TEXT = 0,
	VENTURE_CLI_CELL_MONEY,		/* a money object: its formatted text */
	VENTURE_CLI_CELL_NUMBER,	/* an integer, or a double to four figures */
	VENTURE_CLI_CELL_PERCENT,	/* a double that is already a percent */
	VENTURE_CLI_CELL_FRACTION,	/* a ratio: 0.25 shows as 25.0% */
	VENTURE_CLI_CELL_INDEX		/* the row's 1-based position */
} VentureCliCellKind;

typedef struct
{
	const gchar		*member;	/* "net", or "buy.venue_key" one level down */
	const gchar		*header;
	VentureCliCellKind	 kind;
	gboolean		 whole;		/* never truncated: a key a later verb takes */
} VentureCliColumn;

/* The widest a truncated cell is drawn, as the record table does. */
#define VENTURE_CLI_CELL_WIDTH (40)

/* @member of @object, one dot of nesting allowed; NULL when absent. */
static JsonNode *
venture_cli_member(
	JsonObject	*object,
	const gchar	*member
){
	const gchar *dot;

	if (NULL == object)
		return NULL;

	dot = strchr(member, '.');

	if (NULL != dot)
	{
		g_autofree gchar *outer = g_strndup(member, (gsize)(dot - member));
		JsonNode *node = json_object_get_member(object, outer);

		if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
			return NULL;

		return json_object_get_member(json_node_get_object(node), dot + 1);
	}

	return json_object_get_member(object, member);
}

/*
 * One cell as text. Absent and null are empty, never "0": an unquoted
 * figure in an opportunity is blank on purpose, and a table that drew it
 * as zero would say the opposite of the answer.
 */
static gchar *
venture_cli_cell(
	JsonObject		*object,
	const VentureCliColumn	*column,
	guint			 index
){
	JsonNode *node;

	if (VENTURE_CLI_CELL_INDEX == column->kind)
		return g_strdup_printf("%u", index + 1);

	node = venture_cli_member(object, column->member);

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return g_strdup("");

	if (JSON_NODE_HOLDS_OBJECT(node))
	{
		JsonObject *inner = json_node_get_object(node);

		if (json_object_has_member(inner, "formatted"))
			return g_strdup(json_object_get_string_member(inner, "formatted"));

		return venture_json_to_string(node, FALSE);
	}

	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);
		g_autoptr(GString) text = g_string_new(NULL);
		guint i;

		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonNode *element = json_array_get_element(array, i);
			g_autofree gchar *part = NULL;

			part = (JSON_NODE_HOLDS_VALUE(element) &&
			        (G_TYPE_STRING == json_node_get_value_type(element)))
				? g_strdup(json_node_get_string(element))
				: venture_json_to_string(element, FALSE);
			g_string_append_printf(text, "%s%s", (i > 0) ? ", " : "", part);
		}

		return g_string_free(g_steal_pointer(&text), FALSE);
	}

	switch (json_node_get_value_type(node))
	{
	case G_TYPE_STRING:
		return g_strdup(json_node_get_string(node));

	case G_TYPE_BOOLEAN:
		return g_strdup(json_node_get_boolean(node) ? "yes" : "no");

	case G_TYPE_INT64:
		if (VENTURE_CLI_CELL_FRACTION == column->kind)
			return g_strdup_printf("%" G_GINT64_FORMAT "00%%", json_node_get_int(node));
		if (VENTURE_CLI_CELL_PERCENT == column->kind)
			return g_strdup_printf("%" G_GINT64_FORMAT "%%", json_node_get_int(node));
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));

	case G_TYPE_DOUBLE:
		if (VENTURE_CLI_CELL_FRACTION == column->kind)
			return g_strdup_printf("%.1f%%", json_node_get_double(node) * 100.0);
		if (VENTURE_CLI_CELL_PERCENT == column->kind)
			return g_strdup_printf("%.1f%%", json_node_get_double(node));
		return g_strdup_printf("%.4g", json_node_get_double(node));

	default:
		return venture_json_to_string(node, FALSE);
	}
}

/* Prints @text padded to @width characters (not bytes: names are UTF-8). */
static void
venture_cli_print_padded(
	const gchar	*text,
	gsize		 width
){
	glong length = g_utf8_strlen(text, -1);

	g_print("%s", text);

	while ((gsize)length < width)
	{
		g_print(" ");
		length++;
	}
}

/* @rows (objects) as an aligned table of @columns. */
static void
venture_cli_print_table(
	JsonArray		*rows,
	const VentureCliColumn	*columns
){
	g_autofree gsize *widths = NULL;
	guint n_columns;
	guint i;
	guint j;

	for (n_columns = 0; NULL != columns[n_columns].header; n_columns++)
		;

	widths = g_new0(gsize, n_columns);

	for (i = 0; i < n_columns; i++)
		widths[i] = (gsize)g_utf8_strlen(columns[i].header, -1);

	for (j = 0; j < json_array_get_length(rows); j++)
	{
		JsonObject *row = json_array_get_object_element(rows, j);

		for (i = 0; i < n_columns; i++)
		{
			g_autofree gchar *cell = venture_cli_cell(row, &columns[i], j);
			gsize length = (gsize)g_utf8_strlen(cell, -1);

			if (!columns[i].whole)
				length = MIN(length, (gsize)VENTURE_CLI_CELL_WIDTH);

			widths[i] = MAX(widths[i], length);
		}
	}

	for (i = 0; i < n_columns; i++)
	{
		venture_cli_print_padded(columns[i].header, widths[i]);
		g_print((i + 1 < n_columns) ? "  " : "\n");
	}

	for (j = 0; j < json_array_get_length(rows); j++)
	{
		JsonObject *row = json_array_get_object_element(rows, j);

		for (i = 0; i < n_columns; i++)
		{
			g_autofree gchar *cell = venture_cli_cell(row, &columns[i], j);
			g_autofree gchar *shown = NULL;

			shown = columns[i].whole ? g_strdup(cell) : venture_truncate(cell, VENTURE_CLI_CELL_WIDTH);
			venture_cli_print_padded((NULL != shown) ? shown : "", widths[i]);
			g_print((i + 1 < n_columns) ? "  " : "\n");
		}
	}
}

/* Labelled figures of one object, one per line: a calculator's answer. */
static void
venture_cli_print_figures(
	JsonObject		*object,
	const VentureCliColumn	*figures
){
	gsize width = 0;
	guint i;

	for (i = 0; NULL != figures[i].header; i++)
		width = MAX(width, (gsize)g_utf8_strlen(figures[i].header, -1));

	for (i = 0; NULL != figures[i].header; i++)
	{
		g_autofree gchar *cell = venture_cli_cell(object, &figures[i], 0);

		if ('\0' == cell[0])
			continue;

		venture_cli_print_padded(figures[i].header, width);
		g_print("  %s\n", cell);
	}
}

/* An answer's notes, one per line, after its table. */
static void
venture_cli_print_notes(
	VentureCli	*cli,
	JsonObject	*answer
){
	JsonNode *node;
	JsonArray *notes;
	guint i;

	node = (NULL != answer) ? json_object_get_member(answer, "notes") : NULL;

	if (cli->quiet || (NULL == node) || !JSON_NODE_HOLDS_ARRAY(node))
		return;

	notes = json_node_get_array(node);

	for (i = 0; i < json_array_get_length(notes); i++)
	{
		JsonNode *note = json_array_get_element(notes, i);

		if (JSON_NODE_HOLDS_VALUE(note) && (G_TYPE_STRING == json_node_get_value_type(note)))
			g_print("note: %s\n", json_node_get_string(note));
	}
}

/*
 * Prints an answer whose interesting part is the array @member: a table
 * of @columns on a terminal, the rows as CSV for -f csv, and the whole
 * answer -- notes, echoes and all -- for json and yaml, so a script never
 * loses what the table leaves out. @member NULL means @answer is itself
 * the array.
 */
static void
venture_cli_print_answer(
	VentureCli		*cli,
	JsonNode		*answer,
	const gchar		*member,
	const VentureCliColumn	*columns,
	const gchar		*empty
){
	JsonObject *root = NULL;
	JsonNode *rows_node;
	JsonArray *rows = NULL;

	if (JSON_NODE_HOLDS_OBJECT(answer))
		root = json_node_get_object(answer);

	rows_node = (NULL == member) ? answer
	          : (NULL != root) ? json_object_get_member(root, member) : NULL;

	if ((NULL != rows_node) && JSON_NODE_HOLDS_ARRAY(rows_node))
		rows = json_node_get_array(rows_node);

	if ((VENTURE_OUTPUT_FORMAT_JSON == cli->format) || (VENTURE_OUTPUT_FORMAT_YAML == cli->format))
	{
		venture_cli_output(cli, answer);
		return;
	}

	if (VENTURE_OUTPUT_FORMAT_CSV == cli->format)
	{
		g_autoptr(JsonNode) wrapped = json_node_new(JSON_NODE_OBJECT);
		JsonObject *object = json_object_new();

		json_object_set_array_member(object, "records",
		                             (NULL != rows) ? json_array_ref(rows) : json_array_new());
		json_node_take_object(wrapped, object);
		venture_cli_print_csv(wrapped);
		return;
	}

	if ((NULL == rows) || (0 == json_array_get_length(rows)))
	{
		if (!cli->quiet)
			g_print("%s\n", empty);
	}
	else
	{
		venture_cli_print_table(rows, columns);
	}

	venture_cli_print_notes(cli, root);

	if (!cli->quiet && (NULL != root) &&
	    json_object_get_boolean_member_with_default(root, "truncated", FALSE))
		g_print("More passed than are shown; raise top or narrow the question.\n");
}

/* --- Typed options ----------------------------------------------------------- */

typedef enum
{
	VENTURE_CLI_OPTION_TEXT = 0,
	VENTURE_CLI_OPTION_INT,		/* a whole number, 0 or more */
	VENTURE_CLI_OPTION_BOOL,	/* true/false; sent as 1, or left out */
	VENTURE_CLI_OPTION_NUMBER	/* a decimal number */
} VentureCliOptionKind;

typedef struct
{
	const gchar		*name;		/* what the CLI accepts */
	const gchar		*wire;		/* the query name the route reads */
	VentureCliOptionKind	 kind;
	const gchar		*choices;	/* "asc|desc", or NULL for any */
} VentureCliOption;

/* The names a verb takes, for a refusal that says what would work. */
static gchar *
venture_cli_option_names(const VentureCliOption *options)
{
	g_autoptr(GString) names = g_string_new(NULL);
	guint i;

	for (i = 0; NULL != options[i].name; i++)
		g_string_append_printf(names, "%s%s", (i > 0) ? ", " : "", options[i].name);

	return g_string_free(g_steal_pointer(&names), FALSE);
}

/*
 * Reads one `name=value` against @options, typed: a whole number, a
 * boolean or a choice is refused here, by name, before anything is sent.
 * The route would refuse most of them too, but a page reads an unknown
 * `dir` as ascending and a false `stock` as unset, so the CLI cannot
 * leave typing to it. Returns the wire name and the value to send (NULL
 * value: send nothing, as for a false flag).
 */
static gboolean
venture_cli_option_read(
	const gchar		 *verb,
	const gchar		 *arg,
	const VentureCliOption	 *options,
	const gchar		**out_wire,
	gchar			**out_value,
	GError			**error
){
	g_autofree gchar *name = NULL;
	const gchar *equals;
	const gchar *value;
	guint i;

	equals = strchr(arg, '=');

	if (NULL == equals)
	{
		g_autofree gchar *names = venture_cli_option_names(options);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s takes name=value options (%s); \"%s\" is not one", verb, names, arg);
		return FALSE;
	}

	name = g_strndup(arg, (gsize)(equals - arg));
	value = equals + 1;

	for (i = 0; NULL != options[i].name; i++)
		if (0 == g_strcmp0(options[i].name, name))
			break;

	if (NULL == options[i].name)
	{
		g_autofree gchar *names = venture_cli_option_names(options);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s does not take \"%s\"; it takes %s", verb, name, names);
		return FALSE;
	}

	*out_wire = options[i].wire;
	*out_value = NULL;

	switch (options[i].kind)
	{
	case VENTURE_CLI_OPTION_INT:
	{
		gint64 parsed;

		if (!g_ascii_string_to_signed(value, 10, 0, G_MAXINT64, &parsed, NULL))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s is a whole number, not \"%s\"", name, value);
			return FALSE;
		}

		*out_value = g_strdup_printf("%" G_GINT64_FORMAT, parsed);
		return TRUE;
	}

	case VENTURE_CLI_OPTION_BOOL:
		if ((0 == g_ascii_strcasecmp(value, "true")) || (0 == g_strcmp0(value, "1")) ||
		    (0 == g_ascii_strcasecmp(value, "yes")))
		{
			*out_value = g_strdup("1");
			return TRUE;
		}

		if ((0 == g_ascii_strcasecmp(value, "false")) || (0 == g_strcmp0(value, "0")) ||
		    (0 == g_ascii_strcasecmp(value, "no")))
			return TRUE;

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s is true or false, not \"%s\"", name, value);
		return FALSE;

	case VENTURE_CLI_OPTION_NUMBER:
	{
		gchar *end = NULL;

		g_ascii_strtod(value, &end);

		if (('\0' == value[0]) || (NULL == end) || ('\0' != *end))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s is a number, not \"%s\"", name, value);
			return FALSE;
		}

		*out_value = g_strdup(value);
		return TRUE;
	}

	case VENTURE_CLI_OPTION_TEXT:
	default:
		break;
	}

	if (NULL != options[i].choices)
	{
		g_auto(GStrv) choices = g_strsplit(options[i].choices, "|", -1);

		if (!g_strv_contains((const gchar *const *)choices, value))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s is one of %s, not \"%s\"", name, options[i].choices, value);
			return FALSE;
		}
	}

	*out_value = g_strdup(value);
	return TRUE;
}

/* Appends `args[first..]` to @path as a query string, typed by @options. */
static gboolean
venture_cli_query_from_args(
	GString			 *path,
	const gchar		 *verb,
	gchar			**args,
	guint			  first,
	const VentureCliOption	 *options,
	GError			**error
){
	guint i;

	for (i = first; (NULL != args) && (NULL != args[i]); i++)
	{
		g_autofree gchar *value = NULL;
		const gchar *wire = NULL;

		if (!venture_cli_option_read(verb, args[i], options, &wire, &value, error))
			return FALSE;

		if (NULL == value)
			continue;

		g_string_append_c(path, (NULL == strchr(path->str, '?')) ? '?' : '&');
		g_string_append_uri_escaped(path, wire, NULL, FALSE);
		g_string_append_c(path, '=');
		g_string_append_uri_escaped(path, value, NULL, FALSE);
	}

	return TRUE;
}

/* A positive record id from an argument, or a usage refusal. */
static gboolean
venture_cli_positive_id(
	const gchar	 *text,
	const gchar	 *usage,
	gint64		 *out,
	GError		**error
){
	if ((NULL == text) || !g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, out, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s", usage);
		return FALSE;
	}

	return TRUE;
}

/* --- market ------------------------------------------------------------------ */

/* The query-string names `market quote` passes through; `venue` is the
 * friendlier spelling of `where`, and `product` of `product_id`. */
static const gchar *const venture_cli_market_quote_keys[] = {
	"basis", "where", "venue", "venue_id", "product", "product_id", "at",
	"currency", "prefer_currency", "fallback", "source", "organization_id", NULL
};

/*
 * The page filters, under the page's own names. The wire spellings of the
 * reports (data_source_id, group_key, category_path) are accepted too, so
 * an option that worked on `report market_deals` works here.
 */
static const VentureCliOption venture_cli_market_browse_options[] = {
	{ "source", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "data_source_id", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "search", "search", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "category", "category", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "category_path", "category", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "venue", "venue", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "group", "group", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "group_key", "group", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "stock", "stock", VENTURE_CLI_OPTION_BOOL, NULL },
	{ "sort", "sort", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "dir", "dir", VENTURE_CLI_OPTION_TEXT, "asc|desc" },
	{ "page", "page", VENTURE_CLI_OPTION_INT, NULL },
	{ "per_page", "per_page", VENTURE_CLI_OPTION_INT, NULL },
	{ "organization_id", "organization_id", VENTURE_CLI_OPTION_INT, NULL },
	{ NULL, NULL, 0, NULL }
};

static const VentureCliOption venture_cli_market_deals_options[] = {
	{ "source", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "data_source_id", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "venue", "venue", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "sell_venue", "sell_venue", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "venue_group", "venue_group", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "group", "group", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "group_key", "group", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "category", "category", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "category_path", "category", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "min_value", "min_value", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "max_pct", "max_pct", VENTURE_CLI_OPTION_NUMBER, NULL },
	{ "top", "top", VENTURE_CLI_OPTION_INT, NULL },
	{ "organization_id", "organization_id", VENTURE_CLI_OPTION_INT, NULL },
	{ NULL, NULL, 0, NULL }
};

static const VentureCliOption venture_cli_market_venues_options[] = {
	{ "source", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "data_source_id", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "group", "group", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "group_key", "group", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "organization_id", "organization_id", VENTURE_CLI_OPTION_INT, NULL },
	{ NULL, NULL, 0, NULL }
};

static const VentureCliOption venture_cli_market_instrument_options[] = {
	{ "venue", "venue", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "units", "units", VENTURE_CLI_OPTION_INT, NULL },
	{ "organization_id", "organization_id", VENTURE_CLI_OPTION_INT, NULL },
	{ NULL, NULL, 0, NULL }
};

static const VentureCliOption venture_cli_market_alerts_options[] = {
	{ "count", "count", VENTURE_CLI_OPTION_INT, NULL },
	{ "organization_id", "organization_id", VENTURE_CLI_OPTION_INT, NULL },
	{ NULL, NULL, 0, NULL }
};

/* What a store row shows in browse and deals. */
static const VentureCliColumn venture_cli_market_row_columns[] = {
	{ "instrument_key", "key", VENTURE_CLI_CELL_TEXT, TRUE },
	{ "instrument_name", "instrument", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "venue_key", "venue", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "min_price", "min", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "quantity", "qty", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "market_value", "market", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "region_median", "region", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "pct_vs_region", "vs region", VENTURE_CLI_CELL_PERCENT, FALSE },
	{ "sale_rate", "sale rate", VENTURE_CLI_CELL_FRACTION, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_market_deal_columns[] = {
	{ "data_source_id", "source", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "instrument_key", "key", VENTURE_CLI_CELL_TEXT, TRUE },
	{ "instrument_name", "instrument", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "venue_key", "venue", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "min_price", "min", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "deal_price", "deal", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "discount", "discount", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "pct_vs_region", "vs region", VENTURE_CLI_CELL_PERCENT, FALSE },
	{ "quantity", "qty", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "buy_age_seconds", "age s", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "buy_stale", "stale", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "sell.venue_key", "sell at", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "sell.min_price", "sell", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "sell_age_seconds", "sell age s", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "sell_stale", "sell stale", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_market_venue_columns[] = {
	{ "data_source_id", "source", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "venue_key", "venue", VENTURE_CLI_CELL_TEXT, TRUE },
	{ "venue_name", "name", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "group_key", "group", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "instruments", "items", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "pct_cheaper", "cheaper", VENTURE_CLI_CELL_PERCENT, FALSE },
	{ "avg_ratio", "to region", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "listings", "listings", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "age_seconds", "age s", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "overdue", "overdue", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_market_instrument_venue_columns[] = {
	{ "venue_key", "venue", VENTURE_CLI_CELL_TEXT, TRUE },
	{ "venue_name", "name", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "min_price", "min", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "quantity", "qty", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "market_value", "market", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "pct_vs_region", "vs region", VENTURE_CLI_CELL_PERCENT, FALSE },
	{ "taken_at", "taken", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "age_seconds", "age s", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "stale", "stale", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_market_instrument_figures[] = {
	{ "base.min_price", "Min price", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "base.quantity", "Quantity", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "base.listings", "Listings", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "base.market_value", "Market value", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "base.median_14d", "Median, 14 days", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "base.region_median", "Region median", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "base.deal_price", "Deal price", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "base.pct_vs_region", "Against region", VENTURE_CLI_CELL_PERCENT, FALSE },
	{ "base.taken_at", "Taken", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "bulk.cost", "Bulk cost", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "bulk.filled", "Bulk filled", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "bulk.worst_price", "Bulk worst price", VENTURE_CLI_CELL_MONEY, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_market_watchlists_columns[] = {
	{ "id", "id", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "name", "name", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "group_key", "group", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "entries", "entries", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_market_watchlist_columns[] = {
	{ "instrument_name", "instrument", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "best.venue_key", "best venue", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "best.min_price", "best", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "target_buy", "buy at", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "buy_now", "buy now", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "target_sell", "sell at", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "sell_now", "sell now", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "problem", "problem", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_market_rule_columns[] = {
	{ "id", "id", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "name", "rule", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "kind", "kind", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "enabled", "on", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "last_hit_at", "last hit", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_market_hit_columns[] = {
	{ "id", "id", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "rule_name", "rule", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "observed_at", "observed", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "message", "message", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_market_match_columns[] = {
	{ "venue_key", "venue", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "instrument_key", "key", VENTURE_CLI_CELL_TEXT, TRUE },
	{ "message", "message", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_market_evaluate_figures[] = {
	{ "candidates", "Candidates", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "written", "Hits written", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "cooled", "Held by the cooldown", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "over_cap", "Over the cap", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "failed", "Failed", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const gchar venture_cli_market_usage[] =
	"usage: venturectl market VERB ...\n"
	"  quote INSTRUMENT_ID|product=ID [basis=B] [venue=KEY|GROUP] [venue_id=N]\n"
	"        [at=DATE] [currency=C] [prefer_currency=C] [fallback=true] [source=S]\n"
	"  promote SOURCE_ID instrument|venue|account KEY [organization_id=N]\n"
	"  browse [source=N] [search=TEXT] [category=PATH] [venue=KEY] [group=G]\n"
	"         [stock=true] [sort=COLUMN] [dir=asc|desc] [page=N] [per_page=N]\n"
	"  deals [source=N] [venue=KEY] [group=G] [category=PATH]\n"
	"        [min_value=\"10.00 GOLD\"] [max_pct=80] [top=N]\n"
	"  venues [source=N] [group=G]\n"
	"  instrument SOURCE_ID KEY [venue=KEY] [units=N]\n"
	"  watchlist [ID] [organization_id=N]\n"
	"  alerts [count=N]\n"
	"  alerts evaluate RULE_ID [--dry-run] [organization_id=N]\n"
	"Every verb takes organization_id=N: it answers for that organization (default:\n"
	"the active one, which for a token is the default organization).\n"
	"Examples:\n"
	"  venturectl market browse search=peacebloom sort=pct_vs_region\n"
	"  venturectl market deals group=eu min_value=\"10.00 GOLD\" max_pct=80 top=20\n"
	"  venturectl market venues source=1\n"
	"  venturectl market instrument 1 2589 units=200\n"
	"  venturectl market watchlist 3\n"
	"  venturectl market alerts evaluate 7 --dry-run\n"
	"  venturectl market quote 12 basis=region_median venue=eu\n"
	"  venturectl market promote 1 instrument 2589\n"
	"  venturectl market promote 4 account \"Drgold-Thorium Brotherhood\"\n"
	"  venturectl market deals organization_id=2 group=evermoor-eu";

/* GET @path and print it as @member's table. */
static gint
venture_cli_market_show(
	VentureCli		 *cli,
	const gchar		 *path,
	const gchar		 *member,
	const VentureCliColumn	 *columns,
	const gchar		 *empty,
	GError			**error
){
	g_autoptr(JsonNode) response = NULL;

	response = venture_cli_request(cli, "GET", path, NULL, error);

	if (NULL == response)
		return -1;

	venture_cli_print_answer(cli, response, member, columns, empty);
	return 0;
}

/* market instrument SOURCE KEY: its figures, then every venue's row. */
static gint
venture_cli_market_instrument(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) response = NULL;
	g_autoptr(GString) path = NULL;
	JsonObject *root;
	gint64 source;

	if (!venture_cli_positive_id(args[2], "usage: venturectl market instrument SOURCE_ID KEY "
	                             "[venue=KEY] [units=N]", &source, error) ||
	    (NULL == args[3]) || ('\0' == args[3][0]))
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "usage: venturectl market instrument SOURCE_ID KEY [venue=KEY] [units=N]");
		return -1;
	}

	/* The key is one path segment however many slashes it holds: the
	 * route reads the rest of the path, and the page's own links escape a
	 * '/' the same way. */
	path = g_string_new(NULL);
	g_string_append_printf(path, "/api/v1/market/i/%" G_GINT64_FORMAT "/", source);
	g_string_append_uri_escaped(path, args[3], NULL, FALSE);

	if (!venture_cli_query_from_args(path, "market instrument", args, 4,
	                                 venture_cli_market_instrument_options, error))
		return -1;

	response = venture_cli_request(cli, "GET", path->str, NULL, error);

	if (NULL == response)
		return -1;

	if (VENTURE_OUTPUT_FORMAT_TABLE != cli->format)
	{
		venture_cli_print_answer(cli, response, "venues", venture_cli_market_instrument_venue_columns,
		                         "No venue lists it.");
		return 0;
	}

	root = json_node_get_object(response);

	if (!cli->quiet)
	{
		JsonNode *instrument = json_object_get_member(root, "instrument");
		const gchar *name = NULL;

		if ((NULL != instrument) && JSON_NODE_HOLDS_OBJECT(instrument))
			name = venture_json_object_get_string(json_node_get_object(instrument), "name", NULL);

		g_print("%s (%s) at %s\n", (NULL != name) ? name : args[3], args[3],
		        venture_json_object_get_string(root, "venue_name",
		                                       venture_json_object_get_string(root, "venue", "no venue")));
	}

	venture_cli_print_figures(root, venture_cli_market_instrument_figures);
	g_print("\n");
	venture_cli_print_answer(cli, response, "venues", venture_cli_market_instrument_venue_columns,
	                         "No venue lists it.");
	return 0;
}

/* market alerts [evaluate RULE_ID]: the rules and recent hits, or a test. */
static gint
venture_cli_market_alerts(
	VentureCli	 *cli,
	gchar		**args,
	gboolean	  dry_run,
	GError		**error
){
	g_autoptr(JsonNode) response = NULL;
	g_autoptr(GString) path = NULL;

	if ((NULL != args[2]) && (0 == g_strcmp0(args[2], "evaluate")))
	{
		static const gchar usage[] =
			"usage: venturectl market alerts evaluate RULE_ID [--dry-run] [organization_id=N]";
		g_autofree gchar *base = NULL;
		g_autofree gchar *evaluate = NULL;
		gint64 organization_id;
		gint64 rule;

		if (!venture_cli_positive_id(args[3], usage, &rule, error) ||
		    !venture_cli_organization_arg(args, 4, usage, &organization_id, error))
			return -1;

		/* Without --dry-run the hits are written, under the cooldown, as a
		 * feed run would write them; --dry-run says what would fire. */
		base = g_strdup_printf("/api/v1/market/alerts/%" G_GINT64_FORMAT "/evaluate%s", rule,
		                       dry_run ? "?record=false" : "");
		evaluate = venture_cli_organization_path(base, organization_id);
		response = venture_cli_request(cli, "POST", evaluate, NULL, error);

		if (NULL == response)
			return -1;

		if (VENTURE_OUTPUT_FORMAT_TABLE != cli->format)
		{
			venture_cli_print_answer(cli, response, "matches", venture_cli_market_match_columns, "");
			return 0;
		}

		venture_cli_print_figures(json_node_get_object(response), venture_cli_market_evaluate_figures);
		g_print("\n");
		venture_cli_print_answer(cli, response, "matches", venture_cli_market_match_columns,
		                         dry_run ? "Nothing would fire now." : "Nothing fired.");
		return 0;
	}

	if (dry_run)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "--dry-run belongs to market alerts evaluate");
		return -1;
	}

	path = g_string_new("/api/v1/market/alerts");

	if (!venture_cli_query_from_args(path, "market alerts", args, 2, venture_cli_market_alerts_options,
	                                 error))
		return -1;

	response = venture_cli_request(cli, "GET", path->str, NULL, error);

	if (NULL == response)
		return -1;

	if (VENTURE_OUTPUT_FORMAT_TABLE != cli->format)
	{
		venture_cli_print_answer(cli, response, "hits", venture_cli_market_hit_columns, "");
		return 0;
	}

	venture_cli_print_answer(cli, response, "rules", venture_cli_market_rule_columns, "No alert rules.");
	g_print("\n");
	venture_cli_print_answer(cli, response, "hits", venture_cli_market_hit_columns, "No hits yet.");
	return 0;
}

static gint
venture_cli_command_market(
	VentureCli	 *cli,
	gchar		**args,
	gboolean	  dry_run,
	GError		**error
){
	g_autoptr(JsonNode) response = NULL;
	const gchar *verb;

	verb = args[1];

	if ((NULL != verb) && (0 == g_strcmp0(verb, "help")))
	{
		g_print("%s\n", venture_cli_market_usage);
		return 0;
	}

	if (dry_run && (0 != g_strcmp0(verb, "alerts")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "--dry-run belongs to market alerts evaluate");
		return -1;
	}

	if (0 == g_strcmp0(verb, "alerts"))
		return venture_cli_market_alerts(cli, args, dry_run, error);

	if (0 == g_strcmp0(verb, "instrument"))
		return venture_cli_market_instrument(cli, args, error);

	if ((0 == g_strcmp0(verb, "browse")) || (0 == g_strcmp0(verb, "deals")) ||
	    (0 == g_strcmp0(verb, "venues")))
	{
		g_autoptr(GString) path = NULL;
		gboolean browse = (0 == g_strcmp0(verb, "browse"));
		gboolean deals = (0 == g_strcmp0(verb, "deals"));
		g_autofree gchar *label = g_strconcat("market ", verb, NULL);

		path = g_string_new("/api/v1/market/");
		g_string_append(path, verb);

		if (!venture_cli_query_from_args(path, label, args, 2,
		                                 browse ? venture_cli_market_browse_options
		                                 : deals ? venture_cli_market_deals_options
		                                 : venture_cli_market_venues_options, error))
			return -1;

		return venture_cli_market_show(cli, path->str, browse ? "rows" : deals ? "rows" : "venues",
		                               browse ? venture_cli_market_row_columns
		                               : deals ? venture_cli_market_deal_columns
		                               : venture_cli_market_venue_columns,
		                               browse ? "Nothing listed matches."
		                               : deals ? "No deals now." : "No venues yet.", error);
	}

	if ((0 == g_strcmp0(verb, "watchlist")) || (0 == g_strcmp0(verb, "watchlists")))
	{
		static const gchar usage[] = "usage: venturectl market watchlist [ID] [organization_id=N]";
		g_autofree gchar *base = NULL;
		g_autofree gchar *path = NULL;
		gint64 organization_id;
		gint64 id;

		/* The list of lists when no id comes first. */
		if ((NULL == args[2]) || g_str_has_prefix(args[2], "organization_id="))
		{
			if (!venture_cli_organization_arg(args, 2, usage, &organization_id, error))
				return -1;

			path = venture_cli_organization_path("/api/v1/market/watchlists", organization_id);
			return venture_cli_market_show(cli, path, "watchlists",
			                               venture_cli_market_watchlists_columns, "No watchlists.", error);
		}

		if (!venture_cli_positive_id(args[2], usage, &id, error) ||
		    !venture_cli_organization_arg(args, 3, usage, &organization_id, error))
			return -1;

		base = g_strdup_printf("/api/v1/market/watchlists/%" G_GINT64_FORMAT, id);
		path = venture_cli_organization_path(base, organization_id);
		return venture_cli_market_show(cli, path, "entries", venture_cli_market_watchlist_columns,
		                               "The list is empty.", error);
	}

	if (0 == g_strcmp0(verb, "promote"))
	{
		static const gchar usage[] =
			"usage: venturectl market promote SOURCE_ID instrument|venue|account KEY [organization_id=N]";
		g_autoptr(JsonBuilder) builder = NULL;
		g_autoptr(JsonNode) body = NULL;
		gint64 organization_id;
		gint64 source_id;

		if ((NULL == args[2]) || (NULL == args[3]) || (NULL == args[4]) ||
		    !g_ascii_string_to_signed(args[2], 10, 1, G_MAXINT64, &source_id, NULL) ||
		    ((0 != g_strcmp0(args[3], "instrument")) && (0 != g_strcmp0(args[3], "venue")) &&
		     (0 != g_strcmp0(args[3], "account"))))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, usage);
			return -1;
		}

		if (!venture_cli_organization_arg(args, 5, usage, &organization_id, error))
			return -1;

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "data_source_id");
		json_builder_add_int_value(builder, source_id);
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder, args[3]);
		json_builder_set_member_name(builder, "key");
		json_builder_add_string_value(builder, args[4]);

		if (organization_id > 0)
		{
			json_builder_set_member_name(builder, "organization_id");
			json_builder_add_int_value(builder, organization_id);
		}

		json_builder_end_object(builder);
		body = json_builder_get_root(builder);

		response = venture_cli_request(cli, "POST", "/api/v1/market/promote", body, error);

		if (NULL == response)
			return -1;

		venture_cli_output(cli, response);
		return 0;
	}

	if (0 != g_strcmp0(verb, "quote"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, venture_cli_market_usage);
		return -1;
	}

	{
		g_autoptr(GString) path = g_string_new("/api/v1/market/quote");
		gchar separator = '?';
		gint64 id;
		guint i;

		for (i = 2; NULL != args[i]; i++)
		{
			const gchar *equals = strchr(args[i], '=');
			g_autofree gchar *name = NULL;
			g_autofree gchar *escaped = NULL;

			if ((NULL == equals) &&
			    g_ascii_string_to_signed(args[i], 10, 1, G_MAXINT64, &id, NULL))
			{
				g_string_append_printf(path, "%cinstrument_id=%" G_GINT64_FORMAT, separator, id);
				separator = '&';
				continue;
			}

			name = (NULL != equals) ? g_strndup(args[i], (gsize)(equals - args[i])) : NULL;

			if ((NULL == name) || !g_strv_contains(venture_cli_market_quote_keys, name))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				            "market quote does not take \"%s\"; it takes an instrument id and "
				            "basis, venue, venue_id, product, at, currency, prefer_currency, "
				            "fallback, source and organization_id", args[i]);
				return -1;
			}

			escaped = g_uri_escape_string(equals + 1, NULL, FALSE);
			g_string_append_printf(path, "%c%s=%s", separator,
			                       (0 == g_strcmp0(name, "venue")) ? "where"
			                       : (0 == g_strcmp0(name, "product")) ? "product_id" : name,
			                       escaped);
			separator = '&';
		}

		response = venture_cli_request(cli, "GET", path->str, NULL, error);

		if (NULL == response)
			return -1;

		venture_cli_output(cli, response);
		return 0;
	}
}
