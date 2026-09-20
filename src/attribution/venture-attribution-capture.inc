/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean update_binding(VentureAttributionService *self, VentureEntity *submission,
	VentureEntity *subject, GDateTime *now, GError **error);
static const gchar *optional_string(JsonObject *object, const gchar *name, GError **error)
{
	JsonNode *node = json_object_get_member(object, name);
	if (!node) return "";
	if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) {
		refuse(error, "Capture string has the wrong JSON type"); return NULL;
	}
	return json_node_get_string(node);
}
static gboolean envelope_valid(VentureEntity *site, JsonObject *payload, GError **error)
{
	static const gchar *const allowed[] = { "version", "submission_id", "visitor_token", "fields", "marketing", "honeypot", "tenant_id", "site_id", "origin", NULL };
	g_autoptr(GList) keys = payload ? json_object_get_members(payload) : NULL;
	GList *item;
	JsonNode *version, *fields;
	const gchar *external_names[] = { "tenant_id", "site_id", "origin" };
	const gchar *properties[] = { "external-tenant-id", "external-site-id", "origin" };
	guint i;
	if (!payload || json_object_get_size(payload) > 9) return refuse(error, "Expected version-one capture envelope");
	for (item = keys; item; item = item->next)
		if (!g_strv_contains(allowed, item->data)) return refuse(error, "Unknown capture envelope field");
	version = json_object_get_member(payload, "version"); fields = json_object_get_member(payload, "fields");
	if (!version || !JSON_NODE_HOLDS_VALUE(version) || json_node_get_value_type(version) != G_TYPE_INT64 || json_node_get_int(version) != 1 ||
		!fields || !JSON_NODE_HOLDS_OBJECT(fields)) return refuse(error, "Capture requires version 1 and mapped form fields");
	for (i = 0; i < G_N_ELEMENTS(external_names); i++) {
		const gchar *value = optional_string(payload, external_names[i], error);
		g_autofree gchar *expected = string(site, properties[i]);
		if (!value) return FALSE;
		if (json_object_has_member(payload, external_names[i]) && g_strcmp0(value, expected))
			return refuse(error, "Capture envelope does not match the configured tenant, site and origin");
	}
	return TRUE;
}
static JsonObject *mapped_fields(JsonObject *input, GError **error)
{
	static const gchar *const names[] = { "name", "email", "message", "company_name", "phone", "website", NULL };
	g_autoptr(JsonObject) output = json_object_new();
	JsonObjectIter iter;
	JsonNode *node;
	const gchar *name;
	if (json_object_get_size(input) > 6) { refuse(error, "Too many form fields"); return NULL; }
	json_object_iter_init(&iter, input);
	while (json_object_iter_next(&iter, &name, &node)) {
		const gchar *text;
		gsize maximum = !g_strcmp0(name, "message") ? 8192 : !g_strcmp0(name, "website") ? 2048 : 254;
		if (!g_strv_contains(names, name) || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) {
			refuse(error, "Form fields must follow the documented string mapping"); return NULL;
		}
		text = json_node_get_string(node);
		if (!text || strlen(text) > maximum || !g_utf8_validate(text, -1, NULL) ||
			(strcmp(name, "message") && strpbrk(text, "\r\n"))) { refuse(error, "Form field exceeds its text bounds"); return NULL; }
		json_object_set_string_member(output, !strcmp(name, "message") ? "notes" : name, text);
	}
	if (!bounded(venture_json_object_get_string(output, "name", ""), 254) || !bounded(venture_json_object_get_string(output, "email", ""), 254)) {
		refuse(error, "Site contact forms require name and email"); return NULL;
	}
	return g_steal_pointer(&output);
}
static gboolean snapshot_touches(VentureAttributionService *self, VentureEntity *site, VentureEntity *visitor,
	VentureEntity *submission, GDateTime *now, GError **error)
{
	g_autofree gchar *fallback = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) from = NULL;
	g_autofree gchar *start = NULL, *end = NULL;
	guint i;
	gint64 days = number(site, "lookback-days");
	fallback = g_strdup_printf("form:%s", venture_entity_get_uuid(site));
	g_object_set(submission, "first-source", fallback, "last-source", fallback, "lookback-days", days,
		"first-evidence", visitor ? "No observed touch in the configured lookback" : "Form provenance only; no anonymous analytics permission",
		"last-evidence", visitor ? "No observed touch in the configured lookback" : "Form provenance only; no anonymous analytics permission", NULL);
	if (!visitor) return TRUE;
	query = venture_query_new(VENTURE_TYPE_ATTRIBUTION_TOUCH);
	venture_query_set_organization(query, venture_entity_get_organization_id(site)); venture_query_set_limit(query, 1001);
	venture_query_add_filter_int(query, "visitor-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(visitor), NULL);
	from = g_date_time_add_days(now, -days); start = venture_time_to_string(from); end = venture_time_to_string(now);
	venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_GTE, start, NULL);
	venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_LTE, end, NULL);
	venture_query_add_order(query, "occurred-at", VENTURE_SORT_ASCENDING, NULL); venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error); if (!rows) return FALSE;
	if (rows->len > 1000) return refuse(error, "Visitor touch history exceeds its configured bounds");
	if (!rows->len) return TRUE;
	for (i = 0; i < 2; i++) {
		VentureEntity *touch = g_ptr_array_index(rows, i ? rows->len - 1 : 0);
		g_autofree gchar *source = string(touch, "source");
		g_object_set(submission, i ? "last-touch-id" : "first-touch-id", venture_entity_get_id(touch),
			i ? "last-source" : "first-source", source, i ? "last-campaign-id" : "first-campaign-id", number(touch, "campaign-id"), NULL);
	}
	g_object_set(submission, "first-evidence", "Consented observations within the frozen lookback",
		"last-evidence", "Consented observations within the frozen lookback", NULL);
	return TRUE;
}
static gboolean validate_marketing(VentureEntity *site, JsonObject *payload, GDateTime *now, GError **error)
{
	JsonNode *node = json_object_get_member(payload, "marketing"), *choice;
	JsonObject *permission;
	g_autofree gchar *policy = NULL, *statement = NULL;
	g_autoptr(GDateTime) occurred = NULL;
	const gchar *claimed_policy, *claimed_statement, *time;
	if (!node) return TRUE;
	if (!JSON_NODE_HOLDS_OBJECT(node)) return refuse(error, "Email permission must be a separate explicit object");
	permission = json_node_get_object(node); choice = json_object_get_member(permission, "granted");
	if (!choice || !JSON_NODE_HOLDS_VALUE(choice) || json_node_get_value_type(choice) != G_TYPE_BOOLEAN)
		return refuse(error, "Email permission requires an explicit boolean choice");
	if (!json_node_get_boolean(choice)) return json_object_get_size(permission) == 1 || refuse(error, "A declined email permission carries no additional evidence");
	if (json_object_get_size(permission) != 4) return refuse(error, "Email permission requires policy, statement and actual occurrence time");
	claimed_policy = optional_string(permission, "policy", error); claimed_statement = optional_string(permission, "statement", error);
	time = optional_string(permission, "occurred_at", error);
	if (!claimed_policy || !claimed_statement || !time) return FALSE;
	policy = string(site, "marketing-policy"); statement = string(site, "marketing-statement");
	if (!bounded(policy, 128) || venture_string_is_empty(statement) || strlen(statement) > 4096 ||
		g_strcmp0(policy, claimed_policy) || g_strcmp0(statement, claimed_statement))
		return refuse(error, "Email permission does not match this site's approved statement and policy");
	occurred = g_date_time_new_from_iso8601(time, NULL);
	return (occurred && g_date_time_get_year(occurred) >= 1970 && g_date_time_compare(occurred, now) <= 0) ||
		refuse(error, "Email permission occurrence must not be later than receipt");
}
static gboolean capture_marketing(VentureAttributionService *self, VentureEntity *site, VentureEntity *subject,
	VentureEntity *submission, JsonObject *payload, GError **error)
{
	JsonNode *node = json_object_get_member(payload, "marketing");
	JsonObject *permission;
	g_autofree gchar *policy = NULL, *statement = NULL, *key = NULL, *source = NULL;
	g_autofree gchar *subject_email = NULL, *email = NULL, *claimed = NULL;
	g_autoptr(VentureEntity) suppression = NULL;
	g_autoptr(VentureMarketingConsent) consent = NULL;
	g_autoptr(GDateTime) occurred = NULL;
	g_object_set(submission, "marketing-status", "not_requested", NULL);
	if (!node) return TRUE;
	permission = json_node_get_object(node);
	if (!json_object_get_boolean_member(permission, "granted")) {
		g_object_set(submission, "marketing-status", "declined", NULL); return TRUE;
	}
	policy = string(site, "marketing-policy"); statement = string(site, "marketing-statement");
	occurred = g_date_time_new_from_iso8601(json_object_get_string_member(permission, "occurred_at"), NULL);
	g_object_set(submission, "marketing-policy", policy, "marketing-statement", statement, "marketing-requested-at", occurred, NULL);
	if (!venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_consent")) {
		g_object_set(submission, "marketing-status", "module_unavailable", NULL); return TRUE;
	}
	subject_email = string(subject, "email"); email = g_ascii_strdown(subject_email ? subject_email : "", -1); g_strstrip(email);
	claimed = g_ascii_strdown(json_object_get_string_member(json_object_get_object_member(payload, "fields"), "email"), -1); g_strstrip(claimed);
	/* Phone/website matching may return a record with a different mailbox.
	 * The form's choice authorizes only the address it actually submitted. */
	if (g_strcmp0(email, claimed)) {
		g_object_set(submission, "marketing-status", "address_mismatch", NULL); return TRUE;
	}
	suppression = find_string(self, VENTURE_TYPE_SUPPRESSION, venture_entity_get_organization_id(site), "email", email, error);
	if (error && *error) return FALSE;
	if (suppression) {
		g_object_set(submission, "marketing-status", "suppressed", NULL); return TRUE;
	}
	key = g_strdup_printf("form:%s", venture_entity_get_uuid(submission));
	source = g_strdup_printf("Site %s form choice; email policy %s", venture_entity_get_uuid(site), policy);
	consent = venture_marketing_service_consent(venture_marketing_service_get(self->database),
		venture_entity_get_organization_id(site), subject, source, statement, key, occurred, NULL, error);
	if (!consent) return FALSE;
	g_object_set(submission, "marketing-status", "recorded", "marketing-consent-id", venture_entity_get_id(VENTURE_ENTITY(consent)), NULL); return TRUE;
}
static VentureEntity *capture_payload(VentureAttributionService *self, VentureEntity *site,
	VentureEntity *connection, gint64 connection_version, JsonObject *payload, GDateTime *now, GError **error)
{
	g_autoptr(VentureEntity) existing = NULL, visitor = NULL, submission = NULL, form = NULL, subject = NULL, current = NULL;
	g_autoptr(JsonObject) fields = NULL;
	g_autoptr(GDateTime) minute = g_date_time_add_seconds(now, -60), day = g_date_time_add_days(now, -1);
	g_autofree gchar *key = NULL, *hash = NULL, *token = NULL, *source = NULL;
	const gchar *id, *visitor_token, *honeypot, *field;
	gint64 org = venture_entity_get_organization_id(site);
	if (!envelope_valid(site, payload, error)) return NULL;
	id = optional_string(payload, "submission_id", error); visitor_token = optional_string(payload, "visitor_token", error);
	honeypot = optional_string(payload, "honeypot", error);
	if (!id || !visitor_token || !honeypot) return NULL;
	if (!identifier(id) || strlen(visitor_token) > 64 || strlen(honeypot) > 1024) { refuse(error, "Invalid bounded submission identity or capability"); return NULL; }
	fields = mapped_fields(json_object_get_object_member(payload, "fields"), error); if (!fields) return NULL;
	if (!validate_marketing(site, payload, now, error)) return NULL;
	if (*honeypot) return NULL;
	key = g_strdup_printf("%s:%s:%s", venture_entity_get_uuid(site), connection ? venture_entity_get_uuid(connection) : "browser", id);
	hash = digest(payload);
	existing = find_string(self, VENTURE_TYPE_ATTRIBUTION_SUBMISSION, org, "submission-key", key, error);
	if (error && *error) return NULL;
	if (existing) {
		g_autofree gchar *old = string(existing, "payload-hash");
		if (g_strcmp0(old, hash)) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Submission identity already names different content"); return NULL; }
		return g_steal_pointer(&existing);
	}
	if (!quota(self, site, VENTURE_TYPE_ATTRIBUTION_SUBMISSION, "submitted-at", minute, 60, error) ||
		!quota(self, site, VENTURE_TYPE_ATTRIBUTION_SUBMISSION, "submitted-at", day, 10000, error)) return NULL;
	if (*visitor_token) {
		visitor = visitor_get(self, site, visitor_token, now, TRUE, error); if (!visitor) return NULL;
		if (number(visitor, "submission-count") >= 1000) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Visitor form lifetime limit reached"); return NULL; }
	}
	submission = g_object_new(VENTURE_TYPE_ATTRIBUTION_SUBMISSION, "organization-id", org, "site-id", venture_entity_get_id(site),
		"visitor-id", visitor ? venture_entity_get_id(visitor) : (gint64)0, "submitted-at", now, "submission-key", key,
		"payload-hash", hash, "connection-id", connection ? venture_entity_get_id(connection) : (gint64)0, "connection-version", connection_version, NULL);
	if (!snapshot_touches(self, site, visitor, submission, now, error)) return NULL;
	form = get_row(self, VENTURE_TYPE_LEAD_FORM, org, number(site, "lead-form-id"), error); if (!form) return NULL;
	token = string(form, "public-token"); source = string(submission, "first-source");
	if (!venture_lead_service_capture_result(venture_database_get_lead_service(self->database), token, fields, source,
		number(submission, "first-campaign-id"), &subject, NULL, error) || !subject) return NULL;
	/* Capture history emits synchronous automation. Re-read the selected
	 * subject before interpreting an address-specific permission choice. */
	current = get_row(self, G_OBJECT_TYPE(subject), org, venture_entity_get_id(subject), error);
	if (!current) return NULL;
	g_clear_object(&subject); subject = g_steal_pointer(&current);
	field = VENTURE_IS_LEAD(subject) ? "lead-id" : VENTURE_IS_CONTACT(subject) ? "contact-id" : "company-id";
	g_object_set(submission, field, venture_entity_get_id(subject), NULL);
	if (VENTURE_IS_CONTACT(subject)) g_object_set(submission, "company-id", number(subject, "company-id"), NULL);
	if (!capture_marketing(self, site, subject, submission, payload, error)) return NULL;
	if (!save(self, submission, NULL, error) || !update_binding(self, submission, subject, now, error)) return NULL;
	if (visitor) {
		g_object_set(visitor, "submission-count", number(visitor, "submission-count") + 1, NULL);
		if (!save(self, visitor, NULL, error)) return NULL;
	}
	/* Synchronous automation may edit configuration while capture writes.
	 * Commit only the exact configuration that authorized this transaction. */
	current = get_row(self, VENTURE_TYPE_ATTRIBUTION_SITE, org, venture_entity_get_id(site), error);
	if (!current) return NULL;
	if (venture_entity_get_version(current) != venture_entity_get_version(site)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Site changed during capture; retry against its current configuration"); return NULL;
	}
	return g_steal_pointer(&submission);
}
VentureAttributionSubmission *venture_attribution_service_capture(VentureAttributionService *self,
	const gchar *site_uuid, const gchar *origin, JsonObject *payload, GDateTime *now, GError **error)
{
	g_autoptr(VentureAccessScope) capability = NULL;
	g_autoptr(VentureEntity) site = NULL, result = NULL;
	g_autoptr(GError) local = NULL;
	g_return_val_if_fail(VENTURE_IS_ATTRIBUTION_SERVICE(self), NULL);
	if (!self->database || !origin || !clock_valid(now, error)) return NULL;
	capability = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return NULL;
	site = site_get(self, site_uuid, origin, TRUE, &local);
	if (site) result = capture_payload(self, site, NULL, 0, payload, now, &local);
	if (local || !site) {
		venture_database_rollback(self->database);
		if (local) g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	if (!venture_database_commit(self->database, error)) return NULL;
	return VENTURE_ATTRIBUTION_SUBMISSION(g_steal_pointer(&result));
}
