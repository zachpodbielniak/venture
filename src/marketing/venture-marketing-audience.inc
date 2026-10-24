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
/* One arm's frozen message for one recipient: the rendered public bodies,
 * the private bodies with the unsubscribe link (and survey link), and the
 * tracking wrap with its frozen destinations. An A/B send composes both
 * arms with the same capability and tracking tokens, so whichever arm a
 * recipient finally gets, its links and pixel are that recipient's. */
typedef struct {
	gchar *subject, *text, *html, *private_text, *private_html, *links;
} Composed;
static void composed_clear(Composed *c)
{
	g_free(c->subject); g_free(c->text); g_free(c->html);
	g_free(c->private_text); g_free(c->private_html); g_free(c->links);
}
static gboolean compose(VentureMarketingService *self, VentureEntity *send, VentureEntity *source,
	const gchar *subject_template, const gchar *text_template, const gchar *html_template,
	const gchar *base, const gchar *url, const gchar *tracking_token, Composed *out, GError **error)
{
	g_autoptr(VentureMailTemplate) template = NULL;
	g_autoptr(VentureMailMessage) rendered = NULL;
	g_autofree gchar *escaped_url = g_markup_escape_text(url, -1);
	gint64 org = venture_entity_get_organization_id(send);
	template = g_object_new(VENTURE_TYPE_MAIL_TEMPLATE, "organization-id", org, "subject", subject_template,
		"text-body", text_template, "html-body", html_template, NULL);
	rendered = venture_mail_template_render(template, source, error);
	if (!rendered) return FALSE;
	out->subject = string(VENTURE_ENTITY(rendered), "subject");
	out->text = string(VENTURE_ENTITY(rendered), "text-body");
	out->html = string(VENTURE_ENTITY(rendered), "html-body");
	if (!bounded(out->subject, 998) || strpbrk(out->subject, "\r\n") || (out->text && strlen(out->text) > 65536) || (out->html && strlen(out->html) > 65536))
		return refuse(error, "Rendered content exceeds message limits");
	out->private_text = g_strdup_printf("%s\n\nUnsubscribe from this organization's marketing: %s", out->text ? out->text : "", url);
	if (out->html && *out->html) out->private_html = g_strdup(out->html);
	else {
		g_autofree gchar *escaped = g_markup_escape_text(out->text ? out->text : "", -1);
		out->private_html = g_strdup_printf("<pre>%s</pre>", escaped);
	}
	if (number(send, "survey-form-id") > 0)
	{
		g_autoptr(VentureEntity) form = venture_database_get(self->database, VENTURE_TYPE_FORM, number(send, "survey-form-id"), error);
		g_autoptr(GDateTime) now = venture_time_now();
		g_autoptr(GDateTime) expires = g_date_time_add_days(now, 7);
		g_autofree gchar *personal = NULL, *escaped = NULL, *joined = NULL;
		if (form == NULL) return FALSE;
		personal = venture_forms_personal_link(self->database, form, source, base, expires, now, error);
		if (personal == NULL) return FALSE;
		joined = g_strdup_printf("%s\n\nYour personal form: %s", out->private_text, personal);
		g_free(out->private_text); out->private_text = g_steal_pointer(&joined);
		escaped = g_markup_escape_text(personal, -1);
		joined = g_strdup_printf("%s<p><a href=\"%s\">Your personal form</a></p>", out->private_html, escaped);
		g_free(out->private_html); out->private_html = g_steal_pointer(&joined);
	}
	if (tracking_token) {
		g_autofree gchar *tracking_base = g_strconcat(base, "/marketing", NULL), *wrapped = NULL;
		g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
		g_autoptr(JsonBuilder) builder = json_builder_new();
		g_autoptr(JsonNode) node = NULL;
		guint i;
		wrapped = venture_sequence_tracking_wrap(out->private_html, tracking_base, tracking_token, urls);
		if (urls->len > 1000) return refuse(error, "A tracked message may contain at most 1000 destinations");
		json_builder_begin_array(builder);
		for (i = 0; i < urls->len; i++) json_builder_add_string_value(builder, g_ptr_array_index(urls, i));
		json_builder_end_array(builder); node = json_builder_get_root(builder); out->links = json_to_string(node, FALSE);
		g_free(out->private_html); out->private_html = g_steal_pointer(&wrapped);
	}
	{
		g_autofree gchar *with_footer = g_strdup_printf("%s<p><a href=\"%s\">Unsubscribe from this organization's marketing</a></p>", out->private_html, escaped_url);
		g_free(out->private_html); out->private_html = g_steal_pointer(&with_footer);
	}
	return TRUE;
}
static gboolean ab_test(VentureEntity *send)
{
	return number(send, "test-percent") > 0;
}
/* @forced: (nullable): a decision made before the ordinary ones, for a
 * resend's opened or too-soon recipients. */
static gboolean make_recipient(VentureMarketingService *self, VentureEntity *send, VentureEntity *source,
	const gchar *source_key, const gchar *base, gboolean tracking, GHashTable *seen, const gchar *forced,
	gint64 *eligible, gint64 *excluded, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMarketingRecipient) recipient = venture_marketing_recipient_new();
	g_autoptr(VentureEntity) consent = NULL;
	g_autofree gchar *raw = source ? string(source, "email") : NULL;
	g_autofree gchar *email = normalize(raw), *key = NULL;
	gint64 org = venture_entity_get_organization_id(send);
	const gchar *reason = "eligible";
	gboolean blocked = FALSE;
	if (text_send(send))
		return make_text_recipient(self, send, source, source_key, seen, eligible, excluded, actor, error);
	key = g_strdup_printf("%s:%s", venture_entity_get_uuid(send), source_key);
	if (!source) reason = "source_missing";
	else if (number(send, "survey-form-id") > 0 && !VENTURE_IS_CONTACT(source)) reason = "form_requires_contact";
	else if (!valid_address(email)) reason = "invalid_email";
	else {
		if (!suppressed(self, org, email, &blocked, error)) return FALSE;
		if (blocked) reason = "suppressed";
		else if (forced) reason = forced;
		else if (g_hash_table_contains(seen, email)) reason = "duplicate_address";
		else {
			g_autoptr(GDateTime) now = venture_time_now();
			const gchar *why = NULL;
			/* The preference centre: a topic turned off, or more mail than
			 * the person asked for (venture-marketing-preferences.inc). */
			if (!venture_marketing_preference_allows(self->database, org, email, number(send, "topic-id"), now, &why, error)) {
				if (error && *error) return FALSE;
				reason = why;
			} else {
				consent = permission(self, source, email, error);
				if (error && *error) return FALSE;
				if (!consent) reason = "no_marketing_permission";
			}
		}
	}
	g_object_set(recipient, "organization-id", org, "send-id", venture_entity_get_id(send),
		"recipient-key", key, "email", strlen(email) <= 254 ? email : "", "eligibility", reason,
		"state", !strcmp(reason, "eligible") ? "eligible" : "excluded", NULL);
	if (source) g_object_set(recipient, subject_field(G_OBJECT_TYPE(source)), venture_entity_get_id(source), NULL);
	if (strcmp(reason, "eligible")) { (*excluded)++; return save(self, VENTURE_ENTITY(recipient), actor, error); }
	{
		g_autofree gchar *subject_text = string(send, "subject"), *text = string(send, "text-body"), *html = string(send, "html-body");
		g_autofree gchar *token = venture_sequence_tracking_token(error), *hash = NULL, *url = NULL, *tracking_token = NULL;
		Composed a = { NULL, NULL, NULL, NULL, NULL, NULL };
		gboolean ok;
		if (!token) return FALSE;
		hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
		url = g_strdup_printf("%s/marketing/u/%s", base, token);
		if (tracking) {
			g_autofree gchar *tracking_hash = NULL;
			tracking_token = venture_sequence_tracking_token(error);
			if (!tracking_token) return FALSE;
			tracking_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, tracking_token, -1);
			g_object_set(recipient, "tracking-hash", tracking_hash, NULL);
		}
		ok = compose(self, send, source, subject_text, text, html, base, url, tracking_token, &a, error);
		if (ok) g_object_set(recipient, "consent-id", venture_entity_get_id(consent), "subject", a.subject,
			"text-body", a.text, "html-body", a.html, "private-text-body", a.private_text, "private-html-body", a.private_html,
			"links", a.links, "unsubscribe-url", url, "unsubscribe-hash", hash, NULL);
		composed_clear(&a);
		if (!ok) return FALSE;
		if (ab_test(send)) {
			/* Arm B: whatever the send varies, the rest kept from A. */
			g_autofree gchar *b_subject = string(send, "variant-subject"), *b_text = string(send, "variant-text-body"), *b_html = string(send, "variant-html-body");
			Composed b = { NULL, NULL, NULL, NULL, NULL, NULL };
			ok = compose(self, send, source, b_subject && *b_subject ? b_subject : subject_text,
				b_text && *b_text ? b_text : text, b_html && *b_html ? b_html : html, base, url, tracking_token, &b, error);
			if (ok) g_object_set(recipient, "variant-subject", b.subject, "variant-text-body", b.text, "variant-html-body", b.html,
				"variant-private-text-body", b.private_text, "variant-private-html-body", b.private_html, "variant-links", b.links, NULL);
			composed_clear(&b);
			if (!ok) return FALSE;
		}
	}
	if (!save(self, VENTURE_ENTITY(recipient), actor, error)) return FALSE;
	g_hash_table_add(seen, g_strdup(email)); (*eligible)++; return TRUE;
}
/* Swaps a recipient's frozen A and B messages: the B arm and, when B
 * wins, the rest. Only the service writes either, so the outbox check
 * (validate_mail()) still holds the queued mail to the main fields. */
static void swap_arms(VentureEntity *recipient)
{
	const gchar *pairs[][2] = {
		{ "subject", "variant-subject" }, { "text-body", "variant-text-body" }, { "html-body", "variant-html-body" },
		{ "private-text-body", "variant-private-text-body" }, { "private-html-body", "variant-private-html-body" },
		{ "links", "variant-links" }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(pairs); i++) {
		g_autofree gchar *first = string(recipient, pairs[i][0]), *second = string(recipient, pairs[i][1]);
		g_object_set(recipient, pairs[i][0], second, pairs[i][1], first, NULL);
	}
}
/* After the preview: the deterministic split into A, B and the rest. */
static gboolean assign_arms(VentureMarketingService *self, VentureEntity *send, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_RECIPIENT);
	g_autoptr(GPtrArray) rows = NULL, emails = g_ptr_array_new_with_free_func(g_free);
	g_auto(GStrv) arms = NULL;
	guint i;
	venture_query_set_organization(query, venture_entity_get_organization_id(send));
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "send-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(send), NULL);
	venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_EQ, "eligible", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	if (!rows) return FALSE;
	if (rows->len < 2) return refuse(error, "An A/B test needs at least two eligible recipients");
	for (i = 0; i < rows->len; i++) g_ptr_array_add(emails, string(g_ptr_array_index(rows, i), "email"));
	g_ptr_array_add(emails, NULL);
	arms = venture_marketing_ab_assign(venture_entity_get_uuid(send), (const gchar *const *)emails->pdata, (guint)number(send, "test-percent"));
	for (i = 0; i < rows->len; i++) {
		VentureEntity *recipient = g_ptr_array_index(rows, i);
		if (!g_strcmp0(arms[i], "b")) swap_arms(recipient);
		g_object_set(recipient, "variant", arms[i], NULL);
		if (!save(self, recipient, actor, error)) return FALSE;
	}
	return TRUE;
}
/* A resend's audience: the first send's recipients the relay took. */
static GPtrArray *resend_audience(VentureMarketingService *self, VentureEntity *send, GError **error)
{
	g_autoptr(VentureEntity) first = get_row(self, VENTURE_TYPE_MARKETING_SEND, venture_entity_get_organization_id(send), number(send, "resend-of-id"), error);
	g_autoptr(VentureQuery) query = NULL;
	if (!first) return NULL;
	query = venture_query_new(VENTURE_TYPE_MARKETING_RECIPIENT);
	venture_query_set_organization(query, venture_entity_get_organization_id(send));
	venture_query_set_limit(query, 10001);
	venture_query_add_filter_int(query, "send-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(first), NULL);
	venture_query_add_filter_int(query, "mail-id", VENTURE_FILTER_OP_GT, 0, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(self->database, query, error);
}
/* Why a first-send recipient is not a non-opener (yet), or NULL. */
static const gchar *resend_reason(VentureMarketingService *self, VentureEntity *send, VentureEntity *original, GDateTime *now)
{
	g_autoptr(VentureEntity) mail = venture_database_get(self->database, VENTURE_TYPE_MAIL_MESSAGE, number(original, "mail-id"), NULL);
	g_autoptr(GDateTime) sent = NULL, due = NULL;
	g_autofree gchar *state = mail ? string(mail, "state") : NULL;
	if (!mail || g_strcmp0(state, "sent")) return "not_sent";
	if (has_stamp(original, "opened-at")) return "opened";
	if (has_stamp(original, "unsubscribed-at") || has_stamp(original, "complained-at") || has_stamp(original, "bounced-at")) return "suppressed";
	/* Counted from when the outbox took the message, which is the
	 * server's clock; the relay's acceptance follows within the run. */
	g_object_get(mail, "created-at", &sent, NULL);
	due = sent ? g_date_time_add_days(sent, (gint)number(send, "resend-after-days")) : NULL;
	if (!due || g_date_time_compare(due, now) > 0) return "too_soon";
	return NULL;
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
	gboolean segment, tracking, resend;
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
	if (ab_test(send) && !tracking) {
		refuse(error, "An A/B test picks its winner by people's opens or clicks, so it needs tracking on the send and the organization"); goto fail;
	}
	resend = number(send, "resend-of-id") > 0;
	if (resend) audience = resend_audience(self, send, error);
	else if (segment) {
		audience = segment_members(self, list, now, error);
	} else audience = children(self, VENTURE_TYPE_MARKETING_MEMBER, org, "list-id", venture_entity_get_id(list), 10001, error);
	if (!audience) goto fail;
	if (audience->len > 10000) { refuse(error, "Preview is bounded to 10000 source records; narrow the segment or list"); goto fail; }
	for (i = 0; i < audience->len; i++) {
		VentureEntity *candidate = g_ptr_array_index(audience, i);
		g_autoptr(VentureEntity) source = NULL;
		g_autoptr(GError) missing = NULL;
		if (segment && !resend) source = g_object_ref(candidate);
		else source = subject(self, candidate, &missing);
		if (missing && !g_error_matches(missing, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) {
			g_propagate_error(error, g_steal_pointer(&missing)); goto fail;
		}
		if (!make_recipient(self, send, source, venture_entity_get_uuid(candidate), base, tracking,
			seen, resend ? resend_reason(self, send, candidate, now) : NULL, &eligible, &excluded, actor, error)) goto fail;
	}
	if (ab_test(send) && !assign_arms(self, send, actor, error)) goto fail;
	g_object_set(send, "state", 1, "previewed-at", now, "eligible-count", eligible, "excluded-count", excluded, "public-base", base, NULL);
	if (!save(self, send, actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return VENTURE_MARKETING_SEND(g_steal_pointer(&send));
fail:
	venture_database_rollback(self->database); return NULL;
}
