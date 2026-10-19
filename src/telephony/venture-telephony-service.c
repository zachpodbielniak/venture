/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-telephony-service.c - Lines, click-to-call, call logging and texts
 *
 * The writes here are the only writes of phone lines and text messages
 * (the save validator refuses every other writer), and
 * deliver_one() is the only place a text is handed to a provider: it
 * checks consent and quiet hours again immediately before, so a sequence,
 * a campaign, the API or a page can only queue.
 */
#include <venture.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <glib/gstdio.h>
#include <sys/random.h>
#include <libsoup/soup.h>

struct _VentureTelephonyService {
	GObject parent_instance;
	VentureDatabase *database;	/* weak */
	GWeakRef context;
	gint writing;
	gboolean installed;
};
G_DEFINE_FINAL_TYPE(VentureTelephonyService, venture_telephony_service, G_TYPE_OBJECT)

#define VAULT_PREFIX "telephony-"

static void
finalize(GObject *object)
{
	VentureTelephonyService *self = VENTURE_TELEPHONY_SERVICE(object);
	if (self->database)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_weak_ref_clear(&self->context);
	G_OBJECT_CLASS(venture_telephony_service_parent_class)->finalize(object);
}

static void venture_telephony_service_class_init(VentureTelephonyServiceClass *klass) { G_OBJECT_CLASS(klass)->finalize = finalize; }
static void venture_telephony_service_init(VentureTelephonyService *self) { g_weak_ref_init(&self->context, NULL); }

VentureTelephonyService *
venture_telephony_service_get(VentureDatabase *database)
{
	VentureTelephonyService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-telephony-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_TELEPHONY_SERVICE, NULL);
		self->database = database;
		g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
		g_object_set_data_full(G_OBJECT(database), "venture-telephony-service", self, g_object_unref);
	}
	return self;
}

/* --- Small helpers ------------------------------------------------------------ */

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, code, message);
	return FALSE;
}

static gchar *
string(gpointer row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}

static gint64
number(gpointer row, const gchar *field)
{
	gint64 value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}

static gboolean
flag(gpointer row, const gchar *field)
{
	gboolean value = FALSE;
	g_object_get(row, field, &value, NULL);
	return value;
}

static const VentureActor *
system_actor(void)
{
	static VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor.name = "Phone system";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	return &actor;
}

static gboolean
save(VentureTelephonyService *self, VentureEntity *row, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing++;
	ok = venture_database_save(self->database, row, actor != NULL ? actor : system_actor(), error);
	self->writing--;
	return ok;
}

static gchar *
random_token(GError **error)
{
	guchar bytes[32];
	gsize used = 0, i;
	GString *text;
	while (used < sizeof bytes)
	{
		ssize_t n = getrandom(bytes + used, sizeof bytes - used, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0)
		{
			refuse(error, VENTURE_ERROR_FAILED, "No secure randomness is available");
			return NULL;
		}
		used += (gsize)n;
	}
	text = g_string_sized_new(64);
	for (i = 0; i < sizeof bytes; i++)
		g_string_append_printf(text, "%02x", bytes[i]);
	memset(bytes, 0, sizeof bytes);
	return g_string_free(text, FALSE);
}

static VentureEntity *
organization(VentureTelephonyService *self, gint64 org)
{
	return venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, org, NULL);
}

static gchar *
country(VentureTelephonyService *self, gint64 org)
{
	g_autoptr(VentureEntity) row = organization(self, org);
	return row != NULL ? string(row, "telephony-country") : NULL;
}

static gchar *
normalized(VentureTelephonyService *self, gint64 org, const gchar *raw)
{
	g_autofree gchar *home = country(self, org);
	return venture_phone_normalize(raw, home);
}

static gchar *
display_name(VentureEntity *row)
{
	g_autofree gchar *name = string(row, "name");
	return g_strdup(venture_string_is_empty(name) ? "This contact" : name);
}

static gboolean
module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), "text_message") != G_TYPE_INVALID;
}

static gboolean
require_admin(VentureTelephonyService *self, gint64 org, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureAccessPolicy *policy = venture_database_get_access_policy(self->database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(policy);
	if (principal != NULL && !venture_access_policy_has_organization_role(policy, principal, org, roles, G_N_ELEMENTS(roles)))
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Connecting a phone line needs an owner or administrator of the organization");
	return TRUE;
}

/* --- Write guards ---------------------------------------------------------------- */

static gboolean
validate(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	VentureTelephonyService *self = data;
	(void)database;
	if (previous != NULL && venture_entity_get_organization_id(previous) != venture_entity_get_organization_id(row))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Phone records cannot move to another organization");
	if (VENTURE_IS_TELEPHONY_EXTENSION(row))
	{
		g_autofree gchar *extension = string(row, "extension"), *key = NULL;
		const gchar *p;
		for (p = extension; p != NULL && *p != '\0' && g_ascii_isdigit(*p); p++);
		if (venture_string_is_empty(extension) || *p != '\0' || strlen(extension) > 16)
			return refuse(error, VENTURE_ERROR_VALIDATION, "An extension is digits only, for example 101");
		key = g_strdup_printf("user:%" G_GINT64_FORMAT, number(row, "user-id"));
		g_object_set(row, "user-key", key, NULL);
		return TRUE;
	}
	if (self->writing > 0)
		return TRUE;
	return refuse(error, VENTURE_ERROR_PERMISSION_DENIED,
		"Phone lines and text messages are written only by the telephony service; use Telephony settings or send a text");
}

/* --- Lines ---------------------------------------------------------------------- */

static VentureTelephonyConnector *
connector_for(VentureEntity *account, GError **error)
{
	g_autofree gchar *provider = string(account, "provider");
	VentureTelephonyConnector *connector = venture_telephony_provider_new(provider);
	if (connector == NULL)
		refuse(error, VENTURE_ERROR_CONFIG, "This line's phone provider is not available in this build");
	return connector;
}

static gboolean
allow_loopback(VentureTelephonyService *self)
{
	g_autoptr(VentureContext) context = g_weak_ref_get(&self->context);
	gboolean allowed = FALSE;
	if (context != NULL)
		g_object_get(venture_context_get_config(context), "connectors-allow-plaintext-loopback", &allowed, NULL);
	return allowed;
}

static VentureTelephonyLink *
open_link(VentureTelephonyService *self, VentureEntity *account, VentureTelephonyConnector *connector, GError **error)
{
	g_autoptr(JsonNode) settings = NULL;
	g_autofree gchar *base = string(account, "base-url"), *phone = string(account, "phone-number");
	if (!flag(account, "active"))
	{
		refuse(error, VENTURE_ERROR_CONFIG, "This phone line is disconnected");
		return NULL;
	}
	settings = venture_integration_service_resolve(venture_integration_service_get(self->database),
		venture_entity_get_organization_id(account), number(account, "connection-id"), FALSE, error);
	if (settings == NULL || !JSON_NODE_HOLDS_OBJECT(settings))
		return NULL;
	return venture_telephony_link_new(!venture_string_is_empty(base) ? base : venture_telephony_connector_default_base_url(connector),
		json_node_get_object(settings), phone, allow_loopback(self), error);
}

static gboolean
valid_sources(const gchar *text, GError **error)
{
	g_auto(GStrv) lines = g_strsplit_set(text != NULL ? text : "", "\n,", -1);
	guint i;
	for (i = 0; lines[i] != NULL; i++)
	{
		g_autoptr(GInetAddressMask) mask = NULL;
		if (*g_strstrip(lines[i]) == '\0') continue;
		mask = g_inet_address_mask_new_from_string(lines[i], NULL);
		if (mask == NULL)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Allowed senders are addresses, one per line, for example 192.0.2.10 or 192.0.2.0/24");
	}
	return TRUE;
}

static gboolean
source_allowed(const gchar *list, const gchar *remote)
{
	g_auto(GStrv) lines = g_strsplit_set(list != NULL ? list : "", "\n,", -1);
	g_autoptr(GInetAddress) address = remote != NULL ? g_inet_address_new_from_string(remote) : NULL;
	gboolean any = FALSE;
	guint i;
	for (i = 0; lines[i] != NULL; i++)
	{
		g_autoptr(GInetAddressMask) mask = NULL;
		if (*g_strstrip(lines[i]) == '\0') continue;
		any = TRUE;
		mask = g_inet_address_mask_new_from_string(lines[i], NULL);
		if (mask != NULL && address != NULL && g_inet_address_mask_matches(mask, address))
			return TRUE;
	}
	return !any;
}

VentureEntity *
venture_telephony_service_connect(VentureTelephonyService *self, gint64 organization_id, const gchar *provider,
	JsonObject *settings, gint64 account_id, const VentureActor *actor, gchar **webhook_token, GError **error)
{
	g_autoptr(VentureTelephonyConnector) connector = NULL;
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(JsonNode) secret = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureTelephonyLink) probe = NULL;
	g_autofree gchar *identity = NULL, *vault = NULL, *phone = NULL, *token = NULL, *hash = NULL;
	const gchar *base, *name, *raw_phone;
	JsonObjectIter iter;
	const gchar *field;
	JsonNode *node;
	gint64 version = 0;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), NULL);
	g_return_val_if_fail(settings != NULL, NULL);
	if (self->database == NULL || !module_on())
	{
		refuse(error, VENTURE_ERROR_CONFIG, "Phone and text is switched off");
		return NULL;
	}
	if (organization_id <= 0 || !require_admin(self, organization_id, error))
		return NULL;
	connector = venture_telephony_provider_new(provider);
	if (connector == NULL)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Choose a phone provider Venture knows: 3CX or VoIP.ms");
		return NULL;
	}
	identity = venture_telephony_connector_account_identity(connector, settings, error);
	if (identity == NULL)
		return NULL;
	/* The vault names an account by letters, digits, dots and dashes; an
	 * email-shaped user name is kept as its fingerprint instead. */
	if (strlen(identity) > 128 || strspn(identity, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != strlen(identity))
	{
		gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
		digest[40] = '\0';
		g_free(identity);
		identity = digest;
	}
	base = venture_json_object_get_string(settings, "base_url", NULL);
	if (venture_string_is_empty(base))
		base = venture_telephony_connector_default_base_url(connector);
	if (venture_string_is_empty(base))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s needs its address, for example https://pbx.example.com",
			venture_telephony_connector_get_label(connector));
		return NULL;
	}
	probe = venture_telephony_link_new(base, NULL, NULL, allow_loopback(self), error);
	if (probe == NULL)
		return NULL;
	raw_phone = venture_json_object_get_string(settings, "phone_number", NULL);
	if (!venture_string_is_empty(raw_phone))
	{
		phone = normalized(self, organization_id, raw_phone);
		if (phone == NULL)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "The texting number is not a phone number Venture can read; write it as +15550100100");
			return NULL;
		}
	}
	else if (venture_telephony_connector_get_capabilities(connector) & VENTURE_TELEPHONY_CAPABILITY_SMS)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "A texting line needs the number texts are sent from");
		return NULL;
	}
	if (!valid_sources(venture_json_object_get_string(settings, "webhook_sources", NULL), error))
		return NULL;
	/* Only the provider's own credential fields go to the vault. */
	json_node_take_object(secret, json_object_new());
	schema = venture_telephony_connector_settings_schema(connector);
	json_object_iter_init(&iter, json_object_get_object_member(json_node_get_object(schema), "properties"));
	while (json_object_iter_next(&iter, &field, &node))
	{
		const gchar *value = venture_json_object_get_string(settings, field, NULL);
		if (!venture_string_is_empty(value))
			json_object_set_string_member(json_node_get_object(secret), field, value);
	}
	if (account_id > 0)
	{
		account = venture_database_get(self->database, VENTURE_TYPE_TELEPHONY_ACCOUNT, account_id, error);
		if (account == NULL || venture_entity_get_organization_id(account) != organization_id)
		{
			if (error != NULL && *error == NULL)
				refuse(error, VENTURE_ERROR_NOT_FOUND, "Phone line not found");
			return NULL;
		}
		{
			g_autofree gchar *current = string(account, "provider");
			if (g_strcmp0(current, provider) != 0)
			{
				refuse(error, VENTURE_ERROR_VALIDATION, "A line keeps its provider; disconnect it and connect a new one instead");
				return NULL;
			}
		}
		if (number(account, "connection-id") > 0)
		{
			g_autoptr(VentureEntity) current = venture_database_get(self->database, VENTURE_TYPE_INTEGRATION_CONNECTION,
				number(account, "connection-id"), NULL);
			if (current != NULL && flag(current, "enabled"))
				version = venture_entity_get_version(current);
		}
	}
	vault = g_strconcat(VAULT_PREFIX, provider, NULL);
	connection = venture_integration_service_configure(venture_integration_service_get(self->database), organization_id,
		vault, identity, "live", secret, version, actor, error);
	if (connection == NULL)
		return NULL;
	if (account == NULL)
	{
		account = g_object_new(VENTURE_TYPE_TELEPHONY_ACCOUNT, "organization-id", organization_id, "provider", provider, NULL);
		token = random_token(error);
		if (token == NULL)
			return NULL;
		hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
		g_object_set(account, "webhook-token-hash", hash, NULL);
	}
	name = venture_json_object_get_string(settings, "name", NULL);
	g_object_set(account, "name", !venture_string_is_empty(name) ? name : venture_telephony_connector_get_label(connector),
		"base-url", base, "phone-number", phone, "connection-id", venture_entity_get_id(VENTURE_ENTITY(connection)),
		"webhook-sources", venture_json_object_get_string(settings, "webhook_sources", ""),
		"media-hosts", venture_json_object_get_string(settings, "media_hosts", ""),
		"active", TRUE, "last-error", "", NULL);
	if (!save(self, account, actor, error))
		return NULL;
	if (webhook_token != NULL)
		*webhook_token = g_steal_pointer(&token);
	return g_steal_pointer(&account);
}

gchar *
venture_telephony_service_new_webhook_token(VentureTelephonyService *self, VentureEntity *account,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) live = NULL;
	g_autofree gchar *token = NULL, *hash = NULL;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), NULL);
	if (!require_admin(self, venture_entity_get_organization_id(account), error))
		return NULL;
	live = venture_database_get(self->database, VENTURE_TYPE_TELEPHONY_ACCOUNT, venture_entity_get_id(account), error);
	if (live == NULL)
		return NULL;
	token = random_token(error);
	if (token == NULL)
		return NULL;
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	g_object_set(live, "webhook-token-hash", hash, NULL);
	if (!save(self, live, actor, error))
		return NULL;
	return g_steal_pointer(&token);
}

gboolean
venture_telephony_service_disconnect(VentureTelephonyService *self, VentureEntity *account,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) live = NULL, connection = NULL;
	gint64 org = venture_entity_get_organization_id(account);
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), FALSE);
	if (!require_admin(self, org, error))
		return FALSE;
	live = venture_database_get(self->database, VENTURE_TYPE_TELEPHONY_ACCOUNT, venture_entity_get_id(account), error);
	if (live == NULL)
		return FALSE;
	connection = venture_database_get(self->database, VENTURE_TYPE_INTEGRATION_CONNECTION, number(live, "connection-id"), NULL);
	if (connection != NULL && flag(connection, "enabled") &&
		!venture_integration_service_disable(venture_integration_service_get(self->database), org,
			venture_entity_get_id(connection), venture_entity_get_version(connection), actor, error))
		return FALSE;
	g_object_set(live, "active", FALSE, NULL);
	return save(self, live, actor, error);
}

VentureEntity *
venture_telephony_service_line(VentureTelephonyService *self, gint64 organization_id,
	VentureTelephonyCapabilities needs, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TELEPHONY_ACCOUNT);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), NULL);
	if (self->database == NULL || !module_on())
	{
		refuse(error, VENTURE_ERROR_CONFIG, "Phone and text is switched off");
		return NULL;
	}
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureTelephonyConnector) connector = connector_for(g_ptr_array_index(rows, i), NULL);
		if (connector != NULL && (venture_telephony_connector_get_capabilities(connector) & needs) == needs)
			return g_object_ref(g_ptr_array_index(rows, i));
	}
	if (needs & VENTURE_TELEPHONY_CAPABILITY_CLICK_TO_CALL)
		refuse(error, VENTURE_ERROR_CONFIG, "No phone system is connected for this organization; an administrator can connect one in Telephony settings");
	else if (needs & VENTURE_TELEPHONY_CAPABILITY_MMS)
		refuse(error, VENTURE_ERROR_CONFIG, "No line that sends pictures is connected for this organization");
	else
		refuse(error, VENTURE_ERROR_CONFIG, "No texting line is connected for this organization; an administrator can connect one in Telephony settings");
	return NULL;
}

gchar *
venture_telephony_service_extension(VentureTelephonyService *self, gint64 organization_id, gint64 user_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TELEPHONY_EXTENSION);
	g_autoptr(VentureEntity) row = NULL;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), NULL);
	if (self->database == NULL || user_id <= 0 || !module_on())
		return NULL;
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user_id, NULL);
	row = venture_database_find_one(self->database, query, NULL);
	return row != NULL ? string(row, "extension") : NULL;
}

/* Whose extension it is, for the call's owner. */
static gchar *
extension_owner(VentureTelephonyService *self, gint64 org, const gchar *extension)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TELEPHONY_EXTENSION);
	g_autoptr(VentureEntity) row = NULL, user = NULL;
	if (venture_string_is_empty(extension))
		return NULL;
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "extension", VENTURE_FILTER_OP_EQ, extension, NULL);
	row = venture_database_find_one(self->database, query, NULL);
	if (row != NULL)
		user = venture_database_get(self->database, VENTURE_TYPE_USER, number(row, "user-id"), NULL);
	return user != NULL ? string(user, "username") : NULL;
}

static void
note_error(VentureTelephonyService *self, VentureEntity *account, const GError *problem)
{
	g_autoptr(VentureEntity) live = venture_database_get(self->database, VENTURE_TYPE_TELEPHONY_ACCOUNT,
		venture_entity_get_id(account), NULL);
	if (live == NULL || problem == NULL)
		return;
	g_object_set(live, "last-error", problem->message, NULL);
	save(self, live, NULL, NULL);
}

/* --- Click-to-call ----------------------------------------------------------------- */

static VentureEntity *
subject_contact(VentureTelephonyService *self, VentureEntity *subject, GError **error)
{
	gint64 id;
	VentureEntity *contact;
	if (VENTURE_IS_CONTACT(subject))
		return g_object_ref(subject);
	if (!VENTURE_IS_DEAL(subject) && !VENTURE_IS_TICKET(subject))
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Calls are made from a contact, a deal or a ticket");
		return NULL;
	}
	id = number(subject, "contact-id");
	contact = id > 0 ? venture_database_get(self->database, VENTURE_TYPE_CONTACT, id, NULL) : NULL;
	if (contact == NULL || venture_entity_get_organization_id(contact) != venture_entity_get_organization_id(subject))
	{
		g_clear_object(&contact);
		refuse(error, VENTURE_ERROR_VALIDATION, "This record has no contact to call; set its contact first");
	}
	return contact;
}

gchar *
venture_telephony_service_click_to_call(VentureTelephonyService *self, VentureEntity *subject,
	gint64 user_id, const gchar *number_text, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) contact = NULL, line = NULL;
	g_autoptr(VentureTelephonyConnector) connector = NULL;
	g_autoptr(VentureTelephonyLink) link = NULL;
	g_autoptr(GError) failure = NULL;
	g_autofree gchar *extension = NULL, *raw = NULL, *destination = NULL, *call = NULL, *name = NULL;
	gint64 org;
	(void)actor;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(subject), NULL);
	org = venture_entity_get_organization_id(subject);
	line = venture_telephony_service_line(self, org, VENTURE_TELEPHONY_CAPABILITY_CLICK_TO_CALL, error);
	if (line == NULL)
		return NULL;
	extension = venture_telephony_service_extension(self, org, user_id);
	if (venture_string_is_empty(extension))
	{
		refuse(error, VENTURE_ERROR_CONFIG, "Click-to-call rings your own phone first, and you have no extension in this "
			"organization yet. Ask an administrator to add yours under Telephony settings.");
		return NULL;
	}
	contact = subject_contact(self, subject, error);
	if (contact == NULL)
		return NULL;
	name = display_name(contact);
	raw = !venture_string_is_empty(number_text) ? g_strdup(number_text) : string(contact, "phone");
	if (venture_string_is_empty(raw))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s has no phone number to call", name);
		return NULL;
	}
	destination = normalized(self, org, raw);
	if (destination == NULL)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s's phone number is not one Venture can dial; write it as +15550100123", name);
		return NULL;
	}
	connector = connector_for(line, error);
	if (connector == NULL)
		return NULL;
	link = open_link(self, line, connector, error);
	if (link == NULL)
		return NULL;
	if (!venture_telephony_connector_click_to_call(connector, link, extension, destination, &call, &failure))
	{
		note_error(self, line, failure);
		g_propagate_error(error, g_steal_pointer(&failure));
		return NULL;
	}
	g_message("Click-to-call placed on line %" G_GINT64_FORMAT " from extension %s", venture_entity_get_id(line), extension);
	return g_steal_pointer(&call);
}

/* --- Matching numbers -------------------------------------------------------------- */

VentureEntity *
venture_telephony_service_match_contact(VentureTelephonyService *self, gint64 organization_id,
	const gchar *number_text, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *wanted = NULL, *pattern = NULL;
	gsize length;
	guint i;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), NULL);
	wanted = normalized(self, organization_id, number_text);
	if (wanted == NULL)
		return NULL;
	/* People write numbers every way; the last four digits are written
	 * together in all of them, which narrows the rows normalised here. */
	length = strlen(wanted);
	pattern = g_strdup_printf("%%%s%%", wanted + length - 4);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 500);
	venture_query_add_filter_string(query, "phone", VENTURE_FILTER_OP_LIKE, pattern, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *phone = string(g_ptr_array_index(rows, i), "phone");
		g_autofree gchar *candidate = normalized(self, organization_id, phone);
		if (g_strcmp0(candidate, wanted) == 0)
			return g_object_ref(g_ptr_array_index(rows, i));
	}
	return NULL;
}

/* Someone nobody knows called or texted: a contact, so the call or the
 * conversation has somewhere to live and the inbox someone to show. */
static VentureEntity *
match_or_create(VentureTelephonyService *self, gint64 org, const gchar *raw, GError **error)
{
	g_autoptr(VentureEntity) contact = venture_telephony_service_match_contact(self, org, raw, error);
	g_autofree gchar *phone = NULL, *name = NULL;
	if (contact != NULL || (error != NULL && *error != NULL))
		return g_steal_pointer(&contact);
	phone = normalized(self, org, raw);
	if (phone == NULL)
		return NULL;
	name = g_strdup_printf("Unknown caller %s", phone);
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", org, "name", name, "phone", phone,
		"source", "Phone", NULL);
	if (!venture_database_save(self->database, contact, system_actor(), error))
		return NULL;
	return g_steal_pointer(&contact);
}

/* --- Calls -------------------------------------------------------------------------- */

static gboolean
stable_https(const gchar *url)
{
	g_autoptr(GUri) uri = url != NULL ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
	return uri != NULL && g_strcmp0(g_uri_get_scheme(uri), "https") == 0 && g_uri_get_host(uri) != NULL &&
		g_uri_get_userinfo(uri) == NULL && g_uri_get_query(uri) == NULL && g_uri_get_fragment(uri) == NULL;
}

static gboolean
call_logged(VentureTelephonyService *self, gint64 org, const gchar *source, const gchar *external, gboolean *found, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_string(query, "call-external-source", VENTURE_FILTER_OP_EQ, source, NULL);
	venture_query_add_filter_string(query, "call-external-id", VENTURE_FILTER_OP_EQ, external, NULL);
	row = venture_database_find_one(self->database, query, error);
	*found = row != NULL;
	return error == NULL || *error == NULL;
}

/* Returns 1 for a call logged, 0 for a replay or a call with nobody to
 * log it on, -1 on failure. */
static gint
record_call(VentureTelephonyService *self, VentureEntity *account, const VentureTelephonyEvent *event,
	GDateTime *now, gboolean *duplicate, GError **error)
{
	g_autoptr(VentureEntity) contact = NULL, org_row = NULL, logged = NULL;
	g_autoptr(JsonObject) details = json_object_new();
	g_autoptr(GDateTime) real_now = venture_time_now();
	g_autofree gchar *provider = string(account, "provider"), *source = NULL, *external = NULL, *owner = NULL, *when = NULL;
	const gchar *party = event->inbound ? event->from : event->to;
	gint64 org = venture_entity_get_organization_id(account);
	gboolean found = FALSE, missed = event->inbound && !event->answered;
	gint outcome;
	GDateTime *occurred;
	*duplicate = FALSE;
	source = g_strdup_printf("%s-v1", provider);
	external = g_strdup_printf("%s:%s", venture_entity_get_uuid(account), event->id);
	if (!call_logged(self, org, source, external, &found, error))
		return -1;
	if (found)
	{
		*duplicate = TRUE;
		return 0;
	}
	contact = event->inbound ? match_or_create(self, org, party, error) : venture_telephony_service_match_contact(self, org, party, error);
	if (contact == NULL)
		return (error != NULL && *error != NULL) ? -1 : 0;
	outcome = event->answered ? VENTURE_CALL_OUTCOME_REACHED : event->voicemail ? VENTURE_CALL_OUTCOME_VOICEMAIL :
		event->inbound ? VENTURE_CALL_OUTCOME_MISSED : VENTURE_CALL_OUTCOME_NO_ANSWER;
	owner = extension_owner(self, org, event->extension);
	/* log_call refuses a future time; a phone system's clock may run ahead. */
	occurred = event->occurred_at != NULL && g_date_time_compare(event->occurred_at, real_now) <= 0 ? event->occurred_at
		: (g_date_time_compare(now, real_now) <= 0 ? now : real_now);
	when = venture_time_to_string(occurred);
	json_object_set_string_member(details, "subject", missed ? "Missed call" : event->inbound ? "Inbound call" : "Outbound call");
	json_object_set_string_member(details, "owner", owner != NULL ? owner : "Unassigned");
	json_object_set_string_member(details, "call_direction", event->inbound ? "inbound" : "outbound");
	json_object_set_int_member(details, "call_duration", CLAMP(event->duration, 0, 86400));
	json_object_set_string_member(details, "call_occurred_at", when);
	json_object_set_string_member(details, "call_outcome",
		outcome == VENTURE_CALL_OUTCOME_REACHED ? "reached" : outcome == VENTURE_CALL_OUTCOME_VOICEMAIL ? "voicemail" :
		outcome == VENTURE_CALL_OUTCOME_MISSED ? "missed" : "no_answer");
	json_object_set_string_member(details, "call_external_source", source);
	json_object_set_string_member(details, "call_external_id", external);
	json_object_set_boolean_member(details, "call_inbox_ready", missed);
	org_row = organization(self, org);
	/* Recording links only when the organization turned them on (the
	 * recording note in docs/telephony.org), and only a stable address. */
	if (org_row != NULL && flag(org_row, "telephony-recording-links") && stable_https(event->recording_url))
		json_object_set_string_member(details, "call_recording_url", event->recording_url);
	/* A transcript is the recording in words, so it is kept under the
	 * same switch; log_call bounds its length. */
	if (org_row != NULL && flag(org_row, "telephony-recording-links") && !venture_string_is_empty(event->transcript) &&
		g_utf8_validate(event->transcript, -1, NULL))
	{
		g_autofree gchar *bounded = g_utf8_substring(event->transcript, 0, MIN(g_utf8_strlen(event->transcript, -1), 65536));
		json_object_set_string_member(details, "call_transcript", bounded);
	}
	logged = venture_activity_service_log_call(venture_database_get_activity_service(self->database), contact,
		details, system_actor(), error);
	return logged != NULL ? 1 : -1;
}

/* --- Consent and quiet hours ----------------------------------------------------------- */

gboolean
venture_telephony_service_has_consent(VentureTelephonyService *self, VentureEntity *contact, GError **error)
{
	g_autofree gchar *phone = NULL, *raw = NULL;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), FALSE);
	raw = string(contact, "phone");
	phone = normalized(self, venture_entity_get_organization_id(contact), raw);
	if (phone == NULL)
		return FALSE;
	return venture_marketing_service_text_permitted(venture_marketing_service_get(self->database),
		venture_entity_get_organization_id(contact), phone, NULL, error);
}

static gboolean
consent_change(VentureTelephonyService *self, VentureEntity *contact, gboolean granted, const gchar *source,
	const gchar *evidence, const gchar *key, GDateTime *at, const VentureActor *actor, GError **error)
{
	g_autofree gchar *raw = string(contact, "phone");
	g_autofree gchar *phone = normalized(self, venture_entity_get_organization_id(contact), raw);
	g_autofree gchar *fresh = NULL, *name = display_name(contact);
	g_autoptr(VentureEntity) row = NULL;
	if (phone == NULL)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s has no phone number for text consent", name);
		return FALSE;
	}
	if (key == NULL)
	{
		fresh = g_strdup_printf("text-consent:%s", g_uuid_string_random());
		key = fresh;
	}
	row = venture_marketing_service_text_consent(venture_marketing_service_get(self->database),
		venture_entity_get_organization_id(contact), contact, phone, granted, source, evidence, key, at, actor, error);
	return row != NULL;
}

gboolean
venture_telephony_service_record_consent(VentureTelephonyService *self, VentureEntity *contact, gboolean granted,
	const gchar *evidence, const gchar *key, GDateTime *at, const VentureActor *actor, GError **error)
{
	g_autofree gchar *source = NULL;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_CONTACT(contact), FALSE);
	source = g_strdup_printf("Recorded by %s", actor != NULL && actor->name != NULL ? actor->name : "an operator");
	return consent_change(self, contact, granted, source, evidence, key, at, actor, error);
}

static GTimeZone *
recipient_zone(VentureTelephonyService *self, VentureEntity *contact)
{
	g_autofree gchar *zone = string(contact, "timezone");
	GTimeZone *tz = NULL;
	if (venture_string_is_empty(zone))
	{
		g_autoptr(VentureEntity) org = organization(self, venture_entity_get_organization_id(contact));
		g_free(zone);
		zone = org != NULL ? string(org, "timezone") : NULL;
	}
	if (!venture_string_is_empty(zone))
		tz = g_time_zone_new_identifier(zone);
	return tz != NULL ? tz : g_time_zone_new_utc();
}

static gboolean
parse_clock(const gchar *text, gint *minutes)
{
	gint hour, minute;
	gchar extra;
	if (text == NULL || sscanf(text, "%d:%d%c", &hour, &minute, &extra) != 2 || hour < 0 || hour > 23 || minute < 0 || minute > 59)
		return FALSE;
	*minutes = hour * 60 + minute;
	return TRUE;
}

GDateTime *
venture_telephony_service_quiet_until(VentureTelephonyService *self, VentureEntity *contact, GDateTime *now, GError **error)
{
	g_autoptr(VentureEntity) org = NULL;
	g_autoptr(GTimeZone) zone = NULL;
	g_autoptr(GDateTime) local = NULL, day = NULL;
	g_autofree gchar *setting = NULL;
	g_auto(GStrv) parts = NULL;
	gint start, end, minute;
	gboolean quiet;
	(void)error;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), NULL);
	org = organization(self, venture_entity_get_organization_id(contact));
	setting = org != NULL ? string(org, "telephony-quiet-hours") : NULL;
	if (setting != NULL && g_ascii_strcasecmp(g_strstrip(setting), "off") == 0)
		return NULL;
	parts = g_strsplit(venture_string_is_empty(setting) ? VENTURE_TELEPHONY_QUIET_HOURS : setting, "-", 2);
	if (g_strv_length(parts) != 2 || !parse_clock(g_strstrip(parts[0]), &start) || !parse_clock(g_strstrip(parts[1]), &end))
	{
		/* An unreadable setting is the default, never "no quiet hours". */
		g_strfreev(parts);
		parts = g_strsplit(VENTURE_TELEPHONY_QUIET_HOURS, "-", 2);
		parse_clock(parts[0], &start);
		parse_clock(parts[1], &end);
	}
	if (start == end)
		return NULL;
	zone = recipient_zone(self, contact);
	local = g_date_time_to_timezone(now, zone);
	minute = g_date_time_get_hour(local) * 60 + g_date_time_get_minute(local);
	quiet = start > end ? (minute >= start || minute < end) : (minute >= start && minute < end);
	if (!quiet)
		return NULL;
	day = g_date_time_new(zone, g_date_time_get_year(local), g_date_time_get_month(local), g_date_time_get_day_of_month(local),
		end / 60, end % 60, 0);
	if (start > end && minute >= start)
	{
		GDateTime *tomorrow = g_date_time_add_days(day, 1);
		g_date_time_unref(day);
		day = tomorrow;
	}
	return g_steal_pointer(&day);
}

/* --- Texts: queue, then the one send path ------------------------------------------------ */

static VentureEntity *
find_text(VentureTelephonyService *self, gint64 org, const gchar *field, const gchar *value, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TEXT_MESSAGE);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, NULL);
	return venture_database_find_one(self->database, query, error);
}

static VentureEntity *
queue(VentureTelephonyService *self, VentureEntity *contact, const gchar *body, const gchar *const *media,
	const gchar *origin, gboolean automatic, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) line = NULL, message = NULL;
	g_autoptr(GDateTime) until = NULL;
	g_autofree gchar *raw = NULL, *to = NULL, *from = NULL, *key = NULL, *name = NULL, *pictures = NULL;
	gint64 org;
	gboolean keyed;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_CONTACT(contact), NULL);
	org = venture_entity_get_organization_id(contact);
	name = display_name(contact);
	if (venture_string_is_empty(body) || !g_utf8_validate(body, -1, NULL) || g_utf8_strlen(body, -1) > VENTURE_TELEPHONY_TEXT_MAX)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A text needs words, at most %d characters", VENTURE_TELEPHONY_TEXT_MAX);
		return NULL;
	}
	line = venture_telephony_service_line(self, org, VENTURE_TELEPHONY_CAPABILITY_SMS |
		(media != NULL && media[0] != NULL ? VENTURE_TELEPHONY_CAPABILITY_MMS : 0), error);
	if (line == NULL)
		return NULL;
	raw = string(contact, "phone");
	to = normalized(self, org, raw);
	if (to == NULL)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s has no phone number Venture can text", name);
		return NULL;
	}
	if (!automatic)
	{
		g_autoptr(GError) local = NULL;
		if (!venture_telephony_service_has_consent(self, contact, &local))
		{
			if (local != NULL)
				g_propagate_error(error, g_steal_pointer(&local));
			else
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
					"%s has not agreed to receive texts. Record their agreement first; a STOP is honoured until they text START.", name);
			return NULL;
		}
	}
	/* A sequence step or a campaign recipient texts once, however often
	 * its sweep runs; a person's send is new every time. */
	keyed = g_str_has_prefix(origin, "sequence:") || g_str_has_prefix(origin, "campaign:");
	if (keyed)
	{
		key = g_strdup_printf("out:%s", origin);
		message = find_text(self, org, "message-key", key, error);
		if (message != NULL || (error != NULL && *error != NULL))
			return g_steal_pointer(&message);
	}
	else
		key = g_strdup_printf("out:%s", g_uuid_string_random());
	if (!automatic)
		until = venture_telephony_service_quiet_until(self, contact, now, error);
	from = string(line, "phone-number");
	if (media != NULL && media[0] != NULL)
		pictures = g_strjoinv("\n", (gchar **)media);
	message = g_object_new(VENTURE_TYPE_TEXT_MESSAGE, "organization-id", org,
		"account-id", venture_entity_get_id(line), "contact-id", venture_entity_get_id(contact),
		"direction", VENTURE_TEXT_DIRECTION_OUTBOUND, "from-number", from, "to-number", to,
		"body", body, "media", pictures, "state", until != NULL ? "held" : "queued", "hold-until", until,
		"message-key", key, "origin", origin, "auto-reply", automatic,
		"error", until != NULL ? "Held for quiet hours in the recipient's time zone" : "", NULL);
	if (!save(self, message, actor, error))
		return NULL;
	return g_steal_pointer(&message);
}

VentureEntity *
venture_telephony_service_queue_text(VentureTelephonyService *self, VentureEntity *contact, const gchar *body,
	const gchar *const *media, const gchar *origin, GDateTime *now, const VentureActor *actor, GError **error)
{
	return queue(self, contact, body, media, origin != NULL ? origin : "manual", FALSE, now, actor, error);
}

static void
answered(VentureTelephonyService *self, VentureEntity *message)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TEXT_MESSAGE);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, venture_entity_get_organization_id(message));
	venture_query_set_limit(query, 100);
	venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, number(message, "contact-id"), NULL);
	venture_query_add_filter_string(query, "inbox-ready", VENTURE_FILTER_OP_EQ, "true", NULL);
	rows = venture_database_find(self->database, query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		g_object_set(g_ptr_array_index(rows, i), "inbox-ready", FALSE, NULL);
		save(self, g_ptr_array_index(rows, i), NULL, NULL);
	}
}

/* The send path. Returns 1 when a provider was asked, 0 when the message
 * is not due (or was stopped before a provider was asked), -1 on failure. */
static gint
deliver_one(VentureTelephonyService *self, gint64 id, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) message = NULL, contact = NULL, line = NULL;
	g_autoptr(VentureTelephonyConnector) connector = NULL;
	g_autoptr(VentureTelephonyLink) link = NULL;
	g_autoptr(GError) failure = NULL;
	g_autoptr(GDateTime) hold = NULL, until = NULL;
	g_autofree gchar *state = NULL, *from = NULL, *to = NULL, *body = NULL, *media = NULL, *provider_id = NULL;
	g_auto(GStrv) pictures = NULL;
	VentureTelephonyOutbound outbound;
	gboolean automatic;
	message = venture_database_get(self->database, VENTURE_TYPE_TEXT_MESSAGE, id, error);
	if (message == NULL)
		return -1;
	state = string(message, "state");
	g_object_get(message, "hold-until", &hold, NULL);
	if (g_strcmp0(state, "queued") != 0 && g_strcmp0(state, "held") != 0)
		return 0;
	if (hold != NULL && g_date_time_compare(hold, now) > 0)
		return 0;
	automatic = flag(message, "auto-reply");
	contact = venture_database_get(self->database, VENTURE_TYPE_CONTACT, number(message, "contact-id"), error);
	if (contact == NULL)
		return -1;
	/* Again, now: consent may have been withdrawn and the clock may have
	 * reached quiet hours since this was queued. */
	if (!automatic)
	{
		g_autoptr(GError) local = NULL;
		if (!venture_telephony_service_has_consent(self, contact, &local))
		{
			if (local != NULL) { g_propagate_error(error, g_steal_pointer(&local)); return -1; }
			g_object_set(message, "state", "failed", "error", "Not sent: they have not agreed to texts, or texted STOP", NULL);
			return save(self, message, actor, error) ? 0 : -1;
		}
		until = venture_telephony_service_quiet_until(self, contact, now, NULL);
		if (until != NULL)
		{
			g_object_set(message, "state", "held", "hold-until", until, "error", "Held for quiet hours in the recipient's time zone", NULL);
			return save(self, message, actor, error) ? 0 : -1;
		}
	}
	line = venture_database_get(self->database, VENTURE_TYPE_TELEPHONY_ACCOUNT, number(message, "account-id"), error);
	if (line == NULL)
		return -1;
	connector = connector_for(line, &failure);
	if (connector != NULL)
		link = open_link(self, line, connector, &failure);
	from = string(message, "from-number");
	to = string(message, "to-number");
	body = string(message, "body");
	media = string(message, "media");
	pictures = !venture_string_is_empty(media) ? g_strsplit(media, "\n", -1) : NULL;
	outbound.from = from;
	outbound.to = to;
	outbound.body = body;
	outbound.media = (const gchar *const *)pictures;
	/* Marked before the provider is asked: a crash after the provider
	 * accepted must never send it again. */
	if (link != NULL)
	{
		g_object_set(message, "state", "sending", NULL);
		if (!save(self, message, actor, error))
			return -1;
	}
	if (link != NULL && venture_telephony_connector_send_message(connector, link, &outbound, &provider_id, &failure))
	{
		g_object_set(message, "state", "sent", "sent-at", now, "provider-message-id", provider_id, "error", "",
			"hold-until", NULL, NULL);
		if (!save(self, message, actor, error))
			return -1;
		if (!automatic)
			answered(self, message);
		return 1;
	}
	g_object_set(message, "state", "failed", "error", failure != NULL ? failure->message : "The provider refused the text", NULL);
	if (!save(self, message, actor, error))
		return -1;
	return link != NULL ? 1 : 0;
}

VentureEntity *
venture_telephony_service_send_text(VentureTelephonyService *self, VentureEntity *contact, const gchar *body,
	gint64 hosted_image_id, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) message = NULL;
	g_autofree gchar *url = NULL;
	const gchar *media[2] = { NULL, NULL };
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), NULL);
	if (hosted_image_id > 0)
	{
		g_autoptr(VentureContext) context = g_weak_ref_get(&self->context);
		g_autoptr(VentureEntity) image = venture_database_get(self->database, VENTURE_TYPE_HOSTED_IMAGE, hosted_image_id, NULL);
		if (context == NULL || image == NULL || venture_entity_is_deleted(image) ||
			venture_entity_get_organization_id(image) != venture_entity_get_organization_id(contact))
		{
			refuse(error, VENTURE_ERROR_NOT_FOUND, "That picture is not one of this organization's hosted images");
			return NULL;
		}
		/* Pictures go out by their public hosted address: the provider
		 * fetches them, and nothing private is exposed to it. */
		url = venture_hosted_image_url(context, image);
		media[0] = url;
	}
	message = queue(self, contact, body, media[0] != NULL ? media : NULL, "manual", FALSE, now, actor, error);
	if (message == NULL)
		return NULL;
	if (deliver_one(self, venture_entity_get_id(message), now, actor, error) < 0)
		return NULL;
	return venture_database_get(self->database, VENTURE_TYPE_TEXT_MESSAGE, venture_entity_get_id(message), error);
}

gint
venture_telephony_service_deliver_due(VentureTelephonyService *self, gint64 organization_id, GDateTime *now,
	guint limit, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TEXT_MESSAGE);
	g_autoptr(GPtrArray) rows = NULL, states = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *cutoff = NULL;
	guint i;
	gint handed = 0;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), -1);
	if (self->database == NULL || !module_on())
	{
		refuse(error, VENTURE_ERROR_CONFIG, "Phone and text is switched off");
		return -1;
	}
	if (limit < 1 || limit > 100 || now == NULL)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Delivery needs a clock and a limit of 1 to 100");
		return -1;
	}
	g_ptr_array_add(states, g_strdup("queued"));
	g_ptr_array_add(states, g_strdup("held"));
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 500);
	venture_query_add_filter_int(query, "direction", VENTURE_FILTER_OP_EQ, VENTURE_TEXT_DIRECTION_OUTBOUND, NULL);
	venture_query_add_filter(query, "state", VENTURE_FILTER_OP_IN, states, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return -1;
	(void)cutoff;
	for (i = 0; i < rows->len && (guint)handed < limit; i++)
	{
		gint result = deliver_one(self, venture_entity_get_id(g_ptr_array_index(rows, i)), now, actor, error);
		if (result < 0)
			return -1;
		handed += result;
	}
	return handed;
}

GPtrArray *
venture_telephony_service_thread(VentureTelephonyService *self, VentureEntity *contact, guint limit, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TEXT_MESSAGE);
	GPtrArray *rows;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), NULL);
	venture_query_set_organization(query, venture_entity_get_organization_id(contact));
	venture_query_set_limit(query, limit > 0 ? limit : 50);
	venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(contact), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	if (rows != NULL)
	{
		guint i;
		for (i = 0; i < rows->len / 2; i++)
		{
			gpointer swap = rows->pdata[i];
			rows->pdata[i] = rows->pdata[rows->len - 1 - i];
			rows->pdata[rows->len - 1 - i] = swap;
		}
	}
	return rows;
}

/* --- Inbound texts and keywords ------------------------------------------------------------- */

typedef enum { KEYWORD_NONE, KEYWORD_STOP, KEYWORD_START, KEYWORD_HELP } Keyword;

/* The usual carrier keywords, whole message only, any capitals, ignoring
 * spaces and punctuation around them. */
static Keyword
keyword_of(const gchar *body)
{
	static const struct { const gchar *word; Keyword keyword; } words[] = {
		{ "STOP", KEYWORD_STOP }, { "STOPALL", KEYWORD_STOP }, { "UNSUBSCRIBE", KEYWORD_STOP },
		{ "CANCEL", KEYWORD_STOP }, { "END", KEYWORD_STOP }, { "QUIT", KEYWORD_STOP },
		{ "START", KEYWORD_START }, { "UNSTOP", KEYWORD_START },
		{ "HELP", KEYWORD_HELP }, { "INFO", KEYWORD_HELP }
	};
	g_autoptr(GString) word = g_string_new(NULL);
	const gchar *p;
	gsize i;
	for (p = body != NULL ? body : ""; *p != '\0'; p++)
		if (g_ascii_isalpha(*p)) g_string_append_c(word, g_ascii_toupper(*p));
		else if (!(g_ascii_isspace(*p) || g_ascii_ispunct(*p))) return KEYWORD_NONE;
		else if (word->len > 0 && g_ascii_isalpha(p[1])) return KEYWORD_NONE;
	for (i = 0; i < G_N_ELEMENTS(words); i++)
		if (g_str_equal(word->str, words[i].word))
			return words[i].keyword;
	return KEYWORD_NONE;
}

static gchar *
org_name(VentureTelephonyService *self, gint64 org)
{
	g_autoptr(VentureEntity) row = organization(self, org);
	g_autofree gchar *name = row != NULL ? string(row, "name") : NULL;
	return g_strdup(venture_string_is_empty(name) ? "We" : name);
}

static gboolean
recent_help(VentureTelephonyService *self, VentureEntity *contact, GDateTime *now)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TEXT_MESSAGE);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) since = g_date_time_add_days(now, -1);
	guint i;
	venture_query_set_organization(query, venture_entity_get_organization_id(contact));
	venture_query_set_limit(query, 50);
	venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(contact), NULL);
	venture_query_add_filter_string(query, "origin", VENTURE_FILTER_OP_EQ, "keyword:help", NULL);
	rows = venture_database_find(self->database, query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		g_autoptr(GDateTime) sent = NULL;
		g_object_get(g_ptr_array_index(rows, i), "sent-at", &sent, NULL);
		if (sent != NULL && g_date_time_compare(sent, since) > 0)
			return TRUE;
	}
	return FALSE;
}

static gboolean
auto_reply(VentureTelephonyService *self, VentureEntity *contact, const gchar *body, const gchar *origin, GDateTime *now, GError **error)
{
	g_autoptr(VentureEntity) reply = queue(self, contact, body, NULL, origin, TRUE, now, NULL, error);
	if (reply == NULL)
		return FALSE;
	return deliver_one(self, venture_entity_get_id(reply), now, NULL, error) >= 0;
}

static gboolean
keyword_action(VentureTelephonyService *self, VentureEntity *contact, VentureEntity *message, Keyword keyword,
	GDateTime *now, GError **error)
{
	gint64 org = venture_entity_get_organization_id(contact);
	g_autofree gchar *name = org_name(self, org), *body = string(message, "body"), *evidence = NULL, *key = NULL, *reply = NULL;
	g_autofree gchar *day = g_date_time_format(now, "%Y-%m-%d %H:%M UTC");
	gboolean agreed = venture_telephony_service_has_consent(self, contact, NULL);
	evidence = g_strdup_printf("Texted \"%s\" on %s", g_strstrip(body), day);
	if (keyword == KEYWORD_STOP)
	{
		/* Already out: nothing changes and nobody is texted again. */
		if (!agreed)
			return TRUE;
		key = g_strdup_printf("text-keyword:%s", venture_entity_get_uuid(message));
		if (!consent_change(self, contact, FALSE, "Text keyword", evidence, key, now, system_actor(), error))
			return FALSE;
		reply = g_strdup_printf("%s: you are unsubscribed and will get no more texts from us. Reply START to resubscribe.", name);
		return auto_reply(self, contact, reply, "keyword:stop", now, error);
	}
	if (keyword == KEYWORD_START)
	{
		if (agreed)
			return TRUE;
		key = g_strdup_printf("text-keyword:%s", venture_entity_get_uuid(message));
		if (!consent_change(self, contact, TRUE, "Text keyword", evidence, key, now, system_actor(), error))
			return FALSE;
		reply = g_strdup_printf("%s: you will get our texts again. Reply STOP to opt out.", name);
		return auto_reply(self, contact, reply, "keyword:start", now, error);
	}
	if (keyword == KEYWORD_HELP)
	{
		if (recent_help(self, contact, now))
			return TRUE;
		reply = g_strdup_printf("%s: reply STOP to stop our texts or START to get them again. Message and data rates may apply.", name);
		return auto_reply(self, contact, reply, "keyword:help", now, error);
	}
	return TRUE;
}

static gchar *
attachment_root(VentureTelephonyService *self)
{
	g_autoptr(VentureContext) context = g_weak_ref_get(&self->context);
	return context != NULL ? g_build_filename(venture_config_get_state_dir(venture_context_get_config(context)), "attachments", NULL) : NULL;
}

/* An inbound picture, fetched from a host the line allows and kept as a
 * private document: it is the contact's, not something to publish. */
static gint64
store_picture(VentureTelephonyService *self, VentureEntity *account, const gchar *url, GError **error)
{
	g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	g_autoptr(VentureTelephonyLink) link = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(VentureEntity) document = NULL;
	g_autofree gchar *hosts = string(account, "media-hosts"), *origin = NULL, *root = NULL, *path = NULL, *token = NULL, *hash = NULL;
	g_auto(GStrv) allowed = g_strsplit_set(hosts != NULL ? hosts : "", "\n,", -1);
	const gchar *type, *ext = "bin", *host;
	guint status = 0, i;
	gboolean listed = FALSE;
	host = uri != NULL ? g_uri_get_host(uri) : NULL;
	for (i = 0; host != NULL && allowed[i] != NULL; i++)
		if (g_ascii_strcasecmp(g_strstrip(allowed[i]), host) == 0)
			listed = TRUE;
	if (!listed)
	{
		refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "The picture is on a host this line does not allow");
		return 0;
	}
	origin = g_uri_get_port(uri) > 0 ? g_strdup_printf("%s://%s:%d", g_uri_get_scheme(uri), host, g_uri_get_port(uri))
		: g_strdup_printf("%s://%s", g_uri_get_scheme(uri), host);
	link = venture_telephony_link_new(origin, NULL, NULL, allow_loopback(self), error);
	if (link == NULL)
		return 0;
	{
		const gchar *query = g_uri_get_query(uri);
		g_autofree gchar *target = query != NULL ? g_strdup_printf("%s?%s", g_uri_get_path(uri), query) : g_strdup(g_uri_get_path(uri));
		if (!venture_telephony_link_request(link, "GET", target, NULL, NULL, NULL, &status, &bytes, error) ||
			!venture_telephony_http_status_error(status, "The picture's host", error))
			return 0;
	}
	type = venture_hosted_image_sniff(bytes, NULL, NULL, &ext);
	if (type == NULL)
		type = "application/octet-stream";
	root = attachment_root(self);
	if (root == NULL || g_mkdir_with_parents(root, 0700) != 0)
	{
		refuse(error, VENTURE_ERROR_FAILED, "Cannot create the attachment store");
		return 0;
	}
	token = g_uuid_string_random();
	{
		g_autofree gchar *stored = g_strdup_printf("text-media-%s.%s", token, ext);
		path = g_build_filename(root, stored, NULL);
	}
	if (!g_file_set_contents_full(path, g_bytes_get_data(bytes, NULL), (gssize)g_bytes_get_size(bytes),
		G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
		return 0;
	hash = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
	document = VENTURE_ENTITY(venture_document_new());
	g_object_set(document, "title", "Picture received by text", "kind", "text-media", "mime-type", type, "path", path,
		"size-bytes", (gint64)g_bytes_get_size(bytes), "hash", hash, NULL);
	venture_entity_set_organization_id(document, venture_entity_get_organization_id(account));
	if (!venture_document_service_save_attachment(venture_document_service_get(self->database), document, root, system_actor(), error))
	{
		g_unlink(path);
		return 0;
	}
	return venture_entity_get_id(document);
}

static gint
inbound_text(VentureTelephonyService *self, VentureEntity *account, const VentureTelephonyEvent *event,
	GDateTime *now, gboolean *duplicate, GError **error)
{
	g_autoptr(VentureEntity) existing = NULL, contact = NULL, message = NULL;
	g_autoptr(GString) documents = g_string_new(NULL);
	g_autofree gchar *key = NULL, *from = NULL, *to = NULL, *line_number = string(account, "phone-number");
	gint64 org = venture_entity_get_organization_id(account);
	const gchar *problem = "";
	Keyword keyword;
	guint i;
	*duplicate = FALSE;
	key = g_strdup_printf("in:%s:%s", venture_entity_get_uuid(account), event->id);
	existing = find_text(self, org, "message-key", key, error);
	if (existing != NULL)
	{
		*duplicate = TRUE;
		return 0;
	}
	if (error != NULL && *error != NULL)
		return -1;
	contact = match_or_create(self, org, event->from, error);
	if (contact == NULL)
		return (error != NULL && *error != NULL) ? -1 : 0;
	from = normalized(self, org, event->from);
	to = event->to != NULL ? normalized(self, org, event->to) : NULL;
	for (i = 0; event->media != NULL && i < event->media->len; i++)
	{
		g_autoptr(GError) failed = NULL;
		gint64 id = store_picture(self, account, g_ptr_array_index(event->media, i), &failed);
		if (id > 0)
			g_string_append_printf(documents, "%s%" G_GINT64_FORMAT, documents->len > 0 ? "," : "", id);
		else
		{
			g_message("A picture on an inbound text was not kept: %s", failed != NULL ? failed->message : "unknown");
			problem = "A picture that came with it could not be kept";
		}
	}
	keyword = keyword_of(event->body);
	message = g_object_new(VENTURE_TYPE_TEXT_MESSAGE, "organization-id", org,
		"account-id", venture_entity_get_id(account), "contact-id", venture_entity_get_id(contact),
		"direction", VENTURE_TEXT_DIRECTION_INBOUND, "from-number", from != NULL ? from : event->from,
		"to-number", to != NULL ? to : line_number, "body", event->body != NULL ? event->body : "",
		"media-documents", documents->len > 0 ? documents->str : NULL, "state", "received",
		"received-at", now, "provider-message-id", event->id, "message-key", key, "origin", "inbound",
		"keyword", keyword == KEYWORD_STOP ? "stop" : keyword == KEYWORD_START ? "start" : keyword == KEYWORD_HELP ? "help" : NULL,
		"inbox-ready", keyword == KEYWORD_NONE, "error", problem, NULL);
	if (!save(self, message, system_actor(), error))
		return -1;
	if (keyword != KEYWORD_NONE && !keyword_action(self, contact, message, keyword, now, error))
		return -1;
	return 1;
}

static gint
status_update(VentureTelephonyService *self, VentureEntity *account, const VentureTelephonyEvent *event,
	GDateTime *now, gboolean *duplicate, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TEXT_MESSAGE);
	g_autoptr(VentureEntity) message = NULL;
	g_autofree gchar *state = NULL;
	gboolean delivered = g_strcmp0(event->status, "delivered") == 0;
	*duplicate = FALSE;
	if (!delivered && g_strcmp0(event->status, "failed") != 0)
		return 0;
	venture_query_set_organization(query, venture_entity_get_organization_id(account));
	venture_query_add_filter_int(query, "account-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(account), NULL);
	venture_query_add_filter_string(query, "provider-message-id", VENTURE_FILTER_OP_EQ, event->id, NULL);
	venture_query_add_filter_int(query, "direction", VENTURE_FILTER_OP_EQ, VENTURE_TEXT_DIRECTION_OUTBOUND, NULL);
	message = venture_database_find_one(self->database, query, error);
	if (message == NULL)
		return (error != NULL && *error != NULL) ? -1 : 0;
	state = string(message, "state");
	/* Delivered is final; a receipt only moves a sent text on. */
	if (g_strcmp0(state, "sent") != 0)
	{
		*duplicate = TRUE;
		return 0;
	}
	if (delivered)
		g_object_set(message, "state", "delivered", "delivered-at", now, NULL);
	else
		g_object_set(message, "state", "failed", "error", event->error != NULL ? event->error : "The carrier could not deliver it", NULL);
	return save(self, message, system_actor(), error) ? 1 : -1;
}

JsonNode *
venture_telephony_service_receive(VentureTelephonyService *self, const gchar *token, const VentureTelephonyWebhook *hook,
	GDateTime *now, gchar **reply, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TELEPHONY_ACCOUNT);
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureTelephonyConnector) connector = NULL;
	g_autoptr(VentureTelephonyLink) link = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *hash = NULL, *sources = NULL;
	gint64 counts[4] = { 0, 0, 0, 0 };
	guint i;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_SERVICE(self), NULL);
	g_return_val_if_fail(hook != NULL, NULL);
	if (self->database == NULL || !module_on() || token == NULL || strlen(token) < 32 || strlen(token) > 128)
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "No such webhook");
		return NULL;
	}
	/* The address token is the only thing that names the line; the
	 * request has no session, so it is read in the trusted scope. */
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	venture_query_add_filter_string(query, "webhook-token-hash", VENTURE_FILTER_OP_EQ, hash, NULL);
	account = venture_database_find_one(self->database, query, NULL);
	if (account == NULL || !flag(account, "active"))
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "No such webhook");
		return NULL;
	}
	if (hook->body != NULL && g_bytes_get_size(hook->body) > VENTURE_TELEPHONY_WEBHOOK_MAX_BODY)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "The webhook body is too large");
		return NULL;
	}
	sources = string(account, "webhook-sources");
	if (!source_allowed(sources, hook->remote_address))
	{
		g_message("Telephony webhook for line %" G_GINT64_FORMAT " refused: sender not allowed", venture_entity_get_id(account));
		refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "This sender is not allowed to call the webhook");
		return NULL;
	}
	connector = connector_for(account, error);
	if (connector == NULL)
		return NULL;
	link = open_link(self, account, connector, error);
	if (link == NULL)
		return NULL;
	if (!venture_telephony_connector_verify_webhook(connector, link, hook, error))
	{
		g_message("Telephony webhook for line %" G_GINT64_FORMAT " refused: signature", venture_entity_get_id(account));
		return NULL;
	}
	events = venture_telephony_connector_parse_webhook(connector, link, hook, error);
	if (events == NULL)
		return NULL;
	for (i = 0; i < events->len; i++)
	{
		VentureTelephonyEvent *event = g_ptr_array_index(events, i);
		gboolean duplicate = FALSE;
		gint result;
		if (venture_string_is_empty(event->id))
			continue;
		if (event->kind == VENTURE_TELEPHONY_EVENT_CALL)
			result = record_call(self, account, event, now, &duplicate, error);
		else if (event->kind == VENTURE_TELEPHONY_EVENT_MESSAGE)
			result = inbound_text(self, account, event, now, &duplicate, error);
		else
			result = status_update(self, account, event, now, &duplicate, error);
		if (result < 0)
			return NULL;
		counts[event->kind] += result;
		if (duplicate)
			counts[3]++;
	}
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "calls");
	json_builder_add_int_value(builder, counts[VENTURE_TELEPHONY_EVENT_CALL]);
	json_builder_set_member_name(builder, "messages");
	json_builder_add_int_value(builder, counts[VENTURE_TELEPHONY_EVENT_MESSAGE]);
	json_builder_set_member_name(builder, "statuses");
	json_builder_add_int_value(builder, counts[VENTURE_TELEPHONY_EVENT_STATUS]);
	json_builder_set_member_name(builder, "duplicates");
	json_builder_add_int_value(builder, counts[3]);
	json_builder_end_object(builder);
	if (reply != NULL)
		*reply = g_strdup(venture_telephony_connector_webhook_reply(connector));
	return json_builder_get_root(builder);
}

/* --- The text channel for sequences and campaigns --------------------------------------------- */

static gboolean
sequence_text(VentureDatabase *database, VentureEntity *delivery, VentureEntity *contact, VentureEntity *step,
	GDateTime *as_of, gpointer data, GError **error)
{
	VentureTelephonyService *self = venture_telephony_service_get(database);
	g_autoptr(VentureEntity) message = NULL;
	g_autoptr(GError) refused = NULL;
	g_autofree gchar *body = string(delivery, "body"), *key = string(delivery, "delivery-key"), *origin = NULL;
	(void)step; (void)data;
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG, "Phone and text is switched off, so this text step cannot send");
	origin = g_strdup_printf("sequence:%s", key != NULL ? key : g_uuid_string_random());
	message = venture_telephony_service_queue_text(self, contact, body, NULL, origin, as_of, NULL, &refused);
	if (message == NULL)
	{
		/* No consent is the person's choice, not a fault: the step is
		 * skipped with the reason and the journey goes on. */
		if (g_error_matches(refused, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED))
		{
			g_object_set(delivery, "state", 3, "error", refused->message, NULL);
			return TRUE;
		}
		g_propagate_error(error, g_steal_pointer(&refused));
		return FALSE;
	}
	g_object_set(delivery, "external-message-id", venture_entity_get_uuid(message), NULL);
	return TRUE;
}

static gboolean
campaign_recipient(VentureDatabase *database, VentureEntity *source, gchar **address, const gchar **reason,
	gpointer data, GError **error)
{
	VentureTelephonyService *self = venture_telephony_service_get(database);
	g_autoptr(VentureEntity) line = NULL;
	g_autofree gchar *raw = string(source, "phone");
	g_autofree gchar *phone = normalized(self, venture_entity_get_organization_id(source), raw);
	(void)data;
	if (phone == NULL) { *reason = "no_phone"; return FALSE; }
	if (!venture_telephony_service_has_consent(self, source, error)) { *reason = "no_text_permission"; return FALSE; }
	line = venture_telephony_service_line(self, venture_entity_get_organization_id(source), VENTURE_TELEPHONY_CAPABILITY_SMS, NULL);
	if (line == NULL) { *reason = "no_text_line"; return FALSE; }
	*address = g_steal_pointer(&phone);
	return TRUE;
}

static gboolean
campaign_queue(VentureDatabase *database, VentureEntity *recipient, VentureEntity *source, const gchar *body,
	GDateTime *now, const VentureActor *actor, gpointer data, GError **error)
{
	VentureTelephonyService *self = venture_telephony_service_get(database);
	g_autoptr(VentureEntity) message = NULL;
	g_autoptr(GError) refused = NULL;
	g_autofree gchar *origin = g_strdup_printf("campaign:%s", venture_entity_get_uuid(recipient));
	(void)data;
	if (!VENTURE_IS_CONTACT(source))
		return FALSE;
	message = venture_telephony_service_queue_text(self, source, body, NULL, origin, now, actor, &refused);
	if (message != NULL)
		return TRUE;
	if (refused != NULL && refused->domain == VENTURE_ERROR &&
		(refused->code == VENTURE_ERROR_PERMISSION_DENIED || refused->code == VENTURE_ERROR_VALIDATION || refused->code == VENTURE_ERROR_CONFIG))
		return FALSE;
	g_propagate_error(error, g_steal_pointer(&refused));
	return FALSE;
}

static const VentureMarketingTextChannel campaign_channel = { campaign_recipient, campaign_queue };

void
venture_telephony_install(VentureContext *context)
{
	VentureDatabase *database;
	VentureTelephonyService *self;
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	database = venture_context_get_database(context);
	self = venture_telephony_service_get(database);
	g_weak_ref_set(&self->context, context);
	if (self->installed)
		return;
	self->installed = TRUE;
	venture_database_add_save_validator(database, VENTURE_TYPE_TELEPHONY_ACCOUNT, validate, self, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_TELEPHONY_EXTENSION, validate, self, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_TEXT_MESSAGE, validate, self, NULL);
	/* Process-wide, like the hooks themselves: each finds its own
	 * database's service. Channel 5 is the sequence channel "text". */
	venture_sequence_channel_register(5, sequence_text, NULL);
	venture_marketing_text_channel_register(&campaign_channel, NULL);
}

gchar *
venture_telephony_service_keyword_reply(VentureTelephonyService *self, gint64 organization_id, const gchar *keyword)
{
	(void)self; (void)organization_id; (void)keyword;
	return NULL;
}
