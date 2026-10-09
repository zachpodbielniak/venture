/*
 * venture-arbitrage.c - Arbitrage trades and legs: rules, posting, actions
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * How an attempt reaches the books. Every movement of a trade passes
 * through one asset account, arbitrage positions, so that what is left on
 * it when the trade is over is exactly its profit or loss:
 *
 *   leg                         stock side (execute)        the leg's rule
 *   buy/stake/transfer out      --                          Dr positions / Cr venue cash
 *   sell/payout/refund/in       --                          Dr venue cash / Cr positions
 *   buy with stock              Dr inventory / Cr cash      fees only
 *   sell with stock             Dr positions / Cr inventory Dr venue cash / Cr positions
 *   write_off (abandon)         Dr positions / Cr inventory nothing
 *   fee, and every leg's fees   --                          Dr arbitrage fees / Cr venue cash
 *
 * The rule is pure -- the ledger builds it for the stored version and the
 * new one on every save -- and never calls the inventory service: the
 * stock side is done once, by the execute action, and stamped on the leg,
 * which is what keeps a unit from being posted twice. `close` moves the
 * positions balance to arbitrage gains, one journal per book section.
 */

#include "venture.h"
#include "arbitrage/venture-arbitrage-private.h"

#include <string.h>

#define VENTURE_ARBITRAGE_STATE_KEY "venture-arbitrage-installed"

/*
 * The record an action of this module is writing right now. A leg's stock
 * stamps (`inventory-txn-id`, `cost`), a write-off leg, executing a stock
 * leg, and a trade's closed status and closing journal belong to the
 * actions and nobody else: a hand-written stamp would make a leg look
 * received with nothing on the shelf, and a hand-set "closed" would leave
 * the position on the books. The validators let those through only for
 * the record named here.
 *
 * Named by type and id rather than by pointer: a leg is a registered
 * ledger source, and the ledger saves a copy of the object it is handed,
 * so the validator never sees the caller's pointer. A new record (id 0)
 * is matched by type alone, for the one save the action is making.
 */
#define VENTURE_ARBITRAGE_PERMIT_KEY "venture-arbitrage-permit"

typedef struct
{
	GType	type;
	gint64	id;
} ArbPermit;

/* ==========================================================================
 * Shared
 * ========================================================================== */

static gint64
arb_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

static gint
arb_enum(
	VentureEntity	*entity,
	const gchar	*property
){
	gint value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

static VentureMoney *
arb_money(
	VentureEntity	*entity,
	const gchar	*property
){
	VentureMoney *value;

	value = NULL;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

static GDateTime *
arb_time(
	VentureEntity	*entity,
	const gchar	*property
){
	GDateTime *value;

	value = NULL;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

/* Whether two optional money values differ, NULL being a value. */
static gboolean
arb_money_differs(
	const VentureMoney	*left,
	const VentureMoney	*right
){
	if ((NULL == left) || (NULL == right))
		return left != right;

	return (0 != g_strcmp0(venture_money_get_currency(left), venture_money_get_currency(right))) ||
	       !venture_money_equal(left, right);
}

static gboolean
arb_time_differs(
	GDateTime	*left,
	GDateTime	*right
){
	if ((NULL == left) || (NULL == right))
		return left != right;

	return !g_date_time_equal(left, right);
}

/* "Peacebloom flip", or "trade #7" when it cannot be read. */
static gchar *
arb_name_of(
	VentureDatabase	*database,
	GType		 type,
	const gchar	*noun,
	gint64		 id
){
	g_autoptr(VentureEntity) record = NULL;

	if (id > 0)
		record = venture_database_get(database, type, id, NULL);

	if (NULL == record)
		return g_strdup_printf("%s #%" G_GINT64_FORMAT, noun, id);

	return venture_entity_get_display_name(record);
}

static gboolean
arb_permitted(
	VentureDatabase	*database,
	VentureEntity	*entity
){
	ArbPermit *permit;

	permit = g_object_get_data(G_OBJECT(database), VENTURE_ARBITRAGE_PERMIT_KEY);

	return (NULL != permit) && (permit->type == G_OBJECT_TYPE(entity)) &&
	       (permit->id == venture_entity_get_id(entity));
}

/* Saves @entity as an action of this module, the one writer allowed its
 * stamps. A leg still posts on this save through the ledger's registry. */
static gboolean
arb_save_permitted(
	VentureDatabase		 *database,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	ArbPermit permit;
	gpointer previous;
	gboolean saved;

	permit.type = G_OBJECT_TYPE(entity);
	permit.id = venture_entity_get_id(entity);

	previous = g_object_get_data(G_OBJECT(database), VENTURE_ARBITRAGE_PERMIT_KEY);
	g_object_set_data(G_OBJECT(database), VENTURE_ARBITRAGE_PERMIT_KEY, &permit);
	saved = venture_database_save(database, entity, actor, error);
	g_object_set_data(G_OBJECT(database), VENTURE_ARBITRAGE_PERMIT_KEY, previous);

	return saved;
}

/* Whether the sales module (products, stock) is on. A stock leg needs it. */
static gboolean
arb_sales_enabled(void)
{
	return G_TYPE_INVALID != venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "inventory_item");
}

/* @sum += @amount, @sum starting at NULL. */
static gboolean
arb_money_add(
	VentureMoney		**sum,
	const VentureMoney	 *amount,
	GError			**error
){
	VentureMoney *total;

	if (NULL == amount)
		return TRUE;

	if (NULL == *sum)
	{
		*sum = venture_money_copy(amount);
		return TRUE;
	}

	total = venture_money_add(*sum, amount, error);

	if (NULL == total)
		return FALSE;

	venture_money_free(*sum);
	*sum = total;

	return TRUE;
}

/* The organization a type-level action names, or the default one -- the
 * rule every type-level action keeps for a global administrator who names
 * none. */
static gint64
arb_default_organization(VentureDatabase *database)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) organization = NULL;

	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	venture_query_add_filter_string(query, "is-default", VENTURE_FILTER_OP_EQ, "true", NULL);
	organization = venture_database_find_one(database, query, NULL);

	if (NULL == organization)
	{
		g_clear_object(&query);
		query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		organization = venture_database_find_one(database, query, NULL);
	}

	return (NULL != organization) ? venture_entity_get_id(organization) : 0;
}

gint
venture_arbitrage_leg_direction(
	VentureArbitrageLegKind	 kind,
	const VentureMoney	*amount
){
	switch (kind)
	{
	case VENTURE_ARBITRAGE_LEG_KIND_BUY:
	case VENTURE_ARBITRAGE_LEG_KIND_STAKE:
		return 1;
	case VENTURE_ARBITRAGE_LEG_KIND_SELL:
	case VENTURE_ARBITRAGE_LEG_KIND_PAYOUT:
	case VENTURE_ARBITRAGE_LEG_KIND_REFUND:
		return -1;
	case VENTURE_ARBITRAGE_LEG_KIND_TRANSFER:
		/* One kind, two directions, told apart by the sign: a transfer
		 * between two venues is one leg leaving the first (positive) and
		 * one arriving at the second (negative), and the two net to
		 * nothing on the positions account. */
		return ((NULL != amount) && venture_money_is_negative(amount)) ? -1 : 1;
	case VENTURE_ARBITRAGE_LEG_KIND_FEE:
	case VENTURE_ARBITRAGE_LEG_KIND_WRITE_OFF:
	default:
		return 0;
	}
}

/* ==========================================================================
 * Accounts
 * ========================================================================== */

/* An account in @organization_id found by its exact @code: its id, or 0
 * when there is none (with no error). Deleted rows count, as they do for
 * the unique index a second account under the code would fail. */
static gint64
arb_find_scoped_account(
	VentureDatabase	 *database,
	gint64		  organization_id,
	const gchar	 *code,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(GError) local_error = NULL;

	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_organization(query, organization_id);
	venture_query_set_include_deleted(query, TRUE);

	if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
		return -1;

	found = venture_database_find_one(database, query, &local_error);

	if (NULL != local_error)
	{
		g_propagate_error(error, g_steal_pointer(&local_error));
		return -1;
	}

	return (NULL != found) ? venture_entity_get_id(found) : 0;
}

/* An account in @organization_id found by its exact @code, else made. */
static gint64
arb_scoped_account(
	VentureDatabase		 *database,
	gint64			  organization_id,
	const gchar		 *code,
	const gchar		 *name,
	VentureAccountKind	  kind,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureAccount) created = NULL;
	gint64 found;

	found = arb_find_scoped_account(database, organization_id, code, error);

	if (found < 0)
		return 0;

	if (found > 0)
		return found;

	created = venture_account_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(created), organization_id);
	g_object_set(created, "code", code, "name", name, "kind", kind, "active", TRUE, NULL);

	if (!venture_database_save(database, VENTURE_ENTITY(created), actor, error))
		return 0;

	return venture_entity_get_id(VENTURE_ENTITY(created));
}

/* What a classification means: the scoped number, the name and the kind
 * an account made for it takes. FALSE with @error for any other word. */
static gboolean
arb_classification(
	const gchar		 *classification,
	const gchar		**out_number,
	const gchar		**out_name,
	VentureAccountKind	 *out_kind,
	GError			**error
){
	if (0 == g_strcmp0(classification, "arbitrage_positions"))
	{
		*out_number = "1460";
		*out_name = "Arbitrage positions";
		*out_kind = VENTURE_ACCOUNT_KIND_ASSET;
	}
	else if (0 == g_strcmp0(classification, "arbitrage_gains"))
	{
		*out_number = "4960";
		*out_name = "Arbitrage gains";
		*out_kind = VENTURE_ACCOUNT_KIND_INCOME;
	}
	else if (0 == g_strcmp0(classification, "arbitrage_fees"))
	{
		*out_number = "6960";
		*out_name = "Arbitrage fees";
		*out_kind = VENTURE_ACCOUNT_KIND_EXPENSE;
	}
	/* The books an external ledger posts to (venture-arbitrage-books.c):
	 * a day's sales, purchases, other income and expenses, and the
	 * capital that stands for money the ledger never saw arrive. */
	else if (0 == g_strcmp0(classification, "trading_sales"))
	{
		*out_number = "4970";
		*out_name = "Trading sales";
		*out_kind = VENTURE_ACCOUNT_KIND_INCOME;
	}
	else if (0 == g_strcmp0(classification, "trading_purchases"))
	{
		*out_number = "5970";
		*out_name = "Trading purchases";
		*out_kind = VENTURE_ACCOUNT_KIND_EXPENSE;
	}
	else if (0 == g_strcmp0(classification, "trading_income"))
	{
		*out_number = "4980";
		*out_name = "Trading other income";
		*out_kind = VENTURE_ACCOUNT_KIND_INCOME;
	}
	else if (0 == g_strcmp0(classification, "trading_expenses"))
	{
		*out_number = "6970";
		*out_name = "Trading expenses";
		*out_kind = VENTURE_ACCOUNT_KIND_EXPENSE;
	}
	else if (0 == g_strcmp0(classification, "trading_capital"))
	{
		*out_number = "3970";
		*out_name = "Trading capital";
		*out_kind = VENTURE_ACCOUNT_KIND_EQUITY;
	}
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "No arbitrage account is classified \"%s\"",
		            (NULL != classification) ? classification : "");
		return FALSE;
	}

	return TRUE;
}

gboolean
venture_arbitrage_find_account(
	VentureDatabase	 *database,
	gint64		  organization_id,
	const gchar	 *classification,
	GDateTime	 *when,
	gint64		 *out_account_id,
	GError		**error
){
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *code = NULL;
	const gchar *number;
	const gchar *name;
	VentureAccountKind kind;
	gint64 id;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(NULL != out_account_id, FALSE);

	*out_account_id = 0;

	if (!arb_classification(classification, &number, &name, &kind, error))
		return FALSE;

	if (organization_id <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "An arbitrage account belongs to one organization");
		return FALSE;
	}

	id = venture_setup_resolve_account(database, organization_id, classification,
	                                   "organization", 0, when, &local_error);

	if (NULL != local_error)
	{
		g_propagate_error(error, g_steal_pointer(&local_error));
		return FALSE;
	}

	if (id > 0)
	{
		*out_account_id = id;
		return TRUE;
	}

	/* Scoped, like the session income and currency clearing accounts: a
	 * code an existing chart cannot already hold. */
	code = g_strdup_printf("%" G_GINT64_FORMAT ":%s", organization_id, number);
	id = arb_find_scoped_account(database, organization_id, code, error);

	if (id < 0)
		return FALSE;

	*out_account_id = id;

	return TRUE;
}

gint64
venture_arbitrage_account(
	VentureDatabase		 *database,
	gint64			  organization_id,
	const gchar		 *classification,
	GDateTime		 *when,
	const VentureActor	 *actor,
	GError			**error
){
	g_autofree gchar *code = NULL;
	const gchar *number;
	const gchar *name;
	VentureAccountKind kind;
	gint64 id;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), 0);

	if (!venture_arbitrage_find_account(database, organization_id, classification, when,
	                                    &id, error))
		return 0;

	if (id > 0)
		return id;

	/* Only a writer gets here: the leg posting rule (inside the save that
	 * posts), executing a leg, closing and writing off. A page, a report
	 * or a summary asks venture_arbitrage_find_account() instead, so
	 * looking at a trade never adds to the chart. */
	if (!arb_classification(classification, &number, &name, &kind, error))
		return 0;

	code = g_strdup_printf("%" G_GINT64_FORMAT ":%s", organization_id, number);

	return arb_scoped_account(database, organization_id, code, name, kind, actor, error);
}

/* The organization's cash: the control map's, else the chart's 1000,
 * else one made under a scoped code -- the order the built-in sale and
 * expense rules find it in. */
static gint64
arb_organization_cash(
	VentureDatabase		 *database,
	gint64			  organization_id,
	GDateTime		 *when,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GError) local_error = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) found = NULL;
	g_autofree gchar *code = NULL;
	gint64 id;

	id = venture_setup_resolve_account(database, organization_id, "cash", "organization",
	                                   0, when, &local_error);

	if (NULL != local_error)
	{
		g_propagate_error(error, g_steal_pointer(&local_error));
		return 0;
	}

	if (id > 0)
		return id;

	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "1000", NULL);
	found = venture_database_find_one(database, query, &local_error);

	if (NULL != local_error)
	{
		g_propagate_error(error, g_steal_pointer(&local_error));
		return 0;
	}

	if (NULL != found)
		return venture_entity_get_id(found);

	code = g_strdup_printf("%" G_GINT64_FORMAT ":1000", organization_id);

	return arb_scoped_account(database, organization_id, code, "Cash",
	                          VENTURE_ACCOUNT_KIND_ASSET, actor, error);
}

gboolean
venture_arbitrage_venue_cash_account(
	VentureDatabase		 *database,
	gint64			  organization_id,
	gint64			  venue_id,
	GDateTime		 *when,
	const VentureActor	 *actor,
	gint64			 *out_account_id,
	GError			**error
){
	g_autoptr(VentureEntity) venue = NULL;
	gint64 location_id;
	gint64 account_id;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(NULL != out_account_id, FALSE);

	*out_account_id = 0;

	if (venue_id > 0)
		venue = venture_database_get(database, VENTURE_TYPE_VENUE, venue_id, NULL);

	location_id = arb_int(venue, "location-id");
	account_id = arb_int(venue, "account-id");

	/* A character's purse, a stall's till: the venue's location holds the
	 * money traded there, and its holding is made on first use. */
	if (location_id > 0)
		return venture_holdings_account_for_location(database, organization_id, location_id,
		                                             TRUE, actor, out_account_id, error);

	if (account_id > 0)
	{
		*out_account_id = account_id;
		return TRUE;
	}

	*out_account_id = arb_organization_cash(database, organization_id, when, actor, error);

	return *out_account_id > 0;
}

/*
 * Where one leg's money moves through: the holding at its own location --
 * the character who actually bought or sold, which a venue shared by
 * several characters cannot say -- else the venue's cash. Read from the
 * leg alone, so the posting rule stays pure.
 */
static gboolean
arb_leg_cash_account(
	VentureDatabase		 *database,
	gint64			  organization_id,
	VentureEntity		 *leg,
	GDateTime		 *when,
	const VentureActor	 *actor,
	gint64			 *out_account_id,
	GError			**error
){
	gint64 location_id;

	location_id = arb_int(leg, "location-id");

	if (location_id > 0)
		return venture_holdings_account_for_location(database, organization_id, location_id,
		                                             TRUE, actor, out_account_id, error);

	return venture_arbitrage_venue_cash_account(database, organization_id,
	                                            arb_int(leg, "venue-id"), when, actor,
	                                            out_account_id, error);
}

/* ==========================================================================
 * The leg posting rule
 * ========================================================================== */

/* A leg's amount: as written, else its unit price times its quantity. */
static VentureMoney *
arb_leg_amount(
	VentureEntity	 *leg,
	GError		**error
){
	g_autoptr(VentureMoney) unit = NULL;
	VentureMoney *amount;
	gint64 quantity;

	amount = arb_money(leg, "amount");

	if (NULL != amount)
		return amount;

	unit = arb_money(leg, "unit-price");
	quantity = arb_int(leg, "quantity");

	if ((NULL == unit) || (quantity <= 0))
		return NULL;

	return venture_money_multiply_int(unit, quantity, error);
}

/*
 * Only an executed leg with an amount -- written, or a unit price and a
 * quantity to make one -- belongs in the books. Asked of several versions
 * on one save, so it reads the record alone.
 */
static gboolean
arb_leg_postable(
	VentureEntity	*leg,
	gpointer	 user_data
){
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) unit = NULL;

	(void)user_data;

	if (VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED != arb_enum(leg, "status"))
		return FALSE;

	amount = arb_money(leg, "amount");
	unit = arb_money(leg, "unit-price");

	return (NULL != amount) || ((NULL != unit) && (arb_int(leg, "quantity") > 0));
}

/* Appends a debit/credit pair of @amount's magnitude, the sides swapped
 * when it is negative. A zero adds nothing. */
static void
arb_pair(
	GPtrArray		*rows,
	gint64			 organization_id,
	gint64			 debit,
	gint64			 credit,
	const VentureMoney	*amount
){
	g_autoptr(VentureMoney) magnitude = NULL;
	VentureJournalLine *line;
	gboolean negative;

	if ((NULL == amount) || venture_money_is_zero(amount))
		return;

	negative = venture_money_is_negative(amount);
	magnitude = venture_money_abs(amount);

	line = venture_journal_line_new();
	g_object_set(line, "account-id", negative ? credit : debit, "amount", magnitude,
	             "organization-id", organization_id, "side", VENTURE_LEDGER_SIDE_DEBIT, NULL);
	g_ptr_array_add(rows, line);

	line = venture_journal_line_new();
	g_object_set(line, "account-id", negative ? debit : credit, "amount", magnitude,
	             "organization-id", organization_id, "side", VENTURE_LEDGER_SIDE_CREDIT, NULL);
	g_ptr_array_add(rows, line);
}

G_DECLARE_FINAL_TYPE(VentureArbitrageLegPostingRule, venture_arbitrage_leg_posting_rule,
                     VENTURE, ARBITRAGE_LEG_POSTING_RULE, GObject)

struct _VentureArbitrageLegPostingRule
{
	GObject parent_instance;
};

static void venture_arbitrage_leg_posting_rule_iface(VenturePostingRuleInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(VentureArbitrageLegPostingRule, venture_arbitrage_leg_posting_rule, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_POSTING_RULE, venture_arbitrage_leg_posting_rule_iface))

static const gchar *
venture_arbitrage_leg_posting_rule_name(VenturePostingRule *rule)
{
	(void)rule;

	return VENTURE_ARBITRAGE_LEG_RULE;
}

/*
 * The lines one leg posts, always in this order: its principal (if it has
 * one), then its fees. Pure: the same leg gives the same lines, and the
 * only writes are accounts found or made on first use, which the second
 * build finds. Never the inventory service -- the stock side of a leg was
 * posted once, by execute, and is not rebuilt here.
 */
static GPtrArray *
venture_arbitrage_leg_posting_rule_lines(
	VenturePostingRule	 *rule,
	VentureDatabase		 *database,
	VentureEntity		 *leg,
	GError			**error
){
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) fees = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(GError) local_error = NULL;
	VentureArbitrageLegKind kind;
	gint64 organization_id;
	gint64 cash;
	gint64 positions;
	gint64 fee_account;
	gboolean stock;
	gboolean principal;

	(void)rule;

	if (!VENTURE_IS_ARBITRAGE_LEG(leg))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "The arbitrage_leg rule posts arbitrage legs only");
		return NULL;
	}

	organization_id = venture_entity_get_organization_id(leg);

	if (organization_id <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "Posting requires a legal entity");
		return NULL;
	}

	rows = g_ptr_array_new_with_free_func(g_object_unref);
	amount = arb_leg_amount(leg, &local_error);

	if (NULL != local_error)
	{
		g_propagate_error(error, g_steal_pointer(&local_error));
		return NULL;
	}

	/* Not postable after all: nothing to build. */
	if (NULL == amount)
		return g_steal_pointer(&rows);

	kind = (VentureArbitrageLegKind)arb_enum(leg, "kind");
	fees = arb_money(leg, "fees");
	when = arb_time(leg, "occurred-at");

	if (NULL == when)
		when = arb_time(leg, "created-at");

	stock = arb_int(leg, "inventory-item-id") > 0;

	/* A stock buy's principal and a write-off are the stock side's, done
	 * once by execute and abandon; a stock sell still brings its proceeds
	 * into the venue's cash out of the position here. */
	principal = !venture_money_is_zero(amount) &&
	            (VENTURE_ARBITRAGE_LEG_KIND_WRITE_OFF != kind) &&
	            !(stock && (VENTURE_ARBITRAGE_LEG_KIND_BUY == kind));

	if (!principal && ((NULL == fees) || venture_money_is_zero(fees)))
		return g_steal_pointer(&rows);

	if (!arb_leg_cash_account(database, organization_id, leg, when, NULL, &cash, error))
		return NULL;

	if (principal)
	{
		if (VENTURE_ARBITRAGE_LEG_KIND_FEE == kind)
		{
			fee_account = venture_arbitrage_account(database, organization_id,
			                                        "arbitrage_fees", when, NULL, error);

			if (fee_account <= 0)
				return NULL;

			arb_pair(rows, organization_id, fee_account, cash, amount);
		}
		else
		{
			gint direction;

			positions = venture_arbitrage_account(database, organization_id,
			                                      "arbitrage_positions", when, NULL, error);

			if (positions <= 0)
				return NULL;

			direction = venture_arbitrage_leg_direction(kind, amount);

			/* Out: the money leaves the venue for the position. In: it
			 * comes back out of the position. A transfer arriving is a
			 * negative amount, which arb_pair turns the right way. */
			if (VENTURE_ARBITRAGE_LEG_KIND_TRANSFER == kind)
				arb_pair(rows, organization_id, positions, cash, amount);
			else if (direction > 0)
				arb_pair(rows, organization_id, positions, cash, amount);
			else
				arb_pair(rows, organization_id, cash, positions, amount);
		}
	}

	if ((NULL != fees) && !venture_money_is_zero(fees))
	{
		fee_account = venture_arbitrage_account(database, organization_id, "arbitrage_fees",
		                                        when, NULL, error);

		if (fee_account <= 0)
			return NULL;

		arb_pair(rows, organization_id, fee_account, cash, fees);
	}

	return g_steal_pointer(&rows);
}

static void
venture_arbitrage_leg_posting_rule_iface(VenturePostingRuleInterface *iface)
{
	iface->get_name = venture_arbitrage_leg_posting_rule_name;
	iface->build_lines = venture_arbitrage_leg_posting_rule_lines;
}

static void
venture_arbitrage_leg_posting_rule_class_init(VentureArbitrageLegPostingRuleClass *klass)
{
	(void)klass;
}

static void
venture_arbitrage_leg_posting_rule_init(VentureArbitrageLegPostingRule *self)
{
	(void)self;
}

/* ==========================================================================
 * Validators
 * ========================================================================== */

/*
 * Refuses a reference into another organization, judged only when the
 * value is being written -- the rule every reference follows -- so a row
 * pointing somewhere since moved stays editable. A missing target is the
 * reference check's to refuse, with its own message.
 */
static gboolean
arb_same_organization(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	const gchar	 *property,
	GType		  target_type,
	const gchar	 *label,
	GError		**error
){
	g_autoptr(VentureEntity) target = NULL;
	gint64 target_id;

	target_id = arb_int(entity, property);

	if (target_id <= 0)
		return TRUE;

	if ((NULL != previous) && (arb_int(previous, property) == target_id))
		return TRUE;

	target = venture_database_get(database, target_type, target_id, NULL);

	if ((NULL != target) &&
	    (venture_entity_get_organization_id(target) != venture_entity_get_organization_id(entity)))
	{
		venture_set_error_validation(error, label,
			"#%" G_GINT64_FORMAT " belongs to another organization", target_id);
		return FALSE;
	}

	return TRUE;
}

static gboolean
arb_trade_is_over(gint status)
{
	return (VENTURE_ARBITRAGE_TRADE_STATUS_CLOSED == status) ||
	       (VENTURE_ARBITRAGE_TRADE_STATUS_ABANDONED == status);
}

/*
 * Whether two stored JSON texts say the same thing. Compared as parsed
 * values, so a form that posts the snapshot back re-indented or with its
 * members in another order is not a change; text that does not parse is
 * compared as text.
 */
static gboolean
arb_json_text_same(
	const gchar	*left,
	const gchar	*right
){
	g_autoptr(JsonNode) left_node = NULL;
	g_autoptr(JsonNode) right_node = NULL;

	if (venture_string_is_empty(left) || venture_string_is_empty(right))
		return venture_string_is_empty(left) == venture_string_is_empty(right);

	left_node = venture_json_parse(left, NULL);
	right_node = venture_json_parse(right, NULL);

	if ((NULL == left_node) || (NULL == right_node))
		return 0 == g_strcmp0(left, right);

	return json_node_equal(left_node, right_node);
}

/*
 * A trade's rules:
 *
 *  - closed and abandoned are reached through Close and Abandon, and left
 *    through Reopen: each of them posts or reverses, and a status typed
 *    into a form would do neither. Planned and open move freely;
 *  - the closing journal is Close's stamp;
 *  - `opened-at` and `closed-at` follow the status, the factory's rule for
 *    a derived date: stamped when empty, kept when given, cleared on
 *    leaving the state, refused when typed on a trade never in it. A
 *    trade does not close before it opened;
 *  - `expected` is a JSON object, or nothing;
 *  - its venture is in its organization.
 */
static gboolean
venture_arbitrage_validate_trade(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(GDateTime) opened = NULL;
	g_autoptr(GDateTime) closed = NULL;
	g_autoptr(GDateTime) was_opened = NULL;
	g_autoptr(GDateTime) was_closed = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *was_expected = NULL;
	gboolean permitted;
	gint status;
	gint was_status;

	(void)user_data;

	permitted = arb_permitted(database, entity);
	status = arb_enum(entity, "status");
	was_status = (NULL != previous) ? arb_enum(previous, "status")
	                                : VENTURE_ARBITRAGE_TRADE_STATUS_PLANNED;

	/* --- What only the actions write --- */

	if (!permitted)
	{
		g_autofree gchar *external = NULL;
		g_autofree gchar *was_external = NULL;

		/* Where a recorded flip came from is how recording again knows
		 * it is there: changed by hand, the next run records it twice. */
		g_object_get(entity, "external-ref", &external, NULL);

		if (NULL != previous)
			g_object_get(previous, "external-ref", &was_external, NULL);

		if ((arb_int(entity, "data-source-id") != arb_int(previous, "data-source-id")) ||
		    (0 != g_strcmp0(venture_string_is_empty(external) ? NULL : external,
		                    venture_string_is_empty(was_external) ? NULL : was_external)))
		{
			venture_set_error_validation(error, "External reference",
				"is set by Record flips, which records each sale of a data source's "
				"ledger once; it cannot be written by hand");
			return FALSE;
		}

		/* A recorded flip's snapshot is the ledger rows it took, which
		 * the next Record flips subtracts before matching again: edited,
		 * the rows it no longer names are free, and the same sale is
		 * recorded a second time beside its journals. The whole snapshot
		 * is held, not only its source, so its profit stays what the
		 * ledger said it made. */
		if ((arb_int(entity, "data-source-id") > 0) || (arb_int(previous, "data-source-id") > 0))
		{
			g_autofree gchar *snapshot = NULL;
			g_autofree gchar *was_snapshot = NULL;

			g_object_get(entity, "expected", &snapshot, NULL);

			if (NULL != previous)
				g_object_get(previous, "expected", &was_snapshot, NULL);

			if (!arb_json_text_same(snapshot, was_snapshot))
			{
				venture_set_error_validation(error, "Expected",
					"of a recorded flip is the ledger rows it took, set by Record flips; "
					"it cannot be written by hand, or the next run records the sale again");
				return FALSE;
			}
		}

		if (arb_int(entity, "close-journal-id") != arb_int(previous, "close-journal-id"))
		{
			venture_set_error_validation(error, "Closing journal",
				"is set by the Close action, not by hand");
			return FALSE;
		}

		if (arb_trade_is_over(status) && ((NULL == previous) || (status != was_status)))
		{
			venture_set_error_validation(error, "Status",
				"%s is set by the %s action, which moves what is left on the "
				"positions account to gains; run it instead",
				venture_enum_to_nick(VENTURE_TYPE_ARBITRAGE_TRADE_STATUS, status),
				(VENTURE_ARBITRAGE_TRADE_STATUS_CLOSED == status) ? "Close" : "Abandon");
			return FALSE;
		}

		if ((NULL != previous) && arb_trade_is_over(was_status) && (status != was_status))
		{
			venture_set_error_validation(error, "Status",
				"the trade is %s; the Reopen action reverses its closing journals "
				"and opens it again",
				venture_enum_to_nick(VENTURE_TYPE_ARBITRAGE_TRADE_STATUS, was_status));
			return FALSE;
		}

		/* A finished trade's closing time is the date its close journals
		 * carry, and the P&L counts it in the period of that time: moved by
		 * hand, the result lands in one period and the gains in another.
		 * Restored rather than refused, like a session's posted-at: a form
		 * posts every field back, at whatever precision it rendered, and
		 * the way to move it is Reopen and close again. */
		if ((NULL != previous) && arb_trade_is_over(was_status) && arb_trade_is_over(status))
		{
			g_autoptr(GDateTime) then_closed = arb_time(previous, "closed-at");

			g_object_set(entity, "closed-at", then_closed, NULL);
		}
	}

	/* --- The derived times --- */

	opened = arb_time(entity, "opened-at");
	closed = arb_time(entity, "closed-at");
	was_opened = arb_time(previous, "opened-at");
	was_closed = arb_time(previous, "closed-at");

	if (VENTURE_ARBITRAGE_TRADE_STATUS_PLANNED == status)
	{
		if (NULL != opened)
		{
			/* Back to planned clears it; typed onto a planned trade it
			 * says something that did not happen. */
			if ((NULL != previous) && (VENTURE_ARBITRAGE_TRADE_STATUS_PLANNED != was_status))
				g_clear_pointer(&opened, g_date_time_unref);
			else if (arb_time_differs(opened, was_opened))
			{
				venture_set_error_validation(error, "Opened",
					"a planned trade has not opened; set it open, or leave this empty");
				return FALSE;
			}
			else
				g_clear_pointer(&opened, g_date_time_unref);

			g_object_set(entity, "opened-at", NULL, NULL);
		}
	}
	else if (NULL == opened)
	{
		opened = venture_time_now();
		g_object_set(entity, "opened-at", opened, NULL);
	}

	if (arb_trade_is_over(status))
	{
		if (NULL == closed)
		{
			closed = venture_time_now();
			g_object_set(entity, "closed-at", closed, NULL);
		}
	}
	else if (NULL != closed)
	{
		if ((NULL != previous) && arb_trade_is_over(was_status))
			g_object_set(entity, "closed-at", NULL, NULL);
		else if (arb_time_differs(closed, was_closed))
		{
			venture_set_error_validation(error, "Closed",
				"only a closed or abandoned trade has a closing time; the Close "
				"action sets it");
			return FALSE;
		}
		else
			g_object_set(entity, "closed-at", NULL, NULL);

		g_clear_pointer(&closed, g_date_time_unref);
	}

	if ((NULL != opened) && (NULL != closed) && (g_date_time_compare(closed, opened) < 0))
	{
		venture_set_error_validation(error, "Closed",
			"is before the trade opened; a trade closes after it opens");
		return FALSE;
	}

	/* --- The expected snapshot --- */

	g_object_get(entity, "expected", &expected, NULL);

	if (NULL != previous)
		g_object_get(previous, "expected", &was_expected, NULL);

	if (!venture_string_is_empty(expected) && (0 != g_strcmp0(expected, was_expected)))
	{
		g_autoptr(JsonNode) parsed = NULL;
		g_autoptr(GError) parse_error = NULL;

		parsed = venture_json_parse(expected, &parse_error);

		if ((NULL == parsed) || !JSON_NODE_HOLDS_OBJECT(parsed))
		{
			venture_set_error_validation(error, "Expected",
				"must be a JSON object, such as {\"profit\": [\"12.00 GOLD\"]}");
			return FALSE;
		}
	}

	return arb_same_organization(database, entity, previous, "venture-id",
	                             VENTURE_TYPE_VENTURE, "Venture", error);
}

/* Whether anything a leg posts, or the stock it moved, differs. */
static gboolean
arb_leg_financial_change(
	VentureEntity	*entity,
	VentureEntity	*previous
){
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) was_amount = NULL;
	g_autoptr(VentureMoney) fees = NULL;
	g_autoptr(VentureMoney) was_fees = NULL;
	g_autoptr(VentureMoney) unit = NULL;
	g_autoptr(VentureMoney) was_unit = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(GDateTime) was_when = NULL;

	if (NULL == previous)
		return TRUE;

	amount = arb_money(entity, "amount");
	was_amount = arb_money(previous, "amount");
	fees = arb_money(entity, "fees");
	was_fees = arb_money(previous, "fees");
	unit = arb_money(entity, "unit-price");
	was_unit = arb_money(previous, "unit-price");
	when = arb_time(entity, "occurred-at");
	was_when = arb_time(previous, "occurred-at");

	return (arb_enum(entity, "status") != arb_enum(previous, "status")) ||
	       (arb_enum(entity, "kind") != arb_enum(previous, "kind")) ||
	       (arb_int(entity, "venue-id") != arb_int(previous, "venue-id")) ||
	       (arb_int(entity, "location-id") != arb_int(previous, "location-id")) ||
	       (arb_int(entity, "trade-id") != arb_int(previous, "trade-id")) ||
	       (arb_int(entity, "inventory-item-id") != arb_int(previous, "inventory-item-id")) ||
	       (arb_int(entity, "quantity") != arb_int(previous, "quantity")) ||
	       arb_money_differs(amount, was_amount) || arb_money_differs(fees, was_fees) ||
	       arb_money_differs(unit, was_unit) || arb_time_differs(when, was_when);
}

/* Refuses a financial change to an executed leg of a closed trade. */
static gboolean
arb_check_frozen(
	VentureDatabase	 *database,
	gint64		  trade_id,
	gboolean	  executed,
	GError		**error
){
	g_autoptr(VentureEntity) trade = NULL;
	g_autofree gchar *name = NULL;
	gint status;

	if (!executed || (trade_id <= 0))
		return TRUE;

	trade = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE, trade_id, NULL);
	status = arb_enum(trade, "status");

	if ((NULL == trade) || !arb_trade_is_over(status))
		return TRUE;

	name = venture_entity_get_display_name(trade);
	venture_set_error_validation(error, "Trade",
		"%s is %s: its executed legs are what its closing journals were posted "
		"from, so they cannot change. Reopen it first", name,
		venture_enum_to_nick(VENTURE_TYPE_ARBITRAGE_TRADE_STATUS, status));

	return FALSE;
}

/*
 * A leg's rules, in the order a person would fix them:
 *
 *  - stamps (`inventory-txn-id`, `cost`) and write-off legs are the
 *    actions';
 *  - a leg that moved stock is frozen for good: the units are on a shelf
 *    or gone, and changing or cancelling the leg would leave the shelf and
 *    the books disagreeing. A correction is a counter-leg;
 *  - an executed leg of a closed or abandoned trade is frozen, and no leg
 *    executes into one: the closing journals were posted from them;
 *  - a trade, in the leg's own organization (held always: a leg is part
 *    of its trade, not a reference out);
 *  - stock only on a buy, a sell or a write-off, with a quantity of at
 *    least one, executed only by the Execute action;
 *  - amounts: never negative except a transfer arriving; fees never
 *    negative and in the amount's currency; a unit price in it too, and
 *    with a quantity it fills an empty amount; the venue's currency when
 *    the venue names one;
 *  - executed needs an amount, and gets a time when it has none;
 *  - venue, instrument and stock in the leg's organization, and the stock
 *    holding what the instrument is.
 *
 * Never writes the trade: a derived write would bump its version under
 * whoever is editing it, and a validator is no place for a second save.
 */
static gboolean
venture_arbitrage_validate_leg(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) fees = NULL;
	g_autoptr(VentureMoney) unit = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) was_cost = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *detail = NULL;
	g_autofree gchar *was_detail = NULL;
	VentureArbitrageLegKind kind;
	gboolean permitted;
	gboolean executed;
	gboolean was_executed;
	gint status;
	gint64 trade_id;
	gint64 item_id;
	gint64 quantity;
	gint64 was_txn;

	(void)user_data;

	permitted = arb_permitted(database, entity);
	kind = (VentureArbitrageLegKind)arb_enum(entity, "kind");
	status = arb_enum(entity, "status");
	trade_id = arb_int(entity, "trade-id");
	item_id = arb_int(entity, "inventory-item-id");
	quantity = arb_int(entity, "quantity");
	was_txn = arb_int(previous, "inventory-txn-id");
	executed = (VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED == status);
	was_executed = (NULL != previous) &&
	               (VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED == arb_enum(previous, "status"));
	cost = arb_money(entity, "cost");
	was_cost = arb_money(previous, "cost");
	g_object_get(entity, "cost-detail", &detail, NULL);

	if (NULL != previous)
		g_object_get(previous, "cost-detail", &was_detail, NULL);

	/* --- What only the actions write --- */

	if (!permitted)
	{
		if ((arb_int(entity, "inventory-txn-id") != was_txn) || arb_money_differs(cost, was_cost) ||
		    (0 != g_strcmp0(venture_string_is_empty(detail) ? NULL : detail,
		                    venture_string_is_empty(was_detail) ? NULL : was_detail)))
		{
			venture_set_error_validation(error, "Stock movement",
				"the movement and the cost of stock are set by the Execute action, "
				"not by hand");
			return FALSE;
		}

		if ((VENTURE_ARBITRAGE_LEG_KIND_WRITE_OFF == kind) &&
		    ((NULL == previous) ||
		     (VENTURE_ARBITRAGE_LEG_KIND_WRITE_OFF != arb_enum(previous, "kind"))))
		{
			venture_set_error_validation(error, "Kind",
				"write_off legs are written by the Abandon action when it writes "
				"stock off; abandon the trade instead");
			return FALSE;
		}

		/* --- Frozen: it moved stock --- */

		if ((was_txn > 0) && arb_leg_financial_change(entity, previous))
		{
			venture_set_error_validation(error, "Leg",
				"has moved stock (movement #%" G_GINT64_FORMAT "), so it cannot be "
				"changed or cancelled: the units are on a shelf or gone. Record a "
				"counter-leg instead -- a sell for a buy, a buy for a sell", was_txn);
			return FALSE;
		}

		/* --- Frozen: its trade is over --- */

		if (arb_leg_financial_change(entity, previous))
		{
			if (!arb_check_frozen(database, trade_id, executed || was_executed, error))
				return FALSE;

			/* Moved out of a closed trade: frozen there too. */
			if ((NULL != previous) && (arb_int(previous, "trade-id") != trade_id) &&
			    !arb_check_frozen(database, arb_int(previous, "trade-id"), was_executed, error))
				return FALSE;
		}
	}

	/* --- The trade --- */

	if (trade_id <= 0)
	{
		venture_set_error_validation(error, "Trade", "is required");
		return FALSE;
	}

	trade = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE, trade_id, NULL);

	if (NULL == trade)
	{
		venture_set_error_validation(error, "Trade",
			"#%" G_GINT64_FORMAT " does not exist", trade_id);
		return FALSE;
	}

	if (venture_entity_get_organization_id(trade) != venture_entity_get_organization_id(entity))
	{
		venture_set_error_validation(error, "Trade",
			"#%" G_GINT64_FORMAT " belongs to another organization", trade_id);
		return FALSE;
	}

	/* --- Stock --- */

	if ((item_id > 0) && (VENTURE_ARBITRAGE_LEG_KIND_BUY != kind) &&
	    (VENTURE_ARBITRAGE_LEG_KIND_SELL != kind) && (VENTURE_ARBITRAGE_LEG_KIND_WRITE_OFF != kind))
	{
		venture_set_error_validation(error, "Stock",
			"only a buy or a sell moves stock; leave Stock empty on a %s",
			venture_enum_to_nick(VENTURE_TYPE_ARBITRAGE_LEG_KIND, kind));
		return FALSE;
	}

	if (quantity < 0)
	{
		venture_set_error_validation(error, "Quantity", "cannot be negative");
		return FALSE;
	}

	if ((item_id > 0) && (quantity < 1))
	{
		venture_set_error_validation(error, "Quantity",
			"must be at least 1 when the leg moves stock");
		return FALSE;
	}

	/* --- Amounts --- */

	amount = arb_money(entity, "amount");
	fees = arb_money(entity, "fees");
	unit = arb_money(entity, "unit-price");

	if ((NULL != unit) && venture_money_is_negative(unit))
	{
		venture_set_error_validation(error, "Unit price", "cannot be negative");
		return FALSE;
	}

	if ((NULL != unit) && (NULL != amount) &&
	    (0 != g_strcmp0(venture_money_get_currency(unit), venture_money_get_currency(amount))))
	{
		venture_set_error_validation(error, "Unit price",
			"is in %s and the amount in %s; one leg is one currency",
			venture_money_get_currency(unit), venture_money_get_currency(amount));
		return FALSE;
	}

	/* An empty amount filled from the price: derived, like a session's
	 * minutes, so the books and the page read one figure. */
	if ((NULL == amount) && (NULL != unit) && (quantity > 0))
	{
		amount = venture_money_multiply_int(unit, quantity, error);

		if (NULL == amount)
			return FALSE;

		g_object_set(entity, "amount", amount, NULL);
	}

	if ((NULL != amount) && venture_money_is_negative(amount) &&
	    (VENTURE_ARBITRAGE_LEG_KIND_TRANSFER != kind))
	{
		venture_set_error_validation(error, "Amount",
			"cannot be negative on a %s; its kind says which way the money went",
			venture_enum_to_nick(VENTURE_TYPE_ARBITRAGE_LEG_KIND, kind));
		return FALSE;
	}

	if ((NULL != amount) && venture_money_is_negative(amount) && (item_id > 0))
	{
		venture_set_error_validation(error, "Amount", "cannot be negative when it moves stock");
		return FALSE;
	}

	if (NULL != fees)
	{
		if (venture_money_is_negative(fees))
		{
			venture_set_error_validation(error, "Fees",
				"cannot be negative; a rebate is a refund leg");
			return FALSE;
		}

		if (NULL == amount)
		{
			venture_set_error_validation(error, "Fees",
				"need the leg's amount beside them; zero is an amount");
			return FALSE;
		}

		if (0 != g_strcmp0(venture_money_get_currency(fees), venture_money_get_currency(amount)))
		{
			venture_set_error_validation(error, "Fees",
				"are in %s and the amount in %s; charge fees in another currency "
				"as a fee leg of their own", venture_money_get_currency(fees),
				venture_money_get_currency(amount));
			return FALSE;
		}
	}

	/* The venue's currency, judged when the amount or the venue is written:
	 * a venue that prices in gold does not take tickets. */
	if ((NULL != amount) && (arb_int(entity, "venue-id") > 0))
	{
		g_autoptr(VentureMoney) was_amount = NULL;

		was_amount = arb_money(previous, "amount");

		if ((NULL == previous) || arb_money_differs(amount, was_amount) ||
		    (arb_int(previous, "venue-id") != arb_int(entity, "venue-id")))
		{
			g_autoptr(VentureEntity) venue = NULL;
			g_autofree gchar *currency = NULL;

			venue = venture_database_get(database, VENTURE_TYPE_VENUE,
			                             arb_int(entity, "venue-id"), NULL);

			if (NULL != venue)
				g_object_get(venue, "currency", &currency, NULL);

			if (!venture_string_is_empty(currency) &&
			    (0 != g_ascii_strcasecmp(currency, venture_money_get_currency(amount))))
			{
				g_autofree gchar *venue_name = NULL;

				venue_name = venture_entity_get_display_name(venue);
				venture_set_error_validation(error, "Amount",
					"is in %s, and %s trades in %s", venture_money_get_currency(amount),
					venue_name, currency);
				return FALSE;
			}
		}
	}

	/* --- Execution --- */

	if (executed && !was_executed)
	{
		if ((item_id > 0) && !permitted)
		{
			venture_set_error_validation(error, "Status",
				"this leg moves stock: run the Execute action, which receives or "
				"issues the units and then executes it");
			return FALSE;
		}

		if ((NULL == amount) && (VENTURE_ARBITRAGE_LEG_KIND_WRITE_OFF != kind))
		{
			venture_set_error_validation(error, "Amount",
				"an executed leg needs the money that moved; zero is an amount");
			return FALSE;
		}

		if (!permitted && !arb_check_frozen(database, trade_id, TRUE, error))
			return FALSE;
	}

	/* Stock given to a leg that already executed as cash. The rule posts
	 * a stock buy's principal through the receipt, not the leg, so the
	 * re-save would reverse the cash it paid while no units arrive: the
	 * venue's cash comes back and the purchase reads as profit. Only
	 * Execute pairs a leg with its movement. Judged when written, so a
	 * leg that keeps what it had stays editable. */
	if (executed && was_executed && !permitted && (item_id > 0) &&
	    (arb_int(entity, "inventory-txn-id") <= 0) &&
	    ((NULL == previous) || (arb_int(previous, "inventory-item-id") != item_id)))
	{
		venture_set_error_validation(error, "Stock",
			"this leg executed without moving stock, and only Execute moves the "
			"units; cancel it and record a stock leg instead");
		return FALSE;
	}

	when = arb_time(entity, "occurred-at");

	if (executed && (NULL == when))
	{
		when = venture_time_now();
		g_object_set(entity, "occurred-at", when, NULL);
	}

	/* --- Where it belongs --- */

	if (!arb_same_organization(database, entity, previous, "venue-id",
	                           VENTURE_TYPE_VENUE, "Venue", error) ||
	    !arb_same_organization(database, entity, previous, "location-id",
	                           VENTURE_TYPE_LOCATION, "Paid from or into", error) ||
	    !arb_same_organization(database, entity, previous, "instrument-id",
	                           VENTURE_TYPE_INSTRUMENT, "Instrument", error))
		return FALSE;

	if ((item_id > 0) &&
	    ((NULL == previous) || (arb_int(previous, "inventory-item-id") != item_id) ||
	     (arb_int(previous, "instrument-id") != arb_int(entity, "instrument-id"))))
	{
		g_autoptr(VentureEntity) item = NULL;
		g_autoptr(VentureEntity) instrument = NULL;
		gint64 product_id;
		gint64 instrument_product;

		if (!arb_sales_enabled())
		{
			venture_set_error_validation(error, "Stock",
				"belongs to the sales module, which is off");
			return FALSE;
		}

		item = venture_database_get(database, VENTURE_TYPE_INVENTORY_ITEM, item_id, NULL);

		if (NULL == item)
			return TRUE;

		if (venture_entity_get_organization_id(item) != venture_entity_get_organization_id(entity))
		{
			venture_set_error_validation(error, "Stock",
				"#%" G_GINT64_FORMAT " belongs to another organization", item_id);
			return FALSE;
		}

		product_id = arb_int(item, "product-id");

		if (arb_int(entity, "instrument-id") > 0)
			instrument = venture_database_get(database, VENTURE_TYPE_INSTRUMENT,
			                                  arb_int(entity, "instrument-id"), NULL);

		instrument_product = arb_int(instrument, "product-id");

		if ((instrument_product > 0) && (product_id > 0) && (instrument_product != product_id))
		{
			g_autofree gchar *holds = NULL;
			g_autofree gchar *is = NULL;

			holds = arb_name_of(database, VENTURE_TYPE_PRODUCT, "product", product_id);
			is = arb_name_of(database, VENTURE_TYPE_PRODUCT, "product", instrument_product);
			venture_set_error_validation(error, "Stock",
				"#%" G_GINT64_FORMAT " holds %s, and the instrument is %s",
				item_id, holds, is);
			return FALSE;
		}
	}

	return TRUE;
}

/* ==========================================================================
 * Legs and figures
 * ========================================================================== */

GPtrArray *
venture_arbitrage_trade_legs(
	VentureDatabase	 *database,
	gint64		  trade_id,
	gboolean	  include_deleted,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_LEG);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, include_deleted);

	if (!venture_query_add_filter_int(query, "trade-id", VENTURE_FILTER_OP_EQ, trade_id, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	return venture_database_find(database, query, error);
}

void
venture_arbitrage_figures_free(VentureArbitrageFigures *figures)
{
	if (NULL == figures)
		return;

	g_free(figures->currency);
	g_clear_pointer(&figures->capital, venture_money_free);
	g_clear_pointer(&figures->returned, venture_money_free);
	g_clear_pointer(&figures->stock_bought, venture_money_free);
	g_clear_pointer(&figures->stock_cost, venture_money_free);
	g_clear_pointer(&figures->fees, venture_money_free);
	g_clear_pointer(&figures->realised, venture_money_free);
	g_free(figures);
}

const VentureArbitrageFigures *
venture_arbitrage_figures_lookup(
	GPtrArray	*figures,
	const gchar	*currency
){
	guint i;

	for (i = 0; (NULL != figures) && (i < figures->len); i++)
	{
		const VentureArbitrageFigures *row;

		row = g_ptr_array_index(figures, i);

		if (0 == g_strcmp0(row->currency, currency))
			return row;
	}

	return NULL;
}

/* The figures for @currency in @figures, made (all zeros) when absent. */
static VentureArbitrageFigures *
arb_figures_for(
	GPtrArray		*figures,
	const VentureMoney	*like
){
	VentureArbitrageFigures *row;
	const gchar *currency;

	currency = venture_money_get_currency(like);
	row = (VentureArbitrageFigures *)venture_arbitrage_figures_lookup(figures, currency);

	if (NULL != row)
		return row;

	row = g_new0(VentureArbitrageFigures, 1);
	row->currency = g_strdup(currency);
	row->capital = venture_money_new_zero(currency);
	row->returned = venture_money_new_zero(currency);
	row->stock_bought = venture_money_new_zero(currency);
	row->stock_cost = venture_money_new_zero(currency);
	row->fees = venture_money_new_zero(currency);
	g_ptr_array_add(figures, row);

	return row;
}

static gint
arb_figures_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureArbitrageFigures *left = *(VentureArbitrageFigures *const *)a;
	const VentureArbitrageFigures *right = *(VentureArbitrageFigures *const *)b;

	return g_strcmp0(left->currency, right->currency);
}

/* Adds a leg's cost detail, {CODE: amount}, to the figures' stock cost. */
static gboolean
arb_add_cost_detail(
	GPtrArray	 *figures,
	const gchar	 *detail,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	JsonObjectIter iter;
	const gchar *code;
	JsonNode *value;

	node = json_from_string(detail, NULL);

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "A leg's cost detail is not a JSON object");
		return FALSE;
	}

	json_object_iter_init(&iter, json_node_get_object(node));

	while (json_object_iter_next(&iter, &code, &value))
	{
		g_autoptr(VentureMoney) part = NULL;

		if (!JSON_NODE_HOLDS_VALUE(value) || (G_TYPE_STRING != json_node_get_value_type(value)))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "A leg's cost detail holds no amount for %s", code);
			return FALSE;
		}

		part = venture_money_from_string(json_node_get_string(value), code, error);

		if (NULL == part)
			return FALSE;

		/* The member names the currency; an amount naming another is
		 * not this part. */
		if (0 != g_strcmp0(venture_money_get_currency(part), code))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "A leg's cost detail gives %s an amount in %s", code,
			            venture_money_get_currency(part));
			return FALSE;
		}

		if (!arb_money_add(&arb_figures_for(figures, part)->stock_cost, part, error))
			return FALSE;
	}

	return TRUE;
}

/*
 * The one place a trade's per-currency figures are added up, for the page
 * and the report alike (see venture-arbitrage-private.h for what each
 * one is). Executed legs only, deleted ones included: a deletion is not a
 * correction, and their journals are still in the books. Each figure is
 * kept in its own currency; nothing here adds across two.
 */
GPtrArray *
venture_arbitrage_compute_figures(
	GPtrArray	 *legs,
	GError		**error
){
	g_autoptr(GPtrArray) figures = NULL;
	guint i;

	figures = g_ptr_array_new_with_free_func((GDestroyNotify)venture_arbitrage_figures_free);

	for (i = 0; (NULL != legs) && (i < legs->len); i++)
	{
		VentureEntity *leg;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) magnitude = NULL;
		g_autoptr(VentureMoney) fees = NULL;
		g_autoptr(VentureMoney) cost = NULL;
		VentureArbitrageFigures *row;
		VentureArbitrageLegKind kind;
		gboolean stock;

		leg = g_ptr_array_index(legs, i);

		if (VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED != arb_enum(leg, "status"))
			continue;

		kind = (VentureArbitrageLegKind)arb_enum(leg, "kind");
		stock = arb_int(leg, "inventory-item-id") > 0;
		amount = arb_leg_amount(leg, error);
		fees = arb_money(leg, "fees");
		cost = arb_money(leg, "cost");

		if ((NULL != error) && (NULL != *error))
			return NULL;

		if (NULL != amount)
		{
			row = arb_figures_for(figures, amount);
			magnitude = venture_money_abs(amount);

			if (VENTURE_ARBITRAGE_LEG_KIND_FEE == kind)
			{
				if (!arb_money_add(&row->fees, amount, error))
					return NULL;
			}
			else if (VENTURE_ARBITRAGE_LEG_KIND_WRITE_OFF != kind)
			{
				gint direction;

				direction = venture_arbitrage_leg_direction(kind, amount);

				if (!arb_money_add(direction > 0 ? &row->capital : &row->returned,
				                   magnitude, error))
					return NULL;

				if (stock && (VENTURE_ARBITRAGE_LEG_KIND_BUY == kind) &&
				    !arb_money_add(&row->stock_bought, magnitude, error))
					return NULL;
			}
		}

		if ((NULL != fees) && !arb_money_add(&arb_figures_for(figures, fees)->fees, fees, error))
			return NULL;

		/* What the units that left cost: a sell's FIFO cost, or a write-off's,
		 * in each currency it was paid in, which may not be the sale's. The
		 * detail when the action wrote one -- units made from gold and
		 * tickets cost both, and the positions account carries both --
		 * else the one cost a leg executed before it existed records. */
		if (VENTURE_ARBITRAGE_LEG_KIND_BUY != kind)
		{
			g_autofree gchar *detail = NULL;

			g_object_get(leg, "cost-detail", &detail, NULL);

			if (!venture_string_is_empty(detail))
			{
				if (!arb_add_cost_detail(figures, detail, error))
					return NULL;
			}
			else if ((NULL != cost) &&
			         !arb_money_add(&arb_figures_for(figures, cost)->stock_cost, cost, error))
				return NULL;
		}
	}

	for (i = 0; i < figures->len; i++)
	{
		VentureArbitrageFigures *row;
		g_autoptr(VentureMoney) spent = NULL;
		g_autoptr(VentureMoney) less_spent = NULL;
		g_autoptr(VentureMoney) less_cost = NULL;

		row = g_ptr_array_index(figures, i);

		/* realised = returned - (capital - stock_bought) - stock_cost - fees:
		 * money that became stock is not lost, and comes back as cost when
		 * the stock leaves. */
		spent = venture_money_subtract(row->capital, row->stock_bought, error);

		if (NULL == spent)
			return NULL;

		less_spent = venture_money_subtract(row->returned, spent, error);

		if (NULL == less_spent)
			return NULL;

		less_cost = venture_money_subtract(less_spent, row->stock_cost, error);

		if (NULL == less_cost)
			return NULL;

		row->realised = venture_money_subtract(less_cost, row->fees, error);

		if (NULL == row->realised)
			return NULL;
	}

	g_ptr_array_sort(figures, arb_figures_compare);

	return g_steal_pointer(&figures);
}

/* @text as money only when it names its currency: venture_money_from_string()
 * reads a bare "12.00" in the install's default currency, which is a
 * guess this figure must not make. */
static VentureMoney *
arb_expected_named(const gchar *text)
{
	g_autoptr(VentureMoney) money = NULL;
	g_autofree gchar *upper = NULL;

	money = venture_money_from_string(text, NULL, NULL);

	if (NULL == money)
		return NULL;

	upper = g_ascii_strup(text, -1);

	if (NULL == strstr(upper, venture_money_get_currency(money)))
		return NULL;

	return g_steal_pointer(&money);
}

/*
 * The expected profit a trade's snapshot names, per currency: `profit` as
 * one money string ("12.00 GOLD"), an array of them, or an object of code
 * to amount. Anything that does not name its currency is left out rather
 * than guessed into one.
 */
GPtrArray *
venture_arbitrage_expected_profit(const gchar *expected)
{
	g_autoptr(JsonNode) parsed = NULL;
	g_autoptr(GPtrArray) totals = NULL;
	JsonObject *object;
	JsonNode *profit;

	totals = venture_money_totals_new();

	if (venture_string_is_empty(expected))
		return g_steal_pointer(&totals);

	parsed = venture_json_parse(expected, NULL);

	if ((NULL == parsed) || !JSON_NODE_HOLDS_OBJECT(parsed))
		return g_steal_pointer(&totals);

	object = json_node_get_object(parsed);

	if (!json_object_has_member(object, "profit"))
		return g_steal_pointer(&totals);

	profit = json_object_get_member(object, "profit");

	if (JSON_NODE_HOLDS_VALUE(profit) && (G_TYPE_STRING == json_node_get_value_type(profit)))
	{
		g_autoptr(VentureMoney) money = NULL;

		money = arb_expected_named(json_node_get_string(profit));
		venture_money_totals_add(totals, money, NULL);
	}
	else if (JSON_NODE_HOLDS_ARRAY(profit))
	{
		JsonArray *items;
		guint i;

		items = json_node_get_array(profit);

		for (i = 0; i < json_array_get_length(items); i++)
		{
			JsonNode *item;
			g_autoptr(VentureMoney) money = NULL;

			item = json_array_get_element(items, i);

			if (!JSON_NODE_HOLDS_VALUE(item) || (G_TYPE_STRING != json_node_get_value_type(item)))
				continue;

			money = arb_expected_named(json_node_get_string(item));
			venture_money_totals_add(totals, money, NULL);
		}
	}
	else if (JSON_NODE_HOLDS_OBJECT(profit))
	{
		g_autoptr(GList) members = NULL;
		GList *member;

		members = json_object_get_members(json_node_get_object(profit));

		for (member = members; NULL != member; member = member->next)
		{
			JsonNode *item;
			g_autoptr(VentureMoney) money = NULL;
			g_autofree gchar *text = NULL;

			item = json_object_get_member(json_node_get_object(profit), member->data);

			if (!JSON_NODE_HOLDS_VALUE(item) || !venture_currency_is_valid(member->data))
				continue;

			if (G_TYPE_STRING == json_node_get_value_type(item))
				text = g_strdup(json_node_get_string(item));
			else if (G_TYPE_INT64 == json_node_get_value_type(item))
				text = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(item));
			else if (G_TYPE_DOUBLE == json_node_get_value_type(item))
			{
				/* Its shortest decimal spelling: read as an integer,
				 * 12.75 was 12. One the currency cannot hold is
				 * refused by the parser below and left out. */
				gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

				text = g_strdup(g_ascii_dtostr(buffer, sizeof(buffer), json_node_get_double(item)));
			}
			else
				continue;

			money = venture_money_from_string(text, member->data, NULL);

			if ((NULL != money) &&
			    (0 == g_ascii_strcasecmp(venture_money_get_currency(money), member->data)))
				venture_money_totals_add(totals, money, NULL);
		}
	}

	return g_steal_pointer(&totals);
}

/* ==========================================================================
 * The position
 * ========================================================================== */

/* Adds the ids of every journal @source_type #@source_id posted. */
static gboolean
arb_collect_journals(
	VenturePostingService	 *service,
	const gchar		 *source_type,
	gint64			  source_id,
	gint64			  organization_id,
	GHashTable		 *seen,
	GPtrArray		 *journals,
	GError			**error
){
	g_autoptr(GPtrArray) found = NULL;
	guint i;

	if (source_id <= 0)
		return TRUE;

	found = venture_posting_service_find_source(service, source_type, source_id,
	                                            organization_id, error);

	if (NULL == found)
		return FALSE;

	for (i = 0; i < found->len; i++)
	{
		VentureEntity *journal;
		gint64 id;

		journal = g_ptr_array_index(found, i);
		id = venture_entity_get_id(journal);

		if (g_hash_table_contains(seen, &id))
			continue;

		g_hash_table_add(seen, g_memdup2(&id, sizeof id));
		g_ptr_array_add(journals, g_object_ref(journal));
	}

	return TRUE;
}

/* Every journal the trade, its legs (deleted ones too) and the stock
 * movements those legs made posted, drafts excluded. */
static GPtrArray *
arb_trade_journals(
	VentureDatabase	 *database,
	VentureEntity	 *trade,
	GPtrArray	 *legs,
	GError		**error
){
	VenturePostingService *service;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) posted = NULL;
	g_autoptr(GHashTable) seen = NULL;
	gint64 organization_id;
	guint i;

	service = venture_database_get_posting_service(database);
	organization_id = venture_entity_get_organization_id(trade);
	journals = g_ptr_array_new_with_free_func(g_object_unref);
	seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	if (!arb_collect_journals(service, "arbitrage_trade", venture_entity_get_id(trade),
	                          organization_id, seen, journals, error))
		return NULL;

	for (i = 0; i < legs->len; i++)
	{
		VentureEntity *leg;

		leg = g_ptr_array_index(legs, i);

		if (!arb_collect_journals(service, "arbitrage_leg", venture_entity_get_id(leg),
		                          organization_id, seen, journals, error) ||
		    !arb_collect_journals(service, "inventory_txn", arb_int(leg, "inventory-txn-id"),
		                          organization_id, seen, journals, error))
			return NULL;
	}

	posted = g_ptr_array_new_with_free_func(g_object_unref);

	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal;
		VentureJournalState state;

		journal = g_ptr_array_index(journals, i);
		g_object_get(journal, "state", &state, NULL);

		/* Posted and reversed journals are both evidence; a reversal is a
		 * posted journal of its own, so the two net to nothing. */
		if (VENTURE_JOURNAL_DRAFT != state)
			g_ptr_array_add(posted, g_object_ref(journal));
	}

	return g_steal_pointer(&posted);
}

/* The positions balances of @journals per journal currency, in book
 * amounts, debits positive. */
static GPtrArray *
arb_position_of(
	VentureDatabase	 *database,
	GPtrArray	 *journals,
	gint64		  positions,
	GError		**error
){
	g_autoptr(GPtrArray) balances = NULL;
	guint i;

	balances = venture_money_totals_new();

	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal;
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		g_autofree gchar *currency = NULL;
		guint j;

		journal = g_ptr_array_index(journals, i);
		g_object_get(journal, "currency", &currency, NULL);

		query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		venture_query_set_limit(query, 0);

		if (!venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ,
		                                  venture_entity_get_id(journal), error) ||
		    !venture_query_add_filter_int(query, "account-id", VENTURE_FILTER_OP_EQ,
		                                  positions, error))
			return NULL;

		lines = venture_database_find(database, query, error);

		if (NULL == lines)
			return NULL;

		for (j = 0; j < lines->len; j++)
		{
			VentureEntity *line;
			g_autoptr(VentureMoney) book = NULL;
			g_autoptr(VentureMoney) signed_amount = NULL;
			VentureLedgerSide side;

			line = g_ptr_array_index(lines, j);
			g_object_get(line, "book-amount", &book, "side", &side, NULL);

			if (NULL == book)
				continue;

			/* The books' figure, not the original: a valued currency
			 * converted into the book currency is closed at what it was
			 * booked at, so the section ends at exactly zero. */
			signed_amount = (VENTURE_LEDGER_SIDE_DEBIT == side)
				? venture_money_copy(book) : venture_money_negate(book);

			if (!venture_money_totals_add(balances, signed_amount, error))
				return NULL;
		}
	}

	return g_steal_pointer(&balances);
}

GPtrArray *
venture_arbitrage_position(
	VentureDatabase	 *database,
	gint64		  trade_id,
	GError		**error
){
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	gint64 positions;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	trade = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE, trade_id, error);

	if (NULL == trade)
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Arbitrage trade #%" G_GINT64_FORMAT " does not exist", trade_id);
		return NULL;
	}

	/* Looked up, never made: reading a position is not a reason to add
	 * an account to the chart. No account means nothing was ever posted
	 * to one, so the position is empty. */
	if (!venture_arbitrage_find_account(database, venture_entity_get_organization_id(trade),
	                                    "arbitrage_positions", NULL, &positions, error))
		return NULL;

	if (positions <= 0)
		return venture_money_totals_new();

	legs = venture_arbitrage_trade_legs(database, trade_id, TRUE, error);

	if (NULL == legs)
		return NULL;

	journals = arb_trade_journals(database, trade, legs, error);

	if (NULL == journals)
		return NULL;

	return arb_position_of(database, journals, positions, error);
}

/* ==========================================================================
 * Executing a leg
 * ========================================================================== */

/*
 * What an issue cost, as a leg records it: *@out_cost in @currency when
 * the units cost something there (else the first currency they cost, else
 * a zero in @currency), and *@out_detail every currency's part as JSON
 * {CODE: amount} -- NULL when nothing was costed. Units made from inputs
 * in two currencies cost both, and the figures read the detail: one cost
 * would drop a currency the positions account and the close carry.
 */
static gboolean
arb_cost_parts(
	GPtrArray	 *costs,
	const gchar	 *currency,
	VentureMoney	**out_cost,
	gchar		**out_detail,
	GError		**error
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) root = NULL;
	const VentureMoney *found;
	const VentureMoney *first;
	guint parts;
	guint i;

	(void)error;

	*out_cost = NULL;
	*out_detail = NULL;
	builder = json_builder_new();
	json_builder_begin_object(builder);
	first = NULL;
	parts = 0;

	for (i = 0; (NULL != costs) && (i < costs->len); i++)
	{
		const VentureMoney *part = g_ptr_array_index(costs, i);
		g_autofree gchar *text = NULL;
		gchar *space;

		if (venture_money_is_zero(part))
			continue;

		if (NULL == first)
			first = part;

		/* "12.3400 GOLD" -> "12.3400": the code is the member's name. */
		text = venture_money_to_string(part);
		space = strrchr(text, ' ');

		if (NULL != space)
			*space = '\0';

		json_builder_set_member_name(builder, venture_money_get_currency(part));
		json_builder_add_string_value(builder, text);
		parts++;
	}

	json_builder_end_object(builder);

	if (parts > 0)
	{
		root = json_builder_get_root(builder);
		*out_detail = json_to_string(root, FALSE);
	}

	found = (NULL != currency) ? venture_money_totals_lookup(costs, currency) : NULL;

	if ((NULL != found) && !venture_money_is_zero(found))
		*out_cost = venture_money_copy(found);
	else if (NULL != first)
		*out_cost = venture_money_copy(first);
	else if (NULL != found)
		*out_cost = venture_money_copy(found);
	else if ((NULL != costs) && (costs->len > 0))
		*out_cost = venture_money_copy(g_ptr_array_index(costs, 0));
	else if (NULL != currency)
		*out_cost = venture_money_new_zero(currency);

	return TRUE;
}

/* Executes @leg_id inside the caller's transaction. */
static gboolean
arb_execute_in_transaction(
	VentureDatabase		 *database,
	gint64			  leg_id,
	GDateTime		 *occurred_at,
	const VentureActor	 *actor,
	VentureEntity		**out_leg,
	GError			**error
){
	g_autoptr(VentureEntity) leg = NULL;
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *reference = NULL;
	VentureArbitrageLegKind kind;
	gint status;
	gint64 organization_id;
	gint64 item_id;
	gint64 quantity;

	leg = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_LEG, leg_id, error);

	if (NULL == leg)
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Arbitrage leg #%" G_GINT64_FORMAT " does not exist", leg_id);
		return FALSE;
	}

	if (venture_entity_is_deleted(leg))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Arbitrage leg #%" G_GINT64_FORMAT " was deleted", leg_id);
		return FALSE;
	}

	status = arb_enum(leg, "status");

	if (VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED == status)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "Arbitrage leg #%" G_GINT64_FORMAT " is executed already", leg_id);
		return FALSE;
	}

	if (VENTURE_ARBITRAGE_LEG_STATUS_CANCELLED == status)
	{
		venture_set_error_validation(error, "Status",
			"the leg was cancelled; record a new one for what happened");
		return FALSE;
	}

	trade = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE,
	                             arb_int(leg, "trade-id"), NULL);

	if ((NULL == trade) || venture_entity_is_deleted(trade))
	{
		venture_set_error_validation(error, "Trade",
			"#%" G_GINT64_FORMAT " is gone; a leg executes inside its trade",
			arb_int(leg, "trade-id"));
		return FALSE;
	}

	if (!arb_check_frozen(database, venture_entity_get_id(trade), TRUE, error))
		return FALSE;

	organization_id = venture_entity_get_organization_id(leg);
	kind = (VentureArbitrageLegKind)arb_enum(leg, "kind");
	item_id = arb_int(leg, "inventory-item-id");
	quantity = arb_int(leg, "quantity");

	if (NULL != occurred_at)
		when = g_date_time_ref(occurred_at);
	else
		when = arb_time(leg, "occurred-at");

	if (NULL == when)
		when = venture_time_now();

	/* --- The stock side, once --- */

	if (item_id > 0)
	{
		VentureInventoryService *inventory;
		g_autoptr(VentureEntity) txn = NULL;
		g_autoptr(VentureMoney) cost = NULL;
		g_autofree gchar *detail = NULL;

		if (!arb_sales_enabled())
		{
			venture_set_error_validation(error, "Stock",
				"belongs to the sales module, which is off");
			return FALSE;
		}

		amount = arb_leg_amount(leg, error);

		if (NULL == amount)
		{
			if ((NULL != error) && (NULL == *error))
				venture_set_error_validation(error, "Amount",
					"a leg that moves stock needs what it was bought or sold for");
			return FALSE;
		}

		if (quantity < 1)
		{
			venture_set_error_validation(error, "Quantity",
				"must be at least 1 when the leg moves stock");
			return FALSE;
		}

		inventory = venture_inventory_service_get(database);
		/* The leg, not the trade: every movement a leg made is found by
		 * "arbitrage_leg:12". */
		reference = g_strdup_printf("arbitrage_leg:%" G_GINT64_FORMAT, leg_id);

		if (VENTURE_ARBITRAGE_LEG_KIND_BUY == kind)
		{
			gint64 cash;

			if (!arb_leg_cash_account(database, organization_id, leg, when, actor, &cash, error))
				return FALSE;

			/* Dr inventory, Cr the venue's cash, at what was paid; the
			 * leg's own save then posts only its fees. */
			if (!venture_inventory_service_receive_from(inventory, item_id, quantity, amount,
			                                            when, reference, cash, actor, &txn,
			                                            error))
				return FALSE;

			cost = venture_money_copy(amount);
		}
		else if (VENTURE_ARBITRAGE_LEG_KIND_SELL == kind)
		{
			g_autoptr(GPtrArray) costs = NULL;
			gint64 positions;

			positions = venture_arbitrage_account(database, organization_id,
			                                      "arbitrage_positions", when, actor, error);

			if (positions <= 0)
				return FALSE;

			/* Dr positions, Cr inventory, at first-in-first-out cost; the
			 * leg's own save then brings the proceeds out of the position,
			 * so what stays there is the profit. */
			if (!venture_inventory_service_issue_to(inventory, item_id, quantity, when,
			                                        reference, positions,
			                                        VENTURE_INVENTORY_TXN_KIND_SALE, actor,
			                                        &txn, &costs, error))
				return FALSE;

			if (!arb_cost_parts(costs, venture_money_get_currency(amount), &cost, &detail,
			                    error))
				return FALSE;
		}
		else
		{
			venture_set_error_validation(error, "Stock",
				"only a buy or a sell moves stock");
			return FALSE;
		}

		g_object_set(leg, "inventory-txn-id", venture_entity_get_id(txn), "cost", cost,
		             "cost-detail", detail, NULL);
	}

	/* --- The leg, which posts its money and fees on this save --- */

	g_object_set(leg, "status", (gint)VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED,
	             "occurred-at", when, NULL);

	if (!arb_save_permitted(database, leg, actor, error))
		return FALSE;

	/* A planned trade has started. Opened at the leg that started it,
	 * which is what the hold time is measured from. */
	if (VENTURE_ARBITRAGE_TRADE_STATUS_PLANNED == arb_enum(trade, "status"))
	{
		g_object_set(trade, "status", (gint)VENTURE_ARBITRAGE_TRADE_STATUS_OPEN,
		             "opened-at", when, NULL);

		if (!venture_database_save(database, trade, actor, error))
			return FALSE;
	}

	if (NULL != out_leg)
		*out_leg = g_steal_pointer(&leg);

	return TRUE;
}

gboolean
venture_arbitrage_execute_leg(
	VentureDatabase		 *database,
	gint64			  leg_id,
	GDateTime		 *occurred_at,
	const VentureActor	 *actor,
	VentureEntity		**out_leg,
	GError			**error
){
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) leg = NULL;
	g_autofree gchar *date = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	if (NULL != out_leg)
		*out_leg = NULL;

	leg = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_LEG, leg_id, error);

	if (NULL == leg)
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Arbitrage leg #%" G_GINT64_FORMAT " does not exist", leg_id);
		return FALSE;
	}

	date = (NULL != occurred_at) ? g_date_time_format_iso8601(occurred_at) : NULL;

	/* Consent first, then the transaction: the boundary refuses to begin
	 * inside a business transaction it did not open. */
	operation = venture_accounting_operation_begin(database, "arbitrage.execute", leg, NULL,
		g_variant_new("(xs)", leg_id, (NULL != date) ? date : ""),
		venture_entity_get_organization_id(leg), actor, error);

	if (NULL == operation)
		return FALSE;

	if (!venture_database_begin(database, error))
		return FALSE;

	g_clear_object(&leg);

	if (!arb_execute_in_transaction(database, leg_id, occurred_at, actor, &leg, error))
	{
		venture_database_rollback(database);
		return FALSE;
	}

	if (!venture_database_commit(database, error) ||
	    !venture_accounting_operation_finish(operation, error))
		return FALSE;

	if (NULL != out_leg)
		*out_leg = g_steal_pointer(&leg);

	return TRUE;
}

/* ==========================================================================
 * Closing, reopening, abandoning
 * ========================================================================== */

/* How many closing journals the trade has ever posted, so a new one gets
 * a posting key no earlier close used. */
static gboolean
arb_close_count(
	VentureDatabase	 *database,
	VentureEntity	 *trade,
	guint		 *out_count,
	GError		**error
){
	g_autoptr(GPtrArray) journals = NULL;
	guint i;

	*out_count = 0;
	journals = venture_posting_service_find_source(venture_database_get_posting_service(database),
	                                               "arbitrage_trade", venture_entity_get_id(trade),
	                                               venture_entity_get_organization_id(trade),
	                                               error);

	if (NULL == journals)
		return FALSE;

	for (i = 0; i < journals->len; i++)
	{
		g_autofree gchar *rule = NULL;
		gint64 reverses;

		g_object_get(g_ptr_array_index(journals, i), "rule-name", &rule,
		             "reverses-id", &reverses, NULL);

		if ((0 == g_strcmp0(rule, VENTURE_ARBITRAGE_CLOSE_RULE)) && (0 == reverses))
			(*out_count)++;
	}

	return TRUE;
}

/* The latest and the earliest time an executed leg of @legs happened. */
static void
arb_leg_span(
	GPtrArray	 *legs,
	GDateTime	**out_first,
	GDateTime	**out_last
){
	guint i;

	*out_first = NULL;
	*out_last = NULL;

	for (i = 0; i < legs->len; i++)
	{
		VentureEntity *leg;
		g_autoptr(GDateTime) when = NULL;

		leg = g_ptr_array_index(legs, i);

		if (VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED != arb_enum(leg, "status"))
			continue;

		when = arb_time(leg, "occurred-at");

		if (NULL == when)
			continue;

		if ((NULL == *out_first) || (g_date_time_compare(when, *out_first) < 0))
		{
			g_clear_pointer(out_first, g_date_time_unref);
			*out_first = g_date_time_ref(when);
		}

		if ((NULL == *out_last) || (g_date_time_compare(when, *out_last) > 0))
		{
			g_clear_pointer(out_last, g_date_time_unref);
			*out_last = g_date_time_ref(when);
		}
	}
}

/*
 * Closes @trade (re-read inside the caller's transaction) with @status:
 * one journal per book section that has a balance, positions against
 * gains, in that section's own currency. Each journal is posted with the
 * core post because its lines are all in its header's currency: nothing
 * is converted, and a section that is a separate book is closed in that
 * book, which a routed post could not promise once a rate is recorded
 * after the legs posted.
 */
static gboolean
arb_close_in_transaction(
	VentureDatabase		 *database,
	gint64			  trade_id,
	VentureArbitrageTradeStatus status,
	GDateTime		 *closed_at,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
){
	VenturePostingService *service;
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) balances = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(GDateTime) first = NULL;
	g_autoptr(GDateTime) last = NULL;
	g_autoptr(GDateTime) opened = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *memo = NULL;
	gint64 organization_id;
	gint64 positions;
	gint64 gains;
	gint64 first_journal;
	guint closes;
	guint i;

	service = venture_database_get_posting_service(database);
	trade = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE, trade_id, error);

	if (NULL == trade)
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Arbitrage trade #%" G_GINT64_FORMAT " does not exist", trade_id);
		return FALSE;
	}

	if (venture_entity_is_deleted(trade))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Arbitrage trade #%" G_GINT64_FORMAT " was deleted", trade_id);
		return FALSE;
	}

	if (arb_trade_is_over(arb_enum(trade, "status")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "Arbitrage trade #%" G_GINT64_FORMAT " is %s already; reopen it first",
		            trade_id, venture_enum_to_nick(VENTURE_TYPE_ARBITRAGE_TRADE_STATUS,
		                                           arb_enum(trade, "status")));
		return FALSE;
	}

	organization_id = venture_entity_get_organization_id(trade);
	legs = venture_arbitrage_trade_legs(database, trade_id, TRUE, error);

	if (NULL == legs)
		return FALSE;

	arb_leg_span(legs, &first, &last);
	when = (NULL != closed_at) ? g_date_time_ref(closed_at) : venture_time_now();

	/* A trade closes after its last leg; the position it closes is the
	 * one its legs left, and a closing journal dated before one of them
	 * would close a balance that did not exist yet. */
	if ((NULL != last) && (g_date_time_compare(when, last) < 0))
	{
		g_autofree gchar *text = NULL;

		text = venture_time_to_string(last);
		venture_set_error_validation(error, "closed_at",
			"is before the trade's last executed leg (%s); a trade closes after "
			"it", text);
		return FALSE;
	}

	positions = venture_arbitrage_account(database, organization_id, "arbitrage_positions",
	                                      when, actor, error);

	if (positions <= 0)
		return FALSE;

	gains = venture_arbitrage_account(database, organization_id, "arbitrage_gains", when,
	                                  actor, error);

	if (gains <= 0)
		return FALSE;

	journals = arb_trade_journals(database, trade, legs, error);

	if (NULL == journals)
		return FALSE;

	balances = arb_position_of(database, journals, positions, error);

	if (NULL == balances)
		return FALSE;

	if (!arb_close_count(database, trade, &closes, error))
		return FALSE;

	name = venture_entity_get_display_name(trade);
	memo = g_strdup_printf("%s: %s", (VENTURE_ARBITRAGE_TRADE_STATUS_ABANDONED == status)
	                       ? "Abandoned" : "Closed", name);
	venture_money_totals_sort(balances, NULL);
	first_journal = 0;

	for (i = 0; i < balances->len; i++)
	{
		const VentureMoney *balance;
		g_autoptr(VentureJournal) header = NULL;
		g_autoptr(VentureJournal) posted = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		g_autofree gchar *key = NULL;
		const gchar *currency;

		balance = g_ptr_array_index(balances, i);
		currency = venture_money_get_currency(balance);

		if (venture_money_is_zero(balance))
			continue;

		/* A section left in a currency since made memo has no journal that
		 * can hold it; say so rather than leave it behind in silence. */
		if (VENTURE_BOOK_TREATMENT_MEMO == venture_currency_get_book_treatment(currency))
		{
			g_autofree gchar *text = NULL;

			text = venture_money_to_string(balance);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "The trade's positions hold %s, and %s has become a memo "
			            "currency since, which no journal can post; correct it with a "
			            "manual journal before closing", text, currency);
			return FALSE;
		}

		key = g_strdup_printf("arbitrage_close:%" G_GINT64_FORMAT ":%u:%s", trade_id,
		                      closes, currency);

		header = venture_journal_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(header), organization_id);
		g_object_set(header, "occurred-at", when, "source-type", "arbitrage_trade",
		             "source-id", trade_id, "rule-name", VENTURE_ARBITRAGE_CLOSE_RULE,
		             "memo", memo, "posting-key", key, "currency", currency, NULL);

		lines = g_ptr_array_new_with_free_func(g_object_unref);

		/* A debit balance is a loss: debit gains, credit positions. A credit
		 * balance is a profit: debit positions, credit gains. arb_pair
		 * turns the sides for the sign. */
		arb_pair(lines, organization_id, gains, positions, balance);

		posted = venture_posting_service_post(service, header, lines, NULL, actor, error);

		if (NULL == posted)
			return FALSE;

		if (0 == first_journal)
			first_journal = venture_entity_get_id(VENTURE_ENTITY(posted));
	}

	opened = arb_time(trade, "opened-at");

	if (NULL == opened)
		opened = (NULL != first) ? g_date_time_ref(first) : g_date_time_ref(when);

	g_object_set(trade, "status", (gint)status, "closed-at", when, "opened-at", opened,
	             "close-journal-id", first_journal, NULL);

	if (!arb_save_permitted(database, trade, actor, error))
		return FALSE;

	if (NULL != out_trade)
		*out_trade = g_steal_pointer(&trade);

	return TRUE;
}

/* Begins the posting boundary and a transaction for @name on @trade_id. */
static VentureAccountingOperation *
arb_begin(
	VentureDatabase		 *database,
	const gchar		 *name,
	gint64			  trade_id,
	GVariant		 *arguments,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(GVariant) owned = NULL;
	VentureAccountingOperation *operation;

	owned = (NULL != arguments) ? g_variant_ref_sink(arguments) : NULL;
	trade = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE, trade_id, error);

	if (NULL == trade)
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Arbitrage trade #%" G_GINT64_FORMAT " does not exist", trade_id);
		return NULL;
	}

	/* Handed over owned (not floating): the operation sinks it, which for
	 * an owned value is one more reference that it drops again, and ours
	 * is dropped by the autoptr. */
	operation = venture_accounting_operation_begin(database, name, trade, NULL, owned,
		venture_entity_get_organization_id(trade), actor, error);

	if (NULL == operation)
		return NULL;

	if (!venture_database_begin(database, error))
	{
		venture_accounting_operation_free(operation);
		return NULL;
	}

	return operation;
}

gboolean
venture_arbitrage_close(
	VentureDatabase		 *database,
	gint64			  trade_id,
	GDateTime		 *closed_at,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
){
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) trade = NULL;
	g_autofree gchar *date = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	if (NULL != out_trade)
		*out_trade = NULL;

	date = (NULL != closed_at) ? g_date_time_format_iso8601(closed_at) : NULL;
	operation = arb_begin(database, "arbitrage.close", trade_id,
	                      g_variant_new("(s)", (NULL != date) ? date : ""), actor, error);

	if (NULL == operation)
		return FALSE;

	if (!arb_close_in_transaction(database, trade_id, VENTURE_ARBITRAGE_TRADE_STATUS_CLOSED,
	                              closed_at, actor, &trade, error))
	{
		venture_database_rollback(database);
		return FALSE;
	}

	if (!venture_database_commit(database, error) ||
	    !venture_accounting_operation_finish(operation, error))
		return FALSE;

	if (NULL != out_trade)
		*out_trade = g_steal_pointer(&trade);

	return TRUE;
}

static gboolean
arb_reopen_in_transaction(
	VentureDatabase		 *database,
	gint64			  trade_id,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
){
	VenturePostingService *service;
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GDateTime) now = NULL;
	guint i;

	service = venture_database_get_posting_service(database);
	trade = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE, trade_id, error);

	if (NULL == trade)
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Arbitrage trade #%" G_GINT64_FORMAT " does not exist", trade_id);
		return FALSE;
	}

	if (!arb_trade_is_over(arb_enum(trade, "status")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "Arbitrage trade #%" G_GINT64_FORMAT " is not closed or abandoned",
		            trade_id);
		return FALSE;
	}

	journals = venture_posting_service_find_source(service, "arbitrage_trade", trade_id,
	                                               venture_entity_get_organization_id(trade),
	                                               error);

	if (NULL == journals)
		return FALSE;

	now = venture_time_now();

	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal;
		g_autofree gchar *rule = NULL;
		g_autoptr(GDateTime) posted_at = NULL;
		g_autoptr(VentureJournal) reversal = NULL;
		VentureJournalState state;

		journal = g_ptr_array_index(journals, i);
		g_object_get(journal, "state", &state, "rule-name", &rule, "occurred-at", &posted_at,
		             NULL);

		if ((VENTURE_JOURNAL_POSTED != state) ||
		    (0 != g_strcmp0(rule, VENTURE_ARBITRAGE_CLOSE_RULE)))
			continue;

		/* As a correction: today, or the close's own date when that is
		 * later -- a reversal never precedes what it reverses. */
		reversal = venture_posting_service_reverse(service, venture_entity_get_id(journal),
			(g_date_time_compare(now, posted_at) < 0) ? posted_at : now,
			"Trade reopened", actor, error);

		if (NULL == reversal)
			return FALSE;
	}

	g_object_set(trade, "status", (gint)VENTURE_ARBITRAGE_TRADE_STATUS_OPEN,
	             "closed-at", NULL, "close-journal-id", (gint64)0, NULL);

	if (!arb_save_permitted(database, trade, actor, error))
		return FALSE;

	if (NULL != out_trade)
		*out_trade = g_steal_pointer(&trade);

	return TRUE;
}

gboolean
venture_arbitrage_reopen(
	VentureDatabase		 *database,
	gint64			  trade_id,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
){
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) trade = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	if (NULL != out_trade)
		*out_trade = NULL;

	operation = arb_begin(database, "arbitrage.reopen", trade_id, NULL, actor, error);

	if (NULL == operation)
		return FALSE;

	if (!arb_reopen_in_transaction(database, trade_id, actor, &trade, error))
	{
		venture_database_rollback(database);
		return FALSE;
	}

	if (!venture_database_commit(database, error) ||
	    !venture_accounting_operation_finish(operation, error))
		return FALSE;

	if (NULL != out_trade)
		*out_trade = g_steal_pointer(&trade);

	return TRUE;
}

/* One stock a trade bought into, and how many of those units it still
 * owes the shelf. */
typedef struct
{
	gint64	item_id;
	gint64	units;
} ArbHeld;

/*
 * Writes off what the trade bought and did not sell: per stock, units in
 * by its executed buys less units out by its sells and earlier write-offs,
 * never more than are on hand. Each is a write_off leg issuing the units
 * at first-in-first-out cost into the positions account, so the close
 * that follows takes their cost to gains as a loss.
 */
static gboolean
arb_write_off(
	VentureDatabase		 *database,
	VentureEntity		 *trade,
	GDateTime		 *when,
	const VentureActor	 *actor,
	GError			**error
){
	VentureInventoryService *inventory;
	g_autoptr(GPtrArray) legs = NULL;
	g_autoptr(GArray) held = NULL;
	g_autofree gchar *reference = NULL;
	gint64 organization_id;
	gint64 positions;
	guint i;
	guint j;

	/* Deleted legs too, as close and the figures count them: a deletion
	 * is not a correction, and a deleted sell's units left the shelf all
	 * the same. Leaving it out writes off units that belong to other
	 * stock as this trade's loss. */
	legs = venture_arbitrage_trade_legs(database, venture_entity_get_id(trade), TRUE, error);

	if (NULL == legs)
		return FALSE;

	held = g_array_new(FALSE, TRUE, sizeof(ArbHeld));

	for (i = 0; i < legs->len; i++)
	{
		VentureEntity *leg;
		VentureArbitrageLegKind kind;
		gint64 item_id;
		gint64 units;
		ArbHeld *found;

		leg = g_ptr_array_index(legs, i);
		item_id = arb_int(leg, "inventory-item-id");

		if ((item_id <= 0) || (VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED != arb_enum(leg, "status")))
			continue;

		kind = (VentureArbitrageLegKind)arb_enum(leg, "kind");
		units = arb_int(leg, "quantity");

		if (VENTURE_ARBITRAGE_LEG_KIND_BUY != kind)
			units = -units;

		found = NULL;

		for (j = 0; j < held->len; j++)
			if (g_array_index(held, ArbHeld, j).item_id == item_id)
				found = &g_array_index(held, ArbHeld, j);

		if (NULL == found)
		{
			ArbHeld entry;

			entry.item_id = item_id;
			entry.units = 0;
			g_array_append_val(held, entry);
			found = &g_array_index(held, ArbHeld, held->len - 1);
		}

		found->units += units;
	}

	if (0 == held->len)
		return TRUE;

	if (!arb_sales_enabled())
	{
		venture_set_error_validation(error, "goods",
			"writing stock off needs the sales module, which is off; abandon with "
			"goods=keep");
		return FALSE;
	}

	organization_id = venture_entity_get_organization_id(trade);
	positions = venture_arbitrage_account(database, organization_id, "arbitrage_positions",
	                                      when, actor, error);

	if (positions <= 0)
		return FALSE;

	inventory = venture_inventory_service_get(database);
	reference = g_strdup_printf("arbitrage_trade:%" G_GINT64_FORMAT, venture_entity_get_id(trade));

	for (i = 0; i < held->len; i++)
	{
		ArbHeld *entry;
		g_autoptr(VentureEntity) txn = NULL;
		g_autoptr(GPtrArray) costs = NULL;
		g_autoptr(VentureArbitrageLeg) leg = NULL;
		g_autoptr(VentureMoney) cost = NULL;
		g_autofree gchar *detail = NULL;
		gint64 on_hand;
		gint64 units;

		entry = &g_array_index(held, ArbHeld, i);

		if (entry->units <= 0)
			continue;

		on_hand = venture_inventory_service_on_hand(inventory, entry->item_id, NULL, error);

		if ((NULL != error) && (NULL != *error))
			return FALSE;

		/* Never more than are there: units the trade bought may have left
		 * the shelf some other way since, and those are not its to lose. */
		units = MIN(entry->units, on_hand);

		if (units <= 0)
			continue;

		if (!venture_inventory_service_issue_to(inventory, entry->item_id, units, when, reference,
		                                        positions, VENTURE_INVENTORY_TXN_KIND_WRITE_OFF,
		                                        actor, &txn, &costs, error))
			return FALSE;

		if ((NULL != costs) && (costs->len > 0) &&
		    !arb_cost_parts(costs, NULL, &cost, &detail, error))
			return FALSE;

		leg = venture_arbitrage_leg_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(leg), organization_id);
		g_object_set(leg, "trade-id", venture_entity_get_id(trade),
		             "kind", (gint)VENTURE_ARBITRAGE_LEG_KIND_WRITE_OFF,
		             "status", (gint)VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED,
		             "inventory-item-id", entry->item_id, "quantity", units,
		             "inventory-txn-id", venture_entity_get_id(txn), "cost", cost,
		             "cost-detail", detail,
		             "occurred-at", when, "notes", "Written off when the trade was abandoned",
		             NULL);

		if (!arb_save_permitted(database, VENTURE_ENTITY(leg), actor, error))
			return FALSE;
	}

	return TRUE;
}

gboolean
venture_arbitrage_abandon(
	VentureDatabase		 *database,
	gint64			  trade_id,
	gboolean		  write_off,
	GDateTime		 *when,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
){
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(GDateTime) at = NULL;
	g_autofree gchar *date = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	if (NULL != out_trade)
		*out_trade = NULL;

	at = (NULL != when) ? g_date_time_ref(when) : venture_time_now();
	date = g_date_time_format_iso8601(at);
	operation = arb_begin(database, "arbitrage.abandon", trade_id,
	                      g_variant_new("(bs)", write_off, date), actor, error);

	if (NULL == operation)
		return FALSE;

	trade = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE, trade_id, error);

	if ((NULL == trade) || arb_trade_is_over(arb_enum(trade, "status")) ||
	    venture_entity_is_deleted(trade))
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			            "Arbitrage trade #%" G_GINT64_FORMAT " is not planned or open",
			            trade_id);
		venture_database_rollback(database);
		return FALSE;
	}

	if (write_off && !arb_write_off(database, trade, at, actor, error))
	{
		venture_database_rollback(database);
		return FALSE;
	}

	g_clear_object(&trade);

	if (!arb_close_in_transaction(database, trade_id, VENTURE_ARBITRAGE_TRADE_STATUS_ABANDONED,
	                              at, actor, &trade, error))
	{
		venture_database_rollback(database);
		return FALSE;
	}

	if (!venture_database_commit(database, error) ||
	    !venture_accounting_operation_finish(operation, error))
		return FALSE;

	if (NULL != out_trade)
		*out_trade = g_steal_pointer(&trade);

	return TRUE;
}

/* ==========================================================================
 * Recording an attempt
 * ========================================================================== */

/* The leg members a request may carry, in their wire spelling. Stamps,
 * ids and the trade are the record action's to set. */
static const gchar *const arb_leg_members[] = {
	"kind", "status", "venue_id", "location_id", "instrument_id", "inventory_item_id",
	"quantity", "unit_price", "amount", "fees", "occurred_at", "notes", NULL
};

static gboolean
arb_record_in_transaction(
	VentureDatabase		 *database,
	gint64			  organization_id,
	JsonObject		 *request,
	gint64			  data_source_id,
	const gchar		 *external_ref,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
){
	g_autoptr(VentureArbitrageTrade) trade = NULL;
	g_autoptr(GArray) execute = NULL;
	g_autoptr(VentureEntity) saved = NULL;
	g_autofree gchar *expected = NULL;
	JsonNode *legs_node;
	JsonNode *node;
	JsonArray *legs;
	const gchar *name;
	guint i;

	name = venture_json_object_get_string(request, "name", NULL);

	if (venture_string_is_empty(name))
	{
		venture_set_error_validation(error, "name", "is required");
		return FALSE;
	}

	legs_node = json_object_has_member(request, "legs")
		? json_object_get_member(request, "legs") : NULL;

	if ((NULL == legs_node) || !JSON_NODE_HOLDS_ARRAY(legs_node))
	{
		venture_set_error_validation(error, "legs", "must be an array of legs");
		return FALSE;
	}

	legs = json_node_get_array(legs_node);

	if ((0 == json_array_get_length(legs)) ||
	    (json_array_get_length(legs) > VENTURE_ARBITRAGE_MAX_LEGS))
	{
		venture_set_error_validation(error, "legs", "must hold 1 to %d legs",
		                             VENTURE_ARBITRAGE_MAX_LEGS);
		return FALSE;
	}

	/* The snapshot is kept as text: what the scan said, verbatim. */
	node = json_object_has_member(request, "expected")
		? json_object_get_member(request, "expected") : NULL;

	if ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node))
		expected = venture_json_to_string(node, FALSE);
	else if ((NULL != node) && JSON_NODE_HOLDS_VALUE(node) &&
	         (G_TYPE_STRING == json_node_get_value_type(node)))
		expected = g_strdup(json_node_get_string(node));
	else if ((NULL != node) && !JSON_NODE_HOLDS_NULL(node))
	{
		venture_set_error_validation(error, "expected", "must be a JSON object");
		return FALSE;
	}

	trade = venture_arbitrage_trade_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(trade), organization_id);
	g_object_set(trade, "name", name,
	             "strategy", venture_json_object_get_string(request, "strategy", NULL),
	             "venture-id", venture_json_object_get_int(request, "venture_id", 0),
	             "expected", expected,
	             "notes", venture_json_object_get_string(request, "notes", NULL), NULL);

	/* A flip recorded from an external ledger says where from, which only
	 * this module's permit may write. */
	if (!venture_string_is_empty(external_ref))
	{
		g_object_set(trade, "data-source-id", data_source_id, "external-ref", external_ref, NULL);

		if (!arb_save_permitted(database, VENTURE_ENTITY(trade), actor, error))
			return FALSE;
	}
	else if (!venture_database_save(database, VENTURE_ENTITY(trade), actor, error))
		return FALSE;

	execute = g_array_new(FALSE, FALSE, sizeof(gint64));

	for (i = 0; i < json_array_get_length(legs); i++)
	{
		JsonNode *element;
		g_autoptr(JsonNode) filtered = NULL;
		g_autoptr(VentureArbitrageLeg) leg = NULL;
		g_autoptr(GList) members = NULL;
		JsonObject *object;
		JsonObject *copy;
		GList *member;
		gboolean run;
		gint64 id;

		element = json_array_get_element(legs, i);

		if (!JSON_NODE_HOLDS_OBJECT(element))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "legs[%u] must be an object", i);
			return FALSE;
		}

		object = json_node_get_object(element);
		members = json_object_get_members(object);
		copy = json_object_new();
		filtered = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(filtered, copy);
		run = FALSE;

		for (member = members; NULL != member; member = member->next)
		{
			const gchar *key;

			key = member->data;

			if (!g_strv_contains(arb_leg_members, key))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				            "legs[%u] names %s, which a recorded leg cannot set; a leg "
				            "takes kind, status, venue_id, location_id, instrument_id, "
				            "inventory_item_id, quantity, unit_price, amount, fees, "
				            "occurred_at and notes", i, key);
				return FALSE;
			}

			/* Executed is reached through execute, after the leg exists:
			 * that is what moves its stock and posts it in order. */
			if ((0 == g_strcmp0(key, "status")) &&
			    (0 == g_strcmp0(venture_json_object_get_string(object, "status", NULL),
			                    "executed")))
			{
				run = TRUE;
				continue;
			}

			json_object_set_member(copy, key, json_node_copy(json_object_get_member(object, key)));
		}

		leg = venture_arbitrage_leg_new();

		if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(leg), filtered, error))
		{
			g_prefix_error(error, "legs[%u]: ", i);
			return FALSE;
		}

		venture_entity_set_organization_id(VENTURE_ENTITY(leg), organization_id);
		g_object_set(leg, "trade-id", venture_entity_get_id(VENTURE_ENTITY(trade)), NULL);

		if (!venture_database_save(database, VENTURE_ENTITY(leg), actor, error))
		{
			g_prefix_error(error, "legs[%u]: ", i);
			return FALSE;
		}

		if (run)
		{
			id = venture_entity_get_id(VENTURE_ENTITY(leg));
			g_array_append_val(execute, id);
		}
	}

	/* In the order given: a buy before the sell of the same units. */
	for (i = 0; i < execute->len; i++)
	{
		if (!arb_execute_in_transaction(database, g_array_index(execute, gint64, i), NULL,
		                                actor, NULL, error))
		{
			g_prefix_error(error, "Executing leg #%" G_GINT64_FORMAT ": ",
			               g_array_index(execute, gint64, i));
			return FALSE;
		}
	}

	/* Read back: executing a leg opened it, a save this object has not
	 * seen. */
	saved = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE,
	                             venture_entity_get_id(VENTURE_ENTITY(trade)), error);

	if (NULL == saved)
		return FALSE;

	*out_trade = g_steal_pointer(&saved);

	return TRUE;
}

VentureEntity *
venture_arbitrage_record(
	VentureDatabase		 *database,
	gint64			  organization_id,
	JsonObject		 *request,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *text = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(NULL != request, NULL);

	if (organization_id <= 0)
		organization_id = arb_default_organization(database);

	subject = VENTURE_ENTITY(venture_arbitrage_trade_new());
	venture_entity_set_organization_id(subject, organization_id);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, request);
	text = venture_json_to_string(node, FALSE);

	operation = venture_accounting_operation_begin(database, "arbitrage.record", subject, NULL,
		g_variant_new("(s)", (NULL != text) ? text : ""), organization_id, actor, error);

	if (NULL == operation)
		return NULL;

	if (!venture_database_begin(database, error))
		return NULL;

	if (!arb_record_in_transaction(database, organization_id, request, 0, NULL, actor, &trade, error))
	{
		venture_database_rollback(database);
		return NULL;
	}

	if (!venture_database_commit(database, error) ||
	    !venture_accounting_operation_finish(operation, error))
		return NULL;

	return g_steal_pointer(&trade);
}

/* --- For the books tie-in (venture-arbitrage-books.c), inside its transaction --- */

gboolean
venture_arbitrage_record_in_transaction(
	VentureDatabase		 *database,
	gint64			  organization_id,
	JsonObject		 *request,
	gint64			  data_source_id,
	const gchar		 *external_ref,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
){
	return arb_record_in_transaction(database, organization_id, request, data_source_id,
	                                 external_ref, actor, out_trade, error);
}

gboolean
venture_arbitrage_execute_in_transaction(
	VentureDatabase		 *database,
	gint64			  leg_id,
	GDateTime		 *occurred_at,
	const VentureActor	 *actor,
	VentureEntity		**out_leg,
	GError			**error
){
	return arb_execute_in_transaction(database, leg_id, occurred_at, actor, out_leg, error);
}

gboolean
venture_arbitrage_close_in_transaction(
	VentureDatabase		 *database,
	gint64			  trade_id,
	GDateTime		 *closed_at,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
){
	return arb_close_in_transaction(database, trade_id, VENTURE_ARBITRAGE_TRADE_STATUS_CLOSED,
	                                closed_at, actor, out_trade, error);
}

/* ==========================================================================
 * The trade's summary
 * ========================================================================== */

static void
arb_set_money(
	JsonObject		*object,
	const gchar		*member,
	const VentureMoney	*money
){
	if (NULL == money)
		json_object_set_null_member(object, member);
	else
		json_object_set_member(object, member, venture_money_to_json(money));
}

JsonNode *
venture_arbitrage_trade_summary(
	VentureDatabase	 *database,
	gint64		  trade_id,
	GError		**error
){
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	g_autoptr(GPtrArray) figures = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) position = NULL;
	g_autofree gchar *book = NULL;
	JsonObject *root;
	JsonArray *array;
	JsonNode *node;
	gint64 positions;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	trade = venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE, trade_id, error);

	if (NULL == trade)
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Arbitrage trade #%" G_GINT64_FORMAT " does not exist", trade_id);
		return NULL;
	}

	legs = venture_arbitrage_trade_legs(database, trade_id, TRUE, error);

	if (NULL == legs)
		return NULL;

	figures = venture_arbitrage_compute_figures(legs, error);

	if (NULL == figures)
		return NULL;

	journals = arb_trade_journals(database, trade, legs, error);

	if (NULL == journals)
		return NULL;

	/* Looked up, never made: a page or an API read of a trade must not
	 * add an account to the chart (venture_arbitrage_position()). */
	if (!venture_arbitrage_find_account(database, venture_entity_get_organization_id(trade),
	                                    "arbitrage_positions", NULL, &positions, error))
		return NULL;

	position = (positions > 0) ? arb_position_of(database, journals, positions, error)
	                           : venture_money_totals_new();

	if (NULL == position)
		return NULL;

	book = venture_database_get_book_currency(database, venture_entity_get_organization_id(trade));
	venture_money_totals_sort(position, book);

	root = json_object_new();
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	/* Book currency first, like every operational answer. */
	array = json_array_new();

	{
		const VentureArbitrageFigures *first;

		first = venture_arbitrage_figures_lookup(figures, book);

		for (i = 0; i <= figures->len; i++)
		{
			const VentureArbitrageFigures *row;
			JsonObject *object;

			if (0 == i)
				row = first;
			else
				row = g_ptr_array_index(figures, i - 1);

			if ((NULL == row) || ((0 != i) && (row == first)))
				continue;

			object = json_object_new();
			json_object_set_string_member(object, "currency", row->currency);
			arb_set_money(object, "capital", row->capital);
			arb_set_money(object, "returned", row->returned);
			arb_set_money(object, "stock_bought", row->stock_bought);
			arb_set_money(object, "stock_cost", row->stock_cost);
			arb_set_money(object, "fees", row->fees);
			arb_set_money(object, "realised", row->realised);
			json_array_add_object_element(array, object);
		}
	}

	json_object_set_array_member(root, "figures", array);

	array = json_array_new();

	for (i = 0; i < position->len; i++)
		json_array_add_element(array, venture_money_to_json(g_ptr_array_index(position, i)));

	json_object_set_array_member(root, "position", array);

	array = json_array_new();

	for (i = 0; i < legs->len; i++)
	{
		VentureEntity *leg;
		JsonNode *serialised;

		leg = g_ptr_array_index(legs, i);
		serialised = venture_serializable_to_json(VENTURE_SERIALIZABLE(leg), FALSE);
		json_object_set_boolean_member(json_node_get_object(serialised), "deleted",
		                               venture_entity_is_deleted(leg));
		json_array_add_element(array, serialised);
	}

	json_object_set_array_member(root, "legs", array);

	array = json_array_new();

	for (i = 0; i < journals->len; i++)
		json_array_add_int_element(array, venture_entity_get_id(g_ptr_array_index(journals, i)));

	json_object_set_array_member(root, "journals", array);

	return node;
}

/* ==========================================================================
 * Actions
 * ========================================================================== */

/* A datetime parameter, or NULL; FALSE with @error on one that is not a
 * date. */
static gboolean
arb_param_time(
	GHashTable	 *params,
	const gchar	 *name,
	GDateTime	**out,
	GError		**error
){
	JsonNode *node;

	*out = NULL;
	node = (NULL != params) ? g_hash_table_lookup(params, name) : NULL;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)) ||
	    venture_string_is_empty(json_node_get_string(node)))
		return TRUE;

	*out = venture_time_from_string(json_node_get_string(node), error);

	return NULL != *out;
}

static gboolean
arb_leg_executable(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	gint status;

	(void)action;
	(void)actor;

	status = arb_enum(entity, "status");

	if ((VENTURE_ARBITRAGE_LEG_STATUS_PLANNED != status) &&
	    (VENTURE_ARBITRAGE_LEG_STATUS_FAILED != status))
	{
		venture_set_error_validation(error, "status",
			"only a planned or failed leg can be executed");
		return FALSE;
	}

	return TRUE;
}

static VentureEntity *
arb_execute_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GDateTime) when = NULL;
	VentureEntity *leg;

	leg = NULL;

	if (!arb_param_time(params, "occurred_at", &when, error))
		return NULL;

	if (!venture_arbitrage_execute_leg(venture_action_get_data(action),
	                                   venture_entity_get_id(entity), when, actor, &leg, error))
		return NULL;

	return leg;
}

static gboolean
arb_trade_closable(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	(void)action;
	(void)actor;

	if (arb_trade_is_over(arb_enum(entity, "status")))
	{
		venture_set_error_validation(error, "status",
			"the trade is over already; reopen it first");
		return FALSE;
	}

	return TRUE;
}

static gboolean
arb_trade_reopenable(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	(void)action;
	(void)actor;

	if (!arb_trade_is_over(arb_enum(entity, "status")))
	{
		venture_set_error_validation(error, "status",
			"only a closed or abandoned trade can be reopened");
		return FALSE;
	}

	return TRUE;
}

static VentureEntity *
arb_close_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GDateTime) when = NULL;
	VentureEntity *trade;

	trade = NULL;

	if (!arb_param_time(params, "closed_at", &when, error))
		return NULL;

	if (!venture_arbitrage_close(venture_action_get_data(action), venture_entity_get_id(entity),
	                             when, actor, &trade, error))
		return NULL;

	return trade;
}

static VentureEntity *
arb_reopen_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	VentureEntity *trade;

	(void)params;

	trade = NULL;

	if (!venture_arbitrage_reopen(venture_action_get_data(action), venture_entity_get_id(entity),
	                              actor, &trade, error))
		return NULL;

	return trade;
}

static VentureEntity *
arb_abandon_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GDateTime) when = NULL;
	VentureEntity *trade;
	JsonNode *node;
	const gchar *goods;

	trade = NULL;
	goods = "keep";
	node = g_hash_table_lookup(params, "goods");

	if ((NULL != node) && JSON_NODE_HOLDS_VALUE(node) &&
	    (G_TYPE_STRING == json_node_get_value_type(node)) &&
	    !venture_string_is_empty(json_node_get_string(node)))
		goods = json_node_get_string(node);

	if ((0 != g_strcmp0(goods, "keep")) && (0 != g_strcmp0(goods, "write_off")))
	{
		venture_set_error_validation(error, "goods", "is write_off or keep");
		return NULL;
	}

	if (!arb_param_time(params, "abandoned_at", &when, error))
		return NULL;

	if (!venture_arbitrage_abandon(venture_action_get_data(action), venture_entity_get_id(entity),
	                               0 == g_strcmp0(goods, "write_off"), when, actor, &trade,
	                               error))
		return NULL;

	return trade;
}

static gboolean
arb_always(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	(void)action;
	(void)entity;
	(void)actor;
	(void)error;

	return TRUE;
}

/*
 * The record action. Every parameter arrives typed (the framework checked
 * kinds), and the trade is placed in the organization the placeholder was
 * judged in -- venture_entity_get_organization_id(entity), never a re-read
 * parameter -- so the policy that allowed it is the one it runs under.
 */
static VentureEntity *
arb_record_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(JsonObject) request = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	JsonNode *legs;

	request = json_object_new();
	g_hash_table_iter_init(&iter, params);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		if (0 == g_strcmp0(key, "organization_id"))
			continue;

		json_object_set_member(request, key, json_node_copy(value));
	}

	/* A form posts the legs as text; parse it once, here. */
	legs = json_object_has_member(request, "legs") ? json_object_get_member(request, "legs") : NULL;

	if ((NULL != legs) && JSON_NODE_HOLDS_VALUE(legs) &&
	    (G_TYPE_STRING == json_node_get_value_type(legs)))
	{
		JsonNode *parsed;

		parsed = venture_json_parse(json_node_get_string(legs), error);

		if (NULL == parsed)
			return NULL;

		json_object_set_member(request, "legs", parsed);
	}

	return venture_arbitrage_record(venture_action_get_data(action),
	                                venture_entity_get_organization_id(entity), request, actor,
	                                error);
}

static void
arb_register(
	VentureDatabase		*database,
	const gchar		*type_name,
	const gchar		*name,
	const gchar		*label,
	const gchar		*description,
	GPtrArray		*parameters,
	gboolean		 type_level,
	VentureActionAllowed	 allowed,
	VentureActionInvoke	 invoke
){
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;

	/*
	 * Every one stageable: a staged action holds its parameters, and
	 * approval performs it afresh in one transaction -- recounting the
	 * stock and re-reading the position as they are at approval. Editor
	 * on the global role; the organization role matrix then asks for
	 * finance, because these types belong to a module that requires it.
	 */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", type_name, "name", name, "label", label, "description", description,
		"parameters", parameters, "stageable", TRUE, "type-level", type_level,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);

	if (!venture_action_registry_register(venture_database_get_action_registry(database),
	                                      action, allowed, invoke, database, NULL, &error))
		g_error("Arbitrage action registration: %s", error->message);
}

static void
arb_register_actions(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = NULL;
	VentureFieldSpec *field;

	parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	field = venture_field_spec_new("occurred_at", "When", VENTURE_FIELD_KIND_DATETIME);
	field->help = g_strdup("When it happened; the leg's own time, else now");
	g_ptr_array_add(parameters, field);
	arb_register(database, "arbitrage_leg", "execute", "Execute",
		"Mark this leg done and post it: a leg with stock first receives or issues "
		"the units, at their cost, in the same transaction",
		parameters, FALSE, arb_leg_executable, arb_execute_invoke);

	g_ptr_array_set_size(parameters, 0);
	field = venture_field_spec_new("closed_at", "Closed at", VENTURE_FIELD_KIND_DATETIME);
	field->help = g_strdup("When; now when left empty, never before the last executed leg");
	g_ptr_array_add(parameters, field);
	arb_register(database, "arbitrage_trade", "close", "Close",
		"Move what is left on the positions account to arbitrage gains, one journal "
		"per currency, and mark the trade closed",
		parameters, FALSE, arb_trade_closable, arb_close_invoke);

	g_ptr_array_set_size(parameters, 0);
	arb_register(database, "arbitrage_trade", "reopen", "Reopen",
		"Reverse the closing journals and open the trade again",
		parameters, FALSE, arb_trade_reopenable, arb_reopen_invoke);

	g_ptr_array_set_size(parameters, 0);
	field = venture_field_spec_new("goods", "Stock it bought", VENTURE_FIELD_KIND_ENUM);
	field->help = g_strdup("keep leaves unsold units in stock at their cost; write_off "
	                       "takes them out at cost as a loss");
	field->choices = g_strsplit("keep,write_off", ",", -1);
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("abandoned_at", "Abandoned at", VENTURE_FIELD_KIND_DATETIME);
	field->help = g_strdup("When; now when left empty");
	g_ptr_array_add(parameters, field);
	arb_register(database, "arbitrage_trade", "abandon", "Abandon",
		"Give the trade up: keep or write off the stock it still holds, then close "
		"what is left as a loss or gain",
		parameters, FALSE, arb_trade_closable, arb_abandon_invoke);

	g_ptr_array_set_size(parameters, 0);
	field = venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER);
	field->help = g_strdup("The organization the trade belongs to; needed by a member");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("name", "Name", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("What the attempt is called");
	field->required = TRUE;
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("strategy", "Strategy", VENTURE_FIELD_KIND_STRING);
	field->help = g_strdup("spread, transform, deal, cover, back_lay, or a plugin's");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("venture_id", "Venture", VENTURE_FIELD_KIND_REFERENCE);
	field->help = g_strdup("Optional: the venture it is for");
	field->reference_type = g_strdup("venture");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("expected", "Expected", VENTURE_FIELD_KIND_JSON);
	field->help = g_strdup("What the scan expected, as a JSON object: {\"profit\": "
	                       "[\"12.00 GOLD\"], \"roi\": 0.12, \"data_age_seconds\": 300}");
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("notes", "Notes", VENTURE_FIELD_KIND_TEXT);
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("legs", "Legs", VENTURE_FIELD_KIND_JSON);
	field->help = g_strdup("A JSON array of legs: kind, status, venue_id, instrument_id, "
	                       "inventory_item_id, quantity, unit_price, amount, fees, "
	                       "occurred_at, notes; executed legs are executed in order");
	field->required = TRUE;
	g_ptr_array_add(parameters, field);
	arb_register(database, "arbitrage_trade", "record", "Record attempt",
		"Create a trade and its legs in one transaction, executing the legs marked "
		"executed in the order given",
		parameters, TRUE, arb_always, arb_record_invoke);
}

/* ==========================================================================
 * Installation
 * ========================================================================== */

void
venture_arbitrage_install(VentureContext *context)
{
	VentureDatabase *database;
	g_autoptr(GError) error = NULL;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The engine hands the database this context's registries every
	 * time, so it runs before the once-per-database guard. */
	venture_arbitrage_engine_install(context);

	/* The tests build several contexts over one database; validators,
	 * actions and registrations are per database, so the second one must
	 * add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_ARBITRAGE_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_ARBITRAGE_STATE_KEY, GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_ARBITRAGE_TRADE,
	                                    venture_arbitrage_validate_trade, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_ARBITRAGE_LEG,
	                                    venture_arbitrage_validate_leg, NULL, NULL);
	venture_arbitrage_recipe_lists_install(database);

	/* Posting on save: the rule first, then the type, so a save never
	 * finds the type registered and the rule missing. */
	if (NULL == venture_ledger_lookup_source_type(database, VENTURE_TYPE_ARBITRAGE_LEG))
	{
		venture_posting_rule_registry_add(
			venture_posting_service_get_rules(venture_database_get_posting_service(database)),
			VENTURE_POSTING_RULE(g_object_new(venture_arbitrage_leg_posting_rule_get_type(), NULL)));

		if (!venture_ledger_register_source_type(database, VENTURE_TYPE_ARBITRAGE_LEG,
		                                         VENTURE_ARBITRAGE_LEG_RULE, arb_leg_postable,
		                                         "occurred-at", VENTURE_LEDGER_SOURCE_NONE,
		                                         NULL, NULL, &error))
			g_error("Arbitrage leg source registration: %s", error->message);
	}

	arb_register_actions(database);
}
