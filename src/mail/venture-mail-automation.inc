/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean venture_pod_module_handle_mail(VenturePodModule *self, GVariant *params, GVariant **result)
{
	VentureMailOutbox *outbox = venture_context_get_mail_outbox(self->context);
	const gchar *organization = venture_pod_module_argument(params, 0);
	const gchar *bound = venture_pod_module_argument(params, 1);
	g_autoptr(GError) error = NULL;
	gchar *end;
	gint64 org, limit = 100;
	gint attempted;
	if (!outbox || !organization) return FALSE;
	org = g_ascii_strtoll(organization, &end, 10);
	if (*end || org <= 0) return FALSE;
	if (bound) {
		limit = g_ascii_strtoll(bound, &end, 10);
		if (*end || limit < 1 || limit > 1000) return FALSE;
	}
	attempted = venture_mail_outbox_deliver_due(outbox, org, (guint)limit, NULL, NULL, &error);
	if (attempted < 0) {
		g_warning("venture->mail_deliver: %s", error->message);
		return FALSE;
	}
	if (result) *result = g_variant_ref_sink(g_variant_new_parsed("{'attempted': <%i>}", attempted));
	return TRUE;
}
/* venture->mail_sync("1", "200"): sweep an organization's accounts with at
 * most that many messages; each account's cursor resumes on the next run.
 * A failing account is recorded on the account and backs off; only a sweep
 * that could not run at all fails the handler. */
static gboolean venture_pod_module_handle_mail_sync(VenturePodModule *self, GVariant *params, GVariant **result)
{
	const gchar *organization = venture_pod_module_argument(params, 0);
	const gchar *bound = venture_pod_module_argument(params, 1);
	g_autoptr(VentureMailSyncService) service = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *object;
	JsonArray *errors;
	gchar *end;
	gint64 org, budget = 200;
	if (!organization || !venture_context_module_enabled(self->context, "mail_sync")) return FALSE;
	org = g_ascii_strtoll(organization, &end, 10);
	if (*end || org <= 0) return FALSE;
	if (bound) {
		budget = g_ascii_strtoll(bound, &end, 10);
		if (*end || budget < 1 || budget > 1000) return FALSE;
	}
	service = venture_mail_sync_service_new_for_context(self->context);
	g_object_set(service, "message-budget", (guint)budget, NULL);
	report = venture_mail_sync_service_sweep(service, org, G_MAXUINT, NULL, &error);
	if (!report) {
		g_warning("venture->mail_sync: %s", error->message);
		return FALSE;
	}
	object = json_node_get_object(report);
	errors = json_object_get_array_member(object, "errors");
	if (result) *result = g_variant_ref_sink(g_variant_new_parsed("{'accounts': <%i>, 'messages': <%i>, 'errors': <%i>}",
		(gint)venture_json_object_get_int(object, "accounts", 0), (gint)venture_json_object_get_int(object, "messages", 0),
		errors ? (gint)json_array_get_length(errors) : 0));
	return TRUE;
}
