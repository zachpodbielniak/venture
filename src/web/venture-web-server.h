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
 * @icon: a UTF-8 glyph shown before the label
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

G_END_DECLS

#endif /* VENTURE_WEB_SERVER_H */
