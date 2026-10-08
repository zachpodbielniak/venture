/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Cancellation is a durable decision, including delayed publication and
 * collection requests that were queued before the owner cancelled. */
static void test_cancel_early(Fixture *f, gconstpointer annual)
{
	g_autofree gchar *enroll = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "%s}",
		f->business, f->price, annual ? ",\"payment_path\":\"installments\"" : "");
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	const gchar *route = annual ? "/api/v1/lightsite/billing/prepay" : "/api/v1/lightsite/billing/enroll";
	g_autoptr(JsonNode) enrolled = call(f, route, enroll, 201);
	g_autoptr(JsonNode) cancelled = call(f, "/api/v1/lightsite/billing/cancel", body, 200);
	g_autoptr(JsonNode) replay = call(f, "/api/v1/lightsite/billing/cancel", body, 200);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), future = g_date_time_add_days(now, 40);
	g_autoptr(VentureEntity) sweep = g_object_new(VENTURE_TYPE_BILLING_REQUEST, "action", "renew-sweep", "at", future, NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	gint state = 0;
	guint i;
	const gchar *queued[] = { "/api/v1/lightsite/billing/publish", "/api/v1/lightsite/billing/first-charge" };
	g_assert_true(json_node_equal(cancelled, replay));
	g_assert_true(json_object_get_boolean_member(json_node_get_object(cancelled), "no_charge"));
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(cancelled), "state"), ==, "cancelled");
	for (i = 0; i < G_N_ELEMENTS(queued); i++) {
		g_autoptr(JsonNode) delayed = call(f, queued[i], body, 200);
		g_assert_true(json_node_equal(cancelled, delayed));
	}
	{
		g_autoptr(JsonNode) setup_denied = call(f, "/api/v1/lightsite/billing/setup", body, 409);
		g_autoptr(JsonNode) enroll_denied = call(f, route, enroll, 409);
		g_autofree gchar *subscribe = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_code\":\"team\",\"idempotency_key\":\"after-cancel\"}", f->business);
		g_autoptr(JsonNode) subscribe_denied = call(f, "/api/v1/lightsite/billing/subscriptions", subscribe, 409);
	}
	venture_entity_set_organization_id(sweep, f->billing); save(f, sweep);
	sub = venture_database_get(f->db, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION,
		json_object_get_int_member(json_node_get_object(enrolled), "subscription_id"), &error);
	g_assert_no_error(error); g_object_get(sub, "status", &state, NULL); g_assert_cmpint(state, ==, 4);
	venture_query_set_organization(query, f->billing);
	invoices = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	g_assert_cmpuint(invoices->len, ==, 0);
	{
		g_autofree gchar *view_path = g_strdup_printf("/api/v1/lightsite/billing/%" G_GINT64_FORMAT, f->business);
		g_autoptr(JsonNode) view = call(f, view_path, NULL, 200);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(view), "state"), ==, "cancelled");
	}
	g_test_message("Early cancellation: retries identical, delayed publication and day-14 sweep create zero invoices");
}

static void test_cancel_late(Fixture *f, gconstpointer kind)
{
	g_autofree gchar *enroll = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", enroll, 201);
	g_autoptr(JsonNode) published = NULL, cancelled = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), past = g_date_time_add_days(now, -14);
	g_autofree gchar *date = venture_time_to_string(past), *sql = NULL;
	gint64 subscription = json_object_get_int_member(json_node_get_object(enrolled), "subscription_id");
	if (!strcmp(kind, "published")) {
		g_autoptr(VentureEntity) activate = g_object_new(VENTURE_TYPE_BILLING_REQUEST,
			"action", "activate", "subscription-id", subscription, "at", now, NULL);
		published = call(f, "/api/v1/lightsite/billing/publish", body, 200);
		venture_entity_set_organization_id(activate, f->billing); save(f, activate);
	} else {
		JsonObject *object = json_node_get_object(enrolled);
		g_autofree gchar *text = NULL;
		json_object_set_string_member(object, "enrolled_at", date);
		text = venture_json_to_string(enrolled, FALSE);
		sql = g_strdup_printf("UPDATE lightsite_billing_receipts SET result='%s' WHERE business_id=%" G_GINT64_FORMAT " AND outcome='enroll'", text, f->business);
		g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
	}
	cancelled = call(f, "/api/v1/lightsite/billing/cancel", body, 200);
	g_assert_false(json_object_get_boolean_member(json_node_get_object(cancelled), "no_charge"));
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(cancelled), "state"), ==, "cancel_at_period_end");
	{
		g_autoptr(VentureEntity) sub = venture_database_get(f->db, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription, &error);
		gboolean scheduled = FALSE;
		g_assert_no_error(error); g_object_get(sub, "cancel-at-period-end", &scheduled, NULL); g_assert_true(scheduled);
	}
	if (!strcmp(kind, "published")) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
		g_autoptr(VentureQuery) credits = venture_query_new(VENTURE_TYPE_CUSTOMER_CREDIT);
		g_autoptr(GPtrArray) invoices = NULL, refunds = NULL;
		g_autoptr(VentureMoney) balance = NULL;
		venture_query_set_organization(query, f->billing); venture_query_set_organization(credits, f->billing);
		invoices = venture_database_find(f->db, query, &error); g_assert_no_error(error); g_assert_cmpuint(invoices->len, ==, 1);
		refunds = venture_database_find(f->db, credits, &error); g_assert_no_error(error); g_assert_cmpuint(refunds->len, ==, 0);
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
			venture_entity_get_id(g_ptr_array_index(invoices, 0)), NULL, &error);
		g_assert_no_error(error); g_assert_cmpint(venture_money_get_amount(balance), ==, 1000);
	}
	g_test_message("Late cancellation: normal period-end cancellation, no free-trial promise or automatic refund");
}

static void test_cancel_published_prepaid(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *enroll = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT ",\"payment_path\":\"single\"}", f->business, f->price);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/prepay", enroll, 201);
	g_autoptr(JsonNode) cancelled = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) before = venture_time_now(), after = NULL, earliest = NULL, latest = NULL, effective = NULL, stored = NULL;
	g_autoptr(JsonNode) published = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(GPtrArray) invoices = NULL;
	gint64 subscription = json_object_get_int_member(json_node_get_object(enrolled), "subscription_id");
	gint state = 0;
	(void)unused;
	/* No Stripe connection: publication still commits go_live_at, then collection fails. */
	published = call(f, "/api/v1/lightsite/billing/publish", body, 500);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(published), "error"), ==, "config");
	{
		g_autoptr(OrmResult) rows = venture_database_query_raw(f->db,
			"SELECT outcome FROM lightsite_billing_receipts WHERE outcome='publish'", NULL, &error);
		g_assert_no_error(error);
		g_assert_true(orm_result_next(rows));
	}
	after = venture_time_now();
	earliest = g_date_time_add_months(before, 24);
	latest = g_date_time_add_months(after, 24);
	cancelled = call(f, "/api/v1/lightsite/billing/cancel", body, 200);
	g_assert_false(json_object_get_boolean_member(json_node_get_object(cancelled), "no_charge"));
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(cancelled), "state"), ==, "cancel_at_period_end");
	effective = venture_time_from_string(json_object_get_string_member(json_node_get_object(cancelled), "effective_at"), &error);
	g_assert_no_error(error); g_assert_nonnull(effective);
	g_assert_cmpint(g_date_time_compare(effective, earliest), >=, 0);
	g_assert_cmpint(g_date_time_compare(effective, latest), <=, 0);
	sub = venture_database_get(f->db, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription, &error);
	g_assert_no_error(error); g_object_get(sub, "current-period-end", &stored, NULL);
	g_assert_cmpint(g_date_time_compare(stored, effective), ==, 0);
	venture_query_set_organization(query, f->billing);
	invoices = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	g_assert_cmpuint(invoices->len, ==, 0);
	{
		g_autoptr(GDateTime) later = g_date_time_add_days(effective, 1);
		g_autoptr(VentureEntity) sweep = g_object_new(VENTURE_TYPE_BILLING_REQUEST, "action", "renew-sweep", "at", later, NULL);
		venture_entity_set_organization_id(sweep, f->billing); save(f, sweep);
	}
	g_clear_object(&sub);
	sub = venture_database_get(f->db, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription, &error);
	g_assert_no_error(error); g_object_get(sub, "status", &state, NULL); g_assert_cmpint(state, ==, 4);
	g_clear_pointer(&invoices, g_ptr_array_unref);
	invoices = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	g_assert_cmpuint(invoices->len, ==, 0);
	g_test_message("Published prepaid cancellation stores the 24-month end and the sweep ends it with no invoice");
}

static void test_cancel_missing(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	g_autoptr(JsonNode) missing = call(f, "/api/v1/lightsite/billing/cancel", body, 404);
	g_autoptr(JsonNode) malformed = call(f, "/api/v1/lightsite/billing/cancel", "{\"organization_id\":\"wrong\"}", 422);
	(void)unused;
	{
		g_autofree gchar *original = g_steal_pointer(&f->token);
		g_autoptr(JsonNode) anonymous = NULL;
		f->token = g_strdup("");
		anonymous = call(f, "/api/v1/lightsite/billing/cancel", body, 401);
		g_free(f->token); f->token = g_steal_pointer(&original);
	}
}
