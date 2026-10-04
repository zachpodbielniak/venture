/*
 * venture-web-comments.inc - The discussion on every record's page
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c. A comment names its record by type and
 * id, so nothing here is derived from a field table: the panel, its routes
 * and its API are written by hand against the one type, the way a ticket
 * relation's panel is. The decisions -- who may read, write, edit, delete,
 * who is told -- are all in src/core/venture-comment.c and the access
 * policy; this file only draws them and carries requests to them.
 *
 * Every page is rendered from the same JSON the API returns
 * (venture_comment_thread()), so the panel and the API cannot disagree
 * about what a reader may see or do.
 *
 * The panel works with scripting off: a comment is an ordinary form post
 * that answers with a redirect to the comment's anchor; a reply box and an
 * edit box are <details>; a permalink is a link. venture.js enhances it --
 * write and preview tabs, the toolbar, shortcuts, the @ menu, posting in
 * place -- and asks for the re-rendered panel with X-Venture-Discussion.
 */

/* A header from the request, or NULL. */
static const gchar *
venture_web_comments_header(
	HtmxRequest	*request,
	const gchar	*name
){
	SoupServerMessage *message;

	message = htmx_request_get_message(request);

	if (NULL == message)
		return NULL;

	return soup_message_headers_get_one(
		soup_server_message_get_request_headers(message), name);
}

/*
 * The record a request names, if the caller may open its page: the type's
 * own gate first (users, tokens and webhooks are the owner's), then the
 * record under the caller's scope, which answers not found for anything
 * they may not read.
 */
static VentureEntity *
venture_web_comments_subject(
	VentureWebServer	 *self,
	VentureAuthPrincipal	 *principal,
	const gchar		 *subject_type,
	gint64			  subject_id,
	GError			**error
){
	GType type;

	type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(self->context),
		(NULL != subject_type) ? subject_type : "");

	if ((G_TYPE_INVALID == type) || (subject_id <= 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "No such record");
		return NULL;
	}

	if (!venture_web_require_for_type(self, principal, type,
	                                  VENTURE_USER_ROLE_VIEWER, error))
		return NULL;

	{
		GError *local = NULL;
		VentureEntity *subject;

		/* A missing row is NULL with no error; a refusal must say why. */
		subject = venture_database_get(venture_context_get_database(self->context),
		                               type, subject_id, &local);

		if ((NULL == subject) && (NULL == local))
			g_set_error_literal(&local, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			                    "No such record");

		if (NULL != local)
			g_propagate_error(error, local);

		return subject;
	}
}

/*
 * A comment the caller may reach, with its record's gate applied as well:
 * a comment is a piece of its record.
 */
static VentureEntity *
venture_web_comments_load(
	VentureWebServer	 *self,
	VentureAuthPrincipal	 *principal,
	gint64			  comment_id,
	VentureEntity		**subject_out,
	GError			**error
){
	g_autoptr(VentureEntity) comment = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autofree gchar *subject_type = NULL;
	gint64 subject_id = 0;

	comment = venture_comment_get(self->context, principal, comment_id, error);

	if (NULL == comment)
		return NULL;

	g_object_get(comment, "subject-type", &subject_type,
	             "subject-id", &subject_id, NULL);
	subject = venture_web_comments_subject(self, principal, subject_type,
	                                       subject_id, error);

	if (NULL == subject)
		return NULL;

	if (NULL != subject_out)
		*subject_out = g_steal_pointer(&subject);

	return g_steal_pointer(&comment);
}

/* --- Drawing --------------------------------------------------------------- */

/* "3 hours ago", with the exact time in the configured zone on hover. */
static void
venture_web_comments_time(
	VentureWebServer	*self,
	GString			*html,
	const gchar		*iso
){
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(GDateTime) local = NULL;
	g_autofree gchar *relative = NULL;
	g_autofree gchar *exact = NULL;

	when = (NULL != iso) ? venture_time_from_string(iso, NULL) : NULL;

	if (NULL == when)
		return;

	local = g_date_time_to_timezone(when, venture_context_get_timezone(self->context));
	relative = venture_time_to_relative_string(when);
	exact = g_date_time_format(local, "%Y-%m-%d %H:%M %Z");

	g_string_append(html, "<time datetime=\"");
	venture_html_escape_append(html, iso);
	g_string_append(html, "\" title=\"");
	venture_html_escape_append(html, exact);
	g_string_append(html, "\">");
	venture_html_escape_append(html, relative);
	g_string_append(html, "</time>");
}

/*
 * One composer: a new thread, a reply, or an edit. The bar holding the
 * Write and Preview tabs and the toolbar is drawn hidden and shown by the
 * script, because without the script none of it would do anything.
 */
static void
venture_web_comments_composer(
	GString		*html,
	const gchar	*action,
	const gchar	*subject_type,
	gint64		 subject_id,
	gint64		 parent_id,
	const gchar	*input_id,
	const gchar	*label,
	const gchar	*placeholder,
	const gchar	*text,
	const gchar	*submit,
	gboolean	 cancellable
){
	g_string_append(html,
		"<form class=\"discussion-composer\" method=\"post\" action=\"");
	venture_html_escape_append(html, action);
	g_string_append(html, "\" data-discussion-form>");

	if (NULL != subject_type)
	{
		g_string_append(html, "<input type=\"hidden\" name=\"subject_type\" value=\"");
		venture_html_escape_append(html, subject_type);
		g_string_append_printf(html,
			"\"><input type=\"hidden\" name=\"subject_id\" value=\"%"
			G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"parent_id\" "
			"value=\"%" G_GINT64_FORMAT "\">", subject_id, parent_id);
	}

	g_string_append(html,
		"<div class=\"discussion-bar\" data-discussion-bar hidden>"
		"<div class=\"discussion-tabs\" role=\"tablist\" aria-label=\"Composer\">"
		"<button type=\"button\" role=\"tab\" class=\"discussion-tab\" "
		"aria-selected=\"true\" data-discussion-tab=\"write\">Write</button>"
		"<button type=\"button\" role=\"tab\" class=\"discussion-tab\" "
		"aria-selected=\"false\" data-discussion-tab=\"preview\">Preview</button>"
		"</div>"
		"<div class=\"discussion-tools\" role=\"toolbar\" aria-label=\"Formatting\" data-discussion-tools>"
		"<button type=\"button\" class=\"discussion-tool\" data-discussion-md=\"bold\" "
		"aria-label=\"Bold\" title=\"Bold (Ctrl+B)\"><b>B</b></button>"
		"<button type=\"button\" class=\"discussion-tool\" data-discussion-md=\"italic\" "
		"aria-label=\"Italic\" title=\"Italic (Ctrl+I)\"><i>I</i></button>"
		"<button type=\"button\" class=\"discussion-tool\" data-discussion-md=\"code\" "
		"aria-label=\"Code\" title=\"Code\">" VENTURE_ICON(
			"<path d=\"M8 6l-6 6 6 6\"/><path d=\"M16 6l6 6-6 6\"/>")
		"</button>"
		"<button type=\"button\" class=\"discussion-tool\" data-discussion-md=\"link\" "
		"aria-label=\"Link\" title=\"Link (Ctrl+K)\">" VENTURE_ICON(
			"<path d=\"M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1\"/>"
			"<path d=\"M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1\"/>")
		"</button>"
		"<button type=\"button\" class=\"discussion-tool\" data-discussion-md=\"list\" "
		"aria-label=\"List\" title=\"List\">" VENTURE_ICON(
			"<path d=\"M9 6h11\"/><path d=\"M9 12h11\"/><path d=\"M9 18h11\"/>"
			"<path d=\"M4 6h.01\"/><path d=\"M4 12h.01\"/><path d=\"M4 18h.01\"/>")
		"</button>"
		"<button type=\"button\" class=\"discussion-tool\" data-discussion-md=\"quote\" "
		"aria-label=\"Quote\" title=\"Quote\">" VENTURE_ICON(
			"<path d=\"M4 6v12\"/><path d=\"M9 8h11\"/><path d=\"M9 12h11\"/>"
			"<path d=\"M9 16h7\"/>")
		"</button>"
		"<button type=\"button\" class=\"discussion-tool\" data-discussion-md=\"mention\" "
		"aria-label=\"Mention somebody\" title=\"Mention somebody\">@</button>"
		"</div></div>");

	g_string_append(html, "<div class=\"discussion-field\"><label class=\"visually-hidden\" for=\"");
	venture_html_escape_append(html, input_id);
	g_string_append(html, "\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</label><textarea id=\"");
	venture_html_escape_append(html, input_id);
	g_string_append_printf(html,
		"\" name=\"body\" rows=\"3\" maxlength=\"%d\" required "
		"autocomplete=\"off\" data-discussion-input placeholder=\"",
		VENTURE_COMMENT_MAX_LENGTH);
	venture_html_escape_append(html, placeholder);
	g_string_append(html, "\">");
	venture_html_escape_append(html, text);
	g_string_append(html,
		"</textarea>"
		"<div class=\"discussion-preview markdown\" data-discussion-preview "
		"hidden aria-live=\"polite\"></div>"
		"<div class=\"discussion-menu\" data-discussion-menu hidden "
		"role=\"listbox\" aria-label=\"People\"></div></div>"
		"<div class=\"discussion-foot\"><span class=\"discussion-hint\">"
		"Markdown works &middot; <kbd>@</kbd>name tells somebody</span>"
		"<span class=\"discussion-buttons\">");

	if (cancellable)
		g_string_append(html,
			"<button type=\"button\" class=\"btn btn-ghost btn-sm\" "
			"data-discussion-cancel hidden>Cancel</button>");

	g_string_append(html, "<button class=\"btn btn-primary btn-sm\" type=\"submit\">");
	venture_html_escape_append(html, submit);
	g_string_append(html, "</button></span></div></form>");
}

/* One comment, drawn from its JSON. */
static void
venture_web_comments_article(
	VentureWebServer	*self,
	GString			*html,
	JsonObject		*comment,
	VentureAuthPrincipal	*principal,
	gint64			 thread_id,
	gboolean		 reply
){
	const gchar *name;
	const gchar *author;
	const gchar *username;
	const gchar *role;
	const gchar *url;
	const gchar *edited;
	gint64 id;
	gint64 author_user_id;
	gboolean deleted;
	gboolean own;

	id = venture_json_object_get_int(comment, "id", 0);
	name = venture_json_object_get_string(comment, "author_name", "Somebody");
	author = venture_json_object_get_string(comment, "author", NULL);
	username = venture_json_object_get_string(comment, "author_username", NULL);
	role = venture_json_object_get_string(comment, "author_role", NULL);
	url = venture_json_object_get_string(comment, "url", "#");
	edited = venture_json_object_get_string(comment, "edited_at", NULL);
	author_user_id = venture_json_object_get_int(comment, "author_user_id", 0);
	deleted = venture_json_object_get_bool(comment, "deleted", FALSE);
	own = (NULL != principal) && (author_user_id == principal->user_id) &&
	      ((principal->user_id > 0) || (0 == g_strcmp0(author, principal->name)));

	g_string_append_printf(html,
		"<article class=\"discussion-comment%s%s%s\" id=\"comment-%" G_GINT64_FORMAT
		"\" data-comment-id=\"%" G_GINT64_FORMAT "\">",
		reply ? " is-reply" : "", deleted ? " is-deleted" : "",
		own ? " is-own" : "", id, id);

	/* The avatar is two letters, not a photograph: nobody uploads one, and
	 * a grey silhouette beside every comment says nothing. */
	g_string_append(html, "<span class=\"discussion-avatar\" aria-hidden=\"true\">");
	venture_html_escape_append(html, deleted ? "\xe2\x80\x93"
		: venture_json_object_get_string(comment, "author_initials", "?"));
	g_string_append(html, "</span><div class=\"discussion-main\"><header class=\"discussion-head\">");

	if (deleted)
	{
		g_string_append(html, "<span class=\"discussion-author is-gone\">Deleted comment</span>");
	}
	else
	{
		g_string_append(html, "<span class=\"discussion-author\">");
		venture_html_escape_append(html, name);
		g_string_append(html, "</span>");

		/* What the author is to this install, when it is more than a
		 * name: the reader themself, the owner or an administrator. */
		if (own)
			g_string_append(html, "<span class=\"discussion-badge\">You</span>");
		else if (0 == g_strcmp0(role, "owner"))
			g_string_append(html, "<span class=\"discussion-badge\">Owner</span>");
		else if (0 == g_strcmp0(role, "admin"))
			g_string_append(html, "<span class=\"discussion-badge\">Admin</span>");

		/* A comment made by a token says so, by the token's number --
		 * never its name, which is whatever its owner typed. */
		if (venture_json_object_get_bool(comment, "via_token", FALSE))
		{
			g_string_append(html, "<span class=\"discussion-via\">via ");
			venture_html_escape_append(html, author);
			g_string_append(html, "</span>");
		}
	}

	/* The time is the permalink, as everywhere people discuss things. */
	g_string_append(html, "<a class=\"discussion-when\" href=\"");
	venture_html_escape_append(html, url);
	g_string_append(html, "\" aria-label=\"Link to this comment\">");
	venture_web_comments_time(self, html,
		venture_json_object_get_string(comment, "created_at", NULL));
	g_string_append(html, "</a>");

	if (!deleted && (NULL != edited))
	{
		g_autoptr(GDateTime) when = venture_time_from_string(edited, NULL);
		g_autoptr(GDateTime) local = NULL;
		g_autofree gchar *exact = NULL;

		if (NULL != when)
		{
			local = g_date_time_to_timezone(when,
				venture_context_get_timezone(self->context));
			exact = g_date_time_format(local, "%Y-%m-%d %H:%M %Z");
		}

		g_string_append(html, "<span class=\"discussion-edited\" title=\"Edited ");
		venture_html_escape_append(html, (NULL != exact) ? exact : edited);
		g_string_append(html, "\">edited</span>");
	}

	g_string_append(html, "</header>");

	if (deleted)
	{
		g_string_append(html,
			"<p class=\"discussion-gone\">This comment was deleted. "
			"The replies below answered it.</p></div></article>");
		return;
	}

	/* Rendered by the core's markdown renderer: escaped first, then
	 * formatted, so nothing the author typed is markup. */
	g_string_append(html, "<div class=\"discussion-body markdown\" data-discussion-body>");
	g_string_append(html, venture_json_object_get_string(comment, "html", ""));
	g_string_append(html, "</div><footer class=\"discussion-actions\">");

	g_string_append_printf(html,
		"<a class=\"discussion-action\" href=\"#reply-%" G_GINT64_FORMAT
		"\" data-discussion-reply-to=\"%" G_GINT64_FORMAT "\"",
		thread_id, thread_id);

	/* Answering a reply names whoever wrote it, because the thread is
	 * one level deep and the reply lands at its foot. */
	if (reply && !own && !venture_string_is_empty(username))
	{
		g_string_append(html, " data-discussion-mention=\"");
		venture_html_escape_append(html, username);
		g_string_append(html, "\"");
	}

	g_string_append(html, ">Reply</a>");

	g_string_append(html, "<a class=\"discussion-action\" href=\"");
	venture_html_escape_append(html,
		venture_json_object_get_string(comment, "permalink", url));
	g_string_append(html, "\" data-discussion-copy>Copy link</a>");

	if (venture_json_object_get_bool(comment, "can_edit", FALSE))
	{
		g_autofree gchar *action = NULL;
		g_autofree gchar *input_id = NULL;

		action = g_strdup_printf("/comments/%" G_GINT64_FORMAT "/edit", id);
		input_id = g_strdup_printf("discussion-edit-%" G_GINT64_FORMAT, id);

		g_string_append(html,
			"<details class=\"discussion-edit\" data-discussion-edit>"
			"<summary class=\"discussion-action\">Edit</summary>");
		venture_web_comments_composer(html, action, NULL, 0, 0, input_id,
		                              "Edit your comment", "",
		                              venture_json_object_get_string(comment, "body", ""),
		                              "Save", TRUE);
		g_string_append(html, "</details>");
	}

	if (venture_json_object_get_bool(comment, "can_delete", FALSE))
		g_string_append_printf(html,
			"<form class=\"discussion-delete\" method=\"post\" action=\"/comments/%"
			G_GINT64_FORMAT "/delete\" data-discussion-delete>"
			"<button type=\"submit\" class=\"discussion-action is-danger\">"
			"Delete</button></form>", id);

	g_string_append(html, "</footer></div></article>");
}

/*
 * The Discussion card: every thread, oldest first, replies under the
 * comment they answer, a reply box at the foot of each thread and the box
 * for a new one at the bottom. Nothing at all for a record that takes no
 * comments.
 */
static void
venture_web_append_comments(
	VentureWebServer	*self,
	GString			*content,
	VentureAuthPrincipal	*principal,
	VentureEntity		*record
){
	g_autoptr(JsonNode) thread = NULL;
	const gchar *type_name;
	JsonArray *threads;
	gint64 id;
	gint64 count = 0;
	guint i;

	if (!venture_comment_subject_accepts(venture_context_get_database(self->context),
	                                     record))
		return;

	type_name = venture_entity_get_entity_name(record);
	id = venture_entity_get_id(record);
	thread = venture_comment_thread(self->context, principal, type_name, id, NULL);

	if ((NULL == thread) || !JSON_NODE_HOLDS_ARRAY(thread))
		return;

	threads = json_node_get_array(thread);

	for (i = 0; i < json_array_get_length(threads); i++)
	{
		JsonObject *top = json_array_get_object_element(threads, i);

		if (!venture_json_object_get_bool(top, "deleted", FALSE))
			count++;

		count += json_array_get_length(json_object_get_array_member(top, "replies"));
	}

	g_string_append(content, "<section class=\"card discussion\" id=\"discussion\" "
	                          "aria-labelledby=\"discussion-title\" data-discussion "
	                          "data-subject-type=\"");
	venture_html_escape_append(content, type_name);
	g_string_append_printf(content,
		"\" data-subject-id=\"%" G_GINT64_FORMAT "\">"
		"<div class=\"card-head\"><h2 id=\"discussion-title\">Discussion</h2>"
		"<span class=\"discussion-count\">%" G_GINT64_FORMAT " %s</span></div>"
		"<div class=\"card-body\">",
		id, count, (1 == count) ? "comment" : "comments");

	if (0 == json_array_get_length(threads))
	{
		g_string_append(content,
			"<div class=\"discussion-empty\"><p class=\"discussion-empty-title\">"
			"Nothing said here yet</p><p class=\"muted\">Ask a question, leave "
			"a note for whoever picks this up next, or bring somebody in with "
			"<kbd>@</kbd>their name &mdash; they will find it in their inbox."
			"</p></div>");
	}
	else
	{
		g_string_append(content, "<ol class=\"discussion-threads\">");

		for (i = 0; i < json_array_get_length(threads); i++)
		{
			JsonObject *top = json_array_get_object_element(threads, i);
			JsonArray *replies = json_object_get_array_member(top, "replies");
			g_autofree gchar *input_id = NULL;
			gint64 top_id = venture_json_object_get_int(top, "id", 0);
			guint j;

			g_string_append(content, "<li class=\"discussion-thread\">");
			venture_web_comments_article(self, content, top, principal, top_id,
			                             FALSE);

			if (json_array_get_length(replies) > 0)
			{
				g_string_append(content, "<ol class=\"discussion-replies\">");

				for (j = 0; j < json_array_get_length(replies); j++)
				{
					g_string_append(content, "<li>");
					venture_web_comments_article(self, content,
						json_array_get_object_element(replies, j), principal,
						top_id, TRUE);
					g_string_append(content, "</li>");
				}

				g_string_append(content, "</ol>");
			}

			/* A reply box at the foot of every thread, folded; with
			 * the script, any Reply in the thread opens it. */
			input_id = g_strdup_printf("discussion-reply-%" G_GINT64_FORMAT, top_id);
			g_string_append_printf(content,
				"<details class=\"discussion-reply\" id=\"reply-%" G_GINT64_FORMAT
				"\" data-discussion-reply><summary>Reply</summary>", top_id);
			venture_web_comments_composer(content, "/comments", type_name, id,
			                              top_id, input_id, "Your reply",
			                              "Reply to the thread", "", "Reply",
			                              TRUE);
			g_string_append(content, "</details></li>");
		}

		g_string_append(content, "</ol>");
	}

	g_string_append(content, "<div class=\"discussion-new\">");
	venture_web_comments_composer(content, "/comments", type_name, id, 0,
	                              "discussion-new", "Add a comment",
	                              "Add to the discussion\xe2\x80\xa6", "", "Comment",
	                              FALSE);
	g_string_append(content, "</div></div></section>");
}

/*
 * The answer to a write from the page. The script asks for the panel again
 * (X-Venture-Discussion) and swaps it in place, told which comment to land
 * on; a plain form post is sent to the comment's anchor.
 */
static HtmxResponse *
venture_web_comments_answer(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureAuthPrincipal	*principal,
	VentureEntity		*subject,
	gint64			 comment_id,
	const gchar		*destination
){
	if (NULL != venture_web_comments_header(request, "X-Venture-Discussion"))
	{
		g_autoptr(GString) panel = g_string_new(NULL);
		HtmxResponse *response;

		venture_web_append_comments(self, panel, principal, subject);
		response = venture_web_html_response(g_string_free(g_steal_pointer(&panel), FALSE), 200);

		if (0 != comment_id)
		{
			g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, comment_id);

			htmx_response_add_header(response, "X-Venture-Comment", value);
		}

		return response;
	}

	return venture_web_redirect_to(destination);
}

/* --- The page's routes ----------------------------------------------------- */

/*
 * GET /comments/:id - a comment's permalink: its record's page, at the
 * comment. Not found for anybody who may not read the record, exactly as
 * the record's own page would say.
 */
static HtmxResponse *
venture_web_ui_comment_permalink(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) comment = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);
	comment = venture_web_comments_load(self, principal,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), NULL,
		&error);

	if (NULL == comment)
		return venture_web_error_response(error);

	destination = venture_comment_anchor_url(comment);

	return venture_web_redirect_to(destination);
}

/* POST /comments - subject_type, subject_id, parent_id, body. */
static HtmxResponse *
venture_web_ui_comment_create(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureEntity) comment = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	const gchar *parent;
	HtmxResponse *redirect;

	(void)params;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_hosted_auth_require(self, request, principal,
	                                     VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	subject = venture_web_comments_subject(self, principal,
		htmx_request_get_form_value(request, "subject_type"),
		g_ascii_strtoll(venture_string_is_empty(htmx_request_get_form_value(request, "subject_id"))
			? "0" : htmx_request_get_form_value(request, "subject_id"), NULL, 10),
		&error);

	if (NULL == subject)
		return venture_web_error_response(error);

	parent = htmx_request_get_form_value(request, "parent_id");
	comment = venture_comment_create(self->context, principal,
		venture_entity_get_entity_name(subject), venture_entity_get_id(subject),
		venture_string_is_empty(parent) ? 0 : g_ascii_strtoll(parent, NULL, 10),
		htmx_request_get_form_value(request, "body"), &error);

	if (NULL == comment)
		return venture_web_error_response(error);

	destination = venture_comment_anchor_url(comment);

	return venture_web_comments_answer(self, request, principal, subject,
	                                   venture_entity_get_id(comment), destination);
}

/* POST /comments/:id/edit - body. */
static HtmxResponse *
venture_web_ui_comment_edit(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureEntity) comment = NULL;
	g_autoptr(VentureEntity) saved = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	gint64 id;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	comment = venture_web_comments_load(self, principal, id, &subject, &error);

	if (NULL == comment)
		return venture_web_error_response(error);

	saved = venture_comment_edit(self->context, principal, id,
	                             htmx_request_get_form_value(request, "body"),
	                             &error);

	if (NULL == saved)
		return venture_web_error_response(error);

	destination = venture_comment_anchor_url(saved);

	return venture_web_comments_answer(self, request, principal, subject, id,
	                                   destination);
}

/* POST /comments/:id/delete */
static HtmxResponse *
venture_web_ui_comment_delete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureEntity) comment = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	gint64 id;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	comment = venture_web_comments_load(self, principal, id, &subject, &error);

	if (NULL == comment)
		return venture_web_error_response(error);

	if (!venture_comment_delete(self->context, principal, id, &error))
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT "#discussion",
	                              venture_entity_get_entity_name(subject),
	                              venture_entity_get_id(subject));

	return venture_web_comments_answer(self, request, principal, subject, 0,
	                                   destination);
}

/* --- The API --------------------------------------------------------------- */

/* The JSON body of a request, as an object, or a refusal. */
static JsonObject *
venture_web_comments_body(
	HtmxRequest	 *request,
	JsonNode	**holder,
	GError		**error
){
	*holder = htmx_request_get_json(request, NULL);

	if ((NULL == *holder) || !JSON_NODE_HOLDS_OBJECT(*holder))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The body must be a JSON object");
		return NULL;
	}

	return json_node_get_object(*holder);
}

/*
 * GET /api/v1/comments?subject_type=T&subject_id=N - a record's discussion
 * as a tree: top-level comments, oldest first, each with its replies.
 */
static HtmxResponse *
venture_web_api_comments_list(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(JsonNode) thread = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *url = NULL;
	const gchar *subject_id;
	HtmxResponse *denied;

	(void)params;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);
	subject_id = htmx_request_get_query_param(request, "subject_id");
	subject = venture_web_comments_subject(self, principal,
		htmx_request_get_query_param(request, "subject_type"),
		venture_string_is_empty(subject_id) ? 0 : g_ascii_strtoll(subject_id, NULL, 10),
		&error);

	if (NULL == subject)
		return venture_web_error_response(error);

	if (!venture_comment_type_accepts(G_OBJECT_TYPE(subject), &error))
		return venture_web_error_response(error);

	thread = venture_comment_thread(self->context, principal,
	                                venture_entity_get_entity_name(subject),
	                                venture_entity_get_id(subject), &error);

	if (NULL == thread)
		return venture_web_error_response(error);

	label = venture_entity_get_display_name(subject);
	url = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT,
	                      venture_entity_get_entity_name(subject),
	                      venture_entity_get_id(subject));

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "subject");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, venture_entity_get_entity_name(subject));
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, venture_entity_get_id(subject));
	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, label);
	json_builder_set_member_name(builder, "url");
	json_builder_add_string_value(builder, url);
	json_builder_end_object(builder);
	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder,
		venture_comment_count(self->context, venture_entity_get_entity_name(subject),
		                      venture_entity_get_id(subject)));
	json_builder_set_member_name(builder, "comments");
	json_builder_add_value(builder, g_steal_pointer(&thread));
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/* POST /api/v1/comments - {subject_type, subject_id, body, parent_id}. */
static HtmxResponse *
venture_web_api_comments_create(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureEntity) comment = NULL;
	g_autoptr(JsonNode) holder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *body;
	HtmxResponse *denied;

	(void)params;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);
	body = venture_web_comments_body(request, &holder, &error);

	if (NULL == body)
		return venture_web_error_response(error);

	subject = venture_web_comments_subject(self, principal,
		venture_json_object_get_string(body, "subject_type", NULL),
		venture_json_object_get_int(body, "subject_id", 0), &error);

	if (NULL == subject)
		return venture_web_error_response(error);

	/* An "author" in the body is not read: who wrote it is who asked. */
	comment = venture_comment_create(self->context, principal,
		venture_entity_get_entity_name(subject), venture_entity_get_id(subject),
		venture_json_object_get_int(body, "parent_id", 0),
		venture_json_object_get_string(body, "body", NULL), &error);

	if (NULL == comment)
		return venture_web_error_response(error);

	node = venture_comment_to_json(self->context, principal, comment);

	return venture_web_json_response(node, 201);
}

/* GET /api/v1/comments/:id */
static HtmxResponse *
venture_web_api_comments_get(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) comment = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *denied;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);
	comment = venture_web_comments_load(self, principal,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), NULL,
		&error);

	if (NULL == comment)
		return venture_web_error_response(error);

	node = venture_comment_to_json(self->context, principal, comment);

	return venture_web_json_response(node, 200);
}

/* PATCH /api/v1/comments/:id - {body}. */
static HtmxResponse *
venture_web_api_comments_update(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) comment = NULL;
	g_autoptr(VentureEntity) saved = NULL;
	g_autoptr(JsonNode) holder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *body;
	HtmxResponse *denied;
	gint64 id;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	comment = venture_web_comments_load(self, principal, id, NULL, &error);

	if (NULL == comment)
		return venture_web_error_response(error);

	body = venture_web_comments_body(request, &holder, &error);

	if (NULL == body)
		return venture_web_error_response(error);

	saved = venture_comment_edit(self->context, principal, id,
		venture_json_object_get_string(body, "body", NULL), &error);

	if (NULL == saved)
		return venture_web_error_response(error);

	node = venture_comment_to_json(self->context, principal, saved);

	return venture_web_json_response(node, 200);
}

/* DELETE /api/v1/comments/:id */
static HtmxResponse *
venture_web_api_comments_delete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) comment = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *denied;
	gint64 id;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	comment = venture_web_comments_load(self, principal, id, NULL, &error);

	if (NULL == comment)
		return venture_web_error_response(error);

	if (!venture_comment_delete(self->context, principal, id, &error))
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "deleted");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, id);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * POST /api/v1/comments/preview - {subject_type, subject_id, body}: the
 * HTML the comment would render as, for this reader, from the same
 * function the page uses -- so what the Preview tab shows is what posting
 * shows.
 */
static HtmxResponse *
venture_web_api_comments_preview(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(JsonNode) holder = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *html = NULL;
	JsonObject *body;
	const gchar *text;
	HtmxResponse *denied;

	(void)params;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);
	body = venture_web_comments_body(request, &holder, &error);

	if (NULL == body)
		return venture_web_error_response(error);

	subject = venture_web_comments_subject(self, principal,
		venture_json_object_get_string(body, "subject_type", NULL),
		venture_json_object_get_int(body, "subject_id", 0), &error);

	if (NULL == subject)
		return venture_web_error_response(error);

	text = venture_json_object_get_string(body, "body", "");

	if (strlen(text) > VENTURE_COMMENT_MAX_LENGTH)
	{
		venture_set_error_validation(&error, "body",
			"A comment is at most %d characters", VENTURE_COMMENT_MAX_LENGTH);
		return venture_web_error_response(error);
	}

	html = venture_comment_render(self->context, subject, text, NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "html");
	json_builder_add_string_value(builder, html);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * GET /api/v1/comments/mentions?subject_type=T&subject_id=N&q=PREFIX - who
 * the composer's @ menu offers: only people who may read the record.
 */
static HtmxResponse *
venture_web_api_comments_mentions(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *subject_id;
	const gchar *limit;
	HtmxResponse *denied;

	(void)params;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);
	subject_id = htmx_request_get_query_param(request, "subject_id");
	subject = venture_web_comments_subject(self, principal,
		htmx_request_get_query_param(request, "subject_type"),
		venture_string_is_empty(subject_id) ? 0 : g_ascii_strtoll(subject_id, NULL, 10),
		&error);

	if (NULL == subject)
		return venture_web_error_response(error);

	limit = htmx_request_get_query_param(request, "limit");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "users");
	json_builder_add_value(builder,
		venture_comment_mention_candidates(self->context, subject,
			htmx_request_get_query_param(request, "q"),
			venture_string_is_empty(limit)
				? 8 : (guint)CLAMP(g_ascii_strtoll(limit, NULL, 10), 1, 25)));
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}
