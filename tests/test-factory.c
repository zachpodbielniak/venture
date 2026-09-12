/*
 * test-factory.c - The software factory
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The loop from a ticket to a running release and back: milestones,
 * releases, builds reported by the forge's CI, environments, deployments
 * and incidents. Everything here is about the two paths that write into
 * it without a person -- the webhook and the changelog draft -- and the
 * reports that read the loop end to end.
 */

#include <venture.h>

#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

#define FACTORY_SECRET "hook-secret-for-tests"

/* --- Parsing, with no server ---------------------------------------------- */

/*
 * The factory module owns its six types and requires the forge.
 */
static void
test_factory_module_owns_its_types(void)
{
	g_autoptr(VentureModuleRegistry) registry = NULL;
	VentureModule *factory;
	const gchar *const *types;

	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);

	factory = venture_module_registry_lookup(registry, "factory");
	g_assert_nonnull(factory);
	g_assert_true(g_strv_contains(venture_module_get_requires(factory),
	                              "forge"));

	types = venture_module_get_entity_names(factory);
	g_assert_cmpuint(g_strv_length((gchar **)types), ==, 6);
	g_assert_true(g_strv_contains(types, "release"));
	g_assert_true(g_strv_contains(types, "incident"));

	g_assert_cmpstr(venture_module_get_name(
		venture_module_registry_get_module_for_type(registry, "build")), ==,
		"factory");
}

/*
 * A workflow_run delivery, as Forgejo Actions sends it, is read in full.
 */
static void
test_factory_parses_a_workflow_run(void)
{
	static const gchar *const payload =
		"{\"action\":\"completed\","
		" \"workflow_run\":{\"id\":9001,\"run_number\":17,"
		"   \"display_title\":\"fix: the thing\",\"head_branch\":\"main\","
		"   \"head_sha\":\"abc123\",\"status\":\"completed\","
		"   \"conclusion\":\"success\","
		"   \"html_url\":\"https://git.example.com/zach/venture/actions/runs/9001\","
		"   \"started_at\":\"2026-09-01T10:00:00Z\","
		"   \"updated_at\":\"2026-09-01T10:05:00Z\"},"
		" \"workflow\":{\"name\":\"CI\"},"
		" \"repository\":{\"full_name\":\"zach/venture\"},"
		" \"sender\":{\"login\":\"zach\"}}";
	g_autoptr(VentureForgejoClient) client = NULL;
	g_autoptr(SoupMessageHeaders) headers = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	VentureForgeWorkflowEvent event;

	client = venture_forgejo_client_new("https://git.example.com", "tok", 5,
	                                    &error);
	g_assert_no_error(error);

	parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, payload, -1, &error));

	headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_REQUEST);
	soup_message_headers_replace(headers, "X-Forgejo-Delivery", "d-1");

	g_assert_true(venture_forge_client_parse_workflow_event(
		VENTURE_FORGE_CLIENT(client), json_parser_get_root(parser), headers,
		&event, &error));
	g_assert_no_error(error);

	g_assert_cmpstr(event.action, ==, "completed");
	g_assert_cmpstr(event.repo_full_name, ==, "zach/venture");
	g_assert_cmpint(event.run_id, ==, 9001);
	g_assert_cmpint(event.run_number, ==, 17);
	g_assert_cmpstr(event.workflow_name, ==, "CI");
	g_assert_cmpstr(event.title, ==, "fix: the thing");
	g_assert_cmpstr(event.head_branch, ==, "main");
	g_assert_cmpstr(event.head_sha, ==, "abc123");
	g_assert_cmpstr(event.conclusion, ==, "success");
	g_assert_cmpstr(event.started_at, ==, "2026-09-01T10:00:00Z");
	g_assert_cmpstr(event.delivery_id, ==, "d-1");

	venture_forge_workflow_event_clear(&event);
	venture_forge_workflow_event_clear(&event);

	/* A payload with no run is refused rather than half-read. */
	g_clear_object(&parser);
	parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser,
		"{\"action\":\"completed\",\"repository\":{\"full_name\":\"x/y\"}}",
		-1, NULL));
	g_assert_false(venture_forge_client_parse_workflow_event(
		VENTURE_FORGE_CLIENT(client), json_parser_get_root(parser), headers,
		&event, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	venture_forge_workflow_event_clear(&event);
}

/*
 * A release delivery is read in full, and one with no tag is refused.
 */
static void
test_factory_parses_a_release(void)
{
	static const gchar *const payload =
		"{\"action\":\"published\","
		" \"release\":{\"id\":55,\"tag_name\":\"v1.2.0\",\"name\":\"1.2.0\","
		"   \"body\":\"notes\",\"draft\":false,\"prerelease\":true,"
		"   \"html_url\":\"https://git.example.com/zach/venture/releases/tag/v1.2.0\","
		"   \"published_at\":\"2026-09-02T09:00:00Z\"},"
		" \"repository\":{\"full_name\":\"zach/venture\"},"
		" \"sender\":{\"login\":\"zach\"}}";
	g_autoptr(VentureForgejoClient) client = NULL;
	g_autoptr(SoupMessageHeaders) headers = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	VentureForgeReleaseEvent event;

	client = venture_forgejo_client_new("https://git.example.com", "tok", 5,
	                                    &error);
	parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, payload, -1, &error));
	headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_REQUEST);

	g_assert_true(venture_forge_client_parse_release_event(
		VENTURE_FORGE_CLIENT(client), json_parser_get_root(parser), headers,
		&event, &error));
	g_assert_no_error(error);
	g_assert_cmpint(event.release_id, ==, 55);
	g_assert_cmpstr(event.tag, ==, "v1.2.0");
	g_assert_cmpstr(event.body, ==, "notes");
	g_assert_true(event.prerelease);
	g_assert_false(event.draft);
	g_assert_cmpstr(event.published_at, ==, "2026-09-02T09:00:00Z");
	venture_forge_release_event_clear(&event);

	g_clear_object(&parser);
	parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser,
		"{\"action\":\"published\",\"release\":{\"id\":1},"
		"\"repository\":{\"full_name\":\"x/y\"}}", -1, NULL));
	g_assert_false(venture_forge_client_parse_release_event(
		VENTURE_FORGE_CLIENT(client), json_parser_get_root(parser), headers,
		&event, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	venture_forge_release_event_clear(&event);
}

/* --- Over HTTP ------------------------------------------------------------ */

typedef struct
{
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gchar			*state_dir;
	gchar			*cookie;
	guint16			 port;
	gint64			 forge_id;
	gint64			 repo_id;
} ServerFixture;

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
 * One request with any body and any headers. @headers is name, value,
 * name, value, ..., NULL.
 */
static guint
server_request_full(
	ServerFixture		 *fixture,
	const gchar		 *method,
	const gchar		 *path,
	const gchar		 *content_type,
	const gchar		 *body,
	const gchar *const	 *headers,
	gchar			**out_body
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };
	gsize h;

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != fixture->cookie)
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie",
			fixture->cookie);

	for (h = 0; (NULL != headers) && (NULL != headers[h]); h += 2)
		soup_message_headers_replace(
			soup_message_get_request_headers(message), headers[h],
			headers[h + 1]);

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

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

static guint
server_request(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *form_body,
	gchar		**out_body
){
	return server_request_full(fixture, method, path,
	                           "application/x-www-form-urlencoded", form_body,
	                           NULL, out_body);
}

/*
 * Delivers a signed webhook the way Forgejo would.
 */
static guint
server_deliver_webhook(
	ServerFixture	*fixture,
	const gchar	*event_name,
	const gchar	*delivery,
	const gchar	*payload
){
	g_autofree gchar *signature = NULL;
	g_autofree gchar *path = NULL;
	const gchar *headers[7];

	signature = g_compute_hmac_for_data(G_CHECKSUM_SHA256,
		(const guchar *)FACTORY_SECRET, strlen(FACTORY_SECRET),
		(const guchar *)payload, strlen(payload));
	path = g_strdup_printf("/hooks/forge/%" G_GINT64_FORMAT, fixture->forge_id);

	headers[0] = "X-Forgejo-Event";
	headers[1] = event_name;
	headers[2] = "X-Forgejo-Signature";
	headers[3] = signature;
	headers[4] = "X-Forgejo-Delivery";
	headers[5] = delivery;
	headers[6] = NULL;

	return server_request_full(fixture, "POST", path, "application/json",
	                           payload, headers, NULL);
}

static void
server_fixture_set_up(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(VentureForgeRepo) repo = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	(void)user_data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "factory-test-secret", TRUE);

	fixture->state_dir = g_dir_make_tmp("venture-factory-XXXXXX", NULL);
	fixture->port = (guint16)(20000 + ((getpid() + 8191) % 20000));

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)fixture->port,
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

	fixture->session = soup_session_new();

	user = venture_user_new();
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER,
	             "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 100000,
	                                        NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	g_assert_cmpuint(server_request_full(fixture, "POST", "/login",
		"application/x-www-form-urlencoded",
		"username=owner&password=owner-password-1", NULL, NULL), ==,
		SOUP_STATUS_FOUND);

	/* The cookie is read back through a second login, because the helper
	 * above does not return headers; a fixture this small does not need
	 * a second helper for it. */
	{
		g_autoptr(SoupMessage) message = NULL;
		g_autofree gchar *url = NULL;
		g_autoptr(GBytes) bytes = NULL;
		RequestResult outcome = { FALSE, NULL, NULL };

		url = g_strdup_printf("http://127.0.0.1:%u/login", fixture->port);
		message = soup_message_new("POST", url);
		soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
		bytes = g_bytes_new_static("username=owner&password=owner-password-1",
		                           strlen("username=owner&password=owner-password-1"));
		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
		soup_session_send_and_read_async(fixture->session, message,
		                                 G_PRIORITY_DEFAULT, NULL,
		                                 request_done, &outcome);

		while (!outcome.done)
			g_main_context_iteration(NULL, TRUE);

		g_clear_pointer(&outcome.body, g_bytes_unref);
		g_clear_error(&outcome.error);

		set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));
	}

	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	fixture->cookie = g_steal_pointer(&set_cookie);

	/* A forge with a secret, and one enrolled repository. */
	forge = venture_forge_new();
	g_object_set(forge,
	             "name", "Example forge",
	             "kind", VENTURE_FORGE_KIND_FORGEJO,
	             "base-url", "https://git.example.com",
	             "token", "tok",
	             "webhook-secret", FACTORY_SECRET,
	             "bot-username", "venture-bot",
	             "active", TRUE,
	             NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(forge), NULL, &error));
	g_assert_no_error(error);
	fixture->forge_id = venture_entity_get_id(VENTURE_ENTITY(forge));

	/* The secret is a sensitive field, which keeps it out of every
	 * response; it must still round-trip through the database, or no
	 * webhook could ever verify. */
	{
		g_autoptr(VentureEntity) again = NULL;
		g_autofree gchar *stored = NULL;

		again = venture_database_get(fixture->database, VENTURE_TYPE_FORGE,
		                             fixture->forge_id, NULL);
		g_object_get(again, "webhook-secret", &stored, NULL);
		g_assert_cmpstr(stored, ==, FACTORY_SECRET);
	}

	repo = venture_forge_repo_new();
	g_object_set(repo,
	             "name", "zach/venture",
	             "forge-id", fixture->forge_id,
	             "default-branch", "main",
	             "accept-issues", TRUE,
	             NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(repo), NULL, &error));
	g_assert_no_error(error);
	fixture->repo_id = venture_entity_get_id(VENTURE_ENTITY(repo));
}

static void
server_fixture_tear_down(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_pointer(&fixture->cookie, g_free);
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

	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}

/*
 * Reports scope to the default organisation, as every page does; a record
 * made by hand has to be filed against it or the reports will not see it.
 */
static void
file_under_default(
	ServerFixture	*fixture,
	gpointer	 record
){
	venture_entity_set_organization_id(VENTURE_ENTITY(record),
		venture_context_get_default_organization_id(fixture->context));
}

static gint64
count_of(
	ServerFixture	*fixture,
	GType		 type
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(type);

	return venture_database_count(fixture->database, query, NULL);
}

/*
 * A workflow run becomes a build when it is requested and updates the
 * same build when it completes -- and a retried delivery updates rather
 * than duplicates.
 *
 * What breaks if this regresses: two builds per CI run, one of them
 * forever queued.
 */
static void
test_factory_workflow_run_becomes_a_build(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	static const gchar *const requested =
		"{\"action\":\"requested\","
		" \"workflow_run\":{\"id\":9001,\"run_number\":17,"
		"   \"display_title\":\"feat: builds\",\"head_branch\":\"main\","
		"   \"head_sha\":\"abc123\",\"status\":\"queued\",\"conclusion\":\"\","
		"   \"html_url\":\"https://git.example.com/zach/venture/actions/runs/9001\","
		"   \"started_at\":\"2026-09-01T10:00:00Z\","
		"   \"updated_at\":\"2026-09-01T10:00:00Z\"},"
		" \"workflow\":{\"name\":\"CI\"},"
		" \"repository\":{\"full_name\":\"zach/venture\"},"
		" \"sender\":{\"login\":\"zach\"}}";
	static const gchar *const completed =
		"{\"action\":\"completed\","
		" \"workflow_run\":{\"id\":9001,\"run_number\":17,"
		"   \"display_title\":\"feat: builds\",\"head_branch\":\"main\","
		"   \"head_sha\":\"abc123\",\"status\":\"completed\","
		"   \"conclusion\":\"failure\","
		"   \"html_url\":\"https://git.example.com/zach/venture/actions/runs/9001\","
		"   \"started_at\":\"2026-09-01T10:00:00Z\","
		"   \"updated_at\":\"2026-09-01T10:04:00Z\"},"
		" \"workflow\":{\"name\":\"CI\"},"
		" \"repository\":{\"full_name\":\"zach/venture\"},"
		" \"sender\":{\"login\":\"zach\"}}";
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) builds = NULL;
	g_autoptr(GDateTime) finished = NULL;
	g_autofree gchar *workflow = NULL;
	g_autofree gchar *external = NULL;
	VentureEntity *build;
	VentureBuildStatus status;
	VentureBuildTrigger trigger;
	gint64 number = 0;

	(void)user_data;

	g_assert_cmpuint(server_deliver_webhook(fixture, "workflow_run", "d-1",
	                                        requested), ==,
	                 SOUP_STATUS_ACCEPTED);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_BUILD), ==, 1);

	query = venture_query_new(VENTURE_TYPE_BUILD);
	builds = venture_database_find(fixture->database, query, NULL);
	build = g_ptr_array_index(builds, 0);
	g_object_get(build, "status", &status, "trigger", &trigger,
	             "workflow", &workflow, "external-id", &external,
	             "number", &number, "finished-at", &finished, NULL);
	g_assert_cmpint(status, ==, VENTURE_BUILD_STATUS_QUEUED);
	g_assert_cmpint(trigger, ==, VENTURE_BUILD_TRIGGER_WEBHOOK);
	g_assert_cmpstr(workflow, ==, "CI");
	g_assert_cmpstr(external, ==, "9001");
	g_assert_cmpint(number, ==, 17);
	g_assert_null(finished);

	/* Completed, then the same delivery again. */
	g_assert_cmpuint(server_deliver_webhook(fixture, "workflow_run", "d-2",
	                                        completed), ==,
	                 SOUP_STATUS_ACCEPTED);
	g_assert_cmpuint(server_deliver_webhook(fixture, "workflow_run", "d-2",
	                                        completed), ==,
	                 SOUP_STATUS_ACCEPTED);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_BUILD), ==, 1);

	g_clear_pointer(&builds, g_ptr_array_unref);
	builds = venture_database_find(fixture->database, query, NULL);
	build = g_ptr_array_index(builds, 0);
	g_object_get(build, "status", &status, "finished-at", &finished, NULL);
	g_assert_cmpint(status, ==, VENTURE_BUILD_STATUS_FAILED);
	g_assert_nonnull(finished);

	/* A bad signature is refused, so a build cannot be planted. */
	{
		g_autofree gchar *path = NULL;
		const gchar *headers[] = {
			"X-Forgejo-Event", "workflow_run",
			"X-Forgejo-Signature", "0000",
			NULL
		};

		path = g_strdup_printf("/hooks/forge/%" G_GINT64_FORMAT,
		                       fixture->forge_id);

		/* The refusal is logged as a warning, which the test harness
		 * makes fatal unless it is expected. */
		g_test_expect_message("Venture", G_LOG_LEVEL_WARNING,
		                      "venture_forge: refusing a webhook*");
		g_assert_cmpuint(server_request_full(fixture, "POST", path,
			"application/json", requested, headers, NULL), ==,
			SOUP_STATUS_UNAUTHORIZED);
		g_test_assert_expected_messages();
	}
}

/*
 * A release cut on the forge becomes a release record, its version read
 * from the tag, and a later delivery for the same release updates it.
 */
static void
test_factory_forge_release_becomes_a_release(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	static const gchar *const published =
		"{\"action\":\"published\","
		" \"release\":{\"id\":55,\"tag_name\":\"v1.2.0\",\"name\":\"Summer\","
		"   \"body\":\"- fixed things\",\"draft\":false,\"prerelease\":false,"
		"   \"html_url\":\"https://git.example.com/zach/venture/releases/tag/v1.2.0\","
		"   \"published_at\":\"2026-09-02T09:00:00Z\"},"
		" \"repository\":{\"full_name\":\"zach/venture\"},"
		" \"sender\":{\"login\":\"zach\"}}";
	static const gchar *const deleted =
		"{\"action\":\"deleted\","
		" \"release\":{\"id\":55,\"tag_name\":\"v1.2.0\",\"name\":\"Summer\","
		"   \"body\":\"\",\"draft\":false,\"prerelease\":false,"
		"   \"html_url\":\"\",\"published_at\":null},"
		" \"repository\":{\"full_name\":\"zach/venture\"},"
		" \"sender\":{\"login\":\"zach\"}}";
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) releases = NULL;
	g_autoptr(GDateTime) released_at = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *changelog = NULL;
	g_autofree gchar *tag = NULL;
	VentureEntity *release;
	VentureReleaseStatus status;
	gint64 external = 0;
	gint64 repo_id = 0;

	(void)user_data;

	g_assert_cmpuint(server_deliver_webhook(fixture, "release", "r-1",
	                                        published), ==,
	                 SOUP_STATUS_ACCEPTED);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_RELEASE), ==, 1);

	query = venture_query_new(VENTURE_TYPE_RELEASE);
	releases = venture_database_find(fixture->database, query, NULL);
	release = g_ptr_array_index(releases, 0);
	g_object_get(release, "number", &version, "tag", &tag,
	             "status", &status, "changelog", &changelog,
	             "external-id", &external, "repo-id", &repo_id,
	             "released-at", &released_at, NULL);
	g_assert_cmpstr(version, ==, "1.2.0");
	g_assert_cmpstr(tag, ==, "v1.2.0");
	g_assert_cmpint(status, ==, VENTURE_RELEASE_STATUS_RELEASED);
	g_assert_cmpstr(changelog, ==, "- fixed things");
	g_assert_cmpint(external, ==, 55);
	g_assert_cmpint(repo_id, ==, fixture->repo_id);
	g_assert_nonnull(released_at);
	g_assert_cmpint(g_date_time_get_year(released_at), ==, 2026);

	/* Deleting it on the forge yanks it here, on the same row. */
	g_assert_cmpuint(server_deliver_webhook(fixture, "release", "r-2",
	                                        deleted), ==,
	                 SOUP_STATUS_ACCEPTED);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_RELEASE), ==, 1);

	g_clear_pointer(&releases, g_ptr_array_unref);
	releases = venture_database_find(fixture->database, query, NULL);
	g_object_get(g_ptr_array_index(releases, 0), "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_RELEASE_STATUS_YANKED);
}

static gint64
create_release(
	ServerFixture	*fixture,
	const gchar	*version,
	GDateTime	*released_at
){
	g_autoptr(VentureRelease) release = NULL;

	release = venture_release_new();
	g_object_set(release, "number", version,
	             "status", (NULL != released_at)
	                     ? VENTURE_RELEASE_STATUS_RELEASED
	                     : VENTURE_RELEASE_STATUS_PLANNED,
	             NULL);

	if (NULL != released_at)
		g_object_set(release, "released-at", released_at, NULL);

	file_under_default(fixture, release);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(release), NULL, NULL));

	return venture_entity_get_id(VENTURE_ENTITY(release));
}

static gint64
create_ticket(
	ServerFixture		*fixture,
	const gchar		*title,
	VentureIssueType	 issue_type,
	gint64			 release_id
){
	g_autoptr(VentureTicket) ticket = NULL;

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", title, "issue-type", issue_type,
	             "release-id", release_id, NULL);
		file_under_default(fixture, ticket);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, NULL));

	return venture_entity_get_id(VENTURE_ENTITY(ticket));
}

/*
 * The changelog is drafted from the tickets marked as fixed in the
 * release, grouped by issue type; a changelog somebody wrote is kept
 * unless they ask for it to be replaced.
 */
static void
test_factory_changelog_is_drafted_from_tickets(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) release = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *changelog = NULL;
	g_autofree gchar *page = NULL;
	gint64 release_id;

	(void)user_data;

	release_id = create_release(fixture, "2.0.0", NULL);
	create_ticket(fixture, "Crash on save", VENTURE_ISSUE_TYPE_BUG, release_id);
	create_ticket(fixture, "Dark mode", VENTURE_ISSUE_TYPE_STORY, release_id);
	create_ticket(fixture, "Unrelated", VENTURE_ISSUE_TYPE_TASK, 0);

	path = g_strdup_printf("/releases/%" G_GINT64_FORMAT "/changelog",
	                       release_id);

	g_assert_cmpuint(server_request(fixture, "POST", path, "", NULL), ==,
	                 SOUP_STATUS_FOUND);

	release = venture_database_get(fixture->database, VENTURE_TYPE_RELEASE,
	                               release_id, NULL);
	g_object_get(release, "changelog", &changelog, NULL);
	g_assert_nonnull(strstr(changelog, "## 2.0.0"));
	g_assert_nonnull(strstr(changelog, "### Bug"));
	g_assert_nonnull(strstr(changelog, "Crash on save"));
	g_assert_nonnull(strstr(changelog, "### Story"));
	g_assert_nonnull(strstr(changelog, "Dark mode"));
	g_assert_null(strstr(changelog, "Unrelated"));

	/* Edited by hand, and a plain redraft leaves it alone... */
	g_object_set(release, "changelog", "hand-written", NULL);
	g_assert_true(venture_database_save(fixture->database, release, NULL,
	                                    NULL));
	g_assert_cmpuint(server_request(fixture, "POST", path, "", NULL), ==,
	                 SOUP_STATUS_FOUND);
	g_clear_object(&release);
	release = venture_database_get(fixture->database, VENTURE_TYPE_RELEASE,
	                               release_id, NULL);
	g_clear_pointer(&changelog, g_free);
	g_object_get(release, "changelog", &changelog, NULL);
	g_assert_cmpstr(changelog, ==, "hand-written");

	/* ...unless asked. */
	g_assert_cmpuint(server_request(fixture, "POST", path, "replace=1", NULL),
	                 ==, SOUP_STATUS_FOUND);
	g_clear_object(&release);
	release = venture_database_get(fixture->database, VENTURE_TYPE_RELEASE,
	                               release_id, NULL);
	g_clear_pointer(&changelog, g_free);
	g_object_get(release, "changelog", &changelog, NULL);
	g_assert_nonnull(strstr(changelog, "Crash on save"));

	/* The same draft over the API, which is what an agent calls: the
	 * answer says whether anything changed, and a bare call leaves a
	 * written changelog alone as the form does. */
	{
		g_autofree gchar *api_path = NULL;
		g_autofree gchar *body = NULL;
		g_autoptr(JsonNode) node = NULL;
		JsonObject *reply;

		g_object_set(release, "changelog", "hand-written again", NULL);
		g_assert_true(venture_database_save(fixture->database, release, NULL,
		                                    NULL));
		api_path = g_strdup_printf("/api/v1/releases/%" G_GINT64_FORMAT
		                           "/changelog", release_id);

		g_assert_cmpuint(server_request_full(fixture, "POST", api_path,
			"application/json", "{}", NULL, &body), ==, SOUP_STATUS_OK);
		node = venture_json_parse(body, NULL);
		reply = json_node_get_object(node);
		g_assert_false(json_object_get_boolean_member(reply, "changed"));

		g_clear_pointer(&body, g_free);
		g_clear_pointer(&node, json_node_unref);
		g_assert_cmpuint(server_request_full(fixture, "POST", api_path,
			"application/json", "{\"replace\": true}", NULL, &body), ==,
			SOUP_STATUS_OK);
		node = venture_json_parse(body, NULL);
		reply = json_node_get_object(node);
		g_assert_true(json_object_get_boolean_member(reply, "changed"));
		g_assert_nonnull(strstr(json_object_get_string_member(
			json_object_get_object_member(reply, "release"), "changelog"),
			"Crash on save"));

		/* And publishing over the API is refused for the same reason
		 * the button is missing: no repository. */
		g_clear_pointer(&api_path, g_free);
		api_path = g_strdup_printf("/api/v1/releases/%" G_GINT64_FORMAT
		                           "/publish", release_id);
		g_assert_cmpuint(server_request_full(fixture, "POST", api_path,
			"application/json", "{}", NULL, NULL), ==, 422);
	}

	/* The loop at a glance, as JSON: the release is listed, so is the
	 * nothing that is on fire. */
	{
		g_autofree gchar *body = NULL;
		g_autoptr(JsonNode) node = NULL;
		JsonObject *status;

		g_assert_cmpuint(server_request(fixture, "GET", "/api/v1/factory",
		                                NULL, &body), ==, SOUP_STATUS_OK);
		node = venture_json_parse(body, NULL);
		status = json_node_get_object(node);
		g_assert_cmpuint(json_array_get_length(
			json_object_get_array_member(status, "releases")), ==, 1);
		g_assert_cmpuint(json_array_get_length(
			json_object_get_array_member(status, "incidents")), ==, 0);
		g_assert_true(json_object_has_member(status, "environments"));
		g_assert_true(json_object_has_member(status, "milestones"));
		g_assert_true(json_object_has_member(status, "builds"));
	}

	/* The release page lists what shipped and offers the actions; with
	 * no repository there is nothing to publish to. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/release/%" G_GINT64_FORMAT, release_id);
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, &page), ==,
	                 SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "Shipped in this release"));
	g_assert_nonnull(strstr(page, "Crash on save"));
	g_assert_nonnull(strstr(page, "Redraft changelog"));
	g_assert_null(strstr(page, "Publish to forge"));

	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/releases/%" G_GINT64_FORMAT "/publish",
	                       release_id);
	/* 422: a validation failure, the status every refused save gets. */
	g_assert_cmpuint(server_request(fixture, "POST", path, "", NULL), ==,
	                 422);
}

/*
 * The lead-time report measures from a ticket's creation to its release,
 * and the incidents report from start to resolution.
 */
static void
test_factory_reports_measure_the_loop(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) in_two_days = NULL;
	g_autoptr(GDateTime) started = NULL;
	g_autoptr(GDateTime) resolved = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) lead = NULL;
	g_autoptr(VentureReportResult) incidents = NULL;
	g_autoptr(VentureReportResult) releases = NULL;
	g_autoptr(VentureIncident) incident = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	GPtrArray *metrics;
	gboolean found;
	gint64 release_id;
	guint i;

	(void)user_data;

	now = g_date_time_new_now_utc();
	in_two_days = g_date_time_add_days(now, 2);

	release_id = create_release(fixture, "3.0.0", in_two_days);
	create_ticket(fixture, "One", VENTURE_ISSUE_TYPE_TASK, release_id);
	create_ticket(fixture, "Two", VENTURE_ISSUE_TYPE_BUG, release_id);

	started = g_date_time_add_hours(now, -6);
	resolved = g_date_time_add_hours(now, -2);

	incident = venture_incident_new();
	g_object_set(incident, "title", "Down", "severity",
	             VENTURE_INCIDENT_SEVERITY_SEV1,
	             "status", VENTURE_INCIDENT_STATUS_RESOLVED,
	             "started-at", started, "resolved-at", resolved,
	             "release-id", release_id, NULL);
		file_under_default(fixture, incident);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(incident), NULL, NULL));

	period = venture_date_range_new_all_time();

	/* Lead time: two tickets created now, released in two days. */
	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "lead_time");
	g_assert_nonnull(report);
	lead = venture_report_generate(report, fixture->context, period, NULL,
	                               &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(lead), ==, 1);

	metrics = venture_report_result_get_metrics(lead);
	found = FALSE;

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric;

		metric = g_ptr_array_index(metrics, i);

		if (0 == g_strcmp0(venture_metric_get_key(metric), "average"))
		{
			gdouble days;

			days = venture_metric_get_number(metric);
			g_assert_cmpfloat(days, >, 1.9);
			g_assert_cmpfloat(days, <, 2.1);
			found = TRUE;
		}
	}

	g_assert_true(found);

	/* Incidents: one, resolved in four hours. */
	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "incidents");
	incidents = venture_report_generate(report, fixture->context, period, NULL,
	                                    &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(incidents), ==, 1);

	metrics = venture_report_result_get_metrics(incidents);
	found = FALSE;

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric;

		metric = g_ptr_array_index(metrics, i);

		if (0 == g_strcmp0(venture_metric_get_key(metric), "mttr"))
		{
			gdouble hours;

			hours = venture_metric_get_number(metric);
			g_assert_cmpfloat(hours, >, 3.9);
			g_assert_cmpfloat(hours, <, 4.1);
			found = TRUE;
		}
	}

	g_assert_true(found);

	/* Releases: the one release, carrying two tickets. */
	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "releases");
	releases = venture_report_generate(report, fixture->context, period, NULL,
	                                   &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(releases), ==, 1);

	/* And the reports are on the reports page. */
	{
		g_autofree gchar *page = NULL;

		g_assert_cmpuint(server_request(fixture, "GET", "/reports", NULL,
		                                &page), ==, SOUP_STATUS_OK);
		g_assert_nonnull(strstr(page, "/reports/lead_time"));
	}
}

/*
 * The factory page renders, and each of the factory's pages shows its
 * block.
 */
static void
test_factory_page_renders(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEnvironment) environment = NULL;
	g_autoptr(VentureDeployment) deployment = NULL;
	g_autoptr(VentureMilestone) milestone = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	gint64 release_id;
	gint64 environment_id;

	(void)user_data;

	now = g_date_time_new_now_utc();
	release_id = create_release(fixture, "4.0.0", now);

	environment = venture_environment_new();
	g_object_set(environment, "name", "production",
	             "kind", VENTURE_ENVIRONMENT_KIND_PRODUCTION, "active", TRUE,
	             NULL);
		file_under_default(fixture, environment);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(environment), NULL,
	                                    NULL));
	environment_id = venture_entity_get_id(VENTURE_ENTITY(environment));

	deployment = venture_deployment_new();
	g_object_set(deployment, "release-id", release_id,
	             "environment-id", environment_id,
	             "status", VENTURE_DEPLOYMENT_STATUS_SUCCEEDED,
	             "deployed-at", now, NULL);
		file_under_default(fixture, deployment);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(deployment), NULL,
	                                    NULL));

	milestone = venture_milestone_new();
	g_object_set(milestone, "name", "Q4", "status",
	             VENTURE_MILESTONE_STATUS_ACTIVE, NULL);
		file_under_default(fixture, milestone);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(milestone), NULL, NULL));

	g_assert_cmpuint(server_request(fixture, "GET", "/factory", NULL, &page),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, ">Milestones<"));
	g_assert_nonnull(strstr(page, "Q4"));
	g_assert_nonnull(strstr(page, "4.0.0"));
	g_assert_nonnull(strstr(page, "production"));
	g_assert_nonnull(strstr(page, "Nothing is on fire"));
	g_assert_nonnull(strstr(page, "href=\"/factory\""));

	g_clear_pointer(&page, g_free);
	path = g_strdup_printf("/e/environment/%" G_GINT64_FORMAT, environment_id);
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, &page), ==,
	                 SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "Running now"));
	g_assert_nonnull(strstr(page, "4.0.0"));

	g_clear_pointer(&page, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/milestone/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(VENTURE_ENTITY(milestone)));
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, &page), ==,
	                 SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, ">Progress<"));
}

/*
 * With the module off, the factory is gone: its pages, its types and its
 * reports -- and the CI webhook is acknowledged and dropped, not recorded.
 */
static void
test_factory_module_off_hides_everything(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *page = NULL;
	static const gchar *const requested =
		"{\"action\":\"requested\","
		" \"workflow_run\":{\"id\":9002,\"run_number\":1,"
		"   \"display_title\":\"x\",\"head_branch\":\"main\","
		"   \"head_sha\":\"def\",\"status\":\"queued\",\"conclusion\":\"\","
		"   \"html_url\":\"\"},"
		" \"workflow\":{\"name\":\"CI\"},"
		" \"repository\":{\"full_name\":\"zach/venture\"},"
		" \"sender\":{\"login\":\"zach\"}}";

	(void)user_data;

	/* Flipped on the live configuration; the registry follows. */
	venture_config_set_module_enabled(fixture->config, "factory", FALSE);

	g_assert_false(venture_context_module_enabled(fixture->context, "factory"));
	g_assert_cmpuint(server_request(fixture, "GET", "/factory", NULL, &page),
	                 ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request(fixture, "GET", "/e/release", NULL, NULL),
	                 ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request(fixture, "GET", "/api/v1/build", NULL,
	                                NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_null(venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "lead_time"));

	g_assert_cmpuint(server_deliver_webhook(fixture, "workflow_run", "d-9",
	                                        requested), ==,
	                 SOUP_STATUS_NO_CONTENT);

	/* Back on, and everything returns. */
	venture_config_set_module_enabled(fixture->config, "factory", TRUE);
	g_assert_cmpuint(server_request(fixture, "GET", "/factory", NULL, NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "lead_time"));
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/factory/module-owns-its-types",
	                test_factory_module_owns_its_types);
	g_test_add_func("/factory/parses-a-workflow-run",
	                test_factory_parses_a_workflow_run);
	g_test_add_func("/factory/parses-a-release",
	                test_factory_parses_a_release);

#define ADD(path, func) \
	g_test_add(path, ServerFixture, NULL, server_fixture_set_up, func, \
	           server_fixture_tear_down)

	ADD("/factory/http/workflow-run-becomes-a-build",
	    test_factory_workflow_run_becomes_a_build);
	ADD("/factory/http/forge-release-becomes-a-release",
	    test_factory_forge_release_becomes_a_release);
	ADD("/factory/http/changelog-is-drafted-from-tickets",
	    test_factory_changelog_is_drafted_from_tickets);
	ADD("/factory/http/reports-measure-the-loop",
	    test_factory_reports_measure_the_loop);
	ADD("/factory/http/page-renders", test_factory_page_renders);
	ADD("/factory/http/module-off-hides-everything",
	    test_factory_module_off_hides_everything);

	return g_test_run();
}
