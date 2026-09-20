/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean public_base(const gchar *base, GError **error)
{
	g_autoptr(GUri) uri = NULL;
	const gchar *scheme, *host;
	if (!bounded(base, 2048) || strpbrk(base, "\r\n\t \"'<>")) return refuse(error, "A public HTTPS origin is required");
	uri = g_uri_parse(base, G_URI_FLAGS_NONE, NULL);
	if (!uri || g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri))
		return refuse(error, "Public origin must not contain credentials, a query or fragment");
	scheme = g_uri_get_scheme(uri); host = g_uri_get_host(uri);
	return (host && *host && (!g_strcmp0(scheme, "https") || (!g_strcmp0(scheme, "http") &&
		(!g_strcmp0(host, "localhost") || !g_strcmp0(host, "127.0.0.1") || !g_strcmp0(host, "::1"))))) ||
		refuse(error, "Public marketing links require HTTPS; HTTP is only for loopback development");
}
static gboolean make_recipient(VentureMarketingService *self, VentureEntity *send, VentureEntity *source,
	const gchar *source_key, const gchar *base, gboolean tracking, GHashTable *seen,
	gint64 *eligible, gint64 *excluded, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMarketingRecipient) recipient = venture_marketing_recipient_new();
	g_autoptr(VentureEntity) consent = NULL;
	g_autoptr(VentureMailTemplate) template = NULL;
	g_autoptr(VentureMailMessage) rendered = NULL;
	g_autofree gchar *raw = source ? string(source, "email") : NULL;
	g_autofree gchar *email = normalize(raw), *key = NULL;
	gint64 org = venture_entity_get_organization_id(send);
	const gchar *reason = "eligible";
	gboolean blocked = FALSE;
	key = g_strdup_printf("%s:%s", venture_entity_get_uuid(send), source_key);
	if (!source) reason = "source_missing";
	else if (!valid_address(email)) reason = "invalid_email";
	else {
		if (!suppressed(self, org, email, &blocked, error)) return FALSE;
		if (blocked) reason = "suppressed";
		else if (g_hash_table_contains(seen, email)) reason = "duplicate_address";
		else {
			consent = permission(self, source, email, error);
			if (error && *error) return FALSE;
			if (!consent) reason = "no_marketing_permission";
		}
	}
	g_object_set(recipient, "organization-id", org, "send-id", venture_entity_get_id(send),
		"recipient-key", key, "email", strlen(email) <= 254 ? email : "", "eligibility", reason,
		"state", !strcmp(reason, "eligible") ? "eligible" : "excluded", NULL);
	if (source) g_object_set(recipient, subject_field(G_OBJECT_TYPE(source)), venture_entity_get_id(source), NULL);
	if (strcmp(reason, "eligible")) { (*excluded)++; return save(self, VENTURE_ENTITY(recipient), actor, error); }
	{
		g_autofree gchar *subject_text = string(send, "subject"), *text = string(send, "text-body"), *html = string(send, "html-body");
		template = g_object_new(VENTURE_TYPE_MAIL_TEMPLATE, "organization-id", org, "subject", subject_text,
			"text-body", text, "html-body", html, NULL);
	}
	rendered = venture_mail_template_render(template, source, error);
	if (!rendered) return FALSE;
	{
		g_autofree gchar *subject_text = string(VENTURE_ENTITY(rendered), "subject"), *text = string(VENTURE_ENTITY(rendered), "text-body"), *html = string(VENTURE_ENTITY(rendered), "html-body");
		g_autofree gchar *token = venture_sequence_tracking_token(error), *hash = NULL, *url = NULL;
		g_autofree gchar *private_text = NULL, *private_html = NULL, *escaped_url = NULL;
		if (!token) return FALSE;
		if (!bounded(subject_text, 998) || strpbrk(subject_text, "\r\n") || (text && strlen(text) > 65536) || (html && strlen(html) > 65536))
			return refuse(error, "Rendered content exceeds message limits");
		hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
		url = g_strdup_printf("%s/marketing/u/%s", base, token);
		escaped_url = g_markup_escape_text(url, -1);
		private_text = g_strdup_printf("%s\n\nUnsubscribe from this organization's marketing: %s", text ? text : "", url);
		if (html && *html) private_html = g_strdup(html);
		else {
			g_autofree gchar *escaped = g_markup_escape_text(text ? text : "", -1);
			private_html = g_strdup_printf("<pre>%s</pre>", escaped);
		}
		if (tracking) {
			g_autofree gchar *tracking_token = venture_sequence_tracking_token(error), *tracking_hash = NULL, *tracking_base = NULL, *wrapped = NULL, *links = NULL;
			g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
			g_autoptr(JsonBuilder) builder = json_builder_new();
			g_autoptr(JsonNode) node = NULL;
			guint i;
			if (!tracking_token) return FALSE;
			tracking_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, tracking_token, -1);
			tracking_base = g_strconcat(base, "/marketing", NULL);
			wrapped = venture_sequence_tracking_wrap(private_html, tracking_base, tracking_token, urls);
			if (urls->len > 1000) return refuse(error, "A tracked message may contain at most 1000 destinations");
			json_builder_begin_array(builder);
			for (i = 0; i < urls->len; i++) json_builder_add_string_value(builder, g_ptr_array_index(urls, i));
			json_builder_end_array(builder); node = json_builder_get_root(builder); links = json_to_string(node, FALSE);
			g_object_set(recipient, "tracking-hash", tracking_hash, "links", links, NULL);
			g_free(private_html); private_html = g_steal_pointer(&wrapped);
		}
		{
			g_autofree gchar *with_footer = g_strdup_printf("%s<p><a href=\"%s\">Unsubscribe from this organization's marketing</a></p>", private_html, escaped_url);
			g_free(private_html); private_html = g_steal_pointer(&with_footer);
		}
		g_object_set(recipient, "consent-id", venture_entity_get_id(consent), "subject", subject_text,
			"text-body", text, "html-body", html, "private-text-body", private_text, "private-html-body", private_html,
			"unsubscribe-url", url, "unsubscribe-hash", hash, NULL);
	}
	if (!save(self, VENTURE_ENTITY(recipient), actor, error)) return FALSE;
	g_hash_table_add(seen, g_strdup(email)); (*eligible)++; return TRUE;
}
VentureMarketingSend *venture_marketing_service_preview(VentureMarketingService *self, gint64 org,
	gint64 send_id, const gchar *base_url, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) send = NULL, list = NULL, organization = NULL;
	g_autoptr(GPtrArray) audience = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autofree gchar *base = g_strdup(base_url);
	gint64 eligible = 0, excluded = 0;
	gboolean segment, tracking;
	guint i;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), NULL);
	if (!allowed(self, org, error) || !public_base(base, error)) return NULL;
	while (g_str_has_suffix(base, "/")) base[strlen(base) - 1] = '\0';
	if (!venture_database_begin(self->database, error)) return NULL;
	send = get_row(self, VENTURE_TYPE_MARKETING_SEND, org, send_id, error);
	if (!send) goto fail;
	if (choice(send, "state") != 0) { refuse(error, "Preview requires a new draft send"); goto fail; }
	list = get_row(self, VENTURE_TYPE_MARKETING_LIST, org, number(send, "list-id"), error);
	if (!list) goto fail;
	organization = get_row(self, VENTURE_TYPE_ORGANIZATION, org, org, error);
	if (!organization) goto fail;
	segment = choice(list, "mode") == 1;
	tracking = flag(send, "tracking") && flag(organization, "marketing-tracking");
	if (segment) {
		g_autoptr(VentureQuery) query = segment_query(list, error);
		if (!query) goto fail;
		audience = venture_database_find(self->database, query, error);
	} else audience = children(self, VENTURE_TYPE_MARKETING_MEMBER, org, "list-id", venture_entity_get_id(list), 10001, error);
	if (!audience) goto fail;
	if (audience->len > 10000) { refuse(error, "Preview is bounded to 10000 source records; narrow the segment or list"); goto fail; }
	for (i = 0; i < audience->len; i++) {
		VentureEntity *candidate = g_ptr_array_index(audience, i);
		g_autoptr(VentureEntity) source = NULL;
		g_autoptr(GError) missing = NULL;
		if (segment) source = g_object_ref(candidate);
		else source = subject(self, candidate, &missing);
		if (missing && !g_error_matches(missing, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) {
			g_propagate_error(error, g_steal_pointer(&missing)); goto fail;
		}
		if (!make_recipient(self, send, source, venture_entity_get_uuid(candidate), base, tracking,
			seen, &eligible, &excluded, actor, error)) goto fail;
	}
	g_object_set(send, "state", 1, "previewed-at", now, "eligible-count", eligible, "excluded-count", excluded, "public-base", base, NULL);
	if (!save(self, send, actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return VENTURE_MARKETING_SEND(g_steal_pointer(&send));
fail:
	venture_database_rollback(self->database); return NULL;
}
