/* SPDX-License-Identifier: AGPL-3.0-or-later */

static gboolean
forms_validate_pending(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	(void)database; (void)previous; (void)data;
	if (g_object_get_data(G_OBJECT(entity), VENTURE_FORMS_PENDING_WRITE) == NULL)
	{
		venture_set_error_validation(error, "Unconfirmed signup", "only the forms service may write signups");
		return FALSE;
	}
	return TRUE;
}

static gboolean
forms_pending_save(VentureDatabase *database, VentureEntity *pending, GError **error)
{
	g_object_set_data(G_OBJECT(pending), VENTURE_FORMS_PENDING_WRITE, GINT_TO_POINTER(1));
	g_object_set(pending, "generation", venture_forms_get_int(pending, "generation") + 1, NULL);
	if (!venture_database_save(database, pending, NULL, error)) return FALSE;
	{
		g_autoptr(VentureEntity) form = venture_database_get(database, VENTURE_TYPE_FORM, venture_forms_get_int(pending, "form-id"), error);
		g_autoptr(GDateTime) expires = NULL;
		g_autofree gchar *text = venture_forms_get_string(pending, "answers");
		g_autoptr(JsonNode) values = text != NULL ? json_from_string(text, NULL) : NULL;
		if (form == NULL) return FALSE;
		g_object_get(pending, "expires-at", &expires, NULL);
		return values == NULL || !JSON_NODE_HOLDS_OBJECT(values) || venture_forms_upload_bind_source(database, form, pending, json_node_get_object(values), expires, error);
	}
}

static gboolean
forms_optin_list_valid(VentureDatabase *database, VentureEntity *form, gint64 list_id, GError **error)
{
	if (list_id > 0)
	{
		g_autoptr(VentureEntity) list = venture_database_get(database, VENTURE_TYPE_MARKETING_LIST,
			list_id, error);
		gint mode = 0;
		if (list == NULL) return FALSE;
		g_object_get(list, "mode", &mode, NULL);
		if (venture_entity_is_deleted(list) || mode != 0 || venture_entity_get_organization_id(list) != venture_entity_get_organization_id(form))
		{
			venture_set_error_validation(error, "Signup list", "must be a static list in this organization");
			return FALSE;
		}
	}
	return TRUE;
}

static gboolean
forms_optin_definition(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error)
{
	g_autofree gchar *key = venture_forms_get_string(form, "optin-email-field");
	g_autofree gchar *origin = venture_forms_get_string(form, "public-origin");
	const VentureFormsField *email;
	gboolean consent = FALSE;
	guint i;
	if (!venture_forms_get_bool(form, "double-opt-in")) return TRUE;
	if (venture_forms_payment(fields) != NULL && json_object_get_boolean_member(venture_forms_payment(fields), "enabled"))
	{
		venture_set_error_validation(error, "Signup", "paid intake and double opt-in signup use separate forms");
		return FALSE;
	}
	email = venture_forms_definition_find(fields, key);
	if (email == NULL || email->kind != VENTURE_FORM_FIELD_EMAIL || !email->required ||
	    email->sensitive || email->group_key != NULL)
	{
		venture_set_error_validation(error, "Signup email question", "must name a required, non-sensitive email outside repeating groups");
		return FALSE;
	}
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->marketing_consent && field->required && !field->sensitive) consent = TRUE;
	}
	if (!consent)
	{
		venture_set_error_validation(error, "Signup consent", "requires an explicit required marketing permission question");
		return FALSE;
	}
	if (!venture_forms_public_origin_valid(origin, error)) return FALSE;
	if (!forms_optin_list_valid(database, form, venture_forms_get_int(form, "optin-list-id"), error)) return FALSE;
	return TRUE;
}

/* The link is random as well as signed. Only its digest is retained; rotation
 * invalidates the old link without retaining another bearer credential. */
static gchar *
forms_optin_token(VentureEntity *form, GDateTime *expires)
{
	g_autofree gchar *nonce = venture_generate_token(32);
	g_autofree gchar *key = venture_forms_get_string(form, "ticket-key");
	g_autofree gchar *public = venture_forms_get_string(form, "public-token");
	g_autofree gchar *payload = g_strdup_printf("optin:%s:%s:%" G_GINT64_FORMAT,
		public, nonce, g_date_time_to_unix(expires));
	g_autofree gchar *mac = g_compute_hmac_for_string(G_CHECKSUM_SHA256,
		(const guchar *)key, strlen(key), payload, -1);
	return g_strdup_printf("%s.%" G_GINT64_FORMAT ".%s", nonce, g_date_time_to_unix(expires), mac);
}

static VentureEntity *
forms_optin_find(VentureDatabase *database, VentureEntity *form, const gchar *token, GDateTime *now)
{
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *key = NULL, *public = NULL, *payload = NULL, *mac = NULL, *digest = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) expires = NULL;
	VentureEntity *pending;
	gint64 stamp;
	gchar *end = NULL;
	guint i;
	if (token == NULL || strlen(token) > 160) return NULL;
	parts = g_strsplit(token, ".", 4);
	if (g_strv_length(parts) != 3 || strlen(parts[0]) != 64 || strlen(parts[2]) != 64) return NULL;
	for (i = 0; i < 64; i++) if (!g_ascii_isxdigit(parts[0][i]) || !g_ascii_isxdigit(parts[2][i])) return NULL;
	stamp = g_ascii_strtoll(parts[1], &end, 10);
	if (end == parts[1] || *end != '\0' || stamp <= g_date_time_to_unix(now)) return NULL;
	key = venture_forms_get_string(form, "ticket-key");
	if (venture_string_is_empty(key)) return NULL;
	public = venture_forms_get_string(form, "public-token");
	payload = g_strdup_printf("optin:%s:%s:%" G_GINT64_FORMAT, public, parts[0], stamp);
	mac = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), payload, -1);
	if (!venture_constant_time_equal(mac, parts[2])) return NULL;
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	query = venture_query_new(VENTURE_TYPE_FORM_PENDING);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_string(query, "token-hash", VENTURE_FILTER_OP_EQ, digest, NULL);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(database, query, NULL);
	if (rows == NULL || rows->len != 1) return NULL;
	pending = g_ptr_array_index(rows, 0);
	g_object_get(pending, "expires-at", &expires, NULL);
	if (expires == NULL || g_date_time_to_unix(expires) != stamp || venture_forms_get_bool(pending, "confirming")) return NULL;
	return g_object_ref(pending);
}

/* A pending capability must not arrive after its working copy was erased.
 * Already-sent mail belongs to the outbox's separate retention contract. */
static gboolean
forms_working_copy_purge(VentureDatabase *database, VentureEntity *pending, const gchar *type, const VentureActor *actor, gint64 *cancelled, GError **error)
{
	VentureMailOutbox *outbox = venture_database_get_mail_outbox(database);
	if (outbox != NULL)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		g_autoptr(GPtrArray) messages = NULL;
		guint i;
		venture_query_set_organization(query, venture_entity_get_organization_id(pending));
		venture_query_add_filter_string(query, "related-type", VENTURE_FILTER_OP_EQ, type, NULL);
		venture_query_add_filter_int(query, "related-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(pending), NULL);
		venture_query_set_limit(query, 3);
		messages = venture_database_find(database, query, error);
		if (messages == NULL) return FALSE;
		for (i = 0; i < messages->len; i++)
		{
			VentureEntity *message = g_ptr_array_index(messages, i);
			g_autofree gchar *state = venture_forms_get_string(message, "state");
			/* A refused nested cancellation rolls back its parent too.
			 * Sent/leased mail cannot be recalled; never attempt that write. */
			if (g_strcmp0(state, "queued") != 0 && g_strcmp0(state, "failed") != 0) continue;
			if (!venture_mail_outbox_cancel(outbox, venture_entity_get_organization_id(pending),
				venture_entity_get_id(message), "private form copy ended", actor, error)) return FALSE;
			if (cancelled != NULL) (*cancelled)++;
		}
	}
	return venture_forms_upload_purge_source(database, pending, error) && venture_database_purge(database, pending, actor, error);
}

static gboolean
forms_pending_purge(VentureDatabase *database, VentureEntity *pending, const VentureActor *actor, gint64 *cancelled, GError **error)
{
	return forms_working_copy_purge(database, pending, "form_pending", actor, cancelled, error);
}

static gboolean
forms_optin_suppressed(VentureDatabase *database, VentureEntity *form, const gchar *email,
	gboolean *blocked, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SUPPRESSION);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_EQ, email, NULL);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return FALSE;
	*blocked = rows->len > 0;
	return TRUE;
}

static gboolean
forms_optin_store(VentureDatabase *database, VentureEntity *form, VentureEntity *version,
	GHashTable *answers, GPtrArray *fields, JsonObject *checked, const gchar *origin,
	GDateTime *now, JsonObject *refused, GError **error)
{
	g_autofree gchar *email_key = venture_forms_get_string(form, "optin-email-field");
	g_autofree gchar *email = NULL, *token = NULL, *digest = NULL, *url = NULL, *body = NULL, *idempotency = NULL;
	g_autofree gchar *base = venture_forms_get_string(form, "public-origin");
	g_autofree gchar *public = venture_forms_get_string(form, "public-token");
	g_autoptr(VentureEntity) pending = NULL, contact = NULL, fresh = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL, mail_fields = NULL;
	g_autoptr(GDateTime) expires = NULL, sent = NULL;
	g_autoptr(VentureMailMessage) message = NULL, queued = NULL;
	VentureMailOutbox *outbox = venture_database_get_mail_outbox(database);
	gboolean given = FALSE, blocked = FALSE;
	guint i;
	if (!forms_optin_definition(database, form, fields, error)) return FALSE;
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		JsonNode *answer = json_object_get_member(checked, field->key);
		if (field->marketing_consent && answer != NULL && JSON_NODE_HOLDS_OBJECT(answer) &&
		    json_object_get_boolean_member_with_default(json_node_get_object(answer), "given", FALSE)) given = TRUE;
	}
	if (!given)
	{
		forms_refuse(refused, email_key, "Explicit marketing permission is required to confirm this signup.");
		return TRUE;
	}
	email = g_ascii_strdown(json_object_get_string_member_with_default(checked, email_key, ""), -1);
	g_strstrip(email);
	if (!forms_email_valid(email))
	{
		forms_refuse(refused, email_key, "A signup email address is required.");
		return TRUE;
	}
	if (forms_first(answers, VENTURE_FORMS_PERSONAL) != NULL)
	{
		g_autofree gchar *bound_email = NULL;
		contact = venture_forms_personal_contact(database, form, forms_first(answers, VENTURE_FORMS_PERSONAL), now, error);
		if (contact == NULL) return FALSE;
		bound_email = venture_forms_get_string(contact, "email");
		if (bound_email == NULL || g_ascii_strcasecmp(g_strstrip(bound_email), email) != 0)
		{
			forms_refuse(refused, email_key, "Use the inbox addressed by this personal signup link.");
			return TRUE;
		}
	}
	if (outbox == NULL || G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message") ||
	    G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_consent") ||
	    G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(), "contact"))
	{
		venture_set_error_validation(error, "Signup", "mail, marketing and contacts must be enabled");
		return FALSE;
	}
	if (!venture_database_begin(database, error)) return FALSE;
	fresh = venture_database_get(database, VENTURE_TYPE_FORM, venture_entity_get_id(form), error);
	if (fresh == NULL || !forms_is_open(database, fresh, now)) goto missing;
	if (!forms_optin_suppressed(database, form, email, &blocked, error)) goto fail;
	if (blocked) return venture_database_commit(database, error);
	query = venture_query_new(VENTURE_TYPE_FORM_PENDING);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_EQ, email, NULL);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(database, query, error);
	if (rows == NULL) goto fail;
	if (rows->len > 0)
	{
		pending = g_object_ref(g_ptr_array_index(rows, 0));
		g_object_get(pending, "expires-at", &expires, "last-sent-at", &sent, NULL);
		if (expires == NULL || g_date_time_compare(now, expires) >= 0 || venture_entity_get_deleted_at(pending) != NULL)
		{
			if (!forms_pending_purge(database, pending, NULL, NULL, error)) goto fail;
			g_clear_object(&pending); g_clear_pointer(&expires, g_date_time_unref);
		}
		else if (venture_forms_get_bool(pending, "confirming") || venture_forms_get_int(pending, "send-count") >= 3 ||
		         (sent != NULL && g_date_time_difference(now, sent) < 60 * G_TIME_SPAN_SECOND))
			return venture_database_commit(database, error);
	}
	if (pending == NULL)
	{
		g_autoptr(JsonObject) raw = venture_forms_answers_state(answers);
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		g_autofree gchar *text = NULL;
		json_node_set_object(node, raw); text = json_to_string(node, FALSE);
		pending = VENTURE_ENTITY(venture_form_pending_new());
		expires = g_date_time_add_hours(now, 24);
		g_object_set(pending, "organization-id", venture_entity_get_organization_id(form),
			"name", "Unconfirmed signup", "form-id", venture_entity_get_id(form),
			"version-id", venture_entity_get_id(version), "list-id", venture_forms_get_int(form, "optin-list-id"),
			"email", email, "answers", text, "origin", origin, "expires-at", expires,
			"contact-id", contact != NULL ? venture_entity_get_id(contact) : (gint64)0, NULL);
	}
	token = forms_optin_token(form, expires);
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	g_object_set(pending, "token-hash", digest, "last-sent-at", now,
		"send-count", venture_forms_get_int(pending, "send-count") + 1, NULL);
	if (!forms_pending_save(database, pending, error)) goto fail;
	url = g_strdup_printf("%.*s/pub/form/%s/confirm/%s",
		(gint)(strlen(base) - (g_str_has_suffix(base, "/") ? 1 : 0)), base, public, token);
	/* A resend belongs to the original pending version and selected language. */
	mail_fields = venture_forms_record_definition(database, form, pending, error);
	if (mail_fields == NULL) goto fail;
	body = g_strdup_printf("%s\n%s\n\n%s",
		venture_forms_text(mail_fields, "message.optin_intro", "Confirm your signup by opening this link and pressing Confirm signup:"), url,
		venture_forms_text(mail_fields, "message.optin_expiry", "This link expires 24 hours after the original request. If you did not request this signup, ignore this message."));
	idempotency = g_strdup_printf("form-optin:%s:%" G_GINT64_FORMAT, venture_entity_get_uuid(pending), venture_forms_get_int(pending, "send-count"));
	message = venture_mail_message_new();
	g_object_set(message, "organization-id", venture_entity_get_organization_id(form), "to", email,
		"subject", venture_forms_text(mail_fields, "message.optin_subject", "Confirm your signup"), "text-body", "A private signup confirmation link was issued.",
		"private-text-body", body, "idempotency-key", idempotency,
		"related-type", "form_pending", "related-id", venture_entity_get_id(pending), NULL);
	queued = venture_mail_outbox_enqueue(outbox, message, NULL, error);
	if (queued == NULL) goto fail;
	return venture_database_commit(database, error);
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
fail:
	venture_database_rollback(database);
	return FALSE;
}

static gboolean
forms_optin_follow_up(VentureDatabase *database, VentureEntity *form, VentureEntity *response,
	VentureEntity *pending, GPtrArray *fields, JsonObject *answers, GDateTime *now, GError **error)
{
	g_autofree gchar *email = venture_forms_get_string(pending, "email");
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(GPtrArray) rows = NULL;
	gint64 bound = venture_forms_get_int(pending, "contact-id");
	guint i;
	gboolean blocked = FALSE;
	/* Refuse before contact creation: save signals are synchronous, so a
	 * database rollback alone would not undo an observer's side effect. */
	if (!forms_optin_suppressed(database, form, email, &blocked, error)) return FALSE;
	if (blocked)
	{
		venture_set_error_validation(error, "Signup", "retained address suppression prevents this confirmation");
		return FALSE;
	}
	if (!forms_optin_list_valid(database, form, venture_forms_get_int(pending, "list-id"), error)) return FALSE;
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_ILIKE, email, NULL);
	venture_query_set_limit(query, 101);
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return FALSE;
	if (rows->len > 100) goto ambiguous;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *candidate = g_ptr_array_index(rows, i);
		g_autofree gchar *address = venture_forms_get_string(candidate, "email");
		if (address != NULL && g_ascii_strcasecmp(g_strstrip(address), email) == 0)
		{
			if (bound != 0 && venture_entity_get_id(candidate) != bound) continue;
			if (contact != NULL) goto ambiguous;
			contact = g_object_ref(candidate);
		}
	}
	if (contact == NULL)
	{
		if (bound != 0) goto ambiguous;
		contact = VENTURE_ENTITY(venture_contact_new());
		g_object_set(contact, "organization-id", venture_entity_get_organization_id(form), "name", email, "email", email, NULL);
		if (!venture_database_save(database, contact, NULL, error)) return FALSE;
	}
	{
		g_autoptr(VentureEntity) fresh = venture_database_get(database, VENTURE_TYPE_FORM, venture_entity_get_id(form), error);
		if (fresh == NULL) return FALSE;
		if (venture_forms_get_bool(fresh, "one-per-contact"))
		{
			g_autoptr(VentureQuery) responses = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
			gint64 count;
			venture_query_set_organization(responses, venture_entity_get_organization_id(form));
			venture_query_set_include_deleted(responses, TRUE);
			venture_query_add_filter_int(responses, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
			venture_query_add_filter_int(responses, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(contact), NULL);
			count = venture_database_count(database, responses, error);
			if (count < 0) return FALSE;
			if (count > 0)
			{
				venture_set_error_validation(error, "Signup", "a response for this contact has already been received");
				return FALSE;
			}
		}
	}
	if (!forms_record_marketing_consent(database, form, response, contact, fields, answers, now, error)) return FALSE;
	g_object_set(response, "contact-id", venture_entity_get_id(contact), NULL);
	if (venture_forms_get_int(pending, "list-id") > 0)
	{
		g_autoptr(VentureQuery) members = venture_query_new(VENTURE_TYPE_MARKETING_MEMBER);
		g_autoptr(GPtrArray) existing = NULL;
		venture_query_set_organization(members, venture_entity_get_organization_id(form));
		venture_query_add_filter_int(members, "list-id", VENTURE_FILTER_OP_EQ, venture_forms_get_int(pending, "list-id"), NULL);
		venture_query_add_filter_int(members, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(contact), NULL);
		venture_query_set_limit(members, 1);
		existing = venture_database_find(database, members, error);
		if (existing == NULL) return FALSE;
		if (existing->len == 0)
		{
			g_autoptr(VentureEntity) member = VENTURE_ENTITY(venture_marketing_member_new());
			g_object_set(member, "organization-id", venture_entity_get_organization_id(form),
				"list-id", venture_forms_get_int(pending, "list-id"), "contact-id", venture_entity_get_id(contact), NULL);
			if (!venture_database_save(database, member, NULL, error)) return FALSE;
		}
	}
	return TRUE;
ambiguous:
	venture_set_error_validation(error, "Signup", "the confirmed inbox does not identify one current contact");
	return FALSE;
}

VentureEntity *
venture_forms_confirm_signup(VentureDatabase *database, VentureEntity *form, const gchar *token,
	GDateTime *now, gboolean confirm, GError **error)
{
	g_autoptr(VentureEntity) pending = NULL, version = NULL, response = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GHashTable) answers = NULL;
	g_autoptr(JsonObject) refused = NULL;
	g_autofree gchar *text = NULL, *origin = NULL, *ticket = NULL;
	VentureFormsOutcome outcome;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);
	g_return_val_if_fail(now != NULL, NULL);
	if (!venture_database_begin(database, error)) return NULL;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "contact") == G_TYPE_INVALID ||
	    venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_consent") == G_TYPE_INVALID) goto missing;
	pending = forms_optin_find(database, form, token, now);
	if (pending == NULL || !forms_is_open(database, form, now)) goto missing;
	if (!confirm)
	{
		if (!venture_database_commit(database, error)) return NULL;
		return g_steal_pointer(&pending);
	}
	version = venture_database_get(database, VENTURE_TYPE_FORM_VERSION, venture_forms_get_int(pending, "version-id"), error);
	if (version == NULL) goto fail;
	text = venture_forms_get_string(pending, "answers");
	origin = venture_forms_get_string(pending, "origin");
	if (!json_parser_load_from_data(parser, text, -1, error) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) goto fail;
	answers = venture_forms_answers_from_json(json_node_get_object(json_parser_get_root(parser)), error);
	if (answers == NULL) goto fail;
	ticket = venture_forms_ticket_new_for_version(form, venture_forms_get_int(version, "number"), now);
	g_hash_table_remove(answers, VENTURE_FORMS_TICKET);
	venture_forms_answers_add(answers, VENTURE_FORMS_TICKET, ticket);
	g_object_set(pending, "confirming", TRUE, NULL);
	if (!forms_pending_save(database, pending, error)) goto fail;
	g_object_set_data(G_OBJECT(form), VENTURE_FORMS_CONFIRMING, pending);
	if (!venture_forms_submit(database, form, answers, origin, now, &outcome, &response, &refused, error))
	{
		g_object_set_data(G_OBJECT(form), VENTURE_FORMS_CONFIRMING, NULL);
		goto fail;
	}
	g_object_set_data(G_OBJECT(form), VENTURE_FORMS_CONFIRMING, NULL);
	if (outcome != VENTURE_FORMS_ACCEPTED) goto missing;
	if (!forms_pending_purge(database, pending, NULL, NULL, error)) goto fail;
	if (!venture_database_commit(database, error)) return NULL;
	return g_steal_pointer(&response);
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
fail:
	venture_database_rollback(database);
	return NULL;
}
