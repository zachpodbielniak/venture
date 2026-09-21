/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "sequences/venture-sequence-tracking-private.h"

struct _VentureAttributionService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *permit;
	gboolean installed;
};
G_DEFINE_FINAL_TYPE(VentureAttributionService, venture_attribution_service, G_TYPE_OBJECT)

static gboolean refuse(GError **error, const gchar *message)
{
	venture_set_error_validation(error, "attribution", "%s", message);
	return FALSE;
}
static gchar *string(VentureEntity *row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}
static gint64 number(VentureEntity *row, const gchar *field)
{
	gint64 value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}
static gboolean bounded(const gchar *value, gsize limit)
{
	return value && *value && strlen(value) <= limit && g_utf8_validate(value, -1, NULL) && !strpbrk(value, "\r\n");
}
static gboolean identifier(const gchar *value)
{
	gsize i;
	if (!bounded(value, 128)) return FALSE;
	for (i = 0; value[i]; i++)
		if (!g_ascii_isalnum(value[i]) && !strchr("._:-", value[i])) return FALSE;
	return TRUE;
}
static gchar *origin_normalize(const gchar *text, GError **error)
{
	g_autoptr(GUri) uri = text ? g_uri_parse(text, G_URI_FLAGS_NONE, NULL) : NULL;
	const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL, *host = uri ? g_uri_get_host(uri) : NULL;
	const gchar *path = uri ? g_uri_get_path(uri) : NULL;
	g_autofree gchar *lower = NULL;
	gint port;
	gboolean local = host && (!g_ascii_strcasecmp(host, "localhost") || !strcmp(host, "127.0.0.1") || !strcmp(host, "::1"));
	if (!bounded(text, 2048) || !host || !*host || g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri) ||
		(path && *path && strcmp(path, "/")) || (g_strcmp0(scheme, "https") && !(local && !g_strcmp0(scheme, "http")))) {
		refuse(error, "Site origin requires HTTPS without credentials, path, query or fragment; loopback HTTP is for development"); return NULL;
	}
	lower = g_ascii_strdown(host, -1); port = g_uri_get_port(uri);
	if ((!strcmp(scheme, "https") && port == 443) || (!strcmp(scheme, "http") && port == 80)) port = -1;
	return g_uri_join(G_URI_FLAGS_NONE, scheme, NULL, lower, port, "", NULL, NULL);
}
static VentureEntity *get_row(VentureAttributionService *self, GType type, gint64 org, gint64 id, GError **error)
{
	VentureEntity *row = venture_database_get(self->database, type, id, error);
	if (row && (venture_entity_is_deleted(row) || venture_entity_get_organization_id(row) != org)) g_clear_object(&row);
	if (!row && (!error || !*error)) g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Attribution record unavailable");
	return row;
}
static JsonNode *campaign_mapping(VentureEntity *row, GError **error)
{
	g_autofree gchar *text = string(row, "campaign-map");
	g_autoptr(JsonNode) mapping = NULL;
	if (venture_string_is_empty(text)) return NULL;
	if (strlen(text) > 16384) { refuse(error, "Campaign mapping exceeds its 16 KiB bound"); return NULL; }
	mapping = json_from_string(text, NULL);
	if (!mapping || (!JSON_NODE_HOLDS_NULL(mapping) && (!JSON_NODE_HOLDS_OBJECT(mapping) || json_object_get_size(json_node_get_object(mapping)) > 100))) {
		refuse(error, "Campaign mapping requires an object with at most 100 labels"); return NULL;
	}
	return g_steal_pointer(&mapping);
}
static gboolean site_validate(VentureAttributionService *self, VentureEntity *row, VentureEntity *previous, GError **error)
{
	gint64 org = venture_entity_get_organization_id(row), days = number(row, "lookback-days"), retention = number(row, "retention-days");
	g_autofree gchar *raw = string(row, "origin"), *origin = origin_normalize(raw, error);
	g_autofree gchar *external = string(row, "external-site-id"), *tenant = string(row, "external-tenant-id");
	g_autofree gchar *policy = string(row, "consent-policy"), *key = NULL;
	g_autoptr(VentureEntity) form = NULL, connection = NULL;
	g_autoptr(JsonNode) mapping = NULL;
	g_autofree gchar *map_text = string(row, "campaign-map"), *old_map_text = previous ? string(previous, "campaign-map") : NULL;
	if (!origin) return FALSE;
	if (org <= 0 || !identifier(external) || !identifier(tenant) || !bounded(policy, 128))
		return refuse(error, "Site requires an exact organization, external tenant/site identities and analytics policy version");
	if (!days) days = 30;
	if (!retention) retention = 30;
	if (days < 1 || days > 365 || retention < days || retention > 365)
		return refuse(error, "Lookback and retention require one through 365 days; retention cannot be shorter than lookback");
	if (!previous || number(previous, "lead-form-id") != number(row, "lead-form-id")) {
		form = get_row(self, VENTURE_TYPE_LEAD_FORM, org, number(row, "lead-form-id"), error);
		if (!form) return FALSE;
	}
	if (number(row, "connection-id") && (!previous || number(previous, "connection-id") != number(row, "connection-id"))) {
		g_autofree gchar *provider = NULL, *account = NULL;
		connection = get_row(self, VENTURE_TYPE_INTEGRATION_CONNECTION, org, number(row, "connection-id"), error);
		if (!connection) return FALSE;
		provider = string(connection, "provider"); account = string(connection, "account-id");
		if (g_strcmp0(provider, "lightsite_capture") || g_strcmp0(account, tenant))
			return refuse(error, "Site must name its own organization's exact Lightsite tenant account");
	}
	mapping = campaign_mapping(row, error);
	if (!mapping && !venture_string_is_empty(map_text)) return FALSE;
	if (mapping && !JSON_NODE_HOLDS_NULL(mapping)) {
		JsonObjectIter iter;
		JsonNode *value;
		const gchar *name;
		if (!JSON_NODE_HOLDS_OBJECT(mapping) || json_object_get_size(json_node_get_object(mapping)) > 100)
			return refuse(error, "Campaign mapping requires an object with at most 100 labels");
		json_object_iter_init(&iter, json_node_get_object(mapping));
		while (json_object_iter_next(&iter, &name, &value)) {
			g_autoptr(VentureEntity) campaign = NULL;
			if (!identifier(name) || !JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_INT64 || json_node_get_int(value) <= 0)
				return refuse(error, "Campaign mappings require safe labels and positive integer campaign IDs");
			if (!previous || g_strcmp0(map_text, old_map_text)) {
				campaign = get_row(self, VENTURE_TYPE_CAMPAIGN, org, json_node_get_int(value), error);
				if (!campaign) return FALSE;
			}
		}
	}
	{
		g_autofree gchar *email_policy = string(row, "marketing-policy"), *statement = string(row, "marketing-statement");
		if (!venture_string_is_empty(email_policy) || !venture_string_is_empty(statement)) {
			if (!bounded(email_policy, 128) || venture_string_is_empty(statement) || strlen(statement) > 4096 || !g_utf8_validate(statement, -1, NULL))
				return refuse(error, "Optional email permission requires a bounded policy version and approved statement");
		}
	}
	key = g_strdup_printf("%s/%s", tenant, external);
	g_object_set(row, "origin", origin, "site-key", key, "lookback-days", days, "retention-days", retention, NULL);
	return TRUE;
}
static gboolean administrative(VentureAttributionService *self, gint64 org, GError **error);
static gboolean validate(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	VentureAttributionService *self = data;
	gboolean permitted = self->permit == row;
	self->permit = NULL;
	if (previous && venture_entity_get_organization_id(row) != venture_entity_get_organization_id(previous))
		return refuse(error, "Attribution evidence cannot move to another organization");
	if (permitted) return TRUE;
	if (VENTURE_IS_ATTRIBUTION_SITE(row)) {
		const gchar *fields[] = { "origin", "external-site-id", "external-tenant-id" };
		guint i;
		if (!administrative(self, venture_entity_get_organization_id(row), error) || !site_validate(self, row, previous, error)) return FALSE;
		for (i = 0; previous && i < G_N_ELEMENTS(fields); i++) {
			g_autofree gchar *old = string(previous, fields[i]), *value = string(row, fields[i]);
			if (g_strcmp0(old, value)) return refuse(error, "A site's original origin and remote tenant/site identity are immutable");
		}
		return TRUE;
	}
	return refuse(error, "Analytics consent, observations, submissions and acquisition bindings require the attribution service");
}
#include "venture-attribution-consent.inc"
#include "venture-attribution-observations.inc"
#include "venture-attribution-retention.inc"
#include "venture-attribution-capture.inc"
#include "venture-attribution-bindings.inc"
#include "venture-attribution-lightsite.inc"
#include "venture-attribution-actions.inc"

static void finalize(GObject *object)
{
	VentureAttributionService *self = VENTURE_ATTRIBUTION_SERVICE(object);
	if (self->database) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_attribution_service_parent_class)->finalize(object);
}
static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureAttributionService *self = VENTURE_ATTRIBUTION_SERVICE(object);
	if (id != 1) { G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec); return; }
	self->database = g_value_get_object(value);
	if (self->database) g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
}
static void get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	VentureAttributionService *self = VENTURE_ATTRIBUTION_SERVICE(object);
	if (id == 1) g_value_set_object(value, self->database);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void venture_attribution_service_class_init(VentureAttributionServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = finalize;
	G_OBJECT_CLASS(klass)->set_property = set_property;
	G_OBJECT_CLASS(klass)->get_property = get_property;
	g_object_class_install_property(G_OBJECT_CLASS(klass), 1, g_param_spec_object("database", "Database",
		"Weak owning repository", VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void venture_attribution_service_init(VentureAttributionService *self) { }
VentureAttributionService *venture_attribution_service_get(VentureDatabase *database)
{
	VentureAttributionService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-attribution-service");
	if (!self) {
		self = g_object_new(VENTURE_TYPE_ATTRIBUTION_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-attribution-service", self, g_object_unref);
	}
	return self;
}
void venture_attribution_install(VentureDatabase *database)
{
	GType types[] = { VENTURE_TYPE_ATTRIBUTION_SITE, VENTURE_TYPE_ATTRIBUTION_VISITOR,
		VENTURE_TYPE_ATTRIBUTION_TOUCH, VENTURE_TYPE_ATTRIBUTION_SUBMISSION, VENTURE_TYPE_ATTRIBUTION_BINDING };
	VentureAttributionService *self = venture_attribution_service_get(database);
	guint i;
	if (self->installed) return;
	self->installed = TRUE;
	for (i = 0; i < G_N_ELEMENTS(types); i++) venture_database_add_save_validator(database, types[i], validate, self, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_LEAD, validate_conversion, self, NULL);
}
gboolean venture_attribution_check_removal(VentureEntity *entity, GError **error)
{
	if (VENTURE_IS_ATTRIBUTION_SITE(entity) || VENTURE_IS_ATTRIBUTION_VISITOR(entity) || VENTURE_IS_ATTRIBUTION_TOUCH(entity) ||
		VENTURE_IS_ATTRIBUTION_SUBMISSION(entity) || VENTURE_IS_ATTRIBUTION_BINDING(entity))
		return refuse(error, "Deactivate sites or use analytics withdrawal/retention; retained evidence cannot be removed by generic CRUD");
	return TRUE;
}
