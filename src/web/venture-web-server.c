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

#ifdef VENTURE_HAVE_POPPLER
#include <poppler.h>
#endif

#include "venture-assets.h"

/*
 * A minted secret waiting to be shown once.
 *
 * The expiry is not a convenience. A handle that was redirected to but never
 * loaded -- the browser was closed, the redirect was lost -- would otherwise
 * keep a usable API token in memory for as long as the process runs.
 */
typedef struct
{
	gchar		*secret;
	gint64		 token_id;
	GDateTime	*created_at;
} VentureWebReveal;

#define VENTURE_WEB_REVEAL_TTL_SECONDS 300

static void
venture_web_reveal_free(VentureWebReveal *self)
{
	if (NULL == self)
		return;

	/*
	 * Wiped rather than merely freed. It is a live credential until the
	 * moment it is handed over, and a freed heap block is still readable
	 * in a core dump.
	 */
	if (NULL != self->secret)
	{
		memset(self->secret, 0, strlen(self->secret));
		g_free(self->secret);
	}

	g_clear_pointer(&self->created_at, g_date_time_unref);
	g_free(self);
}

struct _VentureWebServer
{
	GObject parent_instance;

	VentureContext	*context;
	VentureAuth	*auth;
	HtmxServer	*server;
	gchar		*base_url;
	guint16		 port;

	/*
	 * Freshly minted API tokens, keyed by a random handle, waiting to be
	 * shown to the browser exactly once.
	 *
	 * The mint is a POST and the page that displays the secret is a GET,
	 * because a mint that rendered its own response would create a second
	 * token every time somebody refreshed. Redirecting is the fix, and a
	 * redirect cannot carry a secret: a query string lands in history, in
	 * the access log and in any Referer the next page sends. So the
	 * secret stays here and the redirect carries only the handle, which
	 * is spent the moment it is read.
	 *
	 * No lock: htmx-glib serves every request from the one main context,
	 * so these are only ever touched from that thread.
	 */
	GHashTable	*reveals;
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
	g_clear_pointer(&self->reveals, g_hash_table_unref);

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
	self->reveals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      (GDestroyNotify)venture_web_reveal_free);
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

	/*
	 * Chat threads are personal, not business data: an editor listing
	 * /api/v1/chat_message would be reading every colleague's private
	 * conversations. The chat UI reaches them through its own routes,
	 * which filter to the caller's own user-id; the generic surface is
	 * owner-only so the one person who administers the install can still
	 * inspect or export them.
	 */
	if ((VENTURE_TYPE_CHAT_THREAD == entity_type) ||
	    (VENTURE_TYPE_CHAT_MESSAGE == entity_type))
		needed = VENTURE_USER_ROLE_OWNER;

	/* Plugin configuration steers loaded code; that is system
	 * administration, not data entry. */
	if (VENTURE_TYPE_PLUGIN_CONFIG == entity_type)
		needed = VENTURE_USER_ROLE_ADMIN;

	/*
	 * A forge record holds an access token and a webhook secret. The
	 * sensitive flag keeps both out of every response, but the row is
	 * still what decides where a credential is sent: an editor who could
	 * change base-url could point this install's token at a host they
	 * control, and the staged diff would show nothing but a URL changing.
	 * Same reasoning as user and api_token.
	 */
	if (VENTURE_TYPE_FORGE == entity_type)
		needed = VENTURE_USER_ROLE_OWNER;

	/*
	 * A rule decides whether a model runs unattended, which runner it
	 * gets, how many turns it may take and how far its output goes. That
	 * is administration for the same reason plugin configuration is.
	 */
	if (VENTURE_TYPE_FORGE_RULE == entity_type)
		needed = VENTURE_USER_ROLE_ADMIN;

	return venture_auth_require(self->auth, principal, needed, error);
}

/*
 * Whether a record type accepts writes through the generic surfaces at all.
 *
 * The audit log does not, for anybody. It is the record of what happened,
 * written only by the audit system itself as a side effect of the writes it
 * documents; a POST /api/v1/audit_entry that worked would let any editor
 * plant "someone else did this" rows, and an editable trail proves
 * nothing. Reading stays open -- the trail is only useful read.
 *
 * Returns: %TRUE if the type may be written
 */
static gboolean
venture_web_type_accepts_writes(
	GType	  entity_type,
	GError	**error
){
	if (VENTURE_TYPE_AUDIT_ENTRY == entity_type)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_PERMISSION_DENIED,
		                    "The audit log is written by the system as "
		                    "changes happen; it cannot be edited");
		return FALSE;
	}

	/*
	 * Nor does a run record. It is what a runner did, written as it did
	 * it: the branch it produced, what it cost, why it stopped. An
	 * editable one would let somebody rewrite the cost or the outcome
	 * after the fact, and the entire value of the record is that it is
	 * evidence. Same argument as the audit log, and reading stays open.
	 */
	if (VENTURE_TYPE_FORGE_RUN == entity_type)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_PERMISSION_DENIED,
		                    "A run record is written by the runner as it "
		                    "works; it cannot be edited");
		return FALSE;
	}

	return TRUE;
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

/*
 * Every sidebar icon, drawn at one stroke weight.
 *
 * These were assorted geometric Unicode glyphs -- a black circle beside a
 * white one beside a section sign -- which meant three problems at once.
 * Whichever font happened to supply each glyph decided its optical weight
 * and its advance width, so no two rows lined up; several were duplicated
 * across unrelated entries because the glyph repertoire ran out; and the
 * row height moved with the operator's font settings.
 *
 * Inline SVG at a fixed viewBox fixes all three, and the shared wrapper is
 * what guarantees a single stroke weight: an icon cannot pick its own.
 * They inherit currentColor, so the nav's hover and active states apply to
 * the icon without a second rule.
 */
#define VENTURE_ICON(body) \
	"<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" " \
	"stroke-width=\"1.75\" stroke-linecap=\"round\" " \
	"stroke-linejoin=\"round\" aria-hidden=\"true\" " \
	"focusable=\"false\">" body "</svg>"

/*
 * Turns a machine name into a display label: "in_progress" reads "In
 * progress", "research_note" reads "Research note".
 *
 * Only ever for text a person reads. The same nick is also a data-status
 * attribute the board posts back, a CSS class suffix on a ticket card and a
 * form option value, and humanising it in those places would break the
 * behaviour rather than the typography -- so this is applied at the point of
 * display and never to the value itself.
 *
 * Returns: (transfer full): the label
 */
/*
 * The assistant's mark. Defined once because it is emitted from four
 * places in this file and echoed by venture.js when the client renders
 * a message optimistically -- if those disagree, the avatar visibly
 * changes the moment the server's copy of the same message arrives.
 */
#define VENTURE_SPARK \
	"<svg viewBox=\"0 0 24 24\" fill=\"currentColor\" aria-hidden=\"true\" focusable=\"false\"><path d=\"M12 2.5l1.9 6.1 6.1 1.9-6.1 1.9-1.9 6.1-1.9-6.1L4 10.5l6.1-1.9L12 2.5z\"/></svg>"

static gchar *
venture_web_label_from_name(const gchar *name)
{
	gchar *label;
	gchar *cursor;

	if (venture_string_is_empty(name))
		return g_strdup("");

	label = g_strdup(name);

	for (cursor = label; '\0' != *cursor; cursor++)
	{
		if (('_' == *cursor) || ('-' == *cursor))
			*cursor = ' ';
	}

	/* ASCII only, deliberately: these are property and enum nicks, which
	 * the type system already constrains to [a-z0-9_-]. g_utf8_strup on
	 * the first character would drag in locale rules for no gain, and in
	 * a Turkish locale it would produce a dotted capital I. */
	if (g_ascii_islower(label[0]))
		label[0] = g_ascii_toupper(label[0]);

	return label;
}

static const VentureWebNavLink venture_web_nav_links[] = {
	{
		"/", "Dashboard",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"3\" width=\"7\" height=\"7\" rx=\"1.5\"/>"
			"<rect x=\"14\" y=\"3\" width=\"7\" height=\"7\" rx=\"1.5\"/>"
			"<rect x=\"3\" y=\"14\" width=\"7\" height=\"7\" rx=\"1.5\"/>"
			"<rect x=\"14\" y=\"14\" width=\"7\" height=\"7\" rx=\"1.5\"/>"
		),
		"Overview"
	},
	{
		"/reports", "Reports",
		VENTURE_ICON(
			"<path d=\"M3 20h18\"/><path d=\"M6 20v-6\"/>"
			"<path d=\"M12 20V5\"/><path d=\"M18 20v-9\"/>"
		),
		NULL
	},
	{
		"/e/venture", "Ventures",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"7\" width=\"18\" height=\"13\" rx=\"2\"/>"
			"<path d=\"M9 7V5a2 2 0 0 1 2-2h2a2 2 0 0 1 2 2v2\"/>"
			"<path d=\"M3 12h18\"/>"
		),
		"Business"
	},
	{
		"/e/sale", "Sales",
		VENTURE_ICON(
			"<path d=\"M3 17l6-6 4 4 8-8\"/><path d=\"M15 7h6v6\"/>"
		),
		NULL
	},
	{
		"/e/invoice", "Invoices",
		VENTURE_ICON(
			"<path d=\"M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z\"/>"
			"<path d=\"M14 3v5h5\"/><path d=\"M9 13h6\"/>"
			"<path d=\"M9 17h4\"/>"
		),
		NULL
	},
	{
		"/e/product", "Products",
		VENTURE_ICON(
			"<path d=\"M21 8l-9-5-9 5 9 5 9-5z\"/>"
			"<path d=\"M3 8v8l9 5 9-5V8\"/><path d=\"M12 13v8\"/>"
		),
		NULL
	},
	{
		"/e/inventory_item", "Inventory",
		VENTURE_ICON(
			"<path d=\"M12 2.5L3 7.5l9 5 9-5-9-5z\"/>"
			"<path d=\"M3 12l9 5 9-5\"/><path d=\"M3 16.5l9 5 9-5\"/>"
		),
		NULL
	},
	{
		"/e/expense", "Expenses",
		VENTURE_ICON(
			"<path d=\"M3 7l6 6 4-4 8 8\"/><path d=\"M15 17h6v-6\"/>"
		),
		"Money"
	},
	{
		"/e/account", "Accounts",
		VENTURE_ICON(
			"<path d=\"M3 21h18\"/><path d=\"M5 21V10\"/>"
			"<path d=\"M9 21V10\"/><path d=\"M15 21V10\"/>"
			"<path d=\"M19 21V10\"/><path d=\"M12 3L3 8h18l-9-5z\"/>"
		),
		NULL
	},
	{
		"/e/tax_category", "Tax",
		VENTURE_ICON(
			"<path d=\"M19 5L5 19\"/>"
			"<circle cx=\"7.5\" cy=\"7.5\" r=\"2.5\"/>"
			"<circle cx=\"16.5\" cy=\"16.5\" r=\"2.5\"/>"
		),
		NULL
	},
	{
		"/e/company", "Companies",
		VENTURE_ICON(
			"<path d=\"M3 21h18\"/>"
			"<path d=\"M5 21V5a2 2 0 0 1 2-2h6a2 2 0 0 1 2 2v16\"/>"
			"<path d=\"M15 21V11h4a2 2 0 0 1 2 2v8\"/>"
			"<path d=\"M9 7h2\"/><path d=\"M9 11h2\"/>"
			"<path d=\"M9 15h2\"/>"
		),
		"Relations"
	},
	{
		"/e/contact", "Contacts",
		VENTURE_ICON(
			"<path d=\"M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2\"/>"
			"<circle cx=\"12\" cy=\"7\" r=\"4\"/>"
		),
		NULL
	},
	{
		"/e/deal", "Deals",
		VENTURE_ICON(
			"<path d=\"M4 8h13\"/><path d=\"M14 5l3 3-3 3\"/>"
			"<path d=\"M20 16H7\"/><path d=\"M10 13l-3 3 3 3\"/>"
		),
		NULL
	},
	{
		"/e/campaign", "Campaigns",
		VENTURE_ICON(
			"<path d=\"M4 10v4a1 1 0 0 0 1 1h2l5 4V5L7 9H5a1 1 0 0 0-1 1z\"/>"
			"<path d=\"M16.5 9.5a3.5 3.5 0 0 1 0 5\"/>"
			"<path d=\"M19.5 7a7 7 0 0 1 0 10\"/>"
		),
		"Growth"
	},
	{
		"/e/newsletter", "Newsletters",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"5\" width=\"18\" height=\"14\" rx=\"2\"/>"
			"<path d=\"M3.5 7l8.5 6 8.5-6\"/>"
		),
		NULL
	},
	{
		"/e/post", "Posts",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"4\" width=\"18\" height=\"16\" rx=\"2\"/>"
			"<path d=\"M7 9h10\"/><path d=\"M7 13h10\"/>"
			"<path d=\"M7 17h6\"/>"
		),
		NULL
	},
	{
		"/e/idea", "Ideas",
		VENTURE_ICON(
			"<path d=\"M9 18h6\"/><path d=\"M10 21h4\"/>"
			"<path d=\"M12 3a6 6 0 0 0-3.5 10.9c.6.5.9 1.2.9 1.9V16h5.2v-.2c0-.7.3-1.4.9-1.9A6 6 0 0 0 12 3z\"/>"
		),
		"Thinking"
	},
	{
		"/tickets", "Tickets",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\"/>"
			"<path d=\"M8 12l3 3 5-6\"/>"
		),
		NULL
	},
	{
		"/e/research_note", "Research",
		VENTURE_ICON(
			"<circle cx=\"11\" cy=\"11\" r=\"7\"/>"
			"<path d=\"M20 20l-3.9-3.9\"/>"
		),
		NULL
	},
	{
		"/e/forge_repo", "Repositories",
		VENTURE_ICON(
			"<circle cx=\"7\" cy=\"5\" r=\"2\"/>"
			"<circle cx=\"7\" cy=\"19\" r=\"2\"/>"
			"<circle cx=\"17\" cy=\"9\" r=\"2\"/><path d=\"M7 7v10\"/>"
			"<path d=\"M17 11v1a4 4 0 0 1-4 4H7\"/>"
		),
		"Code"
	},
	{
		"/e/forge_rule", "Agent rules",
		VENTURE_ICON(
			"<path d=\"M4 6h10\"/><path d=\"M18 6h2\"/>"
			"<circle cx=\"16\" cy=\"6\" r=\"2\"/><path d=\"M4 12h2\"/>"
			"<path d=\"M10 12h10\"/>"
			"<circle cx=\"8\" cy=\"12\" r=\"2\"/><path d=\"M4 18h8\"/>"
			"<path d=\"M16 18h4\"/>"
			"<circle cx=\"14\" cy=\"18\" r=\"2\"/>"
		),
		NULL
	},
	{
		"/e/forge_run", "Runs",
		VENTURE_ICON(
			"<circle cx=\"12\" cy=\"12\" r=\"9\"/>"
			"<path d=\"M10 8.5l6 3.5-6 3.5z\"/>"
		),
		NULL
	},
	{
		"/entities", "Entities",
		VENTURE_ICON(
			"<rect x=\"9\" y=\"3\" width=\"6\" height=\"5\" rx=\"1.5\"/>"
			"<rect x=\"2\" y=\"16\" width=\"6\" height=\"5\" rx=\"1.5\"/>"
			"<rect x=\"16\" y=\"16\" width=\"6\" height=\"5\" rx=\"1.5\"/>"
			"<path d=\"M12 8v4\"/>"
			"<path d=\"M5 16v-2a2 2 0 0 1 2-2h10a2 2 0 0 1 2 2v2\"/>"
		),
		"System"
	},
	{
		"/automations", "Automations",
		VENTURE_ICON(
			"<path d=\"M13 2L4 14h7l-1 8 9-12h-7l1-8z\"/>"
		),
		NULL
	},
	{
		"/plugins", "Plugins",
		VENTURE_ICON(
			"<path d=\"M9 2v6\"/><path d=\"M15 2v6\"/>"
			"<path d=\"M6 8h12v3a6 6 0 0 1-12 0V8z\"/>"
			"<path d=\"M12 17v5\"/>"
		),
		NULL
	},
	{
		"/account", "Your account",
		VENTURE_ICON(
			"<circle cx=\"12\" cy=\"12\" r=\"9\"/>"
			"<circle cx=\"12\" cy=\"10\" r=\"3\"/>"
			"<path d=\"M6.5 19a6 6 0 0 1 11 0\"/>"
		),
		NULL
	},
	{
		"/account/tokens", "API tokens",
		VENTURE_ICON(
			"<path d=\"M14 7a4 4 0 1 0-3.9 5H14l2 2 2-2 2 2 2-2-2-2h-6z\"/>"
			"<circle cx=\"7\" cy=\"11\" r=\"1.2\"/>"
		),
		NULL
	},
	{
		"/users", "Users",
		VENTURE_ICON(
			"<path d=\"M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2\"/>"
			"<circle cx=\"9\" cy=\"7\" r=\"4\"/>"
			"<path d=\"M22 21v-2a4 4 0 0 0-3-3.9\"/>"
			"<path d=\"M16 3.1a4 4 0 0 1 0 7.8\"/>"
		),
		NULL
	},
	{
		"/settings", "Settings",
		VENTURE_ICON(
			"<circle cx=\"12\" cy=\"12\" r=\"3\"/>"
			"<path d=\"M19.4 15a1.7 1.7 0 0 0 .3 1.9l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-2.9 1.2V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-2.9-1.2l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0-1.2-2.9H3a2 2 0 1 1 0-4h.1A1.7 1.7 0 0 0 4.3 7.1l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 2.9-1.2V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 2.9 1.2l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0 1.2 2.9H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z\"/>"
		),
		NULL
	},
	{
		"/e/forge", "Forges",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"4\" width=\"18\" height=\"7\" rx=\"2\"/>"
			"<rect x=\"3\" y=\"13\" width=\"18\" height=\"7\" rx=\"2\"/>"
			"<path d=\"M7 7.5h.01\"/><path d=\"M7 16.5h.01\"/>"
		),
		NULL
	},
	{
		"/e/audit_entry", "Audit log",
		VENTURE_ICON(
			"<path d=\"M3.5 12a8.5 8.5 0 1 0 2.5-6\"/>"
			"<path d=\"M3 3v5h5\"/><path d=\"M12 8v4.5l3 1.5\"/>"
		),
		NULL
	},
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

	/*
	 * The configured accent overrides the stylesheet's default without
	 * needing the stylesheet regenerated.
	 *
	 * It sets --accent-config rather than --accent: the stylesheet mixes
	 * every accent value -- the link colour, its hover, the soft wash --
	 * out of that one hue, and the dark palette lightens it for a dark
	 * ground. Setting --accent here directly would be outranked by the
	 * dark theme's own :root[data-theme="dark"] rule, so the configured
	 * colour would apply in light mode and silently disappear in dark.
	 */
	g_string_append(html, "<style>:root{--accent-config:");
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

	/*
	 * One box that reaches everything. A GET form, so a search is a URL --
	 * shareable, bookmarkable, and reachable with scripting disabled;
	 * Ctrl+K only focuses it.
	 */
	g_string_append(html,
		"<form class=\"sidebar-search\" action=\"/search\" method=\"get\">"
		"<input type=\"search\" name=\"q\" placeholder=\"Search\xe2\x80\xa6\" "
		"data-global-search title=\"Search everything (Ctrl+K)\">"
		"</form>");

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

	/*
	 * The AI surface: a floating launcher, and a right-hand panel it
	 * opens. The panel is rendered on every page rather than fetched on
	 * demand so that opening it costs nothing and a mid-conversation
	 * navigation does not lose the input box; the transcript inside it is
	 * what the client fills in, from whichever thread localStorage says
	 * was active. dock_expanded carries its old meaning forward: start
	 * with the panel open.
	 */
	if (chat_dock)
	{
		g_string_append(html,
			"<button type=\"button\" class=\"ai-fab\" data-ai-toggle "
			"title=\"Ask VENTURE (Ctrl+/)\">"
			"<span class=\"spark\">"
			"<svg viewBox=\"0 0 24 24\" fill=\"currentColor\" aria-hidden=\"true\" focusable=\"false\">"
			"<path d=\"M12 2.5l1.9 6.1 6.1 1.9-6.1 1.9-1.9 6.1-1.9-6.1L4 10.5l6.1-1.9L12 2.5z\"/></svg>"
			"</span>"
			"<span class=\"ai-fab-label\">Ask VENTURE</span></button>");

		g_string_append_printf(html, "<aside class=\"ai-panel%s\" "
		                       "data-ai-panel aria-label=\"AI assistant\">",
		                       dock_expanded ? " open" : "");

		/* The grab edge. Dragging it resizes the panel; double-click
		 * puts the default width back. */
		g_string_append(html,
			"<div class=\"ai-panel-resizer\" data-ai-resize "
			"title=\"Drag to resize\"></div>");

		g_string_append(html,
			"<div class=\"ai-panel-head\">"
			"<span class=\"spark\">"
			"<svg viewBox=\"0 0 24 24\" fill=\"currentColor\" aria-hidden=\"true\" focusable=\"false\">"
			"<path d=\"M12 2.5l1.9 6.1 6.1 1.9-6.1 1.9-1.9 6.1-1.9-6.1L4 10.5l6.1-1.9L12 2.5z\"/></svg>"
			"</span>"
			"<span class=\"ai-panel-title\" id=\"ai-panel-title\">"
			"Ask VENTURE</span>"
			"<div class=\"ai-panel-actions\">"
			"<button type=\"button\" class=\"btn btn-ghost btn-sm\" "
			"data-ai-threads title=\"Previous conversations\" "
			"hx-get=\"/ui/chat/threads\" hx-target=\"#chat-log\" "
			"hx-swap=\"innerHTML\">\xe2\x98\xb0</button>"
			"<button type=\"button\" class=\"btn btn-ghost btn-sm\" "
			"data-ai-new title=\"New conversation\">+</button>"
			"<button type=\"button\" class=\"btn btn-ghost btn-sm\" "
			"data-ai-close title=\"Hide (Esc)\">\xc3\x97</button>"
			"</div></div>");

		g_string_append(html, "<div class=\"ai-panel-body chat-log\" "
		                      "id=\"chat-log\">");

		if (NULL == venture_context_get_ai_service(self->context))
		{
			/* Saying why the assistant is inert beats a box that
			 * silently does nothing when you type in it. */
			g_string_append(html, "<div class=\"notice info\">"
			                      "AI is not configured. Set a provider API "
			                      "key and restart to enable it.</div>");
		}

		g_string_append(html, "</div>");
		/* Pending attachments appear here as removable chips before the
		 * message they will ride along with is sent. */
		g_string_append(html, "<div class=\"chat-attachments\" "
		                      "id=\"chat-attachments\"></div>");

		g_string_append(html,
			"<form class=\"chat-input\" hx-post=\"/ui/chat\" "
			"hx-target=\"#chat-log\" hx-swap=\"beforeend\">"
			"<input type=\"hidden\" id=\"chat-thread\" name=\"thread\" "
			"value=\"\">"
			"<input type=\"hidden\" id=\"chat-attach-ids\" "
			"name=\"attachments\" value=\"\">"
			"<input type=\"file\" id=\"chat-attach-file\" multiple "
			"class=\"hidden\" "
			"accept=\".pdf,.txt,.md,.org,.csv,.json,.yaml,.yml,"
			".png,.jpg,.jpeg,.webp,.gif,"
			"application/pdf,text/plain,text/csv,application/json,"
			"image/png,image/jpeg,image/webp,image/gif\">"
			"<button type=\"button\" class=\"btn btn-ghost chat-attach\" "
			"data-ai-attach "
			"title=\"Attach a file or screenshot (or just paste one)\">"
			"\xf0\x9f\x93\x8e</button>"
			"<textarea name=\"message\" rows=\"1\" "
			"placeholder=\"Ask about your ventures, or describe a change\">"
			"</textarea>"
			"<button class=\"btn btn-primary\" type=\"submit\">Send</button>"
			"</form>");
		g_string_append(html, "</aside>");
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
	                               query, &error);

	/* The UI path renders a failed count as "?", but an API client reads
	 * numbers programmatically, and a total of -1 walking into someone's
	 * pagination arithmetic is worse than an honest error. */
	if (total < 0)
		return venture_web_error_response(error);

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
 * Whether this write should be proposed instead of performed.
 *
 * Only the four generic write verbs read it. The parsing lives in the
 * library so that an unrecognised spelling is refused rather than read as
 * "no" -- a caller who wrote stage=y meant to hold the write back, and
 * treating the unknown value as false applies it instead.
 */
static gboolean
venture_web_stage_requested(
	HtmxRequest	 *request,
	gboolean	 *out_stage,
	GError		**error
){
	return venture_confirmation_parse_stage_flag(
		htmx_request_get_query_param(request, "stage"), out_stage, error);
}

/*
 * The reply to a write that was staged rather than performed.
 *
 * 202 rather than 201 or 200, and the body says plainly that nothing has
 * changed: a client that read a 201 here would report a record it could not
 * then fetch.
 */
static HtmxResponse *
venture_web_staged_response(VentureConfirmation *confirmation)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder, "awaiting_approval");

	json_builder_set_member_name(builder, "staged");
	json_builder_add_boolean_value(builder, TRUE);

	json_builder_set_member_name(builder, "confirmation");
	json_builder_add_value(builder,
		venture_confirmation_to_json(confirmation));

	json_builder_set_member_name(builder, "note");
	json_builder_add_string_value(builder,
		"Nothing has been changed. This is waiting for somebody with the "
		"editor role to approve it at POST /api/v1/confirmations/"
		"<id>/approve, or reject it. It is listed by GET "
		"/api/v1/confirmations until it is decided or expires.");

	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 202);
}

/*
 * Stages a write that has been built and would otherwise have been saved.
 *
 * @original is the record as it stood, kept so that an approval arriving
 * after somebody else edited the row can say what moved rather than
 * overwriting it.
 */
static HtmxResponse *
venture_web_api_stage(
	VentureWebServer	*self,
	VentureEntity		*record,
	VentureEntity		*original,
	VentureAuthPrincipal	*principal,
	VentureAuditAction	 action
){
	g_autoptr(GError) error = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;

	/* The same actor a direct write would have been audited as, so the
	 * queue records who asked in the spelling the audit trail uses. */
	venture_auth_to_actor(principal, &origin);

	confirmation = venture_confirmation_store_stage(
		venture_context_get_confirmations(self->context), action, record,
		original, &origin, "rest-api", &error);

	if (NULL == confirmation)
		return venture_web_error_response(error);

	return venture_web_staged_response(confirmation);
}

/*
 * Applies a JSON body to a record and saves it. Shared by create and update
 * so the two cannot drift in what they accept.
 *
 * @stage turns the save into a proposal. Everything above that point is the
 * same code, deliberately: a staged create that parsed its body differently
 * from a direct one would be a second create, and the whole reason approval
 * applies the staged *object* is that it cannot then mean something else.
 */
static HtmxResponse *
venture_web_api_write(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureEntity		*record,
	VentureEntity		*original,
	VentureAuthPrincipal	*principal,
	gboolean		 created,
	gboolean		 stage
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

	if (stage)
	{
		return venture_web_api_stage(self, record, original, principal,
			created ? VENTURE_AUDIT_ACTION_CREATE
			        : VENTURE_AUDIT_ACTION_UPDATE);
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
	gboolean stage;

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

	if (!venture_web_type_accepts_writes(entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_stage_requested(request, &stage, &error))
		return venture_web_error_response(error);

	record = g_object_new(entity_type, NULL);

	return venture_web_api_write(self, request, record, NULL, principal, TRUE,
	                             stage);
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
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(GError) error = NULL;
	GType entity_type;
	const gchar *id_text;
	gboolean stage;

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

	if (!venture_web_type_accepts_writes(entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_stage_requested(request, &stage, &error))
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

	/* A second, unmodified copy: it is what the diff is taken against,
	 * and what an approval arriving after somebody else edited the row
	 * compares with to say which fields moved. */
	if (stage)
	{
		original = venture_database_get(
			venture_context_get_database(self->context), entity_type,
			g_ascii_strtoll(id_text, NULL, 10), NULL);
	}

	return venture_web_api_write(self, request, record, original, principal,
	                             FALSE, stage);
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
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	GType entity_type;
	const gchar *id_text;
	gboolean stage;

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

	if (!venture_web_type_accepts_writes(entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_stage_requested(request, &stage, &error))
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

	if (stage)
	{
		/* Untouched by the deletion the approval will perform, so a
		 * stale approval can still name what changed underneath. */
		original = venture_database_get(
			venture_context_get_database(self->context), entity_type,
			g_ascii_strtoll(id_text, NULL, 10), NULL);

		return venture_web_api_stage(self, record, original, principal,
		                             VENTURE_AUDIT_ACTION_DELETE);
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
		g_string_append(content, "<div class=\"mb-4\"></div>");
	}

	g_string_append(content, "<div class=\"dash-grid\">");

	/*
	 * What needs doing. Counts per status rather than a ticket list: the
	 * dashboard's job is "is anything on fire", and the board is one
	 * click away for the detail.
	 */
	{
		static const VentureTicketStatus open_statuses[] = {
			VENTURE_TICKET_STATUS_TRIAGE,
			VENTURE_TICKET_STATUS_TODO,
			VENTURE_TICKET_STATUS_IN_PROGRESS,
			VENTURE_TICKET_STATUS_BLOCKED,
			VENTURE_TICKET_STATUS_REVIEW
		};
		gint64 open_total;
		gsize s;

		open_total = 0;

		g_string_append(content, "<div class=\"card dash-card\">"
		                         "<div class=\"card-head\"><h2>Tickets"
		                         "</h2><a class=\"btn btn-sm\" "
		                         "href=\"/tickets\">Board</a></div>"
		                         "<div class=\"card-body\">"
		                         "<div class=\"stat-row\">");

		for (s = 0; s < G_N_ELEMENTS(open_statuses); s++)
		{
			g_autoptr(VentureQuery) query = NULL;
			const gchar *nick;
			gint64 count;

			query = venture_query_new(VENTURE_TYPE_TICKET);

			/*
			 * Filtered by nick, the same way the board does it.
			 *
			 * These counters used to pass the ordinal to
			 * venture_query_add_filter_int(), which printed it as a
			 * number and asked the database whether the text 'todo'
			 * equals 1 -- a valid comparison that never matches. All
			 * five read zero on every install, and a dashboard
			 * reporting "nothing open" looks like a quiet week rather
			 * than a bug.
			 *
			 * The query layer now translates an ordinal on an enum
			 * column, so either spelling works. This one stays
			 * explicit because it is the same spelling the board and
			 * the list filters use, and one encoding of a status in
			 * the codebase is easier to keep right than two.
			 */
			nick = venture_enum_to_nick(VENTURE_TYPE_TICKET_STATUS,
			                            (gint)open_statuses[s]);

			if (!venture_query_add_filter_string(query, "status",
			                                     VENTURE_FILTER_OP_EQ,
			                                     nick, NULL))
				continue;

			venture_web_scope_to_active_organization(self, request,
			                                         query);

			count = venture_database_count(
				venture_context_get_database(self->context), query,
				NULL);

			if (count < 0)
				continue;

			open_total += count;

			g_string_append(content, "<div class=\"stat\">"
			                         "<span class=\"stat-value\">");
			g_string_append_printf(content, "%" G_GINT64_FORMAT,
			                       count);
			g_string_append(content, "</span>"
			                         "<span class=\"stat-label\">");
			{
				g_autofree gchar *label = NULL;

				label = venture_web_label_from_name(nick);
				venture_html_escape_append(content, label);
			}
			g_string_append(content, "</span></div>");
		}

		g_string_append(content, "</div>");

		if (0 == open_total)
			g_string_append(content, "<p class=\"muted\">Nothing "
			                         "open. Either impressive or "
			                         "suspicious.</p>");

		g_string_append(content, "</div></div>");
	}

	/*
	 * The pipeline: every open deal, weighted by its probability. Summed
	 * here rather than in SQL because money summation must refuse mixed
	 * currencies, and the report engine's rules apply in C.
	 */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) deals = NULL;
		g_autoptr(VentureMoney) pipeline = NULL;
		gboolean mixed;
		guint open_deals;
		guint d;

		query = venture_query_new(VENTURE_TYPE_DEAL);
		venture_web_scope_to_active_organization(self, request, query);
		venture_query_set_limit(query, 1000);

		deals = venture_database_find(
			venture_context_get_database(self->context), query, NULL);

		mixed = FALSE;
		open_deals = 0;

		for (d = 0; (NULL != deals) && (d < deals->len); d++)
		{
			VentureDeal *deal;
			g_autoptr(VentureMoney) weighted = NULL;
			VentureDealStage stage;

			deal = g_ptr_array_index(deals, d);
			g_object_get(deal, "stage", &stage, NULL);

			if ((VENTURE_DEAL_STAGE_WON == stage) ||
			    (VENTURE_DEAL_STAGE_LOST == stage))
				continue;

			open_deals++;
			weighted = venture_deal_get_weighted_value(deal, NULL);

			if (NULL == weighted)
				continue;

			if (NULL == pipeline)
			{
				pipeline = g_steal_pointer(&weighted);
			}
			else
			{
				VentureMoney *sum;

				sum = venture_money_add(pipeline, weighted, NULL);

				if (NULL == sum)
				{
					/* Two currencies in one pipeline: any
					 * single figure would be wrong, so no
					 * figure is shown. */
					mixed = TRUE;
					break;
				}

				g_clear_pointer(&pipeline, venture_money_free);
				pipeline = sum;
			}
		}

		g_string_append(content, "<div class=\"card dash-card\">"
		                         "<div class=\"card-head\"><h2>Pipeline"
		                         "</h2><a class=\"btn btn-sm\" "
		                         "href=\"/e/deal\">Deals</a></div>"
		                         "<div class=\"card-body\">"
		                         "<div class=\"stat-row\">");

		g_string_append(content, "<div class=\"stat\">"
		                         "<span class=\"stat-value\">");
		g_string_append_printf(content, "%u", open_deals);
		g_string_append(content, "</span><span class=\"stat-label\">"
		                         "open</span></div>");

		g_string_append(content, "<div class=\"stat\">"
		                         "<span class=\"stat-value\">");

		if (mixed)
		{
			g_string_append(content, "\xe2\x80\x94");
		}
		else if (NULL != pipeline)
		{
			g_autofree gchar *text = NULL;

			text = venture_money_to_display_string(pipeline, TRUE);
			venture_html_escape_append(content, text);
		}
		else
		{
			g_string_append(content, "0");
		}

		g_string_append(content, "</span><span class=\"stat-label\">"
		                         "weighted</span></div>");
		g_string_append(content, "</div>");

		if (mixed)
			g_string_append(content, "<p class=\"muted\">Deals in "
			                         "more than one currency; a single "
			                         "total would be wrong.</p>");

		g_string_append(content, "</div></div>");
	}

	/*
	 * What just happened, from the audit trail -- which already records
	 * every write from every surface, so this feed is free and cannot
	 * disagree with the truth.
	 */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) entries = NULL;
		guint e;

		query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING,
		                        NULL);
		venture_query_set_limit(query, 8);

		entries = venture_database_find(
			venture_context_get_database(self->context), query, NULL);

		g_string_append(content, "<div class=\"card dash-card dash-wide\">"
		                         "<div class=\"card-head\"><h2>Recent "
		                         "activity</h2><a class=\"btn btn-sm\" "
		                         "href=\"/e/audit_entry\">Audit log</a>"
		                         "</div><ul class=\"activity\">");

		for (e = 0; (NULL != entries) && (e < entries->len); e++)
		{
			VentureEntity *entry;
			g_autofree gchar *actor = NULL;
			g_autofree gchar *target_type = NULL;
			g_autofree gchar *target_label = NULL;
			g_autoptr(GDateTime) occurred = NULL;
			VentureAuditAction action;
			gint64 target_id;

			entry = g_ptr_array_index(entries, e);
			g_object_get(entry,
			             "actor", &actor,
			             "action", &action,
			             "target-type", &target_type,
			             "target-id", &target_id,
			             "target-label", &target_label,
			             "occurred-at", &occurred,
			             NULL);

			g_string_append(content, "<li><span class=\"activity-actor\">");
			venture_html_escape_append(content,
				venture_string_is_empty(actor) ? "someone" : actor);
			g_string_append(content, "</span> ");
			venture_html_escape_append(content,
				venture_enum_to_nick(VENTURE_TYPE_AUDIT_ACTION,
				                     (gint)action));
			g_string_append(content, " ");

			if (!venture_string_is_empty(target_type) &&
			    (target_id > 0))
			{
				g_string_append_printf(content,
					"<a href=\"/e/%s/%" G_GINT64_FORMAT "\">",
					target_type, target_id);
				venture_html_escape_append(content,
					!venture_string_is_empty(target_label)
						? target_label : target_type);
				g_string_append(content, "</a>");
			}
			else
			{
				venture_html_escape_append(content,
					!venture_string_is_empty(target_label)
						? target_label : "something");
			}

			if (NULL != occurred)
			{
				g_autofree gchar *when = NULL;

				when = venture_time_to_date_string(occurred,
					venture_context_get_timezone(self->context));

				if (NULL != when)
				{
					g_string_append(content,
						" <span class=\"activity-when\">");
					venture_html_escape_append(content, when);
					g_string_append(content, "</span>");
				}
			}

			g_string_append(content, "</li>");
		}

		if ((NULL == entries) || (0 == entries->len))
			g_string_append(content, "<li class=\"muted\">Nothing has "
			                         "happened yet.</li>");

		g_string_append(content, "</ul></div>");
	}

	g_string_append(content, "</div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/", "Dashboard", content->str), 200);
}

/* --- The 404 -------------------------------------------------------------- */

/*
 * The terminal not-found handler, as middleware: the router has no
 * unmatched-route hook, so this calls the rest of the pipeline and takes
 * over only when nothing else produced a response.
 *
 * API paths get the same JSON error shape as every other API failure;
 * browser paths get a page in the normal chrome, because a bare-text 404
 * with no navigation is a dead end where a wrong URL should be a wrong
 * turn. Anonymous browsers are redirected to the login page instead --
 * whether a path exists is not information for people without a session.
 */
static void
venture_web_not_found_middleware(
	HtmxContext		*context,
	HtmxMiddlewareNext	 next,
	gpointer		 next_data,
	gpointer		 user_data
){
	VentureWebServer *self;
	HtmxRequest *request;
	const gchar *path;

	self = user_data;

	next(context, next_data);

	if (NULL != htmx_context_get_response(context))
		return;

	request = htmx_context_get_request(context);
	path = htmx_request_get_path(request);

	if (g_str_has_prefix(path, "/api/"))
	{
		g_autoptr(GError) error = NULL;

		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No route %s", path);
		htmx_context_set_response(context,
		                          venture_web_error_response(error));
		return;
	}

	{
		HtmxResponse *redirect;

		redirect = venture_web_ui_require_session(self, request);

		if (NULL != redirect)
		{
			htmx_context_set_response(context, redirect);
			return;
		}
	}

	{
		g_autoptr(GString) body = NULL;

		body = g_string_new("<div class=\"empty\">"
			"<span class=\"empty-icon\">"
						 VENTURE_ICON(
							"<path d=\"M3 13h4l2 3h6l2-3h4\"/>"
							"<path d=\"M5.5 6h13l2.5 7v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-5L5.5 6z\"/>"
						 )
						 "</span>"
			"<h3>There is no page at ");
		venture_html_escape_append(body, path);
		g_string_append(body,
			"</h3><p class=\"muted\">Either the link is stale or the "
			"address is mistyped.</p>"
			"<p><a class=\"btn btn-primary\" href=\"/\">Dashboard</a> "
			"<a class=\"btn\" href=\"/search\">Search</a></p></div>");

		htmx_context_set_response(context, venture_web_html_response(
			venture_web_page(self, request, NULL, "Not found",
			                 body->str), 404));
	}
}

/* --- Automations ----------------------------------------------------------- */

/*
 * The configured pods file, resolved the same way the automation engine
 * resolves it at start -- so the file the editor shows is the file the
 * engine loads, not a lookalike.
 *
 * Returns: (transfer full): the path
 */
static gchar *
venture_web_pods_file_path(VentureWebServer *self)
{
	g_autofree gchar *configured = NULL;

	g_object_get(venture_context_get_config(self->context),
	             "automation-pods-file", &configured, NULL);

	return venture_config_resolve_path(
		venture_context_get_config(self->context), configured);
}

/*
 * GET /automations - the engine, its pods, and the rules editor.
 *
 * The editor's diagnostics come from the same parser that loads the file
 * (see /automations/validate), which is the property that makes them worth
 * having: what validates here loads there, and the line numbers agree.
 */
static HtmxResponse *
venture_web_ui_automations(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) content = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *source = NULL;
	g_autoptr(GError) error = NULL;
	VentureAutomation *automation;
	HtmxResponse *redirect;
	const gchar *notice;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	/* Rules run code -- inline crispy and bash included -- so even
	 * reading them is administration, not data entry. */
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN,
	                          &error))
		return venture_web_error_response(error);

	automation = venture_context_get_automation(self->context);
	path = venture_web_pods_file_path(self);
	g_file_get_contents(path, &source, NULL, NULL);
	notice = htmx_request_get_query_param(request, "notice");

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Automations</h1><span class=\"subtitle\">");

	if (NULL == automation)
	{
		g_string_append(content, "Engine disabled in configuration");
	}
	else
	{
		g_string_append_printf(content, "%s \xc2\xb7 %u pod%s loaded",
			venture_automation_is_running(automation)
				? "Running" : "Stopped",
			venture_automation_get_pod_count(automation),
			(1 == venture_automation_get_pod_count(automation))
				? "" : "s");
	}

	g_string_append(content, "</span></div><div class=\"page-actions\">");

	if (NULL != automation)
		g_string_append(content,
			"<form method=\"post\" action=\"/automations/reload\">"
			"<button class=\"btn\" type=\"submit\" "
			"title=\"Rebuild the engine from the file on disk\">"
			"Reload</button></form>");

	g_string_append(content, "</div></div>");

	if (0 == g_strcmp0(notice, "saved"))
		g_string_append(content, "<div class=\"notice positive\">"
		                         "Saved and reloaded. The rules running "
		                         "now are the rules below.</div>");
	else if (0 == g_strcmp0(notice, "reloaded"))
		g_string_append(content, "<div class=\"notice positive\">"
		                         "Engine rebuilt from the file on disk."
		                         "</div>");

	/* The pods actually loaded, from the engine's own accounting. */
	if (NULL != automation)
	{
		g_autoptr(JsonNode) described = NULL;
		JsonArray *pods = NULL;

		described = venture_automation_describe(automation);

		if ((NULL != described) && JSON_NODE_HOLDS_OBJECT(described) &&
		    json_object_has_member(json_node_get_object(described), "pods"))
			pods = json_object_get_array_member(
				json_node_get_object(described), "pods");

		if ((NULL != pods) && (json_array_get_length(pods) > 0))
		{
			guint i;

			g_string_append(content,
				"<div class=\"card\"><div class=\"card-head\">"
				"<h2>Loaded pods</h2></div>"
				"<ul class=\"pod-list\">");

			for (i = 0; i < json_array_get_length(pods); i++)
			{
				g_string_append(content,
					"<li><span class=\"pod-name\">");
				venture_html_escape_append(content,
					json_array_get_string_element(pods, i));
				g_string_append(content, "</span></li>");
			}

			g_string_append(content, "</ul></div>");
		}
	}

	/* The editor. A plain form POST, so saving works with scripting off;
	 * the gutter, idle validation and error markers are progressive. */
	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Rules</h2><span class=\"muted\">");
	venture_html_escape_append(content, path);
	g_string_append(content, "</span></div><div class=\"card-body\">");

	g_string_append(content,
		"<form method=\"post\" action=\"/automations/save\" "
		"data-pod-editor>"
		"<div class=\"code-editor\">"
		"<pre class=\"code-gutter\" aria-hidden=\"true\"></pre>"
		"<textarea name=\"source\" class=\"code-input\" spellcheck=\"false\" "
		"placeholder=\"pod &quot;monthly-hosting&quot; {\n"
		"    on cron.schedule(&quot;0 9 1 * *&quot;) {\n"
		"        venture.create(&quot;expense&quot;, ...)\n    }\n}\">");

	if (NULL != source)
		venture_html_escape_append(content, source);

	g_string_append(content,
		"</textarea></div>"
		"<div class=\"editor-status\" id=\"pod-diagnostics\" "
		"data-validate-url=\"/automations/validate\">"
		"Diagnostics appear as you type.</div>"
		"<div class=\"editor-actions\">"
		"<button class=\"btn btn-primary\" type=\"submit\">"
		"Save and reload</button>"
		"<span class=\"muted\">Refused unless it parses; the running "
		"rules are never replaced with something broken.</span>"
		"</div></form></div></div>");

	/* The reference: what the rules can actually call, from the live
	 * module manager rather than a list that drifts. */
	if (NULL != automation)
	{
		g_autoptr(JsonNode) modules = NULL;

		modules = venture_automation_describe_modules(automation);

		g_string_append(content,
			"<div class=\"card\"><div class=\"card-head\">"
			"<h2>Module reference</h2></div>"
			"<div class=\"card-body\"><dl class=\"module-reference\">");

		if ((NULL != modules) && JSON_NODE_HOLDS_ARRAY(modules))
		{
			JsonArray *list;
			guint i;

			list = json_node_get_array(modules);

			for (i = 0; i < json_array_get_length(list); i++)
			{
				JsonObject *module;

				module = json_array_get_object_element(list, i);

				g_string_append(content, "<dt><code>");
				venture_html_escape_append(content,
					venture_json_object_get_string(module,
						"name", ""));
				g_string_append(content, "</code></dt><dd>");
				venture_html_escape_append(content,
					venture_json_object_get_string(module,
						"description", ""));
				g_string_append(content, "</dd>");
			}
		}

		g_string_append(content,
			"</dl><p class=\"muted\">The <code>venture</code> module "
			"raises <code>on_created</code>, <code>on_updated</code> "
			"and <code>on_deleted</code> for every record change, and "
			"its handlers can query, report, create, update and delete "
			"records. Everything an automation writes is audited as "
			"the automation actor. Worked examples -- recurring "
			"expenses included -- ship in "
			"<code>data/examples/automations.pod</code>.</p>"
			"</div></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/automations", "Automations",
		                 content->str), 200);
}

/*
 * POST /automations/validate - diagnostics for the editor, as JSON.
 */
static HtmxResponse *
venture_web_ui_automations_validate(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *message = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *source;
	gboolean ok;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN,
	                          &error))
		return venture_web_error_response(error);

	source = htmx_request_get_form_value(request, "source");
	ok = venture_automation_validate_dsl((NULL != source) ? source : "",
	                                     &message);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "ok");
	json_builder_add_boolean_value(builder, ok);

	if (!ok)
	{
		json_builder_set_member_name(builder, "message");
		json_builder_add_string_value(builder, message);
	}

	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * POST /automations/save - validate, write, rebuild.
 *
 * The order is the guarantee: nothing reaches the file until it parses, so
 * the engine is never rebuilt against garbage and a typo cannot take the
 * running rules down.
 */
static HtmxResponse *
venture_web_ui_automations_save(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	VentureAutomation *automation;
	HtmxResponse *redirect;
	const gchar *source;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	/* Rules execute code on the server; writing them is the owner's. */
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER,
	                          &error))
		return venture_web_error_response(error);

	source = htmx_request_get_form_value(request, "source");

	if (NULL == source)
		source = "";

	if (!venture_automation_validate_dsl(source, &message))
	{
		g_autoptr(GString) body = NULL;

		body = g_string_new("<div class=\"notice negative\">"
		                    "Not saved: ");
		venture_html_escape_append(body, message);
		g_string_append(body, "</div><p><a class=\"btn\" "
		                      "href=\"/automations\">Back to the editor"
		                      "</a></p>");

		return venture_web_html_response(
			venture_web_page(self, request, "/automations",
			                 "Automations", body->str), 422);
	}

	path = venture_web_pods_file_path(self);

	{
		g_autofree gchar *directory = NULL;

		directory = g_path_get_dirname(path);
		g_mkdir_with_parents(directory, 0700);
	}

	if (!g_file_set_contents(path, source, -1, &error))
		return venture_web_error_response(error);

	automation = venture_context_get_automation(self->context);

	if (NULL != automation)
	{
		if (!venture_automation_reload(automation, &error))
			return venture_web_error_response(error);
	}

	return venture_web_redirect_to("/automations?notice=saved");
}

/*
 * POST /automations/reload - rebuild from the file on disk, for the file
 * edited outside the browser.
 */
static HtmxResponse *
venture_web_ui_automations_reload(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	VentureAutomation *automation;
	HtmxResponse *redirect;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER,
	                          &error))
		return venture_web_error_response(error);

	automation = venture_context_get_automation(self->context);

	if (NULL == automation)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Automation is disabled in configuration");
		return venture_web_error_response(error);
	}

	if (!venture_automation_reload(automation, &error))
		return venture_web_error_response(error);

	return venture_web_redirect_to("/automations?notice=reloaded");
}

/* --- Invoices -------------------------------------------------------------- */

/* Defined beside the detail page, which also renders invoices. */
static GPtrArray *
venture_web_invoice_lines(
	VentureWebServer	 *self,
	gint64			  invoice_id,
	GError			**error
);

static VentureMoney *
venture_web_invoice_total(
	GPtrArray	 *lines,
	GError		**error
);

/*
 * POST /invoices/:id/status - one transition of the invoice lifecycle.
 *
 * The interesting one is paid. Paying an invoice is the moment owed money
 * becomes earned money, so the transition creates the sale itself -- gross
 * from the invoice total, venture from the invoice, memo naming the number.
 * Wired here rather than left to discipline, because the alternative is an
 * invoicing system and a set of books that quietly disagree about revenue.
 */
static HtmxResponse *
venture_web_ui_invoice_status(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	VentureInvoiceStatus status;
	VentureInvoiceStatus target;
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *to;
	gint64 id;
	gint value;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	record = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_INVOICE, id, &error);

	if (NULL == record)
		return venture_web_error_response(error);

	to = htmx_request_get_form_value(request, "to");

	if (!venture_enum_from_nick(VENTURE_TYPE_INVOICE_STATUS, to, &value))
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not an invoice status", to);
		return venture_web_error_response(error);
	}

	target = (VentureInvoiceStatus)value;
	g_object_get(record, "status", &status, NULL);

	/*
	 * The lifecycle is a line, not a graph: draft -> sent -> paid, with
	 * void an exit from anywhere unpaid. Paid is terminal -- un-paying
	 * an invoice would strand the sale the payment created.
	 */
	{
		gboolean allowed;

		allowed =
			((VENTURE_INVOICE_STATUS_DRAFT == status) &&
			 (VENTURE_INVOICE_STATUS_SENT == target)) ||
			((VENTURE_INVOICE_STATUS_SENT == status) &&
			 (VENTURE_INVOICE_STATUS_PAID == target)) ||
			(((VENTURE_INVOICE_STATUS_DRAFT == status) ||
			  (VENTURE_INVOICE_STATUS_SENT == status)) &&
			 (VENTURE_INVOICE_STATUS_VOID == target));

		if (!allowed)
		{
			g_set_error(&error, VENTURE_ERROR,
			            VENTURE_ERROR_VALIDATION,
			            "An invoice cannot go from %s to %s",
			            venture_enum_to_nick(
			                VENTURE_TYPE_INVOICE_STATUS, (gint)status),
			            venture_enum_to_nick(
			                VENTURE_TYPE_INVOICE_STATUS, (gint)target));
			return venture_web_error_response(error);
		}
	}

	now = venture_time_now();
	venture_auth_to_actor(principal, &actor);

	g_object_set(record, "status", target, NULL);

	if (VENTURE_INVOICE_STATUS_SENT == target)
	{
		g_autoptr(GDateTime) issued = NULL;

		/* Sending stamps the issue date unless one was set by hand. */
		g_object_get(record, "issued-at", &issued, NULL);

		if (NULL == issued)
			g_object_set(record, "issued-at", now, NULL);
	}

	if (VENTURE_INVOICE_STATUS_PAID == target)
		g_object_set(record, "paid-at", now, NULL);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           record, &actor, &error))
		return venture_web_error_response(error);

	/* The revenue, recorded the moment it becomes real. */
	if (VENTURE_INVOICE_STATUS_PAID == target)
	{
		g_autoptr(GPtrArray) lines = NULL;
		g_autoptr(VentureMoney) total = NULL;

		lines = venture_web_invoice_lines(self, id, NULL);
		total = venture_web_invoice_total(lines, &error);

		if ((NULL == total) && (NULL != error))
			return venture_web_error_response(error);

		if (NULL != total)
		{
			g_autoptr(VentureSale) sale = NULL;
			g_autofree gchar *number = NULL;
			g_autofree gchar *memo = NULL;
			gint64 venture_id;

			g_object_get(record, "number", &number,
			             "venture-id", &venture_id, NULL);
			memo = g_strdup_printf("Invoice %s", number);

			sale = venture_sale_new();
			g_object_set(sale,
			             "venture-id", venture_id,
			             "gross", total,
			             "occurred-at", now,
			             "channel", "invoice",
			             "external-id", number,
			             "notes", memo,
			             NULL);
			venture_entity_set_organization_id(VENTURE_ENTITY(sale),
				venture_entity_get_organization_id(record));

			if (!venture_database_save(
				venture_context_get_database(self->context),
				VENTURE_ENTITY(sale), &actor, &error))
				return venture_web_error_response(error);
		}
	}

	destination = g_strdup_printf("/e/invoice/%" G_GINT64_FORMAT, id);

	return venture_web_redirect_to(destination);
}

/*
 * GET /invoices/:id/print - the invoice as a clean printable page: no
 * chrome, no sidebar, just the document. The browser's print dialog is the
 * PDF generator; it is already installed everywhere.
 */
static HtmxResponse *
venture_web_ui_invoice_print(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GString) html = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *terms = NULL;
	g_autofree gchar *ui_title = NULL;
	g_autoptr(GDateTime) issued = NULL;
	g_autoptr(GDateTime) due = NULL;
	VentureInvoiceStatus status;
	HtmxResponse *redirect;
	gint64 company_id;
	gint64 id;
	guint i;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	record = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_INVOICE, id, &error);

	if (NULL == record)
		return venture_web_error_response(error);

	lines = venture_web_invoice_lines(self, id, &error);

	if (NULL == lines)
		return venture_web_error_response(error);

	g_object_get(record, "number", &number, "status", &status,
	             "issued-at", &issued, "due-at", &due,
	             "terms", &terms, "company-id", &company_id, NULL);
	g_object_get(venture_context_get_config(self->context),
	             "ui-title", &ui_title, NULL);

	html = g_string_new(
		"<!doctype html><html lang=\"en\"><head>"
		"<meta charset=\"utf-8\"><title>Invoice ");
	venture_html_escape_append(html, number);
	g_string_append(html,
		"</title><style>"
		"body{font:14px/1.5 system-ui,sans-serif;color:#111;"
		"max-width:720px;margin:40px auto;padding:0 20px}"
		"h1{font-size:22px;margin:0 0 4px}"
		".head{display:flex;justify-content:space-between;"
		"align-items:baseline;margin-bottom:28px}"
		".meta{color:#555;font-size:13px}"
		"table{width:100%;border-collapse:collapse;margin:20px 0}"
		"th,td{text-align:left;padding:8px 10px;"
		"border-bottom:1px solid #ddd}"
		".num{text-align:right}"
		"tfoot td{border-bottom:none;font-weight:700}"
		".status{display:inline-block;padding:2px 10px;"
		"border:1px solid #999;border-radius:999px;font-size:12px;"
		"text-transform:uppercase;letter-spacing:0.06em}"
		".terms{color:#555;font-size:13px;margin-top:24px;"
		"white-space:pre-wrap}"
		"@media print{body{margin:0 auto}}"
		"</style></head><body>");

	g_string_append(html, "<div class=\"head\"><div><h1>Invoice ");
	venture_html_escape_append(html, number);
	g_string_append(html, "</h1><div class=\"meta\">");
	venture_html_escape_append(html, ui_title);
	g_string_append(html, "</div></div><span class=\"status\">");
	venture_html_escape_append(html,
		venture_enum_to_nick(VENTURE_TYPE_INVOICE_STATUS, (gint)status));
	g_string_append(html, "</span></div>");

	g_string_append(html, "<div class=\"meta\">");

	if (company_id > 0)
	{
		g_autoptr(VentureEntity) company = NULL;

		company = venture_database_get(
			venture_context_get_database(self->context),
			VENTURE_TYPE_COMPANY, company_id, NULL);

		if (NULL != company)
		{
			g_autofree gchar *label = NULL;

			label = venture_entity_get_display_name(company);
			g_string_append(html, "Billed to: <strong>");
			venture_html_escape_append(html, label);
			g_string_append(html, "</strong><br>");
		}
	}

	if (NULL != issued)
	{
		g_autofree gchar *when = NULL;

		when = venture_time_to_date_string(issued,
			venture_context_get_timezone(self->context));
		g_string_append(html, "Issued: ");
		venture_html_escape_append(html, when);
		g_string_append(html, "<br>");
	}

	if (NULL != due)
	{
		g_autofree gchar *when = NULL;

		when = venture_time_to_date_string(due,
			venture_context_get_timezone(self->context));
		g_string_append(html, "Due: ");
		venture_html_escape_append(html, when);
	}

	g_string_append(html, "</div>");

	g_string_append(html, "<table><thead><tr><th>Description</th>"
	                      "<th class=\"num\">Qty</th>"
	                      "<th class=\"num\">Unit</th>"
	                      "<th class=\"num\">Amount</th></tr></thead><tbody>");

	for (i = 0; i < lines->len; i++)
	{
		VentureInvoiceLine *line;
		g_autofree gchar *description = NULL;
		g_autoptr(VentureMoney) unit_price = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		gdouble quantity;

		line = g_ptr_array_index(lines, i);
		g_object_get(line, "description", &description,
		             "quantity", &quantity,
		             "unit-price", &unit_price, NULL);
		amount = venture_invoice_line_get_amount(line, NULL);

		g_string_append(html, "<tr><td>");
		venture_html_escape_append(html, description);
		g_string_append_printf(html,
			"</td><td class=\"num\">%g</td><td class=\"num\">",
			quantity);

		if (NULL != unit_price)
		{
			g_autofree gchar *text = NULL;

			text = venture_money_to_display_string(unit_price, TRUE);
			venture_html_escape_append(html, text);
		}

		g_string_append(html, "</td><td class=\"num\">");

		if (NULL != amount)
		{
			g_autofree gchar *text = NULL;

			text = venture_money_to_display_string(amount, TRUE);
			venture_html_escape_append(html, text);
		}

		g_string_append(html, "</td></tr>");
	}

	g_string_append(html, "</tbody><tfoot><tr>"
	                      "<td colspan=\"3\" class=\"num\">Total</td>"
	                      "<td class=\"num\">");

	{
		g_autoptr(VentureMoney) total = NULL;

		total = venture_web_invoice_total(lines, NULL);

		if (NULL != total)
		{
			g_autofree gchar *text = NULL;

			text = venture_money_to_display_string(total, TRUE);
			venture_html_escape_append(html, text);
		}
	}

	g_string_append(html, "</td></tr></tfoot></table>");

	if (!venture_string_is_empty(terms))
	{
		g_string_append(html, "<div class=\"terms\">");
		venture_html_escape_append(html, terms);
		g_string_append(html, "</div>");
	}

	g_string_append(html, "<script>window.print&&window.print()</script>"
	                      "</body></html>");

	return venture_web_html_response(
		g_string_free(g_steal_pointer(&html), FALSE), 200);
}

/* --- Plugins --------------------------------------------------------------- */

/*
 * GET /plugins - what is loaded, and each plugin's configuration.
 *
 * The configuration is YAML in a record, and saving it raises the plugin
 * manager's config-changed signal -- so a plugin that listens follows this
 * page live, without a restart. That loop is the point of the page: a
 * plugin whose settings live in a file it read once can only be
 * reconfigured by bouncing the server.
 */
static HtmxResponse *
venture_web_ui_plugins(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(JsonNode) list = NULL;
	g_autoptr(GError) error = NULL;
	VenturePluginManager *manager;
	HtmxResponse *redirect;
	const gchar *notice;
	JsonArray *plugins;
	guint i;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN,
	                          &error))
		return venture_web_error_response(error);

	manager = venture_context_get_plugin_manager(self->context);
	notice = htmx_request_get_query_param(request, "notice");

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Plugins</h1><span class=\"subtitle\">");

	if (NULL == manager)
	{
		g_string_append(content, "Plugins disabled in configuration"
		                         "</span></div></div>");
		return venture_web_html_response(
			venture_web_page(self, request, "/plugins", "Plugins",
			                 content->str), 200);
	}

	g_string_append_printf(content, "%u loaded",
	                       venture_plugin_manager_get_count(manager));
	g_string_append(content, "</span></div></div>");

	if (0 == g_strcmp0(notice, "saved"))
		g_string_append(content, "<div class=\"notice positive\">"
		                         "Saved. The plugin was signalled and has "
		                         "the new configuration now.</div>");

	list = venture_plugin_manager_list(manager);
	plugins = JSON_NODE_HOLDS_ARRAY(list)
		? json_node_get_array(list) : NULL;

	if ((NULL == plugins) || (0 == json_array_get_length(plugins)))
		g_string_append(content, "<div class=\"empty\">"
		                         "<span class=\"empty-icon\">"
						 VENTURE_ICON(
							"<path d=\"M3 13h4l2 3h6l2-3h4\"/>"
							"<path d=\"M5.5 6h13l2.5 7v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-5L5.5 6z\"/>"
						 )
						 "</span>"
		                         "<h3>No plugins loaded</h3>"
		                         "<p class=\"muted\">Drop a .so or a crispy "
		                         ".c file into a configured plugin directory "
		                         "and restart.</p></div>");

	for (i = 0; (NULL != plugins) && (i < json_array_get_length(plugins));
	     i++)
	{
		JsonObject *plugin;
		g_autofree gchar *config_text = NULL;
		const gchar *name;

		plugin = json_array_get_object_element(plugins, i);
		name = venture_json_object_get_string(plugin, "name", "unknown");
		config_text = venture_plugin_manager_get_config_text(manager,
		                                                     name);

		g_string_append(content, "<div class=\"card plugin-card\">"
		                         "<div class=\"card-head\"><h2>");
		venture_html_escape_append(content, name);
		g_string_append(content, "</h2><span class=\"badge\">");
		venture_html_escape_append(content,
			venture_json_object_get_string(plugin, "kind", ""));
		g_string_append(content, "</span></div><div class=\"card-body\">");

		{
			const gchar *description;

			description = venture_json_object_get_string(plugin,
				"description", NULL);

			if (NULL != description)
			{
				g_string_append(content, "<p class=\"muted\">");
				venture_html_escape_append(content, description);
				g_string_append(content, "</p>");
			}
		}

		g_string_append(content, "<form method=\"post\" "
		                         "action=\"/plugins/config\">"
		                         "<input type=\"hidden\" name=\"plugin\" "
		                         "value=\"");
		venture_html_escape_append(content, name);
		g_string_append(content, "\"><textarea name=\"config\" rows=\"6\" "
		                         "class=\"code-input\" spellcheck=\"false\" "
		                         "placeholder=\"# YAML, whatever shape this "
		                         "plugin documents\">");

		if (NULL != config_text)
			venture_html_escape_append(content, config_text);

		g_string_append(content,
			"</textarea>"
			"<div class=\"editor-actions\">"
			"<button class=\"btn btn-primary\" type=\"submit\">"
			"Save</button>"
			"<span class=\"muted\">Secrets belong in environment "
			"variables -- name the variable here instead.</span>"
			"</div></form></div></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/plugins", "Plugins",
		                 content->str), 200);
}

/*
 * POST /plugins/config - store one plugin's YAML and signal it.
 */
static HtmxResponse *
venture_web_ui_plugins_config(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	VenturePluginManager *manager;
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *plugin;
	const gchar *config;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN,
	                          &error))
		return venture_web_error_response(error);

	manager = venture_context_get_plugin_manager(self->context);

	if (NULL == manager)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Plugins are disabled in configuration");
		return venture_web_error_response(error);
	}

	plugin = htmx_request_get_form_value(request, "plugin");
	config = htmx_request_get_form_value(request, "config");

	if (venture_string_is_empty(plugin))
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Which plugin?");
		return venture_web_error_response(error);
	}

	venture_auth_to_actor(principal, &actor);

	if (!venture_plugin_manager_set_config(manager, plugin,
	                                       (NULL != config) ? config : "",
	                                       &actor, &error))
	{
		g_autoptr(GString) body = NULL;

		body = g_string_new("<div class=\"notice negative\">");
		venture_html_escape_append(body, error->message);
		g_string_append(body, "</div><p><a class=\"btn\" "
		                      "href=\"/plugins\">Back</a></p>");

		return venture_web_html_response(
			venture_web_page(self, request, "/plugins", "Plugins",
			                 body->str), 422);
	}

	return venture_web_redirect_to("/plugins?notice=saved");
}

/* --- Global search -------------------------------------------------------- */

/*
 * GET /search?q= - one query across every record type.
 *
 * This sweeps the registry rather than consulting a per-type list of what is
 * searchable, so a plugin's record type is findable the moment it registers
 * -- the same property the REST routes and the forms have. Which fields
 * match is decided by each type's own SEARCHABLE flags, in the query layer,
 * where it is also decided for the per-list search boxes.
 */
static HtmxResponse *
venture_web_ui_search(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) content = NULL;
	g_auto(GStrv) names = NULL;
	HtmxResponse *redirect;
	const gchar *q;
	guint groups;
	gsize i;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);
	q = htmx_request_get_query_param(request, "q");

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Search</h1><span class=\"subtitle\">");

	if (!venture_string_is_empty(q))
	{
		g_string_append(content, "Results for \xe2\x80\x9c");
		venture_html_escape_append(content, q);
		g_string_append(content, "\xe2\x80\x9d");
	}
	else
	{
		g_string_append(content, "Every record type, one box");
	}

	g_string_append(content, "</span></div></div>");

	g_string_append(content,
		"<form class=\"card search-form\" action=\"/search\" method=\"get\">"
		"<div class=\"card-body\">"
		"<input type=\"search\" name=\"q\" data-search-input "
		"placeholder=\"Search everything\" autofocus value=\"");

	if (NULL != q)
		venture_html_escape_append(content, q);

	g_string_append(content,
		"\">"
		"<button class=\"btn btn-primary\" type=\"submit\">Search</button>"
		"</div></form>");

	groups = 0;

	if (!venture_string_is_empty(q))
	{
		names = venture_entity_registry_list_names(
			venture_context_get_entity_registry(self->context));

		for (i = 0; NULL != names[i]; i++)
		{
			g_autoptr(VentureQuery) query = NULL;
			g_autoptr(GPtrArray) records = NULL;
			GType entity_type;
			gint64 total;
			guint j;

			entity_type = venture_entity_registry_lookup(
				venture_context_get_entity_registry(self->context),
				names[i]);

			if (G_TYPE_INVALID == entity_type)
				continue;

			/*
			 * The same per-type gate as every other route: whoever
			 * cannot list a type cannot search it either. Chat
			 * threads and the access tables fall out of results for
			 * an editor exactly as they fall out of /e/.
			 */
			if (!venture_web_require_for_type(self, principal,
			                                  entity_type,
			                                  VENTURE_USER_ROLE_VIEWER,
			                                  NULL))
				continue;

			/* The audit log matches nearly any term through its diff
			 * text, which buries the actual records. It has its own
			 * page. */
			if (VENTURE_TYPE_AUDIT_ENTRY == entity_type)
				continue;

			query = venture_query_new(entity_type);
			venture_query_set_search(query, q);
			venture_web_scope_to_active_organization(self, request,
			                                         query);
			venture_query_set_limit(query, 5);

			records = venture_database_find(
				venture_context_get_database(self->context), query,
				NULL);

			if ((NULL == records) || (0 == records->len))
				continue;

			total = venture_database_count(
				venture_context_get_database(self->context), query,
				NULL);

			groups++;

			g_string_append(content, "<div class=\"card search-group\">"
			                         "<div class=\"card-head\"><h2>");
			venture_html_escape_append(content, names[i]);
			{
				g_autofree gchar *escaped = NULL;

				escaped = g_uri_escape_string(q, NULL, FALSE);
				g_string_append_printf(content,
					"</h2><a class=\"btn btn-sm\" "
					"href=\"/e/%s?search=%s\">"
					"All %" G_GINT64_FORMAT "</a></div>",
					names[i], escaped,
					(total > 0) ? total : (gint64)records->len);
			}

			g_string_append(content, "<ul class=\"search-hits\">");

			for (j = 0; j < records->len; j++)
			{
				VentureEntity *record;
				g_autofree gchar *label = NULL;

				record = g_ptr_array_index(records, j);
				label = venture_entity_get_display_name(record);

				g_string_append_printf(content,
					"<li><a href=\"/e/%s/%" G_GINT64_FORMAT "\">",
					names[i],
					venture_entity_get_id(record));
				venture_html_escape_append(content, label);
				g_string_append(content, "</a></li>");
			}

			g_string_append(content, "</ul></div>");
		}

		if (0 == groups)
			g_string_append(content, "<div class=\"empty\">"
			                         "<span class=\"empty-icon\">"
						 VENTURE_ICON(
							"<path d=\"M3 13h4l2 3h6l2-3h4\"/>"
							"<path d=\"M5.5 6h13l2.5 7v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-5L5.5 6z\"/>"
						 )
						 "</span>"
			                         "<h3>Nothing matched</h3>"
			                         "<p class=\"muted\">Only fields marked "
			                         "searchable are looked at, and only in "
			                         "the active entity's scope.</p></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/search", "Search", content->str),
		200);
}

/*
 * Rebuilds a list URL's query-string so its links preserve each other's
 * state: sorting a column keeps the search, paging keeps the sort, and so
 * on. @order overrides the current order ("" drops it, %NULL keeps it);
 * @page below 2 drops the parameter, because page one is the URL with no
 * page at all.
 *
 * Returns: (transfer full): the query-string, starting with "?", or ""
 */
static gchar *
venture_web_list_query_string(
	HtmxRequest	*request,
	const gchar	*order,
	gint64		 page
){
	GString *out;
	const gchar *search;
	gchar separator;

	out = g_string_new(NULL);
	separator = '?';
	search = htmx_request_get_query_param(request, "search");

	if (NULL == order)
		order = htmx_request_get_query_param(request, "order");

	if (!venture_string_is_empty(search))
	{
		g_autofree gchar *escaped = NULL;

		escaped = g_uri_escape_string(search, NULL, FALSE);
		g_string_append_printf(out, "%csearch=%s", separator, escaped);
		separator = '&';
	}

	if (!venture_string_is_empty(order))
	{
		g_autofree gchar *escaped = NULL;

		escaped = g_uri_escape_string(order, NULL, FALSE);
		g_string_append_printf(out, "%corder=%s", separator, escaped);
		separator = '&';
	}

	if (page > 1)
		g_string_append_printf(out, "%cpage=%" G_GINT64_FORMAT,
		                       separator, page);

	return g_string_free(out, FALSE);
}

/*
 * Formats one field of one record as plain text, the way the list shows it
 * but without truncation. Shared by the table cells' CSV export.
 *
 * Returns: (transfer full): the text, possibly empty, never %NULL
 */
static gchar *
venture_web_field_to_text(
	VentureWebServer	*self,
	VentureEntity		*record,
	VentureFieldSpec	*spec
){
	g_auto(GValue) value = G_VALUE_INIT;

	if (!venture_entity_get_field(record, venture_field_spec_get_name(spec),
	                              &value))
		return g_strdup("");

	if (G_VALUE_HOLDS(&value, VENTURE_TYPE_MONEY))
	{
		const VentureMoney *money;

		money = g_value_get_boxed(&value);
		return (NULL != money)
			? venture_money_to_display_string(money, TRUE)
			: g_strdup("");
	}

	if (G_VALUE_HOLDS(&value, G_TYPE_DATE_TIME))
	{
		gchar *text;

		text = venture_time_to_date_string(g_value_get_boxed(&value),
			venture_context_get_timezone(self->context));

		return (NULL != text) ? text : g_strdup("");
	}

	if (G_VALUE_HOLDS_STRING(&value))
	{
		const gchar *text;

		text = g_value_get_string(&value);
		return g_strdup((NULL != text) ? text : "");
	}

	if (G_VALUE_HOLDS_ENUM(&value))
	{
		const gchar *nick;

		nick = venture_enum_to_nick(G_VALUE_TYPE(&value),
		                            g_value_get_enum(&value));
		return g_strdup((NULL != nick) ? nick : "");
	}

	if (G_VALUE_HOLDS_BOOLEAN(&value))
		return g_strdup(g_value_get_boolean(&value) ? "yes" : "");

	{
		g_autoptr(JsonNode) node = NULL;
		gchar *text;

		node = venture_json_node_from_value(&value);
		text = venture_json_to_string(node, FALSE);

		if (0 == g_strcmp0(text, "null"))
		{
			g_free(text);
			return g_strdup("");
		}

		return text;
	}
}

/*
 * GET /e/:type/export - the current view of a list, as CSV.
 *
 * "Current view" is the point: the same search, sort and entity scope the
 * table had, so the file that downloads is the table you were looking at
 * rather than a surprise full dump. Sensitive fields are excluded the same
 * way they are excluded everywhere, and cells are defused against
 * spreadsheet formula injection exactly as report exports are.
 */
static HtmxResponse *
venture_web_ui_export(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) records = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GString) csv = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *disposition = NULL;
	VentureEntity *prototype;
	HtmxResponse *response;
	GType entity_type;
	const gchar *type_name;
	guint i;
	guint j;

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

	/* The whole filtered set, not the visible page. Capped all the same:
	 * an export endpoint with no ceiling is a memory-exhaustion lever. */
	venture_query_set_offset(query, 0);
	venture_query_set_limit(query, 10000);

	records = venture_database_find(venture_context_get_database(self->context),
	                                query, &error);

	if (NULL == records)
		return venture_web_error_response(error);

	type_name = g_hash_table_lookup(params, "type");
	prototype = venture_entity_registry_get_prototype(
		venture_context_get_entity_registry(self->context), type_name);

	specs = venture_entity_get_field_specs(prototype);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	csv = g_string_new("id");

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;
		g_autofree gchar *escaped = NULL;

		spec = g_ptr_array_index(specs, i);

		if (0 != (venture_field_spec_get_flags(spec) &
		          VENTURE_COLUMN_FLAG_SENSITIVE))
			continue;

		escaped = venture_csv_escape(venture_field_spec_get_label(spec));
		g_string_append_c(csv, ',');
		g_string_append(csv, escaped);
	}

	g_string_append(csv, "\r\n");

	for (j = 0; j < records->len; j++)
	{
		VentureEntity *record;

		record = g_ptr_array_index(records, j);
		g_string_append_printf(csv, "%" G_GINT64_FORMAT,
		                       venture_entity_get_id(record));

		for (i = 0; i < specs->len; i++)
		{
			VentureFieldSpec *spec;
			g_autofree gchar *text = NULL;
			g_autofree gchar *escaped = NULL;

			spec = g_ptr_array_index(specs, i);

			if (0 != (venture_field_spec_get_flags(spec) &
			          VENTURE_COLUMN_FLAG_SENSITIVE))
				continue;

			text = venture_web_field_to_text(self, record, spec);
			escaped = venture_csv_escape(text);
			g_string_append_c(csv, ',');
			g_string_append(csv, escaped);
		}

		g_string_append(csv, "\r\n");
	}

	response = htmx_response_new_with_content(csv->str);
	htmx_response_set_content_type(response, "text/csv; charset=utf-8");
	htmx_response_set_status(response, 200);

	disposition = g_strdup_printf("attachment; filename=\"%s.csv\"",
	                              type_name);
	htmx_response_add_header(response, "Content-Disposition", disposition);

	return response;
}

/* --- CSV import ------------------------------------------------------------ */

/*
 * An RFC 4180 CSV parser: quoted fields, embedded commas, embedded quotes
 * doubled, embedded newlines inside quotes, and both line endings. Written
 * here rather than pulled in as a dependency because the format fits in a
 * page and the failure modes of a half-matching library do not.
 *
 * Returns: (transfer full): rows, each a %NULL-terminated GStrv
 */
static GPtrArray *
venture_web_csv_parse(
	const gchar	*data,
	gsize		 length
){
	GPtrArray *rows;
	GPtrArray *row;
	GString *field;
	gboolean quoted;
	gsize i;

	rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	row = g_ptr_array_new();
	field = g_string_new(NULL);
	quoted = FALSE;

	for (i = 0; i < length; i++)
	{
		gchar c;

		c = data[i];

		if (quoted)
		{
			if ('"' == c)
			{
				/* A doubled quote is a literal one. */
				if ((i + 1 < length) && ('"' == data[i + 1]))
				{
					g_string_append_c(field, '"');
					i++;
				}
				else
				{
					quoted = FALSE;
				}
			}
			else
			{
				g_string_append_c(field, c);
			}

			continue;
		}

		if (('"' == c) && (0 == field->len))
		{
			quoted = TRUE;
		}
		else if (',' == c)
		{
			g_ptr_array_add(row, g_string_free(field, FALSE));
			field = g_string_new(NULL);
		}
		else if (('\n' == c) || ('\r' == c))
		{
			if (('\r' == c) && (i + 1 < length) &&
			    ('\n' == data[i + 1]))
				i++;

			g_ptr_array_add(row, g_string_free(field, FALSE));
			field = g_string_new(NULL);

			/* A blank line is not a row of empty strings. */
			if ((1 != row->len) ||
			    ('\0' != *(gchar *)g_ptr_array_index(row, 0)))
			{
				g_ptr_array_add(row, NULL);
				g_ptr_array_add(rows,
					g_ptr_array_free(row, FALSE));
			}
			else
			{
				g_free(g_ptr_array_index(row, 0));
				g_ptr_array_unref(row);
			}

			row = g_ptr_array_new();
		}
		else
		{
			g_string_append_c(field, c);
		}
	}

	/* A last row with no trailing newline. */
	if ((field->len > 0) || (row->len > 0))
	{
		g_ptr_array_add(row, g_string_free(field, FALSE));
		g_ptr_array_add(row, NULL);
		g_ptr_array_add(rows, g_ptr_array_free(row, FALSE));
	}
	else
	{
		g_string_free(field, TRUE);
		g_ptr_array_unref(row);
	}

	return rows;
}

/*
 * Matches one CSV header against a type's fields, tolerantly: case does not
 * matter, and space, dash and underscore are the same character -- so the
 * file VENTURE exported ("Unit price") and the file a spreadsheet saved
 * ("unit_price") both map without anybody editing headers.
 *
 * Returns: (transfer none) (nullable): the matching spec, or %NULL
 */
static VentureFieldSpec *
venture_web_csv_match_header(
	GPtrArray	*specs,
	const gchar	*header
){
	g_autofree gchar *wanted = NULL;
	guint i;

	wanted = g_ascii_strdown(header, -1);
	g_strdelimit(wanted, " _", '-');
	g_strstrip(wanted);

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;
		g_autofree gchar *name = NULL;
		g_autofree gchar *label = NULL;

		spec = g_ptr_array_index(specs, i);

		name = g_ascii_strdown(venture_field_spec_get_name(spec), -1);
		g_strdelimit(name, " _", '-');

		label = g_ascii_strdown(venture_field_spec_get_label(spec), -1);
		g_strdelimit(label, " _", '-');

		if ((0 == g_strcmp0(wanted, name)) ||
		    (0 == g_strcmp0(wanted, label)))
			return spec;
	}

	return NULL;
}

/*
 * Builds the typed JSON value one CSV cell becomes, according to what the
 * field declared itself to be. The declaration decides -- the same principle
 * as everywhere else; a CSV of strings would otherwise import every number
 * as text and fail late.
 */
static JsonNode *
venture_web_csv_cell_to_json(
	VentureFieldSpec	*spec,
	const gchar		*cell
){
	JsonNode *node;

	switch (venture_field_spec_get_kind(spec))
	{
	case VENTURE_FIELD_KIND_INTEGER:
	case VENTURE_FIELD_KIND_REFERENCE:
		node = json_node_new(JSON_NODE_VALUE);
		json_node_set_int(node, g_ascii_strtoll(cell, NULL, 10));
		return node;

	case VENTURE_FIELD_KIND_DOUBLE:
		node = json_node_new(JSON_NODE_VALUE);
		json_node_set_double(node, g_ascii_strtod(cell, NULL));
		return node;

	case VENTURE_FIELD_KIND_BOOLEAN:
		node = json_node_new(JSON_NODE_VALUE);
		json_node_set_boolean(node,
			(0 == g_ascii_strcasecmp(cell, "yes")) ||
			(0 == g_ascii_strcasecmp(cell, "true")) ||
			(0 == g_strcmp0(cell, "1")));
		return node;

	default:
		/* Strings, text, money, dates, enums: the serialiser already
		 * accepts these as strings, with its own validation. */
		node = json_node_new(JSON_NODE_VALUE);
		json_node_set_string(node, cell);
		return node;
	}
}

/*
 * GET /e/:type/import - the upload form and a template link.
 */
static HtmxResponse *
venture_web_ui_import_form(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
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

	if (!venture_web_type_accepts_writes(entity_type, &error))
		return venture_web_error_response(error);

	type_name = g_hash_table_lookup(params, "type");

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Import ");
	venture_html_escape_append(content, type_name);
	g_string_append_printf(content,
		"</h1><span class=\"subtitle\">CSV in, records out</span></div>"
		"<div class=\"page-actions\">"
		"<a class=\"btn\" href=\"/e/%s/import/template\">Template</a>"
		"<a class=\"btn\" href=\"/e/%s\">Back</a></div></div>",
		type_name, type_name);

	g_string_append_printf(content,
		"<div class=\"card\"><div class=\"card-body\">"
		"<p>Headers may be field names or their labels -- the Template "
		"button gives you the exact set, and a file exported from the "
		"list page re-imports as-is. Unknown columns are ignored; empty "
		"cells leave the field at its default.</p>"
		"<p>Nothing is imported unless <strong>every</strong> row "
		"validates: an import is all-or-nothing, so a typo on row 40 "
		"never leaves you with 39 half-trusted records.</p>"
		"<form method=\"post\" action=\"/e/%s/import\" "
		"enctype=\"multipart/form-data\">"
		"<input type=\"file\" name=\"file\" accept=\".csv,text/csv\" "
		"required> "
		"<button class=\"btn btn-primary\" type=\"submit\">Import"
		"</button></form></div></div>", type_name);

	return venture_web_html_response(
		venture_web_page(self, request, NULL, "Import", content->str),
		200);
}

/*
 * GET /e/:type/import/template - the header row, ready to fill in.
 */
static HtmxResponse *
venture_web_ui_import_template(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GString) csv = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *disposition = NULL;
	VentureEntity *prototype;
	HtmxResponse *response;
	GType entity_type;
	const gchar *type_name;
	guint i;
	guint written;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);

	type_name = g_hash_table_lookup(params, "type");
	prototype = venture_entity_registry_get_prototype(
		venture_context_get_entity_registry(self->context), type_name);

	specs = venture_entity_get_field_specs(prototype);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	csv = g_string_new(NULL);
	written = 0;

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		if (0 != (venture_field_spec_get_flags(spec) &
		          (VENTURE_COLUMN_FLAG_SENSITIVE |
		           VENTURE_COLUMN_FLAG_TRANSIENT)))
			continue;

		if (written > 0)
			g_string_append_c(csv, ',');

		g_string_append(csv, venture_field_spec_get_name(spec));
		written++;
	}

	g_string_append(csv, "\r\n");

	response = htmx_response_new_with_content(csv->str);
	htmx_response_set_content_type(response, "text/csv; charset=utf-8");
	htmx_response_set_status(response, 200);
	disposition = g_strdup_printf("attachment; filename=\"%s-template.csv\"",
	                              type_name);
	htmx_response_add_header(response, "Content-Disposition", disposition);

	return response;
}

/*
 * POST /e/:type/import - the file, all of it or none of it.
 *
 * Every row is built and validated before anything is saved: an import
 * where row 40 fails after 39 saved is a state nobody asked for and nobody
 * can cleanly undo. What survives validation is then saved through the
 * ordinary path, so every imported record is audited like any other write.
 */
static HtmxResponse *
venture_web_ui_import(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) built = NULL;
	g_autoptr(GPtrArray) columns = NULL;
	g_autoptr(GString) errors = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	VentureEntity *prototype;
	HtmxResponse *redirect;
	GType entity_type;
	const gchar *type_name;
	VentureActor actor;
	guint error_count;
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

	if (!venture_web_type_accepts_writes(entity_type, &error))
		return venture_web_error_response(error);

	type_name = g_hash_table_lookup(params, "type");

	files = htmx_uploaded_file_parse_multipart(
		htmx_request_get_content_type(request),
		htmx_request_get_body_bytes(request), NULL, &error);

	if ((NULL == files) || (0 == files->len))
	{
		if (NULL == error)
			g_set_error_literal(&error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "No file arrived");
		return venture_web_error_response(error);
	}

	{
		GBytes *data;
		const gchar *bytes;
		gsize length;

		data = htmx_uploaded_file_get_data(g_ptr_array_index(files, 0));
		bytes = g_bytes_get_data(data, &length);

		if ((NULL == bytes) || (0 == length) ||
		    !g_utf8_validate(bytes, (gssize)length, NULL))
		{
			g_set_error_literal(&error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "The file is empty or not UTF-8 text");
			return venture_web_error_response(error);
		}

		rows = venture_web_csv_parse(bytes, length);
	}

	if (rows->len < 2)
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The file has a header but no rows");
		return venture_web_error_response(error);
	}

	if (rows->len > 5001)
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%u rows is over the 5000-row limit; split the file",
		            rows->len - 1);
		return venture_web_error_response(error);
	}

	prototype = venture_entity_registry_get_prototype(
		venture_context_get_entity_registry(self->context), type_name);
	specs = venture_entity_get_field_specs(prototype);

	/* Header -> spec, once. NULL entries are ignored columns. */
	columns = g_ptr_array_new();

	{
		gchar **header;

		header = g_ptr_array_index(rows, 0);

		for (i = 0; NULL != header[i]; i++)
			g_ptr_array_add(columns,
				venture_web_csv_match_header(specs, header[i]));
	}

	/* Build and validate everything before saving anything. */
	built = g_ptr_array_new_with_free_func(g_object_unref);
	errors = g_string_new(NULL);
	error_count = 0;

	for (i = 1; i < rows->len; i++)
	{
		g_autoptr(VentureEntity) record = NULL;
		g_autoptr(JsonBuilder) builder = NULL;
		g_autoptr(JsonNode) values = NULL;
		g_autoptr(GError) row_error = NULL;
		gchar **cells;
		guint c;

		cells = g_ptr_array_index(rows, i);

		builder = json_builder_new();
		json_builder_begin_object(builder);

		for (c = 0; (NULL != cells[c]) && (c < columns->len); c++)
		{
			VentureFieldSpec *spec;

			spec = g_ptr_array_index(columns, c);

			if ((NULL == spec) ||
			    venture_string_is_empty(cells[c]))
				continue;

			if (0 != (venture_field_spec_get_flags(spec) &
			          VENTURE_COLUMN_FLAG_TRANSIENT))
				continue;

			json_builder_set_member_name(builder,
				venture_field_spec_get_name(spec));
			json_builder_add_value(builder,
				venture_web_csv_cell_to_json(spec, cells[c]));
		}

		json_builder_end_object(builder);
		values = json_builder_get_root(builder);

		record = venture_entity_registry_create(
			venture_context_get_entity_registry(self->context),
			type_name, &row_error);

		if ((NULL != record) &&
		    venture_serializable_from_json(VENTURE_SERIALIZABLE(record),
		                                   values, &row_error))
		{
			venture_entity_set_organization_id(record,
				venture_context_get_default_organization_id(
					self->context));

			if (venture_entity_validate(record, &row_error))
			{
				g_ptr_array_add(built,
				                g_steal_pointer(&record));
				continue;
			}
		}

		error_count++;

		if (error_count <= 20)
		{
			g_string_append_printf(errors,
				"<li>Row %u: ", i + 1);
			venture_html_escape_append(errors,
				(NULL != row_error) ? row_error->message
				                    : "unusable");
			g_string_append(errors, "</li>");
		}
	}

	content = g_string_new(NULL);

	if (error_count > 0)
	{
		g_string_append_printf(content,
			"<div class=\"notice negative\">Nothing was imported: "
			"%u row%s failed validation.</div><ul>",
			error_count, (1 == error_count) ? "" : "s");
		g_string_append(content, errors->str);

		if (error_count > 20)
			g_string_append_printf(content,
				"<li>\xe2\x80\xa6 and %u more</li>",
				error_count - 20);

		g_string_append_printf(content,
			"</ul><p><a class=\"btn\" href=\"/e/%s/import\">"
			"Try again</a></p>", type_name);

		return venture_web_html_response(
			venture_web_page(self, request, NULL, "Import",
			                 content->str), 422);
	}

	venture_auth_to_actor(principal, &actor);

	for (i = 0; i < built->len; i++)
	{
		if (!venture_database_save(
			venture_context_get_database(self->context),
			g_ptr_array_index(built, i), &actor, &error))
			return venture_web_error_response(error);
	}

	g_string_append_printf(content,
		"<div class=\"notice positive\">Imported %u record%s.</div>"
		"<p><a class=\"btn btn-primary\" href=\"/e/%s\">See them</a></p>",
		built->len, (1 == built->len) ? "" : "s", type_name);

	return venture_web_html_response(
		venture_web_page(self, request, NULL, "Import", content->str),
		200);
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
	const gchar *current_order;
	g_autofree gchar *path = NULL;
	gint64 total;
	gint64 page;
	gint64 page_size;
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

	/* ?page= is the human-facing spelling of offset. */
	{
		const gchar *page_param;

		page_param = htmx_request_get_query_param(request, "page");
		page = (NULL != page_param)
			? g_ascii_strtoll(page_param, NULL, 10) : 1;

		if (page < 1)
			page = 1;

		page_size = (gint64)venture_query_get_limit(query);

		if ((page > 1) && (page_size > 0))
			venture_query_set_offset(query,
				(guint)((page - 1) * page_size));
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

	/* The whole filtered set, not this page of it: "Page 2 of 9" and the
	 * subtitle's count both need the real number. */
	total = venture_database_count(venture_context_get_database(self->context),
	                               query, NULL);

	if (total < 0)
		total = (gint64)records->len;

	specs = venture_entity_get_field_specs(prototype);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	path = g_strdup_printf("/e/%s", type_name);
	content = g_string_new(NULL);

	g_string_append(content, "<div class=\"page-head\"><div class=\"page-title\">"
	                         "<h1>");
	{
		g_autofree gchar *heading = NULL;

		heading = venture_web_label_from_name(type_name);
		venture_html_escape_append(content, heading);
	}
	g_string_append(content, "</h1><span class=\"subtitle\">");
	g_string_append_printf(content, "%" G_GINT64_FORMAT " record%s", total,
	                       (1 == total) ? "" : "s");
	g_string_append(content, "</span></div><div class=\"page-actions\">");
	g_string_append_printf(content,
		"<input type=\"search\" name=\"search\" placeholder=\"Search\" "
		"data-search-input hx-get=\"%s\" hx-trigger=\"keyup changed delay:300ms\" "
		"hx-target=\"body\">", path);

	{
		g_autofree gchar *suffix = NULL;

		suffix = venture_web_list_query_string(request, NULL, 0);
		g_string_append_printf(content,
			"<a class=\"btn\" href=\"/e/%s/export%s\" "
			"title=\"Download this view as CSV\">Export</a>",
			type_name, suffix);
		g_string_append_printf(content,
			"<a class=\"btn\" href=\"/e/%s/import\" "
			"title=\"Create records from a CSV\">Import</a>",
			type_name);
	}

	g_string_append_printf(content,
		"<a class=\"btn btn-primary\" href=\"/e/%s/new\">New</a>", type_name);
	g_string_append(content, "</div></div>");

	g_string_append(content, "<div class=\"card\"><div class=\"table-wrap\">"
	                         "<table class=\"data\"><thead><tr>");

	shown = 0;
	current_order = htmx_request_get_query_param(request, "order");

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;
		const gchar *field_name;
		g_autofree gchar *descending = NULL;
		g_autofree gchar *suffix = NULL;
		gboolean sorted_asc;
		gboolean sorted_desc;

		spec = g_ptr_array_index(specs, i);

		/* A list with forty columns is unreadable; the first several
		 * declared fields are the ones that identify a record. */
		if (!venture_field_spec_get_show_in_list(spec) || (shown >= 7))
			continue;

		/*
		 * Each header is a link that sorts by its column, and clicking
		 * the column already sorted flips the direction. The state
		 * lives in the URL, so a sorted view survives reload and can
		 * be sent to somebody.
		 */
		field_name = venture_field_spec_get_name(spec);
		descending = g_strdup_printf("-%s", field_name);
		sorted_asc = (0 == g_strcmp0(current_order, field_name));
		sorted_desc = (0 == g_strcmp0(current_order, descending));

		suffix = venture_web_list_query_string(request,
			sorted_asc ? descending : field_name, 0);

		g_string_append_printf(content,
			"<th%s><a class=\"th-sort\" href=\"/e/%s%s\">",
			(sorted_asc || sorted_desc) ? " class=\"sorted\"" : "",
			type_name, suffix);
		venture_html_escape_append(content, venture_field_spec_get_label(spec));

		if (sorted_asc)
			g_string_append(content, " \xe2\x96\xb2");
		else if (sorted_desc)
			g_string_append(content, " \xe2\x96\xbc");

		g_string_append(content, "</a></th>");
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
			"\">Open</a></td>",
			type_name, venture_entity_get_id(record));

		g_string_append(content, "</tr>");
	}

	g_string_append(content, "</tbody></table></div>");

	if (0 == records->len)
	{
		g_string_append(content, "<div class=\"empty\">"
		                         "<span class=\"empty-icon\">"
						 VENTURE_ICON(
							"<path d=\"M3 13h4l2 3h6l2-3h4\"/>"
							"<path d=\"M5.5 6h13l2.5 7v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-5L5.5 6z\"/>"
						 )
						 "</span>"
		                         "<h3>Nothing here yet</h3>"
		                         "<p class=\"muted\">Records you add will "
		                         "appear in this list.</p></div>");
	}

	/* The pager appears only once there is somewhere to go. */
	if ((page_size > 0) && (total > page_size))
	{
		gint64 pages;

		pages = (total + page_size - 1) / page_size;

		g_string_append(content, "<div class=\"pager\">");

		if (page > 1)
		{
			g_autofree gchar *suffix = NULL;

			suffix = venture_web_list_query_string(request, NULL,
			                                       page - 1);
			g_string_append_printf(content,
				"<a class=\"btn btn-sm\" href=\"/e/%s%s\">"
				"\xe2\x80\xb9 Prev</a>",
				type_name,
				(NULL != suffix) ? suffix : "");
		}

		g_string_append_printf(content,
			"<span class=\"pager-state\">Page %" G_GINT64_FORMAT
			" of %" G_GINT64_FORMAT "</span>", page, pages);

		if (page < pages)
		{
			g_autofree gchar *suffix = NULL;

			suffix = venture_web_list_query_string(request, NULL,
			                                       page + 1);
			g_string_append_printf(content,
				"<a class=\"btn btn-sm\" href=\"/e/%s%s\">"
				"Next \xe2\x80\xba</a>",
				type_name, suffix);
		}

		g_string_append(content, "</div>");
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

/*
 * The sign-in shell, shared by the form and the failure page.
 *
 * Those were two copies of the same hand-styled markup, and they had already
 * drifted: the failure page dropped the lang attribute and the viewport meta,
 * so a failed login rendered zoomed out on a phone. Both also hard-coded
 * their layout in style attributes, which put a second, invisible set of
 * design decisions outside the stylesheet.
 *
 * The theme script is the same one the main shell emits, for the same
 * reason: without it a dark-theme operator gets a white page at sign-in and
 * a dark one immediately after.
 *
 * Returns: (transfer full): the complete document
 */
static gchar *
venture_web_auth_page(
	VentureWebServer	*self,
	const gchar		*body
){
	g_autoptr(GString) html = NULL;
	g_autofree gchar *ui_title = NULL;
	g_autofree gchar *accent = NULL;

	g_object_get(venture_context_get_config(self->context),
	             "ui-title", &ui_title,
	             "ui-accent", &accent,
	             NULL);

	html = g_string_new("<!doctype html><html lang=\"en\"><head>"
	                    "<meta charset=\"utf-8\">"
	                    "<meta name=\"viewport\" content=\"width=device-width, "
	                    "initial-scale=1\"><title>Sign in");
	g_string_append(html, " &middot; ");
	venture_html_escape_append(html, ui_title);
	g_string_append(html, "</title>");

	g_string_append(html,
		"<script>(function(){try{var t=localStorage.getItem('venture.theme');"
		"if(t==='light'||t==='dark')document.documentElement"
		".setAttribute('data-theme',t);}catch(e){}})();</script>");

	g_string_append(html, "<style>");
	g_string_append(html, venture_asset_venture_css);
	g_string_append(html, "</style>");

	g_string_append(html, "<style>:root{--accent-config:");
	venture_html_escape_append(html, accent);
	g_string_append(html, ";}</style>");

	g_string_append(html, "</head><body><main class=\"auth\">"
	                      "<div class=\"auth-inner\">");

	/* The masthead: the mark, the install's name, and what this page is
	 * for. The name is the operator's, so it is escaped. */
	g_string_append(html, "<div class=\"auth-mast\">"
	                      "<span class=\"brand-mark\">V</span>"
	                      "<h1>");
	venture_html_escape_append(html, ui_title);
	g_string_append(html, "</h1></div>");

	g_string_append(html, body);

	g_string_append(html, "</div></main></body></html>");

	return g_string_free(g_steal_pointer(&html), FALSE);
}

static HtmxResponse *
venture_web_ui_login_form(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autofree gchar *html = NULL;

	self = user_data;

	html = venture_web_auth_page(self,
		"<form class=\"auth-form\" method=\"post\" action=\"/login\">"
		"<div class=\"field\"><label for=\"u\">Username</label>"
		"<input id=\"u\" type=\"text\" name=\"username\" "
		"autocomplete=\"username\" autofocus>"
		"</div>"
		"<div class=\"field\"><label for=\"p\">Password</label>"
		"<input id=\"p\" name=\"password\" type=\"password\" "
		"autocomplete=\"current-password\"></div>"
		"<button class=\"btn btn-primary btn-lg\" type=\"submit\">"
		"Sign in</button></form>");

	return venture_web_html_response(g_steal_pointer(&html), 200);
}

static HtmxResponse *
venture_web_ui_login_submit(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *remote = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *response;

	self = user_data;
	remote = venture_auth_remote_address(request);

	if (!venture_auth_login(self->auth,
	                        htmx_request_get_form_value(request, "username"),
	                        htmx_request_get_form_value(request, "password"),
	                        remote, &cookie, &error))
	{
		g_autoptr(GString) body = NULL;
		g_autofree gchar *html = NULL;

		body = g_string_new("<div class=\"auth-form\">"
		                    "<div class=\"notice negative\">"
		                    "<span class=\"notice-icon\">"
		                    VENTURE_ICON(
		                        "<circle cx=\"12\" cy=\"12\" r=\"9\"/>"
		                        "<path d=\"M12 8v5\"/>"
		                        "<path d=\"M12 16.5h.01\"/>"
		                    )
		                    "</span><span>");
		venture_html_escape_append(body, error->message);
		g_string_append(body, "</span></div>"
		                      "<a class=\"btn btn-primary btn-lg\" "
		                      "href=\"/login\">Try again</a></div>");

		html = venture_web_auth_page(self, body->str);

		return venture_web_html_response(g_steal_pointer(&html), 401);
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

	/*
	 * The label text and the required marker are wrapped together so they
	 * share a line. Everything in a .field label is a flex child, so a
	 * bare text node beside a <span> stacked into two rows -- the asterisk
	 * ended up on a line of its own beneath the caption.
	 */
	g_string_append(content, "<div class=\"field\"><label>"
	                         "<span class=\"field-label\">");
	venture_html_escape_append(content, label);

	if (required)
		g_string_append(content, "<span class=\"required\">*</span>");

	g_string_append(content, "</span>");

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

	/*
	 * The blurb is suppressed when it only repeats the caption.
	 *
	 * Most field specs describe themselves -- "Title" is documented as
	 * "Title" -- so a generated form printed every caption twice, once
	 * above the control and once below it in a second style. Saying it
	 * twice does not make it clearer, and on a twenty-field record it
	 * doubles the height of the form for nothing.
	 */
	if (!venture_string_is_empty(help) &&
	    (0 != g_ascii_strcasecmp(help, label)))
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

	if (!venture_web_type_accepts_writes(entity_type, &error))
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

	if (!venture_web_type_accepts_writes(entity_type, &error))
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

/* --- Record detail ------------------------------------------------------- */

/*
 * Renders one field's value for reading rather than editing.
 */
static void
venture_web_append_detail_value(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record,
	VentureFieldSpec	*spec
){
	g_auto(GValue) value = G_VALUE_INIT;
	const gchar *name;

	name = venture_field_spec_get_name(spec);

	if (!venture_entity_get_field(record, name, &value))
	{
		g_string_append(content, "<span class=\"muted\">—</span>");
		return;
	}

	if (G_VALUE_HOLDS(&value, VENTURE_TYPE_MONEY))
	{
		const VentureMoney *money;
		g_autofree gchar *text = NULL;

		money = g_value_get_boxed(&value);
		text = (NULL != money)
			? venture_money_to_display_string(money, TRUE) : NULL;

		if (venture_string_is_empty(text))
			g_string_append(content, "<span class=\"muted\">—</span>");
		else
			venture_html_escape_append(content, text);
	}
	else if (G_VALUE_HOLDS(&value, G_TYPE_DATE_TIME))
	{
		GDateTime *when;

		when = g_value_get_boxed(&value);

		if (NULL == when)
		{
			g_string_append(content, "<span class=\"muted\">—</span>");
		}
		else
		{
			g_autofree gchar *text = NULL;

			text = g_date_time_format(when, "%Y-%m-%d");
			venture_html_escape_append(content, text);
		}
	}
	else if (G_VALUE_HOLDS_BOOLEAN(&value))
	{
		g_string_append(content, g_value_get_boolean(&value) ? "yes" : "no");
	}
	else if (G_VALUE_HOLDS_ENUM(&value))
	{
		venture_html_escape_append(content,
			venture_enum_to_nick(G_VALUE_TYPE(&value),
			                     g_value_get_enum(&value)));
	}
	else if (VENTURE_FIELD_KIND_REFERENCE == venture_field_spec_get_kind(spec))
	{
		gint64 id;

		id = G_VALUE_HOLDS_INT64(&value) ? g_value_get_int64(&value) : 0;

		if (0 == id)
		{
			g_string_append(content, "<span class=\"muted\">—</span>");
		}
		else
		{
			g_autoptr(VentureEntity) target = NULL;
			const gchar *type_name;
			GType target_type;

			type_name = venture_field_spec_get_reference_type(spec);
			target_type = venture_entity_registry_lookup(
				venture_context_get_entity_registry(self->context), type_name);

			if (G_TYPE_INVALID != target_type)
			{
				target = venture_database_get(
					venture_context_get_database(self->context), target_type,
					id, NULL);
			}

			/* A link, because the whole value of a reference is being
			 * able to follow it. */
			if (NULL != target)
			{
				g_autofree gchar *label = NULL;

				label = venture_entity_get_display_name(target);

				g_string_append_printf(content,
					"<a href=\"/e/%s/%" G_GINT64_FORMAT "\">", type_name, id);
				venture_html_escape_append(content, label);
				g_string_append(content, "</a>");
			}
			else
			{
				g_string_append_printf(content,
					"<span class=\"muted\">#%" G_GINT64_FORMAT " (missing)"
					"</span>", id);
			}
		}
	}
	else if (G_VALUE_HOLDS_STRING(&value))
	{
		const gchar *text;

		text = g_value_get_string(&value);

		if (venture_string_is_empty(text))
			g_string_append(content, "<span class=\"muted\">—</span>");
		else
			venture_html_escape_append(content, text);
	}
	else
	{
		g_autoptr(JsonNode) node = NULL;
		g_autofree gchar *text = NULL;

		node = venture_json_node_from_value(&value);
		text = venture_json_to_string(node, FALSE);

		if ((NULL == text) || (0 == g_strcmp0(text, "null")))
			g_string_append(content, "<span class=\"muted\">—</span>");
		else
			venture_html_escape_append(content, text);
	}
}

/*
 * Lists the records that point at this one.
 *
 * Derived from the reference metadata rather than written out per type, so
 * a company page grows a Tickets section the moment a ticket gains a company
 * field -- and a plugin's record type appears here for free. This is what
 * turns a set of tables into a CRM: the point of a contact is everything
 * attached to them.
 */
static void
venture_web_append_related(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autofree GType *types = NULL;
	const gchar *own_name;
	guint n_types;
	guint i;

	own_name = venture_entity_get_entity_name(record);
	types = venture_entity_registry_list_types(
		venture_context_get_entity_registry(self->context), &n_types);

	for (i = 0; i < n_types; i++)
	{
		/*
		 * Types with a panel of their own on this page are skipped, or
		 * the page shows the same relationships twice -- once
		 * usefully, and once as a list of "ticket_relation #1" links
		 * that say nothing.
		 */
		if ((VENTURE_TYPE_TICKET_RELATION == types[i]) ||
		    (VENTURE_TYPE_TICKET_LINK == types[i]))
			continue;

		g_autoptr(VentureEntity) prototype = NULL;
		g_autoptr(GPtrArray) specs = NULL;
		guint j;

		if (VENTURE_TYPE_AUDIT_ENTRY == types[i])
			continue;

		prototype = g_object_new(types[i], NULL);
		specs = venture_entity_get_field_specs(prototype);

		for (j = 0; j < specs->len; j++)
		{
			g_autoptr(VentureQuery) query = NULL;
			g_autoptr(GPtrArray) related = NULL;
			VentureFieldSpec *spec;
			const gchar *related_name;
			guint k;

			spec = g_ptr_array_index(specs, j);

			if (VENTURE_FIELD_KIND_REFERENCE !=
			    venture_field_spec_get_kind(spec))
				continue;

			if (0 != g_strcmp0(venture_field_spec_get_reference_type(spec),
			                   own_name))
				continue;

			query = venture_query_new(types[i]);
			venture_query_set_limit(query, 50);

			if (!venture_query_add_filter_int(query,
				venture_field_spec_get_name(spec), VENTURE_FILTER_OP_EQ,
				venture_entity_get_id(record), NULL))
				continue;

			related = venture_database_find(
				venture_context_get_database(self->context), query, NULL);

			if ((NULL == related) || (0 == related->len))
				continue;

			related_name = venture_entity_get_entity_name(prototype);

			g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
			                         "<h2>");
			venture_html_escape_append(content, related_name);

			/* Named by the field, because a type can point at the same
			 * target twice -- a ticket has both a parent ticket and
			 * child tickets. */
			g_string_append(content, " <span class=\"muted\">by ");
			venture_html_escape_append(content,
			                           venture_field_spec_get_label(spec));
			g_string_append_printf(content, "</span> <span class=\"count\">%u"
			                                "</span></h2><ul class=\"related\">",
			                       related->len);

			for (k = 0; k < related->len; k++)
			{
				g_autofree gchar *label = NULL;
				VentureEntity *item;

				item = g_ptr_array_index(related, k);
				label = venture_entity_get_display_name(item);

				g_string_append_printf(content,
					"<li><a href=\"/e/%s/%" G_GINT64_FORMAT "\">",
					related_name, venture_entity_get_id(item));
				venture_html_escape_append(content, label);
				g_string_append(content, "</a></li>");
			}

			g_string_append_printf(content,
				"</ul><a class=\"btn btn-sm\" href=\"/e/%s/new\">"
				"Add %s</a></div></div>", related_name, related_name);
		}
	}
}

/*
 * The lines of one invoice, in printed order.
 *
 * Returns: (transfer full) (nullable): the lines
 */
static GPtrArray *
venture_web_invoice_lines(
	VentureWebServer	 *self,
	gint64			  invoice_id,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);

	if (!venture_query_add_filter_int(query, "invoice-id",
	                                  VENTURE_FILTER_OP_EQ, invoice_id,
	                                  error))
		return NULL;

	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 500);

	return venture_database_find(venture_context_get_database(self->context),
	                             query, error);
}

/*
 * Sums the lines. Same-currency enforcement comes free from the money
 * layer: the first mixed-currency addition refuses, and the caller reports
 * that instead of a wrong total.
 *
 * Returns: (transfer full) (nullable): the total, or %NULL -- with @error
 *   set for a real failure, unset for an invoice with no priced lines
 */
static VentureMoney *
venture_web_invoice_total(
	GPtrArray	 *lines,
	GError		**error
){
	g_autoptr(VentureMoney) total = NULL;
	guint i;

	for (i = 0; (NULL != lines) && (i < lines->len); i++)
	{
		g_autoptr(VentureMoney) amount = NULL;

		amount = venture_invoice_line_get_amount(
			g_ptr_array_index(lines, i), NULL);

		if (NULL == amount)
			continue;

		if (NULL == total)
		{
			total = g_steal_pointer(&amount);
		}
		else
		{
			VentureMoney *sum;

			sum = venture_money_add(total, amount, error);

			if (NULL == sum)
				return NULL;

			g_clear_pointer(&total, venture_money_free);
			total = sum;
		}
	}

	return g_steal_pointer(&total);
}

/*
 * The invoice-specific block on the generic detail page: lines, total, and
 * the transitions the current status allows.
 */
/* Defined with the other forge handlers, further down; the detail page
 * needs it before then. */
static VentureTicketLink *
venture_web_forge_find_link(
	VentureWebServer	*self,
	gint64			 ticket_id,
	gint64			 repo_id
);

/*
 * The credential panel on a forge's page.
 *
 * A sensitive field is not rendered by the generated form and not applied by
 * the generated save, so without this panel a forge could be created and
 * never given a token. What is shown is whether each secret is set and when,
 * never a value -- the whole point of the flag.
 */
static void
venture_web_append_forge_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autofree gchar *token = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *bot = NULL;
	g_autoptr(GDateTime) token_at = NULL;
	g_autoptr(GDateTime) secret_at = NULL;
	gint64 id;

	id = venture_entity_get_id(record);

	g_object_get(record,
	             "token", &token,
	             "token-set-at", &token_at,
	             "webhook-secret", &secret,
	             "webhook-secret-set-at", &secret_at,
	             "bot-username", &bot,
	             NULL);

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Credentials</h2></div><div class=\"card-body\">");

	/*
	 * Until the bot account is known, VENTURE cannot tell an issue it
	 * filed itself from one somebody else opened -- that comparison is
	 * the webhook loop guard. Worth saying on the page rather than
	 * leaving as a subtly missing field.
	 */
	if (venture_string_is_empty(bot))
	{
		g_string_append(content,
			"<p class=\"notice\">Not verified yet. Until this forge's "
			"account is known, events caused by VENTURE itself cannot be "
			"told apart from anybody else's.</p>");
	}

	g_string_append_printf(content,
		"<form method=\"post\" action=\"/forges/%" G_GINT64_FORMAT
		"/token\"><label>Access token</label>"
		"<input type=\"password\" name=\"token\" autocomplete=\"off\" "
		"placeholder=\"%s\">"
		"<button class=\"btn btn-primary\" type=\"submit\">Set token</button>"
		"</form>", id,
		venture_string_is_empty(token) ? "not set"
		                              : "set -- leave blank to keep it");

	g_string_append_printf(content,
		"<form method=\"post\" action=\"/forges/%" G_GINT64_FORMAT
		"/secret\"><label>Webhook secret</label>"
		"<input type=\"password\" name=\"secret\" autocomplete=\"off\" "
		"placeholder=\"%s\">"
		"<button class=\"btn\" type=\"submit\">Set or generate</button>"
		"</form>", id,
		venture_string_is_empty(secret) ? "not set -- leave blank to generate"
		                                : "set -- leave blank to regenerate");

	g_string_append_printf(content,
		"<form method=\"post\" action=\"/forges/%" G_GINT64_FORMAT
		"/verify\"><button class=\"btn\" type=\"submit\">"
		"Verify the token</button></form>", id);

	/* The address to paste into the forge's webhook settings. Built from
	 * the configured base URL where there is one, and from what this
	 * server knows about itself otherwise. */
	g_string_append(content, "<p class=\"help\">Webhook URL: <code>");
	venture_html_escape_append(content,
		venture_string_is_empty(self->base_url) ? "" : self->base_url);
	g_string_append_printf(content, "/hooks/forge/%" G_GINT64_FORMAT
	                                "</code></p>", id);

	g_string_append(content, "</div></div>");
}

/*
 * The "Related to" panel on a ticket.
 *
 * A polymorphic subject gets none of what a declared reference gets, so the
 * link, the label and the picker are all written here. The picker is a type
 * select and an id box rather than a searchable record picker: a picker
 * would have to load every record of every type, which is the one thing the
 * reference picker gets away with only because it knows its one type.
 */
static void
venture_web_append_ticket_relations(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) relations = NULL;
	g_auto(GStrv) type_names = NULL;
	gint64 ticket_id;
	guint i;

	ticket_id = venture_entity_get_id(record);

	query = venture_query_new(VENTURE_TYPE_TICKET_RELATION);

	if (!venture_query_add_filter_int(query, "ticket-id",
	                                  VENTURE_FILTER_OP_EQ, ticket_id, NULL))
		return;

	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 50);

	relations = venture_database_find(venture_context_get_database(self->context),
	                                  query, NULL);

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Related to</h2></div><div class=\"card-body\">");

	if ((NULL == relations) || (0 == relations->len))
	{
		g_string_append(content, "<p class=\"muted\">Nothing yet.</p>");
	}
	else
	{
		g_string_append(content, "<ul class=\"relation-list\">");

		for (i = 0; i < relations->len; i++)
		{
			VentureEntity *relation = g_ptr_array_index(relations, i);
			g_autoptr(VentureEntity) subject = NULL;
			g_autofree gchar *subject_type = NULL;
			g_autofree gchar *label = NULL;
			g_autofree gchar *note = NULL;
			gint64 subject_id = 0;

			g_object_get(relation, "subject-type", &subject_type,
			             "subject-id", &subject_id,
			             "subject-label", &label, "note", &note, NULL);

			subject = venture_ticket_relation_resolve(
				venture_context_get_database(self->context),
				VENTURE_TICKET_RELATION(relation), NULL);

			/*
			 * venture_database_get() returns soft-deleted rows -- it
			 * has to, or nothing could ever restore one. Here that
			 * would render a deleted subject as an ordinary link, so
			 * the stamp is checked rather than the lookup.
			 */
			if ((NULL != subject) && venture_entity_is_deleted(subject))
				g_clear_object(&subject);

			g_string_append(content, "<li><span class=\"badge\">");
			venture_html_escape_append(content, subject_type);
			g_string_append(content, "</span> ");

			/*
			 * Linked only while the subject is still there. A deleted
			 * one keeps its label and loses its link, which is why
			 * the label is stored rather than looked up -- the ticket
			 * can still say what it was about.
			 */
			if (NULL != subject)
			{
				g_autofree gchar *fresh = NULL;

				fresh = venture_entity_get_display_name(subject);

				g_string_append_printf(content, "<a href=\"/e/%s/%"
				                                G_GINT64_FORMAT "\">",
				                       subject_type, subject_id);
				venture_html_escape_append(content,
					!venture_string_is_empty(fresh) ? fresh : label);
				g_string_append(content, "</a>");
			}
			else
			{
				venture_html_escape_append(content, label);
				g_string_append(content, " <span class=\"muted\">"
				                         "(deleted)</span>");
			}

			if (!venture_string_is_empty(note))
			{
				g_string_append(content, " <span class=\"muted\">— ");
				venture_html_escape_append(content, note);
				g_string_append(content, "</span>");
			}

			g_string_append_printf(content,
				" <form method=\"post\" action=\"/relations/%"
				G_GINT64_FORMAT "/delete\" class=\"inline\">"
				"<button class=\"btn btn-sm\" type=\"submit\">Remove</button>"
				"</form></li>",
				venture_entity_get_id(relation));
		}

		g_string_append(content, "</ul>");
	}

	/* The picker. Every registered type, so a plugin's type is offered
	 * the moment it is loaded. */
	type_names = venture_entity_registry_list_names(
		venture_context_get_entity_registry(self->context));

	g_string_append_printf(content,
		"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT
		"/relate\" class=\"relate-form\">", ticket_id);

	g_string_append(content, "<select name=\"subject_type\" required>");

	for (i = 0; NULL != type_names[i]; i++)
	{
		/* Offering the ticket's own relations or the audit log as a
		 * subject is noise: nobody links a ticket to an audit row. */
		if ((0 == g_strcmp0(type_names[i], "ticket_relation")) ||
		    (0 == g_strcmp0(type_names[i], "audit_entry")))
			continue;

		g_string_append(content, "<option value=\"");
		venture_html_escape_append(content, type_names[i]);
		g_string_append(content, "\">");
		venture_html_escape_append(content, type_names[i]);
		g_string_append(content, "</option>");
	}

	g_string_append(content,
		"</select> "
		"<input type=\"number\" name=\"subject_id\" placeholder=\"id\" "
		"min=\"1\" required> "
		"<input type=\"text\" name=\"note\" placeholder=\"why (optional)\"> "
		"<button class=\"btn\" type=\"submit\">Relate</button></form>");

	g_string_append(content, "</div></div>");
}

/*
 * The "Tickets about this" panel, on every other kind of record.
 *
 * venture_web_append_related() finds records pointing at this one by walking
 * declared references, and a polymorphic pair is invisible to it. Without
 * this the relation would only be visible from the ticket, which is half a
 * link -- the useful direction is usually the other one: standing on an
 * invoice and asking what is outstanding about it.
 */
static void
venture_web_append_subject_tickets(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(GPtrArray) relations = NULL;
	guint i;

	relations = venture_ticket_relation_find_for_subject(
		venture_context_get_database(self->context),
		venture_entity_get_entity_name(record),
		venture_entity_get_id(record), NULL);

	if ((NULL == relations) || (0 == relations->len))
		return;

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Tickets about this</h2></div>"
	                         "<div class=\"card-body\"><ul class=\"relation-list\">");

	for (i = 0; i < relations->len; i++)
	{
		VentureEntity *relation = g_ptr_array_index(relations, i);
		g_autoptr(VentureEntity) ticket = NULL;
		g_autofree gchar *note = NULL;
		gint64 ticket_id = 0;

		g_object_get(relation, "ticket-id", &ticket_id, "note", &note, NULL);

		ticket = venture_database_get(venture_context_get_database(self->context),
		                              VENTURE_TYPE_TICKET, ticket_id, NULL);

		if (NULL == ticket)
			continue;

		{
			g_autofree gchar *title = NULL;
			VentureTicketStatus status;

			g_object_get(ticket, "title", &title, "status", &status, NULL);

			g_string_append_printf(content,
				"<li><span class=\"badge\">%s</span> "
				"<a href=\"/e/ticket/%" G_GINT64_FORMAT "\">",
				venture_enum_to_nick(VENTURE_TYPE_TICKET_STATUS, status),
				ticket_id);
			venture_html_escape_append(content, title);
			g_string_append(content, "</a>");

			if (!venture_string_is_empty(note))
			{
				g_string_append(content, " <span class=\"muted\">— ");
				venture_html_escape_append(content, note);
				g_string_append(content, "</span>");
			}

			g_string_append(content, "</li>");
		}
	}

	g_string_append(content, "</ul></div></div>");
}

/*
 * The open-and-clone panel on a repository's page.
 *
 * Two things a person wants from a repository record and cannot get from the
 * fields themselves: the address to look at it, and the command to check it
 * out. Both are composed rather than stored, so neither can disagree with
 * the forge the repository belongs to.
 */
static void
venture_web_append_repo_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(VentureEntity) forge = NULL;
	g_autofree gchar *full_name = NULL;
	g_autofree gchar *repo_clone_url = NULL;
	g_autofree gchar *base_url = NULL;
	g_autofree gchar *clone_base = NULL;
	g_autofree gchar *web_url = NULL;
	g_autofree gchar *clone_url = NULL;
	g_autofree gchar *command = NULL;
	gint64 forge_id = 0;

	g_object_get(record, "name", &full_name, "forge-id", &forge_id,
	             "clone-url", &repo_clone_url, NULL);

	if (0 != forge_id)
	{
		forge = venture_database_get(venture_context_get_database(self->context),
		                             VENTURE_TYPE_FORGE, forge_id, NULL);
	}

	if (NULL != forge)
	{
		g_object_get(forge, "base-url", &base_url, "clone-base-url",
		             &clone_base, NULL);
	}

	web_url = venture_forge_web_url(base_url, full_name, NULL);
	clone_url = venture_forge_clone_url(repo_clone_url, clone_base, base_url,
	                                    full_name);

	if ((NULL == web_url) && (NULL == clone_url))
		return;

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Open</h2></div><div class=\"card-body\">");

	if (NULL != web_url)
	{
		g_string_append(content, "<p><a class=\"btn btn-primary\" href=\"");
		venture_html_escape_append(content, web_url);
		g_string_append(content, "\" target=\"_blank\" rel=\"noreferrer\">"
		                         "Open on the forge</a></p>");
	}

	if (NULL != clone_url)
	{
		command = g_strdup_printf("git clone %s", clone_url);

		g_string_append(content, "<p><button class=\"btn\" type=\"button\" "
		                         "data-copy=\"");
		venture_html_escape_append(content, command);
		g_string_append(content, "\">Copy the clone command</button></p>");

		/*
		 * Shown as well as copied. The clipboard API needs a secure
		 * context, and this server is routinely reached over plain
		 * http on a loopback or LAN address where it is unavailable --
		 * so the button may do nothing, and a person still needs the
		 * command.
		 */
		g_string_append(content, "<pre class=\"mono\">");
		venture_html_escape_append(content, command);
		g_string_append(content, "</pre>");
	}

	g_string_append(content, "</div></div>");
}

/*
 * The repository panel on a ticket's page.
 *
 * Only drawn when the ticket names a repository: a ticket about the books
 * has nothing to do with a branch, and an empty panel on every ticket would
 * be noise.
 */
static void
venture_web_append_ticket_forge_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(VentureEntity) repo = NULL;
	g_autoptr(VentureEntity) forge = NULL;
	g_autoptr(VentureTicketLink) link = NULL;
	g_autofree gchar *repo_name = NULL;
	g_autofree gchar *forge_base_url = NULL;
	g_autofree gchar *branch = NULL;
	g_autofree gchar *issue_url = NULL;
	gint64 ticket_id;
	gint64 repo_id = 0;
	gint64 forge_id = 0;
	gint64 issue_number = 0;

	g_object_get(record, "repo-id", &repo_id, NULL);

	if (0 == repo_id)
		return;

	ticket_id = venture_entity_get_id(record);

	repo = venture_database_get(venture_context_get_database(self->context),
	                            VENTURE_TYPE_FORGE_REPO, repo_id, NULL);

	if (NULL == repo)
		return;

	g_object_get(repo, "name", &repo_name, "forge-id", &forge_id, NULL);

	if (0 != forge_id)
	{
		forge = venture_database_get(venture_context_get_database(self->context),
		                             VENTURE_TYPE_FORGE, forge_id, NULL);
	}

	if (NULL != forge)
		g_object_get(forge, "base-url", &forge_base_url, NULL);

	link = venture_web_forge_find_link(self, ticket_id, repo_id);

	if (NULL != link)
	{
		g_object_get(link, "branch", &branch, "issue-number", &issue_number,
		             "issue-url", &issue_url, NULL);
	}

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Code</h2></div><div class=\"card-body\">");

	g_string_append(content, "<dl class=\"detail detail-grid\"><dt>Repository"
	                         "</dt><dd>");
	venture_html_escape_append(content, repo_name);
	g_string_append(content, "</dd><dt>Branch</dt><dd>");

	if (venture_string_is_empty(branch))
	{
		g_string_append(content, "<span class=\"muted\">none yet</span>");
	}
	else
	{
		g_autofree gchar *branch_url = NULL;
		g_autofree gchar *checkout = NULL;

		branch_url = venture_forge_web_url(forge_base_url, repo_name, branch);

		/*
		 * The branch links to the forge and copies as a checkout
		 * command, because those are the two things anybody does with
		 * a branch name: look at it, or switch to it. Composed from
		 * the base URL rather than the clone base -- the clone base is
		 * often an SSH host with no web server on it.
		 */
		if (NULL != branch_url)
		{
			g_string_append(content, "<a href=\"");
			venture_html_escape_append(content, branch_url);
			g_string_append(content, "\" target=\"_blank\" "
			                         "rel=\"noreferrer\"><code>");
			venture_html_escape_append(content, branch);
			g_string_append(content, "</code></a> ");
		}
		else
		{
			g_string_append(content, "<code>");
			venture_html_escape_append(content, branch);
			g_string_append(content, "</code> ");
		}

		checkout = g_strdup_printf("git switch %s", branch);

		g_string_append(content, "<button class=\"btn btn-sm\" "
		                         "type=\"button\" data-copy=\"");
		venture_html_escape_append(content, branch);
		g_string_append(content, "\">Copy name</button> ");

		g_string_append(content, "<button class=\"btn btn-sm\" "
		                         "type=\"button\" data-copy=\"");
		venture_html_escape_append(content, checkout);
		g_string_append(content, "\">Copy switch</button>");
	}

	g_string_append(content, "</dd><dt>Issue</dt><dd>");

	if (0 == issue_number)
	{
		g_string_append(content, "<span class=\"muted\">not filed</span>");
	}
	else if (!venture_string_is_empty(issue_url))
	{
		g_string_append(content, "<a href=\"");
		venture_html_escape_append(content, issue_url);
		g_string_append_printf(content, "\">#%" G_GINT64_FORMAT "</a>",
		                       issue_number);
	}
	else
	{
		g_string_append_printf(content, "#%" G_GINT64_FORMAT, issue_number);
	}

	g_string_append(content, "</dd></dl>");

	g_string_append_printf(content,
		"<div class=\"page-actions\">"
		"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT
		"/link\"><button class=\"btn\" type=\"submit\">File upstream</button>"
		"</form> "
		"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT
		"/branch\"><button class=\"btn\" type=\"submit\">Create the branch"
		"</button></form> ", ticket_id, ticket_id);

	/* Only offered when runs are configured. A button that always
	 * answers "turned off in settings" teaches people to ignore it. */
	if (NULL != venture_context_get_work_service(self->context))
	{
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT
			"/work\"><button class=\"btn btn-primary\" type=\"submit\">"
			"Run the agent</button></form>", ticket_id);
	}

	g_string_append(content, "</div>");

	/*
	 * The run list, loaded once and then refreshed by the fragment
	 * itself. Loading it here rather than inline keeps one renderer for
	 * both the first paint and every poll.
	 */
	g_string_append_printf(content,
		"<div hx-get=\"/tickets/%" G_GINT64_FORMAT "/runs\" "
		"hx-trigger=\"load\" hx-swap=\"outerHTML\"></div>", ticket_id);

	g_string_append(content, "</div></div>");
}

static void
venture_web_append_invoice_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GError) error = NULL;
	VentureInvoiceStatus status;
	gint64 id;
	guint i;

	id = venture_entity_get_id(record);
	g_object_get(record, "status", &status, NULL);
	lines = venture_web_invoice_lines(self, id, &error);

	g_string_append(content, "<div class=\"card invoice-block\">"
	                         "<div class=\"card-head\"><h2>Lines</h2>");
	g_string_append_printf(content,
		"<a class=\"btn btn-sm\" href=\"/e/invoice_line/new\">Add line"
		"</a></div>");

	g_string_append(content, "<div class=\"table-wrap\">"
	                         "<table class=\"data\"><thead><tr>"
	                         "<th>Description</th><th class=\"num\">Qty</th>"
	                         "<th class=\"num\">Unit</th>"
	                         "<th class=\"num\">Amount</th></tr></thead>"
	                         "<tbody>");

	for (i = 0; (NULL != lines) && (i < lines->len); i++)
	{
		VentureInvoiceLine *line;
		g_autofree gchar *description = NULL;
		g_autoptr(VentureMoney) unit_price = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		gdouble quantity;

		line = g_ptr_array_index(lines, i);
		g_object_get(line, "description", &description,
		             "quantity", &quantity,
		             "unit-price", &unit_price, NULL);
		amount = venture_invoice_line_get_amount(line, NULL);

		g_string_append(content, "<tr><td>");
		venture_html_escape_append(content, description);
		g_string_append_printf(content,
			"</td><td class=\"num\">%g</td><td class=\"num\">",
			quantity);

		if (NULL != unit_price)
		{
			g_autofree gchar *text = NULL;

			text = venture_money_to_display_string(unit_price, TRUE);
			venture_html_escape_append(content, text);
		}

		g_string_append(content, "</td><td class=\"num\">");

		if (NULL != amount)
		{
			g_autofree gchar *text = NULL;

			text = venture_money_to_display_string(amount, TRUE);
			venture_html_escape_append(content, text);
		}

		g_string_append(content, "</td></tr>");
	}

	g_string_append(content, "</tbody><tfoot><tr>"
	                         "<td colspan=\"3\" class=\"num\">"
	                         "<strong>Total</strong></td>"
	                         "<td class=\"num\"><strong>");

	{
		g_autoptr(VentureMoney) total = NULL;
		g_autoptr(GError) sum_error = NULL;

		total = venture_web_invoice_total(lines, &sum_error);

		if (NULL != total)
		{
			g_autofree gchar *text = NULL;

			text = venture_money_to_display_string(total, TRUE);
			venture_html_escape_append(content, text);
		}
		else if (NULL != sum_error)
		{
			/* Mixed currencies: any single figure is wrong. */
			g_string_append(content, "\xe2\x80\x94");
		}
		else
		{
			g_string_append(content, "0");
		}
	}

	g_string_append(content, "</strong></td></tr></tfoot></table></div>");

	/* The transitions this status allows, each a form so nothing here
	 * depends on scripting. */
	g_string_append(content, "<div class=\"card-body invoice-actions\">");

	g_string_append_printf(content,
		"<a class=\"btn\" href=\"/invoices/%" G_GINT64_FORMAT
		"/print\" target=\"_blank\">Print</a>", id);

	if (VENTURE_INVOICE_STATUS_DRAFT == status)
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/invoices/%" G_GINT64_FORMAT
			"/status\"><input type=\"hidden\" name=\"to\" value=\"sent\">"
			"<button class=\"btn btn-primary\" type=\"submit\">"
			"Mark sent</button></form>", id);

	if (VENTURE_INVOICE_STATUS_SENT == status)
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/invoices/%" G_GINT64_FORMAT
			"/status\"><input type=\"hidden\" name=\"to\" value=\"paid\">"
			"<button class=\"btn btn-primary\" type=\"submit\" "
			"title=\"Stamps payment and records the revenue as a sale\">"
			"Mark paid</button></form>", id);

	if ((VENTURE_INVOICE_STATUS_DRAFT == status) ||
	    (VENTURE_INVOICE_STATUS_SENT == status))
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/invoices/%" G_GINT64_FORMAT
			"/status\"><input type=\"hidden\" name=\"to\" value=\"void\">"
			"<button class=\"btn\" type=\"submit\">Void</button></form>",
			id);

	g_string_append(content, "</div></div>");
}

static HtmxResponse *
venture_web_ui_detail(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GString) content = NULL;
	g_autofree gchar *title = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	GType entity_type;
	const gchar *type_name;
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
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	type_name = g_hash_table_lookup(params, "type");
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);

	record = venture_database_get(venture_context_get_database(self->context),
	                              entity_type, id, &error);

	if (NULL == record)
		return venture_web_error_response(error);

	specs = venture_entity_get_field_specs(record);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	title = venture_entity_get_display_name(record);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>");
	venture_html_escape_append(content, title);
	g_string_append(content, "</h1><span class=\"subtitle\">");
	venture_html_escape_append(content, type_name);
	g_string_append(content, "</span></div><div class=\"page-actions\">");
	g_string_append_printf(content,
		"<a class=\"btn btn-primary\" href=\"/e/%s/%" G_GINT64_FORMAT
		"/edit\">Edit</a> <a class=\"btn\" href=\"/e/%s\">All %s</a>",
		type_name, id, type_name, type_name);
	g_string_append(content, "</div></div>");

	g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
	                         "<dl class=\"detail detail-grid\">");

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		/* A sensitive value is not shown for reading either. */
		if (0 != (venture_field_spec_get_flags(spec) &
		          VENTURE_COLUMN_FLAG_SENSITIVE))
			continue;

		g_string_append(content, "<dt>");
		venture_html_escape_append(content, venture_field_spec_get_label(spec));
		g_string_append(content, "</dt><dd>");
		venture_web_append_detail_value(self, content, record, spec);
		g_string_append(content, "</dd>");
	}

	g_string_append(content, "</dl></div></div>");

	venture_web_append_related(self, content, record);

	/* An invoice's lines and total, with the actions its status allows.
	 * The other type-specific block, for the same reason as the ticket
	 * composer below: an invoice without its total is a list of hints. */
	if (VENTURE_TYPE_INVOICE == entity_type)
		venture_web_append_invoice_block(self, content, record);

	/* A forge's credentials, which the generated form cannot show. */
	if (VENTURE_TYPE_FORGE == entity_type)
		venture_web_append_forge_block(self, content, record);

	/* A repository's address and its clone command, both composed from
	 * the forge rather than stored, so neither can drift from it. */
	if (VENTURE_TYPE_FORGE_REPO == entity_type)
		venture_web_append_repo_block(self, content, record);

	/*
	 * The other half of a polymorphic relation. On a ticket the panel
	 * above already lists them, and the audit log is deliberately not
	 * relatable, so both are skipped here.
	 */
	if ((VENTURE_TYPE_TICKET != entity_type) &&
	    (VENTURE_TYPE_TICKET_RELATION != entity_type))
		venture_web_append_subject_tickets(self, content, record);

	/*
	 * A ticket's comments are a conversation, and a conversation needs
	 * its reply box on the same page. The related-records section above
	 * already lists the thread; this is the one place the generic detail
	 * page knows a specific type, because "click New, pick the ticket
	 * you were just looking at, type, save" is not how anybody comments.
	 */
	if (VENTURE_TYPE_TICKET == entity_type)
		venture_web_append_ticket_forge_block(self, content, record);

	/* What else this ticket is about, for the many tickets that are
	 * about something other than code. */
	if (VENTURE_TYPE_TICKET == entity_type)
		venture_web_append_ticket_relations(self, content, record);

	if (VENTURE_TYPE_TICKET == entity_type)
	{
		g_string_append(content,
			"<div class=\"card comment-composer\">"
			"<div class=\"card-head\"><h2>Add a comment</h2></div>"
			"<div class=\"card-body\">");
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT
			"/comment\">", id);
		g_string_append(content,
			"<textarea name=\"body\" rows=\"3\" required "
			"placeholder=\"What happened?\"></textarea>"
			"<div class=\"comment-actions\">"
			"<label class=\"checkbox\">"
			"<input type=\"checkbox\" name=\"internal\" value=\"1\"> "
			"Internal note (not shown to whoever raised it)</label>"
			"<button class=\"btn btn-primary\" type=\"submit\">"
			"Comment</button></div></form></div></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, NULL, title, content->str), 200);
}

/*
 * POST /tickets/:id/comment - the composer above.
 *
 * The author comes from the session rather than a form field, because "who
 * said this" is an audit fact, not an input.
 */
static HtmxResponse *
venture_web_ui_ticket_comment(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureTicketComment) comment = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	const gchar *body;
	const gchar *internal;
	gint64 ticket_id;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);

	/* The ticket must exist; a comment on a deleted ticket is a write
	 * nobody can ever read. */
	ticket = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_TICKET, ticket_id, &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	body = htmx_request_get_form_value(request, "body");

	if (venture_string_is_empty(body))
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A comment needs a body");
		return venture_web_error_response(error);
	}

	internal = htmx_request_get_form_value(request, "internal");
	now = venture_time_now();
	venture_auth_to_actor(principal, &actor);

	comment = venture_ticket_comment_new();
	g_object_set(comment,
	             "ticket-id", ticket_id,
	             "body", body,
	             "author", principal->name,
	             "internal", !venture_string_is_empty(internal),
	             "occurred-at", now,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(comment),
		venture_entity_get_organization_id(ticket));

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(comment), &actor, &error))
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	return venture_web_redirect_to(destination);
}

/* --- Tickets ------------------------------------------------------------- */

/*
 * The board's columns, in the order work moves through them.
 *
 * Cancelled is deliberately absent: it is an outcome rather than a stage,
 * and a column nobody drags into is a column that only takes up room. A
 * cancelled ticket is still reachable from the list view.
 */
static const VentureTicketStatus venture_web_board_columns[] = {
	VENTURE_TICKET_STATUS_TRIAGE,
	VENTURE_TICKET_STATUS_TODO,
	VENTURE_TICKET_STATUS_IN_PROGRESS,
	VENTURE_TICKET_STATUS_BLOCKED,
	VENTURE_TICKET_STATUS_REVIEW,
	VENTURE_TICKET_STATUS_DONE
};

/*
 * Builds the query behind both ticket views, applying whichever filters the
 * URL carries so that the board and the list always agree about what they
 * are showing.
 */
static VentureQuery *
venture_web_ticket_query(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	const gchar *kind;
	const gchar *issue_type;
	const gchar *assignee;
	const gchar *venture;

	query = venture_query_new(VENTURE_TYPE_TICKET);
	venture_query_set_limit(query, 0);
	venture_web_scope_to_active_organization(self, request, query);

	kind = htmx_request_get_query_param(request, "kind");

	if (!venture_string_is_empty(kind) && (0 != g_strcmp0(kind, "all")))
	{
		if (!venture_query_add_filter_string(query, "kind",
		                                     VENTURE_FILTER_OP_EQ, kind,
		                                     error))
			return NULL;
	}

	assignee = htmx_request_get_query_param(request, "assignee");

	if (!venture_string_is_empty(assignee))
	{
		if (!venture_query_add_filter_string(query, "assignee",
		                                     VENTURE_FILTER_OP_EQ, assignee,
		                                     error))
			return NULL;
	}

	/*
	 * The board's filters are applied here rather than left to
	 * venture_query_apply_query_string(), which is what the generic list
	 * pages use. Both would work -- issue_type is a real column -- but
	 * the board already builds its own query for kind and assignee, and
	 * having two of its four filters arrive by different routes is how
	 * one of them ends up quietly not applying.
	 */
	issue_type = htmx_request_get_query_param(request, "issue_type");

	if (!venture_string_is_empty(issue_type) &&
	    (0 != g_strcmp0(issue_type, "all")))
	{
		if (!venture_query_add_filter_string(query, "issue-type",
		                                     VENTURE_FILTER_OP_EQ,
		                                     issue_type, error))
			return NULL;
	}

	venture = htmx_request_get_query_param(request, "venture_id");

	if (!venture_string_is_empty(venture))
	{
		if (!venture_query_add_filter_int(query, "venture-id",
		                                  VENTURE_FILTER_OP_EQ,
		                                  g_ascii_strtoll(venture, NULL, 10),
		                                  error))
			return NULL;
	}

	return g_steal_pointer(&query);
}

/*
 * Renders one card. Everything on it answers a question you would otherwise
 * open the ticket to ask: who has it, when it is due, who asked.
 */
static void
venture_web_append_ticket_card(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*ticket
){
	g_autofree gchar *title = NULL;
	g_autofree gchar *assignee = NULL;
	g_autofree gchar *tags = NULL;
	g_autoptr(GDateTime) due = NULL;
	VentureTicketKind kind;
	VenturePriority priority;
	gint64 id;

	id = venture_entity_get_id(ticket);

	g_object_get(ticket, "title", &title, "assignee", &assignee,
	             "kind", &kind, "priority", &priority, "due-at", &due,
	             "tags", &tags, NULL);

	g_string_append_printf(content,
		"<article class=\"card ticket-card priority-%s\" draggable=\"true\" "
		"data-ticket=\"%" G_GINT64_FORMAT "\">",
		venture_enum_to_nick(VENTURE_TYPE_PRIORITY, (gint)priority), id);

	g_string_append_printf(content,
		"<a class=\"ticket-title\" href=\"/e/ticket/%" G_GINT64_FORMAT "\">",
		id);
	venture_html_escape_append(content, title);
	g_string_append(content, "</a>");

	g_string_append(content, "<div class=\"ticket-meta\">");

	g_string_append_printf(content, "<span class=\"badge kind-%s\">%s</span>",
		venture_enum_to_nick(VENTURE_TYPE_TICKET_KIND, (gint)kind),
		(VENTURE_TICKET_KIND_EXTERNAL == kind) ? "support" : "internal");

	if (!venture_string_is_empty(assignee))
	{
		g_string_append(content, "<span class=\"ticket-assignee\">");
		venture_html_escape_append(content, assignee);
		g_string_append(content, "</span>");
	}

	if (NULL != due)
	{
		g_autoptr(GDateTime) now = NULL;
		g_autofree gchar *when = NULL;
		gboolean overdue;

		now = venture_time_now();
		overdue = (g_date_time_compare(due, now) < 0);
		when = g_date_time_format(due, "%d %b");

		/* Overdue is called out rather than left to be worked out from a
		 * date, because the whole point of a due date is noticing. */
		g_string_append_printf(content, "<span class=\"ticket-due%s\">",
		                       overdue ? " overdue" : "");
		venture_html_escape_append(content, when);
		g_string_append(content, "</span>");
	}

	g_string_append(content, "</div>");

	if (!venture_string_is_empty(tags))
	{
		g_auto(GStrv) parts = NULL;
		gsize i;

		parts = g_strsplit(tags, ",", -1);

		g_string_append(content, "<div class=\"ticket-tags\">");

		for (i = 0; NULL != parts[i]; i++)
		{
			g_autofree gchar *tag = NULL;

			tag = g_strstrip(g_strdup(parts[i]));

			if (venture_string_is_empty(tag))
				continue;

			g_string_append(content, "<span class=\"tag\">");
			venture_html_escape_append(content, tag);
			g_string_append(content, "</span>");
		}

		g_string_append(content, "</div>");
	}

	/*
	 * A form per card, so the board works with scripting off. The drag
	 * handler posts the same endpoint; this is the fallback, not a
	 * duplicate implementation.
	 */
	g_string_append_printf(content,
		"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT "/move\" "
		"class=\"ticket-move\"><select name=\"status\">", id);

	{
		gsize i;
		VentureTicketStatus current;

		g_object_get(ticket, "status", &current, NULL);

		for (i = 0; i < G_N_ELEMENTS(venture_web_board_columns); i++)
		{
			const gchar *nick;

			nick = venture_enum_to_nick(VENTURE_TYPE_TICKET_STATUS,
			                            (gint)venture_web_board_columns[i]);

			g_string_append_printf(content, "<option value=\"%s\"%s>", nick,
				(venture_web_board_columns[i] == current) ? " selected" : "");
			venture_html_escape_append(content, nick);
			g_string_append(content, "</option>");
		}
	}

	g_string_append(content, "</select>"
	                         "<button class=\"btn btn-sm\" type=\"submit\">"
	                         "Move</button></form>");

	g_string_append(content, "</article>");
}

/*
 * Builds a board URL carrying every filter the page offers.
 *
 * A helper rather than three format strings, because each control has to
 * preserve the others: picking an issue type must not silently drop the
 * kind you already chose, and switching from board to list must keep both.
 * Written out at each link, that is three places to remember a fourth
 * filter -- and the one that gets forgotten fails silently, by showing more
 * tickets than you asked for rather than by erroring.
 */
static gchar *
venture_web_ticket_url(
	const gchar	*kind,
	const gchar	*issue_type,
	gboolean	 board
){
	g_autoptr(GString) url = NULL;

	url = g_string_new("/tickets?view=");
	g_string_append(url, board ? "board" : "list");

	g_string_append(url, "&kind=");
	g_string_append_uri_escaped(url,
		venture_string_is_empty(kind) ? "all" : kind, NULL, FALSE);

	g_string_append(url, "&issue_type=");
	g_string_append_uri_escaped(url,
		venture_string_is_empty(issue_type) ? "all" : issue_type, NULL,
		FALSE);

	return g_string_free(g_steal_pointer(&url), FALSE);
}

static HtmxResponse *
venture_web_ui_tickets(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	const gchar *view;
	const gchar *kind;
	const gchar *issue_type;
	gboolean board;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	query = venture_web_ticket_query(self, request, &error);

	if (NULL == query)
		return venture_web_error_response(error);

	/* Sorted by board position, so the columns render in the order cards
	 * were left in rather than by identifier. */
	venture_query_add_order(query, "board-order", VENTURE_SORT_ASCENDING, NULL);

	tickets = venture_database_find(venture_context_get_database(self->context),
	                                query, &error);

	if (NULL == tickets)
		return venture_web_error_response(error);

	view = htmx_request_get_query_param(request, "view");
	kind = htmx_request_get_query_param(request, "kind");
	issue_type = htmx_request_get_query_param(request, "issue_type");
	board = (0 != g_strcmp0(view, "list"));

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Tickets</h1><span class=\"subtitle\">");
	g_string_append_printf(content, "%u open item%s", tickets->len,
	                       (1 == tickets->len) ? "" : "s");
	g_string_append(content, "</span></div><div class=\"page-actions\">");

	/* The filters, as links rather than a form: each is a URL you can
	 * bookmark, which is what people actually want from a saved view. */
	{
		static const struct
		{
			const gchar *value;
			const gchar *label;
		} kinds[] = {
			{ "all",      "All" },
			{ "internal", "Internal" },
			{ "external", "Support" }
		};
		gsize i;

		g_string_append(content, "<div class=\"segmented\">");

		for (i = 0; i < G_N_ELEMENTS(kinds); i++)
		{
			gboolean active;

			active = (0 == g_strcmp0(kinds[i].value, "all"))
				? (venture_string_is_empty(kind) ||
				   (0 == g_strcmp0(kind, "all")))
				: (0 == g_strcmp0(kind, kinds[i].value));

			{
				g_autofree gchar *url = NULL;

				url = venture_web_ticket_url(kinds[i].value, issue_type,
				                             board);

				g_string_append_printf(content,
					"<a class=\"seg%s\" href=\"%s\">%s</a>",
					active ? " active" : "", url, kinds[i].label);
			}
		}

		g_string_append(content, "</div>");
	}

	/*
	 * The issue type: what shape of work, as distinct from the kind
	 * above, which is whose problem it is. Both filter at once, so
	 * "external bugs" is expressible -- which is the pair most worth
	 * looking at.
	 */
	{
		g_autoptr(GEnumClass) types = NULL;
		g_autofree gchar *all_url = NULL;
		guint i;

		types = g_type_class_ref(VENTURE_TYPE_ISSUE_TYPE);
		all_url = venture_web_ticket_url(kind, "all", board);

		g_string_append(content, "<div class=\"segmented\">");
		g_string_append_printf(content,
			"<a class=\"seg%s\" href=\"%s\">Any type</a>",
			(venture_string_is_empty(issue_type) ||
			 (0 == g_strcmp0(issue_type, "all"))) ? " active" : "",
			all_url);

		for (i = 0; i < types->n_values; i++)
		{
			g_autofree gchar *url = NULL;
			const gchar *nick;

			nick = types->values[i].value_nick;
			url = venture_web_ticket_url(kind, nick, board);

			g_string_append_printf(content,
				"<a class=\"seg%s\" href=\"%s\">",
				(0 == g_strcmp0(issue_type, nick)) ? " active" : "", url);
			venture_html_escape_append(content, nick);
			g_string_append(content, "</a>");
		}

		g_string_append(content, "</div>");
	}

	{
		g_autofree gchar *board_url = NULL;
		g_autofree gchar *list_url = NULL;

		board_url = venture_web_ticket_url(kind, issue_type, TRUE);
		list_url = venture_web_ticket_url(kind, issue_type, FALSE);

		g_string_append_printf(content,
			"<div class=\"segmented\">"
			"<a class=\"seg%s\" href=\"%s\">Board</a>"
			"<a class=\"seg%s\" href=\"%s\">List</a>"
			"</div>",
			board ? " active" : "", board_url,
			board ? "" : " active", list_url);
	}

	g_string_append(content,
		"<a class=\"btn btn-primary\" href=\"/e/ticket/new\">New ticket</a>");
	g_string_append(content, "</div></div>");

	if (board)
	{
		gsize column;

		g_string_append(content, "<div class=\"board\" data-board>");

		for (column = 0; column < G_N_ELEMENTS(venture_web_board_columns); column++)
		{
			const gchar *nick;
			guint count;
			guint i;

			nick = venture_enum_to_nick(VENTURE_TYPE_TICKET_STATUS,
			                            (gint)venture_web_board_columns[column]);
			count = 0;

			for (i = 0; i < tickets->len; i++)
			{
				VentureTicketStatus status;

				g_object_get(g_ptr_array_index(tickets, i), "status", &status,
				             NULL);

				if (status == venture_web_board_columns[column])
					count++;
			}

			g_string_append_printf(content,
				"<section class=\"board-column\" data-status=\"%s\">"
				"<header class=\"board-column-head\"><span>", nick);
			{
				g_autofree gchar *heading = NULL;

				heading = venture_web_label_from_name(nick);
				venture_html_escape_append(content, heading);
			}
			g_string_append_printf(content,
				"</span><span class=\"count\">%u</span></header>"
				"<div class=\"board-column-body\" data-drop=\"%s\">",
				count, nick);

			for (i = 0; i < tickets->len; i++)
			{
				VentureEntity *ticket;
				VentureTicketStatus status;

				ticket = g_ptr_array_index(tickets, i);
				g_object_get(ticket, "status", &status, NULL);

				if (status != venture_web_board_columns[column])
					continue;

				venture_web_append_ticket_card(self, content, ticket);
			}

			g_string_append(content, "</div></section>");
		}

		g_string_append(content, "</div>");
	}
	else
	{
		guint i;

		g_string_append(content, "<div class=\"card\"><div class=\"table-wrap\">"
		                         "<table class=\"data\"><thead><tr>"
		                         "<th>Title</th><th>Kind</th><th>Status</th>"
		                         "<th>Priority</th><th>Assignee</th>"
		                         "<th>Due</th>"
		                         "<th class=\"row-actions\"></th>"
		                         "</tr></thead><tbody>");

		for (i = 0; i < tickets->len; i++)
		{
			g_autofree gchar *title = NULL;
			g_autofree gchar *assignee = NULL;
			g_autoptr(GDateTime) due = NULL;
			VentureEntity *ticket;
			VentureTicketKind ticket_kind;
			VentureTicketStatus status;
			VenturePriority priority;

			ticket = g_ptr_array_index(tickets, i);

			g_object_get(ticket, "title", &title, "assignee", &assignee,
			             "kind", &ticket_kind, "status", &status,
			             "priority", &priority, "due-at", &due, NULL);

			g_string_append_printf(content,
				"<tr><td><a href=\"/e/ticket/%" G_GINT64_FORMAT "\">",
				venture_entity_get_id(ticket));
			venture_html_escape_append(content, title);
			g_string_append(content, "</a></td><td>");
			venture_html_escape_append(content,
				venture_enum_to_nick(VENTURE_TYPE_TICKET_KIND,
				                     (gint)ticket_kind));
			g_string_append(content, "</td><td>");
			venture_html_escape_append(content,
				venture_enum_to_nick(VENTURE_TYPE_TICKET_STATUS,
				                     (gint)status));
			g_string_append(content, "</td><td>");
			venture_html_escape_append(content,
				venture_enum_to_nick(VENTURE_TYPE_PRIORITY, (gint)priority));
			g_string_append(content, "</td><td>");
			venture_html_escape_append(content, assignee);
			g_string_append(content, "</td><td>");

			if (NULL != due)
			{
				g_autofree gchar *when = NULL;

				when = g_date_time_format(due, "%Y-%m-%d");
				venture_html_escape_append(content, when);
			}

			g_string_append_printf(content,
				"</td><td class=\"row-actions\">"
				"<a class=\"btn btn-sm\" href=\"/e/ticket/%" G_GINT64_FORMAT
				"/edit\">Edit</a></td></tr>",
				venture_entity_get_id(ticket));
		}

		g_string_append(content, "</tbody></table></div></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/tickets", "Tickets", content->str),
		200);
}

/*
 * Moves a ticket to another column, and optionally to a position within it.
 *
 * Shared by the drag handler and the per-card fallback form, so the two
 * cannot drift apart. Answers JSON to a scripted caller and a redirect to a
 * browser that posted a form.
 */
static HtmxResponse *
venture_web_ui_ticket_move(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	const gchar *status_nick;
	const gchar *after;
	gint status_value;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ticket = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_TICKET,
	                              g_ascii_strtoll(g_hash_table_lookup(params,
	                                                                  "id"),
	                                              NULL, 10),
	                              &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	status_nick = htmx_request_get_form_value(request, "status");

	if (!venture_enum_from_nick(VENTURE_TYPE_TICKET_STATUS, status_nick,
	                            &status_value))
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "\"%s\" is not a ticket status", status_nick);
		return venture_web_error_response(error);
	}

	g_object_set(ticket, "status", (VentureTicketStatus)status_value, NULL);

	/*
	 * Reaching a terminal state stamps the resolution time, so "how long
	 * did that take" is answerable without reading the audit log. Moving
	 * back out of one clears it again -- a ticket that was reopened was
	 * not resolved.
	 */
	if ((VENTURE_TICKET_STATUS_DONE == (VentureTicketStatus)status_value) ||
	    (VENTURE_TICKET_STATUS_CANCELLED == (VentureTicketStatus)status_value))
	{
		g_autoptr(GDateTime) resolved = NULL;

		g_object_get(ticket, "resolved-at", &resolved, NULL);

		if (NULL == resolved)
		{
			g_autoptr(GDateTime) now = NULL;

			now = venture_time_now();
			g_object_set(ticket, "resolved-at", now, NULL);
		}
	}
	else
	{
		g_object_set(ticket, "resolved-at", NULL, NULL);
	}

	/*
	 * Position: dropped after a given card, so it lands halfway between
	 * that card and the next. Sparse doubles mean a move rewrites one row
	 * rather than renumbering the column.
	 */
	after = htmx_request_get_form_value(request, "after");

	if (!venture_string_is_empty(after))
	{
		g_autoptr(VentureEntity) neighbour = NULL;

		neighbour = venture_database_get(
			venture_context_get_database(self->context), VENTURE_TYPE_TICKET,
			g_ascii_strtoll(after, NULL, 10), NULL);

		if (NULL != neighbour)
		{
			gdouble position;

			g_object_get(neighbour, "board-order", &position, NULL);
			g_object_set(ticket, "board-order", position + 1.0, NULL);
		}
	}
	else if (NULL != htmx_request_get_form_value(request, "first"))
	{
		g_object_set(ticket, "board-order", -1.0, NULL);
	}

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           ticket, &actor, &error))
		return venture_web_error_response(error);

	/* A scripted move wants an answer, not a page. */
	if (NULL != htmx_request_get_form_value(request, "async"))
	{
		g_autoptr(JsonBuilder) builder = NULL;

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "status");
		json_builder_add_string_value(builder, status_nick);
		json_builder_end_object(builder);

		return venture_web_json_response(json_builder_get_root(builder), 200);
	}

	return venture_web_redirect_to("/tickets");
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

	notice = htmx_request_get_query_param(request, "notice");

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

/*
 * Files a freshly minted secret away and returns the handle that will fetch
 * it back. Expired entries are dropped on the way past, which is enough
 * housekeeping for a table that only ever holds the tokens one operator
 * minted in the last five minutes.
 */
static gchar *
venture_web_reveal_store(
	VentureWebServer	*self,
	const gchar		*secret,
	gint64			 token_id
){
	g_autoptr(GDateTime) now = NULL;
	VentureWebReveal *reveal;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gchar *handle;

	now = venture_time_now();

	g_hash_table_iter_init(&iter, self->reveals);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		VentureWebReveal *stale = value;

		if (g_date_time_difference(now, stale->created_at) >
		    (VENTURE_WEB_REVEAL_TTL_SECONDS * G_TIME_SPAN_SECOND))
			g_hash_table_iter_remove(&iter);
	}

	reveal = g_new0(VentureWebReveal, 1);
	reveal->secret = g_strdup(secret);
	reveal->token_id = token_id;
	reveal->created_at = g_date_time_ref(now);

	handle = venture_generate_token(16);
	g_hash_table_insert(self->reveals, g_strdup(handle), reveal);

	return handle;
}

/*
 * Spends a handle. Returns %NULL for one that was already used, never
 * existed, or sat unread past its expiry -- all three are the same answer to
 * the caller, which is that there is nothing to show.
 */
static VentureWebReveal *
venture_web_reveal_take(
	VentureWebServer	*self,
	const gchar		*handle
){
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *stored_key = NULL;
	VentureWebReveal *reveal = NULL;
	gpointer key = NULL;
	gpointer value = NULL;

	if (venture_string_is_empty(handle))
		return NULL;

	/*
	 * Stolen rather than looked up, so that whatever happens next this
	 * handle is spent. steal_extended hands back the key as well; plain
	 * g_hash_table_steal() skips both destroy notifies and would leak it.
	 */
	if (!g_hash_table_steal_extended(self->reveals, handle, &key, &value))
		return NULL;

	stored_key = key;
	reveal = value;
	now = venture_time_now();

	if (g_date_time_difference(now, reveal->created_at) >
	    (VENTURE_WEB_REVEAL_TTL_SECONDS * G_TIME_SPAN_SECOND))
	{
		venture_web_reveal_free(reveal);

		return NULL;
	}

	return reveal;
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

	notice = htmx_request_get_query_param(request, "notice");

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

		/*
		 * Signposted from here because this is where somebody looks for
		 * their own credentials. The page itself is admin-gated, so the
		 * link is shown only to somebody it will let in.
		 */
		if (venture_auth_require(self->auth, principal,
		                         VENTURE_USER_ROLE_ADMIN, NULL))
			g_string_append(content,
				"<h2>API tokens</h2>"
				"<p class=\"muted small\">Credentials for venturectl and "
				"anything else using the API.</p>"
				"<a class=\"btn\" href=\"/account/tokens\">Manage API "
				"tokens</a>");
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
 * The API tokens page.
 *
 * Admin, matching POST /api/v1/tokens: a token carries the role of whoever
 * minted it, so minting one is handing out a credential rather than editing
 * a record. This page exists because the alternative was two curl calls --
 * sign in for a cookie, then post with it -- which is a lot of ceremony for
 * something an operator needs before they can use the CLI at all.
 */
static HtmxResponse *
venture_web_ui_tokens(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) tokens = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	VentureWebReveal *reveal;
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

	query = venture_query_new(VENTURE_TYPE_API_TOKEN);
	venture_query_set_limit(query, 0);
	tokens = venture_database_find(venture_context_get_database(self->context),
	                               query, &error);

	if (NULL == tokens)
		return venture_web_error_response(error);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>API tokens</h1>"
	                       "<p class=\"muted\">Credentials for venturectl, "
	                       "MCP agents and anything else talking to the "
	                       "API.</p></div></div>");

	notice = htmx_request_get_query_param(request, "notice");

	if (NULL != notice)
	{
		gboolean ok;

		ok = (NULL != g_strrstr(notice, "ok"));
		g_string_append_printf(content, "<div class=\"banner %s\">",
		                       ok ? "banner-ok" : "banner-warn");

		if (0 == g_strcmp0(notice, "revoked-ok"))
			g_string_append(content,
				"Token revoked. Anything using it is now signed out.");
		else if (0 == g_strcmp0(notice, "gone"))
			g_string_append(content,
				"That token has already been shown. It is stored only as a "
				"hash, so it cannot be shown again --- revoke it and mint "
				"another.");
		else
			g_string_append(content, "That did not work.");

		g_string_append(content, "</div>");
	}

	/*
	 * The one and only time this secret is displayed. It is not in the
	 * URL, not in the database and not recoverable: what is stored is a
	 * hash, exactly as for a password.
	 */
	reveal = venture_web_reveal_take(self,
		htmx_request_get_query_param(request, "reveal"));

	if (NULL != reveal)
	{
		g_string_append(content,
			"<div class=\"card\"><div class=\"card-body\">"
			"<div class=\"banner banner-ok\">"
			"Copy this now. It will not be shown again."
			"</div>"
			"<pre class=\"token-reveal\"><code>");
		venture_html_escape_append(content, reveal->secret);
		g_string_append(content,
			"</code></pre>"
			"<p class=\"muted small\">Only a hash of it is stored, so nobody "
			"--- including this page --- can show it to you a second time. "
			"Use it as <code>VENTURE_TOKEN</code>, or as an "
			"<code>Authorization: Bearer</code> header.</p>"
			"</div></div>");

		venture_web_reveal_free(reveal);
	}

	g_string_append_printf(content,
		"<div class=\"card\"><div class=\"card-body\">"
		"<h2>Mint a token</h2>"
		"<p class=\"muted small\">It will carry your own role (%s). There is "
		"no way to mint one with more access than you have, and none to "
		"narrow it either --- for a token that may do less, sign in as a user "
		"who may do less.</p>"
		"<form method=\"post\" action=\"/account/tokens\">"
		"<label>Name"
		"<input type=\"text\" name=\"name\" required maxlength=\"120\" "
		"placeholder=\"laptop venturectl\"></label>"
		"<label>Expires"
		"<select name=\"expires_in_days\">"
		"<option value=\"0\">never</option>"
		"<option value=\"30\">in 30 days</option>"
		"<option value=\"90\">in 90 days</option>"
		"<option value=\"365\">in a year</option>"
		"</select></label>"
		"<button class=\"btn btn-primary\" type=\"submit\">Mint token</button>"
		"</form>"
		"</div></div>",
		venture_enum_to_nick(VENTURE_TYPE_USER_ROLE, (gint)principal->role));

	g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
	                         "<table class=\"table\"><thead><tr>"
	                         "<th>Name</th><th>Prefix</th><th>Role</th>"
	                         "<th>Expires</th><th>Last used</th>"
	                         "<th>Status</th><th></th>"
	                         "</tr></thead><tbody>");

	for (i = 0; i < tokens->len; i++)
	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *prefix = NULL;
		g_autoptr(GDateTime) expires_at = NULL;
		g_autoptr(GDateTime) last_used = NULL;
		VentureEntity *entity;
		VentureUserRole role;
		gboolean active;
		gint64 id;

		entity = g_ptr_array_index(tokens, i);
		id = venture_entity_get_id(entity);

		g_object_get(entity, "name", &name, "prefix", &prefix, "role", &role,
		             "active", &active, "expires-at", &expires_at,
		             "last-used-at", &last_used, NULL);

		g_string_append(content, "<tr><td>");
		venture_html_escape_append(content,
			venture_string_is_empty(name) ? "\xe2\x80\x94" : name);

		/*
		 * The first characters, stored in the clear precisely so a token
		 * can be told apart from another one here and in the logs
		 * without revealing any of them.
		 */
		g_string_append(content, "</td><td><code class=\"muted\">");
		venture_html_escape_append(content,
			venture_string_is_empty(prefix) ? "\xe2\x80\x94" : prefix);
		g_string_append(content, "\xe2\x80\xa6</code></td><td>");
		venture_html_escape_append(content,
			venture_enum_to_nick(VENTURE_TYPE_USER_ROLE, (gint)role));
		g_string_append(content, "</td><td class=\"muted small\">");

		if (NULL != expires_at)
		{
			g_autofree gchar *when = NULL;

			when = g_date_time_format(expires_at, "%Y-%m-%d");
			venture_html_escape_append(content, when);
		}
		else
		{
			g_string_append(content, "never");
		}

		g_string_append(content, "</td><td class=\"muted small\">");

		if (NULL != last_used)
		{
			g_autofree gchar *when = NULL;

			when = g_date_time_format(last_used, "%Y-%m-%d %H:%M");
			venture_html_escape_append(content, when);
		}
		else
		{
			g_string_append(content, "never");
		}

		g_string_append_printf(content,
			"</td><td>%s</td><td>",
			active ? "<span class=\"badge positive\">active</span>"
			       : "<span class=\"badge\">revoked</span>");

		if (active)
			g_string_append_printf(content,
				"<form method=\"post\" action=\"/account/tokens/%"
				G_GINT64_FORMAT "/revoke\" class=\"inline-form\">"
				"<button class=\"btn btn-sm\" type=\"submit\">Revoke</button>"
				"</form>", id);

		g_string_append(content, "</td></tr>");
	}

	if (0 == tokens->len)
		g_string_append(content,
			"<tr><td colspan=\"7\" class=\"muted\">No tokens yet.</td></tr>");

	g_string_append(content, "</tbody></table></div></div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/account/tokens", "API tokens",
		                 content->str), 200);
}

/*
 * Mints a token and redirects to the page that will show it once.
 *
 * Shares venture_web_api_mint_token()'s rule that this is an administrative
 * act, and its consequence that the token carries the minter's role.
 */
static HtmxResponse *
venture_web_ui_tokens_create(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureApiToken) token = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *handle = NULL;
	g_autofree gchar *location = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	const gchar *name;
	const gchar *expires_in;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN,
	                          &error))
		return venture_web_error_response(error);

	name = htmx_request_get_form_value(request, "name");
	expires_in = htmx_request_get_form_value(request, "expires_in_days");

	token = venture_api_token_new();
	g_object_set(token, "name",
	             venture_string_is_empty(name) ? "api token" : name,
	             "role", principal->role, NULL);

	/*
	 * Whose token this is. The API mint route did not record this, which
	 * left a list of credentials nobody could be held to; a token that
	 * outlives the person who made it is exactly the one worth finding.
	 */
	if (0 != principal->user_id)
		g_object_set(token, "user-id", principal->user_id, NULL);

	venture_entity_set_organization_id(VENTURE_ENTITY(token),
		venture_context_get_default_organization_id(self->context));

	if (!venture_string_is_empty(expires_in))
	{
		gint64 days;

		days = g_ascii_strtoll(expires_in, NULL, 10);

		/* Zero is the "never" option rather than "expired on creation",
		 * so anything that is not a positive number means no expiry. */
		if (days > 0)
		{
			g_autoptr(GDateTime) now = NULL;
			g_autoptr(GDateTime) expires_at = NULL;

			now = venture_time_now();
			expires_at = g_date_time_add_days(now, (gint)days);
			g_object_set(token, "expires-at", expires_at, NULL);
		}
	}

	secret = venture_api_token_generate(token);

	if (NULL == secret)
		return venture_web_redirect_to("/account/tokens?notice=failed");

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(token), &actor, &error))
		return venture_web_redirect_to("/account/tokens?notice=failed");

	handle = venture_web_reveal_store(self, secret,
	                                  venture_entity_get_id(
	                                      VENTURE_ENTITY(token)));
	location = g_strdup_printf("/account/tokens?reveal=%s", handle);

	return venture_web_redirect_to(location);
}

/*
 * Revokes a token by clearing its active flag rather than deleting the row.
 *
 * A deleted token takes with it the record that it ever existed, which is
 * the opposite of what somebody revoking a credential after an incident
 * needs: they want the prefix, the role and the last-used time to stay
 * readable. venture_api_token_matches() refuses an inactive token, so this
 * takes effect on the next request.
 */
static HtmxResponse *
venture_web_ui_tokens_revoke(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) entity = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	const gchar *id_text;
	gint64 id;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN,
	                          &error))
		return venture_web_error_response(error);

	id_text = g_hash_table_lookup(params, "id");
	id = (NULL != id_text) ? g_ascii_strtoll(id_text, NULL, 10) : 0;

	entity = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_API_TOKEN, id, NULL);

	if (NULL == entity)
		return venture_web_redirect_to("/account/tokens?notice=failed");

	g_object_set(entity, "active", FALSE, NULL);
	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           entity, &actor, &error))
		return venture_web_redirect_to("/account/tokens?notice=failed");

	return venture_web_redirect_to("/account/tokens?notice=revoked-ok");
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

	notice = htmx_request_get_query_param(request, "notice");

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

	/*
	 * Whether a write may be proposed instead of performed.
	 *
	 * Advertised because the alternative is unsafe. `?stage=1` on a build
	 * that has never heard of it is an unknown query parameter on a write
	 * route, which is ignored -- so a client that assumed staging would
	 * send a change it meant to hold back and the record would be
	 * written. A client can ask here first and refuse to pretend.
	 */
	json_builder_set_member_name(builder, "staged_writes");
	json_builder_add_boolean_value(builder, TRUE);

	/*
	 * And nothing beyond that. This route is the one deliberate
	 * exception to the authentication rule -- a container healthcheck
	 * runs before anybody has credentials -- so it may say what this
	 * build can do and must not say what this install is doing. How many
	 * changes are waiting is business activity, and it is behind the
	 * viewer role at /api/v1/confirmations.
	 */

	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/* --- Chat ---------------------------------------------------------------- */

/* Defined with the rest of the staged-change UI, below. */
static void
venture_web_chat_append_confirmations(
	VentureWebServer	*self,
	GString			*html,
	GHashTable		*already_seen
);


/*
 * Loads a chat thread only if it belongs to the caller.
 *
 * The mismatch case reports NOT_FOUND rather than FORBIDDEN on purpose:
 * "that thread exists but is not yours" confirms the identifier is live,
 * and thread ids are sequential.
 *
 * Returns: (transfer full) (nullable): the thread, or %NULL
 */
static VentureChatThread *
venture_web_chat_get_thread(
	VentureWebServer	 *self,
	VentureAuthPrincipal	 *principal,
	gint64			  thread_id,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	gint64 owner;

	record = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_CHAT_THREAD, thread_id, error);

	if (NULL == record)
		return NULL;

	g_object_get(record, "user-id", &owner, NULL);

	if (owner != principal->user_id)
	{
		g_clear_object(&record);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No chat thread %" G_GINT64_FORMAT, thread_id);
		return NULL;
	}

	return VENTURE_CHAT_THREAD(g_steal_pointer(&record));
}

/*
 * The messages of one thread, oldest first. Insertion order, not timestamp
 * order: two messages written in the same clock tick must still come back in
 * the order they were said.
 */
static GPtrArray *
venture_web_chat_get_messages(
	VentureWebServer	 *self,
	gint64			  thread_id,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_CHAT_MESSAGE);

	if (!venture_query_add_filter_int(query, "thread-id",
	                                  VENTURE_FILTER_OP_EQ, thread_id, error))
		return NULL;

	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 500);

	return venture_database_find(venture_context_get_database(self->context),
	                             query, error);
}

/*
 * Decodes the five standard HTML entities a model may have emitted.
 *
 * Some models -- Grok reliably -- escape punctuation as if their reply were
 * going straight into a page, so "P&L" arrives as "P&amp;L". The stored
 * body is whatever the model said; this runs at render time, before the
 * real escaping, so "&amp;" displays as the "&" the model meant while an
 * actual "<script>" in the decoded text is still neutralised a step later.
 * "&amp;" is decoded last: "&amp;lt;" means a literal "&lt;", and decoding
 * ampersands first would turn it into a "<" instead.
 *
 * Returns: (transfer full): the decoded text
 */
static gchar *
venture_web_chat_decode_entities(const gchar *text)
{
	GString *out;

	out = g_string_new(text);
	g_string_replace(out, "&lt;", "<", 0);
	g_string_replace(out, "&gt;", ">", 0);
	g_string_replace(out, "&quot;", "\"", 0);
	g_string_replace(out, "&#39;", "'", 0);
	g_string_replace(out, "&amp;", "&", 0);

	return g_string_free(out, FALSE);
}

/*
 * Renders an assistant reply as the small markdown chat replies actually
 * use: paragraphs, line breaks, "- " and "1." lists, **bold**, `code`, and
 * "#" headings as bold lines.
 *
 * The pipeline order is the security property: decode the entities the
 * model emitted, escape *everything* for real, then apply formatting to the
 * escaped text. Markdown syntax carries no HTML characters, so the regexes
 * work unchanged on escaped text, and nothing the model says can smuggle
 * markup past the escape.
 */
static void
venture_web_chat_append_rich(
	GString		*html,
	const gchar	*body
){
	static gsize initialized = 0;
	static GRegex *bold_regex = NULL;
	static GRegex *code_regex = NULL;
	g_autofree gchar *decoded = NULL;
	g_autoptr(GString) escaped = NULL;
	g_autofree gchar *with_bold = NULL;
	g_autofree gchar *formatted = NULL;
	g_auto(GStrv) blocks = NULL;
	gsize b;

	if (g_once_init_enter(&initialized))
	{
		bold_regex = g_regex_new("\\*\\*([^*\\n]+)\\*\\*", 0, 0, NULL);
		code_regex = g_regex_new("`([^`\\n]+)`", 0, 0, NULL);
		g_once_init_leave(&initialized, 1);
	}

	decoded = venture_web_chat_decode_entities(body);

	escaped = g_string_new(NULL);
	venture_html_escape_append(escaped, decoded);

	with_bold = g_regex_replace(bold_regex, escaped->str, -1, 0,
	                            "<strong>\\1</strong>", 0, NULL);
	formatted = g_regex_replace(code_regex, with_bold, -1, 0,
	                            "<code>\\1</code>", 0, NULL);

	/* Blank lines separate blocks; within a block, consecutive list lines
	 * group into one list and everything else joins with line breaks. */
	blocks = g_strsplit(formatted, "\n\n", -1);

	for (b = 0; NULL != blocks[b]; b++)
	{
		g_auto(GStrv) lines = NULL;
		gsize i;

		if ('\0' == *g_strstrip(blocks[b]))
			continue;

		lines = g_strsplit(blocks[b], "\n", -1);
		i = 0;

		while (NULL != lines[i])
		{
			const gchar *line;

			line = g_strstrip(lines[i]);

			if ('\0' == *line)
			{
				i++;
			}
			else if (g_str_has_prefix(line, "- ") ||
			         g_str_has_prefix(line, "* "))
			{
				g_string_append(html, "<ul>");

				while ((NULL != lines[i]) &&
				       (g_str_has_prefix(g_strstrip(lines[i]), "- ") ||
				        g_str_has_prefix(g_strstrip(lines[i]), "* ")))
				{
					g_string_append(html, "<li>");
					g_string_append(html,
					                g_strstrip(lines[i]) + 2);
					g_string_append(html, "</li>");
					i++;
				}

				g_string_append(html, "</ul>");
			}
			else if (g_ascii_isdigit(line[0]) &&
			         (NULL != strstr(line, ". ")) &&
			         ((gsize)(strstr(line, ". ") - line) <= 3))
			{
				g_string_append(html, "<ol>");

				while (NULL != lines[i])
				{
					const gchar *item;
					const gchar *dot;

					item = g_strstrip(lines[i]);
					dot = (g_ascii_isdigit(item[0]))
						? strstr(item, ". ") : NULL;

					if ((NULL == dot) ||
					    ((gsize)(dot - item) > 3))
						break;

					g_string_append(html, "<li>");
					g_string_append(html, dot + 2);
					g_string_append(html, "</li>");
					i++;
				}

				g_string_append(html, "</ol>");
			}
			else if ('#' == line[0])
			{
				/* A heading is a bold line; a chat bubble has no
				 * business with an actual <h3>. */
				while ('#' == *line)
					line++;

				g_string_append(html, "<p><strong>");
				g_string_append(html, g_strstrip((gchar *)line));
				g_string_append(html, "</strong></p>");
				i++;
			}
			else
			{
				gboolean first;

				g_string_append(html, "<p>");
				first = TRUE;

				while (NULL != lines[i])
				{
					const gchar *text;

					text = g_strstrip(lines[i]);

					if (('\0' == *text) ||
					    g_str_has_prefix(text, "- ") ||
					    g_str_has_prefix(text, "* ") ||
					    ('#' == *text))
						break;

					if (!first)
						g_string_append(html, "<br>");

					g_string_append(html, text);
					first = FALSE;
					i++;
				}

				g_string_append(html, "</p>");
			}
		}
	}
}

static void
venture_web_chat_append_message(
	GString		*html,
	VentureChatRole	 role,
	const gchar	*body
){
	if (VENTURE_CHAT_ROLE_ASSISTANT == role)
	{
		g_string_append(html, "<div class=\"msg ai\">"
		                      "<span class=\"msg-avatar\">"
						 VENTURE_SPARK
						 "</span>"
		                      "<div class=\"msg-content\">");
		venture_web_chat_append_rich(html, body);
		g_string_append(html, "</div></div>");
		return;
	}

	/* The operator's own words are shown verbatim -- escaped, with the
	 * line breaks they typed preserved by the stylesheet rather than
	 * markup, so the server render and the client's optimistic echo agree
	 * to the byte. */
	g_string_append(html, "<div class=\"msg user\">"
	                      "<span class=\"msg-avatar\">You</span>"
	                      "<div class=\"msg-content\"><p>");
	venture_html_escape_append(html, body);
	g_string_append(html, "</p></div></div>");
}

/*
 * The hidden form field naming the active thread, swapped out-of-band so a
 * reply lands in whichever conversation the panel now shows. The client
 * mirrors this value into localStorage, which is what makes a conversation
 * resume after the tab is long gone.
 */
static void
venture_web_chat_append_thread_input(
	GString		*html,
	gint64		 thread_id
){
	if (thread_id > 0)
		g_string_append_printf(html,
			"<input type=\"hidden\" id=\"chat-thread\" name=\"thread\" "
			"value=\"%" G_GINT64_FORMAT "\" hx-swap-oob=\"true\">",
			thread_id);
	else
		/* An empty value, so the client also forgets the thread it was
		 * resuming -- this is the delete path. */
		g_string_append(html,
			"<input type=\"hidden\" id=\"chat-thread\" name=\"thread\" "
			"value=\"\" hx-swap-oob=\"true\">");
}

/* Swaps the panel heading to the active conversation's title. */
static void
venture_web_chat_append_title(
	GString		*html,
	const gchar	*title
){
	g_string_append(html, "<span class=\"ai-panel-title\" "
	                      "id=\"ai-panel-title\" hx-swap-oob=\"true\">");
	venture_html_escape_append(html, title);
	g_string_append(html, "</span>");
}

/*
 * Renders the resume list into @html.
 *
 * Returns: %TRUE on success
 */
static gboolean
venture_web_chat_render_threads(
	VentureWebServer	 *self,
	VentureAuthPrincipal	 *principal,
	GString			 *html,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) threads = NULL;
	guint i;

	query = venture_query_new(VENTURE_TYPE_CHAT_THREAD);

	if (!venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ,
	                                  principal->user_id, error))
		return FALSE;

	venture_query_add_order(query, "last-activity-at",
	                        VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 50);

	threads = venture_database_find(venture_context_get_database(self->context),
	                                query, error);

	if (NULL == threads)
		return FALSE;

	g_string_append(html, "<div class=\"thread-list\">");

	if (0 == threads->len)
		g_string_append(html, "<div class=\"empty\">"
		                      "<h3>No conversations yet</h3>"
		                      "<p class=\"muted\">Ask something below to start "
		                      "one.</p></div>");

	for (i = 0; i < threads->len; i++)
	{
		VentureEntity *thread;
		g_autofree gchar *title = NULL;
		g_autoptr(GDateTime) last = NULL;
		gint64 id;

		thread = g_ptr_array_index(threads, i);
		id = venture_entity_get_id(thread);
		g_object_get(thread, "title", &title,
		             "last-activity-at", &last, NULL);

		g_string_append(html, "<div class=\"thread-item\">");
		g_string_append_printf(html,
			"<button type=\"button\" class=\"thread-open\" "
			"data-thread-id=\"%" G_GINT64_FORMAT "\" "
			"hx-get=\"/ui/chat/thread/%" G_GINT64_FORMAT "\" "
			"hx-target=\"#chat-log\" hx-swap=\"innerHTML\">", id, id);
		g_string_append(html, "<span class=\"thread-title\">");
		venture_html_escape_append(html,
			venture_string_is_empty(title) ? "Untitled" : title);
		g_string_append(html, "</span>");

		if (NULL != last)
		{
			g_autofree gchar *when = NULL;

			when = venture_time_to_date_string(last,
				venture_context_get_timezone(self->context));

			if (NULL != when)
			{
				g_string_append(html, "<span class=\"thread-when\">");
				venture_html_escape_append(html, when);
				g_string_append(html, "</span>");
			}
		}

		g_string_append(html, "</button>");
		g_string_append_printf(html,
			"<button type=\"button\" class=\"thread-delete\" "
			"title=\"Delete conversation\" "
			"hx-post=\"/ui/chat/thread/%" G_GINT64_FORMAT "/delete\" "
			"hx-confirm=\"Delete this conversation?\" "
			"hx-target=\"#chat-log\" hx-swap=\"innerHTML\">"
			"\xc3\x97</button>", id);
		g_string_append(html, "</div>");
	}

	g_string_append(html, "</div>");
	venture_web_chat_append_title(html, "Conversations");

	return TRUE;
}

/*
 * GET /ui/chat/threads - the resume list.
 */
static HtmxResponse *
venture_web_ui_chat_threads(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) html = NULL;
	g_autoptr(GError) error = NULL;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	html = g_string_new(NULL);

	if (!venture_web_chat_render_threads(self, principal, html, &error))
		return venture_web_error_response(error);

	return venture_web_html_response(
		g_string_free(g_steal_pointer(&html), FALSE), 200);
}

/*
 * GET /ui/chat/thread/:id - one transcript, replayed into the panel.
 */
static HtmxResponse *
venture_web_ui_chat_thread(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureChatThread) thread = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *title = NULL;
	g_autoptr(GError) error = NULL;
	gint64 thread_id;
	guint i;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	thread_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	thread = venture_web_chat_get_thread(self, principal, thread_id, &error);

	if (NULL == thread)
		return venture_web_error_response(error);

	messages = venture_web_chat_get_messages(self, thread_id, &error);

	if (NULL == messages)
		return venture_web_error_response(error);

	html = g_string_new(NULL);

	for (i = 0; i < messages->len; i++)
	{
		VentureEntity *message;
		g_autofree gchar *body = NULL;
		VentureChatRole role;

		message = g_ptr_array_index(messages, i);
		g_object_get(message, "role", &role, "body", &body, NULL);
		venture_web_chat_append_message(html, role, body);
	}

	/*
	 * Anything still waiting for a decision, at the end of the replayed
	 * transcript. A staged change whose conversation was closed before it
	 * was approved is otherwise unreachable until it expires -- the
	 * operator would see the AI say "waiting for approval" with nothing
	 * to approve. This is a fresh render of the log, so nothing is
	 * duplicated by showing them again here.
	 */
	venture_web_chat_append_confirmations(self, html, NULL);

	g_object_get(thread, "title", &title, NULL);
	venture_web_chat_append_thread_input(html, thread_id);
	venture_web_chat_append_title(html,
		venture_string_is_empty(title) ? "Untitled" : title);

	return venture_web_html_response(
		g_string_free(g_steal_pointer(&html), FALSE), 200);
}

/*
 * POST /ui/chat/thread/:id/delete - forget a conversation.
 *
 * Soft, like every other delete here, and the messages go with the thread:
 * a transcript whose thread is gone would be unreachable but still turn up
 * in an owner's export, which is exactly the half-deleted state that erodes
 * trust in a delete button.
 */
static HtmxResponse *
venture_web_ui_chat_thread_delete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureChatThread) thread = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	gint64 thread_id;
	guint i;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	thread_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	thread = venture_web_chat_get_thread(self, principal, thread_id, &error);

	if (NULL == thread)
		return venture_web_error_response(error);

	messages = venture_web_chat_get_messages(self, thread_id, &error);

	if (NULL == messages)
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);

	for (i = 0; i < messages->len; i++)
	{
		if (!venture_database_delete(
			venture_context_get_database(self->context),
			g_ptr_array_index(messages, i), &actor, &error))
			return venture_web_error_response(error);
	}

	if (!venture_database_delete(venture_context_get_database(self->context),
	                             VENTURE_ENTITY(thread), &actor, &error))
		return venture_web_error_response(error);

	/* The panel goes back to the resume list, minus this thread, and the
	 * hidden thread field is cleared in case it named the one just
	 * deleted -- otherwise the next question would try to continue it. */
	{
		g_autoptr(GString) html = NULL;

		html = g_string_new(NULL);

		if (!venture_web_chat_render_threads(self, principal, html, &error))
			return venture_web_error_response(error);

		venture_web_chat_append_thread_input(html, 0);

		return venture_web_html_response(
			g_string_free(g_steal_pointer(&html), FALSE), 200);
	}
}

/* --- Chat attachments ----------------------------------------------------- */

/*
 * How much of an attachment's text rides along in one model message. The
 * full text is stored on the document record, so nothing is lost -- the
 * model is told it was cut and can venture_get the document for the rest.
 */
#define VENTURE_WEB_ATTACHMENT_TEXT_LIMIT (24000)

/* Uploads above this are refused outright. */
#define VENTURE_WEB_ATTACHMENT_MAX_BYTES (15 * 1024 * 1024)

/*
 * Whether a MIME type or filename names an image a vision model can read.
 *
 * The list is deliberately short: these four are what every vision-capable
 * provider accepts. A TIFF or a PSD would upload and then fail at the
 * provider, which is a worse experience than being told here.
 *
 * Returns: (transfer none) (nullable): the canonical MIME type, or %NULL
 */
static const gchar *
venture_web_image_mime_type(
	const gchar	*content_type,
	const gchar	*filename
){
	static const struct
	{
		const gchar *mime;
		const gchar *suffix;
	} kinds[] = {
		{ "image/png",  ".png"  },
		{ "image/jpeg", ".jpg"  },
		{ "image/jpeg", ".jpeg" },
		{ "image/webp", ".webp" },
		{ "image/gif",  ".gif"  }
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
	{
		if ((NULL != content_type) &&
		    g_str_has_prefix(content_type, kinds[i].mime))
			return kinds[i].mime;
	}

	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
	{
		g_autofree gchar *lower = NULL;

		if (NULL == filename)
			break;

		lower = g_ascii_strdown(filename, -1);

		if (g_str_has_suffix(lower, kinds[i].suffix))
			return kinds[i].mime;
	}

	return NULL;
}

/*
 * Pulls readable text out of an uploaded file.
 *
 * PDFs go through poppler when the build has it; anything that announces
 * itself as text -- including JSON, CSV and YAML, which is what exports and
 * invoices actually arrive as -- is taken verbatim if it is valid UTF-8.
 * Everything else yields %NULL, which is stored as "no text" rather than
 * treated as an error: the file itself is still kept.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL
 */
static gchar *
venture_web_extract_text(
	const gchar	*content_type,
	const gchar	*filename,
	GBytes		*data
){
	gboolean looks_pdf;
	gboolean looks_text;

	looks_pdf = ((NULL != content_type) &&
	             g_str_has_prefix(content_type, "application/pdf")) ||
	            ((NULL != filename) &&
	             g_str_has_suffix(filename, ".pdf"));

	looks_text = ((NULL != content_type) &&
	              (g_str_has_prefix(content_type, "text/") ||
	               g_str_has_prefix(content_type, "application/json") ||
	               g_str_has_prefix(content_type, "application/csv") ||
	               g_str_has_prefix(content_type, "application/x-yaml") ||
	               g_str_has_prefix(content_type, "application/yaml"))) ||
	             ((NULL != filename) &&
	              (g_str_has_suffix(filename, ".txt") ||
	               g_str_has_suffix(filename, ".md") ||
	               g_str_has_suffix(filename, ".org") ||
	               g_str_has_suffix(filename, ".csv") ||
	               g_str_has_suffix(filename, ".json") ||
	               g_str_has_suffix(filename, ".yaml") ||
	               g_str_has_suffix(filename, ".yml")));

	if (looks_pdf)
	{
#ifdef VENTURE_HAVE_POPPLER
		g_autoptr(PopplerDocument) document = NULL;
		g_autoptr(GString) text = NULL;
		gint pages;
		gint i;

		document = poppler_document_new_from_bytes(data, NULL, NULL);

		if (NULL == document)
			return NULL;

		text = g_string_new(NULL);
		pages = poppler_document_get_n_pages(document);

		for (i = 0; i < pages; i++)
		{
			g_autoptr(PopplerPage) page = NULL;
			g_autofree gchar *page_text = NULL;

			page = poppler_document_get_page(document, i);

			if (NULL == page)
				continue;

			page_text = poppler_page_get_text(page);

			if (venture_string_is_empty(page_text))
				continue;

			if (0 != text->len)
				g_string_append(text, "\n\n");

			g_string_append(text, page_text);
		}

		if (0 == text->len)
			return NULL;

		return g_string_free(g_steal_pointer(&text), FALSE);
#else
		/* Built without poppler: the PDF is stored, its text is not.
		 * The chat message says so instead of silently attaching an
		 * empty context. */
		return NULL;
#endif
	}

	if (looks_text)
	{
		const gchar *bytes;
		gsize length;

		bytes = g_bytes_get_data(data, &length);

		if ((NULL == bytes) || (0 == length) ||
		    !g_utf8_validate(bytes, (gssize)length, NULL))
			return NULL;

		return g_strndup(bytes, length);
	}

	return NULL;
}

/*
 * POST /ui/chat/upload - one file in, one document record out.
 *
 * The file lands under the state directory and becomes an ordinary document
 * record carrying its extracted text, so an uploaded invoice is not a blob
 * in a chat -- it is a searchable record the whole system can see, that a
 * conversation happens to reference.
 */
static HtmxResponse *
venture_web_ui_chat_upload(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	HtmxUploadedFile *file;
	VentureActor actor;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	/* An upload creates a document record, which is a write. */
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	files = htmx_uploaded_file_parse_multipart(
		htmx_request_get_content_type(request),
		htmx_request_get_body_bytes(request), NULL, &error);

	if ((NULL == files) || (0 == files->len))
	{
		if (NULL == error)
			g_set_error_literal(&error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "No file arrived");
		return venture_web_error_response(error);
	}

	file = g_ptr_array_index(files, 0);

	if (htmx_uploaded_file_get_size(file) > VENTURE_WEB_ATTACHMENT_MAX_BYTES)
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "That file is over the %d MB attachment limit",
		            VENTURE_WEB_ATTACHMENT_MAX_BYTES / (1024 * 1024));
		return venture_web_error_response(error);
	}

	{
		g_autoptr(VentureDocument) document = NULL;
		g_autofree gchar *directory = NULL;
		g_autofree gchar *safe_name = NULL;
		g_autofree gchar *token = NULL;
		g_autofree gchar *stored_name = NULL;
		g_autofree gchar *path = NULL;
		g_autofree gchar *checksum = NULL;
		g_autofree gchar *extracted = NULL;
		const gchar *image_mime;
		const gchar *filename;
		GBytes *data;

		filename = htmx_uploaded_file_get_filename(file);

		if (venture_string_is_empty(filename))
			filename = "attachment";

		/* The stored name is the original with everything that could
		 * traverse a path or confuse a shell squeezed out, prefixed
		 * with a random token so two "invoice.pdf"s never collide. */
		safe_name = g_strcanon(g_strdup(filename),
			"abcdefghijklmnopqrstuvwxyz"
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-", '_');
		token = venture_generate_token(8);
		stored_name = g_strdup_printf("%s_%s", token, safe_name);

		directory = g_build_filename(
			venture_config_get_state_dir(
				venture_context_get_config(self->context)),
			"attachments", NULL);

		if (0 != g_mkdir_with_parents(directory, 0700))
		{
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
			            "Cannot create %s", directory);
			return venture_web_error_response(error);
		}

		path = g_build_filename(directory, stored_name, NULL);

		if (!htmx_uploaded_file_save(file, path, &error))
			return venture_web_error_response(error);

		data = htmx_uploaded_file_get_data(file);
		checksum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, data);
		extracted = venture_web_extract_text(
			htmx_uploaded_file_get_content_type(file), filename, data);

		image_mime = venture_web_image_mime_type(
			htmx_uploaded_file_get_content_type(file), filename);

		document = venture_document_new();
		g_object_set(document,
		             "title", filename,
		             "kind", (NULL != image_mime) ? "screenshot"
		                                          : "attachment",
		             "path", path,
		             "mime-type",
		             htmx_uploaded_file_get_content_type(file),
		             "size-bytes",
		             (gint64)htmx_uploaded_file_get_size(file),
		             "hash", checksum,
		             "extracted-text", extracted,
		             NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(document),
			venture_context_get_default_organization_id(self->context));

		venture_auth_to_actor(principal, &actor);

		if (!venture_database_save(
			venture_context_get_database(self->context),
			VENTURE_ENTITY(document), &actor, &error))
			return venture_web_error_response(error);

		builder = json_builder_new();
		json_builder_begin_object(builder);

		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder,
			venture_entity_get_id(VENTURE_ENTITY(document)));

		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, filename);

		json_builder_set_member_name(builder, "size");
		json_builder_add_int_value(builder,
			(gint64)htmx_uploaded_file_get_size(file));

		/* Whether any text came out, so the client can warn about an
		 * attachment the model will not actually be able to read. An
		 * image needs no text: the model reads the picture. */
		json_builder_set_member_name(builder, "text_chars");
		json_builder_add_int_value(builder, (NULL != extracted)
			? (gint64)g_utf8_strlen(extracted, -1) : 0);

		json_builder_set_member_name(builder, "is_image");
		json_builder_add_boolean_value(builder, NULL != image_mime);

		json_builder_end_object(builder);
		node = json_builder_get_root(builder);

		return venture_web_json_response(node, 201);
	}
}

/*
 * Resolves the "attachments" form field into stored documents, appending
 * their text to what the model sees and a short reference to what the
 * transcript keeps. The two deliberately differ: the transcript names the
 * document; the document carries the text; and a resumed conversation
 * re-reads it through venture_get rather than replaying kilobytes of
 * invoice into every later turn.
 */
static gboolean
venture_web_chat_attach(
	VentureWebServer	 *self,
	const gchar		 *attachments,
	GString			 *model_text,
	GString			 *stored_text,
	GPtrArray		 *images,
	GPtrArray		 *image_types,
	GError			**error
){
	g_auto(GStrv) ids = NULL;
	gsize i;

	if (venture_string_is_empty(attachments))
		return TRUE;

	ids = g_strsplit(attachments, ",", -1);

	for (i = 0; NULL != ids[i]; i++)
	{
		g_autoptr(VentureEntity) record = NULL;
		g_autofree gchar *title = NULL;
		g_autofree gchar *text = NULL;
		g_autofree gchar *mime_type = NULL;
		g_autofree gchar *path = NULL;
		gint64 id;

		id = g_ascii_strtoll(g_strstrip(ids[i]), NULL, 10);

		if (0 == id)
			continue;

		record = venture_database_get(
			venture_context_get_database(self->context),
			VENTURE_TYPE_DOCUMENT, id, error);

		if (NULL == record)
			return FALSE;

		g_object_get(record, "title", &title,
		             "extracted-text", &text,
		             "mime-type", &mime_type,
		             "path", &path, NULL);

		g_string_append_printf(stored_text,
			"\n[Attached: %s (document #%" G_GINT64_FORMAT ")]",
			(NULL != title) ? title : "file", id);

		/*
		 * An image is handed to the model as an image, not described in
		 * text. The message still names it so the model can say which
		 * screenshot a figure came from, and so the stored transcript
		 * and the pictures agree about order.
		 */
		if ((NULL != images) &&
		    (NULL != venture_web_image_mime_type(mime_type, title)))
		{
			g_autoptr(GBytes) bytes = NULL;
			g_autofree gchar *contents = NULL;
			gsize length = 0;

			if ((NULL != path) &&
			    g_file_get_contents(path, &contents, &length, NULL))
			{
				bytes = g_bytes_new_take(g_steal_pointer(&contents),
				                         length);
				g_ptr_array_add(images, g_bytes_ref(bytes));
				g_ptr_array_add(image_types, g_strdup(
					venture_web_image_mime_type(mime_type,
					                            title)));

				g_string_append_printf(model_text,
					"\n\n[Image %u: %s (document #%"
					G_GINT64_FORMAT ")]",
					images->len,
					(NULL != title) ? title : "screenshot",
					id);
				continue;
			}

			/* The record exists but the file is gone: say so
			 * rather than answering about an image nobody sent. */
			g_string_append_printf(model_text,
				"\n\n[Image %s (document #%" G_GINT64_FORMAT
				") could not be read from disk.]",
				(NULL != title) ? title : "attachment", id);
			continue;
		}

		g_string_append_printf(model_text,
			"\n\n--- Attached file: %s (document #%" G_GINT64_FORMAT
			") ---\n",
			(NULL != title) ? title : "file", id);

		if (venture_string_is_empty(text))
		{
			g_string_append(model_text,
				"[No text could be extracted from this file.]");
		}
		else if (strlen(text) > VENTURE_WEB_ATTACHMENT_TEXT_LIMIT)
		{
			g_autofree gchar *cut = NULL;

			cut = venture_truncate(text,
			                       VENTURE_WEB_ATTACHMENT_TEXT_LIMIT);
			g_string_append(model_text, cut);
			g_string_append_printf(model_text,
				"\n[Truncated. The full text is on document #%"
				G_GINT64_FORMAT "; fetch it with venture_get if "
				"you need the rest.]", id);
		}
		else
		{
			g_string_append(model_text, text);
		}
	}

	return TRUE;
}

/* --- Staged changes, in the panel ------------------------------------------ */

/*
 * Whether a serialised field is bookkeeping rather than a value anybody
 * decides about.
 *
 * Returns: %TRUE if the field should stay out of a human-facing diff
 */
static gboolean
venture_web_field_is_machinery(const gchar *name)
{
	static const gchar *const machinery[] = {
		"type", "id", "uuid", "organization_id", "created_at",
		"updated_at", "deleted_at", "version", "display_name",
		"attributes", NULL
	};

	return g_strv_contains(machinery, name);
}

/*
 * Renders one JSON value the way a person reads it rather than the way it
 * serialises: money as an amount and a currency, strings unquoted, and a
 * nested object as something short rather than a wall of braces.
 *
 * Returns: (transfer full): the display text
 */
static gchar *
venture_web_json_to_display(JsonNode *value)
{
	if (JSON_NODE_HOLDS_VALUE(value))
	{
		GType held;

		held = json_node_get_value_type(value);

		if (G_TYPE_STRING == held)
			return g_strdup(json_node_get_string(value));

		if (G_TYPE_BOOLEAN == held)
			return g_strdup(json_node_get_boolean(value)
				? "yes" : "no");
	}

	/* Money serialises as an object; show it as money. */
	if (JSON_NODE_HOLDS_OBJECT(value))
	{
		JsonObject *object;

		object = json_node_get_object(value);

		if (json_object_has_member(object, "amount") &&
		    json_object_has_member(object, "currency"))
		{
			g_autoptr(VentureMoney) money = NULL;

			money = venture_money_new(
				venture_json_object_get_int(object, "amount", 0),
				venture_json_object_get_string(object,
					"currency", "USD"),
				(guint)venture_json_object_get_int(object,
					"exponent", 2));

			if (NULL != money)
				return venture_money_to_display_string(money,
				                                       TRUE);
		}
	}

	return venture_json_to_string(value, FALSE);
}

/*
 * Renders every pending confirmation as a card with its diff and the two
 * buttons that decide it.
 *
 * Until this existed the write-confirmation policy had no browser at all:
 * the AI would stage a change, announce it, and the only way to apply it
 * was a curl against /api/v1/confirmations. A staged write nobody can
 * approve is a feature that reads as a bug.
 */
static void
venture_web_chat_append_confirmations(
	VentureWebServer	*self,
	GString			*html,
	GHashTable		*already_seen
){
	g_autoptr(GPtrArray) pending = NULL;
	guint i;

	pending = venture_confirmation_store_list_pending(
		venture_context_get_confirmations(self->context));

	for (i = 0; (NULL != pending) && (i < pending->len); i++)
	{
		VentureConfirmation *confirmation;
		JsonNode *diff;
		const gchar *id;

		confirmation = g_ptr_array_index(pending, i);
		id = venture_confirmation_get_id(confirmation);

		/*
		 * Only what this turn staged. Rendering everything pending
		 * would repeat a card the log already shows, and two cards
		 * with one id is worse than cosmetic: the decided card is
		 * removed by id, so clicking the second would delete the
		 * first and leave the one just decided sitting there.
		 */
		if ((NULL != already_seen) &&
		    g_hash_table_contains(already_seen, id))
			continue;

		g_string_append(html, "<div class=\"confirm-card\" "
		                      "id=\"confirm-");
		venture_html_escape_append(html, id);
		g_string_append(html, "\"><h4>Waiting for your approval</h4><p>");
		venture_html_escape_append(html,
			venture_confirmation_get_summary(confirmation));
		g_string_append(html, "</p>");

		/* The field-by-field change, so approving is a decision about
		 * values rather than about a sentence describing them. */
		diff = venture_confirmation_get_diff(confirmation);

		if ((NULL != diff) && JSON_NODE_HOLDS_OBJECT(diff))
		{
			JsonObject *object;
			GList *members;
			GList *m;
			guint shown;

			object = json_node_get_object(diff);
			members = json_object_get_members(object);
			shown = 0;

			g_string_append(html, "<div class=\"confirm-diff\">");

			for (m = members; NULL != m; m = m->next)
			{
				g_autofree gchar *rendered = NULL;
				JsonNode *value;

				/*
				 * Only the fields a person is actually deciding
				 * about. The identity spine and the timestamps
				 * are machinery: shown here they bury the two
				 * lines that matter under eight that never
				 * differ, and an approval nobody reads is the
				 * same as no approval at all.
				 */
				if (venture_web_field_is_machinery(m->data))
					continue;

				value = json_object_get_member(object, m->data);

				/* An unset field is not a change. */
				if ((NULL == value) || JSON_NODE_HOLDS_NULL(value))
					continue;

				rendered = venture_web_json_to_display(value);

				if (venture_string_is_empty(rendered))
					continue;

				g_string_append(html, "<div><span class=\"add\">");
				venture_html_escape_append(html, m->data);
				g_string_append(html, "</span>: ");
				venture_html_escape_append(html, rendered);
				g_string_append(html, "</div>");
				shown++;
			}

			if (0 == shown)
				g_string_append(html, "<div class=\"muted\">"
				                      "No field values</div>");

			g_string_append(html, "</div>");
			g_list_free(members);
		}

		g_string_append(html, "<div class=\"confirm-actions\">");
		g_string_append_printf(html,
			"<button class=\"btn btn-primary btn-sm\" type=\"button\" "
			"hx-post=\"/ui/chat/confirm/%s/approve\" "
			"hx-target=\"#chat-log\" hx-swap=\"beforeend\">"
			"Apply</button>"
			"<button class=\"btn btn-sm\" type=\"button\" "
			"hx-post=\"/ui/chat/confirm/%s/reject\" "
			"hx-target=\"#chat-log\" hx-swap=\"beforeend\">"
			"Discard</button></div></div>", id, id);
	}
}

/*
 * POST /ui/chat/confirm/:id/{approve,reject} - decide one staged change.
 */
static HtmxResponse *
venture_web_ui_chat_decide(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data,
	gboolean	 approve
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) html = NULL;
	g_autoptr(GError) error = NULL;
	VentureConfirmationStore *store;
	const gchar *id;
	gboolean decided;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	/* Applying a staged write is a write; deciding is the editor's. */
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	store = venture_context_get_confirmations(self->context);
	id = g_hash_table_lookup(params, "id");

	/*
	 * And the role that record type demands in its own right.
	 *
	 * Approving is an editor's authority in general, but a forge names
	 * the host this install sends its token to and a rule decides what
	 * runs unattended -- the direct routes for those require owner and
	 * admin. Without this check, a change the REST API refuses to an
	 * editor could be made by asking the assistant for it and approving
	 * the result. Staging would launder the authorisation, which is the
	 * opposite of what staging is for.
	 */
	if (approve)
	{
		VentureConfirmation *pending;

		pending = venture_confirmation_store_find(store, id);

		if (NULL != pending)
		{
			GType staged_type;

			staged_type = venture_confirmation_get_entity_type(pending);

			if ((G_TYPE_INVALID != staged_type) &&
			    !venture_web_require_for_type(self, principal, staged_type,
			                                  VENTURE_USER_ROLE_EDITOR,
			                                  &error))
				return venture_web_error_response(error);
		}
	}

	decided = approve
		? venture_confirmation_store_approve(store, id,
			(NULL != principal) ? principal->name : NULL, &error)
		: venture_confirmation_store_reject(store, id,
			(NULL != principal) ? principal->name : NULL, &error);

	html = g_string_new("<div class=\"msg ai\">"
	                    "<span class=\"msg-avatar\">"
						 VENTURE_SPARK
						 "</span>"
	                    "<div class=\"msg-content\">");

	if (!decided)
	{
		g_string_append(html, "<div class=\"notice negative\">");
		venture_html_escape_append(html, error->message);
		g_string_append(html, "</div>");
	}
	else
	{
		g_string_append_printf(html, "<div class=\"notice %s\">%s</div>",
			approve ? "positive" : "info",
			approve ? "Applied and recorded in the audit log."
			        : "Discarded. Nothing was changed.");
	}

	/* The card the buttons lived in is gone -- the confirmation is no
	 * longer pending -- so remove it out of band rather than leaving a
	 * decided change looking like it still needs deciding. */
	g_string_append_printf(html,
		"</div></div><div id=\"confirm-%s\" hx-swap-oob=\"delete\"></div>",
		(NULL != id) ? id : "");

	return venture_web_html_response(
		g_string_free(g_steal_pointer(&html), FALSE), 200);
}

static HtmxResponse *
venture_web_ui_chat_approve(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	return venture_web_ui_chat_decide(request, params, user_data, TRUE);
}

static HtmxResponse *
venture_web_ui_chat_reject(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	return venture_web_ui_chat_decide(request, params, user_data, FALSE);
}

/*
 * POST /ui/chat - one exchange, persisted on both sides.
 */
static HtmxResponse *
venture_web_ui_chat(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureChatThread) thread = NULL;
	g_autoptr(GPtrArray) history = NULL;
	g_autoptr(GString) html = NULL;
	g_autoptr(GString) stored_text = NULL;
	g_autoptr(GString) model_text = NULL;
	g_autoptr(GPtrArray) images = NULL;
	g_autoptr(GPtrArray) image_types = NULL;
	g_autoptr(GHashTable) staged_before = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	const gchar *message;
	const gchar *thread_param;
	gboolean fresh_thread;
	gint64 thread_id;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	message = htmx_request_get_form_value(request, "message");
	html = g_string_new(NULL);

	if (venture_string_is_empty(message))
		return venture_web_html_response(g_strdup(""), 200);

	/*
	 * The response deliberately does not echo the question back. The
	 * client already showed it the moment Send was pressed -- a message
	 * that only appears once the model answers makes every send feel
	 * dropped -- and echoing it here again would double it.
	 */
	if (NULL == venture_context_get_ai_service(self->context))
	{
		/* Nothing is persisted: a transcript of questions nothing
		 * answered is not a conversation worth resuming. */
		g_string_append(html, "<div class=\"msg ai\">"
		                      "<span class=\"msg-avatar\">"
						 VENTURE_SPARK
						 "</span>"
		                      "<div class=\"msg-content\">"
		                      "<div class=\"notice info\">AI is not configured "
		                      "on this instance.</div></div></div>");

		return venture_web_html_response(
			g_string_free(g_steal_pointer(&html), FALSE), 200);
	}

	venture_auth_to_actor(principal, &actor);
	now = venture_time_now();

	/* Resume the named thread, or open one titled after the question. */
	thread_param = htmx_request_get_form_value(request, "thread");
	fresh_thread = venture_string_is_empty(thread_param);

	if (!fresh_thread)
	{
		thread = venture_web_chat_get_thread(self, principal,
			g_ascii_strtoll(thread_param, NULL, 10), &error);

		if (NULL == thread)
			return venture_web_error_response(error);
	}
	else
	{
		g_autofree gchar *title = NULL;

		title = venture_truncate(message, 60);

		thread = venture_chat_thread_new();
		g_object_set(thread, "title", title,
		             "user-id", principal->user_id,
		             "last-activity-at", now, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(thread),
			venture_context_get_default_organization_id(self->context));

		if (!venture_database_save(
			venture_context_get_database(self->context),
			VENTURE_ENTITY(thread), &actor, &error))
			return venture_web_error_response(error);
	}

	thread_id = venture_entity_get_id(VENTURE_ENTITY(thread));

	/*
	 * Attachments: the transcript keeps a one-line reference per file,
	 * the model gets the extracted text appended to this turn only.
	 */
	stored_text = g_string_new(message);
	model_text = g_string_new(message);
	images = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
	image_types = g_ptr_array_new_with_free_func(g_free);

	if (!venture_web_chat_attach(self,
		htmx_request_get_form_value(request, "attachments"),
		model_text, stored_text, images, image_types, &error))
		return venture_web_error_response(error);

	/* answer_with_images wants a NULL-terminated array of types. */
	g_ptr_array_add(image_types, NULL);

	/* What was already waiting before this turn, so the reply shows the
	 * changes this turn staged rather than every one outstanding. */
	staged_before = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      NULL);

	if (NULL != venture_context_get_ai_service(self->context))
	{
		g_autoptr(GPtrArray) already = NULL;
		guint p;

		already = venture_confirmation_store_list_pending(
			venture_context_get_confirmations(self->context));

		for (p = 0; (NULL != already) && (p < already->len); p++)
			g_hash_table_add(staged_before, g_strdup(
				venture_confirmation_get_id(
					g_ptr_array_index(already, p))));
	}

	/* Fetched before the new question is stored, so the model is not
	 * shown the question twice. */
	history = venture_web_chat_get_messages(self, thread_id, &error);

	if (NULL == history)
		return venture_web_error_response(error);

	{
		g_autoptr(VentureChatMessage) stored = NULL;

		stored = venture_chat_message_new();
		g_object_set(stored, "thread-id", thread_id,
		             "role", VENTURE_CHAT_ROLE_USER,
		             "body", stored_text->str, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(stored),
			venture_context_get_default_organization_id(self->context));

		if (!venture_database_save(
			venture_context_get_database(self->context),
			VENTURE_ENTITY(stored), &actor, &error))
			return venture_web_error_response(error);
	}

	{
		g_autofree gchar *answer = NULL;

		answer = venture_ai_service_answer_with_images(
			venture_context_get_ai_service(self->context), history,
			model_text->str, images,
			(const gchar *const *)image_types->pdata, principal,
			&error);

		if (NULL == answer)
		{
			/* The question stays stored -- resuming the thread and
			 * asking again is the recovery -- but a provider error
			 * is not part of the conversation. */
			g_string_append(html, "<div class=\"msg ai\">"
			                      "<span class=\"msg-avatar\">"
						 VENTURE_SPARK
						 "</span>"
			                      "<div class=\"msg-content\">"
			                      "<div class=\"notice negative\">");
			venture_html_escape_append(html, error->message);
			g_string_append(html, "</div></div></div>");
		}
		else
		{
			g_autoptr(VentureChatMessage) stored = NULL;

			stored = venture_chat_message_new();
			g_object_set(stored, "thread-id", thread_id,
			             "role", VENTURE_CHAT_ROLE_ASSISTANT,
			             "body", answer, NULL);
			venture_entity_set_organization_id(VENTURE_ENTITY(stored),
				venture_context_get_default_organization_id(self->context));

			if (!venture_database_save(
				venture_context_get_database(self->context),
				VENTURE_ENTITY(stored), &actor, &error))
				return venture_web_error_response(error);

			venture_web_chat_append_message(html,
				VENTURE_CHAT_ROLE_ASSISTANT, answer);

			/* Anything the turn staged, with the buttons that
			 * decide it. */
			venture_web_chat_append_confirmations(self, html,
			                                      staged_before);
		}
	}

	g_object_set(thread, "last-activity-at", now, NULL);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(thread), &actor, NULL))
	{
		/* Only the resume list's ordering suffers; the exchange itself
		 * is already stored. Not worth failing the reply over. */
	}

	venture_web_chat_append_thread_input(html, thread_id);

	if (fresh_thread)
	{
		g_autofree gchar *title = NULL;

		g_object_get(thread, "title", &title, NULL);
		venture_web_chat_append_title(html, title);
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

	/* Whose token this is, so the list on /account/tokens can answer that
	 * for a token minted here as well as for one minted in the browser. */
	if (0 != principal->user_id)
		g_object_set(token, "user-id", principal->user_id, NULL);

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
	guint i;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	/*
	 * The queue, not the assistant's queue. A change staged by a
	 * token-authenticated write is listed here beside one the in-process
	 * assistant proposed, and an install with AI switched off still has
	 * the former -- which is why this no longer starts by asking whether
	 * there is an AI service and returning an empty array if not.
	 */
	pending = venture_confirmation_store_list_pending(
		venture_context_get_confirmations(self->context));

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < pending->len; i++)
	{
		json_builder_add_value(builder,
			venture_confirmation_to_json(g_ptr_array_index(pending, i)));
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
	VentureConfirmationStore *store;
	const gchar *id;
	gboolean ok;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	/* Approving a staged write is exactly the authority an editor has, and
	 * exactly what a viewer must not have. */
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	store = venture_context_get_confirmations(self->context);
	id = g_hash_table_lookup(params, "id");

	/*
	 * And the role that record type demands in its own right.
	 *
	 * Approving is an editor's authority in general, but a forge names
	 * the host this install sends its token to and a rule decides what
	 * runs unattended -- the direct routes for those require owner and
	 * admin. Without this check, a change the REST API refuses to an
	 * editor could be made by asking the assistant for it and approving
	 * the result. Staging would launder the authorisation, which is the
	 * opposite of what staging is for.
	 */
	if (approve)
	{
		VentureConfirmation *pending;

		pending = venture_confirmation_store_find(store, id);

		if (NULL != pending)
		{
			GType staged_type;

			staged_type = venture_confirmation_get_entity_type(pending);

			if ((G_TYPE_INVALID != staged_type) &&
			    !venture_web_require_for_type(self, principal, staged_type,
			                                  VENTURE_USER_ROLE_EDITOR,
			                                  &error))
				return venture_web_error_response(error);
		}
	}

	ok = approve
		? venture_confirmation_store_approve(store, id,
			(NULL != principal) ? principal->name : NULL, &error)
		: venture_confirmation_store_reject(store, id,
			(NULL != principal) ? principal->name : NULL, &error);

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

/* --- Forge integration ---------------------------------------------------- */

/*
 * Builds the client for a forge record.
 *
 * The token is read here and nowhere else. Everything above this point deals
 * in forge ids; everything below deals in requests that already carry the
 * credential, so there is no layer holding a token it does not immediately
 * use.
 */
static VentureForgeClient *
venture_web_forge_client(
	VentureWebServer	 *self,
	VentureForge		 *forge,
	GError			**error
){
	gint64 timeout = 30;

	g_object_get(venture_context_get_config(self->context),
	             "forge-request-timeout", &timeout, NULL);

	return venture_forge_client_for_forge(forge, (gint)timeout, error);
}

static VentureForge *
venture_web_forge_load(
	VentureWebServer	 *self,
	GHashTable		 *params,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	const gchar *raw;
	gint64 id;

	raw = g_hash_table_lookup(params, "id");
	id = (NULL != raw) ? g_ascii_strtoll(raw, NULL, 10) : 0;

	record = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_FORGE, id, error);

	if (NULL == record)
		return NULL;

	return VENTURE_FORGE(g_steal_pointer(&record));
}

/*
 * POST /forges/:id/token - set the access token.
 *
 * A route of its own for the same reason a password has one: the field is
 * sensitive, so the generated form omits it and the generated save ignores
 * it. A value typed into a box that does not exist cannot be saved. This is
 * the box.
 *
 * Owner-only, and that is not caution. An editor who could set the token
 * could also change base-url, and the pair of those is "send this
 * credential to a host I control".
 */
static HtmxResponse *
venture_web_ui_forge_token(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *token;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER,
	                          &error))
		return venture_web_error_response(error);

	forge = venture_web_forge_load(self, params, &error);

	if (NULL == forge)
		return venture_web_redirect_to("/e/forge");

	token = htmx_request_get_form_value(request, "token");

	/*
	 * An empty box leaves the existing token alone. Clearing a
	 * credential is a deliberate act with its own control, because a
	 * blank field submitted by accident must not silently disconnect a
	 * forge and turn every later call into an authentication failure
	 * nobody can explain.
	 */
	if (!venture_string_is_empty(token))
	{
		now = g_date_time_new_now_utc();
		g_object_set(forge, "token", token, "token-set-at", now, NULL);

		venture_auth_to_actor(principal, &actor);

		if (!venture_database_save(venture_context_get_database(self->context),
		                           VENTURE_ENTITY(forge), &actor, &error))
			return venture_web_error_response(error);
	}

	destination = g_strdup_printf("/e/forge/%" G_GINT64_FORMAT,
	                              venture_entity_get_id(VENTURE_ENTITY(forge)));

	return venture_web_redirect_to(destination);
}

/*
 * POST /forges/:id/secret - set or generate the webhook secret.
 *
 * Generating rather than typing is offered first because this secret is
 * never remembered by a person: it is pasted into the forge once and then
 * only ever compared. A generated one comes from the same source as an API
 * token, which is the system CSPRNG rather than anything guessable.
 */
static HtmxResponse *
venture_web_ui_forge_secret(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *generated = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *secret;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER,
	                          &error))
		return venture_web_error_response(error);

	forge = venture_web_forge_load(self, params, &error);

	if (NULL == forge)
		return venture_web_redirect_to("/e/forge");

	secret = htmx_request_get_form_value(request, "secret");

	if (venture_string_is_empty(secret))
	{
		generated = venture_generate_token(32);
		secret = generated;
	}

	now = g_date_time_new_now_utc();
	g_object_set(forge, "webhook-secret", secret,
	             "webhook-secret-set-at", now, NULL);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(forge), &actor, &error))
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/forge/%" G_GINT64_FORMAT "?secret=%s",
	                              venture_entity_get_id(VENTURE_ENTITY(forge)),
	                              secret);

	return venture_web_redirect_to(destination);
}

/*
 * POST /forges/:id/verify - ask the forge who the token belongs to.
 *
 * The answer is stored as bot-username, and it is not cosmetic: it is the
 * webhook loop guard. An event whose sender is this account was caused by
 * VENTURE itself, and without the login there is no way to tell VENTURE's
 * own issue from one somebody else opened.
 */
static HtmxResponse *
venture_web_ui_forge_verify(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(VentureForgeClient) client = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *login = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	VentureActor actor;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER,
	                          &error))
		return venture_web_error_response(error);

	forge = venture_web_forge_load(self, params, &error);

	if (NULL == forge)
		return venture_web_redirect_to("/e/forge");

	client = venture_web_forge_client(self, forge, &error);

	if (NULL == client)
		return venture_web_error_response(error);

	login = venture_forge_client_whoami(client, &error);

	if (NULL == login)
		return venture_web_error_response(error);

	now = g_date_time_new_now_utc();
	g_object_set(forge, "bot-username", login, "verified-at", now, NULL);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(forge), &actor, &error))
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/forge/%" G_GINT64_FORMAT,
	                              venture_entity_get_id(VENTURE_ENTITY(forge)));

	return venture_web_redirect_to(destination);
}

/*
 * Finds the link joining a ticket to a repository, if there is one.
 */
static VentureTicketLink *
venture_web_forge_find_link(
	VentureWebServer	*self,
	gint64			 ticket_id,
	gint64			 repo_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) links = NULL;

	query = venture_query_new(VENTURE_TYPE_TICKET_LINK);

	if (!venture_query_add_filter_int(query, "ticket-id", VENTURE_FILTER_OP_EQ,
	                                  ticket_id, NULL))
		return NULL;

	if ((0 != repo_id) &&
	    !venture_query_add_filter_int(query, "repo-id", VENTURE_FILTER_OP_EQ,
	                                  repo_id, NULL))
		return NULL;

	venture_query_set_limit(query, 1);

	links = venture_database_find(venture_context_get_database(self->context),
	                              query, NULL);

	if ((NULL == links) || (0 == links->len))
		return NULL;

	return VENTURE_TICKET_LINK(g_object_ref(g_ptr_array_index(links, 0)));
}


/*
 * POST /tickets/:id/relate - point a ticket at any other record.
 *
 * The type and id arrive as text from a form, so the work is all in
 * refusing what cannot mean anything; venture_ticket_relation_create() does
 * that and this route reports it.
 */
static HtmxResponse *
venture_web_ui_ticket_relate(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureTicketRelation) relation = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *subject_type;
	const gchar *subject_id;
	const gchar *note;
	gint64 ticket_id;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	destination = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	subject_type = htmx_request_get_form_value(request, "subject_type");
	subject_id = htmx_request_get_form_value(request, "subject_id");
	note = htmx_request_get_form_value(request, "note");

	relation = venture_ticket_relation_create(
		venture_context_get_database(self->context), ticket_id, subject_type,
		(NULL != subject_id) ? g_ascii_strtoll(subject_id, NULL, 10) : 0,
		note, &error);

	if (NULL == relation)
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(relation), &actor, &error))
		return venture_web_error_response(error);

	return venture_web_redirect_to(destination);
}

/*
 * POST /relations/:id/delete - drop one.
 *
 * Its own route rather than the generic record delete, so the redirect goes
 * back to the ticket the person was looking at rather than to a list of
 * relations, which is not a page anybody wants.
 */
static HtmxResponse *
venture_web_ui_relation_delete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) relation = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	gint64 ticket_id = 0;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	relation = venture_database_get(venture_context_get_database(self->context),
		VENTURE_TYPE_TICKET_RELATION,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);

	if (NULL == relation)
		return venture_web_error_response(error);

	g_object_get(relation, "ticket-id", &ticket_id, NULL);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_delete(venture_context_get_database(self->context),
	                             relation, &actor, &error))
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	return venture_web_redirect_to(destination);
}

/*
 * POST /tickets/:id/link - file this ticket as an issue upstream.
 *
 * The link row is written before the issue is created and updated after,
 * rather than only once at the end. That ordering matters: the forge assigns
 * the issue number, so between the request and the reply there is a window
 * in which the forge has already delivered a webhook for an issue VENTURE
 * has no record of. The row existing -- even with number 0 -- is not what
 * closes that window; the sender check is. But it does mean a failure
 * halfway leaves evidence rather than nothing.
 */
static HtmxResponse *
venture_web_ui_ticket_link(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) repo = NULL;
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(VentureForgeClient) client = NULL;
	g_autoptr(VentureTicketLink) link = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *repo_name = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *description = NULL;
	g_autofree gchar *issue_url = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	gint64 ticket_id;
	gint64 repo_id = 0;
	gint64 forge_id = 0;
	gint64 number = 0;
	gboolean push_issues = FALSE;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	ticket = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_TICKET, ticket_id, &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	g_object_get(ticket, "repo-id", &repo_id, NULL);

	if (0 == repo_id)
		return venture_web_redirect_to(destination);

	repo = venture_database_get(venture_context_get_database(self->context),
	                            VENTURE_TYPE_FORGE_REPO, repo_id, &error);

	if (NULL == repo)
		return venture_web_error_response(error);

	g_object_get(repo, "forge-id", &forge_id, "name", &repo_name,
	             "push-issues", &push_issues, NULL);

	link = venture_web_forge_find_link(self, ticket_id, repo_id);

	if (NULL == link)
	{
		link = venture_ticket_link_new();
		g_object_set(link,
		             "ticket-id", ticket_id,
		             "repo-id", repo_id,
		             "origin", VENTURE_FORGE_LINK_ORIGIN_VENTURE,
		             NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(link),
			venture_entity_get_organization_id(ticket));
	}

	g_object_get(link, "issue-number", &number, NULL);

	venture_auth_to_actor(principal, &actor);

	/* Already filed, or the repository does not want issues pushed to it.
	 * Either way the link is the deliverable. */
	if ((0 != number) || !push_issues)
	{
		if (!venture_database_save(venture_context_get_database(self->context),
		                           VENTURE_ENTITY(link), &actor, &error))
			return venture_web_error_response(error);

		return venture_web_redirect_to(destination);
	}

	forge = VENTURE_FORGE(venture_database_get(
		venture_context_get_database(self->context), VENTURE_TYPE_FORGE,
		forge_id, &error));

	if (NULL == forge)
		return venture_web_error_response(error);

	client = venture_web_forge_client(self, forge, &error);

	if (NULL == client)
		return venture_web_error_response(error);

	g_object_get(ticket, "title", &title, "description", &description, NULL);

	if (!venture_forge_client_create_issue(client, repo_name, title,
	                                       description, NULL, &number,
	                                       &issue_url, &error))
		return venture_web_error_response(error);

	now = g_date_time_new_now_utc();
	g_object_set(link,
	             "issue-number", number,
	             "issue-url", issue_url,
	             "synced-at", now,
	             NULL);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(link), &actor, &error))
		return venture_web_error_response(error);

	return venture_web_redirect_to(destination);
}

/*
 * POST /tickets/:id/branch - cut the branch for this ticket.
 *
 * Through the forge's API rather than a checkout, deliberately. Creating a
 * branch is one request; doing it with git would mean a clone, which is
 * minutes of network and a working tree to clean up for something the forge
 * will do atomically. It also means this works in a container that has no
 * git at all.
 */
static HtmxResponse *
venture_web_ui_ticket_branch(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) repo = NULL;
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(VentureForgeClient) client = NULL;
	g_autoptr(VentureForgeRule) rule = NULL;
	g_autoptr(VentureTicketLink) link = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *repo_name = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *default_branch = NULL;
	g_autofree gchar *rule_template = NULL;
	g_autofree gchar *rule_base = NULL;
	g_autofree gchar *branch = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	VentureIssueType issue_type = VENTURE_ISSUE_TYPE_TASK;
	gint64 ticket_id;
	gint64 repo_id = 0;
	gint64 forge_id = 0;
	gint64 number = 0;
	gboolean exists = FALSE;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	ticket = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_TICKET, ticket_id, &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	g_object_get(ticket, "repo-id", &repo_id, "issue-type", &issue_type,
	             "title", &title, NULL);

	if (0 == repo_id)
		return venture_web_redirect_to(destination);

	repo = venture_database_get(venture_context_get_database(self->context),
	                            VENTURE_TYPE_FORGE_REPO, repo_id, &error);

	if (NULL == repo)
		return venture_web_error_response(error);

	g_object_get(repo, "forge-id", &forge_id, "name", &repo_name,
	             "branch-prefix", &prefix, "default-branch", &default_branch,
	             NULL);

	rule = venture_forge_rule_resolve(venture_context_get_database(self->context),
	                                  repo_id, issue_type, NULL);

	if (NULL != rule)
	{
		g_object_get(rule, "branch-template", &rule_template,
		             "base-branch", &rule_base, NULL);
	}

	link = venture_web_forge_find_link(self, ticket_id, repo_id);

	if (NULL != link)
		g_object_get(link, "issue-number", &number, NULL);

	branch = venture_forge_branch_name(rule_template, prefix, issue_type,
	                                   ticket_id, number, title);

	forge = VENTURE_FORGE(venture_database_get(
		venture_context_get_database(self->context), VENTURE_TYPE_FORGE,
		forge_id, &error));

	if (NULL == forge)
		return venture_web_error_response(error);

	client = venture_web_forge_client(self, forge, &error);

	if (NULL == client)
		return venture_web_error_response(error);

	if (!venture_forge_client_branch_exists(client, repo_name, branch,
	                                        &exists, &error))
		return venture_web_error_response(error);

	if (!exists)
	{
		const gchar *base;

		base = !venture_string_is_empty(rule_base) ? rule_base
		                                           : default_branch;

		if (!venture_forge_client_create_branch(client, repo_name, branch,
		                                        base, &error))
			return venture_web_error_response(error);
	}

	if (NULL == link)
	{
		link = venture_ticket_link_new();
		g_object_set(link,
		             "ticket-id", ticket_id,
		             "repo-id", repo_id,
		             "origin", VENTURE_FORGE_LINK_ORIGIN_VENTURE,
		             NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(link),
			venture_entity_get_organization_id(ticket));
	}

	now = g_date_time_new_now_utc();
	g_object_set(link, "branch", branch, "synced-at", now, NULL);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(link), &actor, &error))
		return venture_web_error_response(error);

	return venture_web_redirect_to(destination);
}

/*
 * Maps an issue's labels onto an issue type.
 *
 * A forge that already labels things "bug" and "epic" works with no
 * configuration at all, which is most of them. Anything unrecognised stays a
 * task rather than guessing.
 */
static VentureIssueType
venture_web_forge_type_from_labels(const GStrv labels)
{
	gsize i;

	if (NULL == labels)
		return VENTURE_ISSUE_TYPE_TASK;

	for (i = 0; NULL != labels[i]; i++)
	{
		gint value;

		if (venture_enum_from_nick(VENTURE_TYPE_ISSUE_TYPE, labels[i],
		                           &value))
			return (VentureIssueType)value;
	}

	return VENTURE_ISSUE_TYPE_TASK;
}

static VentureForgeRepo *
venture_web_forge_repo_by_name(
	VentureWebServer	*self,
	gint64			 forge_id,
	const gchar		*full_name
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) repos = NULL;

	query = venture_query_new(VENTURE_TYPE_FORGE_REPO);

	if (!venture_query_add_filter_int(query, "forge-id", VENTURE_FILTER_OP_EQ,
	                                  forge_id, NULL))
		return NULL;

	if (!venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ,
	                                     full_name, NULL))
		return NULL;

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL))
		return NULL;

	venture_query_set_limit(query, 1);

	repos = venture_database_find(venture_context_get_database(self->context),
	                              query, NULL);

	if ((NULL == repos) || (0 == repos->len))
		return NULL;

	return VENTURE_FORGE_REPO(g_object_ref(g_ptr_array_index(repos, 0)));
}

static VentureTicketLink *
venture_web_forge_link_by_issue(
	VentureWebServer	*self,
	gint64			 repo_id,
	gint64			 issue_number
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) links = NULL;

	query = venture_query_new(VENTURE_TYPE_TICKET_LINK);

	if (!venture_query_add_filter_int(query, "repo-id", VENTURE_FILTER_OP_EQ,
	                                  repo_id, NULL))
		return NULL;

	if (!venture_query_add_filter_int(query, "issue-number",
	                                  VENTURE_FILTER_OP_EQ, issue_number, NULL))
		return NULL;

	venture_query_set_limit(query, 1);

	links = venture_database_find(venture_context_get_database(self->context),
	                              query, NULL);

	if ((NULL == links) || (0 == links->len))
		return NULL;

	return VENTURE_TICKET_LINK(g_object_ref(g_ptr_array_index(links, 0)));
}

/* A bare status with no body, for the many events that are simply not ours. */
static HtmxResponse *
venture_web_forge_ack(guint status)
{
	HtmxResponse *response;

	response = htmx_response_new();
	htmx_response_set_status(response, (gint)status);

	return response;
}


/*
 * POST /tickets/:id/work - start a coding run.
 *
 * Queues and returns. The run has not started when the browser gets its
 * redirect and will not have started for some time after -- which is the
 * entire reason the work service owns a thread.
 */
static HtmxResponse *
venture_web_ui_ticket_work(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	VentureWorkService *work;
	HtmxResponse *redirect;
	gint64 ticket_id;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	destination = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	work = venture_context_get_work_service(self->context);

	/* Turned off is the default. Reported rather than crashed into. */
	if (NULL == work)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Coding runs are turned off in settings");
		return venture_web_error_response(error);
	}

	if (0 == venture_work_service_start_for_ticket(work, ticket_id,
	                                               principal->name, &error))
		return venture_web_error_response(error);

	return venture_web_redirect_to(destination);
}

/*
 * POST /runs/:id/cancel - stop a run.
 *
 * Always allowed and always idempotent: somebody who wants a run stopped
 * should not have to know how far it got, and a button that greys itself out
 * on a run the browser merely believes has finished is a button that cannot
 * stop the run that is still going.
 */
static HtmxResponse *
venture_web_ui_run_cancel(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	VentureWorkService *work;
	HtmxResponse *redirect;
	gint64 run_id;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	run_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);

	work = venture_context_get_work_service(self->context);

	if (NULL != work)
		venture_work_service_cancel(work, run_id);

	destination = g_strdup_printf("/e/forge_run/%" G_GINT64_FORMAT, run_id);

	return venture_web_redirect_to(destination);
}

/*
 * GET /tickets/:id/runs - the run card, polled.
 *
 * A fragment rather than a page, and it re-renders its own hx-trigger. Once
 * the run reaches a terminal state the attribute is left out, so the
 * fragment becomes inert HTML and the browser stops asking. Without that a
 * tab left open overnight would poll a finished run until somebody closed
 * it.
 */
static HtmxResponse *
venture_web_ui_ticket_runs(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) runs = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *denied;
	gint64 ticket_id;
	gint64 interval = 3;
	gboolean live = FALSE;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);

	g_object_get(venture_context_get_config(self->context),
	             "forge-poll-interval", &interval, NULL);

	query = venture_query_new(VENTURE_TYPE_FORGE_RUN);

	if (!venture_query_add_filter_int(query, "ticket-id", VENTURE_FILTER_OP_EQ,
	                                  ticket_id, &error))
		return venture_web_error_response(error);

	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 5);

	runs = venture_database_find(venture_context_get_database(self->context),
	                             query, &error);

	if (NULL == runs)
		return venture_web_error_response(error);

	content = g_string_new(NULL);
	g_string_append_printf(content, "<div id=\"ticket-runs-%" G_GINT64_FORMAT
	                                "\"", ticket_id);

	{
		guint i;

		for (i = 0; i < runs->len; i++)
		{
			VentureForgeRunState state;

			g_object_get(g_ptr_array_index(runs, i), "state", &state, NULL);

			if ((VENTURE_FORGE_RUN_STATE_QUEUED == state) ||
			    (VENTURE_FORGE_RUN_STATE_RUNNING == state))
				live = TRUE;
		}
	}

	if (live)
	{
		g_string_append_printf(content,
			" hx-get=\"/tickets/%" G_GINT64_FORMAT "/runs\""
			" hx-trigger=\"every %" G_GINT64_FORMAT "s\""
			" hx-swap=\"outerHTML\"", ticket_id, interval);
	}

	g_string_append(content, ">");

	if (0 == runs->len)
	{
		g_string_append(content, "<p class=\"muted\">No runs yet.</p>");
	}
	else
	{
		guint i;

		g_string_append(content, "<ul class=\"run-list\">");

		for (i = 0; i < runs->len; i++)
		{
			VentureEntity *run = g_ptr_array_index(runs, i);
			g_autofree gchar *branch = NULL;
			g_autofree gchar *summary = NULL;
			g_autofree gchar *failure = NULL;
			VentureForgeRunState state;

			g_object_get(run, "state", &state, "branch", &branch,
			             "summary", &summary, "failure-reason", &failure,
			             NULL);

			g_string_append(content, "<li><span class=\"badge\">");
			venture_html_escape_append(content,
				venture_enum_to_nick(VENTURE_TYPE_FORGE_RUN_STATE, state));
			g_string_append(content, "</span> ");

			if (!venture_string_is_empty(branch))
			{
				g_string_append(content, "<code>");
				venture_html_escape_append(content, branch);
				g_string_append(content, "</code> ");
			}

			if (!venture_string_is_empty(failure))
				venture_html_escape_append(content, failure);
			else if (!venture_string_is_empty(summary))
				venture_html_escape_append(content, summary);

			g_string_append(content, "</li>");
		}

		g_string_append(content, "</ul>");
	}

	g_string_append(content, "</div>");

	return venture_web_html_response(g_strdup(content->str), 200);
}



/*
 * The credential routes, in their API form.
 *
 * Deliberately forge-specific rather than a generic "set any sensitive
 * field on any type" endpoint. That generic version would let a caller
 * write user.password-hash directly -- a raw hash, bypassing the hashing
 * that makes storing one safe at all. A credential is not a field with a
 * flag on it; each one has its own way of being set correctly, and this is
 * the forge's.
 *
 * Owner-only, matching the UI route and the REST route for the record
 * itself: an editor who could set the token could also point base-url at a
 * host they control, and the pair of those is "send this credential
 * somewhere I can read it".
 */
static VentureForge *
venture_web_api_forge_for_credential(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	GHashTable		 *params,
	HtmxResponse		**out_denied,
	GError			**error
){
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	const gchar *id_text;

	*out_denied = venture_web_api_require(self, request,
	                                     VENTURE_USER_ROLE_OWNER);

	if (NULL != *out_denied)
		return NULL;

	(void)principal;

	id_text = g_hash_table_lookup(params, "id");
	record = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_FORGE,
	                              (NULL != id_text)
	                                      ? g_ascii_strtoll(id_text, NULL, 10)
	                                      : 0,
	                              error);

	if (NULL == record)
	{
		if ((NULL != error) && (NULL == *error))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "No such forge: %s",
			            (NULL != id_text) ? id_text : "");
		}

		return NULL;
	}

	return VENTURE_FORGE(g_steal_pointer(&record));
}

/* Reads a single string member out of a JSON request body. */
static gchar *
venture_web_api_body_string(
	HtmxRequest	*request,
	const gchar	*member
){
	g_autoptr(JsonParser) parser = NULL;
	GBytes *body;
	gconstpointer data;
	gsize length = 0;
	JsonNode *root;

	body = htmx_request_get_body_bytes(request);

	if (NULL == body)
		return NULL;

	data = g_bytes_get_data(body, &length);

	if ((NULL == data) || (0 == length))
		return NULL;

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, data, (gssize)length, NULL))
		return NULL;

	root = json_parser_get_root(parser);

	if ((NULL == root) || (JSON_NODE_OBJECT != json_node_get_node_type(root)))
		return NULL;

	return g_strdup(venture_json_object_get_string(json_node_get_object(root),
	                                               member, NULL));
}

/*
 * POST /api/v1/forge/:id/token - set the access token from a script.
 */
static HtmxResponse *
venture_web_api_forge_token(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *denied = NULL;
	VentureActor actor;

	forge = venture_web_api_forge_for_credential(self, request, params,
	                                             &denied, &error);

	if (NULL != denied)
		return denied;

	if (NULL == forge)
		return venture_web_error_response(error);

	token = venture_web_api_body_string(request, "token");

	/*
	 * An empty token is refused rather than treated as "leave it alone".
	 * The UI form has that behaviour because a blank box is usually a
	 * mistake; a script that sent an empty string meant to send
	 * something and its variable was unset, and silently succeeding
	 * there leaves a forge that will fail every later call for reasons
	 * nobody can trace back to here.
	 */
	if (venture_string_is_empty(token))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "A \"token\" is required and may not be empty");
		return venture_web_error_response(error);
	}

	now = g_date_time_new_now_utc();
	g_object_set(forge, "token", token, "token-set-at", now, NULL);

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(forge), &actor, &error))
		return venture_web_error_response(error);

	/* The value is never echoed, only the fact and the time. */
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "forge_id");
	json_builder_add_int_value(builder,
	                           venture_entity_get_id(VENTURE_ENTITY(forge)));
	json_builder_set_member_name(builder, "token_set");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(g_steal_pointer(&node), 200);
}

/*
 * POST /api/v1/forge/:id/webhook-secret - set or generate it.
 *
 * An absent or empty secret generates one and returns it. This is the only
 * response in VENTURE that carries a credential, and it does so for the same
 * reason minting an API token does: the value has to reach the operator
 * once, to be pasted into the forge, and it is never recoverable
 * afterwards.
 */
static HtmxResponse *
venture_web_api_forge_secret(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *generated = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *denied = NULL;
	VentureActor actor;
	gboolean was_generated = FALSE;

	forge = venture_web_api_forge_for_credential(self, request, params,
	                                             &denied, &error);

	if (NULL != denied)
		return denied;

	if (NULL == forge)
		return venture_web_error_response(error);

	secret = venture_web_api_body_string(request, "secret");

	if (venture_string_is_empty(secret))
	{
		generated = venture_generate_token(32);
		g_clear_pointer(&secret, g_free);
		secret = g_strdup(generated);
		was_generated = TRUE;
	}

	now = g_date_time_new_now_utc();
	g_object_set(forge, "webhook-secret", secret,
	             "webhook-secret-set-at", now, NULL);

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(forge), &actor, &error))
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "forge_id");
	json_builder_add_int_value(builder,
	                           venture_entity_get_id(VENTURE_ENTITY(forge)));
	json_builder_set_member_name(builder, "secret_set");
	json_builder_add_boolean_value(builder, TRUE);

	/* Returned only when VENTURE chose it, and only this once. A secret
	 * the caller supplied is one they already have. */
	if (was_generated)
	{
		json_builder_set_member_name(builder, "secret");
		json_builder_add_string_value(builder, secret);
	}

	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(g_steal_pointer(&node), 200);
}

/*
 * POST /api/v1/forge/:id/verify - ask the forge who the token belongs to.
 */
static HtmxResponse *
venture_web_api_forge_verify(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(VentureForgeClient) client = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *login = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *denied = NULL;
	VentureActor actor;

	forge = venture_web_api_forge_for_credential(self, request, params,
	                                             &denied, &error);

	if (NULL != denied)
		return denied;

	if (NULL == forge)
		return venture_web_error_response(error);

	client = venture_web_forge_client(self, forge, &error);

	if (NULL == client)
		return venture_web_error_response(error);

	login = venture_forge_client_whoami(client, &error);

	if (NULL == login)
		return venture_web_error_response(error);

	now = g_date_time_new_now_utc();
	g_object_set(forge, "bot-username", login, "verified-at", now, NULL);

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(forge), &actor, &error))
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "forge_id");
	json_builder_add_int_value(builder,
	                           venture_entity_get_id(VENTURE_ENTITY(forge)));
	json_builder_set_member_name(builder, "bot_username");
	json_builder_add_string_value(builder, login);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(g_steal_pointer(&node), 200);
}

/*
 * POST /api/v1/:type/:id/restore - bring a soft-deleted record back.
 *
 * The counterpart to delete, which reports that the deletion "is
 * recoverable" -- and until this existed, nothing could recover it. Nothing
 * is ever really removed here: deletion stamps a time, so restoring is
 * clearing that stamp rather than reconstructing anything.
 *
 * Finding the record needs a query that includes deleted rows, because
 * venture_database_get() filters them out -- which is right for every other
 * caller and exactly wrong for this one.
 */
static HtmxResponse *
venture_web_api_restore(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureEntity *record;
	VentureActor actor;
	GType entity_type;
	const gchar *id_text;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	/* A type that refuses writes refuses this one too: restoring an audit
	 * entry or a run record is still editing the record of what
	 * happened. */
	if (!venture_web_type_accepts_writes(entity_type, &error))
		return venture_web_error_response(error);

	id_text = g_hash_table_lookup(params, "id");

	query = venture_query_new(entity_type);
	venture_query_set_include_deleted(query, TRUE);

	if (!venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ,
	                                  g_ascii_strtoll(id_text, NULL, 10),
	                                  &error))
		return venture_web_error_response(error);

	venture_query_set_limit(query, 1);

	found = venture_database_find(venture_context_get_database(self->context),
	                              query, &error);

	if (NULL == found)
		return venture_web_error_response(error);

	if (0 == found->len)
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No such record: %s", id_text);
		return venture_web_error_response(error);
	}

	record = g_ptr_array_index(found, 0);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_restore(venture_context_get_database(self->context),
	                              record, &actor, &error))
		return venture_web_error_response(error);

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(record), FALSE);

	return venture_web_json_response(g_steal_pointer(&node), 200);
}

/*
 * POST /hooks/forge/:id - inbound from a forge.
 *
 * The only route in VENTURE that does not require a session, because the
 * caller is a forge and has neither a cookie nor a token. What replaces that
 * is an HMAC over the request body keyed by a secret only this install and
 * that forge know, and there is no path past it: every early return below
 * the verification is a refusal.
 *
 * It lives under /hooks rather than /api because the not-found middleware
 * turns an unknown /api path into a JSON 404, and /api is where a reader
 * reasonably assumes venture_web_api_require() is somewhere above. This is a
 * third surface and its path says so.
 *
 * The handler verifies, records, and returns. It does not do the work: a
 * forge gives a webhook a few seconds before it calls the delivery failed
 * and retries, and an AI run takes minutes.
 */
static HtmxResponse *
venture_web_forge_webhook(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(VentureForgeClient) client = NULL;
	g_autoptr(VentureForgeRepo) repo = NULL;
	g_autoptr(VentureTicketLink) link = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *bot = NULL;
	g_autofree gchar *existing_delivery = NULL;
	g_autofree gchar *existing_title = NULL;
	g_autofree gchar *existing_body = NULL;
	VentureForgeEvent event;
	VentureActor actor;
	SoupMessageHeaders *headers;
	GBytes *body;
	gconstpointer data;
	gsize length = 0;
	gint64 max_bytes = 32;
	gint64 repo_id;
	gboolean accept_issues = FALSE;
	gboolean enabled = FALSE;
	gboolean created = FALSE;
	const gchar *event_name;

	memset(&event, 0, sizeof(event));

	g_object_get(venture_context_get_config(self->context),
	             "forge-webhooks-enabled", &enabled,
	             "server-max-request-size-mb", &max_bytes, NULL);

	if (!enabled)
		return venture_web_forge_ack(SOUP_STATUS_NOT_FOUND);

	forge = venture_web_forge_load(self, params, NULL);

	/*
	 * A forge that does not exist and a forge that does are answered the
	 * same way. Distinguishing them would let anybody enumerate which
	 * forge ids this install has, and the answer is 401 rather than 404
	 * because the caller is a machine that should retry with a signature,
	 * not a browser that should be sent to a login page.
	 */
	if (NULL == forge)
		return venture_web_forge_ack(SOUP_STATUS_UNAUTHORIZED);

	g_object_get(forge, "webhook-secret", &secret, "bot-username", &bot, NULL);

	headers = soup_server_message_get_request_headers(
		htmx_request_get_message(request));

	/*
	 * The raw bytes, not htmx_request_get_body().
	 *
	 * That accessor is a g_strndup, so a payload containing a NUL byte
	 * would be truncated at it and the HMAC computed over a prefix --
	 * failing for exactly those deliveries and succeeding for every
	 * other, which is close to undiagnosable.
	 */
	body = htmx_request_get_body_bytes(request);

	if (NULL != body)
		data = g_bytes_get_data(body, &length);
	else
		data = NULL;

	/*
	 * The size cap is enforced here rather than assumed.
	 * server.max_request_size_mb is declared in the configuration but
	 * nothing in this tree reads it, so treating it as already applied
	 * would leave this route -- the one an unauthenticated caller can
	 * reach -- with no bound at all.
	 */
	if (length > (gsize)(max_bytes * 1024 * 1024))
		return venture_web_forge_ack(SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);

	client = venture_web_forge_client(self, forge, &error);

	if (NULL == client)
		return venture_web_forge_ack(SOUP_STATUS_UNAUTHORIZED);

	/* Everything below this line has been proved to come from the forge. */
	if (!venture_forge_client_verify_webhook(client, headers, body, secret,
	                                         &error))
	{
		g_warning("venture_forge: refusing a webhook for forge #%"
		          G_GINT64_FORMAT ": %s",
		          venture_entity_get_id(VENTURE_ENTITY(forge)),
		          (NULL != error) ? error->message : "unverified");

		return venture_web_forge_ack(SOUP_STATUS_UNAUTHORIZED);
	}

	event_name = soup_message_headers_get_one(headers, "X-Forgejo-Event");

	if (venture_string_is_empty(event_name))
		event_name = soup_message_headers_get_one(headers, "X-Gitea-Event");

	/*
	 * Anything that is not an issue is acknowledged and dropped, not
	 * rejected. A forge disables a webhook that keeps erroring, so a
	 * repository configured to send every event would switch itself off.
	 */
	if ((0 != g_strcmp0(event_name, "issues")) &&
	    (0 != g_strcmp0(event_name, "issue_comment")))
		return venture_web_forge_ack(SOUP_STATUS_NO_CONTENT);

	parser = json_parser_new();

	if ((NULL == data) ||
	    !json_parser_load_from_data(parser, data, (gssize)length, NULL))
		return venture_web_forge_ack(SOUP_STATUS_BAD_REQUEST);

	if (!venture_forge_client_parse_issue_event(client,
	                                            json_parser_get_root(parser),
	                                            headers, &event, &error))
	{
		venture_forge_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_BAD_REQUEST);
	}

	/*
	 * Loop guard one, and the important one: this event was caused by
	 * VENTURE's own account.
	 *
	 * VENTURE files an issue, the forge delivers a webhook for it, and
	 * without this the webhook would create a second ticket for the issue
	 * VENTURE just created for the first. It is exact and needs no
	 * stored state, which is what makes it work in the window before the
	 * link row has the issue number written back to it.
	 */
	if (!venture_string_is_empty(bot) &&
	    (0 == g_strcmp0(bot, event.sender)))
	{
		venture_forge_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_NO_CONTENT);
	}

	repo = venture_web_forge_repo_by_name(self,
		venture_entity_get_id(VENTURE_ENTITY(forge)), event.repo_full_name);

	if (NULL == repo)
	{
		/* A repository nobody enrolled. Acknowledged so the forge does
		 * not retry, ignored because there is nothing to attach to. */
		venture_forge_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_NO_CONTENT);
	}

	g_object_get(repo, "accept-issues", &accept_issues, NULL);
	repo_id = venture_entity_get_id(VENTURE_ENTITY(repo));

	if (!accept_issues)
	{
		venture_forge_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_NO_CONTENT);
	}

	link = venture_web_forge_link_by_issue(self, repo_id, event.issue_number);

	/*
	 * Loop guard two: a delivery already applied.
	 *
	 * A forge retries a delivery it believes failed, and a retry that
	 * re-applied its payload would reopen a ticket somebody had just
	 * closed by hand.
	 */
	if (NULL != link)
	{
		g_object_get(link, "last-delivery-id", &existing_delivery, NULL);

		if (!venture_string_is_empty(event.delivery_id) &&
		    (0 == g_strcmp0(existing_delivery, event.delivery_id)))
		{
			venture_forge_event_clear(&event);
			return venture_web_forge_ack(SOUP_STATUS_NO_CONTENT);
		}
	}

	/* Attributed to the automation actor with the forge named, so a
	 * ticket the forge raised is distinguishable in the audit log from
	 * one a person typed. */
	actor.kind = VENTURE_ACTOR_KIND_AUTOMATION;
	actor.name = "forge";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;

	now = g_date_time_new_now_utc();

	if (NULL != link)
	{
		gint64 linked_ticket = 0;

		g_object_get(link, "ticket-id", &linked_ticket, NULL);
		ticket = venture_database_get(
			venture_context_get_database(self->context), VENTURE_TYPE_TICKET,
			linked_ticket, NULL);
	}

	if (NULL == ticket)
	{
		gint64 organization;

		/*
		 * The organisation comes from the repository, never from the
		 * default.
		 *
		 * A webhook carries no session and therefore no active entity.
		 * Falling through to the default organisation would file the
		 * ticket against the wrong business on any install with more
		 * than one -- and it would then be invisible on the board,
		 * which reads as "the webhook did not work" rather than as a
		 * scoping mistake.
		 */
		organization = venture_entity_get_organization_id(
			VENTURE_ENTITY(repo));

		ticket = VENTURE_ENTITY(venture_ticket_new());
		venture_entity_set_organization_id(ticket, organization);

		g_object_set(ticket,
		             "kind", VENTURE_TICKET_KIND_EXTERNAL,
		             "issue-type",
		             venture_web_forge_type_from_labels(event.labels),
		             "status", VENTURE_TICKET_STATUS_TRIAGE,
		             "repo-id", repo_id,
		             NULL);

		created = TRUE;
	}

	g_object_get(ticket, "title", &existing_title, "description",
	             &existing_body, NULL);

	/*
	 * Loop guard three: an edit that changes nothing.
	 *
	 * venture_database_save() returns success without writing when the
	 * diff is empty, so "did the save change anything" is not available
	 * as a signal after the fact. The comparison has to happen before the
	 * write, or an edit VENTURE itself pushed comes back, gets re-applied
	 * and is pushed again.
	 */
	if (!created &&
	    (0 == g_strcmp0(existing_title, event.title)) &&
	    (0 == g_strcmp0(existing_body, event.body)))
	{
		venture_forge_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_NO_CONTENT);
	}

	g_object_set(ticket, "title", event.title, "description", event.body, NULL);

	/*
	 * A closed issue closes the ticket, but an open one does not reopen
	 * it. Somebody moved that ticket into review or blocked deliberately,
	 * and the forge does not get to move it back on the next edit.
	 */
	if (0 == g_strcmp0(event.state, "closed"))
	{
		g_object_set(ticket, "status", VENTURE_TICKET_STATUS_DONE,
		             "resolved-at", now, NULL);
	}

	if (!venture_database_save(venture_context_get_database(self->context),
	                           ticket, &actor, &error))
	{
		venture_forge_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_INTERNAL_SERVER_ERROR);
	}

	if (NULL == link)
	{
		link = venture_ticket_link_new();
		g_object_set(link,
		             "ticket-id", venture_entity_get_id(ticket),
		             "repo-id", repo_id,
		             "issue-number", event.issue_number,
		             "origin", VENTURE_FORGE_LINK_ORIGIN_FORGE,
		             NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(link),
			venture_entity_get_organization_id(ticket));
	}

	g_object_set(link,
	             "issue-url", event.url,
	             "last-delivery-id", event.delivery_id,
	             "synced-at", now,
	             NULL);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(link), &actor, &error))
	{
		venture_forge_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_INTERNAL_SERVER_ERROR);
	}

	/* A comment upstream becomes a comment here, attributed to whoever
	 * left it and visible to whoever raised the ticket. */
	if ((0 == g_strcmp0(event_name, "issue_comment")) &&
	    !venture_string_is_empty(event.comment_body))
	{
		g_autoptr(VentureTicketComment) comment = NULL;

		comment = venture_ticket_comment_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(comment),
			venture_entity_get_organization_id(ticket));
		g_object_set(comment,
		             "ticket-id", venture_entity_get_id(ticket),
		             "body", event.comment_body,
		             "author", event.sender,
		             "internal", FALSE,
		             "occurred-at", now,
		             NULL);

		venture_database_save(venture_context_get_database(self->context),
		                      VENTURE_ENTITY(comment), &actor, NULL);
	}

	venture_forge_event_clear(&event);

	/* Accepted, not completed. */
	return venture_web_forge_ack(SOUP_STATUS_ACCEPTED);
}

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

	/* The catch-all 404, wrapped around every route below. */
	htmx_server_use(self->server, venture_web_not_found_middleware, self,
	                NULL);

	/* UI */
	htmx_router_get(router, "/", venture_web_ui_dashboard, self);
	htmx_router_get(router, "/search", venture_web_ui_search, self);
	htmx_router_get(router, "/automations", venture_web_ui_automations, self);
	htmx_router_post(router, "/automations/validate",
	                 venture_web_ui_automations_validate, self);
	htmx_router_post(router, "/automations/save",
	                 venture_web_ui_automations_save, self);
	htmx_router_post(router, "/automations/reload",
	                 venture_web_ui_automations_reload, self);
	htmx_router_get(router, "/plugins", venture_web_ui_plugins, self);
	htmx_router_post(router, "/plugins/config",
	                 venture_web_ui_plugins_config, self);
	htmx_router_post(router, "/invoices/:id/status",
	                 venture_web_ui_invoice_status, self);
	htmx_router_get(router, "/invoices/:id/print",
	                 venture_web_ui_invoice_print, self);
	htmx_router_get(router, "/reports", venture_web_ui_reports, self);
	htmx_router_get(router, "/settings", venture_web_ui_settings, self);
	htmx_router_get(router, "/entity/:id", venture_web_ui_switch_entity, self);
	htmx_router_get(router, "/tickets", venture_web_ui_tickets, self);
	htmx_router_post(router, "/tickets/:id/move", venture_web_ui_ticket_move,
	                 self);
	/* Forge actions on a ticket, with the other ticket actions so they
	 * precede /e/:type/:id. */
	htmx_router_post(router, "/tickets/:id/relate",
	                 venture_web_ui_ticket_relate, self);
	htmx_router_post(router, "/relations/:id/delete",
	                 venture_web_ui_relation_delete, self);
	htmx_router_post(router, "/tickets/:id/link", venture_web_ui_ticket_link,
	                 self);
	htmx_router_post(router, "/tickets/:id/branch",
	                 venture_web_ui_ticket_branch, self);
	htmx_router_post(router, "/tickets/:id/work", venture_web_ui_ticket_work,
	                 self);
	htmx_router_get(router, "/tickets/:id/runs", venture_web_ui_ticket_runs,
	                self);
	htmx_router_post(router, "/runs/:id/cancel", venture_web_ui_run_cancel,
	                 self);

	/* Credentials, the same shape as /users/:id/password. */
	htmx_router_post(router, "/forges/:id/token", venture_web_ui_forge_token,
	                 self);
	htmx_router_post(router, "/forges/:id/secret", venture_web_ui_forge_secret,
	                 self);
	htmx_router_post(router, "/forges/:id/verify", venture_web_ui_forge_verify,
	                 self);

	/*
	 * Inbound from a forge. The only route here that does not require a
	 * session -- see the handler for what stands in for one, and why it
	 * is not under /api.
	 */
	htmx_router_post(router, "/hooks/forge/:id", venture_web_forge_webhook,
	                 self);

	htmx_router_post(router, "/tickets/:id/comment",
	                 venture_web_ui_ticket_comment, self);
	htmx_router_get(router, "/entities", venture_web_ui_entities, self);
	htmx_router_post(router, "/entities", venture_web_ui_entities_create, self);
	htmx_router_post(router, "/entities/:id/default",
	                 venture_web_ui_entities_default, self);
	htmx_router_post(router, "/entities/:id/delete",
	                 venture_web_ui_entities_delete, self);
	htmx_router_get(router, "/account", venture_web_ui_account, self);
	htmx_router_post(router, "/account/password",
	                 venture_web_ui_account_password, self);
	htmx_router_get(router, "/account/tokens", venture_web_ui_tokens, self);
	htmx_router_post(router, "/account/tokens", venture_web_ui_tokens_create,
	                 self);
	htmx_router_post(router, "/account/tokens/:id/revoke",
	                 venture_web_ui_tokens_revoke, self);
	htmx_router_get(router, "/users", venture_web_ui_users, self);
	htmx_router_post(router, "/users", venture_web_ui_users_create, self);
	htmx_router_post(router, "/users/:id/update",
	                 venture_web_ui_users_update, self);
	htmx_router_post(router, "/users/:id/password",
	                 venture_web_ui_users_password, self);
	htmx_router_get(router, "/reports/:name", venture_web_ui_report, self);
	htmx_router_get(router, "/e/:type", venture_web_ui_list, self);
	/* Before /e/:type/:id, or "export" would be parsed as a record id. */
	htmx_router_get(router, "/e/:type/export", venture_web_ui_export, self);
	htmx_router_get(router, "/e/:type/import", venture_web_ui_import_form,
	                self);
	htmx_router_get(router, "/e/:type/import/template",
	                venture_web_ui_import_template, self);
	htmx_router_post(router, "/e/:type/import", venture_web_ui_import, self);
	htmx_router_get(router, "/e/:type/new", venture_web_ui_form, self);
	htmx_router_post(router, "/e/:type", venture_web_ui_save, self);
	htmx_router_get(router, "/e/:type/:id", venture_web_ui_detail, self);
	htmx_router_get(router, "/e/:type/:id/edit", venture_web_ui_form, self);
	htmx_router_post(router, "/e/:type/:id", venture_web_ui_save, self);
	htmx_router_post(router, "/e/:type/:id/delete", venture_web_ui_delete,
	                 self);
	htmx_router_get(router, "/login", venture_web_ui_login_form, self);
	htmx_router_post(router, "/login", venture_web_ui_login_submit, self);
	htmx_router_get(router, "/logout", venture_web_ui_logout, self);
	htmx_router_post(router, "/ui/chat", venture_web_ui_chat, self);
	htmx_router_get(router, "/ui/chat/threads", venture_web_ui_chat_threads,
	                self);
	htmx_router_get(router, "/ui/chat/thread/:id", venture_web_ui_chat_thread,
	                self);
	htmx_router_post(router, "/ui/chat/thread/:id/delete",
	                 venture_web_ui_chat_thread_delete, self);
	htmx_router_post(router, "/ui/chat/upload", venture_web_ui_chat_upload,
	                 self);
	htmx_router_post(router, "/ui/chat/confirm/:id/approve",
	                 venture_web_ui_chat_approve, self);
	htmx_router_post(router, "/ui/chat/confirm/:id/reject",
	                 venture_web_ui_chat_reject, self);

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
	/*
	 * The forge credential routes, in their API form so a deployment can
	 * be scripted. Named for the type rather than generic across types:
	 * a generic "set a sensitive field" endpoint would let a caller
	 * write a raw password hash, which is precisely what hashing exists
	 * to prevent.
	 */
	htmx_router_post(router, "/api/v1/forge/:id/token",
	                 venture_web_api_forge_token, self);
	htmx_router_post(router, "/api/v1/forge/:id/webhook-secret",
	                 venture_web_api_forge_secret, self);
	htmx_router_post(router, "/api/v1/forge/:id/verify",
	                 venture_web_api_forge_verify, self);

	/* Before the generic record routes, so "restore" is not read as an
	 * id. */
	htmx_router_post(router, "/api/v1/:type/:id/restore",
	                 venture_web_api_restore, self);

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
