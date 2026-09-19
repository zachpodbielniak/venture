/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* venture->backups_run("ORG") dispatches the due backup schedules of one
 * organization, or of every one with "0", the way report_packs_run does. */
static gboolean venture_pod_module_handle_backups(VenturePodModule *self, GVariant *params, GVariant **result)
{
	const gchar *organization = venture_pod_module_argument(params, 0);
	g_autoptr(GError) error = NULL;
	gchar *end;
	gint64 org;
	gint ran;
	if (!organization) return FALSE;
	org = g_ascii_strtoll(organization, &end, 10);
	if (*end || org < 0) return FALSE;
	ran = venture_backup_schedule_service_run_due(venture_backup_schedule_service_get(venture_context_get_database(self->context)),
		org, NULL, NULL, &error);
	if (ran < 0) {
		g_warning("venture->backups_run: %s", error->message);
		return FALSE;
	}
	if (result) *result = g_variant_ref_sink(g_variant_new_parsed("{'ran': <%i>}", ran));
	return TRUE;
}
