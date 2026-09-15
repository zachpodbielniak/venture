/*
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef VENTURE_JOURNAL_H
#define VENTURE_JOURNAL_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

/**
 * VentureJournalState:
 * @VENTURE_JOURNAL_DRAFT: editable, excluded from balances
 * @VENTURE_JOURNAL_POSTED: committed, immutable evidence
 * @VENTURE_JOURNAL_REVERSED: posted evidence with a separate reversing journal
 */
typedef enum
{
	VENTURE_JOURNAL_DRAFT,
	VENTURE_JOURNAL_POSTED,
	VENTURE_JOURNAL_REVERSED
} VentureJournalState;

#define VENTURE_TYPE_JOURNAL_STATE (venture_journal_state_get_type())
/**
 * venture_journal_state_get_type:
 * Returns: the journal state enum type
 */
GType venture_journal_state_get_type(void) G_GNUC_CONST;

#define VENTURE_TYPE_JOURNAL (venture_journal_get_type())
VENTURE_DECLARE_ENTITY(VentureJournal, venture_journal, JOURNAL)
#define VENTURE_TYPE_JOURNAL_LINE (venture_journal_line_get_type())
VENTURE_DECLARE_ENTITY(VentureJournalLine, venture_journal_line, JOURNAL_LINE)
#define VENTURE_TYPE_EXCHANGE_RATE (venture_exchange_rate_get_type())
VENTURE_DECLARE_ENTITY(VentureExchangeRate, venture_exchange_rate, EXCHANGE_RATE)

/**
 * venture_exchange_rate_new:
 * Returns: (transfer full): a dated exact rate from one currency into another
 */

/**
 * venture_journal_new:
 * Returns: (transfer full): an editable journal
 */
/**
 * venture_journal_line_new:
 * Returns: (transfer full): an editable journal line
 */

G_END_DECLS
#endif
