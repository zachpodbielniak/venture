/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"

/* These records must reach every generated CRUD surface. */
static void
test_records(void)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	GType competitor = venture_entity_registry_lookup(registry, "competitor");
	GType link = venture_entity_registry_lookup(registry, "deal_competitor");
	g_autoptr(VentureDeal) deal = venture_deal_new();
	g_assert_cmpuint(competitor, !=, G_TYPE_INVALID);
	g_assert_cmpuint(link, !=, G_TYPE_INVALID);
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(deal), "lost-to-competitor-id"));
}

static VentureEntity *
record(VentureDatabase *db, const gchar *type, const gchar *name)
{
	GType t = venture_entity_registry_lookup(venture_entity_registry_get_default(), type);
	VentureEntity *row;
	g_autoptr(GError) error = NULL;
	g_assert_cmpuint(t, !=, G_TYPE_INVALID);
	row = g_object_new(t, "organization-id", (gint64)1, "name", name, NULL);
	if (g_str_equal(type, "loss_reason"))
		g_object_set(row, "active", TRUE, NULL);
	g_assert_true(venture_database_save(db, row, NULL, &error));
	g_assert_no_error(error);
	return row;
}

/* A win against two rivals counts against both; losses credit only the
 * named winner. An unused competitor remains visible with zero totals. */
static void
test_report(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureContext) context = NULL;
	g_autofree gchar *state = g_dir_make_tmp("venture-competitors-XXXXXX", NULL);
	g_autoptr(VentureEntity) a = NULL, b = NULL, idle = NULL, reason = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	VentureReport *report;
	guint i, j;
	g_assert_no_error(error);
	g_object_set(config, "state-dir", state, NULL);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	report = venture_report_registry_lookup(venture_context_get_report_registry(context), "competitor_win_loss");
	g_assert_nonnull(report);
	a = record(db, "competitor", "Example Alpha");
	b = record(db, "competitor", "Example Beta");
	idle = record(db, "competitor", "Example Idle");
	reason = record(db, "loss_reason", "Fit");
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureEntity) d = g_object_new(VENTURE_TYPE_DEAL,
			"organization-id", (gint64)1, "name", "Synthetic opportunity",
			"stage", i == 0 ? VENTURE_DEAL_STAGE_WON : VENTURE_DEAL_STAGE_LOST,
			"loss-reason-id", i == 0 ? (gint64)0 : venture_entity_get_id(reason),
			"lost-to-competitor-id", i == 0 ? (gint64)0 : venture_entity_get_id(a), NULL);
		g_assert_true(venture_entity_set_field_from_string(d, "value", "100.00 USD", &error));
		g_assert_true(venture_database_save(db, d, NULL, &error));
		g_assert_no_error(error);
		for (j = 0; j < 2; j++)
		{
			GType t = venture_entity_registry_lookup(venture_entity_registry_get_default(), "deal_competitor");
			g_autoptr(VentureEntity) link = g_object_new(t, "organization-id", (gint64)1,
				"deal-id", venture_entity_get_id(d), "competitor-id", venture_entity_get_id(j == 0 ? a : b), NULL);
			g_assert_true(venture_database_save(db, link, NULL, &error));
			g_assert_no_error(error);
		}
	}
	result = venture_report_generate(report, context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);
	for (i = 0; i < 3; i++)
	{
		const gchar *name = g_value_get_string(venture_report_result_get_cell(result, i, "competitor"));
		gboolean unused = g_str_equal(name, "Example Idle");
		gboolean alpha = g_str_equal(name, "Example Alpha");
		g_assert_cmpfloat(g_value_get_double(venture_report_result_get_cell(result, i, "faced")), ==, unused ? 0 : 2);
		g_assert_cmpfloat(g_value_get_double(venture_report_result_get_cell(result, i, "won")), ==, unused ? 0 : 1);
		g_assert_cmpfloat(g_value_get_double(venture_report_result_get_cell(result, i, "lost")), ==, alpha ? 1 : 0);
		g_assert_cmpfloat(g_value_get_double(venture_report_result_get_cell(result, i, "win_rate")), ==, unused ? 0 : (alpha ? 50 : 100));
		g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(venture_report_result_get_cell(result, i, "value_lost"))), ==, alpha ? 10000 : 0);
	}
	/* A named winner without a separate link still counts, and a second
	 * currency must get its own row rather than being converted or added. */
	{
		g_autoptr(VentureEntity) d = g_object_new(VENTURE_TYPE_DEAL,
			"organization-id", (gint64)1, "name", "Second currency loss",
			"stage", VENTURE_DEAL_STAGE_LOST,
			"loss-reason-id", venture_entity_get_id(reason),
			"lost-to-competitor-id", venture_entity_get_id(a), NULL);
		gboolean found = FALSE;
		g_assert_true(venture_entity_set_field_from_string(d, "value", "25.00 EUR", &error));
		g_assert_true(venture_database_save(db, d, NULL, &error));
		g_assert_no_error(error);
		g_clear_object(&result);
		result = venture_report_generate(report, context, NULL, NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 4);
		for (i = 0; i < 4; i++)
		{
			const gchar *currency = g_value_get_string(venture_report_result_get_cell(result, i, "currency"));
			if (g_str_equal(currency, "EUR"))
			{
				found = TRUE;
				g_assert_cmpfloat(g_value_get_double(venture_report_result_get_cell(result, i, "lost")), ==, 1);
				g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(venture_report_result_get_cell(result, i, "value_lost"))), ==, 2500);
			}
		}
		g_assert_true(found);
	}
	/* A period after the closes leaves all competitors at zero. */
	{
		g_autoptr(GDateTime) start = g_date_time_new_from_iso8601("2099-01-01T00:00:00Z", NULL);
		g_autoptr(GDateTime) end = g_date_time_add_days(start, 1);
		g_autoptr(VentureDateRange) period = venture_date_range_new(start, end);
		g_clear_object(&result);
		result = venture_report_generate(report, context, period, NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);
		for (i = 0; i < 3; i++)
			g_assert_cmpfloat(g_value_get_double(venture_report_result_get_cell(result, i, "faced")), ==, 0);
	}
	g_clear_object(&context);
	venture_test_remove_tree(state);
}

/* Duplicate links would inflate counts, and another organization's rival
 * must not be attached by a direct database writer. */
static void
test_link_validation(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureContext) context = NULL;
	g_autofree gchar *state = g_dir_make_tmp("venture-competitors-XXXXXX", NULL);
	g_autoptr(VentureEntity) rival = NULL, deal = NULL, first = NULL, duplicate = NULL;
	GType t;
	g_assert_no_error(error);
	g_object_set(config, "state-dir", state, NULL);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	rival = record(db, "competitor", "Example Rival");
	deal = record(db, "deal", "Example Deal");
	t = venture_entity_registry_lookup(venture_entity_registry_get_default(), "deal_competitor");
	g_assert_cmpuint(t, !=, G_TYPE_INVALID);
	first = g_object_new(t, "organization-id", (gint64)1, "deal-id", venture_entity_get_id(deal),
		"competitor-id", venture_entity_get_id(rival), NULL);
	duplicate = g_object_new(t, "organization-id", (gint64)1, "deal-id", venture_entity_get_id(deal),
		"competitor-id", venture_entity_get_id(rival), NULL);
	g_assert_true(venture_database_save(db, first, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_database_save(db, duplicate, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(duplicate, "organization-id", (gint64)2, NULL);
	g_assert_false(venture_database_save(db, duplicate, NULL, &error));
	g_assert_nonnull(error);
	g_clear_object(&context);
	venture_test_remove_tree(state);
}

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gchar *url;
} Fixture;

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	f->state_dir = g_dir_make_tmp("venture-deal-contacts-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
}

static void
start_http(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->url = g_strdup_printf("http://127.0.0.1:%u", venture_web_server_get_port(f->server));
	f->session = soup_session_new();
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	if (NULL != f->server)
		venture_web_server_stop(f->server);
	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->state_dir);
	g_free(f->state_dir);
	g_free(f->url);
}

typedef struct { gboolean done; GBytes *body; GError *error; } Reply;

static void
reply_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static guint
http(Fixture *f, const gchar *method, const gchar *path, const gchar *body, gchar **text)
{
	g_autofree gchar *url = g_strconcat(f->url, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Reply reply = { FALSE, NULL, NULL };
	guint status;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, reply_done, &reply);
	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	*text = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));
	status = soup_message_get_status(message);
	g_bytes_unref(reply.body);
	return status;
}


/* The registered report is served by the ordinary report routes. */
static void
test_http(Fixture *f, gconstpointer unused)
{
	gchar *text = NULL;
	(void)unused;
	start_http(f);
	g_assert_cmpuint(http(f, "GET", "/api/v1/reports/competitor_win_loss", NULL, &text), ==, 200);
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(http(f, "GET", "/reports/competitor_win_loss", NULL, &text), ==, 200);
	g_clear_pointer(&text, g_free);
}

/* Reconcile a populated pre-feature shape, then reopen it twice: no loss
 * attribution is invented and existing customer/deal data survives. */
static void
test_upgrade(void)
{
	g_autofree gchar *state = g_dir_make_tmp("venture-competitor-upgrade-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/data.db", state);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) old = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	guint i;
	g_object_set(config, "state-dir", state, NULL);
	db = venture_database_new(uri, &error);
	g_assert_no_error(error);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	old = record(db, "deal", "Retained opportunity");
	id = venture_entity_get_id(old);
	g_assert_true(venture_database_execute(db,
		"DROP TABLE deal_competitors; DROP TABLE competitors;"
		"DROP INDEX idx_deals_lost_to_competitor_id;"
		"ALTER TABLE deals DROP COLUMN lost_to_competitor_id;"
		"DELETE FROM schema_migrations WHERE version >= 958", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&old);
	g_clear_object(&context);
	g_clear_object(&db);
	for (i = 0; i < 2; i++)
	{
		g_autofree gchar *name = NULL;
		gint64 winner = -1;
		db = venture_database_new(uri, &error);
		g_assert_no_error(error);
		context = venture_context_new(config, db);
		g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		old = venture_database_get(db, VENTURE_TYPE_DEAL, id, &error);
		g_assert_no_error(error);
		g_assert_nonnull(old);
		g_object_get(old, "name", &name, "lost-to-competitor-id", &winner, NULL);
		g_assert_cmpstr(name, ==, "Retained opportunity");
		g_assert_cmpint(winner, ==, 0);
		g_clear_object(&old);
		g_clear_object(&context);
		g_clear_object(&db);
	}
	venture_test_remove_tree(state);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/competitors/records", test_records);
	g_test_add_func("/competitors/report", test_report);
	g_test_add_func("/competitors/link-validation", test_link_validation);
	g_test_add("/competitors/http", Fixture, NULL, setup, test_http, teardown);
	g_test_add_func("/competitors/upgrade", test_upgrade);
	return g_test_run();
}
