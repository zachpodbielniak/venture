/*
 * venture-web-server.c - The REST API and the HTMX web UI
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The routes here are generic. Rather than a handler per record type, there
 * is one handler per verb that resolves the type from the path and works
 * from its property metadata. That is what makes a plugin's record type a
 * first-class REST resource the moment it registers.
 */

#include "venture.h"

#include <string.h>

#include "venture-assets.h"

struct _VentureWebServer
{
	GObject parent_instance;

	VentureContext	*context;
	VentureAuth	*auth;
	HtmxServer	*server;
	gchar		*base_url;
	guint16		 port;
};

G_DEFINE_FINAL_TYPE(VentureWebServer, venture_web_server, G_TYPE_OBJECT)

/*
 * The server is reachable from route callbacks through the user_data pointer
 * htmx-glib passes along, so no global is needed.
 */

static void
venture_web_server_finalize(GObject *object)
{
	VentureWebServer *self;

	self = VENTURE_WEB_SERVER(object);

	g_clear_object(&self->context);
	g_clear_object(&self->auth);
	g_clear_object(&self->server);
	g_clear_pointer(&self->base_url, g_free);

	G_OBJECT_CLASS(venture_web_server_parent_class)->finalize(object);
}

static void
venture_web_server_class_init(VentureWebServerClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_web_server_finalize;
}

static void
venture_web_server_init(VentureWebServer *self)
{
}

/* ==========================================================================
 * Response helpers
 * ========================================================================== */

static HtmxResponse *
venture_web_json_response(
	JsonNode	*node,
	guint		 status
){
	g_autofree gchar *body = NULL;
	HtmxResponse *response;

	body = venture_json_to_string(node, TRUE);
	response = htmx_response_new_with_content(body);
	htmx_response_set_content_type(response, "application/json; charset=utf-8");
	htmx_response_set_status(response, status);

	return response;
}

/*
 * Renders a #GError as the standard JSON error body with the HTTP status the
 * error domain maps to, so every failure looks the same to a client.
 */
static HtmxResponse *
venture_web_error_response(const GError *error)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	guint status;

	status = (NULL != error)
		? venture_error_to_http_status((VentureError)error->code)
		: 500;

	if ((NULL != error) && (VENTURE_ERROR != error->domain))
		status = 500;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	venture_json_builder_add_error(builder, error);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, status);
}

static HtmxResponse *
venture_web_html_response(
	gchar	*body,
	guint	 status
){
	HtmxResponse *response;

	response = htmx_response_new_with_content(body);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	htmx_response_set_status(response, status);
	g_free(body);

	return response;
}

/* ==========================================================================
 * The page shell
 * ========================================================================== */

/*
 * Emits the surrounding document: head, sidebar, main region and the AI
 * dock. Every full page goes through this; an HTMX fragment request skips it
 * entirely and returns just the piece that changed.
 */
/*
 * The sidebar.
 *
 * At file scope and reachable from venture_web_navigation() so the test
 * suite can assert that every link here actually routes. A sidebar entry
 * pointing at a path with no handler is a 404 the operator finds by
 * clicking, which is how /settings was missing for a while.
 */
/*
 * Rejects an API request that is not authorised for @role.
 *
 * The counterpart to venture_web_ui_require_session(), for the same reason:
 * /api/v1/settings, /plugins, /venture-types, /automations and /schema all
 * shipped without a check because each handler carried its own, and an
 * absent one is invisible.
 *
 * Returns: (transfer full) (nullable): an error response to return instead,
 *   or %NULL when the request may proceed
 */
static HtmxResponse *
venture_web_api_require(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureUserRole		 role
){
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;

	principal = venture_auth_authenticate(self->auth, request);

	if (venture_auth_require(self->auth, principal, role, &error))
		return NULL;

	return venture_web_error_response(g_steal_pointer(&error));
}

/*
 * Raises the role a request needs when the record type is itself part of
 * access control.
 *
 * Accounts and API tokens are not business data: creating a user, changing
 * a role or reading the token table is administering who can get in. An
 * editor being able to add a user with role=owner -- even one with no
 * password yet -- is a privilege-escalation shape, and the token table
 * discloses what credentials exist.
 *
 * Returns: %TRUE if the principal may proceed
 */
static gboolean
venture_web_require_for_type(
	VentureWebServer	 *self,
	VentureAuthPrincipal	 *principal,
	GType			  entity_type,
	VentureUserRole		  ordinary,
	GError			**error
){
	VentureUserRole needed;

	needed = ordinary;

	if ((VENTURE_TYPE_USER == entity_type) ||
	    (VENTURE_TYPE_API_TOKEN == entity_type))
		needed = VENTURE_USER_ROLE_OWNER;

	return venture_auth_require(self->auth, principal, needed, error);
}

/*
 * Redirects to the login page unless the request carries a valid session.
 *
 * A helper rather than the same four lines pasted into each page handler,
 * because the pasting is exactly what let /reports and /settings ship with
 * no check at all: a missing guard looks like nothing, and nothing is hard
 * to notice in review.
 *
 * With authentication disabled every principal is the owner, so this
 * correctly lets the request through -- configuration validation already
 * refuses that combination on a non-loopback address.
 *
 * Returns: (transfer full) (nullable): a redirect to return instead, or
 *   %NULL when the request may proceed
 */
static HtmxResponse *
venture_web_ui_require_session(
	VentureWebServer	*self,
	HtmxRequest		*request
){
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	HtmxResponse *response;

	principal = venture_auth_authenticate(self->auth, request);

	if (principal->authenticated)
		return NULL;

	response = htmx_response_new();
	htmx_response_set_status(response, 302);
	htmx_response_add_header(response, "Location", "/login");

	return response;
}

/* Defined below, alongside the rest of the entity handling. */
static gint64
venture_web_active_organization(
	VentureWebServer	*self,
	HtmxRequest		*request
);

static HtmxResponse *
venture_web_redirect_to(const gchar *path);


static void
venture_web_append_entity_picker(
	VentureWebServer	*self,
	HtmxRequest		*request,
	GString			*html,
	const gchar		*current_path
);

static void
venture_web_scope_to_active_organization(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureQuery		*query
);

static const VentureWebNavLink venture_web_nav_links[] = {
	{ "/",                 "Dashboard",   "\xe2\x97\x89", "Overview" },
	{ "/reports",          "Reports",     "\xe2\x96\xa4", NULL },
	{ "/e/venture",        "Ventures",    "\xe2\x97\x86", "Business" },
	{ "/e/sale",           "Sales",       "\xe2\x86\x97", NULL },
	{ "/e/product",        "Products",    "\xe2\x96\xa1", NULL },
	{ "/e/inventory_item", "Inventory",   "\xe2\x96\xa6", NULL },
	{ "/e/expense",        "Expenses",    "\xe2\x86\x98", "Money" },
	{ "/e/account",        "Accounts",    "\xe2\x8a\x9e", NULL },
	{ "/e/tax_category",   "Tax",         "\xc2\xa7",     NULL },
	{ "/e/contact",        "Contacts",    "\xe2\x97\x8b", "Relations" },
	{ "/e/deal",           "Deals",       "\xe2\x86\x92", NULL },
	{ "/e/campaign",       "Campaigns",   "\xe2\x97\x8e", "Growth" },
	{ "/e/newsletter",     "Newsletters", "\xe2\x9c\x89", NULL },
	{ "/e/post",           "Posts",       "\xe2\x96\xa5", NULL },
	{ "/e/idea",           "Ideas",       "\xe2\x97\x87", "Thinking" },
	{ "/e/task",           "Tasks",       "\xe2\x9c\x93", NULL },
	{ "/e/research_note",  "Research",    "\xe2\x96\xa3", NULL },
	{ "/entities",         "Entities",    "\xe2\x97\xa7", "System" },
	{ "/account",          "Your account","\xe2\x97\x8f", NULL },
	{ "/users",            "Users",       "\xe2\x97\x8b", NULL },
	{ "/settings",         "Settings",    "\xe2\x9a\x99", NULL },
	{ "/e/audit_entry",    "Audit log",   "\xe2\x97\x8b", NULL },
	{ NULL, NULL, NULL, NULL }
};

const VentureWebNavLink *
venture_web_navigation(void)
{
	return venture_web_nav_links;
}

static gchar *
venture_web_page(
	VentureWebServer	*self,
	HtmxRequest		*request,
	const gchar		*active,
	const gchar		*title,
	const gchar		*content
){
	g_autoptr(GString) html = NULL;
	g_autofree gchar *ui_title = NULL;
	g_autofree gchar *accent = NULL;
	gboolean chat_dock;
	gboolean dock_expanded;

	g_object_get(venture_context_get_config(self->context),
	             "ui-title", &ui_title,
	             "ui-accent", &accent,
	             "ui-chat-dock", &chat_dock,
	             "ui-chat-dock-expanded", &dock_expanded,
	             NULL);

	html = g_string_new("<!doctype html><html lang=\"en\"><head>");
	g_string_append(html, "<meta charset=\"utf-8\">");
	g_string_append(html, "<meta name=\"viewport\" "
	                      "content=\"width=device-width, initial-scale=1\">");
	g_string_append(html, "<title>");
	venture_html_escape_append(html, title);
	g_string_append(html, " &middot; ");
	venture_html_escape_append(html, ui_title);
	g_string_append(html, "</title>");

	/*
	 * The theme is applied before the stylesheet so a dark-theme reload
	 * never flashes white. This has to be inline and synchronous; a
	 * deferred script would be too late.
	 */
	g_string_append(html,
		"<script>(function(){try{var t=localStorage.getItem('venture.theme');"
		"if(t==='light'||t==='dark')document.documentElement"
		".setAttribute('data-theme',t);}catch(e){}})();</script>");

	g_string_append(html, "<style>");
	g_string_append(html, venture_asset_venture_css);
	g_string_append(html, "</style>");

	/* The configured accent overrides the stylesheet's default without
	 * needing the stylesheet regenerated. */
	g_string_append(html, "<style>:root{--accent:");
	venture_html_escape_append(html, accent);
	g_string_append(html, ";}</style>");

	g_string_append(html, "</head><body><div class=\"app\">");

	/* Sidebar */
	g_string_append(html, "<nav class=\"sidebar\">");
	g_string_append(html, "<a class=\"brand\" href=\"/\">"
	                      "<span class=\"brand-mark\">V</span>");
	venture_html_escape_append(html, ui_title);
	g_string_append(html, "</a>");

	venture_web_append_entity_picker(self, request, html, active);

	{
		const VentureWebNavLink *links;
		gsize i;

		links = venture_web_navigation();

		g_string_append(html, "<div class=\"nav\">");

		for (i = 0; NULL != links[i].path; i++)
		{
			if (NULL != links[i].section)
			{
				g_string_append(html, "<div class=\"nav-section\">");
				venture_html_escape_append(html, links[i].section);
				g_string_append(html, "</div>");
			}

			g_string_append_printf(html, "<a class=\"nav-item%s\" href=\"%s\">",
				(0 == g_strcmp0(active, links[i].path)) ? " active" : "",
				links[i].path);
			g_string_append(html, "<span class=\"icon\">");
			g_string_append(html, links[i].icon);
			g_string_append(html, "</span>");
			venture_html_escape_append(html, links[i].label);
			g_string_append(html, "</a>");
		}

		g_string_append(html, "</div>");
	}

	g_string_append(html, "<div class=\"sidebar-footer\">");

	/*
	 * Who is signed in, above the button that signs them out. With more
	 * than one account it is the difference between signing out and
	 * wondering why the figures look wrong.
	 */
	{
		g_autoptr(VentureAuthPrincipal) principal = NULL;

		principal = venture_auth_authenticate(self->auth, request);

		if ((NULL != principal) && principal->authenticated &&
		    !venture_string_is_empty(principal->name))
		{
			g_string_append(html, "<div class=\"sidebar-user\">"
			                      "<span class=\"name\">");
			venture_html_escape_append(html, principal->name);
			g_string_append(html, "</span><span class=\"role\">");
			venture_html_escape_append(html,
				venture_enum_to_nick(VENTURE_TYPE_USER_ROLE,
				                     (gint)principal->role));
			g_string_append(html, "</span></div>");
		}
	}

	g_string_append(html, "<button class=\"btn btn-ghost btn-sm\" "
	                      "data-theme-toggle>Theme</button>");
	g_string_append(html, "<a class=\"btn btn-ghost btn-sm\" "
	                      "href=\"/logout\">Sign out</a>");
	g_string_append(html, "</div></nav>");

	/* Main */
	g_string_append(html, "<main class=\"main\">");
	g_string_append(html, content);
	g_string_append(html, "</main>");

	/* AI dock */
	if (chat_dock)
	{
		g_string_append_printf(html, "<section class=\"dock%s\">",
		                       dock_expanded ? " open" : "");
		g_string_append(html, "<div class=\"dock-bar\">"
		                      "<span class=\"spark\">\xe2\x9c\xa6</span>"
		                      "<span class=\"dock-title\">Ask VENTURE</span>"
		                      "<span class=\"dock-hint\">Ctrl + /</span></div>");
		g_string_append(html, "<div class=\"dock-body\">");
		g_string_append(html, "<div class=\"chat-log\" id=\"chat-log\">");

		if (NULL == venture_context_get_ai_service(self->context))
		{
			/* Saying why the assistant is inert beats a box that
			 * silently does nothing when you type in it. */
			g_string_append(html, "<div class=\"notice info\">"
			                      "AI is not configured. Set a provider API "
			                      "key and restart to enable it.</div>");
		}

		g_string_append(html, "</div>");
		g_string_append(html,
			"<form class=\"chat-input\" hx-post=\"/ui/chat\" "
			"hx-target=\"#chat-log\" hx-swap=\"beforeend\">"
			"<textarea name=\"message\" rows=\"1\" "
			"placeholder=\"Ask about your ventures, or describe a change\">"
			"</textarea>"
			"<button class=\"btn btn-primary\" type=\"submit\">Send</button>"
			"</form>");
		g_string_append(html, "</div></section>");
	}

	g_string_append(html, "</div>");

	g_string_append(html, "<script>");
	g_string_append(html, venture_asset_venture_hx_js);
	g_string_append(html, "</script><script>");
	g_string_append(html, venture_asset_venture_js);
	g_string_append(html, "</script>");

	g_string_append(html, "</body></html>");

	return g_string_free(g_steal_pointer(&html), FALSE);
}

/* ==========================================================================
 * Generic REST handlers
 * ========================================================================== */

/*
 * Resolves the record type named in the path, or produces the error a client
 * should see.
 */
static gboolean
venture_web_resolve_type(
	VentureWebServer	 *self,
	GHashTable		 *params,
	GType			 *out_type,
	GError			**error
){
	const gchar *name;

	name = g_hash_table_lookup(params, "type");

	if (NULL == name)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "No record type in the path");
		return FALSE;
	}

	*out_type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(self->context), name);

	if (G_TYPE_INVALID == *out_type)
	{
		g_auto(GStrv) known = NULL;
		g_autofree gchar *list = NULL;

		known = venture_entity_registry_list_names(
			venture_context_get_entity_registry(self->context));
		list = g_strjoinv(", ", known);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no record type called \"%s\". Known types: %s",
		            name, list);
		return FALSE;
	}

	return TRUE;
}

/*
 * Builds a query from the request's URL parameters, scoped to the caller's
 * organisation.
 */
static VentureQuery *
venture_web_build_query(
	VentureWebServer	 *self,
	GType			  entity_type,
	HtmxRequest		 *request,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	GHashTable *url_params;

	query = venture_query_new(entity_type);
	url_params = htmx_request_get_query_params(request);

	if (!venture_query_apply_query_string(query, url_params, error))
		return NULL;

	/*
	 * Scoped to whichever entity the browser has selected, and to every
	 * entity beneath it -- that is what keeps personal records out of a
	 * business view, and what makes a parent entity show the whole family.
	 * An explicit organization_id in the query string wins, so the API
	 * can still address one entity directly.
	 */
	if ((NULL == url_params) ||
	    !g_hash_table_contains(url_params, "organization_id"))
	{
		venture_web_scope_to_active_organization(self, request, query);
	}

	if (0 == venture_query_get_limit(query))
	{
		gint64 page_size;

		g_object_get(venture_context_get_config(self->context),
		             "ui-page-size", &page_size, NULL);
		venture_query_set_limit(query, (guint)page_size);
	}

	return g_steal_pointer(&query);
}

static HtmxResponse *
venture_web_api_list(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) records = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	GType entity_type;
	gint64 total;
	guint i;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	query = venture_web_build_query(self, entity_type, request, &error);

	if (NULL == query)
		return venture_web_error_response(error);

	records = venture_database_find(venture_context_get_database(self->context),
	                                query, &error);

	if (NULL == records)
		return venture_web_error_response(error);

	total = venture_database_count(venture_context_get_database(self->context),
	                               query, NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "total");
	json_builder_add_int_value(builder, total);

	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder, (gint64)records->len);

	json_builder_set_member_name(builder, "records");
	json_builder_begin_array(builder);

	for (i = 0; i < records->len; i++)
	{
		/* Never with sensitive fields: this is an API response, and
		 * password hashes and token secrets must not appear in one. */
		json_builder_add_value(builder,
			venture_serializable_to_json(
				VENTURE_SERIALIZABLE(g_ptr_array_index(records, i)), FALSE));
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_get(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	GType entity_type;
	const gchar *id_text;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	id_text = g_hash_table_lookup(params, "id");

	record = venture_database_get(venture_context_get_database(self->context),
	                              entity_type,
	                              g_ascii_strtoll(id_text, NULL, 10), &error);

	if (NULL == record)
	{
		if (NULL == error)
		{
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "No such record: %s", id_text);
		}

		return venture_web_error_response(error);
	}

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(record), FALSE);

	return venture_web_json_response(node, 200);
}

/*
 * Applies a JSON body to a record and saves it. Shared by create and update
 * so the two cannot drift in what they accept.
 */
static HtmxResponse *
venture_web_api_write(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureEntity		*record,
	VentureAuthPrincipal	*principal,
	gboolean		 created
){
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;

	body = htmx_request_get_json(request, &error);

	if (NULL == body)
	{
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "The request body must be a JSON object");
		return venture_web_error_response(error);
	}

	if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(record), body,
	                                    &error))
		return venture_web_error_response(error);

	/*
	 * A password is not a field. It is marked sensitive so that it never
	 * appears in a response, which also means from_json ignores it -- and
	 * without this, `create user ... password=x` would report success and
	 * produce an account that can never log in.
	 *
	 * Only an owner may set one, whether creating an account or changing
	 * an existing one; an editor who could would be granting access.
	 */
	/*
	 * A new account is active unless the caller says otherwise. Inactive
	 * is a deliberate state -- somebody who has left -- and defaulting to
	 * it produces an account that was just created with a password and
	 * still cannot log in, with nothing in the response to explain why.
	 */
	if (VENTURE_IS_USER(record) && created &&
	    (!JSON_NODE_HOLDS_OBJECT(body) ||
	     !json_object_has_member(json_node_get_object(body), "active")))
	{
		g_object_set(record, "active", TRUE, NULL);
	}

	if (VENTURE_IS_USER(record) &&
	    JSON_NODE_HOLDS_OBJECT(body) &&
	    json_object_has_member(json_node_get_object(body), "password"))
	{
		const gchar *password;

		if (!venture_auth_require(self->auth, principal,
		                          VENTURE_USER_ROLE_OWNER, &error))
			return venture_web_error_response(error);

		password = json_object_get_string_member(json_node_get_object(body),
		                                         "password");

		if (!venture_auth_set_password(self->auth, VENTURE_USER(record),
		                               password, &error))
			return venture_web_error_response(error);
	}

	/* A record that names no organisation belongs to the caller's, rather
	 * than to organisation zero where nothing would ever find it. */
	if (0 == venture_entity_get_organization_id(record))
	{
		venture_entity_set_organization_id(record,
			venture_context_get_default_organization_id(self->context));
	}

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           record, &actor, &error))
		return venture_web_error_response(error);

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(record), FALSE);

	return venture_web_json_response(node, created ? 201 : 200);
}

static HtmxResponse *
venture_web_api_create(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) error = NULL;
	GType entity_type;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	record = g_object_new(entity_type, NULL);

	return venture_web_api_write(self, request, record, principal, TRUE);
}

static HtmxResponse *
venture_web_api_update(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) error = NULL;
	GType entity_type;
	const gchar *id_text;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	id_text = g_hash_table_lookup(params, "id");
	record = venture_database_get(venture_context_get_database(self->context),
	                              entity_type,
	                              g_ascii_strtoll(id_text, NULL, 10), &error);

	if (NULL == record)
	{
		if (NULL == error)
		{
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "No such record: %s", id_text);
		}

		return venture_web_error_response(error);
	}

	return venture_web_api_write(self, request, record, principal, FALSE);
}

static HtmxResponse *
venture_web_api_delete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	GType entity_type;
	const gchar *id_text;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	id_text = g_hash_table_lookup(params, "id");
	record = venture_database_get(venture_context_get_database(self->context),
	                              entity_type,
	                              g_ascii_strtoll(id_text, NULL, 10), &error);

	if (NULL == record)
	{
		if (NULL == error)
		{
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "No such record: %s", id_text);
		}

		return venture_web_error_response(error);
	}

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_delete(venture_context_get_database(self->context),
	                             record, &actor, &error))
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "deleted");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, venture_entity_get_id(record));
	/* Saying it is recoverable matters: a client should not present this
	 * as destruction when the row is still there. */
	json_builder_set_member_name(builder, "recoverable");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_describe(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *denied;
	const gchar *name;

	self = user_data;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;
	name = g_hash_table_lookup(params, "type");

	if (NULL == name)
	{
		node = venture_entity_registry_describe_all(
			venture_context_get_entity_registry(self->context));
	}
	else
	{
		node = venture_entity_registry_describe(
			venture_context_get_entity_registry(self->context), name);
	}

	if (NULL == node)
	{
		g_autoptr(GError) error = NULL;

		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no record type called \"%s\"", name);
		return venture_web_error_response(error);
	}

	return venture_web_json_response(node, 200);
}

/* --- Reports ------------------------------------------------------------- */

static HtmxResponse *
venture_web_api_reports(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *denied;

	self = user_data;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;
	node = venture_report_registry_describe(
		venture_context_get_report_registry(self->context));

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_report(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	const gchar *name;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	name = g_hash_table_lookup(params, "name");
	report = venture_report_registry_lookup(
		venture_context_get_report_registry(self->context), name);

	if (NULL == report)
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no report called \"%s\"", name);
		return venture_web_error_response(error);
	}

	period = venture_context_parse_period(self->context,
		htmx_request_get_query_param(request, "period"), &error);

	if (NULL == period)
		return venture_web_error_response(error);

	result = venture_report_generate(report, self->context, period, NULL,
	                                 &error);

	if (NULL == result)
		return venture_web_error_response(error);

	{
		const gchar *format;

		format = htmx_request_get_query_param(request, "format");

		/* CSV is offered because a report a person wants to keep goes
		 * into a spreadsheet, and asking them to convert JSON would be
		 * a poor answer. */
		if (0 == g_strcmp0(format, "csv"))
		{
			HtmxResponse *response;
			g_autofree gchar *body = NULL;

			body = venture_report_result_render(result,
				VENTURE_OUTPUT_FORMAT_CSV);
			response = htmx_response_new_with_content(body);
			htmx_response_set_content_type(response, "text/csv; charset=utf-8");

			return response;
		}
	}

	node = venture_report_result_to_json(result);

	return venture_web_json_response(node, 200);
}

/* ==========================================================================
 * UI pages
 * ========================================================================== */

static HtmxResponse *
venture_web_ui_dashboard(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GError) error = NULL;
	static const gchar *const dashboard_reports[] = {
		"pnl", "ventures", NULL
	};
	gsize i;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!principal->authenticated)
	{
		HtmxResponse *response;

		response = htmx_response_new();
		htmx_response_set_status(response, 302);
		htmx_response_add_header(response, "Location", "/login");

		return response;
	}

	content = g_string_new(NULL);
	g_string_append(content, "<div class=\"page-head\"><div class=\"page-title\">"
	                         "<h1>Dashboard</h1>"
	                         "<span class=\"subtitle\">This month across every "
	                         "venture</span></div></div>");

	period = venture_context_parse_period(self->context, "this_month", &error);

	for (i = 0; NULL != dashboard_reports[i]; i++)
	{
		g_autoptr(VentureReportResult) result = NULL;
		g_autoptr(GError) local_error = NULL;
		g_autofree gchar *rendered = NULL;
		VentureReport *report;

		report = venture_report_registry_lookup(
			venture_context_get_report_registry(self->context),
			dashboard_reports[i]);

		if (NULL == report)
			continue;

		result = venture_report_generate(report, self->context, period, NULL,
		                                 &local_error);

		if (NULL == result)
		{
			/* One failing report must not take the dashboard down;
			 * the others are still useful and the failure is shown
			 * in place. */
			g_string_append(content, "<div class=\"notice negative\">");
			venture_html_escape_append(content, local_error->message);
			g_string_append(content, "</div>");
			continue;
		}

		rendered = venture_report_result_render(result,
			VENTURE_OUTPUT_FORMAT_HTML);
		g_string_append(content, rendered);
		g_string_append(content, "<div style=\"height:16px\"></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/", "Dashboard", content->str), 200);
}

/*
 * Renders a record list. The columns come from the type's field specs, so a
 * plugin's record type gets a usable list view with no UI code.
 */
static HtmxResponse *
venture_web_ui_list(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) records = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	VentureEntity *prototype;
	GType entity_type;
	const gchar *type_name;
	g_autofree gchar *path = NULL;
	guint shown;
	guint i;
	guint j;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!principal->authenticated)
	{
		HtmxResponse *response;

		response = htmx_response_new();
		htmx_response_set_status(response, 302);
		htmx_response_add_header(response, "Location", "/login");

		return response;
	}

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
	{
		g_autofree gchar *body = NULL;

		body = g_strdup_printf("<div class=\"notice negative\">%s</div>",
		                       error->message);
		return venture_web_html_response(
			venture_web_page(self, request, NULL, "Not found", body), 404);
	}

	/* /e/user and /e/api_token are access control, not business data. */
	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	type_name = g_hash_table_lookup(params, "type");
	prototype = venture_entity_registry_get_prototype(
		venture_context_get_entity_registry(self->context), type_name);

	query = venture_web_build_query(self, entity_type, request, &error);

	if (NULL == query)
	{
		g_autofree gchar *body = NULL;

		body = g_strdup_printf("<div class=\"notice negative\">%s</div>",
		                       error->message);
		return venture_web_html_response(
			venture_web_page(self, request, NULL, "Bad request", body), 400);
	}

	records = venture_database_find(venture_context_get_database(self->context),
	                                query, &error);

	if (NULL == records)
	{
		g_autofree gchar *body = NULL;

		body = g_strdup_printf("<div class=\"notice negative\">%s</div>",
		                       error->message);
		return venture_web_html_response(
			venture_web_page(self, request, NULL, "Error", body), 500);
	}

	specs = venture_entity_get_field_specs(prototype);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	path = g_strdup_printf("/e/%s", type_name);
	content = g_string_new(NULL);

	g_string_append(content, "<div class=\"page-head\"><div class=\"page-title\">"
	                         "<h1>");
	venture_html_escape_append(content, type_name);
	g_string_append(content, "</h1><span class=\"subtitle\">");
	g_string_append_printf(content, "%u record%s", records->len,
	                       (1 == records->len) ? "" : "s");
	g_string_append(content, "</span></div><div class=\"page-actions\">");
	g_string_append_printf(content,
		"<input type=\"search\" name=\"search\" placeholder=\"Search\" "
		"data-search-input hx-get=\"%s\" hx-trigger=\"keyup changed delay:300ms\" "
		"hx-target=\"body\" style=\"width:220px\">", path);
	g_string_append_printf(content,
		"<a class=\"btn btn-primary\" href=\"/e/%s/new\">New</a>", type_name);
	g_string_append(content, "</div></div>");

	g_string_append(content, "<div class=\"card\"><div class=\"table-wrap\">"
	                         "<table class=\"data\"><thead><tr>");

	shown = 0;

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		/* A list with forty columns is unreadable; the first several
		 * declared fields are the ones that identify a record. */
		if (!venture_field_spec_get_show_in_list(spec) || (shown >= 7))
			continue;

		g_string_append(content, "<th>");
		venture_html_escape_append(content, venture_field_spec_get_label(spec));
		g_string_append(content, "</th>");
		shown++;
	}

	g_string_append(content, "<th class=\"row-actions\"></th>");
	g_string_append(content, "</tr></thead><tbody>");

	for (j = 0; j < records->len; j++)
	{
		VentureEntity *record;

		record = g_ptr_array_index(records, j);
		shown = 0;

		g_string_append(content, "<tr>");

		for (i = 0; i < specs->len; i++)
		{
			VentureFieldSpec *spec;
			g_auto(GValue) value = G_VALUE_INIT;
			g_autofree gchar *text = NULL;

			spec = g_ptr_array_index(specs, i);

			if (!venture_field_spec_get_show_in_list(spec) || (shown >= 7))
				continue;

			if (!venture_entity_get_field(record,
			                              venture_field_spec_get_name(spec),
			                              &value))
			{
				g_string_append(content, "<td></td>");
				shown++;
				continue;
			}

			if (G_VALUE_HOLDS(&value, VENTURE_TYPE_MONEY))
			{
				const VentureMoney *money;

				money = g_value_get_boxed(&value);
				text = (NULL != money)
					? venture_money_to_display_string(money, TRUE)
					: g_strdup("");

				g_string_append(content, "<td class=\"num\">");
			}
			else if (G_VALUE_HOLDS(&value, G_TYPE_DATE_TIME))
			{
				text = venture_time_to_date_string(
					g_value_get_boxed(&value),
					venture_context_get_timezone(self->context));

				if (NULL == text)
					text = g_strdup("");

				g_string_append(content, "<td>");
			}
			else if (G_VALUE_HOLDS_STRING(&value))
			{
				text = venture_truncate(g_value_get_string(&value), 60);

				if (NULL == text)
					text = g_strdup("");

				g_string_append(content, "<td>");
			}
			else if (G_VALUE_HOLDS_ENUM(&value))
			{
				/* The nick, not the JSON rendering: a status cell
				 * reading &quot;active&quot; is the quoting of a
				 * serialisation leaking into a table. */
				text = g_strdup(venture_enum_to_nick(G_VALUE_TYPE(&value),
				                                     g_value_get_enum(&value)));

				if (NULL == text)
					text = g_strdup("");

				g_string_append(content, "<td>");
			}
			else if (G_VALUE_HOLDS_BOOLEAN(&value))
			{
				text = g_strdup(g_value_get_boolean(&value) ? "yes" : "");
				g_string_append(content, "<td>");
			}
			else
			{
				g_autoptr(JsonNode) node = NULL;

				node = venture_json_node_from_value(&value);
				text = venture_json_to_string(node, FALSE);

				if (0 == g_strcmp0(text, "null"))
				{
					g_free(text);
					text = g_strdup("");
				}

				g_string_append(content, "<td class=\"num\">");
			}

			venture_html_escape_append(content, text);
			g_string_append(content, "</td>");
			shown++;
		}

		g_string_append_printf(content,
			"<td class=\"row-actions\">"
			"<a class=\"btn btn-sm\" href=\"/e/%s/%" G_GINT64_FORMAT
			"/edit\">Edit</a></td>",
			type_name, venture_entity_get_id(record));

		g_string_append(content, "</tr>");
	}

	g_string_append(content, "</tbody></table></div>");

	if (0 == records->len)
	{
		g_string_append(content, "<div class=\"empty\">"
		                         "<span class=\"empty-icon\">\xe2\x97\x8b</span>"
		                         "<h3>Nothing here yet</h3>"
		                         "<p class=\"muted\">Records you add will "
		                         "appear in this list.</p></div>");
	}

	g_string_append(content, "</div>");

	return venture_web_html_response(
		venture_web_page(self, request, path, type_name, content->str), 200);
}

static HtmxResponse *
venture_web_ui_reports(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(GPtrArray) reports = NULL;
	g_autoptr(GString) content = NULL;
	HtmxResponse *redirect;
	guint i;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	reports = venture_report_registry_list(
		venture_context_get_report_registry(self->context));

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Reports</h1></div></div>"
	                       "<div class=\"grid cols-2\">");

	for (i = 0; i < reports->len; i++)
	{
		VentureReport *report;

		report = g_ptr_array_index(reports, i);

		g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
		                         "<h2>");
		venture_html_escape_append(content, venture_report_get_title(report));
		g_string_append(content, "</h2><p class=\"muted\">");
		venture_html_escape_append(content,
			venture_report_get_description(report));
		g_string_append(content, "</p>");
		g_string_append_printf(content,
			"<a class=\"btn btn-primary btn-sm\" href=\"/reports/%s\">Open</a> "
			"<a class=\"btn btn-sm\" "
			"href=\"/api/v1/reports/%s?format=csv\">CSV</a>",
			venture_report_get_name(report),
			venture_report_get_name(report));
		g_string_append(content, "</div></div>");
	}

	g_string_append(content, "</div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/reports", "Reports", content->str), 200);
}

static HtmxResponse *
venture_web_ui_report(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *rendered = NULL;
	VentureReport *report;
	HtmxResponse *redirect;
	const gchar *name;
	const gchar *requested_period;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	name = g_hash_table_lookup(params, "name");
	report = venture_report_registry_lookup(
		venture_context_get_report_registry(self->context), name);

	if (NULL == report)
	{
		return venture_web_html_response(
			venture_web_page(self, request, "/reports", "Not found",
				"<div class=\"notice negative\">No such report.</div>"), 404);
	}

	requested_period = htmx_request_get_query_param(request, "period");
	period = venture_context_parse_period(self->context, requested_period,
	                                      &error);

	if (NULL == period)
	{
		g_autofree gchar *body = NULL;

		body = g_strdup_printf("<div class=\"notice negative\">%s</div>",
		                       error->message);
		return venture_web_html_response(
			venture_web_page(self, request, "/reports", "Bad period", body), 400);
	}

	result = venture_report_generate(report, self->context, period, NULL,
	                                 &error);

	if (NULL == result)
	{
		g_autofree gchar *body = NULL;

		body = g_strdup_printf("<div class=\"notice negative\">%s</div>",
		                       error->message);
		return venture_web_html_response(
			venture_web_page(self, request, "/reports", "Error", body), 500);
	}

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>");
	venture_html_escape_append(content, venture_report_get_title(report));
	g_string_append(content, "</h1></div><div class=\"page-actions\">");

	{
		static const gchar *const periods[] = {
			"this_month", "last_month", "this_quarter", "ytd", "this_year",
			NULL
		};
		gsize i;

		g_string_append(content, "<div class=\"btn-group\">");

		for (i = 0; NULL != periods[i]; i++)
		{
			g_string_append_printf(content,
				"<a class=\"btn btn-sm%s\" href=\"/reports/%s?period=%s\">%s</a>",
				(0 == g_strcmp0(requested_period, periods[i])) ? " active" : "",
				name, periods[i], periods[i]);
		}

		g_string_append(content, "</div>");
	}

	g_string_append_printf(content,
		"<a class=\"btn btn-sm\" href=\"/api/v1/reports/%s?format=csv&period=%s\">"
		"Export CSV</a>", name,
		(NULL != requested_period) ? requested_period : "this_month");
	g_string_append(content, "</div></div>");

	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_HTML);
	g_string_append(content, rendered);

	return venture_web_html_response(
		venture_web_page(self, request, "/reports", venture_report_get_title(report),
		                 content->str), 200);
}

/* --- Login --------------------------------------------------------------- */

static HtmxResponse *
venture_web_ui_login_form(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *ui_title = NULL;

	self = user_data;
	g_object_get(venture_context_get_config(self->context), "ui-title",
	             &ui_title, NULL);

	html = g_string_new("<!doctype html><html lang=\"en\"><head>"
	                    "<meta charset=\"utf-8\">"
	                    "<meta name=\"viewport\" content=\"width=device-width, "
	                    "initial-scale=1\"><title>Sign in</title><style>");
	g_string_append(html, venture_asset_venture_css);
	g_string_append(html, "</style></head><body>");
	g_string_append(html, "<div style=\"display:grid;place-items:center;"
	                      "min-height:100vh;padding:24px\">"
	                      "<div class=\"card\" style=\"width:min(380px,100%)\">"
	                      "<div class=\"card-body\">");
	g_string_append(html, "<h1 style=\"margin-bottom:20px\">");
	venture_html_escape_append(html, ui_title);
	g_string_append(html, "</h1>");
	g_string_append(html,
		"<form method=\"post\" action=\"/login\">"
		"<div class=\"field\"><label for=\"u\">Username</label>"
		"<input id=\"u\" type=\"text\" name=\"username\" "
		"autocomplete=\"username\" autofocus>"
		"</div>"
		"<div class=\"field\"><label for=\"p\">Password</label>"
		"<input id=\"p\" name=\"password\" type=\"password\" "
		"autocomplete=\"current-password\"></div>"
		"<button class=\"btn btn-primary btn-lg\" style=\"width:100%\" "
		"type=\"submit\">Sign in</button></form>");
	g_string_append(html, "</div></div></div></body></html>");

	return venture_web_html_response(g_string_free(g_steal_pointer(&html),
	                                               FALSE), 200);
}

static HtmxResponse *
venture_web_ui_login_submit(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autofree gchar *cookie = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *response;

	self = user_data;

	if (!venture_auth_login(self->auth,
	                        htmx_request_get_form_value(request, "username"),
	                        htmx_request_get_form_value(request, "password"),
	                        &cookie, &error))
	{
		g_autoptr(GString) html = NULL;

		html = g_string_new("<!doctype html><html><head><meta charset=\"utf-8\">"
		                    "<title>Sign in</title><style>");
		g_string_append(html, venture_asset_venture_css);
		g_string_append(html, "</style></head><body>"
		                      "<div style=\"display:grid;place-items:center;"
		                      "min-height:100vh;padding:24px\">"
		                      "<div class=\"card\" style=\"width:min(380px,100%)\">"
		                      "<div class=\"card-body\">"
		                      "<div class=\"notice negative\">");
		venture_html_escape_append(html, error->message);
		g_string_append(html, "</div>"
		                      "<a class=\"btn\" href=\"/login\">Try again</a>"
		                      "</div></div></div></body></html>");

		return venture_web_html_response(
			g_string_free(g_steal_pointer(&html), FALSE), 401);
	}

	response = htmx_response_new();
	htmx_response_set_status(response, 302);
	htmx_response_add_header(response, "Location", "/");
	htmx_response_add_header(response, "Set-Cookie", cookie);

	return response;
}

static HtmxResponse *
venture_web_ui_logout(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autofree gchar *cookie = NULL;
	HtmxResponse *response;

	self = user_data;

	/*
	 * Invalidate the session server-side before clearing the browser's
	 * copy. Clearing the cookie alone leaves anybody holding a copy of it
	 * signed in until it expires, which is precisely the situation
	 * somebody signing out on a shared machine is trying to avoid.
	 */
	{
		g_autoptr(VentureAuthPrincipal) principal = NULL;
		g_autoptr(GError) error = NULL;

		principal = venture_auth_authenticate(self->auth, request);

		if ((NULL != principal) && principal->authenticated &&
		    !venture_auth_end_sessions(self->auth, principal->user_id, &error))
		{
			/* Signing out still proceeds: a browser left holding a
			 * live cookie is worse than a failed audit write. */
			g_warning("Could not end sessions for user %" G_GINT64_FORMAT
			          ": %s", principal->user_id,
			          (NULL != error) ? error->message : "unknown failure");
		}
	}

	cookie = venture_auth_logout_cookie(self->auth);

	response = htmx_response_new();
	htmx_response_set_status(response, 302);
	htmx_response_add_header(response, "Location", "/login");
	htmx_response_add_header(response, "Set-Cookie", cookie);

	return response;
}


/*
 * Whether @id is one of @organizations, used to tell a real parent from a
 * dangling reference.
 */
static gboolean
venture_web_organization_exists(
	VentureWebServer	*self,
	GPtrArray		*organizations,
	gint64			 id
){
	guint i;

	for (i = 0; i < organizations->len; i++)
	{
		if (venture_entity_get_id(g_ptr_array_index(organizations, i)) == id)
			return TRUE;
	}

	return FALSE;
}

/* --- Record forms -------------------------------------------------------- */

/*
 * Renders one form control for @spec, pre-filled from @record when editing.
 *
 * The control is chosen from the field's kind, which is the same metadata
 * the schema builder, the list view and the AI tool schema read. That is the
 * point of deriving everything from one field table: a record type gains a
 * working form the moment it is registered, including one from a plugin,
 * without anybody writing HTML for it.
 */
static void
venture_web_append_form_field(
	VentureWebServer	*self,
	GString			*content,
	VentureFieldSpec	*spec,
	VentureEntity		*record
){
	g_auto(GValue) value = G_VALUE_INIT;
	g_autofree gchar *current = NULL;
	const gchar *name;
	const gchar *label;
	const gchar *help;
	VentureFieldKind kind;
	gboolean required;
	gboolean has_value;

	name = venture_field_spec_get_name(spec);
	label = venture_field_spec_get_label(spec);
	help = venture_field_spec_get_help(spec);
	kind = venture_field_spec_get_kind(spec);
	required = venture_field_spec_get_required(spec);

	has_value = (NULL != record) &&
		venture_entity_get_field(record, name, &value);

	if (has_value)
	{
		if (G_VALUE_HOLDS(&value, VENTURE_TYPE_MONEY))
		{
			const VentureMoney *money;

			money = g_value_get_boxed(&value);

			/* The plain decimal, not the formatted display string: this
			 * goes back through the same parser that reads a CLI
			 * argument, and "$1,234.00" is not something it accepts. */
			current = (NULL != money)
				? venture_money_to_string(money) : NULL;
		}
		else if (G_VALUE_HOLDS(&value, G_TYPE_DATE_TIME))
		{
			GDateTime *when;

			when = g_value_get_boxed(&value);
			current = (NULL != when)
				? g_date_time_format(when, "%Y-%m-%d") : NULL;
		}
		else if (G_VALUE_HOLDS_BOOLEAN(&value))
		{
			current = g_strdup(g_value_get_boolean(&value) ? "true" : "false");
		}
		else if (G_VALUE_HOLDS_INT64(&value))
		{
			gint64 number;

			number = g_value_get_int64(&value);

			/* A zero foreign key means "unset", not "record zero". */
			current = ((0 == number) &&
			           (VENTURE_FIELD_KIND_REFERENCE == kind))
				? NULL : g_strdup_printf("%" G_GINT64_FORMAT, number);
		}
		else if (G_VALUE_HOLDS_ENUM(&value))
		{
			current = g_strdup(venture_enum_to_nick(
				G_VALUE_TYPE(&value), g_value_get_enum(&value)));
		}
		else if (G_VALUE_HOLDS_STRING(&value))
		{
			current = g_value_dup_string(&value);
		}
	}

	g_string_append(content, "<div class=\"field\"><label>");
	venture_html_escape_append(content, label);

	if (required)
		g_string_append(content, " <span class=\"required\">*</span>");

	switch (kind)
	{
	case VENTURE_FIELD_KIND_TEXT:
		g_string_append_printf(content,
			"<textarea name=\"%s\" rows=\"4\"%s>", name,
			required ? " required" : "");
		venture_html_escape_append(content, current);
		g_string_append(content, "</textarea>");
		break;

	case VENTURE_FIELD_KIND_BOOLEAN:
		/*
		 * A hidden false before the checkbox, so that clearing a box
		 * sends "false" rather than nothing at all -- an unchecked box
		 * is simply absent from the submission, which would otherwise
		 * read as "leave it alone" and make it impossible to turn
		 * anything off.
		 */
		g_string_append_printf(content,
			"<input type=\"hidden\" name=\"%s\" value=\"false\">"
			"<input type=\"checkbox\" name=\"%s\" value=\"true\"%s>",
			name, name,
			(0 == g_strcmp0(current, "true")) ? " checked" : "");
		break;

	case VENTURE_FIELD_KIND_ENUM:
		{
			const gchar *const *choices;
			gsize i;

			choices = venture_field_spec_get_choices(spec);

			g_string_append_printf(content, "<select name=\"%s\">", name);

			if (!required)
				g_string_append(content, "<option value=\"\">—</option>");

			for (i = 0; (NULL != choices) && (NULL != choices[i]); i++)
			{
				g_string_append_printf(content, "<option value=\"%s\"%s>",
					choices[i],
					(0 == g_strcmp0(choices[i], current)) ? " selected" : "");
				venture_html_escape_append(content, choices[i]);
				g_string_append(content, "</option>");
			}

			g_string_append(content, "</select>");
		}
		break;

	case VENTURE_FIELD_KIND_REFERENCE:
		{
			g_autoptr(VentureQuery) query = NULL;
			g_autoptr(GPtrArray) options = NULL;
			GType target;

			target = venture_entity_registry_lookup(
				venture_context_get_entity_registry(self->context),
				venture_field_spec_get_reference_type(spec));

			g_string_append_printf(content, "<select name=\"%s\">", name);
			g_string_append(content, "<option value=\"\">—</option>");

			if (G_TYPE_INVALID != target)
			{
				query = venture_query_new(target);
				venture_query_set_limit(query, 0);

				options = venture_database_find(
					venture_context_get_database(self->context), query, NULL);
			}

			/*
			 * Every candidate, not a paged slice: a picker that silently
			 * omits the record you are looking for is worse than a long
			 * list, and these tables are small by nature.
			 */
			if (NULL != options)
			{
				guint i;

				for (i = 0; i < options->len; i++)
				{
					g_autofree gchar *display = NULL;
					VentureEntity *option;
					gint64 id;

					option = g_ptr_array_index(options, i);
					id = venture_entity_get_id(option);
					display = venture_entity_get_display_name(option);

					g_string_append_printf(content,
						"<option value=\"%" G_GINT64_FORMAT "\"%s>", id,
						((NULL != current) &&
						 (g_ascii_strtoll(current, NULL, 10) == id))
							? " selected" : "");
					venture_html_escape_append(content, display);
					g_string_append(content, "</option>");
				}
			}

			g_string_append(content, "</select>");
		}
		break;

	case VENTURE_FIELD_KIND_DATE:
	case VENTURE_FIELD_KIND_DATETIME:
		g_string_append_printf(content,
			"<input type=\"date\" name=\"%s\" value=\"%s\"%s>",
			name, (NULL != current) ? current : "",
			required ? " required" : "");
		break;

	case VENTURE_FIELD_KIND_INTEGER:
		g_string_append_printf(content,
			"<input type=\"number\" step=\"1\" name=\"%s\" value=\"%s\"%s>",
			name, (NULL != current) ? current : "",
			required ? " required" : "");
		break;

	case VENTURE_FIELD_KIND_DOUBLE:
		g_string_append_printf(content,
			"<input type=\"number\" step=\"any\" name=\"%s\" value=\"%s\"%s>",
			name, (NULL != current) ? current : "",
			required ? " required" : "");
		break;

	case VENTURE_FIELD_KIND_MONEY:
		/*
		 * Text rather than number: a money field accepts "12.34 USD" as
		 * well as "12.34", and a number input would refuse the currency.
		 */
		g_string_append_printf(content,
			"<input type=\"text\" inputmode=\"decimal\" name=\"%s\" "
			"value=\"%s\" placeholder=\"0.00\"%s>",
			name, (NULL != current) ? current : "",
			required ? " required" : "");
		break;

	default:
		g_string_append_printf(content,
			"<input type=\"text\" name=\"%s\" value=\"", name);
		venture_html_escape_append(content, current);
		g_string_append_printf(content, "\"%s>", required ? " required" : "");
		break;
	}

	g_string_append(content, "</label>");

	if (!venture_string_is_empty(help))
	{
		g_string_append(content, "<div class=\"field-help\">");
		venture_html_escape_append(content, help);
		g_string_append(content, "</div>");
	}

	g_string_append(content, "</div>");
}

/*
 * The new-record and edit-record form. One function for both, because the
 * only differences are the heading, where it posts and whether the fields
 * start filled.
 */
static HtmxResponse *
venture_web_ui_form(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	GType entity_type;
	const gchar *type_name;
	const gchar *id_text;
	gint64 id;
	guint i;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);

	type_name = g_hash_table_lookup(params, "type");
	id_text = g_hash_table_lookup(params, "id");
	id = (NULL != id_text) ? g_ascii_strtoll(id_text, NULL, 10) : 0;

	if (0 != id)
	{
		record = venture_database_get(
			venture_context_get_database(self->context), entity_type, id,
			&error);

		if (NULL == record)
			return venture_web_error_response(error);
	}
	else
	{
		record = g_object_new(entity_type, NULL);
	}

	specs = venture_entity_get_field_specs(record);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>");
	g_string_append(content, (0 != id) ? "Edit " : "New ");
	venture_html_escape_append(content, type_name);
	g_string_append(content, "</h1></div></div>");

	if (0 != id)
	{
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/e/%s/%" G_GINT64_FORMAT "\">",
			type_name, id);
	}
	else
	{
		g_string_append_printf(content, "<form method=\"post\" action=\"/e/%s\">",
		                       type_name);
	}

	g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
	                         "<div class=\"form-grid\">");

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;
		VentureColumnFlags flags;

		spec = g_ptr_array_index(specs, i);
		flags = venture_field_spec_get_flags(spec);

		/*
		 * A sensitive field is never rendered with its value -- that is
		 * what the flag means -- and a password is set through the user
		 * pages rather than here.
		 */
		if (0 != (flags & VENTURE_COLUMN_FLAG_SENSITIVE))
			continue;

		venture_web_append_form_field(self, content, spec, record);
	}

	g_string_append(content, "</div></div></div>");

	/*
	 * Which entity this belongs to, on every form. It is the field most
	 * likely to be wrong and the least likely to be noticed: a sale filed
	 * against the wrong business is a tax problem months later.
	 */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) organizations = NULL;
		gint64 current_organization;

		current_organization = (0 != id)
			? venture_entity_get_organization_id(record)
			: venture_web_active_organization(self, request);

		if (0 == current_organization)
		{
			current_organization =
				venture_context_get_default_organization_id(self->context);
		}

		query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		venture_query_set_limit(query, 0);
		organizations = venture_database_find(
			venture_context_get_database(self->context), query, NULL);

		if ((NULL != organizations) && (VENTURE_TYPE_ORGANIZATION != entity_type))
		{
			guint j;

			g_string_append(content,
				"<div class=\"card\"><div class=\"card-body\">"
				"<div class=\"field\"><label>Entity"
				"<select name=\"organization_id\">");

			for (j = 0; j < organizations->len; j++)
			{
				g_autofree gchar *display = NULL;
				VentureEntity *organization;
				gint64 organization_id;

				organization = g_ptr_array_index(organizations, j);
				organization_id = venture_entity_get_id(organization);
				display = venture_entity_get_display_name(organization);

				g_string_append_printf(content,
					"<option value=\"%" G_GINT64_FORMAT "\"%s>",
					organization_id,
					(organization_id == current_organization) ? " selected" : "");
				venture_html_escape_append(content, display);
				g_string_append(content, "</option>");
			}

			g_string_append(content, "</select></label>"
			                         "<div class=\"field-help\">"
			                         "Which business or personal entity this "
			                         "record belongs to.</div>"
			                         "</div></div></div>");
		}
	}

	g_string_append_printf(content,
		"<div class=\"form-actions\">"
		"<button class=\"btn btn-primary\" type=\"submit\">Save</button>"
		"<a class=\"btn\" href=\"/e/%s\">Cancel</a>"
		"</div></form>", type_name);

	if (0 != id)
	{
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/e/%s/%" G_GINT64_FORMAT "/delete\" "
			"class=\"danger-zone\">"
			"<button class=\"btn btn-danger\" type=\"submit\">Delete</button>"
			"<span class=\"muted small\">Recoverable: deleting hides the "
			"record and keeps it.</span>"
			"</form>", type_name, id);
	}

	return venture_web_html_response(
		venture_web_page(self, request, NULL,
		                 (0 != id) ? "Edit" : "New", content->str), 200);
}

/*
 * Applies a form submission to @record.
 *
 * Every value arrives as a string and goes through the same parser the CLI
 * and the API use, so "12.34 USD" means the same thing typed into a browser
 * as passed on a command line.
 */
static gboolean
venture_web_apply_form(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	VentureEntity		 *record,
	GPtrArray		 *specs,
	GError			**error
){
	guint i;

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;
		VentureColumnFlags flags;
		const gchar *name;
		const gchar *submitted;

		spec = g_ptr_array_index(specs, i);
		flags = venture_field_spec_get_flags(spec);
		name = venture_field_spec_get_name(spec);

		if (0 != (flags & VENTURE_COLUMN_FLAG_SENSITIVE))
			continue;

		submitted = htmx_request_get_form_value(request, name);

		/* Absent means the form did not carry the field at all, which is
		 * not the same as a field cleared to empty. */
		if (NULL == submitted)
			continue;

		if (venture_string_is_empty(submitted))
		{
			/* An emptied box clears the field rather than being
			 * ignored, or nothing could ever be unset. */
			if (VENTURE_FIELD_KIND_REFERENCE == venture_field_spec_get_kind(spec))
				g_object_set(record, name, (gint64)0, NULL);

			continue;
		}

		if (!venture_entity_set_field_from_string(record, name, submitted,
		                                          error))
			return FALSE;
	}

	{
		const gchar *organization;

		organization = htmx_request_get_form_value(request, "organization_id");

		if (!venture_string_is_empty(organization))
		{
			venture_entity_set_organization_id(record,
				g_ascii_strtoll(organization, NULL, 10));
		}
	}

	/*
	 * An organisation is not filed against an organisation -- the parent
	 * field is what relates one to another. Defaulting it here would make
	 * every entity a member of the default one.
	 */
	if ((0 == venture_entity_get_organization_id(record)) &&
	    !VENTURE_IS_ORGANIZATION(record))
	{
		venture_entity_set_organization_id(record,
			venture_context_get_default_organization_id(self->context));
	}

	return TRUE;
}

static HtmxResponse *
venture_web_ui_save(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	GType entity_type;
	const gchar *type_name;
	const gchar *id_text;
	gint64 id;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);

	type_name = g_hash_table_lookup(params, "type");
	id_text = g_hash_table_lookup(params, "id");
	id = (NULL != id_text) ? g_ascii_strtoll(id_text, NULL, 10) : 0;

	if (0 != id)
	{
		record = venture_database_get(
			venture_context_get_database(self->context), entity_type, id,
			&error);

		if (NULL == record)
			return venture_web_error_response(error);
	}
	else
	{
		record = g_object_new(entity_type, NULL);
	}

	specs = venture_entity_get_field_specs(record);

	if (!venture_web_apply_form(self, request, record, specs, &error))
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           record, &actor, &error))
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/%s", type_name);

	return venture_web_redirect_to(destination);
}

static HtmxResponse *
venture_web_ui_delete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	GType entity_type;
	const gchar *type_name;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);

	type_name = g_hash_table_lookup(params, "type");
	venture_auth_to_actor(principal, &actor);

	{
		g_autoptr(VentureEntity) record = NULL;

		record = venture_database_get(
			venture_context_get_database(self->context), entity_type,
			g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10),
			&error);

		if (NULL == record)
			return venture_web_error_response(error);

		/* Soft: the record is hidden and kept, because a deleted sale is
		 * still part of last quarter's figures. */
		if (!venture_database_delete(
			venture_context_get_database(self->context), record, &actor,
			&error))
			return venture_web_error_response(error);
	}

	destination = g_strdup_printf("/e/%s", type_name);

	return venture_web_redirect_to(destination);
}

/* --- The active entity --------------------------------------------------- */

#define VENTURE_WEB_ENTITY_COOKIE "venture_entity"

/*
 * Reads a cookie from the request.
 *
 * Returns: (transfer full) (nullable): the value, or %NULL
 */
static gchar *
venture_web_read_cookie(
	HtmxRequest	*request,
	const gchar	*name
){
	g_autoptr(GHashTable) cookies = NULL;
	SoupServerMessage *message;
	SoupMessageHeaders *headers;
	const gchar *header;

	message = htmx_request_get_message(request);
	headers = (NULL != message)
		? soup_server_message_get_request_headers(message) : NULL;
	header = (NULL != headers)
		? soup_message_headers_get_list(headers, "Cookie") : NULL;

	if (NULL == header)
		return NULL;

	cookies = htmx_cookie_parse_request(header);

	if (NULL == cookies)
		return NULL;

	return g_strdup(g_hash_table_lookup(cookies, name));
}

/*
 * The entity the browser is currently looking at.
 *
 * Zero means every entity at once, which is a deliberate choice rather than
 * a missing one: totals across a whole portfolio are a thing people want,
 * and the alternative -- silently defaulting to one entity -- is how records
 * end up filed against the wrong business.
 */
static gint64
venture_web_active_organization(
	VentureWebServer	*self,
	HtmxRequest		*request
){
	g_autofree gchar *selected = NULL;

	selected = venture_web_read_cookie(request, VENTURE_WEB_ENTITY_COOKIE);

	if (NULL == selected)
	{
		/* Nothing chosen yet: the default entity, which on a fresh
		 * install is the only one there is. */
		return venture_context_get_default_organization_id(self->context);
	}

	if (0 == g_strcmp0(selected, "all"))
		return 0;

	return g_ascii_strtoll(selected, NULL, 10);
}

/*
 * Collects @root and every entity beneath it, to any depth.
 *
 * Returns: (transfer full): the identifiers, @root first
 */
static GArray *
venture_web_organization_tree(
	VentureWebServer	*self,
	gint64			 root
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) all = NULL;
	GArray *tree;
	gboolean grew;

	tree = g_array_new(FALSE, FALSE, sizeof(gint64));

	if (0 == root)
		return tree;

	g_array_append_val(tree, root);

	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	venture_query_set_limit(query, 0);
	all = venture_database_find(venture_context_get_database(self->context),
	                            query, NULL);

	if (NULL == all)
		return tree;

	/*
	 * Repeated passes rather than recursion: the table is small, the
	 * depth is unknown, and a cycle introduced by a bad edit would make a
	 * recursive walk loop forever instead of simply stopping.
	 */
	do
	{
		guint i;

		grew = FALSE;

		for (i = 0; i < all->len; i++)
		{
			VentureEntity *entity;
			gint64 id;
			gint64 parent;
			guint j;
			gboolean known;

			entity = g_ptr_array_index(all, i);
			id = venture_entity_get_id(entity);
			g_object_get(entity, "parent-id", &parent, NULL);

			if (0 == parent)
				continue;

			known = FALSE;

			for (j = 0; j < tree->len; j++)
			{
				if (g_array_index(tree, gint64, j) == id)
					known = TRUE;
			}

			if (known)
				continue;

			for (j = 0; j < tree->len; j++)
			{
				if (g_array_index(tree, gint64, j) == parent)
				{
					g_array_append_val(tree, id);
					grew = TRUE;
					break;
				}
			}
		}
	}
	while (grew);

	return tree;
}

/*
 * Scopes @query to whichever entity the browser has selected, including any
 * entities beneath it.
 */
static void
venture_web_scope_to_active_organization(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureQuery		*query
){
	g_autoptr(GArray) tree = NULL;
	gint64 active;

	/*
	 * An entity is not filed against an entity. Scoping the list of them
	 * by the selected one hides every entity except whichever happened to
	 * carry a matching identifier -- including, on a fresh install, all
	 * of them.
	 */
	if (VENTURE_TYPE_ORGANIZATION == venture_query_get_entity_type(query))
		return;

	active = venture_web_active_organization(self, request);

	if (0 == active)
		return;

	tree = venture_web_organization_tree(self, active);

	venture_query_set_organization_tree(query, (const gint64 *)tree->data,
	                                    tree->len);
}

static HtmxResponse *
venture_web_ui_switch_entity(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autofree gchar *cookie = NULL;
	HtmxResponse *response;
	HtmxResponse *redirect;
	const gchar *id;
	const gchar *back;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	id = g_hash_table_lookup(params, "id");

	/* Same lifetime and flags as the session cookie: this is a view
	 * preference, so it is not signed, but it should not outlive the
	 * browser session in a way the user would not expect. */
	cookie = g_strdup_printf("%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d",
	                         VENTURE_WEB_ENTITY_COOKIE,
	                         (NULL != id) ? id : "all", 60 * 60 * 24 * 365);

	back = htmx_request_get_query_param(request, "back");

	response = htmx_response_new();
	htmx_response_set_status(response, 302);
	htmx_response_add_header(response, "Location",
	                         venture_string_is_empty(back) ? "/" : back);
	htmx_response_add_header(response, "Set-Cookie", cookie);

	return response;
}

/*
 * Renders the entity picker for the sidebar.
 *
 * Children are indented under their parent, because "which of these is the
 * LLC inside the side business" is the question the list has to answer at a
 * glance.
 */
static void
venture_web_append_entity_picker(
	VentureWebServer	*self,
	HtmxRequest		*request,
	GString			*html,
	const gchar		*current_path
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) organizations = NULL;
	g_autofree gchar *back = NULL;
	gint64 active;
	guint depth;
	guint i;

	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);

	organizations = venture_database_find(
		venture_context_get_database(self->context), query, NULL);

	if (NULL == organizations)
		return;

	active = venture_web_active_organization(self, request);
	back = g_uri_escape_string(
		venture_string_is_empty(current_path) ? "/" : current_path, NULL, TRUE);

	g_string_append(html, "<div class=\"entity-picker\">"
	                      "<div class=\"entity-picker-label\">Entity</div>"
	                      "<details class=\"entity-menu\"><summary>");

	if (0 == active)
	{
		g_string_append(html, "All entities");
	}
	else
	{
		gboolean named;

		named = FALSE;

		for (i = 0; i < organizations->len; i++)
		{
			VentureEntity *entity;

			entity = g_ptr_array_index(organizations, i);

			if (venture_entity_get_id(entity) == active)
			{
				g_autofree gchar *name = NULL;

				g_object_get(entity, "name", &name, NULL);
				venture_html_escape_append(html, name);
				named = TRUE;
			}
		}

		if (!named)
			g_string_append(html, "All entities");
	}

	g_string_append(html, "</summary><div class=\"entity-menu-list\">");

	g_string_append_printf(html,
		"<a class=\"entity-option%s\" href=\"/entity/all?back=%s\">"
		"All entities</a>",
		(0 == active) ? " active" : "", back);

	/*
	 * Roots first, then their children under them. Two passes rather than
	 * a sort, so an entity whose parent was deleted still appears instead
	 * of vanishing from the only list that can fix it.
	 */
	for (depth = 0; depth < 2; depth++)
	{
		for (i = 0; i < organizations->len; i++)
		{
			g_autofree gchar *name = NULL;
			VentureEntity *entity;
			gint64 parent;
			gboolean is_child;

			entity = g_ptr_array_index(organizations, i);
			g_object_get(entity, "name", &name, "parent-id", &parent, NULL);

			is_child = (0 != parent) &&
				venture_web_organization_exists(self, organizations, parent);

			if ((0 == depth) == is_child)
				continue;

			g_string_append_printf(html,
				"<a class=\"entity-option%s%s\" "
				"href=\"/entity/%" G_GINT64_FORMAT "?back=%s\">",
				(venture_entity_get_id(entity) == active) ? " active" : "",
				is_child ? " entity-child" : "",
				venture_entity_get_id(entity), back);
			venture_html_escape_append(html, name);
			g_string_append(html, "</a>");
		}
	}

	g_string_append(html, "<a class=\"entity-option entity-add\" "
	                      "href=\"/entities\">Manage entities</a>");
	g_string_append(html, "</div></details></div>");
}

/* --- Entity management --------------------------------------------------- */

/*
 * Counts every record filed against @organization_id, across every type.
 *
 * Deleting an entity is only safe once you know what is attached to it, and
 * the number is the difference between a harmless tidy-up and losing a
 * quarter of your books.
 */
static gint64
venture_web_count_entity_records(
	VentureWebServer	*self,
	gint64			 organization_id
){
	g_autofree GType *types = NULL;
	gint64 total;
	guint n_types;
	guint i;

	total = 0;
	types = venture_entity_registry_list_types(
		venture_context_get_entity_registry(self->context), &n_types);

	for (i = 0; i < n_types; i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		gint64 count;

		/* An organisation is not filed against itself, and audit
		 * entries are the record of what happened rather than data the
		 * entity owns. */
		if ((VENTURE_TYPE_ORGANIZATION == types[i]) ||
		    (VENTURE_TYPE_AUDIT_ENTRY == types[i]))
			continue;

		query = venture_query_new(types[i]);
		venture_query_set_organization(query, organization_id);

		count = venture_database_count(
			venture_context_get_database(self->context), query, NULL);

		if (count > 0)
			total += count;
	}

	return total;
}

/*
 * Moves every record of @organization_id to @destination.
 *
 * Returns: %TRUE if everything moved
 */
static gboolean
venture_web_move_entity_records(
	VentureWebServer	 *self,
	gint64			  organization_id,
	gint64			  destination,
	const VentureActor	 *actor,
	GError			**error
){
	g_autofree GType *types = NULL;
	guint n_types;
	guint i;

	types = venture_entity_registry_list_types(
		venture_context_get_entity_registry(self->context), &n_types);

	for (i = 0; i < n_types; i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) records = NULL;
		guint j;

		if ((VENTURE_TYPE_ORGANIZATION == types[i]) ||
		    (VENTURE_TYPE_AUDIT_ENTRY == types[i]))
			continue;

		query = venture_query_new(types[i]);
		venture_query_set_organization(query, organization_id);
		venture_query_set_limit(query, 0);

		records = venture_database_find(
			venture_context_get_database(self->context), query, error);

		if (NULL == records)
			return FALSE;

		for (j = 0; j < records->len; j++)
		{
			VentureEntity *record;

			record = g_ptr_array_index(records, j);
			venture_entity_set_organization_id(record, destination);

			/* Saved one at a time and audited, so the move shows up in
			 * the log as what it was rather than as records silently
			 * changing hands. */
			if (!venture_database_save(
				venture_context_get_database(self->context), record, actor,
				error))
				return FALSE;
		}
	}

	return TRUE;
}

/*
 * Whether any entity names @organization_id as its parent.
 */
static gboolean
venture_web_entity_has_children(
	VentureWebServer	*self,
	gint64			 organization_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;

	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);

	if (!venture_query_add_filter_int(query, "parent-id", VENTURE_FILTER_OP_EQ,
	                                  organization_id, &error))
		return FALSE;

	return (venture_database_count(venture_context_get_database(self->context),
	                               query, NULL) > 0);
}

static HtmxResponse *
venture_web_ui_entities(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) organizations = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	const gchar *notice;
	guint i;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN,
	                          &error))
		return venture_web_error_response(error);

	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);

	organizations = venture_database_find(
		venture_context_get_database(self->context), query, &error);

	if (NULL == organizations)
		return venture_web_error_response(error);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Entities</h1>"
	                       "<p class=\"muted\">The businesses and personal "
	                       "spheres you keep books for. Every record belongs "
	                       "to exactly one.</p></div></div>");

	notice = g_hash_table_lookup(params, "notice");

	if (NULL != notice)
	{
		gboolean ok;

		ok = (NULL != g_strrstr(notice, "ok"));
		g_string_append_printf(content, "<div class=\"banner %s\">",
		                       ok ? "banner-ok" : "banner-warn");

		if (0 == g_strcmp0(notice, "created-ok"))
			g_string_append(content, "Entity created.");
		else if (0 == g_strcmp0(notice, "updated-ok"))
			g_string_append(content, "Entity updated.");
		else if (0 == g_strcmp0(notice, "deleted-ok"))
			g_string_append(content, "Entity deleted.");
		else if (0 == g_strcmp0(notice, "moved-ok"))
			g_string_append(content,
				"Entity deleted, and its records moved.");
		else if (0 == g_strcmp0(notice, "last"))
			g_string_append(content,
				"That is the only entity. Everything has to belong to "
				"something.");
		else if (0 == g_strcmp0(notice, "default"))
			g_string_append(content,
				"That is the default entity. Make another one the default "
				"first.");
		else if (0 == g_strcmp0(notice, "children"))
			g_string_append(content,
				"That entity holds others. Move or delete those first, so "
				"nothing is left pointing at something that is gone.");
		else if (0 == g_strcmp0(notice, "records"))
			g_string_append(content,
				"That entity still holds records. Choose where they should "
				"go, or they would be left belonging to nothing.");
		else
			g_string_append(content, "That did not work.");

		g_string_append(content, "</div>");
	}

	g_string_append(content, "<div class=\"card\"><div class=\"table-wrap\">"
	                         "<table class=\"data\"><thead><tr>"
	                         "<th>Name</th><th>Legal form</th><th>Part of</th>"
	                         "<th>Records</th><th>Default</th>"
	                         "<th class=\"row-actions\"></th>"
	                         "</tr></thead><tbody>");

	for (i = 0; i < organizations->len; i++)
	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *parent_name = NULL;
		VentureEntity *entity;
		VentureOrganizationKind kind;
		gint64 id;
		gint64 parent;
		gint64 records;
		gboolean is_default;

		entity = g_ptr_array_index(organizations, i);
		id = venture_entity_get_id(entity);

		g_object_get(entity, "name", &name, "kind", &kind, "parent-id", &parent,
		             "is-default", &is_default, NULL);

		records = venture_web_count_entity_records(self, id);

		g_string_append(content, "<tr><td>");
		venture_html_escape_append(content, name);
		g_string_append(content, "</td><td>");
		venture_html_escape_append(content,
			venture_enum_to_nick(VENTURE_TYPE_ORGANIZATION_KIND, (gint)kind));
		g_string_append(content, "</td><td class=\"muted\">");

		if (0 != parent)
		{
			guint j;

			for (j = 0; j < organizations->len; j++)
			{
				VentureEntity *candidate;

				candidate = g_ptr_array_index(organizations, j);

				if (venture_entity_get_id(candidate) == parent)
				{
					g_object_get(candidate, "name", &parent_name, NULL);
					venture_html_escape_append(content, parent_name);
				}
			}
		}

		g_string_append_printf(content,
			"</td><td class=\"num\">%" G_GINT64_FORMAT "</td><td>%s</td>",
			records, is_default ? "<span class=\"badge\">default</span>" : "");

		g_string_append_printf(content,
			"<td class=\"row-actions\">"
			"<a class=\"btn btn-sm\" href=\"/e/organization/%" G_GINT64_FORMAT
			"/edit\">Edit</a> ", id);

		if (!is_default)
		{
			g_string_append_printf(content,
				"<form method=\"post\" action=\"/entities/%" G_GINT64_FORMAT
				"/default\" class=\"inline-form\">"
				"<button class=\"btn btn-sm\" type=\"submit\">Make default"
				"</button></form> ", id);
		}

		/*
		 * Deleting names its own consequence. When the entity still
		 * holds records the form asks where they should go rather than
		 * refusing outright -- "move my personal things into the
		 * business" is the actual reason somebody deletes an entity.
		 */
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/entities/%" G_GINT64_FORMAT
			"/delete\" class=\"inline-form\">", id);

		if (records > 0)
		{
			guint j;

			g_string_append(content, "<select name=\"move_to\">");
			g_string_append(content, "<option value=\"\">move records to…"
			                         "</option>");

			for (j = 0; j < organizations->len; j++)
			{
				g_autofree gchar *other = NULL;
				VentureEntity *candidate;

				candidate = g_ptr_array_index(organizations, j);

				if (venture_entity_get_id(candidate) == id)
					continue;

				g_object_get(candidate, "name", &other, NULL);

				g_string_append_printf(content,
					"<option value=\"%" G_GINT64_FORMAT "\">",
					venture_entity_get_id(candidate));
				venture_html_escape_append(content, other);
				g_string_append(content, "</option>");
			}

			g_string_append(content, "</select>");
		}

		g_string_append(content,
			"<button class=\"btn btn-sm btn-danger\" type=\"submit\">Delete"
			"</button></form></td></tr>");
	}

	g_string_append(content, "</tbody></table></div></div>");

	/* Adding one. The same fields the generated form offers, kept short
	 * here because the rest can be filled in afterwards. */
	g_string_append(content,
		"<div class=\"card\"><div class=\"card-body\">"
		"<h2>Add an entity</h2>"
		"<form method=\"post\" action=\"/entities\">"
		"<div class=\"form-grid\">"
		"<div class=\"field\"><label>Name<input type=\"text\" "
		"name=\"name\" required></label></div>"
		"<div class=\"field\"><label>Legal form<select name=\"kind\">");

	{
		g_auto(GStrv) nicks = NULL;
		gsize j;

		nicks = venture_enum_list_nicks(VENTURE_TYPE_ORGANIZATION_KIND);

		for (j = 0; (NULL != nicks) && (NULL != nicks[j]); j++)
		{
			g_string_append_printf(content, "<option value=\"%s\">", nicks[j]);
			venture_html_escape_append(content, nicks[j]);
			g_string_append(content, "</option>");
		}
	}

	g_string_append(content, "</select></label>"
	                         "<div class=\"field-help\">Drives which tax "
	                         "treatment applies.</div></div>"
	                         "<div class=\"field\"><label>Part of"
	                         "<select name=\"parent_id\">"
	                         "<option value=\"\">— nothing —</option>");

	for (i = 0; i < organizations->len; i++)
	{
		g_autofree gchar *name = NULL;
		VentureEntity *entity;

		entity = g_ptr_array_index(organizations, i);
		g_object_get(entity, "name", &name, NULL);

		g_string_append_printf(content, "<option value=\"%" G_GINT64_FORMAT "\">",
		                       venture_entity_get_id(entity));
		venture_html_escape_append(content, name);
		g_string_append(content, "</option>");
	}

	g_string_append(content,
		"</select></label>"
		"<div class=\"field-help\">A parent entity this one sits inside. "
		"Viewing the parent rolls up everything beneath it.</div></div>"
		"</div>"
		"<div class=\"form-actions\">"
		"<button class=\"btn btn-primary\" type=\"submit\">Add entity</button>"
		"</div></form></div></div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/entities", "Entities", content->str),
		200);
}

static HtmxResponse *
venture_web_ui_entities_create(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureOrganization) organization = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	const gchar *name;
	const gchar *kind_nick;
	const gchar *parent;
	gint kind_value;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN,
	                          &error))
		return venture_web_error_response(error);

	name = htmx_request_get_form_value(request, "name");

	if (venture_string_is_empty(name))
		return venture_web_redirect_to("/entities?notice=failed");

	organization = venture_organization_new();
	g_object_set(organization, "name", name, "active", TRUE, NULL);

	kind_nick = htmx_request_get_form_value(request, "kind");

	if (venture_enum_from_nick(VENTURE_TYPE_ORGANIZATION_KIND, kind_nick,
	                           &kind_value))
	{
		g_object_set(organization, "kind",
		             (VentureOrganizationKind)kind_value, NULL);
	}

	parent = htmx_request_get_form_value(request, "parent_id");

	if (!venture_string_is_empty(parent))
	{
		g_object_set(organization, "parent-id",
		             g_ascii_strtoll(parent, NULL, 10), NULL);
	}

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(organization), &actor, &error))
		return venture_web_redirect_to("/entities?notice=failed");

	return venture_web_redirect_to("/entities?notice=created-ok");
}

static HtmxResponse *
venture_web_ui_entities_default(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) organizations = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	gint64 id;
	guint i;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN,
	                          &error))
		return venture_web_error_response(error);

	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	venture_auth_to_actor(principal, &actor);

	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	venture_query_set_limit(query, 0);
	organizations = venture_database_find(
		venture_context_get_database(self->context), query, &error);

	if (NULL == organizations)
		return venture_web_error_response(error);

	/* Exactly one entity is the default, so setting one clears the rest
	 * rather than leaving two claiming it. */
	for (i = 0; i < organizations->len; i++)
	{
		VentureEntity *entity;
		gboolean was_default;
		gboolean becomes_default;

		entity = g_ptr_array_index(organizations, i);
		becomes_default = (venture_entity_get_id(entity) == id);

		g_object_get(entity, "is-default", &was_default, NULL);

		if (was_default == becomes_default)
			continue;

		g_object_set(entity, "is-default", becomes_default, NULL);

		if (!venture_database_save(venture_context_get_database(self->context),
		                           entity, &actor, &error))
			return venture_web_redirect_to("/entities?notice=failed");
	}

	return venture_web_redirect_to("/entities?notice=updated-ok");
}

static HtmxResponse *
venture_web_ui_entities_delete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *response;
	HtmxResponse *redirect;
	const gchar *move_to;
	gint64 id;
	gint64 destination;
	gint64 records;
	gboolean is_default;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN,
	                          &error))
		return venture_web_error_response(error);

	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);

	organization = venture_database_get(
		venture_context_get_database(self->context), VENTURE_TYPE_ORGANIZATION,
		id, &error);

	if (NULL == organization)
		return venture_web_redirect_to("/entities?notice=failed");

	/* The last one: everything has to belong to something. */
	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);

	if (venture_database_count(venture_context_get_database(self->context),
	                           query, NULL) < 2)
		return venture_web_redirect_to("/entities?notice=last");

	g_object_get(organization, "is-default", &is_default, NULL);

	/* The default is where unattributed records land, so it must always
	 * exist. Naming another one first is a deliberate step. */
	if (is_default)
		return venture_web_redirect_to("/entities?notice=default");

	if (venture_web_entity_has_children(self, id))
		return venture_web_redirect_to("/entities?notice=children");

	move_to = htmx_request_get_form_value(request, "move_to");
	destination = venture_string_is_empty(move_to)
		? 0 : g_ascii_strtoll(move_to, NULL, 10);
	records = venture_web_count_entity_records(self, id);

	if ((records > 0) && (0 == destination))
		return venture_web_redirect_to("/entities?notice=records");

	/*
	 * One transaction: an entity that is half deleted, with some of its
	 * records moved and some not, is worse than either outcome.
	 */
	if (!venture_database_begin(venture_context_get_database(self->context),
	                            &error))
		return venture_web_redirect_to("/entities?notice=failed");

	venture_auth_to_actor(principal, &actor);

	if (records > 0)
	{
		if (!venture_web_move_entity_records(self, id, destination, &actor,
		                                     &error))
		{
			venture_database_rollback(
				venture_context_get_database(self->context));
			return venture_web_redirect_to("/entities?notice=failed");
		}
	}

	/* Soft, like every other delete here: the entity is hidden and kept,
	 * because last year's figures still refer to it. */
	if (!venture_database_delete(venture_context_get_database(self->context),
	                             organization, &actor, &error))
	{
		venture_database_rollback(venture_context_get_database(self->context));
		return venture_web_redirect_to("/entities?notice=failed");
	}

	if (!venture_database_commit(venture_context_get_database(self->context),
	                             &error))
		return venture_web_redirect_to("/entities?notice=failed");

	/*
	 * If the browser was looking at the entity that just went away, it
	 * would otherwise keep asking for a scope that no longer exists and
	 * see nothing at all, with no clue why.
	 */
	response = venture_web_redirect_to((records > 0)
		? "/entities?notice=moved-ok" : "/entities?notice=deleted-ok");

	if (venture_web_active_organization(self, request) == id)
	{
		g_autofree gchar *cookie = NULL;

		cookie = g_strdup_printf(
			"%s=all; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d",
			VENTURE_WEB_ENTITY_COOKIE, 60 * 60 * 24 * 365);
		htmx_response_add_header(response, "Set-Cookie", cookie);
	}

	return response;
}

/* --- Accounts and users -------------------------------------------------- */

/*
 * Loads the user a principal refers to.
 *
 * Returns: (transfer full) (nullable): the account, or %NULL
 */
static VentureUser *
venture_web_lookup_user(
	VentureWebServer	*self,
	gint64			 user_id
){
	VentureEntity *entity;

	if (0 == user_id)
		return NULL;

	entity = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_USER, user_id, NULL);

	return (NULL != entity) ? VENTURE_USER(entity) : NULL;
}

/*
 * Renders a page with a single message, used after a form post so that a
 * refresh does not repeat the action.
 */
static HtmxResponse *
venture_web_redirect_to(const gchar *path)
{
	HtmxResponse *response;

	response = htmx_response_new();
	htmx_response_set_status(response, 302);
	htmx_response_add_header(response, "Location", path);

	return response;
}

static HtmxResponse *
venture_web_ui_account(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GString) content = NULL;
	HtmxResponse *redirect;
	const gchar *notice;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);
	user = venture_web_lookup_user(self, principal->user_id);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Your account</h1></div></div>");

	notice = g_hash_table_lookup(params, "notice");

	if (NULL != notice)
	{
		g_string_append_printf(content, "<div class=\"banner %s\">",
			(0 == g_strcmp0(notice, "changed")) ? "banner-ok" : "banner-warn");
		venture_html_escape_append(content,
			(0 == g_strcmp0(notice, "changed"))
				? "Password changed."
				: "That did not work. Check the current password and try "
				  "again.");
		g_string_append(content, "</div>");
	}

	g_string_append(content, "<div class=\"card\"><div class=\"card-body\">");

	if (NULL == user)
	{
		/*
		 * An API token with no user behind it, or authentication turned
		 * off entirely. There is no account to change.
		 */
		g_string_append(content,
			"<p class=\"muted\">This session is not tied to a user account, "
			"so there is no password to change.</p>");
	}
	else
	{
		g_autofree gchar *username = NULL;
		g_autofree gchar *display_name = NULL;
		VentureUserRole role;

		g_object_get(user, "username", &username, "display-name",
		             &display_name, "role", &role, NULL);

		g_string_append(content, "<dl class=\"detail\">");
		g_string_append(content, "<dt>Username</dt><dd>");
		venture_html_escape_append(content, username);
		g_string_append(content, "</dd><dt>Name</dt><dd>");
		venture_html_escape_append(content,
			venture_string_is_empty(display_name) ? "—" : display_name);
		g_string_append(content, "</dd><dt>Role</dt><dd>");
		venture_html_escape_append(content,
			venture_enum_to_nick(VENTURE_TYPE_USER_ROLE, (gint)role));
		g_string_append(content, "</dd></dl>");

		g_string_append_printf(content,
			"<h2>Change password</h2>"
			"<form method=\"post\" action=\"/account/password\">"
			"<label>Current password"
			"<input type=\"password\" name=\"current_password\" required "
			"autocomplete=\"current-password\"></label>"
			"<label>New password"
			"<input type=\"password\" name=\"new_password\" required "
			"minlength=\"%u\" autocomplete=\"new-password\"></label>"
			"<label>New password again"
			"<input type=\"password\" name=\"confirm_password\" required "
			"minlength=\"%u\" autocomplete=\"new-password\"></label>"
			"<button class=\"btn btn-primary\" type=\"submit\">"
			"Change password</button>"
			"</form>"
			"<p class=\"muted small\">At least %u characters.</p>",
			venture_auth_get_password_min_length(self->auth),
			venture_auth_get_password_min_length(self->auth),
			venture_auth_get_password_min_length(self->auth));
	}

	g_string_append(content, "</div></div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/account", "Your account", content->str), 200);
}

static HtmxResponse *
venture_web_ui_account_password(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	const gchar *current;
	const gchar *replacement;
	const gchar *confirmation;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);
	user = venture_web_lookup_user(self, principal->user_id);

	if (NULL == user)
		return venture_web_redirect_to("/account?notice=failed");

	current = htmx_request_get_form_value(request, "current_password");
	replacement = htmx_request_get_form_value(request, "new_password");
	confirmation = htmx_request_get_form_value(request, "confirm_password");

	/*
	 * The current password is required even though the session already
	 * proves who this is: it is what stops someone who walked up to an
	 * unlocked browser from locking the owner out of their own books.
	 */
	if (!venture_user_check_password(user, current))
		return venture_web_redirect_to("/account?notice=failed");

	if (0 != g_strcmp0(replacement, confirmation))
		return venture_web_redirect_to("/account?notice=failed");

	if (!venture_auth_set_password(self->auth, user, replacement, &error))
		return venture_web_redirect_to("/account?notice=failed");

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(user), &actor, &error))
		return venture_web_redirect_to("/account?notice=failed");

	return venture_web_redirect_to("/account?notice=changed");
}

/*
 * Managing other people's accounts is owner-only. Admin deliberately does
 * not carry it: the difference between the two roles is precisely who may
 * grant access, and an admin who can promote themselves is an owner.
 */
static HtmxResponse *
venture_web_ui_users(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	const gchar *notice;
	guint i;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER,
	                          &error))
		return venture_web_error_response(error);

	query = venture_query_new(VENTURE_TYPE_USER);
	venture_query_set_limit(query, 0);
	users = venture_database_find(venture_context_get_database(self->context),
	                              query, &error);

	if (NULL == users)
		return venture_web_error_response(error);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Users</h1>"
	                       "<p class=\"muted\">Who can sign in, and what they "
	                       "may do.</p></div></div>");

	notice = g_hash_table_lookup(params, "notice");

	if (NULL != notice)
	{
		gboolean ok;

		ok = (NULL != g_strrstr(notice, "ok"));
		g_string_append_printf(content, "<div class=\"banner %s\">",
		                       ok ? "banner-ok" : "banner-warn");

		if (0 == g_strcmp0(notice, "created-ok"))
			g_string_append(content, "User created.");
		else if (0 == g_strcmp0(notice, "updated-ok"))
			g_string_append(content, "User updated.");
		else if (0 == g_strcmp0(notice, "password-ok"))
			g_string_append(content, "Password reset.");
		else if (0 == g_strcmp0(notice, "duplicate"))
			g_string_append(content, "That username is already taken.");
		else if (0 == g_strcmp0(notice, "short"))
			g_string_append_printf(content,
				"The password is too short: at least %u characters.",
				venture_auth_get_password_min_length(self->auth));
		else if (0 == g_strcmp0(notice, "last-owner"))
			g_string_append(content,
				"That is the only active owner. Promote somebody else first, "
				"or you will lock yourself out.");
		else
			g_string_append(content, "That did not work.");

		g_string_append(content, "</div>");
	}

	g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
	                         "<table class=\"table\"><thead><tr>"
	                         "<th>Username</th><th>Name</th><th>Role</th>"
	                         "<th>Status</th><th>Last login</th>"
	                         "<th>Reset password</th></tr></thead><tbody>");

	for (i = 0; i < users->len; i++)
	{
		g_autofree gchar *username = NULL;
		g_autofree gchar *display_name = NULL;
		g_autoptr(GDateTime) last_login = NULL;
		VentureEntity *entity;
		VentureUserRole role;
		gboolean active;
		gint64 id;

		entity = g_ptr_array_index(users, i);
		id = venture_entity_get_id(entity);

		g_object_get(entity, "username", &username, "display-name",
		             &display_name, "role", &role, "active", &active,
		             "last-login-at", &last_login, NULL);

		g_string_append(content, "<tr><td>");
		venture_html_escape_append(content, username);

		if (id == principal->user_id)
			g_string_append(content, " <span class=\"badge\">you</span>");

		g_string_append(content, "</td><td>");
		venture_html_escape_append(content,
			venture_string_is_empty(display_name) ? "—" : display_name);

		/* Role and active state change together, in one form, so a
		 * single click cannot leave half a change applied. */
		g_string_append_printf(content,
			"</td><td><form method=\"post\" action=\"/users/%" G_GINT64_FORMAT
			"/update\" class=\"inline-form\">"
			"<select name=\"role\">", id);

		{
			g_auto(GStrv) nicks = NULL;
			gsize j;

			nicks = venture_enum_list_nicks(VENTURE_TYPE_USER_ROLE);

			for (j = 0; (NULL != nicks) && (NULL != nicks[j]); j++)
			{
				const gchar *current_nick;

				current_nick = venture_enum_to_nick(VENTURE_TYPE_USER_ROLE,
				                                    (gint)role);

				g_string_append_printf(content, "<option value=\"%s\"%s>",
					nicks[j],
					(0 == g_strcmp0(nicks[j], current_nick)) ? " selected" : "");
				venture_html_escape_append(content, nicks[j]);
				g_string_append(content, "</option>");
			}
		}

		g_string_append_printf(content,
			"</select></td><td>"
			"<label class=\"checkbox\"><input type=\"checkbox\" name=\"active\""
			" value=\"true\"%s> active</label>"
			"</td><td class=\"muted small\">",
			active ? " checked" : "");

		if (NULL != last_login)
		{
			g_autofree gchar *when = NULL;

			when = g_date_time_format(last_login, "%Y-%m-%d %H:%M");
			venture_html_escape_append(content, when);
		}
		else
		{
			g_string_append(content, "never");
		}

		g_string_append(content,
			"</td><td><button class=\"btn btn-sm\" type=\"submit\">Save"
			"</button></form>");

		g_string_append_printf(content,
			"<form method=\"post\" action=\"/users/%" G_GINT64_FORMAT
			"/password\" class=\"inline-form\">"
			"<input type=\"password\" name=\"new_password\" required "
			"minlength=\"%u\" placeholder=\"new password\" "
			"autocomplete=\"new-password\">"
			"<button class=\"btn btn-sm\" type=\"submit\">Reset</button>"
			"</form></td></tr>",
			id, venture_auth_get_password_min_length(self->auth));
	}

	g_string_append(content, "</tbody></table></div></div>");

	g_string_append_printf(content,
		"<div class=\"card\"><div class=\"card-body\">"
		"<h2>Add a user</h2>"
		"<form method=\"post\" action=\"/users\">"
		"<label>Username<input type=\"text\" name=\"username\" required "
		"autocomplete=\"off\"></label>"
		"<label>Display name<input type=\"text\" name=\"display_name\">"
		"</label>"
		"<label>Email<input type=\"email\" name=\"email\"></label>"
		"<label>Password<input type=\"password\" name=\"password\" required "
		"minlength=\"%u\" autocomplete=\"new-password\"></label>"
		"<label>Role<select name=\"role\">",
		venture_auth_get_password_min_length(self->auth));

	{
		g_auto(GStrv) nicks = NULL;
		gsize j;

		nicks = venture_enum_list_nicks(VENTURE_TYPE_USER_ROLE);

		for (j = 0; (NULL != nicks) && (NULL != nicks[j]); j++)
		{
			g_string_append_printf(content, "<option value=\"%s\"%s>",
				nicks[j],
				(0 == g_strcmp0(nicks[j], "editor")) ? " selected" : "");
			venture_html_escape_append(content, nicks[j]);
			g_string_append(content, "</option>");
		}
	}

	g_string_append(content,
		"</select></label>"
		"<button class=\"btn btn-primary\" type=\"submit\">Add user</button>"
		"</form></div></div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/users", "Users", content->str), 200);
}

/*
 * Counts the active owners other than @excluding.
 *
 * Used to refuse the change that locks everybody out: demoting or
 * deactivating the last owner leaves an install nobody can administer, and
 * the only fix is editing the database by hand.
 */
static guint
venture_web_count_other_owners(
	VentureWebServer	*self,
	gint64			 excluding
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) owners = NULL;
	guint count;
	guint i;

	query = venture_query_new(VENTURE_TYPE_USER);
	venture_query_set_limit(query, 0);

	owners = venture_database_find(venture_context_get_database(self->context),
	                               query, NULL);

	if (NULL == owners)
		return 0;

	count = 0;

	for (i = 0; i < owners->len; i++)
	{
		VentureEntity *entity;
		VentureUserRole role;
		gboolean active;

		entity = g_ptr_array_index(owners, i);

		if (venture_entity_get_id(entity) == excluding)
			continue;

		g_object_get(entity, "role", &role, "active", &active, NULL);

		if (active && (VENTURE_USER_ROLE_OWNER == role))
			count++;
	}

	return count;
}

static HtmxResponse *
venture_web_ui_users_create(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	const gchar *username;
	const gchar *password;
	const gchar *role_nick;
	gint role_value;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER,
	                          &error))
		return venture_web_error_response(error);

	username = htmx_request_get_form_value(request, "username");
	password = htmx_request_get_form_value(request, "password");
	role_nick = htmx_request_get_form_value(request, "role");

	if (venture_string_is_empty(username))
		return venture_web_redirect_to("/users?notice=failed");

	user = venture_user_new();
	g_object_set(user, "username", username, "active", TRUE, NULL);

	{
		const gchar *display_name;
		const gchar *email;

		display_name = htmx_request_get_form_value(request, "display_name");
		email = htmx_request_get_form_value(request, "email");

		if (!venture_string_is_empty(display_name))
			g_object_set(user, "display-name", display_name, NULL);

		if (!venture_string_is_empty(email))
			g_object_set(user, "email", email, NULL);
	}

	if (venture_enum_from_nick(VENTURE_TYPE_USER_ROLE, role_nick, &role_value))
		g_object_set(user, "role", (VentureUserRole)role_value, NULL);

	if (!venture_auth_set_password(self->auth, user, password, &error))
		return venture_web_redirect_to("/users?notice=short");

	venture_entity_set_organization_id(VENTURE_ENTITY(user),
		venture_context_get_default_organization_id(self->context));

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(user), &actor, &error))
	{
		/* The username column is unique, so the commonest failure here
		 * is a name already taken. */
		return venture_web_redirect_to("/users?notice=duplicate");
	}

	return venture_web_redirect_to("/users?notice=created-ok");
}

static HtmxResponse *
venture_web_ui_users_update(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	const gchar *role_nick;
	gint role_value;
	gint64 id;
	gboolean active;
	gboolean was_owner;
	gboolean stays_owner;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER,
	                          &error))
		return venture_web_error_response(error);

	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	user = venture_web_lookup_user(self, id);

	if (NULL == user)
		return venture_web_redirect_to("/users?notice=failed");

	role_nick = htmx_request_get_form_value(request, "role");
	active = (NULL != htmx_request_get_form_value(request, "active"));

	{
		VentureUserRole existing;

		g_object_get(user, "role", &existing, NULL);
		was_owner = (VENTURE_USER_ROLE_OWNER == existing);
	}

	stays_owner = venture_enum_from_nick(VENTURE_TYPE_USER_ROLE, role_nick,
	                                     &role_value)
		? (VENTURE_USER_ROLE_OWNER == (VentureUserRole)role_value)
		: was_owner;

	/* Refuse to remove the last way in. */
	if (was_owner && (!stays_owner || !active) &&
	    (0 == venture_web_count_other_owners(self, id)))
		return venture_web_redirect_to("/users?notice=last-owner");

	if (venture_enum_from_nick(VENTURE_TYPE_USER_ROLE, role_nick, &role_value))
		g_object_set(user, "role", (VentureUserRole)role_value, NULL);

	g_object_set(user, "active", active, NULL);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(user), &actor, &error))
		return venture_web_redirect_to("/users?notice=failed");

	return venture_web_redirect_to("/users?notice=updated-ok");
}

static HtmxResponse *
venture_web_ui_users_password(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	gint64 id;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER,
	                          &error))
		return venture_web_error_response(error);

	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	user = venture_web_lookup_user(self, id);

	if (NULL == user)
		return venture_web_redirect_to("/users?notice=failed");

	/*
	 * An owner resetting somebody else's password is not asked for the
	 * old one -- the point of a reset is that it is unknown. Changing
	 * your own goes through /account, which does ask.
	 */
	if (!venture_auth_set_password(self->auth, user,
		htmx_request_get_form_value(request, "new_password"), &error))
		return venture_web_redirect_to("/users?notice=short");

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(user), &actor, &error))
		return venture_web_redirect_to("/users?notice=failed");

	return venture_web_redirect_to("/users?notice=password-ok");
}

/* --- Settings ------------------------------------------------------------ */

/*
 * Renders one group of settings as a table.
 */
static void
venture_web_settings_section(
	GString		*content,
	JsonArray	*settings,
	const gchar	*section,
	const gchar	*title
){
	guint i;
	gboolean opened;

	opened = FALSE;

	for (i = 0; i < json_array_get_length(settings); i++)
	{
		JsonObject *setting;
		const gchar *value_kind;

		setting = json_array_get_object_element(settings, i);

		if (0 != g_strcmp0(json_object_get_string_member(setting, "section"),
		                   section))
			continue;

		if (!opened)
		{
			g_string_append(content,
				"<div class=\"card\"><div class=\"card-body\"><h2>");
			venture_html_escape_append(content, title);
			g_string_append(content,
				"</h2><table class=\"table\"><thead><tr>"
				"<th>Setting</th><th>Value</th>"
				"<th>Environment variable</th></tr></thead><tbody>");
			opened = TRUE;
		}

		g_string_append(content, "<tr><td><code>");
		venture_html_escape_append(content,
			json_object_get_string_member(setting, "key"));
		g_string_append(content, "</code>");

		{
			const gchar *help;

			help = json_object_get_string_member(setting, "help");

			if (!venture_string_is_empty(help))
			{
				g_string_append(content, "<div class=\"muted small\">");
				venture_html_escape_append(content, help);
				g_string_append(content, "</div>");
			}
		}

		g_string_append(content, "</td><td>");

		value_kind = json_object_get_string_member(setting, "type");

		if (0 == g_strcmp0(value_kind, "boolean"))
		{
			gboolean on;

			on = json_object_get_boolean_member(setting, "value");
			g_string_append_printf(content, "<span class=\"badge %s\">%s</span>",
			                       on ? "badge-ok" : "badge-muted",
			                       on ? "on" : "off");
		}
		else if (0 == g_strcmp0(value_kind, "integer"))
		{
			g_string_append_printf(content, "%" G_GINT64_FORMAT,
				json_object_get_int_member(setting, "value"));
		}
		else if (0 == g_strcmp0(value_kind, "list"))
		{
			JsonArray *items;
			guint j;

			items = json_object_get_array_member(setting, "value");

			if (0 == json_array_get_length(items))
			{
				g_string_append(content, "<span class=\"muted\">none</span>");
			}
			else
			{
				for (j = 0; j < json_array_get_length(items); j++)
				{
					if (j > 0)
						g_string_append(content, ", ");

					g_string_append(content, "<code>");
					venture_html_escape_append(content,
						json_array_get_string_element(items, j));
					g_string_append(content, "</code>");
				}
			}
		}
		else
		{
			const gchar *text;

			text = json_object_get_string_member(setting, "value");

			if (venture_string_is_empty(text))
			{
				g_string_append(content, "<span class=\"muted\">unset</span>");
			}
			else
			{
				g_string_append(content, "<code>");
				venture_html_escape_append(content, text);
				g_string_append(content, "</code>");
			}
		}

		/*
		 * For a setting that names a secret's environment variable, say
		 * whether that variable is actually set. Never the value: the
		 * useful question is "is it present", and the answer to "what is
		 * it" belongs nowhere near a web page.
		 */
		if (json_object_has_member(setting, "secret_present"))
		{
			gboolean present;

			present = json_object_get_boolean_member(setting,
			                                         "secret_present");
			g_string_append_printf(content,
				" <span class=\"badge %s\">%s</span>",
				present ? "badge-ok" : "badge-warn",
				present ? "set" : "not set");
		}

		g_string_append(content, "</td><td><code class=\"muted\">");
		venture_html_escape_append(content,
			json_object_get_string_member(setting, "env"));
		g_string_append(content, "</code></td></tr>");
	}

	if (opened)
		g_string_append(content, "</tbody></table></div></div>");
}

/*
 * The settings page is deliberately read-only.
 *
 * Configuration is layered -- built-in defaults, then /etc, then the user's
 * file, then --config, then the compiled C configuration, then environment
 * variables, then command-line options. A form that wrote to one of those
 * could be silently overridden by a later layer, and the page would then be
 * showing a value the server is not using. Showing the resolved value, where
 * the file came from and which variable overrides it answers the question
 * people actually have.
 */
static HtmxResponse *
venture_web_ui_settings(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(JsonNode) described = NULL;
	g_autoptr(GString) content = NULL;
	VentureConfig *config;
	JsonArray *settings;
	HtmxResponse *redirect;

	self = user_data;

	/*
	 * This page names the bind address, every path the server reads from
	 * and which secrets are present. It is not for anonymous eyes.
	 */
	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	config = venture_context_get_config(self->context);
	described = venture_config_describe(config);
	settings = json_node_get_array(described);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Settings</h1>"
	                       "<p class=\"muted\">The configuration this server "
	                       "resolved at startup. Read-only: settings are "
	                       "layered, so edit the file or the environment and "
	                       "restart.</p>"
	                       "</div></div>");

	/* Where things stand right now, before the settings themselves. */
	g_string_append(content, "<div class=\"grid cols-3\">");

	{
		const gchar *loaded_from;

		loaded_from = venture_config_get_loaded_from(config);

		g_string_append(content,
			"<div class=\"card\"><div class=\"card-body\">"
			"<div class=\"metric-label\">Configuration file</div>"
			"<div class=\"metric-value small\"><code>");
		venture_html_escape_append(content,
			(NULL != loaded_from) ? loaded_from : "built-in defaults");
		g_string_append(content, "</code></div></div></div>");
	}

	g_string_append(content,
		"<div class=\"card\"><div class=\"card-body\">"
		"<div class=\"metric-label\">Database</div>"
		"<div class=\"metric-value small\">");
	venture_html_escape_append(content,
		venture_enum_to_nick(VENTURE_TYPE_DATABASE_BACKEND,
			(gint)venture_database_get_backend(
				venture_context_get_database(self->context))));
	g_string_append(content, "</div></div></div>");

	g_string_append(content,
		"<div class=\"card\"><div class=\"card-body\">"
		"<div class=\"metric-label\">Version</div>"
		"<div class=\"metric-value small\">");
	venture_html_escape_append(content, venture_get_version_string());
	g_string_append(content, "</div></div></div>");

	g_string_append(content, "</div>");

	/* What is loaded: the answer to "why is my plugin not working". */
	g_string_append(content, "<div class=\"grid cols-3\">");

	{
		VenturePluginManager *plugins;
		VentureAutomation *automation;
		VentureVentureTypeRegistry *types;
		g_autoptr(GPtrArray) type_names = NULL;

		plugins = venture_context_get_plugin_manager(self->context);
		automation = venture_context_get_automation(self->context);
		types = venture_context_get_venture_types(self->context);

		g_string_append_printf(content,
			"<div class=\"card\"><div class=\"card-body\">"
			"<div class=\"metric-label\">Plugins loaded</div>"
			"<div class=\"metric-value\">%u</div>"
			"<a class=\"btn btn-sm\" href=\"/api/v1/plugins\">Details</a>"
			"</div></div>",
			(NULL != plugins) ? venture_plugin_manager_get_count(plugins) : 0);

		type_names = venture_venture_type_registry_list(types);

		g_string_append_printf(content,
			"<div class=\"card\"><div class=\"card-body\">"
			"<div class=\"metric-label\">Venture types</div>"
			"<div class=\"metric-value\">%u</div>"
			"<a class=\"btn btn-sm\" href=\"/api/v1/venture-types\">Details</a>"
			"</div></div>",
			(NULL != type_names) ? type_names->len : 0);

		g_string_append_printf(content,
			"<div class=\"card\"><div class=\"card-body\">"
			"<div class=\"metric-label\">Automations</div>"
			"<div class=\"metric-value\">%u</div>"
			"<a class=\"btn btn-sm\" href=\"/api/v1/automations\">Details</a>"
			"</div></div>",
			(NULL != automation)
				? venture_automation_get_pod_count(automation) : 0);
	}

	g_string_append(content, "</div>");

	{
		static const struct
		{
			const gchar *section;
			const gchar *title;
		} sections[] = {
			{ "server",     "Server" },
			{ "database",   "Database" },
			{ "security",   "Security" },
			{ "locale",     "Locale and fiscal year" },
			{ "ai",         "AI" },
			{ "automation", "Automation" },
			{ "plugins",    "Plugins" },
			{ "ui",         "Interface" },
			{ "logging",    "Logging" }
		};
		gsize i;

		for (i = 0; i < G_N_ELEMENTS(sections); i++)
		{
			venture_web_settings_section(content, settings,
			                             sections[i].section,
			                             sections[i].title);
		}

		/* Anything with no section, so a setting added later cannot go
		 * missing from this page simply because nobody updated the
		 * table above. */
		venture_web_settings_section(content, settings, "", "Other");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/settings", "Settings", content->str), 200);
}

static HtmxResponse *
venture_web_api_settings(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(JsonNode) described = NULL;
	HtmxResponse *denied;

	self = user_data;

	/*
	 * Owner only. This names the bind address, every path the server
	 * reads from and which secrets are present -- more than a viewer of
	 * the books needs, and plenty for someone probing the host.
	 */
	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_OWNER);

	if (NULL != denied)
		return denied;
	described = venture_config_describe(
		venture_context_get_config(self->context));

	return venture_web_json_response(g_steal_pointer(&described), 200);
}

/* --- Health -------------------------------------------------------------- */

static HtmxResponse *
venture_web_api_health(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;

	self = user_data;

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder, "ok");

	json_builder_set_member_name(builder, "version");
	json_builder_add_string_value(builder, venture_get_version_string());

	json_builder_set_member_name(builder, "database");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_DATABASE_BACKEND,
			(gint)venture_database_get_backend(
				venture_context_get_database(self->context))));

	json_builder_set_member_name(builder, "ai");
	json_builder_add_boolean_value(builder,
		NULL != venture_context_get_ai_service(self->context));

	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/* --- Chat ---------------------------------------------------------------- */

static HtmxResponse *
venture_web_ui_chat(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) html = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *message;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	message = htmx_request_get_form_value(request, "message");
	html = g_string_new(NULL);

	/* The question is echoed immediately so the transcript reads as a
	 * conversation rather than answers appearing from nowhere. */
	g_string_append(html, "<div class=\"msg user\">"
	                      "<span class=\"msg-avatar\">You</span>"
	                      "<div class=\"msg-content\">");
	venture_html_escape_append(html, message);
	g_string_append(html, "</div></div>");

	if (NULL == venture_context_get_ai_service(self->context))
	{
		g_string_append(html, "<div class=\"msg ai\">"
		                      "<span class=\"msg-avatar\">\xe2\x9c\xa6</span>"
		                      "<div class=\"msg-content\">"
		                      "<div class=\"notice info\">AI is not configured "
		                      "on this instance.</div></div></div>");

		return venture_web_html_response(
			g_string_free(g_steal_pointer(&html), FALSE), 200);
	}

	/* The AI service answers; the dock renders whatever it returns. */
	{
		g_autofree gchar *answer = NULL;

		answer = venture_ai_service_answer(
			venture_context_get_ai_service(self->context), message,
			principal, &error);

		g_string_append(html, "<div class=\"msg ai\">"
		                      "<span class=\"msg-avatar\">\xe2\x9c\xa6</span>"
		                      "<div class=\"msg-content\">");

		if (NULL == answer)
		{
			g_string_append(html, "<div class=\"notice negative\">");
			venture_html_escape_append(html, error->message);
			g_string_append(html, "</div>");
		}
		else
		{
			g_string_append(html, "<p>");
			venture_html_escape_append(html, answer);
			g_string_append(html, "</p>");
		}

		g_string_append(html, "</div></div>");
	}

	return venture_web_html_response(
		g_string_free(g_steal_pointer(&html), FALSE), 200);
}


/* --- API tokens ---------------------------------------------------------- */

/*
 * Mints an API token and returns the plaintext once.
 *
 * This is a dedicated route rather than the generic create handler because
 * the secret exists only for the duration of this response: the record
 * stores a hash, and there is no later opportunity to reveal it. Making that
 * a distinct, obviously one-shot endpoint is clearer than a create call with
 * a surprising side effect.
 */
static HtmxResponse *
venture_web_api_mint_token(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureApiToken) token = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *secret = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	const gchar *name = NULL;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	/* Minting a credential is an administrative act, not an editing one. */
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN,
	                          &error))
		return venture_web_error_response(error);

	body = htmx_request_get_json(request, NULL);

	if ((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
	{
		name = venture_json_object_get_string(json_node_get_object(body),
		                                      "name", NULL);
	}

	token = venture_api_token_new();
	g_object_set(token, "name", (NULL != name) ? name : "api token",
	             "role", principal->role, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(token),
		venture_context_get_default_organization_id(self->context));

	secret = venture_api_token_generate(token);

	if (NULL == secret)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		                    "Could not generate a token");
		return venture_web_error_response(error);
	}

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(token), &actor, &error))
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder,
	                           venture_entity_get_id(VENTURE_ENTITY(token)));

	json_builder_set_member_name(builder, "token");
	json_builder_add_string_value(builder, secret);

	json_builder_set_member_name(builder, "note");
	json_builder_add_string_value(builder,
		"This is the only time the token is shown. Only its hash is stored.");

	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 201);
}

/* --- Plugins, venture types and automations ------------------------------ */

static HtmxResponse *
venture_web_api_plugins(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(JsonNode) node = NULL;
	VenturePluginManager *manager;
	HtmxResponse *denied;

	self = user_data;

	/* Names filesystem paths, so owner only. */
	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_OWNER);

	if (NULL != denied)
		return denied;
	manager = venture_context_get_plugin_manager(self->context);

	if (NULL == manager)
	{
		/* An empty array rather than an error: "no plugins" is a valid
		 * answer, and a client should not have to special-case it. */
		g_autoptr(JsonBuilder) builder = NULL;

		builder = json_builder_new();
		json_builder_begin_array(builder);
		json_builder_end_array(builder);
		node = json_builder_get_root(builder);

		return venture_web_json_response(node, 200);
	}

	node = venture_plugin_manager_list(manager);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_venture_types(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *denied;
	const gchar *name;

	self = user_data;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;
	name = g_hash_table_lookup(params, "name");

	if (NULL != name)
	{
		VentureVentureType *type;

		type = venture_venture_type_registry_lookup(
			venture_context_get_venture_types(self->context), name);

		if (NULL == type)
		{
			g_autoptr(GError) error = NULL;

			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "There is no venture type called \"%s\"", name);
			return venture_web_error_response(error);
		}

		node = venture_venture_type_to_json(type);

		return venture_web_json_response(node, 200);
	}

	node = venture_venture_type_registry_to_json(
		venture_context_get_venture_types(self->context));

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_automations(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(JsonNode) node = NULL;
	VentureAutomation *automation;
	HtmxResponse *denied;

	self = user_data;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_OWNER);

	if (NULL != denied)
		return denied;
	automation = venture_context_get_automation(self->context);

	if (NULL == automation)
	{
		g_autoptr(JsonBuilder) builder = NULL;

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "running");
		json_builder_add_boolean_value(builder, FALSE);
		json_builder_end_object(builder);
		node = json_builder_get_root(builder);

		return venture_web_json_response(node, 200);
	}

	node = venture_automation_describe(automation);

	return venture_web_json_response(node, 200);
}

/* --- AI confirmations ---------------------------------------------------- */

static HtmxResponse *
venture_web_api_confirmations(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) pending = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureAiService *ai;
	guint i;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	ai = venture_context_get_ai_service(self->context);
	builder = json_builder_new();
	json_builder_begin_array(builder);

	if (NULL != ai)
	{
		pending = venture_ai_service_list_pending(ai);

		for (i = 0; i < pending->len; i++)
		{
			json_builder_add_value(builder,
				venture_ai_confirmation_to_json(
					g_ptr_array_index(pending, i)));
		}
	}

	json_builder_end_array(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * Approves or rejects a staged AI change. Both go through one handler so the
 * authorisation and the audit path cannot diverge between them.
 */
static HtmxResponse *
venture_web_api_decide(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data,
	gboolean	 approve
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureAiService *ai;
	const gchar *id;
	gboolean ok;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	/* Approving an AI write is exactly the authority an editor has, and
	 * exactly what a viewer must not have. */
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ai = venture_context_get_ai_service(self->context);

	if (NULL == ai)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
		                    "AI is not configured on this instance");
		return venture_web_error_response(error);
	}

	id = g_hash_table_lookup(params, "id");

	ok = approve
		? venture_ai_service_approve(ai, id, principal, &error)
		: venture_ai_service_reject(ai, id, principal, &error);

	if (!ok)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, id);
	json_builder_set_member_name(builder, "state");
	json_builder_add_string_value(builder, approve ? "approved" : "rejected");
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_approve(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	return venture_web_api_decide(request, params, user_data, TRUE);
}

static HtmxResponse *
venture_web_api_reject(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	return venture_web_api_decide(request, params, user_data, FALSE);
}

/* ==========================================================================
 * Construction
 * ========================================================================== */

VentureWebServer *
venture_web_server_new(
	VentureContext	 *context,
	GError		**error
){
	g_autoptr(VentureWebServer) self = NULL;
	g_autoptr(HtmxConfig) config = NULL;
	HtmxRouter *router;
	g_autofree gchar *bind_address = NULL;
	gint64 port;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	self = g_object_new(VENTURE_TYPE_WEB_SERVER, NULL);
	self->context = g_object_ref(context);
	self->auth = venture_auth_new(context);

	g_object_get(venture_context_get_config(context),
	             "server-bind-address", &bind_address,
	             "server-port", &port,
	             NULL);

	self->port = (guint16)port;
	self->base_url = g_strdup_printf("http://%s:%" G_GINT64_FORMAT,
	                                 bind_address, port);

	config = htmx_config_new();
	htmx_config_set_host(config, bind_address);
	htmx_config_set_port(config, (guint16)port);

	self->server = htmx_server_new_with_config(config);
	router = htmx_server_get_router(self->server);

	/* UI */
	htmx_router_get(router, "/", venture_web_ui_dashboard, self);
	htmx_router_get(router, "/reports", venture_web_ui_reports, self);
	htmx_router_get(router, "/settings", venture_web_ui_settings, self);
	htmx_router_get(router, "/entity/:id", venture_web_ui_switch_entity, self);
	htmx_router_get(router, "/entities", venture_web_ui_entities, self);
	htmx_router_post(router, "/entities", venture_web_ui_entities_create, self);
	htmx_router_post(router, "/entities/:id/default",
	                 venture_web_ui_entities_default, self);
	htmx_router_post(router, "/entities/:id/delete",
	                 venture_web_ui_entities_delete, self);
	htmx_router_get(router, "/account", venture_web_ui_account, self);
	htmx_router_post(router, "/account/password",
	                 venture_web_ui_account_password, self);
	htmx_router_get(router, "/users", venture_web_ui_users, self);
	htmx_router_post(router, "/users", venture_web_ui_users_create, self);
	htmx_router_post(router, "/users/:id/update",
	                 venture_web_ui_users_update, self);
	htmx_router_post(router, "/users/:id/password",
	                 venture_web_ui_users_password, self);
	htmx_router_get(router, "/reports/:name", venture_web_ui_report, self);
	htmx_router_get(router, "/e/:type", venture_web_ui_list, self);
	htmx_router_get(router, "/e/:type/new", venture_web_ui_form, self);
	htmx_router_post(router, "/e/:type", venture_web_ui_save, self);
	htmx_router_get(router, "/e/:type/:id/edit", venture_web_ui_form, self);
	htmx_router_post(router, "/e/:type/:id", venture_web_ui_save, self);
	htmx_router_post(router, "/e/:type/:id/delete", venture_web_ui_delete,
	                 self);
	htmx_router_get(router, "/login", venture_web_ui_login_form, self);
	htmx_router_post(router, "/login", venture_web_ui_login_submit, self);
	htmx_router_get(router, "/logout", venture_web_ui_logout, self);
	htmx_router_post(router, "/ui/chat", venture_web_ui_chat, self);

	/* API */
	htmx_router_get(router, "/api/v1/health", venture_web_api_health, self);
	htmx_router_get(router, "/api/v1/schema", venture_web_api_describe, self);
	htmx_router_get(router, "/api/v1/schema/:type", venture_web_api_describe,
	                self);
	htmx_router_get(router, "/api/v1/reports", venture_web_api_reports, self);
	htmx_router_get(router, "/api/v1/reports/:name", venture_web_api_report,
	                self);

	/* Minting a token and deciding an AI change are registered before the
	 * generic record routes, so their paths are not swallowed by
	 * /api/v1/:type. */
	htmx_router_post(router, "/api/v1/tokens", venture_web_api_mint_token, self);
	htmx_router_get(router, "/api/v1/confirmations",
	                venture_web_api_confirmations, self);
	htmx_router_post(router, "/api/v1/confirmations/:id/approve",
	                 venture_web_api_approve, self);
	htmx_router_post(router, "/api/v1/confirmations/:id/reject",
	                 venture_web_api_reject, self);
	htmx_router_get(router, "/api/v1/settings", venture_web_api_settings, self);
	htmx_router_get(router, "/api/v1/plugins", venture_web_api_plugins, self);
	htmx_router_get(router, "/api/v1/venture-types",
	                venture_web_api_venture_types, self);
	htmx_router_get(router, "/api/v1/venture-types/:name",
	                venture_web_api_venture_types, self);
	htmx_router_get(router, "/api/v1/automations", venture_web_api_automations,
	                self);

	/*
	 * One set of handlers serves every record type. A plugin registering
	 * a type gets all five of these immediately, with no routing to add.
	 */
	htmx_router_get(router, "/api/v1/:type", venture_web_api_list, self);
	htmx_router_post(router, "/api/v1/:type", venture_web_api_create, self);
	htmx_router_get(router, "/api/v1/:type/:id", venture_web_api_get, self);
	htmx_router_put(router, "/api/v1/:type/:id", venture_web_api_update, self);
	htmx_router_patch(router, "/api/v1/:type/:id", venture_web_api_update, self);
	htmx_router_delete(router, "/api/v1/:type/:id", venture_web_api_delete,
	                   self);

	return g_steal_pointer(&self);
}

gboolean
venture_web_server_start(
	VentureWebServer	 *self,
	GError			**error
){
	g_autoptr(GError) local_error = NULL;

	g_return_val_if_fail(VENTURE_IS_WEB_SERVER(self), FALSE);

	if (!htmx_server_start(self->server, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
		            "Cannot listen on %s: %s", self->base_url,
		            (NULL != local_error) ? local_error->message
		                                  : "unknown failure");
		return FALSE;
	}

	return TRUE;
}

void
venture_web_server_stop(VentureWebServer *self)
{
	g_return_if_fail(VENTURE_IS_WEB_SERVER(self));

	htmx_server_stop(self->server);
}

const gchar *
venture_web_server_get_base_url(VentureWebServer *self)
{
	g_return_val_if_fail(VENTURE_IS_WEB_SERVER(self), NULL);

	return self->base_url;
}
