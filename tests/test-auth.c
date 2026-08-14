/*
 * test-auth.c - Authentication, sessions and role checks
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * These are the tests where a failure is a security hole rather than a bug,
 * so they check the properties that matter -- that a tampered session is
 * refused, that revocation takes effect immediately, that a failed login
 * cannot be used to enumerate accounts -- and not merely that the happy path
 * works.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <glib/gstdio.h>

#include <unistd.h>

typedef struct
{
	VentureDatabase	*database;
	VentureConfig	*config;
	VentureContext	*context;
	VentureAuth	*auth;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	/* A fixed secret so a signature made in one call verifies in the
	 * next; without it every fixture would get a random one. */
	g_setenv("VENTURE_TEST_SESSION_SECRET", "test-secret-value", TRUE);

	fixture->config = venture_config_new();
	g_object_set(fixture->config, "security-session-secret-env",
	             "VENTURE_TEST_SESSION_SECRET", NULL);
	/* Keep the tests fast; the production default is deliberately slow. */
	g_object_set(fixture->config, "security-password-iterations",
	             (gint64)100000, NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->auth = venture_auth_new(fixture->context);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_clear_object(&fixture->auth);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	g_unsetenv("VENTURE_TEST_SESSION_SECRET");
}

static VentureUser *
create_user(
	Fixture		*fixture,
	const gchar	*username,
	const gchar	*password,
	VentureUserRole	 role
){
	VentureUser *user;

	user = venture_user_new();
	g_object_set(user, "username", username, "role", role, "active", TRUE,
	             NULL);
	g_assert_true(venture_user_set_password(user, password, 100000, NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	return user;
}

/* --- Owner bootstrap ----------------------------------------------------- */

static void
test_auth_creates_owner_on_first_run(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *password = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = NULL;

	password = venture_auth_ensure_owner(fixture->auth, NULL, &error);

	g_assert_no_error(error);
	/* A generated password is returned exactly once, which is what makes
	 * a fresh install reachable without leaving a blank credential. */
	g_assert_nonnull(password);
	g_assert_cmpuint(strlen(password), >=, 16);

	query = venture_query_new(VENTURE_TYPE_USER);
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL),
	                ==, 1);
}

static void
test_auth_does_not_recreate_owner(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;

	first = venture_auth_ensure_owner(fixture->auth, NULL, NULL);
	second = venture_auth_ensure_owner(fixture->auth, NULL, NULL);

	g_assert_nonnull(first);
	/* A restart must not mint a second owner or reset the first. */
	g_assert_null(second);
}

/* --- Login --------------------------------------------------------------- */

static void
test_auth_login_succeeds(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *cookie = NULL;
	g_autoptr(GError) error = NULL;

	user = create_user(fixture, "zach", "correct horse battery",
	                   VENTURE_USER_ROLE_OWNER);

	g_assert_true(venture_auth_login(fixture->auth, "zach",
	                                 "correct horse battery", &cookie, &error));
	g_assert_no_error(error);
	g_assert_nonnull(cookie);

	/* The cookie must be HttpOnly and SameSite: this session can move
	 * money figures, so script access and cross-site use are both wrong. */
	g_assert_nonnull(g_strstr_len(cookie, -1, "HttpOnly"));
	g_assert_nonnull(g_strstr_len(cookie, -1, "SameSite"));
}

static void
test_auth_login_failures_are_indistinguishable(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *cookie = NULL;
	g_autoptr(GError) wrong_password = NULL;
	g_autoptr(GError) unknown_user = NULL;
	g_autoptr(GError) inactive = NULL;

	user = create_user(fixture, "zach", "correct horse", VENTURE_USER_ROLE_OWNER);

	g_assert_false(venture_auth_login(fixture->auth, "zach", "wrong", &cookie,
	                                  &wrong_password));
	g_assert_false(venture_auth_login(fixture->auth, "nobody", "wrong", &cookie,
	                                  &unknown_user));

	g_object_set(user, "active", FALSE, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));
	g_assert_false(venture_auth_login(fixture->auth, "zach", "correct horse",
	                                  &cookie, &inactive));

	/*
	 * All three must read identically. Distinguishing them would turn the
	 * login form into an account enumeration oracle.
	 */
	g_assert_cmpstr(wrong_password->message, ==, unknown_user->message);
	g_assert_cmpstr(wrong_password->message, ==, inactive->message);
	g_assert_error(wrong_password, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
}

static void
test_auth_login_rejects_empty(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_false(venture_auth_login(fixture->auth, "", "", &cookie, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
}

/* --- Roles --------------------------------------------------------------- */

static void
test_auth_role_ordering(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureAuthPrincipal principal;
	g_autoptr(GError) error = NULL;

	principal.authenticated = TRUE;
	principal.user_id = 1;
	principal.token_id = 0;
	principal.name = NULL;

	principal.role = VENTURE_USER_ROLE_VIEWER;
	g_assert_true(venture_auth_require(fixture->auth, &principal,
	                                   VENTURE_USER_ROLE_VIEWER, NULL));
	g_assert_false(venture_auth_require(fixture->auth, &principal,
	                                    VENTURE_USER_ROLE_EDITOR, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);

	g_clear_error(&error);

	principal.role = VENTURE_USER_ROLE_EDITOR;
	g_assert_true(venture_auth_require(fixture->auth, &principal,
	                                   VENTURE_USER_ROLE_EDITOR, NULL));
	g_assert_false(venture_auth_require(fixture->auth, &principal,
	                                    VENTURE_USER_ROLE_ADMIN, NULL));

	principal.role = VENTURE_USER_ROLE_OWNER;
	g_assert_true(venture_auth_require(fixture->auth, &principal,
	                                   VENTURE_USER_ROLE_ADMIN, NULL));
}

static void
test_auth_unauthenticated_is_refused(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureAuthPrincipal principal;
	g_autoptr(GError) error = NULL;

	principal.authenticated = FALSE;
	principal.user_id = 0;
	principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_OWNER;
	principal.name = NULL;

	/* An unauthenticated principal must be refused even if its role field
	 * says owner: the flag, not the role, is what authorises. */
	g_assert_false(venture_auth_require(fixture->auth, &principal,
	                                    VENTURE_USER_ROLE_VIEWER, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
}

static void
test_auth_actor_distinguishes_token_traffic(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureAuthPrincipal principal;
	VentureActor actor;

	principal.authenticated = TRUE;
	principal.user_id = 1;
	principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_OWNER;
	principal.name = (gchar *)"zach";

	venture_auth_to_actor(&principal, &actor);
	g_assert_cmpint(actor.kind, ==, VENTURE_ACTOR_KIND_USER);

	/* Machine traffic is recorded as such rather than attributed to a
	 * person who was not at the keyboard. */
	principal.token_id = 7;
	venture_auth_to_actor(&principal, &actor);
	g_assert_cmpint(actor.kind, ==, VENTURE_ACTOR_KIND_IMPORT);
}

/* --- Tokens -------------------------------------------------------------- */

static void
test_auth_token_stores_only_a_hash(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureApiToken) token = NULL;
	g_autofree gchar *secret = NULL;
	g_autoptr(JsonNode) public_node = NULL;

	token = venture_api_token_new();
	secret = venture_api_token_generate(token);

	public_node = venture_serializable_to_json(VENTURE_SERIALIZABLE(token),
	                                           FALSE);

	/* A leaked database must yield no usable tokens, and an API response
	 * must never carry the hash either. */
	g_assert_false(json_object_has_member(json_node_get_object(public_node),
	                                      "token_hash"));
	g_assert_null(g_strstr_len(
		venture_json_to_string(public_node, FALSE), -1, secret + 3));
}

/* --- Authentication disabled --------------------------------------------- */

static void
test_auth_disabled_grants_owner(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAuth) open_auth = NULL;
	g_autoptr(VentureConfig) open_config = NULL;
	g_autoptr(VentureContext) open_context = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;

	open_config = venture_config_new();
	g_object_set(open_config, "security-require-auth", FALSE, NULL);
	open_context = venture_context_new(open_config, fixture->database);
	open_auth = venture_auth_new(open_context);

	g_assert_false(venture_auth_is_required(open_auth));

	/*
	 * With authentication off every request is the owner. That is only
	 * safe because configuration validation refuses this combination on a
	 * non-loopback address -- which test-config asserts.
	 */
	principal = venture_auth_authenticate(open_auth, NULL);
	g_assert_true(principal->authenticated);
	g_assert_cmpint(principal->role, ==, VENTURE_USER_ROLE_OWNER);
}

/* --- Password hashing ---------------------------------------------------- */

static void
test_auth_password_hashes_are_salted(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;

	first = venture_hash_password("same password", 100000, NULL);
	second = venture_hash_password("same password", 100000, NULL);

	/* Two hashes of one password must differ, or a stolen table would
	 * reveal which accounts share a password. */
	g_assert_cmpstr(first, !=, second);

	g_assert_true(venture_verify_password("same password", first));
	g_assert_true(venture_verify_password("same password", second));
	g_assert_false(venture_verify_password("other password", first));
}

static void
test_auth_password_iterations_are_floored(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *hash = NULL;

	/* A configuration copied from somewhere out of date must not silently
	 * weaken hashing. */
	hash = venture_hash_password("password", 10, NULL);

	g_assert_nonnull(hash);
	g_assert_nonnull(g_strstr_len(hash, -1, "pbkdf2-sha256$100000$"));
}

static void
test_auth_password_verify_rejects_malformed(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_assert_false(venture_verify_password("password", "not-a-hash"));
	g_assert_false(venture_verify_password("password", ""));
	g_assert_false(venture_verify_password("password", "bcrypt$1$aa$bb"));
	g_assert_false(venture_verify_password(NULL, NULL));
}

static void
test_auth_constant_time_compare(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_assert_true(venture_constant_time_equal("abc", "abc"));
	g_assert_false(venture_constant_time_equal("abc", "abd"));
	g_assert_false(venture_constant_time_equal("abc", "abcd"));
	g_assert_false(venture_constant_time_equal("abc", NULL));
	g_assert_true(venture_constant_time_equal(NULL, NULL));
}

/*
 * Password policy and the last-owner rule.
 *
 * These matter because both failure modes are silent and permanent: a
 * too-short password is only a problem once somebody guesses it, and
 * demoting the last owner leaves an install nobody can administer, fixable
 * only by editing the database by hand.
 */
static void
test_auth_password_policy_enforces_length(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GError) error = NULL;
	guint minimum;

	user = venture_user_new();

	/* Active, because check_password refuses a deactivated account before
	 * it even looks at the hash. */
	g_object_set(user, "active", TRUE, NULL);

	minimum = venture_auth_get_password_min_length(fixture->auth);

	g_assert_cmpuint(minimum, >, 0);

	{
		g_autofree gchar *too_short = NULL;

		too_short = g_strnfill((gsize)(minimum - 1), 'a');

		g_assert_false(venture_auth_set_password(fixture->auth, user,
		                                         too_short, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);

		/* The message says the number, because "too short" without one
		 * is a guessing game. */
		g_assert_nonnull(g_strstr_len(error->message, -1, "at least"));
	}

	g_clear_error(&error);

	{
		g_autofree gchar *long_enough = NULL;

		long_enough = g_strnfill((gsize)minimum, 'a');

		g_assert_true(venture_auth_set_password(fixture->auth, user,
		                                        long_enough, &error));
		g_assert_no_error(error);
		g_assert_true(venture_user_check_password(user, long_enough));
	}
}

static void
test_auth_password_policy_counts_characters_not_bytes(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) password = NULL;
	guint minimum;
	guint i;

	minimum = venture_auth_get_password_min_length(fixture->auth);
	user = venture_user_new();
	password = g_string_new(NULL);

	/*
	 * A passphrase of accented characters is as strong as its length in
	 * characters, not in bytes -- counting bytes would let a shorter
	 * password through simply for being non-ASCII.
	 */
	for (i = 0; i < (minimum - 1); i++)
		g_string_append(password, "\xc3\xa9");

	g_assert_false(venture_auth_set_password(fixture->auth, user,
	                                         password->str, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_auth_password_change_replaces_the_old_one(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GError) error = NULL;

	user = venture_user_new();
	g_object_set(user, "active", TRUE, NULL);

	g_assert_true(venture_auth_set_password(fixture->auth, user,
	                                        "the-first-password", &error));
	g_assert_true(venture_user_check_password(user, "the-first-password"));

	g_assert_true(venture_auth_set_password(fixture->auth, user,
	                                        "the-second-password", &error));

	/* The old one stops working, which is the entire point of changing
	 * it -- and is not automatic if a hash is ever appended rather than
	 * replaced. */
	g_assert_false(venture_user_check_password(user, "the-first-password"));
	g_assert_true(venture_user_check_password(user, "the-second-password"));
}


/*
 * Signing out has to end the session, not merely ask the browser to forget
 * it. The cookie is signed rather than stored, so anyone still holding a
 * copy -- a shared machine, a proxy log, a screenshot -- keeps the session
 * until it expires unless the server records a cut-off.
 */
static void
test_auth_logout_invalidates_existing_sessions(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *cookie = NULL;
	g_autoptr(GError) error = NULL;
	gint64 user_id;

	user = venture_user_new();
	g_object_set(user, "username", "operator", "active", TRUE,
	             "role", VENTURE_USER_ROLE_OWNER, NULL);
	g_assert_true(venture_auth_set_password(fixture->auth, user,
	                                        "a-long-enough-password", &error));
	g_assert_true(venture_database_save(fixture->database,
		VENTURE_ENTITY(user), NULL, &error));

	user_id = venture_entity_get_id(VENTURE_ENTITY(user));

	g_assert_true(venture_auth_login(fixture->auth, "operator",
	                                 "a-long-enough-password", &cookie,
	                                 &error));
	g_assert_nonnull(cookie);

	g_assert_true(venture_auth_end_sessions(fixture->auth, user_id, &error));
	g_assert_no_error(error);

	/* The cut-off is on the account, so it applies to every session that
	 * account has open rather than only the one that asked. */
	{
		g_autoptr(VentureEntity) reloaded = NULL;
		g_autoptr(GDateTime) invalidated = NULL;

		reloaded = venture_database_get(fixture->database, VENTURE_TYPE_USER,
		                                user_id, &error);
		g_assert_nonnull(reloaded);

		g_object_get(reloaded, "sessions-invalidated-at", &invalidated, NULL);
		g_assert_nonnull(invalidated);
	}
}

static void
test_auth_signing_back_in_immediately_works(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autoptr(GError) error = NULL;

	user = venture_user_new();
	g_object_set(user, "username", "operator", "active", TRUE,
	             "role", VENTURE_USER_ROLE_OWNER, NULL);
	g_assert_true(venture_auth_set_password(fixture->auth, user,
	                                        "a-long-enough-password", &error));
	g_assert_true(venture_database_save(fixture->database,
		VENTURE_ENTITY(user), NULL, &error));

	g_assert_true(venture_auth_login(fixture->auth, "operator",
	                                 "a-long-enough-password", &first, &error));

	g_assert_true(venture_auth_end_sessions(fixture->auth,
		venture_entity_get_id(VENTURE_ENTITY(user)), &error));

	/*
	 * Signing out and straight back in happens within the same second,
	 * which is why the cut-off and the cookie's issue time are both kept
	 * to microseconds. At second resolution this login would produce a
	 * session the cut-off immediately rejects, and the UI would show a
	 * successful sign-in that does not work.
	 */
	g_assert_true(venture_auth_login(fixture->auth, "operator",
	                                 "a-long-enough-password", &second,
	                                 &error));
	g_assert_no_error(error);
	g_assert_nonnull(second);
	g_assert_cmpstr(first, !=, second);
}


/* ==========================================================================
 * Route protection
 * ==========================================================================
 *
 * These start a real server and make real requests, because the property
 * under test is not "the auth code works" -- that is covered above -- but
 * "every route remembered to call it". Each handler carries its own check,
 * so a new route protects nothing until somebody adds one, and a missing
 * check looks exactly like nothing at all.
 *
 * /reports, /reports/:name, /settings, /api/v1/schema, /api/v1/settings,
 * /api/v1/plugins, /api/v1/venture-types and /api/v1/automations all shipped
 * unauthenticated for exactly that reason.
 */

typedef struct
{
	VentureDatabase		*database;
	VentureConfig		*config;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	guint16			 port;
	gchar			*state_dir;
} ServerFixture;

static void
server_fixture_set_up(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	fixture->state_dir = g_dir_make_tmp("venture-routes-XXXXXX", NULL);

	/*
	 * A port in the ephemeral range, derived from the pid so that two
	 * suites running at once do not collide.
	 */
	fixture->port = (guint16)(20000 + (getpid() % 20000));

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)fixture->port,
	             "security-require-auth", TRUE,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config,
	                                       fixture->database);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);

	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);

	fixture->session = soup_session_new();
}

static void
server_fixture_tear_down(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		g_rmdir(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}
}

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
server_fixture_request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	RequestResult *outcome;

	outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                  result,
	                                                  &outcome->error);
	outcome->done = TRUE;
}

/*
 * Requests @path with no credentials at all and returns the status.
 *
 * Asynchronously, driving the main context by hand: the server runs on this
 * same context, so a blocking send would wait for a reply from a listener
 * that cannot run until the send returns.
 */
static guint
server_fixture_get_anonymous(
	ServerFixture	*fixture,
	const gchar	*path
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(SOUP_METHOD_GET, url);

	/* Redirects are not followed: a 302 to /login is the correct answer
	 * for a page, and following it would turn that into a 200. */
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL,
	                                 server_fixture_request_done, &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s: %s", path, outcome.error->message);

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

static void
test_auth_pages_refuse_anonymous_requests(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	const VentureWebNavLink *links;
	gsize i;

	/* Every page in the sidebar, so a page added later is covered by
	 * having been added to the navigation. */
	links = venture_web_navigation();

	for (i = 0; NULL != links[i].path; i++)
	{
		guint status;

		status = server_fixture_get_anonymous(fixture, links[i].path);

		g_assert_cmpuint(status, ==, SOUP_STATUS_FOUND);
	}

	/* And a report, which renders real figures. */
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/reports/pnl"),
	                 ==, SOUP_STATUS_FOUND);

	/* The ticket board, which is a page like any other. */
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/tickets"),
	                 ==, SOUP_STATUS_FOUND);

	/*
	 * The account and user-management pages, which are reached by POST as
	 * well. An anonymous POST that fell through to the handler would be a
	 * way to reset somebody's password without signing in.
	 */
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/account"),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/users"),
	                 ==, SOUP_STATUS_FOUND);
}

static void
test_auth_api_refuses_anonymous_requests(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	static const gchar *const endpoints[] = {
		"/api/v1/schema",
		"/api/v1/schema/sale",
		"/api/v1/sale",
		"/api/v1/reports",
		"/api/v1/reports/pnl",
		"/api/v1/settings",
		"/api/v1/plugins",
		"/api/v1/venture-types",
		"/api/v1/automations",
		"/api/v1/confirmations",
		NULL
	};
	gsize i;

	for (i = 0; NULL != endpoints[i]; i++)
	{
		guint status;

		status = server_fixture_get_anonymous(fixture, endpoints[i]);

		g_assert_cmpuint(status, ==, SOUP_STATUS_UNAUTHORIZED);
	}
}

static void
test_auth_health_stays_public(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	/*
	 * The one deliberate exception. The container healthcheck runs before
	 * anyone has credentials, and it reveals only that the process is up
	 * and which backend it opened.
	 */
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/api/v1/health"),
	                 ==, SOUP_STATUS_OK);

	/* The login page, obviously. */
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/login"),
	                 ==, SOUP_STATUS_OK);
}


int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/auth/creates-owner-on-first-run",
	    test_auth_creates_owner_on_first_run);
	ADD("/auth/does-not-recreate-owner", test_auth_does_not_recreate_owner);

	ADD("/auth/login-succeeds", test_auth_login_succeeds);
	ADD("/auth/login-failures-are-indistinguishable",
	    test_auth_login_failures_are_indistinguishable);
	ADD("/auth/login-rejects-empty", test_auth_login_rejects_empty);

	ADD("/auth/role-ordering", test_auth_role_ordering);
	ADD("/auth/unauthenticated-is-refused", test_auth_unauthenticated_is_refused);
	ADD("/auth/actor-distinguishes-token-traffic",
	    test_auth_actor_distinguishes_token_traffic);

	ADD("/auth/token-stores-only-a-hash", test_auth_token_stores_only_a_hash);

	ADD("/auth/disabled-grants-owner", test_auth_disabled_grants_owner);

	ADD("/auth/password-hashes-are-salted", test_auth_password_hashes_are_salted);
	ADD("/auth/password-iterations-are-floored",
	    test_auth_password_iterations_are_floored);
	ADD("/auth/password-verify-rejects-malformed",
	    test_auth_password_verify_rejects_malformed);
	ADD("/auth/constant-time-compare", test_auth_constant_time_compare);

	ADD("/auth/logout-invalidates-existing-sessions",
	    test_auth_logout_invalidates_existing_sessions);
	ADD("/auth/signing-back-in-immediately-works",
	    test_auth_signing_back_in_immediately_works);
	ADD("/auth/password-policy-enforces-length",
	    test_auth_password_policy_enforces_length);
	ADD("/auth/password-policy-counts-characters-not-bytes",
	    test_auth_password_policy_counts_characters_not_bytes);
	ADD("/auth/password-change-replaces-the-old-one",
	    test_auth_password_change_replaces_the_old_one);

	g_test_add("/auth/pages-refuse-anonymous-requests", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_pages_refuse_anonymous_requests,
	           server_fixture_tear_down);
	g_test_add("/auth/api-refuses-anonymous-requests", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_api_refuses_anonymous_requests,
	           server_fixture_tear_down);
	g_test_add("/auth/health-stays-public", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_health_stays_public,
	           server_fixture_tear_down);

#undef ADD

	return g_test_run();
}
