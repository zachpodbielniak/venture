/*
 * venture-web-avatars.inc - Profile picture routes
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c. The store, the checks and the badge
 * are src/core/venture-avatar.c; this file is the doors:
 *
 *   GET    /avatar/:id/:size            the picture, to whoever may see the name
 *   PUT    /api/v1/account/avatar       set your own (raw image or multipart "file")
 *   DELETE /api/v1/account/avatar       remove your own
 *   PUT    /api/v1/users/:id/avatar     set anybody's: a global owner
 *   DELETE /api/v1/users/:id/avatar     remove anybody's: a global owner
 *   POST   /account/avatar[/remove]     the account page's forms
 *   POST   /users/:id/avatar[/remove]   the users page's forms
 *
 * Who may change a picture is who may already edit that account: the
 * person themself, and a global owner for anybody (the /users rule). A
 * token acts as its owning user, and a read-only token stays read-only.
 */

/* What a multipart envelope may add around the picture itself. */
#define VENTURE_WEB_AVATAR_ENVELOPE_BYTES (64 * 1024)

/*
 * The transport limit for the picture routes, so a body far past the cap
 * is refused at its headers and never buffered. Zero means "not one of
 * them"; the caller keeps whatever else applies.
 */
static gsize
venture_web_avatar_body_limit(const gchar *path)
{
	const gchar *rest;

	if ((0 == g_strcmp0(path, "/api/v1/account/avatar")) ||
	    (0 == g_strcmp0(path, "/account/avatar")))
		return VENTURE_AVATAR_MAX_BYTES + VENTURE_WEB_AVATAR_ENVELOPE_BYTES;

	if (g_str_has_prefix(path, "/api/v1/users/"))
		rest = path + strlen("/api/v1/users/");
	else if (g_str_has_prefix(path, "/users/"))
		rest = path + strlen("/users/");
	else
		return 0;

	rest = strchr(rest, '/');

	if ((NULL != rest) && (0 == g_strcmp0(rest, "/avatar")))
		return VENTURE_AVATAR_MAX_BYTES + VENTURE_WEB_AVATAR_ENVELOPE_BYTES;

	return 0;
}

/* A path parameter that must be a positive id; 0 when it is not. */
static gint64
venture_web_avatar_id(
	GHashTable	*params,
	const gchar	*name
){
	const gchar *text;
	gint64 id;

	text = (NULL != params) ? g_hash_table_lookup(params, name) : NULL;
	id = 0;

	if ((NULL == text) ||
	    !g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, &id, NULL))
		return 0;

	return id;
}

/*
 * The picture a request carries: the "file" field of a multipart body
 * (what a browser form sends, and what the hosted image upload takes), or
 * else the raw body (what `curl --data-binary @face.png` sends). Neither
 * the file name nor a declared content type is kept: the bytes are sniffed.
 */
static GBytes *
venture_web_avatar_upload(
	HtmxRequest	 *request,
	GError		**error
){
	const gchar *content_type;
	GBytes *body;
	g_autoptr(GPtrArray) files = NULL;
	guint i;

	content_type = htmx_request_get_content_type(request);
	body = htmx_request_get_body_bytes(request);

	if ((NULL != body) &&
	    (g_bytes_get_size(body) > VENTURE_AVATAR_MAX_BYTES + VENTURE_WEB_AVATAR_ENVELOPE_BYTES))
	{
		venture_set_error_validation(error, "Picture",
			"the request is larger than a %d MB picture can be",
			VENTURE_AVATAR_MAX_BYTES / (1024 * 1024));
		return NULL;
	}

	if ((NULL == content_type) || (NULL == strstr(content_type, "multipart/form-data")))
		return (NULL != body) ? g_bytes_ref(body) : g_bytes_new(NULL, 0);

	files = htmx_uploaded_file_parse_multipart(content_type, body, NULL, error);

	if (NULL == files)
	{
		if ((NULL != error) && (NULL == *error))
			venture_set_error_validation(error, "Picture", "no file arrived");

		return NULL;
	}

	for (i = 0; i < files->len; i++)
	{
		HtmxUploadedFile *file = g_ptr_array_index(files, i);

		if (0 == g_strcmp0(htmx_uploaded_file_get_field_name(file), "file"))
			return g_bytes_ref(htmx_uploaded_file_get_data(file));
	}

	venture_set_error_validation(error, "Picture",
		"no file arrived; send it as the multipart field \"file\", or as the "
		"whole request body");

	return NULL;
}

/*
 * Whether @principal may change @user_id's picture: the person themself,
 * or a global owner for anybody -- the rule /users applies to every edit
 * of another person's account. A token acts as its owning user and must
 * be able to write: a read-only token stays read-only.
 */
static gboolean
venture_web_avatar_may_change(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	VentureAuthPrincipal	 *principal,
	gint64			  user_id,
	GError			**error
){
	if (!principal->authenticated)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		                    "Sign in to continue");
		return FALSE;
	}

	if ((0 != principal->token_id) &&
	    !venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, error))
		return FALSE;

	if ((user_id > 0) && (user_id == principal->user_id))
		return TRUE;

	return venture_web_hosted_auth_require(self, request, principal,
	                                       VENTURE_USER_ROLE_OWNER, error);
}

/* What the API answers after a change: the user and their picture now. */
static HtmxResponse *
venture_web_avatar_state(
	VentureWebServer	*self,
	gint64			 user_id,
	const gchar		*version
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	const guint *sizes;
	guint n_sizes;
	guint i;

	(void)self;

	sizes = venture_avatar_sizes(&n_sizes);
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "user_id");
	json_builder_add_int_value(builder, user_id);
	json_builder_set_member_name(builder, "avatar_version");

	if (!venture_string_is_empty(version))
		json_builder_add_string_value(builder, version);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "avatar_url");

	if (!venture_string_is_empty(version))
	{
		g_autofree gchar *url = NULL;

		url = venture_avatar_url(user_id, version, VENTURE_AVATAR_SIZE_SMALL);
		json_builder_add_string_value(builder, url);
	}
	else
	{
		json_builder_add_null_value(builder);
	}

	/* Every size, keyed by pixels, for a client that draws more than the
	 * small badge. Empty with no picture. */
	json_builder_set_member_name(builder, "avatar_urls");
	json_builder_begin_object(builder);

	for (i = 0; !venture_string_is_empty(version) && (i < n_sizes); i++)
	{
		g_autofree gchar *key = NULL;
		g_autofree gchar *url = NULL;

		key = g_strdup_printf("%u", sizes[i]);
		url = venture_avatar_url(user_id, version, sizes[i]);
		json_builder_set_member_name(builder, key);
		json_builder_add_string_value(builder, url);
	}

	json_builder_end_object(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/* PUT or DELETE on @user_id's picture, once the caller is known. */
static HtmxResponse *
venture_web_avatar_api_change(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureAuthPrincipal	*principal,
	gint64			 user_id,
	gboolean		 remove
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) upload = NULL;
	g_autofree gchar *version = NULL;
	VentureActor actor;

	if (!venture_web_avatar_may_change(self, request, principal, user_id, &error))
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);

	if (remove)
	{
		if (!venture_avatar_remove(self->context, user_id, &actor, &error))
			return venture_web_error_response(error);

		return venture_web_avatar_state(self, user_id, NULL);
	}

	upload = venture_web_avatar_upload(request, &error);

	if (NULL == upload)
		return venture_web_error_response(error);

	version = venture_avatar_set(self->context, user_id, upload, &actor, &error);

	if (NULL == version)
	{
		g_message("Picture for user %" G_GINT64_FORMAT " refused: %s", user_id,
		          (NULL != error) ? error->message : "no reason");
		return venture_web_error_response(error);
	}

	return venture_web_avatar_state(self, user_id, version);
}

/* PUT and DELETE /api/v1/account/avatar: the caller's own picture. */
static HtmxResponse *
venture_web_avatar_api_account(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;

	self = user_data;
	(void)params;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_hosted_auth_require(self, request, principal,
	                                     VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	if (principal->user_id <= 0)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"This credential is not tied to a user account, so it has no "
			"picture to change");
		return venture_web_error_response(error);
	}

	return venture_web_avatar_api_change(self, request, principal,
		principal->user_id, HTMX_METHOD_DELETE == htmx_request_get_method(request));
}

/* PUT and DELETE /api/v1/users/:id/avatar: anybody's, for a global owner. */
static HtmxResponse *
venture_web_avatar_api_user(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	gint64 user_id;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_hosted_auth_require(self, request, principal,
	                                     VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	user_id = venture_web_avatar_id(params, "id");

	if (0 == user_id)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "User not found");
		return venture_web_error_response(error);
	}

	return venture_web_avatar_api_change(self, request, principal, user_id,
		HTMX_METHOD_DELETE == htmx_request_get_method(request));
}

/*
 * The notice a page shows for a failed upload, worked out again from the
 * upload so the words match the reason; the full message goes to the log.
 */
static const gchar *
venture_web_avatar_notice(GBytes *upload)
{
	gsize size;
	gint width;
	gint height;

	if (!venture_avatar_is_available())
		return "avatar-unavailable";

	size = (NULL != upload) ? g_bytes_get_size(upload) : 0;

	if (0 == size)
		return "avatar-missing";

	if (size > VENTURE_AVATAR_MAX_BYTES)
		return "avatar-too-large";

	width = 0;
	height = 0;

	if (NULL == venture_avatar_sniff(upload, &width, &height))
		return "avatar-not-image";

	if ((width > VENTURE_AVATAR_MAX_SIDE) || (height > VENTURE_AVATAR_MAX_SIDE))
		return "avatar-dimensions";

	return "avatar-failed";
}

/*
 * The page form's half of a change: set or remove, then back to @back with
 * a notice. Authority was checked by the caller.
 */
static HtmxResponse *
venture_web_avatar_form_change(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureAuthPrincipal	*principal,
	gint64			 user_id,
	gboolean		 remove,
	const gchar		*back
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) upload = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *location = NULL;
	VentureActor actor;

	venture_auth_to_actor(principal, &actor);

	if (remove)
	{
		if (!venture_avatar_remove(self->context, user_id, &actor, &error))
		{
			g_message("Picture for user %" G_GINT64_FORMAT " not removed: %s",
			          user_id, error->message);
			location = g_strconcat(back, "?notice=avatar-failed", NULL);
		}
		else
		{
			location = g_strconcat(back, "?notice=avatar-removed", NULL);
		}

		return venture_web_redirect_to(location);
	}

	upload = venture_web_avatar_upload(request, &error);

	if (NULL != upload)
		version = venture_avatar_set(self->context, user_id, upload, &actor, &error);

	if (NULL == version)
	{
		g_message("Picture for user %" G_GINT64_FORMAT " refused: %s", user_id,
		          (NULL != error) ? error->message : "no reason");
		location = g_strconcat(back, "?notice=",
		                       venture_web_avatar_notice(upload), NULL);
		return venture_web_redirect_to(location);
	}

	location = g_strconcat(back, "?notice=avatar-ok", NULL);

	return venture_web_redirect_to(location);
}

/* POST /account/avatar and /account/avatar/remove. */
static HtmxResponse *
venture_web_avatar_ui_account(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;

	self = user_data;
	(void)params;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if ((principal->user_id <= 0) ||
	    !venture_web_avatar_may_change(self, request, principal,
	                                   principal->user_id, &error))
		return venture_web_redirect_to("/account?notice=avatar-failed");

	return venture_web_avatar_form_change(self, request, principal,
		principal->user_id,
		g_str_has_suffix(htmx_request_get_path(request), "/remove"), "/account");
}

/* POST /users/:id/avatar and /users/:id/avatar/remove: a global owner. */
static HtmxResponse *
venture_web_avatar_ui_user(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	gint64 user_id;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	/* The page itself is the owner's; so is every form on it. */
	if (!venture_web_hosted_auth_require(self, request, principal,
	                                     VENTURE_USER_ROLE_OWNER, &error))
		return venture_web_error_response(error);

	user_id = venture_web_avatar_id(params, "id");

	if (0 == user_id)
		return venture_web_redirect_to("/users?notice=failed");

	return venture_web_avatar_form_change(self, request, principal, user_id,
		g_str_has_suffix(htmx_request_get_path(request), "/remove"), "/users");
}

/*
 * The words for a picture notice on /account or /users, or NULL when
 * @notice is not one of ours. *@ok says which banner it wears.
 */
static const gchar *
venture_web_avatar_notice_text(
	const gchar	*notice,
	gboolean	*ok
){
	static const struct
	{
		const gchar	*code;
		gboolean	 ok;
		const gchar	*text;
	} notices[] = {
		{ "avatar-ok", TRUE, "Picture saved." },
		{ "avatar-removed", TRUE, "Picture removed; the initials are shown instead." },
		{ "avatar-unavailable", FALSE, "This server was built without image support, so it cannot take a picture." },
		{ "avatar-missing", FALSE, "No file arrived. Choose a picture first." },
		{ "avatar-too-large", FALSE, "That file is too large: a picture may be at most 5 MB." },
		{ "avatar-not-image", FALSE, "That is not a PNG, JPEG or WebP picture. SVG, GIF, BMP, TIFF and icon files are not accepted." },
		{ "avatar-dimensions", FALSE, "That picture is too big: at most 4096 pixels on a side." },
		{ "avatar-failed", FALSE, "That picture could not be read or saved. Try a PNG or JPEG." },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(notices); i++)
	{
		if (0 == g_strcmp0(notice, notices[i].code))
		{
			*ok = notices[i].ok;
			return notices[i].text;
		}
	}

	return NULL;
}

/*
 * The account page's picture section: the picture as others see it, and
 * the forms that change it. @viewer is the page's names cache.
 */
static void
venture_web_avatar_account_section(
	GString			*content,
	VentureActorNames	*viewer,
	gint64			 user_id,
	const gchar		*name
){
	g_autofree gchar *picture = NULL;
	gboolean has_picture;

	has_picture = (NULL != venture_actor_names_avatar_version(viewer, user_id));
	picture = venture_avatar_html(viewer, user_id, name, VENTURE_AVATAR_SIZE_LARGE);

	g_string_append(content, "<h2>Picture</h2><div class=\"avatar-edit\">");
	g_string_append(content, picture);
	g_string_append(content, "<div class=\"avatar-edit-forms\">");

	if (!venture_avatar_is_available())
	{
		g_string_append(content, "<p class=\"muted small\">This server was built "
		                         "without image support, so everybody is shown "
		                         "by their initials.</p></div></div>");
		return;
	}

	g_string_append_printf(content,
		"<form method=\"post\" action=\"/account/avatar\" "
		"enctype=\"multipart/form-data\" data-no-inline>"
		"<label>A PNG, JPEG or WebP picture, at most %d MB"
		"<input type=\"file\" name=\"file\" required "
		"accept=\"image/png,image/jpeg,image/webp\"></label>"
		"<button class=\"btn\" type=\"submit\">%s picture</button></form>",
		VENTURE_AVATAR_MAX_BYTES / (1024 * 1024),
		has_picture ? "Replace" : "Upload");

	if (has_picture)
		g_string_append(content,
			"<form method=\"post\" action=\"/account/avatar/remove\" data-no-inline>"
			"<button class=\"btn btn-ghost\" type=\"submit\">Remove picture"
			"</button></form>");

	g_string_append(content,
		"<p class=\"muted small\">The middle square is kept, at a few small "
		"sizes; the file itself and everything in it besides the picture "
		"(location, camera details) is discarded.</p></div></div>");
}

/* The users page's picture cell for one person: set, replace or remove. */
static void
venture_web_avatar_users_cell(
	GString		*content,
	gint64		 user_id,
	gboolean	 has_picture
){
	if (!venture_avatar_is_available())
	{
		g_string_append(content, "<span class=\"muted small\">unavailable</span>");
		return;
	}

	g_string_append_printf(content,
		"<form method=\"post\" action=\"/users/%" G_GINT64_FORMAT "/avatar\" "
		"enctype=\"multipart/form-data\" class=\"inline-form\" data-no-inline>"
		"<input type=\"file\" name=\"file\" required "
		"accept=\"image/png,image/jpeg,image/webp\" aria-label=\"Picture\">"
		"<button class=\"btn btn-sm\" type=\"submit\">%s</button></form>",
		user_id, has_picture ? "Replace" : "Set");

	if (has_picture)
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/users/%" G_GINT64_FORMAT
			"/avatar/remove\" class=\"inline-form\" data-no-inline>"
			"<button class=\"btn btn-sm btn-ghost\" type=\"submit\">Remove"
			"</button></form>", user_id);
}

/* An answer that says nothing: not found, whatever the reason was. */
static HtmxResponse *
venture_web_avatar_missing(void)
{
	HtmxResponse *response;

	response = htmx_response_new_with_content("Not found");
	htmx_response_set_status(response, 404);
	htmx_response_set_content_type(response, "text/plain; charset=utf-8");
	htmx_response_add_header(response, "Cache-Control", "private, no-store");
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");

	return response;
}

/*
 * GET /avatar/:id/:size. Signed in (a session or a token), and answered
 * only to a reader who may already see the person's name
 * (venture_actor_names_may_see_user()): everybody else, an unknown size,
 * no picture and no such account are the same 404, so the address says
 * nothing about who exists.
 */
static HtmxResponse *
venture_web_avatar_serve(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureActorNames) names = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *etag = NULL;
	const gchar *size_text;
	const gchar *asked;
	const gchar *cache;
	const gchar *if_none_match;
	HtmxResponse *response;
	guint64 size;
	gint64 user_id;

	self = user_data;

	response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != response)
		return response;

	user_id = venture_web_avatar_id(params, "id");
	size_text = g_hash_table_lookup(params, "size");
	size = 0;

	if ((0 == user_id) || (NULL == size_text) ||
	    !g_ascii_string_to_unsigned(size_text, 10, 1, G_MAXUINT, &size, NULL) ||
	    !venture_avatar_size_is_valid((guint)size))
		return venture_web_avatar_missing();

	/* Made in the request's scope: its reader is the caller. */
	names = venture_actor_names_new(venture_context_get_database(self->context));

	if (!venture_actor_names_may_see_user(names, user_id))
		return venture_web_avatar_missing();

	bytes = venture_avatar_read(self->context, user_id, (guint)size, &version,
	                            &error);

	if (NULL == bytes)
		return venture_web_avatar_missing();

	etag = g_strdup_printf("\"%s-%u\"", version, (guint)size);
	asked = htmx_request_get_query_param(request, "v");

	/* The versioned address never changes what it names, so the browser
	 * keeps it for a year; any other address is the current picture and
	 * is checked each time. "private" either way: it is behind a sign-in,
	 * and no shared cache may keep it. */
	cache = (0 == g_strcmp0(asked, version))
		? "private, max-age=31536000, immutable"
		: "private, no-cache";

	if_none_match = soup_message_headers_get_one(
		soup_server_message_get_request_headers(htmx_request_get_message(request)),
		"If-None-Match");

	response = htmx_response_new();

	if (0 == g_strcmp0(if_none_match, etag))
	{
		htmx_response_set_status(response, 304);
	}
	else
	{
		htmx_response_set_status(response, 200);
		htmx_response_set_bytes(response, bytes);
		htmx_response_set_content_type(response, "image/png");
	}

	htmx_response_add_header(response, "ETag", etag);
	htmx_response_add_header(response, "Cache-Control", cache);
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
	htmx_response_add_header(response, "Content-Security-Policy",
	                         "default-src 'none'; sandbox");

	return response;
}

/*
 * Before the generic /api/v1/:type/:id routes, which would otherwise read
 * "account/avatar" as a record of type "account".
 */
static void
venture_web_avatars_register(VentureWebServer *self)
{
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/avatar/:id/:size",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_avatar_serve, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_PUT, "/api/v1/account/avatar",
		VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_web_avatar_api_account, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_DELETE, "/api/v1/account/avatar",
		VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_web_avatar_api_account, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_PUT, "/api/v1/users/:id/avatar",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_avatar_api_user, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_DELETE, "/api/v1/users/:id/avatar",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_avatar_api_user, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/account/avatar",
		VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_web_avatar_ui_account, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/account/avatar/remove",
		VENTURE_DATA_CLASS_PERSONAL, VENTURE_HOSTED_ROUTE_CONTROL, venture_web_avatar_ui_account, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/users/:id/avatar",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_avatar_ui_user, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/users/:id/avatar/remove",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_avatar_ui_user, self);
}
