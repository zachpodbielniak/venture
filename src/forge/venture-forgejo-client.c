/*
 * venture-forgejo-client.c - Forgejo and Gitea over API v1
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

struct _VentureForgejoClient
{
	GObject parent_instance;

	SoupSession	*session;
	gchar		*scheme;
	gchar		*host;
	gint		 port;
	gchar		*base_path;
	gchar		*token;
	gint		 timeout_seconds;
};

static void venture_forgejo_client_iface_init(VentureForgeClientInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(VentureForgejoClient, venture_forgejo_client,
                              G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(VENTURE_TYPE_FORGE_CLIENT,
                                                    venture_forgejo_client_iface_init))

static void
venture_forgejo_client_finalize(GObject *object)
{
	VentureForgejoClient *self = VENTURE_FORGEJO_CLIENT(object);

	g_clear_object(&self->session);
	g_clear_pointer(&self->scheme, g_free);
	g_clear_pointer(&self->host, g_free);
	g_clear_pointer(&self->base_path, g_free);

	/* The token is the one thing here worth wiping rather than merely
	 * freeing: a freed heap block keeps its contents until something
	 * else claims it, and a core dump taken in between would carry a
	 * live credential. */
	if (NULL != self->token)
	{
		memset(self->token, 0, strlen(self->token));
		g_clear_pointer(&self->token, g_free);
	}

	G_OBJECT_CLASS(venture_forgejo_client_parent_class)->finalize(object);
}

static void
venture_forgejo_client_class_init(VentureForgejoClientClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_forgejo_client_finalize;
}

static void
venture_forgejo_client_init(VentureForgejoClient *self)
{
	self->timeout_seconds = 30;
}

VentureForgejoClient *
venture_forgejo_client_new(
	const gchar	 *base_url,
	const gchar	 *token,
	gint		  timeout_seconds,
	GError		**error
){
	g_autoptr(VentureForgejoClient) self = NULL;
	g_autoptr(GUri) uri = NULL;
	const gchar *scheme;
	const gchar *path;

	if (venture_string_is_empty(base_url))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "The forge has no base URL");
		return NULL;
	}

	uri = g_uri_parse(base_url, G_URI_FLAGS_NONE, NULL);

	if (NULL == uri)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "That is not a valid forge URL");
		return NULL;
	}

	scheme = g_uri_get_scheme(uri);

	/*
	 * Refused here rather than at request time, because this is the only
	 * moment the origin is chosen. Everything downstream reuses the
	 * scheme, host and port taken from this parse, so a scheme that is
	 * not http or https can never reach a request.
	 */
	if ((0 != g_strcmp0(scheme, "http")) && (0 != g_strcmp0(scheme, "https")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "A forge URL must be http or https, not \"%s\"", scheme);
		return NULL;
	}

	if (venture_string_is_empty(g_uri_get_host(uri)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "That forge URL has no host");
		return NULL;
	}

	self = g_object_new(VENTURE_TYPE_FORGEJO_CLIENT, NULL);

	self->scheme = g_strdup(scheme);
	self->host = g_strdup(g_uri_get_host(uri));
	self->port = g_uri_get_port(uri);
	self->token = g_strdup(token);
	self->timeout_seconds = (timeout_seconds > 0) ? timeout_seconds : 30;

	/* A forge served under a sub-path is normal behind a reverse proxy.
	 * The trailing slash is stripped so joining never doubles one. */
	path = g_uri_get_path(uri);

	if (venture_string_is_empty(path) || (0 == g_strcmp0(path, "/")))
	{
		self->base_path = g_strdup("");
	}
	else
	{
		gsize length = strlen(path);

		self->base_path = ('/' == path[length - 1])
			? g_strndup(path, length - 1)
			: g_strdup(path);
	}

	self->session = soup_session_new();
	soup_session_set_timeout(self->session, (guint)self->timeout_seconds);
	soup_session_set_user_agent(self->session,
		"VENTURE/" VENTURE_VERSION_S " ");

	return g_steal_pointer(&self);
}

/* --- Requests ------------------------------------------------------------ */

/*
 * Builds a message for a path under the pinned origin.
 *
 * @path must already have had its variable segments escaped by the caller;
 * this function does not escape, because it cannot tell a caller-supplied
 * segment from a literal one. It does verify the result, which is what
 * catches an escaping mistake before it becomes a request.
 */
static SoupMessage *
venture_forgejo_message(
	VentureForgejoClient	 *self,
	const gchar		 *method,
	const gchar		 *path,
	GError			**error
){
	g_autofree gchar *url = NULL;
	SoupMessage *message;

	if (-1 == self->port)
	{
		url = g_strdup_printf("%s://%s%s/api/v1/%s", self->scheme,
		                      self->host, self->base_path, path);
	}
	else
	{
		url = g_strdup_printf("%s://%s:%d%s/api/v1/%s", self->scheme,
		                      self->host, self->port, self->base_path,
		                      path);
	}

	message = soup_message_new(method, url);

	if (NULL == message)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Cannot address \"%s\" on this forge", path);
		return NULL;
	}

	/*
	 * Redirects are refused rather than followed.
	 *
	 * A 302 carries the Authorization header to wherever the response
	 * pointed, and the response is the least trustworthy input here --
	 * a compromised or merely misconfigured forge could move a request
	 * holding this install's access token to a host it chose. Refusing
	 * means a genuine redirect surfaces as a clear failure the operator
	 * fixes by correcting the base URL, which is where the decision
	 * belongs.
	 */
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (!venture_string_is_empty(self->token))
	{
		g_autofree gchar *value = NULL;

		/* "token", not "Bearer": both work on Forgejo and Gitea, and
		 * this is the spelling their documentation uses. */
		value = g_strdup_printf("token %s", self->token);
		soup_message_headers_replace(
			soup_message_get_request_headers(message),
			"Authorization", value);
	}

	soup_message_headers_replace(soup_message_get_request_headers(message),
	                             "Accept", "application/json");

	return message;
}

/*
 * Turns a forge response into a VentureError.
 *
 * The module VENTURE cribbed its endpoint shapes from reports every non-2xx
 * as a network error, which makes "the token was rejected" and "that
 * repository does not exist" indistinguishable behind a UI that has to tell
 * an operator which one happened.
 */
static void
venture_forgejo_set_status_error(
	VentureForgejoClient	 *self,
	SoupMessage		 *message,
	const gchar		 *what,
	GBytes			 *body,
	GError			**error
){
	guint status;
	const gchar *forge_message = NULL;
	g_autoptr(JsonParser) parser = NULL;

	status = soup_message_get_status(message);

	/* The forge's own explanation, when it sent one. Far more useful for
	 * a 422 than anything this side could invent. */
	if (NULL != body)
	{
		gsize length = 0;
		const gchar *data = g_bytes_get_data(body, &length);

		if ((NULL != data) && (length > 0))
		{
			parser = json_parser_new();

			if (json_parser_load_from_data(parser, data, (gssize)length,
			                               NULL))
			{
				JsonNode *root = json_parser_get_root(parser);

				if ((NULL != root) &&
				    (JSON_NODE_OBJECT == json_node_get_node_type(root)))
				{
					forge_message = venture_json_object_get_string(
						json_node_get_object(root), "message", NULL);
				}
			}
		}
	}

	switch (status)
	{
	case SOUP_STATUS_UNAUTHORIZED:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		            "%s rejected this install's access token", self->host);
		return;

	case SOUP_STATUS_FORBIDDEN:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		            "The access token is not allowed to %s", what);
		return;

	case SOUP_STATUS_NOT_FOUND:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "%s has no such thing (while trying to %s)",
		            self->host, what);
		return;

	case SOUP_STATUS_CONFLICT:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "That already exists (while trying to %s)", what);
		return;

	case 422:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s refused: %s", what,
		            (NULL != forge_message) ? forge_message
		                                    : "the request was not valid");
		return;

	case 429:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "%s is rate limiting; try again later", self->host);
		return;

	default:
		break;
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
	            "%s returned %u while trying to %s", self->host, status, what);
}

/*
 * Sends a request and parses the response.
 *
 * @what names the attempt in the present tense -- "create an issue" -- so it
 * reads correctly in every error message above.
 */
static JsonNode *
venture_forgejo_send(
	VentureForgejoClient	 *self,
	const gchar		 *method,
	const gchar		 *path,
	JsonNode		 *request_body,
	const gchar		 *what,
	guint			 *out_status,
	GError			**error
){
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) response = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(JsonParser) parser = NULL;
	guint status;
	gsize length = 0;
	const gchar *data;

	if (NULL != out_status)
		*out_status = 0;

	message = venture_forgejo_message(self, method, path, error);

	if (NULL == message)
		return NULL;

	if (NULL != request_body)
	{
		g_autofree gchar *text = NULL;
		g_autoptr(GBytes) payload = NULL;

		text = json_to_string(request_body, FALSE);
		payload = g_bytes_new(text, strlen(text));
		soup_message_set_request_body_from_bytes(message, "application/json",
		                                         payload);
	}

	response = soup_session_send_and_read(self->session, message, NULL,
	                                      &local_error);

	if (NULL == response)
	{
		/*
		 * The transport failure message can carry the URL, and a
		 * misconfigured base URL can carry credentials in its
		 * authority. Redacted before it reaches a log or a page.
		 */
		g_autofree gchar *safe = NULL;

		safe = venture_string_redact_uri((NULL != local_error)
			? local_error->message : "the connection failed");

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
		            "Cannot reach %s: %s", self->host, safe);
		return NULL;
	}

	/*
	 * Redirects are disabled, so the final URI cannot differ from the one
	 * built above. This check is what catches a future change that turns
	 * them back on: without it, that change would silently reintroduce
	 * the credential-forwarding hole rather than failing a test.
	 */
	{
		GUri *final_uri = soup_message_get_uri(message);

		if ((0 != g_strcmp0(g_uri_get_host(final_uri), self->host)) ||
		    (g_uri_get_port(final_uri) != self->port))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
			            "The request to %s was redirected to %s; refusing "
			            "to send this install's token there",
			            self->host, g_uri_get_host(final_uri));
			return NULL;
		}
	}

	status = soup_message_get_status(message);

	if (NULL != out_status)
		*out_status = status;

	if ((status < 200) || (status >= 300))
	{
		venture_forgejo_set_status_error(self, message, what, response,
		                                 error);
		return NULL;
	}

	data = g_bytes_get_data(response, &length);

	/* A 204, or any success with nothing in it. Not every call returns a
	 * document, and treating that as a parse failure would make a
	 * successful comment look broken. */
	if ((NULL == data) || (0 == length))
		return NULL;

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, data, (gssize)length,
	                                &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "%s sent a reply that is not JSON: %s", self->host,
		            (NULL != local_error) ? local_error->message : "unknown");
		return NULL;
	}

	return json_node_ref(json_parser_get_root(parser));
}

/* Escapes one path segment. Nothing reserved is left alone, so a value
 * carrying a slash becomes %2F rather than another segment. */
static gchar *
venture_forgejo_escape(const gchar *segment)
{
	return g_uri_escape_string(segment, NULL, FALSE);
}

/*
 * Builds the `repos/<owner>/<repo>` prefix every call shares.
 *
 * Splitting is done by venture_forge_repo_split(), which refuses a name with
 * an extra slash or a traversal segment outright, and each half is escaped
 * afterwards. Both steps matter: the split is what makes the shape exact,
 * the escaping is what makes a hostile value inert.
 */
static gchar *
venture_forgejo_repo_prefix(
	const gchar	 *repo_full_name,
	GError		**error
){
	g_autofree gchar *owner = NULL;
	g_autofree gchar *repo = NULL;
	g_autofree gchar *owner_escaped = NULL;
	g_autofree gchar *repo_escaped = NULL;

	if (!venture_forge_repo_split(repo_full_name, &owner, &repo))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a valid owner/repo name",
		            (NULL != repo_full_name) ? repo_full_name : "");
		return NULL;
	}

	owner_escaped = venture_forgejo_escape(owner);
	repo_escaped = venture_forgejo_escape(repo);

	return g_strdup_printf("repos/%s/%s", owner_escaped, repo_escaped);
}

/* --- Interface ----------------------------------------------------------- */

static gchar *
venture_forgejo_whoami(
	VentureForgeClient	 *client,
	GError			**error
){
	VentureForgejoClient *self = VENTURE_FORGEJO_CLIENT(client);
	g_autoptr(JsonNode) node = NULL;

	node = venture_forgejo_send(self, SOUP_METHOD_GET, "user", NULL,
	                           "identify the access token", NULL, error);

	if (NULL == node)
		return NULL;

	if (JSON_NODE_OBJECT != json_node_get_node_type(node))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "The forge did not describe the token's account");
		return NULL;
	}

	return g_strdup(venture_json_object_get_string(json_node_get_object(node),
	                                               "login", NULL));
}

static gboolean
venture_forgejo_check_repository(
	VentureForgeClient	 *client,
	const gchar		 *repo_full_name,
	gchar			**out_default_branch,
	GError			**error
){
	VentureForgejoClient *self = VENTURE_FORGEJO_CLIENT(client);
	g_autofree gchar *prefix = NULL;
	g_autoptr(JsonNode) node = NULL;

	if (NULL != out_default_branch)
		*out_default_branch = NULL;

	prefix = venture_forgejo_repo_prefix(repo_full_name, error);

	if (NULL == prefix)
		return FALSE;

	node = venture_forgejo_send(self, SOUP_METHOD_GET, prefix, NULL,
	                           "look the repository up", NULL, error);

	if (NULL == node)
		return FALSE;

	if ((NULL != out_default_branch) &&
	    (JSON_NODE_OBJECT == json_node_get_node_type(node)))
	{
		*out_default_branch = g_strdup(venture_json_object_get_string(
			json_node_get_object(node), "default_branch", NULL));
	}

	return TRUE;
}

static gboolean
venture_forgejo_create_issue(
	VentureForgeClient	 *client,
	const gchar		 *repo_full_name,
	const gchar		 *title,
	const gchar		 *body,
	const gchar *const	 *labels,
	gint64			 *out_number,
	gchar			**out_url,
	GError			**error
){
	VentureForgejoClient *self = VENTURE_FORGEJO_CLIENT(client);
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object;

	if (NULL != out_number)
		*out_number = 0;

	if (NULL != out_url)
		*out_url = NULL;

	prefix = venture_forgejo_repo_prefix(repo_full_name, error);

	if (NULL == prefix)
		return FALSE;

	path = g_strdup_printf("%s/issues", prefix);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, (NULL != title) ? title : "");
	json_builder_set_member_name(builder, "body");
	json_builder_add_string_value(builder, (NULL != body) ? body : "");

	if ((NULL != labels) && (NULL != labels[0]))
	{
		gsize i;

		/*
		 * Forgejo's issue creation takes label ids, not names, so a
		 * name sent here is silently ignored rather than refused.
		 * Labels are applied by name in a second call after the issue
		 * exists; sending them here as well would be dead weight.
		 */
		json_builder_set_member_name(builder, "labels");
		json_builder_begin_array(builder);

		for (i = 0; NULL != labels[i]; i++)
			(void)labels[i];

		json_builder_end_array(builder);
	}

	json_builder_end_object(builder);
	request = json_builder_get_root(builder);

	node = venture_forgejo_send(self, SOUP_METHOD_POST, path, request,
	                           "create an issue", NULL, error);

	if (NULL == node)
		return FALSE;

	if (JSON_NODE_OBJECT != json_node_get_node_type(node))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "The forge did not describe the issue it created");
		return FALSE;
	}

	object = json_node_get_object(node);

	if (NULL != out_number)
		*out_number = venture_json_object_get_int(object, "number", 0);

	if (NULL != out_url)
	{
		*out_url = g_strdup(venture_json_object_get_string(object,
		                                                   "html_url", NULL));
	}

	return TRUE;
}

static gboolean
venture_forgejo_update_issue(
	VentureForgeClient	 *client,
	const gchar		 *repo_full_name,
	gint64			  number,
	const gchar		 *title,
	const gchar		 *body,
	const gchar		 *state,
	GError			**error
){
	VentureForgejoClient *self = VENTURE_FORGEJO_CLIENT(client);
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) node = NULL;

	prefix = venture_forgejo_repo_prefix(repo_full_name, error);

	if (NULL == prefix)
		return FALSE;

	path = g_strdup_printf("%s/issues/%" G_GINT64_FORMAT, prefix, number);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	/* Only what the caller actually asked to change. A PATCH carrying an
	 * empty body because the caller only meant to close the issue is how
	 * somebody's text gets thrown away. */
	if (NULL != title)
	{
		json_builder_set_member_name(builder, "title");
		json_builder_add_string_value(builder, title);
	}

	if (NULL != body)
	{
		json_builder_set_member_name(builder, "body");
		json_builder_add_string_value(builder, body);
	}

	if (NULL != state)
	{
		json_builder_set_member_name(builder, "state");
		json_builder_add_string_value(builder, state);
	}

	json_builder_end_object(builder);
	request = json_builder_get_root(builder);

	node = venture_forgejo_send(self, "PATCH", path, request,
	                           "update an issue", NULL, error);

	return (NULL == error) || (NULL == *error);
}

static gboolean
venture_forgejo_comment_issue(
	VentureForgeClient	 *client,
	const gchar		 *repo_full_name,
	gint64			  number,
	const gchar		 *body,
	GError			**error
){
	VentureForgejoClient *self = VENTURE_FORGEJO_CLIENT(client);
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) node = NULL;

	prefix = venture_forgejo_repo_prefix(repo_full_name, error);

	if (NULL == prefix)
		return FALSE;

	path = g_strdup_printf("%s/issues/%" G_GINT64_FORMAT "/comments", prefix,
	                       number);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "body");
	json_builder_add_string_value(builder, (NULL != body) ? body : "");
	json_builder_end_object(builder);
	request = json_builder_get_root(builder);

	node = venture_forgejo_send(self, SOUP_METHOD_POST, path, request,
	                           "comment on an issue", NULL, error);

	return (NULL == error) || (NULL == *error);
}

static gboolean
venture_forgejo_branch_exists(
	VentureForgeClient	 *client,
	const gchar		 *repo_full_name,
	const gchar		 *branch,
	gboolean		 *out_exists,
	GError			**error
){
	VentureForgejoClient *self = VENTURE_FORGEJO_CLIENT(client);
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) local_error = NULL;

	g_return_val_if_fail(NULL != out_exists, FALSE);

	*out_exists = FALSE;

	prefix = venture_forgejo_repo_prefix(repo_full_name, error);

	if (NULL == prefix)
		return FALSE;

	escaped = venture_forgejo_escape(branch);
	path = g_strdup_printf("%s/branches/%s", prefix, escaped);

	node = venture_forgejo_send(self, SOUP_METHOD_GET, path, NULL,
	                           "look a branch up", NULL, &local_error);

	if (NULL != local_error)
	{
		/* Absent is an answer, not a failure. Every other error is a
		 * failure and must not be reported as "no such branch", or a
		 * rejected token would look like a missing branch and the
		 * caller would helpfully try to create it. */
		if (g_error_matches(local_error, VENTURE_ERROR,
		                    VENTURE_ERROR_NOT_FOUND))
			return TRUE;

		g_propagate_error(error, g_steal_pointer(&local_error));
		return FALSE;
	}

	*out_exists = TRUE;

	return TRUE;
}

static gboolean
venture_forgejo_create_branch(
	VentureForgeClient	 *client,
	const gchar		 *repo_full_name,
	const gchar		 *branch,
	const gchar		 *from_branch,
	GError			**error
){
	VentureForgejoClient *self = VENTURE_FORGEJO_CLIENT(client);
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) node = NULL;

	/*
	 * Checked here as well as wherever the name was composed. This is the
	 * last point before the name reaches the forge, and a branch name is
	 * the one value on this path that can come from a webhook payload --
	 * which is to say, from anybody who can open an issue upstream.
	 */
	if (!venture_forge_refname_is_valid(branch))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a usable branch name",
		            (NULL != branch) ? branch : "");
		return FALSE;
	}

	prefix = venture_forgejo_repo_prefix(repo_full_name, error);

	if (NULL == prefix)
		return FALSE;

	path = g_strdup_printf("%s/branches", prefix);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "new_branch_name");
	json_builder_add_string_value(builder, branch);

	if (!venture_string_is_empty(from_branch))
	{
		json_builder_set_member_name(builder, "old_branch_name");
		json_builder_add_string_value(builder, from_branch);
	}

	json_builder_end_object(builder);
	request = json_builder_get_root(builder);

	node = venture_forgejo_send(self, SOUP_METHOD_POST, path, request,
	                           "create a branch", NULL, error);

	return (NULL == error) || (NULL == *error);
}

static gboolean
venture_forgejo_create_pull_request(
	VentureForgeClient	 *client,
	const gchar		 *repo_full_name,
	const gchar		 *title,
	const gchar		 *body,
	const gchar		 *head,
	const gchar		 *base,
	gboolean		  draft,
	gint64			 *out_number,
	gchar			**out_url,
	GError			**error
){
	VentureForgejoClient *self = VENTURE_FORGEJO_CLIENT(client);
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object;

	if (NULL != out_number)
		*out_number = 0;

	if (NULL != out_url)
		*out_url = NULL;

	prefix = venture_forgejo_repo_prefix(repo_full_name, error);

	if (NULL == prefix)
		return FALSE;

	path = g_strdup_printf("%s/pulls", prefix);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, (NULL != title) ? title : "");
	json_builder_set_member_name(builder, "body");
	json_builder_add_string_value(builder, (NULL != body) ? body : "");
	json_builder_set_member_name(builder, "head");
	json_builder_add_string_value(builder, head);
	json_builder_set_member_name(builder, "base");
	json_builder_add_string_value(builder, base);
	/* Nothing here ever merges. A draft is a place to look at the work,
	 * which is the entire point of the outcome. */
	json_builder_set_member_name(builder, "draft");
	json_builder_add_boolean_value(builder, draft);
	json_builder_end_object(builder);
	request = json_builder_get_root(builder);

	node = venture_forgejo_send(self, SOUP_METHOD_POST, path, request,
	                           "open a pull request", NULL, error);

	if (NULL == node)
		return FALSE;

	if (JSON_NODE_OBJECT != json_node_get_node_type(node))
		return TRUE;

	object = json_node_get_object(node);

	if (NULL != out_number)
		*out_number = venture_json_object_get_int(object, "number", 0);

	if (NULL != out_url)
	{
		*out_url = g_strdup(venture_json_object_get_string(object,
		                                                   "html_url", NULL));
	}

	return TRUE;
}

/* --- Webhooks ------------------------------------------------------------ */

static gboolean
venture_forgejo_verify_webhook(
	VentureForgeClient	 *client,
	SoupMessageHeaders	 *headers,
	GBytes			 *body,
	const gchar		 *secret,
	GError			**error
){
	g_autofree gchar *computed = NULL;
	const gchar *presented;
	gconstpointer data;
	gsize length = 0;

	(void)client;

	/*
	 * No secret is a refusal, not a pass.
	 *
	 * The module this scheme was taken from returns TRUE when no secret
	 * is configured, which turns an unconfigured forge into an
	 * unauthenticated endpoint that creates records and can start an AI
	 * run. Failing closed is the whole reason this is reimplemented here
	 * rather than reused.
	 */
	if (venture_string_is_empty(secret))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		                    "This forge has no webhook secret set");
		return FALSE;
	}

	if (NULL == body)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		                    "The request had no body to verify");
		return FALSE;
	}

	presented = soup_message_headers_get_one(headers, "X-Forgejo-Signature");

	if (venture_string_is_empty(presented))
		presented = soup_message_headers_get_one(headers, "X-Gitea-Signature");

	/* An absent signature is a rejection. Treating it as "nothing to
	 * check" is the same hole as an empty secret. */
	if (venture_string_is_empty(presented))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		                    "The request carried no signature");
		return FALSE;
	}

	data = g_bytes_get_data(body, &length);

	computed = g_compute_hmac_for_data(G_CHECKSUM_SHA256,
	                                   (const guchar *)secret, strlen(secret),
	                                   (const guchar *)data, length);

	/*
	 * Constant time, not g_ascii_strcasecmp.
	 *
	 * A comparison that returns as soon as two bytes differ leaks the
	 * digest one byte at a time to anybody who can measure the reply, and
	 * a webhook endpoint is by definition reachable by whoever is
	 * guessing. g_compute_hmac_for_data returns lowercase hex, which is
	 * what Forgejo sends, so a case-insensitive compare was never needed
	 * in the first place.
	 */
	if (!venture_constant_time_equal(computed, presented))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		                    "The signature does not match");
		return FALSE;
	}

	return TRUE;
}

static GStrv
venture_forgejo_labels(JsonObject *issue)
{
	g_autoptr(GPtrArray) names = NULL;
	JsonArray *array;

	names = g_ptr_array_new();

	if (!json_object_has_member(issue, "labels"))
	{
		g_ptr_array_add(names, NULL);
		return (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);
	}

	array = json_object_get_array_member(issue, "labels");

	if (NULL != array)
	{
		guint i;

		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonObject *label = json_array_get_object_element(array, i);
			const gchar *name;

			if (NULL == label)
				continue;

			name = venture_json_object_get_string(label, "name", NULL);

			if (!venture_string_is_empty(name))
				g_ptr_array_add(names, g_strdup(name));
		}
	}

	g_ptr_array_add(names, NULL);

	return (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);
}

static gboolean
venture_forgejo_parse_issue_event(
	VentureForgeClient	 *client,
	JsonNode		 *payload,
	SoupMessageHeaders	 *headers,
	VentureForgeEvent	 *out_event,
	GError			**error
){
	JsonObject *root;
	JsonObject *issue;
	JsonObject *repository;
	JsonObject *sender;
	const gchar *delivery;

	(void)client;

	g_return_val_if_fail(NULL != out_event, FALSE);

	memset(out_event, 0, sizeof(*out_event));

	if ((NULL == payload) ||
	    (JSON_NODE_OBJECT != json_node_get_node_type(payload)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The payload is not a JSON object");
		return FALSE;
	}

	root = json_node_get_object(payload);

	issue = json_object_has_member(root, "issue")
		? json_object_get_object_member(root, "issue") : NULL;
	repository = json_object_has_member(root, "repository")
		? json_object_get_object_member(root, "repository") : NULL;
	sender = json_object_has_member(root, "sender")
		? json_object_get_object_member(root, "sender") : NULL;

	if ((NULL == issue) || (NULL == repository))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The payload names no issue or no repository");
		return FALSE;
	}

	out_event->action = g_strdup(venture_json_object_get_string(root, "action",
	                                                            ""));
	out_event->repo_full_name = g_strdup(venture_json_object_get_string(
		repository, "full_name", ""));
	out_event->issue_number = venture_json_object_get_int(issue, "number", 0);
	out_event->title = g_strdup(venture_json_object_get_string(issue, "title",
	                                                           ""));
	out_event->body = g_strdup(venture_json_object_get_string(issue, "body",
	                                                          ""));
	out_event->state = g_strdup(venture_json_object_get_string(issue, "state",
	                                                           "open"));
	out_event->url = g_strdup(venture_json_object_get_string(issue, "html_url",
	                                                         ""));
	out_event->labels = venture_forgejo_labels(issue);

	if (NULL != sender)
	{
		out_event->sender = g_strdup(venture_json_object_get_string(sender,
		                                                            "login", ""));
	}

	if (json_object_has_member(root, "comment"))
	{
		JsonObject *comment = json_object_get_object_member(root, "comment");

		if (NULL != comment)
		{
			out_event->comment_body = g_strdup(
				venture_json_object_get_string(comment, "body", ""));
		}
	}

	delivery = soup_message_headers_get_one(headers, "X-Forgejo-Delivery");

	if (venture_string_is_empty(delivery))
		delivery = soup_message_headers_get_one(headers, "X-Gitea-Delivery");

	out_event->delivery_id = g_strdup((NULL != delivery) ? delivery : "");

	return TRUE;
}

/*
 * The delivery id, from whichever header this forge sends it in.
 */
static gchar *
venture_forgejo_delivery_id(SoupMessageHeaders *headers)
{
	const gchar *delivery;

	delivery = soup_message_headers_get_one(headers, "X-Forgejo-Delivery");

	if (venture_string_is_empty(delivery))
		delivery = soup_message_headers_get_one(headers, "X-Gitea-Delivery");

	return g_strdup((NULL != delivery) ? delivery : "");
}

/*
 * A workflow_run delivery, as Forgejo Actions and Gitea Actions send it:
 * the run under "workflow_run", its workflow under "workflow", and the
 * repository and sender beside them.
 */
static gboolean
venture_forgejo_parse_workflow_event(
	VentureForgeClient		 *client,
	JsonNode			 *payload,
	SoupMessageHeaders		 *headers,
	VentureForgeWorkflowEvent	 *out_event,
	GError				**error
){
	JsonObject *root;
	JsonObject *run;
	JsonObject *workflow;
	JsonObject *repository;
	JsonObject *sender;

	(void)client;

	g_return_val_if_fail(NULL != out_event, FALSE);

	memset(out_event, 0, sizeof(*out_event));

	if ((NULL == payload) ||
	    (JSON_NODE_OBJECT != json_node_get_node_type(payload)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The payload is not a JSON object");
		return FALSE;
	}

	root = json_node_get_object(payload);

	run = json_object_has_member(root, "workflow_run")
		? json_object_get_object_member(root, "workflow_run") : NULL;
	workflow = json_object_has_member(root, "workflow")
		? json_object_get_object_member(root, "workflow") : NULL;
	repository = json_object_has_member(root, "repository")
		? json_object_get_object_member(root, "repository") : NULL;
	sender = json_object_has_member(root, "sender")
		? json_object_get_object_member(root, "sender") : NULL;

	if ((NULL == run) || (NULL == repository))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The payload names no workflow run or no "
		                    "repository");
		return FALSE;
	}

	out_event->action = g_strdup(venture_json_object_get_string(root, "action",
	                                                            ""));
	out_event->repo_full_name = g_strdup(venture_json_object_get_string(
		repository, "full_name", ""));
	out_event->run_id = venture_json_object_get_int(run, "id", 0);
	out_event->run_number = venture_json_object_get_int(run, "run_number", 0);
	out_event->title = g_strdup(venture_json_object_get_string(
		run, "display_title", ""));
	out_event->head_branch = g_strdup(venture_json_object_get_string(
		run, "head_branch", ""));
	out_event->head_sha = g_strdup(venture_json_object_get_string(
		run, "head_sha", ""));
	out_event->status = g_strdup(venture_json_object_get_string(run, "status",
	                                                            ""));
	out_event->conclusion = g_strdup(venture_json_object_get_string(
		run, "conclusion", ""));
	out_event->url = g_strdup(venture_json_object_get_string(run, "html_url",
	                                                         ""));
	out_event->started_at = g_strdup(venture_json_object_get_string(
		run, "started_at", NULL));
	out_event->finished_at = g_strdup(venture_json_object_get_string(
		run, "updated_at", NULL));

	/* The workflow's own name where the payload carries the workflow,
	 * else the run's; the two agree on a normal delivery. */
	out_event->workflow_name = g_strdup(venture_json_object_get_string(
		(NULL != workflow) ? workflow : run, "name", ""));

	if (venture_string_is_empty(out_event->workflow_name))
	{
		g_free(out_event->workflow_name);
		out_event->workflow_name = g_strdup(
			venture_json_object_get_string(run, "name", ""));
	}

	if (NULL != sender)
		out_event->sender = g_strdup(venture_json_object_get_string(
			sender, "login", ""));
	else
		out_event->sender = g_strdup("");

	out_event->delivery_id = venture_forgejo_delivery_id(headers);

	if (0 == out_event->run_id)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The workflow run has no id");
		venture_forge_workflow_event_clear(out_event);
		return FALSE;
	}

	return TRUE;
}

/*
 * A release delivery: the release under "release", the repository and the
 * sender beside it.
 */
static gboolean
venture_forgejo_parse_release_event(
	VentureForgeClient		 *client,
	JsonNode			 *payload,
	SoupMessageHeaders		 *headers,
	VentureForgeReleaseEvent	 *out_event,
	GError				**error
){
	JsonObject *root;
	JsonObject *release;
	JsonObject *repository;
	JsonObject *sender;

	(void)client;

	g_return_val_if_fail(NULL != out_event, FALSE);

	memset(out_event, 0, sizeof(*out_event));

	if ((NULL == payload) ||
	    (JSON_NODE_OBJECT != json_node_get_node_type(payload)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The payload is not a JSON object");
		return FALSE;
	}

	root = json_node_get_object(payload);

	release = json_object_has_member(root, "release")
		? json_object_get_object_member(root, "release") : NULL;
	repository = json_object_has_member(root, "repository")
		? json_object_get_object_member(root, "repository") : NULL;
	sender = json_object_has_member(root, "sender")
		? json_object_get_object_member(root, "sender") : NULL;

	if ((NULL == release) || (NULL == repository))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The payload names no release or no repository");
		return FALSE;
	}

	out_event->action = g_strdup(venture_json_object_get_string(root, "action",
	                                                            ""));
	out_event->repo_full_name = g_strdup(venture_json_object_get_string(
		repository, "full_name", ""));
	out_event->release_id = venture_json_object_get_int(release, "id", 0);
	out_event->tag = g_strdup(venture_json_object_get_string(release,
	                                                         "tag_name", ""));
	out_event->name = g_strdup(venture_json_object_get_string(release, "name",
	                                                          ""));
	out_event->body = g_strdup(venture_json_object_get_string(release, "body",
	                                                          ""));
	out_event->url = g_strdup(venture_json_object_get_string(release,
	                                                         "html_url", ""));
	out_event->published_at = g_strdup(venture_json_object_get_string(
		release, "published_at", NULL));
	out_event->draft = venture_json_object_get_bool(release, "draft", FALSE);
	out_event->prerelease = venture_json_object_get_bool(release, "prerelease",
	                                                     FALSE);

	if (NULL != sender)
		out_event->sender = g_strdup(venture_json_object_get_string(
			sender, "login", ""));
	else
		out_event->sender = g_strdup("");

	out_event->delivery_id = venture_forgejo_delivery_id(headers);

	if (venture_string_is_empty(out_event->tag))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The release has no tag");
		venture_forge_release_event_clear(out_event);
		return FALSE;
	}

	return TRUE;
}

/*
 * POST /repos/{owner}/{repo}/releases. Forgejo creates the tag at
 * target_commitish if it does not exist, which is why a release can be
 * published from VENTURE before anybody tagged anything.
 */
static gboolean
venture_forgejo_create_release(
	VentureForgeClient	 *client,
	const gchar		 *repo_full_name,
	const gchar		 *tag,
	const gchar		 *target,
	const gchar		 *name,
	const gchar		 *body,
	gboolean		  draft,
	gboolean		  prerelease,
	gint64			 *out_id,
	gchar			**out_url,
	GError			**error
){
	VentureForgejoClient *self = VENTURE_FORGEJO_CLIENT(client);
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object;

	if (NULL != out_id)
		*out_id = 0;

	if (NULL != out_url)
		*out_url = NULL;

	if (venture_string_is_empty(tag))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A release needs a tag");
		return FALSE;
	}

	prefix = venture_forgejo_repo_prefix(repo_full_name, error);

	if (NULL == prefix)
		return FALSE;

	path = g_strdup_printf("%s/releases", prefix);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "tag_name");
	json_builder_add_string_value(builder, tag);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder,
	                              !venture_string_is_empty(name) ? name : tag);
	json_builder_set_member_name(builder, "body");
	json_builder_add_string_value(builder, (NULL != body) ? body : "");
	json_builder_set_member_name(builder, "draft");
	json_builder_add_boolean_value(builder, draft);
	json_builder_set_member_name(builder, "prerelease");
	json_builder_add_boolean_value(builder, prerelease);

	if (!venture_string_is_empty(target))
	{
		json_builder_set_member_name(builder, "target_commitish");
		json_builder_add_string_value(builder, target);
	}

	json_builder_end_object(builder);
	request = json_builder_get_root(builder);

	node = venture_forgejo_send(self, SOUP_METHOD_POST, path, request,
	                           "create a release", NULL, error);

	if (NULL == node)
		return FALSE;

	if (JSON_NODE_OBJECT != json_node_get_node_type(node))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "The forge did not describe the release it "
		                    "created");
		return FALSE;
	}

	object = json_node_get_object(node);

	if (NULL != out_id)
		*out_id = venture_json_object_get_int(object, "id", 0);

	if (NULL != out_url)
		*out_url = g_strdup(venture_json_object_get_string(object, "html_url",
		                                                   ""));

	return TRUE;
}

static void
venture_forgejo_client_iface_init(VentureForgeClientInterface *iface)
{
	iface->whoami = venture_forgejo_whoami;
	iface->check_repository = venture_forgejo_check_repository;
	iface->create_issue = venture_forgejo_create_issue;
	iface->update_issue = venture_forgejo_update_issue;
	iface->comment_issue = venture_forgejo_comment_issue;
	iface->branch_exists = venture_forgejo_branch_exists;
	iface->create_branch = venture_forgejo_create_branch;
	iface->create_pull_request = venture_forgejo_create_pull_request;
	iface->verify_webhook = venture_forgejo_verify_webhook;
	iface->parse_issue_event = venture_forgejo_parse_issue_event;
	iface->parse_workflow_event = venture_forgejo_parse_workflow_event;
	iface->parse_release_event = venture_forgejo_parse_release_event;
	iface->create_release = venture_forgejo_create_release;
}
