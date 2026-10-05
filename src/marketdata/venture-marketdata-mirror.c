/*
 * venture-marketdata-mirror.c - The operator's accounts and positions as records
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Accounts become locations and positions become listings. Everything
 * here runs on the main thread, reading a source's store through a read
 * handle and writing records as the system ("mirror"); the worker never
 * sees any of it. The rules, in the order a pass applies them:
 *
 *  1. Accounts. With `auto_promote_accounts` (default true) every account
 *     the store knows that has no location yet gets one, inside a location
 *     for its group. A location a person deleted stays deleted: only an
 *     explicit promotion restores one.
 *  2. Positions present in the store. A position with no listing gets
 *     one, if its instrument comes to a product; a listing the mirror
 *     made is kept in step field by field, each field only while it still
 *     holds what the mirror last wrote (`mirror-state`), so a person's
 *     edit stands. A listing whose outcome a person changed is theirs.
 *  3. Open mirrored listings whose position is gone from an account a
 *     complete positions snapshot has restated. Each is judged from the
 *     store's ledger: every listing's units sold while listed claim their
 *     sale rows first (listings still on the market included, so a
 *     vanished one cannot take their sales), then sale rows fill units,
 *     then expired and cancelled rows, oldest listing first (first in,
 *     first out) among the listings of the same account and instrument.
 *     A read that may be short judges nothing. Units the ledger
 *     does not account for expired if the listing was past its expiry when
 *     it was found gone, and are otherwise waited for -- the sale of an
 *     auction reaches a ledger when its mail is opened -- for
 *     `mirror_grace_hours`, then called cancelled.
 *
 * A pass writes at most `mirror_max_writes` records -- the instruments and
 * venues it promotes included -- and stops starting new work there; what
 * is left is still out of step on the next run, so the next pass
 * continues with no cursor to keep. Writes go in transactions of
 * VENTURE_MARKETDATA_MIRROR_BATCH, rolled back whole on a database error.
 */

#include "venture.h"
#include "marketdata/venture-marketdata-private.h"

#include <string.h>

#define MIRROR_INSTALLED_KEY	"venture-marketdata-mirror-installed"
#define MIRROR_RETRY_MS		(50)
#define MIRROR_GROUP_MARK	"/group:"
#define MIRROR_LOGIN_MARK	"/login:"
#define MIRROR_LOCATION_PERMIT_KEY "venture-marketdata-location-permit"
#define MIRROR_NOTE_KEYS	(5)
#define MIRROR_MAX_FAILURE_NOTES (10)
#define MIRROR_IN_BATCH		(200)

/* ==========================================================================
 * Settings
 * ========================================================================== */

#ifdef VENTURE_HAVE_SQLITE

typedef struct
{
	gboolean	 mirror;
	gboolean	 auto_promote;
	gboolean	 create_products;
	gint64		 products_venture_id;
	gchar		*namespace_;
	guint		 max_writes;
	guint		 grace_hours;
} MirrorSettings;

static void
mirror_settings_clear(MirrorSettings *settings)
{
	g_clear_pointer(&settings->namespace_, g_free);
}

/* A boolean setting; absent or null is @fallback. */
static gboolean
mirror_setting_bool(
	JsonObject	 *settings,
	const gchar	 *name,
	gboolean	  fallback,
	gboolean	 *out,
	GError		**error
){
	JsonNode *node;

	node = json_object_get_member(settings, name);
	*out = fallback;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return TRUE;

	if (!JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_BOOLEAN != json_node_get_value_type(node)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "Setting %s must be true or false", name);
		return FALSE;
	}

	*out = json_node_get_boolean(node);

	return TRUE;
}

/* A whole-number setting in [@min, @max]; absent or null is @fallback. */
static gboolean
mirror_setting_int(
	JsonObject	 *settings,
	const gchar	 *name,
	gint64		  min,
	gint64		  max,
	gint64		  fallback,
	gint64		 *out,
	GError		**error
){
	JsonNode *node;

	node = json_object_get_member(settings, name);
	*out = fallback;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return TRUE;

	if (!JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_INT64 != json_node_get_value_type(node)) ||
	    (json_node_get_int(node) < min) || (json_node_get_int(node) > max))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "Setting %s must be a whole number from %" G_GINT64_FORMAT " to %"
		            G_GINT64_FORMAT, name, min, max);
		return FALSE;
	}

	*out = json_node_get_int(node);

	return TRUE;
}

/*
 * A namespace for account references: letters, digits, dot, dash and
 * underscore. No colon (it ends the namespace in "<ns>:<key>") and no
 * slash (a group's location is "<ns>/group:<group>"), so an account's
 * reference and a group's can never be the same string.
 */
static gboolean
mirror_namespace_valid(const gchar *text)
{
	const gchar *p;

	if (venture_string_is_empty(text) || (strlen(text) > VENTURE_MARKETDATA_MAX_NAMESPACE))
		return FALSE;

	for (p = text; '\0' != *p; p++)
	{
		if (!g_ascii_isalnum(*p) && ('.' != *p) && ('-' != *p) && ('_' != *p))
			return FALSE;
	}

	return TRUE;
}

/*
 * The mirror's settings from a source's parsed settings. @uuid is the
 * source's, the namespace when none is set. Shared by the save validator
 * and the pass, so what was accepted is what runs.
 */
static gboolean
mirror_settings_from(
	JsonObject	 *settings,
	const gchar	 *uuid,
	MirrorSettings	 *out,
	GError		**error
){
	gint64 value;
	JsonNode *node;

	memset(out, 0, sizeof(*out));

	if (!mirror_setting_bool(settings, "mirror_positions", TRUE, &out->mirror, error) ||
	    !mirror_setting_bool(settings, "auto_promote_accounts", TRUE, &out->auto_promote, error) ||
	    !mirror_setting_bool(settings, "create_products", FALSE, &out->create_products, error) ||
	    !mirror_setting_int(settings, "products_venture_id", 1, G_MAXINT64, 0,
	                        &out->products_venture_id, error))
		return FALSE;

	if (!mirror_setting_int(settings, "mirror_max_writes", 1, VENTURE_MARKETDATA_MIRROR_WRITES_LIMIT,
	                        VENTURE_MARKETDATA_MIRROR_MAX_WRITES, &value, error))
		return FALSE;

	out->max_writes = (guint)value;

	if (!mirror_setting_int(settings, "mirror_grace_hours", 0,
	                        VENTURE_MARKETDATA_MIRROR_MAX_GRACE_HOURS,
	                        VENTURE_MARKETDATA_MIRROR_GRACE_HOURS, &value, error))
		return FALSE;

	out->grace_hours = (guint)value;

	/* A product is filed under a venture; making one with none would be
	 * a product no venture's report ever counts. */
	if (out->create_products && (out->products_venture_id <= 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "Setting create_products needs products_venture_id: the venture "
		                    "the products it makes are filed under");
		return FALSE;
	}

	node = json_object_get_member(settings, "account_namespace");

	if ((NULL != node) && !JSON_NODE_HOLDS_NULL(node))
	{
		if (!JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_STRING != json_node_get_value_type(node)) ||
		    !mirror_namespace_valid(json_node_get_string(node)))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "Setting account_namespace must be 1 to %d letters, digits, dots, "
			            "dashes or underscores", VENTURE_MARKETDATA_MAX_NAMESPACE);
			return FALSE;
		}

		out->namespace_ = g_strdup(json_node_get_string(node));
	}
	else
		out->namespace_ = g_strdup((NULL != uuid) ? uuid : "");

	return TRUE;
}

/* The mirror's settings of a data_source record. */
static gboolean
mirror_settings_read(
	VentureEntity	 *source,
	MirrorSettings	 *out,
	GError		**error
){
	g_autoptr(JsonObject) settings = NULL;
	g_autofree gchar *text = NULL;

	memset(out, 0, sizeof(*out));
	g_object_get(source, "settings", &text, NULL);
	settings = venture_feeds_parse_settings(text, error);

	if (NULL == settings)
		return FALSE;

	return mirror_settings_from(settings, venture_entity_get_uuid(source), out, error);
}

#endif /* VENTURE_HAVE_SQLITE */

gchar *
venture_marketdata_account_ref(
	VentureEntity	*data_source,
	const gchar	*key
){
	g_return_val_if_fail(VENTURE_IS_ENTITY(data_source), NULL);

	if (venture_string_is_empty(key))
		return NULL;

#ifdef VENTURE_HAVE_SQLITE
	{
		MirrorSettings settings;
		gchar *ref;

		if (!mirror_settings_read(data_source, &settings, NULL))
		{
			mirror_settings_clear(&settings);
			return NULL;
		}

		ref = g_strdup_printf("%s:%s", settings.namespace_, key);
		mirror_settings_clear(&settings);

		return ref;
	}
#else
	return g_strdup_printf("%s:%s", venture_entity_get_uuid(data_source), key);
#endif
}

gboolean
venture_marketdata_source_promotes_accounts(VentureEntity *data_source)
{
	g_return_val_if_fail(VENTURE_IS_ENTITY(data_source), FALSE);

#ifdef VENTURE_HAVE_SQLITE
	{
		MirrorSettings settings;
		gboolean promotes;

		/* Settings that do not parse promote nothing: the pass refuses
		 * them too, and a guess here would act where it does not. */
		promotes = mirror_settings_read(data_source, &settings, NULL) && settings.auto_promote;
		mirror_settings_clear(&settings);

		return promotes;
	}
#else
	return FALSE;
#endif
}

/* 0: the store's own bound. Process-wide, for tests (see the header). */
static guint mirror_max_rows = 0;

/*
 * Never past the store's bound as it is now, not the macro: the store
 * reads its bound at every call (a test lowers it), clamps a ledger page
 * to it in silence and refuses a positions read past it. A mirror bound
 * above the store's would take a cut ledger for a whole one and judge on
 * it, and ask for more positions than the store answers -- a refusal
 * that failed the pass. At the store's bound a read is a page, and a
 * page that comes back full is what the pass already treats as short.
 */
guint
venture_marketdata_mirror_get_max_rows(void)
{
#ifdef VENTURE_HAVE_SQLITE
	guint store_bound = (guint)venture_series_accounts_get_max_rows();

	if ((0 == mirror_max_rows) || (mirror_max_rows > store_bound))
		return store_bound;
#endif

	return mirror_max_rows;
}

void
venture_marketdata_mirror_set_max_rows(guint max_rows)
{
	mirror_max_rows = max_rows;
}

/* ==========================================================================
 * Validators
 * ========================================================================== */

/*
 * A source's mirror settings, judged when the settings are written: the
 * types and ranges, and a products venture that is the source's
 * organization's and not deleted. A venture deleted later leaves the
 * source editable; the pass then fails that product with a note.
 */
static gboolean
mirror_validate_source(
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
	MirrorSettings parsed;
	gboolean ok;

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

	ok = mirror_settings_from(settings, venture_entity_get_uuid(entity), &parsed, error);

	if (ok && (parsed.products_venture_id > 0))
	{
		g_autoptr(VentureEntity) venture = NULL;

		venture = venture_database_get(database, VENTURE_TYPE_VENTURE,
		                               parsed.products_venture_id, NULL);

		if ((NULL == venture) || venture_entity_is_deleted(venture) ||
		    (venture_entity_get_organization_id(venture) !=
		     venture_entity_get_organization_id(entity)))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "Setting products_venture_id names no venture of this organization "
			            "(#%" G_GINT64_FORMAT ")", parsed.products_venture_id);
			ok = FALSE;
		}
	}

	mirror_settings_clear(&parsed);

	return ok;
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
 * A location's reference: sane text, unique in the organization (deleted
 * places included, as the index counts them) said with the place's
 * number, and a data source of its own organization.
 */
static gboolean
mirror_validate_location(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autofree gchar *ref = NULL;
	g_autofree gchar *was = NULL;

	(void)user_data;

	/* Where the mirror last put a place is its own record, as a mirrored
	 * listing's state is: a person writing it would tell the mirror it
	 * had put the place where the person did, and the place would be
	 * moved back on the next pass. Judged on a change, so a form posting
	 * the stored value back is no write. */
	if (g_object_get_data(G_OBJECT(database), MIRROR_LOCATION_PERMIT_KEY) != (gpointer)entity)
	{
		g_autofree gchar *state = NULL;
		g_autofree gchar *was_state = NULL;

		g_object_get(entity, "mirror-state", &state, NULL);

		if (NULL != previous)
			g_object_get(previous, "mirror-state", &was_state, NULL);

		if (0 != g_strcmp0(venture_string_is_empty(state) ? "" : state,
		                   venture_string_is_empty(was_state) ? "" : was_state))
		{
			venture_set_error_validation(error, "Mirror state",
				"is where the account mirror last put this place, and only the mirror "
				"writes it");
			return FALSE;
		}
	}

	g_object_get(entity, "external-ref", &ref, NULL);

	if (NULL != previous)
		g_object_get(previous, "external-ref", &was, NULL);

	if (!venture_string_is_empty(ref) && (0 != g_strcmp0(ref, was)))
	{
		g_autoptr(VentureEntity) other = NULL;
		g_autoptr(GError) lookup_error = NULL;

		if (!venture_marketdata_check_text(ref, "Reference", error))
			return FALSE;

		other = venture_marketdata_find_by_ref(database, VENTURE_TYPE_LOCATION,
		                                      venture_entity_get_organization_id(entity), ref,
		                                      &lookup_error);

		if (NULL != lookup_error)
		{
			g_propagate_error(error, g_steal_pointer(&lookup_error));
			return FALSE;
		}

		if ((NULL != other) && (venture_entity_get_id(other) != venture_entity_get_id(entity)))
		{
			venture_set_error_validation(error, "Reference",
				"location #%" G_GINT64_FORMAT " already stands for %s%s",
				venture_entity_get_id(other), ref,
				venture_entity_is_deleted(other) ? " (it is deleted; restore it instead)" : "");
			return FALSE;
		}
	}

	return venture_marketdata_check_same_organization(database, entity, previous, "data-source-id",
	                                                  VENTURE_TYPE_DATA_SOURCE, "Data source", error);
}

/* What a mirror pass needs before it reads anything. */
static gboolean
mirror_can_run(
	VentureContext	 *context,
	GError		**error
){
	if (!venture_context_module_enabled(context, "marketdata") ||
	    !venture_context_module_enabled(context, "market"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Accounts and positions become records through the market and "
		                    "marketdata modules, and one of them is off");
		return FALSE;
	}

#ifndef VENTURE_HAVE_SQLITE
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
	                    "Accounts and positions live in a series store, which this build "
	                    "(without SQLite) does not have");
	return FALSE;
#else
	if (NULL == venture_context_get_feeds_service(context))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Accounts and positions live in the stores the feeds module "
		                    "keeps, and the feeds module is off");
		return FALSE;
	}

	return TRUE;
#endif
}

#ifdef VENTURE_HAVE_SQLITE

/* ==========================================================================
 * A pass
 * ========================================================================== */

/*
 * A location the pass knows by reference: whether it is deleted, and for
 * an account's place where it is and where the mirror last put it
 * (@placed, -1 when it holds no record of that) -- what the hand-edit
 * rule compares.
 */
typedef struct
{
	gint64		 id;
	gboolean	 deleted;
	gint64		 parent_id;
	gint64		 placed;
	gint64		 data_source_id;
} MirrorKnown;

typedef struct
{
	gint64		 id;
	gchar		*name;
	gint64		 location_id;
} MirrorVenue;

/* NULL-safe: the table also remembers keys that came to no venue. */
static void
mirror_venue_free(gpointer data)
{
	MirrorVenue *venue = data;

	if (NULL == venue)
		return;

	g_free(venue->name);
	g_free(venue);
}

/*
 * What a pass wrote, as the report counts it. Kept apart from what it
 * only saw so that a batch the database undid can take its own writes
 * back out of the report (MirrorRun.at_batch).
 */
typedef struct
{
	guint			 writes;
	gint64			 promoted;
	gint64			 reparented;
	gint64			 venues_linked;
	gint64			 products_created;
	gint64			 created;
	gint64			 updated;
	gint64			 reopened;
	gint64			 sold;
	gint64			 partial;
	gint64			 expired;
	gint64			 cancelled;
} MirrorCounts;

typedef struct
{
	VentureContext		*context;
	VentureDatabase		*database;
	VentureEntity		*source;
	VentureSeriesStore	*store;
	MirrorSettings		 settings;
	gint64			 organization_id;
	gint64			 source_id;
	const gchar		*uuid;
	gint64			 now;
	VentureActor		 actor;
	gboolean		 batching;
	gboolean		 batch_open;
	guint			 in_batch;
	gboolean		 broken;
	gboolean		 cut;		/* the last unit stopped at the bound */
	MirrorCounts		 counts;
	MirrorCounts		 at_batch;	/* counts when the open batch began */

	GHashTable		*location_refs;		/* ref -> MirrorKnown */
	gboolean		 location_refs_complete;
	GHashTable		*account_locations;	/* account key -> gint64 */
	GHashTable		*accounts;		/* account key -> VentureSeriesAccountRow (borrowed) */
	GHashTable		*products;		/* instrument key -> gint64 (0: none, -1: deleted) */
	GHashTable		*venues;		/* venue key -> MirrorVenue, or absent */
	GPtrArray		*venue_rows;		/* VentureSeriesVenueRow, lazily */
	GHashTable		*unpriced;		/* instrument keys with no product */
	GHashTable		*unsellable;		/* instrument keys whose product is deleted */

	gint64			 n_accounts;
	gint64			 n_positions;
	gboolean		 positions_truncated;
	gint64			 unjudged_groups;
	gchar			*unjudged_reason;
	gint64			 waiting;
	gint64			 left_alone;
	gint64			 not_mirrored;
	gint64			 deleted_instruments;
	gint64			 deleted_products;
	gint64			 failed;
	gint64			 over_cap;
	gint64			 no_snapshot;
	gint64			 locations_kept;	/* places a person moved, left there */
	GPtrArray		*notes;
} MirrorRun;

/* Filled field by field, as every hand-made actor is (AGENTS.md). */
static void
mirror_actor(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor->name = VENTURE_MARKETDATA_MIRROR_HOOK;
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static void
mirror_run_init(
	MirrorRun	*m,
	VentureContext	*context,
	VentureEntity	*source,
	gboolean	 batching
){
	memset(m, 0, sizeof(*m));
	m->context = context;
	m->database = venture_context_get_database(context);
	m->source = g_object_ref(source);
	m->organization_id = venture_entity_get_organization_id(source);
	m->source_id = venture_entity_get_id(source);
	m->uuid = venture_entity_get_uuid(m->source);
	m->now = g_get_real_time() / G_USEC_PER_SEC;
	m->batching = batching;
	mirror_actor(&m->actor);
	m->location_refs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	m->account_locations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	m->accounts = g_hash_table_new(g_str_hash, g_str_equal);
	m->products = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	m->venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, mirror_venue_free);
	m->unpriced = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	m->unsellable = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	m->notes = g_ptr_array_new_with_free_func(g_free);
}

static void
mirror_run_clear(MirrorRun *m)
{
	g_clear_object(&m->source);
	g_clear_object(&m->store);
	mirror_settings_clear(&m->settings);
	g_clear_pointer(&m->location_refs, g_hash_table_unref);
	g_clear_pointer(&m->account_locations, g_hash_table_unref);
	g_clear_pointer(&m->accounts, g_hash_table_unref);
	g_clear_pointer(&m->products, g_hash_table_unref);
	g_clear_pointer(&m->venues, g_hash_table_unref);
	g_clear_pointer(&m->venue_rows, g_ptr_array_unref);
	g_clear_pointer(&m->unpriced, g_hash_table_unref);
	g_clear_pointer(&m->unsellable, g_hash_table_unref);
	g_clear_pointer(&m->unjudged_reason, g_free);
	g_clear_pointer(&m->notes, g_ptr_array_unref);
}

static void
mirror_note(
	MirrorRun	*m,
	const gchar	*format,
	...
) G_GNUC_PRINTF(2, 3);

static void
mirror_note(
	MirrorRun	*m,
	const gchar	*format,
	...
){
	va_list args;

	va_start(args, format);
	g_ptr_array_add(m->notes, g_strdup_vprintf(format, args));
	va_end(args);
}

/*
 * Whether the pass may start a unit of @n writes that must not be split:
 * a new place and its venue's link, a product and its instrument's link,
 * or one record. A pass that has written nothing may start a unit bigger
 * than its whole bound, or a bound of 1 would never make a product.
 */
static gboolean
mirror_has_room_for(
	MirrorRun	*m,
	guint		 n
){
	guint max;

	if (m->broken)
		return FALSE;

	max = m->settings.max_writes;

	if ((m->counts.writes < max) && (n <= max - m->counts.writes))
		return TRUE;

	return (0 == m->counts.writes) && (n > max);
}

/* Whether the pass may start more work. */
static gboolean
mirror_has_room(MirrorRun *m)
{
	return mirror_has_room_for(m, 1);
}

/* What a promotion may still write before the pass is at its bound. */
static void
mirror_budget(
	MirrorRun		*m,
	VentureMarketdataBudget	*budget
){
	budget->left = (m->counts.writes < m->settings.max_writes)
		? m->settings.max_writes - m->counts.writes : 0;
	budget->cut = FALSE;
}

/*
 * Opens a batch when a pass writes and none is open: every record the
 * pass writes -- its own saves and the promotions it calls -- goes into
 * one of these transactions.
 */
static gboolean
mirror_batch_open(
	MirrorRun	 *m,
	GError		**error
){
	if (!m->batching || m->batch_open)
		return TRUE;

	if (!venture_database_begin(m->database, error))
	{
		m->broken = TRUE;
		return FALSE;
	}

	m->batch_open = TRUE;
	m->in_batch = 0;
	m->at_batch = m->counts;

	return TRUE;
}

/*
 * Ends the open batch. A commit that fails took everything the batch
 * wrote with it, so the batch's writes are taken back out of the report,
 * the pass stops, and the next run does it again -- it is all still out
 * of step.
 */
static void
mirror_batch_end(MirrorRun *m)
{
	g_autoptr(GError) error = NULL;

	if (!m->batch_open)
		return;

	m->batch_open = FALSE;
	m->in_batch = 0;

	if (!venture_database_commit(m->database, &error))
	{
		m->broken = TRUE;
		m->counts = m->at_batch;
		mirror_note(m, "mirror: a batch of writes was undone (%s); the next run writes it again",
		            error->message);
	}
}

/*
 * After a database error, which is not a refusal. A save inside the
 * batch did not open the transaction, so it rolled nothing back: on
 * SQLite the batch's other writes would commit around the failed one,
 * and on PostgreSQL the transaction is aborted and its commit is a
 * rollback that says nothing. Either way the batch is rolled back here,
 * its writes leave the report, and the pass stops. TRUE when @error was
 * such an error.
 */
static gboolean
mirror_database_failed(
	MirrorRun	*m,
	const GError	*error
){
	if ((NULL == error) || !g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE))
		return FALSE;

	if (m->batch_open)
	{
		venture_database_rollback(m->database);
		m->batch_open = FALSE;
		m->in_batch = 0;
		m->counts = m->at_batch;
	}

	m->broken = TRUE;
	mirror_note(m, "mirror: a batch of writes was undone (%s); the next run writes it again",
	            error->message);

	return TRUE;
}

/* @n records written (or refused): toward the bound and the batch. */
static void
mirror_wrote(
	MirrorRun	*m,
	guint		 n
){
	m->counts.writes += n;

	if (!m->batch_open)
		return;

	m->in_batch += n;

	if (m->in_batch >= VENTURE_MARKETDATA_MIRROR_BATCH)
		mirror_batch_end(m);
}

/*
 * Writes one record as the mirror: a listing through the market module's
 * permit, anything else as an ordinary save. A refusal by a validator
 * counts toward the pass's bound like a write, and the batch goes on; a
 * database error ends the batch and the pass.
 */
static gboolean
mirror_write(
	MirrorRun	 *m,
	VentureEntity	 *entity,
	GError		**error
){
	g_autoptr(GError) local_error = NULL;
	gboolean ok;

	if (!mirror_batch_open(m, error))
		return FALSE;

	if (VENTURE_IS_LISTING(entity))
		ok = venture_market_save_mirrored_listing(m->database, entity, &m->actor, &local_error);
	else if (VENTURE_IS_LOCATION(entity))
	{
		gpointer previous;

		/* The permit names this object for this one save, as the
		 * listing's does: a nested save of another place is not it. */
		previous = g_object_get_data(G_OBJECT(m->database), MIRROR_LOCATION_PERMIT_KEY);
		g_object_set_data(G_OBJECT(m->database), MIRROR_LOCATION_PERMIT_KEY, entity);
		ok = venture_database_save(m->database, entity, &m->actor, &local_error);
		g_object_set_data(G_OBJECT(m->database), MIRROR_LOCATION_PERMIT_KEY, previous);
	}
	else
		ok = venture_database_save(m->database, entity, &m->actor, &local_error);

	if (!ok && mirror_database_failed(m, local_error))
	{
		g_propagate_error(error, g_steal_pointer(&local_error));
		return FALSE;
	}

	mirror_wrote(m, 1);

	if (!ok)
		g_propagate_error(error, g_steal_pointer(&local_error));

	return ok;
}

/* A write that failed, counted and said, a few at a time. */
static void
mirror_failed(
	MirrorRun	*m,
	const gchar	*what,
	const GError	*error
){
	m->failed++;

	if (m->failed <= MIRROR_MAX_FAILURE_NOTES)
		mirror_note(m, "mirror: %s not written: %s", what,
		            (NULL != error) ? error->message : "refused");
}

/* --- Small conversions ------------------------------------------------------------- */

static gint64
mirror_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;
	g_object_get(entity, property, &value, NULL);

	return value;
}

/* A date-time property as unix seconds, or VENTURE_SERIES_NONE. */
static gint64
mirror_unix(
	VentureEntity	*entity,
	const gchar	*property
){
	g_autoptr(GDateTime) when = NULL;

	g_object_get(entity, property, &when, NULL);

	return (NULL != when) ? g_date_time_to_unix(when) : VENTURE_SERIES_NONE;
}

static void
mirror_set_unix(
	VentureEntity	*entity,
	const gchar	*property,
	gint64		 value
){
	g_autoptr(GDateTime) when = NULL;

	if (VENTURE_SERIES_NONE != value)
		when = g_date_time_new_from_unix_utc(value);

	g_object_set(entity, property, when, NULL);
}

/*
 * A money property in its currency's minor units; FALSE when unset or not
 * sayable there. @currency gets the code.
 */
static gboolean
mirror_minor(
	VentureEntity	*entity,
	const gchar	*property,
	gint64		*out_amount,
	gchar		*out_currency
){
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(VentureMoney) scaled = NULL;
	const gchar *currency;

	g_object_get(entity, property, &money, NULL);

	if (NULL == money)
		return FALSE;

	currency = venture_money_get_currency(money);
	scaled = venture_money_rescale(money, venture_currency_get_exponent(currency), NULL);

	if (NULL == scaled)
		return FALSE;

	*out_amount = venture_money_get_amount(scaled);
	g_strlcpy(out_currency, venture_money_get_currency(scaled), VENTURE_MONEY_CURRENCY_LEN);

	return TRUE;
}

static void
mirror_set_money(
	VentureEntity	*entity,
	const gchar	*property,
	gint64		 amount,
	const gchar	*currency
){
	g_autoptr(VentureMoney) money = NULL;

	if (VENTURE_SERIES_NONE != amount)
		money = venture_money_new_for_currency(amount, currency);

	g_object_set(entity, property, money, NULL);
}

/* --- The mirror's record of what it wrote ------------------------------------------- */

/*
 * `mirror-state` is a JSON object: account, instrument, venue (the store's
 * keys, so the listing is matched to its ledger rows however its product
 * or place are edited), and quantity, sold, unit_price, currency, bid,
 * expires_at, location_id, outcome -- the values the mirror last wrote.
 * A closed listing adds evidence ("ledger", "expiry" or "inferred") and
 * txns ({ledger key: units} it was matched to); one waiting for its
 * ledger rows adds vanished_at. Absent ints are null.
 */
static JsonObject *
mirror_state_read(VentureEntity *listing)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *root;

	g_object_get(listing, "mirror-state", &text, NULL);

	if (venture_string_is_empty(text))
		return NULL;

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, text, -1, NULL))
		return NULL;

	root = json_parser_get_root(parser);

	if ((NULL == root) || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;

	return json_object_ref(json_node_get_object(root));
}

static void
mirror_state_store(
	VentureEntity	*listing,
	JsonObject	*state
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *text = NULL;

	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, state);
	text = json_to_string(node, FALSE);
	g_object_set(listing, "mirror-state", text, NULL);
}

static gint64
mirror_state_int(
	JsonObject	*state,
	const gchar	*name
){
	JsonNode *node;

	node = json_object_get_member(state, name);

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_INT64 != json_node_get_value_type(node)))
		return VENTURE_SERIES_NONE;

	return json_node_get_int(node);
}

static void
mirror_state_set_int(
	JsonObject	*state,
	const gchar	*name,
	gint64		 value
){
	if (VENTURE_SERIES_NONE == value)
		json_object_set_null_member(state, name);
	else
		json_object_set_int_member(state, name, value);
}

static const gchar *
mirror_state_string(
	JsonObject	*state,
	const gchar	*name
){
	JsonNode *node;

	node = json_object_get_member(state, name);

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)))
		return NULL;

	return json_node_get_string(node);
}

static const gchar *
mirror_outcome_nick(VentureListingOutcome outcome)
{
	return venture_enum_to_nick(VENTURE_TYPE_LISTING_OUTCOME, (gint)outcome);
}

static VentureListingOutcome
mirror_outcome(VentureEntity *listing)
{
	VentureListingOutcome outcome;

	outcome = VENTURE_LISTING_OUTCOME_OPEN;
	g_object_get(listing, "outcome", &outcome, NULL);

	return outcome;
}

/* Whether the outcome is still the one the mirror wrote. */
static gboolean
mirror_outcome_unedited(
	VentureEntity	*listing,
	JsonObject	*state
){
	return 0 == g_strcmp0(mirror_outcome_nick(mirror_outcome(listing)),
	                      mirror_state_string(state, "outcome"));
}

static gboolean
mirror_int_unedited(
	VentureEntity	*listing,
	const gchar	*property,
	JsonObject	*state,
	const gchar	*name
){
	return mirror_int(listing, property) == mirror_state_int(state, name);
}

static gboolean
mirror_money_unedited(
	VentureEntity	*listing,
	const gchar	*property,
	JsonObject	*state,
	const gchar	*name
){
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64 amount;

	if (!mirror_minor(listing, property, &amount, currency))
		return VENTURE_SERIES_NONE == mirror_state_int(state, name);

	return (amount == mirror_state_int(state, name)) &&
	       (0 == g_strcmp0(currency, mirror_state_string(state, "currency")));
}

static gboolean
mirror_time_unedited(
	VentureEntity	*listing,
	const gchar	*property,
	JsonObject	*state,
	const gchar	*name
){
	return mirror_unix(listing, property) == mirror_state_int(state, name);
}

/* --- Accounts and their locations --------------------------------------------------- */

static gchar *
mirror_account_ref(
	MirrorRun	*m,
	const gchar	*key
){
	return g_strdup_printf("%s:%s", m->settings.namespace_, key);
}

/*
 * Where the mirror last put a place, from its `mirror-state`: the parent
 * it gave it, or -1 when the place holds no such record (made before the
 * mirror kept one, or adopted by hand).
 */
static gint64
mirror_placed_parent(VentureEntity *location)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonNode *member;

	g_object_get(location, "mirror-state", &text, NULL);

	if (venture_string_is_empty(text))
		return -1;

	node = venture_json_parse(text, NULL);

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return -1;

	member = json_object_get_member(json_node_get_object(node), "parent_id");

	if ((NULL == member) || !JSON_NODE_HOLDS_VALUE(member) ||
	    (G_TYPE_INT64 != json_node_get_value_type(member)))
		return -1;

	return json_node_get_int(member);
}

/* What the pass needs to know of a location found by its reference. */
static MirrorKnown *
mirror_known_new(VentureEntity *location)
{
	MirrorKnown *known;

	known = g_new0(MirrorKnown, 1);
	known->id = venture_entity_get_id(location);
	known->deleted = venture_entity_is_deleted(location);
	known->parent_id = mirror_int(location, "parent-id");
	known->placed = mirror_placed_parent(location);
	known->data_source_id = mirror_int(location, "data-source-id");

	return known;
}

/*
 * Reads the organization's locations whose reference starts with this
 * source's namespace, deleted ones too, once per pass: one query rather
 * than one per account. Past the bound, a reference not in the table is
 * looked up on its own.
 */
static gboolean
mirror_load_location_refs(
	MirrorRun	 *m,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *pattern = NULL;
	guint i;

	query = venture_query_new(VENTURE_TYPE_LOCATION);
	venture_query_set_organization(query, m->organization_id);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, VENTURE_MARKETDATA_MIRROR_MAX_LISTINGS);

	/* The namespace holds no LIKE wildcard but '_', which only widens the
	 * read; the table is keyed by the exact reference. */
	pattern = g_strdup_printf("%s%%", m->settings.namespace_);

	if (!venture_query_add_filter_string(query, "external-ref", VENTURE_FILTER_OP_LIKE, pattern,
	                                     error))
		return FALSE;

	rows = venture_database_find(m->database, query, error);

	if (NULL == rows)
		return FALSE;

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *location = g_ptr_array_index(rows, i);
		g_autofree gchar *ref = NULL;
		MirrorKnown *known;

		g_object_get(location, "external-ref", &ref, NULL);

		if (venture_string_is_empty(ref))
			continue;

		known = mirror_known_new(location);
		g_hash_table_replace(m->location_refs, g_steal_pointer(&ref), known);
	}

	m->location_refs_complete = rows->len < VENTURE_MARKETDATA_MIRROR_MAX_LISTINGS;

	return TRUE;
}

/* The location with @ref: TRUE and *@out_id 0 when there is none. */
static gboolean
mirror_find_location(
	MirrorRun	 *m,
	const gchar	 *ref,
	gint64		 *out_id,
	gboolean	 *out_deleted,
	GError		**error
){
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(GError) lookup_error = NULL;
	MirrorKnown *known;

	*out_id = 0;
	*out_deleted = FALSE;
	known = g_hash_table_lookup(m->location_refs, ref);

	if (NULL != known)
	{
		*out_id = known->id;
		*out_deleted = known->deleted;
		return TRUE;
	}

	if (m->location_refs_complete)
		return TRUE;

	found = venture_marketdata_find_by_ref(m->database, VENTURE_TYPE_LOCATION, m->organization_id,
	                                      ref, &lookup_error);

	if (NULL != lookup_error)
	{
		g_propagate_error(error, g_steal_pointer(&lookup_error));
		return FALSE;
	}

	/* Kept, so the placement the hand-edit rule reads is at hand and a
	 * second question about the same place asks the database once. */
	if (NULL != found)
	{
		*out_id = venture_entity_get_id(found);
		*out_deleted = venture_entity_is_deleted(found);
		g_hash_table_replace(m->location_refs, g_strdup(ref), mirror_known_new(found));
	}

	return TRUE;
}

static void
mirror_remember_location(
	MirrorRun	*m,
	const gchar	*ref,
	gint64		 id
){
	MirrorKnown *known;

	known = g_new0(MirrorKnown, 1);
	known->id = id;
	known->placed = -1;
	known->data_source_id = m->source_id;
	g_hash_table_replace(m->location_refs, g_strdup(ref), known);
}

/*
 * Brings back a deleted location: one write, so only with room for it
 * (m->cut otherwise, and nothing done).
 */
static gboolean
mirror_restore_location(
	MirrorRun	 *m,
	gint64		  id,
	GError		**error
){
	g_autoptr(VentureEntity) location = NULL;
	g_autoptr(GError) local_error = NULL;
	gboolean ok;

	if (!mirror_has_room(m))
	{
		m->cut = TRUE;
		return TRUE;
	}

	location = venture_database_get(m->database, VENTURE_TYPE_LOCATION, id, error);

	if ((NULL == location) || !mirror_batch_open(m, error))
		return FALSE;

	ok = venture_database_restore(m->database, location, &m->actor, &local_error);

	if (!ok && mirror_database_failed(m, local_error))
	{
		g_propagate_error(error, g_steal_pointer(&local_error));
		return FALSE;
	}

	mirror_wrote(m, 1);

	if (!ok)
		g_propagate_error(error, g_steal_pointer(&local_error));

	return ok;
}

/* A store venue's row, from one read of the store's venues per pass. */
static const VentureSeriesVenueRow *
mirror_venue_row(
	MirrorRun	*m,
	const gchar	*key
){
	guint i;

	if (NULL == m->venue_rows)
	{
		g_autoptr(GError) error = NULL;

		m->venue_rows = venture_series_store_list_venues(m->store, &error);

		if (NULL == m->venue_rows)
		{
			mirror_note(m, "mirror: the store's venues could not be read: %s", error->message);
			m->venue_rows = g_ptr_array_new();
		}
	}

	for (i = 0; i < m->venue_rows->len; i++)
	{
		const VentureSeriesVenueRow *row = g_ptr_array_index(m->venue_rows, i);

		if (0 == g_strcmp0(row->key, key))
			return row;
	}

	return NULL;
}

/*
 * The venue record of a store venue key, promoted when there is none and
 * remembered for the pass; NULL when the store does not know it or a
 * person deleted its record. A promotion the bound has no room for sets
 * m->cut and is not remembered: the next pass makes it.
 */
static const MirrorVenue *
mirror_venue(
	MirrorRun	*m,
	const gchar	*key,
	gboolean	 restore
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) error = NULL;
	const VentureSeriesVenueRow *row;
	VentureMarketdataBudget budget;
	MirrorVenue *venue;

	if (venture_string_is_empty(key))
		return NULL;

	if (g_hash_table_contains(m->venues, key))
		return g_hash_table_lookup(m->venues, key);

	row = mirror_venue_row(m, key);
	venue = NULL;

	if (NULL != row)
	{
		guint before;

		mirror_budget(m, &budget);
		before = budget.left;

		if (!mirror_batch_open(m, &error) ||
		    !venture_marketdata_promote_venue_in(m->context, m->source, row, restore, &budget,
		                                         &m->actor, &record, &error))
		{
			if (m->broken || mirror_database_failed(m, error))
				return NULL;

			/* Remembered as no venue, so the note is said once. */
			mirror_note(m, "mirror: venue %s not promoted: %s", key, error->message);
		}
		else
		{
			mirror_wrote(m, before - budget.left);

			if (budget.cut)
			{
				m->cut = TRUE;
				return NULL;
			}
		}
	}

	if ((NULL != record) && !venture_entity_is_deleted(record))
	{
		venue = g_new0(MirrorVenue, 1);
		venue->id = venture_entity_get_id(record);
		g_object_get(record, "name", &venue->name, "location-id", &venue->location_id, NULL);
	}

	g_hash_table_insert(m->venues, g_strdup(key), venue);

	return venue;
}

/*
 * Points a venue at the account's location when the venue points at
 * none: the place its money moves through. An existing link is somebody's
 * decision and is never taken.
 */
static void
mirror_link_venue(
	MirrorRun	*m,
	MirrorVenue	*venue,
	gint64		 location_id
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) error = NULL;

	if ((location_id <= 0) || (NULL == venue) || (venue->location_id > 0))
		return;

	record = venture_database_get(m->database, VENTURE_TYPE_VENUE, venue->id, &error);

	if ((NULL == record) || (mirror_int(record, "location-id") > 0))
		return;

	g_object_set(record, "location-id", location_id, NULL);

	if (!mirror_write(m, record, &error))
	{
		mirror_failed(m, "a venue's location", error);
		return;
	}

	venue->location_id = location_id;
	m->counts.venues_linked++;
}

/*
 * A place an account's place sits inside -- a login, a group, a login's
 * group -- found by @ref, made when missing (@kind, @name, inside
 * @parent_id), restored only with @restore; 0 for a deleted one without
 * it. Short of room, m->cut and 0: the account waits for its parent
 * rather than being made without one nothing would give it later.
 */
static gboolean
mirror_parent_place(
	MirrorRun	 *m,
	const gchar	 *ref,
	const gchar	 *kind,
	const gchar	 *name,
	gint64		  parent_id,
	gboolean	  restore,
	gint64		 *out_id,
	GError		**error
){
	g_autoptr(VentureLocation) location = NULL;
	gboolean deleted;

	*out_id = 0;

	if (!mirror_find_location(m, ref, out_id, &deleted, error))
		return FALSE;

	if ((*out_id > 0) && deleted)
	{
		if (!restore)
		{
			*out_id = 0;
			return TRUE;
		}

		if (!mirror_restore_location(m, *out_id, error))
			return FALSE;

		if (m->cut)
		{
			*out_id = 0;
			return TRUE;
		}

		mirror_remember_location(m, ref, *out_id);
	}

	if (*out_id > 0)
		return TRUE;

	if (!mirror_has_room(m))
	{
		m->cut = TRUE;
		return TRUE;
	}

	location = venture_location_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(location), m->organization_id);
	g_object_set(location, "name", name, "kind", kind, "parent-id", parent_id, "active", TRUE,
	             "external-ref", ref, "data-source-id", m->source_id, NULL);

	if (!mirror_write(m, VENTURE_ENTITY(location), error))
		return FALSE;

	*out_id = venture_entity_get_id(VENTURE_ENTITY(location));
	mirror_remember_location(m, ref, *out_id);

	return TRUE;
}

/* The location for an account's group, made when missing; 0 for none. */
static gboolean
mirror_group_location(
	MirrorRun	 *m,
	const gchar	 *group,
	gboolean	  restore,
	gint64		 *out_id,
	GError		**error
){
	g_autofree gchar *ref = NULL;

	*out_id = 0;

	if (venture_string_is_empty(group))
		return TRUE;

	ref = g_strdup_printf("%s" MIRROR_GROUP_MARK "%s", m->settings.namespace_, group);

	return mirror_parent_place(m, ref, "group", group, 0, restore, out_id, error);
}

/*
 * The place an account's place belongs inside, made when missing. An
 * account reached through a login sits in that login's group (a realm
 * per login, "<ns>/login:<L>/group:<G>"), inside the login's own place
 * ("<ns>/login:<L>"), or straight inside the login's place when it has no
 * group: the login is the first thing to know about where a character
 * is, since reaching it means signing in with it. An account reached
 * through none keeps the chain it always had: its group, else nothing.
 * Under a bound, the login comes first and its group after it, each a
 * unit a later pass finds made.
 */
static gboolean
mirror_account_parent(
	MirrorRun			 *m,
	const VentureSeriesAccountRow	 *row,
	gboolean			  restore,
	gint64				 *out_id,
	GError				**error
){
	g_autofree gchar *login_ref = NULL;
	g_autofree gchar *group_ref = NULL;
	gint64 login_id;

	*out_id = 0;

	if (venture_string_is_empty(row->login_key))
		return mirror_group_location(m, row->group_key, restore, out_id, error);

	login_ref = g_strdup_printf("%s" MIRROR_LOGIN_MARK "%s", m->settings.namespace_, row->login_key);

	if (!mirror_parent_place(m, login_ref, "login",
	                         !venture_string_is_empty(row->login_name) ? row->login_name : row->login_key,
	                         0, restore, &login_id, error))
		return FALSE;

	/* A login's place a person deleted stays deleted, and nothing is made
	 * inside it: no realm place at the top standing for a login nobody
	 * wants filed. The account then has no parent, as with a deleted
	 * realm place. */
	if (m->cut || (0 == login_id) || venture_string_is_empty(row->group_key))
	{
		*out_id = login_id;
		return TRUE;
	}

	group_ref = g_strdup_printf("%s" MIRROR_GROUP_MARK "%s", login_ref, row->group_key);

	return mirror_parent_place(m, group_ref, "group", row->group_key, login_id, restore, out_id, error);
}

/*
 * Whether an account's place is where the mirror put it -- the only kind
 * of place the mirror ever moves. Its `mirror-state` says where that was;
 * a parent that differs is a person's move and stands for good. A place
 * with no such record (made before the mirror kept one, or adopted by a
 * person) counts as the mirror's only when it is filed under this source
 * and sits exactly where the earlier rule left every place it made: in
 * its group's place, or at the top when its account has no group or that
 * group's place was deleted. Anywhere else -- including the top, for an
 * account whose group's place never existed -- somebody put it there.
 */
static gboolean
mirror_placed_by_mirror(
	MirrorRun			*m,
	const VentureSeriesAccountRow	*row,
	const MirrorKnown		*known
){
	g_autofree gchar *group_ref = NULL;
	g_autoptr(GError) error = NULL;
	gboolean deleted;
	gint64 group_id;

	if (known->placed >= 0)
		return known->parent_id == known->placed;

	if (known->data_source_id != m->source_id)
		return FALSE;

	if (venture_string_is_empty(row->group_key))
		return 0 == known->parent_id;

	group_ref = g_strdup_printf("%s" MIRROR_GROUP_MARK "%s", m->settings.namespace_, row->group_key);

	if (!mirror_find_location(m, group_ref, &group_id, &deleted, &error))
		return FALSE;

	if (0 == group_id)
		return FALSE;

	return known->parent_id == (deleted ? 0 : group_id);
}

/* The JSON an account's place records where the mirror put it with. */
static gchar *
mirror_placement_text(gint64 parent_id)
{
	return g_strdup_printf("{\"parent_id\":%" G_GINT64_FORMAT "}", parent_id);
}

/*
 * Moves an existing account's place to where the chain now puts it --
 * a login that appeared, a group that changed -- when the mirror put it
 * where it is and nobody moved it since (mirror_placed_by_mirror()). A
 * deleted place, or a parent that comes to no live place (deleted, or
 * waiting for room), leaves it where it is.
 */
static gboolean
mirror_place_account(
	MirrorRun			 *m,
	const VentureSeriesAccountRow	 *row,
	const gchar			 *ref,
	gboolean			  restore,
	GError				**error
){
	g_autoptr(VentureEntity) location = NULL;
	g_autofree gchar *state = NULL;
	MirrorKnown *known;
	gint64 target;

	known = g_hash_table_lookup(m->location_refs, ref);

	if ((NULL == known) || known->deleted || (known->id <= 0))
		return TRUE;

	/* A person's placement is counted, never computed against: working
	 * out where the chain would put it could make places for nothing. */
	if (!mirror_placed_by_mirror(m, row, known))
	{
		m->locations_kept++;
		return TRUE;
	}

	if (!mirror_account_parent(m, row, restore, &target, error))
		return FALSE;

	if (m->cut || (target <= 0) || (target == known->parent_id))
		return TRUE;

	if (!mirror_has_room(m))
	{
		m->cut = TRUE;
		return TRUE;
	}

	location = venture_database_get(m->database, VENTURE_TYPE_LOCATION, known->id, error);

	if (NULL == location)
		return FALSE;

	state = mirror_placement_text(target);
	g_object_set(location, "parent-id", target, "mirror-state", state, NULL);

	if (!mirror_write(m, location, error))
		return FALSE;

	known->parent_id = target;
	known->placed = target;
	m->counts.reparented++;

	return TRUE;
}

/*
 * The location of one account: found, or (with @restore) restored, or
 * made inside its login's and group's (mirror_account_parent()). With
 * @restore FALSE a deleted one -- or a deleted parent or venue -- stays
 * deleted, and a deleted location's answer is 0. The account's venue is
 * linked to it when the location is made, and on every explicit
 * (@restore) promotion. A place that exists is moved to where the chain
 * now puts it, under the hand-edit rule (mirror_place_account()).
 *
 * Under a bound, the parents and the venue come first, each a unit of its
 * own that a later pass finds made; the location and its venue's link
 * are written together or not at all, since nothing links a venue to a
 * place that already exists. Short of room: m->cut and 0.
 */
static gboolean
mirror_promote_account(
	MirrorRun			 *m,
	const VentureSeriesAccountRow	 *row,
	gboolean			  restore,
	gint64				 *out_id,
	GError				**error
){
	g_autoptr(VentureLocation) location = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *state = NULL;
	MirrorVenue *venue;
	gboolean deleted;
	gint64 parent_id;

	*out_id = 0;
	ref = mirror_account_ref(m, row->key);

	if (!mirror_find_location(m, ref, out_id, &deleted, error))
		return FALSE;

	if (*out_id > 0)
	{
		if (deleted)
		{
			if (!restore)
			{
				*out_id = 0;
				return TRUE;
			}

			if (!mirror_restore_location(m, *out_id, error))
				return FALSE;

			if (m->cut)
			{
				*out_id = 0;
				return TRUE;
			}

			/* Restored where it was, its placement with it. */
			{
				MirrorKnown *known = g_hash_table_lookup(m->location_refs, ref);

				if (NULL != known)
					known->deleted = FALSE;
				else
					mirror_remember_location(m, ref, *out_id);
			}
		}

		if (!mirror_place_account(m, row, ref, restore, error))
			return FALSE;

		if (restore)
		{
			venue = (MirrorVenue *)mirror_venue(m, row->venue_key, restore);
			mirror_link_venue(m, venue, *out_id);
		}

		return TRUE;
	}

	if (!mirror_account_parent(m, row, restore, &parent_id, error))
		return FALSE;

	if (m->cut)
		return TRUE;

	venue = (MirrorVenue *)mirror_venue(m, row->venue_key, restore);

	if (m->cut || m->broken)
		return TRUE;

	if (!mirror_has_room_for(m, ((NULL != venue) && (venue->location_id <= 0)) ? 2 : 1))
	{
		m->cut = TRUE;
		return TRUE;
	}

	state = mirror_placement_text(parent_id);
	location = venture_location_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(location), m->organization_id);
	g_object_set(location,
	             "name", !venture_string_is_empty(row->name) ? row->name : row->key,
	             "kind", !venture_string_is_empty(row->kind) ? row->kind : "other",
	             "parent-id", parent_id,
	             "active", TRUE,
	             "external-ref", ref,
	             "data-source-id", m->source_id,
	             "mirror-state", state,
	             NULL);

	if (!mirror_write(m, VENTURE_ENTITY(location), error))
		return FALSE;

	*out_id = venture_entity_get_id(VENTURE_ENTITY(location));
	mirror_remember_location(m, ref, *out_id);

	{
		MirrorKnown *known = g_hash_table_lookup(m->location_refs, ref);

		known->parent_id = parent_id;
		known->placed = parent_id;
	}

	m->counts.promoted++;
	mirror_link_venue(m, venue, *out_id);

	return TRUE;
}

/*
 * Step 1: every account's location, promoting the ones that have none
 * when the source says so. Fills m->account_locations for the positions.
 */
static void
mirror_accounts(
	MirrorRun	*m,
	GPtrArray	*accounts
){
	guint i;

	for (i = 0; i < accounts->len; i++)
	{
		const VentureSeriesAccountRow *row = g_ptr_array_index(accounts, i);
		g_autofree gchar *ref = mirror_account_ref(m, row->key);
		g_autoptr(GError) error = NULL;
		gboolean deleted;
		gint64 id;

		g_hash_table_insert(m->accounts, row->key, (gpointer)row);

		if (!mirror_find_location(m, ref, &id, &deleted, &error))
		{
			mirror_failed(m, "an account's location", error);
			continue;
		}

		if ((0 == id) && m->settings.auto_promote && !m->broken)
		{
			if (!mirror_has_room(m))
			{
				m->over_cap++;
				continue;
			}

			m->cut = FALSE;

			if (!mirror_promote_account(m, row, FALSE, &id, &error))
			{
				mirror_failed(m, "an account's location", error);
				id = 0;
			}
			else if (m->cut)
			{
				m->over_cap++;
				m->cut = FALSE;
				id = 0;
			}
		}
		else if (deleted)
			id = 0;
		else if ((id > 0) && m->settings.auto_promote && !m->broken && mirror_has_room(m))
		{
			/* A place that exists follows its account's login and group
			 * where the mirror put it; a cut here waits for the next
			 * pass and changes nothing else. */
			m->cut = FALSE;

			if (!mirror_place_account(m, row, ref, FALSE, &error))
				mirror_failed(m, "an account's location", error);
			else if (m->cut)
				m->over_cap++;

			m->cut = FALSE;
		}

		g_hash_table_insert(m->account_locations, g_strdup(row->key), g_memdup2(&id, sizeof(id)));
	}
}

static gint64
mirror_account_location(
	MirrorRun	*m,
	const gchar	*account_key
){
	gint64 *id;

	id = g_hash_table_lookup(m->account_locations, account_key);

	return (NULL != id) ? *id : 0;
}

/* --- Products ------------------------------------------------------------------------- */

#define MIRROR_DELETED_INSTRUMENT	(-1)
#define MIRROR_DELETED_PRODUCT		(-2)

/*
 * The product a position's instrument comes to: its instrument record's
 * `product-id`, the instrument promoted first when it has no record. With
 * `create_products` a product is made in the configured venture and the
 * instrument linked to it, the two written together or not at all -- a
 * product left unlinked would be made again by the next pass. 0: none;
 * MIRROR_DELETED_INSTRUMENT: a person deleted the instrument;
 * MIRROR_DELETED_PRODUCT: a person deleted its product, which the
 * listing's reference check would refuse on every pass, and which is not
 * replaced: a deleted product is a decision, not a gap to fill. Short of
 * room (m->cut) or after a database error (m->broken): 0, remembered for
 * nothing, so the next pass asks again.
 */
static gint64
mirror_product(
	MirrorRun	*m,
	const gchar	*instrument_key
){
	g_autoptr(VentureEntity) instrument = NULL;
	g_autoptr(GError) error = NULL;
	VentureMarketdataBudget budget;
	gint64 *cached;
	gint64 product_id;
	guint before;

	cached = g_hash_table_lookup(m->products, instrument_key);

	if (NULL != cached)
		return *cached;

	product_id = 0;
	mirror_budget(m, &budget);
	before = budget.left;

	if (!mirror_batch_open(m, &error) ||
	    !venture_marketdata_promote_instrument_in(m->context, m->store, m->source, instrument_key,
	                                              FALSE, &budget, &m->actor, &instrument, &error))
	{
		if (m->broken || mirror_database_failed(m, error))
			return 0;

		mirror_note(m, "mirror: instrument %s not promoted: %s", instrument_key, error->message);
	}
	else
	{
		mirror_wrote(m, before - budget.left);

		if (budget.cut)
		{
			m->cut = TRUE;
			return 0;
		}

		if (venture_entity_is_deleted(instrument))
			product_id = MIRROR_DELETED_INSTRUMENT;
		else
			product_id = mirror_int(instrument, "product-id");
	}

	if (product_id > 0)
	{
		g_autoptr(VentureEntity) product = NULL;

		product = venture_database_get(m->database, VENTURE_TYPE_PRODUCT, product_id, NULL);

		if ((NULL == product) || venture_entity_is_deleted(product))
			product_id = MIRROR_DELETED_PRODUCT;
	}

	if ((0 == product_id) && (NULL != instrument) && m->settings.create_products)
	{
		g_autoptr(VentureProduct) product = NULL;
		g_autofree gchar *name = NULL;

		if (!mirror_has_room_for(m, 2))
		{
			m->cut = TRUE;
			return 0;
		}

		g_object_get(instrument, "name", &name, NULL);
		product = venture_product_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(product), m->organization_id);
		g_object_set(product, "name", !venture_string_is_empty(name) ? name : instrument_key,
		             "venture-id", m->settings.products_venture_id, NULL);

		if (!mirror_write(m, VENTURE_ENTITY(product), &error))
		{
			if (m->broken)
				return 0;

			mirror_failed(m, "a product", error);
		}
		else
		{
			m->counts.products_created++;
			product_id = venture_entity_get_id(VENTURE_ENTITY(product));

			/* Linked, so the oracle, the alerts and the next pass
			 * all find it through the instrument. */
			g_object_set(instrument, "product-id", product_id, NULL);

			if (!mirror_write(m, instrument, &error))
			{
				if (m->broken)
					return 0;

				mirror_failed(m, "an instrument's product", error);
			}
		}
	}

	g_hash_table_insert(m->products, g_strdup(instrument_key), g_memdup2(&product_id, sizeof(product_id)));

	return product_id;
}

/* --- Positions present ---------------------------------------------------------------- */

static gchar *
mirror_external_id(
	MirrorRun	*m,
	const gchar	*position_key
){
	return g_strdup_printf("%s:%s", m->uuid, position_key);
}

/* Whether @listing is one this source's mirror made. */
static gboolean
mirror_owns(
	MirrorRun	*m,
	VentureEntity	*listing
){
	g_autofree gchar *external_id = NULL;
	g_autofree gchar *prefix = NULL;

	if (mirror_int(listing, "data-source-id") != m->source_id)
		return FALSE;

	g_object_get(listing, "external-id", &external_id, NULL);
	prefix = g_strdup_printf("%s:", m->uuid);

	return (NULL != external_id) && g_str_has_prefix(external_id, prefix);
}

/* When a listing opened: when it was posted, else when it was first seen. */
static gint64
mirror_listed_at(const VentureSeriesPositionRow *position)
{
	gint64 listed;

	listed = (VENTURE_SERIES_NONE != position->posted_at) ? position->posted_at
	                                                      : position->first_seen;

	/* A source's clock can put the posting after the expiry; a listing
	 * that runs out before it opened is refused, so it opened then. */
	if ((VENTURE_SERIES_NONE != position->expires_at) && (position->expires_at < listed))
		listed = position->expires_at;

	return listed;
}

/* A new listing for a position nobody has mirrored. */
static void
mirror_create(
	MirrorRun			*m,
	const VentureSeriesPositionRow	*position,
	gint64				 product_id
){
	g_autoptr(VentureListing) listing = NULL;
	g_autoptr(JsonObject) state = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *external_id = NULL;
	const MirrorVenue *venue;
	gint64 location_id;

	venue = mirror_venue(m, position->venue_key, FALSE);

	/* The venue's promotion took the last of the room, or the database
	 * failed: a listing made now would name no venue for good. */
	if (m->cut || m->broken)
	{
		if (m->cut)
			m->over_cap++;

		return;
	}

	if (!mirror_has_room(m))
	{
		m->over_cap++;
		return;
	}

	location_id = mirror_account_location(m, position->account_key);
	external_id = mirror_external_id(m, position->key);

	listing = venture_listing_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(listing), m->organization_id);
	g_object_set(listing,
	             "product-id", product_id,
	             "venue-id", (NULL != venue) ? venue->id : (gint64)0,
	             "channel", (NULL != venue) ? venue->name : position->venue_key,
	             "location-id", location_id,
	             "quantity", position->quantity,
	             "quantity-sold", (gint64)0,
	             "outcome", VENTURE_LISTING_OUTCOME_OPEN,
	             "external-id", external_id,
	             "data-source-id", m->source_id,
	             NULL);
	mirror_set_money(VENTURE_ENTITY(listing), "unit-price", position->unit_price, position->currency);
	mirror_set_money(VENTURE_ENTITY(listing), "bid", position->bid, position->currency);
	mirror_set_unix(VENTURE_ENTITY(listing), "listed-at", mirror_listed_at(position));
	mirror_set_unix(VENTURE_ENTITY(listing), "expires-at", position->expires_at);

	state = json_object_new();
	json_object_set_string_member(state, "account", position->account_key);
	json_object_set_string_member(state, "instrument", position->instrument_key);
	json_object_set_string_member(state, "venue", (NULL != position->venue_key) ? position->venue_key : "");
	json_object_set_int_member(state, "quantity", position->quantity);
	json_object_set_int_member(state, "sold", 0);
	json_object_set_int_member(state, "unit_price", position->unit_price);
	json_object_set_string_member(state, "currency", position->currency);
	mirror_state_set_int(state, "bid", position->bid);
	mirror_state_set_int(state, "expires_at", position->expires_at);
	json_object_set_int_member(state, "location_id", location_id);
	json_object_set_string_member(state, "outcome", mirror_outcome_nick(VENTURE_LISTING_OUTCOME_OPEN));
	mirror_state_store(VENTURE_ENTITY(listing), state);

	if (!mirror_write(m, VENTURE_ENTITY(listing), &error))
	{
		g_autofree gchar *what = g_strdup_printf("the listing of position %s", position->key);

		mirror_failed(m, what, error);
		return;
	}

	m->counts.created++;
}

/*
 * Keeps a mirrored listing in step with its present position: each field
 * only while it holds what the mirror last wrote. A listing the mirror
 * had closed is open again -- the position is there, so whatever made it
 * look gone was wrong. Units that left a position still present were
 * sold: an auction loses units only to buyers.
 */
static void
mirror_update(
	MirrorRun			*m,
	VentureEntity			*listing,
	JsonObject			*state,
	const VentureSeriesPositionRow	*position
){
	g_autoptr(GError) error = NULL;
	gboolean changed;
	gboolean reopened;
	gint64 quantity;
	gint64 sold;
	gint64 location_id;
	gint64 listed;

	changed = FALSE;
	reopened = FALSE;

	if (venture_market_listing_outcome_is_closed(mirror_outcome(listing)))
	{
		g_object_set(listing, "outcome", VENTURE_LISTING_OUTCOME_OPEN, "closed-at", NULL, NULL);
		json_object_set_string_member(state, "outcome",
		                              mirror_outcome_nick(VENTURE_LISTING_OUTCOME_OPEN));
		reopened = TRUE;
		changed = TRUE;
	}

	if (json_object_has_member(state, "vanished_at") || json_object_has_member(state, "txns") ||
	    json_object_has_member(state, "evidence"))
	{
		json_object_remove_member(state, "vanished_at");
		json_object_remove_member(state, "txns");
		json_object_remove_member(state, "evidence");
		changed = TRUE;
	}

	quantity = mirror_int(listing, "quantity");

	if ((position->quantity > quantity) && mirror_int_unedited(listing, "quantity", state, "quantity"))
	{
		quantity = position->quantity;
		g_object_set(listing, "quantity", quantity, NULL);
		json_object_set_int_member(state, "quantity", quantity);
		changed = TRUE;
	}

	sold = MAX((gint64)0, quantity - position->quantity);

	if ((sold != mirror_int(listing, "quantity-sold")) &&
	    mirror_int_unedited(listing, "quantity-sold", state, "sold"))
	{
		g_object_set(listing, "quantity-sold", sold, NULL);
		json_object_set_int_member(state, "sold", sold);
		changed = TRUE;
	}

	if (mirror_money_unedited(listing, "unit-price", state, "unit_price") &&
	    ((position->unit_price != mirror_state_int(state, "unit_price")) ||
	     (0 != g_strcmp0(position->currency, mirror_state_string(state, "currency")))))
	{
		mirror_set_money(listing, "unit-price", position->unit_price, position->currency);
		json_object_set_int_member(state, "unit_price", position->unit_price);
		json_object_set_string_member(state, "currency", position->currency);
		changed = TRUE;
	}

	if (mirror_money_unedited(listing, "bid", state, "bid") &&
	    (position->bid != mirror_state_int(state, "bid")))
	{
		mirror_set_money(listing, "bid", position->bid, position->currency);
		mirror_state_set_int(state, "bid", position->bid);
		changed = TRUE;
	}

	listed = mirror_unix(listing, "listed-at");

	if ((position->expires_at != mirror_state_int(state, "expires_at")) &&
	    mirror_time_unedited(listing, "expires-at", state, "expires_at") &&
	    ((VENTURE_SERIES_NONE == position->expires_at) || (VENTURE_SERIES_NONE == listed) ||
	     (position->expires_at >= listed)))
	{
		mirror_set_unix(listing, "expires-at", position->expires_at);
		mirror_state_set_int(state, "expires_at", position->expires_at);
		changed = TRUE;
	}

	/* An account with no place now (switched off, or deleted by a
	 * person) says nothing about where the listing was posted. */
	location_id = mirror_account_location(m, position->account_key);

	if ((location_id > 0) && (location_id != mirror_state_int(state, "location_id")) &&
	    mirror_int_unedited(listing, "location-id", state, "location_id"))
	{
		g_object_set(listing, "location-id", location_id, NULL);
		json_object_set_int_member(state, "location_id", location_id);
		changed = TRUE;
	}

	/* The same id under another account moved there. */
	if (0 != g_strcmp0(position->account_key, mirror_state_string(state, "account")))
	{
		json_object_set_string_member(state, "account", position->account_key);
		changed = TRUE;
	}

	if (!changed)
		return;

	if (!mirror_has_room(m))
	{
		m->over_cap++;
		return;
	}

	mirror_state_store(listing, state);

	if (!mirror_write(m, listing, &error))
	{
		g_autofree gchar *what = g_strdup_printf("listing #%" G_GINT64_FORMAT,
		                                         venture_entity_get_id(listing));

		mirror_failed(m, what, error);
		return;
	}

	if (reopened)
		m->counts.reopened++;
	else
		m->counts.updated++;
}

/* The source's open mirrored listings, by external id. */
static GHashTable *
mirror_open_listings(
	MirrorRun	 *m,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	GHashTable *table;
	guint i;

	query = venture_query_new(VENTURE_TYPE_LISTING);
	venture_query_set_organization(query, m->organization_id);
	venture_query_set_limit(query, VENTURE_MARKETDATA_MIRROR_MAX_LISTINGS);

	if (!venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ, m->source_id,
	                                  error) ||
	    !venture_query_add_filter_int(query, "outcome", VENTURE_FILTER_OP_EQ,
	                                  VENTURE_LISTING_OUTCOME_OPEN, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	rows = venture_database_find(m->database, query, error);

	if (NULL == rows)
		return NULL;

	if (rows->len >= VENTURE_MARKETDATA_MIRROR_MAX_LISTINGS)
		mirror_note(m, "mirror: more than %d open mirrored listings; only the oldest %d were read",
		            VENTURE_MARKETDATA_MIRROR_MAX_LISTINGS, VENTURE_MARKETDATA_MIRROR_MAX_LISTINGS);

	table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *listing = g_ptr_array_index(rows, i);
		g_autofree gchar *external_id = NULL;

		g_object_get(listing, "external-id", &external_id, NULL);

		if (!venture_string_is_empty(external_id) && mirror_owns(m, listing))
			g_hash_table_replace(table, g_steal_pointer(&external_id), g_object_ref(listing));
	}

	return table;
}

/*
 * Every listing of the organization, deleted ones included, carrying one
 * of @external_ids -- the positions with no open mirrored listing, which
 * may have a closed or deleted one, or one the mirror did not make.
 */
static gboolean
mirror_listings_by_id(
	MirrorRun	 *m,
	GPtrArray	 *external_ids,
	GHashTable	 *into,
	GError		**error
){
	guint start;

	for (start = 0; start < external_ids->len; start += MIRROR_IN_BATCH)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) values = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		guint i;

		values = g_ptr_array_new();

		for (i = start; (i < external_ids->len) && (i < start + MIRROR_IN_BATCH); i++)
			g_ptr_array_add(values, g_ptr_array_index(external_ids, i));

		query = venture_query_new(VENTURE_TYPE_LISTING);
		venture_query_set_organization(query, m->organization_id);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_set_limit(query, 0);

		if (!venture_query_add_filter(query, "external-id", VENTURE_FILTER_OP_IN, values, error))
			return FALSE;

		rows = venture_database_find(m->database, query, error);

		if (NULL == rows)
			return FALSE;

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *listing = g_ptr_array_index(rows, i);
			g_autofree gchar *external_id = NULL;

			g_object_get(listing, "external-id", &external_id, NULL);

			if (!venture_string_is_empty(external_id))
				g_hash_table_replace(into, g_steal_pointer(&external_id), g_object_ref(listing));
		}
	}

	return TRUE;
}

/*
 * Step 2: every position the store holds, in its order (soonest expiry
 * first, so a capped pass mirrors the urgent ones), made or kept in step.
 * @seen collects the external ids that are present.
 */
static gboolean
mirror_present(
	MirrorRun	 *m,
	GPtrArray	 *positions,
	GHashTable	 *open,
	GHashTable	 *seen,
	GError		**error
){
	g_autoptr(GHashTable) others = NULL;
	g_autoptr(GPtrArray) wanted = NULL;
	guint i;

	others = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	wanted = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < positions->len; i++)
	{
		const VentureSeriesPositionRow *position = g_ptr_array_index(positions, i);
		gchar *external_id = mirror_external_id(m, position->key);

		if (!g_hash_table_contains(open, external_id))
			g_ptr_array_add(wanted, g_strdup(external_id));

		g_hash_table_add(seen, external_id);
	}

	if ((wanted->len > 0) && !mirror_listings_by_id(m, wanted, others, error))
		return FALSE;

	for (i = 0; (i < positions->len) && !m->broken; i++)
	{
		const VentureSeriesPositionRow *position = g_ptr_array_index(positions, i);
		g_autofree gchar *external_id = mirror_external_id(m, position->key);
		g_autoptr(JsonObject) state = NULL;
		VentureEntity *listing;

		listing = g_hash_table_lookup(open, external_id);

		if (NULL == listing)
			listing = g_hash_table_lookup(others, external_id);

		if (NULL == listing)
		{
			gint64 product_id;

			if ('\0' == position->currency[0])
			{
				m->not_mirrored++;
				continue;
			}

			if (!mirror_has_room(m))
			{
				m->over_cap++;
				continue;
			}

			m->cut = FALSE;
			product_id = mirror_product(m, position->instrument_key);

			if (m->broken)
				break;

			if (m->cut)
			{
				m->over_cap++;
				continue;
			}

			if (MIRROR_DELETED_INSTRUMENT == product_id)
			{
				m->deleted_instruments++;
				m->not_mirrored++;
				continue;
			}

			if (MIRROR_DELETED_PRODUCT == product_id)
			{
				m->deleted_products++;
				m->not_mirrored++;
				g_hash_table_add(m->unsellable, g_strdup(position->instrument_key));
				continue;
			}

			if (0 == product_id)
			{
				m->not_mirrored++;
				g_hash_table_add(m->unpriced, g_strdup(position->instrument_key));
				continue;
			}

			m->cut = FALSE;
			mirror_create(m, position, product_id);
			continue;
		}

		/* A person deleted it, or it is not the mirror's: never touched. */
		if (venture_entity_is_deleted(listing) || !mirror_owns(m, listing))
		{
			m->left_alone++;
			continue;
		}

		state = mirror_state_read(listing);

		/* An outcome a person set makes the listing theirs. */
		if ((NULL == state) || !mirror_outcome_unedited(listing, state))
		{
			m->left_alone++;
			continue;
		}

		mirror_update(m, listing, state, position);
	}

	return TRUE;
}

/* --- Positions gone: judging from the ledger -------------------------------------------- */

/*
 * A listing taking part in its group's matching. A vanished one is
 * judged; a present one (@present) only takes the sale rows of the units
 * it lost while listed, so that a vanished listing cannot take them, and
 * is never judged -- it is still on the market.
 */
typedef struct
{
	VentureEntity	*listing;
	JsonObject	*state;
	gboolean	 present;
	const gchar	*venue;
	gint64		 listed_at;
	gint64		 expires_at;
	gint64		 quantity;
	gint64		 sold_floor;
	gint64		 vanished_at;
	gint64		 sale_units;
	gint64		 end_units;
	gboolean	 any_cancel;
	gint64		 last_at;
	JsonObject	*used;		/* ledger key -> units */
} MirrorCandidate;

static void
mirror_candidate_free(gpointer data)
{
	MirrorCandidate *candidate = data;

	g_object_unref(candidate->listing);
	json_object_unref(candidate->state);
	json_object_unref(candidate->used);
	g_free(candidate);
}

/* First in, first out: the listing posted first takes a ledger row first. */
static gint
mirror_compare_candidates(
	gconstpointer	a,
	gconstpointer	b
){
	const MirrorCandidate *x = *(const MirrorCandidate *const *)a;
	const MirrorCandidate *y = *(const MirrorCandidate *const *)b;
	gint64 xi;
	gint64 yi;

	if (x->listed_at != y->listed_at)
		return (x->listed_at < y->listed_at) ? -1 : 1;

	xi = venture_entity_get_id(x->listing);
	yi = venture_entity_get_id(y->listing);

	return (xi < yi) ? -1 : ((xi > yi) ? 1 : 0);
}

static gint
mirror_compare_txns(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureSeriesTxnRow *x = *(const VentureSeriesTxnRow *const *)a;
	const VentureSeriesTxnRow *y = *(const VentureSeriesTxnRow *const *)b;

	if (x->at != y->at)
		return (x->at < y->at) ? -1 : 1;

	return g_strcmp0(x->key, y->key);
}

static gchar *
mirror_group_key(
	const gchar	*account,
	const gchar	*instrument
){
	return g_strdup_printf("%s\x1f%s", (NULL != account) ? account : "",
	                       (NULL != instrument) ? instrument : "");
}

/* Adds what a closed listing's judgement used of each ledger row. */
static void
mirror_add_consumption(
	GHashTable	*consumed,
	JsonObject	*state
){
	JsonNode *node;
	JsonObjectIter iter;
	const gchar *key;
	JsonNode *value;

	node = json_object_get_member(state, "txns");

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return;

	json_object_iter_init(&iter, json_node_get_object(node));

	while (json_object_iter_next(&iter, &key, &value))
	{
		gint64 *units;

		if (!JSON_NODE_HOLDS_VALUE(value) || (G_TYPE_INT64 != json_node_get_value_type(value)))
			continue;

		units = g_hash_table_lookup(consumed, key);

		if (NULL == units)
		{
			units = g_new0(gint64, 1);
			g_hash_table_insert(consumed, g_strdup(key), units);
		}

		*units += json_node_get_int(value);
	}
}

/* Whether @txn can be @c's: listed by then, and at the same venue. */
static gboolean
mirror_eligible(
	const MirrorCandidate		*c,
	const VentureSeriesTxnRow	*txn
){
	/* Nothing sells or ends before it is listed, and a row from another
	 * venue is another venue's listing. */
	return (txn->at >= c->listed_at) &&
	       (venture_string_is_empty(txn->venue_key) || venture_string_is_empty(c->venue) ||
	        (0 == g_strcmp0(txn->venue_key, c->venue)));
}

/* What of @c no row has accounted for yet. */
static gint64
mirror_remainder(const MirrorCandidate *c)
{
	return c->quantity - MAX(c->sale_units, c->sold_floor) - c->end_units;
}

static void
mirror_take(
	MirrorCandidate			*c,
	const VentureSeriesTxnRow	*txn,
	gint64				 take,
	gboolean			 sale
){
	if (sale)
		c->sale_units += take;
	else
	{
		c->end_units += take;
		c->any_cancel = c->any_cancel || (0 == g_strcmp0(txn->kind, "cancelled"));
	}

	c->last_at = MAX(c->last_at, txn->at);
	json_object_set_int_member(c->used, txn->key,
	                           json_object_get_int_member_with_default(c->used, txn->key, 0) + take);
}

/*
 * Units of one ledger row handed to the group's vanished listings, oldest
 * first; returns the units nobody took. Present listings took their own
 * sales before this (mirror_reserve) and take nothing here.
 *
 * A sale's units fill listings in order, each taking up to what it has
 * not sold: a buyer may take part of a stack, and a source may merge two
 * sales of one item into one row.
 *
 * An expiry or a cancellation ends a listing's whole remainder at once --
 * nothing expires a unit at a time -- so it goes first to the oldest
 * listing whose remainder is exactly the row's quantity; failing that,
 * whole remainders are taken oldest first while they fit, which is how a
 * row two expiries were merged into is split. What fits nowhere is left
 * unused rather than shaved off a bigger listing.
 */
static gint64
mirror_assign(
	GPtrArray			*candidates,
	const VentureSeriesTxnRow	*txn,
	gint64				 units,
	gboolean			 sale
){
	guint i;

	if (sale)
	{
		for (i = 0; (i < candidates->len) && (units > 0); i++)
		{
			MirrorCandidate *c = g_ptr_array_index(candidates, i);
			gint64 demand;
			gint64 take;

			if (c->present || !mirror_eligible(c, txn))
				continue;

			demand = c->quantity - c->sale_units;

			if (demand <= 0)
				continue;

			take = MIN(units, demand);
			units -= take;
			mirror_take(c, txn, take, TRUE);
		}

		return units;
	}

	for (i = 0; i < candidates->len; i++)
	{
		MirrorCandidate *c = g_ptr_array_index(candidates, i);

		if (!c->present && mirror_eligible(c, txn) && (mirror_remainder(c) == units))
		{
			mirror_take(c, txn, units, FALSE);
			return 0;
		}
	}

	for (i = 0; (i < candidates->len) && (units > 0); i++)
	{
		MirrorCandidate *c = g_ptr_array_index(candidates, i);
		gint64 rest;

		if (c->present || !mirror_eligible(c, txn))
			continue;

		rest = mirror_remainder(c);

		if ((rest > 0) && (rest <= units))
		{
			units -= rest;
			mirror_take(c, txn, rest, FALSE);
		}
	}

	return units;
}

/*
 * Before anything is handed out: every listing's units sold while it was
 * listed (`quantity-sold`, the floor) take sale rows of their own, oldest
 * listing first, each from the earliest rows it can have. Those units are
 * known to be that listing's -- a position still on the market that lost
 * units lost them to buyers -- so a row they account for is not free for
 * an older vanished listing to take by first in, first out. Oldest first
 * from the earliest rows is also what leaves the later rows, the only
 * ones a newer listing can have, to the newer listings. @left holds each
 * row's units not yet used, and is spent here.
 */
static void
mirror_reserve(
	GPtrArray	*candidates,
	GPtrArray	*txns,
	gint64		*left
){
	guint i;
	guint j;

	for (i = 0; i < candidates->len; i++)
	{
		MirrorCandidate *c = g_ptr_array_index(candidates, i);

		for (j = 0; (j < txns->len) && (c->sale_units < MIN(c->sold_floor, c->quantity)); j++)
		{
			const VentureSeriesTxnRow *txn = g_ptr_array_index(txns, j);
			gint64 take;

			if ((left[j] <= 0) || (0 != g_strcmp0(txn->kind, "sale")) || !mirror_eligible(c, txn))
				continue;

			take = MIN(left[j], MIN(c->sold_floor, c->quantity) - c->sale_units);
			left[j] -= take;
			mirror_take(c, txn, take, TRUE);
		}
	}
}

/*
 * Judges one vanished listing from what the ledger rows gave it, and
 * writes the judgement -- or, with nothing yet to go on and the grace not
 * spent, writes when it was found gone and waits.
 */
static void
mirror_judge(
	MirrorRun	*m,
	MirrorCandidate	*c
){
	g_autoptr(GError) error = NULL;
	VentureListingOutcome outcome;
	const gchar *evidence;
	gint64 sold;
	gint64 ended;
	gint64 rest;
	gint64 closed;

	sold = MIN(c->quantity, MAX(c->sale_units, c->sold_floor));
	ended = MIN(c->end_units, c->quantity - sold);
	rest = c->quantity - sold - ended;

	if (0 == rest)
	{
		evidence = "ledger";
		closed = (VENTURE_SERIES_NONE != c->last_at) ? c->last_at : c->vanished_at;
	}
	else if ((VENTURE_SERIES_NONE != c->expires_at) && (c->expires_at <= c->vanished_at))
	{
		/* Gone at or after its expiry: what nothing accounts for ran out. */
		evidence = "expiry";
		closed = MAX(c->expires_at, c->last_at);
	}
	else if ((m->now - c->vanished_at) < (gint64)m->settings.grace_hours * 3600)
	{
		m->waiting++;

		if (VENTURE_SERIES_NONE != mirror_state_int(c->state, "vanished_at"))
			return;

		if (!mirror_has_room(m))
		{
			m->over_cap++;
			return;
		}

		json_object_set_int_member(c->state, "vanished_at", c->vanished_at);
		mirror_state_store(c->listing, c->state);

		if (!mirror_write(m, c->listing, &error))
			mirror_failed(m, "a vanished listing", error);

		return;
	}
	else
	{
		/* Taken down before it ran out, and no ledger row said so
		 * within the grace: cancelled is what is left. */
		evidence = "inferred";
		closed = MAX(c->vanished_at, c->last_at);
	}

	if (sold == c->quantity)
		outcome = VENTURE_LISTING_OUTCOME_SOLD;
	else if (sold > 0)
		outcome = VENTURE_LISTING_OUTCOME_PARTIAL;
	else if (c->any_cancel || (0 == g_strcmp0(evidence, "inferred")))
		outcome = VENTURE_LISTING_OUTCOME_CANCELLED;
	else
		outcome = VENTURE_LISTING_OUTCOME_EXPIRED;

	if ((VENTURE_SERIES_NONE == closed) || (closed < c->listed_at))
		closed = c->listed_at;

	if (!mirror_has_room(m))
	{
		m->over_cap++;
		return;
	}

	g_object_set(c->listing, "outcome", outcome, "quantity-sold", sold, NULL);
	mirror_set_unix(c->listing, "closed-at", closed);
	json_object_set_string_member(c->state, "outcome", mirror_outcome_nick(outcome));
	json_object_set_int_member(c->state, "sold", sold);
	json_object_set_int_member(c->state, "vanished_at", c->vanished_at);
	json_object_set_string_member(c->state, "evidence", evidence);
	json_object_set_object_member(c->state, "txns", json_object_ref(c->used));
	mirror_state_store(c->listing, c->state);

	if (!mirror_write(m, c->listing, &error))
	{
		g_autofree gchar *what = g_strdup_printf("listing #%" G_GINT64_FORMAT,
		                                         venture_entity_get_id(c->listing));

		mirror_failed(m, what, error);
		return;
	}

	switch (outcome)
	{
	case VENTURE_LISTING_OUTCOME_SOLD:
		m->counts.sold++;
		break;
	case VENTURE_LISTING_OUTCOME_PARTIAL:
		m->counts.partial++;
		break;
	case VENTURE_LISTING_OUTCOME_EXPIRED:
		m->counts.expired++;
		break;
	case VENTURE_LISTING_OUTCOME_CANCELLED:
	case VENTURE_LISTING_OUTCOME_OPEN:
	default:
		m->counts.cancelled++;
		break;
	}
}

/* A group left open this pass, and why: said once, in the notes. */
static void
mirror_unjudged(
	MirrorRun	*m,
	const gchar	*reason
){
	m->unjudged_groups++;

	if (NULL == m->unjudged_reason)
		m->unjudged_reason = g_strdup(reason);
}

/*
 * One account and instrument: the ledger rows since the earliest listing
 * taking part, less what closed listings already used; every listing's
 * own sold units first (mirror_reserve), then the rest handed out first
 * in, first out to the vanished ones -- sales first, then expiries and
 * cancellations -- and each vanished listing judged. A ledger read that
 * stopped at its bound, or failed, judges nothing: the rows it did not
 * return are the ones that would say what became of the newest listings.
 */
static void
mirror_judge_group(
	MirrorRun	 *m,
	GPtrArray	 *candidates,
	GPtrArray	 *settled
){
	g_autoptr(GHashTable) consumed = NULL;
	g_autoptr(GPtrArray) txns = NULL;
	g_autoptr(GError) read_error = NULL;
	g_autofree gint64 *left = NULL;
	VentureSeriesTxnFilter filter;
	MirrorCandidate *first;
	gint64 since;
	guint bound;
	guint pass;
	guint i;

	g_ptr_array_sort(candidates, mirror_compare_candidates);
	first = g_ptr_array_index(candidates, 0);
	since = first->listed_at;

	consumed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	for (i = 0; (NULL != settled) && (i < settled->len); i++)
		mirror_add_consumption(consumed, g_ptr_array_index(settled, i));

	venture_series_txn_filter_init(&filter);
	filter.account_key = mirror_state_string(first->state, "account");
	filter.instrument_key = mirror_state_string(first->state, "instrument");
	filter.since = since;
	bound = venture_marketdata_mirror_get_max_rows();
	filter.count = bound;
	filter.descending = FALSE;

	txns = venture_series_store_list_txns(m->store, &filter, &read_error);

	if (NULL == txns)
	{
		mirror_unjudged(m, read_error->message);
		return;
	}

	if (txns->len >= bound)
	{
		g_autofree gchar *reason = g_strdup_printf("one item's ledger since its oldest open "
		                                           "listing is more than %u rows", bound);

		mirror_unjudged(m, reason);
		return;
	}

	g_ptr_array_sort(txns, mirror_compare_txns);
	left = g_new0(gint64, MAX(txns->len, 1));

	for (i = 0; i < txns->len; i++)
	{
		const VentureSeriesTxnRow *txn = g_ptr_array_index(txns, i);
		gint64 *used;

		if ((VENTURE_SERIES_NONE == txn->quantity) || (txn->quantity <= 0))
			continue;

		used = g_hash_table_lookup(consumed, txn->key);
		left[i] = txn->quantity - ((NULL != used) ? *used : 0);
	}

	mirror_reserve(candidates, txns, left);

	for (pass = 0; pass < 2; pass++)
	{
		for (i = 0; i < txns->len; i++)
		{
			const VentureSeriesTxnRow *txn = g_ptr_array_index(txns, i);
			gboolean sale = (0 == g_strcmp0(txn->kind, "sale"));
			gboolean ended = (0 == g_strcmp0(txn->kind, "expired")) ||
			                 (0 == g_strcmp0(txn->kind, "cancelled"));

			if ((0 == pass) ? !sale : !ended)
				continue;

			if (left[i] > 0)
				left[i] = mirror_assign(candidates, txn, left[i], sale);
		}
	}

	for (i = 0; (i < candidates->len) && !m->broken; i++)
	{
		MirrorCandidate *c = g_ptr_array_index(candidates, i);

		if (!c->present)
			mirror_judge(m, c);
	}
}

/*
 * The source's closed mirrored listings closed since @since, deleted ones
 * included, grouped by account and instrument (their states, which say
 * what each used).
 */
static GHashTable *
mirror_settled(
	MirrorRun	 *m,
	gint64		  since,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) from = NULL;
	g_autofree gchar *cutoff = NULL;
	GHashTable *groups;
	guint i;

	from = g_date_time_new_from_unix_utc(since);
	cutoff = venture_time_to_string(from);

	/* A listing a person deleted after the mirror closed it still used
	 * its rows: leaving it out would hand its sale to the next listing. */
	query = venture_query_new(VENTURE_TYPE_LISTING);
	venture_query_set_organization(query, m->organization_id);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, VENTURE_MARKETDATA_MIRROR_MAX_LISTINGS);

	if (!venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ, m->source_id,
	                                  error) ||
	    !venture_query_add_filter_int(query, "outcome", VENTURE_FILTER_OP_NE,
	                                  VENTURE_LISTING_OUTCOME_OPEN, error) ||
	    !venture_query_add_filter_string(query, "closed-at", VENTURE_FILTER_OP_GTE, cutoff, error))
		return NULL;

	rows = venture_database_find(m->database, query, error);

	if (NULL == rows)
		return NULL;

	if (rows->len >= VENTURE_MARKETDATA_MIRROR_MAX_LISTINGS)
		mirror_note(m, "mirror: more than %d recently closed mirrored listings; ledger rows "
		            "they used may be matched again", VENTURE_MARKETDATA_MIRROR_MAX_LISTINGS);

	groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                               (GDestroyNotify)g_ptr_array_unref);

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *listing = g_ptr_array_index(rows, i);
		JsonObject *state;
		g_autofree gchar *group = NULL;
		GPtrArray *members;

		if (!mirror_owns(m, listing))
			continue;

		state = mirror_state_read(listing);

		if (NULL == state)
			continue;

		group = mirror_group_key(mirror_state_string(state, "account"),
		                         mirror_state_string(state, "instrument"));
		members = g_hash_table_lookup(groups, group);

		if (NULL == members)
		{
			members = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
			g_hash_table_insert(groups, g_steal_pointer(&group), members);
		}

		g_ptr_array_add(members, state);
	}

	return groups;
}

/*
 * Step 3: the open mirrored listings whose positions are gone. Only an
 * account a complete positions snapshot restated can have lost one; a
 * listing of any other is waiting for a snapshot that says so.
 */
static gboolean
mirror_vanished(
	MirrorRun	 *m,
	GHashTable	 *open,
	GHashTable	 *seen,
	GError		**error
){
	g_autoptr(GHashTable) groups = NULL;
	g_autoptr(GHashTable) settled = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gint64 since;

	groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                               (GDestroyNotify)g_ptr_array_unref);
	since = G_MAXINT64;

	g_hash_table_iter_init(&iter, open);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		VentureEntity *listing = value;
		const VentureSeriesAccountRow *account;
		g_autoptr(JsonObject) state = NULL;
		g_autofree gchar *group = NULL;
		MirrorCandidate *candidate;
		GPtrArray *members;
		gint64 vanished_at;

		if (g_hash_table_contains(seen, key))
			continue;

		state = mirror_state_read(listing);

		if ((NULL == state) || !mirror_outcome_unedited(listing, state))
		{
			m->left_alone++;
			continue;
		}

		account = g_hash_table_lookup(m->accounts, mirror_state_string(state, "account"));

		if ((NULL == account) || (VENTURE_SERIES_NONE == account->positions_at))
		{
			m->no_snapshot++;
			continue;
		}

		/* When it was first found gone, kept: a later snapshot moves the
		 * account's time on, and the grace would never run out. */
		vanished_at = mirror_state_int(state, "vanished_at");

		if (VENTURE_SERIES_NONE == vanished_at)
			vanished_at = account->positions_at;

		candidate = g_new0(MirrorCandidate, 1);
		candidate->listing = g_object_ref(listing);
		candidate->state = json_object_ref(state);
		candidate->venue = mirror_state_string(state, "venue");
		candidate->listed_at = mirror_unix(listing, "listed-at");
		candidate->expires_at = mirror_unix(listing, "expires-at");
		candidate->quantity = mirror_int(listing, "quantity");
		candidate->sold_floor = mirror_int(listing, "quantity-sold");
		candidate->vanished_at = vanished_at;
		candidate->last_at = VENTURE_SERIES_NONE;
		candidate->used = json_object_new();

		if (VENTURE_SERIES_NONE == candidate->listed_at)
			candidate->listed_at = vanished_at;

		since = MIN(since, candidate->listed_at);
		group = mirror_group_key(mirror_state_string(state, "account"),
		                         mirror_state_string(state, "instrument"));
		members = g_hash_table_lookup(groups, group);

		if (NULL == members)
		{
			members = g_ptr_array_new_with_free_func(mirror_candidate_free);
			g_hash_table_insert(groups, g_steal_pointer(&group), members);
		}

		g_ptr_array_add(members, candidate);
	}

	if (0 == g_hash_table_size(groups))
		return TRUE;

	/* The listings of those groups still on the market that lost units
	 * while listed: their sales are theirs, and they take part so that
	 * no vanished listing takes them (mirror_reserve). */
	g_hash_table_iter_init(&iter, open);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		VentureEntity *listing = value;
		g_autoptr(JsonObject) state = NULL;
		g_autofree gchar *group = NULL;
		MirrorCandidate *candidate;
		GPtrArray *members;
		gint64 listed_at;

		if (!g_hash_table_contains(seen, key) || (mirror_int(listing, "quantity-sold") <= 0))
			continue;

		listed_at = mirror_unix(listing, "listed-at");
		state = mirror_state_read(listing);

		if ((NULL == state) || (VENTURE_SERIES_NONE == listed_at))
			continue;

		group = mirror_group_key(mirror_state_string(state, "account"),
		                         mirror_state_string(state, "instrument"));
		members = g_hash_table_lookup(groups, group);

		if (NULL == members)
			continue;

		candidate = g_new0(MirrorCandidate, 1);
		candidate->listing = g_object_ref(listing);
		candidate->state = json_object_ref(state);
		candidate->present = TRUE;
		candidate->venue = mirror_state_string(state, "venue");
		candidate->listed_at = listed_at;
		candidate->expires_at = mirror_unix(listing, "expires-at");
		candidate->quantity = mirror_int(listing, "quantity");
		candidate->sold_floor = mirror_int(listing, "quantity-sold");
		candidate->vanished_at = VENTURE_SERIES_NONE;
		candidate->last_at = VENTURE_SERIES_NONE;
		candidate->used = json_object_new();
		since = MIN(since, listed_at);
		g_ptr_array_add(members, candidate);
	}

	settled = mirror_settled(m, since, error);

	if (NULL == settled)
		return FALSE;

	g_hash_table_iter_init(&iter, groups);

	while (g_hash_table_iter_next(&iter, &key, &value) && !m->broken)
		mirror_judge_group(m, value, g_hash_table_lookup(settled, key));

	return TRUE;
}

/* --- The report ---------------------------------------------------------------------- */

static void
mirror_notes_finish(MirrorRun *m)
{
	if (m->over_cap > 0)
		mirror_note(m, "mirror: %" G_GINT64_FORMAT " changes left for the next run: a pass writes "
		            "at most %u records (setting mirror_max_writes)", m->over_cap,
		            m->settings.max_writes);

	if (g_hash_table_size(m->unpriced) > 0)
	{
		g_autoptr(GString) examples = g_string_new(NULL);
		g_autoptr(GList) keys = g_hash_table_get_keys(m->unpriced);
		GList *link;
		guint shown;

		keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
		shown = 0;

		for (link = keys; (NULL != link) && (shown < MIRROR_NOTE_KEYS); link = link->next, shown++)
			g_string_append_printf(examples, "%s%s", (shown > 0) ? ", " : "", (const gchar *)link->data);

		mirror_note(m, "mirror: %" G_GINT64_FORMAT " positions not mirrored: their items have no "
		            "product (%u items, e.g. %s); link the instrument to a product, or set "
		            "create_products and products_venture_id",
		            m->not_mirrored - m->deleted_instruments - m->deleted_products,
		            g_hash_table_size(m->unpriced),
		            examples->str);
	}

	if (m->deleted_instruments > 0)
		mirror_note(m, "mirror: %" G_GINT64_FORMAT " positions not mirrored: their instrument "
		            "records are deleted", m->deleted_instruments);

	if (g_hash_table_size(m->unsellable) > 0)
	{
		g_autoptr(GString) examples = g_string_new(NULL);
		g_autoptr(GList) keys = g_hash_table_get_keys(m->unsellable);
		GList *link;
		guint shown;

		keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
		shown = 0;

		for (link = keys; (NULL != link) && (shown < MIRROR_NOTE_KEYS); link = link->next, shown++)
			g_string_append_printf(examples, "%s%s", (shown > 0) ? ", " : "", (const gchar *)link->data);

		mirror_note(m, "mirror: %" G_GINT64_FORMAT " positions not mirrored: their items' products "
		            "are deleted (%u items, e.g. %s); restore the product, or link the instrument "
		            "to another", m->deleted_products, g_hash_table_size(m->unsellable),
		            examples->str);
	}

	if (m->positions_truncated)
		mirror_note(m, "mirror: the store holds at least %" G_GINT64_FORMAT " positions, as many as "
		            "a pass reads; listings whose positions were not read are not judged gone",
		            m->n_positions);

	if (m->unjudged_groups > 0)
		mirror_note(m, "mirror: %" G_GINT64_FORMAT " items' gone listings were not judged: %s",
		            m->unjudged_groups, m->unjudged_reason);

	if (m->no_snapshot > 0)
		mirror_note(m, "mirror: %" G_GINT64_FORMAT " mirrored listings are gone from the store "
		            "but their accounts have no complete positions snapshot; they stay open",
		            m->no_snapshot);
}

static JsonNode *
mirror_report(MirrorRun *m)
{
	g_autoptr(JsonBuilder) builder = NULL;
	guint i;

	builder = json_builder_new();
	json_builder_begin_object(builder);

#define MIRROR_MEMBER(name, value) \
	G_STMT_START { \
		json_builder_set_member_name(builder, (name)); \
		json_builder_add_int_value(builder, (value)); \
	} G_STMT_END

	MIRROR_MEMBER("data_source_id", m->source_id);
	MIRROR_MEMBER("accounts", m->n_accounts);
	MIRROR_MEMBER("positions", m->n_positions);
	MIRROR_MEMBER("promoted", m->counts.promoted);
	MIRROR_MEMBER("reparented", m->counts.reparented);
	MIRROR_MEMBER("locations_kept", m->locations_kept);
	MIRROR_MEMBER("venues_linked", m->counts.venues_linked);
	MIRROR_MEMBER("products_created", m->counts.products_created);
	MIRROR_MEMBER("created", m->counts.created);
	MIRROR_MEMBER("updated", m->counts.updated);
	MIRROR_MEMBER("reopened", m->counts.reopened);
	json_builder_set_member_name(builder, "closed");
	json_builder_begin_object(builder);
	MIRROR_MEMBER("sold", m->counts.sold);
	MIRROR_MEMBER("partial", m->counts.partial);
	MIRROR_MEMBER("expired", m->counts.expired);
	MIRROR_MEMBER("cancelled", m->counts.cancelled);
	json_builder_end_object(builder);
	MIRROR_MEMBER("waiting", m->waiting);
	MIRROR_MEMBER("left_alone", m->left_alone);
	MIRROR_MEMBER("not_mirrored", m->not_mirrored);
	MIRROR_MEMBER("failed", m->failed);
	MIRROR_MEMBER("over_cap", m->over_cap);
	MIRROR_MEMBER("writes", (gint64)m->counts.writes);

#undef MIRROR_MEMBER

	json_builder_set_member_name(builder, "notes");
	json_builder_begin_array(builder);

	for (i = 0; i < m->notes->len; i++)
		json_builder_add_string_value(builder, g_ptr_array_index(m->notes, i));

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* The source and its settings, or an error. */
static gboolean
mirror_open(
	MirrorRun	 *m,
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  data_source_id,
	gboolean	  batching,
	GError		**error
){
	g_autoptr(VentureEntity) source = NULL;

	source = venture_marketdata_source_in(venture_context_get_database(context), organization_id,
	                                      data_source_id, error);

	if (NULL == source)
		return FALSE;

	mirror_run_init(m, context, source, batching);

	return mirror_settings_read(source, &m->settings, error);
}

/*
 * TRUE when @read_error is the store refusing a read of everything past
 * its bound ("narrow the read"), noted on the run: the pass then stops
 * with nothing judged gone, because what the read would have returned is
 * exactly what tells a gone listing from an unread one. Any other error
 * is the caller's to propagate.
 */
static gboolean
mirror_read_refused(
	MirrorRun	*m,
	const GError	*read_error,
	const gchar	*what
){
	if (!g_error_matches(read_error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT))
		return FALSE;

	mirror_note(m, "mirror: the store would not read every %s (%s); no listing was written or "
	            "judged gone this pass", what, read_error->message);
	return TRUE;
}

/* The whole pass after the source is open: steps 1 to 3. */
static gboolean
mirror_pass(
	MirrorRun	 *m,
	GError		**error
){
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GPtrArray) positions = NULL;
	g_autoptr(GHashTable) open = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autoptr(GError) store_error = NULL;
	VentureSeriesPositionFilter position_filter;

	m->store = venture_marketdata_reader(m->context, m->source_id, &store_error);

	if (NULL == m->store)
	{
		if (g_error_matches(store_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
		{
			mirror_note(m, "mirror: the source has stored nothing yet");
			return TRUE;
		}

		g_propagate_error(error, g_steal_pointer(&store_error));
		return FALSE;
	}

	accounts = venture_series_store_list_accounts(m->store, NULL, NULL, NULL, m->now, &store_error);

	/*
	 * A store with more accounts than one read answers refuses the read
	 * rather than cutting it (INVALID_ARGUMENT). That is the size of the
	 * store, not a fault in the run: the pass writes nothing, judges
	 * nothing and says so, and the listings stay as they are. Failing it
	 * would only lose the note, and every later pass would fail alike.
	 */
	if ((NULL == accounts) && mirror_read_refused(m, store_error, "account"))
		return TRUE;

	if (NULL == accounts)
	{
		g_propagate_error(error, g_steal_pointer(&store_error));
		return FALSE;
	}

	m->n_accounts = accounts->len;

	/* A store with no accounts is a market data source: nothing here. */
	if (0 == accounts->len)
		return TRUE;

	if (!mirror_load_location_refs(m, error))
		return FALSE;

	mirror_accounts(m, accounts);

	/* A read that stops at its bound has left positions out, and a
	 * listing whose position is merely unread is not gone: it is mirrored
	 * as far as the read goes, and nothing is judged gone this pass. */
	venture_series_position_filter_init(&position_filter);
	position_filter.count = venture_marketdata_mirror_get_max_rows();
	positions = venture_series_store_list_positions(m->store, &position_filter, &store_error);

	/* A refusal past the store's bound (the mirror's bound is clamped to
	 * it, so only a bound lowered between the two reads gets here): no
	 * position was read, so none is mirrored and none is judged gone. */
	if ((NULL == positions) && mirror_read_refused(m, store_error, "position"))
		return TRUE;

	if (NULL == positions)
	{
		g_propagate_error(error, g_steal_pointer(&store_error));
		return FALSE;
	}

	m->n_positions = positions->len;
	m->positions_truncated = positions->len >= position_filter.count;
	open = mirror_open_listings(m, error);

	if (NULL == open)
		return FALSE;

	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	if (!mirror_present(m, positions, open, seen, error))
		return FALSE;

	if (!m->broken && !m->positions_truncated && !mirror_vanished(m, open, seen, error))
		return FALSE;

	/* Its rows are borrowed from @accounts, freed on return. */
	g_hash_table_remove_all(m->accounts);

	return TRUE;
}

#endif /* VENTURE_HAVE_SQLITE */

gboolean
venture_marketdata_mirror_positions(
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  data_source_id,
	JsonNode	**out_report,
	GError		**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	if (NULL != out_report)
		*out_report = NULL;

	if (venture_automation_is_dispatching(venture_context_get_automation(context)) ||
	    venture_database_has_transaction(venture_context_get_database(context)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "Positions are not mirrored inside an automation handler or a "
		                    "transaction: its listings would be written where nobody hears them, "
		                    "or rolled back with somebody else's work");
		return FALSE;
	}

	if (!mirror_can_run(context, error))
		return FALSE;

#ifdef VENTURE_HAVE_SQLITE
	{
		g_autoptr(VentureAccessScope) internal = NULL;
		MirrorRun m;
		gboolean ok;

		internal = venture_access_policy_enter(
			venture_database_get_access_policy(venture_context_get_database(context)), NULL);

		memset(&m, 0, sizeof(m));

		if (!mirror_open(&m, context, organization_id, data_source_id, TRUE, error))
		{
			mirror_run_clear(&m);
			return FALSE;
		}

		if (!m.settings.mirror)
			mirror_note(&m, "mirror: switched off for this source (setting mirror_positions)");

		ok = !m.settings.mirror || mirror_pass(&m, error);
		mirror_batch_end(&m);

		if (ok)
		{
			mirror_notes_finish(&m);

			if (NULL != out_report)
				*out_report = mirror_report(&m);
		}

		mirror_run_clear(&m);

		return ok;
	}
#else
	(void)organization_id;
	(void)data_source_id;
	return FALSE;
#endif
}

gboolean
venture_marketdata_promote_account(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  data_source_id,
	const gchar		 *key,
	const VentureActor	 *actor,
	VentureEntity		**out_location,
	GError			**error
){
	return venture_marketdata_promote_account_full(context, organization_id, data_source_id, key,
	                                               TRUE, actor, out_location, error);
}

gboolean
venture_marketdata_promote_account_full(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  data_source_id,
	const gchar		 *key,
	gboolean		  restore,
	const VentureActor	 *actor,
	VentureEntity		**out_location,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	if (NULL != out_location)
		*out_location = NULL;

	if (venture_string_is_empty(key))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Promotion needs the key the store knows the account by");
		return FALSE;
	}

	if (!mirror_can_run(context, error))
		return FALSE;

#ifdef VENTURE_HAVE_SQLITE
	{
		g_autoptr(VentureSeriesAccountRow) row = NULL;
		MirrorRun m;
		gint64 id;
		gboolean ok;

		memset(&m, 0, sizeof(m));
		id = 0;

		if (!mirror_open(&m, context, organization_id, data_source_id, FALSE, error))
		{
			mirror_run_clear(&m);
			return FALSE;
		}

		/* A person asked: their name on what is written, and no bound. */
		if (NULL != actor)
			m.actor = *actor;

		m.settings.max_writes = G_MAXUINT;
		m.store = venture_marketdata_reader(context, data_source_id, error);
		ok = (NULL != m.store) &&
		     venture_series_store_get_account(m.store, key, m.now, &row, error);

		if (ok && (NULL == row))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Data source #%" G_GINT64_FORMAT " has not seen an account \"%s\"",
			            data_source_id, key);
			ok = FALSE;
		}

		m.location_refs_complete = FALSE;
		ok = ok && mirror_promote_account(&m, row, restore, &id, error);

		/* Without @restore a deleted location is no place: 0, and so
		 * no location handed back. */
		if (ok && (id > 0) && (NULL != out_location))
		{
			*out_location = venture_database_get(m.database, VENTURE_TYPE_LOCATION, id, error);
			ok = (NULL != *out_location);
		}

		mirror_run_clear(&m);

		return ok;
	}
#else
	(void)organization_id;
	(void)data_source_id;
	(void)restore;
	(void)actor;
	return FALSE;
#endif
}

/* ==========================================================================
 * The feeds hook
 * ========================================================================== */

#ifdef VENTURE_HAVE_SQLITE

static gboolean
mirror_must_wait(VentureContext *context)
{
	return venture_automation_is_dispatching(venture_context_get_automation(context)) ||
	       venture_database_has_transaction(venture_context_get_database(context));
}

/* A line for the run record: what the pass changed, when it changed any. */
static gchar *
mirror_summary(JsonObject *report)
{
	JsonObject *closed;
	gint64 created;
	gint64 updated;
	gint64 reopened;
	gint64 waiting;
	gint64 promoted;
	gint64 shut;

	closed = json_object_get_object_member(report, "closed");
	created = json_object_get_int_member(report, "created");
	updated = json_object_get_int_member(report, "updated");
	reopened = json_object_get_int_member(report, "reopened");
	waiting = json_object_get_int_member(report, "waiting");
	promoted = json_object_get_int_member(report, "promoted");
	shut = json_object_get_int_member(closed, "sold") + json_object_get_int_member(closed, "partial") +
	       json_object_get_int_member(closed, "expired") + json_object_get_int_member(closed, "cancelled");

	if (0 == (created + updated + reopened + shut + promoted + waiting))
		return NULL;

	return g_strdup_printf("mirror: listings %" G_GINT64_FORMAT " created, %" G_GINT64_FORMAT
	                       " updated, %" G_GINT64_FORMAT " reopened, %" G_GINT64_FORMAT " closed (%"
	                       G_GINT64_FORMAT " sold, %" G_GINT64_FORMAT " partial, %" G_GINT64_FORMAT
	                       " expired, %" G_GINT64_FORMAT " cancelled), %" G_GINT64_FORMAT
	                       " waiting for the ledger; %" G_GINT64_FORMAT " accounts promoted",
	                       created, updated, reopened, shut,
	                       json_object_get_int_member(closed, "sold"),
	                       json_object_get_int_member(closed, "partial"),
	                       json_object_get_int_member(closed, "expired"),
	                       json_object_get_int_member(closed, "cancelled"),
	                       waiting, promoted);
}

/* Appends the pass's lines to the run record's notes, as the system. */
static void
mirror_note_run(
	VentureContext	*context,
	gint64		 run_record_id,
	JsonObject	*report
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GString) notes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *existing = NULL;
	g_autofree gchar *summary = NULL;
	JsonArray *lines;
	VentureDatabase *database;
	VentureActor actor;
	guint before;
	guint i;

	database = venture_context_get_database(context);
	record = (run_record_id > 0)
		? venture_database_get(database, VENTURE_TYPE_DATA_SOURCE_RUN, run_record_id, NULL) : NULL;

	if (NULL == record)
		return;

	g_object_get(record, "notes", &existing, NULL);
	notes = g_string_new(existing);
	before = (guint)notes->len;
	summary = mirror_summary(report);

	if (NULL != summary)
		g_string_append_printf(notes, "%s%s", (notes->len > 0) ? "\n" : "", summary);

	lines = json_object_get_array_member(report, "notes");

	for (i = 0; i < json_array_get_length(lines); i++)
		g_string_append_printf(notes, "%s%s", (notes->len > 0) ? "\n" : "",
		                       json_array_get_string_element(lines, i));

	if (notes->len == before)
		return;

	g_object_set(record, "notes", notes->str, NULL);
	mirror_actor(&actor);

	if (!venture_database_save(database, record, &actor, &error))
		g_message("mirror: the run's note was not written: %s", error->message);
}

static void
mirror_deliver(
	VentureContext	*context,
	gint64		 organization_id,
	gint64		 source_id,
	gint64		 run_record_id
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(GError) error = NULL;

	/* Switched off since: nothing to say, and nothing failed. */
	if (!mirror_can_run(context, NULL))
		return;

	if (!venture_marketdata_mirror_positions(context, organization_id, source_id, &report, &error))
	{
		/* Its source deleted since, most likely; the store keeps the
		 * positions and the next run tries again. */
		g_message("mirror: source %" G_GINT64_FORMAT " was not mirrored: %s", source_id,
		          error->message);
		return;
	}

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(venture_context_get_database(context)), NULL);
	mirror_note_run(context, run_record_id, json_node_get_object(report));
}

typedef struct
{
	GWeakRef	 context;
	gint64		 organization_id;
	gint64		 source_id;
	gint64		 run_record_id;
} MirrorDeferred;

static void
mirror_deferred_free(gpointer data)
{
	MirrorDeferred *deferred = data;

	g_weak_ref_clear(&deferred->context);
	g_free(deferred);
}

static gboolean
mirror_deferred_fire(gpointer data)
{
	MirrorDeferred *deferred = data;
	g_autoptr(VentureContext) context = NULL;

	context = g_weak_ref_get(&deferred->context);

	if (NULL == context)
		return G_SOURCE_REMOVE;

	if (mirror_must_wait(context))
		return G_SOURCE_CONTINUE;

	mirror_deliver(context, deferred->organization_id, deferred->source_id,
	               deferred->run_record_id);

	return G_SOURCE_REMOVE;
}

/*
 * After every run, on the main thread. A failed run stored nothing, so
 * there is nothing new to mirror. The feeds service has waited out
 * transactions already; an automation handler's nested loop is the case
 * left, and a listing written inside one would raise no on_created.
 */
static void
mirror_hook_run(
	VentureContext	*context,
	VentureFeedRun	*run,
	VentureEntity	*run_record,
	gpointer	 user_data
){
	gint64 run_record_id;

	(void)user_data;

	if ((VENTURE_DATA_SOURCE_RUN_STATUS_FAILED == venture_feed_run_get_status(run)) ||
	    !mirror_can_run(context, NULL))
		return;

	run_record_id = (NULL != run_record) ? venture_entity_get_id(run_record) : 0;

	if (mirror_must_wait(context))
	{
		MirrorDeferred *deferred = g_new0(MirrorDeferred, 1);

		g_weak_ref_init(&deferred->context, context);
		deferred->organization_id = venture_feed_run_get_organization_id(run);
		deferred->source_id = venture_feed_run_get_source_id(run);
		deferred->run_record_id = run_record_id;
		g_timeout_add_full(G_PRIORITY_DEFAULT, MIRROR_RETRY_MS, mirror_deferred_fire, deferred,
		                   mirror_deferred_free);
		return;
	}

	mirror_deliver(context, venture_feed_run_get_organization_id(run),
	               venture_feed_run_get_source_id(run), run_record_id);
}

#endif /* VENTURE_HAVE_SQLITE */

void
venture_marketdata_mirror_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* Validators are per database; the tests build several contexts
	 * over one. */
	if (NULL == g_object_get_data(G_OBJECT(database), MIRROR_INSTALLED_KEY))
	{
		g_object_set_data(G_OBJECT(database), MIRROR_INSTALLED_KEY, GINT_TO_POINTER(1));
		venture_database_add_save_validator(database, VENTURE_TYPE_LOCATION,
		                                    mirror_validate_location, NULL, NULL);
		venture_database_add_save_validator(database, VENTURE_TYPE_DATA_SOURCE,
		                                    mirror_validate_source, NULL, NULL);
	}

#ifdef VENTURE_HAVE_SQLITE
	venture_feeds_add_hook(context, VENTURE_MARKETDATA_MIRROR_HOOK, NULL, NULL, mirror_hook_run,
	                       NULL, NULL);
#endif
}
