/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

static JsonNode *
sync_object(void)
{
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, json_object_new());
	return node;
}

static JsonObject *
sync_child(JsonNode *node, const gchar *name)
{
	JsonNode *child;
	if (!node || !JSON_NODE_HOLDS_OBJECT(node))
		return NULL;
	child = json_object_get_member(json_node_get_object(node), name);
	return child && JSON_NODE_HOLDS_OBJECT(child) ? json_node_get_object(child) : NULL;
}

static gboolean
sync_error(GError **error, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, message);
	return FALSE;
}

static JsonNode *
sync_load(VentureEntity *replica, const gchar *field)
{
	g_autofree gchar *text = NULL;
	g_object_get(replica, field, &text, NULL);
	return venture_json_parse(text, NULL);
}

static void
sync_store(VentureEntity *replica, const gchar *field, JsonNode *node)
{
	g_autofree gchar *text = venture_json_to_string(node, FALSE);
	g_object_set(replica, field, text, NULL);
}

static gboolean
sync_save(VentureContext *context, VentureEntity *replica,
	const VentureActor *actor, GError **error)
{
	gboolean saved;
	/* The marker is private process state: generic JSON, forms, imports and
	 * staged writes cannot manufacture a replica or replace its merge base. */
	g_object_set_data(G_OBJECT(replica), "federation-replica-write", GINT_TO_POINTER(1));
	saved = venture_database_save(venture_context_get_database(context), replica, actor, error);
	g_object_set_data(G_OBJECT(replica), "federation-replica-write", NULL);
	return saved;
}

static gboolean
sync_equal(JsonNode *a, JsonNode *b)
{
	return a == b || (a && b && json_node_equal(a, b));
}

static void
sync_copy(JsonObject *to, const gchar *name, JsonNode *from)
{
	if (from)
		json_object_set_member(to, name, json_node_copy(from));
	else
		json_object_remove_member(to, name);
}

static gboolean
sync_writable(JsonNode *snapshot, const gchar *name)
{
	JsonNode *node;
	JsonArray *array;
	guint i;
	if (!snapshot || !JSON_NODE_HOLDS_OBJECT(snapshot))
		return FALSE;
	node = json_object_get_member(json_node_get_object(snapshot), "writable");
	if (!node || !JSON_NODE_HOLDS_ARRAY(node))
		return FALSE;
	array = json_node_get_array(node);
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *item = json_array_get_element(array, i);
		if (JSON_NODE_HOLDS_VALUE(item) && json_node_get_value_type(item) == G_TYPE_STRING &&
			!g_strcmp0(json_node_get_string(item), name))
			return TRUE;
	}
	return FALSE;
}

static gboolean
sync_valid(JsonNode *snapshot, const gchar *type, const gchar *uuid)
{
	JsonObject *object;
	JsonObject *fields = sync_child(snapshot, "fields");
	JsonObject *schema = sync_child(snapshot, "schema");
	JsonNode *writable;
	JsonArray *array;
	g_autoptr(GList) names = NULL;
	GList *it;
	guint i;
	if (!fields || !schema || !type || !uuid || !g_uuid_string_is_valid(uuid) ||
		strlen(type) > 64 || json_object_get_size(fields) > 256) return FALSE;
	object = json_node_get_object(snapshot);
	writable = json_object_get_member(object, "writable");
	if (!writable || !JSON_NODE_HOLDS_ARRAY(writable)) return FALSE;
	array = json_node_get_array(writable);
	if (json_array_get_length(array) > 256) return FALSE;
	names = json_object_get_members(fields);
	for (it = names; it; it = it->next)
	{
		const gchar *name = it->data;
		const gchar *c;
		JsonNode *spec = json_object_get_member(schema, name);
		if (!*name || strlen(name) > 128 || !spec || !JSON_NODE_HOLDS_OBJECT(spec)) return FALSE;
		for (c = name; *c; c++)
			if (!(g_ascii_islower(*c) || g_ascii_isdigit(*c) || *c == '_')) return FALSE;
	}
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *name = json_array_get_element(array, i);
		if (!JSON_NODE_HOLDS_VALUE(name) || json_node_get_value_type(name) != G_TYPE_STRING ||
			!json_object_has_member(fields, json_node_get_string(name))) return FALSE;
	}
	return !g_strcmp0(venture_json_object_get_string(object, "type", ""), type) &&
		!g_strcmp0(venture_json_object_get_string(object, "uuid", ""), uuid) &&
		venture_json_object_get_int(object, "version", 0) > 0;
}

/* Union keys from all three versions. A remotely removed field is a deletion,
 * not a null, and a local edit to it is a conflict requiring human attention. */
static void
sync_add_keys(GHashTable *keys, JsonObject *object)
{
	g_autoptr(GList) names = json_object_get_members(object);
	GList *it;
	for (it = names; it; it = it->next)
		g_hash_table_add(keys, g_strdup(it->data));
}

static gboolean
sync_merge(VentureEntity *replica, JsonNode *remote, GError **error)
{
	g_autoptr(JsonNode) base = sync_load(replica, "base");
	g_autoptr(JsonNode) working = sync_load(replica, "working");
	g_autoptr(JsonNode) old_conflicts = sync_load(replica, "conflicts");
	g_autofree gchar *remote_text = venture_json_to_string(remote, FALSE);
	g_autoptr(JsonNode) merged = venture_json_parse(remote_text, NULL);
	g_autoptr(JsonNode) conflicts = sync_object();
	g_autoptr(GHashTable) keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	JsonObject *b = sync_child(base, "fields");
	JsonObject *l = sync_child(working, "fields");
	JsonObject *r = sync_child(remote, "fields");
	JsonObject *m = sync_child(merged, "fields");
	JsonObject *c = json_node_get_object(conflicts);
	GHashTableIter iter;
	gpointer key;
	gboolean pending = FALSE;

	if (!b || !l || !r)
		return sync_error(error, "Replica merge state is invalid; preserved for recovery");
	sync_add_keys(keys, b);
	sync_add_keys(keys, l);
	sync_add_keys(keys, r);
	g_hash_table_iter_init(&iter, keys);
	while (g_hash_table_iter_next(&iter, &key, NULL))
	{
		const gchar *name = key;
		JsonNode *bv = json_object_get_member(b, name);
		JsonNode *lv = json_object_get_member(l, name);
		JsonNode *rv = json_object_get_member(r, name);
		gboolean unresolved = old_conflicts && JSON_NODE_HOLDS_OBJECT(old_conflicts) &&
			json_object_has_member(json_node_get_object(old_conflicts), name);
		if (sync_equal(lv, rv))
			continue;
		if (!unresolved && sync_equal(lv, bv))
			continue;
		pending = TRUE;
		sync_copy(m, name, lv);
		if (unresolved || !sync_equal(rv, bv) || !sync_writable(remote, name))
		{
			JsonObject *detail = json_object_new();
			/* Explicit presence bits distinguish an absent member from null. */
			json_object_set_boolean_member(detail, "base_present", bv != NULL);
			json_object_set_boolean_member(detail, "local_present", lv != NULL);
			json_object_set_boolean_member(detail, "remote_present", rv != NULL);
			if (bv) json_object_set_member(detail, "base", json_node_copy(bv));
			if (lv) json_object_set_member(detail, "local", json_node_copy(lv));
			if (rv) json_object_set_member(detail, "remote", json_node_copy(rv));
			json_object_set_object_member(c, name, detail);
		}
	}
	sync_store(replica, "base", remote);
	sync_store(replica, "working", merged);
	sync_store(replica, "conflicts", conflicts);
	g_object_set(replica, "status", json_object_get_size(c) ? "conflict" : pending ? "pending" : "clean", NULL);
	return TRUE;
}

static VentureEntity *
sync_find(VentureContext *context, gint64 peer_id, const gchar *type, const gchar *uuid, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FEDERATION_REPLICA);
	venture_query_add_filter_int(query, "peer-id", VENTURE_FILTER_OP_EQ, peer_id, NULL);
	venture_query_add_filter_string(query, "record-type", VENTURE_FILTER_OP_EQ, type, NULL);
	venture_query_add_filter_string(query, "record-uuid", VENTURE_FILTER_OP_EQ, uuid, NULL);
	return venture_database_find_one(venture_context_get_database(context), query, error);
}

VentureEntity *
venture_federation_replica_pull(VentureContext *context, gint64 peer_id,
	const gchar *type, const gchar *uuid, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) replica = NULL;
	g_autoptr(VentureEntity) peer = NULL;
	g_autoptr(JsonNode) operation = sync_object();
	g_autoptr(JsonNode) remote = NULL;
	g_autoptr(JsonNode) empty = sync_object();
	g_autofree gchar *pin = NULL;
	g_autofree gchar *old_pin = NULL;
	g_autofree gchar *name = NULL;
	JsonObject *op = json_node_get_object(operation);

	if (!type || !*type || strlen(type) > 64 || !uuid || !g_uuid_string_is_valid(uuid))
	{
		sync_error(error, "Name a record type and valid UUID to pull");
		return NULL;
	}
	peer = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_FEDERATION_PEER, peer_id, error);
	if (!peer)
		return NULL;
	g_object_get(peer, "public-key", &pin, NULL);
	replica = sync_find(context, peer_id, type, uuid, error);
	if (error && *error)
		return NULL;
	if (replica)
	{
		g_object_get(replica, "peer-key", &old_pin, NULL);
		if (g_strcmp0(old_pin, pin))
		{
			sync_error(error, "Source key changed; explicitly re-establish trust before replacing this replica");
			return NULL;
		}
	}
	json_object_set_string_member(op, "action", "get");
	json_object_set_string_member(op, "type", type);
	json_object_set_string_member(op, "uuid", uuid);
	remote = venture_federation_request(context, peer_id, operation, error);
	if (!remote)
		return NULL;
	if (!sync_valid(remote, type, uuid))
	{
		sync_error(error, "Source returned an invalid or mismatched replica");
		return NULL;
	}
	if (replica)
	{
		if (!sync_merge(replica, remote, error))
			return NULL;
	}
	else
	{
		/* Another request can finish a pull while the HTTP main loop runs.
		 * Refuse a duplicate instead of splitting the working copy in two. */
		g_autoptr(VentureEntity) concurrent = sync_find(context, peer_id, type, uuid, error);
		if (concurrent || (error && *error))
		{
			if (concurrent) sync_error(error, "Replica was imported concurrently; retry");
			return NULL;
		}
		g_autofree gchar *identity = g_strdup_printf("%" G_GINT64_FORMAT "/%s/%s", peer_id, type, uuid);
		replica = VENTURE_ENTITY(venture_federation_replica_new());
		name = g_strdup_printf("%s: %s", type,
			venture_json_object_get_string(sync_child(remote, "fields"), "name", uuid));
		g_object_set(replica, "name", name, "source-identity", identity, "peer-id", peer_id, "peer-key", pin,
			"record-type", type, "record-uuid", uuid, "status", "clean", NULL);
		venture_entity_set_organization_id(replica, venture_context_get_default_organization_id(context));
		sync_store(replica, "base", remote);
		sync_store(replica, "working", remote);
		sync_store(replica, "conflicts", empty);
	}
	/* Optimistic concurrency refuses an overwrite if somebody edited the
	 * local copy while the network operation was in flight. */
	if (!sync_save(context, replica, actor, error))
		return NULL;
	return g_steal_pointer(&replica);
}

VentureEntity *
venture_federation_replica_edit(VentureContext *context, gint64 id,
	gint64 version, JsonNode *changes, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) replica = NULL;
	g_autoptr(JsonNode) working = NULL;
	g_autoptr(JsonNode) conflicts = NULL;
	g_autoptr(GList) names = NULL;
	GList *it;
	JsonObject *fields;

	replica = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_FEDERATION_REPLICA, id, error);
	if (!replica)
		return NULL;
	if (venture_entity_is_deleted(replica) || version != venture_entity_get_version(replica) ||
		!changes || !JSON_NODE_HOLDS_OBJECT(changes))
	{
		sync_error(error, "Replica changed; reload before editing");
		return NULL;
	}
	working = sync_load(replica, "working");
	conflicts = sync_load(replica, "conflicts");
	fields = sync_child(working, "fields");
	if (!fields || !conflicts || !JSON_NODE_HOLDS_OBJECT(conflicts))
	{
		sync_error(error, "Replica state is invalid");
		return NULL;
	}
	names = json_object_get_members(json_node_get_object(changes));
	for (it = names; it; it = it->next)
	{
		const gchar *name = it->data;
		JsonNode *value = json_object_get_member(json_node_get_object(changes), name);
		/* No local type is required: a new remote plugin's record is editable
		 * from its granted schema without installing that plugin locally. */
		if (!json_object_has_member(fields, name) || !sync_writable(working, name))
		{
			sync_error(error, "Field is not editable in the last authenticated schema");
			return NULL;
		}
		sync_copy(fields, name, value);
		json_object_remove_member(json_node_get_object(conflicts), name);
	}
	sync_store(replica, "working", working);
	sync_store(replica, "conflicts", conflicts);
	g_object_set(replica, "status", json_object_get_size(json_node_get_object(conflicts)) ? "conflict" : "pending", NULL);
	if (!sync_save(context, replica, actor, error))
		return NULL;
	return g_steal_pointer(&replica);
}

VentureEntity *
venture_federation_replica_sync(VentureContext *context, gint64 id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) replica = NULL;
	g_autoptr(VentureEntity) pulled = NULL;
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(JsonNode) base = NULL;
	g_autoptr(JsonNode) working = NULL;
	g_autoptr(JsonNode) operation = sync_object();
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GList) names = NULL;
	g_autofree gchar *type = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *status = NULL;
	gint64 peer_id;
	JsonObject *op = json_node_get_object(operation);
	JsonObject *changes;
	JsonObject *b;
	JsonObject *l;
	GList *it;

	replica = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_FEDERATION_REPLICA, id, error);
	if (!replica)
		return NULL;
	g_object_get(replica, "peer-id", &peer_id, "record-type", &type, "record-uuid", &uuid, NULL);
	pulled = venture_federation_replica_pull(context, peer_id, type, uuid, actor, error);
	if (!pulled)
		return NULL;
	g_object_get(pulled, "status", &status, NULL);
	if (g_strcmp0(status, "pending"))
		return g_steal_pointer(&pulled);
	base = sync_load(pulled, "base");
	working = sync_load(pulled, "working");
	b = sync_child(base, "fields");
	l = sync_child(working, "fields");
	changes = json_object_new();
	json_object_set_string_member(op, "action", "update");
	json_object_set_string_member(op, "type", type);
	json_object_set_string_member(op, "uuid", uuid);
	json_object_set_int_member(op, "version", venture_json_object_get_int(json_node_get_object(base), "version", 0));
	json_object_set_object_member(op, "fields", changes);
	names = json_object_get_members(l);
	for (it = names; it; it = it->next)
	{
		const gchar *name = it->data;
		JsonNode *value = json_object_get_member(l, name);
		if (!sync_equal(value, json_object_get_member(b, name)))
			json_object_set_member(changes, name, json_node_copy(value));
	}
	answer = venture_federation_request(context, peer_id, operation, error);
	if (!answer)
		return NULL;
	if (!sync_valid(answer, type, uuid))
	{
		sync_error(error, "Source returned an invalid merge acknowledgement");
		return NULL;
	}
	/* If the remote commit succeeded but saving this acknowledgment loses a
	 * local race or the process dies, the next pull recognizes equal values.
	 * There is no blind replay of an old mutation or increment operation. */
	current = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_FEDERATION_REPLICA, id, error);
	if (!current)
		return NULL;
	if (venture_entity_get_version(current) != venture_entity_get_version(pulled))
	{
		sync_error(error, "Remote saved; local edits arrived meanwhile. Sync again to reconcile");
		return NULL;
	}
	/* The acknowledged write is the new base. Preserve server-side validator
	 * adjustments as canonical instead of treating them as fresh conflicts. */
	sync_store(pulled, "base", answer);
	sync_store(pulled, "working", answer);
	g_object_set(pulled, "status", "clean", NULL);
	if (!sync_save(context, pulled, actor, error))
		return NULL;
	return g_steal_pointer(&pulled);
}

VentureEntity *
venture_federation_replica_resolve(VentureContext *context, gint64 id,
	gint64 version, const gchar *field, gboolean keep_local,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) replica = NULL;
	g_autoptr(JsonNode) working = NULL;
	g_autoptr(JsonNode) base = NULL;
	g_autoptr(JsonNode) conflicts = NULL;
	JsonObject *local;
	JsonObject *remote;
	replica = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_FEDERATION_REPLICA, id, error);
	if (!replica) return NULL;
	working = sync_load(replica, "working");
	base = sync_load(replica, "base");
	conflicts = sync_load(replica, "conflicts");
	local = sync_child(working, "fields");
	remote = sync_child(base, "fields");
	if (venture_entity_is_deleted(replica) || version != venture_entity_get_version(replica) ||
		!field || !local || !remote || !conflicts || !JSON_NODE_HOLDS_OBJECT(conflicts) ||
		!json_object_has_member(json_node_get_object(conflicts), field) ||
		(keep_local && !sync_writable(base, field)))
	{
		sync_error(error, "Conflict changed or the field is no longer writable");
		return NULL;
	}
	/* Accepting remote absence is a real resolution. An editor never needs
	 * permission to send a revoked field just to discard their local edit. */
	if (!keep_local) sync_copy(local, field, json_object_get_member(remote, field));
	json_object_remove_member(json_node_get_object(conflicts), field);
	sync_store(replica, "working", working);
	sync_store(replica, "conflicts", conflicts);
	g_object_set(replica, "status", json_object_get_size(json_node_get_object(conflicts)) ? "conflict" :
		json_object_equal(local, remote) ? "clean" : "pending", NULL);
	if (!sync_save(context, replica, actor, error)) return NULL;
	return g_steal_pointer(&replica);
}

JsonNode *
venture_federation_collection_pull(VentureContext *context, gint64 peer_id,
	const gchar *collection, gint64 offset, const VentureActor *actor, GError **error)
{
	g_autoptr(JsonNode) operation = sync_object();
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(JsonNode) report = sync_object();
	JsonObject *op = json_node_get_object(operation);
	JsonNode *records_node;
	JsonArray *records;
	JsonArray *outcomes = json_array_new();
	guint i;
	if (venture_string_is_empty(collection) || strlen(collection) > 128 || offset < 0)
	{
		json_array_unref(outcomes);
		sync_error(error, "Name a collection and a nonnegative page offset");
		return NULL;
	}
	json_object_set_array_member(json_node_get_object(report), "replicas", outcomes);
	json_object_set_string_member(op, "action", "list");
	json_object_set_string_member(op, "collection", collection);
	json_object_set_int_member(op, "offset", offset);
	json_object_set_int_member(op, "limit", 10);
	result = venture_federation_request(context, peer_id, operation, error);
	if (!result) return NULL;
	records_node = json_object_get_member(json_node_get_object(result), "records");
	if (!records_node || !JSON_NODE_HOLDS_ARRAY(records_node) || json_array_get_length(json_node_get_array(records_node)) > 10)
	{
		sync_error(error, "Invalid collection manifest");
		return NULL;
	}
	records = json_node_get_array(records_node);
	for (i = 0; i < json_array_get_length(records); i++)
	{
		JsonNode *item = json_array_get_element(records, i);
		g_autoptr(VentureEntity) replica = NULL;
		g_autoptr(GError) item_error = NULL;
		JsonObject *outcome = json_object_new();
		const gchar *type;
		const gchar *uuid;
		if (!JSON_NODE_HOLDS_OBJECT(item))
		{
			json_object_unref(outcome);
			sync_error(error, "Invalid collection record");
			return NULL;
		}
		type = venture_json_object_get_string(json_node_get_object(item), "type", "");
		uuid = venture_json_object_get_string(json_node_get_object(item), "uuid", "");
		replica = venture_federation_replica_pull(context, peer_id, type, uuid, actor, &item_error);
		json_object_set_string_member(outcome, "type", type);
		json_object_set_string_member(outcome, "uuid", uuid);
		if (replica)
			json_object_set_int_member(outcome, "id", venture_entity_get_id(replica));
		else
			json_object_set_string_member(outcome, "error", item_error ? item_error->message : "Pull failed");
		json_array_add_object_element(outcomes, outcome);
	}
	json_object_set_int_member(json_node_get_object(report), "next_offset", offset + json_array_get_length(records));
	json_object_set_boolean_member(json_node_get_object(report), "more", json_array_get_length(records) == 10);
	return g_steal_pointer(&report);
}

/* One replica per tick keeps reconnect work bounded. Async HTTP is pumped on
 * the main context and never gives a database operation to a worker thread. */
static gboolean
sync_tick(gpointer data)
{
	g_autoptr(VentureContext) context = g_object_ref(data);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FEDERATION_REPLICA);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) synced = NULL;
	g_autoptr(GError) error = NULL;
	gint64 last = (gint64)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(context), "federation-sync-last"));
	VentureActor actor;
	if (!venture_context_module_enabled(context, "federation")) return G_SOURCE_CONTINUE;
	venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_GT, last, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(venture_context_get_database(context), query, &error);
	if (!rows || rows->len == 0)
	{
		g_object_set_data(G_OBJECT(context), "federation-sync-last", NULL);
		return G_SOURCE_CONTINUE;
	}
	last = venture_entity_get_id(g_ptr_array_index(rows, 0));
	g_object_set_data(G_OBJECT(context), "federation-sync-last", GSIZE_TO_POINTER((gsize)last));
	actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor.name = "federation reconnect";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	synced = venture_federation_replica_sync(context, last, &actor, &error);
	/* A down peer is routine. Preserve the working copy and try again next
	 * rotation, without logging remote data or creating an endless audit. */
	if (!synced && error) g_debug("Federation replica %" G_GINT64_FORMAT " awaits reconnect: %s", last, error->message);
	return G_SOURCE_CONTINUE;
}

static void
sync_source_free(gpointer data)
{
	guint source = GPOINTER_TO_UINT(data);
	if (source) g_source_remove(source);
}

void
venture_federation_sync_start(VentureContext *context)
{
	gint64 interval;
	gboolean enabled;
	guint source;
	if (g_object_get_data(G_OBJECT(context), "federation-sync-source")) return;
	g_object_get(venture_context_get_config(context), "federation-enabled", &enabled,
		"federation-sync-interval", &interval, NULL);
	if (!enabled || interval <= 0) return;
	interval = CLAMP(interval, 5, 86400);
	source = g_timeout_add_seconds((guint)interval, sync_tick, context);
	g_object_set_data_full(G_OBJECT(context), "federation-sync-source", GUINT_TO_POINTER(source), sync_source_free);
}

void
venture_federation_sync_stop(VentureContext *context)
{
	g_object_set_data(G_OBJECT(context), "federation-sync-source", NULL);
}
