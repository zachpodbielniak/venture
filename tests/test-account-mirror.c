/*
 * test-account-mirror.c - Accounts as locations, positions as listings
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A push source's store holds the operator's accounts and open positions;
 * the marketdata mirror turns them into location and listing records on
 * the main thread after every run. These tests push real JSON lines
 * through the feeds worker, so the store, the hook and the records are
 * the production path end to end, and then read the records back. Times
 * are relative to the real clock because the mirror judges a vanished
 * listing's grace against it.
 */

#include <venture.h>
#include <string.h>

#include "venture-test-util.h"

#ifdef VENTURE_HAVE_SQLITE

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))
#define HOUR (G_GINT64_CONSTANT(3600))

typedef struct
{
	gchar			*state_dir;
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	gint64			 org;
	gint64			 venture_id;
	gint64			 now;
} Fixture;

/* --- Helpers ------------------------------------------------------------------- */

static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

static void
save_refused(
	Fixture		*fixture,
	gpointer	 record,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;
	gboolean saved;

	saved = venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error);
	g_assert_nonnull(error);
	g_assert_false(saved);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

static VentureEntity *
reread(
	Fixture	*fixture,
	GType	 type,
	gint64	 id
){
	g_autoptr(GError) error = NULL;
	VentureEntity *record;

	record = venture_database_get(fixture->database, type, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(record);

	return record;
}

static VentureMoney *
money_of(const gchar *text)
{
	VentureMoney *money;

	money = venture_money_from_string(text, NULL, NULL);
	g_assert_nonnull(money);

	return money;
}

static gchar *
iso(gint64 unix_time)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(unix_time);

	return g_date_time_format(when, "%Y-%m-%dT%H:%M:%SZ");
}

static VentureFeedsService *
service_of(Fixture *fixture)
{
	VentureFeedsService *service;

	service = venture_context_get_feeds_service(fixture->context);
	g_assert_nonnull(service);

	return service;
}

/* Lets runs come back until nothing is pending, bounded. */
static void
settle(Fixture *fixture)
{
	VentureFeedsService *service;
	gint64 deadline;

	service = venture_context_get_feeds_service(fixture->context);

	if (NULL == service)
		return;

	deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

	while (venture_feeds_service_count_pending(service) > 0)
	{
		if (g_get_monotonic_time() > deadline)
			g_error("the feeds worker did not settle within 30 seconds");

		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(2000);
	}

	while (g_main_context_iteration(NULL, FALSE))
		;
}

/* A push source in @org with @settings (YAML), switched on. */
static VentureEntity *
push_source(
	Fixture		*fixture,
	gint64		 org,
	const gchar	*name,
	const gchar	*settings
){
	VentureEntity *source;

	source = VENTURE_ENTITY(venture_data_source_new());
	venture_entity_set_organization_id(source, org);
	g_object_set(source, "name", name, "provider", "push", "settings", settings,
	             "schedule", "manual", "currency", "USD", "instrument-namespace", "wow-item",
	             "venue-namespace", "wow-realm", NULL);
	save(fixture, source);

	return source;
}

/* Pushes @text to @source and waits, bounded, for its run; the mirror runs
 * inside that wait. Returns the run record. */
static VentureEntity *
push(
	Fixture		*fixture,
	VentureEntity	*source,
	const gchar	*text
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *push_id = NULL;
	VentureEntity *run;
	gint64 run_id = 0;
	VentureDataSourceRunStatus status;

	body = g_bytes_new(text, strlen(text));
	g_assert_true(venture_feeds_service_push(service_of(fixture), ID(source), body, &push_id,
	                                         &error));
	g_assert_no_error(error);
	g_assert_true(venture_feeds_service_wait_push(service_of(fixture), push_id, 60, &run_id));
	g_assert_cmpint(run_id, >, 0);
	settle(fixture);

	run = reread(fixture, VENTURE_TYPE_DATA_SOURCE_RUN, run_id);
	g_object_get(run, "status", &status, NULL);

	if (VENTURE_DATA_SOURCE_RUN_STATUS_FAILED == status)
	{
		g_autofree gchar *message = NULL;

		g_object_get(run, "error", &message, NULL);
		g_error("push failed: %s", message);
	}

	return run;
}

/* Whether the run record's notes hold @fragment. */
static gboolean
run_says(
	VentureEntity	*run,
	const gchar	*fragment
){
	g_autofree gchar *notes = NULL;

	g_object_get(run, "notes", &notes, NULL);

	return (NULL != notes) && (NULL != strstr(notes, fragment));
}

/* One pass by hand; its report. */
static JsonObject *
mirror(
	Fixture		*fixture,
	VentureEntity	*source
){
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_marketdata_mirror_positions(fixture->context,
	                                                  venture_entity_get_organization_id(source),
	                                                  ID(source), &report, &error));
	g_assert_no_error(error);

	return json_object_ref(json_node_get_object(report));
}

/* The listing mirroring @position of @source, or NULL. */
static VentureEntity *
listing_of(
	Fixture		*fixture,
	VentureEntity	*source,
	const gchar	*position
){
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *external_id = NULL;

	external_id = g_strdup_printf("%s:%s", venture_entity_get_uuid(source), position);
	query = venture_query_new(VENTURE_TYPE_LISTING);
	venture_query_set_include_deleted(query, TRUE);
	g_assert_true(venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ,
	                                              external_id, NULL));

	return venture_database_find_one(fixture->database, query, NULL);
}

static VentureListingOutcome
outcome_of(VentureEntity *listing)
{
	VentureListingOutcome outcome;

	g_object_get(listing, "outcome", &outcome, NULL);

	return outcome;
}

static gint64
int_of(
	VentureEntity	*record,
	const gchar	*property
){
	gint64 value = 0;

	g_object_get(record, property, &value, NULL);

	return value;
}

static gint64
time_of(
	VentureEntity	*record,
	const gchar	*property
){
	g_autoptr(GDateTime) when = NULL;

	g_object_get(record, property, &when, NULL);

	return (NULL != when) ? g_date_time_to_unix(when) : 0;
}

static gchar *
money_text(
	VentureEntity	*record,
	const gchar	*property
){
	g_autoptr(VentureMoney) money = NULL;

	g_object_get(record, property, &money, NULL);

	return (NULL != money) ? venture_money_to_string(money) : NULL;
}

static gint64
count_of(
	Fixture		*fixture,
	GType		 type
){
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;

	venture_query_set_limit(query, 0);
	rows = venture_database_find(fixture->database, query, NULL);
	g_assert_nonnull(rows);

	return rows->len;
}

/* The location whose reference is @ref, or NULL. */
static VentureEntity *
location_by_ref(
	Fixture		*fixture,
	gint64		 org,
	const gchar	*ref
){
	g_autoptr(GError) error = NULL;
	VentureEntity *found;

	found = venture_marketdata_find_by_ref(fixture->database, VENTURE_TYPE_LOCATION, org, ref, &error);
	g_assert_no_error(error);

	return found;
}

/* --- JSON lines ------------------------------------------------------------------ */

static void
line_account(
	GString		*body,
	const gchar	*key,
	const gchar	*kind,
	const gchar	*group,
	const gchar	*venue
){
	g_string_append_printf(body, "{\"type\":\"account\",\"key\":\"%s\",\"name\":\"%.*s\","
	                       "\"kind\":\"%s\"", key, (gint)strcspn(key, "-"), key, kind);

	if (NULL != group)
		g_string_append_printf(body, ",\"group\":\"%s\"", group);

	if (NULL != venue)
		g_string_append_printf(body, ",\"venue\":\"%s\"", venue);

	g_string_append(body, "}\n");
}

static void
line_snapshot(
	GString		*body,
	const gchar	*account,
	gint64		 at
){
	g_autofree gchar *when = iso(at);

	g_string_append_printf(body, "{\"type\":\"account_snapshot\",\"account\":\"%s\",\"at\":\"%s\","
	                       "\"covers\":[\"positions\"]}\n", account, when);
}

static void
line_position(
	GString		*body,
	const gchar	*account,
	const gchar	*id,
	const gchar	*instrument,
	gint64		 quantity,
	const gchar	*price,
	const gchar	*bid,
	gint64		 posted_at,
	gint64		 expires_at
){
	g_autofree gchar *posted = iso(posted_at);
	g_autofree gchar *expires = iso(expires_at);

	g_string_append_printf(body, "{\"type\":\"position\",\"account\":\"%s\",\"venue\":\"thorium\","
	                       "\"id\":\"%s\",\"instrument\":\"%s\",\"quantity\":%" G_GINT64_FORMAT
	                       ",\"price\":\"%s\",", account, id, instrument, quantity, price);

	if (NULL != bid)
		g_string_append_printf(body, "\"bid\":\"%s\",", bid);

	g_string_append_printf(body, "\"posted_at\":\"%s\",\"expires_at\":\"%s\"}\n", posted, expires);
}

static void
line_txn(
	GString		*body,
	const gchar	*id,
	const gchar	*account,
	const gchar	*kind,
	const gchar	*instrument,
	gint64		 quantity,
	const gchar	*amount,
	gint64		 at
){
	g_autofree gchar *when = iso(at);

	g_string_append_printf(body, "{\"type\":\"txn\",\"id\":\"%s\",\"account\":\"%s\","
	                       "\"venue\":\"thorium\",\"kind\":\"%s\",\"instrument\":\"%s\","
	                       "\"quantity\":%" G_GINT64_FORMAT, id, account, kind, instrument, quantity);

	if (NULL != amount)
		g_string_append_printf(body, ",\"amount\":\"%s\"", amount);

	g_string_append_printf(body, ",\"at\":\"%s\"}\n", when);
}

/* The instruments and the realm every test trades on. */
static void
line_market(GString *body)
{
	g_string_append(body,
		"{\"type\":\"instrument\",\"key\":\"2770\",\"name\":\"Copper Ore\",\"kind\":\"item\"}\n"
		"{\"type\":\"instrument\",\"key\":\"2447\",\"name\":\"Peacebloom\",\"kind\":\"item\"}\n"
		"{\"type\":\"venue\",\"key\":\"thorium\",\"name\":\"Thorium Brotherhood\","
		"\"kind\":\"auction_house\",\"group\":\"us\"}\n");
	line_account(body, "Drgold-Thorium", "character", "Thorium Brotherhood", "thorium");
}

/* Settings that make products, so a position becomes a listing at once. */
static gchar *
products_settings(
	Fixture		*fixture,
	const gchar	*extra
){
	return g_strdup_printf("create_products: true\nproducts_venture_id: %" G_GINT64_FORMAT "\n%s",
	                       fixture->venture_id, (NULL != extra) ? extra : "");
}

/* --- Fixture ---------------------------------------------------------------------- */

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) venture = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-mirror-XXXXXX", &error);
	g_assert_no_error(error);

	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir, "feeds-enabled", TRUE,
	             "feeds-run-window-minutes", (gint64)0, NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
	                                       venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->org = venture_context_get_default_organization_id(fixture->context);

	/* The registry is process-wide; with this context's switches the
	 * feeds and marketdata tables exist now. */
	g_assert_true(venture_database_migrate(fixture->database,
	                                       venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	venture = VENTURE_ENTITY(venture_venture_new());
	venture_entity_set_organization_id(venture, fixture->org);
	g_object_set(venture, "name", "Gold", NULL);
	save(fixture, venture);
	fixture->venture_id = ID(venture);
	fixture->now = g_get_real_time() / G_USEC_PER_SEC;
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	settle(fixture);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	/* A test that switched a module off must not leave it off. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

/* --- Accounts ---------------------------------------------------------------------- */

/*
 * An account becomes a location of its kind, inside one for its group,
 * linked to its venue -- once, however often it is promoted; a deleted
 * one is restored by hand but never by the mirror; a venue already linked
 * keeps its link; a shared namespace makes two sources one place.
 *
 * What breaks if this regresses: every push makes another "Drgold", the
 * mirror brings back a character a person removed every few minutes, or a
 * second character on a realm takes the realm's money holding from the
 * first.
 */
static void
test_mirror_promotion(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) drgold = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(VentureEntity) alt = NULL;
	g_autoptr(VentureEntity) bank = NULL;
	g_autoptr(VentureEntity) group = NULL;
	g_autoptr(VentureEntity) venue = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *kind = NULL;
	g_autofree gchar *name = NULL;
	gint64 drgold_id;

	(void)user_data;

	source = push_source(fixture, fixture->org, "Characters", "auto_promote_accounts: false\n");
	line_market(body);
	line_account(body, "Alt-Thorium", "character", "Thorium Brotherhood", "thorium");
	line_account(body, "warbank:ZAK", "shared", NULL, NULL);
	run = push(fixture, source, body->str);

	/* Switched off: the push made no place. */
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_LOCATION), ==, 0);

	g_assert_true(venture_marketdata_promote_account(fixture->context, fixture->org, ID(source),
	                                                 "Drgold-Thorium", NULL, &drgold, &error));
	g_assert_no_error(error);
	g_object_get(drgold, "external-ref", &ref, "kind", &kind, "name", &name, NULL);
	g_assert_cmpstr(kind, ==, "character");
	g_assert_cmpstr(name, ==, "Drgold");
	g_assert_cmpint(int_of(drgold, "data-source-id"), ==, ID(source));
	drgold_id = ID(drgold);

	{
		g_autofree gchar *expected = g_strdup_printf("%s:Drgold-Thorium",
		                                             venture_entity_get_uuid(source));
		g_autofree gchar *derived = venture_marketdata_account_ref(source, "Drgold-Thorium");

		g_assert_cmpstr(ref, ==, expected);
		g_assert_cmpstr(derived, ==, expected);
	}

	/* Inside its realm's place. */
	group = reread(fixture, VENTURE_TYPE_LOCATION, int_of(drgold, "parent-id"));
	{
		g_autofree gchar *group_kind = NULL;
		g_autofree gchar *group_name = NULL;

		g_object_get(group, "kind", &group_kind, "name", &group_name, NULL);
		g_assert_cmpstr(group_kind, ==, "group");
		g_assert_cmpstr(group_name, ==, "Thorium Brotherhood");
	}

	/* The venue was promoted and now moves money through Drgold. */
	{
		g_autoptr(GPtrArray) venues = NULL;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_VENUE);

		venues = venture_database_find(fixture->database, query, NULL);
		g_assert_cmpuint(venues->len, ==, 1);
		venue = g_object_ref(g_ptr_array_index(venues, 0));
		g_assert_cmpint(int_of(venue, "location-id"), ==, drgold_id);
	}

	/* Twice is the same place. */
	g_assert_true(venture_marketdata_promote_account(fixture->context, fixture->org, ID(source),
	                                                 "Drgold-Thorium", NULL, &again, &error));
	g_assert_cmpint(ID(again), ==, drgold_id);
	g_clear_object(&again);

	/* A second character on the realm does not take the realm's link. */
	g_assert_true(venture_marketdata_promote_account(fixture->context, fixture->org, ID(source),
	                                                 "Alt-Thorium", NULL, &alt, &error));
	g_assert_cmpint(int_of(alt, "parent-id"), ==, ID(group));
	g_clear_object(&venue);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_VENUE);

		venue = venture_database_find_one(fixture->database, query, NULL);
		g_assert_cmpint(int_of(venue, "location-id"), ==, drgold_id);
	}

	/* No group: no parent. */
	g_assert_true(venture_marketdata_promote_account(fixture->context, fixture->org, ID(source),
	                                                 "warbank:ZAK", NULL, &bank, &error));
	g_assert_cmpint(int_of(bank, "parent-id"), ==, 0);
	{
		g_autofree gchar *bank_kind = NULL;

		g_object_get(bank, "kind", &bank_kind, NULL);
		g_assert_cmpstr(bank_kind, ==, "shared");
	}

	/* Deleted, then promoted: the same record, restored. */
	g_assert_true(venture_database_delete(fixture->database, drgold, NULL, &error));
	g_assert_true(venture_marketdata_promote_account(fixture->context, fixture->org, ID(source),
	                                                 "Drgold-Thorium", NULL, &again, &error));
	g_assert_cmpint(ID(again), ==, drgold_id);
	g_assert_false(venture_entity_is_deleted(again));

	/* What the store never saw, and another organization's source. */
	g_assert_false(venture_marketdata_promote_account(fixture->context, fixture->org, ID(source),
	                                                  "Nobody", NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(venture_marketdata_promote_account(fixture->context, fixture->org + 1000,
	                                                  ID(source), "Drgold-Thorium", NULL, NULL,
	                                                  &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* A hand-made place may not take a promoted one's reference. */
	{
		g_autoptr(VentureEntity) copy = VENTURE_ENTITY(venture_location_new());

		venture_entity_set_organization_id(copy, fixture->org);
		g_object_set(copy, "name", "Copy", "external-ref", ref, NULL);
		save_refused(fixture, copy, "already stands for");
	}
}

/*
 * By default a push promotes every account it carries; a place a person
 * then deletes stays deleted through later pushes. Two sources sharing an
 * account_namespace share their places.
 */
static void
test_mirror_auto_promotion(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) drgold = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	gint64 places;

	(void)user_data;

	first = push_source(fixture, fixture->org, "Push", "account_namespace: wow-character\n");
	second = push_source(fixture, fixture->org, "Exec", "account_namespace: wow-character\n");
	line_market(body);
	run = push(fixture, first, body->str);
	g_assert_true(run_says(run, "1 accounts promoted"));

	drgold = location_by_ref(fixture, fixture->org, "wow-character:Drgold-Thorium");
	g_assert_nonnull(drgold);
	places = count_of(fixture, VENTURE_TYPE_LOCATION);
	g_assert_cmpint(places, ==, 2);

	/* The other source describes the same character: no second place. */
	g_clear_object(&run);
	run = push(fixture, second, body->str);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_LOCATION), ==, places);

	/* Deleted by a person: the next push leaves it deleted. */
	g_assert_true(venture_database_delete(fixture->database, drgold, NULL, &error));
	g_clear_object(&run);
	run = push(fixture, first, body->str);
	g_clear_object(&drgold);
	drgold = location_by_ref(fixture, fixture->org, "wow-character:Drgold-Thorium");
	g_assert_true(venture_entity_is_deleted(drgold));
}

/*
 * The mirror's settings are judged when written.
 *
 * What breaks if this regresses: a namespace with a colon makes two
 * accounts one place, or products are made in another organization's
 * venture.
 */
static void
test_mirror_settings(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureEntity) theirs = NULL;
	guint i;
	const struct
	{
		const gchar	*settings;
		const gchar	*fragment;
	} refused[] = {
		{ "mirror_positions: maybe\n", "mirror_positions" },
		{ "mirror_max_writes: 0\n", "mirror_max_writes" },
		{ "mirror_max_writes: 5001\n", "mirror_max_writes" },
		{ "mirror_grace_hours: 721\n", "mirror_grace_hours" },
		{ "account_namespace: \"wow:char\"\n", "account_namespace" },
		{ "account_namespace: \"a/b\"\n", "account_namespace" },
		{ "create_products: true\n", "products_venture_id" },
		{ "create_products: true\nproducts_venture_id: 999999\n", "no venture" },
	};

	(void)user_data;

	for (i = 0; i < G_N_ELEMENTS(refused); i++)
	{
		g_autoptr(VentureEntity) source = VENTURE_ENTITY(venture_data_source_new());

		venture_entity_set_organization_id(source, fixture->org);
		g_object_set(source, "name", "Bad", "provider", "push", "settings",
		             refused[i].settings, "currency", "USD", NULL);
		save_refused(fixture, source, refused[i].fragment);
	}

	/* A venture of another organization is no venture here. */
	organization = VENTURE_ENTITY(venture_organization_new());
	g_object_set(organization, "name", "Elsewhere", "slug", "elsewhere", NULL);
	save(fixture, organization);
	theirs = VENTURE_ENTITY(venture_venture_new());
	venture_entity_set_organization_id(theirs, ID(organization));
	g_object_set(theirs, "name", "Theirs", NULL);
	save(fixture, theirs);
	{
		g_autoptr(VentureEntity) source = VENTURE_ENTITY(venture_data_source_new());
		g_autofree gchar *settings = g_strdup_printf("create_products: true\n"
		                                             "products_venture_id: %" G_GINT64_FORMAT "\n",
		                                             ID(theirs));

		venture_entity_set_organization_id(source, fixture->org);
		g_object_set(source, "name", "Bad", "provider", "push", "settings", settings,
		             "currency", "USD", NULL);
		save_refused(fixture, source, "no venture");
	}
}

/* --- Listing fields ---------------------------------------------------------------- */

/*
 * The new listing fields' rules for any writer: a bid in the price's
 * currency, not negative and not above a buyout; an expiry not before the
 * listing; a unique external id; and the mirror's own fields refused to
 * everybody but the mirror.
 *
 * What breaks if this regresses: a person's typo in a data source id makes
 * the mirror adopt and rewrite a hand listing, or a bid in gold on a
 * dollar listing totals as dollars.
 */
static void
test_mirror_listing_fields(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureEntity) listing = NULL;
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GDateTime) listed = NULL;
	g_autoptr(GDateTime) early = NULL;
	g_autoptr(VentureMoney) price = money_of("10.00 USD");
	g_autoptr(GError) error = NULL;

	(void)user_data;

	source = push_source(fixture, fixture->org, "Push", "");
	product = VENTURE_ENTITY(venture_product_new());
	venture_entity_set_organization_id(product, fixture->org);
	g_object_set(product, "name", "Ore", "venture-id", fixture->venture_id, NULL);
	save(fixture, product);

	listed = venture_time_from_string("2026-03-02", NULL);
	early = venture_time_from_string("2026-03-01", NULL);
	listing = VENTURE_ENTITY(venture_listing_new());
	venture_entity_set_organization_id(listing, fixture->org);
	g_object_set(listing, "product-id", ID(product), "quantity", (gint64)2, "unit-price", price,
	             "listed-at", listed, NULL);

	{
		g_autoptr(VentureMoney) bid = money_of("1.00 EUR");

		g_object_set(listing, "bid", bid, NULL);
		save_refused(fixture, listing, "share one currency");
	}
	{
		g_autoptr(VentureMoney) bid = money_of("-1.00 USD");

		g_object_set(listing, "bid", bid, NULL);
		save_refused(fixture, listing, "cannot be negative");
	}
	{
		g_autoptr(VentureMoney) bid = money_of("10.01 USD");

		g_object_set(listing, "bid", bid, NULL);
		save_refused(fixture, listing, "at most its buyout");
	}
	{
		g_autoptr(VentureMoney) bid = money_of("9.00 USD");

		g_object_set(listing, "bid", bid, "expires-at", early, NULL);
		save_refused(fixture, listing, "run out before it was listed");
	}

	g_object_set(listing, "expires-at", NULL, "external-id", "bad\nid", NULL);
	save_refused(fixture, listing, "control characters");
	g_object_set(listing, "external-id", "etsy:1", "data-source-id", ID(source), NULL);
	save_refused(fixture, listing, "position mirror alone");
	g_object_set(listing, "data-source-id", (gint64)0, "mirror-state", "{}", NULL);
	save_refused(fixture, listing, "only the mirror writes it");
	g_object_set(listing, "mirror-state", NULL, NULL);
	save(fixture, listing);

	/* A bid with no buyout at all is any bid. */
	{
		g_autoptr(VentureEntity) auction = VENTURE_ENTITY(venture_listing_new());
		g_autoptr(VentureMoney) zero = money_of("0.00 USD");
		g_autoptr(VentureMoney) bid = money_of("5.00 USD");

		venture_entity_set_organization_id(auction, fixture->org);
		g_object_set(auction, "product-id", ID(product), "quantity", (gint64)1,
		             "unit-price", zero, "bid", bid, "listed-at", listed, NULL);
		save(fixture, auction);
	}

	/* The id is unique, deleted listings included, and says which. */
	other = VENTURE_ENTITY(venture_listing_new());
	venture_entity_set_organization_id(other, fixture->org);
	g_object_set(other, "product-id", ID(product), "quantity", (gint64)1, "unit-price", price,
	             "listed-at", listed, "external-id", "etsy:1", NULL);
	save_refused(fixture, other, "already has the external id");
	g_assert_true(venture_database_delete(fixture->database, listing, NULL, &error));
	save_refused(fixture, other, "restore it instead");
}

/* --- Positions ---------------------------------------------------------------------- */

/*
 * Positions become open listings with every field the store has; a later
 * push moves the price, the expiry and the bid, and a position that lost
 * units while still listed counts them sold.
 *
 * What breaks if this regresses: the operator's auctions never reach the
 * books, or a repriced auction keeps its first price for ever and the
 * undercut alert compares against a stale number.
 */
static void
test_mirror_creates_and_updates(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) listing = NULL;
	g_autoptr(VentureEntity) location = NULL;
	g_autoptr(VentureEntity) venue = NULL;
	g_autoptr(VentureEntity) instrument = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autofree gchar *settings = products_settings(fixture, NULL);
	g_autofree gchar *channel = NULL;
	g_autofree gchar *price = NULL;
	g_autofree gchar *bid = NULL;
	g_autofree gchar *ref = NULL;
	gint64 posted = fixture->now - 7 * HOUR;
	gint64 expires = fixture->now + 40 * HOUR;

	(void)user_data;

	source = push_source(fixture, fixture->org, "Push", settings);
	line_market(body);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 6 * HOUR);
	line_position(body, "Drgold-Thorium", "a1", "2770", 20, "8.17", "7.50", posted, expires);
	line_position(body, "Drgold-Thorium", "a2", "2447", 1, "3.00", NULL, posted, expires);
	run = push(fixture, source, body->str);
	g_assert_true(run_says(run, "mirror: listings 2 created"));

	listing = listing_of(fixture, source, "a1");
	g_assert_nonnull(listing);
	g_assert_cmpint(outcome_of(listing), ==, VENTURE_LISTING_OUTCOME_OPEN);
	g_assert_cmpint(int_of(listing, "quantity"), ==, 20);
	g_assert_cmpint(int_of(listing, "quantity-sold"), ==, 0);
	g_assert_cmpint(time_of(listing, "listed-at"), ==, posted);
	g_assert_cmpint(time_of(listing, "expires-at"), ==, expires);
	g_assert_cmpint(int_of(listing, "data-source-id"), ==, ID(source));
	price = money_text(listing, "unit-price");
	bid = money_text(listing, "bid");
	g_assert_cmpstr(price, ==, "8.17 USD");
	g_assert_cmpstr(bid, ==, "7.50 USD");
	g_object_get(listing, "channel", &channel, NULL);
	g_assert_cmpstr(channel, ==, "Thorium Brotherhood");

	/* Its place, its venue and its product came with it. */
	ref = venture_marketdata_account_ref(source, "Drgold-Thorium");
	location = location_by_ref(fixture, fixture->org, ref);
	g_assert_cmpint(int_of(listing, "location-id"), ==, ID(location));
	venue = reread(fixture, VENTURE_TYPE_VENUE, int_of(listing, "venue-id"));
	g_assert_cmpint(int_of(venue, "location-id"), ==, ID(location));
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INSTRUMENT);
		g_autoptr(VentureEntity) product = NULL;
		g_autofree gchar *name = NULL;

		g_assert_true(venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, "2770",
		                                              NULL));
		instrument = venture_database_find_one(fixture->database, query, NULL);
		g_assert_cmpint(int_of(instrument, "product-id"), ==, int_of(listing, "product-id"));
		product = reread(fixture, VENTURE_TYPE_PRODUCT, int_of(listing, "product-id"));
		g_object_get(product, "name", &name, NULL);
		g_assert_cmpstr(name, ==, "Copper Ore");
		g_assert_cmpint(int_of(product, "venture-id"), ==, fixture->venture_id);
	}

	/* Repriced, a new expiry, no bid, and 5 of 20 bought while listed. */
	g_string_truncate(body, 0);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 5 * HOUR);
	line_position(body, "Drgold-Thorium", "a1", "2770", 15, "7.99", NULL, posted, expires + HOUR);
	line_position(body, "Drgold-Thorium", "a2", "2447", 1, "3.00", NULL, posted, expires);
	g_clear_object(&run);
	run = push(fixture, source, body->str);
	g_assert_true(run_says(run, "mirror: listings 0 created, 1 updated"));

	g_clear_object(&listing);
	g_clear_pointer(&price, g_free);
	g_clear_pointer(&bid, g_free);
	listing = listing_of(fixture, source, "a1");
	price = money_text(listing, "unit-price");
	bid = money_text(listing, "bid");
	g_assert_cmpstr(price, ==, "7.99 USD");
	g_assert_null(bid);
	g_assert_cmpint(time_of(listing, "expires-at"), ==, expires + HOUR);
	g_assert_cmpint(int_of(listing, "quantity"), ==, 20);
	g_assert_cmpint(int_of(listing, "quantity-sold"), ==, 5);
	g_assert_cmpint(outcome_of(listing), ==, VENTURE_LISTING_OUTCOME_OPEN);

	/* Nothing changed: nothing written, nothing said. */
	{
		g_autoptr(JsonObject) report = mirror(fixture, source);

		g_assert_cmpint(json_object_get_int_member(report, "writes"), ==, 0);
		g_assert_cmpint(json_object_get_int_member(report, "created"), ==, 0);
		g_assert_cmpint(json_object_get_int_member(report, "updated"), ==, 0);
	}
}

/* The outcome, sold count and closing time of one position's listing. */
static void
assert_closed(
	Fixture			*fixture,
	VentureEntity		*source,
	const gchar		*position,
	VentureListingOutcome	 outcome,
	gint64			 sold,
	gint64			 closed_at
){
	g_autoptr(VentureEntity) listing = listing_of(fixture, source, position);

	g_assert_nonnull(listing);

	if (outcome_of(listing) != outcome)
		g_error("position %s: outcome %d, expected %d", position, outcome_of(listing), outcome);

	g_assert_cmpint(int_of(listing, "quantity-sold"), ==, sold);

	if (closed_at > 0)
		g_assert_cmpint(time_of(listing, "closed-at"), ==, closed_at);
}

/*
 * Positions gone from a complete snapshot are judged from the ledger:
 * sold, partly sold, expired by a row, cancelled by a row, expired by
 * their own expiry with no row at all, and cancelled when they left early
 * and no row came within the grace (here none).
 *
 * What breaks if this regresses: every auction that sold shows as
 * cancelled, the sale rate is a fiction, or an auction that ran out is
 * counted as a sale.
 */
static void
test_mirror_closes(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autofree gchar *settings = products_settings(fixture, "mirror_grace_hours: 0\n");
	gint64 posted = fixture->now - 10 * HOUR;
	gint64 later = fixture->now + 30 * HOUR;
	gint64 soon = fixture->now - 4 * HOUR;
	gint64 gone = fixture->now - 2 * HOUR;

	(void)user_data;

	source = push_source(fixture, fixture->org, "Push", settings);
	line_market(body);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 9 * HOUR);
	line_position(body, "Drgold-Thorium", "sold", "2770", 3, "5.00", NULL, posted, later);
	line_position(body, "Drgold-Thorium", "partial", "2447", 5, "2.00", NULL, posted, later);
	line_position(body, "Drgold-Thorium", "expired-row", "2770", 2, "6.00", NULL, posted + 60, later);
	line_position(body, "Drgold-Thorium", "ran-out", "2770", 1, "7.00", NULL, posted + 120, soon);
	line_position(body, "Drgold-Thorium", "cancel-row", "2447", 1, "9.00", NULL, posted + 180, later);
	line_position(body, "Drgold-Thorium", "taken-down", "2447", 1, "9.50", NULL, posted + 240, later);
	line_position(body, "Drgold-Thorium", "stays", "2447", 1, "9.75", NULL, posted + 300, later);
	run = push(fixture, source, body->str);
	g_assert_true(run_says(run, "listings 7 created"));

	/* Only "stays" is still there; the ledger says what became of the
	 * rest -- the 2447 sale fills "partial", the oldest of that item, and
	 * the cancel row is the next one's. */
	g_string_truncate(body, 0);
	line_snapshot(body, "Drgold-Thorium", gone);
	line_position(body, "Drgold-Thorium", "stays", "2447", 1, "9.75", NULL, posted + 300, later);
	line_txn(body, "t1", "Drgold-Thorium", "sale", "2770", 3, "14.25", fixture->now - 5 * HOUR);
	line_txn(body, "t2", "Drgold-Thorium", "sale", "2447", 2, "3.80", fixture->now - 6 * HOUR);
	line_txn(body, "t3", "Drgold-Thorium", "expired", "2770", 2, NULL, fixture->now - 3 * HOUR);
	line_txn(body, "t4", "Drgold-Thorium", "cancelled", "2447", 1, NULL, fixture->now - 4 * HOUR);
	g_clear_object(&run);
	run = push(fixture, source, body->str);

	assert_closed(fixture, source, "sold", VENTURE_LISTING_OUTCOME_SOLD, 3, fixture->now - 5 * HOUR);
	/* Two of five sold and nothing said about the rest, gone before
	 * expiry with no grace: partial, closed when it was found gone. */
	assert_closed(fixture, source, "partial", VENTURE_LISTING_OUTCOME_PARTIAL, 2, gone);
	assert_closed(fixture, source, "expired-row", VENTURE_LISTING_OUTCOME_EXPIRED, 0,
	              fixture->now - 3 * HOUR);
	assert_closed(fixture, source, "ran-out", VENTURE_LISTING_OUTCOME_EXPIRED, 0, soon);
	assert_closed(fixture, source, "cancel-row", VENTURE_LISTING_OUTCOME_CANCELLED, 0,
	              fixture->now - 4 * HOUR);
	assert_closed(fixture, source, "taken-down", VENTURE_LISTING_OUTCOME_CANCELLED, 0, gone);
	assert_closed(fixture, source, "stays", VENTURE_LISTING_OUTCOME_OPEN, 0, 0);
	g_assert_true(run_says(run, "6 closed (1 sold, 1 partial, 2 expired, 2 cancelled)"));

	/* listing_performance counts them like any listing. */
	{
		g_autoptr(VentureReportResult) result = NULL;
		g_autoptr(JsonObject) options = json_object_new();
		g_autoptr(VentureDateRange) range = NULL;
		g_autoptr(GError) error = NULL;
		VentureReport *report;
		const GValue *value;
		guint row;
		gdouble sold = 0;
		gdouble closed = 0;

		json_object_set_string_member(options, "group_by", "channel");
		report = venture_report_registry_lookup(
			venture_context_get_report_registry(fixture->context), "listing_performance");
		range = venture_context_parse_period(fixture->context, "all", NULL);
		result = venture_report_generate(report, fixture->context, range, options, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);

		for (row = 0; row < venture_report_result_get_row_count(result); row++)
		{
			value = venture_report_result_get_cell(result, row, "units_sold");
			sold += g_value_get_double(value);
			value = venture_report_result_get_cell(result, row, "units_closed");
			closed += g_value_get_double(value);
		}

		/* 3 + 2 sold, of 3 + 5 + 2 + 1 + 1 + 1 closed units. */
		g_assert_cmpfloat(sold, ==, 5);
		g_assert_cmpfloat(closed, ==, 13);
	}
}

/*
 * A position that left early waits for its ledger row within the grace;
 * when the row arrives it closes on it. Two listings of one item are
 * filled oldest first and a row is never used twice.
 *
 * What breaks if this regresses: an auction that sold is called
 * cancelled because its sale reached the ledger an hour after it left
 * the auction house, or one sale closes two auctions.
 */
static void
test_mirror_waits_and_fifo(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autofree gchar *settings = products_settings(fixture, NULL);
	gint64 posted = fixture->now - 10 * HOUR;
	gint64 later = fixture->now + 30 * HOUR;

	(void)user_data;

	source = push_source(fixture, fixture->org, "Push", settings);
	line_market(body);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 9 * HOUR);
	line_position(body, "Drgold-Thorium", "older", "2770", 1, "5.00", NULL, posted, later);
	line_position(body, "Drgold-Thorium", "newer", "2770", 1, "5.00", NULL, posted + 600, later);
	run = push(fixture, source, body->str);

	/* Both gone, nothing in the ledger yet: both wait. */
	g_string_truncate(body, 0);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 3 * HOUR);
	g_clear_object(&run);
	run = push(fixture, source, body->str);
	assert_closed(fixture, source, "older", VENTURE_LISTING_OUTCOME_OPEN, 0, 0);
	assert_closed(fixture, source, "newer", VENTURE_LISTING_OUTCOME_OPEN, 0, 0);
	g_assert_true(run_says(run, "2 waiting for the ledger"));

	/* One sale arrives: the older listing is the one that sold. */
	g_string_truncate(body, 0);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 2 * HOUR);
	line_txn(body, "s1", "Drgold-Thorium", "sale", "2770", 1, "4.75", fixture->now - 4 * HOUR);
	g_clear_object(&run);
	run = push(fixture, source, body->str);
	assert_closed(fixture, source, "older", VENTURE_LISTING_OUTCOME_SOLD, 1, fixture->now - 4 * HOUR);
	assert_closed(fixture, source, "newer", VENTURE_LISTING_OUTCOME_OPEN, 0, 0);

	/* The same row again, and a pass by hand: still one sale for one
	 * listing. */
	{
		g_autoptr(JsonObject) report = mirror(fixture, source);

		g_assert_cmpint(json_object_get_int_member(report, "waiting"), ==, 1);
		assert_closed(fixture, source, "newer", VENTURE_LISTING_OUTCOME_OPEN, 0, 0);
	}

	/* Its own sale: closed on it. */
	g_string_truncate(body, 0);
	line_snapshot(body, "Drgold-Thorium", fixture->now - HOUR);
	line_txn(body, "s2", "Drgold-Thorium", "sale", "2770", 1, "4.75", fixture->now - 90 * 60);
	g_clear_object(&run);
	run = push(fixture, source, body->str);
	assert_closed(fixture, source, "newer", VENTURE_LISTING_OUTCOME_SOLD, 1, fixture->now - 90 * 60);
	first = listing_of(fixture, source, "older");
	g_assert_cmpint(time_of(first, "closed-at"), ==, fixture->now - 4 * HOUR);
}

/*
 * A mirrored listing a person edited keeps the edit: a price they set
 * stays while the expiry still follows the store, and an outcome they set
 * makes the listing theirs. Listings the mirror did not make -- typed in,
 * or deleted mirrored ones -- are never written.
 *
 * What breaks if this regresses: every push undoes a correction, or the
 * mirror re-creates an auction a person deleted as a duplicate.
 */
static void
test_mirror_hand_edits(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) listing = NULL;
	g_autoptr(VentureEntity) hand = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = products_settings(fixture, "mirror_grace_hours: 0\n");
	gint64 posted = fixture->now - 10 * HOUR;
	gint64 later = fixture->now + 30 * HOUR;
	gint64 hand_version;
	gint64 hand_id;

	(void)user_data;

	source = push_source(fixture, fixture->org, "Push", settings);
	line_market(body);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 9 * HOUR);
	line_position(body, "Drgold-Thorium", "priced", "2770", 1, "5.00", NULL, posted, later);
	line_position(body, "Drgold-Thorium", "closed", "2770", 1, "5.00", NULL, posted, later);
	line_position(body, "Drgold-Thorium", "deleted", "2447", 1, "5.00", NULL, posted, later);
	run = push(fixture, source, body->str);

	/* A typed-in listing of the same product, not the mirror's. */
	listing = listing_of(fixture, source, "priced");
	hand = VENTURE_ENTITY(venture_listing_new());
	venture_entity_set_organization_id(hand, fixture->org);
	{
		g_autoptr(VentureMoney) price = money_of("6.00 USD");
		g_autoptr(GDateTime) listed = g_date_time_new_from_unix_utc(posted);

		g_object_set(hand, "product-id", int_of(listing, "product-id"), "quantity", (gint64)1,
		             "unit-price", price, "listed-at", listed, NULL);
		save(fixture, hand);
	}
	hand_version = venture_entity_get_version(hand);
	hand_id = ID(hand);

	/* A person sets a price; another closes one; a third is deleted. */
	{
		g_autoptr(VentureMoney) price = money_of("4.50 USD");

		g_object_set(listing, "unit-price", price, "notes", "priced by hand", NULL);
		save(fixture, listing);
	}
	{
		g_autoptr(VentureEntity) closed = listing_of(fixture, source, "closed");

		g_object_set(closed, "outcome", VENTURE_LISTING_OUTCOME_CANCELLED, NULL);
		save(fixture, closed);
	}
	{
		g_autoptr(VentureEntity) deleted = listing_of(fixture, source, "deleted");

		g_assert_true(venture_database_delete(fixture->database, deleted, NULL, &error));
	}

	/* The store moves both prices and the expiries; "closed" is gone. */
	g_string_truncate(body, 0);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 5 * HOUR);
	line_position(body, "Drgold-Thorium", "priced", "2770", 1, "5.25", NULL, posted, later + HOUR);
	line_position(body, "Drgold-Thorium", "deleted", "2447", 1, "5.25", NULL, posted, later + HOUR);
	g_clear_object(&run);
	run = push(fixture, source, body->str);

	g_clear_object(&listing);
	listing = listing_of(fixture, source, "priced");
	{
		g_autofree gchar *price = money_text(listing, "unit-price");

		g_assert_cmpstr(price, ==, "4.50 USD");
		g_assert_cmpint(time_of(listing, "expires-at"), ==, later + HOUR);
	}
	{
		g_autoptr(VentureEntity) closed = listing_of(fixture, source, "closed");
		g_autoptr(VentureEntity) deleted = listing_of(fixture, source, "deleted");

		g_assert_cmpint(outcome_of(closed), ==, VENTURE_LISTING_OUTCOME_CANCELLED);
		g_assert_true(venture_entity_is_deleted(deleted));
		g_assert_cmpint(count_of(fixture, VENTURE_TYPE_LISTING), ==, 3);
	}

	g_clear_object(&hand);
	hand = reread(fixture, VENTURE_TYPE_LISTING, hand_id);
	g_assert_cmpint(venture_entity_get_version(hand), ==, hand_version);

	/* The form posts the mirror's fields back as they are: that is no
	 * write of them. */
	g_object_set(listing, "notes", "still mine", NULL);
	save(fixture, listing);

	/* A venue a person deleted stays deleted; a new listing there names
	 * no venue and keeps the store's key as its channel. */
	{
		g_autoptr(VentureEntity) venue = reread(fixture, VENTURE_TYPE_VENUE,
		                                        int_of(listing, "venue-id"));
		g_autoptr(VentureEntity) fresh = NULL;
		g_autoptr(VentureEntity) still = NULL;
		g_autofree gchar *channel = NULL;

		g_assert_true(venture_database_delete(fixture->database, venue, NULL, &error));
		g_string_truncate(body, 0);
		line_snapshot(body, "Drgold-Thorium", fixture->now - 4 * HOUR);
		line_position(body, "Drgold-Thorium", "priced", "2770", 1, "5.25", NULL, posted,
		              later + HOUR);
		line_position(body, "Drgold-Thorium", "fresh", "2770", 1, "5.50", NULL, posted, later);
		g_clear_object(&run);
		run = push(fixture, source, body->str);

		fresh = listing_of(fixture, source, "fresh");
		g_assert_nonnull(fresh);
		g_assert_cmpint(int_of(fresh, "venue-id"), ==, 0);
		g_object_get(fresh, "channel", &channel, NULL);
		g_assert_cmpstr(channel, ==, "thorium");
		still = reread(fixture, VENTURE_TYPE_VENUE, ID(venue));
		g_assert_true(venture_entity_is_deleted(still));
	}
}

/*
 * A position whose item has no product is not mirrored, and the run says
 * which items and what to do; linking the instrument to a product lets
 * the next pass mirror it.
 *
 * What breaks if this regresses: listings with no product (refused) fail
 * every run silently, or products appear nobody asked for.
 */
static void
test_mirror_needs_a_product(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) instrument = NULL;
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureEntity) listing = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	gint64 products;

	(void)user_data;

	source = push_source(fixture, fixture->org, "Push", "");
	line_market(body);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 9 * HOUR);
	line_position(body, "Drgold-Thorium", "p1", "2770", 1, "5.00", NULL, fixture->now - 10 * HOUR,
	              fixture->now + 30 * HOUR);
	products = count_of(fixture, VENTURE_TYPE_PRODUCT);
	run = push(fixture, source, body->str);

	g_assert_true(run_says(run, "1 positions not mirrored: their items have no product"));
	g_assert_true(run_says(run, "e.g. 2770"));
	g_assert_null(listing_of(fixture, source, "p1"));
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_PRODUCT), ==, products);

	/* The instrument was promoted on the way; give it a product. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INSTRUMENT);

		g_assert_true(venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, "2770",
		                                              NULL));
		instrument = venture_database_find_one(fixture->database, query, NULL);
		g_assert_nonnull(instrument);
	}
	product = VENTURE_ENTITY(venture_product_new());
	venture_entity_set_organization_id(product, fixture->org);
	g_object_set(product, "name", "Ore", "venture-id", fixture->venture_id, NULL);
	save(fixture, product);
	g_object_set(instrument, "product-id", ID(product), NULL);
	save(fixture, instrument);

	{
		g_autoptr(JsonObject) report = mirror(fixture, source);

		g_assert_cmpint(json_object_get_int_member(report, "created"), ==, 1);
	}

	listing = listing_of(fixture, source, "p1");
	g_assert_cmpint(int_of(listing, "product-id"), ==, ID(product));
}

/*
 * A pass writes at most mirror_max_writes records and the next one goes
 * on from there, with nothing to remember between them.
 *
 * What breaks if this regresses: a first push of three thousand auctions
 * holds the server for minutes, or the rest are never mirrored.
 */
static void
test_mirror_cap(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) instrument = NULL;
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	guint i;

	(void)user_data;

	/* No places and products to make: only listings count here. */
	source = push_source(fixture, fixture->org, "Push",
	                     "auto_promote_accounts: false\nmirror_max_writes: 2\n");
	line_market(body);
	run = push(fixture, source, body->str);
	g_assert_true(venture_marketdata_promote_instrument(fixture->context, fixture->org, ID(source),
	                                                    "2770", NULL, &instrument, &error));
	product = VENTURE_ENTITY(venture_product_new());
	venture_entity_set_organization_id(product, fixture->org);
	g_object_set(product, "name", "Ore", "venture-id", fixture->venture_id, NULL);
	save(fixture, product);
	g_object_set(instrument, "product-id", ID(product), NULL);
	save(fixture, instrument);

	g_string_truncate(body, 0);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 9 * HOUR);

	for (i = 0; i < 5; i++)
	{
		g_autofree gchar *id = g_strdup_printf("p%u", i);

		line_position(body, "Drgold-Thorium", id, "2770", 1, "5.00", NULL,
		              fixture->now - 10 * HOUR, fixture->now + (gint64)(10 + i) * HOUR);
	}

	g_clear_object(&run);
	run = push(fixture, source, body->str);
	g_assert_true(run_says(run, "3 changes left for the next run"));
	/* Soonest expiry first. */
	g_assert_nonnull(listing_of(fixture, source, "p0"));
	g_assert_nonnull(listing_of(fixture, source, "p1"));
	g_assert_null(listing_of(fixture, source, "p2"));

	{
		g_autoptr(JsonObject) report = mirror(fixture, source);

		g_assert_cmpint(json_object_get_int_member(report, "created"), ==, 2);
		g_assert_cmpint(json_object_get_int_member(report, "over_cap"), ==, 1);
	}
	{
		g_autoptr(JsonObject) report = mirror(fixture, source);

		g_assert_cmpint(json_object_get_int_member(report, "created"), ==, 1);
		g_assert_cmpint(json_object_get_int_member(report, "over_cap"), ==, 0);
	}
	{
		g_autoptr(JsonObject) report = mirror(fixture, source);

		g_assert_cmpint(json_object_get_int_member(report, "writes"), ==, 0);
	}

	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_LISTING), ==, 5);
}

/*
 * The undercut alert reads mirrored listings like any open listing at a
 * venue: a rival's cheaper offer in the venue's newest snapshot fires.
 *
 * What breaks if this regresses: the alert the whole mirror exists for
 * never fires on the operator's real auctions.
 */
static void
test_mirror_undercut(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) listing = NULL;
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = products_settings(fixture, NULL);
	g_autofree gchar *taken = iso(fixture->now - 4 * HOUR);

	(void)user_data;

	source = push_source(fixture, fixture->org, "Push", settings);
	line_market(body);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 5 * HOUR);
	line_position(body, "Drgold-Thorium", "mine", "2770", 2, "8.00", NULL, fixture->now - 6 * HOUR,
	              fixture->now + 30 * HOUR);
	g_string_append_printf(body, "{\"type\":\"snapshot\",\"venue\":\"thorium\",\"taken_at\":\"%s\"}\n"
	                       "{\"type\":\"listing\",\"venue\":\"thorium\",\"instrument\":\"2770\","
	                       "\"price\":\"8.00\",\"quantity\":2,\"id\":\"1\"}\n"
	                       "{\"type\":\"listing\",\"venue\":\"thorium\",\"instrument\":\"2770\","
	                       "\"price\":\"7.50\",\"quantity\":4,\"id\":\"2\"}\n", taken);
	run = push(fixture, source, body->str);
	listing = listing_of(fixture, source, "mine");
	g_assert_nonnull(listing);

	rule = VENTURE_ENTITY(venture_alert_rule_new());
	venture_entity_set_organization_id(rule, fixture->org);
	g_object_set(rule, "name", "undercut", "kind", VENTURE_ALERT_KIND_UNDERCUT, NULL);
	save(fixture, rule);

	g_assert_true(venture_marketdata_alerts_evaluate(fixture->context, rule, TRUE, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(report), "written"), ==, 1);

	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ALERT_HIT);
		g_autoptr(VentureEntity) hit = venture_database_find_one(fixture->database, query, NULL);

		g_assert_nonnull(hit);
		g_assert_cmpint(int_of(hit, "listing-id"), ==, ID(listing));
	}
}

/*
 * With marketdata off the hook writes nothing and a pass by hand says so;
 * back on, a pass mirrors what the store kept meanwhile.
 *
 * What breaks if this regresses: a push fails, or warns on every run,
 * while the module is off -- or listings are written into masked tables.
 */
static void
test_mirror_modules_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = products_settings(fixture, NULL);

	(void)user_data;

	source = push_source(fixture, fixture->org, "Push", settings);
	venture_config_set_module_enabled(fixture->config, "marketdata", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "marketdata"));

	line_market(body);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 9 * HOUR);
	line_position(body, "Drgold-Thorium", "p1", "2770", 1, "5.00", NULL, fixture->now - 10 * HOUR,
	              fixture->now + 30 * HOUR);
	run = push(fixture, source, body->str);
	g_assert_false(run_says(run, "mirror"));
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_LISTING), ==, 0);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_LOCATION), ==, 0);

	g_assert_false(venture_marketdata_mirror_positions(fixture->context, fixture->org, ID(source),
	                                                   NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	g_assert_false(venture_marketdata_promote_account(fixture->context, fixture->org, ID(source),
	                                                  "Drgold-Thorium", NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);

	venture_config_set_module_enabled(fixture->config, "marketdata", TRUE);
	{
		g_autoptr(JsonObject) report = mirror(fixture, source);

		g_assert_cmpint(json_object_get_int_member(report, "created"), ==, 1);
		g_assert_cmpint(json_object_get_int_member(report, "promoted"), ==, 1);
	}

	/* Switched off for the source: a note, nothing written. */
	g_object_set(source, "settings", "mirror_positions: false\n", NULL);
	save(fixture, source);
	{
		g_autoptr(JsonObject) report = mirror(fixture, source);
		JsonArray *notes = json_object_get_array_member(report, "notes");

		g_assert_cmpint(json_object_get_int_member(report, "writes"), ==, 0);
		g_assert_nonnull(strstr(json_array_get_string_element(notes, 0), "switched off"));
	}
}

/*
 * A second organization's source mirrors into that organization alone:
 * its places, venues, products and listings are its own, and the first
 * organization's pass and promotion cannot reach them.
 *
 * What breaks if this regresses: one business's auctions show up in
 * another's sale rate, or a promotion files a character under the wrong
 * books.
 */
static void
test_mirror_organizations(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureEntity) venture = NULL;
	g_autoptr(VentureEntity) mine = NULL;
	g_autoptr(VentureEntity) theirs = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) listing = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *mine_settings = products_settings(fixture, NULL);
	g_autofree gchar *their_settings = NULL;
	gint64 other;

	(void)user_data;

	organization = VENTURE_ENTITY(venture_organization_new());
	g_object_set(organization, "name", "Evermoor", "slug", "evermoor", NULL);
	save(fixture, organization);
	other = ID(organization);
	venture = VENTURE_ENTITY(venture_venture_new());
	venture_entity_set_organization_id(venture, other);
	g_object_set(venture, "name", "Their gold", NULL);
	save(fixture, venture);
	their_settings = g_strdup_printf("create_products: true\nproducts_venture_id: %"
	                                 G_GINT64_FORMAT "\naccount_namespace: shared\n", ID(venture));

	mine = push_source(fixture, fixture->org, "Mine", mine_settings);
	theirs = push_source(fixture, other, "Theirs", their_settings);

	line_market(body);
	line_snapshot(body, "Drgold-Thorium", fixture->now - 9 * HOUR);
	line_position(body, "Drgold-Thorium", "p1", "2770", 1, "5.00", NULL, fixture->now - 10 * HOUR,
	              fixture->now + 30 * HOUR);
	run = push(fixture, theirs, body->str);

	listing = listing_of(fixture, theirs, "p1");
	g_assert_nonnull(listing);
	g_assert_cmpint(venture_entity_get_organization_id(listing), ==, other);
	{
		g_autoptr(VentureEntity) product = reread(fixture, VENTURE_TYPE_PRODUCT,
		                                          int_of(listing, "product-id"));
		g_autoptr(VentureEntity) venue = reread(fixture, VENTURE_TYPE_VENUE,
		                                        int_of(listing, "venue-id"));
		g_autoptr(VentureEntity) place = reread(fixture, VENTURE_TYPE_LOCATION,
		                                        int_of(listing, "location-id"));

		g_assert_cmpint(venture_entity_get_organization_id(product), ==, other);
		g_assert_cmpint(venture_entity_get_organization_id(venue), ==, other);
		g_assert_cmpint(venture_entity_get_organization_id(place), ==, other);
	}
	g_assert_null(location_by_ref(fixture, fixture->org, "shared:Drgold-Thorium"));

	/* The home organization's pass sees its own source only, and cannot
	 * name the other's. */
	{
		g_autoptr(JsonObject) report = mirror(fixture, mine);

		g_assert_cmpint(json_object_get_int_member(report, "writes"), ==, 0);
	}
	g_assert_false(venture_marketdata_mirror_positions(fixture->context, fixture->org, ID(theirs),
	                                                   NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(venture_marketdata_promote_account(fixture->context, fixture->org, ID(theirs),
	                                                  "Drgold-Thorium", NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/account-mirror/promotion", test_mirror_promotion);
	ADD("/account-mirror/auto-promotion", test_mirror_auto_promotion);
	ADD("/account-mirror/settings", test_mirror_settings);
	ADD("/account-mirror/listing-fields", test_mirror_listing_fields);
	ADD("/account-mirror/creates-and-updates", test_mirror_creates_and_updates);
	ADD("/account-mirror/closes", test_mirror_closes);
	ADD("/account-mirror/waits-and-fifo", test_mirror_waits_and_fifo);
	ADD("/account-mirror/hand-edits", test_mirror_hand_edits);
	ADD("/account-mirror/needs-a-product", test_mirror_needs_a_product);
	ADD("/account-mirror/cap", test_mirror_cap);
	ADD("/account-mirror/undercut", test_mirror_undercut);
	ADD("/account-mirror/modules-off", test_mirror_modules_off);
	ADD("/account-mirror/organizations", test_mirror_organizations);

#undef ADD

	return g_test_run();
}

#else /* !VENTURE_HAVE_SQLITE */

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	return g_test_run();
}

#endif /* VENTURE_HAVE_SQLITE */
