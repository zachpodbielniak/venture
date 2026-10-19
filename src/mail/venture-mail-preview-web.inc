/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Included by the web server: the mail preview page, its test send and the
 * JSON behind `venturectl mail preview`. Owner or administrator of the
 * record's organization only -- a preview shows customer data as the
 * customer will read it.
 *
 * The email's HTML is shown in iframes with srcdoc and an empty sandbox:
 * no script runs, and the frame's origin is opaque, so nothing in a message
 * can reach this page, its cookie or the API. */

static const gint venture_web_mail_preview_widths[] = { 390, 768, 1440 };
static const gchar *const venture_web_mail_preview_devices[] = { "Phone", "Tablet", "Desktop" };

/* The kind, its source row's organization and id, refused before anything
 * is built: an unknown kind and a missing row are not found, a principal
 * who is not an owner or administrator there is refused. */
static gboolean
venture_web_mail_preview_resolve(VentureWebServer *self, HtmxRequest *request, GHashTable *params,
	const VentureMailPreviewKind **kind, gint64 *org, gint64 *id, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureDatabase *database = venture_context_get_database(self->context);
	const gchar *kind_id = g_hash_table_lookup(params, "kind");
	const gchar *text = g_hash_table_lookup(params, "id");
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) row = NULL;
	GType type;
	*kind = NULL;
	if (text == NULL || !g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, id, NULL)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Record not found");
		return FALSE;
	}
	if (g_strcmp0(kind_id, "message") == 0)
		type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message");
	else {
		*kind = venture_mail_preview_lookup(kind_id);
		if (*kind == NULL) {
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "There is no previewable email kind \"%s\"", kind_id);
			return FALSE;
		}
		if ((*kind)->module != NULL && !venture_context_module_enabled(self->context, (*kind)->module)) {
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "The %s module is off", (*kind)->module);
			return FALSE;
		}
		type = venture_entity_registry_lookup(venture_entity_registry_get_default(), (*kind)->source_type);
	}
	if (type != G_TYPE_INVALID)
		row = venture_database_get(database, type, *id, NULL);
	if (row == NULL || venture_entity_is_deleted(row)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Record not found");
		return FALSE;
	}
	*org = venture_entity_get_organization_id(row);
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(database),
		principal, *org, roles, G_N_ELEMENTS(roles))) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"Previewing mail needs an owner or administrator of the organization");
		return FALSE;
	}
	return TRUE;
}

static VentureMailPreview *
venture_web_mail_preview_make(VentureWebServer *self, HtmxRequest *request, const VentureMailPreviewKind *kind,
	gint64 org, gint64 id, GError **error)
{
	g_autoptr(VentureHttpLinkChecker) checker = NULL;
	const gchar *asked = htmx_request_get_query_param(request, "reachability");
	gboolean reach = g_strcmp0(asked, "1") == 0;
	/* Reachability is asked for, never assumed: it makes requests to other
	 * people's servers from this one. An organization may ask for it on
	 * every preview (mail.preview-link-check, off by default); ?reachability=0
	 * still turns it off for one look. */
	if (asked == NULL) {
		g_autoptr(VentureSettingValue) setting = venture_settings_get(self->context, org, "mail.preview-link-check", NULL);
		reach = setting != NULL && g_strcmp0(setting->value, "true") == 0;
	}
	if (reach)
		checker = venture_http_link_checker_new();
	if (kind == NULL)
		return venture_mail_preview_message(self->context, org, id, VENTURE_MAIL_LINK_CHECKER(checker), error);
	return venture_mail_preview_build(self->context, kind->id, org, id, NULL, VENTURE_MAIL_LINK_CHECKER(checker), error);
}

/* What goes in srcdoc. A text-only message is shown the way mail apps show
 * one, wrapped, and allowed to follow the frame's colour scheme as they do;
 * an HTML message is shown untouched, so only its own dark-mode rules act. */
static gchar *
venture_web_mail_preview_document(const VentureMailPreview *preview)
{
	g_autoptr(GString) doc = NULL;
	if (preview->html != NULL)
		return g_strdup(preview->html);
	doc = g_string_new("<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"color-scheme\" content=\"light dark\"></head>"
		"<body><pre style=\"white-space:pre-wrap;font:14px/1.5 sans-serif;margin:16px\">");
	venture_html_escape_append(doc, preview->text);
	g_string_append(doc, "</pre></body></html>");
	return g_string_free(g_steal_pointer(&doc), FALSE);
}

static void
venture_web_mail_preview_frame(GString *content, const VentureMailPreview *preview, gint width,
	const gchar *device, const gchar *scheme, const gchar *standalone)
{
	g_autofree gchar *doc = venture_web_mail_preview_document(preview);
	g_string_append_printf(content,
		"<figure class=\"mail-preview-frame\" data-width=\"%d\" data-scheme=\"%s\" style=\"margin:0 0 1.5rem;overflow-x:auto\">"
		"<figcaption>%s, %d px, %s", width, scheme, device, width, scheme);
	if (standalone != NULL)
		g_string_append_printf(content, " &middot; <a href=\"%s?frame=%d&amp;scheme=%s\">open alone</a>", standalone, width, scheme);
	g_string_append_printf(content, "</figcaption>"
		"<iframe sandbox=\"\" referrerpolicy=\"no-referrer\" title=\"%s, %d px, %s\" "
		"style=\"display:block;width:%dpx;height:720px;border:1px solid #8884;border-radius:6px;color-scheme: %s;background:%s\" srcdoc=\"",
		device, width, scheme, width, scheme, g_strcmp0(scheme, "dark") == 0 ? "#121212" : "#ffffff");
	venture_html_escape_append(content, doc);
	g_string_append(content, "\"></iframe></figure>");
}

static const gchar *
venture_web_mail_preview_status(VentureMailCheckStatus status)
{
	return status == VENTURE_MAIL_CHECK_OK ? "ok" : status == VENTURE_MAIL_CHECK_WARN ? "warn" : "fail";
}

static HtmxResponse *
venture_web_mail_preview_page(VentureWebServer *self, HtmxRequest *request, const gchar *kind_id,
	const VentureMailPreviewKind *kind, gint64 id, const VentureMailPreview *preview, const gchar *notice)
{
	g_autoptr(GString) content = g_string_new(NULL);
	g_autofree gchar *base = g_strdup_printf("/mail/preview/%s/%" G_GINT64_FORMAT, kind_id, id);
	g_autofree gchar *token = g_uuid_string_random();
	const gchar *frame = htmx_request_get_query_param(request, "frame");
	const gchar *scheme_param = htmx_request_get_query_param(request, "scheme");
	guint i, s;
	/* One frame alone, the whole window: for looking at a single width,
	 * and for taking a picture of it. */
	if (frame != NULL) {
		gint width = (gint)g_ascii_strtoll(frame, NULL, 10);
		const gchar *scheme = g_strcmp0(scheme_param, "dark") == 0 ? "dark" : "light";
		const gchar *device = "Desktop";
		for (i = 0; i < G_N_ELEMENTS(venture_web_mail_preview_widths); i++)
			if (venture_web_mail_preview_widths[i] == width)
				device = venture_web_mail_preview_devices[i];
		if (width < 320 || width > 2560) width = 768;
		g_string_append_printf(content, "<!doctype html><html><head><meta charset=\"utf-8\"><title>Mail preview</title>"
			"<style>html,body{margin:0;background:%s;color:%s;font:13px sans-serif;color-scheme:%s}"
			"figure{margin:0!important}figcaption{padding:4px 8px}iframe{height:calc(100vh - 28px)!important;border:0!important;border-radius:0!important}</style></head><body>",
			g_strcmp0(scheme, "dark") == 0 ? "#121212" : "#ffffff", g_strcmp0(scheme, "dark") == 0 ? "#ddd" : "#222", scheme);
		venture_web_mail_preview_frame(content, preview, width, device, scheme, NULL);
		g_string_append(content, "</body></html>");
		return venture_web_html_response(g_strdup(content->str), 200);
	}
	g_string_append(content, "<h1>Mail preview: ");
	venture_html_escape_append(content, kind != NULL ? kind->label : "Queued message");
	g_string_append_printf(content, " #%" G_GINT64_FORMAT "</h1>", id);
	if (notice != NULL) {
		g_string_append(content, "<p role=\"status\">");
		g_string_append(content, notice);
		g_string_append(content, "</p>");
	}
	g_string_append(content, "<dl class=\"mail-preview-envelope\"><dt>To</dt><dd>");
	venture_html_escape_append(content, preview->to);
	g_string_append(content, "</dd><dt>Subject</dt><dd>");
	venture_html_escape_append(content, preview->subject);
	g_string_append(content, "</dd>");
	if (preview->headers->len > 0) {
		g_string_append(content, "<dt>Headers</dt><dd>");
		for (i = 0; i < preview->headers->len; i++) {
			if (i) g_string_append(content, "<br>");
			venture_html_escape_append(content, g_ptr_array_index(preview->headers, i));
		}
		g_string_append(content, "</dd>");
	}
	if (preview->attachments->len > 0) {
		g_string_append(content, "<dt>Attachments</dt><dd>");
		for (i = 0; i < preview->attachments->len; i++) {
			if (i) g_string_append(content, ", ");
			venture_html_escape_append(content, g_ptr_array_index(preview->attachments, i));
		}
		g_string_append(content, "</dd>");
	}
	g_string_append(content, "</dl>");
	g_string_append(content, "<h2>Pre-send checks</h2><ul class=\"mail-preview-checks\">");
	for (i = 0; i < preview->checks->len; i++) {
		const VentureMailCheck *check = g_ptr_array_index(preview->checks, i);
		g_string_append_printf(content, "<li data-check=\"%s\" data-status=\"%s\"><strong>%s</strong> <code>%s</code> ",
			check->id, venture_web_mail_preview_status(check->status), venture_web_mail_preview_status(check->status), check->id);
		venture_html_escape_append(content, check->message);
		g_string_append(content, "</li>");
	}
	g_string_append_printf(content, "</ul><p>Links are checked for form only. "
		"<a href=\"%s?reachability=1\">Also ask each web link whether it answers</a> "
		"(one HEAD request each, at most %d, from this server).</p>", base, VENTURE_MAIL_PREVIEW_MAX_REACHABILITY);
	g_string_append_printf(content, "<form method=\"post\" action=\"%s/test\"><input type=\"hidden\" name=\"token\" value=\"%s\">"
		"<button class=\"btn\" type=\"submit\">Send me a test</button> "
		"A copy goes to your own address only, the subject marked [TEST] and every private link replaced by a placeholder.</form>",
		base, token);
	g_string_append(content, "<h2>How it renders</h2><p>Private links -- pay, accept, portal, unsubscribe and tracking links -- "
		"are replaced by <code>" VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL "</code>. Dark is a simulation: the frame's colour "
		"scheme is set to dark, so the message's own <code>prefers-color-scheme: dark</code> rules apply. Real mail apps "
		"differ, and some invert colours by themselves. The email runs in a sandbox with no scripts.</p>");
	for (s = 0; s < 2; s++) {
		const gchar *scheme = s == 0 ? "light" : "dark";
		g_string_append_printf(content, "<h3>%s</h3>", s == 0 ? "Light" : "Dark");
		for (i = 0; i < G_N_ELEMENTS(venture_web_mail_preview_widths); i++)
			venture_web_mail_preview_frame(content, preview, venture_web_mail_preview_widths[i],
				venture_web_mail_preview_devices[i], scheme, base);
	}
	g_string_append(content, "<h2>Plain-text alternative</h2><pre class=\"mail-preview-text\" style=\"white-space:pre-wrap\">");
	venture_html_escape_append(content, preview->text);
	g_string_append(content, "</pre>");
	return venture_web_html_response(venture_web_page(self, request, "/e/mail_message", "Mail preview", content->str), 200);
}

/* GET /mail/preview/:kind/:id, and /mail/preview/message/:id for a queued row. */
static HtmxResponse *
venture_web_mail_preview(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMailPreview) preview = NULL;
	const VentureMailPreviewKind *kind = NULL;
	gint64 org = 0, id = 0;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "mail");
	if (gate != NULL) return gate;
	if (!venture_web_mail_preview_resolve(self, request, params, &kind, &org, &id, &error))
		return venture_web_error_response(error);
	preview = venture_web_mail_preview_make(self, request, kind, org, id, &error);
	if (preview == NULL)
		return venture_web_error_response(error);
	return venture_web_mail_preview_page(self, request, g_hash_table_lookup(params, "kind"), kind, id, preview, NULL);
}

/* POST /mail/preview/:kind/:id/test - one copy to the person pressing it,
 * through the ordinary outbox, then delivered at once like the SMTP test. */
static HtmxResponse *
venture_web_mail_preview_test(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMailPreview) preview = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(VentureEntity) outcome = NULL;
	g_autoptr(GString) notice = g_string_new(NULL);
	g_autofree gchar *to = NULL, *state = NULL, *reason = NULL;
	const VentureMailPreviewKind *kind = NULL;
	const gchar *token = htmx_request_get_form_value(request, "token");
	gint64 org = 0, id = 0;
	VentureActor actor;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "mail");
	if (gate != NULL) return gate;
	if (!venture_web_mail_preview_resolve(self, request, params, &kind, &org, &id, &error))
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (kind != NULL)
		queued = venture_mail_preview_send_test(self->context, kind->id, org, id, token, &actor, &error);
	else if (token == NULL || !g_uuid_string_is_valid(token))
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Reload the preview to send another test");
	else {
		g_autoptr(VentureEntity) row = venture_database_get(database, VENTURE_TYPE_MAIL_MESSAGE, id, &error);
		g_autofree gchar *key = g_strdup_printf("mail-preview-test:message:%" G_GINT64_FORMAT ":%s", id, token);
		if (row != NULL)
			queued = venture_mail_preview_enqueue_test(database, VENTURE_MAIL_MESSAGE(row), key, NULL, 0, &actor, &error);
	}
	if (queued == NULL)
		return venture_web_error_response(error);
	/* Only this message; a repeated click finds it already sent. */
	if (venture_mail_outbox_deliver_one(venture_context_get_mail_outbox(self->context), org,
		venture_entity_get_id(VENTURE_ENTITY(queued)), 0, 0, NULL, &error) < 0)
		return venture_web_error_response(error);
	outcome = venture_database_get(database, VENTURE_TYPE_MAIL_MESSAGE, venture_entity_get_id(VENTURE_ENTITY(queued)), NULL);
	g_object_get(outcome != NULL ? outcome : VENTURE_ENTITY(queued), "to", &to, "state", &state, "last-error", &reason, NULL);
	g_string_append(notice, "A test copy went to ");
	venture_html_escape_append(notice, to);
	g_string_append(notice, " through the outbox: ");
	venture_html_escape_append(notice, state);
	if (!venture_string_is_empty(reason)) {
		g_string_append(notice, " -- ");
		venture_html_escape_append(notice, reason);
	}
	g_string_append_printf(notice, ". <a href=\"/e/mail_message/%" G_GINT64_FORMAT "\">Delivery record</a>",
		venture_entity_get_id(VENTURE_ENTITY(queued)));
	preview = venture_web_mail_preview_make(self, request, kind, org, id, &error);
	if (preview == NULL)
		return venture_web_error_response(error);
	return venture_web_mail_preview_page(self, request, g_hash_table_lookup(params, "kind"), kind, id, preview, notice->str);
}

/* GET /api/v1/mail/preview/:kind/:id - the same preview as JSON. */
static HtmxResponse *
venture_web_api_mail_preview(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMailPreview) preview = NULL;
	g_autoptr(JsonNode) node = NULL;
	const VentureMailPreviewKind *kind = NULL;
	gint64 org = 0, id = 0;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "mail");
	if (gate != NULL) return gate;
	if (!venture_web_mail_preview_resolve(self, request, params, &kind, &org, &id, &error))
		return venture_web_error_response(error);
	preview = venture_web_mail_preview_make(self, request, kind, org, id, &error);
	if (preview == NULL)
		return venture_web_error_response(error);
	node = venture_mail_preview_to_json(preview);
	json_object_set_int_member(json_node_get_object(node), "source_id", id);
	json_object_set_int_member(json_node_get_object(node), "organization_id", org);
	return venture_web_json_response(node, 200);
}
