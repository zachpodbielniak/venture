/*
 * venture-forgejo-client.h - A #VentureForgeClient for Forgejo and Gitea
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One implementation serves both: Gitea's API v1 and Forgejo's are the same
 * interface, and Forgejo still sends X-Gitea-* headers alongside its own.
 */

#ifndef VENTURE_FORGEJO_CLIENT_H
#define VENTURE_FORGEJO_CLIENT_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_FORGEJO_CLIENT (venture_forgejo_client_get_type())

G_DECLARE_FINAL_TYPE(VentureForgejoClient, venture_forgejo_client,
                     VENTURE, FORGEJO_CLIENT, GObject)

/**
 * venture_forgejo_client_new:
 * @base_url: the forge's origin, e.g. `https://git.example.com`
 * @token: (nullable): the access token
 * @timeout_seconds: how long a single call may take
 * @error: (out) (optional): return location for a #GError
 *
 * Creates a client pinned to @base_url.
 *
 * The origin is parsed once, here. Every later request is built from the
 * scheme, host and port taken from that parse, and never from a string a
 * caller or a response supplied -- which is what makes this safe to point at
 * a private address that the AI's fetcher would refuse.
 *
 * Returns: (transfer full) (nullable): the client, or %NULL if @base_url is
 *   not an absolute http or https URL
 */
VentureForgejoClient *
venture_forgejo_client_new(
	const gchar	 *base_url,
	const gchar	 *token,
	gint		  timeout_seconds,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_FORGEJO_CLIENT_H */
