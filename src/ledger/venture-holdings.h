/*
 * venture-holdings.h - What each wallet, till or character holds
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A holding is an `account` that carries a `location-id`: a wallet, a
 * till, a petty-cash tin, a game character's purse. It holds any number of
 * currencies, and what it holds in each comes from exactly one place,
 * decided by the currency's book treatment:
 *
 *  - a posted currency (valued or separate_book) moves a holding through
 *    the account's journal lines -- a sale paid into it, an expense paid
 *    out of it, a session's money yield, a transfer;
 *  - a memo currency never posts, so the posting service writes each memo
 *    line on a holding account as a `holding_txn` movement instead, in the
 *    same transaction as whatever journals the document did post.
 *
 * The balance of a holding in a currency is the sum of both, so a
 * currency whose treatment changed keeps its history. A holding may not go
 * below zero at any moment unless its account allows it -- the running
 * balance from a change's date onward, not only today's -- and ordinary
 * accounts, which carry no location, are never judged.
 */

#ifndef VENTURE_HOLDINGS_H
#define VENTURE_HOLDINGS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_HOLDINGS_TRANSFER_RULE:
 *
 * The rule name a transfer's journal and movements carry. The holdings
 * report reads it to count a transfer as moved, not earned or spent.
 */
#define VENTURE_HOLDINGS_TRANSFER_RULE "holding_transfer"

/**
 * VENTURE_HOLDINGS_SESSION_RULE:
 *
 * The rule name a session's money yield posts under.
 */
#define VENTURE_HOLDINGS_SESSION_RULE "session_yield"

/**
 * venture_holdings_install:
 * @context: the wiring
 *
 * Installs the `holding_txn` save validator, the posting service's guard
 * that refuses a journal taking a holding below zero, and the `transfer`
 * action on `location`. A second context over the same database installs
 * nothing twice.
 */
void
venture_holdings_install(VentureContext *context);

/**
 * venture_holdings_check_write:
 * @database: the database being written
 * @entity: the record being saved or removed
 * @removal: whether this is a delete, purge or restore
 * @error: (out) (optional): return location for a #GError
 *
 * The removal half of the holding rules, in the database's subsystem guard
 * list: a movement the ledger derived from a document is removed only by
 * the ledger, and deleting (or restoring) a movement typed by hand may
 * not leave its holding below zero from the movement's date on. Every
 * other write passes.
 *
 * Returns: %TRUE when the write may proceed
 */
gboolean
venture_holdings_check_write(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	gboolean	  removal,
	GError		**error
);

/**
 * venture_holdings_account_for_location:
 * @database: the database
 * @organization_id: the organization the holding belongs to
 * @location_id: the place or character
 * @create: whether to make the account when the location has none
 * @actor: (nullable): audit actor for a created account
 * @out_account_id: (out): the holding account, 0 when none and not created
 * @error: (out) (optional): return location for a #GError
 *
 * The one account holding money at @location_id. A location with two is
 * refused rather than guessed between -- name the account itself instead.
 * A created account is an asset coded `<org>:holding:<location>` and
 * named after the location's path.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_holdings_account_for_location(
	VentureDatabase		 *database,
	gint64			  organization_id,
	gint64			  location_id,
	gboolean		  create,
	const VentureActor	 *actor,
	gint64			 *out_account_id,
	GError			**error
);

/**
 * venture_holdings_balance:
 * @database: the database
 * @account_id: a holding account
 * @currency: the currency asked about
 * @exclude_txn_id: a movement to leave out (the one being saved), or 0
 * @error: (out) (optional): return location for a #GError
 *
 * What the account holds in @currency now: its posted journal lines in
 * that original currency plus its live memo movements, every date.
 *
 * Returns: (transfer full) (nullable): the balance, or %NULL with @error set
 */
VentureMoney *
venture_holdings_balance(
	VentureDatabase	 *database,
	gint64		  account_id,
	const gchar	 *currency,
	gint64		  exclude_txn_id,
	GError		**error
);

/**
 * venture_holdings_record_memo:
 * @database: the database, inside the caller's transaction
 * @header: the journal header the lines belong to: organization, date,
 *   source type and id, rule name
 * @lines: (element-type VentureJournalLine): the document's lines
 * @replace: %TRUE to replace this source and rule's earlier movements
 *   (unchanged ones are kept as they are); %FALSE to append
 * @actor: (nullable): audit actor
 * @out_movements: (out) (optional) (transfer full) (element-type VentureHoldingTxn):
 *   the movements now standing for these lines
 * @error: (out) (optional): return location for a #GError
 *
 * Writes each line in a memo currency whose account carries a location as
 * that holding's movement: a debit adds, a credit takes away. The posting
 * service calls this beside every automatic journal; it is not for other
 * callers.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_holdings_record_memo(
	VentureDatabase		 *database,
	VentureJournal		 *header,
	GPtrArray		 *lines,
	gboolean		  replace,
	const VentureActor	 *actor,
	GPtrArray		**out_movements,
	GError			**error
);

/**
 * venture_holdings_clear_memo:
 * @database: the database, inside the caller's transaction
 * @organization_id: the organization
 * @source_type: the document's type
 * @source_id: the document's id
 * @rule_name: the rule whose movements to remove
 * @actor: (nullable): audit actor
 * @error: (out) (optional): return location for a #GError
 *
 * Removes the movements a document's rule wrote, when a save leaves it
 * nothing to hold (a memo refund taken back to zero).
 *
 * Returns: %TRUE on success
 */
gboolean
venture_holdings_clear_memo(
	VentureDatabase		 *database,
	gint64			  organization_id,
	const gchar		 *source_type,
	gint64			  source_id,
	const gchar		 *rule_name,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_holdings_transfer:
 * @database: the database
 * @from_location_id: where the money leaves
 * @to_location_id: where it arrives, in the same organization
 * @amount: a positive amount, in any currency
 * @when: (nullable): when it moved; now when %NULL
 * @notes: (nullable): why
 * @actor: (nullable): audit actor
 * @error: (out) (optional): return location for a #GError
 *
 * Moves @amount from one location's holding to another's, making either
 * account on first use. A posted currency moves by one journal (debit the
 * destination, credit the source, rule `holding_transfer`); a memo one by a
 * pair of transfer movements. Either way the source may not go below zero
 * unless its account allows it, and nothing is written on a refusal.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_holdings_transfer(
	VentureDatabase		 *database,
	gint64			  from_location_id,
	gint64			  to_location_id,
	const VentureMoney	 *amount,
	GDateTime		 *when,
	const gchar		 *notes,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_holdings_report:
 * @context: the wiring
 * @period: (nullable): earned, spent and transferred are counted in it;
 *   the balance is as of its end
 * @options: (nullable): `organization_id`, `location_id` (with every
 *   location beneath it), `currency`, `as_of`
 * @error: (out) (optional): return location for a #GError
 *
 * One row per location and currency: earned, spent, transferred, net and
 * balance, with how the currency reaches the books. The book currency
 * comes first under each location.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL with @error set
 */
VentureReportResult *
venture_holdings_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_holdings_register_report:
 * @registry: the report registry
 *
 * Registers `holdings`. It belongs to the ledger module.
 */
void
venture_holdings_register_report(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_HOLDINGS_H */
