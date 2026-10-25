/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Contact roles on deals: who on the customer's side is on a deal and in
 * what capacity, one primary at most, the primary as a recipient, and the
 * open deals nobody has named a decision maker on.
 */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"

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

static void
save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, row, NULL, &error));
	g_assert_no_error(error);
}

static VentureEntity *
company(Fixture *f, const gchar *name)
{
	VentureEntity *row = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", (gint64)1, "name", name, NULL);
	save(f, row);
	return row;
}

static VentureEntity *
contact(Fixture *f, const gchar *name, const gchar *email, VentureEntity *at)
{
	VentureEntity *row = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1, "name", name,
		"email", email, "company-id", NULL != at ? venture_entity_get_id(at) : (gint64)0, NULL);
	save(f, row);
	return row;
}

static VentureEntity *
deal(Fixture *f, const gchar *name, VentureEntity *at)
{
	VentureEntity *row = VENTURE_ENTITY(venture_deal_new());
	g_autoptr(GError) error = NULL;
	g_object_set(row, "organization-id", (gint64)1, "name", name,
		"company-id", NULL != at ? venture_entity_get_id(at) : (gint64)0, NULL);
	g_assert_true(venture_entity_set_field_from_string(row, "value", "500.00 USD", &error));
	save(f, row);
	return row;
}

/* A deal role, unsaved. */
static VentureEntity *
role(VentureEntity *d, VentureEntity *person, VentureDealContactRole kind, gboolean primary)
{
	return g_object_new(VENTURE_TYPE_DEAL_CONTACT, "organization-id", (gint64)1,
		"deal-id", venture_entity_get_id(d), "contact-id", venture_entity_get_id(person),
		"role", kind, "primary", primary, NULL);
}

static gint
count_roles(Fixture *f, const gchar *field, gint64 id, gboolean primary_only)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEAL_CONTACT);
	g_autoptr(GError) error = NULL;
	gint result;
	venture_query_set_organization(query, 1);
	venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, NULL);
	if (primary_only)
		venture_query_add_filter_string(query, "primary", VENTURE_FILTER_OP_EQ, "true", NULL);
	result = venture_database_count(f->db, query, &error);
	g_assert_no_error(error);
	return result;
}

/* The record has the fields the issue names, in the crm module. */
static void
test_records(void)
{
	static const gchar *names[] = { "deal-id", "contact-id", "role", "primary", "note" };
	g_autoptr(VentureDealContact) row = venture_deal_contact_new();
	guint i;
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "deal_contact"), !=, G_TYPE_INVALID);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(row), names[i]));
	{
		GEnumClass *roles = g_type_class_ref(VENTURE_TYPE_DEAL_CONTACT_ROLE);
		static const gchar *nicks[] = { "decision_maker", "economic_buyer", "champion", "influencer",
			"technical", "user", "other" };
		for (i = 0; i < G_N_ELEMENTS(nicks); i++)
			g_assert_nonnull(g_enum_get_value_by_nick(roles, nicks[i]));
		g_type_class_unref(roles);
	}
}

/* At most one primary a deal: a second is refused, from any door. */
static void
test_second_primary_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com", acme);
	g_autoptr(VentureEntity) bob = contact(f, "Bob", "bob@example.com", acme);
	g_autoptr(VentureEntity) d = deal(f, "Rollout", acme);
	g_autoptr(VentureEntity) first = role(d, ada, VENTURE_DEAL_CONTACT_ROLE_DECISION_MAKER, TRUE);
	g_autoptr(VentureEntity) second = role(d, bob, VENTURE_DEAL_CONTACT_ROLE_CHAMPION, TRUE);
	g_autoptr(VentureEntity) again = role(d, ada, VENTURE_DEAL_CONTACT_ROLE_INFLUENCER, FALSE);
	g_autoptr(VentureEntity) other = deal(f, "Second site", acme);
	g_autoptr(VentureEntity) elsewhere = role(other, bob, VENTURE_DEAL_CONTACT_ROLE_USER, TRUE);
	g_autoptr(GError) error = NULL;
	(void)unused;
	save(f, first);
	g_assert_false(venture_database_save(f->db, second, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "primary"));
	g_clear_error(&error);
	g_assert_cmpint(count_roles(f, "deal-id", venture_entity_get_id(d), TRUE), ==, 1);

	/* Bob joins as a plain champion; the same person twice is refused. */
	g_object_set(second, "primary", FALSE, NULL);
	save(f, second);
	g_assert_false(venture_database_save(f->db, again, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* Promoting Bob while Ada is primary is the same refusal. */
	g_object_set(second, "primary", TRUE, NULL);
	g_assert_false(venture_database_save(f->db, second, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* Another deal has its own primary. */
	save(f, elsewhere);
	g_assert_cmpint(count_roles(f, "deal-id", venture_entity_get_id(other), TRUE), ==, 1);
}

/* Converting a lead makes its contact the new deal's one primary. */
static void
test_lead_conversion(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) lead = VENTURE_ENTITY(g_object_new(VENTURE_TYPE_LEAD, "organization-id", (gint64)1,
		"name", "Grace", "email", "grace@example.com", "company-name", "Example Works",
		"status", VENTURE_LEAD_QUALIFIED, NULL));
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(VentureEntity) primary = NULL;
	g_autoptr(VentureEntity) d = NULL;
	g_autoptr(GError) error = NULL;
	gint64 contact_id = 0, deal_id = 0;
	(void)unused;
	save(f, lead);
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(converted);
	g_object_get(converted, "converted-contact-id", &contact_id, "converted-deal-id", &deal_id, NULL);
	g_assert_cmpint(deal_id, >, 0);
	g_assert_cmpint(count_roles(f, "deal-id", deal_id, FALSE), ==, 1);
	g_assert_cmpint(count_roles(f, "deal-id", deal_id, TRUE), ==, 1);
	d = venture_database_get(f->db, VENTURE_TYPE_DEAL, deal_id, &error);
	g_assert_no_error(error);
	primary = venture_deal_contact_primary(f->db, d, &error);
	g_assert_no_error(error);
	g_assert_nonnull(primary);
	g_assert_cmpint(venture_entity_get_id(primary), ==, contact_id);
}

/* Deleting a contact takes its deal roles with it and leaves the deal. */
static void
test_contact_delete(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com", acme);
	g_autoptr(VentureEntity) bob = contact(f, "Bob", "bob@example.com", acme);
	g_autoptr(VentureEntity) d = deal(f, "Rollout", acme);
	g_autoptr(VentureEntity) e = deal(f, "Renewal", acme);
	g_autoptr(VentureEntity) one = role(d, ada, VENTURE_DEAL_CONTACT_ROLE_DECISION_MAKER, TRUE);
	g_autoptr(VentureEntity) two = role(e, ada, VENTURE_DEAL_CONTACT_ROLE_ECONOMIC_BUYER, FALSE);
	g_autoptr(VentureEntity) three = role(d, bob, VENTURE_DEAL_CONTACT_ROLE_TECHNICAL, FALSE);
	g_autoptr(VentureEntity) fresh = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	save(f, one);
	save(f, two);
	save(f, three);
	g_assert_true(venture_database_delete(f->db, ada, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(count_roles(f, "contact-id", venture_entity_get_id(ada), FALSE), ==, 0);
	g_assert_cmpint(count_roles(f, "contact-id", venture_entity_get_id(bob), FALSE), ==, 1);
	fresh = venture_database_get(f->db, VENTURE_TYPE_DEAL, venture_entity_get_id(d), &error);
	g_assert_no_error(error);
	g_assert_nonnull(fresh);
	g_assert_false(venture_entity_is_deleted(fresh));
	g_clear_object(&fresh);
	fresh = venture_database_get(f->db, VENTURE_TYPE_DEAL, venture_entity_get_id(e), &error);
	g_assert_no_error(error);
	g_assert_false(venture_entity_is_deleted(fresh));
}

/* The primary is the recipient; without one, the deal's own contact is. */
static void
test_primary_recipient(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com", acme);
	g_autoptr(VentureEntity) bob = contact(f, "Bob", "bob@example.com", acme);
	g_autoptr(VentureEntity) d = deal(f, "Rollout", acme);
	g_autoptr(VentureEntity) bare = deal(f, "Nobody yet", acme);
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(VentureEntity) link = NULL;
	g_autoptr(VentureEntity) sequence = NULL;
	g_autoptr(VentureEntity) step = NULL;
	g_autoptr(VentureEntity) enrollment = NULL;
	g_autoptr(GError) error = NULL;
	gint64 enrolled = 0;
	(void)unused;
	g_object_set(d, "contact-id", venture_entity_get_id(ada), NULL);
	save(f, d);

	/* No roles yet: the deal's own contact. */
	found = venture_deal_contact_primary(f->db, d, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(found), ==, venture_entity_get_id(ada));
	g_clear_object(&found);

	/* A primary role wins over it. */
	link = role(d, bob, VENTURE_DEAL_CONTACT_ROLE_CHAMPION, TRUE);
	save(f, link);
	found = venture_deal_contact_primary(f->db, d, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(found), ==, venture_entity_get_id(bob));
	g_clear_object(&found);

	/* A deal with neither has no recipient, and that is not an error. */
	found = venture_deal_contact_primary(f->db, bare, &error);
	g_assert_no_error(error);
	g_assert_null(found);

	/* A sequence enrolment that names only the deal goes to its primary. */
	sequence = g_object_new(VENTURE_TYPE_SEQUENCE, "organization-id", (gint64)1, "name", "Proposal follow-up",
		"active", TRUE, "timezone", "UTC", "send-window-start", (gint64)0, "send-window-end", (gint64)24,
		"weekdays", "1,2,3,4,5,6,7", NULL);
	save(f, sequence);
	step = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(sequence), "position", (gint64)10, "active", TRUE,
		"subject", "About {contact.name}", "body", "Hello", NULL);
	save(f, step);
	enrollment = g_object_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(sequence), "deal-id", venture_entity_get_id(d), NULL);
	g_assert_true(venture_sequence_service_enroll(venture_sequence_service_get(f->db), enrollment, NULL, &error));
	g_assert_no_error(error);
	g_object_get(enrollment, "contact-id", &enrolled, NULL);
	g_assert_cmpint(enrolled, ==, venture_entity_get_id(bob));
}

static const GValue *
cell(VentureReportResult *result, guint row, const gchar *column)
{
	const GValue *value = venture_report_result_get_cell(result, row, column);
	g_assert_nonnull(value);
	return value;
}

/* The report lists open deals with no decision maker, and only those. */
static void
test_no_decision_maker_report(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com", acme);
	g_autoptr(VentureEntity) bob = contact(f, "Bob", "bob@example.com", acme);
	g_autoptr(VentureEntity) decided = deal(f, "Has a decider", acme);
	g_autoptr(VentureEntity) undecided = deal(f, "Champion only", acme);
	g_autoptr(VentureEntity) nobody = deal(f, "No contacts", acme);
	g_autoptr(VentureEntity) closed = VENTURE_ENTITY(venture_deal_new());
	g_autoptr(VentureEntity) r1 = role(decided, ada, VENTURE_DEAL_CONTACT_ROLE_DECISION_MAKER, TRUE);
	g_autoptr(VentureEntity) r2 = role(undecided, bob, VENTURE_DEAL_CONTACT_ROLE_CHAMPION, TRUE);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	gboolean saw_undecided = FALSE, saw_nobody = FALSE;
	guint i;
	(void)unused;
	save(f, r1);
	save(f, r2);
	/* A new deal keeps the stage it is made in: this one closed won. */
	g_object_set(closed, "organization-id", (gint64)1, "name", "Won already",
		"company-id", venture_entity_get_id(acme), "stage", VENTURE_DEAL_STAGE_WON, NULL);
	save(f, closed);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "deals_without_decision_maker");
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	for (i = 0; i < 2; i++)
	{
		const gchar *name = g_value_get_string(cell(result, i, "deal"));
		saw_undecided = saw_undecided || 0 == g_strcmp0(name, "Champion only");
		saw_nobody = saw_nobody || 0 == g_strcmp0(name, "No contacts");
	}
	g_assert_true(saw_undecided);
	g_assert_true(saw_nobody);
	g_assert_cmpfloat(venture_metric_get_number(g_ptr_array_index(venture_report_result_get_metrics(result), 0)), ==, 2);
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

/* The deal page adds and removes people with a role; the contact page
 * lists the deals they are on and as what. */
static void
test_pages(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) ada = contact(f, "Ada Lovelace", "ada@example.com", acme);
	g_autoptr(VentureEntity) d = deal(f, "Analytical engine", acme);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) added = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL, *person = NULL, *add = NULL, *body = NULL, *remove = NULL;
	gchar *text = NULL;
	(void)unused;
	start_http(f);
	page = g_strdup_printf("/e/deal/%" G_GINT64_FORMAT, venture_entity_get_id(d));
	person = g_strdup_printf("/e/contact/%" G_GINT64_FORMAT, venture_entity_get_id(ada));
	add = g_strdup_printf("/deals/%" G_GINT64_FORMAT "/contacts", venture_entity_get_id(d));

	g_assert_cmpuint(http(f, "GET", page, NULL, &text), ==, 200);
	g_assert_nonnull(strstr(text, "Buying committee"));
	g_assert_nonnull(strstr(text, add));
	g_clear_pointer(&text, g_free);

	body = g_strdup_printf("contact_id=%" G_GINT64_FORMAT "&role=decision_maker&primary=1&note=Signs+off",
		venture_entity_get_id(ada));
	g_assert_cmpuint(http(f, "POST", add, body, &text), ==, 302);
	g_clear_pointer(&text, g_free);
	g_assert_cmpint(count_roles(f, "deal-id", venture_entity_get_id(d), TRUE), ==, 1);

	g_assert_cmpuint(http(f, "GET", page, NULL, &text), ==, 200);
	g_assert_nonnull(strstr(text, "Ada Lovelace"));
	g_assert_nonnull(strstr(text, "Decision maker"));
	g_assert_nonnull(strstr(text, "Signs off"));
	g_clear_pointer(&text, g_free);

	g_assert_cmpuint(http(f, "GET", person, NULL, &text), ==, 200);
	g_assert_nonnull(strstr(text, "Analytical engine"));
	g_assert_nonnull(strstr(text, "Decision maker"));
	g_clear_pointer(&text, g_free);

	/* A second primary through the page is refused, not saved. */
	{
		g_autoptr(VentureEntity) bob = contact(f, "Bob", "bob@example.com", acme);
		g_autofree gchar *again = g_strdup_printf("contact_id=%" G_GINT64_FORMAT "&role=champion&primary=1",
			venture_entity_get_id(bob));
		g_assert_cmpuint(http(f, "POST", add, again, &text), >=, 400);
		g_clear_pointer(&text, g_free);
		g_assert_cmpint(count_roles(f, "deal-id", venture_entity_get_id(d), FALSE), ==, 1);
	}

	query = venture_query_new(VENTURE_TYPE_DEAL_CONTACT);
	venture_query_add_filter_int(query, "deal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(d), NULL);
	added = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(added);
	remove = g_strdup_printf("/deals/%" G_GINT64_FORMAT "/contacts/%" G_GINT64_FORMAT "/remove",
		venture_entity_get_id(d), venture_entity_get_id(added));
	g_assert_cmpuint(http(f, "POST", remove, "", &text), ==, 302);
	g_clear_pointer(&text, g_free);
	g_assert_cmpint(count_roles(f, "deal-id", venture_entity_get_id(d), FALSE), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/deal-contacts/records", test_records);
	g_test_add("/deal-contacts/second-primary-refused", Fixture, NULL, setup, test_second_primary_refused, teardown);
	g_test_add("/deal-contacts/lead-conversion", Fixture, NULL, setup, test_lead_conversion, teardown);
	g_test_add("/deal-contacts/contact-delete", Fixture, NULL, setup, test_contact_delete, teardown);
	g_test_add("/deal-contacts/primary-recipient", Fixture, NULL, setup, test_primary_recipient, teardown);
	g_test_add("/deal-contacts/no-decision-maker-report", Fixture, NULL, setup, test_no_decision_maker_report, teardown);
	g_test_add("/deal-contacts/pages", Fixture, NULL, setup, test_pages, teardown);
	return g_test_run();
}
