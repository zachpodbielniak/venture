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

/*
 * A moment some days or hours from now. venture_time_now() is transfer
 * full, so nesting it in g_date_time_add_*() leaks the instant.
 */
static GDateTime *
time_from_now(
	gint	days,
	gint	hours
){
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) shifted = NULL;

	now = venture_time_now();
	shifted = g_date_time_add_days(now, days);

	return g_date_time_add_hours(shifted, hours);
}

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

	/* ...and says so, rather than coming back looking as though the
	 * button did nothing. */
	{
		g_autofree gchar *kept_path = NULL;
		g_autofree gchar *kept_page = NULL;

		kept_path = g_strdup_printf("/e/release/%" G_GINT64_FORMAT
		                            "?changelog=kept", release_id);
		g_assert_cmpuint(server_request(fixture, "GET", kept_path, NULL,
		                                &kept_page), ==, SOUP_STATUS_OK);
		g_assert_nonnull(strstr(kept_page, "which was kept"));
	}

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
		g_assert_cmpstr(json_object_get_string_member(reply, "reason"), ==,
		                VENTURE_FACTORY_CHANGELOG_KEPT);

		g_clear_pointer(&body, g_free);
		g_clear_pointer(&node, json_node_unref);
		g_assert_cmpuint(server_request_full(fixture, "POST", api_path,
			"application/json", "{\"replace\": true}", NULL, &body), ==,
			SOUP_STATUS_OK);
		node = venture_json_parse(body, NULL);
		reply = json_node_get_object(node);
		g_assert_true(json_object_get_boolean_member(reply, "changed"));
		g_assert_true(json_object_get_null_member(reply, "reason"));
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

/* --- The webhook, when deliveries misbehave ------------------------------- */

/*
 * Forgejo sends no workflow_run. It sends action_run_success and
 * action_run_failure once a run is done, with the run under "run" -- the
 * shape of Forgejo's own ActionPayload and ActionRun structs -- and those
 * become builds as well.
 *
 * What breaks if this regresses: on Forgejo, the forge this is mostly
 * pointed at, no build ever arrives and every build figure reads zero.
 */
static void
test_factory_forgejo_action_runs_become_builds(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	static const gchar *const failure =
		"{\"action\":\"failure\","
		" \"run\":{\"id\":501,\"title\":\"fix: flaky upload\","
		"   \"repository\":{\"full_name\":\"zach/venture\"},"
		"   \"workflow_id\":\"ci.yml\",\"index_in_repo\":42,"
		"   \"trigger_user\":{\"login\":\"zach\"},"
		"   \"prettyref\":\"main\",\"commit_sha\":\"0badc0de\","
		"   \"event\":\"push\",\"status\":\"failure\","
		"   \"started\":\"2026-09-05T08:00:00Z\","
		"   \"stopped\":\"2026-09-05T08:06:30Z\","
		"   \"html_url\":\"https://git.example.com/zach/venture/actions/runs/42\"},"
		" \"prior_status\":\"running\"}";
	static const gchar *const cancelled =
		"{\"action\":\"failure\","
		" \"run\":{\"id\":502,\"title\":\"chore: bump\","
		"   \"repository\":{\"full_name\":\"zach/venture\"},"
		"   \"workflow_id\":\"ci.yml\",\"index_in_repo\":43,"
		"   \"prettyref\":\"feature/x\",\"commit_sha\":\"feedface\","
		"   \"status\":\"cancelled\","
		"   \"started\":\"0001-01-01T00:00:00Z\","
		"   \"stopped\":\"2026-09-05T09:00:00Z\","
		"   \"html_url\":\"https://git.example.com/zach/venture/actions/runs/43\"},"
		" \"prior_status\":\"waiting\"}";
	static const gchar *const success =
		"{\"action\":\"success\","
		" \"run\":{\"id\":501,\"title\":\"fix: flaky upload\","
		"   \"repository\":{\"full_name\":\"zach/venture\"},"
		"   \"workflow_id\":\"ci.yml\",\"index_in_repo\":42,"
		"   \"prettyref\":\"main\",\"commit_sha\":\"0badc0de\","
		"   \"status\":\"success\","
		"   \"started\":\"2026-09-05T10:00:00Z\","
		"   \"stopped\":\"2026-09-05T10:05:00Z\","
		"   \"html_url\":\"https://git.example.com/zach/venture/actions/runs/42\"},"
		" \"prior_status\":\"running\"}";
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) build = NULL;
	g_autoptr(VentureEntity) stopped = NULL;
	g_autoptr(GDateTime) started = NULL;
	g_autoptr(GDateTime) finished = NULL;
	g_autofree gchar *workflow = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *commit = NULL;
	VentureBuildStatus status;
	gint64 number = 0;

	(void)user_data;

	g_assert_cmpuint(server_deliver_webhook(fixture, "action_run_failure",
	                                        "f-1", failure), ==,
	                 SOUP_STATUS_ACCEPTED);

	query = venture_query_new(VENTURE_TYPE_BUILD);
	venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ,
	                                "501", NULL);
	build = venture_database_find_one(fixture->database, query, NULL);
	g_assert_nonnull(build);
	g_object_get(build, "status", &status, "workflow", &workflow, "ref", &ref,
	             "commit", &commit, "number", &number,
	             "finished-at", &finished, NULL);
	g_assert_cmpint(status, ==, VENTURE_BUILD_STATUS_FAILED);
	g_assert_cmpstr(workflow, ==, "ci.yml");
	g_assert_cmpstr(ref, ==, "main");
	g_assert_cmpstr(commit, ==, "0badc0de");
	g_assert_cmpint(number, ==, 42);
	g_assert_nonnull(finished);
	g_assert_cmpint(g_date_time_get_minute(finished), ==, 6);
	g_clear_object(&build);

	/* Cancelled before it started: cancelled, and Go's zero time is no
	 * start at all. */
	g_assert_cmpuint(server_deliver_webhook(fixture, "action_run_failure",
	                                        "f-2", cancelled), ==,
	                 SOUP_STATUS_ACCEPTED);
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_BUILD);
	venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ,
	                                "502", NULL);
	stopped = venture_database_find_one(fixture->database, query, NULL);
	g_assert_nonnull(stopped);
	g_object_get(stopped, "status", &status, "started-at", &started, NULL);
	g_assert_cmpint(status, ==, VENTURE_BUILD_STATUS_CANCELLED);

	/* The zero start is dropped rather than stored, and the lifecycle
	 * rule gives a finished build a start -- never year 1. */
	g_assert_nonnull(started);
	g_assert_cmpint(g_date_time_get_year(started), >=, 2000);

	/* Re-run and green: the same row, succeeded. */
	g_assert_cmpuint(server_deliver_webhook(fixture, "action_run_success",
	                                        "f-3", success), ==,
	                 SOUP_STATUS_ACCEPTED);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_BUILD), ==, 2);
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_BUILD);
	venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ,
	                                "501", NULL);
	build = venture_database_find_one(fixture->database, query, NULL);
	g_object_get(build, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_BUILD_STATUS_SUCCEEDED);
}

/*
 * A workflow_run payload, for the tests that send several.
 */
static gchar *
workflow_payload(
	gint64		 run_id,
	const gchar	*action,
	const gchar	*status,
	const gchar	*conclusion,
	const gchar	*started_at,
	const gchar	*completed_at
){
	return g_strdup_printf(
		"{\"action\":\"%s\","
		" \"workflow_run\":{\"id\":%" G_GINT64_FORMAT ",\"run_number\":3,"
		"   \"display_title\":\"fix: a thing\",\"head_branch\":\"main\","
		"   \"head_sha\":\"def456\",\"status\":\"%s\",\"conclusion\":\"%s\","
		"   \"html_url\":\"https://git.example.com/zach/venture/actions/runs/1\","
		"   \"started_at\":\"%s\",\"completed_at\":\"%s\"},"
		" \"workflow\":{\"name\":\"CI\"},"
		" \"repository\":{\"full_name\":\"zach/venture\"},"
		" \"sender\":{\"login\":\"zach\"}}",
		action, run_id, status, conclusion, started_at, completed_at);
}

/*
 * The one build the fixture's repository has, fresh from the database.
 */
static VentureEntity *
only_build(ServerFixture *fixture)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) builds = NULL;

	query = venture_query_new(VENTURE_TYPE_BUILD);
	builds = venture_database_find(fixture->database, query, NULL);
	g_assert_cmpuint(builds->len, ==, 1);

	return g_object_ref(g_ptr_array_index(builds, 0));
}

/*
 * Deliveries arrive late, twice and out of order, and a finished build
 * stays finished through all of it -- while a run that really did start
 * again goes back to running.
 *
 * What breaks if this regresses: a red build flips back to "running"
 * when the forge retries an old delivery, drops out of the failed counts,
 * and keeps a finished time from a run that is apparently still going.
 */
static void
test_factory_late_deliveries_do_not_unfinish_a_build(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *completed = NULL;
	g_autofree gchar *late = NULL;
	g_autofree gchar *rerun = NULL;
	g_autofree gchar *neutral = NULL;
	g_autoptr(VentureEntity) build = NULL;
	g_autoptr(GDateTime) started = NULL;
	g_autoptr(GDateTime) finished = NULL;
	VentureBuildStatus status;

	(void)user_data;

	/* Gitea's shape: completed_at, and Go's zero time for "not yet". */
	completed = workflow_payload(7001, "completed", "completed", "failure",
	                             "2026-09-01T10:00:00Z", "2026-09-01T10:04:00Z");
	late = workflow_payload(7001, "in_progress", "in_progress", "",
	                        "2026-09-01T10:00:00Z", "0001-01-01T00:00:00Z");
	rerun = workflow_payload(7001, "in_progress", "in_progress", "",
	                         "2026-09-01T11:00:00Z", "0001-01-01T00:00:00Z");
	neutral = workflow_payload(7001, "completed", "completed", "neutral",
	                           "2026-09-01T11:00:00Z", "2026-09-01T11:02:00Z");

	g_assert_cmpuint(server_deliver_webhook(fixture, "workflow_run", "w-1",
	                                        completed), ==,
	                 SOUP_STATUS_ACCEPTED);

	build = only_build(fixture);
	g_object_get(build, "status", &status, "finished-at", &finished, NULL);
	g_assert_cmpint(status, ==, VENTURE_BUILD_STATUS_FAILED);
	g_assert_nonnull(finished);
	g_assert_cmpint(g_date_time_get_minute(finished), ==, 4);
	g_clear_object(&build);
	g_clear_pointer(&finished, g_date_time_unref);

	/* The "in progress" for the same start, arriving after. Ignored. */
	g_assert_cmpuint(server_deliver_webhook(fixture, "workflow_run", "w-2",
	                                        late), ==,
	                 SOUP_STATUS_NO_CONTENT);

	build = only_build(fixture);
	g_object_get(build, "status", &status, "finished-at", &finished, NULL);
	g_assert_cmpint(status, ==, VENTURE_BUILD_STATUS_FAILED);
	g_assert_nonnull(finished);
	g_clear_object(&build);
	g_clear_pointer(&finished, g_date_time_unref);

	/* A later start is somebody pressing Re-run: running again, and no
	 * longer finished. The zero completed_at is not a finish time. */
	g_assert_cmpuint(server_deliver_webhook(fixture, "workflow_run", "w-3",
	                                        rerun), ==,
	                 SOUP_STATUS_ACCEPTED);

	build = only_build(fixture);
	g_object_get(build, "status", &status, "started-at", &started,
	             "finished-at", &finished, NULL);
	g_assert_cmpint(status, ==, VENTURE_BUILD_STATUS_RUNNING);
	g_assert_null(finished);
	g_assert_cmpint(g_date_time_get_hour(started), ==, 11);
	g_clear_object(&build);

	/* Neutral is not red. */
	g_assert_cmpuint(server_deliver_webhook(fixture, "workflow_run", "w-4",
	                                        neutral), ==,
	                 SOUP_STATUS_ACCEPTED);

	build = only_build(fixture);
	g_object_get(build, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_BUILD_STATUS_SUCCEEDED);
}

static gchar *
release_payload(
	const gchar	*action,
	gint64		 release_id,
	const gchar	*tag,
	gboolean	 prerelease,
	const gchar	*url,
	const gchar	*published_at
){
	return g_strdup_printf(
		"{\"action\":\"%s\","
		" \"release\":{\"id\":%" G_GINT64_FORMAT ",\"tag_name\":\"%s\","
		"   \"name\":\"\",\"body\":\"- from the forge\",\"draft\":false,"
		"   \"prerelease\":%s,\"html_url\":\"%s\",\"published_at\":%s%s%s},"
		" \"repository\":{\"full_name\":\"zach/venture\"},"
		" \"sender\":{\"login\":\"zach\"}}",
		action, release_id, tag, prerelease ? "true" : "false", url,
		(NULL != published_at) ? "\"" : "",
		(NULL != published_at) ? published_at : "null",
		(NULL != published_at) ? "\"" : "");
}

/*
 * A release planned here and then tagged on the forge by hand is one
 * release, not two; a release candidate is not a release; an edit to the
 * notes of one yanked here does not put it back out; and the late
 * "deleted" for a tag that was cut again does not yank its replacement.
 *
 * What breaks if this regresses: the tickets are marked against one
 * "1.2.0" and the forge's release is another, with none.
 */
static void
test_factory_forge_release_finds_the_row_it_means(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *published = NULL;
	g_autofree gchar *candidate = NULL;
	g_autofree gchar *edited = NULL;
	g_autofree gchar *recut = NULL;
	g_autofree gchar *deleted_old = NULL;
	g_autoptr(VentureRelease) planned = NULL;
	g_autoptr(VentureEntity) release = NULL;
	g_autoptr(VentureEntity) rc = NULL;
	g_autoptr(GDateTime) first_released = NULL;
	g_autoptr(GDateTime) still_released = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *tag = NULL;
	VentureReleaseStatus status;
	gint64 planned_id;
	gint64 external = 0;

	(void)user_data;

	/* Planned here: a version, the repository, tickets, and no tag. */
	planned = venture_release_new();
	g_object_set(planned, "number", "1.2.0", "repo-id", fixture->repo_id,
	             "status", VENTURE_RELEASE_STATUS_IN_PROGRESS,
	             "changelog", "Written here.", NULL);
	file_under_default(fixture, planned);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(planned), NULL, NULL));
	planned_id = venture_entity_get_id(VENTURE_ENTITY(planned));

	published = release_payload("published", 55, "v1.2.0", FALSE,
		"https://git.example.com/zach/venture/releases/tag/v1.2.0",
		"2026-09-02T09:00:00Z");
	g_assert_cmpuint(server_deliver_webhook(fixture, "release", "r-1",
	                                        published), ==,
	                 SOUP_STATUS_ACCEPTED);

	/* The same row, now out, with the forge's id and tag -- and the
	 * changelog somebody wrote here left alone. */
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_RELEASE), ==, 1);
	release = venture_database_get(fixture->database, VENTURE_TYPE_RELEASE,
	                               planned_id, NULL);
	g_object_get(release, "status", &status, "external-id", &external,
	             "tag", &tag, "released-at", &first_released, NULL);
	g_assert_cmpint(status, ==, VENTURE_RELEASE_STATUS_RELEASED);
	g_assert_cmpint(external, ==, 55);
	g_assert_cmpstr(tag, ==, "v1.2.0");
	g_assert_nonnull(first_released);

	/* Yanked here, by hand; then somebody edits the notes on the forge. */
	g_object_set(release, "status", VENTURE_RELEASE_STATUS_YANKED, NULL);
	g_assert_true(venture_database_save(fixture->database, release, NULL,
	                                    NULL));
	g_clear_object(&release);

	edited = release_payload("updated", 55, "v1.2.0", FALSE,
		"https://git.example.com/zach/venture/releases/tag/v1.2.0", NULL);
	g_assert_cmpuint(server_deliver_webhook(fixture, "release", "r-2", edited),
	                 ==, SOUP_STATUS_ACCEPTED);

	release = venture_database_get(fixture->database, VENTURE_TYPE_RELEASE,
	                               planned_id, NULL);
	g_object_get(release, "status", &status, "released-at", &still_released,
	             NULL);
	g_assert_cmpint(status, ==, VENTURE_RELEASE_STATUS_YANKED);
	g_assert_true(g_date_time_equal(first_released, still_released));
	g_clear_object(&release);

	/* The tag is deleted and cut again: a new release with a new id. It
	 * is not the yanked row, and the late "deleted" for the old id must
	 * find the old row, not the new one. */
	recut = release_payload("published", 99, "v1.2.0", FALSE,
		"https://git.example.com/zach/venture/releases/tag/v1.2.0",
		"2026-09-03T09:00:00Z");
	g_assert_cmpuint(server_deliver_webhook(fixture, "release", "r-3", recut),
	                 ==, SOUP_STATUS_ACCEPTED);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_RELEASE), ==, 2);

	deleted_old = release_payload("deleted", 55, "v1.2.0", FALSE, "", NULL);
	g_assert_cmpuint(server_deliver_webhook(fixture, "release", "r-4",
	                                        deleted_old), ==,
	                 SOUP_STATUS_ACCEPTED);

	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		guint i;

		query = venture_query_new(VENTURE_TYPE_RELEASE);
		rows = venture_database_find(fixture->database, query, NULL);

		for (i = 0; i < rows->len; i++)
		{
			gint64 forge_id = 0;

			g_clear_pointer(&url, g_free);
			g_object_get(g_ptr_array_index(rows, i), "external-id", &forge_id,
			             "status", &status, "url", &url, NULL);

			if (99 == forge_id)
				g_assert_cmpint(status, ==, VENTURE_RELEASE_STATUS_RELEASED);
			else
				g_assert_cmpint(status, ==, VENTURE_RELEASE_STATUS_YANKED);

			/* A deletion's empty html_url blanks nothing: with the page
			 * gone from the record, Publish would be offered on a yanked
			 * release. */
			g_assert_false(venture_string_is_empty(url));
		}
	}

	/* A release candidate is on the forge and is not shipped. */
	candidate = release_payload("published", 120, "v2.0.0-rc1", TRUE,
		"https://git.example.com/zach/venture/releases/tag/v2.0.0-rc1",
		"2026-09-04T09:00:00Z");
	g_assert_cmpuint(server_deliver_webhook(fixture, "release", "r-5",
	                                        candidate), ==,
	                 SOUP_STATUS_ACCEPTED);

	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GDateTime) rc_released = NULL;

		query = venture_query_new(VENTURE_TYPE_RELEASE);
		venture_query_add_filter_int(query, "external-id",
		                             VENTURE_FILTER_OP_EQ, 120, NULL);
		rc = venture_database_find_one(fixture->database, query, NULL);
		g_assert_nonnull(rc);
		g_object_get(rc, "status", &status, "released-at", &rc_released, NULL);
		g_assert_cmpint(status, ==, VENTURE_RELEASE_STATUS_IN_PROGRESS);
		g_assert_null(rc_released);
	}
}

/* --- The operations, over HTTP -------------------------------------------- */

/*
 * What needs you, readiness, deploying and rolling back, from a page and
 * from the API, and a scope that cannot be read is an error rather than
 * everything.
 */
static void
test_factory_operations_over_http(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEnvironment) environment = NULL;
	g_autoptr(VentureIncident) incident = NULL;
	g_autoptr(JsonNode) actions = NULL;
	g_autoptr(JsonNode) readiness = NULL;
	g_autoptr(VentureEntity) current = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *release_page = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *deploy_body = NULL;
	gint64 environment_id;
	gint64 first;
	gint64 second;
	gint64 running = 0;

	(void)user_data;

	environment = venture_environment_new();
	g_object_set(environment, "name", "production",
	             "kind", VENTURE_ENVIRONMENT_KIND_PRODUCTION, "active", TRUE,
	             NULL);
	file_under_default(fixture, environment);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(environment), NULL,
	                                    NULL));
	environment_id = venture_entity_get_id(VENTURE_ENTITY(environment));

	incident = venture_incident_new();
	g_object_set(incident, "title", "Checkout <b>down</b>",
	             "severity", VENTURE_INCIDENT_SEVERITY_SEV1,
	             "status", VENTURE_INCIDENT_STATUS_OPEN, NULL);
	file_under_default(fixture, incident);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(incident), NULL, NULL));

	first = create_release(fixture, "5.0.0", NULL);
	second = create_release(fixture, "5.1.0", NULL);
	create_ticket(fixture, "Unfinished", VENTURE_ISSUE_TYPE_BUG, second);

	/* What needs you: as JSON, and at the top of the Factory page, with
	 * the incident's title escaped. */
	g_assert_cmpuint(server_request(fixture, "GET", "/api/v1/factory/actions",
	                                NULL, &body), ==, SOUP_STATUS_OK);
	actions = venture_json_parse(body, NULL);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(actions));
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(json_node_get_array(actions), 0),
		"key"), ==, "incident_without_fix");
	g_clear_pointer(&body, g_free);

	g_assert_cmpuint(server_request(fixture, "GET", "/factory", NULL, &page),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "What needs you"));
	g_assert_nonnull(strstr(page, "Checkout &lt;b&gt;down&lt;/b&gt;"));
	g_assert_null(strstr(page, "Checkout <b>down</b>"));

	/* An organization that cannot be read is an error, not "all of
	 * them". */
	g_assert_cmpuint(server_request(fixture, "GET",
	                                "/api/v1/factory?organization_id=junk",
	                                NULL, NULL), ==, SOUP_STATUS_BAD_REQUEST);
	g_assert_cmpuint(server_request(fixture, "GET",
	                                "/api/v1/factory/actions?organization_id=0",
	                                NULL, NULL), ==, SOUP_STATUS_BAD_REQUEST);

	/* Readiness: an open ticket is in the way, on the API and the page. */
	path = g_strdup_printf("/api/v1/releases/%" G_GINT64_FORMAT "/readiness",
	                       second);
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, &body), ==,
	                 SOUP_STATUS_OK);
	readiness = venture_json_parse(body, NULL);
	g_assert_false(json_object_get_boolean_member(
		json_node_get_object(readiness), "ready"));
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);

	path = g_strdup_printf("/e/release/%" G_GINT64_FORMAT, second);
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, &release_page),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(release_page, "Ready to go out?"));
	g_assert_nonnull(strstr(release_page, "Record deployment"));
	g_clear_pointer(&path, g_free);

	/* Deploy the first from its page and the second over the API. */
	path = g_strdup_printf("/releases/%" G_GINT64_FORMAT "/deploy", first);
	deploy_body = g_strdup_printf("environment_id=%" G_GINT64_FORMAT,
	                              environment_id);
	g_assert_cmpuint(server_request(fixture, "POST", path, deploy_body, NULL),
	                 ==, SOUP_STATUS_FOUND);
	g_clear_pointer(&path, g_free);
	g_clear_pointer(&deploy_body, g_free);

	/* Dated a day back, so which of the two is newer does not hang on
	 * two timestamps a millisecond apart. */
	{
		g_autoptr(VentureEntity) deployment = NULL;
		g_autoptr(GDateTime) yesterday = NULL;

		deployment = venture_factory_current_deployment(fixture->database,
		                                                environment_id);
		g_assert_nonnull(deployment);
		yesterday = time_from_now(-1, 0);
		g_object_set(deployment, "deployed-at", yesterday, NULL);
		g_assert_true(venture_database_save(fixture->database, deployment,
		                                    NULL, NULL));
	}

	path = g_strdup_printf("/api/v1/releases/%" G_GINT64_FORMAT "/deploy",
	                       second);
	deploy_body = g_strdup_printf("{\"environment_id\":%" G_GINT64_FORMAT
	                              ",\"notes\":\"friday\"}", environment_id);
	g_assert_cmpuint(server_request_full(fixture, "POST", path,
	                                     "application/json", deploy_body, NULL,
	                                     NULL), ==, SOUP_STATUS_CREATED);
	g_clear_pointer(&path, g_free);

	/* No environment named is refused, not recorded against nothing. */
	path = g_strdup_printf("/api/v1/releases/%" G_GINT64_FORMAT "/deploy",
	                       second);
	g_assert_cmpuint(server_request_full(fixture, "POST", path,
	                                     "application/json", "{}", NULL, NULL),
	                 !=, SOUP_STATUS_CREATED);
	g_clear_pointer(&path, g_free);

	current = venture_factory_current_deployment(fixture->database,
	                                             environment_id);
	g_object_get(current, "release-id", &running, NULL);
	g_assert_cmpint(running, ==, second);
	g_clear_object(&current);

	/* And take it back. */
	path = g_strdup_printf("/api/v1/environments/%" G_GINT64_FORMAT
	                       "/rollback", environment_id);
	g_assert_cmpuint(server_request_full(fixture, "POST", path,
	                                     "application/json",
	                                     "{\"reason\":\"it was friday\"}", NULL,
	                                     NULL), ==, SOUP_STATUS_CREATED);

	current = venture_factory_current_deployment(fixture->database,
	                                             environment_id);
	g_object_get(current, "release-id", &running, NULL);
	g_assert_cmpint(running, ==, first);
}

/*
 * With no assistant configured the pages offer no assistant, and asking
 * the API for a draft says why rather than failing obscurely.
 */
static void
test_factory_without_an_assistant(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	gint64 release_id;

	(void)user_data;

	release_id = create_release(fixture, "6.0.0", NULL);
	create_ticket(fixture, "A change", VENTURE_ISSUE_TYPE_STORY, release_id);

	g_assert_cmpuint(server_request(fixture, "GET", "/factory", NULL, &page),
	                 ==, SOUP_STATUS_OK);
	g_assert_null(strstr(page, "Brief me"));

	path = g_strdup_printf("/api/v1/releases/%" G_GINT64_FORMAT "/notes",
	                       release_id);
	g_assert_cmpuint(server_request_full(fixture, "POST", path,
	                                     "application/json", "{}", NULL, &body),
	                 !=, SOUP_STATUS_OK);
	g_assert_nonnull(body);
}

/*
 * An address that is not a web address is never a link.
 *
 * What breaks if this regresses: an editor sets a release's URL, or the
 * forge's base URL every repository and branch link is built from, to
 * "javascript:..." and it runs for whoever clicks it -- usually an admin.
 */
static void
test_factory_script_urls_are_not_links(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) release = NULL;
	g_autoptr(VentureEntity) forge = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *release_page = NULL;
	g_autofree gchar *repo_page = NULL;
	gint64 release_id;

	(void)user_data;

	release_id = create_release(fixture, "7.0.0", NULL);
	release = venture_database_get(fixture->database, VENTURE_TYPE_RELEASE,
	                               release_id, NULL);
	g_object_set(release, "url", "JavaScript:alert(document.cookie)", NULL);
	g_assert_true(venture_database_save(fixture->database, release, NULL,
	                                    NULL));

	path = g_strdup_printf("/e/release/%" G_GINT64_FORMAT, release_id);
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, &release_page),
	                 ==, SOUP_STATUS_OK);
	g_assert_null(strstr(release_page, "href=\"JavaScript:"));
	g_assert_null(strstr(release_page, "On the forge"));
	g_clear_pointer(&path, g_free);

	forge = venture_database_get(fixture->database, VENTURE_TYPE_FORGE,
	                             fixture->forge_id, NULL);
	g_object_set(forge, "base-url", "javascript:alert(1)//", NULL);
	g_assert_true(venture_database_save(fixture->database, forge, NULL, NULL));

	path = g_strdup_printf("/e/forge_repo/%" G_GINT64_FORMAT, fixture->repo_id);
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, &repo_page),
	                 ==, SOUP_STATUS_OK);
	g_assert_null(strstr(repo_page, "href=\"javascript:"));
	g_assert_null(strstr(repo_page, "Open on the forge"));
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

	/* The operations go with it: none of them is a way round the switch. */
	g_assert_cmpuint(server_request(fixture, "GET", "/api/v1/factory/actions",
	                                NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request(fixture, "GET",
	                                "/api/v1/releases/1/readiness", NULL, NULL),
	                 ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request_full(fixture, "POST",
	                                     "/api/v1/environments/1/rollback",
	                                     "application/json", "{}", NULL, NULL),
	                 ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request(fixture, "POST", "/builds/1/ticket", "",
	                                NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request(fixture, "GET", "/factory/assist/factory/0",
	                                NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);

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

/* --- venturectl ----------------------------------------------------------- */

typedef struct
{
	gboolean	 done;
	gchar		*out;
	gchar		*err;
	GError		*error;
} CliResult;

static void
cli_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	CliResult *outcome = user_data;

	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
	                                     &outcome->out, &outcome->err,
	                                     &outcome->error);
	outcome->done = TRUE;
}

/*
 * Runs venturectl against a server that is not there, and returns what it
 * said on stderr. Nothing here needs an answer: what is tested is what
 * the command line is refused for before any request is made.
 */
static gchar *
cli_stderr(const gchar *const *arguments)
{
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GPtrArray) argv = NULL;
	g_autoptr(GError) error = NULL;
	CliResult result = { FALSE, NULL, NULL, NULL };
	gsize i;

	argv = g_ptr_array_new();
	g_ptr_array_add(argv, (gpointer)"build/debug/venturectl");
	g_ptr_array_add(argv, (gpointer)"--server");
	g_ptr_array_add(argv, (gpointer)"http://127.0.0.1:1");

	for (i = 0; NULL != arguments[i]; i++)
		g_ptr_array_add(argv, (gpointer)arguments[i]);

	g_ptr_array_add(argv, NULL);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	child = g_subprocess_launcher_spawnv(launcher,
		(const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);

	while (!result.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(result.error);
	g_assert_false(g_subprocess_get_successful(child));
	g_free(result.out);

	return result.err;
}

/*
 * The two flags the help has always promised are flags, and each belongs
 * to its own verb.
 *
 * What breaks if this regresses: `release changelog 5 --replace` dies as
 * an unknown option, as it did; or `release publish 5 --replace` quietly
 * publishes, which cannot be undone.
 */
static void
test_factory_cli_release_flags(void)
{
	static const gchar *const replace[] = {
		"release", "changelog", "5", "--replace", NULL
	};
	static const gchar *const wrong_verb[] = {
		"release", "publish", "5", "--replace", NULL
	};
	static const gchar *const typo[] = {
		"release", "publish", "5", "prerelease", NULL
	};
	static const gchar *const elsewhere[] = {
		"factory", "--prerelease", NULL
	};
	g_autofree gchar *accepted = NULL;
	g_autofree gchar *refused = NULL;
	g_autofree gchar *stray = NULL;
	g_autofree gchar *misplaced = NULL;

	/* Accepted: it gets as far as failing to reach the server. */
	accepted = cli_stderr(replace);
	g_assert_null(strstr(accepted, "Unknown option"));
	g_assert_null(strstr(accepted, "usage:"));

	refused = cli_stderr(wrong_verb);
	g_assert_nonnull(strstr(refused, "--replace does not apply"));

	/* A stray word after the id is not read as "no flag". */
	stray = cli_stderr(typo);
	g_assert_nonnull(strstr(stray, "usage:"));

	misplaced = cli_stderr(elsewhere);
	g_assert_nonnull(strstr(misplaced, "belong to the release command"));
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
	g_test_add_func("/factory/cli-release-flags",
	                test_factory_cli_release_flags);

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
	ADD("/factory/http/forgejo-action-runs-become-builds",
	    test_factory_forgejo_action_runs_become_builds);
	ADD("/factory/http/late-deliveries-do-not-unfinish-a-build",
	    test_factory_late_deliveries_do_not_unfinish_a_build);
	ADD("/factory/http/forge-release-finds-the-row-it-means",
	    test_factory_forge_release_finds_the_row_it_means);
	ADD("/factory/http/operations", test_factory_operations_over_http);
	ADD("/factory/http/without-an-assistant",
	    test_factory_without_an_assistant);
	ADD("/factory/http/script-urls-are-not-links",
	    test_factory_script_urls_are_not_links);
	ADD("/factory/http/page-renders", test_factory_page_renders);
	ADD("/factory/http/module-off-hides-everything",
	    test_factory_module_off_hides_everything);

	return g_test_run();
}
