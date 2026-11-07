/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Connect with Stripe (Ben, 2026-11-06: "I think standard is the way").
 *
 * The installation holds the platform's keys (stripe.connect.*). For a
 * business it makes one Standard-style connected account (Accounts v2: full
 * dashboard, Stripe collects fees and losses from the owner) and a hosted
 * onboarding link. The business's Stripe connection then has mode "connect":
 * its client is the platform key plus Stripe-Account, so every existing call
 * -- customers, the Checkout Session behind /pay/<token> -- is a direct charge
 * on the owner's own account. No application fee is sent anywhere.
 *
 * One platform webhook, /webhooks/stripe/connect, carries every connected
 * account's events. It is verified with the platform's Connect secret on the
 * raw bytes, then routed by the event's account to that business only.
 */

#define CONNECT_CONFIG_KEY "venture-stripe-connect-config"

typedef struct
{
	gchar *secret;
	gchar *publishable;
	gchar *webhook;
	gchar *environment;
	gchar *version;
} ConnectPlatform;

static void
connect_platform_clear(ConnectPlatform *platform)
{
	g_free(platform->secret);
	g_free(platform->publishable);
	g_free(platform->webhook);
	g_free(platform->environment);
	g_free(platform->version);
	memset(platform, 0, sizeof(*platform));
}

void
venture_stripe_connect_set_config(VentureDatabase *database, VentureConfig *config)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	g_object_set_data_full(G_OBJECT(database), CONNECT_CONFIG_KEY,
		config ? g_object_ref(config) : NULL, config ? g_object_unref : NULL);
}

StripeTransport *
venture_stripe_service_get_transport(VentureStripeService *self)
{
	g_return_val_if_fail(VENTURE_IS_STRIPE_SERVICE(self), NULL);
	return self->transport;
}

/* A value that goes into the client's YAML: a bounded token, never YAML. */
static gboolean
connect_token(const gchar *value)
{
	return value && *value && strlen(value) <= 255 && !strpbrk(value, "\n\r'");
}

/* The platform keys, read when used so a changed configuration applies at
 * once. All empty is "Connect is off": the routes say so and nothing else
 * changes. The mode is test or live, and every key must be of that mode: a
 * live key used as test would take real money from a business that thinks
 * it is rehearsing. A restricted key (rk_) stands in for the secret key. */
static gboolean
connect_platform_load(VentureDatabase *database, ConnectPlatform *platform, GError **error)
{
	static const gchar *const names[] = { "secret_key", "publishable_key", "webhook_secret", "environment", "api_version" };
	VentureConfig *config = g_object_get_data(G_OBJECT(database), CONNECT_CONFIG_KEY);
	const gchar *values[G_N_ELEMENTS(names)];
	guint i;
	memset(platform, 0, sizeof(*platform));
	if (config)
		g_object_get(config, "stripe-connect-secret-key", &platform->secret,
			"stripe-connect-publishable-key", &platform->publishable,
			"stripe-connect-webhook-secret", &platform->webhook,
			"stripe-connect-environment", &platform->environment,
			"stripe-connect-api-version", &platform->version, NULL);
	values[0] = platform->secret; values[1] = platform->publishable; values[2] = platform->webhook;
	values[3] = platform->environment; values[4] = platform->version;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		if (values[i] && *values[i]) continue;
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"Stripe Connect is not configured on this installation (stripe.connect.%s is empty)", names[i]);
		connect_platform_clear(platform);
		return FALSE;
	}
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		if (!connect_token(values[i])) goto invalid;
	if (g_strcmp0(platform->environment, "test") && g_strcmp0(platform->environment, "live"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			"Stripe Connect accepts stripe.connect.environment: test or live");
		connect_platform_clear(platform);
		return FALSE;
	}
	{
		g_autofree gchar *secret = g_strdup_printf("sk_%s_", platform->environment);
		g_autofree gchar *restricted = g_strdup_printf("rk_%s_", platform->environment);
		g_autofree gchar *publishable = g_strdup_printf("pk_%s_", platform->environment);
		if ((!g_str_has_prefix(platform->secret, secret) && !g_str_has_prefix(platform->secret, restricted)) ||
			!g_str_has_prefix(platform->publishable, publishable) || !g_str_has_prefix(platform->webhook, "whsec_"))
			goto invalid;
	}
	return TRUE;
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		"Stripe Connect configuration is invalid; check stripe.connect.* (values are not shown)");
	connect_platform_clear(platform);
	return FALSE;
}

/* A connected account exists in one mode only. A business connected in the
 * other mode is refused, never reused under these keys or replaced by a new
 * account: the owner reconnects on purpose. */
static gboolean
connect_same_environment(const gchar *platform, const gchar *environment, GError **error)
{
	if (!g_strcmp0(platform, environment)) return TRUE;
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		"This business was connected with Stripe in %s mode; disconnect it before connecting in %s mode",
		!g_strcmp0(environment, "live") ? "live" : "test", platform);
	return FALSE;
}

/* The platform's client, or with @account the connected account's client. */
static gchar *
connect_yaml(const ConnectPlatform *platform, const gchar *account)
{
	g_autofree gchar *selector = account ? g_strdup_printf("stripe_account: '%s'\n", account) : g_strdup("");
	return g_strdup_printf("secret_key: '%s'\npublishable_key: '%s'\nwebhook_signing_secrets: ['%s']\napi_version: '%s'\n%s",
		platform->secret, platform->publishable, platform->webhook, platform->version, selector);
}

static StripeClient *
connect_client(const ConnectPlatform *platform, StripeTransport *transport, GError **error)
{
	g_autofree gchar *yaml = connect_yaml(platform, NULL);
	StripeClient *client = stripe_client_new(yaml, transport, NULL, NULL);
	if (!client) refuse(error, "Stripe rejected the Connect platform configuration");
	return client;
}

static gboolean
connect_account_valid(const gchar *account)
{
	return account && strlen(account) <= 255 && g_regex_match_simple("^acct_[A-Za-z0-9]+$", account, 0, 0);
}

static gboolean
connect_https(const gchar *url)
{
	g_autoptr(GUri) uri = NULL;
	if (!url || strlen(url) > 2048 || strpbrk(url, "\n\r'")) return FALSE;
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	return uri && !g_strcmp0(g_uri_get_scheme(uri), "https") && g_uri_get_host(uri) &&
		*g_uri_get_host(uri) && !g_uri_get_userinfo(uri);
}

/* A Stripe failure says what failed, never what Stripe wrote. */
static gpointer
connect_provider_failed(GError **error, const gchar *what)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK, "Stripe did not %s; try again", what);
	return NULL;
}

/* Settings of a connect connection: a closed set of public facts. The
 * platform keys live in the installation configuration, not here. */
static gboolean
settings_load_connect(VentureStripeService *self, JsonObject *object, const gchar *suffix, GError **error)
{
	ConnectPlatform platform;
	g_autoptr(StripeClient) client = NULL;
	g_autofree gchar *yaml = NULL;
	const gchar *account = settings_string(object, "account");
	const gchar *environment = settings_string(object, "environment");
	const gchar *state = settings_string(object, "state");
	const gchar *success = settings_string(object, "success_url");
	const gchar *cancel = settings_string(object, "cancel_url");
	if (!connect_account_valid(account) || (g_strcmp0(environment, "test") && g_strcmp0(environment, "live")) ||
		(g_strcmp0(state, "needs_more") && g_strcmp0(state, "ready")) ||
		!connect_https(success) || !connect_https(cancel))
		return refuse(error, "Stripe Connect settings need an account, environment, state and HTTPS return URLs");
	if (!connect_platform_load(self->database, &platform, error)) return FALSE;
	if (!connect_same_environment(platform.environment, environment, error))
	{
		connect_platform_clear(&platform);
		return FALSE;
	}
	yaml = connect_yaml(&platform, account);
	connect_platform_clear(&platform);
	client = stripe_client_new(yaml, self->transport, NULL, NULL);
	if (!client) return refuse(error, "Stripe rejected the Connect platform configuration");
	g_set_object(&self->client, client);
	g_free(self->yaml);
	self->yaml = g_steal_pointer(&yaml);
	g_free(self->api_version);
	g_object_get(g_object_get_data(G_OBJECT(self->database), CONNECT_CONFIG_KEY),
		"stripe-connect-api-version", &self->api_version, NULL);
	g_free(self->success_url);
	self->success_url = g_strdup(success);
	g_free(self->cancel_url);
	self->cancel_url = g_strdup(cancel);
	g_free(self->environment);
	self->environment = g_strdup(environment);
	g_free(self->descriptor_suffix);
	self->descriptor_suffix = suffix && *suffix ? g_strdup(suffix) : NULL;
	g_signal_connect(self->client, "event", G_CALLBACK(audit_event), self);
	return TRUE;
}

static JsonNode *
connect_answer(const gchar *state, const gchar *mode, const gchar *url)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "state");
	json_builder_add_string_value(builder, state);
	json_builder_set_member_name(builder, "mode");
	if (mode) json_builder_add_string_value(builder, mode);
	else json_builder_add_null_value(builder);
	if (url)
	{
		json_builder_set_member_name(builder, "url");
		json_builder_add_string_value(builder, url);
	}
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/* The enabled Stripe connection of @organization_id and its settings, or
 * none (both NULL, TRUE). */
static gboolean
connect_current(VentureDatabase *database, gint64 organization_id,
	VentureIntegrationConnection **connection, JsonNode **settings, GError **error)
{
	VentureIntegrationService *integrations = venture_integration_service_get(database);
	g_autoptr(GError) lookup = NULL;
	*connection = venture_integration_service_find(integrations, organization_id, "stripe", &lookup);
	*settings = NULL;
	if (!*connection)
	{
		if (lookup && !g_error_matches(lookup, VENTURE_ERROR, VENTURE_ERROR_CONFIG))
		{
			g_propagate_error(error, g_steal_pointer(&lookup));
			return FALSE;
		}
		return TRUE;
	}
	*settings = venture_integration_service_resolve_version(integrations, organization_id,
		venture_entity_get_id(VENTURE_ENTITY(*connection)), venture_entity_get_version(VENTURE_ENTITY(*connection)),
		FALSE, error);
	if (!*settings) g_clear_object(connection);
	return *settings != NULL;
}

static gboolean
connect_mode(JsonNode *settings)
{
	return !g_strcmp0(settings_string(json_node_get_object(settings), "mode"), "connect");
}

/* Stores @settings over @connection: a new state or new return URLs. */
static gboolean
connect_store(VentureDatabase *database, VentureIntegrationConnection *connection, JsonNode *settings,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *account = NULL, *environment = NULL;
	g_autoptr(VentureIntegrationConnection) stored = NULL;
	g_object_get(connection, "account-id", &account, "environment", &environment, NULL);
	stored = venture_integration_service_configure(venture_integration_service_get(database),
		venture_entity_get_organization_id(VENTURE_ENTITY(connection)), "stripe", account, environment,
		settings, venture_entity_get_version(VENTURE_ENTITY(connection)), actor, error);
	return stored != NULL;
}

static const gchar *
connect_state(JsonObject *account)
{
	JsonNode *node = account ? json_object_get_member(account, "charges_enabled") : NULL;
	return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN &&
		json_node_get_boolean(node) ? "ready" : "needs_more";
}

/* Who to prefill onboarding with: the business's name and email, or the
 * calling person's email when the business has none. */
static void
connect_contact(VentureDatabase *database, gint64 organization_id, gchar **name, gchar **email)
{
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(venture_database_get_access_policy(database));
	g_autoptr(VentureEntity) organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, organization_id, NULL);
	*name = NULL; *email = NULL;
	if (organization) g_object_get(organization, "name", name, "email", email, NULL);
	if ((!*email || !**email) && principal && principal->user_id > 0)
	{
		g_autoptr(VentureEntity) user = venture_database_get(database, VENTURE_TYPE_USER, principal->user_id, NULL);
		g_clear_pointer(email, g_free);
		if (user) g_object_get(user, "email", email, NULL);
	}
	if (*name && !**name) g_clear_pointer(name, g_free);
	if (*email && !**email) g_clear_pointer(email, g_free);
}

JsonNode *
venture_stripe_connect_start(VentureDatabase *database, gint64 organization_id,
	const gchar *return_url, const gchar *refresh_url, const gchar *success_url, const gchar *cancel_url,
	StripeTransport *transport, const VentureActor *actor, GError **error)
{
	ConnectPlatform platform;
	g_autoptr(StripeClient) client = NULL;
	g_autoptr(VentureIntegrationConnection) current = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autoptr(StripeRequest) link = NULL;
	g_autoptr(StripeResource) made = NULL;
	g_autofree gchar *account = NULL, *state = NULL, *environment = NULL;
	const gchar *url;
	if (!settings_manage(database, organization_id, error)) return NULL;
	if (!connect_https(return_url) || !connect_https(refresh_url) || !connect_https(success_url) || !connect_https(cancel_url))
	{
		refuse(error, "Connect with Stripe needs HTTPS return_url, refresh_url, success_url and cancel_url");
		return NULL;
	}
	if (!connect_platform_load(database, &platform, error)) return NULL;
	environment = g_strdup(platform.environment);
	client = connect_client(&platform, transport, error);
	connect_platform_clear(&platform);
	if (!client) return NULL;
	if (!connect_current(database, organization_id, &current, &settings, error)) return NULL;
	if (current && !connect_mode(settings))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			"This business already uses pasted Stripe keys; disconnect them before connecting with Stripe");
		return NULL;
	}
	if (current)
	{
		/* Reuse the account; keep the newest return URLs for payments. */
		JsonObject *object = json_node_get_object(settings);
		if (!connect_same_environment(environment, settings_string(object, "environment"), error)) return NULL;
		account = g_strdup(settings_string(object, "account"));
		state = g_strdup(settings_string(object, "state"));
		if (g_strcmp0(settings_string(object, "success_url"), success_url) ||
			g_strcmp0(settings_string(object, "cancel_url"), cancel_url))
		{
			json_object_set_string_member(object, "success_url", success_url);
			json_object_set_string_member(object, "cancel_url", cancel_url);
			if (!connect_store(database, current, settings, actor, error)) return NULL;
		}
	}
	else
	{
		g_autoptr(StripeRequest) create = stripe_request_new(STRIPE_CONNECT_ACCOUNT_CREATE);
		g_autoptr(VentureEntity) organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, organization_id, error);
		g_autoptr(VentureIntegrationConnection) stored = NULL;
		JsonObject *object;
		if (!organization) return NULL;
		connect_contact(database, organization_id, &create->name, &create->email);
		/* One business, one account, however often this is retried. */
		g_free(create->idempotency_key);
		create->idempotency_key = g_strdup_printf("venture-connect-account-%s", venture_entity_get_uuid(organization));
		made = stripe_client_execute(client, create, NULL, NULL);
		if (!made) return connect_provider_failed(error, "create the connected account");
		account = g_strdup(stripe_resource_get_string(made, "id"));
		if (!connect_account_valid(account)) return connect_provider_failed(error, "return a connected account");
		settings = json_node_new(JSON_NODE_OBJECT);
		object = json_object_new();
		json_node_take_object(settings, object);
		json_object_set_string_member(object, "mode", "connect");
		json_object_set_string_member(object, "account", account);
		json_object_set_string_member(object, "environment", environment);
		json_object_set_string_member(object, "success_url", success_url);
		json_object_set_string_member(object, "cancel_url", cancel_url);
		json_object_set_string_member(object, "state", "needs_more");
		stored = venture_integration_service_configure(venture_integration_service_get(database),
			organization_id, "stripe", account, environment, settings, 0, actor, error);
		if (!stored) return NULL;
		state = g_strdup("needs_more");
	}
	link = stripe_request_new(STRIPE_ACCOUNT_LINK_CREATE);
	link->id = g_strdup(account);
	link->return_url = g_strdup(return_url);
	link->refresh_url = g_strdup(refresh_url);
	g_clear_pointer(&made, stripe_resource_free);
	made = stripe_client_execute(client, link, NULL, NULL);
	url = made ? stripe_resource_get_string(made, "url") : NULL;
	if (!url || !g_str_has_prefix(url, "https://")) return connect_provider_failed(error, "create an onboarding link");
	return connect_answer(state, "connect", url);
}

JsonNode *
venture_stripe_connect_status(VentureDatabase *database, gint64 organization_id,
	StripeTransport *transport, const VentureActor *actor, GError **error)
{
	ConnectPlatform platform;
	g_autoptr(StripeClient) client = NULL;
	g_autoptr(VentureIntegrationConnection) current = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autoptr(StripeRequest) read = NULL;
	g_autoptr(StripeResource) account = NULL;
	g_autoptr(JsonObject) details = NULL;
	JsonObject *object, *requirements = NULL;
	JsonNode *node;
	g_autofree gchar *environment = NULL;
	const gchar *state;
	if (!settings_manage(database, organization_id, error)) return NULL;
	if (!connect_platform_load(database, &platform, error)) return NULL;
	environment = g_strdup(platform.environment);
	client = connect_client(&platform, transport, error);
	connect_platform_clear(&platform);
	if (!client) return NULL;
	if (!connect_current(database, organization_id, &current, &settings, error)) return NULL;
	if (!current) return connect_answer("not_connected", NULL, NULL);
	/* Pasted keys that were verified and are enabled take payments. */
	if (!connect_mode(settings)) return connect_answer("ready", "keys", NULL);
	object = json_node_get_object(settings);
	if (!connect_same_environment(environment, settings_string(object, "environment"), error)) return NULL;
	read = stripe_request_new(STRIPE_CONNECT_ACCOUNT_RETRIEVE);
	read->id = g_strdup(settings_string(object, "account"));
	account = stripe_client_execute(client, read, NULL, NULL);
	if (!account) return connect_provider_failed(error, "answer the account read");
	details = stripe_resource_get_json(account);
	state = connect_state(details);
	/* What Stripe still wants goes to the log for staff, never to the owner. */
	node = details ? json_object_get_member(details, "requirements") : NULL;
	if (node && JSON_NODE_HOLDS_OBJECT(node)) requirements = json_node_get_object(node);
	g_message("Stripe Connect %s: %s; currently_due %u; disabled_reason %s", read->id, state,
		requirements && json_object_has_member(requirements, "currently_due") &&
		JSON_NODE_HOLDS_ARRAY(json_object_get_member(requirements, "currently_due")) ?
		json_array_get_length(json_object_get_array_member(requirements, "currently_due")) : 0,
		requirements && settings_string(requirements, "disabled_reason") ? settings_string(requirements, "disabled_reason") : "none");
	if (g_strcmp0(settings_string(object, "state"), state))
	{
		json_object_set_string_member(object, "state", state);
		if (!connect_store(database, current, settings, actor, error)) return NULL;
	}
	return connect_answer(state, "connect", NULL);
}

/* The enabled connect-mode Stripe connection for @account in @environment,
 * whichever business it belongs to. Pasted keys for the same account are
 * not it, and neither is a connection made in the other mode. */
static VentureIntegrationConnection *
connect_find_account(VentureDatabase *database, const gchar *account, const gchar *environment, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INTEGRATION_CONNECTION);
	g_autoptr(GPtrArray) found = NULL;
	VentureIntegrationConnection *match = NULL;
	guint i;
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_string(query, "provider", VENTURE_FILTER_OP_EQ, "stripe", error) ||
		!venture_query_add_filter_string(query, "account-id", VENTURE_FILTER_OP_EQ, account, error) ||
		!venture_query_add_filter_int(query, "enabled", VENTURE_FILTER_OP_EQ, 1, error)) return NULL;
	found = venture_database_find(database, query, error);
	if (!found) return NULL;
	for (i = 0; i < found->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(found, i);
		g_autoptr(JsonNode) settings = venture_integration_service_resolve_version(
			venture_integration_service_get(database), venture_entity_get_organization_id(row),
			venture_entity_get_id(row), venture_entity_get_version(row), FALSE, NULL);
		if (!settings || !connect_mode(settings) ||
			g_strcmp0(settings_string(json_node_get_object(settings), "environment"), environment)) continue;
		if (match)
		{
			g_clear_object(&match);
			refuse(error, "Two businesses claim one Stripe connected account");
			return NULL;
		}
		match = g_object_ref(VENTURE_INTEGRATION_CONNECTION(row));
	}
	if (!match) g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No business uses this Stripe account");
	return match;
}

gboolean
venture_stripe_connect_webhook(VentureDatabase *database, GBytes *raw, const gchar *signature,
	StripeTransport *transport, GError **error)
{
	ConnectPlatform platform;
	g_autoptr(StripeClient) verifier = NULL;
	g_autoptr(JsonObject) event = NULL;
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(GError) lookup = NULL;
	gboolean live;
	const gchar *environment;
	JsonNode *node;
	const gchar *account, *type;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "stripe_checkout") == G_TYPE_INVALID)
		return refuse(error, "Stripe module is disabled (stripe.enabled)");
	if (!connect_platform_load(database, &platform, error)) return FALSE;
	live = !g_strcmp0(platform.environment, "live");
	environment = live ? "live" : "test";
	verifier = connect_client(&platform, transport, error);
	connect_platform_clear(&platform);
	if (!verifier) return FALSE;
	/* The raw bytes are verified before anything reads them as JSON. */
	event = stripe_client_verify_webhook(verifier, raw, signature, NULL, NULL);
	if (!event) return refuse(error, "Stripe Connect webhook signature is invalid");
	node = json_object_get_member(event, "livemode");
	if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN ||
		json_node_get_boolean(node) != live)
		return refuse(error, "Stripe Connect event environment does not match the platform");
	account = string_member(event, "account");
	type = string_member(event, "type");
	/* A platform event, or one for an account no business uses: answered
	 * so Stripe stops retrying, and nothing is recorded. */
	if (!account) return TRUE;
	connection = connect_find_account(database, account, environment, &lookup);
	if (!connection)
	{
		if (g_error_matches(lookup, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) return TRUE;
		g_propagate_error(error, g_steal_pointer(&lookup));
		return FALSE;
	}
	if (!g_strcmp0(type, "account.updated"))
	{
		g_autoptr(JsonNode) settings = venture_integration_service_resolve_version(
			venture_integration_service_get(database), venture_entity_get_organization_id(VENTURE_ENTITY(connection)),
			venture_entity_get_id(VENTURE_ENTITY(connection)), venture_entity_get_version(VENTURE_ENTITY(connection)), FALSE, error);
		const gchar *state = connect_state(object_member(object_member(event, "data"), "object"));
		if (!settings) return FALSE;
		if (!g_strcmp0(settings_string(json_node_get_object(settings), "state"), state)) return TRUE;
		json_object_set_string_member(json_node_get_object(settings), "state", state);
		return connect_store(database, connection, settings, NULL, error);
	}
	/* Everything else is that business's own event: its service verifies
	 * again, checks the account, records the event and settles. */
	service = venture_stripe_service_for_connection(database, venture_entity_get_organization_id(VENTURE_ENTITY(connection)),
		venture_entity_get_id(VENTURE_ENTITY(connection)), FALSE, transport, error);
	if (!service) return FALSE;
	return venture_stripe_service_handle_webhook(service, raw, signature, error);
}
