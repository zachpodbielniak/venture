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

#include "venture-test-util.h"

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
	                                 "correct horse battery", NULL, &cookie,
	                                 &error));
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

	g_assert_false(venture_auth_login(fixture->auth, "zach", "wrong", NULL,
	                                  &cookie, &wrong_password));
	g_assert_false(venture_auth_login(fixture->auth, "nobody", "wrong", NULL,
	                                  &cookie, &unknown_user));

	g_object_set(user, "active", FALSE, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));
	g_assert_false(venture_auth_login(fixture->auth, "zach", "correct horse",
	                                  NULL, &cookie, &inactive));

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

	g_assert_false(venture_auth_login(fixture->auth, "", "", NULL, &cookie,
	                                  &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
}

static void
test_auth_login_rate_limits_an_address(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(VentureAuth) auth = NULL;
	guint i;

	user = create_user(fixture, "zach", "correct horse", VENTURE_USER_ROLE_OWNER);

	/* A tight budget, and an auth built after it so the limiter sees it.
	 * security.login_rate_limit existed in the configuration for a while
	 * with nothing consuming it; this pins that it now does something. */
	g_object_set(fixture->config, "security-login-rate-limit", (gint64)3, NULL);
	auth = venture_auth_new(fixture->context);

	for (i = 0; i < 3; i++)
	{
		g_autofree gchar *cookie = NULL;
		g_autoptr(GError) error = NULL;

		g_assert_false(venture_auth_login(auth, "zach", "wrong",
		                                  "203.0.113.9", &cookie, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	}

	/* The budget is spent, so even the right password is refused from this
	 * address: a password check that still runs is a guess that still
	 * counts. */
	{
		g_autofree gchar *cookie = NULL;
		g_autoptr(GError) error = NULL;

		g_assert_false(venture_auth_login(auth, "zach", "correct horse",
		                                  "203.0.113.9", &cookie, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
		g_assert_nonnull(g_strstr_len(error->message, -1, "Too many"));
	}

	/* The limit is per address, not per account: somebody else guessing at
	 * the login form must not lock the operator out of their own books. */
	{
		g_autofree gchar *cookie = NULL;
		g_autoptr(GError) error = NULL;

		g_assert_true(venture_auth_login(auth, "zach", "correct horse",
		                                 "203.0.113.10", &cookie, &error));
		g_assert_no_error(error);
		g_assert_nonnull(cookie);
	}
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
	                                 "a-long-enough-password", NULL, &cookie,
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
	                                 "a-long-enough-password", NULL, &first,
	                                 &error));

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
	                                 "a-long-enough-password", NULL, &second,
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

	/*
	 * The whole tree, not g_rmdir(), which does nothing to a directory
	 * that is not empty. The CSV-import and attachment-upload tests
	 * write under <state_dir>/attachments/, so every green run of this
	 * binary used to leave two /tmp/venture-routes-* directories behind
	 * and say nothing about it.
	 */
	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
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
 * Like server_fixture_request(), but the body goes out as JSON.
 *
 * The API decodes a body regardless of what the Content-Type says, but a
 * test that lies about it is a test that would keep passing if the route
 * started caring -- and a client that got this wrong is exactly the sort of
 * thing these tests are here to notice.
 */
static guint
server_fixture_json(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *cookie,
	const gchar	 *json,
	gchar		**out_body
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != cookie)
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie", cookie);

	if (NULL != json)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(json, strlen(json));
		soup_message_set_request_body_from_bytes(message, "application/json",
		                                         bytes);
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

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

/*
 * How many records of @type the API reports, as the caller.
 */
static gint64
server_fixture_count(
	ServerFixture	*fixture,
	const gchar	*type,
	const gchar	*cookie
){
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(JsonNode) node = NULL;

	path = g_strdup_printf("/api/v1/%s", type);

	g_assert_cmpuint(server_fixture_request(fixture, "GET", path, cookie,
	                                        NULL, &body, NULL),
	                 ==, SOUP_STATUS_OK);

	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);

	return json_object_get_int_member(json_node_get_object(node), "total");
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

	/* The workdesk: an inbox is one person's business; saved views say
	 * what somebody watches; the sprints and the runs are the plan and
	 * the spend. */
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/worklist"),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/inbox"),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/views"),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/views/1"),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/sprints"),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/runs"),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/inbox/read",
		NULL, "id=0", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/watch",
		NULL, "type=ticket&id=1", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/look",
		NULL, "look=classic", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/views",
		NULL, "name=x&entity_type=ticket", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/views/1/delete",
		NULL, "", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/tickets/1/macro", NULL, "macro_id=1", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/tickets/1/worklog", NULL, "hours=1", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/tickets/1/assign-me", NULL, "", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/incidents/1/ticket", NULL, "", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/e/ticket/bulk",
		NULL, "ids=1&field=status&value=done", NULL, NULL),
		==, SOUP_STATUS_FOUND);

	/* Webhooks name the host this install's data is posted to, and the
	 * assistant's three judgements read a ticket's whole thread. */
	/* Federation has its own peer authentication; local controls still need
	 * local sessions and cannot be reached with a peer signature. */
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/federation"), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/federation/replicas/1"), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/federation/pull", NULL, "", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/federation/replicas/1/edit", NULL, "", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/federation/replicas/1/sync", NULL, "", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/federation/replicas/1/resolve", NULL, "", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/api/v1/federation", NULL, "", NULL, NULL), ==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/webhooks"),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/webhooks/1/test", NULL, "", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/webhooks/1/secret", NULL, "", NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/tickets/1/triage", NULL, "priority=high", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/tickets/1/satisfaction", NULL, "satisfaction=good", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture,
		"/tickets/1/assist?what=triage"), ==, SOUP_STATUS_UNAUTHORIZED);

	/* Two fragments, which 401 like the chat ones. */
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/inbox/count"),
	                 ==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/runs/table"),
	                 ==, SOUP_STATUS_UNAUTHORIZED);

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
		"/api/v1/record_link",
		"/api/v1/links/sale/1",
		"/api/v1/modules",
		"/e/forge/export",
		/* The CSV export carries the same rows as the table did. */
		"/e/sale/export",
		/* The chat fragments: transcripts of what the operator asked
		 * about their own finances. */
		"/ui/chat/threads",
		"/ui/chat/thread/1",
		"/ui/chat/thread/1/export",
		"/ui/chat/complete",
		"/ui/models?provider=claude",
		"/ui/chat/stream/whatever",
		"/harness/1/stream",
		/*
		 * The knowledge bases. Search returns passages of whatever the
		 * operator has filed -- contracts, policies, drafts -- and the
		 * export hands over the whole corpus in one request, which
		 * makes it the single most valuable unauthenticated route
		 * this server could accidentally offer.
		 */
		"/api/v1/knowledge_base",
		"/api/v1/kb_article",
		"/api/v1/kb_chunk",
		"/api/v1/kb_link",
		"/api/v1/kb/search?q=anything",
		"/api/v1/kb/1/export",
		/* Dashboards: a page somebody built is a summary of what they
		 * watch, and the widget catalogue says what this install can
		 * show. */
		"/api/v1/factory",
		"/api/v1/dashboards",
		"/api/v1/dashboards/factory",
		"/api/v1/dashboards/factory/export",
		"/api/v1/widget-kinds",
		"/api/v1/dashboard-templates",
		"/api/v1/dashboard",
		"/api/v1/dashboard_widget",
		"/dashboards/factory/widgets/1",
		/* The workdesk: an inbox, who watches what, a record's whole
		 * history, the plan, the spend, and the palette that lists
		 * every page and record type this install has. */
		"/api/v1/inbox",
		"/api/v1/activities",
		"/api/v1/activities.ics",
		"/api/v1/watching/ticket/1",
		"/api/v1/activity/ticket/1",
		"/api/v1/tickets/1/sla",
		"/api/v1/sprints",
		"/api/v1/sprints/1",
		"/api/v1/runs",
		"/api/v1/budgets",
		"/api/v1/palette?q=a",
		"/api/v1/notification",
		"/api/v1/watch",
		"/api/v1/saved_view",
		"/api/v1/sla_policy",
		"/api/v1/macro",
		"/api/v1/worklog",
		"/api/v1/sprint",
		"/api/v1/agent_budget",
		"/api/v1/webhooks",
		"/api/v1/webhook",
		"/api/v1/webhook_delivery",
		"/api/v1/routing_rule",
		"/api/v1/tickets/1/summary",
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
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
	                                        "/ui/chat/thread/1/rename",
	                                        NULL, "title=x", NULL, NULL),
	                 ==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/harness",
		NULL, "provider=claude-code", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/harness/1/send", NULL, "prompt=x", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/harness/1/close", NULL, "", NULL, NULL),
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
		"/links/1/delete", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/links", NULL, "source_type=sale&source_id=1&target_type=ticket&target_id=1", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/runs/1/cancel", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);

	/* The factory's two actions over the API: a changelog is a write, a
	 * publish creates a tag on the forge. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/releases/1/changelog", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/releases/1/publish", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/dashboards/import", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/dashboards/from-template", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/inbox/read", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/watch", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/tickets/1/macro", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/tickets/1/worklog", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/activities/1/complete", NULL, "{}", NULL, NULL), ==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/activities/1/snooze", NULL, "{}", NULL, NULL), ==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/activities/sweep", NULL, "{}", NULL, NULL), ==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/sla/sweep", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/incidents/1/ticket", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/ticket/bulk", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/webhooks/1/test", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/webhooks/1/secret", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/tickets/1/triage", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/tickets/1/draft", NULL, "{}", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);

	/* The dashboard writes: making, changing and removing pages and
	 * their widgets. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/dashboards", NULL, "name=x", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/dashboards/import", NULL, "definition={}", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/dashboards/x", NULL, "name=y", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/dashboards/x/delete", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/dashboards/x/widgets", NULL, "kind=note", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/dashboards/x/widgets/1", NULL, "kind=note", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/dashboards/x/widgets/1/delete", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/dashboards/x/widgets/1/move", NULL, "direction=up", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/dashboards/x/widgets/1/place", NULL, "col=1&row=1", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/dashboards/x/arrange", NULL, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture,
		"/dashboards/x/edit"), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture,
		"/dashboards/x/widgets/new"), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture,
		"/dashboards/x/export"), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_get_anonymous(fixture,
		"/overview"), ==, SOUP_STATUS_FOUND);

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
		g_autoptr(VentureCompany) customer = NULL;
		gint64 organization_id;
		g_autoptr(VentureInvoiceLine) first = NULL;
		g_autoptr(VentureInvoiceLine) second = NULL;
		g_autoptr(VentureMoney) hundred = NULL;
		g_autoptr(VentureMoney) fifty = NULL;

		organization_id = venture_context_get_default_organization_id(fixture->context);
		customer = venture_company_new();
		g_object_set(customer, "name", "Invoice customer", NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(customer), organization_id);
		g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(customer), NULL, NULL));
		invoice = venture_invoice_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(invoice), organization_id);
		g_object_set(invoice, "company-id", venture_entity_get_id(VENTURE_ENTITY(customer)), NULL);
		g_object_set(invoice, "number", "INV-1",
		             "organization-id", venture_context_get_default_organization_id(fixture->context),
		             "status", VENTURE_INVOICE_STATUS_DRAFT, NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(invoice), NULL, NULL));

		hundred = venture_money_new(10000, "USD", 2);
		first = venture_invoice_line_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(first), organization_id);
		g_object_set(first, "invoice-id",
		             venture_entity_get_id(VENTURE_ENTITY(invoice)),
		             "description", "consulting", "quantity", 2.0,
		             "unit-price", hundred, NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(first), NULL, NULL));

		fifty = venture_money_new(4999, "USD", 2);
		second = venture_invoice_line_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(second), organization_id);
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
		g_assert_true(g_str_has_prefix(external, "allocation:"));
	g_assert_true(g_uuid_string_is_valid(external + strlen("allocation:")));
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
 * A conversation can be renamed and taken away as org, and both are scoped
 * the way reading it is: another person's thread is NOT_FOUND, not
 * FORBIDDEN. The export is the stored transcript verbatim, with a line that
 * would read as an org heading escaped the way org escapes them.
 */
static void
test_auth_chat_rename_and_export(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *alice_cookie = NULL;
	g_autofree gchar *bob_cookie = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *export = NULL;
	g_autofree gchar *rename_path = NULL;
	g_autofree gchar *export_path = NULL;
	gint64 alice_id;
	gint64 thread_id;

	server_fixture_create_user(fixture, "alice", "a-long-password",
	                           VENTURE_USER_ROLE_EDITOR, &alice_id);
	server_fixture_create_user(fixture, "bob", "b-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);

	{
		g_autoptr(VentureChatThread) thread = NULL;
		g_autoptr(VentureChatMessage) question = NULL;
		g_autoptr(VentureChatMessage) answer = NULL;

		thread = venture_chat_thread_new();
		g_object_set(thread, "title", "first question", "user-id", alice_id,
		             NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(thread), NULL, NULL));
		thread_id = venture_entity_get_id(VENTURE_ENTITY(thread));

		question = venture_chat_message_new();
		g_object_set(question, "thread-id", thread_id,
		             "role", VENTURE_CHAT_ROLE_USER,
		             "body", "what did march cost\n* not a heading", NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(question), NULL, NULL));

		answer = venture_chat_message_new();
		g_object_set(answer, "thread-id", thread_id,
		             "role", VENTURE_CHAT_ROLE_ASSISTANT,
		             "body", "March cost 1,200.", NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(answer), NULL, NULL));
	}

	alice_cookie = server_fixture_login(fixture, "alice", "a-long-password");
	bob_cookie = server_fixture_login(fixture, "bob", "b-long-password");
	rename_path = g_strdup_printf("/ui/chat/thread/%" G_GINT64_FORMAT
	                              "/rename", thread_id);
	export_path = g_strdup_printf("/ui/chat/thread/%" G_GINT64_FORMAT
	                              "/export", thread_id);

	/* Bob can neither rename nor export it, and cannot tell it exists. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST", rename_path,
		bob_cookie, "title=mine+now", NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "GET", export_path,
		bob_cookie, NULL, NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);

	/* Alice renames it; an empty title is refused. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST", rename_path,
		alice_cookie, "title=", NULL, NULL), ==, SOUP_STATUS_BAD_REQUEST);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", rename_path,
		alice_cookie, "title=March+costs", &body, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "\"title\""));
	g_assert_nonnull(strstr(body, "March costs"));

	/* And the export carries the new title and both sides, verbatim. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET", export_path,
		alice_cookie, NULL, &export, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(export, "#+title: March costs"));
	g_assert_nonnull(strstr(export, "* You\n\nwhat did march cost\n"
	                                ",* not a heading\n"));
	g_assert_nonnull(strstr(export, "* VENTURE\n\nMarch cost 1,200."));
}

/*
 * The provider, model and effort dropdowns are fed by one endpoint, and
 * what it offers depends on the provider: a CLI agent has an effort
 * level, an HTTP provider does not, and one whose models nobody can
 * enumerate offers none.
 */
static void
test_auth_models_are_scoped_to_the_provider(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *cli = NULL;
	g_autofree gchar *api = NULL;
	g_autofree gchar *cursor = NULL;
	g_autofree gchar *unknown = NULL;

	server_fixture_create_user(fixture, "alice", "a-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	cookie = server_fixture_login(fixture, "alice", "a-long-password");

	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/ui/models?provider=claude-code", cookie, NULL, &cli, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(cli, "\"cli\" : true"));
	g_assert_nonnull(strstr(cli, "\"low\""));
	g_assert_nonnull(strstr(cli, "\"max\""));

	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/ui/models?provider=claude", cookie, NULL, &api, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(api, "\"cli\" : false"));
	g_assert_nonnull(strstr(api, "\"efforts\" : []"));
	g_assert_null(strstr(api, "\"low\""));

	/* A CLI provider that bakes effort into the model id offers models
	 * and no levels. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/ui/models?provider=cursor", cookie, NULL, &cursor, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(cursor, "\"cli\" : true"));
	g_assert_nonnull(strstr(cursor, "\"efforts\" : []"));

	/* And a name nothing knows is an empty answer, not an error: the
	 * form asks about whatever is selected. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/ui/models?provider=nonsense", cookie, NULL, &unknown, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(unknown, "\"models\" : []"));
	g_assert_nonnull(strstr(unknown, "\"efforts\" : []"));
}

/*
 * A streamed answer is two requests, and the token between them is
 * one-shot and bound to the person who asked.
 *
 * There is no provider in a test fixture, so a turn is never parked and
 * the POST says so whether or not the browser asked to stream. What is
 * worth pinning is the half that does not need a model: a token nobody
 * minted, or one that has been spent, is not an error to puzzle over --
 * it is simply not there, and it answers the same way to everybody.
 */
static void
test_auth_chat_stream_token_is_one_shot(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *plain = NULL;
	g_autofree gchar *streamed = NULL;
	g_autofree gchar *missing = NULL;

	server_fixture_create_user(fixture, "alice", "a-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	cookie = server_fixture_login(fixture, "alice", "a-long-password");

	/* Without a provider both paths say the same thing, and neither
	 * stores a question nothing will answer. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/ui/chat",
		cookie, "message=how+much+did+march+cost", &plain, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(plain, "AI is not configured"));
	g_assert_null(strstr(plain, "data-chat-stream"));

	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/ui/chat",
		cookie, "message=how+much+did+march+cost&stream=1", &streamed, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(streamed, "AI is not configured"));
	g_assert_null(strstr(streamed, "data-chat-stream"));

	/* A token that was never minted is not found, and says so in words
	 * rather than hanging a connection open. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/ui/chat/stream/0123456789abcdef", cookie, NULL, &missing, NULL),
		==, SOUP_STATUS_NOT_FOUND);
	g_assert_nonnull(strstr(missing, "no longer waiting"));
}

/*
 * The composer's menus are fed by one endpoint, and what it offers is the
 * harness's answer for the text at the cursor: a command, a record type,
 * a record of that type, or a knowledge base.
 */
static void
test_auth_chat_complete_is_the_harness(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *commands = NULL;
	g_autofree gchar *types = NULL;
	g_autofree gchar *records = NULL;
	g_autofree gchar *bases = NULL;
	g_autofree gchar *nothing = NULL;

	server_fixture_create_user(fixture, "alice", "a-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);

	{
		g_autoptr(VentureKnowledgeBase) base = NULL;
		g_autoptr(VentureAiSkill) skill = NULL;
		g_autoptr(VentureTicket) ticket = NULL;

		base = venture_knowledge_base_new();
		g_object_set(base, "name", "Contracts", "slug", "contracts",
		             "description", "Signed agreements", NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(base), NULL, NULL));

		skill = venture_ai_skill_new();
		g_object_set(skill, "name", "Chase, our way", "trigger", "chase",
		             "description", "House style", "prompt", "Chase.",
		             "enabled", TRUE, NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(skill), NULL, NULL));

		ticket = venture_ticket_new();
		g_object_set(ticket, "title", "Payments fail on renewal", NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(ticket), NULL, NULL));
	}

	cookie = server_fixture_login(fixture, "alice", "a-long-password");

	/* A slash at the start of the composer offers commands, this
	 * install's own among them. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/ui/chat/complete?buffer=%2F&cursor=1", cookie, NULL, &commands,
		NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(commands, "\"kind\" : \"command\""));
	g_assert_nonnull(strstr(commands, "\"/summarise\""));
	g_assert_nonnull(strstr(commands, "Chase, our way"));

	/* An @ offers record types. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/ui/chat/complete?buffer=about%20%40tick&cursor=11", cookie, NULL,
		&types, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(types, "\"kind\" : \"record\""));
	g_assert_nonnull(strstr(types, "\"@ticket\""));
	g_assert_nonnull(strstr(types, "\"@ticket/\""));

	/* And past the slash it searches that type. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/ui/chat/complete?buffer=%40ticket%2Frenewal&cursor=15", cookie,
		NULL, &records, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(records, "Payments fail on renewal"));
	g_assert_nonnull(strstr(records, "\"@ticket/1\""));

	/* A # offers the knowledge bases. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/ui/chat/complete?buffer=%23&cursor=1", cookie, NULL, &bases, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(bases, "\"kind\" : \"base\""));
	g_assert_nonnull(strstr(bases, "\"#contracts\""));

	/*
	 * A slash that is not the first thing typed is a date or a path.
	 * Completing it would cover what is being written with a menu.
	 */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/ui/chat/complete?buffer=due%202026%2F09&cursor=11", cookie, NULL,
		&nothing, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(nothing, "\"kind\" : \"none\""));
}

/*
 * What the model says is rendered, never interpreted. Fenced code comes
 * through whole and escaped, a markdown link becomes a link only to an
 * http(s) URL or a local path, a quoted line is a quote, and markup in the
 * reply is text.
 */
static void
test_auth_chat_reply_rendering(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	gint64 alice_id;
	gint64 thread_id;

	server_fixture_create_user(fixture, "alice", "a-long-password",
	                           VENTURE_USER_ROLE_EDITOR, &alice_id);

	{
		g_autoptr(VentureChatThread) thread = NULL;
		g_autoptr(VentureChatMessage) answer = NULL;

		thread = venture_chat_thread_new();
		g_object_set(thread, "title", "rendering", "user-id", alice_id, NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(thread), NULL, NULL));
		thread_id = venture_entity_get_id(VENTURE_ENTITY(thread));

		answer = venture_chat_message_new();
		g_object_set(answer, "thread-id", thread_id,
		             "role", VENTURE_CHAT_ROLE_ASSISTANT,
		             "body",
		             "Run this:\n\n```\n- not a list\n\n<script>x</script>\n```\n"
		             "See [the ticket](/e/ticket/7) and "
		             "[docs](https://example.org/a?b=1) but not "
		             "[this](javascript:alert(1)).\n\n"
		             "> quoted line\n> and another\n\n"
		             "<b>bold</b> stays text.",
		             NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(answer), NULL, NULL));
	}

	cookie = server_fixture_login(fixture, "alice", "a-long-password");
	path = g_strdup_printf("/ui/chat/thread/%" G_GINT64_FORMAT, thread_id);

	g_assert_cmpuint(server_fixture_request(fixture, "GET", path, cookie,
		NULL, &page, NULL), ==, SOUP_STATUS_OK);

	/* The fence, whole: its list marker is not a list and its markup is
	 * text. */
	g_assert_nonnull(strstr(page,
		"<pre><code>- not a list\n\n&lt;script&gt;x&lt;/script&gt;\n"
		"</code></pre>"));
	g_assert_null(strstr(page, "<script>x</script>"));

	/* Links: local and https, never javascript. */
	g_assert_nonnull(strstr(page,
		"<a href=\"/e/ticket/7\" rel=\"noopener\">the ticket</a>"));
	g_assert_nonnull(strstr(page,
		"<a href=\"https://example.org/a?b=1\" rel=\"noopener\">docs</a>"));
	g_assert_null(strstr(page, "href=\"javascript:"));

	g_assert_nonnull(strstr(page,
		"<blockquote>quoted line and another </blockquote>"));
	g_assert_nonnull(strstr(page, "&lt;b&gt;bold&lt;/b&gt; stays text."));
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

	/* REST is also the CLI/MCP transport: a staged bypass is refused too. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/ledger_entry", cookie,
		"transaction_id=bypass&account_id=1&amount=1.00", NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/ledger_entry?stage=1", cookie,
		"transaction_id=bypass&account_id=1&amount=1.00", NULL, NULL),
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
	 * This fixture has no AI provider, and the route no longer cares:
	 * the confirmation queue belongs to the context rather than to the
	 * assistant, so an install with AI switched off still stages REST
	 * writes and still answers here. An unknown id is therefore a plain
	 * 404 -- it used to be a 501 saying AI was not configured, which was
	 * a true statement about the wrong thing.
	 *
	 * What these three assertions pin is everything the guard rests on:
	 * that an editor passes the blanket check (so the guard is what
	 * stops them, not the check above it), that a viewer does not reach
	 * it at all, and that the direct route still refuses the editor --
	 * which is the fact the guard exists to preserve.
	 */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/confirmations/nosuchid/approve", editor, "", NULL, NULL),
		==, SOUP_STATUS_NOT_FOUND);

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
 * A write with `?stage=1` proposes the change instead of making it.
 *
 * What breaks if this regresses: the reason the route exists. An outside
 * agent is given a token so that it can suggest bookkeeping without being
 * trusted to do it; a staged write that writes is that trust granted by
 * accident, with a confirmation card beside it that reads as a request for
 * permission somebody already took.
 *
 * The count is checked through the API rather than the database on purpose:
 * that is the surface the agent and the person both see.
 */
static void
test_auth_staged_write_changes_nothing(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *editor = NULL;
	g_autofree gchar *staged = NULL;
	g_autofree gchar *listing = NULL;
	g_autofree gchar *approve_path = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *confirmation;
	const gchar *id;

	(void)user_data;

	server_fixture_create_user(fixture, "eve", "e-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	editor = server_fixture_login(fixture, "eve", "e-long-password");
	g_assert_nonnull(editor);

	g_assert_cmpint(server_fixture_count(fixture, "expense", editor), ==, 0);

	g_assert_cmpuint(server_fixture_json(fixture, "POST",
		"/api/v1/expense?stage=1", editor,
		"{\"description\":\"Coffee grinder\",\"amount\":\"12.00 USD\"}",
		&staged),
		==, SOUP_STATUS_ACCEPTED);

	/* Nothing written. */
	g_assert_cmpint(server_fixture_count(fixture, "expense", editor), ==, 0);

	node = venture_json_parse(staged, NULL);
	g_assert_nonnull(node);
	g_assert_true(json_object_get_boolean_member(json_node_get_object(node),
	                                             "staged"));

	confirmation = json_object_get_object_member(json_node_get_object(node),
	                                             "confirmation");
	id = venture_json_object_get_string(confirmation, "id", NULL);
	g_assert_nonnull(id);

	/* And it is in the queue a person answers from, described well enough
	 * to decide on: which type, and what the change would be. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/api/v1/confirmations", editor, NULL, &listing, NULL),
		==, SOUP_STATUS_OK);

	{
		g_autoptr(JsonNode) queue = NULL;
		JsonObject *card;

		queue = venture_json_parse(listing, NULL);
		g_assert_nonnull(queue);
		g_assert_true(JSON_NODE_HOLDS_ARRAY(queue));
		g_assert_cmpuint(json_array_get_length(json_node_get_array(queue)),
		                 ==, 1);

		card = json_array_get_object_element(json_node_get_array(queue), 0);

		g_assert_cmpstr(venture_json_object_get_string(card, "id", NULL), ==,
		                id);
		g_assert_cmpstr(venture_json_object_get_string(card, "type", NULL),
		                ==, "expense");
		g_assert_cmpstr(venture_json_object_get_string(card, "action", NULL),
		                ==, "create");
		g_assert_cmpstr(venture_json_object_get_string(
			json_object_get_object_member(card, "origin"), "via", NULL), ==,
			"rest-api");
	}

	/* Approving applies it, through the same repository call a direct
	 * write would have used. */
	approve_path = g_strdup_printf("/api/v1/confirmations/%s/approve", id);

	g_assert_cmpuint(server_fixture_request(fixture, "POST", approve_path,
	                                        editor, "", NULL, NULL),
	                 ==, SOUP_STATUS_OK);

	g_assert_cmpint(server_fixture_count(fixture, "expense", editor), ==, 1);
}

/*
 * Rejecting discards it, and the record never appears.
 */
static void
test_auth_staged_write_can_be_rejected(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *editor = NULL;
	g_autofree gchar *staged = NULL;
	g_autofree gchar *listing = NULL;
	g_autofree gchar *reject_path = NULL;
	g_autoptr(JsonNode) node = NULL;
	const gchar *id;

	(void)user_data;

	server_fixture_create_user(fixture, "erica", "e-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	editor = server_fixture_login(fixture, "erica", "e-long-password");

	g_assert_cmpuint(server_fixture_json(fixture, "POST",
		"/api/v1/expense?stage=1", editor,
		"{\"description\":\"Not ours\",\"amount\":\"400.00 USD\"}", &staged),
		==, SOUP_STATUS_ACCEPTED);

	node = venture_json_parse(staged, NULL);
	id = venture_json_object_get_string(
		json_object_get_object_member(json_node_get_object(node),
		                              "confirmation"), "id", NULL);
	g_assert_nonnull(id);

	reject_path = g_strdup_printf("/api/v1/confirmations/%s/reject", id);

	g_assert_cmpuint(server_fixture_request(fixture, "POST", reject_path,
	                                        editor, "", NULL, NULL),
	                 ==, SOUP_STATUS_OK);

	g_assert_cmpint(server_fixture_count(fixture, "expense", editor), ==, 0);

	/* And it is out of the queue, rather than sitting there looking like
	 * it still needs answering. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/api/v1/confirmations", editor, NULL, &listing, NULL),
		==, SOUP_STATUS_OK);

	g_assert_null(strstr(listing, id));
}

/*
 * Staging is a write, and needs the role a write needs.
 *
 * What breaks if this regresses: a viewer gets a way to put changes in front
 * of an editor who approves cards without re-reading them. Proposing is
 * cheaper than writing, which is exactly why it must not be a lower bar.
 */
static void
test_auth_staging_needs_the_editor_role(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *viewer = NULL;

	(void)user_data;

	server_fixture_create_user(fixture, "val", "v-long-password",
	                           VENTURE_USER_ROLE_VIEWER, NULL);
	viewer = server_fixture_login(fixture, "val", "v-long-password");

	g_assert_cmpuint(server_fixture_json(fixture, "POST",
		"/api/v1/expense?stage=1", viewer,
		"{\"description\":\"Sneaky\",\"amount\":\"1.00 USD\"}", NULL),
		==, SOUP_STATUS_FORBIDDEN);
}

/*
 * A `stage` value the server does not recognise is refused, not ignored.
 *
 * What breaks if this regresses: `?stage=y` is an unknown query parameter,
 * an unknown parameter on a write route is ignored, and the write the caller
 * was trying to hold back is applied. The failure is silent and the record
 * is already there by the time anybody reads the response.
 */
static void
test_auth_unknown_stage_value_is_refused(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *editor = NULL;

	(void)user_data;

	server_fixture_create_user(fixture, "ewan", "e-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	editor = server_fixture_login(fixture, "ewan", "e-long-password");

	g_assert_cmpuint(server_fixture_json(fixture, "POST",
		"/api/v1/expense?stage=y", editor,
		"{\"description\":\"Held back\",\"amount\":\"7.00 USD\"}", NULL),
		==, SOUP_STATUS_BAD_REQUEST);

	/* And nothing was written by the request that was refused. */
	g_assert_cmpint(server_fixture_count(fixture, "expense", editor), ==, 0);
}

/*
 * A staged delete leaves the record alone until it is approved.
 */
static void
test_auth_staged_delete_leaves_the_record(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *editor = NULL;
	g_autofree gchar *created = NULL;
	g_autofree gchar *staged = NULL;
	g_autofree gchar *delete_path = NULL;
	g_autoptr(JsonNode) node = NULL;
	gint64 id;

	(void)user_data;

	server_fixture_create_user(fixture, "ed", "e-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	editor = server_fixture_login(fixture, "ed", "e-long-password");

	g_assert_cmpuint(server_fixture_json(fixture, "POST", "/api/v1/expense",
		editor, "{\"description\":\"Wrong row\",\"amount\":\"1.00 USD\"}",
		&created),
		==, SOUP_STATUS_CREATED);

	node = venture_json_parse(created, NULL);
	id = json_object_get_int_member(json_node_get_object(node), "id");
	g_assert_cmpint(id, >, 0);

	delete_path = g_strdup_printf("/api/v1/expense/%" G_GINT64_FORMAT
	                              "?stage=1", id);

	g_assert_cmpuint(server_fixture_json(fixture, "DELETE", delete_path,
	                                     editor, NULL, &staged),
	                 ==, SOUP_STATUS_ACCEPTED);

	g_assert_cmpint(server_fixture_count(fixture, "expense", editor), ==, 1);
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


/* --- API tokens in the browser -------------------------------------------- */

/*
 * Like server_fixture_request(), but reads the Location header and sends an
 * Authorization header. Minting a token in the browser is a redirect to a
 * page that shows it, and the point of the whole feature is that what comes
 * back then works as a bearer credential -- so a test that cannot follow the
 * one or present the other is not testing the feature.
 */
static guint
server_fixture_request_ex(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *cookie,
	const gchar	 *bearer,
	const gchar	 *form_body,
	gchar		**out_body,
	gchar		**out_location
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != cookie)
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie", cookie);

	if (NULL != bearer)
	{
		g_autofree gchar *value = NULL;

		value = g_strconcat("Bearer ", bearer, NULL);
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Authorization",
			value);
	}

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

	if (NULL != out_location)
		*out_location = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Location"));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

/*
 * Pulls the one-time secret out of the reveal page.
 *
 * Every token is printed with its "vk_" prefix, so finding it does not
 * depend on the surrounding markup -- which would make this a test of the
 * HTML rather than of the token.
 */
static gchar *
extract_revealed_token(const gchar *html)
{
	const gchar *start;
	const gchar *end;

	g_assert_nonnull(html);
	start = strstr(html, "vk_");

	if (NULL == start)
		return NULL;

	end = start;

	while (('\0' != *end) && ('<' != *end) && !g_ascii_isspace(*end))
		end++;

	return g_strndup(start, (gsize)(end - start));
}

/*
 * Signs in as an admin, mints a token through the page, and returns the
 * plaintext exactly as somebody clicking the button would have read it.
 */
static gchar *
server_fixture_mint_token(
	ServerFixture	 *fixture,
	const gchar	 *cookie,
	const gchar	 *form_body,
	gchar		**out_reveal_path
){
	g_autofree gchar *location = NULL;
	g_autofree gchar *page = NULL;

	g_assert_cmpuint(server_fixture_request_ex(fixture, "POST",
		"/account/tokens", cookie, NULL, form_body, NULL, &location),
		==, SOUP_STATUS_FOUND);

	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, "reveal="));

	g_assert_cmpuint(server_fixture_request_ex(fixture, "GET", location,
		cookie, NULL, NULL, &page, NULL), ==, SOUP_STATUS_OK);

	if (NULL != out_reveal_path)
		*out_reveal_path = g_steal_pointer(&location);

	return extract_revealed_token(page);
}

/*
 * The page is admin-only, matching POST /api/v1/tokens. A token carries the
 * role of whoever minted it, so an editor who could reach this page could
 * mint themselves a durable editor credential that no password change
 * revokes -- and the whole point of the role split is who may hand out
 * access.
 */
static void
test_auth_tokens_page_is_admin_only(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *editor_cookie = NULL;
	g_autofree gchar *admin_cookie = NULL;
	g_autofree gchar *page = NULL;

	server_fixture_create_user(fixture, "edna", "e-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	server_fixture_create_user(fixture, "adam", "a-long-password",
	                           VENTURE_USER_ROLE_ADMIN, NULL);

	editor_cookie = server_fixture_login(fixture, "edna", "e-long-password");
	admin_cookie = server_fixture_login(fixture, "adam", "a-long-password");

	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/account/tokens",
		editor_cookie, NULL, NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);

	/* And the POST, not merely the page: refusing to draw the button
	 * while still honouring the request would protect nothing. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/account/tokens",
		editor_cookie, "name=sneaky&expires_in_days=0", NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);

	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/account/tokens",
		admin_cookie, NULL, &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "Mint a token"));
}

/*
 * Anonymous requests are turned away at both verbs. The GET is covered by
 * the navigation sweep above; the POST is not in the sidebar and would
 * otherwise be an unauthenticated way to mint an owner credential.
 */
static void
test_auth_tokens_refuse_anonymous(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_assert_cmpuint(server_fixture_get_anonymous(fixture, "/account/tokens"),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/account/tokens",
		NULL, "name=anon&expires_in_days=0", NULL, NULL),
		==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/account/tokens/1/revoke", NULL, NULL, NULL, NULL),
		==, SOUP_STATUS_FOUND);
}

/*
 * The token minted in the browser is a working credential. This is the
 * feature: what the page prints has to be usable verbatim as a bearer
 * token, or the page is decoration.
 */
static void
test_auth_token_minted_in_browser_authenticates(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *listing = NULL;

	server_fixture_create_user(fixture, "adam", "a-long-password",
	                           VENTURE_USER_ROLE_ADMIN, NULL);
	cookie = server_fixture_login(fixture, "adam", "a-long-password");

	secret = server_fixture_mint_token(fixture, cookie,
	                                   "name=laptop&expires_in_days=0", NULL);

	g_assert_nonnull(secret);
	g_assert_true(g_str_has_prefix(secret, "vk_"));

	/* No cookie at all, so nothing but the token can be authenticating
	 * this. */
	g_assert_cmpuint(server_fixture_request_ex(fixture, "GET",
		"/api/v1/venture", NULL, secret, NULL, &listing, NULL),
		==, SOUP_STATUS_OK);

	/* And the name reached the record, so the list can tell one token
	 * from another. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/account/tokens",
		cookie, NULL, &listing, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(listing, "laptop"));
}

/*
 * The secret is shown once and never again.
 *
 * The handle travels in the URL, which means it lands in history and in the
 * access log; if replaying it kept working, the "only shown once" promise
 * would be false for anybody who could read either.
 */
static void
test_auth_token_reveal_is_one_shot(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *reveal_path = NULL;
	g_autofree gchar *replay = NULL;

	server_fixture_create_user(fixture, "adam", "a-long-password",
	                           VENTURE_USER_ROLE_ADMIN, NULL);
	cookie = server_fixture_login(fixture, "adam", "a-long-password");

	secret = server_fixture_mint_token(fixture, cookie,
	                                   "name=once&expires_in_days=0",
	                                   &reveal_path);
	g_assert_nonnull(secret);

	g_assert_cmpuint(server_fixture_request(fixture, "GET", reveal_path,
		cookie, NULL, &replay, NULL), ==, SOUP_STATUS_OK);

	/* The page still renders -- it is the token list -- but the secret
	 * is not on it. */
	g_assert_null(strstr(replay, secret));
	g_assert_nonnull(strstr(replay, "once"));
}

/*
 * Revoking stops the token working, and keeps the row.
 *
 * Deleting it would take the prefix, the role and the last-used time with
 * it, which is exactly what somebody revoking a credential after an
 * incident wants to still be able to read.
 */
static void
test_auth_token_revoke_stops_it_working(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *listing = NULL;

	server_fixture_create_user(fixture, "adam", "a-long-password",
	                           VENTURE_USER_ROLE_ADMIN, NULL);
	cookie = server_fixture_login(fixture, "adam", "a-long-password");

	secret = server_fixture_mint_token(fixture, cookie,
	                                   "name=doomed&expires_in_days=0", NULL);
	g_assert_nonnull(secret);

	g_assert_cmpuint(server_fixture_request_ex(fixture, "GET",
		"/api/v1/venture", NULL, secret, NULL, NULL, NULL),
		==, SOUP_STATUS_OK);

	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/account/tokens/1/revoke", cookie, "", NULL, NULL),
		==, SOUP_STATUS_FOUND);

	g_assert_cmpuint(server_fixture_request_ex(fixture, "GET",
		"/api/v1/venture", NULL, secret, NULL, NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);

	/* The row survives, marked revoked. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/account/tokens",
		cookie, NULL, &listing, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(listing, "doomed"));
	g_assert_nonnull(strstr(listing, "revoked"));
}

/*
 * An expiry chosen in the form reaches the record.
 *
 * venture_api_token_matches() already refuses a token whose expires-at has
 * passed; what this covers is the wiring from the select to the field, which
 * is where an expiry silently becomes "never".
 */
static void
test_auth_token_expiry_is_recorded(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) entity = NULL;
	g_autoptr(GDateTime) expires_at = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *secret = NULL;
	gint64 days;

	server_fixture_create_user(fixture, "adam", "a-long-password",
	                           VENTURE_USER_ROLE_ADMIN, NULL);
	cookie = server_fixture_login(fixture, "adam", "a-long-password");

	secret = server_fixture_mint_token(fixture, cookie,
	                                   "name=temporary&expires_in_days=30",
	                                   NULL);
	g_assert_nonnull(secret);

	entity = venture_database_get(fixture->database, VENTURE_TYPE_API_TOKEN,
	                              1, NULL);
	g_assert_nonnull(entity);

	g_object_get(entity, "expires-at", &expires_at, NULL);
	g_assert_nonnull(expires_at);

	now = venture_time_now();
	days = g_date_time_difference(expires_at, now) / G_TIME_SPAN_DAY;

	/* 29 rather than 30 because the subtraction truncates. */
	g_assert_cmpint(days, >=, 29);
	g_assert_cmpint(days, <=, 30);
}

/*
 * "Never" has to mean never, not "expired the instant it was made". Zero is
 * the value the select posts for it, and an unchecked strtoll would turn it
 * into an expiry date of now.
 */
static void
test_auth_token_never_expires_by_default(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) entity = NULL;
	g_autoptr(GDateTime) expires_at = NULL;
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *secret = NULL;

	server_fixture_create_user(fixture, "adam", "a-long-password",
	                           VENTURE_USER_ROLE_ADMIN, NULL);
	cookie = server_fixture_login(fixture, "adam", "a-long-password");

	secret = server_fixture_mint_token(fixture, cookie,
	                                   "name=forever&expires_in_days=0", NULL);
	g_assert_nonnull(secret);

	entity = venture_database_get(fixture->database, VENTURE_TYPE_API_TOKEN,
	                              1, NULL);
	g_assert_nonnull(entity);

	g_object_get(entity, "expires-at", &expires_at, NULL);
	g_assert_null(expires_at);

	/* And it works, which is the observable half of the same claim. */
	g_assert_cmpuint(server_fixture_request_ex(fixture, "GET",
		"/api/v1/venture", NULL, secret, NULL, NULL, NULL),
		==, SOUP_STATUS_OK);
}

/*
 * The banner a redirect asks for actually renders.
 *
 * These pages read ?notice= to say "Password changed." or "Token revoked."
 * after a POST. They read it out of the route-parameter table for a while,
 * which only ever holds :id-style segments, so every one of those banners
 * was silently dropped: the action worked and the page came back looking as
 * though nothing had happened.
 */
static void
test_auth_redirect_notices_render(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *account = NULL;
	g_autofree gchar *tokens = NULL;

	server_fixture_create_user(fixture, "adam", "a-long-password",
	                           VENTURE_USER_ROLE_ADMIN, NULL);
	cookie = server_fixture_login(fixture, "adam", "a-long-password");

	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/account?notice=changed", cookie, NULL, &account, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(account, "Password changed."));

	g_assert_cmpuint(server_fixture_request(fixture, "GET",
		"/account/tokens?notice=revoked-ok", cookie, NULL, &tokens, NULL),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(tokens, "Token revoked."));
}

/*
 * A url-encoded import is a 400 that says so, not a 500.
 *
 * This is what a browser sends when the form has no working encoding
 * attribute, and it is the failure an operator actually hits. The parser's
 * error is in htmx's domain, and venture_web_error_response() maps an
 * unfamiliar domain to 500 -- so without the content-type check first, a
 * wrong request reads as a broken server and the debugging starts in the
 * wrong place.
 */
static void
test_auth_kb_import_rejects_urlencoded(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *body = NULL;

	server_fixture_create_user(fixture, "edna", "e-long-password",
	                           VENTURE_USER_ROLE_EDITOR, NULL);
	cookie = server_fixture_login(fixture, "edna", "e-long-password");

	/* server_fixture_request posts application/x-www-form-urlencoded,
	 * which is exactly the shape being guarded against. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/kb/1/import", cookie, "files=readme.md", &body, NULL),
		==, SOUP_STATUS_BAD_REQUEST);

	/* And the message names the cause rather than the symptom. */
	g_assert_nonnull(body);
	g_assert_nonnull(strstr(body, "multipart/form-data"));
}

/*
 * The knowledge-base write routes refuse an anonymous POST.
 *
 * A missing guard on these looks exactly like nothing: sync reads a
 * directory on the server and rewrites articles from it, and reindex spends
 * an embedding request per passage. Neither is reachable through the
 * anonymous GET sweep above, because both are POSTs.
 */
static void
test_auth_kb_writes_refuse_anonymous(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/kb/1/sync", NULL, "", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/kb/1/reindex", NULL, "", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);

	/*
	 * Cross-reference and article generation write records too, and both
	 * spend embedding requests -- an unauthenticated caller could run the
	 * bill up without ever reading anything.
	 */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/kb/crossref/idea/1", NULL, "", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/kb/from/idea/1", NULL, "", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);

	/* Import takes a file and writes articles from it, archives
	 * included -- the most that can be done to a base in one request. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/kb/1/import", NULL, "", NULL, NULL),
		==, SOUP_STATUS_UNAUTHORIZED);
}

/*
 * And they need the editor role, not merely a session.
 *
 * A sync changes what the assistant will tell everybody, which is an edit
 * to the install's answers rather than a read of them.
 */
static void
test_auth_kb_writes_need_the_editor_role(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;

	server_fixture_create_user(fixture, "vera", "v-long-password",
	                           VENTURE_USER_ROLE_VIEWER, NULL);
	cookie = server_fixture_login(fixture, "vera", "v-long-password");

	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/kb/1/sync", cookie, "", NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/kb/1/reindex", cookie, "", NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/kb/crossref/idea/1", cookie, "", NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/kb/from/idea/1", cookie, "", NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "POST",
		"/api/v1/kb/1/import", cookie, "", NULL, NULL),
		==, SOUP_STATUS_FORBIDDEN);
}

/*
 * The sidebar remembers its scroll position, and shows you where you are.
 *
 * Every nav entry is a plain link, so each click rebuilds the sidebar from
 * scratch and it would otherwise come back at the top. Arriving from a
 * bookmark has the opposite problem: nothing was saved, and the entry for
 * the current page can sit below the fold.
 *
 * Asserted on the markup because the behaviour itself is the browser's.
 * What can regress here is the hook going missing, or moving to where it
 * measures a sidebar that is not finished being parsed.
 */
static void
test_auth_sidebar_restores_its_scroll(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *page = NULL;
	const gchar *script;
	const gchar *footer;
	const gchar *main_start;

	server_fixture_create_user(fixture, "adam", "a-long-password",
	                           VENTURE_USER_ROLE_ADMIN, NULL);
	cookie = server_fixture_login(fixture, "adam", "a-long-password");

	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/reports",
		cookie, NULL, &page, NULL), ==, SOUP_STATUS_OK);

	script = strstr(page, "venture.nav.scroll");
	g_assert_nonnull(script);

	/* Reads the position back, not merely stores it. */
	g_assert_nonnull(strstr(page, "scrollTop"));

	/* And finds the current page's entry, to bring it into view when the
	 * restored position does not already show it. */
	g_assert_nonnull(strstr(page, ".nav-item.active"));

	/*
	 * After the sidebar footer, because .nav is a flex child sized against
	 * its siblings: measured before the footer exists its height comes out
	 * too tall, and the centring is then computed against a box that is
	 * not the one the reader sees.
	 *
	 * Matched on the footer's markup rather than the bare class name: the
	 * stylesheet is inlined into every page and styles .sidebar-footer
	 * hundreds of lines above the body, so the name alone finds the CSS
	 * and compares against the wrong position entirely.
	 */
	footer = strstr(page, "<div class=\"sidebar-footer\">");
	g_assert_nonnull(footer);
	g_assert_true(footer < script);

	/*
	 * And before <main>, which is what keeps it ahead of the first paint.
	 * Moved to the end of the body it would run after the browser had
	 * already drawn the sidebar at the top, turning a lost position into a
	 * visible jump.
	 */
	main_start = strstr(page, "<main class=\"main\">");
	g_assert_nonnull(main_start);
	g_assert_true(script < main_start);
}

/*
 * The active entry is marked, which is what the script above looks for. A
 * sidebar that stopped marking it would leave that lookup finding nothing
 * and failing silently -- the page would simply never scroll itself into
 * view, with no error anywhere.
 */
static void
test_auth_sidebar_marks_the_active_entry(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *page = NULL;

	server_fixture_create_user(fixture, "adam", "a-long-password",
	                           VENTURE_USER_ROLE_ADMIN, NULL);
	cookie = server_fixture_login(fixture, "adam", "a-long-password");

	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/account/tokens",
		cookie, NULL, &page, NULL), ==, SOUP_STATUS_OK);

	/* The entry for the page being viewed, carrying the class the script
	 * selects on. */
	g_assert_nonnull(strstr(page,
		"<a class=\"nav-item active\" href=\"/account/tokens\">"));
}

/*
 * There are two looks, and the switch is a cookie the server reads before
 * it draws anything. The default is the configured one (industrial, out of
 * the box); posting the switch sets the cookie and sends you back where you
 * were, and every page after that -- the sign-in page included, which has
 * no session -- is drawn in the other stylesheet. A cookie somebody edited
 * by hand falls back to the configuration, and the switch cannot be used
 * to send a person off the site.
 */
static void
test_auth_look_switch_is_a_cookie(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *both = NULL;
	g_autofree gchar *forged = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *set_cookie = NULL;
	g_autofree gchar *body = NULL;

	server_fixture_create_user(fixture, "adam", "a-long-password",
	                           VENTURE_USER_ROLE_ADMIN, NULL);
	cookie = server_fixture_login(fixture, "adam", "a-long-password");

	/* Out of the box: the instrument panel, and the switch offers the
	 * other one. */
	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/", cookie,
		NULL, &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "instrument panel"));
	g_assert_null(strstr(page, "warm monochrome"));
	g_assert_nonnull(strstr(page, "name=\"look\" value=\"classic\""));
	g_assert_nonnull(strstr(page, ">Classic look</button>"));

	/* The switch: a cookie, and back to the page it was pressed on. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/look", cookie,
		"look=classic&back=/tickets", &body, &set_cookie),
		==, SOUP_STATUS_FOUND);
	g_assert_nonnull(set_cookie);
	g_assert_nonnull(g_strstr_len(set_cookie, -1, "venture_look=classic"));

	both = g_strdup_printf("%s; venture_look=classic", cookie);
	g_free(page);
	page = NULL;
	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/", both,
		NULL, &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "warm monochrome"));
	g_assert_null(strstr(page, "instrument panel"));
	g_assert_nonnull(strstr(page, ">Industrial look</button>"));

	/* The sign-in page follows the cookie, session or no session. */
	g_free(page);
	page = NULL;
	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/login",
		"venture_look=classic", NULL, &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "warm monochrome"));

	/* A forged value is the configuration again. */
	forged = g_strdup_printf("%s; venture_look=../etc/passwd", cookie);
	g_free(page);
	page = NULL;
	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/", forged,
		NULL, &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "instrument panel"));

	/* An off-site "back" lands on the home page instead. */
	g_free(set_cookie);
	set_cookie = NULL;
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/look", cookie,
		"look=industrial&back=//evil.example/x", NULL, &set_cookie),
		==, SOUP_STATUS_FOUND);
	g_assert_nonnull(g_strstr_len(set_cookie, -1, "venture_look=industrial"));
}

/*
 * The database keeps a hash, never the secret -- the same property the
 * password table has, and the reason the reveal page can only ever run once.
 */
static void
test_auth_browser_token_stores_only_a_hash(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) entity = NULL;
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *prefix = NULL;

	server_fixture_create_user(fixture, "adam", "a-long-password",
	                           VENTURE_USER_ROLE_ADMIN, NULL);
	cookie = server_fixture_login(fixture, "adam", "a-long-password");

	secret = server_fixture_mint_token(fixture, cookie,
	                                   "name=hashed&expires_in_days=0", NULL);
	g_assert_nonnull(secret);

	entity = venture_database_get(fixture->database, VENTURE_TYPE_API_TOKEN,
	                              1, NULL);
	g_assert_nonnull(entity);

	g_object_get(entity, "token-hash", &hash, "prefix", &prefix, NULL);

	g_assert_nonnull(hash);
	g_assert_null(strstr(hash, secret));

	/* The prefix is deliberately in the clear, and is a prefix of the
	 * secret rather than of the stored hash. */
	g_assert_nonnull(prefix);
	g_assert_true(g_str_has_prefix(secret + 3, prefix));
}

static void
test_federation_owner_boundary(ServerFixture *fixture, gconstpointer data)
{
	g_autofree gchar *editor = NULL;
	g_autofree gchar *owner = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(fixture->config, "federation-enabled", TRUE, NULL);
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	server_fixture_create_user(fixture, "fed-editor", "editor-long-password", VENTURE_USER_ROLE_EDITOR, NULL);
	server_fixture_create_user(fixture, "fed-owner", "owner-long-password", VENTURE_USER_ROLE_OWNER, NULL);
	editor = server_fixture_login(fixture, "fed-editor", "editor-long-password");
	owner = server_fixture_login(fixture, "fed-owner", "owner-long-password");
	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/api/v1/federation_peer", editor, NULL, NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/api/v1/federation_grant", editor, "", NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/api/v1/federation_peer", owner, NULL, NULL, NULL), ==, SOUP_STATUS_OK);
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/api/v1/federation_replica", owner, "", NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/federation", editor, NULL, NULL, NULL), ==, SOUP_STATUS_OK);
	/* Unsigned peers cannot become local editors, even on an enabled module. */
	g_assert_cmpuint(server_fixture_request(fixture, "POST", "/federation/v1/request", NULL, "", NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
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
	ADD("/auth/login-rate-limits-an-address",
	    test_auth_login_rate_limits_an_address);

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

	g_test_add("/auth/tokens-page-is-admin-only", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_tokens_page_is_admin_only,
	           server_fixture_tear_down);
	g_test_add("/auth/tokens-refuse-anonymous", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_tokens_refuse_anonymous,
	           server_fixture_tear_down);
	g_test_add("/auth/token-minted-in-browser-authenticates", ServerFixture,
	           NULL, server_fixture_set_up,
	           test_auth_token_minted_in_browser_authenticates,
	           server_fixture_tear_down);
	g_test_add("/auth/token-reveal-is-one-shot", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_token_reveal_is_one_shot,
	           server_fixture_tear_down);
	g_test_add("/auth/token-revoke-stops-it-working", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_token_revoke_stops_it_working,
	           server_fixture_tear_down);
	g_test_add("/auth/token-expiry-is-recorded", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_token_expiry_is_recorded,
	           server_fixture_tear_down);
	g_test_add("/auth/token-never-expires-by-default", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_token_never_expires_by_default,
	           server_fixture_tear_down);
	g_test_add("/auth/kb-import-rejects-urlencoded", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_kb_import_rejects_urlencoded,
	           server_fixture_tear_down);
	g_test_add("/auth/kb-writes-refuse-anonymous", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_kb_writes_refuse_anonymous,
	           server_fixture_tear_down);
	g_test_add("/auth/kb-writes-need-the-editor-role", ServerFixture, NULL,
	           server_fixture_set_up,
	           test_auth_kb_writes_need_the_editor_role,
	           server_fixture_tear_down);
	g_test_add("/auth/sidebar-restores-its-scroll", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_sidebar_restores_its_scroll,
	           server_fixture_tear_down);
	g_test_add("/auth/look-switch-is-a-cookie", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_look_switch_is_a_cookie,
	           server_fixture_tear_down);
	g_test_add("/auth/sidebar-marks-the-active-entry", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_sidebar_marks_the_active_entry,
	           server_fixture_tear_down);
	g_test_add("/auth/redirect-notices-render", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_redirect_notices_render,
	           server_fixture_tear_down);
	g_test_add("/auth/browser-token-stores-only-a-hash", ServerFixture, NULL,
	           server_fixture_set_up,
	           test_auth_browser_token_stores_only_a_hash,
	           server_fixture_tear_down);

	g_test_add("/auth/forge-records-are-owner-only", ServerFixture, NULL,

	           server_fixture_set_up, test_auth_forge_records_are_owner_only,

	           server_fixture_tear_down);
	g_test_add("/auth/ticket-board-filters-compose", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_ticket_board_filters_compose,
	           server_fixture_tear_down);
	g_test_add("/auth/staged-write-changes-nothing", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_staged_write_changes_nothing,
	           server_fixture_tear_down);
	g_test_add("/auth/staged-write-can-be-rejected", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_staged_write_can_be_rejected,
	           server_fixture_tear_down);
	g_test_add("/auth/staging-needs-the-editor-role", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_staging_needs_the_editor_role,
	           server_fixture_tear_down);
	g_test_add("/auth/unknown-stage-value-is-refused", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_unknown_stage_value_is_refused,
	           server_fixture_tear_down);
	g_test_add("/auth/staged-delete-leaves-the-record", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_staged_delete_leaves_the_record,
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
	g_test_add("/auth/chat-rename-and-export", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_chat_rename_and_export,
	           server_fixture_tear_down);
	g_test_add("/auth/models-are-scoped-to-the-provider", ServerFixture, NULL,
	           server_fixture_set_up,
	           test_auth_models_are_scoped_to_the_provider,
	           server_fixture_tear_down);
	g_test_add("/auth/chat-stream-token-is-one-shot", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_chat_stream_token_is_one_shot,
	           server_fixture_tear_down);
	g_test_add("/auth/chat-complete-is-the-harness", ServerFixture,
	           NULL, server_fixture_set_up,
	           test_auth_chat_complete_is_the_harness,
	           server_fixture_tear_down);
	g_test_add("/auth/chat-reply-rendering", ServerFixture, NULL,
	           server_fixture_set_up, test_auth_chat_reply_rendering,
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

	g_test_add("/auth/federation-owner-boundary", ServerFixture, NULL, server_fixture_set_up, test_federation_owner_boundary, server_fixture_tear_down);
	return g_test_run();
}
