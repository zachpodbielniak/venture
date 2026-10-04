/*
 * venture-feed-batch.c - What one fetch of a market data source produced
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Plain data. Every string is copied in, every price is an integer, and
 * nothing here reads the database, the configuration or a store, so a
 * provider may build a batch on any thread. The checks are the ones that
 * can be made without the store -- lengths, a quantity of at least one, a
 * listing whose venue has a snapshot -- and a refusal is per item, so a
 * provider can count it and carry on.
 */

#include "venture.h"
#include "feeds/venture-feeds-private.h"

#include <string.h>

G_DEFINE_BOXED_TYPE(VentureFeedBatch, venture_feed_batch,
                    venture_feed_batch_ref, venture_feed_batch_unref)

/* The most log lines one batch turns into notes: a chatty producer must not
 * turn a run record into a log file. */
#define FEED_BATCH_MAX_LOG_NOTES (20)

/* The most notes of any kind a batch keeps. */
#define FEED_BATCH_MAX_NOTES (64)

/* --- Freeing ---------------------------------------------------------------- */

static void
feed_batch_venue_free(gpointer data)
{
	FeedBatchVenue *venue = data;

	g_free(venue->key);
	g_free(venue->name);
	g_free(venue->kind);
	g_free(venue->group_key);
	g_free(venue->currency);
	g_free(venue->attrs_json);
	g_free(venue);
}

static void
feed_batch_instrument_free(gpointer data)
{
	FeedBatchInstrument *instrument = data;

	g_free(instrument->key);
	g_free(instrument->name);
	g_free(instrument->kind);
	g_free(instrument->category);
	g_free(instrument->parent_key);
	g_free(instrument->attrs_json);
	g_free(instrument);
}

static void
feed_batch_listing_clear(gpointer data)
{
	FeedBatchListing *listing = data;

	g_free(listing->instrument_key);
}

static void
feed_batch_stats_clear(gpointer data)
{
	FeedBatchStats *stats = data;

	g_free(stats->instrument_key);
}

static void
feed_batch_snapshot_free(gpointer data)
{
	FeedBatchSnapshot *snapshot = data;

	g_free(snapshot->venue_key);
	g_free(snapshot->currency);
	g_array_unref(snapshot->listings);
	g_array_unref(snapshot->stats);
	g_free(snapshot);
}

static void
feed_batch_quote_free(gpointer data)
{
	FeedBatchQuote *quote = data;

	g_free(quote->venue_key);
	g_free(quote->instrument_key);
	g_free(quote->currency);
	g_free(quote);
}

static void
feed_batch_entry_free(gpointer data)
{
	FeedBatchEntry *entry = data;

	g_free(entry->key);
	g_free(entry->title);
	g_free(entry->url);
	g_free(entry->summary);
	g_free(entry->venue_key);
	g_free(entry->instrument_key);
	g_free(entry);
}

static void
feed_batch_record_free(gpointer data)
{
	FeedBatchRecord *record = data;

	g_free(record->record_type);
	g_free(record->fields_json);
	g_strfreev(record->match);
	g_free(record);
}

void
venture_feeds_record_free(gpointer data)
{
	feed_batch_record_free(data);
}

VentureFeedBatch *
venture_feed_batch_new(void)
{
	VentureFeedBatch *self;

	self = g_new0(VentureFeedBatch, 1);
	g_atomic_ref_count_init(&self->ref);
	self->venues = g_ptr_array_new_with_free_func(feed_batch_venue_free);
	self->instruments = g_ptr_array_new_with_free_func(feed_batch_instrument_free);
	self->snapshots = g_ptr_array_new_with_free_func(feed_batch_snapshot_free);
	self->open_snapshots = g_hash_table_new(g_str_hash, g_str_equal);
	self->quotes = g_ptr_array_new_with_free_func(feed_batch_quote_free);
	self->entries = g_ptr_array_new_with_free_func(feed_batch_entry_free);
	self->records = g_ptr_array_new_with_free_func(feed_batch_record_free);
	self->notes = g_ptr_array_new_with_free_func(g_free);
	self->remote_used = -1;
	self->remote_remaining = -1;

	return self;
}

VentureFeedBatch *
venture_feed_batch_ref(VentureFeedBatch *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	g_atomic_ref_count_inc(&self->ref);

	return self;
}

void
venture_feed_batch_unref(VentureFeedBatch *self)
{
	if (NULL == self)
		return;

	if (!g_atomic_ref_count_dec(&self->ref))
		return;

	g_hash_table_unref(self->open_snapshots);
	g_ptr_array_unref(self->venues);
	g_ptr_array_unref(self->instruments);
	g_ptr_array_unref(self->snapshots);
	g_ptr_array_unref(self->quotes);
	g_ptr_array_unref(self->entries);
	g_ptr_array_unref(self->records);
	g_ptr_array_unref(self->notes);
	g_free(self->cursor);
	g_free(self->error);
	g_free(self);
}

/* --- Checks ------------------------------------------------------------------ */

/*
 * A key the store will take: present, not overlong, and valid UTF-8. The
 * store checks again; refusing here names the item instead of failing a
 * whole commit.
 */
static gboolean
feed_batch_check_key(
	const gchar	 *what,
	const gchar	 *key,
	gboolean	  required,
	GError		**error
){
	if (NULL == key || '\0' == key[0])
	{
		if (!required)
			return TRUE;

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A %s needs a key", what);
		return FALSE;
	}

	if ((strlen(key) > VENTURE_SERIES_MAX_KEY_LENGTH) ||
	    !g_utf8_validate(key, -1, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A %s key must be UTF-8 of at most %d bytes", what,
		            VENTURE_SERIES_MAX_KEY_LENGTH);
		return FALSE;
	}

	return TRUE;
}

/* Free text: a name, a title. Overlong text is cut at a character, not
 * refused -- a long name is still a name. */
static gchar *
feed_batch_text(
	const gchar	*text,
	gsize		 limit
){
	const gchar *end;

	if (NULL == text)
		return NULL;

	if (!g_utf8_validate(text, -1, NULL))
		return g_utf8_make_valid(text, -1);

	if (strlen(text) <= limit)
		return g_strdup(text);

	end = g_utf8_find_prev_char(text, text + limit);

	return g_strndup(text, (NULL != end) ? (gsize)(end - text) : 0);
}

static gboolean
feed_batch_check_currency(
	const gchar	 *currency,
	GError		**error
){
	if ((NULL != currency) && !venture_currency_is_valid(currency))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Not a currency code");
		return FALSE;
	}

	return TRUE;
}

static gboolean
feed_batch_check_attrs(
	const gchar	 *attrs_json,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;

	if (NULL == attrs_json)
		return TRUE;

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, attrs_json, -1, NULL) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Attributes must be a JSON object");
		return FALSE;
	}

	return TRUE;
}

/* --- Adding ------------------------------------------------------------------ */

gboolean
venture_feed_batch_add_venue(
	VentureFeedBatch	 *self,
	const gchar		 *key,
	const gchar		 *name,
	const gchar		 *kind,
	const gchar		 *group_key,
	const gchar		 *currency,
	const gchar		 *attrs_json,
	GError			**error
){
	FeedBatchVenue *venue;

	g_return_val_if_fail(NULL != self, FALSE);

	if (!feed_batch_check_key("venue", key, TRUE, error) ||
	    !feed_batch_check_key("venue group", group_key, FALSE, error) ||
	    !feed_batch_check_currency(currency, error) ||
	    !feed_batch_check_attrs(attrs_json, error))
		return FALSE;

	venue = g_new0(FeedBatchVenue, 1);
	venue->key = g_strdup(key);
	venue->name = feed_batch_text(name, VENTURE_SERIES_MAX_KEY_LENGTH);
	venue->kind = feed_batch_text(kind, 64);
	venue->group_key = g_strdup(group_key);
	venue->currency = (NULL != currency) ? g_ascii_strup(currency, -1) : NULL;
	venue->attrs_json = g_strdup(attrs_json);
	g_ptr_array_add(self->venues, venue);

	return TRUE;
}

gboolean
venture_feed_batch_add_instrument(
	VentureFeedBatch	 *self,
	const gchar		 *key,
	const gchar		 *name,
	const gchar		 *kind,
	const gchar		 *category,
	const gchar		 *parent_key,
	const gchar		 *attrs_json,
	GError			**error
){
	FeedBatchInstrument *instrument;

	g_return_val_if_fail(NULL != self, FALSE);

	if (!feed_batch_check_key("instrument", key, TRUE, error) ||
	    !feed_batch_check_key("parent instrument", parent_key, FALSE, error) ||
	    !feed_batch_check_attrs(attrs_json, error))
		return FALSE;

	instrument = g_new0(FeedBatchInstrument, 1);
	instrument->key = g_strdup(key);
	instrument->name = feed_batch_text(name, VENTURE_SERIES_MAX_KEY_LENGTH);
	instrument->kind = feed_batch_text(kind, 64);
	instrument->category = feed_batch_text(category, VENTURE_SERIES_MAX_KEY_LENGTH);
	instrument->parent_key = g_strdup(parent_key);
	instrument->attrs_json = g_strdup(attrs_json);
	g_ptr_array_add(self->instruments, instrument);

	return TRUE;
}

gboolean
venture_feed_batch_begin_snapshot(
	VentureFeedBatch	 *self,
	const gchar		 *venue_key,
	const gchar		 *currency,
	gint64			  taken_at,
	gboolean		  complete,
	GError			**error
){
	FeedBatchSnapshot *snapshot;

	g_return_val_if_fail(NULL != self, FALSE);

	if (!feed_batch_check_key("snapshot's venue", venue_key, TRUE, error) ||
	    !feed_batch_check_currency(currency, error))
		return FALSE;

	if (taken_at <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A snapshot needs the time it was taken");
		return FALSE;
	}

	snapshot = g_new0(FeedBatchSnapshot, 1);
	snapshot->venue_key = g_strdup(venue_key);
	snapshot->currency = (NULL != currency) ? g_ascii_strup(currency, -1) : NULL;
	snapshot->taken_at = taken_at;
	snapshot->complete = complete;
	snapshot->listings = g_array_new(FALSE, TRUE, sizeof(FeedBatchListing));
	g_array_set_clear_func(snapshot->listings, feed_batch_listing_clear);
	snapshot->stats = g_array_new(FALSE, TRUE, sizeof(FeedBatchStats));
	g_array_set_clear_func(snapshot->stats, feed_batch_stats_clear);

	g_ptr_array_add(self->snapshots, snapshot);

	/* The newest snapshot of a venue is the one its listings join. */
	g_hash_table_replace(self->open_snapshots, snapshot->venue_key, snapshot);

	return TRUE;
}

static FeedBatchSnapshot *
feed_batch_open_snapshot(
	VentureFeedBatch	 *self,
	const gchar		 *venue_key,
	GError			**error
){
	FeedBatchSnapshot *snapshot;

	snapshot = (NULL != venue_key)
		? g_hash_table_lookup(self->open_snapshots, venue_key) : NULL;

	if (NULL == snapshot)
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A listing or figure names a venue with no snapshot begun");

	return snapshot;
}

gboolean
venture_feed_batch_add_listing(
	VentureFeedBatch	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	guint64			  listing_id,
	gint64			  unit_price,
	gint64			  quantity,
	gboolean		  buy,
	gint64			  expires_in_min,
	GError			**error
){
	FeedBatchSnapshot *snapshot;
	FeedBatchListing listing;

	g_return_val_if_fail(NULL != self, FALSE);

	snapshot = feed_batch_open_snapshot(self, venue_key, error);

	if (NULL == snapshot)
		return FALSE;

	if (!feed_batch_check_key("listing's instrument", instrument_key, TRUE, error))
		return FALSE;

	if ((quantity < 1) || (unit_price < 0) || (VENTURE_SERIES_NONE == unit_price))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A listing needs a quantity of at least one and a price of zero or more");
		return FALSE;
	}

	listing.instrument_key = g_strdup(instrument_key);
	listing.listing_id = listing_id;
	listing.unit_price = unit_price;
	listing.quantity = quantity;
	listing.side = buy ? VENTURE_SERIES_SIDE_BUY : VENTURE_SERIES_SIDE_SELL;
	listing.expires_in_min = (expires_in_min < 0) ? -1 : expires_in_min;
	g_array_append_val(snapshot->listings, listing);

	return TRUE;
}

gboolean
venture_feed_batch_add_stats(
	VentureFeedBatch	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	gint64			  min_price,
	gint64			  market_value,
	gint64			  mean,
	gint64			  median,
	gint64			  sale_avg,
	gint64			  quantity,
	gint64			  listings,
	gint64			  sold,
	GError			**error
){
	FeedBatchSnapshot *snapshot;
	FeedBatchStats stats;

	g_return_val_if_fail(NULL != self, FALSE);

	snapshot = feed_batch_open_snapshot(self, venue_key, error);

	if (NULL == snapshot)
		return FALSE;

	if (!feed_batch_check_key("figure's instrument", instrument_key, TRUE, error))
		return FALSE;

	if ((VENTURE_SERIES_NONE == min_price) && (VENTURE_SERIES_NONE == market_value) &&
	    (VENTURE_SERIES_NONE == mean) && (VENTURE_SERIES_NONE == median) &&
	    (VENTURE_SERIES_NONE == sale_avg) && (VENTURE_SERIES_NONE == quantity) &&
	    (VENTURE_SERIES_NONE == listings) && (VENTURE_SERIES_NONE == sold))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Figures need at least one figure");
		return FALSE;
	}

	stats.instrument_key = g_strdup(instrument_key);
	stats.min_price = min_price;
	stats.market_value = market_value;
	stats.mean = mean;
	stats.median = median;
	stats.sale_avg = sale_avg;
	stats.quantity = quantity;
	stats.listings = listings;
	stats.sold = sold;
	g_array_append_val(snapshot->stats, stats);

	return TRUE;
}

gboolean
venture_feed_batch_add_quote(
	VentureFeedBatch	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	VentureSeriesQuoteSide	  side,
	gint64			  value,
	const gchar		 *currency,
	gint64			  liquidity,
	gint64			  taken_at,
	GError			**error
){
	FeedBatchQuote *quote;

	g_return_val_if_fail(NULL != self, FALSE);

	if (!feed_batch_check_key("quote's venue", venue_key, TRUE, error) ||
	    !feed_batch_check_key("quote's instrument", instrument_key, TRUE, error) ||
	    !feed_batch_check_currency(currency, error))
		return FALSE;

	if ((value < 0) || (VENTURE_SERIES_NONE == value) || (taken_at <= 0) ||
	    ((NULL == currency) && (value <= VENTURE_SERIES_ODDS_SCALE)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A quote needs a value (odds above 1) and a time");
		return FALSE;
	}

	quote = g_new0(FeedBatchQuote, 1);
	quote->venue_key = g_strdup(venue_key);
	quote->instrument_key = g_strdup(instrument_key);
	quote->side = side;
	quote->value = value;
	quote->currency = (NULL != currency) ? g_ascii_strup(currency, -1) : NULL;
	quote->liquidity = liquidity;
	quote->taken_at = taken_at;
	g_ptr_array_add(self->quotes, quote);

	return TRUE;
}

gboolean
venture_feed_batch_add_entry(
	VentureFeedBatch	 *self,
	const gchar		 *key,
	const gchar		 *title,
	const gchar		 *url,
	const gchar		 *summary,
	gint64			  published_at,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	GError			**error
){
	FeedBatchEntry *entry;

	g_return_val_if_fail(NULL != self, FALSE);

	if (!feed_batch_check_key("entry", key, TRUE, error) ||
	    !feed_batch_check_key("entry's venue", venue_key, FALSE, error) ||
	    !feed_batch_check_key("entry's instrument", instrument_key, FALSE, error))
		return FALSE;

	if (venture_string_is_empty(title))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An entry needs a title");
		return FALSE;
	}

	/* Only a web address: a javascript: link from a feed would otherwise
	 * reach a page that renders it. */
	if ((NULL != url) && !g_str_has_prefix(url, "https://") &&
	    !g_str_has_prefix(url, "http://"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An entry's address must be http or https");
		return FALSE;
	}

	entry = g_new0(FeedBatchEntry, 1);
	entry->key = g_strdup(key);
	entry->title = feed_batch_text(title, VENTURE_JSONL_MAX_KEY_LENGTH);
	entry->url = feed_batch_text(url, VENTURE_JSONL_MAX_TEXT_LENGTH);
	entry->summary = feed_batch_text(summary, VENTURE_JSONL_MAX_TEXT_LENGTH);
	entry->published_at = published_at;
	entry->venue_key = g_strdup(venue_key);
	entry->instrument_key = g_strdup(instrument_key);
	g_ptr_array_add(self->entries, entry);

	return TRUE;
}

gboolean
venture_feed_batch_add_record(
	VentureFeedBatch	 *self,
	const gchar		 *record_type,
	JsonObject		 *fields,
	const gchar *const	 *match,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	FeedBatchRecord *record;

	g_return_val_if_fail(NULL != self, FALSE);

	if (venture_string_is_empty(record_type) || (strlen(record_type) > 64) ||
	    !g_regex_match_simple("^[a-z][a-z0-9_]*$", record_type, 0, 0) ||
	    (NULL == fields))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A record needs a lower-case type name and its fields");
		return FALSE;
	}

	if (self->records->len >= VENTURE_FEED_BATCH_MAX_RECORDS)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A batch carries at most %d records", VENTURE_FEED_BATCH_MAX_RECORDS);
		return FALSE;
	}

	/* Serialised now: the batch crosses threads, a JsonObject's references
	 * are not safe to share, and the main thread parses its own copy. */
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, fields);

	record = g_new0(FeedBatchRecord, 1);
	record->record_type = g_strdup(record_type);
	record->fields_json = json_to_string(node, FALSE);
	record->match = (NULL != match) ? g_strdupv((gchar **)match) : NULL;
	g_ptr_array_add(self->records, record);

	return TRUE;
}

void
venture_feed_batch_set_cursor(
	VentureFeedBatch	*self,
	const gchar		*cursor
){
	g_return_if_fail(NULL != self);

	g_free(self->cursor);
	self->cursor = feed_batch_text(cursor, VENTURE_JSONL_MAX_TEXT_LENGTH);
}

const gchar *
venture_feed_batch_get_cursor(VentureFeedBatch *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->cursor;
}

void
venture_feed_batch_set_not_modified(
	VentureFeedBatch	*self,
	gboolean		 not_modified
){
	g_return_if_fail(NULL != self);

	self->not_modified = not_modified;
}

gboolean
venture_feed_batch_get_not_modified(VentureFeedBatch *self)
{
	g_return_val_if_fail(NULL != self, FALSE);

	return self->not_modified;
}

void
venture_feed_batch_set_error(
	VentureFeedBatch	*self,
	const gchar		*message,
	gint64			 retry_after
){
	g_return_if_fail(NULL != self);

	g_free(self->error);
	self->error = feed_batch_text(message, VENTURE_JSONL_MAX_TEXT_LENGTH);
	self->retry_after = MAX(retry_after, 0);
}

const gchar *
venture_feed_batch_get_error(
	VentureFeedBatch	*self,
	gint64			*out_retry_after
){
	g_return_val_if_fail(NULL != self, NULL);

	if (NULL != out_retry_after)
		*out_retry_after = self->retry_after;

	return self->error;
}

void
venture_feed_batch_add_note(
	VentureFeedBatch	*self,
	const gchar		*note
){
	g_return_if_fail(NULL != self);

	if (venture_string_is_empty(note) || (self->notes->len >= FEED_BATCH_MAX_NOTES))
		return;

	g_ptr_array_add(self->notes, feed_batch_text(note, 512));
}

void
venture_feed_batch_add_refused(
	VentureFeedBatch	*self,
	gint64			 count
){
	g_return_if_fail(NULL != self);

	if (count > 0)
		self->refused += count;
}

void
venture_feed_batch_set_remote_quota(
	VentureFeedBatch	*self,
	gint64			 used,
	gint64			 remaining
){
	g_return_if_fail(NULL != self);

	/* Both or neither: half an account is a number nobody can read. */
	if ((used < 0) || (remaining < 0))
		return;

	self->remote_used = used;
	self->remote_remaining = remaining;
}

gboolean
venture_feed_batch_get_remote_quota(
	VentureFeedBatch	*self,
	gint64			*out_used,
	gint64			*out_remaining
){
	g_return_val_if_fail(NULL != self, FALSE);

	if (self->remote_used < 0)
		return FALSE;

	if (NULL != out_used)
		*out_used = self->remote_used;

	if (NULL != out_remaining)
		*out_remaining = self->remote_remaining;

	return TRUE;
}

gint64
venture_feed_batch_count_items(VentureFeedBatch *self)
{
	gint64 total;
	guint i;

	g_return_val_if_fail(NULL != self, 0);

	total = (gint64)self->venues->len + (gint64)self->instruments->len +
	        (gint64)self->quotes->len + (gint64)self->entries->len +
	        (gint64)self->records->len;

	for (i = 0; i < self->snapshots->len; i++)
	{
		FeedBatchSnapshot *snapshot = g_ptr_array_index(self->snapshots, i);

		total += (gint64)snapshot->listings->len + (gint64)snapshot->stats->len;
	}

	return total;
}

gchar *
venture_feed_batch_describe(VentureFeedBatch *self)
{
	gint64 listings = 0;
	gint64 figures = 0;
	guint i;

	g_return_val_if_fail(NULL != self, NULL);

	if (self->not_modified)
		return g_strdup("Not modified since the last fetch");

	for (i = 0; i < self->snapshots->len; i++)
	{
		FeedBatchSnapshot *snapshot = g_ptr_array_index(self->snapshots, i);

		listings += snapshot->listings->len;
		figures += snapshot->stats->len;
	}

	return g_strdup_printf("%u venues, %u instruments, %u snapshots, %"
	                       G_GINT64_FORMAT " listings, %" G_GINT64_FORMAT
	                       " figures, %u quotes, %u entries, %u records, %"
	                       G_GINT64_FORMAT " refused",
	                       self->venues->len, self->instruments->len,
	                       self->snapshots->len, listings, figures,
	                       self->quotes->len, self->entries->len,
	                       self->records->len, self->refused);
}

/* --- Exact decimals ---------------------------------------------------------- */

/*
 * Reads "[-]digits[.digits]" into an integer scaled by 10^places. More
 * fraction digits than @places are refused when @exact, else rounded half
 * to even on the first dropped digit and anything after it. Nothing here
 * goes near a double.
 */
static gboolean
feed_decimal_scale(
	const gchar	*text,
	guint		 places,
	gboolean	 exact,
	gint64		*out
){
	const gchar *p;
	gboolean negative;
	gint64 value;
	guint fraction;
	gint dropped_first;
	gboolean dropped_rest;

	if ((NULL == text) || ('\0' == text[0]) || (strlen(text) > VENTURE_JSONL_MAX_DECIMAL_LENGTH))
		return FALSE;

	p = text;
	negative = ('-' == *p);

	if (negative)
		p++;

	if (!g_ascii_isdigit(*p))
		return FALSE;

	value = 0;
	fraction = 0;
	dropped_first = -1;
	dropped_rest = FALSE;

	for (; g_ascii_isdigit(*p); p++)
	{
		if (!venture_series_math_mul(value, 10, &value) ||
		    !venture_series_math_add(value, (gint64)(*p - '0'), &value))
			return FALSE;
	}

	if ('.' == *p)
	{
		p++;

		if (!g_ascii_isdigit(*p))
			return FALSE;

		for (; g_ascii_isdigit(*p); p++)
		{
			if (fraction < places)
			{
				if (!venture_series_math_mul(value, 10, &value) ||
				    !venture_series_math_add(value, (gint64)(*p - '0'), &value))
					return FALSE;
				fraction++;
			}
			else if (dropped_first < 0)
				dropped_first = *p - '0';
			else if ('0' != *p)
				dropped_rest = TRUE;
		}
	}

	if ('\0' != *p)
		return FALSE;

	if ((dropped_first > 0) || dropped_rest)
	{
		if (exact)
			return FALSE;

		/* Half to even: above half rounds up, exactly half rounds to
		 * the even neighbour. */
		if ((dropped_first > 5) || ((5 == dropped_first) && dropped_rest) ||
		    ((5 == dropped_first) && !dropped_rest && (0 != (value % 2))))
		{
			if (!venture_series_math_add(value, 1, &value))
				return FALSE;
		}
	}

	for (; fraction < places; fraction++)
	{
		if (!venture_series_math_mul(value, 10, &value))
			return FALSE;
	}

	*out = negative ? -value : value;

	return TRUE;
}

gboolean
venture_feed_decimal_to_minor(
	const gchar	 *text,
	const gchar	 *currency,
	gint64		 *out_minor,
	GError		**error
){
	guint8 exponent;

	g_return_val_if_fail(NULL != out_minor, FALSE);

	if ((NULL == currency) || !venture_currency_is_valid(currency))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A price needs a currency");
		return FALSE;
	}

	exponent = venture_currency_get_exponent(currency);

	if (!feed_decimal_scale(text, exponent, TRUE, out_minor))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A price must be a decimal with at most %u places for %s",
		            (guint)exponent, currency);
		return FALSE;
	}

	return TRUE;
}

gboolean
venture_feed_decimal_to_odds(
	const gchar	 *text,
	const gchar	 *format,
	gint64		 *out_scaled,
	GError		**error
){
	gint64 scaled;

	g_return_val_if_fail(NULL != out_scaled, FALSE);

	if ((NULL == text) || (strlen(text) > VENTURE_JSONL_MAX_DECIMAL_LENGTH))
		goto refuse;

	if ((NULL == format) || (0 == g_strcmp0(format, "decimal")))
	{
		if (!feed_decimal_scale(text, 6, FALSE, &scaled))
			goto refuse;
	}
	else if (0 == g_strcmp0(format, "american"))
	{
		const gchar *digits;
		gint64 line;
		gboolean minus;

		minus = ('-' == text[0]);
		digits = (('+' == text[0]) || minus) ? text + 1 : text;

		if (('\0' == digits[0]) || (strlen(digits) > 12) ||
		    (strspn(digits, "0123456789") != strlen(digits)))
			goto refuse;

		line = g_ascii_strtoll(digits, NULL, 10);

		if (line < 100)
			goto refuse;

		/* +150 pays 150 on 100: 2.5. -200 pays 100 on 200: 1.5. */
		scaled = VENTURE_SERIES_ODDS_SCALE + (minus
			? venture_series_math_div_round(100 * (gint64)VENTURE_SERIES_ODDS_SCALE, line)
			: venture_series_math_div_round(line * (gint64)VENTURE_SERIES_ODDS_SCALE, 100));
	}
	else if (0 == g_strcmp0(format, "fractional"))
	{
		g_auto(GStrv) parts = NULL;
		gint64 numerator;
		gint64 denominator;

		parts = g_strsplit(text, "/", -1);

		if ((2 != g_strv_length(parts)) || ('\0' == parts[0][0]) || ('\0' == parts[1][0]) ||
		    (strlen(parts[0]) > 12) || (strlen(parts[1]) > 12) ||
		    (strspn(parts[0], "0123456789") != strlen(parts[0])) ||
		    (strspn(parts[1], "0123456789") != strlen(parts[1])))
			goto refuse;

		numerator = g_ascii_strtoll(parts[0], NULL, 10);
		denominator = g_ascii_strtoll(parts[1], NULL, 10);

		if ((numerator <= 0) || (denominator <= 0))
			goto refuse;

		scaled = VENTURE_SERIES_ODDS_SCALE +
		         venture_series_math_div_round(numerator * (gint64)VENTURE_SERIES_ODDS_SCALE,
		                                       denominator);
	}
	else
		goto refuse;

	if (scaled <= VENTURE_SERIES_ODDS_SCALE)
		goto refuse;

	*out_scaled = scaled;

	return TRUE;

refuse:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	                    "Odds must be decimal above 1, American (+150, -200) or fractional (5/2)");
	return FALSE;
}

/* --- The JSON-lines vocabulary ----------------------------------------------- */

static const gchar *
jsonl_string(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = json_object_get_member(object, member);

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)))
		return NULL;

	return json_node_get_string(node);
}

static gint64
jsonl_int(
	JsonObject	*object,
	const gchar	*member,
	gint64		 fallback
){
	JsonNode *node;

	node = json_object_get_member(object, member);

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_INT64 != json_node_get_value_type(node)))
		return fallback;

	return json_node_get_int(node);
}

static gboolean
jsonl_bool(
	JsonObject	*object,
	const gchar	*member,
	gboolean	 fallback
){
	JsonNode *node;

	node = json_object_get_member(object, member);

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_BOOLEAN != json_node_get_value_type(node)))
		return fallback;

	return json_node_get_boolean(node);
}

static gint64
jsonl_time(
	JsonObject	*object,
	const gchar	*member,
	gint64		 fallback
){
	g_autoptr(GDateTime) when = NULL;
	const gchar *text;

	text = jsonl_string(object, member);

	if (NULL == text)
		return fallback;

	when = g_date_time_new_from_iso8601(text, NULL);

	return (NULL != when) ? g_date_time_to_unix(when) : fallback;
}

static gchar *
jsonl_attrs(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = json_object_get_member(object, member);

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return NULL;

	return json_to_string(node, FALSE);
}

/* A decimal figure, or NONE when absent; FALSE when present and unusable. */
static gboolean
jsonl_price(
	JsonObject	*object,
	const gchar	*member,
	const gchar	*currency,
	gint64		*out
){
	const gchar *text;

	text = jsonl_string(object, member);

	if (NULL == text)
	{
		*out = VENTURE_SERIES_NONE;
		return TRUE;
	}

	return venture_feed_decimal_to_minor(text, currency, out, NULL);
}

/*
 * The currency of a price: the message's own, then its venue's from this
 * stream, then the source's. Without one the price cannot be turned into
 * minor units at all, so the item is refused rather than guessed.
 */
static const gchar *
jsonl_currency_for(
	JsonObject	*object,
	GHashTable	*venue_currency,
	const gchar	*default_currency
){
	const gchar *currency;
	const gchar *venue;

	currency = jsonl_string(object, "currency");

	if (NULL != currency)
		return currency;

	venue = jsonl_string(object, "venue");

	if ((NULL != venue) && (NULL != (currency = g_hash_table_lookup(venue_currency, venue))))
		return currency;

	return default_currency;
}

/*
 * The snapshot a listing or figure joins: the open one of its venue, or a
 * new incomplete one at the item's own time. Incomplete because nothing
 * said it lists everything, and a complete one would zero every instrument
 * the stream happened not to mention.
 */
static FeedBatchSnapshot *
jsonl_snapshot_for(
	VentureFeedBatch	*self,
	JsonObject		*object,
	const gchar		*currency,
	gint64			 fetched_at
){
	FeedBatchSnapshot *snapshot;
	const gchar *venue;

	venue = jsonl_string(object, "venue");
	snapshot = g_hash_table_lookup(self->open_snapshots, venue);

	if (NULL != snapshot)
		return snapshot;

	if (!venture_feed_batch_begin_snapshot(self, venue, currency,
	                                       jsonl_time(object, "taken_at", fetched_at),
	                                       FALSE, NULL))
		return NULL;

	return g_hash_table_lookup(self->open_snapshots, venue);
}

gboolean
venture_feed_batch_add_jsonl(
	VentureFeedBatch	 *self,
	GPtrArray		 *messages,
	const gchar		 *default_currency,
	gint64			  fetched_at,
	GError			**error
){
	g_autoptr(GHashTable) venue_currency = NULL;
	guint logs;
	guint i;

	g_return_val_if_fail(NULL != self, FALSE);
	g_return_val_if_fail(NULL != messages, FALSE);

	(void)error;

	/* venue key -> currency, borrowed from the messages. */
	venue_currency = g_hash_table_new(g_str_hash, g_str_equal);
	logs = 0;

	for (i = 0; i < messages->len; i++)
	{
		VentureJsonlMessage *message;
		JsonObject *object;
		g_autoptr(GError) local_error = NULL;
		gboolean ok;

		message = g_ptr_array_index(messages, i);
		object = venture_jsonl_message_get_object(message);
		ok = TRUE;

		switch (venture_jsonl_message_get_kind(message))
		{
		case VENTURE_JSONL_MESSAGE_VENUE:
		{
			g_autofree gchar *attrs = jsonl_attrs(object, "attrs");
			const gchar *currency = jsonl_string(object, "currency");

			ok = venture_feed_batch_add_venue(self, jsonl_string(object, "key"),
			                                  jsonl_string(object, "name"),
			                                  jsonl_string(object, "kind"),
			                                  jsonl_string(object, "group"),
			                                  currency, attrs, &local_error);

			if (ok && (NULL != currency))
				g_hash_table_replace(venue_currency,
				                     (gpointer)jsonl_string(object, "key"),
				                     (gpointer)currency);
			break;
		}

		case VENTURE_JSONL_MESSAGE_INSTRUMENT:
		{
			g_autofree gchar *attrs = jsonl_attrs(object, "attrs");

			ok = venture_feed_batch_add_instrument(self, jsonl_string(object, "key"),
			                                       jsonl_string(object, "name"),
			                                       jsonl_string(object, "kind"),
			                                       jsonl_string(object, "category"),
			                                       jsonl_string(object, "parent"),
			                                       attrs, &local_error);
			break;
		}

		case VENTURE_JSONL_MESSAGE_SNAPSHOT:
		{
			const gchar *venue = jsonl_string(object, "venue");

			ok = venture_feed_batch_begin_snapshot(self, venue,
				jsonl_currency_for(object, venue_currency, default_currency),
				jsonl_time(object, "taken_at", fetched_at),
				jsonl_bool(object, "complete", FALSE), &local_error);
			break;
		}

		case VENTURE_JSONL_MESSAGE_LISTING:
		{
			FeedBatchSnapshot *snapshot;
			const gchar *currency;
			const gchar *id;
			gint64 price;

			currency = jsonl_currency_for(object, venue_currency, default_currency);
			snapshot = jsonl_snapshot_for(self, object, currency, fetched_at);

			/* One snapshot, one currency: a listing in another is a
			 * different market and is refused, not converted. */
			if ((NULL == snapshot) ||
			    ((NULL != snapshot->currency) && (NULL != currency) &&
			     (0 != g_ascii_strcasecmp(snapshot->currency, currency))) ||
			    !jsonl_price(object, "price", (NULL != snapshot->currency) ? snapshot->currency : currency, &price) ||
			    (VENTURE_SERIES_NONE == price))
			{
				ok = FALSE;
				break;
			}

			id = jsonl_string(object, "id");
			ok = venture_feed_batch_add_listing(self, snapshot->venue_key,
				jsonl_string(object, "instrument"),
				(NULL != id) ? venture_series_listing_id_from_string(id) : 0,
				price, jsonl_int(object, "quantity", 1),
				0 == g_strcmp0(jsonl_string(object, "side"), "buy"),
				jsonl_int(object, "expires_in_min", -1), &local_error);
			break;
		}

		case VENTURE_JSONL_MESSAGE_STAT:
		{
			FeedBatchSnapshot *snapshot;
			const gchar *currency;
			gint64 figures[5];
			static const gchar *const prices[] = {
				"min", "market", "mean", "median", "sale_avg"
			};
			guint j;

			currency = jsonl_currency_for(object, venue_currency, default_currency);
			snapshot = jsonl_snapshot_for(self, object, currency, fetched_at);

			if (NULL == snapshot)
			{
				ok = FALSE;
				break;
			}

			currency = (NULL != snapshot->currency) ? snapshot->currency : currency;

			for (j = 0; ok && (j < G_N_ELEMENTS(prices)); j++)
				ok = jsonl_price(object, prices[j], currency, &figures[j]);

			if (!ok)
				break;

			ok = venture_feed_batch_add_stats(self, snapshot->venue_key,
				jsonl_string(object, "instrument"), figures[0], figures[1],
				figures[2], figures[3], figures[4],
				jsonl_int(object, "quantity", VENTURE_SERIES_NONE),
				jsonl_int(object, "listings", VENTURE_SERIES_NONE),
				jsonl_int(object, "sold", VENTURE_SERIES_NONE), &local_error);
			break;
		}

		case VENTURE_JSONL_MESSAGE_QUOTE:
		{
			const gchar *odds;
			const gchar *side_name;
			const gchar *currency;
			VentureSeriesQuoteSide side;
			gint64 value;
			gint64 liquidity;

			odds = jsonl_string(object, "odds");
			side_name = jsonl_string(object, "side");
			currency = jsonl_currency_for(object, venue_currency, default_currency);

			if (0 == g_strcmp0(side_name, "lay"))
				side = VENTURE_SERIES_QUOTE_LAY;
			else if (0 == g_strcmp0(side_name, "bid"))
				side = VENTURE_SERIES_QUOTE_BID;
			else if (0 == g_strcmp0(side_name, "ask"))
				side = VENTURE_SERIES_QUOTE_ASK;
			else if (0 == g_strcmp0(side_name, "back"))
				side = VENTURE_SERIES_QUOTE_BACK;
			else
				side = (NULL != odds) ? VENTURE_SERIES_QUOTE_BACK : VENTURE_SERIES_QUOTE_ASK;

			if (NULL != odds)
				ok = venture_feed_decimal_to_odds(odds, jsonl_string(object, "format"),
				                                  &value, &local_error);
			else
				ok = jsonl_price(object, "price", currency, &value) &&
				     (VENTURE_SERIES_NONE != value);

			if (ok)
				ok = jsonl_price(object, "liquidity", currency, &liquidity);

			if (ok)
				ok = venture_feed_batch_add_quote(self, jsonl_string(object, "venue"),
					jsonl_string(object, "instrument"), side, value,
					(NULL != odds) ? NULL : currency, liquidity,
					jsonl_time(object, "taken_at", fetched_at), &local_error);
			break;
		}

		case VENTURE_JSONL_MESSAGE_ENTRY:
			ok = venture_feed_batch_add_entry(self, jsonl_string(object, "key"),
				jsonl_string(object, "title"), jsonl_string(object, "url"),
				jsonl_string(object, "summary"),
				jsonl_time(object, "published_at", VENTURE_SERIES_NONE),
				jsonl_string(object, "venue"), jsonl_string(object, "instrument"),
				&local_error);
			break;

		case VENTURE_JSONL_MESSAGE_RECORD:
		{
			g_autoptr(GPtrArray) match = NULL;
			JsonNode *node;

			node = json_object_get_member(object, "match");

			if ((NULL != node) && JSON_NODE_HOLDS_ARRAY(node))
			{
				JsonArray *array = json_node_get_array(node);
				guint j;

				match = g_ptr_array_new();

				for (j = 0; j < json_array_get_length(array); j++)
				{
					JsonNode *element = json_array_get_element(array, j);

					if (JSON_NODE_HOLDS_VALUE(element) &&
					    (G_TYPE_STRING == json_node_get_value_type(element)))
						g_ptr_array_add(match, (gpointer)json_node_get_string(element));
				}

				g_ptr_array_add(match, NULL);
			}

			ok = venture_feed_batch_add_record(self, jsonl_string(object, "record_type"),
				json_object_get_object_member(object, "fields"),
				(NULL != match) ? (const gchar *const *)match->pdata : NULL,
				&local_error);
			break;
		}

		case VENTURE_JSONL_MESSAGE_CURSOR:
			venture_feed_batch_set_cursor(self, jsonl_string(object, "value"));
			break;

		case VENTURE_JSONL_MESSAGE_NOT_MODIFIED:
			/* For one venue it says nothing the store needs: a venue
			 * with no snapshot this time keeps the one it had. */
			if (NULL == jsonl_string(object, "venue"))
				venture_feed_batch_set_not_modified(self, TRUE);
			break;

		case VENTURE_JSONL_MESSAGE_LOG:
			if (logs++ < FEED_BATCH_MAX_LOG_NOTES)
				venture_feed_batch_add_note(self, jsonl_string(object, "message"));
			break;

		case VENTURE_JSONL_MESSAGE_ERROR:
			/* The first error is the one that explains the rest. */
			if (NULL == self->error)
				venture_feed_batch_set_error(self, jsonl_string(object, "message"),
				                             jsonl_int(object, "retry_after", 0));
			break;

		default:
			/* A result answers an automation handler, not a feed. */
			break;
		}

		if (!ok)
		{
			self->refused++;

			if (NULL != local_error)
			{
				g_autofree gchar *note = NULL;

				note = g_strdup_printf("Line %u (%s): %s",
				                       venture_jsonl_message_get_line(message),
				                       venture_jsonl_message_get_kind_name(message),
				                       local_error->message);
				venture_feed_batch_add_note(self, note);
			}
		}
	}

	return TRUE;
}
