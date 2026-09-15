/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean venture_pod_module_handle_report_packs(VenturePodModule *self, GVariant *params, GVariant **result)
{
	const gchar *organization = venture_pod_module_argument(params, 0);
	g_autoptr(GError) error = NULL;
	gchar *end;
	gint64 org;
	gint ran;
	if (!organization) return FALSE;
	org = g_ascii_strtoll(organization, &end, 10);
	if (*end || org <= 0) return FALSE;
	ran = venture_report_pack_service_run_due(venture_report_pack_service_get(venture_context_get_database(self->context)),
		self->context, org, NULL, NULL, &error);
	if (ran < 0) {
		g_warning("venture->report_packs_run: %s", error->message);
		return FALSE;
	}
	if (result) *result = g_variant_ref_sink(g_variant_new_parsed("{'ran': <%i>}", ran));
	return TRUE;
}
