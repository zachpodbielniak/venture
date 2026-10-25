/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BOOKS_IMPORT_SERVICE_H
#define VENTURE_BOOKS_IMPORT_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_BOOKS_IMPORT_SERVICE (venture_books_import_service_get_type())
G_DECLARE_FINAL_TYPE(VentureBooksImportService, venture_books_import_service, VENTURE, BOOKS_IMPORT_SERVICE, GObject)

/**
 * venture_books_import_service_get:
 * @database: database owning the records
 *
 * Returns: (transfer none): the per-database service
 */
VentureBooksImportService *venture_books_import_service_get(VentureDatabase *database);

/**
 * venture_books_import_service_preview:
 * @self: the service
 * @organization_id: the legal entity the books land in
 * @manifest: source, start, settings and files (backup file name to CSV text)
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Parses the whole backup without writing a document: counts per source
 * object, located problems, the accounts and parties that will be created
 * or matched, the files with no home and the source ledger the import must
 * reproduce. A manifest that cannot be read at all is refused and no batch
 * is created.
 *
 * Returns: (transfer full) (nullable): the batch, state preview
 */
VentureEntity *venture_books_import_service_preview(VentureBooksImportService *self, gint64 organization_id,
	JsonObject *manifest, const VentureActor *actor, GError **error);

/**
 * venture_books_import_service_import:
 * @self: the service
 * @batch: a previewed batch
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * One transaction: the chart, the parties, the opening balances through a
 * cutover batch at the start date, then every document in date order
 * through its owning service. No receipt, reminder, dunning or SMS is sent.
 *
 * Returns: %TRUE when the batch is imported
 */
gboolean venture_books_import_service_import(VentureBooksImportService *self, VentureBooksImport *batch,
	const VentureActor *actor, GError **error);

/**
 * venture_books_import_service_reconcile:
 * @self: the service
 * @batch: an imported batch
 * @actor: (nullable): audit actor
 * @error: (out) (optional): the first failing checks
 *
 * Compares every account's ledger balance with the source ledger at the
 * export date and at every month end, and each month's document counts and
 * totals with the backup's. Appends the evidence to the report.
 *
 * Returns: %TRUE when everything ties
 */
gboolean venture_books_import_service_reconcile(VentureBooksImportService *self, VentureBooksImport *batch,
	const VentureActor *actor, GError **error);

/**
 * venture_books_import_service_activate:
 * @self: the service
 * @batch: a reconciled batch
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Re-runs reconcile, activates the opening cutover and makes the books the
 * organization's own. An active batch cannot be rolled back.
 *
 * Returns: %TRUE when active
 */
gboolean venture_books_import_service_activate(VentureBooksImportService *self, VentureBooksImport *batch,
	const VentureActor *actor, GError **error);

/**
 * venture_books_import_service_rollback_preflight:
 * @self: the service
 * @batch: the batch
 * @error: (out) (optional): failure
 *
 * Returns: (transfer full) (element-type utf8) (nullable): what would stop a rollback
 */
GPtrArray *venture_books_import_service_rollback_preflight(VentureBooksImportService *self,
	VentureBooksImport *batch, GError **error);

/**
 * venture_books_import_service_rollback:
 * @self: the service
 * @batch: a batch that is not active
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal, with every blocker
 *
 * One transaction: refunds the imported cash, voids the imported invoices
 * and bills and frees their numbers, reverses the batch's own journals and
 * expenses, rolls back the opening cutover and removes the non-ledger
 * records the batch created. Companies and accounts stay, so a corrected
 * batch matches them.
 *
 * Returns: %TRUE when rolled back
 */
gboolean venture_books_import_service_rollback(VentureBooksImportService *self, VentureBooksImport *batch,
	const VentureActor *actor, GError **error);

/**
 * venture_books_import_check_write: (skip)
 * @database: repository
 * @record: candidate write
 * @removal: TRUE when deleting
 * @error: (out) (optional): refusal
 * Returns: TRUE if the write is service-owned or not a books import record
 */
gboolean venture_books_import_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);

/**
 * venture_books_import_actions_register:
 * @database: database owning the records
 */
void venture_books_import_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
