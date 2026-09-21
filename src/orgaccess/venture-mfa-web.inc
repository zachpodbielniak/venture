/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Second-factor pages, included by the route owner so they share its page
 * shell, session gate and error policy. Every business rule lives in
 * VentureMfaService; these handlers only render and redirect.
 */

static const gchar *
venture_mfa_web_notice_text(const gchar *notice)
{
	if (0 == g_strcmp0(notice, "disabled")) return "Second factor turned off.";
	if (0 == g_strcmp0(notice, "wrong")) return "That code did not match. Check the app's clock and try the next one.";
	if (0 == g_strcmp0(notice, "password")) return "The current password did not match.";
	if (0 == g_strcmp0(notice, "failed")) return "That did not work. Try again.";
	return NULL;
}

static gchar *
venture_mfa_web_issuer(VentureWebServer *self)
{
	gchar *title = NULL;
	g_object_get(venture_context_get_config(self->context), "ui-title", &title, NULL);
	if (venture_string_is_empty(title))
	{
		g_free(title);
		title = g_strdup("VENTURE");
	}
	return title;
}

static void
venture_mfa_web_append_notice(GString *content, HtmxRequest *request)
{
	const gchar *notice = htmx_request_get_query_param(request, "notice");
	const gchar *text = venture_mfa_web_notice_text(notice);
	if (NULL == text)
		return;
	g_string_append_printf(content, "<div class=\"banner %s\">", 0 == g_strcmp0(notice, "disabled") ? "banner-ok" : "banner-warn");
	venture_html_escape_append(content, text);
	g_string_append(content, "</div>");
}

static void
venture_mfa_web_append_codes(GString *content, GStrv codes)
{
	guint i;
	g_string_append(content, "<div class=\"card\"><div class=\"card-body\"><h2>Recovery codes</h2>"
		"<p>Each of these works once, in place of a code from the app, if the phone is lost. "
		"They are shown now and never again: write them down and keep them somewhere safe.</p>"
		"<ul class=\"recovery-codes mono\">");
	for (i = 0; NULL != codes && NULL != codes[i]; i++)
	{
		g_string_append(content, "<li><code>");
		venture_html_escape_append(content, codes[i]);
		g_string_append(content, "</code></li>");
	}
	g_string_append(content, "</ul><a class=\"btn btn-primary\" href=\"/account/mfa\">I have saved these</a></div></div>");
}

/* GET /account/mfa: where the factor stands, and the forms to change it. */
static HtmxResponse *
venture_mfa_web_ui_status(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureUserMfa) row = NULL;
	g_autoptr(GString) content = NULL;
	VentureMfaService *service;
	HtmxResponse *redirect;
	gboolean enabled = FALSE;
	(void)params;
	redirect = venture_web_ui_require_session(self, request);
	if (NULL != redirect)
		return redirect;
	if (!venture_web_module_enabled(self, "mfa"))
		return venture_web_redirect_to("/account");
	principal = venture_auth_authenticate(self->auth, request);
	service = venture_mfa_service_get(venture_context_get_database(self->context));
	row = venture_mfa_service_get_row(service, principal->user_id);
	if (NULL != row)
		g_object_get(row, "enabled", &enabled, NULL);
	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\"><h1>Second factor</h1></div></div>");
	venture_mfa_web_append_notice(content, request);
	g_string_append(content, "<div class=\"card\"><div class=\"card-body\">");
	if (principal->user_id <= 0)
		g_string_append(content, "<p class=\"muted\">This session is not tied to a user account, so there is no second factor to manage.</p>");
	else if (!enabled)
	{
		g_string_append(content, "<p>Sign-in needs only a password. Adding a code from an authenticator app "
			"means a stolen password alone cannot open the books.</p>"
			"<form method=\"post\" action=\"/account/mfa/enrol\">"
			"<button class=\"btn btn-primary\" type=\"submit\">Set up an authenticator app</button></form>");
	}
	else
	{
		g_autoptr(GDateTime) enrolled = NULL;
		g_autofree gchar *since = NULL;
		g_object_get(row, "enrolled-at", &enrolled, NULL);
		since = venture_time_to_date_string(enrolled, NULL);
		g_string_append(content, "<p>A code from your authenticator app is required at every sign-in");
		if (NULL != since)
		{
			g_string_append(content, ", since ");
			venture_html_escape_append(content, since);
		}
		g_string_append(content, ".</p>"
			"<h2>New recovery codes</h2>"
			"<p class=\"muted small\">Replaces every unused code with ten new ones.</p>"
			"<form method=\"post\" action=\"/account/mfa/recovery\">"
			"<label>Code from the app<input name=\"code\" inputmode=\"numeric\" autocomplete=\"one-time-code\" required></label>"
			"<button class=\"btn\" type=\"submit\">Generate new recovery codes</button></form>"
			"<h2>Turn off</h2>"
			"<p class=\"muted small\">Needs the current password and a code, so somebody at an unlocked browser cannot do it.</p>"
			"<form method=\"post\" action=\"/account/mfa/disable\">"
			"<label>Current password<input type=\"password\" name=\"current_password\" autocomplete=\"current-password\" required></label>"
			"<label>Code from the app or a recovery code<input name=\"code\" autocomplete=\"one-time-code\" required></label>"
			"<button class=\"btn btn-danger\" type=\"submit\">Turn off the second factor</button></form>");
	}
	g_string_append(content, "</div></div>");
	return venture_web_html_response(venture_web_page(self, request, "/account", "Second factor", content->str), 200);
}

/* Renders a pending enrolment: the QR code, the typed secret, the confirm form. */
static HtmxResponse *
venture_mfa_web_render_enrolment(VentureWebServer *self, HtmxRequest *request, VentureMfaEnrolment *enrolment)
{
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><div class=\"page-title\"><h1>Set up your authenticator app</h1></div></div>");
	venture_mfa_web_append_notice(content, request);
	g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
		"<p>Scan this with an authenticator app (any that speaks TOTP), then enter the six-digit code it shows to finish.</p>"
		"<div class=\"qr\">");
	/* The SVG is generated in-process from the URI and contains only its
	 * own markup; nothing user-supplied reaches it unescaped. */
	g_string_append(content, enrolment->svg);
	g_string_append(content, "</div><p class=\"muted small\">Cannot scan? Enter this key by hand: <code>");
	venture_html_escape_append(content, enrolment->secret);
	g_string_append(content, "</code></p>"
		"<form method=\"post\" action=\"/account/mfa/confirm\">"
		"<label>Code from the app<input name=\"code\" inputmode=\"numeric\" autocomplete=\"one-time-code\" autofocus required></label>"
		"<button class=\"btn btn-primary\" type=\"submit\">Turn on the second factor</button></form>"
		"</div></div>");
	return venture_web_html_response(venture_web_page(self, request, "/account", "Second factor", content->str), 200);
}

/* GET /account/mfa/enrol: the pending enrolment, started here if there is
 * none, which is what the enrolment gate sends an admin to. */
static HtmxResponse *
venture_mfa_web_ui_enrol(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureMfaEnrolment) enrolment = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *issuer = NULL;
	g_autofree gchar *username = NULL;
	VentureMfaService *service;
	HtmxResponse *redirect;
	VentureActor actor;
	(void)params;
	redirect = venture_web_ui_require_session(self, request);
	if (NULL != redirect)
		return redirect;
	if (!venture_web_module_enabled(self, "mfa"))
		return venture_web_redirect_to("/account");
	principal = venture_auth_authenticate(self->auth, request);
	user = venture_web_lookup_user(self, principal->user_id);
	if (NULL == user)
		return venture_web_redirect_to("/account/mfa");
	if (venture_mfa_service_is_enabled(venture_mfa_service_get(venture_context_get_database(self->context)), principal->user_id))
		return venture_web_redirect_to("/account/mfa");
	g_object_get(user, "username", &username, NULL);
	issuer = venture_mfa_web_issuer(self);
	service = venture_mfa_service_get(venture_context_get_database(self->context));
	enrolment = venture_mfa_service_pending_enrolment(service, principal->user_id, issuer, username, NULL);
	if (NULL == enrolment)
	{
		venture_auth_to_actor(principal, &actor);
		enrolment = venture_mfa_service_begin_enrolment(service, principal->user_id, issuer, username, &actor, &error);
	}
	if (NULL == enrolment)
		return venture_web_error_response(error);
	return venture_mfa_web_render_enrolment(self, request, enrolment);
}

/* POST /account/mfa/enrol: mint a fresh secret, replacing a pending one. */
static HtmxResponse *
venture_mfa_web_ui_enrol_start(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureMfaEnrolment) enrolment = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *issuer = NULL;
	g_autofree gchar *username = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	(void)params;
	redirect = venture_web_ui_require_session(self, request);
	if (NULL != redirect)
		return redirect;
	if (!venture_web_module_enabled(self, "mfa"))
		return venture_web_redirect_to("/account");
	principal = venture_auth_authenticate(self->auth, request);
	user = venture_web_lookup_user(self, principal->user_id);
	if (NULL == user)
		return venture_web_redirect_to("/account/mfa?notice=failed");
	g_object_get(user, "username", &username, NULL);
	issuer = venture_mfa_web_issuer(self);
	venture_auth_to_actor(principal, &actor);
	enrolment = venture_mfa_service_begin_enrolment(venture_mfa_service_get(venture_context_get_database(self->context)),
		principal->user_id, issuer, username, &actor, &error);
	if (NULL == enrolment)
		return venture_web_error_response(error);
	return venture_web_redirect_to("/account/mfa/enrol");
}

/* POST /account/mfa/confirm: the first valid code enables the factor and
 * shows the recovery codes exactly once. */
static HtmxResponse *
venture_mfa_web_ui_confirm(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) codes = NULL;
	g_autoptr(GString) content = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	(void)params;
	redirect = venture_web_ui_require_session(self, request);
	if (NULL != redirect)
		return redirect;
	if (!venture_web_module_enabled(self, "mfa"))
		return venture_web_redirect_to("/account");
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	codes = venture_mfa_service_confirm_enrolment(venture_mfa_service_get(venture_context_get_database(self->context)),
		principal->user_id, htmx_request_get_form_value(request, "code"), &actor, &error);
	if (NULL == codes)
	{
		if (VENTURE_ERROR_UNAUTHENTICATED == error->code)
			return venture_web_redirect_to("/account/mfa/enrol?notice=wrong");
		return venture_web_error_response(error);
	}
	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\"><h1>Second factor is on</h1></div></div>"
		"<div class=\"banner banner-ok\">From now on, sign-in asks for a code from the app after the password.</div>");
	venture_mfa_web_append_codes(content, codes);
	return venture_web_html_response(venture_web_page(self, request, "/account", "Second factor", content->str), 200);
}

/* POST /account/mfa/disable: the password and a code, then off. */
static HtmxResponse *
venture_mfa_web_ui_disable(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *remote = NULL;
	VentureMfaService *service;
	HtmxResponse *redirect;
	VentureActor actor;
	(void)params;
	redirect = venture_web_ui_require_session(self, request);
	if (NULL != redirect)
		return redirect;
	if (!venture_web_module_enabled(self, "mfa"))
		return venture_web_redirect_to("/account");
	principal = venture_auth_authenticate(self->auth, request);
	user = venture_web_lookup_user(self, principal->user_id);
	if (NULL == user)
		return venture_web_redirect_to("/account/mfa?notice=failed");
	/* The password again, for the same reason the password change asks:
	 * an unlocked browser must not be enough to weaken the account. */
	if (!venture_user_check_password(user, htmx_request_get_form_value(request, "current_password")))
		return venture_web_redirect_to("/account/mfa?notice=password");
	venture_auth_to_actor(principal, &actor);
	remote = venture_auth_remote_address(request);
	service = venture_mfa_service_get(venture_context_get_database(self->context));
	if (!venture_mfa_service_verify(service, principal->user_id, htmx_request_get_form_value(request, "code"), &actor, remote, &error))
		return venture_web_redirect_to("/account/mfa?notice=wrong");
	if (!venture_mfa_service_disable(service, principal->user_id, &actor, &error))
		return venture_web_error_response(error);
	return venture_web_redirect_to("/account/mfa?notice=disabled");
}

/* POST /account/mfa/recovery: a code, then ten new recovery codes. */
static HtmxResponse *
venture_mfa_web_ui_recovery(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) codes = NULL;
	g_autofree gchar *remote = NULL;
	g_autoptr(GString) content = NULL;
	VentureMfaService *service;
	HtmxResponse *redirect;
	VentureActor actor;
	(void)params;
	redirect = venture_web_ui_require_session(self, request);
	if (NULL != redirect)
		return redirect;
	if (!venture_web_module_enabled(self, "mfa"))
		return venture_web_redirect_to("/account");
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	remote = venture_auth_remote_address(request);
	service = venture_mfa_service_get(venture_context_get_database(self->context));
	if (!venture_mfa_service_verify(service, principal->user_id, htmx_request_get_form_value(request, "code"), &actor, remote, &error))
		return venture_web_redirect_to("/account/mfa?notice=wrong");
	codes = venture_mfa_service_regenerate_recovery_codes(service, principal->user_id, &actor, &error);
	if (NULL == codes)
		return venture_web_error_response(error);
	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\"><h1>New recovery codes</h1></div></div>"
		"<div class=\"banner banner-ok\">The old codes no longer work.</div>");
	venture_mfa_web_append_codes(content, codes);
	return venture_web_html_response(venture_web_page(self, request, "/account", "Second factor", content->str), 200);
}

/* GET /login/mfa: the code prompt between the password and the session. */
static HtmxResponse *
venture_mfa_web_ui_login_form(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GString) body = NULL;
	g_autofree gchar *html = NULL;
	const gchar *notice;
	(void)params;
	if (0 == venture_auth_mfa_challenge_user(self->auth, request))
		return venture_web_redirect_to("/login");
	body = g_string_new(NULL);
	notice = htmx_request_get_query_param(request, "notice");
	if (NULL != notice)
	{
		g_string_append(body, "<div class=\"notice negative\"><span>");
		venture_html_escape_append(body, 0 == g_strcmp0(notice, "limited")
			? "Too many wrong codes; wait fifteen minutes and try again."
			: "That code is not valid.");
		g_string_append(body, "</span></div>");
	}
	g_string_append(body,
		"<form class=\"auth-form\" method=\"post\" action=\"/login/mfa\">"
		"<p>Enter the code from your authenticator app, or one of your recovery codes.</p>"
		"<div class=\"field\"><label for=\"c\">Code</label>"
		"<input id=\"c\" name=\"code\" inputmode=\"numeric\" autocomplete=\"one-time-code\" autofocus></div>"
		"<button class=\"btn btn-primary btn-lg\" type=\"submit\">Continue</button></form>"
		"<p class=\"muted small\"><a href=\"/login\">Start over</a></p>");
	html = venture_web_auth_page(self, request, body->str);
	return venture_web_html_response(g_steal_pointer(&html), 200);
}

/* POST /login/mfa: the code opens the session the password only promised. */
static HtmxResponse *
venture_mfa_web_ui_login_submit(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *clear = NULL;
	g_autofree gchar *remote = NULL;
	HtmxResponse *response;
	(void)params;
	remote = venture_auth_remote_address(request);
	if (!venture_auth_complete_mfa(self->auth, request, htmx_request_get_form_value(request, "code"), remote, &cookie, &error))
	{
		if (0 == venture_auth_mfa_challenge_user(self->auth, request))
			return venture_web_redirect_to("/login");
		return venture_web_redirect_to(NULL != strstr(error->message, "Too many") ? "/login/mfa?notice=limited" : "/login/mfa?notice=wrong");
	}
	clear = venture_auth_mfa_clear_cookie(self->auth);
	response = htmx_response_new();
	htmx_response_set_status(response, 302);
	htmx_response_add_header(response, "Location", "/");
	htmx_response_add_header(response, "Set-Cookie", cookie);
	/* A second Set-Cookie must be appended; add_header replaces. */
	htmx_response_append_header(response, "Set-Cookie", clear);
	return response;
}

static void
venture_mfa_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/account/mfa", VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_mfa_web_ui_status, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/account/mfa/enrol", VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_mfa_web_ui_enrol, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/account/mfa/enrol", VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_mfa_web_ui_enrol_start, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/account/mfa/confirm", VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_mfa_web_ui_confirm, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/account/mfa/disable", VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_mfa_web_ui_disable, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/account/mfa/recovery", VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_mfa_web_ui_recovery, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/login/mfa", VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_mfa_web_ui_login_form, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/login/mfa", VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_mfa_web_ui_login_submit, self);
}
