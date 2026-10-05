/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * /api/v1/lightsite/billing: the operator billing each Lightsite customer from
 * its own billing organization. A workspace administrator's service token
 * binds a business (an organization made by the sign-up route) to a plan;
 * Venture finds or creates the customer company for that business in the
 * billing organization and starts, or changes at renewal, its monthly
 * subscription through VentureBillingService. Two reads answer what the
 * business is on, what it owes, whether it is overdue or a payment failed,
 * and whether the business has connected its own Stripe.
 *
 * Everything goes through a real hosted server and real bearer tokens,
 * because the authority checks live in the request path as much as in
 * the service.
 *
 * What breaks if these regress: a retried subscribe starts a second
 * subscription or issues a second first invoice; a reused key silently
 * changes a customer's plan; a plan change bills now instead of at renewal;
 * an overdue invoice or a failed payment reads as healthy; a Stripe key or
 * account id reaches Lightsite; an ordinary member's token -- or a browser
 * session -- moves money; an install that never configured a billing
 * organization bills from whichever one happened to be first.
 */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

#define WORKSPACE "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a"
#define ORIGIN "https://example.test:8443"
#define AUTHORITY "example.test:8443"
#define ISSUER "https://id.example.test/realms/lightsite"
#define SIGNUPS "/api/v1/lightsite/signups"
#define SUBSCRIPTIONS "/api/v1/lightsite/billing/subscriptions"
#define OVERVIEW "/api/v1/lightsite/billing"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	VentureTenantService *service;
	SoupSession *session;
	gchar *directory;
	gchar *admin_secret;
	gint64 admin_id;
	gint64 home;
	gint64 prices[3];
} Fixture;

typedef struct { gboolean done; GBytes *body; GError *error; } Reply;

/* team, growth, starter: what the operator sells, in the billing organization. */
static const struct { const gchar *code; const gchar *name; const gchar *amount; } PLANS[] = {
	{ "team", "Plan A", "10 USD" },
	{ "growth", "Plan B", "20 USD" },
	{ "starter", "Plan C", "30 USD" },
};

static void
received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static guint
exchange(Fixture *f, const gchar *method, const gchar *path, const gchar *host, const gchar *cookie,
	const gchar *token, const gchar *content_type, const gchar *payload, gchar **out_cookie, gchar **out_body,
	gboolean *no_store)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	Reply reply = { FALSE, NULL, NULL };
	const gchar *bytes, *cache;
	gsize length;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_replace(headers, "Host", host ? host : AUTHORITY);
	if (cookie) soup_message_headers_replace(headers, "Cookie", cookie);
	if (token) {
		g_autofree gchar *header = g_strconcat("Bearer ", token, NULL);
		soup_message_headers_replace(headers, "Authorization", header);
	}
	if (payload) {
		g_autoptr(GBytes) body = g_bytes_new(payload, strlen(payload));
		soup_message_set_request_body_from_bytes(message, content_type, body);
	}
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	if (out_cookie) {
		const gchar *set = soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie");
		*out_cookie = set ? g_strndup(set, strcspn(set, ";")) : NULL;
	}
	cache = soup_message_headers_get_one(soup_message_get_response_headers(message), "Cache-Control");
	if (no_store) *no_store = g_strcmp0(cache, "no-store") == 0;
	bytes = g_bytes_get_data(reply.body, &length);
	if (out_body) *out_body = g_strndup(bytes, length);
	g_bytes_unref(reply.body);
	return soup_message_get_status(message);
}

/* One request to a billing route. Every answer, refusals included, must be
 * uncacheable: a cached billing view would show one business another's. */
static gchar *
call_text(Fixture *f, const gchar *method, const gchar *path, const gchar *token, const gchar *host,
	const gchar *payload, guint expected)
{
	g_autofree gchar *body = NULL;
	gboolean no_store = FALSE;
	guint status = exchange(f, method, path, host, NULL, token, payload ? "application/json" : NULL, payload,
		NULL, &body, &no_store);
	if (status != expected) g_test_message("unexpected %u for %s %s: %s", status, method, path, body);
	g_assert_cmpuint(status, ==, expected);
	g_assert_true(no_store);
	return g_steal_pointer(&body);
}

static JsonNode *
parse(const gchar *text)
{
	g_autoptr(GError) error = NULL;
	JsonNode *node = venture_json_parse(text, &error);
	g_assert_no_error(error); g_assert_nonnull(node);
	return node;
}

static JsonNode *
call(Fixture *f, const gchar *method, const gchar *path, const gchar *token, const gchar *host,
	const gchar *payload, guint expected)
{
	g_autofree gchar *text = call_text(f, method, path, token, host, payload, expected);
	return parse(text);
}

static gchar *
sub_body(gint64 organization_id, const gchar *plan, const gchar *key)
{
	return g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_code\":\"%s\",\"idempotency_key\":\"%s\"}",
		organization_id, plan, key);
}

static gchar *
subscribe_text(Fixture *f, gint64 organization_id, const gchar *plan, const gchar *key, guint expected)
{
	g_autofree gchar *payload = sub_body(organization_id, plan, key);
	return call_text(f, "POST", SUBSCRIPTIONS, f->admin_secret, NULL, payload, expected);
}

static JsonNode *
subscribe(Fixture *f, gint64 organization_id, const gchar *plan, const gchar *key, guint expected)
{
	g_autofree gchar *text = subscribe_text(f, organization_id, plan, key, expected);
	return parse(text);
}

static gchar *
view_path(gint64 organization_id)
{
	return g_strdup_printf(OVERVIEW "/%" G_GINT64_FORMAT, organization_id);
}

static gchar *
view_text(Fixture *f, gint64 organization_id)
{
	g_autofree gchar *path = view_path(organization_id);
	return call_text(f, "GET", path, f->admin_secret, NULL, NULL, 200);
}

static void
sql(VentureDatabase *db, const gchar *statement)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_execute(db, statement, NULL, &error);
	g_assert_no_error(error); g_assert_true(ok);
}

static GType
type_named(const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	return type;
}

/* Rows of @name, deleted included, in @organization (0: every one). */
static guint
count_in(Fixture *f, const gchar *name, gint64 organization)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(type_named(name));
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->db), NULL);
	venture_query_set_include_deleted(query, TRUE);
	if (organization > 0) venture_query_set_organization(query, organization);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error); g_assert_nonnull(rows);
	return rows->len;
}

typedef struct {
	guint companies;
	guint subscriptions;
	guint events;
	guint invoices;
	guint receipts;
} Counts;

static Counts
counts(Fixture *f)
{
	Counts c;
	c.companies = count_in(f, "company", 0);
	c.subscriptions = count_in(f, "customer_subscription", 0);
	c.events = count_in(f, "subscription_event", 0);
	c.invoices = count_in(f, "invoice", 0);
	c.receipts = count_in(f, "lightsite_billing_receipt", 0);
	return c;
}

static void
assert_counts(Counts a, Counts b)
{
	g_assert_cmpuint(a.companies, ==, b.companies);
	g_assert_cmpuint(a.subscriptions, ==, b.subscriptions);
	g_assert_cmpuint(a.events, ==, b.events);
	g_assert_cmpuint(a.invoices, ==, b.invoices);
	g_assert_cmpuint(a.receipts, ==, b.receipts);
}

static gchar *
mint(VentureDatabase *db, gint64 user_id, gint role)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;
	g_object_set(token, "name", "Lightsite service", "user-id", user_id, "role", role, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(token), NULL, &error)); g_assert_no_error(error);
	return secret;
}

/* What an accepted invitation produces, made under maintenance. */
static VentureEntity *
hosted_member(Fixture *f, const gchar *username, const gchar *email, gint64 organization, gint role)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(f->service, "Provision a member", &error);
	g_autoptr(VentureEntity) tenant_member = NULL;
	g_autoptr(VentureEntity) org_member = NULL;
	VentureEntity *user = g_object_new(VENTURE_TYPE_USER, "username", username, "email", email, "organization-id", organization,
		"role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_no_error(error); g_assert_nonnull(maintenance);
	g_assert_true(venture_user_set_password(VENTURE_USER(user), "member-test-password", 2000, &error));
	{ gboolean saved = venture_database_save(f->db, user, NULL, &error); g_assert_no_error(error); g_assert_true(saved); }
	tenant_member = g_object_new(VENTURE_TYPE_TENANT_MEMBERSHIP, "name", username,
		"user-id", venture_entity_get_id(user), "role", VENTURE_TENANT_ROLE_MEMBER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, tenant_member, NULL, &error)); g_assert_no_error(error);
	org_member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", organization,
		"user-id", venture_entity_get_id(user), "role", role, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, org_member, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	return user;
}

static guint
login_status(Fixture *f, const gchar *username, const gchar *password, gchar **cookie)
{
	g_autofree gchar *form = g_strdup_printf("username=%s&password=%s", username, password);
	return exchange(f, "POST", "/login", NULL, NULL, NULL, "application/x-www-form-urlencoded", form, cookie, NULL, NULL);
}

/* A business made the way Lightsite makes one: through the sign-up route. */
static gint64
business(Fixture *f, const gchar *slug, const gchar *name)
{
	g_autofree gchar *payload = g_strdup_printf("{\"idempotency_key\":\"signup:%s\",\"email\":\"%s@example.test\","
		"\"issuer\":\"" ISSUER "\",\"subject\":\"kc-%s\",\"business_name\":\"%s\"}", slug, slug, slug, name);
	g_autoptr(JsonNode) result = call(f, "POST", SIGNUPS, f->admin_secret, NULL, payload, 201);
	return json_object_get_int_member(json_node_get_object(result), "organization_id");
}

static void
save(Fixture *f, VentureEntity *entity)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, entity, NULL, &error);
	g_assert_no_error(error); g_assert_true(ok);
}

static void
field(VentureEntity *entity, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(entity, name, value, &error));
	g_assert_no_error(error);
}

/* The settlement accounts any organization that invoices already has
 * (docs/receivables.org); the seeded chart covers only the first one. */
static void
create_accounts(Fixture *f)
{
	static const struct { const gchar *code; const gchar *name; VentureAccountKind kind; } accounts[] = {
		{ "1000", "Cash", VENTURE_ACCOUNT_KIND_ASSET },
		{ "1100", "Accounts receivable", VENTURE_ACCOUNT_KIND_ASSET },
		{ "2100", "Sales tax payable", VENTURE_ACCOUNT_KIND_LIABILITY },
		{ "2200", "Deferred revenue", VENTURE_ACCOUNT_KIND_LIABILITY },
		{ "4000", "Sales", VENTURE_ACCOUNT_KIND_INCOME },
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(accounts); i++) {
		g_autoptr(VentureEntity) account = g_object_new(VENTURE_TYPE_ACCOUNT, "code", accounts[i].code,
			"name", accounts[i].name, "kind", accounts[i].kind, "active", TRUE, NULL);
		venture_entity_set_organization_id(account, f->home);
		save(f, account);
	}
}

/* The disposable example in docs/lightsite-billing.org, as records. */
static void
create_plans(Fixture *f)
{
	guint i;
	create_accounts(f);
	for (i = 0; i < G_N_ELEMENTS(PLANS); i++) {
		g_autoptr(VentureEntity) plan = g_object_new(type_named("plan"), NULL);
		g_autoptr(VentureEntity) price = g_object_new(type_named("plan_price"), NULL);
		venture_entity_set_organization_id(plan, f->home);
		g_object_set(plan, "name", PLANS[i].name, "code", PLANS[i].code, "active", TRUE, NULL);
		save(f, plan);
		venture_entity_set_organization_id(price, f->home);
		g_object_set(price, "plan-id", venture_entity_get_id(plan), "currency", "USD", "active", TRUE, NULL);
		field(price, "interval", "month");
		field(price, "amount", PLANS[i].amount);
		save(f, price);
		f->prices[i] = venture_entity_get_id(price);
	}
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GInetAddress) loopback_address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *loopback = g_inet_address_to_string(loopback_address);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(VentureEntity) home = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "the operator", "active", TRUE,
		"is-default", TRUE, NULL);
	gboolean ok;
	(void)data;
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-lightsite-billing-XXXXXX", &error); g_assert_no_error(error);
	f->session = soup_session_new_with_options("timeout", 10, NULL);
	g_object_set(f->config, "hosted-enabled", TRUE, "hosted-workspace-id", WORKSPACE, "hosted-origin", ORIGIN,
		"security-password-iterations", (gint64)2000, "state-dir", f->directory,
		"server-bind-address", loopback, "server-port", (gint64)0, NULL);
	ok = venture_database_migrate(f->db, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error); g_assert_true(ok);
	f->service = venture_tenant_service_get(f->db);
	g_assert_true(venture_tenant_service_configure(f->service, f->config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(f->service, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(f->db, home, NULL, &error)); g_assert_no_error(error);
	f->home = venture_entity_get_id(home);
	/* the operator's own organization is the billing organization. */
	g_object_set(f->config, "lightsite-billing-organization-id", f->home, NULL);
	create_plans(f);
	g_assert_true(venture_tenant_service_bootstrap_admin(f->service, f->config, "provisioner", "private-test-password",
		FALSE, "Billing fixture", &error)); g_assert_no_error(error);
	users = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	g_assert_cmpuint(users->len, ==, 1);
	f->admin_id = venture_entity_get_id(g_ptr_array_index(users, 0));
	f->admin_secret = mint(f->db, f->admin_id, VENTURE_USER_ROLE_EDITOR);
	f->context = venture_context_new(f->config, f->db);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->session);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->directory);
	g_free(f->directory);
	g_free(f->admin_secret);
}

static VentureEntity *
customer_company(Fixture *f, gint64 organization_id)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(type_named("company"));
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->db), NULL);
	g_autofree gchar *reference = g_strdup_printf("lightsite:organization:%" G_GINT64_FORMAT, organization_id);
	VentureEntity *company;
	venture_query_set_organization(query, f->home);
	venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ, reference, NULL);
	company = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	return company;
}

static VentureEntity *
only_subscription(Fixture *f, gint64 company_id)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(type_named("customer_subscription"));
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->db), NULL);
	venture_query_set_organization(query, f->home);
	venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ, company_id, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	return g_object_ref(g_ptr_array_index(rows, 0));
}

static gint64
integer(VentureEntity *entity, const gchar *name)
{
	gint64 value = 0;
	g_object_get(entity, name, &value, NULL);
	return value;
}

static void
assert_plan(JsonObject *view, const gchar *code, const gchar *name, const gchar *amount)
{
	JsonObject *plan = json_object_get_object_member(view, "plan");
	g_assert_nonnull(plan);
	g_assert_cmpuint(json_object_get_size(plan), ==, 5);
	g_assert_cmpstr(json_object_get_string_member(plan, "code"), ==, code);
	g_assert_cmpstr(json_object_get_string_member(plan, "name"), ==, name);
	g_assert_cmpstr(json_object_get_string_member(plan, "amount"), ==, amount);
	g_assert_cmpstr(json_object_get_string_member(plan, "currency"), ==, "USD");
	g_assert_cmpstr(json_object_get_string_member(plan, "interval"), ==, "month");
}

static void
assert_owed(JsonObject *view, const gchar *amount)
{
	JsonObject *owed = json_object_get_object_member(view, "owed");
	g_assert_nonnull(owed);
	g_assert_cmpuint(json_object_get_size(owed), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(owed, "amount"), ==, amount);
	g_assert_cmpstr(json_object_get_string_member(owed, "currency"), ==, "USD");
}

/* A new business on Plan A: its customer company in the billing organization,
 * one active subscription and its first month's invoice, issued now. */
static void
test_subscribe_new(Fixture *f, gconstpointer data)
{
	gint64 bea = business(f, "bea", "Example Co");
	g_autofree gchar *answer = NULL, *read = NULL, *reference = NULL, *name = NULL, *source = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(VentureEntity) company = NULL, subscription = NULL;
	JsonObject *view, *invoice;
	Counts before = counts(f), after;
	(void)data;
	answer = subscribe_text(f, bea, "team", "bill:bea:1", 201);
	result = parse(answer);
	view = json_node_get_object(result);
	g_assert_cmpuint(json_object_get_size(view), ==, 9);
	g_assert_cmpint(json_object_get_int_member(view, "organization_id"), ==, bea);
	assert_plan(view, "team", "Plan A", "10.00");
	g_assert_cmpstr(json_object_get_string_member(view, "state"), ==, "active");
	g_assert_nonnull(json_object_get_string_member(view, "current_period_end"));
	assert_owed(view, "10.00");
	g_assert_false(json_object_get_boolean_member(view, "overdue"));
	g_assert_false(json_object_get_boolean_member(view, "failed_payment"));
	g_assert_false(json_object_get_boolean_member(view, "stripe_connected"));
	invoice = json_object_get_object_member(view, "last_invoice");
	g_assert_nonnull(invoice);
	g_assert_cmpuint(json_object_get_size(invoice), ==, 5);
	g_assert_true(json_object_get_string_member(invoice, "number")[0] != '\0');
	g_assert_cmpstr(json_object_get_string_member(invoice, "status"), ==, "sent");
	g_assert_cmpstr(json_object_get_string_member(invoice, "total"), ==, "10.00");
	g_assert_nonnull(json_object_get_string_member(invoice, "due_date"));
	g_assert_true(json_object_get_null_member(invoice, "paid_at"));

	after = counts(f);
	g_assert_cmpuint(after.companies, ==, before.companies + 1);
	g_assert_cmpuint(after.subscriptions, ==, before.subscriptions + 1);
	g_assert_cmpuint(after.invoices, ==, before.invoices + 1);
	g_assert_cmpuint(after.receipts, ==, before.receipts + 1);
	/* The customer is the operator's, named after the business and pointing at it;
	 * the business's own organization gets nothing. */
	company = customer_company(f, bea);
	g_assert_nonnull(company);
	g_assert_cmpint(venture_entity_get_organization_id(company), ==, f->home);
	g_object_get(company, "name", &name, "external-id", &reference, "source", &source, NULL);
	g_assert_cmpstr(name, ==, "Example Co");
	g_assert_cmpstr(source, ==, "lightsite");
	g_assert_cmpuint(count_in(f, "company", bea), ==, 0);
	g_assert_cmpuint(count_in(f, "customer_subscription", bea), ==, 0);
	subscription = only_subscription(f, venture_entity_get_id(company));
	g_assert_cmpint(integer(subscription, "plan-price-id"), ==, f->prices[0]);
	g_assert_cmpint(integer(subscription, "seats"), ==, 1);

	/* The read answers exactly what the write answered. */
	read = view_text(f, bea);
	g_assert_cmpstr(read, ==, answer);
}

/* A retry is answered from the receipt; a reused key with another body is
 * a conflict. Neither writes a row. */
static void
test_replay(Fixture *f, gconstpointer data)
{
	gint64 bea = business(f, "bea", "Example Co");
	gint64 cal = business(f, "cal", "Second Example Co");
	g_autofree gchar *first = NULL, *second = NULL, *third = NULL;
	g_autoptr(JsonNode) conflict = NULL;
	Counts made;
	(void)data;
	first = subscribe_text(f, bea, "team", "bill:replay", 201);
	made = counts(f);
	second = subscribe_text(f, bea, "team", "bill:replay", 200);
	g_assert_cmpstr(first, ==, second);
	assert_counts(counts(f), made);
	conflict = subscribe(f, bea, "growth", "bill:replay", 409); g_clear_pointer(&conflict, json_node_unref);
	conflict = subscribe(f, cal, "team", "bill:replay", 409); g_clear_pointer(&conflict, json_node_unref);
	assert_counts(counts(f), made);
	/* Still answered from the receipt after the conflicts. */
	third = subscribe_text(f, bea, "team", "bill:replay", 200);
	g_assert_cmpstr(first, ==, third);
	assert_counts(counts(f), made);
}

/* A business that already has a subscription moves at renewal: today's
 * price stands, nothing is invoiced now, and the switch is pending. */
static void
test_change_plan(Fixture *f, gconstpointer data)
{
	gint64 bea = business(f, "bea", "Example Co");
	g_autoptr(JsonNode) first = NULL, change = NULL, again = NULL, back = NULL;
	g_autoptr(VentureEntity) company = NULL, subscription = NULL;
	Counts started, changed;
	(void)data;
	first = subscribe(f, bea, "team", "bill:bea:team", 201);
	started = counts(f);
	change = subscribe(f, bea, "growth", "bill:bea:growth", 201);
	changed = counts(f);
	g_assert_cmpuint(changed.companies, ==, started.companies);
	g_assert_cmpuint(changed.subscriptions, ==, started.subscriptions);
	g_assert_cmpuint(changed.invoices, ==, started.invoices);
	g_assert_cmpuint(changed.events, ==, started.events + 1);
	g_assert_cmpuint(changed.receipts, ==, started.receipts + 1);
	assert_plan(json_node_get_object(change), "team", "Plan A", "10.00");
	assert_owed(json_node_get_object(change), "10.00");
	company = customer_company(f, bea);
	subscription = only_subscription(f, venture_entity_get_id(company));
	g_assert_cmpint(integer(subscription, "plan-price-id"), ==, f->prices[0]);
	g_assert_cmpint(integer(subscription, "pending-plan-price-id"), ==, f->prices[1]);
	/* Asking again for the plan already scheduled changes nothing. */
	again = subscribe(f, bea, "growth", "bill:bea:growth-again", 201);
	g_assert_cmpuint(counts(f).events, ==, changed.events);
	/* And the down-sell is a change like any other. */
	back = subscribe(f, bea, "starter", "bill:bea:starter", 201);
	g_clear_object(&subscription);
	subscription = only_subscription(f, venture_entity_get_id(company));
	g_assert_cmpint(integer(subscription, "pending-plan-price-id"), ==, f->prices[2]);
	g_assert_cmpuint(counts(f).invoices, ==, started.invoices);
}

/* An overdue invoice and a failed payment are both visible, and neither
 * suspends anything: the subscription is still there and still renews. */
static void
test_overdue_and_failed(Fixture *f, gconstpointer data)
{
	gint64 bea = business(f, "bea", "Example Co");
	g_autoptr(JsonNode) result = NULL, current = NULL;
	g_autoptr(VentureEntity) company = NULL, subscription = NULL;
	g_autoptr(VentureBillingRequest) failed = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc(), past = g_date_time_add_days(now, -10);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *stamp = g_date_time_format_iso8601(past), *update = NULL, *text = NULL;
	JsonObject *view;
	(void)data;
	result = subscribe(f, bea, "team", "bill:bea:1", 201);
	company = customer_company(f, bea);
	subscription = only_subscription(f, venture_entity_get_id(company));
	/* Due on receipt is not yet overdue. */
	text = view_text(f, bea);
	current = parse(text);
	g_assert_false(json_object_get_boolean_member(json_node_get_object(current), "overdue"));
	g_clear_pointer(&current, json_node_unref); g_clear_pointer(&text, g_free);

	/* Ten days past due and unpaid. */
	update = g_strdup_printf("UPDATE invoices SET due_at='%s' WHERE company_id=%" G_GINT64_FORMAT, stamp,
		venture_entity_get_id(company));
	sql(f->db, update);
	text = view_text(f, bea);
	current = parse(text);
	view = json_node_get_object(current);
	g_assert_true(json_object_get_boolean_member(view, "overdue"));
	g_assert_false(json_object_get_boolean_member(view, "failed_payment"));
	g_assert_cmpstr(json_object_get_string_member(view, "state"), ==, "active");
	assert_owed(view, "10.00");
	g_clear_pointer(&current, json_node_unref); g_clear_pointer(&text, g_free);

	/* The processor (or an operator) reports a failed attempt. */
	failed = venture_billing_request_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(failed), f->home);
	g_clear_pointer(&now, g_date_time_unref);
	now = g_date_time_new_now_utc();
	g_object_set(failed, "action", "mark-payment-failed", "subscription-id", venture_entity_get_id(subscription),
		"at", now, NULL);
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->db), NULL);
		gboolean ok = venture_billing_service_execute(venture_billing_service_get(f->db), failed, NULL, &error);
		g_assert_no_error(error); g_assert_true(ok);
	}
	text = view_text(f, bea);
	current = parse(text);
	view = json_node_get_object(current);
	g_assert_true(json_object_get_boolean_member(view, "failed_payment"));
	g_assert_true(json_object_get_boolean_member(view, "overdue"));
	g_assert_cmpstr(json_object_get_string_member(view, "state"), ==, "past_due");
	assert_plan(view, "team", "Plan A", "10.00");
}

static JsonNode *
stripe_settings(void)
{
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	json_node_take_object(node, object);
	json_object_set_string_member(object, "secret_key", "fixture-secret");
	json_object_set_string_member(object, "publishable_key", "fixture-public");
	json_object_set_string_member(object, "webhook_secret", "fixture-signing");
	json_object_set_string_member(object, "api_version", "2024-06-20");
	json_object_set_string_member(object, "success_url", "https://example.test/paid");
	json_object_set_string_member(object, "cancel_url", "https://example.test/cancel");
	json_object_set_string_member(object, "environment", "test");
	return node;
}

/* stripe_connected is about the business's own Stripe, and it is only ever
 * a yes or a no: no key, no account id. */
static void
test_stripe_connected(Fixture *f, gconstpointer data)
{
	gint64 bea = business(f, "bea", "Example Co");
	g_autoptr(JsonNode) result = NULL, current = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(JsonNode) settings = stripe_settings();
	g_autoptr(VentureIntegrationConnection) connection = NULL, operator = NULL;
	g_autofree gchar *text = NULL;
	(void)data;
	result = subscribe(f, bea, "team", "bill:bea:1", 201);
	g_assert_false(json_object_get_boolean_member(json_node_get_object(result), "stripe_connected"));
	/* the operator's own Stripe, on the billing organization, is not the
	 * business's: it must not make the business read as connected. */
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(f->service, "Connect Stripe", &error);
		g_assert_no_error(error);
		operator = venture_integration_service_configure(venture_integration_service_get(f->db), f->home, "stripe",
			"acct_operator_fixture", "test", settings, 0, NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(operator);
		g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	}
	text = view_text(f, bea);
	current = parse(text);
	g_assert_false(json_object_get_boolean_member(json_node_get_object(current), "stripe_connected"));
	g_clear_pointer(&current, json_node_unref); g_clear_pointer(&text, g_free);
	/* The business connects its own, through the same integration binding
	 * its /organizations/<id>/settings/stripe page writes. */
	{
		g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(f->service, "Connect Stripe", &error);
		g_assert_no_error(error);
		connection = venture_integration_service_configure(venture_integration_service_get(f->db), bea, "stripe",
			"acct_business_fixture", "test", settings, 0, NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(connection);
		g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	}
	text = view_text(f, bea);
	current = parse(text);
	g_assert_true(json_object_get_boolean_member(json_node_get_object(current), "stripe_connected"));
	g_assert_null(strstr(text, "acct_"));
	g_assert_null(strstr(text, "sk_test"));
	g_assert_null(strstr(text, "whsec"));
	g_assert_null(strstr(text, "pk_test"));
}

/* Staff see every Lightsite customer with a subscription, and nobody else. */
static void
test_staff_list(Fixture *f, gconstpointer data)
{
	gint64 bea = business(f, "bea", "Example Co");
	gint64 cal = business(f, "cal", "Second Example Co");
	gint64 dot = business(f, "dot", "Dot's Diner");
	g_autoptr(JsonNode) a = NULL, b = NULL, list = NULL, none = NULL;
	g_autoptr(VentureEntity) direct = NULL;
	g_autoptr(VentureBillingRequest) start = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL, *bea_view = NULL, *cal_view = NULL, *dot_view = NULL;
	JsonArray *customers;
	(void)data;
	a = subscribe(f, cal, "growth", "bill:cal:1", 201);
	b = subscribe(f, bea, "starter", "bill:bea:1", 201);
	/* A customer of the operator's that is not a Lightsite business. */
	direct = g_object_new(type_named("company"), NULL);
	venture_entity_set_organization_id(direct, f->home);
	g_object_set(direct, "name", "Consulting client", "active", TRUE, NULL);
	save(f, direct);
	start = venture_billing_request_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(start), f->home);
	g_object_set(start, "action", "start", "company-id", venture_entity_get_id(direct), "plan-price-id", f->prices[0],
		"seats", (gint64)1, "at", now, NULL);
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->db), NULL);
		gboolean ok = venture_billing_service_execute(venture_billing_service_get(f->db), start, NULL, &error);
		g_assert_no_error(error); g_assert_true(ok);
	}

	text = call_text(f, "GET", OVERVIEW, f->admin_secret, NULL, NULL, 200);
	list = parse(text);
	g_assert_cmpuint(json_object_get_size(json_node_get_object(list)), ==, 1);
	customers = json_object_get_array_member(json_node_get_object(list), "customers");
	g_assert_nonnull(customers);
	g_assert_cmpuint(json_array_get_length(customers), ==, 2);
	/* Ordered by business, each entry exactly the business's own view. */
	bea_view = view_text(f, bea);
	cal_view = view_text(f, cal);
	{
		g_autoptr(JsonNode) first = json_node_copy(json_array_get_element(customers, 0));
		g_autoptr(JsonNode) second = json_node_copy(json_array_get_element(customers, 1));
		g_autofree gchar *first_text = venture_json_to_string(first, FALSE);
		g_autofree gchar *second_text = venture_json_to_string(second, FALSE);
		g_autoptr(JsonNode) bea_node = parse(bea_view), cal_node = parse(cal_view);
		g_autofree gchar *bea_text = venture_json_to_string(bea_node, FALSE);
		g_autofree gchar *cal_text = venture_json_to_string(cal_node, FALSE);
		g_assert_cmpstr(first_text, ==, bea_text);
		g_assert_cmpstr(second_text, ==, cal_text);
	}
	assert_owed(json_array_get_object_element(customers, 0), "30.00");
	assert_owed(json_array_get_object_element(customers, 1), "20.00");

	/* A business with no subscription is answered, not refused: nothing
	 * yet, nothing owed. */
	dot_view = view_text(f, dot);
	none = parse(dot_view);
	g_assert_cmpuint(json_object_get_size(json_node_get_object(none)), ==, 9);
	g_assert_true(json_object_get_null_member(json_node_get_object(none), "plan"));
	g_assert_true(json_object_get_null_member(json_node_get_object(none), "last_invoice"));
	g_assert_true(json_object_get_null_member(json_node_get_object(none), "current_period_end"));
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(none), "state"), ==, "none");
	assert_owed(json_node_get_object(none), "0.00");
}

/* Only a workspace administrator's write-capable bearer token, judged now
 * and as captured when minted, behind the hosted Host boundary -- for all
 * three routes, exactly as for the sign-up. */
static void
test_authority(Fixture *f, gconstpointer data)
{
	gint64 bea = business(f, "bea", "Example Co");
	g_autoptr(VentureEntity) mo = hosted_member(f, "mo", "mo@example.test", f->home, VENTURE_ORGANIZATION_ROLE_OWNER);
	g_autofree gchar *member_secret = mint(f->db, venture_entity_get_id(mo), VENTURE_USER_ROLE_EDITOR);
	g_autofree gchar *viewer_secret = mint(f->db, f->admin_id, VENTURE_USER_ROLE_VIEWER);
	g_autofree gchar *unsnapshotted = mint(f->db, f->admin_id, VENTURE_USER_ROLE_EDITOR);
	g_autofree gchar *payload = sub_body(bea, "team", "bill:authority");
	g_autofree gchar *path = view_path(bea);
	g_autofree gchar *cookie = NULL;
	struct { const gchar *method; const gchar *path; const gchar *payload; } routes[3];
	Counts before;
	guint i;
	(void)data;
	routes[0].method = "POST"; routes[0].path = SUBSCRIPTIONS; routes[0].payload = payload;
	routes[1].method = "GET"; routes[1].path = path; routes[1].payload = NULL;
	routes[2].method = "GET"; routes[2].path = OVERVIEW; routes[2].payload = NULL;
	g_assert_cmpuint(login_status(f, "provisioner", "private-test-password", &cookie), ==, 302);
	g_assert_nonnull(cookie);
	sql(f->db, "UPDATE api_tokens SET membership_snapshot='{}' WHERE id=(SELECT MAX(id) FROM api_tokens)");
	before = counts(f);
	for (i = 0; i < G_N_ELEMENTS(routes); i++) {
		g_autoptr(JsonNode) result = NULL;
		g_autofree gchar *body = NULL;
		result = call(f, routes[i].method, routes[i].path, NULL, NULL, routes[i].payload, 401); g_clear_pointer(&result, json_node_unref);
		result = call(f, routes[i].method, routes[i].path, "invalid", NULL, routes[i].payload, 401); g_clear_pointer(&result, json_node_unref);
		/* A browser session is not a service credential. */
		g_assert_cmpuint(exchange(f, routes[i].method, routes[i].path, NULL, cookie, NULL,
			routes[i].payload ? "application/json" : NULL, routes[i].payload, NULL, &body, NULL), ==, 401);
		result = call(f, routes[i].method, routes[i].path, member_secret, NULL, routes[i].payload, 403); g_clear_pointer(&result, json_node_unref);
		result = call(f, routes[i].method, routes[i].path, viewer_secret, NULL, routes[i].payload, 403); g_clear_pointer(&result, json_node_unref);
		result = call(f, routes[i].method, routes[i].path, unsnapshotted, NULL, routes[i].payload, 403); g_clear_pointer(&result, json_node_unref);
		result = call(f, routes[i].method, routes[i].path, f->admin_secret, "wrong.example", routes[i].payload, 403); g_clear_pointer(&result, json_node_unref);
		sql(f->db, "UPDATE tenant_workspaces SET state='read_only'");
		result = call(f, routes[i].method, routes[i].path, f->admin_secret, NULL, routes[i].payload, 403); g_clear_pointer(&result, json_node_unref);
		sql(f->db, "UPDATE tenant_workspaces SET state='active'");
		sql(f->db, "UPDATE tenant_memberships SET role='member' WHERE name='provisioner'");
		result = call(f, routes[i].method, routes[i].path, f->admin_secret, NULL, routes[i].payload, 403); g_clear_pointer(&result, json_node_unref);
		sql(f->db, "UPDATE tenant_memberships SET role='admin' WHERE name='provisioner'");
	}
	assert_counts(counts(f), before);
	{
		g_autoptr(JsonNode) result = subscribe(f, bea, "team", "bill:authority", 201);
	}
}

/* No billing organization configured: the routes are off, and say 404 to
 * an administrator -- never bill from whichever organization is first. */
static void
test_unconfigured(Fixture *f, gconstpointer data)
{
	gint64 bea = business(f, "bea", "Example Co");
	g_autofree gchar *payload = sub_body(bea, "team", "bill:off");
	g_autofree gchar *path = view_path(bea);
	g_autoptr(JsonNode) result = NULL;
	gint64 settings[] = { 0, 987654 };
	Counts before = counts(f);
	guint i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(settings); i++) {
		g_object_set(f->config, "lightsite-billing-organization-id", settings[i], NULL);
		result = call(f, "POST", SUBSCRIPTIONS, f->admin_secret, NULL, payload, 404); g_clear_pointer(&result, json_node_unref);
		result = call(f, "GET", path, f->admin_secret, NULL, NULL, 404); g_clear_pointer(&result, json_node_unref);
		result = call(f, "GET", OVERVIEW, f->admin_secret, NULL, NULL, 404); g_clear_pointer(&result, json_node_unref);
		/* Off is not a way around authentication. */
		result = call(f, "POST", SUBSCRIPTIONS, NULL, NULL, payload, 401); g_clear_pointer(&result, json_node_unref);
		result = call(f, "GET", OVERVIEW, NULL, NULL, NULL, 401); g_clear_pointer(&result, json_node_unref);
	}
	assert_counts(counts(f), before);
	g_object_set(f->config, "lightsite-billing-organization-id", f->home, NULL);
	result = subscribe(f, bea, "team", "bill:off", 201);
}

/* The body is a closed, bounded object; the business must be one that
 * exists and is not the billing organization; the plan must be offered.
 * Every refusal writes nothing. */
static void
test_body(Fixture *f, gconstpointer data)
{
	static const gchar *const refused[] = {
		"not json",
		"[]",
		"{}",
		"\"text\"",
		"{\"organization_id\":%s,\"plan_code\":\"team\"}",
		"{\"organization_id\":%s,\"idempotency_key\":\"k1\"}",
		"{\"plan_code\":\"team\",\"idempotency_key\":\"k1\"}",
		"{\"organization_id\":%s,\"plan_code\":\"team\",\"idempotency_key\":\"k1\",\"seats\":2}",
		"{\"organization_id\":\"%s\",\"plan_code\":\"team\",\"idempotency_key\":\"k1\"}",
		"{\"organization_id\":%s.5,\"plan_code\":\"team\",\"idempotency_key\":\"k1\"}",
		"{\"organization_id\":true,\"plan_code\":\"team\",\"idempotency_key\":\"k1\"}",
		"{\"organization_id\":null,\"plan_code\":\"team\",\"idempotency_key\":\"k1\"}",
		"{\"organization_id\":0,\"plan_code\":\"team\",\"idempotency_key\":\"k1\"}",
		"{\"organization_id\":-%s,\"plan_code\":\"team\",\"idempotency_key\":\"k1\"}",
		"{\"organization_id\":99999999999999999999,\"plan_code\":\"team\",\"idempotency_key\":\"k1\"}",
		"{\"organization_id\":%s,\"plan_code\":\"enterprise\",\"idempotency_key\":\"k1\"}",
		"{\"organization_id\":%s,\"plan_code\":\"Plan A\",\"idempotency_key\":\"k1\"}",
		"{\"organization_id\":%s,\"plan_code\":1,\"idempotency_key\":\"k1\"}",
		"{\"organization_id\":%s,\"plan_code\":\"team\",\"idempotency_key\":\"\"}",
		"{\"organization_id\":%s,\"plan_code\":\"team\",\"idempotency_key\":\"has space\"}",
		"{\"organization_id\":%s,\"plan_code\":\"team\",\"idempotency_key\":\"k/1\"}",
		"{\"organization_id\":%s,\"plan_code\":\"team\",\"idempotency_key\":7}",
		"{\"organization_id\":%s,\"plan_code\":\"team\\u0000\",\"idempotency_key\":\"k1\"}",
	};
	gint64 bea = business(f, "bea", "Example Co");
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, bea);
	g_autoptr(GString) long_key = g_string_new(NULL), huge = g_string_new(NULL);
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *payload = NULL;
	Counts before = counts(f);
	guint i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(refused); i++) {
		g_autoptr(GString) text = g_string_new(refused[i]);
		g_string_replace(text, "%s", id, 0);
		result = call(f, "POST", SUBSCRIPTIONS, f->admin_secret, NULL, text->str, 422);
		g_clear_pointer(&result, json_node_unref);
	}
	for (i = 0; i < 129; i++) g_string_append_c(long_key, 'k');
	payload = sub_body(bea, "team", long_key->str);
	result = call(f, "POST", SUBSCRIPTIONS, f->admin_secret, NULL, payload, 422); g_clear_pointer(&result, json_node_unref);
	g_clear_pointer(&payload, g_free);
	for (i = 0; i < 4200; i++) g_string_append_c(huge, 'x');
	payload = sub_body(bea, "team", huge->str);
	result = call(f, "POST", SUBSCRIPTIONS, f->admin_secret, NULL, payload, 413); g_clear_pointer(&result, json_node_unref);
	g_clear_pointer(&payload, g_free);
	/* An organization nobody made, and the billing organization itself. */
	payload = sub_body(987654, "team", "k-unknown");
	result = call(f, "POST", SUBSCRIPTIONS, f->admin_secret, NULL, payload, 404); g_clear_pointer(&result, json_node_unref);
	g_clear_pointer(&payload, g_free);
	payload = sub_body(f->home, "team", "k-self");
	result = call(f, "POST", SUBSCRIPTIONS, f->admin_secret, NULL, payload, 422); g_clear_pointer(&result, json_node_unref);
	g_clear_pointer(&payload, g_free);
	/* A plan the billing organization does not (or no longer) offers. */
	{
		g_autoptr(VentureEntity) price = NULL;
		g_autoptr(GError) error = NULL;
		price = venture_database_get(f->db, type_named("plan_price"), f->prices[2], &error);
		g_assert_no_error(error);
		g_object_set(price, "active", FALSE, NULL);
		save(f, price);
	}
	payload = sub_body(bea, "starter", "k-retired");
	result = call(f, "POST", SUBSCRIPTIONS, f->admin_secret, NULL, payload, 404); g_clear_pointer(&result, json_node_unref);
	g_clear_pointer(&payload, g_free);
	/* The reads take a positive organization ID, and one that exists. */
	result = call(f, "GET", OVERVIEW "/abc", f->admin_secret, NULL, NULL, 422); g_clear_pointer(&result, json_node_unref);
	result = call(f, "GET", OVERVIEW "/0", f->admin_secret, NULL, NULL, 422); g_clear_pointer(&result, json_node_unref);
	result = call(f, "GET", OVERVIEW "/987654", f->admin_secret, NULL, NULL, 404); g_clear_pointer(&result, json_node_unref);
	assert_counts(counts(f), before);
	/* The bounds themselves are accepted. */
	g_string_truncate(long_key, 120); g_string_append(long_key, "Az09._:-");
	payload = sub_body(bea, "team", long_key->str);
	result = call(f, "POST", SUBSCRIPTIONS, f->admin_secret, NULL, payload, 201);
}

/* A failure after the company, the subscription and the first invoice were
 * written leaves none of them, and the same request then succeeds: undone,
 * and resumable. */
static void
test_rollback(Fixture *f, gconstpointer data)
{
	gint64 bea = business(f, "bea", "Example Co");
	g_autoptr(JsonNode) result = NULL;
	Counts before = counts(f), after;
	(void)data;
	sql(f->db, "ALTER TABLE lightsite_billing_receipts RENAME TO receipts_unavailable");
	result = subscribe(f, bea, "team", "bill:rollback", 500); g_clear_pointer(&result, json_node_unref);
	sql(f->db, "ALTER TABLE receipts_unavailable RENAME TO lightsite_billing_receipts");
	assert_counts(counts(f), before);
	g_assert_null(customer_company(f, bea));
	result = subscribe(f, bea, "team", "bill:rollback", 201);
	after = counts(f);
	g_assert_cmpuint(after.companies, ==, before.companies + 1);
	g_assert_cmpuint(after.subscriptions, ==, before.subscriptions + 1);
	g_assert_cmpuint(after.invoices, ==, before.invoices + 1);
	g_assert_cmpuint(after.receipts, ==, before.receipts + 1);
}

/* A receipt is what makes a retry safe, so no generic writer may forge,
 * alter or remove one: a forged receipt would answer a request that never
 * ran, and a deleted one would let a retry bill twice. */
static void
test_receipts_guarded(Fixture *f, gconstpointer data)
{
	gint64 bea = business(f, "bea", "Example Co");
	g_autoptr(JsonNode) result = subscribe(f, bea, "team", "bill:guard", 201);
	g_autoptr(VentureEntity) forged = g_object_new(type_named("lightsite_billing_receipt"), NULL);
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(type_named("lightsite_billing_receipt"));
	g_autoptr(GError) error = NULL;
	(void)data;
	venture_entity_set_organization_id(forged, f->home);
	g_object_set(forged, "idempotency-key", "bill:forged", "request-hash", "0", "result", "{}", NULL);
	g_assert_false(venture_database_save(f->db, forged, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	venture_query_set_organization(query, f->home);
	stored = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error); g_assert_nonnull(stored);
	g_object_set(stored, "result", "{\"forged\":true}", NULL);
	g_assert_false(venture_database_save(f->db, stored, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_false(venture_database_delete(f->db, stored, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_cmpuint(count_in(f, "lightsite_billing_receipt", 0), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/lightsite-billing/subscribe-new", Fixture, NULL, setup, test_subscribe_new, teardown);
	g_test_add("/lightsite-billing/replay", Fixture, NULL, setup, test_replay, teardown);
	g_test_add("/lightsite-billing/change-plan", Fixture, NULL, setup, test_change_plan, teardown);
	g_test_add("/lightsite-billing/overdue-and-failed", Fixture, NULL, setup, test_overdue_and_failed, teardown);
	g_test_add("/lightsite-billing/stripe-connected", Fixture, NULL, setup, test_stripe_connected, teardown);
	g_test_add("/lightsite-billing/staff-list", Fixture, NULL, setup, test_staff_list, teardown);
	g_test_add("/lightsite-billing/authority", Fixture, NULL, setup, test_authority, teardown);
	g_test_add("/lightsite-billing/unconfigured", Fixture, NULL, setup, test_unconfigured, teardown);
	g_test_add("/lightsite-billing/body", Fixture, NULL, setup, test_body, teardown);
	g_test_add("/lightsite-billing/rollback", Fixture, NULL, setup, test_rollback, teardown);
	g_test_add("/lightsite-billing/receipts-guarded", Fixture, NULL, setup, test_receipts_guarded, teardown);
	return g_test_run();
}
