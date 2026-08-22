/*
 * venture-auth.c - Authentication and session handling
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

#define VENTURE_AUTH_COOKIE_NAME "venture_session"

struct _VentureAuth
{
	GObject parent_instance;

	VentureContext	*context;
	gchar		*secret;
	gboolean	 required;
	gint64		 lifetime;
	gboolean	 cookie_secure;
	guint		 password_iterations;
	guint		 password_min_length;
	HtmxRateLimiter	*login_limiter;
};

G_DEFINE_FINAL_TYPE(VentureAuth, venture_auth, G_TYPE_OBJECT)

static void
venture_auth_finalize(GObject *object)
{
	VentureAuth *self;

	self = VENTURE_AUTH(object);

	g_clear_object(&self->context);
	g_clear_object(&self->login_limiter);
	g_clear_pointer(&self->secret, g_free);

	G_OBJECT_CLASS(venture_auth_parent_class)->finalize(object);
}

static void
venture_auth_class_init(VentureAuthClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_auth_finalize;
}

static void
venture_auth_init(VentureAuth *self)
{
}

VentureAuth *
venture_auth_new(VentureContext *context)
{
	VentureAuth *self;
	VentureConfig *config;
	const gchar *secret;
	gint64 iterations;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	self = g_object_new(VENTURE_TYPE_AUTH, NULL);
	self->context = g_object_ref(context);

	config = venture_context_get_config(context);
	secret = venture_config_get_secret(config, "security-session-secret-env");

	if (venture_string_is_empty(secret))
	{
		/* A random secret is secure but ephemeral. Saying so is
		 * important: otherwise every restart silently logs everyone
		 * out and it looks like a bug. */
		self->secret = venture_generate_token(32);
		g_message("No session secret in the environment; sessions will not "
		          "survive a restart. Set the variable named by "
		          "security.session_secret_env to keep them.");
	}
	else
	{
		self->secret = g_strdup(secret);
	}

	g_object_get(config,
	             "security-require-auth", &self->required,
	             "security-session-lifetime", &self->lifetime,
	             "security-cookie-secure", &self->cookie_secure,
	             "security-password-iterations", &iterations,
	             NULL);

	self->password_iterations = (guint)iterations;

	{
		gint64 minimum;

		g_object_get(config, "security-password-min-length", &minimum, NULL);
		self->password_min_length = (guint)MAX(minimum, 1);
	}

	{
		gint64 rate;

		g_object_get(config, "security-login-rate-limit", &rate, NULL);

		/* The bucket holds a minute's budget and refills continuously,
		 * so a burst up to the limit is fine and a sustained guess rate
		 * beyond it is not. Zero or negative disables the limit, which
		 * is for tests, not deployments. */
		if (rate > 0)
		{
			self->login_limiter = htmx_rate_limiter_new((guint)rate,
				(gdouble)rate / 60.0);
		}
	}

	return self;
}

gchar *
venture_auth_remote_address(HtmxRequest *request)
{
	GSocketAddress *remote;
	GInetAddress *inet;
	SoupServerMessage *message;

	g_return_val_if_fail(HTMX_IS_REQUEST(request), NULL);

	message = htmx_request_get_message(request);

	if (NULL == message)
		return NULL;

	remote = soup_server_message_get_remote_address(message);

	if (!G_IS_INET_SOCKET_ADDRESS(remote))
		return NULL;

	inet = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(remote));

	return g_inet_address_to_string(inet);
}

void
venture_auth_principal_free(VentureAuthPrincipal *principal)
{
	if (NULL == principal)
		return;

	g_free(principal->name);
	g_free(principal);
}

gboolean
venture_auth_is_required(VentureAuth *self)
{
	g_return_val_if_fail(VENTURE_IS_AUTH(self), TRUE);

	return self->required;
}

/* --- Session cookies ----------------------------------------------------- */

/*
 * Signs a session payload. The MAC covers the user id and the expiry
 * together, so neither can be altered independently -- extending a session
 * by editing the expiry would invalidate the signature.
 */
static gchar *
venture_auth_sign(
	VentureAuth	*self,
	gint64		 user_id,
	gint64		 issued_at,
	gint64		 expires_at
){
	g_autoptr(GHmac) hmac = NULL;
	g_autofree gchar *payload = NULL;

	payload = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT
	                          ":%" G_GINT64_FORMAT,
	                          user_id, issued_at, expires_at);

	hmac = g_hmac_new(G_CHECKSUM_SHA256, (const guchar *)self->secret,
	                  strlen(self->secret));
	g_hmac_update(hmac, (const guchar *)payload, (gssize)strlen(payload));

	return g_strdup(g_hmac_get_string(hmac));
}

static gchar *
venture_auth_make_cookie(
	VentureAuth	*self,
	gint64		 user_id
){
	g_autoptr(HtmxCookie) cookie = NULL;
	g_autofree gchar *value = NULL;
	g_autofree gchar *signature = NULL;
	g_autoptr(GDateTime) now = NULL;
	gint64 issued_at;
	gint64 expires_at;

	now = venture_time_now();

	/*
	 * Microseconds, not seconds. A cut-off recorded in the same second a
	 * cookie was issued is ambiguous at second resolution, and the two
	 * ways to resolve it are both wrong: accept, and signing out leaves
	 * that second's sessions alive; reject, and signing straight back in
	 * fails until the second is over.
	 */
	issued_at = (g_date_time_to_unix(now) * G_USEC_PER_SEC) +
		g_date_time_get_microsecond(now);
	expires_at = g_date_time_to_unix(now) + self->lifetime;
	signature = venture_auth_sign(self, user_id, issued_at, expires_at);

	/* The issue time is carried and signed so that signing out can
	 * invalidate everything issued before a given instant. */
	value = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT
	                        ":%" G_GINT64_FORMAT ":%s",
	                        user_id, issued_at, expires_at, signature);

	cookie = htmx_cookie_new(VENTURE_AUTH_COOKIE_NAME, value);
	htmx_cookie_set_path(cookie, "/");
	htmx_cookie_set_max_age(cookie, self->lifetime);
	/* HttpOnly keeps the cookie out of reach of any script on the page,
	 * and SameSite=Lax stops a third-party form from acting as the
	 * operator. Both matter more here than on a typical site, because the
	 * session can move money figures. */
	htmx_cookie_set_http_only(cookie, TRUE);
	htmx_cookie_set_same_site(cookie, HTMX_COOKIE_SAME_SITE_LAX);
	htmx_cookie_set_secure(cookie, self->cookie_secure);

	return htmx_cookie_to_set_cookie(cookie);
}

/*
 * Verifies a cookie value and returns the user it identifies, or 0.
 */
static gint64
venture_auth_verify_cookie(
	VentureAuth	*self,
	const gchar	*value,
	gint64		*out_issued_at
){
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *expected = NULL;
	g_autoptr(GDateTime) now = NULL;
	gint64 user_id;
	gint64 issued_at;
	gint64 expires_at;

	if (NULL != out_issued_at)
		*out_issued_at = 0;

	if (venture_string_is_empty(value))
		return 0;

	parts = g_strsplit(value, ":", 4);

	/* A cookie from before the issue time was added has three parts and
	 * simply fails here, which signs those sessions out once. */
	if ((NULL == parts[0]) || (NULL == parts[1]) || (NULL == parts[2]) ||
	    (NULL == parts[3]))
		return 0;

	user_id = g_ascii_strtoll(parts[0], NULL, 10);
	issued_at = g_ascii_strtoll(parts[1], NULL, 10);
	expires_at = g_ascii_strtoll(parts[2], NULL, 10);

	if (0 == user_id)
		return 0;

	expected = venture_auth_sign(self, user_id, issued_at, expires_at);

	/* Constant-time comparison: a timing-variable one would leak the
	 * signature a byte at a time to a patient attacker. */
	if (!venture_constant_time_equal(expected, parts[3]))
		return 0;

	now = venture_time_now();

	if (g_date_time_to_unix(now) >= expires_at)
		return 0;

	if (NULL != out_issued_at)
		*out_issued_at = issued_at;

	return user_id;
}

gboolean
venture_auth_end_sessions(
	VentureAuth	 *self,
	gint64		  user_id,
	GError		**error
){
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(GDateTime) now = NULL;

	g_return_val_if_fail(VENTURE_IS_AUTH(self), FALSE);

	if (0 == user_id)
		return TRUE;

	user = venture_database_get(venture_context_get_database(self->context),
	                            VENTURE_TYPE_USER, user_id, NULL);

	/* No account behind the session -- an API token, or authentication
	 * turned off. There is nothing to invalidate. */
	if (NULL == user)
		return TRUE;

	now = venture_time_now();
	g_object_set(user, "sessions-invalidated-at", now, NULL);

	return venture_database_save(venture_context_get_database(self->context),
	                             user, NULL, error);
}

gchar *
venture_auth_logout_cookie(VentureAuth *self)
{
	g_autoptr(HtmxCookie) cookie = NULL;

	g_return_val_if_fail(VENTURE_IS_AUTH(self), NULL);

	cookie = htmx_cookie_new(VENTURE_AUTH_COOKIE_NAME, "");
	htmx_cookie_set_path(cookie, "/");
	htmx_cookie_set_max_age(cookie, 0);
	htmx_cookie_set_http_only(cookie, TRUE);

	return htmx_cookie_to_set_cookie(cookie);
}

/* --- Authentication ------------------------------------------------------ */

/*
 * Extracts the bearer token from an Authorization header, if present.
 */
static const gchar *
venture_auth_bearer_token(HtmxRequest *request)
{
	SoupServerMessage *message;
	SoupMessageHeaders *headers;
	const gchar *authorization;

	message = htmx_request_get_message(request);

	if (NULL == message)
		return NULL;

	headers = soup_server_message_get_request_headers(message);

	if (NULL == headers)
		return NULL;

	authorization = soup_message_headers_get_one(headers, "Authorization");

	if (NULL == authorization)
		return NULL;

	if (!g_str_has_prefix(authorization, "Bearer "))
		return NULL;

	return authorization + strlen("Bearer ");
}

/*
 * Resolves a presented bearer token by looking up its prefix and then
 * verifying the full value against the stored hash.
 *
 * The prefix lookup is what keeps this from hashing every token in the
 * table on every request; the hash comparison is what makes the prefix
 * alone useless.
 */
static VentureAuthPrincipal *
venture_auth_from_token(
	VentureAuth	*self,
	const gchar	*presented
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) tokens = NULL;
	VentureDatabase *database;
	const gchar *secret;
	g_autofree gchar *prefix = NULL;
	guint i;

	secret = g_str_has_prefix(presented, "vk_") ? (presented + 3) : presented;

	if (strlen(secret) < 8)
		return NULL;

	prefix = g_strndup(secret, 8);
	database = venture_context_get_database(self->context);

	query = venture_query_new(VENTURE_TYPE_API_TOKEN);

	if (!venture_query_add_filter_string(query, "prefix",
	                                     VENTURE_FILTER_OP_EQ, prefix, NULL))
		return NULL;

	tokens = venture_database_find(database, query, NULL);

	if (NULL == tokens)
		return NULL;

	for (i = 0; i < tokens->len; i++)
	{
		VentureAuthPrincipal *principal;
		VentureEntity *token;
		g_autofree gchar *name = NULL;
		VentureUserRole role;
		gint64 user_id;

		token = g_ptr_array_index(tokens, i);

		if (!venture_api_token_matches(VENTURE_API_TOKEN(token), presented))
			continue;

		g_object_get(token, "name", &name, "role", &role,
		             "user-id", &user_id, NULL);

		principal = g_new0(VentureAuthPrincipal, 1);
		principal->authenticated = TRUE;
		principal->token_id = venture_entity_get_id(token);
		principal->user_id = user_id;
		principal->role = role;
		principal->name = g_strdup_printf("token:%s",
			(NULL != name) ? name : prefix);

		/* Recording use makes a leaked token visible in the audit
		 * trail and lets an unused one be retired with confidence. */
		{
			g_autoptr(GDateTime) now = NULL;

			now = venture_time_now();
			g_object_set(token, "last-used-at", now, NULL);
			venture_database_save(database, token, NULL, NULL);
		}

		return principal;
	}

	return NULL;
}

VentureAuthPrincipal *
venture_auth_authenticate(
	VentureAuth	*self,
	HtmxRequest	*request
){
	VentureAuthPrincipal *principal;
	const gchar *token;

	g_return_val_if_fail(VENTURE_IS_AUTH(self), NULL);

	principal = g_new0(VentureAuthPrincipal, 1);
	principal->role = VENTURE_USER_ROLE_VIEWER;

	/*
	 * With authentication off every request is the owner. Configuration
	 * validation refuses that combination on a non-loopback address, so
	 * this can only be reached on a machine the operator already
	 * controls.
	 */
	if (!self->required)
	{
		principal->authenticated = TRUE;
		principal->role = VENTURE_USER_ROLE_OWNER;
		principal->name = g_strdup("local");

		return principal;
	}

	token = venture_auth_bearer_token(request);

	if (NULL != token)
	{
		VentureAuthPrincipal *from_token;

		from_token = venture_auth_from_token(self, token);

		if (NULL != from_token)
		{
			venture_auth_principal_free(principal);
			return from_token;
		}

		/* A presented-but-invalid token is not silently downgraded to
		 * an anonymous request: it stays unauthenticated so the caller
		 * gets a 401 rather than a confusing 403 later. */
		return principal;
	}

	{
		SoupServerMessage *message;
		SoupMessageHeaders *headers;
		const gchar *cookie_header;

		message = htmx_request_get_message(request);
		headers = (NULL != message)
			? soup_server_message_get_request_headers(message) : NULL;
		cookie_header = (NULL != headers)
			? soup_message_headers_get_list(headers, "Cookie") : NULL;

		if (NULL != cookie_header)
		{
			g_autoptr(GHashTable) cookies = NULL;

			cookies = htmx_cookie_parse_request(cookie_header);

			if (NULL != cookies)
			{
				gint64 user_id;
				gint64 issued_at;

				user_id = venture_auth_verify_cookie(self,
					g_hash_table_lookup(cookies,
					                    VENTURE_AUTH_COOKIE_NAME),
					&issued_at);

				if (0 != user_id)
				{
					g_autoptr(VentureEntity) user = NULL;

					user = venture_database_get(
						venture_context_get_database(self->context),
						VENTURE_TYPE_USER, user_id, NULL);

					if (NULL != user)
					{
						g_autoptr(GDateTime) invalidated = NULL;
						gboolean active;
						gboolean current;

						g_object_get(user, "active", &active,
						             "sessions-invalidated-at", &invalidated,
						             NULL);

						/*
						 * A cookie issued before the account last
						 * signed out is refused. Without this,
						 * signing out only asks the browser to
						 * forget the cookie -- anybody still
						 * holding a copy stays signed in until it
						 * expires.
						 */
						current = (NULL == invalidated) ||
							(issued_at >=
							 ((g_date_time_to_unix(invalidated) *
							   G_USEC_PER_SEC) +
							  g_date_time_get_microsecond(invalidated)));

						/* A valid cookie for a deactivated account
						 * must not work: revocation has to take
						 * effect without waiting for expiry. */
						if (active && current)
						{
							g_autofree gchar *username = NULL;
							VentureUserRole role;

							g_object_get(user, "username", &username,
							             "role", &role, NULL);

							principal->authenticated = TRUE;
							principal->user_id = user_id;
							principal->role = role;
							principal->name = g_steal_pointer(&username);
						}
					}
				}
			}
		}
	}

	return principal;
}

gboolean
venture_auth_login(
	VentureAuth	 *self,
	const gchar	 *username,
	const gchar	 *password,
	const gchar	 *remote_address,
	gchar		**out_cookie,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) user = NULL;
	VentureDatabase *database;

	g_return_val_if_fail(VENTURE_IS_AUTH(self), FALSE);
	g_return_val_if_fail(NULL != out_cookie, FALSE);

	/* The attempt is spent before the password is looked at, because the
	 * guesses a brute force needs are exactly the ones arriving after the
	 * budget is gone. A NULL address is an in-process caller with no
	 * network peer, and is not limited. */
	if ((NULL != self->login_limiter) && (NULL != remote_address) &&
	    !htmx_rate_limiter_allow(self->login_limiter, remote_address))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		                    "Too many sign-in attempts; wait a minute and "
		                    "try again");
		return FALSE;
	}

	database = venture_context_get_database(self->context);

	if (venture_string_is_empty(username) || venture_string_is_empty(password))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		                    "That username and password do not match");
		return FALSE;
	}

	query = venture_query_new(VENTURE_TYPE_USER);

	if (!venture_query_add_filter_string(query, "username",
	                                     VENTURE_FILTER_OP_EQ, username, error))
		return FALSE;

	user = venture_database_find_one(database, query, NULL);

	if (NULL == user)
	{
		g_autoptr(VentureQuery) by_email = NULL;

		/* Logging in by email is what people actually try. */
		by_email = venture_query_new(VENTURE_TYPE_USER);

		if (venture_query_add_filter_string(by_email, "email",
		                                    VENTURE_FILTER_OP_EQ, username,
		                                    NULL))
			user = venture_database_find_one(database, by_email, NULL);
	}

	/*
	 * An unknown user, a wrong password and a deactivated account all
	 * produce this one message. Distinguishing them would turn the login
	 * form into an account enumeration oracle.
	 */
	if ((NULL == user) ||
	    !venture_user_check_password(VENTURE_USER(user), password))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		                    "That username and password do not match");
		return FALSE;
	}

	{
		g_autoptr(GDateTime) now = NULL;

		now = venture_time_now();
		g_object_set(user, "last-login-at", now, NULL);
		venture_database_save(database, user, NULL, NULL);
	}

	*out_cookie = venture_auth_make_cookie(self, venture_entity_get_id(user));

	return TRUE;
}

/*
 * Roles are ordered from most to least privileged, so "at least this role"
 * is a numeric comparison. Service is deliberately outside that order and
 * treated as an editor.
 */
static guint
venture_auth_role_rank(VentureUserRole role)
{
	switch (role)
	{
	case VENTURE_USER_ROLE_OWNER:   return 4;
	case VENTURE_USER_ROLE_ADMIN:   return 3;
	case VENTURE_USER_ROLE_EDITOR:  return 2;
	case VENTURE_USER_ROLE_SERVICE: return 2;
	case VENTURE_USER_ROLE_VIEWER:
	default:                        return 1;
	}
}

gboolean
venture_auth_require(
	VentureAuth		 *self,
	VentureAuthPrincipal	 *principal,
	VentureUserRole		  minimum_role,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_AUTH(self), FALSE);
	g_return_val_if_fail(NULL != principal, FALSE);

	if (!principal->authenticated)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		                    "Sign in to continue");
		return FALSE;
	}

	if (venture_auth_role_rank(principal->role) <
	    venture_auth_role_rank(minimum_role))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		            "This needs the %s role or higher",
		            venture_enum_to_nick(VENTURE_TYPE_USER_ROLE,
		                                 (gint)minimum_role));
		return FALSE;
	}

	return TRUE;
}

void
venture_auth_to_actor(
	VentureAuthPrincipal	*principal,
	VentureActor		*actor
){
	g_return_if_fail(NULL != actor);

	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = (NULL != principal) ? principal->name : NULL;
	actor->prompt = NULL;
	actor->request_id = NULL;

	/* A token-authenticated request is machine traffic, and the audit
	 * trail should say so rather than attributing it to a person. */
	if ((NULL != principal) && (0 != principal->token_id))
		actor->kind = VENTURE_ACTOR_KIND_IMPORT;
}

gboolean
venture_auth_set_password(
	VentureAuth	 *self,
	VentureUser	 *user,
	const gchar	 *password,
	GError		**error
){
	g_return_val_if_fail(VENTURE_IS_AUTH(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_USER(user), FALSE);

	if (venture_string_is_empty(password))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "A password is required");
		return FALSE;
	}

	/*
	 * Length is the only rule. Composition rules -- a digit, a symbol,
	 * mixed case -- push people towards short passwords they mangle to
	 * satisfy a checker, and this system holds tax records.
	 */
	if (g_utf8_strlen(password, -1) < (glong)self->password_min_length)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "Password must be at least %u characters",
		            self->password_min_length);
		return FALSE;
	}

	return venture_user_set_password(user, password,
	                                 self->password_iterations, error);
}

guint
venture_auth_get_password_min_length(VentureAuth *self)
{
	g_return_val_if_fail(VENTURE_IS_AUTH(self), 0);

	return self->password_min_length;
}

gchar *
venture_auth_ensure_owner(
	VentureAuth	 *self,
	const gchar	 *password,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *generated = NULL;
	VentureDatabase *database;
	gint64 existing;

	g_return_val_if_fail(VENTURE_IS_AUTH(self), NULL);

	database = venture_context_get_database(self->context);
	query = venture_query_new(VENTURE_TYPE_USER);
	existing = venture_database_count(database, query, error);

	/* A failed count is not an empty table. Bootstrapping an owner
	 * account because the user count errored would mint a privileged
	 * login on a database that already has its owners. */
	if (existing < 0)
		return NULL;

	if (existing > 0)
		return NULL;

	user = venture_user_new();
	g_object_set(user,
	             "username", "owner",
	             "display-name", "Owner",
	             "role", VENTURE_USER_ROLE_OWNER,
	             "active", TRUE,
	             NULL);

	if (venture_string_is_empty(password))
	{
		/* Better a strong generated password shown once than an
		 * account with a blank or guessable one. */
		generated = venture_generate_token(12);
		password = generated;
	}

	if (!venture_user_set_password(user, password,
	                               self->password_iterations, error))
		return NULL;

	if (!venture_database_save(database, VENTURE_ENTITY(user), NULL, error))
		return NULL;

	return g_steal_pointer(&generated);
}
