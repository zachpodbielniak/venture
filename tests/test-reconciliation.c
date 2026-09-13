#include <venture.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

VENTURE_DECLARE_ENTITY(VentureMatchFixture, venture_match_fixture, MATCH_FIXTURE)
static const VentureFieldDecl fixture_fields[] = {
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD("reference", "Reference", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureMatchFixture, venture_match_fixture, fixture_fields)

static VentureEntity *
record(gint64 id, gint64 amount, const gchar *currency, gint day, const gchar *description)
{
	g_autoptr(VentureMoney) money = venture_money_new(amount, currency, 2);
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 9, day, 0, 0, 0);
	return g_object_new(venture_match_fixture_get_type(), "id", id,
		"organization-id", (gint64)1, "amount", money, "date", date,
		"description", description, NULL);
}
static void
check_score(gint day, gint64 amount, const gchar *currency, const gchar *description,
	gint expected, gboolean duplicate)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureExactMatcher) matcher = venture_exact_matcher_new();
	g_autoptr(VentureEntity) transaction = record(1, 10000, "USD", 5, "Coffee SUPPLIER");
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	g_autoptr(GError) error = NULL;
	g_ptr_array_add(candidates, record(2, amount, currency, day, description));
	if (duplicate)
		g_ptr_array_add(candidates, record(3, amount, currency, day, description));
	result = venture_reconciliation_matcher_suggest(VENTURE_RECONCILIATION_MATCHER(matcher), db, transaction, candidates, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(result->len, ==, expected ? (duplicate ? 2 : 1) : 0);
	if (expected)
	{
		gint score;
		g_object_get(g_ptr_array_index(result, 0), "confidence", &score, NULL);
		g_assert_cmpint(score, ==, expected);
	}
}
static void test_exact_unique(void) { check_score(8, 10000, "USD", "", 100, FALSE); }
static void test_exact_ambiguous(void) { check_score(8, 10000, "USD", "", 70, TRUE); }
static void test_exact_ten_days(void) { check_score(15, 10000, "USD", "", 70, FALSE); }
static void test_exact_outside_date(void) { check_score(16, 10000, "USD", "", 0, FALSE); }
static void test_exact_partial(void) { check_score(20, 10200, "USD", "coffee beans", 40, FALSE); }
static void test_exact_outside_amount(void) { check_score(20, 10201, "USD", "coffee beans", 0, FALSE); }
static void test_exact_no_overlap(void) { check_score(20, 10200, "USD", "unrelated", 0, FALSE); }
static void test_exact_currency(void) { check_score(5, 10000, "EUR", "Coffee", 0, FALSE); }
static void
test_exact_missing(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureExactMatcher) matcher = venture_exact_matcher_new();
	g_autoptr(VentureEntity) transaction = VENTURE_ENTITY(venture_contact_new());
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	g_ptr_array_add(candidates, record(2, 10000, "USD", 5, "Coffee"));
	result = venture_reconciliation_matcher_suggest(VENTURE_RECONCILIATION_MATCHER(matcher), db, transaction, candidates, NULL, NULL);
	g_assert_cmpuint(result->len, ==, 0);
}

static void
test_registry(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureReconciliationRegistry) registry = venture_reconciliation_registry_new();
	g_autoptr(VentureEntity) transaction = record(1, 10000, "USD", 5, "coffee");
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	g_autoptr(GPtrArray) list = NULL;
	gint score;
	venture_reconciliation_registry_add(registry, VENTURE_RECONCILIATION_MATCHER(venture_exact_matcher_new()));
	venture_reconciliation_registry_add(registry, VENTURE_RECONCILIATION_MATCHER(venture_exact_matcher_new()));
	list = venture_reconciliation_registry_list(registry);
	g_assert_cmpuint(list->len, ==, 1);
	g_ptr_array_add(candidates, record(3, 10200, "USD", 20, "coffee"));
	g_ptr_array_add(candidates, record(2, 10000, "USD", 5, ""));
	g_ptr_array_add(candidates, record(3, 10200, "USD", 20, "coffee"));
	result = venture_reconciliation_registry_suggest_all(registry, db, transaction, candidates, NULL, NULL);
	g_assert_cmpuint(result->len, ==, 2);
	g_object_get(g_ptr_array_index(result, 0), "confidence", &score, NULL);
	g_assert_cmpint(score, ==, 100);
	g_assert_true(venture_reconciliation_registry_remove(registry, "exact"));
	g_assert_null(venture_reconciliation_registry_lookup(registry, "exact"));
}
static void
async_done(GObject *source, GAsyncResult *result, gpointer data)
{
	GPtrArray **output = data;
	*output = venture_reconciliation_matcher_suggest_finish(VENTURE_RECONCILIATION_MATCHER(source), result, NULL);
}
static void
test_async(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureExactMatcher) matcher = venture_exact_matcher_new();
	g_autoptr(VentureEntity) transaction = record(1, 10000, "USD", 5, "");
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	g_autoptr(GError) error = NULL;
	g_ptr_array_add(candidates, record(2, 10000, "USD", 5, ""));
	venture_reconciliation_matcher_suggest_async(VENTURE_RECONCILIATION_MATCHER(matcher), db, transaction, candidates, NULL, async_done, &result);
	g_assert_null(result);
	while (result == NULL) g_main_context_iteration(NULL, TRUE);
	g_assert_cmpuint(result->len, ==, 1);
	g_clear_pointer(&result, g_ptr_array_unref);
	g_cancellable_cancel(cancel);
	result = venture_reconciliation_matcher_suggest(VENTURE_RECONCILIATION_MATCHER(matcher), db, transaction, candidates, cancel, &error);
	g_assert_null(result);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

#define TYPE_CANNED_PROVIDER (canned_provider_get_type())
G_DECLARE_FINAL_TYPE(CannedProvider, canned_provider, CANNED, PROVIDER, GObject)
struct _CannedProvider { GObject parent_instance; gchar *answer; gchar *prompt; gchar *tool_input; guint calls; };
static void canned_iface(AiProviderInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(CannedProvider, canned_provider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER, canned_iface))
static void canned_free(GObject *object)
{
	CannedProvider *self = CANNED_PROVIDER(object);
	g_free(self->answer); g_free(self->prompt); g_free(self->tool_input);
	G_OBJECT_CLASS(canned_provider_parent_class)->finalize(object);
}
static void canned_provider_class_init(CannedProviderClass *klass) { G_OBJECT_CLASS(klass)->finalize = canned_free; }
static void canned_provider_init(CannedProvider *self) { (void)self; }
static void
canned_chat(AiProvider *provider, GList *messages, const gchar *system_prompt, gint max_tokens,
	GList *offered_tools, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
	CannedProvider *self = CANNED_PROVIDER(provider);
	g_autoptr(GTask) task = g_task_new(provider, cancellable, callback, data);
	AiResponse *response = ai_response_new("fixture", "fixture");
	g_autoptr(AiTextContent) text = ai_text_content_new(self->answer);
	(void)max_tokens;
	if (self->tool_input != NULL && self->calls == 0)
	{
		GList *item;
		gboolean found = FALSE;
		AiToolUse *call;
		for (item = offered_tools; item != NULL; item = item->next)
			if (g_str_equal(ai_tool_get_name(item->data), "venture_reconcile_suggest")) found = TRUE;
		g_assert_true(found);
		call = ai_tool_use_new_from_json_string("call-1", "venture_reconcile_suggest", self->tool_input);
		ai_response_add_content_block(response, AI_CONTENT_BLOCK(call));
		ai_response_set_stop_reason(response, AI_STOP_REASON_TOOL_USE);
		self->calls++;
		g_task_return_pointer(task, response, g_object_unref);
		return;
	}
	if (self->tool_input == NULL)
	{
		g_assert_null(offered_tools);
		g_assert_nonnull(strstr(system_prompt, "JSON"));
	}
	g_free(self->prompt);
	self->prompt = ai_message_get_text(messages->data);
	self->calls++;
	ai_response_add_content_block(response, AI_CONTENT_BLOCK(g_steal_pointer(&text)));
	g_task_return_pointer(task, response, g_object_unref);
}
static AiResponse *canned_finish(AiProvider *provider, GAsyncResult *result, GError **error)
{ (void)provider; return g_task_propagate_pointer(G_TASK(result), error); }
static const gchar *canned_name(AiProvider *provider) { (void)provider; return "fixture"; }
static AiProviderType canned_type(AiProvider *provider) { (void)provider; return AI_PROVIDER_OPENAI; }
static void canned_iface(AiProviderInterface *iface)
{
	iface->chat_async = canned_chat; iface->chat_finish = canned_finish;
	iface->get_name = canned_name; iface->get_default_model = canned_name;
	iface->get_provider_type = canned_type;
}
static void
test_ai_answer(gconstpointer data)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, db);
	g_autoptr(CannedProvider) provider = g_object_new(TYPE_CANNED_PROVIDER, NULL);
	g_autoptr(VentureAiService) ai = NULL;
	g_autoptr(VentureAiMatcher) matcher = NULL;
	g_autoptr(VentureEntity) transaction = record(1, 10000, "USD", 5, "coffee");
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	provider->answer = g_strdup(data);
	ai = venture_ai_service_new_with_provider(context, AI_PROVIDER(provider), &error);
	g_assert_no_error(error);
	matcher = venture_ai_matcher_new(ai);
	for (i = 2; i < 24; i++)
		g_ptr_array_add(candidates, record(i, 10000, "USD", 5, i == 22 ? "OMITTED_CANDIDATE" : "coffee"));
	result = venture_reconciliation_matcher_suggest(VENTURE_RECONCILIATION_MATCHER(matcher), db, transaction, candidates, NULL, &error);
	g_assert_cmpuint(provider->calls, ==, 1);
	g_assert_null(strstr(provider->prompt, "OMITTED_CANDIDATE"));
	if (g_str_has_prefix(data, "[{\"id\":2,"))
	{
		gint confidence;
		g_assert_no_error(error);
		g_assert_cmpuint(result->len, ==, 1);
		g_object_get(g_ptr_array_index(result, 0), "confidence", &confidence, NULL);
		g_assert_cmpint(confidence, ==, strstr(data, "-20") != NULL ? 0 : 100);
	}
	else
	{
		g_assert_null(result);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_AI);
	}
}

VENTURE_DECLARE_ENTITY(VentureBankMatch, venture_bank_match, BANK_MATCH)
static const VentureFieldDecl match_fields[] = {
	VENTURE_FIELD_REF("transaction-id", "Transaction", NULL, "match_fixture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("target-type", "Target type", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("target-id", "Target", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL)
};
VENTURE_DEFINE_ENTITY(VentureBankMatch, venture_bank_match, match_fields)

static void
test_staging(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, db);
	g_autoptr(VentureEntity) transaction = record(0, 10000, "USD", 5, "coffee");
	g_autoptr(VentureEntity) candidate = record(0, 10000, "USD", 5, "coffee");
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GPtrArray) pending = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	VentureReconciliationService *service;
	VentureConfirmationStore *store = venture_context_get_confirmations(context);
	VentureActor origin;
	const gchar *type;
	origin.kind = VENTURE_ACTOR_KIND_USER;
	origin.name = "tester";
	origin.prompt = NULL;
	origin.request_id = NULL;
	origin.approved_by = NULL;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(db, transaction, &origin, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(db, candidate, &origin, &error));
	g_assert_no_error(error);
	type = venture_entity_get_entity_name(transaction);
	service = venture_context_get_reconciliation_service(context);
	g_assert_nonnull(service);
	g_assert_true(venture_context_get_reconciliation_registry(context) == venture_context_get_reconciliation_registry(context));
	result = venture_reconciliation_service_suggest(service, type, venture_entity_get_id(transaction), "exact", 80, &origin, "test", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(result), "suggestions")), ==, 1);
	pending = venture_confirmation_store_list_pending(store);
	g_assert_cmpuint(pending->len, ==, 0);
	g_clear_pointer(&pending, g_ptr_array_unref);
	g_clear_pointer(&result, json_node_unref);
	g_assert_true(venture_entity_registry_register(venture_entity_registry_get_default(), venture_bank_match_get_type(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	result = venture_reconciliation_service_suggest(service, type, venture_entity_get_id(transaction), "exact", 80, &origin, "test", &error);
	g_assert_no_error(error);
	pending = venture_confirmation_store_list_pending(store);
	g_assert_cmpuint(pending->len, ==, 1);
	query = venture_query_new(venture_bank_match_get_type());
	g_assert_cmpint(venture_database_count(db, query, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_true(venture_confirmation_store_approve(store, venture_confirmation_get_id(g_ptr_array_index(pending, 0)), "operator", &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_database_count(db, query, &error), ==, 1);
	g_assert_no_error(error);
	venture_config_set_module_enabled(config, "reconciliation", FALSE);
	g_assert_null(venture_context_get_reconciliation_service(context));
}

typedef struct { gboolean done; gchar *out; gchar *err; GError *error; } ReconcileCliResult;
static void
reconcile_cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	ReconcileCliResult *reply = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &reply->out, &reply->err, &reply->error);
	reply->done = TRUE;
}
static void
check_cli(guint port, VentureEntity *transaction)
{
	g_autofree gchar *base = g_strdup_printf("http://127.0.0.1:%u", port);
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(transaction));
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	ReconcileCliResult result = { FALSE, NULL, NULL, NULL };
	const gchar *args[] = { "build/debug/venturectl", "--server", base, "reconcile", "suggest",
		venture_entity_get_entity_name(transaction), id, "--matcher", "exact", "--threshold", "80", NULL };
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	child = g_subprocess_launcher_spawnv(launcher, args, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, reconcile_cli_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	g_test_message("CLI stderr: %s", result.err);
	g_assert_true(g_subprocess_get_successful(child));
	g_assert_nonnull(strstr(result.out, "confidence"));
	g_free(result.out); g_free(result.err);
}

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	GBytes **body = data;
	g_autoptr(GError) error = NULL;
	*body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
	g_assert_no_error(error);
}
static void
test_http(gconstpointer data)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	g_autoptr(VentureEntity) transaction = record(0, 10000, "USD", 5, "coffee");
	g_autoptr(VentureEntity) candidate = record(0, 10000, "USD", 5, "coffee");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = g_dir_make_tmp("venture-reconciliation-XXXXXX", NULL);
	g_autofree gchar *url = NULL;
	g_autofree gchar *payload = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GBytes) response = NULL;
	g_autoptr(GPtrArray) pending = NULL;
	guint port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	gboolean stage = GPOINTER_TO_INT(data);
	g_assert_no_error(error);
	g_clear_object(&probe);
	if (stage && venture_entity_registry_lookup(venture_entity_registry_get_default(), "bank_match") == G_TYPE_INVALID)
		g_assert_true(venture_entity_registry_register(venture_entity_registry_get_default(), venture_bank_match_get_type(), NULL));
	g_object_set(config, "state-dir", state, "server-bind-address", "127.0.0.1", "server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(db, transaction, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(db, candidate, NULL, &error));
	g_assert_no_error(error);
	server = venture_web_server_new(context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	url = g_strdup_printf("http://127.0.0.1:%u/api/v1/reconciliation/suggest", port);
	payload = g_strdup_printf("{\"type\":\"%s\",\"id\":%" G_GINT64_FORMAT "}", venture_entity_get_entity_name(transaction), venture_entity_get_id(transaction));
	message = soup_message_new("POST", url);
	bytes = g_bytes_new(payload, strlen(payload));
	soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &response);
	while (response == NULL) g_main_context_iteration(NULL, TRUE);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	pending = venture_confirmation_store_list_pending(venture_context_get_confirmations(context));
	g_assert_cmpuint(pending->len, ==, stage ? 1 : 0);
	if (stage) check_cli(port, transaction);
	venture_web_server_stop(server);
	g_clear_object(&server);
	g_clear_object(&context);
	g_clear_object(&db);
	g_clear_object(&config);
	venture_test_remove_tree(state);
}

static void
test_assistant_stages(gconstpointer data)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, db);
	g_autoptr(VentureEntity) transaction = record(0, 10000, "USD", 5, "coffee");
	g_autoptr(VentureEntity) candidate = record(0, 10000, "USD", 5, "coffee");
	g_autoptr(CannedProvider) provider = g_object_new(TYPE_CANNED_PROVIDER, NULL);
	g_autoptr(VentureAiService) ai = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) pending = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *answer = NULL;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "bank_match") == G_TYPE_INVALID)
		g_assert_true(venture_entity_registry_register(venture_entity_registry_get_default(), venture_bank_match_get_type(), NULL));
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(db, transaction, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(db, candidate, NULL, &error));
	g_assert_no_error(error);
	g_object_set(config, "ai-policy", GPOINTER_TO_INT(data), NULL);
	provider->answer = g_strdup("Awaiting operator approval");
	provider->tool_input = g_strdup_printf("{\"type\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"matcher\":\"exact\"}", venture_entity_get_entity_name(transaction), venture_entity_get_id(transaction));
	ai = venture_ai_service_new_with_provider(context, AI_PROVIDER(provider), &error);
	g_assert_no_error(error);
	answer = venture_ai_service_answer(ai, "Suggest the matching record", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(answer);
	g_assert_cmpuint(provider->calls, ==, 2);
	pending = venture_confirmation_store_list_pending(venture_context_get_confirmations(context));
	g_assert_cmpuint(pending->len, ==, 1);
	query = venture_query_new(venture_bank_match_get_type());
	g_assert_cmpint(venture_database_count(db, query, &error), ==, 0);
	g_assert_no_error(error);
}

static void
test_exact_duplicate_identity(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureExactMatcher) matcher = venture_exact_matcher_new();
	g_autoptr(VentureEntity) transaction = record(1, 10000, "USD", 5, "");
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	gint confidence;
	g_ptr_array_add(candidates, record(2, 10000, "USD", 5, ""));
	g_ptr_array_add(candidates, record(2, 10000, "USD", 5, ""));
	result = venture_reconciliation_matcher_suggest(VENTURE_RECONCILIATION_MATCHER(matcher), db, transaction, candidates, NULL, NULL);
	g_assert_cmpuint(result->len, >, 0);
	g_object_get(g_ptr_array_index(result, 0), "confidence", &confidence, NULL);
	g_assert_cmpint(confidence, ==, 100);
}

static void
test_registry_merges_matchers(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, db);
	g_autoptr(CannedProvider) provider = g_object_new(TYPE_CANNED_PROVIDER, NULL);
	g_autoptr(VentureAiService) ai = NULL;
	g_autoptr(VentureReconciliationRegistry) registry = venture_reconciliation_registry_new();
	g_autoptr(VentureEntity) transaction = record(1, 10000, "USD", 5, "coffee");
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	g_autoptr(GError) error = NULL;
	gint confidence;
	provider->answer = g_strdup("[{\"id\":3,\"confidence\":95,\"why\":\"reference agrees\"}]");
	ai = venture_ai_service_new_with_provider(context, AI_PROVIDER(provider), &error);
	g_assert_no_error(error);
	venture_reconciliation_registry_add(registry, VENTURE_RECONCILIATION_MATCHER(venture_exact_matcher_new()));
	venture_reconciliation_registry_add(registry, VENTURE_RECONCILIATION_MATCHER(venture_ai_matcher_new(ai)));
	g_ptr_array_add(candidates, record(2, 10000, "USD", 5, "coffee"));
	g_ptr_array_add(candidates, record(3, 10200, "USD", 20, "coffee"));
	result = venture_reconciliation_registry_suggest_all(registry, db, transaction, candidates, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(result->len, ==, 2);
	g_object_get(g_ptr_array_index(result, 0), "confidence", &confidence, NULL);
	g_assert_cmpint(confidence, ==, 100);
	g_object_get(g_ptr_array_index(result, 1), "confidence", &confidence, NULL);
	g_assert_cmpint(confidence, ==, 95);
	g_assert_cmpuint(provider->calls, ==, 1);
}
static void
test_scope_and_extremes(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureExactMatcher) matcher = venture_exact_matcher_new();
	g_autoptr(VentureEntity) transaction = record(1, G_MININT64, "USD", 5, "coffee");
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	VentureEntity *foreign = record(4, G_MININT64, "USD", 5, "coffee");
	g_object_set(foreign, "organization-id", (gint64)2, NULL);
	g_ptr_array_add(candidates, foreign);
	g_ptr_array_add(candidates, g_object_ref(transaction));
	g_ptr_array_add(candidates, record(2, G_MAXINT64, "USD", 5, "coffee"));
	g_ptr_array_add(candidates, record(3, G_MININT64 + 1, "USD", 20, "coffee"));
	result = venture_reconciliation_matcher_suggest(VENTURE_RECONCILIATION_MATCHER(matcher), db, transaction, candidates, NULL, NULL);
	g_assert_cmpuint(result->len, ==, 1);
}

static void
test_service_refuses_absent_source(gconstpointer data)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, db);
	g_autoptr(VentureEntity) transaction = record(0, 10000, "USD", 5, "coffee");
	g_autoptr(VentureEntity) candidate = record(0, 10000, "USD", 5, "coffee");
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	gint64 id = 999;
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER; actor.name = "tester";
	actor.prompt = NULL; actor.request_id = NULL; actor.approved_by = NULL;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	if (GPOINTER_TO_INT(data))
	{
		g_assert_true(venture_database_save(db, transaction, &actor, &error));
		g_assert_no_error(error);
		g_assert_true(venture_database_save(db, candidate, &actor, &error));
		g_assert_no_error(error);
		id = venture_entity_get_id(transaction);
		g_assert_true(venture_database_delete(db, transaction, &actor, &error));
		g_assert_no_error(error);
	}
	result = venture_reconciliation_service_suggest(venture_context_get_reconciliation_service(context),
		venture_entity_get_entity_name(transaction), id, "exact", 80, &actor, "test", &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

VENTURE_DECLARE_ENTITY(VentureOtherMatchFixture, venture_other_match_fixture, OTHER_MATCH_FIXTURE)
VENTURE_DEFINE_ENTITY(VentureOtherMatchFixture, venture_other_match_fixture, fixture_fields)
static void
test_source_reference_type(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, db);
	g_autoptr(VentureEntity) candidate = record(0, 10000, "USD", 5, "coffee");
	g_autoptr(VentureMoney) amount = venture_money_new(10000, "USD", 2);
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 9, 5, 0, 0, 0);
	g_autoptr(VentureEntity) transaction = g_object_new(venture_other_match_fixture_get_type(),
		"organization-id", (gint64)1, "amount", amount, "date", date, "description", "coffee", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER; actor.name = "tester";
	actor.prompt = NULL; actor.request_id = NULL; actor.approved_by = NULL;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "bank_match") == G_TYPE_INVALID)
		g_assert_true(venture_entity_registry_register(venture_entity_registry_get_default(), venture_bank_match_get_type(), NULL));
	g_assert_true(venture_entity_registry_register(venture_entity_registry_get_default(), venture_other_match_fixture_get_type(), NULL));
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(db, candidate, &actor, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(db, transaction, &actor, &error));
	g_assert_no_error(error);
	/* IDs are per type: source 1 must not become a reference to candidate 1. */
	g_assert_cmpint(venture_entity_get_id(candidate), ==, venture_entity_get_id(transaction));
	result = venture_reconciliation_service_suggest(venture_context_get_reconciliation_service(context),
		venture_entity_get_entity_name(transaction), venture_entity_get_id(transaction), "exact", 80, &actor, "test", &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_matcher_contract(void)
{
	g_type_ensure(VENTURE_TYPE_RECONCILIATION_MATCHER);
	g_assert_cmpuint(g_type_from_name("VentureReconciliationMatcher"), !=, G_TYPE_INVALID);
	{
		g_autoptr(VentureEntity) candidate = record(1, 10000, "USD", 5, "coffee");
		g_autoptr(VentureMatchSuggestion) suggestion = venture_match_suggestion_new(candidate, 100, "equal", VENTURE_MATCH_EXACT);
		g_autoptr(VentureEntity) held = NULL;
		g_autofree gchar *why = NULL;
		gint score, kind;
		const gchar *names[] = { "candidate", "confidence", "rationale", "kind" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(names); i++)
			g_assert_true((g_object_class_find_property(G_OBJECT_GET_CLASS(suggestion), names[i])->flags & G_PARAM_CONSTRUCT_ONLY) != 0);
		g_object_get(suggestion, "candidate", &held, "confidence", &score, "rationale", &why, "kind", &kind, NULL);
		g_assert_true(held == candidate);
		g_assert_cmpint(score, ==, 100);
		g_assert_cmpstr(why, ==, "equal");
		g_assert_cmpint(kind, ==, VENTURE_MATCH_EXACT);
		g_assert_true(g_type_test_flags(VENTURE_TYPE_MATCH_SUGGESTION, G_TYPE_FLAG_FINAL));
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/reconciliation/contract", test_matcher_contract);
	g_assert_true(venture_entity_registry_register(venture_entity_registry_get_default(), venture_match_fixture_get_type(), NULL));
	g_test_add_func("/reconciliation/exact/unique", test_exact_unique);
	g_test_add_func("/reconciliation/exact/ambiguous", test_exact_ambiguous);
	g_test_add_func("/reconciliation/exact/ten-days", test_exact_ten_days);
	g_test_add_func("/reconciliation/exact/outside-date", test_exact_outside_date);
	g_test_add_func("/reconciliation/exact/partial", test_exact_partial);
	g_test_add_func("/reconciliation/exact/outside-amount", test_exact_outside_amount);
	g_test_add_func("/reconciliation/exact/no-overlap", test_exact_no_overlap);
	g_test_add_func("/reconciliation/exact/currency", test_exact_currency);
	g_test_add_func("/reconciliation/exact/missing", test_exact_missing);

	g_test_add_func("/reconciliation/registry", test_registry);
	g_test_add_func("/reconciliation/async", test_async);
	g_test_add_data_func("/reconciliation/ai/valid", "[{\"id\":2,\"confidence\":110,\"why\":\"same\"},{\"id\":999,\"confidence\":90,\"why\":\"invented\"},{\"id\":22,\"confidence\":90,\"why\":\"not offered\"}]", test_ai_answer);
	g_test_add_data_func("/reconciliation/ai/non-json", "not json", test_ai_answer);
	g_test_add_data_func("/reconciliation/ai/wrong-shape", "{}", test_ai_answer);
	g_test_add_data_func("/reconciliation/ai/wrong-confidence", "[{\"id\":3,\"confidence\":\"90\",\"why\":\"bad\"}]", test_ai_answer);
	g_test_add_data_func("/reconciliation/http-only", GINT_TO_POINTER(0), test_http);
	g_test_add_func("/reconciliation/staging", test_staging);
	g_test_add_data_func("/reconciliation/http-staged", GINT_TO_POINTER(1), test_http);
	g_test_add_data_func("/reconciliation/assistant-stages", GINT_TO_POINTER(VENTURE_AI_POLICY_CONFIRM_WRITES), test_assistant_stages);
	g_test_add_data_func("/reconciliation/assistant-autonomous-stages", GINT_TO_POINTER(VENTURE_AI_POLICY_AUTONOMOUS), test_assistant_stages);
	g_test_add_func("/reconciliation/exact/duplicate-identity", test_exact_duplicate_identity);
	g_test_add_func("/reconciliation/registry-merges-matchers", test_registry_merges_matchers);
	g_test_add_func("/reconciliation/exact/scope-extremes", test_scope_and_extremes);
	g_test_add_data_func("/reconciliation/ai/clamp-low", "[{\"id\":2,\"confidence\":-20,\"why\":\"no match\"}]", test_ai_answer);
	g_test_add_data_func("/reconciliation/refuses-missing-source", GINT_TO_POINTER(0), test_service_refuses_absent_source);
	g_test_add_data_func("/reconciliation/refuses-deleted-source", GINT_TO_POINTER(1), test_service_refuses_absent_source);
	g_test_add_func("/reconciliation/source-reference-type", test_source_reference_type);
	return g_test_run();
}
