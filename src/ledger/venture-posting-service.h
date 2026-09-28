/*
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef VENTURE_POSTING_SERVICE_H
#define VENTURE_POSTING_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_POSTING_SERVICE (venture_posting_service_get_type())
G_DECLARE_FINAL_TYPE(VenturePostingService, venture_posting_service,
	VENTURE, POSTING_SERVICE, GObject)

/**
 * venture_database_get_posting_service:
 * @database: owning database
 * Returns: (transfer none): the database's single posting service
 */
VenturePostingService *venture_database_get_posting_service(VentureDatabase *database);
/**
 * venture_context_get_posting_service:
 * @context: application context
 * Returns: (transfer none) (nullable): service, or NULL when ledger is disabled
 */
VenturePostingService *venture_context_get_posting_service(VentureContext *context);
/**
 * venture_posting_service_get_rules:
 * @self: posting service
 * Returns: (transfer none): plugin-extensible rules, including the built-ins
 */
VenturePostingRuleRegistry *venture_posting_service_get_rules(VenturePostingService *self);
/**
 * venture_posting_service_post:
 * @self: posting service
 * @journal: unsaved header or an existing draft (never modified by this call)
 * @lines: (element-type VentureJournalLine) (nullable): unsaved lines for a new
 *   header; NULL posts all saved lines of an existing draft
 * @exchange_policy: (nullable): required for mixed original currencies
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal or storage error
 *
 * Joins an enclosing database transaction. The caller must roll it back on
 * failure. Inputs are untouched even when persistence fails. A repeated post
 * of a saved journal is a conflict; document-rule posts also reject duplicates.
 *
 * Returns: (transfer full) (nullable): posted header, or NULL on failure
 */
VentureJournal *venture_posting_service_post(VenturePostingService *self,
	VentureJournal *journal, GPtrArray *lines, VentureExchangePolicy *exchange_policy,
	const VentureActor *actor, GError **error);
/**
 * venture_posting_service_post_entries:
 * @self: the database's canonical posting service
 * @entries: (element-type VentureLedgerEntry): unsaved entry-shaped inputs
 * @exchange_policy: (nullable): explicit valuation policy for mixed currencies
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal or storage error
 *
 * Integration adapter for modules using venture_database_save_ledger_transaction().
 * All entries must share a nonempty transaction-id, organization, source type/id
 * and accounting date. The first amount chooses the book currency. Missing source
 * linkage means a manual journal against the organization; missing dates mean now.
 * A repeated transaction-id within an organization is refused, including after
 * reversal. Inputs stay untouched. This translates then calls the single post
 * service; it does not write ledger rows independently.
 *
 * Returns: TRUE if the journal was posted into the enclosing transaction
 */
gboolean venture_posting_service_post_entries(VenturePostingService *self,
	GPtrArray *entries, VentureExchangePolicy *exchange_policy,
	const VentureActor *actor, GError **error);
/**
 * venture_posting_service_post_document:
 * @self: posting service
 * @rule_name: registered rule key
 * @source: persisted source document
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): error location
 * Returns: (transfer full) (nullable): posted journal
 */
VentureJournal *venture_posting_service_post_document(VenturePostingService *self,
	const gchar *rule_name, VentureEntity *source, const VentureActor *actor, GError **error);
/**
 * venture_posting_service_book_currency:
 * @self: posting service
 * @organization_id: legal entity
 * @error: (out) (optional): the organization does not exist
 *
 * The organization's book currency: its default currency, or the install's
 * when it names none. Every automatic journal is anchored on it.
 *
 * Returns: (transfer full) (nullable): the currency code
 */
gchar *venture_posting_service_book_currency(VenturePostingService *self,
	gint64 organization_id, GError **error);
/**
 * venture_posting_service_route_currency:
 * @self: posting service
 * @organization_id: legal entity posting the amount
 * @currency: the amount's currency
 * @when: (nullable): accounting date, now when %NULL
 * @route: (out) (optional): where the amount goes
 * @book_currency: (out) (optional) (transfer full): the organization's book currency
 * @error: (out) (optional): storage error, or an invalid code
 *
 * The one place the ledger decides what an amount in @currency does when
 * @organization_id posts it on @when. Memo currencies are never posted; the
 * book currency is posted as it is; a separate-book currency (or any
 * currency when the book currency itself is kept separate) goes to a
 * journal of its own; a valued currency is converted into the book
 * currency when the organization's exchange-rate table has a rate on
 * @when, and otherwise also keeps a journal of its own -- a rate is never
 * invented. Reports, holdings and inventory ask this rather than repeating
 * the rule.
 *
 * Returns: %TRUE if @route was answered
 */
gboolean venture_posting_service_route_currency(VenturePostingService *self,
	gint64 organization_id, const gchar *currency, GDateTime *when,
	VentureBookRoute *route, gchar **book_currency, GError **error);
/**
 * venture_posting_service_book_label:
 * @self: posting service
 * @organization_id: legal entity whose books are being read
 * @currency: the currency of one section of those books
 * @when: (nullable): the date the section is read at, now when %NULL
 * @error: (out) (optional): storage error, or an invalid code
 *
 * How a statement names a section of an organization's books, from the
 * same rule venture_posting_service_route_currency() applies: "Book
 * currency: GOLD" for the book currency, "Separate book: TICKET" for a
 * currency kept apart by its treatment, "Own book: EUR (no rate to GOLD)"
 * for a valued currency that had no rate, and "Own book: EUR (posted
 * before a rate to GOLD)" when a rate exists now but the section holds
 * journals posted without one. A memo currency posts nothing, so its
 * label ("Memo: X (posted before it was memo)") is only ever read for
 * journals older than the treatment. Converted amounts are already in the
 * book-currency section and never get a section of their own.
 *
 * Returns: (transfer full) (nullable): the label, or %NULL on error
 */
gchar *venture_posting_service_book_label(VenturePostingService *self,
	gint64 organization_id, const gchar *currency, GDateTime *when, GError **error);
/**
 * venture_posting_service_post_by_currency:
 * @self: posting service
 * @header: unsaved header: organization, date, source, memo, rule name and
 *   an optional posting key; its currency is ignored
 * @lines: (element-type VentureJournalLine): unsaved lines, in any currencies
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal or storage error
 *
 * Posts @lines by the rule venture_posting_service_route_currency()
 * decides, in one transaction: book-currency and converted lines in one
 * journal valued by the organization's exchange-rate table, each
 * separate-book (or rate-less) currency in a balanced journal of its own,
 * memo lines not at all. When the lines land in more than one journal,
 * each is balanced through the currency clearing account in its own
 * currency; a single journal must balance by itself. Every journal shares
 * the header's source; the first keeps the posting key and the others add
 * ":CODE" to it. A caller with lines in several currencies -- an issue that
 * consumed cost in two -- passes them all in one call.
 *
 * Returns: (transfer full) (element-type VentureJournal) (nullable): the
 *   posted journals, book journal first; empty when every line was memo
 */
GPtrArray *venture_posting_service_post_by_currency(VenturePostingService *self,
	VentureJournal *header, GPtrArray *lines, const VentureActor *actor, GError **error);
/**
 * venture_posting_service_find_source:
 * @self: posting service
 * @source_type: registered source record type
 * @source_id: source identifier
 * @organization_id: exact owning legal entity
 * @error: (out) (optional): error location
 * Returns: (transfer full) (element-type VentureJournal) (nullable): all journals,
 *   including drafts and reversals, ordered by id
 */
GPtrArray *venture_posting_service_find_source(VenturePostingService *self,
	const gchar *source_type, gint64 source_id, gint64 organization_id, GError **error);
/**
 * venture_posting_service_source_has_reversal:
 * @self: posting service
 * @source_type: registered source record type
 * @source_id: source identifier
 * @organization_id: exact owning legal entity
 * @error: (out) (optional): error location
 *
 * True when a reversing journal exists for the source. Cutover rollback
 * reverses credit postings this way; the remaining balance must then be
 * treated as consumed so the credit cannot be allocated again.
 * Returns: TRUE if a reversal is on file
 */
gboolean venture_posting_service_source_has_reversal(VenturePostingService *self,
	const gchar *source_type, gint64 source_id, gint64 organization_id, GError **error);
/**
 * venture_posting_service_reverse:
 * @self: posting service
 * @journal_id: original posted journal
 * @when: correction's accounting date
 * @memo: reason for correction
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): error location
 * Returns: (transfer full) (nullable): posted reversing journal
 */
VentureJournal *venture_posting_service_reverse(VenturePostingService *self,
	gint64 journal_id, GDateTime *when, const gchar *memo,
	const VentureActor *actor, GError **error);
/**
 * venture_posting_service_is_date_postable:
 * @self: posting service
 * @organization_id: exact legal entity
 * @when: accounting date
 * @error: (out) (optional): veto error
 *
 * Emits date-postable. Period modules connect here; ledger defines no periods.
 * Returns: TRUE unless a handler vetoes
 */
gboolean venture_posting_service_is_date_postable(VenturePostingService *self,
	gint64 organization_id, GDateTime *when, GError **error);
/**
 * venture_posting_service_account_balance:
 * @self: posting service
 * @account_id: chart account
 * @organization_id: exact legal entity (no descendant consolidation)
 * @currency: book currency
 * @as_of: inclusive accounting timestamp
 * @error: (out) (optional): error location
 *
 * Debits are positive, credits negative. Reversed originals remain posted
 * evidence; their reversing journals take effect on their own dates.
 * Returns: (transfer full) (nullable): exact signed balance
 */
VentureMoney *venture_posting_service_account_balance(VenturePostingService *self,
	gint64 account_id, gint64 organization_id, const gchar *currency,
	GDateTime *as_of, GError **error);

G_END_DECLS
#endif
