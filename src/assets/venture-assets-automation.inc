/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean
venture_pod_module_assets_run(VenturePodModule *self, GVariant *params, GVariant **result)
{
	const gchar *requested = venture_pod_module_argument(params, 0);
	const gchar *organization = venture_pod_module_argument(params, 1);
	g_autofree gchar *period = NULL;
	g_autoptr(GError) error = NULL;
	gint64 org = venture_context_get_default_organization_id(self->context);
	gint count;
	VentureActor actor;
	if (venture_string_is_empty(requested) || g_str_equal(requested, "last_month"))
	{
		g_autoptr(GDateTime) now = venture_time_now();
		g_autoptr(GDateTime) previous = g_date_time_add_months(now, -1);
		period = g_date_time_format(previous, "%Y-%m");
	}
	else
		period = g_strdup(requested);
	if (!venture_string_is_empty(organization))
	{
		gchar *end = NULL;
		org = g_ascii_strtoll(organization, &end, 10);
		if (end == organization || *end != '\0' || org <= 0)
			return FALSE;
	}
	actor.kind = VENTURE_ACTOR_KIND_AUTOMATION;
	actor.name = "assets period sweep";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	count = venture_asset_service_run_period(venture_asset_service_get(venture_context_get_database(self->context)), period, org, FALSE, &actor, &error);
	if (count < 0)
	{
		g_warning("venture->assets_run_period: %s", error->message);
		return FALSE;
	}
	if (result != NULL)
		*result = venture_pod_module_result(count, period, NULL);
	return TRUE;
}
