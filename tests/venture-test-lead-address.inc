/* The matching key must never replace a delivery address. */
static void
test_address_keys(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) tagged = record(f, "lead", "Tagged");
	g_autoptr(VentureEntity) plain = record(f, "lead", "Plain");
	g_autoptr(VentureEntity) another = record(f, "lead", "Other tag");
	g_autoptr(VentureEntity) duplicate = record(f, "lead", "Duplicate");
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *email = NULL;
	g_autofree gchar *key = venture_lead_normalize_email(" Visitor+Event@EXAMPLE.ORG ");
	(void)data;
	g_assert_cmpstr(key, ==, "visitor+event@example.org");
	g_object_set(tagged, "email", "Visitor+Event@example.org", NULL);
	save(f, tagged);
	stored = venture_database_get(f->db, VENTURE_TYPE_LEAD, venture_entity_get_id(tagged), &error);
	g_assert_no_error(error);
	g_object_get(stored, "email", &email, NULL);
	g_assert_cmpstr(email, ==, "Visitor+Event@example.org");
	g_object_set(plain, "email", "visitor@example.org", NULL);
	g_object_set(another, "email", "visitor+other@example.org", NULL);
	save(f, plain);
	save(f, another);
	g_object_set(duplicate, "email", "visitor+event@EXAMPLE.ORG", NULL);
	g_assert_false(venture_database_save(f->db, duplicate, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
}

static void
test_address_capture_convert(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = record(f, "lead_form", "Address capture");
	g_autoptr(VentureEntity) lead = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LEAD);
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *email = NULL, *path = NULL;
	gint64 contact_id = 0;
	(void)data;
	g_object_set(form, "public-token", "address-test", "active", TRUE, NULL);
	save(f, form);
	start_http(f);
	g_assert_cmpuint(post(f, "/f/address-test", "name=Visitor&email=Visitor%2BEvent%40example.org", FALSE), ==, 200);
	lead = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(lead);
	g_object_get(lead, "email", &email, NULL);
	g_assert_cmpstr(email, ==, "Visitor+Event@example.org");
	g_assert_cmpuint(post(f, "/f/address-test", "name=Visitor&email=visitor%2Bevent%40EXAMPLE.ORG", FALSE), ==, 200);
	g_assert_cmpint(count(f, "lead"), ==, 1);
	g_clear_object(&lead);
	lead = venture_database_find_one(f->db, query, &error);
	g_clear_pointer(&email, g_free);
	g_object_get(lead, "email", &email, NULL);
	g_assert_cmpstr(email, ==, "Visitor+Event@example.org");
	g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL);
	save(f, lead);
	path = g_strdup_printf("/api/v1/leads/%" G_GINT64_FORMAT "/convert", venture_entity_get_id(lead));
	g_assert_cmpuint(post(f, path, "{\"deal\":false}", TRUE), ==, 200);
	g_clear_object(&lead);
	lead = venture_database_find_one(f->db, query, &error);
	g_object_get(lead, "converted-contact-id", &contact_id, NULL);
	contact = venture_database_get(f->db, VENTURE_TYPE_CONTACT, contact_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(contact);
	g_clear_pointer(&email, g_free);
	g_object_get(contact, "email", &email, NULL);
	g_assert_cmpstr(email, ==, "Visitor+Event@example.org");
	{
		g_autofree gchar *uri = g_strdup_printf("http://localhost:%u/e/lead/export", f->port);
		g_autoptr(SoupMessage) request = soup_message_new("GET", uri);
		g_autofree gchar *csv = NULL;
		Reply reply = { FALSE, NULL, NULL };
		soup_session_send_and_read_async(f->session, request, G_PRIORITY_DEFAULT, NULL, received, &reply);
		while (!reply.done) g_main_context_iteration(NULL, TRUE);
		g_assert_no_error(reply.error);
		g_assert_cmpuint(soup_message_get_status(request), ==, 200);
		csv = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));
		g_assert_nonnull(strstr(csv, "Visitor+Event@example.org"));
		g_bytes_unref(reply.body);
	}
	venture_context_set_mailer(f->context, VENTURE_MAILER(mailer));
	g_object_set(message, "organization-id", f->org, "to", email, "subject", "Follow up",
		"text-body", "Thanks for your inquiry.", "idempotency-key", "lead-address-followup", NULL);
	queued = venture_mail_outbox_enqueue(venture_context_get_mail_outbox(f->context), message, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(queued);
	g_assert_cmpint(venture_mail_outbox_deliver_due(venture_context_get_mail_outbox(f->context), f->org, 10, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(mailer)->len, ==, 1);
	g_clear_pointer(&email, g_free);
	g_object_get(g_ptr_array_index((GPtrArray *)venture_log_mailer_get_messages(mailer), 0), "to", &email, NULL);
	g_assert_cmpstr(email, ==, "Visitor+Event@example.org");
}
