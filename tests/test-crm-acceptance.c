/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-accounting.h"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
} Fixture;

static void
save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, row, NULL, &error));
	g_assert_no_error(error);
}

static VentureEntity *
record(Fixture *f, GType type)
{
	return g_object_new(type, "organization-id", f->org, NULL);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->config = venture_config_new();
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static gint64
id_field(VentureEntity *row, const gchar *name)
{
	gint64 value = 0;
	g_object_get(row, name, &value, NULL);
	return value;
}

static gint64
count(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	gint64 result;
	venture_query_set_organization(query, f->org);
	result = venture_database_count(f->db, query, &error);
	g_assert_no_error(error);
	return result;
}

static VentureEntity *
first(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	VentureEntity *result;
	venture_query_set_organization(query, f->org);
	result = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static VentureEntity *
action(Fixture *f, VentureEntity *row, const gchar *name, const gchar *json)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = json_from_string(json, &error);
	g_autoptr(GHashTable) params = venture_action_parameters_from_json(node, &error);
	VentureEntity *result = venture_action_registry_perform(venture_database_get_action_registry(f->db),
		venture_entity_get_entity_name(row), venture_entity_get_id(row), name, params, NULL,
		VENTURE_USER_ROLE_OWNER, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static VentureEntity *
quote_transition(Fixture *f, VentureEntity *quote, const gchar *name)
{
	g_autoptr(VentureEntity) transition = record(f, VENTURE_TYPE_QUOTE_ACTION);
	g_autoptr(GError) error = NULL;
	g_object_set(transition, "quote-id", venture_entity_get_id(quote), "action", name,
		"expected-version", venture_entity_get_version(quote), "accepted-by", "Alex Buyer", NULL);
	g_assert_true(venture_quote_service_execute(venture_database_get_quote_service(f->db),
		transition, "manual", NULL, NULL, &error));
	g_assert_no_error(error);
	return venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(quote), &error);
}

/* A successful subsystem test cannot prove that adjacent services preserve
 * sales identity, ownership and money when a real workflow crosses them. */
static void
test_capture_to_collection(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = record(f, VENTURE_TYPE_LEAD_FORM);
	g_autoptr(VentureEntity) rule = record(f, VENTURE_TYPE_LEAD_ASSIGNMENT_RULE);
	g_autoptr(VentureEntity) campaign = record(f, VENTURE_TYPE_CAMPAIGN);
	g_autoptr(VentureEntity) lead = NULL, converted = NULL, deal = NULL, replay = NULL;
	g_autoptr(VentureEntity) line = record(f, VENTURE_TYPE_DEAL_LINE);
	g_autoptr(VentureEntity) quote = NULL, sent = NULL, accepted = NULL, project = NULL;
	g_autoptr(VentureEntity) scope = NULL, delivery = NULL, ticket = NULL, invoice = NULL;
	g_autoptr(VentureEntity) activity = record(f, VENTURE_TYPE_ACTIVITY);
	g_autoptr(JsonObject) fields = json_object_new();
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autofree gchar *owner = NULL, *params = NULL;
	gint64 company_id, contact_id, deal_id, activity_id;
	(void)data;
	g_object_set(form, "name", "Website inquiry", "public-token", "acceptance-form", "active", TRUE,
		"on-duplicate", "merge", NULL);
	save(f, form);
	g_object_set(rule, "name", "Inquiry owner", "active", TRUE, "assignees", "rep", NULL);
	save(f, rule);
	g_object_set(campaign, "name", "Autumn inquiries", NULL);
	save(f, campaign);
	json_object_set_string_member(fields, "name", "Alex Buyer");
	json_object_set_string_member(fields, "company_name", "Example Client");
	json_object_set_string_member(fields, "email", "alex+web@example.invalid");
	g_assert_true(venture_lead_service_capture(venture_database_get_lead_service(f->db), "acceptance-form", fields, NULL, &error));
	json_object_set_string_member(fields, "email", "alex@example.invalid");
	g_assert_true(venture_lead_service_capture(venture_database_get_lead_service(f->db), "acceptance-form", fields, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(count(f, VENTURE_TYPE_LEAD), ==, 1);
	lead = first(f, VENTURE_TYPE_LEAD);
	g_object_get(lead, "owner", &owner, NULL);
	g_assert_cmpstr(owner, ==, "rep");
	g_clear_pointer(&owner, g_free);
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, "campaign-id", venture_entity_get_id(campaign), NULL);
	save(f, lead);
	g_object_set(activity, "subject", "Qualification follow-up", "owner", "rep", "related-type", "lead", "related-id", venture_entity_get_id(lead), NULL);
	save(f, activity);
	activity_id = venture_entity_get_id(activity);
	json_object_set_boolean_member(options, "deal", TRUE);
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, options, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(converted);
	company_id = id_field(converted, "converted-company-id");
	contact_id = id_field(converted, "converted-contact-id");
	deal_id = id_field(converted, "converted-deal-id");
	g_assert_cmpint(count(f, VENTURE_TYPE_COMPANY), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_CONTACT), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_DEAL), ==, 1);
	deal = venture_database_get(f->db, VENTURE_TYPE_DEAL, deal_id, &error);
	g_assert_no_error(error);
	g_object_get(deal, "owner", &owner, NULL);
	g_assert_cmpstr(owner, ==, "rep");
	g_assert_cmpint(id_field(deal, "company-id"), ==, company_id);
	g_assert_cmpint(id_field(deal, "contact-id"), ==, contact_id);
	g_assert_cmpint(id_field(deal, "campaign-id"), ==, venture_entity_get_id(campaign));
	{
		GType types[] = { VENTURE_TYPE_COMPANY, VENTURE_TYPE_CONTACT, VENTURE_TYPE_DEAL };
		gint64 ids[] = { company_id, contact_id, deal_id };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(types); i++)
		{
			g_autoptr(VentureEntity) linked = venture_database_get(f->db, types[i], ids[i], &error);
			g_autofree gchar *source = NULL;
			g_assert_no_error(error);
			g_assert_cmpint(venture_entity_get_organization_id(linked), ==, f->org);
			g_assert_cmpint(id_field(linked, "campaign-id"), ==, venture_entity_get_id(campaign));
			g_object_get(linked, "source", &source, NULL);
			g_assert_cmpstr(source, ==, "Website inquiry");
		}
	}
	replay = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, options, NULL, &error);
	g_assert_null(replay);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_clear_object(&activity);
	activity = venture_database_get(f->db, VENTURE_TYPE_ACTIVITY, activity_id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(id_field(activity, "related-id"), ==, venture_entity_get_id(lead));
	g_assert_cmpint(count(f, VENTURE_TYPE_INTERACTION), >=, 2);
	{
		g_autoptr(VentureLogMailer) transport = venture_log_mailer_new();
		g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(transport));
		g_autoptr(VentureMailMessage) message = venture_mail_message_new();
		g_autoptr(VentureMailMessage) queued = NULL, duplicate = NULL;
		g_object_set(message, "organization-id", f->org, "to", "alex@example.invalid",
			"subject", "Requested proposal", "text-body", "We will prepare the requested agreement.",
			"related-type", "deal", "related-id", deal_id, "idempotency-key", "proposal-response", NULL);
		queued = venture_mail_outbox_enqueue(outbox, message, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(queued);
		duplicate = venture_mail_outbox_enqueue(outbox, message, NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(queued)), ==, venture_entity_get_id(VENTURE_ENTITY(duplicate)));
		g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, NULL, NULL, &error), ==, 1);
		g_assert_no_error(error);
		g_assert_cmpuint(venture_log_mailer_get_messages(transport)->len, ==, 1);
		g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, NULL, NULL, &error), ==, 0);
		g_assert_no_error(error);
	}
	g_object_set(line, "deal-id", deal_id, "description", "Delivery agreement", "quantity", (gint64)1, NULL);
	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "1000 USD", &error));
	save(f, line);
	quote = VENTURE_ENTITY(venture_deal_service_create_quote(venture_database_get_deal_service(f->db), VENTURE_DEAL(deal), NULL, &error));
	g_assert_no_error(error);
	g_assert_nonnull(quote);
	g_object_set(quote, "billing-mode", "progress", NULL);
	save(f, quote);
	sent = quote_transition(f, quote, "send");
	accepted = quote_transition(f, sent, "accept");
	project = action(f, accepted, "handoff", "{\"name\":\"Client delivery\",\"owner\":\"rep\",\"scope\":\"Agreed work\"}");
	g_assert_cmpint(id_field(project, "customer-id"), ==, company_id);
	g_assert_cmpint(id_field(project, "deal-id"), ==, deal_id);
	scope = first(f, VENTURE_TYPE_PROJECT_SCOPE);
	params = g_strdup_printf("{\"key\":\"first-slice\",\"title\":\"First delivery\",\"scope_id\":%" G_GINT64_FORMAT
		",\"due\":\"2026-10-01\",\"amount\":\"400 USD\"}", venture_entity_get_id(scope));
	delivery = action(f, project, "plan_work", params);
	ticket = venture_database_get(f->db, VENTURE_TYPE_TICKET, id_field(delivery, "ticket-id"), &error);
	g_assert_no_error(error);
	g_object_set(ticket, "status", VENTURE_TICKET_STATUS_DONE, NULL);
	save(f, ticket);
	replay = action(f, delivery, "accept", "{\"evidence\":\"Customer accepted first slice\"}");
	invoice = action(f, delivery, "bill", "{}");
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 40000);
	now = g_date_time_new_now_utc();
	g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db), venture_entity_get_id(invoice), now, NULL, &error));
	g_assert_no_error(error);
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
	g_clear_object(&replay);
	replay = action(f, delivery, "bill", "{}");
	g_assert_cmpint(venture_entity_get_id(replay), ==, venture_entity_get_id(invoice));
	g_assert_cmpint(count(f, VENTURE_TYPE_INVOICE), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_PAYMENT), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/crm-acceptance/capture-to-collection", Fixture, NULL, setup, test_capture_to_collection, teardown);
	return g_test_run();
}
