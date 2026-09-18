/*
 * test-report-delivery.c - Scheduled report packs mailed through the outbox.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include <unistd.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureLogMailer *mailer;
	gint64 org;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	f->mailer = venture_log_mailer_new();
	venture_context_set_mailer(f->context, VENTURE_MAILER(f->mailer));
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->mailer);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static gint64
account_id(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(f->db, query, NULL);
	return venture_entity_get_id(row);
}

/* A posting so the tabular report in the pack has a row to put in its CSV. */
static void
post_something(Fixture *f, const gchar *when, gint64 amount)
{
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureJournalLine) debit = venture_journal_line_new();
	g_autoptr(VentureJournalLine) credit = venture_journal_line_new();
	g_autoptr(GDateTime) date = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) money = venture_money_new_for_currency(amount, "USD");
	g_object_set(journal, "source-type", "organization", "source-id", f->org,
		"occurred-at", date, "currency", "USD", "organization-id", f->org, NULL);
	g_object_set(debit, "account-id", account_id(f, "1000"), "side", VENTURE_LEDGER_SIDE_DEBIT,
		"organization-id", f->org, "amount", money, NULL);
	g_object_set(credit, "account-id", account_id(f, "4000"), "side", VENTURE_LEDGER_SIDE_CREDIT,
		"organization-id", f->org, "amount", money, NULL);
	g_ptr_array_add(lines, g_object_ref(debit));
	g_ptr_array_add(lines, g_object_ref(credit));
	g_assert_nonnull(venture_posting_service_post(venture_database_get_posting_service(f->db),
		journal, lines, NULL, NULL, &error));
	g_assert_no_error(error);
}

/* A daily pack of one tabular report, optionally set to mail its output. */
static VentureEntity *
make_pack(Fixture *f, const gchar *name, const gchar *deliver, const gchar *recipients)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) saved = NULL;
	VentureEntity *pack;
	saved = venture_report_pack_service_save(venture_report_pack_service_get(f->db),
		f->org, "Balances", "account_balances", "2026-08", NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	pack = venture_report_pack_service_schedule(venture_report_pack_service_get(f->db),
		f->org, name, "0 8 * * *", venture_entity_get_id(saved), NULL, &error);
	g_assert_no_error(error);
	if (deliver != NULL || recipients != NULL)
	{
		g_object_set(pack, "deliver", deliver, "recipients", recipients, NULL);
		g_assert_true(venture_database_save(f->db, pack, NULL, &error));
		g_assert_no_error(error);
	}
	return pack;
}

static VentureEntity *
pack_row(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_REPORT_PACK);
	g_autoptr(GError) error = NULL;
	VentureEntity *row;
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, name, NULL);
	row = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	return row;
}

static VentureEntity *
reload(Fixture *f, VentureEntity *pack)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *stored = venture_database_get(f->db, VENTURE_TYPE_REPORT_PACK,
		venture_entity_get_id(pack), &error);
	g_assert_no_error(error);
	return stored;
}

static GPtrArray *
pack_messages(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_string(query, "related-type", VENTURE_FILTER_OP_EQ, "report_pack", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}

static gint
sweep(Fixture *f, const gchar *when)
{
	g_autoptr(GDateTime) as_of = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(GError) error = NULL;
	gint ran = venture_report_pack_service_run_due(venture_report_pack_service_get(f->db),
		f->context, f->org, as_of, NULL, &error);
	g_assert_no_error(error);
	return ran;
}

/* DONE WHEN 1: recipients are validated on save; deliver is none or email. */
static void
test_fields(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) pack = make_pack(f, "Plain", NULL, NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *deliver = NULL;
	g_autofree gchar *recipients = NULL;
	(void)data;
	/* The default is none, and it is stored that way. */
	g_object_get(pack, "deliver", &deliver, NULL);
	g_assert_cmpstr(deliver, ==, "none");

	g_object_set(pack, "deliver", "email", "recipients", " ceo@example.test ,cfo@example.test", NULL);
	g_assert_true(venture_database_save(f->db, pack, NULL, &error));
	g_assert_no_error(error);
	g_object_get(pack, "recipients", &recipients, NULL);
	g_assert_cmpstr(recipients, ==, "ceo@example.test,cfo@example.test");

	g_object_set(pack, "deliver", "fax", NULL);
	g_assert_false(venture_database_save(f->db, pack, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	g_object_set(pack, "deliver", "email", "recipients", "ceo@example.test,not-an-address", NULL);
	g_assert_false(venture_database_save(f->db, pack, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "not-an-address"));
	g_clear_error(&error);

	g_object_set(pack, "recipients", "ceo@example.test,", NULL);
	g_assert_false(venture_database_save(f->db, pack, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	g_object_set(pack, "recipients", "ceo@example.test\r\nbcc: x@example.test", NULL);
	g_assert_false(venture_database_save(f->db, pack, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	g_object_set(pack, "recipients", "two@@example.test", NULL);
	g_assert_false(venture_database_save(f->db, pack, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* Mail with nobody to mail is a misconfiguration, not a quiet no-op. */
	g_object_set(pack, "deliver", "email", "recipients", "", NULL);
	g_assert_false(venture_database_save(f->db, pack, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* Addresses may be kept on a pack that is not (yet) mailed. */
	g_object_set(pack, "deliver", "none", "recipients", "ceo@example.test", NULL);
	g_assert_true(venture_database_save(f->db, pack, NULL, &error));
	g_assert_no_error(error);
}

/* DONE WHEN 2 and 3: a due pack set to email is queued through the outbox
 * with its JSON and CSV attached, and the run records the delivery. */
static void
test_sweep_queues_mail(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) pack = make_pack(f, "Morning pack", "email", "ceo@example.test,cfo@example.test");
	g_autoptr(VentureEntity) quiet = make_pack(f, "Quiet pack", "none", "nobody@example.test");
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) delivered_at = NULL;
	g_autofree gchar *to = NULL, *subject = NULL, *body = NULL, *related = NULL, *state = NULL;
	g_autofree gchar *message_id = NULL, *stamped_id = NULL, *failure = NULL, *key = NULL;
	gint64 mail_id = 0;
	const gchar *snapshot;
	VentureEntity *row;
	(void)data;
	post_something(f, "2026-08-01T00:00:00Z", 2500);
	g_assert_cmpint(sweep(f, "2026-08-02T09:00:00Z"), ==, 2);

	rows = pack_messages(f);
	g_assert_cmpuint(rows->len, ==, 1);
	row = g_ptr_array_index(rows, 0);
	g_object_get(row, "to", &to, "subject", &subject, "text-body", &body, "related-type", &related,
		"state", &state, "message-id", &message_id, "idempotency-key", &key, NULL);
	g_assert_cmpstr(to, ==, "ceo@example.test,cfo@example.test");
	g_assert_cmpstr(state, ==, "queued");
	g_assert_cmpstr(related, ==, "report_pack");
	g_assert_nonnull(strstr(subject, "Morning pack"));
	g_assert_nonnull(strstr(subject, "Aug"));
	g_assert_nonnull(strstr(body, "Morning pack"));
	{
		/* The body names each report by its title, whatever its case. */
		g_autofree gchar *folded = g_ascii_strdown(body, -1);
		g_assert_nonnull(strstr(folded, "balances"));
	}
	g_assert_true(g_str_has_prefix(key, "report_pack:"));
	/* The pack's JSON and a CSV per tabular report ride along. */
	snapshot = venture_entity_get_attribute(row, "_mail_attachments");
	g_assert_nonnull(snapshot);
	g_assert_nonnull(strstr(snapshot, "morning-pack.json"));
	g_assert_nonnull(strstr(snapshot, "application/json"));
	g_assert_nonnull(strstr(snapshot, "balances.csv"));
	g_assert_nonnull(strstr(snapshot, "text/csv"));
	{
		g_autoptr(JsonNode) parsed = json_from_string(snapshot, &error);
		JsonArray *files;
		g_autofree gchar *csv = NULL;
		gsize length;
		g_assert_no_error(error);
		files = json_node_get_array(parsed);
		g_assert_cmpuint(json_array_get_length(files), ==, 2);
		csv = (gchar *)g_base64_decode(json_object_get_string_member(json_array_get_object_element(files, 1), "data"), &length);
		g_assert_nonnull(strstr(csv, "1000"));
		g_assert_nonnull(strstr(csv, "25.00"));
	}

	stored = reload(f, pack);
	g_object_get(stored, "last-delivered-at", &delivered_at, "last-delivery-message-id", &stamped_id,
		"last-delivery-mail-id", &mail_id, "last-delivery-error", &failure, NULL);
	g_assert_nonnull(delivered_at);
	g_assert_cmpstr(stamped_id, ==, message_id);
	g_assert_cmpint(mail_id, ==, venture_entity_get_id(row));
	g_assert_true(failure == NULL || failure[0] == '\0');

	/* Nothing was sent synchronously; the outbox sweep does that. */
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	g_assert_cmpint(venture_mail_outbox_deliver_due(venture_context_get_mail_outbox(f->context),
		f->org, 10, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);

	/* Running the sweep again neither re-runs nor re-mails. */
	g_assert_cmpint(sweep(f, "2026-08-02T09:00:00Z"), ==, 0);
	g_ptr_array_unref(rows);
	rows = pack_messages(f);
	g_assert_cmpuint(rows->len, ==, 1);
}

/* A validator standing in for a policy plugin that refuses the enqueue. */
static gboolean
deny_mail_write(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	gboolean *deny = data;
	(void)database;
	(void)previous;
	if (*deny && VENTURE_IS_MAIL_MESSAGE(entity))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Mail denied by policy");
		return FALSE;
	}
	return TRUE;
}

static void
assert_delivery_failed(Fixture *f, VentureEntity *pack, const gchar *why)
{
	g_autoptr(VentureEntity) stored = reload(f, pack);
	g_autoptr(GDateTime) delivered_at = NULL;
	g_autoptr(GDateTime) run_at = NULL;
	g_autofree gchar *failure = NULL, *output = NULL, *message_id = NULL;
	g_object_get(stored, "last-delivered-at", &delivered_at, "last-delivery-error", &failure,
		"last-run-at", &run_at, "last-output", &output, "last-delivery-message-id", &message_id, NULL);
	g_assert_null(delivered_at);
	g_assert_true(message_id == NULL || message_id[0] == '\0');
	g_assert_nonnull(failure);
	g_assert_nonnull(strstr(failure, why));
	/* The run itself succeeded and is retained. */
	g_assert_nonnull(run_at);
	g_assert_nonnull(output);
	{
		g_autofree gchar *folded = g_ascii_strdown(output, -1);
		g_assert_nonnull(strstr(folded, "balances"));
	}
}

/* DONE WHEN 3: a refused send marks the delivery failed but never the run,
 * and the report is not run again for it. */
static void
test_refused_send(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) pack = make_pack(f, "Refused pack", "email", "ceo@example.test");
	g_autoptr(GPtrArray) rows = NULL;
	gboolean *deny = g_new0(gboolean, 1);
	(void)data;
	*deny = TRUE;
	venture_database_add_save_validator(f->db, VENTURE_TYPE_MAIL_MESSAGE, deny_mail_write, deny, g_free);
	g_assert_cmpint(sweep(f, "2026-08-02T09:00:00Z"), ==, 1);
	assert_delivery_failed(f, pack, "Mail denied by policy");
	rows = pack_messages(f);
	g_assert_cmpuint(rows->len, ==, 0);
	/* Lifting the refusal does not make the sweep run the report again. */
	*deny = FALSE;
	g_assert_cmpint(sweep(f, "2026-08-02T09:30:00Z"), ==, 0);
	g_ptr_array_unref(rows);
	rows = pack_messages(f);
	g_assert_cmpuint(rows->len, ==, 0);
}

/* DONE WHEN 4: with the mail module off the delivery is a recorded failure,
 * the sweep goes on to the next pack, and nothing crashes. */
static void
test_mail_module_off(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) pack = make_pack(f, "Offline pack", "email", "ceo@example.test");
	g_autoptr(VentureEntity) other = make_pack(f, "Other pack", "none", NULL);
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GDateTime) run_at = NULL;
	(void)data;
	venture_config_set_module_enabled(f->config, "mail", FALSE);
	g_assert_cmpint(sweep(f, "2026-08-02T09:00:00Z"), ==, 2);
	venture_config_set_module_enabled(f->config, "mail", TRUE);
	assert_delivery_failed(f, pack, "mail module");
	stored = reload(f, other);
	g_object_get(stored, "last-run-at", &run_at, NULL);
	g_assert_nonnull(run_at);
	g_assert_cmpint(sweep(f, "2026-08-02T09:00:00Z"), ==, 0);
}

/* DONE WHEN 4: mail on but no transport configured is the same recorded refusal. */
static void
test_no_transport(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) bare = venture_context_new(config, f->db);
	g_autoptr(VentureEntity) pack = make_pack(f, "Untransported pack", "email", "ceo@example.test");
	g_autoptr(GDateTime) as_of = g_date_time_new_from_iso8601("2026-08-02T09:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	(void)data;
	g_assert_cmpint(venture_report_pack_service_run_due(venture_report_pack_service_get(f->db),
		bare, f->org, as_of, NULL, &error), ==, 1);
	g_assert_no_error(error);
	assert_delivery_failed(f, pack, "transport");
	rows = pack_messages(f);
	g_assert_cmpuint(rows->len, ==, 0);
}

/* DONE WHEN 5: the deliver action re-sends the last retained output, and
 * refuses when there is none or the pack is not set to email. */
static void
test_deliver_resends(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) pack = make_pack(f, "Resend pack", "email", "ceo@example.test");
	g_autoptr(VentureEntity) quiet = make_pack(f, "Unmailed pack", "none", NULL);
	g_autoptr(VentureMailMessage) refused = NULL;
	g_autoptr(VentureMailMessage) again = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) later = g_date_time_new_from_iso8601("2026-08-02T10:00:00Z", NULL);
	g_autofree gchar *first_key = NULL, *again_key = NULL, *failure = NULL, *stamped = NULL, *again_id = NULL;
	(void)data;
	refused = venture_report_pack_service_deliver(venture_report_pack_service_get(f->db),
		f->context, VENTURE_REPORT_PACK(pack), NULL, NULL, NULL, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "output"));
	g_clear_error(&error);
	stored = reload(f, pack);
	g_object_get(stored, "last-delivery-error", &failure, NULL);
	g_assert_nonnull(failure);
	g_assert_nonnull(strstr(failure, "output"));
	g_clear_object(&stored);

	g_assert_cmpint(sweep(f, "2026-08-02T09:00:00Z"), ==, 2);
	rows = pack_messages(f);
	g_assert_cmpuint(rows->len, ==, 1);
	g_object_get(g_ptr_array_index(rows, 0), "idempotency-key", &first_key, NULL);

	/* The sweep saved its own copies; a client re-sends from a fresh load. */
	g_clear_object(&pack);
	pack = pack_row(f, "Resend pack");
	g_clear_object(&quiet);
	quiet = pack_row(f, "Unmailed pack");
	again = venture_report_pack_service_deliver(venture_report_pack_service_get(f->db),
		f->context, VENTURE_REPORT_PACK(pack), later, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(again);
	g_object_get(again, "idempotency-key", &again_key, "message-id", &again_id, NULL);
	g_assert_cmpstr(again_key, !=, first_key);
	g_ptr_array_unref(rows);
	rows = pack_messages(f);
	g_assert_cmpuint(rows->len, ==, 2);
	stored = reload(f, pack);
	g_object_get(stored, "last-delivery-message-id", &stamped, NULL);
	g_assert_cmpstr(stamped, ==, again_id);
	{
		/* The re-send carries the retained output, not a fresh run. */
		g_autoptr(GDateTime) run_at = NULL;
		g_autoptr(GDateTime) expected = g_date_time_new_from_iso8601("2026-08-02T09:00:00Z", NULL);
		g_object_get(stored, "last-run-at", &run_at, NULL);
		g_assert_cmpint(g_date_time_compare(run_at, expected), ==, 0);
	}
	g_clear_object(&again);
	again = venture_report_pack_service_deliver(venture_report_pack_service_get(f->db),
		f->context, VENTURE_REPORT_PACK(quiet), later, NULL, NULL, &error);
	g_assert_null(again);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "deliver"));
}

/* --- REST and CLI surfaces ---------------------------------------------- */

typedef struct
{
	gboolean done;
	GBytes *bytes;
	gchar *out;
	gchar *err;
	GError *error;
} Outcome;

typedef struct
{
	Fixture base;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gint64 pack_id;
} WebFixture;

static void
web_setup(WebFixture *w, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	g_autoptr(VentureEntity) pack = NULL;
	guint16 port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	w->state_dir = g_dir_make_tmp("venture-report-delivery-XXXXXX", &error);
	g_assert_no_error(error);
	setup(&w->base, data);
	g_object_set(w->base.config, "state-dir", w->state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", TRUE, NULL);
	w->server = venture_web_server_new(w->base.context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(w->server, &error));
	g_assert_no_error(error);
	w->session = soup_session_new();
	pack = make_pack(&w->base, "Board pack", "email", "board@example.test");
	w->pack_id = venture_entity_get_id(pack);
}

static void
web_teardown(WebFixture *w, gconstpointer data)
{
	venture_web_server_stop(w->server);
	g_clear_object(&w->session);
	g_clear_object(&w->server);
	teardown(&w->base, data);
	venture_test_remove_tree(w->state_dir);
	g_free(w->state_dir);
}

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Outcome *o = data;
	o->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &o->error);
	o->done = TRUE;
}

static guint
request(WebFixture *w, const gchar *method, const gchar *path, const gchar *cookie,
	const gchar *bearer, const gchar *form, gchar **out, gchar **set_cookie)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(w->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Outcome o = { FALSE, NULL, NULL, NULL, NULL };
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (cookie)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", cookie);
	if (bearer)
	{
		g_autofree gchar *header = g_strconcat("Bearer ", bearer, NULL);
		soup_message_headers_append(soup_message_get_request_headers(message), "Authorization", header);
	}
	if (form)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(w->session, message, G_PRIORITY_DEFAULT, NULL, http_done, &o);
	while (!o.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(o.error);
	if (out)
		*out = g_strndup(g_bytes_get_data(o.bytes, NULL), g_bytes_get_size(o.bytes));
	if (set_cookie)
		*set_cookie = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie"));
	g_bytes_unref(o.bytes);
	return soup_message_get_status(message);
}

static gint64
create_member(WebFixture *w, const gchar *username, const gchar *password, VentureUserRole role)
{
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureEntity) member = NULL;
	gint64 id;
	g_object_set(user, "username", username, "role", role, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, password, 100000, NULL));
	g_assert_true(venture_database_save(w->base.db, VENTURE_ENTITY(user), NULL, NULL));
	id = venture_entity_get_id(VENTURE_ENTITY(user));
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", id,
		"organization-id", w->base.org, "role", VENTURE_ORGANIZATION_ROLE_FINANCE, "active", TRUE, NULL);
	g_assert_true(venture_database_save(w->base.db, member, NULL, NULL));
	return id;
}

static gchar *
login(WebFixture *w, const gchar *username, const gchar *password)
{
	g_autofree gchar *form = g_strdup_printf("username=%s&password=%s", username, password);
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;
	g_assert_cmpuint(request(w, "POST", "/login", NULL, NULL, form, NULL, &set_cookie), ==, SOUP_STATUS_FOUND);
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');
	if (semicolon)
		*semicolon = '\0';
	return g_steal_pointer(&set_cookie);
}

static gchar *
editor_token(WebFixture *w, gint64 user_id)
{
	g_autoptr(VentureApiToken) token = g_object_new(VENTURE_TYPE_API_TOKEN, "name", "cli",
		"role", VENTURE_USER_ROLE_EDITOR, "user-id", user_id, NULL);
	gchar *secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(w->base.db, VENTURE_ENTITY(token), NULL, NULL));
	return secret;
}

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Outcome *o = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &o->out, &o->err, &o->error);
	o->done = TRUE;
}

static gboolean
cli_timeout(gpointer process)
{
	g_subprocess_force_exit(process);
	return G_SOURCE_CONTINUE;
}

/* Runs venturectl against the fixture server; returns stdout, or NULL when
 * the command failed (stderr then goes to the test log). */
static gchar *
cli(WebFixture *w, const gchar *token, const gchar *const *args)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	Outcome o = { FALSE, NULL, NULL, NULL, NULL };
	guint i, timeout;
	g_ptr_array_add(argv, g_canonicalize_filename("build/debug/venturectl", NULL));
	g_ptr_array_add(argv, g_strdup("--server"));
	g_ptr_array_add(argv, g_strdup(venture_web_server_get_base_url(w->server)));
	g_ptr_array_add(argv, g_strdup("-f"));
	g_ptr_array_add(argv, g_strdup("json"));
	for (i = 0; args[i]; i++)
		g_ptr_array_add(argv, g_strdup(args[i]));
	g_ptr_array_add(argv, NULL);
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", token, TRUE);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &o);
	while (!o.done)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(o.error);
	if (!g_subprocess_get_successful(process))
	{
		g_test_message("CLI failure: %s", o.err);
		g_clear_pointer(&o.out, g_free);
	}
	g_free(o.err);
	return o.out;
}

/* DONE WHEN 5: the REST action shares the editor gate; the CLI verb reaches it. */
static void
test_surfaces(WebFixture *w, gconstpointer data)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/report_pack/%" G_GINT64_FORMAT "/deliver", w->pack_id);
	g_autofree gchar *missing = g_strdup_printf("/api/v1/report_pack/%" G_GINT64_FORMAT "/deliver", w->pack_id + 100);
	g_autofree gchar *viewer = NULL, *editor = NULL, *token = NULL, *body = NULL, *out = NULL, *id_text = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gint64 editor_id;
	(void)data;
	post_something(&w->base, "2026-08-01T00:00:00Z", 2500);
	create_member(w, "vic", "v-long-password", VENTURE_USER_ROLE_VIEWER);
	editor_id = create_member(w, "ed", "e-long-password", VENTURE_USER_ROLE_EDITOR);
	viewer = login(w, "vic", "v-long-password");
	editor = login(w, "ed", "e-long-password");

	g_assert_cmpuint(request(w, "POST", path, NULL, NULL, NULL, NULL, NULL), ==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(request(w, "POST", path, viewer, NULL, NULL, NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
	/* An editor may, but there is nothing retained to send yet. */
	g_assert_cmpuint(request(w, "POST", path, editor, NULL, NULL, &body, NULL), ==, SOUP_STATUS_UNPROCESSABLE_ENTITY);
	g_assert_nonnull(strstr(body, "output"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(w, "POST", missing, editor, NULL, NULL, NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);

	g_assert_cmpint(sweep(&w->base, "2026-08-02T09:00:00Z"), ==, 1);
	g_assert_cmpuint(request(w, "POST", path, editor, NULL, NULL, &body, NULL), ==, SOUP_STATUS_ACCEPTED);
	g_assert_nonnull(strstr(body, "board@example.test"));
	g_assert_nonnull(strstr(body, "\"queued\""));
	rows = pack_messages(&w->base);
	g_assert_cmpuint(rows->len, ==, 2);

	token = editor_token(w, editor_id);
	id_text = g_strdup_printf("%" G_GINT64_FORMAT, w->pack_id);
	{
		const gchar *args[] = { "report", "packs", "deliver", id_text, NULL };
		out = cli(w, token, args);
	}
	g_assert_nonnull(out);
	g_assert_nonnull(strstr(out, "board@example.test"));
	g_ptr_array_unref(rows);
	rows = pack_messages(&w->base);
	g_assert_cmpuint(rows->len, ==, 3);
	{
		const gchar *args[] = { "report", "packs", "deliver", NULL };
		g_clear_pointer(&out, g_free);
		out = cli(w, token, args);
		g_assert_null(out);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/report-delivery/fields", Fixture, NULL, setup, test_fields, teardown);
	g_test_add("/report-delivery/sweep-queues-mail", Fixture, NULL, setup, test_sweep_queues_mail, teardown);
	g_test_add("/report-delivery/refused-send", Fixture, NULL, setup, test_refused_send, teardown);
	g_test_add("/report-delivery/mail-module-off", Fixture, NULL, setup, test_mail_module_off, teardown);
	g_test_add("/report-delivery/no-transport", Fixture, NULL, setup, test_no_transport, teardown);
	g_test_add("/report-delivery/deliver-resends", Fixture, NULL, setup, test_deliver_resends, teardown);
	g_test_add("/report-delivery/surfaces", WebFixture, NULL, web_setup, test_surfaces, web_teardown);
	return g_test_run();
}
