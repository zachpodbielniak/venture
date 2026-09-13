/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BILLING_SERVICE_H
#define VENTURE_BILLING_SERVICE_H
G_BEGIN_DECLS
#define VENTURE_TYPE_BILLING_SERVICE (venture_billing_service_get_type())
G_DECLARE_FINAL_TYPE(VentureBillingService, venture_billing_service, VENTURE, BILLING_SERVICE, GObject)
/**
 * venture_billing_service_get:
 * @database: the owning database
 * Returns: (transfer none): the database's billing service
 */
VentureBillingService *venture_billing_service_get(VentureDatabase *database);
/**
 * venture_billing_service_execute:
 * @self: the service
 * @request: an unsaved instruction; results replace it only on success
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): error location
 * Returns: TRUE when the entire instruction commits
 */
gboolean venture_billing_service_execute(VentureBillingService *self, VentureBillingRequest *request, const VentureActor *actor, GError **error);
/**
 * venture_billing_save_hook: (skip)
 * @database: the database with its lock held
 * @record: the proposed save
 * @actor: (nullable): the audit actor
 * @handled: (out): whether the service completed the save
 * @error: (out) (optional): error location
 * Returns: TRUE if allowed
 */
gboolean venture_billing_save_hook(VentureDatabase *database, VentureEntity *record, const VentureActor *actor, gboolean *handled, GError **error);
/**
 * venture_billing_check_removal: (skip)
 * @database: the database
 * @record: the record being removed or restored
 * @error: (out) (optional): error location
 * Returns: TRUE if no billing history is changed
 */
gboolean venture_billing_check_removal(VentureDatabase *database, VentureEntity *record, GError **error);
G_END_DECLS
#endif
