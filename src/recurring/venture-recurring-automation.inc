/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean
venture_pod_module_recurring_run(VenturePodModule *self, GVariant *params, GVariant **result)
{
	const gchar *as_of_text = venture_pod_module_argument(params, 0);
	const gchar *organization = venture_pod_module_argument(params, 1);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	gint64 org = venture_context_get_default_organization_id(self->context);
	gint count;
	VentureActor actor;
	if (!venture_string_is_empty(as_of_text))
	{
		as_of = venture_time_from_string(as_of_text, &error);
		if (as_of == NULL)
			return FALSE;
	}
	if (!venture_string_is_empty(organization))
	{
		gchar *end = NULL;
		org = g_ascii_strtoll(organization, &end, 10);
		if (end == organization || *end != '\0' || org <= 0)
			return FALSE;
	}
	actor.kind = VENTURE_ACTOR_KIND_AUTOMATION;
	actor.name = "recurring sweep";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	count = venture_recurring_service_run(venture_recurring_service_get(venture_context_get_database(self->context)),
		org, as_of, FALSE, &actor, &error);
	if (count < 0)
	{
		g_warning("venture->recurring_run: %s", error->message);
		return FALSE;
	}
	if (result != NULL)
		*result = venture_pod_module_result(count, NULL, NULL);
	return TRUE;
}

static gboolean
venture_pod_module_collections_run(VenturePodModule *self, GVariant *params, GVariant **result)
{
	const gchar *as_of_text = venture_pod_module_argument(params, 0);
	const gchar *organization = venture_pod_module_argument(params, 1);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	gint64 org = venture_context_get_default_organization_id(self->context);
	gint count;
	VentureActor actor;
	if (!venture_string_is_empty(as_of_text))
	{
		as_of = venture_time_from_string(as_of_text, &error);
		if (as_of == NULL)
			return FALSE;
	}
	if (!venture_string_is_empty(organization))
	{
		gchar *end = NULL;
		org = g_ascii_strtoll(organization, &end, 10);
		if (end == organization || *end != '\0' || org <= 0)
			return FALSE;
	}
	actor.kind = VENTURE_ACTOR_KIND_AUTOMATION;
	actor.name = "collections sweep";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	count = venture_collection_service_run(venture_collection_service_get(venture_context_get_database(self->context)),
		self->context, org, as_of, &actor, &error);
	if (count < 0)
	{
		g_warning("venture->collections_run: %s", error->message);
		return FALSE;
	}
	if (result != NULL)
		*result = venture_pod_module_result(count, NULL, NULL);
	return TRUE;
}
