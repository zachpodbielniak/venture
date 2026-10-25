/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A hosted workspace has no automation engine, so no mail_deliver rule runs.
 * Its outbox must still go out on its own, and mail queued before the
 * business has set up sending must wait for it rather than fail. */
#include <venture.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureLogMailer *mailer;
	VentureWebServer *server;
	gchar *directory;
	gint64 business;
} Fixture;

static void
setup(Fixture *f, gconstpointer transport)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) business = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Example Plumbing", "active", TRUE, NULL);
	VentureTenantService *tenant;
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-hosted-outbox-XXXXXX", &error); g_assert_no_error(error);
	g_object_set(f->config, "hosted-enabled", TRUE, "hosted-workspace-id", "5a1b8c3e-2f47-4d6a-9b0e-7c4d2e1f8a90",
		"hosted-origin", "https://example.test", "security-password-iterations", (gint64)2000,
		"state-dir", f->directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)0, NULL);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	tenant = venture_tenant_service_get(f->db);
	g_assert_true(venture_tenant_service_configure(tenant, f->config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(tenant, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(f->db, business, NULL, &error)); g_assert_no_error(error);
	f->business = venture_entity_get_id(business);
	g_assert_true(venture_tenant_service_bootstrap_admin(tenant, f->config, "operator", "test-password", FALSE, "Fixture", &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	/* The fixture's own transport stands in for a business that set up sending. */
	if (transport) {
		f->mailer = venture_log_mailer_new();
		venture_context_set_mailer(f->context, VENTURE_MAILER(f->mailer));
	}
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server); g_clear_object(&f->context); g_clear_object(&f->mailer);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db); g_clear_object(&f->config);
	venture_test_remove_tree(f->directory); g_free(f->directory);
}

static gint64
enqueue(Fixture *f, const gchar *key)
{
	g_autoptr(VentureMailMessage) message = venture_mail_message_new(), queued = NULL;
	g_autoptr(GError) error = NULL;
	venture_entity_set_organization_id(VENTURE_ENTITY(message), f->business);
	g_object_set(message, "to", "customer@example.test", "subject", "We got your request",
		"text-body", "Thanks, we will call you back today.", "idempotency-key", key, NULL);
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(f->db), message, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(queued);
	return venture_entity_get_id(VENTURE_ENTITY(queued));
}

static gchar *
state_of(Fixture *f, gint64 id)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) row = venture_database_get(f->db, VENTURE_TYPE_MAIL_MESSAGE, id, &error);
	gchar *state = NULL;
	g_assert_no_error(error); g_assert_nonnull(row);
	g_object_get(row, "state", &state, NULL);
	return state;
}

/* Nobody calls the deliver API and no rule runs: the server sends it. */
static void
test_delivers_without_caller(Fixture *f, gconstpointer unused)
{
	gint64 id;
	guint tick;
	(void)unused;
	g_assert_null(venture_context_get_automation(f->context));
	id = enqueue(f, "hosted-outbox:form-confirmation");
	for (tick = 0; tick < 150 && venture_log_mailer_get_messages(f->mailer)->len == 0; tick++) {
		while (g_main_context_iteration(NULL, FALSE));
		g_usleep(G_TIME_SPAN_SECOND / 10);
	}
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	{
		g_autofree gchar *state = state_of(f, id);
		g_assert_cmpstr(state, ==, "sent");
	}
}

/* A sweep before the business has a mail account leaves the message queued,
 * so it goes out once sending is set up instead of being given up on. */
static void
test_waits_for_mail_setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL;
	gint64 id;
	(void)unused;
	id = enqueue(f, "hosted-outbox:before-setup");
	g_assert_cmpint(venture_mail_delivery_sweep(f->context, &error), ==, 0);
	g_assert_no_error(error);
	state = state_of(f, id);
	g_assert_cmpstr(state, ==, "queued");
}

/* The sweep reaches every business in the workspace, not only the default one. */
static void
test_sweeps_every_business(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) second = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Example Heating", "active", TRUE, NULL);
	g_autoptr(VentureMailMessage) message = venture_mail_message_new(), queued = NULL;
	(void)unused;
	g_assert_true(venture_database_save(f->db, second, NULL, &error)); g_assert_no_error(error);
	enqueue(f, "hosted-outbox:first-business");
	venture_entity_set_organization_id(VENTURE_ENTITY(message), venture_entity_get_id(second));
	g_object_set(message, "to", "owner@example.test", "subject", "New booking", "text-body", "A booking came in.",
		"idempotency-key", "hosted-outbox:second-business", NULL);
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(f->db), message, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(queued);
	g_assert_cmpint(venture_mail_delivery_sweep(f->context, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 2);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/hosted-outbox/delivers-without-caller", Fixture, "log", setup, test_delivers_without_caller, teardown);
	g_test_add("/hosted-outbox/waits-for-mail-setup", Fixture, NULL, setup, test_waits_for_mail_setup, teardown);
	g_test_add("/hosted-outbox/sweeps-every-business", Fixture, "log", setup, test_sweeps_every_business, teardown);
	return g_test_run();
}
