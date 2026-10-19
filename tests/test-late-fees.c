/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Late fees: a schedule of tiers, moved forward only by the dunning sweep,
 * added and waived only through the settlement service. Every amount here
 * is exact minor units. The invoice is 1,000.00 USD, issued 2026-01-01 and
 * due 2026-01-10, so "day N" is 2026-01-10 plus N days. The reminder policy
 * sends at days 7, 25, 40, 55 and 85, which is what gives each tier its
 * advance notice.
 */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	VentureLogMailer *mailer;
	gint64 org, company, contact, invoice, policy, template;
} Fixture;

static GType
type_of(const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	return type;
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	return g_object_new(type_of(name), "organization-id", f->org, NULL);
}

static void
save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, row, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
field(VentureEntity *row, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(row, name, value, &error));
	g_assert_no_error(error);
}

static VentureEntity *
reload(Fixture *f, const gchar *name, gint64 id)
{
	VentureEntity *row = venture_database_get(f->db, type_of(name), id, NULL);
	g_assert_nonnull(row);
	return row;
}

static GPtrArray *
rows(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = venture_query_new(type_of(name));
	venture_query_set_organization(query, f->org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(f->db, query, NULL);
}

static GPtrArray *
fees_for(Fixture *f, gint64 invoice)
{
	g_autoptr(VentureQuery) query = venture_query_new(type_of("late_fee"));
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice, NULL);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(f->db, query, NULL);
}

static gchar *
text_of(VentureEntity *row, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(row, name, &value, NULL);
	return value;
}

static gint64
minor(VentureEntity *row, const gchar *name)
{
	g_autoptr(VentureMoney) money = NULL;
	g_object_get(row, name, &money, NULL);
	return money != NULL ? venture_money_get_amount(money) : -1;
}

/* How many of the invoice's fees are in @status, and their exact total. */
static guint
count_status(Fixture *f, gint64 invoice, const gchar *status, gint64 *total)
{
	g_autoptr(GPtrArray) fees = fees_for(f, invoice);
	guint i, n = 0;
	if (total != NULL)
		*total = 0;
	for (i = 0; fees != NULL && i < fees->len; i++)
	{
		g_autofree gchar *s = text_of(g_ptr_array_index(fees, i), "status");
		if (g_strcmp0(s, status) != 0)
			continue;
		n++;
		if (total != NULL)
			*total += minor(g_ptr_array_index(fees, i), "amount");
	}
	return n;
}

static VentureEntity *
first_fee(Fixture *f, gint64 invoice, const gchar *status)
{
	g_autoptr(GPtrArray) fees = fees_for(f, invoice);
	guint i;
	for (i = 0; fees != NULL && i < fees->len; i++)
	{
		g_autofree gchar *s = text_of(g_ptr_array_index(fees, i), "status");
		if (g_strcmp0(s, status) == 0)
			return g_object_ref(g_ptr_array_index(fees, i));
	}
	return NULL;
}

static gint64
balance(Fixture *f, gint64 invoice)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) money = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		invoice, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(money);
	return venture_money_get_amount(money);
}

static gchar *
date_of_day(gint day)
{
	g_autoptr(GDateTime) due = venture_time_from_string("2026-01-10", NULL);
	g_autoptr(GDateTime) at = g_date_time_add_days(due, day);
	return g_date_time_format(at, "%Y-%m-%d");
}

static JsonNode *
sweep(Fixture *f, gint day)
{
	g_autofree gchar *date = date_of_day(day);
	g_autoptr(GDateTime) at = venture_time_from_string(date, NULL);
	g_autoptr(GError) error = NULL;
	JsonNode *answer = venture_dunning_service_sweep_detailed(venture_dunning_service_get(f->db), f->org, at, 100,
		FALSE, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(answer);
	return answer;
}

static void
deliver(Fixture *f, gint day)
{
	g_autofree gchar *date = date_of_day(day);
	g_autoptr(GDateTime) at = venture_time_from_string(date, NULL);
	g_autoptr(GError) error = NULL;
	gint n = venture_mail_outbox_deliver_due(venture_database_get_mail_outbox(f->db), f->org, 100, at, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(n, >=, 0);
}

/* A day of operations: the sweep, then the mail run. */
static void
day(Fixture *f, gint n)
{
	g_autoptr(JsonNode) answer = sweep(f, n);
	deliver(f, n);
}

static void
days(Fixture *f, gint from, gint to)
{
	gint n;
	for (n = from; n <= to; n++)
		day(f, n);
}

/* Every body the log mailer has accepted, joined. */
static gchar *
mailed(Fixture *f)
{
	const GPtrArray *sent = venture_log_mailer_get_messages(f->mailer);
	GString *all = g_string_new(NULL);
	guint i;
	for (i = 0; i < sent->len; i++)
	{
		g_autofree gchar *body = NULL;
		g_object_get(g_ptr_array_index((GPtrArray *)sent, i), "text-body", &body, NULL);
		g_string_append_printf(all, "%s\n---\n", body ? body : "");
	}
	return g_string_free(all, FALSE);
}

static gint64
schedule(Fixture *f, const gchar *name, const gchar *tiers, gint64 grace, const gchar *max_percent, const gchar *max_total)
{
	g_autoptr(VentureEntity) row = record(f, "late_fee_schedule");
	g_object_set(row, "name", name, "tiers", tiers, "grace-days", grace, NULL);
	if (max_percent != NULL)
		field(row, "max-percent", max_percent);
	if (max_total != NULL)
		field(row, "max-total", max_total);
	field(row, "adopted-at", "2026-01-01");
	save(f, row);
	return venture_entity_get_id(row);
}

static gint64
enable(Fixture *f, gint64 default_schedule, gboolean auto_apply)
{
	g_autoptr(VentureEntity) row = record(f, "late_fee_setting");
	g_object_set(row, "enabled", TRUE, "auto-apply", auto_apply, "default-schedule-id", default_schedule, NULL);
	field(row, "enabled-at", "2026-01-01");
	save(f, row);
	return venture_entity_get_id(row);
}

static gint64
issue(Fixture *f, const gchar *number, const gchar *price, gint64 company)
{
	g_autoptr(VentureEntity) invoice = record(f, "invoice");
	g_autoptr(VentureEntity) line = record(f, "invoice_line");
	g_autoptr(GDateTime) issued = venture_time_from_string("2026-01-01", NULL);
	g_autoptr(GError) error = NULL;
	g_object_set(invoice, "number", number, "company-id", company, "owner", "alice", NULL);
	if (company == f->company)
		g_object_set(invoice, "contact-id", f->contact, NULL);
	field(invoice, "issued-at", "2026-01-01");
	field(invoice, "due-at", "2026-01-10");
	save(f, invoice);
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Monthly service", "quantity", 1.0, NULL);
	field(line, "unit-price", price);
	save(f, line);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		VENTURE_INVOICE(invoice), "sent", issued, NULL, &error));
	g_assert_no_error(error);
	return venture_entity_get_id(invoice);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) company = NULL, contact = NULL, policy = NULL, template = NULL;
	g_autofree gchar *steps = NULL;
	gint64 t;
	(void)unused;
	f->config = venture_config_new();
	g_object_set(f->config, "server-base-url", "https://books.example.test", NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	f->mailer = venture_log_mailer_new();
	g_object_set(venture_database_get_mail_outbox(f->db), "mailer", f->mailer, NULL);
	company = record(f, "company");
	g_object_set(company, "name", "Harbor Lane Bakery", "email", "accounts@harborlane.example.com", NULL);
	save(f, company);
	f->company = venture_entity_get_id(company);
	contact = record(f, "contact");
	g_object_set(contact, "name", "Robin", "email", "robin@harborlane.example.com", "company-id", f->company, NULL);
	save(f, contact);
	f->contact = venture_entity_get_id(contact);
	template = record(f, "mail_template");
	/* No {late_fee_notice}: the notice must be appended anyway, or a policy
	 * switched on over existing templates would charge fees nobody was told of. */
	g_object_set(template, "name", "Reminder", "subject", "Invoice {number}",
		"text-body", "Hello {customer_name}: {number} has {open_balance} outstanding.", NULL);
	save(f, template);
	t = f->template = venture_entity_get_id(template);
	steps = g_strdup_printf("[{\"offset\":7,\"template_id\":%" G_GINT64_FORMAT "},{\"offset\":25,\"template_id\":%" G_GINT64_FORMAT "},"
		"{\"offset\":40,\"template_id\":%" G_GINT64_FORMAT "},{\"offset\":55,\"template_id\":%" G_GINT64_FORMAT "},"
		"{\"offset\":85,\"template_id\":%" G_GINT64_FORMAT "}]", t, t, t, t, t);
	policy = record(f, "dunning_policy");
	g_object_set(policy, "name", "Standard", "steps", steps, "is-default", TRUE, NULL);
	field(policy, "adopted-at", "2026-01-01");
	save(f, policy);
	f->policy = venture_entity_get_id(policy);
	f->invoice = issue(f, "INV-LF-1", "1000.00 USD", f->company);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	g_clear_object(&f->mailer);
}

#define FLAT_15 "[{\"day\":15,\"type\":\"flat\",\"amount\":\"25.00 USD\"}]"

/* Off by default: a schedule alone charges nothing and says nothing. */
static void
test_off_by_default(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *all = NULL;
	g_autoptr(GPtrArray) fees = NULL;
	(void)unused;
	schedule(f, "Residential", FLAT_15, 14, NULL, NULL);
	days(f, 1, 40);
	fees = rows(f, "late_fee");
	g_assert_cmpuint(fees->len, ==, 0);
	g_assert_cmpint(balance(f, f->invoice), ==, 100000);
	all = mailed(f);
	g_assert_nonnull(strstr(all, "Harbor Lane Bakery"));
	g_assert_null(strstr(all, "late fee"));
}

/* Grace, the day before and the day of the trigger, the stated notice, the
 * approval default and replays of the same day. */
static void
test_grace_notice_approval(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *all = NULL;
	g_autoptr(VentureEntity) fee = NULL;
	(void)unused;
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), FALSE);
	days(f, 1, 7);
	all = mailed(f);
	g_assert_nonnull(strstr(all, "A $25.00 late fee will be added on 2026-01-25."));
	g_assert_cmpuint(count_status(f, f->invoice, "scheduled", NULL), ==, 1);
	days(f, 8, 14);
	g_assert_cmpuint(count_status(f, f->invoice, "proposed", NULL), ==, 0);
	g_assert_cmpint(balance(f, f->invoice), ==, 100000);
	day(f, 15);
	{
		gint64 total = 0;
		g_assert_cmpuint(count_status(f, f->invoice, "proposed", &total), ==, 1);
		g_assert_cmpint(total, ==, 2500);
	}
	/* Approval is the default: proposed is not added. */
	g_assert_cmpint(balance(f, f->invoice), ==, 100000);
	/* Replays of the same day and the days after write nothing new. */
	day(f, 15);
	day(f, 15);
	days(f, 16, 20);
	g_assert_cmpuint(fees_for(f, f->invoice)->len, ==, 1);
	fee = first_fee(f, f->invoice, "proposed");
	g_assert_nonnull(fee);
	g_assert_cmpint(venture_entity_get_id(fee), >, 0);
}

/* Approving adds a separate adjustment; the invoice's lines never move. */
static void
test_approve_adds_adjustment(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) fee = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) customer = NULL;
	guint i, fee_events = 0;
	gint status = 0;
	(void)unused;
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), FALSE);
	days(f, 1, 15);
	fee = first_fee(f, f->invoice, "proposed");
	g_assert_nonnull(fee);
	g_assert_true(venture_late_fee_service_approve(venture_late_fee_service_get(f->db), fee, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", NULL), ==, 1);
	g_assert_cmpint(balance(f, f->invoice), ==, 102500);
	lines = rows(f, "invoice_line");
	g_assert_cmpuint(lines->len, ==, 1);
	g_assert_cmpint(minor(g_ptr_array_index(lines, 0), "unit-price"), ==, 100000);
	events = rows(f, "invoice_event");
	for (i = 0; i < events->len; i++)
	{
		g_autofree gchar *kind = text_of(g_ptr_array_index(events, i), "kind");
		if (g_strcmp0(kind, "late_fee") == 0)
		{
			fee_events++;
			g_assert_cmpint(minor(g_ptr_array_index(events, i), "amount"), ==, 2500);
		}
	}
	g_assert_cmpuint(fee_events, ==, 1);
	/* A fee on top is not a part payment. */
	invoice = reload(f, "invoice", f->invoice);
	g_object_get(invoice, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_SENT);
	customer = venture_settlement_service_customer_balance(venture_settlement_service_get(f->db), f->org, f->company,
		NULL, "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(customer), ==, 102500);
	/* Approving twice is refused rather than charged twice. */
	g_clear_object(&fee);
	fee = first_fee(f, f->invoice, "applied");
	g_assert_false(venture_late_fee_service_approve(venture_late_fee_service_get(f->db), fee, NULL, NULL, &error));
	g_assert_nonnull(error);
	g_assert_cmpint(balance(f, f->invoice), ==, 102500);
	/* Paying the whole balance settles the fee with the invoice. */
	g_clear_error(&error);
	{
		g_autoptr(GDateTime) paid = venture_time_from_string("2026-01-28", NULL);
		g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db), f->invoice, paid, NULL, &error));
		g_assert_no_error(error);
	}
	g_assert_cmpint(balance(f, f->invoice), ==, 0);
}

/* No fee without the notice having been sent or explicitly skipped. */
static void
test_notice_required(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) fee = NULL;
	g_autoptr(GError) error = NULL;
	gint n;
	(void)unused;
	/* Auto-apply, so the only thing standing between the due day and the
	 * balance is the notice. The reminders are queued but never delivered. */
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), TRUE);
	for (n = 1; n <= 16; n++)
	{
		g_autoptr(JsonNode) answer = sweep(f, n);
	}
	g_assert_cmpuint(count_status(f, f->invoice, "awaiting_notice", NULL), ==, 1);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", NULL), ==, 0);
	g_assert_cmpint(balance(f, f->invoice), ==, 100000);
	fee = first_fee(f, f->invoice, "awaiting_notice");
	g_assert_true(venture_late_fee_service_skip_notice(venture_late_fee_service_get(f->db), fee,
		"told them on the phone", NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(JsonNode) answer = sweep(f, 16);
		JsonObject *late = json_object_get_object_member(json_node_get_object(answer), "late_fees");
		g_assert_nonnull(late);
		g_assert_cmpint(json_object_get_int_member(late, "applied"), ==, 1);
	}
	g_assert_cmpint(balance(f, f->invoice), ==, 102500);
}

/* Auto-apply is the explicit opt-in that adds a noticed fee on its day. */
static void
test_auto_apply(Fixture *f, gconstpointer unused)
{
	(void)unused;
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), TRUE);
	days(f, 1, 14);
	g_assert_cmpint(balance(f, f->invoice), ==, 100000);
	day(f, 15);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", NULL), ==, 1);
	g_assert_cmpint(balance(f, f->invoice), ==, 102500);
	days(f, 15, 24);
	g_assert_cmpint(balance(f, f->invoice), ==, 102500);
}

/* Bulk approval and skipping, each fee its own decision. */
static void
test_bulk_and_skip(Fixture *f, gconstpointer unused)
{
	gint64 second, third;
	gint64 ids[2];
	g_autoptr(VentureEntity) a = NULL, b = NULL, c = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), FALSE);
	second = issue(f, "INV-LF-2", "200.00 USD", f->company);
	third = issue(f, "INV-LF-3", "300.00 USD", f->company);
	days(f, 1, 15);
	a = first_fee(f, f->invoice, "proposed");
	b = first_fee(f, second, "proposed");
	c = first_fee(f, third, "proposed");
	g_assert_nonnull(a);
	g_assert_nonnull(b);
	g_assert_nonnull(c);
	ids[0] = venture_entity_get_id(a);
	ids[1] = venture_entity_get_id(b);
	answer = venture_late_fee_service_decide(venture_late_fee_service_get(f->db), f->org, ids, 2, TRUE, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(answer);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(answer), "decided"), ==, 2);
	g_assert_cmpint(balance(f, f->invoice), ==, 102500);
	g_assert_cmpint(balance(f, second), ==, 22500);
	g_assert_true(venture_late_fee_service_skip(venture_late_fee_service_get(f->db), c, "new customer", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(count_status(f, third, "skipped", NULL), ==, 1);
	g_assert_cmpint(balance(f, third), ==, 30000);
	/* A skipped occurrence is decided; the sweep does not bring it back. */
	days(f, 16, 18);
	g_assert_cmpuint(count_status(f, third, "skipped", NULL), ==, 1);
	g_assert_cmpuint(fees_for(f, third)->len, ==, 1);
}

/* One action, a reason, an audit trail, a note to the customer, and the
 * balance and the report move with it. */
static void
test_waive(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) fee = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) credits = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(GPtrArray) audit = NULL;
	guint i, waivers = 0, notes = 0, trail = 0;
	(void)unused;
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), TRUE);
	days(f, 1, 15);
	fee = first_fee(f, f->invoice, "applied");
	g_assert_nonnull(fee);
	g_assert_false(venture_late_fee_service_waive(venture_late_fee_service_get(f->db), fee, "", NULL, NULL, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(venture_late_fee_service_waive(venture_late_fee_service_get(f->db), fee, "goodwill",
		"We have removed the late fee from INV-LF-1.", NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(balance(f, f->invoice), ==, 100000);
	g_assert_cmpuint(count_status(f, f->invoice, "waived", NULL), ==, 1);
	g_clear_object(&fee);
	fee = first_fee(f, f->invoice, "waived");
	g_assert_false(venture_late_fee_service_waive(venture_late_fee_service_get(f->db), fee, "again", NULL, NULL, NULL, &error));
	g_assert_nonnull(error);
	g_assert_cmpint(balance(f, f->invoice), ==, 100000);
	credits = rows(f, "customer_credit");
	for (i = 0; i < credits->len; i++)
	{
		g_autofree gchar *kind = text_of(g_ptr_array_index(credits, i), "kind");
		if (g_strcmp0(kind, "late_fee_waiver") == 0)
			waivers++;
	}
	g_assert_cmpuint(waivers, ==, 1);
	messages = rows(f, "mail_message");
	for (i = 0; i < messages->len; i++)
	{
		g_autofree gchar *related = text_of(g_ptr_array_index(messages, i), "related-type");
		g_autofree gchar *body = text_of(g_ptr_array_index(messages, i), "text-body");
		if (g_strcmp0(related, "late_fee") == 0 && body != NULL && strstr(body, "removed the late fee") != NULL)
			notes++;
	}
	g_assert_cmpuint(notes, ==, 1);
	audit = rows(f, "audit_entry");
	for (i = 0; i < audit->len; i++)
	{
		g_autofree gchar *type = text_of(g_ptr_array_index(audit, i), "target-type");
		if (g_strcmp0(type, "late_fee") == 0)
			trail++;
	}
	g_assert_cmpuint(trail, >=, 2);
	/* The waived occurrence is decided: no replay charges it again. */
	days(f, 15, 20);
	g_assert_cmpint(balance(f, f->invoice), ==, 100000);
	g_assert_cmpuint(fees_for(f, f->invoice)->len, ==, 1);
}

/* A recurring tier: the day before and the day of each occurrence. */
static void
test_recurring(Fixture *f, gconstpointer unused)
{
	gint64 total = 0;
	(void)unused;
	enable(f, schedule(f, "Commercial net 30",
		"[{\"day\":30,\"every\":30,\"type\":\"percent_open\",\"percent\":\"1.5\"}]", 0, NULL, NULL), TRUE);
	days(f, 1, 29);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", NULL), ==, 0);
	day(f, 30);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", &total), ==, 1);
	g_assert_cmpint(total, ==, 1500);
	days(f, 31, 59);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", NULL), ==, 1);
	day(f, 60);
	/* Percent of the open balance excluding fees already charged: 1.5% of
	 * 1,000.00 again, not of 1,015.00. */
	g_assert_cmpuint(count_status(f, f->invoice, "applied", &total), ==, 2);
	g_assert_cmpint(total, ==, 3000);
	days(f, 61, 90);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", &total), ==, 3);
	g_assert_cmpint(total, ==, 4500);
	g_assert_cmpint(balance(f, f->invoice), ==, 104500);
}

/* Markup compounds: a percent of everything outstanding, prior fees too. */
static void
test_markup(Fixture *f, gconstpointer unused)
{
	gint64 total = 0;
	(void)unused;
	enable(f, schedule(f, "Markup",
		"[{\"day\":30,\"every\":30,\"type\":\"markup\",\"percent\":\"10\"}]", 0, NULL, NULL), TRUE);
	days(f, 1, 60);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", &total), ==, 2);
	/* 10% of 1,000.00, then 10% of 1,100.00. */
	g_assert_cmpint(total, ==, 10000 + 11000);
}

/* Caps hit mid-schedule: the fee that crosses is cut, later ones vanish. */
static void
test_caps(Fixture *f, gconstpointer unused)
{
	gint64 total = 0;
	gint64 second;
	g_autoptr(VentureEntity) company = NULL;
	(void)unused;
	enable(f, schedule(f, "Capped",
		"[{\"day\":15,\"type\":\"flat\",\"amount\":\"25.00 USD\"},"
		"{\"day\":30,\"type\":\"flat\",\"amount\":\"50.00 USD\"},"
		"{\"day\":45,\"type\":\"flat\",\"amount\":\"50.00 USD\"}]", 0, NULL, "100.00 USD"), TRUE);
	/* A second customer on a percent-of-original cap. */
	company = record(f, "company");
	g_object_set(company, "name", "Quarry Road Studio", "email", "pay@quarryroad.example.com", NULL);
	save(f, company);
	{
		gint64 percent = schedule(f, "Five percent",
			"[{\"day\":15,\"type\":\"flat\",\"amount\":\"25.00 USD\"},"
			"{\"day\":30,\"every\":30,\"type\":\"percent_original\",\"percent\":\"2\",\"cap\":\"30.00 USD\"}]", 0, "5", NULL);
		g_object_set(company, "late-fee-schedule-id", percent, NULL);
		save(f, company);
	}
	second = issue(f, "INV-LF-Q", "1000.00 USD", venture_entity_get_id(company));
	days(f, 1, 29);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", &total), ==, 1);
	g_assert_cmpint(total, ==, 2500);
	days(f, 30, 44);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", &total), ==, 2);
	g_assert_cmpint(total, ==, 7500);
	days(f, 45, 46);
	/* 100.00 total: the third fee is cut from 50.00 to 25.00. */
	g_assert_cmpuint(count_status(f, f->invoice, "applied", &total), ==, 3);
	g_assert_cmpint(total, ==, 10000);
	g_assert_cmpint(balance(f, f->invoice), ==, 110000);
	/* The second invoice has no contact and no reminders reach it, so the
	 * owner skips each notice; the caps are what is tested. Tier cap 30.00:
	 * 20.00 on day 30, 10.00 on day 60, nothing after. Schedule cap 5% of
	 * 1,000.00 = 50.00: 25 + 20 + 5. */
	{
		gint n;
		for (n = 15; n <= 95; n++)
		{
			g_autoptr(VentureEntity) waiting = NULL;
			day(f, n);
			while ((waiting = first_fee(f, second, "awaiting_notice")) != NULL)
			{
				g_autoptr(GError) error = NULL;
				g_assert_true(venture_late_fee_service_skip_notice(venture_late_fee_service_get(f->db), waiting, "test", NULL, &error));
				g_assert_no_error(error);
				g_clear_object(&waiting);
				day(f, n);
			}
		}
	}
	g_assert_cmpuint(count_status(f, second, "applied", &total), ==, 3);
	g_assert_cmpint(total, ==, 5000);
	g_assert_cmpint(balance(f, second), ==, 105000);
}

/* Every pause scope: nothing added and no fee wording while it holds. */
static void
test_pause_scopes(Fixture *f, gconstpointer unused)
{
	VentureLateFeeService *service;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) until = venture_time_from_string("2026-02-01", NULL);
	g_autoptr(VentureEntity) invoice = NULL;
	g_autofree gchar *all = NULL;
	g_autofree gchar *state = NULL;
	gint64 second, third;
	(void)unused;
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), TRUE);
	service = venture_late_fee_service_get(f->db);
	second = issue(f, "INV-LF-P2", "100.00 USD", f->company);
	/* The invoice indefinitely, then the customer until a date. */
	g_assert_true(venture_late_fee_service_pause(service, "invoice", f->invoice, NULL, "disputed delivery", NULL, &error));
	g_assert_no_error(error);
	invoice = reload(f, "invoice", f->invoice);
	{
		g_autoptr(GDateTime) at = venture_time_from_string("2026-01-20", NULL);
		state = venture_late_fee_service_pause_state(service, invoice, at);
	}
	g_assert_nonnull(state);
	g_assert_nonnull(strstr(state, "indefinitely"));
	g_assert_true(venture_late_fee_service_pause(service, "company", f->company, until, "promised by phone", NULL, &error));
	g_assert_no_error(error);
	days(f, 1, 21);
	all = mailed(f);
	g_assert_null(strstr(all, "late fee"));
	g_assert_cmpuint(count_status(f, f->invoice, "applied", NULL), ==, 0);
	g_assert_cmpuint(fees_for(f, second)->len, ==, 0);
	g_assert_cmpint(balance(f, second), ==, 10000);
	/* The day the customer's pause ends the fee is due, and since no notice
	 * could be sent while paused it waits for one. */
	day(f, 22);
	g_assert_cmpuint(count_status(f, second, "awaiting_notice", NULL), ==, 1);
	g_assert_cmpuint(fees_for(f, f->invoice)->len, ==, 0);
	/* Resuming the invoice lets its fee come due too. */
	g_assert_true(venture_late_fee_service_resume(service, "invoice", f->invoice, NULL, &error));
	g_assert_no_error(error);
	day(f, 23);
	g_assert_cmpuint(count_status(f, f->invoice, "awaiting_notice", NULL), ==, 1);
	/* The organization, indefinitely: skipped notices still add nothing. */
	third = issue(f, "INV-LF-P3", "100.00 USD", f->company);
	g_assert_true(venture_late_fee_service_pause(service, "organization", f->org, NULL, "year-end review", NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) waiting = first_fee(f, second, "awaiting_notice");
		g_assert_true(venture_late_fee_service_skip_notice(service, waiting, "owner", NULL, &error));
		g_assert_no_error(error);
	}
	days(f, 24, 30);
	g_assert_cmpint(balance(f, second), ==, 10000);
	g_assert_cmpint(balance(f, third), ==, 10000);
	{
		g_autoptr(JsonNode) preview = venture_late_fee_service_preview(service, third, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(preview);
		g_assert_true(json_object_get_boolean_member(json_node_get_object(preview), "paused"));
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(preview), "pause_scope"), ==, "organization");
	}
	g_assert_true(venture_late_fee_service_resume(service, "organization", f->org, NULL, &error));
	g_assert_no_error(error);
	day(f, 31);
	g_assert_cmpint(balance(f, second), ==, 12500);
}

static JsonObject *
entry_on(JsonNode *preview, gint wanted_day, guint nth)
{
	JsonArray *entries = json_object_get_array_member(json_node_get_object(preview), "entries");
	guint i, seen = 0;
	for (i = 0; i < json_array_get_length(entries); i++)
	{
		JsonObject *entry = json_array_get_object_element(entries, i);
		if (json_object_get_int_member(entry, "day") == wanted_day && seen++ == nth)
			return entry;
	}
	return NULL;
}

static JsonNode *
preview(Fixture *f, gint64 invoice, gint as_of_day)
{
	g_autofree gchar *date = date_of_day(as_of_day);
	g_autoptr(GDateTime) at = venture_time_from_string(date, NULL);
	g_autoptr(GError) error = NULL;
	JsonNode *node = venture_late_fee_service_preview(venture_late_fee_service_get(f->db), invoice, at, &error);
	g_assert_no_error(error);
	g_assert_nonnull(node);
	return node;
}

static const gchar *
status_on(JsonNode *node, gint wanted_day)
{
	JsonObject *entry = entry_on(node, wanted_day, 0);
	g_assert_nonnull(entry);
	return json_object_get_string_member(entry, "status");
}

/* Conditions, alone and combined, judged the way the sweep judges them. */
static void
test_conditions(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	(void)unused;
	enable(f, schedule(f, "Conditional",
		"[{\"day\":15,\"type\":\"flat\",\"amount\":\"25.00 USD\",\"conditions\":{\"balance_above\":\"500.00 USD\",\"customer_tags\":[\"commercial\"]}},"
		"{\"day\":20,\"type\":\"flat\",\"amount\":\"10.00 USD\",\"conditions\":{\"currency\":\"EUR\"}},"
		"{\"day\":25,\"type\":\"flat\",\"amount\":\"10.00 USD\",\"conditions\":{\"min_prior_late\":1}},"
		"{\"day\":35,\"type\":\"flat\",\"amount\":\"10.00 USD\",\"conditions\":{\"skip_if_promise\":true,\"invoice_types\":[\"commercial\"]}},"
		"{\"day\":40,\"type\":\"flat\",\"amount\":\"10.00 USD\",\"conditions\":{\"balance_below\":\"500.00 USD\"}}]", 0, NULL, NULL), FALSE);
	{
		g_autoptr(JsonNode) p = preview(f, f->invoice, 1);
		/* Balance is above 500 but the customer is not tagged commercial. */
		g_assert_cmpstr(status_on(p, 15), ==, "skipped_condition");
		g_assert_cmpstr(status_on(p, 20), ==, "skipped_condition");
		g_assert_cmpstr(status_on(p, 25), ==, "skipped_condition");
		g_assert_cmpstr(status_on(p, 35), ==, "skipped_condition");
		g_assert_cmpstr(status_on(p, 40), ==, "skipped_condition");
	}
	company = reload(f, "company", f->company);
	g_object_set(company, "tags", "bakery, Commercial", NULL);
	save(f, company);
	invoice = reload(f, "invoice", f->invoice);
	g_object_set(invoice, "invoice-type", "commercial", NULL);
	save(f, invoice);
	{
		g_autoptr(JsonNode) p = preview(f, f->invoice, 1);
		g_assert_cmpstr(status_on(p, 15), ==, "upcoming");
		g_assert_cmpstr(status_on(p, 35), ==, "upcoming");
		g_assert_cmpstr(status_on(p, 25), ==, "skipped_condition");
	}
	/* A promise to pay that is still in the future skips the promise tier. */
	g_clear_object(&invoice);
	invoice = reload(f, "invoice", f->invoice);
	field(invoice, "dunning-paused-until", "2026-03-01");
	save(f, invoice);
	{
		g_autoptr(JsonNode) p = preview(f, f->invoice, 1);
		g_assert_cmpstr(status_on(p, 35), ==, "skipped_condition");
		g_assert_cmpstr(status_on(p, 15), ==, "upcoming");
	}
	/* A prior invoice paid after its due date counts as late. */
	{
		g_autoptr(VentureEntity) prior = record(f, "invoice");
		g_autoptr(VentureEntity) line = record(f, "invoice_line");
		g_autoptr(GDateTime) issued = venture_time_from_string("2025-11-01", NULL);
		g_autoptr(GDateTime) paid = venture_time_from_string("2025-12-20", NULL);
		g_autoptr(GError) error = NULL;
		g_object_set(prior, "number", "INV-LF-OLD", "company-id", f->company, NULL);
		field(prior, "issued-at", "2025-11-01");
		field(prior, "due-at", "2025-12-01");
		save(f, prior);
		g_object_set(line, "invoice-id", venture_entity_get_id(prior), "description", "Old work", "quantity", 1.0, NULL);
		field(line, "unit-price", "50.00 USD");
		save(f, line);
		g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
			VENTURE_INVOICE(prior), "sent", issued, NULL, &error));
		g_assert_no_error(error);
		g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db),
			venture_entity_get_id(prior), paid, NULL, &error));
		g_assert_no_error(error);
	}
	{
		g_autoptr(JsonNode) p = preview(f, f->invoice, 1);
		g_assert_cmpstr(status_on(p, 25), ==, "upcoming");
	}
}

/* Moving an invoice to another schedule keeps what was charged; the new
 * schedule only governs what comes after, and the move is audited. */
static void
test_schedule_change(Fixture *f, gconstpointer unused)
{
	gint64 total = 0;
	gint64 second;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GPtrArray) audit = NULL;
	guint i, moved = 0;
	(void)unused;
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), TRUE);
	second = schedule(f, "Commercial net 30",
		"[{\"day\":10,\"type\":\"flat\",\"amount\":\"10.00 USD\"},{\"day\":30,\"type\":\"flat\",\"amount\":\"40.00 USD\"}]", 0, NULL, NULL);
	days(f, 1, 20);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", &total), ==, 1);
	g_assert_cmpint(total, ==, 2500);
	invoice = reload(f, "invoice", f->invoice);
	g_object_set(invoice, "late-fee-schedule-id", second, NULL);
	save(f, invoice);
	days(f, 20, 31);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", &total), ==, 2);
	/* 25.00 stays; the new day-10 tier is in the past and never charged. */
	g_assert_cmpint(total, ==, 6500);
	g_assert_cmpint(balance(f, f->invoice), ==, 106500);
	audit = rows(f, "audit_entry");
	for (i = 0; i < audit->len; i++)
	{
		g_autofree gchar *type = text_of(g_ptr_array_index(audit, i), "target-type");
		g_autofree gchar *diff = text_of(g_ptr_array_index(audit, i), "diff");
		if (g_strcmp0(type, "invoice") == 0 && diff != NULL && strstr(diff, "late_fee_schedule_id") != NULL)
			moved++;
	}
	g_assert_cmpuint(moved, ==, 1);
}

/* The whole timeline before anything is applied, with the caps. */
static void
test_preview_timeline(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) p = NULL;
	JsonArray *entries;
	JsonObject *entry;
	gint64 sum = 0;
	guint i;
	gboolean capped = FALSE;
	(void)unused;
	/* The issue's example: 14 days grace; day 15 $25 once; day 30 1.5% of
	 * the open balance; every 30 days after, 1.5% again; never more than 10%
	 * of the original invoice. */
	enable(f, schedule(f, "Residential",
		"[{\"day\":15,\"type\":\"flat\",\"amount\":\"25.00 USD\"},"
		"{\"day\":30,\"type\":\"percent_open\",\"percent\":\"1.5\"},"
		"{\"after\":30,\"every\":30,\"type\":\"percent_open\",\"percent\":\"1.5\"}]", 14, "10", NULL), FALSE);
	p = preview(f, f->invoice, 1);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(p), "schedule_name"), ==, "Residential");
	entry = entry_on(p, 15, 0);
	g_assert_nonnull(entry);
	g_assert_cmpstr(json_object_get_string_member(entry, "date"), ==, "2026-01-25");
	g_assert_cmpstr(json_object_get_string_member(entry, "amount"), ==, "25.00 USD");
	g_assert_cmpstr(json_object_get_string_member(entry, "status"), ==, "upcoming");
	g_assert_cmpstr(json_object_get_string_member(entry_on(p, 30, 0), "amount"), ==, "15.00 USD");
	g_assert_cmpstr(json_object_get_string_member(entry_on(p, 60, 0), "amount"), ==, "15.00 USD");
	entries = json_object_get_array_member(json_node_get_object(p), "entries");
	for (i = 0; i < json_array_get_length(entries); i++)
	{
		JsonObject *e = json_array_get_object_element(entries, i);
		g_autoptr(VentureMoney) m = venture_money_from_string(json_object_get_string_member(e, "amount"), "USD", NULL);
		if (g_strcmp0(json_object_get_string_member(e, "status"), "capped") == 0)
			capped = TRUE;
		else if (m != NULL)
			sum += venture_money_get_amount(m);
	}
	/* 25 + 15 * 5 = 100.00 exactly: 10% of 1,000.00. */
	g_assert_cmpint(sum, ==, 10000);
	g_assert_true(capped);
	/* The day before a trigger it is upcoming, on the day it is due. */
	{
		g_autoptr(JsonNode) before = preview(f, f->invoice, 14);
		g_autoptr(JsonNode) on = preview(f, f->invoice, 15);
		g_assert_cmpstr(status_on(before, 15), ==, "upcoming");
		g_assert_cmpstr(status_on(on, 15), ==, "due");
	}
	/* Nothing was written. */
	g_assert_cmpuint(rows(f, "late_fee")->len, ==, 0);
}

/* Rounding rules, exact to the minor unit. 1.5% of 1,234.56 is 18.5184. */
static void
test_rounding(Fixture *f, gconstpointer unused)
{
	gint64 odd;
	g_autoptr(JsonNode) p = NULL;
	(void)unused;
	enable(f, schedule(f, "Rounding",
		"[{\"day\":30,\"type\":\"percent_open\",\"percent\":\"1.5\"},"
		"{\"day\":31,\"type\":\"percent_open\",\"percent\":\"1.5\",\"round\":\"down\"},"
		"{\"day\":32,\"type\":\"percent_open\",\"percent\":\"1.5\",\"round\":\"up\"},"
		"{\"day\":33,\"type\":\"percent_open\",\"percent\":\"1.5\",\"round\":\"whole\"},"
		"{\"day\":34,\"type\":\"percent_open\",\"percent\":\"0.125\",\"round\":\"up\"}]", 0, NULL, NULL), FALSE);
	odd = issue(f, "INV-LF-R", "1234.56 USD", f->company);
	p = preview(f, odd, 1);
	g_assert_cmpstr(json_object_get_string_member(entry_on(p, 30, 0), "amount"), ==, "18.52 USD");
	g_assert_cmpstr(json_object_get_string_member(entry_on(p, 31, 0), "amount"), ==, "18.51 USD");
	g_assert_cmpstr(json_object_get_string_member(entry_on(p, 32, 0), "amount"), ==, "18.52 USD");
	g_assert_cmpstr(json_object_get_string_member(entry_on(p, 33, 0), "amount"), ==, "19.00 USD");
	/* 0.125% of 1,234.56 is 1.5432: up is 1.55. */
	g_assert_cmpstr(json_object_get_string_member(entry_on(p, 34, 0), "amount"), ==, "1.55 USD");
}

/* A flat amount in another currency is never converted; it is not charged. */
static void
test_currency(Fixture *f, gconstpointer unused)
{
	gint64 euro;
	g_autoptr(JsonNode) p = NULL;
	(void)unused;
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), TRUE);
	euro = issue(f, "INV-LF-EUR", "500.00 EUR", f->company);
	p = preview(f, euro, 1);
	g_assert_cmpstr(status_on(p, 15), ==, "skipped_condition");
	days(f, 1, 16);
	g_assert_cmpuint(fees_for(f, euro)->len, ==, 0);
	g_assert_cmpint(balance(f, euro), ==, 50000);
}

/* Paid before the fee's day: the announced fee is cancelled, not charged;
 * and an exempt customer is never charged at all. */
static void
test_settled_and_exempt(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) paid = venture_time_from_string("2026-01-20", NULL);
	g_autoptr(VentureEntity) company = NULL;
	gint64 other;
	(void)unused;
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), TRUE);
	company = record(f, "company");
	g_object_set(company, "name", "Exempt Co", "email", "ap@exempt.example.com", "late-fee-exempt", TRUE, NULL);
	save(f, company);
	other = issue(f, "INV-LF-X", "100.00 USD", venture_entity_get_id(company));
	days(f, 1, 9);
	g_assert_cmpuint(count_status(f, f->invoice, "scheduled", NULL), ==, 1);
	g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db), f->invoice, paid, NULL, &error));
	g_assert_no_error(error);
	days(f, 10, 20);
	g_assert_cmpuint(count_status(f, f->invoice, "applied", NULL), ==, 0);
	g_assert_cmpuint(count_status(f, f->invoice, "cancelled", NULL), ==, 1);
	g_assert_cmpuint(fees_for(f, other)->len, ==, 0);
	g_assert_cmpint(balance(f, other), ==, 10000);
}

/* Visible on the statement, the customer timeline and the report. */
static void
test_visibility(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) fee = NULL;
	g_autoptr(JsonNode) timeline = NULL;
	g_autoptr(VentureReportResult) statement = NULL;
	g_autoptr(VentureReportResult) report = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autofree gchar *text = NULL;
	g_autofree gchar *tl = NULL;
	g_autofree gchar *rendered = NULL;
	VentureReportRegistry *registry;
	(void)unused;
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), TRUE);
	days(f, 1, 15);
	fee = first_fee(f, f->invoice, "applied");
	g_assert_nonnull(fee);
	registry = venture_context_get_report_registry(f->context);
	json_object_set_int_member(options, "customer_id", f->company);
	statement = venture_report_generate(venture_report_registry_lookup(registry, "customer_statement"), f->context, NULL, options, &error);
	g_assert_no_error(error);
	text = venture_report_result_render(statement, VENTURE_OUTPUT_FORMAT_JSON);
	g_assert_nonnull(strstr(text, "Late fee"));
	g_assert_nonnull(strstr(text, "1025"));
	timeline = venture_desk_activity(f->context, "company", f->company, 100, &error);
	g_assert_no_error(error);
	tl = venture_json_to_string(timeline, FALSE);
	g_assert_nonnull(strstr(tl, "late fee $25.00 applied"));
	g_clear_pointer(&tl, g_free);
	{
		g_autoptr(JsonNode) inv = venture_desk_activity(f->context, "invoice", f->invoice, 100, &error);
		g_assert_no_error(error);
		tl = venture_json_to_string(inv, FALSE);
		g_assert_nonnull(strstr(tl, "late fee $25.00 applied"));
	}
	g_assert_true(venture_late_fee_service_waive(venture_late_fee_service_get(f->db), fee, "goodwill", NULL, NULL, NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&fee);
	report = venture_report_generate(venture_report_registry_lookup(registry, "late_fees"), f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(report);
	rendered = venture_report_result_render(report, VENTURE_OUTPUT_FORMAT_JSON);
	g_assert_nonnull(strstr(rendered, "\"applied\""));
	g_assert_nonnull(strstr(rendered, "\"waived\""));
	g_assert_nonnull(strstr(rendered, "\"collected\""));
	g_assert_nonnull(strstr(rendered, "25.00"));
}

/* Generic writers may not forge a fee, and fee history cannot be removed. */
static void
test_history_guarded(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) forged = NULL;
	g_autoptr(VentureEntity) fee = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	enable(f, schedule(f, "Residential", FLAT_15, 14, NULL, NULL), TRUE);
	forged = record(f, "late_fee");
	g_object_set(forged, "invoice-id", f->invoice, "status", "applied", "occurrence-key", "forged", NULL);
	g_assert_false(venture_database_save(f->db, forged, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	days(f, 1, 15);
	fee = first_fee(f, f->invoice, "applied");
	g_assert_false(venture_database_delete(f->db, fee, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	/* A second organization-wide settings row is refused. */
	{
		g_autoptr(VentureEntity) again = record(f, "late_fee_setting");
		g_object_set(again, "enabled", FALSE, NULL);
		g_assert_false(venture_database_save(f->db, again, NULL, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
	/* A schedule's tiers are judged when written. */
	{
		g_autoptr(VentureEntity) bad = record(f, "late_fee_schedule");
		g_object_set(bad, "name", "Bad", "tiers", "[{\"day\":15,\"type\":\"percent_open\",\"percent\":1.5}]", NULL);
		g_assert_false(venture_database_save(f->db, bad, NULL, &error));
		g_assert_nonnull(error);
	}
}

/* The actions the API, venturectl and the assistant reach; the writing ones
 * stage, so the assistant goes through the staged-change approval. */
static void
test_actions(Fixture *f, gconstpointer unused)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(f->db);
	static const gchar *const fee_actions[] = { "approve", "skip", "skip_notice", "waive", "decide", NULL };
	guint i;
	(void)unused;
	for (i = 0; fee_actions[i] != NULL; i++)
	{
		VentureAction *action = venture_action_registry_lookup(registry, "late_fee", fee_actions[i]);
		gboolean stageable = FALSE;
		g_assert_nonnull(action);
		g_object_get(action, "stageable", &stageable, NULL);
		g_assert_true(stageable);
	}
	g_assert_nonnull(venture_action_registry_lookup(registry, "company", "pause_late_fees"));
	g_assert_nonnull(venture_action_registry_lookup(registry, "invoice", "pause_late_fees"));
	g_assert_nonnull(venture_action_registry_lookup(registry, "invoice", "late_fee_preview"));
	g_assert_nonnull(venture_action_registry_lookup(registry, "late_fee_setting", "pause_late_fees"));
	g_assert_nonnull(venture_action_registry_lookup(registry, "company", "resume_late_fees"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(path, fn) g_test_add(path, Fixture, NULL, setup, fn, teardown)
	ADD("/late-fees/off-by-default", test_off_by_default);
	ADD("/late-fees/grace-notice-approval", test_grace_notice_approval);
	ADD("/late-fees/approve-adds-adjustment", test_approve_adds_adjustment);
	ADD("/late-fees/notice-required", test_notice_required);
	ADD("/late-fees/auto-apply", test_auto_apply);
	ADD("/late-fees/bulk-and-skip", test_bulk_and_skip);
	ADD("/late-fees/waive", test_waive);
	ADD("/late-fees/recurring", test_recurring);
	ADD("/late-fees/markup", test_markup);
	ADD("/late-fees/caps", test_caps);
	ADD("/late-fees/pause-scopes", test_pause_scopes);
	ADD("/late-fees/conditions", test_conditions);
	ADD("/late-fees/schedule-change", test_schedule_change);
	ADD("/late-fees/preview-timeline", test_preview_timeline);
	ADD("/late-fees/rounding", test_rounding);
	ADD("/late-fees/currency", test_currency);
	ADD("/late-fees/settled-and-exempt", test_settled_and_exempt);
	ADD("/late-fees/visibility", test_visibility);
	ADD("/late-fees/history-guarded", test_history_guarded);
	ADD("/late-fees/actions", test_actions);
#undef ADD
	return g_test_run();
}
