/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included beside the web adapters. Phone and text (docs/telephony.org):
 * the telephony settings page, the Call button on contacts, deals and
 * tickets, a contact's text thread and composer, their API twins and the
 * inbound webhook. Every page and API route checks its own session or
 * role; the webhook has none by design and is held to its address token,
 * its allowed senders and the provider's signature where there is one. */

static const gint venture_web_telephony_managers[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };

static const gchar venture_web_telephony_style[] =
	"<style>.phone-card .row{display:flex;flex-wrap:wrap;gap:.5rem;align-items:center}"
	".text-thread{list-style:none;margin:0 0 1rem;padding:0;display:flex;flex-direction:column;gap:.5rem}"
	".text-thread li{max-width:85%;padding:.5rem .75rem;border-radius:.75rem;border:1px solid var(--border);overflow-wrap:anywhere}"
	".text-thread .in{align-self:flex-start;background:var(--surface-sunken)}"
	".text-thread .out{align-self:flex-end;background:var(--accent-soft)}"
	".text-thread .meta{display:block;font-size:.8em;color:var(--text-muted);margin-top:.25rem}"
	".phone-card textarea{width:100%;box-sizing:border-box}"
	".telephony-settings table{width:100%}.telephony-settings code{overflow-wrap:anywhere}</style>";

static gboolean
venture_web_telephony_is_manager(VentureWebServer *self, const VentureAuthPrincipal *principal, gint64 org)
{
	return venture_access_policy_has_organization_role(venture_database_get_access_policy(venture_context_get_database(self->context)),
		principal, org, venture_web_telephony_managers, G_N_ELEMENTS(venture_web_telephony_managers));
}

static void
venture_web_telephony_when(VentureWebServer *self, GString *html, GDateTime *when)
{
	g_autoptr(GDateTime) local = NULL;
	g_autofree gchar *text = NULL;
	if (when == NULL)
		return;
	local = g_date_time_to_timezone(when, venture_context_get_timezone(self->context));
	text = g_date_time_format(local, "%Y-%m-%d %H:%M");
	venture_html_escape_append(html, text);
}

/* --- The settings page ---------------------------------------------------------- */

static void
venture_web_telephony_lines(VentureWebServer *self, GString *html, gint64 org)
{
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TELEPHONY_ACCOUNT);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(db, query, NULL);
	g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>Phone lines</h2></div><div class=\"card-body\">");
	if (rows == NULL || rows->len == 0)
	{
		g_string_append(html, "<p>No line is connected yet. Connect your 3CX phone system for click-to-call and call logging, "
			"and a VoIP.ms number for texts.</p></div></section>");
		return;
	}
	g_string_append(html, "<div class=\"table-wrap\"><table><thead><tr><th>Line</th><th>Provider</th><th>Number</th>"
		"<th>Can</th><th>State</th><th></th></tr></thead><tbody>");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = NULL, *provider = NULL, *phone = NULL, *problem = NULL;
		g_autoptr(VentureTelephonyConnector) connector = NULL;
		gboolean active = FALSE;
		VentureTelephonyCapabilities can = 0;
		g_object_get(row, "name", &name, "provider", &provider, "phone-number", &phone, "active", &active, "last-error", &problem, NULL);
		connector = venture_telephony_provider_new(provider);
		if (connector != NULL)
			can = venture_telephony_connector_get_capabilities(connector);
		g_string_append(html, "<tr><td>");
		venture_html_escape_append(html, name);
		g_string_append(html, "</td><td>");
		venture_html_escape_append(html, connector != NULL ? venture_telephony_connector_get_label(connector) : provider);
		g_string_append(html, "</td><td>");
		venture_html_escape_append(html, !venture_string_is_empty(phone) ? phone : "—");
		g_string_append_printf(html, "</td><td>%s%s%s%s</td><td>%s",
			(can & VENTURE_TELEPHONY_CAPABILITY_CLICK_TO_CALL) ? "calls " : "",
			(can & VENTURE_TELEPHONY_CAPABILITY_SMS) ? "texts " : "",
			(can & VENTURE_TELEPHONY_CAPABILITY_MMS) ? "pictures " : "",
			(can & VENTURE_TELEPHONY_CAPABILITY_DELIVERY_STATUS) ? "receipts" : "",
			active ? "Connected" : "Disconnected");
		if (!venture_string_is_empty(problem))
		{
			g_string_append(html, "<br><span class=\"muted\">Last refusal: ");
			venture_html_escape_append(html, problem);
			g_string_append(html, "</span>");
		}
		g_string_append(html, "</td><td>");
		if (active)
			g_string_append_printf(html, "<form method=\"post\" class=\"row\"><input type=\"hidden\" name=\"account_id\" value=\"%" G_GINT64_FORMAT "\">"
				"<button class=\"btn\" name=\"operation\" value=\"rotate\">New webhook address</button>"
				"<button class=\"btn\" name=\"operation\" value=\"disconnect\">Disconnect</button></form>", venture_entity_get_id(row));
		g_string_append(html, "</td></tr>");
	}
	g_string_append(html, "</tbody></table></div><p class=\"muted\">Each line's webhook address is shown once, when it is made. "
		"Making a new one stops the old one at once.</p></div></section>");
}

static void
venture_web_telephony_connect_forms(GString *html)
{
	g_auto(GStrv) ids = venture_telephony_provider_ids();
	guint i;
	g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>Connect a line</h2></div><div class=\"card-body\">"
		"<p>Credentials are encrypted in the organization's vault and never shown again. To rotate them, connect the same provider again "
		"with the complete settings.</p>");
	for (i = 0; ids[i] != NULL; i++)
	{
		g_autoptr(VentureTelephonyConnector) connector = venture_telephony_provider_new(ids[i]);
		g_autoptr(JsonNode) schema = venture_telephony_connector_settings_schema(connector);
		VentureTelephonyCapabilities can = venture_telephony_connector_get_capabilities(connector);
		const gchar *base = venture_telephony_connector_default_base_url(connector);
		g_string_append(html, "<details><summary>");
		venture_html_escape_append(html, venture_telephony_connector_get_label(connector));
		g_string_append(html, "</summary><form method=\"post\"><input type=\"hidden\" name=\"operation\" value=\"connect\">"
			"<input type=\"hidden\" name=\"provider\" value=\"");
		venture_html_escape_append(html, ids[i]);
		g_string_append(html, "\"><label>Name<input name=\"name\" autocomplete=\"off\"></label>");
		g_string_append(html, "<label>Address<input name=\"base_url\" autocomplete=\"off\" placeholder=\"");
		venture_html_escape_append(html, base != NULL ? base : "https://pbx.example.com");
		g_string_append(html, "\"></label>");
		if (can & VENTURE_TELEPHONY_CAPABILITY_SMS)
			g_string_append(html, "<label>Texting number<input name=\"phone_number\" autocomplete=\"off\" placeholder=\"+15550100100\"></label>"
				"<label>Picture hosts<textarea name=\"media_hosts\" rows=\"2\" placeholder=\"one host per line\"></textarea></label>");
		venture_integration_web_fields(html, schema);
		g_string_append(html, "<label>Allowed senders<textarea name=\"webhook_sources\" rows=\"2\" placeholder=\"192.0.2.10 or 192.0.2.0/24, one per line\"></textarea></label>");
		if (!(can & VENTURE_TELEPHONY_CAPABILITY_SIGNED_WEBHOOK))
			g_string_append(html, "<p class=\"muted\">This provider does not sign what it sends, so the webhook address's long secret token, "
				"and the allowed senders if you list them, are what stop anyone else posting to it.</p>");
		g_string_append(html, "<button class=\"btn btn-primary\" type=\"submit\">Connect</button></form></details>");
	}
	g_string_append(html, "</div></section>");
}

static void
venture_web_telephony_extensions(VentureWebServer *self, GString *html, gint64 org)
{
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TELEPHONY_EXTENSION);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "extension", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(db, query, NULL);
	g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>Extensions</h2></div><div class=\"card-body\">"
		"<p>Click-to-call rings a person's own extension first and dials the contact once they pick up, so each person who calls needs theirs here.</p>");
	if (rows != NULL && rows->len > 0)
	{
		g_string_append(html, "<ul>");
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			g_autoptr(VentureEntity) user = NULL;
			g_autofree gchar *extension = NULL, *username = NULL;
			gint64 user_id = 0;
			g_object_get(row, "extension", &extension, "user-id", &user_id, NULL);
			user = venture_database_get(db, VENTURE_TYPE_USER, user_id, NULL);
			if (user != NULL)
				g_object_get(user, "username", &username, NULL);
			g_string_append(html, "<li>");
			venture_html_escape_append(html, username != NULL ? username : "(removed person)");
			g_string_append(html, " — extension ");
			venture_html_escape_append(html, extension);
			g_string_append(html, "</li>");
		}
		g_string_append(html, "</ul>");
	}
	g_string_append(html, "<form method=\"post\" class=\"row\"><input type=\"hidden\" name=\"operation\" value=\"extension\">"
		"<label>Username<input name=\"username\" autocomplete=\"off\" required></label>"
		"<label>Extension<input name=\"extension\" inputmode=\"numeric\" autocomplete=\"off\" required></label>"
		"<button class=\"btn\" type=\"submit\">Save extension</button></form></div></section>");
}

static void
venture_web_telephony_rules(GString *html, VentureEntity *organization)
{
	g_autofree gchar *country = NULL, *quiet = NULL;
	gboolean recording = FALSE;
	g_object_get(organization, "telephony-country", &country, "telephony-quiet-hours", &quiet,
		"telephony-recording-links", &recording, NULL);
	g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>Texting and call rules</h2></div><div class=\"card-body\">"
		"<form method=\"post\"><input type=\"hidden\" name=\"operation\" value=\"rules\">"
		"<label>Phone country<input name=\"telephony_country\" maxlength=\"2\" autocomplete=\"off\" placeholder=\"US\" value=\"");
	venture_html_escape_append(html, country != NULL ? country : "");
	g_string_append(html, "\"></label><p class=\"muted\">Numbers written without a country code are read as this country's.</p>"
		"<label>Text quiet hours<input name=\"telephony_quiet_hours\" autocomplete=\"off\" placeholder=\"" VENTURE_TELEPHONY_QUIET_HOURS "\" value=\"");
	venture_html_escape_append(html, quiet != NULL ? quiet : "");
	g_string_append_printf(html, "\"></label><p class=\"muted\">No text is sent between these times in the recipient's own time zone "
		"(the contact's, else this organization's); texts wait until the quiet hours end. Empty is " VENTURE_TELEPHONY_QUIET_HOURS "; off turns them off.</p>"
		"<label><input type=\"checkbox\" name=\"telephony_recording_links\" value=\"1\"%s> Keep call recording links</label>"
		"<p class=\"muted\"><strong>Recording note.</strong> Recording a call is legal in some places only when everyone on the call "
		"agrees, and some places require you to say so at the start. Venture never records and never fetches a recording; this only keeps "
		"the phone system's link on the logged call. Turn it on only if your phone system announces recording and you are allowed to keep it.</p>"
		"<p class=\"muted\"><strong>Consent.</strong> Venture sends a text only to someone whose latest text consent is an agreement. "
		"STOP, STOPALL, UNSUBSCRIBE, CANCEL, END and QUIT withdraw it; START and UNSTOP give it again; HELP and INFO get a short answer.</p>"
		"<button class=\"btn btn-primary\" type=\"submit\">Save rules</button></form></div></section>", recording ? " checked" : "");
}

static HtmxResponse *
venture_web_telephony_settings(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureTelephonyService *service = venture_telephony_service_get(db);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GString) html = g_string_new(venture_web_telephony_style);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *shown_token = NULL;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	VentureActor actor;
	gint64 org = 0;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "telephony");
	if (gate != NULL) return gate;
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &org, &error))
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_telephony_is_manager(self, principal, org))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Telephony settings need an owner or administrator of the organization");
		return venture_web_error_response(error);
	}
	organization = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, org, &error);
	if (organization == NULL || venture_entity_is_deleted(organization))
	{
		if (error == NULL) g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Organization is unavailable");
		return venture_web_error_response(error);
	}
	venture_auth_to_actor(principal, &actor);
	g_string_append(html, "<div class=\"telephony-settings\"><h1>Phone and text</h1>");
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *account_text = htmx_request_get_form_value(request, "account_id");
		gint64 account_id = account_text != NULL ? g_ascii_strtoll(account_text, NULL, 10) : 0;
		g_autoptr(VentureEntity) account = account_id > 0 ? venture_database_get(db, VENTURE_TYPE_TELEPHONY_ACCOUNT, account_id, NULL) : NULL;
		if (account != NULL && venture_entity_get_organization_id(account) != org)
			g_clear_object(&account);
		if (g_strcmp0(operation, "connect") == 0)
		{
			const gchar *provider = htmx_request_get_form_value(request, "provider");
			g_autoptr(VentureTelephonyConnector) connector = venture_telephony_provider_new(provider);
			g_autoptr(JsonObject) values = NULL;
			g_autoptr(VentureEntity) line = NULL;
			if (connector == NULL)
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose a phone provider");
			else
			{
				g_autoptr(JsonNode) schema = venture_telephony_connector_settings_schema(connector);
				static const gchar *const plain[] = { "name", "base_url", "phone_number", "webhook_sources", "media_hosts" };
				gsize i;
				values = venture_integration_web_values(request, schema, &error);
				for (i = 0; values != NULL && i < G_N_ELEMENTS(plain); i++)
				{
					const gchar *value = htmx_request_get_form_value(request, plain[i]);
					if (value != NULL && *value != '\0')
						json_object_set_string_member(values, plain[i], value);
				}
				if (values != NULL)
				{
					g_autoptr(VentureEntity) existing = venture_telephony_service_line(service, org, VENTURE_TELEPHONY_CAPABILITY_NONE, NULL);
					gint64 same = 0;
					/* Connecting the same provider again rotates that line. */
					if (existing != NULL)
					{
						g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TELEPHONY_ACCOUNT);
						g_autoptr(VentureEntity) match = NULL;
						venture_query_set_organization(query, org);
						venture_query_add_filter_string(query, "provider", VENTURE_FILTER_OP_EQ, provider, NULL);
						venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
						match = venture_database_find_one(db, query, NULL);
						same = match != NULL ? venture_entity_get_id(match) : 0;
					}
					line = venture_telephony_service_connect(service, org, provider, values, same, &actor, &shown_token, &error);
				}
				if (line != NULL)
					g_string_append(html, "<div class=\"notice positive\"><div>Line saved.</div></div>");
			}
		}
		else if (g_strcmp0(operation, "rotate") == 0 && account != NULL)
			shown_token = venture_telephony_service_new_webhook_token(service, account, &actor, &error);
		else if (g_strcmp0(operation, "disconnect") == 0 && account != NULL)
		{
			if (venture_telephony_service_disconnect(service, account, &actor, &error))
				g_string_append(html, "<div class=\"notice positive\"><div>Disconnected. Logged calls and texts stay.</div></div>");
		}
		else if (g_strcmp0(operation, "extension") == 0)
		{
			const gchar *username = htmx_request_get_form_value(request, "username");
			const gchar *extension = htmx_request_get_form_value(request, "extension");
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
			g_autoptr(VentureEntity) user = NULL, row = NULL;
			venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, username != NULL ? username : "", NULL);
			user = venture_database_find_one(db, query, NULL);
			if (user == NULL)
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No person has that username");
			else
			{
				g_autoptr(VentureQuery) existing = venture_query_new(VENTURE_TYPE_TELEPHONY_EXTENSION);
				venture_query_set_organization(existing, org);
				venture_query_add_filter_int(existing, "user-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(user), NULL);
				row = venture_database_find_one(db, existing, NULL);
				if (row == NULL)
					row = g_object_new(VENTURE_TYPE_TELEPHONY_EXTENSION, "organization-id", org, "user-id", venture_entity_get_id(user), NULL);
				g_object_set(row, "extension", extension, NULL);
				if (venture_database_save(db, row, &actor, &error))
					g_string_append(html, "<div class=\"notice positive\"><div>Extension saved.</div></div>");
			}
		}
		else if (g_strcmp0(operation, "rules") == 0)
		{
			const gchar *country = htmx_request_get_form_value(request, "telephony_country");
			const gchar *quiet = htmx_request_get_form_value(request, "telephony_quiet_hours");
			g_autofree gchar *upper = country != NULL ? g_ascii_strup(g_strstrip(g_strdup(country)), -1) : g_strdup("");
			if (*upper != '\0' && venture_phone_country_code(upper) == NULL)
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Venture does not know that country's calling code; write numbers with their + code instead");
			else
			{
				g_object_set(organization, "telephony-country", upper, "telephony-quiet-hours", quiet != NULL ? quiet : "",
					"telephony-recording-links", htmx_request_get_form_value(request, "telephony_recording_links") != NULL, NULL);
				if (venture_database_save(db, organization, &actor, &error))
					g_string_append(html, "<div class=\"notice positive\"><div>Rules saved.</div></div>");
			}
		}
		else
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose a telephony settings operation");
		if (error != NULL)
		{
			g_string_append(html, "<div class=\"notice negative\" role=\"alert\"><div>");
			venture_html_escape_append(html, error->message);
			g_string_append(html, "</div></div>");
		}
		g_clear_object(&organization);
		organization = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, org, NULL);
	}
	if (shown_token != NULL)
	{
		g_autofree gchar *configured = NULL;
		g_object_get(venture_context_get_config(self->context), "server-base-url", &configured, NULL);
		g_string_append(html, "<div class=\"notice positive\"><div><p>Give this webhook address to the provider now; it is shown once:</p><p><code>");
		venture_html_escape_append(html, !venture_string_is_empty(configured) ? configured : venture_web_server_get_base_url(self));
		g_string_append(html, "/hooks/telephony/");
		venture_html_escape_append(html, shown_token);
		g_string_append(html, "</code></p><p class=\"muted\">For VoIP.ms, add <code>?to={TO}&amp;from={FROM}&amp;message={MESSAGE}&amp;id={ID}&amp;media={MEDIA}</code> "
			"and set it as the number's SMS URL callback. For 3CX, set it as the CRM integration's call journal address.</p></div></div>");
	}
	g_string_append(html, "<p>Call contacts from Venture through your phone system, log every call on the contact, and text "
		"with consent and quiet hours. See <a href=\"/docs/telephony.html\">the phone and text guide</a>.</p>");
	venture_web_telephony_lines(self, html, org);
	venture_web_telephony_connect_forms(html);
	venture_web_telephony_extensions(self, html, org);
	if (organization != NULL)
		venture_web_telephony_rules(html, organization);
	g_string_append(html, "</div>");
	return venture_web_html_response(venture_web_page(self, request, "/organizations", "Phone and text", html->str), error != NULL ? 400 : 200);
}

/* --- Click-to-call -------------------------------------------------------------- */

static VentureEntity *
venture_web_telephony_subject(VentureDatabase *db, JsonObject *json, HtmxRequest *request, GError **error)
{
	static const struct { const gchar *field; GType (*type)(void); } kinds[] = {
		{ "contact_id", venture_contact_get_type }, { "deal_id", venture_deal_get_type }, { "ticket_id", venture_ticket_get_type }
	};
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
	{
		gint64 id = 0;
		if (json != NULL && json_object_has_member(json, kinds[i].field))
			id = json_object_get_int_member(json, kinds[i].field);
		else if (request != NULL && htmx_request_get_form_value(request, kinds[i].field) != NULL)
			id = g_ascii_strtoll(htmx_request_get_form_value(request, kinds[i].field), NULL, 10);
		if (id > 0)
		{
			VentureEntity *row = venture_database_get(db, kinds[i].type(), id, error);
			if (row == NULL && error != NULL && *error == NULL)
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Record not found");
			return row;
		}
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Name the contact, deal or ticket to call");
	return NULL;
}

/* POST /telephony/call (the button) and POST /api/v1/telephony/call. */
static HtmxResponse *
venture_web_telephony_call(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *call = NULL;
	gboolean ui = !g_str_has_prefix(htmx_request_get_path(request), "/api/");
	const gchar *number = NULL;
	HtmxResponse *gate;
	VentureActor actor;
	(void)params;
	if (ui && (gate = venture_web_ui_require_session(self, request)) != NULL) return gate;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "telephony")) != NULL) return gate;
	if (!ui)
	{
		json = htmx_request_get_json(request, NULL);
		if (json != NULL && JSON_NODE_HOLDS_OBJECT(json))
			number = venture_json_object_get_string(json_node_get_object(json), "number", NULL);
	}
	subject = venture_web_telephony_subject(db, json != NULL && JSON_NODE_HOLDS_OBJECT(json) ? json_node_get_object(json) : NULL,
		ui ? request : NULL, &error);
	if (subject == NULL)
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	call = venture_telephony_service_click_to_call(venture_telephony_service_get(db), subject, principal->user_id, number, &actor, &error);
	if (call == NULL)
		return venture_web_error_response(error);
	if (ui)
	{
		g_autoptr(GString) html = g_string_new("<h1>Calling</h1><p>Your phone is ringing. Pick it up and the phone system dials ");
		g_autofree gchar *path = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, venture_entity_get_entity_name(subject), venture_entity_get_id(subject));
		g_autofree gchar *name = NULL;
		g_object_get(subject, VENTURE_IS_TICKET(subject) ? "title" : "name", &name, NULL);
		venture_html_escape_append(html, name != NULL ? name : "the contact");
		g_string_append(html, ". The call is logged on the contact when it ends.</p><p><a class=\"btn\" href=\"");
		venture_html_escape_append(html, path);
		g_string_append(html, "\">Back</a></p>");
		return venture_web_html_response(venture_web_page(self, request, path, "Calling", html->str), 200);
	}
	{
		g_autoptr(JsonBuilder) builder = json_builder_new();
		g_autoptr(JsonNode) result = NULL;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "call_id");
		json_builder_add_string_value(builder, call);
		json_builder_set_member_name(builder, "state");
		json_builder_add_string_value(builder, "ringing your extension");
		json_builder_end_object(builder);
		result = json_builder_get_root(builder);
		return venture_web_json_response(result, 202);
	}
}

/* --- Texts ---------------------------------------------------------------------- */

static VentureEntity *
venture_web_telephony_contact(VentureWebServer *self, GHashTable *params, GError **error)
{
	VentureEntity *contact = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_CONTACT,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), error);
	if (contact == NULL && error != NULL && *error == NULL)
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Contact not found");
	return contact;
}

static JsonNode *
venture_web_telephony_thread_json(VentureTelephonyService *service, VentureEntity *contact, GError **error)
{
	g_autoptr(GPtrArray) rows = venture_telephony_service_thread(service, contact, 200, error);
	JsonArray *array;
	JsonNode *node;
	guint i;
	if (rows == NULL)
		return NULL;
	array = json_array_new();
	for (i = 0; i < rows->len; i++)
		json_array_add_element(array, venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(rows, i)), FALSE));
	node = json_node_new(JSON_NODE_ARRAY);
	json_node_take_array(node, array);
	return node;
}

/* POST /contacts/:id/texts (body=...) and POST /api/v1/contacts/:id/texts
 * ({"body": "...", "hosted_image_id": 0}); GET /api/v1/contacts/:id/texts. */
static HtmxResponse *
venture_web_telephony_texts(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureTelephonyService *service = venture_telephony_service_get(db);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) contact = NULL, message = NULL;
	g_autoptr(JsonNode) json = NULL, result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gboolean ui = !g_str_has_prefix(htmx_request_get_path(request), "/api/");
	gboolean reading = htmx_request_get_method(request) == HTMX_METHOD_GET;
	const gchar *body = NULL;
	gint64 image = 0;
	HtmxResponse *gate;
	VentureActor actor;
	if (ui && (gate = venture_web_ui_require_session(self, request)) != NULL) return gate;
	if ((gate = venture_web_api_require(self, request, reading ? VENTURE_USER_ROLE_VIEWER : VENTURE_USER_ROLE_EDITOR)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "telephony")) != NULL) return gate;
	contact = venture_web_telephony_contact(self, params, &error);
	if (contact == NULL)
		return venture_web_error_response(error);
	if (reading)
	{
		result = venture_web_telephony_thread_json(service, contact, &error);
		return result != NULL ? venture_web_json_response(result, 200) : venture_web_error_response(error);
	}
	if (ui)
	{
		body = htmx_request_get_form_value(request, "body");
		if (htmx_request_get_form_value(request, "hosted_image_id") != NULL)
			image = g_ascii_strtoll(htmx_request_get_form_value(request, "hosted_image_id"), NULL, 10);
	}
	else
	{
		json = htmx_request_get_json(request, NULL);
		if (json != NULL && JSON_NODE_HOLDS_OBJECT(json))
		{
			body = venture_json_object_get_string(json_node_get_object(json), "body", NULL);
			if (json_object_has_member(json_node_get_object(json), "hosted_image_id"))
				image = json_object_get_int_member(json_node_get_object(json), "hosted_image_id");
		}
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	message = venture_telephony_service_send_text(service, contact, body, image, now, &actor, &error);
	if (message == NULL)
		return venture_web_error_response(error);
	if (ui)
	{
		g_autofree gchar *path = g_strdup_printf("/e/contact/%" G_GINT64_FORMAT "#texts", venture_entity_get_id(contact));
		return venture_web_redirect_to(path);
	}
	result = venture_serializable_to_json(VENTURE_SERIALIZABLE(message), FALSE);
	return venture_web_json_response(result, 201);
}

/* POST /contacts/:id/text-consent and POST /api/v1/contacts/:id/text-consent
 * ({"granted": true, "evidence": "..."}): an operator records what the
 * person said, as evidence. */
static HtmxResponse *
venture_web_telephony_consent(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gboolean ui = !g_str_has_prefix(htmx_request_get_path(request), "/api/");
	gboolean granted = TRUE;
	const gchar *evidence = NULL;
	HtmxResponse *gate;
	VentureActor actor;
	if (ui && (gate = venture_web_ui_require_session(self, request)) != NULL) return gate;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "telephony")) != NULL) return gate;
	contact = venture_web_telephony_contact(self, params, &error);
	if (contact == NULL)
		return venture_web_error_response(error);
	if (ui)
	{
		evidence = htmx_request_get_form_value(request, "evidence");
		granted = g_strcmp0(htmx_request_get_form_value(request, "granted"), "0") != 0;
	}
	else
	{
		json = htmx_request_get_json(request, NULL);
		if (json != NULL && JSON_NODE_HOLDS_OBJECT(json))
		{
			evidence = venture_json_object_get_string(json_node_get_object(json), "evidence", NULL);
			granted = venture_json_object_get_bool(json_node_get_object(json), "granted", TRUE);
		}
	}
	if (venture_string_is_empty(evidence))
		evidence = granted ? "Agreed to texts, as recorded by an operator" : "Asked to stop texts, as recorded by an operator";
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (!venture_telephony_service_record_consent(venture_telephony_service_get(db), contact, granted, evidence, NULL, now, &actor, &error))
		return venture_web_error_response(error);
	if (ui)
	{
		g_autofree gchar *path = g_strdup_printf("/e/contact/%" G_GINT64_FORMAT "#texts", venture_entity_get_id(contact));
		return venture_web_redirect_to(path);
	}
	{
		g_autoptr(JsonBuilder) builder = json_builder_new();
		g_autoptr(JsonNode) result = NULL;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "text_consent");
		json_builder_add_boolean_value(builder, venture_telephony_service_has_consent(venture_telephony_service_get(db), contact, NULL));
		json_builder_end_object(builder);
		result = json_builder_get_root(builder);
		return venture_web_json_response(result, 201);
	}
}

/* POST /api/v1/telephony/run?organization_id=N: send the texts that are due. */
static HtmxResponse *
venture_web_telephony_run(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	const gchar *text = htmx_request_get_query_param(request, "organization_id");
	gint64 org = text != NULL ? g_ascii_strtoll(text, NULL, 10) : venture_context_get_default_organization_id(self->context);
	HtmxResponse *gate;
	VentureActor actor;
	gint sent;
	(void)params;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "telephony")) != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	sent = venture_telephony_service_deliver_due(venture_telephony_service_get(venture_context_get_database(self->context)),
		org, now, 100, &actor, &error);
	if (sent < 0)
		return venture_web_error_response(error);
	{
		g_autoptr(JsonBuilder) builder = json_builder_new();
		g_autoptr(JsonNode) result = NULL;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "sent");
		json_builder_add_int_value(builder, sent);
		json_builder_end_object(builder);
		result = json_builder_get_root(builder);
		return venture_web_json_response(result, 200);
	}
}

/* --- The webhook ------------------------------------------------------------------ */

static void
venture_web_telephony_header(const gchar *name, const gchar *value, gpointer data)
{
	g_hash_table_replace(data, g_ascii_strdown(name, -1), g_strdup(value));
}

/* GET or POST /hooks/telephony/:token. No session: the token names the
 * line, and the service checks senders and signatures. */
static HtmxResponse *
venture_web_telephony_hook(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	SoupServerMessage *message = htmx_request_get_message(request);
	g_autoptr(GHashTable) headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(GHashTable) query = NULL;
	g_autoptr(GBytes) empty = g_bytes_new_static("", 0);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *reply = NULL, *remote = NULL;
	GBytes *body = htmx_request_get_body_bytes(request);
	GSocketAddress *peer = soup_server_message_get_remote_address(message);
	const gchar *raw_query = g_uri_get_query(soup_server_message_get_uri(message));
	VentureTelephonyWebhook hook;
	HtmxResponse *response;
	if (body != NULL && g_bytes_get_size(body) > VENTURE_TELEPHONY_WEBHOOK_MAX_BODY)
	{
		response = htmx_response_new_with_content("{\"error\":\"too large\"}");
		htmx_response_set_status(response, 413);
		return response;
	}
	if (!venture_context_module_enabled(self->context, "telephony"))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such webhook");
		return venture_web_error_response(error);
	}
	soup_message_headers_foreach(soup_server_message_get_request_headers(message), venture_web_telephony_header, headers);
	if (raw_query != NULL)
		query = soup_form_decode(raw_query);
	if (G_IS_INET_SOCKET_ADDRESS(peer))
		remote = g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(peer)));
	hook.method = htmx_request_get_method(request) == HTMX_METHOD_GET ? "GET" : "POST";
	hook.remote_address = remote;
	hook.query = query;
	hook.headers = headers;
	hook.content_type = g_hash_table_lookup(headers, "content-type");
	hook.body = body != NULL ? body : empty;
	result = venture_telephony_service_receive(venture_telephony_service_get(venture_context_get_database(self->context)),
		g_hash_table_lookup(params, "token"), &hook, now, &reply, &error);
	if (result == NULL)
	{
		response = venture_web_error_response(error);
		if (g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED))
			htmx_response_set_status(response, 401);
		return response;
	}
	if (reply != NULL)
	{
		response = htmx_response_new_with_content(reply);
		htmx_response_set_status(response, 200);
		return response;
	}
	return venture_web_json_response(result, 200);
}

/* --- On a record's page --------------------------------------------------------------- */

static void
venture_web_telephony_thread_block(VentureWebServer *self, GString *content, VentureEntity *contact, gboolean can_text)
{
	VentureTelephonyService *service = venture_telephony_service_get(venture_context_get_database(self->context));
	g_autoptr(GPtrArray) rows = venture_telephony_service_thread(service, contact, 50, NULL);
	gboolean agreed = venture_telephony_service_has_consent(service, contact, NULL);
	gint64 id = venture_entity_get_id(contact);
	guint i;
	g_string_append_printf(content, "<section class=\"card phone-card\" id=\"texts\"><div class=\"card-head\"><h2>Texts</h2>"
		"<span class=\"badge\">%s</span></div><div class=\"card-body\">", agreed ? "Agreed to texts" : "Has not agreed to texts");
	if (rows != NULL && rows->len > 0)
	{
		g_string_append(content, "<ol class=\"text-thread\">");
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			g_autofree gchar *body = NULL, *state = NULL, *why = NULL, *documents = NULL;
			g_autoptr(GDateTime) when = NULL, received = NULL, held = NULL;
			gint direction = 0;
			gboolean automatic = FALSE;
			g_object_get(row, "body", &body, "state", &state, "error", &why, "direction", &direction, "sent-at", &when,
				"received-at", &received, "hold-until", &held, "auto-reply", &automatic, "media-documents", &documents, NULL);
			g_string_append_printf(content, "<li class=\"%s\">", direction == VENTURE_TEXT_DIRECTION_INBOUND ? "in" : "out");
			venture_html_escape_append(content, body);
			if (!venture_string_is_empty(documents))
				g_string_append(content, " <em>(picture kept privately)</em>");
			g_string_append(content, "<span class=\"meta\">");
			if (direction == VENTURE_TEXT_DIRECTION_INBOUND)
			{
				g_string_append(content, "Received ");
				venture_web_telephony_when(self, content, received);
			}
			else
			{
				venture_html_escape_append(content, state);
				if (automatic)
					g_string_append(content, " · automatic reply");
				if (when != NULL)
				{
					g_string_append(content, " · ");
					venture_web_telephony_when(self, content, when);
				}
				else if (held != NULL)
				{
					g_string_append(content, " until ");
					venture_web_telephony_when(self, content, held);
				}
				if (!venture_string_is_empty(why) && g_strcmp0(state, "sent") != 0 && g_strcmp0(state, "delivered") != 0)
				{
					g_string_append(content, " · ");
					venture_html_escape_append(content, why);
				}
			}
			g_string_append(content, "</span></li>");
		}
		g_string_append(content, "</ol>");
	}
	else
		g_string_append(content, "<p class=\"muted\">No texts yet.</p>");
	if (!can_text)
		g_string_append(content, "<p class=\"muted\">Connect a texting line in Telephony settings to text from here.</p>");
	else if (agreed)
		g_string_append_printf(content, "<form method=\"post\" action=\"/contacts/%" G_GINT64_FORMAT "/texts\">"
			"<label>Text<textarea name=\"body\" rows=\"3\" maxlength=\"%d\" required></textarea></label>"
			"<p class=\"muted\">Texts wait during their quiet hours (" VENTURE_TELEPHONY_QUIET_HOURS " their time unless your organization set others).</p>"
			"<div class=\"row\"><button class=\"btn btn-primary\" type=\"submit\">Send text</button></div></form>"
			"<form method=\"post\" action=\"/contacts/%" G_GINT64_FORMAT "/text-consent\" class=\"row\"><input type=\"hidden\" name=\"granted\" value=\"0\">"
			"<input type=\"hidden\" name=\"evidence\" value=\"Asked to stop texts, as recorded by an operator\">"
			"<button class=\"btn\" type=\"submit\">They asked to stop</button></form>", id, VENTURE_TELEPHONY_TEXT_MAX, id);
	else
		g_string_append_printf(content, "<form method=\"post\" action=\"/contacts/%" G_GINT64_FORMAT "/text-consent\">"
			"<input type=\"hidden\" name=\"granted\" value=\"1\">"
			"<label>How did they agree to texts?<textarea name=\"evidence\" rows=\"2\" required placeholder=\"Said yes on the phone on 14 September\"></textarea></label>"
			"<p class=\"muted\">Venture texts only people whose latest consent is an agreement. It is kept as evidence, like email consent.</p>"
			"<button class=\"btn\" type=\"submit\">Record their agreement</button></form>", id);
	g_string_append(content, "</div></section>");
}

static void
venture_web_telephony_block(VentureWebServer *self, GString *content, VentureEntity *record, const VentureAuthPrincipal *principal)
{
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureTelephonyService *service;
	g_autoptr(VentureEntity) line = NULL, texting = NULL, contact = NULL;
	g_autofree gchar *extension = NULL, *phone = NULL, *subject_field = NULL;
	gint64 org;
	if (!venture_web_module_enabled(self, "telephony"))
		return;
	if (VENTURE_IS_ORGANIZATION(record))
	{
		if (venture_web_telephony_is_manager(self, principal, venture_entity_get_id(record)))
			g_string_append_printf(content, "<p><a class=\"btn\" href=\"/organizations/%" G_GINT64_FORMAT "/settings/telephony\">Telephony settings</a></p>",
				venture_entity_get_id(record));
		return;
	}
	if (!VENTURE_IS_CONTACT(record) && !VENTURE_IS_DEAL(record) && !VENTURE_IS_TICKET(record))
		return;
	service = venture_telephony_service_get(db);
	org = venture_entity_get_organization_id(record);
	line = venture_telephony_service_line(service, org, VENTURE_TELEPHONY_CAPABILITY_CLICK_TO_CALL, NULL);
	texting = venture_telephony_service_line(service, org, VENTURE_TELEPHONY_CAPABILITY_SMS, NULL);
	if (VENTURE_IS_CONTACT(record))
		contact = g_object_ref(record);
	else
	{
		gint64 id = 0;
		g_object_get(record, "contact-id", &id, NULL);
		contact = id > 0 ? venture_database_get(db, VENTURE_TYPE_CONTACT, id, NULL) : NULL;
	}
	if (contact != NULL)
		g_object_get(contact, "phone", &phone, NULL);
	extension = principal != NULL ? venture_telephony_service_extension(service, org, principal->user_id) : NULL;
	subject_field = g_strdup_printf("%s_id", venture_entity_get_entity_name(record));
	g_string_append(content, venture_web_telephony_style);
	g_string_append(content, "<section class=\"card phone-card\" id=\"phone\"><div class=\"card-head\"><h2>Phone</h2></div><div class=\"card-body\">");
	if (venture_string_is_empty(phone))
		g_string_append(content, "<p class=\"muted\">No phone number to call.</p>");
	else if (line == NULL)
	{
		g_string_append(content, "<p><a href=\"tel:");
		venture_html_escape_append(content, phone);
		g_string_append(content, "\">");
		venture_html_escape_append(content, phone);
		g_string_append(content, "</a></p><p class=\"muted\">Connect your phone system in Telephony settings to call from here and log calls automatically.</p>");
	}
	else
	{
		g_string_append_printf(content, "<form method=\"post\" action=\"/telephony/call\" class=\"row\"><input type=\"hidden\" name=\"%s\" value=\"%" G_GINT64_FORMAT "\">"
			"<button class=\"btn btn-primary\" type=\"submit\"%s>Call ", subject_field, venture_entity_get_id(record),
			venture_string_is_empty(extension) ? " disabled" : "");
		venture_html_escape_append(content, phone);
		g_string_append(content, "</button></form>");
		if (venture_string_is_empty(extension))
			g_string_append(content, "<p class=\"muted\">Click-to-call rings your own phone first. Ask an administrator to add your extension in Telephony settings.</p>");
		else
		{
			g_string_append(content, "<p class=\"muted\">Rings your extension ");
			venture_html_escape_append(content, extension);
			g_string_append(content, " first, then dials. The call is logged here when it ends.</p>");
		}
	}
	g_string_append(content, "</div></section>");
	if (VENTURE_IS_CONTACT(record))
		venture_web_telephony_thread_block(self, content, record, texting != NULL);
}

static void
venture_telephony_pages_register(VentureWebServer *self)
{
	static const struct {
		HtmxMethod method;
		const gchar *path;
		HtmxRouteCallback handler;
	} routes[] = {
		{ HTMX_METHOD_GET, "/organizations/:id/settings/telephony", venture_web_telephony_settings },
		{ HTMX_METHOD_POST, "/organizations/:id/settings/telephony", venture_web_telephony_settings },
		{ HTMX_METHOD_POST, "/telephony/call", venture_web_telephony_call },
		{ HTMX_METHOD_POST, "/api/v1/telephony/call", venture_web_telephony_call },
		{ HTMX_METHOD_POST, "/api/v1/telephony/run", venture_web_telephony_run },
		{ HTMX_METHOD_POST, "/contacts/:id/texts", venture_web_telephony_texts },
		{ HTMX_METHOD_POST, "/api/v1/contacts/:id/texts", venture_web_telephony_texts },
		{ HTMX_METHOD_GET, "/api/v1/contacts/:id/texts", venture_web_telephony_texts },
		{ HTMX_METHOD_POST, "/contacts/:id/text-consent", venture_web_telephony_consent },
		{ HTMX_METHOD_POST, "/api/v1/contacts/:id/text-consent", venture_web_telephony_consent },
		{ HTMX_METHOD_GET, "/hooks/telephony/:token", venture_web_telephony_hook },
		{ HTMX_METHOD_POST, "/hooks/telephony/:token", venture_web_telephony_hook },
	};
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(routes); i++)
		venture_web_server_add_classified_route(self, routes[i].method, routes[i].path,
			VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, routes[i].handler, self);
}
