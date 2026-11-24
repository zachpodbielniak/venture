/*
 * venture-forgejo-client-private.h - The Forgejo client's origin check
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exposed to the tests only. A real request cannot be made to an origin
 * with no port from a test, because that needs a listener on 80 or 443.
 */

#ifndef VENTURE_FORGEJO_CLIENT_PRIVATE_H
#define VENTURE_FORGEJO_CLIENT_PRIVATE_H

#include "venture.h"

G_BEGIN_DECLS

/*
 * venture_forgejo_client_is_pinned_origin:
 * @self: a #VentureForgejoClient
 * @uri: the URI a request was finally sent to
 *
 * Whether @uri is on the host and port the client was pinned to. A request
 * that ends anywhere else was redirected, and its answer is refused.
 *
 * Returns: %TRUE when @uri is on the pinned origin
 */
gboolean
venture_forgejo_client_is_pinned_origin(
	VentureForgejoClient	*self,
	GUri			*uri
);

G_END_DECLS

#endif /* VENTURE_FORGEJO_CLIENT_PRIVATE_H */
