/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture->dunning_sweep([as_of], [organization], [limit]): the overdue
 * reminder sweep on a schedule. Empty arguments take the defaults -- now,
 * the default organization, 100 invoices -- so a daily rule needs none.
 * The pipe result's count is reminders queued plus escalations, and its
 * summary is the sweep's JSON answer, failures and warnings included.
 */
static gboolean
venture_pod_module_dunning_sweep(VenturePodModule *self, GVariant *params, GVariant **result)
{
	const gchar *as_of_text = venture_pod_module_argument(params, 0);
	const gchar *organization = venture_pod_module_argument(params, 1);
	const gchar *bound = venture_pod_module_argument(params, 2);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *summary = NULL;
	gint64 org = venture_context_get_default_organization_id(self->context);
	gint64 limit = 100;
	JsonObject *object;
	VentureActor actor;
	if (!venture_string_is_empty(as_of_text))
	{
		as_of = venture_time_from_string(as_of_text, &error);
		if (as_of == NULL)
		{
			g_warning("venture->dunning_sweep: %s", error->message);
			return FALSE;
		}
	}
	if (!venture_string_is_empty(organization))
	{
		gchar *end = NULL;
		org = g_ascii_strtoll(organization, &end, 10);
		if (end == organization || *end != '\0' || org <= 0)
			return FALSE;
	}
	if (!venture_string_is_empty(bound))
	{
		gchar *end = NULL;
		limit = g_ascii_strtoll(bound, &end, 10);
		if (end == bound || *end != '\0' || limit < 1 || limit > 1000)
			return FALSE;
	}
	actor.kind = VENTURE_ACTOR_KIND_AUTOMATION;
	actor.name = "dunning sweep";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	answer = venture_dunning_service_sweep_detailed(venture_dunning_service_get(venture_context_get_database(self->context)),
		org, as_of, (guint)limit, FALSE, &actor, &error);
	if (answer == NULL)
	{
		g_warning("venture->dunning_sweep: %s", error->message);
		return FALSE;
	}
	object = json_node_get_object(answer);
	summary = venture_json_to_string(answer, FALSE);
	if (result != NULL)
		*result = venture_pod_module_result(json_object_get_int_member(object, "queued") +
			json_object_get_int_member(object, "escalated"), summary, NULL);
	return TRUE;
}
