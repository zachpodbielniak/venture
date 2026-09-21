/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-accounting.h"

typedef struct {
	GObject parent_instance;
	GThread *owner;
	VentureLogMailer *transport;
	guint prepared;
	gboolean refuse;
} Selection;
typedef GObjectClass SelectionClass;
static void selection_iface(VentureMailerInterface *iface);
GType selection_get_type(void);
G_DEFINE_TYPE_WITH_CODE(Selection, selection, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_MAILER, selection_iface))

static VentureMailer *
prepare(VentureMailer *mailer, VentureMailMessage *message, GError **error)
{
	Selection *self = (Selection *)mailer;
	(void)message;
	g_assert_true(g_thread_self() == self->owner);
	self->prepared++;
	if (self->refuse)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Organization mail is not configured");
		return NULL;
	}
	return g_object_ref(VENTURE_MAILER(self->transport));
}
static void selection_iface(VentureMailerInterface *iface) { iface->prepare = prepare; }
static void selection_finalize(GObject *object)
{
	g_clear_object(&((Selection *)object)->transport);
	G_OBJECT_CLASS(selection_parent_class)->finalize(object);
}
static void selection_class_init(SelectionClass *klass) { klass->finalize = selection_finalize; }
static void selection_init(Selection *self)
{
	self->owner = g_thread_self();
	self->transport = venture_log_mailer_new();
}

typedef struct {
	gboolean done;
	gboolean result;
	GError *error;
} Completion;
static void complete(GObject *source, GAsyncResult *result, gpointer data)
{
	Completion *completion = data;
	completion->result = venture_mailer_send_finish(VENTURE_MAILER(source), result, &completion->error);
	completion->done = TRUE;
}

/* Resolving a credential binding is database work: it must finish on the
 * calling thread before a transport worker is launched. */
static void test_async_prepare(void)
{
	Selection *selection = g_object_new(selection_get_type(), NULL);
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	Completion completion;
	guint attempt;
	for (attempt = 0; attempt < 3; attempt++)
	{
		gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
		completion.done = FALSE;
		completion.result = FALSE;
		completion.error = NULL;
		selection->refuse = attempt == 1;
		if (attempt == 2) g_cancellable_cancel(cancellable);
		venture_mailer_send_async(VENTURE_MAILER(selection), message, cancellable, complete, &completion);
		g_assert_cmpuint(selection->prepared, ==, attempt == 0 ? 1 : 2);
		while (!completion.done && g_get_monotonic_time() < deadline)
		{
			g_main_context_iteration(NULL, FALSE);
			g_usleep(1000);
		}
		g_assert_true(completion.done);
		if (attempt == 0)
		{
			g_assert_no_error(completion.error);
			g_assert_true(completion.result);
		}
		else
		{
			g_assert_false(completion.result);
			g_assert_nonnull(completion.error);
			g_clear_error(&completion.error);
		}
		g_assert_cmpuint(venture_log_mailer_get_messages(selection->transport)->len, ==, 1);
	}
	g_object_unref(selection);
}

static void test_literal_smtp_settings(void)
{
	g_autoptr(JsonObject) values = json_object_new();
	g_autoptr(VentureSmtpMailer) mailer = NULL;
	g_autoptr(GError) error = NULL;
	json_object_set_string_member(values, "host", "smtp.example.invalid");
	json_object_set_string_member(values, "username", "organization");
	json_object_set_string_member(values, "password", "env:NOT_A_PLATFORM_SECRET_LOOKUP");
	json_object_set_string_member(values, "from", "billing@example.invalid");
	json_object_set_int_member(values, "retries", 0);
	mailer = venture_smtp_mailer_new_from_values(values, &error);
	g_assert_no_error(error);
	g_assert_nonnull(mailer);
	g_clear_object(&mailer);
	json_object_set_string_member(values, "tls", "none");
	mailer = venture_smtp_mailer_new_from_values(values, &error);
	g_assert_null(mailer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	json_object_set_string_member(values, "tls", "starttls");
	json_object_remove_member(values, "from");
	g_assert_null(venture_smtp_mailer_new_from_values(values, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

/* An installation relay must never become an organization's implicit account. */
static void test_organization_selection(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureOrganization) organization = venture_organization_new();
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureOrganizationMailer) selector = NULL;
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(VentureMailer) transport = NULL;
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureIntegrationConnection) second = NULL, rotated = NULL, replacement = NULL;
	g_autoptr(JsonNode) status = NULL;
	g_autoptr(JsonObject) values = json_object_new();
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	gint64 org, other_org, connection_id = 0, version = 0;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_object_set(organization, "name", "Mail owner", NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(organization), NULL, &error));
	org = venture_entity_get_id(VENTURE_ENTITY(organization));
	g_object_set(other, "name", "Second mail owner", NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(other), NULL, &error));
	other_org = venture_entity_get_id(VENTURE_ENTITY(other));
	g_object_set(config, "mail-host", "ambient.example.invalid", NULL);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(db), key, &error));
	selector = venture_organization_mailer_new(db, config);
	status = venture_organization_mailer_status(selector, org, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(status), "state"), ==, "unconfigured");
	g_clear_pointer(&status, json_node_unref);
	venture_entity_set_organization_id(VENTURE_ENTITY(message), org);
	transport = venture_mailer_prepare(VENTURE_MAILER(selector), message, &error);
	g_assert_null(transport);
	g_assert_nonnull(error);
	g_clear_error(&error);
	json_object_set_string_member(values, "host", "smtp.example.invalid");
	json_object_set_string_member(values, "from", "billing@example.invalid");
	json_object_set_string_member(values, "username", "organization");
	json_object_set_string_member(values, "password", "secret-A");
	json_object_set_int_member(values, "retries", 0);
	connection = venture_organization_mailer_configure(selector, org, values, 0, 0, NULL, &error);
	g_assert_null(connection);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	g_object_set(config, "mail-allowed-endpoints", "smtp.example.invalid:587", NULL);
	connection = venture_organization_mailer_configure(selector, org, values, 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(connection);
	status = venture_organization_mailer_status(selector, org, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(status), "state"), ==, "unverified");
	g_clear_pointer(&status, json_node_unref);
	json_object_set_string_member(values, "port", "465");
	g_assert_null(venture_organization_mailer_configure(selector, org, values, 0, 0, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	json_object_remove_member(values, "port");
	json_object_set_string_member(values, "tls-ca-file", "/private/operator.pem");
	g_assert_null(venture_organization_mailer_configure(selector, org, values, 0, 0, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	json_object_remove_member(values, "tls-ca-file");
	transport = venture_mailer_prepare(VENTURE_MAILER(selector), message, &error);
	g_assert_no_error(error);
	g_assert_nonnull(transport);
	venture_mailer_get_binding(transport, &connection_id, &version);
	g_assert_cmpint(connection_id, ==, venture_entity_get_id(VENTURE_ENTITY(connection)));
	g_assert_cmpint(version, ==, venture_entity_get_version(VENTURE_ENTITY(connection)));
	json_object_set_string_member(values, "password", "secret-B");
	second = venture_organization_mailer_configure(selector, other_org, values, 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(second);
	g_clear_object(&transport);
	venture_entity_set_organization_id(VENTURE_ENTITY(message), other_org);
	transport = venture_mailer_prepare(VENTURE_MAILER(selector), message, &error);
	g_assert_no_error(error);
	venture_mailer_get_binding(transport, &connection_id, &version);
	g_assert_cmpint(connection_id, ==, venture_entity_get_id(VENTURE_ENTITY(second)));
	g_clear_object(&transport);
	/* A saved message cannot be rebound to a different organization's key. */
	g_object_set(message, "connection-id", connection_id, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(message), org);
	g_assert_null(venture_mailer_prepare(VENTURE_MAILER(selector), message, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	g_object_set(message, "connection-id", venture_entity_get_id(VENTURE_ENTITY(connection)), NULL);
	json_object_set_string_member(values, "password", "rotated-A");
	rotated = venture_organization_mailer_configure(selector, org, values,
		venture_entity_get_version(VENTURE_ENTITY(connection)), venture_entity_get_id(VENTURE_ENTITY(connection)), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rotated);
	transport = venture_mailer_prepare(VENTURE_MAILER(selector), message, &error);
	g_assert_no_error(error);
	venture_mailer_get_binding(transport, &connection_id, &version);
	g_assert_cmpint(connection_id, ==, venture_entity_get_id(VENTURE_ENTITY(connection)));
	g_assert_cmpint(version, ==, venture_entity_get_version(VENTURE_ENTITY(rotated)));
	g_clear_object(&transport);
	g_object_set(config, "mail-allowed-endpoints", "", NULL);
	status = venture_organization_mailer_status(selector, org, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(status), "state"), ==, "unavailable");
	g_clear_pointer(&status, json_node_unref);
	g_assert_null(venture_mailer_prepare(VENTURE_MAILER(selector), message, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	g_object_set(config, "mail-allowed-endpoints", "smtp.example.invalid:587", NULL);
	g_assert_true(venture_integration_service_disable(venture_integration_service_get(db), org,
		venture_entity_get_id(VENTURE_ENTITY(rotated)), venture_entity_get_version(VENTURE_ENTITY(rotated)), NULL, &error));
	transport = venture_mailer_prepare(VENTURE_MAILER(selector), message, &error);
	g_assert_null(transport);
	g_assert_nonnull(error);
	g_clear_error(&error);
	json_object_set_string_member(values, "username", "replacement");
	replacement = venture_organization_mailer_configure(selector, org, values, 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(replacement);
	g_assert_null(venture_organization_mailer_configure(selector, org, values,
		venture_entity_get_version(VENTURE_ENTITY(replacement)), venture_entity_get_id(VENTURE_ENTITY(connection)), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_assert_null(venture_mailer_prepare(VENTURE_MAILER(selector), message, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	/* The other organization remains available through rotation/disconnect. */
	venture_entity_set_organization_id(VENTURE_ENTITY(message), other_org);
	g_object_set(message, "connection-id", venture_entity_get_id(VENTURE_ENTITY(second)), NULL);
	transport = venture_mailer_prepare(VENTURE_MAILER(selector), message, &error);
	g_assert_no_error(error);
	g_assert_nonnull(transport);
	venture_test_accounting_database_cleanup(db);
}
/* A transport's binding is evidence, not a hint to resolve a different key
 * after a crash. This fixture checks storage before its simulated relay runs. */
typedef struct {
	GObject parent_instance;
	VentureDatabase *database;
	VentureLogMailer *log;
	gint64 id, version, message_id;
	gboolean conflict;
} BoundMailer;
typedef GObjectClass BoundMailerClass;
static void bound_mailer_iface(VentureMailerInterface *iface);
GType bound_mailer_get_type(void);
G_DEFINE_TYPE_WITH_CODE(BoundMailer, bound_mailer, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_MAILER, bound_mailer_iface))
static void bound_identity(VentureMailer *mailer, gint64 *id, gint64 *version)
{
	BoundMailer *self = (BoundMailer *)mailer;
	*id = self->id;
	*version = self->version;
	if (self->conflict)
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE mail_messages SET version = version + 1 WHERE id = %" G_GINT64_FORMAT, self->message_id);
		g_assert_true(venture_database_execute(self->database, sql, NULL, NULL));
	}
}
static gboolean bound_send(VentureMailer *mailer, VentureMailMessage *message, GCancellable *cancellable, GError **error)
{
	BoundMailer *self = (BoundMailer *)mailer;
	g_autoptr(VentureEntity) stored = venture_database_get(self->database, VENTURE_TYPE_MAIL_MESSAGE,
		venture_entity_get_id(VENTURE_ENTITY(message)), error);
	gint64 id = 0, version = 0;
	g_assert_nonnull(stored);
	g_object_get(stored, "connection-id", &id, "connection-version", &version, NULL);
	g_assert_cmpint(id, ==, self->id);
	g_assert_cmpint(version, ==, self->version);
	return venture_mailer_send(VENTURE_MAILER(self->log), message, cancellable, error);
}
static void bound_mailer_iface(VentureMailerInterface *iface)
{
	iface->send = bound_send;
	iface->get_binding = bound_identity;
}
static void bound_finalize(GObject *object)
{
	g_clear_object(&((BoundMailer *)object)->log);
	G_OBJECT_CLASS(bound_mailer_parent_class)->finalize(object);
}
static void bound_mailer_class_init(BoundMailerClass *klass) { klass->finalize = bound_finalize; }
static void bound_mailer_init(BoundMailer *self) { self->log = venture_log_mailer_new(); }

static void test_outbox_binding(gconstpointer data)
{
	guint scenario = GPOINTER_TO_UINT(data);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureOrganization) organization = venture_organization_new();
	g_autoptr(VentureMailOutbox) outbox = NULL;
	g_autoptr(VentureMailMessage) message = venture_mail_message_new(), queued = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autofree gchar *state = NULL;
	BoundMailer *mailer = g_object_new(bound_mailer_get_type(), NULL);
	gint64 org, id;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_object_set(organization, "name", "Mail delivery", NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(organization), NULL, &error));
	org = venture_entity_get_id(VENTURE_ENTITY(organization));
	mailer->database = db;
	mailer->id = 100;
	mailer->version = 1;
	outbox = venture_mail_outbox_new(db, VENTURE_MAILER(mailer));
	g_object_set(message, "organization-id", org, "to", "recipient@example.invalid", "subject", "Account evidence", NULL);
	/* Generic writes cannot preselect a provider identity. */
	g_object_set(message, "connection-id", (gint64)100, NULL);
	g_assert_null(venture_mail_outbox_enqueue(outbox, message, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(message, "connection-id", (gint64)0, NULL);
	queued = venture_mail_outbox_enqueue(outbox, message, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(queued);
	id = venture_entity_get_id(VENTURE_ENTITY(queued));
	mailer->message_id = id;
	if (scenario == 2)
	{
		mailer->conflict = TRUE;
		g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, org, 1, now, NULL, &error), ==, -1);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
		g_assert_cmpuint(venture_log_mailer_get_messages(mailer->log)->len, ==, 0);
		stored = venture_database_get(db, VENTURE_TYPE_MAIL_MESSAGE, id, NULL);
		g_object_get(stored, "state", &state, NULL);
		g_assert_cmpstr(state, ==, "sending");
	}
	else
	{
		g_autoptr(GError) transient = g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_MAIL_TRANSIENT, "Relay unavailable before acceptance");
		venture_log_mailer_set_error(mailer->log, transient);
		g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, org, 1, now, NULL, &error), ==, 1);
		g_assert_no_error(error);
		venture_log_mailer_set_error(mailer->log, NULL);
		if (scenario == 0) mailer->version = 2;
		else mailer->id = 200;
		g_assert_true(venture_mail_outbox_retry(outbox, org, id, NULL, &error));
		g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, org, 1, NULL, NULL, &error), ==, 1);
		g_assert_no_error(error);
		g_assert_cmpuint(venture_log_mailer_get_messages(mailer->log)->len, ==, scenario == 0 ? 2 : 1);
		stored = venture_database_get(db, VENTURE_TYPE_MAIL_MESSAGE, id, &error);
		g_object_get(stored, "state", &state, NULL);
		g_assert_cmpstr(state, ==, scenario == 0 ? "sent" : "dead");
	}
	g_object_unref(mailer);
	venture_test_accounting_database_cleanup(db);
}
static void test_selected_delivery(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureOrganization) organization = venture_organization_new();
	g_autoptr(VentureLogMailer) transport = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = NULL;
	g_autoptr(VentureMailMessage) first = venture_mail_message_new(), second = venture_mail_message_new();
	g_autoptr(VentureMailMessage) first_row = NULL, second_row = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autofree gchar *state = NULL;
	gint64 org, id;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_object_set(organization, "name", "Test mail", NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(organization), NULL, &error));
	org = venture_entity_get_id(VENTURE_ENTITY(organization));
	outbox = venture_mail_outbox_new(db, VENTURE_MAILER(transport));
	g_object_set(first, "organization-id", org, "to", "customer@example.invalid", "subject", "Existing work", NULL);
	g_object_set(second, "organization-id", org, "to", "admin@example.invalid", "subject", "Connection test", NULL);
	first_row = venture_mail_outbox_enqueue(outbox, first, NULL, &error);
	second_row = venture_mail_outbox_enqueue(outbox, second, NULL, &error);
	g_assert_no_error(error);
	id = venture_entity_get_id(VENTURE_ENTITY(second_row));
	g_assert_cmpint(venture_mail_outbox_deliver_one(outbox, org, id, 0, 0, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(transport)->len, ==, 1);
	stored = venture_database_get(db, VENTURE_TYPE_MAIL_MESSAGE, venture_entity_get_id(VENTURE_ENTITY(first_row)), &error);
	g_object_get(stored, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "queued");
	g_assert_cmpint(venture_mail_outbox_deliver_one(outbox, org, id, 0, 0, NULL, &error), ==, 0);
	g_assert_no_error(error);
	/* A test for a named provider must not succeed against an injected logger. */
	g_assert_cmpint(venture_mail_outbox_deliver_one(outbox, org, venture_entity_get_id(VENTURE_ENTITY(first_row)), 100, 1, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(transport)->len, ==, 1);
	venture_test_accounting_database_cleanup(db);
}
static void test_real_smtp_isolation(void)
{
	const gchar *port_text = g_getenv("VENTURE_TEST_SMTP_PORT");
	const gchar *ca = g_getenv("VENTURE_TEST_SMTP_CA_FILE");
	const gchar *api = g_getenv("VENTURE_TEST_SMTP_API");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureOrganizationMailer) selector = NULL;
	g_autoptr(VentureMailOutbox) outbox = NULL;
	g_autoptr(VentureOrganization) a = venture_organization_new(), b = venture_organization_new();
	g_autoptr(VentureIntegrationConnection) first = NULL, second = NULL, rotated = NULL;
	g_autoptr(JsonObject) values = json_object_new();
	g_autoptr(GBytes) encryption = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) request = NULL;
	g_autoptr(GBytes) response = NULL;
	g_autoptr(JsonNode) listing = NULL;
	g_autofree gchar *endpoint = NULL, *url = NULL, *json = NULL;
	gint64 port, org_a, org_b;
	guint i, count_a = 0, count_b = 0;
	JsonArray *messages;
	if (port_text == NULL)
	{
		g_test_skip("Set VENTURE_TEST_SMTP_PORT, CA_FILE and API for the isolated authenticated TLS relay fixture");
		return;
	}
	g_assert_nonnull(ca);
	g_assert_nonnull(api);
	g_assert_true(g_ascii_string_to_signed(port_text, 10, 1, 65535, &port, &error));
	db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_object_set(a, "name", "SMTP A", NULL);
	g_object_set(b, "name", "SMTP B", NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(a), NULL, &error));
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(b), NULL, &error));
	org_a = venture_entity_get_id(VENTURE_ENTITY(a));
	org_b = venture_entity_get_id(VENTURE_ENTITY(b));
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(db), encryption, &error));
	endpoint = g_strdup_printf("127.0.0.1:%" G_GINT64_FORMAT, port);
	g_object_set(config, "mail-allowed-endpoints", endpoint, "mail-tls-ca-file", ca, NULL);
	selector = venture_organization_mailer_new(db, config);
	outbox = venture_mail_outbox_new(db, VENTURE_MAILER(selector));
	json_object_set_string_member(values, "host", "127.0.0.1");
	json_object_set_int_member(values, "port", port);
	json_object_set_string_member(values, "from", "a@example.test");
	json_object_set_string_member(values, "username", "org-a");
	json_object_set_string_member(values, "password", "alpha-test");
	first = venture_organization_mailer_configure(selector, org_a, values, 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(first);
	json_object_set_string_member(values, "from", "b@example.test");
	json_object_set_string_member(values, "username", "org-b");
	json_object_set_string_member(values, "password", "bravo-test");
	second = venture_organization_mailer_configure(selector, org_b, values, 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(second);
	for (i = 0; i < 4; i++)
	{
		g_autoptr(VentureMailMessage) message = venture_mail_message_new(), queued = NULL;
		g_autoptr(VentureEntity) stored = NULL;
		g_autofree gchar *subject = g_strdup_printf("organization-route-%u", i), *state = NULL;
		gint64 org = (i % 2) == 0 ? org_a : org_b;
		if (i == 2)
		{
			json_object_set_string_member(values, "from", "a@example.test");
			json_object_set_string_member(values, "username", "org-a");
			json_object_set_string_member(values, "password", "incorrect-fixture-password");
			rotated = venture_organization_mailer_configure(selector, org_a, values,
				venture_entity_get_version(VENTURE_ENTITY(first)), venture_entity_get_id(VENTURE_ENTITY(first)), NULL, &error);
			g_assert_no_error(error);
			g_assert_nonnull(rotated);
		}
		g_object_set(message, "organization-id", org, "to", "recipient@example.test", "subject", subject, "text-body", "Synthetic organization isolation check", NULL);
		queued = venture_mail_outbox_enqueue(outbox, message, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(queued);
		g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, org, 1, NULL, NULL, &error), ==, 1);
		g_assert_no_error(error);
		stored = venture_database_get(db, VENTURE_TYPE_MAIL_MESSAGE, venture_entity_get_id(VENTURE_ENTITY(queued)), &error);
		g_object_get(stored, "state", &state, NULL);
		g_test_message("organization %s delivery %u: %s", (i % 2) == 0 ? "A" : "B", i, state);
		g_assert_cmpstr(state, ==, i == 2 ? "dead" : "sent");
		{
			g_autoptr(JsonNode) status = venture_organization_mailer_status(selector, org, &error);
			g_autofree gchar *safe = NULL;
			g_assert_no_error(error);
			g_assert_cmpstr(json_object_get_string_member(json_node_get_object(status), "state"), ==, state);
			safe = json_to_string(status, FALSE);
			g_assert_null(strstr(safe, "alpha-test"));
			g_assert_null(strstr(safe, "bravo-test"));
			g_assert_null(strstr(safe, "incorrect-fixture-password"));
		}
	}
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(api, "/api/v1/messages", NULL);
	request = soup_message_new("GET", url);
	response = soup_session_send_and_read(session, request, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(soup_message_get_status(request), ==, 200);
	json = g_strndup(g_bytes_get_data(response, NULL), g_bytes_get_size(response));
	listing = json_from_string(json, &error);
	g_assert_no_error(error);
	messages = json_object_get_array_member(json_node_get_object(listing), "messages");
	g_assert_cmpuint(json_array_get_length(messages), ==, 3);
	for (i = 0; i < json_array_get_length(messages); i++)
	{
		JsonObject *message = json_array_get_object_element(messages, i);
		JsonObject *from = json_object_get_object_member(message, "From");
		const gchar *address = json_object_get_string_member(from, "Address");
		if (!g_strcmp0(address, "a@example.test")) count_a++;
		if (!g_strcmp0(address, "b@example.test")) count_b++;
	}
	g_assert_cmpuint(count_a, ==, 1);
	g_assert_cmpuint(count_b, ==, 2);
	venture_test_accounting_database_cleanup(db);
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/mail-routing/organization-selection", test_organization_selection);
	g_test_add_func("/mail-routing/prepare-before-worker", test_async_prepare);
	g_test_add_func("/mail-routing/literal-smtp-settings", test_literal_smtp_settings);
	g_test_add_func("/mail-routing/real-tls-isolation", test_real_smtp_isolation);
	g_test_add_func("/mail-routing/selected-delivery", test_selected_delivery);
	g_test_add_data_func("/mail-routing/outbox-rotation", GUINT_TO_POINTER(0), test_outbox_binding);
	g_test_add_data_func("/mail-routing/outbox-replacement", GUINT_TO_POINTER(1), test_outbox_binding);
	g_test_add_data_func("/mail-routing/outbox-persistence-conflict", GUINT_TO_POINTER(2), test_outbox_binding);
	return g_test_run();
}
