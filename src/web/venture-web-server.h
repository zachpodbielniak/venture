/*
 * venture-web-server.h - The REST API and the HTMX web UI
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The API is generated rather than written. One set of handlers serves every
 * registered record type at /api/v1/<type>, deriving the payload shape, the
 * validation and the filter vocabulary from the type's own properties. A
 * plugin that registers a record type gets a complete REST resource without
 * contributing a line of routing.
 *
 * The UI is server-rendered HTML with hx-* attributes. There is no client
 * application and no build step: a page is HTML, a fragment is HTML, and the
 * only JavaScript is the small attribute client and the behaviour script,
 * both compiled into the binary.
 */

#ifndef VENTURE_WEB_SERVER_H
#define VENTURE_WEB_SERVER_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <htmx-glib.h>

G_BEGIN_DECLS

/**
 * VentureWebNavLink:
 * @path: the URL the entry links to
 * @label: the visible text
 * @icon: inline SVG markup for the icon shown before the label
 * @section: (nullable): a heading to start a new group with
 *
 * One entry in the web UI's sidebar. Exposed so that the test suite can
 * check every link resolves to something: a sidebar entry pointing at a
 * path with no route is a 404 the operator finds by clicking it.
 */
typedef struct
{
	const gchar *path;
	const gchar *label;
	const gchar *icon;
	const gchar *section;

	/* The module the link belongs to. A link whose module is off is not
	 * rendered, and a section whose every link is off loses its heading. */
	const gchar *module;
} VentureWebNavLink;

/**
 * venture_web_navigation:
 *
 * Retrieves the sidebar entries, terminated by one with a %NULL path.
 *
 * Returns: (transfer none) (array zero-terminated=1): the navigation table
 */
const VentureWebNavLink *
venture_web_navigation(void);

#define VENTURE_TYPE_WEB_SERVER (venture_web_server_get_type())

G_DECLARE_FINAL_TYPE(VentureWebServer, venture_web_server,
                     VENTURE, WEB_SERVER, GObject)

/**
 * venture_web_server_new:
 * @context: the wiring
 * @error: (out) (optional): return location for a #GError
 *
 * Creates the server and registers every route.
 *
 * Returns: (transfer full) (nullable): the server, or %NULL on error
 */
VentureWebServer *
venture_web_server_new(
	VentureContext	 *context,
	GError		**error
);

/**
 * venture_web_server_start:
 * @self: a #VentureWebServer
 * @error: (out) (optional): return location for a #GError
 *
 * Binds the configured address and begins accepting requests.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_web_server_start(
	VentureWebServer	 *self,
	GError			**error
);

/**
 * venture_web_server_stop:
 * @self: a #VentureWebServer
 *
 * Stops accepting requests.
 */
void
venture_web_server_stop(VentureWebServer *self);

/**
 * venture_web_server_get_base_url:
 * @self: a #VentureWebServer
 *
 * Returns: (transfer none): the address the server is reachable at
 */
const gchar *
venture_web_server_get_base_url(VentureWebServer *self);

/**
 * venture_web_server_get_port:
 * @self: a #VentureWebServer
 *
 * The port the server listens on. Before venture_web_server_start() this is
 * the configured port; after it, the port actually bound -- which differs
 * when the configuration asked for port 0 and the kernel chose one. The
 * base URL follows the same rule.
 *
 * Returns: the listening port, or 0 before a port-0 server has started
 */
guint16
venture_web_server_get_port(VentureWebServer *self);

/**
 * VentureWebNavSection:
 * @heading: the section's name, as the sidebar heads it
 * @group: (nullable): the area it is part of. Consecutive sections naming
 *   the same group are drawn as one folding area -- "Money" holding Money
 *   in, Money out, Bank and Books -- with each section's heading as a
 *   label inside it. %NULL is an area of its own, headed by @heading.
 * @paths: (array zero-terminated=1): the sidebar paths drawn under it, in order
 *
 * The sidebar's map: a second table over the link table. A link named
 * here is drawn under its section instead of in its place, so the link
 * table keeps its order and every page keeps its URL, module gate and
 * role. Exposed so the test suite can hold the grouping to its membership.
 */
typedef struct
{
	const gchar *heading;
	const gchar *group;
	const gchar *const *paths;
} VentureWebNavSection;

/**
 * venture_web_navigation_sections:
 *
 * Retrieves the sidebar's sections in the order they are drawn, terminated
 * by one with a %NULL heading.
 *
 * Returns: (transfer none) (array zero-terminated=1): the section table
 */
const VentureWebNavSection *
venture_web_navigation_sections(void);

/**
 * VentureHostedRouteFlags:
 * @VENTURE_HOSTED_ROUTE_NONE: ordinary lifecycle enforcement
 * @VENTURE_HOSTED_ROUTE_CONTROL: identity or workspace-administration route
 * @VENTURE_HOSTED_ROUTE_SUPPORT: audited generic repository route safe for scoped support
 * @VENTURE_HOSTED_ROUTE_CAPABILITY_ORIGIN: tenant intake whose handler validates
 *   an exact configured external Origin and capability without ambient sessions
 */
typedef enum {
	VENTURE_HOSTED_ROUTE_NONE = 0,
	VENTURE_HOSTED_ROUTE_CONTROL = 1 << 0,
	VENTURE_HOSTED_ROUTE_SUPPORT = 1 << 1,
	VENTURE_HOSTED_ROUTE_CAPABILITY_ORIGIN = 1 << 2
} VentureHostedRouteFlags;

/**
 * venture_web_server_add_classified_route:
 * @self: server
 * @method: HTTP method
 * @pattern: existing router pattern syntax
 * @classification: explicit authority class
 * @flags: explicit lifecycle-control or scoped-support declaration
 * @callback: (scope forever) (closure user_data): guarded handler
 * @user_data: (nullable): handler data
 *
 * Registers execution and classification together. Hosted mode refuses direct
 * unclassified router additions. Control bypasses lifecycle blocking, never
 * host verification or the handler's authentication and permission checks.
 * Capability-origin routes remain subject to pinned Host and lifecycle checks;
 * only their Origin comparison is delegated to their mandatory configured-site
 * validator. This flag is tenant-only and cannot combine with control/support.
 */
void venture_web_server_add_classified_route(VentureWebServer *self, HtmxMethod method,
	const gchar *pattern, VentureDataClass classification, VentureHostedRouteFlags flags,
	HtmxRouteCallback callback, gpointer user_data);

/* ==========================================================================
 * For plugins' web extensions
 * ========================================================================== */

/**
 * venture_web_server_add_nav_link:
 * @self: a #VentureWebServer
 * @path: the page the row opens: absolute, at most 256 bytes, with no
 *   quote, angle bracket, backslash, backtick, whitespace, control byte,
 *   query, fragment, `.` or `..` segment, and not starting `//`
 * @label: the visible text, 1 to 64 bytes of UTF-8 with no control
 *   characters; escaped when drawn
 * @icon: (nullable): the name of a built-in sidebar icon (see
 *   venture_web_server_nav_icon_names()); %NULL is "plug"
 * @module: (nullable): the module the row belongs to; while it is off the
 *   row is not drawn. %NULL is no module
 * @error: (out) (optional): return location for a #GError
 *
 * Adds a plugin's page to the sidebar, under a "Plugins" heading that is
 * drawn only when at least one such row is. For a web extension only
 * (see #VentureWebExtensionFunc): built-in pages belong in the static
 * navigation table, and a call from anywhere else is refused.
 *
 * Refused with %VENTURE_ERROR_INVALID_ARGUMENT for a bad path, label,
 * unknown icon or unknown module; %VENTURE_ERROR_ALREADY_EXISTS for a path
 * the built-in sidebar or an earlier row already has; and
 * %VENTURE_ERROR_NOT_FOUND when no GET route registered through
 * venture_web_server_add_classified_route() serves @path -- add the route
 * first. An icon is a name and never markup, because the sidebar is drawn
 * into every page every signed-in person sees.
 *
 * An accountant-only sidebar shows a plugin's row only when @module is a
 * module that sidebar already offers.
 *
 * Returns: %TRUE if the row was added
 */
gboolean
venture_web_server_add_nav_link(
	VentureWebServer	 *self,
	const gchar		 *path,
	const gchar		 *label,
	const gchar		 *icon,
	const gchar		 *module,
	GError			**error
);

/**
 * venture_web_server_get_plugin_navigation:
 * @self: a #VentureWebServer
 *
 * The rows plugins added, in the order added, each with the section
 * "Plugins". Exposed so the test suite can hold them to the rules the
 * static table is held to.
 *
 * Returns: (transfer none) (array zero-terminated=1): the rows,
 *   terminated by one with a %NULL path
 */
const VentureWebNavLink *
venture_web_server_get_plugin_navigation(VentureWebServer *self);

/**
 * venture_web_server_nav_icon_names:
 *
 * Returns: (transfer full) (array zero-terminated=1): the icon names
 *   venture_web_server_add_nav_link() accepts
 */
gchar **
venture_web_server_nav_icon_names(void);

/**
 * venture_web_server_get_context:
 * @self: a #VentureWebServer
 *
 * Returns: (transfer none): the context the server was built over
 */
VentureContext *
venture_web_server_get_context(VentureWebServer *self);

/**
 * venture_web_server_require_page:
 * @self: a #VentureWebServer
 * @request: the request
 * @role: the least role the page needs
 *
 * The guard for an extension's page. Anonymous requests are redirected to
 * the login page and a signed-in person without @role is refused (403),
 * exactly as a built-in page treats them. Every route needs its own
 * guard: a missing one looks exactly like nothing.
 *
 * Returns: (transfer full) (nullable): a response to return instead, or
 *   %NULL when the request may proceed
 */
HtmxResponse *
venture_web_server_require_page(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureUserRole		 role
);

/**
 * venture_web_server_require_module:
 * @self: a #VentureWebServer
 * @request: the request
 * @module: the module the page belongs to
 *
 * The page a request into a switched-off module gets: the module named,
 * with the switch that turns it back on, as a 404. Call it before
 * venture_web_server_require_page() on a page that belongs to a module.
 *
 * Returns: (transfer full) (nullable): a response to return instead, or
 *   %NULL when the module is on
 */
HtmxResponse *
venture_web_server_require_module(
	VentureWebServer	*self,
	HtmxRequest		*request,
	const gchar		*module
);

/**
 * venture_web_server_require_api:
 * @self: a #VentureWebServer
 * @request: the request
 * @role: the least role the endpoint needs
 *
 * The guard for an extension's API endpoint: 401 when anonymous, 403
 * without @role, as JSON.
 *
 * Returns: (transfer full) (nullable): a response to return instead, or
 *   %NULL when the request may proceed
 */
HtmxResponse *
venture_web_server_require_api(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureUserRole		 role
);

/**
 * venture_web_server_render_page:
 * @self: a #VentureWebServer
 * @request: the request
 * @active: (nullable): the sidebar path to mark as current
 * @title: the page title
 * @content: (nullable): the page body, already-escaped HTML
 *
 * Wraps @content in the page every built-in page uses -- head, sidebar,
 * assistant dock -- and answers 200. @content is written as given: escape
 * anything a person typed with venture_html_escape_append().
 *
 * Returns: (transfer full): the response
 */
HtmxResponse *
venture_web_server_render_page(
	VentureWebServer	*self,
	HtmxRequest		*request,
	const gchar		*active,
	const gchar		*title,
	const gchar		*content
);

G_END_DECLS

#endif /* VENTURE_WEB_SERVER_H */
