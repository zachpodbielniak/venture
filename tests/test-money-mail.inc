/* Quote acceptance queues one confirmation; payment receipts have their own sender. */
static void test_money_confirmations(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "name", "Example Co", "email", "buyer@example.test", NULL);
	g_autoptr(VentureEntity) quote = g_object_new(VENTURE_TYPE_QUOTE, "number", "EXAMPLE-1", "currency", "USD", "billing-mode", "progress", NULL);
	g_autoptr(VentureEntity) line = g_object_new(VENTURE_TYPE_QUOTE_LINE, "description", "Example service", "quantity", (gint64)1, NULL);
	g_autoptr(VentureMoney) amount = venture_money_from_string("100.00 USD", NULL, &error);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(GPtrArray) messages = NULL;
	const gchar *actions[] = {"send", "accept"};
	guint i;
	(void)unused;
	venture_entity_set_organization_id(company, f->org);
	g_assert_true(venture_database_save(f->db, company, NULL, &error)); g_assert_no_error(error);
	venture_entity_set_organization_id(quote, f->org);
	g_object_set(quote, "company-id", venture_entity_get_id(company), NULL);
	g_assert_true(venture_database_save(f->db, quote, NULL, &error)); g_assert_no_error(error);
	venture_entity_set_organization_id(line, f->org);
	g_object_set(line, "quote-id", venture_entity_get_id(quote), "unit-price", amount, NULL);
	g_assert_true(venture_database_save(f->db, line, NULL, &error)); g_assert_no_error(error);
	for (i = 0; i < G_N_ELEMENTS(actions); i++) {
		g_autoptr(VentureEntity) current = venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(quote), &error);
		g_autoptr(VentureEntity) action = g_object_new(VENTURE_TYPE_QUOTE_ACTION,
			"quote-id", venture_entity_get_id(quote), "action", actions[i], "accepted-by", "Example buyer",
			"expected-version", venture_entity_get_version(current), NULL);
		venture_entity_set_organization_id(action, f->org);
		g_assert_true(venture_database_save(f->db, action, NULL, &error)); g_assert_no_error(error);
	}
	venture_query_set_organization(query, f->org);
	messages = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	g_assert_cmpuint(messages->len, ==, 1);
	for (i = 0; i < messages->len; i++) {
		g_autofree gchar *to = NULL, *html = NULL, *key = NULL;
		g_object_get(g_ptr_array_index(messages, i), "to", &to, "html-body", &html, "idempotency-key", &key, NULL);
		g_assert_cmpstr(to, ==, "buyer@example.test"); g_assert_nonnull(html); g_assert_nonnull(key);
	}
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, NULL, NULL, &error), ==, 1); g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, NULL, NULL, &error), ==, 0); g_assert_no_error(error);
}
