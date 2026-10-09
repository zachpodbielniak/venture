/*
 * venture-feeds-private.h - What the feeds files share and nobody else sees
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The batch's arrays, the frozen source, the request's accounting and the
 * run's figures: plain data the providers, the worker and the service pass
 * between them. Kept out of the public headers so a plugin builds against
 * accessors and the layout can change.
 */

#ifndef VENTURE_FEEDS_PRIVATE_H
#define VENTURE_FEEDS_PRIVATE_H

#include "venture.h"

#include <libsoup/soup.h>
#include <glib/gstdio.h>

G_BEGIN_DECLS

/* --- The batch ------------------------------------------------------------ */

typedef struct
{
	gchar	*key;
	gchar	*name;
	gchar	*kind;
	gchar	*group_key;
	gchar	*currency;
	gchar	*attrs_json;
} FeedBatchVenue;

typedef struct
{
	gchar	*key;
	gchar	*name;
	gchar	*kind;
	gchar	*category;
	gchar	*parent_key;
	gchar	*attrs_json;
} FeedBatchInstrument;

typedef struct
{
	gchar			*instrument_key;
	guint64			 listing_id;
	gint64			 unit_price;
	gint64			 quantity;
	VentureSeriesSide	 side;
	gint64			 expires_in_min;
} FeedBatchListing;

typedef struct
{
	gchar	*instrument_key;
	gint64	 min_price;
	gint64	 market_value;
	gint64	 mean;
	gint64	 median;
	gint64	 sale_avg;
	gint64	 quantity;
	gint64	 listings;
	gint64	 sold;
	gint64	 historical;
	gdouble	 sale_rate;
	gdouble	 sold_per_day;
} FeedBatchStats;

typedef struct
{
	gchar		*venue_key;
	gchar		*currency;
	gint64		 taken_at;
	gboolean	 complete;
	GArray		*listings;	/* FeedBatchListing */
	GArray		*stats;		/* FeedBatchStats */
} FeedBatchSnapshot;

typedef struct
{
	gchar			*venue_key;
	gchar			*instrument_key;
	VentureSeriesQuoteSide	 side;
	gint64			 value;
	gchar			*currency;
	gint64			 liquidity;
	gint64			 taken_at;
} FeedBatchQuote;

typedef struct
{
	gchar	*key;
	gchar	*title;
	gchar	*url;
	gchar	*summary;
	gint64	 published_at;
	gchar	*venue_key;
	gchar	*instrument_key;
} FeedBatchEntry;

typedef struct
{
	gchar	 *record_type;
	gchar	 *fields_json;	/* a JSON object, wire spelling */
	gchar	**match;	/* nullable */
} FeedBatchRecord;

struct _VentureFeedBatch
{
	gatomicrefcount	 ref;

	GPtrArray	*venues;	/* FeedBatchVenue */
	GPtrArray	*instruments;	/* FeedBatchInstrument */
	GPtrArray	*snapshots;	/* FeedBatchSnapshot, in order */
	GHashTable	*open_snapshots;/* venue key -> FeedBatchSnapshot, borrowed */
	GPtrArray	*quotes;	/* FeedBatchQuote */
	GPtrArray	*entries;	/* FeedBatchEntry */
	GPtrArray	*records;	/* FeedBatchRecord */
	GPtrArray	*notes;		/* gchar* */

	gchar		*cursor;
	gboolean	 not_modified;
	gboolean	 units_changed;	/* the provider's units() would now answer differently */
	gchar		*error;
	gint64		 retry_after;
	gint64		 refused;
	gint64		 remote_used;		/* -1: the far end said nothing */
	gint64		 remote_remaining;

	/*
	 * The operator's accounts, as the series store's own input structs
	 * so the worker hands them over without a copy. Every string they
	 * point at lives in @strings: one block of memory for fifty
	 * thousand ledger rows rather than four hundred thousand small
	 * allocations, and repeated keys (an account, a venue, a kind)
	 * stored once.
	 */
	GStringChunk	*strings;
	GArray		*accounts;		/* VentureSeriesAccount */
	GArray		*account_snapshots;	/* VentureSeriesAccountSnapshot */
	GArray		*balances;		/* VentureSeriesBalance */
	GArray		*holdings;		/* VentureSeriesHolding */
	GArray		*positions;		/* VentureSeriesPosition */
	GArray		*inbound;		/* VentureSeriesInbound */
	GArray		*txns;			/* VentureSeriesTxn */
	GArray		*logins;		/* VentureSeriesLogin */
	GHashTable	*holding_index;		/* "account\x1fplace\x1finstrument" -> index + 1 */
	GHashTable	*account_state;		/* account key -> FeedAccountState */

	/* What venture_feed_batch_add_jsonl() carries from one call to the
	 * next, so a large stream can be read a slice at a time. */
	GHashTable	*jsonl_venue_currency;	/* venue key -> currency, owned */
	guint		 jsonl_logs;
};

/* What a batch knows of one account while it is being built: whether a
 * snapshot of it was begun, what it covers, and which kinds already have
 * rows -- a snapshot after its own rows would say they were all of them
 * when they were not. @refused is the kinds a row of was refused: the
 * snapshot stops covering those, or the sweep would delete the stored row
 * the batch could not read. @snapshot is its place in the batch's
 * snapshots, plus one (0: none kept). */
typedef struct
{
	gboolean	 snapshotted;
	guint		 covers;
	guint		 rows;
	guint		 refused;
	guint		 snapshot;
} FeedAccountState;

/* --- The frozen source ------------------------------------------------------ */

/*
 * One hook a later module (alerts, promotion) hangs on every run: a frozen
 * copy of whatever it needs, made on the main thread when the source is
 * frozen, read on the worker after each commit, and an answer read back on
 * the main thread after the run is written.
 */
typedef struct _FeedHook FeedHook;

struct _FeedHook
{
	gatomicrefcount			 ref;
	gchar				*name;
	VentureFeedsHookFreezeFunc	 freeze;
	VentureFeedsHookCommitFunc	 commit;
	VentureFeedsHookRunFunc		 run;
	gpointer			 user_data;
	GDestroyNotify			 destroy;
};

FeedHook *feed_hook_ref(FeedHook *hook);
void feed_hook_unref(FeedHook *hook);

typedef struct
{
	FeedHook	*hook;		/* reference held */
	gpointer	 frozen;
	GDestroyNotify	 frozen_free;
} FeedFrozenHook;

/*
 * Everything the worker needs to run a source, read on the main thread and
 * never changed afterwards. The worker parses its own JSON from the strings
 * here: a JsonObject's references and caches are not safe to share between
 * threads, a string is. A new spec replaces the old one whole; nothing is
 * patched in place.
 */
struct _VentureFeedSource
{
	gatomicrefcount		 ref;

	gint64			 id;
	gchar			*uuid;
	gint64			 organization_id;
	gchar			*name;

	gchar			*provider_name;
	VentureDataSourceProvider *provider;
	gpointer		 frozen;
	GDestroyNotify		 frozen_free;

	gchar			*settings_json;
	gchar			*secrets_json;
	GPtrArray		*secret_values;	/* gchar*, length >= 6, for redaction */

	gboolean		 enabled;
	gchar			*schedule;	/* "auto", "hourly", "manual" or cron */
	gboolean		 track_known;
	GHashTable		*known;		/* instrument keys; NULL unless known */

	gchar			*currency;
	gchar			*venue_namespace;
	gchar			*instrument_namespace;
	gint64			 min_value;
	gchar			*min_currency;

	gchar		       **units;
	gchar			*store_dir;

	guint64			 max_store_bytes;
	guint			 hourly_days;
	guint			 daily_days;
	guint			 idle_days;
	gint			 upkeep_hour;	/* UTC; -1: a day after the last */
	gsize			 max_response_bytes;
	guint			 request_timeout;
	gchar		       **allowed_origins;
	gchar		       **file_roots;
	guint			 requests_per_hour;
	guint			 max_records;
	guint			 run_window;	/* seconds; 0 records every pass */
	gchar		       **record_types;

	GPtrArray		*hooks;		/* FeedFrozenHook */
};

/* --- A run ------------------------------------------------------------------ */

struct _VentureFeedRun
{
	gatomicrefcount			 ref;

	gint64				 source_id;
	gint64				 organization_id;
	VentureDataSourceRunTrigger	 trigger;
	VentureDataSourceRunStatus	 status;
	gint64				 started_at;
	gint64				 finished_at;

	gint64				 units;
	gint64				 not_modified;
	gint64				 rows;
	gint64				 new_instruments;
	gint64				 refused;
	gint64				 requests;
	gint64				 bytes;
	gint64				 quota_used;
	gint64				 quota_limit;
	gint64				 remote_used;	/* -1: none reported */
	gint64				 remote_limit;
	gint				 http_status;

	/* Units by outcome, which decide the status. */
	gint64				 succeeded;
	gint64				 failed;
	gint64				 deferred;

	gchar				*error;
	GPtrArray			*notes;		/* gchar* */
	GPtrArray			*records;	/* FeedBatchRecord */
	gint64				 records_dropped;
	guint				 max_records;
	GPtrArray			*venues;	/* gchar*, NULL-terminated */
	GHashTable			*payloads;	/* name -> JsonNode */
	gchar				*push_id;	/* NULL unless a push made it */
};

VentureFeedRun *
venture_feed_run_new_internal(
	VentureFeedSource		*source,
	VentureDataSourceRunTrigger	 trigger,
	gint64				 started_at
);

void
venture_feed_run_finish(
	VentureFeedRun	*self,
	gint64		 finished_at
);

/* --- The request ------------------------------------------------------------ */

/*
 * The per-source quota, owned by the worker and lent to one request at a
 * time: a source's units are fetched one after another, never at once.
 */
typedef struct
{
	gint64	window_start;
	gint64	spent;
} FeedQuota;

VentureFeedRequest *
venture_feed_request_new_internal(
	VentureFeedSource	*source,
	const gchar		*unit,
	JsonObject		*settings,
	JsonObject		*secrets,
	SoupSession		*session,
	const FeedQuota		*quota,
	const gchar		*cursor,
	gint64			 if_modified_since,
	gint64			 fetched_at,
	GCancellable		*cancellable
);

/* A pushed body for the request to read instead of fetching; the push
 * provider takes it with venture_feed_request_get_body(). */
void
venture_feed_request_set_body(
	VentureFeedRequest	*self,
	GBytes			*body
);

/* The request's accounting, read by the worker when the fetch ends. */
gint64	venture_feed_request_get_requests(VentureFeedRequest *self);
gint64	venture_feed_request_get_bytes(VentureFeedRequest *self);
gint	venture_feed_request_get_http_status(VentureFeedRequest *self);
gint64	venture_feed_request_get_retry_after(VentureFeedRequest *self);
gint64	venture_feed_request_get_deferred_until(VentureFeedRequest *self);
gint64	venture_feed_request_get_last_modified(VentureFeedRequest *self);
void	venture_feed_request_get_quota(VentureFeedRequest *self, FeedQuota *out);

SoupSession *
venture_feeds_session_new(void);

void
venture_feeds_record_free(gpointer data);

/* --- Redaction -------------------------------------------------------------- */

gchar *
venture_feed_source_redact(
	VentureFeedSource	*source,
	const gchar		*text
);

/* --- Metrics ---------------------------------------------------------------- */

/* Adds the feeds' collector to the context's registry (venture-feeds-metrics.c). */
void
venture_feeds_metrics_install(VentureContext *context);

/* Counts a run as it is written: by status, and its units, rows and bytes.
 * Main thread. */
void
venture_feeds_metrics_count_run(
	VentureContext	*context,
	VentureFeedRun	*run
);

/* --- Built-in providers ----------------------------------------------------- */

void
venture_feeds_register_builtin_providers(VentureDataSourceProviderRegistry *registry);

G_END_DECLS

#endif /* VENTURE_FEEDS_PRIVATE_H */
