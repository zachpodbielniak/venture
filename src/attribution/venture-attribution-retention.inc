/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean administrative(VentureAttributionService *self, gint64 org, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureAccessPolicy *policy;
	const VentureAuthPrincipal *principal;
	if (!self->database || org <= 0) return refuse(error, "An exact organization is required");
	policy = venture_database_get_access_policy(self->database); principal = venture_access_policy_get_actor(policy);
	if (principal && !venture_access_policy_has_organization_role(policy, principal, org, roles, G_N_ELEMENTS(roles))) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization administration is required for analytics configuration and retention"); return FALSE;
	}
	return TRUE;
}
static gboolean redact_visitor(VentureAttributionService *self, VentureEntity *visitor, GDateTime *now,
	gboolean withdraw, gboolean forget, const VentureActor *actor, GError **error)
{
	GType types[] = { VENTURE_TYPE_ATTRIBUTION_TOUCH, VENTURE_TYPE_ATTRIBUTION_SUBMISSION };
	g_autoptr(GDateTime) redacted = NULL, withdrawn = NULL;
	guint i, j;
	g_object_get(visitor, "redacted-at", &redacted, "withdrawn-at", &withdrawn, NULL);
	if (!redacted) {
		for (i = 0; i < G_N_ELEMENTS(types); i++) {
			g_autoptr(VentureQuery) query = venture_query_new(types[i]);
			g_autoptr(GPtrArray) rows = NULL;
			venture_query_set_organization(query, venture_entity_get_organization_id(visitor));
			venture_query_set_limit(query, 1001);
			venture_query_add_filter_int(query, "visitor-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(visitor), NULL);
			rows = venture_database_find(self->database, query, error); if (!rows) return FALSE;
			if (rows->len > 1000) return refuse(error, "Visitor evidence exceeds its documented retention bounds");
			for (j = 0; j < rows->len; j++) {
				VentureEntity *row = g_ptr_array_index(rows, j);
				g_object_set(row, "visitor-id", (gint64)0, NULL);
				if (VENTURE_IS_ATTRIBUTION_TOUCH(row))
					g_object_set(row, "session-key", NULL, "path", NULL, "referrer-origin", NULL,
						"payload-hash", NULL, "event-key", NULL, "source", "redacted", "medium", NULL, "campaign-label", NULL,
						"campaign-id", (gint64)0, NULL);
				if (!save(self, row, actor, error)) return FALSE;
			}
		}
		g_object_set(visitor, "redacted-at", now, "session-key", NULL, "last-seen-at", NULL,
			"rate-window-at", NULL, "rate-count", (gint64)0, NULL);
	}
	if (withdraw && !withdrawn) g_object_set(visitor, "withdrawn-at", now, NULL);
	if (forget) g_object_set(visitor, "token-hash", NULL, NULL);
	return save(self, visitor, actor, error);
}
gboolean venture_attribution_service_withdraw(VentureAttributionService *self,
	const gchar *site_uuid, const gchar *token, GDateTime *now, GError **error)
{
	g_autoptr(VentureAccessScope) capability = NULL;
	g_autoptr(VentureEntity) site = NULL, visitor = NULL;
	g_return_val_if_fail(VENTURE_IS_ATTRIBUTION_SERVICE(self), FALSE);
	if (!self->database || !clock_valid(now, error)) return FALSE;
	/* A nested transaction cannot make a durable privacy decision: its
	 * caller could roll back after this API reported successful revocation. */
	if (venture_database_has_transaction(self->database)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Retry analytics withdrawal after the active transaction completes"); return FALSE;
	}
	capability = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return FALSE;
	site = site_get(self, site_uuid, NULL, FALSE, error); if (!site) goto fail;
	visitor = visitor_get(self, site, token, now, FALSE, error); if (!visitor) goto fail;
	if (!redact_visitor(self, visitor, now, TRUE, FALSE, NULL, error)) goto fail;
	return venture_database_commit(self->database, error);
fail:
	venture_database_rollback(self->database); return FALSE;
}
gint venture_attribution_service_sweep(VentureAttributionService *self, gint64 org,
	guint limit, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ATTRIBUTION_VISITOR);
	g_autoptr(GPtrArray) visitors = NULL;
	g_autofree gchar *until = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_ATTRIBUTION_SERVICE(self), -1);
	if (!administrative(self, org, error) || !clock_valid(now, error)) return -1;
	if (limit < 1 || limit > 100) { refuse(error, "Retention sweeps examine one through 100 visitors"); return -1; }
	if (!venture_database_begin(self->database, error)) return -1;
	until = venture_time_to_string(now);
	venture_query_set_organization(query, org); venture_query_set_limit(query, limit);
	venture_query_add_filter_string(query, "expires-at", VENTURE_FILTER_OP_LTE, until, NULL);
	venture_query_add_filter(query, "token-hash", VENTURE_FILTER_OP_NOT_NULL, NULL, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	visitors = venture_database_find(self->database, query, error); if (!visitors) goto fail;
	for (i = 0; i < visitors->len; i++)
		if (!redact_visitor(self, g_ptr_array_index(visitors, i), now, FALSE, TRUE, actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return -1;
	return visitors->len;
fail:
	venture_database_rollback(self->database); return -1;
}
