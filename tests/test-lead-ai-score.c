/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* AI lead score proposals beside the scoring rules (docs/leads.org, AI
 * score proposals). The model is a canned provider: no network, and each
 * test says what breaks if the rule it names goes. */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

#define TYPE_CANNED_PROVIDER (canned_provider_get_type())
G_DECLARE_FINAL_TYPE(CannedProvider, canned_provider, CANNED, PROVIDER, GObject)

struct _CannedProvider
{
	GObject	 parent_instance;
	gchar	*answer;	/* what it replies */
	gchar	*prompt;	/* the user turn it was last sent */
	gchar	*system;	/* the system prompt it was last sent */
	guint	 calls;
};

static void canned_iface(AiProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(CannedProvider, canned_provider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER, canned_iface))

static void
canned_finalize(GObject *object)
{
	CannedProvider *self = CANNED_PROVIDER(object);

	g_free(self->answer);
	g_free(self->prompt);
	g_free(self->system);
	G_OBJECT_CLASS(canned_provider_parent_class)->finalize(object);
}

static void
canned_provider_class_init(CannedProviderClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = canned_finalize;
}

static void
canned_provider_init(CannedProvider *self)
{
	(void)self;
}

static void
canned_chat(
	AiProvider		*provider,
	GList			*messages,
	const gchar		*system_prompt,
	gint			 max_tokens,
	GList			*offered_tools,
	GCancellable		*cancellable,
	GAsyncReadyCallback	 callback,
	gpointer		 data
){
	CannedProvider *self = CANNED_PROVIDER(provider);
	g_autoptr(GTask) task = NULL;
	g_autoptr(AiTextContent) text = NULL;
	AiResponse *response;

	(void)max_tokens;

	/* Every judgement here goes down the toolless path. A tool offered
	 * to a model reading strangers' answers is a tool those answers can ask for. */
	g_assert_null(offered_tools);

	task = g_task_new(provider, cancellable, callback, data);
	response = ai_response_new("fixture", "fixture");
	text = ai_text_content_new(self->answer);

	g_free(self->prompt);
	g_free(self->system);
	self->prompt = ai_message_get_text(messages->data);
	self->system = g_strdup(system_prompt);
	self->calls++;

	ai_response_add_content_block(response,
	                              AI_CONTENT_BLOCK(g_steal_pointer(&text)));
	g_task_return_pointer(task, response, g_object_unref);
}

static AiResponse *
canned_finish(
	AiProvider	 *provider,
	GAsyncResult	 *result,
	GError		**error
){
	(void)provider;

	return g_task_propagate_pointer(G_TASK(result), error);
}

static const gchar *
canned_name(AiProvider *provider)
{
	(void)provider;

	return "fixture";
}

static AiProviderType
canned_type(AiProvider *provider)
{
	(void)provider;

	return AI_PROVIDER_OPENAI;
}

static void
canned_iface(AiProviderInterface *iface)
{
	iface->chat_async = canned_chat;
	iface->chat_finish = canned_finish;
	iface->get_name = canned_name;
	iface->get_default_model = canned_name;
	iface->get_provider_type = canned_type;
}

typedef struct {
	VentureConfig *config;
	VentureDatabase *database;
	VentureContext *context;
	CannedProvider *provider;
	VentureAiService *ai;
	gint64 org;
} Fixture;

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->database, e, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
switch_on(Fixture *f, gboolean on)
{
	g_autoptr(VentureEntity) org = venture_database_get(f->database, VENTURE_TYPE_ORGANIZATION, f->org, NULL);
	g_assert_nonnull(org);
	g_object_set(org, "ai-lead-scoring", on, NULL);
	save(f, org);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = venture_context_get_default_organization_id(f->context);
	f->provider = g_object_new(TYPE_CANNED_PROVIDER, NULL);
	f->ai = venture_ai_service_new_with_provider(f->context, AI_PROVIDER(f->provider), &error);
	g_assert_no_error(error);
	venture_context_set_ai_service(f->context, f->ai);
	switch_on(f, TRUE);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_context_set_ai_service(f->context, NULL);
	g_clear_object(&f->ai);
	g_clear_object(&f->provider);
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
}

static void
answer(Fixture *f, const gchar *text)
{
	g_free(f->provider->answer);
	f->provider->answer = g_strdup(text);
}

static VentureActor
person(void)
{
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "sales-lead";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	return actor;
}

/* A web lead worth 30 by the rules, with a company size and two dated
 * interactions: the record and activities a proposal may read. */
static VentureEntity *
scored_lead(Fixture *f)
{
	g_autoptr(VentureEntity) rule = VENTURE_ENTITY(venture_lead_scoring_rule_new());
	g_autoptr(VentureEntity) first = VENTURE_ENTITY(venture_interaction_new());
	g_autoptr(VentureEntity) second = VENTURE_ENTITY(venture_interaction_new());
	g_autoptr(GError) error = NULL;
	VentureEntity *lead = VENTURE_ENTITY(venture_lead_new());
	VentureEntity *fresh;
	g_object_set(rule, "organization-id", f->org, "name", "Web", "conditions", "source=web", "points", (gint64)30, "active", TRUE, NULL);
	save(f, rule);
	g_object_set(lead, "organization-id", f->org, "name", "Robin Example", "company-name", "Example Works",
		"email", "robin@example.com", "source", "web", "notes", "Asked about 25 seats for the spring rollout", NULL);
	save(f, lead);
	g_object_set(first, "organization-id", f->org, "lead-id", venture_entity_get_id(lead), "kind", VENTURE_INTERACTION_KIND_EMAIL,
		"subject", "Pricing for the team plan", "body", "Can we start in March?", NULL);
	save(f, first);
	g_object_set(second, "organization-id", f->org, "lead-id", venture_entity_get_id(lead), "kind", VENTURE_INTERACTION_KIND_CALL,
		"subject", "Discovery call", "body", "Budget approved by finance", "outbound", TRUE, NULL);
	save(f, second);
	fresh = venture_database_get(f->database, VENTURE_TYPE_LEAD, venture_entity_get_id(lead), &error);
	g_assert_no_error(error);
	g_object_unref(lead);
	return fresh;
}

static gint64
lead_score(Fixture *f, VentureEntity *lead, gboolean *manual)
{
	g_autoptr(VentureEntity) current = venture_database_get(f->database, VENTURE_TYPE_LEAD, venture_entity_get_id(lead), NULL);
	gint64 score = 0;
	gboolean mark = FALSE;
	g_object_get(current, "score", &score, "score-manual", &mark, NULL);
	if (manual != NULL) *manual = mark;
	return score;
}

static guint
audit_rows(Fixture *f, const gchar *type, gint64 id)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_add_filter_string(q, "target-type", VENTURE_FILTER_OP_EQ, type, NULL);
	venture_query_add_filter_int(q, "target-id", VENTURE_FILTER_OP_EQ, id, NULL);
	rows = venture_database_find(f->database, q, NULL);
	g_assert_nonnull(rows);
	return rows->len;
}

/* The proposal reads the lead's record and activities between the data
 * markers on the toolless path, keeps the rule score as the score, and
 * stores what the model said as a pending proposal. If the proposal ever
 * wrote the lead, the rule score would be silently overwritten. */
static void
test_propose(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = scored_lead(f);
	g_autoptr(VentureEntity) proposal = NULL;
	g_autoptr(VentureEntity) pending = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reasons = NULL, *status = NULL;
	VentureActor actor = person();
	gint64 score = 0, rule = 0, lead_id = 0;
	gboolean manual = TRUE;
	(void)data;
	g_assert_cmpint(lead_score(f, lead, NULL), ==, 30);
	g_assert_true(venture_lead_ai_score_enabled(f->context, f->org));
	answer(f, "{\"score\":72,\"reasons\":["
		"{\"facts\":[1,2],\"text\":\"An inquiry from the web form about the team plan\"},"
		"{\"facts\":[3],\"text\":\"Asked about 25 seats\"}]}");
	proposal = venture_lead_ai_score_propose(f->context, lead, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(proposal);
	g_assert_cmpuint(f->provider->calls, ==, 1);
	g_assert_nonnull(strstr(f->provider->prompt, "BEGIN RECORDS"));
	g_assert_nonnull(strstr(f->provider->prompt, "END RECORDS"));
	g_assert_nonnull(strstr(f->provider->prompt, "Pricing for the team plan"));
	g_assert_nonnull(strstr(f->provider->prompt, "Budget approved by finance"));
	g_assert_nonnull(strstr(f->provider->system, "0 to 100"));
	g_object_get(proposal, "score", &score, "rule-score", &rule, "reasons", &reasons, "status", &status, "lead-id", &lead_id, NULL);
	g_assert_cmpint(score, ==, 72);
	g_assert_cmpint(rule, ==, 30);
	g_assert_cmpint(lead_id, ==, venture_entity_get_id(lead));
	g_assert_cmpstr(status, ==, "proposed");
	g_assert_nonnull(strstr(reasons, "Asked about 25 seats"));
	/* The lead is untouched: still the rule score, still not manual. */
	g_assert_cmpint(lead_score(f, lead, &manual), ==, 30);
	g_assert_false(manual);
	pending = venture_lead_ai_score_pending(f->database, lead);
	g_assert_nonnull(pending);
	g_assert_cmpint(venture_entity_get_id(pending), ==, venture_entity_get_id(proposal));
	g_assert_cmpuint(audit_rows(f, "lead_score_proposal", venture_entity_get_id(proposal)), >=, 1);
}

/* Grounding: a reason that cites a fact that was not given, cites none, or
 * names a number the cited facts do not hold is dropped; with none left,
 * or a score outside 0..100, nothing is stored at all. A model that
 * invents "500 employees" must not have it shown as a reason. */
static void
test_grounding(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = scored_lead(f);
	g_autoptr(VentureEntity) proposal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reasons = NULL;
	VentureActor actor = person();
	(void)data;
	answer(f, "{\"score\":64,\"reasons\":["
		"{\"facts\":[99],\"text\":\"Works at a famous company\"},"
		"{\"facts\":[],\"text\":\"Seems keen\"},"
		"{\"facts\":[3],\"text\":\"Has 500 employees\"},"
		"{\"facts\":[3],\"text\":\"Wants 25 seats this spring\"}]}");
	proposal = venture_lead_ai_score_propose(f->context, lead, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(proposal);
	g_object_get(proposal, "reasons", &reasons, NULL);
	g_assert_nonnull(strstr(reasons, "Wants 25 seats this spring"));
	g_assert_null(strstr(reasons, "famous"));
	g_assert_null(strstr(reasons, "keen"));
	g_assert_null(strstr(reasons, "500"));
	g_clear_object(&proposal);
	answer(f, "{\"score\":64,\"reasons\":[{\"facts\":[42],\"text\":\"Invented\"}]}");
	proposal = venture_lead_ai_score_propose(f->context, lead, &actor, &error);
	g_assert_null(proposal);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	answer(f, "{\"score\":150,\"reasons\":[{\"facts\":[1],\"text\":\"Came from the web\"}]}");
	proposal = venture_lead_ai_score_propose(f->context, lead, &actor, &error);
	g_assert_null(proposal);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	answer(f, "I would give this lead a solid seventy.");
	proposal = venture_lead_ai_score_propose(f->context, lead, &actor, &error);
	g_assert_null(proposal);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(lead_score(f, lead, NULL), ==, 30);
}

/* Accepting writes the proposed score through the lead service as an
 * operator's score: the manual mark is set and a history row says so.
 * A second decision is refused. */
static void
test_accept(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = scored_lead(f);
	g_autoptr(VentureEntity) proposal = NULL, stored = NULL, history = NULL;
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_LEAD_SCORE_HISTORY);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *status = NULL, *decided_by = NULL;
	VentureActor actor = person();
	gboolean manual = FALSE, history_manual = FALSE;
	gint64 history_score = 0;
	(void)data;
	answer(f, "{\"score\":72,\"reasons\":[{\"facts\":[1],\"text\":\"An inquiry from the web form\"}]}");
	proposal = venture_lead_ai_score_propose(f->context, lead, &actor, &error);
	g_assert_no_error(error);
	g_assert_true(venture_lead_ai_score_decide(f->context, proposal, TRUE, &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(lead_score(f, lead, &manual), ==, 72);
	g_assert_true(manual);
	stored = venture_database_get(f->database, VENTURE_TYPE_LEAD_SCORE_PROPOSAL, venture_entity_get_id(proposal), &error);
	g_object_get(stored, "status", &status, "decided-by", &decided_by, NULL);
	g_assert_cmpstr(status, ==, "accepted");
	g_assert_cmpstr(decided_by, ==, "sales-lead");
	venture_query_add_filter_int(q, "lead-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(lead), NULL);
	venture_query_add_order(q, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(q, 1);
	history = venture_database_find_one(f->database, q, &error);
	g_assert_nonnull(history);
	g_object_get(history, "score", &history_score, "manual", &history_manual, NULL);
	g_assert_cmpint(history_score, ==, 72);
	g_assert_true(history_manual);
	g_assert_cmpuint(audit_rows(f, "lead_score_proposal", venture_entity_get_id(proposal)), >=, 2);
	g_assert_null(venture_lead_ai_score_pending(f->database, lead));
	g_assert_false(venture_lead_ai_score_decide(f->context, stored, FALSE, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

/* Dismissing leaves the lead exactly as the rules scored it. */
static void
test_dismiss(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = scored_lead(f);
	g_autoptr(VentureEntity) proposal = NULL, stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *status = NULL;
	VentureActor actor = person();
	gboolean manual = TRUE;
	(void)data;
	answer(f, "{\"score\":10,\"reasons\":[{\"facts\":[1],\"text\":\"An inquiry from the web form\"}]}");
	proposal = venture_lead_ai_score_propose(f->context, lead, &actor, &error);
	g_assert_no_error(error);
	g_assert_true(venture_lead_ai_score_decide(f->context, proposal, FALSE, &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(lead_score(f, lead, &manual), ==, 30);
	g_assert_false(manual);
	stored = venture_database_get(f->database, VENTURE_TYPE_LEAD_SCORE_PROPOSAL, venture_entity_get_id(proposal), &error);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "dismissed");
	g_assert_cmpuint(audit_rows(f, "lead_score_proposal", venture_entity_get_id(proposal)), >=, 2);
	g_assert_false(venture_lead_ai_score_decide(f->context, stored, TRUE, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_cmpint(lead_score(f, lead, NULL), ==, 30);
}

/* The organization's switch turns the feature off: nothing is asked of the
 * model, nothing is proposed and a pending proposal cannot be accepted. */
static void
test_switch_off(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = scored_lead(f);
	g_autoptr(VentureEntity) proposal = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor = person();
	(void)data;
	answer(f, "{\"score\":72,\"reasons\":[{\"facts\":[1],\"text\":\"An inquiry from the web form\"}]}");
	proposal = venture_lead_ai_score_propose(f->context, lead, &actor, &error);
	g_assert_no_error(error);
	switch_on(f, FALSE);
	g_assert_false(venture_lead_ai_score_enabled(f->context, f->org));
	g_assert_false(venture_lead_ai_score_decide(f->context, proposal, TRUE, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	g_clear_object(&proposal);
	proposal = venture_lead_ai_score_propose(f->context, lead, &actor, &error);
	g_assert_null(proposal);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_cmpuint(f->provider->calls, ==, 1);
	g_assert_cmpint(lead_score(f, lead, NULL), ==, 30);
	/* Turning AI off for the whole install does the same. */
	switch_on(f, TRUE);
	venture_config_set_module_enabled(f->config, "ai", FALSE);
	g_assert_false(venture_lead_ai_score_enabled(f->context, f->org));
}

/* Only the service writes proposals, so a generic create cannot plant an
 * "accepted" one or a score the model never gave. */
static void
test_generic_write_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = scored_lead(f);
	g_autoptr(VentureEntity) forged = VENTURE_ENTITY(venture_lead_score_proposal_new());
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(forged, "organization-id", f->org, "lead-id", venture_entity_get_id(lead), "score", (gint64)99,
		"status", "proposed", NULL);
	g_assert_false(venture_database_save(f->database, forged, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

typedef struct { gboolean done; GBytes *body; GError *error; } WebCall;

static void
web_done(GObject *source, GAsyncResult *result, gpointer data)
{
	WebCall *call = data;
	call->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &call->error);
	call->done = TRUE;
}

/* One request to a server on this thread's context, driven by hand. */
static guint
web_request(VentureWebServer *server, const gchar *method, const gchar *path, const gchar *form, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new();
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", venture_web_server_get_port(server), path);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	WebCall call = { FALSE, NULL, NULL };
	guint status;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (form != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, web_done, &call);
	while (!call.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(call.error);
	if (out != NULL)
		*out = g_strndup(g_bytes_get_data(call.body, NULL), g_bytes_get_size(call.body));
	status = soup_message_get_status(message);
	g_bytes_unref(call.body);
	return status;
}

static VentureWebServer *
web_start(VentureConfig *config, VentureContext *context)
{
	g_autoptr(GError) error = NULL;
	VentureWebServer *server;
	g_object_set(config, "server-bind-address", "127.0.0.1", "server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	return server;
}

/* The lead's page offers the request, shows a pending proposal beside the
 * rule score with its reasons and the two decisions, and the accept button
 * goes through the same service. With the switch off nothing is offered. */
static void
test_lead_page(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) lead = scored_lead(f);
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *page = NULL, *path = NULL, *ask = NULL, *accept = NULL;
	g_autoptr(VentureEntity) pending = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-lead-ai-XXXXXX", NULL);
	gint64 id = venture_entity_get_id(lead);
	(void)data;
	g_object_set(f->config, "state-dir", dir, NULL);
	server = web_start(f->config, f->context);
	path = g_strdup_printf("/e/lead/%" G_GINT64_FORMAT, id);
	ask = g_strdup_printf("/leads/%" G_GINT64_FORMAT "/ai_score", id);
	g_assert_cmpuint(web_request(server, "GET", path, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, ask));
	g_assert_nonnull(strstr(page, "Rule score"));
	g_clear_pointer(&page, g_free);
	answer(f, "{\"score\":72,\"reasons\":[{\"facts\":[3],\"text\":\"Asked about 25 seats\"}]}");
	g_assert_cmpuint(web_request(server, "POST", ask, "", NULL), ==, 302);
	pending = venture_lead_ai_score_pending(f->database, lead);
	g_assert_nonnull(pending);
	g_assert_cmpuint(web_request(server, "GET", path, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Asked about 25 seats"));
	g_assert_nonnull(strstr(page, ">72<"));
	accept = g_strdup_printf("/lead_score_proposals/%" G_GINT64_FORMAT "/accept", venture_entity_get_id(pending));
	g_assert_nonnull(strstr(page, accept));
	g_assert_nonnull(strstr(page, "/dismiss\""));
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(web_request(server, "POST", accept, "", NULL), ==, 302);
	g_assert_cmpint(lead_score(f, lead, NULL), ==, 72);
	switch_on(f, FALSE);
	g_assert_cmpuint(web_request(server, "GET", path, NULL, &page), ==, 200);
	g_assert_null(strstr(page, ask));
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/lead-ai-score/propose", Fixture, NULL, setup, test_propose, teardown);
	g_test_add("/lead-ai-score/grounding", Fixture, NULL, setup, test_grounding, teardown);
	g_test_add("/lead-ai-score/accept", Fixture, NULL, setup, test_accept, teardown);
	g_test_add("/lead-ai-score/dismiss", Fixture, NULL, setup, test_dismiss, teardown);
	g_test_add("/lead-ai-score/switch-off", Fixture, NULL, setup, test_switch_off, teardown);
	g_test_add("/lead-ai-score/generic-write-refused", Fixture, NULL, setup, test_generic_write_refused, teardown);
	g_test_add("/lead-ai-score/lead-page", Fixture, NULL, setup, test_lead_page, teardown);
	return g_test_run();
}
