/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#ifdef VENTURE_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

/*
 * The year-end pack is one report whose rows are the lines of every file in
 * it: (file, line, content). That shape is deliberate. A scheduled report
 * pack retains report results as JSON in report_pack.last_output, so a pack
 * that is itself a report result is retained by the machinery that already
 * exists, and the zip an accountant downloads is rebuilt from exactly those
 * retained bytes rather than from the live books a year later.
 */

typedef struct
{
	const gchar *file;
	const gchar *report;
	const gchar *title;
} PackMember;

/* The order is the order an accountant reads them: position first, then
 * performance, then the detail that supports both, then the returns. */
static const PackMember members[] = {
	{ "trial_balance.csv", "trial_balance", "Trial balance at year end" },
	{ "income_statement.csv", "income_statement", "Profit and loss" },
	{ "balance_sheet.csv", "balance_sheet", "Balance sheet" },
	{ "cash_flow.csv", "cash_flow", "Cash-flow statement" },
	{ "receivables_aging.csv", "receivables", "Receivables aging at year end" },
	{ "payables_aging.csv", "payables", "Payables aging at year end" },
	{ "general_ledger.csv", "general_ledger", "General ledger detail" },
	{ "sales_tax.csv", "tax_liability", "Sales-tax return: frozen tax by code" },
	{ "contractor_1099.csv", NULL, "1099 pack summary" },
	{ "fixed_assets.csv", "fixed_assets", "Fixed-asset register with depreciation" }
};

static void
add_lines(VentureReportResult *pack, const gchar *file, const gchar *text)
{
	g_auto(GStrv) lines = g_strsplit(text != NULL ? text : "", "\n", -1);
	guint count = g_strv_length(lines);
	guint i;
	/* A trailing newline splits into an empty last element; it is the
	 * terminator, not a blank line in the file. */
	if (count > 0 && lines[count - 1][0] == '\0')
		count--;
	for (i = 0; i < count; i++)
	{
		venture_report_result_begin_row(pack);
		venture_report_result_set_text(pack, "file", file);
		venture_report_result_set_number(pack, "line", (gdouble)(i + 1));
		venture_report_result_set_text(pack, "content", lines[i]);
	}
}

static gchar *
result_note(VentureReportResult *result)
{
	g_autoptr(JsonNode) node = venture_report_result_to_json(result);
	const gchar *note = venture_json_object_get_string(json_node_get_object(node), "note", NULL);
	return venture_string_is_empty(note) ? NULL : g_strdup(note);
}

/* Which 1099-NEC packs were prepared for the calendar year the fiscal year
 * ends in. The frozen CSV on each pack carries the vendor's TIN and is not
 * copied here; the summary names the vendor, the total and the status. */
static VentureReportResult *
contractor_summary(VentureContext *context, VentureDateRange *period, gint64 org, GError **error)
{
	g_autoptr(VentureReportResult) r = venture_report_result_new("1099 pack summary", period);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) packs = NULL;
	g_autoptr(GDateTime) last_day = g_date_time_add(venture_date_range_get_end(period), -1);
	VentureDatabase *db = venture_context_get_database(context);
	guint i;
	venture_report_result_add_column(r, "vendor_id", "Vendor ID", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(r, "vendor", "Vendor", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(r, "year", "Year", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(r, "form", "Form", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(r, "status", "Status", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(r, "amount", "Paid", VENTURE_REPORT_COLUMN_MONEY);
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "contractor_tax_pack"))
	{
		venture_report_result_set_note(r, "The tax_filing module is off; no 1099 packs are kept.");
		return g_steal_pointer(&r);
	}
	query = venture_query_new(VENTURE_TYPE_CONTRACTOR_TAX_PACK);
	venture_query_set_limit(query, 0);
	if (org > 0)
		venture_query_set_organization(query, org);
	venture_query_add_filter_int(query, "year", VENTURE_FILTER_OP_EQ, g_date_time_get_year(last_day), NULL);
	venture_query_add_order(query, "vendor-id", VENTURE_SORT_ASCENDING, NULL);
	packs = venture_database_find(db, query, error);
	if (packs == NULL)
		return NULL;
	for (i = 0; i < packs->len; i++)
	{
		VentureEntity *pack = g_ptr_array_index(packs, i);
		g_autoptr(VentureEntity) vendor = NULL;
		g_autofree gchar *vendor_name = NULL;
		g_autofree gchar *form = NULL;
		g_autofree gchar *status = NULL;
		g_autofree gchar *vendor_id = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		gint64 vendor_ref = 0;
		gint64 year = 0;
		g_object_get(pack, "vendor-id", &vendor_ref, "year", &year, "form-kind", &form,
			"status", &status, "amount", &amount, NULL);
		if (vendor_ref > 0)
			vendor = venture_database_get(db, VENTURE_TYPE_COMPANY, vendor_ref, NULL);
		if (vendor != NULL)
			g_object_get(vendor, "name", &vendor_name, NULL);
		vendor_id = g_strdup_printf("%" G_GINT64_FORMAT, vendor_ref);
		venture_report_result_begin_row(r);
		venture_report_result_set_text(r, "vendor_id", vendor_id);
		venture_report_result_set_text(r, "vendor", vendor_name != NULL ? vendor_name : "");
		venture_report_result_set_number(r, "year", (gdouble)year);
		venture_report_result_set_text(r, "form", form != NULL ? form : "");
		venture_report_result_set_text(r, "status", status != NULL ? status : "");
		venture_report_result_set_money(r, "amount", amount);
	}
	return g_steal_pointer(&r);
}

static VentureReportResult *
year_end_pack(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureReportResult) pack = venture_report_result_new("Year-end pack", period);
	g_autoptr(GString) index = g_string_new(NULL);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *generated = venture_time_to_string(now);
	g_autoptr(JsonObject) passed = json_object_new();
	VentureReportRegistry *registry = venture_context_get_report_registry(context);
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	guint files = 0;
	guint i;

	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	json_object_set_int_member(passed, "organization_id", org);
	/* Basis, currency and dimension ride through to every member so the
	 * statements agree with each other on what they count. */
	if (options != NULL)
	{
		const gchar *keys[] = { "basis", "currency", "dimension" };
		for (i = 0; i < G_N_ELEMENTS(keys); i++)
			if (json_object_has_member(options, keys[i]))
				json_object_set_member(passed, keys[i], json_node_copy(json_object_get_member(options, keys[i])));
	}

	venture_report_result_add_column(pack, "file", "File", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(pack, "line", "Line", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(pack, "content", "Content", VENTURE_REPORT_COLUMN_TEXT);

	g_string_append_printf(index, "Year-end pack: %s\nOrganization: %" G_GINT64_FORMAT "\nGenerated: %s\n\n",
		venture_date_range_get_label(period), org, generated);

	for (i = 0; i < G_N_ELEMENTS(members); i++)
	{
		g_autoptr(VentureReportResult) result = NULL;
		g_autofree gchar *csv = NULL;
		g_autofree gchar *note = NULL;
		if (members[i].report == NULL)
			result = contractor_summary(context, period, org, error);
		else
		{
			VentureReport *report = venture_report_registry_lookup(registry, members[i].report);
			if (report == NULL)
			{
				/* A module that is off leaves a named gap, never a
				 * pack that looks complete and is not. */
				g_string_append_printf(index, "%s\t%s -- omitted: the %s report is not available (its module is off)\n",
					members[i].file, members[i].title, members[i].report);
				continue;
			}
			result = venture_report_generate(report, context, period, passed, error);
		}
		if (result == NULL)
		{
			g_prefix_error(error, "%s: ", members[i].title);
			return NULL;
		}
		csv = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_CSV);
		add_lines(pack, members[i].file, csv);
		note = result_note(result);
		g_string_append_printf(index, "%s\t%s (%u rows)%s%s\n", members[i].file, members[i].title,
			venture_report_result_get_row_count(result), note != NULL ? " -- " : "", note != NULL ? note : "");
		files++;
	}
	add_lines(pack, "index.txt", index->str);
	venture_report_result_add_metric(pack, venture_metric_new_number("files", "Files", (gdouble)files));
	venture_report_result_add_metric(pack, venture_metric_new_number("lines", "Lines",
		(gdouble)venture_report_result_get_row_count(pack)));
	venture_report_result_set_note(pack, index->str);
	return g_steal_pointer(&pack);
}

void
venture_year_end_pack_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		VENTURE_YEAR_END_PACK_REPORT, "Year-end pack",
		"Trial balance, P&L, balance sheet, cash flow, AR and AP aging, general "
		"ledger, sales-tax and 1099 summaries and the fixed-asset register for a "
		"fiscal year, as the lines of one zip of CSVs plus index.txt",
		year_end_pack)));
}

/* --- The zip -------------------------------------------------------------- */

#ifdef VENTURE_HAVE_LIBARCHIVE
static la_ssize_t
pack_write(struct archive *archive, void *client_data, const void *buffer, size_t length)
{
	(void)archive;
	g_byte_array_append(client_data, buffer, (guint)length);
	return (la_ssize_t)length;
}

static gint
pack_noop(struct archive *archive, void *client_data)
{
	(void)archive;
	(void)client_data;
	return ARCHIVE_OK;
}

static gboolean
pack_add(struct archive *writer, const gchar *path, GString *contents)
{
	struct archive_entry *entry = archive_entry_new();
	gboolean ok;
	archive_entry_set_pathname(entry, path);
	archive_entry_set_size(entry, (la_int64_t)contents->len);
	archive_entry_set_filetype(entry, AE_IFREG);
	archive_entry_set_perm(entry, 0644);
	ok = ARCHIVE_OK == archive_write_header(writer, entry);
	if (ok && contents->len > 0)
		ok = archive_write_data(writer, contents->str, contents->len) == (la_ssize_t)contents->len;
	archive_entry_free(entry);
	return ok;
}
#endif

static void
free_body(gpointer body)
{
	g_string_free(body, TRUE);
}

/* Rows are (file, line, content) in file order; consecutive rows of one
 * file are joined back into that file with the newline each line lost. */
static GBytes *
zip_rows(JsonArray *rows, GError **error)
{
#ifdef VENTURE_HAVE_LIBARCHIVE
	g_autoptr(GByteArray) out = g_byte_array_new();
	g_autoptr(GHashTable) files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_body);
	g_autoptr(GPtrArray) order = g_ptr_array_new_with_free_func(g_free);
	struct archive *writer;
	gboolean ok = TRUE;
	guint i;
	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		const gchar *file = venture_json_object_get_string(row, "file", NULL);
		const gchar *content = venture_json_object_get_string(row, "content", "");
		GString *body;
		if (venture_string_is_empty(file))
			continue;
		body = g_hash_table_lookup(files, file);
		if (body == NULL)
		{
			body = g_string_new(NULL);
			g_hash_table_insert(files, g_strdup(file), body);
			g_ptr_array_add(order, g_strdup(file));
		}
		g_string_append(body, content);
		g_string_append_c(body, '\n');
	}
	if (order->len == 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"The result holds no pack files to archive");
		return NULL;
	}
	writer = archive_write_new();
	archive_write_set_format_zip(writer);
	if (ARCHIVE_OK != archive_write_open2(writer, out, pack_noop, pack_write, pack_noop, pack_noop))
	{
		archive_write_free(writer);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Could not start writing the pack");
		return NULL;
	}
	for (i = 0; ok && i < order->len; i++)
		ok = pack_add(writer, g_ptr_array_index(order, i), g_hash_table_lookup(files, g_ptr_array_index(order, i)));
	archive_write_close(writer);
	archive_write_free(writer);
	if (!ok)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "The pack could not be written");
		return NULL;
	}
	return g_byte_array_free_to_bytes(g_steal_pointer(&out));
#else
	(void)rows;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
		"This build has no libarchive, so it cannot write the year-end pack");
	return NULL;
#endif
}

static gboolean
is_pack_result(JsonObject *result)
{
	JsonArray *columns = json_object_has_member(result, "columns") ? json_object_get_array_member(result, "columns") : NULL;
	gboolean file = FALSE;
	gboolean content = FALSE;
	guint i;
	for (i = 0; columns != NULL && i < json_array_get_length(columns); i++)
	{
		const gchar *key = venture_json_object_get_string(json_array_get_object_element(columns, i), "key", "");
		file = file || 0 == g_strcmp0(key, "file");
		content = content || 0 == g_strcmp0(key, "content");
	}
	return file && content && json_object_has_member(result, "rows");
}

GBytes *
venture_year_end_pack_zip(VentureReportResult *result, GError **error)
{
	g_autoptr(JsonNode) node = NULL;
	g_return_val_if_fail(VENTURE_IS_REPORT_RESULT(result), NULL);
	node = venture_report_result_to_json(result);
	if (!is_pack_result(json_node_get_object(node)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"That result is not a year-end pack");
		return NULL;
	}
	return zip_rows(json_object_get_array_member(json_node_get_object(node), "rows"), error);
}

GBytes *
venture_year_end_pack_zip_from_output(const gchar *output, GError **error)
{
	g_autoptr(JsonNode) node = NULL;
	JsonArray *results;
	guint i;
	if (venture_string_is_empty(output))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "This pack has not run yet");
		return NULL;
	}
	node = venture_json_parse(output, error);
	if (node == NULL)
		return NULL;
	if (!JSON_NODE_HOLDS_ARRAY(node))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "The retained output is not a list of results");
		return NULL;
	}
	results = json_node_get_array(node);
	for (i = 0; i < json_array_get_length(results); i++)
	{
		JsonNode *element = json_array_get_element(results, i);
		if (JSON_NODE_HOLDS_OBJECT(element) && is_pack_result(json_node_get_object(element)))
			return zip_rows(json_object_get_array_member(json_node_get_object(element), "rows"), error);
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		"This pack's retained output holds no year-end pack");
	return NULL;
}
