/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_OIDC_RECORDS_H
#define VENTURE_OIDC_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_OIDC_IDENTITY (venture_oidc_identity_get_type())
G_DECLARE_FINAL_TYPE(VentureOidcIdentity, venture_oidc_identity, VENTURE, OIDC_IDENTITY, VentureEntity)
/**
 * venture_oidc_identity_new:
 * Returns: (transfer full): empty metadata record; links are created by the service
 */
VentureOidcIdentity *venture_oidc_identity_new(void);
G_END_DECLS
#endif
