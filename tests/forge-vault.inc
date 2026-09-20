/* SPDX-License-Identifier: AGPL-3.0-or-later */

/* Rotation must revoke A without borrowing B's account on the same origin. */
static void test_forge_vault_lifecycle(Fixture *fixture, gconstpointer data)
{
	FakeForge *remote = fake_forge_start();
	g_autofree gchar *base = fake_forge_url(remote);
	g_autoptr(VentureForge) first = venture_forge_new(), second = venture_forge_new();
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureIntegrationConnection) a = NULL, b = NULL, rotated = NULL;
	g_autoptr(VentureForgeClient) client = NULL, client_b = NULL;
	g_autoptr(VentureForgeCredentials) lease = NULL;
	g_autoptr(JsonNode) settings = venture_json_parse("{\"token\":\"account-A-token\",\"webhook_secret\":\"01234567890123456789012345678901\"}", NULL);
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *login = NULL;
	gint64 id;
	(void)data;
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(fixture->database), key, &error));
	g_assert_no_error(error);
	g_object_set(other, "name", "Independent organization", "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(other), NULL, &error));
	g_object_set(first, "name", "A", "organization-id", (gint64)1, "base-url", base, "active", TRUE, NULL);
	g_object_set(second, "name", "B", "organization-id", venture_entity_get_id(VENTURE_ENTITY(other)), "base-url", base, "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(first), NULL, &error));
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(second), NULL, &error));
	g_assert_no_error(error);
	id = venture_entity_get_id(VENTURE_ENTITY(first));
	fake_forge_reply(remote, 200, "{\"login\":\"account-a\"}");
	a = venture_forge_settings_configure(fixture->database, id, settings, 0, 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(a);
	client = venture_forge_client_for_database(fixture->database, id, 5, &error);
	g_assert_no_error(error); g_assert_nonnull(client);
	lease = g_object_ref(venture_forge_client_get_credentials(client));
	json_object_set_string_member(json_node_get_object(settings), "token", "account-B-token");
	fake_forge_reply(remote, 200, "{\"login\":\"account-b\"}");
	b = venture_forge_settings_configure(fixture->database, venture_entity_get_id(VENTURE_ENTITY(second)), settings, 0, 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(b);
	client_b = venture_forge_client_for_database(fixture->database, venture_entity_get_id(VENTURE_ENTITY(second)), 5, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_forge_credentials_get_token(venture_forge_client_get_credentials(client_b)), ==, "account-B-token");
	g_assert_true(venture_forge_credentials_check(lease, &error));
	/* Same-account rotation is permitted; an account swap is refused. */
	rotated = venture_forge_settings_configure(fixture->database, id, settings, venture_entity_get_version(VENTURE_ENTITY(a)), venture_entity_get_id(VENTURE_ENTITY(a)), NULL, &error);
	g_assert_null(rotated); g_assert_nonnull(error); g_clear_error(&error);
	g_assert_true(venture_forge_credentials_check(lease, &error));
	fake_forge_reply(remote, 200, "{\"login\":\"account-a\"}");
	json_object_set_string_member(json_node_get_object(settings), "token", "account-A-rotated");
	rotated = venture_forge_settings_configure(fixture->database, id, settings, venture_entity_get_version(VENTURE_ENTITY(a)), venture_entity_get_id(VENTURE_ENTITY(a)), NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(rotated);
	g_assert_false(venture_forge_credentials_check(lease, &error)); g_clear_error(&error);
	login = venture_forge_client_whoami(client, &error);
	g_assert_null(login); g_assert_nonnull(error); g_clear_error(&error);
	g_assert_true(venture_forge_client_check_credentials(client_b, &error));
	g_clear_object(&client); client = venture_forge_client_for_database(fixture->database, id, 5, &error);
	g_assert_no_error(error); g_assert_nonnull(client);
	g_assert_cmpstr(venture_forge_credentials_get_token(venture_forge_client_get_credentials(client)), ==, "account-A-rotated");
	g_assert_true(venture_forge_settings_disconnect(fixture->database, id, venture_entity_get_version(VENTURE_ENTITY(rotated)), venture_entity_get_id(VENTURE_ENTITY(rotated)), NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_forge_client_check_credentials(client, &error)); g_clear_error(&error);
	g_assert_true(venture_forge_client_check_credentials(client_b, &error));
	g_clear_object(&client); client = venture_forge_client_for_database(fixture->database, id, 5, &error);
	g_assert_null(client); g_assert_nonnull(error); g_clear_error(&error);
	fake_forge_stop(remote);
}

typedef struct { VentureForgeClient *client; GAsyncQueue *done; gchar *login; GError *error; } VaultThread;
static gpointer vault_request_thread(gpointer data)
{
	VaultThread *call = data;
	call->login = venture_forge_client_whoami(call->client, &call->error);
	g_async_queue_push(call->done, GINT_TO_POINTER(1));
	return NULL;
}
static gboolean vault_refuse_clear(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	gboolean *refuse_clear = data;
	(void)database; (void)entity; (void)previous;
	if (!*refuse_clear) return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Fixture refusal after encrypted binding save");
	return FALSE;
}
static void test_forge_vault_import_and_worker(Fixture *fixture, gconstpointer data)
{
	FakeForge *remote = fake_forge_start();
	g_autofree gchar *base = fake_forge_url(remote), *sql = NULL;
	g_autoptr(VentureForge) forge = venture_forge_new();
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureForgeClient) client = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = NULL, *secret = NULL;
	gboolean *refuse_clear = g_new(gboolean, 1);
	gint64 id;
	VaultThread call;
	GThread *thread;
	(void)data;
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(fixture->database), key, &error));
	g_object_set(forge, "name", "Legacy", "organization-id", (gint64)1, "base-url", base, "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(forge), NULL, &error));
	g_assert_no_error(error);
	id = venture_entity_get_id(VENTURE_ENTITY(forge));
	sql = g_strdup_printf("UPDATE forges SET token = 'import-token', webhook_secret = '01234567890123456789012345678901' WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_true(venture_database_execute(fixture->database, sql, NULL, &error));
	g_assert_no_error(error);
	client = venture_forge_client_for_database(fixture->database, id, 5, &error);
	g_assert_null(client); g_assert_nonnull(error); g_clear_error(&error);
	fake_forge_reply(remote, 200, "{\"login\":\"import-account\"}");
	*refuse_clear = TRUE;
	venture_database_add_save_validator(fixture->database, VENTURE_TYPE_FORGE, vault_refuse_clear, refuse_clear, g_free);
	g_assert_false(venture_forge_settings_import_legacy(fixture->database, id, NULL, &error));
	g_assert_nonnull(error); g_clear_error(&error);
	binding = venture_forge_settings_find(fixture->database, id, &error);
	g_assert_null(binding); g_assert_nonnull(error); g_clear_error(&error);
	stored = venture_database_get(fixture->database, VENTURE_TYPE_FORGE, id, &error);
	g_object_get(stored, "token", &token, NULL); g_assert_cmpstr(token, ==, "import-token");
	*refuse_clear = FALSE;
	g_assert_true(venture_forge_settings_import_legacy(fixture->database, id, NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&stored); g_clear_pointer(&token, g_free);
	stored = venture_database_get(fixture->database, VENTURE_TYPE_FORGE, id, &error);
	g_object_get(stored, "token", &token, "webhook-secret", &secret, NULL);
	g_assert_true(venture_string_is_empty(token)); g_assert_true(venture_string_is_empty(secret));
	client = venture_forge_client_for_database(fixture->database, id, 5, &error);
	g_assert_no_error(error); g_assert_nonnull(client);
	/* Holding the DB lock must not stall credential-bound HTTP on a worker. */
	g_assert_true(venture_database_begin(fixture->database, &error));
	call.client = client; call.done = g_async_queue_new(); call.login = NULL; call.error = NULL;
	thread = g_thread_new("vault-client", vault_request_thread, &call);
	g_assert_nonnull(g_async_queue_timeout_pop(call.done, 5 * G_USEC_PER_SEC));
	venture_database_rollback(fixture->database);
	g_thread_join(thread); g_async_queue_unref(call.done);
	g_assert_no_error(call.error); g_assert_cmpstr(call.login, ==, "import-account"); g_free(call.login);
	g_assert_true(venture_forge_credentials_revalidate(venture_forge_client_get_credentials(client), fixture->database, &error));
	/* A later configuration write revokes this exact snapshot. */
	g_object_set(stored, "active", FALSE, NULL);
	g_assert_true(venture_database_save(fixture->database, stored, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_forge_client_check_credentials(client, &error)); g_clear_error(&error);
	fake_forge_stop(remote);
}
static void test_forge_vault_clone_boundary(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureForge) forge = venture_forge_new();
	g_autoptr(VentureForgeCredentials) lease = NULL;
	g_autoptr(GError) error = NULL;
	static const gchar *const bad[] = { "https://neighbor.example.com/project.git", "https://git.example.com.evil/project.git",
		"https://git.example.com:444/project.git", "http://git.example.com/project.git", "file:///tmp/repo", "ftp://git.example.com/project.git",
		"https:///project.git", "ssh://user:password@ssh.example.com/project.git", "git@-oProxyCommand:project.git",
		"https://user:password@git.example.com/project.git", "https://git.example.com/project.git?secret=value" };
	guint i;
	(void)data;
	g_object_set(forge, "organization-id", (gint64)1, "name", "Pinned", "base-url", "https://git.example.com", "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(forge), NULL, &error));
	venture_test_forge_bind(fixture->database, VENTURE_ENTITY(forge), "token", "01234567890123456789012345678901");
	lease = venture_forge_credentials_acquire(fixture->database, venture_entity_get_id(VENTURE_ENTITY(forge)), &error);
	g_assert_no_error(error); g_assert_nonnull(lease);
	g_assert_true(venture_forge_credentials_check_clone(lease, "https://git.example.com/project.git", &error));
	g_assert_true(venture_forge_credentials_check_clone(lease, "git@ssh.example.com:project.git", &error));
	g_assert_true(venture_forge_credentials_check_clone(lease, "ssh://git@ssh.example.com/project.git", &error));
	for (i = 0; i < G_N_ELEMENTS(bad); i++)
	{ g_assert_false(venture_forge_credentials_check_clone(lease, bad[i], &error)); g_assert_nonnull(error); g_clear_error(&error); }
	g_object_set(forge, "base-url", "https://neighbor.example.com", NULL);
	g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(forge), NULL, &error));
	g_assert_nonnull(error); g_clear_error(&error);
	g_assert_true(venture_forge_credentials_check(lease, &error));
}
