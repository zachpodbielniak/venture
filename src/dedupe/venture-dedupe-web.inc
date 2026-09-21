/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The Duplicates page under Relations: open candidates side by side, a merge
 * button each way, a dismissal, and the scan buttons. Every button calls the
 * dedupe service; no rule lives here. Also the forwarding a merged record's
 * old id gets from the generic record routes.
 */
static HtmxResponse *
venture_web_dedupe_forward(
	VentureWebServer	*self,
	VentureEntity		*record,
	const gchar		*prefix
){
	HtmxResponse *response;
	g_autofree gchar *path = NULL;
	gint64 survivor = venture_dedupe_merged_into(venture_context_get_database(self->context), record);

	if (survivor <= 0)
		return NULL;

	/* 301, deliberately: the old id will never come back, and a client
	 * that caches the redirect is right to. */
	path = g_strdup_printf("%s/%s/%" G_GINT64_FORMAT, prefix,
		venture_entity_get_entity_name(record), survivor);
	response = htmx_response_new();
	htmx_response_set_status(response, 301);
	htmx_response_add_header(response, "Location", path);

	return response;
}

static void
venture_web_dedupe_append_pair(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*candidate
){
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autofree gchar *kind = NULL, *reasons = NULL, *label_a = NULL, *label_b = NULL;
	g_autoptr(VentureEntity) a = NULL, b = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	gint64 id, record_a = 0, record_b = 0, score = 0;
	GType type;
	guint i;

	id = venture_entity_get_id(candidate);
	g_object_get(candidate, "kind", &kind, "reasons", &reasons, "label-a", &label_a, "label-b", &label_b,
		"record-a", &record_a, "record-b", &record_b, "score", &score, NULL);
	type = venture_entity_registry_lookup(venture_context_get_entity_registry(self->context), kind);
	if (G_TYPE_INVALID != type)
	{
		a = venture_database_get(db, type, record_a, NULL);
		b = venture_database_get(db, type, record_b, NULL);
	}

	g_string_append(content, "<div class=\"card duplicate-pair\"><div class=\"card-head\"><h2>");
	venture_html_escape_append(content, kind);
	g_string_append(content, ": ");
	venture_html_escape_append(content, label_a);
	g_string_append(content, " ~ ");
	venture_html_escape_append(content, label_b);
	g_string_append_printf(content, "</h2><span class=\"badge\">score %" G_GINT64_FORMAT "</span> <span class=\"muted\">", score);
	venture_html_escape_append(content, reasons ? reasons : "");
	g_string_append(content, "</span></div><div class=\"card-body\">");

	if ((NULL == a) || (NULL == b) || venture_entity_is_deleted(a) || venture_entity_is_deleted(b))
	{
		g_string_append(content, "<p class=\"muted\">One side of this pair is gone; rescan.</p></div></div>");
		return;
	}

	g_string_append_printf(content, "<table class=\"table\"><thead><tr><th>Field</th>"
		"<th><a href=\"/e/%s/%" G_GINT64_FORMAT "\">#%" G_GINT64_FORMAT "</a></th>"
		"<th><a href=\"/e/%s/%" G_GINT64_FORMAT "\">#%" G_GINT64_FORMAT "</a></th></tr></thead><tbody>",
		kind, record_a, record_a, kind, record_b, record_b);
	specs = venture_entity_get_field_specs(a);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);
	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		const gchar *name = venture_field_spec_get_name(spec);
		g_autofree gchar *left = NULL, *right = NULL;

		if (0 != (venture_field_spec_get_flags(spec) & (VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_TRANSIENT)))
			continue;
		if (0 == g_strcmp0(name, "merged-into-id"))
			continue;
		left = venture_dedupe_field_text(a, name);
		right = venture_dedupe_field_text(b, name);
		if ((NULL == left) && (NULL == right))
			continue;
		g_string_append_printf(content, "<tr%s><td class=\"field-label\">",
			(0 != g_strcmp0(left, right)) && (NULL != left) && (NULL != right) ? " class=\"conflict\"" : "");
		venture_html_escape_append(content, venture_field_spec_get_label(spec));
		g_string_append(content, "</td><td>");
		venture_html_escape_append(content, left ? left : "");
		g_string_append(content, "</td><td>");
		venture_html_escape_append(content, right ? right : "");
		g_string_append(content, "</td></tr>");
	}
	g_string_append(content, "</tbody></table><div class=\"page-actions\">");
	g_string_append_printf(content,
		"<form method=\"post\" action=\"/customers/duplicates/%" G_GINT64_FORMAT "/merge\" style=\"display:inline\">"
		"<input type=\"hidden\" name=\"survivor\" value=\"%" G_GINT64_FORMAT "\">"
		"<button class=\"btn btn-primary\" type=\"submit\">Keep #%" G_GINT64_FORMAT ", merge #%" G_GINT64_FORMAT " into it</button></form> ",
		id, record_a, record_a, record_b);
	g_string_append_printf(content,
		"<form method=\"post\" action=\"/customers/duplicates/%" G_GINT64_FORMAT "/merge\" style=\"display:inline\">"
		"<input type=\"hidden\" name=\"survivor\" value=\"%" G_GINT64_FORMAT "\">"
		"<button class=\"btn btn-primary\" type=\"submit\">Keep #%" G_GINT64_FORMAT ", merge #%" G_GINT64_FORMAT " into it</button></form> ",
		id, record_b, record_b, record_a);
	g_string_append_printf(content,
		"<form method=\"post\" action=\"/customers/duplicates/%" G_GINT64_FORMAT "/dismiss\" style=\"display:inline\">"
		"<button class=\"btn\" type=\"submit\">Not a duplicate</button></form>", id);
	g_string_append(content, "</div></div></div>");
}

/*
 * GET /customers/duplicates - the open proposals for the active organization.
 */
static HtmxResponse *
venture_web_ui_duplicates(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) candidates = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	gint64 organization_id;
	guint i;

	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (NULL != gate)
		return gate;
	gate = venture_web_require_module_ui(self, request, "dedupe");
	if (NULL != gate)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_require_for_type(self, principal, VENTURE_TYPE_DUPLICATE_CANDIDATE,
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	organization_id = venture_web_active_organization(self, request);
	if (0 == organization_id)
		organization_id = venture_context_get_default_organization_id(self->context);

	query = venture_query_new(VENTURE_TYPE_DUPLICATE_CANDIDATE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "open", NULL);
	venture_query_add_order(query, "score", VENTURE_SORT_DESCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 200);
	candidates = venture_database_find(venture_context_get_database(self->context), query, &error);
	if (NULL == candidates)
		return venture_web_error_response(error);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\"><h1>Duplicates</h1>"
		"<span class=\"subtitle\">Companies and contacts that look like one record. A scan proposes; you merge.</span>"
		"</div><div class=\"page-actions\">"
		"<form method=\"post\" action=\"/customers/duplicates/scan\" style=\"display:inline\">"
		"<input type=\"hidden\" name=\"kind\" value=\"company\">"
		"<button class=\"btn\" type=\"submit\">Scan companies</button></form> "
		"<form method=\"post\" action=\"/customers/duplicates/scan\" style=\"display:inline\">"
		"<input type=\"hidden\" name=\"kind\" value=\"contact\">"
		"<button class=\"btn\" type=\"submit\">Scan contacts</button></form>"
		"</div></div>");
	if (0 == candidates->len)
		g_string_append(content, "<div class=\"card\"><div class=\"card-body\"><p class=\"muted\">"
			"No duplicates proposed. Run a scan, or every pair has been merged or dismissed.</p></div></div>");
	for (i = 0; i < candidates->len; i++)
		venture_web_dedupe_append_pair(self, content, g_ptr_array_index(candidates, i));

	return venture_web_html_response(
		venture_web_page(self, request, "/customers/duplicates", "Duplicates", content->str), 200);
}

/*
 * POST /customers/duplicates/scan, /customers/duplicates/:id/merge and
 * /customers/duplicates/:id/dismiss - the page's buttons.
 */
static HtmxResponse *
venture_web_ui_duplicates_action(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) candidate = NULL;
	g_autoptr(VentureEntity) survivor = NULL;
	g_autoptr(GError) error = NULL;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureDedupeService *service = venture_dedupe_service_get(db);
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *action = g_hash_table_lookup(params, "action");
	const gchar *id_text = g_hash_table_lookup(params, "id");

	gate = venture_web_ui_require_session(self, request);
	if (NULL != gate)
		return gate;
	gate = venture_web_require_module_ui(self, request, "dedupe");
	if (NULL != gate)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	if (!venture_web_require_for_type(self, principal, VENTURE_TYPE_DUPLICATE_CANDIDATE,
	                                  VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);

	if (NULL == id_text)
	{
		const gchar *kind = htmx_request_get_form_value(request, "kind");
		gint64 organization_id = venture_web_active_organization(self, request);

		if (0 == organization_id)
			organization_id = venture_context_get_default_organization_id(self->context);
		if (venture_dedupe_service_scan(service, organization_id,
			venture_string_is_empty(kind) ? "company" : kind, &actor, &error) < 0)
			return venture_web_error_response(error);
		return venture_web_redirect_to("/customers/duplicates");
	}

	candidate = venture_database_get(db, VENTURE_TYPE_DUPLICATE_CANDIDATE,
	                                 g_ascii_strtoll(id_text, NULL, 10), &error);
	if (NULL == candidate)
	{
		if (NULL == error)
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such candidate: %s", id_text);
		return venture_web_error_response(error);
	}
	if (0 == g_strcmp0(action, "merge"))
	{
		const gchar *survivor_text = htmx_request_get_form_value(request, "survivor");

		survivor = venture_dedupe_service_merge(service, candidate,
			g_ascii_strtoll(survivor_text ? survivor_text : "0", NULL, 10), &actor, &error);
		if (NULL == survivor)
			return venture_web_error_response(error);
	}
	else if (0 == g_strcmp0(action, "dismiss"))
	{
		if (!venture_dedupe_service_dismiss(service, candidate, &actor, &error))
			return venture_web_error_response(error);
	}
	else
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such action: %s", action);
		return venture_web_error_response(error);
	}

	return venture_web_redirect_to("/customers/duplicates");
}
