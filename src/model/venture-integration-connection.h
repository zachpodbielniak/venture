/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_INTEGRATION_CONNECTION_H
#define VENTURE_INTEGRATION_CONNECTION_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * VentureIntegrationConnection:
 *
 * One organization's durable provider/account/environment binding. Credentials
 * are authenticated ciphertext and never part of generated output. Only the
 * integration service changes a binding; disabling retains historical identity.
 */
#define VENTURE_TYPE_INTEGRATION_CONNECTION (venture_integration_connection_get_type())
VENTURE_DECLARE_ENTITY(VentureIntegrationConnection, venture_integration_connection, INTEGRATION_CONNECTION)
G_END_DECLS
#endif
