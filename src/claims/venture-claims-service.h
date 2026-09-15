/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CLAIMS_SERVICE_H
#define VENTURE_CLAIMS_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CLAIMS_SERVICE (venture_claims_service_get_type())
G_DECLARE_FINAL_TYPE(VentureClaimsService, venture_claims_service, VENTURE, CLAIMS_SERVICE, GObject)

/**
 * venture_claims_service_get:
 * @database: the owning database
 * Returns: (transfer none): the database's claims service
 */
VentureClaimsService *venture_claims_service_get(VentureDatabase *database);

/**
 * venture_claims_service_submit:
 * @self: the service
 * @claim: a draft claim
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: TRUE after the claim is submitted
 */
gboolean venture_claims_service_submit(VentureClaimsService *self, VentureEntity *claim,
	const VentureActor *actor, GError **error);

/**
 * venture_claims_service_approve:
 * @self: the service
 * @claim: a submitted claim
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: TRUE after approval
 */
gboolean venture_claims_service_approve(VentureClaimsService *self, VentureEntity *claim,
	const VentureActor *actor, GError **error);

/**
 * venture_claims_service_pay:
 * @self: the service
 * @claim: an approved claim
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Posts expense plus cash or AP through the posting service.
 * Returns: TRUE after payment
 */
gboolean venture_claims_service_pay(VentureClaimsService *self, VentureEntity *claim,
	const VentureActor *actor, GError **error);

/**
 * venture_claims_save_hook: (skip)
 */
gboolean venture_claims_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);

/**
 * venture_claims_check_write: (skip)
 */
gboolean venture_claims_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);

G_END_DECLS
#endif
