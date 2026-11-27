/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-lql.h - VENTURE's records as LQL sources (docs/lql.org)
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * An LQL environment built for one caller: `fetch ticket`, `fetch
 * invoice`, ... for every record type that caller may open, each source
 * reading through the same query layer and access policy GET
 * /api/v1/<type> reads through, plus VENTURE's own functions (me(),
 * my_teams(), today(), period(), organization()). Nothing that reads a
 * file, runs a program or talks SQL is registered.
 *
 * This header is deliberately outside <venture.h> and the GIR: its API
 * speaks LQL's own types, and only server code links the LQL archive.
 */

#ifndef VENTURE_LQL_H
#define VENTURE_LQL_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <lql.h>

G_BEGIN_DECLS

/* The defaults of query.max_rows and query.timeout_ms. */
#define VENTURE_LQL_DEFAULT_MAX_ROWS (10000)
#define VENTURE_LQL_DEFAULT_TIMEOUT_MS (5000)

/* The parameter `fetch TYPE, include_deleted: TRUE` reads soft-deleted
 * rows with, the same name the list API's query string uses. */
#define VENTURE_LQL_PARAM_INCLUDE_DELETED "include_deleted"

/**
 * VentureLqlTypeGate:
 * @entity_type: a record type the environment would offer as a source
 * @user_data: (closure): the data given with the gate
 * @error: (out) (optional): why the caller may not read it
 *
 * Decides whether the caller may list @entity_type at all: the role a
 * type needs (owner-only types such as user and api_token), tenant data
 * classes, support grants. The web layer passes the very check GET
 * /api/v1/<type> makes, so a type the list API refuses is never a source.
 * Module gates are applied before the gate is asked: a type of a module
 * that is off is never offered.
 *
 * Returns: %TRUE when the caller may read @entity_type
 */
typedef gboolean (*VentureLqlTypeGate)(GType     entity_type,
                                       gpointer  user_data,
                                       GError  **error);

#define VENTURE_TYPE_LQL_SOURCE (venture_lql_source_get_type())

G_DECLARE_FINAL_TYPE(VentureLqlSource, venture_lql_source, VENTURE, LQL_SOURCE, GObject)

/**
 * venture_lql_environment_new:
 * @context: the wiring: database, registry, configuration, calendar
 * @principal: the caller; every row is read as this principal
 * @organizations: (array length=n_organizations) (nullable): the selected
 *   business first, then every business beneath it; what the list API
 *   scopes an unqualified list to. %NULL or empty means no selection:
 *   every business the caller may read
 * @n_organizations: how many entries @organizations has
 * @gate: (scope call) (closure gate_data): which record types the caller
 *   may list; asked once per enabled type, while building
 * @gate_data: (nullable): data for @gate
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the environment one caller's queries run in. It holds LQL's
 * built-in commands and functions, one source per record type the caller
 * may read (named as the type is: `fetch ticket`), and VENTURE's
 * functions. Limits come from query.max_rows and query.timeout_ms.
 *
 * The caller's reader -- principal, any organization pin and narrowing
 * of the scope this is called in -- is captured here, and every fetch
 * reads as exactly that reader, on whatever thread it runs.
 *
 * Returns: (transfer full) (nullable): the environment, or %NULL with
 *   @error set
 */
LqlEnvironment *
venture_lql_environment_new(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	const gint64			 *organizations,
	gsize				  n_organizations,
	VentureLqlTypeGate		  gate,
	gpointer			  gate_data,
	GError				**error
);

/**
 * venture_lql_source_get_entity_type:
 * @self: a #VentureLqlSource
 *
 * Returns: the record type the source reads
 */
GType
venture_lql_source_get_entity_type(VentureLqlSource *self);

/**
 * venture_web_server_new_lql_environment:
 * @server: the running web server
 * @request: the request a query arrived with
 * @error: (out) (optional): return location for a #GError
 *
 * The one door the web layer's query surfaces use: authenticates
 * @request, admits it as GET /api/v1/<type> admits a reader (hosted
 * lifecycle, viewer role), scopes it to the business selected in the
 * browser and everything beneath it, and gates every record type with the
 * list API's own role check. A query run in the result sees exactly the
 * rows and fields the list API would give the same request.
 *
 * Call it inside the request's own dispatch: the caller's reader is
 * captured from the access scope the server set up for the request.
 *
 * Returns: (transfer full) (nullable): the environment, or %NULL with
 *   @error set (unauthenticated, refused, or no such business)
 */
LqlEnvironment *
venture_web_server_new_lql_environment(
	VentureWebServer	 *server,
	HtmxRequest		 *request,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_LQL_H */
