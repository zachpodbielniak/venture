/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * CSV templates and CSV to payload. Included by venture-cutover-service.c.
 *
 * Most source systems export spreadsheets, not the mapped JSON a batch
 * stores. One CSV per section is turned into exactly that JSON, cell by cell
 * as strings, and previewed like any other payload: every rule, every
 * located problem and the strict money grammar still apply, so the CSV path
 * is a second way to write a payload, not a second importer.
 *
 * Parsing the CSV text itself is the web server's RFC 4180 parser; callers
 * hand this file the parsed rows.
 */

typedef struct
{
	const gchar *section;
	const gchar *const *columns;
} CsvSection;

static const gchar *const csv_party_columns[] = {
	"source_id", "name", "email", "phone", "website", "address", "external_id", "terms", "tax_exempt", NULL
};
static const gchar *const csv_chart_columns[] = { "code", "name", "kind", "active", NULL };
static const gchar *const csv_map_columns[] = { "source_code", "venture_code", NULL };
static const gchar *const csv_open_ar_columns[] = {
	"source_id", "customer_source_id", "customer_name", "number", "date", "due_date", "terms", "currency",
	"amount", "net", "tax", "paid", "balance", "tax_exempt", "tax_exempt_reason", "memo", NULL
};
/* One bill per source_id; each CSV row adds a line from its line_ columns. */
static const gchar *const csv_open_ap_columns[] = {
	"source_id", "vendor_source_id", "vendor_name", "number", "date", "due_date", "currency", "paid", "balance",
	"memo", "line_description", "line_amount", "line_tax", "line_account_code", "line_category", NULL
};
static const gchar *const csv_credit_columns[] = {
	"source_id", "kind", "type", "customer_source_id", "customer_name", "vendor_source_id", "vendor_name",
	"number", "date", "currency", "amount", NULL
};
static const gchar *const csv_bank_columns[] = { "source_id", "name", "account_code", "currency", "amount", NULL };
static const gchar *const csv_asset_columns[] = {
	"source_id", "tag", "name", "category", "currency", "cost", "salvage", "accumulated_depreciation",
	"in_service_at", "acquired_at", "method", "useful_life_months", "asset_account_code",
	"accumulated_depreciation_account_code", "depreciation_expense_account_code", NULL
};
static const gchar *const csv_trial_balance_columns[] = { "account_code", "name", "currency", "debit", "credit", NULL };
static const gchar *const csv_inventory_columns[] = { "source_id", "sku", "location", "quantity", "unit_cost", "currency", NULL };

static const CsvSection csv_sections[] = {
	{ "customers", csv_party_columns },
	{ "vendors", csv_party_columns },
	{ "chart", csv_chart_columns },
	{ "account_map", csv_map_columns },
	{ "open_ar", csv_open_ar_columns },
	{ "open_ap", csv_open_ap_columns },
	{ "credits", csv_credit_columns },
	{ "bank_balances", csv_bank_columns },
	{ "assets", csv_asset_columns },
	{ "trial_balance", csv_trial_balance_columns },
	{ "inventory", csv_inventory_columns }
};

static const CsvSection *
csv_section(const gchar *name)
{
	guint i;
	for (i = 0; i < G_N_ELEMENTS(csv_sections); i++)
		if (g_strcmp0(csv_sections[i].section, name) == 0)
			return &csv_sections[i];
	return NULL;
}

gchar *
venture_cutover_csv_template(const gchar *section)
{
	const CsvSection *found = csv_section(section);
	g_autoptr(GString) csv = NULL;
	guint i;
	if (found == NULL)
		return NULL;
	csv = g_string_new(NULL);
	for (i = 0; found->columns[i] != NULL; i++)
		g_string_append_printf(csv, "%s%s", i > 0 ? "," : "", found->columns[i]);
	g_string_append(csv, "\r\n");
	return g_string_free(g_steal_pointer(&csv), FALSE);
}

gchar **
venture_cutover_csv_sections(void)
{
	GStrvBuilder *builder = g_strv_builder_new();
	GStrv sections;
	guint i;
	for (i = 0; i < G_N_ELEMENTS(csv_sections); i++)
		g_strv_builder_add(builder, csv_sections[i].section);
	sections = g_strv_builder_end(builder);
	g_strv_builder_unref(builder);
	return sections;
}

static void csv_problem(GPtrArray *problems, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static void
csv_problem(GPtrArray *problems, const gchar *format, ...)
{
	va_list args;
	va_start(args, format);
	g_ptr_array_add(problems, g_strdup_vprintf(format, args));
	va_end(args);
}

/* Header cells as the generic importer reads them: case, spaces and dashes
 * do not matter, so "Due date" and "due-date" both name due_date. */
static gchar *
csv_column_name(const gchar *header)
{
	gchar *name = g_ascii_strdown(header != NULL ? header : "", -1);
	g_strstrip(name);
	g_strdelimit(name, " -", '_');
	return name;
}

static gboolean
csv_bill_field_conflicts(JsonObject *bill, const gchar *column, const gchar *value)
{
	return json_object_has_member(bill, column) &&
		g_strcmp0(json_object_get_string_member(bill, column), value) != 0;
}

static void
csv_section_rows(JsonObject *payload, const CsvSection *section, GPtrArray *rows, GPtrArray *problems)
{
	g_autofree gint *columns = NULL;
	g_autoptr(GHashTable) bills = g_hash_table_new(g_str_hash, g_str_equal);
	JsonArray *array = NULL;
	JsonObject *map = NULL;
	gchar **header;
	guint i, c, width = 0;
	if (rows->len == 0)
	{
		csv_problem(problems, "%s.csv is empty; it needs at least the header row", section->section);
		return;
	}
	if (rows->len - 1 > CUTOVER_ROW_LIMIT)
	{
		csv_problem(problems, "%s.csv has %u rows, over the %d-row limit; split the migration into batches",
			section->section, rows->len - 1, CUTOVER_ROW_LIMIT);
		return;
	}
	header = g_ptr_array_index(rows, 0);
	width = g_strv_length(header);
	columns = g_new0(gint, width);
	for (c = 0; c < width; c++)
	{
		g_autofree gchar *name = csv_column_name(header[c]);
		guint k;
		columns[c] = -1;
		for (k = 0; section->columns[k] != NULL; k++)
			if (g_strcmp0(section->columns[k], name) == 0)
				columns[c] = (gint)k;
		/* A misspelled column would drop a figure silently. */
		if (columns[c] < 0 && *name != '\0')
			csv_problem(problems, "%s.csv: unknown column \"%s\"; the template lists the columns", section->section, header[c]);
	}
	if (g_strcmp0(section->section, "account_map") == 0)
	{
		if (json_object_has_member(payload, "account_map"))
			map = json_object_get_object_member(payload, "account_map");
		else
		{
			map = json_object_new();
			json_object_set_object_member(payload, "account_map", map);
		}
	}
	else
	{
		array = json_array_new();
		json_object_set_array_member(payload, section->section, array);
	}
	for (i = 1; i < rows->len; i++)
	{
		gchar **cells = g_ptr_array_index(rows, i);
		JsonObject *object = json_object_new();
		JsonObject *line = NULL;
		const gchar *source_id = NULL;
		gboolean ap = g_strcmp0(section->section, "open_ap") == 0;
		if (g_strv_length(cells) > width)
			csv_problem(problems, "%s.csv row %u has more cells than the header", section->section, i + 1);
		for (c = 0; c < width && cells[c] != NULL; c++)
		{
			g_autofree gchar *value = g_strstrip(g_strdup(cells[c]));
			const gchar *column;
			if (columns[c] < 0 || *value == '\0')
				continue;
			column = section->columns[columns[c]];
			if (ap && g_str_has_prefix(column, "line_"))
			{
				if (line == NULL)
					line = json_object_new();
				json_object_set_string_member(line, column + strlen("line_"), value);
			}
			else
				json_object_set_string_member(object, column, value);
			if (g_strcmp0(column, "source_id") == 0)
				source_id = json_object_get_string_member(object, "source_id");
		}
		if (map != NULL)
		{
			if (json_object_has_member(object, "source_code") && json_object_has_member(object, "venture_code"))
				json_object_set_string_member(map, json_object_get_string_member(object, "source_code"),
					json_object_get_string_member(object, "venture_code"));
			else
				csv_problem(problems, "account_map.csv row %u needs source_code and venture_code", i + 1);
			json_object_unref(object);
			continue;
		}
		if (ap && source_id != NULL && g_hash_table_contains(bills, source_id))
		{
			/* A further line of a bill already started: its bill-level
			 * cells must be empty or agree with the first row. */
			JsonObject *bill = g_hash_table_lookup(bills, source_id);
			GList *members = json_object_get_members(object), *l;
			for (l = members; l != NULL; l = l->next)
				if (csv_bill_field_conflicts(bill, l->data, json_object_get_string_member(object, l->data)))
					csv_problem(problems, "open_ap.csv row %u: bill %s has a different %s than its first row",
						i + 1, source_id, (const gchar *)l->data);
			g_list_free(members);
			if (line != NULL)
				json_array_add_object_element(json_object_get_array_member(bill, "lines"), line);
			json_object_unref(object);
			continue;
		}
		if (ap)
		{
			JsonArray *lines = json_array_new();
			if (line != NULL)
				json_array_add_object_element(lines, line);
			json_object_set_array_member(object, "lines", lines);
			if (source_id != NULL)
				g_hash_table_insert(bills, (gpointer)json_object_get_string_member(object, "source_id"), object);
		}
		else if (line != NULL)
			json_object_unref(line);
		json_array_add_object_element(array, object);
	}
}

/* Builds a payload from settings and per-section parsed CSV rows. Every
 * structural problem across every file is reported together. */
JsonObject *
venture_cutover_service_payload_from_csv(VentureCutoverService *self, JsonObject *settings,
	GHashTable *tables, GError **error)
{
	static const gchar *const copied[] = {
		"source", "cutoff", "currency", "decimal_separator", "thousands_separator", "clearing_account_code"
	};
	g_autoptr(JsonObject) payload = json_object_new();
	g_autoptr(GPtrArray) problems = g_ptr_array_new_with_free_func(g_free);
	GHashTableIter iter;
	gpointer key, value;
	guint i;
	g_return_val_if_fail(VENTURE_IS_CUTOVER_SERVICE(self), NULL);
	for (i = 0; settings != NULL && i < G_N_ELEMENTS(copied); i++)
	{
		JsonNode *node = member(settings, copied[i]);
		if (node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING &&
			*json_node_get_string(node) != '\0')
			json_object_set_string_member(payload, copied[i], json_node_get_string(node));
	}
	if (tables != NULL)
	{
		g_hash_table_iter_init(&iter, tables);
		while (g_hash_table_iter_next(&iter, &key, &value))
		{
			const CsvSection *section = csv_section(key);
			if (section == NULL)
			{
				csv_problem(problems, "%s is not a cutover CSV section", (const gchar *)key);
				continue;
			}
			csv_section_rows(payload, section, value, problems);
		}
	}
	if (problems->len > 0)
	{
		g_autoptr(GString) text = g_string_new("VentureCutoverService: the CSV files cannot form a payload:");
		for (i = 0; i < problems->len && i < 20; i++)
			g_string_append_printf(text, "\n- %s", (const gchar *)g_ptr_array_index(problems, i));
		if (problems->len > 20)
			g_string_append_printf(text, "\n- ... and %u more", problems->len - 20);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, text->str);
		return NULL;
	}
	return g_steal_pointer(&payload);
}
