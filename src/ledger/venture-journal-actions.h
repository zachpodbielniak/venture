/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_JOURNAL_ACTIONS_H
#define VENTURE_JOURNAL_ACTIONS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_journal_actions_register:
 * @database: owning ledger database
 *
 * Registers journal post, reverse and type-level create-and-post callbacks.
 * The entity registry masks them together with the ledger module.
 */
void venture_journal_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
