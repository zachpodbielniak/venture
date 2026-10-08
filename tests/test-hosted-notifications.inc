/* The feed exposes committed billing facts, not cards or another business. */
static JsonObject *notification(JsonNode *answer, const gchar *kind)
{
	JsonArray *events = json_object_get_array_member(json_node_get_object(answer), "events");
	guint i;
	for (i = 0; i < json_array_get_length(events); i++) {
		JsonObject *event = json_array_get_object_element(events, i);
		if (!g_strcmp0(json_object_get_string_member(event, "kind"), kind)) return event;
	}
	return NULL;
}
static void test_notifications(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/lightsite/billing/%" G_GINT64_FORMAT "/notifications", f->business);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autoptr(JsonNode) before = call(f, path, NULL, 200), enrolled = NULL, soon = NULL, cancelled = NULL, after = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), due = g_date_time_add_days(now, 1);
	g_autofree gchar *date = venture_time_to_string(due), *stored = NULL, *sql = NULL, *cancel = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(before), "organization_id"), ==, f->business);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(before), "workspace_id"), ==, "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a");
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(before), "events")), ==, 0);
	enrolled = call(f, "/api/v1/lightsite/billing/enroll", body, 201);
	json_object_set_string_member(json_node_get_object(enrolled), "charge_due_at", date);
	stored = venture_json_to_string(enrolled, FALSE);
	sql = g_strdup_printf("UPDATE lightsite_billing_receipts SET result='%s' WHERE business_id=%" G_GINT64_FORMAT " AND outcome='enroll'", stored, f->business);
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
	soon = call(f, path, NULL, 200);
	g_assert_nonnull(notification(soon, "charge_due"));
	g_assert_cmpstr(json_object_get_string_member(notification(soon, "charge_due"), "due_at"), ==, date);
	cancel = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
	cancelled = call(f, "/api/v1/lightsite/billing/cancel", cancel, 200);
	after = call(f, path, NULL, 200);
	g_assert_null(notification(after, "charge_due"));
	g_assert_nonnull(notification(after, "cancelled"));
	g_assert_cmpstr(json_object_get_string_member(notification(after, "cancelled"), "state"), ==, "cancelled");
	{
		g_autofree gchar *serialized = venture_json_to_string(after, FALSE);
		g_assert_null(strstr(serialized, "payment_method"));
		g_assert_null(strstr(serialized, "client_secret"));
	}
}
static void test_notification_adjustments(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/lightsite/billing/%" G_GINT64_FORMAT "/notifications", f->business);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", body, 201), answer = NULL;
	g_autoptr(VentureMoney) amount = venture_money_from_string("100.00 USD", NULL, NULL);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureEntity) payment = g_object_new(VENTURE_TYPE_PAYMENT, "method", "manual", NULL);
	g_autoptr(VentureEntity) opening = g_object_new(VENTURE_TYPE_PAYMENT, "method", "opening", NULL);
	g_autoptr(VentureEntity) credit = g_object_new(VENTURE_TYPE_CUSTOMER_CREDIT, "kind", "credit_note", NULL);
	g_autoptr(VentureEntity) refund = g_object_new(VENTURE_TYPE_REFUND, NULL);
	gint64 company = json_object_get_int_member(json_node_get_object(enrolled), "company_id");
	guint i;
	VentureEntity *records[] = {payment, opening, credit, refund};
	(void)unused;
	for (i = 0; i < G_N_ELEMENTS(records); i++) {
		venture_entity_set_organization_id(records[i], f->billing);
		g_object_set(records[i], "customer-id", company, "amount", amount, "date", now, NULL);
		if (i == 3) g_object_set(refund, "credit-id", venture_entity_get_id(credit), NULL);
		save(f, records[i]);
	}
	{
		g_autoptr(VentureEntity) invoice = g_object_new(VENTURE_TYPE_INVOICE, "number", "EXAMPLE-FAILED", "company-id", company, NULL);
		g_autofree gchar *uuid = g_uuid_string_random(), *sql = NULL;
		g_autoptr(GError) error = NULL;
		venture_entity_set_organization_id(invoice, f->billing); save(f, invoice);
		sql = g_strdup_printf("INSERT INTO stripe_checkouts (uuid,organization_id,invoice_id,status) VALUES ('%s',%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",'failed')", uuid, f->billing, venture_entity_get_id(invoice));
		g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
	}
	answer = call(f, path, NULL, 200);
	g_assert_nonnull(notification(answer, "receipt")); g_assert_nonnull(notification(answer, "credit"));
	g_assert_nonnull(notification(answer, "refund"));
	g_assert_nonnull(notification(answer, "payment_failed"));
	/* An opening balance is old money, not a newly received payment. */
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(answer), "events")), ==, 4);
	{
		g_autoptr(VentureMailMessage) prior = venture_mail_message_new(), queued = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *key = g_strdup_printf("receipt:%s", venture_entity_get_uuid(payment));
		venture_entity_set_organization_id(VENTURE_ENTITY(prior), f->billing);
		g_object_set(prior, "to", "owner@example.org", "subject", "Existing receipt", "text-body", "Receipt already prepared",
			"idempotency-key", key, "related-type", "payment", "related-id", venture_entity_get_id(payment), NULL);
		queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(f->db), prior, NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(queued);
		g_clear_pointer(&answer, json_node_unref); answer = call(f, path, NULL, 200);
		/* Upgrading must not add a branded duplicate beside an old receipt,
		 * whether that receipt is queued, retrying or already delivered. */
		g_assert_null(notification(answer, "receipt"));
		{
			g_autofree gchar *sql = g_strdup_printf("UPDATE mail_messages SET state='sent',deleted_at='2026-01-01T00:00:00Z' WHERE id=%" G_GINT64_FORMAT,
				venture_entity_get_id(VENTURE_ENTITY(queued)));
			g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
			g_clear_pointer(&answer, json_node_unref); answer = call(f, path, NULL, 200);
			g_assert_null(notification(answer, "receipt"));
		}
	}
}
static void test_notification_installment(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/lightsite/billing/%" G_GINT64_FORMAT "/notifications", f->business);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", body, 201), answer = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), published = g_date_time_add_days(now, -29);
	g_autofree gchar *date = venture_time_to_string(published), *uuid = g_uuid_string_random(), *sql = NULL;
	g_autoptr(GError) error = NULL;
	gint64 subscription = json_object_get_int_member(json_node_get_object(enrolled), "subscription_id");
	gint64 company = json_object_get_int_member(json_node_get_object(enrolled), "company_id");
	(void)unused;
	/* Retained provider fixture, never a public write of service-owned fields. */
	sql = g_strdup_printf("INSERT INTO stripe_prepayments (uuid,organization_id,subscription_id,company_id,payment_path,total_amount,total_currency,total_exponent,state,legs,published_at) VALUES ('%s',%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",'installments',20000,'USD',2,'waiting','{\"legs\":[{\"amount\":%d,\"paid\":true,\"method\":\"example-method\",\"setup\":\"example-setup\"},{\"amount\":%d,\"method\":\"example-method\"}]}','%s')", uuid, f->billing, subscription, company, 100 * 100, 100 * 100, date);
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
	answer = call(f, path, NULL, 200);
	g_assert_nonnull(notification(answer, "installment_due"));
	g_assert_cmpstr(json_object_get_string_member(notification(answer, "installment_due"), "amount"), ==, "100.00 USD");
	g_assert_nonnull(notification(answer, "card_saved"));
	{
		g_autofree gchar *text = venture_json_to_string(answer, FALSE);
		g_assert_null(strstr(text, "example-method")); g_assert_null(strstr(text, "example-setup"));
	}
	/* The installment worker retains a retry number after a definite failure.
	 * That is a payment failure even though no checkout row exists. */
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("UPDATE stripe_prepayments SET legs=json_set(legs,'$.legs[1].attempt',1) WHERE uuid='%s'", uuid);
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
	g_clear_pointer(&answer, json_node_unref); answer = call(f, path, NULL, 200);
	g_assert_nonnull(notification(answer, "payment_failed"));
	g_assert_cmpstr(json_object_get_string_member(notification(answer, "payment_failed"), "amount"), ==, "100.00 USD");
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("UPDATE stripe_prepayments SET legs=json_set(legs,'$.legs[1].paid',json('true')) WHERE uuid='%s'", uuid);
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
	g_clear_pointer(&answer, json_node_unref); answer = call(f, path, NULL, 200);
	g_assert_null(notification(answer, "payment_failed"));
}
static void test_notification_isk_charge_units(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/lightsite/billing/%" G_GINT64_FORMAT "/notifications", f->business);
	g_autofree gchar *body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", f->business, f->price);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", body, 201), answer = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), published = g_date_time_add_days(now, -29);
	g_autofree gchar *date = venture_time_to_string(published), *uuid = g_uuid_string_random(), *sql = NULL;
	g_autoptr(GError) error = NULL;
	gint64 subscription = json_object_get_int_member(json_node_get_object(enrolled), "subscription_id");
	gint64 company = json_object_get_int_member(json_node_get_object(enrolled), "company_id");
	(void)unused;
	/* 10000 Stripe units of ISK are 100.00 kr, not 10000 kr. */
	sql = g_strdup_printf("INSERT INTO stripe_prepayments (uuid,organization_id,subscription_id,company_id,payment_path,total_amount,total_currency,total_exponent,state,legs,published_at) VALUES ('%s',%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",'installments',20000,'ISK',0,'waiting','{\"legs\":[{\"amount\":10000,\"paid\":true,\"method\":\"example-method\",\"setup\":\"example-setup\"},{\"amount\":10000,\"method\":\"example-method\"}]}','%s')", uuid, f->billing, subscription, company, date);
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
	answer = call(f, path, NULL, 200);
	g_assert_cmpstr(json_object_get_string_member(notification(answer, "installment_due"), "amount"), ==, "100.00 ISK");
}
