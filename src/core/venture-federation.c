/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#define FED_LIMIT (1024 * 1024)
#define FED_CHALLENGES (1024)
#define FED_TTL (60 * G_USEC_PER_SEC)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_PKEY, EVP_PKEY_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(EVP_MD_CTX, EVP_MD_CTX_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(BIO, BIO_free)

/* Challenges live on the context, so a restart invalidates outstanding
 * messages rather than opening a replay window. No keys live in the database. */
static GHashTable *
fed_challenges(VentureContext *context)
{
	GHashTable *table;
	table = g_object_get_data(G_OBJECT(context), "federation-challenges");
	if (NULL == table)
	{
		table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_object_set_data_full(G_OBJECT(context), "federation-challenges", table,
			(GDestroyNotify)g_hash_table_unref);
	}
	return table;
}

static gboolean
fed_refuse(GError **error, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, message);
	return FALSE;
}

static gboolean
fed_origin_valid(const gchar *text)
{
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *canonical = NULL;
	const gchar *host;
	gint port;

	if (NULL == text || strlen(text) > 255)
		return FALSE;
	uri = g_uri_parse(text, G_URI_FLAGS_NONE, NULL);
	if (NULL == uri || g_strcmp0(g_uri_get_scheme(uri), "https") != 0 ||
		g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri) ||
		g_strcmp0(g_uri_get_path(uri), "") != 0)
		return FALSE;
	host = g_uri_get_host(uri);
	if (venture_string_is_empty(host) || strchr(host, ':') ||
		(!g_hostname_is_ascii_encoded(host) && !g_str_is_ascii(host)))
		return FALSE;
	/* DNS names have one spelling. No wildcard, suffix, path, userinfo,
	 * encoded authority or implicit default-port alias is accepted. */
	{
		const gchar *c;
		for (c = host; *c; c++)
			if (!(g_ascii_islower(*c) || g_ascii_isdigit(*c) || *c == '-' || *c == '.'))
				return FALSE;
	}
	if (g_str_has_suffix(host, "."))
		return FALSE;
	port = g_uri_get_port(uri);
	canonical = port == -1 ? g_strdup_printf("https://%s", host) :
		g_strdup_printf("https://%s:%d", host, port);
	return port != 443 && g_strcmp0(canonical, text) == 0;
}

static guchar *
fed_decode(const gchar *text, gsize expected)
{
	g_autofree guchar *bytes = NULL;
	g_autofree gchar *canonical = NULL;
	gsize size;
	if (NULL == text || strlen(text) != 4 * ((expected + 2) / 3))
		return NULL;
	bytes = g_base64_decode(text, &size);
	canonical = g_base64_encode(bytes, size);
	if (size != expected || g_strcmp0(text, canonical) != 0)
		return NULL;
	return g_steal_pointer(&bytes);
}

static EVP_PKEY *
fed_key(VentureContext *context, gchar **origin, gchar **public_key, GError **error)
{
	g_autofree gchar *mode = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(BIO) bio = NULL;
	g_autoptr(EVP_PKEY) key = NULL;
	struct stat st;
	guchar raw[32];
	size_t length = sizeof(raw);
	gint fd;
	gboolean enabled;

	g_object_get(venture_context_get_config(context), "federation-enabled", &enabled,
		"federation-mode", &mode, "federation-origin", origin,
		"federation-key-file", &path, NULL);
	if (!enabled || !venture_context_module_enabled(context, "federation") ||
		(g_strcmp0(mode, "allowlist") && g_strcmp0(mode, "global")) ||
		!fed_origin_valid(*origin) || venture_string_is_empty(path))
	{
		fed_refuse(error, "Federation is disabled or not configured correctly");
		return NULL;
	}
	/* Check the descriptor we actually read, not a path before reopening it.
	 * Refuse symlinks and group/other-readable private material. */
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
	{
		fed_refuse(error, "Cannot open federation identity key");
		return NULL;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
		(st.st_mode & 0077) || st.st_size > 4096)
	{
		close(fd);
		fed_refuse(error, "Federation key must be an owner-only regular file");
		return NULL;
	}
	bio = BIO_new_fd(fd, BIO_CLOSE);
	if (NULL == bio)
		close(fd);
	else
		key = PEM_read_bio_PrivateKey(bio, NULL, NULL, (void *)"");
	if (NULL == key || !EVP_PKEY_is_a(key, "ED25519") ||
		EVP_PKEY_get_raw_public_key(key, raw, &length) != 1 || length != 32)
	{
		fed_refuse(error, "Federation requires an unencrypted Ed25519 identity key");
		return NULL;
	}
	*public_key = g_base64_encode(raw, length);
	return g_steal_pointer(&key);
}

static JsonNode *
fed_object(void)
{
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, json_object_new());
	return node;
}

static VentureFieldSpec *
fed_field(GPtrArray *fields, const gchar *wire)
{
	guint i;
	for (i = 0; i < fields->len; i++)
	{
		VentureFieldSpec *field = g_ptr_array_index(fields, i);
		g_autofree gchar *name = g_strdup(field->name);
		g_strdelimit(name, "-", '_');
		if (g_strcmp0(name, wire) == 0)
			return field;
	}
	return NULL;
}

static gboolean
fed_contains(const gchar *csv, const gchar *name)
{
	g_auto(GStrv) words = g_strsplit(csv ? csv : "", ",", -1);
	return g_strv_contains((const gchar *const *)words, name);
}

static gboolean
fed_safe_field(VentureFieldSpec *field, gboolean write)
{
	static const gchar *const spine[] = {"id", "uuid", "organization-id",
		"created-at", "updated-at", "deleted-at", "version", NULL};
	return field && !g_strv_contains(spine, field->name) &&
		!(field->flags & (VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_TRANSIENT)) &&
		(!write || !(field->flags & VENTURE_COLUMN_FLAG_IMMUTABLE));
}

static gboolean
fed_validate(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	g_autofree gchar *origin = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *type = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *read_fields = NULL;
	g_autofree gchar *write_fields = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	GType gtype;
	gint64 peer;
	gint pass;
	(void)previous;
	(void)data;

	if (VENTURE_IS_FEDERATION_REPLICA(entity))
		return g_object_get_data(G_OBJECT(entity), "federation-replica-write") != NULL ||
			fed_refuse(error, "Replica state can only be written through the federation service");
	if (VENTURE_IS_FEDERATION_PEER(entity))
	{
		g_autofree guchar *raw = NULL;
		g_object_get(entity, "origin", &origin, "public-key", &key, NULL);
		raw = fed_decode(key, 32);
		return (fed_origin_valid(origin) && raw != NULL) ||
			fed_refuse(error, "A peer needs an exact HTTPS origin and a canonical Ed25519 public key");
	}
	g_object_get(entity, "record-type", &type, "record-uuid", &uuid,
		"fields", &read_fields, "write-fields", &write_fields, "peer-id", &peer, NULL);
	gtype = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), type);
	if (!venture_entity_type_get_federation_access(gtype) || !g_uuid_string_is_valid(uuid))
		return fed_refuse(error, "This record type or UUID cannot be federated");
	record = venture_database_get_by_uuid(database, gtype, uuid, NULL);
	if (!record || venture_entity_is_deleted(record) ||
		g_strcmp0(type, venture_entity_get_entity_name(record)))
		return fed_refuse(error, "Share an existing record using its canonical type and UUID");
	if (venture_string_is_empty(read_fields))
		return fed_refuse(error, "Select exact readable fields; wildcards are not permitted");
	if (!venture_string_is_empty(write_fields) &&
		(peer == 0 || venture_entity_type_get_federation_access(gtype) != 2))
		return fed_refuse(error, "Only a named peer may receive editable fields on a writable type");
	fields = venture_entity_get_field_specs(record);
	for (pass = 0; pass < 2; pass++)
	{
		g_auto(GStrv) names = NULL;
		guint i;
		const gchar *csv = pass ? write_fields : read_fields;
		if (venture_string_is_empty(csv))
			continue;
		if (strlen(csv) > 4096)
			return fed_refuse(error, "Too many grant fields");
		names = g_strsplit(csv, ",", -1);
		for (i = 0; names[i]; i++)
			if (!fed_safe_field(fed_field(fields, names[i]), pass != 0) ||
				(pass && !fed_contains(read_fields, names[i])))
				return fed_refuse(error, "Grant names an unavailable, sensitive, immutable or unreadable field");
	}
	return TRUE;
}

void
venture_federation_install_validators(VentureDatabase *database)
{
	venture_database_add_save_validator(database, VENTURE_TYPE_FEDERATION_REPLICA, fed_validate, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_FEDERATION_PEER, fed_validate, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_FEDERATION_GRANT, fed_validate, NULL, NULL);
}

/* Stateless issuance cannot exhaust the replay cache. Only authenticated
 * requests consume cache capacity, with half reserved for named peers. */
static GBytes *
fed_challenge_secret(VentureContext *context)
{
	GBytes *secret = g_object_get_data(G_OBJECT(context), "federation-challenge-secret");
	if (!secret)
	{
		guchar raw[32];
		if (RAND_bytes(raw, sizeof(raw)) != 1) return NULL;
		secret = g_bytes_new(raw, sizeof(raw));
		g_object_set_data_full(G_OBJECT(context), "federation-challenge-secret", secret, (GDestroyNotify)g_bytes_unref);
	}
	return secret;
}

static gchar *
fed_challenge_mac(VentureContext *context, const gchar *payload)
{
	GBytes *secret = fed_challenge_secret(context);
	gsize size;
	const guchar *key;
	if (!secret) return NULL;
	key = g_bytes_get_data(secret, &size);
	return g_compute_hmac_for_string(G_CHECKSUM_SHA256, key, size, payload, -1);
}

static gint64
fed_challenge_expiry(VentureContext *context, const gchar *challenge)
{
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *payload = NULL;
	g_autofree gchar *mac = NULL;
	gint64 expiry;
	gchar *end = NULL;
	guint difference = 0;
	guint i;
	if (!challenge || strlen(challenge) > 160) return 0;
	parts = g_strsplit(challenge, ":", -1);
	if (g_strv_length(parts) != 3 || strlen(parts[2]) != 64) return 0;
	expiry = g_ascii_strtoll(parts[0], &end, 10);
	if (!end || *end || expiry < g_get_monotonic_time() ||
		expiry - g_get_monotonic_time() > FED_TTL) return 0;
	payload = g_strconcat(parts[0], ":", parts[1], NULL);
	mac = fed_challenge_mac(context, payload);
	if (!mac) return 0;
	for (i = 0; i < 64; i++) difference |= (guint)(mac[i] ^ parts[2][i]);
	return difference ? 0 : expiry;
}

JsonNode *
venture_federation_identity(VentureContext *context, GError **error)
{
	g_autofree gchar *origin = NULL;
	g_autofree gchar *pub = NULL;
	g_autofree gchar *random = g_uuid_string_random();
	g_autofree gchar *payload = NULL;
	g_autofree gchar *mac = NULL;
	g_autofree gchar *challenge = NULL;
	g_autoptr(EVP_PKEY) key = fed_key(context, &origin, &pub, error);
	JsonNode *node;
	JsonObject *object;
	if (!key) return NULL;
	payload = g_strdup_printf("%" G_GINT64_FORMAT ":%s", g_get_monotonic_time() + FED_TTL, random);
	mac = fed_challenge_mac(context, payload);
	if (!mac)
	{
		fed_refuse(error, "Cannot create federation challenge");
		return NULL;
	}
	challenge = g_strconcat(payload, ":", mac, NULL);
	node = fed_object();
	object = json_node_get_object(node);
	json_object_set_int_member(object, "protocol", 1);
	json_object_set_string_member(object, "origin", origin);
	json_object_set_string_member(object, "public_key", pub);
	json_object_set_string_member(object, "challenge", challenge);
	return node;
}

GBytes *
venture_federation_sign(VentureContext *context, JsonNode *identity,
	JsonNode *operation, gchar **signature, GError **error)
{
	g_autofree gchar *origin = NULL;
	g_autofree gchar *pub = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(EVP_PKEY) key = NULL;
	g_autoptr(EVP_MD_CTX) md = NULL;
	g_autoptr(JsonNode) envelope = NULL;
	JsonObject *object;
	JsonObject *remote;
	guchar sig[64];
	size_t size = sizeof(sig);

	*signature = NULL;
	if (!identity || !JSON_NODE_HOLDS_OBJECT(identity) || !operation || !JSON_NODE_HOLDS_OBJECT(operation))
	{
		fed_refuse(error, "Invalid federation envelope");
		return NULL;
	}
	key = fed_key(context, &origin, &pub, error);
	if (!key)
		return NULL;
	remote = json_node_get_object(identity);
	if (venture_json_object_get_int(remote, "protocol", 0) != 1 ||
		!fed_origin_valid(venture_json_object_get_string(remote, "origin", "")))
	{
		fed_refuse(error, "Unsupported federation identity");
		return NULL;
	}
	envelope = fed_object();
	object = json_node_get_object(envelope);
	json_object_set_string_member(object, "purpose", "VENTURE federation request v1");
	json_object_set_string_member(object, "origin", origin);
	json_object_set_string_member(object, "public_key", pub);
	json_object_set_string_member(object, "audience", venture_json_object_get_string(remote, "origin", ""));
	json_object_set_string_member(object, "audience_key", venture_json_object_get_string(remote, "public_key", ""));
	json_object_set_string_member(object, "challenge", venture_json_object_get_string(remote, "challenge", ""));
	json_object_set_member(object, "operation", json_node_copy(operation));
	text = venture_json_to_string(envelope, FALSE);
	md = EVP_MD_CTX_new();
	if (strlen(text) > FED_LIMIT || !md || EVP_DigestSignInit(md, NULL, NULL, NULL, key) != 1 ||
		EVP_DigestSign(md, sig, &size, (const guchar *)text, strlen(text)) != 1)
	{
		fed_refuse(error, "Cannot sign federation request");
		return NULL;
	}
	*signature = g_base64_encode(sig, size);
	return g_bytes_new(text, strlen(text));
}

/* Grants are always read live, including while serializing references. No
 * traversal, org roll-up or possession of a UUID confers any authority. */
static GPtrArray *
fed_grants(VentureDatabase *database, gint64 peer, gboolean global, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FEDERATION_GRANT);
	g_autoptr(GPtrArray) all = NULL;
	GPtrArray *selected;
	guint i;
	venture_query_add_filter_int(query, "active", VENTURE_FILTER_OP_EQ, 1, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 10001);
	all = venture_database_find(database, query, error);
	if (!all)
		return NULL;
	if (all->len > 10000)
	{
		fed_refuse(error, "Federation grant limit exceeded");
		return NULL;
	}
	selected = g_ptr_array_new_with_free_func(g_object_unref);
	for (i = 0; i < all->len; i++)
	{
		VentureEntity *grant = g_ptr_array_index(all, i);
		gint64 target;
		g_object_get(grant, "peer-id", &target, NULL);
		if ((peer > 0 && peer == target) || (global && target == 0))
			g_ptr_array_add(selected, g_object_ref(grant));
	}
	return selected;
}

static gchar *
fed_permissions(GPtrArray *grants, const gchar *type, const gchar *uuid, gboolean write)
{
	g_autoptr(GString) csv = g_string_new("");
	guint i;
	for (i = 0; i < grants->len; i++)
	{
		g_autofree gchar *gt = NULL;
		g_autofree gchar *gu = NULL;
		g_autofree gchar *fields = NULL;
		gint64 peer;
		g_object_get(g_ptr_array_index(grants, i), "record-type", &gt, "record-uuid", &gu,
			write ? "write-fields" : "fields", &fields, "peer-id", &peer, NULL);
		if (!g_strcmp0(gt, type) && !g_strcmp0(gu, uuid) && !venture_string_is_empty(fields) && (!write || peer > 0))
		{
			if (csv->len)
				g_string_append_c(csv, ',');
			g_string_append(csv, fields);
		}
	}
	return g_strdup(csv->str);
}

static JsonNode *
fed_export(VentureDatabase *database, VentureEntity *record, GPtrArray *grants)
{
	g_autofree gchar *allowed = NULL;
	g_autofree gchar *writable = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(JsonNode) full = NULL;
	JsonNode *node;
	JsonObject *result;
	JsonObject *values;
	JsonObject *schema;
	JsonArray *writes;
	guint i;
	const gchar *type = venture_entity_get_entity_name(record);
	const gchar *uuid = venture_entity_get_uuid(record);

	if (venture_entity_is_deleted(record) || !venture_entity_type_get_federation_access(G_OBJECT_TYPE(record)))
		return NULL;
	allowed = fed_permissions(grants, type, uuid, FALSE);
	if (venture_string_is_empty(allowed))
		return NULL;
	writable = fed_permissions(grants, type, uuid, TRUE);
	fields = venture_entity_get_field_specs(record);
	full = venture_serializable_to_json(VENTURE_SERIALIZABLE(record), FALSE);
	node = fed_object();
	result = json_node_get_object(node);
	values = json_object_new();
	schema = json_object_new();
	writes = json_array_new();
	json_object_set_string_member(result, "type", type);
	json_object_set_string_member(result, "uuid", uuid);
	json_object_set_int_member(result, "version", venture_entity_get_version(record));
	json_object_set_object_member(result, "fields", values);
	json_object_set_object_member(result, "schema", schema);
	json_object_set_array_member(result, "writable", writes);
	for (i = 0; i < fields->len; i++)
	{
		VentureFieldSpec *field = g_ptr_array_index(fields, i);
		g_autofree gchar *wire = g_strdup(field->name);
		JsonNode *value;
		g_strdelimit(wire, "-", '_');
		if (!fed_safe_field(field, FALSE) || !fed_contains(allowed, wire))
			continue;
		value = json_object_get_member(json_node_get_object(full), wire);
		if (!value)
			continue;
		if (field->reference_type)
		{
			g_autoptr(VentureEntity) target = NULL;
			g_autofree gchar *target_permissions = NULL;
			GType target_type;
			gint64 id;
			id = json_node_get_int(value);
			target_type = venture_entity_registry_lookup(venture_entity_registry_get_default(), field->reference_type);
			if (id == 0)
			{
				json_object_set_null_member(values, wire);
				goto field_metadata;
			}
			if (!target_type)
				continue;
			target = venture_database_get(database, target_type, id, NULL);
			if (!target || venture_entity_is_deleted(target) || !venture_entity_type_get_federation_access(target_type))
				continue;
			target_permissions = fed_permissions(grants, field->reference_type, venture_entity_get_uuid(target), FALSE);
			if (venture_string_is_empty(target_permissions))
				continue;
			{
				JsonObject *ref = json_object_new();
				json_object_set_string_member(ref, "type", field->reference_type);
				json_object_set_string_member(ref, "uuid", venture_entity_get_uuid(target));
				json_object_set_object_member(values, wire, ref);
			}
		}
		else
		{
			/* URI credentials are not public merely because this is a normal
			 * string field. Omit them entirely so offline edits cannot later
			 * push a redaction placeholder over the source credential. */
			if (JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING)
			{
				const gchar *text = json_node_get_string(value);
				g_autofree gchar *redacted = venture_string_redact_uri(text);
				if (g_strcmp0(text, redacted)) continue;
			}
			json_object_set_member(values, wire, json_node_copy(value));
		}
field_metadata:
		json_object_set_member(schema, wire, venture_field_spec_to_json(field));
		if (venture_entity_type_get_federation_access(G_OBJECT_TYPE(record)) == 2 &&
			fed_safe_field(field, TRUE) && fed_contains(writable, wire))
			json_array_add_string_element(writes, wire);
	}
	return node;
}

static JsonNode *
fed_operation(VentureContext *context, JsonObject *operation, gint64 peer,
	gboolean global, const gchar *actor_name, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(GPtrArray) grants = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autofree gchar *allowed = NULL;
	g_autofree gchar *writable = NULL;
	const gchar *action = venture_json_object_get_string(operation, "action", "");
	const gchar *type = venture_json_object_get_string(operation, "type", "");
	const gchar *uuid = venture_json_object_get_string(operation, "uuid", "");
	GType gtype;
	JsonNode *result;

	grants = fed_grants(database, peer, global, error);
	if (!grants)
		return NULL;
	if (g_strcmp0(action, "list") == 0)
	{
		g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		JsonArray *array = json_array_new();
		guint i;
		gint64 offset = venture_json_object_get_int(operation, "offset", 0);
		gint64 matched = 0;
		gint64 limit = CLAMP(venture_json_object_get_int(operation, "limit", 50), 1, 50);
		for (i = 0; i < grants->len; i++)
		{
			g_autofree gchar *gt = NULL;
			g_autofree gchar *collection = NULL;
			g_autofree gchar *gu = NULL;
			g_autofree gchar *identity = NULL;
			g_autoptr(VentureEntity) item = NULL;
			g_autoptr(JsonNode) exported = NULL;
			g_object_get(g_ptr_array_index(grants, i), "record-type", &gt, "record-uuid", &gu, NULL);
			g_object_get(g_ptr_array_index(grants, i), "collection", &collection, NULL);
			if (json_object_has_member(operation, "collection") &&
				g_strcmp0(collection, venture_json_object_get_string(operation, "collection", "")))
				continue;
			identity = g_strdup_printf("%s/%s", gt, gu);
			if (g_hash_table_contains(seen, identity))
				continue;
			g_hash_table_add(seen, g_steal_pointer(&identity));
			gtype = venture_entity_registry_lookup(venture_entity_registry_get_default(), gt);
			if (!gtype)
				continue;
			item = venture_database_get_by_uuid(database, gtype, gu, NULL);
			if (!item || venture_entity_is_deleted(item))
				continue;
			exported = fed_export(database, item, grants);
			if (!exported || matched++ < offset)
				continue;
			json_array_add_element(array, g_steal_pointer(&exported));
			if (json_array_get_length(array) == (guint)limit)
				break;
		}
		result = fed_object();
		json_object_set_array_member(json_node_get_object(result), "records", array);
		json_object_set_int_member(json_node_get_object(result), "next_offset", offset + json_array_get_length(array));
		return result;
	}
	gtype = venture_entity_registry_lookup(venture_entity_registry_get_default(), type);
	allowed = fed_permissions(grants, type, uuid, FALSE);
	if (!gtype || !venture_entity_type_get_federation_access(gtype) || venture_string_is_empty(allowed))
		goto absent;
	record = venture_database_get_by_uuid(database, gtype, uuid, NULL);
	if (!record || venture_entity_is_deleted(record))
		goto absent;
	if (g_strcmp0(action, "get") == 0)
		return fed_export(database, record, grants);
	if (g_strcmp0(action, "update") == 0 && peer > 0 && venture_entity_type_get_federation_access(gtype) == 2)
	{
		JsonNode *changes = json_object_get_member(operation, "fields");
		g_autoptr(GList) names = NULL;
		GList *it;
		VentureActor actor;
		gint64 version = venture_json_object_get_int(operation, "version", 0);
		if (version != venture_entity_get_version(record))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Shared record changed; merge against the current version");
			return NULL;
		}
		if (!changes || !JSON_NODE_HOLDS_OBJECT(changes))
			goto absent;
		writable = fed_permissions(grants, type, uuid, TRUE);
		fields = venture_entity_get_field_specs(record);
		names = json_object_get_members(json_node_get_object(changes));
		for (it = names; it; it = it->next)
		{
			const gchar *name = it->data;
			VentureFieldSpec *field = fed_field(fields, name);
			JsonNode *value = json_object_get_member(json_node_get_object(changes), name);
			g_autofree gchar *text = NULL;
			g_autoptr(JsonNode) patch = fed_object();
			if (!fed_safe_field(field, TRUE) || !fed_contains(writable, name) || !fed_contains(allowed, name))
				goto absent;
			if (field->reference_type && !JSON_NODE_HOLDS_NULL(value))
			{
				g_autoptr(VentureEntity) target = NULL;
				g_autofree gchar *permission = NULL;
				const gchar *target_uuid;
				GType target_type;
				if (!JSON_NODE_HOLDS_OBJECT(value))
					goto absent;
				target_uuid = venture_json_object_get_string(json_node_get_object(value), "uuid", "");
				if (g_strcmp0(field->reference_type, venture_json_object_get_string(json_node_get_object(value), "type", "")))
					goto absent;
				target_type = venture_entity_registry_lookup(venture_entity_registry_get_default(), field->reference_type);
				permission = fed_permissions(grants, field->reference_type, target_uuid, FALSE);
				if (!target_type || venture_string_is_empty(permission))
					goto absent;
				target = venture_database_get_by_uuid(database, target_type, target_uuid, NULL);
				if (!target || venture_entity_is_deleted(target))
					goto absent;
				text = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(target));
			}
			if (field->reference_type)
				json_object_set_int_member(json_node_get_object(patch), name,
					text ? g_ascii_strtoll(text, NULL, 10) : 0);
			else
				json_object_set_member(json_node_get_object(patch), name, json_node_copy(value));
			if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(record), patch, error))
				return NULL;
		}
		/* The remote server vouches for its users. Never accept a supplied
		 * local username, approver, audit identity or organization. */
		actor.kind = VENTURE_ACTOR_KIND_USER;
		actor.name = actor_name;
		actor.prompt = NULL;
		actor.request_id = NULL;
		actor.approved_by = NULL;
		if (!venture_database_save(database, record, &actor, error))
			return NULL;
		return fed_export(database, record, grants);
	}
absent:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Shared record or operation unavailable");
	return NULL;
}

JsonNode *
venture_federation_receive(VentureContext *context, GBytes *body,
	const gchar *signature, GError **error)
{
	g_autofree gchar *origin = NULL;
	g_autofree gchar *public_key = NULL;
	g_autofree gchar *mode = NULL;
	g_autofree gchar *actor = NULL;
	g_autofree gchar *fingerprint = NULL;
	g_autofree guchar *raw = NULL;
	g_autofree guchar *sig = NULL;
	g_autoptr(EVP_PKEY) own_key = NULL;
	g_autoptr(EVP_PKEY) remote_key = NULL;
	g_autoptr(EVP_MD_CTX) md = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) peer = NULL;
	JsonObject *object;
	JsonNode *operation;
	const gchar *claimed_origin;
	const gchar *claimed_key;
	const gchar *challenge;
	const gchar *bytes;
	gsize length;
	gint64 expiry;
	gint64 peer_id = 0;
	gboolean global;

	own_key = fed_key(context, &origin, &public_key, error);
	if (!own_key)
		return NULL;
	if (!body)
		goto refused;
	bytes = g_bytes_get_data(body, &length);
	if (!length || length > FED_LIMIT || memchr(bytes, '\0', length))
		goto refused;
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, bytes, (gssize)length, NULL) ||
		!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		goto refused;
	object = json_node_get_object(json_parser_get_root(parser));
	claimed_origin = venture_json_object_get_string(object, "origin", "");
	claimed_key = venture_json_object_get_string(object, "public_key", "");
	challenge = venture_json_object_get_string(object, "challenge", "");
	operation = json_object_get_member(object, "operation");
	if (!fed_origin_valid(claimed_origin) ||
		g_strcmp0(venture_json_object_get_string(object, "purpose", ""), "VENTURE federation request v1") ||
		g_strcmp0(venture_json_object_get_string(object, "audience", ""), origin) ||
		g_strcmp0(venture_json_object_get_string(object, "audience_key", ""), public_key) ||
		!operation || !JSON_NODE_HOLDS_OBJECT(operation))
		goto refused;
	expiry = fed_challenge_expiry(context, challenge);
	if (!expiry || g_hash_table_contains(fed_challenges(context), challenge))
		goto refused;
	raw = fed_decode(claimed_key, 32);
	sig = fed_decode(signature, 64);
	if (!raw || !sig)
		goto refused;
	remote_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, raw, 32);
	md = EVP_MD_CTX_new();
	if (!remote_key || !md || EVP_DigestVerifyInit(md, NULL, NULL, NULL, remote_key) != 1 ||
		EVP_DigestVerify(md, sig, 64, (const guchar *)bytes, length) != 1)
		goto refused;
	g_object_get(venture_context_get_config(context), "federation-mode", &mode, NULL);
	global = g_strcmp0(mode, "global") == 0;
	query = venture_query_new(VENTURE_TYPE_FEDERATION_PEER);
	venture_query_add_filter_string(query, "origin", VENTURE_FILTER_OP_EQ, claimed_origin, NULL);
	venture_query_set_include_deleted(query, TRUE);
	peer = venture_database_find_one(venture_context_get_database(context), query, error);
	if (error && *error)
		return NULL;
	if (peer)
	{
		g_autofree gchar *pin = NULL;
		gboolean active;
		g_object_get(peer, "public-key", &pin, "active", &active, NULL);
		if (!active || venture_entity_is_deleted(peer) || g_strcmp0(pin, claimed_key))
			goto refused;
		peer_id = venture_entity_get_id(peer);
	}
	else if (!global)
		goto refused;
	{
		GHashTable *consumed = fed_challenges(context);
		GHashTableIter iter;
		gpointer value;
		gint64 *until;
		g_hash_table_iter_init(&iter, consumed);
		while (g_hash_table_iter_next(&iter, NULL, &value))
			if (*(gint64 *)value < g_get_monotonic_time()) g_hash_table_iter_remove(&iter);
		if (g_hash_table_size(consumed) >= (peer_id ? FED_CHALLENGES : FED_CHALLENGES / 2))
			goto refused;
		until = g_new(gint64, 1);
		*until = expiry;
		g_hash_table_insert(consumed, g_strdup(challenge), until);
	}
	fingerprint = g_compute_checksum_for_data(G_CHECKSUM_SHA256, raw, 32);
	actor = g_strdup_printf("federation:%s:%s", claimed_origin, fingerprint);
	return fed_operation(context, json_node_get_object(operation), peer_id, global, actor, error);
refused:
	fed_refuse(error, "Federation authentication refused");
	return NULL;
}

/* Responses prove possession of the pinned key even when TLS terminates at
 * a tunnel. The binding prevents replay as a different request's answer. */
static GBytes *
fed_response_message(GBytes *body, const gchar *binding)
{
	g_autoptr(GByteArray) bytes = g_byte_array_new();
	g_autofree gchar *prefix = g_strdup_printf("VENTURE federation response v1\n%s\n", binding);
	gsize size;
	const guint8 *data = g_bytes_get_data(body, &size);
	g_byte_array_append(bytes, (const guint8 *)prefix, (guint)strlen(prefix));
	g_byte_array_append(bytes, data, (guint)size);
	return g_byte_array_free_to_bytes(g_steal_pointer(&bytes));
}

gchar *
venture_federation_sign_response(VentureContext *context, GBytes *body,
	const gchar *binding, GError **error)
{
	g_autofree gchar *origin = NULL;
	g_autofree gchar *pub = NULL;
	g_autoptr(EVP_PKEY) key = fed_key(context, &origin, &pub, error);
	g_autoptr(EVP_MD_CTX) md = EVP_MD_CTX_new();
	g_autoptr(GBytes) message = fed_response_message(body, binding);
	guchar signature[64];
	size_t length = sizeof(signature);
	gsize size;
	const guchar *bytes = g_bytes_get_data(message, &size);
	if (!key) return NULL;
	if (!md || EVP_DigestSignInit(md, NULL, NULL, NULL, key) != 1 ||
		EVP_DigestSign(md, signature, &length, bytes, size) != 1)
	{
		fed_refuse(error, "Cannot sign federation response");
		return NULL;
	}
	return g_base64_encode(signature, length);
}

gboolean
venture_federation_verify_response(const gchar *public_key, GBytes *body,
	const gchar *binding, const gchar *signature, GError **error)
{
	g_autofree guchar *raw = fed_decode(public_key, 32);
	g_autofree guchar *sig = fed_decode(signature, 64);
	g_autoptr(EVP_PKEY) key = NULL;
	g_autoptr(EVP_MD_CTX) md = EVP_MD_CTX_new();
	g_autoptr(GBytes) message = fed_response_message(body, binding);
	gsize size;
	const guchar *bytes = g_bytes_get_data(message, &size);
	if (raw) key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, raw, 32);
	return (key && sig && md && EVP_DigestVerifyInit(md, NULL, NULL, NULL, key) == 1 &&
		EVP_DigestVerify(md, sig, 64, bytes, size) == 1) ||
		fed_refuse(error, "Federation response failed pinned-key signature verification");
}
