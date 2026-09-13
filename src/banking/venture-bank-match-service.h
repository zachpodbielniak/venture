/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BANK_MATCH_SERVICE_H
#define VENTURE_BANK_MATCH_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_BANK_MATCH_SERVICE (venture_bank_match_service_get_type())
G_DECLARE_FINAL_TYPE(VentureBankMatchService, venture_bank_match_service, VENTURE, BANK_MATCH_SERVICE, GObject)
/**
 * venture_database_get_bank_match_service:
 * @database: owning database
 * Returns: (transfer none): its banking service
 */
VentureBankMatchService *venture_database_get_bank_match_service(VentureDatabase *database);
/**
 * venture_bank_check_write: (skip)
 * @database: database
 * @record: proposed record
 * @removal: deletion, restoration or purge
 * @error: (out) (optional): error
 * Returns: TRUE if permitted at the database boundary
 */
gboolean venture_bank_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error);
/**
 * venture_bank_transaction_candidates:
 * @database: database
 * @transaction: bank transaction
 * @error: (out) (optional): error
 * Returns: (transfer full) (element-type VentureEntity) (nullable): owned candidates
 */
GPtrArray *venture_bank_transaction_candidates(VentureDatabase *database, VentureEntity *transaction, GError **error);
/**
 * venture_bank_match_service_execute:
 * @self: service
 * @action: import, auto, match, unmatch, exclude, create or reconcile
 * @id: account for import, statement for auto/reconcile, transaction otherwise
 * @args: (nullable): action parameters in JSON wire spelling
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error
 * Returns: (transfer full) (nullable): the action's persisted result; changes are atomic
 */
VentureEntity *venture_bank_match_service_execute(VentureBankMatchService *self, const gchar *action,
	gint64 id, JsonObject *args, const VentureActor *actor, GError **error);
/**
 * venture_bank_register_reports:
 * @registry: report registry
 * Registers statement evidence and posted-balance reporting.
 */
void venture_bank_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
