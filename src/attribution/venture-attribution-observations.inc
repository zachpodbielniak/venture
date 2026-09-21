/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean observation_values(VentureEntity *site, VentureEntity *touch, JsonObject *fields, GError **error)
{
	static const gchar *const names[] = { "page", "referrer", "utm_source", "utm_medium", "utm_campaign", NULL };
	const gchar *page, *referrer, *source, *medium, *label;
	g_autoptr(GUri) page_uri = NULL, referrer_uri = NULL;
	g_autofree gchar *origin = string(site, "origin"), *page_origin = NULL, *from = NULL;
	g_autoptr(JsonNode) mapping = NULL;
	gint64 campaign = 0;
	if (!fields_valid(fields, names, 2048, error)) return FALSE;
	page = venture_json_object_get_string(fields, "page", "");
	referrer = venture_json_object_get_string(fields, "referrer", "");
	source = venture_json_object_get_string(fields, "utm_source", "");
	medium = venture_json_object_get_string(fields, "utm_medium", "");
	label = venture_json_object_get_string(fields, "utm_campaign", "");
	if ((*source && !identifier(source)) || (*medium && !identifier(medium)) || (*label && !identifier(label)))
		return refuse(error, "UTM values require bounded identifiers, never free text or contact details");
	page_uri = g_uri_parse(page, G_URI_FLAGS_NONE, NULL);
	if (!page_uri || !g_uri_get_host(page_uri) || g_uri_get_userinfo(page_uri))
		return refuse(error, "Observation requires a page URL on its configured site");
	page_origin = g_uri_join(G_URI_FLAGS_NONE, g_uri_get_scheme(page_uri), NULL, g_uri_get_host(page_uri), g_uri_get_port(page_uri), "", NULL, NULL);
	{
		g_autofree gchar *canonical = origin_normalize(page_origin, error);
		if (!canonical || g_strcmp0(origin, canonical)) return refuse(error && *error ? NULL : error, "Page URL belongs to another site");
	}
	if (*referrer) {
		referrer_uri = g_uri_parse(referrer, G_URI_FLAGS_NONE, NULL);
		if (!referrer_uri || !g_uri_get_host(referrer_uri) || g_uri_get_userinfo(referrer_uri) ||
			(g_strcmp0(g_uri_get_scheme(referrer_uri), "https") && g_strcmp0(g_uri_get_scheme(referrer_uri), "http")))
			return refuse(error, "Referrer must be an HTTP(S) URL without credentials");
		from = g_uri_join(G_URI_FLAGS_NONE, g_uri_get_scheme(referrer_uri), NULL, g_uri_get_host(referrer_uri), g_uri_get_port(referrer_uri), "", NULL, NULL);
	}
	if (!*source) source = from && g_strcmp0(from, origin) ? g_uri_get_host(referrer_uri) : "direct";
	mapping = campaign_mapping(site, error);
	if (error && *error) return FALSE;
	if (*label && mapping && JSON_NODE_HOLDS_OBJECT(mapping)) {
		JsonNode *value = json_object_get_member(json_node_get_object(mapping), label);
		if (value) campaign = json_node_get_int(value);
	}
	g_object_set(touch, "path", g_uri_get_path(page_uri), "referrer-origin", from,
		"source", source, "medium", medium, "campaign-label", label, "campaign-id", campaign, NULL);
	return TRUE;
}
VentureAttributionTouch *venture_attribution_service_observe(VentureAttributionService *self,
	const gchar *site_uuid, const gchar *origin, const gchar *token, const gchar *event_id,
	JsonObject *fields, GDateTime *now, GError **error)
{
	g_autoptr(VentureAccessScope) capability = NULL;
	g_autoptr(VentureEntity) site = NULL, visitor = NULL, touch = NULL, existing = NULL;
	g_autofree gchar *key = NULL, *hash = NULL, *session = NULL;
	g_autoptr(GDateTime) last = NULL, window = NULL, minute = NULL;
	gint64 rate;
	g_return_val_if_fail(VENTURE_IS_ATTRIBUTION_SERVICE(self), NULL);
	if (!self->database || !origin || !identifier(event_id) || !clock_valid(now, error)) {
		if (!error || !*error) refuse(error, "Observation requires an exact site origin and bounded event identity");
		return NULL;
	}
	capability = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return NULL;
	site = site_get(self, site_uuid, origin, TRUE, error); if (!site) goto fail;
	visitor = visitor_get(self, site, token, now, TRUE, error); if (!visitor) goto fail;
	touch = g_object_new(VENTURE_TYPE_ATTRIBUTION_TOUCH, "organization-id", venture_entity_get_organization_id(site),
		"site-id", venture_entity_get_id(site), "visitor-id", venture_entity_get_id(visitor), "occurred-at", now, NULL);
	if (!observation_values(site, touch, fields, error)) goto fail;
	key = g_strdup_printf("%s:%s", venture_entity_get_uuid(visitor), event_id); hash = digest(fields);
	existing = find_string(self, VENTURE_TYPE_ATTRIBUTION_TOUCH, venture_entity_get_organization_id(site), "event-key", key, error);
	if (error && *error) goto fail;
	if (existing) {
		g_autofree gchar *old = string(existing, "payload-hash");
		if (g_strcmp0(hash, old)) { refuse(error, "Observation identity already names different content"); goto fail; }
		if (!venture_database_commit(self->database, error)) return NULL;
		return VENTURE_ATTRIBUTION_TOUCH(g_steal_pointer(&existing));
	}
	g_object_get(visitor, "last-seen-at", &last, "rate-window-at", &window, "session-key", &session, NULL);
	rate = number(visitor, "rate-count");
	if (!window || g_date_time_difference(now, window) >= 60 * G_TIME_SPAN_SECOND) {
		g_clear_pointer(&window, g_date_time_unref); window = g_date_time_ref(now); rate = 0;
	}
	if (rate >= 60 || number(visitor, "event-count") >= 1000) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Visitor analytics limit reached"); goto fail;
	}
	minute = g_date_time_add_seconds(now, -60);
	if (!quota(self, site, VENTURE_TYPE_ATTRIBUTION_TOUCH, "occurred-at", minute, 600, error)) goto fail;
	if (!last || g_date_time_difference(now, last) >= 30 * 60 * G_TIME_SPAN_SECOND) {
		g_free(session); session = g_uuid_string_random();
	}
	g_object_set(touch, "session-key", session, "event-key", key, "payload-hash", hash, NULL);
	g_object_set(visitor, "last-seen-at", now, "session-key", session, "rate-window-at", window,
		"rate-count", rate + 1, "event-count", number(visitor, "event-count") + 1, NULL);
	if (!save(self, visitor, NULL, error) || !save(self, touch, NULL, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return VENTURE_ATTRIBUTION_TOUCH(g_steal_pointer(&touch));
fail:
	venture_database_rollback(self->database); return NULL;
}
