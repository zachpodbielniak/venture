/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-settings-web.inc - The settings screen and its API
 * (docs/settings.org)
 *
 * Included by venture-web-server.c. Both are drawn from the registry: a
 * setting a feature registers appears here with nothing written per
 * setting, and both change it through venture_settings_set() and
 * venture_settings_reset(), the same calls venturectl reaches and the
 * same record change the assistant stages.
 */

static const gchar venture_web_settings_style[] =
	"<style>.settings-intro{max-width:46rem}"
	".settings-list{list-style:none;margin:0;padding:0}"
	".settings-item{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,22rem);gap:.5rem 1.5rem;padding:.9rem 0;"
	"border-bottom:1px solid var(--border)}"
	".settings-item:last-child{border-bottom:0}"
	".settings-item h3{margin:0 0 .25rem;font-size:1rem}"
	".settings-help{margin:.1rem 0 .35rem;color:var(--text-muted);overflow-wrap:anywhere}"
	".settings-facts{display:flex;flex-wrap:wrap;gap:.35rem .9rem;margin:0;padding:0;list-style:none;font-size:.85em}"
	".settings-facts code{overflow-wrap:anywhere;white-space:normal}"
	".settings-changed{font-weight:600}"
	".settings-forms{display:flex;flex-direction:column;gap:.45rem;align-self:start}"
	".settings-forms form{display:flex;flex-wrap:wrap;gap:.4rem;align-items:center;margin:0}"
	".settings-forms input,.settings-forms select{flex:1 1 9rem;min-width:0;box-sizing:border-box}"
	"@media (max-width:720px){.settings-item{grid-template-columns:minmax(0,1fr)}}</style>";

/* The organization a request means: the path's, else ?organization_id=,
 * else the default one. */
static gboolean
venture_web_settings_organization(VentureWebServer *self, HtmxRequest *request, GHashTable *params, gint64 *org,
	GError **error)
{
	const gchar *text = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	if (text == NULL)
		text = htmx_request_get_query_param(request, "organization_id");
	if (text == NULL) {
		*org = venture_context_get_default_organization_id(self->context);
		return TRUE;
	}
	if (!g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, org, NULL)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "organization_id is a number");
		return FALSE;
	}
	return TRUE;
}

static void
venture_web_settings_value_text(GString *html, const VentureSettingInfo *info, const gchar *value)
{
	if (info->type == VENTURE_SETTING_BOOLEAN)
		g_string_append(html, g_strcmp0(value, "true") == 0 ? "on" : "off");
	else if (venture_string_is_empty(value))
		g_string_append(html, "none");
	else {
		g_string_append(html, "<code>");
		venture_html_escape_append(html, value);
		g_string_append(html, "</code>");
	}
}

/* The control for one setting, from its type alone. */
static void
venture_web_settings_control(GString *html, const VentureSettingValue *value, const gchar *id)
{
	const VentureSettingInfo *info = value->info;
	if (info->type == VENTURE_SETTING_BOOLEAN) {
		gboolean on = g_strcmp0(value->value, "true") == 0;
		g_string_append_printf(html, "<select id=\"%s\" name=\"value\"><option value=\"true\"%s>On</option>"
			"<option value=\"false\"%s>Off</option></select>", id, on ? " selected" : "", on ? "" : " selected");
	} else if (info->type == VENTURE_SETTING_CHOICE) {
		g_auto(GStrv) choices = g_strsplit(info->choices != NULL ? info->choices : "", ",", -1);
		guint i;
		g_string_append_printf(html, "<select id=\"%s\" name=\"value\">", id);
		for (i = 0; choices[i]; i++) {
			g_string_append(html, "<option value=\"");
			venture_html_escape_append(html, choices[i]);
			g_string_append_printf(html, "\"%s>", g_strcmp0(choices[i], value->value) == 0 ? " selected" : "");
			venture_html_escape_append(html, choices[i]);
			g_string_append(html, "</option>");
		}
		g_string_append(html, "</select>");
	} else if (info->type == VENTURE_SETTING_INTEGER) {
		g_string_append_printf(html, "<input id=\"%s\" name=\"value\" type=\"number\" inputmode=\"numeric\" min=\"%" G_GINT64_FORMAT
			"\" max=\"%" G_GINT64_FORMAT "\" value=\"", id, info->minimum, info->maximum);
		venture_html_escape_append(html, value->value);
		g_string_append(html, "\">");
	} else {
		g_string_append_printf(html, "<input id=\"%s\" name=\"value\" maxlength=\"2000\" value=\"", id);
		venture_html_escape_append(html, value->value);
		g_string_append(html, "\" placeholder=\"");
		venture_html_escape_append(html, info->default_value);
		g_string_append(html, "\">");
	}
}

static void
venture_web_settings_item(GString *html, const VentureSettingValue *value)
{
	const VentureSettingInfo *info = value->info;
	g_autofree gchar *id = g_strdup(value->key);
	g_strdelimit(id, ".", '-');
	g_string_append_printf(html, "<li class=\"settings-item\" id=\"setting-%s\"><div><h3><label for=\"input-%s\">", id, id);
	venture_html_escape_append(html, info->label);
	if (value->member_label != NULL) {
		g_string_append(html, ": ");
		venture_html_escape_append(html, value->member_label);
	}
	g_string_append(html, "</label></h3><p class=\"settings-help\">");
	venture_html_escape_append(html, info->help);
	g_string_append(html, "</p><ul class=\"settings-facts\"><li");
	if (!value->is_default)
		g_string_append(html, " class=\"settings-changed\"");
	g_string_append(html, ">Now: ");
	venture_web_settings_value_text(html, info, value->value);
	if (!value->is_default)
		g_string_append(html, " (changed)");
	g_string_append(html, "</li><li>Default: ");
	venture_web_settings_value_text(html, info, info->default_value);
	g_string_append(html, "</li>");
	if (info->type == VENTURE_SETTING_INTEGER)
		g_string_append_printf(html, "<li>%" G_GINT64_FORMAT " to %" G_GINT64_FORMAT "</li>", info->minimum, info->maximum);
	g_string_append(html, "<li class=\"muted\"><code>");
	venture_html_escape_append(html, value->key);
	g_string_append(html, "</code></li></ul></div><div class=\"settings-forms\"><form method=\"post\">"
		"<input type=\"hidden\" name=\"action\" value=\"set\"><input type=\"hidden\" name=\"key\" value=\"");
	venture_html_escape_append(html, value->key);
	g_string_append(html, "\">");
	{
		g_autofree gchar *input = g_strdup_printf("input-%s", id);
		venture_web_settings_control(html, value, input);
	}
	g_string_append(html, "<button class=\"btn btn-primary\" type=\"submit\">Save</button></form>"
		"<form method=\"post\"><input type=\"hidden\" name=\"action\" value=\"reset\"><input type=\"hidden\" name=\"key\" value=\"");
	venture_html_escape_append(html, value->key);
	g_string_append_printf(html, "\"><button class=\"btn\" type=\"submit\"%s>Reset to default</button></form></div></li>",
		value->is_default ? " disabled" : "");
}

/* GET and POST /organizations/:id/settings/workflows: an owner's or
 * administrator's. */
static HtmxResponse *
venture_web_settings_screen(HtmxRequest *request, GHashTable *params, gpointer data)
{
	static const gint managers[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureAccessPolicy *policy = venture_database_get_access_policy(db);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GString) html = g_string_new(venture_web_settings_style);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) values = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autofree gchar *self_path = NULL, *name = NULL;
	const gchar *notice = htmx_request_get_query_param(request, "notice");
	const gchar *problem = htmx_request_get_query_param(request, "problem");
	const gchar *workflow = NULL;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	gint64 org = 0;
	guint i;
	if (gate != NULL) return gate;
	if (!venture_web_settings_organization(self, request, params, &org, &error))
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(policy, principal, org, managers, G_N_ELEMENTS(managers))) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"These settings need an owner or administrator of the organization");
		return venture_web_error_response(error);
	}
	scope = venture_access_policy_enter(policy, principal);
	self_path = g_strdup_printf("/organizations/%" G_GINT64_FORMAT "/settings/workflows", org);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		const gchar *action = htmx_request_get_form_value(request, "action");
		const gchar *key = htmx_request_get_form_value(request, "key");
		g_autoptr(VentureSettingValue) changed = NULL;
		g_autofree gchar *escaped = NULL, *back = NULL, *anchor = g_strdup(key != NULL ? key : "");
		HtmxResponse *response;
		VentureActor actor;
		venture_auth_to_actor(principal, &actor);
		if (g_strcmp0(action, "reset") == 0)
			changed = venture_settings_reset(self->context, org, key, &actor, &error);
		else
			changed = venture_settings_set(self->context, org, key, htmx_request_get_form_value(request, "value"), &actor, &error);
		g_strdelimit(anchor, ".", '-');
		escaped = g_uri_escape_string(changed != NULL ? (g_strcmp0(action, "reset") == 0 ? "Back to its default." : "Saved.")
			: (error != NULL ? error->message : "Not saved"), NULL, FALSE);
		back = g_strdup_printf("%s?%s=%s#setting-%s", self_path, changed != NULL ? "notice" : "problem", escaped, anchor);
		response = venture_web_redirect_to(back);
		htmx_response_set_status(response, 303);
		return response;
	}
	organization = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, org, NULL);
	values = venture_settings_list(self->context, org, &error);
	if (organization == NULL || values == NULL) {
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Organization not found");
		return venture_web_error_response(error);
	}
	g_object_get(organization, "name", &name, NULL);
	/* One block, so the page arrives whole rather than card by card. */
	g_string_append(html, "<div class=\"settings-page\">");
	g_string_append_printf(html, "<div class=\"page-head\"><div class=\"page-title\"><p><a href=\"/e/organization/%" G_GINT64_FORMAT
		"\">&larr; ", org);
	venture_html_escape_append(html, name != NULL ? name : "Organization");
	g_string_append(html, "</a></p><h1>Marketing, sales, phone and AI</h1></div></div>"
		"<p class=\"settings-intro\">How email, campaigns, sales, phone and text, web chat and AI on messages behave for this "
		"organization. Each setting shows what it is now and its default; resetting puts the default back. Every change is "
		"recorded in the audit log with who made it. The assistant can change these too: it proposes the change and you approve it.</p>");
	if (!venture_string_is_empty(notice)) {
		g_string_append(html, "<div class=\"notice positive\" role=\"status\"><div>");
		venture_html_escape_append(html, notice);
		g_string_append(html, "</div></div>");
	}
	if (!venture_string_is_empty(problem)) {
		g_string_append(html, "<div class=\"notice negative\" role=\"alert\"><div>");
		venture_html_escape_append(html, problem);
		g_string_append(html, "</div></div>");
	}
	for (i = 0; i < values->len; i++) {
		VentureSettingValue *value = g_ptr_array_index(values, i);
		if (g_strcmp0(workflow, value->info->workflow) != 0) {
			if (workflow != NULL)
				g_string_append(html, "</ul></div></section>");
			workflow = value->info->workflow;
			g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>");
			venture_html_escape_append(html, workflow);
			g_string_append(html, "</h2></div><div class=\"card-body\"><ul class=\"settings-list\">");
		}
		venture_web_settings_item(html, value);
	}
	if (workflow != NULL)
		g_string_append(html, "</ul></div></section>");
	g_string_append(html, "<p class=\"muted\">Every setting, its default and its limit, and how to change it from chat, the API "
		"or venturectl: <a href=\"/docs/settings.html\">Settings</a>.</p></div>");
	return venture_web_html_response(venture_web_page(self, request, "/e/organization", "Marketing, sales, phone and AI", html->str), 200);
}

/* Anybody in the organization reads its settings; changing one is the
 * registration's (owner or administrator). */
static const gint venture_web_settings_readers[] = {
	VENTURE_ORGANIZATION_ROLE_VIEWER, VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN,
	VENTURE_ORGANIZATION_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_FINANCE, VENTURE_ORGANIZATION_ROLE_SALES,
	VENTURE_ORGANIZATION_ROLE_SUPPORT, VENTURE_ORGANIZATION_ROLE_ACCOUNTANT
};

/* GET /api/v1/workflow-settings, GET and PUT /api/v1/workflow-settings/:key. A PUT carries
 * {"value": ...} or {"reset": true}. */
static HtmxResponse *
venture_web_settings_api(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *key = g_hash_table_lookup(params, "key");
	HtmxResponse *gate;
	gint64 org = 0;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER)) != NULL) return gate;
	if (!venture_web_settings_organization(self, request, NULL, &org, &error))
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(db), principal, org,
		venture_web_settings_readers, G_N_ELEMENTS(venture_web_settings_readers))) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Organization not found");
		return venture_web_error_response(error);
	}
	scope = venture_access_policy_enter(venture_database_get_access_policy(db), principal);
	if (key == NULL) {
		g_autoptr(GPtrArray) values = venture_settings_list(self->context, org, &error);
		g_autoptr(JsonArray) array = json_array_new();
		g_autoptr(JsonObject) object = json_object_new();
		g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
		guint i;
		if (values == NULL)
			return venture_web_error_response(error);
		for (i = 0; i < values->len; i++)
			json_array_add_element(array, venture_setting_value_to_json(g_ptr_array_index(values, i)));
		json_object_set_int_member(object, "organization_id", org);
		json_object_set_array_member(object, "items", g_steal_pointer(&array));
		json_node_set_object(root, g_steal_pointer(&object));
		return venture_web_json_response(root, 200);
	}
	{
		g_autoptr(VentureSettingValue) value = NULL;
		g_autoptr(JsonNode) node = NULL;
		if (htmx_request_get_method(request) == HTMX_METHOD_PUT) {
			g_autoptr(JsonNode) body = htmx_request_get_json(request, NULL);
			JsonObject *object = body != NULL && JSON_NODE_HOLDS_OBJECT(body) ? json_node_get_object(body) : NULL;
			VentureActor actor;
			venture_auth_to_actor(principal, &actor);
			if (object != NULL && json_object_has_member(object, "reset") && json_object_get_boolean_member(object, "reset"))
				value = venture_settings_reset(self->context, org, key, &actor, &error);
			else {
				g_autofree gchar *text = object != NULL && json_object_has_member(object, "value")
					? venture_settings_value_from_json(json_object_get_member(object, "value")) : NULL;
				if (text == NULL) {
					g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
						"Send {\"value\": ...} with a boolean, a number or text, or {\"reset\": true}");
					return venture_web_error_response(error);
				}
				value = venture_settings_set(self->context, org, key, text, &actor, &error);
			}
		} else
			value = venture_settings_get(self->context, org, key, &error);
		if (value == NULL)
			return venture_web_error_response(error);
		node = venture_setting_value_to_json(value);
		return venture_web_json_response(node, 200);
	}
}

/* On an organization's page: the way to its settings. */
static void
venture_web_settings_block(VentureWebServer *self, GString *content, VentureEntity *record)
{
	(void)self;
	if (G_OBJECT_TYPE(record) != VENTURE_TYPE_ORGANIZATION)
		return;
	g_string_append_printf(content, "<p><a class=\"btn\" href=\"/organizations/%" G_GINT64_FORMAT "/settings/workflows\">"
		"Marketing, sales, phone and AI</a> <span class=\"muted\">Defaults for email, campaigns, sales, phone and text, web chat "
		"and AI on messages.</span></p>", venture_entity_get_id(record));
}

static void
venture_settings_pages_register(VentureWebServer *self)
{
	static const struct {
		HtmxMethod method;
		const gchar *path;
		HtmxRouteCallback handler;
	} routes[] = {
		{ HTMX_METHOD_GET, "/organizations/:id/settings/workflows", venture_web_settings_screen },
		{ HTMX_METHOD_POST, "/organizations/:id/settings/workflows", venture_web_settings_screen },
		{ HTMX_METHOD_GET, "/api/v1/workflow-settings", venture_web_settings_api },
		{ HTMX_METHOD_GET, "/api/v1/workflow-settings/:key", venture_web_settings_api },
		{ HTMX_METHOD_PUT, "/api/v1/workflow-settings/:key", venture_web_settings_api },
	};
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(routes); i++)
		venture_web_server_add_classified_route(self, routes[i].method, routes[i].path,
			VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, routes[i].handler, self);
}
