/*
 * test-database.c - Storage, querying and persistence
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every test runs against a private in-memory SQLite database, so the suite
 * needs no external service and cannot leave state behind between cases.
 * Set VENTURE_TEST_DB to a postgres:// URI to run the same tests against
 * PostgreSQL instead.
 */

#include <venture.h>

typedef struct
{
	VentureDatabase		*database;
	VentureEntityRegistry	*registry;
	gint64			 organization_id;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	const gchar *uri;

	uri = g_getenv("VENTURE_TEST_DB");

	fixture->database = venture_database_new(
		(NULL != uri) ? uri : "sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_nonnull(fixture->database);

	/*
	 * Every test starts from an empty database.
	 *
	 * With the default sqlite://:memory: that is free -- each connection
	 * is its own database. An external VENTURE_TEST_DB is one shared
	 * database for the whole run, so without this the tests interfere:
	 * several of them create a venture with the slug "books", the column
	 * is UNIQUE, and the second one to run fails a save for reasons that
	 * have nothing to do with what it is testing. The suite passed on
	 * SQLite and died on PostgreSQL at the ninth test.
	 */
	if (NULL != uri)
	{
		g_assert_true(orm_connection_execute(
			venture_database_get_connection(fixture->database),
			"DROP SCHEMA public CASCADE; CREATE SCHEMA public;",
			&error));
		g_assert_no_error(error);
	}

	fixture->registry = venture_entity_registry_get_default();

	g_assert_true(venture_database_migrate(fixture->database,
	                                       fixture->registry, &error));
	g_assert_no_error(error);

	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	organization = venture_database_find_one(fixture->database, query, NULL);
	g_assert_nonnull(organization);
	fixture->organization_id = venture_entity_get_id(organization);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_clear_object(&fixture->database);
}

/* --- Migration ----------------------------------------------------------- */

static void
test_database_migrate_creates_tables(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GHashTable) columns = NULL;
	g_autoptr(GError) error = NULL;

	columns = venture_schema_get_existing_columns(
		venture_database_get_connection(fixture->database), "sales", &error);

	g_assert_no_error(error);
	g_assert_nonnull(columns);

	g_assert_true(g_hash_table_contains(columns, "id"));
	g_assert_true(g_hash_table_contains(columns, "uuid"));
	g_assert_true(g_hash_table_contains(columns, "occurred_at"));

	/* Money expands to three columns so the database can total it. */
	g_assert_true(g_hash_table_contains(columns, "gross_amount"));
	g_assert_true(g_hash_table_contains(columns, "gross_currency"));
	g_assert_true(g_hash_table_contains(columns, "gross_exponent"));
}

static void
test_database_migrate_is_idempotent(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = NULL;
	gint64 before;
	gint64 after;

	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	before = venture_database_count(fixture->database, query, NULL);

	/* Running again must not duplicate the seeded rows. */
	g_assert_true(venture_database_migrate(fixture->database,
	                                       fixture->registry, &error));
	g_assert_no_error(error);

	after = venture_database_count(fixture->database, query, NULL);
	g_assert_cmpint(before, ==, after);
}

static void
test_database_seeds_chart_of_accounts(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) sales = NULL;
	VentureAccountKind kind;

	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_assert_true(venture_query_add_filter_string(query, "code",
		VENTURE_FILTER_OP_EQ, "4000", NULL));

	sales = venture_database_find_one(fixture->database, query, NULL);

	g_assert_nonnull(sales);
	g_object_get(sales, "kind", &kind, NULL);
	g_assert_cmpint(kind, ==, VENTURE_ACCOUNT_KIND_INCOME);
}

static void
test_database_seeds_tax_categories_conservatively(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) travel = NULL;
	VentureDeductibility deductibility;

	query = venture_query_new(VENTURE_TYPE_TAX_CATEGORY);
	g_assert_true(venture_query_add_filter_string(query, "code",
		VENTURE_FILTER_OP_EQ, "TRAVEL", NULL));

	travel = venture_database_find_one(fixture->database, query, NULL);
	g_assert_nonnull(travel);

	/* The contentious categories default to review, not to a confident
	 * deduction the operator never actually confirmed. */
	g_object_get(travel, "deductibility", &deductibility, NULL);
	g_assert_cmpint(deductibility, ==, VENTURE_DEDUCTIBILITY_REVIEW);
}

/* --- Round trip ---------------------------------------------------------- */

static VentureVenture *
create_venture(
	Fixture		*fixture,
	const gchar	*name
){
	VentureVenture *venture;
	g_autoptr(GError) error = NULL;

	venture = venture_venture_new();
	g_object_set(venture,
	             "name", name,
	             "slug", name,
	             "venture-type", "books",
	             "status", VENTURE_VENTURE_STATUS_ACTIVE,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture),
	                                   fixture->organization_id);

	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(venture), NULL, &error));
	g_assert_no_error(error);

	return venture;
}

static void
test_database_save_and_get(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) original = NULL;
	g_autoptr(VentureEntity) loaded = NULL;
	g_autofree gchar *name = NULL;
	VentureVentureStatus status;

	original = create_venture(fixture, "etsy-shop");

	g_assert_true(venture_entity_is_persisted(VENTURE_ENTITY(original)));
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(original)), >, 0);

	loaded = venture_database_get(fixture->database, VENTURE_TYPE_VENTURE,
	                              venture_entity_get_id(VENTURE_ENTITY(original)),
	                              NULL);

	g_assert_nonnull(loaded);
	g_object_get(loaded, "name", &name, "status", &status, NULL);
	g_assert_cmpstr(name, ==, "etsy-shop");
	g_assert_cmpint(status, ==, VENTURE_VENTURE_STATUS_ACTIVE);
}

/*
 * A stored conversation is replayed to the model when its thread resumes, so
 * the transcript must come back exactly as written and in the order it was
 * said. If the role enum did not survive the round trip, a resumed thread
 * would replay the assistant's words as the user's -- which does not crash
 * anything, it just quietly makes every resumed conversation nonsense.
 */
static void
test_database_chat_round_trip(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureChatThread) thread = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	static const struct
	{
		VentureChatRole	 role;
		const gchar	*body;
	} lines[] = {
		{ VENTURE_CHAT_ROLE_USER,      "how were sales in march" },
		{ VENTURE_CHAT_ROLE_ASSISTANT, "March grossed $1,240."   },
		{ VENTURE_CHAT_ROLE_USER,      "and the fees on that"    }
	};
	gsize i;

	thread = venture_chat_thread_new();
	g_object_set(thread, "title", "march numbers", "user-id", (gint64)7,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(thread),
	                                   fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(thread), NULL, &error));
	g_assert_no_error(error);

	for (i = 0; i < G_N_ELEMENTS(lines); i++)
	{
		g_autoptr(VentureChatMessage) message = NULL;

		message = venture_chat_message_new();
		g_object_set(message,
		             "thread-id",
		             venture_entity_get_id(VENTURE_ENTITY(thread)),
		             "role", lines[i].role,
		             "body", lines[i].body,
		             NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(message),
		                                   fixture->organization_id);
		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(message), NULL,
		                                    &error));
		g_assert_no_error(error);
	}

	query = venture_query_new(VENTURE_TYPE_CHAT_MESSAGE);
	g_assert_true(venture_query_add_filter_int(query, "thread-id",
		VENTURE_FILTER_OP_EQ,
		venture_entity_get_id(VENTURE_ENTITY(thread)), NULL));
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);

	messages = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(messages->len, ==, G_N_ELEMENTS(lines));

	for (i = 0; i < messages->len; i++)
	{
		g_autofree gchar *body = NULL;
		VentureChatRole role;

		g_object_get(g_ptr_array_index(messages, i),
		             "role", &role, "body", &body, NULL);
		g_assert_cmpint(role, ==, lines[i].role);
		g_assert_cmpstr(body, ==, lines[i].body);
	}
}

/*
 * 2.5 x $19.99 is $49.975, which does not exist. The line amount must be
 * $49.98 by one half-to-even rounding of the exact rational -- not $49.97
 * from a truncated double, and not a stored third copy of the number that
 * could disagree with quantity and unit price later.
 */
static void
test_database_invoice_line_amount(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureInvoiceLine) line = NULL;
	g_autoptr(VentureMoney) unit_price = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GError) error = NULL;

	line = venture_invoice_line_new();
	unit_price = venture_money_new(1999, "USD", 2);
	g_object_set(line, "description", "editing, hourly",
	             "quantity", 2.5,
	             "unit-price", unit_price, NULL);

	amount = venture_invoice_line_get_amount(line, &error);

	g_assert_no_error(error);
	g_assert_nonnull(amount);
	g_assert_cmpint(venture_money_get_amount(amount), ==, 4998);

	/* A whole quantity stays exact. */
	g_object_set(line, "quantity", 3.0, NULL);
	g_clear_pointer(&amount, venture_money_free);
	amount = venture_invoice_line_get_amount(line, NULL);
	g_assert_cmpint(venture_money_get_amount(amount), ==, 5997);

	/* No unit price is an error, not a zero: a zero would total as if
	 * the line were free, which is a claim, not an absence. */
	g_object_set(line, "unit-price", NULL, NULL);
	g_clear_pointer(&amount, venture_money_free);
	amount = venture_invoice_line_get_amount(line, &error);
	g_assert_null(amount);
	g_assert_nonnull(error);
}

static void
test_database_money_round_trip(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureSale) sale = NULL;
	g_autoptr(VentureEntity) loaded = NULL;
	g_autoptr(VentureMoney) gross = NULL;
	g_autoptr(VentureMoney) loaded_gross = NULL;
	g_autoptr(GError) error = NULL;

	venture = create_venture(fixture, "books");

	sale = venture_sale_new();
	gross = venture_money_new(2599, "EUR", 2);
	g_object_set(sale,
	             "venture-id", venture_entity_get_id(VENTURE_ENTITY(venture)),
	             "gross", gross,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(sale),
	                                   fixture->organization_id);

	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(sale), NULL, &error));
	g_assert_no_error(error);

	loaded = venture_database_get(fixture->database, VENTURE_TYPE_SALE,
	                              venture_entity_get_id(VENTURE_ENTITY(sale)),
	                              NULL);
	g_object_get(loaded, "gross", &loaded_gross, NULL);

	/* The exact amount, the currency and the exponent all survive. */
	g_assert_nonnull(loaded_gross);
	g_assert_cmpint(venture_money_get_amount(loaded_gross), ==, 2599);
	g_assert_cmpstr(venture_money_get_currency(loaded_gross), ==, "EUR");
	g_assert_true(venture_money_equal(gross, loaded_gross));
}

static void
test_database_null_money_stays_null(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureSale) sale = NULL;
	g_autoptr(VentureEntity) loaded = NULL;
	g_autoptr(VentureMoney) fees = NULL;

	venture = create_venture(fixture, "books");

	sale = venture_sale_new();
	g_object_set(sale, "venture-id",
	             venture_entity_get_id(VENTURE_ENTITY(venture)), NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(sale), NULL, NULL));

	loaded = venture_database_get(fixture->database, VENTURE_TYPE_SALE,
	                              venture_entity_get_id(VENTURE_ENTITY(sale)),
	                              NULL);
	g_object_get(loaded, "fees", &fees, NULL);

	/* An unset amount must stay unset rather than becoming zero, so
	 * "no fee recorded" is distinguishable from "the fee was nothing". */
	g_assert_null(fees);
}

static void
test_database_timestamp_round_trip(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureEntity) loaded = NULL;
	g_autoptr(GDateTime) started = NULL;
	g_autoptr(GDateTime) loaded_started = NULL;

	venture = create_venture(fixture, "books");
	started = g_date_time_new_utc(2026, 3, 14, 9, 26, 53.0);
	g_object_set(venture, "started-at", started, NULL);

	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(venture), NULL, NULL));

	loaded = venture_database_get(fixture->database, VENTURE_TYPE_VENTURE,
	                              venture_entity_get_id(VENTURE_ENTITY(venture)),
	                              NULL);
	g_object_get(loaded, "started-at", &loaded_started, NULL);

	g_assert_nonnull(loaded_started);
	g_assert_cmpint(g_date_time_get_year(loaded_started), ==, 2026);
	g_assert_cmpint(g_date_time_get_month(loaded_started), ==, 3);
	g_assert_cmpint(g_date_time_get_day_of_month(loaded_started), ==, 14);
	g_assert_cmpint(g_date_time_get_hour(loaded_started), ==, 9);
}

static void
test_database_get_by_uuid(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureEntity) loaded = NULL;

	venture = create_venture(fixture, "books");

	loaded = venture_database_get_by_uuid(fixture->database,
		VENTURE_TYPE_VENTURE,
		venture_entity_get_uuid(VENTURE_ENTITY(venture)), NULL);

	g_assert_nonnull(loaded);
	g_assert_cmpint(venture_entity_get_id(loaded), ==,
	                venture_entity_get_id(VENTURE_ENTITY(venture)));
}

/* --- Concurrency --------------------------------------------------------- */

static void
test_database_optimistic_concurrency(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureEntity) first_window = NULL;
	g_autoptr(VentureEntity) second_window = NULL;
	g_autoptr(GError) error = NULL;

	venture = create_venture(fixture, "books");

	/* Two independent loads of the same row, as two browser tabs or a
	 * person and an automation would produce. */
	first_window = venture_database_get(fixture->database, VENTURE_TYPE_VENTURE,
		venture_entity_get_id(VENTURE_ENTITY(venture)), NULL);
	second_window = venture_database_get(fixture->database, VENTURE_TYPE_VENTURE,
		venture_entity_get_id(VENTURE_ENTITY(venture)), NULL);

	g_object_set(first_window, "name", "renamed-by-first", NULL);
	g_assert_true(venture_database_save(fixture->database, first_window,
	                                    NULL, &error));
	g_assert_no_error(error);

	/* The second write carries a version that is no longer current and
	 * must be refused rather than silently overwriting the first. */
	g_object_set(second_window, "name", "renamed-by-second", NULL);
	g_assert_false(venture_database_save(fixture->database, second_window,
	                                     NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

static void
test_database_save_without_changes_is_noop(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	gint64 version_before;

	venture = create_venture(fixture, "books");
	version_before = venture_entity_get_version(VENTURE_ENTITY(venture));

	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(venture), NULL, NULL));

	/* Saving an unchanged record must not bump the version, or two tabs
	 * merely viewing a record would start conflicting with each other. */
	g_assert_cmpint(venture_entity_get_version(VENTURE_ENTITY(venture)),
	                ==, version_before);
}

static void
test_database_validation_blocks_save(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAccount) account = NULL;
	g_autoptr(GError) error = NULL;

	account = venture_account_new();

	/* Required fields are checked before anything is written. */
	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(account), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_false(venture_entity_is_persisted(VENTURE_ENTITY(account)));
}

/* --- Querying ------------------------------------------------------------ */

static void
test_database_query_filters(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) results = NULL;
	g_autoptr(VentureVenture) active = NULL;
	g_autoptr(VentureVenture) paused = NULL;

	active = create_venture(fixture, "active-one");
	paused = create_venture(fixture, "paused-one");
	g_object_set(paused, "status", VENTURE_VENTURE_STATUS_PAUSED, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(paused), NULL, NULL));

	query = venture_query_new(VENTURE_TYPE_VENTURE);
	g_assert_true(venture_query_add_filter_string(query, "status",
		VENTURE_FILTER_OP_EQ, "active", NULL));

	results = venture_database_find(fixture->database, query, NULL);

	g_assert_nonnull(results);
	g_assert_cmpuint(results->len, ==, 1);
}

static void
test_database_query_rejects_unknown_field(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;

	query = venture_query_new(VENTURE_TYPE_VENTURE);

	/*
	 * The gate that makes injection structurally impossible: a field name
	 * that is not a real property is rejected here, so no caller-supplied
	 * string can ever reach the statement as text.
	 */
	g_assert_false(venture_query_add_filter_string(query,
		"name\"; DROP TABLE ventures; --", VENTURE_FILTER_OP_EQ, "x",
		&error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

static void
test_database_query_value_is_never_interpolated(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) results = NULL;
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(GError) error = NULL;

	venture = create_venture(fixture, "books");

	query = venture_query_new(VENTURE_TYPE_VENTURE);
	g_assert_true(venture_query_add_filter_string(query, "name",
		VENTURE_FILTER_OP_EQ, "'; DROP TABLE ventures; --", NULL));

	/* A hostile value is bound as a parameter, so it simply matches
	 * nothing and the table is still there afterwards. */
	results = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(results->len, ==, 0);

	{
		g_autoptr(VentureQuery) verify = NULL;

		verify = venture_query_new(VENTURE_TYPE_VENTURE);
		g_assert_cmpint(venture_database_count(fixture->database, verify,
		                                       NULL), ==, 1);
	}
}

static void
test_database_query_in_operator(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) results = NULL;
	g_autoptr(GPtrArray) values = NULL;
	g_autoptr(VentureVenture) a = NULL;
	g_autoptr(VentureVenture) b = NULL;
	g_autoptr(VentureVenture) c = NULL;

	a = create_venture(fixture, "alpha");
	b = create_venture(fixture, "beta");
	c = create_venture(fixture, "gamma");

	values = g_ptr_array_new();
	g_ptr_array_add(values, (gpointer)"alpha");
	g_ptr_array_add(values, (gpointer)"gamma");

	query = venture_query_new(VENTURE_TYPE_VENTURE);
	g_assert_true(venture_query_add_filter(query, "name",
		VENTURE_FILTER_OP_IN, values, NULL));

	results = venture_database_find(fixture->database, query, NULL);
	g_assert_cmpuint(results->len, ==, 2);
}

static void
test_database_query_search(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) results = NULL;
	g_autoptr(VentureContact) contact = NULL;

	contact = venture_contact_new();
	g_object_set(contact, "name", "Ada Lovelace",
	             "role", "Analyst", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(contact),
	                                   fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(contact), NULL, NULL));

	query = venture_query_new(VENTURE_TYPE_CONTACT);
	venture_query_set_search(query, "lovelace");

	/* Search spans every searchable field and ignores case. */
	results = venture_database_find(fixture->database, query, NULL);
	g_assert_cmpuint(results->len, ==, 1);
}

static void
test_database_query_paging_is_stable(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) first_page = NULL;
	g_autoptr(VentureQuery) second_page = NULL;
	g_autoptr(GPtrArray) first = NULL;
	g_autoptr(GPtrArray) second = NULL;
	guint i;

	for (i = 0; i < 5; i++)
	{
		g_autofree gchar *name = NULL;
		g_autoptr(VentureVenture) venture = NULL;

		name = g_strdup_printf("venture-%u", i);
		venture = create_venture(fixture, name);
	}

	first_page = venture_query_new(VENTURE_TYPE_VENTURE);
	venture_query_set_limit(first_page, 2);
	first = venture_database_find(fixture->database, first_page, NULL);

	second_page = venture_query_new(VENTURE_TYPE_VENTURE);
	venture_query_set_limit(second_page, 2);
	venture_query_set_offset(second_page, 2);
	second = venture_database_find(fixture->database, second_page, NULL);

	g_assert_cmpuint(first->len, ==, 2);
	g_assert_cmpuint(second->len, ==, 2);

	/* Without a deterministic default order the two pages could overlap. */
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(first, 0)), !=,
	                venture_entity_get_id(g_ptr_array_index(second, 0)));
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(first, 1)), !=,
	                venture_entity_get_id(g_ptr_array_index(second, 0)));
}

static void
test_database_query_organization_scope(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) scoped = NULL;
	g_autoptr(GPtrArray) results = NULL;
	g_autoptr(VentureVenture) mine = NULL;
	g_autoptr(VentureVenture) theirs = NULL;

	mine = create_venture(fixture, "mine");

	theirs = venture_venture_new();
	g_object_set(theirs, "name", "theirs", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(theirs), 9999);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(theirs), NULL, NULL));

	scoped = venture_query_new(VENTURE_TYPE_VENTURE);
	venture_query_set_organization(scoped, fixture->organization_id);

	/* Organisation scoping is what keeps a personal entity's records out
	 * of a business's reports. */
	results = venture_database_find(fixture->database, scoped, NULL);
	g_assert_cmpuint(results->len, ==, 1);
}

static void
test_database_query_date_range_is_half_open(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureDateRange) january = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) results = NULL;
	g_autoptr(GTimeZone) utc = NULL;
	guint i;

	venture = create_venture(fixture, "books");
	utc = g_time_zone_new_utc();

	/* One sale on the last instant of January and one at the start of
	 * February. A half-open month must claim exactly the first. */
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureSale) sale = NULL;
		g_autoptr(GDateTime) when = NULL;

		when = (0 == i)
			? g_date_time_new(utc, 2026, 1, 31, 23, 59, 59.0)
			: g_date_time_new(utc, 2026, 2, 1, 0, 0, 0.0);

		sale = venture_sale_new();
		g_object_set(sale,
		             "venture-id",
		             venture_entity_get_id(VENTURE_ENTITY(venture)),
		             "occurred-at", when,
		             NULL);
		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(sale), NULL, NULL));
	}

	january = venture_date_range_new_month(2026, 1, utc);
	query = venture_query_new(VENTURE_TYPE_SALE);
	g_assert_true(venture_query_set_date_range(query, "occurred-at", january,
	                                           NULL));

	results = venture_database_find(fixture->database, query, NULL);
	g_assert_cmpuint(results->len, ==, 1);
}

static void
test_database_query_from_json(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GPtrArray) results = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) active = NULL;
	g_autoptr(VentureVenture) paused = NULL;

	active = create_venture(fixture, "active-one");
	paused = create_venture(fixture, "paused-one");
	g_object_set(paused, "status", VENTURE_VENTURE_STATUS_PAUSED, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(paused), NULL, NULL));

	node = venture_json_parse(
		"{\"filters\": [{\"field\": \"status\", \"op\": \"eq\","
		" \"value\": \"paused\"}], \"limit\": 10}", &error);
	g_assert_no_error(error);

	query = venture_query_new(VENTURE_TYPE_VENTURE);
	g_assert_true(venture_query_apply_json(query, node, &error));
	g_assert_no_error(error);

	results = venture_database_find(fixture->database, query, NULL);
	g_assert_cmpuint(results->len, ==, 1);
}

static void
test_database_query_json_rejects_bad_operator(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;

	node = venture_json_parse(
		"{\"filters\": [{\"field\": \"name\", \"op\": \"sqlinject\","
		" \"value\": \"x\"}]}", NULL);

	query = venture_query_new(VENTURE_TYPE_VENTURE);

	g_assert_false(venture_query_apply_json(query, node, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	/* The message lists the valid comparisons, since the AI reads it. */
	g_assert_nonnull(g_strstr_len(error->message, -1, "between"));
}

/* --- Deletion ------------------------------------------------------------ */

static void
test_database_soft_delete_hides_but_keeps(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureQuery) visible = NULL;
	g_autoptr(VentureQuery) including_deleted = NULL;
	g_autoptr(GError) error = NULL;

	venture = create_venture(fixture, "books");

	g_assert_true(venture_database_delete(fixture->database,
	                                      VENTURE_ENTITY(venture), NULL,
	                                      &error));
	g_assert_no_error(error);
	g_assert_true(venture_entity_is_deleted(VENTURE_ENTITY(venture)));

	visible = venture_query_new(VENTURE_TYPE_VENTURE);
	g_assert_cmpint(venture_database_count(fixture->database, visible, NULL),
	                ==, 0);

	/* The row survives, so a report over a past period still
	 * reconstructs and an accidental deletion is recoverable. */
	including_deleted = venture_query_new(VENTURE_TYPE_VENTURE);
	venture_query_set_include_deleted(including_deleted, TRUE);
	g_assert_cmpint(venture_database_count(fixture->database,
	                                       including_deleted, NULL), ==, 1);
}

static void
test_database_restore(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureQuery) visible = NULL;

	venture = create_venture(fixture, "books");

	g_assert_true(venture_database_delete(fixture->database,
	                                      VENTURE_ENTITY(venture), NULL, NULL));
	g_assert_true(venture_database_restore(fixture->database,
	                                       VENTURE_ENTITY(venture), NULL, NULL));

	g_assert_false(venture_entity_is_deleted(VENTURE_ENTITY(venture)));

	visible = venture_query_new(VENTURE_TYPE_VENTURE);
	g_assert_cmpint(venture_database_count(fixture->database, visible, NULL),
	                ==, 1);
}

static void
test_database_purge_removes_row(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureQuery) including_deleted = NULL;

	venture = create_venture(fixture, "books");

	g_assert_true(venture_database_purge(fixture->database,
	                                     VENTURE_ENTITY(venture), NULL, NULL));

	including_deleted = venture_query_new(VENTURE_TYPE_VENTURE);
	venture_query_set_include_deleted(including_deleted, TRUE);
	g_assert_cmpint(venture_database_count(fixture->database,
	                                       including_deleted, NULL), ==, 0);
}

/* --- Auditing ------------------------------------------------------------ */

static void
test_database_writes_audit_entries(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	VentureActor actor;

	actor.kind = VENTURE_ACTOR_KIND_AI;
	actor.name = "claude-sonnet-5";
	actor.prompt = "mark the etsy shop as active";
	actor.request_id = "req-1";

	venture = venture_venture_new();
	g_object_set(venture, "name", "etsy", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(venture), &actor, NULL));

	query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_assert_true(venture_query_add_filter_string(query, "target-type",
		VENTURE_FILTER_OP_EQ, "venture", NULL));

	entries = venture_database_find(fixture->database, query, NULL);
	g_assert_cmpuint(entries->len, ==, 1);

	{
		g_autofree gchar *actor_name = NULL;
		g_autofree gchar *prompt = NULL;
		VentureActorKind kind;
		VentureAuditAction action;

		g_object_get(g_ptr_array_index(entries, 0),
		             "actor", &actor_name,
		             "actor-kind", &kind,
		             "action", &action,
		             "prompt", &prompt,
		             NULL);

		g_assert_cmpstr(actor_name, ==, "claude-sonnet-5");
		g_assert_cmpint(kind, ==, VENTURE_ACTOR_KIND_AI);
		g_assert_cmpint(action, ==, VENTURE_AUDIT_ACTION_CREATE);
		/* The instruction that caused the change is retained, because
		 * "why did this number change" is otherwise unanswerable. */
		g_assert_cmpstr(prompt, ==, "mark the etsy shop as active");
	}
}

static void
test_database_audit_records_diff(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autofree gchar *diff = NULL;

	venture = create_venture(fixture, "books");
	g_object_set(venture, "status", VENTURE_VENTURE_STATUS_PAUSED, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(venture), NULL, NULL));

	query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_assert_true(venture_query_add_filter_string(query, "action",
		VENTURE_FILTER_OP_EQ, "update", NULL));

	entries = venture_database_find(fixture->database, query, NULL);
	g_assert_cmpuint(entries->len, >=, 1);

	g_object_get(g_ptr_array_index(entries, 0), "diff", &diff, NULL);
	g_assert_nonnull(diff);
	g_assert_nonnull(g_strstr_len(diff, -1, "paused"));
}

/* --- Ledger -------------------------------------------------------------- */

static VentureLedgerEntry *
make_ledger_line(
	Fixture			*fixture,
	const gchar		*transaction_id,
	VentureLedgerSide	 side,
	gint64			 minor_units
){
	VentureLedgerEntry *entry;
	g_autoptr(VentureMoney) amount = NULL;

	entry = venture_ledger_entry_new();
	amount = venture_money_new(minor_units, "USD", 2);

	g_object_set(entry,
	             "transaction-id", transaction_id,
	             "account-id", (gint64)1,
	             "side", side,
	             "amount", amount,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(entry),
	                                   fixture->organization_id);

	return entry;
}

static void
test_database_ledger_balanced_transaction(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = NULL;

	entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_ptr_array_add(entries, make_ledger_line(fixture, "txn-1",
		VENTURE_LEDGER_SIDE_DEBIT, 10000));
	g_ptr_array_add(entries, make_ledger_line(fixture, "txn-1",
		VENTURE_LEDGER_SIDE_CREDIT, 10000));

	g_assert_true(venture_database_save_ledger_transaction(fixture->database,
	                                                       entries, NULL,
	                                                       &error));
	g_assert_no_error(error);

	query = venture_query_new(VENTURE_TYPE_LEDGER_ENTRY);
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL),
	                ==, 2);
}

static void
test_database_ledger_rejects_unbalanced(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = NULL;

	entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_ptr_array_add(entries, make_ledger_line(fixture, "txn-2",
		VENTURE_LEDGER_SIDE_DEBIT, 10000));
	g_ptr_array_add(entries, make_ledger_line(fixture, "txn-2",
		VENTURE_LEDGER_SIDE_CREDIT, 9900));

	/*
	 * A half-posted transaction leaves the books wrong in a way nothing
	 * downstream can detect, so the whole set is refused and nothing at
	 * all is written.
	 */
	g_assert_false(venture_database_save_ledger_transaction(fixture->database,
	                                                        entries, NULL,
	                                                        &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_BALANCE);

	query = venture_query_new(VENTURE_TYPE_LEDGER_ENTRY);
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL),
	                ==, 0);
}

/* --- Aggregation --------------------------------------------------------- */

static void
test_database_sum_money(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GError) error = NULL;
	gint64 amounts[3];
	gsize i;

	venture = create_venture(fixture, "books");

	amounts[0] = 1050;
	amounts[1] = 2075;
	amounts[2] = 999;

	for (i = 0; i < G_N_ELEMENTS(amounts); i++)
	{
		g_autoptr(VentureSale) sale = NULL;
		g_autoptr(VentureMoney) gross = NULL;

		sale = venture_sale_new();
		gross = venture_money_new(amounts[i], "USD", 2);
		g_object_set(sale,
		             "venture-id",
		             venture_entity_get_id(VENTURE_ENTITY(venture)),
		             "gross", gross,
		             NULL);
		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(sale), NULL, NULL));
	}

	query = venture_query_new(VENTURE_TYPE_SALE);
	total = venture_database_sum_money(fixture->database, query, "gross",
	                                   &error);

	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(total), ==, 1050 + 2075 + 999);
	g_assert_cmpstr(venture_money_get_currency(total), ==, "USD");
}

static void
test_database_sum_money_empty_is_zero(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GError) error = NULL;

	query = venture_query_new(VENTURE_TYPE_SALE);
	total = venture_database_sum_money(fixture->database, query, "gross",
	                                   &error);

	/* A month with no sales reports 0.00; it does not fail. */
	g_assert_no_error(error);
	g_assert_nonnull(total);
	g_assert_true(venture_money_is_zero(total));
}

/* --- Transactions -------------------------------------------------------- */

static void
test_database_transaction_rollback(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureVenture) venture = NULL;

	g_assert_true(venture_database_begin(fixture->database, NULL));

	venture = venture_venture_new();
	g_object_set(venture, "name", "doomed", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(venture), NULL, NULL));

	venture_database_rollback(fixture->database);

	query = venture_query_new(VENTURE_TYPE_VENTURE);
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL),
	                ==, 0);
}

static void
test_database_transaction_commit(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_database_begin(fixture->database, NULL));

	venture = venture_venture_new();
	g_object_set(venture, "name", "kept", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(venture), NULL, NULL));

	g_assert_true(venture_database_commit(fixture->database, &error));
	g_assert_no_error(error);

	query = venture_query_new(VENTURE_TYPE_VENTURE);
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL),
	                ==, 1);
}

/* --- Schema evolution ---------------------------------------------------- */

static void
test_database_adds_missing_columns(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GHashTable) columns = NULL;
	g_autoptr(GError) error = NULL;

	/* Simulate a table created before a field existed. */
	g_assert_true(venture_database_execute(fixture->database,
		"ALTER TABLE ventures DROP COLUMN \"color\"", NULL, &error));
	g_assert_no_error(error);

	g_assert_true(venture_schema_create_table(
		venture_database_get_connection(fixture->database),
		VENTURE_TYPE_VENTURE, &error));
	g_assert_no_error(error);

	/* Adding a field must be a non-event: the column appears on next
	 * startup and nothing has to be migrated by hand. */
	columns = venture_schema_get_existing_columns(
		venture_database_get_connection(fixture->database), "ventures",
		&error);
	g_assert_true(g_hash_table_contains(columns, "color"));
}

/*
 * The database password comes from the environment, never from the
 * configuration file -- that is what database.password_env is for. These
 * check the URI that gets built, because that is the thing handed to the
 * driver; the error message deliberately shows a redacted one.
 */
static void
test_database_uri_takes_password_from_environment(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autofree gchar *uri = NULL;

	config = venture_config_new();
	g_object_set(config,
	             "database-uri", "postgres://venture@db.internal:5432/venture",
	             "database-password-env", "VENTURE_TEST_DB_PASSWORD",
	             NULL);

	g_setenv("VENTURE_TEST_DB_PASSWORD", "s3cr3t", TRUE);

	uri = venture_database_build_uri(config);

	g_assert_cmpstr(uri, ==,
	                "postgres://venture:s3cr3t@db.internal:5432/venture");

	g_unsetenv("VENTURE_TEST_DB_PASSWORD");
}

static void
test_database_uri_keeps_an_explicit_password(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autofree gchar *uri = NULL;

	config = venture_config_new();
	g_object_set(config,
	             "database-uri", "postgres://venture:spelled-out@db/venture",
	             "database-password-env", "VENTURE_TEST_DB_PASSWORD",
	             NULL);

	g_setenv("VENTURE_TEST_DB_PASSWORD", "from-the-environment", TRUE);

	uri = venture_database_build_uri(config);

	/* Someone who spelled a password out is not overridden by a stray
	 * variable in their environment. */
	g_assert_cmpstr(uri, ==, "postgres://venture:spelled-out@db/venture");

	g_unsetenv("VENTURE_TEST_DB_PASSWORD");
}

static void
test_database_uri_escapes_the_password(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autofree gchar *uri = NULL;

	config = venture_config_new();
	g_object_set(config,
	             "database-uri", "postgres://venture@db/venture",
	             "database-password-env", "VENTURE_TEST_DB_PASSWORD",
	             NULL);

	/* `openssl rand -base64` produces these routinely, and all three are
	 * structural in a URI. */
	g_setenv("VENTURE_TEST_DB_PASSWORD", "a/b+c@d", TRUE);

	uri = venture_database_build_uri(config);

	g_assert_cmpstr(uri, ==, "postgres://venture:a%2Fb%2Bc%40d@db/venture");

	g_unsetenv("VENTURE_TEST_DB_PASSWORD");
}

static void
test_database_uri_needs_a_user_to_attach_a_password_to(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autofree gchar *uri = NULL;

	config = venture_config_new();
	g_object_set(config, "database-uri", "postgres://db/venture",
	             "database-password-env", "VENTURE_TEST_DB_PASSWORD", NULL);

	g_setenv("VENTURE_TEST_DB_PASSWORD", "s3cr3t", TRUE);

	/* With no username there is nobody to authenticate as, and inventing
	 * one would connect as somebody unexpected. */
	uri = venture_database_build_uri(config);
	g_assert_cmpstr(uri, ==, "postgres://db/venture");

	g_unsetenv("VENTURE_TEST_DB_PASSWORD");
}

static void
test_database_sqlite_ignores_the_password(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();
	g_object_set(config, "database-uri", "sqlite://:memory:",
	             "database-password-env", "VENTURE_TEST_DB_PASSWORD", NULL);

	g_setenv("VENTURE_TEST_DB_PASSWORD", "irrelevant", TRUE);

	/* SQLite has no authority to rewrite, and mangling the path would
	 * turn a working default into a broken one. */
	database = venture_database_new_for_config(config, &error);

	g_assert_no_error(error);
	g_assert_nonnull(database);

	g_unsetenv("VENTURE_TEST_DB_PASSWORD");
}

static void
test_database_error_message_hides_the_password(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();
	g_object_set(config,
	             "database-uri", "postgres://venture@127.0.0.1:1/venture",
	             "database-password-env", "VENTURE_TEST_DB_PASSWORD",
	             NULL);

	g_setenv("VENTURE_TEST_DB_PASSWORD", "sup3rs3cret", TRUE);

	database = venture_database_new_for_config(config, &error);

	/*
	 * A refused connection is the moment the URI gets printed, and the
	 * moment somebody copies it into a bug report. The password must not
	 * travel with it -- but enough of the URI must survive to be worth
	 * reading.
	 */
	g_assert_null(database);
	g_assert_nonnull(error);
	g_assert_null(g_strstr_len(error->message, -1, "sup3rs3cret"));
	g_assert_nonnull(g_strstr_len(error->message, -1, "127.0.0.1:1/venture"));

	g_unsetenv("VENTURE_TEST_DB_PASSWORD");
}


/*
 * Entities nest, and looking at a parent means looking at everything
 * beneath it. Without that, a business holding a sub-entity shows figures
 * that silently exclude part of itself -- which is worse than showing
 * nothing, because it looks like an answer.
 */
static void
test_database_organization_tree_rolls_up_children(void)
{
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	gint64 tree[2];
	gint64 count;

	database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database,
		venture_entity_registry_get_default(), &error));

	/* One venture filed against each of two entities. */
	{
		g_autoptr(VentureVenture) parent_record = NULL;
		g_autoptr(VentureVenture) child_record = NULL;

		parent_record = venture_venture_new();
		g_object_set(parent_record, "name", "Filed against the parent", NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(parent_record), 10);
		g_assert_true(venture_database_save(database,
			VENTURE_ENTITY(parent_record), NULL, &error));

		child_record = venture_venture_new();
		g_object_set(child_record, "name", "Filed against the child", NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(child_record), 11);
		g_assert_true(venture_database_save(database,
			VENTURE_ENTITY(child_record), NULL, &error));
	}

	/* The parent alone sees only its own. */
	query = venture_query_new(VENTURE_TYPE_VENTURE);
	venture_query_set_organization(query, 10);
	g_assert_cmpint(venture_database_count(database, query, &error), ==, 1);

	/* The parent plus its child sees both. */
	{
		g_autoptr(VentureQuery) rolled_up = NULL;

		tree[0] = 10;
		tree[1] = 11;

		rolled_up = venture_query_new(VENTURE_TYPE_VENTURE);
		venture_query_set_organization_tree(rolled_up, tree, 2);

		count = venture_database_count(database, rolled_up, &error);
		g_assert_no_error(error);
		g_assert_cmpint(count, ==, 2);
	}

	/* And an unrelated entity still sees neither, so the widening is
	 * scoped rather than a way of turning the filter off. */
	{
		g_autoptr(VentureQuery) elsewhere = NULL;

		elsewhere = venture_query_new(VENTURE_TYPE_VENTURE);
		venture_query_set_organization(elsewhere, 12);

		g_assert_cmpint(venture_database_count(database, elsewhere, &error),
		                ==, 0);
	}
}


/*
 * Moving a record between entities has to persist.
 *
 * A save short-circuits when the diff is empty, so anything the diff cannot
 * see is a field that silently refuses to change: the call returns success,
 * the audit log records nothing, and the row keeps its old value. That is
 * exactly what happened when the entity identifier was mistaken for
 * bookkeeping and excluded from diffing.
 */
static void
test_database_moving_a_record_between_entities_persists(void)
{
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureEntity) reloaded = NULL;
	g_autoptr(GError) error = NULL;

	database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database,
		venture_entity_registry_get_default(), &error));

	venture = venture_venture_new();
	g_object_set(venture, "name", "Filed against the wrong entity", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), 10);

	g_assert_true(venture_database_save(database, VENTURE_ENTITY(venture),
	                                    NULL, &error));

	venture_entity_set_organization_id(VENTURE_ENTITY(venture), 11);

	g_assert_true(venture_database_save(database, VENTURE_ENTITY(venture),
	                                    NULL, &error));
	g_assert_no_error(error);

	reloaded = venture_database_get(database, VENTURE_TYPE_VENTURE,
	                                venture_entity_get_id(
	                                        VENTURE_ENTITY(venture)),
	                                &error);

	g_assert_nonnull(reloaded);
	g_assert_cmpint(venture_entity_get_organization_id(reloaded), ==, 11);
}

static void
test_database_moving_a_record_between_entities_is_audited(void)
{
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	gboolean found;
	guint i;

	database = venture_database_new("sqlite://:memory:", &error);
	g_assert_true(venture_database_migrate(database,
		venture_entity_registry_get_default(), &error));

	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "someone";
	actor.prompt = NULL;
	actor.request_id = NULL;

	venture = venture_venture_new();
	g_object_set(venture, "name", "Moved", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), 10);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(venture),
	                                    &actor, &error));

	venture_entity_set_organization_id(VENTURE_ENTITY(venture), 11);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(venture),
	                                    &actor, &error));

	query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	venture_query_set_limit(query, 0);
	entries = venture_database_find(database, query, &error);

	g_assert_nonnull(entries);

	/*
	 * Which business a record belongs to is exactly the sort of change
	 * somebody needs to be able to find afterwards -- it moves money
	 * between tax returns.
	 */
	found = FALSE;

	for (i = 0; i < entries->len; i++)
	{
		g_autofree gchar *changes = NULL;

		g_object_get(g_ptr_array_index(entries, i), "diff", &changes, NULL);

		if ((NULL != changes) &&
		    (NULL != g_strstr_len(changes, -1, "organization_id")))
			found = TRUE;
	}

	g_assert_true(found);
}


int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/database/migrate-creates-tables", test_database_migrate_creates_tables);
	ADD("/database/migrate-is-idempotent", test_database_migrate_is_idempotent);
	ADD("/database/seeds-chart-of-accounts", test_database_seeds_chart_of_accounts);
	ADD("/database/seeds-tax-categories-conservatively",
	    test_database_seeds_tax_categories_conservatively);

	ADD("/database/save-and-get", test_database_save_and_get);
	ADD("/database/chat-round-trip", test_database_chat_round_trip);
	ADD("/database/invoice-line-amount", test_database_invoice_line_amount);
	ADD("/database/money-round-trip", test_database_money_round_trip);
	ADD("/database/null-money-stays-null", test_database_null_money_stays_null);
	ADD("/database/timestamp-round-trip", test_database_timestamp_round_trip);
	ADD("/database/get-by-uuid", test_database_get_by_uuid);

	ADD("/database/optimistic-concurrency", test_database_optimistic_concurrency);
	ADD("/database/save-without-changes-is-noop",
	    test_database_save_without_changes_is_noop);
	ADD("/database/validation-blocks-save", test_database_validation_blocks_save);

	ADD("/database/query-filters", test_database_query_filters);
	ADD("/database/query-rejects-unknown-field",
	    test_database_query_rejects_unknown_field);
	ADD("/database/query-value-is-never-interpolated",
	    test_database_query_value_is_never_interpolated);
	ADD("/database/query-in-operator", test_database_query_in_operator);
	ADD("/database/query-search", test_database_query_search);
	ADD("/database/query-paging-is-stable", test_database_query_paging_is_stable);
	ADD("/database/query-organization-scope",
	    test_database_query_organization_scope);
	ADD("/database/query-date-range-is-half-open",
	    test_database_query_date_range_is_half_open);
	ADD("/database/query-from-json", test_database_query_from_json);
	ADD("/database/query-json-rejects-bad-operator",
	    test_database_query_json_rejects_bad_operator);

	ADD("/database/soft-delete-hides-but-keeps",
	    test_database_soft_delete_hides_but_keeps);
	ADD("/database/restore", test_database_restore);
	ADD("/database/purge-removes-row", test_database_purge_removes_row);

	ADD("/database/writes-audit-entries", test_database_writes_audit_entries);
	ADD("/database/audit-records-diff", test_database_audit_records_diff);

	ADD("/database/ledger-balanced-transaction",
	    test_database_ledger_balanced_transaction);
	ADD("/database/ledger-rejects-unbalanced",
	    test_database_ledger_rejects_unbalanced);

	ADD("/database/sum-money", test_database_sum_money);
	ADD("/database/sum-money-empty-is-zero", test_database_sum_money_empty_is_zero);

	ADD("/database/transaction-rollback", test_database_transaction_rollback);
	ADD("/database/transaction-commit", test_database_transaction_commit);

	ADD("/database/adds-missing-columns", test_database_adds_missing_columns);

	g_test_add_func("/database/moving-a-record-between-entities-persists",
	                test_database_moving_a_record_between_entities_persists);
	g_test_add_func("/database/moving-a-record-between-entities-is-audited",
	                test_database_moving_a_record_between_entities_is_audited);
	g_test_add_func("/database/organization-tree-rolls-up-children",
	                test_database_organization_tree_rolls_up_children);
	g_test_add_func("/database/uri-takes-password-from-environment",
	                test_database_uri_takes_password_from_environment);
	g_test_add_func("/database/uri-keeps-an-explicit-password",
	                test_database_uri_keeps_an_explicit_password);
	g_test_add_func("/database/uri-escapes-the-password",
	                test_database_uri_escapes_the_password);
	g_test_add_func("/database/uri-needs-a-user-to-attach-a-password-to",
	                test_database_uri_needs_a_user_to_attach_a_password_to);
	g_test_add_func("/database/sqlite-ignores-the-password",
	                test_database_sqlite_ignores_the_password);
	g_test_add_func("/database/error-message-hides-the-password",
	                test_database_error_message_hides_the_password);

#undef ADD

	return g_test_run();
}
