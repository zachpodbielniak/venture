/* SPDX-License-Identifier: AGPL-3.0-or-later */

/* Shared with schema discovery so native clients refuse unsupported workflows
 * before collecting answers, rather than learning this only on submission. */
gboolean
venture_forms_relay_supported(VentureEntity *form, GPtrArray *fields)
{
	guint i;
	if (venture_forms_get_bool(form, "double-opt-in") ||
	    (venture_forms_payment(fields) && json_object_get_boolean_member(venture_forms_payment(fields), "enabled"))) return FALSE;
	for (i = 0; i < fields->len; i++) {
		VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->kind == VENTURE_FORM_FIELD_FILE || field->kind == VENTURE_FORM_FIELD_BOOKING) return FALSE;
	}
	return TRUE;
}

JsonNode *
venture_forms_relay(VentureDatabase *database, gint64 organization_id,
	const gchar *scope, const gchar *origin, JsonObject *envelope, GDateTime *now, GError **error)
{
	static const gchar *const names[] = { "version", "tenant_id", "site_id", "submission_id", "form_id", "form_version", "answers", NULL };
	g_autoptr(VentureEntity) form = NULL, version = NULL, response = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(GHashTable) answers = NULL;
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GDateTime) epoch = g_date_time_new_from_unix_utc(0);
	g_autofree gchar *material = NULL, *digest = NULL, *id = NULL, *key = NULL, *mac = NULL, *nonce = NULL, *ticket = NULL;
	JsonObjectIter iter;
	const gchar *name, *submission;
	JsonNode *value, *input;
	gint64 form_id, number;
	VentureFormsOutcome outcome;

	if (organization_id <= 0 || !scope || !origin || !envelope) goto malformed;
	json_object_iter_init(&iter, envelope);
	while (json_object_iter_next(&iter, &name, &value))
		if (!g_strv_contains(names, name)) goto malformed;
	value = json_object_get_member(envelope, "version");
	if (!value || json_node_get_value_type(value) != G_TYPE_INT64 || json_node_get_int(value) != 1) goto malformed;
	value = json_object_get_member(envelope, "form_id");
	if (!value || json_node_get_value_type(value) != G_TYPE_INT64 || (form_id = json_node_get_int(value)) <= 0) goto malformed;
	value = json_object_get_member(envelope, "form_version");
	if (!value || json_node_get_value_type(value) != G_TYPE_INT64 || (number = json_node_get_int(value)) <= 0) goto malformed;
	value = json_object_get_member(envelope, "submission_id");
	if (!value || json_node_get_value_type(value) != G_TYPE_STRING) goto malformed;
	submission = json_node_get_string(value);
	if (!submission || !*submission || strlen(submission) > 128 || !g_utf8_validate(submission, -1, NULL) || strpbrk(submission, "\r\n")) goto malformed;
	input = json_object_get_member(envelope, "answers");
	if (!input || !JSON_NODE_HOLDS_OBJECT(input)) goto malformed;
	/* The relay owns controls. A visitor cannot forge a draft, personal link,
	 * consent seed or payment capability by hiding it among ordinary answers. */
	json_object_iter_init(&iter, json_node_get_object(input));
	while (json_object_iter_next(&iter, &name, &value))
		if (g_str_has_prefix(name, "_vf_")) goto malformed;
	if (!venture_entity_registry_lookup(venture_entity_registry_get_default(), "form")) goto missing;
	form = venture_database_get(database, VENTURE_TYPE_FORM, form_id, error);
	if (!form) return NULL;
	if (venture_entity_get_organization_id(form) != organization_id || venture_entity_is_deleted(form) ||
	    !forms_is_open(database, form, now) || !venture_forms_origin_allowed(form, origin)) goto missing;
	version = venture_forms_version_by_number(database, form, number, error);
	if (!version) { if (error && *error) return NULL; goto missing; }
	fields = venture_forms_definition_for(database, form, version, error);
	if (!fields) return NULL;
	if (!venture_forms_relay_supported(form, fields)) goto unsupported;
	answers = venture_forms_answers_from_json(json_node_get_object(input), error);
	if (!answers) return NULL;
	/* A domain-separated UUID-shaped hash fits the existing receipt identity.
	 * Neither signing-key rotation nor a refreshed delivery timestamp creates
	 * another operation. The form key authenticates the internal nonce only. */
	material = g_strdup_printf("lightsite-form-relay-v1\n%s\n%s", scope, submission);
	key = venture_forms_get_string(form, "ticket-key");
	digest = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), material, -1);
	id = g_strdup_printf("%.8s-%.4s-%.4s-%.4s-%.12s", digest, digest + 8, digest + 12, digest + 16, digest + 20);
	mac = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), id, -1);
	nonce = g_strdup_printf("%s.%s", id, mac);
	ticket = venture_forms_ticket_new_for_version(form, number, epoch);
	venture_forms_answers_add(answers, VENTURE_FORMS_PAYMENT_NONCE, nonce);
	venture_forms_answers_add(answers, VENTURE_FORMS_TICKET, ticket);
	if (!venture_forms_submit(database, form, answers, origin, now, &outcome, &response, &errors, error)) return NULL;
	result = json_from_string("{}", NULL);
	json_object_set_boolean_member(json_node_get_object(result), "accepted", outcome == VENTURE_FORMS_ACCEPTED);
	if (errors != NULL) json_object_set_object_member(json_node_get_object(result), "errors", g_steal_pointer(&errors));
	return g_steal_pointer(&result);
unsupported:
	venture_set_error_validation(error, "form", "Relay supports ordinary complete responses; use hosted forms for payments, bookings, uploads or double opt-in");
	return NULL;
malformed:
	venture_set_error_validation(error, "relay", "Expected the documented form relay envelope and ordinary answers");
	return NULL;
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
	return NULL;
}
