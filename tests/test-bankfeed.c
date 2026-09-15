/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Covers src/bankfeed: pluggable feeds, Teller transport, idempotent sync. */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include <gio/gio.h>
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
static void
fake_feed_fetch_async(VentureBankFeed *feed, const gchar *account_id, GDateTime *from,
	GDateTime *to, const gchar *currency, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new(feed, cancellable, callback, user_data);
	g_autoptr(GError) error = NULL;
	GPtrArray *items = fake_feed_fetch(feed, account_id, from, to, currency, &error);
	if (items == NULL)
		g_task_return_error(task, g_steal_pointer(&error));
	else
		g_task_return_pointer(task, items, (GDestroyNotify)g_ptr_array_unref);
}

static void fake_feed_iface(VentureBankFeedInterface *iface)
{
	iface->get_name = fake_feed_name;
	iface->fetch = fake_feed_fetch;
	iface->fetch_async = fake_feed_fetch_async;
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
	g_assert_nonnull(service);
	g_assert_no_error(error);
	g_assert_null(venture_bank_feed_registry_lookup(venture_bankfeed_service_get_registry(service), "teller"));
	g_setenv("VENTURE_BANKFEED_TELLER_KEY", "test-token", TRUE);
}

static void
test_start_without_key(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	g_unsetenv("VENTURE_BANKFEED_TELLER_KEY");
	g_assert_true(venture_context_start_bankfeed(f->context, &error));
	g_assert_no_error(error);
	g_assert_nonnull(venture_context_get_bankfeed_service(f->context));
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
	/* Teller ignores unknown from/to filters; its actual names bound the import. */
	g_assert_nonnull(strstr(transport->url, "start_date="));
	g_assert_nonnull(strstr(transport->url, "end_date="));
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

typedef struct { GObject parent; guint delay_ms; gboolean cancelled; } SlowTransport;
typedef struct { GObjectClass parent; } SlowTransportClass;
GType slow_transport_get_type(void);
static void slow_transport_iface(VentureBankFeedTransportInterface *iface);
G_DEFINE_TYPE_WITH_CODE(SlowTransport, slow_transport, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_BANK_FEED_TRANSPORT, slow_transport_iface))
static gchar *
slow_transport_get(VentureBankFeedTransport *transport, const gchar *url,
	const gchar *authorization, GError **error)
{
	(void)transport; (void)url; (void)authorization; (void)error;
	g_usleep(800 * 1000);
	return g_strdup("[]");
}
typedef struct { GTask *task; } SlowIdle;
static gboolean
slow_fire(gpointer data)
{
	SlowIdle *idle = data;
	if (!g_task_return_error_if_cancelled(idle->task))
		g_task_return_pointer(idle->task, g_strdup("[]"), g_free);
	g_object_unref(idle->task);
	g_free(idle);
	return G_SOURCE_REMOVE;
}
static void
slow_transport_get_async(VentureBankFeedTransport *transport, const gchar *url,
	const gchar *authorization, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task;
	SlowIdle *idle;
	(void)url; (void)authorization;
	task = g_task_new(transport, cancellable, callback, user_data);
	idle = g_new0(SlowIdle, 1);
	idle->task = g_object_ref(task);
	g_timeout_add(((SlowTransport *)transport)->delay_ms, slow_fire, idle);
	g_object_unref(task);
}
static void
slow_transport_iface(VentureBankFeedTransportInterface *iface)
{
	iface->get = slow_transport_get;
	iface->get_async = slow_transport_get_async;
}
static void slow_transport_class_init(SlowTransportClass *klass) { (void)klass; }
static void slow_transport_init(SlowTransport *self) { self->delay_ms = 800; }

typedef struct { gboolean done; gint imported; GError *error; } SyncDone;
typedef struct { gboolean done; guint status; GError *error; gint64 elapsed; gint64 started; } HealthDone;

static void
sync_finished(GObject *source, GAsyncResult *result, gpointer data)
{
	SyncDone *done = data;
	done->imported = venture_bankfeed_service_sync_finish(VENTURE_BANKFEED_SERVICE(source), result, &done->error);
	done->done = TRUE;
}

static void
health_finished(GObject *source, GAsyncResult *result, gpointer data)
{
	HealthDone *done = data;
	g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &done->error);
	(void)bytes;
	done->elapsed = g_get_monotonic_time() - done->started;
	done->done = TRUE;
}

static void
test_async_health_stays_responsive(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureBankFeedService) service = NULL;
	g_autoptr(VentureBankConnection) connection = NULL;
	g_autoptr(GDateTime) from = g_date_time_new_from_iso8601("2026-01-01T00:00:00Z", NULL);
	g_autoptr(GDateTime) to = g_date_time_new_from_iso8601("2026-01-31T00:00:00Z", NULL);
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 5, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	g_autofree gchar *dir = NULL;
	g_autofree gchar *url = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	SlowTransport *transport;
	SyncDone sync = { FALSE, -1, NULL };
	HealthDone health = { FALSE, 0, NULL, 0, 0 };
	gint64 started;
	guint16 port;
	(void)data;
	dir = g_dir_make_tmp("venture-bankfeed-XXXXXX", &error);
	g_assert_no_error(error);
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_true(venture_web_server_start(server, &error));
	transport = g_object_new(slow_transport_get_type(), NULL);
	service = venture_bankfeed_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	connection = link_account(f, "teller", "acc_slow");
	started = g_get_monotonic_time();
	health.started = started;
	venture_bankfeed_service_sync_async(service, venture_entity_get_id(VENTURE_ENTITY(connection)),
		from, to, NULL, cancel, sync_finished, &sync);
	url = g_strdup_printf("%s/api/v1/health", venture_web_server_get_base_url(server));
	message = soup_message_new("GET", url);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, health_finished, &health);
	while (!health.done || !sync.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_true(health.done);
	g_assert_no_error(health.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	g_assert_cmpint(health.elapsed, <, 400 * 1000);
	g_assert_true(sync.done);
	g_assert_no_error(sync.error);
	g_clear_error(&sync.error);
	g_clear_error(&health.error);
	venture_test_remove_tree(dir);
}

static void
test_async_cancel(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureBankFeedService) service = NULL;
	g_autoptr(VentureBankConnection) connection = NULL;
	g_autoptr(GDateTime) from = g_date_time_new_from_iso8601("2026-01-01T00:00:00Z", NULL);
	g_autoptr(GDateTime) to = g_date_time_new_from_iso8601("2026-01-31T00:00:00Z", NULL);
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	SlowTransport *transport;
	SyncDone sync = { FALSE, -1, NULL };
	(void)data;
	transport = g_object_new(slow_transport_get_type(), NULL);
	transport->delay_ms = 1200;
	service = venture_bankfeed_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	connection = link_account(f, "teller", "acc_cancel");
	venture_bankfeed_service_sync_async(service, venture_entity_get_id(VENTURE_ENTITY(connection)),
		from, to, NULL, cancel, sync_finished, &sync);
	g_cancellable_cancel(cancel);
	while (!sync.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_cmpint(sync.imported, ==, -1);
	g_assert_error(sync.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error(&sync.error);
}

static void
test_actor_lifetime(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureBankFeedService) service = NULL;
	g_autoptr(VentureBankConnection) connection = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *prompt = g_strdup("owned-prompt");
	g_autoptr(GObject) transport = g_object_new(slow_transport_get_type(), NULL);
	SyncDone sync = { FALSE, -1, NULL };
	VentureActor actor;
	gboolean found = FALSE;
	guint i;
	(void)data;
	service = venture_bankfeed_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	connection = link_account(f, "teller", "async-lifetime");
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "lifetime";
	actor.prompt = prompt;
	actor.request_id = "lifetime-request";
	actor.approved_by = "lifetime-approver";
	venture_bankfeed_service_sync_async(service, venture_entity_get_id(VENTURE_ENTITY(connection)),
		NULL, NULL, &actor, NULL, sync_finished, &sync);
	memset(prompt, 'x', strlen(prompt));
	while (!sync.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(sync.error);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *stored = NULL;
		g_object_get(g_ptr_array_index(rows, i), "prompt", &stored, NULL);
		g_assert_cmpstr(stored, !=, prompt);
		if (g_strcmp0(stored, "owned-prompt") == 0)
			found = TRUE;
	}
	g_assert_true(found);
}

typedef struct { gchar *redirect; guint leaked; } RedirectFixture;

static void
redirect_handler(SoupServer *server, SoupServerMessage *message, const gchar *path,
	GHashTable *query, gpointer data)
{
	RedirectFixture *fixture = data;
	(void)server;
	(void)query;
	if (g_str_equal(path, "/redirect"))
		soup_server_message_set_redirect(message, 302, fixture->redirect);
	else
	{
		fixture->leaked++;
		soup_server_message_set_status(message, 200, NULL);
	}
}

/* Custom authorization headers must never reach a redirect-selected host. */
static void
test_transport_redirect(void)
{
	g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
	g_autoptr(VentureBankFeedTransport) transport = venture_bank_feed_transport_new_http();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *body = NULL;
	GSList *uris;
	RedirectFixture fixture;
	fixture.leaked = 0;
	g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	uris = soup_server_get_uris(server);
	url = g_strdup_printf("http://127.0.0.1:%d/redirect", g_uri_get_port(uris->data));
	fixture.redirect = g_strdup_printf("http://localhost:%d/leak", g_uri_get_port(uris->data));
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	soup_server_add_handler(server, NULL, redirect_handler, &fixture, NULL);
	body = venture_bank_feed_transport_get(transport, url, "X-Shopify-Access-Token: synthetic-test-token", &error);
	g_assert_null(body);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK);
	g_assert_cmpuint(fixture.leaked, ==, 0);
	g_free(fixture.redirect);
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
	g_test_add("/bankfeed/async-health", Fixture, NULL, setup, test_async_health_stays_responsive, teardown);
	g_test_add("/bankfeed/async-cancel", Fixture, NULL, setup, test_async_cancel, teardown);
	g_test_add("/bankfeed/actor-lifetime", Fixture, NULL, setup, test_actor_lifetime, teardown);
	g_test_add_func("/bankfeed/redirect", test_transport_redirect);
	return g_test_run();
}
