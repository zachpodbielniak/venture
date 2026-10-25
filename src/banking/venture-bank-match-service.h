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
 * venture_bank_account_ledger_hook: (skip)
 * @database: database
 * @record: proposed record
 * @actor: (nullable): who is writing
 * @handled: (out): set when this hook performed the whole save
 * @error: (out) (optional): error
 *
 * A new bank account with no ledger account gets its own: an asset for
 * checking, savings, cash and investments, a liability for cards and
 * loans, named after the bank account, in the book currency when none is
 * given. Both are saved in one transaction, or neither.
 *
 * Returns: TRUE unless the save was attempted and failed
 */
gboolean venture_bank_account_ledger_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);
/**
 * venture_bank_transaction_candidates:
 * @database: database
 * @transaction: bank transaction
 * @error: (out) (optional): error
 * Returns: (transfer full) (element-type VentureEntity) (nullable): owned candidates
 */
GPtrArray *venture_bank_transaction_candidates(VentureDatabase *database, VentureEntity *transaction, GError **error);
/**
 * venture_bank_transaction_candidates_search:
 * @database: database
 * @transaction: bank transaction
 * @search: (nullable): case-folded token that must appear in description or reference
 * @error: (out) (optional): error
 * Returns: (transfer full) (element-type VentureEntity) (nullable): owned candidates
 */
GPtrArray *venture_bank_transaction_candidates_search(VentureDatabase *database, VentureEntity *transaction,
	const gchar *search, GError **error);
/**
 * venture_bank_candidate_amount:
 * @record: proposed cash document
 * Returns: (transfer full) (nullable): signed bank contribution, or NULL for an unsupported document
 */
VentureMoney *venture_bank_candidate_amount(VentureEntity *record);
/**
 * venture_bank_candidate_date:
 * @record: proposed cash document
 * Returns: (transfer full) (nullable): the document's accounting date
 */
GDateTime *venture_bank_candidate_date(VentureEntity *record);
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
