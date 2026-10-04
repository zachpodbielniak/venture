/*
 * venture-feeds-providers.c - The providers that ship, and exec plugins'
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Four ways to get market data without writing C:
 *
 * - http_json: GET a JSON document and map its items to listings, figures
 *   or quotes with dotted paths. A generic scraper for any API that answers
 *   JSON.
 * - csv: the same mapping over the rows of a CSV, read from an address or
 *   from a file under feeds.file_roots.
 * - file_jsonl: a file in the JSON-lines protocol of docs/plugins.org --
 *   whatever produced it.
 * - exec: an exec plugin's program, run with the same protocol on its
 *   standard output, registered by the plugin's `data_source_provider`
 *   manifest entry.
 *
 * Parsing is done on a GTask thread, never on the feeds worker's loop: a
 * fifty-megabyte answer must not stop every other source's timers. The
 * HTTP exchange itself stays on the worker, which owns the session.
 */

#include "venture.h"
#include "feeds/venture-feeds-private.h"

#include <string.h>

/* The longest line a file_jsonl source may hold. */
#define FEED_JSONL_MAX_LINE (1024 * 1024)

/* The most refusal notes one mapping writes; the count says the rest. */
#define FEED_MAX_ITEM_NOTES (5)

/* --- Settings helpers ----------------------------------------------------------- */

static const gchar *
feed_setting_string(
	JsonObject	*settings,
	const gchar	*name
){
	JsonNode *node;

	node = (NULL != settings) ? json_object_get_member(settings, name) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)))
		return NULL;

	return json_node_get_string(node);
}

static gboolean
feed_setting_bool(
	JsonObject	*settings,
	const gchar	*name,
	gboolean	 fallback
){
	JsonNode *node;

	node = (NULL != settings) ? json_object_get_member(settings, name) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_BOOLEAN != json_node_get_value_type(node)))
		return fallback;

	return json_node_get_boolean(node);
}

static gint64
feed_setting_int(
	JsonObject	*settings,
	const gchar	*name,
	gint64		 fallback
){
	JsonNode *node;

	node = (NULL != settings) ? json_object_get_member(settings, name) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_INT64 != json_node_get_value_type(node)))
		return fallback;

	return json_node_get_int(node);
}

static JsonObject *
feed_setting_object(
	JsonObject	*settings,
	const gchar	*name
){
	JsonNode *node;

	node = (NULL != settings) ? json_object_get_member(settings, name) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return NULL;

	return json_node_get_object(node);
}

/* --- Mapping items ---------------------------------------------------------------- */

/*
 * A value at a dotted path: "item.id", "data.0.price". A segment of digits
 * indexes an array.
 */
static JsonNode *
feed_path_lookup(
	JsonNode	*root,
	const gchar	*path
){
	g_auto(GStrv) segments = NULL;
	JsonNode *node;
	guint i;

	if ((NULL == root) || (NULL == path))
		return NULL;

	if ('\0' == path[0])
		return root;

	segments = g_strsplit(path, ".", -1);
	node = root;

	for (i = 0; (NULL != segments[i]) && (NULL != node); i++)
	{
		if (JSON_NODE_HOLDS_OBJECT(node))
			node = json_object_get_member(json_node_get_object(node), segments[i]);
		else if (JSON_NODE_HOLDS_ARRAY(node) &&
		         (strspn(segments[i], "0123456789") == strlen(segments[i])) &&
		         ('\0' != segments[i][0]))
		{
			JsonArray *array = json_node_get_array(node);
			guint64 index = g_ascii_strtoull(segments[i], NULL, 10);

			node = (index < json_array_get_length(array))
				? json_array_get_element(array, (guint)index) : NULL;
		}
		else
			node = NULL;
	}

	return node;
}

/*
 * A scalar as text. A JSON number with a fraction is a double by the time
 * json-glib hands it over, so it is spelt back in its shortest exact form
 * (what the producer wrote, for any price of seventeen digits or fewer)
 * and then parsed as a decimal: no arithmetic ever happens on the double.
 */
static gchar *
feed_value_text(JsonNode *node)
{
	GType type;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
		return NULL;

	type = json_node_get_value_type(node);

	if (G_TYPE_STRING == type)
		return g_strdup(json_node_get_string(node));

	if (G_TYPE_INT64 == type)
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));

	if (G_TYPE_DOUBLE == type)
	{
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

		return g_strdup(g_ascii_dtostr(buffer, sizeof(buffer), json_node_get_double(node)));
	}

	if (G_TYPE_BOOLEAN == type)
		return g_strdup(json_node_get_boolean(node) ? "true" : "false");

	return NULL;
}

typedef struct
{
	const gchar	*record_as;	/* listing, stat or quote */
	JsonObject	*fields;	/* target -> path */
	gboolean	 minor_units;
	const gchar	*odds_format;
	gboolean	 complete;
	gint64		 taken_at;
	const gchar	*currency;
	const gchar	*default_venue;
} FeedMapping;

static gchar *
feed_field(
	FeedMapping	*mapping,
	JsonNode	*item,
	const gchar	*target
){
	const gchar *path;

	path = feed_setting_string(mapping->fields, target);

	return (NULL != path) ? feed_value_text(feed_path_lookup(item, path)) : NULL;
}

/* A price: minor units given whole, or a decimal at the currency's exponent;
 * NONE when the item has none. */
static gboolean
feed_field_price(
	FeedMapping	 *mapping,
	JsonNode	 *item,
	const gchar	 *target,
	const gchar	 *currency,
	gint64		 *out,
	GError		**error
){
	g_autofree gchar *text = NULL;

	text = feed_field(mapping, item, target);
	*out = VENTURE_SERIES_NONE;

	if (NULL == text)
		return TRUE;

	if (mapping->minor_units)
	{
		gchar *end;
		gint64 value;

		value = g_ascii_strtoll(text, &end, 10);

		if ((end == text) || ('\0' != *end) || (value < 0))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s must be a whole number of minor units", target);
			return FALSE;
		}

		*out = value;
		return TRUE;
	}

	return venture_feed_decimal_to_minor(text, currency, out, error);
}

static gint64
feed_field_count(
	FeedMapping	*mapping,
	JsonNode	*item,
	const gchar	*target,
	gint64		 fallback
){
	g_autofree gchar *text = NULL;
	gchar *end;
	gint64 value;

	text = feed_field(mapping, item, target);

	if (NULL == text)
		return fallback;

	value = g_ascii_strtoll(text, &end, 10);

	return ((end != text) && ('\0' == *end) && (value >= 0)) ? value : fallback;
}

/*
 * One item into the batch: its instrument (once per key), its venue's
 * snapshot (begun on the first item of that venue), then the listing,
 * figures or quote the mapping asks for.
 */
static gboolean
feed_map_item(
	VentureFeedBatch	 *batch,
	FeedMapping		 *mapping,
	JsonNode		 *item,
	GHashTable		 *seen_instruments,
	GHashTable		 *seen_venues,
	GError			**error
){
	g_autofree gchar *venue = NULL;
	g_autofree gchar *instrument = NULL;
	g_autofree gchar *currency_field = NULL;
	const gchar *currency;

	venue = feed_field(mapping, item, "venue");

	if (NULL == venue)
		venue = g_strdup(mapping->default_venue);

	instrument = feed_field(mapping, item, "instrument");

	if (venture_string_is_empty(instrument))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "an item has no instrument");
		return FALSE;
	}

	currency_field = feed_field(mapping, item, "currency");
	currency = (NULL != currency_field) ? currency_field : mapping->currency;

	if (!g_hash_table_contains(seen_instruments, instrument))
	{
		g_autofree gchar *name = feed_field(mapping, item, "name");
		g_autofree gchar *category = feed_field(mapping, item, "category");
		g_autofree gchar *kind = feed_field(mapping, item, "kind");
		g_autofree gchar *parent = feed_field(mapping, item, "parent");

		if (!venture_feed_batch_add_instrument(batch, instrument, name, kind, category,
		                                       parent, NULL, error))
			return FALSE;

		g_hash_table_add(seen_instruments, g_strdup(instrument));
	}

	if (0 == g_strcmp0(mapping->record_as, "quote"))
	{
		g_autofree gchar *odds = feed_field(mapping, item, "odds");
		g_autofree gchar *side_name = feed_field(mapping, item, "side");
		VentureSeriesQuoteSide side;
		gint64 value;
		gint64 liquidity;

		side = (0 == g_strcmp0(side_name, "lay")) ? VENTURE_SERIES_QUOTE_LAY
		     : (0 == g_strcmp0(side_name, "bid")) ? VENTURE_SERIES_QUOTE_BID
		     : (0 == g_strcmp0(side_name, "ask")) ? VENTURE_SERIES_QUOTE_ASK
		     : (0 == g_strcmp0(side_name, "back")) ? VENTURE_SERIES_QUOTE_BACK
		     : (NULL != odds) ? VENTURE_SERIES_QUOTE_BACK : VENTURE_SERIES_QUOTE_ASK;

		if (NULL != odds)
		{
			if (!venture_feed_decimal_to_odds(odds, mapping->odds_format, &value, error))
				return FALSE;
		}
		else if (!feed_field_price(mapping, item, "price", currency, &value, error))
			return FALSE;

		if (VENTURE_SERIES_NONE == value)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "a quote has neither price nor odds");
			return FALSE;
		}

		if (!feed_field_price(mapping, item, "liquidity", currency, &liquidity, error))
			return FALSE;

		return venture_feed_batch_add_quote(batch, venue, instrument, side, value,
		                                    (NULL != odds) ? NULL : currency, liquidity,
		                                    mapping->taken_at, error);
	}

	if (!g_hash_table_contains(seen_venues, venue))
	{
		if (!venture_feed_batch_begin_snapshot(batch, venue, currency, mapping->taken_at,
		                                       mapping->complete, error))
			return FALSE;

		g_hash_table_add(seen_venues, g_strdup(venue));
	}

	if (0 == g_strcmp0(mapping->record_as, "stat"))
	{
		static const gchar *const prices[] = { "min", "market", "mean", "median", "sale_avg" };
		gint64 figures[G_N_ELEMENTS(prices)];
		guint i;

		for (i = 0; i < G_N_ELEMENTS(prices); i++)
		{
			if (!feed_field_price(mapping, item, prices[i], currency, &figures[i], error))
				return FALSE;
		}

		return venture_feed_batch_add_stats(batch, venue, instrument, figures[0],
			figures[1], figures[2], figures[3], figures[4],
			feed_field_count(mapping, item, "quantity", VENTURE_SERIES_NONE),
			feed_field_count(mapping, item, "listings", VENTURE_SERIES_NONE),
			feed_field_count(mapping, item, "sold", VENTURE_SERIES_NONE), error);
	}

	{
		g_autofree gchar *id = feed_field(mapping, item, "id");
		g_autofree gchar *side = feed_field(mapping, item, "side");
		gint64 price;

		if (!feed_field_price(mapping, item, "price", currency, &price, error))
			return FALSE;

		if (VENTURE_SERIES_NONE == price)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "a listing has no price");
			return FALSE;
		}

		return venture_feed_batch_add_listing(batch, venue, instrument,
			(NULL != id) ? venture_series_listing_id_from_string(id) : 0,
			price, feed_field_count(mapping, item, "quantity", 1),
			0 == g_strcmp0(side, "buy"),
			feed_field_count(mapping, item, "expires_in_min", -1), error);
	}
}

/*
 * The mapping, read from the settings. `fields.instrument` is the one path
 * every mapping needs; the rest default to absent.
 */
static gboolean
feed_mapping_init(
	FeedMapping		 *mapping,
	VentureFeedRequest	 *request,
	gint64			  taken_at,
	GError			**error
){
	JsonObject *settings;
	VentureFeedSource *source;

	settings = venture_feed_request_get_settings(request);
	source = venture_feed_request_get_source(request);

	mapping->record_as = feed_setting_string(settings, "record_as");

	if (NULL == mapping->record_as)
		mapping->record_as = "listing";

	mapping->fields = feed_setting_object(settings, "fields");
	mapping->minor_units = (0 == g_strcmp0(feed_setting_string(settings, "price_units"), "minor"));
	mapping->odds_format = feed_setting_string(settings, "odds_format");
	mapping->complete = feed_setting_bool(settings, "complete", TRUE);
	mapping->taken_at = taken_at;
	mapping->currency = venture_feed_source_get_currency(source);
	mapping->default_venue = venture_feed_request_get_unit(request);

	if ((NULL == mapping->fields) || (NULL == feed_setting_string(mapping->fields, "instrument")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "The settings need fields.instrument: the path to each item's key");
		return FALSE;
	}

	return TRUE;
}

static void
feed_map_items(
	VentureFeedBatch	*batch,
	FeedMapping		*mapping,
	JsonArray		*items
){
	g_autoptr(GHashTable) seen_instruments = NULL;
	g_autoptr(GHashTable) seen_venues = NULL;
	guint notes;
	guint i;

	seen_instruments = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	seen_venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	notes = 0;

	for (i = 0; i < json_array_get_length(items); i++)
	{
		g_autoptr(GError) error = NULL;

		if (feed_map_item(batch, mapping, json_array_get_element(items, i),
		                  seen_instruments, seen_venues, &error))
			continue;

		venture_feed_batch_add_refused(batch, 1);

		if (notes++ < FEED_MAX_ITEM_NOTES)
		{
			g_autofree gchar *note = NULL;

			note = g_strdup_printf("Item %u: %s", i,
			                       (NULL != error) ? error->message : "refused");
			venture_feed_batch_add_note(batch, note);
		}
	}
}

/* --- Shared: parse a body on a thread ---------------------------------------------- */

typedef enum
{
	FEED_PARSE_JSON = 0,
	FEED_PARSE_CSV,
	FEED_PARSE_JSONL
} FeedParseKind;

typedef struct
{
	FeedParseKind		 kind;
	VentureFeedRequest	*request;
	GBytes			*body;		/* NULL: read the file named below */
	gchar			*file;
	gint64			 taken_at;
} FeedParseJob;

static void
feed_parse_job_free(gpointer data)
{
	FeedParseJob *job = data;

	g_clear_object(&job->request);
	g_clear_pointer(&job->body, g_bytes_unref);
	g_free(job->file);
	g_free(job);
}

static VentureFeedBatch *
feed_parse_json(
	FeedParseJob	 *job,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(VentureFeedBatch) batch = NULL;
	g_autoptr(GError) local_error = NULL;
	const gchar *items_path;
	FeedMapping mapping;
	JsonNode *items;
	gsize length;
	const gchar *data;

	if (!feed_mapping_init(&mapping, job->request, job->taken_at, error))
		return NULL;

	data = g_bytes_get_data(job->body, &length);
	parser = json_parser_new_immutable();

	if (!json_parser_load_from_data(parser, data, (gssize)length, &local_error))
	{
		/* json-glib's message quotes the offending text; say where only. */
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "The answer is not well-formed JSON");
		return NULL;
	}

	items_path = feed_setting_string(venture_feed_request_get_settings(job->request), "items");
	items = feed_path_lookup(json_parser_get_root(parser), (NULL != items_path) ? items_path : "");

	if ((NULL == items) || !JSON_NODE_HOLDS_ARRAY(items))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "The answer has no list at \"%s\"", (NULL != items_path) ? items_path : "");
		return NULL;
	}

	batch = venture_feed_batch_new();
	feed_map_items(batch, &mapping, json_node_get_array(items));

	return g_steal_pointer(&batch);
}

/*
 * RFC 4180: fields separated by the delimiter, a field in double quotes
 * may hold the delimiter, a newline and "" for a quote. Each row becomes a
 * JSON object of column name to text, so the mapping that reads JSON reads
 * CSV with no second code path.
 */
static JsonArray *
feed_csv_rows(
	const gchar	 *data,
	gsize		  length,
	gchar		  delimiter,
	gboolean	  header,
	GError		**error
){
	g_autoptr(JsonArray) rows = NULL;
	g_autoptr(GPtrArray) names = NULL;
	g_autoptr(GPtrArray) row = NULL;
	g_autoptr(GString) field = NULL;
	gboolean quoted;
	gboolean in_quotes;
	gsize i;

	rows = json_array_new();
	row = g_ptr_array_new_with_free_func(g_free);
	field = g_string_new(NULL);
	quoted = FALSE;
	in_quotes = FALSE;

	for (i = 0; i <= length; i++)
	{
		gchar c = (i < length) ? data[i] : '\n';
		gboolean end_row;

		if (in_quotes)
		{
			if ('"' == c)
			{
				if ((i + 1 < length) && ('"' == data[i + 1]))
				{
					g_string_append_c(field, '"');
					i++;
				}
				else
					in_quotes = FALSE;
			}
			else if (i == length)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
				                    "The CSV ends inside a quoted field");
				return NULL;
			}
			else
				g_string_append_c(field, c);

			continue;
		}

		if (('"' == c) && (0 == field->len) && !quoted)
		{
			in_quotes = TRUE;
			quoted = TRUE;
			continue;
		}

		if ('\r' == c)
			continue;

		end_row = ('\n' == c);

		if ((c != delimiter) && !end_row)
		{
			g_string_append_c(field, c);
			continue;
		}

		g_ptr_array_add(row, g_strdup(field->str));
		g_string_truncate(field, 0);
		quoted = FALSE;

		if (!end_row)
			continue;

		/* A blank line is not a row. */
		if ((1 == row->len) && ('\0' == ((gchar *)g_ptr_array_index(row, 0))[0]))
		{
			g_ptr_array_set_size(row, 0);
			continue;
		}

		if (header && (NULL == names))
		{
			names = g_steal_pointer(&row);
			row = g_ptr_array_new_with_free_func(g_free);
			continue;
		}

		{
			JsonObject *object = json_object_new();
			guint j;

			for (j = 0; j < row->len; j++)
			{
				g_autofree gchar *name = NULL;

				name = ((NULL != names) && (j < names->len))
					? g_strdup(g_ptr_array_index(names, j))
					: g_strdup_printf("%u", j);
				json_object_set_string_member(object, name, g_ptr_array_index(row, j));
			}

			json_array_add_object_element(rows, object);
		}

		g_ptr_array_set_size(row, 0);
	}

	return g_steal_pointer(&rows);
}

static VentureFeedBatch *
feed_parse_csv(
	FeedParseJob	 *job,
	GError		**error
){
	g_autoptr(VentureFeedBatch) batch = NULL;
	g_autoptr(JsonArray) rows = NULL;
	JsonObject *settings;
	const gchar *delimiter;
	FeedMapping mapping;
	const gchar *data;
	gsize length;

	if (!feed_mapping_init(&mapping, job->request, job->taken_at, error))
		return NULL;

	settings = venture_feed_request_get_settings(job->request);
	delimiter = feed_setting_string(settings, "delimiter");

	if ((NULL != delimiter) && (1 != strlen(delimiter)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "delimiter must be one character");
		return NULL;
	}

	data = g_bytes_get_data(job->body, &length);

	if (!g_utf8_validate(data, (gssize)length, NULL))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "The CSV is not UTF-8");
		return NULL;
	}

	rows = feed_csv_rows(data, length, (NULL != delimiter) ? delimiter[0] : ',',
	                     feed_setting_bool(settings, "header", TRUE), error);

	if (NULL == rows)
		return NULL;

	batch = venture_feed_batch_new();
	feed_map_items(batch, &mapping, rows);

	return g_steal_pointer(&batch);
}

static VentureFeedBatch *
feed_parse_jsonl(
	FeedParseJob	 *job,
	GError		**error
){
	g_autoptr(VentureFeedBatch) batch = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	VentureJsonlReader *reader;
	const guint8 *data;
	gsize length;
	gboolean ok;

	data = g_bytes_get_data(job->body, &length);
	messages = g_ptr_array_new_with_free_func((GDestroyNotify)venture_jsonl_message_unref);
	reader = venture_jsonl_reader_new(FEED_JSONL_MAX_LINE);

	/* The first line that breaks the protocol fails the unit, as it fails
	 * an exec plugin's run: a batch cut at a bad line reads exactly like a
	 * complete one. */
	ok = venture_jsonl_reader_feed(reader, data, length, messages, error) &&
	     venture_jsonl_reader_finish(reader, messages, error);
	venture_jsonl_reader_free(reader);

	if (!ok)
		return NULL;

	batch = venture_feed_batch_new();

	if (!venture_feed_batch_add_jsonl(batch, messages,
	                                  venture_feed_source_get_currency(venture_feed_request_get_source(job->request)),
	                                  job->taken_at, error))
		return NULL;

	return g_steal_pointer(&batch);
}

static void
feed_parse_thread(
	GTask		*task,
	gpointer	 source,
	gpointer	 task_data,
	GCancellable	*cancellable
){
	FeedParseJob *job = task_data;
	VentureFeedBatch *batch;
	GError *error = NULL;

	(void)source;

	if (g_cancellable_set_error_if_cancelled(cancellable, &error))
	{
		g_task_return_error(task, error);
		return;
	}

	if (NULL == job->body)
	{
		job->body = venture_feed_request_read_file(job->request, job->file, &error);

		if (NULL == job->body)
		{
			g_task_return_error(task, error);
			return;
		}
	}

	switch (job->kind)
	{
	case FEED_PARSE_CSV:
		batch = feed_parse_csv(job, &error);
		break;

	case FEED_PARSE_JSONL:
		batch = feed_parse_jsonl(job, &error);
		break;

	case FEED_PARSE_JSON:
	default:
		batch = feed_parse_json(job, &error);
		break;
	}

	if (NULL == batch)
		g_task_return_error(task, error);
	else
		g_task_return_pointer(task, batch, (GDestroyNotify)venture_feed_batch_unref);
}

/* --- Fetching over HTTP, then parsing ----------------------------------------------- */

typedef struct
{
	FeedParseKind	 kind;
	GTask		*task;		/* the fetch's, completed with the batch */
} FeedHttpFetch;

static void
feed_parsed(
	GObject		*object,
	GAsyncResult	*result,
	gpointer	 user_data
){
	g_autoptr(GTask) task = user_data;
	VentureFeedBatch *batch;
	GError *error = NULL;

	(void)object;

	batch = g_task_propagate_pointer(G_TASK(result), &error);

	if (NULL == batch)
		g_task_return_error(task, error);
	else
		g_task_return_pointer(task, batch, (GDestroyNotify)venture_feed_batch_unref);
}

static void
feed_parse_in_thread(
	GTask			*task,
	FeedParseKind		 kind,
	VentureFeedRequest	*request,
	GBytes			*body,
	const gchar		*file,
	gint64			 taken_at
){
	g_autoptr(GTask) parse = NULL;
	FeedParseJob *job;

	job = g_new0(FeedParseJob, 1);
	job->kind = kind;
	job->request = g_object_ref(request);
	job->body = (NULL != body) ? g_bytes_ref(body) : NULL;
	job->file = g_strdup(file);
	job->taken_at = taken_at;

	parse = g_task_new(NULL, g_task_get_cancellable(task), feed_parsed, g_object_ref(task));
	g_task_set_task_data(parse, job, feed_parse_job_free);
	g_task_run_in_thread(parse, feed_parse_thread);
}

static void
feed_http_fetched(
	GObject		*object,
	GAsyncResult	*result,
	gpointer	 user_data
){
	g_autoptr(GTask) task = user_data;
	g_autoptr(VentureFeedHttpResponse) response = NULL;
	VentureFeedRequest *request;
	GError *error = NULL;
	FeedParseKind kind;
	gint64 taken_at;

	request = VENTURE_FEED_REQUEST(object);
	response = venture_feed_request_http_get_finish(request, result, &error);

	if (NULL == response)
	{
		g_task_return_error(task, error);
		return;
	}

	if (SOUP_STATUS_NOT_MODIFIED == response->status)
	{
		VentureFeedBatch *batch = venture_feed_batch_new();

		venture_feed_batch_set_not_modified(batch, TRUE);
		g_task_return_pointer(task, batch, (GDestroyNotify)venture_feed_batch_unref);
		return;
	}

	/* The snapshot is the source's, dated when the source says it was
	 * made; the fetch time only when it does not say. */
	taken_at = (VENTURE_SERIES_NONE != response->last_modified)
		? response->last_modified : venture_feed_request_get_fetched_at(request);
	kind = GPOINTER_TO_INT(g_task_get_task_data(task));

	feed_parse_in_thread(task, kind, request, response->body, NULL, taken_at);
}

/*
 * The headers setting: a mapping of name to template. A value that would
 * end the header line is refused -- a credential with a newline in it is
 * a header injection, not a credential.
 */
static gchar **
feed_build_headers(
	VentureFeedRequest	 *request,
	GError			**error
){
	g_autoptr(GPtrArray) headers = NULL;
	JsonObject *templates;
	JsonObjectIter iter;
	const gchar *name;
	JsonNode *node;

	headers = g_ptr_array_new_with_free_func(g_free);
	templates = feed_setting_object(venture_feed_request_get_settings(request), "headers");

	if (NULL != templates)
	{
		json_object_iter_init(&iter, templates);

		while (json_object_iter_next(&iter, &name, &node))
		{
			g_autofree gchar *value = NULL;

			if (!JSON_NODE_HOLDS_VALUE(node) ||
			    (G_TYPE_STRING != json_node_get_value_type(node)))
				continue;

			value = venture_feed_request_expand(request, json_node_get_string(node), FALSE, error);

			if (NULL == value)
				return NULL;

			if ((NULL != strpbrk(value, "\r\n")) || (NULL != strpbrk(name, "\r\n: ")))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
				            "The header %s cannot hold a line break", name);
				return NULL;
			}

			g_ptr_array_add(headers, g_strdup(name));
			g_ptr_array_add(headers, g_steal_pointer(&value));
		}
	}

	g_ptr_array_add(headers, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&headers), FALSE);
}

static void
feed_fetch_url_or_file(
	VentureDataSourceProvider	*provider,
	FeedParseKind			 kind,
	VentureFeedRequest		*request,
	GCancellable			*cancellable,
	GAsyncReadyCallback		 callback,
	gpointer			 user_data
){
	g_autoptr(GTask) task = NULL;
	g_autofree gchar *url = NULL;
	g_auto(GStrv) headers = NULL;
	JsonObject *settings;
	GError *error = NULL;
	const gchar *file;
	const gchar *url_template;

	task = g_task_new(provider, cancellable, callback, user_data);
	g_task_set_task_data(task, GINT_TO_POINTER(kind), NULL);
	settings = venture_feed_request_get_settings(request);
	file = feed_setting_string(settings, "file");
	url_template = feed_setting_string(settings, "url");

	if ((NULL != file) && (FEED_PARSE_JSON != kind))
	{
		g_autofree gchar *path = NULL;

		path = venture_feed_request_expand(request, file, FALSE, &error);

		if (NULL == path)
		{
			g_task_return_error(task, error);
			return;
		}

		/* A file has no Last-Modified to trust across copies; the
		 * snapshot is the fetch's. */
		feed_parse_in_thread(task, kind, request, NULL, path,
		                     venture_feed_request_get_fetched_at(request));
		return;
	}

	if (NULL == url_template)
	{
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                        (FEED_PARSE_JSON == kind)
		                        ? "The settings need url"
		                        : "The settings need url or file");
		return;
	}

	url = venture_feed_request_expand(request, url_template, TRUE, &error);
	headers = (NULL != url) ? feed_build_headers(request, &error) : NULL;

	if ((NULL == url) || (NULL == headers))
	{
		g_task_return_error(task, error);
		return;
	}

	venture_feed_request_http_get_async(request, url, (const gchar *const *)headers,
	                                    (guint)CLAMP(feed_setting_int(settings, "cost", 1), 1, 100000),
	                                    feed_http_fetched, g_steal_pointer(&task));
}

/* --- http_json ----------------------------------------------------------------------- */

typedef struct { GObject parent_instance; FeedParseKind kind; } FeedBuiltinProvider;
typedef struct { GObjectClass parent_class; } FeedBuiltinProviderClass;

static GType feed_builtin_provider_get_type(void);
static void feed_builtin_provider_iface_init(VentureDataSourceProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(FeedBuiltinProvider, feed_builtin_provider, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(VENTURE_TYPE_DATA_SOURCE_PROVIDER,
                                              feed_builtin_provider_iface_init))

static void
feed_builtin_provider_class_init(FeedBuiltinProviderClass *klass)
{
	(void)klass;
}

static void
feed_builtin_provider_init(FeedBuiltinProvider *self)
{
	(void)self;
}

static const gchar *
feed_builtin_name(VentureDataSourceProvider *provider)
{
	switch (((FeedBuiltinProvider *)provider)->kind)
	{
	case FEED_PARSE_CSV:
		return "csv";
	case FEED_PARSE_JSONL:
		return "file_jsonl";
	case FEED_PARSE_JSON:
	default:
		return "http_json";
	}
}

static const gchar *
feed_builtin_label(VentureDataSourceProvider *provider)
{
	switch (((FeedBuiltinProvider *)provider)->kind)
	{
	case FEED_PARSE_CSV:
		return "CSV from an address or a file";
	case FEED_PARSE_JSONL:
		return "JSON-lines file (the plugin protocol)";
	case FEED_PARSE_JSON:
	default:
		return "JSON over HTTP";
	}
}

/*
 * The settings each built-in reads. Two credential slots, `token` and
 * `api_key`, both sensitive: a template names them as {secret:token} in a
 * header or {secret:api_key} in the address, and neither is ever stored
 * in the record's settings. `url` is `x-endpoint`: it decides where those
 * credentials go, so they are bound to its origin when they are set
 * (venture_feeds_set_credentials()) and withheld once it names another.
 */
static const gchar feed_mapping_schema_properties[] =
	"\"record_as\":{\"type\":\"string\",\"title\":\"Items become\",\"enum\":[\"listing\",\"stat\",\"quote\"],\"default\":\"listing\"},"
	"\"fields\":{\"type\":\"object\",\"title\":\"Field paths\","
	  "\"description\":\"instrument (required), venue, name, category, kind, parent, price, quantity, id, side, expires_in_min, currency, min, market, mean, median, sale_avg, listings, sold, odds, liquidity\"},"
	"\"price_units\":{\"type\":\"string\",\"title\":\"Prices are\",\"enum\":[\"major\",\"minor\"],\"default\":\"major\"},"
	"\"odds_format\":{\"type\":\"string\",\"title\":\"Odds format\",\"enum\":[\"decimal\",\"american\",\"fractional\"]},"
	"\"complete\":{\"type\":\"boolean\",\"title\":\"Each fetch lists everything at its venue\",\"default\":true},"
	"\"units\":{\"type\":\"array\",\"title\":\"Units\",\"description\":\"One fetch each; {unit} in the address or file\"},"
	"\"instruments\":{\"type\":\"array\",\"title\":\"Known instruments\",\"description\":\"With track: known, the only ones stored\"},"
	"\"requests_per_hour\":{\"type\":\"integer\",\"title\":\"Request budget an hour\",\"default\":0},"
	"\"record_types\":{\"type\":\"array\",\"title\":\"Record types it may write\"},"
	"\"token\":{\"type\":\"string\",\"title\":\"Bearer token\",\"x-sensitive\":true},"
	"\"api_key\":{\"type\":\"string\",\"title\":\"API key\",\"x-sensitive\":true}";

static JsonNode *
feed_builtin_schema(VentureDataSourceProvider *provider)
{
	g_autofree gchar *text = NULL;

	switch (((FeedBuiltinProvider *)provider)->kind)
	{
	case FEED_PARSE_JSONL:
		text = g_strdup(
			"{\"type\":\"object\",\"required\":[\"file\"],\"properties\":{"
			"\"file\":{\"type\":\"string\",\"title\":\"File\",\"description\":\"Under feeds.file_roots; {unit} is filled in\"},"
			"\"units\":{\"type\":\"array\",\"title\":\"Units\"},"
			"\"instruments\":{\"type\":\"array\",\"title\":\"Known instruments\"},"
			"\"record_types\":{\"type\":\"array\",\"title\":\"Record types it may write\"}}}");
		break;

	case FEED_PARSE_CSV:
		text = g_strdup_printf(
			"{\"type\":\"object\",\"required\":[\"fields\"],\"properties\":{"
			"\"url\":{\"type\":\"string\",\"title\":\"Address\",\"x-endpoint\":true},"
			"\"file\":{\"type\":\"string\",\"title\":\"File\",\"description\":\"Under feeds.file_roots\"},"
			"\"headers\":{\"type\":\"object\",\"title\":\"Request headers\"},"
			"\"delimiter\":{\"type\":\"string\",\"title\":\"Delimiter\",\"default\":\",\"},"
			"\"header\":{\"type\":\"boolean\",\"title\":\"First row names the columns\",\"default\":true},"
			"\"cost\":{\"type\":\"integer\",\"title\":\"Budget cost of one request\",\"default\":1},"
			"%s}}", feed_mapping_schema_properties);
		break;

	case FEED_PARSE_JSON:
	default:
		text = g_strdup_printf(
			"{\"type\":\"object\",\"required\":[\"url\",\"fields\"],\"properties\":{"
			"\"url\":{\"type\":\"string\",\"title\":\"Address\",\"description\":\"{unit} and {secret:api_key} are filled in\",\"x-endpoint\":true},"
			"\"headers\":{\"type\":\"object\",\"title\":\"Request headers\",\"description\":\"Authorization: Bearer {secret:token}\"},"
			"\"items\":{\"type\":\"string\",\"title\":\"Path to the list of items\",\"default\":\"\"},"
			"\"cost\":{\"type\":\"integer\",\"title\":\"Budget cost of one request\",\"default\":1},"
			"%s}}", feed_mapping_schema_properties);
		break;
	}

	return json_from_string(text, NULL);
}

static void
feed_builtin_fetch_async(
	VentureDataSourceProvider	*provider,
	VentureFeedRequest		*request,
	GCancellable			*cancellable,
	GAsyncReadyCallback		 callback,
	gpointer			 user_data
){
	FeedParseKind kind;

	kind = ((FeedBuiltinProvider *)provider)->kind;

	if (FEED_PARSE_JSONL == kind)
	{
		g_autoptr(GTask) task = NULL;
		g_autofree gchar *path = NULL;
		const gchar *file;
		GError *error = NULL;

		task = g_task_new(provider, cancellable, callback, user_data);
		file = feed_setting_string(venture_feed_request_get_settings(request), "file");
		path = (NULL != file) ? venture_feed_request_expand(request, file, FALSE, &error) : NULL;

		if (NULL == path)
		{
			if (NULL == error)
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
				                    "The settings need file");
			g_task_return_error(task, error);
			return;
		}

		feed_parse_in_thread(task, kind, request, NULL, path,
		                     venture_feed_request_get_fetched_at(request));
		return;
	}

	feed_fetch_url_or_file(provider, kind, request, cancellable, callback, user_data);
}

static void
feed_builtin_provider_iface_init(VentureDataSourceProviderInterface *iface)
{
	iface->get_name = feed_builtin_name;
	iface->get_label = feed_builtin_label;
	iface->dup_settings_schema = feed_builtin_schema;
	iface->fetch_async = feed_builtin_fetch_async;
}

static VentureDataSourceProvider *
feed_builtin_new(FeedParseKind kind)
{
	FeedBuiltinProvider *self;

	self = g_object_new(feed_builtin_provider_get_type(), NULL);
	self->kind = kind;

	return VENTURE_DATA_SOURCE_PROVIDER(self);
}

void
venture_feeds_register_builtin_providers(VentureDataSourceProviderRegistry *registry)
{
	static const FeedParseKind kinds[] = { FEED_PARSE_JSON, FEED_PARSE_CSV, FEED_PARSE_JSONL };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
	{
		g_autoptr(VentureDataSourceProvider) provider = feed_builtin_new(kinds[i]);
		g_autoptr(GError) error = NULL;

		if (!venture_data_source_provider_registry_add(registry, provider, &error))
			g_debug("feeds: %s", error->message);
	}
}

/* --- exec: a plugin's program ------------------------------------------------------- */

typedef struct
{
	GObject		 parent_instance;
	gchar		*name;		/* the provider's registry name */
	gchar		*label;
	gchar		*plugin;
	gchar		*command;
	gchar		*attribution;	/* the manifest's line, judged; NULL for none */
} FeedExecProvider;

typedef struct { GObjectClass parent_class; } FeedExecProviderClass;

static GType feed_exec_provider_get_type(void);
static void feed_exec_provider_iface_init(VentureDataSourceProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(FeedExecProvider, feed_exec_provider, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(VENTURE_TYPE_DATA_SOURCE_PROVIDER,
                                              feed_exec_provider_iface_init))

static void
feed_exec_provider_finalize(GObject *object)
{
	FeedExecProvider *self = (FeedExecProvider *)object;

	g_free(self->name);
	g_free(self->label);
	g_free(self->plugin);
	g_free(self->command);
	g_free(self->attribution);

	G_OBJECT_CLASS(feed_exec_provider_parent_class)->finalize(object);
}

static void
feed_exec_provider_class_init(FeedExecProviderClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = feed_exec_provider_finalize;
}

static void
feed_exec_provider_init(FeedExecProvider *self)
{
	(void)self;
}

/*
 * What an exec fetch needs from the main thread: the plugin's frozen
 * program, the request built from its stored settings (as text, parsed
 * again on the thread that runs it), and whether plugins.allow_exec was
 * on when the source was frozen. A change to any of them refreezes every
 * source.
 */
typedef struct
{
	VentureExecSpec	*spec;
	gchar		*request_json;
	gboolean	 allow_exec;
} FeedExecFrozen;

static void
feed_exec_frozen_free(gpointer data)
{
	FeedExecFrozen *frozen = data;

	if (NULL != frozen->spec)
		venture_exec_spec_unref(frozen->spec);
	g_free(frozen->request_json);
	g_free(frozen);
}

static const gchar *
feed_exec_name(VentureDataSourceProvider *provider)
{
	return ((FeedExecProvider *)provider)->name;
}

static const gchar *
feed_exec_label(VentureDataSourceProvider *provider)
{
	return ((FeedExecProvider *)provider)->label;
}

static const gchar *
feed_exec_attribution(VentureDataSourceProvider *provider)
{
	return ((FeedExecProvider *)provider)->attribution;
}

static JsonNode *
feed_exec_schema(VentureDataSourceProvider *provider)
{
	(void)provider;

	/* Whatever the program reads is its own business; these are what
	 * every source has, and one credential slot for the program. */
	return json_from_string(
		"{\"type\":\"object\",\"properties\":{"
		"\"units\":{\"type\":\"array\",\"title\":\"Units\"},"
		"\"instruments\":{\"type\":\"array\",\"title\":\"Known instruments\"},"
		"\"requests_per_hour\":{\"type\":\"integer\",\"title\":\"Request budget an hour\",\"default\":0},"
		"\"record_types\":{\"type\":\"array\",\"title\":\"Record types it may write\"},"
		"\"token\":{\"type\":\"string\",\"title\":\"Credential for the program\",\"x-sensitive\":true}}}",
		NULL);
}

static gpointer
feed_exec_freeze(
	VentureDataSourceProvider	 *provider,
	VentureContext			 *context,
	JsonObject			 *settings,
	GDestroyNotify			 *out_free,
	GError				**error
){
	FeedExecProvider *self = (FeedExecProvider *)provider;
	g_autoptr(JsonObject) request = NULL;
	g_autoptr(JsonNode) node = NULL;
	VenturePluginManager *manager;
	FeedExecFrozen *frozen;
	VentureExecSpec *spec;

	(void)settings;

	manager = venture_context_get_plugin_manager(context);
	spec = (NULL != manager) ? venture_plugin_manager_lookup_exec(manager, self->plugin) : NULL;

	if (NULL == spec)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "The exec plugin %s is not loaded", self->plugin);
		return NULL;
	}

	frozen = g_new0(FeedExecFrozen, 1);
	frozen->spec = venture_exec_spec_ref(spec);
	g_object_get(venture_context_get_config(context), "plugins-allow-exec", &frozen->allow_exec, NULL);

	request = venture_plugin_manager_build_exec_request(manager, self->plugin, self->command, NULL);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, request);
	frozen->request_json = json_to_string(node, FALSE);

	*out_free = feed_exec_frozen_free;

	return frozen;
}

static void
feed_exec_thread(
	GTask		*task,
	gpointer	 source,
	gpointer	 task_data,
	GCancellable	*cancellable
){
	VentureFeedRequest *request = task_data;
	g_autoptr(JsonObject) params = NULL;
	g_autoptr(VentureExecResult) result = NULL;
	g_autoptr(VentureFeedBatch) batch = NULL;
	g_autoptr(GError) check_error = NULL;
	FeedExecFrozen *frozen;
	JsonObject *body;
	GError *error = NULL;
	JsonNode *parsed;

	(void)source;

	frozen = venture_feed_request_get_frozen(request);

	/* Checked again at every run, from what the main thread froze: the
	 * operator turning exec off refreezes every source. */
	if ((NULL == frozen) || !frozen->allow_exec)
	{
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		                        "The exec provider cannot run: plugins.allow_exec is off");
		return;
	}

	parsed = json_from_string(frozen->request_json, &error);

	if ((NULL == parsed) || !JSON_NODE_HOLDS_OBJECT(parsed))
	{
		g_clear_pointer(&parsed, json_node_unref);

		if (NULL == error)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			                    "The exec request could not be rebuilt");
		g_task_return_error(task, error);
		return;
	}

	body = json_object_ref(json_node_get_object(parsed));
	json_node_unref(parsed);

	/* The fetch's parameters: which unit, where it resumes, what it last
	 * saw, and the source's settings. Credentials go as secrets. */
	params = json_object_new();
	json_object_set_string_member(params, "unit", venture_feed_request_get_unit(request));

	if (NULL != venture_feed_request_get_cursor(request))
		json_object_set_string_member(params, "cursor", venture_feed_request_get_cursor(request));

	if (VENTURE_SERIES_NONE != venture_feed_request_get_if_modified_since(request))
	{
		g_autoptr(GDateTime) since = NULL;
		g_autofree gchar *text = NULL;

		since = g_date_time_new_from_unix_utc(venture_feed_request_get_if_modified_since(request));
		text = g_date_time_format_iso8601(since);
		json_object_set_string_member(params, "if_modified_since", text);
	}

	json_object_set_object_member(params, "source",
	                              json_object_ref(venture_feed_request_get_settings(request)));
	json_object_set_object_member(body, "params", g_steal_pointer(&params));

	result = venture_exec_run(frozen->spec, body, venture_feed_request_get_secrets(request),
	                          cancellable, &error);
	json_object_unref(body);

	if (NULL == result)
	{
		g_task_return_error(task, error);
		return;
	}

	batch = venture_feed_batch_new();

	if (!venture_feed_batch_add_jsonl(batch, venture_exec_result_get_messages(result),
	                                  venture_feed_source_get_currency(venture_feed_request_get_source(request)),
	                                  venture_feed_request_get_fetched_at(request), &error))
	{
		g_task_return_error(task, error);
		return;
	}

	/*
	 * A program that wrote data and then failed hands over a partial
	 * batch: what it wrote is kept and the unit is partial. One that
	 * failed having written nothing failed.
	 */
	if (!venture_exec_result_check(result, &check_error))
	{
		if (0 == venture_feed_batch_count_items(batch))
		{
			g_task_return_error(task, g_steal_pointer(&check_error));
			return;
		}

		if (NULL == venture_feed_batch_get_error(batch, NULL))
			venture_feed_batch_set_error(batch, check_error->message,
			                             MAX(venture_exec_result_get_retry_after(result), 0));
	}

	g_task_return_pointer(task, g_steal_pointer(&batch), (GDestroyNotify)venture_feed_batch_unref);
}

static void
feed_exec_fetch_async(
	VentureDataSourceProvider	*provider,
	VentureFeedRequest		*request,
	GCancellable			*cancellable,
	GAsyncReadyCallback		 callback,
	gpointer			 user_data
){
	g_autoptr(GTask) task = NULL;

	/* venture_exec_run() blocks for up to the plugin's timeout; on a GTask
	 * thread it blocks nobody's loop. */
	task = g_task_new(provider, cancellable, callback, user_data);
	g_task_set_task_data(task, g_object_ref(request), g_object_unref);
	g_task_run_in_thread(task, feed_exec_thread);
}

static void
feed_exec_provider_iface_init(VentureDataSourceProviderInterface *iface)
{
	iface->get_name = feed_exec_name;
	iface->get_label = feed_exec_label;
	iface->dup_settings_schema = feed_exec_schema;
	iface->freeze = feed_exec_freeze;
	iface->fetch_async = feed_exec_fetch_async;
	iface->get_attribution = feed_exec_attribution;
}

/*
 * The `data_source_provider` provides kind: an exec plugin naming a
 * provider its program implements. Like `automation_handler`, only an
 * exec plugin may provide one -- native and crispy plugins register theirs
 * in code -- and the keys are a closed list. It is accepted whether or not
 * the feeds module is on, so turning feeds on needs no plugin reload;
 * nothing runs while it is off.
 *
 * Judged whole (feed_validate_provider()) before any entry of the
 * manifest is registered, so a refused manifest registers nothing.
 */
static gboolean
feed_validate_provider(
	VenturePluginManager	 *manager,
	VenturePluginManifest	 *manifest,
	JsonObject		 *entry,
	gpointer		  user_data,
	GError			**error
){
	static const gchar *const allowed[] = {
		"kind", "name", "label", "command", "description", "attribution", NULL
	};
	g_autoptr(GList) members = NULL;
	const gchar *name;
	const gchar *command;
	const gchar *plugin;
	GList *l;

	(void)user_data;

	plugin = venture_plugin_manifest_get_name(manifest);
	members = json_object_get_members(entry);

	for (l = members; NULL != l; l = l->next)
	{
		if (!g_strv_contains(allowed, l->data))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "a data_source_provider entry has no \"%s\" "
			            "(it takes name, label, command, description and attribution)",
			            (const gchar *)l->data);
			return FALSE;
		}
	}

	name = feed_setting_string(entry, "name");
	command = feed_setting_string(entry, "command");

	if (!venture_data_source_provider_name_is_valid(name))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		                    "a data_source_provider needs a name: a lower-case "
		                    "letter followed by lower-case letters, digits and underscores");
		return FALSE;
	}

	if (json_object_has_member(entry, "command") &&
	    (venture_string_is_empty(command) || (strlen(command) > 256)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "data_source_provider %s has an empty or overlong command", name);
		return FALSE;
	}

	/*
	 * The line every page shows beside this provider's data, which the
	 * far end's terms may require. Plain text, judged here so a manifest
	 * that would put a paragraph, a newline or a number on every page is
	 * refused at load with its reason, rather than shown mangled.
	 */
	if (json_object_has_member(entry, "attribution"))
	{
		g_autoptr(GError) attribution_error = NULL;
		const gchar *attribution = feed_setting_string(entry, "attribution");

		if (NULL == attribution)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "data_source_provider %s: attribution must be a string", name);
			return FALSE;
		}

		if (!venture_data_source_attribution_check(attribution, &attribution_error))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "data_source_provider %s: %s", name, attribution_error->message);
			return FALSE;
		}
	}

	if (NULL == venture_plugin_manager_lookup_exec(manager, plugin))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		                    "a data_source_provider entry is run as a program, so only an "
		                    "exec plugin may provide one; a native or crispy plugin "
		                    "registers its provider in code");
		return FALSE;
	}

	if (NULL != venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(venture_plugin_manager_get_context(manager)),
		name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "A data source provider named %s is already registered", name);
		return FALSE;
	}

	return TRUE;
}

static gboolean
feed_accept_provider(
	VenturePluginManager	 *manager,
	VenturePluginManifest	 *manifest,
	JsonObject		 *entry,
	gpointer		  user_data,
	GError			**error
){
	g_autoptr(GError) local_error = NULL;
	FeedExecProvider *provider;
	const gchar *name;
	const gchar *command;
	const gchar *label;
	const gchar *attribution;

	if (!feed_validate_provider(manager, manifest, entry, user_data, error))
		return FALSE;

	name = feed_setting_string(entry, "name");
	command = feed_setting_string(entry, "command");
	label = feed_setting_string(entry, "label");

	provider = g_object_new(feed_exec_provider_get_type(), NULL);
	provider->name = g_strdup(name);
	provider->label = g_strdup((NULL != label) ? label : name);
	provider->plugin = g_strdup(venture_plugin_manifest_get_name(manifest));
	provider->command = g_strdup((NULL != command) ? command : "fetch");
	attribution = feed_setting_string(entry, "attribution");
	provider->attribution = (NULL != attribution) ? g_strstrip(g_strdup(attribution)) : NULL;

	if (!venture_data_source_provider_registry_add(
		venture_context_get_data_source_providers(venture_plugin_manager_get_context(manager)),
		VENTURE_DATA_SOURCE_PROVIDER(provider), &local_error))
	{
		g_object_unref(provider);
		g_propagate_error(error, g_steal_pointer(&local_error));
		return FALSE;
	}

	g_object_unref(provider);

	return TRUE;
}

/* Takes back an entry this manifest registered when a later one failed. */
static void
feed_remove_provider(
	VenturePluginManager	*manager,
	VenturePluginManifest	*manifest,
	JsonObject		*entry,
	gpointer		 user_data
){
	(void)manifest;
	(void)user_data;

	venture_data_source_provider_registry_remove(
		venture_context_get_data_source_providers(venture_plugin_manager_get_context(manager)),
		feed_setting_string(entry, "name"));
}

void
venture_feeds_register_provides(VenturePluginProvidesRegistry *registry)
{
	g_autoptr(GError) error = NULL;

	if (!venture_plugin_provides_registry_add_full(registry, "data_source_provider",
		"A source of market data for feeds, run as the plugin's program",
		feed_validate_provider, feed_accept_provider, feed_remove_provider,
		NULL, NULL, &error))
		g_debug("feeds: %s", error->message);
}
