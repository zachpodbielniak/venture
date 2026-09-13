/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Included by venture-web-server.c to share the ordinary auth/page pipeline.
 * All protocol, persistence and merge semantics live in core. */
static HtmxResponse *
venture_web_federation_signed(VentureWebServer *self, JsonNode *node, const gchar *binding)
{
	g_autofree gchar *text = venture_json_to_string(node, FALSE);
	g_autoptr(GBytes) body = g_bytes_new(text, strlen(text));
	g_autoptr(GError) error = NULL;
	g_autofree gchar *signature = NULL;
	HtmxResponse *response;
	signature = venture_federation_sign_response(self->context, body, binding, &error);
	if (!signature) return venture_web_error_response(error);
	response = htmx_response_new_with_content(text);
	htmx_response_set_content_type(response, "application/json");
	htmx_response_add_header(response, "Cache-Control", "no-store");
	htmx_response_add_header(response, "X-Venture-Federation-Signature", signature);
	return response;
}

static HtmxResponse *
venture_web_federation_identity(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = venture_federation_identity(self->context, &error);
	const gchar *binding = htmx_request_get_query_param(request, "nonce");
	(void)request;
	(void)params;
	if (!node) return venture_web_error_response(error);
	return venture_web_federation_signed(self, node, binding && strlen(binding) <= 128 ? binding : "identity");
}

static HtmxResponse *
venture_web_federation_receive(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	GBytes *body = htmx_request_get_body_bytes(request);
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	const gchar *signature;
	g_autofree gchar *binding = NULL;
	(void)params;
	signature = soup_message_headers_get_one(soup_server_message_get_request_headers(
		htmx_request_get_message(request)), "X-Venture-Federation-Signature");
	node = venture_federation_receive(self->context, body, signature, &error);
	if (!node) return venture_web_error_response(error);
	{
		g_autofree gchar *text = venture_json_to_string(node, FALSE);
		if (strlen(text) > 1024 * 1024)
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"Shared response exceeds 1 MiB; narrow the grant");
			return venture_web_error_response(error);
		}
	}
	binding = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, body);
	return venture_web_federation_signed(self, node, binding);
}

/* Local auth is checked separately from peer auth. A signed federation
 * request cannot call this endpoint or become a local owner/editor. */
static HtmxResponse *
venture_web_api_federation(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(VentureEntity) replica = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	JsonObject *object;
	const gchar *action;
	gint64 peer;
	VentureActor actor;
	HtmxResponse *gate;
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate) return gate;
	gate = venture_web_require_module_api(self, "federation");
	if (gate) return gate;
	body = htmx_request_get_json(request, &error);
	if (!body) return venture_web_error_response(error);
	if (!JSON_NODE_HOLDS_OBJECT(body))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Expected a federation operation object");
		return venture_web_error_response(error);
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	object = json_node_get_object(body);
	action = venture_json_object_get_string(object, "action", "");
	peer = venture_json_object_get_int(object, "peer_id", 0);
	if (!g_strcmp0(action, "identity"))
		result = venture_federation_identity(self->context, &error);
	else if (!g_strcmp0(action, "remote"))
	{
		JsonNode *operation = json_object_get_member(object, "operation");
		if (operation && JSON_NODE_HOLDS_OBJECT(operation))
			result = venture_federation_request(self->context, peer, operation, &error);
	}
	else if (!g_strcmp0(action, "pull"))
		replica = venture_federation_replica_pull(self->context, peer,
			venture_json_object_get_string(object, "type", ""),
			venture_json_object_get_string(object, "uuid", ""), &actor, &error);
	else if (!g_strcmp0(action, "pull_collection"))
		result = venture_federation_collection_pull(self->context, peer,
			venture_json_object_get_string(object, "collection", ""),
			venture_json_object_get_int(object, "offset", 0), &actor, &error);
	else if (!g_strcmp0(action, "resolve"))
		replica = venture_federation_replica_resolve(self->context,
			venture_json_object_get_int(object, "id", 0),
			venture_json_object_get_int(object, "version", 0),
			venture_json_object_get_string(object, "field", ""),
			venture_json_object_get_bool(object, "keep_local", FALSE), &actor, &error);
	else if (!g_strcmp0(action, "sync"))
		replica = venture_federation_replica_sync(self->context,
			venture_json_object_get_int(object, "id", 0), &actor, &error);
	else if (!g_strcmp0(action, "edit"))
		replica = venture_federation_replica_edit(self->context,
			venture_json_object_get_int(object, "id", 0),
			venture_json_object_get_int(object, "version", 0),
			json_object_get_member(object, "fields"), &actor, &error);
	if (replica) result = venture_serializable_to_json(VENTURE_SERIALIZABLE(replica), FALSE);
	if (!result && !error)
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Unknown or invalid federation operation");
	return result ? venture_web_json_response(result, 200) : venture_web_error_response(error);
}

static HtmxResponse *
venture_web_ui_federation(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) html = g_string_new("");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FEDERATION_REPLICA);
	g_autoptr(GPtrArray) rows = NULL;
	HtmxResponse *gate;
	guint i;
	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate) return gate;
	gate = venture_web_require_module_ui(self, request, "federation");
	if (gate) return gate;
	rows = venture_database_find(venture_context_get_database(self->context), query, &error);
	if (!rows) return venture_web_error_response(error);
	g_string_append(html, "<div class=\"page-head\"><h1>Federation</h1><p>Shared work, available locally. Private records stay on their home server.</p></div>"
		"<p><a class=\"btn\" href=\"/e/federation_peer\">Servers</a> "
		"<a class=\"btn\" href=\"/e/federation_grant\">Sharing grants</a></p>"
		"<h2>Local working copies</h2><table><thead><tr><th>Record</th><th>Status</th></tr></thead><tbody>");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *status = NULL;
		g_object_get(row, "name", &name, "status", &status, NULL);
		g_string_append_printf(html, "<tr><td><a href=\"/federation/replicas/%" G_GINT64_FORMAT "\">", venture_entity_get_id(row));
		venture_html_escape_append(html, name);
		g_string_append(html, "</a></td><td>");
		venture_html_escape_append(html, status);
		g_string_append(html, "</td></tr>");
	}
	g_string_append(html, "</tbody></table><h2>Import a shared record</h2>"
		"<form method=\"post\" action=\"/federation/pull\">"
		"<label>Server ID<input name=\"peer_id\" type=\"number\" min=\"1\" required></label>"
		"<label>Record type<input name=\"type\" required></label>"
		"<label>Record UUID<input name=\"uuid\" required></label>"
		"<button class=\"btn\">Import working copy</button></form>");
	return venture_web_html_response(venture_web_page(self, request, "/federation", "Federation", html->str), 200);
}

static HtmxResponse *
venture_web_ui_federation_replica(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) replica = NULL;
	g_autoptr(JsonNode) working = NULL;
	g_autoptr(JsonNode) conflicts = NULL;
	g_autoptr(GString) html = g_string_new("");
	g_autoptr(GList) names = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *status = NULL;
	gint64 id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	JsonObject *fields;
	JsonArray *writable;
	GList *it;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate) return gate;
	gate = venture_web_require_module_ui(self, request, "federation");
	if (gate) return gate;
	replica = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_FEDERATION_REPLICA, id, &error);
	if (!replica) return venture_web_error_response(error);
	g_object_get(replica, "working", &text, "name", &name, "status", &status, NULL);
	working = venture_json_parse(text, &error);
	if (!working) return venture_web_error_response(error);
	fields = json_object_get_object_member(json_node_get_object(working), "fields");
	writable = json_object_get_array_member(json_node_get_object(working), "writable");
	g_string_append(html, "<h1>");
	venture_html_escape_append(html, name);
	g_string_append(html, "</h1><p>Status: ");
	venture_html_escape_append(html, status);
	g_string_append_printf(html, "</p><form method=\"post\" action=\"/federation/replicas/%" G_GINT64_FORMAT "/sync\"><button class=\"btn\">Sync with home server</button></form>", id);
	names = json_object_get_members(fields);
	for (it = names; it; it = it->next)
	{
		const gchar *field = it->data;
		JsonNode *value = json_object_get_member(fields, field);
		g_autofree gchar *display = NULL;
		gboolean editable = FALSE;
		guint i;
		for (i = 0; i < json_array_get_length(writable); i++)
			if (!g_strcmp0(json_array_get_string_element(writable, i), field)) editable = TRUE;
		display = JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING ?
			g_strdup(json_node_get_string(value)) : venture_json_to_string(value, TRUE);
		g_string_append(html, "<section class=\"card\"><h2>");
		venture_html_escape_append(html, field);
		g_string_append(html, "</h2>");
		if (editable)
		{
			g_string_append_printf(html, "<form method=\"post\" action=\"/federation/replicas/%" G_GINT64_FORMAT "/edit\"><input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"field\" value=\"", id, venture_entity_get_version(replica));
			venture_html_escape_append(html, field);
			g_string_append(html, "\"><textarea name=\"value\">");
			venture_html_escape_append(html, display);
			g_string_append(html, "</textarea><button class=\"btn\">Save locally</button></form>");
		}
		else
		{
			g_string_append(html, "<pre>");
			venture_html_escape_append(html, display);
			g_string_append(html, "</pre>");
		}
		g_string_append(html, "</section>");
	}
	g_free(text);
	g_object_get(replica, "conflicts", &text, NULL);
	conflicts = venture_json_parse(text, NULL);
	if (conflicts && json_object_get_size(json_node_get_object(conflicts)))
	{
		g_autofree gchar *display = venture_json_to_string(conflicts, TRUE);
		g_string_append(html, "<h2>Conflicts need your decision</h2><p>Review the base, local and remote values, then save the intended value above. Nothing is pushed until every conflict is resolved.</p><pre>");
		venture_html_escape_append(html, display);
		g_string_append(html, "</pre>");
		{
			g_autoptr(GList) conflicted = json_object_get_members(json_node_get_object(conflicts));
			GList *entry;
			for (entry = conflicted; entry; entry = entry->next)
			{
				g_string_append_printf(html, "<form method=\"post\" action=\"/federation/replicas/%" G_GINT64_FORMAT "/resolve\"><input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"field\" value=\"", id, venture_entity_get_version(replica));
				venture_html_escape_append(html, entry->data);
				g_string_append(html, "\"><strong>");
				venture_html_escape_append(html, entry->data);
				g_string_append(html, "</strong> <button class=\"btn\" name=\"choice\" value=\"remote\">Use remote value</button> <button class=\"btn\" name=\"choice\" value=\"local\">Keep local value</button></form>");
			}
		}
	}
	return venture_web_html_response(venture_web_page(self, request, "/federation", name, html->str), 200);
}

static HtmxResponse *
venture_web_ui_federation_write(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) replica = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	const gchar *id_text = g_hash_table_lookup(params, "id");
	const gchar *action = g_hash_table_lookup(params, "action");
	gint64 id = id_text ? g_ascii_strtoll(id_text, NULL, 10) : 0;
	VentureActor actor;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate) return gate;
	gate = venture_web_require_module_ui(self, request, "federation");
	if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	if (!id_text)
	{
		const gchar *peer = htmx_request_get_form_value(request, "peer_id");
		replica = venture_federation_replica_pull(self->context,
			peer ? g_ascii_strtoll(peer, NULL, 10) : 0,
			htmx_request_get_form_value(request, "type"),
			htmx_request_get_form_value(request, "uuid"), &actor, &error);
	}
	else if (!g_strcmp0(action, "sync"))
		replica = venture_federation_replica_sync(self->context, id, &actor, &error);
	else if (!g_strcmp0(action, "resolve"))
	{
		const gchar *version = htmx_request_get_form_value(request, "version");
		replica = venture_federation_replica_resolve(self->context, id,
			version ? g_ascii_strtoll(version, NULL, 10) : 0,
			htmx_request_get_form_value(request, "field"),
			!g_strcmp0(htmx_request_get_form_value(request, "choice"), "local"), &actor, &error);
	}
	else if (!g_strcmp0(action, "edit"))
	{
		g_autoptr(VentureEntity) stored = NULL;
		g_autoptr(JsonNode) working = NULL;
		g_autoptr(JsonNode) changes = NULL;
		g_autoptr(JsonNode) value = NULL;
		g_autofree gchar *text = NULL;
		const gchar *field = htmx_request_get_form_value(request, "field");
		const gchar *input = htmx_request_get_form_value(request, "value");
		const gchar *version = htmx_request_get_form_value(request, "version");
		JsonNode *old;
		stored = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_FEDERATION_REPLICA, id, &error);
		if (!stored) return venture_web_error_response(error);
		g_object_get(stored, "working", &text, NULL);
		working = venture_json_parse(text, &error);
		if (!working) return venture_web_error_response(error);
		old = field ? json_object_get_member(json_object_get_object_member(json_node_get_object(working), "fields"), field) : NULL;
		if (old && JSON_NODE_HOLDS_VALUE(old) && json_node_get_value_type(old) == G_TYPE_STRING)
		{
			value = json_node_new(JSON_NODE_VALUE);
			json_node_set_string(value, input ? input : "");
		}
		else if (input)
			value = venture_json_parse(input, &error);
		if (value && field)
		{
			changes = json_node_new(JSON_NODE_OBJECT);
			json_node_take_object(changes, json_object_new());
			json_object_set_member(json_node_get_object(changes), field, g_steal_pointer(&value));
			replica = venture_federation_replica_edit(self->context, id,
				version ? g_ascii_strtoll(version, NULL, 10) : 0, changes, &actor, &error);
		}
	}
	if (!replica)
	{
		if (!error) g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Invalid federation action");
		return venture_web_error_response(error);
	}
	destination = g_strdup_printf("/federation/replicas/%" G_GINT64_FORMAT, venture_entity_get_id(replica));
	return venture_web_redirect_to(destination);
}
