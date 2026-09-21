/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <glib/gstdio.h>
#include <string.h>
#include "venture-test-util.h"

/* A new plugin type participates through one class metadata declaration. */
#define VENTURE_TYPE_FEDERATION_TEST_RECORD (venture_federation_test_record_get_type())
VENTURE_DECLARE_ENTITY(VentureFederationTestRecord, venture_federation_test_record, FEDERATION_TEST_RECORD)
static const VentureFieldDecl test_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "A field unknown to the federation service"),
	VENTURE_FIELD_TEXT("detail", "Detail", NULL)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureFederationTestRecord, venture_federation_test_record, test_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

typedef struct
{
	gchar *directory;
	VentureConfig *config[2];
	VentureDatabase *database[2];
	VentureContext *context[2];
	VentureWebServer *server[2];
	gchar *origin[2];
	gchar *pub[2];
	gchar *key_path[2];
	gint64 peer[2];
	VentureEntity *record;
	VentureEntity *grant;
} Fixture;

static void
save(Fixture *fixture, guint server, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	venture_entity_set_organization_id(record, venture_context_get_default_organization_id(fixture->context[server]));
	g_assert_true(venture_database_save(fixture->database[server], record, NULL, &error));
	g_assert_no_error(error);
}

static void
make_key(const gchar *path)
{
	EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
	BIO *bio = BIO_new(BIO_s_mem());
	char *data;
	long length;
	g_assert_nonnull(key);
	g_assert_cmpint(PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL), ==, 1);
	length = BIO_get_mem_data(bio, &data);
	g_assert_true(g_file_set_contents_full(path, data, length, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL));
	BIO_free(bio);
	EVP_PKEY_free(key);
}

static void
setup(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cert = g_canonicalize_filename("tests/fixtures/federation/tls-cert.pem", NULL);
	g_autofree gchar *tls_key = g_canonicalize_filename("tests/fixtures/federation/tls-key.pem", NULL);
	guint i;
	(void)data;
	fixture->directory = g_dir_make_tmp("venture-federation-XXXXXX", &error);
	g_assert_no_error(error);
	for (i = 0; i < 2; i++)
	{
		g_autofree gchar *state = g_strdup_printf("%s/server-%u", fixture->directory, i);
		g_autofree gchar *uri = g_strdup_printf("sqlite://%s/records.db", state);
		g_autoptr(JsonNode) identity = NULL;
		g_assert_cmpint(g_mkdir_with_parents(state, 0700), ==, 0);
		fixture->key_path[i] = g_build_filename(state, "identity.pem", NULL);
		make_key(fixture->key_path[i]);
		fixture->config[i] = venture_config_new();
		g_object_set(fixture->config[i], "state-dir", state,
			"federation-enabled", TRUE, "federation-sync-interval", (gint64)0, "federation-key-file", fixture->key_path[i],
			"federation-ca-file", cert, "server-port", (gint64)0,
			"server-tls-certificate", cert, "server-tls-private-key", tls_key,
			"server-base-url", "https://normal-web.example", NULL);
		fixture->database[i] = venture_database_new(uri, &error);
		g_assert_no_error(error);
		fixture->context[i] = venture_context_new(fixture->config[i], fixture->database[i]);
		g_assert_true(venture_database_migrate(fixture->database[i], venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		fixture->server[i] = venture_web_server_new(fixture->context[i], &error);
		g_assert_no_error(error);
		g_assert_true(venture_web_server_start(fixture->server[i], &error));
		g_assert_no_error(error);
		fixture->origin[i] = g_strdup(venture_web_server_get_base_url(fixture->server[i]));
		fixture->origin[i][strlen(fixture->origin[i]) - 1] = '\0';
		g_object_set(fixture->config[i], "federation-origin", fixture->origin[i], NULL);
		identity = venture_federation_identity(fixture->context[i], &error);
		g_assert_no_error(error);
		g_assert_nonnull(identity);
		fixture->pub[i] = g_strdup(json_object_get_string_member(json_node_get_object(identity), "public_key"));
	}
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureEntity) peer = VENTURE_ENTITY(venture_federation_peer_new());
		g_object_set(peer, "name", "Other server", "origin", fixture->origin[1-i],
			"public-key", fixture->pub[1-i], "active", TRUE, NULL);
		save(fixture, i, peer);
		fixture->peer[i] = venture_entity_get_id(peer);
	}
	fixture->record = VENTURE_ENTITY(venture_venture_new());
	g_object_set(fixture->record, "name", "Shared business", "description", "Original", "notes", "PRIVATE", NULL);
	save(fixture, 0, fixture->record);
	fixture->grant = VENTURE_ENTITY(venture_federation_grant_new());
	g_object_set(fixture->grant, "name", "Business collaboration", "peer-id", fixture->peer[0],
		"record-type", "venture", "record-uuid", venture_entity_get_uuid(fixture->record),
		"fields", "name,description", "write-fields", "name,description", "collection", "business", "active", TRUE, NULL);
	save(fixture, 0, fixture->grant);
}

static void
teardown(Fixture *fixture, gconstpointer data)
{
	guint i;
	(void)data;
	g_clear_object(&fixture->record);
	g_clear_object(&fixture->grant);
	for (i = 0; i < 2; i++)
	{
		venture_web_server_stop(fixture->server[i]);
		g_clear_object(&fixture->server[i]);
		g_clear_object(&fixture->context[i]);
		g_clear_object(&fixture->database[i]);
		g_clear_object(&fixture->config[i]);
		g_free(fixture->origin[i]);
		g_free(fixture->pub[i]);
		g_free(fixture->key_path[i]);
	}
	venture_test_remove_tree(fixture->directory);
	g_free(fixture->directory);
}

static JsonNode *
operation(Fixture *fixture, const gchar *action)
{
	JsonNode *node = venture_json_parse("{}", NULL);
	JsonObject *object = json_node_get_object(node);
	json_object_set_string_member(object, "action", action);
	json_object_set_string_member(object, "type", "venture");
	json_object_set_string_member(object, "uuid", venture_entity_get_uuid(fixture->record));
	return node;
}

static JsonNode *
call(Fixture *fixture, JsonNode *op, GError **error)
{
	return venture_federation_request(fixture->context[1], fixture->peer[1], op, error);
}

static void
test_https_and_scope(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) op = operation(fixture, "get");
	g_autoptr(JsonNode) result = call(fixture, op, &error);
	g_autoptr(VentureEntity) private_record = VENTURE_ENTITY(venture_venture_new());
	JsonObject *fields;
	(void)data;
	/* Real HTTPS with a private CA, two identities and a federation origin
	 * different from the ordinary UI URL. A local numeric id never escapes. */
	g_assert_no_error(error);
	g_assert_nonnull(result);
	fields = json_object_get_object_member(json_node_get_object(result), "fields");
	g_assert_cmpstr(json_object_get_string_member(fields, "name"), ==, "Shared business");
	g_assert_false(json_object_has_member(fields, "notes"));
	g_assert_false(json_object_has_member(fields, "organization_id"));
	g_assert_false(json_object_has_member(json_node_get_object(result), "id"));
	g_object_set(private_record, "name", "Private business", NULL);
	save(fixture, 0, private_record);
	json_object_set_string_member(json_node_get_object(op), "uuid", venture_entity_get_uuid(private_record));
	g_clear_pointer(&result, json_node_unref);
	result = call(fixture, op, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
}

static void
test_signature_replay(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) identity = venture_federation_identity(fixture->context[0], &error);
	g_autoptr(JsonNode) op = operation(fixture, "get");
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GBytes) changed = NULL;
	g_autofree gchar *signature = NULL;
	g_autofree gchar *text = NULL;
	gsize length;
	(void)data;
	body = venture_federation_sign(fixture->context[1], identity, op, &signature, &error);
	g_assert_no_error(error);
	text = g_strndup(g_bytes_get_data(body, &length), g_bytes_get_size(body));
	text[1] = ' ';
	changed = g_bytes_new(text, strlen(text));
	answer = venture_federation_receive(fixture->context[0], changed, signature, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	answer = venture_federation_receive(fixture->context[0], body, signature, &error);
	g_assert_no_error(error);
	g_assert_nonnull(answer);
	g_clear_pointer(&answer, json_node_unref);
	answer = venture_federation_receive(fixture->context[0], body, signature, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

static void
test_revocation_and_global(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) peer = venture_database_get(fixture->database[0], VENTURE_TYPE_FEDERATION_PEER, fixture->peer[0], NULL);
	g_autoptr(JsonNode) op = operation(fixture, "get");
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(peer, "active", FALSE, NULL);
	save(fixture, 0, peer);
	answer = call(fixture, op, &error);
	g_assert_null(answer);
	g_assert_nonnull(error);
	g_clear_error(&error);
	/* Global access is a separate explicit grant. Claiming a fresh origin
	 * cannot inherit the disabled named peer's grant. */
	g_object_set(fixture->config[0], "federation-mode", "global", NULL);
	g_object_set(fixture->config[1], "federation-origin", "https://unknown.example", NULL);
	answer = call(fixture, op, &error);
	g_assert_null(answer);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(fixture->grant, "peer-id", (gint64)0, "write-fields", "", NULL);
	save(fixture, 0, fixture->grant);
	answer = call(fixture, op, &error);
	g_assert_no_error(error);
	g_assert_nonnull(answer);
	g_clear_pointer(&answer, json_node_unref);
	g_object_set(fixture->grant, "active", FALSE, NULL);
	save(fixture, 0, fixture->grant);
	answer = call(fixture, op, &error);
	g_assert_null(answer);
	g_assert_nonnull(error);
}

static void
test_grant_validation(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) organization = venture_database_get(fixture->database[0], VENTURE_TYPE_ORGANIZATION,
		venture_context_get_default_organization_id(fixture->context[0]), NULL);
	g_autoptr(VentureEntity) user = VENTURE_ENTITY(venture_user_new());
	(void)data;
	g_object_set(fixture->grant, "fields", "*", NULL);
	g_assert_false(venture_database_save(fixture->database[0], fixture->grant, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(fixture->grant, "record-type", "organization", "record-uuid", venture_entity_get_uuid(organization),
		"fields", "name,tax_id", "write-fields", "", NULL);
	g_assert_false(venture_database_save(fixture->database[0], fixture->grant, NULL, &error));
	g_clear_error(&error);
	g_object_set(user, "username", "private-account", NULL);
	save(fixture, 0, user);
	g_object_set(fixture->grant, "record-type", "user", "record-uuid", venture_entity_get_uuid(user), "fields", "username", NULL);
	g_assert_false(venture_database_save(fixture->database[0], fixture->grant, NULL, &error));
	g_clear_error(&error);
	g_object_set(fixture->grant, "record-type", "venture", "record-uuid", venture_entity_get_uuid(fixture->record),
		"fields", "name", "write-fields", "name", "peer-id", (gint64)0, NULL);
	g_assert_false(venture_database_save(fixture->database[0], fixture->grant, NULL, &error));
	g_assert_nonnull(error);
}

static void
test_updates(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) op = operation(fixture, "update");
	g_autoptr(JsonNode) fields = venture_json_parse("{\"name\":\"Together\"}", NULL);
	g_autoptr(JsonNode) answer = NULL;
	JsonObject *object = json_node_get_object(op);
	(void)data;
	json_object_set_int_member(object, "version", venture_entity_get_version(fixture->record));
	json_object_set_member(object, "fields", json_node_copy(fields));
	answer = call(fixture, op, &error);
	g_assert_no_error(error);
	g_assert_nonnull(answer);
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_node_get_object(answer), "fields"), "name"), ==, "Together");
	g_clear_pointer(&answer, json_node_unref);
	answer = call(fixture, op, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	json_object_set_int_member(object, "version", venture_entity_get_version(fixture->record) + 1);
	json_object_set_string_member(json_node_get_object(fields), "notes", "stolen");
	json_object_set_member(object, "fields", json_node_copy(fields));
	answer = call(fixture, op, &error);
	g_assert_null(answer);
	g_assert_nonnull(error);
}

static VentureEntity *
pull(Fixture *fixture)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *replica = venture_federation_replica_pull(fixture->context[1], fixture->peer[1],
		"venture", venture_entity_get_uuid(fixture->record), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(replica);
	return replica;
}

static VentureEntity *
edit(Fixture *fixture, VentureEntity *replica, const gchar *json)
{
	g_autoptr(JsonNode) fields = venture_json_parse(json, NULL);
	g_autoptr(GError) error = NULL;
	VentureEntity *updated = venture_federation_replica_edit(fixture->context[1],
		venture_entity_get_id(replica), venture_entity_get_version(replica), fields, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(updated);
	return updated;
}

static void
assert_field(VentureEntity *replica, const gchar *field, const gchar *expected)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_object_get(replica, "working", &text, NULL);
	node = venture_json_parse(text, NULL);
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_node_get_object(node), "fields"), field), ==, expected);
}

static void
test_offline_merge(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = NULL;
	g_autoptr(VentureEntity) synced = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *status = NULL;
	(void)data;
	venture_web_server_stop(fixture->server[0]);
	edited = edit(fixture, replica, "{\"name\":\"Offline local\"}");
	synced = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_assert_null(synced);
	g_assert_nonnull(error);
	g_clear_error(&error);
	assert_field(edited, "name", "Offline local");
	/* A disjoint home-server edit merges without clobbering either side. */
	g_object_set(fixture->record, "description", "Remote changed", NULL);
	save(fixture, 0, fixture->record);
	/* Restore the original ephemeral port, so the pinned origin is unchanged. */
	{
		g_autoptr(GUri) origin = g_uri_parse(fixture->origin[0], G_URI_FLAGS_NONE, NULL);
		g_object_set(fixture->config[0], "server-port", (gint64)g_uri_get_port(origin), NULL);
	}
	g_clear_object(&fixture->server[0]);
	fixture->server[0] = venture_web_server_new(fixture->context[0], &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server[0], &error));
	g_assert_no_error(error);
	synced = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(synced);
	assert_field(synced, "name", "Offline local");
	assert_field(synced, "description", "Remote changed");
	g_object_get(synced, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "clean");
}

/* Automatic reconnect must actually submit durable local work, not merely
 * leave a timer running while only the manual endpoint works. */
static void
test_scheduler(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = edit(fixture, replica, "{\"name\":\"Scheduled edit\"}");
	g_autoptr(VentureEntity) current = NULL;
	gint64 deadline = g_get_monotonic_time() + 12 * G_TIME_SPAN_SECOND;
	gboolean clean = FALSE;
	(void)data;
	g_object_set(fixture->config[1], "federation-sync-interval", (gint64)5, NULL);
	venture_federation_sync_start(fixture->context[1]);
	while (!clean && g_get_monotonic_time() < deadline)
	{
		g_autofree gchar *status = NULL;
		while (g_main_context_iteration(NULL, FALSE)) {}
		g_clear_object(&current);
		current = venture_database_get(fixture->database[1], VENTURE_TYPE_FEDERATION_REPLICA,
			venture_entity_get_id(edited), NULL);
		g_assert_nonnull(current);
		g_object_get(current, "status", &status, NULL);
		clean = g_strcmp0(status, "clean") == 0;
		if (!clean) g_usleep(10000);
	}
	venture_federation_sync_stop(fixture->context[1]);
	g_assert_true(clean);
	g_clear_object(&current);
	current = venture_database_get(fixture->database[0], VENTURE_TYPE_VENTURE,
		venture_entity_get_id(fixture->record), NULL);
	{
		g_autofree gchar *name = NULL;
		g_object_get(current, "name", &name, NULL);
		g_assert_cmpstr(name, ==, "Scheduled edit");
	}
}

static void
test_conflict(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = edit(fixture, replica, "{\"name\":\"Local\"}");
	g_autoptr(VentureEntity) synced = NULL;
	g_autoptr(VentureEntity) resolved = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *conflicts = NULL;
	g_autofree gchar *status = NULL;
	(void)data;
	g_object_set(fixture->record, "name", "Remote", NULL);
	save(fixture, 0, fixture->record);
	synced = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(synced);
	assert_field(synced, "name", "Local");
	g_object_get(synced, "conflicts", &conflicts, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "conflict");
	g_assert_nonnull(strstr(conflicts, "Remote"));
	/* Repeated sync must not erase an unresolved conflict by moving its base. */
	g_clear_object(&synced);
	synced = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_assert_no_error(error);
	resolved = edit(fixture, synced, "{\"name\":\"Agreed\"}");
	g_clear_object(&synced);
	synced = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(resolved), NULL, &error);
	g_assert_no_error(error);
	assert_field(synced, "name", "Agreed");
}

/* Read from disk and from the authoritative server: an in-memory result alone
 * cannot prove that a merge was persisted or that its remote write happened. */
static void
assert_status(VentureEntity *replica, const gchar *expected)
{
	g_autofree gchar *status = NULL;
	g_object_get(replica, "status", &status, NULL);
	g_assert_cmpstr(status, ==, expected);
}

static void
assert_home(Fixture *fixture, const gchar *expected, gint64 version)
{
	g_autoptr(VentureEntity) home = venture_database_get(fixture->database[0],
		VENTURE_TYPE_VENTURE, venture_entity_get_id(fixture->record), NULL);
	g_autofree gchar *name = NULL;
	g_autofree gchar *notes = NULL;
	g_assert_nonnull(home);
	g_object_get(home, "name", &name, "notes", &notes, NULL);
	g_assert_cmpstr(name, ==, expected);
	g_assert_cmpstr(notes, ==, "PRIVATE");
	g_assert_cmpint(venture_entity_get_version(home), ==, version);
}

/* Retained platform peers must not turn a hosted timer into operator work.
 * Real signed HTTPS proves both the refusal and explicit maintenance path. */
static void
test_hosted_outbound(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = edit(fixture, replica, "{\"name\":\"Operator reviewed edit\"}");
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) op = operation(fixture, "get"), answer = NULL;
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/server-1/records.db", fixture->directory);
	VentureTenantService *tenant;
	(void)data;
	venture_web_server_stop(fixture->server[1]);
	g_clear_object(&fixture->server[1]);
	g_clear_object(&fixture->context[1]);
	g_clear_object(&fixture->database[1]);
	g_object_set(fixture->config[1], "hosted-enabled", TRUE,
		"hosted-workspace-id", "c82ee8ea-1842-44bf-91f4-8231be82d5d1",
		"hosted-origin", "https://hosted.example.test", NULL);
	fixture->database[1] = venture_database_new(uri, &error);
	g_assert_no_error(error);
	fixture->context[1] = venture_context_new(fixture->config[1], fixture->database[1]);
	fixture->server[1] = venture_web_server_new(fixture->context[1], &error);
	g_assert_no_error(error);
	tenant = venture_tenant_service_get(fixture->database[1]);
	answer = call(fixture, op, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_true(venture_tenant_service_set_state_operator(tenant, "suspended", "Review retained federation", &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(tenant, "Explicit federation review", &error);
		gint64 deadline = g_get_monotonic_time() + 7 * G_TIME_SPAN_SECOND;
		g_assert_no_error(error);
		/* An unrelated timer dispatched by an operator's nested main loop
		 * must not inherit that operator's privileged maintenance scope. */
		g_object_set(fixture->config[1], "federation-sync-interval", (gint64)5, NULL);
		venture_federation_sync_start(fixture->context[1]);
		while (g_get_monotonic_time() < deadline) {
			while (g_main_context_iteration(NULL, FALSE)) {}
			g_usleep(10000);
		}
		venture_federation_sync_stop(fixture->context[1]);
		assert_home(fixture, "Shared business", venture_entity_get_version(fixture->record));
		result = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(result);
		assert_home(fixture, "Operator reviewed edit", venture_entity_get_version(fixture->record) + 1);
		g_assert_true(venture_tenant_maintenance_finish(maintenance, &error));
		g_assert_no_error(error);
	}
}

static void
restart_home(Fixture *fixture)
{
	g_autoptr(GUri) origin = g_uri_parse(fixture->origin[0], G_URI_FLAGS_NONE, NULL);
	g_autoptr(GError) error = NULL;
	g_object_set(fixture->config[0], "server-port", (gint64)g_uri_get_port(origin), NULL);
	g_clear_object(&fixture->server[0]);
	fixture->server[0] = venture_web_server_new(fixture->context[0], &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server[0], &error));
	g_assert_no_error(error);
}

/* Cartesian product: unchanged, independently changed, and convergently
 * changed. Every case crosses a real outage, then repeats sync to pin
 * idempotence, conflict blocking and the authoritative row version. */
static void
test_merge_matrix(Fixture *fixture, gconstpointer data)
{
	guint code = GPOINTER_TO_UINT(data);
	guint local = code / 3;
	guint remote = code % 3;
	const gchar *local_values[] = {"Shared business", "Local", "Agreed"};
	const gchar *remote_values[] = {"Shared business", "Remote", "Agreed"};
	gboolean conflict = local && remote && g_strcmp0(local_values[local], remote_values[remote]);
	const gchar *expected = local ? local_values[local] : remote_values[remote];
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id = venture_entity_get_id(replica);
	gint64 remote_version;
	guint i;
	venture_web_server_stop(fixture->server[0]);
	if (local)
	{
		g_autofree gchar *json = g_strdup_printf("{\"name\":\"%s\"}", local_values[local]);
		current = edit(fixture, replica, json);
		g_clear_object(&current);
	}
	for (i = 0; i < 3; i++)
	{
		current = venture_federation_replica_sync(fixture->context[1], id, NULL, &error);
		g_assert_null(current);
		g_assert_nonnull(error);
		g_clear_error(&error);
		current = venture_database_get(fixture->database[1], VENTURE_TYPE_FEDERATION_REPLICA, id, NULL);
		assert_field(current, "name", local_values[local]);
		g_clear_object(&current);
	}
	g_object_set(fixture->record, "name", remote_values[remote], NULL);
	save(fixture, 0, fixture->record);
	remote_version = venture_entity_get_version(fixture->record);
	if (local && !remote) remote_version++;
	restart_home(fixture);
	for (i = 0; i < 3; i++)
	{
		current = venture_federation_replica_sync(fixture->context[1], id, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(current);
		assert_status(current, conflict ? "conflict" : "clean");
		assert_field(current, "name", expected);
		assert_home(fixture, conflict ? remote_values[remote] : expected, remote_version);
		g_clear_object(&current);
	}
}

/* A repeated conflict must retain the original common ancestor, even if
 * the remote side changes again. Otherwise the operator sees false history. */
static void
test_conflict_history(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = edit(fixture, replica, "{\"name\":\"Local\",\"description\":\"Pending too\"}");
	g_autoptr(VentureEntity) synced = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	(void)data;
	for (i = 0; i < 3; i++)
	{
		g_autofree gchar *remote = g_strdup_printf("Remote %u", i);
		g_autofree gchar *text = NULL;
		g_autoptr(JsonNode) conflicts = NULL;
		JsonObject *detail;
		g_object_set(fixture->record, "name", remote, NULL);
		save(fixture, 0, fixture->record);
		g_clear_object(&synced);
		synced = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
		g_assert_no_error(error);
		g_object_get(synced, "conflicts", &text, NULL);
		conflicts = venture_json_parse(text, NULL);
		detail = json_object_get_object_member(json_node_get_object(conflicts), "name");
		g_assert_cmpstr(json_object_get_string_member(detail, "base"), ==, "Shared business");
		g_assert_cmpstr(json_object_get_string_member(detail, "local"), ==, "Local");
		g_assert_cmpstr(json_object_get_string_member(detail, "remote"), ==, remote);
		assert_home(fixture, remote, venture_entity_get_version(fixture->record));
		/* One conflict blocks even the non-conflicting field's push. */
		assert_field(synced, "description", "Pending too");
	}
}

static void
test_stale_edit(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = edit(fixture, replica, "{\"name\":\"First editor\"}");
	g_autoptr(VentureEntity) refused = NULL;
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(JsonNode) fields = venture_json_parse("{\"name\":\"Stale editor\"}", NULL);
	g_autoptr(GError) error = NULL;
	(void)data;
	refused = venture_federation_replica_edit(fixture->context[1], venture_entity_get_id(replica),
		venture_entity_get_version(replica), fields, NULL, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	current = venture_database_get(fixture->database[1], VENTURE_TYPE_FEDERATION_REPLICA, venture_entity_get_id(edited), NULL);
	assert_field(current, "name", "First editor");
	assert_home(fixture, "Shared business", venture_entity_get_version(fixture->record));
}

/* The idle runs inside the client's nested main loop, after pull has read
 * its local version. A later local edit must never be overwritten by pull. */
typedef struct
{
	Fixture *fixture;
	VentureEntity *replica;
	gboolean fired;
} MergeRace;

static gboolean
local_edit_during_request(gpointer data)
{
	MergeRace *race = data;
	g_autoptr(VentureEntity) edited = edit(race->fixture, race->replica, "{\"name\":\"Concurrent local\"}");
	race->fired = TRUE;
	return G_SOURCE_REMOVE;
}

static void
test_local_race(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	MergeRace race;
	(void)data;
	race.fixture = fixture;
	race.replica = replica;
	race.fired = FALSE;
	g_idle_add(local_edit_during_request, &race);
	result = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(replica), NULL, &error);
	g_assert_true(race.fired);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	result = venture_database_get(fixture->database[1], VENTURE_TYPE_FEDERATION_REPLICA, venture_entity_get_id(replica), NULL);
	assert_field(result, "name", "Concurrent local");
	g_clear_object(&result);
	result = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(replica), NULL, &error);
	g_assert_no_error(error);
	assert_status(result, "clean");
	assert_home(fixture, "Concurrent local", venture_entity_get_version(fixture->record) + 1);
}

/* Write at the exact boundary between persisted pull and outbound update.
 * No timing guess or worker thread is involved. */
static void
remote_edit_after_pull(VentureDatabase *database, VentureEntity *entity,
	gboolean created, gpointer data)
{
	MergeRace *race = data;
	(void)database;
	(void)created;
	if (race->fired || !G_TYPE_CHECK_INSTANCE_TYPE(entity, VENTURE_TYPE_FEDERATION_REPLICA)) return;
	race->fired = TRUE;
	g_object_set(race->fixture->record, "name", "Concurrent remote", NULL);
	save(race->fixture, 0, race->fixture->record);
}

static void
test_remote_race(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = edit(fixture, replica, "{\"name\":\"Pending local\"}");
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	gulong handler;
	MergeRace race;
	(void)data;
	race.fixture = fixture;
	race.replica = edited;
	race.fired = FALSE;
	g_object_set(fixture->record, "description", "Trigger a persisted pull", NULL);
	save(fixture, 0, fixture->record);
	handler = g_signal_connect(fixture->database[1], "entity-saved", G_CALLBACK(remote_edit_after_pull), &race);
	result = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_signal_handler_disconnect(fixture->database[1], handler);
	g_assert_true(race.fired);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	assert_home(fixture, "Concurrent remote", venture_entity_get_version(fixture->record));
	result = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_assert_no_error(error);
	assert_status(result, "conflict");
	assert_field(result, "name", "Pending local");
}

/* Permission withdrawal during an outage never authorizes a stale pending
 * update, and it must not destroy the local draft needed for recovery. */
static void
test_pending_revocation(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = edit(fixture, replica, "{\"name\":\"Keep my draft\"}");
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(fixture->grant, "active", FALSE, NULL);
	save(fixture, 0, fixture->grant);
	result = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	result = venture_database_get(fixture->database[1], VENTURE_TYPE_FEDERATION_REPLICA, venture_entity_get_id(edited), NULL);
	assert_field(result, "name", "Keep my draft");
	assert_home(fixture, "Shared business", venture_entity_get_version(fixture->record));
	g_object_set(fixture->grant, "active", TRUE, NULL);
	save(fixture, 0, fixture->grant);
	g_clear_object(&result);
	result = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_assert_no_error(error);
	assert_status(result, "clean");
	assert_home(fixture, "Keep my draft", venture_entity_get_version(fixture->record) + 1);
}

/* A nullable reference is a structured UUID projection. Null must survive a
 * three-way merge as a value, while a revoked field is genuinely absent. */
static void
test_null_reference_merge(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) idea = VENTURE_ENTITY(venture_idea_new());
	g_autoptr(VentureEntity) grant = VENTURE_ENTITY(venture_federation_grant_new());
	g_autoptr(VentureEntity) replica = NULL;
	g_autoptr(VentureEntity) edited = NULL;
	g_autoptr(VentureEntity) synced = NULL;
	g_autoptr(VentureEntity) home = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) snapshot = NULL;
	gint64 reference;
	(void)data;
	g_object_set(idea, "title", "Shared idea", NULL);
	save(fixture, 0, idea);
	g_object_set(grant, "name", "Reference grant", "peer-id", fixture->peer[0],
		"record-type", "idea", "record-uuid", venture_entity_get_uuid(idea),
		"fields", "title", "active", TRUE, NULL);
	save(fixture, 0, grant);
	g_object_set(fixture->grant, "fields", "name,description,idea_id", "write-fields", "name,description,idea_id", NULL);
	save(fixture, 0, fixture->grant);
	g_object_set(fixture->record, "idea-id", venture_entity_get_id(idea), NULL);
	save(fixture, 0, fixture->record);
	replica = pull(fixture);
	edited = edit(fixture, replica, "{\"idea_id\":null}");
	g_object_set(fixture->record, "description", "Remote description", NULL);
	save(fixture, 0, fixture->record);
	synced = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_assert_no_error(error);
	assert_status(synced, "clean");
	assert_field(synced, "description", "Remote description");
	g_object_get(synced, "working", &text, NULL);
	snapshot = venture_json_parse(text, NULL);
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(
		json_object_get_object_member(json_node_get_object(snapshot), "fields"), "idea_id")));
	home = venture_database_get(fixture->database[0], VENTURE_TYPE_VENTURE, venture_entity_get_id(fixture->record), NULL);
	g_object_get(home, "idea-id", &reference, NULL);
	g_assert_cmpint(reference, ==, 0);
}

static void
test_pin_change_pending(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = edit(fixture, replica, "{\"name\":\"Original source only\"}");
	g_autoptr(VentureEntity) peer = venture_database_get(fixture->database[1], VENTURE_TYPE_FEDERATION_PEER, fixture->peer[1], NULL);
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	/* An owner changing the pin cannot silently retarget existing drafts. */
	g_object_set(peer, "public-key", fixture->pub[1], NULL);
	save(fixture, 1, peer);
	result = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	result = venture_database_get(fixture->database[1], VENTURE_TYPE_FEDERATION_REPLICA, venture_entity_get_id(edited), NULL);
	assert_field(result, "name", "Original source only");
	assert_home(fixture, "Shared business", venture_entity_get_version(fixture->record));
}

static void
test_resolve_choice(Fixture *fixture, gconstpointer data)
{
	gboolean keep_local = GPOINTER_TO_INT(data);
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = edit(fixture, replica, "{\"name\":\"Local choice\"}");
	g_autoptr(VentureEntity) synced = NULL;
	g_autoptr(VentureEntity) resolved = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(fixture->record, "name", "Remote choice", NULL);
	save(fixture, 0, fixture->record);
	synced = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_assert_no_error(error);
	/* A stale conflict screen cannot resolve a newer local version. */
	resolved = venture_federation_replica_resolve(fixture->context[1], venture_entity_get_id(synced),
		venture_entity_get_version(edited), "name", keep_local, NULL, &error);
	g_assert_null(resolved);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	resolved = venture_federation_replica_resolve(fixture->context[1], venture_entity_get_id(synced),
		venture_entity_get_version(synced), "name", keep_local, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resolved);
	g_clear_object(&synced);
	synced = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(resolved), NULL, &error);
	g_assert_no_error(error);
	assert_status(synced, "clean");
	assert_home(fixture, keep_local ? "Local choice" : "Remote choice",
		venture_entity_get_version(fixture->record) + (keep_local ? 1 : 0));
}

static void
test_response_proof(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = g_bytes_new_static("{\"ok\":true}", 11);
	g_autoptr(GBytes) forged = g_bytes_new_static("{\"ok\":false}", 12);
	g_autofree gchar *signature = venture_federation_sign_response(fixture->context[0], body, "request-one", &error);
	(void)data;
	g_assert_no_error(error);
	g_assert_true(venture_federation_verify_response(fixture->pub[0], body, "request-one", signature, &error));
	g_assert_false(venture_federation_verify_response(fixture->pub[0], forged, "request-one", signature, &error));
	g_clear_error(&error);
	g_assert_false(venture_federation_verify_response(fixture->pub[0], body, "request-two", signature, &error));
	g_clear_error(&error);
	g_assert_false(venture_federation_verify_response(fixture->pub[1], body, "request-one", signature, &error));
	g_assert_nonnull(error);
}

static void
test_key_and_discovery_limits(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(JsonNode) op = operation(fixture, "get");
	guint i;
	(void)data;
	/* An unauthenticated discovery flood must not occupy replay slots. */
	for (i = 0; i < 1100; i++)
	{
		g_autoptr(JsonNode) identity = venture_federation_identity(fixture->context[0], &error);
		g_assert_no_error(error);
		g_assert_nonnull(identity);
	}
	result = call(fixture, op, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_clear_pointer(&result, json_node_unref);
	g_assert_cmpint(g_chmod(fixture->key_path[0], 0644), ==, 0);
	result = venture_federation_identity(fixture->context[0], &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(g_chmod(fixture->key_path[0], 0600), ==, 0);
	g_object_set(fixture->config[0], "federation-enabled", FALSE, NULL);
	result = venture_federation_identity(fixture->context[0], &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_object_set(fixture->config[0], "federation-enabled", TRUE, NULL);
}

static void
test_references_and_uri(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) op = operation(fixture, "get");
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(VentureEntity) idea = VENTURE_ENTITY(venture_idea_new());
	JsonObject *object;
	JsonArray *writable;
	gboolean found = FALSE;
	guint i;
	(void)data;
	g_object_set(fixture->record, "url", "https://alice:secret@example.com/path", NULL);
	save(fixture, 0, fixture->record);
	g_object_set(fixture->grant, "fields", "name,description,idea_id,url", "write-fields", "name,description,idea_id", NULL);
	save(fixture, 0, fixture->grant);
	result = call(fixture, op, &error);
	g_assert_no_error(error);
	object = json_node_get_object(result);
	g_assert_false(json_object_has_member(json_object_get_object_member(object, "fields"), "url"));
	g_assert_true(json_object_has_member(json_object_get_object_member(object, "schema"), "idea_id"));
	writable = json_object_get_array_member(object, "writable");
	for (i = 0; i < json_array_get_length(writable); i++)
		if (!g_strcmp0(json_array_get_string_element(writable, i), "idea_id")) found = TRUE;
	g_assert_true(found);
	g_object_set(idea, "title", "Private idea", NULL);
	save(fixture, 0, idea);
	g_object_set(fixture->record, "idea-id", venture_entity_get_id(idea), NULL);
	save(fixture, 0, fixture->record);
	g_clear_pointer(&result, json_node_unref);
	result = call(fixture, op, &error);
	g_assert_no_error(error);
	g_assert_false(json_object_has_member(json_object_get_object_member(json_node_get_object(result), "fields"), "idea_id"));
}

static void
test_revoked_field_resolution(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = edit(fixture, replica, "{\"description\":\"Local draft\"}");
	g_autoptr(VentureEntity) synced = NULL;
	g_autoptr(VentureEntity) resolved = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *status = NULL;
	(void)data;
	g_object_set(fixture->grant, "fields", "name", "write-fields", "name", NULL);
	save(fixture, 0, fixture->grant);
	synced = venture_federation_replica_sync(fixture->context[1], venture_entity_get_id(edited), NULL, &error);
	g_assert_no_error(error);
	assert_field(synced, "description", "Local draft");
	resolved = venture_federation_replica_resolve(fixture->context[1], venture_entity_get_id(synced),
		venture_entity_get_version(synced), "description", FALSE, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resolved);
	g_object_get(resolved, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "clean");
}

static void
test_restart_and_lost_ack(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) replica = pull(fixture);
	g_autoptr(VentureEntity) edited = edit(fixture, replica, "{\"name\":\"Durable edit\"}");
	g_autoptr(VentureEntity) synced = NULL;
	g_autoptr(JsonNode) op = operation(fixture, "update");
	g_autoptr(JsonNode) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/server-1/records.db", fixture->directory);
	g_autofree gchar *status = NULL;
	gint64 id = venture_entity_get_id(edited);
	JsonObject *fields = json_object_new();
	(void)data;
	/* Model a committed remote update whose acknowledgement never reached
	 * the local disk, then restart the local server before reconciling. */
	json_object_set_string_member(fields, "name", "Durable edit");
	json_object_set_object_member(json_node_get_object(op), "fields", fields);
	json_object_set_int_member(json_node_get_object(op), "version", venture_entity_get_version(fixture->record));
	response = call(fixture, op, &error);
	g_assert_no_error(error);
	venture_web_server_stop(fixture->server[1]);
	g_clear_object(&fixture->server[1]);
	g_clear_object(&fixture->context[1]);
	g_clear_object(&fixture->database[1]);
	fixture->database[1] = venture_database_new(uri, &error);
	g_assert_no_error(error);
	fixture->context[1] = venture_context_new(fixture->config[1], fixture->database[1]);
	g_assert_true(venture_database_migrate(fixture->database[1], venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	fixture->server[1] = venture_web_server_new(fixture->context[1], &error);
	g_assert_no_error(error);
	/* Outbound synchronization needs no local listener. */
	synced = venture_federation_replica_sync(fixture->context[1], id, NULL, &error);
	g_assert_no_error(error);
	assert_field(synced, "name", "Durable edit");
	g_object_get(synced, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "clean");
	g_clear_object(&synced);
	synced = venture_database_get(fixture->database[0], VENTURE_TYPE_VENTURE, venture_entity_get_id(fixture->record), &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_version(synced), ==, venture_entity_get_version(fixture->record) + 1);
}

static void
test_collection(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = venture_federation_collection_pull(fixture->context[1], fixture->peer[1], "business", 0, NULL, &error);
	(void)data;
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(result), "replicas")), ==, 1);
	g_assert_false(json_object_get_boolean_member(json_node_get_object(result), "more"));
}

static void
test_future_type(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) record = VENTURE_ENTITY(venture_federation_test_record_new());
	g_autoptr(VentureEntity) replica = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *type = venture_entity_get_entity_name(record);
	(void)data;
	g_assert_true(venture_entity_registry_register(venture_entity_registry_get_default(), VENTURE_TYPE_FEDERATION_TEST_RECORD, &error));
	g_assert_no_error(error);
	g_assert_true(venture_schema_create_table(venture_database_get_connection(fixture->database[0]), VENTURE_TYPE_FEDERATION_TEST_RECORD, &error));
	g_assert_no_error(error);
	g_object_set(record, "name", "New plugin type", "detail", "Extension data", NULL);
	save(fixture, 0, record);
	g_object_set(fixture->grant, "record-type", type, "record-uuid", venture_entity_get_uuid(record), "fields", "name,detail", "write-fields", "detail", NULL);
	save(fixture, 0, fixture->grant);
	replica = venture_federation_replica_pull(fixture->context[1], fixture->peer[1], type, venture_entity_get_uuid(record), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(replica);
	assert_field(replica, "detail", "Extension data");
}

static void
test_upgrade_stays_private(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureContext) context = venture_context_new(config, database);
	g_autoptr(VentureEntity) record = VENTURE_ENTITY(venture_venture_new());
	g_autoptr(VentureEntity) reloaded = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) grants = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *name = NULL;
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_object_set(record, "name", "Historical private venture", NULL);
	venture_entity_set_organization_id(record, venture_context_get_default_organization_id(context));
	g_assert_true(venture_database_save(database, record, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_context_module_enabled(context, "federation"));
	g_object_set(config, "federation-enabled", TRUE, NULL);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	query = venture_query_new(VENTURE_TYPE_FEDERATION_GRANT);
	grants = venture_database_find(database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(grants->len, ==, 0);
	reloaded = venture_database_get(database, VENTURE_TYPE_VENTURE, venture_entity_get_id(record), &error);
	g_assert_no_error(error);
	g_object_get(reloaded, "name", &name, NULL);
	g_assert_cmpstr(name, ==, "Historical private venture");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	{
		guint i;
		for (i = 0; i < 9; i++)
		{
			g_autofree gchar *path = g_strdup_printf("/federation/merge-matrix/%u-%u", i / 3, i % 3);
			g_test_add(path, Fixture, GUINT_TO_POINTER(i), setup, test_merge_matrix, teardown);
		}
	}
	g_test_add("/federation/conflict-history", Fixture, NULL, setup, test_conflict_history, teardown);
	g_test_add("/federation/stale-edit", Fixture, NULL, setup, test_stale_edit, teardown);
	g_test_add("/federation/local-race", Fixture, NULL, setup, test_local_race, teardown);
	g_test_add("/federation/remote-race", Fixture, NULL, setup, test_remote_race, teardown);
	g_test_add("/federation/pending-revocation", Fixture, NULL, setup, test_pending_revocation, teardown);
	g_test_add("/federation/null-reference-merge", Fixture, NULL, setup, test_null_reference_merge, teardown);
	g_test_add("/federation/pin-change-pending", Fixture, NULL, setup, test_pin_change_pending, teardown);
	g_test_add("/federation/resolve-local", Fixture, GINT_TO_POINTER(1), setup, test_resolve_choice, teardown);
	g_test_add("/federation/resolve-remote", Fixture, NULL, setup, test_resolve_choice, teardown);
	g_test_add("/federation/https-scope", Fixture, NULL, setup, test_https_and_scope, teardown);
	g_test_add("/federation/signature-replay", Fixture, NULL, setup, test_signature_replay, teardown);
	g_test_add("/federation/revocation-global", Fixture, NULL, setup, test_revocation_and_global, teardown);
	g_test_add("/federation/grant-validation", Fixture, NULL, setup, test_grant_validation, teardown);
	g_test_add("/federation/updates", Fixture, NULL, setup, test_updates, teardown);
	g_test_add("/federation/offline-merge", Fixture, NULL, setup, test_offline_merge, teardown);
	g_test_add("/federation/hosted-outbound", Fixture, NULL, setup, test_hosted_outbound, teardown);
	g_test_add("/federation/scheduler", Fixture, NULL, setup, test_scheduler, teardown);
	g_test_add("/federation/conflict", Fixture, NULL, setup, test_conflict, teardown);
	g_test_add("/federation/response-proof", Fixture, NULL, setup, test_response_proof, teardown);
	g_test_add("/federation/key-discovery-limits", Fixture, NULL, setup, test_key_and_discovery_limits, teardown);
	g_test_add("/federation/references-uri", Fixture, NULL, setup, test_references_and_uri, teardown);
	g_test_add("/federation/revoked-field-resolution", Fixture, NULL, setup, test_revoked_field_resolution, teardown);
	g_test_add("/federation/restart-lost-ack", Fixture, NULL, setup, test_restart_and_lost_ack, teardown);
	g_test_add("/federation/collection", Fixture, NULL, setup, test_collection, teardown);
	g_test_add("/federation/future-type", Fixture, NULL, setup, test_future_type, teardown);
	g_test_add_func("/federation/upgrade-private", test_upgrade_stays_private);
	return g_test_run();
}
