/*
 * venture-forms-web.inc - The forms module's public door and builder panel
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c. The routes here take no session: a
 * stranger's browser is the caller. They parse the request, pick HTML or
 * JSON, and leave every decision about the form to venture-forms.c.
 *
 * Every refusal that could tell a stranger something -- a draft, a closed
 * form, a full one, a wrong token, the module switched off -- is the one
 * forms_web_missing() page, byte for byte.
 */

/* A form's hourly bucket, rebuilt when its limit is changed. */
typedef struct
{
	gint64		 limit;
	HtmxRateLimiter	*limiter;
} FormsWebBucket;

static void
forms_web_bucket_free(gpointer data)
{
	FormsWebBucket *bucket = data;

	g_clear_object(&bucket->limiter);
	g_free(bucket);
}

static const gchar *
forms_web_header(HtmxRequest *request, const gchar *name)
{
	SoupMessageHeaders *headers;

	headers = soup_server_message_get_request_headers(htmx_request_get_message(request));
	return soup_message_headers_get_one(headers, name);
}

/* The Origin a browser sent, or NULL. "null" is what a sandboxed frame or a
 * file:// page sends; it names no site, so no allowed list can include it. */
static const gchar *
forms_web_origin(HtmxRequest *request)
{
	const gchar *origin = forms_web_header(request, "Origin");

	if (NULL == origin || '\0' == origin[0] || strlen(origin) > 2048)
		return NULL;
	return origin;
}

/* Whether @origin is this server itself: the hosted page posting to its own
 * address is always allowed, whatever sites the form names. */
static gboolean
forms_web_same_origin(HtmxRequest *request, const gchar *origin)
{
	GUri *uri = soup_server_message_get_uri(htmx_request_get_message(request));
	g_autoptr(GUri) sent = NULL;
	const gchar *host = forms_web_header(request, "Host");
	g_autofree gchar *authority = NULL;

	sent = g_uri_parse(origin, G_URI_FLAGS_NONE, NULL);
	if (NULL == sent || NULL == g_uri_get_host(sent))
		return FALSE;
	authority = (g_uri_get_port(sent) > 0) ?
		g_strdup_printf("%s:%d", g_uri_get_host(sent), g_uri_get_port(sent)) :
		g_strdup(g_uri_get_host(sent));
	if (NULL != host && 0 == g_ascii_strcasecmp(host, authority))
		return TRUE;
	return NULL != uri && 0 == g_ascii_strcasecmp(g_uri_get_host(uri) != NULL ? g_uri_get_host(uri) : "", g_uri_get_host(sent)) &&
	       g_uri_get_port(uri) == g_uri_get_port(sent);
}

static gboolean
forms_web_origin_allowed(HtmxRequest *request, VentureEntity *form, const gchar *origin)
{
	if (NULL == origin)
		return TRUE;
	if (forms_web_same_origin(request, origin))
		return TRUE;
	return venture_forms_origin_allowed(form, origin);
}

static void
forms_web_common(HtmxResponse *response, const gchar *allow_origin)
{
	htmx_response_add_header(response, "Cache-Control", "no-store, private");
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
	htmx_response_add_header(response, "Referrer-Policy", "strict-origin-when-cross-origin");
	if (NULL != allow_origin)
	{
		htmx_response_add_header(response, "Access-Control-Allow-Origin", allow_origin);
		htmx_response_add_header(response, "Vary", "Origin");
	}
}

/* The hosted page may be framed by the sites allowed to use the form, or
 * by any when none are named; it may post to itself and follow the form's
 * redirect; it runs no script at all. */
static void
forms_web_page_policy(HtmxResponse *response, VentureEntity *form)
{
	g_autoptr(GString) policy = g_string_new("default-src 'none'; style-src 'unsafe-inline'; "
	                                         "img-src 'self' data:; form-action 'self' https: http:; "
	                                         "base-uri 'none'; frame-ancestors");
	g_autofree gchar *origins = NULL;

	g_object_get(form, "allowed-origins", &origins, NULL);
	if (venture_string_is_empty(origins))
		g_string_append(policy, " *");
	else
	{
		g_auto(GStrv) parts = NULL;
		guint i;

		g_strdelimit(origins, ",\t\r ", '\n');
		parts = g_strsplit(origins, "\n", -1);
		g_string_append(policy, " 'self'");
		for (i = 0; NULL != parts[i]; i++)
			if ('\0' != parts[i][0] && NULL == strpbrk(parts[i], ";'\""))
				g_string_append_printf(policy, " %s", parts[i]);
	}
	htmx_response_add_header(response, "Content-Security-Policy", policy->str);
}

/* Compact JSON: these answers are read by scripts on other sites, not
 * people, and every byte crosses somebody else's page load. */
static HtmxResponse *
forms_web_json(JsonNode *node, guint status)
{
	g_autofree gchar *body = json_to_string(node, FALSE);
	HtmxResponse *response = htmx_response_new_with_content(body);

	htmx_response_set_content_type(response, "application/json; charset=utf-8");
	htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
forms_web_missing(void)
{
	HtmxResponse *response = htmx_response_new_with_content(
		"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Not found</title>"
		"</head><body><p>This form is not available.</p></body></html>");

	htmx_response_set_status(response, 404);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	forms_web_common(response, NULL);
	return response;
}

static HtmxResponse *
forms_web_status(guint status, const gchar *text)
{
	HtmxResponse *response = htmx_response_new_with_content(text);

	htmx_response_set_status(response, status);
	htmx_response_set_content_type(response, "text/plain; charset=utf-8");
	forms_web_common(response, NULL);
	return response;
}

static HtmxResponse *
forms_web_html(VentureEntity *form, gchar *html, guint status, const gchar *allow_origin, gboolean page)
{
	HtmxResponse *response = htmx_response_new_with_content(html);

	g_free(html);
	htmx_response_set_status(response, status);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	forms_web_common(response, allow_origin);
	if (page)
		forms_web_page_policy(response, form);
	return response;
}

/* The live form a public path names, or NULL when the door is shut. */
static VentureEntity *
forms_web_live(VentureWebServer *self, GHashTable *params, GDateTime *now)
{
	if (!venture_web_module_enabled(self, "forms"))
		return NULL;
	return venture_forms_find_live(venture_context_get_database(self->context),
		g_hash_table_lookup(params, "token"), now, NULL);
}

static gchar *
forms_web_action(VentureWebServer *self, VentureEntity *form, gboolean absolute)
{
	g_autofree gchar *token = NULL;
	g_autofree gchar *base = NULL;

	g_object_get(form, "public-token", &token, NULL);
	if (absolute && !venture_string_is_empty(self->base_url))
	{
		base = g_strdup(self->base_url);
		while (g_str_has_suffix(base, "/"))
			base[strlen(base) - 1] = '\0';
	}
	return g_strdup_printf("%s/pub/form/%s", base != NULL ? base : "", token);
}

static HtmxResponse *
venture_web_forms_script(HtmxRequest *request, GHashTable *params, gpointer data)
{
	HtmxResponse *response = htmx_response_new_with_content(venture_asset_forms_js);

	(void)request;
	(void)params;
	(void)data;
	htmx_response_set_content_type(response, "application/javascript; charset=utf-8");
	htmx_response_add_header(response, "Cache-Control", "public, max-age=300");
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
	htmx_response_add_header(response, "Access-Control-Allow-Origin", "*");
	return response;
}

/* GET /pub/form/:token/fragment -- the bare form the script loader fetches
 * into a page. Absolute action, so it posts here from wherever it lands. */
static HtmxResponse *
venture_web_forms_fragment(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureEntity) form = forms_web_live(self, params, now);
	g_autoptr(VentureEntity) version = NULL;
	g_autofree gchar *ticket = NULL, *action = NULL;
	const gchar *origin = forms_web_origin(request);
	VentureFormsRender options;
	gchar *html;

	if (NULL == form)
		return forms_web_missing();
	if (!forms_web_origin_allowed(request, form, origin))
		return forms_web_status(403, "This site may not use this form.");
	version = venture_forms_published_version(venture_context_get_database(self->context), form, NULL);
	ticket = venture_forms_ticket_new(form, now);
	action = forms_web_action(self, form, TRUE);
	options.mode = VENTURE_FORMS_RENDER_FRAGMENT;
	options.action = action;
	options.ticket = ticket;
	options.values = NULL;
	options.errors = NULL;
	options.version = version;
	html = venture_forms_render(venture_context_get_database(self->context), form, &options, NULL);
	if (NULL == html)
		return forms_web_missing();
	return forms_web_html(form, html, 200, origin, FALSE);
}

/* GET /pub/form/:token/schema -- the form as JSON for a site's own framework. */
static HtmxResponse *
venture_web_forms_schema(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureEntity) form = forms_web_live(self, params, now);
	g_autoptr(JsonNode) schema = NULL;
	g_autofree gchar *action = NULL;
	const gchar *origin = forms_web_origin(request);
	HtmxResponse *response;

	if (NULL == form)
		return forms_web_missing();
	if (!forms_web_origin_allowed(request, form, origin))
		return forms_web_status(403, "This site may not use this form.");
	action = forms_web_action(self, form, TRUE);
	schema = venture_forms_schema(venture_context_get_database(self->context), form, action, now, NULL);
	if (NULL == schema)
		return forms_web_missing();
	response = forms_web_json(schema, 200);
	forms_web_common(response, origin);
	return response;
}

static gboolean
forms_web_wants_json(HtmxRequest *request, gboolean json_body)
{
	const gchar *accept = forms_web_header(request, "Accept");

	if (json_body)
		return TRUE;
	return NULL != accept && NULL != strstr(accept, "application/json") && NULL == strstr(accept, "text/html");
}

static HtmxResponse *
forms_web_json_result(gboolean ok, const gchar *message, const gchar *redirect, JsonObject *errors,
	guint status, const gchar *allow_origin)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	HtmxResponse *response;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "ok");
	json_builder_add_boolean_value(builder, ok);
	json_builder_set_member_name(builder, "message");
	json_builder_add_string_value(builder, message);
	if (NULL != redirect)
	{
		json_builder_set_member_name(builder, "redirect");
		json_builder_add_string_value(builder, redirect);
	}
	if (NULL != errors)
	{
		json_builder_set_member_name(builder, "errors");
		json_builder_add_value(builder, json_node_init_object(json_node_alloc(), errors));
	}
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	response = forms_web_json(root, status);
	forms_web_common(response, allow_origin);
	return response;
}

/* What a person sees once a response is in -- or once a robot is told it is. */
static HtmxResponse *
forms_web_accepted(VentureEntity *form, gboolean json, const gchar *allow_origin)
{
	g_autofree gchar *redirect = NULL, *message = NULL;

	g_object_get(form, "redirect-url", &redirect, "success-message", &message, NULL);
	if (venture_string_is_empty(message))
	{
		g_free(message);
		message = g_strdup("Thank you. Your response has been received.");
	}
	if (json)
		return forms_web_json_result(TRUE, message, venture_string_is_empty(redirect) ? NULL : redirect,
		                             NULL, 200, allow_origin);
	if (!venture_string_is_empty(redirect))
	{
		/* 303, so the browser follows with a GET rather than re-posting. */
		HtmxResponse *response = htmx_response_new_with_content("");

		htmx_response_set_status(response, 303);
		htmx_response_add_header(response, "Location", redirect);
		forms_web_common(response, allow_origin);
		return response;
	}
	return forms_web_html(form, venture_forms_render_success(form, TRUE), 200, allow_origin, TRUE);
}

/* Reads the body into answers, by content type. JSON may come as text/plain
 * because that is the one JSON a cross-origin fetch can send with no
 * preflight, and this server answers none. */
static GHashTable *
forms_web_answers(HtmxRequest *request, gboolean *json_body, guint *status, GError **error)
{
	GBytes *bytes = htmx_request_get_body_bytes(request);
	const gchar *type = htmx_request_get_content_type(request);
	const gchar *data = NULL;
	gsize length = 0;

	*json_body = FALSE;
	*status = 422;
	if (NULL != bytes)
		data = g_bytes_get_data(bytes, &length);
	if (NULL == data)
		data = "";
	if (NULL != type && g_str_has_prefix(type, "application/x-www-form-urlencoded"))
		return venture_forms_answers_from_urlencoded(data, length, error);
	if (NULL != type && (g_str_has_prefix(type, "application/json") || g_str_has_prefix(type, "text/plain")))
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonNode *root;

		*json_body = TRUE;
		if (NULL != memchr(data, '\0', length) || !json_parser_load_from_data(parser, data, (gssize)length, NULL))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "The answers are not valid JSON");
			return NULL;
		}
		root = json_parser_get_root(parser);
		if (NULL == root || !JSON_NODE_HOLDS_OBJECT(root))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "The answers must be a JSON object");
			return NULL;
		}
		return venture_forms_answers_from_json(json_node_get_object(root), error);
	}
	*status = 415;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
	                    "Send the answers as a form post or as JSON");
	return NULL;
}

static gboolean
forms_web_rate_allowed(VentureWebServer *self, VentureEntity *form, const gchar *token, const gchar *address)
{
	FormsWebBucket *bucket;
	gint64 limit = 0;

	g_object_get(form, "hourly-limit", &limit, NULL);
	if (limit <= 0)
		limit = 600;
	if (NULL == self->form_client_limiter)
		self->form_client_limiter = htmx_rate_limiter_new(20, 20.0 / 60.0);
	if (!htmx_rate_limiter_allow(self->form_client_limiter, address != NULL ? address : "unknown"))
		return FALSE;
	if (NULL == self->form_limiters)
		self->form_limiters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, forms_web_bucket_free);
	bucket = g_hash_table_lookup(self->form_limiters, token);
	if (NULL == bucket || bucket->limit != limit)
	{
		bucket = g_new0(FormsWebBucket, 1);
		bucket->limit = limit;
		bucket->limiter = htmx_rate_limiter_new((guint)MIN(limit, G_MAXUINT), (gdouble)limit / 3600.0);
		g_hash_table_replace(self->form_limiters, g_strdup(token), bucket);
	}
	return htmx_rate_limiter_allow(bucket->limiter, token);
}

/* GET and POST /pub/form/:token -- the hosted page, and where every way in
 * posts its answers. */
static HtmxResponse *
venture_web_forms_public(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(GHashTable) answers = NULL;
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *address = NULL, *action = NULL;
	const gchar *origin = forms_web_origin(request);
	const gchar *token = g_hash_table_lookup(params, "token");
	const gchar *allow_origin;
	GBytes *bytes;
	VentureFormsOutcome outcome;
	gboolean json_body = FALSE, json;
	guint status = 422;

	if (HTMX_METHOD_POST != htmx_request_get_method(request))
	{
		g_autofree gchar *ticket = NULL;
		g_autoptr(VentureEntity) version = NULL;
		const gchar *style = htmx_request_get_query_param(request, "style");
		VentureFormsRender options;
		gchar *html;

		form = forms_web_live(self, params, now);
		if (NULL == form)
			return forms_web_missing();
		version = venture_forms_published_version(database, form, NULL);
		ticket = venture_forms_ticket_new(form, now);
		options.mode = (0 == g_strcmp0(style, "basic")) ? VENTURE_FORMS_RENDER_HOSTED_BASIC : VENTURE_FORMS_RENDER_HOSTED;
		options.action = NULL;
		options.ticket = ticket;
		options.values = NULL;
		options.errors = NULL;
		options.version = version;
		html = venture_forms_render(database, form, &options, NULL);
		if (NULL == html)
			return forms_web_missing();
		return forms_web_html(form, html, 200, NULL, TRUE);
	}

	/* Refuse the expensive things before doing them: size before parsing,
	 * the door before the rate, the rate before the database write. */
	bytes = htmx_request_get_body_bytes(request);
	if (NULL != bytes && g_bytes_get_size(bytes) > VENTURE_FORMS_MAX_BODY)
		return forms_web_status(413, "Too much data for one response.");
	form = forms_web_live(self, params, now);
	if (NULL == form)
		return forms_web_missing();
	if (!forms_web_origin_allowed(request, form, origin))
		return forms_web_status(403, "This site may not use this form.");
	allow_origin = origin;
	address = venture_auth_remote_address(request);
	if (!forms_web_rate_allowed(self, form, token, address))
	{
		HtmxResponse *response = forms_web_status(429, "Too many responses; try again later.");

		htmx_response_add_header(response, "Retry-After", "60");
		return response;
	}

	answers = forms_web_answers(request, &json_body, &status, &error);
	json = forms_web_wants_json(request, json_body);
	if (NULL == answers)
	{
		if (json)
		{
			g_autoptr(JsonObject) problem = json_object_new();

			json_object_set_string_member(problem, "_form", error->message);
			return forms_web_json_result(FALSE, error->message, NULL, problem, status, allow_origin);
		}
		return forms_web_status(status, error->message);
	}

	/* A robot is told it succeeded. Telling it why it did not would teach
	 * it which of the two guards to get past. */
	if (!venture_forms_screen(form, answers, now))
		return forms_web_accepted(form, json, allow_origin);

	if (!venture_forms_submit(database, form, answers, origin, now, &outcome, NULL, &errors, &error))
	{
		if (g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
			return forms_web_missing();
		return forms_web_status(500, "The response could not be saved; please try again.");
	}
	if (VENTURE_FORMS_ACCEPTED == outcome || VENTURE_FORMS_DISCARDED == outcome)
		return forms_web_accepted(form, json, allow_origin);

	if (json)
		return forms_web_json_result(FALSE, "Please correct the answers marked below.", NULL, errors, 422, allow_origin);

	/* No script: show the form again with the messages beside the answers
	 * and the answers still in the boxes, under the ticket it came with. */
	{
		g_autoptr(JsonObject) values = venture_forms_answers_to_json(answers);
		g_autoptr(VentureEntity) version = venture_forms_version_for_answers(database, form, answers);
		GPtrArray *posted = g_hash_table_lookup(answers, VENTURE_FORMS_TICKET);
		VentureFormsRender options;
		gchar *html;

		/* The questions the person was answering, which after a publish
		 * mid-way are not necessarily today's. */
		options.mode = VENTURE_FORMS_RENDER_HOSTED;
		options.action = NULL;
		options.ticket = (NULL != posted && posted->len > 0) ? g_ptr_array_index(posted, 0) : NULL;
		options.values = values;
		options.errors = errors;
		options.version = version;
		html = venture_forms_render(database, form, &options, NULL);
		if (NULL == html)
			return forms_web_missing();
		return forms_web_html(form, html, 422, allow_origin, TRUE);
	}
}

/* ==========================================================================
 * The builder panel on a form's page
 * ========================================================================== */

static void
forms_web_copy_block(GString *content, const gchar *id, const gchar *title, const gchar *help, const gchar *code)
{
	g_string_append_printf(content, "<div class=\"field\"><label for=\"%s\">", id);
	venture_html_escape_append(content, title);
	g_string_append(content, "</label><p class=\"field-help\">");
	venture_html_escape_append(content, help);
	g_string_append_printf(content, "</p><textarea id=\"%s\" rows=\"4\" readonly spellcheck=\"false\">", id);
	venture_html_escape_append(content, code);
	/* The page's existing copy buttons read the payload from data-copy;
	 * the textarea beside it is what to select where the clipboard is not
	 * available. */
	g_string_append(content, "</textarea><button class=\"btn btn-sm\" type=\"button\" data-copy=\"");
	venture_html_escape_append(content, code);
	g_string_append(content, "\">Copy</button></div>");
}

/* A response's answers, labelled by the version of the questions it
 * answered, on its own page. */
static void
venture_web_append_form_answers(VentureWebServer *self, GString *content, VentureEntity *record)
{
	g_autofree gchar *answers = venture_forms_render_answers(venture_context_get_database(self->context),
		record, NULL);

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Answers</h2></div>"
	                         "<div class=\"card-body\">");
	if (NULL != answers)
		g_string_append(content, answers);
	g_string_append(content, "</div></div>");
}

static void
venture_web_append_form_block(VentureWebServer *self, GString *content, VentureEntity *record)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(GPtrArray) fields = venture_forms_fields(database, record, NULL);
	g_autoptr(VentureQuery) responses = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
	g_autofree gchar *base = NULL, *hosted = NULL;
	g_autofree gchar *snippet = NULL, *script = NULL, *iframe = NULL, *schema = NULL, *preview = NULL;
	VentureFormState state = VENTURE_FORM_DRAFT;
	VentureFormsRender options;
	gint64 id = venture_entity_get_id(record);
	gint64 count;
	guint i;

	g_object_get(record, "state", &state, NULL);
	base = g_strdup(!venture_string_is_empty(self->base_url) ? self->base_url : "");
	while (g_str_has_suffix(base, "/"))
		base[strlen(base) - 1] = '\0';
	hosted = forms_web_action(self, record, TRUE);
	venture_query_set_organization(responses, venture_entity_get_organization_id(record));
	venture_query_add_filter_int(responses, "form-id", VENTURE_FILTER_OP_EQ, id, NULL);
	count = venture_database_count(database, responses, NULL);

	/* --- What the public sees --- */

	{
		g_autoptr(VentureEntity) published = venture_forms_published_version(database, record, NULL);
		gboolean changed = venture_forms_has_unpublished_changes(database, record);

		g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Publishing</h2></div>"
		                         "<div class=\"card-body\">");
		if (NULL == published)
			g_string_append(content, "<p>Not published yet. Nothing is public until you publish.</p>");
		else
		{
			g_autoptr(GDateTime) at = NULL;
			g_autofree gchar *when = NULL;
			gint64 number = 0;

			g_object_get(published, "number", &number, "published-at", &at, NULL);
			when = (NULL != at) ? g_date_time_format(at, "%Y-%m-%d %H:%M") : g_strdup("");
			g_string_append_printf(content, "<p>The public sees <a href=\"/e/form_version/%" G_GINT64_FORMAT
			                       "\">version %" G_GINT64_FORMAT "</a>, published ", venture_entity_get_id(published), number);
			venture_html_escape_append(content, when);
			g_string_append(content, " UTC.</p>");
		}
		if (changed)
			g_string_append(content, "<p class=\"notice\">The questions have changed since then. "
			                         "Answers already received keep the questions they were given.</p>");
		g_string_append_printf(content, "<form method=\"post\" action=\"/forms/%" G_GINT64_FORMAT "/publish\">"
		                       "<button class=\"btn btn-primary\" type=\"submit\"%s>Publish</button> "
		                       "<a href=\"/e/form_version?form_id=%" G_GINT64_FORMAT "\">All versions</a></form>"
		                       "</div></div>", id, changed ? "" : " disabled", id);
	}

	/* --- Questions, in the order asked --- */

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Questions</h2></div>"
	                         "<div class=\"card-body\">");
	if (NULL == fields || 0 == fields->len)
		g_string_append(content, "<p class=\"muted\">No questions yet.</p>");
	else
	{
		g_string_append(content, "<table><thead><tr><th>Order</th><th>Question</th><th>Kind</th>"
		                         "<th>Required</th></tr></thead><tbody>");
		for (i = 0; i < fields->len; i++)
		{
			VentureEntity *field = g_ptr_array_index(fields, i);
			g_autofree gchar *label = NULL, *kind = NULL;
			VentureFormFieldKind kind_value = VENTURE_FORM_FIELD_SHORT_TEXT;
			gint64 position = 0;
			gboolean required = FALSE;

			g_object_get(field, "label", &label, "kind", &kind_value, "position", &position,
			             "required", &required, NULL);
			kind = venture_web_label_from_name(venture_enum_to_nick(VENTURE_TYPE_FORM_FIELD_KIND, (gint)kind_value));
			g_string_append_printf(content, "<tr><td>%" G_GINT64_FORMAT "</td><td><a href=\"/e/form_field/%"
			                       G_GINT64_FORMAT "\">", position, venture_entity_get_id(field));
			venture_html_escape_append(content, label);
			g_string_append(content, "</a></td><td>");
			venture_html_escape_append(content, kind);
			g_string_append_printf(content, "</td><td>%s</td></tr>", required ? "Yes" : "No");
		}
		g_string_append(content, "</tbody></table>");
	}
	g_string_append_printf(content, "<p><a class=\"btn btn-sm\" href=\"/e/form_field/new?form_id=%" G_GINT64_FORMAT
	                       "&amp;organization_id=%" G_GINT64_FORMAT "\">Add a question</a></p></div></div>",
	                       id, venture_entity_get_organization_id(record));

	/* --- Responses --- */

	g_string_append_printf(content, "<div class=\"card\"><div class=\"card-head\"><h2>Responses</h2></div>"
	                       "<div class=\"card-body\"><p>%" G_GINT64_FORMAT " received. "
	                       "<a href=\"/e/form_submission?form_id=%" G_GINT64_FORMAT "\">Read them</a> or "
	                       "<a href=\"/reports/form_summary?form_id=%" G_GINT64_FORMAT "&amp;period=all\">see the summary</a>.</p>"
	                       "</div></div>", count, id, id);

	/* --- Preview, from the one renderer --- */

	options.mode = VENTURE_FORMS_RENDER_PREVIEW;
	options.action = NULL;
	options.ticket = NULL;
	options.values = NULL;
	options.errors = NULL;
	options.version = NULL;
	preview = venture_forms_render(database, record, &options, NULL);
	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Preview</h2></div>"
	                         "<div class=\"card-body\"><p class=\"field-help\">Unstyled here, as it arrives on "
	                         "another site: that site's styles decide how it looks.</p><div class=\"form-preview\">");
	if (NULL != preview)
		g_string_append(content, preview);
	g_string_append(content, "</div></div></div>");

	/* --- The ways onto another site --- */

	snippet = venture_forms_embed_code(database, record, base, VENTURE_FORMS_EMBED_HTML, NULL);
	script = venture_forms_embed_code(database, record, base, VENTURE_FORMS_EMBED_SCRIPT, NULL);
	iframe = venture_forms_embed_code(database, record, base, VENTURE_FORMS_EMBED_IFRAME, NULL);
	schema = venture_forms_embed_code(database, record, base, VENTURE_FORMS_EMBED_SCHEMA, NULL);
	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Put it on a site</h2></div>"
	                         "<div class=\"card-body\">");
	if (VENTURE_FORM_LIVE != state)
		g_string_append(content, "<p class=\"notice\">This form is not live, so these answer "
		                         "&ldquo;not available&rdquo; until you set its state to live.</p>");
	if (venture_string_is_empty(base))
		g_string_append(content, "<p class=\"notice\">Set server.base_url so these codes carry this "
		                         "server's full address; without it they only work on this server's own pages.</p>");
	g_string_append(content, "<p>Hosted page: <a href=\"");
	venture_html_escape_append(content, hosted);
	g_string_append(content, "\">");
	venture_html_escape_append(content, hosted);
	g_string_append(content, "</a></p>");
	forms_web_copy_block(content, "form-embed-html", "HTML",
		"Paste into any page. Works with JavaScript off; the page's CSS styles it.",
		snippet != NULL ? snippet : "");
	forms_web_copy_block(content, "form-embed-script", "Script",
		"Loads the form into the page and shows errors in place.", script != NULL ? script : "");
	forms_web_copy_block(content, "form-embed-iframe", "Frame",
		"For a site that can take neither. Add ?style=basic to the address for a minimal layout.",
		iframe != NULL ? iframe : "");
	forms_web_copy_block(content, "form-embed-schema", "JSON schema",
		"For a site that renders the form itself and posts the answers.", schema != NULL ? schema : "");
	g_string_append(content, "</div></div>");
}

/* POST /forms/:id/publish -- the builder's Publish button. The same work
 * as the publish action, which the API, the CLI and the assistant use. */
static HtmxResponse *
venture_web_forms_publish(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(VentureEntity) version = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	gint64 id;

	gate = venture_web_ui_require_session(self, request);
	if (NULL != gate)
		return gate;
	gate = venture_web_require_module_ui(self, request, "forms");
	if (NULL != gate)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	form = venture_database_get(database, VENTURE_TYPE_FORM, id, &error);
	if (NULL == form)
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	if (!venture_access_policy_check_write(venture_database_get_access_policy(database), form, "write", &error))
		return venture_web_error_response(error);
	version = venture_forms_publish(database, form, &actor, &error);
	if (NULL == version)
		return venture_web_error_response(error);
	path = g_strdup_printf("/e/form/%" G_GINT64_FORMAT, id);
	return venture_web_redirect_to(path);
}
