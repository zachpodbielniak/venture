/*
 * test-mfa.c - The second factor: TOTP enrolment, verification, recovery
 * codes, the require-MFA organization setting and the break-glass reset
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every clock-dependent check runs against the service's fixed-time
 * property, so drift, replay and the failure window are asserted at exact
 * instants rather than whatever the wall clock happens to say.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

#define KEY_VARIABLE "VENTURE_TEST_MFA_KEY"
#define SESSION_VARIABLE "VENTURE_TEST_MFA_SESSION_SECRET"

typedef struct
{
	VentureDatabase		*database;
	VentureConfig		*config;
	VentureContext		*context;
	VentureAuth		*auth;
	VentureMfaService	*service;
	VentureWebServer	*server;
	SoupSession		*session;
	guint16			 port;
	gchar			*state_dir;
	gint64			 org;
} Fixture;

static void
fixture_set_up(Fixture *fixture, gconstpointer user_data)
{
	g_autoptr(GError) error = NULL;
	gboolean with_server = GPOINTER_TO_INT(user_data);

	g_setenv(SESSION_VARIABLE, "test-session-secret", TRUE);
	g_setenv(KEY_VARIABLE, "test-mfa-key", TRUE);

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "security-session-secret-env", SESSION_VARIABLE,
	             "security-mfa-key-env", KEY_VARIABLE,
	             "security-password-iterations", (gint64)100000,
	             "security-login-rate-limit", (gint64)0,
	             NULL);
	if (with_server)
	{
		fixture->state_dir = g_dir_make_tmp("venture-mfa-XXXXXX", NULL);
		fixture->port = (guint16)(20000 + ((getpid() + 7) % 20000));
		g_object_set(fixture->config,
		             "state-dir", fixture->state_dir,
		             "server-bind-address", "127.0.0.1",
		             "server-port", (gint64)fixture->port,
		             "security-require-auth", TRUE,
		             NULL);
	}

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->auth = venture_auth_new(fixture->context);
	fixture->service = venture_mfa_service_get(fixture->database);
	fixture->org = venture_context_get_default_organization_id(fixture->context);

	if (with_server)
	{
		fixture->server = venture_web_server_new(fixture->context, &error);
		g_assert_no_error(error);
		g_assert_true(venture_web_server_start(fixture->server, &error));
		g_assert_no_error(error);
		fixture->session = soup_session_new();
	}
}

static void
fixture_tear_down(Fixture *fixture, gconstpointer user_data)
{
	(void)user_data;
	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->auth);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}
	g_unsetenv(KEY_VARIABLE);
	g_unsetenv(SESSION_VARIABLE);
}

/* --- Helpers --------------------------------------------------------------- */

static gint64
create_user(Fixture *fixture, const gchar *username, VentureUserRole role, gint org_role)
{
	g_autoptr(VentureUser) user = venture_user_new();
	g_object_set(user, "username", username, "role", role, "active", TRUE, "organization-id", fixture->org, NULL);
	g_assert_true(venture_user_set_password(user, "correct horse battery", 100000, NULL));
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(user), NULL, NULL));
	if (org_role >= 0)
	{
		g_autoptr(VentureEntity) member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
			"organization-id", fixture->org, "user-id", venture_entity_get_id(VENTURE_ENTITY(user)),
			"role", org_role, "active", TRUE, NULL);
		g_assert_true(venture_database_save(fixture->database, member, NULL, NULL));
	}
	return venture_entity_get_id(VENTURE_ENTITY(user));
}

static GDateTime *
at(const gchar *text)
{
	GDateTime *when = venture_time_from_string(text, NULL);
	g_assert_nonnull(when);
	return when;
}

static void
set_clock(Fixture *fixture, const gchar *text)
{
	g_autoptr(GDateTime) when = at(text);
	g_object_set(fixture->service, "fixed-time", when, NULL);
}

/* The code an authenticator app would show for @secret at the service's
 * clock, offset by @steps periods. */
static gchar *
app_code(Fixture *fixture, const gchar *base32, gint steps)
{
	g_autoptr(GDateTime) now = venture_mfa_service_now(fixture->service);
	g_autofree guchar *secret = NULL;
	gsize length = 0;
	secret = venture_base32_decode(base32, &length);
	g_assert_nonnull(secret);
	return venture_totp_code(secret, length, (guint64)((gint64)venture_totp_counter(now) + steps), VENTURE_TOTP_DIGITS);
}

static VentureActor
actor_named(const gchar *name)
{
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = name;
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	return actor;
}

/* Enrols @user_id at the clock's current instant and returns the base32
 * secret; the recovery codes go to @out_codes when wanted. */
static gchar *
enrol(Fixture *fixture, gint64 user_id, GStrv *out_codes)
{
	g_autoptr(VentureMfaEnrolment) enrolment = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *code = NULL;
	GStrv codes;
	VentureActor actor = actor_named("alice");
	enrolment = venture_mfa_service_begin_enrolment(fixture->service, user_id, "Books", "alice", &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(enrolment);
	code = app_code(fixture, enrolment->secret, 0);
	codes = venture_mfa_service_confirm_enrolment(fixture->service, user_id, code, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(codes);
	if (NULL != out_codes)
		*out_codes = codes;
	else
		g_strfreev(codes);
	return g_strdup(enrolment->secret);
}

static guint
count_audit(Fixture *fixture, const gchar *event)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *needle = g_strdup_printf("\"event\":\"%s\"", event);
	guint i, n = 0;
	venture_query_add_filter_string(query, "source", VENTURE_FILTER_OP_EQ, "mfa", NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(fixture->database, query, NULL);
	for (i = 0; NULL != rows && i < rows->len; i++)
	{
		g_autofree gchar *diff = NULL;
		g_object_get(g_ptr_array_index(rows, i), "diff", &diff, NULL);
		if (NULL != diff && NULL != strstr(diff, needle))
			n++;
	}
	return n;
}

static GPtrArray *
recovery_rows(Fixture *fixture, gint64 user_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MFA_RECOVERY_CODE);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user_id, NULL);
	venture_query_set_limit(query, 0);
	return venture_database_find(fixture->database, query, NULL);
}

/* --- Records and module ------------------------------------------------------ */

static void
test_records_registered(Fixture *fixture, gconstpointer user_data)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	VentureModule *module;
	const gchar *const *requires;
	(void)fixture;
	(void)user_data;
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "user_mfa"), ==, VENTURE_TYPE_USER_MFA);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "mfa_recovery_code"), ==, VENTURE_TYPE_MFA_RECOVERY_CODE);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "mfa_policy"), ==, VENTURE_TYPE_MFA_POLICY);
	module = venture_module_registry_lookup(venture_context_get_modules(fixture->context), "mfa");
	g_assert_nonnull(module);
	requires = venture_module_get_requires(module);
	g_assert_cmpstr(requires[0], ==, "orgaccess");
	/* The secret and the hash carry the sensitive flag; that is what keeps
	 * them out of every generated surface. */
	g_assert_true(venture_entity_class_get_column_flags(g_type_class_ref(VENTURE_TYPE_USER_MFA), "secret-ref") & VENTURE_COLUMN_FLAG_SENSITIVE);
	g_assert_true(venture_entity_class_get_column_flags(g_type_class_ref(VENTURE_TYPE_MFA_RECOVERY_CODE), "code-hash") & VENTURE_COLUMN_FLAG_SENSITIVE);
	g_assert_true(venture_entity_class_get_column_flags(g_type_class_ref(VENTURE_TYPE_USER_MFA), "user-id") & VENTURE_COLUMN_FLAG_PERSONAL_OWNER);
}

/* Generic writes must go through the service; a second policy row per
 * organization is refused. */
static void
test_generic_writes_refused(Fixture *fixture, gconstpointer user_data)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureEntity) code = NULL;
	g_autoptr(VentureEntity) policy = NULL;
	g_autoptr(VentureEntity) duplicate = NULL;
	g_autoptr(GError) error = NULL;
	gint64 user = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	(void)user_data;
	row = g_object_new(VENTURE_TYPE_USER_MFA, "organization-id", fixture->org, "user-id", user, "enabled", TRUE, "secret-ref", "plain", NULL);
	g_assert_false(venture_database_save(fixture->database, row, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "VentureMfaService"));
	g_clear_error(&error);
	code = g_object_new(VENTURE_TYPE_MFA_RECOVERY_CODE, "organization-id", fixture->org, "user-id", user, "prefix", "abcd", "code-hash", "x", NULL);
	g_assert_false(venture_database_save(fixture->database, code, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	policy = g_object_new(VENTURE_TYPE_MFA_POLICY, "organization-id", fixture->org, "require-mfa-for-admins", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, policy, NULL, &error));
	g_assert_no_error(error);
	duplicate = g_object_new(VENTURE_TYPE_MFA_POLICY, "organization-id", fixture->org, "require-mfa-for-admins", FALSE, NULL);
	g_assert_false(venture_database_save(fixture->database, duplicate, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
}

/* --- TOTP, base32 and the QR code ------------------------------------------------ */

/* RFC 6238 appendix B, SHA-1 column, against the 20-byte ASCII secret. */
static void
test_totp_vectors(Fixture *fixture, gconstpointer user_data)
{
	static const struct { gint64 seconds; const gchar *code; } vectors[] = {
		{ 59, "94287082" }, { 1111111109, "07081804" }, { 1111111111, "14050471" },
		{ 1234567890, "89005924" }, { 2000000000, "69279037" }, { 20000000000, "65353130" }
	};
	const guchar *secret = (const guchar *)"12345678901234567890";
	g_autofree gchar *encoded = NULL;
	g_autofree guchar *decoded = NULL;
	g_autofree gchar *uri = NULL;
	gsize length = 0;
	guint i;
	(void)fixture;
	(void)user_data;
	for (i = 0; i < G_N_ELEMENTS(vectors); i++)
	{
		g_autofree gchar *code = venture_totp_code(secret, 20, (guint64)(vectors[i].seconds / 30), 8);
		g_assert_cmpstr(code, ==, vectors[i].code);
	}
	encoded = venture_base32_encode(secret, 20);
	g_assert_cmpstr(encoded, ==, "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ");
	decoded = venture_base32_decode("gezd gnbv gy3t qojq gezd gnbv gy3t qojq", &length);
	g_assert_cmpuint(length, ==, 20);
	g_assert_cmpint(memcmp(decoded, secret, 20), ==, 0);
	g_assert_null(venture_base32_decode("not!base32", &length));
	uri = venture_totp_uri("My Books", "alice@example.test", encoded);
	g_assert_cmpstr(uri, ==, "otpauth://totp/My%20Books:alice%40example.test?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=My%20Books&algorithm=SHA1&digits=6&period=30");
}

/* Drift: a step either side verifies; two steps away does not. */
static void
test_totp_drift_window(Fixture *fixture, gconstpointer user_data)
{
	const guchar *secret = (const guchar *)"12345678901234567890";
	g_autoptr(GDateTime) now = at("2026-03-01T12:00:00Z");
	guint64 centre = venture_totp_counter(now);
	guint64 matched = 0;
	gint delta;
	(void)fixture;
	(void)user_data;
	for (delta = -2; delta <= 2; delta++)
	{
		g_autofree gchar *code = venture_totp_code(secret, 20, (guint64)((gint64)centre + delta), 6);
		gboolean ok = venture_totp_verify(secret, 20, code, now, 1, &matched);
		if (ABS(delta) <= 1)
		{
			g_assert_true(ok);
			g_assert_cmpuint(matched, ==, (guint64)((gint64)centre + delta));
		}
		else
			g_assert_false(ok);
	}
	g_assert_false(venture_totp_verify(secret, 20, "12345", now, 1, NULL));
	g_assert_false(venture_totp_verify(secret, 20, "abcdef", now, 1, NULL));
	g_assert_false(venture_totp_verify(secret, 20, NULL, now, 1, NULL));
}

static void
test_qr_svg(Fixture *fixture, gconstpointer user_data)
{
	g_autofree guint8 *matrix = NULL;
	g_autofree gchar *svg = NULL;
	g_autofree gchar *large = NULL;
	g_autofree gchar *too_long = g_strnfill(300, 'a');
	g_autoptr(GError) error = NULL;
	guint size = 0;
	(void)fixture;
	(void)user_data;
	matrix = venture_qr_matrix("HELLO", &size, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(size, ==, 21);
	/* The three finder patterns: dark corners, light ring, dark centre. */
	g_assert_cmpuint(matrix[0], ==, 1);
	g_assert_cmpuint(matrix[1 * 21 + 1], ==, 0);
	g_assert_cmpuint(matrix[3 * 21 + 3], ==, 1);
	g_assert_cmpuint(matrix[0 * 21 + 20], ==, 1);
	g_assert_cmpuint(matrix[20 * 21 + 0], ==, 1);
	/* The separator ring and the always-dark module. */
	g_assert_cmpuint(matrix[7 * 21 + 7], ==, 0);
	g_assert_cmpuint(matrix[(21 - 8) * 21 + 8], ==, 1);
	svg = venture_qr_svg_render("otpauth://totp/Books:alice?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=Books&algorithm=SHA1&digits=6&period=30", 0, &error);
	g_assert_no_error(error);
	g_assert_true(g_str_has_prefix(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\""));
	g_assert_nonnull(strstr(svg, "<path fill=\"#000000\""));
	/* Self-contained: no image reference, no script, no external fetch. */
	g_assert_null(strstr(svg, "<image"));
	g_assert_null(strstr(svg, "href="));
	g_free(matrix);
	/* 169 bytes: past version 8's 152, within version 9's 180 at level M. */
	matrix = venture_qr_matrix("otpauth://totp/A%20Long%20Install%20Name:some.person@example.test?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=A%20Long%20Install%20Name&algorithm=SHA1&digits=6&period=30", &size, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(size, ==, 17 + 4 * 9);
	/* Version 9 carries an alignment pattern centred at (46, 46). */
	g_assert_cmpuint(matrix[46 * size + 46], ==, 1);
	g_assert_cmpuint(matrix[45 * size + 46], ==, 0);
	g_assert_cmpuint(matrix[44 * size + 46], ==, 1);
	large = venture_qr_svg_render(too_long, 0, &error);
	g_assert_null(large);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* --- Enrolment ------------------------------------------------------------------------ */

static void
test_enrolment(Fixture *fixture, gconstpointer user_data)
{
	g_autoptr(VentureMfaEnrolment) enrolment = NULL;
	g_autoptr(VentureMfaEnrolment) again = NULL;
	g_autoptr(VentureUserMfa) row = NULL;
	g_autoptr(GPtrArray) codes_rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *stored = NULL;
	g_autofree gchar *wrong = NULL;
	g_autofree gchar *right = NULL;
	g_autofree gchar *json = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_auto(GStrv) codes = NULL;
	g_autoptr(GDateTime) enrolled = NULL;
	VentureActor actor = actor_named("alice");
	gint64 user = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	gboolean enabled = TRUE;
	guint i;
	(void)user_data;
	set_clock(fixture, "2026-02-01T09:00:00Z");
	g_assert_false(venture_mfa_service_is_enabled(fixture->service, user));

	enrolment = venture_mfa_service_begin_enrolment(fixture->service, user, "Books", "alice", &actor, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(strlen(enrolment->secret), ==, 32);
	g_assert_true(g_str_has_prefix(enrolment->uri, "otpauth://totp/Books:alice?secret="));
	g_assert_nonnull(strstr(enrolment->uri, enrolment->secret));
	g_assert_true(g_str_has_prefix(enrolment->svg, "<svg"));

	/* Pending, not enabled; the stored form is ciphertext, not the secret. */
	row = venture_mfa_service_get_row(fixture->service, user);
	g_assert_nonnull(row);
	g_object_get(row, "enabled", &enabled, "secret-ref", &stored, NULL);
	g_assert_false(enabled);
	g_assert_true(g_str_has_prefix(stored, "gcm1$"));
	g_assert_null(strstr(stored, enrolment->secret));
	g_assert_false(venture_mfa_service_is_enabled(fixture->service, user));

	/* The generated JSON never carries the secret. */
	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(row), FALSE);
	json = venture_json_to_string(node, FALSE);
	g_assert_null(strstr(json, stored));
	g_assert_null(strstr(json, "secret_ref"));

	/* Refreshing the page re-renders the same pending secret. */
	again = venture_mfa_service_pending_enrolment(fixture->service, user, "Books", "alice", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(again->secret, ==, enrolment->secret);

	/* A wrong code does not enable anything. */
	wrong = app_code(fixture, enrolment->secret, 5);
	codes = venture_mfa_service_confirm_enrolment(fixture->service, user, wrong, &actor, &error);
	g_assert_null(codes);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_clear_error(&error);
	g_assert_false(venture_mfa_service_is_enabled(fixture->service, user));
	g_assert_cmpuint(count_audit(fixture, "mfa_enrolment_failed"), ==, 1);

	/* The right code enables it, once, with ten recovery codes shown once
	 * and stored only as hashes. */
	right = app_code(fixture, enrolment->secret, 0);
	codes = venture_mfa_service_confirm_enrolment(fixture->service, user, right, &actor, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_strv_length(codes), ==, 10);
	g_assert_true(venture_mfa_service_is_enabled(fixture->service, user));
	g_clear_object(&row);
	row = venture_mfa_service_get_row(fixture->service, user);
	g_object_get(row, "enrolled-at", &enrolled, NULL);
	g_assert_nonnull(enrolled);
	g_assert_cmpint(g_date_time_to_unix(enrolled), ==, 1769936400);
	g_assert_cmpuint(count_audit(fixture, "mfa_enrolled"), ==, 1);
	codes_rows = recovery_rows(fixture, user);
	g_assert_cmpuint(codes_rows->len, ==, 10);
	for (i = 0; i < codes_rows->len; i++)
	{
		g_autofree gchar *hash = NULL;
		g_autofree gchar *prefix = NULL;
		guint j;
		g_object_get(g_ptr_array_index(codes_rows, i), "code-hash", &hash, "prefix", &prefix, NULL);
		g_assert_true(g_str_has_prefix(hash, "pbkdf2-sha256$"));
		g_assert_cmpuint(strlen(prefix), ==, 4);
		for (j = 0; j < 10; j++)
			g_assert_null(strstr(hash, codes[j]));
	}
	for (i = 0; i < 10; i++)
		g_assert_cmpuint(strlen(codes[i]), ==, 11);

	/* Enrolling again while enabled is refused; a second confirm has
	 * nothing pending. */
	g_clear_pointer(&again, venture_mfa_enrolment_free);
	again = venture_mfa_service_begin_enrolment(fixture->service, user, "Books", "alice", &actor, &error);
	g_assert_null(again);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_strfreev(g_steal_pointer(&codes));
	codes = venture_mfa_service_confirm_enrolment(fixture->service, user, right, &actor, &error);
	g_assert_null(codes);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/* Without a key from the environment, enrolment refuses rather than
 * storing a secret in the clear. */
static void
test_enrolment_needs_key(Fixture *fixture, gconstpointer user_data)
{
	g_autoptr(VentureMfaEnrolment) enrolment = NULL;
	g_autoptr(GError) error = NULL;
	gint64 user = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	(void)user_data;
	g_unsetenv(KEY_VARIABLE);
	g_unsetenv(SESSION_VARIABLE);
	venture_mfa_service_configure(fixture->service, fixture->config);
	enrolment = venture_mfa_service_begin_enrolment(fixture->service, user, "Books", "alice", NULL, &error);
	g_assert_null(enrolment);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "security.mfa_key_env"));
	g_assert_null(venture_mfa_service_get_row(fixture->service, user));
}

/* --- Verification at login ---------------------------------------------------------------- */

static void
test_login_requires_code(Fixture *fixture, gconstpointer user_data)
{
	g_autofree gchar *secret = NULL;
	g_autofree gchar *cookie = NULL;
	g_autoptr(GError) error = NULL;
	gboolean pending = TRUE;
	gint64 user = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	(void)user_data;
	set_clock(fixture, "2026-02-01T09:00:00Z");

	/* Before enrolment the password alone opens a session. */
	g_assert_true(venture_auth_login_with_mfa(fixture->auth, "alice", "correct horse battery", NULL, &cookie, &pending, &error));
	g_assert_no_error(error);
	g_assert_false(pending);
	g_assert_true(g_str_has_prefix(cookie, "venture_session="));
	g_clear_pointer(&cookie, g_free);

	secret = enrol(fixture, user, NULL);

	/* After it, the password-only path refuses outright... */
	g_assert_false(venture_auth_login(fixture->auth, "alice", "correct horse battery", NULL, &cookie, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_assert_null(cookie);
	g_clear_error(&error);

	/* ...and the browser path hands out a challenge, not a session. */
	g_assert_true(venture_auth_login_with_mfa(fixture->auth, "alice", "correct horse battery", NULL, &cookie, &pending, &error));
	g_assert_no_error(error);
	g_assert_true(pending);
	g_assert_true(g_str_has_prefix(cookie, "venture_mfa="));
	g_assert_null(strstr(cookie, "venture_session="));

	/* The wrong password is still just the wrong password. */
	g_clear_pointer(&cookie, g_free);
	g_assert_false(venture_auth_login_with_mfa(fixture->auth, "alice", "nope", NULL, &cookie, &pending, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
}

static void
test_verify_drift_and_replay(Fixture *fixture, gconstpointer user_data)
{
	g_autofree gchar *secret = NULL;
	g_autofree gchar *code = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor = actor_named("alice");
	gint64 user = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	(void)user_data;
	set_clock(fixture, "2026-02-01T09:00:00Z");
	secret = enrol(fixture, user, NULL);

	/* Ten steps later: one step of drift either side is accepted. */
	set_clock(fixture, "2026-02-01T09:05:00Z");
	code = app_code(fixture, secret, -1);
	g_assert_true(venture_mfa_service_verify(fixture->service, user, code, &actor, "203.0.113.9", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(count_audit(fixture, "mfa_success"), ==, 1);

	/* The same step again is a replay, however fresh the clock. */
	g_assert_false(venture_mfa_service_verify(fixture->service, user, code, &actor, "203.0.113.9", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_clear_error(&error);
	g_assert_cmpuint(count_audit(fixture, "mfa_replay_refused"), ==, 1);

	/* A later step is fine; two steps ahead is outside the window. */
	g_clear_pointer(&code, g_free);
	code = app_code(fixture, secret, 1);
	g_assert_true(venture_mfa_service_verify(fixture->service, user, code, &actor, NULL, &error));
	g_assert_no_error(error);
	g_clear_pointer(&code, g_free);
	code = app_code(fixture, secret, 3);
	g_assert_false(venture_mfa_service_verify(fixture->service, user, code, &actor, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_clear_error(&error);
	g_assert_cmpuint(count_audit(fixture, "mfa_failure"), ==, 1);

	/* Spaces as the app displays them are fine. */
	set_clock(fixture, "2026-02-01T09:10:00Z");
	g_clear_pointer(&code, g_free);
	code = app_code(fixture, secret, 0);
	{
		g_autofree gchar *spaced = g_strdup_printf("%.3s %s", code, code + 3);
		g_assert_true(venture_mfa_service_verify(fixture->service, user, spaced, &actor, NULL, &error));
		g_assert_no_error(error);
	}
}

static void
test_verify_rate_limit(Fixture *fixture, gconstpointer user_data)
{
	g_autofree gchar *secret = NULL;
	g_autofree gchar *right = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor = actor_named("alice");
	gint64 user = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	guint i;
	(void)user_data;
	set_clock(fixture, "2026-02-01T09:00:00Z");
	secret = enrol(fixture, user, NULL);
	set_clock(fixture, "2026-02-01T10:00:00Z");
	for (i = 0; i < VENTURE_MFA_FAILURE_LIMIT; i++)
	{
		g_assert_false(venture_mfa_service_verify(fixture->service, user, "000000", &actor, NULL, &error));
		g_clear_error(&error);
	}
	g_assert_cmpuint(count_audit(fixture, "mfa_failure"), ==, VENTURE_MFA_FAILURE_LIMIT);

	/* The sixth attempt is refused before the code is even looked at. */
	set_clock(fixture, "2026-02-01T10:14:00Z");
	right = app_code(fixture, secret, 0);
	g_assert_false(venture_mfa_service_verify(fixture->service, user, right, &actor, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_assert_nonnull(strstr(error->message, "Too many"));
	g_clear_error(&error);
	g_assert_cmpuint(count_audit(fixture, "mfa_rate_limited"), ==, 1);

	/* Once the window has passed the same kind of code is accepted, and
	 * the count starts over. */
	set_clock(fixture, "2026-02-01T10:16:00Z");
	g_clear_pointer(&right, g_free);
	right = app_code(fixture, secret, 0);
	g_assert_true(venture_mfa_service_verify(fixture->service, user, right, &actor, NULL, &error));
	g_assert_no_error(error);
}

static void
test_recovery_codes_single_use(Fixture *fixture, gconstpointer user_data)
{
	g_autofree gchar *secret = NULL;
	g_auto(GStrv) codes = NULL;
	g_auto(GStrv) fresh = NULL;
	g_autofree gchar *shouted = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	VentureActor actor = actor_named("alice");
	gint64 user = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	guint i, unused = 0;
	(void)user_data;
	set_clock(fixture, "2026-02-01T09:00:00Z");
	secret = enrol(fixture, user, &codes);
	set_clock(fixture, "2026-02-01T11:00:00Z");

	/* Upper case and without the dash, because that is how it gets typed. */
	shouted = g_ascii_strup(codes[3], -1);
	g_assert_true(venture_mfa_service_verify(fixture->service, user, shouted, &actor, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(count_audit(fixture, "mfa_recovery_success"), ==, 1);

	/* Spent: the same code never works again. */
	g_assert_false(venture_mfa_service_verify(fixture->service, user, codes[3], &actor, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_clear_error(&error);
	g_assert_cmpuint(count_audit(fixture, "mfa_failure"), ==, 1);

	/* A made-up code is refused. */
	g_assert_false(venture_mfa_service_verify(fixture->service, user, "aaaaa-aaaaa", &actor, NULL, &error));
	g_clear_error(&error);

	/* Another code still works. */
	g_assert_true(venture_mfa_service_verify(fixture->service, user, codes[7], &actor, NULL, &error));
	g_assert_no_error(error);

	/* Regeneration spends the rest and issues ten new ones. */
	fresh = venture_mfa_service_regenerate_recovery_codes(fixture->service, user, &actor, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_strv_length(fresh), ==, 10);
	g_assert_false(venture_mfa_service_verify(fixture->service, user, codes[0], &actor, NULL, &error));
	g_clear_error(&error);
	rows = recovery_rows(fixture, user);
	g_assert_cmpuint(rows->len, ==, 20);
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(GDateTime) used = NULL;
		g_object_get(g_ptr_array_index(rows, i), "used-at", &used, NULL);
		if (NULL == used)
			unused++;
	}
	g_assert_cmpuint(unused, ==, 10);
	g_assert_true(venture_mfa_service_verify(fixture->service, user, fresh[0], &actor, NULL, &error));
	g_assert_no_error(error);
}

/* Every audit row the module writes is attributed and redacted. */
static void
test_audit_rows_never_carry_secrets(Fixture *fixture, gconstpointer user_data)
{
	g_autofree gchar *secret = NULL;
	g_autofree gchar *stored = NULL;
	g_autoptr(VentureUserMfa) row = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor = actor_named("alice");
	gint64 user = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	guint i, mfa_rows = 0;
	(void)user_data;
	set_clock(fixture, "2026-02-01T09:00:00Z");
	secret = enrol(fixture, user, NULL);
	row = venture_mfa_service_get_row(fixture->service, user);
	g_object_get(row, "secret-ref", &stored, NULL);
	g_assert_false(venture_mfa_service_verify(fixture->service, user, "000000", &actor, "198.51.100.4", &error));
	g_clear_error(&error);
	g_assert_true(venture_mfa_service_disable(fixture->service, user, &actor, &error));
	g_assert_no_error(error);
	venture_query_set_limit(query, 0);
	entries = venture_database_find(fixture->database, query, NULL);
	for (i = 0; i < entries->len; i++)
	{
		VentureEntity *entry = g_ptr_array_index(entries, i);
		g_autofree gchar *diff = NULL;
		g_autofree gchar *source = NULL;
		g_autofree gchar *who = NULL;
		g_autofree gchar *json = NULL;
		g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(entry), FALSE);
		json = venture_json_to_string(node, FALSE);
		g_object_get(entry, "diff", &diff, "source", &source, "actor", &who, NULL);
		g_assert_null(strstr(json, stored));
		g_assert_null(strstr(json, secret));
		if (NULL != diff)
		{
			g_assert_null(strstr(diff, stored));
			g_assert_null(strstr(diff, secret));
		}
		if (0 == g_strcmp0(source, "mfa"))
		{
			mfa_rows++;
			g_assert_cmpstr(who, ==, "alice");
		}
	}
	g_assert_cmpuint(mfa_rows, ==, 4);
	g_assert_cmpuint(count_audit(fixture, "mfa_enrolment_started"), ==, 1);
	g_assert_cmpuint(count_audit(fixture, "mfa_enrolled"), ==, 1);
	g_assert_cmpuint(count_audit(fixture, "mfa_failure"), ==, 1);
	g_assert_cmpuint(count_audit(fixture, "mfa_disabled"), ==, 1);
	/* The row's own update diff records that the secret changed, not what
	 * it became. */
	{
		g_autoptr(VentureQuery) changes = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
		g_autoptr(GPtrArray) rows = NULL;
		gboolean redacted = FALSE;
		venture_query_add_filter_string(changes, "target-type", VENTURE_FILTER_OP_EQ, "user_mfa", NULL);
		venture_query_set_limit(changes, 0);
		rows = venture_database_find(fixture->database, changes, NULL);
		for (i = 0; i < rows->len; i++)
		{
			g_autofree gchar *diff = NULL;
			g_object_get(g_ptr_array_index(rows, i), "diff", &diff, NULL);
			if (NULL != diff && NULL != strstr(diff, "\"redacted\""))
				redacted = TRUE;
		}
		g_assert_true(redacted);
	}
}

/* --- Disable and the break-glass reset ------------------------------------------------- */

static void
test_disable_and_reset(Fixture *fixture, gconstpointer user_data)
{
	g_autofree gchar *secret = NULL;
	g_autofree gchar *code = NULL;
	g_auto(GStrv) codes = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) params = g_hash_table_new(g_str_hash, g_str_equal);
	VentureActionRegistry *registry = venture_database_get_action_registry(fixture->database);
	VentureActor alice = actor_named("alice");
	VentureActor owner = actor_named("owner");
	gint64 user = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	guint i;
	(void)user_data;
	set_clock(fixture, "2026-02-01T09:00:00Z");
	secret = enrol(fixture, user, &codes);

	/* Disable spends every unused recovery code and drops the secret. */
	g_assert_true(venture_mfa_service_disable(fixture->service, user, &alice, &error));
	g_assert_no_error(error);
	g_assert_false(venture_mfa_service_is_enabled(fixture->service, user));
	rows = recovery_rows(fixture, user);
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(GDateTime) used = NULL;
		g_object_get(g_ptr_array_index(rows, i), "used-at", &used, NULL);
		g_assert_nonnull(used);
	}
	g_assert_false(venture_mfa_service_verify(fixture->service, user, codes[0], &alice, NULL, &error));
	g_clear_error(&error);
	g_assert_cmpuint(count_audit(fixture, "mfa_disabled"), ==, 1);
	g_assert_false(venture_mfa_service_disable(fixture->service, user, &alice, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* Enrol again, then break glass through the action registry. */
	g_clear_pointer(&secret, g_free);
	g_strfreev(g_steal_pointer(&codes));
	secret = enrol(fixture, user, &codes);
	g_assert_true(venture_mfa_service_is_enabled(fixture->service, user));

	/* An editor may not run it. */
	result = venture_action_registry_perform(registry, "user", user, "mfa_reset", params, &alice, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_true(venture_mfa_service_is_enabled(fixture->service, user));

	/* An owner may, and it is audited as a reset by them. */
	result = venture_action_registry_perform(registry, "user", user, "mfa_reset", params, &owner, VENTURE_USER_ROLE_OWNER, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_false(venture_mfa_service_is_enabled(fixture->service, user));
	g_assert_cmpuint(count_audit(fixture, "mfa_reset"), ==, 1);
	code = app_code(fixture, secret, 0);
	g_assert_false(venture_mfa_service_verify(fixture->service, user, code, &alice, NULL, &error));
	g_clear_error(&error);
	/* The password alone signs in again, which is the point of the rescue. */
	{
		g_autofree gchar *cookie = NULL;
		g_assert_true(venture_auth_login(fixture->auth, "alice", "correct horse battery", NULL, &cookie, &error));
		g_assert_no_error(error);
	}
}

/* --- The organization setting ---------------------------------------------------------- */

static void
test_policy_requires_admins(Fixture *fixture, gconstpointer user_data)
{
	g_autoptr(VentureEntity) policy = NULL;
	VentureAuthPrincipal admin = { 0, 0, VENTURE_USER_ROLE_EDITOR, (gchar *)"admin", TRUE };
	VentureAuthPrincipal editor = { 0, 0, VENTURE_USER_ROLE_EDITOR, (gchar *)"editor", TRUE };
	VentureAuthPrincipal global = { 0, 0, VENTURE_USER_ROLE_OWNER, (gchar *)"root", TRUE };
	VentureAuthPrincipal nobody = { 0, 0, VENTURE_USER_ROLE_VIEWER, NULL, FALSE };
	(void)user_data;
	admin.user_id = create_user(fixture, "admin", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_ADMIN);
	editor.user_id = create_user(fixture, "editor", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	global.user_id = create_user(fixture, "root", VENTURE_USER_ROLE_OWNER, -1);

	/* No policy: nobody is gated. */
	g_assert_false(venture_mfa_service_requires_enrolment(fixture->service, &admin));
	g_assert_false(venture_mfa_service_requires_enrolment(fixture->service, &global));

	policy = g_object_new(VENTURE_TYPE_MFA_POLICY, "organization-id", fixture->org, "require-mfa-for-admins", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, policy, NULL, NULL));

	g_assert_true(venture_mfa_service_requires_enrolment(fixture->service, &admin));
	g_assert_true(venture_mfa_service_requires_enrolment(fixture->service, &global));
	g_assert_false(venture_mfa_service_requires_enrolment(fixture->service, &editor));
	g_assert_false(venture_mfa_service_requires_enrolment(fixture->service, &nobody));

	/* Enrolled, the admin is through. */
	set_clock(fixture, "2026-02-01T09:00:00Z");
	g_free(enrol(fixture, admin.user_id, NULL));
	g_assert_false(venture_mfa_service_requires_enrolment(fixture->service, &admin));

	/* Switching the setting off releases the rest. */
	g_object_set(policy, "require-mfa-for-admins", FALSE, NULL);
	g_assert_true(venture_database_save(fixture->database, policy, NULL, NULL));
	g_assert_false(venture_mfa_service_requires_enrolment(fixture->service, &global));
}

/* --- HTTP surface ------------------------------------------------------------------------ */

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
request_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	RequestResult *outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &outcome->error);
	outcome->done = TRUE;
}

static guint
request(Fixture *fixture, const gchar *method, const gchar *path, const gchar *cookie,
	const gchar *form_body, const gchar *bearer, gchar **out_body, gchar **out_set_cookie, gchar **out_location)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	RequestResult outcome = { FALSE, NULL, NULL };
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (NULL != cookie)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", cookie);
	if (NULL != bearer)
	{
		g_autofree gchar *header = g_strdup_printf("Bearer %s", bearer);
		soup_message_headers_append(soup_message_get_request_headers(message), "Authorization", header);
	}
	if (NULL != form_body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(form_body, strlen(form_body));
		soup_message_set_request_body_from_bytes(message,
			g_str_has_prefix(form_body, "{") ? "application/json" : "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(fixture->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &outcome);
	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);
	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);
	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL), g_bytes_get_size(outcome.body));
	if (NULL != out_set_cookie)
		*out_set_cookie = g_strdup(soup_message_headers_get_list(soup_message_get_response_headers(message), "Set-Cookie"));
	if (NULL != out_location)
		*out_location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));
	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);
	return soup_message_get_status(message);
}

/* The name=value part of the named cookie in a Set-Cookie header list;
 * libsoup joins several headers with a comma. */
static gchar *
cookie_named(const gchar *set_cookie, const gchar *name)
{
	g_autofree gchar *prefix = g_strdup_printf("%s=", name);
	const gchar *start;
	const gchar *end;
	g_assert_nonnull(set_cookie);
	start = strstr(set_cookie, prefix);
	g_assert_nonnull(start);
	end = strchr(start, ';');
	return NULL != end ? g_strndup(start, (gsize)(end - start)) : g_strdup(start);
}

static gchar *
cookie_pair(const gchar *set_cookie)
{
	return cookie_named(set_cookie, strstr(set_cookie, "venture_mfa=") == set_cookie ? "venture_mfa" : "venture_session");
}

static gchar *
sign_in(Fixture *fixture, const gchar *username)
{
	g_autofree gchar *body = g_strdup_printf("username=%s&password=correct%%20horse%%20battery", username);
	g_autofree gchar *set_cookie = NULL;
	g_assert_cmpuint(request(fixture, "POST", "/login", NULL, body, NULL, NULL, &set_cookie, NULL), ==, 302);
	return cookie_pair(set_cookie);
}

/* An organization admin without a second factor is sent to enrolment and
 * refused everywhere else until enrolled. */
static void
test_web_enrolment_gate(Fixture *fixture, gconstpointer user_data)
{
	g_autoptr(VentureEntity) policy = NULL;
	g_autoptr(VentureMfaEnrolment) pending = NULL;
	g_autofree gchar *admin_cookie = NULL;
	g_autofree gchar *editor_cookie = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *code = NULL;
	g_autofree gchar *form = NULL;
	gint64 admin = create_user(fixture, "admin", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_ADMIN);
	(void)user_data;
	create_user(fixture, "editor", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	set_clock(fixture, "2026-02-01T09:00:00Z");
	admin_cookie = sign_in(fixture, "admin");
	editor_cookie = sign_in(fixture, "editor");

	/* Without the setting the admin browses freely. */
	g_assert_cmpuint(request(fixture, "GET", "/account", admin_cookie, NULL, NULL, NULL, NULL, NULL), ==, 200);
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/companies", admin_cookie, NULL, NULL, NULL, NULL, NULL), ==, 200);

	policy = g_object_new(VENTURE_TYPE_MFA_POLICY, "organization-id", fixture->org, "require-mfa-for-admins", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, policy, NULL, NULL));

	/* Now every page redirects to enrolment and every API call is 403. */
	g_assert_cmpuint(request(fixture, "GET", "/", admin_cookie, NULL, NULL, NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/account/mfa/enrol");
	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(request(fixture, "GET", "/e/company", admin_cookie, NULL, NULL, NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/account/mfa/enrol");
	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/companies", admin_cookie, NULL, NULL, &body, NULL, NULL), ==, 403);
	g_assert_nonnull(strstr(body, "second factor"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "POST", "/api/v1/companies", admin_cookie, "{\"name\":\"X\"}", NULL, NULL, NULL, NULL), ==, 403);
	/* The editor is not an admin and is untouched. */
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/companies", editor_cookie, NULL, NULL, NULL, NULL, NULL), ==, 200);
	/* Signing out and the account page stay reachable. */
	g_assert_cmpuint(request(fixture, "GET", "/account", admin_cookie, NULL, NULL, NULL, NULL, NULL), ==, 200);

	/* Enrolment itself works: the page carries the QR code. */
	g_assert_cmpuint(request(fixture, "GET", "/account/mfa/enrol", admin_cookie, NULL, NULL, &body, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "<svg"));
	g_assert_nonnull(strstr(body, "action=\"/account/mfa/confirm\""));
	g_clear_pointer(&body, g_free);
	pending = venture_mfa_service_pending_enrolment(fixture->service, admin, "Books", "admin", NULL);
	g_assert_nonnull(pending);
	g_assert_cmpuint(request(fixture, "POST", "/account/mfa/confirm", admin_cookie, "code=000000", NULL, NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/account/mfa/enrol?notice=wrong");
	g_clear_pointer(&location, g_free);
	code = app_code(fixture, pending->secret, 0);
	form = g_strdup_printf("code=%s", code);
	g_assert_cmpuint(request(fixture, "POST", "/account/mfa/confirm", admin_cookie, form, NULL, &body, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "Recovery codes"));
	g_clear_pointer(&body, g_free);

	/* Through. */
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/companies", admin_cookie, NULL, NULL, NULL, NULL, NULL), ==, 200);
	g_assert_cmpuint(request(fixture, "GET", "/account/mfa", admin_cookie, NULL, NULL, &body, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "Turn off"));
}

/* The browser sign-in: password, then code, then session. */
static void
test_web_login_flow(Fixture *fixture, gconstpointer user_data)
{
	g_autofree gchar *secret = NULL;
	g_autofree gchar *set_cookie = NULL;
	g_autofree gchar *challenge = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *code = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *session = NULL;
	gint64 user = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	(void)user_data;
	set_clock(fixture, "2026-02-01T09:00:00Z");
	secret = enrol(fixture, user, NULL);
	set_clock(fixture, "2026-02-01T09:30:00Z");

	g_assert_cmpuint(request(fixture, "POST", "/login", NULL, "username=alice&password=correct%20horse%20battery", NULL, NULL, &set_cookie, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/login/mfa");
	challenge = cookie_pair(set_cookie);
	g_assert_true(g_str_has_prefix(challenge, "venture_mfa="));
	g_clear_pointer(&location, g_free);

	/* The challenge is not a session. */
	g_assert_cmpuint(request(fixture, "GET", "/account", challenge, NULL, NULL, NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/login");
	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/companies", challenge, NULL, NULL, NULL, NULL, NULL), ==, 401);

	/* The code page needs the challenge. */
	g_assert_cmpuint(request(fixture, "GET", "/login/mfa", NULL, NULL, NULL, NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/login");
	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(request(fixture, "GET", "/login/mfa", challenge, NULL, NULL, &body, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "name=\"code\""));
	g_clear_pointer(&body, g_free);

	/* Wrong code: back to the prompt. */
	g_assert_cmpuint(request(fixture, "POST", "/login/mfa", challenge, "code=000000", NULL, NULL, &set_cookie, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/login/mfa?notice=wrong");
	g_clear_pointer(&location, g_free);
	g_clear_pointer(&set_cookie, g_free);

	/* Right code: a session. */
	code = app_code(fixture, secret, 0);
	form = g_strdup_printf("code=%s", code);
	g_assert_cmpuint(request(fixture, "POST", "/login/mfa", challenge, form, NULL, NULL, &set_cookie, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/");
	session = cookie_named(set_cookie, "venture_session");
	g_assert_true(g_str_has_prefix(session, "venture_session="));
	/* The challenge is discarded alongside. */
	g_assert_nonnull(strstr(set_cookie, "venture_mfa=;"));
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/companies", session, NULL, NULL, NULL, NULL, NULL), ==, 200);
	g_assert_cmpuint(count_audit(fixture, "mfa_success"), ==, 1);

	/* Disabling from the settings page needs the password and a code. */
	set_clock(fixture, "2026-02-01T09:35:00Z");
	g_clear_pointer(&code, g_free);
	g_clear_pointer(&form, g_free);
	code = app_code(fixture, secret, 0);
	form = g_strdup_printf("current_password=wrong&code=%s", code);
	g_assert_cmpuint(request(fixture, "POST", "/account/mfa/disable", session, form, NULL, NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/account/mfa?notice=password");
	g_clear_pointer(&location, g_free);
	g_assert_true(venture_mfa_service_is_enabled(fixture->service, user));
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("current_password=correct%%20horse%%20battery&code=%s", code);
	g_assert_cmpuint(request(fixture, "POST", "/account/mfa/disable", session, form, NULL, NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/account/mfa?notice=disabled");
	g_assert_false(venture_mfa_service_is_enabled(fixture->service, user));
}

/* REST, CSV and the audit surface never emit the secret or a hash. */
static void
test_web_redaction(Fixture *fixture, gconstpointer user_data)
{
	g_autofree gchar *secret = NULL;
	g_autofree gchar *stored = NULL;
	g_autofree gchar *owner_cookie = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(VentureUserMfa) row = NULL;
	g_autoptr(GPtrArray) codes = NULL;
	g_autofree gchar *hash = NULL;
	gint64 user;
	(void)user_data;
	create_user(fixture, "root", VENTURE_USER_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_OWNER);
	user = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	set_clock(fixture, "2026-02-01T09:00:00Z");
	secret = enrol(fixture, user, NULL);
	row = venture_mfa_service_get_row(fixture->service, user);
	g_object_get(row, "secret-ref", &stored, NULL);
	codes = recovery_rows(fixture, user);
	g_object_get(g_ptr_array_index(codes, 0), "code-hash", &hash, NULL);
	owner_cookie = sign_in(fixture, "root");

	g_assert_cmpuint(request(fixture, "GET", "/api/v1/user_mfa", owner_cookie, NULL, NULL, &body, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "\"enabled\""));
	g_assert_null(strstr(body, stored));
	g_assert_null(strstr(body, secret));
	g_assert_null(strstr(body, "secret_ref"));
	g_clear_pointer(&body, g_free);
	path = g_strdup_printf("/api/v1/user_mfa/%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(row)));
	g_assert_cmpuint(request(fixture, "GET", path, owner_cookie, NULL, NULL, &body, NULL, NULL), ==, 200);
	g_assert_null(strstr(body, stored));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "GET", "/e/user_mfa/export", owner_cookie, NULL, NULL, &body, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "Enabled"));
	g_assert_null(strstr(body, "Secret"));
	g_assert_null(strstr(body, stored));
	g_assert_null(strstr(body, "secret_ref"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/mfa_recovery_code", owner_cookie, NULL, NULL, &body, NULL, NULL), ==, 200);
	g_assert_null(strstr(body, hash));
	g_assert_null(strstr(body, "code_hash"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/audit_entry", owner_cookie, NULL, NULL, &body, NULL, NULL), ==, 200);
	g_assert_null(strstr(body, stored));
	g_assert_null(strstr(body, secret));
	g_assert_null(strstr(body, hash));
	g_clear_pointer(&body, g_free);
	/* The generic update route cannot set the secret or flip enabled. */
	g_assert_cmpuint(request(fixture, "PUT", path, owner_cookie, "{\"enabled\":false}", NULL, &body, NULL, NULL), ==, 403);
	g_assert_true(venture_mfa_service_is_enabled(fixture->service, user));
}

static void
cli_wait(GObject *source, GAsyncResult *result, gpointer user_data)
{
	gboolean *done = user_data;
	g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
	*done = TRUE;
}

/* venturectl user mfa reset USER, over the action route, owner only. */
static void
test_cli_reset(Fixture *fixture, gconstpointer user_data)
{
	g_autofree gchar *secret = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u", fixture->port);
	g_autofree gchar *owner_secret = NULL;
	g_autofree gchar *editor_secret = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(VentureApiToken) owner_token = venture_api_token_new();
	g_autoptr(VentureApiToken) editor_token = venture_api_token_new();
	g_autoptr(GSubprocess) process = NULL;
	gboolean done = FALSE;
	gint64 root = create_user(fixture, "root", VENTURE_USER_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_OWNER);
	gint64 bob = create_user(fixture, "bob", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	gint64 alice = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	(void)user_data;
	set_clock(fixture, "2026-02-01T09:00:00Z");
	secret = enrol(fixture, alice, NULL);
	g_object_set(owner_token, "name", "cli", "user-id", root, "role", VENTURE_USER_ROLE_OWNER, NULL);
	owner_secret = venture_api_token_generate(owner_token);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(owner_token), NULL, NULL));
	g_object_set(editor_token, "name", "cli", "user-id", bob, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	editor_secret = venture_api_token_generate(editor_token);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(editor_token), NULL, NULL));

	/* The route refuses an editor. */
	path = g_strdup_printf("/api/v1/users/%" G_GINT64_FORMAT "/actions/mfa_reset", alice);
	g_assert_cmpuint(request(fixture, "POST", path, NULL, "{}", editor_secret, &body, NULL, NULL), ==, 403);
	g_assert_true(venture_mfa_service_is_enabled(fixture->service, alice));

	/* The CLI, as an owner, by username. */
	process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE, NULL, "build/debug/venturectl",
		"--server", url, "--token", owner_secret, "user", "mfa", "reset", "alice", NULL);
	g_assert_nonnull(process);
	g_subprocess_wait_async(process, NULL, cli_wait, &done);
	while (!done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_true(g_subprocess_get_successful(process));
	g_assert_false(venture_mfa_service_is_enabled(fixture->service, alice));
	g_assert_cmpuint(count_audit(fixture, "mfa_reset"), ==, 1);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
		g_autoptr(VentureEntity) entry = NULL;
		g_autofree gchar *who = NULL;
		venture_query_add_filter_string(query, "source", VENTURE_FILTER_OP_EQ, "mfa", NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		entry = venture_database_find_one(fixture->database, query, NULL);
		g_object_get(entry, "actor", &who, NULL);
		g_assert_nonnull(strstr(who, "token:cli"));
	}
	/* A bad usage line is refused before any request is made. */
	g_clear_object(&process);
	done = FALSE;
	process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "build/debug/venturectl",
		"--server", url, "--token", owner_secret, "user", "mfa", "off", "alice", NULL);
	g_subprocess_wait_async(process, NULL, cli_wait, &done);
	while (!done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_false(g_subprocess_get_successful(process));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/mfa/records/registered", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_records_registered, fixture_tear_down);
	g_test_add("/mfa/records/generic-writes-refused", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_generic_writes_refused, fixture_tear_down);
	g_test_add("/mfa/totp/rfc6238-vectors", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_totp_vectors, fixture_tear_down);
	g_test_add("/mfa/totp/drift-window", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_totp_drift_window, fixture_tear_down);
	g_test_add("/mfa/qr/svg", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_qr_svg, fixture_tear_down);
	g_test_add("/mfa/enrolment/flow", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_enrolment, fixture_tear_down);
	g_test_add("/mfa/enrolment/needs-key", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_enrolment_needs_key, fixture_tear_down);
	g_test_add("/mfa/login/requires-code", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_login_requires_code, fixture_tear_down);
	g_test_add("/mfa/verify/drift-and-replay", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_verify_drift_and_replay, fixture_tear_down);
	g_test_add("/mfa/verify/rate-limit", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_verify_rate_limit, fixture_tear_down);
	g_test_add("/mfa/verify/recovery-codes", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_recovery_codes_single_use, fixture_tear_down);
	g_test_add("/mfa/audit/redacted", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_audit_rows_never_carry_secrets, fixture_tear_down);
	g_test_add("/mfa/disable-and-reset", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_disable_and_reset, fixture_tear_down);
	g_test_add("/mfa/policy/requires-admins", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up, test_policy_requires_admins, fixture_tear_down);
	g_test_add("/mfa/web/enrolment-gate", Fixture, GINT_TO_POINTER(TRUE), fixture_set_up, test_web_enrolment_gate, fixture_tear_down);
	g_test_add("/mfa/web/login-flow", Fixture, GINT_TO_POINTER(TRUE), fixture_set_up, test_web_login_flow, fixture_tear_down);
	g_test_add("/mfa/web/redaction", Fixture, GINT_TO_POINTER(TRUE), fixture_set_up, test_web_redaction, fixture_tear_down);
	g_test_add("/mfa/cli/reset", Fixture, GINT_TO_POINTER(TRUE), fixture_set_up, test_cli_reset, fixture_tear_down);
	return g_test_run();
}
