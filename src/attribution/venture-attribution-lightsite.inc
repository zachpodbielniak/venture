/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean signing_settings(JsonNode *node, GError **error)
{
	static const gchar *const names[] = { "tenant_id", "environment", "signing_secret", NULL };
	JsonObject *object;
	const gchar *secret, *environment;
	guint i;
	if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return refuse(error, "Lightsite settings require an object");
	object = json_node_get_object(node);
	if (!fields_valid(object, names, 128, error)) return FALSE;
	secret = venture_json_object_get_string(object, "signing_secret", "");
	environment = venture_json_object_get_string(object, "environment", "");
	if (!identifier(venture_json_object_get_string(object, "tenant_id", "")) ||
		(g_strcmp0(environment, "test") && g_strcmp0(environment, "live")) || strlen(secret) != 64)
		return refuse(error, "Lightsite requires tenant identity, test/live environment and a 64-character hexadecimal shared secret");
	for (i = 0; i < 64; i++) if (!g_ascii_isxdigit(secret[i])) return refuse(error, "Invalid Lightsite shared secret format");
	return TRUE;
}
JsonNode *venture_attribution_settings_schema(void)
{
	return json_from_string("{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"tenant_id\",\"environment\",\"signing_secret\"],\"properties\":{"
		"\"tenant_id\":{\"type\":\"string\",\"title\":\"Verified Lightsite tenant identity\"},"
		"\"environment\":{\"type\":\"string\",\"enum\":[\"test\",\"live\"],\"title\":\"Environment\"},"
		"\"signing_secret\":{\"type\":\"string\",\"format\":\"password\",\"writeOnly\":true,\"x-sensitive\":true,\"title\":\"Shared signing secret (64 hexadecimal characters)\"}}}", NULL);
}
VentureIntegrationConnection *venture_attribution_settings_configure(VentureDatabase *database,
	gint64 org, JsonNode *settings, gint64 expected_version, gint64 expected_connection,
	const VentureActor *actor, GError **error)
{
	VentureAttributionService *self = venture_attribution_service_get(database);
	VentureIntegrationService *integrations = venture_integration_service_get(database);
	g_autoptr(VentureIntegrationConnection) current = NULL, result = NULL;
	g_autoptr(GError) local = NULL;
	JsonObject *object;
	if (!administrative(self, org, error) || !signing_settings(settings, error)) return NULL;
	if (!venture_database_begin(database, error)) return NULL;
	current = venture_integration_service_find(integrations, org, "lightsite_capture", &local);
	if (!current && local && !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_CONFIG)) {
		g_propagate_error(error, g_steal_pointer(&local)); goto fail;
	}
	if ((current ? venture_entity_get_id(VENTURE_ENTITY(current)) : 0) != expected_connection) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Lightsite connection changed; reload settings"); goto fail;
	}
	object = json_node_get_object(settings);
	result = venture_integration_service_configure(integrations, org, "lightsite_capture",
		json_object_get_string_member(object, "tenant_id"), json_object_get_string_member(object, "environment"),
		settings, expected_version, actor, error);
	if (!result) goto fail;
	if (!venture_database_commit(database, error)) goto fail;
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(database); return NULL;
}
static gboolean signature_valid(const gchar *timestamp, const gchar *signature, GBytes *body,
	const gchar *secret, GDateTime *now, GError **error)
{
	g_autoptr(GHmac) hmac = NULL;
	gint64 seconds, clock = g_date_time_to_unix(now);
	gsize length, i;
	const guint8 *bytes = body ? g_bytes_get_data(body, &length) : NULL;
	/* The framing is unambiguous and body bytes are never NUL-terminated
	 * before authentication. Timestamp tolerance does not replace replay IDs. */
	if (!timestamp || strlen(timestamp) > 12 || !*timestamp || !signature || strlen(signature) != 64 ||
		!bytes || !length || length > 32768 || !g_ascii_string_to_signed(timestamp, 10, 0, G_MAXINT64, &seconds, NULL)) goto denied;
	for (i = 0; timestamp[i]; i++) if (!g_ascii_isdigit(timestamp[i])) goto denied;
	for (i = 0; i < 64; i++) if (!g_ascii_isxdigit(signature[i])) goto denied;
	if (seconds < clock - 300 || seconds > clock + 300) goto denied;
	hmac = g_hmac_new(G_CHECKSUM_SHA256, (const guchar *)secret, strlen(secret));
	g_hmac_update(hmac, (const guchar *)timestamp, strlen(timestamp)); g_hmac_update(hmac, (const guchar *)"\n", 1);
	g_hmac_update(hmac, bytes, length);
	if (venture_constant_time_equal(g_hmac_get_string(hmac), signature)) return TRUE;
denied:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Lightsite delivery authentication refused"); return FALSE;
}
VentureAttributionSubmission *venture_attribution_service_receive(VentureAttributionService *self,
	const gchar *site_uuid, gint64 connection_id, const gchar *timestamp, const gchar *signature,
	GBytes *body, GDateTime *now, GError **error)
{
	g_autoptr(VentureAccessScope) capability = NULL;
	g_autoptr(VentureEntity) site = NULL, connection = NULL, result = NULL, current = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) local = NULL;
	g_autofree gchar *provider = NULL, *account = NULL, *tenant = NULL, *environment = NULL;
	JsonObject *object, *payload;
	const guint8 *bytes;
	gsize length;
	gint64 org, version;
	g_return_val_if_fail(VENTURE_IS_ATTRIBUTION_SERVICE(self), NULL);
	if (!self->database || !clock_valid(now, error)) return NULL;
	capability = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return NULL;
	site = site_get(self, site_uuid, NULL, TRUE, error); if (!site) goto fail;
	org = venture_entity_get_organization_id(site);
	if (connection_id <= 0 || connection_id != number(site, "connection-id")) { refuse(error, "Site connection does not match this delivery"); goto fail; }
	connection = get_row(self, VENTURE_TYPE_INTEGRATION_CONNECTION, org, connection_id, error); if (!connection) goto fail;
	provider = string(connection, "provider"); account = string(connection, "account-id"); tenant = string(site, "external-tenant-id");
	environment = string(connection, "environment"); version = venture_entity_get_version(connection);
	if (g_strcmp0(provider, "lightsite_capture") || g_strcmp0(account, tenant)) { refuse(error, "Lightsite account does not match configured site"); goto fail; }
	settings = venture_integration_service_resolve_version(venture_integration_service_get(self->database), org, connection_id, version, FALSE, error);
	if (!settings || !signing_settings(settings, error)) goto fail;
	object = json_node_get_object(settings);
	if (g_strcmp0(json_object_get_string_member(object, "tenant_id"), tenant) ||
		g_strcmp0(json_object_get_string_member(object, "environment"), environment)) { refuse(error, "Lightsite encrypted identity mismatch"); goto fail; }
	if (!signature_valid(timestamp, signature, body, json_object_get_string_member(object, "signing_secret"), now, error)) goto fail;
	bytes = g_bytes_get_data(body, &length);
	if (memchr(bytes, 0, length) || !json_parser_load_from_data(parser, (const gchar *)bytes, length, NULL) ||
		!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) { refuse(error, "Lightsite delivery requires bounded JSON without embedded NUL"); goto fail; }
	payload = json_node_get_object(json_parser_get_root(parser));
	if (!json_object_has_member(payload, "tenant_id") || !json_object_has_member(payload, "site_id") || !json_object_has_member(payload, "origin")) {
		refuse(error, "Signed Lightsite delivery requires verified tenant, site and origin"); goto fail;
	}
	result = capture_payload(self, site, connection, version, payload, now, &local);
	if (local) { g_propagate_error(error, g_steal_pointer(&local)); goto fail; }
	/* Automation may disconnect or rotate synchronously during capture. */
	current = get_row(self, VENTURE_TYPE_INTEGRATION_CONNECTION, org, connection_id, error); if (!current) goto fail;
	if (venture_entity_get_version(current) != version) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Lightsite connection changed during delivery; retry"); goto fail;
	}
	if (!venture_database_commit(self->database, error)) goto fail;
	return VENTURE_ATTRIBUTION_SUBMISSION(g_steal_pointer(&result));
fail:
	venture_database_rollback(self->database); return NULL;
}
