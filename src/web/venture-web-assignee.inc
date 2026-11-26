/*
 * venture-web-assignee.inc - Choosing whom the work goes to
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c. Three things, all over the one list
 * venture_assignee_list_members() makes, so nothing here can offer a name
 * the save would refuse:
 *
 *  - GET /api/v1/organizations/:organization_id/members, the lookup an API
 *    client, venturectl or an agent types a name into;
 *  - the picker: a <select> of the organization's members, rendered on the
 *    server, which venture.js turns into the type-to-filter combobox every
 *    other <select data-picker-search> gets. With scripting off it is a
 *    plain <select>, and it still works;
 *  - POST /tickets/:id/assign, what the board card's and the Desk card's
 *    pickers post.
 */

/* ==========================================================================
 * The lookup
 * ========================================================================== */

/*
 * Whether @principal may list @organization_id's members: anybody who may
 * read that organization's work. With organization access on that is a
 * membership of it (any role) -- or the owner and administrators, who read
 * every organization; with it off, every signed-in reader. Deliberately
 * not the users list's rule, which is the owner's alone: an editor who may
 * assign a ticket has to be able to find out to whom, and what this
 * answers -- usernames and display names of colleagues -- is what the
 * tickets they already read show on every card.
 */
static gboolean
venture_web_members_visible(
	VentureWebServer	*self,
	VentureAuthPrincipal	*principal,
	gint64			 organization_id
){
	static const gint roles[] = {
		VENTURE_ORGANIZATION_ROLE_VIEWER, VENTURE_ORGANIZATION_ROLE_OWNER,
		VENTURE_ORGANIZATION_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_EDITOR,
		VENTURE_ORGANIZATION_ROLE_FINANCE, VENTURE_ORGANIZATION_ROLE_SALES,
		VENTURE_ORGANIZATION_ROLE_SUPPORT, VENTURE_ORGANIZATION_ROLE_ACCOUNTANT
	};
	VentureDatabase *database;

	database = venture_context_get_database(self->context);

	if (!venture_entity_registry_is_type_enabled(
		venture_entity_registry_get_default(), "organization_membership"))
		return TRUE;

	return venture_access_policy_has_organization_role(
		venture_database_get_access_policy(database), principal,
		organization_id, roles, G_N_ELEMENTS(roles));
}

/*
 * GET /api/v1/organizations/:organization_id/members?q=&limit=&offset=
 *
 * The people a ticket in that organization may be assigned to, ordered
 * by username: {"total", "count", "limit", "offset", "records": [
 * {"username", "display_name"}]}. q matches anywhere in the username or
 * the display name, ignoring case, so "ox" finds "oxpecker". Nothing else
 * about a person is answered -- not an email, not a role, not an id.
 *
 * An organization the caller may not read answers 404, the same as one
 * that does not exist: the list must not confirm which do.
 */
static HtmxResponse *
venture_web_api_organization_members(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	VentureDatabase *database;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GPtrArray) members = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	HtmxResponse *refused;
	const gchar *text;
	gint64 organization_id;
	gint64 limit;
	gint64 offset;
	guint total;
	guint i;

	self = user_data;
	database = venture_context_get_database(self->context);

	refused = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != refused)
		return refused;

	principal = venture_auth_authenticate(self->auth, request);

	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "organization_id"),
	                              10, 1, G_MAXINT64, &organization_id, NULL))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "organization_id: a positive organization ID is "
		                    "required");
		return venture_web_error_response(error);
	}

	/* Reading the organization under the caller's own scope is the first
	 * check, membership the second; either refusal is the same 404. */
	{
		g_autoptr(VentureAccessScope) scope = NULL;

		scope = venture_access_policy_enter(
			venture_database_get_access_policy(database), principal);
		organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION,
		                                    organization_id, NULL);
	}

	if ((NULL == organization) || venture_entity_is_deleted(organization) ||
	    !venture_web_members_visible(self, principal, organization_id))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "That organization is not available.");
		return venture_web_error_response(error);
	}

	limit = 20;
	offset = 0;

	if ((NULL != htmx_request_get_query_param(request, "limit")) &&
	    !g_ascii_string_to_signed(htmx_request_get_query_param(request, "limit"),
	                              10, 1, VENTURE_ASSIGNEE_MEMBERS_MAX, &limit,
	                              NULL))
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "limit: a whole number from 1 to %d",
		            VENTURE_ASSIGNEE_MEMBERS_MAX);
		return venture_web_error_response(error);
	}

	if ((NULL != htmx_request_get_query_param(request, "offset")) &&
	    !g_ascii_string_to_signed(htmx_request_get_query_param(request, "offset"),
	                              10, 0, G_MAXINT32, &offset, NULL))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "offset: a whole number, 0 or more");
		return venture_web_error_response(error);
	}

	text = htmx_request_get_query_param(request, "q");
	members = venture_assignee_list_members(database, organization_id, text,
	                                        (guint)offset, (guint)limit,
	                                        &total, &error);

	if (NULL == members)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "total");
	json_builder_add_int_value(builder, (gint64)total);
	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder, (gint64)members->len);
	json_builder_set_member_name(builder, "limit");
	json_builder_add_int_value(builder, limit);
	json_builder_set_member_name(builder, "offset");
	json_builder_add_int_value(builder, offset);
	json_builder_set_member_name(builder, "records");
	json_builder_begin_array(builder);

	for (i = 0; i < members->len; i++)
	{
		g_autofree gchar *username = NULL;
		g_autofree gchar *display = NULL;

		g_object_get(g_ptr_array_index(members, i), "username", &username,
		             "display-name", &display, NULL);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "username");
		json_builder_add_string_value(builder, username);
		json_builder_set_member_name(builder, "display_name");

		if (venture_string_is_empty(display))
			json_builder_add_null_value(builder);
		else
			json_builder_add_string_value(builder, display);

		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	{
		g_autoptr(JsonNode) node = NULL;

		node = json_builder_get_root(builder);

		return venture_web_json_response(node, 200);
	}
}

/* ==========================================================================
 * The picker
 * ========================================================================== */

/*
 * The username of whoever is looking at the page, from their user row: a
 * principal's name is "API token #N" for a token, which nobody can be
 * assigned. %NULL for nobody in particular.
 */
static gchar *
venture_web_member_me(VentureWebServer *self)
{
	VentureDatabase *database;
	const VentureAuthPrincipal *actor;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) user = NULL;
	gchar *username;

	database = venture_context_get_database(self->context);
	actor = venture_access_policy_get_actor(
		venture_database_get_access_policy(database));

	if ((NULL == actor) || (actor->user_id <= 0))
		return NULL;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);
	user = venture_database_get(database, VENTURE_TYPE_USER, actor->user_id,
	                            NULL);

	if (NULL == user)
		return NULL;

	username = NULL;
	g_object_get(user, "username", &username, NULL);

	return username;
}

/*
 * The members of @organization_id as "username\tdisplay name\n" lines,
 * kept in @cache when one is given: the board draws a picker on every
 * card, and the list is the same for every card in one organization.
 */
static const gchar *
venture_web_member_lines(
	VentureWebServer	*self,
	GHashTable		*cache,
	gint64			 organization_id,
	gchar		       **owned
){
	g_autofree gchar *key = NULL;
	g_autoptr(GPtrArray) members = NULL;
	g_autoptr(GString) lines = NULL;
	const gchar *known;
	guint i;

	key = g_strdup_printf("assign-members:%" G_GINT64_FORMAT, organization_id);

	if (NULL != cache)
	{
		known = g_hash_table_lookup(cache, key);

		if (NULL != known)
			return known;
	}

	members = venture_assignee_list_members(
		venture_context_get_database(self->context), organization_id, NULL, 0,
		VENTURE_ASSIGNEE_MEMBERS_MAX, NULL, NULL);
	lines = g_string_new(NULL);

	for (i = 0; (NULL != members) && (i < members->len); i++)
	{
		g_autofree gchar *username = NULL;
		g_autofree gchar *display = NULL;

		g_object_get(g_ptr_array_index(members, i), "username", &username,
		             "display-name", &display, NULL);

		/* A tab or a newline in a name would break the lines; neither
		 * belongs in one, so the row is left out rather than misread. */
		if (venture_string_is_empty(username) || (NULL != strpbrk(username, "\t\n")))
			continue;

		g_string_append_printf(lines, "%s\t%s\n", username,
			(NULL != display) ? g_strdelimit(display, "\t\n", ' ') : "");
	}

	if (NULL != cache)
	{
		gchar *text;

		text = g_string_free(g_steal_pointer(&lines), FALSE);
		g_hash_table_insert(cache, g_steal_pointer(&key), text);
		return text;
	}

	*owned = g_string_free(g_steal_pointer(&lines), FALSE);
	return *owned;
}

/* "oxpecker (Ox Pecker)", or the username alone. The username first, so a
 * native select's type-ahead finds a person by it with scripting off. */
static void
venture_web_append_member_label(
	GString		*content,
	const gchar	*username,
	const gchar	*display
){
	venture_html_escape_append(content, username);

	if (!venture_string_is_empty(display) && (0 != g_strcmp0(display, username)))
	{
		g_string_append(content, " (");
		venture_html_escape_append(content, display);
		g_string_append(content, ")");
	}
}

/*
 * The options of a member picker: nobody, the reader ("Me"), any
 * placeholder, then every member. A @current the list does not hold -- a
 * name stored before the rule, a member who left -- is kept as a choice
 * and selected, marked as not a member: dropping it would make the form
 * post "nobody" for a ticket nobody meant to unassign, and keeping it
 * unchanged is what lets the rest of the record still be edited.
 */
static void
venture_web_append_member_options(
	GString		*content,
	const gchar	*lines,
	const gchar	*current,
	const gchar	*me,
	const gchar	*nobody,
	gboolean	 placeholder
){
	g_auto(GStrv) rows = NULL;
	g_autofree gchar *me_display = NULL;
	gboolean me_member;
	gboolean current_seen;
	gsize i;

	rows = g_strsplit((NULL != lines) ? lines : "", "\n", -1);
	me_member = FALSE;
	current_seen = venture_string_is_empty(current);

	/* Whether the reader is one of them, and under what name. */
	for (i = 0; NULL != rows[i]; i++)
	{
		g_auto(GStrv) parts = g_strsplit(rows[i], "\t", 2);

		if ((NULL != parts[0]) && !venture_string_is_empty(me) &&
		    (0 == g_strcmp0(parts[0], me)))
		{
			me_member = TRUE;
			me_display = g_strdup(parts[1]);
		}
	}

	g_string_append(content, "<option value=\"\"");

	if (venture_string_is_empty(current))
		g_string_append(content, " selected");

	g_string_append(content, ">");
	venture_html_escape_append(content, nobody);
	g_string_append(content, "</option>");

	if (placeholder)
	{
		gboolean chosen;

		chosen = (0 == g_strcmp0(current, VENTURE_ASSIGNEE_PLACEHOLDER_ME));
		current_seen = current_seen || chosen;
		g_string_append_printf(content,
			"<option value=\"%s\"%s>Whoever applies it</option>",
			VENTURE_ASSIGNEE_PLACEHOLDER_ME, chosen ? " selected" : "");
	}

	/* Assign to me: first after nobody, and not repeated below. */
	if (me_member)
	{
		gboolean chosen;

		chosen = (0 == g_strcmp0(current, me));
		current_seen = current_seen || chosen;
		g_string_append(content, "<option value=\"");
		venture_html_escape_append(content, me);
		g_string_append_printf(content, "\"%s>Me \xe2\x80\x94 ",
		                       chosen ? " selected" : "");
		venture_web_append_member_label(content, me, me_display);
		g_string_append(content, "</option>");
	}

	for (i = 0; NULL != rows[i]; i++)
	{
		g_auto(GStrv) parts = NULL;
		gboolean chosen;

		if ('\0' == rows[i][0])
			continue;

		parts = g_strsplit(rows[i], "\t", 2);

		if (me_member && (0 == g_strcmp0(parts[0], me)))
			continue;

		chosen = (0 == g_strcmp0(current, parts[0]));
		current_seen = current_seen || chosen;
		g_string_append(content, "<option value=\"");
		venture_html_escape_append(content, parts[0]);
		g_string_append_printf(content, "\"%s>", chosen ? " selected" : "");
		venture_web_append_member_label(content, parts[0], parts[1]);
		g_string_append(content, "</option>");
	}

	if (!current_seen)
	{
		g_string_append(content, "<option value=\"");
		venture_html_escape_append(content, current);
		g_string_append(content, "\" selected>");
		venture_html_escape_append(content, current);
		g_string_append(content, " \xe2\x80\x94 not a member</option>");
	}
}

/*
 * A form field flagged %VENTURE_COLUMN_FLAG_MEMBER_USERNAME, drawn as a
 * member picker; one flagged %VENTURE_COLUMN_FLAG_MEMBER_USERNAMES (a
 * rota), as its text box with the members named beneath it. The label
 * is already open; this draws the control only.
 *
 * Returns: %TRUE when it drew the control, %FALSE for a field it does not
 *   draw (the caller's own default then applies)
 */
static gboolean
venture_web_append_member_field(
	VentureWebServer	*self,
	GString			*content,
	VentureFieldSpec	*spec,
	VentureEntity		*record,
	gint64			 organization,
	const gchar		*current
){
	g_autofree gchar *owned = NULL;
	g_autofree gchar *me = NULL;
	VentureColumnFlags flags;
	const gchar *name;
	const gchar *lines;

	flags = venture_field_spec_get_flags(spec);

	if (0 == (flags & (VENTURE_COLUMN_FLAG_MEMBER_USERNAME |
	                   VENTURE_COLUMN_FLAG_MEMBER_USERNAMES)))
		return FALSE;

	name = venture_field_spec_get_name(spec);

	if ((organization <= 0) && (NULL != record))
		organization = venture_entity_get_organization_id(record);

	if (organization <= 0)
		organization = venture_context_get_default_organization_id(self->context);

	lines = venture_web_member_lines(self, NULL, organization, &owned);

	/*
	 * A rota is an ordered list, and a <select multiple> keeps no order:
	 * the box stays a box, and says who may be in it. The save refuses
	 * any name that is not one of these.
	 */
	if (0 != (flags & VENTURE_COLUMN_FLAG_MEMBER_USERNAMES))
	{
		g_auto(GStrv) rows = NULL;
		guint shown;
		gsize i;

		g_string_append_printf(content,
			"<input type=\"text\" name=\"%s\" value=\"", name);
		venture_html_escape_append(content, current);
		g_string_append(content, "\" autocomplete=\"off\">"
		                         "<span class=\"field-help member-hint\">"
		                         "Members: ");
		rows = g_strsplit(lines, "\n", -1);
		shown = 0;

		for (i = 0; NULL != rows[i]; i++)
		{
			g_auto(GStrv) parts = NULL;

			if ('\0' == rows[i][0])
				continue;

			parts = g_strsplit(rows[i], "\t", 2);

			if (shown > 0)
				g_string_append(content, ", ");

			venture_html_escape_append(content, parts[0]);
			shown++;
		}

		if (0 == shown)
			g_string_append(content, "nobody yet");

		g_string_append(content, "</span>");
		return TRUE;
	}

	me = venture_web_member_me(self);

	/* data-picker-search: venture.js filters it as you type; without
	 * scripting it is the <select> it was rendered as. */
	g_string_append_printf(content,
		"<select name=\"%s\" data-picker-search data-member-picker>", name);
	venture_web_append_member_options(content, lines, current, me,
		(0 != (flags & VENTURE_COLUMN_FLAG_MEMBER_PLACEHOLDER))
			? "Leave it as it is" : "Unassigned",
		0 != (flags & VENTURE_COLUMN_FLAG_MEMBER_PLACEHOLDER));
	g_string_append(content, "</select>");

	return TRUE;
}

/*
 * The quick-assign form a ticket carries on the board and on its Desk
 * card: the member picker and a button, posting to /tickets/:id/assign.
 * A form of its own so it works with scripting off and from the keyboard,
 * like the board's Move.
 */
static void
venture_web_append_assign_form(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*ticket,
	GHashTable		*cache,
	const gchar		*from,
	const gchar		*css_class
){
	g_autofree gchar *assignee = NULL;
	g_autofree gchar *owned = NULL;
	g_autofree gchar *me = NULL;
	const gchar *lines;
	gint64 id;

	id = venture_entity_get_id(ticket);
	g_object_get(ticket, "assignee", &assignee, NULL);
	lines = venture_web_member_lines(self, cache,
		venture_entity_get_organization_id(ticket), &owned);
	me = venture_web_member_me(self);

	g_string_append_printf(content,
		"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT "/assign\" "
		"class=\"%s\"><input type=\"hidden\" name=\"from\" value=\"%s\">"
		"<select name=\"assignee\" data-picker-search data-member-picker "
		"aria-label=\"Assign ticket #%" G_GINT64_FORMAT " to\">",
		id, css_class, from, id);
	venture_web_append_member_options(content, lines, assignee, me,
	                                  "Unassigned", FALSE);
	g_string_append(content, "</select><button class=\"btn btn-sm\" "
	                         "type=\"submit\">Assign</button></form>");
}

/* The board card's quick assign. @names is the board's own lookup cache. */
static void
venture_web_append_ticket_assign(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*ticket,
	GHashTable		*names
){
	venture_web_append_assign_form(self, content, ticket, names, "board",
	                               "ticket-assign");
}

/* The Desk card's quick assign, beside "Assign to me". */
static void
venture_web_append_desk_assign(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*ticket
){
	venture_web_append_assign_form(self, content, ticket, NULL, "ticket",
	                               "inline desk-assign");
}

/* ==========================================================================
 * The action
 * ========================================================================== */

/*
 * POST /tickets/:id/assign - assignee=<username>, or empty for nobody.
 *
 * Saved like every other write, so the save validator is what refuses a
 * name that is not a member: there is no second check here to drift from
 * it. Back to the board or to the ticket, by from=; JSON to a scripted
 * caller (async=1), like the Desk's other actions.
 */
static HtmxResponse *
venture_web_ui_ticket_assign(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *back = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *assignee;
	gint64 ticket_id;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "tickets");

		if (NULL != gate)
			return gate;
	}

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_hosted_auth_require(self, request, principal,
	                                     VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	ticket = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_TICKET, ticket_id, &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	/* Absent is not "nobody": a post without the field is a mistake, and
	 * unassigning a ticket by accident is the one thing not to do. */
	assignee = htmx_request_get_form_value(request, "assignee");

	if (NULL == assignee)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "assignee: say whom to assign it to, or send it "
		                    "empty for nobody");
		return venture_web_error_response(error);
	}

	{
		g_autofree gchar *name = g_strstrip(g_strdup(assignee));

		g_object_set(ticket, "assignee", name, NULL);
	}

	venture_auth_to_actor(principal, &actor);
	venture_database_save(venture_context_get_database(self->context), ticket,
	                      &actor, &error);

	back = (0 == g_strcmp0(htmx_request_get_form_value(request, "from"), "board"))
		? g_strdup("/tickets")
		: g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	return venture_web_desk_answer(request, back, error);
}
