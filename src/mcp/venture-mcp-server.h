/*
 * venture-mcp-server.h - `venturectl mcp`, a stdio MCP server over the API
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #VentureMcpServer speaks the Model Context Protocol on standard input
 * and standard output, and answers each tool call by talking to a VENTURE
 * server's REST API with a bearer token. It is what puts VENTURE in front of
 * an AI coding agent, which discovers it through an `.mcp.json` entry the
 * same way it finds any other MCP server.
 *
 * The transport is newline-delimited JSON-RPC 2.0, written directly rather
 * than through an SDK -- see docs/mcp.org for why.
 *
 * Configuration comes from the environment (`VENTURE_URL`, `VENTURE_TOKEN`)
 * and never from argv. An argv is world-readable through /proc and lands in
 * the shell's history; a token that has been in either has to be rotated.
 *
 * The tool surface is not written down here. It is built at startup from
 * `GET /api/v1/schema` by #VentureMcpCatalog, so a record type registered by
 * a plugin is offered without a line changing in this file.
 */

#ifndef VENTURE_MCP_SERVER_H
#define VENTURE_MCP_SERVER_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "mcp/venture-mcp-catalog.h"

G_BEGIN_DECLS

#define VENTURE_TYPE_MCP_SERVER (venture_mcp_server_get_type())

G_DECLARE_FINAL_TYPE(VentureMcpServer, venture_mcp_server, VENTURE,
                     MCP_SERVER, GObject)

/**
 * VentureMcpTransportFunc:
 * @method: the HTTP method
 * @path: the path, already escaped, beginning with a slash
 * @body: (nullable): a JSON request body, or %NULL
 * @out_status: (out): the HTTP status that came back
 * @out_content_type: (out) (optional) (nullable): the response's Content-Type
 *   header, which the caller owns and must free
 * @user_data: whatever was handed to venture_mcp_server_set_transport()
 * @error: (out) (optional): return location for a #GError
 *
 * Performs one request against the VENTURE server.
 *
 * This is an injection point so the whole protocol can be tested against
 * canned responses: `make test` must pass with no VENTURE server running and
 * no network, and a test that needs one is a test that does not run.
 *
 * Returns: (transfer full) (nullable): the response body, or %NULL on a
 *   transport failure
 */
typedef gchar *(*VentureMcpTransportFunc) (
	const gchar	 *method,
	const gchar	 *path,
	JsonNode	 *body,
	guint		 *out_status,
	gchar		**out_content_type,
	gpointer	  user_data,
	GError		**error
);

/**
 * venture_mcp_server_new:
 * @base_url: (nullable): the server to talk to; %NULL takes `VENTURE_URL`,
 *   and failing that http://localhost:8747
 * @token: (nullable): the bearer token; %NULL takes `VENTURE_TOKEN`
 * @error: (out) (optional): return location for a #GError
 *
 * Creates a server.
 *
 * A missing token is refused here rather than at the first tool call, and
 * the refusal says how to mint one: every call would fail with a 401
 * otherwise, and an agent reading a wall of them concludes the records are
 * missing rather than that it was never given a credential.
 *
 * Returns: (transfer full) (nullable): the server, or %NULL on error
 */
VentureMcpServer *
venture_mcp_server_new(
	const gchar	 *base_url,
	const gchar	 *token,
	GError		**error
);

/**
 * venture_mcp_server_set_transport:
 * @self: a #VentureMcpServer
 * @func: (nullable) (scope notified) (closure user_data) (destroy notify):
 *   the transport to use, or %NULL to restore the built-in HTTP one
 * @user_data: passed to @func
 * @notify: (nullable): called on @user_data when the server is finalised
 *
 * Replaces how requests reach the VENTURE server.
 */
void
venture_mcp_server_set_transport(
	VentureMcpServer	*self,
	VentureMcpTransportFunc	 func,
	gpointer		 user_data,
	GDestroyNotify		 notify
);

/**
 * venture_mcp_server_set_stage_writes:
 * @self: a #VentureMcpServer
 * @stage_writes: whether a write tool should describe the change instead of
 *   performing it
 *
 * Turns staging on or off.
 *
 * With staging on, venture_create, venture_update and venture_delete report
 * exactly the request they would send and send nothing. It is a hold, not a
 * queue: see docs/mcp.org, which is explicit that VENTURE's REST API has no
 * server-side staging for a token-authenticated write, so nothing is left
 * pending anywhere. Saying a change was staged when the server never heard
 * about it would be worse than applying it.
 */
void
venture_mcp_server_set_stage_writes(
	VentureMcpServer	*self,
	gboolean		 stage_writes
);

/**
 * venture_mcp_server_get_stage_writes:
 * @self: a #VentureMcpServer
 *
 * Returns: whether write tools currently hold rather than apply
 */
gboolean
venture_mcp_server_get_stage_writes(VentureMcpServer *self);

/**
 * venture_mcp_server_load_catalog:
 * @self: a #VentureMcpServer
 * @error: (out) (optional): return location for a #GError
 *
 * Fetches `GET /api/v1/schema` and builds the tool surface from it.
 *
 * Called once before the protocol loop starts. A failure here is fatal on
 * purpose: an MCP server that answers `tools/list` with a guess is worse
 * than one that does not start, because the guess is indistinguishable from
 * the truth until a call fails.
 *
 * Returns: %TRUE if the catalog was built
 */
gboolean
venture_mcp_server_load_catalog(
	VentureMcpServer	 *self,
	GError			**error
);

/**
 * venture_mcp_server_get_catalog:
 * @self: a #VentureMcpServer
 *
 * Returns: (transfer none) (nullable): the tool surface, or %NULL before
 *   venture_mcp_server_load_catalog() has succeeded
 */
VentureMcpCatalog *
venture_mcp_server_get_catalog(VentureMcpServer *self);

/**
 * venture_mcp_server_handle:
 * @self: a #VentureMcpServer
 * @request: one parsed JSON-RPC request or notification
 *
 * Answers one message.
 *
 * The whole protocol is here rather than in the read loop, so a test can
 * drive every method without a pipe, a subprocess or a main loop.
 *
 * Returns: (transfer full) (nullable): the response, or %NULL when @request
 *   was a notification and JSON-RPC forbids answering it
 */
JsonNode *
venture_mcp_server_handle(
	VentureMcpServer	*self,
	JsonNode		*request
);

/**
 * venture_mcp_server_run:
 * @self: a #VentureMcpServer
 * @error: (out) (optional): return location for a #GError
 *
 * Reads newline-delimited JSON-RPC from standard input until it closes,
 * writing each response to standard output.
 *
 * Returns: %TRUE if the stream ended cleanly
 */
gboolean
venture_mcp_server_run(
	VentureMcpServer	 *self,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_MCP_SERVER_H */
