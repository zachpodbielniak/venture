/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean suppressed(VentureMarketingService *self, gint64 org, const gchar *email,
	gboolean *result, GError **error)
{
	g_autoptr(VentureEntity) row = find_string(self, VENTURE_TYPE_SUPPRESSION, org, "email", email, error);
	*result = row != NULL;
	return !error || !*error;
}
static VentureEntity *permission(VentureMarketingService *self, VentureEntity *source,
	const gchar *email, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_CONSENT);
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GDateTime) withdrawn = NULL;
	venture_query_set_organization(query, venture_entity_get_organization_id(source));
	venture_query_add_filter_int(query, subject_field(G_OBJECT_TYPE(source)), VENTURE_FILTER_OP_EQ, venture_entity_get_id(source), NULL);
	venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_EQ, email, NULL);
	venture_query_add_filter_string(query, "purpose", VENTURE_FILTER_OP_EQ, "marketing", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	result = venture_database_find_one(self->database, query, error);
	if (result) g_object_get(result, "withdrawn-at", &withdrawn, NULL);
	if (withdrawn) g_clear_object(&result);
	return g_steal_pointer(&result);
}
VentureMarketingConsent *venture_marketing_service_consent(VentureMarketingService *self,
	gint64 org, VentureEntity *input, const gchar *source, const gchar *evidence,
	const gchar *evidence_key, GDateTime *occurred_at, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) subject_row = NULL, existing = NULL;
	g_autoptr(VentureMarketingConsent) row = NULL;
	g_autofree gchar *raw = NULL, *email = NULL;
	gboolean blocked;
	const gchar *field;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), NULL);
	if (!self->database) { refuse(error, "Marketing database is unavailable"); return NULL; }
	g_return_val_if_fail(VENTURE_IS_ENTITY(input), NULL);
	field = subject_field(G_OBJECT_TYPE(input));
	if (!allowed(self, org, error) || !valid_time(occurred_at, error)) return NULL;
	if (!field || !bounded(source, 2048) || !bounded(evidence, 8192) || !bounded(evidence_key, 128) || g_str_has_prefix(evidence_key, "withdraw:")) {
		refuse(error, "Permission requires a contact, company or lead and bounded source, evidence and nonreserved identity"); return NULL;
	}
	if (!venture_database_begin(self->database, error)) return NULL;
	subject_row = get_row(self, G_OBJECT_TYPE(input), org, venture_entity_get_id(input), error);
	if (!subject_row) goto fail;
	raw = string(subject_row, "email"); email = normalize(raw);
	if (!valid_address(email)) { refuse(error, "Permission requires one valid recipient mailbox"); goto fail; }
	existing = find_string(self, VENTURE_TYPE_MARKETING_CONSENT, org, "evidence-key", evidence_key, error);
	if (error && *error) goto fail;
	if (existing) {
		g_autofree gchar *old_email = string(existing, "email"), *old_source = string(existing, "source"), *old_evidence = string(existing, "evidence");
		g_autoptr(GDateTime) old_at = NULL, withdrawn = NULL;
		g_object_get(existing, "recorded-at", &old_at, "withdrawn-at", &withdrawn, NULL);
		if (number(existing, field) != venture_entity_get_id(subject_row) || g_strcmp0(old_email, email) ||
			g_strcmp0(old_source, source) || g_strcmp0(old_evidence, evidence) || withdrawn || !old_at || g_date_time_compare(old_at, occurred_at)) {
			refuse(error, "Consent identity already belongs to different evidence"); goto fail;
		}
		if (!venture_database_commit(self->database, error)) return NULL;
		return VENTURE_MARKETING_CONSENT(g_steal_pointer(&existing));
	}
	if (!suppressed(self, org, email, &blocked, error)) goto fail;
	if (blocked) { refuse(error, "A new permission record cannot clear retained address suppression"); goto fail; }
	row = g_object_new(VENTURE_TYPE_MARKETING_CONSENT, "organization-id", org, field, venture_entity_get_id(subject_row),
		"email", email, "purpose", "marketing", "source", source, "evidence", evidence,
		"evidence-key", evidence_key, "recorded-at", occurred_at, NULL);
	if (!save(self, VENTURE_ENTITY(row), actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&row);
fail:
	venture_database_rollback(self->database); return NULL;
}
static gboolean withdraw(VentureMarketingService *self, gint64 org, const gchar *email,
	gint reason, const gchar *source, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) existing = find_string(self, VENTURE_TYPE_SUPPRESSION, org, "email", email, error);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_CONSENT);
	g_autoptr(GPtrArray) consents = NULL;
	guint i;
	if (error && *error) return FALSE;
	if (!existing) {
		g_autoptr(VentureEntity) row = g_object_new(VENTURE_TYPE_SUPPRESSION, "organization-id", org,
			"email", email, "reason", reason, "at", now, NULL);
		/* Sequence's ordinary save path normalizes and retains suppression,
		 * and exits applicable enrollments in this same transaction. */
		if (!venture_database_save(self->database, row, actor, error)) return FALSE;
	}
	venture_query_set_organization(query, org); venture_query_set_limit(query, 0);
	venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_EQ, email, NULL);
	consents = venture_database_find(self->database, query, error);
	if (!consents) return FALSE;
	for (i = 0; i < consents->len; i++) {
		VentureEntity *original = g_ptr_array_index(consents, i);
		g_autoptr(GDateTime) withdrawn = NULL;
		g_autoptr(VentureEntity) row = NULL, replay = NULL;
		g_autofree gchar *key = NULL;
		g_object_get(original, "withdrawn-at", &withdrawn, NULL);
		if (withdrawn) continue;
		key = g_strdup_printf("withdraw:%s", venture_entity_get_uuid(original));
		replay = find_string(self, VENTURE_TYPE_MARKETING_CONSENT, org, "evidence-key", key, error);
		if (error && *error) return FALSE;
		if (replay) continue;
		row = g_object_new(VENTURE_TYPE_MARKETING_CONSENT, "organization-id", org,
			"withdraws-id", venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_consent") ? venture_entity_get_id(original) : (gint64)0,
			"withdraws-uuid", venture_entity_get_uuid(original),
			"email", email, "purpose", "marketing", "source", source, "evidence", "Marketing permission withdrawn; transactional mail remains independent",
			"recorded-at", now, "withdrawn-at", now, "evidence-key", key, NULL);
		if (!save(self, row, actor, error)) return FALSE;
	}
	return cancel_address(self, org, email, reason, now, actor, error);
}
static VentureEntity *token_recipient(VentureMarketingService *self, const gchar *token,
	const gchar *field, GError **error)
{
	g_autofree gchar *hash = NULL;
	VentureEntity *row;
	gsize i;
	if (!token || strlen(token) != 64) goto missing;
	for (i = 0; i < 64; i++) if (!g_ascii_isxdigit(token[i])) goto missing;
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	row = find_string(self, VENTURE_TYPE_MARKETING_RECIPIENT, 0, field, hash, error);
	if (row || (error && *error)) return row;
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Unknown marketing capability"); return NULL;
}
gboolean venture_marketing_service_unsubscribe(VentureMarketingService *self, const gchar *token, GError **error)
{
	g_autoptr(VentureAccessScope) capability = NULL;
	g_autoptr(VentureEntity) recipient = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *email = NULL;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), FALSE);
	if (!self->database) return refuse(error, "Marketing database is unavailable");
	/* The bearer capability is the authorization. An unrelated session must
	 * neither grant additional scope nor prevent its retained withdrawal. */
	capability = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return FALSE;
	recipient = token_recipient(self, token, "unsubscribe-hash", error);
	if (!recipient) goto fail;
	email = string(recipient, "email");
	if (!withdraw(self, venture_entity_get_organization_id(recipient), email, 0, "Public unsubscribe capability", now, NULL, error)) goto fail;
	{
		g_autoptr(GDateTime) first = NULL;
		gint64 id = venture_entity_get_id(recipient), org = venture_entity_get_organization_id(recipient);
		g_clear_object(&recipient); recipient = get_row(self, VENTURE_TYPE_MARKETING_RECIPIENT, org, id, error);
		if (!recipient) goto fail;
		g_object_get(recipient, "unsubscribed-at", &first, NULL);
		if (!first) { g_object_set(recipient, "unsubscribed-at", now, NULL); if (!save(self, recipient, NULL, error)) goto fail; }
	}
	return venture_database_commit(self->database, error);
fail:
	venture_database_rollback(self->database); return FALSE;
}
