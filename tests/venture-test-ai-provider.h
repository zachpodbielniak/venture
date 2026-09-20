/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	VentureAiProviderService *service;
	SoupServer *server;
	GPtrArray *paused;
	gchar *directory, *base;
	const gchar *expected_key;
	gint64 first, second, user;
	guint requests;
	gboolean hold, fail;
	VentureAuthPrincipal principal;
} AiFixture;
typedef struct { gboolean done; AiResponse *response; GError *error; } AiCall;
static void ai_fixture_reply(AiFixture *f, SoupServerMessage *message)
{
	const gchar *body = "{\"id\":\"fixture-reply\",\"object\":\"chat.completion\",\"model\":\"fixture-model\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"Fixture answer\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3,\"total_tokens\":10}}";
	soup_server_message_set_status(message, f->fail ? 500 : 200, NULL);
	soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, body, strlen(body));
}
static void ai_fixture_http(SoupServer *server, SoupServerMessage *message, const gchar *path, GHashTable *query, gpointer data)
{
	AiFixture *f = data;
	g_autofree gchar *expected = g_strconcat("Bearer ", f->expected_key, NULL);
	g_autoptr(GBytes) body = soup_message_body_flatten(soup_server_message_get_request_body(message));
	g_autofree gchar *text = g_strndup(g_bytes_get_data(body, NULL), g_bytes_get_size(body));
	g_autoptr(JsonNode) json = venture_json_parse(text, NULL);
	(void)server; (void)query;
	if (!strcmp(path, "/v1/embeddings")) {
		const gchar *embedding = "{\"object\":\"list\",\"model\":\"fixture-model\",\"data\":[{\"object\":\"embedding\",\"index\":0,\"embedding\":[0.6,0.8]}],\"usage\":{\"prompt_tokens\":2,\"total_tokens\":2}}";
		g_assert_cmpstr(soup_message_headers_get_one(soup_server_message_get_request_headers(message), "Authorization"), ==, expected);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(json), "model"), ==, "fixture-model");
		f->requests++;
		soup_server_message_set_status(message, 200, NULL);
		soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, embedding, strlen(embedding));
		return;
	}
	g_assert_cmpstr(path, ==, "/v1/chat/completions");
	g_assert_cmpstr(soup_message_headers_get_one(soup_server_message_get_request_headers(message), "Authorization"), ==, expected);
	g_assert_nonnull(json);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(json), "model"), ==, "fixture-model");
	f->requests++;
	if (f->hold) { soup_server_message_pause(message); g_ptr_array_add(f->paused, g_object_ref(message)); }
	else ai_fixture_reply(f, message);
}
static void ai_fixture_resume(AiFixture *f)
{
	guint i;
	for (i = 0; i < f->paused->len; i++) {
		SoupServerMessage *message = g_ptr_array_index(f->paused, i);
		ai_fixture_reply(f, message); soup_server_message_unpause(message);
	}
	g_ptr_array_set_size(f->paused, 0);
}
static void ai_fixture_setup(AiFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) first = NULL, second = NULL, user = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("fixture-key-32-bytes-for-tests!!!", 32);
	GSList *uris;
	gchar *allowed[2];
	(void)data;
	f->database = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-ai-provider-XXXXXX", &error); g_assert_no_error(error);
	g_object_set(f->config, "state-dir", f->directory, "ai-enabled", TRUE, "ai-allow-loopback", TRUE, NULL);
	f->context = venture_context_new(f->config, f->database);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	f->service = venture_ai_provider_service_get(f->database);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error)); g_assert_no_error(error);
	first = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "First business", "slug", "ai-first", "active", TRUE, NULL);
	second = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Second business", "slug", "ai-second", "active", TRUE, NULL);
	user = g_object_new(VENTURE_TYPE_USER, "username", "ai-platform-owner", "active", TRUE, "role", VENTURE_USER_ROLE_OWNER, NULL);
	g_assert_true(venture_database_save(f->database, first, NULL, &error));
	g_assert_true(venture_database_save(f->database, second, NULL, &error));
	g_assert_true(venture_database_save(f->database, user, NULL, &error)); g_assert_no_error(error);
	f->first = venture_entity_get_id(first); f->second = venture_entity_get_id(second); f->user = venture_entity_get_id(user);
	f->principal.user_id = f->user; f->principal.token_id = 0; f->principal.role = VENTURE_USER_ROLE_OWNER;
	f->principal.name = g_strdup("ai-platform-owner"); f->principal.authenticated = TRUE;
	f->server = soup_server_new(NULL, NULL); f->paused = g_ptr_array_new_with_free_func(g_object_unref);
	soup_server_add_handler(f->server, NULL, ai_fixture_http, f, NULL);
	g_assert_true(soup_server_listen_local(f->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)); g_assert_no_error(error);
	uris = soup_server_get_uris(f->server);
	f->base = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	allowed[0] = f->base; allowed[1] = NULL;
	g_object_set(f->config, "ai-allowed-base-urls", allowed, NULL);
	f->expected_key = "first-fixture-key";
}
static void ai_fixture_teardown(AiFixture *f, gconstpointer data)
{
	(void)data;
	g_assert_cmpuint(f->paused->len, ==, 0);
	soup_server_disconnect(f->server); g_clear_object(&f->server); g_ptr_array_unref(f->paused);
	g_clear_object(&f->context); g_clear_object(&f->config);
	venture_test_accounting_database_cleanup(f->database); g_clear_object(&f->database);
	venture_test_remove_tree(f->directory); g_free(f->directory); g_free(f->base); g_free(f->principal.name);
}
static JsonNode *ai_fixture_settings(AiFixture *f, const gchar *key)
{
	JsonObject *object = json_object_new(); JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	json_object_set_string_member(object, "provider", "openai"); json_object_set_string_member(object, "model", "fixture-model");
	json_object_set_string_member(object, "api_key", key); json_object_set_string_member(object, "base_url", f->base);
	json_node_take_object(node, object); return node;
}
static VentureAiConfiguration *ai_fixture_configure(AiFixture *f, gint64 org, const gchar *key, gint64 quota, gint64 concurrency, gint64 version)
{
	g_autoptr(JsonNode) settings = ai_fixture_settings(f, key);
	g_autoptr(GError) error = NULL;
	VentureAiConfiguration *result = venture_ai_provider_service_configure(f->service, org, "chat", settings, quota, concurrency, version, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(result); return result;
}
static void ai_call_done(GObject *source, GAsyncResult *result, gpointer data)
{
	AiCall *call = data;
	call->response = ai_provider_chat_finish(AI_PROVIDER(source), result, &call->error); call->done = TRUE;
}
static void ai_call_start(AiProvider *provider, AiCall *call)
{
	GList *messages = g_list_append(NULL, ai_message_new_user("Synthetic request with no business data"));
	call->done = FALSE; call->response = NULL; call->error = NULL;
	ai_provider_chat_async(provider, messages, "Reply briefly", 16, NULL, NULL, ai_call_done, call);
	g_list_free_full(messages, g_object_unref);
}
static void ai_call_wait(AiCall *call)
{
	gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
	while (!call->done && g_get_monotonic_time() < deadline) { while (g_main_context_iteration(NULL, FALSE)) { } g_usleep(1000); }
	g_assert_true(call->done);
}
static void ai_call_clear(AiCall *call) { g_clear_object(&call->response); g_clear_error(&call->error); }
static AiProvider *ai_fixture_provider(AiFixture *f, gint64 org)
{
	g_autoptr(GError) error = NULL;
	AiProvider *provider = venture_ai_provider_service_create_provider(f->service, org, "chat", &f->principal, &error);
	g_assert_no_error(error); g_assert_nonnull(provider); return provider;
}
static void test_ai_owned_isolation(AiFixture *f, gconstpointer data)
{
	g_autoptr(VentureAiConfiguration) first = ai_fixture_configure(f, f->first, "first-fixture-key", 100, 2, 0);
	g_autoptr(VentureAiConfiguration) second = ai_fixture_configure(f, f->second, "second-fixture-key", 100, 2, 0);
	g_autoptr(AiProvider) first_provider = ai_fixture_provider(f, f->first), second_provider = ai_fixture_provider(f, f->second);
	g_autoptr(JsonNode) effective = venture_ai_provider_service_effective(f->service, f->first, "chat", NULL);
	g_autofree gchar *json = venture_json_to_string(effective, FALSE);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AI_USAGE);
	g_autoptr(GPtrArray) rows = NULL;
	AiCall call;
	gint64 input, output;
	(void)data;
	g_assert_null(strstr(json, "fixture-key")); g_assert_null(strstr(json, "api_key"));
	ai_call_start(first_provider, &call); ai_call_wait(&call); g_assert_no_error(call.error); g_assert_nonnull(call.response); ai_call_clear(&call);
	f->expected_key = "second-fixture-key";
	ai_call_start(second_provider, &call); ai_call_wait(&call); g_assert_no_error(call.error); g_assert_nonnull(call.response); ai_call_clear(&call);
	venture_query_set_organization(query, f->first); rows = venture_database_find(f->database, query, NULL);
	g_assert_cmpuint(rows->len, ==, 1); g_object_get(g_ptr_array_index(rows, 0), "input-tokens", &input, "output-tokens", &output, NULL);
	g_assert_cmpint(input, ==, 7); g_assert_cmpint(output, ==, 3);
	g_test_message("Two organizations sent distinct explicit credentials; usage attributed seven input and three output tokens to the originating organization");
}
static void test_ai_quota(AiFixture *f, gconstpointer data)
{
	g_autoptr(VentureAiConfiguration) configuration = ai_fixture_configure(f, f->first, "first-fixture-key", 2, 1, 0);
	g_autoptr(AiProvider) provider = ai_fixture_provider(f, f->first), reopened = NULL;
	gint64 deadline;
	AiCall first, refused;
	(void)data;
	f->hold = TRUE; ai_call_start(provider, &first);
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while (!f->paused->len && g_get_monotonic_time() < deadline) { while (g_main_context_iteration(NULL, FALSE)) { } g_usleep(1000); }
	g_assert_cmpuint(f->paused->len, ==, 1);
	ai_call_start(provider, &refused); ai_call_wait(&refused); g_assert_error(refused.error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT); ai_call_clear(&refused);
	g_assert_cmpuint(f->requests, ==, 1); f->hold = FALSE; ai_fixture_resume(f); ai_call_wait(&first); g_assert_no_error(first.error); ai_call_clear(&first);
	ai_call_start(provider, &first); ai_call_wait(&first); g_assert_no_error(first.error); ai_call_clear(&first);
	reopened = ai_fixture_provider(f, f->first);
	ai_call_start(reopened, &refused); ai_call_wait(&refused); g_assert_error(refused.error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT); ai_call_clear(&refused);
	g_assert_cmpuint(f->requests, ==, 2);
	g_test_message("Concurrent request refused before network I/O; completed calls release concurrency, and a newly constructed client still observes the durable monthly quota");
}
static void test_ai_missing_and_rotation(AiFixture *f, gconstpointer data)
{
	g_autoptr(AiProvider) provider = NULL, other = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAiConfiguration) row = NULL, rotated = NULL, disabled = NULL, second = NULL;
	AiCall call;
	(void)data;
	provider = venture_ai_provider_service_create_provider(f->service, f->first, "chat", &f->principal, &error);
	g_assert_null(provider); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG); g_clear_error(&error);
	row = ai_fixture_configure(f, f->first, "first-fixture-key", 100, 2, 0);
	second = ai_fixture_configure(f, f->second, "second-fixture-key", 100, 2, 0);
	provider = ai_fixture_provider(f, f->first); other = ai_fixture_provider(f, f->second);
	rotated = ai_fixture_configure(f, f->first, "rotated-fixture-key", 100, 2, venture_entity_get_version(VENTURE_ENTITY(row)));
	ai_call_start(provider, &call); ai_call_wait(&call); g_assert_error(call.error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT); ai_call_clear(&call);
	g_assert_cmpuint(f->requests, ==, 0);
	f->expected_key = "second-fixture-key"; ai_call_start(other, &call); ai_call_wait(&call); g_assert_no_error(call.error); ai_call_clear(&call);
	disabled = venture_ai_provider_service_select(f->service, f->first, "chat", 0, venture_entity_get_version(VENTURE_ENTITY(rotated)), NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(disabled); g_clear_object(&provider);
	provider = venture_ai_provider_service_create_provider(f->service, f->first, "chat", &f->principal, &error);
	g_assert_null(provider); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}
static void test_ai_grants(AiFixture *f, gconstpointer data)
{
	g_autoptr(JsonNode) settings = ai_fixture_settings(f, "platform-fixture-key");
	g_autoptr(VentureAiPlatformOffer) offer = NULL;
	g_autoptr(VentureAiGrant) grant = NULL, revoked = NULL;
	g_autoptr(VentureAiConfiguration) selected = NULL, wrong = NULL;
	g_autoptr(AiProvider) provider = NULL;
	g_autoptr(GError) error = NULL;
	AiCall call;
	gint64 deadline;
	(void)data;
	offer = venture_ai_provider_service_offer(f->service, f->second, "chat", "Shared fixture AI", settings, 0, 0, NULL, &error); g_assert_no_error(error); g_assert_nonnull(offer);
	grant = venture_ai_provider_service_grant(f->service, f->first, venture_entity_get_id(VENTURE_ENTITY(offer)), TRUE, 10, 1, 0, NULL, &error); g_assert_no_error(error); g_assert_nonnull(grant);
	provider = venture_ai_provider_service_create_provider(f->service, f->first, "chat", &f->principal, &error);
	g_assert_null(provider); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG); g_clear_error(&error);
	wrong = venture_ai_provider_service_select(f->service, f->second, "chat", venture_entity_get_id(VENTURE_ENTITY(grant)), 0, NULL, &error);
	g_assert_null(wrong); g_assert_nonnull(error); g_clear_error(&error);
	selected = venture_ai_provider_service_select(f->service, f->first, "chat", venture_entity_get_id(VENTURE_ENTITY(grant)), 0, NULL, &error); g_assert_no_error(error); g_assert_nonnull(selected);
	provider = ai_fixture_provider(f, f->first); f->expected_key = "platform-fixture-key";
	f->hold = TRUE; ai_call_start(provider, &call);
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while (!f->paused->len && g_get_monotonic_time() < deadline) { while (g_main_context_iteration(NULL, FALSE)) { } g_usleep(1000); }
	g_assert_cmpuint(f->paused->len, ==, 1);
	revoked = venture_ai_provider_service_grant(f->service, f->first, venture_entity_get_id(VENTURE_ENTITY(offer)), FALSE, 10, 1,
		venture_entity_get_version(VENTURE_ENTITY(grant)), NULL, &error); g_assert_no_error(error); g_assert_nonnull(revoked);
	f->hold = FALSE; ai_fixture_resume(f); ai_call_wait(&call);
	g_assert_error(call.error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_assert_null(call.response); ai_call_clear(&call);
	g_assert_false(venture_ai_provider_service_check_provider(provider, &error)); g_assert_nonnull(error);
	g_test_message("A grant alone did not enable AI; explicit selection used platform credentials, and revocation refused a response already in flight");
}
/* A chat model change must never select or rotate the embedding binding. */
static void test_ai_purpose_isolation(AiFixture *f, gconstpointer data)
{
	g_autoptr(VentureAiConfiguration) chat = ai_fixture_configure(f, f->first, "first-fixture-key", 10, 2, 0), embedding = NULL, rotated = NULL;
	g_autoptr(JsonNode) settings = ai_fixture_settings(f, "embedding-fixture-key"), effective = NULL;
	g_autoptr(AiProvider) provider = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	json_object_set_string_member(json_node_get_object(settings), "model", "independent-embedding-model");
	embedding = venture_ai_provider_service_configure(f->service, f->first, "embedding", settings, 20, 1, 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(embedding);
	provider = venture_ai_provider_service_create_provider(f->service, f->first, "embedding", &f->principal, &error);
	g_assert_no_error(error); g_assert_nonnull(provider);
	rotated = ai_fixture_configure(f, f->first, "rotated-chat-key", 10, 2, venture_entity_get_version(VENTURE_ENTITY(chat)));
	g_assert_true(venture_ai_provider_service_check_provider(provider, &error)); g_assert_no_error(error);
	effective = venture_ai_provider_service_effective(f->service, f->first, "embedding", &error);
	g_assert_no_error(error); g_assert_nonnull(effective);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(effective), "model"), ==, "independent-embedding-model");
}
/* Cached platform authority cannot survive disabling its local user. */
static void test_ai_local_revocation(AiFixture *f, gconstpointer data)
{
	g_autoptr(VentureAiConfiguration) config = ai_fixture_configure(f, f->first, "first-fixture-key", 10, 1, 0);
	g_autoptr(AiProvider) provider = ai_fixture_provider(f, f->first);
	g_autoptr(VentureEntity) user = venture_database_get(f->database, VENTURE_TYPE_USER, f->user, NULL);
	g_autoptr(GError) error = NULL;
	AiCall call;
	(void)data;
	g_object_set(user, "active", FALSE, NULL);
	g_assert_true(venture_database_save(f->database, user, NULL, &error)); g_assert_no_error(error);
	ai_call_start(provider, &call); ai_call_wait(&call);
	g_assert_null(call.response); g_assert_error(call.error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_cmpuint(f->requests, ==, 0); ai_call_clear(&call);
}
typedef struct { AiProvider *provider; GThread *database_thread; gint done; guint writes; } AiThreadWitness;
static void ai_thread_saved(VentureDatabase *database, VentureEntity *entity, gboolean created, gpointer data)
{
	AiThreadWitness *witness = data;
	(void)database; (void)created;
	if (!VENTURE_IS_AI_USAGE(entity) && !VENTURE_IS_AI_USAGE_PERIOD(entity)) return;
	g_assert_true(g_thread_self() == witness->database_thread);
	witness->writes++;
}
static gpointer ai_thread_request(gpointer data)
{
	AiThreadWitness *witness = data;
	g_autoptr(GMainContext) context = g_main_context_new();
	AiCall call;
	gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
	g_main_context_push_thread_default(context);
	ai_call_start(witness->provider, &call);
	while (!call.done && g_get_monotonic_time() < deadline) {
		while (g_main_context_iteration(context, FALSE)) { }
		g_usleep(1000);
	}
	g_assert_true(call.done); g_assert_no_error(call.error); g_assert_nonnull(call.response);
	ai_call_clear(&call);
	g_main_context_pop_thread_default(context);
	g_atomic_int_set(&witness->done, 1);
	return NULL;
}
/* Coding may call a provider on its worker; all quota writes must stay on
 * the database owner, including reservation and asynchronous completion. */
static void test_ai_worker_dispatch(AiFixture *f, gconstpointer data)
{
	g_autoptr(VentureAiConfiguration) config = ai_fixture_configure(f, f->first, "first-fixture-key", 10, 1, 0);
	g_autoptr(AiProvider) provider = ai_fixture_provider(f, f->first);
	AiThreadWitness witness;
	GThread *thread;
	gulong handler;
	gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
	(void)data;
	witness.provider = provider; witness.database_thread = g_thread_self(); witness.done = 0; witness.writes = 0;
	handler = g_signal_connect(f->database, "entity-saved", G_CALLBACK(ai_thread_saved), &witness);
	thread = g_thread_new("ai-fixture-worker", ai_thread_request, &witness);
	while (!g_atomic_int_get(&witness.done) && g_get_monotonic_time() < deadline) {
		while (g_main_context_iteration(NULL, FALSE)) { }
		g_usleep(1000);
	}
	g_assert_cmpint(g_atomic_int_get(&witness.done), ==, 1); g_thread_join(thread);
	g_signal_handler_disconnect(f->database, handler);
	g_assert_cmpuint(witness.writes, ==, 3);
	g_test_message("A worker requested AI; reservation, durable usage and completion stayed on the database-owning thread");
}

typedef struct { gboolean done; AiEmbedding *embedding; GError *error; } AiEmbeddingCall;
static void ai_embedding_done(GObject *source, GAsyncResult *result, gpointer data)
{
	AiEmbeddingCall *call = data;
	call->embedding = ai_embedder_embed_finish(AI_EMBEDDER(source), result, &call->error); call->done = TRUE;
}
static void test_ai_embedding_request(AiFixture *f, gconstpointer data)
{
	g_autoptr(JsonNode) settings = ai_fixture_settings(f, "first-fixture-key");
	g_autoptr(VentureAiConfiguration) config = NULL;
	g_autoptr(AiProvider) provider = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AI_USAGE);
	g_autoptr(VentureEntity) usage = NULL;
	const gchar *texts[] = { "A bounded passage", NULL };
	AiEmbeddingCall call;
	AiCall chat;
	gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
	g_autofree gchar *purpose = NULL;
	(void)data;
	config = venture_ai_provider_service_configure(f->service, f->first, "embedding", settings, 10, 1, 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(config);
	provider = venture_ai_provider_service_create_provider(f->service, f->first, "embedding", &f->principal, &error);
	g_assert_no_error(error); g_assert_true(AI_IS_EMBEDDER(provider));
	call.done = FALSE; call.embedding = NULL; call.error = NULL;
	ai_embedder_embed_async(AI_EMBEDDER(provider), texts, "fixture-model", NULL, ai_embedding_done, &call);
	while (!call.done && g_get_monotonic_time() < deadline) { while (g_main_context_iteration(NULL, FALSE)) { } g_usleep(1000); }
	g_assert_true(call.done); g_assert_no_error(call.error); g_assert_nonnull(call.embedding);
	g_assert_cmpuint(ai_embedding_get_dimensions(call.embedding), ==, 2); ai_embedding_unref(call.embedding);
	usage = venture_database_find_one(f->database, query, &error); g_assert_no_error(error); g_assert_nonnull(usage);
	g_object_get(usage, "purpose", &purpose, NULL); g_assert_cmpstr(purpose, ==, "embedding");
	ai_call_start(provider, &chat); ai_call_wait(&chat);
	g_assert_null(chat.response); g_assert_error(chat.error, VENTURE_ERROR, VENTURE_ERROR_CONFIG); ai_call_clear(&chat);
	g_assert_cmpuint(f->requests, ==, 1);
}
/* Paying the invoice does not give an organization's owner administrative
 * control over the platform's shared secret or other tenants' availability. */
static void test_ai_platform_credential_owner(AiFixture *f, gconstpointer data)
{
	g_autoptr(JsonNode) settings = ai_fixture_settings(f, "platform-fixture-key"), resolved = NULL;
	g_autoptr(VentureAiPlatformOffer) offer = venture_ai_provider_service_offer(f->service, f->second, "chat", "Platform fixture", settings, 0, 0, NULL, NULL);
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "billing-owner", "active", TRUE, "role", VENTURE_USER_ROLE_EDITOR, NULL), membership = NULL, connection = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	VentureAuthPrincipal principal;
	g_autofree gchar *billing_name = g_strdup("billing-owner");
	gint64 connection_id = 0, version;
	(void)data;
	g_assert_nonnull(offer);
	g_object_get(offer, "connection-id", &connection_id, NULL);
	connection = venture_database_get(f->database, VENTURE_TYPE_INTEGRATION_CONNECTION, connection_id, NULL);
	g_assert_nonnull(connection); version = venture_entity_get_version(connection);
	g_assert_true(venture_database_save(f->database, user, NULL, &error)); g_assert_no_error(error);
	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", venture_entity_get_id(user), "organization-id", f->second,
		"role", VENTURE_ORGANIZATION_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->database, membership, NULL, &error)); g_assert_no_error(error);
	principal.user_id = venture_entity_get_id(user); principal.token_id = 0; principal.authenticated = TRUE;
	principal.name = billing_name; principal.role = VENTURE_USER_ROLE_EDITOR;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &principal);
	resolved = venture_integration_service_resolve_version(venture_integration_service_get(f->database), f->second, connection_id, version, FALSE, &error);
	g_assert_null(resolved); g_assert_nonnull(error); g_clear_error(&error);
	g_assert_false(venture_integration_service_disable(venture_integration_service_get(f->database), f->second, connection_id, version, NULL, &error));
	g_assert_nonnull(error); g_clear_error(&error);
	g_object_set(connection, "enabled", FALSE, NULL);
	g_assert_false(venture_database_save(f->database, connection, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
	g_clear_object(&scope);
	resolved = venture_integration_service_resolve_version(venture_integration_service_get(f->database), f->second, connection_id, version, FALSE, &error);
	g_assert_no_error(error); g_assert_nonnull(resolved);
}
static void test_ai_deadline(AiFixture *f, gconstpointer data)
{
	g_autoptr(VentureAiConfiguration) config = ai_fixture_configure(f, f->first, "first-fixture-key", 10, 1, 0);
	g_autoptr(AiProvider) provider = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AI_USAGE);
	g_autoptr(VentureEntity) usage = NULL;
	g_autofree gchar *state = NULL;
	AiCall call;
	gint64 started = g_get_monotonic_time();
	(void)data;
	g_object_set(f->config, "ai-provider-deadline-seconds", (gint64)1, NULL);
	provider = ai_fixture_provider(f, f->first); f->hold = TRUE;
	ai_call_start(provider, &call); ai_call_wait(&call);
	g_assert_null(call.response); g_assert_nonnull(call.error);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 5 * G_USEC_PER_SEC);
	usage = venture_database_find_one(f->database, query, NULL); g_assert_nonnull(usage);
	g_object_get(usage, "state", &state, NULL); g_assert_cmpstr(state, ==, "failed");
	f->hold = FALSE; ai_fixture_resume(f); ai_call_clear(&call);
	g_test_message("An unanswered provider request was cancelled within the explicit deadline and retained charged failed usage");
}
static void test_ai_failed_request_no_fallback(AiFixture *f, gconstpointer data)
{
	g_autoptr(VentureAiConfiguration) own = ai_fixture_configure(f, f->first, "first-fixture-key", 1, 1, 0);
	g_autoptr(JsonNode) settings = ai_fixture_settings(f, "platform-fixture-key");
	g_autoptr(VentureAiPlatformOffer) offer = venture_ai_provider_service_offer(f->service, f->second, "chat", "Eligible fallback must not run", settings, 0, 0, NULL, NULL);
	g_autoptr(VentureAiGrant) grant = venture_ai_provider_service_grant(f->service, f->first, venture_entity_get_id(VENTURE_ENTITY(offer)), TRUE, 50, 2, 0, NULL, NULL);
	g_autoptr(AiProvider) provider = ai_fixture_provider(f, f->first);
	AiCall call;
	(void)data;
	g_assert_nonnull(grant); f->fail = TRUE;
	ai_call_start(provider, &call); ai_call_wait(&call); g_assert_null(call.response); g_assert_nonnull(call.error); ai_call_clear(&call);
	g_assert_cmpuint(f->requests, ==, 1);
	f->fail = FALSE;
	ai_call_start(provider, &call); ai_call_wait(&call);
	g_assert_null(call.response); g_assert_error(call.error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT); ai_call_clear(&call);
	g_assert_cmpuint(f->requests, ==, 1);
	g_test_message("Own provider failure consumed quota; an available platform grant was never used as a fallback");
}
static void test_ai_tool_scope(AiFixture *f, gconstpointer data)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(VentureAiService) prototype = venture_ai_service_new_with_provider(f->context, AI_PROVIDER(mock), NULL), bound = NULL;
	g_autoptr(VentureEntity) first = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->first, "name", "FirstPaidOrganizationMarker", NULL);
	g_autoptr(VentureEntity) second = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->second, "name", "SecondPrivateOrganizationMarker", NULL);
	g_autoptr(AiToolUse) query = ai_tool_use_new_from_json_string("scope", "venture_query", "{\"type\":\"company\"}"), get = NULL;
	g_autofree gchar *reply = NULL, *input = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_assert_true(venture_database_save(f->database, first, NULL, &error)); g_assert_true(venture_database_save(f->database, second, NULL, &error)); g_assert_no_error(error);
	bound = venture_ai_service_for_organization(prototype, f->first, &error); g_assert_no_error(error); g_assert_nonnull(bound);
	reply = venture_ai_service_execute_tool(bound, query, &f->principal, &error); g_assert_no_error(error); g_assert_nonnull(reply);
	g_assert_nonnull(strstr(reply, "FirstPaidOrganizationMarker")); g_assert_null(strstr(reply, "SecondPrivateOrganizationMarker")); g_clear_pointer(&reply, g_free);
	input = g_strdup_printf("{\"type\":\"company\",\"id\":%" G_GINT64_FORMAT "}", venture_entity_get_id(second));
	get = ai_tool_use_new_from_json_string("foreign", "venture_get", input);
	reply = venture_ai_service_execute_tool(bound, get, &f->principal, &error);
	g_assert_true(reply == NULL || strstr(reply, "SecondPrivateOrganizationMarker") == NULL);
	g_test_message("A global administrator's paid AI tools saw only the explicitly selected organization's records");
}
typedef struct { AiFixture *fixture; VentureAiGrant *grant; gboolean revoked; } AiReservationRevocation;
static void ai_reservation_revoke(VentureDatabase *database, VentureEntity *entity, gboolean created, gpointer data)
{
	AiReservationRevocation *watch = data;
	g_autoptr(VentureAiGrant) revoked = NULL;
	g_autoptr(GError) error = NULL;
	gint64 offer = 0;
	(void)database;
	if (!created || !VENTURE_IS_AI_USAGE(entity) || watch->revoked) return;
	watch->revoked = TRUE;
	g_object_get(watch->grant, "offer-id", &offer, NULL);
	revoked = venture_ai_provider_service_grant(watch->fixture->service, watch->fixture->first, offer, FALSE, 10, 1,
		venture_entity_get_version(VENTURE_ENTITY(watch->grant)), NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(revoked);
}
static void test_ai_reservation_observer(AiFixture *f, gconstpointer data)
{
	g_autoptr(JsonNode) settings = ai_fixture_settings(f, "platform-fixture-key");
	g_autoptr(VentureAiPlatformOffer) offer = venture_ai_provider_service_offer(f->service, f->second, "chat", "Observer revocation", settings, 0, 0, NULL, NULL);
	g_autoptr(VentureAiGrant) grant = venture_ai_provider_service_grant(f->service, f->first, venture_entity_get_id(VENTURE_ENTITY(offer)), TRUE, 10, 1, 0, NULL, NULL);
	g_autoptr(VentureAiConfiguration) selection = venture_ai_provider_service_select(f->service, f->first, "chat", venture_entity_get_id(VENTURE_ENTITY(grant)), 0, NULL, NULL);
	g_autoptr(AiProvider) provider = ai_fixture_provider(f, f->first);
	AiReservationRevocation watch;
	AiCall call;
	gulong handler;
	(void)data;
	g_assert_nonnull(selection); watch.fixture = f; watch.grant = grant; watch.revoked = FALSE;
	f->expected_key = "platform-fixture-key";
	handler = g_signal_connect(f->database, "entity-saved", G_CALLBACK(ai_reservation_revoke), &watch);
	ai_call_start(provider, &call); ai_call_wait(&call);
	g_assert_true(watch.revoked); g_assert_null(call.response); g_assert_nonnull(call.error);
	g_assert_cmpuint(f->requests, ==, 0); ai_call_clear(&call); g_signal_handler_disconnect(f->database, handler);
	g_test_message("A grant revoked by a reservation observer stopped business input before HTTP dispatch");
}

/* A scoped administrator must not use a service helper to widen the paid
 * organization, even though that administrator normally manages both. */
static void test_ai_service_scope(AiFixture *f, gconstpointer data)
{
	g_autoptr(VentureAiConfiguration) configured = ai_fixture_configure(f, f->second, "second-fixture-key", 10, 1, 0), changed = NULL;
	g_autoptr(VentureAccessScope) scope = venture_access_policy_enter_organization(venture_database_get_access_policy(f->database), &f->principal, f->first);
	g_autoptr(AiProvider) provider = NULL;
	g_autoptr(JsonNode) settings = ai_fixture_settings(f, "refused-key");
	g_autoptr(GError) error = NULL;
	(void)data;
	provider = venture_ai_provider_service_create_provider(f->service, f->second, "chat", &f->principal, &error);
	g_assert_null(provider); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	changed = venture_ai_provider_service_configure(f->service, f->second, "chat", settings, 10, 1,
		venture_entity_get_version(VENTURE_ENTITY(configured)), NULL, &error);
	g_assert_null(changed); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_cmpuint(f->requests, ==, 0);
}

/* Reopening the database must retain both the monthly charge and a crashed
 * request's concurrency lease; only lease expiry releases that slot. */
static void test_ai_restart(AiFixture *f, gconstpointer data)
{
	g_autoptr(VentureAiConfiguration) configured = ai_fixture_configure(f, f->first, "first-fixture-key", 2, 1, 0);
	g_autoptr(AiProvider) provider = ai_fixture_provider(f, f->first);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("fixture-key-32-bytes-for-tests!!!", 32);
	g_autofree gchar *schema = g_strdup(g_object_get_data(G_OBJECT(f->database), "accounting-test-schema"));
	g_autofree gchar *path = g_build_filename(f->directory, "restart.sqlite", NULL), *uri = NULL, *sql = NULL;
	const gchar *postgres = g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI");
	AiCall call;
	(void)data;
	ai_call_start(provider, &call); ai_call_wait(&call); g_assert_no_error(call.error); ai_call_clear(&call);
	/* Represent the durable pre-completion state left by a process crash. */
	g_assert_true(venture_database_execute(f->database, "UPDATE ai_usages SET state = 'reserved'", NULL, &error)); g_assert_no_error(error);
	g_clear_object(&provider); g_clear_object(&configured); g_clear_object(&f->context);
	if (!postgres) {
		sql = g_strdup_printf("VACUUM INTO '%s'", path);
		g_assert_true(venture_database_execute(f->database, sql, NULL, &error)); g_assert_no_error(error);
		g_clear_pointer(&sql, g_free); uri = g_strconcat("sqlite://", path, NULL);
	} else uri = g_strdup(postgres);
	g_clear_object(&f->database);
	f->database = venture_database_new(uri, &error); g_assert_no_error(error); g_assert_nonnull(f->database);
	if (schema) {
		sql = g_strdup_printf("SET search_path TO %s", schema);
		g_assert_true(venture_database_execute(f->database, sql, NULL, &error)); g_assert_no_error(error);
		g_object_set_data_full(G_OBJECT(f->database), "accounting-test-schema", g_strdup(schema), g_free);
	}
	f->context = venture_context_new(f->config, f->database);
	f->service = venture_ai_provider_service_get(f->database);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error)); g_assert_no_error(error);
	provider = ai_fixture_provider(f, f->first);
	ai_call_start(provider, &call); ai_call_wait(&call); g_assert_error(call.error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT); ai_call_clear(&call);
	g_assert_cmpuint(f->requests, ==, 1);
	g_assert_true(venture_database_execute(f->database, "UPDATE ai_usages SET lease_until = '2000-01-01T00:00:00Z'", NULL, &error)); g_assert_no_error(error);
	ai_call_start(provider, &call); ai_call_wait(&call); g_assert_no_error(call.error); ai_call_clear(&call);
	ai_call_start(provider, &call); ai_call_wait(&call); g_assert_error(call.error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT); ai_call_clear(&call);
	g_assert_cmpuint(f->requests, ==, 2);
	g_test_message("Database reopen retained the crashed reservation and monthly charge; expiry released concurrency without refunding quota");
}
