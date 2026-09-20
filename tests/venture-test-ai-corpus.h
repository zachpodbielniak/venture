/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
typedef struct { GMutex mutex; GCond condition; GMainContext *context; GMainLoop *loop; gboolean ready; gint port; } AiCorpusServer;
static void ai_corpus_http(SoupServer *server, SoupServerMessage *message, const gchar *path, GHashTable *query, gpointer data)
{
	g_autoptr(GBytes) bytes = soup_message_body_flatten(soup_server_message_get_request_body(message));
	g_autofree gchar *text = g_strndup(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
	g_autoptr(JsonNode) request = venture_json_parse(text, NULL);
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *encoded = NULL;
	JsonObject *object = json_node_get_object(request);
	JsonArray *inputs = json_object_get_array_member(object, "input");
	guint i;
	(void)server; (void)query; (void)data;
	g_assert_cmpstr(path, ==, "/v1/embeddings");
	g_assert_cmpstr(soup_message_headers_get_one(soup_server_message_get_request_headers(message), "Authorization"), ==, "Bearer corpus-fixture-key");
	json_builder_begin_object(builder); json_builder_set_member_name(builder, "model"); json_builder_add_string_value(builder, json_object_get_string_member(object, "model"));
	json_builder_set_member_name(builder, "data"); json_builder_begin_array(builder);
	for (i = 0; i < json_array_get_length(inputs); i++) {
		json_builder_begin_object(builder); json_builder_set_member_name(builder, "index"); json_builder_add_int_value(builder, i);
		json_builder_set_member_name(builder, "embedding"); json_builder_begin_array(builder); json_builder_add_double_value(builder, 0.6); json_builder_add_double_value(builder, 0.8);
		json_builder_end_array(builder); json_builder_end_object(builder);
	}
	json_builder_end_array(builder); json_builder_end_object(builder);
	response = json_builder_get_root(builder); encoded = venture_json_to_string(response, FALSE);
	soup_server_message_set_status(message, 200, NULL);
	soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, encoded, strlen(encoded));
}
static gpointer ai_corpus_server_run(gpointer data)
{
	AiCorpusServer *state = data;
	g_autoptr(SoupServer) server = NULL;
	g_autoptr(GError) error = NULL;
	GSList *uris;
	g_main_context_push_thread_default(state->context);
	server = soup_server_new(NULL, NULL); soup_server_add_handler(server, NULL, ai_corpus_http, NULL, NULL);
	g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)); g_assert_no_error(error);
	uris = soup_server_get_uris(server);
	g_mutex_lock(&state->mutex); state->port = g_uri_get_port(uris->data); state->ready = TRUE; g_cond_signal(&state->condition); g_mutex_unlock(&state->mutex);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	g_main_loop_run(state->loop); soup_server_disconnect(server);
	g_clear_object(&server); g_main_context_pop_thread_default(state->context);
	return NULL;
}
static gboolean ai_corpus_server_stop(gpointer data) { g_main_loop_quit(data); return G_SOURCE_REMOVE; }
/* Real HTTP vectors flow through encrypted organization selection into the
 * persisted corpus. Chat rotation cannot change the corpus's vector space. */
static void test_ai_corpus_scope(AiFixture *f, gconstpointer data)
{
	AiCorpusServer server;
	GThread *thread;
	g_autofree gchar *base_url = NULL, *model = NULL;
	gchar *allowed[2];
	g_autoptr(JsonNode) settings = ai_fixture_settings(f, "corpus-fixture-key");
	g_autoptr(VentureAiConfiguration) binding = NULL, rotated = NULL;
	g_autoptr(VentureEntity) base = g_object_new(VENTURE_TYPE_KNOWLEDGE_BASE, "name", "Scoped corpus", "slug", "scoped-corpus", "organization-id", f->first, NULL);
	g_autoptr(VentureEntity) other = g_object_new(VENTURE_TYPE_KNOWLEDGE_BASE, "name", "Other corpus", "slug", "other-corpus", "organization-id", f->second, NULL), article = NULL, current = NULL;
	g_autoptr(VentureKbService) service = NULL;
	g_autoptr(GPtrArray) hits = NULL;
	g_autoptr(GError) error = NULL;
	gint64 ids[2];
	(void)data;
	g_mutex_init(&server.mutex); g_cond_init(&server.condition); server.context = g_main_context_new(); server.loop = g_main_loop_new(server.context, FALSE); server.ready = FALSE; server.port = 0;
	thread = g_thread_new("ai-corpus-fixture", ai_corpus_server_run, &server);
	g_mutex_lock(&server.mutex);
	while (!server.ready) g_assert_true(g_cond_wait_until(&server.condition, &server.mutex, g_get_monotonic_time() + 10 * G_USEC_PER_SEC));
	g_mutex_unlock(&server.mutex);
	base_url = g_strdup_printf("http://127.0.0.1:%d", server.port); allowed[0] = base_url; allowed[1] = NULL;
	g_object_set(f->config, "ai-allowed-base-urls", allowed, NULL);
	json_object_set_string_member(json_node_get_object(settings), "base_url", base_url);
	binding = venture_ai_provider_service_configure(f->service, f->first, "embedding", settings, 30, 2, 0, NULL, &error); g_assert_no_error(error); g_assert_nonnull(binding);
	g_assert_true(venture_database_save(f->database, base, NULL, &error)); g_assert_true(venture_database_save(f->database, other, NULL, &error)); g_assert_no_error(error);
	ids[0] = venture_entity_get_id(base); ids[1] = venture_entity_get_id(other);
	article = g_object_new(VENTURE_TYPE_KB_ARTICLE, "organization-id", f->first, "kb-id", ids[0], "slug", "cashbook", "title", "Cashbook", "body", "Reconcile the bank statement with the cashbook.", "status", VENTURE_KB_ARTICLE_STATUS_PUBLISHED, NULL);
	g_assert_true(venture_database_save(f->database, article, NULL, &error)); g_assert_no_error(error);
	service = venture_kb_service_new(f->context, &error); g_assert_no_error(error); g_assert_nonnull(service);
	g_assert_cmpint(venture_kb_service_index_article(service, venture_entity_get_id(article), NULL, &error), ==, 1); g_assert_no_error(error);
	hits = venture_kb_service_search(service, "cashbook", ids, 1, 3, &error); g_assert_no_error(error); g_assert_cmpuint(hits->len, ==, 1); g_clear_pointer(&hits, g_ptr_array_unref);
	/* Independently corrupted provenance at either level must refuse a
	 * plausible matching vector rather than silently scoring it. */
	current = venture_database_get(f->database, VENTURE_TYPE_KB_ARTICLE, venture_entity_get_id(article), NULL);
	g_object_set(current, "embedding-model", "different-space", NULL);
	g_assert_true(venture_database_save(f->database, current, NULL, &error)); g_assert_no_error(error);
	hits = venture_kb_service_search(service, "cashbook", ids, 1, 3, &error); g_assert_no_error(error); g_assert_cmpuint(hits->len, ==, 0); g_clear_pointer(&hits, g_ptr_array_unref);
	g_object_set(current, "embedding-model", "fixture-model", NULL);
	g_assert_true(venture_database_save(f->database, current, NULL, &error)); g_assert_no_error(error); g_clear_object(&current);
	current = venture_database_get(f->database, VENTURE_TYPE_KNOWLEDGE_BASE, ids[0], NULL);
	g_object_set(current, "embedding-model", "different-space", NULL);
	g_assert_true(venture_database_save(f->database, current, NULL, &error)); g_assert_no_error(error);
	hits = venture_kb_service_search(service, "cashbook", ids, 1, 3, &error); g_assert_no_error(error); g_assert_cmpuint(hits->len, ==, 0); g_clear_pointer(&hits, g_ptr_array_unref);
	g_object_set(current, "embedding-model", "fixture-model", NULL);
	g_assert_true(venture_database_save(f->database, current, NULL, &error)); g_assert_no_error(error); g_clear_object(&current);
	hits = venture_kb_service_search(service, "cashbook", ids, 2, 3, &error); g_assert_null(hits); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	json_object_set_string_member(json_node_get_object(settings), "model", "changed-embedding-model");
	rotated = venture_ai_provider_service_configure(f->service, f->first, "embedding", settings, 30, 2, venture_entity_get_version(VENTURE_ENTITY(binding)), NULL, &error); g_assert_no_error(error);
	g_assert_cmpint(venture_kb_service_index_article(service, venture_entity_get_id(article), NULL, &error), ==, -1); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT); g_clear_error(&error);
	g_assert_cmpint(venture_kb_service_reindex(service, ids[0], TRUE, NULL, &error), ==, 1); g_assert_no_error(error);
	current = venture_database_get(f->database, VENTURE_TYPE_KB_ARTICLE, venture_entity_get_id(article), &error); g_assert_no_error(error);
	g_object_get(current, "embedding-model", &model, NULL); g_assert_cmpstr(model, ==, "changed-embedding-model");
	g_main_context_invoke(server.context, ai_corpus_server_stop, server.loop); g_thread_join(thread);
	g_main_loop_unref(server.loop); g_main_context_unref(server.context); g_cond_clear(&server.condition); g_mutex_clear(&server.mutex);
	g_test_message("Organization embedding HTTP request indexed and searched a corpus; cross-organization search was refused and model rotation required explicit forced reindex");
}
