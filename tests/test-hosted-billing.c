/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A hosted signup must not create a payable invoice. A retry is the same
 * enrollment, and neither a browser nor another tenant may start collection. */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *directory, *token;
	gint64 billing, business, price, admin, owner;
} Fixture;
typedef struct { gboolean done; GBytes *bytes; GError *error; } Reply;
static void received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}
static JsonNode *call(Fixture *f, const gchar *path, const gchar *body, guint expected)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autofree gchar *auth = g_strconcat("Bearer ", f->token, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(body ? "POST" : "GET", url);
	g_autoptr(GBytes) payload = body ? g_bytes_new(body, strlen(body)) : NULL;
	Reply reply = { FALSE, NULL, NULL };
	JsonNode *answer;
	gsize length;
	const gchar *text;
	g_autofree gchar *terminated = NULL;
	soup_message_headers_replace(soup_message_get_request_headers(message), "Host", "example.test");
	soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", auth);
	if (payload) soup_message_set_request_body_from_bytes(message, "application/json", payload);
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	text = g_bytes_get_data(reply.bytes, &length);
	if (soup_message_get_status(message) != expected) g_test_message("Response for %s: %.*s", path, (gint)length, text);
	g_assert_cmpuint(soup_message_get_status(message), ==, expected);
	g_assert_cmpstr(soup_message_headers_get_one(soup_message_get_response_headers(message), "Cache-Control"), ==, "no-store");
	terminated = g_strndup(text, length);
	answer = venture_json_parse(terminated, NULL);
	g_bytes_unref(reply.bytes);
	return answer;
}
static void save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, row, NULL, &error);
	g_assert_no_error(error); g_assert_true(ok);
}
static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GInetAddress) address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *bind = g_inet_address_to_string(address);
	g_autoptr(VentureEntity) home = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Example Co", "active", TRUE, NULL);
	g_autoptr(VentureEntity) plan = g_object_new(VENTURE_TYPE_PLAN, "name", "Example plan", "active", TRUE, NULL);
	g_autoptr(VentureEntity) price = g_object_new(VENTURE_TYPE_PLAN_PRICE, "currency", "USD", "active", TRUE, NULL);
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(JsonNode) business = NULL;
	VentureTenantService *tenant;
	(void)unused;
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-hosted-billing-XXXXXX", &error); g_assert_no_error(error);
	g_object_set(f->config, "hosted-enabled", TRUE, "hosted-workspace-id", "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a",
		"hosted-origin", "https://example.test", "security-password-iterations", (gint64)2000,
		"state-dir", f->directory, "server-bind-address", bind, "server-port", (gint64)0, NULL);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	tenant = venture_tenant_service_get(f->db);
	g_assert_true(venture_tenant_service_configure(tenant, f->config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(tenant, &error)); g_assert_no_error(error);
	save(f, home); f->billing = venture_entity_get_id(home);
	{
		static const struct { const gchar *code; VentureAccountKind kind; } accounts[] = {
			{ "1000", VENTURE_ACCOUNT_KIND_ASSET }, { "1100", VENTURE_ACCOUNT_KIND_ASSET },
			{ "2100", VENTURE_ACCOUNT_KIND_LIABILITY }, { "2200", VENTURE_ACCOUNT_KIND_LIABILITY },
			{ "4000", VENTURE_ACCOUNT_KIND_INCOME }
		};
		guint i;
		for (i = 0; i < G_N_ELEMENTS(accounts); i++) {
			g_autoptr(VentureEntity) account = g_object_new(VENTURE_TYPE_ACCOUNT, "name", "Example account",
				"code", accounts[i].code, "kind", accounts[i].kind, "active", TRUE, NULL);
			venture_entity_set_organization_id(account, f->billing); save(f, account);
		}
	}
	venture_entity_set_organization_id(plan, f->billing); save(f, plan);
	venture_entity_set_organization_id(price, f->billing);
	g_object_set(price, "plan-id", venture_entity_get_id(plan), NULL);
	g_assert_true(venture_entity_set_field_from_string(price, "interval", unused && !strcmp(unused, "annual") ? "year" : "month", &error));
	g_assert_true(venture_entity_set_field_from_string(price, "amount", "10 USD", &error));
	g_assert_no_error(error); save(f, price); f->price = venture_entity_get_id(price);
	g_object_set(f->config, "lightsite-billing-organization-id", f->billing, NULL);
	g_assert_true(venture_tenant_service_bootstrap_admin(tenant, f->config, "operator", "test-password", FALSE, "Fixture", &error));
	g_assert_no_error(error);
	users = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	f->admin = venture_entity_get_id(g_ptr_array_index(users, 0));
	g_object_set(token, "name", "Example service", "user-id", venture_entity_get_id(g_ptr_array_index(users, 0)), "role", VENTURE_USER_ROLE_EDITOR, NULL);
	f->token = venture_api_token_generate(token); save(f, VENTURE_ENTITY(token));
	f->context = venture_context_new(f->config, f->db);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
	f->session = soup_session_new();
	business = call(f, "/api/v1/lightsite/signups", "{\"idempotency_key\":\"enrollment-example\",\"email\":\"owner@example.test\",\"issuer\":\"https://id.example.test\",\"subject\":\"example-owner\",\"business_name\":\"Example business\"}", 201);
	f->business = json_object_get_int_member(json_node_get_object(business), "organization_id");
	f->owner = json_object_get_int_member(json_node_get_object(business), "user_id");
}
static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server); g_clear_object(&f->context); g_clear_object(&f->session);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db); g_clear_object(&f->config);
	venture_test_remove_tree(f->directory); g_free(f->directory); g_free(f->token);
}
static void test_enrollment(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autoptr(JsonNode) first = NULL, replay = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	first = call(f, "/api/v1/lightsite/billing/enroll", body, 201);
	replay = call(f, "/api/v1/lightsite/billing/enroll", body, 200);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(first), "subscription_id"), ==,
		json_object_get_int_member(json_node_get_object(replay), "subscription_id"));
	venture_query_set_organization(query, f->billing);
	invoices = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	g_assert_cmpuint(invoices->len, ==, 0);
	{
		g_autofree gchar *collision = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_code\":\"example\",\"idempotency_key\":\"hosted:%" G_GINT64_FORMAT ":enroll\"}", f->business, f->business);
		g_autoptr(JsonNode) refused = call(f, "/api/v1/lightsite/billing/subscriptions", collision, 422);
	}
}
static void test_activation(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) customer = g_object_new(VENTURE_TYPE_COMPANY, "name", "Example customer", NULL);
	g_autoptr(VentureEntity) price = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) start = g_object_new(VENTURE_TYPE_BILLING_REQUEST, "action", "start", NULL);
	g_autoptr(VentureEntity) activate = g_object_new(VENTURE_TYPE_BILLING_REQUEST, "action", "activate", NULL);
	g_autoptr(GDateTime) now = venture_time_now();
	gint64 subscription = 0;
	(void)unused;
	venture_entity_set_organization_id(customer, f->billing); save(f, customer);
	g_object_set(price, "trial-days", (gint64)14, NULL); save(f, price);
	venture_entity_set_organization_id(start, f->billing);
	g_object_set(start, "company-id", venture_entity_get_id(customer), "plan-price-id", f->price, "at", now, NULL);
	save(f, start);
	g_object_get(start, "subscription-id", &subscription, NULL);
	venture_entity_set_organization_id(activate, f->billing);
	g_object_set(activate, "subscription-id", subscription, "at", now, NULL);
	save(f, activate);
}
static void test_publish_without_card(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *enroll = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	g_autoptr(JsonNode) enrollment = call(f, "/api/v1/lightsite/billing/enroll", enroll, 201);
	g_autoptr(JsonNode) first = call(f, "/api/v1/lightsite/billing/publish", body, 200);
	g_autoptr(JsonNode) second = call(f, "/api/v1/lightsite/billing/publish", body, 200);
	JsonObject *a = json_node_get_object(first), *b = json_node_get_object(second);
	(void)unused;
	g_assert_cmpstr(json_object_get_string_member(a, "state"), ==, "awaiting_card");
	g_assert_cmpstr(json_object_get_string_member(a, "go_live_at"), ==, json_object_get_string_member(b, "go_live_at"));
	g_assert_cmpint(json_object_get_int_member(a, "invoice_id"), ==, 0);
}
static void test_guarantee(Fixture *f, gconstpointer unused)
{
	gboolean zero = unused && (!strcmp(unused, "zero") || !strcmp(unused, "ineligible"));
	g_autofree gchar *enroll = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autofree gchar *publish = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"shortfall\":\"%s USD\",\"eligible\":%s,\"measurement\":\"example-measurement\"}",
		f->business, unused && !strcmp(unused, "zero") ? "0" : "100", unused && !strcmp(unused, "ineligible") ? "false" : "true");
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", enroll, 201);
	g_autoptr(JsonNode) published = call(f, "/api/v1/lightsite/billing/publish", publish, 200);
	g_autoptr(JsonNode) early = call(f, "/api/v1/lightsite/billing/guarantee", body, 409);
	g_autoptr(JsonNode) first = NULL, replay = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), ago = g_date_time_add_days(now, -91);
	g_autofree gchar *date = venture_time_to_string(ago);
	g_autofree gchar *sql = g_strdup_printf("UPDATE lightsite_billing_receipts SET result='{\"go_live_at\":\"%s\"}' WHERE business_id=%" G_GINT64_FORMAT " AND outcome='publish'", date, f->business);
	g_autoptr(GError) error = NULL;
	JsonObject *a, *b;
	(void)unused;
	/* Advance retained fixture time; production callers cannot edit receipts. */
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
	first = call(f, "/api/v1/lightsite/billing/guarantee", body, 200);
	replay = call(f, "/api/v1/lightsite/billing/guarantee", body, 200);
	a = json_node_get_object(first); b = json_node_get_object(replay);
	if (zero) g_assert_cmpint(json_object_get_int_member(a, "credit_id"), ==, 0);
	else g_assert_cmpint(json_object_get_int_member(a, "credit_id"), >, 0);
	g_assert_cmpint(json_object_get_int_member(a, "credit_id"), ==, json_object_get_int_member(b, "credit_id"));
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(a, "credit"), "amount"), ==,
		zero ? "0.00" : unused && !strcmp(unused, "annual") ? "2.50" : "30.00");
	{
		g_autofree gchar *changed = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"shortfall\":\"100 USD\",\"eligible\":true,\"measurement\":\"second-measurement\"}", f->business);
		g_autoptr(JsonNode) refused = call(f, "/api/v1/lightsite/billing/guarantee", changed, 409);
		g_autoptr(VentureQuery) credits = venture_query_new(VENTURE_TYPE_CUSTOMER_CREDIT);
		g_autoptr(GPtrArray) rows = NULL;
		venture_query_set_organization(credits, f->billing);
		rows = venture_database_find(f->db, credits, &error); g_assert_no_error(error);
		g_assert_cmpuint(rows->len, ==, zero ? 0 : 1);
		if (!zero) {
			gint64 customer = 0;
			g_object_get(g_ptr_array_index(rows, 0), "customer-id", &customer, NULL);
			g_assert_cmpint(customer, ==, json_object_get_int_member(json_node_get_object(enrolled), "company_id"));
		}
	}
}
static void test_made_back(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *enroll = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	g_autofree gchar *path = g_strdup_printf("/api/v1/lightsite/billing/%" G_GINT64_FORMAT "/made-back", f->business);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", enroll, 201);
	g_autoptr(JsonNode) published = call(f, "/api/v1/lightsite/billing/publish", body, 200);
	g_autoptr(JsonNode) answer = call(f, path, NULL, 200);
	(void)unused;
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_node_get_object(answer), "made_back"), "amount"), ==, "0.00");
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(GDateTime) now = venture_time_now();
		g_autoptr(VentureEntity) form = g_object_new(VENTURE_TYPE_LEAD_FORM, "name", "Example enquiries", "active", TRUE, NULL);
		g_autoptr(VentureEntity) site = g_object_new(VENTURE_TYPE_ATTRIBUTION_SITE, "name", "Example site",
			"origin", "https://site.example.test", "external-site-id", "example-site", "external-tenant-id", "example-tenant", "consent-policy", "analytics-v1", "active", TRUE, NULL);
		g_autoptr(JsonNode) settings = venture_json_parse("{\"tenant_id\":\"example-tenant\",\"environment\":\"test\",\"signing_secret\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}", &error);
		g_autoptr(GBytes) key = g_bytes_new_take(g_malloc0(32), 32);
		g_autoptr(VentureIntegrationConnection) connection = NULL;
		g_autoptr(JsonObject) payload = json_object_new(), fields = json_object_new(), options = json_object_new();
		g_autoptr(VentureAttributionSubmission) capture = NULL;
		g_autoptr(VentureEntity) lead = NULL, converted = NULL, deal = NULL, won_stage = NULL;
		g_autoptr(VentureDeal) won = NULL;
		g_autoptr(VentureQuery) stages = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
		gint64 lead_id = 0, deal_id = 0;
		g_assert_no_error(error);
		venture_entity_set_organization_id(form, f->business); save(f, form);
		g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error));
		connection = venture_attribution_settings_configure(f->db, f->business, settings, 0, 0, NULL, &error); g_assert_no_error(error);
		venture_entity_set_organization_id(site, f->business);
		g_object_set(site, "lead-form-id", venture_entity_get_id(form), "connection-id", venture_entity_get_id(VENTURE_ENTITY(connection)), NULL); save(f, site);
		json_object_set_int_member(payload, "version", 1); json_object_set_string_member(payload, "submission_id", "example-enquiry");
		json_object_set_string_member(fields, "name", "Example buyer"); json_object_set_string_member(fields, "email", "buyer@example.test");
		json_object_set_string_member(fields, "message", "Please send a quote"); json_object_set_object_member(payload, "fields", json_object_ref(fields));
		if (unused) capture = venture_attribution_service_capture(venture_attribution_service_get(f->db), venture_entity_get_uuid(site), "https://site.example.test", payload, now, &error);
		if (!unused || !g_strcmp0(unused, "mixed")) {
			g_autoptr(JsonNode) envelope = json_node_new(JSON_NODE_OBJECT);
			g_autofree gchar *text = NULL, *material = NULL, *signature = NULL;
			g_autofree gchar *timestamp = g_strdup_printf("%" G_GINT64_FORMAT, g_date_time_to_unix(now));
			g_autoptr(GBytes) bytes = NULL;
			const gchar *fixture_secret = json_object_get_string_member(json_node_get_object(settings), "signing_secret");
			g_clear_object(&capture);
			json_object_set_string_member(payload, "tenant_id", "example-tenant");
			json_object_set_string_member(payload, "site_id", "example-site");
			json_object_set_string_member(payload, "origin", "https://site.example.test");
			json_node_set_object(envelope, payload); text = venture_json_to_string(envelope, FALSE);
			material = g_strdup_printf("%s\n%s", timestamp, text);
			signature = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)fixture_secret, strlen(fixture_secret), material, -1);
			bytes = g_bytes_new(text, strlen(text));
			capture = venture_attribution_service_receive(venture_attribution_service_get(f->db), venture_entity_get_uuid(site),
				venture_entity_get_id(VENTURE_ENTITY(connection)), timestamp, signature, bytes, now, &error);
		}
		g_assert_no_error(error); g_assert_nonnull(capture); g_object_get(capture, "lead-id", &lead_id, NULL);
		lead = venture_database_get(f->db, VENTURE_TYPE_LEAD, lead_id, &error); g_assert_no_error(error);
		g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL); save(f, lead);
		json_object_set_boolean_member(options, "deal", TRUE);
		converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, options, NULL, &error); g_assert_no_error(error);
		g_object_get(converted, "converted-deal-id", &deal_id, NULL);
		deal = venture_database_get(f->db, VENTURE_TYPE_DEAL, deal_id, &error); g_assert_no_error(error);
		g_assert_true(venture_entity_set_field_from_string(deal, "value", "20 USD", &error)); save(f, deal);
		venture_query_set_organization(stages, f->business); venture_query_add_filter_string(stages, "kind", VENTURE_FILTER_OP_EQ, "won", NULL);
		won_stage = venture_database_find_one(f->db, stages, &error); g_assert_no_error(error); g_assert_nonnull(won_stage);
		won = venture_deal_service_move_stage(venture_database_get_deal_service(f->db), VENTURE_DEAL(deal), venture_entity_get_id(won_stage), "Accepted", NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(won);
		g_clear_pointer(&answer, json_node_unref); answer = call(f, path, NULL, 200);
		g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_node_get_object(answer), "made_back"), "amount"), ==, unused ? "0.00" : "20.00");
		g_test_message("Only a retained signed hosted acquisition contributes won value");
		if (!unused) {
			g_autoptr(VentureQuery) account_query = venture_query_new(VENTURE_TYPE_ACCOUNT);
			g_autoptr(GPtrArray) accounts = NULL;
			g_autoptr(VentureEntity) quote = g_object_new(VENTURE_TYPE_QUOTE, "number", "EXAMPLE-1", "currency", "USD", NULL);
			g_autoptr(VentureEntity) line = g_object_new(VENTURE_TYPE_QUOTE_LINE, "description", "Example work", NULL);
			gint64 company = 0;
			guint i;
			const gchar *actions[] = { "send", "accept" };
			venture_query_set_organization(account_query, f->billing);
			accounts = venture_database_find(f->db, account_query, &error); g_assert_no_error(error);
			for (i = 0; i < accounts->len; i++) {
				g_autofree gchar *code = NULL;
				gint kind = 0;
				g_autoptr(VentureEntity) account = NULL;
				g_object_get(g_ptr_array_index(accounts, i), "code", &code, "kind", &kind, NULL);
				account = g_object_new(VENTURE_TYPE_ACCOUNT, "name", "Example account", "code", code, "kind", kind, "active", TRUE, NULL);
				venture_entity_set_organization_id(account, f->business); save(f, account);
			}
			g_object_get(converted, "converted-company-id", &company, NULL);
			venture_entity_set_organization_id(quote, f->business);
			g_object_set(quote, "company-id", company, "deal-id", deal_id, NULL); save(f, quote);
			venture_entity_set_organization_id(line, f->business);
			g_object_set(line, "quote-id", venture_entity_get_id(quote), NULL);
			g_assert_true(venture_entity_set_field_from_string(line, "quantity", "1", &error));
			g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "15 USD", &error)); save(f, line);
			for (i = 0; i < G_N_ELEMENTS(actions); i++) {
				g_autoptr(VentureEntity) current = venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(quote), &error);
				g_autoptr(VentureEntity) action = g_object_new(VENTURE_TYPE_QUOTE_ACTION, "quote-id", venture_entity_get_id(quote),
					"action", actions[i], "expected-version", venture_entity_get_version(current), "accepted-by", "Example buyer", NULL);
				venture_entity_set_organization_id(action, f->business); save(f, action);
			}
			g_clear_pointer(&answer, json_node_unref); answer = call(f, path, NULL, 200);
			g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_node_get_object(answer), "invoiced"), "amount"), ==, "15.00");
			g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_node_get_object(answer), "overlap"), "amount"), ==, "15.00");
			g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_node_get_object(answer), "made_back"), "amount"), ==, "20.00");
			g_test_message("Same-day won job and accepted quote invoice count once, including a calendar-dated invoice");
		}
	}
}
static void test_boundary(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	guint i;
	(void)unused;
	for (i = 0; i < 2; i++) {
		g_autoptr(VentureApiToken) token = venture_api_token_new();
		g_autoptr(JsonNode) denied = NULL;
		g_object_set(token, "name", "Limited fixture", "user-id", i ? f->admin : f->owner,
			"role", i ? VENTURE_USER_ROLE_VIEWER : VENTURE_USER_ROLE_EDITOR, NULL);
		g_free(f->token); f->token = venture_api_token_generate(token); save(f, VENTURE_ENTITY(token));
		denied = call(f, "/api/v1/lightsite/billing/enroll", body, 403);
		{
			g_autofree gchar *prepaid = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT ",\"payment_path\":\"single\"}", f->business, f->price);
			g_autoptr(JsonNode) no_prepay = call(f, "/api/v1/lightsite/billing/prepay", prepaid, 403);
		}
	}
}
static void test_prepaid_enrollment(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT ",\"payment_path\":\"installments\"}", f->business, f->price);
	g_autoptr(JsonNode) first = call(f, "/api/v1/lightsite/billing/prepay", body, 201);
	g_autoptr(JsonNode) replay = call(f, "/api/v1/lightsite/billing/prepay", body, 200);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *object = json_node_get_object(first);
	(void)unused;
	g_assert_cmpint(json_object_get_int_member(object, "term_months"), ==, 24);
	g_assert_false(json_object_has_member(object, "charge_due_at"));
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(object, "total"), "amount"), ==, "20.00");
	g_assert_cmpint(json_object_get_int_member(object, "subscription_id"), ==,
		json_object_get_int_member(json_node_get_object(replay), "subscription_id"));
	{
		g_autoptr(GDateTime) now = venture_time_now(), later = g_date_time_add_days(now, 15);
		g_autoptr(VentureEntity) renew = g_object_new(VENTURE_TYPE_BILLING_REQUEST,
			"action", "renew", "subscription-id", json_object_get_int_member(object, "subscription_id"), "at", later, NULL);
		venture_entity_set_organization_id(renew, f->billing); save(f, renew);
		g_clear_object(&renew);
		renew = g_object_new(VENTURE_TYPE_BILLING_REQUEST, "action", "renew-sweep", "at", later, NULL);
		venture_entity_set_organization_id(renew, f->billing); save(f, renew);
	}
	venture_query_set_organization(query, f->billing);
	invoices = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	g_assert_cmpuint(invoices->len, ==, 0);
}
static void test_prepaid_terms(Fixture *f, gconstpointer annual)
{
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) answer = NULL;
	if (!annual) {
		body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT ",\"payment_path\":\"single\"}", f->business, f->price);
		answer = call(f, "/api/v1/lightsite/billing/prepay", body, 422);
		return;
	}
	body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT ",\"payment_path\":\"split\",\"shares\":[500,700,700]}", f->business, f->price);
	answer = call(f, "/api/v1/lightsite/billing/prepay", body, 422);
	g_clear_pointer(&body, g_free); g_clear_pointer(&answer, json_node_unref);
	body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT ",\"payment_path\":\"split\",\"shares\":[500,700,800]}", f->business, f->price);
	answer = call(f, "/api/v1/lightsite/billing/prepay", body, 201);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(answer), "shares")), ==, 3);
	g_clear_pointer(&answer, json_node_unref);
	answer = call(f, "/api/v1/lightsite/billing/prepay", body, 200);
	g_clear_pointer(&body, g_free); g_clear_pointer(&answer, json_node_unref);
	body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT ",\"payment_path\":\"split\",\"shares\":[600,600,800]}", f->business, f->price);
	answer = call(f, "/api/v1/lightsite/billing/prepay", body, 409);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/hosted-billing/prepaid-enrollment", Fixture, "annual", setup, test_prepaid_enrollment, teardown);
	g_test_add("/hosted-billing/prepaid-shares", Fixture, "annual", setup, test_prepaid_terms, teardown);
	g_test_add("/hosted-billing/prepaid-monthly-refused", Fixture, NULL, setup, test_prepaid_terms, teardown);
	g_test_add("/hosted-billing/enrollment", Fixture, NULL, setup, test_enrollment, teardown);
	g_test_add("/hosted-billing/activation", Fixture, NULL, setup, test_activation, teardown);
	g_test_add("/hosted-billing/publish-without-card", Fixture, NULL, setup, test_publish_without_card, teardown);
	g_test_add("/hosted-billing/guarantee", Fixture, NULL, setup, test_guarantee, teardown);
	g_test_add("/hosted-billing/guarantee-annual", Fixture, "annual", setup, test_guarantee, teardown);
	g_test_add("/hosted-billing/guarantee-zero", Fixture, "zero", setup, test_guarantee, teardown);
	g_test_add("/hosted-billing/guarantee-ineligible", Fixture, "ineligible", setup, test_guarantee, teardown);
	g_test_add("/hosted-billing/made-back", Fixture, NULL, setup, test_made_back, teardown);
	g_test_add("/hosted-billing/made-back-unsigned", Fixture, "unsigned", setup, test_made_back, teardown);
	g_test_add("/hosted-billing/made-back-mixed", Fixture, "mixed", setup, test_made_back, teardown);
	g_test_add("/hosted-billing/authority", Fixture, NULL, setup, test_boundary, teardown);
	return g_test_run();
}
