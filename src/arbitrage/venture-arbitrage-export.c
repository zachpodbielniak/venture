/*
 * venture-arbitrage-export.c - Writing a set of opportunities out
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The export format registry and its two built-ins:
 *
 *  - csv: one line per opportunity, the figures a spreadsheet wants;
 *  - shopping_list: what to buy where -- every buy and stake leg of the
 *    rows, grouped by venue, the same instrument at the same venue added
 *    up, a total per venue per currency (never across two).
 *
 * A plugin adds its own (a game addon's import string, say) with
 * venture_export_format_registry_add(); /arbitrage/export?format=NAME
 * serves whatever is registered.
 */

#include "venture.h"
#include "arbitrage/venture-arbitrage-engine-private.h"

#include <math.h>
#include <string.h>

typedef struct
{
	gchar			*name;
	gchar			*label;
	gchar			*content_type;
	gchar			*extension;
	VentureExportFunc	 func;
	gpointer		 user_data;
	GDestroyNotify		 destroy;
} ArbExport;

struct _VentureExportFormatRegistry
{
	GObject		 parent_instance;

	GPtrArray	*entries;
};

G_DEFINE_FINAL_TYPE(VentureExportFormatRegistry, venture_export_format_registry, G_TYPE_OBJECT)

static void
arb_export_free(gpointer data)
{
	ArbExport *entry;

	entry = data;

	if (NULL != entry->destroy)
		entry->destroy(entry->user_data);

	g_free(entry->name);
	g_free(entry->label);
	g_free(entry->content_type);
	g_free(entry->extension);
	g_free(entry);
}

static void
venture_export_format_registry_finalize(GObject *object)
{
	g_ptr_array_unref(VENTURE_EXPORT_FORMAT_REGISTRY(object)->entries);

	G_OBJECT_CLASS(venture_export_format_registry_parent_class)->finalize(object);
}

static void
venture_export_format_registry_class_init(VentureExportFormatRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_export_format_registry_finalize;
}

static void
venture_export_format_registry_init(VentureExportFormatRegistry *self)
{
	self->entries = g_ptr_array_new_with_free_func(arb_export_free);
}

static ArbExport *
arb_export_lookup(
	VentureExportFormatRegistry	*self,
	const gchar			*name
){
	guint i;

	for (i = 0; (NULL != name) && (i < self->entries->len); i++)
	{
		ArbExport *entry = g_ptr_array_index(self->entries, i);

		if (0 == g_strcmp0(entry->name, name))
			return entry;
	}

	return NULL;
}

/* --- Writing values ---------------------------------------------------------- */

/*
 * Whether @text is a whole figure: an optional minus, digits with at most
 * one point, and optionally one space and a currency code ("-1.50 USD").
 * Only such a field may start with a minus unguarded; "-2+3+cmd|..." also
 * starts with a minus and a digit, and a spreadsheet runs it.
 */
static gboolean
arb_csv_is_figure(const gchar *text)
{
	const gchar *cursor;
	gboolean point;
	gboolean digits;

	cursor = text;
	point = FALSE;
	digits = FALSE;

	if ('-' == *cursor)
		cursor++;

	for (; ('\0' != *cursor) && (' ' != *cursor); cursor++)
	{
		if (g_ascii_isdigit(*cursor))
			digits = TRUE;
		else if (('.' == *cursor) && !point)
			point = TRUE;
		else
			return FALSE;
	}

	if (!digits)
		return FALSE;

	if ('\0' == *cursor)
		return TRUE;

	/* One space, then a code: [A-Z][A-Z0-9_]*. */
	cursor++;

	if (!g_ascii_isupper(*cursor))
		return FALSE;

	for (cursor++; '\0' != *cursor; cursor++)
		if (!g_ascii_isupper(*cursor) && !g_ascii_isdigit(*cursor) && ('_' != *cursor))
			return FALSE;

	return TRUE;
}

/*
 * One CSV field. Quoted when it holds a separator, a quote or a line
 * break; and a field a spreadsheet would read as a formula (=, +, -, @,
 * a tab or a carriage return first) gets a leading apostrophe, because an
 * instrument's name is whatever a data source said it was. A negative
 * figure is the one exception, and only when the whole field is one.
 */
static void
arb_csv_field(
	GString		*out,
	const gchar	*text
){
	const gchar *cursor;
	gboolean quote;

	if (NULL == text)
		text = "";

	quote = (NULL != strpbrk(text, ",\"\r\n"));

	if (quote)
		g_string_append_c(out, '"');

	if (('\0' != text[0]) && (NULL != strchr("=+-@\t\r", text[0])) &&
	    !(('-' == text[0]) && arb_csv_is_figure(text)))
		g_string_append_c(out, '\'');

	for (cursor = text; '\0' != *cursor; cursor++)
	{
		if ('"' == *cursor)
			g_string_append_c(out, '"');

		g_string_append_c(out, *cursor);
	}

	if (quote)
		g_string_append_c(out, '"');
}

void
venture_arbitrage_csv_field(
	GString		*out,
	const gchar	*text
){
	arb_csv_field(out, text);
}

/* A money member as "12.5000 GOLD", or empty. */
static gchar *
arb_money_text(
	JsonObject	*object,
	const gchar	*member
){
	g_autoptr(VentureMoney) money = NULL;

	money = venture_arbitrage_get_money(object, member);

	return (NULL != money) ? venture_money_to_string(money) : g_strdup("");
}

static gchar *
arb_ratio_text(
	JsonObject	*object,
	const gchar	*member
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	gdouble value;

	value = venture_arbitrage_get_ratio(object, member);

	if (!isfinite(value))
		return g_strdup("");

	return g_strdup(g_ascii_formatd(buffer, sizeof(buffer), "%.6f", value));
}

static const gchar *
arb_side_text(
	JsonObject	*row,
	const gchar	*side,
	const gchar	*member
){
	JsonObject *object;

	object = json_object_has_member(row, side) ? json_object_get_object_member(row, side) : NULL;

	return venture_json_object_get_string(object, member, "");
}

/* --- csv -------------------------------------------------------------------- */

static GBytes *
arb_export_csv(
	JsonArray	 *opportunities,
	JsonObject	 *options,
	gpointer	  user_data,
	GError		**error
){
	static const gchar *const header[] = {
		"strategy", "key", "title", "instrument", "buy_venue", "sell_venue", "units", "currency",
		"capital", "net", "roi", "roi_per_day", "annualized", "confidence", "sale_rate",
		"age_seconds", "missing", NULL
	};
	g_autoptr(GString) out = NULL;
	guint i;
	guint j;

	(void)options;
	(void)user_data;
	(void)error;

	out = g_string_new(NULL);

	for (j = 0; NULL != header[j]; j++)
	{
		if (j > 0)
			g_string_append_c(out, ',');

		g_string_append(out, header[j]);
	}

	g_string_append(out, "\r\n");

	for (i = 0; (NULL != opportunities) && (i < json_array_get_length(opportunities)); i++)
	{
		JsonObject *row = json_array_get_object_element(opportunities, i);
		g_autofree gchar *capital = arb_money_text(row, "capital");
		g_autofree gchar *net = arb_money_text(row, "net");
		g_autofree gchar *roi = arb_ratio_text(row, "roi");
		g_autofree gchar *per_day = arb_ratio_text(row, "roi_per_day");
		g_autofree gchar *annual = arb_ratio_text(row, "annualized");
		g_autofree gchar *confidence = arb_ratio_text(row, "confidence");
		g_autofree gchar *sale_rate = arb_ratio_text(row, "sale_rate");
		g_autofree gchar *units = NULL;
		g_autofree gchar *age = NULL;
		g_autoptr(GString) missing = NULL;
		JsonArray *list;
		const gchar *fields[17];

		units = g_strdup_printf("%" G_GINT64_FORMAT, venture_json_object_get_int(row, "units", 0));
		age = g_strdup_printf("%" G_GINT64_FORMAT, venture_json_object_get_int(row, "age_seconds", 0));
		missing = g_string_new(NULL);
		list = json_object_has_member(row, "missing") ? json_object_get_array_member(row, "missing")
		                                              : NULL;

		for (j = 0; (NULL != list) && (j < json_array_get_length(list)); j++)
		{
			if (j > 0)
				g_string_append(missing, "; ");

			g_string_append(missing, json_array_get_string_element(list, j));
		}

		fields[0] = venture_json_object_get_string(row, "strategy", "");
		fields[1] = venture_json_object_get_string(row, "key", "");
		fields[2] = venture_json_object_get_string(row, "title", "");
		fields[3] = venture_json_object_get_string(row, "instrument_name",
		                                           venture_json_object_get_string(row, "instrument_key", ""));
		fields[4] = arb_side_text(row, "buy", "venue_name");
		fields[5] = arb_side_text(row, "sell", "venue_name");
		fields[6] = units;
		fields[7] = venture_json_object_get_string(row, "currency", "");
		fields[8] = capital;
		fields[9] = net;
		fields[10] = roi;
		fields[11] = per_day;
		fields[12] = annual;
		fields[13] = confidence;
		fields[14] = sale_rate;
		fields[15] = age;
		fields[16] = missing->str;

		for (j = 0; j < G_N_ELEMENTS(fields); j++)
		{
			if (j > 0)
				g_string_append_c(out, ',');

			arb_csv_field(out, fields[j]);
		}

		g_string_append(out, "\r\n");
	}

	return g_string_free_to_bytes(g_steal_pointer(&out));
}

/* --- shopping_list ------------------------------------------------------------ */

typedef struct
{
	gchar		*venue;
	GPtrArray	*lines;		/* ArbLine */
	GPtrArray	*totals;	/* VentureMoney per currency */
} ArbStop;

typedef struct
{
	gchar		*instrument;
	gint64		 quantity;
	VentureMoney	*amount;
} ArbLine;

static void
arb_line_free(gpointer data)
{
	ArbLine *line;

	line = data;
	g_free(line->instrument);
	g_clear_pointer(&line->amount, venture_money_free);
	g_free(line);
}

static void
arb_stop_free(gpointer data)
{
	ArbStop *stop;

	stop = data;
	g_free(stop->venue);
	g_ptr_array_unref(stop->lines);
	g_ptr_array_unref(stop->totals);
	g_free(stop);
}

/* The name a leg's venue goes by in its row: the buy side's, an input's
 * or an outcome's, else its key. */
static const gchar *
arb_leg_venue_name(
	JsonObject	*row,
	JsonObject	*leg
){
	const gchar *key;
	const gchar *lists[] = { "inputs", "outcomes" };
	guint i;
	guint j;

	key = venture_json_object_get_string(leg, "venue_key", "");

	if (0 == g_strcmp0(arb_side_text(row, "buy", "venue_key"), key))
		return arb_side_text(row, "buy", "venue_name");

	for (i = 0; i < G_N_ELEMENTS(lists); i++)
	{
		JsonArray *array;

		array = json_object_has_member(row, lists[i]) ? json_object_get_array_member(row, lists[i]) : NULL;

		for (j = 0; (NULL != array) && (j < json_array_get_length(array)); j++)
		{
			JsonObject *line = json_array_get_object_element(array, j);

			if (0 == g_strcmp0(venture_json_object_get_string(line, "venue_key", NULL), key))
				return venture_json_object_get_string(line, "venue_name", key);
		}
	}

	return key;
}

/* The name of a leg's instrument as its row knows it. */
static const gchar *
arb_leg_instrument_name(
	JsonObject	*row,
	JsonObject	*leg
){
	const gchar *key;
	const gchar *lists[] = { "inputs", "outcomes" };
	guint i;
	guint j;

	key = venture_json_object_get_string(leg, "instrument_key", "");

	for (i = 0; i < G_N_ELEMENTS(lists); i++)
	{
		JsonArray *array;

		array = json_object_has_member(row, lists[i]) ? json_object_get_array_member(row, lists[i]) : NULL;

		for (j = 0; (NULL != array) && (j < json_array_get_length(array)); j++)
		{
			JsonObject *line = json_array_get_object_element(array, j);

			if (0 == g_strcmp0(venture_json_object_get_string(line, "instrument_key", NULL), key))
				return venture_json_object_get_string(line, "name", key);
		}
	}

	if (0 == g_strcmp0(venture_json_object_get_string(row, "instrument_key", NULL), key))
		return venture_json_object_get_string(row, "instrument_name", key);

	return key;
}

static GBytes *
arb_export_shopping_list(
	JsonArray	 *opportunities,
	JsonObject	 *options,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(GPtrArray) stops = NULL;
	g_autoptr(GString) out = NULL;
	guint i;
	guint j;
	guint k;

	(void)options;
	(void)user_data;

	stops = g_ptr_array_new_with_free_func(arb_stop_free);

	/* Every buy and stake, at its venue, like added to like. */
	for (i = 0; (NULL != opportunities) && (i < json_array_get_length(opportunities)); i++)
	{
		JsonObject *row = json_array_get_object_element(opportunities, i);
		JsonArray *legs;

		legs = json_object_has_member(row, "legs") ? json_object_get_array_member(row, "legs") : NULL;

		for (j = 0; (NULL != legs) && (j < json_array_get_length(legs)); j++)
		{
			JsonObject *leg = json_array_get_object_element(legs, j);
			const gchar *kind = venture_json_object_get_string(leg, "kind", "");
			const gchar *venue = arb_leg_venue_name(row, leg);
			const gchar *instrument = arb_leg_instrument_name(row, leg);
			g_autoptr(VentureMoney) amount = NULL;
			ArbStop *stop = NULL;
			ArbLine *line = NULL;
			const gchar *text;

			if ((0 != g_strcmp0(kind, "buy")) && (0 != g_strcmp0(kind, "stake")))
				continue;

			text = venture_json_object_get_string(leg, "amount", NULL);
			amount = (NULL != text) ? venture_money_from_string(text, NULL, error) : NULL;

			if ((NULL != text) && (NULL == amount))
				return NULL;

			for (k = 0; (NULL == stop) && (k < stops->len); k++)
				if (0 == g_strcmp0(((ArbStop *)g_ptr_array_index(stops, k))->venue, venue))
					stop = g_ptr_array_index(stops, k);

			if (NULL == stop)
			{
				stop = g_new0(ArbStop, 1);
				stop->venue = g_strdup(venue);
				stop->lines = g_ptr_array_new_with_free_func(arb_line_free);
				stop->totals = venture_money_totals_new();
				g_ptr_array_add(stops, stop);
			}

			for (k = 0; (NULL == line) && (k < stop->lines->len); k++)
			{
				ArbLine *candidate = g_ptr_array_index(stop->lines, k);

				if ((0 == g_strcmp0(candidate->instrument, instrument)) &&
				    ((NULL == amount) || (NULL == candidate->amount) ||
				     (0 == g_strcmp0(venture_money_get_currency(candidate->amount),
				                     venture_money_get_currency(amount)))))
					line = candidate;
			}

			if (NULL == line)
			{
				line = g_new0(ArbLine, 1);
				line->instrument = g_strdup(instrument);
				g_ptr_array_add(stop->lines, line);
			}

			line->quantity += venture_json_object_get_int(leg, "quantity", 0);

			if (NULL != amount)
			{
				if (NULL == line->amount)
					line->amount = venture_money_copy(amount);
				else
				{
					VentureMoney *sum = venture_money_add(line->amount, amount, error);

					if (NULL == sum)
						return NULL;

					venture_money_free(line->amount);
					line->amount = sum;
				}

				if (!venture_money_totals_add(stop->totals, amount, error))
					return NULL;
			}
		}
	}

	out = g_string_new("Shopping list\n=============\n");

	if (0 == stops->len)
		g_string_append(out, "\nNothing to buy.\n");

	for (i = 0; i < stops->len; i++)
	{
		ArbStop *stop = g_ptr_array_index(stops, i);

		g_string_append_printf(out, "\nAt %s\n", stop->venue);

		for (j = 0; j < stop->lines->len; j++)
		{
			ArbLine *line = g_ptr_array_index(stop->lines, j);
			g_autofree gchar *amount = (NULL != line->amount) ? venture_money_to_string(line->amount)
			                                                 : g_strdup("unpriced");

			if (line->quantity > 0)
				g_string_append_printf(out, "  %" G_GINT64_FORMAT " x %s  %s\n", line->quantity,
				                       line->instrument, amount);
			else
				g_string_append_printf(out, "  %s  %s\n", line->instrument, amount);
		}

		venture_money_totals_sort(stop->totals, NULL);

		for (j = 0; j < stop->totals->len; j++)
		{
			g_autofree gchar *total = venture_money_to_string(g_ptr_array_index(stop->totals, j));

			g_string_append_printf(out, "  Total: %s\n", total);
		}
	}

	return g_string_free_to_bytes(g_steal_pointer(&out));
}

/* --- The registry -------------------------------------------------------------- */

VentureExportFormatRegistry *
venture_export_format_registry_new(void)
{
	VentureExportFormatRegistry *self;

	self = g_object_new(VENTURE_TYPE_EXPORT_FORMAT_REGISTRY, NULL);
	venture_arbitrage_register_builtin_exports(self);

	return self;
}

void
venture_arbitrage_register_builtin_exports(VentureExportFormatRegistry *registry)
{
	venture_export_format_registry_add(registry, "csv", "CSV", "text/csv; charset=utf-8", "csv",
	                                   arb_export_csv, NULL, NULL, NULL);
	venture_export_format_registry_add(registry, "shopping_list", "Shopping list",
	                                   "text/plain; charset=utf-8", "txt",
	                                   arb_export_shopping_list, NULL, NULL, NULL);
}

gboolean
venture_export_format_registry_add(
	VentureExportFormatRegistry	 *self,
	const gchar			 *name,
	const gchar			 *label,
	const gchar			 *content_type,
	const gchar			 *extension,
	VentureExportFunc		  func,
	gpointer			  user_data,
	GDestroyNotify			  destroy,
	GError				**error
){
	ArbExport *entry;
	const gchar *cursor;

	g_return_val_if_fail(VENTURE_IS_EXPORT_FORMAT_REGISTRY(self), FALSE);
	g_return_val_if_fail(NULL != func, FALSE);

	if (!venture_arbitrage_name_check("export format", name, error))
		return FALSE;

	if ((NULL == extension) || (NULL == content_type) ||
	    !venture_arbitrage_name_check("file extension", extension, error))
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "An export format needs a content type and an extension");
		return FALSE;
	}

	/* It becomes a response header verbatim; a line break in it would
	 * be a header of the plugin's choosing. */
	for (cursor = content_type; '\0' != *cursor; cursor++)
	{
		if (g_ascii_iscntrl(*cursor))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "Export format %s: the content type holds a control character", name);
			return FALSE;
		}
	}

	if (NULL != arb_export_lookup(self, name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "An export format named %s is already registered", name);
		return FALSE;
	}

	entry = g_new0(ArbExport, 1);
	entry->name = g_strdup(name);
	entry->label = g_strdup((NULL != label) ? label : name);
	entry->content_type = g_strdup(content_type);
	entry->extension = g_strdup(extension);
	entry->func = func;
	entry->user_data = user_data;
	entry->destroy = destroy;
	g_ptr_array_add(self->entries, entry);

	return TRUE;
}

gboolean
venture_export_format_registry_has(
	VentureExportFormatRegistry	*self,
	const gchar			*name
){
	g_return_val_if_fail(VENTURE_IS_EXPORT_FORMAT_REGISTRY(self), FALSE);

	return NULL != arb_export_lookup(self, name);
}

gboolean
venture_export_format_registry_remove(
	VentureExportFormatRegistry	*self,
	const gchar	*name
){
	gpointer entry;

	g_return_val_if_fail(VENTURE_IS_EXPORT_FORMAT_REGISTRY(self), FALSE);

	entry = (gpointer)arb_export_lookup(self, name);

	/* Only the plugin manager's rollback calls this: a scan, a venue or
	 * a page holds a name, never an entry, so nothing is left pointing
	 * at what is freed here. */
	return (NULL != entry) && g_ptr_array_remove(self->entries, entry);
}

gchar **
venture_export_format_registry_dup_names(VentureExportFormatRegistry *self)
{
	GStrvBuilder *builder;
	gchar **names;
	guint i;

	g_return_val_if_fail(VENTURE_IS_EXPORT_FORMAT_REGISTRY(self), NULL);

	builder = g_strv_builder_new();

	for (i = 0; i < self->entries->len; i++)
		g_strv_builder_add(builder, ((ArbExport *)g_ptr_array_index(self->entries, i))->name);

	names = g_strv_builder_end(builder);
	g_strv_builder_unref(builder);

	return names;
}

const gchar *
venture_export_format_registry_get_label(
	VentureExportFormatRegistry	*self,
	const gchar			*name
){
	ArbExport *entry;

	g_return_val_if_fail(VENTURE_IS_EXPORT_FORMAT_REGISTRY(self), NULL);

	entry = arb_export_lookup(self, name);

	return (NULL != entry) ? entry->label : NULL;
}

const gchar *
venture_export_format_registry_get_content_type(
	VentureExportFormatRegistry	*self,
	const gchar			*name
){
	ArbExport *entry;

	g_return_val_if_fail(VENTURE_IS_EXPORT_FORMAT_REGISTRY(self), NULL);

	entry = arb_export_lookup(self, name);

	return (NULL != entry) ? entry->content_type : NULL;
}

const gchar *
venture_export_format_registry_get_extension(
	VentureExportFormatRegistry	*self,
	const gchar			*name
){
	ArbExport *entry;

	g_return_val_if_fail(VENTURE_IS_EXPORT_FORMAT_REGISTRY(self), NULL);

	entry = arb_export_lookup(self, name);

	return (NULL != entry) ? entry->extension : NULL;
}

GBytes *
venture_export_format_registry_export(
	VentureExportFormatRegistry	 *self,
	const gchar			 *name,
	JsonArray			 *opportunities,
	JsonObject			 *options,
	GError				**error
){
	ArbExport *entry;

	g_return_val_if_fail(VENTURE_IS_EXPORT_FORMAT_REGISTRY(self), NULL);

	entry = arb_export_lookup(self, name);

	if (NULL == entry)
	{
		g_auto(GStrv) names = venture_export_format_registry_dup_names(self);
		g_autofree gchar *list = g_strjoinv(", ", names);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "\"%s\" is not an export format; registered: %s",
		            (NULL != name) ? name : "", list);
		return NULL;
	}

	return entry->func(opportunities, options, entry->user_data, error);
}
