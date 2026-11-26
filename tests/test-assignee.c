/*
 * test-assignee.c - Whom a record may be handed to
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A ticket's assignee used to be any string: "oxpeker" saved, belonged to
 * nobody, and an intake listing tickets by exact username never saw it.
 * These hold the rule at every door -- the REST API (which venturectl and
 * the MCP server speak), a staged write when it is approved, a direct
 * save, the board and Desk quick-assign -- and hold the rule's limits: a
 * stored name gone stale does not lock the ticket, a rota does not refuse
 * a ticket because one of its names left, and the lookup lists only
 * people of an organization the caller belongs to.
 */

#include <venture.h>

#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "venture-test-util.h"

#define PASSWORD "assignee-test-password"

typedef struct
{
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gchar			*state_dir;
	gchar			*editor;	/* session cookie of "editor" */
	guint16			 port;
	gint64			 home;		/* the default organization */
	gint64			 elsewhere;	/* a second organization */
	gint64			 editor_id;
	gint64			 oxpecker_id;
} Fixture;

/* ==========================================================================
 * Plumbing
 * ========================================================================== */

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	RequestResult *outcome;

	outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                  result, &outcome->error);
	outcome->done = TRUE;
}

/*
 * One request, driving the main context by hand: the server runs on this
 * context, so a blocking send would wait on a listener that cannot run.
 */
static guint
request(
	Fixture		 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *cookie,
	const gchar	 *content_type,
	const gchar	 *body,
	gchar		**out_body,
	gchar		**out_set_cookie
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != cookie)
		soup_message_headers_append(soup_message_get_request_headers(message),
		                            "Cookie", cookie);

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL, request_done,
	                                 &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	if (NULL != out_set_cookie)
		*out_set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

/* A JSON request as the editor. */
static guint
json_as_editor(
	Fixture		 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *json,
	gchar		**out_body
){
	return request(fixture, method, path, fixture->editor, "application/json",
	               json, out_body, NULL);
}

/* A form post as the editor. */
static guint
form_as_editor(
	Fixture		 *fixture,
	const gchar	 *path,
	const gchar	 *form,
	gchar		**out_body
){
	return request(fixture, "POST", path, fixture->editor,
	               "application/x-www-form-urlencoded", form, out_body, NULL);
}

/* A user, with (when @organization is positive) a membership of it. */
static gint64
make_user(
	Fixture		*fixture,
	const gchar	*username,
	const gchar	*display,
	VentureUserRole	 role,
	gboolean	 active,
	gint64		 organization,
	gboolean	 membership_active
){
	g_autoptr(VentureUser) user = NULL;
	gint64 id;

	user = venture_user_new();
	g_object_set(user, "username", username, "display-name", display,
	             "role", role, "active", active, NULL);
	g_assert_true(venture_user_set_password(user, PASSWORD, 100000, NULL));
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(user),
	                                    NULL, NULL));
	id = venture_entity_get_id(VENTURE_ENTITY(user));

	if (organization > 0)
	{
		g_autoptr(VentureEntity) member = NULL;

		member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
			"user-id", id, "organization-id", organization,
			"role", VENTURE_ORGANIZATION_ROLE_EDITOR,
			"active", membership_active, NULL);
		g_assert_true(venture_database_save(fixture->database, member, NULL,
		                                    NULL));
	}

	return id;
}

static gchar *
login(
	Fixture		*fixture,
	const gchar	*username
){
	g_autofree gchar *form = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	form = g_strdup_printf("username=%s&password=%s", username, PASSWORD);
	g_assert_cmpuint(request(fixture, "POST", "/login", NULL,
	                         "application/x-www-form-urlencoded", form, NULL,
	                         &set_cookie), ==, SOUP_STATUS_FOUND);
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	return g_steal_pointer(&set_cookie);
}

static gint64
make_organization(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*slug
){
	g_autoptr(VentureOrganization) organization = NULL;

	organization = venture_organization_new();
	g_object_set(organization, "name", name, "slug", slug,
	             "kind", VENTURE_ORGANIZATION_KIND_SOLE_PROPRIETOR,
	             "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(organization), NULL,
	                                    NULL));

	return venture_entity_get_id(VENTURE_ENTITY(organization));
}

/* A ticket in @organization saved straight to the database. */
static gint64
make_ticket(
	Fixture		*fixture,
	gint64		 organization,
	const gchar	*title,
	const gchar	*assignee
){
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(GError) error = NULL;

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", title, "assignee", assignee,
	             "kind", VENTURE_TICKET_KIND_INTERNAL, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(ticket), organization);

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(ticket), NULL,
	                           &error))
		g_error("ticket refused: %s", error->message);

	return venture_entity_get_id(VENTURE_ENTITY(ticket));
}

static gchar *
stored_assignee(
	Fixture	*fixture,
	gint64	 ticket_id
){
	g_autoptr(VentureEntity) ticket = NULL;
	gchar *assignee;

	ticket = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
	                              ticket_id, NULL);
	g_assert_nonnull(ticket);
	assignee = NULL;
	g_object_get(ticket, "assignee", &assignee, NULL);

	return assignee;
}

/* A refusal body: the validation shape, naming @field and @value. */
static void
assert_refusal(
	const gchar	*body,
	const gchar	*field,
	const gchar	*value
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *quoted = NULL;
	const gchar *message;

	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);
	g_assert_cmpstr(venture_json_object_get_string(json_node_get_object(node),
	                                               "error", NULL), ==,
	                "validation");
	message = venture_json_object_get_string(json_node_get_object(node),
	                                         "message", "");
	prefix = g_strdup_printf("%s: ", field);
	quoted = g_strdup_printf("\"%s\"", value);
	g_assert_true(g_str_has_prefix(message, prefix));
	g_assert_nonnull(strstr(message, quoted));
}

static void
module_registry_reset(void)
{
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

/*
 * Two organizations and the people around them:
 *
 *   editor    member of home, the one signed in
 *   oxpecker  member of home ("Red-billed Oxpecker")
 *   outsider  member of elsewhere only
 *   retired   member of home, but the user is switched off
 *   lapsed    an active user whose home membership is switched off
 */
static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "assignee-test-secret", TRUE);
	fixture->state_dir = g_dir_make_tmp("venture-assignee-XXXXXX", NULL);

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             "security-password-iterations", (gint64)100000,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);
	fixture->port = venture_web_server_get_port(fixture->server);
	fixture->session = soup_session_new();

	fixture->home = venture_context_get_default_organization_id(fixture->context);
	fixture->elsewhere = make_organization(fixture, "Elsewhere Ltd", "elsewhere");

	fixture->editor_id = make_user(fixture, "editor", "Eddie Tor",
	                               VENTURE_USER_ROLE_EDITOR, TRUE,
	                               fixture->home, TRUE);
	fixture->oxpecker_id = make_user(fixture, "oxpecker", "Red-billed Oxpecker",
	                                 VENTURE_USER_ROLE_EDITOR, TRUE,
	                                 fixture->home, TRUE);
	make_user(fixture, "outsider", "Out Sider", VENTURE_USER_ROLE_EDITOR, TRUE,
	          fixture->elsewhere, TRUE);
	make_user(fixture, "retired", "Re Tired", VENTURE_USER_ROLE_EDITOR, FALSE,
	          fixture->home, TRUE);
	make_user(fixture, "lapsed", "Lap Sed", VENTURE_USER_ROLE_EDITOR, TRUE,
	          fixture->home, FALSE);

	fixture->editor = login(fixture, "editor");
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_pointer(&fixture->editor, g_free);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_unsetenv("VENTURE_TEST_SESSION_SECRET");
	module_registry_reset();
}

/* ==========================================================================
 * The rule, through the REST API
 * ========================================================================== */

/*
 * Create: a name that is nobody is a 422 naming the field and the value;
 * a member saves; empty saves, unassigned. The REST create is the path
 * venturectl and the MCP server's venture_create both take.
 */
static void
test_rest_create(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *json = NULL;
	g_autofree gchar *body = NULL;
	guint status;

	(void)user_data;

	/* The typo. */
	json = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ","
	                       "\"title\":\"Typo\",\"assignee\":\"oxpeker\"}",
	                       fixture->home);
	status = json_as_editor(fixture, "POST", "/api/v1/ticket", json, &body);
	g_assert_cmpuint(status, ==, 422);
	assert_refusal(body, "assignee", "oxpeker");
	g_clear_pointer(&json, g_free);
	g_clear_pointer(&body, g_free);

	/* The member. */
	json = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ","
	                       "\"title\":\"Right\",\"assignee\":\"oxpecker\"}",
	                       fixture->home);
	g_assert_cmpuint(json_as_editor(fixture, "POST", "/api/v1/ticket", json,
	                                NULL), ==, SOUP_STATUS_CREATED);
	g_clear_pointer(&json, g_free);

	/* Nobody. */
	json = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ","
	                       "\"title\":\"Open\",\"assignee\":\"\"}",
	                       fixture->home);
	g_assert_cmpuint(json_as_editor(fixture, "POST", "/api/v1/ticket", json,
	                                NULL), ==, SOUP_STATUS_CREATED);
}

/*
 * Update: a member of another organization, a switched-off user and a
 * switched-off membership are each refused; a member is accepted. The
 * REST update is venture_update's path.
 */
static void
test_rest_update(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *stored = NULL;
	const gchar *refused[] = { "outsider", "retired", "lapsed" };
	gint64 id;
	guint i;

	(void)user_data;

	id = make_ticket(fixture, fixture->home, "To hand over", NULL);
	path = g_strdup_printf("/api/v1/ticket/%" G_GINT64_FORMAT, id);

	for (i = 0; i < G_N_ELEMENTS(refused); i++)
	{
		g_autofree gchar *json = NULL;
		g_autofree gchar *body = NULL;

		json = g_strdup_printf("{\"assignee\":\"%s\"}", refused[i]);
		g_assert_cmpuint(json_as_editor(fixture, "PATCH", path, json, &body),
		                 ==, 422);
		assert_refusal(body, "assignee", refused[i]);
	}

	g_assert_cmpuint(json_as_editor(fixture, "PATCH", path,
	                                "{\"assignee\":\"oxpecker\"}", NULL),
	                 ==, SOUP_STATUS_OK);
	stored = stored_assignee(fixture, id);
	g_assert_cmpstr(stored, ==, "oxpecker");
}

/*
 * A stored name gone stale -- here a member who was switched off after
 * being assigned -- leaves the ticket editable: a new title saves, and so
 * does a body that repeats the stale name unchanged. Changing it to
 * another non-member is still refused, and moving the ticket to an
 * organization the name is not a member of is judged again.
 */
static void
test_stale_assignee_stays_editable(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	gint64 id;

	(void)user_data;

	id = make_ticket(fixture, fixture->home, "Was Ox's", "oxpecker");

	user = venture_database_get(fixture->database, VENTURE_TYPE_USER,
	                            fixture->oxpecker_id, NULL);
	g_object_set(user, "active", FALSE, NULL);
	g_assert_true(venture_database_save(fixture->database, user, NULL, NULL));

	path = g_strdup_printf("/api/v1/ticket/%" G_GINT64_FORMAT, id);

	g_assert_cmpuint(json_as_editor(fixture, "PATCH", path,
	                                "{\"title\":\"Still Ox's, for now\"}", NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_cmpuint(json_as_editor(fixture, "PATCH", path,
		"{\"title\":\"Again\",\"assignee\":\"oxpecker\"}", NULL),
		==, SOUP_STATUS_OK);

	g_assert_cmpuint(json_as_editor(fixture, "PATCH", path,
	                                "{\"assignee\":\"outsider\"}", &body),
	                 ==, 422);
	assert_refusal(body, "assignee", "outsider");

	/* Unassigning a stale ticket is always possible. */
	g_assert_cmpuint(json_as_editor(fixture, "PATCH", path,
	                                "{\"assignee\":\"\"}", NULL),
	                 ==, SOUP_STATUS_OK);

	/* A member of home, carried to an organization they are not in. */
	{
		g_autoptr(VentureEntity) ticket = NULL;
		g_autoptr(GError) error = NULL;
		gint64 moved;

		moved = make_ticket(fixture, fixture->home, "Moving", "editor");
		ticket = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
		                              moved, NULL);
		venture_entity_set_organization_id(ticket, fixture->elsewhere);
		g_assert_false(venture_database_save(fixture->database, ticket, NULL,
		                                     &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}
}

/*
 * A staged write is judged when it is applied: staging the typo answers
 * 202 with nothing written, and approving it is the 422.
 */
static void
test_staged_write_judged_when_applied(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *json = NULL;
	g_autofree gchar *staged = NULL;
	g_autofree gchar *approve = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) node = NULL;
	const gchar *id;

	(void)user_data;

	json = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ","
	                       "\"title\":\"Proposed\",\"assignee\":\"outsider\"}",
	                       fixture->home);
	g_assert_cmpuint(json_as_editor(fixture, "POST", "/api/v1/ticket?stage=1",
	                                json, &staged), ==, SOUP_STATUS_ACCEPTED);

	node = venture_json_parse(staged, NULL);
	g_assert_nonnull(node);
	id = venture_json_object_get_string(
		json_object_get_object_member(json_node_get_object(node),
		                              "confirmation"), "id", NULL);
	g_assert_nonnull(id);

	approve = g_strdup_printf("/api/v1/confirmations/%s/approve", id);
	g_assert_cmpuint(request(fixture, "POST", approve, fixture->editor,
	                         "application/json", "", &body, NULL), ==, 422);
	assert_refusal(body, "assignee", "outsider");
}

/*
 * Any writer at all: a save straight to the database -- an automation, a
 * plugin, the assistant -- meets the same validator, with the field and
 * the value in the message.
 */
static void
test_direct_save(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "From a plugin", "assignee", "oxpeker", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(ticket), fixture->home);

	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(ticket), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_true(g_str_has_prefix(error->message, "assignee: \"oxpeker\""));
}

/* ==========================================================================
 * Rotas and macros
 * ========================================================================== */

/*
 * A routing rule's rota is checked name by name: one bad name refuses the
 * rule and is named; a rota of members saves. A macro may say {me}, and
 * nothing else that is not a member.
 */
static void
test_routing_rule_and_macro(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *json = NULL;
	g_autofree gchar *body = NULL;

	(void)user_data;

	json = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ","
	                       "\"name\":\"Rota\",\"strategy\":\"round_robin\","
	                       "\"assignees\":\"oxpecker, oxpeker\",\"active\":true}",
	                       fixture->home);
	g_assert_cmpuint(json_as_editor(fixture, "POST", "/api/v1/routing_rule",
	                                json, &body), ==, 422);
	assert_refusal(body, "assignees", "oxpeker");
	g_clear_pointer(&json, g_free);
	g_clear_pointer(&body, g_free);

	json = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ","
	                       "\"name\":\"Rota\",\"strategy\":\"round_robin\","
	                       "\"assignees\":\"oxpecker, editor\",\"active\":true}",
	                       fixture->home);
	g_assert_cmpuint(json_as_editor(fixture, "POST", "/api/v1/routing_rule",
	                                json, NULL), ==, SOUP_STATUS_CREATED);
	g_clear_pointer(&json, g_free);

	json = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ","
	                       "\"name\":\"Take it\",\"assignee\":\"{me}\","
	                       "\"active\":true}", fixture->home);
	g_assert_cmpuint(json_as_editor(fixture, "POST", "/api/v1/macro", json,
	                                NULL), ==, SOUP_STATUS_CREATED);
	g_clear_pointer(&json, g_free);

	json = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ","
	                       "\"name\":\"Give it away\",\"assignee\":\"outsider\","
	                       "\"active\":true}", fixture->home);
	g_assert_cmpuint(json_as_editor(fixture, "POST", "/api/v1/macro", json,
	                                &body), ==, 422);
	assert_refusal(body, "assignee", "outsider");

	/* {me} is a template's word, not an assignee: a ticket refuses it. */
	{
		g_autoptr(VentureTicket) ticket = NULL;
		g_autoptr(GError) error = NULL;

		ticket = venture_ticket_new();
		g_object_set(ticket, "title", "Literal", "assignee", "{me}", NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(ticket), fixture->home);
		g_assert_false(venture_database_save(fixture->database,
		                                     VENTURE_ENTITY(ticket), NULL,
		                                     &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}
}

/*
 * A rota hands a ticket only to somebody who may take it. A rule matches
 * by kind, priority and tag, not organization, so elsewhere's rota of
 * "outsider" covers a ticket raised in home: the ticket must arrive
 * unassigned, not be refused. And in its own organization the rota
 * skips a name that has since left, to the next one.
 */
static void
test_routing_skips_non_members(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureRoutingRule) rule = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	gint64 id;

	(void)user_data;

	rule = venture_routing_rule_new();
	g_object_set(rule, "name", "Their rota", "assignees", "outsider",
	             "all-kinds", TRUE, "all-priorities", TRUE,
	             "strategy", VENTURE_ROUTING_STRATEGY_ROUND_ROBIN,
	             "active", TRUE, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(rule), fixture->elsewhere);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(rule),
	                                    NULL, NULL));

	id = make_ticket(fixture, fixture->home, "Raised at home", NULL);
	first = stored_assignee(fixture, id);
	g_assert_true(venture_string_is_empty(first));

	/* In elsewhere, the rota's name is a member and gets it. */
	id = make_ticket(fixture, fixture->elsewhere, "Raised there", NULL);
	second = stored_assignee(fixture, id);
	g_assert_cmpstr(second, ==, "outsider");

	/* Outsider leaves: the next ticket there arrives with nobody. */
	g_clear_pointer(&second, g_free);
	{
		g_autoptr(VentureQuery) query = NULL;

		query = venture_query_new(VENTURE_TYPE_USER);
		g_assert_true(venture_query_add_filter_string(query, "username",
			VENTURE_FILTER_OP_EQ, "outsider", NULL));
		user = venture_database_find_one(fixture->database, query, NULL);
	}
	g_assert_nonnull(user);
	g_object_set(user, "active", FALSE, NULL);
	g_assert_true(venture_database_save(fixture->database, user, NULL, NULL));

	id = make_ticket(fixture, fixture->elsewhere, "Raised later", NULL);
	second = stored_assignee(fixture, id);
	g_assert_true(venture_string_is_empty(second));
}

/* ==========================================================================
 * The lookup
 * ========================================================================== */

/* The usernames in a members answer, comma separated, in order. */
static gchar *
member_names(const gchar *body)
{
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GString) names = NULL;
	JsonArray *records;
	guint i;

	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);
	records = json_object_get_array_member(json_node_get_object(node),
	                                       "records");
	names = g_string_new(NULL);

	for (i = 0; i < json_array_get_length(records); i++)
	{
		JsonObject *row;

		row = json_array_get_object_element(records, i);

		/* Nothing about a person beyond the two names. */
		g_assert_false(json_object_has_member(row, "email"));
		g_assert_false(json_object_has_member(row, "role"));
		g_assert_false(json_object_has_member(row, "id"));

		if (i > 0)
			g_string_append_c(names, ',');

		g_string_append(names, json_object_get_string_member(row, "username"));
	}

	return g_string_free(g_steal_pointer(&names), FALSE);
}

/*
 * "ox" finds oxpecker, case and all; the display name is searched too;
 * nobody switched off, lapsed or in another organization is listed; an
 * organization the caller is not a member of answers 404, as one that
 * does not exist does; and the list is paged.
 */
static void
test_lookup(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *names = NULL;
	g_autoptr(JsonNode) node = NULL;

	(void)user_data;

	path = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT
	                       "/members?q=ox", fixture->home);
	g_assert_cmpuint(request(fixture, "GET", path, fixture->editor, NULL, NULL,
	                         &body, NULL), ==, SOUP_STATUS_OK);
	names = member_names(body);
	g_assert_cmpstr(names, ==, "oxpecker");
	g_clear_pointer(&path, g_free);
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&names, g_free);

	/* Case, and the display name. */
	path = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT
	                       "/members?q=RED-BILLED", fixture->home);
	g_assert_cmpuint(request(fixture, "GET", path, fixture->editor, NULL, NULL,
	                         &body, NULL), ==, SOUP_STATUS_OK);
	names = member_names(body);
	g_assert_cmpstr(names, ==, "oxpecker");
	g_clear_pointer(&path, g_free);
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&names, g_free);

	/* Everybody home: not outsider, retired or lapsed. */
	path = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT
	                       "/members", fixture->home);
	g_assert_cmpuint(request(fixture, "GET", path, fixture->editor, NULL, NULL,
	                         &body, NULL), ==, SOUP_STATUS_OK);
	names = member_names(body);
	g_assert_cmpstr(names, ==, "editor,oxpecker");
	g_clear_pointer(&path, g_free);
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&names, g_free);

	/* Paged. */
	path = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT
	                       "/members?limit=1&offset=1", fixture->home);
	g_assert_cmpuint(request(fixture, "GET", path, fixture->editor, NULL, NULL,
	                         &body, NULL), ==, SOUP_STATUS_OK);
	names = member_names(body);
	g_assert_cmpstr(names, ==, "oxpecker");
	node = venture_json_parse(body, NULL);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node),
	                                           "total"), ==, 2);
	g_clear_pointer(&path, g_free);
	g_clear_pointer(&body, g_free);

	/* Another organization's people are not the editor's to list. */
	path = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT
	                       "/members?q=out", fixture->elsewhere);
	g_assert_cmpuint(request(fixture, "GET", path, fixture->editor, NULL, NULL,
	                         &body, NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_null(strstr(body, "outsider"));
	g_clear_pointer(&path, g_free);
	g_clear_pointer(&body, g_free);

	g_assert_cmpuint(request(fixture, "GET",
	                         "/api/v1/organizations/999999/members", fixture->editor,
	                         NULL, NULL, NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(request(fixture, "GET",
	                         "/api/v1/organizations/1/members?limit=0",
	                         fixture->editor, NULL, NULL, NULL, NULL), ==, 422);
}

/* ==========================================================================
 * The pages
 * ========================================================================== */

/*
 * The ticket form draws the assignee as a <select> of members -- the
 * no-script fallback and what venture.js turns into the combobox -- with
 * "Unassigned" and the reader as "Me", and nobody from elsewhere. A stale
 * stored name stays the selected choice, marked, so saving the form
 * unchanged does not unassign it. The board card and the Desk card each
 * carry a quick-assign form.
 */
static void
test_pages(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *action = NULL;
	g_autoptr(VentureEntity) user = NULL;
	gint64 id;

	(void)user_data;

	g_assert_cmpuint(request(fixture, "GET", "/e/ticket/new", fixture->editor,
	                         NULL, NULL, &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "<select name=\"assignee\" data-picker-search "
	                              "data-member-picker>"));
	g_assert_nonnull(strstr(page, "<option value=\"\" selected>Unassigned"));
	g_assert_nonnull(strstr(page, "<option value=\"editor\">Me \xe2\x80\x94 editor"));
	g_assert_nonnull(strstr(page, "<option value=\"oxpecker\">oxpecker "
	                              "(Red-billed Oxpecker)</option>"));
	g_assert_null(strstr(page, "value=\"outsider\""));
	g_assert_null(strstr(page, "value=\"retired\""));
	g_clear_pointer(&page, g_free);

	/* The rota's box names who may be in it. */
	g_assert_cmpuint(request(fixture, "GET", "/e/routing_rule/new",
	                         fixture->editor, NULL, NULL, &page, NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "Members: editor, oxpecker"));
	g_clear_pointer(&page, g_free);

	/* A stale name, kept and marked. */
	id = make_ticket(fixture, fixture->home, "Ox's old one", "oxpecker");
	user = venture_database_get(fixture->database, VENTURE_TYPE_USER,
	                            fixture->oxpecker_id, NULL);
	g_object_set(user, "active", FALSE, NULL);
	g_assert_true(venture_database_save(fixture->database, user, NULL, NULL));

	path = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT "/edit", id);
	g_assert_cmpuint(request(fixture, "GET", path, fixture->editor, NULL, NULL,
	                         &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "<option value=\"oxpecker\" selected>oxpecker "
	                              "\xe2\x80\x94 not a member</option>"));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&path, g_free);

	/* The Desk card. */
	action = g_strdup_printf("action=\"/tickets/%" G_GINT64_FORMAT "/assign\"", id);
	path = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, id);
	g_assert_cmpuint(request(fixture, "GET", path, fixture->editor, NULL, NULL,
	                         &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, action));
	g_clear_pointer(&page, g_free);

	/* The board. */
	g_assert_cmpuint(request(fixture, "GET", "/tickets?view=board",
	                         fixture->editor, NULL, NULL, &page, NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, action));
	g_assert_nonnull(strstr(page, "name=\"from\" value=\"board\""));
}

/*
 * The quick-assign action saves through the same rule: a member is
 * assigned and the browser sent back; the typo is the 422; a post with
 * no assignee at all is refused rather than read as "nobody".
 */
static void
test_assign_action(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *stored = NULL;
	gint64 id;

	(void)user_data;

	id = make_ticket(fixture, fixture->home, "Quick", NULL);
	path = g_strdup_printf("/tickets/%" G_GINT64_FORMAT "/assign", id);

	g_assert_cmpuint(form_as_editor(fixture, path, "assignee=oxpecker&from=board",
	                                NULL), ==, SOUP_STATUS_FOUND);
	stored = stored_assignee(fixture, id);
	g_assert_cmpstr(stored, ==, "oxpecker");
	g_clear_pointer(&stored, g_free);

	g_assert_cmpuint(form_as_editor(fixture, path, "assignee=oxpeker&async=1",
	                                &body), ==, 422);
	assert_refusal(body, "assignee", "oxpeker");
	g_clear_pointer(&body, g_free);

	g_assert_cmpuint(form_as_editor(fixture, path, "from=board&async=1", &body),
	                 ==, 422);
	stored = stored_assignee(fixture, id);
	g_assert_cmpstr(stored, ==, "oxpecker");
	g_clear_pointer(&stored, g_free);

	g_assert_cmpuint(form_as_editor(fixture, path, "assignee=&async=1", NULL),
	                 ==, SOUP_STATUS_OK);
	stored = stored_assignee(fixture, id);
	g_assert_true(venture_string_is_empty(stored));
}

/* ==========================================================================
 * Without organization access
 * ========================================================================== */

/*
 * With organization access off there are no memberships: every active
 * user reads every organization, so every active user may be assigned --
 * and a switched-off one still may not.
 */
static void
test_without_orgaccess(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureUser) active = NULL;
	g_autoptr(VentureUser) inactive = NULL;
	g_autoptr(GError) error = NULL;
	gint64 home;

	config = venture_config_new();
	database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	venture_config_set_module_enabled(config, "orgaccess", FALSE);
	g_assert_true(venture_database_migrate(database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	home = venture_context_get_default_organization_id(context);

	active = venture_user_new();
	g_object_set(active, "username", "solo", "role", VENTURE_USER_ROLE_EDITOR,
	             "active", TRUE, NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(active), NULL,
	                                    NULL));
	inactive = venture_user_new();
	g_object_set(inactive, "username", "gone", "role", VENTURE_USER_ROLE_EDITOR,
	             "active", FALSE, NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(inactive),
	                                    NULL, NULL));

	g_assert_true(venture_assignee_is_member(database, home, "solo"));
	g_assert_false(venture_assignee_is_member(database, home, "gone"));
	g_assert_false(venture_assignee_is_member(database, home, "nobody"));
	g_assert_true(venture_assignee_check(database, home, "assignee", " solo ",
	                                     VENTURE_COLUMN_FLAG_MEMBER_USERNAME,
	                                     NULL));

	g_clear_object(&context);
	module_registry_reset();
}

int
main(
	int	 argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/assignee/rest-create", test_rest_create);
	ADD("/assignee/rest-update", test_rest_update);
	ADD("/assignee/stale-assignee-stays-editable",
	    test_stale_assignee_stays_editable);
	ADD("/assignee/staged-write-judged-when-applied",
	    test_staged_write_judged_when_applied);
	ADD("/assignee/direct-save", test_direct_save);
	ADD("/assignee/routing-rule-and-macro", test_routing_rule_and_macro);
	ADD("/assignee/routing-skips-non-members", test_routing_skips_non_members);
	ADD("/assignee/lookup", test_lookup);
	ADD("/assignee/pages", test_pages);
	ADD("/assignee/assign-action", test_assign_action);
	g_test_add_func("/assignee/without-orgaccess", test_without_orgaccess);

#undef ADD

	return g_test_run();
}
