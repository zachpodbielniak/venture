/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The 30-day setup window. Nothing is charged before the business's site
 * goes live: no sweep, renewal or first-charge call may issue an invoice for
 * an unpublished deferred enrollment, on day 14, day 31 or ever. Lightsite's
 * daemon switches the account off at expires_at; publishing later still
 * activates and bills it. If this regresses, a business is charged for a
 * site it never launched. */

/* Move a fresh enrollment back in time, as retained fixture state (no
 * production caller can edit receipts). Legacy shapes the enrollment the way
 * the fourteen-day release left it: a fourteen-day trial and a receipt with
 * charge_due_at instead of expires_at (the caller removes the identity). */
static void hosted_backdate(Fixture *f, JsonNode *enrolled, gint64 days_ago, gboolean legacy)
{
	JsonObject *object = json_node_get_object(enrolled);
	g_autoptr(GDateTime) now = venture_time_now(), enrolled_at = g_date_time_add_days(now, -days_ago);
	g_autoptr(GDateTime) trial_end = g_date_time_add_days(enrolled_at, legacy ? 14 : 30);
	g_autofree gchar *start = venture_time_to_string(enrolled_at), *end = venture_time_to_string(trial_end);
	g_autofree gchar *text = NULL, *sql = NULL;
	g_autoptr(GError) error = NULL;
	json_object_set_string_member(object, "enrolled_at", start);
	if (legacy) {
		json_object_remove_member(object, "expires_at");
		json_object_set_string_member(object, "charge_due_at", end);
	} else json_object_set_string_member(object, "expires_at", end);
	text = venture_json_to_string(enrolled, FALSE);
	sql = g_strdup_printf("UPDATE lightsite_billing_receipts SET result='%s' WHERE business_id=%" G_GINT64_FORMAT " AND outcome='enroll'", text, f->business);
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("UPDATE customer_subscriptions SET current_period_start='%s',current_period_end='%s',trial_end='%s',billing_anchor='%s',last_event_at='%s' WHERE id=%" G_GINT64_FORMAT,
		start, end, end, end, start, json_object_get_int_member(object, "subscription_id"));
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
}

static guint hosted_invoices(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(query, f->billing);
	invoices = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	return invoices->len;
}

static void hosted_sweep(Fixture *f, GDateTime *at)
{
	g_autoptr(VentureEntity) sweep = g_object_new(VENTURE_TYPE_BILLING_REQUEST, "action", "renew-sweep", "at", at, NULL);
	venture_entity_set_organization_id(sweep, f->billing); save(f, sweep);
}

static gint hosted_status(Fixture *f, gint64 subscription)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) sub = venture_database_get(f->db, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription, &error);
	gint state = -1;
	g_assert_no_error(error); g_object_get(sub, "status", &state, NULL);
	return state;
}

/* Day 14 (the old charge date), an explicit renewal and day 31 (after the
 * window) all leave the unpublished enrollment an uninvoiced trial. */
static void test_window_sweeps(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *enroll = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", enroll, 201), first = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), day14 = g_date_time_add_hours(now, 14 * 24 + 1);
	g_autoptr(GDateTime) day15 = g_date_time_add_days(now, 15), day31 = g_date_time_add_days(now, 31);
	gint64 subscription = json_object_get_int_member(json_node_get_object(enrolled), "subscription_id");
	(void)unused;
	hosted_sweep(f, day14);
	{
		g_autoptr(VentureEntity) renew = g_object_new(VENTURE_TYPE_BILLING_REQUEST,
			"action", "renew", "subscription-id", subscription, "at", day15, NULL);
		venture_entity_set_organization_id(renew, f->billing); save(f, renew);
	}
	hosted_sweep(f, day31);
	g_assert_cmpuint(hosted_invoices(f), ==, 0);
	g_assert_cmpint(hosted_status(f, subscription), ==, 0);
	first = call(f, "/api/v1/lightsite/billing/first-charge", body, 200);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(first), "state"), ==, "awaiting_card");
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(first), "invoice_id"), ==, 0);
	g_assert_cmpuint(hosted_invoices(f), ==, 0);
	g_test_message("Unpublished: day-14 sweep, a renewal and a day-31 sweep create zero invoices; trial kept");
}

/* At or after expires_at the unpublished first charge reports "expired"
 * and still issues nothing; publication is what bills. */
static void test_window_expired(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *enroll = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", enroll, 201);
	g_autoptr(JsonNode) expired = NULL, replay = NULL, published = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gint64 subscription = json_object_get_int_member(json_node_get_object(enrolled), "subscription_id");
	(void)unused;
	hosted_backdate(f, enrolled, 31, FALSE);
	hosted_sweep(f, now);
	expired = call(f, "/api/v1/lightsite/billing/first-charge", body, 200);
	replay = call(f, "/api/v1/lightsite/billing/first-charge", body, 200);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(expired), "state"), ==, "expired");
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(expired), "invoice_id"), ==, 0);
	g_assert_true(json_node_equal(expired, replay));
	g_assert_cmpuint(hosted_invoices(f), ==, 0);
	g_assert_cmpint(hosted_status(f, subscription), ==, 0);
	/* Publication after the window is not refused: with no card yet it
	 * waits for one, as it would on day one. */
	published = call(f, "/api/v1/lightsite/billing/publish", body, 200);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(published), "state"), ==, "awaiting_card");
	g_assert_cmpuint(hosted_invoices(f), ==, 0);
	g_test_message("Day 31 unpublished: sweep and first-charge create zero invoices; state expired");
}

/* An enrollment made under the fourteen-day release keeps its receipt, but
 * the upgrade gives its subscription the hosted identity, so its trial end
 * charges nothing either and its expiry is enrolled_at plus 30 days. */
static void test_window_legacy(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *enroll = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", enroll, 201);
	g_autoptr(JsonNode) waiting = NULL, expired = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = g_build_filename("migrations", g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI") ? "postgresql" : "sqlite",
		"000956_hosted_deferred_identity.sql", NULL);
	g_autofree gchar *script = NULL, *identity = NULL, *expected = g_strdup_printf("hosted:%" G_GINT64_FORMAT, f->business);
	gint64 subscription = json_object_get_int_member(json_node_get_object(enrolled), "subscription_id");
	(void)unused;
	hosted_backdate(f, enrolled, 15, TRUE);
	{
		/* The fourteen-day release started the trial with no identity. */
		g_autofree gchar *sql = g_strdup_printf("UPDATE customer_subscriptions SET external_id=NULL WHERE id=%" G_GINT64_FORMAT, subscription);
		g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
	}
	g_assert_true(g_file_get_contents(path, &script, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_migrations_execute_sql(venture_database_get_connection(f->db), script, &error)); g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) sub = venture_database_get(f->db, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription, &error);
		g_assert_no_error(error); g_object_get(sub, "external-id", &identity, NULL);
		g_assert_cmpstr(identity, ==, expected);
	}
	/* Day 15: the fourteen-day trial ended yesterday. */
	hosted_sweep(f, now);
	g_assert_cmpuint(hosted_invoices(f), ==, 0);
	g_assert_cmpint(hosted_status(f, subscription), ==, 0);
	waiting = call(f, "/api/v1/lightsite/billing/first-charge", body, 200);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(waiting), "state"), ==, "awaiting_card");
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(waiting), "invoice_id"), ==, 0);
	/* Day 31: past enrolled_at + 30 days. */
	hosted_backdate(f, enrolled, 31, TRUE);
	{
		g_autoptr(GDateTime) later = g_date_time_add_days(now, 1);
		hosted_sweep(f, later);
	}
	expired = call(f, "/api/v1/lightsite/billing/first-charge", body, 200);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(expired), "state"), ==, "expired");
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(expired), "invoice_id"), ==, 0);
	g_assert_cmpuint(hosted_invoices(f), ==, 0);
	g_test_message("Fourteen-day enrollment after upgrade: hosted identity, day-15 and day-32 sweeps create zero invoices; expired at enrolled_at+30");
}
