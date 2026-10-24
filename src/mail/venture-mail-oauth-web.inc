/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included beside the web adapters. Token mailbox sign-in: start, the
 * provider's callback, and the operator's client credential. */

static gchar *
venture_web_mail_oauth_redirect(VentureWebServer *self)
{
	g_autofree gchar *configured = NULL;
	const gchar *base;
	g_object_get(venture_context_get_config(self->context), "server-base-url", &configured, NULL);
	base = !venture_string_is_empty(configured) ? configured : venture_web_server_get_base_url(self);
	return g_strdup_printf("%s/mail/oauth/callback", base != NULL ? base : "");
}

/* GET /mail_accounts/:id/oauth/start -- off to the provider's sign-in. */
static HtmxResponse *
venture_web_mail_oauth_start(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *redirect = NULL, *url = NULL;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "mail_sync");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	account = venture_database_get(db, VENTURE_TYPE_MAIL_ACCOUNT, g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (account == NULL)
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Mail account not found");
		return venture_web_error_response(error);
	}
	redirect = venture_web_mail_oauth_redirect(self);
	url = venture_mail_oauth_begin(db, venture_context_get_config(self->context), account, redirect,
		principal != NULL ? principal->user_id : 0, &error);
	if (url == NULL)
		return venture_web_error_response(error);
	return venture_web_redirect_to(url);
}

/* GET /mail/oauth/callback?state=...&code=... -- the provider sends the
 * browser back; only the person who started the sign-in may finish it. */
static HtmxResponse *
venture_web_mail_oauth_callback(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	const gchar *state = htmx_request_get_query_param(request, "state");
	const gchar *code = htmx_request_get_query_param(request, "code");
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	VentureActor actor;
	gint64 account;
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "mail_sync");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (htmx_request_get_query_param(request, "error") != NULL && state != NULL)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
			"The provider did not grant access to the mailbox. Nothing was changed; start the sign-in again.");
		return venture_web_error_response(error);
	}
	account = venture_mail_oauth_complete(venture_context_get_database(self->context), venture_context_get_config(self->context),
		state, code, principal != NULL ? principal->user_id : 0, &actor, &error);
	if (account == 0)
		return venture_web_error_response(error);
	path = g_strdup_printf("/connectors/mail_account/%" G_GINT64_FORMAT "/settings", account);
	return venture_web_redirect_to(path);
}

/* POST /mail_oauth_providers/:id/client -- the operator's client id and
 * secret, stored encrypted; the service requires organization admin. */
static HtmxResponse *
venture_web_mail_oauth_client(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) provider = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	VentureActor actor;
	if (gate != NULL)
		return gate;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "mail_sync");
	if (gate != NULL)
		return gate;
	provider = venture_database_get(db, VENTURE_TYPE_MAIL_OAUTH_PROVIDER, g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (provider == NULL)
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Sign-in provider not found");
		return venture_web_error_response(error);
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (!venture_mail_oauth_configure_client(db, venture_context_get_config(self->context), provider,
		htmx_request_get_form_value(request, "client_id"), htmx_request_get_form_value(request, "client_secret"), &actor, &error))
		return venture_web_error_response(error);
	path = g_strdup_printf("/e/mail_oauth_provider/%" G_GINT64_FORMAT, venture_entity_get_id(provider));
	return venture_web_redirect_to(path);
}

/* A provider's page takes its client credential (never shown back); an
 * account's page that names a provider offers the sign-in. */
static void
venture_web_mail_oauth_block(VentureWebServer *self, GString *content, VentureEntity *record)
{
	VentureDatabase *db = venture_context_get_database(self->context);
	if (!venture_web_module_enabled(self, "mail_sync"))
		return;
	if (VENTURE_IS_MAIL_OAUTH_PROVIDER(record))
	{
		gboolean configured = venture_mail_oauth_client_configured(db, record);
		g_string_append_printf(content, "<section class=\"card\"><div class=\"card-head\"><h2>Client credential</h2></div><div class=\"card-body\">"
			"<p>%s The secret is stored encrypted and never shown again.</p>"
			"<form method=\"post\" action=\"/mail_oauth_providers/%" G_GINT64_FORMAT "/client\">"
			"<label>Client ID<input name=\"client_id\" required autocomplete=\"off\"></label>"
			"<label>Client secret<input name=\"client_secret\" type=\"password\" autocomplete=\"new-password\"></label>"
			"<button class=\"btn\" type=\"submit\">%s</button></form></div></section>",
			configured ? "A client credential is set." : "No client credential is set, so nobody can sign in yet.",
			venture_entity_get_id(record), configured ? "Replace credential" : "Save credential");
	}
	else if (VENTURE_IS_MAIL_ACCOUNT(record))
	{
		gint64 provider_id = 0;
		g_autoptr(VentureEntity) provider = NULL;
		g_autofree gchar *name = NULL;
		g_object_get(record, "oauth-provider-id", &provider_id, NULL);
		if (provider_id <= 0)
			return;
		provider = venture_database_get(db, VENTURE_TYPE_MAIL_OAUTH_PROVIDER, provider_id, NULL);
		if (provider == NULL)
			return;
		g_object_get(provider, "name", &name, NULL);
		g_string_append_printf(content, "<p><a class=\"btn\" href=\"/mail_accounts/%" G_GINT64_FORMAT "/oauth/start\">Sign in with ",
			venture_entity_get_id(record));
		venture_html_escape_append(content, name != NULL ? name : "the provider");
		g_string_append(content, "</a> <span class=\"muted\">The mailbox then syncs with a token instead of a password.</span></p>");
	}
}
