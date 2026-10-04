/*
 * venture-feeds-records.c - Market data sources and their runs
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/* ==========================================================================
 * Data sources
 *
 * Where market data comes from: a provider (a registered name such as
 * http_json, csv, file_jsonl, or one a plugin added), the provider's
 * settings as YAML, and how often to ask. Credentials are not here: a
 * setting the provider's schema marks sensitive is sealed in the
 * integration store under "feed-<uuid>", and the save validator refuses
 * one written into the settings. Nor is any filesystem path: a source's
 * store lives at <state_dir>/series/<uuid>/, derived every time, so no
 * record can point the server at a file. A file a provider reads must sit
 * under the operator's feeds.file_roots.
 *
 * Writes need an administrator, like a currency's: a source decides which
 * outside host this server calls and what it stores, which is
 * configuration rather than data entry.
 * ========================================================================== */

static const VentureFieldDecl venture_data_source_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What the source is: an auction house's region, a supplier's price list"),
	VENTURE_FIELD("provider", "Provider",
	              "Which provider fetches it: http_json, csv, file_jsonl, or one a plugin registered",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("enabled", "Enabled",
	              "Fetched on its schedule; a source switched off keeps its history and can still be synced by hand",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("schedule", "Schedule",
	              "auto (learn when each venue updates), hourly, manual, or five cron fields; empty is auto",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("settings", "Settings",
	              "The provider's settings as YAML; never credentials, which are set separately and sealed",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("currency", "Currency",
	              "What a price that names no currency is in",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("venue-namespace", "Venue namespace",
	              "Prefix that tells this source's venues from another's with the same key: eu-realm, bookmaker",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("instrument-namespace", "Instrument namespace",
	              "The same for its instruments: wow-item, sku, event",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("track", "Track",
	                   "all stores every instrument the source reports; known only the ones in the settings' instruments list and the instrument records filed under the source",
	                   venture_data_source_track_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("min-value", "Minimum value",
	                    "Optional: an instrument worth less than this, in this currency, is left out of deal prices"),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD("result", "Result", "What the last action said; shown once, never stored",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_TRANSIENT)
};

static void venture_data_source_constructed(GObject *object);

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureDataSource, venture_data_source,
	venture_data_source_fields,
	G_OBJECT_CLASS(klass)->constructed = venture_data_source_constructed;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Data source", NULL);
	g_type_set_qdata(G_TYPE_FROM_CLASS(klass),
		g_quark_from_static_string("venture-access-admin-write"), GINT_TO_POINTER(1));)

/*
 * A new source starts enabled. The column's zero value is FALSE -- off --
 * which is the safe reading of a row nobody wrote the flag on; but the
 * form and a create that does not mention it build the object here, and
 * somebody adding a source means it to run. A row read back from the
 * database overwrites this with what was stored.
 */
static void
venture_data_source_constructed(GObject *object)
{
	if (NULL != G_OBJECT_CLASS(venture_data_source_parent_class)->constructed)
		G_OBJECT_CLASS(venture_data_source_parent_class)->constructed(object);

	g_object_set(object, "enabled", TRUE, NULL);
}

/* ==========================================================================
 * Runs
 *
 * What a pass over a data source did, written once when it ends -- by the
 * feeds service on the main thread, as the system, from the plain figures
 * the worker thread handed back. Nobody edits one: it is evidence, like a
 * forge run or a webhook delivery, and the generic write routes refuse it.
 * Scheduled passes are gathered into one run per feeds.run_window_minutes
 * so a source that polls a hundred venues a minute apart does not write a
 * hundred rows an hour; a manual sync is always a run of its own.
 * ========================================================================== */

static const VentureFieldDecl venture_data_source_run_fields[] = {
	VENTURE_FIELD_REF("data-source-id", "Data source", "Which source ran",
	                  "data_source", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_ENUM("status", "Status", "How it ended",
	                   venture_data_source_run_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("trigger", "Trigger", "What started it",
	                   venture_data_source_run_trigger_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("started-at", "Started", "When its first fetch began",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("finished-at", "Finished", "When its last fetch ended",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("units", "Units", "Fetches made: one per realm, page or file",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("not-modified", "Not modified", "Fetches that answered that nothing had changed",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("rows", "Rows", "Rows written to the source's series store",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("new-instruments", "New instruments", "Instruments the store had not seen before",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("refused", "Refused", "Listings and instruments refused: malformed, or past the store's size cap",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("records", "Records", "Main-database records the source created or updated",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("requests", "Requests", "HTTP requests made",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("bytes", "Bytes", "Bytes of answers read",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("quota-used", "Quota used", "Request budget spent in the current hour, when the source has one",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("quota-limit", "Quota limit", "The source's request budget an hour; 0 for none",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("http-status", "HTTP status", "The last HTTP status other than 200, or 200; 0 when nothing was fetched over HTTP",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("error", "Error", "Why it failed, when it did; credentials are redacted"),
	VENTURE_FIELD_TEXT("notes", "Notes", "What else it noticed: refusals, skipped records, deferrals")
};

/* "failed run" -- the source's name needs the database, which a display
 * name does not have. */
static gchar *
venture_data_source_run_display_name(VentureEntity *self)
{
	GEnumClass *klass;
	GEnumValue *value;
	VentureDataSourceRunStatus status;
	gchar *name;

	g_object_get(self, "status", &status, NULL);
	klass = (GEnumClass *)g_type_class_ref(VENTURE_TYPE_DATA_SOURCE_RUN_STATUS);
	value = g_enum_get_value(klass, (gint)status);
	name = g_strdup_printf("Run #%" G_GINT64_FORMAT " (%s)",
	                       venture_entity_get_id(self),
	                       (NULL != value) ? value->value_nick : "unknown");
	g_type_class_unref(klass);

	return name;
}

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureDataSourceRun, venture_data_source_run,
	venture_data_source_run_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = venture_data_source_run_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Data source run", NULL);
	/* Written once by the feeds service as the system; its source is
	 * what to discuss. */
	venture_entity_class_set_commentable(VENTURE_ENTITY_CLASS(klass), FALSE);)
