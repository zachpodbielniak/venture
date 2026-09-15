/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SETUP_SERVICE_H
#define VENTURE_SETUP_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_SETUP_SERVICE (venture_setup_service_get_type())
G_DECLARE_FINAL_TYPE(VentureSetupService, venture_setup_service, VENTURE, SETUP_SERVICE, GObject)
/**
 * venture_setup_service_get:
 * @database: owning database
 * Returns: (transfer none): the database's accounting setup service
 */
VentureSetupService *venture_setup_service_get(VentureDatabase *database);
/**
 * venture_setup_service_checklist:
 * @self: service
 * @organization_id: legal entity
 * @error: (out) (optional): error
 * Returns: (transfer full) (nullable): JSON array of checklist steps
 */
JsonNode *venture_setup_service_checklist(VentureSetupService *self, gint64 organization_id, GError **error);
/**
 * venture_setup_service_preview:
 * @self: service
 * @organization_id: legal entity
 * @payload: (nullable): setup answers
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error
 * Returns: (transfer full) (nullable): persisted resumable setup
 */
VentureEntity *venture_setup_service_preview(VentureSetupService *self, gint64 organization_id,
	JsonObject *payload, const VentureActor *actor, GError **error);
/**
 * venture_setup_service_complete:
 * @self: service
 * @setup: a previewed setup
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error
 * Returns: TRUE after mappings, calendar, bank and openings are in place
 */
gboolean venture_setup_service_complete(VentureSetupService *self, VentureAccountingSetup *setup,
	const VentureActor *actor, GError **error);
/**
 * venture_setup_resolve_account:
 * @database: database
 * @organization_id: legal entity
 * @classification: semantic role such as cash
 * @subject_type: (nullable): organization, bank_account or product
 * @subject_id: override subject, or zero
 * @as_of: (nullable): dated mapping cutoff
 * @error: (out) (optional): error
 * Returns: the mapped account id, or 0 when none is configured
 */
gint64 venture_setup_resolve_account(VentureDatabase *database, gint64 organization_id,
	const gchar *classification, const gchar *subject_type, gint64 subject_id,
	GDateTime *as_of, GError **error);
/**
 * venture_setup_account_classified:
 * @database: database
 * @organization_id: legal entity
 * @account_id: account, including descendants of a mapped parent
 * @classification: semantic role
 * @as_of: (nullable): dated mapping cutoff
 * Returns: TRUE when the account is classified that way
 */
gboolean venture_setup_account_classified(VentureDatabase *database, gint64 organization_id,
	gint64 account_id, const gchar *classification, GDateTime *as_of);
/**
 * venture_setup_check_write: (skip)
 * Returns: TRUE if the write may proceed
 */
gboolean venture_setup_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_setup_seed_defaults:
 * @database: database
 * @organization_id: legal entity whose seeded chart should be classified
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error
 * Returns: TRUE when org-level maps exist
 */
gboolean venture_setup_seed_defaults(VentureDatabase *database, gint64 organization_id,
	const VentureActor *actor, GError **error);
/**
 * venture_setup_actions_register:
 * @database: database owning the records
 */
void venture_setup_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
