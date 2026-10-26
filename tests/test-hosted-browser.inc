/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A fresh hosted owner signs in through the provider, never through an
 * operator-made local password. The callback returns to an authorized record. */
static gchar *browser_begin(Fixture *f, const gchar *target, gchar **browser)
{
	g_autofree gchar *encoded = g_uri_escape_string(target, NULL, FALSE);
	g_autofree gchar *path = g_strconcat("/auth/hosted/start?return_to=", encoded, NULL);
	g_autoptr(GUri) uri = NULL;
	g_autoptr(GHashTable) query = NULL;
	g_assert_cmpuint(exchange(f, "GET", path, NULL, NULL, NULL, NULL, NULL, browser), ==, 302);
	g_assert_nonnull(*browser); g_assert_nonnull(f->location);
	uri = g_uri_parse(f->location, G_URI_FLAGS_NONE, NULL);
	query = soup_form_decode(g_uri_get_query(uri));
	g_assert_cmpstr(g_hash_table_lookup(query, "response_type"), ==, "code");
	g_assert_cmpstr(g_hash_table_lookup(query, "client_id"), ==, "source-browser");
	g_assert_cmpstr(g_hash_table_lookup(query, "code_challenge_method"), ==, "S256");
	g_assert_cmpstr(g_hash_table_lookup(query, "redirect_uri"), ==, ORIGIN "/auth/hosted/callback");
	g_mutex_lock(&f->provider.mutex);
	g_free(f->provider.nonce); f->provider.nonce = g_strdup(g_hash_table_lookup(query, "nonce"));
	g_free(f->provider.pkce); f->provider.pkce = g_strdup(g_hash_table_lookup(query, "code_challenge"));
	g_mutex_unlock(&f->provider.mutex);
	return g_strdup_printf("/auth/hosted/callback?state=%s&code=fixture-code", (gchar *)g_hash_table_lookup(query, "state"));
}
static gchar *browser_login(Fixture *f, const gchar *target)
{
	g_autofree gchar *browser = NULL, *callback = browser_begin(f, target, &browser);
	gchar *session = NULL;
	g_assert_cmpuint(exchange(f, "GET", callback, browser, NULL, NULL, NULL, NULL, &session), ==, 302);
	g_assert_cmpstr(f->location, ==, target);
	g_assert_nonnull(session); g_assert_true(g_str_has_prefix(session, "venture_session="));
	/* Neither the browser binding nor the completed state can be replayed. */
	g_assert_cmpuint(exchange(f, "GET", callback, browser, NULL, NULL, NULL, NULL, NULL), ==, 401);
	return session;
}
static void test_browser_records(Fixture *f, gconstpointer data)
{
	g_autofree gchar *cookie = NULL, *body = NULL, *own_path = NULL, *other_path = NULL;
	g_autoptr(VentureEntity) user = NULL;
	gchar *password = NULL;
	gint64 owner, own, other;
	(void)data;
	own = sign_up(f, "browser-signup", "owner@example.test", "browser-owner", "Example Co", &owner);
	other = sign_up(f, "other-signup", "other@example.test", "other-owner", "Other Example Co", NULL);
	user = venture_database_get(f->db, VENTURE_TYPE_USER, owner, NULL);
	g_object_get(user, "password-hash", &password, NULL);
	g_assert_true(password == NULL || !*password); g_free(password);
	own_path = g_strdup_printf("/e/organization/%" G_GINT64_FORMAT, own);
	other_path = g_strdup_printf("/e/organization/%" G_GINT64_FORMAT, other);
	g_assert_cmpuint(exchange(f, "GET", own_path, NULL, NULL, NULL, NULL, NULL, NULL), ==, 302);
	g_assert_true(g_str_has_prefix(f->location, "/auth/hosted/start?return_to="));
	g_assert_cmpuint(exchange(f, "GET", "/login", NULL, NULL, NULL, NULL, &body, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "Continue with Lightsite")); g_clear_pointer(&body, g_free);
	cookie = browser_login(f, own_path);
	g_assert_cmpuint(exchange(f, "GET", own_path, cookie, NULL, NULL, NULL, &body, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "Example Co")); g_assert_null(strstr(body, "Other Example Co"));
	g_assert_cmpuint(exchange(f, "GET", other_path, cookie, NULL, NULL, NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/account-authority", cookie, NULL, NULL, NULL, NULL, NULL), ==, 200);
}
static void test_browser_refusals(Fixture *f, gconstpointer data)
{
	g_autofree gchar *browser = NULL, *callback = NULL;
	(void)data;
	sign_up(f, "browser-signup", "owner@example.test", "browser-owner", "Example Co", NULL);
	callback = browser_begin(f, "/", &browser);
	g_assert_cmpuint(exchange(f, "GET", callback, NULL, NULL, NULL, NULL, NULL, NULL), ==, 401);
	g_assert_cmpuint(exchange(f, "GET", callback, browser, NULL, NULL, NULL, NULL, NULL), ==, 401);
	g_clear_pointer(&browser, g_free); g_clear_pointer(&callback, g_free);
	callback = browser_begin(f, "/", &browser);
	g_mutex_lock(&f->provider.mutex); f->provider.bad_nonce = TRUE; g_mutex_unlock(&f->provider.mutex);
	g_assert_cmpuint(exchange(f, "GET", callback, browser, NULL, NULL, NULL, NULL, NULL), ==, 401);
	g_clear_pointer(&browser, g_free); g_clear_pointer(&callback, g_free);
	g_mutex_lock(&f->provider.mutex); f->provider.bad_nonce = FALSE; f->provider.unlinked = TRUE; g_mutex_unlock(&f->provider.mutex);
	callback = browser_begin(f, "/", &browser);
	g_assert_cmpuint(exchange(f, "GET", callback, browser, NULL, NULL, NULL, NULL, NULL), ==, 401);
	g_assert_cmpuint(exchange(f, "GET", "/auth/hosted/start?return_to=https%3A%2F%2Fother.example", NULL, NULL, NULL, NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(exchange(f, "GET", "/auth/hosted/start?return_to=%2F%2Fother.example", NULL, NULL, NULL, NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(exchange(f, "GET", "/auth/hosted/start?return_to=%2F%5Cother.example", NULL, NULL, NULL, NULL, NULL, NULL), ==, 400);
	g_assert_cmpuint(exchange(f, "GET", "/auth/hosted/start?return_to=%2Fauth%2Fhosted%2Fstart", NULL, NULL, NULL, NULL, NULL, NULL), ==, 400);
}
static void test_browser_revocation(Fixture *f, gconstpointer data)
{
	g_autofree gchar *cookie = NULL, *statement = NULL;
	gint64 owner;
	(void)data;
	sign_up(f, "browser-signup", "owner@example.test", "browser-owner", "Example Co", &owner);
	cookie = browser_login(f, "/");
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/account-authority", cookie, NULL, NULL, NULL, NULL, NULL), ==, 200);
	statement = g_strdup_printf("UPDATE tenant_identities SET active=FALSE, version=version+1 WHERE user_id=%" G_GINT64_FORMAT, owner);
	g_assert_true(venture_database_execute(f->db, statement, NULL, NULL));
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/account-authority", cookie, NULL, NULL, NULL, NULL, NULL), ==, 401);
	g_clear_pointer(&statement, g_free);
	statement = g_strdup_printf("UPDATE tenant_identities SET active=TRUE, version=version+1 WHERE user_id=%" G_GINT64_FORMAT, owner);
	g_assert_true(venture_database_execute(f->db, statement, NULL, NULL));
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/account-authority", cookie, NULL, NULL, NULL, NULL, NULL), ==, 401);
}
static void test_browser_policy(Fixture *f, gconstpointer data)
{
	g_autofree gchar *browser = NULL, *callback = NULL, *cookie = NULL, *body = NULL;
	(void)data;
	sign_up(f, "browser-signup", "owner@example.test", "browser-owner", "Example Co", NULL);
	g_object_set(f->config, "hosted-identity-issuer", "https://other.example.test", NULL);
	g_assert_false(venture_oidc_service_hosted_enabled(venture_oidc_service_get(f->db)));
	g_object_set(f->config, "hosted-identity-issuer", f->provider.issuer, NULL);
	cookie = browser_login(f, "/");
	callback = browser_begin(f, "/", &browser);
	/* Changing the deployment's client invalidates both pending and signed state. */
	g_object_set(f->config, "hosted-identity-browser-client", "replacement-client", NULL);
	g_assert_cmpuint(exchange(f, "GET", callback, browser, NULL, NULL, NULL, NULL, NULL), ==, 401);
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/account-authority", cookie, NULL, NULL, NULL, NULL, NULL), ==, 401);
	g_object_set(f->config, "hosted-identity-browser-client", "", NULL);
	g_assert_cmpuint(exchange(f, "GET", "/login", NULL, NULL, NULL, NULL, &body, NULL), ==, 200);
	g_assert_null(strstr(body, "Continue with Lightsite"));
	g_assert_cmpuint(exchange(f, "GET", "/auth/hosted/start", NULL, NULL, NULL, NULL, NULL, NULL), ==, 401);
	g_object_set(f->config, "hosted-identity-browser-client", "source-browser", "security-cookie-secure", FALSE, NULL);
	g_assert_cmpuint(exchange(f, "GET", "/auth/hosted/start", NULL, NULL, NULL, NULL, NULL, NULL), ==, 401);
}
static void test_browser_mfa(Fixture *f, gconstpointer data)
{
	gint64 owner;
	VentureMfaService *mfa = venture_mfa_service_get(f->db);
	g_autoptr(VentureMfaEnrolment) enrolment = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autofree guchar *secret = NULL;
	g_autofree gchar *code = NULL, *browser = NULL, *callback = NULL, *challenge_cookie = NULL;
	g_autofree gchar *form = NULL, *session = NULL, *statement = NULL;
	g_auto(GStrv) recovery = NULL;
	gsize length = 0;
	(void)data;
	owner = 0;
	sign_up(f, "browser-signup", "owner@example.test", "browser-owner", "Example Co", &owner);
	enrolment = venture_mfa_service_begin_enrolment(mfa, owner, "Example", "owner", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(enrolment);
	secret = venture_base32_decode(enrolment->secret, &length);
	code = venture_totp_code(secret, length, venture_totp_counter(now), VENTURE_TOTP_DIGITS);
	recovery = venture_mfa_service_confirm_enrolment(mfa, owner, code, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(recovery);
	callback = browser_begin(f, "/", &browser);
	g_assert_cmpuint(exchange(f, "GET", callback, browser, NULL, NULL, NULL, NULL, &challenge_cookie), ==, 302);
	g_assert_cmpstr(f->location, ==, "/login/mfa");
	g_assert_nonnull(challenge_cookie); g_assert_true(g_str_has_prefix(challenge_cookie, "venture_mfa="));
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/account-authority", challenge_cookie, NULL, NULL, NULL, NULL, NULL), ==, 401);
	form = soup_form_encode("code", recovery[0], NULL);
	g_assert_cmpuint(exchange(f, "POST", "/login/mfa", challenge_cookie, NULL, "application/x-www-form-urlencoded", form, NULL, &session), ==, 302);
	g_assert_nonnull(session); g_assert_true(g_str_has_prefix(session, "venture_session="));
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/account-authority", session, NULL, NULL, NULL, NULL, NULL), ==, 200);
	statement = g_strdup_printf("UPDATE tenant_identities SET active=FALSE WHERE user_id=%" G_GINT64_FORMAT, owner);
	g_assert_true(venture_database_execute(f->db, statement, NULL, NULL));
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/account-authority", session, NULL, NULL, NULL, NULL, NULL), ==, 401);
}
static void test_browser_business_context(Fixture *f, gconstpointer data)
{
	gint64 business;
	g_autofree gchar *cookie = NULL, *payload = NULL, *body = NULL;
	(void)data;
	business = sign_up(f, "browser-signup", "owner@example.test", "browser-owner", "Example Co", NULL);
	cookie = browser_login(f, "/e/lead");
	payload = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"name\":\"Example browser enquiry\"}", business);
	g_assert_cmpuint(exchange(f, "POST", "/api/v1/lead", cookie, NULL, "application/json", payload, NULL, NULL), ==, 201);
	g_assert_cmpuint(exchange(f, "GET", "/e/lead", cookie, NULL, NULL, NULL, &body, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "Example browser enquiry"));
}

static void test_browser_edit_pages(Fixture *f, gconstpointer data)
{
	const gchar *types[] = { "quote", "invoice", NULL };
	g_autofree gchar *cookie = NULL;
	gint64 business;
	guint i;
	(void)data;
	business = sign_up(f, "browser-signup", "owner@example.test", "browser-owner", "Example Co", NULL);
	cookie = browser_login(f, "/");
	for (i = 0; types[i]; i++) {
		g_autofree gchar *path = g_strdup_printf("/api/v1/%s", types[i]), *body = NULL, *edit = NULL;
		g_autofree gchar *payload = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"number\":\"EXAMPLE-1\",\"currency\":\"USD\"}", business);
		g_autoptr(JsonNode) record = NULL;
		g_assert_cmpuint(exchange(f, "POST", path, cookie, NULL, "application/json", payload, &body, NULL), ==, 201);
		record = venture_json_parse(body, NULL); g_assert_nonnull(record);
		edit = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT "/edit", types[i], json_object_get_int_member(json_node_get_object(record), "id"));
		g_clear_pointer(&body, g_free);
		g_assert_cmpuint(exchange(f, "GET", edit, cookie, NULL, NULL, NULL, &body, NULL), ==, 200);
		g_assert_nonnull(strstr(body, "name=\"notes\""));
		/* The owner's form must not tell them to call an internal API. */
		g_assert_null(strstr(body, "Use Venture"));
	}
}
/* The owner's calendar is the business they were given, with no sidebar
 * cookie and no organization id. Another business stays unread. Calendar
 * is in the menu on the first page. */
static void test_browser_calendar(Fixture *f, gconstpointer data)
{
	gint64 business, other;
	g_autofree gchar *token = NULL, *payload = NULL, *body = NULL, *cookie = NULL;
	g_autofree gchar *other_query = NULL;
	(void)data;
	business = sign_up(f, "calendar-signup", "calendar-owner@example.test", "browser-owner", "Example Co", NULL);
	other = sign_up(f, "calendar-other", "calendar-other@example.test", "other-owner", "Other Example Co", NULL);
	token = token_for(f, "browser-owner");
	payload = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"name\":\"Roof visit\","
		"\"starts_at\":\"2026-12-11T15:00:00Z\",\"ends_at\":\"2026-12-11T16:00:00Z\"}", business);
	g_assert_cmpuint(exchange(f, "POST", "/api/v1/event", NULL, token, "application/json", payload, &body, NULL), ==, 201);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/calendar?from=2026-12-01&to=2026-12-31", NULL, token,
		NULL, NULL, &body, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "Roof visit"));
	g_assert_null(strstr(body, "Other Example Co"));
	g_clear_pointer(&body, g_free);
	other_query = g_strdup_printf("/api/v1/calendar?from=2026-12-01&to=2026-12-31&organization_id=%" G_GINT64_FORMAT, other);
	g_assert_cmpuint(exchange(f, "GET", other_query, NULL, token, NULL, NULL, NULL, NULL), ==, 404);
	cookie = browser_login(f, "/");
	g_assert_cmpuint(exchange(f, "GET", "/", cookie, NULL, NULL, NULL, &body, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "href=\"/calendar\""));
	g_assert_nonnull(strstr(body, "Calendar"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(exchange(f, "GET", "/calendar?view=month&date=2026-12-11", cookie, NULL, NULL, NULL, &body, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "planner-calendar"));
	g_assert_nonnull(strstr(body, "Roof visit"));
	g_assert_null(strstr(body, "Venture id"));
}
static void test_browser_token_refusals(Fixture *f, gconstpointer data)
{
	guint defect;
	(void)data;
	sign_up(f, "browser-signup", "owner@example.test", "browser-owner", "Example Co", NULL);
	for (defect = 1; defect <= 5; defect++) {
		g_autofree gchar *browser = NULL, *callback = NULL, *session = NULL;
		callback = browser_begin(f, "/", &browser);
		g_mutex_lock(&f->provider.mutex); f->provider.claim_defect = defect; g_mutex_unlock(&f->provider.mutex);
		g_assert_cmpuint(exchange(f, "GET", callback, browser, NULL, NULL, NULL, NULL, &session), ==, 401);
		g_assert_null(session);
	}
}

/* Ordinary owner navigation must use their business, not the workspace's
 * billing organization. Compare the actual receivable and CSV to the
 * explicitly scoped report so an empty successful report cannot pass. */
static void test_browser_reports(Fixture *f, gconstpointer data)
{
	gint64 business, other;
	g_autofree gchar *cookie = NULL, *path = NULL, *expected = NULL, *actual = NULL;
	g_autoptr(VentureEntity) customer = NULL, invoice = NULL, line = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *formats[] = { "json", "csv", NULL };
	guint i;
	(void)data;
	business = sign_up(f, "browser-signup", "owner@example.test", "browser-owner", "Example Co", NULL);
	other = sign_up(f, "other-signup", "other@example.test", "other-owner", "Other Example Co", NULL);
	customer = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", business, "name", "Example Customer", NULL);
	g_assert_true(venture_database_save(f->db, customer, NULL, &error));
	g_assert_no_error(error);
	invoice = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", business,
		"company-id", venture_entity_get_id(customer), "number", "SYNTHETIC-REPORT-1", NULL);
	g_assert_true(venture_entity_set_field_from_string(invoice, "issued-at", "2026-01-01", &error));
	g_assert_true(venture_database_save(f->db, invoice, NULL, &error));
	g_assert_no_error(error);
	line = g_object_new(VENTURE_TYPE_INVOICE_LINE, "organization-id", business,
		"invoice-id", venture_entity_get_id(invoice), "description", "Synthetic work", "quantity", 1.0, NULL);
	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "USD 125.00", &error));
	g_assert_true(venture_database_save(f->db, line, NULL, &error));
	g_assert_no_error(error);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	g_assert_true(venture_database_save(f->db, invoice, NULL, &error));
	g_assert_no_error(error);
	cookie = browser_login(f, "/reports/receivables");
	for (i = 0; formats[i]; i++) {
		g_clear_pointer(&path, g_free);
		g_clear_pointer(&expected, g_free);
		g_clear_pointer(&actual, g_free);
		path = g_strdup_printf("/api/v1/reports/receivables?period=2026&as_of=2026-10-25&format=%s&organization_id=%" G_GINT64_FORMAT, formats[i], business);
		g_assert_cmpuint(exchange(f, "GET", path, cookie, NULL, NULL, NULL, &expected, NULL), ==, 200);
		g_clear_pointer(&path, g_free);
		path = g_strdup_printf("/api/v1/reports/receivables?period=2026&as_of=2026-10-25&format=%s", formats[i]);
		g_assert_cmpuint(exchange(f, "GET", path, cookie, NULL, NULL, NULL, &actual, NULL), ==, 200);
		g_assert_cmpstr(actual, ==, expected);
	}
	g_clear_pointer(&actual, g_free);
	g_assert_cmpuint(exchange(f, "GET", "/reports/receivables?period=2026&as_of=2026-10-25", cookie, NULL, NULL, NULL, &actual, NULL), ==, 200);
	g_assert_nonnull(strstr(actual, "125.00"));
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("organization_id=%" G_GINT64_FORMAT, business);
	g_assert_nonnull(strstr(actual, path));
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/reports/receivables?organization_id=%" G_GINT64_FORMAT, other);
	g_assert_cmpuint(exchange(f, "GET", path, cookie, NULL, NULL, NULL, NULL, NULL), ==, 404);
}

static void test_browser_report_selection(Fixture *f, gconstpointer data)
{
	gint64 own, other;
	g_autofree gchar *cookie = NULL, *selected = NULL, *body = NULL, *expected = NULL, *path = NULL;
	(void)data;
	own = sign_up(f, "browser-signup", "owner@example.test", "browser-owner", "Example Co", NULL);
	other = sign_up(f, "other-signup", "other@example.test", "other-owner", "Other Example Co", NULL);
	cookie = browser_login(f, "/reports/balance_sheet");
	path = g_strdup_printf("/api/v1/reports/balance_sheet?period=2026&organization_id=%" G_GINT64_FORMAT, own);
	g_assert_cmpuint(exchange(f, "GET", path, cookie, NULL, NULL, NULL, &expected, NULL), ==, 200);
	selected = g_strdup_printf("%s; venture_entity=%" G_GINT64_FORMAT, cookie, own);
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/reports/balance_sheet?period=2026", selected, NULL, NULL, NULL, &body, NULL), ==, 200);
	g_assert_cmpstr(body, ==, expected);
	g_assert_cmpuint(exchange(f, "GET", "/reports/balance_sheet?period=2026", selected, NULL, NULL, NULL, NULL, NULL), ==, 200);
	g_clear_pointer(&selected, g_free);
	selected = g_strdup_printf("%s; venture_entity=%" G_GINT64_FORMAT, cookie, other);
	g_assert_cmpuint(exchange(f, "GET", "/reports/receivables", selected, NULL, NULL, NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/reports/receivables?organization_id=invalid", cookie, NULL, NULL, NULL, NULL, NULL), ==, 400);
}

/* Webhook controls may expose only the current business's destinations and
 * must retain the owner-only credential boundary after hosted classification. */
static void test_browser_webhooks(Fixture *f, gconstpointer data)
{
	gint64 own, other, editor;
	g_autoptr(VentureEntity) hook = NULL, foreign = NULL, stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookie = NULL, *body = NULL, *path = NULL, *payload = NULL;
	g_autofree gchar *secret = NULL, *editor_token = NULL;
	(void)data;
	own = sign_up(f, "browser-signup", "owner@example.test", "browser-owner", "Example Co", NULL);
	other = sign_up(f, "other-signup", "other@example.test", "other-owner", "Other Example Co", NULL);
	hook = g_object_new(VENTURE_TYPE_WEBHOOK, "organization-id", own,
		"name", "Synthetic own webhook", "url", "https://hooks.example.test/own",
		"secret", "synthetic-own-secret", "active", FALSE, NULL);
	foreign = g_object_new(VENTURE_TYPE_WEBHOOK, "organization-id", other,
		"name", "Synthetic foreign webhook", "url", "https://hooks.example.test/foreign",
		"secret", "synthetic-foreign-secret", "active", FALSE, NULL);
	if (!venture_database_save(f->db, hook, NULL, &error))
		g_error("own webhook setup: %s", error ? error->message : "unknown error");
	if (!venture_database_save(f->db, foreign, NULL, &error))
		g_error("foreign webhook setup: %s", error ? error->message : "unknown error");
	cookie = browser_login(f, "/webhooks");
	g_assert_cmpuint(exchange(f, "GET", "/webhooks", cookie, NULL, NULL, NULL, &body, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "Synthetic own webhook"));
	g_assert_null(strstr(body, "Synthetic foreign webhook"));
	g_assert_null(strstr(body, "synthetic-own-secret"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/webhooks", cookie, NULL, NULL, NULL, &body, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "Synthetic own webhook"));
	g_assert_null(strstr(body, "Synthetic foreign webhook"));
	g_assert_null(strstr(body, "synthetic-own-secret"));
	g_assert_cmpuint(exchange(f, "GET", "/e/webhook/new", cookie, NULL, NULL, NULL, NULL, NULL), ==, 200);
	payload = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"name\":\"Synthetic created webhook\",\"url\":\"https://hooks.example.test/new\",\"active\":false}", own);
	g_assert_cmpuint(exchange(f, "POST", "/api/v1/webhook", cookie, NULL, "application/json", payload, NULL, NULL), ==, 201);
	path = g_strdup_printf("/api/v1/webhooks/%" G_GINT64_FORMAT "/secret", venture_entity_get_id(hook));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(exchange(f, "POST", path, cookie, NULL, "application/json", "{\"secret\":\"synthetic-rotated-secret\"}", &body, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "synthetic-rotated-secret"));
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/webhooks/%" G_GINT64_FORMAT "/secret", venture_entity_get_id(foreign));
	g_assert_cmpuint(exchange(f, "POST", path, cookie, NULL, "application/json", "{}", NULL, NULL), ==, 404);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/webhook/%" G_GINT64_FORMAT, venture_entity_get_id(foreign));
	g_assert_cmpuint(exchange(f, "GET", path, cookie, NULL, NULL, NULL, NULL, NULL), ==, 404);
	stored = venture_database_get(f->db, VENTURE_TYPE_WEBHOOK, venture_entity_get_id(foreign), &error);
	g_assert_no_error(error); g_assert_nonnull(stored);
	g_object_get(stored, "secret", &secret, NULL);
	g_assert_cmpstr(secret, ==, "synthetic-foreign-secret");
	editor = member_with_password(f, "synthetic-webhook-editor", own, VENTURE_ORGANIZATION_ROLE_EDITOR);
	editor_token = mint(f->db, editor, VENTURE_USER_ROLE_EDITOR);
	g_assert_cmpuint(exchange(f, "GET", "/api/v1/webhooks", NULL, editor_token, NULL, NULL, NULL, NULL), ==, 403);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/webhooks/%" G_GINT64_FORMAT "/secret", venture_entity_get_id(hook));
	g_assert_cmpuint(exchange(f, "POST", path, NULL, editor_token, "application/json", "{}", NULL, NULL), ==, 403);
}
