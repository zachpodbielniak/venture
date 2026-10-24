/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Journeys are sequences: the same records, the same lazy sweep and the
 * same delivery adapter, extended with branch steps (opened, clicked,
 * replied, or not, by the end of a wait), jumps and path ends, a channel
 * hook for later channels, named triggers that enrol a contact once per
 * event, and the recipient's marketing preferences. Every clock here is
 * the sweep's own, so the branch timing is exact. No network, no mail. */
#include <venture.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureSequenceService *service;
	VentureEntity *journey;
} Fixture;

static void persist(Fixture *f, gpointer row)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_no_error(error);
}
static void refuse(Fixture *f, gpointer row, const gchar *words)
{
	g_autoptr(GError) error = NULL;
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_nonnull(error);
	g_test_message("refused: %s", error->message);
	if (words && !strstr(error->message, words)) g_error("expected \"%s\" in \"%s\"", words, error->message);
}
static VentureEntity *reread(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *fresh = venture_database_get(f->db, G_OBJECT_TYPE(row), venture_entity_get_id(row), &error);
	g_assert_no_error(error);
	return fresh;
}
static GDateTime *at(const gchar *text)
{
	GDateTime *value = g_date_time_new_from_iso8601(text, NULL);
	g_assert_nonnull(value);
	return value;
}
static VentureEntity *contact(Fixture *f, const gchar *name, const gchar *email)
{
	VentureEntity *row = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1, "name", name, "email", email, NULL);
	persist(f, row);
	return row;
}
static VentureEntity *journey(Fixture *f, const gchar *name)
{
	VentureEntity *row = g_object_new(VENTURE_TYPE_SEQUENCE, "organization-id", (gint64)1, "name", name,
		"active", TRUE, "timezone", "UTC", "send-window-start", (gint64)0, "send-window-end", (gint64)24,
		"weekdays", "1,2,3,4,5,6,7", "exit-on-reply", FALSE, "exit-on-unsubscribe", TRUE,
		"exit-on-deal-won", FALSE, "tracking", TRUE, NULL);
	persist(f, row);
	return row;
}
static VentureEntity *email_step(Fixture *f, VentureEntity *sequence, gint64 position, const gchar *subject, gboolean end)
{
	VentureEntity *row = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(sequence), "position", position, "channel", 0, "active", TRUE,
		"subject", subject, "body", "Hello {contact.name}, read https://example.com/guide", "end-after", end, NULL);
	persist(f, row);
	return row;
}
static VentureEntity *branch_step(Fixture *f, VentureEntity *sequence, gint64 position, gint on,
	gint64 days, gint64 yes, gint64 no)
{
	VentureEntity *row = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(sequence), "position", position, "channel", 4, "active", TRUE,
		"branch-on", on, "delay-days", days, "yes-position", yes, "no-position", no, NULL);
	persist(f, row);
	return row;
}
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->service = venture_sequence_service_get(f->db);
	g_object_set(f->service, "base-url", "https://venture.example.com", NULL);
	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, 1, &error);
	g_assert_no_error(error);
	g_object_set(organization, "sequence-tracking", TRUE, NULL);
	persist(f, organization);
	f->journey = journey(f, "Welcome journey");
}
static void teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->journey);
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}
static VentureEntity *enrol(Fixture *f, VentureEntity *sequence, VentureEntity *person, const gchar *when)
{
	g_autoptr(GDateTime) moment = at(when);
	g_autoptr(GError) error = NULL;
	VentureEntity *row = g_object_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(sequence), "contact-id", venture_entity_get_id(person),
		"enrolled-at", moment, "enrollment-reason", "Test", NULL);
	g_assert_true(venture_sequence_service_enroll(f->service, row, NULL, &error));
	g_assert_no_error(error);
	return row;
}
static gint sweep(Fixture *f, const gchar *when)
{
	g_autoptr(GDateTime) moment = at(when);
	g_autoptr(GError) error = NULL;
	gint processed = venture_sequence_service_run_due(f->service, 1, moment, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(processed, >=, 0);
	return processed;
}
static VentureEntity *delivery_for(Fixture *f, VentureEntity *enrollment, VentureEntity *step)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_DELIVERY);
	g_autoptr(GError) error = NULL;
	VentureEntity *row;
	venture_query_set_organization(query, 1);
	venture_query_add_filter_int(query, "enrollment-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(enrollment), NULL);
	venture_query_add_filter_int(query, "step-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(step), NULL);
	row = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	return row;
}
static gint delivery_state(Fixture *f, VentureEntity *enrollment, VentureEntity *step)
{
	g_autoptr(VentureEntity) row = delivery_for(f, enrollment, step);
	gint state = -1;
	if (row) g_object_get(row, "state", &state, NULL);
	return state;
}
static gchar *branch_result(Fixture *f, VentureEntity *enrollment, VentureEntity *step)
{
	g_autoptr(VentureEntity) row = delivery_for(f, enrollment, step);
	gchar *result = NULL;
	g_assert_nonnull(row);
	g_object_get(row, "branch-result", &result, NULL);
	return result;
}
static gint status_of(Fixture *f, VentureEntity *enrollment)
{
	g_autoptr(VentureEntity) fresh = reread(f, enrollment);
	gint status = -1;
	g_object_get(fresh, "status", &status, NULL);
	return status;
}
static gchar *token_of(Fixture *f, VentureEntity *enrollment, VentureEntity *step)
{
	g_autoptr(VentureEntity) row = delivery_for(f, enrollment, step);
	gchar *token = NULL;
	g_assert_nonnull(row);
	g_object_get(row, "tracking-token", &token, NULL);
	g_assert_nonnull(token);
	return token;
}
static void open_at(Fixture *f, VentureEntity *enrollment, VentureEntity *step, const gchar *agent, const gchar *when)
{
	g_autofree gchar *token = token_of(f, enrollment, step);
	g_autoptr(GDateTime) moment = at(when);
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_sequence_service_record_open_full(f->service, token, agent, moment, NULL, &error));
	g_assert_no_error(error);
}

/* ------------------------------------------------------------------ */
/* An opened branch: the email goes, the branch waits three days to the
 * second, then the journey takes the yes path or the no path. A
 * scanner's open is not a person's and takes the no path; an open after
 * the wait is over does not count even if the sweep runs late. */
static void test_branch_opened(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) hello = email_step(f, f->journey, 10, "Hello", FALSE);
	g_autoptr(VentureEntity) branch = branch_step(f, f->journey, 20, 1, 3, 30, 40);
	g_autoptr(VentureEntity) thanks = email_step(f, f->journey, 30, "Thanks for reading", TRUE);
	g_autoptr(VentureEntity) missed = email_step(f, f->journey, 40, "Did you miss this?", TRUE);
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com");
	g_autoptr(VentureEntity) bob = contact(f, "Bob", "bob@example.com");
	g_autoptr(VentureEntity) cy = contact(f, "Cy", "cy@example.com");
	g_autoptr(VentureEntity) dee = contact(f, "Dee", "dee@example.com");
	g_autoptr(VentureEntity) a = NULL, b = NULL, c = NULL, d = NULL;
	g_autofree gchar *result = NULL;
	(void)data;
	a = enrol(f, f->journey, ada, "2026-09-14T10:00:00Z");
	b = enrol(f, f->journey, bob, "2026-09-14T10:00:00Z");
	c = enrol(f, f->journey, cy, "2026-09-14T10:00:00Z");
	d = enrol(f, f->journey, dee, "2026-09-14T10:00:00Z");
	g_assert_cmpint(sweep(f, "2026-09-14T10:00:00Z"), ==, 4);
	g_assert_cmpint(delivery_state(f, a, hello), ==, 0);
	open_at(f, a, hello, "Mozilla/5.0 (X11; Linux x86_64)", "2026-09-14T11:00:00Z");
	open_at(f, c, hello, "Barracuda Web Security", "2026-09-14T11:00:00Z");
	/* One second before the wait is over nothing moves. */
	g_assert_cmpint(sweep(f, "2026-09-17T09:59:59Z"), ==, 0);
	g_assert_null(delivery_for(f, a, branch));
	/* Dee opens after the deadline; the sweep runs an hour late. */
	open_at(f, d, hello, "Mozilla/5.0", "2026-09-17T10:30:00Z");
	g_assert_cmpint(sweep(f, "2026-09-17T11:00:00Z"), ==, 4);
	result = branch_result(f, a, branch); g_assert_cmpstr(result, ==, "yes"); g_clear_pointer(&result, g_free);
	result = branch_result(f, b, branch); g_assert_cmpstr(result, ==, "no"); g_clear_pointer(&result, g_free);
	result = branch_result(f, c, branch); g_assert_cmpstr(result, ==, "no"); g_clear_pointer(&result, g_free);
	result = branch_result(f, d, branch); g_assert_cmpstr(result, ==, "no"); g_clear_pointer(&result, g_free);
	g_assert_cmpint(sweep(f, "2026-09-17T11:00:00Z"), ==, 4);
	/* Ada took the yes path and it ended there; the others the no path. */
	g_assert_cmpint(delivery_state(f, a, thanks), ==, 0);
	g_assert_cmpint(delivery_state(f, a, missed), ==, -1);
	g_assert_cmpint(delivery_state(f, b, thanks), ==, 3);
	g_assert_cmpint(delivery_state(f, b, missed), ==, 0);
	g_assert_cmpint(delivery_state(f, c, missed), ==, 0);
	g_assert_cmpint(status_of(f, a), ==, 2);
	g_assert_cmpint(status_of(f, b), ==, 2);
	/* Nothing more happens however often it is swept. */
	g_assert_cmpint(sweep(f, "2026-09-30T11:00:00Z"), ==, 0);
}

/* Clicked and replied branches: a click on the latest email, and an
 * inbound reply after it within the wait; "no response within the wait"
 * is the no path of replied. A reply from before the email is not one. */
static void test_branch_clicked_replied(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) second = journey(f, "Reply journey");
	g_autoptr(VentureEntity) hello = email_step(f, f->journey, 10, "Hello", FALSE);
	g_autoptr(VentureEntity) clicked = branch_step(f, f->journey, 20, 2, 2, 30, 40);
	g_autoptr(VentureEntity) yes = email_step(f, f->journey, 30, "You clicked", TRUE);
	g_autoptr(VentureEntity) no = email_step(f, f->journey, 40, "You did not", TRUE);
	g_autoptr(VentureEntity) ask = email_step(f, second, 10, "Question", FALSE);
	g_autoptr(VentureEntity) replied = branch_step(f, second, 20, 3, 2, 0, 40);
	g_autoptr(VentureEntity) answered = email_step(f, second, 30, "Thanks for answering", TRUE);
	g_autoptr(VentureEntity) silent = email_step(f, second, 40, "Still there?", TRUE);
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com");
	g_autoptr(VentureEntity) bob = contact(f, "Bob", "bob@example.com");
	g_autoptr(VentureEntity) a = NULL, b = NULL, ra = NULL, rb = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	(void)data;
	a = enrol(f, f->journey, ada, "2026-09-14T10:00:00Z");
	b = enrol(f, f->journey, bob, "2026-09-14T10:00:00Z");
	ra = enrol(f, second, ada, "2026-09-14T10:00:00Z");
	rb = enrol(f, second, bob, "2026-09-14T10:00:00Z");
	{
		/* Bob wrote the day before: not a reply to this email. */
		g_autoptr(GDateTime) before = at("2026-09-13T10:00:00Z");
		g_autoptr(VentureEntity) early = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1,
			"contact-id", venture_entity_get_id(bob), "kind", VENTURE_INTERACTION_KIND_EMAIL, "outbound", FALSE,
			"subject", "Earlier question", "occurred-at", before, NULL);
		persist(f, early);
	}
	g_assert_cmpint(sweep(f, "2026-09-14T10:00:00Z"), ==, 4);
	{
		g_autofree gchar *token = token_of(f, a, hello);
		g_autoptr(GDateTime) moment = at("2026-09-15T08:00:00Z");
		g_autofree gchar *url = venture_sequence_service_record_click_full(f->service, token, 1, "Mozilla/5.0", moment, NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(url, ==, "https://example.com/guide");
	}
	{
		g_autoptr(GDateTime) moment = at("2026-09-15T09:00:00Z");
		g_autoptr(VentureEntity) reply = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1,
			"contact-id", venture_entity_get_id(ada), "kind", VENTURE_INTERACTION_KIND_EMAIL, "outbound", FALSE,
			"subject", "Re: Question", "occurred-at", moment, NULL);
		persist(f, reply);
	}
	/* The reply did not exit the journey: exit-on-reply is off for it. */
	g_assert_cmpint(status_of(f, ra), ==, 0);
	g_assert_cmpint(sweep(f, "2026-09-16T10:00:00Z"), ==, 4);
	result = branch_result(f, a, clicked); g_assert_cmpstr(result, ==, "yes"); g_clear_pointer(&result, g_free);
	result = branch_result(f, b, clicked); g_assert_cmpstr(result, ==, "no"); g_clear_pointer(&result, g_free);
	result = branch_result(f, ra, replied); g_assert_cmpstr(result, ==, "yes"); g_clear_pointer(&result, g_free);
	result = branch_result(f, rb, replied); g_assert_cmpstr(result, ==, "no"); g_clear_pointer(&result, g_free);
	g_assert_cmpint(sweep(f, "2026-09-16T10:00:00Z"), ==, 4);
	g_assert_cmpint(delivery_state(f, a, yes), ==, 0);
	g_assert_cmpint(delivery_state(f, b, no), ==, 0);
	/* yes-position empty is the next step. */
	g_assert_cmpint(delivery_state(f, ra, answered), ==, 0);
	g_assert_cmpint(delivery_state(f, ra, silent), ==, -1);
	g_assert_cmpint(delivery_state(f, rb, silent), ==, 0);
	(void)ask;
}

/* A branch only goes forward, and only a branch step branches: a path
 * that could loop back would never end, and the save says so. */
static void test_branch_validation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) hello = email_step(f, f->journey, 10, "Hello", FALSE);
	g_autoptr(VentureEntity) step = NULL;
	(void)data;
	step = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1, "sequence-id", venture_entity_get_id(f->journey),
		"position", (gint64)20, "channel", 4, "active", TRUE, "branch-on", 0, "delay-days", (gint64)1, NULL);
	refuse(f, step, "what to branch on");
	g_object_set(step, "branch-on", 1, "yes-position", (gint64)10, NULL);
	refuse(f, step, "after");
	g_object_set(step, "yes-position", (gint64)30, "no-position", (gint64)20, NULL);
	refuse(f, step, "after");
	g_object_set(step, "no-position", (gint64)0, NULL);
	persist(f, step);
	g_object_set(hello, "then-position", (gint64)5, NULL);
	refuse(f, hello, "after");
	g_object_set(hello, "then-position", (gint64)0, "branch-on", 1, NULL);
	refuse(f, hello, "branch step");
}

/* ------------------------------------------------------------------ */
/* The channel hook: a later lane registers what a channel does. The
 * registered executor runs in the step's transaction instead of the
 * built-in owner notification. */
static gboolean texting(VentureDatabase *database, VentureEntity *delivery, VentureEntity *person,
	VentureEntity *step, GDateTime *as_of, gpointer data, GError **error)
{
	guint *calls = data;
	(void)database; (void)person; (void)step; (void)as_of; (void)error;
	(*calls)++;
	g_object_set(delivery, "state", 1, "external-message-id", "hook-1", NULL);
	return TRUE;
}
static void test_channel_hook(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com");
	g_autoptr(VentureEntity) text = g_object_new(VENTURE_TYPE_SEQUENCE_STEP, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(f->journey), "position", (gint64)10, "channel", 2, "active", TRUE,
		"subject", "Text", "body", "Hi {contact.name}", NULL);
	g_autoptr(VentureEntity) row = NULL, delivery = NULL;
	g_autofree gchar *external = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_NOTIFICATION);
	g_autoptr(GError) error = NULL;
	guint calls = 0;
	gint state = -1;
	(void)data;
	persist(f, text);
	venture_sequence_channel_register(2, texting, &calls);
	row = enrol(f, f->journey, ada, "2026-09-14T10:00:00Z");
	g_assert_cmpint(sweep(f, "2026-09-14T10:00:00Z"), ==, 1);
	venture_sequence_channel_unregister(2);
	g_assert_cmpuint(calls, ==, 1);
	delivery = delivery_for(f, row, text);
	g_assert_nonnull(delivery);
	g_object_get(delivery, "state", &state, "external-message-id", &external, NULL);
	g_assert_cmpint(state, ==, 1);
	g_assert_cmpstr(external, ==, "hook-1");
	venture_query_set_organization(query, 1);
	g_assert_cmpint(venture_database_count(f->db, query, &error), ==, 0);
}

/* ------------------------------------------------------------------ */
/* Triggers. Each enrols a contact at most once per event per journey:
 * the event's identity is in the enrollment's unique trigger key, so a
 * re-run, a re-save or two triggers on one journey cannot enrol twice.
 * Suppression is honoured: a suppressed address is never enrolled. */
static VentureEntity *trigger(Fixture *f, VentureEntity *sequence, gint kind, const gchar *tag)
{
	VentureEntity *row = g_object_new(VENTURE_TYPE_SEQUENCE_TRIGGER, "organization-id", (gint64)1,
		"name", "Test trigger", "sequence-id", venture_entity_get_id(sequence), "kind", kind, "tag", tag, "active", TRUE, NULL);
	persist(f, row);
	return row;
}
static gint64 enrolled(Fixture *f, VentureEntity *sequence, VentureEntity *person)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT);
	g_autoptr(GError) error = NULL;
	gint64 count;
	venture_query_set_organization(query, 1);
	venture_query_add_filter_int(query, "sequence-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(sequence), NULL);
	venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(person), NULL);
	count = venture_database_count(f->db, query, &error);
	g_assert_no_error(error);
	return count;
}
static VentureEntity *only_enrollment(Fixture *f, VentureEntity *sequence, VentureEntity *person)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT);
	g_autoptr(GError) error = NULL;
	VentureEntity *row;
	venture_query_set_organization(query, 1);
	venture_query_add_filter_int(query, "sequence-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(sequence), NULL);
	venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(person), NULL);
	row = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	return row;
}
static gint triggers_at(Fixture *f, GDateTime *now)
{
	g_autoptr(GError) error = NULL;
	gint count = venture_sequence_service_run_triggers(f->service, 1, now, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(count, >=, 0);
	return count;
}

static void test_trigger_tag(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) step = email_step(f, f->journey, 10, "Welcome, VIP", TRUE);
	g_autoptr(VentureEntity) rule = trigger(f, f->journey, 1, "VIP");
	g_autoptr(VentureEntity) ada = NULL, bob = NULL, row = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = NULL;
	gint64 trigger_id = 0;
	(void)data;
	ada = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1, "name", "Ada", "email", "ada@example.com", "tags", "early, vip", NULL);
	persist(f, ada);
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 1);
	row = only_enrollment(f, f->journey, ada);
	g_object_get(row, "trigger-id", &trigger_id, "trigger-key", &key, NULL);
	g_assert_cmpint(trigger_id, ==, venture_entity_get_id(rule));
	g_assert_nonnull(key);
	/* A save that adds no tag adds no enrollment. */
	g_object_set(ada, "name", "Ada L.", NULL); persist(f, ada);
	g_object_set(ada, "tags", "VIP, early, gold", NULL); persist(f, ada);
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 1);
	/* After the journey, removing and adding the tag again is a new event. */
	g_assert_true(venture_sequence_service_transition(f->service, 1, venture_entity_get_id(row), "exit", NULL, NULL, &error));
	g_assert_no_error(error);
	g_object_set(ada, "tags", "early", NULL); persist(f, ada);
	g_object_set(ada, "tags", "early, Vip", NULL); persist(f, ada);
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 2);
	/* The tag action is a tag operation like any other. */
	bob = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1, "name", "Bob", "email", "bob@example.com", NULL);
	persist(f, bob);
	{
		g_autoptr(JsonNode) values = json_from_string("{\"tag\":\"vip\"}", NULL);
		g_autoptr(GHashTable) parameters = venture_action_parameters_from_json(values, &error);
		g_autoptr(VentureEntity) result = NULL;
		g_assert_no_error(error);
		result = venture_action_registry_perform(venture_database_get_action_registry(f->db), "contact",
			venture_entity_get_id(bob), "add_tag", parameters, NULL, VENTURE_USER_ROLE_EDITOR, &error);
		g_assert_no_error(error);
	}
	g_assert_cmpint(enrolled(f, f->journey, bob), ==, 1);
	/* An inactive trigger enrols nobody; a suppressed address is never enrolled. */
	{
		g_autoptr(VentureEntity) cy = NULL;
		g_autoptr(VentureEntity) suppression = g_object_new(VENTURE_TYPE_SUPPRESSION, "organization-id", (gint64)1,
			"email", "cy@example.com", "reason", 0, NULL);
		persist(f, suppression);
		cy = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1, "name", "Cy", "email", "cy@example.com", "tags", "vip", NULL);
		persist(f, cy);
		g_assert_cmpint(enrolled(f, f->journey, cy), ==, 0);
	}
	(void)step;
}

static VentureEntity *stage_of(Fixture *f, gint64 pipeline, const gchar *kind)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
	g_autoptr(GError) error = NULL;
	VentureEntity *row;
	venture_query_add_filter_int(query, "pipeline-id", VENTURE_FILTER_OP_EQ, pipeline, NULL);
	venture_query_add_filter_string(query, "kind", VENTURE_FILTER_OP_EQ, kind, NULL);
	/* The last open stage: a new deal already sits in the first. */
	venture_query_add_order(query, "position", VENTURE_SORT_DESCENDING, NULL);
	row = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	return row;
}
static void move(Fixture *f, VentureEntity **deal, VentureEntity *stage)
{
	g_autoptr(GError) error = NULL;
	VentureDeal *moved = venture_deal_service_move_stage(venture_database_get_deal_service(f->db), VENTURE_DEAL(*deal),
		venture_entity_get_id(stage), "Moved", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(moved);
	g_object_unref(*deal);
	*deal = VENTURE_ENTITY(moved);
}
/* Deal stage changed: a move into the named stage, through the deal
 * service like every move; other moves are not it. */
static void test_trigger_deal_stage(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) step = email_step(f, f->journey, 10, "Welcome aboard", TRUE);
	g_autoptr(VentureEntity) rule = NULL, any = NULL, open = NULL, won = NULL;
	g_autoptr(VentureEntity) other = journey(f, "Any move");
	g_autoptr(VentureEntity) other_step = email_step(f, other, 10, "Moved", TRUE);
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com");
	VentureEntity *deal = g_object_new(VENTURE_TYPE_DEAL, "organization-id", (gint64)1, "name", "Purchase",
		"contact-id", venture_entity_get_id(ada), NULL);
	gint64 pipeline = 0;
	(void)data;
	rule = g_object_new(VENTURE_TYPE_SEQUENCE_TRIGGER, "organization-id", (gint64)1, "name", "Won",
		"sequence-id", venture_entity_get_id(f->journey), "kind", 2, "deal-stage", "won", "active", TRUE, NULL);
	persist(f, rule);
	any = g_object_new(VENTURE_TYPE_SEQUENCE_TRIGGER, "organization-id", (gint64)1, "name", "Any move",
		"sequence-id", venture_entity_get_id(other), "kind", 2, "active", TRUE, NULL);
	persist(f, any);
	persist(f, deal);
	g_object_get(deal, "pipeline-id", &pipeline, NULL);
	open = stage_of(f, pipeline, "open");
	won = stage_of(f, pipeline, "won");
	move(f, &deal, open);
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 0);
	move(f, &deal, won);
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 1);
	g_assert_cmpint(enrolled(f, other, ada), >=, 1);
	g_object_unref(deal);
	(void)step; (void)other_step;
}

static void pay_invoice(Fixture *f, VentureEntity *person, const gchar *number)
{
	g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", (gint64)1, "name", number, NULL);
	g_autoptr(VentureEntity) invoice = NULL, line = NULL, payment = NULL;
	persist(f, company);
	invoice = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", (gint64)1, "number", number,
		"company-id", venture_entity_get_id(company), "contact-id", venture_entity_get_id(person), NULL);
	g_assert_true(venture_entity_set_field_from_string(invoice, "issued-at", "2026-07-01", NULL));
	g_assert_true(venture_entity_set_field_from_string(invoice, "due-at", "2026-07-01", NULL));
	persist(f, invoice);
	line = g_object_new(VENTURE_TYPE_INVOICE_LINE, "organization-id", (gint64)1,
		"invoice-id", venture_entity_get_id(invoice), "description", "Example service", "quantity", 1.0, NULL);
	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "100.00 USD", NULL));
	persist(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL); persist(f, invoice);
	payment = g_object_new(VENTURE_TYPE_PAYMENT, "organization-id", (gint64)1,
		"customer-id", venture_entity_get_id(company), "invoice-id", venture_entity_get_id(invoice), "method", "manual", NULL);
	g_assert_true(venture_entity_set_field_from_string(payment, "amount", "100.00 USD", NULL));
	g_assert_true(venture_entity_set_field_from_string(payment, "date", "2026-07-02", NULL));
	persist(f, payment);
}

/* Invoice paid comes from the business-event feed. A trigger reads only
 * events after it was made, so adding one does not enrol everybody who
 * ever paid; two triggers on one journey still enrol once per event. */
static void test_trigger_invoice(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) step = email_step(f, f->journey, 10, "Thank you for your order", TRUE);
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com");
	g_autoptr(VentureEntity) bob = contact(f, "Bob", "bob@example.com");
	g_autoptr(VentureEntity) other = journey(f, "Late journey");
	g_autoptr(VentureEntity) other_step = email_step(f, other, 10, "Late", TRUE);
	g_autoptr(VentureEntity) first = NULL, second = NULL, late = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	(void)data;
	pay_invoice(f, bob, "INV-BEFORE");
	first = trigger(f, f->journey, 4, NULL);
	second = trigger(f, f->journey, 4, NULL);
	pay_invoice(f, ada, "INV-AFTER");
	late = trigger(f, other, 4, NULL);
	g_assert_cmpint(triggers_at(f, now), ==, 1);
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 1);
	g_assert_cmpint(enrolled(f, f->journey, bob), ==, 0);
	g_assert_cmpint(enrolled(f, other, ada), ==, 0);
	g_assert_cmpint(triggers_at(f, now), ==, 0);
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 1);
	/* The sequence sweep reads the triggers first. */
	pay_invoice(f, ada, "INV-LATER");
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureEntity) row = only_enrollment(f, f->journey, ada);
		g_assert_true(venture_sequence_service_transition(f->service, 1, venture_entity_get_id(row), "exit", NULL, NULL, &error));
		g_assert_no_error(error);
		g_assert_cmpint(venture_sequence_service_run_due(f->service, 1, now, NULL, &error), >=, 0);
		g_assert_no_error(error);
	}
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 2);
	g_assert_cmpint(enrolled(f, other, ada), ==, 1);
	(void)step; (void)other_step; (void)first; (void)second; (void)late;
}

static void test_trigger_booking(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) step = email_step(f, f->journey, 10, "See you soon", TRUE);
	g_autoptr(VentureEntity) rule = trigger(f, f->journey, 5, NULL);
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com");
	g_autoptr(VentureBookingService) bookings = venture_booking_service_new(f->db);
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "calendar-owner", "active", TRUE, NULL);
	g_autoptr(VentureEntity) page = g_object_new(VENTURE_TYPE_BOOKING_PAGE, "organization-id", (gint64)1,
		"title", "Example appointment", "slug", "example", "owner", "calendar-owner", "timezone", "UTC",
		"duration-minutes", (gint64)30, "availability", "{\"mon\":\"09:00-10:00\"}", "horizon-days", (gint64)2, "active", TRUE, NULL);
	g_autoptr(GDateTime) now = at("2026-09-21T08:00:00Z");
	g_autoptr(VentureEntity) activity = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	persist(f, user); persist(f, page);
	activity = venture_booking_service_book(bookings, page, "2026-09-21T09:00:00Z", "Ada", "ada@example.com", NULL, now, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(activity);
	g_assert_cmpint(triggers_at(f, now), ==, 1);
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 1);
	(void)step; (void)rule;
}

static void quote_action(Fixture *f, VentureEntity *quote, const gchar *action)
{
	g_autoptr(VentureEntity) fresh = reread(f, quote);
	g_autoptr(VentureEntity) request = g_object_new(VENTURE_TYPE_QUOTE_ACTION,
		"organization-id", (gint64)1, "quote-id", venture_entity_get_id(fresh),
		"expected-version", venture_entity_get_version(fresh), "action", action, "accepted-by", "Example person", NULL);
	persist(f, request);
}
static VentureEntity *quote_for(Fixture *f, VentureEntity *person, const gchar *number)
{
	g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", (gint64)1, "name", number, NULL);
	g_autoptr(VentureEntity) line = NULL;
	VentureEntity *quote;
	persist(f, company);
	g_object_set(person, "company-id", venture_entity_get_id(company), NULL);
	persist(f, person);
	quote = g_object_new(VENTURE_TYPE_QUOTE, "organization-id", (gint64)1, "number", number,
		"company-id", venture_entity_get_id(company), "contact-id", venture_entity_get_id(person), "currency", "USD", NULL);
	persist(f, quote);
	line = g_object_new(VENTURE_TYPE_QUOTE_LINE, "organization-id", (gint64)1,
		"quote-id", venture_entity_get_id(quote), "description", "Example service", "quantity", (gint64)1, NULL);
	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "100.00 USD", NULL));
	persist(f, line);
	return quote;
}

/* Quote sent but not accepted after N days: read from the feed's
 * quote.sent once it is N days old, and only if the quote is still sent. */
static void test_trigger_quote(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) step = email_step(f, f->journey, 10, "Any questions about the quote?", TRUE);
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com");
	g_autoptr(VentureEntity) bob = contact(f, "Bob", "bob@example.com");
	g_autoptr(VentureEntity) waiting = NULL, accepted = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) six = g_date_time_add_days(now, 6);
	g_autoptr(GDateTime) eight = g_date_time_add_days(now, 8);
	(void)data;
	rule = g_object_new(VENTURE_TYPE_SEQUENCE_TRIGGER, "organization-id", (gint64)1, "name", "Quote waiting",
		"sequence-id", venture_entity_get_id(f->journey), "kind", 3, "days", (gint64)7, "active", TRUE, NULL);
	persist(f, rule);
	waiting = quote_for(f, ada, "Q-WAIT");
	accepted = quote_for(f, bob, "Q-YES");
	quote_action(f, waiting, "send");
	quote_action(f, accepted, "send");
	quote_action(f, accepted, "accept");
	g_assert_cmpint(triggers_at(f, now), ==, 0);
	g_assert_cmpint(triggers_at(f, six), ==, 0);
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 0);
	g_assert_cmpint(triggers_at(f, eight), ==, 1);
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 1);
	g_assert_cmpint(enrolled(f, f->journey, bob), ==, 0);
	g_assert_cmpint(triggers_at(f, eight), ==, 0);
	(void)step;
}

/* Form submitted: a response that names a contact (a personal link, or
 * the contact its lead became) enrols that contact, once per response,
 * and a trigger naming a form ignores other forms. */
static VentureEntity *live_form(Fixture *f, const gchar *token)
{
	VentureEntity *form = VENTURE_ENTITY(venture_form_new());
	g_autoptr(VentureEntity) field = VENTURE_ENTITY(venture_form_field_new());
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) version = NULL;
	venture_entity_set_organization_id(form, 1);
	g_object_set(form, "name", token, "title", token, "public-token", token, "state", VENTURE_FORM_LIVE, NULL);
	persist(f, form);
	venture_entity_set_organization_id(field, 1);
	g_object_set(field, "form-id", venture_entity_get_id(form), "key", "message", "label", "Message",
		"kind", VENTURE_FORM_FIELD_SHORT_TEXT, "required", FALSE, "position", (gint64)1, NULL);
	persist(f, field);
	version = venture_forms_publish(f->db, form, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(version);
	{
		g_autoptr(VentureEntity) fresh = reread(f, form);
		g_object_unref(form);
		form = g_steal_pointer(&fresh);
	}
	return form;
}
static void submit_personal(Fixture *f, VentureEntity *form, VentureEntity *person)
{
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) expires = g_date_time_add_days(now, 1);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = venture_forms_personal_link(f->db, form, person, "https://forms.example.com", expires, now, &error);
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(VentureEntity) submission = NULL;
	VentureFormsOutcome outcome;
	g_assert_no_error(error);
	g_assert_nonnull(url);
	venture_forms_answers_add(answers, "message", "Hello");
	venture_forms_answers_add(answers, "_vf_personal", strstr(url, "personal=") + strlen("personal="));
	g_assert_true(venture_forms_submit(f->db, form, answers, NULL, now, &outcome, &submission, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_ACCEPTED);
}
static void test_trigger_form(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) step = email_step(f, f->journey, 10, "Thanks for writing", TRUE);
	g_autoptr(VentureEntity) wanted = live_form(f, "journey-form");
	g_autoptr(VentureEntity) other = live_form(f, "other-form");
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com");
	g_autoptr(VentureEntity) bob = contact(f, "Bob", "bob@example.com");
	(void)data;
	rule = g_object_new(VENTURE_TYPE_SEQUENCE_TRIGGER, "organization-id", (gint64)1, "name", "Wrote in",
		"sequence-id", venture_entity_get_id(f->journey), "kind", 0, "form-id", venture_entity_get_id(wanted), "active", TRUE, NULL);
	persist(f, rule);
	submit_personal(f, wanted, ada);
	submit_personal(f, other, bob);
	g_assert_cmpint(enrolled(f, f->journey, ada), ==, 1);
	g_assert_cmpint(enrolled(f, f->journey, bob), ==, 0);
	(void)step;
}

/* ------------------------------------------------------------------ */
/* A marketing journey (one with a topic) honours the recipient's
 * preference centre: an email on a topic they turned off, or past the
 * frequency they asked for, is skipped with the reason, and the journey
 * goes on. A journey with no topic is one-to-one outreach: only the
 * stop-everything suppression applies, as before. */
static void test_journey_preferences(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) news = g_object_new(VENTURE_TYPE_MARKETING_TOPIC, "organization-id", (gint64)1, "name", "News", "active", TRUE, NULL);
	g_autoptr(VentureEntity) first = NULL, second = NULL, ada = NULL, bob = NULL, a = NULL, b = NULL, delivery = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *off = NULL, *why = NULL;
	gint state = -1;
	(void)data;
	persist(f, news);
	g_object_set(f->journey, "topic-id", venture_entity_get_id(news), NULL);
	persist(f, f->journey);
	first = email_step(f, f->journey, 10, "First", FALSE);
	second = email_step(f, f->journey, 20, "Second", TRUE);
	g_object_set(second, "delay-days", (gint64)1, NULL); persist(f, second);
	ada = contact(f, "Ada", "ada@example.com");
	bob = contact(f, "Bob", "bob@example.com");
	off = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(news));
	g_assert_true(venture_marketing_service_record_preferences(venture_marketing_service_get(f->db), 1, "ada@example.com",
		off, "any", "Asked by phone", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_marketing_service_record_preferences(venture_marketing_service_get(f->db), 1, "bob@example.com",
		"", "weekly", "Asked by phone", NULL, &error));
	g_assert_no_error(error);
	a = enrol(f, f->journey, ada, "2026-09-14T10:00:00Z");
	b = enrol(f, f->journey, bob, "2026-09-14T10:00:00Z");
	g_assert_cmpint(sweep(f, "2026-09-14T10:00:00Z"), ==, 2);
	g_assert_cmpint(sweep(f, "2026-09-15T10:00:00Z"), ==, 2);
	delivery = delivery_for(f, a, first);
	g_object_get(delivery, "state", &state, "error", &why, NULL);
	g_assert_cmpint(state, ==, 3);
	g_assert_nonnull(strstr(why, "topic"));
	g_clear_object(&delivery); g_clear_pointer(&why, g_free);
	g_assert_cmpint(delivery_state(f, b, first), ==, 0);
	delivery = delivery_for(f, b, second);
	g_object_get(delivery, "state", &state, "error", &why, NULL);
	g_assert_cmpint(state, ==, 3);
	g_assert_nonnull(strstr(why, "week"));
	g_assert_cmpint(status_of(f, a), ==, 2);
	g_assert_cmpint(status_of(f, b), ==, 2);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/journeys/branch/opened", Fixture, NULL, setup, test_branch_opened, teardown);
	g_test_add("/journeys/branch/clicked-replied", Fixture, NULL, setup, test_branch_clicked_replied, teardown);
	g_test_add("/journeys/branch/validation", Fixture, NULL, setup, test_branch_validation, teardown);
	g_test_add("/journeys/channel-hook", Fixture, NULL, setup, test_channel_hook, teardown);
	g_test_add("/journeys/triggers/tag", Fixture, NULL, setup, test_trigger_tag, teardown);
	g_test_add("/journeys/triggers/deal-stage", Fixture, NULL, setup, test_trigger_deal_stage, teardown);
	g_test_add("/journeys/triggers/invoice", Fixture, NULL, setup, test_trigger_invoice, teardown);
	g_test_add("/journeys/triggers/booking", Fixture, NULL, setup, test_trigger_booking, teardown);
	g_test_add("/journeys/triggers/quote", Fixture, NULL, setup, test_trigger_quote, teardown);
	g_test_add("/journeys/triggers/form", Fixture, NULL, setup, test_trigger_form, teardown);
	g_test_add("/journeys/preferences", Fixture, NULL, setup, test_journey_preferences, teardown);
	return g_test_run();
}
