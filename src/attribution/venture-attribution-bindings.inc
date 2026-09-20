/* SPDX-License-Identifier: AGPL-3.0-or-later */
static void copy_touch_snapshot(VentureEntity *target, VentureEntity *source, gboolean first)
{
	const gchar *prefix = first ? "first" : "last";
	g_autofree gchar *name = g_strdup_printf("%s-source", prefix), *text = string(source, name);
	g_autofree gchar *touch = g_strdup_printf("%s-touch-id", prefix), *campaign = g_strdup_printf("%s-campaign-id", prefix);
	g_autofree gchar *evidence = g_strdup_printf("%s-evidence", prefix), *reason = string(source, evidence);
	g_autofree gchar *lookback = g_strdup_printf("%s-lookback-days", prefix);
	g_object_set(target, name, text, touch, number(source, touch), campaign, number(source, campaign),
		evidence, reason, lookback, number(source, "lookback-days"), NULL);
}
static gboolean update_binding(VentureAttributionService *self, VentureEntity *submission,
	VentureEntity *subject, GDateTime *now, GError **error)
{
	g_autoptr(VentureEntity) binding = NULL;
	g_autoptr(GDateTime) converted = NULL;
	gint64 org = venture_entity_get_organization_id(subject);
	if (!VENTURE_IS_LEAD(subject)) return TRUE;
	binding = find_string(self, VENTURE_TYPE_ATTRIBUTION_BINDING, org, "binding-key", venture_entity_get_uuid(subject), error);
	if (error && *error) return FALSE;
	if (binding) {
		g_object_get(binding, "bound-at", &converted, NULL);
		if (converted) return TRUE;
	} else {
		binding = g_object_new(VENTURE_TYPE_ATTRIBUTION_BINDING, "organization-id", org,
			"lead-id", venture_entity_get_id(subject), "binding-key", venture_entity_get_uuid(subject), "captured-at", now, NULL);
		copy_touch_snapshot(binding, submission, TRUE);
	}
	copy_touch_snapshot(binding, submission, FALSE);
	g_object_set(binding, "submission-id", venture_entity_get_id(submission), "latest-submitted-at", now, NULL);
	return save(self, binding, NULL, error);
}
static gboolean validate_conversion(VentureDatabase *database, VentureEntity *row, VentureEntity *previous,
	gpointer data, GError **error)
{
	VentureAttributionService *self = data;
	g_autoptr(VentureEntity) binding = NULL;
	g_autoptr(GDateTime) at = NULL, before = NULL;
	g_autoptr(OrmInspector) inspector = NULL;
	g_autoptr(GError) local = NULL;
	gint state, old;
	if (!previous) return TRUE;
	g_object_get(row, "status", &state, NULL); g_object_get(previous, "status", &old, NULL);
	if (state != VENTURE_LEAD_CONVERTED || old == VENTURE_LEAD_CONVERTED) return TRUE;
	/* A disabled optional module may never have created its tables. Existing
	 * bindings still finish inside the canonical conversion transaction. */
	inspector = orm_inspector_new(venture_database_get_connection(database), &local);
	if (!inspector) {
		if (local) g_propagate_error(error, g_steal_pointer(&local));
		else refuse(error, "Attribution schema inspection unavailable");
		return FALSE;
	}
	if (!orm_inspector_has_table(inspector, "attribution_bindings", NULL, &local)) {
		if (local) { g_propagate_error(error, g_steal_pointer(&local)); return FALSE; }
		return TRUE;
	}
	binding = find_string(self, VENTURE_TYPE_ATTRIBUTION_BINDING, venture_entity_get_organization_id(row),
		"binding-key", venture_entity_get_uuid(row), error);
	if (!binding) return !error || !*error;
	g_object_get(binding, "bound-at", &before, NULL);
	if (before) return refuse(error, "Lead acquisition was already bound to a conversion");
	g_object_get(row, "last-activity-at", &at, NULL);
	if (!at) return refuse(error, "Conversion requires its canonical activity timestamp");
	g_object_set(binding, "company-id", number(row, "converted-company-id"),
		"contact-id", number(row, "converted-contact-id"), "deal-id", number(row, "converted-deal-id"), "bound-at", at, NULL);
	return save(self, binding, NULL, error);
}
