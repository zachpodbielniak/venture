/*
 * venture-holdings.c - What each wallet, till or character holds
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * See venture-holdings.h for the shape. The rules in one place:
 *
 *  - a holding is an account with a location, and there is one truth per
 *    currency: posted journal lines for a currency the ledger posts, memo
 *    movements (`holding_txn`) for one it does not. The balance adds both,
 *    so a treatment changed later keeps the history either side of it;
 *  - memo movements that came from a document are the ledger's to write,
 *    replace and remove; a person writes only adjustments, in a memo
 *    currency, and never a transfer (that has two sides);
 *  - a holding may not go below zero at any moment from a change's date
 *    on unless its account allows it. A memo movement is judged by its
 *    save validator, a journal by the posting service's `posting` signal
 *    -- both before anything is written.
 */

#include "venture.h"

#include <string.h>

#define VENTURE_HOLDINGS_STATE_KEY "venture-holdings-installed"

/*
 * The movement the ledger is writing or removing right now. Derived
 * movements belong to the document they came from: a person who could edit
 * one could make a character hold tickets no session ever paid. The
 * validator and the removal guard let only the object named here through
 * -- the pattern the sessions and inventory services use for their rows.
 */
#define VENTURE_HOLDINGS_PERMIT_KEY "venture-holdings-writing"

/* How one movement counts in the report. */
typedef enum
{
	HOLDINGS_FLOW_EARNED = 0,
	HOLDINGS_FLOW_SPENT,
	HOLDINGS_FLOW_TRANSFER
} HoldingsFlow;

/* One signed movement of one holding, from either source. */
typedef struct
{
	gint64		 account_id;
	VentureMoney	*amount;
	GDateTime	*when;
	HoldingsFlow	 flow;
} HoldingsMovement;

static void
holdings_movement_free(gpointer data)
{
	HoldingsMovement *movement;

	movement = data;
	venture_money_free(movement->amount);
	g_clear_pointer(&movement->when, g_date_time_unref);
	g_free(movement);
}

/* ==========================================================================
 * Shared
 * ========================================================================== */

static gint64
holdings_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

/* Whether the ledger module is on: journals and holding movements are its
 * types, and a hidden type reads as not registered. */
static gboolean
holdings_ledger_enabled(void)
{
	return G_TYPE_INVALID != venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "holding_txn");
}

static gboolean
holdings_permitted(
	VentureDatabase	*database,
	VentureEntity	*entity
){
	return g_object_get_data(G_OBJECT(database), VENTURE_HOLDINGS_PERMIT_KEY) ==
	       (gpointer)entity;
}

/* Saves or removes @entity as the ledger, the one writer of derived rows. */
static gboolean
holdings_write_permitted(
	VentureDatabase		 *database,
	VentureEntity		 *entity,
	gboolean		  removal,
	const VentureActor	 *actor,
	GError			**error
){
	gpointer previous;
	gboolean written;

	previous = g_object_get_data(G_OBJECT(database), VENTURE_HOLDINGS_PERMIT_KEY);
	g_object_set_data(G_OBJECT(database), VENTURE_HOLDINGS_PERMIT_KEY, entity);
	written = removal ? venture_database_delete(database, entity, actor, error)
	                  : venture_database_save(database, entity, actor, error);
	g_object_set_data(G_OBJECT(database), VENTURE_HOLDINGS_PERMIT_KEY, previous);

	return written;
}

/*
 * Reads a record under the internal scope. A rule about a holding judges
 * the holding, whoever is writing: a viewer's scope that hid half its
 * movements would let them spend what is not there.
 */
static VentureEntity *
holdings_read(
	VentureDatabase	*database,
	GType		 type,
	gint64		 id
){
	g_autoptr(VentureAccessScope) internal = NULL;

	if (id <= 0)
		return NULL;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);

	return venture_database_get(database, type, id, NULL);
}

/* "Characters / Aria", or "location #4" when it cannot be read. */
static gchar *
holdings_location_name(
	VentureDatabase	*database,
	gint64		 location_id
){
	gchar *path;

	path = venture_category_path(database, VENTURE_TYPE_LOCATION, location_id, NULL);

	return (NULL != path) ? path
	                      : g_strdup_printf("location #%" G_GINT64_FORMAT, location_id);
}

/* What a holding account is called in a refusal: its location's path. */
static gchar *
holdings_account_name(
	VentureDatabase	*database,
	VentureEntity	*account
){
	gint64 location_id;

	location_id = holdings_int(account, "location-id");

	if (location_id > 0)
		return holdings_location_name(database, location_id);

	return venture_entity_get_display_name(account);
}

static gboolean
holdings_money_add(
	VentureMoney		**total,
	const VentureMoney	 *term,
	GError			**error
){
	VentureMoney *next;

	if (NULL == *total)
	{
		*total = venture_money_copy(term);
		return TRUE;
	}

	next = venture_money_add(*total, term, error);

	if (NULL == next)
		return FALSE;

	venture_money_free(*total);
	*total = next;

	return TRUE;
}

/* ==========================================================================
 * Collecting a holding's movements
 * ========================================================================== */

/* The rule a journal was posted under -- a reversal's is the original's,
 * so reversing a transfer is still a transfer. */
static gchar *
holdings_journal_rule(
	VentureDatabase	*database,
	VentureEntity	*journal,
	GHashTable	*journals
){
	VentureEntity *original;
	gchar *rule;
	gint64 reverses;

	reverses = holdings_int(journal, "reverses-id");

	if (reverses <= 0)
	{
		g_object_get(journal, "rule-name", &rule, NULL);
		return rule;
	}

	original = g_hash_table_lookup(journals, &reverses);

	if (NULL == original)
	{
		gint64 *key;

		original = holdings_read(database, VENTURE_TYPE_JOURNAL, reverses);

		if (NULL == original)
			return NULL;

		key = g_new(gint64, 1);
		*key = reverses;
		g_hash_table_insert(journals, key, original);
	}

	g_object_get(original, "rule-name", &rule, NULL);

	return rule;
}

/*
 * The ledger half: every posted journal line on @account_id, signed (debit
 * adds), in its original currency. A reversal counts against the flow it
 * reverses -- a reversed sale takes its takings back out of "earned"
 * rather than adding the same amount to "spent" -- so correcting a
 * document never inflates both sides.
 */
static gboolean
holdings_collect_ledger(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  account_id,
	const gchar	 *currency,
	GPtrArray	 *out,
	GError		**error
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GHashTable) journals = NULL;
	guint i;

	if (G_TYPE_INVALID == venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "journal_line"))
		return TRUE;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);
	journals = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_object_unref);

	query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	venture_query_set_limit(query, 0);
	/* Journals are evidence and are never deleted; read the way the
	 * statements do, so a holding agrees with the trial balance. */
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, organization_id);

	if (!venture_query_add_filter_int(query, "account-id", VENTURE_FILTER_OP_EQ,
	                                  account_id, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;

	lines = venture_database_find(database, query, error);

	if (NULL == lines)
		return FALSE;

	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line;
		VentureEntity *journal;
		HoldingsMovement *movement;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) when = NULL;
		g_autofree gchar *rule = NULL;
		VentureJournalState state;
		VentureLedgerSide side;
		gboolean tax_book;
		gboolean reversal;
		gint64 journal_id;

		line = g_ptr_array_index(lines, i);
		g_object_get(line, "journal-id", &journal_id, "amount", &amount,
		             "side", &side, NULL);

		if ((NULL == amount) ||
		    ((NULL != currency) && (0 != g_strcmp0(amount->currency, currency))))
			continue;

		journal = g_hash_table_lookup(journals, &journal_id);

		if (NULL == journal)
		{
			gint64 *key;

			journal = holdings_read(database, VENTURE_TYPE_JOURNAL, journal_id);

			if (NULL == journal)
				continue;

			key = g_new(gint64, 1);
			*key = journal_id;
			g_hash_table_insert(journals, key, journal);
		}

		tax_book = FALSE;
		g_object_get(journal, "state", &state, "occurred-at", &when,
		             "tax-book", &tax_book, NULL);

		if (((VENTURE_JOURNAL_POSTED != state) && (VENTURE_JOURNAL_REVERSED != state)) ||
		    tax_book || (NULL == when))
			continue;

		reversal = holdings_int(journal, "reverses-id") > 0;
		rule = holdings_journal_rule(database, journal, journals);

		movement = g_new0(HoldingsMovement, 1);
		movement->account_id = account_id;
		movement->when = g_date_time_ref(when);
		movement->amount = (VENTURE_LEDGER_SIDE_DEBIT == side)
			? venture_money_copy(amount) : venture_money_negate(amount);

		if (0 == g_strcmp0(rule, VENTURE_HOLDINGS_TRANSFER_RULE))
			movement->flow = HOLDINGS_FLOW_TRANSFER;
		else if ((VENTURE_LEDGER_SIDE_DEBIT == side) != reversal)
			movement->flow = HOLDINGS_FLOW_EARNED;
		else
			movement->flow = HOLDINGS_FLOW_SPENT;

		if (NULL == movement->amount)
		{
			holdings_movement_free(movement);
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "A journal amount on this holding overflows");
			return FALSE;
		}

		g_ptr_array_add(out, movement);
	}

	return TRUE;
}

/* The memo half: every live movement of @account_id but @exclude_id. */
static gboolean
holdings_collect_memo(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  account_id,
	const gchar	 *currency,
	gint64		  exclude_id,
	GPtrArray	 *out,
	GError		**error
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);

	query = venture_query_new(VENTURE_TYPE_HOLDING_TXN);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, organization_id);

	if (!venture_query_add_filter_int(query, "account-id", VENTURE_FILTER_OP_EQ,
	                                  account_id, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;

	rows = venture_database_find(database, query, error);

	if (NULL == rows)
		return FALSE;

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row;
		HoldingsMovement *movement;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) when = NULL;
		VentureHoldingKind kind;

		row = g_ptr_array_index(rows, i);

		if (venture_entity_get_id(row) == exclude_id)
			continue;

		g_object_get(row, "amount", &amount, "occurred-at", &when, "kind", &kind, NULL);

		if ((NULL == amount) ||
		    ((NULL != currency) && (0 != g_strcmp0(amount->currency, currency))))
			continue;

		movement = g_new0(HoldingsMovement, 1);
		movement->account_id = account_id;
		movement->amount = g_steal_pointer(&amount);
		movement->when = (NULL != when) ? g_date_time_ref(when)
		                                : g_date_time_ref(venture_entity_get_created_at(row));

		switch (kind)
		{
		case VENTURE_HOLDING_KIND_TRANSFER:
			movement->flow = HOLDINGS_FLOW_TRANSFER;
			break;
		case VENTURE_HOLDING_KIND_EARN:
			movement->flow = HOLDINGS_FLOW_EARNED;
			break;
		case VENTURE_HOLDING_KIND_SPEND:
			movement->flow = HOLDINGS_FLOW_SPENT;
			break;
		case VENTURE_HOLDING_KIND_ADJUST:
		default:
			movement->flow = venture_money_is_negative(movement->amount)
				? HOLDINGS_FLOW_SPENT : HOLDINGS_FLOW_EARNED;
			break;
		}

		g_ptr_array_add(out, movement);
	}

	return TRUE;
}

VentureMoney *
venture_holdings_balance(
	VentureDatabase	 *database,
	gint64		  account_id,
	const gchar	 *currency,
	gint64		  exclude_txn_id,
	GError		**error
){
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(GPtrArray) movements = NULL;
	g_autoptr(VentureMoney) total = NULL;
	gint64 organization_id;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(NULL != currency, NULL);

	account = holdings_read(database, VENTURE_TYPE_ACCOUNT, account_id);

	if (NULL == account)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Account #%" G_GINT64_FORMAT " does not exist", account_id);
		return NULL;
	}

	organization_id = venture_entity_get_organization_id(account);
	movements = g_ptr_array_new_with_free_func(holdings_movement_free);

	if (!holdings_collect_ledger(database, organization_id, account_id, currency,
	                             movements, error) ||
	    !holdings_collect_memo(database, organization_id, account_id, currency,
	                           exclude_txn_id, movements, error))
		return NULL;

	total = venture_money_new_zero(currency);

	for (i = 0; i < movements->len; i++)
	{
		HoldingsMovement *movement;

		movement = g_ptr_array_index(movements, i);

		if (!holdings_money_add(&total, movement->amount, error))
			return NULL;
	}

	return g_steal_pointer(&total);
}

/* One event on a holding's timeline: which balances it counts in. */
typedef enum
{
	HOLDINGS_EVENT_BOTH = 0,
	HOLDINGS_EVENT_BEFORE,
	HOLDINGS_EVENT_AFTER
} HoldingsEventSide;

typedef struct
{
	GDateTime		*when;
	const VentureMoney	*amount;
	HoldingsEventSide	 side;
} HoldingsEvent;

static gint
holdings_event_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const HoldingsEvent *left;
	const HoldingsEvent *right;

	left = a;
	right = b;

	return g_date_time_compare(left->when, right->when);
}

/*
 * Refuses a change to the holding @account when it would take the holding
 * below zero at any moment from the change onward, unless the account
 * allows that.
 *
 * The change is described as what it removes and what it adds, in one
 * currency: @was (at @was_at) is a movement that stops counting -- the
 * stored value of a row being edited or deleted -- and @delta (at @at) one
 * that starts. Either may be NULL. @exclude_txn_id is the stored row
 * itself, left out of the rest of the timeline.
 *
 * Judged on the running balance, not today's: a spend dated in March is
 * refused when the holding was short in March even if it has plenty now,
 * because in March it did not. The rule is "never take a moment below
 * zero, and never deepen a moment already below it": a point is refused
 * only where the balance after the change is negative *and* lower than it
 * was. A shortfall a reversal left in the past -- which is never refused,
 * because it corrects evidence -- therefore does not block an unrelated
 * change that leaves it as it was. Movements at the same instant are
 * counted together, so a document dated the same second as the takings it
 * spends is judged against them.
 */
static gboolean
holdings_check_floor(
	VentureDatabase		 *database,
	VentureEntity		 *account,
	const VentureMoney	 *delta,
	GDateTime		 *at,
	const VentureMoney	 *was,
	GDateTime		 *was_at,
	gint64			  exclude_txn_id,
	GError			**error
){
	g_autoptr(GPtrArray) movements = NULL;
	g_autoptr(GArray) events = NULL;
	g_autoptr(VentureMoney) before = NULL;
	g_autoptr(VentureMoney) after = NULL;
	g_autoptr(GDateTime) now = NULL;
	const gchar *currency;
	gint64 organization_id;
	gboolean allow;
	guint i;

	allow = FALSE;
	g_object_get(account, "allow-negative", &allow, NULL);

	if (allow || (holdings_int(account, "location-id") <= 0))
		return TRUE;

	currency = (NULL != delta) ? delta->currency : ((NULL != was) ? was->currency : NULL);

	if (NULL == currency)
		return TRUE;

	/* Only a change that removes something positive or adds something
	 * negative can lower any moment. */
	if (((NULL == delta) || !venture_money_is_negative(delta)) &&
	    ((NULL == was) || venture_money_is_negative(was) || venture_money_is_zero(was)))
		return TRUE;

	organization_id = venture_entity_get_organization_id(account);
	movements = g_ptr_array_new_with_free_func(holdings_movement_free);

	if (!holdings_collect_ledger(database, organization_id, venture_entity_get_id(account),
	                             currency, movements, error) ||
	    !holdings_collect_memo(database, organization_id, venture_entity_get_id(account),
	                           currency, exclude_txn_id, movements, error))
		return FALSE;

	now = venture_time_now();
	events = g_array_sized_new(FALSE, FALSE, sizeof(HoldingsEvent), movements->len + 2);

	for (i = 0; i < movements->len; i++)
	{
		HoldingsMovement *movement;
		HoldingsEvent event;

		movement = g_ptr_array_index(movements, i);
		event.when = movement->when;
		event.amount = movement->amount;
		event.side = HOLDINGS_EVENT_BOTH;
		g_array_append_val(events, event);
	}

	if (NULL != was)
	{
		HoldingsEvent event;

		event.when = (NULL != was_at) ? was_at : now;
		event.amount = was;
		event.side = HOLDINGS_EVENT_BEFORE;
		g_array_append_val(events, event);
	}

	if (NULL != delta)
	{
		HoldingsEvent event;

		event.when = (NULL != at) ? at : now;
		event.amount = delta;
		event.side = HOLDINGS_EVENT_AFTER;
		g_array_append_val(events, event);
	}

	g_array_sort(events, holdings_event_compare);
	before = venture_money_new_zero(currency);
	after = venture_money_new_zero(currency);

	for (i = 0; i < events->len; i++)
	{
		HoldingsEvent *event;

		event = &g_array_index(events, HoldingsEvent, i);

		if ((HOLDINGS_EVENT_AFTER != event->side) &&
		    !holdings_money_add(&before, event->amount, error))
			return FALSE;

		if ((HOLDINGS_EVENT_BEFORE != event->side) &&
		    !holdings_money_add(&after, event->amount, error))
			return FALSE;

		/* A moment is the end of every movement at one instant. */
		if ((i + 1 < events->len) &&
		    (0 == g_date_time_compare(event->when,
		                              g_array_index(events, HoldingsEvent, i + 1).when)))
			continue;

		if (venture_money_is_negative(after) && (venture_money_compare(after, before) < 0))
		{
			g_autofree gchar *name = NULL;
			g_autofree gchar *has = NULL;
			g_autofree gchar *wants = NULL;
			g_autofree gchar *day = NULL;
			g_autoptr(VentureMoney) taken = NULL;

			name = holdings_account_name(database, account);
			taken = venture_money_subtract(before, after, NULL);
			has = venture_money_to_display_string(before, TRUE);
			wants = (NULL != taken) ? venture_money_to_display_string(taken, TRUE)
			                        : g_strdup("more");
			day = venture_time_to_date_string(event->when, NULL);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "%s holds %s on %s and this takes %s, which would leave it below "
			            "zero then. Record what came in first (dated no later), or tick "
			            "Allow negative on account #%" G_GINT64_FORMAT " if it may be "
			            "overdrawn", name, has, (NULL != day) ? day : "that day", wants,
			            venture_entity_get_id(account));
			return FALSE;
		}
	}

	return TRUE;
}

/* ==========================================================================
 * The movement record's rules
 * ========================================================================== */

/*
 * A memo movement, in the order a person would fix it:
 *
 *  - the source fields are the ledger's: a row claiming to come from a
 *    sale must be one the sale wrote. A derived row is frozen except for
 *    its notes -- change the document instead, and the ledger rewrites it;
 *  - a transfer is written in pairs by the Transfer action, never one side
 *    by hand;
 *  - the account carries a location (that is what makes it a holding) and
 *    lives in the row's organization;
 *  - the amount is not zero, and is in a memo currency when a person
 *    writes it: a posted currency's holding is its journal lines, and a
 *    movement beside them would be a second truth. Earn is positive,
 *    spend negative;
 *  - the date defaults to now, the factory's rule for a derived date;
 *  - what lowers the holding may not take it below zero unless the account
 *    allows it.
 */
static gboolean
venture_holdings_validate_txn(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) was_amount = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *source_type = NULL;
	g_autofree gchar *was_source = NULL;
	g_autofree gchar *rule = NULL;
	g_autofree gchar *was_rule = NULL;
	VentureHoldingKind kind;
	VentureHoldingKind was_kind;
	g_autoptr(GDateTime) when_set = NULL;
	gboolean permitted;
	gint64 account_id;
	gint64 source_id;

	(void)user_data;

	permitted = holdings_permitted(database, entity);
	g_object_get(entity, "account-id", &account_id, "amount", &amount, "kind", &kind,
	             "occurred-at", &when, "source-type", &source_type,
	             "source-id", &source_id, "rule-name", &rule, NULL);
	was_kind = VENTURE_HOLDING_KIND_ADJUST;

	if (NULL != previous)
		g_object_get(previous, "amount", &was_amount, "kind", &was_kind,
		             "source-type", &was_source, "rule-name", &was_rule, NULL);

	/* --- The ledger's fields --- */

	if (!permitted)
	{
		gboolean derived;

		derived = !venture_string_is_empty(was_source);

		if (derived)
		{
			g_autoptr(GDateTime) was_when = NULL;

			g_object_get(previous, "occurred-at", &was_when, NULL);

			if ((holdings_int(previous, "account-id") != account_id) ||
			    (NULL == amount) || (NULL == was_amount) ||
			    !venture_money_equal(amount, was_amount) || (was_kind != kind) ||
			    (0 != g_strcmp0(was_source, source_type)) ||
			    (holdings_int(previous, "source-id") != source_id) ||
			    (0 != g_strcmp0(was_rule, rule)) ||
			    ((NULL != was_when) != (NULL != when)) ||
			    ((NULL != when) && !g_date_time_equal(when, was_when)))
			{
				venture_set_error_validation(error, "Holding movement",
					"came from %s #%" G_GINT64_FORMAT "; change that record and "
					"the ledger rewrites this. Only the notes are yours to edit",
					was_source, holdings_int(previous, "source-id"));
				return FALSE;
			}

			return TRUE;
		}

		if (!venture_string_is_empty(source_type) || (0 != source_id) ||
		    !venture_string_is_empty(rule))
		{
			venture_set_error_validation(error, "Source",
				"is set by the ledger when a document moves a holding, not by hand");
			return FALSE;
		}

		if ((VENTURE_HOLDING_KIND_TRANSFER == kind) &&
		    ((NULL == previous) || (VENTURE_HOLDING_KIND_TRANSFER != was_kind)))
		{
			venture_set_error_validation(error, "Kind",
				"transfer has two sides; use the Transfer action on the location "
				"the money leaves, which writes both");
			return FALSE;
		}
	}

	/* --- The holding --- */

	if (account_id <= 0)
	{
		venture_set_error_validation(error, "Holding", "is required");
		return FALSE;
	}

	account = holdings_read(database, VENTURE_TYPE_ACCOUNT, account_id);

	/* Missing is the reference check's refusal, with its own words. */
	if (NULL != account)
	{
		if (venture_entity_get_organization_id(account) !=
		    venture_entity_get_organization_id(entity))
		{
			venture_set_error_validation(error, "Holding",
				"account #%" G_GINT64_FORMAT " belongs to another organization",
				account_id);
			return FALSE;
		}

		if (holdings_int(account, "location-id") <= 0)
		{
			venture_set_error_validation(error, "Holding",
				"account #%" G_GINT64_FORMAT " is held nowhere; a holding is an "
				"account with a location. Set its Held at first", account_id);
			return FALSE;
		}
	}

	/* --- The amount --- */

	if ((NULL == amount) || venture_money_is_zero(amount))
	{
		venture_set_error_validation(error, "Amount",
			"is required and cannot be zero");
		return FALSE;
	}

	if (!permitted &&
	    ((NULL == was_amount) || !venture_money_equal(amount, was_amount)) &&
	    (VENTURE_BOOK_TREATMENT_MEMO != venture_currency_get_book_treatment(amount->currency)))
	{
		venture_set_error_validation(error, "Amount",
			"is in %s, which the ledger posts; its holdings are the account's "
			"journal lines. Move it with a sale, an expense, a journal or the "
			"location's Transfer action. Holding movements are for memo currencies",
			amount->currency);
		return FALSE;
	}

	if ((VENTURE_HOLDING_KIND_EARN == kind) && venture_money_is_negative(amount))
	{
		venture_set_error_validation(error, "Amount",
			"is negative, and an earning adds to the holding; use spend");
		return FALSE;
	}

	if ((VENTURE_HOLDING_KIND_SPEND == kind) && !venture_money_is_negative(amount))
	{
		venture_set_error_validation(error, "Amount",
			"is positive, and spending takes from the holding: write it negative");
		return FALSE;
	}

	if (NULL == when)
	{
		g_autoptr(GDateTime) now = NULL;

		now = venture_time_now();
		g_object_set(entity, "occurred-at", now, NULL);
	}

	/* --- The floor --- */

	/* Only a change to what the movement is -- its holding, amount or
	 * date -- is judged: editing the notes of an old movement must not
	 * fail because of a later one. The stored value stops counting and
	 * the new one starts, each at its own date, so moving a spend earlier
	 * or an earning later is judged like a new one. A move to another
	 * holding or currency is two changes: the old holding loses the
	 * stored movement, the new one gains this. */
	if (NULL != account)
	{
		g_autoptr(GDateTime) was_when = NULL;
		gboolean same_line;
		gboolean changed;

		if (NULL != previous)
			g_object_get(previous, "occurred-at", &was_when, NULL);

		g_object_get(entity, "occurred-at", &when_set, NULL);
		same_line = (NULL != previous) && (NULL != was_amount) &&
		            (holdings_int(previous, "account-id") == account_id) &&
		            (0 == g_strcmp0(was_amount->currency, amount->currency));
		changed = !same_line || !venture_money_equal(amount, was_amount) ||
		          ((NULL != was_when) != (NULL != when_set)) ||
		          ((NULL != was_when) && !g_date_time_equal(was_when, when_set));

		if (changed &&
		    !holdings_check_floor(database, account, amount, when_set,
		                          same_line ? was_amount : NULL, was_when,
		                          venture_entity_get_id(entity), error))
			return FALSE;

		if (!same_line && (NULL != previous) && (NULL != was_amount) &&
		    venture_entity_is_persisted(previous))
		{
			g_autoptr(VentureEntity) was_account = NULL;

			was_account = holdings_read(database, VENTURE_TYPE_ACCOUNT,
			                            holdings_int(previous, "account-id"));

			if ((NULL != was_account) &&
			    !holdings_check_floor(database, was_account, NULL, NULL, was_amount,
			                          was_when, venture_entity_get_id(entity), error))
				return FALSE;
		}
	}

	return TRUE;
}

gboolean
venture_holdings_check_write(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	gboolean	  removal,
	GError		**error
){
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *source_type = NULL;

	if (!removal || !VENTURE_IS_HOLDING_TXN(entity) || holdings_permitted(database, entity))
		return TRUE;

	/* Judged on the stored row: a removal is of the record. */
	stored = holdings_read(database, VENTURE_TYPE_HOLDING_TXN, venture_entity_get_id(entity));

	if (NULL == stored)
		return TRUE;

	g_object_get(stored, "source-type", &source_type, "amount", &amount, NULL);

	if (!venture_string_is_empty(source_type))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "This movement came from %s #%" G_GINT64_FORMAT "; change or "
		            "delete that record instead, and the ledger removes it",
		            source_type, holdings_int(stored, "source-id"));
		return FALSE;
	}

	if (NULL == amount)
		return TRUE;

	account = holdings_read(database, VENTURE_TYPE_ACCOUNT, holdings_int(stored, "account-id"));

	if (NULL == account)
		return TRUE;

	g_object_get(stored, "occurred-at", &when, NULL);

	/* A restore puts the movement back at its own date, which takes the
	 * holding down when it was a spend; a deletion takes it out, which
	 * takes the holding down when it was an earning. Both are judged on
	 * the running balance from that date on. */
	if (venture_entity_is_deleted(stored))
		return holdings_check_floor(database, account, amount, when, NULL, NULL,
		                            venture_entity_get_id(stored), error);

	return holdings_check_floor(database, account, NULL, NULL, amount, when,
	                            venture_entity_get_id(stored), error);
}

/* ==========================================================================
 * The posting guard
 * ========================================================================== */

/*
 * The journal half of the floor, on the posting service's `posting`
 * signal: after validation, before anything is written. Each holding the
 * journal touches is judged on its net, per original currency, at the
 * journal's date, so a journal paying into and out of one holding is
 * judged once and a back-dated one against the balance it was dated in. A reversal is
 * a correction of evidence already posted and is never refused -- the
 * holding it leaves short is the truth.
 */
static GError *
holdings_posting_guard(
	VenturePostingService	*service,
	VentureJournal		*journal,
	GPtrArray		*lines,
	gpointer		 user_data
){
	VentureDatabase *database;
	g_autoptr(GHashTable) nets = NULL;
	g_autoptr(GHashTable) accounts = NULL;
	GHashTableIter iter;
	g_autoptr(GDateTime) when = NULL;
	gpointer key;
	gpointer value;
	GError *error;
	guint i;

	(void)service;

	database = user_data;
	error = NULL;

	if ((NULL == lines) || (holdings_int(VENTURE_ENTITY(journal), "reverses-id") > 0))
		return NULL;

	/* The journal's date is where its net lands on each holding's
	 * timeline; a back-dated spend is judged against the balance then. */
	g_object_get(journal, "occurred-at", &when, NULL);

	nets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                             (GDestroyNotify)venture_money_free);
	accounts = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_object_unref);

	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line;
		VentureEntity *account;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) signed_amount = NULL;
		VentureMoney *net;
		gchar *net_key;
		VentureLedgerSide side;
		gint64 account_id;
		gboolean allow;

		line = g_ptr_array_index(lines, i);
		g_object_get(line, "account-id", &account_id, "amount", &amount, "side", &side, NULL);

		if (NULL == amount)
			continue;

		account = g_hash_table_lookup(accounts, &account_id);

		if (NULL == account)
		{
			gint64 *id_key;

			account = holdings_read(database, VENTURE_TYPE_ACCOUNT, account_id);

			if (NULL == account)
				continue;

			id_key = g_new(gint64, 1);
			*id_key = account_id;
			g_hash_table_insert(accounts, id_key, account);
		}

		allow = FALSE;
		g_object_get(account, "allow-negative", &allow, NULL);

		if (allow || (holdings_int(account, "location-id") <= 0))
			continue;

		signed_amount = (VENTURE_LEDGER_SIDE_DEBIT == side)
			? venture_money_copy(amount) : venture_money_negate(amount);

		if (NULL == signed_amount)
			continue;

		net_key = g_strdup_printf("%" G_GINT64_FORMAT ":%s", account_id, amount->currency);
		net = g_hash_table_lookup(nets, net_key);

		if (NULL == net)
		{
			g_hash_table_insert(nets, net_key, g_steal_pointer(&signed_amount));
			continue;
		}

		{
			VentureMoney *sum;

			sum = venture_money_add(net, signed_amount, &error);

			if (NULL == sum)
			{
				g_free(net_key);
				return error;
			}

			/* Replacing frees the old total and this copy of the key. */
			g_hash_table_insert(nets, net_key, sum);
		}
	}

	g_hash_table_iter_init(&iter, nets);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		VentureEntity *account;
		gint64 account_id;

		account_id = g_ascii_strtoll(key, NULL, 10);
		account = g_hash_table_lookup(accounts, &account_id);

		if ((NULL != account) &&
		    !holdings_check_floor(database, account, value, when, NULL, NULL, 0, &error))
			return error;
	}

	return NULL;
}

/* ==========================================================================
 * Holding accounts
 * ========================================================================== */

gboolean
venture_holdings_account_for_location(
	VentureDatabase		 *database,
	gint64			  organization_id,
	gint64			  location_id,
	gboolean		  create,
	const VentureActor	 *actor,
	gint64			 *out_account_id,
	GError			**error
){
	g_autoptr(VentureEntity) location = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(VentureAccount) created = NULL;
	g_autofree gchar *code = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *name = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(NULL != out_account_id, FALSE);

	*out_account_id = 0;
	location = holdings_read(database, VENTURE_TYPE_LOCATION, location_id);

	if ((NULL == location) || venture_entity_is_deleted(location) ||
	    (venture_entity_get_organization_id(location) != organization_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Location #%" G_GINT64_FORMAT " does not exist in this organization",
		            location_id);
		return FALSE;
	}

	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, organization_id);

	if (!venture_query_add_filter_int(query, "location-id", VENTURE_FILTER_OP_EQ,
	                                  location_id, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;

	found = venture_database_find(database, query, error);

	if (NULL == found)
		return FALSE;

	path = holdings_location_name(database, location_id);

	if (found->len > 1)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s has %u holding accounts; name the account itself (a sale's or "
		            "expense's cash account) instead of the location", path, found->len);
		return FALSE;
	}

	if (1 == found->len)
	{
		*out_account_id = venture_entity_get_id(g_ptr_array_index(found, 0));
		return TRUE;
	}

	if (!create)
		return TRUE;

	/* The code is scoped like the clearing account's, so it never takes a
	 * number a chart of accounts already uses. A deleted one of the same
	 * code comes back rather than failing the unique index. */
	code = g_strdup_printf("%" G_GINT64_FORMAT ":holding:%" G_GINT64_FORMAT,
	                       organization_id, location_id);
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_include_deleted(query, TRUE);

	if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
		return FALSE;

	existing = venture_database_find_one(database, query, error);

	if ((NULL != error) && (NULL != *error))
		return FALSE;

	if (NULL != existing)
	{
		if (venture_entity_is_deleted(existing) &&
		    !venture_database_restore(database, existing, actor, error))
			return FALSE;

		g_object_set(existing, "location-id", location_id, NULL);

		if (!venture_database_save(database, existing, actor, error))
			return FALSE;

		*out_account_id = venture_entity_get_id(existing);
		return TRUE;
	}

	name = g_strdup_printf("Holding: %s", path);
	created = venture_account_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(created), organization_id);
	g_object_set(created, "code", code, "name", name, "kind", VENTURE_ACCOUNT_KIND_ASSET,
	             "active", TRUE, "location-id", location_id, NULL);

	if (!venture_database_save(database, VENTURE_ENTITY(created), actor, error))
		return FALSE;

	*out_account_id = venture_entity_get_id(VENTURE_ENTITY(created));

	return TRUE;
}

/* ==========================================================================
 * Memo movements from documents
 * ========================================================================== */

/* The movements this source and rule wrote before, oldest first. */
static GPtrArray *
holdings_source_movements(
	VentureDatabase	 *database,
	gint64		  organization_id,
	const gchar	 *source_type,
	gint64		  source_id,
	const gchar	 *rule_name,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_HOLDING_TXN);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, organization_id);

	if (!venture_query_add_filter_string(query, "source-type", VENTURE_FILTER_OP_EQ,
	                                     source_type, error) ||
	    !venture_query_add_filter_int(query, "source-id", VENTURE_FILTER_OP_EQ,
	                                  source_id, error) ||
	    !venture_query_add_filter_string(query, "rule-name", VENTURE_FILTER_OP_EQ,
	                                     rule_name, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	return venture_database_find(database, query, error);
}

gboolean
venture_holdings_clear_memo(
	VentureDatabase		 *database,
	gint64			  organization_id,
	const gchar		 *source_type,
	gint64			  source_id,
	const gchar		 *rule_name,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GPtrArray) existing = NULL;
	guint i;

	if (!holdings_ledger_enabled() || (organization_id <= 0))
		return TRUE;

	existing = holdings_source_movements(database, organization_id, source_type,
	                                     source_id, rule_name, error);

	if (NULL == existing)
		return FALSE;

	for (i = 0; i < existing->len; i++)
		if (!holdings_write_permitted(database, g_ptr_array_index(existing, i), TRUE,
		                              actor, error))
			return FALSE;

	return TRUE;
}

gboolean
venture_holdings_record_memo(
	VentureDatabase		 *database,
	VentureJournal		 *header,
	GPtrArray		 *lines,
	gboolean		  replace,
	const VentureActor	 *actor,
	GPtrArray		**out_movements,
	GError			**error
){
	VenturePostingService *posting;
	g_autoptr(GPtrArray) planned = NULL;
	g_autoptr(GPtrArray) written = NULL;
	g_autoptr(GPtrArray) existing = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(GHashTable) routes = NULL;
	g_autofree gchar *source_type = NULL;
	g_autofree gchar *rule = NULL;
	g_autofree gchar *memo = NULL;
	gint64 organization_id;
	gint64 source_id;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_JOURNAL(header), FALSE);

	if (NULL != out_movements)
		*out_movements = NULL;

	written = g_ptr_array_new_with_free_func(g_object_unref);

	if (!holdings_ledger_enabled() || (NULL == lines))
		goto done;

	organization_id = venture_entity_get_organization_id(VENTURE_ENTITY(header));
	g_object_get(header, "occurred-at", &when, "source-type", &source_type,
	             "source-id", &source_id, "rule-name", &rule, "memo", &memo, NULL);

	if (NULL == when)
		when = venture_time_now();

	posting = venture_database_get_posting_service(database);
	routes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	planned = g_ptr_array_new_with_free_func(g_object_unref);

	/* --- What the lines say the holdings should show --- */

	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line;
		g_autoptr(VentureEntity) account = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) signed_amount = NULL;
		VentureHoldingTxn *movement;
		VentureLedgerSide side;
		VentureHoldingKind kind;
		gpointer cached;
		gint64 account_id;
		gint route;

		line = g_ptr_array_index(lines, i);

		if (!VENTURE_IS_JOURNAL_LINE(line))
			continue;

		g_object_get(line, "account-id", &account_id, "amount", &amount, "side", &side, NULL);

		if ((NULL == amount) || venture_money_is_zero(amount))
			continue;

		if (g_hash_table_lookup_extended(routes, amount->currency, NULL, &cached))
			route = GPOINTER_TO_INT(cached);
		else
		{
			VentureBookRoute answer;

			if (!venture_posting_service_route_currency(posting, organization_id,
			                                            amount->currency, when,
			                                            &answer, NULL, error))
				return FALSE;

			route = (gint)answer;
			g_hash_table_insert(routes, g_strdup(amount->currency), GINT_TO_POINTER(route));
		}

		if (VENTURE_BOOK_ROUTE_MEMO != route)
			continue;

		account = holdings_read(database, VENTURE_TYPE_ACCOUNT, account_id);

		if ((NULL == account) || (holdings_int(account, "location-id") <= 0))
			continue;

		signed_amount = (VENTURE_LEDGER_SIDE_DEBIT == side)
			? venture_money_copy(amount) : venture_money_negate(amount);

		if (NULL == signed_amount)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "Amount magnitude overflows");
			return FALSE;
		}

		if (0 == g_strcmp0(rule, VENTURE_HOLDINGS_TRANSFER_RULE))
			kind = VENTURE_HOLDING_KIND_TRANSFER;
		else if (venture_money_is_negative(signed_amount))
			kind = VENTURE_HOLDING_KIND_SPEND;
		else
			kind = VENTURE_HOLDING_KIND_EARN;

		movement = venture_holding_txn_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(movement), organization_id);
		g_object_set(movement, "account-id", account_id, "amount", signed_amount,
		             "kind", kind, "occurred-at", when, "source-type", source_type,
		             "source-id", source_id, "rule-name", rule, "notes", memo, NULL);
		g_ptr_array_add(planned, movement);
	}

	/* --- Replacing what this document wrote before --- */

	if (replace && !venture_string_is_empty(source_type) && !venture_string_is_empty(rule))
	{
		gboolean same;

		existing = holdings_source_movements(database, organization_id, source_type,
		                                     source_id, rule, error);

		if (NULL == existing)
			return FALSE;

		/* Unchanged is left alone, so a note edited on a sale does not
		 * churn its holding's history. */
		same = (existing->len == planned->len);

		for (i = 0; same && (i < existing->len); i++)
		{
			VentureEntity *old;
			VentureEntity *new;
			g_autoptr(VentureMoney) old_amount = NULL;
			g_autoptr(VentureMoney) new_amount = NULL;
			g_autoptr(GDateTime) old_when = NULL;

			old = g_ptr_array_index(existing, i);
			new = g_ptr_array_index(planned, i);
			g_object_get(old, "amount", &old_amount, "occurred-at", &old_when, NULL);
			g_object_get(new, "amount", &new_amount, NULL);
			same = (holdings_int(old, "account-id") == holdings_int(new, "account-id")) &&
			       (NULL != old_amount) && venture_money_equal(old_amount, new_amount) &&
			       (NULL != old_when) && g_date_time_equal(old_when, when);
		}

		if (same)
		{
			for (i = 0; i < existing->len; i++)
				g_ptr_array_add(written, g_object_ref(g_ptr_array_index(existing, i)));

			goto done;
		}

		for (i = 0; i < existing->len; i++)
			if (!holdings_write_permitted(database, g_ptr_array_index(existing, i), TRUE,
			                              actor, error))
				return FALSE;
	}

	/* Additions first: a transfer's arriving side is saved before the
	 * leaving side, and neither depends on the other, but a document that
	 * pays into one holding and out of the same one nets in the order it
	 * was written. */
	for (i = 0; i < planned->len; i++)
	{
		VentureEntity *movement;

		movement = g_ptr_array_index(planned, i);

		if (!holdings_write_permitted(database, movement, FALSE, actor, error))
			return FALSE;

		g_ptr_array_add(written, g_object_ref(movement));
	}

done:
	if (NULL != out_movements)
		*out_movements = g_steal_pointer(&written);

	return TRUE;
}

/* ==========================================================================
 * Transfers
 * ========================================================================== */

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
){
	g_autoptr(VentureEntity) from = NULL;
	g_autoptr(VentureJournal) header = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GDateTime) moved = NULL;
	g_autofree gchar *to_path = NULL;
	g_autofree gchar *memo = NULL;
	VentureJournalLine *line;
	gint64 organization_id;
	gint64 from_account;
	gint64 to_account;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	if (!holdings_ledger_enabled())
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "Holdings belong to the general journal (ledger) module, "
		                    "which is off");
		return FALSE;
	}

	if ((NULL == amount) || venture_money_is_zero(amount) ||
	    venture_money_is_negative(amount))
	{
		venture_set_error_validation(error, "amount",
			"must be more than zero; it moves from this location to the other");
		return FALSE;
	}

	if (from_location_id == to_location_id)
	{
		venture_set_error_validation(error, "to_location_id",
			"is the location the money leaves; name another one");
		return FALSE;
	}

	from = holdings_read(database, VENTURE_TYPE_LOCATION, from_location_id);

	if ((NULL == from) || venture_entity_is_deleted(from))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Location #%" G_GINT64_FORMAT " does not exist", from_location_id);
		return FALSE;
	}

	organization_id = venture_entity_get_organization_id(from);

	/* One transaction: the accounts made on first use, the journal or the
	 * movements, and the refusal of any of them leaves none of it. */
	if (!venture_database_begin(database, error))
		return FALSE;

	if (!venture_holdings_account_for_location(database, organization_id, from_location_id,
	                                           TRUE, actor, &from_account, error))
		goto fail;

	if (!venture_holdings_account_for_location(database, organization_id, to_location_id,
	                                           TRUE, actor, &to_account, error))
	{
		/* The other location's own refusal names it; one in another
		 * organization reads as not found, which is what it is here. */
		goto fail;
	}

	moved = (NULL != when) ? g_date_time_ref(when) : venture_time_now();
	to_path = holdings_location_name(database, to_location_id);
	memo = venture_string_is_empty(notes)
		? g_strdup_printf("Transfer to %s", to_path)
		: g_strdup_printf("Transfer to %s: %s", to_path, notes);

	header = venture_journal_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(header), organization_id);
	g_object_set(header, "occurred-at", moved, "source-type", "location",
	             "source-id", from_location_id, "rule-name", VENTURE_HOLDINGS_TRANSFER_RULE,
	             "memo", memo, NULL);

	lines = g_ptr_array_new_with_free_func(g_object_unref);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", to_account, "amount", amount, "organization-id",
	             organization_id, "side", VENTURE_LEDGER_SIDE_DEBIT, "memo", memo, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", from_account, "amount", amount, "organization-id",
	             organization_id, "side", VENTURE_LEDGER_SIDE_CREDIT, "memo", memo, NULL);
	g_ptr_array_add(lines, line);

	journals = venture_posting_service_post_by_currency_full(
		venture_database_get_posting_service(database), header, lines, FALSE, actor,
		NULL, error);

	if (NULL == journals)
		goto fail;

	return venture_database_commit(database, error);

fail:
	venture_database_rollback(database);
	return FALSE;
}

/* ==========================================================================
 * The transfer action
 * ========================================================================== */

static JsonNode *
holdings_param(
	GHashTable	*params,
	const gchar	*name
){
	JsonNode *node;

	node = (NULL != params) ? g_hash_table_lookup(params, name) : NULL;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node) || !JSON_NODE_HOLDS_VALUE(node))
		return NULL;

	return node;
}

static const gchar *
holdings_param_string(
	GHashTable	*params,
	const gchar	*name
){
	JsonNode *node;

	node = holdings_param(params, name);

	if ((NULL == node) || (G_TYPE_STRING != json_node_get_value_type(node)))
		return NULL;

	return json_node_get_string(node);
}

/* An integer, or a string of digits as a form sends one. */
static gint64
holdings_param_int(
	GHashTable	*params,
	const gchar	*name
){
	JsonNode *node;
	const gchar *text;

	node = holdings_param(params, name);

	if (NULL == node)
		return 0;

	if (G_TYPE_INT64 == json_node_get_value_type(node))
		return json_node_get_int(node);

	if (G_TYPE_STRING != json_node_get_value_type(node))
		return 0;

	text = json_node_get_string(node);

	return (NULL != text) ? g_ascii_strtoll(text, NULL, 10) : 0;
}

/* Offered while the ledger is on; holdings are its accounts. Must not
 * write. */
static gboolean
holdings_transfer_allowed(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	(void)action;
	(void)entity;
	(void)actor;

	if (!holdings_ledger_enabled())
	{
		venture_set_error_validation(error, "transfer",
			"Transfers move holdings, which belong to the general journal "
			"(ledger) module, and it is off");
		return FALSE;
	}

	return TRUE;
}

static VentureEntity *
holdings_transfer_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	VentureDatabase *database;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *book = NULL;
	const gchar *amount_text;
	const gchar *when_text;
	gint64 to_location_id;

	database = venture_action_get_data(action);
	to_location_id = holdings_param_int(params, "to_location_id");
	amount_text = holdings_param_string(params, "amount");
	when_text = holdings_param_string(params, "occurred_at");

	if (to_location_id <= 0)
	{
		venture_set_error_validation(error, "to_location_id",
			"is required: the location the money goes to");
		return NULL;
	}

	/* A bare number is in the book currency, the same default a sale's
	 * missing amount has. */
	book = venture_posting_service_book_currency(venture_database_get_posting_service(database),
		venture_entity_get_organization_id(entity), NULL);
	amount = venture_money_from_string(amount_text, book, error);

	if (NULL == amount)
		return NULL;

	if (!venture_string_is_empty(when_text))
	{
		when = venture_time_from_string(when_text, error);

		if (NULL == when)
			return NULL;
	}

	if (!venture_holdings_transfer(database, venture_entity_get_id(entity), to_location_id,
	                               amount, when, holdings_param_string(params, "notes"),
	                               actor, error))
		return NULL;

	/* The location as it stands; the movement is in the holdings report
	 * and on the journal or the movement list. */
	return venture_database_get(database, VENTURE_TYPE_LOCATION,
	                            venture_entity_get_id(entity), error);
}

static void
holdings_register_transfer(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = NULL;
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	VentureFieldSpec *field;

	parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);

	field = venture_field_spec_new("to_location_id", "To", VENTURE_FIELD_KIND_REFERENCE);
	field->help = g_strdup("The location or character the money goes to");
	field->reference_type = g_strdup("location");
	field->required = TRUE;
	g_ptr_array_add(parameters, field);

	field = venture_field_spec_new("amount", "Amount", VENTURE_FIELD_KIND_MONEY);
	field->help = g_strdup("How much, with its currency: \"5 TICKET\"; a bare number "
	                       "is in the book currency");
	field->required = TRUE;
	g_ptr_array_add(parameters, field);

	field = venture_field_spec_new("occurred_at", "Moved at", VENTURE_FIELD_KIND_DATETIME);
	field->help = g_strdup("When it moved; now when left empty");
	g_ptr_array_add(parameters, field);

	field = venture_field_spec_new("notes", "Notes", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("Why, kept on the journal or the movements");
	g_ptr_array_add(parameters, field);

	/*
	 * A record action on the location the money leaves: the access policy
	 * judges that location's organization, so no organization_id is
	 * needed, and the destination is checked to be in the same one.
	 * Stageable: a staged action holds only its parameters, and approval
	 * performs the whole move afresh, judging the holding as it is then.
	 */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "location", "name", "transfer", "label", "Transfer money",
		"description", "Move an amount of any currency from this location's holding "
		"to another's, in one transaction; it may not leave this one below zero",
		"parameters", parameters, "stageable", TRUE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);

	if (!venture_action_registry_register(venture_database_get_action_registry(database),
	                                      action, holdings_transfer_allowed,
	                                      holdings_transfer_invoke, database, NULL, &error))
		g_error("Transfer action registration: %s", error->message);
}

void
venture_holdings_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators,
	 * handlers and actions are per database, so the second adds nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_HOLDINGS_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_HOLDINGS_STATE_KEY, GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_HOLDING_TXN,
	                                    venture_holdings_validate_txn, NULL, NULL);
	g_signal_connect(venture_database_get_posting_service(database), "posting",
	                 G_CALLBACK(holdings_posting_guard), database);
	holdings_register_transfer(database);
}

/* ==========================================================================
 * The holdings report
 * ========================================================================== */

typedef struct
{
	gchar		*location;
	gint64		 location_id;
	gchar		*currency;
	VentureMoney	*earned;
	VentureMoney	*spent;
	VentureMoney	*transfers;
	VentureMoney	*balance;
} HoldingsRow;

static void
holdings_row_free(gpointer data)
{
	HoldingsRow *row;

	row = data;
	g_free(row->location);
	g_free(row->currency);
	venture_money_free(row->earned);
	venture_money_free(row->spent);
	venture_money_free(row->transfers);
	venture_money_free(row->balance);
	g_free(row);
}

/* Sorts rows by location path, then the book currency first. */
static gint
holdings_row_compare(
	gconstpointer	 a,
	gconstpointer	 b,
	gpointer	 book
){
	const HoldingsRow *left;
	const HoldingsRow *right;
	gboolean left_book;
	gboolean right_book;
	gint order;

	left = *(HoldingsRow *const *)a;
	right = *(HoldingsRow *const *)b;
	order = g_utf8_collate(left->location, right->location);

	if (0 != order)
		return order;

	left_book = (0 == g_strcmp0(left->currency, book));
	right_book = (0 == g_strcmp0(right->currency, book));

	if (left_book != right_book)
		return left_book ? -1 : 1;

	return g_strcmp0(left->currency, right->currency);
}

/* How a currency reaches the books, in words; the posting service decides
 * every case but memo, which it never labels because it never posts. */
static gchar *
holdings_books_label(
	VenturePostingService	 *posting,
	gint64			  organization_id,
	const gchar		 *currency,
	GDateTime		 *when,
	GError			**error
){
	VentureBookRoute route;

	if (!venture_posting_service_route_currency(posting, organization_id, currency, when,
	                                            &route, NULL, error))
		return NULL;

	if (VENTURE_BOOK_ROUTE_MEMO == route)
		return g_strdup_printf("Memo: %s (counted here, never posted)", currency);

	return venture_posting_service_book_label(posting, organization_id, currency, when, error);
}

VentureReportResult *
venture_holdings_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	VentureDatabase *database;
	VenturePostingService *posting;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GPtrArray) ordered = NULL;
	g_autoptr(GPtrArray) totals = NULL;
	g_autoptr(GHashTable) rows = NULL;
	g_autoptr(GHashTable) paths = NULL;
	g_autoptr(GHashTable) within = NULL;
	g_autoptr(GHashTable) labels = NULL;
	g_autoptr(GDateTime) cutoff = NULL;
	g_autofree gchar *book = NULL;
	GDateTime *start;
	const gchar *currency;
	gint64 organization_id;
	gint64 location_id;
	GHashTableIter iter;
	gpointer value;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	database = venture_context_get_database(context);
	posting = venture_context_get_posting_service(context);

	if ((NULL == posting) || !holdings_ledger_enabled())
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		                    "The general journal (ledger) module is off, and holdings are its accounts");
		return NULL;
	}

	/* --- The question --- */

	organization_id = venture_context_get_default_organization_id(context);

	if (NULL != options)
		organization_id = venture_json_object_get_int(options, "organization_id", organization_id);

	organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, organization_id, NULL);

	if ((NULL == organization) || venture_entity_is_deleted(organization))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Organization #%" G_GINT64_FORMAT " not found", organization_id);
		return NULL;
	}

	currency = (NULL != options) ? venture_json_object_get_string(options, "currency", NULL) : NULL;

	if (!venture_string_is_empty(currency) && !venture_currency_is_valid(currency))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a currency code", currency);
		return NULL;
	}

	if (venture_string_is_empty(currency))
		currency = NULL;

	location_id = (NULL != options) ? venture_json_object_get_int(options, "location_id", 0) : 0;

	if (location_id > 0)
	{
		g_autoptr(VentureEntity) location = NULL;
		g_autoptr(GArray) subtree = NULL;

		location = venture_database_get(database, VENTURE_TYPE_LOCATION, location_id, NULL);

		if ((NULL == location) ||
		    (venture_entity_get_organization_id(location) != organization_id))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Location #%" G_GINT64_FORMAT " not found in organization #%"
			            G_GINT64_FORMAT, location_id, organization_id);
			return NULL;
		}

		subtree = venture_category_descendants(database, VENTURE_TYPE_LOCATION,
		                                       location_id, TRUE, error);

		if (NULL == subtree)
			return NULL;

		within = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

		for (i = 0; i < subtree->len; i++)
		{
			gint64 *key;

			key = g_new(gint64, 1);
			*key = g_array_index(subtree, gint64, i);
			g_hash_table_add(within, key);
		}
	}

	/* The balance is as of the period's end -- "all" is now -- or as_of
	 * when that is earlier, the trial balance's rule. */
	start = (NULL != period) ? venture_date_range_get_start(period) : NULL;
	cutoff = ((NULL != period) && (NULL != venture_date_range_get_end(period)))
		? g_date_time_add(venture_date_range_get_end(period), -1) : venture_time_now();

	if ((NULL != options) && json_object_has_member(options, "as_of"))
	{
		g_autoptr(GDateTime) requested = NULL;

		requested = venture_period_report_as_of(options, error);

		if (NULL == requested)
			return NULL;

		if (g_date_time_compare(requested, cutoff) < 0)
		{
			g_clear_pointer(&cutoff, g_date_time_unref);
			cutoff = g_steal_pointer(&requested);
		}
	}

	book = venture_posting_service_book_currency(posting, organization_id, error);

	if (NULL == book)
		return NULL;

	/* --- The holdings --- */

	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, organization_id);

	if (!venture_query_add_filter_int(query, "location-id", VENTURE_FILTER_OP_GT, 0, error))
		return NULL;

	accounts = venture_database_find(database, query, error);

	if (NULL == accounts)
		return NULL;

	rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, holdings_row_free);
	paths = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);

	for (i = 0; i < accounts->len; i++)
	{
		VentureEntity *account;
		g_autoptr(GPtrArray) movements = NULL;
		const gchar *path;
		gint64 held_at;
		guint j;

		account = g_ptr_array_index(accounts, i);
		held_at = holdings_int(account, "location-id");

		if ((NULL != within) && !g_hash_table_contains(within, &held_at))
			continue;

		path = g_hash_table_lookup(paths, &held_at);

		if (NULL == path)
		{
			gint64 *key;

			key = g_new(gint64, 1);
			*key = held_at;
			path = holdings_location_name(database, held_at);
			g_hash_table_insert(paths, key, (gpointer)path);
		}

		movements = g_ptr_array_new_with_free_func(holdings_movement_free);

		if (!holdings_collect_ledger(database, organization_id, venture_entity_get_id(account),
		                             currency, movements, error) ||
		    !holdings_collect_memo(database, organization_id, venture_entity_get_id(account),
		                           currency, 0, movements, error))
			return NULL;

		for (j = 0; j < movements->len; j++)
		{
			HoldingsMovement *movement;
			HoldingsRow *row;
			g_autofree gchar *key = NULL;

			movement = g_ptr_array_index(movements, j);

			if (g_date_time_compare(movement->when, cutoff) > 0)
				continue;

			/* Keyed by location and currency before anything is added:
			 * nothing below the key adds across currencies. */
			key = g_strdup_printf("%" G_GINT64_FORMAT ":%s", held_at,
			                      movement->amount->currency);
			row = g_hash_table_lookup(rows, key);

			if (NULL == row)
			{
				row = g_new0(HoldingsRow, 1);
				row->location = g_strdup(path);
				row->location_id = held_at;
				row->currency = g_strdup(movement->amount->currency);
				row->earned = venture_money_new_zero(row->currency);
				row->spent = venture_money_new_zero(row->currency);
				row->transfers = venture_money_new_zero(row->currency);
				row->balance = venture_money_new_zero(row->currency);
				g_hash_table_insert(rows, g_steal_pointer(&key), row);
			}

			if (!holdings_money_add(&row->balance, movement->amount, error))
				return NULL;

			/* Before the period: only the balance it carried in. */
			if ((NULL != start) && (g_date_time_compare(movement->when, start) < 0))
				continue;

			switch (movement->flow)
			{
			case HOLDINGS_FLOW_TRANSFER:
				if (!holdings_money_add(&row->transfers, movement->amount, error))
					return NULL;
				break;
			case HOLDINGS_FLOW_SPENT:
			{
				g_autoptr(VentureMoney) out = NULL;

				out = venture_money_negate(movement->amount);

				if ((NULL == out) || !holdings_money_add(&row->spent, out, error))
					return NULL;
				break;
			}
			case HOLDINGS_FLOW_EARNED:
			default:
				if (!holdings_money_add(&row->earned, movement->amount, error))
					return NULL;
				break;
			}
		}
	}

	/* --- The rows --- */

	result = venture_report_result_new("Holdings", period);
	venture_report_result_add_column(result, "location", "Held at", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "books", "In the books", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "earned", "Earned", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "spent", "Spent", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "transfers", "Transferred", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "net", "Net", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "balance", "Balance", VENTURE_REPORT_COLUMN_MONEY);

	ordered = g_ptr_array_new();
	g_hash_table_iter_init(&iter, rows);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(ordered, value);

	g_ptr_array_sort_with_data(ordered, holdings_row_compare, book);
	labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	totals = venture_money_totals_new();

	for (i = 0; i < ordered->len; i++)
	{
		HoldingsRow *row;
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(VentureMoney) gained = NULL;
		const gchar *label;

		row = g_ptr_array_index(ordered, i);
		label = g_hash_table_lookup(labels, row->currency);

		if (NULL == label)
		{
			gchar *made;

			made = holdings_books_label(posting, organization_id, row->currency, cutoff, error);

			if (NULL == made)
				return NULL;

			g_hash_table_insert(labels, g_strdup(row->currency), made);
			label = made;
		}

		gained = venture_money_subtract(row->earned, row->spent, error);

		if (NULL == gained)
			return NULL;

		net = venture_money_add(gained, row->transfers, error);

		if ((NULL == net) || !venture_money_totals_add(totals, row->balance, error))
			return NULL;

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "location", row->location);
		venture_report_result_set_text(result, "currency", row->currency);
		venture_report_result_set_text(result, "books", label);
		venture_report_result_set_money(result, "earned", row->earned);
		venture_report_result_set_money(result, "spent", row->spent);
		venture_report_result_set_money(result, "transfers", row->transfers);
		venture_report_result_set_money(result, "net", net);
		venture_report_result_set_money(result, "balance", row->balance);
	}

	/* Totals per currency: the book currency under the plain key, every
	 * other under `_<CODE>`, the pnl rule, so a metric widget reading
	 * `held` never shows tickets as gold. */
	{
		const VentureMoney *held_book;

		held_book = venture_money_totals_lookup(totals, book);

		if (NULL == held_book)
		{
			g_autoptr(VentureMoney) zero = NULL;

			zero = venture_money_new_zero(book);
			venture_report_result_add_metric(result,
				venture_metric_new_money("held", "Held", zero));
		}
		else
			venture_report_result_add_metric(result,
				venture_metric_new_money("held", "Held", held_book));

		venture_money_totals_sort(totals, book);

		for (i = 0; i < totals->len; i++)
		{
			VentureMoney *total;
			g_autofree gchar *key = NULL;
			g_autofree gchar *label = NULL;

			total = g_ptr_array_index(totals, i);

			if (0 == g_strcmp0(total->currency, book))
				continue;

			key = g_strdup_printf("held_%s", total->currency);
			label = g_strdup_printf("Held (%s)", total->currency);
			venture_report_result_add_metric(result,
				venture_metric_new_money(key, label, total));
		}
	}

	venture_report_result_append_note(result,
		"A holding is an account with a location. A currency the ledger posts "
		"is read from the account's journal lines, a memo currency from its "
		"holding movements; nothing is converted, so each currency is a row of "
		"its own.");
	venture_report_result_append_note(result,
		"Earned, spent and transferred are counted within the period; the "
		"balance is everything up to its end (or as_of). A reversal takes back "
		"what it reverses rather than counting on the other side.");

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Registration
 *
 * A report class rather than a function report, so the report carries its
 * parameter schema: that is what the assistant's report tool and the
 * options form read, and an option they cannot see is one never sent.
 * ========================================================================== */

#define VENTURE_TYPE_HOLDINGS_REPORT (venture_holdings_report_get_type())

G_DECLARE_FINAL_TYPE(VentureHoldingsReport, venture_holdings_report,
                     VENTURE, HOLDINGS_REPORT, VentureReport)

struct _VentureHoldingsReport
{
	VentureReport	parent_instance;
};

G_DEFINE_FINAL_TYPE(VentureHoldingsReport, venture_holdings_report, VENTURE_TYPE_REPORT)

static VentureReportResult *
venture_holdings_report_generate(
	VentureReport		 *self,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	(void)self;

	return venture_holdings_report(context, period, options, error);
}

static JsonNode *
venture_holdings_report_parameters(VentureReport *self)
{
	(void)self;

	return venture_json_parse(
		"{\"type\":\"object\",\"properties\":{"
		"\"location_id\":{\"type\":\"integer\",\"description\":\"Only this "
		"location and every location beneath it\"},"
		"\"currency\":{\"type\":\"string\",\"description\":\"Only this "
		"currency\"},"
		"\"as_of\":{\"type\":\"string\",\"description\":\"Balances as of this "
		"date, when earlier than the period's end\"},"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal "
		"entity; defaults to the default organization\"}}}", NULL);
}

static void
venture_holdings_report_class_init(VentureHoldingsReportClass *klass)
{
	VentureReportClass *report_class;

	report_class = VENTURE_REPORT_CLASS(klass);
	report_class->generate = venture_holdings_report_generate;
	report_class->describe_parameters = venture_holdings_report_parameters;
}

static void
venture_holdings_report_init(VentureHoldingsReport *self)
{
	(void)self;
}

void
venture_holdings_register_report(VentureReportRegistry *registry)
{
	VentureHoldingsReport *report;

	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	report = g_object_new(VENTURE_TYPE_HOLDINGS_REPORT, "name", "holdings",
	                      "title", "Holdings",
	                      "description", "What each location -- a wallet, a till, a "
	                      "character -- holds in every currency: earned, spent, "
	                      "transferred and the balance, whatever the currency's "
	                      "book treatment",
	                      NULL);
	venture_data_class_declare_resource(G_OBJECT(report), VENTURE_DATA_CLASS_TENANT);
	venture_report_registry_add(registry, VENTURE_REPORT(report));
}
