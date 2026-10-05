/*
 * venture-arbitrage-books.c - An external ledger in the books
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A source's external ledger (TSM's, a marketplace's sales report) reaches
 * the general journal by one of two doors, and the source's `books`
 * setting picks which; the other refuses, so a sale is never booked once
 * as a day's sales and again as a trade.
 *
 * Daily (post_ledger)
 * -------------------
 * One journal per account per day, dated the day's midnight UTC, through
 * venture_posting_service_post_by_currency_full() -- so a separate-book
 * gold, a valued currency with a rate and a memo currency are each posted
 * by the one currency rule, unchanged. The lines, always in this order:
 *
 *    holding (Dr if the day brought money in, Cr if it took money out) net
 *    trading_sales      Cr  what sold, after the venue's cut
 *    trading_purchases  Dr  what was bought
 *    trading_income     Cr  other money in
 *    trading_expenses   Dr  other money out
 *    trading_capital    Cr  money the day spent that the ledger never
 *                           showed arriving
 *
 * plus, once per account, an opening journal (Dr holding / Cr capital)
 * dated the first day, for what the source saw the account hold then.
 *
 * Purchases are expensed as they are made (periodic, the way the source
 * itself reports profit); stock bought and still held is not an asset in
 * these books. Trades mode is the door that matches cost to sales.
 *
 * Why capital: a ledger of sales and purchases is not a ledger of the
 * purse. Gold mailed between the operator's own characters is left out
 * (tsmctl's default), so a character that spends gold an alt sent it would
 * take its holding below zero, and the holding floor refuses that. The
 * books cannot invent where the gold came from, but they know it came: so
 * a day that would go below zero carries the least capital that keeps it
 * at zero, credited to trading capital, an equity account, on that day.
 * The capital is computed from this source's own days alone -- the running
 * balance of the opening and every day before -- never from the books'
 * state, so it is the same every time it is asked, and is part of the
 * day's fingerprint: a day earlier in the chain that changes moves it.
 *
 * Idempotent. Each day and opening is an external_posting record keyed
 * "<source uuid>:<account key>:<YYYY-MM-DD>" (":opening"), holding the
 * figures it was posted with as a fingerprint. A pass plans every day the
 * books hold plus the window's new ones, compares, and writes only what
 * differs: a changed day is reversed (dated as the original) and posted
 * again under a new posting key "extledger:<uuid>:<record id>:<revision>".
 * Because capital chains, every posted day after the first change is
 * reposted with it: reversing all of them first, newest first, then
 * posting oldest first is what keeps every posting the floor judges
 * against a timeline in which the purse never goes short.
 *
 * Trades (record_flips)
 * ---------------------
 * The store's first-in-first-out matching (venture_series_store_flip_
 * pairs()) from the start of the source's books, one trade per sale: the
 * buys it drew on as buy legs at each buyer's purse, the sale as one sell
 * leg at the seller's, executed in time order and closed at the sale.
 * What each ledger row gave is kept in the trade's `expected`, and the
 * next run matches only what is left, so no unit is booked twice even
 * when the ledger grows behind it.
 */

#include "venture.h"
#include "arbitrage/venture-arbitrage-private.h"

#include <string.h>

#define BOOKS_INSTALLED_KEY	"venture-arbitrage-books-installed"
#define BOOKS_CONTEXT_KEY	"venture-arbitrage-books-context"
#define BOOKS_PERMIT_KEY	"venture-arbitrage-books-permit"
#define BOOKS_NONE		(G_MININT64)
#define BOOKS_DAY		(G_GINT64_CONSTANT(86400))
#define BOOKS_RETRY_MS		(50)
#define BOOKS_LISTED		(200)
#define BOOKS_SEP		"\x1f"

/* ==========================================================================
 * Shared
 * ========================================================================== */

typedef enum
{
	BOOKS_MODE_NONE = 0,
	BOOKS_MODE_DAILY,
	BOOKS_MODE_TRADES
} BooksMode;

typedef struct
{
	BooksMode	 mode;
	gboolean	 auto_post;
	gint64		 from;
	guint		 max_writes;
} BooksSettings;

#ifdef VENTURE_HAVE_SQLITE

static const gchar *const books_mode_names[] = { "none", "daily", "trades", NULL };

static gint64
books_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

static gint
books_enum(
	VentureEntity	*entity,
	const gchar	*property
){
	gint value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

static gchar *
books_string(
	VentureEntity	*entity,
	const gchar	*property
){
	gchar *value;

	value = NULL;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

/* The minor units of a money field, 0 when it is empty. */
static gint64
books_minor(
	VentureEntity	*entity,
	const gchar	*property
){
	g_autoptr(VentureMoney) money = NULL;

	if (NULL != entity)
		g_object_get(entity, property, &money, NULL);

	return (NULL != money) ? venture_money_get_amount(money) : 0;
}

/* The start of @when's UTC day. */
static gint64
books_day_of(gint64 when)
{
	gint64 day;

	day = when / BOOKS_DAY;

	if ((when % BOOKS_DAY) < 0)
		day--;

	return day * BOOKS_DAY;
}

static gchar *
books_date(gint64 day)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(day);

	return (NULL != when) ? g_date_time_format(when, "%Y-%m-%d") : g_strdup("?");
}

static gchar *
books_iso(gint64 when)
{
	g_autoptr(GDateTime) value = g_date_time_new_from_unix_utc(when);

	return venture_time_to_string(value);
}

/* @sum += @term, refusing an overflow rather than wrapping. */
static gboolean
books_add(
	gint64		 *sum,
	gint64		  term,
	GError		**error
){
	gint64 result;

	if (__builtin_add_overflow(*sum, term, &result))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "An external ledger total does not fit in a 64-bit integer");
		return FALSE;
	}

	*sum = result;

	return TRUE;
}

/* Filled field by field, as every hand-made actor is (AGENTS.md). */
static void
books_system_actor(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor->name = VENTURE_ARBITRAGE_BOOKS_HOOK;
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

#endif /* VENTURE_HAVE_SQLITE */

static void
books_weak_ref_free(gpointer data)
{
	g_weak_ref_clear(data);
	g_free(data);
}

/* The last context made over @database, for the actions, which are
 * registered once per database. */
static VentureContext *
books_context_for(VentureDatabase *database)
{
	GWeakRef *ref;

	ref = g_object_get_data(G_OBJECT(database), BOOKS_CONTEXT_KEY);

	return (NULL != ref) ? g_weak_ref_get(ref) : NULL;
}

/* ==========================================================================
 * Settings
 * ========================================================================== */

#ifdef VENTURE_HAVE_SQLITE

/*
 * The books' settings from a source's parsed settings. Absent is the
 * default: books none, no automatic posting, the whole ledger, the
 * default bound. Shared by the save validator and the passes, so what
 * was accepted is what runs.
 */
static gboolean
books_settings_from(
	JsonObject	 *settings,
	BooksSettings	 *out,
	GError		**error
){
	JsonNode *node;
	guint i;

	out->mode = BOOKS_MODE_NONE;
	out->auto_post = FALSE;
	out->from = BOOKS_NONE;
	out->max_writes = VENTURE_ARBITRAGE_BOOKS_MAX_WRITES;

	if (NULL == settings)
		return TRUE;

	node = json_object_get_member(settings, "books");

	if ((NULL != node) && !JSON_NODE_HOLDS_NULL(node))
	{
		const gchar *text;

		text = (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_STRING == json_node_get_value_type(node)))
			? json_node_get_string(node) : NULL;

		for (i = 0; NULL != books_mode_names[i]; i++)
			if (0 == g_strcmp0(text, books_mode_names[i]))
				break;

		if (NULL == books_mode_names[i])
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "Setting books must be none, daily (a summary journal per "
			                    "account per day) or trades (flips recorded as arbitrage "
			                    "trades)");
			return FALSE;
		}

		out->mode = (BooksMode)i;
	}

	node = json_object_get_member(settings, "post_to_books");

	if ((NULL != node) && !JSON_NODE_HOLDS_NULL(node))
	{
		if (!JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_BOOLEAN != json_node_get_value_type(node)))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "Setting post_to_books must be true or false");
			return FALSE;
		}

		out->auto_post = json_node_get_boolean(node);
	}

	/* Posting after every run is the daily door's: a trade is a
	 * decision about matching, made by asking for it. */
	if (out->auto_post && (BOOKS_MODE_DAILY != out->mode))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "Setting post_to_books needs books: daily -- it posts each run's "
		                    "days; flips are recorded by asking for them");
		return FALSE;
	}

	node = json_object_get_member(settings, "books_from");

	if ((NULL != node) && !JSON_NODE_HOLDS_NULL(node))
	{
		g_autoptr(GDateTime) when = NULL;

		if (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_STRING == json_node_get_value_type(node)))
			when = venture_time_from_string(json_node_get_string(node), NULL);

		if (NULL == when)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "Setting books_from must be a date, such as 2026-09-01");
			return FALSE;
		}

		out->from = books_day_of(g_date_time_to_unix(when));
	}

	node = json_object_get_member(settings, "books_max_writes");

	if ((NULL != node) && !JSON_NODE_HOLDS_NULL(node))
	{
		if (!JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_INT64 != json_node_get_value_type(node)) ||
		    (json_node_get_int(node) < 1) ||
		    (json_node_get_int(node) > VENTURE_ARBITRAGE_BOOKS_WRITES_LIMIT))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "Setting books_max_writes must be a whole number from 1 to %d",
			            VENTURE_ARBITRAGE_BOOKS_WRITES_LIMIT);
			return FALSE;
		}

		out->max_writes = (guint)json_node_get_int(node);
	}

	return TRUE;
}

#endif /* VENTURE_HAVE_SQLITE */

#ifdef VENTURE_HAVE_SQLITE

static gboolean
books_settings_read(
	VentureEntity	 *source,
	BooksSettings	 *out,
	GError		**error
){
	g_autoptr(JsonObject) settings = NULL;
	g_autofree gchar *text = NULL;

	g_object_get(source, "settings", &text, NULL);
	settings = venture_feeds_parse_settings(text, error);

	if (NULL == settings)
		return FALSE;

	return books_settings_from(settings, out, error);
}

#endif /* VENTURE_HAVE_SQLITE */

/* ==========================================================================
 * Validators and the guard
 * ========================================================================== */

/* A source's books settings, judged when the settings are written. */
static gboolean
books_validate_source(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
#ifdef VENTURE_HAVE_SQLITE
	g_autofree gchar *text = NULL;
	g_autofree gchar *was = NULL;
	g_autoptr(JsonObject) settings = NULL;
	BooksSettings parsed;

	(void)database;
	(void)user_data;

	g_object_get(entity, "settings", &text, NULL);

	if (NULL != previous)
		g_object_get(previous, "settings", &was, NULL);

	if ((NULL != previous) && (0 == g_strcmp0(text, was)))
		return TRUE;

	/* Settings that are not YAML are the feeds validator's to refuse. */
	settings = venture_feeds_parse_settings(text, NULL);

	if (NULL == settings)
		return TRUE;

	return books_settings_from(settings, &parsed, error);
#else
	(void)database;
	(void)entity;
	(void)previous;
	(void)user_data;
	(void)error;

	return TRUE;
#endif
}

/*
 * The record the books service is writing right now, named by id (0 for
 * a new one): an external_posting is the books' memory of what they
 * posted, and a person writing one could make a posted day look unposted
 * (posted twice next pass) or the other way round (never posted).
 */
static gboolean
books_permitted(
	VentureDatabase	*database,
	VentureEntity	*entity
){
	gint64 *permit;

	permit = g_object_get_data(G_OBJECT(database), BOOKS_PERMIT_KEY);

	return (NULL != permit) && (*permit == venture_entity_get_id(entity));
}

#ifdef VENTURE_HAVE_SQLITE

static gboolean
books_save_permitted(
	VentureDatabase		 *database,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	gint64 permit;
	gpointer previous;
	gboolean saved;

	permit = venture_entity_get_id(entity);
	previous = g_object_get_data(G_OBJECT(database), BOOKS_PERMIT_KEY);
	g_object_set_data(G_OBJECT(database), BOOKS_PERMIT_KEY, &permit);
	saved = venture_database_save(database, entity, actor, error);
	g_object_set_data(G_OBJECT(database), BOOKS_PERMIT_KEY, previous);

	return saved;
}

#endif /* VENTURE_HAVE_SQLITE */

static gboolean
books_validate_posting(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	(void)previous;
	(void)user_data;

	if (books_permitted(database, entity))
		return TRUE;

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
	                    "An external ledger day is written by the data source's Post to "
	                    "books action, beside the journal it posts; it cannot be written "
	                    "by hand");

	return FALSE;
}

gboolean
venture_arbitrage_books_check_write(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	gboolean	  removal,
	GError		**error
){
	(void)database;

	if (!removal || !VENTURE_IS_EXTERNAL_POSTING(entity))
		return TRUE;

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
	                    "An external ledger day is how the books know the day is posted; "
	                    "without it the next pass would post the day again beside its "
	                    "journal. A day whose rows changed is reversed and posted again by "
	                    "Post to books itself");

	return FALSE;
}

#ifdef VENTURE_HAVE_SQLITE

/* ==========================================================================
 * A run's shared state
 * ========================================================================== */

typedef struct
{
	VentureContext		*context;
	VentureDatabase		*database;
	VenturePostingService	*posting;
	VentureEntity		*source;
	gint64			 org;
	gint64			 source_id;
	gchar			*uuid;
	gchar			*currency;
	BooksSettings		 settings;
	VentureSeriesStore	*store;
	gint64			 now;
	gint64			 horizon;

	/* external_posting records of the source, by ref, and by account. */
	GHashTable		*records;
	GHashTable		*records_by_account;

	/* Recorded flips: what each ledger row gave (key -> FlipUse), how
	 * many trades each sale made, and the account days they touch. */
	GHashTable		*used;
	GHashTable		*sale_trades;
	GHashTable		*flip_days;
	guint			 trades;

	/* Dates the period guard was asked about: day -> 1 open, 2 closed. */
	GHashTable		*postable;

	/* The source's journals, read once, for reversals. */
	GPtrArray		*journals;

	/* Accounts made on first use, by classification. */
	GHashTable		*accounts;

	GPtrArray		*notes;
} BooksRun;

static void
books_run_clear(BooksRun *run)
{
	g_clear_object(&run->source);
	g_clear_pointer(&run->uuid, g_free);
	g_clear_pointer(&run->currency, g_free);
	g_clear_object(&run->store);
	g_clear_pointer(&run->records, g_hash_table_unref);
	g_clear_pointer(&run->records_by_account, g_hash_table_unref);
	g_clear_pointer(&run->used, g_hash_table_unref);
	g_clear_pointer(&run->sale_trades, g_hash_table_unref);
	g_clear_pointer(&run->flip_days, g_hash_table_unref);
	g_clear_pointer(&run->postable, g_hash_table_unref);
	g_clear_pointer(&run->journals, g_ptr_array_unref);
	g_clear_pointer(&run->accounts, g_hash_table_unref);
	g_clear_pointer(&run->notes, g_ptr_array_unref);
}

/* A note for the answer, once however often it is said. */
static void
books_note(
	BooksRun	*run,
	const gchar	*format,
	...
) G_GNUC_PRINTF(2, 3);

static void
books_note(
	BooksRun	*run,
	const gchar	*format,
	...
){
	va_list args;
	gchar *text;
	guint i;

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);

	for (i = 0; i < run->notes->len; i++)
	{
		if (0 == g_strcmp0(g_ptr_array_index(run->notes, i), text))
		{
			g_free(text);
			return;
		}
	}

	g_ptr_array_add(run->notes, text);
}

/* "<account>\x1f<day>", the key of an account's day in the sets. */
static gchar *
books_day_key(
	const gchar	*account,
	gint64		 day
){
	return g_strdup_printf("%s" BOOKS_SEP "%" G_GINT64_FORMAT, (NULL != account) ? account : "",
	                       books_day_of(day));
}

/* What the modules and the build must offer before anything is read. */
static gboolean
books_can_run(
	VentureContext	 *context,
	GError		**error
){
	if (!venture_context_module_enabled(context, "arbitrage") ||
	    !venture_context_module_enabled(context, "ledger") ||
	    !venture_context_module_enabled(context, "marketdata"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "An external ledger reaches the books through the arbitrage, "
		                    "ledger and market data modules, and one of them is off");
		return FALSE;
	}

	if (NULL == venture_context_get_feeds_service(context))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "An external ledger lives in the stores the feeds module keeps, "
		                    "and the feeds module is off");
		return FALSE;
	}

	return TRUE;
}

/* The organization asked, or the default one. */
static gint64
books_organization(
	VentureContext	*context,
	gint64		 organization_id
){
	return (organization_id > 0) ? organization_id
	                             : venture_context_get_default_organization_id(context);
}

/*
 * Opens a run over @data_source_id in @organization_id: the source (the
 * organization's, not deleted), its settings and currency, its store and
 * what the books already hold for it.
 */
static gboolean
books_run_open(
	BooksRun	 *run,
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  data_source_id,
	GError		**error
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) local_error = NULL;
	gint64 daily_days;

	memset(run, 0, sizeof(*run));
	run->notes = g_ptr_array_new_with_free_func(g_free);
	run->context = context;
	run->database = venture_context_get_database(context);
	run->posting = venture_database_get_posting_service(run->database);
	run->org = books_organization(context, organization_id);
	run->source_id = data_source_id;
	run->now = g_get_real_time() / G_USEC_PER_SEC;
	run->horizon = BOOKS_NONE;

	if (!books_can_run(context, error))
		return FALSE;

	if (data_source_id > 0)
		source = venture_database_get(run->database, VENTURE_TYPE_DATA_SOURCE, data_source_id, NULL);

	if ((NULL == source) || venture_entity_is_deleted(source) ||
	    (venture_entity_get_organization_id(source) != run->org))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no data source #%" G_GINT64_FORMAT " in this organization",
		            data_source_id);
		return FALSE;
	}

	run->source = g_steal_pointer(&source);
	run->uuid = g_strdup(venture_entity_get_uuid(run->source));

	if (!books_settings_read(run->source, &run->settings, error))
		return FALSE;

	run->currency = books_string(run->source, "currency");

	if (venture_string_is_empty(run->currency))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "The data source names no currency, so its ledger carries no "
		                    "money to post; set its currency first");
		return FALSE;
	}

	{
		gchar *upper = g_ascii_strup(run->currency, -1);

		g_free(run->currency);
		run->currency = upper;
	}

	run->store = venture_feeds_service_open_reader(venture_context_get_feeds_service(context),
	                                               data_source_id, &local_error);

	if (NULL == run->store)
	{
		if (g_error_matches(local_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Data source #%" G_GINT64_FORMAT " has stored nothing yet",
			            data_source_id);
		else
			g_propagate_error(error, g_steal_pointer(&local_error));

		return FALSE;
	}

	/*
	 * A day retention has purged from the store reads as a day with no
	 * rows. That is not a correction: such days are kept as posted. The
	 * store purges whole days before today - N + 1 (series.daily_days),
	 * judged by its own clock earlier than this one, so every day before
	 * this clock's bound is treated as gone.
	 */
	config = g_object_ref(venture_context_get_config(context));
	daily_days = 0;
	g_object_get(config, "series-daily-days", &daily_days, NULL);

	if (daily_days > 0)
		run->horizon = books_day_of(run->now) - (daily_days - 1) * BOOKS_DAY;

	run->records = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	run->records_by_account = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                                (GDestroyNotify)g_ptr_array_unref);
	run->used = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	run->sale_trades = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	run->flip_days = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	run->postable = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	run->accounts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	return TRUE;
}

/* The source's external_posting records, deleted ones included (none can
 * be: the guard refuses it, but a hand-run SQL could). */
static gboolean
books_load_records(
	BooksRun	 *run,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	query = venture_query_new(VENTURE_TYPE_EXTERNAL_POSTING);
	venture_query_set_organization(query, run->org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, VENTURE_ARBITRAGE_BOOKS_MAX_RECORDS + 1);

	if (!venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ,
	                                  run->source_id, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;

	rows = venture_database_find(run->database, query, error);

	if (NULL == rows)
		return FALSE;

	if (rows->len > VENTURE_ARBITRAGE_BOOKS_MAX_RECORDS)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The source has more than %d posted days; the books are not read in "
		            "part", VENTURE_ARBITRAGE_BOOKS_MAX_RECORDS);
		return FALSE;
	}

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *ref = books_string(row, "ref");
		g_autofree gchar *key = books_string(row, "account-key");
		GPtrArray *list;

		if (venture_string_is_empty(ref))
			continue;

		g_hash_table_replace(run->records, g_strdup(ref), g_object_ref(row));
		list = g_hash_table_lookup(run->records_by_account, (NULL != key) ? key : "");

		if (NULL == list)
		{
			list = g_ptr_array_new_with_free_func(g_object_unref);
			g_hash_table_replace(run->records_by_account, g_strdup((NULL != key) ? key : ""), list);
		}

		g_ptr_array_add(list, g_object_ref(row));
	}

	return TRUE;
}

/* Whether a record stands for something posted: its fingerprint is set. */
static gboolean
books_record_posted(VentureEntity *record)
{
	g_autofree gchar *fingerprint = books_string(record, "fingerprint");

	return !venture_string_is_empty(fingerprint);
}

/* Whether a posted record put any money in the books. */
static gboolean
books_record_has_money(VentureEntity *record)
{
	return books_record_posted(record) &&
	       ((0 != books_minor(record, "sales")) || (0 != books_minor(record, "purchases")) ||
	        (0 != books_minor(record, "income")) || (0 != books_minor(record, "expenses")) ||
	        (0 != books_minor(record, "capital")));
}

/* Adds what a recorded trade's ledger row gave to the run's totals. */
static void
books_use_add(
	BooksRun	*run,
	const gchar	*key,
	gint64		 units,
	gint64		 amount
){
	VentureSeriesFlipUse *use;

	if (venture_string_is_empty(key))
		return;

	use = g_hash_table_lookup(run->used, key);

	if (NULL == use)
	{
		use = g_new0(VentureSeriesFlipUse, 1);
		g_hash_table_replace(run->used, g_strdup(key), use);
	}

	use->units += MAX((gint64)0, units);
	use->amount += MAX((gint64)0, amount);
}

/*
 * The source's recorded flips, deleted ones included: a deleted trade's
 * journals stay posted (deletion is not a correction), so what it took
 * from the ledger stays taken. Reads what each row gave, how many trades
 * each sale made, and the account days they touch.
 */
static gboolean
books_load_trades(
	BooksRun	 *run,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	run->trades = 0;

	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(),
	                                                     "arbitrage_trade"))
		return TRUE;

	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_TRADE);
	venture_query_set_organization(query, run->org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, VENTURE_ARBITRAGE_BOOKS_MAX_RECORDS + 1);

	if (!venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ,
	                                  run->source_id, error))
		return FALSE;

	rows = venture_database_find(run->database, query, error);

	if (NULL == rows)
		return FALSE;

	if (rows->len > VENTURE_ARBITRAGE_BOOKS_MAX_RECORDS)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The source has more than %d recorded flips; they are not read in part",
		            VENTURE_ARBITRAGE_BOOKS_MAX_RECORDS);
		return FALSE;
	}

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *expected = books_string(row, "expected");
		g_autoptr(JsonNode) parsed = NULL;
		JsonObject *root;
		JsonObject *from;
		JsonObject *sale;
		JsonArray *buys;
		const gchar *sale_key;
		guint j;

		run->trades++;
		parsed = venture_string_is_empty(expected) ? NULL : venture_json_parse(expected, NULL);

		if ((NULL == parsed) || !JSON_NODE_HOLDS_OBJECT(parsed))
			continue;

		root = json_node_get_object(parsed);
		from = (json_object_has_member(root, "source") &&
		        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "source")))
			? json_object_get_object_member(root, "source") : NULL;

		if (NULL == from)
			continue;

		sale = (json_object_has_member(from, "sale") &&
		        JSON_NODE_HOLDS_OBJECT(json_object_get_member(from, "sale")))
			? json_object_get_object_member(from, "sale") : NULL;

		if (NULL != sale)
		{
			g_autofree gchar *day = NULL;

			sale_key = venture_json_object_get_string(sale, "key", NULL);
			books_use_add(run, sale_key, venture_json_object_get_int(sale, "units", 0),
			              venture_json_object_get_int(sale, "proceeds", 0));

			if (NULL != sale_key)
				g_hash_table_replace(run->sale_trades, g_strdup(sale_key),
				                     GUINT_TO_POINTER(GPOINTER_TO_UINT(
				                         g_hash_table_lookup(run->sale_trades, sale_key)) + 1));

			day = books_day_key(venture_json_object_get_string(sale, "account", ""),
			                    venture_json_object_get_int(sale, "at", 0));
			g_hash_table_add(run->flip_days, g_steal_pointer(&day));
		}

		buys = (json_object_has_member(from, "buys") &&
		        JSON_NODE_HOLDS_ARRAY(json_object_get_member(from, "buys")))
			? json_object_get_array_member(from, "buys") : NULL;

		for (j = 0; (NULL != buys) && (j < json_array_get_length(buys)); j++)
		{
			JsonNode *element = json_array_get_element(buys, j);
			JsonObject *buy;
			gchar *day;

			if (!JSON_NODE_HOLDS_OBJECT(element))
				continue;

			buy = json_node_get_object(element);
			books_use_add(run, venture_json_object_get_string(buy, "key", NULL),
			              venture_json_object_get_int(buy, "units", 0),
			              venture_json_object_get_int(buy, "cost", 0));
			day = books_day_key(venture_json_object_get_string(buy, "account", ""),
			                    venture_json_object_get_int(buy, "at", 0));
			g_hash_table_add(run->flip_days, day);
		}
	}

	return TRUE;
}

/* Whether the period guard lets @day be posted; asked once per day. */
static gboolean
books_postable(
	BooksRun	 *run,
	gint64		  day,
	gboolean	 *out_open,
	GError		**error
){
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(GError) veto = NULL;
	gpointer cached;
	gint64 *key;

	cached = g_hash_table_lookup(run->postable, &day);

	if (NULL != cached)
	{
		*out_open = (1 == GPOINTER_TO_INT(cached));
		return TRUE;
	}

	when = g_date_time_new_from_unix_utc(day);
	*out_open = venture_posting_service_is_date_postable(run->posting, run->org, when, &veto);

	if (!*out_open && (NULL == veto))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		                    "The period check refused a day without saying why");
		return FALSE;
	}

	key = g_new(gint64, 1);
	*key = day;
	g_hash_table_replace(run->postable, key, GINT_TO_POINTER(*out_open ? 1 : 2));

	return TRUE;
}

/* An account made on first use, or found; cached for the run. */
static gint64
books_account(
	BooksRun		 *run,
	const gchar		 *classification,
	GDateTime		 *when,
	const VentureActor	 *actor,
	GError			**error
){
	gpointer cached;
	gint64 id;

	cached = g_hash_table_lookup(run->accounts, classification);

	if (NULL != cached)
		return (gint64)GPOINTER_TO_SIZE(cached);

	id = venture_arbitrage_account(run->database, run->org, classification, when, actor, error);

	if (id > 0)
		g_hash_table_replace(run->accounts, g_strdup(classification),
		                     GSIZE_TO_POINTER((gsize)id));

	return id;
}

/* The location an account stands for, found by its reference; deleted
 * ones too (*out_deleted), never made here. */
static gboolean
books_find_location(
	BooksRun	 *run,
	const gchar	 *account_key,
	gint64		 *out_id,
	gboolean	 *out_deleted,
	GError		**error
){
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *ref = NULL;

	*out_id = 0;
	*out_deleted = FALSE;

	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(),
	                                                     "location"))
		return TRUE;

	ref = venture_marketdata_account_ref(run->source, account_key);

	if (NULL == ref)
		return TRUE;

	found = venture_marketdata_find_by_ref(run->database, VENTURE_TYPE_LOCATION, run->org, ref,
	                                       &local_error);

	if (NULL != local_error)
	{
		g_propagate_error(error, g_steal_pointer(&local_error));
		return FALSE;
	}

	if (NULL == found)
		return TRUE;

	*out_deleted = venture_entity_is_deleted(found);
	*out_id = venture_entity_get_id(found);

	return TRUE;
}

/*
 * The place an account's money moves through, made when it is missing
 * (an explicit promotion, which never has anything to restore here: a
 * deleted one is refused before). 0 with no error when the sales module,
 * which owns locations, is off.
 */
static gboolean
books_ensure_location(
	BooksRun		 *run,
	const gchar		 *account_key,
	const VentureActor	 *actor,
	gint64			 *out_id,
	GError			**error
){
	g_autoptr(VentureEntity) location = NULL;
	gboolean deleted;

	if (!books_find_location(run, account_key, out_id, &deleted, error))
		return FALSE;

	if (deleted)
	{
		*out_id = 0;
		return TRUE;
	}

	if ((*out_id > 0) ||
	    (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(),
	                                                      "location")))
		return TRUE;

	if (!venture_marketdata_promote_account(run->context, run->org, run->source_id, account_key,
	                                        actor, &location, error))
		return FALSE;

	*out_id = venture_entity_get_id(location);

	return TRUE;
}

/* ==========================================================================
 * Daily: the plan
 * ========================================================================== */

typedef enum
{
	BOOKS_STATUS_POSTED = 0,	/* in the books as its rows say */
	BOOKS_STATUS_NEW,		/* not in the books; posts */
	BOOKS_STATUS_CHANGED,		/* in the books; its figures moved; posts again */
	BOOKS_STATUS_FOLLOWS,		/* in the books; posts again behind an earlier change */
	BOOKS_STATUS_KEPT,		/* in the books; kept as posted (retention, a closed period) */
	BOOKS_STATUS_LEFT_OUT,		/* not posted: flips of the day are recorded as trades */
	BOOKS_STATUS_CLOSED,		/* not posted: its period is closed */
	BOOKS_STATUS_NO_PLACE,		/* not posted: the account's place was deleted */
	BOOKS_STATUS_LATER		/* would post; left for the next pass by the bound */
} BooksStatus;

static const gchar *const books_status_nicks[] = {
	"posted", "unposted", "changed", "follows", "kept", "left_out", "closed", "no_place", "later"
};

typedef struct
{
	VentureExternalPostingKind	 kind;
	gint64				 day;
	gint64				 sales;
	gint64				 purchases;
	gint64				 income;
	gint64				 expenses;
	gint64				 capital;
	gchar				*fingerprint;
	gchar				*ref;
	VentureEntity			*record;
	BooksStatus			 status;
	gboolean			 written;
} BooksItem;

static void
books_item_free(gpointer data)
{
	BooksItem *item = data;

	g_free(item->fingerprint);
	g_free(item->ref);
	g_clear_object(&item->record);
	g_free(item);
}

typedef struct
{
	gchar		*key;
	gchar		*name;
	gint64		 location_id;
	gboolean	 no_place;
	gboolean	 allow_negative;
	GPtrArray	*items;
	guint		 writes;
} BooksAccount;

static void
books_account_free(gpointer data)
{
	BooksAccount *account = data;

	g_free(account->key);
	g_free(account->name);
	g_ptr_array_unref(account->items);
	g_free(account);
}

/* Whether a status means the pass writes the item. */
static gboolean
books_status_writes(BooksStatus status)
{
	return (BOOKS_STATUS_NEW == status) || (BOOKS_STATUS_CHANGED == status) ||
	       (BOOKS_STATUS_FOLLOWS == status);
}

/* An item's net on the holding: everything in less everything out. */
static gboolean
books_item_net(
	const BooksItem	 *item,
	gint64		 *out,
	GError		**error
){
	gint64 net = 0;

	if (!books_add(&net, item->sales, error) || !books_add(&net, item->income, error) ||
	    !books_add(&net, item->capital, error) || !books_add(&net, -item->purchases, error) ||
	    !books_add(&net, -item->expenses, error))
		return FALSE;

	*out = net;

	return TRUE;
}

static gboolean
books_item_has_money(const BooksItem *item)
{
	return (0 != item->sales) || (0 != item->purchases) || (0 != item->income) ||
	       (0 != item->expenses) || (0 != item->capital);
}

/* The figures as text: what a later pass compares to decide "changed". */
static gchar *
books_fingerprint(
	BooksRun	*run,
	BooksItem	*item
){
	if (VENTURE_EXTERNAL_POSTING_KIND_OPENING == item->kind)
		return g_strdup_printf("v1:%s:opening:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
		                       run->currency, item->day, item->capital);

	return g_strdup_printf("v1:%s:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT
	                       ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, run->currency, item->sales,
	                       item->purchases, item->income, item->expenses, item->capital);
}

static gchar *
books_ref(
	BooksRun			*run,
	const gchar			*account_key,
	VentureExternalPostingKind	 kind,
	gint64				 day
){
	g_autofree gchar *date = NULL;

	if (VENTURE_EXTERNAL_POSTING_KIND_OPENING == kind)
		return g_strdup_printf("%s:%s:opening", run->uuid, account_key);

	date = books_date(day);

	return g_strdup_printf("%s:%s:%s", run->uuid, account_key, date);
}

/* Takes a kept record's stored figures, which are what the books hold. */
static void
books_item_from_record(
	BooksItem	*item,
	VentureEntity	*record
){
	item->sales = books_minor(record, "sales");
	item->purchases = books_minor(record, "purchases");
	item->income = books_minor(record, "income");
	item->expenses = books_minor(record, "expenses");
	item->capital = books_minor(record, "capital");
}

typedef struct
{
	gint64	sales;
	gint64	purchases;
	gint64	income;
	gint64	expenses;
} BooksFigures;

static gint
books_compare_days(
	gconstpointer	a,
	gconstpointer	b
){
	gint64 left = *(const gint64 *)a;
	gint64 right = *(const gint64 *)b;

	return (left < right) ? -1 : ((left > right) ? 1 : 0);
}

/* What the account held at @when, as the source last saw it before, or 0. */
static gboolean
books_balance_at(
	BooksRun	 *run,
	const gchar	 *account_key,
	gint64		  when,
	gint64		 *out,
	GError		**error
){
	g_autoptr(GArray) points = NULL;

	*out = 0;
	points = venture_series_store_balance_history(run->store, account_key, run->currency,
	                                              VENTURE_SERIES_NONE, when + 1, error);

	if (NULL == points)
		return FALSE;

	if (points->len > 0)
		*out = MAX((gint64)0, g_array_index(points, VentureSeriesAmount, points->len - 1).amount);

	return TRUE;
}

/*
 * Plans one account: which days are in the books (every one posted
 * before, and the window's days with money), what each should say now,
 * and which differ. See the file's comment for the capital chain.
 */
static gboolean
books_plan_account(
	BooksRun	 *run,
	const gchar	 *account_key,
	const gchar	 *account_name,
	gint64		  window_from,
	gint64		  window_until,
	BooksAccount	**out,
	GError		**error
){
	g_autoptr(GPtrArray) totals = NULL;
	g_autoptr(GHashTable) figures = NULL;
	g_autoptr(GArray) chain = NULL;
	g_autoptr(GPtrArray) left_out = NULL;
	BooksAccount *account;
	VentureSeriesTxnFilter filter;
	GPtrArray *records;
	BooksItem *opening;
	gint64 holding;
	gint64 running;
	gint first_change;
	guint other_currency;
	guint i;

	account = g_new0(BooksAccount, 1);
	account->key = g_strdup(account_key);
	account->name = g_strdup(!venture_string_is_empty(account_name) ? account_name : account_key);
	account->items = g_ptr_array_new_with_free_func(books_item_free);
	*out = account;

	if (!books_find_location(run, account_key, &account->location_id, &account->no_place, error))
		return FALSE;

	if ((account->location_id > 0) && !account->no_place)
	{
		g_autoptr(VentureEntity) row = NULL;

		holding = 0;

		if (!venture_holdings_account_for_location(run->database, run->org, account->location_id,
		                                           FALSE, NULL, &holding, error))
			return FALSE;

		row = (holding > 0) ? venture_database_get(run->database, VENTURE_TYPE_ACCOUNT, holding, NULL)
		                    : NULL;

		if (NULL != row)
			g_object_get(row, "allow-negative", &account->allow_negative, NULL);
	}

	/* --- The store's days, in the source's currency --- */

	venture_series_txn_filter_init(&filter);
	filter.account_key = account_key;
	totals = venture_series_store_txn_totals(run->store, &filter, VENTURE_SERIES_TXN_GROUP_DAY, error);

	if (NULL == totals)
		return FALSE;

	figures = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	other_currency = 0;

	for (i = 0; i < totals->len; i++)
	{
		VentureSeriesTxnTotal *bucket = g_ptr_array_index(totals, i);
		BooksFigures *day;
		gint64 *key;

		if ((0 == bucket->sales_amount) && (0 == bucket->buys_amount) && (0 == bucket->income) &&
		    (0 == bucket->expense))
			continue;

		if (0 != g_ascii_strcasecmp(bucket->currency, run->currency))
		{
			other_currency++;
			continue;
		}

		day = g_new0(BooksFigures, 1);
		day->sales = bucket->sales_amount;
		day->purchases = bucket->buys_amount;
		day->income = bucket->income;
		day->expenses = bucket->expense;
		key = g_new(gint64, 1);
		*key = books_day_of(bucket->period_start);
		g_hash_table_replace(figures, key, day);
	}

	if (other_currency > 0)
		books_note(run, "Rows in a currency other than the source's %s are not posted "
		           "(the source's currency changed since they were stored)", run->currency);

	/* --- Which days: every day posted before, and the window's new ones --- */

	chain = g_array_new(FALSE, FALSE, sizeof(gint64));
	left_out = g_ptr_array_new_with_free_func(books_item_free);
	records = g_hash_table_lookup(run->records_by_account, account_key);

	for (i = 0; (NULL != records) && (i < records->len); i++)
	{
		VentureEntity *record = g_ptr_array_index(records, i);
		g_autoptr(GDateTime) when = NULL;
		gint64 day;

		if ((VENTURE_EXTERNAL_POSTING_KIND_DAY != books_enum(record, "kind")) ||
		    !books_record_posted(record))
			continue;

		g_object_get(record, "day", &when, NULL);

		if (NULL == when)
			continue;

		day = books_day_of(g_date_time_to_unix(when));
		g_array_append_val(chain, day);
	}

	{
		GHashTableIter iter;
		gpointer key;

		g_hash_table_iter_init(&iter, figures);

		while (g_hash_table_iter_next(&iter, &key, NULL))
		{
			gint64 day = *(gint64 *)key;
			g_autofree gchar *ref = NULL;
			g_autofree gchar *flip_key = NULL;
			VentureEntity *record;

			if (((BOOKS_NONE != window_from) && (day < window_from)) ||
			    ((BOOKS_NONE != window_until) && (day >= window_until)))
				continue;

			ref = books_ref(run, account_key, VENTURE_EXTERNAL_POSTING_KIND_DAY, day);
			record = g_hash_table_lookup(run->records, ref);

			if ((NULL != record) && books_record_posted(record))
				continue;

			/* A day whose flips are recorded as trades is booked by
			 * them; posting its sales as well would book them twice. */
			flip_key = books_day_key(account_key, day);

			if (g_hash_table_contains(run->flip_days, flip_key))
			{
				BooksItem *item = g_new0(BooksItem, 1);
				BooksFigures *found = g_hash_table_lookup(figures, &day);

				item->kind = VENTURE_EXTERNAL_POSTING_KIND_DAY;
				item->day = day;
				item->sales = found->sales;
				item->purchases = found->purchases;
				item->income = found->income;
				item->expenses = found->expenses;
				item->ref = g_steal_pointer(&ref);
				item->status = BOOKS_STATUS_LEFT_OUT;
				g_ptr_array_add(left_out, item);
				continue;
			}

			g_array_append_val(chain, day);
		}
	}

	g_array_sort(chain, books_compare_days);

	/* --- The opening --- */

	{
		g_autofree gchar *ref = books_ref(run, account_key, VENTURE_EXTERNAL_POSTING_KIND_OPENING, 0);
		VentureEntity *record = g_hash_table_lookup(run->records, ref);
		gboolean open_period;

		opening = g_new0(BooksItem, 1);
		opening->kind = VENTURE_EXTERNAL_POSTING_KIND_OPENING;
		opening->ref = g_steal_pointer(&ref);
		opening->record = (NULL != record) ? g_object_ref(record) : NULL;

		if ((NULL != record) && books_record_posted(record))
		{
			g_autoptr(GDateTime) when = NULL;

			g_object_get(record, "day", &when, NULL);
			opening->day = (NULL != when) ? g_date_time_to_unix(when) : 0;
		}

		if ((NULL != record) && books_record_posted(record) && (BOOKS_NONE != run->horizon) &&
		    (opening->day < run->horizon))
		{
			books_item_from_record(opening, record);
			opening->status = BOOKS_STATUS_KEPT;
		}
		else if (0 == chain->len)
		{
			/* Nothing in the books any more: an opening posted
			 * before is taken back out. */
			opening->capital = 0;
			opening->status = ((NULL != record) && books_record_has_money(record))
				? BOOKS_STATUS_CHANGED : BOOKS_STATUS_POSTED;
		}
		else
		{
			opening->day = g_array_index(chain, gint64, 0);

			if (!books_balance_at(run, account_key, opening->day, &opening->capital, error))
			{
				books_item_free(opening);
				return FALSE;
			}

			if ((NULL == record) || !books_record_posted(record))
				opening->status = (0 != opening->capital) ? BOOKS_STATUS_NEW : BOOKS_STATUS_POSTED;
			else
			{
				g_autofree gchar *now = books_fingerprint(run, opening);
				g_autofree gchar *was = books_string(record, "fingerprint");

				opening->status = (0 == g_strcmp0(now, was)) ? BOOKS_STATUS_POSTED
				                                            : BOOKS_STATUS_CHANGED;
			}

			if (books_status_writes(opening->status))
			{
				if (!books_postable(run, opening->day, &open_period, error))
				{
					books_item_free(opening);
					return FALSE;
				}

				if (!open_period)
				{
					if ((NULL != record) && books_record_posted(record))
					{
						books_item_from_record(opening, record);
						opening->status = BOOKS_STATUS_KEPT;
					}
					else
					{
						opening->capital = 0;
						opening->status = BOOKS_STATUS_CLOSED;
					}
				}
			}
		}

		opening->fingerprint = books_fingerprint(run, opening);

		/* Nothing to say about an opening never posted and still zero. */
		if ((NULL == record) && (0 == opening->capital) && (BOOKS_STATUS_POSTED == opening->status))
			books_item_free(opening);
		else
			g_ptr_array_add(account->items, opening);
	}

	/* --- The days, oldest first, each with the capital it needs --- */

	running = 0;

	for (i = 0; i < account->items->len; i++)
	{
		BooksItem *item = g_ptr_array_index(account->items, i);

		if (BOOKS_STATUS_CLOSED != item->status)
			running = item->capital;
	}

	for (i = 0; i < chain->len; i++)
	{
		gint64 day = g_array_index(chain, gint64, i);
		BooksItem *item;
		BooksFigures *found;
		VentureEntity *record;
		gint64 net;
		gboolean open_period;

		/* The chain may hold a day twice (posted, and new in the window
		 * -- which the lookup above prevents -- or a record kept twice). */
		if ((i > 0) && (g_array_index(chain, gint64, i - 1) == day))
			continue;

		item = g_new0(BooksItem, 1);
		item->kind = VENTURE_EXTERNAL_POSTING_KIND_DAY;
		item->day = day;
		item->ref = books_ref(run, account_key, VENTURE_EXTERNAL_POSTING_KIND_DAY, day);
		record = g_hash_table_lookup(run->records, item->ref);
		item->record = (NULL != record) ? g_object_ref(record) : NULL;
		g_ptr_array_add(account->items, item);

		if ((NULL != record) && books_record_posted(record) && (BOOKS_NONE != run->horizon) &&
		    (day < run->horizon))
		{
			books_item_from_record(item, record);
			item->status = BOOKS_STATUS_KEPT;
		}
		else
		{
			found = g_hash_table_lookup(figures, &day);

			if (NULL != found)
			{
				item->sales = found->sales;
				item->purchases = found->purchases;
				item->income = found->income;
				item->expenses = found->expenses;
			}

			item->capital = 0;

			if (!books_item_net(item, &net, error) || !books_add(&net, running, error))
				return FALSE;

			/* The least capital that keeps the purse at zero. An
			 * account allowed to go negative is shown as it is. */
			if ((net < 0) && !account->allow_negative)
				item->capital = -net;

			g_free(item->fingerprint);
			item->fingerprint = books_fingerprint(run, item);

			if ((NULL == record) || !books_record_posted(record))
				item->status = BOOKS_STATUS_NEW;
			else
			{
				g_autofree gchar *was = books_string(record, "fingerprint");

				item->status = (0 == g_strcmp0(item->fingerprint, was)) ? BOOKS_STATUS_POSTED
				                                                       : BOOKS_STATUS_CHANGED;
			}

			if (books_status_writes(item->status))
			{
				if (!books_postable(run, day, &open_period, error))
					return FALSE;

				if (!open_period)
				{
					if ((NULL != record) && books_record_posted(record))
					{
						books_item_from_record(item, record);
						item->status = BOOKS_STATUS_KEPT;
					}
					else
						item->status = BOOKS_STATUS_CLOSED;
				}
			}
		}

		if (BOOKS_STATUS_CLOSED == item->status)
			continue;

		if (!books_item_net(item, &net, error) || !books_add(&running, net, error))
			return FALSE;

		if (NULL == item->fingerprint)
			item->fingerprint = books_fingerprint(run, item);
	}

	/* --- Everything posted after the first change follows it --- */

	first_change = -1;

	for (i = 0; i < account->items->len; i++)
	{
		BooksItem *item = g_ptr_array_index(account->items, i);

		if ((first_change < 0) && books_status_writes(item->status))
			first_change = (gint)i;
		else if ((first_change >= 0) && (BOOKS_STATUS_POSTED == item->status))
			item->status = BOOKS_STATUS_FOLLOWS;
	}

	/* What the pass writes for this account: a reversal for each item
	 * that has something posted, a journal for each with money. */
	for (i = 0; i < account->items->len; i++)
	{
		BooksItem *item = g_ptr_array_index(account->items, i);

		if (!books_status_writes(item->status))
			continue;

		if ((NULL != item->record) && books_record_has_money(item->record))
			account->writes++;

		if (books_item_has_money(item))
			account->writes++;
	}

	/* A place a person deleted stays deleted: nothing of this account
	 * is posted until it is restored. */
	if (account->no_place)
	{
		for (i = 0; i < account->items->len; i++)
		{
			BooksItem *item = g_ptr_array_index(account->items, i);

			if (books_status_writes(item->status))
				item->status = BOOKS_STATUS_NO_PLACE;
		}

		account->writes = 0;
	}

	for (i = 0; i < left_out->len; i++)
		g_ptr_array_add(account->items, g_ptr_array_steal_index(left_out, i--));

	return TRUE;
}

/* ==========================================================================
 * Daily: writing
 * ========================================================================== */

/* "external_ledger:12": the rule a record's journals and movements carry. */
static gchar *
books_rule_name(VentureEntity *record)
{
	return g_strdup_printf("%s:%" G_GINT64_FORMAT, VENTURE_ARBITRAGE_BOOKS_RULE,
	                       venture_entity_get_id(record));
}

/*
 * Takes what @item's record put in the books back out: every journal
 * still posted under its rule reversed on its own date (a reversal is
 * never refused by the floor, and dated as the original it leaves the
 * period's books as if the day had never posted), and its memo movements
 * cleared. The record then says nothing is posted.
 */
static gboolean
books_reverse_item(
	BooksRun		 *run,
	BooksItem		 *item,
	const VentureActor	 *actor,
	GError			**error
){
	g_autofree gchar *rule = NULL;
	gboolean has_memo;
	guint i;

	if ((NULL == item->record) || !books_record_posted(item->record))
		return TRUE;

	rule = books_rule_name(item->record);

	for (i = 0; (NULL != run->journals) && (i < run->journals->len); i++)
	{
		VentureEntity *journal = g_ptr_array_index(run->journals, i);
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(VentureJournal) reversal = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *memo = NULL;
		VentureJournalState state;

		g_object_get(journal, "rule-name", &name, "state", &state, "occurred-at", &when, NULL);

		if ((0 != g_strcmp0(name, rule)) || (VENTURE_JOURNAL_POSTED != state))
			continue;

		memo = g_strdup_printf("External ledger day changed: %s", item->ref);
		reversal = venture_posting_service_reverse(run->posting, venture_entity_get_id(journal),
		                                           when, memo, actor, error);

		if (NULL == reversal)
			return FALSE;
	}

	has_memo = FALSE;

	if (!venture_holdings_source_has_memo(run->database, run->org, "data_source", run->source_id,
	                                      rule, &has_memo, error))
		return FALSE;

	if (has_memo && !venture_holdings_clear_memo(run->database, run->org, "data_source",
	                                             run->source_id, rule, actor, error))
		return FALSE;

	g_object_set(item->record, "journal-id", (gint64)0, "fingerprint", NULL, NULL);

	return books_save_permitted(run->database, item->record, actor, error);
}

/* One line of @minor in the source's currency; nothing for zero. */
static void
books_line(
	BooksRun		*run,
	GPtrArray		*lines,
	gint64			 account_id,
	VentureLedgerSide	 side,
	gint64			 minor,
	const gchar		*memo
){
	g_autoptr(VentureMoney) amount = NULL;
	VentureJournalLine *line;

	if (0 == minor)
		return;

	if (minor < 0)
	{
		minor = -minor;
		side = (VENTURE_LEDGER_SIDE_DEBIT == side) ? VENTURE_LEDGER_SIDE_CREDIT
		                                           : VENTURE_LEDGER_SIDE_DEBIT;
	}

	amount = venture_money_new_for_currency(minor, run->currency);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", account_id, "amount", amount, "side", side,
	             "organization-id", run->org, "memo", memo, NULL);
	g_ptr_array_add(lines, line);
}

/*
 * Writes @item: its record (figures, fingerprint, one more revision),
 * then its journal through the posting service's currency rule -- the
 * holding first, then sales, purchases, other income, expenses and
 * capital, in that order -- and the record again with the journal.
 */
static gboolean
books_write_item(
	BooksRun		 *run,
	BooksAccount		 *account,
	BooksItem		 *item,
	gint64			  holding,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(VentureEntity) fresh = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(VentureJournal) header = NULL;
	g_autoptr(VentureMoney) sales = NULL;
	g_autoptr(VentureMoney) purchases = NULL;
	g_autoptr(VentureMoney) income = NULL;
	g_autoptr(VentureMoney) expenses = NULL;
	g_autoptr(VentureMoney) capital = NULL;
	g_autofree gchar *rule = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *memo = NULL;
	g_autofree gchar *date = NULL;
	VentureEntity *record;
	gint64 revision;
	gint64 net;

	when = g_date_time_new_from_unix_utc(item->day);
	record = item->record;

	if (NULL == record)
	{
		fresh = VENTURE_ENTITY(venture_external_posting_new());
		venture_entity_set_organization_id(fresh, run->org);
		record = fresh;
	}

	revision = books_int(record, "revision") + 1;
	sales = venture_money_new_for_currency(item->sales, run->currency);
	purchases = venture_money_new_for_currency(item->purchases, run->currency);
	income = venture_money_new_for_currency(item->income, run->currency);
	expenses = venture_money_new_for_currency(item->expenses, run->currency);
	capital = venture_money_new_for_currency(item->capital, run->currency);
	g_object_set(record, "data-source-id", run->source_id, "account-key", account->key,
	             "location-id", account->location_id, "kind", item->kind, "day", when,
	             "sales", sales, "purchases", purchases, "income", income, "expenses", expenses,
	             "capital", capital, "fingerprint", item->fingerprint, "revision", revision,
	             "journal-id", (gint64)0, "ref", item->ref, NULL);

	if (!books_save_permitted(run->database, record, actor, error))
		return FALSE;

	if (NULL == item->record)
		item->record = g_object_ref(record);

	item->written = TRUE;

	/* A day that came to nothing -- its rows moved to another account --
	 * is recorded as posted with nothing, so it is not asked again. */
	if (!books_item_has_money(item))
		return TRUE;

	if (!books_item_net(item, &net, error))
		return FALSE;

	lines = g_ptr_array_new_with_free_func(g_object_unref);
	date = books_date(item->day);

	if (VENTURE_EXTERNAL_POSTING_KIND_OPENING == item->kind)
	{
		gint64 equity = books_account(run, "trading_capital", when, actor, error);

		if (equity <= 0)
			return FALSE;

		memo = g_strdup_printf("%s: opening balance", account->name);
		books_line(run, lines, holding, VENTURE_LEDGER_SIDE_DEBIT, item->capital, memo);
		books_line(run, lines, equity, VENTURE_LEDGER_SIDE_CREDIT, item->capital, "Opening balance");
	}
	else
	{
		memo = g_strdup_printf("%s, %s: external ledger", account->name, date);
		books_line(run, lines, holding, VENTURE_LEDGER_SIDE_DEBIT, net, memo);

		if (0 != item->sales)
		{
			gint64 id = books_account(run, "trading_sales", when, actor, error);

			if (id <= 0)
				return FALSE;

			books_line(run, lines, id, VENTURE_LEDGER_SIDE_CREDIT, item->sales, "Sales");
		}

		if (0 != item->purchases)
		{
			gint64 id = books_account(run, "trading_purchases", when, actor, error);

			if (id <= 0)
				return FALSE;

			books_line(run, lines, id, VENTURE_LEDGER_SIDE_DEBIT, item->purchases, "Purchases");
		}

		if (0 != item->income)
		{
			gint64 id = books_account(run, "trading_income", when, actor, error);

			if (id <= 0)
				return FALSE;

			books_line(run, lines, id, VENTURE_LEDGER_SIDE_CREDIT, item->income, "Other income");
		}

		if (0 != item->expenses)
		{
			gint64 id = books_account(run, "trading_expenses", when, actor, error);

			if (id <= 0)
				return FALSE;

			books_line(run, lines, id, VENTURE_LEDGER_SIDE_DEBIT, item->expenses, "Expenses");
		}

		if (0 != item->capital)
		{
			gint64 id = books_account(run, "trading_capital", when, actor, error);

			if (id <= 0)
				return FALSE;

			books_line(run, lines, id, VENTURE_LEDGER_SIDE_CREDIT, item->capital,
			           "Money the ledger does not show arriving");
		}
	}

	/* A journal needs two lines; equal sales and purchases on a day with
	 * nothing else net to no holding line and still make two. */
	if (lines->len < 2)
		return TRUE;

	rule = books_rule_name(record);
	key = g_strdup_printf("extledger:%s:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, run->uuid,
	                      venture_entity_get_id(record), revision);
	header = venture_journal_new();
	g_object_set(header, "organization-id", run->org, "occurred-at", when, "currency", run->currency,
	             "source-type", "data_source", "source-id", run->source_id, "memo", memo,
	             "rule-name", rule, "posting-key", key, NULL);
	journals = venture_posting_service_post_by_currency_full(run->posting, header, lines, TRUE, actor,
	                                                         NULL, error);

	if (NULL == journals)
		return FALSE;

	if (0 == journals->len)
		return TRUE;

	g_object_set(record, "journal-id",
	             venture_entity_get_id(VENTURE_ENTITY(g_ptr_array_index(journals, 0))), NULL);

	return books_save_permitted(run->database, record, actor, error);
}

/* Takes an account's changes into the books: its place and purse made if
 * they are missing, every changed item reversed newest first, then
 * written oldest first. */
static gboolean
books_execute_account(
	BooksRun		 *run,
	BooksAccount		 *account,
	const VentureActor	 *actor,
	GError			**error
){
	gint64 holding;
	gboolean any;
	guint i;

	any = FALSE;

	for (i = 0; i < account->items->len; i++)
		if (books_status_writes(((BooksItem *)g_ptr_array_index(account->items, i))->status))
			any = TRUE;

	if (!any)
		return TRUE;

	if (!books_ensure_location(run, account->key, actor, &account->location_id, error))
		return FALSE;

	if (account->location_id <= 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Account %s has no place to hold its money", account->key);
		return FALSE;
	}

	holding = 0;

	if (!venture_holdings_account_for_location(run->database, run->org, account->location_id, TRUE,
	                                           actor, &holding, error))
		return FALSE;

	for (i = account->items->len; i > 0; i--)
	{
		BooksItem *item = g_ptr_array_index(account->items, i - 1);

		if (books_status_writes(item->status) && !books_reverse_item(run, item, actor, error))
			return FALSE;
	}

	for (i = 0; i < account->items->len; i++)
	{
		BooksItem *item = g_ptr_array_index(account->items, i);

		if (!books_status_writes(item->status))
			continue;

		if (!books_write_item(run, account, item, holding, actor, error))
		{
			g_autofree gchar *date = books_date(item->day);

			g_prefix_error(error, "%s, %s: ", account->name,
			               (VENTURE_EXTERNAL_POSTING_KIND_OPENING == item->kind) ? "opening" : date);
			return FALSE;
		}
	}

	return TRUE;
}

/* The second-actor rule on posting, which the automatic pass respects by
 * not posting at all: a proposal nobody asked for, every run, is noise. */
static gboolean
books_needs_second_actor(BooksRun *run)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(),
	                                                     "accounting_approval_rule"))
		return FALSE;

	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_APPROVAL_RULE);
	venture_query_set_organization(query, run->org);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(run->database, query, NULL);

	for (i = 0; (NULL != rows) && (i < rows->len); i++)
	{
		g_autofree gchar *action = NULL;
		gboolean required = FALSE;

		g_object_get(g_ptr_array_index(rows, i), "action", &action, "require-second-actor",
		             &required, NULL);

		if (required && (0 == g_strcmp0(action, "post")))
			return TRUE;
	}

	return FALSE;
}

/* The money of @minor as the report's JSON. */
static JsonNode *
books_money_json(
	BooksRun	*run,
	gint64		 minor
){
	g_autoptr(VentureMoney) money = venture_money_new_for_currency(minor, run->currency);

	return venture_money_to_json(money);
}

static void
books_notes_json(
	BooksRun	*run,
	JsonObject	*root
){
	JsonArray *notes = json_array_new();
	guint i;

	for (i = 0; i < run->notes->len; i++)
		json_array_add_string_element(notes, g_ptr_array_index(run->notes, i));

	json_object_set_array_member(root, "notes", notes);
}

static void
books_set_date(
	JsonObject	*object,
	const gchar	*member,
	gint64		 when
){
	g_autofree gchar *date = NULL;

	if (BOOKS_NONE == when)
	{
		json_object_set_null_member(object, member);
		return;
	}

	date = books_date(when);
	json_object_set_string_member(object, member, date);
}

/*
 * The plan and what was done, as JSON. @listed_from/@listed_until choose
 * which days are listed (BOOKS_NONE: all), @cap how many (0: all).
 */
static JsonNode *
books_post_report(
	BooksRun	*run,
	GPtrArray	*accounts,
	gboolean	 dry_run,
	gint64		 window_from,
	gint64		 window_until,
	gint64		 listed_from,
	gint64		 listed_until,
	guint		 cap,
	guint		 writes
){
	JsonObject *root;
	JsonObject *counts;
	JsonArray *days;
	JsonNode *node;
	guint per_status[G_N_ELEMENTS(books_status_nicks)];
	guint listed;
	guint total;
	guint written;
	gint64 capital;
	guint i;
	guint j;

	memset(per_status, 0, sizeof(per_status));
	root = json_object_new();
	days = json_array_new();
	listed = 0;
	total = 0;
	written = 0;
	capital = 0;

	for (i = 0; i < accounts->len; i++)
	{
		BooksAccount *account = g_ptr_array_index(accounts, i);

		for (j = 0; j < account->items->len; j++)
		{
			BooksItem *item = g_ptr_array_index(account->items, j);
			JsonObject *row;
			g_autofree gchar *date = NULL;
			gint64 net = 0;

			per_status[item->status]++;

			if (item->written)
				written++;

			if (books_status_writes(item->status) && (VENTURE_EXTERNAL_POSTING_KIND_DAY == item->kind))
				capital += item->capital;

			if (((BOOKS_NONE != listed_from) && (item->day < listed_from)) ||
			    ((BOOKS_NONE != listed_until) && (item->day >= listed_until)))
				continue;

			total++;

			if ((cap > 0) && (listed >= cap))
				continue;

			listed++;
			row = json_object_new();
			date = books_date(item->day);
			json_object_set_string_member(row, "account_key", account->key);
			json_object_set_string_member(row, "account", account->name);

			if (account->location_id > 0)
				json_object_set_int_member(row, "location_id", account->location_id);
			else
				json_object_set_null_member(row, "location_id");

			json_object_set_string_member(row, "kind",
			                              (VENTURE_EXTERNAL_POSTING_KIND_OPENING == item->kind)
			                              ? "opening" : "day");
			json_object_set_string_member(row, "day", date);
			json_object_set_string_member(row, "status", books_status_nicks[item->status]);
			json_object_set_boolean_member(row, "written", item->written);
			json_object_set_member(row, "sales", books_money_json(run, item->sales));
			json_object_set_member(row, "purchases", books_money_json(run, item->purchases));
			json_object_set_member(row, "income", books_money_json(run, item->income));
			json_object_set_member(row, "expenses", books_money_json(run, item->expenses));
			json_object_set_member(row, "capital", books_money_json(run, item->capital));
			books_item_net(item, &net, NULL);
			json_object_set_member(row, "net", books_money_json(run, net));

			if ((NULL != item->record) && (books_int(item->record, "journal-id") > 0))
				json_object_set_int_member(row, "journal_id", books_int(item->record, "journal-id"));
			else
				json_object_set_null_member(row, "journal_id");

			json_array_add_object_element(days, row);
		}
	}

	json_object_set_int_member(root, "data_source_id", run->source_id);
	json_object_set_string_member(root, "currency", run->currency);
	json_object_set_string_member(root, "books", books_mode_names[run->settings.mode]);
	json_object_set_boolean_member(root, "dry_run", dry_run);
	books_set_date(root, "from", window_from);
	books_set_date(root, "until", window_until);
	json_object_set_int_member(root, "accounts", accounts->len);
	counts = json_object_new();

	for (i = 0; i < G_N_ELEMENTS(books_status_nicks); i++)
		json_object_set_int_member(counts, books_status_nicks[i], per_status[i]);

	json_object_set_object_member(root, "days_by_status", counts);
	json_object_set_int_member(root, "written", written);
	json_object_set_int_member(root, "journals", writes);
	json_object_set_member(root, "capital", books_money_json(run, capital));
	json_object_set_array_member(root, "days", days);
	json_object_set_int_member(root, "days_total", total);
	books_notes_json(run, root);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	return node;
}

static gint
books_compare_keys(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* The plan's digest: what the pass will write, for the posting boundary
 * to bind a proposal to. Data that moved since is a new proposal. */
static gchar *
books_post_digest(GPtrArray *accounts)
{
	g_autoptr(GString) material = g_string_new("venture-external-ledger-v1");
	guint i;
	guint j;

	for (i = 0; i < accounts->len; i++)
	{
		BooksAccount *account = g_ptr_array_index(accounts, i);

		for (j = 0; j < account->items->len; j++)
		{
			BooksItem *item = g_ptr_array_index(account->items, j);

			if (books_status_writes(item->status))
				g_string_append_printf(material, "\n%s|%s|%s", item->ref,
				                       books_status_nicks[item->status], item->fingerprint);
		}
	}

	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, material->str, -1);
}

static gboolean
books_post_internal(
	VentureContext			 *context,
	const VentureBooksPostQuery	 *query,
	const VentureActor		 *actor,
	gint64				  listed_from,
	gint64				  listed_until,
	guint				  cap,
	JsonNode			**out_report,
	GError				**error
){
	BooksRun run;
	g_autoptr(GPtrArray) store_accounts = NULL;
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autoptr(GPtrArray) keys = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autofree gchar *digest = NULL;
	gint64 window_from;
	gint64 window_until;
	guint max_writes;
	guint planned;
	guint writes;
	gboolean any;
	gboolean late;
	gboolean ok;
	guint i;

	ok = FALSE;
	writes = 0;

	if (!books_run_open(&run, context, query->organization_id, query->data_source_id, error))
		goto out;

	if (!query->dry_run && (BOOKS_MODE_DAILY != run.settings.mode))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Data source #%" G_GINT64_FORMAT " books %s, not daily: set its books "
		            "setting to daily to post its ledger a day at a time (a source books one "
		            "way, so nothing is booked twice)", run.source_id,
		            books_mode_names[run.settings.mode]);
		goto out;
	}

	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(),
	                                                     "location"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Posting an external ledger needs the places its money is held "
		                    "at, which the sales module keeps, and the sales module is off");
		goto out;
	}

	if (!books_load_records(&run, error) || !books_load_trades(&run, error))
		goto out;

	/* Nothing before books_from is the books', whatever is asked. */
	window_from = (NULL != query->from) ? books_day_of(g_date_time_to_unix(query->from))
	                                    : run.settings.from;

	if ((BOOKS_NONE != run.settings.from) && ((BOOKS_NONE == window_from) ||
	                                         (window_from < run.settings.from)))
		window_from = run.settings.from;
	window_until = (NULL != query->until) ? g_date_time_to_unix(query->until)
	                                      : books_day_of(run.now);

	if (BOOKS_NONE != run.horizon)
	{
		g_autofree gchar *kept = books_date(run.horizon);

		books_note(&run, "Days before %s are kept as they were posted: the store keeps only "
		           "so much history (series.daily_days), so their rows may be gone", kept);
	}

	/* --- Which accounts: the store's, and any the books hold that it
	 * no longer lists --- */

	store_accounts = venture_series_store_list_accounts(run.store, NULL, NULL, run.now, error);

	if (NULL == store_accounts)
		goto out;

	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	for (i = 0; i < store_accounts->len; i++)
	{
		VentureSeriesAccountRow *row = g_ptr_array_index(store_accounts, i);

		g_hash_table_replace(seen, g_strdup(row->key), g_strdup(row->name));
	}

	{
		GHashTableIter iter;
		gpointer key;

		g_hash_table_iter_init(&iter, run.records_by_account);

		while (g_hash_table_iter_next(&iter, &key, NULL))
			if (!g_hash_table_contains(seen, key))
				g_hash_table_replace(seen, g_strdup(key), NULL);
	}

	keys = g_ptr_array_new();

	{
		GHashTableIter iter;
		gpointer key;

		g_hash_table_iter_init(&iter, seen);

		while (g_hash_table_iter_next(&iter, &key, NULL))
			if ((NULL == query->account_key) || (0 == g_strcmp0(key, query->account_key)))
				g_ptr_array_add(keys, key);
	}

	g_ptr_array_sort(keys, books_compare_keys);

	if ((NULL != query->account_key) && (0 == keys->len))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Data source #%" G_GINT64_FORMAT " has no account %s", run.source_id,
		            query->account_key);
		goto out;
	}

	/* --- The plan --- */

	accounts = g_ptr_array_new_with_free_func(books_account_free);

	for (i = 0; i < keys->len; i++)
	{
		const gchar *key = g_ptr_array_index(keys, i);
		BooksAccount *account = NULL;

		if (!books_plan_account(&run, key, g_hash_table_lookup(seen, key), window_from,
		                        window_until, &account, error))
		{
			if (NULL != account)
				books_account_free(account);
			goto out;
		}

		if (account->no_place)
			books_note(&run, "%s's place was deleted, so nothing of it is posted; restore the "
			           "location to post it", account->name);

		g_ptr_array_add(accounts, account);
	}

	/* --- The bound: whole accounts, at least one --- */

	max_writes = (query->max_writes > 0) ? query->max_writes : run.settings.max_writes;
	planned = 0;
	late = FALSE;

	for (i = 0; i < accounts->len; i++)
	{
		BooksAccount *account = g_ptr_array_index(accounts, i);
		guint j;

		if (0 == account->writes)
			continue;

		if (!late && ((0 == planned) || (planned + account->writes <= max_writes)))
		{
			planned += account->writes;
			continue;
		}

		late = TRUE;

		for (j = 0; j < account->items->len; j++)
		{
			BooksItem *item = g_ptr_array_index(account->items, j);

			if (books_status_writes(item->status))
				item->status = BOOKS_STATUS_LATER;
		}
	}

	if (late)
		books_note(&run, "More days than one pass writes (books_max_writes %u): the rest are "
		           "left for the next", max_writes);

	any = FALSE;

	for (i = 0; i < accounts->len; i++)
	{
		BooksAccount *account = g_ptr_array_index(accounts, i);
		guint j;

		for (j = 0; j < account->items->len; j++)
			if (books_status_writes(((BooksItem *)g_ptr_array_index(account->items, j))->status))
				any = TRUE;
	}

	/* --- The writing: one operation, one transaction --- */

	if (!query->dry_run && any)
	{
		run.journals = venture_posting_service_find_source(run.posting, "data_source", run.source_id,
		                                                   run.org, error);

		if (NULL == run.journals)
			goto out;

		digest = books_post_digest(accounts);
		operation = venture_accounting_operation_begin(run.database, "external_ledger.post",
			run.source, NULL, g_variant_new("(sss)", run.uuid, digest,
			(NULL != query->account_key) ? query->account_key : ""), run.org, actor, error);

		if (NULL == operation)
			goto out;

		if (!venture_database_begin(run.database, error))
			goto out;

		for (i = 0; i < accounts->len; i++)
		{
			if (!books_execute_account(&run, g_ptr_array_index(accounts, i), actor, error))
			{
				venture_database_rollback(run.database);
				goto out;
			}
		}

		if (!venture_database_commit(run.database, error) ||
		    !venture_accounting_operation_finish(operation, error))
			goto out;

		writes = planned;
	}

	if (NULL != out_report)
		*out_report = books_post_report(&run, accounts, query->dry_run, window_from, window_until,
		                                listed_from, listed_until, cap, writes);

	ok = TRUE;

out:
	books_run_clear(&run);

	return ok;
}

#endif /* VENTURE_HAVE_SQLITE */

gboolean
venture_arbitrage_books_post(
	VentureContext			 *context,
	const VentureBooksPostQuery	 *query,
	const VentureActor		 *actor,
	JsonNode			**out_report,
	GError				**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(NULL != query, FALSE);

#ifdef VENTURE_HAVE_SQLITE
	return books_post_internal(context, query, actor, BOOKS_NONE, BOOKS_NONE, BOOKS_LISTED, out_report,
	                           error);
#else
	(void)actor;
	(void)out_report;

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
	                    "An external ledger lives in a series store, which this build (without "
	                    "SQLite) does not have");
	return FALSE;
#endif
}

#ifdef VENTURE_HAVE_SQLITE

/* ==========================================================================
 * Trades: the flips
 * ========================================================================== */

typedef struct
{
	gchar		*instrument_key;
	gchar		*instrument_name;
	gchar		*sale_key;
	gchar		*sale_account;
	gchar		*sale_venue;
	gint64		 sale_at;
	gint64		 units;
	gint64		 cost;
	gint64		 proceeds;
	gboolean	 other_currency;
	GPtrArray	*buys;		/* VentureSeriesFlipPair, borrowed */
	const gchar	*skip;
	gchar		*external_ref;
	gint64		 trade_id;
	gint64		 capital;
} BooksFlip;

static void
books_flip_free(gpointer data)
{
	BooksFlip *flip = data;

	g_free(flip->instrument_key);
	g_free(flip->instrument_name);
	g_free(flip->sale_key);
	g_free(flip->sale_account);
	g_free(flip->sale_venue);
	g_ptr_array_unref(flip->buys);
	g_free(flip->external_ref);
	g_free(flip);
}

static gint
books_compare_flips(
	gconstpointer	a,
	gconstpointer	b
){
	const BooksFlip *one = *(BooksFlip *const *)a;
	const BooksFlip *two = *(BooksFlip *const *)b;

	if (one->sale_at != two->sale_at)
		return (one->sale_at < two->sale_at) ? -1 : 1;

	return g_strcmp0(one->sale_key, two->sale_key);
}

/* Ids already promoted in this call, by key; -1 for none. */
typedef struct
{
	GHashTable	*venues;
	GHashTable	*instruments;
	GHashTable	*locations;
} BooksIds;

static void
books_ids_clear(BooksIds *ids)
{
	g_clear_pointer(&ids->venues, g_hash_table_unref);
	g_clear_pointer(&ids->instruments, g_hash_table_unref);
	g_clear_pointer(&ids->locations, g_hash_table_unref);
}

static gboolean
books_cached_id(
	GHashTable	*cache,
	const gchar	*key,
	gint64		*out
){
	gpointer value;

	if (!g_hash_table_lookup_extended(cache, key, NULL, &value))
		return FALSE;

	*out = (gint64)GPOINTER_TO_SIZE(value);

	return TRUE;
}

static void
books_cache_id(
	GHashTable	*cache,
	const gchar	*key,
	gint64		 id
){
	g_hash_table_replace(cache, g_strdup(key), GSIZE_TO_POINTER((gsize)MAX((gint64)0, id)));
}

/*
 * A venue or an instrument the store knows, as a record: found or made by
 * the public promotion (an explicit action, so a deleted one is brought
 * back, as the scan's Record does). One the store does not know -- a row
 * that names a venue it never described -- is 0, not an error.
 */
static gboolean
books_promoted_id(
	BooksRun		 *run,
	GHashTable		 *cache,
	gboolean		  venue,
	const gchar		 *key,
	const VentureActor	 *actor,
	gint64			 *out,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) local_error = NULL;
	gboolean ok;

	*out = 0;

	if (venture_string_is_empty(key) || books_cached_id(cache, key, out))
		return TRUE;

	ok = venue ? venture_marketdata_promote_venue(run->context, run->org, run->source_id, key, actor,
	                                              &record, &local_error)
	           : venture_marketdata_promote_instrument(run->context, run->org, run->source_id, key,
	                                                   actor, &record, &local_error);

	if (!ok)
	{
		if (!g_error_matches(local_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
		{
			g_propagate_error(error, g_steal_pointer(&local_error));
			return FALSE;
		}

		books_cache_id(cache, key, 0);
		return TRUE;
	}

	*out = venture_entity_get_id(record);
	books_cache_id(cache, key, *out);

	return TRUE;
}

static gboolean
books_location_id(
	BooksRun		 *run,
	BooksIds		 *ids,
	const gchar		 *account_key,
	const VentureActor	 *actor,
	gint64			 *out,
	GError			**error
){
	*out = 0;

	if (venture_string_is_empty(account_key) || books_cached_id(ids->locations, account_key, out))
		return TRUE;

	if (!books_ensure_location(run, account_key, actor, out, error))
		return FALSE;

	books_cache_id(ids->locations, account_key, *out);

	return TRUE;
}

static gchar *
books_money_text(
	BooksRun	*run,
	gint64		 minor
){
	g_autoptr(VentureMoney) money = venture_money_new_for_currency(minor, run->currency);

	return venture_money_to_string(money);
}

/* A leg of the recorded trade, in the record action's wire spelling. */
static JsonObject *
books_leg_json(
	BooksRun	*run,
	const gchar	*kind,
	gint64		 venue_id,
	gint64		 location_id,
	gint64		 instrument_id,
	gint64		 units,
	gint64		 amount,
	gint64		 at,
	const gchar	*notes
){
	JsonObject *leg = json_object_new();
	g_autofree gchar *money = books_money_text(run, amount);
	g_autofree gchar *when = books_iso(at);

	json_object_set_string_member(leg, "kind", kind);

	if (venue_id > 0)
		json_object_set_int_member(leg, "venue_id", venue_id);

	if (location_id > 0)
		json_object_set_int_member(leg, "location_id", location_id);

	if (instrument_id > 0)
		json_object_set_int_member(leg, "instrument_id", instrument_id);

	json_object_set_int_member(leg, "quantity", units);
	json_object_set_string_member(leg, "amount", money);
	json_object_set_string_member(leg, "occurred_at", when);
	json_object_set_string_member(leg, "notes", notes);

	return leg;
}

/*
 * The trade's snapshot: the profit (what the report compares realised
 * with, so a recorded flip has no slippage) and where every unit came
 * from -- which is what the next run takes off before matching.
 */
static JsonObject *
books_flip_expected(
	BooksRun	*run,
	BooksFlip	*flip
){
	JsonObject *root = json_object_new();
	JsonObject *source = json_object_new();
	JsonObject *sale = json_object_new();
	JsonArray *profit = json_array_new();
	JsonArray *buys = json_array_new();
	g_autofree gchar *made = books_money_text(run, flip->proceeds - flip->cost);
	guint i;

	json_array_add_string_element(profit, made);
	json_object_set_array_member(root, "profit", profit);
	json_object_set_int_member(source, "data_source_id", run->source_id);
	json_object_set_string_member(source, "instrument", flip->instrument_key);
	json_object_set_string_member(sale, "key", flip->sale_key);
	json_object_set_string_member(sale, "account", (NULL != flip->sale_account) ? flip->sale_account : "");
	json_object_set_int_member(sale, "at", flip->sale_at);
	json_object_set_int_member(sale, "units", flip->units);
	json_object_set_int_member(sale, "proceeds", flip->proceeds);
	json_object_set_object_member(source, "sale", sale);

	for (i = 0; i < flip->buys->len; i++)
	{
		VentureSeriesFlipPair *pair = g_ptr_array_index(flip->buys, i);
		JsonObject *buy = json_object_new();

		json_object_set_string_member(buy, "key", pair->buy_key);
		json_object_set_string_member(buy, "account", (NULL != pair->buy_account) ? pair->buy_account : "");
		json_object_set_int_member(buy, "at", pair->buy_at);
		json_object_set_int_member(buy, "units", pair->units);
		json_object_set_int_member(buy, "cost", pair->cost);
		json_array_add_object_element(buys, buy);
	}

	json_object_set_array_member(source, "buys", buys);
	json_object_set_object_member(root, "source", source);

	return root;
}

/*
 * Brings money the ledger never showed arriving into the purse a buy leg
 * spends from, at the buy's time, when the floor would otherwise refuse
 * the leg: Dr the purse, Cr trading capital. See the file's comment.
 */
static gboolean
books_fund_leg(
	BooksRun		 *run,
	BooksFlip		 *flip,
	VentureEntity		 *leg,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) delta = NULL;
	g_autoptr(VentureMoney) shortfall = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(VentureJournal) header = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *memo = NULL;
	gint64 location_id;
	gint64 cash;
	gint64 equity;

	g_object_get(leg, "amount", &amount, "occurred-at", &when, NULL);

	if ((NULL == amount) || venture_money_is_zero(amount) || (NULL == when))
		return TRUE;

	location_id = books_int(leg, "location-id");
	cash = 0;

	if (location_id > 0)
	{
		if (!venture_holdings_account_for_location(run->database, run->org, location_id, TRUE, actor,
		                                           &cash, error))
			return FALSE;
	}
	else if (!venture_arbitrage_venue_cash_account(run->database, run->org, books_int(leg, "venue-id"),
	                                               when, actor, &cash, error))
		return FALSE;

	delta = venture_money_negate(amount);

	if (!venture_holdings_floor_shortfall(run->database, cash, delta, when, &shortfall, error))
		return FALSE;

	if (venture_money_is_zero(shortfall))
		return TRUE;

	equity = books_account(run, "trading_capital", when, actor, error);

	if (equity <= 0)
		return FALSE;

	lines = g_ptr_array_new_with_free_func(g_object_unref);
	memo = g_strdup_printf("Money the ledger does not show arriving, spent by %s", flip->external_ref);
	books_line(run, lines, cash, VENTURE_LEDGER_SIDE_DEBIT, venture_money_get_amount(shortfall), memo);
	books_line(run, lines, equity, VENTURE_LEDGER_SIDE_CREDIT, venture_money_get_amount(shortfall),
	           "Trading capital");
	key = g_strdup_printf("extflip:%s:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, run->uuid,
	                      flip->trade_id, venture_entity_get_id(leg));
	header = venture_journal_new();
	g_object_set(header, "organization-id", run->org, "occurred-at", when, "currency", run->currency,
	             "source-type", "data_source", "source-id", run->source_id, "memo", memo,
	             "rule-name", VENTURE_ARBITRAGE_BOOKS_CAPITAL_RULE, "posting-key", key, NULL);
	journals = venture_posting_service_post_by_currency_full(run->posting, header, lines, FALSE, actor,
	                                                         NULL, error);

	if (NULL == journals)
		return FALSE;

	flip->capital += venture_money_get_amount(shortfall);

	return TRUE;
}

/*
 * Records one flip inside the caller's transaction: the trade with its
 * legs planned, then each leg executed in time order -- every buy after
 * its purse is funded when the ledger cannot show the money arriving --
 * and the trade closed at the sale.
 */
static gboolean
books_record_flip(
	BooksRun		 *run,
	BooksIds		 *ids,
	BooksFlip		 *flip,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(JsonObject) request = NULL;
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	g_autoptr(GDateTime) closed = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *notes = NULL;
	g_autofree gchar *source_name = NULL;
	JsonArray *legs_json;
	gint64 instrument_id;
	gint64 venue_id;
	gint64 location_id;
	guint i;

	if (!books_promoted_id(run, ids->instruments, FALSE, flip->instrument_key, actor, &instrument_id,
	                       error))
		return FALSE;

	request = json_object_new();
	legs_json = json_array_new();

	for (i = 0; i < flip->buys->len; i++)
	{
		VentureSeriesFlipPair *pair = g_ptr_array_index(flip->buys, i);
		g_autofree gchar *leg_notes = g_strdup_printf("Bought: ledger row %s", pair->buy_key);

		if (!books_promoted_id(run, ids->venues, TRUE, pair->buy_venue, actor, &venue_id, error) ||
		    !books_location_id(run, ids, pair->buy_account, actor, &location_id, error))
			return FALSE;

		json_array_add_object_element(legs_json, books_leg_json(run, "buy", venue_id, location_id,
		                                                        instrument_id, pair->units, pair->cost,
		                                                        pair->buy_at, leg_notes));
	}

	{
		g_autofree gchar *leg_notes = g_strdup_printf("Sold: ledger row %s", flip->sale_key);

		if (!books_promoted_id(run, ids->venues, TRUE, flip->sale_venue, actor, &venue_id, error) ||
		    !books_location_id(run, ids, flip->sale_account, actor, &location_id, error))
			return FALSE;

		json_array_add_object_element(legs_json, books_leg_json(run, "sell", venue_id, location_id,
		                                                        instrument_id, flip->units,
		                                                        flip->proceeds, flip->sale_at,
		                                                        leg_notes));
	}

	name = g_strdup_printf("Flip: %s x%" G_GINT64_FORMAT,
	                       !venture_string_is_empty(flip->instrument_name) ? flip->instrument_name
	                                                                       : flip->instrument_key,
	                       flip->units);
	source_name = books_string(run->source, "name");
	notes = g_strdup_printf("Recorded from the ledger of %s (sale %s)",
	                        (NULL != source_name) ? source_name : "a data source", flip->sale_key);
	json_object_set_string_member(request, "name", name);
	json_object_set_string_member(request, "strategy", "flip");
	json_object_set_object_member(request, "expected", books_flip_expected(run, flip));
	json_object_set_string_member(request, "notes", notes);
	json_object_set_array_member(request, "legs", legs_json);

	if (!venture_arbitrage_record_in_transaction(run->database, run->org, request, run->source_id,
	                                             flip->external_ref, actor, &trade, error))
		return FALSE;

	flip->trade_id = venture_entity_get_id(trade);
	legs = venture_arbitrage_trade_legs(run->database, flip->trade_id, FALSE, error);

	if (NULL == legs)
		return FALSE;

	/* The legs were made in time order -- buys, then the sale -- and are
	 * executed in it, so each purse is judged as it stood then. */
	for (i = 0; i < legs->len; i++)
	{
		VentureEntity *leg = g_ptr_array_index(legs, i);
		gint kind = 0;

		g_object_get(leg, "kind", &kind, NULL);

		if ((VENTURE_ARBITRAGE_LEG_KIND_BUY == kind) && !books_fund_leg(run, flip, leg, actor, error))
			return FALSE;

		if (!venture_arbitrage_execute_in_transaction(run->database, venture_entity_get_id(leg), NULL,
		                                              actor, NULL, error))
			return FALSE;
	}

	closed = g_date_time_new_from_unix_utc(flip->sale_at);

	return venture_arbitrage_close_in_transaction(run->database, flip->trade_id, closed, actor, NULL,
	                                              error);
}

/* Turns one instrument's pairs (in the walk's order: by sale) into flips,
 * one per sale. The pairs stay owned by @pairs. */
static void
books_group_pairs(
	BooksRun	*run,
	GPtrArray	*pairs,
	gint64		 from,
	GPtrArray	*flips
){
	BooksFlip *flip;
	guint i;

	flip = NULL;

	for (i = 0; i < pairs->len; i++)
	{
		VentureSeriesFlipPair *pair = g_ptr_array_index(pairs, i);

		if ((NULL == flip) || (0 != g_strcmp0(flip->sale_key, pair->sale_key)) ||
		    (0 != g_strcmp0(flip->instrument_key, pair->instrument_key)))
		{
			/* A sale before the books start is not recorded, though its
			 * matching still took units from the lots before it. */
			if ((BOOKS_NONE != from) && (pair->sale_at < from))
			{
				flip = NULL;
				continue;
			}

			flip = g_new0(BooksFlip, 1);
			flip->instrument_key = g_strdup(pair->instrument_key);
			flip->instrument_name = g_strdup(pair->instrument_name);
			flip->sale_key = g_strdup(pair->sale_key);
			flip->sale_account = g_strdup(pair->sale_account);
			flip->sale_venue = g_strdup(pair->sale_venue);
			flip->sale_at = pair->sale_at;
			flip->other_currency = (0 != g_ascii_strcasecmp(pair->currency, run->currency));
			flip->buys = g_ptr_array_new();
			g_ptr_array_add(flips, flip);
		}

		g_ptr_array_add(flip->buys, pair);
		flip->units += pair->units;
		flip->cost += pair->cost;
		flip->proceeds += pair->proceeds;
	}
}

static gboolean
books_flips_internal(
	VentureContext			 *context,
	const VentureBooksFlipsQuery	 *query,
	const VentureActor		 *actor,
	JsonNode			**out_report,
	GError				**error
){
	BooksRun run;
	BooksIds ids;
	VentureSeriesTxnFilter filter;
	g_autoptr(GPtrArray) instruments = NULL;
	g_autoptr(GPtrArray) pair_sets = NULL;
	g_autoptr(GPtrArray) flips = NULL;
	g_autoptr(GPtrArray) chosen = NULL;
	g_autoptr(GHashTable) posted_days = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(GString) material = NULL;
	g_autofree gchar *digest = NULL;
	JsonObject *root;
	JsonObject *skipped;
	JsonArray *rows;
	gint64 from;
	gint64 until;
	gint64 units;
	gint64 cost;
	gint64 proceeds;
	gint64 capital;
	guint limit;
	guint more;
	guint skip_posted;
	guint skip_profit;
	guint skip_buys;
	guint skip_currency;
	guint skip_rows;
	gboolean ok;
	guint i;

	ok = FALSE;
	memset(&ids, 0, sizeof(ids));

	if (!books_run_open(&run, context, query->organization_id, query->data_source_id, error))
		goto out;

	if (!query->dry_run && (BOOKS_MODE_TRADES != run.settings.mode))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Data source #%" G_GINT64_FORMAT " books %s, not trades: set its books "
		            "setting to trades to record its flips as arbitrage trades (a source books "
		            "one way, so nothing is booked twice)", run.source_id,
		            books_mode_names[run.settings.mode]);
		goto out;
	}

	limit = (query->limit > 0) ? query->limit : VENTURE_ARBITRAGE_FLIPS_DEFAULT_LIMIT;

	if (limit > VENTURE_ARBITRAGE_FLIPS_MAX_LIMIT)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "limit is at most %d trades a call", VENTURE_ARBITRAGE_FLIPS_MAX_LIMIT);
		goto out;
	}

	if ((NULL != query->min_profit) &&
	    (0 != g_ascii_strcasecmp(venture_money_get_currency(query->min_profit), run.currency)))
	{
		venture_set_error_validation(error, "min_profit", "is in %s and the source's ledger is in %s",
		                             venture_money_get_currency(query->min_profit), run.currency);
		goto out;
	}

	if (!books_load_records(&run, error) || !books_load_trades(&run, error))
		goto out;

	/* Nothing before the books start: from is never earlier than
	 * books_from, and matching starts there too. */
	from = (NULL != query->from) ? g_date_time_to_unix(query->from) : BOOKS_NONE;

	if ((BOOKS_NONE != run.settings.from) && ((BOOKS_NONE == from) || (from < run.settings.from)))
		from = run.settings.from;

	until = (NULL != query->until) ? g_date_time_to_unix(query->until) : run.now + 1;

	/* Days already posted a day at a time are booked; a flip touching one
	 * would book its sale or its buy a second time. */
	posted_days = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	{
		GHashTableIter iter;
		gpointer value;

		g_hash_table_iter_init(&iter, run.records);

		while (g_hash_table_iter_next(&iter, NULL, &value))
		{
			VentureEntity *record = value;
			g_autoptr(GDateTime) when = NULL;
			g_autofree gchar *key = NULL;

			if ((VENTURE_EXTERNAL_POSTING_KIND_DAY != books_enum(record, "kind")) ||
			    !books_record_has_money(record))
				continue;

			g_object_get(record, "day", &when, NULL);
			key = books_string(record, "account-key");

			if (NULL != when)
				g_hash_table_add(posted_days, books_day_key(key, g_date_time_to_unix(when)));
		}
	}

	/* --- Which instruments sold in the window --- */

	instruments = g_ptr_array_new_with_free_func(g_free);

	if (NULL != query->instrument_key)
		g_ptr_array_add(instruments, g_strdup(query->instrument_key));
	else
	{
		g_autoptr(GPtrArray) totals = NULL;

		venture_series_txn_filter_init(&filter);
		filter.kind = "sale";
		filter.since = (BOOKS_NONE != from) ? from : VENTURE_SERIES_NONE;
		filter.until = until;
		totals = venture_series_store_txn_totals(run.store, &filter, VENTURE_SERIES_TXN_GROUP_INSTRUMENT,
		                                         error);

		if (NULL == totals)
			goto out;

		for (i = 0; i < totals->len; i++)
		{
			VentureSeriesTxnTotal *bucket = g_ptr_array_index(totals, i);

			if (!venture_string_is_empty(bucket->key))
				g_ptr_array_add(instruments, g_strdup(bucket->key));
		}

		g_ptr_array_sort(instruments, books_compare_keys);
	}

	/* --- Each instrument's walk, from the start of the books --- */

	pair_sets = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
	flips = g_ptr_array_new_with_free_func(books_flip_free);
	skip_rows = 0;

	for (i = 0; i < instruments->len; i++)
	{
		g_autoptr(GError) local_error = NULL;
		GPtrArray *pairs;

		venture_series_txn_filter_init(&filter);
		filter.instrument_key = g_ptr_array_index(instruments, i);
		filter.since = (BOOKS_NONE != run.settings.from) ? run.settings.from : VENTURE_SERIES_NONE;
		filter.until = until;
		pairs = venture_series_store_flip_pairs(run.store, &filter, run.used, &local_error);

		if (NULL == pairs)
		{
			if (!g_error_matches(local_error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT))
			{
				g_propagate_error(error, g_steal_pointer(&local_error));
				goto out;
			}

			skip_rows++;
			continue;
		}

		g_ptr_array_add(pair_sets, pairs);
		books_group_pairs(&run, pairs, from, flips);
	}

	if (skip_rows > 0)
		books_note(&run, "%u item(s) have more buys and sales than one walk reads; set books_from "
		           "to start the books later", skip_rows);

	/* --- What is recorded, in time order --- */

	g_ptr_array_sort(flips, books_compare_flips);
	chosen = g_ptr_array_new();
	skip_posted = 0;
	skip_profit = 0;
	skip_buys = 0;
	skip_currency = 0;
	more = 0;

	for (i = 0; i < flips->len; i++)
	{
		BooksFlip *flip = g_ptr_array_index(flips, i);
		g_autofree gchar *sale_day = books_day_key(flip->sale_account, flip->sale_at);
		gboolean touches_posted;
		guint j;

		touches_posted = g_hash_table_contains(posted_days, sale_day);

		for (j = 0; !touches_posted && (j < flip->buys->len); j++)
		{
			VentureSeriesFlipPair *pair = g_ptr_array_index(flip->buys, j);
			g_autofree gchar *buy_day = books_day_key(pair->buy_account, pair->buy_at);

			touches_posted = g_hash_table_contains(posted_days, buy_day);
		}

		if (flip->other_currency)
		{
			flip->skip = "other_currency";
			skip_currency++;
		}
		else if (touches_posted)
		{
			flip->skip = "posted_days";
			skip_posted++;
		}
		else if ((NULL != query->min_profit) &&
		         (flip->proceeds - flip->cost < venture_money_get_amount(query->min_profit)))
		{
			flip->skip = "below_min_profit";
			skip_profit++;
		}
		else if (flip->buys->len >= VENTURE_ARBITRAGE_MAX_LEGS)
		{
			flip->skip = "too_many_buys";
			skip_buys++;
		}
		else if (chosen->len >= limit)
		{
			flip->skip = "later";
			more++;
		}

		if (NULL != flip->skip)
			continue;

		flip->external_ref = g_strdup_printf("%s:%s:%u", run.uuid, flip->sale_key,
		                                     GPOINTER_TO_UINT(g_hash_table_lookup(run.sale_trades,
		                                                                          flip->sale_key)) + 1);
		g_ptr_array_add(chosen, flip);
	}

	if (skip_posted > 0)
		books_note(&run, "%u flip(s) touch a day already posted a day at a time and are left out, "
		           "or they would be booked twice", skip_posted);

	if (skip_buys > 0)
		books_note(&run, "%u sale(s) drew on more buys than one trade holds legs (%d) and are left out",
		           skip_buys, VENTURE_ARBITRAGE_MAX_LEGS - 1);

	if (more > 0)
		books_note(&run, "%u more flip(s) are left for the next call (limit %u)", more, limit);

	/* --- The writing --- */

	if (!query->dry_run && (chosen->len > 0))
	{
		material = g_string_new("venture-external-flips-v1");

		for (i = 0; i < chosen->len; i++)
		{
			BooksFlip *flip = g_ptr_array_index(chosen, i);

			g_string_append_printf(material, "\n%s|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%"
			                       G_GINT64_FORMAT, flip->external_ref, flip->units, flip->cost,
			                       flip->proceeds);
		}

		digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, material->str, -1);
		operation = venture_accounting_operation_begin(run.database, "external_ledger.flips",
			run.source, NULL, g_variant_new("(ss)", run.uuid, digest), run.org, actor, error);

		if (NULL == operation)
			goto out;

		if (!venture_database_begin(run.database, error))
			goto out;

		ids.venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		ids.instruments = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		ids.locations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

		for (i = 0; i < chosen->len; i++)
		{
			BooksFlip *flip = g_ptr_array_index(chosen, i);

			if (!books_record_flip(&run, &ids, flip, actor, error))
			{
				g_prefix_error(error, "Sale %s: ", flip->sale_key);
				venture_database_rollback(run.database);
				goto out;
			}
		}

		if (!venture_database_commit(run.database, error) ||
		    !venture_accounting_operation_finish(operation, error))
			goto out;
	}

	/* --- The answer --- */

	if (NULL != out_report)
	{
		JsonNode *node;

		root = json_object_new();
		rows = json_array_new();
		units = 0;
		cost = 0;
		proceeds = 0;
		capital = 0;

		for (i = 0; i < chosen->len; i++)
		{
			BooksFlip *flip = g_ptr_array_index(chosen, i);
			g_autofree gchar *when = books_iso(flip->sale_at);
			JsonObject *row;

			units += flip->units;
			cost += flip->cost;
			proceeds += flip->proceeds;
			capital += flip->capital;

			if (i >= BOOKS_LISTED)
				continue;

			row = json_object_new();
			json_object_set_string_member(row, "external_ref", flip->external_ref);

			if (flip->trade_id > 0)
				json_object_set_int_member(row, "trade_id", flip->trade_id);
			else
				json_object_set_null_member(row, "trade_id");

			json_object_set_string_member(row, "instrument_key", flip->instrument_key);
			json_object_set_string_member(row, "instrument",
			                              (NULL != flip->instrument_name) ? flip->instrument_name
			                                                              : flip->instrument_key);
			json_object_set_string_member(row, "sale_key", flip->sale_key);
			json_object_set_string_member(row, "sale_account",
			                              (NULL != flip->sale_account) ? flip->sale_account : "");
			json_object_set_string_member(row, "sale_at", when);
			json_object_set_int_member(row, "units", flip->units);
			json_object_set_int_member(row, "buys", flip->buys->len);
			json_object_set_member(row, "cost", books_money_json(&run, flip->cost));
			json_object_set_member(row, "proceeds", books_money_json(&run, flip->proceeds));
			json_object_set_member(row, "profit", books_money_json(&run, flip->proceeds - flip->cost));
			json_object_set_member(row, "capital", books_money_json(&run, flip->capital));
			json_array_add_object_element(rows, row);
		}

		json_object_set_int_member(root, "data_source_id", run.source_id);
		json_object_set_string_member(root, "currency", run.currency);
		json_object_set_string_member(root, "books", books_mode_names[run.settings.mode]);
		json_object_set_boolean_member(root, "dry_run", query->dry_run);
		books_set_date(root, "from", from);
		json_object_set_int_member(root, "recorded", query->dry_run ? 0 : chosen->len);
		json_object_set_int_member(root, "flips", chosen->len);
		json_object_set_int_member(root, "already_recorded", run.trades);
		json_object_set_int_member(root, "units", units);
		json_object_set_member(root, "cost", books_money_json(&run, cost));
		json_object_set_member(root, "proceeds", books_money_json(&run, proceeds));
		json_object_set_member(root, "profit", books_money_json(&run, proceeds - cost));
		json_object_set_member(root, "capital", books_money_json(&run, capital));
		json_object_set_int_member(root, "more", more);
		skipped = json_object_new();
		json_object_set_int_member(skipped, "posted_days", skip_posted);
		json_object_set_int_member(skipped, "below_min_profit", skip_profit);
		json_object_set_int_member(skipped, "too_many_buys", skip_buys);
		json_object_set_int_member(skipped, "other_currency", skip_currency);
		json_object_set_int_member(skipped, "too_many_rows", skip_rows);
		json_object_set_object_member(root, "skipped", skipped);
		json_object_set_array_member(root, "trades", rows);
		books_notes_json(&run, root);
		node = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(node, root);
		*out_report = node;
	}

	ok = TRUE;

out:
	books_ids_clear(&ids);
	books_run_clear(&run);

	return ok;
}

#endif /* VENTURE_HAVE_SQLITE */

gboolean
venture_arbitrage_books_record_flips(
	VentureContext			 *context,
	const VentureBooksFlipsQuery	 *query,
	const VentureActor		 *actor,
	JsonNode			**out_report,
	GError				**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(NULL != query, FALSE);

#ifdef VENTURE_HAVE_SQLITE
	return books_flips_internal(context, query, actor, out_report, error);
#else
	(void)actor;
	(void)out_report;

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
	                    "An external ledger lives in a series store, which this build (without "
	                    "SQLite) does not have");
	return FALSE;
#endif
}

/* ==========================================================================
 * The external_books report
 * ========================================================================== */

#ifdef VENTURE_HAVE_SQLITE

static void
books_report_money(
	VentureReportResult	*result,
	const gchar		*column,
	JsonObject		*row,
	const gchar		*member
){
	g_autoptr(VentureMoney) money = NULL;
	JsonNode *node;

	node = json_object_get_member(row, member);

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return;

	money = venture_money_from_json(node, NULL, NULL);

	if ((NULL != money) && !venture_money_is_zero(money))
		venture_report_result_set_money(result, column, money);
}

#endif /* VENTURE_HAVE_SQLITE */

VentureReportResult *
venture_arbitrage_books_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
#ifdef VENTURE_HAVE_SQLITE
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) report = NULL;
	VentureBooksPostQuery query;
	GDateTime *start;
	GDateTime *end;
	JsonObject *root;
	JsonObject *counts;
	JsonArray *days;
	gint64 listed_from;
	gint64 listed_until;
	guint i;

	memset(&query, 0, sizeof(query));
	query.organization_id = (NULL != options) ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	query.data_source_id = (NULL != options) ? venture_json_object_get_int(options, "data_source_id", 0) : 0;
	query.account_key = (NULL != options) ? venture_json_object_get_string(options, "account_key", NULL)
	                                      : NULL;

	if (venture_string_is_empty(query.account_key))
		query.account_key = NULL;

	if (query.data_source_id <= 0)
	{
		venture_set_error_validation(error, "data_source_id",
		                             "names the data source whose ledger to show");
		return NULL;
	}

	start = (NULL != period) ? venture_date_range_get_start(period) : NULL;
	end = (NULL != period) ? venture_date_range_get_end(period) : NULL;
	query.from = start;
	query.until = end;
	query.dry_run = TRUE;
	listed_from = (NULL != start) ? books_day_of(g_date_time_to_unix(start)) : BOOKS_NONE;
	listed_until = (NULL != end) ? g_date_time_to_unix(end) : BOOKS_NONE;

	if (!books_post_internal(context, &query, NULL, listed_from, listed_until, 0, &report, error))
		return NULL;

	root = json_node_get_object(report);
	result = venture_report_result_new("External ledger in the books", period);
	venture_report_result_add_column(result, "account", "Account", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "day", "Day", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "In the books", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "sales", "Sales", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "purchases", "Purchases", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "income", "Other income", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "expenses", "Expenses", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "capital", "Capital", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "net", "Net", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "journal", "Journal", VENTURE_REPORT_COLUMN_NUMBER);
	days = json_object_get_array_member(root, "days");

	for (i = 0; i < json_array_get_length(days); i++)
	{
		JsonObject *row = json_array_get_object_element(days, i);
		const gchar *kind = venture_json_object_get_string(row, "kind", "day");
		g_autofree gchar *label = NULL;

		label = (0 == g_strcmp0(kind, "opening"))
			? g_strdup_printf("%s (opening)", venture_json_object_get_string(row, "day", ""))
			: g_strdup(venture_json_object_get_string(row, "day", ""));
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "account", venture_json_object_get_string(row, "account", ""));
		venture_report_result_set_text(result, "day", label);
		venture_report_result_set_text(result, "status", venture_json_object_get_string(row, "status", ""));
		books_report_money(result, "sales", row, "sales");
		books_report_money(result, "purchases", row, "purchases");
		books_report_money(result, "income", row, "income");
		books_report_money(result, "expenses", row, "expenses");
		books_report_money(result, "capital", row, "capital");
		books_report_money(result, "net", row, "net");

		if (venture_json_object_get_int(row, "journal_id", 0) > 0)
			venture_report_result_set_number(result, "journal",
			                                 (gdouble)venture_json_object_get_int(row, "journal_id", 0));
	}

	counts = json_object_get_object_member(root, "days_by_status");

	{
		g_autofree gchar *summary = g_strdup_printf(
			"Books: %s. Posted %" G_GINT64_FORMAT ", not yet posted %" G_GINT64_FORMAT
			", changed since posting %" G_GINT64_FORMAT ", kept as posted %" G_GINT64_FORMAT
			", left out for recorded flips %" G_GINT64_FORMAT ", in a closed period %"
			G_GINT64_FORMAT " (the whole ledger, not only the period).",
			venture_json_object_get_string(root, "books", "none"),
			json_object_get_int_member_with_default(counts, "posted", 0),
			json_object_get_int_member_with_default(counts, "unposted", 0),
			json_object_get_int_member_with_default(counts, "changed", 0) +
			json_object_get_int_member_with_default(counts, "follows", 0),
			json_object_get_int_member_with_default(counts, "kept", 0),
			json_object_get_int_member_with_default(counts, "left_out", 0),
			json_object_get_int_member_with_default(counts, "closed", 0));
		JsonArray *notes = json_object_get_array_member(root, "notes");

		venture_report_result_append_note(result, summary);

		for (i = 0; (NULL != notes) && (i < json_array_get_length(notes)); i++)
			venture_report_result_append_note(result, json_array_get_string_element(notes, i));
	}

	return g_steal_pointer(&result);
#else
	(void)context;
	(void)period;
	(void)options;

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
	                    "An external ledger lives in a series store, which this build (without "
	                    "SQLite) does not have");
	return NULL;
#endif
}

/* ==========================================================================
 * Actions
 * ========================================================================== */

/* A typed time parameter, absent or empty being NULL. */
static gboolean
books_param_time(
	GHashTable	 *params,
	const gchar	 *name,
	GDateTime	**out,
	GError		**error
){
	JsonNode *node;

	*out = NULL;
	node = (NULL != params) ? g_hash_table_lookup(params, name) : NULL;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)) ||
	    venture_string_is_empty(json_node_get_string(node)))
		return TRUE;

	*out = venture_time_from_string(json_node_get_string(node), NULL);

	if (NULL == *out)
	{
		venture_set_error_validation(error, name, "must be a date, such as 2026-09-01");
		return FALSE;
	}

	return TRUE;
}

static const gchar *
books_param_string(
	GHashTable	*params,
	const gchar	*name
){
	JsonNode *node;

	node = (NULL != params) ? g_hash_table_lookup(params, name) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)) ||
	    venture_string_is_empty(json_node_get_string(node)))
		return NULL;

	return json_node_get_string(node);
}

static gboolean
books_param_bool(
	GHashTable	*params,
	const gchar	*name
){
	JsonNode *node;

	node = (NULL != params) ? g_hash_table_lookup(params, name) : NULL;

	return (NULL != node) && JSON_NODE_HOLDS_VALUE(node) &&
	       (G_TYPE_BOOLEAN == json_node_get_value_type(node)) && json_node_get_boolean(node);
}

static gint64
books_param_int(
	GHashTable	*params,
	const gchar	*name
){
	JsonNode *node;

	node = (NULL != params) ? g_hash_table_lookup(params, name) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_INT64 != json_node_get_value_type(node)))
		return 0;

	return json_node_get_int(node);
}

/*
 * The actions are type-level actions on the books' own types --
 * post_ledger on external_posting, record_flips on arbitrage_trade -- not
 * record actions on the data source. That is what makes the organization
 * role matrix judge them as financial work: those types belong to a
 * module that requires the ledger, so a finance member may run them and
 * an editor member may not. A data source is an administrator's record to
 * write, so an action on it would refuse the finance member as well.
 * Each names the source by data_source_id and runs in the organization
 * the placeholder was judged in (venture_action_prepare_target()).
 */
static gboolean
books_action_allowed(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureContext) context = NULL;

	(void)entity;
	(void)actor;

	context = books_context_for(VENTURE_DATABASE(venture_action_get_data(action)));

	if (NULL == context)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Posting an external ledger needs a running server");
		return FALSE;
	}

	return TRUE;
}

/*
 * The data source the action names, read under the caller's scope (a
 * member who may not read it is told it is not there), in the action's
 * organization.
 */
static VentureEntity *
books_action_source(
	VentureDatabase	 *database,
	VentureEntity	 *placeholder,
	GHashTable	 *params,
	gint64		 *out_organization_id,
	GError		**error
){
	g_autoptr(VentureEntity) source = NULL;
	gint64 source_id;
	gint64 organization_id;

	source_id = books_param_int(params, "data_source_id");
	organization_id = venture_entity_get_organization_id(placeholder);

	if (source_id <= 0)
	{
		venture_set_error_validation(error, "data_source_id", "names the data source whose ledger it is");
		return NULL;
	}

	source = venture_database_get(database, VENTURE_TYPE_DATA_SOURCE, source_id, NULL);

	if ((NULL == source) || venture_entity_is_deleted(source) ||
	    ((organization_id > 0) && (venture_entity_get_organization_id(source) != organization_id)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no data source #%" G_GINT64_FORMAT " in this organization", source_id);
		return NULL;
	}

	*out_organization_id = (organization_id > 0) ? organization_id
	                                             : venture_entity_get_organization_id(source);

	return g_steal_pointer(&source);
}

/* The answer: the data source, with the report's JSON as its one-time
 * `result` -- which a form's redirect lands on, and the API returns. */
static VentureEntity *
books_action_answer(
	VentureEntity	*source,
	JsonNode	*report
){
	g_autofree gchar *text = venture_json_to_string(report, FALSE);

	g_object_set(source, "result", text, NULL);

	return g_object_ref(source);
}

static VentureEntity *
books_post_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GDateTime) from = NULL;
	g_autoptr(GDateTime) until = NULL;
	g_autoptr(JsonNode) report = NULL;
	VentureBooksPostQuery query;
	VentureDatabase *database;
	gint64 organization_id;

	database = VENTURE_DATABASE(venture_action_get_data(action));
	context = books_context_for(database);

	if (NULL == context)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Posting an external ledger needs a running server");
		return NULL;
	}

	if (!books_param_time(params, "from", &from, error) ||
	    !books_param_time(params, "until", &until, error))
		return NULL;

	source = books_action_source(database, entity, params, &organization_id, error);

	if (NULL == source)
		return NULL;

	memset(&query, 0, sizeof(query));
	query.organization_id = organization_id;
	query.data_source_id = venture_entity_get_id(source);
	query.from = from;
	query.until = until;
	query.account_key = books_param_string(params, "account");
	query.dry_run = books_param_bool(params, "dry_run");

	if (!venture_arbitrage_books_post(context, &query, actor, &report, error))
		return NULL;

	return books_action_answer(source, report);
}

static VentureEntity *
books_flips_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GDateTime) from = NULL;
	g_autoptr(GDateTime) until = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(VentureMoney) min_profit = NULL;
	VentureBooksFlipsQuery query;
	VentureDatabase *database;
	const gchar *text;
	gint64 organization_id;
	gint64 limit;

	database = VENTURE_DATABASE(venture_action_get_data(action));
	context = books_context_for(database);

	if (NULL == context)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Recording flips needs a running server");
		return NULL;
	}

	if (!books_param_time(params, "from", &from, error) ||
	    !books_param_time(params, "until", &until, error))
		return NULL;

	text = books_param_string(params, "min_profit");

	if (NULL != text)
	{
		/* A bound must name its currency: a bare number reads as the
		 * install's default, which is never what a gold ledger means. */
		if (NULL == strchr(text, ' '))
		{
			venture_set_error_validation(error, "min_profit",
			                             "must name its currency, such as \"10.0000 GOLD\"");
			return NULL;
		}

		min_profit = venture_money_from_string(text, NULL, error);

		if (NULL == min_profit)
			return NULL;
	}

	limit = books_param_int(params, "limit");

	if ((limit < 0) || (limit > VENTURE_ARBITRAGE_FLIPS_MAX_LIMIT))
	{
		venture_set_error_validation(error, "limit", "is 1 to %d", VENTURE_ARBITRAGE_FLIPS_MAX_LIMIT);
		return NULL;
	}

	source = books_action_source(database, entity, params, &organization_id, error);

	if (NULL == source)
		return NULL;

	memset(&query, 0, sizeof(query));
	query.organization_id = organization_id;
	query.data_source_id = venture_entity_get_id(source);
	query.from = from;
	query.until = until;
	query.instrument_key = books_param_string(params, "instrument");
	query.min_profit = min_profit;
	query.limit = (guint)limit;
	query.dry_run = books_param_bool(params, "dry_run");

	if (!venture_arbitrage_books_record_flips(context, &query, actor, &report, error))
		return NULL;

	return books_action_answer(source, report);
}

static void
books_register(
	VentureDatabase		*database,
	const gchar		*type_name,
	const gchar		*name,
	const gchar		*label,
	const gchar		*description,
	GPtrArray		*parameters,
	VentureActionInvoke	 invoke
){
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;

	/*
	 * Type-level, judged in the organization it names (see above).
	 * Stageable: approval runs the pass afresh against the ledger then.
	 * The service owns the transaction: it begins the posting boundary
	 * first, as the boundary requires.
	 */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", type_name, "name", name, "label", label, "description", description,
		"parameters", parameters, "stageable", TRUE, "type-level", TRUE,
		"service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);

	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
	                                      books_action_allowed, invoke, database, NULL, &error))
		g_error("External ledger action registration: %s", error->message);
}

/* The parameters both actions open with: where, and whose. */
static void
books_common_parameters(GPtrArray *parameters)
{
	VentureFieldSpec *field;

	field = venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER);
	field->help = g_strdup("The organization whose books these are; needed by a member");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("data_source_id", "Data source", VENTURE_FIELD_KIND_REFERENCE);
	field->help = g_strdup("The data source whose external ledger it is");
	field->reference_type = g_strdup("data_source");
	field->required = TRUE;
	g_ptr_array_add(parameters, field);
}

static void
books_register_actions(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = NULL;
	VentureFieldSpec *field;

	parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	books_common_parameters(parameters);
	field = venture_field_spec_new("from", "From", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("The first day new days are taken from, never before the source's "
	                       "books_from; the whole ledger by default");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("until", "Until", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("The day new days stop before; today, which is not over, by default");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("account", "Account", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("One account's key; every account when left empty");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("dry_run", "Dry run", VENTURE_FIELD_KIND_BOOLEAN);
	field->help = g_strdup("Say what would be posted and write nothing");
	g_ptr_array_add(parameters, field);
	books_register(database, "external_posting", "post_ledger", "Post a ledger to the books",
		"Post a data source's external ledger to the books: one journal per account per day, "
		"through each account's purse; a day already posted whose rows changed is reversed "
		"and posted again. The source must book daily",
		parameters, books_post_invoke);

	g_ptr_array_set_size(parameters, 0);
	books_common_parameters(parameters);
	field = venture_field_spec_new("from", "From", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("Only sales from this day; never before the source's books_from");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("until", "Until", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("Only sales before this; now by default");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("instrument", "Instrument", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("One instrument's key; every one that sold when left empty");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("min_profit", "Least profit", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("Only flips that made at least this, with its currency: \"10.0000 GOLD\"");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("limit", "Limit", VENTURE_FIELD_KIND_INTEGER);
	field->help = g_strdup("Trades to record at most, 1 to 500; 100 by default");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("dry_run", "Dry run", VENTURE_FIELD_KIND_BOOLEAN);
	field->help = g_strdup("Say what would be recorded and write nothing");
	g_ptr_array_add(parameters, field);
	books_register(database, "arbitrage_trade", "record_flips", "Record flips from a ledger",
		"Record each sale a data source's external ledger can match to earlier buys, first in "
		"first out, as a closed arbitrage trade with executed buy and sell legs, once. The "
		"source must book trades",
		parameters, books_flips_invoke);
}

/* ==========================================================================
 * After a run: post_to_books
 * ========================================================================== */

#ifdef VENTURE_HAVE_SQLITE

static gboolean
books_must_wait(VentureContext *context)
{
	return venture_automation_is_dispatching(venture_context_get_automation(context)) ||
	       venture_database_has_transaction(venture_context_get_database(context));
}

/* Appends @line to the run record's notes, as the system. */
static void
books_note_run(
	VentureContext	*context,
	gint64		 run_record_id,
	const gchar	*line
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *existing = NULL;
	g_autofree gchar *notes = NULL;
	VentureDatabase *database;
	VentureActor actor;

	database = venture_context_get_database(context);
	record = (run_record_id > 0)
		? venture_database_get(database, VENTURE_TYPE_DATA_SOURCE_RUN, run_record_id, NULL) : NULL;

	if ((NULL == record) || (NULL == line))
		return;

	g_object_get(record, "notes", &existing, NULL);
	notes = venture_string_is_empty(existing) ? g_strdup(line)
	                                          : g_strdup_printf("%s\n%s", existing, line);
	g_object_set(record, "notes", notes, NULL);
	books_system_actor(&actor);

	if (!venture_database_save(database, record, &actor, &error))
		g_message("books: the run's note was not written: %s", error->message);
}

/* The pass after a run, and its one line for the run record. */
static void
books_deliver(
	VentureContext	*context,
	gint64		 organization_id,
	gint64		 source_id,
	gint64		 run_record_id
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *line = NULL;
	BooksSettings settings;
	BooksRun probe;
	VentureBooksPostQuery query;
	VentureActor actor;
	gboolean second;

	if (!books_can_run(context, NULL))
		return;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(venture_context_get_database(context)), NULL);
	source = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_DATA_SOURCE,
	                              source_id, NULL);

	if ((NULL == source) || venture_entity_is_deleted(source) ||
	    !books_settings_read(source, &settings, NULL) || (BOOKS_MODE_DAILY != settings.mode) ||
	    !settings.auto_post)
		return;

	/* A second-actor rule on posting is a person's decision each time;
	 * a pass nobody asked for would only file proposals. */
	memset(&probe, 0, sizeof(probe));
	probe.database = venture_context_get_database(context);
	probe.org = organization_id;
	second = books_needs_second_actor(&probe);

	if (second)
	{
		books_note_run(context, run_record_id,
		               "books: not posted -- a second-actor rule covers posting; run Post to books "
		               "and have it approved");
		return;
	}

	memset(&query, 0, sizeof(query));
	query.organization_id = organization_id;
	query.data_source_id = source_id;
	books_system_actor(&actor);

	if (!venture_arbitrage_books_post(context, &query, &actor, &report, &error))
	{
		line = g_strdup_printf("books: not posted -- %s", error->message);
		books_note_run(context, run_record_id, line);
		return;
	}

	{
		JsonObject *root = json_node_get_object(report);
		JsonObject *counts = json_object_get_object_member(root, "days_by_status");
		gint64 posted = json_object_get_int_member_with_default(counts, "unposted", 0);
		gint64 again = json_object_get_int_member_with_default(counts, "changed", 0) +
		               json_object_get_int_member_with_default(counts, "follows", 0);
		gint64 later = json_object_get_int_member_with_default(counts, "later", 0);

		if (0 == (posted + again + later))
			return;

		line = g_strdup_printf("books: %" G_GINT64_FORMAT " day(s) posted, %" G_GINT64_FORMAT
		                       " posted again, %" G_GINT64_FORMAT " left for the next run",
		                       posted, again, later);
		books_note_run(context, run_record_id, line);
	}
}

typedef struct
{
	GWeakRef	 context;
	gint64		 organization_id;
	gint64		 source_id;
	gint64		 run_record_id;
} BooksDeferred;

static void
books_deferred_free(gpointer data)
{
	BooksDeferred *deferred = data;

	g_weak_ref_clear(&deferred->context);
	g_free(deferred);
}

static gboolean
books_deferred_fire(gpointer data)
{
	BooksDeferred *deferred = data;
	g_autoptr(VentureContext) context = NULL;

	context = g_weak_ref_get(&deferred->context);

	if (NULL == context)
		return G_SOURCE_REMOVE;

	if (books_must_wait(context))
		return G_SOURCE_CONTINUE;

	books_deliver(context, deferred->organization_id, deferred->source_id, deferred->run_record_id);

	return G_SOURCE_REMOVE;
}

/*
 * After every run, on the main thread, after the mirror's (installed
 * before this), so a run's new accounts are places already. A failed run
 * stored nothing new. Like the mirror, it waits out an automation
 * handler's nested loop or an open transaction.
 */
static void
books_hook_run(
	VentureContext	*context,
	VentureFeedRun	*run,
	VentureEntity	*run_record,
	gpointer	 user_data
){
	gint64 run_record_id;

	(void)user_data;

	if ((VENTURE_DATA_SOURCE_RUN_STATUS_FAILED == venture_feed_run_get_status(run)) ||
	    !books_can_run(context, NULL))
		return;

	run_record_id = (NULL != run_record) ? venture_entity_get_id(run_record) : 0;

	if (books_must_wait(context))
	{
		BooksDeferred *deferred = g_new0(BooksDeferred, 1);

		g_weak_ref_init(&deferred->context, context);
		deferred->organization_id = venture_feed_run_get_organization_id(run);
		deferred->source_id = venture_feed_run_get_source_id(run);
		deferred->run_record_id = run_record_id;
		g_timeout_add_full(G_PRIORITY_DEFAULT, BOOKS_RETRY_MS, books_deferred_fire, deferred,
		                   books_deferred_free);
		return;
	}

	books_deliver(context, venture_feed_run_get_organization_id(run), venture_feed_run_get_source_id(run),
	              run_record_id);
}

#endif /* VENTURE_HAVE_SQLITE */

/* ==========================================================================
 * Installation
 * ========================================================================== */

void
venture_arbitrage_books_install(VentureContext *context)
{
	VentureDatabase *database;
	GWeakRef *ref;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The last context over a database is the one its actions ask. */
	ref = g_new0(GWeakRef, 1);
	g_weak_ref_init(ref, context);
	g_object_set_data_full(G_OBJECT(database), BOOKS_CONTEXT_KEY, ref, books_weak_ref_free);

	/* Validators and actions are per database; the tests build several
	 * contexts over one. */
	if (NULL == g_object_get_data(G_OBJECT(database), BOOKS_INSTALLED_KEY))
	{
		g_object_set_data(G_OBJECT(database), BOOKS_INSTALLED_KEY, GINT_TO_POINTER(1));
		venture_database_add_save_validator(database, VENTURE_TYPE_DATA_SOURCE, books_validate_source,
		                                    NULL, NULL);
		venture_database_add_save_validator(database, VENTURE_TYPE_EXTERNAL_POSTING,
		                                    books_validate_posting, NULL, NULL);
		books_register_actions(database);
	}

#ifdef VENTURE_HAVE_SQLITE
	venture_feeds_add_hook(context, VENTURE_ARBITRAGE_BOOKS_HOOK, NULL, NULL, books_hook_run, NULL, NULL);
#endif
}
