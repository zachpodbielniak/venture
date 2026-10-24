/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Included by the web server: the deliverability page (domain check,
 * warm-up, feed secret), its JSON, a send's report with its click map,
 * and the signed provider feed. The page and the domain check are for an
 * owner or administrator of the organization; the report for anyone who
 * works its campaigns; the feed has no session and its signature is the
 * guard (src/marketing/venture-deliverability.c). */

static const gint venture_web_deliverability_managers[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
static const gint venture_web_deliverability_readers[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN,
	VENTURE_ORGANIZATION_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_SALES };
#define VENTURE_WEB_DELIVERABILITY_DOMAINS 10

static gboolean
venture_web_deliverability_role(VentureWebServer *self, HtmxRequest *request, gint64 org,
	const gint *roles, gsize n_roles, const gchar *what, GError **error)
{
	g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);
	if (venture_access_policy_has_organization_role(venture_database_get_access_policy(venture_context_get_database(self->context)),
		principal, org, roles, n_roles))
		return TRUE;
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "%s", what);
	return FALSE;
}

/* The organization named by organization_id, or the one being worked in. */
static gint64
venture_web_deliverability_org(VentureWebServer *self, HtmxRequest *request, const gchar *value, GError **error)
{
	g_autofree gchar *fallback = NULL;
	if (venture_string_is_empty(value)) {
		gint64 active = venture_web_active_organization(self, request);
		if (active > 0) value = fallback = g_strdup_printf("%" G_GINT64_FORMAT, active);
	}
	return venture_web_ai_organization(self, value, error);
}

static const gchar *
venture_web_deliverability_badge(const gchar *status)
{
	if (!g_strcmp0(status, "pass")) return "positive";
	if (!g_strcmp0(status, "weak")) return "warning";
	if (!g_strcmp0(status, "missing")) return "negative";
	return "info";
}

static const gchar *
venture_web_deliverability_status_words(const gchar *status)
{
	if (!g_strcmp0(status, "pass")) return "Set up";
	if (!g_strcmp0(status, "weak")) return "Needs work";
	if (!g_strcmp0(status, "missing")) return "Missing";
	return "Could not check";
}

static void
venture_web_deliverability_append_check(GString *html, JsonNode *result)
{
	JsonObject *object = json_node_get_object(result);
	JsonArray *checks = json_object_get_array_member(object, "checks");
	const gchar *overall = json_object_get_string_member(object, "overall");
	guint i;
	g_string_append(html, "<div class=\"card deliverability-domain\"><div class=\"card-head\"><h2>");
	venture_html_escape_append(html, json_object_get_string_member(object, "domain"));
	g_string_append_printf(html, "</h2><span class=\"badge %s\"><span class=\"dot\"></span>%s</span></div><div class=\"card-body\">",
		venture_web_deliverability_badge(overall), venture_web_deliverability_status_words(overall));
	for (i = 0; i < json_array_get_length(checks); i++) {
		JsonObject *check = json_array_get_object_element(checks, i);
		const gchar *status = json_object_get_string_member(check, "status");
		const gchar *fix = json_object_get_string_member_with_default(check, "fix", "");
		JsonNode *record = json_object_get_member(check, "record");
		g_string_append(html, "<section class=\"deliverability-check\"><h3>");
		venture_html_escape_append(html, json_object_get_string_member(check, "label"));
		g_string_append_printf(html, " <span class=\"badge %s\"><span class=\"dot\"></span>%s</span></h3><p>",
			venture_web_deliverability_badge(status), venture_web_deliverability_status_words(status));
		venture_html_escape_append(html, json_object_get_string_member(check, "summary"));
		g_string_append(html, "</p><p class=\"muted\">Looked up at <code>");
		venture_html_escape_append(html, json_object_get_string_member(check, "name"));
		g_string_append(html, "</code></p>");
		if (record && JSON_NODE_HOLDS_VALUE(record)) {
			g_string_append(html, "<pre class=\"deliverability-record\" style=\"white-space:pre-wrap;overflow-wrap:anywhere\">");
			venture_html_escape_append(html, json_node_get_string(record));
			g_string_append(html, "</pre>");
		}
		if (*fix) {
			g_string_append(html, "<p><strong>What to change:</strong> ");
			venture_html_escape_append(html, fix);
			g_string_append(html, "</p>");
		}
		g_string_append(html, "</section>");
	}
	g_string_append(html, "</div></div>");
}

static void
venture_web_deliverability_stat(GString *html, const gchar *label, gint64 value, const gchar *note)
{
	g_string_append_printf(html, "<div class=\"stat\"><span class=\"stat-label\">%s</span>"
		"<span class=\"stat-value\">%" G_GINT64_FORMAT "</span>", label, value);
	if (note) {
		g_string_append(html, "<span class=\"stat-note\">");
		venture_html_escape_append(html, note);
		g_string_append(html, "</span>");
	}
	g_string_append(html, "</div>");
}

/* GET /marketing/deliverability */
static HtmxResponse *
venture_web_deliverability_page_with(VentureWebServer *self, HtmxRequest *request, gint64 org, const gchar *notice)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(GString) html = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDnsResolver) resolver = venture_deliverability_get_resolver(database);
	g_autoptr(GPtrArray) domains = NULL, sends = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	const gchar *asked = htmx_request_get_query_param(request, "domain");
	const gchar *selector = htmx_request_get_query_param(request, "selector");
	gint64 day = 0, limit = 0, used = 0;
	guint i;
	g_string_append(html, "<h1>Deliverability</h1><p>Whether mail from your domain is believed, how fast a new domain may send, "
		"and what happened to campaign mail after your mail server accepted it.</p>");
	if (notice) g_string_append_printf(html, "<div class=\"notice positive\"><div>%s</div></div>", notice);

	/* The domain check. */
	g_string_append(html, "<h2>Sending domain check</h2><p>SPF says which servers may send for your domain, DKIM signs each message, "
		"and DMARC tells receivers what to do when neither checks out. This reads public DNS; it cannot see what your mail server signs, "
		"so also send yourself a test and look for <code>spf=pass</code>, <code>dkim=pass</code> and <code>dmarc=pass</code> in its headers.</p>");
	g_string_append_printf(html, "<form method=\"get\" action=\"/marketing/deliverability\" class=\"deliverability-form\">"
		"<input type=\"hidden\" name=\"organization_id\" value=\"%" G_GINT64_FORMAT "\">"
		"<label>Domain <input type=\"text\" name=\"domain\" maxlength=\"253\" placeholder=\"example.com\" value=\"", org);
	venture_html_escape_append(html, asked ? asked : "");
	g_string_append(html, "\"></label> <label>DKIM selector <input type=\"text\" name=\"selector\" maxlength=\"63\" placeholder=\"mail\" value=\"");
	venture_html_escape_append(html, selector ? selector : "");
	g_string_append(html, "\"></label> <button class=\"btn\" type=\"submit\">Check</button></form>");
	if (!venture_string_is_empty(asked)) {
		g_autoptr(JsonNode) result = venture_deliverability_check_domain(resolver, asked, selector, &error);
		if (result) venture_web_deliverability_append_check(html, result);
		else {
			g_string_append(html, "<div class=\"notice negative\"><div>");
			venture_html_escape_append(html, error->message);
			g_string_append(html, "</div></div>");
			g_clear_error(&error);
		}
	}
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_SENDING_DOMAIN);
		venture_query_set_organization(query, org);
		venture_query_set_limit(query, VENTURE_WEB_DELIVERABILITY_DOMAINS);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		domains = venture_database_find(database, query, NULL);
	}
	if (venture_string_is_empty(asked)) {
		if (!domains || !domains->len)
			g_string_append(html, "<p>No sending domain saved yet. <a href=\"/e/marketing_sending_domain/new\">Add the domain in your From address</a> "
				"with its DKIM selector, and it is checked here every time you open this page.</p>");
		for (i = 0; domains && i < domains->len; i++) {
			VentureEntity *row = g_ptr_array_index(domains, i);
			g_autofree gchar *name = NULL, *key = NULL;
			g_autoptr(JsonNode) result = NULL;
			g_object_get(row, "domain", &name, "dkim-selector", &key, NULL);
			result = venture_deliverability_check_domain(resolver, name, key, NULL);
			if (result) venture_web_deliverability_append_check(html, result);
		}
	}

	/* The warm-up. */
	g_string_append(html, "<h2>Warm-up</h2>");
	if (venture_deliverability_ramp_status(database, org, now, &day, &limit, &used, &error)) {
		g_autofree gchar *schedule = NULL;
		if (domains && domains->len) g_object_get(g_ptr_array_index(domains, 0), "ramp-schedule", &schedule, NULL);
		g_string_append(html, "<div class=\"stat-row\">");
		venture_web_deliverability_stat(html, "Day of sending", day, NULL);
		if (limit > 0) {
			venture_web_deliverability_stat(html, "Today's limit", limit, "campaign messages, UTC day");
			venture_web_deliverability_stat(html, "Queued today", used, used >= limit ? "limit reached; the rest wait for tomorrow" : NULL);
		} else {
			venture_web_deliverability_stat(html, "Queued today", used, "no warm-up limit applies");
		}
		g_string_append(html, "</div><p>Mailbox providers distrust a new domain that suddenly sends a lot. Campaigns therefore start small and grow: ");
		{
			/* Spaced after each comma so a phone can wrap it. */
			g_auto(GStrv) pairs = g_strsplit(venture_string_is_empty(schedule) ? VENTURE_DELIVERABILITY_DEFAULT_RAMP : schedule, ",", 64);
			g_autofree gchar *spaced = g_strjoinv(", ", pairs);
			venture_html_escape_append(html, spaced);
		}
		g_string_append(html, " (<code>day:limit</code>; 0 is no limit). Day one is the sending domain's warm-up date, or the day your first campaign "
			"message was accepted. Change it on the <a href=\"/e/marketing_sending_domain\">sending domain</a>; <code>off</code> turns it off.</p>");
	} else {
		g_string_append(html, "<div class=\"notice negative\"><div>");
		venture_html_escape_append(html, error->message);
		g_string_append(html, "</div></div>");
		g_clear_error(&error);
	}

	/* The feed. */
	g_string_append(html, "<h2>Bounce and complaint feed</h2><p>Bounces and complaints reach Venture two ways. A bounce "
		"(delivery status notification) that lands in a synced mailbox is read automatically. A mail provider can also post "
		"events to this organization's feed address, signed with its secret:</p>");
	{
		const gchar *base = venture_web_server_get_base_url(self);
		g_autofree gchar *configured = NULL;
		g_object_get(venture_context_get_config(self->context), "server-base-url", &configured, NULL);
		g_string_append(html, "<pre style=\"white-space:pre-wrap;overflow-wrap:anywhere\">POST ");
		venture_html_escape_append(html, !venture_string_is_empty(configured) ? configured : base);
		g_string_append_printf(html, "/hooks/deliverability/%" G_GINT64_FORMAT "/generic</pre>", org);
	}
	g_string_append_printf(html, "<p>%s The provider sends <code>X-Venture-Timestamp</code> (Unix seconds) and "
		"<code>X-Venture-Signature</code> (hex HMAC-SHA256 of the timestamp, a newline and the body). "
		"Deliveries more than five minutes old are refused. See <a href=\"/docs/deliverability.html\">the deliverability guide</a> for the event shape.</p>"
		"<form method=\"post\" action=\"/marketing/deliverability/secret\"><input type=\"hidden\" name=\"organization_id\" value=\"%" G_GINT64_FORMAT "\">"
		"<button class=\"btn\" type=\"submit\">%s</button> The secret is shown once; making a new one stops the old one at once.</form>",
		venture_deliverability_feed_configured(database, org) ? "A signing secret is set." : "No signing secret is set, so the feed refuses everything.",
		org, venture_deliverability_feed_configured(database, org) ? "Make a new signing secret" : "Make a signing secret");

	/* Recent sends. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_SEND);
		venture_query_set_organization(query, org);
		venture_query_set_limit(query, 20);
		venture_query_add_filter_int(query, "state", VENTURE_FILTER_OP_GTE, 2, NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		sends = venture_database_find(database, query, NULL);
	}
	g_string_append(html, "<h2>Send reports</h2>");
	if (!sends || !sends->len) g_string_append(html, "<p>No approved campaign sends yet.</p>");
	else {
		g_string_append(html, "<ul>");
		for (i = 0; i < sends->len; i++) {
			VentureEntity *send = g_ptr_array_index(sends, i);
			g_autofree gchar *name = NULL;
			g_object_get(send, "name", &name, NULL);
			g_string_append_printf(html, "<li><a href=\"/marketing/sends/%" G_GINT64_FORMAT "/report\">", venture_entity_get_id(send));
			venture_html_escape_append(html, name);
			g_string_append(html, "</a></li>");
		}
		g_string_append(html, "</ul>");
	}
	return venture_web_html_response(venture_web_page(self, request, "/marketing/deliverability", "Deliverability", html->str), 200);
}

static HtmxResponse *
venture_web_deliverability_page(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	gint64 org;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "marketing");
	if (gate != NULL) return gate;
	org = venture_web_deliverability_org(self, request, htmx_request_get_query_param(request, "organization_id"), &error);
	if (org <= 0 || !venture_web_deliverability_role(self, request, org, venture_web_deliverability_managers,
		G_N_ELEMENTS(venture_web_deliverability_managers), "Deliverability needs an owner or administrator of the organization", &error))
		return venture_web_error_response(error);
	return venture_web_deliverability_page_with(self, request, org, NULL);
}

/* POST /marketing/deliverability/secret: a new secret, shown once. */
static HtmxResponse *
venture_web_deliverability_secret(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) notice = g_string_new(NULL);
	g_autofree gchar *secret = NULL;
	VentureActor actor;
	gint64 org;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "marketing");
	if (gate != NULL) return gate;
	org = venture_web_deliverability_org(self, request, htmx_request_get_form_value(request, "organization_id"), &error);
	if (org <= 0 || !venture_web_deliverability_role(self, request, org, venture_web_deliverability_managers,
		G_N_ELEMENTS(venture_web_deliverability_managers), "Deliverability needs an owner or administrator of the organization", &error))
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	secret = venture_deliverability_set_feed_secret(venture_context_get_database(self->context), org, NULL, &actor, &error);
	if (!secret) return venture_web_error_response(error);
	g_string_append(notice, "New signing secret, shown once -- give it to your mail provider now: <code>");
	venture_html_escape_append(notice, secret);
	g_string_append(notice, "</code>");
	return venture_web_deliverability_page_with(self, request, org, notice->str);
}

/* GET /api/v1/marketing/domain-health?organization_id=&domain=&selector= */
static HtmxResponse *
venture_web_api_deliverability_domain(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDnsResolver) resolver = NULL;
	g_autoptr(JsonNode) result = NULL;
	gint64 org;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	(void)params;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "marketing");
	if (gate != NULL) return gate;
	org = venture_web_deliverability_org(self, request, htmx_request_get_query_param(request, "organization_id"), &error);
	if (org <= 0 || !venture_web_deliverability_role(self, request, org, venture_web_deliverability_managers,
		G_N_ELEMENTS(venture_web_deliverability_managers), "Checking a domain needs an owner or administrator of the organization", &error))
		return venture_web_error_response(error);
	resolver = venture_deliverability_get_resolver(venture_context_get_database(self->context));
	result = venture_deliverability_check_domain(resolver, htmx_request_get_query_param(request, "domain"),
		htmx_request_get_query_param(request, "selector"), &error);
	if (!result) return venture_web_error_response(error);
	json_object_set_int_member(json_node_get_object(result), "organization_id", org);
	return venture_web_json_response(result, 200);
}

/* POST /api/v1/marketing/feedback-secret {"organization_id": N, "secret": optional} */
static HtmxResponse *
venture_web_api_deliverability_secret(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *secret = NULL, *org_text = NULL;
	GBytes *body = htmx_request_get_body_bytes(request);
	JsonObject *object = NULL;
	VentureActor actor;
	gint64 org;
	gsize length = 0;
	const gchar *bytes = body ? g_bytes_get_data(body, &length) : NULL;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	(void)params;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "marketing");
	if (gate != NULL) return gate;
	if (bytes && length && length < 4096 && !memchr(bytes, 0, length) && json_parser_load_from_data(parser, bytes, (gssize)length, NULL) &&
		JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		object = json_node_get_object(json_parser_get_root(parser));
	if (object && json_object_has_member(object, "organization_id"))
		org_text = g_strdup_printf("%" G_GINT64_FORMAT, json_object_get_int_member(object, "organization_id"));
	org = venture_web_deliverability_org(self, request, org_text, &error);
	if (org <= 0 || !venture_web_deliverability_role(self, request, org, venture_web_deliverability_managers,
		G_N_ELEMENTS(venture_web_deliverability_managers), "The feed secret needs an owner or administrator of the organization", &error))
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	secret = venture_deliverability_set_feed_secret(venture_context_get_database(self->context), org,
		object ? venture_json_object_get_string(object, "secret", NULL) : NULL, &actor, &error);
	if (!secret) return venture_web_error_response(error);
	result = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(result, json_object_new());
	json_object_set_int_member(json_node_get_object(result), "organization_id", org);
	json_object_set_string_member(json_node_get_object(result), "secret", secret);
	return venture_web_json_response(result, 200);
}

/* The send in the path, and the caller's right to read its report. */
static VentureEntity *
venture_web_deliverability_send(VentureWebServer *self, HtmxRequest *request, GHashTable *params, GError **error)
{
	const gchar *text = g_hash_table_lookup(params, "id");
	g_autoptr(VentureEntity) send = NULL;
	gint64 id = 0;
	if (text && g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, &id, NULL))
		send = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_MARKETING_SEND, id, NULL);
	if (!send || venture_entity_is_deleted(send)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Campaign send not found");
		return NULL;
	}
	if (!venture_web_deliverability_role(self, request, venture_entity_get_organization_id(send), venture_web_deliverability_readers,
		G_N_ELEMENTS(venture_web_deliverability_readers), "A send's report needs an owner, administrator, editor or sales member of its organization", error))
		return NULL;
	return g_steal_pointer(&send);
}

/* GET /api/v1/marketing/sends/:id/report */
static HtmxResponse *
venture_web_api_deliverability_report(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) send = NULL;
	g_autoptr(VentureMarketingSendReport) report = NULL;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "marketing");
	if (gate != NULL) return gate;
	send = venture_web_deliverability_send(self, request, params, &error);
	if (send) report = venture_marketing_send_report(venture_context_get_database(self->context),
		venture_entity_get_organization_id(send), venture_entity_get_id(send), &error);
	if (!report) return venture_web_error_response(error);
	return venture_web_json_response(venture_marketing_send_report_to_json(report), 200);
}

static gchar *
venture_web_deliverability_share(gint64 part, gint64 whole)
{
	if (whole <= 0) return NULL;
	return g_strdup_printf("%" G_GINT64_FORMAT "%% of accepted", (part * 100 + whole / 2) / whole);
}

/* GET /marketing/sends/:id/report */
static HtmxResponse *
venture_web_deliverability_report(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) send = NULL;
	g_autoptr(VentureMarketingSendReport) report = NULL;
	g_autoptr(GString) html = g_string_new(NULL);
	g_autofree gchar *title = NULL;
	gint64 widest = 1;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "marketing");
	if (gate != NULL) return gate;
	send = venture_web_deliverability_send(self, request, params, &error);
	if (send) report = venture_marketing_send_report(venture_context_get_database(self->context),
		venture_entity_get_organization_id(send), venture_entity_get_id(send), &error);
	if (!report) return venture_web_error_response(error);
	title = g_strdup_printf("Report: %s", report->name ? report->name : "send");
	g_string_append(html, "<h1>");
	venture_html_escape_append(html, title);
	g_string_append_printf(html, "</h1><p><a href=\"/e/marketing_send/%" G_GINT64_FORMAT "\">The send</a> &middot; "
		"%" G_GINT64_FORMAT " approved, %" G_GINT64_FORMAT " excluded at preview, %" G_GINT64_FORMAT " pending, "
		"%" G_GINT64_FORMAT " uncertain, %" G_GINT64_FORMAT " failed, %" G_GINT64_FORMAT " cancelled.</p>",
		report->send_id, report->eligible, report->excluded, report->pending, report->uncertain, report->failed, report->cancelled);
	{
		const struct { const gchar *label; gint64 value; gboolean share; } tiles[] = {
			{ "Accepted by your server", report->accepted, FALSE }, { "Delivered (provider)", report->delivered, TRUE },
			{ "Opened", report->opened, TRUE }, { "Clicked", report->clicked, TRUE },
			{ "Replied", report->replied, TRUE }, { "Unsubscribed", report->unsubscribed, TRUE },
			{ "Bounced", report->bounced, TRUE }, { "Soft bounced", report->soft_bounced, TRUE },
			{ "Complained", report->complained, TRUE }, { "Machine opens only", report->machine_opened, TRUE }
		};
		g_string_append(html, "<div class=\"stat-row deliverability-numbers\">");
		for (i = 0; i < G_N_ELEMENTS(tiles); i++) {
			g_autofree gchar *note = tiles[i].share ? venture_web_deliverability_share(tiles[i].value, report->accepted) : NULL;
			venture_web_deliverability_stat(html, tiles[i].label, tiles[i].value, note);
		}
		g_string_append(html, "</div>");
	}
	g_string_append(html, "<div class=\"notice\"><div><strong>How to read this.</strong> Accepted means your mail server took the message, "
		"not that it reached an inbox; delivered is counted only when a provider reports it. Opened and clicked count people, once each. "
		"An open or click is set aside as a machine's when it came from a known link scanner or prefetching proxy, or within seconds of "
		"the message being accepted -- those are shown as machine opens and are not in Opened. Apple Mail Privacy Protection and Gmail's "
		"image proxy cannot be told apart from a person, so Opened is still an upper bound. A reply is any inbound email or logged contact "
		"from the recipient after this message, the way sequences count replies.</div></div>");
	if (report->arms && report->arms->len) {
		/* An A/B test: both arms side by side, and the rest once picked. */
		g_string_append(html, "<h2>A/B test</h2><p>");
		if (report->winner) g_string_append_printf(html, "Arm <strong>%s</strong> won and went to the rest of the audience.", !g_strcmp0(report->winner, "b") ? "B" : "A");
		else g_string_append(html, "The winner is picked once the test arms have been out for the wait you set; until then the rest wait.");
		g_string_append(html, " People's opens and clicks only; a tie goes to A.</p><div class=\"table-wrap\"><table class=\"data deliverability-arms\"><thead><tr>"
			"<th scope=\"col\">Arm</th><th scope=\"col\">Subject</th><th scope=\"col\" class=\"num\">Recipients</th>"
			"<th scope=\"col\" class=\"num\">Accepted</th><th scope=\"col\" class=\"num\">Opened</th><th scope=\"col\" class=\"num\">Clicked</th></tr></thead><tbody>");
		for (i = 0; i < report->arms->len; i++) {
			const VentureMarketingArmCount *arm = g_ptr_array_index(report->arms, i);
			g_autofree gchar *opened = venture_web_deliverability_share(arm->opened, arm->accepted);
			gboolean won = report->winner && !g_strcmp0(report->winner, arm->arm);
			g_string_append_printf(html, "<tr><td class=\"row-head\">%s%s</td><td style=\"min-width:9rem;overflow-wrap:anywhere\">",
				!g_strcmp0(arm->arm, "a") ? "A" : !g_strcmp0(arm->arm, "b") ? "B" : "Rest", won ? " <span class=\"badge positive\">winner</span>" : "");
			venture_html_escape_append(html, arm->subject && *arm->subject ? arm->subject : "The winner's subject, once it is picked");
			g_string_append_printf(html, "</td><td class=\"num\">%" G_GINT64_FORMAT "</td><td class=\"num\">%" G_GINT64_FORMAT "</td>"
				"<td class=\"num\">%" G_GINT64_FORMAT "%s%s%s</td><td class=\"num\">%" G_GINT64_FORMAT "</td></tr>",
				arm->recipients, arm->accepted, arm->opened, opened ? " <span class=\"muted\">(" : "", opened ? opened : "", opened ? ")</span>" : "", arm->clicked);
		}
		g_string_append(html, "</tbody></table></div>");
	}
	g_string_append(html, "<h2>Click map</h2>");
	if (!report->links->len) g_string_append(html, "<p>This send had no tracked links.</p>");
	else {
		for (i = 0; i < report->links->len; i++) {
			const VentureMarketingLinkCount *link = g_ptr_array_index(report->links, i);
			widest = MAX(widest, link->clicks);
		}
		g_string_append(html, "<div class=\"table-wrap\"><table class=\"data deliverability-clicks\"><thead><tr>"
			"<th scope=\"col\" class=\"num\">#</th><th scope=\"col\">Link</th><th scope=\"col\" class=\"num\">People</th>"
			"<th scope=\"col\">Share</th><th scope=\"col\" class=\"num\">Machine clicks</th></tr></thead><tbody>");
		for (i = 0; i < report->links->len; i++) {
			const VentureMarketingLinkCount *link = g_ptr_array_index(report->links, i);
			g_string_append_printf(html, "<tr><td class=\"num\">%" G_GINT64_FORMAT "</td><td style=\"min-width:9rem;overflow-wrap:anywhere\">", link->position);
			venture_html_escape_append(html, link->destination);
			g_string_append_printf(html, "</td><td class=\"num\">%" G_GINT64_FORMAT "</td>"
				"<td><div aria-hidden=\"true\" style=\"width:4rem;height:0.6rem;background:var(--border)\">"
				"<div style=\"height:100%%;width:%" G_GINT64_FORMAT "%%;background:var(--accent)\"></div></div></td>"
				"<td class=\"num\">%" G_GINT64_FORMAT "</td></tr>",
				link->clicks, link->clicks * 100 / widest, link->machine_clicks);
		}
		g_string_append(html, "</tbody></table></div><p class=\"muted\">Links are shown without their query string, which can carry a personal value.</p>");
	}
	return venture_web_html_response(venture_web_page(self, request, "/marketing/deliverability", title, html->str), 200);
}

/* POST /hooks/deliverability/:org/:provider -- no session; the signature
 * is the guard. 401 so a provider retries with a signature rather than a
 * browser being sent to a login page. */
static HtmxResponse *
venture_web_deliverability_hook(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	SoupMessageHeaders *headers = soup_server_message_get_request_headers(htmx_request_get_message(request));
	GBytes *body = htmx_request_get_body_bytes(request);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GBytes) empty = g_bytes_new_static("", 0);
	const gchar *org_text = g_hash_table_lookup(params, "org");
	HtmxResponse *response;
	gint64 org = 0;
	if (!org_text || !g_ascii_string_to_signed(org_text, 10, 1, G_MAXINT64, &org, NULL) ||
		!venture_context_module_enabled(self->context, "marketing"))
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such deliverability feed");
	else if (body && g_bytes_get_size(body) > VENTURE_DELIVERABILITY_MAX_BODY) {
		response = htmx_response_new_with_content("{\"error\":\"too large\"}");
		htmx_response_set_status(response, 413);
		return response;
	} else
		result = venture_deliverability_receive(venture_context_get_database(self->context), org,
			g_hash_table_lookup(params, "provider"), soup_message_headers_get_one(headers, "X-Venture-Timestamp"),
			soup_message_headers_get_one(headers, "X-Venture-Signature"), body ? body : empty, now, &error);
	if (result) return venture_web_json_response(result, 200);
	response = venture_web_error_response(error);
	if (g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED)) htmx_response_set_status(response, 401);
	return response;
}

static void
venture_deliverability_pages_register(VentureWebServer *self)
{
	static const struct {
		HtmxMethod method;
		const gchar *path;
		HtmxRouteCallback handler;
	} routes[] = {
		{ HTMX_METHOD_GET, "/marketing/deliverability", venture_web_deliverability_page },
		{ HTMX_METHOD_POST, "/marketing/deliverability/secret", venture_web_deliverability_secret },
		{ HTMX_METHOD_GET, "/marketing/sends/:id/report", venture_web_deliverability_report },
		{ HTMX_METHOD_GET, "/api/v1/marketing/domain-health", venture_web_api_deliverability_domain },
		{ HTMX_METHOD_POST, "/api/v1/marketing/feedback-secret", venture_web_api_deliverability_secret },
		{ HTMX_METHOD_GET, "/api/v1/marketing/sends/:id/report", venture_web_api_deliverability_report },
		{ HTMX_METHOD_POST, "/hooks/deliverability/:org/:provider", venture_web_deliverability_hook },
	};
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(routes); i++)
		venture_web_server_add_classified_route(self, routes[i].method, routes[i].path,
			VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, routes[i].handler, self);
}
