/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

/*
 * Referral tracking: who sent whom, followed to a won customer, and the
 * reward the sender is given. Every test drives the ordinary writers -- a
 * plain save, lead capture, a form, the relay, conversion, billing -- because
 * the point of the module is that none of them has to know it exists.
 */

typedef struct {
	VentureConfig *config;
	VentureDatabase *db;
	VentureContext *context;
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
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureEntity *
record(Fixture *f, const gchar *type)
{
	GType t = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), type);
	VentureEntity *e;
	g_assert_cmpuint(t, !=, G_TYPE_INVALID);
	e = g_object_new(t, NULL);
	venture_entity_set_organization_id(e, f->org);
	return e;
}

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, e, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
refused(Fixture *f, VentureEntity *e, const gchar *needle)
{
	g_autoptr(GError) error = NULL;
	g_assert_false(venture_database_save(f->db, e, NULL, &error));
	g_assert_nonnull(error);
	if (strstr(error->message, needle) == NULL)
		g_error("expected \"%s\" in \"%s\"", needle, error->message);
}

static void
field(VentureEntity *e, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(e, name, value, &error));
	g_assert_no_error(error);
}

static gint64
number(VentureEntity *e, const gchar *name)
{
	gint64 value = 0;
	g_object_get(e, name, &value, NULL);
	return value;
}

static gint
choice(VentureEntity *e, const gchar *name)
{
	gint value = 0;
	g_object_get(e, name, &value, NULL);
	return value;
}

static gchar *
text(VentureEntity *e, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(e, name, &value, NULL);
	return value;
}

static VentureEntity *
reread(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *fresh = venture_database_get(f->db, G_OBJECT_TYPE(e), venture_entity_get_id(e), &error);
	g_assert_no_error(error);
	g_assert_nonnull(fresh);
	return fresh;
}

static VentureEntity *
program(Fixture *f, VentureReferralRewardKind kind, const gchar *amount)
{
	VentureEntity *p = record(f, "referral_program");
	g_object_set(p, "name", "Friends", "active", TRUE, "reward-kind", kind,
		"landing-url", "https://shop.example/join", NULL);
	if (amount != NULL) field(p, "reward-amount", amount);
	save(f, p);
	return p;
}

static VentureEntity *
company(Fixture *f, const gchar *name)
{
	VentureEntity *c = record(f, "company");
	g_object_set(c, "name", name, NULL);
	save(f, c);
	return c;
}

static VentureEntity *
code_for(Fixture *f, VentureEntity *referrer)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *code = venture_referral_code_for(f->db, referrer, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(code);
	return code;
}

static VentureEntity *
lead(Fixture *f, const gchar *name, const gchar *email, const gchar *code)
{
	VentureEntity *l = record(f, "lead");
	g_object_set(l, "name", name, "company-name", name, "email", email, "referral-code", code, NULL);
	save(f, l);
	return l;
}

static GPtrArray *
rows(Fixture *f, const gchar *type, const gchar *name, gint64 value)
{
	g_autoptr(VentureQuery) query = venture_query_new(venture_entity_registry_lookup_any(venture_entity_registry_get_default(), type));
	g_autoptr(GError) error = NULL;
	GPtrArray *found;
	venture_query_set_organization(query, f->org);
	if (name != NULL) venture_query_add_filter_int(query, name, VENTURE_FILTER_OP_EQ, value, NULL);
	found = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return found;
}

/* The one referral a lead carries, which must exist. */
static VentureEntity *
referral_of(Fixture *f, VentureEntity *l)
{
	g_autoptr(GPtrArray) found = rows(f, "referral", "lead-id", venture_entity_get_id(l));
	g_assert_cmpuint(found->len, ==, 1);
	return g_object_ref(g_ptr_array_index(found, 0));
}

static VentureEntity *
convert(Fixture *f, VentureEntity *l)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) current = reread(f, l);
	VentureEntity *converted;
	/* Only a qualified lead converts. */
	if (choice(current, "status") != VENTURE_LEAD_QUALIFIED)
	{
		g_object_set(current, "status", VENTURE_LEAD_QUALIFIED, NULL);
		save(f, current);
	}
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db),
		current, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(converted);
	return converted;
}

static VentureEntity *
reward_of(Fixture *f, VentureEntity *referral)
{
	g_autoptr(GPtrArray) found = rows(f, "referral_reward", "referral-id", venture_entity_get_id(referral));
	g_assert_cmpuint(found->len, ==, 1);
	return g_object_ref(g_ptr_array_index(found, 0));
}

static void
money_is(VentureEntity *e, const gchar *name, gint64 minor, const gchar *currency)
{
	g_autoptr(VentureMoney) money = NULL;
	g_object_get(e, name, &money, NULL);
	g_assert_nonnull(money);
	g_assert_cmpint(venture_money_get_amount(money), ==, minor);
	g_assert_cmpstr(venture_money_get_currency(money), ==, currency);
}

static void
test_catalog(void)
{
	static const gchar *const names[] = { "referral_program", "referral_code", "referral", "referral_reward" };
	g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
	guint i;
	venture_module_registry_register_builtins(modules);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		VentureModule *module = venture_module_registry_get_module_for_type(modules, names[i]);
		g_assert_nonnull(module);
		g_assert_cmpstr(venture_module_get_name(module), ==, "referrals");
	}
}

/* A customer's code is made once and is the same code every time it is
 * asked for; the link to share is the program's page carrying it. */
static void
test_code(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = program(f, VENTURE_REFERRAL_REWARD_NONE, NULL);
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) first = code_for(f, acme);
	g_autoptr(VentureEntity) again = code_for(f, acme);
	g_autoptr(VentureEntity) copy = record(f, "referral_code");
	g_autoptr(VentureEntity) orphan = record(f, "referral_code");
	g_autoptr(VentureEntity) typed = record(f, "referral_code");
	g_autofree gchar *code = text(first, "code");
	g_autofree gchar *link = text(first, "link");
	g_autofree gchar *expected = g_strdup_printf("https://shop.example/join?ref=%s", code);
	g_autofree gchar *normal = NULL;
	(void)data;
	g_assert_cmpint(venture_entity_get_id(first), ==, venture_entity_get_id(again));
	g_assert_cmpint(number(first, "company-id"), ==, venture_entity_get_id(acme));
	g_assert_cmpint(number(first, "program-id"), ==, venture_entity_get_id(p));
	g_assert_cmpuint(strlen(code), >=, 6);
	{
		g_autofree gchar *upper = g_ascii_strup(code, -1);
		g_assert_cmpstr(code, ==, upper);
	}
	g_assert_cmpstr(link, ==, expected);
	/* Codes are unique within the organization, whatever their case. */
	g_object_set(copy, "program-id", venture_entity_get_id(p), "company-id", venture_entity_get_id(acme), NULL);
	{
		g_autofree gchar *lower = g_ascii_strdown(code, -1);
		g_object_set(copy, "code", lower, NULL);
	}
	refused(f, copy, "already");
	/* A code that names nobody would reward nobody. */
	g_object_set(orphan, "program-id", venture_entity_get_id(p), "code", "NOBODY1", NULL);
	refused(f, orphan, "referrer");
	/* A typed code is kept, in its stored spelling. */
	g_object_set(typed, "program-id", venture_entity_get_id(p), "company-id", venture_entity_get_id(acme),
		"code", "  spring-26 ", NULL);
	save(f, typed);
	normal = text(typed, "code");
	g_assert_cmpstr(normal, ==, "SPRING-26");
}

/* A lead that arrives with a code, by any writer, becomes a referral; a
 * code that matches nothing leaves the lead alone rather than losing it. */
static void
test_lead_attributed(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = program(f, VENTURE_REFERRAL_REWARD_NONE, NULL);
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) code = code_for(f, acme);
	g_autofree gchar *spelled = NULL;
	g_autoptr(VentureEntity) referred = NULL;
	g_autoptr(VentureEntity) referral = NULL;
	g_autoptr(VentureEntity) stranger = NULL;
	g_autoptr(GPtrArray) all = NULL;
	(void)data;
	{
		g_autofree gchar *raw = text(code, "code");
		g_autofree gchar *lower = g_ascii_strdown(raw, -1);
		spelled = g_strdup_printf(" %s ", lower);
	}
	referred = lead(f, "Bea", "bea@example.com", spelled);
	referral = referral_of(f, referred);
	g_assert_cmpint(choice(referral, "status"), ==, VENTURE_REFERRAL_PENDING);
	g_assert_cmpint(number(referral, "referrer-company-id"), ==, venture_entity_get_id(acme));
	g_assert_cmpint(number(referral, "code-id"), ==, venture_entity_get_id(code));
	g_assert_cmpint(number(referral, "program-id"), ==, venture_entity_get_id(p));
	stranger = lead(f, "Cal", "cal@example.com", "NO-SUCH-CODE");
	all = rows(f, "referral", NULL, 0);
	g_assert_cmpuint(all->len, ==, 1);
	/* A retired code attributes nothing either. */
	g_object_set(code, "retired", TRUE, NULL);
	save(f, code);
	{
		g_autofree gchar *raw = text(code, "code");
		g_autoptr(VentureEntity) late = lead(f, "Dee", "dee@example.com", raw);
		g_autoptr(GPtrArray) after = rows(f, "referral", NULL, 0);
		g_assert_cmpuint(after->len, ==, 1);
	}
}

/* The referral follows its lead: qualified, ruled out, back, and won. */
static void
test_status_follows(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = program(f, VENTURE_REFERRAL_REWARD_NONE, NULL);
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) code = code_for(f, acme);
	g_autofree gchar *raw = text(code, "code");
	g_autoptr(VentureEntity) referred = lead(f, "Bea Works", "bea@example.com", raw);
	g_autoptr(VentureEntity) referral = NULL;
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(GDateTime) won = NULL;
	(void)data;
	g_object_set(referred, "status", VENTURE_LEAD_QUALIFIED, NULL);
	save(f, referred);
	referral = referral_of(f, referred);
	g_assert_cmpint(choice(referral, "status"), ==, VENTURE_REFERRAL_QUALIFIED);
	g_clear_object(&referral);
	g_object_set(referred, "status", VENTURE_LEAD_UNQUALIFIED, "unqualified-reason", "Budget", NULL);
	save(f, referred);
	referral = referral_of(f, referred);
	g_assert_cmpint(choice(referral, "status"), ==, VENTURE_REFERRAL_LOST);
	/* Typed by hand, a status that disagrees with the lead is put back. */
	g_object_set(referral, "status", VENTURE_REFERRAL_WON, NULL);
	save(f, referral);
	g_assert_cmpint(choice(referral, "status"), ==, VENTURE_REFERRAL_LOST);
	g_clear_object(&referral);
	g_object_set(referred, "status", VENTURE_LEAD_WORKING, NULL);
	save(f, referred);
	converted = convert(f, referred);
	referral = referral_of(f, referred);
	g_assert_cmpint(choice(referral, "status"), ==, VENTURE_REFERRAL_WON);
	g_assert_cmpint(number(referral, "company-id"), ==, number(converted, "converted-company-id"));
	g_assert_cmpint(number(referral, "contact-id"), ==, number(converted, "converted-contact-id"));
	g_object_get(referral, "won-at", &won, NULL);
	g_assert_nonnull(won);
}

/* Winning the customer gives the referrer a credit note, once, and the
 * reward is on the referrer's record and on the new customer's. */
static void
test_credit_reward(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = program(f, VENTURE_REFERRAL_REWARD_CREDIT, "25 USD");
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) code = code_for(f, acme);
	g_autofree gchar *raw = text(code, "code");
	g_autoptr(VentureEntity) referred = lead(f, "Bea Works", "bea@example.com", raw);
	g_autoptr(VentureEntity) converted = convert(f, referred);
	g_autoptr(VentureEntity) referral = referral_of(f, referred);
	g_autoptr(VentureEntity) reward = reward_of(f, referral);
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(GPtrArray) given = NULL;
	g_autoptr(GPtrArray) received = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *kind = NULL;
	g_autoptr(GDateTime) applied = NULL;
	(void)data;
	g_assert_cmpint(choice(reward, "status"), ==, VENTURE_REFERRAL_REWARD_APPLIED);
	g_assert_cmpint(choice(reward, "kind"), ==, VENTURE_REFERRAL_REWARD_CREDIT);
	g_assert_cmpint(number(reward, "company-id"), ==, venture_entity_get_id(acme));
	g_assert_cmpint(number(reward, "referred-company-id"), ==, number(converted, "converted-company-id"));
	g_object_get(reward, "applied-at", &applied, NULL);
	g_assert_nonnull(applied);
	money_is(reward, "amount", 2500, "USD");
	credit = venture_database_get(f->db, VENTURE_TYPE_CUSTOMER_CREDIT, number(reward, "credit-id"), &error);
	g_assert_no_error(error);
	g_assert_nonnull(credit);
	g_assert_cmpint(number(credit, "customer-id"), ==, venture_entity_get_id(acme));
	kind = text(credit, "kind");
	g_assert_cmpstr(kind, ==, "credit_note");
	money_is(credit, "amount", 2500, "USD");
	/* Saving the won referral again gives nothing more. */
	g_object_set(referral, "notes", "Thanked them", NULL);
	save(f, referral);
	given = rows(f, "referral_reward", "company-id", venture_entity_get_id(acme));
	received = rows(f, "referral_reward", "referred-company-id", number(converted, "converted-company-id"));
	g_assert_cmpuint(given->len, ==, 1);
	g_assert_cmpuint(received->len, ==, 1);
	/* Only the service gives rewards: one typed in would claim money moved
	 * that never did, since the zero status is "applied". */
	{
		g_autoptr(VentureEntity) forged = record(f, "referral_reward");
		g_object_set(forged, "referral-id", venture_entity_get_id(referral), "company-id", venture_entity_get_id(acme), NULL);
		refused(f, forged, "given by");
		g_object_set(reward, "failure", "edited", NULL);
		refused(f, reward, "given by");
		g_clear_object(&reward);
		reward = reward_of(f, referral);
	}
	/* A reward that was given is evidence, not a draft. */
	g_assert_false(venture_database_delete(f->db, reward, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "given"));
}

static gint64
subscribe(Fixture *f, VentureEntity *customer)
{
	g_autoptr(VentureEntity) plan = record(f, "plan");
	g_autoptr(VentureEntity) price = record(f, "plan_price");
	g_autoptr(VentureEntity) start = record(f, "billing_request");
	g_object_set(plan, "name", "Starter", "code", "starter", "active", TRUE, NULL);
	save(f, plan);
	g_object_set(price, "plan-id", venture_entity_get_id(plan), "currency", "USD", "active", TRUE, "per-seat", TRUE, NULL);
	field(price, "amount", "30 USD");
	save(f, price);
	g_object_set(start, "action", "start", "company-id", venture_entity_get_id(customer),
		"plan-price-id", venture_entity_get_id(price), "seats", (gint64)2, NULL);
	field(start, "at", "2026-01-01");
	save(f, start);
	return number(start, "subscription-id");
}

/* A free period credits the referrer's next invoice in full: the reward
 * is carried on the subscription and the renewal invoice is paid by it. */
static void
test_free_period(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = program(f, VENTURE_REFERRAL_REWARD_FREE_PERIOD, NULL);
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	gint64 sub_id = subscribe(f, acme);
	g_autoptr(VentureEntity) code = code_for(f, acme);
	g_autofree gchar *raw = text(code, "code");
	g_autoptr(VentureEntity) referred = lead(f, "Bea Works", "bea@example.com", raw);
	g_autoptr(VentureEntity) converted = convert(f, referred);
	g_autoptr(VentureEntity) referral = referral_of(f, referred);
	g_autoptr(VentureEntity) reward = reward_of(f, referral);
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(VentureEntity) renew = record(f, "billing_request");
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_assert_cmpint(choice(reward, "status"), ==, VENTURE_REFERRAL_REWARD_APPLIED);
	g_assert_cmpint(number(reward, "subscription-id"), ==, sub_id);
	money_is(reward, "amount", 6000, "USD");
	sub = venture_database_get(f->db, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, sub_id, &error);
	g_assert_no_error(error);
	money_is(sub, "pending-adjustment", -6000, "USD");
	g_object_set(renew, "action", "renew", "subscription-id", sub_id, NULL);
	field(renew, "at", "2026-02-01");
	save(f, renew);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		number(renew, "invoice-id"), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(balance);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
}

/* A reward that cannot be given does not undo winning the customer. It is
 * recorded with the reason, and given once the cause is fixed -- once. */
static void
test_reward_failure(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = program(f, VENTURE_REFERRAL_REWARD_FREE_PERIOD, NULL);
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) code = code_for(f, acme);
	g_autofree gchar *raw = text(code, "code");
	g_autoptr(VentureEntity) referred = lead(f, "Bea Works", "bea@example.com", raw);
	g_autoptr(VentureEntity) converted = convert(f, referred);
	g_autoptr(VentureEntity) referral = referral_of(f, referred);
	g_autoptr(VentureEntity) reward = reward_of(f, referral);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *why = text(reward, "failure");
	(void)data;
	g_assert_cmpint(choice(converted, "status"), ==, VENTURE_LEAD_CONVERTED);
	g_assert_cmpint(choice(reward, "status"), ==, VENTURE_REFERRAL_REWARD_FAILED);
	g_assert_nonnull(why);
	g_assert_nonnull(strstr(why, "subscription"));
	subscribe(f, acme);
	g_assert_true(venture_referral_reward_apply(f->db, reward, NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) fresh = reread(f, reward);
		g_assert_cmpint(choice(fresh, "status"), ==, VENTURE_REFERRAL_REWARD_APPLIED);
		g_assert_false(venture_referral_reward_apply(f->db, fresh, NULL, &error));
		g_assert_nonnull(error);
	}
}

/* Public capture carries the code in, and a capture that merges into a
 * lead nobody referred yet attributes that lead. */
static void
test_capture(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = program(f, VENTURE_REFERRAL_REWARD_NONE, NULL);
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) code = code_for(f, acme);
	g_autoptr(VentureEntity) form = record(f, "lead_form");
	g_autoptr(VentureEntity) earlier = lead(f, "Eve", "eve@example.com", NULL);
	g_autoptr(JsonObject) fields = json_object_new();
	g_autoptr(JsonObject) again = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autofree gchar *raw = text(code, "code");
	(void)data;
	g_object_set(form, "name", "Join", "public-token", "join-token", "active", TRUE, NULL);
	save(f, form);
	json_object_set_string_member(fields, "name", "Bea");
	json_object_set_string_member(fields, "email", "bea@example.com");
	json_object_set_string_member(fields, "referral_code", raw);
	g_assert_true(venture_lead_service_capture(venture_database_get_lead_service(f->db), "join-token", fields, NULL, &error));
	g_assert_no_error(error);
	json_object_set_string_member(again, "name", "Eve");
	json_object_set_string_member(again, "email", "eve@example.com");
	json_object_set_string_member(again, "referral_code", raw);
	g_assert_true(venture_lead_service_capture(venture_database_get_lead_service(f->db), "join-token", again, NULL, &error));
	g_assert_no_error(error);
	all = rows(f, "referral", NULL, 0);
	g_assert_cmpuint(all->len, ==, 2);
	{
		g_autoptr(VentureEntity) merged = referral_of(f, earlier);
		g_assert_cmpint(number(merged, "referrer-company-id"), ==, venture_entity_get_id(acme));
	}
}

static VentureEntity *
form_field(Fixture *f, VentureEntity *form, const gchar *key, VentureFormFieldKind kind, const gchar *maps_to, gint64 position)
{
	VentureEntity *q = record(f, "form_field");
	g_object_set(q, "form-id", venture_entity_get_id(form), "key", key, "label", key, "kind", kind,
		"position", position, "maps-to", maps_to, NULL);
	save(f, q);
	return q;
}

/* A form that makes leads attributes them from a question mapped to the
 * code, and the signed relay carries a connected site's code beside the
 * answers without the form asking for it. */
static void
test_form_and_relay(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = program(f, VENTURE_REFERRAL_REWARD_NONE, NULL);
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) code = code_for(f, acme);
	g_autoptr(VentureEntity) form = record(f, "form");
	g_autoptr(VentureEntity) name = NULL, email = NULL, ref = NULL, version = NULL;
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(VentureEntity) response = NULL;
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(JsonNode) envelope = NULL, result = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autofree gchar *raw = text(code, "code");
	g_autofree gchar *json = NULL;
	VentureFormsOutcome outcome;
	(void)data;
	g_object_set(form, "name", "Join us", "title", "Join us", "public-token", "join-form", "state", VENTURE_FORM_LIVE,
		"create-lead", TRUE, "allowed-origins", "https://shop.example", NULL);
	save(f, form);
	name = form_field(f, form, "name", VENTURE_FORM_FIELD_SHORT_TEXT, "name", 10);
	email = form_field(f, form, "email", VENTURE_FORM_FIELD_EMAIL, "email", 20);
	ref = form_field(f, form, "ref", VENTURE_FORM_FIELD_HIDDEN, "referral_code", 30);
	version = venture_forms_publish(f->db, form, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(version);
	/* Publishing saved the form; submit the row as it now is. */
	{
		VentureEntity *fresh = reread(f, form);
		g_object_unref(form);
		form = fresh;
	}
	venture_forms_answers_add(answers, "name", "Bea");
	venture_forms_answers_add(answers, "email", "bea@example.com");
	venture_forms_answers_add(answers, "ref", raw);
	g_assert_true(venture_forms_submit(f->db, form, answers, NULL, now, &outcome, &response, &errors, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_ACCEPTED);
	{
		g_autoptr(VentureEntity) captured = venture_database_get(f->db, VENTURE_TYPE_LEAD, number(response, "lead-id"), NULL);
		g_autoptr(VentureEntity) referral = NULL;
		g_assert_nonnull(captured);
		referral = referral_of(f, captured);
		g_assert_cmpint(number(referral, "referrer-company-id"), ==, venture_entity_get_id(acme));
	}
	json = g_strdup_printf("{\"version\":1,\"form_id\":%" G_GINT64_FORMAT ",\"form_version\":%" G_GINT64_FORMAT
		",\"submission_id\":\"site-1\",\"answers\":{\"name\":\"Cal\",\"email\":\"cal@example.com\"},\"referral_code\":\"%s\"}",
		venture_entity_get_id(form), number(version, "number"), raw);
	envelope = json_from_string(json, &error);
	g_assert_no_error(error);
	result = venture_forms_relay(f->db, f->org, "scope", "https://shop.example", json_node_get_object(envelope), now, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_true(json_object_get_boolean_member(json_node_get_object(result), "accepted"));
	all = rows(f, "referral", NULL, 0);
	g_assert_cmpuint(all->len, ==, 2);
}

/* A referral may be recorded by hand, for a customer who never was a
 * lead; it must still name somebody on both ends, and not the same one. */
static void
test_manual(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = program(f, VENTURE_REFERRAL_REWARD_CREDIT, "10 USD");
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) newco = company(f, "Newco");
	g_autoptr(VentureEntity) contact = record(f, "contact");
	g_autoptr(VentureEntity) manual = record(f, "referral");
	g_autoptr(VentureEntity) self = record(f, "referral");
	g_autoptr(VentureEntity) nobody = record(f, "referral");
	g_autoptr(VentureEntity) twice = record(f, "referral");
	g_autoptr(VentureEntity) referred = lead(f, "Bea", "bea@example.com", NULL);
	g_autoptr(VentureEntity) first = record(f, "referral");
	g_autoptr(VentureEntity) reward = NULL;
	(void)data;
	g_object_set(contact, "name", "Ann Acme", "company-id", venture_entity_get_id(acme), NULL);
	save(f, contact);
	g_object_set(manual, "referrer-contact-id", venture_entity_get_id(contact), "company-id", venture_entity_get_id(newco),
		"program-id", venture_entity_get_id(p), "status", VENTURE_REFERRAL_WON, NULL);
	save(f, manual);
	/* A contact's reward goes to the contact's company. */
	reward = reward_of(f, manual);
	g_assert_cmpint(choice(reward, "status"), ==, VENTURE_REFERRAL_REWARD_APPLIED);
	g_assert_cmpint(number(reward, "company-id"), ==, venture_entity_get_id(acme));
	g_assert_cmpint(number(reward, "contact-id"), ==, venture_entity_get_id(contact));
	g_object_set(self, "referrer-company-id", venture_entity_get_id(acme), "company-id", venture_entity_get_id(acme), NULL);
	refused(f, self, "themselves");
	g_object_set(nobody, "company-id", venture_entity_get_id(newco), NULL);
	refused(f, nobody, "referrer");
	g_object_set(first, "lead-id", venture_entity_get_id(referred), "referrer-company-id", venture_entity_get_id(acme), NULL);
	save(f, first);
	g_object_set(twice, "lead-id", venture_entity_get_id(referred), "referrer-company-id", venture_entity_get_id(newco), NULL);
	refused(f, twice, "already");
}

/* Referrals per referrer, how many of their leads were won, and what
 * they were given, in the reward's own currency. */
static void
test_report(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = program(f, VENTURE_REFERRAL_REWARD_CREDIT, "25 USD");
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) code = code_for(f, acme);
	g_autofree gchar *raw = text(code, "code");
	g_autoptr(VentureEntity) a = lead(f, "Bea Works", "bea@example.com", raw);
	g_autoptr(VentureEntity) b = lead(f, "Cal Works", "cal@example.com", raw);
	g_autoptr(VentureEntity) won = convert(f, a);
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "referrals");
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *referrer = NULL, *amount = NULL;
	const GValue *cell;
	(void)data;
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	referrer = venture_report_result_format_cell(result, 0, "referrer");
	g_assert_cmpstr(referrer, ==, "Acme");
	cell = venture_report_result_get_cell(result, 0, "referrals");
	g_assert_cmpfloat(g_value_get_double(cell), ==, 2);
	cell = venture_report_result_get_cell(result, 0, "won");
	g_assert_cmpfloat(g_value_get_double(cell), ==, 1);
	cell = venture_report_result_get_cell(result, 0, "conversion_rate");
	g_assert_cmpfloat(g_value_get_double(cell), ==, 50);
	cell = venture_report_result_get_cell(result, 0, "rewards");
	g_assert_cmpfloat(g_value_get_double(cell), ==, 1);
	amount = venture_report_result_format_cell(result, 0, "reward_value");
	g_assert_nonnull(strstr(amount, "25.00"));
}

/* With the module off, a lead carrying a code saves exactly as before. */
static void
test_module_off(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = program(f, VENTURE_REFERRAL_REWARD_NONE, NULL);
	g_autoptr(VentureEntity) acme = company(f, "Acme");
	g_autoptr(VentureEntity) code = code_for(f, acme);
	g_autofree gchar *raw = text(code, "code");
	g_autoptr(VentureEntity) referred = NULL;
	g_autoptr(GPtrArray) all = NULL;
	(void)data;
	venture_config_set_module_enabled(f->config, "referrals", FALSE);
	referred = lead(f, "Bea", "bea@example.com", raw);
	venture_config_set_module_enabled(f->config, "referrals", TRUE);
	all = rows(f, "referral", NULL, 0);
	g_assert_cmpuint(all->len, ==, 0);
}

static gchar *
scalar(VentureDatabase *database, const gchar *sql)
{
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GError) error = NULL;
	result = venture_database_query_raw(database, sql, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	return g_strdup(orm_row_get_string(orm_result_get_row(result), 0));
}

static void
execute(VentureDatabase *database, const gchar *sql)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_execute(database, sql, NULL, &error));
	g_assert_no_error(error);
}

/*
 * An install from before referrals: leads without the code column, no
 * referral tables, no history of either script. Upgrading keeps its leads,
 * adds both, records both scripts, and a restart finds nothing to do. If
 * the scripts ran before reconciliation, or named a column the field
 * tables do not make, this is where startup would refuse.
 */
static void
test_upgrade(gconstpointer data)
{
	const gchar *backend = data;
	gboolean postgres = g_str_equal(backend, "postgresql");
	g_autofree gchar *directory = NULL, *uri = NULL, *schema = NULL, *setup_sql = NULL, *cleanup_sql = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;
	guint run;
	if (postgres)
	{
		g_autofree gchar *uuid = NULL;
		if (g_getenv("VENTURE_TEST_MIGRATION_POSTGRES_URI") == NULL)
		{
			g_test_skip("Set VENTURE_TEST_MIGRATION_POSTGRES_URI for a disposable PostgreSQL server");
			return;
		}
		uri = g_strdup(g_getenv("VENTURE_TEST_MIGRATION_POSTGRES_URI"));
		uuid = g_uuid_string_random();
		g_strdelimit(uuid, "-", '_');
		schema = g_strconcat("venture_referrals_", uuid, NULL);
		setup_sql = g_strdup_printf("CREATE SCHEMA %s; SET search_path TO %s", schema, schema);
		cleanup_sql = g_strdup_printf("DROP SCHEMA %s CASCADE", schema);
	}
	else
	{
		directory = g_dir_make_tmp("venture-referrals-XXXXXX", &error);
		g_assert_no_error(error);
		uri = g_strdup_printf("sqlite://%s/database.db", directory);
	}
	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	if (postgres) execute(database, setup_sql);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	execute(database, "INSERT INTO leads (uuid, organization_id, name, email, status, version) "
		"VALUES ('old-lead', 1, 'Old inquiry', 'old@example.com', 'new', 1)");
	execute(database, "DELETE FROM schema_migrations WHERE version IN (905, 906)");
	execute(database, "DROP TABLE referral_rewards; DROP TABLE referrals; DROP TABLE referral_codes; DROP TABLE referral_programs");
	execute(database, "DROP INDEX idx_leads_referral_code");
	execute(database, "ALTER TABLE leads DROP COLUMN referral_code");
	for (run = 0; run < 2; run++)
	{
		g_autofree gchar *history = NULL, *kept = NULL, *code = NULL, *tables = NULL;
		g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		history = scalar(database, "SELECT CAST(COUNT(*) AS TEXT) FROM schema_migrations WHERE version IN (905, 906)");
		g_assert_cmpstr(history, ==, "2");
		kept = scalar(database, "SELECT name FROM leads WHERE uuid = 'old-lead'");
		g_assert_cmpstr(kept, ==, "Old inquiry");
		code = scalar(database, "SELECT CAST(COUNT(*) AS TEXT) FROM leads WHERE uuid = 'old-lead' AND referral_code IS NULL");
		g_assert_cmpstr(code, ==, "1");
		tables = scalar(database, "SELECT CAST(COUNT(*) AS TEXT) FROM referral_rewards");
		g_assert_cmpstr(tables, ==, "0");
	}
	if (postgres) execute(database, cleanup_sql);
	g_clear_object(&database);
	if (directory != NULL) venture_test_remove_tree(directory);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/referrals/catalog", test_catalog);
	g_test_add_data_func("/referrals/upgrade-sqlite", "sqlite", test_upgrade);
	g_test_add_data_func("/referrals/upgrade-postgresql", "postgresql", test_upgrade);
#define ADD(path, fn) g_test_add(path, Fixture, NULL, setup, fn, teardown)
	ADD("/referrals/code", test_code);
	ADD("/referrals/lead-attributed", test_lead_attributed);
	ADD("/referrals/status-follows", test_status_follows);
	ADD("/referrals/credit-reward", test_credit_reward);
	ADD("/referrals/free-period", test_free_period);
	ADD("/referrals/reward-failure", test_reward_failure);
	ADD("/referrals/capture", test_capture);
	ADD("/referrals/form-and-relay", test_form_and_relay);
	ADD("/referrals/manual", test_manual);
	ADD("/referrals/report", test_report);
	ADD("/referrals/module-off", test_module_off);
#undef ADD
	return g_test_run();
}
