/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

typedef struct {
	const gchar *name;
	const gchar *label;
	const gchar *fallback;
	gboolean integer;
	gboolean sensitive;
} SettingField;
static const SettingField fields[] = {
	{ "host", "Relay hostname", NULL, FALSE, FALSE },
	{ "port", "Relay port", "587", TRUE, FALSE },
	{ "tls", "TLS mode (starttls or implicit)", "starttls", FALSE, FALSE },
	{ "auth", "Authentication (plain, login or none)", "plain", FALSE, FALSE },
	{ "username", "Account username", NULL, FALSE, TRUE },
	{ "password", "Account password", NULL, FALSE, TRUE },
	{ "from", "Sender address", NULL, FALSE, FALSE },
	{ "from-name", "Sender name", "Venture", FALSE, FALSE },
	{ "reply-to", "Reply address (optional)", NULL, FALSE, FALSE },
	{ "timeout", "Timeout in seconds (1–30)", "30", TRUE, FALSE },
	{ "retries", "Transport retries (must be zero)", "0", TRUE, FALSE }
};

JsonNode *venture_organization_mailer_dup_schema(void)
{
	JsonNode *schema = json_node_new(JSON_NODE_OBJECT);
	JsonObject *root = json_object_new(), *properties = json_object_new();
	guint i;
	json_node_take_object(schema, root);
	json_object_set_string_member(root, "type", "object");
	json_object_set_boolean_member(root, "additionalProperties", FALSE);
	json_object_set_object_member(root, "properties", properties);
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
	{
		JsonObject *field = json_object_new();
		json_object_set_string_member(field, "type", fields[i].integer ? "integer" : "string");
		json_object_set_string_member(field, "title", fields[i].label);
		json_object_set_boolean_member(field, "x-sensitive", fields[i].sensitive);
		if (fields[i].fallback != NULL)
		{
			if (fields[i].integer) json_object_set_int_member(field, "default", g_ascii_strtoll(fields[i].fallback, NULL, 10));
			else json_object_set_string_member(field, "default", fields[i].fallback);
		}
		json_object_set_object_member(properties, fields[i].name, field);
	}
	return schema;
}

static JsonNode *normalized_settings(JsonObject *values)
{
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *copy = json_object_new();
	guint i;
	json_node_take_object(node, copy);
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
	{
		JsonNode *value = json_object_get_member(values, fields[i].name);
		if (value != NULL) json_object_set_member(copy, fields[i].name, json_node_copy(value));
		else if (fields[i].fallback != NULL)
		{
			if (fields[i].integer) json_object_set_int_member(copy, fields[i].name, g_ascii_strtoll(fields[i].fallback, NULL, 10));
			else json_object_set_string_member(copy, fields[i].name, fields[i].fallback);
		}
	}
	return node;
}

struct _VentureOrganizationMailer {
	GObject parent_instance;
	VentureDatabase *database;
	VentureConfig *config;
	GThread *owner;
};
static void mailer_iface(VentureMailerInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureOrganizationMailer, venture_organization_mailer, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_MAILER, mailer_iface))

static gboolean refuse(GError **error, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, message);
	return FALSE;
}
static gboolean available(VentureOrganizationMailer *self, GError **error)
{
	if (self->database == NULL || self->owner != g_thread_self())
		return refuse(error, "Organization mail must resolve on its repository thread");
	return TRUE;
}
static gboolean endpoint_allowed(VentureOrganizationMailer *self, JsonObject *values, GError **error)
{
	const gchar *host = venture_json_object_get_string(values, "host", "");
	gint64 port = venture_json_object_get_int(values, "port", 587);
	g_autofree gchar *allowed = NULL, *endpoint = NULL;
	g_auto(GStrv) endpoints = NULL;
	const gchar *p;
	guint i;
	JsonObjectIter iterator;
	const gchar *name;
	JsonNode *value;
	/* A provider's superset includes local CA/DKIM files and SES endpoints.
	 * Organization input may not gain those ambient process capabilities. */
	json_object_iter_init(&iterator, values);
	while (json_object_iter_next(&iterator, &name, &value))
	{
		gboolean valid = FALSE;
		for (i = 0; i < G_N_ELEMENTS(fields); i++)
			if (!g_strcmp0(name, fields[i].name))
				valid = json_node_get_value_type(value) == (fields[i].integer ? G_TYPE_INT64 : G_TYPE_STRING);
		if (!valid) return refuse(error, "SMTP settings contain an unsupported field or value type");
	}
	if (*host == '\0' || port < 1 || port > 65535)
		return refuse(error, "SMTP requires a valid operator-allowed endpoint");
	for (p = host; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '.' && *p != '-')
			return refuse(error, "SMTP requires a DNS hostname or IPv4 endpoint");
	endpoint = g_strdup_printf("%s:%" G_GINT64_FORMAT, host, port);
	g_object_get(self->config, "mail-allowed-endpoints", &allowed, NULL);
	endpoints = g_strsplit(allowed != NULL ? allowed : "", ",", -1);
	for (i = 0; endpoints[i] != NULL; i++)
		if (g_ascii_strcasecmp(g_strstrip(endpoints[i]), endpoint) == 0) return TRUE;
	return refuse(error, "SMTP endpoint is not permitted by the operator");
}
static VentureMailer *prepare(VentureMailer *mailer, VentureMailMessage *message, GError **error)
{
	VentureOrganizationMailer *self = VENTURE_ORGANIZATION_MAILER(mailer);
	VentureIntegrationService *service;
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autofree gchar *ca_file = NULL;
	gint64 org = venture_entity_get_organization_id(VENTURE_ENTITY(message));
	gint64 bound = 0;
	if (!available(self, error)) return NULL;
	service = venture_integration_service_get(self->database);
	connection = venture_integration_service_find(service, org, "smtp", error);
	if (connection == NULL) return NULL;
	g_object_get(message, "connection-id", &bound, NULL);
	if (bound != 0 && bound != venture_entity_get_id(VENTURE_ENTITY(connection)))
	{
		refuse(error, "Mail belongs to a previous SMTP account; enqueue a new message deliberately");
		return NULL;
	}
	settings = venture_integration_service_resolve_version(service, org,
		venture_entity_get_id(VENTURE_ENTITY(connection)), venture_entity_get_version(VENTURE_ENTITY(connection)), FALSE, error);
	if (settings == NULL || !endpoint_allowed(self, json_node_get_object(settings), error)) return NULL;
	g_object_get(self->config, "mail-tls-ca-file", &ca_file, NULL);
	if (ca_file != NULL && *ca_file != '\0')
		json_object_set_string_member(json_node_get_object(settings), "tls-ca-file", ca_file);
	return VENTURE_MAILER(venture_smtp_mailer_new_for_connection(json_node_get_object(settings),
		venture_entity_get_id(VENTURE_ENTITY(connection)), venture_entity_get_version(VENTURE_ENTITY(connection)), error));
}
static void mailer_iface(VentureMailerInterface *iface) { iface->prepare = prepare; }
static void finalize(GObject *object)
{
	VentureOrganizationMailer *self = VENTURE_ORGANIZATION_MAILER(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_clear_object(&self->config);
	g_thread_unref(self->owner);
	G_OBJECT_CLASS(venture_organization_mailer_parent_class)->finalize(object);
}
static void venture_organization_mailer_class_init(VentureOrganizationMailerClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = finalize;
}
static void venture_organization_mailer_init(VentureOrganizationMailer *self)
{
	self->owner = g_thread_ref(g_thread_self());
}
VentureOrganizationMailer *venture_organization_mailer_new(VentureDatabase *database, VentureConfig *config)
{
	VentureOrganizationMailer *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_CONFIG(config), NULL);
	self = g_object_new(VENTURE_TYPE_ORGANIZATION_MAILER, NULL);
	self->database = database;
	g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
	self->config = g_object_ref(config);
	return self;
}
VentureIntegrationConnection *venture_organization_mailer_configure(VentureOrganizationMailer *self,
	gint64 organization_id, JsonObject *values, gint64 expected_version, gint64 expected_connection,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureSmtpMailer) checked = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autoptr(VentureIntegrationConnection) active = NULL, result = NULL;
	g_autoptr(GError) lookup_error = NULL;
	g_autofree gchar *host = NULL, *identity = NULL, *digest = NULL;
	VentureIntegrationService *service;
	g_return_val_if_fail(VENTURE_IS_ORGANIZATION_MAILER(self), NULL);
	g_return_val_if_fail(values != NULL, NULL);
	if (!available(self, error) || !endpoint_allowed(self, values, error)) return NULL;
	settings = normalized_settings(values);
	values = json_node_get_object(settings);
	checked = venture_smtp_mailer_new_from_values(values, error);
	if (checked == NULL) return NULL;
	host = g_ascii_strdown(venture_json_object_get_string(values, "host", ""), -1);
	identity = g_strdup_printf("%s\n%" G_GINT64_FORMAT "\n%s\n%s\n%s", host,
		venture_json_object_get_int(values, "port", 587),
		venture_json_object_get_string(values, "auth", "plain"),
		venture_json_object_get_string(values, "username", ""),
		venture_json_object_get_string(values, "from", ""));
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
	if (!venture_database_begin(self->database, error)) return NULL;
	service = venture_integration_service_get(self->database);
	active = venture_integration_service_find(service, organization_id, "smtp", &lookup_error);
	if (lookup_error != NULL && !g_error_matches(lookup_error, VENTURE_ERROR, VENTURE_ERROR_CONFIG))
	{
		g_propagate_error(error, g_steal_pointer(&lookup_error));
		goto fail;
	}
	if (expected_connection != (active != NULL ? venture_entity_get_id(VENTURE_ENTITY(active)) : 0) ||
		expected_version != (active != NULL ? venture_entity_get_version(VENTURE_ENTITY(active)) : 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "SMTP binding changed; reload organization settings");
		goto fail;
	}
	result = venture_integration_service_configure(service,
		organization_id, "smtp", digest, "live", settings, expected_version, actor, error);
	if (result == NULL) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

JsonNode *venture_organization_mailer_status(VentureOrganizationMailer *self,
	gint64 organization_id, GError **error)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(VentureMailer) transport = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(VentureEntity) latest = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *account = NULL, *state = NULL, *reason = NULL;
	gint64 id, version, selected_id = 0, selected_version = 0;
	json_node_take_object(node, object);
	g_return_val_if_fail(VENTURE_IS_ORGANIZATION_MAILER(self), NULL);
	if (!available(self, error)) return NULL;
	connection = venture_integration_service_find(venture_integration_service_get(self->database), organization_id, "smtp", &local);
	if (connection == NULL)
	{
		if (!g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_CONFIG))
		{
			g_propagate_error(error, g_steal_pointer(&local));
			return NULL;
		}
		json_object_set_string_member(object, "state", "unconfigured");
		return g_steal_pointer(&node);
	}
	id = venture_entity_get_id(VENTURE_ENTITY(connection));
	version = venture_entity_get_version(VENTURE_ENTITY(connection));
	g_object_get(connection, "account-id", &account, NULL);
	json_object_set_int_member(object, "connection_id", id);
	json_object_set_int_member(object, "connection_version", version);
	json_object_set_string_member(object, "account_id", account);
	g_object_set(message, "organization-id", organization_id, "connection-id", id, NULL);
	transport = venture_mailer_prepare(VENTURE_MAILER(self), message, &local);
	if (transport != NULL) venture_mailer_get_binding(transport, &selected_id, &selected_version);
	if (transport == NULL || selected_id != id || selected_version != version)
	{
		json_object_set_string_member(object, "state", "unavailable");
		json_object_set_string_member(object, "error", local != NULL ? local->message : "Configuration changed; reload settings");
		return g_steal_pointer(&node);
	}
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_int(query, "connection-id", VENTURE_FILTER_OP_EQ, id, NULL);
	venture_query_add_filter_int(query, "connection-version", VENTURE_FILTER_OP_EQ, version, NULL);
	venture_query_add_order(query, "updated-at", VENTURE_SORT_DESCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	latest = venture_database_find_one(self->database, query, &local);
	if (local != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	if (latest != NULL)
	{
		g_object_get(latest, "state", &state, "last-error", &reason, NULL);
		json_object_set_int_member(object, "message_id", venture_entity_get_id(latest));
	}
	json_object_set_string_member(object, "state", state != NULL ? state : "unverified");
	if (reason != NULL && *reason != '\0') json_object_set_string_member(object, "error", reason);
	return g_steal_pointer(&node);
}
