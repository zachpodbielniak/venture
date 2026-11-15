/*
 * venture-settlement-private.h - The open-balance fold receivables and
 * payables share
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An invoice and a vendor bill are settled the same way: the issue, plus
 * late fees, less allocations, plus refunds of each allocation, nothing
 * open once voided. The arithmetic is the vk_settlement kernel
 * (src/kernels/vk_settlement.tlw); this is the C side both services
 * use to hand it the records they read. Not installed: include
 * "receivables/venture-settlement-private.h" only from src/receivables
 * and src/payables.
 */

#ifndef VENTURE_SETTLEMENT_PRIVATE_H
#define VENTURE_SETTLEMENT_PRIVATE_H

#include "venture.h"

G_BEGIN_DECLS

/**
 * VentureSettlementRecords: (skip)
 *
 * The records of one document's balance, in the order the kernel folds
 * them: the document's events, then each allocation followed by its
 * refunds.
 */
typedef struct _VentureSettlementRecords VentureSettlementRecords;

/**
 * venture_settlement_records_new: (skip)
 * @who: the service name errors are prefixed with, e.g.
 *   "VentureSettlementService"
 *
 * Returns: (transfer full): an empty set of records
 */
VentureSettlementRecords *
venture_settlement_records_new(
	const gchar	*who
);

/**
 * venture_settlement_records_free: (skip)
 * @self: (nullable): records
 */
void
venture_settlement_records_free(
	VentureSettlementRecords	*self
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSettlementRecords, venture_settlement_records_free)

/**
 * venture_settlement_records_add_events: (skip)
 * @self: records
 * @events: (element-type VentureEntity): a document's events, in the
 *   order read
 * @late_fees: whether a "late_fee" event adds to the balance (invoices)
 *
 * Adds the events that bear on the balance: "issue", "void" and, when
 * @late_fees, "late_fee". Other kinds are left out.
 */
void
venture_settlement_records_add_events(
	VentureSettlementRecords	*self,
	GPtrArray			*events,
	gboolean			 late_fees
);

/**
 * venture_settlement_records_add_allocation: (skip)
 * @self: records
 * @allocation: a payment allocated to the document
 * @refunds: (nullable) (element-type VentureEntity): what was refunded
 *   of @allocation, in the order read
 *
 * Adds @allocation, which lowers the balance, then each of @refunds,
 * which raise it again.
 */
void
venture_settlement_records_add_allocation(
	VentureSettlementRecords	*self,
	VentureEntity			*allocation,
	GPtrArray			*refunds
);

/**
 * venture_settlement_records_balance: (skip)
 * @self: records
 * @open: (out) (optional): set to %TRUE when the document is issued and
 *   not voided, so its payments bear on the balance
 * @error: return location for a #GError
 *
 * Folds the records with vk_settlement_balance(). Nothing issued is a
 * zero with no currency; a void is a zero in the issue's currency at the
 * exponent the issued total reached; otherwise the open amount, which is
 * zero when paid and negative when overpaid. A record without an amount,
 * in another currency, or that takes the total out of range is refused
 * as adding #VentureMoney values refused it.
 *
 * Returns: (transfer full) (nullable): the balance, or %NULL with @error
 *   set
 */
VentureMoney *
venture_settlement_records_balance(
	VentureSettlementRecords	 *self,
	gboolean			 *open,
	GError				**error
);

G_END_DECLS

#endif
