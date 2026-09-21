/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean save(VentureAttributionService *self, VentureEntity *row, const VentureActor *actor, GError **error)
{
	gboolean result;
	self->permit = row;
	result = venture_database_save(self->database, row, actor, error);
	self->permit = NULL;
	return result;
}
static VentureEntity *find_string(VentureAttributionService *self, GType type, gint64 org,
	const gchar *field, const gchar *value, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	if (org > 0) venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, NULL);
	return venture_database_find_one(self->database, query, error);
}
static gboolean clock_valid(GDateTime *now, GError **error)
{
	return (now && g_date_time_get_year(now) >= 1970) || refuse(error, "A valid trusted receipt clock is required");
}
static VentureEntity *site_get(VentureAttributionService *self, const gchar *uuid, const gchar *origin,
	gboolean require_active, GError **error)
{
	g_autoptr(VentureEntity) site = NULL;
	g_autofree gchar *expected = NULL, *actual = NULL;
	gboolean active;
	if (!self->database || !uuid || !g_uuid_string_is_valid(uuid)) goto missing;
	if (require_active && !venture_entity_registry_lookup(venture_entity_registry_get_default(), "attribution_site")) goto missing;
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ATTRIBUTION_SITE);
		venture_query_set_include_deleted(query, !require_active);
		venture_query_add_filter_string(query, "uuid", VENTURE_FILTER_OP_EQ, uuid, NULL);
		site = venture_database_find_one(self->database, query, error);
	}
	if (!site) goto missing;
	g_object_get(site, "active", &active, NULL);
	if (require_active && !active) goto missing;
	if (origin) {
		expected = string(site, "origin"); actual = origin_normalize(origin, error);
		if (!actual || g_strcmp0(expected, actual)) goto missing;
	}
	return g_steal_pointer(&site);
missing:
	if (!error || !*error) g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Attribution site unavailable");
	return NULL;
}
static gchar *digest(JsonObject *object)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GString) material = g_string_new(NULL);
	json_node_set_object(node, object); venture_json_append_canonical(material, node);
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, material->str, material->len);
}
static gboolean fields_valid(JsonObject *fields, const gchar *const *names, gsize maximum, GError **error)
{
	JsonObjectIter iter;
	const gchar *name;
	JsonNode *value;
	if (!fields || json_object_get_size(fields) > g_strv_length((gchar **)names))
		return refuse(error, "Unknown or excessive attribution fields");
	json_object_iter_init(&iter, fields);
	while (json_object_iter_next(&iter, &name, &value)) {
		const gchar *text;
		if (!g_strv_contains(names, name) || !JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_STRING)
			return refuse(error, "Attribution fields require the documented string schema");
		text = json_node_get_string(value);
		if (!text || strlen(text) > maximum || !g_utf8_validate(text, -1, NULL) || strpbrk(text, "\r\n"))
			return refuse(error, "Attribution field exceeds its text bounds");
	}
	return TRUE;
}
static gboolean quota(VentureAttributionService *self, VentureEntity *site, GType type,
	const gchar *field, GDateTime *from, guint maximum, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *time = venture_time_to_string(from);
	venture_query_set_organization(query, venture_entity_get_organization_id(site));
	venture_query_set_limit(query, maximum);
	venture_query_add_filter_int(query, "site-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(site), NULL);
	venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_GTE, time, NULL);
	rows = venture_database_find(self->database, query, error);
	if (!rows) return FALSE;
	if (rows->len >= maximum) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Site analytics rate limit reached; retry later"); return FALSE;
	}
	return TRUE;
}
static VentureEntity *visitor_get(VentureAttributionService *self, VentureEntity *site,
	const gchar *token, GDateTime *now, gboolean active, GError **error)
{
	g_autofree gchar *hash = NULL;
	g_autoptr(VentureEntity) visitor = NULL;
	g_autoptr(GDateTime) expires = NULL, withdrawn = NULL, last = NULL;
	gsize i;
	if (!token || strlen(token) != 64) goto missing;
	for (i = 0; i < 64; i++) if (!g_ascii_isxdigit(token[i])) goto missing;
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	visitor = find_string(self, VENTURE_TYPE_ATTRIBUTION_VISITOR, venture_entity_get_organization_id(site), "token-hash", hash, error);
	if (!visitor || number(visitor, "site-id") != venture_entity_get_id(site)) goto missing;
	g_object_get(visitor, "expires-at", &expires, "withdrawn-at", &withdrawn, "last-seen-at", &last, NULL);
	if (active && (withdrawn || !expires || g_date_time_compare(now, expires) >= 0 ||
		number(visitor, "site-version") != venture_entity_get_version(site) || (last && g_date_time_compare(now, last) < 0))) goto missing;
	return g_steal_pointer(&visitor);
missing:
	if (!error || !*error) g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Analytics permission unavailable");
	return NULL;
}
JsonNode *venture_attribution_service_grant(VentureAttributionService *self, const gchar *site_uuid,
	const gchar *origin, const gchar *policy, GDateTime *now, GError **error)
{
	g_autoptr(VentureAccessScope) capability = NULL;
	g_autoptr(VentureEntity) site = NULL, visitor = NULL;
	g_autofree gchar *expected = NULL, *token = NULL, *hash = NULL, *until = NULL;
	g_autoptr(GDateTime) expires = NULL, minute = NULL, day = NULL;
	g_autoptr(JsonNode) result = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object;
	g_return_val_if_fail(VENTURE_IS_ATTRIBUTION_SERVICE(self), NULL);
	if (!self->database || !origin || !clock_valid(now, error)) {
		if (!error || !*error) refuse(error, "Analytics grant requires a live repository and exact site origin");
		return NULL;
	}
	capability = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return NULL;
	site = site_get(self, site_uuid, origin, TRUE, error); if (!site) goto fail;
	expected = string(site, "consent-policy");
	if (!bounded(policy, 128) || g_strcmp0(policy, expected)) { refuse(error, "Explicit consent to the site's current anonymous analytics policy is required"); goto fail; }
	minute = g_date_time_add_seconds(now, -60); day = g_date_time_add_days(now, -1);
	if (!quota(self, site, VENTURE_TYPE_ATTRIBUTION_VISITOR, "consented-at", minute, 60, error) ||
		!quota(self, site, VENTURE_TYPE_ATTRIBUTION_VISITOR, "consented-at", day, 10000, error)) goto fail;
	token = venture_sequence_tracking_token(error); if (!token) goto fail;
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	expires = g_date_time_add_days(now, number(site, "retention-days"));
	if (!expires) { refuse(error, "Analytics expiration exceeds calendar bounds"); goto fail; }
	visitor = g_object_new(VENTURE_TYPE_ATTRIBUTION_VISITOR, "organization-id", venture_entity_get_organization_id(site),
		"site-id", venture_entity_get_id(site), "site-version", (gint64)venture_entity_get_version(site),
		"token-hash", hash, "consent-policy", policy, "consent-source", "Explicit site analytics consent action",
		"consented-at", now, "expires-at", expires, NULL);
	if (!save(self, visitor, NULL, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	object = json_object_new(); json_node_take_object(result, object); until = venture_time_to_string(expires);
	json_object_set_string_member(object, "token", token); json_object_set_string_member(object, "expires_at", until);
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(self->database); return NULL;
}
