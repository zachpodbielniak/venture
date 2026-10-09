/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Hosted sign-in consumes only links established by verified enrollment. It
 * neither provisions an account nor infers identity from an email address. */
gboolean venture_oidc_service_hosted_enabled(VentureOidcService *self)
{
	g_autofree gchar *client = NULL, *issuer = NULL;
	gboolean required = FALSE, secure = FALSE, hosted = FALSE;
	g_return_val_if_fail(VENTURE_IS_OIDC_SERVICE(self), FALSE);
	if (!self->database || !self->config) return FALSE;
	g_object_get(self->config, "hosted-identity-browser-client", &client,
		"hosted-identity-issuer", &issuer, "hosted-enabled", &hosted,
		"security-require-auth", &required, "security-cookie-secure", &secure, NULL);
	return hosted && required && secure && issuer && *issuer && client && *client && strlen(client) <= 255 &&
		venture_tenant_service_trusts_identity(venture_tenant_service_get(self->database), issuer);
}
static gchar *hosted_callback(VentureOidcService *self, GError **error)
{
	g_autofree gchar *origin = NULL;
	g_autoptr(GUri) uri = NULL;
	if (!venture_oidc_service_hosted_enabled(self)) { denied(error); return NULL; }
	g_object_get(self->config, "hosted-origin", &origin, NULL);
	uri = origin ? g_uri_parse(origin, G_URI_FLAGS_NONE, NULL) : NULL;
	if (!uri || !safe_uri(self, origin) || g_strcmp0(g_uri_get_scheme(uri), "https") ||
		(*g_uri_get_path(uri) && strcmp(g_uri_get_path(uri), "/"))) { denied(error); return NULL; }
	if (g_str_has_suffix(origin, "/")) origin[strlen(origin) - 1] = 0;
	return g_strconcat(origin, "/auth/hosted/callback", NULL);
}
static gboolean hosted_return_valid(const gchar *target)
{
	g_autofree gchar *decoded = NULL;
	const gchar *p;
	if (!target || !*target) return TRUE;
	if (strlen(target) > 2048) return FALSE;
	decoded = g_uri_unescape_string(target, NULL);
	if (!decoded || decoded[0] != '/' || decoded[1] == '/' ||
		g_str_has_prefix(decoded, "/auth/") || g_str_has_prefix(decoded, "/login")) return FALSE;
	for (p = decoded; *p; p++)
		if (*p == '\\' || (guchar)*p <= 32 || (guchar)*p == 127) return FALSE;
	return TRUE;
}
static VentureEntity *hosted_identity(VentureOidcService *self, gint64 id, GError **error)
{
	g_autoptr(VentureEntity) link = NULL;
	g_autofree gchar *issuer = NULL;
	gboolean active = FALSE;
	if (!venture_oidc_service_hosted_enabled(self) || id <= 0) { denied(error); return NULL; }
	link = venture_database_get(self->database, VENTURE_TYPE_TENANT_IDENTITY, id, NULL);
	if (link) g_object_get(link, "active", &active, "issuer", &issuer, NULL);
	if (!link || !active || venture_entity_is_deleted(link) ||
		!venture_tenant_service_trusts_identity(venture_tenant_service_get(self->database), issuer)) { denied(error); return NULL; }
	return g_steal_pointer(&link);
}
static VentureUser *hosted_user(VentureOidcService *self, gint64 id, GError **error)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	g_autoptr(VentureEntity) link = hosted_identity(self, id, error), user = NULL;
	gint64 user_id = 0;
	gboolean active = FALSE;
	if (!link) return NULL;
	g_object_get(link, "user-id", &user_id, NULL);
	user = venture_database_get(self->database, VENTURE_TYPE_USER, user_id, NULL);
	if (user) g_object_get(user, "active", &active, NULL);
	if (!user || !active || venture_entity_is_deleted(user)) { denied(error); return NULL; }
	/* Membership and role are still checked by the normal hosted request gate. */
	return VENTURE_USER(g_steal_pointer(&user));
}
static gchar *hosted_session_binding(VentureOidcService *self, gint64 id, GError **error)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	g_autoptr(VentureEntity) link = hosted_identity(self, id, error);
	g_autofree gchar *issuer = NULL, *client = NULL;
	if (!link) return NULL;
	g_object_get(self->config, "hosted-identity-issuer", &issuer, "hosted-identity-browser-client", &client, NULL);
	return g_strdup_printf("hosted:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s:%s",
		id, venture_entity_get_version(link), issuer, client);
}
gchar *venture_oidc_service_begin_hosted(VentureOidcService *self, const gchar *target,
	const gchar *remote, gchar **browser_token, GError **error)
{
	g_autofree gchar *callback = NULL, *issuer = NULL, *client = NULL, *discovery_uri = NULL;
	g_autofree gchar *peer = NULL, *state = NULL, *s256 = NULL, *form = NULL;
	g_autoptr(JsonNode) discovery = NULL;
	JsonObject *metadata;
	const gchar *authorize, *token, *jwks;
	Attempt *a;
	g_return_val_if_fail(VENTURE_IS_OIDC_SERVICE(self), NULL);
	g_return_val_if_fail(browser_token != NULL, NULL);
	*browser_token = NULL;
	if (!hosted_return_valid(target)) { fail(error, VENTURE_ERROR_INVALID_ARGUMENT, "Choose a page in this workspace"); return NULL; }
	callback = hosted_callback(self, error); if (!callback) return NULL;
	peer = g_compute_checksum_for_string(G_CHECKSUM_SHA256, remote ? remote : "", -1);
	if (!budget(self, peer, error)) return NULL;
	g_object_get(self->config, "hosted-identity-issuer", &issuer, "hosted-identity-browser-client", &client, NULL);
	discovery_uri = g_strconcat(issuer, g_str_has_suffix(issuer, "/") ? ".well-known/openid-configuration" : "/.well-known/openid-configuration", NULL);
	discovery = http_json(discovery_uri, NULL, error); if (!discovery) return NULL;
	metadata = json_node_get_object(discovery);
	authorize = member(metadata, "authorization_endpoint"); token = member(metadata, "token_endpoint"); jwks = member(metadata, "jwks_uri");
	if (g_strcmp0(member(metadata, "issuer"), issuer) || !same_origin(self, issuer, authorize) ||
		!same_origin(self, issuer, token) || !same_origin(self, issuer, jwks) ||
		!contains(metadata, "response_types_supported", "code") || !contains(metadata, "code_challenge_methods_supported", "S256")) { denied(error); return NULL; }
	/* Public clients have no client-authentication method. Providers may
	 * advertise only confidential methods; the registered client enforces
	 * PKCE and the exchange below always supplies its original verifier. */
	a = g_new0(Attempt, 1); a->hosted = TRUE;
	a->expires = g_get_monotonic_time() + OIDC_ATTEMPT_SECONDS * G_USEC_PER_SEC;
	a->issuer = g_strdup(issuer); a->client = g_strdup(client); a->token_uri = g_strdup(token);
	a->return_to = g_strdup(target && *target ? target : "/");
	a->nonce = venture_generate_token(32); a->verifier = venture_generate_token(32); a->peer_hash = g_steal_pointer(&peer);
	a->validator = venture_oidc_verifier_new(issuer, client, jwks, error);
	if (!a->validator) { attempt_free(a); return NULL; }
	*browser_token = venture_generate_token(32);
	a->browser_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, *browser_token, -1);
	state = venture_generate_token(32); s256 = challenge(a->verifier);
	form = soup_form_encode("response_type", "code", "scope", "openid", "client_id", client,
		"redirect_uri", callback, "state", state, "nonce", a->nonce, "code_challenge", s256, "code_challenge_method", "S256", NULL);
	g_hash_table_insert(self->attempts, g_steal_pointer(&state), a);
	return g_strconcat(authorize, "?", form, NULL);
}
VentureTenantIdentity *venture_oidc_service_finish_hosted(VentureOidcService *self,
	const gchar *state, const gchar *code, const gchar *browser, gchar **return_to, GError **error)
{
	gpointer stored_key = NULL, stored_value = NULL;
	Attempt *a;
	g_autofree gchar *browser_hash = NULL, *callback = NULL, *issuer = NULL, *client = NULL, *form = NULL, *subject = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) link = NULL;
	g_autoptr(VentureUser) user = NULL;
	VentureTenantIdentity *result = NULL;
	const gchar *token;
	g_return_val_if_fail(VENTURE_IS_OIDC_SERVICE(self), NULL);
	g_return_val_if_fail(return_to != NULL, NULL);
	*return_to = NULL;
	if (!state || strlen(state) != 64 || !g_hash_table_steal_extended(self->attempts, state, &stored_key, &stored_value)) { denied(error); return NULL; }
	g_free(stored_key); a = stored_value;
	if (!a->hosted || !code || !*code || strlen(code) > 4096 || !browser || strlen(browser) != 64 ||
		a->expires <= g_get_monotonic_time()) { denied(error); goto done; }
	browser_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, browser, -1);
	if (!venture_constant_time_equal(browser_hash, a->browser_hash)) { denied(error); goto done; }
	callback = hosted_callback(self, error); if (!callback) goto done;
	g_object_get(self->config, "hosted-identity-issuer", &issuer, "hosted-identity-browser-client", &client, NULL);
	if (g_strcmp0(issuer, a->issuer) || g_strcmp0(client, a->client)) { denied(error); goto done; }
	form = soup_form_encode("grant_type", "authorization_code", "code", code, "client_id", a->client,
		"redirect_uri", callback, "code_verifier", a->verifier, NULL);
	response = http_json(a->token_uri, form, error); if (!response) goto done;
	token = member(json_node_get_object(response), "id_token");
	if (!token || !*token) { denied(error); goto done; }
	subject = venture_oidc_verifier_subject(a->validator, a->issuer, a->client, token, a->nonce, error); if (!subject) goto done;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	query = venture_query_new(VENTURE_TYPE_TENANT_IDENTITY);
	venture_query_add_filter_string(query, "issuer", VENTURE_FILTER_OP_EQ, a->issuer, NULL);
	venture_query_add_filter_string(query, "subject", VENTURE_FILTER_OP_EQ, subject, NULL);
	link = venture_database_find_one(self->database, query, NULL);
	if (!link) { denied(error); goto done; }
	user = hosted_user(self, venture_entity_get_id(link), error); if (!user) goto done;
	*return_to = g_strdup(a->return_to);
	result = VENTURE_TENANT_IDENTITY(g_steal_pointer(&link));
done:
	attempt_free(a); return result;
}
