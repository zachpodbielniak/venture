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
 * VentureWebNavSection:
 * @heading: the question, as the sidebar heads it
 * @paths: (array zero-terminated=1): the sidebar paths drawn under it, in order
 *
 * One of the five questions the sidebar is grouped by: money in, money out,
 * growth, customers, support. A second table over the link table: a link
 * named here is drawn under its question instead of in its place, so the
 * link table keeps its order and every page keeps its URL, module gate and
 * role. Exposed so the test suite can hold the grouping to its membership.
 */
typedef struct
{
	const gchar *heading;
	const gchar *const *paths;
} VentureWebNavSection;

/**
 * venture_web_navigation_sections:
 *
 * Retrieves the five questions in the order they are drawn, terminated by
 * one with a %NULL heading.
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
 */
typedef enum {
	VENTURE_HOSTED_ROUTE_NONE = 0,
	VENTURE_HOSTED_ROUTE_CONTROL = 1 << 0,
	VENTURE_HOSTED_ROUTE_SUPPORT = 1 << 1
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
 */
void venture_web_server_add_classified_route(VentureWebServer *self, HtmxMethod method,
	const gchar *pattern, VentureDataClass classification, VentureHostedRouteFlags flags,
	HtmxRouteCallback callback, gpointer user_data);

G_END_DECLS

#endif /* VENTURE_WEB_SERVER_H */
