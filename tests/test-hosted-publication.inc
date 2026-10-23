/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The billing clock starts when the site actually went live. The trusted
 * client may report that moment; Venture keeps the first one it is told. */
static void test_publication_time(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *enroll = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", enroll, 201);
	g_autoptr(GDateTime) now = venture_time_now(), live = g_date_time_add_minutes(now, -50), later = g_date_time_add_minutes(now, -5);
	g_autoptr(GDateTime) future = g_date_time_add_hours(now, 2), early = g_date_time_add_days(now, -3);
	g_autofree gchar *live_text = venture_time_to_string(live), *later_text = venture_time_to_string(later);
	g_autofree gchar *future_text = venture_time_to_string(future), *early_text = venture_time_to_string(early);
	g_autofree gchar *body = NULL, *again = NULL, *ahead = NULL, *before = NULL, *junk = NULL, *cancel = NULL, *extra = NULL;
	g_autoptr(JsonNode) published = NULL, replayed = NULL, refused = NULL, measured = NULL;
	g_autofree gchar *measure = g_strdup_printf("/api/v1/lightsite/billing/%" G_GINT64_FORMAT "/made-back", f->business);
	(void)unused;
	{
		/* Enrolled two days ago; the site went live fifty minutes ago. */
		g_autoptr(GDateTime) enrolled_at = g_date_time_add_days(now, -2);
		g_autofree gchar *date = venture_time_to_string(enrolled_at), *text = NULL, *sql = NULL;
		g_autoptr(GError) error = NULL;
		json_object_set_string_member(json_node_get_object(enrolled), "enrolled_at", date);
		text = venture_json_to_string(enrolled, FALSE);
		sql = g_strdup_printf("UPDATE lightsite_billing_receipts SET result='%s' WHERE business_id=%" G_GINT64_FORMAT " AND outcome='enroll'", text, f->business);
		g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
	}
	ahead = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"published_at\":\"%s\"}", f->business, future_text);
	before = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"published_at\":\"%s\"}", f->business, early_text);
	junk = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"published_at\":\"yesterday\"}", f->business);
	extra = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"published_at\":\"%s\",\"note\":1}", f->business, live_text);
	cancel = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"published_at\":\"%s\"}", f->business, live_text);
	/* A time in the future, before enrollment, unreadable, alongside other
	 * fields, or sent to another operation is refused and starts nothing. */
	refused = call(f, "/api/v1/lightsite/billing/publish", ahead, 422); g_clear_pointer(&refused, json_node_unref);
	refused = call(f, "/api/v1/lightsite/billing/publish", before, 422); g_clear_pointer(&refused, json_node_unref);
	refused = call(f, "/api/v1/lightsite/billing/publish", junk, 422); g_clear_pointer(&refused, json_node_unref);
	refused = call(f, "/api/v1/lightsite/billing/publish", extra, 422); g_clear_pointer(&refused, json_node_unref);
	refused = call(f, "/api/v1/lightsite/billing/cancel", cancel, 422); g_clear_pointer(&refused, json_node_unref);
	refused = call(f, measure, NULL, 409); g_clear_pointer(&refused, json_node_unref);
	/* Nothing was charged before go-live, so there is no money to give back. */
	refused = call(f, "/api/v1/lightsite/billing/money-back", (body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business)), 409);
	g_clear_pointer(&refused, json_node_unref); g_clear_pointer(&body, g_free);
	body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"published_at\":\"%s\"}", f->business, live_text);
	published = call(f, "/api/v1/lightsite/billing/publish", body, 200);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(published), "go_live_at"), ==, live_text);
	/* A later report never moves the clock. */
	again = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"published_at\":\"%s\"}", f->business, later_text);
	replayed = call(f, "/api/v1/lightsite/billing/publish", again, 200);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(replayed), "go_live_at"), ==, live_text);
	measured = call(f, measure, NULL, 200);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(measured), "go_live_at"), ==, live_text);
	g_test_message("Publication time: future, pre-enrollment, unreadable and misplaced times refused; first actual time kept");
}
