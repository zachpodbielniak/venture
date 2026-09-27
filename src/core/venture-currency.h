/*
 * venture-currency.h - User-defined currencies, from their records
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A `currency` record defines a unit of account the ISO 4217 table does
 * not have: points, a commodity, a game's gold. The money type knows how
 * to format and parse such a currency once it is in the process-wide
 * registry (venture_currency_register() in venture-money.h); this file is
 * what puts the records there and keeps them there, and what refuses a
 * record the registry could not hold.
 */

#ifndef VENTURE_CURRENCY_H
#define VENTURE_CURRENCY_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_CURRENCY_SYMBOL_MAX:
 *
 * The longest symbol a currency record may carry, in bytes. Long enough
 * for "pts" or a multi-byte sign, short enough that a figure stays a
 * figure in a table column.
 */
#define VENTURE_CURRENCY_SYMBOL_MAX (16)

/**
 * venture_currency_install:
 * @context: the wiring
 *
 * Installs the currency record's save validator, loads every currency
 * record into the registry, and keeps the registry in step with the table:
 * a write to a currency reloads it at once (so the rest of the same request
 * formats with it) and again when the transaction around it finishes (so a
 * rollback takes it back out). Called once by the context.
 */
void
venture_currency_install(VentureContext *context);

/**
 * venture_currency_load_registry:
 * @database: the database to read
 * @error: (out) (optional): return location for a #GError
 *
 * Replaces the user-defined part of the currency registry with the
 * `currency` rows in @database -- **including soft-deleted ones**. A
 * deleted currency still has amounts stored in it, and deleting the record
 * must never change how those amounts display or what their natural
 * exponent is. A row the registry refuses (a code that a later build made
 * built-in, say) is skipped with a message rather than stopping the load.
 *
 * Returns: %TRUE if the table was read
 */
gboolean
venture_currency_load_registry(
	VentureDatabase	 *database,
	GError		**error
);

/**
 * venture_currency_database_migrated:
 * @database: a database whose schema was just brought up to date
 * @error: (out) (optional): return location for a #GError
 *
 * Loads the registry from @database now that its currency table exists,
 * but only when a context installed currencies on it. A context is built
 * before the server migrates, so its own load finds no table on a fresh
 * install; and a scratch database migrated to check a backup must not
 * replace the running install's currencies with its own.
 *
 * Returns: %TRUE unless the load failed
 */
gboolean
venture_currency_database_migrated(
	VentureDatabase	 *database,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_CURRENCY_H */
