/*
 * test-token-actor.c - Whose token it was, shown and never stored
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A token acts as "API token #N" and that string is a key: clawtilla
 * matches a confirmation's origin against whoami, the marketdata alerts
 * and the comment thread parse it, and webhooks carry it. These tests pin
 * both halves of the fix for "changes show as API token #N everywhere":
 * every surface a person reads (a record's activity, the audit log, the
 * inbox, the timeline API, a confirmation) names the account behind the
 * token when the reader may know it, and nothing stored or matched on
 * changes. They also pin who may not know it -- a reader who shares no
 * organization with the owner -- and that a deleted token or owner reads
 * exactly as before.
 */

#include <venture.h>

#include <string.h>
#include <libsoup/soup.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig *config;
	VentureDatabase *database;
	VentureContext *context;
	VentureWebServer *server;
	gchar *state_dir;
	gint64 org;
	gint64 other_org;
	gint64 owner_id;		/* olive: the install's owner */
	gint64 editor_id;		/* erin: an editor, and the token owner */
	gint64 viewer_id;		/* vic: a global viewer who owns the company */
	gint64 stranger_id;		/* gail: an administrator of the other organization only */
	gint64 editor_token_id;
	gint64 stranger_token_id;
	gchar *owner_token;
	gchar *editor_token;
	gchar *viewer_token;
	gchar *stranger_token;
	VentureEntity *company;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(f->database, record, NULL, &error))
		g_error("save %s: %s", G_OBJECT_TYPE_NAME(record), error->message);
}

static gint64
make_user(
	Fixture		*f,
	const gchar	*username,
	const gchar	*display,
	VentureUserRole	 role
){
	g_autoptr(VentureEntity) user = NULL;

	user = g_object_new(VENTURE_TYPE_USER, "username", username,
	                    "display-name", display, "role", role, "active", TRUE,
	                    NULL);
	save(f, user);

	return venture_entity_get_id(user);
}

static void
make_member(
	Fixture			*f,
	gint64			 user_id,
	gint64			 org,
	VentureOrganizationRole	 role
){
	g_autoptr(VentureEntity) member = NULL;

	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
	                      "user-id", user_id, "organization-id", org,
	                      "role", role, "active", TRUE, NULL);
	save(f, member);
}

/* A token for @user_id. Its name is never meant to be seen, so a test that
 * finds it on a page has found a leak. */
static gchar *
make_token(
	Fixture		*f,
	gint64		 user_id,
	VentureUserRole	 role,
	gint64		*out_id
){
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;

	g_object_set(token, "name", "a name nobody should ever see",
	             "user-id", user_id, "role", role, NULL);
	secret = venture_api_token_generate(token);
	save(f, VENTURE_ENTITY(token));

	if (NULL != out_id)
		*out_id = venture_entity_get_id(VENTURE_ENTITY(token));

	return secret;
}

/*
 * Two organizations. Olive owns the install; Erin edits in the default one,
 * and Vic -- a global viewer, who changes nothing -- reads it and owns the
 * company every test changes. Gail administers the install but is a member
 * only of the other organization: her admin token may write anywhere, and
 * Vic has never shared an organization with her.
 *
 * Vic's membership is editor, not viewer, on purpose. A viewer, sales or
 * support member reads only records assigned to them, and an audit entry is
 * assigned to nobody, so such a member is shown no history at all -- there
 * would be no actor on the page to name. The global viewer role keeps him
 * read-only all the same.
 */
static void
fixture_set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) other = NULL;

	(void)data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "test-secret-value", TRUE);
	f->state_dir = g_dir_make_tmp("venture-token-actor-XXXXXX", &error);
	g_assert_no_error(error);

	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0,
	             "security-require-auth", TRUE,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             NULL);

	f->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->org = venture_context_get_default_organization_id(f->context);

	other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other Ltd",
	                     "slug", "other", NULL);
	save(f, other);
	f->other_org = venture_entity_get_id(other);

	f->owner_id = make_user(f, "olive", "Olive Owner", VENTURE_USER_ROLE_OWNER);
	f->editor_id = make_user(f, "erin", "Erin Edits", VENTURE_USER_ROLE_EDITOR);
	f->viewer_id = make_user(f, "vic", "Vic Views", VENTURE_USER_ROLE_VIEWER);
	f->stranger_id = make_user(f, "gail", "Gail Elsewhere", VENTURE_USER_ROLE_ADMIN);

	/* Memberships before tokens: a token captures them when it is made. */
	make_member(f, f->owner_id, f->org, VENTURE_ORGANIZATION_ROLE_OWNER);
	make_member(f, f->editor_id, f->org, VENTURE_ORGANIZATION_ROLE_EDITOR);
	make_member(f, f->viewer_id, f->org, VENTURE_ORGANIZATION_ROLE_EDITOR);
	make_member(f, f->stranger_id, f->other_org, VENTURE_ORGANIZATION_ROLE_ADMIN);

	f->owner_token = make_token(f, f->owner_id, VENTURE_USER_ROLE_OWNER, NULL);
	f->editor_token = make_token(f, f->editor_id, VENTURE_USER_ROLE_EDITOR,
	                             &f->editor_token_id);
	f->viewer_token = make_token(f, f->viewer_id, VENTURE_USER_ROLE_VIEWER, NULL);
	f->stranger_token = make_token(f, f->stranger_id, VENTURE_USER_ROLE_ADMIN,
	                               &f->stranger_token_id);

	f->company = g_object_new(VENTURE_TYPE_COMPANY, "name", "Bellhaven Books",
	                          "owner-user-id", f->viewer_id, NULL);
	venture_entity_set_organization_id(f->company, f->org);
	save(f, f->company);

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
fixture_tear_down(Fixture *f, gconstpointer data)
{
	(void)data;

	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->company);
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
	g_free(f->owner_token);
	g_free(f->editor_token);
	g_free(f->viewer_token);
	g_free(f->stranger_token);
	venture_test_remove_tree(f->state_dir);
	g_free(f->state_dir);
	g_unsetenv("VENTURE_TEST_SESSION_SECRET");
}

/* --- Speaking HTTP ---------------------------------------------------------- */

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} Reply;

static void
reply_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;

	reply->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                 result, &reply->error);
	reply->done = TRUE;
}

/* One request as @token, answered on this thread's main context, which is
 * where the server runs. */
static guint
request(
	Fixture		 *f,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *token,
	const gchar	 *json,
	gchar		**out_body
){
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *bearer = NULL;
	Reply reply;

	memset(&reply, 0, sizeof(reply));
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	bearer = g_strconcat("Bearer ", token, NULL);
	soup_message_headers_replace(soup_message_get_request_headers(message),
	                             "Authorization", bearer);

	if (NULL != json)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(json, strlen(json));

		soup_message_set_request_body_from_bytes(message, "application/json",
		                                         bytes);
	}

	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
	                                 NULL, reply_done, &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(reply.bytes, NULL),
		                      g_bytes_get_size(reply.bytes));

	g_bytes_unref(reply.bytes);

	return soup_message_get_status(message);
}

/* GET @path as @token, which must answer 200; the body. */
static gchar *
page_as(Fixture *f, const gchar *path, const gchar *token)
{
	gchar *body = NULL;

	g_assert_cmpuint(request(f, "GET", path, token, NULL, &body), ==, 200);

	return body;
}

/* GET @path as @token, which must answer 200 with JSON. */
static JsonNode *
json_as(Fixture *f, const gchar *path, const gchar *token)
{
	g_autofree gchar *body = NULL;
	JsonNode *node;

	body = page_as(f, path, token);
	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);

	return node;
}

/* --- Helpers over the record ------------------------------------------------ */

static gchar *
token_actor(gint64 token_id)
{
	return g_strdup_printf(VENTURE_ACTOR_NAMES_TOKEN_PREFIX "%" G_GINT64_FORMAT,
	                       token_id);
}

/* The company changed by @token over the generic REST route. */
static void
change_company(Fixture *f, const gchar *token, const gchar *industry)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *json = NULL;

	path = g_strdup_printf("/api/v1/company/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(f->company));
	json = g_strdup_printf("{\"industry\":\"%s\"}", industry);
	g_assert_cmpuint(request(f, "PATCH", path, token, json, NULL), ==, 200);
}

/* The newest audit entry on the company written as @actor, read as the
 * system: what is stored, not what anybody is shown. */
static VentureEntity *
stored_entry(Fixture *f, const gchar *actor)
{
	g_autoptr(VentureQuery) query = NULL;
	VentureEntity *entry;

	query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_assert_true(venture_query_add_filter_string(query, "target-type",
		VENTURE_FILTER_OP_EQ, "company", NULL));
	g_assert_true(venture_query_add_filter_int(query, "target-id",
		VENTURE_FILTER_OP_EQ, venture_entity_get_id(f->company), NULL));
	g_assert_true(venture_query_add_filter_string(query, "actor",
		VENTURE_FILTER_OP_EQ, actor, NULL));
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	entry = venture_database_find_one(f->database, query, NULL);
	g_assert_nonnull(entry);

	return entry;
}

/* The change event @actor made, in a timeline as somebody read it. */
static JsonObject *
timeline_event(JsonNode *timeline, const gchar *actor)
{
	JsonArray *events;
	guint i;

	g_assert_true(JSON_NODE_HOLDS_ARRAY(timeline));
	events = json_node_get_array(timeline);

	for (i = 0; i < json_array_get_length(events); i++)
	{
		JsonObject *event = json_array_get_object_element(events, i);

		if ((0 == g_strcmp0(venture_json_object_get_string(event, "kind", NULL),
		                    "change")) &&
		    (0 == g_strcmp0(venture_json_object_get_string(event, "actor", NULL),
		                    actor)))
			return event;
	}

	{
		g_autofree gchar *dump = venture_json_to_string(timeline, TRUE);

		g_error("no change by \"%s\" in the timeline:\n%s", actor, dump);
	}
	return NULL;
}

static JsonNode *
timeline_as(Fixture *f, const gchar *token)
{
	g_autofree gchar *path = NULL;

	path = g_strdup_printf("/api/v1/activity/company/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(f->company));

	return json_as(f, path, token);
}

static gchar *
record_page_as(Fixture *f, const gchar *token)
{
	g_autofree gchar *path = NULL;

	path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(f->company));

	return page_as(f, path, token);
}

/* --- The tests ---------------------------------------------------------------- */

/*
 * What counts as a token actor. The comment thread and the alert worker
 * parse the same prefix; this is the one parser the display uses.
 */
static void
test_parse(void)
{
	g_assert_cmpint(venture_actor_names_parse_token("API token #12"), ==, 12);
	g_assert_cmpint(venture_actor_names_parse_token("API token #1"), ==, 1);
	/* what an ambiguous upgrade left (000640) names no token */
	g_assert_cmpint(venture_actor_names_parse_token("API token"), ==, 0);
	g_assert_cmpint(venture_actor_names_parse_token("API token #"), ==, 0);
	g_assert_cmpint(venture_actor_names_parse_token("API token #12 (old)"), ==, 0);
	g_assert_cmpint(venture_actor_names_parse_token("API token #0"), ==, 0);
	g_assert_cmpint(venture_actor_names_parse_token("erin"), ==, 0);
	g_assert_cmpint(venture_actor_names_parse_token(NULL), ==, 0);

	/* the one wording */
	{
		g_autofree gchar *named = NULL;
		g_autofree gchar *bare = NULL;

		named = venture_actor_names_format("API token #4", "erin");
		bare = venture_actor_names_format("API token #4", NULL);
		g_assert_cmpstr(named, ==, "erin (API token #4)");
		g_assert_cmpstr(bare, ==, "API token #4");
		g_assert_null(venture_actor_names_format(NULL, "erin"));
	}
}

/*
 * Erin's token changes Vic's company. Everything Vic reads names Erin --
 * they share the organization -- and everything stored or matched on
 * still says "API token #N", recorded as a token rather than an import.
 */
static void
test_owner_named(Fixture *f, gconstpointer data)
{
	g_autofree gchar *actor = NULL;
	g_autofree gchar *shown = NULL;
	g_autofree gchar *inbox_line = NULL;
	g_autofree gchar *stored_actor = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *audit_list = NULL;
	g_autofree gchar *audit_page = NULL;
	g_autofree gchar *audit_list_path = NULL;
	g_autofree gchar *audit_page_path = NULL;
	g_autofree gchar *inbox = NULL;
	g_autoptr(VentureEntity) entry = NULL;
	g_autoptr(JsonNode) timeline = NULL;
	g_autoptr(JsonNode) whoami = NULL;
	g_autoptr(GPtrArray) notifications = NULL;
	VentureActorKind kind;
	JsonObject *event;
	gboolean told;
	guint i;

	(void)data;

	actor = token_actor(f->editor_token_id);
	shown = g_strdup_printf("erin (%s)", actor);
	inbox_line = g_strdup_printf("%s updated Bellhaven Books", shown);

	g_assert_true(venture_notify_watch(f->context, f->viewer_id, "company",
	                                   venture_entity_get_id(f->company), NULL));
	change_company(f, f->editor_token, "Books");

	/* Stored: the key, unchanged, and the kind that says "a token". The
	 * alert worker and the comment thread read this string as it is. */
	entry = stored_entry(f, actor);
	g_object_get(entry, "actor", &stored_actor, "actor-kind", &kind, NULL);
	g_assert_cmpstr(stored_actor, ==, actor);
	g_assert_cmpint(kind, ==, VENTURE_ACTOR_KIND_TOKEN);
	g_assert_cmpint(venture_actor_names_parse_token(stored_actor), ==,
	                f->editor_token_id);

	/* The timeline API: "actor" as stored, the owner beside it. */
	timeline = timeline_as(f, f->viewer_token);
	event = timeline_event(timeline, actor);
	g_assert_cmpstr(venture_json_object_get_string(event, "actor_username",
	                                               NULL), ==, "erin");
	g_assert_cmpstr(venture_json_object_get_string(event, "actor_kind", NULL),
	                ==, "token");

	/* The record's Activity panel. */
	page = record_page_as(f, f->viewer_token);
	g_assert_nonnull(strstr(page, shown));
	g_assert_null(strstr(page, "a name nobody should ever see"));

	/* The audit log, as a list and as the entry's own page. */
	audit_list_path = g_strdup_printf("/e/audit_entry?target_type=company"
	                                  "&target_id=%" G_GINT64_FORMAT,
	                                  venture_entity_get_id(f->company));
	audit_list = page_as(f, audit_list_path, f->viewer_token);
	g_assert_nonnull(strstr(audit_list, shown));
	audit_page_path = g_strdup_printf("/e/audit_entry/%" G_GINT64_FORMAT,
	                                  venture_entity_get_id(entry));
	audit_page = page_as(f, audit_page_path, f->viewer_token);
	g_assert_nonnull(strstr(audit_page, shown));

	/* The inbox line Vic was sent as a watcher of the company... */
	inbox = page_as(f, "/inbox?show=all", f->viewer_token);
	g_assert_nonnull(strstr(inbox, inbox_line));

	/* ...whose stored title and actor still lead with the key. */
	notifications = venture_notify_list(f->context, f->viewer_id, FALSE, 0,
	                                    NULL);
	g_assert_nonnull(notifications);
	told = FALSE;

	for (i = 0; i < notifications->len; i++)
	{
		g_autofree gchar *who = NULL;
		g_autofree gchar *title = NULL;

		g_object_get(g_ptr_array_index(notifications, i), "actor", &who,
		             "title", &title, NULL);

		if (0 != g_strcmp0(who, actor))
			continue;

		told = TRUE;
		g_assert_true(g_str_has_prefix(title, actor));
		g_assert_null(strstr(title, "erin"));
	}

	g_assert_true(told);

	/* whoami is what clawtilla matches a confirmation's origin against. */
	whoami = json_as(f, "/api/v1/whoami", f->editor_token);
	g_assert_cmpstr(venture_json_object_get_string(
		json_node_get_object(whoami), "actor", NULL), ==, actor);
}

/*
 * A token that has been deleted, and a token whose owner has, read as
 * every token did before: "API token #N" and nothing else -- even to the
 * install's owner, who may see any owner there is.
 */
static void
test_deleted_falls_back(Fixture *f, gconstpointer data)
{
	g_autofree gchar *erins = NULL;
	g_autofree gchar *doras = NULL;
	g_autofree gchar *dora_token = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *named = NULL;
	g_autoptr(JsonNode) timeline = NULL;
	g_autoptr(GError) error = NULL;
	gint64 dora_id;
	gint64 dora_token_id = 0;

	(void)data;

	erins = token_actor(f->editor_token_id);

	dora_id = make_user(f, "dora", "Dora Departed", VENTURE_USER_ROLE_EDITOR);
	make_member(f, dora_id, f->org, VENTURE_ORGANIZATION_ROLE_EDITOR);
	dora_token = make_token(f, dora_id, VENTURE_USER_ROLE_EDITOR,
	                        &dora_token_id);
	doras = token_actor(dora_token_id);

	change_company(f, f->editor_token, "Books");
	change_company(f, dora_token, "Maps");

	/* Erin's token goes; Dora herself goes. */
	{
		g_autoptr(VentureEntity) token = NULL;
		g_autoptr(VentureEntity) dora = NULL;

		token = venture_database_get(f->database, VENTURE_TYPE_API_TOKEN,
		                             f->editor_token_id, NULL);
		g_assert_nonnull(token);
		g_assert_true(venture_database_delete(f->database, token, NULL,
		                                      &error));
		g_assert_no_error(error);

		dora = venture_database_get(f->database, VENTURE_TYPE_USER, dora_id,
		                            NULL);
		g_assert_nonnull(dora);
		g_assert_true(venture_database_delete(f->database, dora, NULL,
		                                      &error));
		g_assert_no_error(error);
	}

	timeline = timeline_as(f, f->owner_token);
	g_assert_false(json_object_has_member(timeline_event(timeline, erins),
	                                      "actor_username"));
	g_assert_false(json_object_has_member(timeline_event(timeline, doras),
	                                      "actor_username"));

	page = record_page_as(f, f->owner_token);
	g_assert_nonnull(strstr(page, erins));
	g_assert_nonnull(strstr(page, doras));
	named = g_strdup_printf("(%s)", erins);
	g_assert_null(strstr(page, named));
	g_clear_pointer(&named, g_free);
	named = g_strdup_printf("(%s)", doras);
	g_assert_null(strstr(page, named));
}

/*
 * A staged change's card: origin.name is the key clawtilla routes on,
 * unchanged; origin.username is beside it -- on the staging answer and
 * in the queue's listing alike.
 */
static void
test_confirmation_names_owner(Fixture *f, gconstpointer data)
{
	g_autofree gchar *actor = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) staged = NULL;
	g_autoptr(JsonNode) queue = NULL;
	JsonObject *confirmation;
	JsonObject *origin;
	const gchar *id;
	JsonArray *pending;
	gboolean listed;
	guint i;

	(void)data;

	actor = token_actor(f->editor_token_id);
	path = g_strdup_printf("/api/v1/company/%" G_GINT64_FORMAT "?stage=1",
	                       venture_entity_get_id(f->company));
	g_assert_cmpuint(request(f, "PATCH", path, f->editor_token,
	                         "{\"industry\":\"Staged\"}", &body), ==, 202);
	staged = venture_json_parse(body, NULL);
	g_assert_nonnull(staged);

	confirmation = json_object_get_object_member(json_node_get_object(staged),
	                                             "confirmation");
	id = json_object_get_string_member(confirmation, "id");
	origin = json_object_get_object_member(confirmation, "origin");
	g_assert_cmpstr(json_object_get_string_member(origin, "name"), ==, actor);
	g_assert_cmpstr(json_object_get_string_member(origin, "kind"), ==, "token");
	g_assert_cmpstr(venture_json_object_get_string(origin, "username", NULL),
	                ==, "erin");
	g_assert_cmpint(json_object_get_int_member(origin, "token_id"), ==,
	                f->editor_token_id);
	g_assert_cmpint(json_object_get_int_member(origin, "user_id"), ==,
	                f->editor_id);
	g_assert_cmpint(json_object_get_int_member(origin, "organization_id"), ==,
	                venture_entity_get_organization_id(f->company));

	queue = json_as(f, "/api/v1/confirmations", f->owner_token);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(queue));
	pending = json_node_get_array(queue);
	listed = FALSE;

	for (i = 0; i < json_array_get_length(pending); i++)
	{
		JsonObject *card = json_array_get_object_element(pending, i);

		if (0 != g_strcmp0(json_object_get_string_member(card, "id"), id))
			continue;

		listed = TRUE;
		origin = json_object_get_object_member(card, "origin");
		g_assert_cmpstr(json_object_get_string_member(origin, "name"), ==,
		                actor);
		g_assert_cmpstr(venture_json_object_get_string(origin, "username",
		                                               NULL), ==, "erin");
	}

	g_assert_true(listed);
}

/*
 * Gail administers the install but belongs only to the other
 * organization. Her token changes Vic's company. Vic has never shared an
 * organization with her, so to Vic it is "API token #N" and nothing more;
 * the install's owner, who reads the users page anyway, sees her name.
 */
static void
test_reader_outside_owners_organizations(Fixture *f, gconstpointer data)
{
	g_autofree gchar *actor = NULL;
	g_autofree gchar *named = NULL;
	g_autofree gchar *vics_page = NULL;
	g_autofree gchar *olives_page = NULL;
	g_autoptr(JsonNode) vics = NULL;
	g_autoptr(JsonNode) olives = NULL;
	g_autoptr(JsonNode) erins = NULL;

	(void)data;

	actor = token_actor(f->stranger_token_id);
	named = g_strdup_printf("gail (%s)", actor);

	change_company(f, f->stranger_token, "Atlases");

	vics = timeline_as(f, f->viewer_token);
	g_assert_false(json_object_has_member(timeline_event(vics, actor),
	                                      "actor_username"));
	vics_page = record_page_as(f, f->viewer_token);
	g_assert_nonnull(strstr(vics_page, actor));
	g_assert_null(strstr(vics_page, named));

	/* Erin edits in the same organization, and still shares none with
	 * Gail: what decides is the people, not the role. */
	erins = timeline_as(f, f->editor_token);
	g_assert_false(json_object_has_member(timeline_event(erins, actor),
	                                      "actor_username"));

	olives = timeline_as(f, f->owner_token);
	g_assert_cmpstr(venture_json_object_get_string(timeline_event(olives, actor),
	                                               "actor_username", NULL),
	                ==, "gail");
	olives_page = record_page_as(f, f->owner_token);
	g_assert_nonnull(strstr(olives_page, named));
}

/*
 * A change staged from a browser session has no token behind it: the card
 * carries origin.user_id for the person and no token_id, and the staged
 * record's organization_id.
 */
static void
test_confirmation_session_requester(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) edited = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(JsonNode) card = NULL;
	g_autoptr(GError) error = NULL;
	VentureAuthPrincipal principal;
	VentureConfirmation *confirmation;
	VentureActor origin;
	JsonObject *object;

	(void)data;

	memset(&principal, 0, sizeof(principal));
	principal.user_id = f->editor_id;
	principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_EDITOR;
	principal.name = (gchar *)"erin";
	principal.authenticated = TRUE;

	edited = venture_database_get(f->database, VENTURE_TYPE_COMPANY,
	                              venture_entity_get_id(f->company), &error);
	g_assert_no_error(error);
	g_assert_nonnull(edited);
	g_object_set(edited, "industry", "Session", NULL);

	memset(&origin, 0, sizeof(origin));
	origin.kind = VENTURE_ACTOR_KIND_USER;
	origin.name = "erin";

	scope = venture_access_policy_enter(
		venture_database_get_access_policy(f->database), &principal);
	confirmation = venture_confirmation_store_stage(
		venture_context_get_confirmations(f->context),
		VENTURE_AUDIT_ACTION_UPDATE, edited, f->company, &origin, "web",
		&error);
	g_assert_no_error(error);
	g_assert_nonnull(confirmation);

	card = venture_confirmation_to_json(confirmation);
	object = json_object_get_object_member(json_node_get_object(card),
	                                       "origin");
	g_assert_false(json_object_has_member(object, "token_id"));
	g_assert_cmpint(json_object_get_int_member(object, "user_id"), ==,
	                f->editor_id);
	g_assert_cmpint(json_object_get_int_member(object, "organization_id"), ==,
	                venture_entity_get_organization_id(f->company));
}

/*
 * Vic has never shared an organization with Gail, so the card gives him her
 * token's id (it is in origin.name already) and neither her user id nor her
 * username.
 */
static void
test_confirmation_reader_outside_owners_organizations(Fixture *f, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) staged = NULL;
	g_autoptr(JsonNode) vics = NULL;
	g_autoptr(JsonNode) olives = NULL;
	JsonObject *origin;
	const gchar *id;
	JsonArray *queue;
	guint i;

	(void)data;

	path = g_strdup_printf("/api/v1/company/%" G_GINT64_FORMAT "?stage=1",
	                       venture_entity_get_id(f->company));
	g_assert_cmpuint(request(f, "PATCH", path, f->stranger_token,
	                         "{\"industry\":\"Staged\"}", &body), ==, 202);
	staged = venture_json_parse(body, NULL);
	g_assert_nonnull(staged);
	id = json_object_get_string_member(
		json_object_get_object_member(json_node_get_object(staged),
		                              "confirmation"), "id");

	/* Vic, who owns the company and reads the card. */
	vics = json_as(f, "/api/v1/confirmations", f->viewer_token);
	queue = json_node_get_array(vics);
	origin = NULL;

	for (i = 0; i < json_array_get_length(queue); i++)
	{
		JsonObject *card = json_array_get_object_element(queue, i);

		if (0 == g_strcmp0(json_object_get_string_member(card, "id"), id))
			origin = json_object_get_object_member(card, "origin");
	}

	g_assert_nonnull(origin);
	g_assert_cmpint(json_object_get_int_member(origin, "token_id"), ==,
	                f->stranger_token_id);
	g_assert_false(json_object_has_member(origin, "user_id"));
	g_assert_false(json_object_has_member(origin, "username"));

	/* The install's owner may know who it is. */
	olives = json_as(f, "/api/v1/confirmations", f->owner_token);
	queue = json_node_get_array(olives);
	origin = NULL;

	for (i = 0; i < json_array_get_length(queue); i++)
	{
		JsonObject *card = json_array_get_object_element(queue, i);

		if (0 == g_strcmp0(json_object_get_string_member(card, "id"), id))
			origin = json_object_get_object_member(card, "origin");
	}

	g_assert_nonnull(origin);
	g_assert_cmpint(json_object_get_int_member(origin, "user_id"), ==,
	                f->stranger_id);
}

/*
 * The comment thread's own "via API token #N" still reads the stored
 * author, which is still the key.
 */
static void
test_comment_prefix_unchanged(Fixture *f, gconstpointer data)
{
	g_autofree gchar *json = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *actor = NULL;
	g_autoptr(JsonNode) made = NULL;
	JsonObject *object;

	(void)data;

	actor = token_actor(f->editor_token_id);
	json = g_strdup_printf("{\"subject_type\":\"company\",\"subject_id\":%"
	                       G_GINT64_FORMAT ",\"body\":\"Shelved.\"}",
	                       venture_entity_get_id(f->company));
	g_assert_cmpuint(request(f, "POST", "/api/v1/comments", f->editor_token,
	                         json, &body), ==, 201);
	made = venture_json_parse(body, NULL);
	g_assert_nonnull(made);
	object = json_node_get_object(made);
	g_assert_cmpstr(json_object_get_string_member(object, "author"), ==, actor);
	g_assert_true(json_object_get_boolean_member(object, "via_token"));
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/token-actor/parse", test_parse);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/token-actor/owner-named", test_owner_named);
	ADD("/token-actor/deleted-falls-back", test_deleted_falls_back);
	ADD("/token-actor/confirmation-names-owner", test_confirmation_names_owner);
	ADD("/token-actor/reader-outside-owners-organizations",
	    test_reader_outside_owners_organizations);
	ADD("/token-actor/confirmation-session-requester",
	    test_confirmation_session_requester);
	ADD("/token-actor/confirmation-reader-outside-owners-organizations",
	    test_confirmation_reader_outside_owners_organizations);
	ADD("/token-actor/comment-prefix-unchanged", test_comment_prefix_unchanged);

#undef ADD

	return g_test_run();
}
