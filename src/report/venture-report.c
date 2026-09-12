/*
 * venture-report.c - The reporting engine
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <yaml-glib.h>

#include <string.h>

/* ==========================================================================
 * Results
 * ========================================================================== */

typedef struct
{
	gchar			*key;
	gchar			*label;
	VentureReportColumnKind	 kind;
} VentureReportColumn;

struct _VentureReportResult
{
	GObject parent_instance;

	gchar			*title;
	gchar			*note;
	VentureDateRange	*period;

	GPtrArray		*columns;	/* VentureReportColumn */
	GPtrArray		*metrics;	/* VentureMetric */
	GPtrArray		*rows;		/* GHashTable, key -> GValue */
	GHashTable		*current_row;
};

static const GEnumValue venture_report_column_kind_values[] = {
	{ VENTURE_REPORT_COLUMN_TEXT,    "VENTURE_REPORT_COLUMN_TEXT",    "text" },
	{ VENTURE_REPORT_COLUMN_NUMBER,  "VENTURE_REPORT_COLUMN_NUMBER",  "number" },
	{ VENTURE_REPORT_COLUMN_MONEY,   "VENTURE_REPORT_COLUMN_MONEY",   "money" },
	{ VENTURE_REPORT_COLUMN_PERCENT, "VENTURE_REPORT_COLUMN_PERCENT", "percent" },
	{ VENTURE_REPORT_COLUMN_DATE,    "VENTURE_REPORT_COLUMN_DATE",    "date" },
	{ 0, NULL, NULL }
};

GType
venture_report_column_kind_get_type(void)
{
	static gsize type_id = 0;

	if (g_once_init_enter(&type_id))
	{
		GType registered;

		registered = g_enum_register_static("VentureReportColumnKind",
		                                    venture_report_column_kind_values);
		g_once_init_leave(&type_id, registered);
	}

	return (GType)type_id;
}

G_DEFINE_FINAL_TYPE(VentureReportResult, venture_report_result, G_TYPE_OBJECT)

static void
venture_report_column_free(gpointer data)
{
	VentureReportColumn *column;

	column = data;

	g_free(column->key);
	g_free(column->label);
	g_free(column);
}

static void
venture_report_value_free(gpointer data)
{
	GValue *value;

	value = data;

	g_value_unset(value);
	g_free(value);
}

static void
venture_report_result_finalize(GObject *object)
{
	VentureReportResult *self;

	self = VENTURE_REPORT_RESULT(object);

	g_clear_pointer(&self->title, g_free);
	g_clear_pointer(&self->note, g_free);
	g_clear_pointer(&self->period, venture_date_range_free);
	g_clear_pointer(&self->columns, g_ptr_array_unref);
	g_clear_pointer(&self->metrics, g_ptr_array_unref);
	g_clear_pointer(&self->rows, g_ptr_array_unref);

	G_OBJECT_CLASS(venture_report_result_parent_class)->finalize(object);
}

static void
venture_report_result_class_init(VentureReportResultClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_report_result_finalize;
}

static void
venture_report_result_init(VentureReportResult *self)
{
	self->columns = g_ptr_array_new_with_free_func(venture_report_column_free);
	self->metrics = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_metric_free);
	self->rows = g_ptr_array_new_with_free_func(
		(GDestroyNotify)g_hash_table_unref);
}

VentureReportResult *
venture_report_result_new(
	const gchar		*title,
	const VentureDateRange	*period
){
	VentureReportResult *self;

	self = g_object_new(VENTURE_TYPE_REPORT_RESULT, NULL);
	self->title = g_strdup(title);
	self->period = venture_date_range_copy(period);

	return self;
}

void
venture_report_result_add_column(
	VentureReportResult	*self,
	const gchar		*key,
	const gchar		*label,
	VentureReportColumnKind	 kind
){
	VentureReportColumn *column;

	g_return_if_fail(VENTURE_IS_REPORT_RESULT(self));
	g_return_if_fail(NULL != key);

	column = g_new0(VentureReportColumn, 1);
	column->key = g_strdup(key);
	column->label = g_strdup((NULL != label) ? label : key);
	column->kind = kind;

	g_ptr_array_add(self->columns, column);
}

void
venture_report_result_add_metric(
	VentureReportResult	*self,
	VentureMetric		*metric
){
	g_return_if_fail(VENTURE_IS_REPORT_RESULT(self));
	g_return_if_fail(NULL != metric);

	g_ptr_array_add(self->metrics, metric);
}

void
venture_report_result_begin_row(VentureReportResult *self)
{
	g_return_if_fail(VENTURE_IS_REPORT_RESULT(self));

	self->current_row = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                          g_free,
	                                          venture_report_value_free);
	g_ptr_array_add(self->rows, self->current_row);
}

/*
 * Stores a value in the row being built. Values are keyed rather than
 * positional so a report that gains a column does not have to keep every
 * row's list in step.
 */
static void
venture_report_result_set_value(
	VentureReportResult	*self,
	const gchar		*key,
	GValue			*value
){
	if (NULL == self->current_row)
	{
		g_warning("venture_report_result: a value was set before any row "
		          "was begun; discarding it");
		venture_report_value_free(value);
		return;
	}

	g_hash_table_insert(self->current_row, g_strdup(key), value);
}

void
venture_report_result_set_text(
	VentureReportResult	*self,
	const gchar		*key,
	const gchar		*value
){
	GValue *stored;

	g_return_if_fail(VENTURE_IS_REPORT_RESULT(self));
	g_return_if_fail(NULL != key);

	stored = g_new0(GValue, 1);
	g_value_init(stored, G_TYPE_STRING);
	g_value_set_string(stored, value);

	venture_report_result_set_value(self, key, stored);
}

void
venture_report_result_set_money(
	VentureReportResult	*self,
	const gchar		*key,
	const VentureMoney	*value
){
	GValue *stored;

	g_return_if_fail(VENTURE_IS_REPORT_RESULT(self));
	g_return_if_fail(NULL != key);

	stored = g_new0(GValue, 1);
	g_value_init(stored, VENTURE_TYPE_MONEY);
	g_value_set_boxed(stored, value);

	venture_report_result_set_value(self, key, stored);
}

void
venture_report_result_set_number(
	VentureReportResult	*self,
	const gchar		*key,
	gdouble			 value
){
	GValue *stored;

	g_return_if_fail(VENTURE_IS_REPORT_RESULT(self));
	g_return_if_fail(NULL != key);

	stored = g_new0(GValue, 1);
	g_value_init(stored, G_TYPE_DOUBLE);
	g_value_set_double(stored, value);

	venture_report_result_set_value(self, key, stored);
}

GPtrArray *
venture_report_result_get_metrics(VentureReportResult *self)
{
	g_return_val_if_fail(VENTURE_IS_REPORT_RESULT(self), NULL);

	return self->metrics;
}

guint
venture_report_result_get_row_count(VentureReportResult *self)
{
	g_return_val_if_fail(VENTURE_IS_REPORT_RESULT(self), 0);

	return self->rows->len;
}

const gchar *
venture_report_result_get_title(VentureReportResult *self)
{
	g_return_val_if_fail(VENTURE_IS_REPORT_RESULT(self), NULL);

	return self->title;
}

void
venture_report_result_set_note(
	VentureReportResult	*self,
	const gchar		*note
){
	g_return_if_fail(VENTURE_IS_REPORT_RESULT(self));

	g_free(self->note);
	self->note = g_strdup(note);
}

void
venture_report_result_append_note(
	VentureReportResult	*self,
	const gchar		*note
){
	gchar *combined;

	g_return_if_fail(VENTURE_IS_REPORT_RESULT(self));
	g_return_if_fail(NULL != note);

	if (NULL == self->note)
	{
		self->note = g_strdup(note);
		return;
	}

	combined = g_strdup_printf("%s\n%s", self->note, note);
	g_free(self->note);
	self->note = combined;
}

/* --- Rendering ----------------------------------------------------------- */

/*
 * Formats one cell as text, honouring the column's kind. Every renderer goes
 * through this, so a money column looks the same in the terminal, the CSV
 * and the org table.
 */
static gchar *
venture_report_format_cell(
	const VentureReportColumn	*column,
	GHashTable			*row
){
	const GValue *value;

	value = g_hash_table_lookup(row, column->key);

	if (NULL == value)
		return g_strdup("");

	if (G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY))
	{
		const VentureMoney *money;

		money = g_value_get_boxed(value);

		if (NULL == money)
			return g_strdup("");

		return venture_money_to_display_string(money, TRUE);
	}

	if (G_VALUE_HOLDS_DOUBLE(value))
	{
		gdouble number;

		number = g_value_get_double(value);

		if (VENTURE_REPORT_COLUMN_PERCENT == column->kind)
			return g_strdup_printf("%.1f%%", number * 100.0);

		/* A whole number prints without a pointless ".00". */
		if (number == (gdouble)(gint64)number)
			return g_strdup_printf("%" G_GINT64_FORMAT, (gint64)number);

		return g_strdup_printf("%.2f", number);
	}

	return g_strdup((NULL != g_value_get_string(value))
		? g_value_get_string(value) : "");
}

static gchar *
venture_report_render_table(VentureReportResult *self)
{
	g_autoptr(GString) text = NULL;
	g_autofree gsize *widths = NULL;
	guint i;
	guint j;

	text = g_string_new(NULL);
	g_string_append_printf(text, "%s\n", self->title);

	if (NULL != self->period)
	{
		g_string_append_printf(text, "%s\n",
			venture_date_range_get_label(self->period));
	}

	for (i = 0; i < self->metrics->len; i++)
	{
		VentureMetric *metric;
		g_autofree gchar *formatted = NULL;
		g_autofree gchar *change = NULL;

		metric = g_ptr_array_index(self->metrics, i);
		formatted = venture_metric_format_value(metric);
		change = venture_metric_format_change(metric);

		g_string_append_printf(text, "  %-28s %s%s%s\n",
		                       venture_metric_get_label(metric), formatted,
		                       (NULL != change) ? "  " : "",
		                       (NULL != change) ? change : "");
	}

	/* A result with no table can still carry a caveat, and dropping it
	 * here would silence exactly the reports that most need one. */
	if (0 == self->columns->len)
	{
		if (NULL != self->note)
			g_string_append_printf(text, "\n%s\n", self->note);

		return g_string_free(g_steal_pointer(&text), FALSE);
	}

	/* Two passes: measure every cell, then emit. Aligned columns are the
	 * entire point of the table format. */
	widths = g_new0(gsize, self->columns->len);

	for (i = 0; i < self->columns->len; i++)
	{
		const VentureReportColumn *column;

		column = g_ptr_array_index(self->columns, i);
		widths[i] = g_utf8_strlen(column->label, -1);
	}

	for (j = 0; j < self->rows->len; j++)
	{
		for (i = 0; i < self->columns->len; i++)
		{
			g_autofree gchar *cell = NULL;
			gsize length;

			cell = venture_report_format_cell(
				g_ptr_array_index(self->columns, i),
				g_ptr_array_index(self->rows, j));
			length = (gsize)g_utf8_strlen(cell, -1);

			if (length > widths[i])
				widths[i] = length;
		}
	}

	g_string_append_c(text, '\n');

	for (i = 0; i < self->columns->len; i++)
	{
		const VentureReportColumn *column;

		column = g_ptr_array_index(self->columns, i);
		g_string_append_printf(text, "%-*s  ", (int)widths[i], column->label);
	}

	g_string_append_c(text, '\n');

	for (i = 0; i < self->columns->len; i++)
	{
		gsize k;

		for (k = 0; k < widths[i]; k++)
			g_string_append_c(text, '-');

		g_string_append(text, "  ");
	}

	g_string_append_c(text, '\n');

	for (j = 0; j < self->rows->len; j++)
	{
		for (i = 0; i < self->columns->len; i++)
		{
			const VentureReportColumn *column;
			g_autofree gchar *cell = NULL;

			column = g_ptr_array_index(self->columns, i);
			cell = venture_report_format_cell(column,
				g_ptr_array_index(self->rows, j));

			/* Numbers right-align so the digits line up. */
			if (VENTURE_REPORT_COLUMN_TEXT == column->kind)
			{
				g_string_append_printf(text, "%-*s  ",
				                       (int)widths[i], cell);
			}
			else
			{
				g_string_append_printf(text, "%*s  ",
				                       (int)widths[i], cell);
			}
		}

		g_string_append_c(text, '\n');
	}

	if (NULL != self->note)
		g_string_append_printf(text, "\n%s\n", self->note);

	return g_string_free(g_steal_pointer(&text), FALSE);
}

static gchar *
venture_report_render_csv(VentureReportResult *self)
{
	g_autoptr(GString) text = NULL;
	guint i;
	guint j;

	text = g_string_new(NULL);

	for (i = 0; i < self->columns->len; i++)
	{
		const VentureReportColumn *column;
		g_autofree gchar *escaped = NULL;

		column = g_ptr_array_index(self->columns, i);
		escaped = venture_csv_escape(column->label);

		if (i > 0)
			g_string_append_c(text, ',');

		g_string_append(text, escaped);
	}

	g_string_append_c(text, '\n');

	for (j = 0; j < self->rows->len; j++)
	{
		for (i = 0; i < self->columns->len; i++)
		{
			g_autofree gchar *cell = NULL;
			g_autofree gchar *escaped = NULL;

			cell = venture_report_format_cell(
				g_ptr_array_index(self->columns, i),
				g_ptr_array_index(self->rows, j));
			/* Escaping here also defuses a leading '=' that a
			 * spreadsheet would otherwise execute as a formula. */
			escaped = venture_csv_escape(cell);

			if (i > 0)
				g_string_append_c(text, ',');

			g_string_append(text, escaped);
		}

		g_string_append_c(text, '\n');
	}

	return g_string_free(g_steal_pointer(&text), FALSE);
}

static gchar *
venture_report_render_org(VentureReportResult *self)
{
	g_autoptr(GString) text = NULL;
	guint i;
	guint j;

	text = g_string_new(NULL);
	g_string_append_printf(text, "* %s\n", self->title);

	if (NULL != self->period)
	{
		g_string_append_printf(text, "  :PROPERTIES:\n  :PERIOD: %s\n  :END:\n",
			venture_date_range_get_label(self->period));
	}

	for (i = 0; i < self->metrics->len; i++)
	{
		VentureMetric *metric;
		g_autofree gchar *formatted = NULL;

		metric = g_ptr_array_index(self->metrics, i);
		formatted = venture_metric_format_value(metric);

		g_string_append_printf(text, "  - %s :: %s\n",
		                       venture_metric_get_label(metric), formatted);
	}

	if (0 == self->columns->len)
		return g_string_free(g_steal_pointer(&text), FALSE);

	g_string_append(text, "\n  |");

	for (i = 0; i < self->columns->len; i++)
	{
		const VentureReportColumn *column;

		column = g_ptr_array_index(self->columns, i);
		g_string_append_printf(text, " %s |", column->label);
	}

	g_string_append(text, "\n  |");

	for (i = 0; i < self->columns->len; i++)
		g_string_append(text, "---+");

	g_string_append_c(text, '\n');

	for (j = 0; j < self->rows->len; j++)
	{
		g_string_append(text, "  |");

		for (i = 0; i < self->columns->len; i++)
		{
			g_autofree gchar *cell = NULL;

			cell = venture_report_format_cell(
				g_ptr_array_index(self->columns, i),
				g_ptr_array_index(self->rows, j));
			g_string_append_printf(text, " %s |", cell);
		}

		g_string_append_c(text, '\n');
	}

	if (NULL != self->note)
		g_string_append_printf(text, "\n  %s\n", self->note);

	return g_string_free(g_steal_pointer(&text), FALSE);
}

static gchar *
venture_report_render_html(VentureReportResult *self)
{
	g_autoptr(GString) html = NULL;
	guint i;
	guint j;

	html = g_string_new(NULL);

	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>");
	venture_html_escape_append(html, self->title);
	g_string_append(html, "</h2>");

	if (NULL != self->period)
	{
		g_string_append(html, "<span class=\"badge\">");
		venture_html_escape_append(html,
			venture_date_range_get_label(self->period));
		g_string_append(html, "</span>");
	}

	g_string_append(html, "</div>");

	if (self->metrics->len > 0)
	{
		g_string_append(html, "<div class=\"card-body\">"
		                      "<div class=\"grid cols-4\">");

		for (i = 0; i < self->metrics->len; i++)
		{
			VentureMetric *metric;
			g_autofree gchar *formatted = NULL;
			g_autofree gchar *change = NULL;
			gint direction;

			metric = g_ptr_array_index(self->metrics, i);
			formatted = venture_metric_format_value(metric);
			change = venture_metric_format_change(metric);
			direction = venture_metric_get_direction(metric);

			g_string_append(html, "<div class=\"stat\">"
			                      "<span class=\"stat-label\">");
			venture_html_escape_append(html,
				venture_metric_get_label(metric));
			g_string_append(html, "</span><span class=\"stat-value\">");
			venture_html_escape_append(html, formatted);
			g_string_append(html, "</span>");

			if (NULL != change)
			{
				g_string_append_printf(html,
					"<span class=\"stat-delta %s\">",
					(direction > 0) ? "up"
					                : ((direction < 0) ? "down" : "flat"));
				venture_html_escape_append(html, change);
				g_string_append(html, "</span>");
			}

			g_string_append(html, "</div>");
		}

		g_string_append(html, "</div></div>");
	}

	if (self->columns->len > 0)
	{
		g_string_append(html, "<div class=\"table-wrap\">"
		                      "<table class=\"data\"><thead><tr>");

		for (i = 0; i < self->columns->len; i++)
		{
			const VentureReportColumn *column;

			column = g_ptr_array_index(self->columns, i);
			g_string_append_printf(html, "<th class=\"%s\">",
				(VENTURE_REPORT_COLUMN_TEXT == column->kind) ? "" : "num");
			venture_html_escape_append(html, column->label);
			g_string_append(html, "</th>");
		}

		g_string_append(html, "</tr></thead><tbody>");

		for (j = 0; j < self->rows->len; j++)
		{
			g_string_append(html, "<tr>");

			for (i = 0; i < self->columns->len; i++)
			{
				const VentureReportColumn *column;
				g_autofree gchar *cell = NULL;

				column = g_ptr_array_index(self->columns, i);
				cell = venture_report_format_cell(column,
					g_ptr_array_index(self->rows, j));

				g_string_append_printf(html, "<td class=\"%s\">",
					(VENTURE_REPORT_COLUMN_TEXT == column->kind)
						? "" : "num");
				venture_html_escape_append(html, cell);
				g_string_append(html, "</td>");
			}

			g_string_append(html, "</tr>");
		}

		g_string_append(html, "</tbody></table></div>");
	}

	if (self->rows->len == 0)
	{
		g_string_append(html, "<div class=\"empty\">"
		                      "<h3>Nothing in this period</h3>"
		                      "<p class=\"muted\">No records matched.</p></div>");
	}

	if (NULL != self->note)
	{
		g_string_append(html, "<div class=\"card-body\">"
		                      "<div class=\"notice warning\">");
		venture_html_escape_append(html, self->note);
		g_string_append(html, "</div></div>");
	}

	g_string_append(html, "</div>");

	return g_string_free(g_steal_pointer(&html), FALSE);
}

JsonNode *
venture_report_result_to_json(VentureReportResult *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	guint i;
	guint j;

	g_return_val_if_fail(VENTURE_IS_REPORT_RESULT(self), NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, self->title);

	if (NULL != self->period)
	{
		json_builder_set_member_name(builder, "period");
		json_builder_add_value(builder,
			venture_date_range_to_json(self->period));
	}

	json_builder_set_member_name(builder, "metrics");
	json_builder_begin_array(builder);

	for (i = 0; i < self->metrics->len; i++)
	{
		json_builder_add_value(builder,
			venture_metric_to_json(g_ptr_array_index(self->metrics, i)));
	}

	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "columns");
	json_builder_begin_array(builder);

	for (i = 0; i < self->columns->len; i++)
	{
		const VentureReportColumn *column;

		column = g_ptr_array_index(self->columns, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "key");
		json_builder_add_string_value(builder, column->key);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, column->label);
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder,
			venture_enum_to_nick(VENTURE_TYPE_REPORT_COLUMN_KIND,
			                     (gint)column->kind));
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "rows");
	json_builder_begin_array(builder);

	for (j = 0; j < self->rows->len; j++)
	{
		json_builder_begin_object(builder);

		for (i = 0; i < self->columns->len; i++)
		{
			const VentureReportColumn *column;
			const GValue *value;
			g_autofree gchar *formatted = NULL;

			column = g_ptr_array_index(self->columns, i);
			value = g_hash_table_lookup(g_ptr_array_index(self->rows, j),
			                            column->key);

			json_builder_set_member_name(builder, column->key);

			if (NULL == value)
			{
				json_builder_add_null_value(builder);
				continue;
			}

			json_builder_add_value(builder,
			                       venture_json_node_from_value(value));

			/* A formatted twin accompanies each raw value, so a
			 * consumer that does not understand minor units -- an
			 * AI reading a tool result -- still reports the right
			 * figure. */
			formatted = venture_report_format_cell(column,
				g_ptr_array_index(self->rows, j));

			{
				g_autofree gchar *display_key = NULL;

				display_key = g_strconcat(column->key, "_formatted",
				                          NULL);
				json_builder_set_member_name(builder, display_key);
				json_builder_add_string_value(builder, formatted);
			}
		}

		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	if (NULL != self->note)
	{
		json_builder_set_member_name(builder, "note");
		json_builder_add_string_value(builder, self->note);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

gchar *
venture_report_result_render(
	VentureReportResult	*self,
	VentureOutputFormat	 format
){
	g_return_val_if_fail(VENTURE_IS_REPORT_RESULT(self), NULL);

	switch (format)
	{
	case VENTURE_OUTPUT_FORMAT_JSON:
	{
		g_autoptr(JsonNode) node = NULL;

		node = venture_report_result_to_json(self);

		return venture_json_to_string(node, TRUE);
	}

	case VENTURE_OUTPUT_FORMAT_CSV:
		return venture_report_render_csv(self);

	case VENTURE_OUTPUT_FORMAT_ORG:
		return venture_report_render_org(self);

	case VENTURE_OUTPUT_FORMAT_HTML:
		return venture_report_render_html(self);

	case VENTURE_OUTPUT_FORMAT_YAML:
	{
		g_autoptr(JsonNode) node = NULL;
		g_autoptr(YamlDocument) document = NULL;
		g_autoptr(YamlGenerator) generator = NULL;

		node = venture_report_result_to_json(self);
		document = yaml_document_from_json_node(node);

		if (NULL == document)
			return venture_report_render_table(self);

		generator = yaml_generator_new();
		yaml_generator_set_document(generator, document);
		yaml_generator_set_indent(generator, 2);

		return yaml_generator_to_data(generator, NULL, NULL);
	}

	case VENTURE_OUTPUT_FORMAT_TABLE:
	case VENTURE_OUTPUT_FORMAT_TEXT:
	default:
		return venture_report_render_table(self);
	}
}

/* ==========================================================================
 * Reports
 * ========================================================================== */

typedef struct
{
	gchar	*name;
	gchar	*title;
	gchar	*description;
} VentureReportPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(VentureReport, venture_report, G_TYPE_OBJECT)

enum
{
	REPORT_PROP_0,
	REPORT_PROP_NAME,
	REPORT_PROP_TITLE,
	REPORT_PROP_DESCRIPTION,
	REPORT_N_PROPERTIES
};

static GParamSpec *venture_report_properties[REPORT_N_PROPERTIES] = { NULL };

static void
venture_report_get_property(
	GObject		*object,
	guint		 prop_id,
	GValue		*value,
	GParamSpec	*pspec
){
	VentureReportPrivate *priv;

	priv = venture_report_get_instance_private(VENTURE_REPORT(object));

	switch (prop_id)
	{
	case REPORT_PROP_NAME:
		g_value_set_string(value, priv->name);
		break;

	case REPORT_PROP_TITLE:
		g_value_set_string(value, priv->title);
		break;

	case REPORT_PROP_DESCRIPTION:
		g_value_set_string(value, priv->description);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
venture_report_set_property(
	GObject		*object,
	guint		 prop_id,
	const GValue	*value,
	GParamSpec	*pspec
){
	VentureReportPrivate *priv;

	priv = venture_report_get_instance_private(VENTURE_REPORT(object));

	switch (prop_id)
	{
	case REPORT_PROP_NAME:
		g_free(priv->name);
		priv->name = g_value_dup_string(value);
		break;

	case REPORT_PROP_TITLE:
		g_free(priv->title);
		priv->title = g_value_dup_string(value);
		break;

	case REPORT_PROP_DESCRIPTION:
		g_free(priv->description);
		priv->description = g_value_dup_string(value);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
venture_report_finalize(GObject *object)
{
	VentureReportPrivate *priv;

	priv = venture_report_get_instance_private(VENTURE_REPORT(object));

	g_clear_pointer(&priv->name, g_free);
	g_clear_pointer(&priv->title, g_free);
	g_clear_pointer(&priv->description, g_free);

	G_OBJECT_CLASS(venture_report_parent_class)->finalize(object);
}

static void
venture_report_class_init(VentureReportClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = venture_report_get_property;
	object_class->set_property = venture_report_set_property;
	object_class->finalize = venture_report_finalize;

	venture_report_properties[REPORT_PROP_NAME] =
		g_param_spec_string("name", "Name", "Machine name", NULL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	venture_report_properties[REPORT_PROP_TITLE] =
		g_param_spec_string("title", "Title", "Human title", NULL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	venture_report_properties[REPORT_PROP_DESCRIPTION] =
		g_param_spec_string("description", "Description",
		                    "What the report answers", NULL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, REPORT_N_PROPERTIES,
	                                  venture_report_properties);
}

static void
venture_report_init(VentureReport *self)
{
}

const gchar *
venture_report_get_name(VentureReport *self)
{
	VentureReportPrivate *priv;

	g_return_val_if_fail(VENTURE_IS_REPORT(self), NULL);

	priv = venture_report_get_instance_private(self);

	return priv->name;
}

const gchar *
venture_report_get_title(VentureReport *self)
{
	VentureReportPrivate *priv;

	g_return_val_if_fail(VENTURE_IS_REPORT(self), NULL);

	priv = venture_report_get_instance_private(self);

	return (NULL != priv->title) ? priv->title : priv->name;
}

const gchar *
venture_report_get_description(VentureReport *self)
{
	VentureReportPrivate *priv;

	g_return_val_if_fail(VENTURE_IS_REPORT(self), NULL);

	priv = venture_report_get_instance_private(self);

	return priv->description;
}

VentureReportResult *
venture_report_generate(
	VentureReport		 *self,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	VentureReportClass *klass;

	g_return_val_if_fail(VENTURE_IS_REPORT(self), NULL);
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	klass = VENTURE_REPORT_GET_CLASS(self);

	if (NULL == klass->generate)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
		            "The %s report has no implementation",
		            venture_report_get_name(self));
		return NULL;
	}

	return klass->generate(self, context, period, options, error);
}

JsonNode *
venture_report_describe_parameters(VentureReport *self)
{
	VentureReportClass *klass;

	g_return_val_if_fail(VENTURE_IS_REPORT(self), NULL);

	klass = VENTURE_REPORT_GET_CLASS(self);

	if (NULL == klass->describe_parameters)
		return NULL;

	return klass->describe_parameters(self);
}

/* --- Function-backed reports --------------------------------------------- */

struct _VentureFuncReport
{
	VentureReport parent_instance;

	VentureReportFunc func;
};

G_DEFINE_FINAL_TYPE(VentureFuncReport, venture_func_report, VENTURE_TYPE_REPORT)

static VentureReportResult *
venture_func_report_generate(
	VentureReport		 *report,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	VentureFuncReport *self;

	self = VENTURE_FUNC_REPORT(report);

	return self->func(context, period, options, error);
}

static void
venture_func_report_class_init(VentureFuncReportClass *klass)
{
	VENTURE_REPORT_CLASS(klass)->generate = venture_func_report_generate;
}

static void
venture_func_report_init(VentureFuncReport *self)
{
}

VentureFuncReport *
venture_func_report_new(
	const gchar		*name,
	const gchar		*title,
	const gchar		*description,
	VentureReportFunc	 func
){
	VentureFuncReport *self;

	g_return_val_if_fail(NULL != name, NULL);
	g_return_val_if_fail(NULL != func, NULL);

	self = g_object_new(VENTURE_TYPE_FUNC_REPORT,
	                    "name", name,
	                    "title", title,
	                    "description", description,
	                    NULL);
	self->func = func;

	return self;
}

/* --- Registry ------------------------------------------------------------ */

struct _VentureReportRegistry
{
	GObject parent_instance;

	GHashTable *reports;

	/* name -> present when a module has switched the report off. The
	 * report stays registered so turning the module back on restores
	 * it, but lookups and listings skip it. */
	GHashTable *hidden;
};

G_DEFINE_FINAL_TYPE(VentureReportRegistry, venture_report_registry, G_TYPE_OBJECT)

static void
venture_report_registry_finalize(GObject *object)
{
	VentureReportRegistry *self;

	self = VENTURE_REPORT_REGISTRY(object);

	g_clear_pointer(&self->reports, g_hash_table_unref);
	g_clear_pointer(&self->hidden, g_hash_table_unref);

	G_OBJECT_CLASS(venture_report_registry_parent_class)->finalize(object);
}

static void
venture_report_registry_class_init(VentureReportRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_report_registry_finalize;
}

static void
venture_report_registry_init(VentureReportRegistry *self)
{
	self->hidden = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                     NULL);
	self->reports = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                      g_free, g_object_unref);
}

VentureReportRegistry *
venture_report_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_REPORT_REGISTRY, NULL);
}

void
venture_report_registry_add(
	VentureReportRegistry	*self,
	VentureReport		*report
){
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(self));
	g_return_if_fail(VENTURE_IS_REPORT(report));

	/* Replacing rather than refusing, so a plugin can deliberately
	 * override a built-in report with a better version for its data. */
	g_hash_table_insert(self->reports,
	                    g_strdup(venture_report_get_name(report)), report);
}

gboolean
venture_report_registry_remove(
	VentureReportRegistry	*self,
	const gchar		*name
){
	g_return_val_if_fail(VENTURE_IS_REPORT_REGISTRY(self), FALSE);
	g_return_val_if_fail(NULL != name, FALSE);

	return g_hash_table_remove(self->reports, name);
}

VentureReport *
venture_report_registry_lookup(
	VentureReportRegistry	*self,
	const gchar		*name
){
	g_return_val_if_fail(VENTURE_IS_REPORT_REGISTRY(self), NULL);
	g_return_val_if_fail(NULL != name, NULL);

	if (g_hash_table_contains(self->hidden, name))
		return NULL;

	return g_hash_table_lookup(self->reports, name);
}

void
venture_report_registry_set_enabled(
	VentureReportRegistry	*self,
	const gchar		*name,
	gboolean		 enabled
){
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(self));
	g_return_if_fail(NULL != name);

	if (enabled)
		g_hash_table_remove(self->hidden, name);
	else
		g_hash_table_add(self->hidden, g_strdup(name));
}

static gint
venture_report_compare_by_name(
	gconstpointer	a,
	gconstpointer	b
){
	VentureReport * const *report_a = a;
	VentureReport * const *report_b = b;

	return g_strcmp0(venture_report_get_name(*report_a),
	                 venture_report_get_name(*report_b));
}

GPtrArray *
venture_report_registry_list(VentureReportRegistry *self)
{
	GPtrArray *reports;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_REPORT_REGISTRY(self), NULL);

	reports = g_ptr_array_new();
	g_hash_table_iter_init(&iter, self->reports);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		if (g_hash_table_contains(self->hidden, key))
			continue;

		g_ptr_array_add(reports, value);
	}

	/* Sorted so generated help, tool descriptions and the UI's report
	 * menu are deterministic. */
	g_ptr_array_sort(reports, venture_report_compare_by_name);

	return reports;
}

JsonNode *
venture_report_registry_describe(VentureReportRegistry *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GPtrArray) reports = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_REPORT_REGISTRY(self), NULL);

	reports = venture_report_registry_list(self);
	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < reports->len; i++)
	{
		VentureReport *report;
		g_autoptr(JsonNode) parameters = NULL;

		report = g_ptr_array_index(reports, i);

		json_builder_begin_object(builder);

		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder,
		                              venture_report_get_name(report));

		json_builder_set_member_name(builder, "title");
		json_builder_add_string_value(builder,
		                              venture_report_get_title(report));

		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder,
		                              venture_report_get_description(report));

		parameters = venture_report_describe_parameters(report);

		if (NULL != parameters)
		{
			json_builder_set_member_name(builder, "parameters");
			json_builder_add_value(builder, g_steal_pointer(&parameters));
		}

		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}
