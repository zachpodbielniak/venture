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
