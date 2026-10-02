/* SPDX-License-Identifier: AGPL-3.0-or-later */

static gboolean
forms_resume_save(VentureDatabase *database, VentureEntity *form, VentureEntity *draft,
	VentureFormsStep *step, const gchar *email, GDateTime *now, GError **error)
{
	g_autofree gchar *nonce = NULL, *digest = NULL, *base = NULL, *public = NULL;
	g_autofree gchar *address = NULL;
	g_autoptr(GDateTime) saved = NULL, sent = NULL;
	VentureMailOutbox *outbox = venture_database_get_mail_outbox(database);
	gboolean send = !venture_string_is_empty(email);
	if (send)
	{
		address = g_ascii_strdown(email, -1); g_strstrip(address);
		if (!forms_email_valid(address))
		{
			forms_refuse(step->errors, "_form", "Enter a valid inbox for the resume link, or leave it empty.");
			return TRUE;
		}
		g_object_get(draft, "resume-sent-at", &sent, NULL);
		if (outbox == NULL || G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message") ||
		    venture_forms_get_int(draft, "resume-send-count") >= 3 ||
		    (sent != NULL && g_date_time_difference(now, sent) < 60 * G_TIME_SPAN_SECOND))
		{
			forms_refuse(step->errors, "_form", "The link cannot be emailed now. Leave the inbox empty to save it on screen.");
			return TRUE;
		}
	}
	base = venture_forms_get_string(form, "public-origin");
	if (!venture_forms_public_origin_valid(base, error)) return FALSE;
	public = venture_forms_get_string(form, "public-token");
	nonce = venture_generate_token(32);
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, nonce, -1);
	step->resume_url = g_strdup_printf("%.*s/pub/form/%s/resume/%s",
		(gint)(strlen(base) - (g_str_has_suffix(base, "/") ? 1 : 0)), base, public, nonce);
	g_object_get(draft, "resume-saved-at", &saved, NULL);
	if (saved == NULL)
	{
		gint64 days = venture_forms_get_int(form, "resume-days");
		g_autoptr(GDateTime) expires = g_date_time_add_days(now, (gint)(days > 0 ? days : 7));
		g_object_set(draft, "resume-saved-at", now, "expires-at", expires, NULL);
	}
	g_object_set(draft, "resume-hash", digest, NULL);
	if (send)
		g_object_set(draft, "resume-email", address, "resume-sent-at", now,
			"resume-send-count", venture_forms_get_int(draft, "resume-send-count") + 1, NULL);
	if (!forms_draft_save(database, draft, step, FALSE, error)) return FALSE;
	if (send)
	{
		g_autoptr(VentureMailMessage) message = venture_mail_message_new();
		g_autoptr(VentureMailMessage) queued = NULL;
		g_autoptr(GPtrArray) fields = venture_forms_definition_for(database, form, step->version, error);
		g_autofree gchar *language = venture_forms_language_from_values(form, step->version, step->values);
		g_autofree gchar *body = NULL, *idempotency = NULL;
		if (fields == NULL) return FALSE;
		venture_forms_localize(fields, language);
		body = g_strdup_printf("%s\n%s\n\n%s", venture_forms_text(fields, "message.resume_intro", "Open this private link and press Resume form:"),
			step->resume_url, venture_forms_text(fields, "message.resume_warning", "Anyone with this link can read your unfinished answers. It expires and works only once."));
		idempotency = g_strdup_printf("form-resume:%s:%" G_GINT64_FORMAT, venture_entity_get_uuid(draft), venture_forms_get_int(draft, "resume-send-count"));
		g_object_set(message, "organization-id", venture_entity_get_organization_id(form), "to", address,
			"subject", venture_forms_text(fields, "message.resume_subject", "Resume your form"),
			"text-body", "A private form resume link was issued.", "private-text-body", body,
			"idempotency-key", idempotency, "related-type", "form_draft", "related-id", venture_entity_get_id(draft), NULL);
		queued = venture_mail_outbox_enqueue(outbox, message, NULL, error);
		if (queued == NULL) return FALSE;
	}
	return TRUE;
}

static VentureEntity *
forms_resume_find(VentureDatabase *database, VentureEntity *form, const gchar *capability, GDateTime *now)
{
	g_autofree gchar *digest = NULL, *stored = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) expires = NULL;
	VentureEntity *draft;
	guint i;
	if (capability == NULL || strlen(capability) != 64) return NULL;
	for (i = 0; i < 64; i++) if (!g_ascii_isxdigit(capability[i])) return NULL;
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, capability, -1);
	query = venture_query_new(VENTURE_TYPE_FORM_DRAFT_RECORD);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_string(query, "resume-hash", VENTURE_FILTER_OP_EQ, digest, NULL);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(database, query, NULL);
	if (rows == NULL || rows->len != 1) return NULL;
	draft = g_ptr_array_index(rows, 0);
	stored = venture_forms_get_string(draft, "resume-hash");
	g_object_get(draft, "expires-at", &expires, NULL);
	if (expires == NULL || g_date_time_compare(now, expires) >= 0 || venture_forms_get_bool(draft, "finishing") ||
	    !venture_constant_time_equal(stored, digest)) return NULL;
	return g_object_ref(draft);
}

/* Copy only compatible question identities. Repeated fields are compared after
 * expansion, so removed rows and changed group bounds cannot leak back in. */
static JsonObject *
forms_resume_values(GPtrArray *old_fields, GPtrArray *new_fields, JsonObject *old_values, gboolean changed)
{
	g_autoptr(JsonObject) errors = json_object_new();
	g_autoptr(GPtrArray) before = venture_forms_expand_groups(old_fields, old_values, TRUE, errors);
	g_autoptr(GPtrArray) after = venture_forms_expand_groups(new_fields, old_values, TRUE, errors);
	JsonObject *values = json_object_new();
	guint i;
	for (i = 0; i < after->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(after, i);
		const VentureFormsField *old = venture_forms_definition_find(before, field->key);
		JsonNode *value = json_object_get_member(old_values, field->key);
		if (old == NULL || value == NULL || old->kind != field->kind || old->sensitive != field->sensitive ||
		    g_strcmp0(old->group_key, field->group_key) != 0 || (changed && field->kind == VENTURE_FORM_FIELD_CONSENT)) continue;
		json_object_set_member(values, field->key, json_node_copy(value));
	}
	if (json_object_has_member(old_values, VENTURE_FORMS_PERSONAL))
		json_object_set_member(values, VENTURE_FORMS_PERSONAL, json_node_copy(json_object_get_member(old_values, VENTURE_FORMS_PERSONAL)));
	return values;
}

/**
 * venture_forms_resume:
 * @database: the form database
 * @form: the form owning the capability
 * @capability: a random, single-draft bearer token
 * @now: the expiry-check clock
 * @consume: whether an explicit resume request should rotate the capability
 * @error: return location for an error
 *
 * A non-consuming lookup returns no answers. A consuming lookup upgrades the
 * private working copy to the latest published definition under the save lock.
 *
 * Returns: (transfer full) (nullable): the next render state
 */
VentureFormsStep *
venture_forms_resume(VentureDatabase *database, VentureEntity *form, const gchar *capability,
	GDateTime *now, gboolean consume, GError **error)
{
	g_autoptr(VentureFormsStep) step = g_new0(VentureFormsStep, 1);
	g_autoptr(VentureEntity) fresh = NULL, draft = NULL, previous = NULL;
	g_autoptr(GPtrArray) old_fields = NULL, fields = NULL, expanded = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonObject) seed = NULL;
	g_autoptr(GHashTable) answers = NULL;
	g_autoptr(GHashTable) hidden = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autofree gchar *text = NULL, *old_ticket = NULL, *language = NULL, *packed = NULL;
	gint64 issued = 0, number = 0;
	guint i, reached;
	if (!venture_database_begin(database, error)) return NULL;
	fresh = venture_database_get(database, VENTURE_TYPE_FORM, venture_entity_get_id(form), error);
	if (fresh == NULL) goto fail;
	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(), "form") ||
	    !forms_is_open(database, fresh, now) || !venture_forms_get_bool(fresh, "allow-resume")) goto missing;
	draft = forms_resume_find(database, fresh, capability, now);
	if (draft == NULL) goto missing;
	step->version = venture_forms_published_version(database, fresh, error);
	if (step->version == NULL) goto fail;
	step->errors = json_object_new();
	step->values = json_object_new();
	/* Email scanners may inspect the landing page, but cannot read the answers
	 * or rotate a capability. Only the explicit POST crosses that boundary. */
	if (!consume)
	{
		if (!venture_database_commit(database, error)) return NULL;
		return g_steal_pointer(&step);
	}
	previous = venture_database_get(database, VENTURE_TYPE_FORM_VERSION, venture_forms_get_int(draft, "version-id"), error);
	if (previous == NULL) goto fail;
	old_fields = venture_forms_definition_for(database, fresh, previous, error);
	fields = venture_forms_definition_for(database, fresh, step->version, error);
	if (old_fields == NULL || fields == NULL) goto fail;
	text = venture_forms_get_string(draft, "answers");
	if (!json_parser_load_from_data(parser, text, -1, error)) goto fail;
	if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) goto missing;
	step->changed = venture_entity_get_id(previous) != venture_entity_get_id(step->version);
	g_clear_pointer(&step->values, json_object_unref);
	step->values = forms_resume_values(old_fields, fields, json_node_get_object(json_parser_get_root(parser)), step->changed);
	language = venture_forms_language_from_values(fresh, previous, json_node_get_object(json_parser_get_root(parser)));
	venture_forms_localize(fields, language);
	seed = json_object_new();
	json_object_set_string_member(seed, VENTURE_FORMS_LANGUAGE, venture_forms_language(fields));
	packed = venture_forms_prefill_pack(fresh, step->version, seed);
	json_object_set_string_member(step->values, VENTURE_FORMS_PREFILL, packed);
	if (json_object_has_member(step->values, VENTURE_FORMS_PERSONAL))
	{
		g_autoptr(VentureEntity) contact = venture_forms_personal_contact(database, fresh,
			json_object_get_string_member(step->values, VENTURE_FORMS_PERSONAL), now, error);
		if (contact == NULL) goto fail;
		step->contact_id = venture_entity_get_id(contact);
	}
	expanded = venture_forms_expand_groups(fields, step->values, TRUE, step->errors);
	venture_forms_rules_filter(expanded, step->values, hidden);
	answers = venture_forms_answers_from_json(step->values, error);
	if (answers == NULL) goto fail;
	step->pages = venture_forms_page_count(fields);
	reached = MIN((guint)venture_forms_get_int(draft, "page"), step->pages - 1);
	step->page = 0;
	/* Revisit the first newly-required or now-invalid reachable page. Values
	 * remain editable; a changed choice list must never silently accept them. */
	while (step->page < reached)
	{
		g_autoptr(GPtrArray) page = venture_forms_page_fields(expanded, step->page);
		g_autoptr(JsonObject) errors = json_object_new(), checked = json_object_new();
		g_autoptr(GString) summary = g_string_new(NULL);
		guint next;
		venture_forms_groups_validate(page, hidden, errors);
		for (i = 0; i < page->len; i++)
		{
			VentureFormsField *field = g_ptr_array_index(page, i);
			VentureFormsField effective = *field;
			if (g_hash_table_contains(hidden, field->key)) continue;
			venture_forms_field_active(field, step->values, &effective.required);
			forms_check_field(&effective, g_hash_table_lookup(answers, field->key), checked, errors, summary);
		}
		if (json_object_get_size(errors) > 0) break;
		next = venture_forms_next_page(expanded, step->page, step->values);
		if (next >= step->pages) break;
		step->page = next;
	}
	old_ticket = venture_forms_get_string(draft, "ticket");
	if (!venture_forms_ticket_parse(fresh, old_ticket, &issued, &number)) goto missing;
	{
		g_autoptr(GDateTime) started = g_date_time_new_from_unix_utc(issued);
		/* Ticket versions are per-form numbers, not global record ids. */
		step->ticket = venture_forms_ticket_new_for_version(fresh, venture_forms_get_int(step->version, "number"), started);
	}
	/* The resumed page receives a replacement link without extending expiry
	 * or emailing it again. Only its digest remains in the working copy. */
	g_object_set(draft, "version-id", venture_entity_get_id(step->version), "ticket", step->ticket, NULL);
	if (!forms_resume_save(database, fresh, draft, step, NULL, now, error)) goto fail;
	step->token = forms_draft_token(fresh, draft);
	if (!venture_database_commit(database, error)) return NULL;
	return g_steal_pointer(&step);
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
fail:
	venture_database_rollback(database);
	return NULL;
}
