/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Audiences: tags on contacts, behaviour segments, the preference centre,
 * A/B tests with an automatic winner, recipient local-time sends and the
 * resend to non-openers. Each extends the one campaign pipeline (frozen
 * preview, approval, bounded delivery) rather than standing beside it, so
 * every test ends at what the outbox would actually send. No network: mail
 * goes to the log mailer and HTTP stays on loopback. */
#include <venture.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"
#include <libsoup/soup.h>

typedef struct {
	VentureDatabase *db;
	VentureLogMailer *mailer;
	VentureMailOutbox *outbox;
	VentureMarketingService *service;
	VentureEntity *list;
	VentureEntity *send;
} Fixture;

static void persist(Fixture *f, gpointer row)
{
	g_autoptr(GError) error = NULL;
	gboolean saved = venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(saved);
}
static VentureEntity *reread(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *fresh = venture_database_get(f->db, G_OBJECT_TYPE(row), venture_entity_get_id(row), &error);
	g_assert_no_error(error);
	g_assert_nonnull(fresh);
	return fresh;
}
static VentureEntity *contact(Fixture *f, const gchar *name, const gchar *email, const gchar *tags)
{
	VentureEntity *row = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1, "name", name,
		"email", email, "tags", tags, NULL);
	persist(f, row);
	return row;
}
static void member(Fixture *f, VentureEntity *list, VentureEntity *row)
{
	g_autoptr(VentureEntity) entry = g_object_new(VENTURE_TYPE_MARKETING_MEMBER, "organization-id", (gint64)1,
		"list-id", venture_entity_get_id(list), "contact-id", venture_entity_get_id(row), NULL);
	persist(f, entry);
}
static void permit(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *key = g_strdup_printf("permission-%s", venture_entity_get_uuid(row));
	g_autoptr(VentureMarketingConsent) consent = venture_marketing_service_consent(f->service, 1, row,
		"Signed preference form", "I request this organization's marketing email", key, now, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(consent);
}
/* A contact on the fixture's list, with permission. */
static VentureEntity *reader(Fixture *f, const gchar *name, const gchar *email)
{
	VentureEntity *row = contact(f, name, email, NULL);
	member(f, f->list, row);
	permit(f, row);
	return row;
}
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->service = venture_marketing_service_get(f->db);
	/* Every open in these tests is a person's unless a user agent says
	 * otherwise; the timing rule has its own tests in test-deliverability. */
	venture_marketing_service_set_machine_seconds(f->service, 0);
	f->mailer = venture_log_mailer_new();
	f->outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(f->mailer));
	f->list = g_object_new(VENTURE_TYPE_MARKETING_LIST, "organization-id", (gint64)1, "name", "Readers", NULL);
	persist(f, f->list);
	f->send = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1,
		"name", "Autumn", "list-id", venture_entity_get_id(f->list), "subject", "Hello {name}",
		"text-body", "Read https://example.com/news",
		"html-body", "<p>Hello {name}</p><a href=\"https://example.com/news\">News</a>",
		"interval-seconds", (gint64)1, NULL);
}
static void teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->list); g_clear_object(&f->send);
	g_clear_object(&f->outbox); g_clear_object(&f->mailer);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
}
static void track(Fixture *f)
{
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GError) error = NULL;
	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, 1, &error);
	g_assert_no_error(error);
	g_object_set(organization, "marketing-tracking", TRUE, NULL);
	persist(f, organization);
	g_object_set(f->send, "tracking", TRUE, NULL);
}
static GPtrArray *recipients_of(Fixture *f, VentureEntity *send)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_RECIPIENT);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, 1);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "send-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(send), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}
static VentureEntity *recipient_for(Fixture *f, VentureEntity *send, const gchar *email)
{
	g_autoptr(GPtrArray) rows = recipients_of(f, send);
	guint i;
	for (i = 0; i < rows->len; i++) {
		g_autofree gchar *address = NULL;
		g_object_get(g_ptr_array_index(rows, i), "email", &address, NULL);
		if (!g_strcmp0(address, email)) return g_object_ref(g_ptr_array_index(rows, i));
	}
	g_error("no recipient %s", email);
	return NULL;
}
static gchar *field_text(VentureEntity *row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}
static void assert_reason(Fixture *f, VentureEntity *send, const gchar *email, const gchar *reason)
{
	g_autoptr(VentureEntity) row = recipient_for(f, send, email);
	g_autofree gchar *eligibility = field_text(row, "eligibility");
	g_assert_cmpstr(eligibility, ==, reason);
}
static VentureEntity *preview(Fixture *f, VentureEntity *send)
{
	g_autoptr(GError) error = NULL;
	VentureMarketingSend *row = venture_marketing_service_preview(f->service, 1,
		venture_entity_get_id(send), "https://venture.example.com", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	return VENTURE_ENTITY(row);
}
static VentureEntity *preview_and_approve(Fixture *f, VentureEntity *send)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) previewed = preview(f, send);
	g_assert_true(venture_marketing_service_transition(f->service, 1, venture_entity_get_id(previewed), "approve", NULL, &error));
	g_assert_no_error(error);
	return reread(f, previewed);
}
/* One run at @now and whatever it queued delivered; returns how many queued. */
static gint run_at(Fixture *f, VentureEntity *send, GDateTime *now)
{
	g_autoptr(GError) error = NULL;
	gint queued = venture_marketing_service_run(f->service, 1, venture_entity_get_id(send), 100, now, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(queued, >=, 0);
	if (queued > 0) {
		g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, 1, 10, now, NULL, &error), >=, 0);
		g_assert_no_error(error);
	}
	return queued;
}
static gchar *tracking_token(VentureEntity *recipient)
{
	g_autofree gchar *html = field_text(recipient, "private-html-body");
	const gchar *pixel = strstr(html, "/marketing/t/o/");
	g_assert_nonnull(pixel);
	return g_strndup(pixel + strlen("/marketing/t/o/"), 64);
}
static gchar *unsubscribe_token(VentureEntity *recipient)
{
	g_autofree gchar *url = field_text(recipient, "unsubscribe-url");
	return g_strdup(strrchr(url, '/') + 1);
}
static void open_as(Fixture *f, VentureEntity *send, const gchar *email, const gchar *agent)
{
	g_autoptr(VentureEntity) row = recipient_for(f, send, email);
	g_autofree gchar *token = tracking_token(row);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_marketing_service_record_open(f->service, token, agent, now, &error));
	g_assert_no_error(error);
}
static void click_as(Fixture *f, VentureEntity *send, const gchar *email)
{
	g_autoptr(VentureEntity) row = recipient_for(f, send, email);
	g_autofree gchar *token = tracking_token(row);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = venture_marketing_service_record_click(f->service, token, 1, "Mozilla/5.0", now, &error);
	g_assert_no_error(error);
	g_assert_nonnull(url);
}
/* Sends the fixture's draft to everybody now, one run per recipient. */
static void send_all(Fixture *f)
{
	g_autoptr(GDateTime) now = venture_time_now();
	gint guard = 0;
	g_autoptr(VentureEntity) approved = NULL;
	persist(f, f->send);
	approved = preview_and_approve(f, f->send);
	g_set_object(&f->send, approved);
	while (guard++ < 50) {
		g_autoptr(GDateTime) later = g_date_time_add_seconds(now, guard * 2);
		if (run_at(f, f->send, later) == 0) {
			g_autoptr(VentureEntity) fresh = reread(f, f->send);
			gint state = 0;
			g_object_get(fresh, "state", &state, NULL);
			if (state == 5) break;
		}
	}
}
static GPtrArray *members_at(Fixture *f, VentureEntity *list, GDateTime *now)
{
	g_autoptr(GError) error = NULL;
	GPtrArray *rows = venture_marketing_service_segment_members(f->service, 1, venture_entity_get_id(list), now, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rows);
	return rows;
}
static gboolean has_member(GPtrArray *rows, VentureEntity *row)
{
	guint i;
	for (i = 0; i < rows->len; i++)
		if (venture_entity_get_id(g_ptr_array_index(rows, i)) == venture_entity_get_id(row)) return TRUE;
	return FALSE;
}
static VentureEntity *segment(Fixture *f, const gchar *filters)
{
	VentureEntity *list = g_object_new(VENTURE_TYPE_MARKETING_LIST, "organization-id", (gint64)1,
		"name", filters, "mode", 1, "target", 0, "filters", filters, NULL);
	persist(f, list);
	return list;
}

/* ------------------------------------------------------------------ */
/* 1. Tags: the model is in tests/test-venture-tags.c. */

/* The tag operations are ordinary actions on a contact, so the API, the
 * CLI, the page and the assistant all reach them, audited like a save. */
static void test_tag_actions(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com", "early");
	g_autoptr(VentureEntity) result = NULL, fresh = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *tags = NULL;
	(void)data;
	{
		g_autoptr(JsonNode) values = json_from_string("{\"tag\":\"VIP\"}", NULL);
		g_autoptr(GHashTable) parameters = venture_action_parameters_from_json(values, &error);
		g_assert_no_error(error);
		result = venture_action_registry_perform(venture_database_get_action_registry(f->db), "contact",
			venture_entity_get_id(ada), "add_tag", parameters, NULL, VENTURE_USER_ROLE_EDITOR, &error);
		g_assert_no_error(error);
		g_assert_nonnull(result);
	}
	fresh = reread(f, ada);
	tags = field_text(fresh, "tags");
	g_assert_cmpstr(tags, ==, "early, VIP");
	g_clear_object(&result); g_clear_object(&fresh); g_clear_pointer(&tags, g_free);
	{
		g_autoptr(JsonNode) values = json_from_string("{\"tag\":\"Early\"}", NULL);
		g_autoptr(GHashTable) parameters = venture_action_parameters_from_json(values, &error);
		g_assert_no_error(error);
		result = venture_action_registry_perform(venture_database_get_action_registry(f->db), "contact",
			venture_entity_get_id(ada), "remove_tag", parameters, NULL, VENTURE_USER_ROLE_EDITOR, &error);
		g_assert_no_error(error);
		g_assert_nonnull(result);
	}
	fresh = reread(f, ada);
	tags = field_text(fresh, "tags");
	g_assert_cmpstr(tags, ==, "VIP");
	{
		g_autoptr(JsonNode) values = json_from_string("{\"tag\":\"a,b\"}", NULL);
		g_autoptr(GHashTable) parameters = venture_action_parameters_from_json(values, &error);
		g_assert_no_error(error);
		g_clear_object(&result);
		result = venture_action_registry_perform(venture_database_get_action_registry(f->db), "contact",
			venture_entity_get_id(ada), "add_tag", parameters, NULL, VENTURE_USER_ROLE_EDITOR, &error);
		g_assert_null(result);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}
}

/* A saved search can ask for a tag; the narrowing is a whole tag, any
 * capitals, and it composes with the ordinary field filters. */
static void test_tag_segment(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ada = contact(f, "Ada", "ada@example.com", "early, VIP");
	g_autoptr(VentureEntity) bob = contact(f, "Bob", "bob@example.com", "vipers");
	g_autoptr(VentureEntity) cy = contact(f, "Cy", "cy@example.com", "vip");
	g_autoptr(VentureEntity) list = NULL, both = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	(void)data;
	g_object_set(cy, "source", "conference", NULL); persist(f, cy);
	list = segment(f, "tag=vip");
	rows = members_at(f, list, now);
	g_assert_cmpuint(rows->len, ==, 2);
	g_assert_true(has_member(rows, ada));
	g_assert_false(has_member(rows, bob));
	g_assert_true(has_member(rows, cy));
	g_clear_pointer(&rows, g_ptr_array_unref);
	both = segment(f, "tag=VIP&source=conference");
	rows = members_at(f, both, now);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_true(has_member(rows, cy));
}

/* Behaviour segments: opened, clicked, replied, bought and booked within
 * N days. Opens and clicks are people's only -- a scanner's open is lane
 * 3's machine evidence and never puts anybody in "opened". */
static void test_behaviour_segments(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ada = NULL, bob = NULL, cy = NULL, dee = NULL, eve = NULL;
	g_autoptr(VentureEntity) opened = NULL, clicked = NULL, replied = NULL, bought = NULL, booked = NULL, mixed = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) soon = g_date_time_add_days(now, 5);
	g_autoptr(GDateTime) later = g_date_time_add_days(now, 40);
	g_autoptr(GError) error = NULL;
	(void)data;
	track(f);
	ada = reader(f, "Ada", "ada@example.com");
	bob = reader(f, "Bob", "bob@example.com");
	cy = reader(f, "Cy", "cy@example.com");
	dee = reader(f, "Dee", "dee@example.com");
	eve = contact(f, "Eve", "eve@example.com", NULL);
	send_all(f);
	open_as(f, f->send, "ada@example.com", "Mozilla/5.0");
	open_as(f, f->send, "bob@example.com", "Barracuda link scanner");
	click_as(f, f->send, "cy@example.com");
	{
		g_autoptr(VentureEntity) reply = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", (gint64)1,
			"contact-id", venture_entity_get_id(dee), "kind", VENTURE_INTERACTION_KIND_EMAIL, "outbound", FALSE,
			"subject", "Re: Hello", "occurred-at", now, NULL);
		persist(f, reply);
	}
	/* Bought: a paid invoice addressed to Eve. */
	{
		g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", (gint64)1, "name", "Example Co", NULL);
		g_autoptr(VentureEntity) invoice = NULL, line = NULL, payment = NULL;
		g_autoptr(GDateTime) yesterday = g_date_time_add_days(now, -1);
		g_autofree gchar *today = g_date_time_format(yesterday, "%Y-%m-%d");
		persist(f, company);
		invoice = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", (gint64)1, "number", "INV-AUD-1",
			"company-id", venture_entity_get_id(company), "contact-id", venture_entity_get_id(eve), NULL);
		g_assert_true(venture_entity_set_field_from_string(invoice, "issued-at", today, NULL));
		g_assert_true(venture_entity_set_field_from_string(invoice, "due-at", today, NULL));
		persist(f, invoice);
		line = g_object_new(VENTURE_TYPE_INVOICE_LINE, "organization-id", (gint64)1,
			"invoice-id", venture_entity_get_id(invoice), "description", "Example service", "quantity", 1.0, NULL);
		g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "100.00 USD", NULL));
		persist(f, line);
		g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL); persist(f, invoice);
		payment = g_object_new(VENTURE_TYPE_PAYMENT, "organization-id", (gint64)1,
			"customer-id", venture_entity_get_id(company), "invoice-id", venture_entity_get_id(invoice), "method", "manual", NULL);
		g_assert_true(venture_entity_set_field_from_string(payment, "amount", "100.00 USD", NULL));
		g_assert_true(venture_entity_set_field_from_string(payment, "date", today, NULL));
		persist(f, payment);
	}
	/* Booked: a confirmed booking by Bob's address. */
	{
		g_autoptr(VentureBookingService) bookings = venture_booking_service_new(f->db);
		g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "calendar-owner", "active", TRUE, NULL);
		g_autoptr(VentureEntity) page = g_object_new(VENTURE_TYPE_BOOKING_PAGE, "organization-id", (gint64)1,
			"title", "Example appointment", "slug", "example", "owner", "calendar-owner", "timezone", "UTC",
			"duration-minutes", (gint64)30, "availability", "{\"mon\":\"00:00-23:30\",\"tue\":\"00:00-23:30\",\"wed\":\"00:00-23:30\",\"thu\":\"00:00-23:30\",\"fri\":\"00:00-23:30\",\"sat\":\"00:00-23:30\",\"sun\":\"00:00-23:30\"}",
			"horizon-days", (gint64)7, "active", TRUE, NULL);
		g_autoptr(GDateTime) slot_day = g_date_time_add_days(now, 2);
		g_autofree gchar *slot = g_date_time_format(slot_day, "%Y-%m-%dT10:00:00Z");
		g_autoptr(VentureEntity) activity = NULL;
		persist(f, user); persist(f, page);
		activity = venture_booking_service_book(bookings, page, slot, "Bob", "bob@example.com", NULL, now, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(activity);
	}
	opened = segment(f, "opened_within_days=30");
	clicked = segment(f, "clicked_within_days=30");
	replied = segment(f, "replied_within_days=30");
	bought = segment(f, "bought_within_days=30");
	booked = segment(f, "booked_within_days=30");
	mixed = segment(f, "opened_within_days=30&tag=none-has-this");

	rows = members_at(f, opened, soon);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_true(has_member(rows, ada));
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = members_at(f, opened, later);
	g_assert_cmpuint(rows->len, ==, 0);
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = members_at(f, clicked, soon);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_true(has_member(rows, cy));
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = members_at(f, replied, soon);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_true(has_member(rows, dee));
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = members_at(f, bought, soon);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_true(has_member(rows, eve));
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = members_at(f, bought, later);
	g_assert_cmpuint(rows->len, ==, 0);
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = members_at(f, booked, soon);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_true(has_member(rows, bob));
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = members_at(f, mixed, soon);
	g_assert_cmpuint(rows->len, ==, 0);
	g_clear_pointer(&rows, g_ptr_array_unref);
	/* Behaviour is about people: a company segment cannot ask it, and a
	 * window must be a whole number of days. */
	{
		g_autoptr(VentureEntity) wrong = g_object_new(VENTURE_TYPE_MARKETING_LIST, "organization-id", (gint64)1,
			"name", "Companies that opened", "mode", 1, "target", 1, "filters", "opened_within_days=30", NULL);
		g_autoptr(VentureEntity) bad = g_object_new(VENTURE_TYPE_MARKETING_LIST, "organization-id", (gint64)1,
			"name", "Bad window", "mode", 1, "target", 0, "filters", "opened_within_days=soon", NULL);
		g_assert_false(venture_database_save(f->db, wrong, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
		g_assert_false(venture_database_save(f->db, bad, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}
}

/* A behaviour segment is an audience like any other: the preview freezes
 * it, and permission and suppression are still decided per recipient. */
static void test_behaviour_audience(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ada = NULL, bob = NULL, list = NULL, follow = NULL, previewed = NULL;
	(void)data;
	track(f);
	ada = reader(f, "Ada", "ada@example.com");
	bob = reader(f, "Bob", "bob@example.com");
	send_all(f);
	open_as(f, f->send, "ada@example.com", "Mozilla/5.0");
	open_as(f, f->send, "bob@example.com", "Mozilla/5.0");
	{
		g_autoptr(VentureEntity) row = recipient_for(f, f->send, "bob@example.com");
		g_autofree gchar *token = unsubscribe_token(row);
		g_autoptr(GError) error = NULL;
		g_assert_true(venture_marketing_service_unsubscribe(f->service, token, &error));
		g_assert_no_error(error);
	}
	list = segment(f, "opened_within_days=7");
	follow = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1, "name", "For readers",
		"list-id", venture_entity_get_id(list), "subject", "Thanks", "text-body", "Thanks for reading", NULL);
	persist(f, follow);
	previewed = preview(f, follow);
	assert_reason(f, previewed, "ada@example.com", "eligible");
	assert_reason(f, previewed, "bob@example.com", "suppressed");
}

/* ------------------------------------------------------------------ */
/* 3. The preference centre. The page a recipient's link opens lets them
 * turn topics off, ask for less, or stop everything, and every campaign
 * after that honours it -- at preview and again just before the run
 * queues the message. */
static VentureEntity *topic(Fixture *f, const gchar *name)
{
	VentureEntity *row = g_object_new(VENTURE_TYPE_MARKETING_TOPIC, "organization-id", (gint64)1,
		"name", name, "active", TRUE, NULL);
	persist(f, row);
	return row;
}
static void test_preferences(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) news = topic(f, "Product news");
	g_autoptr(VentureEntity) offers = topic(f, "Offers");
	g_autoptr(VentureEntity) ada = NULL, bob = NULL, cy = NULL, stored = NULL;
	g_autoptr(VentureEntity) offer_send = NULL, news_send = NULL, previewed = NULL, approved = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ada_token = NULL, *bob_token = NULL, *cy_token = NULL, *off = NULL, *topics_off = NULL;
	gint frequency = -1;
	(void)data;
	ada = reader(f, "Ada", "ada@example.com");
	bob = reader(f, "Bob", "bob@example.com");
	cy = reader(f, "Cy", "cy@example.com");
	send_all(f);
	ada_token = unsubscribe_token(recipient_for(f, f->send, "ada@example.com"));
	bob_token = unsubscribe_token(recipient_for(f, f->send, "bob@example.com"));
	cy_token = unsubscribe_token(recipient_for(f, f->send, "cy@example.com"));

	/* Ada turns offers off; Bob asks for one a month; Cy stops all. */
	off = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(offers));
	g_assert_true(venture_marketing_service_set_preferences(f->service, ada_token, off, "any", &error));
	g_assert_no_error(error);
	g_assert_true(venture_marketing_service_set_preferences(f->service, bob_token, "", "monthly", &error));
	g_assert_no_error(error);
	g_assert_true(venture_marketing_service_unsubscribe(f->service, cy_token, &error));
	g_assert_no_error(error);
	/* Unknown capability: nothing recorded, an ordinary not found. */
	g_assert_false(venture_marketing_service_set_preferences(f->service,
		"0000000000000000000000000000000000000000000000000000000000000000", off, "any", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	/* A topic from another organization or a made-up frequency is refused. */
	g_assert_false(venture_marketing_service_set_preferences(f->service, ada_token, "99999", "any", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(venture_marketing_service_set_preferences(f->service, ada_token, off, "hourly", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	stored = venture_marketing_service_preferences_for(f->service, 1, "ada@example.com", &error);
	g_assert_no_error(error);
	g_assert_nonnull(stored);
	topics_off = field_text(stored, "topics-off");
	g_assert_cmpstr(topics_off, ==, off);
	g_object_get(stored, "frequency", &frequency, NULL);
	g_assert_cmpint(frequency, ==, 0);
	/* A preference is the person's: generic writes cannot make or change one. */
	g_object_set(stored, "topics-off", "", NULL);
	g_assert_false(venture_database_save(f->db, stored, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* An offers campaign: Ada is left out for the topic, Bob for the
	 * frequency (he had one today), Cy is suppressed. */
	offer_send = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1, "name", "Offer",
		"list-id", venture_entity_get_id(f->list), "subject", "An offer", "text-body", "Ten percent off",
		"topic-id", venture_entity_get_id(offers), "interval-seconds", (gint64)1, NULL);
	persist(f, offer_send);
	previewed = preview(f, offer_send);
	assert_reason(f, previewed, "ada@example.com", "topic_turned_off");
	assert_reason(f, previewed, "bob@example.com", "frequency");
	assert_reason(f, previewed, "cy@example.com", "suppressed");
	g_clear_object(&previewed);

	/* A news campaign reaches Ada; she turns news off after approval and
	 * the run cancels her rather than sending. */
	news_send = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1, "name", "News",
		"list-id", venture_entity_get_id(f->list), "subject", "News", "text-body", "What is new",
		"topic-id", venture_entity_get_id(news), "interval-seconds", (gint64)1, NULL);
	persist(f, news_send);
	approved = preview_and_approve(f, news_send);
	assert_reason(f, approved, "ada@example.com", "eligible");
	{
		g_autofree gchar *both = g_strdup_printf("%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT,
			venture_entity_get_id(offers), venture_entity_get_id(news));
		g_autoptr(GDateTime) now = venture_time_now();
		g_autoptr(VentureEntity) row = NULL;
		g_autofree gchar *state = NULL;
		g_assert_true(venture_marketing_service_set_preferences(f->service, ada_token, both, "any", &error));
		g_assert_no_error(error);
		g_assert_cmpint(run_at(f, approved, now), ==, 0);
		row = recipient_for(f, approved, "ada@example.com");
		state = field_text(row, "state");
		g_assert_cmpstr(state, ==, "cancelled");
	}
	/* An operator records what somebody asked for on the phone. */
	g_assert_true(venture_marketing_service_record_preferences(f->service, 1, "bob@example.com", "", "weekly",
		"Asked on the phone", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&stored);
	stored = venture_marketing_service_preferences_for(f->service, 1, "bob@example.com", &error);
	g_assert_no_error(error);
	g_object_get(stored, "frequency", &frequency, NULL);
	g_assert_cmpint(frequency, ==, 1);
}

/* ------------------------------------------------------------------ */
/* 6. A/B tests. Assignment is a pure function of the send and the
 * addresses, so a re-preview or a restart cannot move anybody between
 * arms; the split is exact and the arms differ by at most one. */
static void test_ab_assignment(void)
{
	const gchar *emails[] = { "a@example.com", "b@example.com", "c@example.com", "d@example.com", "e@example.com",
		"f@example.com", "g@example.com", "h@example.com", "i@example.com", "j@example.com", NULL };
	const gchar *shuffled[] = { "j@example.com", "c@example.com", "a@example.com", "h@example.com", "e@example.com",
		"f@example.com", "b@example.com", "d@example.com", "i@example.com", "g@example.com", NULL };
	g_auto(GStrv) first = venture_marketing_ab_assign("11111111-2222-3333-4444-555555555555", emails, 40);
	g_auto(GStrv) second = venture_marketing_ab_assign("11111111-2222-3333-4444-555555555555", emails, 40);
	g_auto(GStrv) moved = venture_marketing_ab_assign("11111111-2222-3333-4444-555555555555", shuffled, 40);
	g_auto(GStrv) other = venture_marketing_ab_assign("99999999-2222-3333-4444-555555555555", emails, 40);
	g_auto(GStrv) everyone = venture_marketing_ab_assign("11111111-2222-3333-4444-555555555555", emails, 100);
	guint i, j, a = 0, b = 0, rest = 0, differ = 0;
	g_assert_nonnull(first);
	g_assert_cmpuint(g_strv_length(first), ==, 10);
	for (i = 0; i < 10; i++) {
		g_assert_cmpstr(first[i], ==, second[i]);
		if (!g_strcmp0(first[i], "a")) a++;
		else if (!g_strcmp0(first[i], "b")) b++;
		else if (!g_strcmp0(first[i], "rest")) rest++;
		else g_assert_not_reached();
		if (g_strcmp0(first[i], other[i])) differ++;
		/* The same address gets the same arm whatever order it came in. */
		for (j = 0; j < 10; j++)
			if (!g_strcmp0(emails[i], shuffled[j])) g_assert_cmpstr(first[i], ==, moved[j]);
	}
	g_assert_cmpuint(a, ==, 2);
	g_assert_cmpuint(b, ==, 2);
	g_assert_cmpuint(rest, ==, 6);
	g_assert_cmpuint(differ, >, 0);
	for (i = 0, a = 0; i < 10; i++) {
		g_assert_cmpstr(everyone[i], !=, "rest");
		if (!g_strcmp0(everyone[i], "a")) a++;
	}
	g_assert_cmpuint(a, ==, 5);
}

static gint64 count_state(Fixture *f, VentureEntity *send, const gchar *field, const gchar *value)
{
	g_autoptr(GPtrArray) rows = recipients_of(f, send);
	gint64 count = 0;
	guint i;
	for (i = 0; i < rows->len; i++) {
		g_autofree gchar *text = field_text(g_ptr_array_index(rows, i), field);
		if (!g_strcmp0(text, value)) count++;
	}
	return count;
}
static gint64 queued_in(Fixture *f, VentureEntity *send, const gchar *arm)
{
	g_autoptr(GPtrArray) rows = recipients_of(f, send);
	gint64 count = 0;
	guint i;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *variant = field_text(row, "variant");
		gint64 mail = 0;
		g_object_get(row, "mail-id", &mail, NULL);
		if (mail && !g_strcmp0(variant, arm)) count++;
	}
	return count;
}

/* The test arms go first; the rest wait for the winner, which is picked
 * by people's opens once the wait is over, and then the rest receive the
 * winning subject. Both arms are reported. */
static void test_ab_winner(Fixture *f, gconstpointer data)
{
	const gchar *names[] = { "Ada", "Bob", "Cy", "Dee", "Eve", "Fay", "Gus", "Hal" };
	g_autoptr(GPtrArray) people = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureEntity) approved = NULL, fresh = NULL;
	g_autoptr(GDateTime) start = venture_time_now();
	g_autoptr(VentureMarketingSendReport) report = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *winner = NULL;
	guint i, sent;
	gint64 tick = 0;
	(void)data;
	track(f);
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		g_autofree gchar *lower = g_ascii_strdown(names[i], -1);
		g_autofree gchar *email = g_strdup_printf("%s@example.com", lower);
		g_ptr_array_add(people, reader(f, names[i], email));
	}
	g_object_set(f->send, "subject", "First subject", "variant-subject", "Second subject",
		"test-percent", (gint64)50, "winner-metric", 0, "winner-wait-hours", (gint64)2, NULL);
	persist(f, f->send);
	approved = preview_and_approve(f, f->send);
	g_assert_cmpint(count_state(f, approved, "variant", "a"), ==, 2);
	g_assert_cmpint(count_state(f, approved, "variant", "b"), ==, 2);
	g_assert_cmpint(count_state(f, approved, "variant", "rest"), ==, 4);
	/* Each B arm recipient's frozen message is B's. */
	rows = recipients_of(f, approved);
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *variant = field_text(row, "variant"), *subject = field_text(row, "subject");
		if (!g_strcmp0(variant, "b")) g_assert_cmpstr(subject, ==, "Second subject");
		else g_assert_cmpstr(subject, ==, "First subject");
	}
	/* The test arms are sent; the rest wait. */
	for (sent = 0; sent < 10; sent++) {
		g_autoptr(GDateTime) now = g_date_time_add_seconds(start, ++tick * 5);
		if (run_at(f, approved, now) == 0) break;
	}
	g_assert_cmpint(queued_in(f, approved, "a"), ==, 2);
	g_assert_cmpint(queued_in(f, approved, "b"), ==, 2);
	g_assert_cmpint(queued_in(f, approved, "rest"), ==, 0);
	/* People open B, a scanner opens A twice: machines do not vote. */
	rows = recipients_of(f, approved);
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *variant = field_text(row, "variant"), *email = field_text(row, "email");
		if (!g_strcmp0(variant, "b")) open_as(f, approved, email, "Mozilla/5.0");
		if (!g_strcmp0(variant, "a")) open_as(f, approved, email, "Mimecast scanner");
	}
	/* Just before the wait is over nothing more goes. */
	{
		g_autoptr(GDateTime) early = g_date_time_add_seconds(start, tick * 5 + 2 * 3600 - 30);
		g_assert_cmpint(run_at(f, approved, early), ==, 0);
		fresh = reread(f, approved);
		winner = field_text(fresh, "winner");
		g_assert_true(winner == NULL || *winner == '\0');
		g_clear_pointer(&winner, g_free); g_clear_object(&fresh);
	}
	for (sent = 0; sent < 10; sent++) {
		g_autoptr(GDateTime) now = g_date_time_add_seconds(start, tick * 5 + 2 * 3600 + 30 + sent * 5);
		if (run_at(f, approved, now) == 0) break;
	}
	fresh = reread(f, approved);
	winner = field_text(fresh, "winner");
	g_assert_cmpstr(winner, ==, "b");
	g_assert_cmpint(queued_in(f, approved, "rest"), ==, 4);
	rows = recipients_of(f, approved);
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *variant = field_text(row, "variant"), *subject = field_text(row, "subject");
		if (!g_strcmp0(variant, "rest")) g_assert_cmpstr(subject, ==, "Second subject");
	}
	/* What the outbox sent the rest is B's subject too. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		g_autoptr(GPtrArray) mail = NULL;
		guint second = 0;
		venture_query_set_organization(query, 1);
		venture_query_set_limit(query, 0);
		venture_query_add_filter_string(query, "related-type", VENTURE_FILTER_OP_EQ, "marketing_recipient", NULL);
		mail = venture_database_find(f->db, query, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(mail->len, ==, 8);
		for (i = 0; i < mail->len; i++) {
			g_autofree gchar *subject = field_text(g_ptr_array_index(mail, i), "subject");
			if (!g_strcmp0(subject, "Second subject")) second++;
		}
		g_assert_cmpuint(second, ==, 6);
	}
	report = venture_marketing_send_report(f->db, 1, venture_entity_get_id(approved), &error);
	g_assert_no_error(error);
	g_assert_nonnull(report);
	g_assert_cmpstr(report->winner, ==, "b");
	g_assert_nonnull(report->arms);
	g_assert_cmpuint(report->arms->len, ==, 3);
	{
		const VentureMarketingArmCount *arm_a = g_ptr_array_index(report->arms, 0);
		const VentureMarketingArmCount *arm_b = g_ptr_array_index(report->arms, 1);
		const VentureMarketingArmCount *rest = g_ptr_array_index(report->arms, 2);
		g_assert_cmpstr(arm_a->arm, ==, "a");
		g_assert_cmpstr(arm_a->subject, ==, "First subject");
		g_assert_cmpint(arm_a->recipients, ==, 2);
		g_assert_cmpint(arm_a->opened, ==, 0);
		g_assert_cmpstr(arm_b->arm, ==, "b");
		g_assert_cmpint(arm_b->recipients, ==, 2);
		g_assert_cmpint(arm_b->opened, ==, 2);
		g_assert_cmpstr(rest->arm, ==, "rest");
		g_assert_cmpint(rest->recipients, ==, 4);
	}
	{
		g_autoptr(JsonNode) json = venture_marketing_send_report_to_json(report);
		g_autofree gchar *text = json_to_string(json, FALSE);
		g_assert_nonnull(strstr(text, "\"arms\""));
		g_assert_nonnull(strstr(text, "\"winner\":\"b\""));
	}
}

/* No opens either way is a tie, and a tie is A: the original. A rest
 * recipient who unsubscribes during the wait is never sent the winner. */
static void test_ab_tie_and_consent(Fixture *f, gconstpointer data)
{
	const gchar *names[] = { "Ada", "Bob", "Cy", "Dee" };
	g_autoptr(VentureEntity) approved = NULL, fresh = NULL;
	g_autoptr(GDateTime) start = venture_time_now();
	g_autofree gchar *winner = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	gint64 tick = 0;
	(void)data;
	track(f);
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		g_autofree gchar *lower = g_ascii_strdown(names[i], -1);
		g_autofree gchar *email = g_strdup_printf("%s@example.com", lower);
		g_autoptr(VentureEntity) row = reader(f, names[i], email);
	}
	g_object_set(f->send, "subject", "First", "variant-html-body", "<p>Other content</p>",
		"test-percent", (gint64)50, "winner-metric", 1, NULL);
	persist(f, f->send);
	approved = preview_and_approve(f, f->send);
	for (i = 0; i < 6; i++) {
		g_autoptr(GDateTime) now = g_date_time_add_seconds(start, ++tick * 5);
		if (run_at(f, approved, now) == 0) break;
	}
	rows = recipients_of(f, approved);
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *variant = field_text(row, "variant");
		if (!g_strcmp0(variant, "rest")) {
			g_autofree gchar *token = unsubscribe_token(row);
			g_assert_true(venture_marketing_service_unsubscribe(f->service, token, &error));
			g_assert_no_error(error);
			break;
		}
	}
	/* Default wait: four hours. */
	for (i = 0; i < 6; i++) {
		g_autoptr(GDateTime) now = g_date_time_add_seconds(start, tick * 5 + 4 * 3600 + 10 + i * 5);
		run_at(f, approved, now);
	}
	fresh = reread(f, approved);
	winner = field_text(fresh, "winner");
	g_assert_cmpstr(winner, ==, "a");
	g_assert_cmpint(queued_in(f, approved, "rest"), ==, 1);
	g_assert_cmpint(count_state(f, approved, "state", "cancelled"), ==, 1);
	/* An A/B test picks by people's opens or clicks, so it needs tracking. */
	{
		g_autoptr(VentureEntity) untracked = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1,
			"name", "Untracked test", "list-id", venture_entity_get_id(f->list), "subject", "One", "variant-subject", "Two",
			"text-body", "Body", "test-percent", (gint64)50, NULL);
		persist(f, untracked);
		g_assert_null(venture_marketing_service_preview(f->service, 1, venture_entity_get_id(untracked),
			"https://venture.example.com", NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}
}

/* ------------------------------------------------------------------ */
/* 7. Local time. The due moment is the chosen hour on the wall clock in
 * the recipient's zone, through daylight saving: 09:00 in New York is
 * 14:00 UTC the day before the clocks change and 13:00 UTC on the day. */
static void assert_due(const gchar *zone, gint hour, const gchar *day, const gchar *after_text, const gchar *expected)
{
	g_autoptr(GDateTime) after = g_date_time_new_from_iso8601(after_text, NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) due = venture_marketing_local_due(zone, hour, day, after, &error);
	g_autofree gchar *text = NULL;
	g_assert_no_error(error);
	g_assert_nonnull(due);
	text = g_date_time_format_iso8601(due);
	g_test_message("%s %02d:00 on %s after %s: %s", zone, hour, day ? day : "(next)", after_text, text);
	g_assert_cmpstr(text, ==, expected);
}
static void test_local_due(void)
{
	g_autoptr(GDateTime) after = g_date_time_new_from_iso8601("2030-03-01T00:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) bad = NULL;
	assert_due("America/New_York", 9, "2030-03-09", "2030-03-01T00:00:00Z", "2030-03-09T14:00:00Z");
	assert_due("America/New_York", 9, "2030-03-10", "2030-03-01T00:00:00Z", "2030-03-10T13:00:00Z");
	/* 02:30 does not exist on that day in New York: the clock jumps from
	 * 02:00 to 03:00, and the message goes at 03:00 EDT. */
	assert_due("America/New_York", 2, "2030-03-10", "2030-03-01T00:00:00Z", "2030-03-10T07:00:00Z");
	assert_due("Europe/London", 9, "2030-10-26", "2030-10-01T00:00:00Z", "2030-10-26T08:00:00Z");
	assert_due("Europe/London", 9, "2030-10-27", "2030-10-01T00:00:00Z", "2030-10-27T09:00:00Z");
	assert_due("Asia/Tokyo", 9, "2030-03-10", "2030-03-01T00:00:00Z", "2030-03-10T00:00:00Z");
	assert_due("Australia/Sydney", 9, "2030-04-07", "2030-03-01T00:00:00Z", "2030-04-06T23:00:00Z");
	/* No day: the next time that hour comes round, today if not yet past. */
	assert_due("Asia/Tokyo", 9, NULL, "2030-03-09T23:30:00Z", "2030-03-10T00:00:00Z");
	assert_due("Asia/Tokyo", 9, NULL, "2030-03-10T00:30:00Z", "2030-03-11T00:00:00Z");
	assert_due("UTC", 9, NULL, "2030-03-10T09:00:00Z", "2030-03-10T09:00:00Z");
	/* A day already past in that zone is due straight away, late rather than never. */
	assert_due("Asia/Tokyo", 9, "2030-03-10", "2030-03-10T05:00:00Z", "2030-03-10T05:00:00Z");
	bad = venture_marketing_local_due("Not/AZone", 9, NULL, after, &error);
	g_assert_null(bad);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	bad = venture_marketing_local_due("UTC", 24, NULL, after, &error);
	g_assert_null(bad);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* The send goes out zone by zone: each recipient is due at the hour in
 * the contact's zone, else the organization's, and the bounded delivery
 * and the warm-up still decide how fast. */
static void test_local_send(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) tokyo = NULL, york = NULL, berlin = NULL, organization = NULL, approved = NULL, row = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autofree gchar *text = NULL;
	(void)data;
	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, 1, &error);
	g_assert_no_error(error);
	g_object_set(organization, "timezone", "Europe/Berlin", NULL);
	persist(f, organization);
	tokyo = reader(f, "Tokyo", "tokyo@example.com");
	york = reader(f, "York", "york@example.com");
	berlin = reader(f, "Berlin", "berlin@example.com");
	g_object_set(tokyo, "timezone", "Asia/Tokyo", NULL); persist(f, tokyo);
	g_object_set(york, "timezone", "America/New_York", NULL); persist(f, york);
	g_assert_true(venture_entity_set_field_from_string(f->send, "local-date", "2030-03-10", NULL));
	g_object_set(f->send, "local-time", TRUE, "local-hour", (gint64)9, NULL);
	persist(f, f->send);
	approved = preview_and_approve(f, f->send);
	row = recipient_for(f, approved, "berlin@example.com");
	g_object_get(row, "due-at", &due, NULL);
	g_assert_nonnull(due);
	text = g_date_time_format_iso8601(due);
	g_assert_cmpstr(text, ==, "2030-03-10T08:00:00Z");
	{
		const struct { const gchar *at; gint queued; const gchar *who; } steps[] = {
			{ "2030-03-09T23:59:00Z", 0, NULL },
			{ "2030-03-10T00:00:30Z", 1, "tokyo@example.com" },
			{ "2030-03-10T00:05:00Z", 0, NULL },
			{ "2030-03-10T08:00:30Z", 1, "berlin@example.com" },
			{ "2030-03-10T12:59:00Z", 0, NULL },
			{ "2030-03-10T13:00:30Z", 1, "york@example.com" },
		};
		guint i;
		for (i = 0; i < G_N_ELEMENTS(steps); i++) {
			g_autoptr(GDateTime) at = g_date_time_new_from_iso8601(steps[i].at, NULL);
			g_assert_cmpint(run_at(f, approved, at), ==, steps[i].queued);
			if (steps[i].who) {
				g_autoptr(VentureEntity) queued = recipient_for(f, approved, steps[i].who);
				g_autofree gchar *state = field_text(queued, "state");
				g_assert_cmpstr(state, ==, "queued");
			}
		}
	}
	/* Local time and an A/B test are not combined. */
	{
		g_autoptr(VentureEntity) both = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1,
			"name", "Both", "list-id", venture_entity_get_id(f->list), "subject", "One", "variant-subject", "Two",
			"text-body", "Body", "test-percent", (gint64)50, "local-time", TRUE, "local-hour", (gint64)9, NULL);
		g_assert_false(venture_database_save(f->db, both, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}
	/* An hour out of range is refused at the save. */
	{
		g_autoptr(VentureEntity) wrong = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1,
			"name", "Wrong", "list-id", venture_entity_get_id(f->list), "subject", "One",
			"text-body", "Body", "local-time", TRUE, "local-hour", (gint64)25, NULL);
		g_assert_false(venture_database_save(f->db, wrong, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}
}

/* The warm-up still holds a local-time send: with a limit of one a day,
 * two recipients due at the same hour do not both go today. The warm-up
 * counts what the outbox took today by the server's own clock, so this
 * runs on today's date. */
static void test_local_warmup(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ada = NULL, bob = NULL, approved = NULL, domain = NULL, fresh = NULL, organization = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) later = g_date_time_add_seconds(now, 5);
	g_autoptr(GDateTime) next = NULL;
	g_autofree gchar *today = g_date_time_format(now, "%Y-%m-%d"), *text = NULL;
	(void)data;
	/* The business is on UTC and sends new local-time mail at midnight
	 * (hour 0 on a new send takes the business's hour, else 9), so the
	 * sends below are due at any time of day the test runs. */
	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, 1, NULL);
	g_assert_nonnull(organization);
	g_object_set(organization, "timezone", "UTC", "marketing-local-hour", "0", NULL);
	persist(f, organization);
	ada = reader(f, "Ada", "ada@example.com");
	bob = reader(f, "Bob", "bob@example.com");
	domain = g_object_new(VENTURE_TYPE_MARKETING_SENDING_DOMAIN, "organization-id", (gint64)1,
		"domain", "example.com", "ramp-schedule", "1:1", NULL);
	g_assert_true(venture_entity_set_field_from_string(domain, "warmup-started-on", today, NULL));
	persist(f, domain);
	/* Midnight UTC today has passed, so both are due straight away. */
	g_assert_true(venture_entity_set_field_from_string(f->send, "local-date", today, NULL));
	g_object_set(f->send, "local-time", TRUE, "local-hour", (gint64)0, NULL);
	persist(f, f->send);
	approved = preview_and_approve(f, f->send);
	g_clear_pointer(&now, g_date_time_unref); g_clear_pointer(&later, g_date_time_unref);
	now = venture_time_now();
	later = g_date_time_add_seconds(now, 5);
	g_assert_cmpint(run_at(f, approved, now), ==, 1);
	g_assert_cmpint(run_at(f, approved, later), ==, 0);
	fresh = reread(f, approved);
	g_object_get(fresh, "next-enqueue-at", &next, NULL);
	g_assert_nonnull(next);
	text = g_date_time_format(next, "%H:%M:%S");
	g_assert_cmpstr(text, ==, "00:00:00");
	g_assert_cmpint(g_date_time_compare(next, now), >, 0);
}

/* ------------------------------------------------------------------ */
/* 8. Resend to non-openers: a new draft whose audience is the first
 * send's recipients with no human open N days after their message, never
 * anybody who unsubscribed, bounced, complained or is suppressed. */
static void test_resend(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ada = NULL, bob = NULL, cy = NULL, dee = NULL, eve = NULL;
	g_autoptr(VentureMarketingSend) again = NULL, waiting = NULL;
	g_autoptr(VentureEntity) previewed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *subject = NULL, *text = NULL;
	gint64 of = 0, list = 0;
	(void)data;
	track(f);
	ada = reader(f, "Ada", "ada@example.com");
	bob = reader(f, "Bob", "bob@example.com");
	cy = reader(f, "Cy", "cy@example.com");
	dee = reader(f, "Dee", "dee@example.com");
	eve = reader(f, "Eve", "eve@example.com");
	send_all(f);
	open_as(f, f->send, "ada@example.com", "Mozilla/5.0");
	open_as(f, f->send, "bob@example.com", "Proofpoint URL defense");
	{
		g_autoptr(VentureEntity) row = recipient_for(f, f->send, "cy@example.com");
		g_autofree gchar *token = unsubscribe_token(row);
		g_assert_true(venture_marketing_service_unsubscribe(f->service, token, &error));
		g_assert_no_error(error);
	}
	{
		g_autoptr(VentureEntity) row = recipient_for(f, f->send, "dee@example.com");
		g_autoptr(GDateTime) now = venture_time_now();
		g_assert_true(venture_marketing_service_feedback(f->service, 1, venture_entity_get_id(row), "complaint",
			"Provider complaint report", "complaint-dee", now, NULL, &error));
		g_assert_no_error(error);
	}
	/* Refused before the first send is approved and done with. */
	{
		g_autoptr(VentureEntity) draft = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", (gint64)1,
			"name", "Draft", "list-id", venture_entity_get_id(f->list), "subject", "x", "text-body", "y", NULL);
		g_autoptr(VentureMarketingSend) refused = NULL;
		persist(f, draft);
		refused = venture_marketing_service_resend(f->service, 1, venture_entity_get_id(draft), 3, NULL, NULL, &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}
	again = venture_marketing_service_resend(f->service, 1, venture_entity_get_id(f->send), 0, "Did you see this?", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(again);
	g_object_get(again, "resend-of-id", &of, "list-id", &list, "subject", &subject, "text-body", &text, NULL);
	g_assert_cmpint(of, ==, venture_entity_get_id(f->send));
	g_assert_cmpint(list, ==, venture_entity_get_id(f->list));
	g_assert_cmpstr(subject, ==, "Did you see this?");
	g_assert_cmpstr(text, ==, "Read https://example.com/news");
	previewed = preview(f, VENTURE_ENTITY(again));
	assert_reason(f, previewed, "ada@example.com", "opened");
	/* A machine's open is not a person's: Bob is sent the follow-up. */
	assert_reason(f, previewed, "bob@example.com", "eligible");
	assert_reason(f, previewed, "cy@example.com", "suppressed");
	assert_reason(f, previewed, "dee@example.com", "suppressed");
	assert_reason(f, previewed, "eve@example.com", "eligible");
	/* With a three-day wait nobody is a non-opener yet. */
	waiting = venture_marketing_service_resend(f->service, 1, venture_entity_get_id(f->send), 3, NULL, NULL, &error);
	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) waited = preview(f, VENTURE_ENTITY(waiting));
		g_autofree gchar *kept = field_text(waited, "subject");
		g_assert_cmpstr(kept, ==, "Hello {name}");
		assert_reason(f, waited, "eve@example.com", "too_soon");
	}
}

/* ------------------------------------------------------------------ */
/* The preference page is the existing public capability page, extended:
 * GET changes nothing and discloses no address, POST with the form saves
 * the choices, RFC 8058 one-click still stops everything, and an unknown
 * capability is the same 404 as before. */
typedef struct { gboolean done; GBytes *body; GError *error; } Reply;
static void http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}
static guint http_with_policy(SoupSession *session, const gchar *base, const gchar *method, const gchar *path,
	const gchar *form, gchar **text, gchar **policy)
{
	g_autofree gchar *url = g_strconcat(base, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Reply reply = { FALSE, NULL, NULL };
	guint status;
	if (form) {
		g_autoptr(GBytes) body = g_bytes_new(form, strlen(form));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", body);
	}
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	status = soup_message_get_status(message);
	if (text) *text = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));
	if (policy) *policy = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Referrer-Policy"));
	g_bytes_unref(reply.body);
	return status;
}
static guint http(SoupSession *session, const gchar *base, const gchar *method, const gchar *path,
	const gchar *form, gchar **text)
{
	return http_with_policy(session, base, method, path, form, text, NULL);
}
static void test_preference_page(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(VentureEntity) news = topic(f, "Product news");
	g_autoptr(VentureEntity) offers = topic(f, "Offers");
	g_autoptr(VentureEntity) ada = NULL, bob = NULL, stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-audiences-web-XXXXXX", NULL), *base = NULL;
	g_autofree gchar *path = NULL, *bob_path = NULL, *text = NULL, *form = NULL, *topics_off = NULL, *expected = NULL, *policy = NULL;
	gint frequency = -1;
	(void)data;
	g_object_set(config, "state-dir", directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)0,
		"security-require-auth", TRUE, NULL);
	context = venture_context_new(config, f->db);
	server = venture_web_server_new(context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	base = g_strdup(venture_web_server_get_base_url(server));
	ada = reader(f, "Ada", "ada@example.com");
	bob = reader(f, "Bob", "bob@example.com");
	send_all(f);
	{
		g_autofree gchar *token = unsubscribe_token(recipient_for(f, f->send, "ada@example.com"));
		g_autofree gchar *other = unsubscribe_token(recipient_for(f, f->send, "bob@example.com"));
		path = g_strdup_printf("/marketing/u/%s", token);
		bob_path = g_strdup_printf("/marketing/u/%s", other);
	}
	g_assert_cmpuint(http_with_policy(session, base, "GET", path, NULL, &text, &policy), ==, 200);
	g_assert_cmpstr(policy, ==, "same-origin");
	g_assert_nonnull(strstr(text, "Product news"));
	g_assert_nonnull(strstr(text, "Offers"));
	g_assert_nonnull(strstr(text, "name=\"frequency\""));
	g_assert_nonnull(strstr(text, "List-Unsubscribe"));
	g_assert_null(strstr(text, "ada@example.com"));
	g_clear_pointer(&text, g_free);
	/* Ticked topics stay on; the unticked one is turned off. */
	form = g_strdup_printf("action=save&topic=%" G_GINT64_FORMAT "&frequency=weekly", venture_entity_get_id(news));
	g_assert_cmpuint(http(session, base, "POST", path, form, &text), ==, 200);
	g_assert_nonnull(strstr(text, "saved"));
	g_clear_pointer(&text, g_free);
	stored = venture_marketing_service_preferences_for(f->service, 1, "ada@example.com", &error);
	g_assert_no_error(error);
	g_assert_nonnull(stored);
	topics_off = field_text(stored, "topics-off");
	expected = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(offers));
	g_assert_cmpstr(topics_off, ==, expected);
	g_object_get(stored, "frequency", &frequency, NULL);
	g_assert_cmpint(frequency, ==, 1);
	/* The page shows what was chosen. */
	g_assert_cmpuint(http(session, base, "GET", path, NULL, &text), ==, 200);
	g_assert_nonnull(strstr(text, "value=\"weekly\" checked"));
	g_clear_pointer(&text, g_free);
	/* One-click from the List-Unsubscribe header still stops everything. */
	g_assert_cmpuint(http(session, base, "POST", bob_path, "List-Unsubscribe=One-Click", &text), ==, 200);
	g_clear_pointer(&text, g_free);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SUPPRESSION);
		venture_query_set_organization(query, 1);
		venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_EQ, "bob@example.com", NULL);
		g_assert_cmpint(venture_database_count(f->db, query, &error), ==, 1);
	}
	g_assert_cmpuint(http(session, base, "POST", "/marketing/u/0000000000000000000000000000000000000000000000000000000000000000",
		"action=save&frequency=weekly", NULL), ==, 404);
	g_assert_cmpuint(http(session, base, "POST", path, "action=save&frequency=hourly", NULL), ==, 400);
	venture_web_server_stop(server);
	g_clear_object(&server);
	g_clear_object(&context);
	while (g_main_context_iteration(NULL, FALSE)) { }
	venture_test_remove_tree(directory);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/audiences/tags/actions", Fixture, NULL, setup, test_tag_actions, teardown);
	g_test_add("/audiences/tags/segment", Fixture, NULL, setup, test_tag_segment, teardown);
	g_test_add("/audiences/behaviour/segments", Fixture, NULL, setup, test_behaviour_segments, teardown);
	g_test_add("/audiences/behaviour/audience", Fixture, NULL, setup, test_behaviour_audience, teardown);
	g_test_add("/audiences/preferences/choices", Fixture, NULL, setup, test_preferences, teardown);
	g_test_add("/audiences/preferences/page", Fixture, NULL, setup, test_preference_page, teardown);
	g_test_add_func("/audiences/ab/assignment", test_ab_assignment);
	g_test_add("/audiences/ab/winner", Fixture, NULL, setup, test_ab_winner, teardown);
	g_test_add("/audiences/ab/tie-and-consent", Fixture, NULL, setup, test_ab_tie_and_consent, teardown);
	g_test_add_func("/audiences/local/due", test_local_due);
	g_test_add("/audiences/local/send", Fixture, NULL, setup, test_local_send, teardown);
	g_test_add("/audiences/local/warmup", Fixture, NULL, setup, test_local_warmup, teardown);
	g_test_add("/audiences/resend", Fixture, NULL, setup, test_resend, teardown);
	return g_test_run();
}
