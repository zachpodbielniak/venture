/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Metadata must not let an authorized operator join another legal entity's site data. */
static void test_site_boundaries(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) org = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other site owner", NULL);
	g_autoptr(VentureEntity) form = NULL, site = NULL, campaign = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *mapping = NULL, *first_token = NULL, *second_token = NULL;
	persist(f, org);
	form = g_object_new(VENTURE_TYPE_LEAD_FORM, "organization-id", venture_entity_get_id(org), "name", "Other form", "active", TRUE, NULL); persist(f, form);
	site = venture_database_get(f->db, VENTURE_TYPE_ATTRIBUTION_SITE, venture_entity_get_id(f->site), &error); g_assert_no_error(error);
	g_object_set(site, "lead-form-id", venture_entity_get_id(form), NULL);
	g_assert_false(venture_database_save(f->db, site, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
	campaign = g_object_new(VENTURE_TYPE_CAMPAIGN, "organization-id", venture_entity_get_id(org), "name", "Other campaign", NULL); persist(f, campaign);
	mapping = g_strdup_printf("{\"other\":%" G_GINT64_FORMAT "}", venture_entity_get_id(campaign));
	g_object_set(site, "lead-form-id", venture_entity_get_id(f->form), "campaign-map", mapping, NULL);
	g_assert_false(venture_database_save(f->db, site, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
	g_object_set(site, "campaign-map", "{}", "lookback-days", (gint64)31, "retention-days", (gint64)30, NULL);
	g_assert_false(venture_database_save(f->db, site, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	first_token = grant(f); second_token = grant(f);
	g_assert_cmpstr(first_token, !=, second_token);
	g_assert_false(venture_database_execute(f->db, "UPDATE attribution_visitors SET token_hash = (SELECT token_hash FROM attribution_visitors ORDER BY id LIMIT 1) WHERE id = (SELECT MAX(id) FROM attribution_visitors)", NULL, &error));
	g_assert_nonnull(error);
	g_test_message("Cross-organization form/campaign references and inconsistent retention refused; SQL cannot duplicate a visitor capability hash");
}
/* Receipt-time lookback and session boundaries cannot be replaced by mutable CRM source. */
static void test_lookback(Fixture *f, gconstpointer data)
{
	g_autofree gchar *token = NULL, *first_session = NULL, *last_session = NULL, *source = NULL;
	g_autoptr(JsonObject) fields = page(), payload = NULL;
	g_autoptr(VentureAttributionTouch) first = NULL, second = NULL;
	g_autoptr(VentureAttributionSubmission) captured = NULL;
	g_autoptr(GDateTime) later = g_date_time_add_days(f->now, 2);
	g_autoptr(GError) error = NULL;
	gint64 touch_id;
	g_object_set(f->site, "lookback-days", (gint64)1, NULL); persist(f, f->site);
	token = grant(f);
	first = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "old-touch", fields, f->now, &error); g_assert_no_error(error);
	payload = submission("outside-lookback", token);
	captured = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, later, &error); g_assert_no_error(error);
	g_object_get(captured, "first-touch-id", &touch_id, "first-source", &source, NULL);
	g_assert_cmpint(touch_id, ==, 0); g_assert_true(g_str_has_prefix(source, "form:")); g_clear_pointer(&source, g_free);
	json_object_remove_member(fields, "utm_source"); json_object_remove_member(fields, "referrer");
	second = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "direct-touch", fields, later, &error); g_assert_no_error(error);
	g_object_get(first, "session-key", &first_session, NULL); g_object_get(second, "session-key", &last_session, "source", &source, NULL);
	g_assert_cmpstr(first_session, !=, last_session); g_assert_cmpstr(source, ==, "direct");
	g_clear_object(&captured); json_object_set_string_member(payload, "submission_id", "new-lookback");
	captured = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, later, &error); g_assert_no_error(error);
	g_object_get(captured, "first-touch-id", &touch_id, NULL); g_assert_cmpint(touch_id, ==, venture_entity_get_id(VENTURE_ENTITY(second)));
	g_test_message("Two-day-old touch excluded from one-day lookback; later direct visit receives a new session and freezes its own source");
}
/* A later consented form must never rewrite an earlier untracked inquiry's evidence. */
static void test_repeated_evidence(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(JsonObject) payload = submission("untracked-first", NULL), fields = page(), options = json_object_new();
	g_autoptr(VentureAttributionSubmission) first = NULL, last = NULL;
	g_autoptr(VentureAttributionTouch) touch = NULL;
	g_autoptr(VentureReportResult) report = NULL;
	g_autoptr(VentureDateRange) period = venture_date_range_new(NULL, NULL);
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = NULL, *original = NULL;
	VentureReport *definition = venture_report_registry_lookup(venture_context_get_report_registry(context), "attribution");
	JsonObject *row;
	first = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error); g_assert_no_error(error);
	g_object_get(first, "first-source", &original, NULL);
	g_object_set(f->site, "lookback-days", (gint64)7, NULL); persist(f, f->site);
	token = grant(f); touch = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "later-consented", fields, f->now, &error); g_assert_no_error(error);
	json_object_set_string_member(payload, "submission_id", "tracked-later"); json_object_set_string_member(payload, "visitor_token", token);
	last = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error); g_assert_no_error(error);
	report = venture_report_generate(definition, context, period, options, &error); g_assert_no_error(error); node = venture_report_result_to_json(report);
	row = report_row(node, "leads", original); g_assert_nonnull(row);
	g_assert_nonnull(strstr(json_object_get_string_member(row, "evidence"), "Form provenance only"));
	g_clear_object(&report); g_clear_pointer(&node, json_node_unref); json_object_set_string_member(options, "model", "last");
	report = venture_report_generate(definition, context, period, options, &error); g_assert_no_error(error); node = venture_report_result_to_json(report);
	row = report_row(node, "leads", "newsletter"); g_assert_nonnull(row);
	g_assert_nonnull(strstr(json_object_get_string_member(row, "evidence"), "Consented observations"));
	{
		g_autoptr(GPtrArray) bindings = all_rows(f, VENTURE_TYPE_ATTRIBUTION_BINDING);
		gint64 first_days = 0, last_days = 0;
		g_assert_cmpuint(bindings->len, ==, 1);
		g_object_get(g_ptr_array_index(bindings, 0), "first-lookback-days", &first_days, "last-lookback-days", &last_days, NULL);
		g_assert_cmpint(first_days, ==, 30); g_assert_cmpint(last_days, ==, 7);
	}
	g_test_message("Repeated inquiry: first model retains untracked form evidence; last model uses independently consented newsletter observation");
}
/* Erasing live references is ineffective if exports or audit diffs preserve the same private linkage. */
static void test_private_history(Fixture *f, gconstpointer data)
{
	g_autofree gchar *token = grant(f);
	g_autoptr(JsonObject) fields = page(), payload = submission("private-history", token);
	g_autoptr(VentureAttributionTouch) touch = NULL;
	g_autoptr(VentureAttributionSubmission) captured = NULL;
	g_autoptr(JsonNode) exported = NULL;
	g_autoptr(GPtrArray) audits = NULL;
	g_autoptr(GError) error = NULL;
	guint i, redactions = 0;
	touch = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "private-replay", fields, f->now, &error); g_assert_no_error(error);
	captured = venture_attribution_service_capture(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", payload, f->now, &error); g_assert_no_error(error);
	exported = venture_serializable_to_json(VENTURE_SERIALIZABLE(touch), FALSE);
	g_assert_false(json_object_has_member(json_node_get_object(exported), "visitor_id"));
	g_assert_false(json_object_has_member(json_node_get_object(exported), "event_key"));
	g_assert_false(json_object_has_member(json_node_get_object(exported), "referrer_origin"));
	g_clear_pointer(&exported, json_node_unref); exported = venture_serializable_to_json(VENTURE_SERIALIZABLE(captured), FALSE);
	g_assert_false(json_object_has_member(json_node_get_object(exported), "visitor_id"));
	g_assert_true(venture_attribution_service_withdraw(f->service, venture_entity_get_uuid(f->site), token, f->now, &error)); g_assert_no_error(error);
	audits = all_rows(f, VENTURE_TYPE_AUDIT_ENTRY);
	for (i = 0; i < audits->len; i++) {
		g_autofree gchar *type = NULL, *diff = NULL;
		g_autoptr(JsonNode) changes = NULL;
		JsonObject *object, *link;
		g_object_get(g_ptr_array_index(audits, i), "target-type", &type, "diff", &diff, NULL);
		if ((g_strcmp0(type, "attribution_touch") && g_strcmp0(type, "attribution_submission")) || !diff) continue;
		changes = json_from_string(diff, &error); g_assert_no_error(error); object = json_node_get_object(changes);
		if (!json_object_has_member(object, "visitor_id")) continue;
		link = json_object_get_object_member(object, "visitor_id");
		g_assert_true(json_object_get_boolean_member(link, "redacted"));
		g_assert_false(json_object_has_member(link, "from")); g_assert_false(json_object_has_member(link, "to"));
		g_assert_null(strstr(diff, "search.example.test")); g_assert_null(strstr(diff, "private-replay")); redactions++;
	}
	g_assert_cmpuint(redactions, ==, 2);
	g_test_message("Withdrawal audit records redacted changes only; visitor linkage, replay keys and referrer origins never appear in ordinary exports");
}
typedef struct { Fixture *fixture; const gchar *token; gboolean invoked; gboolean accepted; GError *error; } WithdrawalHook;
static void withdraw_during_observation(VentureDatabase *database, VentureEntity *row, gboolean created, gpointer data)
{
	WithdrawalHook *hook = data;
	if (!VENTURE_IS_ATTRIBUTION_VISITOR(row) || hook->invoked) return;
	hook->invoked = TRUE;
	hook->accepted = venture_attribution_service_withdraw(hook->fixture->service,
		venture_entity_get_uuid(hook->fixture->site), hook->token, hook->fixture->now, &hook->error);
}
/* A nested commit is not durable revocation: an enclosing observation could undo it. */
static void test_nested_withdrawal(Fixture *f, gconstpointer data)
{
	g_autofree gchar *token = grant(f), *path = NULL;
	g_autoptr(JsonObject) fields = page();
	g_autoptr(VentureAttributionTouch) touch = NULL;
	g_autoptr(VentureEntity) retained = NULL;
	g_autoptr(GError) error = NULL;
	WithdrawalHook hook = { f, token, FALSE, FALSE, NULL };
	gulong handler = g_signal_connect(f->db, "entity-saved", G_CALLBACK(withdraw_during_observation), &hook);
	gint64 visitor = 0;
	touch = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "signal-observation", fields, f->now, &error);
	g_signal_handler_disconnect(f->db, handler); g_assert_no_error(error); g_assert_nonnull(touch);
	g_assert_true(hook.invoked); g_assert_false(hook.accepted); g_assert_error(hook.error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&hook.error);
	g_assert_false(venture_database_has_transaction(f->db));
	g_assert_true(venture_attribution_service_withdraw(f->service, venture_entity_get_uuid(f->site), token, f->now, &error)); g_assert_no_error(error);
	retained = venture_database_get(f->db, VENTURE_TYPE_ATTRIBUTION_TOUCH, venture_entity_get_id(VENTURE_ENTITY(touch)), &error); g_assert_no_error(error);
	g_object_get(retained, "visitor-id", &visitor, "path", &path, NULL); g_assert_cmpint(visitor, ==, 0); g_assert_true(!path || !*path);
	g_clear_object(&touch);
	touch = venture_attribution_service_observe(f->service, venture_entity_get_uuid(f->site), "https://site.example.test", token, "after-standalone-withdrawal", fields, f->now, &error);
	g_assert_null(touch); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_test_message("Reentrant signal withdrawal reports BUSY without mutation; standalone retry commits redaction and future observations are refused");
}
