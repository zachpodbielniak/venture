/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included in the service so only its decision path can lend send authority. */
void
venture_quotes_settings_register(void)
{
	static const VentureSettingInfo settings[] = {
		{ "quotes.discount-approval", "Quotes", "quotes", "Require discount approval",
		  "Sending or sharing above the line discount limit needs another person's approval.",
		  VENTURE_SETTING_BOOLEAN, "false", 0, 0, NULL, VENTURE_SETTING_PERMISSION_OWNER,
		  "Organization", "quote-discount-approval", NULL, NULL, NULL, NULL },
		{ "quotes.max-line-discount-percent", "Quotes", "quotes", "Maximum unapproved line discount (%)",
		  "A line strictly above this percentage requires approval when the policy is enabled.",
		  VENTURE_SETTING_INTEGER, "0", 0, 100, NULL, VENTURE_SETTING_PERMISSION_OWNER,
		  "Organization", "quote-max-line-discount-percent", NULL, NULL, NULL, NULL },
		{ "quotes.discount-approver", "Quotes", "quotes", "Discount approver role",
		  "Global owner, or global administrator and owner. The author and proposer cannot approve.",
		  VENTURE_SETTING_CHOICE, "owner", 0, 0, "owner,admin", VENTURE_SETTING_PERMISSION_OWNER,
		  "Organization", "quote-discount-approver", "", NULL, NULL, NULL }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(settings); i++) venture_settings_register(&settings[i]);
}

void
venture_quotes_bind_context(VentureContext *context)
{
	VentureQuoteService *self = venture_database_get_quote_service(venture_context_get_database(context));
	g_weak_ref_set(&self->context, context);
}

/* Header versions alone miss description changes and equal-total edits.
 * Bind the in-memory proposal to every live line's identity and version. */
static gchar *
discount_line_versions(VentureQuoteService *self, VentureEntity *request, GError **error)
{
	g_autoptr(GPtrArray) lines = find(self, VENTURE_TYPE_QUOTE_LINE,
		venture_entity_get_organization_id(request), "quote-id", integer(request, "quote-id"), error);
	g_autoptr(GString) versions = g_string_new(NULL);
	guint i;
	if (lines == NULL) return NULL;
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_string_append_printf(versions, "%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ";",
			venture_entity_get_id(line), venture_entity_get_version(line));
	}
	return g_string_free(g_steal_pointer(&versions), FALSE);
}

static GError *
discount_gate(VentureQuoteService *self, VentureEntity *q, const gchar *verb, gpointer data)
{
	g_autoptr(VentureEntity) org = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	GError *error = NULL;
	gboolean enabled = FALSE;
	gint64 limit;
	guint i;
	(void)data;
	if ((g_strcmp0(verb, "send") != 0 && g_strcmp0(verb, "share") != 0) || state(q) != VENTURE_QUOTE_DRAFT)
		return NULL;
	if (self->approved_request != NULL &&
		integer(self->approved_request, "quote-id") == venture_entity_get_id(q) &&
		integer(self->approved_request, "expected-version") == venture_entity_get_version(q) &&
		venture_entity_get_organization_id(self->approved_request) == venture_entity_get_organization_id(q))
	{
		g_autofree gchar *current = discount_line_versions(self, self->approved_request, &error);
		if (current == NULL) return error;
		if (g_strcmp0(current, g_object_get_data(G_OBJECT(self->approved_request), "venture-quote-discount-lines")) != 0)
			return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Quote lines changed; request approval again");
		return NULL;
	}
	org = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, venture_entity_get_organization_id(q), &error);
	if (org == NULL) return error != NULL ? error : g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Quote organization is missing");
	g_object_get(org, "quote-discount-approval", &enabled, NULL);
	if (!enabled) return NULL;
	limit = integer(org, "quote-max-line-discount-percent");
	lines = find(self, VENTURE_TYPE_QUOTE_LINE, venture_entity_get_organization_id(q), "quote-id", venture_entity_get_id(q), &error);
	if (lines == NULL) return error;
	for (i = 0; i < lines->len; i++)
	{
		gint64 percent = integer(g_ptr_array_index(lines, i), "discount-percent");
		if (percent > limit)
			return g_error_new(VENTURE_ERROR, VENTURE_ERROR_AI_CONFIRMATION_REQUIRED,
				"Line discount %" G_GINT64_FORMAT "%% exceeds the %" G_GINT64_FORMAT "%% approval threshold", percent, limit);
	}
	return NULL;
}

static gboolean
execute_with_discount(VentureQuoteService *self, VentureEntity *request,
	const gchar *method, const gchar *ip, const VentureActor *actor, GError **error)
{
	g_autoptr(GError) local = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) staged = NULL;
	g_autofree gchar *via = NULL;
	g_autofree gchar *versions = NULL;
	VentureConfirmation *confirmation;
	const VentureAuthPrincipal *principal;
	VentureActor origin;
	g_autoptr(VentureAuthPrincipal) origin_principal = NULL;
	if (venture_quote_service_execute_impl(self, request, method, ip, actor, &local)) return TRUE;
	if (!g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_AI_CONFIRMATION_REQUIRED))
	{
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	principal = venture_access_policy_get_actor(venture_database_get_access_policy(self->database));
	context = g_weak_ref_get(&self->context);
	if (context == NULL || principal == NULL || !principal->authenticated || principal->user_id <= 0)
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "discount approval requires an authenticated user and confirmation queue");
	staged = venture_entity_duplicate(request);
	venture_entity_set_organization_id(staged, venture_entity_get_organization_id(request));
	g_object_set(staged, "reason", local->message, NULL);
	versions = discount_line_versions(self, staged, error);
	if (versions == NULL) return FALSE;
	g_object_set_data_full(G_OBJECT(staged), "venture-quote-discount-lines", g_steal_pointer(&versions), g_free);
	origin_principal = venture_auth_principal_copy(principal);
	venture_auth_to_actor(origin_principal, &origin);
	via = g_strdup_printf("quote-discount:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
		integer(request, "quote-id"), integer(request, "expected-version"), principal->user_id);
	confirmation = venture_confirmation_store_stage(venture_context_get_confirmations(context),
		VENTURE_AUDIT_ACTION_CREATE, staged, NULL, &origin, via, error);
	if (confirmation == NULL) return FALSE;
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_AI_CONFIRMATION_REQUIRED,
		"Pending approval %s: %s. The quote has not been sent.", venture_confirmation_get_id(confirmation), local->message);
	return FALSE;
}

gboolean
venture_quote_service_decide_discount(VentureQuoteService *self, VentureEntity *request,
	gint64 proposer, gboolean approve, const gchar *reason, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) q = NULL;
	g_autoptr(VentureEntity) org = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *role = NULL;
	g_autofree gchar *trigger = NULL;
	g_autofree gchar *decision = NULL;
	g_autofree gchar *versions = NULL;
	g_autoptr(VentureEntity) approved = NULL;
	g_autoptr(VentureEntity) evidence = venture_entity_duplicate(request);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(venture_database_get_access_policy(self->database));
	gboolean ok;
	q = get(self, VENTURE_TYPE_QUOTE, venture_entity_get_organization_id(request), integer(request, "quote-id"), error);
	if (q == NULL) return FALSE;
	org = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, venture_entity_get_organization_id(q), error);
	if (org == NULL) return FALSE;
	g_object_get(org, "quote-discount-approver", &role, NULL);
	if (principal == NULL || !principal->authenticated || principal->user_id <= 0 ||
		principal->user_id == proposer || principal->user_id == integer(q, "author-user-id") ||
		(principal->role != VENTURE_USER_ROLE_OWNER &&
		 !(g_strcmp0(role, "admin") == 0 && principal->role == VENTURE_USER_ROLE_ADMIN)))
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "another user in the configured approver role must decide this discount");
	if (integer(request, "expected-version") != venture_entity_get_version(q) || state(q) != VENTURE_QUOTE_DRAFT)
		return refuse(error, VENTURE_ERROR_CONFLICT, "quote changed; request approval again");
	versions = discount_line_versions(self, request, error);
	if (versions == NULL) return FALSE;
	if (g_strcmp0(versions, g_object_get_data(G_OBJECT(request), "venture-quote-discount-lines")) != 0)
		return refuse(error, VENTURE_ERROR_CONFLICT, "quote lines changed; request approval again");
	if (!approve && venture_string_is_empty(reason))
		return refuse(error, VENTURE_ERROR_VALIDATION, "give a reason for rejecting the discount");
	if (self->busy || self->approved_request != NULL)
		return refuse(error, VENTURE_ERROR_CONFLICT, "a quote decision is already in progress");
	g_object_get(request, "reason", &trigger, NULL);
	decision = g_strdup_printf("%s; decided by user #%" G_GINT64_FORMAT "%s%s", trigger != NULL ? trigger : "Discount approval",
		principal->user_id, approve ? "" : "; rejection: ", approve ? "" : reason);
	g_object_set(evidence, "reason", decision, NULL);
	if (!venture_database_begin(self->database, error)) return FALSE;
	ok = event(self, q, approve ? "discount-approved" : "discount-rejected", evidence, "manual", NULL, now, actor, error);
	if (ok && approve)
	{
		/* A failed persistence attempt must not leave the queued object
		 * carrying an id/version assigned by a transaction that rolled back. */
		approved = venture_entity_duplicate(request);
		venture_entity_set_organization_id(approved, venture_entity_get_organization_id(request));
		g_object_set_data_full(G_OBJECT(approved), "venture-quote-discount-lines", g_strdup(versions), g_free);
		self->approved_request = approved;
		ok = venture_quote_service_execute_impl(self, approved, "manual", NULL, actor, error);
		self->approved_request = NULL;
	}
	if (ok) return venture_database_commit(self->database, error);
	venture_database_rollback(self->database);
	return FALSE;
}

gboolean
venture_quote_service_discount_pending(VentureQuoteService *self, VentureEntity *request,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) q = NULL;
	g_autoptr(VentureEntity) org = NULL;
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *role = NULL;
	g_autofree gchar *reason = NULL;
	g_autofree gchar *label = NULL;
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(venture_database_get_access_policy(self->database));
	gint64 proposer = principal != NULL ? principal->user_id : 0;
	guint i;
	gboolean ok = FALSE;
	q = get(self, VENTURE_TYPE_QUOTE, venture_entity_get_organization_id(request), integer(request, "quote-id"), error);
	if (q == NULL) return FALSE;
	org = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, venture_entity_get_organization_id(q), error);
	if (org == NULL) return FALSE;
	g_object_get(org, "quote-discount-approver", &role, NULL);
	g_object_get(request, "reason", &reason, NULL);
	g_object_get(q, "number", &label, NULL);
	if (!venture_database_begin(self->database, error)) return FALSE;
	if (!event(self, q, "discount-requested", request, "manual", NULL, now, actor, error)) goto done;
	/* Only system inbox delivery needs internal authority. The action itself
	 * was checked above under the originating principal. */
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	venture_query_set_limit(query, 0);
	users = venture_database_find(self->database, query, error);
	if (users == NULL) goto done;
	for (i = 0; i < users->len; i++)
	{
		VentureEntity *user = g_ptr_array_index(users, i);
		g_autoptr(VentureEntity) notice = NULL;
		gint user_role;
		gboolean active;
		gint64 id = venture_entity_get_id(user);
		g_object_get(user, "role", &user_role, "active", &active, NULL);
		if (!active || id == proposer || id == integer(q, "author-user-id") ||
			(user_role != VENTURE_USER_ROLE_OWNER && !(g_strcmp0(role, "admin") == 0 && user_role == VENTURE_USER_ROLE_ADMIN))) continue;
		notice = VENTURE_ENTITY(venture_notification_new());
		venture_entity_set_organization_id(notice, venture_entity_get_organization_id(q));
		g_object_set(notice, "user-id", id, "kind", VENTURE_NOTIFICATION_KIND_SYSTEM,
			"title", "Quote discount needs approval", "body", reason, "target-type", "quote",
			"target-id", venture_entity_get_id(q), "target-label", label,
			"actor", actor != NULL ? actor->name : NULL, "occurred-at", now, NULL);
		if (!venture_database_save(self->database, notice, NULL, error)) goto done;
	}
	ok = TRUE;
done:
	if (ok) return venture_database_commit(self->database, error);
	venture_database_rollback(self->database);
	return FALSE;
}
