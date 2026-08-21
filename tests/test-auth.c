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

/*
 * Requests @path with an optional Cookie header and an optional form POST
 * body, returning the status and optionally the body text.
 */
static guint
server_fixture_request(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *cookie,
	const gchar	 *form_body,
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
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie",
			cookie);

	if (NULL != form_body)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(form_body, strlen(form_body));
		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
	}

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL,
	                                 server_fixture_request_done, &outcome);

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

/*
 * Like server_fixture_request(), but the body is raw bytes under an
 * explicit Content-Type -- which is what a multipart upload is.
 */
static guint
server_fixture_post_raw(
	ServerFixture	 *fixture,
	const gchar	 *path,
	const gchar	 *cookie,
	const gchar	 *content_type,
	GBytes		 *body,
	gchar		**out_body
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new("POST", url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != cookie)
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie",
			cookie);

	soup_message_set_request_body_from_bytes(message, content_type, body);

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL,
	                                 server_fixture_request_done, &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("POST %s: %s", path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

/*
 * Creates an active account directly in the fixture's database, so a test
 * can sign in over HTTP as somebody specific.
 */
static void
server_fixture_create_user(
	ServerFixture	*fixture,
	const gchar	*username,
	const gchar	*password,
	VentureUserRole	 role,
	gint64		*out_id
){
	g_autoptr(VentureUser) user = NULL;

	user = venture_user_new();
	g_object_set(user, "username", username, "role", role, "active", TRUE,
	             NULL);
	g_assert_true(venture_user_set_password(user, password, 100000, NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	if (NULL != out_id)
		*out_id = venture_entity_get_id(VENTURE_ENTITY(user));
}

/*
 * Signs in over HTTP and returns the session cookie, name=value only.
 */
static gchar *
server_fixture_login(
	ServerFixture	*fixture,
	const gchar	*username,
	const gchar	*password
){
	g_autofree gchar *body = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;
	guint status;

	body = g_strdup_printf("username=%s&password=%s", username, password);
	status = server_fixture_request(fixture, "POST", "/login", NULL, body,
	                                NULL, &set_cookie);

	g_assert_cmpuint(status, ==, SOUP_STATUS_FOUND);
	g_assert_nonnull(set_cookie);

	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	return g_steal_pointer(&set_cookie);
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

	/* Global search reaches every record type at once, which makes it the
	 * single worst page to leave unauthenticated. */
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/search"),
	                 ==, SOUP_STATUS_FOUND);

	/*
	 * An unknown path redirects rather than 404s while anonymous: whether
	 * a page exists is not information for people without a session, and
	 * a 404/302 split would let one map the route table from outside.
	 */
	g_assert_cmpuint(server_fixture_get_anonymous(fixture,
	                                              "/no-such-page-anywhere"),
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
		/* The forge surfaces. A forge row names the host this
		 * install's token is sent to; a rule decides what runs
		 * unattended; a run says what it did and what it cost. */
		"/api/v1/forge",
		"/api/v1/forge_repo",
		"/api/v1/forge_rule",
		"/api/v1/forge_run",
		"/api/v1/ticket_link",
		"/api/v1/ticket_relation",
		"/e/forge/export",
		/* The CSV export carries the same rows as the table did. */
		"/e/sale/export",
		/* The chat fragments: transcripts of what the operator asked
		 * about their own finances. */
		"/ui/chat/threads",
		"/ui/chat/thread/1",
		NULL
	};
	gsize i;

	for (i = 0; NULL != endpoints[i]; i++)
	{
		guint status;

		status = server_fixture_get_anonymous(fixture, endpoints[i]);

		g_assert_cmpuint(status, ==, SOUP_STATUS_UNAUTHORIZED);
	}

	/* The chat POSTs, which write records. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/ui/chat",
	                                        NULL, "message=hi", NULL, NULL),
	                 ==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
	                                        "/ui/chat/upload", NULL, "x",
	                                        NULL, NULL),
	                 ==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
	                                        "/ui/chat/thread/1/delete",
	                                        NULL, "", NULL, NULL),
	                 ==, SOUP_STATUS_UNAUTHORIZED);

	/* The comment composer redirects to login like the page it sits on. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
	                                        "/tickets/1/comment", NULL,
	                                        "body=hi", NULL, NULL),
	                 ==, SOUP_STATUS_FOUND);

	/* The round-four surfaces: automations, plugin config, invoices,
	 * import. Pages redirect; fragments and files 401. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/automations/save", NULL, "source=", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/automations/validate", NULL, "source=", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/automations/reload", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/plugins/config", NULL, "plugin=x", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/invoices/1/status", NULL, "to=sent", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture,
		"/invoices/1/print"), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture,
		"/e/sale/import"), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture,
		"/e/sale/import/template"), ==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/e/sale/import", NULL, "x", NULL, NULL),
		==, SOUP_STATUS_FOUND);

	/*
	 * Deciding a staged AI write applies it to the books. Anonymous must
	 * not reach either verdict -- an unauthenticated approve is a way to
	 * commit a change nobody with an account ever saw.
	 */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/ui/chat/confirm/abc123/approve", NULL, "", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);

	/*
	 * The forge actions. Pages redirect to login like every other page
	 * action; the credential routes are among them because they are
	 * reached from the forge page.
	 */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/forges/1/token", NULL, "token=x", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/forges/1/secret", NULL, "secret=x", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/forges/1/verify", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/tickets/1/link", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/tickets/1/branch", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/tickets/1/work", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/tickets/1/relate", NULL, "subject_type=invoice&subject_id=1",
		NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/relations/1/delete", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/runs/1/cancel", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);

	/* The run card is a fragment, so it 401s rather than redirecting --
	 * the same rule the chat fragments follow. */
	g_assert_cmpuint(server_fixture_get_anonymous(fixture,
		"/tickets/1/runs"), ==, SOUP_STATUS_UNAUTHORIZED);

	/*
	 * And the one route that is deliberately not session-authenticated.
	 *
	 * It must refuse -- 401, because the caller is a forge that should
	 * retry with a signature rather than a browser to be sent to a login
	 * page. It is asserted here rather than left out precisely because
	 * this sweep is an allowlist: a new unauthenticated route that nobody
	 * adds is a route nobody is checking.
	 */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/hooks/forge/1", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);

	/*
	 * The scriptable credential routes. These exist so a deployment can
	 * be automated, which means they are reachable with nothing but a
	 * token -- so they are the ones worth checking hardest.
	 */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/forge/1/token", NULL, "{\"token\":\"x\"}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/forge/1/webhook-secret", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/forge/1/verify", NULL, "", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/ui/chat/confirm/abc123/reject", NULL, "", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
}

/*
 * An uploaded screenshot must be recognised as an image: stored with no
 * extracted text, flagged so the client shows a thumbnail instead of a "no
 * text" warning, and kept as a document the model can be handed later.
 *
 * The flag is the load-bearing part. Treated as an ordinary attachment, a
 * screenshot reaches the model as "[No text could be extracted]" -- so it
 * answers about a picture it was never shown, which reads as the model
 * hallucinating rather than as a plumbing bug.
 */
static void
test_auth_screenshot_upload_is_an_image(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	/* A one-pixel PNG, complete with the NUL bytes that make it a real
	 * binary payload rather than text wearing a .png suffix. */
	static const guchar png[] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00,
		0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
		0x15, 0xc4, 0x89
	};
	static const gchar head[] =
		"--IMGBND\r\n"
		"Content-Disposition: form-data; name=\"file\"; "
		"filename=\"campaign.png\"\r\n"
		"Content-Type: image/png\r\n"
		"\r\n";
	static const gchar tail[] = "\r\n--IMGBND--\r\n";
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *response = NULL;
	g_autofree gchar *document = NULL;
	g_autoptr(GBytes) body = NULL;
	GByteArray *raw;

	server_fixture_create_user(fixture, "grace", "g-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	cookie = server_fixture_login(fixture, "grace", "g-long-password");

	raw = g_byte_array_new();
	g_byte_array_append(raw, (const guchar *)head, strlen(head));
	g_byte_array_append(raw, png, sizeof(png));
	g_byte_array_append(raw, (const guchar *)tail, strlen(tail));
	body = g_byte_array_free_to_bytes(raw);

	g_assert_cmpuint(server_fixture_post_raw(fixture, "/ui/chat/upload",
		cookie, "multipart/form-data; boundary=IMGBND", body,
		&response),
		==, SOUP_STATUS_CREATED);

	/* Flagged an image, and no text was invented for it. */
	g_assert_nonnull(strstr(response, "\"is_image\" : true"));
	g_assert_nonnull(strstr(response, "\"text_chars\" : 0"));

	/* Stored as a document, distinguishable from a text attachment. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/api/v1/document/1", cookie, NULL, &document, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(document, "\"kind\" : \"screenshot\""));
	g_assert_nonnull(strstr(document, "campaign.png"));

	/* The bytes survived the multipart parse intact. */
	g_assert_nonnull(strstr(document, "\"size_bytes\" : 33"));
}

/*
 * The invoice lifecycle over HTTP, including the transition that matters:
 * marking paid must create the sale, with the invoice total as gross. If
 * this regresses, invoicing and the books quietly diverge -- the exact
 * disagreement the wiring exists to make impossible.
 */
static void
test_auth_invoice_paid_creates_the_sale(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *body = NULL;
	guint status;

	server_fixture_create_user(fixture, "erin", "e-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	cookie = server_fixture_login(fixture, "erin", "e-long-password");

	/* An invoice with two lines: 2 x $100.00 and 1 x $49.99. */
	{
		g_autoptr(VentureInvoice) invoice = NULL;
		g_autoptr(VentureInvoiceLine) first = NULL;
		g_autoptr(VentureInvoiceLine) second = NULL;
		g_autoptr(VentureMoney) hundred = NULL;
		g_autoptr(VentureMoney) fifty = NULL;

		invoice = venture_invoice_new();
		g_object_set(invoice, "number", "INV-1",
		             "status", VENTURE_INVOICE_STATUS_DRAFT, NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(invoice), NULL, NULL));

		hundred = venture_money_new(10000, "USD", 2);
		first = venture_invoice_line_new();
		g_object_set(first, "invoice-id",
		             venture_entity_get_id(VENTURE_ENTITY(invoice)),
		             "description", "consulting", "quantity", 2.0,
		             "unit-price", hundred, NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(first), NULL, NULL));

		fifty = venture_money_new(4999, "USD", 2);
		second = venture_invoice_line_new();
		g_object_set(second, "invoice-id",
		             venture_entity_get_id(VENTURE_ENTITY(invoice)),
		             "description", "rush fee", "quantity", 1.0,
		             "unit-price", fifty, NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(second), NULL, NULL));
	}

	/* Draft cannot jump straight to paid. */
	status = server_fixture_request(fixture, "POST", "/invoices/1/status",
	                                cookie, "to=paid", NULL, NULL);
	g_assert_cmpuint(status, ==, 422);

	/* Draft -> sent -> paid. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/invoices/1/status", cookie, "to=sent", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/invoices/1/status", cookie, "to=paid", NULL, NULL),
		==, SOUP_STATUS_FOUND);

	/* The revenue exists now: one sale, $249.99 gross, named after the
	 * invoice. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) sales = NULL;
		g_autoptr(VentureMoney) gross = NULL;
		g_autofree gchar *external = NULL;

		query = venture_query_new(VENTURE_TYPE_SALE);
		sales = venture_database_find(fixture->database, query, NULL);

		g_assert_nonnull(sales);
		g_assert_cmpuint(sales->len, ==, 1);

		g_object_get(g_ptr_array_index(sales, 0),
		             "gross", &gross, "external-id", &external, NULL);
		g_assert_nonnull(gross);
		g_assert_cmpint(venture_money_get_amount(gross), ==, 24999);
		g_assert_cmpstr(external, ==, "INV-1");
	}

	/* Paid is terminal. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/invoices/1/status", cookie, "to=void", NULL, NULL),
		==, 422);

	/* And the printable view renders the number and the total. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/invoices/1/print", cookie, NULL, &body, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "INV-1"));
	g_assert_nonnull(strstr(body, "249.99"));
}

/*
 * CSV import over HTTP: all-or-nothing is the property under test. A file
 * with one bad row must import nothing -- 39 records that arrived beside a
 * validation failure are 39 records nobody meant to trust.
 */
static void
test_auth_csv_import_is_all_or_nothing(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	static const gchar good_csv[] =
		"--CSVBND\r\n"
		"Content-Disposition: form-data; name=\"file\"; "
		"filename=\"companies.csv\"\r\n"
		"Content-Type: text/csv\r\n"
		"\r\n"
		"name,kind,industry\r\n"
		"\"Acme, Inc\",customer,publishing\r\n"
		"Beta LLC,supplier,print\r\n"
		"\r\n--CSVBND--\r\n";
	static const gchar bad_csv[] =
		"--CSVBND\r\n"
		"Content-Disposition: form-data; name=\"file\"; "
		"filename=\"companies.csv\"\r\n"
		"Content-Type: text/csv\r\n"
		"\r\n"
		"name,kind\r\n"
		"Gamma Co,customer\r\n"
		"Delta Co,not-a-kind\r\n"
		"\r\n--CSVBND--\r\n";
	g_autofree gchar *cookie = NULL;
	g_autoptr(GBytes) good = NULL;
	g_autoptr(GBytes) bad = NULL;
	g_autoptr(VentureQuery) query = NULL;

	server_fixture_create_user(fixture, "frank", "f-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	cookie = server_fixture_login(fixture, "frank", "f-long-password");

	good = g_bytes_new_static(good_csv, strlen(good_csv));
	bad = g_bytes_new_static(bad_csv, strlen(bad_csv));

	/* The good file: both rows land, quoted comma intact. */
	{
		g_autofree gchar *body = NULL;

		g_assert_cmpuint(server_fixture_post_raw(fixture,
			"/e/company/import", cookie,
			"multipart/form-data; boundary=CSVBND", good, &body),
			==, SOUP_STATUS_OK);
		g_assert_nonnull(strstr(body, "Imported 2 records"));
	}

	query = venture_query_new(VENTURE_TYPE_COMPANY);
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL),
	                ==, 2);

	/* The bad file: one row has an enum typo, so NEITHER row lands. */
	{
		g_autofree gchar *body = NULL;

		g_assert_cmpuint(server_fixture_post_raw(fixture,
			"/e/company/import", cookie,
			"multipart/form-data; boundary=CSVBND", bad, &body),
			==, 422);
		g_assert_nonnull(strstr(body, "Nothing was imported"));
	}

	g_assert_cmpint(venture_database_count(fixture->database, query, NULL),
	                ==, 2);
}

/*
 * A conversation belongs to the account that had it. Without this property,
 * any signed-in editor could read what the owner has been asking the AI
 * about the books -- and the mismatch case must be indistinguishable from a
 * thread that does not exist, or sequential thread ids would confirm which
 * conversations are real.
 */
static void
test_auth_chat_threads_are_scoped_per_user(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *alice_cookie = NULL;
	g_autofree gchar *bob_cookie = NULL;
	g_autofree gchar *alice_body = NULL;
	g_autofree gchar *bob_list = NULL;
	g_autofree gchar *path = NULL;
	gint64 alice_id;
	gint64 bob_id;
	gint64 thread_id;

	server_fixture_create_user(fixture, "alice", "a-long-password",
	                           VENTURE_USER_ROLE_EDITOR, &alice_id);
	server_fixture_create_user(fixture, "bob", "b-long-password",
	                           VENTURE_USER_ROLE_EDITOR, &bob_id);

	/* Alice's conversation, planted directly in the database. */
	{
		g_autoptr(VentureChatThread) thread = NULL;
		g_autoptr(VentureChatMessage) message = NULL;

		thread = venture_chat_thread_new();
		g_object_set(thread, "title", "alices private numbers",
		             "user-id", alice_id, NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(thread), NULL, NULL));
		thread_id = venture_entity_get_id(VENTURE_ENTITY(thread));

		message = venture_chat_message_new();
		g_object_set(message, "thread-id", thread_id,
		             "role", VENTURE_CHAT_ROLE_USER,
		             "body", "how much did I make", NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(message), NULL, NULL));
	}

	alice_cookie = server_fixture_login(fixture, "alice", "a-long-password");
	bob_cookie = server_fixture_login(fixture, "bob", "b-long-password");

	path = g_strdup_printf("/ui/chat/thread/%" G_GINT64_FORMAT, thread_id);

	/* Alice reads her own transcript. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET", path,
	                                        alice_cookie, NULL, &alice_body,
	                                        NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(alice_body, "how much did I make"));

	/* Bob gets NOT_FOUND, not FORBIDDEN: indistinguishable from a thread
	 * that was never created. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET", path,
	                                        bob_cookie, NULL, NULL, NULL),
	                 ==, SOUP_STATUS_NOT_FOUND);

	/* And Bob's resume list does not mention it. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
	                                        "/ui/chat/threads", bob_cookie,
	                                        NULL, &bob_list, NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_null(strstr(bob_list, "alices private numbers"));

	/* The generic surfaces are owner-only, so an editor cannot reach the
	 * table the scoping protects by walking around the chat routes. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
	                                        "/api/v1/chat_thread",
	                                        bob_cookie, NULL, NULL, NULL),
	                 ==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
	                                        "/api/v1/chat_message",
	                                        bob_cookie, NULL, NULL, NULL),
	                 ==, SOUP_STATUS_FORBIDDEN);
}

/*
 * The audit log is written by the audit system as a side effect of the
 * writes it documents, and by nothing else. A POST that succeeded would let
 * an editor plant "someone else did this" rows -- an editable trail proves
 * nothing -- so the refusal must hold for every role, including the owner.
 */
static void
test_auth_audit_log_refuses_writes(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;

	server_fixture_create_user(fixture, "carol", "c-long-password",
	                           VENTURE_USER_ROLE_OWNER, NULL);
	cookie = server_fixture_login(fixture, "carol", "c-long-password");

	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/audit_entry", cookie,
		"actor=mallory&action=update", NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);

	/* Rewriting or deleting history is the same forgery. */
	g_assert_cmpuint(server_fixture_request(fixture, "PUT",
		"/api/v1/audit_entry/1", cookie, "actor=mallory", NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "DELETE",
		"/api/v1/audit_entry/1", cookie, NULL, NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);
}

/*
 * An uploaded file becomes a document record carrying its extracted text.
 * The payload here contains NUL bytes on purpose: multipart parsing of
 * binary data is exactly what regressed once, in a way a text-only test
 * cannot catch -- the parse "succeeded" with zero files.
 */
static void
test_auth_chat_upload_round_trip(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	static const gchar head[] =
		"--TESTBND\r\n"
		"Content-Disposition: form-data; name=\"file\"; "
		"filename=\"note.txt\"\r\n"
		"Content-Type: text/plain\r\n"
		"\r\n"
		"Amount due: $145.50 USD"
		"\r\n--TESTBND\r\n"
		"Content-Disposition: form-data; name=\"file2\"; "
		"filename=\"blob.bin\"\r\n"
		"Content-Type: application/octet-stream\r\n"
		"\r\n";
	static const guchar binary[] = { 0x00, 0x01, 0xff, 0x00, 0x7f };
	static const gchar tail[] = "\r\n--TESTBND--\r\n";
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *response = NULL;
	g_autofree gchar *document = NULL;
	g_autoptr(GBytes) body = NULL;
	GByteArray *raw;

	server_fixture_create_user(fixture, "dave", "d-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	cookie = server_fixture_login(fixture, "dave", "d-long-password");

	raw = g_byte_array_new();
	g_byte_array_append(raw, (const guchar *)head, strlen(head));
	g_byte_array_append(raw, binary, sizeof(binary));
	g_byte_array_append(raw, (const guchar *)tail, strlen(tail));
	body = g_byte_array_free_to_bytes(raw);

	g_assert_cmpuint(server_fixture_post_raw(fixture, "/ui/chat/upload",
		cookie, "multipart/form-data; boundary=TESTBND", body,
		&response),
		==, SOUP_STATUS_CREATED);

	/* The first file is the one taken, and its text was extracted. */
	g_assert_nonnull(strstr(response, "\"name\" : \"note.txt\""));
	g_assert_nonnull(strstr(response, "\"text_chars\" : 23"));

	/* And it is now an ordinary document record, text included. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/api/v1/document/1", cookie, NULL, &document, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(document, "Amount due: $145.50 USD"));
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


/*
 * A forge is owner-only, and a run record cannot be edited at all.
 *
 * What breaks if this regresses -- and it is a credential-theft chain, not a
 * tidiness point: a forge row holds the access token and names the host that
 * token is sent to. venture_entity_serializable_from_json() has no sensitive
 * filter, so an editor who could PUT that row could rewrite base-url to a
 * host they control and collect this install's token on the next call. The
 * staged diff would show a bland URL change and the token itself as
 * "redacted", so nothing would look wrong. Owner-only is what closes it.
 *
 * A run record is refused writes for everybody, like the audit log: it is
 * what a runner did, and an editable one proves nothing.
 */
static void
test_auth_forge_records_are_owner_only(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *editor = NULL;
	g_autofree gchar *owner = NULL;

	(void)user_data;

	server_fixture_create_user(fixture, "eddie", "e-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	editor = server_fixture_login(fixture, "eddie", "e-long-password");
	g_assert_nonnull(editor);

	/* An editor may not read a forge... */
	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/api/v1/forge",
	                                        editor, NULL, NULL, NULL),
	                 ==, SOUP_STATUS_FORBIDDEN);

	/* ...and above all may not write one. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/api/v1/forge",
		editor,
		"{\"name\":\"mine\",\"base_url\":\"https://attacker.example\"}",
		NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);

	/* A rule steers what runs unattended: admin, not editor. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
	                                        "/api/v1/forge_rule", editor,
	                                        NULL, NULL, NULL),
	                 ==, SOUP_STATUS_FORBIDDEN);

	/* The credential routes are owner-only even for a logged-in editor. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/forges/1/token",
	                                        editor, "token=stolen", NULL, NULL),
	                 ==, SOUP_STATUS_FORBIDDEN);

	/*
	 * Nor the scriptable credential routes. Adding an API form of
	 * something that was owner-only in the UI is exactly how a role
	 * check gets left behind.
	 */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/forge/1/token", editor, "{\"token\":\"stolen\"}", NULL,
		NULL),
		==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/forge/1/webhook-secret", editor, "{}", NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/forge/1/verify", editor, "", NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);

	/* An ordinary repository is ordinary data, so an editor keeps it. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
	                                        "/api/v1/forge_repo", editor,
	                                        NULL, NULL, NULL),
	                 ==, SOUP_STATUS_OK);

	/* And a run record refuses writes from the owner too. */
	server_fixture_create_user(fixture, "olive", "o-long-password",
	                           VENTURE_USER_ROLE_OWNER, NULL);
	owner = server_fixture_login(fixture, "olive", "o-long-password");
	g_assert_nonnull(owner);

	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/api/v1/forge",
	                                        owner, NULL, NULL, NULL),
	                 ==, SOUP_STATUS_OK);

	g_assert_cmpuint(server_fixture_request(fixture, "POST",
	                                        "/api/v1/forge_run", owner,
	                                        "{\"ticket_id\":1}", NULL, NULL),
	                 ==, SOUP_STATUS_FORBIDDEN);
}

/*
 * The assistant can read and propose forge configuration; approving it still
 * needs the role the direct route needs.
 *
 * What breaks if this regresses: the AI tool layer has its own type gate,
 * separate from the web role gate. Opening forge records to the assistant --
 * so it can help set the integration up, which is most of the work of using
 * it -- means an editor could ask it to point base-url at a host they
 * control and then approve the result. The REST route for a forge requires
 * owner; if approving does not, staging launders the authorisation, which is
 * the opposite of what staging is for.
 *
 * Credentials are a separate matter and are covered regardless: from_json
 * refuses sensitive members, so no tool call can set a token at all.
 */
static void
test_auth_approving_respects_the_records_own_role(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *editor = NULL;

	(void)user_data;

	server_fixture_create_user(fixture, "edna", "e-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	editor = server_fixture_login(fixture, "edna", "e-long-password");
	g_assert_nonnull(editor);

	/*
	 * This fixture has no AI provider, so the route reports that before
	 * it ever looks a confirmation up -- the guard's own branch cannot
	 * be reached from here, and exercising it would need a live model.
	 * What these three assertions pin is everything the guard rests on:
	 * that an editor passes the blanket check (so the guard is what
	 * stops them, not the check above it), that a viewer does not reach
	 * it at all, and that the direct route still refuses the editor --
	 * which is the fact the guard exists to preserve.
	 */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/confirmations/nosuchid/approve", editor, "", NULL, NULL),
		==, SOUP_STATUS_NOT_IMPLEMENTED);

	/* A viewer must not reach it at all. */
	{
		g_autofree gchar *viewer = NULL;

		server_fixture_create_user(fixture, "vic", "v-long-password",
		                           VENTURE_USER_ROLE_VIEWER, NULL);
		viewer = server_fixture_login(fixture, "vic", "v-long-password");

		g_assert_cmpuint(server_fixture_request(fixture, "POST",
			"/api/v1/confirmations/nosuchid/approve", viewer, "", NULL,
			NULL),
			==, SOUP_STATUS_FORBIDDEN);
	}

	/*
	 * And the direct route the guard mirrors still refuses that editor,
	 * which is the fact the guard exists to preserve.
	 */
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/api/v1/forge",
		editor, "{\"name\":\"theirs\",\"base_url\":\"https://attacker.example\"}",
		NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);
}

/*
 * The board's filters compose, and every control preserves the others.
 *
 * What breaks if this regresses, and why it is worth a test: each control
 * on the board is a plain link that has to rebuild the whole query string.
 * Written out per link, adding a filter means remembering it in three
 * places -- and the one that gets forgotten fails *silently*, by showing
 * more tickets than you asked for. Nobody notices a board that is slightly
 * too full. venture_web_ticket_url() exists so there is one place instead
 * of three; this is what stops somebody inlining it again.
 *
 * The second half matters as much as the first: issue-type and kind are
 * different questions -- what shape of work, and whose problem it is -- so
 * "external bugs" has to be expressible. That pair is the one most worth
 * looking at.
 */
static void
test_auth_ticket_board_filters_compose(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *unfiltered = NULL;
	g_autofree gchar *bugs = NULL;
	g_autofree gchar *external_bugs = NULL;
	g_autofree gchar *links = NULL;
	static const struct
	{
		const gchar *title;
		const gchar *issue_type;
		const gchar *kind;
	} seed[] = {
		{ "Crash on save",     "bug",      "internal" },
		{ "Add an export",     "story",    "internal" },
		{ "Cannot log in",     "bug",      "external" },
		{ "Look into caching", "research", "internal" }
	};
	gsize i;

	(void)user_data;

	server_fixture_create_user(fixture, "boardy", "b-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	cookie = server_fixture_login(fixture, "boardy", "b-long-password");
	g_assert_nonnull(cookie);

	for (i = 0; i < G_N_ELEMENTS(seed); i++)
	{
		g_autoptr(VentureTicket) ticket = NULL;
		gint value = 0;

		ticket = venture_ticket_new();
		g_assert_true(venture_enum_from_nick(VENTURE_TYPE_ISSUE_TYPE,
		                                     seed[i].issue_type, &value));
		g_object_set(ticket, "title", seed[i].title, "issue-type", value,
		             NULL);

		g_assert_true(venture_enum_from_nick(VENTURE_TYPE_TICKET_KIND,
		                                     seed[i].kind, &value));
		g_object_set(ticket, "kind", value, NULL);

		/*
		 * Saved straight to the database, so the organisation the web
		 * layer would have supplied has to be set here. Every list is
		 * scoped to the active entity, and a record belonging to none
		 * is filtered out of all of them.
		 */
		venture_entity_set_organization_id(VENTURE_ENTITY(ticket),
			venture_context_get_default_organization_id(fixture->context));

		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(ticket), NULL,
		                                    NULL));
	}

	/* Everything, so the filtered cases below mean something. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/tickets?view=board", cookie, NULL, &unfiltered, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(unfiltered, "Crash on save"));
	g_assert_nonnull(strstr(unfiltered, "Add an export"));
	g_assert_nonnull(strstr(unfiltered, "Look into caching"));

	/* One type. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/tickets?view=board&issue_type=bug", cookie, NULL, &bugs, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(bugs, "Crash on save"));
	g_assert_nonnull(strstr(bugs, "Cannot log in"));
	g_assert_null(strstr(bugs, "Add an export"));
	g_assert_null(strstr(bugs, "Look into caching"));

	/* Both at once. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/tickets?view=board&issue_type=bug&kind=external", cookie, NULL,
		&external_bugs, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(external_bugs, "Cannot log in"));
	g_assert_null(strstr(external_bugs, "Crash on save"));

	/*
	 * And the links on that page carry both filters onward. Asserted on
	 * the rendered hrefs rather than by following them, because what
	 * regresses is the URL a control builds, not the filtering it
	 * arrives at.
	 */
	links = g_strdup(external_bugs);

	/* A kind link keeps the issue type... */
	g_assert_nonnull(strstr(links,
		"/tickets?view=board&kind=internal&issue_type=bug"));
	/* ...an issue-type link keeps the kind... */
	g_assert_nonnull(strstr(links,
		"/tickets?view=board&kind=external&issue_type=story"));
	/* ...and switching view keeps both. */
	g_assert_nonnull(strstr(links,
		"/tickets?view=list&kind=external&issue_type=bug"));

	/* "all" clears one without disturbing the other. */
	g_assert_nonnull(strstr(links,
		"/tickets?view=board&kind=external&issue_type=all"));
	g_assert_nonnull(strstr(links,
		"/tickets?view=board&kind=all&issue_type=bug"));
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

	g_test_add("/auth/forge-records-are-owner-only", ServerFixture, NULL,

	           server_fixture_set_up, test_auth_forge_records_are_owner_only,

	           server_fixture_tear_down);
	g_test_add("/auth/ticket-board-filters-compose", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_ticket_board_filters_compose,
	           server_fixture_tear_down);
	g_test_add("/auth/approving-respects-the-records-own-role", ServerFixture,
	           NULL, server_fixture_set_up,
	           test_auth_approving_respects_the_records_own_role,
	           server_fixture_tear_down);
	g_test_add("/auth/api-refuses-anonymous-requests", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_api_refuses_anonymous_requests,
	           server_fixture_tear_down);
	g_test_add("/auth/health-stays-public", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_health_stays_public,
	           server_fixture_tear_down);
	g_test_add("/auth/chat-threads-are-scoped-per-user", ServerFixture, NULL,
	           server_fixture_set_up,
	           test_auth_chat_threads_are_scoped_per_user,
	           server_fixture_tear_down);
	g_test_add("/auth/audit-log-refuses-writes", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_audit_log_refuses_writes,
	           server_fixture_tear_down);
	g_test_add("/auth/chat-upload-round-trip", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_chat_upload_round_trip,
	           server_fixture_tear_down);
	g_test_add("/auth/invoice-paid-creates-the-sale", ServerFixture, NULL,
	           server_fixture_set_up,
	           test_auth_invoice_paid_creates_the_sale,
	           server_fixture_tear_down);
	g_test_add("/auth/csv-import-is-all-or-nothing", ServerFixture, NULL,
	           server_fixture_set_up,
	           test_auth_csv_import_is_all_or_nothing,
	           server_fixture_tear_down);
	g_test_add("/auth/screenshot-upload-is-an-image", ServerFixture, NULL,
	           server_fixture_set_up,
	           test_auth_screenshot_upload_is_an_image,
	           server_fixture_tear_down);

#undef ADD

	return g_test_run();
}
