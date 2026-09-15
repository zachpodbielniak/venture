/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Covers src/bankfeed: pluggable feeds, Teller transport, idempotent sync. */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	gint64 bank_id;
} Fixture;

typedef struct { GObject parent; GPtrArray *items; gchar *name; } FakeFeed;
typedef struct { GObjectClass parent; } FakeFeedClass;
GType fake_feed_get_type(void);
static void fake_feed_iface(VentureBankFeedInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeFeed, fake_feed, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_BANK_FEED, fake_feed_iface))
static const gchar *fake_feed_name(VentureBankFeed *self)
{
	return ((FakeFeed *)self)->name ? ((FakeFeed *)self)->name : "fake";
}
static GPtrArray *
fake_feed_fetch(VentureBankFeed *feed, const gchar *account_id, GDateTime *from,
	GDateTime *to, const gchar *currency, GError **error)
{
	FakeFeed *self = (FakeFeed *)feed;
	GPtrArray *copy = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
	guint i;
	(void)account_id; (void)from; (void)to; (void)currency;
	(void)error;
	for (i = 0; i < self->items->len; i++)
		g_ptr_array_add(copy, json_object_ref(g_ptr_array_index(self->items, i)));
	return copy;
}
static void fake_feed_iface(VentureBankFeedInterface *iface)
{
	iface->get_name = fake_feed_name;
	iface->fetch = fake_feed_fetch;
}
static void fake_feed_finalize(GObject *object)
{
	FakeFeed *self = (FakeFeed *)object;
	g_clear_pointer(&self->items, g_ptr_array_unref);
	g_free(self->name);
	G_OBJECT_CLASS(fake_feed_parent_class)->finalize(object);
}
static void fake_feed_class_init(FakeFeedClass *klass) { G_OBJECT_CLASS(klass)->finalize = fake_feed_finalize; }
static void fake_feed_init(FakeFeed *self)
{
	self->items = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
}

typedef struct { GObject parent; guint calls; gchar *url; gchar *authorization; gchar *body; } FakeTransport;
typedef struct { GObjectClass parent; } FakeTransportClass;
GType fake_transport_get_type(void);
static void fake_transport_iface(VentureBankFeedTransportInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeTransport, fake_transport, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_BANK_FEED_TRANSPORT, fake_transport_iface))
static gchar *
fake_transport_get(VentureBankFeedTransport *transport, const gchar *url,
	const gchar *authorization, GError **error)
{
	FakeTransport *self = (FakeTransport *)transport;
	(void)error;
	self->calls++;
	g_free(self->url);
	g_free(self->authorization);
	self->url = g_strdup(url);
	self->authorization = g_strdup(authorization);
	return g_strdup(self->body ? self->body : "[]");
}
static void fake_transport_iface(VentureBankFeedTransportInterface *iface) { iface->get = fake_transport_get; }
static void fake_transport_finalize(GObject *object)
{
	FakeTransport *self = (FakeTransport *)object;
	g_free(self->url);
	g_free(self->authorization);
	g_free(self->body);
	G_OBJECT_CLASS(fake_transport_parent_class)->finalize(object);
}
static void fake_transport_class_init(FakeTransportClass *klass) { G_OBJECT_CLASS(klass)->finalize = fake_transport_finalize; }
static void fake_transport_init(FakeTransport *self) { (void)self; }

static JsonObject *
txn(const gchar *id, const gchar *date, const gchar *amount, const gchar *description)
{
	JsonObject *object = json_object_new();
	json_object_set_string_member(object, "id", id);
	json_object_set_string_member(object, "date", date);
	json_object_set_string_member(object, "amount", amount);
	json_object_set_string_member(object, "description", description);
	return object;
}

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, record, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(VentureBankAccount) bank = NULL;
	(void)data;
	g_setenv("VENTURE_BANKFEED_TELLER_KEY", "test-token", TRUE);
	f->config = venture_config_new();
	g_object_set(f->config, "bankfeed-enabled", TRUE, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "1000", &error));
	accounts = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(accounts->len, ==, 1);
	bank = venture_bank_account_new();
	g_object_set(bank, "name", "Checking", "account-id",
		venture_entity_get_id(g_ptr_array_index(accounts, 0)), "currency", "USD",
		"date-column", "Date", "amount-column", "Amount", "description-column", "Memo",
		"reference-column", "Ref", "external-id-column", "ID", "date-format", "%Y-%m-%d",
		"sign-convention", "normal", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(bank), f->org);
	save(f, VENTURE_ENTITY(bank));
	f->bank_id = venture_entity_get_id(VENTURE_ENTITY(bank));
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static guint
count_txns(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_organization(query, f->org);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows->len;
}

static void
test_records(void)
{
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"bank_connection"), !=, G_TYPE_INVALID);
}

static void
test_missing_key(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureBankFeedService) service = NULL;

	g_unsetenv("VENTURE_BANKFEED_TELLER_KEY");
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	service = venture_bankfeed_service_new(db, 1, NULL, &error);
	g_assert_null(service);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "VENTURE_BANKFEED_TELLER_KEY"));
	g_setenv("VENTURE_BANKFEED_TELLER_KEY", "test-token", TRUE);
}

static void
test_start_without_key(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	g_unsetenv("VENTURE_BANKFEED_TELLER_KEY");
	g_assert_false(venture_context_start_bankfeed(f->context, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "VENTURE_BANKFEED_TELLER_KEY"));
	g_setenv("VENTURE_BANKFEED_TELLER_KEY", "test-token", TRUE);
}

static VentureBankConnection *
link_account(Fixture *f, const gchar *provider, const gchar *provider_account)
{
	g_autoptr(VentureBankConnection) connection = venture_bank_connection_new();
	g_object_set(connection, "name", "Teller checking", "provider", provider,
		"provider-account-id", provider_account, "bank-account-id", f->bank_id, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(connection), f->org);
	save(f, VENTURE_ENTITY(connection));
	return g_steal_pointer(&connection);
}

static void
test_fake_feed_idempotent_sync(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureBankFeedService) service = NULL;
	g_autoptr(VentureBankConnection) connection = NULL;
	g_autoptr(GDateTime) from = g_date_time_new_from_iso8601("2026-01-01T00:00:00Z", NULL);
	g_autoptr(GDateTime) to = g_date_time_new_from_iso8601("2026-01-31T00:00:00Z", NULL);
	FakeFeed *feed;
	gint imported;
	(void)data;
	service = venture_bankfeed_service_new(f->db, f->org, NULL, &error);
	g_assert_no_error(error);
	feed = g_object_new(fake_feed_get_type(), NULL);
	g_ptr_array_add(feed->items, txn("txn_1", "2026-01-10", "-10.00", "Fee"));
	g_ptr_array_add(feed->items, txn("txn_2", "2026-01-11", "25.00", "Deposit"));
	venture_bank_feed_registry_add(venture_bankfeed_service_get_registry(service), VENTURE_BANK_FEED(feed));
	connection = link_account(f, "fake", "acc_1");
	imported = venture_bankfeed_service_sync(service, venture_entity_get_id(VENTURE_ENTITY(connection)),
		from, to, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(imported, ==, 2);
	g_assert_cmpuint(count_txns(f), ==, 2);
	imported = venture_bankfeed_service_sync(service, venture_entity_get_id(VENTURE_ENTITY(connection)),
		from, to, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(imported, ==, 0);
	g_assert_cmpuint(count_txns(f), ==, 2);
}

static void
test_failure_rolls_back(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureBankFeedService) service = NULL;
	g_autoptr(VentureBankConnection) connection = NULL;
	g_autoptr(GDateTime) from = g_date_time_new_from_iso8601("2026-01-01T00:00:00Z", NULL);
	g_autoptr(GDateTime) to = g_date_time_new_from_iso8601("2026-01-31T00:00:00Z", NULL);
	FakeFeed *feed;
	gint imported;
	(void)data;
	service = venture_bankfeed_service_new(f->db, f->org, NULL, &error);
	g_assert_no_error(error);
	feed = g_object_new(fake_feed_get_type(), NULL);
	g_ptr_array_add(feed->items, txn("txn_1", "2026-01-10", "-10.00", "Fee"));
	g_ptr_array_add(feed->items, txn("txn_bad", "not-a-date", "-1.00", "Bad"));
	venture_bank_feed_registry_add(venture_bankfeed_service_get_registry(service), VENTURE_BANK_FEED(feed));
	connection = link_account(f, "fake", "acc_1");
	imported = venture_bankfeed_service_sync(service, venture_entity_get_id(VENTURE_ENTITY(connection)),
		from, to, NULL, &error);
	g_assert_cmpint(imported, <, 0);
	g_assert_nonnull(error);
	g_assert_cmpuint(count_txns(f), ==, 0);
}

static void
test_teller_uses_transport(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureBankFeedService) service = NULL;
	g_autoptr(VentureBankConnection) connection = NULL;
	g_autoptr(GDateTime) from = g_date_time_new_from_iso8601("2026-01-01T00:00:00Z", NULL);
	g_autoptr(GDateTime) to = g_date_time_new_from_iso8601("2026-01-31T00:00:00Z", NULL);
	FakeTransport *transport;
	gint imported;
	(void)data;
	transport = g_object_new(fake_transport_get_type(), NULL);
	transport->body = g_strdup("[{\"id\":\"txn_t1\",\"date\":\"2026-01-12\",\"amount\":\"-4.00\",\"description\":\"Card\"}]");
	service = venture_bankfeed_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	connection = link_account(f, "teller", "acc_teller");
	imported = venture_bankfeed_service_sync(service, venture_entity_get_id(VENTURE_ENTITY(connection)),
		from, to, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(imported, ==, 1);
	g_assert_cmpuint(transport->calls, ==, 1);
	g_assert_nonnull(strstr(transport->url, "api.teller.io"));
	g_assert_nonnull(strstr(transport->url, "acc_teller"));
	g_assert_nonnull(transport->authorization);
	g_assert_cmpuint(count_txns(f), ==, 1);
	imported = venture_bankfeed_service_sync(service, venture_entity_get_id(VENTURE_ENTITY(connection)),
		from, to, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(imported, ==, 0);
	g_assert_cmpuint(count_txns(f), ==, 1);
}

static void
test_scheduled_sync(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureBankFeedService) service = NULL;
	g_autoptr(VentureBankConnection) connection = NULL;
	FakeFeed *feed;
	gint imported;
	(void)data;
	service = venture_bankfeed_service_new(f->db, f->org, NULL, &error);
	g_assert_no_error(error);
	feed = g_object_new(fake_feed_get_type(), NULL);
	{
		g_autoptr(GDateTime) now = venture_time_now();
		g_autofree gchar *day = g_date_time_format(now, "%Y-%m-%d");
		g_ptr_array_add(feed->items, txn("txn_s", day, "1.00", "Sweep"));
	}
	venture_bank_feed_registry_add(venture_bankfeed_service_get_registry(service), VENTURE_BANK_FEED(feed));
	connection = link_account(f, "fake", "acc_s");
	(void)connection;
	imported = venture_bankfeed_service_sync_due(service, f->org, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(imported, ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/bankfeed/records", test_records);
	g_test_add_func("/bankfeed/missing-key", test_missing_key);
	g_test_add("/bankfeed/start-without-key", Fixture, NULL, setup, test_start_without_key, teardown);
	g_test_add("/bankfeed/fake-idempotent", Fixture, NULL, setup, test_fake_feed_idempotent_sync, teardown);
	g_test_add("/bankfeed/failure-rollback", Fixture, NULL, setup, test_failure_rolls_back, teardown);
	g_test_add("/bankfeed/teller-transport", Fixture, NULL, setup, test_teller_uses_transport, teardown);
	g_test_add("/bankfeed/scheduled-sync", Fixture, NULL, setup, test_scheduled_sync, teardown);
	return g_test_run();
}
