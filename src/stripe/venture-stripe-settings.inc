/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included beside the provider client so clear settings never cross a UI API. */
static const gchar *
settings_string(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);
	return node && json_node_get_value_type(node) == G_TYPE_STRING
		? json_node_get_string(node) : NULL;
}

static gboolean
settings_manage(VentureDatabase *database, gint64 organization_id, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(policy);
	g_autoptr(VentureEntity) organization = NULL;
	if (organization_id <= 0)
		return refuse(error, "Stripe settings require an explicit organization");
	if (principal && !venture_access_policy_has_organization_role(policy, principal,
		organization_id, roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"Organization integration administration is required");
		return FALSE;
	}
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "stripe_checkout") == G_TYPE_INVALID)
		return refuse(error, "Stripe module is disabled");
	organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, organization_id, error);
	if (!organization) return FALSE;
	if (venture_entity_is_deleted(organization)) return refuse(error, "Organization is unavailable");
	return TRUE;
}

static gboolean
settings_load(VentureStripeService *self, JsonNode *settings, GError **error)
{
	static const gchar *const names[] = { "secret_key", "publishable_key", "webhook_secret", "api_version", "success_url", "cancel_url", "environment" };
	const gchar *values[G_N_ELEMENTS(names)];
	g_autoptr(StripeClient) client = NULL;
	g_autofree gchar *yaml = NULL;
	JsonObject *object;
	guint i;
	if (!settings || !JSON_NODE_HOLDS_OBJECT(settings))
		return refuse(error, "Stripe settings must be an object");
	object = json_node_get_object(settings);
	if (json_object_has_member(object, "ach_enabled"))
	{
		JsonNode *node = json_object_get_member(object, "ach_enabled");
		if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
			return refuse(error, "ACH selection must be a boolean");
		self->ach_enabled = json_node_get_boolean(node);
	}
	else self->ach_enabled = FALSE;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		values[i] = settings_string(object, names[i]);
		if (!values[i] || !*values[i] || strlen(values[i]) > 2048 ||
			strchr(values[i], '\n') || strchr(values[i], '\r') || strchr(values[i], '\''))
			return refuse(error, "Stripe settings require bounded nonempty token and URL values");
	}
	if (strcmp(values[6], "test") && strcmp(values[6], "live"))
		return refuse(error, "Stripe environment must be test or live");
	if (!g_str_has_prefix(values[0], !strcmp(values[6], "test") ? "sk_test_" : "sk_live_") ||
		!g_str_has_prefix(values[1], !strcmp(values[6], "test") ? "pk_test_" : "pk_live_") ||
		!g_str_has_prefix(values[2], "whsec_"))
		return refuse(error, "Stripe keys must match the selected environment");
	for (i = 4; i <= 5; i++)
	{
		g_autoptr(GUri) uri = g_uri_parse(values[i], G_URI_FLAGS_NONE, NULL);
		if (!uri || g_strcmp0(g_uri_get_scheme(uri), "https") ||
			!g_uri_get_host(uri) || !*g_uri_get_host(uri) || g_uri_get_userinfo(uri))
			return refuse(error, "Stripe return URLs require HTTPS without embedded credentials");
	}
	yaml = g_strdup_printf("secret_key: '%s'\npublishable_key: '%s'\nwebhook_signing_secrets: ['%s']\napi_version: '%s'\n", values[0], values[1], values[2], values[3]);
	client = stripe_client_new(yaml, self->transport, NULL, NULL);
	if (!client) return refuse(error, "Stripe rejected the supplied configuration");
	g_set_object(&self->client, client);
	g_free(self->yaml);
	self->yaml = g_steal_pointer(&yaml);
	g_free(self->api_version);
	self->api_version = g_strdup(values[3]);
	g_free(self->success_url);
	self->success_url = g_strdup(values[4]);
	g_free(self->cancel_url);
	self->cancel_url = g_strdup(values[5]);
	g_free(self->environment);
	self->environment = g_strdup(values[6]);
	g_signal_connect(self->client, "event", G_CALLBACK(audit_event), self);
	return TRUE;
}

static gchar *
settings_verify_account(VentureStripeService *self, GError **error)
{
	g_autoptr(StripeRequest) request = stripe_request_new(STRIPE_ACCOUNT_RETRIEVE);
	g_autoptr(StripeResource) resource = stripe_client_execute(self->client, request, NULL, NULL);
	const gchar *account;
	if (!resource)
	{
		refuse(error, "Stripe account verification failed; check credentials and connectivity");
		return NULL;
	}
	account = stripe_resource_get_string(resource, "id");
	if (!account || !g_str_has_prefix(account, "acct_") || strlen(account) > 255)
	{
		refuse(error, "Stripe account verification returned no account identity");
		return NULL;
	}
	return g_strdup(account);
}

static gboolean
stripe_refresh(VentureStripeService *self, gboolean allow_disabled, GError **error)
{
	g_autoptr(VentureEntity) connection = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autofree gchar *provider = NULL;
	g_autofree gchar *account = NULL;
	g_autofree gchar *environment = NULL;
	if (!self->connection_id) return TRUE; /* Explicit offline fixture only. */
	connection = venture_database_get(self->database, VENTURE_TYPE_INTEGRATION_CONNECTION,
		self->connection_id, error);
	if (!connection) return FALSE;
	g_object_get(connection, "provider", &provider, "account-id", &account, "environment", &environment, NULL);
	if (g_strcmp0(provider, "stripe") || venture_entity_get_organization_id(connection) != self->organization_id ||
		(self->account_id && g_strcmp0(account, self->account_id)))
		return refuse(error, "Stripe connection identity does not match this endpoint");
	settings = venture_integration_service_resolve_version(venture_integration_service_get(self->database),
		self->organization_id, self->connection_id, venture_entity_get_version(connection), allow_disabled, error);
	if (!settings) return FALSE;
	if (g_strcmp0(settings_string(json_node_get_object(settings), "environment"), environment))
		return refuse(error, "Stripe settings do not match the connection environment");
	if (!self->client || self->connection_version != venture_entity_get_version(connection))
	{
		if (!settings_load(self, settings, error)) return FALSE;
		self->connection_version = venture_entity_get_version(connection);
		g_free(self->account_id);
		self->account_id = g_strdup(account);
		g_free(self->connection_uuid);
		self->connection_uuid = g_strdup(venture_entity_get_uuid(connection));
	}
	return TRUE;
}

VentureStripeService *
venture_stripe_service_for_connection(VentureDatabase *database, gint64 organization_id,
	gint64 connection_id, gboolean allow_disabled, StripeTransport *transport, GError **error)
{
	g_autoptr(VentureStripeService) self = NULL;
	if (organization_id <= 0 || connection_id <= 0)
	{
		refuse(error, "Stripe requires an explicit organization and connection");
		return NULL;
	}
	self = g_object_new(VENTURE_TYPE_STRIPE_SERVICE, "database", database, "organization-id", organization_id, NULL);
	self->connection_id = connection_id;
	if (transport) self->transport = g_object_ref(transport);
	if (!stripe_refresh(self, allow_disabled, error)) return NULL;
	return g_steal_pointer(&self);
}

VentureStripeService *
venture_stripe_service_for_organization(VentureDatabase *database, gint64 organization_id,
	StripeTransport *transport, GError **error)
{
	g_autoptr(VentureIntegrationConnection) connection = venture_integration_service_find(
		venture_integration_service_get(database), organization_id, "stripe", error);
	if (!connection) return NULL;
	return venture_stripe_service_for_connection(database, organization_id,
		venture_entity_get_id(VENTURE_ENTITY(connection)), FALSE, transport, error);
}

VentureIntegrationConnection *
venture_stripe_settings_configure(VentureDatabase *database, gint64 organization_id,
	JsonNode *settings, gint64 expected_version, gint64 expected_connection_id, StripeTransport *transport,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureStripeService) candidate = NULL;
	g_autofree gchar *account = NULL;
	g_autoptr(VentureIntegrationConnection) current = NULL;
	g_autoptr(VentureIntegrationConnection) configured = NULL;
	g_autoptr(GError) lookup_error = NULL;
	if (!settings_manage(database, organization_id, error)) return NULL;
	candidate = g_object_new(VENTURE_TYPE_STRIPE_SERVICE, "database", database,
		"organization-id", organization_id, NULL);
	if (transport) candidate->transport = g_object_ref(transport);
	if (!settings_load(candidate, settings, error)) return NULL;
	account = settings_verify_account(candidate, error);
	if (!account) return NULL;
	/* A replacement record can have the same numeric version. Bind the
	 * caller's intent to both identity and version, after the network read. */
	if (!venture_database_begin(database, error)) return NULL;
	current = venture_integration_service_find(venture_integration_service_get(database), organization_id, "stripe", &lookup_error);
	if (!current && lookup_error && !g_error_matches(lookup_error, VENTURE_ERROR, VENTURE_ERROR_CONFIG))
	{
		g_propagate_error(error, g_steal_pointer(&lookup_error));
		goto fail;
	}
	if ((current ? venture_entity_get_id(VENTURE_ENTITY(current)) : 0) != expected_connection_id)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Stripe connection changed; reload settings before retrying");
		goto fail;
	}
	configured = venture_integration_service_configure(venture_integration_service_get(database),
		organization_id, "stripe", account, candidate->environment, settings, expected_version, actor, error);
	if (!configured) goto fail;
	if (expected_connection_id > 0 && venture_entity_get_id(VENTURE_ENTITY(configured)) != expected_connection_id)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Stripe connection changed; reload settings before retrying");
		goto fail;
	}
	if (!venture_database_commit(database, error)) goto fail;
	return g_steal_pointer(&configured);
fail:
	venture_database_rollback(database);
	return NULL;
}

gboolean
venture_stripe_settings_test(VentureDatabase *database, gint64 organization_id,
	StripeTransport *transport, GError **error)
{
	g_autoptr(VentureStripeService) self = NULL;
	g_autofree gchar *account = NULL;
	if (!settings_manage(database, organization_id, error)) return FALSE;
	self = venture_stripe_service_for_organization(database, organization_id, transport, error);
	if (!self) return FALSE;
	account = settings_verify_account(self, error);
	if (!account) return FALSE;
	return !g_strcmp0(account, self->account_id) || refuse(error, "Stripe credentials now identify a different account");
}
