/*
 * venture-currency.c - User-defined currencies, from their records
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two jobs: refuse a currency record the money type could not honour, and
 * keep the process-wide registry equal to the table. The registry is what
 * every formatter and parser consults, from any writer and any thread, so
 * it is fed from the database's own signals rather than from the handlers
 * that happen to save a currency today.
 */

#include "venture.h"

#include <string.h>

/* The key the installed state hangs off the database under. */
#define VENTURE_CURRENCY_STATE_KEY "venture-currency-state"

/*
 * What the signal handlers share. A write inside a transaction reloads the
 * registry at once and marks it pending, so the transaction's end -- commit
 * or rollback -- reloads it again from what actually stayed.
 */
typedef struct
{
	gboolean pending;
} VentureCurrencyState;

/* --- The save validator ----------------------------------------------------- */

static gboolean
venture_currency_refuse(
	GError		**error,
	const gchar	 *message
){
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, message);
	return FALSE;
}

/*
 * The rules a currency record must meet, on every writer: the form, the
 * API, an approved staged change and the assistant. They are the rules
 * venture_currency_register() applies, checked here so that a record the
 * registry would refuse is never written -- a row that saves but does not
 * load is a currency that silently displays as two-decimal nothing.
 */
static gboolean
venture_currency_validate(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autofree gchar *code = NULL;
	g_autofree gchar *symbol = NULL;
	g_autofree gchar *denominations = NULL;
	gint64 exponent;
	gsize i;

	(void)database;
	(void)user_data;

	g_object_get(entity, "code", &code, "exponent", &exponent,
	             "symbol", &symbol, "denominations", &denominations, NULL);

	/* Stored in the spelling amounts use, so "gold" and "GOLD" cannot
	 * become two currencies that display differently. */
	if (NULL != code)
	{
		g_strstrip(code);

		for (i = 0; '\0' != code[i]; i++)
			code[i] = g_ascii_toupper(code[i]);

		g_object_set(entity, "code", code, NULL);
	}

	if (!venture_currency_is_valid(code))
		return venture_currency_refuse(error,
			"A currency code is 2 to 15 characters: a letter, then letters, "
			"digits or underscores");

	/* Redefining USD would change every figure in the books without
	 * touching a row; the built-in codes are not the operator's to edit. */
	if (venture_currency_is_builtin(code))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is a built-in ISO 4217 currency and cannot be defined here",
		            code);
		return FALSE;
	}

	if ((exponent < 0) || (exponent > VENTURE_MONEY_MAX_EXPONENT))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "A currency's decimal places must be 0 to %d",
		            VENTURE_MONEY_MAX_EXPONENT);
		return FALSE;
	}

	/*
	 * Both are fixed once saved. Amounts name their currency by its code,
	 * so a new code orphans all of them; and every amount entered without
	 * decimals is scaled to the exponent, so changing it would make the
	 * same text mean a different number from one day to the next. The
	 * column flag says so but nothing generic enforces it, hence here.
	 */
	if (NULL != previous)
	{
		g_autofree gchar *old_code = NULL;
		gint64 old_exponent;

		g_object_get(previous, "code", &old_code, "exponent", &old_exponent, NULL);

		if (0 != g_strcmp0(old_code, code))
			return venture_currency_refuse(error,
				"A currency's code cannot change once saved; create a new currency");

		if (old_exponent != exponent)
			return venture_currency_refuse(error,
				"A currency's decimal places cannot change once saved; create a new currency");
	}

	if ((NULL != symbol) && (strlen(symbol) > VENTURE_CURRENCY_SYMBOL_MAX))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "A currency symbol may be at most %d bytes",
		            VENTURE_CURRENCY_SYMBOL_MAX);
		return FALSE;
	}

	return venture_currency_check_denominations(denominations, error);
}

/* --- The registry ------------------------------------------------------------ */

gboolean
venture_currency_load_registry(
	VentureDatabase	 *database,
	GError		**error
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) codes = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	/* The registry describes the install, not what a caller may read. */
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);

	query = venture_query_new(VENTURE_TYPE_CURRENCY);
	venture_query_set_limit(query, 0);
	/* Deleted rows too: amounts stored in a deleted currency must keep
	 * displaying as they always did. */
	venture_query_set_include_deleted(query, TRUE);

	rows = venture_database_find(database, query, error);

	if (NULL == rows)
		return FALSE;

	codes = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row;
		g_autofree gchar *code = NULL;
		g_autofree gchar *symbol = NULL;
		g_autofree gchar *denominations = NULL;
		g_autoptr(GError) refused = NULL;
		gint64 exponent;
		gint position;

		row = g_ptr_array_index(rows, i);
		g_object_get(row, "code", &code, "exponent", &exponent,
		             "symbol", &symbol, "symbol-position", &position,
		             "denominations", &denominations, NULL);

		if ((exponent < 0) || (exponent > VENTURE_MONEY_MAX_EXPONENT) ||
		    !venture_currency_register(code, (guint8)exponent, symbol,
		                               VENTURE_SYMBOL_POSITION_SUFFIX == position,
		                               denominations, &refused))
		{
			/* A message, not a warning: a row a later release cannot
			 * honour must not stop the server starting. */
			g_message("Currency #%" G_GINT64_FORMAT " (%s) is not loaded: %s",
			          venture_entity_get_id(row), (NULL != code) ? code : "",
			          (NULL != refused) ? refused->message : "exponent out of range");
			continue;
		}

		g_ptr_array_add(codes, g_steal_pointer(&code));
	}

	/* Registered first, pruned second: a currency that still exists is
	 * never missing from the registry, even between the two steps. */
	g_ptr_array_add(codes, NULL);
	venture_currency_retain_registered((const gchar *const *)codes->pdata);

	return TRUE;
}

/* Reloads, reporting a failure the way a signal handler can. */
static void
venture_currency_reload(VentureDatabase *database)
{
	g_autoptr(GError) local_error = NULL;

	if (!venture_currency_load_registry(database, &local_error))
		g_message("Cannot reload currencies: %s", local_error->message);
}

/*
 * Any audited write to a currency -- a create, an edit, a delete or a
 * restore -- reloads the registry at once, so the page that saved it and
 * the rest of the same transaction already format in it.
 */
static void
venture_currency_on_audit(
	VentureDatabase	*database,
	VentureEntity	*entry,
	gpointer	 user_data
){
	VentureCurrencyState *state;
	g_autofree gchar *target_type = NULL;

	state = user_data;

	g_object_get(entry, "target-type", &target_type, NULL);

	if (0 != g_strcmp0(target_type, "currency"))
		return;

	state->pending = TRUE;
	venture_currency_reload(database);
}

/*
 * And again once the transaction around it ends. On commit this changes
 * nothing; on rollback it takes out the currency the audit handler put in,
 * which would otherwise format amounts for a row that was never written.
 */
static void
venture_currency_on_transaction_finished(
	VentureDatabase	*database,
	gboolean	 committed,
	gpointer	 user_data
){
	VentureCurrencyState *state;

	(void)committed;
	state = user_data;

	if (!state->pending)
		return;

	state->pending = FALSE;
	venture_currency_reload(database);
}

void
venture_currency_install(VentureContext *context)
{
	VentureDatabase *database;
	VentureCurrencyState *state;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* A second context over the same database (the tests make several)
	 * shares the first one's validator and handlers; it only needs the
	 * registry to describe this database rather than the last one. */
	if (NULL == g_object_get_data(G_OBJECT(database), VENTURE_CURRENCY_STATE_KEY))
	{
		state = g_new0(VentureCurrencyState, 1);
		g_object_set_data_full(G_OBJECT(database), VENTURE_CURRENCY_STATE_KEY,
		                       state, g_free);

		venture_database_add_save_validator(database, VENTURE_TYPE_CURRENCY,
		                                    venture_currency_validate, NULL, NULL);

		/* The state lives exactly as long as the database that emits
		 * these, so it needs no reference of its own. */
		g_signal_connect(database, "audit",
		                 G_CALLBACK(venture_currency_on_audit), state);
		g_signal_connect(database, "transaction-finished",
		                 G_CALLBACK(venture_currency_on_transaction_finished), state);
	}

	/*
	 * A context is usually made before the schema is migrated (the server
	 * migrates after building it, so the migration can see the module
	 * masks), and then there is no table to read yet. That is not an
	 * error: venture_database_migrate() loads the registry again once the
	 * table exists, so the failure is dropped rather than logged once per
	 * context. Only an install that does not migrate at startup depends
	 * on this load.
	 */
	venture_currency_load_registry(database, NULL);
}

gboolean
venture_currency_database_migrated(
	VentureDatabase	 *database,
	GError		**error
){
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	if (NULL == g_object_get_data(G_OBJECT(database), VENTURE_CURRENCY_STATE_KEY))
		return TRUE;

	return venture_currency_load_registry(database, error);
}
