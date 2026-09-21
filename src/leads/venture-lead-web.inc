/* Lead actions share the service used by imports and generic writes. */
static HtmxResponse *
venture_web_lead_ack(guint status)
{
	HtmxResponse *response = htmx_response_new_with_content("{}");
	htmx_response_set_content_type(response, "application/json");
	htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
venture_web_lead_capture(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonObject) fields = NULL;
	GBytes *bytes = htmx_request_get_body_bytes(request);
	g_autofree gchar *address = venture_auth_remote_address(request);
	g_autofree gchar *redirect = NULL;
	gint64 max_mb = 0, rate = 0;
	gsize length = bytes != NULL ? g_bytes_get_size(bytes) : 0;
	const gchar *content_type = htmx_request_get_content_type(request);
	g_object_get(venture_context_get_config(self->context), "server-max-request-size-mb", &max_mb,
		"security-login-rate-limit", &rate, NULL);
	if (length > (guint64)MAX(max_mb, 1) * 1024 * 1024) return venture_web_lead_ack(413);
	if (self->lead_limiter == NULL)
		self->lead_limiter = htmx_rate_limiter_new((guint)MAX(rate, 1), MAX(rate, 1) / 60.0);
	if (!htmx_rate_limiter_allow(self->lead_limiter, address != NULL ? address : "unknown"))
		return venture_web_lead_ack(429);
	if (content_type != NULL && g_str_has_prefix(content_type, "application/json"))
	{
		body = htmx_request_get_json(request, &error);
		if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body)) return venture_web_lead_ack(400);
		fields = json_object_ref(json_node_get_object(body));
	}
	else if (content_type != NULL && g_str_has_prefix(content_type, "application/x-www-form-urlencoded"))
	{
		g_autofree gchar *text = NULL;
		g_autoptr(GHashTable) form = NULL;
		GHashTableIter iter;
		gpointer key, value;
		const gchar *raw = bytes != NULL ? g_bytes_get_data(bytes, NULL) : "";
		if (memchr(raw, '\0', length) != NULL) return venture_web_lead_ack(400);
		text = g_strndup(raw, length);
		form = soup_form_decode(text);
		fields = json_object_new();
		g_hash_table_iter_init(&iter, form);
		while (g_hash_table_iter_next(&iter, &key, &value))
			json_object_set_string_member(fields, key, value);
	}
	else return venture_web_lead_ack(415);
	if (!venture_lead_service_capture(venture_database_get_lead_service(venture_context_get_database(self->context)),
		g_hash_table_lookup(params, "token"), fields, &redirect, &error)) return venture_web_error_response(error);
	if (!venture_string_is_empty(redirect) && g_str_has_prefix(content_type, "application/x-www-form-urlencoded"))
		return venture_web_redirect_to(redirect);
	return venture_web_lead_ack(200);
}

static HtmxResponse *
venture_web_lead_action(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) lead = NULL;
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(GError) error = NULL;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureLeadService *service = venture_database_get_lead_service(db);
	VentureActor actor;
	HtmxResponse *gate;
	gint64 id;
	gboolean ui = !g_str_has_prefix(htmx_request_get_path(request), "/api/");
	const gchar *action = g_hash_table_lookup(params, "action");
	gate = ui ? venture_web_ui_require_session(self, request) : venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error)) return venture_web_error_response(error);
	gate = venture_web_require_module_api(self, "leads");
	if (gate != NULL) return gate;
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	lead = venture_database_get(db, VENTURE_TYPE_LEAD, id, &error);
	if (lead == NULL) return venture_web_lead_ack(404);
	if (ui)
	{
		options = json_object_new();
		json_object_set_boolean_member(options, "deal", TRUE);
	}
	else
	{
		body = htmx_request_get_json(request, &error);
		if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body)) return venture_web_lead_ack(400);
		options = json_object_ref(json_node_get_object(body));
	}
	venture_auth_to_actor(principal, &actor);
	if (g_strcmp0(action, "convert") == 0)
	{
		gboolean stage = FALSE;
		if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, &error))
			return venture_web_error_response(error);
		if (stage)
		{
			VentureConfirmation *confirmation = venture_lead_service_stage_convert(service,
				venture_context_get_confirmations(self->context), lead, options, &actor, &error);
			if (confirmation == NULL) return venture_web_error_response(error);
			result = venture_confirmation_to_json(confirmation);
			return venture_web_json_response(result, 202);
		}
		converted = venture_lead_service_convert(service, lead, options, &actor, &error);
		if (converted == NULL) return venture_web_error_response(error);
	}
	else if (g_strcmp0(action, "reassign") == 0)
	{
		gboolean stage = FALSE;
		if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, &error))
			return venture_web_error_response(error);
		if (stage) return venture_web_lead_ack(501);
		if (!venture_lead_service_reassign(service, lead, venture_json_object_get_string(options, "owner", NULL), &actor, &error))
			return venture_web_error_response(error);
		converted = g_object_ref(lead);
	}
	else if (g_strcmp0(action, "reroute") == 0 || g_strcmp0(action, "rescore") == 0)
	{
		gboolean stage = FALSE;
		gboolean ok;
		if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, &error))
			return venture_web_error_response(error);
		if (stage) return venture_web_lead_ack(501);
		ok = g_strcmp0(action, "reroute") == 0 ?
			venture_lead_service_reroute(service, lead, &actor, &error) :
			venture_lead_service_rescore(service, lead, &actor, &error);
		if (!ok) return venture_web_error_response(error);
		converted = g_object_ref(lead);
	}
	else return venture_web_lead_ack(404);
	if (ui)
	{
		g_autofree gchar *path = g_strdup_printf("/e/lead/%" G_GINT64_FORMAT, id);
		return venture_web_redirect_to(path);
	}
	result = venture_serializable_to_json(VENTURE_SERIALIZABLE(converted), FALSE);
	return venture_web_json_response(result, 200);
}

static void
venture_web_append_lead_actions(GString *html, VentureEntity *record)
{
	VentureLeadStatus status;
	g_object_get(record, "status", &status, NULL);
	if (status == VENTURE_LEAD_QUALIFIED)
		g_string_append_printf(html, "<form method=\"post\" action=\"/leads/%" G_GINT64_FORMAT
			"/convert\"><button class=\"btn\" type=\"submit\">Convert</button></form>", venture_entity_get_id(record));
	if (status != VENTURE_LEAD_CONVERTED)
		g_string_append_printf(html, "<form method=\"post\" action=\"/leads/%" G_GINT64_FORMAT
			"/reroute\"><button class=\"btn\" type=\"submit\">Re-route</button></form>"
			"<form method=\"post\" action=\"/leads/%" G_GINT64_FORMAT
			"/rescore\"><button class=\"btn\" type=\"submit\">Rescore</button></form>",
			venture_entity_get_id(record), venture_entity_get_id(record));
}
