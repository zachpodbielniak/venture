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
	gchar		*error;
	gint64		 retry_after;
	gint64		 refused;
};

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
};

VentureFeedRun *
venture_feed_run_new_internal(
	VentureFeedSource		*source,
	VentureDataSourceRunTrigger	 trigger,
	gint64				 started_at
);

void
venture_feed_run_add_note(
	VentureFeedRun	*self,
	const gchar	*note
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

/* --- Built-in providers ----------------------------------------------------- */

void
venture_feeds_register_builtin_providers(VentureDataSourceProviderRegistry *registry);

G_END_DECLS

#endif /* VENTURE_FEEDS_PRIVATE_H */
