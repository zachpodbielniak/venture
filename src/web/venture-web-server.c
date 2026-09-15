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
#include "quotes/venture-document-print-style.h"
#include "statements/venture-statements-private.h"

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

/*
 * One turn, worked out by the POST and waiting for the GET that will
 * stream it.
 *
 * It holds refs to the history records rather than ids: they were read
 * once already, and re-reading them in the second request would let a
 * message saved in between change what the model is shown compared with
 * what the first request decided.
 */
typedef struct
{
	gint64			 thread_id;
	gint64			 user_id;
	gchar			*model_text;
	GPtrArray		*history;
	GPtrArray		*images;
	GPtrArray		*image_types;
	GHashTable		*staged_before;
	GDateTime		*created_at;
} VentureWebChatTurn;

/*
 * Long enough for a slow page to open the stream, short enough that an
 * abandoned turn is not still holding a conversation's history an hour
 * later.
 */
#define VENTURE_WEB_CHAT_TURN_TTL_SECONDS 120

static void
venture_web_chat_turn_free(VentureWebChatTurn *self)
{
	if (NULL == self)
		return;

	g_free(self->model_text);
	g_clear_pointer(&self->history, g_ptr_array_unref);
	g_clear_pointer(&self->images, g_ptr_array_unref);
	g_clear_pointer(&self->image_types, g_ptr_array_unref);
	g_clear_pointer(&self->staged_before, g_hash_table_unref);
	g_clear_pointer(&self->created_at, g_date_time_unref);
	g_free(self);
}

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

	/*
	 * Turns waiting to be streamed, keyed by a one-shot token.
	 *
	 * A streamed answer is two requests: the POST that stores the
	 * question and decides what the model will be shown, and the GET
	 * that holds a connection open while it answers. Everything the
	 * second needs is worked out by the first -- the history to replay,
	 * the images, what was already staged -- because the POST is where
	 * the form, the attachments and the page context are, and passing
	 * that lot through a query string would put a conversation in the
	 * access log.
	 */
	GHashTable	*chat_turns;
	HtmxRateLimiter *quote_limiter;
	HtmxRateLimiter *lead_limiter;
};

G_DEFINE_FINAL_TYPE(VentureWebServer, venture_web_server, G_TYPE_OBJECT)

static void venture_web_append_lead_actions(GString *html, VentureEntity *record);

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
	g_clear_object(&self->quote_limiter);
	g_clear_object(&self->server);
	g_clear_pointer(&self->base_url, g_free);
	g_clear_pointer(&self->reveals, g_hash_table_unref);
	g_clear_pointer(&self->chat_turns, g_hash_table_unref);
	g_clear_object(&self->lead_limiter);

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
	self->chat_turns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)venture_web_chat_turn_free);
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
 * Whether a module is on. A NULL module means "not gated", which is what a
 * link or page that belongs to no module in particular says.
 *
 * Returns: %TRUE if the page or link may be offered
 */
static gboolean
venture_web_module_enabled(
	VentureWebServer	*self,
	const gchar		*module_name
){
	if (NULL == module_name)
		return TRUE;

	return venture_context_module_enabled(self->context, module_name);
}

/*
 * Builds the error a request into a disabled module gets. Named so the
 * operator who turned the module off recognises it, and so somebody who
 * did not learns which switch to look at.
 */
static void
venture_web_set_module_disabled_error(
	const gchar	 *module_name,
	GError		**error
){
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
	            "The %s module is disabled on this install "
	            "(modules.%s.enabled)", module_name, module_name);
}

/*
 * Refuses an API request into a module that is off.
 *
 * Every API route that belongs to a module calls this first. The generic
 * record routes do not need to: a disabled module's types are hidden from
 * the registry, so /api/v1/<type> already resolves to nothing.
 *
 * Returns: (transfer full) (nullable): a 404 to return instead, or %NULL
 */
static HtmxResponse *
venture_web_require_module_api(
	VentureWebServer	*self,
	const gchar		*module_name
){
	g_autoptr(GError) error = NULL;

	if (venture_web_module_enabled(self, module_name))
		return NULL;

	venture_web_set_module_disabled_error(module_name, &error);

	return venture_web_error_response(error);
}

static HtmxResponse *
venture_web_ui_require_session(
	VentureWebServer	*self,
	HtmxRequest		*request
);

static void
venture_web_chat_append_starters(
	VentureWebServer	*self,
	GString			*html,
	const gchar		*path
);

static void activity_lazy_sweep(VentureWebServer *self, HtmxRequest *request);

static gboolean
venture_web_field_is_machinery(const gchar *name);

static gchar *
venture_web_json_to_display(JsonNode *value);

/*
 * The inline theme script, emitted before the stylesheet so a dark-theme
 * reload never flashes white. It has to be inline and synchronous; a
 * deferred script would be too late.
 *
 * The configured ui.theme is the fallback when the browser has stored no
 * choice, and is handed to venture.js in the same breath so the toggle
 * starts its cycle from the right place. The value is a closed set --
 * validated at configuration load -- so it goes into the script as a
 * quoted literal without escaping beyond that check.
 */
static gboolean
venture_web_theme_is_valid(const gchar *theme)
{
	return venture_config_theme_is_valid(theme);
}

static void
venture_web_append_theme_script(
	VentureWebServer	*self,
	GString			*html
){
	g_autofree gchar *theme = NULL;

	g_object_get(venture_context_get_config(self->context), "ui-theme", &theme,
	             NULL);

	if (!venture_web_theme_is_valid(theme))
	{
		g_free(theme);
		theme = g_strdup("system");
	}

	g_string_append_printf(html,
		"<script>(function(){var d='%s';window.VENTURE_THEME_DEFAULT=d;"
		"try{var t=localStorage.getItem('venture.theme')||d;"
		"if(t==='light'||t==='dark'||t==='mocha')document.documentElement"
		".setAttribute('data-theme',t);}catch(e){}})();</script>", theme);
}

/*
 * Which of the two looks a page is drawn in.
 *
 * A look is a whole stylesheet: "classic" is the editorial design, white
 * cards on a bone canvas with serif titles; "industrial" is the
 * instrument panel. Each carries every theme. The browser's own choice is
 * a plain cookie set by the switch in the sidebar, and the configured
 * ui.look is what applies until it has made one. The cookie is read here
 * rather than through the session because the sign-in page has no
 * session and should still come up in the look the person last chose.
 *
 * The value is validated against the closed set on the way in, so a
 * cookie somebody edited by hand falls back to the configuration rather
 * than reaching the page.
 */
#define VENTURE_WEB_LOOK_COOKIE "venture_look"

static const gchar *
venture_web_look(
	VentureWebServer	*self,
	HtmxRequest		*request
){
	g_autofree gchar *configured = NULL;
	SoupServerMessage *message;
	const gchar *header;

	message = (NULL != request) ? htmx_request_get_message(request) : NULL;
	header = (NULL != message)
		? soup_message_headers_get_list(
			soup_server_message_get_request_headers(message), "Cookie")
		: NULL;

	if (NULL != header)
	{
		g_autoptr(GHashTable) cookies = NULL;

		cookies = htmx_cookie_parse_request(header);

		if (NULL != cookies)
		{
			const gchar *chosen;

			chosen = g_hash_table_lookup(cookies, VENTURE_WEB_LOOK_COOKIE);

			if (venture_config_look_is_valid(chosen))
				return (0 == g_strcmp0(chosen, "classic"))
					? "classic" : "industrial";
		}
	}

	g_object_get(venture_context_get_config(self->context), "ui-look",
	             &configured, NULL);

	return (0 == g_strcmp0(configured, "classic")) ? "classic" : "industrial";
}

/*
 * The stylesheet for the chosen look, inlined. One or the other, never
 * both: they are each the whole design, and the page is the same weight
 * it was when there was only one.
 */
static void
venture_web_append_stylesheet(
	VentureWebServer	*self,
	HtmxRequest		*request,
	GString			*html
){
	const gchar *look;

	look = venture_web_look(self, request);

	g_string_append(html, "<style>");
	g_string_append(html, (0 == g_strcmp0(look, "classic"))
		? venture_asset_venture_classic_css
		: venture_asset_venture_industrial_css);
	g_string_append(html, "</style>");
}

static gchar *
venture_web_page(
	VentureWebServer	*self,
	HtmxRequest		*request,
	const gchar		*active,
	const gchar		*title,
	const gchar		*content
);

/* The dashboards, defined beside their pages further down but reached
 * from the home page and the sidebar above them. */
static gchar *
venture_web_render_home_dashboard(
	VentureWebServer	*self,
	HtmxRequest		*request
);

static void
venture_web_append_dashboard_nav(
	VentureWebServer	*self,
	HtmxRequest		*request,
	GString			*html,
	const gchar		*active
);

static void
venture_web_append_sla_badge(
	GString		*content,
	VentureEntity	*ticket
);

static void
venture_web_append_save_view_form(
	GString		*content,
	HtmxRequest	*request,
	const gchar	*entity_type,
	gboolean	 board
);

static void
venture_web_append_bulk_bar(
	VentureWebServer	*self,
	GString			*content,
	HtmxRequest		*request,
	const gchar		*type_name,
	VentureEntity		*prototype
);

/* The workdesk's sidebar entries, defined with its pages further down. */
static void
venture_web_append_inbox_nav(
	VentureWebServer	*self,
	HtmxRequest		*request,
	GString			*html,
	const gchar		*active
);

static void
venture_web_append_saved_view_nav(
	VentureWebServer	*self,
	HtmxRequest		*request,
	GString			*html,
	const gchar		*active
);

static GArray *
venture_web_organization_tree(
	VentureWebServer	*self,
	gint64			 root
);

/*
 * The UI counterpart. The session is checked before the module, so an
 * anonymous request is sent to sign in rather than told what the install
 * runs; a signed-in one gets a page saying which module is off.
 *
 * Returns: (transfer full) (nullable): a response to return instead, or
 *   %NULL when the request may proceed
 */
static HtmxResponse *
venture_web_require_module_ui(
	VentureWebServer	*self,
	HtmxRequest		*request,
	const gchar		*module_name
){
	g_autoptr(GString) body = NULL;
	HtmxResponse *redirect;

	if (venture_web_module_enabled(self, module_name))
		return NULL;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	body = g_string_new("<div class=\"empty\"><h3>The ");
	venture_html_escape_append(body, module_name);
	g_string_append(body,
		" module is turned off</h3><p class=\"muted\">This page belongs to "
		"a module the configuration has disabled. Turn it on with "
		"<code>modules.");
	venture_html_escape_append(body, module_name);
	g_string_append(body,
		".enabled: true</code> and restart, or see what is on at "
		"<a href=\"/modules\">Modules</a>.</p>"
		"<p><a class=\"btn btn-primary\" href=\"/\">Dashboard</a></p></div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/modules", "Module off",
		                 body->str), 404);
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

	if ((VENTURE_TYPE_FEDERATION_PEER == entity_type) ||
	    (VENTURE_TYPE_FEDERATION_GRANT == entity_type) ||
	    (VENTURE_TYPE_USER == entity_type) ||
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

	/* An inbox and a watch list are personal for the same reason; the
	 * inbox routes and the watch button filter to the caller. */
	if ((VENTURE_TYPE_NOTIFICATION == entity_type) ||
	    (VENTURE_TYPE_WATCH == entity_type))
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
	 * A webhook holds a signing secret and names the host this install's
	 * business data is posted to. An editor who could change its URL
	 * could point every change in the books at a server they control,
	 * and the staged diff would show nothing but a URL moving. Same
	 * reasoning as a forge.
	 */
	if (VENTURE_TYPE_WEBHOOK == entity_type)
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
	if (VENTURE_TYPE_FEDERATION_REPLICA == entity_type)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"Use federation replica operations to edit or synchronize a working copy");
		return FALSE;
	}

	if (VENTURE_TYPE_LEDGER_ENTRY == entity_type)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"Ledger entries are read-only journal projections; use the posting service");
		return FALSE;
	}

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

	/* And a delivery is what went out and what came back, written as it
	 * happened. Same argument again; reading stays open. */
	if (VENTURE_TYPE_WEBHOOK_DELIVERY == entity_type)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_PERMISSION_DENIED,
		                    "A delivery record is written as the delivery "
		                    "happens; it cannot be edited");
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
		"Overview",
		"core"
	},
	{
		"/dashboards", "Dashboards",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"4\" width=\"18\" height=\"16\" rx=\"2\"/>"
			"<path d=\"M3 10h18\"/><path d=\"M10 10v10\"/>"
		),
		NULL,
		"dashboards"
	},
	{
		"/reports", "Reports",
		VENTURE_ICON(
			"<path d=\"M3 20h18\"/><path d=\"M6 20v-6\"/>"
			"<path d=\"M12 20V5\"/><path d=\"M18 20v-9\"/>"
		),
		NULL,
		"core"
	},
	{
		"/e/venture", "Ventures",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"7\" width=\"18\" height=\"13\" rx=\"2\"/>"
			"<path d=\"M9 7V5a2 2 0 0 1 2-2h2a2 2 0 0 1 2 2v2\"/>"
			"<path d=\"M3 12h18\"/>"
		),
		"Business",
		"core"
	},
	{
		"/e/sale", "Sales",
		VENTURE_ICON(
			"<path d=\"M3 17l6-6 4 4 8-8\"/><path d=\"M15 7h6v6\"/>"
		),
		NULL,
		"sales"
	},
	{
		"/e/invoice", "Invoices",
		VENTURE_ICON(
			"<path d=\"M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z\"/>"
			"<path d=\"M14 3v5h5\"/><path d=\"M9 13h6\"/>"
			"<path d=\"M9 17h4\"/>"
		),
		NULL,
		"invoicing"
	},
	{
		"/setup", "Setup",
		VENTURE_ICON(
			"<path d=\"M4 4h16v4H4z\"/><path d=\"M8 12h8\"/><path d=\"M8 16h5\"/>"
		),
		NULL,
		"setup"
	},
	{
		"/e/accounting_cutover", "Cutover",
		VENTURE_ICON(
			"<path d=\"M4 4h16v4H4z\"/><path d=\"M4 10h10v10H4z\"/><path d=\"M16 14h4v6h-4z\"/>"
		),
		NULL,
		"cutover"
	},
	{
		"/e/product", "Products",
		VENTURE_ICON(
			"<path d=\"M21 8l-9-5-9 5 9 5 9-5z\"/>"
			"<path d=\"M3 8v8l9 5 9-5V8\"/><path d=\"M12 13v8\"/>"
		),
		NULL,
		"sales"
	},
	{
		"/e/inventory_item", "Inventory",
		VENTURE_ICON(
			"<path d=\"M12 2.5L3 7.5l9 5 9-5-9-5z\"/>"
			"<path d=\"M3 12l9 5 9-5\"/><path d=\"M3 16.5l9 5 9-5\"/>"
		),
		NULL,
		"sales"
	},
	{
		"/e/expense", "Expenses",
		VENTURE_ICON(
			"<path d=\"M3 7l6 6 4-4 8 8\"/><path d=\"M15 17h6v-6\"/>"
		),
		"Money",
		"finance"
	},
	{
		"/e/account", "Accounts",
		VENTURE_ICON(
			"<path d=\"M3 21h18\"/><path d=\"M5 21V10\"/>"
			"<path d=\"M9 21V10\"/><path d=\"M15 21V10\"/>"
			"<path d=\"M19 21V10\"/><path d=\"M12 3L3 8h18l-9-5z\"/>"
		),
		NULL,
		"finance"
	},
	{
		"/e/tax_category", "Tax",
		VENTURE_ICON(
			"<path d=\"M19 5L5 19\"/>"
			"<circle cx=\"7.5\" cy=\"7.5\" r=\"2.5\"/>"
			"<circle cx=\"16.5\" cy=\"16.5\" r=\"2.5\"/>"
		),
		NULL,
		"finance"
	},
	{
		"/e/journal", "Journals",
		VENTURE_ICON(
			"<path d=\"M4 4h12a2 2 0 0 1 2 2v14H6a2 2 0 0 1-2-2z\"/>"
			"<path d=\"M4 4v14\"/><path d=\"M9 9h5\"/><path d=\"M9 13h5\"/>"
		),
		NULL,
		"ledger"
	},
	{
		"/e/journal_line", "Journal lines",
		VENTURE_ICON(
			"<path d=\"M8 6h12\"/><path d=\"M8 12h12\"/>"
			"<path d=\"M8 18h12\"/><path d=\"M4 6h.01\"/>"
			"<path d=\"M4 12h.01\"/><path d=\"M4 18h.01\"/>"
		),
		NULL,
		"ledger"
	},
	{
		"/e/payment", "Payments",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"6\" width=\"18\" height=\"12\" rx=\"2\"/>"
			"<path d=\"M3 10h18\"/><path d=\"M7 15h3\"/>"
		),
		NULL,
		"receivables"
	},
	{
		"/e/payment_allocation", "Allocations",
		VENTURE_ICON(
			"<path d=\"M4 12h16\"/><path d=\"M14 6l6 6-6 6\"/>"
		),
		NULL,
		"receivables"
	},
	{
		"/e/customer_credit", "Customer credits",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"6\" width=\"18\" height=\"12\" rx=\"2\"/>"
			"<path d=\"M7 12h4\"/><path d=\"M9 10v4\"/>"
			"<path d=\"M15 12h2\"/>"
		),
		NULL,
		"receivables"
	},
	{
		"/e/refund", "Refunds",
		VENTURE_ICON(
			"<path d=\"M9 5L4 10l5 5\"/>"
			"<path d=\"M4 10h10a6 6 0 0 1 0 12\"/>"
		),
		NULL,
		"receivables"
	},
	{
		"/e/fiscal_year", "Fiscal years",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"5\" width=\"18\" height=\"16\" rx=\"2\"/>"
			"<path d=\"M3 10h18\"/><path d=\"M8 3v4\"/><path d=\"M16 3v4\"/>"
			"<path d=\"M8 15l2 2 4-4\"/>"
		),
		NULL,
		"periods"
	},
	{
		"/e/customer_subscription", "Subscriptions",
		VENTURE_ICON("<path d=\"M4 12a8 8 0 1 0 3-6\"/><path d=\"M3 3v6h6\"/>"),
		NULL, "billing"
	},
	{
		"/e/recurring_schedule", "Recurring",
		VENTURE_ICON("<path d=\"M4 12a8 8 0 1 0 3-6\"/><path d=\"M3 3v6h6\"/>"),
		NULL, "recurring"
	},
	{
		"/e/collection_case", "Collections",
		VENTURE_ICON("<path d=\"M4 4h16v4H4z\"/><path d=\"M4 12h16v8H4z\"/>"),
		NULL, "recurring"
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
		"Relations",
		"crm"
	},
	{
		"/e/contact", "Contacts",
		VENTURE_ICON(
			"<path d=\"M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2\"/>"
			"<circle cx=\"12\" cy=\"7\" r=\"4\"/>"
		),
		NULL,
		"crm"
	},
	{
		"/e/deal", "Deals",
		VENTURE_ICON(
			"<path d=\"M4 8h13\"/><path d=\"M14 5l3 3-3 3\"/>"
			"<path d=\"M20 16H7\"/><path d=\"M10 13l-3 3 3 3\"/>"
		),
		NULL,
		"crm"
	},
	{
		"/e/campaign", "Campaigns",
		VENTURE_ICON(
			"<path d=\"M4 10v4a1 1 0 0 0 1 1h2l5 4V5L7 9H5a1 1 0 0 0-1 1z\"/>"
			"<path d=\"M16.5 9.5a3.5 3.5 0 0 1 0 5\"/>"
			"<path d=\"M19.5 7a7 7 0 0 1 0 10\"/>"
		),
		"Growth",
		"outreach"
	},
	{
		"/e/newsletter", "Newsletters",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"5\" width=\"18\" height=\"14\" rx=\"2\"/>"
			"<path d=\"M3.5 7l8.5 6 8.5-6\"/>"
		),
		NULL,
		"outreach"
	},
	{
		"/e/post", "Posts",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"4\" width=\"18\" height=\"16\" rx=\"2\"/>"
			"<path d=\"M7 9h10\"/><path d=\"M7 13h10\"/>"
			"<path d=\"M7 17h6\"/>"
		),
		NULL,
		"outreach"
	},
	{
		"/e/idea", "Ideas",
		VENTURE_ICON(
			"<path d=\"M9 18h6\"/><path d=\"M10 21h4\"/>"
			"<path d=\"M12 3a6 6 0 0 0-3.5 10.9c.6.5.9 1.2.9 1.9V16h5.2v-.2c0-.7.3-1.4.9-1.9A6 6 0 0 0 12 3z\"/>"
		),
		"Thinking",
		"ideas"
	},
	{
		"/tickets", "Tickets",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\"/>"
			"<path d=\"M8 12l3 3 5-6\"/>"
		),
		NULL,
		"tickets"
	},
	{
		"/sprints", "Sprints",
		VENTURE_ICON(
			"<circle cx=\"12\" cy=\"12\" r=\"9\"/>"
			"<path d=\"M12 7v5l3 2\"/>"
		),
		NULL,
		"tickets"
	},
	{
		"/e/research_note", "Research",
		VENTURE_ICON(
			"<circle cx=\"11\" cy=\"11\" r=\"7\"/>"
			"<path d=\"M20 20l-3.9-3.9\"/>"
		),
		NULL,
		"ideas"
	},
	{
		"/e/forge_repo", "Repositories",
		VENTURE_ICON(
			"<circle cx=\"7\" cy=\"5\" r=\"2\"/>"
			"<circle cx=\"7\" cy=\"19\" r=\"2\"/>"
			"<circle cx=\"17\" cy=\"9\" r=\"2\"/><path d=\"M7 7v10\"/>"
			"<path d=\"M17 11v1a4 4 0 0 1-4 4H7\"/>"
		),
		"Code",
		"forge"
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
		NULL,
		"forge"
	},
	{
		"/harness", "Harness",
		VENTURE_ICON(
			"<path d=\"M4 17l6-5-6-5\"/>"
			"<path d=\"M12 19h8\"/>"
		),
		NULL,
		"forge"
	},
	{
		"/runs", "Runs",
		VENTURE_ICON(
			"<circle cx=\"12\" cy=\"12\" r=\"9\"/>"
			"<path d=\"M10 8.5l6 3.5-6 3.5z\"/>"
		),
		NULL,
		"forge"
	},
	{
		"/factory", "Factory",
		VENTURE_ICON(
			"<path d=\"M3 21V9l6 4V9l6 4V9l6 4v8z\"/>"
			"<path d=\"M3 21h18\"/><path d=\"M7 17h2\"/>"
			"<path d=\"M12 17h2\"/><path d=\"M17 17h2\"/>"
		),
		"Factory",
		"factory"
	},
	{
		"/e/milestone", "Milestones",
		VENTURE_ICON(
			"<path d=\"M4 21V4\"/><path d=\"M4 5h13l-3 4 3 4H4\"/>"
		),
		NULL,
		"factory"
	},
	{
		"/e/release", "Releases",
		VENTURE_ICON(
			"<path d=\"M20.6 3.4a7 7 0 0 0-9.9 9.9l-6.4 6.4v1.9h1.9l6.4-6.4a7 7 0 0 0 8-11.8z\"/>"
			"<circle cx=\"15.5\" cy=\"8.5\" r=\"1.5\"/>"
		),
		NULL,
		"factory"
	},
	{
		"/e/build", "Builds",
		VENTURE_ICON(
			"<path d=\"M14.7 6.3a4 4 0 0 0-5.4 5.4L3 18v3h3l6.3-6.3a4 4 0 0 0 5.4-5.4l-2.4 2.4-2-2z\"/>"
		),
		NULL,
		"factory"
	},
	{
		"/e/environment", "Environments",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"4\" width=\"18\" height=\"6\" rx=\"1.5\"/>"
			"<rect x=\"3\" y=\"14\" width=\"18\" height=\"6\" rx=\"1.5\"/>"
			"<path d=\"M7 7h.01\"/><path d=\"M7 17h.01\"/>"
		),
		NULL,
		"factory"
	},
	{
		"/e/deployment", "Deployments",
		VENTURE_ICON(
			"<path d=\"M12 3v12\"/><path d=\"M7 10l5 5 5-5\"/>"
			"<path d=\"M4 20h16\"/>"
		),
		NULL,
		"factory"
	},
	{
		"/e/incident", "Incidents",
		VENTURE_ICON(
			"<path d=\"M12 3l9.5 16.5H2.5z\"/><path d=\"M12 9v5\"/>"
			"<path d=\"M12 17.5h.01\"/>"
		),
		NULL,
		"factory"
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
		"System",
		"core"
	},
	{
		"/automations", "Automations",
		VENTURE_ICON(
			"<path d=\"M13 2L4 14h7l-1 8 9-12h-7l1-8z\"/>"
		),
		NULL,
		"automation"
	},
	{
		"/plugins", "Plugins",
		VENTURE_ICON(
			"<path d=\"M9 2v6\"/><path d=\"M15 2v6\"/>"
			"<path d=\"M6 8h12v3a6 6 0 0 1-12 0V8z\"/>"
			"<path d=\"M12 17v5\"/>"
		),
		NULL,
		"plugins"
	},
	{
		"/modules", "Modules",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"3\" width=\"8\" height=\"8\" rx=\"1.5\"/>"
			"<rect x=\"13\" y=\"3\" width=\"8\" height=\"8\" rx=\"1.5\"/>"
			"<rect x=\"3\" y=\"13\" width=\"8\" height=\"8\" rx=\"1.5\"/>"
			"<path d=\"M17 13v8\"/><path d=\"M13 17h8\"/>"
		),
		NULL,
		"core"
	},
	{
		"/account", "Your account",
		VENTURE_ICON(
			"<circle cx=\"12\" cy=\"12\" r=\"9\"/>"
			"<circle cx=\"12\" cy=\"10\" r=\"3\"/>"
			"<path d=\"M6.5 19a6 6 0 0 1 11 0\"/>"
		),
		NULL,
		"core"
	},
	{
		"/kb", "Knowledge",
		VENTURE_ICON(
			"<path d=\"M4 5a2 2 0 0 1 2-2h5v18H6a2 2 0 0 1-2-2z\"/>"
			"<path d=\"M20 5a2 2 0 0 0-2-2h-5v18h5a2 2 0 0 0 2-2z\"/>"
			"<path d=\"M11 3v18\"/>"
		),
		NULL,
		"kb"
	},
	{
		"/assistant", "Assistant",
		VENTURE_ICON(
			"<path d=\"M13 3l-6 10h5l-1 8 6-10h-5z\"/>"
		),
		NULL,
		"chat"
	},
	{
		"/account/tokens", "API tokens",
		VENTURE_ICON(
			"<path d=\"M14 7a4 4 0 1 0-3.9 5H14l2 2 2-2 2 2 2-2-2-2h-6z\"/>"
			"<circle cx=\"7\" cy=\"11\" r=\"1.2\"/>"
		),
		NULL,
		"core"
	},
	{
		"/users", "Users",
		VENTURE_ICON(
			"<path d=\"M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2\"/>"
			"<circle cx=\"9\" cy=\"7\" r=\"4\"/>"
			"<path d=\"M22 21v-2a4 4 0 0 0-3-3.9\"/>"
			"<path d=\"M16 3.1a4 4 0 0 1 0 7.8\"/>"
		),
		NULL,
		"core"
	},
	{
		"/settings", "Settings",
		VENTURE_ICON(
			"<circle cx=\"12\" cy=\"12\" r=\"3\"/>"
			"<path d=\"M19.4 15a1.7 1.7 0 0 0 .3 1.9l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-2.9 1.2V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-2.9-1.2l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0-1.2-2.9H3a2 2 0 1 1 0-4h.1A1.7 1.7 0 0 0 4.3 7.1l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 2.9-1.2V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 2.9 1.2l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0 1.2 2.9H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z\"/>"
		),
		NULL,
		"core"
	},
	{
		"/e/forge", "Forges",
		VENTURE_ICON(
			"<rect x=\"3\" y=\"4\" width=\"18\" height=\"7\" rx=\"2\"/>"
			"<rect x=\"3\" y=\"13\" width=\"18\" height=\"7\" rx=\"2\"/>"
			"<path d=\"M7 7.5h.01\"/><path d=\"M7 16.5h.01\"/>"
		),
		NULL,
		"forge"
	},
	{
		"/federation", "Federation",
		VENTURE_ICON("<path d=\"M3 12h18M12 3v18\"/>"),
		NULL, "federation"
	},
	{
		"/webhooks", "Webhooks",
		VENTURE_ICON(
			"<path d=\"M9 12a3 3 0 1 1 4.2 2.75\"/>"
			"<path d=\"M6.5 17.5a5.5 5.5 0 0 1 2.2-9.8\"/>"
			"<path d=\"M13 21h4a4 4 0 0 0 .8-7.92\"/>"
		),
		NULL,
		"webhooks"
	},
	{
		"/e/audit_entry", "Audit log",
		VENTURE_ICON(
			"<path d=\"M3.5 12a8.5 8.5 0 1 0 2.5-6\"/>"
			"<path d=\"M3 3v5h5\"/><path d=\"M12 8v4.5l3 1.5\"/>"
		),
		NULL,
		"core"
	},
	{
		"/e/mail_message", "Mail outbox",
		VENTURE_ICON("<path d=\"M3 5h18v14H3zM3 5l9 7 9-7\"/>"),
		NULL, "mail"
	},
	{ "/accounting", "Books", VENTURE_ICON("<path d=\"M4 4h16v16H4zM8 8h8M8 12h8M8 16h5\"/>"), "Accounting", "accounting" },
	{ "/bankfeed", "Bank feeds", VENTURE_ICON("<path d=\"M4 12h16M4 7h16M4 17h10\"/>"), NULL, "bankfeed" },
	{ "/payables", "Pay bills", VENTURE_ICON("<path d=\"M4 12h16M14 6l6 6-6 6\"/>"), NULL, "payables" },
	{ "/claims", "Claims", VENTURE_ICON("<path d=\"M4 4h16v16H4zM8 8h8M8 12h6\"/>"), NULL, "claims" },
	{ "/payroll", "Payroll", VENTURE_ICON("<path d=\"M4 6h16M4 12h16M4 18h10\"/>"), NULL, "payroll" },
	{ "/purchasing", "Purchasing", VENTURE_ICON("<path d=\"M4 7h16M4 12h10M4 17h7\"/>"), NULL, "goods" },
	{ "/close", "Period close", VENTURE_ICON("<rect x=\"3\" y=\"5\" width=\"18\" height=\"16\" rx=\"2\"/><path d=\"M8 15l2 2 4-4\"/>"), NULL, "close" },
	{ "/tax-filings", "Tax filings", VENTURE_ICON("<path d=\"M4 4h16v16H4zM8 8h8M8 12h8M8 16h5\"/>"), NULL, "tax_filing" },
	{ "/capture", "Capture inbox", VENTURE_ICON("<path d=\"M4 4h16v12H4zM8 20h8\"/>"), NULL, "capture" },
	{ "/worklist", "My day", VENTURE_ICON("<path d=\"M4 7h16M4 12h16M4 17h10\"/>"), "Activities", "activities" },
	{ "/deals", "Sales board", VENTURE_ICON("<path d=\"M4 4v16M12 4v16M20 4v16\"/>"), "Sales pipelines", "pipelines" },
	{ "/invoices/compose", "New invoice", VENTURE_ICON("<path d=\"M4 4h16v16H4zM8 8h8M8 12h8M8 16h5\"/>"), "Invoicing", "invoicing" },
	{ "/quotes/compose", "New quote", VENTURE_ICON("<path d=\"M4 4h16v16H4zM8 8h8M8 12h6\"/>"), "Quotes", "quotes" },
	{ NULL, NULL, NULL, NULL, NULL }
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
	venture_web_append_theme_script(self, html);

	venture_web_append_stylesheet(self, request, html);

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
	/* An empty ui.accent means the look's own colour -- blue in the
	 * classic stylesheet, red in the industrial -- so nothing is emitted
	 * and each look keeps the accent it was drawn with. */
	if (!venture_string_is_empty(accent))
	{
		g_string_append(html, "<style>:root{--accent-config:");
		venture_html_escape_append(html, accent);
		g_string_append(html, ";}</style>");
	}

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

	venture_web_append_inbox_nav(self, request, html, active);

	{
		const VentureWebNavLink *links;
		gsize i;

		const gchar *section = NULL;
		const gchar *shown = NULL;

		links = venture_web_navigation();

		g_string_append(html, "<div class=\"nav\">");

		for (i = 0; NULL != links[i].path; i++)
		{
			/* A section heading belongs to the first link that carries
			 * it and every link after, until the next heading. */
			if (NULL != links[i].section)
				section = links[i].section;

			/* A link whose module is off is not offered. The heading
			 * follows the first link actually shown under it, so a
			 * section emptied by configuration leaves no orphan. */
			if (!venture_web_module_enabled(self, links[i].module))
				continue;

			if ((NULL != section) && (section != shown))
			{
				g_string_append(html, "<div class=\"nav-section\">");
				venture_html_escape_append(html, section);
				g_string_append(html, "</div>");
				shown = section;
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

		/* The operator's own pages, after the built-in ones. */
		venture_web_append_dashboard_nav(self, request, html, active);
		venture_web_append_saved_view_nav(self, request, html, active);

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

	/*
	 * The data plate: which build this is, and that it answered. A
	 * <samp> because it is the program's own output rather than a
	 * person's words, which is also what sets it in the mono.
	 */
	g_string_append(html, "<div class=\"sidebar-rev\">"
	                      "<samp>REV " VENTURE_VERSION_S "</samp>"
	                      "<samp class=\"live\">Online</samp></div>");

	/*
	 * The look switch: a form, so it works with scripting off and the
	 * choice is a cookie the server reads before it draws anything. The
	 * button names the look it switches to, and the current path rides
	 * along so the same page comes back in the other design.
	 */
	{
		const gchar *look;

		look = venture_web_look(self, request);

		g_string_append(html, "<form class=\"look-switch\" method=\"post\" "
		                      "action=\"/look\">"
		                      "<input type=\"hidden\" name=\"look\" value=\"");
		g_string_append(html, (0 == g_strcmp0(look, "classic"))
		                      ? "industrial" : "classic");
		g_string_append(html, "\"><input type=\"hidden\" name=\"back\" value=\"");
		venture_html_escape_append(html, htmx_request_get_path(request));
		g_string_append(html, "\"><button class=\"btn btn-ghost btn-sm\" "
		                      "type=\"submit\" title=\"Switch the design; "
		                      "the themes are the same in both\">");
		g_string_append(html, (0 == g_strcmp0(look, "classic"))
		                      ? "Industrial look" : "Classic look");
		g_string_append(html, "</button></form>");
	}

	g_string_append(html, "<button class=\"btn btn-ghost btn-sm\" "
	                      "data-theme-toggle>Theme</button>");
	g_string_append(html, "<a class=\"btn btn-ghost btn-sm\" "
	                      "href=\"/logout\">Sign out</a>");
	g_string_append(html, "</div></nav>");

	/*
	 * Put the sidebar back where the reader left it, and make sure the
	 * page they are on is one of the entries they can see.
	 *
	 * Every nav entry is an ordinary link, so following one is a full page
	 * load and the rebuilt sidebar starts at the top -- which throws away
	 * the scroll of anybody working from the entries far down it, on
	 * exactly the click that proves they were reading them. Arriving from
	 * a bookmark or a typed URL has the opposite problem: nothing was
	 * saved, and the entry for the current page may sit below the fold
	 * with no indication the sidebar goes any further.
	 *
	 * So: restore first, then bring the active entry into view only if it
	 * is not already there. That order matters. Correcting unconditionally
	 * would yank the sidebar on every ordinary click, undoing the
	 * restore it just performed.
	 *
	 * Emitted here, after the sidebar closes rather than after the list,
	 * because .nav is a flex child sized against its siblings: run this
	 * with the footer still unparsed and its height is computed as though
	 * the footer were not there, which is too tall, and both the scroll
	 * clamp and the centring are then measured against a box that does not
	 * exist. It still precedes <main>, which is what keeps it ahead of the
	 * first paint -- the same reason the theme script is inline. Restoring
	 * from venture.js at the end of the body would leave the browser free
	 * to paint the sidebar at the top first, replacing the lost position
	 * with a visible jump: a different annoyance, not a fix.
	 *
	 * sessionStorage rather than localStorage: a scroll position is worth
	 * remembering across a click, not across a week. The writes are
	 * throttled because a scroll event fires far more often than anything
	 * needs storing, and every failure is swallowed -- private-mode
	 * storage that throws must cost the navigation nothing.
	 */
	g_string_append(html,
		"<script>(function(){try{"
		"var n=document.querySelector('.sidebar .nav');"
		"if(!n)return;"
		"var k='venture.nav.scroll',t=null;"
		"var v=sessionStorage.getItem(k);"
		"if(v)n.scrollTop=parseInt(v,10)||0;"
		"var a=n.querySelector('.nav-item.active');"
		"if(a){"
		"var ar=a.getBoundingClientRect(),nr=n.getBoundingClientRect();"
		"if(ar.top<nr.top||ar.bottom>nr.bottom)"
		"n.scrollTop+=ar.top-nr.top-(nr.height-ar.height)/2;"
		"}"
		"n.addEventListener('scroll',function(){"
		"if(t)return;"
		"t=setTimeout(function(){t=null;"
		"try{sessionStorage.setItem(k,n.scrollTop);}catch(e){}"
		"},150);"
		"},{passive:true});"
		"}catch(e){}})();</script>");

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
	/* The panel is the chat module's; without it there is nothing for
	 * the launcher to open. */
	if (chat_dock && venture_web_module_enabled(self, "chat"))
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
			"data-ai-export title=\"Download this conversation as org\">"
			"\xe2\xa4\x93</button>"
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

		/* Ways in, for the page this is. Shown while the conversation
		 * is empty; the script hides them once there is one. */
		venture_web_chat_append_starters(self, html,
			htmx_request_get_path(request));

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
			"<input type=\"hidden\" id=\"chat-context\" "
			"name=\"context\" value=\"\">"
			/* Set to 1 by the script when this browser can hold a
			 * stream open. Empty means answer it in one go. */
			"<input type=\"hidden\" id=\"chat-stream\" "
			"name=\"stream\" value=\"\">"
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
		/* Says "disabled module" or "no such type", whichever it is. */
		venture_entity_registry_set_unknown_type_error(
			venture_context_get_entity_registry(self->context), name, error);
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
venture_web_orgaccess_post(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	return venture_orgaccess_web_post(self->auth, self->context, request, params);
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

	if (venture_access_policy_requires_approval(venture_database_get_access_policy(venture_context_get_database(self->context)), principal, "write", record, &error)) stage = TRUE;
	if (NULL != error) return venture_web_error_response(error);

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

#include "billing/venture-billing-web.inc"

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

#include "venture-web-actions-private.h"

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

	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *types = json_node_get_array(node);
		guint i;
		for (i = 0; i < json_array_get_length(types); i++)
		{
			JsonObject *object = json_array_get_object_element(types, i);
			json_object_set_member(object, "actions", venture_action_registry_describe(
				venture_database_get_action_registry(venture_context_get_database(self->context)),
				json_object_get_string_member(object, "name")));
		}
	}
	else
		json_object_set_member(json_node_get_object(node), "actions", venture_action_registry_describe(
			venture_database_get_action_registry(venture_context_get_database(self->context)), name));

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
	g_autoptr(JsonObject) report_options = json_object_new();
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

	{
		const gchar *as_of = htmx_request_get_query_param(request, "as_of");
		const gchar *organization = htmx_request_get_query_param(request, "organization_id");
		static const gchar *const strings[] = { "currency", "group_by", "owner", "compare_to", "basis", NULL };
		static const gchar *const integers[] = { "customer_id", "venture_id", "vendor_id", "pipeline_id", "statement_id", "account_id", "days", NULL };
		guint i;
		for (i = 0; strings[i] != NULL; i++)
		{
			const gchar *value = htmx_request_get_query_param(request, strings[i]);
			if (!venture_string_is_empty(value))
				json_object_set_string_member(report_options, strings[i], value);
		}
		for (i = 0; integers[i] != NULL; i++)
		{
			const gchar *value = htmx_request_get_query_param(request, integers[i]);
			if (!venture_string_is_empty(value))
				json_object_set_int_member(report_options, integers[i], g_ascii_strtoll(value, NULL, 10));
		}
		if (!venture_string_is_empty(as_of))
			json_object_set_string_member(report_options, "as_of", as_of);
		if (!venture_string_is_empty(organization))
			json_object_set_int_member(report_options, "organization_id", g_ascii_strtoll(organization, NULL, 10));
	}
	result = venture_report_generate(report, self->context, period, report_options,
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
venture_web_ui_overview(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
);

/*
 * GET / - the home page: a dashboard marked as home, if the viewer may
 * see one, otherwise the built-in overview, which is always at /overview.
 */
static HtmxResponse *
venture_web_ui_dashboard(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	gchar *home;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!principal->authenticated)
		return venture_web_ui_overview(request, params, user_data);

	home = venture_web_render_home_dashboard(self, request);

	if (NULL != home)
		return venture_web_html_response(home, 200);

	return venture_web_ui_overview(request, params, user_data);
}

static HtmxResponse *
venture_web_ui_overview(
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
	if (venture_web_module_enabled(self, "tickets"))
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
	if (venture_web_module_enabled(self, "crm"))
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
				venture_string_is_empty(actor) ? "The system" : actor);
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

	venture_orgaccess_web_dispatch(self->auth, self->context, context, next, next_data);

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "automation");

		if (NULL != gate)
			return gate;
	}

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
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "automation");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "automation");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "automation");

		if (NULL != gate)
			return gate;
	}

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

static HtmxResponse *
venture_web_stripe_checkout(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureStripeService *service;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	gint64 id;

	gate = api ? venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR) : venture_web_ui_require_session(self, request);
	if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, &error)) return venture_web_error_response(error);
	gate = venture_web_require_module_api(self, "stripe");
	if (gate) return gate;
	service = venture_context_get_stripe_service(self->context);
	if (!service)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Stripe module has not started");
		return venture_web_error_response(error);
	}
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	venture_auth_to_actor(principal, &actor);
	checkout = venture_stripe_service_checkout(service, id, &actor, &error);
	if (!checkout) return venture_web_error_response(error);
	if (!api)
	{
		g_autofree gchar *url = NULL;
		HtmxResponse *response = htmx_response_new();
		g_object_get(checkout, "url", &url, NULL);
		htmx_response_set_status(response, 303);
		htmx_response_add_header(response, "Location", url);
		return response;
	}
	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(checkout), FALSE);
	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_stripe_webhook(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureStripeService *service = venture_context_get_stripe_service(self->context);
	g_autoptr(GError) error = NULL;
	HtmxResponse *response = htmx_response_new();
	GBytes *body = htmx_request_get_body_bytes(request);
	SoupMessageHeaders *headers = soup_server_message_get_request_headers(htmx_request_get_message(request));
	const gchar *signature = soup_message_headers_get_one(headers, "Stripe-Signature");
	(void)params;
	if (!service || !body)
		htmx_response_set_status(response, service ? 400 : 503);
	else if (!venture_stripe_service_handle_webhook(service, body, signature, &error))
		htmx_response_set_status(response, 400);
	else
		htmx_response_set_status(response, 200);
	return response;
}
#include "payables/venture-payables-web.inc"
#include "claims/venture-claims-web.inc"
#include "payroll/venture-payroll-web.inc"
#include "goods/venture-goods-web.inc"

/*
 * POST /invoices/:id/status - the same service reached by generated writes.
 * The paid action records a receipt; status itself remains derived.
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
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *to;
	gint64 id;

	self = user_data;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "invoicing");

		if (NULL != gate)
			return gate;
	}

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

	now = venture_time_now();
	venture_auth_to_actor(principal, &actor);

	if (g_strcmp0(to, "paid") == 0)
	{
		HtmxResponse *gate = venture_web_require_module_ui(self, request, "receivables");
		if (NULL != gate)
			return gate;
		if (!venture_settlement_service_settle_invoice(
			venture_settlement_service_get(venture_context_get_database(self->context)),
			id, now, &actor, &error))
			return venture_web_error_response(error);
	}
	else if (g_strcmp0(to, "write-off") == 0)
	{
		HtmxResponse *gate = venture_web_require_module_ui(self, request, "receivables");
		if (NULL != gate)
			return gate;
		if (!venture_settlement_service_write_off(
			venture_settlement_service_get(venture_context_get_database(self->context)),
			id, now, &actor, &error))
			return venture_web_error_response(error);
	}
	else if (!venture_settlement_service_transition(
		venture_settlement_service_get(venture_context_get_database(self->context)),
		VENTURE_INVOICE(record), to, now, &actor, &error))
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/invoice/%" G_GINT64_FORMAT, id);

	return venture_web_redirect_to(destination);
}

/*
 * GET /invoices/:id/print - the invoice as a clean printable page: no
 * chrome, no sidebar, just the document. The browser's print dialog is the
 * PDF generator; it is already installed everywhere.
 */
static void quote_buttons(GString *html, VentureEntity *record);

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "invoicing");

		if (NULL != gate)
			return gate;
	}

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
		VENTURE_DOCUMENT_PRINT_STYLE
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
	                      "<th class=\"num\">Net</th>"
	                      "<th class=\"num\">Tax</th>"
	                      "<th class=\"num\">Amount</th></tr></thead><tbody>");

	for (i = 0; i < lines->len; i++)
	{
		VentureInvoiceLine *line;
		g_autofree gchar *description = NULL;
		g_autoptr(VentureMoney) unit_price = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(VentureMoney) tax = NULL;
		gdouble quantity;

		line = g_ptr_array_index(lines, i);
		g_object_get(line, "description", &description,
		             "quantity", &quantity,
		             "unit-price", &unit_price,
		             "income-amount", &net, "tax-amount", &tax, NULL);
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
		if (NULL != net)
		{
			g_autofree gchar *text = venture_money_to_display_string(net, TRUE);
			venture_html_escape_append(html, text);
		}
		g_string_append(html, "</td><td class=\"num\">");
		if (NULL != tax)
		{
			g_autofree gchar *text = venture_money_to_display_string(tax, TRUE);
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
	                      "<td colspan=\"5\" class=\"num\">Total</td>"
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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "plugins");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "plugins");

		if (NULL != gate)
			return gate;
	}

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

			/* The importer feeds the same underscore-keyed JSON decoder as
			 * REST; dashed properties would silently lose external IDs. */
			{
				g_autofree gchar *column = venture_entity_property_to_column(
					venture_field_spec_get_name(spec));
				json_builder_set_member_name(builder, column);
			}
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

	/* A uniqueness refusal can happen at save, after field validation.
	 * Keep earlier rows in this import inside the same transaction. */
	if (!venture_database_begin(venture_context_get_database(self->context), &error))
		return venture_web_error_response(error);
	for (i = 0; i < built->len; i++)
	{
		if (!venture_database_save(
			venture_context_get_database(self->context),
			g_ptr_array_index(built, i), &actor, &error))
		{
			venture_database_rollback(venture_context_get_database(self->context));
			return venture_web_error_response(error);
		}
	}
	if (!venture_database_commit(venture_context_get_database(self->context), &error))
		return venture_web_error_response(error);

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

	venture_web_append_save_view_form(content, request, type_name, FALSE);

	g_string_append_printf(content,
		"<a class=\"btn btn-primary\" href=\"/e/%s/new\">New</a>", type_name);
	g_string_append(content, "</div></div>");

	/* Bulk edits, for an editor. The bar stays hidden until a row is
	 * ticked; the tick column is only rendered when the bar is. */
	{
		gboolean bulk;

		bulk = venture_web_require_for_type(self, principal, entity_type,
		                                    VENTURE_USER_ROLE_EDITOR, NULL) &&
		       venture_web_type_accepts_writes(entity_type, NULL);

		if (bulk)
			venture_web_append_bulk_bar(self, content, request, type_name,
			                            prototype);

		g_string_append_printf(content,
			"<div class=\"card\"><div class=\"table-wrap\">"
			"<table class=\"data%s\" data-list><thead><tr>%s",
			bulk ? " selectable" : "",
			bulk ? "<th class=\"tick\"><input type=\"checkbox\" "
			       "data-bulk-all title=\"Select all\"></th>" : "");
	}

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

		g_string_append_printf(content, "<tr data-href=\"/e/%s/%"
		                       G_GINT64_FORMAT "\">",
		                       type_name, venture_entity_get_id(record));

		if (strstr(content->str, "data-bulk-all"))
			g_string_append_printf(content,
				"<td class=\"tick\"><input type=\"checkbox\" data-bulk-id=\"%"
				G_GINT64_FORMAT "\"></td>", venture_entity_get_id(record));

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
		if (venture_statements_owns_report(self->context, venture_report_get_name(report)))
			continue;

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

	venture_statements_append_index(self->context, content);

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
	g_autoptr(JsonObject) report_options = json_object_new();
	g_autofree gchar *rendered = NULL;
	g_autoptr(GString) historical_suffix = g_string_new(NULL);
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

	{
		const gchar *as_of = htmx_request_get_query_param(request, "as_of");
		const gchar *organization = htmx_request_get_query_param(request, "organization_id");
		static const gchar *const strings[] = { "currency", "group_by", "owner", "compare_to", "basis", NULL };
		static const gchar *const integers[] = { "customer_id", "venture_id", "vendor_id", "pipeline_id", "statement_id", "account_id", "days", NULL };
		guint i;
		for (i = 0; strings[i] != NULL; i++)
		{
			const gchar *value = htmx_request_get_query_param(request, strings[i]);
			if (!venture_string_is_empty(value))
				json_object_set_string_member(report_options, strings[i], value);
		}
		for (i = 0; integers[i] != NULL; i++)
		{
			const gchar *value = htmx_request_get_query_param(request, integers[i]);
			if (!venture_string_is_empty(value))
				json_object_set_int_member(report_options, integers[i], g_ascii_strtoll(value, NULL, 10));
		}
		if (!venture_string_is_empty(as_of))
			json_object_set_string_member(report_options, "as_of", as_of);
		if (!venture_string_is_empty(organization))
			json_object_set_int_member(report_options, "organization_id", g_ascii_strtoll(organization, NULL, 10));
	}
	result = venture_report_generate(report, self->context, period, report_options,
	                                 &error);

	if (NULL == result)
	{
		g_autofree gchar *body = NULL;

		body = g_strdup_printf("<div class=\"notice negative\">%s</div>",
		                       error->message);
		return venture_web_html_response(
			venture_web_page(self, request, "/reports", "Error", body), 500);
	}

	{
		const gchar *as_of = venture_json_object_get_string(report_options, "as_of", NULL);
		static const gchar *const names[] = { "customer_id", "venture_id", "currency", "group_by", "vendor_id", "pipeline_id", "owner", "account_id", "compare_to", NULL };
		guint i;
		for (i = 0; names[i] != NULL; i++)
		{
			const gchar *value = htmx_request_get_query_param(request, names[i]);
			if (!venture_string_is_empty(value))
			{
				g_string_append_printf(historical_suffix, "&amp;%s=", names[i]);
				g_string_append_uri_escaped(historical_suffix, value, NULL, FALSE);
			}
		}

		if (NULL != as_of)
		{
			g_string_append(historical_suffix, "&amp;as_of=");
			g_string_append_uri_escaped(historical_suffix, as_of, NULL, FALSE);
		}
		if (json_object_has_member(report_options, "organization_id"))
			g_string_append_printf(historical_suffix, "&amp;organization_id=%" G_GINT64_FORMAT,
				venture_json_object_get_int(report_options, "organization_id", 0));
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
				"<a class=\"btn btn-sm%s\" href=\"/reports/%s?period=%s%s\">%s</a>",
				(0 == g_strcmp0(requested_period, periods[i])) ? " active" : "",
				name, periods[i], historical_suffix->str, periods[i]);
		}

		g_string_append(content, "</div>");
	}

	g_string_append_printf(content,
		"<a class=\"btn btn-sm\" href=\"/api/v1/reports/%s?format=csv&period=%s%s\">"
		"Export CSV</a>", name,
		(NULL != requested_period) ? requested_period : "this_month", historical_suffix->str);
	g_string_append(content, "</div></div>");

	{
		gboolean financial;
		g_object_get(report, "financial", &financial, NULL);
		if (financial)
		{
			g_string_append(content, "<form method=\"get\" class=\"form-grid\"><label>Period<input name=\"period\" value=\"");
			venture_html_escape_append(content, (NULL != requested_period) ? requested_period : "this_month");
			g_string_append(content, "\"></label><label>As of<input name=\"as_of\" placeholder=\"Live totals\" value=\"");
			venture_html_escape_append(content, venture_json_object_get_string(report_options, "as_of", ""));
			g_string_append(content, "\"></label>");
			if (json_object_has_member(report_options, "organization_id"))
				g_string_append_printf(content, "<input type=\"hidden\" name=\"organization_id\" value=\"%" G_GINT64_FORMAT "\">",
					venture_json_object_get_int(report_options, "organization_id", 0));
			{
				static const gchar *const names[] = { "customer_id", "venture_id", "currency", "group_by", "vendor_id", "pipeline_id", "owner", "account_id", "compare_to", NULL };
				guint i;
				/* Preserve the question when changing only its cutoff. */
				for (i = 0; names[i] != NULL; i++)
				{
					const gchar *value = htmx_request_get_query_param(request, names[i]);
					if (value == NULL)
						continue;
					g_string_append_printf(content, "<input type=\"hidden\" name=\"%s\" value=\"", names[i]);
					venture_html_escape_append(content, value);
					g_string_append(content, "\">");
				}
			}
			venture_statements_append_controls(self->context, report, report_options, content);
			g_string_append(content, "<button class=\"btn\" type=\"submit\">Run report</button></form>");
		}
	}

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
	HtmxRequest		*request,
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

	venture_web_append_theme_script(self, html);

	venture_web_append_stylesheet(self, request, html);

	/* An empty ui.accent means the look's own colour -- blue in the
	 * classic stylesheet, red in the industrial -- so nothing is emitted
	 * and each look keeps the accent it was drawn with. */
	if (!venture_string_is_empty(accent))
	{
		g_string_append(html, "<style>:root{--accent-config:");
		venture_html_escape_append(html, accent);
		g_string_append(html, ";}</style>");
	}

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

	html = venture_web_auth_page(self, request,
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

		html = venture_web_auth_page(self, request, body->str);

		return venture_web_html_response(g_steal_pointer(&html), 401);
	}

	response = htmx_response_new();
	htmx_response_set_status(response, 302);
	htmx_response_add_header(response, "Location", "/");
	htmx_response_add_header(response, "Set-Cookie", cookie);

	return response;
}

/*
 * POST /look: remember which design this browser wants.
 *
 * The choice is a plain cookie -- not HttpOnly, there is nothing in it
 * worth hiding from a script, and not tied to the session, because the
 * sign-in page is drawn in it too. A year, because a person who picked a
 * look picked it for good. The page to go back to is the one the switch
 * was on, checked to be a local path so the form cannot be used to send
 * somebody elsewhere.
 */
static HtmxResponse *
venture_web_ui_look(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	HtmxResponse *redirect;
	HtmxResponse *response;
	const gchar *look;
	const gchar *back;

	self = user_data;
	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	look = htmx_request_get_form_value(request, "look");
	back = htmx_request_get_form_value(request, "back");

	if (venture_string_is_empty(back) || ('/' != back[0]) ||
	    g_str_has_prefix(back, "//"))
		back = "/";

	response = venture_web_redirect_to(back);

	if (venture_config_look_is_valid(look))
	{
		g_autoptr(HtmxCookie) cookie = NULL;
		g_autofree gchar *header = NULL;
		gboolean secure;

		g_object_get(venture_context_get_config(self->context),
		             "security-cookie-secure", &secure, NULL);

		cookie = htmx_cookie_new(VENTURE_WEB_LOOK_COOKIE, look);
		htmx_cookie_set_path(cookie, "/");
		htmx_cookie_set_max_age(cookie, 365 * 24 * 3600);
		htmx_cookie_set_same_site(cookie, HTMX_COOKIE_SAME_SITE_LAX);
		htmx_cookie_set_secure(cookie, secure);
		header = htmx_cookie_to_set_cookie(cookie);
		htmx_response_add_header(response, "Set-Cookie", header);
	}

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
static void venture_web_sequence_prefill(VentureEntity *record, HtmxRequest *request);

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
		venture_web_sequence_prefill(record, request);
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

		if (id == 0 && venture_field_spec_get_kind(spec) == VENTURE_FIELD_KIND_REFERENCE)
		{
			g_autofree gchar *wire = g_strdup(venture_field_spec_get_name(spec));
			const gchar *value;
			g_strdelimit(wire, "-", '_');
			value = htmx_request_get_query_param(request, wire);
			if (value != NULL && !venture_entity_set_field_from_string(record, venture_field_spec_get_name(spec), value, &error))
				return venture_web_error_response(error);
		}
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
		if (id == 0 && htmx_request_get_query_param(request, "organization_id") != NULL)
			current_organization = g_ascii_strtoll(htmx_request_get_query_param(request, "organization_id"), NULL, 10);

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

	if (venture_access_policy_requires_approval(venture_database_get_access_policy(venture_context_get_database(self->context)), principal, "write", record, &error)) return venture_web_api_stage(self, record, NULL, principal, VENTURE_AUDIT_ACTION_CREATE);
	if (NULL != error) return venture_web_error_response(error);

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
/*
 * Defined further down, beside the knowledge-base page it shares helpers
 * with, but used from the detail page above it.
 */
static void
venture_web_append_knowledge(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
);

#include "banking/venture-bank-panel.inc"
#include "cutover/venture-cutover-panel.inc"
#include "setup/venture-setup-panel.inc"
#include "backup/venture-backup-web.inc"

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

		/*
		 * kb_link names its subject by type and id rather than by a
		 * reference field, so the walk below would never find it
		 * anyway -- but kb_chunk *does* reference kb_article, and a
		 * knowledge-base article's page listing its own passages as
		 * "kb_chunk #1 ... #47" is a wall of links that say nothing.
		 * Both get a panel of their own instead.
		 */
		if ((VENTURE_TYPE_KB_LINK == types[i]) ||
		    (VENTURE_TYPE_KB_CHUNK == types[i]))
			continue;

		/*
		 * A ticket's comments and worklogs are its timeline, rendered
		 * at the foot of the page in order with its changes; a list of
		 * "ticket_comment #2" links above it says the same thing worse.
		 * Watches and notifications reference a user and are private.
		 */
		if ((VENTURE_TYPE_TICKET_COMMENT == types[i]) ||
		    (VENTURE_TYPE_WORKLOG == types[i]) ||
		    (VENTURE_TYPE_WATCH == types[i]) ||
		    (VENTURE_TYPE_NOTIFICATION == types[i]))
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

			venture_query_set_organization(query, venture_entity_get_organization_id(record));
			if (types[i] == VENTURE_TYPE_ACTIVITY)
				venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "planned", NULL);

			related = venture_database_find(
				venture_context_get_database(self->context), query, NULL);

			if ((NULL == related) || (0 == related->len && types[i] != VENTURE_TYPE_ACTIVITY))
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

			{
				g_autofree gchar *wire = g_strdup(venture_field_spec_get_name(spec));
				g_strdelimit(wire, "-", '_');
				g_string_append_printf(content,
					"</ul><a class=\"btn btn-sm\" href=\"/e/%s/new?%s=%" G_GINT64_FORMAT "&amp;organization_id=%" G_GINT64_FORMAT "\">"
					"New %s</a></div></div>", related_name, wire, venture_entity_get_id(record),
					venture_entity_get_organization_id(record), related_name);
			}
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
		    (0 == g_strcmp0(type_names[i], "record_link")) ||
		    (0 == g_strcmp0(type_names[i], "audit_entry")))
			continue;

		g_string_append(content, "<option value=\"");
		venture_html_escape_append(content, type_names[i]);
		g_string_append(content, "\">");
		venture_html_escape_append(content, type_names[i]);
		g_string_append(content, "</option>");
	}

	/* Searched rather than typed, the same as the link form below it. */
	g_string_append(content,
		"</select> "
		"<span class=\"record-pick\" data-record-pick "
		"data-type-field=\"subject_type\">"
		"<input type=\"number\" name=\"subject_id\" placeholder=\"id\" "
		"min=\"1\" required></span> "
		"<input type=\"text\" name=\"note\" placeholder=\"why (optional)\"> "
		"<button class=\"btn\" type=\"submit\">Relate</button></form>");

	g_string_append(content, "</div></div>");
}

/*
 * Appends a <select> of link kinds, each option carrying the words the
 * kind reads as from the record the form is on.
 */
static void
venture_web_append_link_kind_options(GString *content)
{
	GEnumClass *enum_class;
	guint i;

	enum_class = g_type_class_ref(VENTURE_TYPE_LINK_KIND);

	for (i = 0; i < enum_class->n_values; i++)
	{
		const GEnumValue *value;

		value = &enum_class->values[i];

		g_string_append(content, "<option value=\"");
		venture_html_escape_append(content, value->value_nick);
		g_string_append(content, "\">");
		venture_html_escape_append(content,
			venture_link_kind_to_label((VentureLinkKind)value->value));
		g_string_append(content, "</option>");
	}

	g_type_class_unref(enum_class);
}

/*
 * The "Links" panel, on every detail page.
 *
 * venture_web_append_related() finds records pointing at this one through
 * declared references, and a polymorphic pair is invisible to it. This
 * panel is the other half: every link touching the record, read from
 * where the reader stands, with the form to add one. Offered on every
 * type because that is the point of a link -- an expense can point at the
 * invoice it was billed through as readily as a release at its tickets.
 */
static void
venture_web_append_links(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(GPtrArray) links = NULL;
	g_auto(GStrv) type_names = NULL;
	const gchar *own_type;
	gint64 own_id;
	guint i;

	own_type = venture_entity_get_entity_name(record);
	own_id = venture_entity_get_id(record);

	links = venture_record_link_find_for(
		venture_context_get_database(self->context), own_type, own_id, NULL);

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Links</h2></div><div class=\"card-body\">");

	if ((NULL == links) || (0 == links->len))
	{
		g_string_append(content, "<p class=\"muted\">Nothing linked yet.</p>");
	}
	else
	{
		g_string_append(content, "<ul class=\"relation-list\">");

		for (i = 0; i < links->len; i++)
		{
			g_autoptr(VentureEntity) other = NULL;
			g_autofree gchar *other_type = NULL;
			g_autofree gchar *other_label = NULL;
			g_autofree gchar *note = NULL;
			VentureRecordLink *link;
			VentureLinkKind kind;
			gint64 other_id = 0;

			link = g_ptr_array_index(links, i);

			if (!venture_record_link_other_end(link, own_type, own_id,
			                                   &other_type, &other_id,
			                                   &other_label, &kind))
				continue;

			other = venture_record_link_resolve(
				venture_context_get_database(self->context), other_type,
				other_id, NULL);

			if ((NULL != other) && venture_entity_is_deleted(other))
				g_clear_object(&other);

			g_object_get(link, "note", &note, NULL);

			g_string_append(content, "<li><span class=\"badge\">");
			venture_html_escape_append(content, venture_link_kind_to_label(kind));
			g_string_append(content, "</span> <span class=\"muted\">");
			venture_html_escape_append(content, other_type);
			g_string_append(content, "</span> ");

			/* Linked only while the other end is still there and its
			 * module is on; otherwise the stored label, so the page
			 * can still say what the link was about. */
			if (NULL != other)
			{
				g_autofree gchar *fresh = NULL;

				fresh = venture_entity_get_display_name(other);

				g_string_append_printf(content, "<a href=\"/e/%s/%"
				                                G_GINT64_FORMAT "\">",
				                       other_type, other_id);
				venture_html_escape_append(content,
					!venture_string_is_empty(fresh) ? fresh : other_label);
				g_string_append(content, "</a>");
			}
			else
			{
				venture_html_escape_append(content, other_label);
				g_string_append(content, " <span class=\"muted\">"
				                         "(unavailable)</span>");
			}

			if (!venture_string_is_empty(note))
			{
				g_string_append(content, " <span class=\"muted\">\xe2\x80\x94 ");
				venture_html_escape_append(content, note);
				g_string_append(content, "</span>");
			}

			g_string_append_printf(content,
				" <form method=\"post\" action=\"/links/%" G_GINT64_FORMAT
				"/delete\" class=\"inline\">"
				"<input type=\"hidden\" name=\"back\" value=\"/e/%s/%"
				G_GINT64_FORMAT "\">"
				"<button class=\"btn btn-sm\" type=\"submit\">Unlink</button>"
				"</form></li>",
				venture_entity_get_id(VENTURE_ENTITY(link)), own_type, own_id);
		}

		g_string_append(content, "</ul>");
	}

	/* The picker. Every offered type, so a plugin's type and a module's
	 * are there the moment they are on. */
	type_names = venture_entity_registry_list_names(
		venture_context_get_entity_registry(self->context));

	g_string_append(content, "<form method=\"post\" action=\"/links\" "
	                         "class=\"relate-form\">");
	g_string_append_printf(content,
		"<input type=\"hidden\" name=\"source_type\" value=\"%s\">"
		"<input type=\"hidden\" name=\"source_id\" value=\"%" G_GINT64_FORMAT
		"\">", own_type, own_id);

	g_string_append(content, "<select name=\"kind\">");
	venture_web_append_link_kind_options(content);
	g_string_append(content, "</select> <select name=\"target_type\" required>");

	for (i = 0; NULL != type_names[i]; i++)
	{
		/* Links, relations and the audit log are not things one links
		 * to. */
		if ((0 == g_strcmp0(type_names[i], "record_link")) ||
		    (0 == g_strcmp0(type_names[i], "ticket_relation")) ||
		    (0 == g_strcmp0(type_names[i], "audit_entry")))
			continue;

		g_string_append(content, "<option value=\"");
		venture_html_escape_append(content, type_names[i]);
		g_string_append(content, "\">");
		venture_html_escape_append(content, type_names[i]);
		g_string_append(content, "</option>");
	}

	/*
	 * The target is searched, not typed. Asking for a record's id means
	 * opening the list in another tab to go and read one off it, and the
	 * number that comes back is unverifiable until the link is made: a
	 * mistyped digit silently links to a different record that exists.
	 *
	 * The number input is still what the form posts, and it still works
	 * on its own with scripting off; the script puts a search box in
	 * front of it that fills it in.
	 */
	g_string_append(content,
		"</select> "
		"<span class=\"record-pick\" data-record-pick "
		"data-type-field=\"target_type\">"
		"<input type=\"number\" name=\"target_id\" placeholder=\"id\" "
		"min=\"1\" required></span> "
		"<input type=\"text\" name=\"note\" placeholder=\"why (optional)\"> "
		"<button class=\"btn\" type=\"submit\">Link</button></form>");

	g_string_append(content, "</div></div>");
}

/*
 * Parses a kind nick from a form or JSON field, defaulting to related.
 *
 * Returns: %TRUE if @text named a kind or was empty
 */
static gboolean
venture_web_parse_link_kind(
	const gchar	 *text,
	VentureLinkKind	 *out_kind,
	GError		**error
){
	gint value;

	*out_kind = VENTURE_LINK_KIND_RELATED;

	if (venture_string_is_empty(text))
		return TRUE;

	if (!venture_enum_from_nick(VENTURE_TYPE_LINK_KIND, text, &value))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a link kind", text);
		return FALSE;
	}

	*out_kind = (VentureLinkKind)value;

	return TRUE;
}

/*
 * A path back to a record's page, or the dashboard for a value that is
 * not one. The form sends where it came from; nothing outside this site
 * is ever redirected to.
 */
static gchar *
venture_web_link_return_path(
	const gchar	*back,
	const gchar	*source_type,
	gint64		 source_id
){
	if ((NULL != back) && g_str_has_prefix(back, "/e/") &&
	    (NULL == strstr(back, "//")))
		return g_strdup(back);

	if (!venture_string_is_empty(source_type) && (0 != source_id))
		return g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, source_type,
		                       source_id);

	return g_strdup("/");
}

/*
 * POST /links - link the record the form was on to another.
 */
static HtmxResponse *
venture_web_ui_link_create(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureRecordLink) link = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	VentureLinkKind kind;
	const gchar *source_type;
	const gchar *source_id;
	const gchar *target_type;
	const gchar *target_id;
	gint64 source;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	source_type = htmx_request_get_form_value(request, "source_type");
	source_id = htmx_request_get_form_value(request, "source_id");
	target_type = htmx_request_get_form_value(request, "target_type");
	target_id = htmx_request_get_form_value(request, "target_id");
	source = (NULL != source_id) ? g_ascii_strtoll(source_id, NULL, 10) : 0;

	if (!venture_web_parse_link_kind(
		htmx_request_get_form_value(request, "kind"), &kind, &error))
		return venture_web_error_response(error);

	link = venture_record_link_create(
		venture_context_get_database(self->context), source_type, source,
		kind, target_type,
		(NULL != target_id) ? g_ascii_strtoll(target_id, NULL, 10) : 0,
		htmx_request_get_form_value(request, "note"), &error);

	if (NULL == link)
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(link), &actor, &error))
		return venture_web_error_response(error);

	destination = venture_web_link_return_path(
		htmx_request_get_form_value(request, "back"), source_type, source);

	return venture_web_redirect_to(destination);
}

/*
 * POST /links/:id/delete - drop one, and go back to the page it was on.
 */
static HtmxResponse *
venture_web_ui_link_delete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) link = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	g_autofree gchar *source_type = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	gint64 source_id = 0;
	gint64 id;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	link = venture_database_get(venture_context_get_database(self->context),
	                            VENTURE_TYPE_RECORD_LINK, id, &error);

	if (NULL == link)
		return venture_web_error_response(error);

	g_object_get(link, "source-type", &source_type, "source-id", &source_id,
	             NULL);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_delete(venture_context_get_database(self->context),
	                             link, &actor, &error))
		return venture_web_error_response(error);

	destination = venture_web_link_return_path(
		htmx_request_get_form_value(request, "back"), source_type, source_id);

	return venture_web_redirect_to(destination);
}

/*
 * GET /api/v1/links/:type/:id - every link touching a record, read from it.
 */
static HtmxResponse *
venture_web_api_links(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	GType entity_type;
	gint64 id;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	/* Reading a record's links discloses the record's existence and
	 * name, so the same role the record itself needs. */
	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);

	node = venture_record_link_describe_for(
		venture_context_get_database(self->context),
		g_hash_table_lookup(params, "type"), id, &error);

	if (NULL == node)
		return venture_web_error_response(error);

	return venture_web_json_response(node, 200);
}

/*
 * POST /api/v1/links - create a link from a body naming both ends.
 *
 * The same as POST /api/v1/record_link, with the body's meaning spelled out
 * and ?stage=1 honoured the same way, so a client that has learned the one
 * shape can use either.
 */
static HtmxResponse *
venture_web_api_link_create(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) error = NULL;
	gboolean stage;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	if (!venture_web_stage_requested(request, &stage, &error))
		return venture_web_error_response(error);

	record = VENTURE_ENTITY(venture_record_link_new());

	return venture_web_api_write(self, request, record, NULL, principal, TRUE,
	                             stage);
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
	                         "<th class=\"num\">Net</th>"
	                         "<th class=\"num\">Tax</th>"
	                         "<th class=\"num\">Amount</th></tr></thead>"
	                         "<tbody>");

	for (i = 0; (NULL != lines) && (i < lines->len); i++)
	{
		VentureInvoiceLine *line;
		g_autofree gchar *description = NULL;
		g_autoptr(VentureMoney) unit_price = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(VentureMoney) tax = NULL;
		gdouble quantity;

		line = g_ptr_array_index(lines, i);
		g_object_get(line, "description", &description,
		             "quantity", &quantity,
		             "unit-price", &unit_price,
		             "income-amount", &net, "tax-amount", &tax, NULL);
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
		if (NULL != net)
		{
			g_autofree gchar *text = venture_money_to_display_string(net, TRUE);
			venture_html_escape_append(content, text);
		}
		g_string_append(content, "</td><td class=\"num\">");
		if (NULL != tax)
		{
			g_autofree gchar *text = venture_money_to_display_string(tax, TRUE);
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
	                         "<td colspan=\"5\" class=\"num\">"
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
	{
		gboolean exempt = FALSE;
		g_autofree gchar *reason = NULL;
		g_object_get(record, "tax-exempt", &exempt, "tax-exempt-reason", &reason, NULL);
		if (exempt)
		{
			g_string_append(content, "<p class=\"muted\">Tax exempt");
			if (reason != NULL && *reason != '\0')
			{
				g_string_append(content, ": ");
				venture_html_escape_append(content, reason);
			}
			g_string_append(content, "</p>");
		}
	}

	/* The transitions this status allows, each a form so nothing here
	 * depends on scripting. */
	g_string_append(content, "<div class=\"card-body invoice-actions\">");
	{
		VentureStripeService *stripe = venture_context_get_stripe_service(self->context);
		if (stripe && venture_stripe_service_can_checkout(stripe, id, NULL))
			g_string_append_printf(content, "<form method=\"post\" action=\"/invoices/%" G_GINT64_FORMAT "/checkout\"><button class=\"btn btn-primary\" type=\"submit\">Pay with Stripe</button></form>", id);
	}


	g_string_append_printf(content,
		"<a class=\"btn\" href=\"/invoices/%" G_GINT64_FORMAT
		"/print\" target=\"_blank\">Print</a>", id);

	if (venture_context_module_enabled(self->context, "mail"))
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/invoices/%" G_GINT64_FORMAT
			"/send\"><button class=\"btn\" type=\"submit\">Send</button></form>", id);


	if (VENTURE_INVOICE_STATUS_DRAFT == status)
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/invoices/%" G_GINT64_FORMAT
			"/status\"><input type=\"hidden\" name=\"to\" value=\"sent\">"
			"<button class=\"btn btn-primary\" type=\"submit\">"
			"Mark sent</button></form>", id);

	if ((VENTURE_INVOICE_STATUS_DRAFT == status) ||
	    (VENTURE_INVOICE_STATUS_SENT == status))
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/invoices/%" G_GINT64_FORMAT
			"/status\"><input type=\"hidden\" name=\"to\" value=\"void\">"
			"<button class=\"btn\" type=\"submit\">Void</button></form>",
			id);

	if (!venture_context_module_enabled(self->context, "receivables"))
	{
		g_string_append(content, "</div></div>");
		return;
	}

	if ((VENTURE_INVOICE_STATUS_SENT == status) ||
	    (VENTURE_INVOICE_STATUS_PARTIALLY_PAID == status))
	{
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/invoices/%" G_GINT64_FORMAT
			"/status\"><input type=\"hidden\" name=\"to\" value=\"paid\">"
			"<button class=\"btn btn-primary\" type=\"submit\" "
			"title=\"Records a receipt for the outstanding balance\">"
			"Mark paid</button></form>", id);
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/invoices/%" G_GINT64_FORMAT
			"/status\"><input type=\"hidden\" name=\"to\" value=\"write-off\">"
			"<button class=\"btn\" type=\"submit\" "
			"title=\"Write off remaining AR; journals stay posted\">"
			"Write off</button></form>", id);
	}

	g_string_append(content, "</div></div>");
}

static void
venture_web_append_release_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
);

static void
venture_web_append_milestone_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
);

static void
venture_web_append_environment_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
);

static void
venture_web_append_watch_button(
	VentureWebServer	*self,
	GString			*content,
	VentureAuthPrincipal	*principal,
	VentureEntity		*record
);

static void
venture_web_append_activity(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
);

static void
venture_web_append_ticket_desk_block(
	VentureWebServer	*self,
	GString			*content,
	VentureAuthPrincipal	*principal,
	VentureEntity		*record
);

static void
venture_web_append_sprint_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
);

static void
venture_web_append_incident_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
);

#include "assets/venture-assets-web.inc"
#include "sequences/venture-sequence-web.inc"

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
	/* Watching comes first so it sits beside the title on every page;
	 * the audit log and a notification are not things to follow. */
	if ((VENTURE_TYPE_AUDIT_ENTRY != entity_type) &&
	    (VENTURE_TYPE_NOTIFICATION != entity_type) &&
	    (VENTURE_TYPE_WATCH != entity_type))
		venture_web_append_watch_button(self, content, principal, record);

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

	if (venture_web_module_enabled(self, "federation") &&
		venture_entity_type_get_federation_access(entity_type) &&
		venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER, NULL))
	{
		g_string_append(content, "<section class=\"card\"><h2>Federation sharing</h2><p>This record stays private unless explicitly granted. Use its UUID to select exact fields and peers.</p><code>");
		venture_html_escape_append(content, venture_entity_get_uuid(record));
		g_string_append(content, "</code><p><a class=\"btn\" href=\"/e/federation_grant/new\">Create sharing grant</a> <a href=\"/federation\">Federation workspace</a></p></section>");
	}

	venture_billing_web_buttons(self, content, record, principal);
	venture_web_append_record_actions(self, content, record, principal);
	venture_web_append_related(self, content, record);
	venture_bank_append_actions(content, record);
	venture_cutover_append_actions(content, record);
	venture_setup_append_actions(content, record);
	venture_backup_append_actions(content, record);
	venture_web_sequence_panel(self, content, principal, record);

	/* A link is not offered on a link; the audit log is not linkable. */
	if ((VENTURE_TYPE_RECORD_LINK != entity_type) &&
	    (VENTURE_TYPE_AUDIT_ENTRY != entity_type))
		venture_web_append_links(self, content, record);

	if (venture_web_module_enabled(self, "kb"))
		venture_web_append_knowledge(self, content, record);

	/* An invoice's lines and total, with the actions its status allows.
	 * The other type-specific block, for the same reason as the ticket
	 * composer below: an invoice without its total is a list of hints. */
	if (VENTURE_TYPE_INVOICE == entity_type)
		venture_web_append_invoice_block(self, content, record);
	quote_buttons(content, record);
	venture_web_append_payables_actions(self, content, record);
	venture_web_append_claims_actions(self, content, record);

	if (VENTURE_TYPE_FIXED_ASSET == entity_type)
		venture_web_append_asset_actions(content, record);

	/* A forge's credentials, which the generated form cannot show. */
	if (VENTURE_TYPE_FORGE == entity_type)
		venture_web_append_forge_block(self, content, record);

	/* A repository's address and its clone command, both composed from
	 * the forge rather than stored, so neither can drift from it. */
	if (VENTURE_TYPE_FORGE_REPO == entity_type)
		venture_web_append_repo_block(self, content, record);

	if (VENTURE_IS_LEAD(record)) venture_web_append_lead_actions(content, record);

	/* The factory's pages: what a release shipped and the actions on it,
	 * a milestone's progress, what an environment is running. */
	if (venture_web_module_enabled(self, "factory"))
	{
		if (VENTURE_TYPE_RELEASE == entity_type)
			venture_web_append_release_block(self, content, record);

		if (VENTURE_TYPE_MILESTONE == entity_type)
			venture_web_append_milestone_block(self, content, record);

		if (VENTURE_TYPE_ENVIRONMENT == entity_type)
			venture_web_append_environment_block(self, content, record);
	}

	/*
	 * The other half of a polymorphic relation. On a ticket the panel
	 * above already lists them, and the audit log is deliberately not
	 * relatable, so both are skipped here.
	 */
	if ((VENTURE_TYPE_TICKET != entity_type) &&
	    (VENTURE_TYPE_TICKET_RELATION != entity_type) &&
	    venture_web_module_enabled(self, "tickets"))
		venture_web_append_subject_tickets(self, content, record);

	/*
	 * A ticket's comments are a conversation, and a conversation needs
	 * its reply box on the same page. The related-records section above
	 * already lists the thread; this is the one place the generic detail
	 * page knows a specific type, because "click New, pick the ticket
	 * you were just looking at, type, save" is not how anybody comments.
	 */
	if ((VENTURE_TYPE_TICKET == entity_type) &&
	    venture_web_module_enabled(self, "forge"))
		venture_web_append_ticket_forge_block(self, content, record);

	/* What else this ticket is about, for the many tickets that are
	 * about something other than code. */
	if (VENTURE_TYPE_TICKET == entity_type)
		venture_web_append_ticket_relations(self, content, record);

	/* The desk: service level, macros, time. Above the composer, because
	 * "apply the canned reply" and "write a reply" are the same moment. */
	if (VENTURE_TYPE_TICKET == entity_type)
		venture_web_append_ticket_desk_block(self, content, principal, record);

	if ((VENTURE_TYPE_SPRINT == entity_type) &&
	    venture_web_module_enabled(self, "tickets"))
		venture_web_append_sprint_block(self, content, record);

	if ((VENTURE_TYPE_INCIDENT == entity_type) &&
	    venture_web_module_enabled(self, "factory"))
		venture_web_append_incident_block(self, content, record);

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

	/* What happened, last. The audit log has its own page; this is the
	 * record's own story, with its conversation woven in. */
	if ((VENTURE_TYPE_AUDIT_ENTRY != entity_type) &&
	    (VENTURE_TYPE_NOTIFICATION != entity_type))
		venture_web_append_activity(self, content, record);

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

	/* A sprint's board: the same columns, only what is planned into it. */
	{
		const gchar *sprint;

		sprint = htmx_request_get_query_param(request, "sprint_id");

		if (!venture_string_is_empty(sprint))
		{
			if (!venture_query_add_filter_int(query, "sprint-id",
			                                  VENTURE_FILTER_OP_EQ,
			                                  g_ascii_strtoll(sprint, NULL, 10),
			                                  error))
				return NULL;
		}
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
	gint64 points = 0;

	id = venture_entity_get_id(ticket);

	g_object_get(ticket, "title", &title, "assignee", &assignee,
	             "kind", &kind, "priority", &priority, "due-at", &due,
	             "tags", &tags, "story-points", &points, NULL);

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

	/* The service-level clock, and the weight in the sprint. */
	venture_web_append_sla_badge(content, ticket);

	if (points > 0)
		g_string_append_printf(content,
			"<span class=\"ticket-points\" title=\"Story points\">%"
			G_GINT64_FORMAT "</span>", points);

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

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	/* Anything that has fallen due is marked before the board is drawn,
	 * so a breached card is red the first time anybody looks. */
	venture_sla_sweep(self->context, 50, NULL);
	activity_lazy_sweep(self, request);

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

	venture_web_append_save_view_form(content, request, "ticket", board);
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

	if (!venture_access_policy_has_membership(venture_database_get_access_policy(venture_context_get_database(self->context)), principal)) g_string_append(content, "<div class=\"notice info\">No organization membership. Ask an organization owner to grant access. You can manage your own account here.</div>");

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
 * The knowledge that bears on this record.
 *
 * Hand-written rather than derived, because kb_link names its subject by
 * type and id: the generic walk keys on reference fields and would never
 * find it. Reads the stored links rather than recomputing, so rendering a
 * page costs a query instead of an embedding request -- and each link says
 * when it was computed, because that is the cost of not recomputing.
 */
static void
venture_web_append_knowledge(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(GPtrArray) links = NULL;
	VentureKbService *kb;
	const gchar *type_name;
	gint64 record_id;
	guint i;

	kb = venture_context_get_kb_service(self->context);

	if (NULL == kb)
		return;

	type_name = venture_entity_get_entity_name(record);
	record_id = venture_entity_get_id(record);

	if (!venture_kb_crossref_type_is_eligible(G_OBJECT_TYPE(record)))
		return;

	links = venture_kb_crossref_links_for(kb, type_name, record_id, NULL);

	g_string_append(content,
		"<div class=\"card\"><div class=\"card-body\">"
		"<div class=\"section-head\"><h2>Related knowledge</h2>");

	/*
	 * The button is offered even when links exist, because the record's
	 * text changes and the links do not follow it. Posting to the API
	 * from here rather than a form action keeps the page a GET.
	 */
	g_string_append_printf(content,
		"<button class=\"btn btn-sm\" hx-post=\"/api/v1/kb/crossref/%s/%"
		G_GINT64_FORMAT "\" hx-swap=\"none\" "
		"hx-on::after-request=\"window.location.reload()\">"
		"%s</button></div>",
		type_name, record_id,
		((NULL != links) && (links->len > 0)) ? "Recompute" : "Find related");

	if ((NULL == links) || (0 == links->len))
	{
		g_string_append(content,
			"<p class=\"muted\">Nothing linked yet. Cross-referencing "
			"embeds this record's text and compares it against the "
			"knowledge bases.</p></div></div>");
		return;
	}

	g_string_append(content, "<div class=\"kb-links\">");

	for (i = 0; i < links->len; i++)
	{
		VentureEntity *link = g_ptr_array_index(links, i);
		g_autoptr(VentureEntity) article = NULL;
		g_autofree gchar *excerpt = NULL;
		g_autofree gchar *title = NULL;
		g_autoptr(GDateTime) computed = NULL;
		gint64 article_id;
		gdouble score;

		g_object_get(link, "article-id", &article_id, "score", &score,
		             "excerpt", &excerpt, "computed-at", &computed, NULL);

		article = venture_database_get(
			venture_context_get_database(self->context),
			VENTURE_TYPE_KB_ARTICLE, article_id, NULL);

		if (NULL != article)
			g_object_get(article, "title", &title, NULL);

		g_string_append(content, "<div class=\"kb-link\">");
		g_string_append_printf(content,
			"<a class=\"kb-link-title\" href=\"/e/kb_article/%"
			G_GINT64_FORMAT "\">", article_id);
		venture_html_escape_append(content,
			venture_string_is_empty(title) ? "(deleted article)"
			                               : title);
		g_string_append(content, "</a>");

		/*
		 * The score is shown as a percentage rather than hidden. A
		 * reader deciding whether to follow a link wants to know
		 * whether it is a strong match or the weakest one that got
		 * past the threshold.
		 */
		g_string_append_printf(content,
			"<span class=\"badge\">%.0f%%</span>", score * 100.0);

		if (!venture_string_is_empty(excerpt))
		{
			g_string_append(content, "<p class=\"kb-link-excerpt\">");
			venture_html_escape_append(content, excerpt);
			g_string_append(content, "</p>");
		}

		g_string_append(content, "</div>");
	}

	g_string_append(content, "</div></div></div>");
}

/*
 * The knowledge-base browser: the bases, and one search box across them.
 *
 * A page of its own rather than only the generic /e/knowledge_base list,
 * because searching by meaning is the one thing the generic machinery cannot
 * do -- its list filter matches characters.
 */
static HtmxResponse *
venture_web_ui_kb(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	VentureKbService *kb;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) bases = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	const gchar *search;
	guint i;

	self = user_data;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "kb");

		if (NULL != gate)
			return gate;
	}

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Knowledge</h1>"
	                       "<p class=\"muted\">Documents the assistant can "
	                       "read, searched by meaning.</p></div></div>");

	kb = venture_context_get_kb_service(self->context);

	if (NULL == kb)
	{
		/*
		 * Says which of the two reasons it is. An operator whose
		 * search box has gone wants to know whether to edit the config
		 * or restart the embedding service.
		 */
		g_string_append(content,
			"<div class=\"card\"><div class=\"card-body\">"
			"<div class=\"banner banner-warn\">Knowledge bases are "
			"unavailable. Either <code>kb.enabled</code> is false, or "
			"the embedding service named by <code>kb.embedding_url</code> "
			"could not be reached when the server started.</div>"
			"</div></div>");

		return venture_web_html_response(
			venture_web_page(self, request, "/kb", "Knowledge",
			                 content->str), 200);
	}

	search = htmx_request_get_query_param(request, "q");

	g_string_append(content,
		"<div class=\"card\"><div class=\"card-body\">"
		"<form class=\"kb-search\" action=\"/kb\" method=\"get\">"
		"<input type=\"search\" name=\"q\" placeholder=\"Ask the "
		"knowledge bases…\" value=\"");

	if (!venture_string_is_empty(search))
		venture_html_escape_append(content, search);

	g_string_append(content,
		"\" autofocus>"
		"<button class=\"btn btn-primary\" type=\"submit\">Search</button>"
		"</form>");

	if (!venture_string_is_empty(search))
	{
		g_autoptr(GPtrArray) hits = NULL;

		hits = venture_kb_service_search(kb, search, NULL, 0, 0, &error);

		if (NULL == hits)
		{
			g_string_append(content, "<div class=\"banner banner-warn\">");
			venture_html_escape_append(content,
				(NULL != error) ? error->message
				                : "The search failed");
			g_string_append(content, "</div>");
		}
		else if (0 == hits->len)
		{
			g_string_append(content,
				"<p class=\"muted\">Nothing matched. Only published "
				"articles in bases offered to the assistant are "
				"searched.</p>");
		}
		else
		{
			g_string_append(content, "<div class=\"kb-links\">");

			for (i = 0; i < hits->len; i++)
			{
				const VentureKbHit *hit = g_ptr_array_index(hits, i);

				g_string_append(content, "<div class=\"kb-link\">");
				g_string_append_printf(content,
					"<a class=\"kb-link-title\" href=\"/e/kb_article/%"
					G_GINT64_FORMAT "\">", hit->article_id);
				venture_html_escape_append(content, hit->title);
				g_string_append(content, "</a>");

				g_string_append_printf(content,
					"<span class=\"badge\">%.0f%%</span>"
					"<span class=\"muted small\"> #",
					hit->score * 100.0);
				venture_html_escape_append(content, hit->kb_slug);

				if (!venture_string_is_empty(hit->heading))
				{
					g_string_append(content, " / ");
					venture_html_escape_append(content,
					                           hit->heading);
				}

				g_string_append(content, "</span>");
				g_string_append(content, "<p class=\"kb-link-excerpt\">");
				venture_html_escape_append(content, hit->text);
				g_string_append(content, "</p></div>");
			}

			g_string_append(content, "</div>");
		}
	}

	g_string_append(content, "</div></div>");

	query = venture_query_new(VENTURE_TYPE_KNOWLEDGE_BASE);
	venture_query_set_limit(query, 0);
	bases = venture_database_find(
		venture_context_get_database(self->context), query, NULL);

	g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
	                         "<div class=\"section-head\"><h2>Bases</h2>"
	                         "<a class=\"btn btn-sm\" "
	                         "href=\"/e/knowledge_base/new\">New base</a>"
	                         "</div>"
	                         "<table class=\"table\"><thead><tr>"
	                         "<th>Name</th><th>#slug</th><th>Articles</th>"
	                         "<th>Model</th><th>Synced</th><th></th>"
	                         "</tr></thead><tbody>");

	for (i = 0; (NULL != bases) && (i < bases->len); i++)
	{
		VentureEntity *base = g_ptr_array_index(bases, i);
		g_autoptr(VentureQuery) count_query = NULL;
		g_autoptr(GPtrArray) articles = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *slug = NULL;
		g_autofree gchar *model = NULL;
		g_autoptr(GDateTime) synced = NULL;
		gint64 base_id;
		gboolean automatic = FALSE;

		base_id = venture_entity_get_id(base);
		g_object_get(base, "name", &name, "slug", &slug,
		             "embedding-model", &model, "synced-at", &synced,
		             "auto-retrieve", &automatic, NULL);

		count_query = venture_query_new(VENTURE_TYPE_KB_ARTICLE);
		venture_query_set_limit(count_query, 0);
		venture_query_add_filter_int(count_query, "kb-id",
		                             VENTURE_FILTER_OP_EQ, base_id, NULL);
		articles = venture_database_find(
			venture_context_get_database(self->context), count_query,
			NULL);

		g_string_append_printf(content,
			"<tr><td><a href=\"/e/knowledge_base/%" G_GINT64_FORMAT
			"\">", base_id);
		venture_html_escape_append(content, name);
		g_string_append(content, "</a>");

		if (!automatic)
			g_string_append(content,
				" <span class=\"badge\">not offered to the AI</span>");

		g_string_append(content, "</td><td><code>#");
		venture_html_escape_append(content, slug);
		g_string_append_printf(content, "</code></td><td>%u</td><td class=\"muted small\">",
		                       (NULL != articles) ? articles->len : 0);
		venture_html_escape_append(content,
			venture_string_is_empty(model) ? "not indexed" : model);
		g_string_append(content, "</td><td class=\"muted small\">");

		if (NULL != synced)
		{
			g_autofree gchar *when = NULL;

			when = g_date_time_format(synced, "%Y-%m-%d %H:%M");
			venture_html_escape_append(content, when);
		}
		else
		{
			g_string_append(content, "never");
		}

		/*
		 * Sync, reindex and export, as the three things a base needs
		 * that its record form cannot express. Export is a link
		 * because it is a download; the other two post.
		 */
		/*
		 * Upload posts multipart straight at the API route, so one
		 * ingest path serves the browser, the CLI and a sync. The
		 * accept list is advisory -- the server decides by content,
		 * and a file it cannot read is skipped with a note rather
		 * than refused.
		 */
		g_string_append_printf(content,
			"</td><td class=\"kb-actions\">"
			"<form class=\"kb-upload\" hx-post=\"/api/v1/kb/%"
			G_GINT64_FORMAT "/import\" hx-encoding=\"multipart/form-data\" "
			"hx-swap=\"none\" "
			"hx-on::after-request=\"window.location.reload()\">"
			"<label class=\"btn btn-sm\">Import"
			"<input type=\"file\" name=\"files\" multiple hidden "
			"accept=\".org,.md,.txt,.html,.pdf,.docx,.zip,.tar.gz,.tgz\" "
			"onchange=\"this.form.requestSubmit()\">"
			"</label></form>"
			/*
			 * A second form rather than a second input in the first:
			 * webkitdirectory is a property of the input, and one form
			 * cannot offer both a file picker and a folder picker from
			 * the same control. Deliberately no accept list -- a folder
			 * is picked whole and the browser would apply the filter to
			 * the directory entry itself, not its contents; the server
			 * decides what it can read and notes the rest.
			 */
			"<form class=\"kb-upload\" hx-post=\"/api/v1/kb/%"
			G_GINT64_FORMAT "/import\" hx-encoding=\"multipart/form-data\" "
			"hx-swap=\"none\" "
			"hx-on::after-request=\"window.location.reload()\">"
			"<label class=\"btn btn-sm\" title=\"Add every supported file "
			"in a folder, including subfolders\">Import folder"
			"<input type=\"file\" name=\"files\" multiple hidden "
			"webkitdirectory directory "
			"onchange=\"this.form.requestSubmit()\">"
			"</label></form>"
			"<button class=\"btn btn-sm\" hx-post=\"/api/v1/kb/%"
			G_GINT64_FORMAT "/sync\" hx-swap=\"none\" "
			"hx-on::after-request=\"window.location.reload()\">Sync</button>"
			"<button class=\"btn btn-sm\" hx-post=\"/api/v1/kb/%"
			G_GINT64_FORMAT "/reindex\" hx-swap=\"none\" "
			"hx-on::after-request=\"window.location.reload()\">Reindex</button>"
			"<a class=\"btn btn-sm\" href=\"/api/v1/kb/%"
			G_GINT64_FORMAT "/export?format=zip\">Export</a>"
			"</td></tr>",
			base_id, base_id, base_id, base_id, base_id);
	}

	if ((NULL == bases) || (0 == bases->len))
		g_string_append(content,
			"<tr><td colspan=\"6\" class=\"muted\">No knowledge bases "
			"yet.</td></tr>");

	g_string_append(content, "</tbody></table></div></div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/kb", "Knowledge", content->str),
		200);
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
venture_web_chat_append_prose(
	GString		*html,
	const gchar	*escaped
){
	static gsize initialized = 0;
	static GRegex *bold_regex = NULL;
	static GRegex *code_regex = NULL;
	static GRegex *link_regex = NULL;
	g_autofree gchar *with_bold = NULL;
	g_autofree gchar *with_code = NULL;
	g_autofree gchar *formatted = NULL;
	g_auto(GStrv) blocks = NULL;
	gsize b;

	if (g_once_init_enter(&initialized))
	{
		bold_regex = g_regex_new("\\*\\*([^*\\n]+)\\*\\*", 0, 0, NULL);
		code_regex = g_regex_new("`([^`\\n]+)`", 0, 0, NULL);
		/*
		 * A markdown link, but only to somewhere that is a place: an
		 * http(s) URL or a local path. The text is already escaped, so
		 * a quote in the URL is &quot; and cannot close the attribute;
		 * a javascript: URL never matches at all.
		 */
		link_regex = g_regex_new(
			"\\[([^\\]\\n]+)\\]\\(((?:https?://|/)[^)\\s]+)\\)", 0, 0, NULL);
		g_once_init_leave(&initialized, 1);
	}

	with_bold = g_regex_replace(bold_regex, escaped, -1, 0,
	                            "<strong>\\1</strong>", 0, NULL);
	with_code = g_regex_replace(code_regex, with_bold, -1, 0,
	                            "<code>\\1</code>", 0, NULL);
	formatted = g_regex_replace(link_regex, with_code, -1, 0,
	                            "<a href=\"\\2\" rel=\"noopener\">\\1</a>",
	                            0, NULL);

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
			else if (g_str_has_prefix(line, "&gt; ") ||
			         (0 == g_strcmp0(line, "&gt;")))
			{
				/* A quoted passage -- what the model is citing from
				 * a document or a ticket -- set apart from what it
				 * says about it. */
				g_string_append(html, "<blockquote>");

				while ((NULL != lines[i]) &&
				       g_str_has_prefix(g_strstrip(lines[i]), "&gt;"))
				{
					const gchar *quoted;

					quoted = g_strstrip(lines[i]) + 4;

					if (' ' == *quoted)
						quoted++;

					g_string_append(html, quoted);
					g_string_append(html, " ");
					i++;
				}

				g_string_append(html, "</blockquote>");
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

/*
 * Renders an assistant reply.
 *
 * The pipeline order is the security property: decode the entities the
 * model emitted, escape *everything* for real, then apply formatting to the
 * escaped text. Markdown syntax carries no HTML characters, so the patterns
 * work unchanged on escaped text, and nothing the model says can smuggle
 * markup past the escape.
 *
 * Fenced code is cut out first, whole: a block of YAML or a shell command
 * the model wrote must come through byte for byte, with no list or bold
 * rules applied to its insides, and a blank line in it must not split it
 * into two paragraphs.
 */
static void
venture_web_chat_append_rich(
	GString		*html,
	const gchar	*body
){
	g_autofree gchar *decoded = NULL;
	g_autoptr(GString) escaped = NULL;
	g_autoptr(GString) prose = NULL;
	g_auto(GStrv) lines = NULL;
	gsize i;

	decoded = venture_web_chat_decode_entities(body);

	escaped = g_string_new(NULL);
	venture_html_escape_append(escaped, decoded);

	prose = g_string_new(NULL);
	lines = g_strsplit(escaped->str, "\n", -1);

	for (i = 0; NULL != lines[i]; i++)
	{
		if (g_str_has_prefix(g_strstrip(lines[i]), "```"))
		{
			/* Everything before the fence is prose; everything up
			 * to the closing fence is code. */
			venture_web_chat_append_prose(html, prose->str);
			g_string_truncate(prose, 0);

			g_string_append(html, "<pre><code>");
			i++;

			while ((NULL != lines[i]) &&
			       !g_str_has_prefix(g_strstrip(lines[i]), "```"))
			{
				g_string_append(html, lines[i]);
				g_string_append_c(html, '\n');
				i++;
			}

			g_string_append(html, "</code></pre>");

			/* An unterminated fence ends with the message. */
			if (NULL == lines[i])
				break;

			continue;
		}

		g_string_append(prose, lines[i]);
		g_string_append_c(prose, '\n');
	}

	venture_web_chat_append_prose(html, prose->str);
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
 * The bubble a failed turn leaves behind: what went wrong, and the button
 * that sends the question again.
 *
 * The question itself stays stored. A provider that timed out is not part
 * of the conversation, but the thing that was asked is, and re-asking it
 * should not mean retyping it.
 */
static void
venture_web_chat_append_failure(
	GString		*html,
	const gchar	*message
){
	g_string_append(html, "<div class=\"msg ai\">"
	                      "<span class=\"msg-avatar\">"
	                      VENTURE_SPARK
	                      "</span>"
	                      "<div class=\"msg-content\">"
	                      "<div class=\"notice negative\"><span>");
	venture_html_escape_append(html, message);
	g_string_append(html, "</span></div>"
	                      "<div class=\"chat-retry\">"
	                      "<button type=\"button\" class=\"btn btn-sm\" "
	                      "data-ai-retry>Try again</button>"
	                      "<span class=\"muted small\">The question is kept; "
	                      "a retry sends it again.</span>"
	                      "</div></div></div>");
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
 * What the operator is looking at, for the model.
 *
 * A record page becomes the record: its type, id, name and the fields a
 * person would read, so "summarise this" and "what should I do about it"
 * mean the thing on the screen rather than a guess. Every other page is
 * named -- the inbox, the ticket board, a dashboard -- which is enough
 * for "what needs me here". The same permission check the page itself
 * made is made again, because the path arrives from the browser and a
 * viewer could otherwise name a record type they may not read.
 *
 * Returns: (transfer full) (nullable): a paragraph for the model, or %NULL
 *   when the path names nothing worth saying
 */
static gchar *
venture_web_chat_describe_context(
	VentureWebServer	*self,
	VentureAuthPrincipal	*principal,
	const gchar		*path
){
	g_autoptr(GString) out = NULL;
	g_auto(GStrv) parts = NULL;

	if (venture_string_is_empty(path) || ('/' != path[0]) ||
	    g_str_has_prefix(path, "//") || (strlen(path) > 200))
		return NULL;

	parts = g_strsplit(path + 1, "/", -1);
	out = g_string_new(NULL);

	/* A record: /e/<type>/<id>. */
	if ((0 == g_strcmp0(parts[0], "e")) && (NULL != parts[1]) &&
	    (NULL != parts[2]) && (NULL == parts[3]) &&
	    (0 != g_strcmp0(parts[2], "new")))
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *described = NULL;
		GType type;

		type = venture_entity_registry_lookup(
			venture_context_get_entity_registry(self->context), parts[1]);

		if (G_TYPE_INVALID == type)
			return NULL;

		if (!venture_web_require_for_type(self, principal, type,
		                                  VENTURE_USER_ROLE_VIEWER, &error))
			return NULL;

		/*
		 * The harness writes the record out. It is the one place a
		 * record is rendered for a model -- an @ mention goes through
		 * the same call -- so the page you are looking at and the
		 * record you named cannot describe the same ticket
		 * differently.
		 */
		described = venture_ai_harness_describe_record(
			venture_context_get_ai_harness(self->context), parts[1],
			g_ascii_strtoll(parts[2], NULL, 10));

		if (NULL == described)
			return NULL;

		g_string_append_printf(out,
			"The operator is looking at the %s (at %s). When they say "
			"\"this\" they mean it.\n", described, path);

		return g_string_free(g_steal_pointer(&out), FALSE);
	}

	/* A list: /e/<type>. */
	if ((0 == g_strcmp0(parts[0], "e")) && (NULL != parts[1]) &&
	    (NULL == parts[2]))
	{
		if (G_TYPE_INVALID == venture_entity_registry_lookup(
			venture_context_get_entity_registry(self->context), parts[1]))
			return NULL;

		g_string_append_printf(out,
			"The operator is looking at the list of %s records (%s).",
			parts[1], path);

		return g_string_free(g_steal_pointer(&out), FALSE);
	}

	/* A named page, from the same table the sidebar is drawn from. */
	{
		const VentureWebNavLink *links;
		gsize i;

		links = venture_web_navigation();

		for (i = 0; NULL != links[i].path; i++)
		{
			if (0 != g_strcmp0(links[i].path, path))
				continue;

			g_string_append_printf(out,
				"The operator is on the %s page (%s).",
				links[i].label, path);

			return g_string_free(g_steal_pointer(&out), FALSE);
		}
	}

	if (0 == g_strcmp0(path, "/"))
		return g_strdup("The operator is on the home dashboard.");

	if (g_str_has_prefix(path, "/dashboards/"))
		return g_strdup_printf("The operator is looking at a dashboard (%s).",
		                       path);

	if (g_str_has_prefix(path, "/reports/"))
		return g_strdup_printf("The operator is reading a report (%s).",
		                       path);

	return NULL;
}

/*
 * Whether a path names one record, which is when the transcript keeps a
 * line saying so: a question asked about "this" is meaningless a week
 * later without it.
 */
static gboolean
venture_web_chat_path_is_record(const gchar *path)
{
	g_auto(GStrv) parts = NULL;

	if (venture_string_is_empty(path) || ('/' != path[0]))
		return FALSE;

	parts = g_strsplit(path + 1, "/", -1);

	return (0 == g_strcmp0(parts[0], "e")) && (NULL != parts[1]) &&
	       (NULL != parts[2]) && (NULL == parts[3]) &&
	       (0 != g_strcmp0(parts[2], "new")) &&
	       (g_ascii_strtoll(parts[2], NULL, 10) > 0);
}

/*
 * Starter questions for the page, shown while the conversation is empty.
 *
 * Each is a button carrying the question it asks; the script puts it in
 * the composer and sends it, with the page as context. They are the
 * questions the page is for -- a ticket wants summarising and answering,
 * the inbox wants triage, the home page wants "what needs me" -- plus one
 * that explains what the assistant can do at all, because a blank box
 * with a blinking cursor is the least useful thing to show somebody who
 * has not used it yet.
 */
static void
venture_web_chat_append_starters(
	VentureWebServer	*self,
	GString			*html,
	const gchar		*path
){
	static const struct
	{
		const gchar *prefix;
		gboolean exact;
		const gchar *questions[4];
	} starters[] = {
		{ "/e/ticket/", FALSE,
		  { "Summarise this ticket and what is still open on it",
		    "Draft a reply to the requester",
		    "What should happen next on this, and who should do it",
		    NULL } },
		{ "/e/", FALSE,
		  { "Summarise this record",
		    "What is related to this, and what needs attention",
		    NULL, NULL } },
		{ "/tickets", TRUE,
		  { "Which tickets are overdue or about to breach their SLA",
		    "Triage what is waiting: suggest priorities and owners",
		    NULL, NULL } },
		{ "/inbox", TRUE,
		  { "What in my inbox needs an answer from me today",
		    NULL, NULL, NULL } },
		{ "/sprints", TRUE,
		  { "How is the current sprint going, and what is at risk",
		    NULL, NULL, NULL } },
		{ "/runs", TRUE,
		  { "What did the agents do today, and what did it cost",
		    NULL, NULL, NULL } },
		{ "/reports", FALSE,
		  { "Explain the headline figures on this report",
		    NULL, NULL, NULL } },
		{ "/", TRUE,
		  { "What needs my attention today",
		    "What changed since yesterday",
		    NULL, NULL } }
	};
	static const gchar *const always[] = {
		"What can you do, and what needs my approval",
		NULL
	};
	const gchar *const *questions;
	gsize i;

	if (NULL == venture_context_get_ai_service(self->context))
		return;

	questions = NULL;

	for (i = 0; (NULL == questions) && (i < G_N_ELEMENTS(starters)); i++)
	{
		gboolean matches;

		matches = starters[i].exact
			? (0 == g_strcmp0(path, starters[i].prefix))
			: ((NULL != path) &&
			   g_str_has_prefix(path, starters[i].prefix));

		/* A list page is /e/<type> with no id; the record starters
		 * want an id after the type. */
		if (matches && !starters[i].exact &&
		    (0 == g_strcmp0(starters[i].prefix, "/e/")) &&
		    !venture_web_chat_path_is_record(path))
			matches = FALSE;

		if (matches && !starters[i].exact &&
		    (0 == g_strcmp0(starters[i].prefix, "/e/ticket/")) &&
		    !venture_web_chat_path_is_record(path))
			matches = FALSE;

		if (matches)
			questions = starters[i].questions;
	}

	g_string_append(html, "<div class=\"chat-starters\" id=\"chat-starters\">"
	                      "<div class=\"chat-starters-label\">"
	                      "Try asking</div>");

	for (i = 0; (NULL != questions) && (NULL != questions[i]); i++)
	{
		g_string_append(html, "<button type=\"button\" "
		                      "class=\"chat-starter\" data-ai-starter>");
		venture_html_escape_append(html, questions[i]);
		g_string_append(html, "</button>");
	}

	for (i = 0; NULL != always[i]; i++)
	{
		g_string_append(html, "<button type=\"button\" "
		                      "class=\"chat-starter\" data-ai-starter>");
		venture_html_escape_append(html, always[i]);
		g_string_append(html, "</button>");
	}

	g_string_append(html, "</div>");
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

	/* A filter, once there are enough to lose one in. Client-side: the
	 * list is already here, and a round trip per keystroke for a few
	 * dozen titles would be the slower way to do the same thing. */
	if (threads->len > 6)
		g_string_append(html,
			"<input type=\"search\" class=\"thread-filter\" "
			"placeholder=\"Filter conversations\" data-ai-filter "
			"aria-label=\"Filter conversations\">");

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

		g_string_append_printf(html, "<div class=\"thread-item\" "
		                       "data-thread-item=\"%" G_GINT64_FORMAT "\">",
		                       id);
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
			"<button type=\"button\" class=\"thread-rename\" "
			"title=\"Rename conversation\" "
			"data-ai-rename=\"%" G_GINT64_FORMAT "\">"
			"\xe2\x9c\x8e</button>", id);
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
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "chat");

		if (NULL != gate)
			return gate;
	}

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
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "chat");

		if (NULL != gate)
			return gate;
	}

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
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "chat");

		if (NULL != gate)
			return gate;
	}

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

/*
 * GET /harness - what the assistant can be asked for, and from where.
 *
 * Three things live here that used to have no home: the skills an
 * operator writes as records, the command files this machine already has
 * for other agent tools, and the directories those are read from. Seeing
 * them in one list is how somebody answers "why does /release do that" --
 * the answer is nearly always that two files of the same name exist and
 * one is shadowing the other.
 */
static void
venture_web_harness_append_section(
	GString		*content,
	const gchar	*title,
	const gchar	*blurb,
	GPtrArray	*items
){
	guint i;

	g_string_append(content, "<div class=\"card mb-4\"><div class=\"card-head\">"
	                         "<h2>");
	venture_html_escape_append(content, title);
	g_string_append(content, "</h2></div>");

	if ((NULL == items) || (0 == items->len))
	{
		g_string_append(content, "<div class=\"card-body\">"
		                         "<p class=\"muted mb-0\">");
		venture_html_escape_append(content, blurb);
		g_string_append(content, "</p></div></div>");
		return;
	}

	g_string_append(content, "<div class=\"table-wrap\">"
	                         "<table class=\"data\"><thead><tr>"
	                         "<th>Name</th><th>What it does</th>"
	                         "<th>From</th></tr></thead><tbody>");

	for (i = 0; i < items->len; i++)
	{
		VentureHarnessItem *item;

		item = g_ptr_array_index(items, i);

		g_string_append(content, "<tr><td><code>");
		venture_html_escape_append(content, item->label);
		g_string_append(content, "</code></td><td>");
		venture_html_escape_append(content, item->description);
		g_string_append(content, "</td><td><span class=\"badge\">");
		venture_html_escape_append(content, item->origin);
		g_string_append(content, "</span></td></tr>");
	}

	g_string_append(content, "</tbody></table></div></div>");
}

static void
venture_web_harness_append_paths(
	GString		*content,
	const gchar	*title,
	gchar	       **paths
){
	gsize i;

	g_string_append(content, "<h3 class=\"mt-4\">");
	venture_html_escape_append(content, title);
	g_string_append(content, "</h3><ul class=\"related\">");

	for (i = 0; (NULL != paths) && (NULL != paths[i]); i++)
	{
		g_string_append(content, "<li><code>");
		venture_html_escape_append(content, paths[i]);
		g_string_append(content, "</code></li>");
	}

	if ((NULL == paths) || (NULL == paths[0]))
		g_string_append(content, "<li class=\"muted\">None</li>");

	g_string_append(content, "</ul>");
}

static HtmxResponse *
venture_web_ui_assistant(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) skills = NULL;
	g_autoptr(GPtrArray) commands = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(GPtrArray) agents = NULL;
	g_autoptr(GString) content = NULL;
	g_auto(GStrv) command_paths = NULL;
	g_auto(GStrv) skill_paths = NULL;
	VentureAiHarness *harness;
	HtmxResponse *redirect;
	guint i;

	self = user_data;
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "chat");

		if (NULL != gate)
			return gate;
	}

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);
	harness = venture_context_get_ai_harness(self->context);
	venture_ai_harness_refresh(harness);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<span class=\"eyebrow\">Assistant</span>"
	                       "<h1>Commands</h1><span class=\"subtitle\">"
	                       "What the assistant can be asked for: the skills "
	                       "kept here, the command files this machine already "
	                       "has, and what an @ can name. For an agent that "
	                       "writes code, see the Harness."
	                       "</span></div><div class=\"page-actions\">"
	                       "<a class=\"btn btn-primary\" href=\"/e/ai_skill\">"
	                       "Skills</a>"
	                       "<a class=\"btn\" href=\"/e/ai_skill/new\">"
	                       "New skill</a></div></div>");

	/* This install's own, which shadow anything of the same name. */
	{
		g_autoptr(GPtrArray) infos = NULL;

		skills = g_ptr_array_new_with_free_func(
			(GDestroyNotify)venture_harness_item_free);
		infos = venture_ai_skills_list(self->context, NULL);

		for (i = 0; (NULL != infos) && (i < infos->len); i++)
		{
			VentureAiSkillInfo *info;
			VentureHarnessItem *item;

			info = g_ptr_array_index(infos, i);
			item = g_new0(VentureHarnessItem, 1);
			item->kind = VENTURE_HARNESS_COMPLETION_COMMAND;
			item->insert = g_strdup_printf("/%s ", info->trigger);
			item->label = g_strdup_printf("/%s", info->trigger);
			item->name = g_strdup(info->name);
			item->description = g_strdup(info->description);
			item->origin = g_strdup(info->builtin ? "built in" : "yours");
			g_ptr_array_add(skills, item);
		}
	}

	venture_web_harness_append_section(content, "Skills",
		"None yet.", skills);

	files = venture_ai_harness_list(harness, AI_RESOURCE_COMMAND);
	venture_web_harness_append_section(content, "Command files",
		"No command files found in the directories below. A markdown file "
		"with a description in its frontmatter becomes a slash command.",
		files);

	agents = venture_ai_harness_list(harness, AI_RESOURCE_AGENT);
	venture_web_harness_append_section(content, "Agents",
		"No agent files found. These are read but not yet dispatched from "
		"the web composer.", agents);

	commands = venture_ai_harness_list(harness, AI_RESOURCE_SKILL);
	venture_web_harness_append_section(content, "Skill files",
		"No skill files found.", commands);

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Where these come from</h2></div>"
	                         "<div class=\"card-body\">"
	                         "<p class=\"muted\">Read in order; a project "
	                         "directory beats your home directory, and a skill "
	                         "kept here beats both.</p>");

	command_paths = venture_ai_harness_search_paths(harness,
	                                                AI_RESOURCE_COMMAND);
	skill_paths = venture_ai_harness_search_paths(harness, AI_RESOURCE_SKILL);
	venture_web_harness_append_paths(content, "Commands", command_paths);
	venture_web_harness_append_paths(content, "Skills", skill_paths);
	g_string_append(content, "</div></div>");

	/* What an @ reaches, said plainly rather than left to be discovered. */
	g_string_append(content, "<div class=\"card mt-4\">"
	                         "<div class=\"card-head\">"
	                         "<h2>Naming a record</h2></div>"
	                         "<div class=\"card-body\">"
	                         "<p>Type <code>@</code> in the composer to name "
	                         "something the question is about: "
	                         "<code>@ticket/12</code>, <code>@release/4</code>, "
	                         "<code>@incident/2</code>. The menu offers the "
	                         "record types you may read, then searches that "
	                         "type. The record's fields go to the model ahead "
	                         "of the question; the transcript keeps the "
	                         "<code>@</code> token.</p>"
	                         "<p class=\"mb-0 muted\">A <code>#</code> names a "
	                         "knowledge base to read from instead.</p>"
	                         "</div></div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/assistant", "Assistant",
		                 content->str),
		200);
}

/*
 * GET /ui/records/search - records of one type, by name.
 *
 * What the link picker searches. Deliberately not the generic list API:
 * that returns whole records, and this is a menu that needs an id and
 * something to read. The same per-type permission the list page applies
 * is applied here, so a search cannot be used to enumerate a type the
 * caller may not open.
 */
static HtmxResponse *
venture_web_ui_records_search(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *type_name;
	const gchar *text;
	GType type;
	guint i;

	self = user_data;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	type_name = htmx_request_get_query_param(request, "type");
	text = htmx_request_get_query_param(request, "q");

	type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(self->context), type_name);

	if (G_TYPE_INVALID == type)
	{
		venture_entity_registry_set_unknown_type_error(
			venture_context_get_entity_registry(self->context), type_name,
			&error);
		return venture_web_error_response(error);
	}

	if (!venture_web_require_for_type(self, principal, type,
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	query = venture_query_new(type);

	/* All digits is an id: somebody who knows the number should not have
	 * to find the record by its name to use it. */
	if (!venture_string_is_empty(text))
	{
		gboolean numeric;
		gsize c;

		numeric = TRUE;

		for (c = 0; '\0' != text[c]; c++)
		{
			if (!g_ascii_isdigit(text[c]))
			{
				numeric = FALSE;
				break;
			}
		}

		if (numeric)
			venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ,
			                             g_ascii_strtoll(text, NULL, 10),
			                             NULL);
		else
			venture_query_set_search(query, text);
	}

	venture_query_set_limit(query, 12);
	found = venture_database_find(venture_context_get_database(self->context),
	                              query, &error);

	if (NULL == found)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "items");
	json_builder_begin_array(builder);

	for (i = 0; i < found->len; i++)
	{
		VentureEntity *record;
		g_autofree gchar *label = NULL;

		record = g_ptr_array_index(found, i);
		label = venture_entity_get_display_name(record);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, venture_entity_get_id(record));
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder,
			!venture_string_is_empty(label) ? label : "(untitled)");
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/* ==========================================================================
 * The agent harness
 *
 * A coding agent, kept open, in a workspace. Every turn is streamed: the
 * page holds a connection open and the service's session-output signal is
 * forwarded down it, so somebody watching sees the agent working rather
 * than a blank panel for the length of a turn.
 *
 * Separate from the assistant panel on purpose. That answers questions
 * about records; this runs `claude-code` or `codex` against a checkout and
 * changes files.
 * ========================================================================== */

/* One browser watching one session. */
typedef struct
{
	VentureWebServer	*self;
	HtmxSseConnection	*connection;
	gint64			 session_id;
	gulong			 output_handler;
	gulong			 finished_handler;
	gboolean		 open;
} VentureWebSessionWatch;

static void
venture_web_session_watch_free(VentureWebSessionWatch *watch)
{
	VentureWorkService *work;

	if (NULL == watch)
		return;

	work = venture_context_get_work_service(watch->self->context);

	if (NULL != work)
	{
		if (0 != watch->output_handler)
			g_signal_handler_disconnect(work, watch->output_handler);

		if (0 != watch->finished_handler)
			g_signal_handler_disconnect(work, watch->finished_handler);
	}

	g_clear_object(&watch->connection);
	g_free(watch);
}

/* A GClosureNotify, so the watch goes when the connection's handler does
 * rather than needing a second place to remember it. */
static void
venture_web_session_watch_destroy(
	gpointer	 data,
	GClosure	*closure
){
	(void)closure;

	venture_web_session_watch_free(data);
}

static void
venture_web_session_watch_closed(
	HtmxSseConnection	*connection,
	gpointer		 user_data
){
	VentureWebSessionWatch *watch = user_data;

	(void)connection;

	/*
	 * The tab went. The turn is not cancelled -- an agent halfway
	 * through editing a file should finish and say so, and the turn is
	 * stored either way -- but nothing more is written to this socket.
	 */
	watch->open = FALSE;
}

static void
venture_web_session_watch_send(
	VentureWebSessionWatch	*watch,
	const gchar		*event,
	const gchar		*data
){
	if (!watch->open || venture_string_is_empty(data))
		return;

	if (!htmx_sse_connection_is_connected(watch->connection))
	{
		watch->open = FALSE;
		return;
	}

	htmx_sse_connection_send_event(watch->connection, event, data, NULL);
}

static void
venture_web_session_on_output(
	VentureWorkService	*work,
	gint64			 session_id,
	const gchar		*text,
	gpointer		 user_data
){
	VentureWebSessionWatch *watch = user_data;

	(void)work;

	if (session_id != watch->session_id)
		return;

	venture_web_session_watch_send(watch, "delta", text);
}

static void
venture_web_session_on_finished(
	VentureWorkService	*work,
	gint64			 session_id,
	const gchar		*failure,
	gpointer		 user_data
){
	VentureWebSessionWatch *watch = user_data;

	(void)work;

	if (session_id != watch->session_id)
		return;

	/*
	 * The page reloads the transcript rather than being handed it: the
	 * turn is a stored record now, and rendering it here would be a
	 * second renderer to keep in step with the one the page already has.
	 */
	venture_web_session_watch_send(watch, "done",
		venture_string_is_empty(failure) ? "ok" : failure);
}

/*
 * Reads a session the caller is allowed to see.
 *
 * Sessions are not personal the way a chat thread is -- a coding agent
 * working in a shared repository is the team's business -- so this is the
 * ordinary per-type check rather than a per-user one.
 *
 * Returns: (transfer full) (nullable): the session
 */
static VentureEntity *
venture_web_session_get(
	VentureWebServer	 *self,
	VentureAuthPrincipal	 *principal,
	gint64			  session_id,
	GError			**error
){
	if (!venture_web_require_for_type(self, principal,
	                                  VENTURE_TYPE_AGENT_SESSION,
	                                  VENTURE_USER_ROLE_EDITOR, error))
		return NULL;

	return venture_database_get(venture_context_get_database(self->context),
	                            VENTURE_TYPE_AGENT_SESSION, session_id, error);
}

/* Renders the turns of one session. */
static void
venture_web_session_append_turns(
	VentureWebServer	*self,
	GString			*content,
	gint64			 session_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) turns = NULL;
	guint i;

	query = venture_query_new(VENTURE_TYPE_AGENT_TURN);
	venture_query_add_filter_int(query, "session-id", VENTURE_FILTER_OP_EQ,
	                             session_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	turns = venture_database_find(venture_context_get_database(self->context),
	                              query, NULL);

	for (i = 0; (NULL != turns) && (i < turns->len); i++)
	{
		g_autofree gchar *body = NULL;
		VentureChatRole role;

		g_object_get(g_ptr_array_index(turns, i), "role", &role,
		             "body", &body, NULL);

		g_string_append_printf(content, "<div class=\"harness-turn %s\">"
		                                "<div class=\"harness-who\">%s</div>"
		                                "<pre class=\"harness-body\">",
		                       (VENTURE_CHAT_ROLE_USER == role)
		                               ? "asked" : "said",
		                       (VENTURE_CHAT_ROLE_USER == role)
		                               ? "You" : "Agent");
		venture_html_escape_append(content, body);
		g_string_append(content, "</pre></div>");
	}

	if ((NULL == turns) || (0 == turns->len))
		g_string_append(content, "<p class=\"muted\">Nothing asked yet. "
		                         "Describe what you want done.</p>");
}

/*
 * GET /ui/models?provider= - what a provider can be asked to run.
 *
 * The models and the effort levels both come from the catalogue, which is
 * generated from ai-glib's own headers, so what a dropdown offers is what
 * the library will actually accept. A provider ai-glib names no models for
 * -- ollama, whose models are whatever has been pulled locally -- answers
 * with an empty list and the form falls back to a text box.
 */
static HtmxResponse *
venture_web_ui_models(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *const *models;
	const gchar *const *efforts;
	const gchar *provider;
	const gchar *fallback;
	gsize i;

	self = user_data;
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	provider = htmx_request_get_query_param(request, "provider");

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "provider");
	json_builder_add_string_value(builder, (NULL != provider) ? provider : "");

	json_builder_set_member_name(builder, "cli");
	json_builder_add_boolean_value(builder,
	                               venture_ai_provider_is_cli(provider));

	fallback = venture_ai_provider_default_model(provider);
	json_builder_set_member_name(builder, "default");
	json_builder_add_string_value(builder, (NULL != fallback) ? fallback : "");

	json_builder_set_member_name(builder, "models");
	json_builder_begin_array(builder);
	models = venture_ai_provider_models(provider);

	for (i = 0; (NULL != models) && (NULL != models[i]); i++)
		json_builder_add_string_value(builder, models[i]);

	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "efforts");
	json_builder_begin_array(builder);
	efforts = venture_ai_provider_efforts(provider);

	for (i = 0; (NULL != efforts) && (NULL != efforts[i]); i++)
		json_builder_add_string_value(builder, efforts[i]);

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * GET /harness - the sessions, and the form that opens one.
 */
static HtmxResponse *
venture_web_ui_harness(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) sessions = NULL;
	g_autoptr(GString) content = NULL;
	g_auto(GStrv) roots = NULL;
	HtmxResponse *redirect;
	guint i;

	self = user_data;
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<span class=\"eyebrow\">Software factory</span>"
	                       "<h1>Harness</h1><span class=\"subtitle\">"
	                       "A coding agent, kept open, working in a checkout. "
	                       "Every turn is streamed as it happens."
	                       "</span></div></div>");

	if (NULL == venture_context_get_work_service(self->context))
	{
		g_string_append(content,
			"<div class=\"notice warning\"><span>Coding runs are off, so "
			"the harness has nothing to drive. Set "
			"<code>forge.runs_enabled: true</code> and restart."
			"</span></div>");

		return venture_web_html_response(
			venture_web_page(self, request, "/harness", "Harness",
			                 content->str), 200);
	}

	/* What a session may be pointed at, said plainly: the commonest
	 * first question is why a path was refused. */
	g_object_get(venture_context_get_config(self->context),
	             "forge-workspace-roots", &roots, NULL);

	g_string_append(content, "<div class=\"card mb-4\">"
	                         "<div class=\"card-head\"><h2>New session</h2>"
	                         "</div><div class=\"card-body\">"
	                         "<form method=\"post\" action=\"/harness\" "
	                         "class=\"harness-open\">");

	g_string_append(content, "<div class=\"form-row\">"
	                         "<div class=\"field\"><label>"
	                         "<span class=\"field-label\">Name</span>"
	                         "<input type=\"text\" name=\"name\" "
	                         "placeholder=\"What this is for\" required>"
	                         "</label></div>"
	                         "<div class=\"field\"><label>"
	                         "<span class=\"field-label\">Provider</span>"
	                         "<select name=\"provider\" data-provider-select>");

	/*
	 * Every provider ai-glib can build, in its own order, with the CLI
	 * ones said so: which kind it is decides whether the session can
	 * edit files with the agent's own tools or VENTURE's, and it is not
	 * inferable from a name like "antigravity".
	 */
	{
		const gchar *const *providers;
		gsize p;

		providers = venture_ai_providers();

		for (p = 0; (NULL != providers) && (NULL != providers[p]); p++)
		{
			g_string_append(content, "<option value=\"");
			venture_html_escape_append(content, providers[p]);
			g_string_append(content, "\"");

			if (0 == g_strcmp0(providers[p], "claude-code"))
				g_string_append(content, " selected");

			g_string_append(content, ">");
			venture_html_escape_append(content, providers[p]);
			g_string_append(content,
				venture_ai_provider_is_cli(providers[p])
					? " (CLI)" : " (API)");
			g_string_append(content, "</option>");
		}
	}

	/*
	 * The model and the effort are filled in from /ui/models when a
	 * provider is chosen, and again whenever it changes. Rendered empty
	 * rather than pre-filled for the default provider so there is one
	 * path that populates them rather than two that can disagree; with
	 * scripting off both fall back to a text box, which still works
	 * because the service takes a model by name.
	 */
	g_string_append(content, "</select></label></div>"
	                         "<div class=\"field\" data-model-field><label>"
	                         "<span class=\"field-label\">Model</span>"
	                         "<input type=\"text\" name=\"model\" "
	                         "data-model-input "
	                         "placeholder=\"the provider's default\">"
	                         "</label></div>"
	                         "<div class=\"field\" data-effort-field hidden>"
	                         "<label>"
	                         "<span class=\"field-label\">Effort</span>"
	                         "<input type=\"text\" name=\"effort\" "
	                         "data-effort-input "
	                         "placeholder=\"the provider's default\">"
	                         "</label></div></div>");

	g_string_append(content, "<div class=\"form-row\">"
	                         "<div class=\"field\"><label>"
	                         "<span class=\"field-label\">Directory</span>"
	                         "<input type=\"text\" name=\"workspace\" "
	                         "placeholder=\"/path/to/a/checkout\">"
	                         "</label>"
	                         "<span class=\"field-help\">");

	if ((NULL == roots) || (NULL == roots[0]))
	{
		g_string_append(content,
			"No workspace roots are configured, so a directory here will "
			"be refused. Set <code>forge.workspace_roots</code>, or name a "
			"repository instead and the harness will clone it.");
	}
	else
	{
		g_string_append(content, "Must be under: ");

		for (i = 0; NULL != roots[i]; i++)
		{
			if (0 != i)
				g_string_append(content, ", ");

			g_string_append(content, "<code>");
			venture_html_escape_append(content, roots[i]);
			g_string_append(content, "</code>");
		}
	}

	g_string_append(content, "</span></div>"
	                         "<div class=\"field\"><label>"
	                         "<span class=\"field-label\">Or a repository</span>"
	                         "<span class=\"record-pick\" data-record-pick "
	                         "data-type=\"forge_repo\">"
	                         "<input type=\"number\" name=\"repo_id\" "
	                         "min=\"1\"></span></label>"
	                         "<span class=\"field-help\">Cloned into the "
	                         "workspace directory, the way a run is.</span>"
	                         "</div>"
	                         "<div class=\"field\"><label>"
	                         "<span class=\"field-label\">About a ticket</span>"
	                         "<span class=\"record-pick\" data-record-pick "
	                         "data-type=\"ticket\">"
	                         "<input type=\"number\" name=\"ticket_id\" "
	                         "min=\"1\"></span></label></div></div>");

	g_string_append(content, "<div class=\"form-actions\">"
	                         "<button class=\"btn btn-primary\" "
	                         "type=\"submit\">Open session</button>"
	                         "</div></form></div></div>");

	query = venture_query_new(VENTURE_TYPE_AGENT_SESSION);
	venture_query_add_order(query, "last-activity-at", VENTURE_SORT_DESCENDING,
	                        NULL);
	venture_query_set_limit(query, 50);
	sessions = venture_database_find(venture_context_get_database(self->context),
	                                 query, NULL);

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Sessions</h2></div>");

	if ((NULL == sessions) || (0 == sessions->len))
	{
		g_string_append(content, "<div class=\"empty\">"
		                         "<h3>No sessions yet</h3>"
		                         "<p class=\"muted\">Open one above.</p>"
		                         "</div></div>");
	}
	else
	{
		g_string_append(content, "<div class=\"table-wrap\">"
		                         "<table class=\"data\"><thead><tr>"
		                         "<th>Name</th><th>Provider</th>"
		                         "<th>Workspace</th><th>State</th>"
		                         "<th class=\"num\">Turns</th>"
		                         "</tr></thead><tbody>");

		for (i = 0; i < sessions->len; i++)
		{
			VentureEntity *session;
			g_autofree gchar *name = NULL;
			g_autofree gchar *provider = NULL;
			g_autofree gchar *workspace = NULL;
			VentureAgentSessionState state;
			gint64 turns = 0;

			session = g_ptr_array_index(sessions, i);
			g_object_get(session, "name", &name, "provider", &provider,
			             "workspace", &workspace, "state", &state,
			             "turns", &turns, NULL);

			g_string_append_printf(content,
				"<tr><td><a href=\"/harness/%" G_GINT64_FORMAT "\">",
				venture_entity_get_id(session));
			venture_html_escape_append(content,
				!venture_string_is_empty(name) ? name : "Session");
			g_string_append(content, "</a></td><td><code>");
			venture_html_escape_append(content, provider);
			g_string_append(content, "</code></td><td class=\"truncate\">");
			venture_html_escape_append(content,
				!venture_string_is_empty(workspace) ? workspace : "\xe2\x80\x94");
			g_string_append_printf(content, "</td><td>"
				"<span class=\"badge %s\">",
				(VENTURE_AGENT_SESSION_STATE_FAILED == state) ? "negative"
					: (VENTURE_AGENT_SESSION_STATE_WORKING == state)
						? "warning" : "");
			venture_html_escape_append(content,
				venture_enum_to_nick(VENTURE_TYPE_AGENT_SESSION_STATE,
				                     (gint)state));
			g_string_append_printf(content, "</span></td>"
				"<td class=\"num\">%" G_GINT64_FORMAT "</td></tr>", turns);
		}

		g_string_append(content, "</tbody></table></div></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/harness", "Harness", content->str),
		200);
}

/*
 * GET /harness/:id - one session, and the box that drives it.
 */
static HtmxResponse *
venture_web_ui_harness_session(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) session = NULL;
	g_autoptr(GString) content = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *provider = NULL;
	g_autofree gchar *model = NULL;
	g_autofree gchar *workspace = NULL;
	g_autofree gchar *failure = NULL;
	g_autoptr(GError) error = NULL;
	VentureAgentSessionState state;
	HtmxResponse *redirect;
	gint64 session_id;
	gint64 turns = 0;

	self = user_data;
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);
	session_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	session = venture_web_session_get(self, principal, session_id, &error);

	if (NULL == session)
		return venture_web_error_response(error);

	g_object_get(session, "name", &name, "provider", &provider,
	             "model", &model, "workspace", &workspace, "state", &state,
	             "turns", &turns, "failure-reason", &failure, NULL);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<span class=\"eyebrow\">Harness</span><h1>");
	venture_html_escape_append(content,
		!venture_string_is_empty(name) ? name : "Session");
	g_string_append(content, "</h1><span class=\"subtitle\"><code>");
	venture_html_escape_append(content, provider);

	if (!venture_string_is_empty(model))
	{
		g_string_append(content, "</code> <code>");
		venture_html_escape_append(content, model);
	}

	g_string_append(content, "</code>");

	{
		g_autofree gchar *effort = NULL;

		g_object_get(session, "effort", &effort, NULL);

		if (!venture_string_is_empty(effort))
		{
			g_string_append(content, " at <code>");
			venture_html_escape_append(content, effort);
			g_string_append(content, "</code> effort");
		}
	}

	if (!venture_string_is_empty(workspace))
	{
		g_string_append(content, " in <code>");
		venture_html_escape_append(content, workspace);
		g_string_append(content, "</code>");
	}
	else
	{
		g_string_append(content, " with no working directory, so it can "
		                         "plan but not edit");
	}

	g_string_append(content, "</span></div><div class=\"page-actions\">"
	                         "<a class=\"btn\" href=\"/harness\">"
	                         "All sessions</a>");

	if (VENTURE_AGENT_SESSION_STATE_CLOSED != state)
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/harness/%" G_GINT64_FORMAT
			"/close\" class=\"inline\">"
			"<button class=\"btn\" type=\"submit\">Close</button></form>",
			session_id);

	g_string_append(content, "</div></div>");

	if (!venture_string_is_empty(failure))
	{
		g_string_append(content, "<div class=\"notice negative\"><span>");
		venture_html_escape_append(content, failure);
		g_string_append(content, "</span></div>");
	}

	/*
	 * The transcript, then the live region the stream writes into, then
	 * the box. The stream's own element carries the session id; the
	 * script opens it and reloads the page when a turn finishes, which
	 * is how the finished turn gets rendered by the one renderer that
	 * exists rather than by a second one in JavaScript.
	 */
	g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
	                         "<div class=\"harness-log\" id=\"harness-log\">");
	venture_web_session_append_turns(self, content, session_id);
	g_string_append(content, "</div>");

	g_string_append_printf(content,
		"<div class=\"harness-live\" id=\"harness-live\" hidden "
		"data-harness-stream=\"/harness/%" G_GINT64_FORMAT "/stream\">"
		"<div class=\"harness-who\">Agent</div>"
		"<pre class=\"harness-body\" data-harness-text></pre></div>",
		session_id);

	if (VENTURE_AGENT_SESSION_STATE_CLOSED == state)
	{
		g_string_append(content, "<p class=\"muted\">This session is "
		                         "closed.</p>");
	}
	else
	{
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/harness/%" G_GINT64_FORMAT
			"/send\" class=\"harness-send\" data-harness-send>"
			"<textarea name=\"prompt\" rows=\"3\" required "
			"placeholder=\"What should the agent do?\"></textarea>"
			"<button class=\"btn btn-primary\" type=\"submit\">Send</button>"
			"</form>", session_id);
	}

	g_string_append(content, "</div></div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/harness", "Harness", content->str),
		200);
}

/*
 * POST /harness - open a session.
 */
static HtmxResponse *
venture_web_ui_harness_open(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	VentureAgentSessionSpec spec;
	VentureWorkService *work;
	g_autofree gchar *path = NULL;
	gint64 session_id;

	self = user_data;
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

	principal = venture_auth_authenticate(self->auth, request);

	/* Opening a session is starting a coding agent against a checkout.
	 * That is an editor's authority at least. */
	if (!venture_web_require_for_type(self, principal,
	                                  VENTURE_TYPE_AGENT_SESSION,
	                                  VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);

	work = venture_context_get_work_service(self->context);

	if (NULL == work)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Coding runs are off; set forge.runs_enabled");
		return venture_web_error_response(error);
	}

	memset(&spec, 0, sizeof(spec));
	spec.name = htmx_request_get_form_value(request, "name");
	spec.provider = htmx_request_get_form_value(request, "provider");
	spec.model = htmx_request_get_form_value(request, "model");
	spec.effort = htmx_request_get_form_value(request, "effort");
	spec.workspace = htmx_request_get_form_value(request, "workspace");
	spec.user_id = principal->user_id;

	{
		const gchar *repo;
		const gchar *ticket;

		repo = htmx_request_get_form_value(request, "repo_id");
		ticket = htmx_request_get_form_value(request, "ticket_id");
		spec.repo_id = (NULL != repo) ? g_ascii_strtoll(repo, NULL, 10) : 0;
		spec.ticket_id = (NULL != ticket)
			? g_ascii_strtoll(ticket, NULL, 10) : 0;
	}

	session_id = venture_work_service_session_open(work, &spec, &error);

	if (0 == session_id)
		return venture_web_error_response(error);

	path = g_strdup_printf("/harness/%" G_GINT64_FORMAT, session_id);

	return venture_web_redirect_to(path);
}

/*
 * POST /harness/:id/send - one turn.
 */
static HtmxResponse *
venture_web_ui_harness_send(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) session = NULL;
	g_autoptr(GError) error = NULL;
	VentureWorkService *work;
	g_autofree gchar *path = NULL;
	gint64 session_id;

	self = user_data;
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

	principal = venture_auth_authenticate(self->auth, request);
	session_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	session = venture_web_session_get(self, principal, session_id, &error);

	if (NULL == session)
		return venture_web_error_response(error);

	work = venture_context_get_work_service(self->context);

	if (NULL == work)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Coding runs are off; set forge.runs_enabled");
		return venture_web_error_response(error);
	}

	if (!venture_work_service_session_send(work, session_id,
		htmx_request_get_form_value(request, "prompt"), &error))
		return venture_web_error_response(error);

	path = g_strdup_printf("/harness/%" G_GINT64_FORMAT, session_id);

	return venture_web_redirect_to(path);
}

/*
 * POST /harness/:id/close - end one.
 */
static HtmxResponse *
venture_web_ui_harness_close(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) session = NULL;
	g_autoptr(GError) error = NULL;
	VentureWorkService *work;
	g_autofree gchar *path = NULL;
	gint64 session_id;

	self = user_data;
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

	principal = venture_auth_authenticate(self->auth, request);
	session_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	session = venture_web_session_get(self, principal, session_id, &error);

	if (NULL == session)
		return venture_web_error_response(error);

	work = venture_context_get_work_service(self->context);

	if (NULL != work)
		venture_work_service_session_close(work, session_id);

	path = g_strdup_printf("/harness/%" G_GINT64_FORMAT, session_id);

	return venture_web_redirect_to(path);
}

/*
 * GET /harness/:id/stream - what the agent is saying, as it says it.
 */
static HtmxResponse *
venture_web_ui_harness_stream(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) session = NULL;
	g_autoptr(GError) error = NULL;
	VentureWebSessionWatch *watch;
	VentureWorkService *work;
	SoupServerMessage *message;
	gint64 session_id;

	self = user_data;
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

	principal = venture_auth_authenticate(self->auth, request);
	session_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	session = venture_web_session_get(self, principal, session_id, &error);

	if (NULL == session)
		return venture_web_error_response(error);

	work = venture_context_get_work_service(self->context);
	message = htmx_request_get_message(request);

	if ((NULL == work) || (NULL == message))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
		                    "This request cannot be streamed");
		return venture_web_error_response(error);
	}

	watch = g_new0(VentureWebSessionWatch, 1);
	watch->self = self;
	watch->session_id = session_id;
	watch->open = TRUE;
	watch->connection = htmx_sse_connection_new(message);

	watch->output_handler = g_signal_connect(work, "session-output",
		G_CALLBACK(venture_web_session_on_output), watch);
	watch->finished_handler = g_signal_connect(work, "session-finished",
		G_CALLBACK(venture_web_session_on_finished), watch);

	/* The watch outlives this handler and goes when the socket does. */
	g_signal_connect_data(watch->connection, "closed",
		G_CALLBACK(venture_web_session_watch_closed), watch,
		venture_web_session_watch_destroy, 0);

	return htmx_response_new_streaming();
}

/* The completion kind, as the client names it. */
static const gchar *
venture_web_harness_kind_name(VentureHarnessCompletion kind)
{
	switch (kind)
	{
	case VENTURE_HARNESS_COMPLETION_COMMAND:
		return "command";

	case VENTURE_HARNESS_COMPLETION_RECORD:
		return "record";

	case VENTURE_HARNESS_COMPLETION_BASE:
		return "base";

	default:
		return "none";
	}
}

/*
 * Whether the person asking may see a record type.
 *
 * The harness asks through this rather than deciding for itself: which
 * role a type needs is this file's policy, and a second copy of it in the
 * AI layer is a second place for it to go stale.
 */
typedef struct
{
	VentureWebServer	*self;
	VentureAuthPrincipal	*principal;
} VentureWebHarnessAllow;

static gboolean
venture_web_harness_allows(
	GType		 entity_type,
	gpointer	 user_data
){
	VentureWebHarnessAllow *allow = user_data;

	return venture_web_require_for_type(allow->self, allow->principal,
	                                    entity_type, VENTURE_USER_ROLE_VIEWER,
	                                    NULL);
}

/*
 * GET /ui/chat/complete - what the composer's menus offer.
 *
 * The skills, built in and stored, for the / menu; the knowledge bases
 * for the # menu, only when the kb module is on and this person may read
 * them. The client fetches it once and keeps it for a minute. The
 * commands the composer handles itself are not here: the script is the
 * authority on what it can do, and a menu offering a command the script
 * did not understand would be a menu that lies.
 */
static HtmxResponse *
venture_web_ui_chat_complete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureWebHarnessAllow allow;
	VentureAiHarness *harness;
	const gchar *buffer;
	const gchar *cursor_text;
	guint cursor;
	guint start;
	guint end;
	guint i;

	self = user_data;
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "chat");

		if (NULL != gate)
			return gate;
	}

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	buffer = htmx_request_get_query_param(request, "buffer");
	cursor_text = htmx_request_get_query_param(request, "cursor");

	if (NULL == buffer)
		buffer = "";

	cursor = (NULL != cursor_text)
		? (guint)g_ascii_strtoull(cursor_text, NULL, 10)
		: (guint)strlen(buffer);

	harness = venture_context_get_ai_harness(self->context);

	/*
	 * Rescanned on every open. A command file written a minute ago
	 * belongs in the menu, and the alternative is restarting a server to
	 * pick up a markdown file.
	 */
	venture_ai_harness_refresh(harness);

	allow.self = self;
	allow.principal = principal;
	start = 0;
	end = 0;
	items = venture_ai_harness_complete(harness, buffer, cursor,
	                                    venture_web_harness_allows, &allow,
	                                    &start, &end);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "kind");
	json_builder_add_string_value(builder,
		venture_web_harness_kind_name(
			venture_ai_harness_completion_kind(buffer, cursor)));

	json_builder_set_member_name(builder, "start");
	json_builder_add_int_value(builder, start);
	json_builder_set_member_name(builder, "end");
	json_builder_add_int_value(builder, end);

	json_builder_set_member_name(builder, "items");
	json_builder_begin_array(builder);

	for (i = 0; (NULL != items) && (i < items->len); i++)
	{
		VentureHarnessItem *item;

		item = g_ptr_array_index(items, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "insert");
		json_builder_add_string_value(builder, item->insert);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, item->label);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, item->name);
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder, item->description);
		json_builder_set_member_name(builder, "origin");
		json_builder_add_string_value(builder, item->origin);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * POST /ui/chat/thread/:id/rename - give a conversation a name.
 *
 * The first question makes a serviceable title until the conversation
 * turns into something else, which most useful ones do. Scoped the same
 * way reading it is: another person's thread is NOT_FOUND. Answers JSON,
 * because the list row and the panel heading are both updated in place by
 * the script rather than re-rendered.
 */
static HtmxResponse *
venture_web_ui_chat_thread_rename(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureChatThread) thread = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *title = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	const gchar *wanted;
	gint64 thread_id;

	self = user_data;
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "chat");

		if (NULL != gate)
			return gate;
	}

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	thread_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	thread = venture_web_chat_get_thread(self, principal, thread_id, &error);

	if (NULL == thread)
		return venture_web_error_response(error);

	wanted = htmx_request_get_form_value(request, "title");

	if (venture_string_is_empty(wanted))
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A conversation needs a title");
		return venture_web_error_response(error);
	}

	title = venture_truncate(g_strstrip(g_strdup(wanted)), 120);
	venture_auth_to_actor(principal, &actor);
	g_object_set(thread, "title", title, NULL);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(thread), &actor, &error))
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, thread_id);
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, title);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * GET /ui/chat/thread/:id/export - the transcript as an org-mode file.
 *
 * A conversation with the thing that has write access to the books is
 * worth keeping somewhere other than its own table: in the notes, in a
 * ticket, in an email to whoever asked. Org because that is where notes
 * go here; plain text so anything can read it. The bodies are verbatim --
 * this is the stored record, not the rendered one.
 */
static HtmxResponse *
venture_web_ui_chat_thread_export(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureChatThread) thread = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(GString) out = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *disposition = NULL;
	g_autoptr(GDateTime) created = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *response;
	gint64 thread_id;
	guint i;

	self = user_data;
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "chat");

		if (NULL != gate)
			return gate;
	}

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

	g_object_get(thread, "title", &title, "created-at", &created, NULL);

	out = g_string_new("#+title: ");
	g_string_append(out, venture_string_is_empty(title) ? "Untitled" : title);
	g_string_append(out, "\n#+description: A conversation with VENTURE\n");

	if (NULL != created)
	{
		g_autofree gchar *stamp = NULL;

		stamp = g_date_time_format_iso8601(created);
		g_string_append_printf(out, "#+created: %s\n", stamp);
	}

	g_string_append_printf(out, "#+conversation: %" G_GINT64_FORMAT "\n\n",
	                       thread_id);

	for (i = 0; i < messages->len; i++)
	{
		VentureEntity *message;
		g_autofree gchar *body = NULL;
		VentureChatRole role;

		message = g_ptr_array_index(messages, i);
		g_object_get(message, "role", &role, "body", &body, NULL);

		g_string_append(out, (VENTURE_CHAT_ROLE_ASSISTANT == role)
		                     ? "* VENTURE\n\n" : "* You\n\n");

		/* A line of the body that would read as an org heading gets a
		 * leading comma, the way org itself escapes them in blocks. */
		if (NULL != body)
		{
			g_auto(GStrv) lines = NULL;
			gsize l;

			lines = g_strsplit(body, "\n", -1);

			for (l = 0; NULL != lines[l]; l++)
			{
				if ('*' == lines[l][0])
					g_string_append_c(out, ',');

				g_string_append(out, lines[l]);
				g_string_append_c(out, '\n');
			}
		}

		g_string_append_c(out, '\n');
	}

	response = htmx_response_new_with_content(out->str);
	htmx_response_set_content_type(response, "text/x-org; charset=utf-8");
	htmx_response_set_status(response, 200);
	disposition = g_strdup_printf(
		"attachment; filename=\"venture-conversation-%" G_GINT64_FORMAT
		".org\"", thread_id);
	htmx_response_add_header(response, "Content-Disposition", disposition);

	return response;
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
 * How much of a conversation is replayed to the model on each turn: the
 * newest messages up to this many characters, and never more than this
 * many messages. The stored transcript keeps everything; this is what
 * the model is shown, which is the part that costs and can overflow.
 */
#define VENTURE_WEB_CHAT_HISTORY_CHARS (40000)
#define VENTURE_WEB_CHAT_HISTORY_MESSAGES (60)

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
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "chat");

		if (NULL != gate)
			return gate;
	}

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
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "chat");

		if (NULL != gate)
			return gate;
	}

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
		? venture_confirmation_store_approve_as(store, id,
			principal->name, principal->role, &error)
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

/* --- Streaming one turn ---------------------------------------------------- */

/*
 * Parks a worked-out turn and returns the one-shot token that runs it.
 * Expired turns are dropped on the way past, which is enough housekeeping
 * for a table that holds the questions asked in the last two minutes.
 *
 * Returns: (transfer full): the token
 */
static gchar *
venture_web_chat_turn_park(
	VentureWebServer	*self,
	VentureWebChatTurn	*turn
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gchar *token;

	turn->created_at = venture_time_now();

	g_hash_table_iter_init(&iter, self->chat_turns);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		VentureWebChatTurn *stale = value;

		if (g_date_time_difference(turn->created_at, stale->created_at) >
		    (VENTURE_WEB_CHAT_TURN_TTL_SECONDS * G_TIME_SPAN_SECOND))
			g_hash_table_iter_remove(&iter);
	}

	token = venture_generate_token(16);
	g_hash_table_insert(self->chat_turns, g_strdup(token), turn);

	return token;
}

/*
 * Spends a token. %NULL for one that was already spent, never existed,
 * expired, or belongs to somebody else -- all four are the same answer,
 * which is that there is nothing to stream.
 *
 * Returns: (transfer full) (nullable): the turn
 */
static VentureWebChatTurn *
venture_web_chat_turn_take(
	VentureWebServer	*self,
	const gchar		*token,
	gint64			 user_id
){
	g_autoptr(GDateTime) now = NULL;
	VentureWebChatTurn *turn;
	gpointer key;
	gpointer value;

	if (venture_string_is_empty(token))
		return NULL;

	if (!g_hash_table_steal_extended(self->chat_turns, token, &key, &value))
		return NULL;

	g_free(key);
	turn = value;
	now = venture_time_now();

	if ((turn->user_id != user_id) ||
	    (g_date_time_difference(now, turn->created_at) >
	     (VENTURE_WEB_CHAT_TURN_TTL_SECONDS * G_TIME_SPAN_SECOND)))
	{
		venture_web_chat_turn_free(turn);
		return NULL;
	}

	return turn;
}

/*
 * One turn being streamed: the connection it writes to, the turn it is
 * answering, and a copy of who asked.
 *
 * The principal is copied rather than borrowed. The request that opened
 * the stream returns long before the model does, taking its principal
 * with it, and the AI service holds on to the one it was given for the
 * whole turn so that a staged write is attributed to a person.
 */
typedef struct
{
	VentureWebServer	*self;
	HtmxSseConnection	*connection;
	VentureWebChatTurn	*turn;
	VentureAuthPrincipal	*principal;
	gboolean		 open;
} VentureWebChatStream;

static void
venture_web_chat_stream_free(VentureWebChatStream *self)
{
	if (NULL == self)
		return;

	g_clear_object(&self->connection);
	g_clear_pointer(&self->turn, venture_web_chat_turn_free);
	g_clear_pointer(&self->principal, venture_auth_principal_free);
	g_free(self);
}

static void
venture_web_chat_stream_closed(
	HtmxSseConnection	*connection,
	gpointer		 user_data
){
	VentureWebChatStream *stream = user_data;

	(void)connection;

	/*
	 * The tab was closed, or the network went. The turn is not
	 * cancelled: the model is already working and the answer is worth
	 * storing whether or not anybody is still watching -- resuming the
	 * conversation later is how it is read. Only the writing stops.
	 */
	stream->open = FALSE;
}

/*
 * Sends one event, with the line endings the framing can carry.
 *
 * A bare carriage return ends a field in the SSE grammar, so a reply
 * written on a machine that uses CRLF would have arrived split across
 * events with the halves reassembled in the wrong order.
 */
static void
venture_web_chat_stream_send(
	VentureWebChatStream	*stream,
	const gchar		*event,
	const gchar		*data
){
	g_autofree gchar *normalised = NULL;

	if (!stream->open || venture_string_is_empty(data))
		return;

	if (!htmx_sse_connection_is_connected(stream->connection))
	{
		stream->open = FALSE;
		return;
	}

	if (NULL == strchr(data, '\r'))
	{
		htmx_sse_connection_send_event(stream->connection, event, data, NULL);
		return;
	}

	{
		g_auto(GStrv) parts = NULL;

		parts = g_strsplit(data, "\r\n", -1);
		normalised = g_strjoinv("\n", parts);
		g_strdelimit(normalised, "\r", '\n');
	}

	htmx_sse_connection_send_event(stream->connection, event, normalised, NULL);
}

/*
 * Each thing the turn does, on its way past.
 *
 * Prose goes out as it arrives. A tool call becomes a line of status
 * instead: what the model is doing while it is not talking is the part a
 * person waiting actually wants, and it is the difference between a slow
 * answer and an interface that looks stuck.
 */
static void
venture_web_chat_stream_event(
	VentureAiService	*service,
	AiEvent			*event,
	gpointer		 user_data
){
	VentureWebChatStream *stream = user_data;

	(void)service;

	switch (ai_event_get_kind(event))
	{
	case AI_EVENT_TEXT_DELTA:
		venture_web_chat_stream_send(stream, "delta",
		                             ai_event_get_text(event));
		break;

	case AI_EVENT_TOOL_STARTED:
	{
		AiToolUse *use;
		const gchar *name;

		use = ai_event_get_tool_use(event);
		name = (NULL != use) ? ai_tool_use_get_name(use) : NULL;

		if (!venture_string_is_empty(name))
		{
			g_autofree gchar *status = NULL;

			status = g_strconcat(name, "\xe2\x80\xa6", NULL);
			venture_web_chat_stream_send(stream, "status", status);
		}

		break;
	}

	case AI_EVENT_TOOL_FINISHED:
		/* A space rather than nothing: an empty event body is not
		 * sent at all, so the line would stay on the last tool. */
		venture_web_chat_stream_send(stream, "status", " ");
		break;

	default:
		break;
	}
}

/*
 * The turn is answered. What the model finally said is stored and
 * rendered the same way the blocking path renders it, and that markup --
 * not the deltas -- is what the panel ends up showing: a turn that called
 * a tool halfway through said things before it that are not part of the
 * answer, and markdown cannot be formatted a fragment at a time anyway.
 */
static void
venture_web_chat_stream_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	VentureWebChatStream *stream = user_data;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *answer = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;

	answer = venture_ai_service_answer_stream_finish(
		VENTURE_AI_SERVICE(source), result, &error);

	html = g_string_new(NULL);

	if (NULL == answer)
	{
		venture_web_chat_append_failure(html, error->message);
		venture_web_chat_stream_send(stream, "done", html->str);
		htmx_sse_connection_close(stream->connection);
		venture_web_chat_stream_free(stream);
		return;
	}

	venture_auth_to_actor(stream->principal, &actor);

	{
		g_autoptr(VentureChatMessage) stored = NULL;

		stored = venture_chat_message_new();
		g_object_set(stored, "thread-id", stream->turn->thread_id,
		             "role", VENTURE_CHAT_ROLE_ASSISTANT,
		             "body", answer, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(stored),
			venture_context_get_default_organization_id(
				stream->self->context));

		if (!venture_database_save(
			venture_context_get_database(stream->self->context),
			VENTURE_ENTITY(stored), &actor, &error))
		{
			/*
			 * The answer exists and the operator is owed it, but
			 * the thread will not have it tomorrow. Said plainly
			 * rather than swallowed: a conversation that quietly
			 * loses half of itself is worse than one that says so.
			 */
			venture_web_chat_append_message(html,
				VENTURE_CHAT_ROLE_ASSISTANT, answer);
			venture_web_chat_append_failure(html, error->message);
			venture_web_chat_stream_send(stream, "done", html->str);
			htmx_sse_connection_close(stream->connection);
			venture_web_chat_stream_free(stream);
			return;
		}
	}

	venture_web_chat_append_message(html, VENTURE_CHAT_ROLE_ASSISTANT, answer);
	venture_web_chat_append_confirmations(stream->self, html,
	                                      stream->turn->staged_before);

	venture_web_chat_stream_send(stream, "done", html->str);
	htmx_sse_connection_close(stream->connection);
	venture_web_chat_stream_free(stream);
}

/*
 * GET /ui/chat/stream/:token - answer a parked turn, out loud.
 *
 * The token is spent here, so a reconnecting browser finds nothing rather
 * than asking the same question twice, and a token minted for somebody
 * else is indistinguishable from one that never existed.
 */
static HtmxResponse *
venture_web_ui_chat_stream(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	VentureWebChatTurn *turn;
	VentureWebChatStream *stream;
	VentureAiService *service;
	SoupServerMessage *message;

	self = user_data;
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "chat");

		if (NULL != gate)
			return gate;
	}

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	turn = venture_web_chat_turn_take(self,
		g_hash_table_lookup(params, "token"), principal->user_id);

	if (NULL == turn)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "That question is no longer waiting to be "
		                    "answered");
		return venture_web_error_response(error);
	}

	message = htmx_request_get_message(request);

	if (NULL == message)
	{
		venture_web_chat_turn_free(turn);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
		                    "This request cannot be streamed");
		return venture_web_error_response(error);
	}

	stream = g_new0(VentureWebChatStream, 1);
	stream->self = self;
	stream->turn = turn;
	stream->open = TRUE;
	stream->connection = htmx_sse_connection_new(message);

	/* Our own copy, because the request's goes when this handler
	 * returns and the model has not started yet. */
	stream->principal = g_new0(VentureAuthPrincipal, 1);
	stream->principal->user_id = principal->user_id;
	stream->principal->token_id = principal->token_id;
	stream->principal->role = principal->role;
	stream->principal->authenticated = principal->authenticated;
	stream->principal->name = g_strdup(principal->name);

	g_signal_connect(stream->connection, "closed",
	                 G_CALLBACK(venture_web_chat_stream_closed), stream);

	service = venture_context_get_ai_service(self->context);

	/*
	 * Reported down the stream rather than as a status: the connection
	 * is open by now, and an EventSource that fails to connect tells the
	 * browser nothing more useful than "it did not work".
	 */
	if (NULL == service)
	{
		g_autoptr(GString) html = NULL;

		html = g_string_new(NULL);
		venture_web_chat_append_failure(html,
			"AI is not configured on this instance.");
		venture_web_chat_stream_send(stream, "done", html->str);
		htmx_sse_connection_close(stream->connection);
		venture_web_chat_stream_free(stream);

		return htmx_response_new_streaming();
	}

	venture_ai_service_answer_stream_async(service, turn->history,
		turn->model_text, turn->images,
		(const gchar *const *)turn->image_types->pdata,
		stream->principal, venture_web_chat_stream_event, stream, NULL,
		venture_web_chat_stream_done, stream);

	return htmx_response_new_streaming();
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
	g_autoptr(GPtrArray) all_history = NULL;
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
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "chat");

		if (NULL != gate)
			return gate;
	}

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

	/*
	 * A slash and a known command is a command: the model gets the
	 * prompt it stands for, the transcript keeps what was typed. An
	 * unknown one is just a question that starts with a slash.
	 */
	{
		g_autofree gchar *expanded = NULL;

		expanded = venture_ai_harness_expand(
			venture_context_get_ai_harness(self->context), message);

		if (NULL != expanded)
			g_string_assign(model_text, expanded);
	}

	/*
	 * And an @record names something to read. The records ride ahead of
	 * the question on this turn only; the transcript keeps the @ token,
	 * which is shorter than the record and still says what was meant.
	 */
	{
		g_autofree gchar *mentioned = NULL;
		VentureWebHarnessAllow allow;

		allow.self = self;
		allow.principal = principal;
		mentioned = venture_ai_harness_expand_mentions(
			venture_context_get_ai_harness(self->context), message,
			venture_web_harness_allows, &allow);

		if (!venture_string_is_empty(mentioned))
		{
			g_string_prepend(model_text, "\n\n");
			g_string_prepend(model_text, mentioned);
			g_string_prepend(model_text, "[Named] ");
		}
	}
	image_types = g_ptr_array_new_with_free_func(g_free);

	if (!venture_web_chat_attach(self,
		htmx_request_get_form_value(request, "attachments"),
		model_text, stored_text, images, image_types, &error))
		return venture_web_error_response(error);

	/* answer_with_images wants a NULL-terminated array of types. */
	g_ptr_array_add(image_types, NULL);

	/*
	 * Where the question was asked from. The model is told what is on
	 * the screen -- the record, or the page -- ahead of the question,
	 * on this turn only; the transcript keeps one line naming a record
	 * page, so "summarise this" still means something when the thread
	 * is read back next month.
	 */
	{
		const gchar *context_path;
		g_autofree gchar *described = NULL;

		context_path = htmx_request_get_form_value(request, "context");
		described = venture_web_chat_describe_context(self, principal,
		                                              context_path);

		if (!venture_string_is_empty(described))
		{
			g_string_prepend(model_text, "\n\n");
			g_string_prepend(model_text, described);
			g_string_prepend(model_text, "[Context] ");

			if (venture_web_chat_path_is_record(context_path))
			{
				g_string_append(stored_text, "\n[While viewing ");
				g_string_append(stored_text, context_path);
				g_string_append(stored_text, "]");
			}
		}
	}

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
	all_history = venture_web_chat_get_messages(self, thread_id, &error);

	if (NULL == all_history)
		return venture_web_error_response(error);

	/*
	 * A long conversation is replayed from its tail. Every provider has
	 * a context window and every token of history is paid for on every
	 * turn; a thread that has been going for a month would otherwise
	 * grow until the provider refused it, and the failure would land on
	 * whichever question happened to be one too many. The cut is by
	 * size and by count, oldest first, and always at a message boundary.
	 */
	{
		gsize budget;
		guint kept;
		guint i;

		/* A view: the records stay owned by the full list. */
		history = g_ptr_array_new();
		budget = VENTURE_WEB_CHAT_HISTORY_CHARS;
		kept = 0;

		for (i = all_history->len; i > 0; i--)
		{
			VentureEntity *earlier;
			g_autofree gchar *body = NULL;
			gsize length;

			earlier = g_ptr_array_index(all_history, i - 1);
			g_object_get(earlier, "body", &body, NULL);
			length = (NULL != body) ? strlen(body) : 0;

			if ((kept >= VENTURE_WEB_CHAT_HISTORY_MESSAGES) ||
			    ((kept > 0) && (length > budget)))
				break;

			budget -= MIN(length, budget);
			kept++;
		}

		for (i = all_history->len - kept; i < all_history->len; i++)
			g_ptr_array_add(history, g_ptr_array_index(all_history, i));
	}

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

	/*
	 * A browser that can hold a stream open gets one: the turn is parked
	 * here and answered by the GET that follows, so the model's prose
	 * arrives as it is written rather than in one lump at the end.
	 *
	 * Only when the script asks for it. A form posted without scripting
	 * has nowhere to put an EventSource, and the blocking path below is
	 * what answers it -- the same path the CLI, the API and every test
	 * take.
	 */
	if (0 == g_strcmp0(htmx_request_get_form_value(request, "stream"), "1"))
	{
		VentureWebChatTurn *turn;
		g_autofree gchar *token = NULL;

		turn = g_new0(VentureWebChatTurn, 1);
		turn->thread_id = thread_id;
		turn->user_id = principal->user_id;
		turn->model_text = g_strdup(model_text->str);
		turn->staged_before = g_hash_table_ref(staged_before);
		turn->images = g_ptr_array_ref(images);
		turn->image_types = g_ptr_array_ref(image_types);

		/* Refs, because the arrays this was read into go with this
		 * request and the model is shown them in the next one. */
		turn->history = g_ptr_array_new_with_free_func(g_object_unref);

		{
			guint h;

			for (h = 0; h < history->len; h++)
				g_ptr_array_add(turn->history,
					g_object_ref(g_ptr_array_index(history, h)));
		}

		token = venture_web_chat_turn_park(self, turn);

		/*
		 * An empty bubble that says where its words will come from.
		 * The script opens the stream; the status line and the
		 * paragraph are what it writes into.
		 */
		g_string_append(html, "<div class=\"msg ai streaming\" "
		                      "data-chat-stream=\"/ui/chat/stream/");
		venture_html_escape_append(html, token);
		g_string_append(html, "\"><span class=\"msg-avatar\">"
		                      VENTURE_SPARK
		                      "</span><div class=\"msg-content\">"
		                      "<div class=\"chat-status\" data-chat-status>"
		                      "</div>"
		                      "<p class=\"chat-stream-text\" data-chat-text>"
		                      "</p></div></div>");

		venture_web_chat_append_thread_input(html, thread_id);

		if (fresh_thread)
		{
			g_autofree gchar *title = NULL;

			g_object_get(thread, "title", &title, NULL);
			venture_web_chat_append_title(html, title);
		}

		g_object_set(thread, "last-activity-at", now, NULL);
		venture_database_save(venture_context_get_database(self->context),
		                      VENTURE_ENTITY(thread), &actor, NULL);

		return venture_web_html_response(
			g_string_free(g_steal_pointer(&html), FALSE), 200);
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
			venture_web_chat_append_failure(html, error->message);
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


/* --- Knowledge bases ------------------------------------------------------ */

/*
 * The shared knowledge-base service, or a refusal explaining its absence.
 *
 * Absence is a configuration state rather than an error in the request, so
 * the message says which -- an operator whose search returns 503 wants to
 * know whether to fix the config or the ollama container.
 */
static VentureKbService *
venture_web_kb_service(
	VentureWebServer	 *self,
	HtmxResponse		**denied
){
	VentureKbService *kb;

	*denied = NULL;
	kb = venture_context_get_kb_service(self->context);

	if (NULL == kb)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(JsonBuilder) builder = NULL;
		g_autoptr(JsonNode) node = NULL;

		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Knowledge bases are not available on this "
		                    "instance. Either kb.enabled is false, or the "
		                    "embedding service named by kb.embedding_url "
		                    "could not be reached at startup.");

		/*
		 * 503, built here rather than through venture_web_error_response(),
		 * which maps VENTURE_ERROR_CONFIG to 500 along with every other
		 * internal fault. This is not the server breaking: it is a
		 * capability this install does not currently offer, which is what
		 * 503 means and what tells the operator to look at the config or
		 * the embedding container rather than at a stack trace.
		 */
		builder = json_builder_new();
		json_builder_begin_object(builder);
		venture_json_builder_add_error(builder, error);
		json_builder_end_object(builder);
		node = json_builder_get_root(builder);

		*denied = venture_web_json_response(node, 503);
	}

	return kb;
}

static HtmxResponse *
venture_web_api_kb_search(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	VentureKbService *kb;
	g_autoptr(GPtrArray) hits = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gint64 *ids = NULL;
	HtmxResponse *denied;
	const gchar *query;
	const gchar *base;
	const gchar *limit_text;
	gsize n_ids = 0;
	guint limit = 0;
	guint i;

	self = user_data;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "kb");

		if (NULL != gate)
			return gate;
	}

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	kb = venture_web_kb_service(self, &denied);

	if (NULL == kb)
		return denied;

	query = htmx_request_get_query_param(request, "q");

	if (venture_string_is_empty(query))
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A \"q\" to search for is required");
		return venture_web_error_response(error);
	}

	base = htmx_request_get_query_param(request, "kb");
	limit_text = htmx_request_get_query_param(request, "limit");

	if (!venture_string_is_empty(limit_text))
		limit = (guint)g_ascii_strtoull(limit_text, NULL, 10);

	if (!venture_string_is_empty(base))
	{
		g_auto(GStrv) slugs = NULL;

		/* Comma separated, so one request can scope to several bases
		 * the way the chat's #tokens do. */
		slugs = g_strsplit(base, ",", -1);
		ids = venture_kb_service_resolve_slugs(kb,
			(const gchar *const *)slugs, &n_ids, &error);

		if (NULL == ids)
			return venture_web_error_response(error);
	}

	hits = venture_kb_service_search(kb, query, ids, n_ids, limit, &error);

	if (NULL == hits)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "query");
	json_builder_add_string_value(builder, query);
	json_builder_set_member_name(builder, "hits");
	json_builder_begin_array(builder);

	for (i = 0; i < hits->len; i++)
	{
		const VentureKbHit *hit = g_ptr_array_index(hits, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "knowledge_base");
		json_builder_add_string_value(builder, hit->kb_slug);
		json_builder_set_member_name(builder, "kb_id");
		json_builder_add_int_value(builder, hit->kb_id);
		json_builder_set_member_name(builder, "article_id");
		json_builder_add_int_value(builder, hit->article_id);
		json_builder_set_member_name(builder, "title");
		json_builder_add_string_value(builder, hit->title);
		json_builder_set_member_name(builder, "heading");
		json_builder_add_string_value(builder, hit->heading);
		json_builder_set_member_name(builder, "ordinal");
		json_builder_add_int_value(builder, hit->ordinal);
		json_builder_set_member_name(builder, "score");
		json_builder_add_double_value(builder, hit->score);
		json_builder_set_member_name(builder, "text");
		json_builder_add_string_value(builder, hit->text);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * Renders what an import or sync did.
 */
static HtmxResponse *
venture_web_kb_result_response(VentureKbIngestResult *result)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	guint i;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "created");
	json_builder_add_int_value(builder, result->created);
	json_builder_set_member_name(builder, "updated");
	json_builder_add_int_value(builder, result->updated);
	json_builder_set_member_name(builder, "unchanged");
	json_builder_add_int_value(builder, result->unchanged);
	json_builder_set_member_name(builder, "skipped");
	json_builder_add_int_value(builder, result->skipped);
	json_builder_set_member_name(builder, "failed");
	json_builder_add_int_value(builder, result->failed);
	json_builder_set_member_name(builder, "indexed");
	json_builder_add_int_value(builder, result->indexed);
	json_builder_set_member_name(builder, "notes");
	json_builder_begin_array(builder);

	for (i = 0; i < result->notes->len; i++)
		json_builder_add_string_value(builder,
			g_ptr_array_index(result->notes, i));

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_kb_sync(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	VentureKbService *kb;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureKbIngestResult) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *denied;
	const gchar *id_text;
	gint64 kb_id;

	self = user_data;

	/*
	 * Editor, not viewer. A sync reads a directory on the server and
	 * writes records from it, which is a change to what the assistant
	 * will say.
	 */
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "kb");

		if (NULL != gate)
			return gate;
	}

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != denied)
		return denied;

	kb = venture_web_kb_service(self, &denied);

	if (NULL == kb)
		return denied;

	id_text = g_hash_table_lookup(params, "id");
	kb_id = (NULL != id_text) ? g_ascii_strtoll(id_text, NULL, 10) : 0;

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	result = venture_kb_sync_directory(kb, kb_id, &actor, &error);

	if (NULL == result)
		return venture_web_error_response(error);

	return venture_web_kb_result_response(result);
}

static HtmxResponse *
venture_web_api_kb_reindex(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	VentureKbService *kb;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	VentureActor actor;
	HtmxResponse *denied;
	const gchar *id_text;
	const gchar *force_text;
	gint64 kb_id;
	gint indexed;

	self = user_data;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "kb");

		if (NULL != gate)
			return gate;
	}

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != denied)
		return denied;

	kb = venture_web_kb_service(self, &denied);

	if (NULL == kb)
		return denied;

	id_text = g_hash_table_lookup(params, "id");
	kb_id = (NULL != id_text) ? g_ascii_strtoll(id_text, NULL, 10) : 0;
	force_text = htmx_request_get_query_param(request, "force");

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	indexed = venture_kb_service_reindex(kb, kb_id,
		(NULL != force_text) && (0 == g_strcmp0(force_text, "true")),
		&actor, &error);

	if (indexed < 0)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "indexed");
	json_builder_add_int_value(builder, indexed);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_kb_export(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	VentureKbService *kb;
	g_autoptr(GBytes) archive = NULL;
	g_autoptr(VentureEntity) base = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *disposition = NULL;
	HtmxResponse *response;
	HtmxResponse *denied;
	const gchar *id_text;
	const gchar *format;
	gint64 kb_id;

	self = user_data;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "kb");

		if (NULL != gate)
			return gate;
	}

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	kb = venture_web_kb_service(self, &denied);

	if (NULL == kb)
		return denied;

	id_text = g_hash_table_lookup(params, "id");
	kb_id = (NULL != id_text) ? g_ascii_strtoll(id_text, NULL, 10) : 0;
	format = htmx_request_get_query_param(request, "format");

	if (venture_string_is_empty(format))
		format = "zip";

	if (!venture_kb_export(kb, kb_id, format, &archive, &error))
		return venture_web_error_response(error);

	base = venture_database_get(venture_context_get_database(self->context),
	                            VENTURE_TYPE_KNOWLEDGE_BASE, kb_id, NULL);

	if (NULL != base)
		g_object_get(base, "slug", &slug, NULL);

	if (venture_string_is_empty(slug))
	{
		g_free(slug);
		slug = g_strdup("knowledge-base");
	}

	response = htmx_response_new();
	htmx_response_set_status(response, 200);
	htmx_response_set_bytes(response, archive);
	htmx_response_set_content_type(response,
		(0 == g_strcmp0(format, "zip"))
			? "application/zip" : "application/gzip");

	/*
	 * Named, so a browser saves something recognisable rather than the
	 * route's last path segment.
	 */
	disposition = g_strdup_printf("attachment; filename=\"%s.%s\"", slug,
	                              (0 == g_strcmp0(format, "zip"))
	                                  ? "zip" : "tar.gz");
	htmx_response_add_header(response, "Content-Disposition", disposition);

	return response;
}

/*
 * Uploads one or more files into a knowledge base.
 *
 * Every file goes through the same ingest path as a sync, so an archive is
 * unpacked and its members follow the same rules recursively -- which is
 * what makes "drop a zip of the handbook in" work without a second code
 * path for it.
 *
 * Editor, like sync: this writes articles the assistant will quote.
 */
static HtmxResponse *
venture_web_api_kb_import(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	VentureKbService *kb;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureKbIngestResult) result = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(GError) error = NULL;
	VentureConfig *config;
	VentureActor actor;
	HtmxResponse *denied;
	const gchar *content_type;
	const gchar *id_text;
	gint64 kb_id;
	gint64 max_mb = 64;
	guint i;

	self = user_data;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "kb");

		if (NULL != gate)
			return gate;
	}

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != denied)
		return denied;

	content_type = htmx_request_get_content_type(request);

	/*
	 * The request is checked before the service is looked up. A malformed
	 * upload is malformed whether or not knowledge bases happen to be
	 * available, and answering "your body is url-encoded" is what the
	 * caller has to act on first -- otherwise fixing the encoding is
	 * rewarded with a second, different error and the impression that
	 * nothing works.
	 */

	/*
	 * Checked before parsing, so the answer names the cause. A client that
	 * posted the form url-encoded -- which is what happens when the
	 * encoding attribute is missing or unsupported -- otherwise gets a
	 * parser failure in someone else's error domain, and
	 * venture_web_error_response() maps an unfamiliar domain to 500. A
	 * 500 says the server broke; this is the request being wrong, and the
	 * difference is the whole debugging session.
	 */
	if ((NULL == content_type) ||
	    (NULL == strstr(content_type, "multipart/form-data")))
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "An import must be posted as multipart/form-data; this "
		            "request was %s. A file cannot survive url-encoding.",
		            venture_string_is_empty(content_type)
		                ? "sent without a content type" : content_type);
		return venture_web_error_response(error);
	}

	kb = venture_web_kb_service(self, &denied);

	if (NULL == kb)
		return denied;

	id_text = g_hash_table_lookup(params, "id");
	kb_id = (NULL != id_text) ? g_ascii_strtoll(id_text, NULL, 10) : 0;

	config = venture_context_get_config(self->context);
	g_object_get(config, "kb-max-upload-mb", &max_mb, NULL);

	files = htmx_uploaded_file_parse_multipart(
		content_type, htmx_request_get_body_bytes(request), NULL, &error);

	if ((NULL == files) || (0 == files->len))
	{
		/*
		 * Re-raised in this domain rather than passed through. The
		 * parser's own error is about multipart syntax and belongs in
		 * the message, but its domain would become a 500.
		 */
		g_autofree gchar *why = NULL;

		why = g_strdup((NULL != error) ? error->message
		                               : "no file part was found");
		g_clear_error(&error);
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "That upload carried no file: %s", why);

		return venture_web_error_response(error);
	}

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	result = venture_kb_ingest_result_new();

	for (i = 0; i < files->len; i++)
	{
		HtmxUploadedFile *file = g_ptr_array_index(files, i);

		/*
		 * The cap is per file rather than per request, and checked
		 * before the bytes reach the unpacker: an archive is expanded
		 * in memory, so the limit that matters is on what arrives.
		 */
		if ((max_mb > 0) &&
		    (htmx_uploaded_file_get_size(file) >
		     (gsize)(max_mb * 1024 * 1024)))
		{
			g_set_error(&error, VENTURE_ERROR,
			            VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s is over the %" G_GINT64_FORMAT " MB import "
			            "limit",
			            htmx_uploaded_file_get_filename(file), max_mb);
			return venture_web_error_response(error);
		}

		/*
		 * source_path is deliberately NULL for an upload. It is what
		 * sync reconciles against, and an uploaded file has no path on
		 * this server -- recording the browser's filename there would
		 * make the next sync archive the article for having "gone".
		 */
		if (!venture_kb_ingest_bytes(kb, kb_id,
		                             htmx_uploaded_file_get_data(file),
		                             htmx_uploaded_file_get_filename(file),
		                             NULL, &actor, result, &error))
			return venture_web_error_response(error);
	}

	return venture_web_kb_result_response(result);
}

/*
 * Cross-references one record against the corpus.
 *
 * Editor, like sync: it writes kb_link rows that then appear on the record's
 * page, so it changes what the install says about that record.
 */
static HtmxResponse *
venture_web_api_kb_crossref(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	VentureKbService *kb;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	VentureActor actor;
	HtmxResponse *denied;
	const gchar *type_name;
	const gchar *id_text;
	gint64 record_id;
	gint written;

	self = user_data;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "kb");

		if (NULL != gate)
			return gate;
	}

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != denied)
		return denied;

	kb = venture_web_kb_service(self, &denied);

	if (NULL == kb)
		return denied;

	type_name = g_hash_table_lookup(params, "type");
	id_text = g_hash_table_lookup(params, "id");
	record_id = (NULL != id_text) ? g_ascii_strtoll(id_text, NULL, 10) : 0;

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	written = venture_kb_crossref_record(kb, type_name, record_id, &actor,
	                                     &error);

	if (written < 0)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "links");
	json_builder_add_int_value(builder, written);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * Writes a knowledge-base article from a record.
 *
 * The body names the base; the route names the record. Built from the
 * record's own text rather than by asking a model to summarise it, because a
 * summary is a second version of the truth and this one is meant to be
 * citable.
 */
static HtmxResponse *
venture_web_api_kb_from_record(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	VentureKbService *kb;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	VentureActor actor;
	HtmxResponse *denied;
	const gchar *type_name;
	const gchar *id_text;
	gint64 record_id;
	gint64 kb_id = 0;
	gint64 article_id;

	self = user_data;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "kb");

		if (NULL != gate)
			return gate;
	}

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != denied)
		return denied;

	kb = venture_web_kb_service(self, &denied);

	if (NULL == kb)
		return denied;

	type_name = g_hash_table_lookup(params, "type");
	id_text = g_hash_table_lookup(params, "id");
	record_id = (NULL != id_text) ? g_ascii_strtoll(id_text, NULL, 10) : 0;

	body = htmx_request_get_json(request, NULL);

	if ((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
		kb_id = venture_json_object_get_int(json_node_get_object(body),
		                                    "kb_id", 0);

	if (0 == kb_id)
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Say which knowledge base to write into, as "
		                    "\"kb_id\"");
		return venture_web_error_response(error);
	}

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	article_id = venture_kb_article_from_record(kb, kb_id, type_name,
	                                            record_id, &actor, &error);

	if (article_id < 0)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "article_id");
	json_builder_add_int_value(builder, article_id);
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
	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "plugins");

		if (NULL != gate)
			return gate;
	}

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

/*
 * GET /api/v1/modules - every module, in dependency order, with its state.
 *
 * Viewer role: which modules an install runs is the shape of the product
 * a signed-in user is looking at, not activity, and a client -- venturectl,
 * an agent -- needs it to know which record types and pages to expect.
 */
static HtmxResponse *
venture_web_api_modules(
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

	node = venture_module_registry_describe(
		venture_context_get_modules(self->context));

	return venture_web_json_response(node, 200);
}

/*
 * Appends the names in @array as a comma-separated run of badges, or a
 * dash when there are none.
 */
static void
venture_web_append_module_names(
	GString		*content,
	JsonArray	*array
){
	guint n;
	guint i;

	n = (NULL != array) ? json_array_get_length(array) : 0;

	if (0 == n)
	{
		g_string_append(content, "<span class=\"muted\">\xe2\x80\x94</span>");
		return;
	}

	for (i = 0; i < n; i++)
	{
		if (i > 0)
			g_string_append(content, " ");

		g_string_append(content, "<span class=\"badge\">");
		venture_html_escape_append(content,
		                           json_array_get_string_element(array, i));
		g_string_append(content, "</span>");
	}
}

/*
 * GET /modules - the module table, for reading.
 *
 * Deliberately read-only. A module is switched in the configuration file
 * and takes effect at the next start, because turning one off means its
 * tables stop being migrated, its reports leave the registry and its
 * services are never built -- none of which can be undone in a running
 * process without a restart's worth of teardown. The page says how.
 */
static HtmxResponse *
venture_web_ui_modules(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(JsonNode) list = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	JsonArray *modules;
	guint enabled;
	guint i;

	self = user_data;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	list = venture_module_registry_describe(
		venture_context_get_modules(self->context));
	modules = json_node_get_array(list);
	enabled = 0;

	for (i = 0; i < json_array_get_length(modules); i++)
	{
		if (venture_json_object_get_bool(
			json_array_get_object_element(modules, i), "enabled", FALSE))
			enabled++;
	}

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Modules</h1><span class=\"subtitle\">");
	g_string_append_printf(content, "%u of %u enabled", enabled,
	                       json_array_get_length(modules));
	g_string_append(content, "</span></div></div>");

	g_string_append(content,
		"<div class=\"card\"><div class=\"card-body\">"
		"<p>A module is a group of record types, pages, reports and "
		"services that switch on and off together. Modules are switched "
		"in the configuration file and take effect at the next start:</p>"
		"<pre><code>modules:\n  crm:\n    enabled: false</code></pre>"
		"<p class=\"muted\">A module that requires one that is off is "
		"refused at startup rather than run half-working. Nothing is "
		"deleted by turning a module off; its tables and rows stay, and "
		"turning it back on shows them again. <code>venture "
		"--list-modules</code> prints this table for a configuration "
		"without starting the server.</p></div></div>");

	g_string_append(content,
		"<div class=\"card\"><div class=\"table-wrap\"><table class=\"data\">"
		"<thead><tr><th>Module</th><th>State</th><th>Requires</th>"
		"<th>Needed by</th><th>Record types</th><th>Reports</th></tr></thead>"
		"<tbody>");

	for (i = 0; i < json_array_get_length(modules); i++)
	{
		JsonObject *module;
		const gchar *reason;
		gboolean on;
		gboolean locked;

		module = json_array_get_object_element(modules, i);
		on = venture_json_object_get_bool(module, "enabled", FALSE);
		locked = venture_json_object_get_bool(module, "locked", FALSE);
		reason = venture_json_object_get_string(module, "disabled_reason",
		                                        NULL);

		g_string_append(content, "<tr><td><strong>");
		venture_html_escape_append(content,
			venture_json_object_get_string(module, "label", ""));
		g_string_append(content, "</strong><br><span class=\"muted\">");
		venture_html_escape_append(content,
			venture_json_object_get_string(module, "name", ""));

		if (0 == g_strcmp0(venture_json_object_get_string(module, "origin",
		                                                  "builtin"),
		                   "plugin"))
			g_string_append(content, " \xc2\xb7 plugin");

		g_string_append(content, "</span><br><small>");
		venture_html_escape_append(content,
			venture_json_object_get_string(module, "description", ""));
		g_string_append(content, "</small></td><td>");

		if (locked)
			g_string_append(content, "<span class=\"badge\">always on</span>");
		else if (on)
			g_string_append(content, "<span class=\"badge positive\">"
			                         "enabled</span>");
		else
		{
			g_string_append(content, "<span class=\"badge negative\">"
			                         "disabled</span>");

			if (!venture_string_is_empty(reason))
			{
				g_string_append(content, "<br><small class=\"muted\">");
				venture_html_escape_append(content, reason);
				g_string_append(content, "</small>");
			}
		}

		g_string_append(content, "</td><td>");
		venture_web_append_module_names(content,
			json_object_get_array_member(module, "requires"));
		g_string_append(content, "</td><td>");
		venture_web_append_module_names(content,
			json_object_get_array_member(module, "required_by"));
		g_string_append(content, "</td><td>");
		venture_web_append_module_names(content,
			json_object_get_array_member(module, "entity_types"));
		g_string_append(content, "</td><td>");
		venture_web_append_module_names(content,
			json_object_get_array_member(module, "reports"));
		g_string_append(content, "</td></tr>");
	}

	g_string_append(content, "</tbody></table></div></div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/modules", "Modules", content->str),
		200);
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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "plugins");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "automation");

		if (NULL != gate)
			return gate;
	}

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
		? venture_confirmation_store_approve_as(store, id,
			principal->name, principal->role, &error)
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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "forge");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "forge");

		if (NULL != gate)
			return gate;
	}

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

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_api(self, "forge");

		if (NULL != gate)
			return gate;
	}

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
venture_web_forge_webhook_workflow(
	VentureWebServer	*self,
	VentureForge		*forge,
	VentureForgeClient	*client,
	SoupMessageHeaders	*headers,
	JsonNode		*payload
);

static HtmxResponse *
venture_web_forge_webhook_release(
	VentureWebServer	*self,
	VentureForge		*forge,
	VentureForgeClient	*client,
	SoupMessageHeaders	*headers,
	JsonNode		*payload
);

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

	/* A forge that keeps getting 404s disables the hook, which is the
	 * right outcome for a module that is off. */
	if (!venture_web_module_enabled(self, "forge"))
		return venture_web_forge_ack(SOUP_STATUS_NOT_FOUND);

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
	 * The factory's two events, handed off whole: a CI run becomes a
	 * build, a release cut on the forge becomes a release record.
	 */
	if ((0 == g_strcmp0(event_name, "workflow_run")) ||
	    (0 == g_strcmp0(event_name, "release")))
	{
		parser = json_parser_new();

		if ((NULL == data) ||
		    !json_parser_load_from_data(parser, data, (gssize)length, NULL))
			return venture_web_forge_ack(SOUP_STATUS_BAD_REQUEST);

		if (0 == g_strcmp0(event_name, "workflow_run"))
			return venture_web_forge_webhook_workflow(self, forge, client,
				headers, json_parser_get_root(parser));

		return venture_web_forge_webhook_release(self, forge, client, headers,
			json_parser_get_root(parser));
	}

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

/* --- The software factory ------------------------------------------------- */

/*
 * The automation actor the factory writes as, so a build the CI reported
 * is distinguishable in the audit log from one a person typed.
 */
static void
venture_web_factory_actor(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_AUTOMATION;
	actor->name = "forge";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

/*
 * Parses a forge timestamp, tolerating an absent or unparseable one: a
 * build without a start time is still a build.
 *
 * Returns: (transfer full) (nullable): the instant
 */
static GDateTime *
venture_web_factory_parse_time(const gchar *text)
{
	if (venture_string_is_empty(text))
		return NULL;

	return venture_time_from_string(text, NULL);
}

/*
 * What a workflow run's status and conclusion mean as a build status.
 */
static VentureBuildStatus
venture_web_factory_build_status(
	const gchar	*action,
	const gchar	*status,
	const gchar	*conclusion
){
	if ((0 == g_strcmp0(status, "completed")) ||
	    (0 == g_strcmp0(action, "completed")))
	{
		if (0 == g_strcmp0(conclusion, "success"))
			return VENTURE_BUILD_STATUS_SUCCEEDED;

		if ((0 == g_strcmp0(conclusion, "cancelled")) ||
		    (0 == g_strcmp0(conclusion, "skipped")))
			return VENTURE_BUILD_STATUS_CANCELLED;

		return VENTURE_BUILD_STATUS_FAILED;
	}

	if ((0 == g_strcmp0(status, "in_progress")) ||
	    (0 == g_strcmp0(action, "in_progress")))
		return VENTURE_BUILD_STATUS_RUNNING;

	return VENTURE_BUILD_STATUS_QUEUED;
}

/*
 * Finds the build a workflow run already produced, matched on the
 * repository and the forge's own run id. A retried delivery and the
 * completed event after the requested one both land on the same row.
 *
 * Returns: (transfer full) (nullable): the build
 */
static VentureEntity *
venture_web_factory_find_build(
	VentureWebServer	*self,
	gint64			 repo_id,
	gint64			 run_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) builds = NULL;
	g_autofree gchar *external = NULL;

	external = g_strdup_printf("%" G_GINT64_FORMAT, run_id);
	query = venture_query_new(VENTURE_TYPE_BUILD);

	if (!venture_query_add_filter_int(query, "repo-id", VENTURE_FILTER_OP_EQ,
	                                  repo_id, NULL) ||
	    !venture_query_add_filter_string(query, "external-id",
	                                     VENTURE_FILTER_OP_EQ, external, NULL))
		return NULL;

	venture_query_set_limit(query, 1);
	builds = venture_database_find(venture_context_get_database(self->context),
	                               query, NULL);

	if ((NULL == builds) || (0 == builds->len))
		return NULL;

	return g_object_ref(g_ptr_array_index(builds, 0));
}

/*
 * A workflow_run delivery: one CI run becomes, or updates, one build.
 */
static HtmxResponse *
venture_web_forge_webhook_workflow(
	VentureWebServer	*self,
	VentureForge		*forge,
	VentureForgeClient	*client,
	SoupMessageHeaders	*headers,
	JsonNode		*payload
){
	g_autoptr(VentureForgeRepo) repo = NULL;
	g_autoptr(VentureEntity) build = NULL;
	g_autoptr(GDateTime) started = NULL;
	g_autoptr(GDateTime) finished = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *external = NULL;
	VentureForgeWorkflowEvent event;
	VentureActor actor;
	VentureBuildStatus status;
	gint64 repo_id;

	/* Builds are the factory's; without it the delivery is acknowledged
	 * and dropped, like any event nobody subscribed to. */
	if (!venture_web_module_enabled(self, "factory"))
		return venture_web_forge_ack(SOUP_STATUS_NO_CONTENT);

	if (!venture_forge_client_parse_workflow_event(client, payload, headers,
	                                               &event, &error))
	{
		venture_forge_workflow_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_BAD_REQUEST);
	}

	repo = venture_web_forge_repo_by_name(self,
		venture_entity_get_id(VENTURE_ENTITY(forge)), event.repo_full_name);

	if (NULL == repo)
	{
		venture_forge_workflow_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_NO_CONTENT);
	}

	repo_id = venture_entity_get_id(VENTURE_ENTITY(repo));
	build = venture_web_factory_find_build(self, repo_id, event.run_id);
	status = venture_web_factory_build_status(event.action, event.status,
	                                          event.conclusion);
	started = venture_web_factory_parse_time(event.started_at);
	finished = venture_web_factory_parse_time(event.finished_at);
	external = g_strdup_printf("%" G_GINT64_FORMAT, event.run_id);

	if (NULL == build)
	{
		build = VENTURE_ENTITY(venture_build_new());
		venture_entity_set_organization_id(build,
			venture_entity_get_organization_id(VENTURE_ENTITY(repo)));
		g_object_set(build,
		             "repo-id", repo_id,
		             "external-id", external,
		             "trigger", VENTURE_BUILD_TRIGGER_WEBHOOK,
		             NULL);
	}

	g_object_set(build,
	             "title", event.title,
	             "workflow", event.workflow_name,
	             "number", event.run_number,
	             "ref", event.head_branch,
	             "commit", event.head_sha,
	             "url", event.url,
	             "status", status,
	             NULL);

	if (NULL != started)
		g_object_set(build, "started-at", started, NULL);

	/* Finished only once it has: a queued run's updated_at is when it
	 * was queued, and a build that reads as finished before it started
	 * is a lie in the lead-time report. */
	if ((NULL != finished) &&
	    ((VENTURE_BUILD_STATUS_SUCCEEDED == status) ||
	     (VENTURE_BUILD_STATUS_FAILED == status) ||
	     (VENTURE_BUILD_STATUS_CANCELLED == status)))
		g_object_set(build, "finished-at", finished, NULL);

	venture_web_factory_actor(&actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           build, &actor, &error))
	{
		g_warning("venture_forge: cannot record build %s of %s: %s", external,
		          event.repo_full_name, error->message);
		venture_forge_workflow_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_INTERNAL_SERVER_ERROR);
	}

	venture_forge_workflow_event_clear(&event);

	return venture_web_forge_ack(SOUP_STATUS_ACCEPTED);
}

/*
 * Finds the release a forge release already produced: by the forge's id
 * first, then by tag, so a release VENTURE published (which knows the id)
 * and one cut on the forge (which VENTURE first hears of by tag) both
 * resolve to one row.
 *
 * Returns: (transfer full) (nullable): the release
 */
static VentureEntity *
venture_web_factory_find_release(
	VentureWebServer	*self,
	gint64			 repo_id,
	gint64			 external_id,
	const gchar		*tag
){
	gint pass;

	for (pass = 0; pass < 2; pass++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) releases = NULL;

		if ((0 == pass) && (0 == external_id))
			continue;

		if ((1 == pass) && venture_string_is_empty(tag))
			continue;

		query = venture_query_new(VENTURE_TYPE_RELEASE);

		if (!venture_query_add_filter_int(query, "repo-id",
		                                  VENTURE_FILTER_OP_EQ, repo_id, NULL))
			return NULL;

		if (0 == pass)
		{
			if (!venture_query_add_filter_int(query, "external-id",
			                                  VENTURE_FILTER_OP_EQ,
			                                  external_id, NULL))
				return NULL;
		}
		else
		{
			if (!venture_query_add_filter_string(query, "tag",
			                                     VENTURE_FILTER_OP_EQ, tag,
			                                     NULL))
				return NULL;
		}

		venture_query_set_limit(query, 1);
		releases = venture_database_find(
			venture_context_get_database(self->context), query, NULL);

		if ((NULL != releases) && (releases->len > 0))
			return g_object_ref(g_ptr_array_index(releases, 0));
	}

	return NULL;
}

/*
 * "v1.2.0" is version 1.2.0; "1.2.0" is too. Anything else is its own
 * version string.
 */
static gchar *
venture_web_factory_version_from_tag(const gchar *tag)
{
	if (venture_string_is_empty(tag))
		return g_strdup("");

	if ((('v' == tag[0]) || ('V' == tag[0])) && g_ascii_isdigit(tag[1]))
		return g_strdup(tag + 1);

	return g_strdup(tag);
}

/*
 * A release delivery: a release cut on the forge becomes, or updates, a
 * release record.
 */
static HtmxResponse *
venture_web_forge_webhook_release(
	VentureWebServer	*self,
	VentureForge		*forge,
	VentureForgeClient	*client,
	SoupMessageHeaders	*headers,
	JsonNode		*payload
){
	g_autoptr(VentureForgeRepo) repo = NULL;
	g_autoptr(VentureEntity) release = NULL;
	g_autoptr(GDateTime) published = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *existing_changelog = NULL;
	VentureForgeReleaseEvent event;
	VentureActor actor;
	gint64 repo_id;
	gint64 product_id = 0;

	if (!venture_web_module_enabled(self, "factory"))
		return venture_web_forge_ack(SOUP_STATUS_NO_CONTENT);

	if (!venture_forge_client_parse_release_event(client, payload, headers,
	                                              &event, &error))
	{
		venture_forge_release_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_BAD_REQUEST);
	}

	repo = venture_web_forge_repo_by_name(self,
		venture_entity_get_id(VENTURE_ENTITY(forge)), event.repo_full_name);

	if (NULL == repo)
	{
		venture_forge_release_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_NO_CONTENT);
	}

	repo_id = venture_entity_get_id(VENTURE_ENTITY(repo));
	g_object_get(repo, "product-id", &product_id, NULL);

	release = venture_web_factory_find_release(self, repo_id, event.release_id,
	                                           event.tag);

	if (NULL == release)
	{
		g_autofree gchar *version = NULL;

		/* A release deleted on the forge that VENTURE never knew is
		 * nothing to record. */
		if (0 == g_strcmp0(event.action, "deleted"))
		{
			venture_forge_release_event_clear(&event);
			return venture_web_forge_ack(SOUP_STATUS_NO_CONTENT);
		}

		version = venture_web_factory_version_from_tag(event.tag);

		release = VENTURE_ENTITY(venture_release_new());
		venture_entity_set_organization_id(release,
			venture_entity_get_organization_id(VENTURE_ENTITY(repo)));
		g_object_set(release,
		             "number", version,
		             "repo-id", repo_id,
		             "product-id", product_id,
		             NULL);
	}

	g_object_get(release, "changelog", &existing_changelog, NULL);

	g_object_set(release,
	             "tag", event.tag,
	             "name", event.name,
	             "url", event.url,
	             "external-id", event.release_id,
	             NULL);

	/* The forge's notes fill an empty changelog and never overwrite one
	 * somebody wrote here. */
	if (venture_string_is_empty(existing_changelog) &&
	    !venture_string_is_empty(event.body))
		g_object_set(release, "changelog", event.body, NULL);

	if (0 == g_strcmp0(event.action, "deleted"))
	{
		g_object_set(release, "status", VENTURE_RELEASE_STATUS_YANKED, NULL);
	}
	else if (event.draft)
	{
		g_object_set(release, "status", VENTURE_RELEASE_STATUS_IN_PROGRESS,
		             NULL);
	}
	else
	{
		published = venture_web_factory_parse_time(event.published_at);

		if (NULL == published)
			published = g_date_time_new_now_utc();

		g_object_set(release,
		             "status", VENTURE_RELEASE_STATUS_RELEASED,
		             "released-at", published,
		             NULL);
	}

	venture_web_factory_actor(&actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           release, &actor, &error))
	{
		g_warning("venture_forge: cannot record release %s of %s: %s",
		          event.tag, event.repo_full_name, error->message);
		venture_forge_release_event_clear(&event);
		return venture_web_forge_ack(SOUP_STATUS_INTERNAL_SERVER_ERROR);
	}

	venture_forge_release_event_clear(&event);

	return venture_web_forge_ack(SOUP_STATUS_ACCEPTED);
}

/*
 * A release's page: what it shipped, how it was built, where it went, and
 * the two actions the generated form cannot offer -- drafting the changelog
 * from the tickets, and publishing to the forge.
 */
static void
venture_web_append_release_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(GPtrArray) tickets = NULL;
	g_autofree gchar *changelog = NULL;
	g_autofree gchar *tag = NULL;
	g_autofree gchar *url = NULL;
	VentureReleaseStatus status;
	gint64 id;
	gint64 repo_id = 0;
	guint i;

	id = venture_entity_get_id(record);
	g_object_get(record, "changelog", &changelog, "tag", &tag, "url", &url,
	             "status", &status, "repo-id", &repo_id, NULL);

	tickets = venture_factory_release_tickets(
		venture_context_get_database(self->context), id);

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Shipped in this release</h2></div>"
	                         "<div class=\"card-body\">");

	if ((NULL == tickets) || (0 == tickets->len))
	{
		g_string_append(content,
			"<p class=\"muted\">No tickets are marked as fixed in this "
			"release. Set a ticket's <em>Fixed in</em> to add it.</p>");
	}
	else
	{
		g_string_append(content, "<ul class=\"relation-list\">");

		for (i = 0; i < tickets->len; i++)
		{
			g_autofree gchar *title = NULL;
			VentureEntity *ticket;
			VentureIssueType issue_type;

			ticket = g_ptr_array_index(tickets, i);
			g_object_get(ticket, "title", &title, "issue-type", &issue_type,
			             NULL);

			g_string_append(content, "<li><span class=\"badge\">");
			venture_html_escape_append(content,
				venture_enum_to_nick(VENTURE_TYPE_ISSUE_TYPE, (gint)issue_type));
			g_string_append_printf(content,
				"</span> <a href=\"/e/ticket/%" G_GINT64_FORMAT "\">",
				venture_entity_get_id(ticket));
			venture_html_escape_append(content, title);
			g_string_append(content, "</a></li>");
		}

		g_string_append(content, "</ul>");
	}

	g_string_append_printf(content,
		"<form method=\"post\" action=\"/releases/%" G_GINT64_FORMAT
		"/changelog\" class=\"inline\">"
		"<button class=\"btn btn-sm\" type=\"submit\">%s</button>"
		"<label class=\"muted\"> <input type=\"checkbox\" name=\"replace\" "
		"value=\"1\"> replace what is there</label></form>",
		id, venture_string_is_empty(changelog) ? "Draft changelog"
		                                       : "Redraft changelog");

	/* Publishing needs a repository on a forge, and is offered once,
	 * because a second publish is a second release on the forge. */
	if ((0 != repo_id) && venture_web_module_enabled(self, "forge") &&
	    venture_string_is_empty(url))
	{
		g_string_append_printf(content,
			" <form method=\"post\" action=\"/releases/%" G_GINT64_FORMAT
			"/publish\" class=\"inline\">"
			"<button class=\"btn btn-sm btn-primary\" type=\"submit\">"
			"Publish to forge</button>"
			"<label class=\"muted\"> <input type=\"checkbox\" "
			"name=\"prerelease\" value=\"1\"> pre-release</label></form>", id);
	}
	else if (!venture_string_is_empty(url))
	{
		g_string_append(content, " <a class=\"btn btn-sm\" href=\"");
		venture_html_escape_append(content, url);
		g_string_append(content, "\">On the forge</a>");
	}

	g_string_append(content, "</div></div>");
}

/*
 * POST /releases/:id/changelog - draft the changelog from the tickets.
 */
static HtmxResponse *
venture_web_ui_release_changelog(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) release = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	gint64 id;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "factory");

		if (NULL != gate)
			return gate;
	}

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	release = venture_database_get(venture_context_get_database(self->context),
	                               VENTURE_TYPE_RELEASE, id, &error);

	if (NULL == release)
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/release/%" G_GINT64_FORMAT, id);

	if (!venture_factory_apply_changelog(self->context, release,
		(0 == g_strcmp0(htmx_request_get_form_value(request, "replace"),
		                "1"))))
		return venture_web_redirect_to(destination);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           release, &actor, &error))
		return venture_web_error_response(error);

	return venture_web_redirect_to(destination);
}

/*
 * POST /releases/:id/publish - cut the release on the forge.
 *
 * The tag comes from the record, or "v" plus the version when none is set;
 * the body is the changelog. The forge creates the tag on the default
 * branch if it does not exist, and answers with the id and the page,
 * which are written back so the release is recognised when its own
 * webhook arrives a moment later.
 */
static HtmxResponse *
venture_web_ui_release_publish(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) release = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	gint64 id;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "factory");

		if (NULL != gate)
			return gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	release = venture_database_get(venture_context_get_database(self->context),
	                               VENTURE_TYPE_RELEASE, id, &error);

	if (NULL == release)
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/release/%" G_GINT64_FORMAT, id);
	venture_auth_to_actor(principal, &actor);

	if (!venture_factory_publish_release(self->context, release,
		(0 == g_strcmp0(htmx_request_get_form_value(request, "prerelease"),
		                "1")),
		&actor, &error))
		return venture_web_error_response(error);

	return venture_web_redirect_to(destination);
}

/* --- The factory API ------------------------------------------------------ */

/*
 * Loads the release in the path for an API action, with the gates.
 */
static HtmxResponse *
venture_web_api_release_load(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	GHashTable		 *params,
	VentureAuthPrincipal	**out_principal,
	VentureEntity		**out_release
){
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) release = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = venture_web_require_module_api(self, "factory");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != gate)
		return gate;

	principal = venture_auth_authenticate(self->auth, request);
	release = venture_database_get(venture_context_get_database(self->context),
	                               VENTURE_TYPE_RELEASE,
	                               g_ascii_strtoll(g_hash_table_lookup(params,
	                                                                   "id"),
	                                               NULL, 10),
	                               &error);

	if (NULL == release)
		return venture_web_error_response(error);

	*out_principal = g_steal_pointer(&principal);
	*out_release = g_steal_pointer(&release);

	return NULL;
}

/*
 * POST /api/v1/releases/:id/changelog - draft the changelog from the
 * tickets. {"replace": true} overwrites one somebody wrote. Answers with
 * the release; "changed" says whether anything was.
 */
static HtmxResponse *
venture_web_api_release_changelog(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) release = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	gboolean replace;
	gboolean changed;

	gate = venture_web_api_release_load(self, request, params, &principal,
	                                    &release);

	if (NULL != gate)
		return gate;

	body = htmx_request_get_json(request, NULL);
	replace = ((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
		? venture_json_object_get_bool(json_node_get_object(body), "replace",
		                               FALSE)
		: FALSE;

	changed = venture_factory_apply_changelog(self->context, release, replace);

	if (changed)
	{
		venture_auth_to_actor(principal, &actor);

		if (!venture_database_save(venture_context_get_database(self->context),
		                           release, &actor, &error))
			return venture_web_error_response(error);
	}

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "changed");
	json_builder_add_boolean_value(builder, changed);
	json_builder_set_member_name(builder, "release");
	json_builder_add_value(builder,
		venture_serializable_to_json(VENTURE_SERIALIZABLE(release), FALSE));
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * POST /api/v1/releases/:id/publish - cut it on the forge.
 * {"prerelease": true} marks it so.
 */
static HtmxResponse *
venture_web_api_release_publish(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) release = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	gboolean prerelease;

	gate = venture_web_api_release_load(self, request, params, &principal,
	                                    &release);

	if (NULL != gate)
		return gate;

	body = htmx_request_get_json(request, NULL);
	prerelease = ((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
		? venture_json_object_get_bool(json_node_get_object(body),
		                               "prerelease", FALSE)
		: FALSE;

	venture_auth_to_actor(principal, &actor);

	if (!venture_factory_publish_release(self->context, release, prerelease,
	                                     &actor, &error))
		return venture_web_error_response(error);

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(release), FALSE);

	return venture_web_json_response(node, 200);
}

/*
 * GET /api/v1/factory - the loop at a glance. ?organization_id=N scopes
 * it to that entity and those beneath it.
 */
static HtmxResponse *
venture_web_api_factory(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GArray) tree = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	const gchar *organization;

	(void)params;

	gate = venture_web_require_module_api(self, "factory");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	organization = htmx_request_get_query_param(request, "organization_id");

	if (!venture_string_is_empty(organization))
		tree = venture_web_organization_tree(self,
			g_ascii_strtoll(organization, NULL, 10));

	node = venture_factory_describe(self->context,
		(NULL != tree) ? (const gint64 *)tree->data : NULL,
		(NULL != tree) ? tree->len : 0, &error);

	if (NULL == node)
		return venture_web_error_response(error);

	return venture_web_json_response(node, 200);
}

/*
 * A milestone's page: how far along it is. The related-records panel
 * already lists the tickets; this is the number.
 */
static void
venture_web_append_milestone_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	gint64 total;
	gint64 done;

	venture_factory_milestone_progress(
		venture_context_get_database(self->context), venture_entity_get_id(record),
	                                       &total, &done);

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Progress</h2></div><div class=\"card-body\">"
	                         "<div class=\"stat-row\">");
	g_string_append_printf(content,
		"<div class=\"stat\"><span class=\"stat-value\">%" G_GINT64_FORMAT
		"</span><span class=\"stat-label\">Tickets</span></div>"
		"<div class=\"stat\"><span class=\"stat-value\">%" G_GINT64_FORMAT
		"</span><span class=\"stat-label\">Finished</span></div>"
		"<div class=\"stat\"><span class=\"stat-value\">%d%%</span>"
		"<span class=\"stat-label\">Complete</span></div>",
		total, done, (total > 0) ? (gint)((done * 100) / total) : 0);
	g_string_append(content, "</div></div></div>");
}

/*
 * An environment's page: what it is running now.
 */
static void
venture_web_append_environment_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(VentureEntity) deployment = NULL;
	g_autoptr(VentureEntity) release = NULL;
	gint64 release_id = 0;

	deployment = venture_factory_current_deployment(
		venture_context_get_database(self->context), venture_entity_get_id(record));

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Running now</h2></div><div class=\"card-body\">");

	if (NULL != deployment)
	{
		g_object_get(deployment, "release-id", &release_id, NULL);
		release = venture_database_get(
			venture_context_get_database(self->context), VENTURE_TYPE_RELEASE,
			release_id, NULL);
	}

	if (NULL == release)
	{
		g_string_append(content, "<p class=\"muted\">Nothing has been "
		                         "deployed here successfully yet.</p>");
	}
	else
	{
		g_autofree gchar *version = NULL;
		g_autoptr(GDateTime) when = NULL;

		g_object_get(release, "number", &version, NULL);
		g_object_get(deployment, "deployed-at", &when, NULL);

		g_string_append_printf(content,
			"<p><a href=\"/e/release/%" G_GINT64_FORMAT "\">", release_id);
		venture_html_escape_append(content, version);
		g_string_append(content, "</a>");

		if (NULL != when)
		{
			g_autofree gchar *relative = NULL;

			relative = venture_time_to_relative_string(when);
			g_string_append(content, " <span class=\"muted\">deployed ");
			venture_html_escape_append(content, relative);
			g_string_append(content, "</span>");
		}

		g_string_append_printf(content,
			" <a class=\"btn btn-sm\" href=\"/e/deployment/%" G_GINT64_FORMAT
			"\">Deployment</a></p>",
			venture_entity_get_id(deployment));
	}

	g_string_append(content, "</div></div>");
}

/*
 * Appends one list card for the factory page from a query, with a
 * per-row renderer.
 */
typedef void (*VentureWebFactoryRowFunc) (VentureWebServer *self,
                                           GString          *content,
                                           VentureEntity    *record);

static void
venture_web_factory_card(
	VentureWebServer		*self,
	HtmxRequest			*request,
	GString				*content,
	const gchar			*title,
	const gchar			*all_path,
	VentureQuery			*query,
	VentureWebFactoryRowFunc	 row,
	const gchar			*empty,
	gboolean			 wide
){
	g_autoptr(GPtrArray) records = NULL;
	guint i;

	venture_web_scope_to_active_organization(self, request, query);
	records = venture_database_find(venture_context_get_database(self->context),
	                                query, NULL);

	g_string_append_printf(content,
		"<div class=\"card dash-card%s\"><div class=\"card-head\"><h2>",
		wide ? " dash-wide" : "");
	venture_html_escape_append(content, title);
	g_string_append_printf(content,
		"</h2><a class=\"btn btn-sm\" href=\"%s\">All</a></div>"
		"<div class=\"card-body\">", all_path);

	if ((NULL == records) || (0 == records->len))
	{
		g_string_append(content, "<p class=\"muted\">");
		venture_html_escape_append(content, empty);
		g_string_append(content, "</p>");
	}
	else
	{
		g_string_append(content, "<ul class=\"relation-list\">");

		for (i = 0; i < records->len; i++)
			row(self, content, g_ptr_array_index(records, i));

		g_string_append(content, "</ul>");
	}

	g_string_append(content, "</div></div>");
}

static void
venture_web_factory_milestone_row(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autofree gchar *name = NULL;
	g_autoptr(GDateTime) due = NULL;
	VentureMilestoneStatus status;
	gint64 total;
	gint64 done;

	g_object_get(record, "name", &name, "status", &status, "due-on", &due,
	             NULL);
	venture_factory_milestone_progress(
		venture_context_get_database(self->context), venture_entity_get_id(record),
	                                       &total, &done);

	g_string_append(content, "<li><span class=\"badge\">");
	venture_html_escape_append(content,
		venture_enum_to_nick(VENTURE_TYPE_MILESTONE_STATUS, (gint)status));
	g_string_append_printf(content,
		"</span> <a href=\"/e/milestone/%" G_GINT64_FORMAT "\">",
		venture_entity_get_id(record));
	venture_html_escape_append(content, name);
	g_string_append_printf(content,
		"</a> <span class=\"muted\">%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT
		" done</span>", done, total);

	if (NULL != due)
	{
		g_autofree gchar *text = NULL;

		text = venture_time_to_date_string(due, NULL);
		g_string_append(content, " <span class=\"muted\">due ");
		venture_html_escape_append(content, text);
		g_string_append(content, "</span>");
	}

	g_string_append(content, "</li>");
}

static void
venture_web_factory_release_row(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autofree gchar *version = NULL;
	g_autoptr(GDateTime) released = NULL;
	VentureReleaseStatus status;

	g_object_get(record, "number", &version, "status", &status,
	             "released-at", &released, NULL);

	g_string_append(content, "<li><span class=\"badge\">");
	venture_html_escape_append(content,
		venture_enum_to_nick(VENTURE_TYPE_RELEASE_STATUS, (gint)status));
	g_string_append_printf(content,
		"</span> <a href=\"/e/release/%" G_GINT64_FORMAT "\">",
		venture_entity_get_id(record));
	venture_html_escape_append(content, version);
	g_string_append(content, "</a>");

	if (NULL != released)
	{
		g_autofree gchar *relative = NULL;

		relative = venture_time_to_relative_string(released);
		g_string_append(content, " <span class=\"muted\">");
		venture_html_escape_append(content, relative);
		g_string_append(content, "</span>");
	}

	g_string_append(content, "</li>");
}

static void
venture_web_factory_build_row(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autofree gchar *title = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *workflow = NULL;
	g_autoptr(GDateTime) started = NULL;
	VentureBuildStatus status;
	const gchar *tone;

	g_object_get(record, "title", &title, "ref", &ref, "workflow", &workflow,
	             "status", &status, "started-at", &started, NULL);

	tone = (VENTURE_BUILD_STATUS_SUCCEEDED == status) ? " positive"
	     : (VENTURE_BUILD_STATUS_FAILED == status) ? " negative" : "";

	g_string_append_printf(content, "<li><span class=\"badge%s\">", tone);
	venture_html_escape_append(content,
		venture_enum_to_nick(VENTURE_TYPE_BUILD_STATUS, (gint)status));
	g_string_append_printf(content,
		"</span> <a href=\"/e/build/%" G_GINT64_FORMAT "\">",
		venture_entity_get_id(record));
	venture_html_escape_append(content,
		!venture_string_is_empty(title) ? title
		: !venture_string_is_empty(workflow) ? workflow : "build");
	g_string_append(content, "</a>");

	if (!venture_string_is_empty(ref))
	{
		g_string_append(content, " <span class=\"muted\">");
		venture_html_escape_append(content, ref);
		g_string_append(content, "</span>");
	}

	if (NULL != started)
	{
		g_autofree gchar *relative = NULL;

		relative = venture_time_to_relative_string(started);
		g_string_append(content, " <span class=\"muted\">");
		venture_html_escape_append(content, relative);
		g_string_append(content, "</span>");
	}

	g_string_append(content, "</li>");
}

static void
venture_web_factory_environment_row(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(VentureEntity) deployment = NULL;
	g_autoptr(VentureEntity) release = NULL;
	g_autofree gchar *name = NULL;
	VentureEnvironmentKind kind;
	gint64 release_id = 0;

	g_object_get(record, "name", &name, "kind", &kind, NULL);

	g_string_append(content, "<li><span class=\"badge\">");
	venture_html_escape_append(content,
		venture_enum_to_nick(VENTURE_TYPE_ENVIRONMENT_KIND, (gint)kind));
	g_string_append_printf(content,
		"</span> <a href=\"/e/environment/%" G_GINT64_FORMAT "\">",
		venture_entity_get_id(record));
	venture_html_escape_append(content, name);
	g_string_append(content, "</a>");

	deployment = venture_factory_current_deployment(
		venture_context_get_database(self->context), venture_entity_get_id(record));

	if (NULL != deployment)
	{
		g_object_get(deployment, "release-id", &release_id, NULL);
		release = venture_database_get(
			venture_context_get_database(self->context), VENTURE_TYPE_RELEASE,
			release_id, NULL);
	}

	if (NULL != release)
	{
		g_autofree gchar *version = NULL;

		g_object_get(release, "number", &version, NULL);
		g_string_append_printf(content,
			" <span class=\"muted\">running</span> <a href=\"/e/release/%"
			G_GINT64_FORMAT "\">", release_id);
		venture_html_escape_append(content, version);
		g_string_append(content, "</a>");
	}
	else
	{
		g_string_append(content, " <span class=\"muted\">nothing deployed"
		                         "</span>");
	}

	g_string_append(content, "</li>");
}

static void
venture_web_factory_incident_row(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autofree gchar *title = NULL;
	g_autoptr(GDateTime) started = NULL;
	VentureIncidentSeverity severity;
	VentureIncidentStatus status;

	g_object_get(record, "title", &title, "severity", &severity,
	             "status", &status, "started-at", &started, NULL);

	g_string_append_printf(content, "<li><span class=\"badge%s\">",
		((VENTURE_INCIDENT_SEVERITY_SEV1 == severity) ||
		 (VENTURE_INCIDENT_SEVERITY_SEV2 == severity)) ? " negative"
		                                              : " warning");
	venture_html_escape_append(content,
		venture_enum_to_nick(VENTURE_TYPE_INCIDENT_SEVERITY, (gint)severity));
	g_string_append(content, "</span> <span class=\"badge\">");
	venture_html_escape_append(content,
		venture_enum_to_nick(VENTURE_TYPE_INCIDENT_STATUS, (gint)status));
	g_string_append_printf(content,
		"</span> <a href=\"/e/incident/%" G_GINT64_FORMAT "\">",
		venture_entity_get_id(record));
	venture_html_escape_append(content, title);
	g_string_append(content, "</a>");

	if (NULL != started)
	{
		g_autofree gchar *relative = NULL;

		relative = venture_time_to_relative_string(started);
		g_string_append(content, " <span class=\"muted\">");
		venture_html_escape_append(content, relative);
		g_string_append(content, "</span>");
	}

	g_string_append(content, "</li>");
}

/*
 * GET /factory - the loop at a glance.
 *
 * One page, five lists: what is planned, what shipped, what the CI is
 * doing, what each environment is running, and what is on fire. Each is
 * the newest few with a link to the full list, because the page's job is
 * "where does the factory stand", and the answer to that is short.
 */
static HtmxResponse *
venture_web_ui_factory(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "factory");

		if (NULL != gate)
			return gate;
	}

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	content = g_string_new(
		"<div class=\"page-head\"><div class=\"page-title\">"
		"<h1>Factory</h1><span class=\"subtitle\">From a ticket to a "
		"running release, and back</span></div>"
		"<div class=\"page-actions\">"
		"<a class=\"btn\" href=\"/reports/lead_time\">Lead time</a> "
		"<a class=\"btn\" href=\"/reports/releases\">Releases report</a> "
		"<a class=\"btn btn-primary\" href=\"/e/release/new\">New release</a>"
		"</div></div><div class=\"dash-grid\">");

	{
		g_autoptr(VentureQuery) query = NULL;

		query = venture_query_new(VENTURE_TYPE_MILESTONE);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "completed", NULL);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "cancelled", NULL);
		venture_query_add_order(query, "due-on", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, 8);
		venture_web_factory_card(self, request, content, "Milestones",
		                         "/e/milestone", query,
		                         venture_web_factory_milestone_row,
		                         "No open milestones.", FALSE);
	}

	{
		g_autoptr(VentureQuery) query = NULL;

		query = venture_query_new(VENTURE_TYPE_RELEASE);
		venture_query_add_order(query, "released-at", VENTURE_SORT_DESCENDING,
		                        NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		venture_query_set_limit(query, 8);
		venture_web_factory_card(self, request, content, "Releases",
		                         "/e/release", query,
		                         venture_web_factory_release_row,
		                         "No releases yet.", FALSE);
	}

	{
		g_autoptr(VentureQuery) query = NULL;

		query = venture_query_new(VENTURE_TYPE_BUILD);
		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		venture_query_set_limit(query, 10);
		venture_web_factory_card(self, request, content, "Builds",
		                         "/e/build", query,
		                         venture_web_factory_build_row,
		                         "No builds reported. Point the forge's "
		                         "webhook at this server with the "
		                         "workflow_run event on, or record one.",
		                         FALSE);
	}

	{
		g_autoptr(VentureQuery) query = NULL;

		query = venture_query_new(VENTURE_TYPE_ENVIRONMENT);
		venture_query_add_order(query, "kind", VENTURE_SORT_DESCENDING, NULL);
		venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, 12);
		venture_web_factory_card(self, request, content, "Environments",
		                         "/e/environment", query,
		                         venture_web_factory_environment_row,
		                         "No environments yet.", FALSE);
	}

	{
		g_autoptr(VentureQuery) query = NULL;

		query = venture_query_new(VENTURE_TYPE_INCIDENT);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
		                                "resolved", NULL);
		venture_query_add_order(query, "started-at", VENTURE_SORT_DESCENDING,
		                        NULL);
		venture_query_set_limit(query, 10);
		venture_web_factory_card(self, request, content, "Open incidents",
		                         "/e/incident", query,
		                         venture_web_factory_incident_row,
		                         "Nothing is on fire.", TRUE);
	}

	g_string_append(content, "</div>");

	return venture_web_html_response(
		venture_web_page(self, request, "/factory", "Factory", content->str),
		200);
}

/* --- Dashboards ----------------------------------------------------------- */

/*
 * Pages of widgets. Every handler here loads the dashboard by slug through
 * one gate, which checks the module, the session, the role and -- for a
 * personal dashboard -- that the viewer owns it, answering NOT_FOUND
 * rather than FORBIDDEN so that whether somebody else's page exists is not
 * something a stranger can learn.
 */

/*
 * Gates a dashboard request and loads the dashboard.
 *
 * Returns: (transfer full) (nullable): a response to return instead, or
 *   %NULL when the request may proceed and @out_dashboard is set
 */
static HtmxResponse *
venture_web_dashboard_load(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	GHashTable		 *params,
	VentureUserRole		  role,
	gboolean		  api,
	VentureAuthPrincipal	**out_principal,
	VentureDashboard	**out_dashboard
){
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	const gchar *slug;

	gate = api ? venture_web_require_module_api(self, "dashboards")
	           : venture_web_require_module_ui(self, request, "dashboards");

	if (NULL != gate)
		return gate;

	if (!api)
	{
		gate = venture_web_ui_require_session(self, request);

		if (NULL != gate)
			return gate;
	}

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, role, &error))
		return venture_web_error_response(error);

	slug = g_hash_table_lookup(params, "slug");
	dashboard = venture_dashboard_find_by_slug(
		venture_context_get_database(self->context), slug, &error);

	if ((NULL != dashboard) &&
	    !venture_dashboard_is_visible_to(dashboard, principal->user_id))
	{
		g_clear_object(&dashboard);
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no dashboard called \"%s\"", slug);
	}

	if (NULL == dashboard)
	{
		if (api)
			return venture_web_error_response(error);

		return venture_web_html_response(
			venture_web_page(self, request, "/dashboards", "Not found",
				"<div class=\"notice negative\">No such dashboard.</div>"),
			404);
	}

	if (NULL != out_principal)
		*out_principal = g_steal_pointer(&principal);

	*out_dashboard = g_steal_pointer(&dashboard);

	return NULL;
}

/*
 * Fills the scope a page renders under: the viewer, and the entity they
 * have picked in the sidebar, with everything beneath it. @tree is owned
 * by the caller and must outlive @scope.
 */
static void
venture_web_dashboard_scope(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	VentureAuthPrincipal	 *principal,
	VentureDashboard	 *dashboard,
	VentureWidgetScope	 *scope,
	GArray			**tree
){
	gint64 active;

	scope->organization_ids = NULL;
	scope->n_organizations = 0;
	scope->user_id = principal->user_id;
	scope->username = principal->name;
	scope->venture_id = 0;
	g_object_get(dashboard, "venture-id", &scope->venture_id, NULL);

	active = venture_web_active_organization(self, request);
	*tree = NULL;

	if (0 != active)
	{
		*tree = venture_web_organization_tree(self, active);
		scope->organization_ids = (const gint64 *)(*tree)->data;
		scope->n_organizations = (*tree)->len;
	}
}

/*
 * One widget as a card. The same markup serves the page and the refresh
 * fragment, so a widget that reloads itself every minute lands exactly
 * where it was.
 */
static void
venture_web_append_widget_card(
	VentureWebServer		*self,
	GString				*content,
	const gchar			*slug,
	const VentureWidgetPlacement	*placement,
	VentureWidgetResult		*result,
	gboolean			 editing
){
	VentureDashboardWidget *widget;
	g_autofree gchar *kind = NULL;
	g_autofree gchar *slug_attr = NULL;
	gint64 refresh = 0;
	gint64 id;

	(void)self;

	widget = placement->widget;
	id = venture_entity_get_id(VENTURE_ENTITY(widget));
	g_object_get(widget, "kind", &kind, "refresh-seconds", &refresh, NULL);
	slug_attr = venture_attribute_escape(slug);

	/* The place on the grid is written into the style, not a class, so
	 * a card lands exactly where the layout put it whatever the row
	 * count; the data attributes are what the editor's drag reads. */
	g_string_append_printf(content,
		"<div class=\"card dash-card widget widget-%s%s\" "
		"id=\"widget-%" G_GINT64_FORMAT "\" "
		"style=\"grid-column:%u / span %u;grid-row:%u / span %u\" "
		"data-widget=\"%" G_GINT64_FORMAT "\" data-col=\"%u\" data-row=\"%u\" "
		"data-width=\"%u\" data-height=\"%u\"%s",
		(NULL != kind) ? kind : "unknown",
		editing ? " placeable" : "", id,
		placement->col, placement->width, placement->row, placement->height,
		id, placement->col, placement->row, placement->width,
		placement->height, "");

	/* A refresh is an ordinary HTMX poll; nothing here knows or cares
	 * what the widget shows. Not while editing, where a reload would
	 * pull the controls out from under the cursor. */
	if ((refresh > 0) && !editing)
	{
		g_string_append_printf(content,
			" hx-get=\"/dashboards/%s/widgets/%" G_GINT64_FORMAT "\" "
			"hx-trigger=\"every %" G_GINT64_FORMAT "s\" hx-swap=\"outerHTML\"",
			slug_attr, id, MAX(refresh, (gint64)5));
	}

	g_string_append(content, "><div class=\"card-head\"><h2>");
	venture_html_escape_append(content, result->title);
	g_string_append(content, "</h2><div class=\"card-tools\">");

	if (editing)
	{
		g_string_append_printf(content,
			"<a class=\"btn btn-sm\" href=\"/dashboards/%s/widgets/%"
			G_GINT64_FORMAT "/edit\">Edit</a>", slug_attr, id);
	}
	else if (NULL != result->link)
	{
		g_autofree gchar *href = NULL;

		href = venture_attribute_escape(result->link);
		g_string_append(content, "<a class=\"btn btn-sm\" href=\"");
		g_string_append(content, href);
		g_string_append(content, "\">");
		venture_html_escape_append(content,
			(NULL != result->link_label) ? result->link_label : "All");
		g_string_append(content, "</a>");
	}

	g_string_append(content, "</div></div>");

	/*
	 * The corner you drag to resize, and the cell the card currently
	 * holds. Rendered rather than made in script so the card carries
	 * its own affordance the moment the grid is editable.
	 */
	if (editing)
	{
		g_string_append_printf(content,
			"<span class=\"widget-where\">%u,%u &middot; %u&times;%u</span>"
			"<span class=\"widget-resize\" data-resize "
			"title=\"Drag to resize\" aria-hidden=\"true\"></span>",
			placement->col, placement->row, placement->width,
			placement->height);
	}

	/*
	 * The grid controls, on a bar of their own under the head: eight
	 * nudge buttons do not fit beside a title in a one-column card. As
	 * forms, so the grid is editable with no scripting and from the
	 * keyboard; the drag is the same route with a destination.
	 */
	if (editing)
	{
		g_string_append_printf(content,
			"<div class=\"widget-controls\">"
			"<form method=\"post\" action=\"/dashboards/%s/widgets/%"
			G_GINT64_FORMAT "/move\" class=\"inline nudge\">"
			"<span class=\"muted small\">Move</span>"
			"<button class=\"btn btn-sm\" name=\"direction\" value=\"left\" "
			"title=\"Move left\">&larr;</button>"
			"<button class=\"btn btn-sm\" name=\"direction\" value=\"up\" "
			"title=\"Move up\">&uarr;</button>"
			"<button class=\"btn btn-sm\" name=\"direction\" value=\"down\" "
			"title=\"Move down\">&darr;</button>"
			"<button class=\"btn btn-sm\" name=\"direction\" value=\"right\" "
			"title=\"Move right\">&rarr;</button>"
			"<span class=\"muted small\">Size</span>"
			"<button class=\"btn btn-sm\" name=\"direction\" "
			"value=\"narrower\" title=\"Narrower\">W&minus;</button>"
			"<button class=\"btn btn-sm\" name=\"direction\" value=\"wider\" "
			"title=\"Wider\">W+</button>"
			"<button class=\"btn btn-sm\" name=\"direction\" "
			"value=\"shorter\" title=\"Shorter\">H&minus;</button>"
			"<button class=\"btn btn-sm\" name=\"direction\" value=\"taller\" "
			"title=\"Taller\">H+</button></form>"
			"<form method=\"post\" action=\"/dashboards/%s/widgets/%"
			G_GINT64_FORMAT "/delete\" class=\"inline\">"
			"<button class=\"btn btn-sm btn-danger\" type=\"submit\">"
			"Remove</button></form>"
			"</div>",
			slug_attr, id, slug_attr, id);
	}

	g_string_append(content, "<div class=\"card-body\">");

	if (NULL != result->error)
	{
		g_string_append(content, "<div class=\"notice negative\">");
		venture_html_escape_append(content, result->error);
		g_string_append(content, "</div>");
	}
	else if (NULL != result->html)
	{
		g_string_append(content, result->html);
	}

	g_string_append(content, "</div></div>");
}

/*
 * The grid of every widget on a dashboard.
 */
static void
venture_web_append_dashboard_grid(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureAuthPrincipal	*principal,
	VentureDashboard	*dashboard,
	GString			*content,
	gboolean		 editing
){
	g_autoptr(GPtrArray) placements = NULL;
	g_autoptr(GArray) tree = NULL;
	g_autofree gchar *slug = NULL;
	VentureWidgetScope scope;
	VentureDashboardLayout layout;
	guint columns;
	guint rows;
	guint i;

	g_object_get(dashboard, "slug", &slug, "layout", &layout, NULL);
	columns = venture_dashboard_layout_get_columns(layout);
	venture_web_dashboard_scope(self, request, principal, dashboard, &scope,
	                            &tree);

	placements = venture_dashboard_layout(
		venture_context_get_database(self->context), dashboard, NULL);
	rows = 0;

	for (i = 0; (NULL != placements) && (i < placements->len); i++)
	{
		const VentureWidgetPlacement *placement;

		placement = g_ptr_array_index(placements, i);
		rows = MAX(rows, placement->row + placement->height - 1);
	}

	g_string_append_printf(content,
		"<div class=\"widget-grid cols-%u%s\" data-grid-columns=\"%u\" "
		"data-grid-rows=\"%u\"%s>",
		columns, editing ? " editing" : "", columns, rows,
		editing ? " data-grid-editor" : "");

	/*
	 * While editing the cells are drawn behind the cards, two spare rows
	 * past the last one. They are scenery rather than drop targets --
	 * the editor works out where a card will land from the pointer's
	 * coordinates, not from what is under it -- but they are what makes
	 * a grid look like a grid to somebody moving things around on it.
	 */
	if (editing)
	{
		guint r;
		guint c;

		/* Two spare rows below the last card, so there is somewhere to
		 * drag downward to and the canvas reads as having room. */
		for (r = 1; r <= rows + 2; r++)
			for (c = 1; c <= columns; c++)
				g_string_append_printf(content,
					"<div class=\"grid-cell\" data-cell data-col=\"%u\" "
					"data-row=\"%u\" style=\"grid-column:%u;grid-row:%u\">"
					"</div>", c, r, c, r);
	}

	for (i = 0; (NULL != placements) && (i < placements->len); i++)
	{
		g_autoptr(VentureWidgetResult) result = NULL;
		const VentureWidgetPlacement *placement;

		placement = g_ptr_array_index(placements, i);
		result = venture_dashboard_render_widget(self->context,
		                                         placement->widget, &scope);
		venture_web_append_widget_card(self, content, slug, placement, result,
		                               editing);
	}

	if ((NULL == placements) || (0 == placements->len))
	{
		g_string_append_printf(content,
			"<div class=\"empty span-full\"><h3>No widgets yet</h3>"
			"<p class=\"muted\">Add a count, a list, a report, a note -- "
			"anything from any module -- and arrange them here.</p>"
			"<p><a class=\"btn btn-primary\" href=\"/dashboards/%s/widgets/new\">"
			"Add a widget</a></p></div>", slug);
	}

	g_string_append(content, "</div>");
}

/*
 * A home dashboard, rendered for "/". Returns %NULL when none is set or
 * the viewer may not see it, in which case the built-in overview is shown.
 */
static gchar *
venture_web_render_home_dashboard(
	VentureWebServer	*self,
	HtmxRequest		*request
){
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(GString) content = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *description = NULL;

	if (!venture_web_module_enabled(self, "dashboards"))
		return NULL;

	principal = venture_auth_authenticate(self->auth, request);
	dashboard = venture_dashboard_find_home(
		venture_context_get_database(self->context), principal->user_id);

	if (NULL == dashboard)
		return NULL;

	g_object_get(dashboard, "name", &name, "slug", &slug,
	             "description", &description, NULL);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>");
	venture_html_escape_append(content, name);
	g_string_append(content, "</h1><span class=\"subtitle\">");
	venture_html_escape_append(content,
		!venture_string_is_empty(description) ? description
		                                      : "Your home dashboard");
	g_string_append(content, "</span></div><div class=\"page-actions\">"
	                         "<a class=\"btn\" href=\"/overview\">Built-in "
	                         "overview</a> ");
	g_string_append_printf(content,
		"<a class=\"btn\" href=\"/dashboards\">All dashboards</a> "
		"<a class=\"btn btn-primary\" href=\"/dashboards/%s/edit\">Edit</a>"
		"</div></div>", slug);

	venture_web_append_dashboard_grid(self, request, principal, dashboard,
	                                  content, FALSE);

	return venture_web_page(self, request, "/", name, content->str);
}

/*
 * The dashboards a viewer may see, for the sidebar.
 */
static void
venture_web_append_dashboard_nav(
	VentureWebServer	*self,
	HtmxRequest		*request,
	GString			*html,
	const gchar		*active
){
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) dashboards = NULL;
	guint i;

	if (!venture_web_module_enabled(self, "dashboards"))
		return;

	principal = venture_auth_authenticate(self->auth, request);

	if (!principal->authenticated)
		return;

	dashboards = venture_dashboard_list_visible(
		venture_context_get_database(self->context), principal->user_id,
		NULL);

	if ((NULL == dashboards) || (0 == dashboards->len))
		return;

	g_string_append(html, "<div class=\"nav-section\">Views</div>");

	for (i = 0; i < dashboards->len; i++)
	{
		VentureDashboard *dashboard;
		g_autofree gchar *name = NULL;
		g_autofree gchar *slug = NULL;
		g_autofree gchar *path = NULL;

		dashboard = g_ptr_array_index(dashboards, i);
		g_object_get(dashboard, "name", &name, "slug", &slug, NULL);
		path = g_strdup_printf("/dashboards/%s", slug);

		g_string_append_printf(html, "<a class=\"nav-item%s\" href=\"%s\">"
			"<span class=\"icon\">" VENTURE_ICON(
				"<rect x=\"3\" y=\"4\" width=\"18\" height=\"16\" rx=\"2\"/>"
				"<path d=\"M3 10h18\"/><path d=\"M10 10v10\"/>")
			"</span>",
			(0 == g_strcmp0(active, path)) ? " active" : "", path);
		venture_html_escape_append(html, name);
		g_string_append(html, "</a>");
	}
}

/*
 * GET /dashboards - every dashboard you may see, and the ways to make one.
 */
static HtmxResponse *
venture_web_ui_dashboards(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) dashboards = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	const VentureDashboardTemplate *templates;
	HtmxResponse *gate;
	gsize n_templates;
	gsize t;
	guint i;

	(void)params;

	gate = venture_web_require_module_ui(self, request, "dashboards");

	if (NULL != gate)
		return gate;

	gate = venture_web_ui_require_session(self, request);

	if (NULL != gate)
		return gate;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	dashboards = venture_dashboard_list_visible(
		venture_context_get_database(self->context), principal->user_id,
		&error);

	if (NULL == dashboards)
		return venture_web_error_response(error);

	content = g_string_new(
		"<div class=\"page-head\"><div class=\"page-title\">"
		"<h1>Dashboards</h1><span class=\"subtitle\">Pages of widgets, "
		"arranged your way</span></div>"
		"<div class=\"page-actions\">"
		"<a class=\"btn\" href=\"/overview\">Built-in overview</a>"
		"</div></div>");

	g_string_append(content, "<div class=\"grid cols-3\">");

	for (i = 0; i < dashboards->len; i++)
	{
		VentureDashboard *dashboard;
		g_autofree gchar *name = NULL;
		g_autofree gchar *slug = NULL;
		g_autofree gchar *description = NULL;
		VentureDashboardPurpose purpose;
		gboolean home = FALSE;
		gboolean personal = FALSE;

		dashboard = g_ptr_array_index(dashboards, i);
		g_object_get(dashboard, "name", &name, "slug", &slug,
		             "description", &description, "purpose", &purpose,
		             "home", &home, "personal", &personal, NULL);

		g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
		                         "<h2>");
		g_string_append_printf(content, "<a href=\"/dashboards/%s\">", slug);
		venture_html_escape_append(content, name);
		g_string_append(content, "</a></h2><p class=\"muted\">");
		venture_html_escape_append(content,
			!venture_string_is_empty(description) ? description : "");
		g_string_append(content, "</p><p><span class=\"badge\">");
		venture_html_escape_append(content,
			venture_enum_to_nick(VENTURE_TYPE_DASHBOARD_PURPOSE, (gint)purpose));
		g_string_append(content, "</span>");

		if (home)
			g_string_append(content, " <span class=\"badge info\">home</span>");

		if (personal)
			g_string_append(content, " <span class=\"badge\">personal</span>");

		g_string_append_printf(content,
			"</p><a class=\"btn btn-primary btn-sm\" href=\"/dashboards/%s\">"
			"Open</a> <a class=\"btn btn-sm\" href=\"/dashboards/%s/edit\">"
			"Edit</a></div></div>", slug, slug);
	}

	g_string_append(content, "</div>");

	if (0 == dashboards->len)
	{
		g_string_append(content,
			"<div class=\"empty\"><h3>No dashboards yet</h3>"
			"<p class=\"muted\">Start from a template below, or make an "
			"empty one and add widgets to it.</p></div>");
	}

	/* Making one: from a template, or blank. Editors only, so the forms
	 * are not offered to a viewer who could not submit them. */
	if (venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, NULL))
	{
		templates = venture_dashboard_get_templates(&n_templates);

		g_string_append(content,
			"<h2 class=\"section-title\">Start from a template</h2>"
			"<div class=\"grid cols-2\">");

		for (t = 0; t < n_templates; t++)
		{
			g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
			                         "<h2>");
			venture_html_escape_append(content, templates[t].label);
			g_string_append(content, "</h2><p class=\"muted\">");
			venture_html_escape_append(content, templates[t].description);
			g_string_append(content, "</p>");

			if ((NULL != templates[t].module) &&
			    !venture_web_module_enabled(self, templates[t].module))
			{
				g_string_append(content, "<p class=\"muted small\">Mostly "
				                         "about the ");
				venture_html_escape_append(content, templates[t].module);
				g_string_append(content, " module, which is off; its "
				                         "widgets will say so until it is "
				                         "on.</p>");
			}

			g_string_append_printf(content,
				"<form method=\"post\" action=\"/dashboards\" class=\"inline\">"
				"<input type=\"hidden\" name=\"template\" value=\"%s\">"
				"<button class=\"btn btn-primary btn-sm\" type=\"submit\">"
				"Create</button></form></div></div>", templates[t].name);
		}

		g_string_append(content, "</div>");

		g_string_append(content,
			"<h2 class=\"section-title\">Or an empty one</h2>"
			"<div class=\"card\"><div class=\"card-body\">"
			"<form method=\"post\" action=\"/dashboards\">"
			"<div class=\"form-grid\">"
			"<div class=\"field\"><label><span class=\"field-label\">Name"
			"<span class=\"required\">*</span></span>"
			"<input type=\"text\" name=\"name\" required placeholder=\"e.g. "
			"Launch week\"></label></div>"
			"<div class=\"field\"><label><span class=\"field-label\">Purpose"
			"</span><select name=\"purpose\">"
			"<option value=\"overview\">Overview</option>"
			"<option value=\"reporting\">Reporting</option>"
			"<option value=\"work\">Work</option></select></label></div>"
			"<div class=\"field\"><label><span class=\"field-label\">Personal"
			"</span><input type=\"hidden\" name=\"personal\" value=\"false\">"
			"<input type=\"checkbox\" name=\"personal\" value=\"true\"></label>"
			"<div class=\"hint\">Only you can see it</div></div>"
			"</div><div class=\"form-actions\">"
			"<button class=\"btn btn-primary\" type=\"submit\">Create</button>"
			"</div></form></div></div>");

		g_string_append(content,
			"<h2 class=\"section-title\">Or import a definition</h2>"
			"<div class=\"card\"><div class=\"card-body\">"
			"<form method=\"post\" action=\"/dashboards/import\">"
			"<div class=\"field\"><label><span class=\"field-label\">JSON, "
			"as Export writes it</span>"
			"<textarea name=\"definition\" rows=\"8\" class=\"mono\" "
			"placeholder='{\"name\": \"...\", \"widgets\": [...]}'>"
			"</textarea></label></div>"
			"<div class=\"form-actions\">"
			"<button class=\"btn btn-primary\" type=\"submit\">Import</button>"
			"</div></form></div></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/dashboards", "Dashboards",
		                 content->str), 200);
}

/*
 * POST /dashboards - make one, from a template or from a name.
 */
static HtmxResponse *
venture_web_ui_dashboard_create(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	g_autofree gchar *slug = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *template_name;

	(void)params;

	gate = venture_web_require_module_ui(self, request, "dashboards");

	if (NULL != gate)
		return gate;

	gate = venture_web_ui_require_session(self, request);

	if (NULL != gate)
		return gate;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);
	template_name = htmx_request_get_form_value(request, "template");

	if (!venture_string_is_empty(template_name))
	{
		dashboard = venture_dashboard_create_from_template(self->context,
			template_name, principal->user_id, &actor, &error);
	}
	else
	{
		g_autoptr(GPtrArray) specs = NULL;

		dashboard = venture_dashboard_new();
		specs = venture_entity_get_field_specs(VENTURE_ENTITY(dashboard));

		if (!venture_web_apply_form(self, request, VENTURE_ENTITY(dashboard),
		                            specs, &error))
			return venture_web_error_response(error);

		g_object_set(dashboard, "owner-user-id", principal->user_id, NULL);

		if (!venture_database_save(venture_context_get_database(self->context),
		                           VENTURE_ENTITY(dashboard), &actor, &error))
			g_clear_object(&dashboard);
	}

	if (NULL == dashboard)
		return venture_web_error_response(error);

	g_object_get(dashboard, "slug", &slug, NULL);
	destination = g_strdup_printf("/dashboards/%s", slug);

	return venture_web_redirect_to(destination);
}

/*
 * POST /dashboards/import - a definition pasted into the box.
 */
static HtmxResponse *
venture_web_ui_dashboard_import(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(JsonNode) definition = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	g_autofree gchar *slug = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *text;

	(void)params;

	gate = venture_web_require_module_ui(self, request, "dashboards");

	if (NULL != gate)
		return gate;

	gate = venture_web_ui_require_session(self, request);

	if (NULL != gate)
		return gate;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	text = htmx_request_get_form_value(request, "definition");

	if (venture_string_is_empty(text))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "Paste a dashboard definition to import");
		return venture_web_error_response(error);
	}

	definition = venture_json_parse(text, &error);

	if (NULL == definition)
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);
	dashboard = venture_dashboard_import(self->context, definition,
	                                     principal->user_id, &actor, &error);

	if (NULL == dashboard)
		return venture_web_error_response(error);

	g_object_get(dashboard, "slug", &slug, NULL);
	destination = g_strdup_printf("/dashboards/%s", slug);

	return venture_web_redirect_to(destination);
}

/*
 * GET /dashboards/:slug - the page.
 */
static HtmxResponse *
venture_web_ui_dashboard_view(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(GString) content = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *description = NULL;
	g_autofree gchar *active = NULL;
	HtmxResponse *gate;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_VIEWER, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	g_object_get(dashboard, "name", &name, "slug", &slug,
	             "description", &description, NULL);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>");
	venture_html_escape_append(content, name);
	g_string_append(content, "</h1>");

	if (!venture_string_is_empty(description))
	{
		g_string_append(content, "<span class=\"subtitle\">");
		venture_html_escape_append(content, description);
		g_string_append(content, "</span>");
	}

	g_string_append(content, "</div><div class=\"page-actions\">"
	                         "<a class=\"btn\" href=\"/dashboards\">All "
	                         "dashboards</a> ");

	if (venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, NULL))
	{
		g_string_append_printf(content,
			"<a class=\"btn\" href=\"/dashboards/%s/export\">Export</a> "
			"<a class=\"btn btn-primary\" href=\"/dashboards/%s/edit\">Edit"
			"</a>", slug, slug);
	}

	g_string_append(content, "</div></div>");

	venture_web_append_dashboard_grid(self, request, principal, dashboard,
	                                  content, FALSE);

	active = g_strdup_printf("/dashboards/%s", slug);

	return venture_web_html_response(
		venture_web_page(self, request, active, name, content->str), 200);
}

/*
 * GET /dashboards/:slug/widgets/:id - one card, for a refresh.
 *
 * A fragment, so an anonymous request gets a 401 rather than a redirect:
 * HTMX would otherwise swap the login page into the card.
 */
static HtmxResponse *
venture_web_ui_dashboard_widget(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureEntity) widget = NULL;
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GArray) tree = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	VentureWidgetScope scope;
	HtmxResponse *gate;
	gint64 dashboard_id = 0;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_VIEWER, TRUE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	widget = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_DASHBOARD_WIDGET,
	                              g_ascii_strtoll(g_hash_table_lookup(params,
	                                                                  "id"),
	                                              NULL, 10),
	                              &error);

	if (NULL == widget)
		return venture_web_error_response(error);

	g_object_get(widget, "dashboard-id", &dashboard_id, NULL);

	if (dashboard_id != venture_entity_get_id(VENTURE_ENTITY(dashboard)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "No such widget on this dashboard");
		return venture_web_error_response(error);
	}

	g_object_get(dashboard, "slug", &slug, NULL);
	venture_web_dashboard_scope(self, request, principal, dashboard, &scope,
	                            &tree);
	result = venture_dashboard_render_widget(self->context,
		VENTURE_DASHBOARD_WIDGET(widget), &scope);

	content = g_string_new(NULL);

	/* The card carries its place, so it is refreshed where it sits. */
	{
		g_autoptr(GPtrArray) placements = NULL;
		const VentureWidgetPlacement *placement = NULL;
		guint i;

		placements = venture_dashboard_layout(
			venture_context_get_database(self->context), dashboard, NULL);

		for (i = 0; (NULL != placements) && (i < placements->len); i++)
		{
			const VentureWidgetPlacement *candidate;

			candidate = g_ptr_array_index(placements, i);

			if (candidate->widget == VENTURE_DASHBOARD_WIDGET(widget) ||
			    (venture_entity_get_id(VENTURE_ENTITY(candidate->widget)) ==
			     venture_entity_get_id(widget)))
				placement = candidate;
		}

		if (NULL == placement)
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			                    "No such widget on this dashboard");
			return venture_web_error_response(error);
		}

		venture_web_append_widget_card(self, content, slug, placement, result,
		                               FALSE);
	}

	return venture_web_html_response(g_string_free(g_steal_pointer(&content),
	                                               FALSE), 200);
}

/*
 * GET /dashboards/:slug/edit - the page with its controls out, and the
 * dashboard's own settings.
 */
static HtmxResponse *
venture_web_ui_dashboard_edit(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GString) content = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *active = NULL;
	HtmxResponse *gate;
	guint i;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_EDITOR, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	g_object_get(dashboard, "name", &name, "slug", &slug, NULL);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>");
	venture_html_escape_append(content, name);
	g_string_append_printf(content,
		"</h1><span class=\"subtitle\">Editing</span></div>"
		"<div class=\"page-actions\">"
		"<a class=\"btn btn-primary\" href=\"/dashboards/%s/widgets/new\">"
		"Add a widget</a> "
		"<form method=\"post\" action=\"/dashboards/%s/arrange\" "
		"class=\"inline\"><button class=\"btn\" type=\"submit\" "
		"title=\"Close the gaps: every widget to the first free cell, in "
		"page order\">Tidy</button></form> "
		"<a class=\"btn\" href=\"/dashboards/%s\">Done</a></div></div>"
		"<p class=\"muted small\">Drag a card anywhere on the grid, or its "
		"corner to resize it. Drop it on a card the same size and the two "
		"trade places; anywhere a card will not fit is shown in red and "
		"nothing moves. Escape cancels a drag. The arrows on each card do "
		"the same from the keyboard.</p>",
		slug, slug, slug);

	venture_web_append_dashboard_grid(self, request, principal, dashboard,
	                                  content, TRUE);

	/* The dashboard's own settings, from its field table. */
	specs = venture_entity_get_field_specs(VENTURE_ENTITY(dashboard));
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	g_string_append_printf(content,
		"<h2 class=\"section-title\">Settings</h2>"
		"<form method=\"post\" action=\"/dashboards/%s\">"
		"<div class=\"card\"><div class=\"card-body\"><div class=\"form-grid\">",
		slug);

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		/* Who owns it is set when it is made, not edited. */
		if (0 == g_strcmp0(venture_field_spec_get_name(spec), "owner-user-id"))
			continue;

		venture_web_append_form_field(self, content, spec,
		                              VENTURE_ENTITY(dashboard));
	}

	g_string_append_printf(content,
		"</div></div></div><div class=\"form-actions\">"
		"<button class=\"btn btn-primary\" type=\"submit\">Save settings"
		"</button></div></form>"
		"<form method=\"post\" action=\"/dashboards/%s/delete\" "
		"class=\"danger-zone\">"
		"<button class=\"btn btn-danger\" type=\"submit\">Delete dashboard"
		"</button><span class=\"muted small\">Recoverable: the dashboard "
		"and its widgets are hidden and kept.</span></form>", slug);

	active = g_strdup_printf("/dashboards/%s", slug);

	return venture_web_html_response(
		venture_web_page(self, request, active, name, content->str), 200);
}

/*
 * POST /dashboards/:slug - save the settings.
 */
static HtmxResponse *
venture_web_ui_dashboard_update(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *destination = NULL;
	VentureActor actor;
	HtmxResponse *gate;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_EDITOR, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	specs = venture_entity_get_field_specs(VENTURE_ENTITY(dashboard));

	if (!venture_web_apply_form(self, request, VENTURE_ENTITY(dashboard), specs,
	                            &error))
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(dashboard), &actor, &error))
		return venture_web_error_response(error);

	/* The slug may have changed; follow it. */
	g_object_get(dashboard, "slug", &slug, NULL);
	destination = g_strdup_printf("/dashboards/%s", slug);

	return venture_web_redirect_to(destination);
}

/*
 * POST /dashboards/:slug/delete
 */
static HtmxResponse *
venture_web_ui_dashboard_delete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(GPtrArray) widgets = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	guint i;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_EDITOR, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	venture_auth_to_actor(principal, &actor);

	/* The widgets go with it. Soft, like every deletion, so restoring
	 * the dashboard is a matter of restoring the rows. */
	widgets = venture_dashboard_list_widgets(
		venture_context_get_database(self->context),
		venture_entity_get_id(VENTURE_ENTITY(dashboard)), NULL);

	for (i = 0; (NULL != widgets) && (i < widgets->len); i++)
	{
		if (!venture_database_delete(venture_context_get_database(self->context),
		                             g_ptr_array_index(widgets, i), &actor,
		                             &error))
			return venture_web_error_response(error);
	}

	if (!venture_database_delete(venture_context_get_database(self->context),
	                             VENTURE_ENTITY(dashboard), &actor, &error))
		return venture_web_error_response(error);

	return venture_web_redirect_to("/dashboards");
}

/*
 * GET /dashboards/:slug/export - the definition, as a file.
 */
static HtmxResponse *
venture_web_ui_dashboard_export(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *disposition = NULL;
	HtmxResponse *gate;
	HtmxResponse *response;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_VIEWER, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	node = venture_dashboard_export(venture_context_get_database(self->context),
	                                dashboard, &error);

	if (NULL == node)
		return venture_web_error_response(error);

	g_object_get(dashboard, "slug", &slug, NULL);
	response = venture_web_json_response(node, 200);
	disposition = g_strdup_printf("attachment; filename=\"dashboard-%s.json\"",
	                              slug);
	htmx_response_add_header(response, "Content-Disposition", disposition);

	return response;
}

/*
 * The widget form, shared by new and edit. The kind is a select built
 * from the registry, each option carrying the fields its kind reads, so
 * the page can show only those; without scripting every field is shown
 * with its help text, which still works.
 */
static void
venture_web_append_widget_form(
	VentureWebServer	*self,
	GString			*content,
	VentureDashboard	*dashboard,
	VentureDashboardWidget	*widget
){
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(JsonNode) kinds = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *current_kind = NULL;
	JsonArray *array;
	gint64 id;
	guint i;

	g_object_get(dashboard, "slug", &slug, NULL);
	g_object_get(widget, "kind", &current_kind, NULL);
	id = venture_entity_get_id(VENTURE_ENTITY(widget));

	if (0 != id)
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/dashboards/%s/widgets/%"
			G_GINT64_FORMAT "\" data-widget-editor>", slug, id);
	else
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/dashboards/%s/widgets\" "
			"data-widget-editor>", slug);

	g_string_append(content, "<div class=\"card\"><div class=\"card-body\">"
	                         "<div class=\"form-grid\">");

	/* The kind first, because it decides what else is shown. */
	kinds = venture_widget_kind_registry_describe(
		venture_widget_kind_registry_get_default(), self->context);
	array = json_node_get_array(kinds);

	g_string_append(content,
		"<div class=\"field\" data-widget-field=\"kind\"><label>"
		"<span class=\"field-label\">Kind<span class=\"required\">*</span>"
		"</span><select name=\"kind\" data-widget-kind required>");

	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *kind;
		JsonArray *uses;
		g_autoptr(GString) uses_text = NULL;
		const gchar *name;
		guint u;

		kind = json_array_get_object_element(array, i);
		name = json_object_get_string_member(kind, "name");
		uses = json_object_get_array_member(kind, "uses");
		uses_text = g_string_new(NULL);

		for (u = 0; u < json_array_get_length(uses); u++)
		{
			if (u > 0)
				g_string_append_c(uses_text, ' ');

			g_string_append(uses_text, json_array_get_string_element(uses, u));
		}

		g_string_append_printf(content,
			"<option value=\"%s\" data-uses=\"%s\"%s%s>", name, uses_text->str,
			(0 == g_strcmp0(name, current_kind)) ? " selected" : "",
			json_object_get_boolean_member(kind, "enabled") ? ""
			                                                : " disabled");
		venture_html_escape_append(content,
			json_object_get_string_member(kind, "label"));

		if (!json_object_get_boolean_member(kind, "enabled"))
			g_string_append(content, " (module off)");

		g_string_append(content, "</option>");
	}

	g_string_append(content, "</select></label>"
	                         "<div class=\"hint\" data-widget-kind-help>"
	                         "What the widget shows.</div></div>");

	specs = venture_entity_get_field_specs(VENTURE_ENTITY(widget));
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;
		g_autofree gchar *wire = NULL;
		const gchar *name;

		spec = g_ptr_array_index(specs, i);
		name = venture_field_spec_get_name(spec);

		if ((0 == g_strcmp0(name, "kind")) ||
		    (0 == g_strcmp0(name, "dashboard-id")))
			continue;

		wire = venture_entity_property_to_column(name);
		g_string_append_printf(content, "<div data-widget-field=\"%s\">",
		                       wire);
		venture_web_append_form_field(self, content, spec,
		                              VENTURE_ENTITY(widget));
		g_string_append(content, "</div>");
	}

	g_string_append_printf(content,
		"</div></div></div><div class=\"form-actions\">"
		"<button class=\"btn btn-primary\" type=\"submit\">Save widget</button>"
		" <a class=\"btn\" href=\"/dashboards/%s/edit\">Cancel</a>"
		"</div></form>", slug);

	if (0 != id)
	{
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/dashboards/%s/widgets/%"
			G_GINT64_FORMAT "/delete\" class=\"danger-zone\">"
			"<button class=\"btn btn-danger\" type=\"submit\">Remove widget"
			"</button></form>", slug, id);
	}

	/* The catalogue, for reading while choosing. */
	g_string_append(content, "<h2 class=\"section-title\">The kinds</h2>"
	                         "<div class=\"table-wrap\"><table class=\"data\">"
	                         "<thead><tr><th>Kind</th><th>Shows</th>"
	                         "<th>Reads</th></tr></thead><tbody>");

	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *kind;
		JsonArray *uses;
		guint u;

		kind = json_array_get_object_element(array, i);
		uses = json_object_get_array_member(kind, "uses");

		g_string_append(content, "<tr><td><strong>");
		venture_html_escape_append(content,
			json_object_get_string_member(kind, "label"));
		g_string_append(content, "</strong><br><code>");
		venture_html_escape_append(content,
			json_object_get_string_member(kind, "name"));
		g_string_append(content, "</code></td><td>");
		venture_html_escape_append(content,
			json_object_get_string_member(kind, "description"));
		g_string_append(content, "</td><td>");

		for (u = 0; u < json_array_get_length(uses); u++)
		{
			g_string_append(content, (u > 0) ? ", <code>" : "<code>");
			venture_html_escape_append(content,
				json_array_get_string_element(uses, u));
			g_string_append(content, "</code>");
		}

		g_string_append(content, "</td></tr>");
	}

	g_string_append(content, "</tbody></table></div>"
		"<p class=\"muted small\">A filter is written as on a list page: "
		"<code>status=open&amp;priority__in=high,urgent</code>. "
		"<code>{me}</code> is your username, <code>{user_id}</code> your id. "
		"Periods: this_month, last_month, last_30_days, ytd, this_year, "
		"all_time, 2026-Q2. Options are JSON, e.g. <code>{\"days\": 30}</code> "
		"for Upcoming or <code>{\"table\": false}</code> for Report.</p>");
}

/*
 * Loads a widget that belongs to the dashboard in the path.
 */
static VentureDashboardWidget *
venture_web_dashboard_widget_load(
	VentureWebServer	 *self,
	GHashTable		 *params,
	VentureDashboard	 *dashboard,
	GError			**error
){
	g_autoptr(VentureEntity) widget = NULL;
	const gchar *id_text;
	gint64 dashboard_id = 0;

	id_text = g_hash_table_lookup(params, "id");
	widget = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_DASHBOARD_WIDGET,
	                              (NULL != id_text)
	                                      ? g_ascii_strtoll(id_text, NULL, 10)
	                                      : 0,
	                              error);

	if (NULL == widget)
		return NULL;

	g_object_get(widget, "dashboard-id", &dashboard_id, NULL);

	if ((dashboard_id != venture_entity_get_id(VENTURE_ENTITY(dashboard))) ||
	    venture_entity_is_deleted(widget))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "No such widget on this dashboard");
		return NULL;
	}

	return VENTURE_DASHBOARD_WIDGET(g_steal_pointer(&widget));
}

/*
 * GET /dashboards/:slug/widgets/new and /dashboards/:slug/widgets/:id/edit
 */
static HtmxResponse *
venture_web_ui_dashboard_widget_form(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) widget = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *active = NULL;
	HtmxResponse *gate;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_EDITOR, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	if (NULL != g_hash_table_lookup(params, "id"))
	{
		widget = venture_web_dashboard_widget_load(self, params, dashboard,
		                                           &error);

		if (NULL == widget)
			return venture_web_error_response(error);
	}
	else
	{
		const gchar *kind;

		widget = venture_dashboard_widget_new();
		kind = htmx_request_get_query_param(request, "kind");

		if (!venture_string_is_empty(kind))
			g_object_set(widget, "kind", kind, NULL);
	}

	g_object_get(dashboard, "name", &name, "slug", &slug, NULL);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>");
	g_string_append(content, (NULL != g_hash_table_lookup(params, "id"))
	                         ? "Edit widget" : "Add a widget");
	g_string_append(content, "</h1><span class=\"subtitle\">on ");
	venture_html_escape_append(content, name);
	g_string_append(content, "</span></div></div>");

	venture_web_append_widget_form(self, content, dashboard, widget);

	active = g_strdup_printf("/dashboards/%s", slug);

	return venture_web_html_response(
		venture_web_page(self, request, active, "Widget", content->str), 200);
}

/*
 * POST /dashboards/:slug/widgets and /dashboards/:slug/widgets/:id
 */
static HtmxResponse *
venture_web_ui_dashboard_widget_save(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) widget = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *destination = NULL;
	VentureActor actor;
	HtmxResponse *gate;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_EDITOR, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	if (NULL != g_hash_table_lookup(params, "id"))
	{
		widget = venture_web_dashboard_widget_load(self, params, dashboard,
		                                           &error);

		if (NULL == widget)
			return venture_web_error_response(error);
	}
	else
	{
		widget = venture_dashboard_widget_new();
	}

	specs = venture_entity_get_field_specs(VENTURE_ENTITY(widget));

	if (!venture_web_apply_form(self, request, VENTURE_ENTITY(widget), specs,
	                            &error))
		return venture_web_error_response(error);

	/* The dashboard is the one in the path, whatever the form carried. */
	g_object_set(widget, "dashboard-id",
	             venture_entity_get_id(VENTURE_ENTITY(dashboard)), NULL);

	/* A new widget goes last. */
	if (!venture_entity_is_persisted(VENTURE_ENTITY(widget)))
	{
		gint64 position = 0;

		g_object_get(widget, "position", &position, NULL);

		if (0 == position)
		{
			g_autoptr(GPtrArray) existing = NULL;

			existing = venture_dashboard_list_widgets(
				venture_context_get_database(self->context),
				venture_entity_get_id(VENTURE_ENTITY(dashboard)), NULL);
			g_object_set(widget, "position",
			             (gint64)(((NULL != existing) ? existing->len : 0) + 1)
			                 * 10,
			             NULL);
		}
	}

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(widget), &actor, &error))
		return venture_web_error_response(error);

	g_object_get(dashboard, "slug", &slug, NULL);
	destination = g_strdup_printf("/dashboards/%s/edit", slug);

	return venture_web_redirect_to(destination);
}

/*
 * POST /dashboards/:slug/widgets/:id/delete
 */
static HtmxResponse *
venture_web_ui_dashboard_widget_delete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) widget = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *destination = NULL;
	VentureActor actor;
	HtmxResponse *gate;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_EDITOR, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	widget = venture_web_dashboard_widget_load(self, params, dashboard, &error);

	if (NULL == widget)
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_delete(venture_context_get_database(self->context),
	                             VENTURE_ENTITY(widget), &actor, &error))
		return venture_web_error_response(error);

	g_object_get(dashboard, "slug", &slug, NULL);
	destination = g_strdup_printf("/dashboards/%s/edit", slug);

	return venture_web_redirect_to(destination);
}

/*
 * Answers a grid change: a redirect back to the editor for a form, a JSON
 * outcome for the drag, which sent async=1 and reloads on its own. A
 * refusal is JSON either way so the message reaches the person.
 */
static HtmxResponse *
venture_web_dashboard_grid_answer(
	HtmxRequest	*request,
	const gchar	*slug,
	GError		*error
){
	g_autofree gchar *destination = NULL;

	if (NULL != error)
		return venture_web_error_response(error);

	if (NULL != htmx_request_get_form_value(request, "async"))
	{
		g_autoptr(JsonBuilder) builder = NULL;
		g_autoptr(JsonNode) node = NULL;

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "placed");
		json_builder_add_boolean_value(builder, TRUE);
		json_builder_end_object(builder);
		node = json_builder_get_root(builder);

		return venture_web_json_response(node, 200);
	}

	destination = g_strdup_printf("/dashboards/%s/edit", slug);

	return venture_web_redirect_to(destination);
}

/*
 * POST /dashboards/:slug/widgets/:id/place - col, row, width, height.
 * The drag's destination; also what an API client with a form posts.
 */
static HtmxResponse *
venture_web_ui_dashboard_widget_place(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) widget = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *text;
	guint col;
	guint row;
	guint width;
	guint height;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_EDITOR, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	widget = venture_web_dashboard_widget_load(self, params, dashboard, &error);

	if (NULL == widget)
		return venture_web_error_response(error);

	g_object_get(dashboard, "slug", &slug, NULL);

	/* Width and height default to what the widget has, so a drag that
	 * only moves sends only col and row. */
	{
		g_autoptr(GPtrArray) placements = NULL;
		guint i;

		width = 1;
		height = 1;
		placements = venture_dashboard_layout(
			venture_context_get_database(self->context), dashboard, NULL);

		for (i = 0; (NULL != placements) && (i < placements->len); i++)
		{
			const VentureWidgetPlacement *placement;

			placement = g_ptr_array_index(placements, i);

			if (venture_entity_get_id(VENTURE_ENTITY(placement->widget)) ==
			    venture_entity_get_id(VENTURE_ENTITY(widget)))
			{
				width = placement->width;
				height = placement->height;
			}
		}
	}

	text = htmx_request_get_form_value(request, "col");
	col = (NULL != text) ? (guint)g_ascii_strtoull(text, NULL, 10) : 0;
	text = htmx_request_get_form_value(request, "row");
	row = (NULL != text) ? (guint)g_ascii_strtoull(text, NULL, 10) : 0;
	text = htmx_request_get_form_value(request, "width");

	if (!venture_string_is_empty(text))
		width = (guint)g_ascii_strtoull(text, NULL, 10);

	text = htmx_request_get_form_value(request, "height");

	if (!venture_string_is_empty(text))
		height = (guint)g_ascii_strtoull(text, NULL, 10);

	venture_auth_to_actor(principal, &actor);
	venture_dashboard_place_widget(venture_context_get_database(self->context),
	                               dashboard, widget, col, row, width, height,
	                               &actor, &error);

	return venture_web_dashboard_grid_answer(request, slug, error);
}

/*
 * POST /dashboards/:slug/widgets/:id/swap - two cards trade places.
 *
 * The gesture is dragging one card onto another, which on a full grid is
 * the only way to rearrange anything: a placement refuses a taken cell,
 * and a tidy page has no spare cell to move through.
 */
static HtmxResponse *
venture_web_ui_dashboard_widget_swap(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autofree gchar *slug = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *with;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_EDITOR, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	g_object_get(dashboard, "slug", &slug, NULL);

	first = venture_database_get(venture_context_get_database(self->context),
	                             VENTURE_TYPE_DASHBOARD_WIDGET,
	                             g_ascii_strtoll(g_hash_table_lookup(params,
	                                                                 "id"),
	                                             NULL, 10),
	                             &error);

	if (NULL == first)
		return venture_web_error_response(error);

	with = htmx_request_get_form_value(request, "with");
	second = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_DASHBOARD_WIDGET,
	                              (NULL != with)
	                              	? g_ascii_strtoll(with, NULL, 10) : 0,
	                              &error);

	if (NULL == second)
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);
	venture_dashboard_swap_widgets(venture_context_get_database(self->context),
	                               dashboard,
	                               VENTURE_DASHBOARD_WIDGET(first),
	                               VENTURE_DASHBOARD_WIDGET(second),
	                               &actor, &error);

	return venture_web_dashboard_grid_answer(request, slug, error);
}

/*
 * POST /dashboards/:slug/arrange - close the gaps.
 */
static HtmxResponse *
venture_web_ui_dashboard_arrange(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	VentureActor actor;
	HtmxResponse *gate;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_EDITOR, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	g_object_get(dashboard, "slug", &slug, NULL);
	venture_auth_to_actor(principal, &actor);
	venture_dashboard_arrange(venture_context_get_database(self->context),
	                          dashboard, &actor, &error);

	return venture_web_dashboard_grid_answer(request, slug, error);
}

/*
 * POST /dashboards/:slug/widgets/:id/move - direction=up|down|left|right|
 * wider|narrower|taller|shorter, one cell each.
 */
static HtmxResponse *
venture_web_ui_dashboard_widget_move(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) widget = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *direction;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_EDITOR, FALSE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	widget = venture_web_dashboard_widget_load(self, params, dashboard, &error);

	if (NULL == widget)
		return venture_web_error_response(error);

	direction = htmx_request_get_form_value(request, "direction");
	venture_auth_to_actor(principal, &actor);
	g_object_get(dashboard, "slug", &slug, NULL);

	venture_dashboard_nudge_widget(venture_context_get_database(self->context),
	                               dashboard, widget, direction, &actor,
	                               &error);

	return venture_web_dashboard_grid_answer(request, slug, error);
}

/* --- The dashboards API --------------------------------------------------- */

/*
 * The scope an API caller renders under: the token's user, and either
 * everything or the entity named in ?organization_id=.
 */
static void
venture_web_api_dashboard_scope(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	VentureAuthPrincipal	 *principal,
	VentureDashboard	 *dashboard,
	VentureWidgetScope	 *scope,
	GArray			**tree
){
	const gchar *organization;

	scope->organization_ids = NULL;
	scope->n_organizations = 0;
	scope->user_id = principal->user_id;
	scope->username = principal->name;
	scope->venture_id = 0;
	g_object_get(dashboard, "venture-id", &scope->venture_id, NULL);
	*tree = NULL;

	organization = htmx_request_get_query_param(request, "organization_id");

	if (!venture_string_is_empty(organization))
	{
		*tree = venture_web_organization_tree(self,
			g_ascii_strtoll(organization, NULL, 10));
		scope->organization_ids = (const gint64 *)(*tree)->data;
		scope->n_organizations = (*tree)->len;
	}
}

/*
 * GET /api/v1/dashboards - every dashboard the caller may see.
 */
static HtmxResponse *
venture_web_api_dashboards(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) dashboards = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	guint i;

	(void)params;

	gate = venture_web_require_module_api(self, "dashboards");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	principal = venture_auth_authenticate(self->auth, request);
	dashboards = venture_dashboard_list_visible(
		venture_context_get_database(self->context), principal->user_id,
		&error);

	if (NULL == dashboards)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < dashboards->len; i++)
	{
		g_autoptr(JsonNode) described = NULL;

		described = venture_dashboard_describe(self->context,
			g_ptr_array_index(dashboards, i), NULL, FALSE, &error);

		if (NULL == described)
			return venture_web_error_response(error);

		json_builder_add_value(builder, g_steal_pointer(&described));
	}

	json_builder_end_array(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * GET /api/v1/dashboards/:slug - the dashboard with every widget's answer.
 * ?data=0 lists the widgets without evaluating them.
 */
static HtmxResponse *
venture_web_api_dashboard(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GArray) tree = NULL;
	g_autoptr(GError) error = NULL;
	VentureWidgetScope scope;
	HtmxResponse *gate;
	const gchar *data;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_VIEWER, TRUE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	venture_web_api_dashboard_scope(self, request, principal, dashboard,
	                                &scope, &tree);
	data = htmx_request_get_query_param(request, "data");
	node = venture_dashboard_describe(self->context, dashboard, &scope,
		(NULL == data) || (0 != g_strcmp0(data, "0")), &error);

	if (NULL == node)
		return venture_web_error_response(error);

	return venture_web_json_response(node, 200);
}

/*
 * GET /api/v1/dashboards/:slug/export
 */
static HtmxResponse *
venture_web_api_dashboard_export(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = venture_web_dashboard_load(self, request, params,
	                                  VENTURE_USER_ROLE_VIEWER, TRUE,
	                                  &principal, &dashboard);

	if (NULL != gate)
		return gate;

	node = venture_dashboard_export(venture_context_get_database(self->context),
	                                dashboard, &error);

	if (NULL == node)
		return venture_web_error_response(error);

	return venture_web_json_response(node, 200);
}

/*
 * POST /api/v1/dashboards/import - a definition in the body.
 * POST /api/v1/dashboards/from-template - {"template": "factory"}.
 */
static HtmxResponse *
venture_web_api_dashboard_import(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	gboolean from_template;

	gate = venture_web_require_module_api(self, "dashboards");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != gate)
		return gate;

	principal = venture_auth_authenticate(self->auth, request);
	body = htmx_request_get_json(request, NULL);

	if ((NULL == body) || !JSON_NODE_HOLDS_OBJECT(body))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "The request body must be a JSON object");
		return venture_web_error_response(error);
	}

	venture_auth_to_actor(principal, &actor);
	from_template = (0 == g_strcmp0(g_hash_table_lookup(params, "action"),
	                                "from-template"));

	if (!from_template &&
	    (0 != g_strcmp0(g_hash_table_lookup(params, "action"), "import")))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "POST /api/v1/dashboards/import or "
		                    "/api/v1/dashboards/from-template");
		return venture_web_error_response(error);
	}

	if (from_template)
	{
		dashboard = venture_dashboard_create_from_template(self->context,
			venture_json_object_get_string(json_node_get_object(body),
			                               "template", NULL),
			principal->user_id, &actor, &error);
	}
	else
	{
		dashboard = venture_dashboard_import(self->context, body,
		                                     principal->user_id, &actor,
		                                     &error);
	}

	if (NULL == dashboard)
		return venture_web_error_response(error);

	node = venture_dashboard_describe(self->context, dashboard, NULL, FALSE,
	                                  &error);

	if (NULL == node)
		return venture_web_error_response(error);

	return venture_web_json_response(node, 201);
}

/*
 * GET /api/v1/widget-kinds - the catalogue.
 */
static HtmxResponse *
venture_web_api_widget_kinds(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *gate;

	(void)params;

	gate = venture_web_require_module_api(self, "dashboards");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	node = venture_widget_kind_registry_describe(
		venture_widget_kind_registry_get_default(), self->context);

	return venture_web_json_response(node, 200);
}

/*
 * GET /api/v1/dashboard-templates - what can be made in one call.
 */
static HtmxResponse *
venture_web_api_dashboard_templates(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	const VentureDashboardTemplate *templates;
	HtmxResponse *gate;
	gsize n_templates;
	gsize i;

	(void)params;

	gate = venture_web_require_module_api(self, "dashboards");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	templates = venture_dashboard_get_templates(&n_templates);
	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < n_templates; i++)
	{
		g_autoptr(JsonNode) definition = NULL;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, templates[i].name);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, templates[i].label);
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder, templates[i].description);
		json_builder_set_member_name(builder, "module");

		if (NULL != templates[i].module)
			json_builder_add_string_value(builder, templates[i].module);
		else
			json_builder_add_null_value(builder);

		json_builder_set_member_name(builder, "definition");
		definition = venture_json_parse(templates[i].definition, NULL);

		if (NULL != definition)
			json_builder_add_value(builder, g_steal_pointer(&definition));
		else
			json_builder_add_null_value(builder);

		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/* ==========================================================================
 * The workdesk: inbox, watches, saved views, macros, worklogs, sprints,
 * bulk edits, the timeline, and mission control for the runs.
 *
 * Every page here is a thin rendering over core/venture-notify.c,
 * core/venture-sla.c, core/venture-desk.c and core/venture-factory.c;
 * the API routes beside each page answer the same call as JSON.
 * ========================================================================== */

/*
 * Where to send a browser back to after a small action: the form's own
 * "back" field when it names a path on this site, else a fallback. A
 * path that could leave the site -- anything not starting with a single
 * slash -- is ignored, because a redirect target read from a form is a
 * redirect target anybody can write.
 */
static gchar *
venture_web_desk_back(
	HtmxRequest	*request,
	const gchar	*fallback
){
	const gchar *back;

	back = htmx_request_get_form_value(request, "back");

	if (!venture_string_is_empty(back) && ('/' == back[0]) &&
	    ('/' != back[1]) && ('\\' != back[1]))
		return g_strdup(back);

	return g_strdup(fallback);
}

/*
 * A small action's answer: JSON to a scripted caller (async=1), and to a
 * browser a redirect back, or the error page when it failed.
 */
static HtmxResponse *
venture_web_desk_answer(
	HtmxRequest	*request,
	const gchar	*back,
	const GError	*error
){
	if (NULL != htmx_request_get_form_value(request, "async"))
	{
		g_autoptr(JsonBuilder) builder = NULL;
		g_autoptr(JsonNode) node = NULL;

		if (NULL != error)
			return venture_web_error_response(error);

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "ok");
		json_builder_add_boolean_value(builder, TRUE);
		json_builder_end_object(builder);
		node = json_builder_get_root(builder);

		return venture_web_json_response(node, 200);
	}

	if (NULL != error)
		return venture_web_error_response(error);

	return venture_web_redirect_to(back);
}

/*
 * "1,2,3" or a repeated form field into ids. Blank entries are skipped;
 * anything that is not a number is refused, because a bulk edit that
 * quietly changed the wrong rows is the worst thing a bulk edit can do.
 */
static GArray *
venture_web_desk_parse_ids(
	const gchar	 *text,
	GError		**error
){
	g_autoptr(GArray) ids = NULL;
	g_auto(GStrv) parts = NULL;
	gsize i;

	ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	parts = g_strsplit_set((NULL != text) ? text : "", ", \n", -1);

	for (i = 0; NULL != parts[i]; i++)
	{
		gchar *end;
		gint64 id;

		if ('\0' == parts[i][0])
			continue;

		id = g_ascii_strtoll(parts[i], &end, 10);

		if ((id <= 0) || ('\0' != *end))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not a record id", parts[i]);
			return NULL;
		}

		g_array_append_val(ids, id);
	}

	return g_steal_pointer(&ids);
}


/*
 * A JSON scalar as text, for a diff's from and to. The shared coercion
 * is private to the JSON utilities.
 */
static gchar *
venture_web_node_text(JsonNode *node)
{
	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
		return NULL;

	switch (json_node_get_value_type(node))
	{
	case G_TYPE_STRING:
		return g_strdup(json_node_get_string(node));
	case G_TYPE_INT64:
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
	case G_TYPE_DOUBLE:
		return g_strdup_printf("%g", json_node_get_double(node));
	case G_TYPE_BOOLEAN:
		return g_strdup(json_node_get_boolean(node) ? "yes" : "no");
	default:
		return NULL;
	}
}

/* --- The sidebar ----------------------------------------------------------- */

/*
 * The inbox entry, with the unread count. Rendered for a signed-in user
 * only; the count is what makes the sidebar worth looking at.
 */
static void
venture_web_append_inbox_nav(
	VentureWebServer	*self,
	HtmxRequest		*request,
	GString			*html,
	const gchar		*active
){
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	gint64 unread;

	principal = venture_auth_authenticate(self->auth, request);

	if (!principal->authenticated || (0 == principal->user_id))
		return;

	unread = venture_notify_unread_count(self->context, principal->user_id);

	g_string_append_printf(html,
		"<a class=\"nav-item nav-inbox%s\" href=\"/inbox\" data-inbox-link>"
		"<span class=\"icon\">" VENTURE_ICON(
			"<path d=\"M22 12h-6l-2 3h-4l-2-3H2\"/>"
			"<path d=\"M5.5 5h13l3.5 7v6a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2v-6l3.5-7z\"/>")
		"</span>Inbox"
		"<span class=\"nav-count%s\" data-inbox-count>%" G_GINT64_FORMAT
		"</span></a>",
		(0 == g_strcmp0(active, "/inbox")) ? " active" : "",
		(unread > 0) ? "" : " empty", unread);
}

/*
 * The address a saved view opens at.
 */
static gchar *
venture_web_saved_view_url(VentureEntity *view)
{
	g_autofree gchar *entity_type = NULL;
	g_autofree gchar *query = NULL;
	gboolean board = FALSE;

	g_object_get(view, "entity-type", &entity_type, "query", &query,
	             "board", &board, NULL);

	if (board && (0 == g_strcmp0(entity_type, "ticket")))
		return g_strdup_printf("/tickets%s%s",
		                       venture_string_is_empty(query) ? "" : "?",
		                       venture_string_is_empty(query) ? "" : query);

	return g_strdup_printf("/e/%s%s%s", entity_type,
	                       venture_string_is_empty(query) ? "" : "?",
	                       venture_string_is_empty(query) ? "" : query);
}

/*
 * The saved views a user may see: everybody's shared ones and their own
 * personal ones, in position order.
 */
static GPtrArray *
venture_web_saved_views_for(
	VentureWebServer	*self,
	VentureAuthPrincipal	*principal,
	gboolean		 pinned_only
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) all = NULL;
	GPtrArray *visible;
	guint i;

	query = venture_query_new(VENTURE_TYPE_SAVED_VIEW);

	if (pinned_only)
		venture_query_add_filter_string(query, "pinned", VENTURE_FILTER_OP_EQ,
		                                "true", NULL);

	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	all = venture_database_find(venture_context_get_database(self->context),
	                            query, NULL);
	visible = g_ptr_array_new_with_free_func(g_object_unref);

	for (i = 0; (NULL != all) && (i < all->len); i++)
	{
		VentureEntity *view;
		gboolean personal = FALSE;
		gint64 owner = 0;

		view = g_ptr_array_index(all, i);
		g_object_get(view, "personal", &personal, "owner-user-id", &owner,
		             NULL);

		if (personal && (owner != principal->user_id))
			continue;

		g_ptr_array_add(visible, g_object_ref(view));
	}

	return visible;
}

static void
venture_web_append_saved_view_nav(
	VentureWebServer	*self,
	HtmxRequest		*request,
	GString			*html,
	const gchar		*active
){
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) views = NULL;
	guint i;

	principal = venture_auth_authenticate(self->auth, request);

	if (!principal->authenticated)
		return;

	views = venture_web_saved_views_for(self, principal, TRUE);

	if (0 == views->len)
		return;

	g_string_append(html, "<div class=\"nav-section\">Saved views</div>");

	for (i = 0; i < views->len; i++)
	{
		VentureEntity *view;
		g_autofree gchar *name = NULL;
		g_autofree gchar *url = NULL;

		view = g_ptr_array_index(views, i);
		g_object_get(view, "name", &name, NULL);
		url = venture_web_saved_view_url(view);

		g_string_append_printf(html, "<a class=\"nav-item%s\" href=\"",
			(0 == g_strcmp0(active, url)) ? " active" : "");
		venture_html_escape_append(html, url);
		g_string_append(html, "\"><span class=\"icon\">" VENTURE_ICON(
			"<path d=\"M19 21l-7-4-7 4V5a2 2 0 0 1 2-2h10a2 2 0 0 1 2 2z\"/>")
			"</span>");
		venture_html_escape_append(html, name);
		g_string_append(html, "</a>");
	}
}

/*
 * The "save this view" control on a list page: a name box that posts the
 * page's own query string. Details rather than a dialog, so it works
 * with scripting off.
 */
static void
venture_web_append_save_view_form(
	GString		*content,
	HtmxRequest	*request,
	const gchar	*entity_type,
	gboolean	 board
){
	const gchar *query;

	query = htmx_request_get_query(request);

	g_string_append(content,
		"<details class=\"save-view\"><summary class=\"btn\" "
		"title=\"Keep this list with its filters and sorting\">"
		"Save view</summary>"
		"<form method=\"post\" action=\"/views\" class=\"save-view-form\">"
		"<input type=\"hidden\" name=\"entity_type\" value=\"");
	venture_html_escape_append(content, entity_type);
	g_string_append(content, "\"><input type=\"hidden\" name=\"query\" value=\"");

	if (!venture_string_is_empty(query))
		venture_html_escape_append(content, query);

	g_string_append_printf(content,
		"\"><input type=\"hidden\" name=\"board\" value=\"%s\">"
		"<input type=\"text\" name=\"name\" placeholder=\"Name this view\" "
		"required autocomplete=\"off\">"
		"<label class=\"checkbox\"><input type=\"checkbox\" name=\"pinned\" "
		"value=\"1\" checked> Pin to sidebar</label>"
		"<label class=\"checkbox\"><input type=\"checkbox\" name=\"personal\" "
		"value=\"1\"> Only me</label>"
		"<button class=\"btn btn-primary btn-sm\" type=\"submit\">Save</button>"
		"</form></details>", board ? "1" : "0");
}

/* --- Saved views ----------------------------------------------------------- */

/*
 * GET /views - every saved view you may see.
 */
static HtmxResponse *
venture_web_ui_views(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) views = NULL;
	g_autoptr(GString) content = NULL;
	HtmxResponse *redirect;
	guint i;

	(void)params;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);
	views = venture_web_saved_views_for(self, principal, FALSE);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Saved views</h1><span class=\"subtitle\">"
	                       "Lists with their filters kept, yours and shared"
	                       "</span></div><div class=\"page-actions\">"
	                       "<a class=\"btn\" href=\"/e/saved_view\">All as "
	                       "records</a></div></div>");

	if (0 == views->len)
	{
		g_string_append(content, "<div class=\"empty\"><h3>No saved views "
		                         "yet</h3><p class=\"muted\">Filter any "
		                         "list, then press Save view on it.</p>"
		                         "</div>");
	}
	else
	{
		g_string_append(content, "<div class=\"card\"><div class=\"table-wrap\">"
		                         "<table class=\"data\"><thead><tr>"
		                         "<th>Name</th><th>Lists</th><th>Filters</th>"
		                         "<th>Who</th><th>Pinned</th>"
		                         "<th class=\"row-actions\"></th></tr></thead>"
		                         "<tbody>");

		for (i = 0; i < views->len; i++)
		{
			VentureEntity *view;
			g_autofree gchar *name = NULL;
			g_autofree gchar *entity_type = NULL;
			g_autofree gchar *query = NULL;
			g_autofree gchar *url = NULL;
			gboolean personal = FALSE;
			gboolean pinned = FALSE;
			gint64 owner = 0;

			view = g_ptr_array_index(views, i);
			g_object_get(view, "name", &name, "entity-type", &entity_type,
			             "query", &query, "personal", &personal,
			             "pinned", &pinned, "owner-user-id", &owner, NULL);
			url = venture_web_saved_view_url(view);

			g_string_append(content, "<tr><td><a href=\"");
			venture_html_escape_append(content, url);
			g_string_append(content, "\">");
			venture_html_escape_append(content, name);
			g_string_append(content, "</a></td><td>");
			venture_html_escape_append(content, entity_type);
			g_string_append(content, "</td><td class=\"muted\"><code>");
			venture_html_escape_append(content,
				venture_string_is_empty(query) ? "everything" : query);
			g_string_append_printf(content, "</code></td><td>%s</td><td>%s</td>",
				personal ? "Only me" : "Everybody",
				pinned ? "yes" : "");
			g_string_append_printf(content,
				"<td class=\"row-actions\">"
				"<a class=\"btn btn-sm\" href=\"/e/saved_view/%"
				G_GINT64_FORMAT "/edit\">Edit</a> "
				"<form method=\"post\" action=\"/views/%" G_GINT64_FORMAT
				"/delete\" class=\"inline\">"
				"<button class=\"btn btn-sm btn-ghost\" type=\"submit\">"
				"Remove</button></form></td></tr>",
				venture_entity_get_id(view), venture_entity_get_id(view));
		}

		g_string_append(content, "</tbody></table></div></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/views", "Saved views", content->str),
		200);
}

/*
 * POST /views - keep the list you are looking at.
 */
static HtmxResponse *
venture_web_ui_view_create(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureSavedView) view = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *name;
	const gchar *entity_type;
	const gchar *query;
	const gchar *board;
	GType type;

	(void)params;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	name = htmx_request_get_form_value(request, "name");
	entity_type = htmx_request_get_form_value(request, "entity_type");
	query = htmx_request_get_form_value(request, "query");
	board = htmx_request_get_form_value(request, "board");

	if (venture_string_is_empty(name) || venture_string_is_empty(entity_type))
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A saved view needs a name and a record type");
		return venture_web_error_response(error);
	}

	type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(self->context), entity_type);

	if (G_TYPE_INVALID == type)
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no record type called \"%s\"", entity_type);
		return venture_web_error_response(error);
	}

	view = venture_saved_view_new();
	g_object_set(view,
	             "name", name,
	             "entity-type", entity_type,
	             "query", (NULL != query) ? query : "",
	             "board", (0 == g_strcmp0(board, "1")),
	             "personal", (NULL != htmx_request_get_form_value(request,
	                                                              "personal")),
	             "pinned", (NULL != htmx_request_get_form_value(request,
	                                                            "pinned")),
	             "owner-user-id", principal->user_id,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(view),
		venture_context_get_default_organization_id(self->context));
	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(view), &actor, &error))
		return venture_web_error_response(error);

	url = venture_web_saved_view_url(VENTURE_ENTITY(view));

	return venture_web_redirect_to(url);
}

/*
 * GET /views/:id - open a saved view.
 */
static HtmxResponse *
venture_web_ui_view_open(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureEntity) view = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	HtmxResponse *redirect;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	view = venture_database_get(venture_context_get_database(self->context),
	                            VENTURE_TYPE_SAVED_VIEW,
	                            g_ascii_strtoll(g_hash_table_lookup(params, "id"),
	                                            NULL, 10),
	                            &error);

	if (NULL == view)
		return venture_web_error_response(error);

	url = venture_web_saved_view_url(view);

	return venture_web_redirect_to(url);
}

/*
 * POST /views/:id/delete - a shared view can be removed by any editor;
 * a personal one only by its owner, or an admin.
 */
static HtmxResponse *
venture_web_ui_view_delete(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) view = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	gboolean personal = FALSE;
	gint64 owner = 0;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	view = venture_database_get(venture_context_get_database(self->context),
	                            VENTURE_TYPE_SAVED_VIEW,
	                            g_ascii_strtoll(g_hash_table_lookup(params, "id"),
	                                            NULL, 10),
	                            &error);

	if (NULL == view)
		return venture_web_error_response(error);

	g_object_get(view, "personal", &personal, "owner-user-id", &owner, NULL);

	if (owner != principal->user_id)
	{
		if (!venture_auth_require(self->auth, principal,
		                          personal ? VENTURE_USER_ROLE_ADMIN
		                                   : VENTURE_USER_ROLE_EDITOR,
		                          &error))
			return venture_web_error_response(error);
	}

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_delete(venture_context_get_database(self->context),
	                             view, &actor, &error))
		return venture_web_error_response(error);

	return venture_web_redirect_to("/views");
}

/* --- The inbox ------------------------------------------------------------- */

static const gchar *
venture_web_notification_tone(VentureNotificationKind kind)
{
	switch (kind)
	{
	case VENTURE_NOTIFICATION_KIND_MENTION:  return "accent";
	case VENTURE_NOTIFICATION_KIND_ASSIGNED: return "info";
	case VENTURE_NOTIFICATION_KIND_SLA:      return "negative";
	case VENTURE_NOTIFICATION_KIND_BUDGET:   return "warning";
	case VENTURE_NOTIFICATION_KIND_RUN:      return "positive";
	case VENTURE_NOTIFICATION_KIND_WATCHED:
	case VENTURE_NOTIFICATION_KIND_SYSTEM:
	default:                                 return "";
	}
}

/*
 * GET /inbox - what you have been told.
 */
static HtmxResponse *
venture_web_ui_inbox(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	const gchar *filter;
	gboolean unread_only;
	gint64 unread;
	guint i;

	(void)params;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	/* Anything that has fallen due since the last look is marked now,
	 * so the inbox never says "nothing" about a breach an hour old. */
	venture_sla_sweep(self->context, 50, NULL);
	activity_lazy_sweep(self, request);

	filter = htmx_request_get_query_param(request, "show");
	unread_only = (0 != g_strcmp0(filter, "all"));
	unread = venture_notify_unread_count(self->context, principal->user_id);
	rows = venture_notify_list(self->context, principal->user_id, unread_only,
	                           100, &error);

	if (NULL == rows)
		return venture_web_error_response(error);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Inbox</h1><span class=\"subtitle\">");
	g_string_append_printf(content, "%" G_GINT64_FORMAT " unread", unread);
	g_string_append(content, "</span></div><div class=\"page-actions\">"
	                         "<div class=\"segmented\">");
	g_string_append_printf(content,
		"<a class=\"seg%s\" href=\"/inbox\">Unread</a>"
		"<a class=\"seg%s\" href=\"/inbox?show=all\">Everything</a></div>",
		unread_only ? " active" : "", unread_only ? "" : " active");

	if (unread > 0)
		g_string_append(content,
			"<form method=\"post\" action=\"/inbox/read\" class=\"inline\">"
			"<input type=\"hidden\" name=\"id\" value=\"0\">"
			"<button class=\"btn\" type=\"submit\">Mark all read</button>"
			"</form>");

	g_string_append(content, "</div></div>");

	if (0 == rows->len)
	{
		g_string_append(content,
			"<div class=\"empty\"><span class=\"empty-icon\">" VENTURE_ICON(
				"<path d=\"M22 12h-6l-2 3h-4l-2-3H2\"/>"
				"<path d=\"M5.5 5h13l3.5 7v6a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2v-6l3.5-7z\"/>")
			"</span><h3>All caught up</h3><p class=\"muted\">Watch a record, "
			"get assigned a ticket, or be mentioned with @your-name in a "
			"comment, and it lands here.</p></div>");
	}
	else
	{
		g_string_append(content, "<div class=\"card\"><ul class=\"inbox\">");

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row;
			g_autofree gchar *title = NULL;
			g_autofree gchar *body = NULL;
			g_autofree gchar *target_type = NULL;
			g_autoptr(GDateTime) read_at = NULL;
			g_autoptr(GDateTime) when = NULL;
			g_autofree gchar *relative = NULL;
			VentureNotificationKind kind;
			gint64 target_id = 0;

			row = g_ptr_array_index(rows, i);
			g_object_get(row, "kind", &kind, "title", &title, "body", &body,
			             "target-type", &target_type, "target-id", &target_id,
			             "read-at", &read_at, "occurred-at", &when, NULL);
			relative = (NULL != when) ? venture_time_to_relative_string(when)
			                          : g_strdup("");

			g_string_append_printf(content,
				"<li class=\"inbox-item%s\" data-notification=\"%"
				G_GINT64_FORMAT "\">"
				"<span class=\"badge %s\">%s</span>"
				"<div class=\"inbox-body\">",
				(NULL == read_at) ? " unread" : "",
				venture_entity_get_id(row),
				venture_web_notification_tone(kind),
				venture_enum_to_nick(VENTURE_TYPE_NOTIFICATION_KIND, (gint)kind));

			if (!venture_string_is_empty(target_type) && (0 != target_id))
			{
				g_string_append_printf(content,
					"<a class=\"inbox-title\" href=\"/e/%s/%" G_GINT64_FORMAT
					"\">", target_type, target_id);
				venture_html_escape_append(content, title);
				g_string_append(content, "</a>");
			}
			else
			{
				g_string_append(content, "<span class=\"inbox-title\">");
				venture_html_escape_append(content, title);
				g_string_append(content, "</span>");
			}

			if (!venture_string_is_empty(body))
			{
				g_string_append(content, "<span class=\"inbox-detail\">");
				venture_html_escape_append(content, body);
				g_string_append(content, "</span>");
			}

			g_string_append(content, "</div><span class=\"inbox-when\">");
			venture_html_escape_append(content, relative);
			g_string_append(content, "</span>");

			if (NULL == read_at)
				g_string_append_printf(content,
					"<form method=\"post\" action=\"/inbox/read\" "
					"class=\"inline\" data-inbox-read>"
					"<input type=\"hidden\" name=\"id\" value=\"%"
					G_GINT64_FORMAT "\">"
					"<button class=\"btn btn-sm btn-ghost\" type=\"submit\" "
					"title=\"Mark read\">Done</button></form>",
					venture_entity_get_id(row));

			g_string_append(content, "</li>");
		}

		g_string_append(content, "</ul></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/inbox", "Inbox", content->str), 200);
}

/*
 * POST /inbox/read - one notification, or every one with id 0.
 */
static HtmxResponse *
venture_web_ui_inbox_read(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *back = NULL;
	HtmxResponse *redirect;
	const gchar *id;

	(void)params;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);
	id = htmx_request_get_form_value(request, "id");
	back = venture_web_desk_back(request, "/inbox");

	venture_notify_mark_read(self->context, principal->user_id,
	                         (NULL != id) ? g_ascii_strtoll(id, NULL, 10) : 0,
	                         &error);

	return venture_web_desk_answer(request, back, error);
}

/*
 * GET /inbox/count - the unread count, for the badge to poll.
 */
static HtmxResponse *
venture_web_ui_inbox_count(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *denied;

	(void)params;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "unread");
	json_builder_add_int_value(builder,
		venture_notify_unread_count(self->context, principal->user_id));
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * GET /api/v1/inbox - the caller's notifications; ?unread=0 for all.
 */
static HtmxResponse *
venture_web_api_inbox(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *denied;
	const gchar *unread;
	const gchar *limit;
	guint i;

	(void)params;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);

	if (0 == principal->user_id)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "A token with no user has no inbox");
		return venture_web_error_response(error);
	}

	venture_sla_sweep(self->context, 50, NULL);
	activity_lazy_sweep(self, request);

	unread = htmx_request_get_query_param(request, "unread");
	limit = htmx_request_get_query_param(request, "limit");
	rows = venture_notify_list(self->context, principal->user_id,
	                           (0 != g_strcmp0(unread, "0")),
	                           (NULL != limit)
	                           	? (guint)g_ascii_strtoull(limit, NULL, 10) : 0,
	                           &error);

	if (NULL == rows)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "unread");
	json_builder_add_int_value(builder,
		venture_notify_unread_count(self->context, principal->user_id));
	json_builder_set_member_name(builder, "notifications");
	json_builder_begin_array(builder);

	for (i = 0; i < rows->len; i++)
		json_builder_add_value(builder,
			venture_notify_to_json(g_ptr_array_index(rows, i)));

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * POST /api/v1/inbox/read - {"id": N} or {"all": true}.
 */
static HtmxResponse *
venture_web_api_inbox_read(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *denied;
	gint64 id;
	gint changed;

	(void)params;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);
	body = htmx_request_get_json(request, NULL);
	id = ((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
		? venture_json_object_get_int(json_node_get_object(body), "id", 0) : 0;

	changed = venture_notify_mark_read(self->context, principal->user_id, id,
	                                   &error);

	if (changed < 0)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "read");
	json_builder_add_int_value(builder, changed);
	json_builder_set_member_name(builder, "unread");
	json_builder_add_int_value(builder,
		venture_notify_unread_count(self->context, principal->user_id));
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/* --- Watching -------------------------------------------------------------- */

/*
 * POST /watch - type, id, action=watch|unwatch, back.
 */
static HtmxResponse *
venture_web_ui_watch(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *fallback = NULL;
	HtmxResponse *redirect;
	const gchar *type_name;
	const gchar *id;
	const gchar *action;

	(void)params;

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);
	type_name = htmx_request_get_form_value(request, "type");
	id = htmx_request_get_form_value(request, "id");
	action = htmx_request_get_form_value(request, "action");

	if (venture_string_is_empty(type_name) || venture_string_is_empty(id))
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Watching needs a record type and an id");
		return venture_web_error_response(error);
	}

	fallback = g_strdup_printf("/e/%s/%s", type_name, id);
	back = venture_web_desk_back(request, fallback);

	if (0 == g_strcmp0(action, "unwatch"))
		venture_notify_unwatch(self->context, principal->user_id, type_name,
		                       g_ascii_strtoll(id, NULL, 10), &error);
	else
		venture_notify_watch(self->context, principal->user_id, type_name,
		                     g_ascii_strtoll(id, NULL, 10), &error);

	return venture_web_desk_answer(request, back, error);
}

/*
 * POST /api/v1/watch - {"type", "id", "watch": true|false}.
 */
static HtmxResponse *
venture_web_api_watch(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *denied;
	JsonObject *object;
	const gchar *type_name;
	gint64 id;
	gboolean watch;

	(void)params;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);
	body = htmx_request_get_json(request, NULL);

	if ((NULL == body) || !JSON_NODE_HOLDS_OBJECT(body))
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The body must be a JSON object with type and id");
		return venture_web_error_response(error);
	}

	object = json_node_get_object(body);
	type_name = venture_json_object_get_string(object, "type", NULL);
	id = venture_json_object_get_int(object, "id", 0);
	watch = venture_json_object_get_bool(object, "watch", TRUE);

	if (venture_string_is_empty(type_name) || (0 == id))
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Watching needs a record type and an id");
		return venture_web_error_response(error);
	}

	if (watch
	    ? !venture_notify_watch(self->context, principal->user_id, type_name,
	                            id, &error)
	    : !venture_notify_unwatch(self->context, principal->user_id, type_name,
	                              id, &error))
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "watching");
	json_builder_add_boolean_value(builder, watch);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * GET /api/v1/watching/:type/:id - whether you follow it, and who does.
 */
static HtmxResponse *
venture_web_api_watching(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GArray) watchers = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *denied;
	const gchar *type_name;
	gint64 id;
	guint i;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);
	type_name = g_hash_table_lookup(params, "type");
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	watchers = venture_notify_list_watchers(self->context, type_name, id);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "watching");
	json_builder_add_boolean_value(builder,
		venture_notify_is_watching(self->context, principal->user_id,
		                           type_name, id));
	json_builder_set_member_name(builder, "watchers");
	json_builder_begin_array(builder);

	for (i = 0; i < watchers->len; i++)
		json_builder_add_int_value(builder, g_array_index(watchers, gint64, i));

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * The watch button on a record's page, and the eye that says who else
 * follows it.
 */
static void
venture_web_append_watch_button(
	VentureWebServer	*self,
	GString			*content,
	VentureAuthPrincipal	*principal,
	VentureEntity		*record
){
	g_autoptr(GArray) watchers = NULL;
	const gchar *type_name;
	gboolean watching;
	gint64 id;

	if (0 == principal->user_id)
		return;

	type_name = venture_entity_get_entity_name(record);
	id = venture_entity_get_id(record);
	watching = venture_notify_is_watching(self->context, principal->user_id,
	                                      type_name, id);
	watchers = venture_notify_list_watchers(self->context, type_name, id);

	g_string_append_printf(content,
		"<form method=\"post\" action=\"/watch\" class=\"inline\" data-watch>"
		"<input type=\"hidden\" name=\"type\" value=\"%s\">"
		"<input type=\"hidden\" name=\"id\" value=\"%" G_GINT64_FORMAT "\">"
		"<input type=\"hidden\" name=\"action\" value=\"%s\">"
		"<button class=\"btn%s\" type=\"submit\" title=\"%s\">"
		"<span class=\"icon\">" VENTURE_ICON(
			"<path d=\"M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z\"/>"
			"<circle cx=\"12\" cy=\"12\" r=\"3\"/>")
		"</span> %s%s%u</button></form>",
		type_name, id, watching ? "unwatch" : "watch",
		watching ? " watching" : "",
		watching ? "Stop being told about changes to this"
		         : "Be told in your inbox when this changes",
		watching ? "Watching" : "Watch",
		(watchers->len > 0) ? " \xc2\xb7 " : "",
		(watchers->len > 0) ? watchers->len : 0);
}

/* --- The timeline ---------------------------------------------------------- */

/*
 * What happened to a record, as one list: changes with who and what
 * moved, comments and worklogs on a ticket, newest first. Rendered from
 * the same JSON the API returns.
 */
static void
venture_web_append_activity(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(JsonNode) events = NULL;
	JsonArray *array;
	guint i;

	events = venture_desk_activity(self->context,
	                               venture_entity_get_entity_name(record),
	                               venture_entity_get_id(record), 60, NULL);

	if ((NULL == events) || !JSON_NODE_HOLDS_ARRAY(events))
		return;

	array = json_node_get_array(events);

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Activity</h2>");
	g_string_append_printf(content,
		"<a class=\"btn btn-sm\" href=\"/e/audit_entry?target_type=%s"
		"&target_id=%" G_GINT64_FORMAT "\">Audit log</a></div>"
		"<div class=\"card-body\">",
		venture_entity_get_entity_name(record), venture_entity_get_id(record));

	if (0 == json_array_get_length(array))
	{
		g_string_append(content, "<p class=\"muted\">Nothing recorded yet."
		                         "</p></div></div>");
		return;
	}

	g_string_append(content, "<ol class=\"timeline\">");

	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *event;
		const gchar *kind;
		const gchar *actor;
		const gchar *when;
		const gchar *relative;

		event = json_array_get_object_element(array, i);
		kind = venture_json_object_get_string(event, "kind", "change");
		actor = venture_json_object_get_string(event, "actor", NULL);
		when = venture_json_object_get_string(event, "when", NULL);
		relative = venture_json_object_get_string(event, "when_relative", "");

		/* The system's roll-ups -- a first-reply stamp, a logged-hours
		 * total -- follow something a person did that is already on the
		 * list. The API keeps them; the page reads better without. */
		if ((0 == g_strcmp0(kind, "change")) &&
		    (0 == g_strcmp0(venture_json_object_get_string(event, "actor_kind",
		                                                   ""), "system")) &&
		    (0 == g_strcmp0(venture_json_object_get_string(event, "action",
		                                                   ""), "update")))
			continue;

		g_string_append_printf(content,
			"<li class=\"timeline-item timeline-%s\">"
			"<span class=\"timeline-dot\"></span>"
			"<div class=\"timeline-body\"><div class=\"timeline-head\">"
			"<span class=\"timeline-actor\">", kind);
		venture_html_escape_append(content,
			venture_string_is_empty(actor) ? "System" : actor);
		g_string_append(content, "</span> ");

		if (0 == g_strcmp0(kind, "comment"))
		{
			gboolean internal;

			internal = venture_json_object_get_bool(event, "internal", FALSE);
			g_string_append(content, internal ? "left a note" : "commented");
		}
		else if (0 == g_strcmp0(kind, "worklog"))
		{
			g_string_append_printf(content, "logged %.2g hours",
				json_object_has_member(event, "hours")
					? json_object_get_double_member(event, "hours") : 0.0);
		}
		else
		{
			const gchar *action;
			const gchar *approved;

			action = venture_json_object_get_string(event, "action", "changed");
			approved = venture_json_object_get_string(event, "approved_by",
			                                          NULL);
			venture_html_escape_append(content,
				(0 == g_strcmp0(action, "create")) ? "created this"
				: (0 == g_strcmp0(action, "delete")) ? "deleted this"
				: "updated this");

			if (!venture_string_is_empty(approved))
			{
				g_string_append(content, " <span class=\"muted\">(approved by ");
				venture_html_escape_append(content, approved);
				g_string_append(content, ")</span>");
			}
		}

		g_string_append(content, " <time class=\"timeline-when\" title=\"");

		if (NULL != when)
			venture_html_escape_append(content, when);

		g_string_append(content, "\">");
		venture_html_escape_append(content, relative);
		g_string_append(content, "</time></div>");

		if ((0 == g_strcmp0(kind, "comment")) || (0 == g_strcmp0(kind, "worklog")))
		{
			const gchar *body;

			body = venture_json_object_get_string(event, "body", NULL);

			if (!venture_string_is_empty(body))
			{
				g_string_append(content, "<div class=\"timeline-text\">");
				venture_html_escape_append(content, body);
				g_string_append(content, "</div>");
			}
		}
		else
		{
			JsonNode *changes;

			changes = json_object_get_member(event, "changes");

			if ((NULL != changes) && JSON_NODE_HOLDS_OBJECT(changes))
			{
				JsonObject *diff;
				g_autoptr(GList) members = NULL;
				GList *cursor;
				guint shown;

				diff = json_node_get_object(changes);
				members = json_object_get_members(diff);
				shown = 0;

				g_string_append(content, "<dl class=\"timeline-diff\">");

				for (cursor = members; NULL != cursor; cursor = cursor->next)
				{
					JsonNode *pair;
					const gchar *member;
					g_autofree gchar *from = NULL;
					g_autofree gchar *to = NULL;

					member = cursor->data;

					if ((0 == g_strcmp0(member, "updated-at")) ||
					    (0 == g_strcmp0(member, "version")))
						continue;

					if (shown >= 12)
					{
						g_string_append(content, "<dt>\xe2\x80\xa6</dt><dd></dd>");
						break;
					}

					pair = json_object_get_member(diff, member);

					if ((NULL != pair) && JSON_NODE_HOLDS_OBJECT(pair))
					{
						JsonObject *fromto;

						fromto = json_node_get_object(pair);
						from = venture_web_node_text(
							json_object_get_member(fromto, "from"));
						to = venture_web_node_text(
							json_object_get_member(fromto, "to"));
					}

					{
						g_autofree gchar *label = NULL;

						label = venture_web_label_from_name(member);
						g_string_append(content, "<dt>");
						venture_html_escape_append(content, label);
						g_string_append(content, "</dt><dd><s>");
					}
					venture_html_escape_append(content,
						venture_string_is_empty(from) ? "\xe2\x80\x94" : from);
					g_string_append(content, "</s> <b>");
					venture_html_escape_append(content,
						venture_string_is_empty(to) ? "\xe2\x80\x94" : to);
					g_string_append(content, "</b></dd>");
					shown++;
				}

				g_string_append(content, "</dl>");
			}
		}

		g_string_append(content, "</div></li>");
	}

	g_string_append(content, "</ol></div></div>");
}

/*
 * GET /api/v1/activity/:type/:id - the timeline as JSON.
 */
static HtmxResponse *
venture_web_api_activity(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *denied;
	GType entity_type;
	const gchar *limit;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	limit = htmx_request_get_query_param(request, "limit");
	node = venture_desk_activity(self->context,
	                             g_hash_table_lookup(params, "type"),
	                             g_ascii_strtoll(g_hash_table_lookup(params, "id"),
	                                             NULL, 10),
	                             (NULL != limit)
	                             	? (guint)g_ascii_strtoull(limit, NULL, 10)
	                             	: 0,
	                             &error);

	if (NULL == node)
		return venture_web_error_response(error);

	return venture_web_json_response(node, 200);
}

/* --- A ticket's desk ------------------------------------------------------- */

/*
 * "2h 15m left", "3d over", from seconds.
 */
static gchar *
venture_web_sla_countdown(gint64 seconds)
{
	gint64 magnitude;
	const gchar *suffix;

	magnitude = (seconds < 0) ? -seconds : seconds;
	suffix = (seconds < 0) ? "over" : "left";

	if (magnitude >= 2 * 86400)
		return g_strdup_printf("%" G_GINT64_FORMAT "d %s", magnitude / 86400,
		                       suffix);

	if (magnitude >= 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT "h %" G_GINT64_FORMAT
		                       "m %s", magnitude / 3600,
		                       (magnitude % 3600) / 60, suffix);

	return g_strdup_printf("%" G_GINT64_FORMAT "m %s", magnitude / 60, suffix);
}

static const gchar *
venture_web_sla_tone(VentureSlaState state)
{
	switch (state)
	{
	case VENTURE_SLA_STATE_OK:       return "positive";
	case VENTURE_SLA_STATE_WARNING:  return "warning";
	case VENTURE_SLA_STATE_BREACHED: return "negative";
	case VENTURE_SLA_STATE_NONE:
	default:                         return "";
	}
}

/*
 * The service-level badge on a card: the more pressing of the two
 * clocks, or nothing when no policy covers the ticket.
 */
static void
venture_web_append_sla_badge(
	GString		*content,
	VentureEntity	*ticket
){
	VentureSlaStatus status;
	VentureSlaState state;
	gint64 remaining;
	g_autofree gchar *text = NULL;

	venture_sla_status(ticket, NULL, &status);

	if (!status.has_policy || status.closed)
		return;

	/* First response until it is made; resolution after. */
	if (!status.responded && (VENTURE_SLA_STATE_NONE != status.first_response))
	{
		state = status.first_response;
		remaining = status.first_response_remaining;
		text = venture_web_sla_countdown(remaining);
		g_string_append_printf(content,
			"<span class=\"badge sla %s\" title=\"First reply due\">reply %s"
			"</span>", venture_web_sla_tone(state), text);
		return;
	}

	if (VENTURE_SLA_STATE_NONE == status.resolution)
		return;

	state = status.resolution;
	remaining = status.resolution_remaining;
	text = venture_web_sla_countdown(remaining);
	g_string_append_printf(content,
		"<span class=\"badge sla %s\" title=\"Resolution due\">%s</span>",
		venture_web_sla_tone(state), text);
}

/*
 * The ticket's desk: its service level, the macros, time logging, and
 * the quick assignment. One card, above the comment composer.
 */
static void
venture_web_append_ticket_desk_block(
	VentureWebServer	*self,
	GString			*content,
	VentureAuthPrincipal	*principal,
	VentureEntity		*ticket
){
	g_autoptr(GPtrArray) macros = NULL;
	g_autofree gchar *assignee = NULL;
	VentureSlaStatus status;
	gdouble logged = 0.0;
	gint64 id;
	guint i;

	id = venture_entity_get_id(ticket);
	g_object_get(ticket, "assignee", &assignee, "logged-hours", &logged, NULL);
	venture_sla_status(ticket, NULL, &status);

	g_string_append(content, "<div class=\"card desk\"><div class=\"card-head\">"
	                         "<h2>Desk</h2><div class=\"desk-actions\">");

	if (!venture_string_is_empty(principal->name) &&
	    (0 != g_strcmp0(assignee, principal->name)))
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT
			"/assign-me\" class=\"inline\">"
			"<button class=\"btn btn-sm\" type=\"submit\">Assign to me"
			"</button></form>", id);

	g_string_append(content, "</div></div><div class=\"card-body desk-body\">");

	/* The clocks. */
	g_string_append(content, "<div class=\"desk-sla\">");

	if (!status.has_policy)
	{
		g_string_append(content, "<span class=\"muted\">No service level "
		                         "covers this ticket.</span>");
	}
	else
	{
		if (VENTURE_SLA_STATE_NONE != status.first_response)
		{
			g_autofree gchar *text = NULL;

			text = status.responded
				? g_strdup((VENTURE_SLA_STATE_OK == status.first_response)
				           ? "replied in time" : "replied late")
				: venture_web_sla_countdown(status.first_response_remaining);
			g_string_append_printf(content,
				"<div class=\"desk-clock\"><span class=\"desk-clock-label\">"
				"First reply</span><span class=\"badge %s\">%s</span></div>",
				venture_web_sla_tone(status.first_response), text);
		}

		if (VENTURE_SLA_STATE_NONE != status.resolution)
		{
			g_autofree gchar *text = NULL;

			text = status.closed
				? g_strdup((VENTURE_SLA_STATE_OK == status.resolution)
				           ? "resolved in time" : "resolved late")
				: venture_web_sla_countdown(status.resolution_remaining);
			g_string_append_printf(content,
				"<div class=\"desk-clock\"><span class=\"desk-clock-label\">"
				"Resolution</span><span class=\"badge %s\">%s</span></div>",
				venture_web_sla_tone(status.resolution), text);
		}
	}

	g_string_append(content, "</div>");

	/* The macros, as a form with a select: one press applies one. */
	macros = venture_desk_list_macros(self->context);

	if ((NULL != macros) && (macros->len > 0))
	{
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT
			"/macro\" class=\"desk-form\">"
			"<label>Macro <select name=\"macro_id\">", id);

		for (i = 0; i < macros->len; i++)
		{
			VentureEntity *macro;
			g_autofree gchar *name = NULL;
			g_autofree gchar *description = NULL;

			macro = g_ptr_array_index(macros, i);
			g_object_get(macro, "name", &name, "description", &description,
			             NULL);
			g_string_append_printf(content, "<option value=\"%" G_GINT64_FORMAT
			                                "\" title=\"",
			                       venture_entity_get_id(macro));

			if (NULL != description)
				venture_html_escape_append(content, description);

			g_string_append(content, "\">");
			venture_html_escape_append(content, name);
			g_string_append(content, "</option>");
		}

		g_string_append(content, "</select></label>"
		                         "<button class=\"btn btn-sm btn-primary\" "
		                         "type=\"submit\">Apply</button></form>");
	}
	else
	{
		g_string_append(content, "<p class=\"muted desk-hint\">No macros yet. "
		                         "<a href=\"/e/macro/new\">Write one</a> and "
		                         "it appears here.</p>");
	}

	/* Time. */
	g_string_append_printf(content,
		"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT
		"/worklog\" class=\"desk-form\">"
		"<label>Log <input type=\"number\" name=\"hours\" step=\"0.25\" "
		"min=\"0.25\" placeholder=\"hours\" required style=\"width:6em\">"
		"</label>"
		"<input type=\"text\" name=\"note\" placeholder=\"on what\">"
		"<button class=\"btn btn-sm\" type=\"submit\">Log time</button>"
		"<span class=\"muted\">%.2f h logged</span></form>", id, logged);

	/*
	 * What whoever raised it made of it. Shown always rather than only
	 * on a closed ticket: a rating arrives when it arrives, often in the
	 * reply that says thanks, and a control that appears only in one
	 * state is a control nobody finds.
	 */
	{
		g_autofree gchar *comment = NULL;
		VentureSatisfaction satisfaction;
		g_autoptr(GEnumClass) klass = NULL;
		guint j;

		g_object_get(ticket, "satisfaction", &satisfaction,
		             "satisfaction-comment", &comment, NULL);
		klass = g_type_class_ref(VENTURE_TYPE_SATISFACTION);

		g_string_append_printf(content,
			"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT
			"/satisfaction\" class=\"desk-form\">"
			"<label>Satisfaction <select name=\"satisfaction\">", id);

		for (j = 0; j < klass->n_values; j++)
		{
			const gchar *nick;

			nick = klass->values[j].value_nick;
			g_string_append_printf(content, "<option value=\"%s\"%s>", nick,
				((gint)satisfaction == klass->values[j].value) ? " selected"
				                                               : "");
			venture_html_escape_append(content, nick);
			g_string_append(content, "</option>");
		}

		g_string_append(content, "</select></label>"
		                         "<input type=\"text\" name=\"comment\" "
		                         "placeholder=\"what they said\" value=\"");

		if (!venture_string_is_empty(comment))
			venture_html_escape_append(content, comment);

		g_string_append(content, "\">"
		                         "<button class=\"btn btn-sm\" "
		                         "type=\"submit\">Record</button></form>");
	}

	/*
	 * The assistant's three judgements, each its own request: a model
	 * call takes seconds, and a ticket page that waited for one before
	 * rendering would be a ticket page nobody opened twice.
	 */
	if (venture_ai_assist_available(self->context))
	{
		g_string_append(content, "<div class=\"desk-form assist-actions\">"
		                         "<span class=\"desk-clock-label\">"
		                         "Assistant</span>");
		g_string_append_printf(content,
			"<button class=\"btn btn-sm\" hx-get=\"/tickets/%"
			G_GINT64_FORMAT "/assist?what=triage\" hx-target=\"#assist\" "
			"hx-swap=\"outerHTML\" hx-indicator=\"#assist\" "
			"title=\"Propose a priority, a type and tags\">Triage</button>"
			"<button class=\"btn btn-sm\" hx-get=\"/tickets/%"
			G_GINT64_FORMAT "/assist?what=summary\" hx-target=\"#assist\" "
			"hx-swap=\"outerHTML\" hx-indicator=\"#assist\" "
			"title=\"What the thread amounts to\">Summarise</button>"
			"<button class=\"btn btn-sm\" hx-get=\"/tickets/%"
			G_GINT64_FORMAT "/assist?what=draft\" hx-target=\"#assist\" "
			"hx-swap=\"outerHTML\" hx-indicator=\"#assist\" "
			"title=\"Draft the next reply, for you to edit\">Draft a reply"
			"</button>", id, id, id);
		g_string_append(content, "</div>"
		                         "<div class=\"assist-slot\" id=\"assist\">"
		                         "</div>");
	}

	g_string_append(content, "</div></div>");
}

/*
 * POST /tickets/:id/macro - apply one.
 */
static HtmxResponse *
venture_web_ui_ticket_macro(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) macro = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *back = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *macro_id;
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

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	ticket = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_TICKET, ticket_id, &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	macro_id = htmx_request_get_form_value(request, "macro_id");
	macro = venture_desk_find_macro(self->context, macro_id);

	if (NULL == macro)
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no macro \"%s\"",
		            (NULL != macro_id) ? macro_id : "");
		return venture_web_error_response(error);
	}

	venture_auth_to_actor(principal, &actor);
	venture_desk_apply_macro(self->context, ticket, macro, &actor, &error);

	back = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	return venture_web_desk_answer(request, back, error);
}

/*
 * POST /tickets/:id/worklog - hours and a note.
 */
static HtmxResponse *
venture_web_ui_ticket_worklog(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) worklog = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *back = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *hours;
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

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	hours = htmx_request_get_form_value(request, "hours");
	venture_auth_to_actor(principal, &actor);

	worklog = venture_desk_log_work(self->context, ticket_id,
	                                (NULL != hours) ? g_ascii_strtod(hours, NULL)
	                                                : 0.0,
	                                htmx_request_get_form_value(request, "note"),
	                                &actor, &error);

	back = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	return venture_web_desk_answer(request, back, error);
}

/*
 * POST /tickets/:id/assign-me - the one-press handover.
 */
static HtmxResponse *
venture_web_ui_ticket_assign_me(
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

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	ticket = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_TICKET, ticket_id, &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	g_object_set(ticket, "assignee", principal->name, NULL);
	venture_auth_to_actor(principal, &actor);
	venture_database_save(venture_context_get_database(self->context), ticket,
	                      &actor, &error);

	back = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	return venture_web_desk_answer(request, back, error);
}

/*
 * The API forms of the three, plus the service level as JSON.
 */
static HtmxResponse *
venture_web_api_ticket_load(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	GHashTable		 *params,
	VentureUserRole		  role,
	VentureAuthPrincipal	**out_principal,
	VentureEntity		**out_ticket
){
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = venture_web_require_module_api(self, "tickets");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, role);

	if (NULL != gate)
		return gate;

	*out_principal = venture_auth_authenticate(self->auth, request);
	*out_ticket = venture_database_get(venture_context_get_database(self->context),
	                                   VENTURE_TYPE_TICKET,
	                                   g_ascii_strtoll(g_hash_table_lookup(params,
	                                                                       "id"),
	                                                   NULL, 10),
	                                   &error);

	if (NULL == *out_ticket)
		return venture_web_error_response(error);

	return NULL;
}

static HtmxResponse *
venture_web_api_ticket_sla(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(JsonNode) node = NULL;
	VentureSlaStatus status;
	HtmxResponse *gate;

	gate = venture_web_api_ticket_load(self, request, params,
	                                   VENTURE_USER_ROLE_VIEWER, &principal,
	                                   &ticket);

	if (NULL != gate)
		return gate;

	venture_sla_status(ticket, NULL, &status);
	node = venture_sla_status_to_json(&status);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_ticket_macro(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) macro = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *name;

	gate = venture_web_api_ticket_load(self, request, params,
	                                   VENTURE_USER_ROLE_EDITOR, &principal,
	                                   &ticket);

	if (NULL != gate)
		return gate;

	body = htmx_request_get_json(request, NULL);
	name = ((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
		? venture_json_object_get_string(json_node_get_object(body), "macro",
		                                 NULL)
		: NULL;
	macro = venture_desk_find_macro(self->context, name);

	if (NULL == macro)
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no macro \"%s\"; the body needs {\"macro\": "
		            "name or id}", (NULL != name) ? name : "");
		return venture_web_error_response(error);
	}

	venture_auth_to_actor(principal, &actor);

	if (!venture_desk_apply_macro(self->context, ticket, macro, &actor, &error))
		return venture_web_error_response(error);

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(ticket), FALSE);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_ticket_worklog(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) worklog = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	JsonObject *object;
	gdouble hours;

	gate = venture_web_api_ticket_load(self, request, params,
	                                   VENTURE_USER_ROLE_EDITOR, &principal,
	                                   &ticket);

	if (NULL != gate)
		return gate;

	body = htmx_request_get_json(request, NULL);
	object = ((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
		? json_node_get_object(body) : NULL;
	hours = ((NULL != object) && json_object_has_member(object, "hours"))
		? json_node_get_double(json_object_get_member(object, "hours")) : 0.0;

	venture_auth_to_actor(principal, &actor);
	worklog = venture_desk_log_work(self->context, venture_entity_get_id(ticket),
	                                hours,
	                                (NULL != object)
	                                	? venture_json_object_get_string(object,
	                                	                                 "note",
	                                	                                 NULL)
	                                	: NULL,
	                                &actor, &error);

	if (NULL == worklog)
		return venture_web_error_response(error);

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(worklog), FALSE);

	return venture_web_json_response(node, 201);
}

/*
 * POST /api/v1/sla/sweep - mark what has fallen due, now.
 */
static HtmxResponse *
venture_web_api_sla_sweep(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	gint marked;

	(void)params;

	gate = venture_web_require_module_api(self, "tickets");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != gate)
		return gate;

	marked = venture_sla_sweep(self->context, 0, &error);

	if (marked < 0)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "marked");
	json_builder_add_int_value(builder, marked);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/* --- Sprints --------------------------------------------------------------- */

/*
 * A sprint's burn as a bar: done points over planned, against capacity.
 */
static void
venture_web_append_sprint_progress(
	GString			*content,
	VentureEntity		*sprint,
	VentureSprintProgress	*progress
){
	gint percent;
	gint64 denominator;

	denominator = (progress->points > 0) ? progress->points : progress->tickets;
	percent = (denominator > 0)
		? (gint)((((progress->points > 0) ? progress->points_done
		                                  : progress->done) * 100)
		         / denominator)
		: 0;

	g_string_append_printf(content,
		"<div class=\"sprint-burn\"><span class=\"bar-track\">"
		"<span class=\"bar-fill\" style=\"width:%d%%\"></span></span>"
		"<span class=\"bar-value\">", percent);

	if (progress->points > 0)
		g_string_append_printf(content, "%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT
		                       " pts", progress->points_done, progress->points);
	else
		g_string_append_printf(content, "%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT
		                       " tickets", progress->done, progress->tickets);

	if (progress->capacity > 0)
		g_string_append_printf(content, " \xc2\xb7 capacity %" G_GINT64_FORMAT,
		                       progress->capacity);

	if (progress->days_total > 0)
	{
		if (progress->days_left >= 0)
			g_string_append_printf(content, " \xc2\xb7 %" G_GINT64_FORMAT
			                       " day%s left", progress->days_left,
			                       (1 == progress->days_left) ? "" : "s");
		else
			g_string_append_printf(content, " \xc2\xb7 ended %" G_GINT64_FORMAT
			                       " day%s ago", -progress->days_left,
			                       (-1 == progress->days_left) ? "" : "s");
	}

	g_string_append(content, "</span></div>");
	(void)sprint;
}

/*
 * The sprint's page: its burn and its tickets by status.
 */
static void
venture_web_append_sprint_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(JsonNode) node = NULL;
	VentureSprintProgress progress;
	JsonObject *sprint;
	JsonArray *items;
	guint i;

	venture_desk_sprint_progress(self->context, record, &progress);
	node = venture_desk_sprint_to_json(self->context, record, TRUE);
	sprint = json_node_get_object(node);
	items = json_object_get_array_member(sprint, "items");

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Burn</h2>");
	g_string_append_printf(content,
		"<a class=\"btn btn-sm\" href=\"/tickets?sprint_id=%" G_GINT64_FORMAT
		"\">On the board</a></div><div class=\"card-body\">",
		venture_entity_get_id(record));
	venture_web_append_sprint_progress(content, record, &progress);

	if (0 == json_array_get_length(items))
	{
		g_string_append(content, "<p class=\"muted\">No tickets are planned "
		                         "into this sprint. Set a ticket's Sprint "
		                         "field, or drag it on the board.</p>");
	}
	else
	{
		g_string_append(content, "<div class=\"table-wrap\"><table class=\"data\">"
		                         "<thead><tr><th>Ticket</th><th>Status</th>"
		                         "<th>Priority</th><th>Assignee</th>"
		                         "<th class=\"num\">Points</th></tr></thead>"
		                         "<tbody>");

		for (i = 0; i < json_array_get_length(items); i++)
		{
			JsonObject *item;

			item = json_array_get_object_element(items, i);
			g_string_append_printf(content,
				"<tr data-href=\"/e/ticket/%" G_GINT64_FORMAT "\"><td><a href=\""
				"/e/ticket/%" G_GINT64_FORMAT "\">",
				venture_json_object_get_int(item, "id", 0),
				venture_json_object_get_int(item, "id", 0));
			venture_html_escape_append(content,
				venture_json_object_get_string(item, "title", ""));
			g_string_append(content, "</a></td><td><span class=\"badge\">");
			venture_html_escape_append(content,
				venture_json_object_get_string(item, "status", ""));
			g_string_append(content, "</span></td><td>");
			venture_html_escape_append(content,
				venture_json_object_get_string(item, "priority", ""));
			g_string_append(content, "</td><td>");
			venture_html_escape_append(content,
				venture_json_object_get_string(item, "assignee", ""));
			g_string_append_printf(content, "</td><td class=\"num\">%"
			                       G_GINT64_FORMAT "</td></tr>",
			                       venture_json_object_get_int(item, "points", 0));
		}

		g_string_append(content, "</tbody></table></div>");
	}

	g_string_append(content, "</div></div>");
}

/*
 * GET /sprints - every sprint with its burn, the active one first.
 */
static HtmxResponse *
venture_web_ui_sprints(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) sprints = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	guint i;

	(void)params;

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

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	sprints = venture_desk_list_sprints(self->context, NULL, 0, &error);

	if (NULL == sprints)
		return venture_web_error_response(error);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Sprints</h1><span class=\"subtitle\">"
	                       "Fixed windows of work, and how each is burning"
	                       "</span></div><div class=\"page-actions\">"
	                       "<a class=\"btn btn-primary\" href=\"/e/sprint/new\">"
	                       "New sprint</a></div></div>");

	if (0 == sprints->len)
	{
		g_string_append(content, "<div class=\"empty\"><h3>No sprints yet</h3>"
		                         "<p class=\"muted\">Make one, then plan "
		                         "tickets into it from their Sprint field."
		                         "</p></div>");
	}

	for (i = 0; i < sprints->len; i++)
	{
		VentureEntity *sprint;
		g_autofree gchar *name = NULL;
		g_autofree gchar *goal = NULL;
		VentureSprintStatus status;
		VentureSprintProgress progress;

		sprint = g_ptr_array_index(sprints, i);
		g_object_get(sprint, "name", &name, "goal", &goal, "status", &status,
		             NULL);
		venture_desk_sprint_progress(self->context, sprint, &progress);

		g_string_append_printf(content,
			"<div class=\"card sprint-card sprint-%s\"><div class=\"card-head\">"
			"<h2><a href=\"/e/sprint/%" G_GINT64_FORMAT "\">",
			venture_enum_to_nick(VENTURE_TYPE_SPRINT_STATUS, (gint)status),
			venture_entity_get_id(sprint));
		venture_html_escape_append(content, name);
		g_string_append_printf(content, "</a> <span class=\"badge %s\">%s</span>"
		                                "</h2></div><div class=\"card-body\">",
			(VENTURE_SPRINT_STATUS_ACTIVE == status) ? "positive" : "",
			venture_enum_to_nick(VENTURE_TYPE_SPRINT_STATUS, (gint)status));

		if (!venture_string_is_empty(goal))
		{
			g_string_append(content, "<p class=\"sprint-goal\">");
			venture_html_escape_append(content, goal);
			g_string_append(content, "</p>");
		}

		venture_web_append_sprint_progress(content, sprint, &progress);
		g_string_append(content, "</div></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/sprints", "Sprints", content->str),
		200);
}

/*
 * GET /api/v1/sprints and GET /api/v1/sprints/:id.
 */
static HtmxResponse *
venture_web_api_sprints(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(GPtrArray) sprints = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	guint i;

	(void)params;

	gate = venture_web_require_module_api(self, "tickets");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	sprints = venture_desk_list_sprints(self->context, NULL, 0, &error);

	if (NULL == sprints)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < sprints->len; i++)
		json_builder_add_value(builder,
			venture_desk_sprint_to_json(self->context,
			                            g_ptr_array_index(sprints, i), FALSE));

	json_builder_end_array(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_sprint(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureEntity) sprint = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = venture_web_require_module_api(self, "tickets");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	sprint = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_SPRINT,
	                              g_ascii_strtoll(g_hash_table_lookup(params, "id"),
	                                              NULL, 10),
	                              &error);

	if (NULL == sprint)
		return venture_web_error_response(error);

	node = venture_desk_sprint_to_json(self->context, sprint, TRUE);

	return venture_web_json_response(node, 200);
}

/* --- Incidents ------------------------------------------------------------- */

/*
 * The incident's page: the fix, or the button that opens it.
 */
static void
venture_web_append_incident_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	gint64 ticket_id = 0;

	(void)self;
	g_object_get(record, "ticket-id", &ticket_id, NULL);

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>The fix</h2></div><div class=\"card-body\">");

	if (0 != ticket_id)
	{
		g_string_append_printf(content,
			"<p>Tracked as <a href=\"/e/ticket/%" G_GINT64_FORMAT
			"\">ticket #%" G_GINT64_FORMAT "</a>.</p>", ticket_id, ticket_id);
	}
	else if (venture_web_module_enabled(self, "tickets"))
	{
		g_string_append_printf(content,
			"<p class=\"muted\">No ticket tracks the fix yet.</p>"
			"<form method=\"post\" action=\"/incidents/%" G_GINT64_FORMAT
			"/ticket\"><button class=\"btn btn-primary\" type=\"submit\">"
			"Open a fix ticket</button> <span class=\"muted\">A bug, "
			"prioritised from the severity, on the release's repository."
			"</span></form>", venture_entity_get_id(record));
	}
	else
	{
		g_string_append(content, "<p class=\"muted\">The tickets module is "
		                         "off.</p>");
	}

	g_string_append(content, "</div></div>");
}

/*
 * POST /incidents/:id/ticket - open the bug.
 */
static HtmxResponse *
venture_web_ui_incident_ticket(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) incident = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	HtmxResponse *redirect;
	VentureActor actor;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "factory");

		if (NULL != gate)
			return gate;
	}

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	incident = venture_database_get(venture_context_get_database(self->context),
	                                VENTURE_TYPE_INCIDENT,
	                                g_ascii_strtoll(g_hash_table_lookup(params,
	                                                                    "id"),
	                                                NULL, 10),
	                                &error);

	if (NULL == incident)
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);
	ticket = venture_factory_open_fix_ticket(self->context, incident, &actor,
	                                         &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	destination = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT,
	                              venture_entity_get_id(ticket));

	return venture_web_redirect_to(destination);
}

static HtmxResponse *
venture_web_api_incident_ticket(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) incident = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	VentureActor actor;

	gate = venture_web_require_module_api(self, "factory");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != gate)
		return gate;

	principal = venture_auth_authenticate(self->auth, request);
	incident = venture_database_get(venture_context_get_database(self->context),
	                                VENTURE_TYPE_INCIDENT,
	                                g_ascii_strtoll(g_hash_table_lookup(params,
	                                                                    "id"),
	                                                NULL, 10),
	                                &error);

	if (NULL == incident)
		return venture_web_error_response(error);

	venture_auth_to_actor(principal, &actor);
	ticket = venture_factory_open_fix_ticket(self->context, incident, &actor,
	                                         &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(ticket), FALSE);

	return venture_web_json_response(node, 201);
}

/* --- Mission control ------------------------------------------------------- */

/*
 * The runs table, rendered from the same JSON the API returns. A
 * fragment, so the page can poll it while anything is live.
 */
static void
venture_web_append_runs_table(
	GString		*content,
	JsonObject	*status,
	gint64		 interval,
	const gchar	*state
){
	JsonArray *runs;
	JsonObject *totals;
	guint i;

	runs = json_object_get_array_member(status, "runs");
	totals = json_object_get_object_member(status, "totals");

	g_string_append(content, "<div id=\"runs-table\"");

	if (venture_json_object_get_int(totals, "live", 0) > 0)
	{
		g_string_append_printf(content,
			" hx-get=\"/runs/table?state=%s\" hx-trigger=\"every %"
			G_GINT64_FORMAT "s\" hx-swap=\"outerHTML\"",
			venture_string_is_empty(state) ? "all" : state, interval);
	}

	g_string_append(content, ">");

	g_string_append(content, "<div class=\"stat-row\">");
	g_string_append_printf(content,
		"<div class=\"stat\"><span class=\"stat-value\">%" G_GINT64_FORMAT
		"</span><span class=\"stat-label\">Live</span></div>"
		"<div class=\"stat\"><span class=\"stat-value\">%" G_GINT64_FORMAT
		"</span><span class=\"stat-label\">Succeeded</span></div>"
		"<div class=\"stat\"><span class=\"stat-value\">%" G_GINT64_FORMAT
		"</span><span class=\"stat-label\">Failed</span></div>"
		"<div class=\"stat\"><span class=\"stat-value\">%" G_GINT64_FORMAT
		"</span><span class=\"stat-label\">Pull requests</span></div>"
		"<div class=\"stat\"><span class=\"stat-value\">",
		venture_json_object_get_int(totals, "live", 0),
		venture_json_object_get_int(totals, "succeeded", 0),
		venture_json_object_get_int(totals, "failed", 0),
		venture_json_object_get_int(totals, "pull_requests", 0));
	venture_html_escape_append(content,
		venture_json_object_get_string(totals, "cost_display", "\xe2\x80\x94"));
	g_string_append(content, "</span><span class=\"stat-label\">Cost</span>"
	                         "</div><div class=\"stat\"><span class=\"stat-value\">");
	venture_html_escape_append(content,
		venture_json_object_get_string(totals, "cost_per_success_display",
		                               "\xe2\x80\x94"));
	g_string_append(content, "</span><span class=\"stat-label\">Per success"
	                         "</span></div></div>");

	if (0 == json_array_get_length(runs))
	{
		g_string_append(content, "<div class=\"empty\"><h3>No runs</h3>"
		                         "<p class=\"muted\">Start one from a ticket "
		                         "that names a repository, or let a rule "
		                         "start it.</p></div></div>");
		return;
	}

	g_string_append(content, "<div class=\"card\"><div class=\"table-wrap\">"
	                         "<table class=\"data runs\"><thead><tr>"
	                         "<th>Run</th><th>Ticket</th><th>State</th>"
	                         "<th>Runner</th><th>Model</th><th>Branch</th>"
	                         "<th class=\"num\">Tokens</th><th class=\"num\">"
	                         "Cost</th><th class=\"num\">Time</th>"
	                         "<th class=\"row-actions\"></th></tr></thead>"
	                         "<tbody>");

	for (i = 0; i < json_array_get_length(runs); i++)
	{
		JsonObject *run;
		const gchar *run_state;
		const gchar *tone;
		gint64 seconds;
		gint64 id;

		run = json_array_get_object_element(runs, i);
		id = venture_json_object_get_int(run, "id", 0);
		run_state = venture_json_object_get_string(run, "state", "");
		seconds = venture_json_object_get_int(run, "seconds", 0);
		tone = (0 == g_strcmp0(run_state, "succeeded")) ? "positive"
		     : (0 == g_strcmp0(run_state, "running")) ? "info"
		     : (0 == g_strcmp0(run_state, "queued")) ? "warning"
		     : ((0 == g_strcmp0(run_state, "failed")) ||
		        (0 == g_strcmp0(run_state, "interrupted"))) ? "negative" : "";

		g_string_append_printf(content,
			"<tr data-href=\"/e/forge_run/%" G_GINT64_FORMAT "\">"
			"<td><a href=\"/e/forge_run/%" G_GINT64_FORMAT "\">#%"
			G_GINT64_FORMAT "</a></td>"
			"<td><a href=\"/e/ticket/%" G_GINT64_FORMAT "\">",
			id, id, id, venture_json_object_get_int(run, "ticket_id", 0));
		venture_html_escape_append(content,
			venture_json_object_get_string(run, "ticket", ""));
		g_string_append_printf(content, "</a></td><td><span class=\"badge %s\">",
		                       tone);
		venture_html_escape_append(content, run_state);
		g_string_append(content, "</span></td><td>");
		venture_html_escape_append(content,
			venture_json_object_get_string(run, "runner", ""));
		g_string_append(content, "</td><td class=\"muted\">");
		venture_html_escape_append(content,
			venture_json_object_get_string(run, "model", ""));
		g_string_append(content, "</td><td><code>");
		venture_html_escape_append(content,
			venture_json_object_get_string(run, "branch", ""));
		g_string_append_printf(content, "</code></td><td class=\"num\">%"
		                       G_GINT64_FORMAT "</td><td class=\"num\">",
			venture_json_object_get_int(run, "input_tokens", 0)
			+ venture_json_object_get_int(run, "output_tokens", 0));
		venture_html_escape_append(content,
			venture_json_object_get_string(run, "cost_display", ""));
		g_string_append(content, "</td><td class=\"num\">");

		if (seconds >= 3600)
			g_string_append_printf(content, "%" G_GINT64_FORMAT "h %"
			                       G_GINT64_FORMAT "m", seconds / 3600,
			                       (seconds % 3600) / 60);
		else if (seconds > 0)
			g_string_append_printf(content, "%" G_GINT64_FORMAT "m %"
			                       G_GINT64_FORMAT "s", seconds / 60,
			                       seconds % 60);

		g_string_append(content, "</td><td class=\"row-actions\">");

		if ((0 == g_strcmp0(run_state, "queued")) ||
		    (0 == g_strcmp0(run_state, "running")))
			g_string_append_printf(content,
				"<form method=\"post\" action=\"/runs/%" G_GINT64_FORMAT
				"/cancel\" class=\"inline\"><button class=\"btn btn-sm\" "
				"type=\"submit\">Cancel</button></form>", id);

		g_string_append(content, "</td></tr>");
	}

	g_string_append(content, "</tbody></table></div></div></div>");
}

/*
 * The budgets, each as a bar.
 */
static void
venture_web_append_budgets(
	VentureWebServer	*self,
	GString			*content
){
	g_autoptr(JsonNode) node = NULL;
	JsonArray *budgets;
	guint i;

	node = venture_factory_budgets_describe(self->context, NULL);

	g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
	                         "<h2>Budgets</h2><a class=\"btn btn-sm\" "
	                         "href=\"/e/agent_budget/new\">New budget</a></div>"
	                         "<div class=\"card-body\">");

	budgets = ((NULL != node) && JSON_NODE_HOLDS_ARRAY(node))
		? json_node_get_array(node) : NULL;

	if ((NULL == budgets) || (0 == json_array_get_length(budgets)))
	{
		g_string_append(content, "<p class=\"muted\">No budget caps the agent "
		                         "spend. Set one and the runs page, the "
		                         "inbox and the run itself all respect it."
		                         "</p></div></div>");
		return;
	}

	g_string_append(content, "<ul class=\"bar-list\">");

	for (i = 0; i < json_array_get_length(budgets); i++)
	{
		JsonObject *budget;
		gint64 percent;
		gboolean exhausted;
		gboolean warning;

		budget = json_array_get_object_element(budgets, i);
		percent = venture_json_object_get_int(budget, "percent", 0);
		exhausted = venture_json_object_get_bool(budget, "exhausted", FALSE);
		warning = venture_json_object_get_bool(budget, "warning", FALSE);

		g_string_append_printf(content,
			"<li><a class=\"bar-row\" href=\"/e/agent_budget/%" G_GINT64_FORMAT
			"\"><span class=\"bar-label\">",
			venture_json_object_get_int(budget, "id", 0));
		venture_html_escape_append(content,
			venture_json_object_get_string(budget, "name", ""));
		g_string_append_printf(content,
			" <span class=\"muted\">%s</span></span>"
			"<span class=\"bar-track\"><span class=\"bar-fill%s\" "
			"style=\"width:%d%%\"></span></span><span class=\"bar-value\">",
			venture_json_object_get_string(budget, "period", ""),
			exhausted ? " negative" : warning ? " warning" : "",
			(gint)MIN(percent, 100));
		venture_html_escape_append(content,
			venture_json_object_get_string(budget, "spent_display", ""));
		g_string_append(content, " / ");
		venture_html_escape_append(content,
			venture_json_object_get_string(budget, "limit_display", ""));
		g_string_append_printf(content, " (%" G_GINT64_FORMAT "%%)%s</span></a></li>",
			percent,
			venture_json_object_get_bool(budget, "comparable", TRUE)
				? "" : " \xe2\x80\x94 other currency");
	}

	g_string_append(content, "</ul></div></div>");
}

/*
 * GET /runs - mission control.
 */
static HtmxResponse *
venture_web_ui_runs(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	const gchar *state;
	gint64 interval = 3;

	(void)params;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER,
	                          &error))
		return venture_web_error_response(error);

	state = htmx_request_get_query_param(request, "state");
	g_object_get(venture_context_get_config(self->context),
	             "forge-poll-interval", &interval, NULL);
	node = venture_factory_runs_describe(self->context, NULL, 0, state, 100,
	                                     &error);

	if (NULL == node)
		return venture_web_error_response(error);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Runs</h1><span class=\"subtitle\">"
	                       "Every coding run, what it did and what it cost"
	                       "</span></div><div class=\"page-actions\">"
	                       "<div class=\"segmented\">");

	{
		static const gchar *const states[] = {
			"all", "queued", "running", "succeeded", "failed", NULL
		};
		gsize i;

		for (i = 0; NULL != states[i]; i++)
		{
			gboolean active;

			active = (0 == i)
				? (venture_string_is_empty(state) ||
				   (0 == g_strcmp0(state, "all")))
				: (0 == g_strcmp0(state, states[i]));
			g_string_append_printf(content,
				"<a class=\"seg%s\" href=\"/runs?state=%s\">%s</a>",
				active ? " active" : "", states[i],
				(0 == i) ? "All" : states[i]);
		}
	}

	g_string_append(content, "</div><a class=\"btn\" href=\"/e/forge_rule\">"
	                         "Rules</a><a class=\"btn\" href=\"/reports/delivery\">"
	                         "Delivery report</a></div></div>");

	venture_web_append_runs_table(content, json_node_get_object(node), interval,
	                              state);
	venture_web_append_budgets(self, content);

	return venture_web_html_response(
		venture_web_page(self, request, "/runs", "Runs", content->str), 200);
}

/*
 * GET /runs/table - the fragment the page polls.
 */
static HtmxResponse *
venture_web_ui_runs_table(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *denied;
	const gchar *state;
	gint64 interval = 3;

	(void)params;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "forge");

		if (NULL != gate)
			return gate;
	}

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	state = htmx_request_get_query_param(request, "state");
	g_object_get(venture_context_get_config(self->context),
	             "forge-poll-interval", &interval, NULL);
	node = venture_factory_runs_describe(self->context, NULL, 0, state, 100,
	                                     &error);

	if (NULL == node)
		return venture_web_error_response(error);

	content = g_string_new(NULL);
	venture_web_append_runs_table(content, json_node_get_object(node), interval,
	                              state);

	return venture_web_html_response(g_strdup(content->str), 200);
}

/*
 * GET /api/v1/runs and GET /api/v1/budgets.
 */
static HtmxResponse *
venture_web_api_runs(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GArray) tree = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	const gchar *organization;
	const gchar *limit;

	(void)params;

	gate = venture_web_require_module_api(self, "forge");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	organization = htmx_request_get_query_param(request, "organization_id");

	if (!venture_string_is_empty(organization))
		tree = venture_web_organization_tree(self,
			g_ascii_strtoll(organization, NULL, 10));

	limit = htmx_request_get_query_param(request, "limit");
	node = venture_factory_runs_describe(self->context,
		(NULL != tree) ? (const gint64 *)tree->data : NULL,
		(NULL != tree) ? tree->len : 0,
		htmx_request_get_query_param(request, "state"),
		(NULL != limit) ? (guint)g_ascii_strtoull(limit, NULL, 10) : 0,
		&error);

	if (NULL == node)
		return venture_web_error_response(error);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_budgets(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	(void)params;

	gate = venture_web_require_module_api(self, "forge");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	node = venture_factory_budgets_describe(self->context, &error);

	if (NULL == node)
		return venture_web_error_response(error);

	return venture_web_json_response(node, 200);
}

/* --- Bulk edits ------------------------------------------------------------ */

/*
 * POST /e/:type/bulk - ids, then either field and value, or action=delete.
 */
static HtmxResponse *
venture_web_ui_bulk(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *fallback = NULL;
	g_autofree gchar *back = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	GType entity_type;
	const gchar *type_name;
	const gchar *action;
	const gchar *field;
	const gchar *value;
	gint changed;

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
	fallback = g_strdup_printf("/e/%s", type_name);
	back = venture_web_desk_back(request, fallback);
	ids = venture_web_desk_parse_ids(htmx_request_get_form_value(request, "ids"),
	                                 &error);

	if (NULL == ids)
		return venture_web_error_response(error);

	action = htmx_request_get_form_value(request, "action");
	field = htmx_request_get_form_value(request, "field");
	value = htmx_request_get_form_value(request, "value");
	venture_auth_to_actor(principal, &actor);

	if (0 == g_strcmp0(action, "delete"))
	{
		changed = venture_desk_bulk_delete(self->context, entity_type,
		                                   (const gint64 *)ids->data, ids->len,
		                                   &actor, &error);
	}
	else
	{
		g_autoptr(JsonObject) changes = NULL;

		if (venture_string_is_empty(field))
		{
			g_set_error_literal(&error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "Choose a field to change");
			return venture_web_error_response(error);
		}

		changes = json_object_new();
		json_object_set_string_member(changes, field,
		                              (NULL != value) ? value : "");
		changed = venture_desk_bulk_update(self->context, entity_type,
		                                   (const gint64 *)ids->data, ids->len,
		                                   changes, &actor, &error);
	}

	if (changed < 0)
		return venture_web_desk_answer(request, back, error);

	return venture_web_desk_answer(request, back, NULL);
}

/*
 * POST /api/v1/:type/bulk - {"ids": [...], "changes": {...}} or
 * {"ids": [...], "delete": true}.
 */
static HtmxResponse *
venture_web_api_bulk(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	JsonObject *object;
	JsonArray *id_array;
	GType entity_type;
	gint changed;
	guint i;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != gate)
		return gate;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_resolve_type(self, params, &entity_type, &error))
		return venture_web_error_response(error);

	if (!venture_web_require_for_type(self, principal, entity_type,
	                                  VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);

	/* The audit log and the runs are evidence; see the generic writes. */
	if (!venture_web_type_accepts_writes(entity_type, &error))
		return venture_web_error_response(error);

	body = htmx_request_get_json(request, NULL);

	if ((NULL == body) || !JSON_NODE_HOLDS_OBJECT(body) ||
	    !json_object_has_member(json_node_get_object(body), "ids"))
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The body needs {\"ids\": [...]} and either "
		                    "\"changes\" or \"delete\": true");
		return venture_web_error_response(error);
	}

	object = json_node_get_object(body);
	id_array = json_object_get_array_member(object, "ids");
	ids = g_array_new(FALSE, FALSE, sizeof(gint64));

	for (i = 0; (NULL != id_array) && (i < json_array_get_length(id_array)); i++)
	{
		gint64 id;

		id = json_array_get_int_element(id_array, i);
		g_array_append_val(ids, id);
	}

	venture_auth_to_actor(principal, &actor);

	if (venture_json_object_get_bool(object, "delete", FALSE))
		changed = venture_desk_bulk_delete(self->context, entity_type,
		                                   (const gint64 *)ids->data, ids->len,
		                                   &actor, &error);
	else if (json_object_has_member(object, "changes") &&
	         JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "changes")))
		changed = venture_desk_bulk_update(self->context, entity_type,
		                                   (const gint64 *)ids->data, ids->len,
		                                   json_object_get_object_member(object,
		                                                                 "changes"),
		                                   &actor, &error);
	else
	{
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Give \"changes\" (an object) or \"delete\": true");
		return venture_web_error_response(error);
	}

	if (changed < 0)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "changed");
	json_builder_add_int_value(builder, changed);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * The bulk bar on a list page: hidden until a row is ticked, then a
 * field, a value and a Go. The field list is the editable fields of the
 * type, so a select is offered where the field is an enum.
 */
static void
venture_web_append_bulk_bar(
	VentureWebServer	*self,
	GString			*content,
	HtmxRequest		*request,
	const gchar		*type_name,
	VentureEntity		*prototype
){
	g_autoptr(GPtrArray) specs = NULL;
	const gchar *query;
	guint i;

	(void)self;
	query = htmx_request_get_query(request);
	specs = venture_entity_get_field_specs(prototype);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	g_string_append_printf(content,
		"<form method=\"post\" action=\"/e/%s/bulk\" class=\"bulk-bar\" "
		"data-bulk-bar hidden>"
		"<input type=\"hidden\" name=\"ids\" value=\"\" data-bulk-ids>"
		"<input type=\"hidden\" name=\"back\" value=\"/e/%s%s",
		type_name, type_name, venture_string_is_empty(query) ? "" : "?");

	if (!venture_string_is_empty(query))
		venture_html_escape_append(content, query);

	g_string_append(content, "\"><span class=\"bulk-count\" data-bulk-count>0"
	                         " selected</span>"
	                         "<select name=\"field\" data-bulk-field>");

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;
		VentureFieldKind kind;
		g_autofree gchar *options = NULL;

		spec = g_ptr_array_index(specs, i);
		kind = venture_field_spec_get_kind(spec);

		if (0 != (venture_field_spec_get_flags(spec) &
		          VENTURE_COLUMN_FLAG_SENSITIVE))
			continue;

		if ((VENTURE_FIELD_KIND_JSON == kind) ||
		    (VENTURE_FIELD_KIND_TEXT == kind))
			continue;

		/* An enum's choices ride along on the option, so the value box
		 * can turn into a select without a second request. */
		if (VENTURE_FIELD_KIND_ENUM == kind)
		{
			const gchar *const *choices;
			g_autoptr(GString) list = NULL;
			guint j;

			choices = venture_field_spec_get_choices(spec);
			list = g_string_new(NULL);

			for (j = 0; (NULL != choices) && (NULL != choices[j]); j++)
			{
				if (j > 0)
					g_string_append_c(list, ',');

				g_string_append(list, choices[j]);
			}

			options = g_string_free(g_steal_pointer(&list), FALSE);
		}

		g_string_append(content, "<option value=\"");
		venture_html_escape_append(content, venture_field_spec_get_name(spec));
		g_string_append(content, "\"");

		if (NULL != options)
		{
			g_string_append(content, " data-options=\"");
			venture_html_escape_append(content, options);
			g_string_append(content, "\"");
		}

		if (VENTURE_FIELD_KIND_BOOLEAN == kind)
			g_string_append(content, " data-options=\"true,false\"");

		g_string_append(content, ">");
		venture_html_escape_append(content, venture_field_spec_get_label(spec));
		g_string_append(content, "</option>");
	}

	g_string_append(content,
		"</select><span data-bulk-value-slot>"
		"<input type=\"text\" name=\"value\" placeholder=\"New value\" "
		"data-bulk-value></span>"
		"<button class=\"btn btn-primary btn-sm\" type=\"submit\" "
		"name=\"action\" value=\"update\">Apply</button>"
		"<button class=\"btn btn-sm btn-danger\" type=\"submit\" "
		"name=\"action\" value=\"delete\" data-bulk-delete>Delete</button>"
		"<button class=\"btn btn-sm btn-ghost\" type=\"button\" "
		"data-bulk-clear>Clear</button></form>");
}

/* --- The palette ----------------------------------------------------------- */

/*
 * GET /api/v1/palette?q= - what Ctrl+K offers: pages, record types with
 * their New action, and the records that match. The pages come from the
 * same navigation table as the sidebar, so the palette and the sidebar
 * cannot disagree about what exists.
 */
static HtmxResponse *
venture_web_api_palette(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_auto(GStrv) names = NULL;
	g_autofree gchar *needle = NULL;
	HtmxResponse *denied;
	const VentureWebNavLink *links;
	const gchar *q;
	guint records;
	gsize i;

	(void)params;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	principal = venture_auth_authenticate(self->auth, request);
	q = htmx_request_get_query_param(request, "q");
	needle = g_utf8_casefold((NULL != q) ? q : "", -1);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	/* Pages. */
	json_builder_set_member_name(builder, "pages");
	json_builder_begin_array(builder);
	links = venture_web_navigation();

	for (i = 0; NULL != links[i].path; i++)
	{
		g_autofree gchar *label = NULL;

		if (!venture_web_module_enabled(self, links[i].module))
			continue;

		label = g_utf8_casefold(links[i].label, -1);

		if (('\0' != needle[0]) && (NULL == strstr(label, needle)))
			continue;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, links[i].label);
		json_builder_set_member_name(builder, "url");
		json_builder_add_string_value(builder, links[i].path);
		json_builder_end_object(builder);
	}

	{
		static const struct
		{
			const gchar *label;
			const gchar *path;
			const gchar *module;
		} extra[] = {
			{ "Inbox", "/inbox", NULL },
			{ "Saved views", "/views", NULL },
			{ "Search", "/search", NULL }
		};

		for (i = 0; i < G_N_ELEMENTS(extra); i++)
		{
			g_autofree gchar *label = NULL;

			if (!venture_web_module_enabled(self, extra[i].module))
				continue;

			label = g_utf8_casefold(extra[i].label, -1);

			if (('\0' != needle[0]) && (NULL == strstr(label, needle)))
				continue;

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "label");
			json_builder_add_string_value(builder, extra[i].label);
			json_builder_set_member_name(builder, "url");
			json_builder_add_string_value(builder, extra[i].path);
			json_builder_end_object(builder);
		}
	}

	json_builder_end_array(builder);

	/* Record types, each with its list and its New. */
	json_builder_set_member_name(builder, "types");
	json_builder_begin_array(builder);
	names = venture_entity_registry_list_names(
		venture_context_get_entity_registry(self->context));

	for (i = 0; NULL != names[i]; i++)
	{
		g_autofree gchar *label = NULL;
		g_autofree gchar *folded = NULL;
		g_autofree gchar *list_url = NULL;
		g_autofree gchar *new_url = NULL;
		GType entity_type;

		entity_type = venture_entity_registry_lookup(
			venture_context_get_entity_registry(self->context), names[i]);

		if ((G_TYPE_INVALID == entity_type) ||
		    !venture_web_require_for_type(self, principal, entity_type,
		                                  VENTURE_USER_ROLE_VIEWER, NULL))
			continue;

		label = venture_web_label_from_name(names[i]);
		folded = g_utf8_casefold(label, -1);

		if (('\0' != needle[0]) && (NULL == strstr(folded, needle)))
			continue;

		list_url = g_strdup_printf("/e/%s", names[i]);
		new_url = g_strdup_printf("/e/%s/new", names[i]);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, names[i]);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, label);
		json_builder_set_member_name(builder, "url");
		json_builder_add_string_value(builder, list_url);
		json_builder_set_member_name(builder, "new_url");
		json_builder_add_string_value(builder, new_url);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	/* Records, a few per type, only with a term to look for. */
	json_builder_set_member_name(builder, "records");
	json_builder_begin_array(builder);
	records = 0;

	for (i = 0; ('\0' != needle[0]) && (NULL != names[i]) && (records < 24); i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) hits = NULL;
		GType entity_type;
		guint j;

		entity_type = venture_entity_registry_lookup(
			venture_context_get_entity_registry(self->context), names[i]);

		/* Bookkeeping rows are not places to go: a notification points
		 * at its record, a watch is a flag, a chunk is a passage. */
		if ((G_TYPE_INVALID == entity_type) ||
		    (VENTURE_TYPE_AUDIT_ENTRY == entity_type) ||
		    (VENTURE_TYPE_NOTIFICATION == entity_type) ||
		    (VENTURE_TYPE_WATCH == entity_type) ||
		    (VENTURE_TYPE_KB_CHUNK == entity_type) ||
		    (VENTURE_TYPE_KB_LINK == entity_type) ||
		    (VENTURE_TYPE_LEDGER_ENTRY == entity_type) ||
		    !venture_web_require_for_type(self, principal, entity_type,
		                                  VENTURE_USER_ROLE_VIEWER, NULL))
			continue;

		query = venture_query_new(entity_type);
		venture_query_set_search(query, q);
		venture_web_scope_to_active_organization(self, request, query);
		venture_query_set_limit(query, 4);
		hits = venture_database_find(venture_context_get_database(self->context),
		                             query, NULL);

		for (j = 0; (NULL != hits) && (j < hits->len); j++)
		{
			VentureEntity *hit;
			g_autofree gchar *label = NULL;
			g_autofree gchar *url = NULL;

			hit = g_ptr_array_index(hits, j);
			label = venture_entity_get_display_name(hit);
			url = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, names[i],
			                      venture_entity_get_id(hit));

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "type");
			json_builder_add_string_value(builder, names[i]);
			json_builder_set_member_name(builder, "label");
			json_builder_add_string_value(builder, label);
			json_builder_set_member_name(builder, "url");
			json_builder_add_string_value(builder, url);
			json_builder_end_object(builder);
			records++;
		}
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}


/* ==========================================================================
 * Webhooks out, and the assistant at the ticket desk
 * ========================================================================== */

/*
 * GET /webhooks - what is subscribed, how it is doing, and what went out.
 */
static HtmxResponse *
venture_web_ui_webhooks(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	JsonArray *webhooks;
	const gchar *secret;
	guint i;

	(void)params;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "webhooks");

		if (NULL != gate)
			return gate;
	}

	redirect = venture_web_ui_require_session(self, request);

	if (NULL != redirect)
		return redirect;

	principal = venture_auth_authenticate(self->auth, request);

	/* A webhook names the host this install's business data is posted
	 * to, and holds the secret that signs it. Same reasoning as a
	 * forge: that is access management, not data entry. */
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER,
	                          &error))
		return venture_web_error_response(error);

	node = venture_webhook_describe(self->context, &error);

	if (NULL == node)
		return venture_web_error_response(error);

	webhooks = json_node_get_array(node);

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Webhooks</h1><span class=\"subtitle\">"
	                       "Telling something outside that a record changed"
	                       "</span></div><div class=\"page-actions\">"
	                       "<a class=\"btn btn-primary\" href=\"/e/webhook/new\">"
	                       "New webhook</a></div></div>");

	/* A freshly generated secret, shown exactly once. */
	secret = htmx_request_get_query_param(request, "secret");

	if (!venture_string_is_empty(secret))
	{
		g_string_append(content,
			"<div class=\"notice warning\"><p><strong>The signing secret, "
			"shown once.</strong> Paste it into the receiving end now; it "
			"is stored hashed nowhere and shown again never.</p>"
			"<pre class=\"secret\" data-copy>");
		venture_html_escape_append(content, secret);
		g_string_append(content, "</pre></div>");
	}

	if (0 == json_array_get_length(webhooks))
	{
		g_string_append(content,
			"<div class=\"empty\"><h3>Nothing is listening</h3>"
			"<p class=\"muted\">A webhook posts a signed JSON body to a URL "
			"every time a record it subscribes to changes. Name the events "
			"as <code>ticket.created, invoice.*</code> or <code>*</code> for "
			"everything.</p></div>");
	}

	for (i = 0; i < json_array_get_length(webhooks); i++)
	{
		JsonObject *webhook;
		JsonArray *deliveries;
		gint64 id;
		gint64 failures;
		gboolean active;
		guint j;

		webhook = json_array_get_object_element(webhooks, i);
		id = venture_json_object_get_int(webhook, "id", 0);
		failures = venture_json_object_get_int(webhook, "failure_count", 0);
		active = venture_json_object_get_bool(webhook, "active", FALSE);
		deliveries = json_object_get_array_member(webhook, "deliveries");

		g_string_append(content, "<div class=\"card\"><div class=\"card-head\">"
		                         "<h2>");
		g_string_append_printf(content, "<a href=\"/e/webhook/%" G_GINT64_FORMAT
		                                "\">", id);
		venture_html_escape_append(content,
			venture_json_object_get_string(webhook, "name", ""));
		g_string_append_printf(content, "</a> <span class=\"badge %s\">%s</span>",
			active ? "positive" : "", active ? "active" : "off");

		if (!venture_json_object_get_bool(webhook, "signed", FALSE))
			g_string_append(content, " <span class=\"badge warning\">"
			                         "unsigned</span>");

		if (failures > 0)
			g_string_append_printf(content,
				" <span class=\"badge negative\">%" G_GINT64_FORMAT
				" failed in a row</span>", failures);

		g_string_append(content, "</h2><div class=\"desk-actions\">");
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/webhooks/%" G_GINT64_FORMAT
			"/test\" class=\"inline\">"
			"<button class=\"btn btn-sm\" type=\"submit\">Test</button></form>"
			"<form method=\"post\" action=\"/webhooks/%" G_GINT64_FORMAT
			"/secret\" class=\"inline\">"
			"<button class=\"btn btn-sm\" type=\"submit\" "
			"title=\"Generate a new signing secret; the old one stops "
			"working at once\">New secret</button></form>", id, id);
		g_string_append(content, "</div></div><div class=\"card-body\">");

		g_string_append(content, "<dl class=\"detail detail-grid\"><dt>URL</dt>"
		                         "<dd><code>");
		venture_html_escape_append(content,
			venture_json_object_get_string(webhook, "url", ""));
		g_string_append(content, "</code></dd><dt>Events</dt><dd><code>");
		venture_html_escape_append(content,
			venture_json_object_get_string(webhook, "events", "*"));
		g_string_append_printf(content, "</code></dd><dt>Body</dt><dd>%s</dd>"
		                                "</dl>",
			venture_json_object_get_bool(webhook, "include_record", FALSE)
				? "the envelope and the whole record"
				: "the envelope only");

		if (0 == json_array_get_length(deliveries))
		{
			g_string_append(content, "<p class=\"muted\">Nothing has gone out "
			                         "yet. Press Test to try it.</p>");
		}
		else
		{
			g_string_append(content, "<div class=\"table-wrap\">"
			                         "<table class=\"data\"><thead><tr>"
			                         "<th>Event</th><th>State</th>"
			                         "<th class=\"num\">Status</th>"
			                         "<th class=\"num\">Took</th>"
			                         "<th>When</th><th>Detail</th>"
			                         "</tr></thead><tbody>");

			for (j = 0; j < json_array_get_length(deliveries); j++)
			{
				JsonObject *delivery;
				const gchar *state;
				const gchar *failure;

				delivery = json_array_get_object_element(deliveries, j);
				state = venture_json_object_get_string(delivery, "state", "");
				failure = venture_json_object_get_string(delivery,
				                                         "failure_reason", NULL);

				g_string_append_printf(content,
					"<tr data-href=\"/e/webhook_delivery/%" G_GINT64_FORMAT
					"\"><td><code>",
					venture_json_object_get_int(delivery, "id", 0));
				venture_html_escape_append(content,
					venture_json_object_get_string(delivery, "event", ""));
				g_string_append_printf(content,
					"</code></td><td><span class=\"badge %s\">%s</span></td>"
					"<td class=\"num\">%" G_GINT64_FORMAT "</td>"
					"<td class=\"num\">%" G_GINT64_FORMAT " ms</td><td>",
					(0 == g_strcmp0(state, "succeeded")) ? "positive"
					                                     : "negative",
					state,
					venture_json_object_get_int(delivery, "status_code", 0),
					venture_json_object_get_int(delivery, "duration_ms", 0));
				venture_html_escape_append(content,
					venture_json_object_get_string(delivery, "attempted_at",
					                               ""));
				g_string_append(content, "</td><td class=\"muted\">");

				if (!venture_string_is_empty(failure))
					venture_html_escape_append(content, failure);

				g_string_append(content, "</td></tr>");
			}

			g_string_append(content, "</tbody></table></div>");
		}

		g_string_append(content, "</div></div>");
	}

	return venture_web_html_response(
		venture_web_page(self, request, "/webhooks", "Webhooks",
		                 content->str), 200);
}

/*
 * Loads a webhook for one of the two owner-only actions on it.
 */
static HtmxResponse *
venture_web_webhook_load(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	GHashTable		 *params,
	gboolean		  api,
	VentureAuthPrincipal	**out_principal,
	VentureEntity		**out_webhook
){
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = api ? venture_web_require_module_api(self, "webhooks")
	           : venture_web_require_module_ui(self, request, "webhooks");

	if (NULL != gate)
		return gate;

	if (api)
	{
		gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_OWNER);

		if (NULL != gate)
			return gate;
	}
	else
	{
		gate = venture_web_ui_require_session(self, request);

		if (NULL != gate)
			return gate;
	}

	*out_principal = venture_auth_authenticate(self->auth, request);

	if (!venture_auth_require(self->auth, *out_principal,
	                          VENTURE_USER_ROLE_OWNER, &error))
		return venture_web_error_response(error);

	*out_webhook = venture_database_get(
		venture_context_get_database(self->context), VENTURE_TYPE_WEBHOOK,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);

	if (NULL == *out_webhook)
		return venture_web_error_response(error);

	return NULL;
}

/*
 * POST /webhooks/:id/test - send a ping and wait for the answer.
 */
static HtmxResponse *
venture_web_ui_webhook_test(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) webhook = NULL;
	g_autoptr(VentureEntity) delivery = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = venture_web_webhook_load(self, request, params, FALSE, &principal,
	                                &webhook);

	if (NULL != gate)
		return gate;

	delivery = venture_webhook_test(self->context, webhook, &error);

	if (NULL == delivery)
		return venture_web_error_response(error);

	return venture_web_redirect_to("/webhooks");
}

/*
 * POST /webhooks/:id/secret - generate one, and show it once.
 */
static HtmxResponse *
venture_web_ui_webhook_secret(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) webhook = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *destination = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;

	gate = venture_web_webhook_load(self, request, params, FALSE, &principal,
	                                &webhook);

	if (NULL != gate)
		return gate;

	venture_auth_to_actor(principal, &actor);
	secret = venture_webhook_set_secret(self->context, webhook,
		htmx_request_get_form_value(request, "secret"), &actor, &error);

	if (NULL == secret)
		return venture_web_error_response(error);

	/*
	 * Carried back in the query string, which is the same trade the
	 * token page makes: it is in this one redirect and nowhere else,
	 * and the alternative is a page that mints a secret every time
	 * somebody refreshes it.
	 */
	escaped = g_uri_escape_string(secret, NULL, FALSE);
	destination = g_strdup_printf("/webhooks?secret=%s", escaped);

	return venture_web_redirect_to(destination);
}

/*
 * GET /api/v1/webhooks, POST /api/v1/webhooks/:id/test and /secret.
 */
static HtmxResponse *
venture_web_api_webhooks(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	(void)params;

	gate = venture_web_require_module_api(self, "webhooks");

	if (NULL != gate)
		return gate;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_OWNER);

	if (NULL != gate)
		return gate;

	node = venture_webhook_describe(self->context, &error);

	if (NULL == node)
		return venture_web_error_response(error);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_webhook_test(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) webhook = NULL;
	g_autoptr(VentureEntity) delivery = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = venture_web_webhook_load(self, request, params, TRUE, &principal,
	                                &webhook);

	if (NULL != gate)
		return gate;

	delivery = venture_webhook_test(self->context, webhook, &error);

	if (NULL == delivery)
		return venture_web_error_response(error);

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(delivery), FALSE);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_webhook_secret(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) webhook = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *secret = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;

	gate = venture_web_webhook_load(self, request, params, TRUE, &principal,
	                                &webhook);

	if (NULL != gate)
		return gate;

	body = htmx_request_get_json(request, NULL);
	venture_auth_to_actor(principal, &actor);
	secret = venture_webhook_set_secret(self->context, webhook,
		((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
			? venture_json_object_get_string(json_node_get_object(body),
			                                 "secret", NULL)
			: NULL,
		&actor, &error);

	if (NULL == secret)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "secret");
	json_builder_add_string_value(builder, secret);
	json_builder_set_member_name(builder, "note");
	json_builder_add_string_value(builder,
		"This is the only time the secret is shown. Deliveries carry it as "
		"an HMAC-SHA256 of the body in X-Venture-Signature.");
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/* --- The assistant at the desk --------------------------------------------- */

/*
 * The triage card: what the model proposes, and the button that applies
 * it. Rendered as a fragment so the Desk can swap it in without the
 * page, and as a form so applying is an ordinary POST.
 */
static void
venture_web_append_triage_card(
	GString		*content,
	gint64		 ticket_id,
	JsonObject	*proposal
){
	const gchar *sentiment;
	JsonArray *tags;
	guint i;

	g_string_append(content, "<div class=\"assist-card\" id=\"assist\">");
	g_string_append(content, "<div class=\"assist-head\"><span class=\"badge "
	                         "accent\">suggested</span>");
	sentiment = venture_json_object_get_string(proposal, "sentiment", NULL);

	if (!venture_string_is_empty(sentiment))
	{
		g_string_append_printf(content, "<span class=\"badge %s\">",
			(0 == g_strcmp0(sentiment, "angry")) ? "negative"
			: (0 == g_strcmp0(sentiment, "frustrated")) ? "warning" : "");
		venture_html_escape_append(content, sentiment);
		g_string_append(content, "</span>");
	}

	g_string_append(content, "</div>");

	g_string_append(content, "<p class=\"assist-summary\">");
	venture_html_escape_append(content,
		venture_json_object_get_string(proposal, "summary", ""));
	g_string_append(content, "</p>");

	g_string_append_printf(content,
		"<form method=\"post\" action=\"/tickets/%" G_GINT64_FORMAT
		"/triage\" class=\"assist-form\">", ticket_id);

	g_string_append(content, "<label>Priority <input type=\"text\" "
	                         "name=\"priority\" value=\"");
	venture_html_escape_append(content,
		venture_json_object_get_string(proposal, "priority", ""));
	g_string_append(content, "\" size=\"8\"></label>");

	g_string_append(content, "<label>Type <input type=\"text\" "
	                         "name=\"issue_type\" value=\"");
	venture_html_escape_append(content,
		venture_json_object_get_string(proposal, "issue_type", ""));
	g_string_append(content, "\" size=\"8\"></label>");

	g_string_append(content, "<label>Tags <input type=\"text\" name=\"tags\" "
	                         "value=\"");
	tags = json_object_has_member(proposal, "tags")
		? json_object_get_array_member(proposal, "tags") : NULL;

	for (i = 0; (NULL != tags) && (i < json_array_get_length(tags)); i++)
	{
		if (i > 0)
			g_string_append(content, ", ");

		venture_html_escape_append(content,
		                           json_array_get_string_element(tags, i));
	}

	g_string_append(content, "\"></label>"
	                         "<button class=\"btn btn-sm btn-primary\" "
	                         "type=\"submit\">Apply</button></form>");

	{
		const gchar *reasoning;

		reasoning = venture_json_object_get_string(proposal, "reasoning", NULL);

		if (!venture_string_is_empty(reasoning))
		{
			g_string_append(content, "<p class=\"assist-why muted\">");
			venture_html_escape_append(content, reasoning);
			g_string_append(content, "</p>");
		}
	}

	g_string_append(content, "</div>");
}

/*
 * GET /tickets/:id/assist?what=triage|summary|draft - the fragment the
 * Desk's three buttons swap in. A model call takes seconds, so this is
 * its own request rather than something the ticket page waits for.
 */
static HtmxResponse *
venture_web_ui_ticket_assist(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *denied;
	const gchar *what;
	gint64 ticket_id;

	{
		HtmxResponse *gate;

		gate = venture_web_require_module_ui(self, request, "tickets");

		if (NULL != gate)
			return gate;
	}

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != denied)
		return denied;

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	ticket = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_TICKET, ticket_id, &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	what = htmx_request_get_query_param(request, "what");
	content = g_string_new(NULL);

	if (0 == g_strcmp0(what, "triage"))
	{
		g_autoptr(JsonNode) proposal = NULL;

		proposal = venture_ai_assist_triage(self->context, ticket, &error);

		if (NULL == proposal)
		{
			g_string_append(content, "<div class=\"assist-card\" id=\"assist\">"
			                         "<p class=\"negative\">");
			venture_html_escape_append(content, error->message);
			g_string_append(content, "</p></div>");
		}
		else
		{
			venture_web_append_triage_card(content, ticket_id,
			                               json_node_get_object(proposal));
		}
	}
	else
	{
		g_autofree gchar *text = NULL;

		text = (0 == g_strcmp0(what, "draft"))
			? venture_ai_assist_draft_reply(self->context, ticket,
				htmx_request_get_query_param(request, "instruction"), &error)
			: venture_ai_assist_summarise(self->context, ticket, &error);

		g_string_append(content, "<div class=\"assist-card\" id=\"assist\">");

		if (NULL == text)
		{
			g_string_append(content, "<p class=\"negative\">");
			venture_html_escape_append(content, error->message);
			g_string_append(content, "</p>");
		}
		else
		{
			g_string_append_printf(content,
				"<div class=\"assist-head\"><span class=\"badge accent\">%s"
				"</span><button class=\"btn btn-sm\" type=\"button\" "
				"data-assist-copy>Copy</button>",
				(0 == g_strcmp0(what, "draft")) ? "draft reply" : "summary");

			/* A drafted reply goes into the composer rather than out to
			 * anybody: this button fills the box a person then edits. */
			if (0 == g_strcmp0(what, "draft"))
				g_string_append(content,
					"<button class=\"btn btn-sm btn-primary\" type=\"button\" "
					"data-assist-use>Use it</button>");

			g_string_append(content, "</div><div class=\"assist-text\" "
			                         "data-assist-body>");
			venture_html_escape_append(content, text);
			g_string_append(content, "</div>");
		}

		g_string_append(content, "</div>");
	}

	return venture_web_html_response(g_strdup(content->str), 200);
}

/*
 * POST /tickets/:id/triage - apply what was proposed, as edited.
 */
static HtmxResponse *
venture_web_ui_ticket_triage(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) proposal = NULL;
	g_autofree gchar *back = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *priority;
	const gchar *issue_type;
	const gchar *tags;
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

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	ticket = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_TICKET, ticket_id, &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	priority = htmx_request_get_form_value(request, "priority");
	issue_type = htmx_request_get_form_value(request, "issue_type");
	tags = htmx_request_get_form_value(request, "tags");

	/* Rebuilt into the shape the applier takes, so the form and an
	 * agent's proposal go through one code path. */
	builder = json_builder_new();
	json_builder_begin_object(builder);

	if (!venture_string_is_empty(priority))
	{
		json_builder_set_member_name(builder, "priority");
		json_builder_add_string_value(builder, priority);
	}

	if (!venture_string_is_empty(issue_type))
	{
		json_builder_set_member_name(builder, "issue_type");
		json_builder_add_string_value(builder, issue_type);
	}

	if (!venture_string_is_empty(tags))
	{
		g_auto(GStrv) parts = NULL;
		gsize i;

		json_builder_set_member_name(builder, "tags");
		json_builder_begin_array(builder);
		parts = g_strsplit(tags, ",", -1);

		for (i = 0; NULL != parts[i]; i++)
		{
			g_autofree gchar *tag = NULL;

			tag = g_strstrip(g_strdup(parts[i]));

			if (!venture_string_is_empty(tag))
				json_builder_add_string_value(builder, tag);
		}

		json_builder_end_array(builder);
	}

	json_builder_end_object(builder);
	proposal = json_builder_get_root(builder);

	if (venture_ai_assist_apply_triage(self->context, ticket, proposal, &error))
	{
		venture_auth_to_actor(principal, &actor);

		if (!venture_database_save(venture_context_get_database(self->context),
		                           ticket, &actor, &error))
			return venture_web_error_response(error);
	}

	back = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	return venture_web_desk_answer(request, back, error);
}

/*
 * POST /tickets/:id/satisfaction - what whoever raised it made of it.
 */
static HtmxResponse *
venture_web_ui_ticket_satisfaction(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autofree gchar *back = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *redirect;
	VentureActor actor;
	const gchar *rating;
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

	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR,
	                          &error))
		return venture_web_error_response(error);

	ticket_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	ticket = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_TICKET, ticket_id, &error);

	if (NULL == ticket)
		return venture_web_error_response(error);

	rating = htmx_request_get_form_value(request, "satisfaction");

	if (!venture_string_is_empty(rating) &&
	    !venture_entity_set_field_from_string(ticket, "satisfaction", rating,
	                                          &error))
		return venture_web_error_response(error);

	{
		const gchar *comment;

		comment = htmx_request_get_form_value(request, "comment");

		if (NULL != comment)
			g_object_set(ticket, "satisfaction-comment", comment, NULL);
	}

	venture_auth_to_actor(principal, &actor);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           ticket, &actor, &error))
		return venture_web_error_response(error);

	back = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ticket_id);

	return venture_web_desk_answer(request, back, error);
}

/*
 * The API forms: triage (optionally applied), summary, draft, rating.
 */
static HtmxResponse *
venture_web_api_ticket_triage(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(JsonNode) proposal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	gboolean apply;
	gboolean changed = FALSE;

	gate = venture_web_api_ticket_load(self, request, params,
	                                   VENTURE_USER_ROLE_EDITOR, &principal,
	                                   &ticket);

	if (NULL != gate)
		return gate;

	proposal = venture_ai_assist_triage(self->context, ticket, &error);

	if (NULL == proposal)
		return venture_web_error_response(error);

	body = htmx_request_get_json(request, NULL);
	apply = ((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
		? venture_json_object_get_bool(json_node_get_object(body), "apply",
		                               FALSE)
		: FALSE;

	if (apply)
	{
		changed = venture_ai_assist_apply_triage(self->context, ticket,
		                                         proposal, &error);

		if (changed)
		{
			venture_auth_to_actor(principal, &actor);

			if (!venture_database_save(
				venture_context_get_database(self->context), ticket, &actor,
				&error))
				return venture_web_error_response(error);
		}
	}

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "triage");
	json_builder_add_value(builder, json_node_ref(proposal));
	json_builder_set_member_name(builder, "applied");
	json_builder_add_boolean_value(builder, apply && changed);
	json_builder_set_member_name(builder, "ticket");
	json_builder_add_value(builder,
		venture_serializable_to_json(VENTURE_SERIALIZABLE(ticket), FALSE));
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_api_ticket_summary(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = venture_web_api_ticket_load(self, request, params,
	                                   VENTURE_USER_ROLE_VIEWER, &principal,
	                                   &ticket);

	if (NULL != gate)
		return gate;

	text = venture_ai_assist_summarise(self->context, ticket, &error);

	if (NULL == text)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "summary");
	json_builder_add_string_value(builder, text);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}


static HtmxResponse *
venture_web_api_ticket_draft(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = venture_web_api_ticket_load(self, request, params,
	                                   VENTURE_USER_ROLE_EDITOR, &principal,
	                                   &ticket);

	if (NULL != gate)
		return gate;

	body = htmx_request_get_json(request, NULL);
	text = venture_ai_assist_draft_reply(self->context, ticket,
		((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
			? venture_json_object_get_string(json_node_get_object(body),
			                                 "instruction", NULL)
			: NULL,
		&error);

	if (NULL == text)
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "draft");
	json_builder_add_string_value(builder, text);
	json_builder_set_member_name(builder, "posted");
	json_builder_add_boolean_value(builder, FALSE);
	json_builder_set_member_name(builder, "note");
	json_builder_add_string_value(builder,
		"A draft, never posted. Add it with POST /api/v1/ticket_comment "
		"once somebody has read it.");
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}


#include "venture-web-federation.inc"
#include "reconciliation/venture-reconciliation-web.inc"
#include "activities/venture-activity-web.inc"
#include "banking/venture-bank-web.inc"
#include "cutover/venture-cutover-web.inc"
#include "setup/venture-setup-web.inc"
#include "documents/venture-document-web.inc"
#include "portal/venture-portal-web.inc"
#include "autojournal/venture-autojournal-web.inc"

#include "mail/venture-mail-web.inc"
#include "quotes/venture-quote-web.inc"
#include "pipelines/venture-pipeline-web.inc"

#include "leads/venture-lead-web.inc"
#include "close/venture-close-web.inc"
#include "tax/venture-tax-web.inc"
#include "capture/venture-capture-web.inc"
#include "accounting/venture-accounting-web.inc"
#include "bankfeed/venture-bankfeed-web.inc"

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
	htmx_router_get(router, "/overview", venture_web_ui_overview, self);

	/* Dashboards. The literal paths go before the :slug ones, because the
	 * router takes the first match. */
	htmx_router_get(router, "/dashboards", venture_web_ui_dashboards, self);
	htmx_router_post(router, "/dashboards", venture_web_ui_dashboard_create,
	                 self);
	htmx_router_post(router, "/dashboards/import",
	                 venture_web_ui_dashboard_import, self);
	htmx_router_get(router, "/dashboards/:slug", venture_web_ui_dashboard_view,
	                self);
	htmx_router_post(router, "/dashboards/:slug",
	                 venture_web_ui_dashboard_update, self);
	htmx_router_get(router, "/dashboards/:slug/edit",
	                venture_web_ui_dashboard_edit, self);
	htmx_router_get(router, "/dashboards/:slug/export",
	                venture_web_ui_dashboard_export, self);
	htmx_router_post(router, "/dashboards/:slug/delete",
	                 venture_web_ui_dashboard_delete, self);
	htmx_router_get(router, "/dashboards/:slug/widgets/new",
	                venture_web_ui_dashboard_widget_form, self);
	htmx_router_post(router, "/dashboards/:slug/widgets",
	                 venture_web_ui_dashboard_widget_save, self);
	htmx_router_get(router, "/dashboards/:slug/widgets/:id",
	                venture_web_ui_dashboard_widget, self);
	htmx_router_get(router, "/dashboards/:slug/widgets/:id/edit",
	                venture_web_ui_dashboard_widget_form, self);
	htmx_router_post(router, "/dashboards/:slug/widgets/:id",
	                 venture_web_ui_dashboard_widget_save, self);
	htmx_router_post(router, "/dashboards/:slug/widgets/:id/delete",
	                 venture_web_ui_dashboard_widget_delete, self);
	htmx_router_post(router, "/dashboards/:slug/widgets/:id/move",
	                 venture_web_ui_dashboard_widget_move, self);
	htmx_router_post(router, "/dashboards/:slug/widgets/:id/swap",
	                 venture_web_ui_dashboard_widget_swap, self);
	htmx_router_post(router, "/dashboards/:slug/widgets/:id/place",
	                 venture_web_ui_dashboard_widget_place, self);
	htmx_router_post(router, "/dashboards/:slug/arrange",
	                 venture_web_ui_dashboard_arrange, self);
	htmx_router_get(router, "/search", venture_web_ui_search, self);

	/* The workdesk. */
	htmx_router_get(router, "/inbox", venture_web_ui_inbox, self);
	htmx_router_post(router, "/inbox/read", venture_web_ui_inbox_read, self);
	htmx_router_get(router, "/inbox/count", venture_web_ui_inbox_count, self);
	htmx_router_post(router, "/watch", venture_web_ui_watch, self);
	htmx_router_get(router, "/views", venture_web_ui_views, self);
	htmx_router_post(router, "/views", venture_web_ui_view_create, self);
	htmx_router_get(router, "/views/:id", venture_web_ui_view_open, self);
	htmx_router_post(router, "/views/:id/delete", venture_web_ui_view_delete,
	                 self);
	htmx_router_post(router, "/tickets/:id/macro", venture_web_ui_ticket_macro,
	                 self);
	htmx_router_post(router, "/tickets/:id/worklog",
	                 venture_web_ui_ticket_worklog, self);
	htmx_router_post(router, "/tickets/:id/assign-me",
	                 venture_web_ui_ticket_assign_me, self);
	htmx_router_get(router, "/sprints", venture_web_ui_sprints, self);
	htmx_router_get(router, "/runs", venture_web_ui_runs, self);
	htmx_router_get(router, "/runs/table", venture_web_ui_runs_table, self);
	htmx_router_post(router, "/incidents/:id/ticket",
	                 venture_web_ui_incident_ticket, self);
	htmx_router_post(router, "/e/:type/bulk", venture_web_ui_bulk, self);
	htmx_router_get(router, "/webhooks", venture_web_ui_webhooks, self);
	htmx_router_post(router, "/webhooks/:id/test", venture_web_ui_webhook_test,
	                 self);
	htmx_router_post(router, "/webhooks/:id/secret",
	                 venture_web_ui_webhook_secret, self);
	htmx_router_get(router, "/tickets/:id/assist", venture_web_ui_ticket_assist,
	                self);
	htmx_router_post(router, "/tickets/:id/triage",
	                 venture_web_ui_ticket_triage, self);
	htmx_router_post(router, "/tickets/:id/satisfaction",
	                 venture_web_ui_ticket_satisfaction, self);
	htmx_router_get(router, "/automations", venture_web_ui_automations, self);
	htmx_router_post(router, "/automations/validate",
	                 venture_web_ui_automations_validate, self);
	htmx_router_post(router, "/automations/save",
	                 venture_web_ui_automations_save, self);
	htmx_router_post(router, "/automations/reload",
	                 venture_web_ui_automations_reload, self);
	htmx_router_get(router, "/plugins", venture_web_ui_plugins, self);
	htmx_router_get(router, "/modules", venture_web_ui_modules, self);
	htmx_router_post(router, "/plugins/config",
	                 venture_web_ui_plugins_config, self);
	htmx_router_post(router, "/invoices/:id/checkout", venture_web_stripe_checkout, self);
	htmx_router_post(router, "/api/v1/invoices/:id/checkout", venture_web_stripe_checkout, self);
	htmx_router_post(router, "/webhooks/stripe", venture_web_stripe_webhook, self);
	htmx_router_post(router, "/invoices/:id/status",
	                 venture_web_ui_invoice_status, self);
	htmx_router_post(router, "/api/v1/vendor_bill/:id/:action", venture_web_payables_action, self);
	htmx_router_post(router, "/bills/:id/:action", venture_web_payables_action, self);
	htmx_router_get(router, "/payables", venture_web_payables_workbench, self);
	htmx_router_post(router, "/payables/pay", venture_web_payables_workbench_pay, self);
	htmx_router_post(router, "/api/v1/payables/pay", venture_web_payables_workbench, self);
	htmx_router_get(router, "/claims", venture_web_claims_workbench, self);
	htmx_router_post(router, "/claims/:id/:action", venture_web_claims_action, self);
	htmx_router_post(router, "/api/v1/expense_claim/:id/:action", venture_web_claims_action, self);
	htmx_router_get(router, "/payroll", venture_web_payroll_workbench, self);
	htmx_router_post(router, "/api/v1/payroll/import", venture_web_payroll_action, self);
	htmx_router_post(router, "/api/v1/payroll_run/:id/:action", venture_web_payroll_action, self);
	htmx_router_post(router, "/payroll/:id/:action", venture_web_payroll_action, self);
	htmx_router_get(router, "/purchasing", venture_web_purchasing_workbench, self);
	htmx_router_post(router, "/purchase_order/:id/:action", venture_web_goods_action, self);
	htmx_router_post(router, "/api/v1/purchase_order/:id/:action", venture_web_goods_action, self);
	htmx_router_post(router, "/sales_order/:id/:action", venture_web_goods_action, self);
	htmx_router_post(router, "/api/v1/sales_order/:id/:action", venture_web_goods_action, self);
	htmx_router_get(router, "/close", close_ui, self);
	htmx_router_post(router, "/api/v1/close/open", close_api, self);
	htmx_router_post(router, "/api/v1/close/:id/:action", close_api, self);
	htmx_router_get(router, "/api/v1/close/:id/pack", close_api, self);
	htmx_router_get(router, "/tax-filings", tax_filings_ui, self);
	htmx_router_post(router, "/api/v1/tax-filings/prepare", tax_filing_api, self);
	htmx_router_post(router, "/api/v1/tax-filings/:id/:action", tax_filing_api, self);
	htmx_router_post(router, "/api/v1/contractor-tax/prepare", contractor_tax_api, self);
	htmx_router_post(router, "/api/v1/contractor-tax/:id/:action", contractor_tax_api, self);
	htmx_router_get(router, "/api/v1/contractor-tax/:id/export", contractor_tax_api, self);
	htmx_router_get(router, "/capture", capture_ui, self);
	htmx_router_post(router, "/api/v1/capture", capture_api, self);
	htmx_router_post(router, "/api/v1/capture/:id/:action", capture_api, self);
	htmx_router_get(router, "/accounting", accounting_ui_home, self);
	htmx_router_get(router, "/api/v1/accounting/home", accounting_api_home, self);
	htmx_router_get(router, "/invoices/:id/print",
	                 venture_web_ui_invoice_print, self);
	htmx_router_get(router, "/quotes/:id/print", quote_route, self);
	htmx_router_post(router, "/quotes/:id/:action", quote_route, self);
	htmx_router_post(router, "/api/v1/quotes/:id/:action", quote_route, self);
	htmx_router_get(router, "/q/:token", quote_public, self);
	htmx_router_post(router, "/q/:token", quote_public, self);
	htmx_router_post(router, "/q/:token/accept", quote_public, self);
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
	htmx_router_post(router, "/links", venture_web_ui_link_create, self);
	htmx_router_get(router, "/factory", venture_web_ui_factory, self);
	htmx_router_post(router, "/releases/:id/changelog",
	                 venture_web_ui_release_changelog, self);
	htmx_router_post(router, "/releases/:id/publish",
	                 venture_web_ui_release_publish, self);
	htmx_router_post(router, "/links/:id/delete", venture_web_ui_link_delete,
	                 self);
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
	htmx_router_get(router, "/kb", venture_web_ui_kb, self);
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
	htmx_router_post(router, "/look", venture_web_ui_look, self);
	htmx_router_post(router, "/ui/chat", venture_web_ui_chat, self);
	htmx_router_get(router, "/ui/chat/threads", venture_web_ui_chat_threads,
	                self);
	htmx_router_get(router, "/ui/chat/thread/:id", venture_web_ui_chat_thread,
	                self);
	htmx_router_get(router, "/assistant", venture_web_ui_assistant, self);
	htmx_router_get(router, "/ui/models", venture_web_ui_models, self);
	htmx_router_get(router, "/harness", venture_web_ui_harness, self);
	htmx_router_post(router, "/harness", venture_web_ui_harness_open, self);
	htmx_router_get(router, "/harness/:id", venture_web_ui_harness_session,
	                self);
	htmx_router_post(router, "/harness/:id/send", venture_web_ui_harness_send,
	                 self);
	htmx_router_post(router, "/harness/:id/close", venture_web_ui_harness_close,
	                 self);
	htmx_router_get(router, "/harness/:id/stream",
	                venture_web_ui_harness_stream, self);
	htmx_router_get(router, "/ui/records/search",
	                venture_web_ui_records_search, self);
	htmx_router_get(router, "/ui/chat/complete", venture_web_ui_chat_complete,
	                self);
	htmx_router_get(router, "/ui/chat/stream/:token",
	                venture_web_ui_chat_stream, self);
	htmx_router_post(router, "/ui/chat/thread/:id/rename",
	                 venture_web_ui_chat_thread_rename, self);
	htmx_router_get(router, "/ui/chat/thread/:id/export",
	                venture_web_ui_chat_thread_export, self);
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
	htmx_router_post(router, "/f/:token", venture_web_lead_capture, self);
	htmx_router_post(router, "/api/v1/leads/:id/:action", venture_web_lead_action, self);
	htmx_router_post(router, "/leads/:id/:action", venture_web_lead_action, self);
	htmx_router_get(router, "/api/v1/factory", venture_web_api_factory, self);
	htmx_router_post(router, "/api/v1/post/backfill", venture_web_autojournal_backfill, self);
	htmx_router_get(router, "/api/v1/inbox", venture_web_api_inbox, self);
	htmx_router_post(router, "/api/v1/inbox/read", venture_web_api_inbox_read,
	                 self);
	htmx_router_post(router, "/api/v1/watch", venture_web_api_watch, self);
	htmx_router_get(router, "/api/v1/watching/:type/:id",
	                venture_web_api_watching, self);
	htmx_router_get(router, "/api/v1/activity/:type/:id",
	                venture_web_api_activity, self);
	htmx_router_get(router, "/api/v1/tickets/:id/sla",
	                venture_web_api_ticket_sla, self);
	htmx_router_post(router, "/api/v1/tickets/:id/macro",
	                 venture_web_api_ticket_macro, self);
	htmx_router_post(router, "/api/v1/tickets/:id/worklog",
	                 venture_web_api_ticket_worklog, self);
	htmx_router_post(router, "/api/v1/sla/sweep", venture_web_api_sla_sweep,
	                 self);
	htmx_router_get(router, "/api/v1/sprints", venture_web_api_sprints, self);
	htmx_router_get(router, "/api/v1/sprints/:id", venture_web_api_sprint, self);
	htmx_router_post(router, "/api/v1/incidents/:id/ticket",
	                 venture_web_api_incident_ticket, self);
	htmx_router_get(router, "/api/v1/runs", venture_web_api_runs, self);
	htmx_router_get(router, "/api/v1/budgets", venture_web_api_budgets, self);
	htmx_router_get(router, "/api/v1/palette", venture_web_api_palette, self);
	htmx_router_get(router, "/api/v1/webhooks", venture_web_api_webhooks, self);
	htmx_router_post(router, "/api/v1/webhooks/:id/test",
	                 venture_web_api_webhook_test, self);
	htmx_router_post(router, "/api/v1/webhooks/:id/secret",
	                 venture_web_api_webhook_secret, self);
	htmx_router_post(router, "/api/v1/tickets/:id/triage",
	                 venture_web_api_ticket_triage, self);
	htmx_router_get(router, "/api/v1/tickets/:id/summary",
	                venture_web_api_ticket_summary, self);
	htmx_router_post(router, "/api/v1/tickets/:id/draft",
	                 venture_web_api_ticket_draft, self);
	htmx_router_post(router, "/api/v1/releases/:id/changelog",
	                 venture_web_api_release_changelog, self);
	htmx_router_get(router, "/worklist", activity_ui_worklist, self);
	htmx_router_post(router, "/activities/:id/complete", activity_ui_done, self);
	htmx_router_get(router, "/api/v1/activities.ics", activity_api_calendar, self);
	htmx_router_get(router, "/api/v1/activities", activity_api_list, self);
	htmx_router_post(router, "/api/v1/activities/sweep", activity_api_sweep, self);
	htmx_router_post(router, "/api/v1/activities/:id/:action", activity_api_action, self);
	htmx_router_post(router, "/api/v1/releases/:id/publish",
	                 venture_web_api_release_publish, self);
	htmx_router_post(router, "/api/v1/fixed_assets/:id/:operation", venture_web_asset_action, self);
	htmx_router_post(router, "/assets/:id/:operation", venture_web_asset_action, self);
	htmx_router_post(router, "/api/v1/assets/run-period", venture_web_assets_run, self);
	htmx_router_post(router, "/api/v1/customer_subscriptions/:id/:action", venture_billing_web_action, self);
	htmx_router_post(router, "/api/v1/billing/start", venture_billing_web_action, self);
	htmx_router_post(router, "/api/v1/billing/:action", venture_billing_web_action, self);
	htmx_router_post(router, "/billing/subscriptions/:id/action", venture_billing_web_action, self);
	htmx_router_get(router, "/api/v1/widget-kinds",
	                venture_web_api_widget_kinds, self);
	htmx_router_get(router, "/api/v1/dashboard-templates",
	                venture_web_api_dashboard_templates, self);
	htmx_router_get(router, "/api/v1/dashboards", venture_web_api_dashboards,
	                self);
	htmx_router_post(router, "/api/v1/dashboards/:action",
	                 venture_web_api_dashboard_import, self);
	htmx_router_get(router, "/api/v1/dashboards/:slug",
	                venture_web_api_dashboard, self);
	htmx_router_get(router, "/api/v1/dashboards/:slug/export",
	                venture_web_api_dashboard_export, self);
	htmx_router_get(router, "/api/v1/schema", venture_web_api_describe, self);
	htmx_router_get(router, "/api/v1/schema/:type", venture_web_api_describe,
	                self);
	htmx_router_post(router, "/api/v1/journals/:id/post", venture_web_orgaccess_post, self);
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
	htmx_router_get(router, "/api/v1/kb/search", venture_web_api_kb_search,
	                self);
	htmx_router_post(router, "/api/v1/kb/:id/sync", venture_web_api_kb_sync,
	                 self);
	htmx_router_post(router, "/api/v1/kb/:id/reindex",
	                 venture_web_api_kb_reindex, self);
	htmx_router_get(router, "/api/v1/kb/:id/export",
	                venture_web_api_kb_export, self);
	htmx_router_post(router, "/api/v1/kb/:id/import",
	                 venture_web_api_kb_import, self);
	htmx_router_post(router, "/api/v1/kb/crossref/:type/:id",
	                 venture_web_api_kb_crossref, self);
	htmx_router_post(router, "/api/v1/kb/from/:type/:id",
	                 venture_web_api_kb_from_record, self);
	htmx_router_get(router, "/api/v1/plugins", venture_web_api_plugins, self);
	htmx_router_get(router, "/api/v1/modules", venture_web_api_modules, self);
	/* Before /api/v1/:type/:id, or "links" would be read as a type. */
	htmx_router_get(router, "/api/v1/links/:type/:id", venture_web_api_links,
	                self);
	htmx_router_post(router, "/api/v1/links", venture_web_api_link_create,
	                 self);
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
	htmx_router_post(router, "/api/v1/:type/bulk", venture_web_api_bulk, self);
	htmx_router_post(router, "/api/v1/:type/:id/restore",
	                 venture_web_api_restore, self);

	htmx_router_get(router, "/federation/v1/identity", venture_web_federation_identity, self);
	htmx_router_post(router, "/federation/v1/request", venture_web_federation_receive, self);
	htmx_router_post(router, "/api/v1/federation", venture_web_api_federation, self);
	htmx_router_get(router, "/federation", venture_web_ui_federation, self);
	htmx_router_get(router, "/federation/replicas/:id", venture_web_ui_federation_replica, self);
	htmx_router_post(router, "/federation/pull", venture_web_ui_federation_write, self);
	htmx_router_post(router, "/federation/replicas/:id/:action", venture_web_ui_federation_write, self);

	htmx_router_post(router, "/api/v1/reconciliation/suggest", venture_web_api_reconciliation_suggest, self);
	htmx_router_post(router, "/invoices/:id/send", venture_web_mail_invoice_ui, self);
	htmx_router_post(router, "/api/v1/mail/:action", venture_web_mail_action, self);
	htmx_router_post(router, "/api/v1/mail_messages/:id/retry", venture_web_mail_action, self);
	htmx_router_post(router, "/api/v1/invoices/:id/send", venture_web_mail_invoice, self);
	htmx_router_post(router, "/api/v1/deals/:id/move", venture_web_deal_move, self);
	htmx_router_post(router, "/deals/:id/move", venture_web_deal_move_ui, self);
	htmx_router_get(router, "/deals", venture_web_deals_board, self);
	htmx_router_post(router, "/api/v1/sequence/:id/enroll", venture_web_sequence_enroll, self);
	htmx_router_post(router, "/api/v1/sequence_enrollment/:id/:action", venture_web_sequence_action, self);
	htmx_router_post(router, "/ui/sequence_enrollment/:id/:action", venture_web_sequence_action, self);
	htmx_router_post(router, "/api/v1/sequences/run", venture_web_sequence_run, self);

	htmx_router_get(router, "/api/v1/:type", venture_web_api_list, self);
	htmx_router_post(router, "/api/v1/:type", venture_web_api_create, self);
	htmx_router_get(router, "/api/v1/:type/:id", venture_web_api_get, self);
	htmx_router_put(router, "/api/v1/:type/:id", venture_web_api_update, self);
	htmx_router_patch(router, "/api/v1/:type/:id", venture_web_api_update, self);
	htmx_router_delete(router, "/api/v1/:type/:id", venture_web_api_delete,
	                   self);

	venture_bank_web_register(router, self);
	venture_bankfeed_web_register(router, self);
	venture_cutover_web_register(router, self);
	venture_setup_web_register(router, self);
venture_document_web_register(router, self);
	venture_portal_web_register(router, self);
	venture_backup_web_register(router, self);
	htmx_router_post(router, "/api/v1/:type/:id/actions/:action", venture_web_api_action, self);
	htmx_router_post(router, "/api/v1/journals/post", venture_web_api_action, self);

	return g_steal_pointer(&self);
}

gboolean
venture_web_server_start(
	VentureWebServer	 *self,
	GError			**error
){
	g_autoptr(GError) local_error = NULL;

	g_return_val_if_fail(VENTURE_IS_WEB_SERVER(self), FALSE);

	/* Honor the existing TLS settings. A partially configured certificate
	 * must fail closed, never start a plaintext listener by accident. */
	{
		g_autofree gchar *certificate = NULL;
		g_autofree gchar *private_key = NULL;
		g_autofree gchar *bind = NULL;
		g_object_get(venture_context_get_config(self->context),
			"server-tls-certificate", &certificate,
			"server-tls-private-key", &private_key,
			"server-bind-address", &bind, NULL);
		if (!venture_string_is_empty(certificate) || !venture_string_is_empty(private_key))
		{
			g_autoptr(GTlsCertificate) tls = NULL;
			g_autoptr(GInetAddress) address = NULL;
			g_autoptr(GSocketAddress) socket_address = NULL;
			SoupServer *server = htmx_server_get_soup_server(self->server);
			GSList *uris;
			tls = g_tls_certificate_new_from_files(certificate, private_key, error);
			if (!tls) return FALSE;
			address = g_inet_address_new_from_string(bind);
			if (!address)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
					"TLS bind address must be an IP address");
				return FALSE;
			}
			soup_server_set_tls_certificate(server, tls);
			socket_address = g_inet_socket_address_new(address, self->port);
			if (!soup_server_listen(server, socket_address, SOUP_SERVER_LISTEN_HTTPS, error))
				return FALSE;
			uris = soup_server_get_uris(server);
			g_free(self->base_url);
			self->base_url = g_uri_to_string((GUri *)uris->data);
			g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
			venture_federation_sync_start(self->context);
			return TRUE;
		}
	}

	if (!htmx_server_start(self->server, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
		            "Cannot listen on %s: %s", self->base_url,
		            (NULL != local_error) ? local_error->message
		                                  : "unknown failure");
		return FALSE;
	}

	venture_federation_sync_start(self->context);
	return TRUE;
}

void
venture_web_server_stop(VentureWebServer *self)
{
	g_return_if_fail(VENTURE_IS_WEB_SERVER(self));

	venture_federation_sync_stop(self->context);
	soup_server_disconnect(htmx_server_get_soup_server(self->server));
	htmx_server_stop(self->server);
}

const gchar *
venture_web_server_get_base_url(VentureWebServer *self)
{
	g_return_val_if_fail(VENTURE_IS_WEB_SERVER(self), NULL);

	return self->base_url;
}
