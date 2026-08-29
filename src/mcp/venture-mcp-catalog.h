/*
 * venture-mcp-catalog.h - The MCP tool surface, generated from the schema
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #VentureMcpCatalog turns one `GET /api/v1/schema` response into the tool
 * list an MCP client is offered. It holds no connection and performs no I/O,
 * which is the point: the tool surface is a pure function of the schema, so
 * it can be tested against a fixture and cannot drift from the registry.
 *
 * Nothing here enumerates record types in C. VENTURE's API is generated from
 * a record registry, and `docs/api.org` says outright that a plugin's record
 * type becomes a resource the moment it registers. A hand-written tool list
 * would therefore be a second copy of the data model that goes stale the
 * first time a plugin registers a type -- the same reason the CLI is generic
 * over record types and adds no subcommand per type.
 *
 * Singular and plural name the same resource (`/api/v1/sale` and
 * `/api/v1/sales`), so the catalog offers the singular once and resolves
 * either spelling to it. Offering both would present one resource as two.
 */

#ifndef VENTURE_MCP_CATALOG_H
#define VENTURE_MCP_CATALOG_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_MCP_CATALOG (venture_mcp_catalog_get_type())

G_DECLARE_FINAL_TYPE(VentureMcpCatalog, venture_mcp_catalog, VENTURE,
                     MCP_CATALOG, GObject)

/**
 * venture_mcp_catalog_new_from_schema:
 * @schema: the parsed body of `GET /api/v1/schema`: an array of record type
 *   descriptions, each with at least a `name`
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the tool surface from one schema response.
 *
 * A response that is not an array of objects carrying names is refused with
 * a message saying what arrived, rather than yielding a catalog holding
 * whichever entries happened to parse. Half a tool list is worse than none:
 * an agent handed it reports "there is no such record type" for everything
 * the malformed half described, which reads as missing data rather than as a
 * broken server.
 *
 * Returns: (transfer full) (nullable): the catalog, or %NULL on error
 */
VentureMcpCatalog *
venture_mcp_catalog_new_from_schema(
	JsonNode	 *schema,
	GError		**error
);

/**
 * venture_mcp_catalog_get_n_types:
 * @self: a #VentureMcpCatalog
 *
 * Returns: how many record types the schema described
 */
guint
venture_mcp_catalog_get_n_types(VentureMcpCatalog *self);

/**
 * venture_mcp_catalog_list_types:
 * @self: a #VentureMcpCatalog
 *
 * Lists the canonical singular name of every record type, in the order the
 * schema gave them. Plurals are aliases and do not appear.
 *
 * Returns: (transfer full) (array zero-terminated=1): the type names
 */
gchar **
venture_mcp_catalog_list_types(VentureMcpCatalog *self);

/**
 * venture_mcp_catalog_resolve_type:
 * @self: a #VentureMcpCatalog
 * @name: a record type name in either spelling, in any case
 *
 * Resolves a caller-supplied type name to the one canonical spelling.
 * `sale`, `sales` and `Sales` all answer `sale`.
 *
 * Returns: (transfer none) (nullable): the canonical name, or %NULL if no
 *   such record type was described
 */
const gchar *
venture_mcp_catalog_resolve_type(
	VentureMcpCatalog	*self,
	const gchar		*name
);

/**
 * venture_mcp_catalog_describe_type:
 * @self: a #VentureMcpCatalog
 * @name: a record type name in either spelling
 *
 * The schema entry for one record type, with every field name in the
 * spelling a caller has to type.
 *
 * Properties are `forge-id` in C and `forge_id` on the wire, and a POST body
 * with dashed keys is ignored field by field -- the record saves and the
 * values simply are not there. This is the one place an agent looks a field
 * name up, so it is translated here rather than left to be guessed.
 *
 * Returns: (transfer full) (nullable): the description, or %NULL
 */
JsonNode *
venture_mcp_catalog_describe_type(
	VentureMcpCatalog	*self,
	const gchar		*name
);

/**
 * venture_mcp_catalog_get_tools:
 * @self: a #VentureMcpCatalog
 *
 * The `tools/list` payload: an array of objects carrying `name`,
 * `description` and `inputSchema`.
 *
 * Every tool that names a record type carries the catalog's type list as the
 * `enum` of its `type` parameter, so a type registered by a plugin is
 * offered without any change here.
 *
 * Returns: (transfer full): the tool array
 */
JsonNode *
venture_mcp_catalog_get_tools(VentureMcpCatalog *self);

/**
 * venture_mcp_catalog_has_tool:
 * @self: a #VentureMcpCatalog
 * @tool_name: the name an MCP client asked to call
 *
 * Returns: %TRUE if this catalog offers a tool by that name
 */
gboolean
venture_mcp_catalog_has_tool(
	VentureMcpCatalog	*self,
	const gchar		*tool_name
);

G_END_DECLS

#endif /* VENTURE_MCP_CATALOG_H */
