/*
 * venture-sessions.c - The sessions module's rules and posting
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

#define VENTURE_SESSIONS_STATE_KEY "venture-sessions-installed"

/*
 * The record `post` is writing right now, if any. Two fields belong to the
 * post and nobody else: a yield's `inventory-txn-id` (which says its units
 * are in stock) and a session's `posted-at`. A writer that could set the
 * first by hand could make a yield look posted with nothing on the shelf,
 * or clear it and post the same units twice. The validators let those
 * fields change only for the object named here -- the pattern the
 * inventory service uses for the rows it owns.
 */
#define VENTURE_SESSIONS_PERMIT_KEY "venture-sessions-posting"

/* ==========================================================================
 * Shared
 * ========================================================================== */

/* "Copper Ore", or "product #7" when it cannot be read. */
static gchar *
sessions_name_of(
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

/*
 * Whether the sales module is on. Products, inventory items and their
 * transactions are its types; a hidden type reads as not registered.
 * Asked of the process-wide registry because the validators and the
 * post have a database, not a context, and the registry is what the
 * module switches mask.
 */
static gboolean
sessions_sales_enabled(void)
{
	return G_TYPE_INVALID != venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "inventory_item");
}

static gboolean
sessions_permitted(
	VentureDatabase	*database,
	VentureEntity	*entity
){
	return g_object_get_data(G_OBJECT(database), VENTURE_SESSIONS_PERMIT_KEY) ==
	       (gpointer)entity;
}

/* Saves @entity as the post, the one writer allowed its stamps. */
static gboolean
sessions_save_permitted(
	VentureDatabase		 *database,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	gpointer previous;
	gboolean saved;

	previous = g_object_get_data(G_OBJECT(database), VENTURE_SESSIONS_PERMIT_KEY);
	g_object_set_data(G_OBJECT(database), VENTURE_SESSIONS_PERMIT_KEY, entity);
	saved = venture_database_save(database, entity, actor, error);
	g_object_set_data(G_OBJECT(database), VENTURE_SESSIONS_PERMIT_KEY, previous);

	return saved;
}

/* An integer property's value on @entity, 0 on a missing @entity. */
static gint64
sessions_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

/* Whether two optional money values differ, NULL being a value. */
static gboolean
sessions_money_differs(
	const VentureMoney	*left,
	const VentureMoney	*right
){
	if ((NULL == left) || (NULL == right))
		return left != right;

	return !venture_money_equal(left, right);
}

/*
 * Refuses a reference into another organization. The generic reference
 * check only asks whether the target exists; a session in one
 * organization filed under another's venture would be counted in books
 * that are not its own. Only a value being written is judged, the rule
 * every reference follows, so a row pointing somewhere since moved stays
 * editable.
 */
static gboolean
sessions_same_organization(
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

	target_id = sessions_int(entity, property);

	if (target_id <= 0)
		return TRUE;

	if ((NULL != previous) && (sessions_int(previous, property) == target_id))
		return TRUE;

	/* A target that does not exist is the reference check's to refuse,
	 * with its own message; this one only judges where it lives. */
	target = venture_database_get(database, target_type, target_id, NULL);

	if ((NULL != target) &&
	    (venture_entity_get_organization_id(target) !=
	     venture_entity_get_organization_id(entity)))
	{
		venture_set_error_validation(error, label,
			"#%" G_GINT64_FORMAT " belongs to another organization",
			target_id);
		return FALSE;
	}

	return TRUE;
}

/* ==========================================================================
 * Sessions
 * ========================================================================== */

/*
 * A session's length, and the stamp only the post may write.
 *
 * `minutes` follows the times, because two sources of one fact disagree
 * sooner or later and the report divides by this one:
 *
 *  - both times set: minutes is the difference, to the nearest minute. A
 *    minutes value somebody wrote that disagrees is refused rather than
 *    overwritten -- silently keeping the times would throw away what they
 *    typed -- unless it is the value already stored, which is what a form
 *    posts back after the times were edited;
 *  - no end yet, and a minutes value written: the end is filled in from
 *    it, the factory's rule for a derived date (a date given is kept, only
 *    an empty one is filled). "I farmed for 90 minutes" is a finished
 *    session;
 *  - no end and nothing written: the session is open and minutes is 0.
 *
 * An end before the start is refused, as is a run longer than
 * VENTURE_SESSIONS_MAX_MINUTES.
 */
static gboolean
venture_sessions_validate_session(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(GDateTime) started = NULL;
	g_autoptr(GDateTime) ended = NULL;
	gint64 minutes;
	gboolean minutes_written;

	(void)user_data;

	g_object_get(entity, "started-at", &started, "ended-at", &ended,
	             "minutes", &minutes, NULL);

	/* --- The post's stamp --- */

	/* Anything else writing posted-at is ignored rather than refused: a
	 * form posts every field back, and a timestamp that went through a
	 * text box need not come back byte for byte. */
	if (!sessions_permitted(database, entity))
	{
		g_autoptr(GDateTime) stamped = NULL;

		if (NULL != previous)
			g_object_get(previous, "posted-at", &stamped, NULL);

		g_object_set(entity, "posted-at", stamped, NULL);
	}

	/* --- The length --- */

	if (NULL == started)
	{
		venture_set_error_validation(error, "Started", "is required");
		return FALSE;
	}

	if (minutes < 0)
	{
		venture_set_error_validation(error, "Minutes",
			"cannot be negative; a session takes some time or none");
		return FALSE;
	}

	minutes_written = (NULL == previous)
		? (0 != minutes)
		: (sessions_int(previous, "minutes") != minutes);

	if (NULL != ended)
	{
		GTimeSpan span;
		gint64 derived;

		span = g_date_time_difference(ended, started);

		if (span < 0)
		{
			venture_set_error_validation(error, "Ended",
				"is before it started; a session ends after it begins");
			return FALSE;
		}

		/* Nearest minute: a 59.9-second session is a minute, not zero. */
		derived = (span + (30 * G_TIME_SPAN_SECOND)) / G_TIME_SPAN_MINUTE;

		if (derived > VENTURE_SESSIONS_MAX_MINUTES)
		{
			venture_set_error_validation(error, "Ended",
				"is more than a year after it started; check the date");
			return FALSE;
		}

		if (minutes_written && (minutes != derived))
		{
			venture_set_error_validation(error, "Minutes",
				"is %" G_GINT64_FORMAT " but the times give %" G_GINT64_FORMAT
				"; change the end instead, or leave minutes empty", minutes,
				derived);
			return FALSE;
		}

		g_object_set(entity, "minutes", derived, NULL);
	}
	else if (minutes_written && (minutes > 0))
	{
		g_autoptr(GDateTime) filled = NULL;

		if (minutes > VENTURE_SESSIONS_MAX_MINUTES)
		{
			venture_set_error_validation(error, "Minutes",
				"is more than a year; check the figure");
			return FALSE;
		}

		filled = g_date_time_add_minutes(started, (gint)minutes);
		g_object_set(entity, "ended-at", filled, NULL);
	}
	else
		g_object_set(entity, "minutes", (gint64)0, NULL);

	/* --- Where it belongs --- */

	return sessions_same_organization(database, entity, previous, "venture-id",
	                                  VENTURE_TYPE_VENTURE, "Venture", error) &&
	       sessions_same_organization(database, entity, previous, "category-id",
	                                  VENTURE_TYPE_CATEGORY, "Category", error) &&
	       sessions_same_organization(database, entity, previous, "location-id",
	                                  VENTURE_TYPE_LOCATION, "Location", error);
}

/* ==========================================================================
 * Yields
 * ========================================================================== */

/*
 * A yield's rules, in the order a person would fix them:
 *
 *  - a session, in the yield's own organization (held always: a yield is
 *    part of its session, not a reference out);
 *  - exactly one form. Goods are a product and a quantity of at least
 *    one; money is a positive amount. Both at once would be counted twice
 *    by the report, and neither is nothing. `unit-value` and a stock item
 *    belong to goods only;
 *  - goods need the sales module, which keeps products. Only a product
 *    being written is judged, so a yield recorded before sales was
 *    switched off stays editable;
 *  - a stock item in the yield's organization that holds the yielded
 *    product -- posting ore into the herb bag would be wrong on both
 *    shelves;
 *  - once posted, the goods are frozen: the product, the quantity, the
 *    stock and the session are what the inventory transaction says, and
 *    changing them would make the yield and the shelf disagree. The value
 *    and the notes stay editable -- they move nothing;
 *  - `inventory-txn-id` is the post's to write, and nobody else's.
 */
static gboolean
venture_sessions_validate_yield(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureEntity) session = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) unit_value = NULL;
	gint64 session_id;
	gint64 product_id;
	gint64 quantity;
	gint64 item_id;
	gint64 txn_id;
	gint64 was_txn;

	(void)user_data;

	g_object_get(entity, "session-id", &session_id, "product-id", &product_id,
	             "quantity", &quantity, "amount", &amount,
	             "unit-value", &unit_value, "inventory-item-id", &item_id,
	             "inventory-txn-id", &txn_id, NULL);
	was_txn = sessions_int(previous, "inventory-txn-id");

	/* --- The post's stamp --- */

	if ((txn_id != was_txn) && !sessions_permitted(database, entity))
	{
		venture_set_error_validation(error, "Stock movement",
			"is set by posting the session, not by hand");
		return FALSE;
	}

	/* --- Frozen once posted --- */

	if (was_txn > 0)
	{
		g_autoptr(VentureMoney) was_amount = NULL;

		g_object_get(previous, "amount", &was_amount, NULL);

		if ((sessions_int(previous, "product-id") != product_id) ||
		    (sessions_int(previous, "quantity") != quantity) ||
		    (sessions_int(previous, "inventory-item-id") != item_id) ||
		    (sessions_int(previous, "session-id") != session_id) ||
		    sessions_money_differs(was_amount, amount))
		{
			venture_set_error_validation(error, "Yield",
				"is posted: its units are in stock as movement #%" G_GINT64_FORMAT
				", so its product, quantity, stock and session cannot change. "
				"Record a correction as a stock adjustment", was_txn);
			return FALSE;
		}
	}

	/* --- The session --- */

	if (session_id <= 0)
	{
		venture_set_error_validation(error, "Session", "is required");
		return FALSE;
	}

	session = venture_database_get(database, VENTURE_TYPE_SESSION, session_id, NULL);

	if (NULL == session)
	{
		venture_set_error_validation(error, "Session",
			"#%" G_GINT64_FORMAT " does not exist", session_id);
		return FALSE;
	}

	if (venture_entity_get_organization_id(session) !=
	    venture_entity_get_organization_id(entity))
	{
		venture_set_error_validation(error, "Session",
			"#%" G_GINT64_FORMAT " belongs to another organization", session_id);
		return FALSE;
	}

	/* --- One form --- */

	if ((product_id <= 0) && (NULL == amount))
	{
		venture_set_error_validation(error, "Yield",
			"needs goods (a product and a quantity) or money (an amount)");
		return FALSE;
	}

	if ((product_id > 0) && (NULL != amount))
	{
		venture_set_error_validation(error, "Yield",
			"is goods or money, not both; record the amount as a second yield");
		return FALSE;
	}

	if (NULL != amount)
	{
		if (venture_money_get_amount(amount) <= 0)
		{
			venture_set_error_validation(error, "Amount",
				"must be more than zero; what a session cost goes in its cost");
			return FALSE;
		}

		if ((0 != quantity) || (NULL != unit_value) || (item_id > 0))
		{
			venture_set_error_validation(error, "Amount",
				"is money: quantity, unit value and stock are for goods");
			return FALSE;
		}

		return TRUE;
	}

	/* --- Goods --- */

	if (!sessions_sales_enabled() &&
	    ((NULL == previous) || (sessions_int(previous, "product-id") != product_id)))
	{
		venture_set_error_validation(error, "Product",
			"goods name a product, and products belong to the sales module, "
			"which is off; record an amount instead, or turn sales on");
		return FALSE;
	}

	if (quantity < 1)
	{
		venture_set_error_validation(error, "Quantity",
			"must be at least 1: a goods yield is some units of a product");
		return FALSE;
	}

	if ((NULL != unit_value) && venture_money_is_negative(unit_value))
	{
		venture_set_error_validation(error, "Unit value",
			"cannot be negative");
		return FALSE;
	}

	if (!sessions_same_organization(database, entity, previous, "product-id",
	                                VENTURE_TYPE_PRODUCT, "Product", error))
		return FALSE;

	/* The stock, judged when it or the product is written. */
	if ((item_id > 0) &&
	    ((NULL == previous) ||
	     (sessions_int(previous, "inventory-item-id") != item_id) ||
	     (sessions_int(previous, "product-id") != product_id)))
	{
		g_autoptr(VentureEntity) item = NULL;

		item = venture_database_get(database, VENTURE_TYPE_INVENTORY_ITEM, item_id, NULL);

		/* Missing is the reference check's refusal. */
		if (NULL != item)
		{
			if (venture_entity_get_organization_id(item) !=
			    venture_entity_get_organization_id(entity))
			{
				venture_set_error_validation(error, "Stock",
					"#%" G_GINT64_FORMAT " belongs to another organization",
					item_id);
				return FALSE;
			}

			if (sessions_int(item, "product-id") != product_id)
			{
				g_autofree gchar *holds = NULL;
				g_autofree gchar *wanted = NULL;

				holds = sessions_name_of(database, VENTURE_TYPE_PRODUCT, "product",
				                         sessions_int(item, "product-id"));
				wanted = sessions_name_of(database, VENTURE_TYPE_PRODUCT, "product",
				                          product_id);
				venture_set_error_validation(error, "Stock",
					"#%" G_GINT64_FORMAT " holds %s, not %s", item_id, holds,
					wanted);
				return FALSE;
			}
		}
	}

	return TRUE;
}

/* ==========================================================================
 * Removal
 * ========================================================================== */

/* Whether any live yield of @session_id has been posted. */
static gboolean
sessions_has_posted_yields(
	VentureDatabase	 *database,
	gint64		  session_id,
	gboolean	 *out_posted,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) posted = NULL;

	query = venture_query_new(VENTURE_TYPE_SESSION_YIELD);
	venture_query_set_limit(query, 1);

	if (!venture_query_add_filter_int(query, "session-id", VENTURE_FILTER_OP_EQ,
	                                  session_id, error) ||
	    !venture_query_add_filter_int(query, "inventory-txn-id", VENTURE_FILTER_OP_GT,
	                                  0, error))
		return FALSE;

	posted = venture_database_find(database, query, error);

	if (NULL == posted)
		return FALSE;

	*out_posted = (posted->len > 0);

	return TRUE;
}

gboolean
venture_sessions_check_write(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	gboolean	  removal,
	GError		**error
){
	g_autoptr(VentureEntity) stored = NULL;

	if (!removal)
		return TRUE;

	if (!VENTURE_IS_SESSION_YIELD(entity) && !VENTURE_IS_SESSION(entity))
		return TRUE;

	/* Judged on the stored row, not on the object handed in: a removal
	 * is of the record, whatever the caller's copy says. */
	stored = venture_database_get(database, G_OBJECT_TYPE(entity),
	                              venture_entity_get_id(entity), NULL);

	if (NULL == stored)
		return TRUE;

	if (VENTURE_IS_SESSION_YIELD(stored))
	{
		gint64 txn_id;

		txn_id = sessions_int(stored, "inventory-txn-id");

		if (txn_id > 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "This yield is posted: its units went into stock as "
			            "movement #%" G_GINT64_FORMAT ", and deleting it would "
			            "leave them there with no record of where they came from. "
			            "Take them out with a stock adjustment instead",
			            txn_id);
			return FALSE;
		}

		return TRUE;
	}

	{
		gboolean posted;

		posted = FALSE;

		if (!sessions_has_posted_yields(database, venture_entity_get_id(stored),
		                                &posted, error))
			return FALSE;

		if (posted)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "This session has yields in stock; deleting it "
			                    "would leave them there with no record of where "
			                    "they came from");
			return FALSE;
		}
	}

	return TRUE;
}

/* ==========================================================================
 * Posting
 * ========================================================================== */

/*
 * The inventory item a goods yield lands in: its own `inventory-item-id`
 * when set -- checked again here, because the item may have been edited
 * since the yield was saved -- else the one item holding the product at
 * the session's location (any location when the session names none).
 */
static gboolean
sessions_yield_stock(
	VentureDatabase	 *database,
	VentureEntity	 *session,
	VentureEntity	 *yield,
	gint64		 *out_item_id,
	GError		**error
){
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *product = NULL;
	gint64 organization_id;
	gint64 product_id;
	gint64 item_id;

	organization_id = venture_entity_get_organization_id(session);
	product_id = sessions_int(yield, "product-id");
	item_id = sessions_int(yield, "inventory-item-id");
	product = sessions_name_of(database, VENTURE_TYPE_PRODUCT, "product", product_id);

	if (item_id > 0)
	{
		g_autoptr(VentureEntity) item = NULL;

		item = venture_database_get(database, VENTURE_TYPE_INVENTORY_ITEM, item_id, NULL);

		if ((NULL == item) || venture_entity_is_deleted(item) ||
		    (venture_entity_get_organization_id(item) != organization_id) ||
		    (sessions_int(item, "product-id") != product_id))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "The yield of %s names stock #%" G_GINT64_FORMAT
			            ", which is gone or no longer holds %s; set the "
			            "yield's Stock again", product, item_id, product);
			return FALSE;
		}

		*out_item_id = item_id;
		return TRUE;
	}

	if (!venture_production_find_stock(database, organization_id, product_id,
	                                   sessions_int(session, "location-id"),
	                                   out_item_id, &local_error))
	{
		/* The same finder the craft uses, and so the same message, with
		 * the two ways a session answers it. */
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "Cannot post the yield of %s: %s. Set the session's location, "
		            "or the yield's Stock, to say where it goes", product,
		            local_error->message);
		return FALSE;
	}

	return TRUE;
}

gboolean
venture_sessions_post(
	VentureDatabase		 *database,
	gint64			  session_id,
	const VentureActor	 *actor,
	guint			 *out_posted,
	VentureEntity		**out_session,
	GError			**error
){
	VentureInventoryService *inventory;
	g_autoptr(VentureEntity) session = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) yields = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *reference = NULL;
	guint posted;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	if (NULL != out_posted)
		*out_posted = 0;

	if (NULL != out_session)
		*out_session = NULL;

	if (!sessions_sales_enabled())
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "Posting puts yields into stock, and stock belongs to "
		                    "the sales module, which is off");
		return FALSE;
	}

	/* One transaction for the reads and the writes: the yields found
	 * unposted are the ones stamped, and a refusal or a failure halfway
	 * leaves nothing behind. Inside the action framework this joins the
	 * transaction it already holds. */
	if (!venture_database_begin(database, error))
		return FALSE;

	session = venture_database_get(database, VENTURE_TYPE_SESSION, session_id, error);

	if (NULL == session)
		goto fail;

	if (venture_entity_is_deleted(session))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Session #%" G_GINT64_FORMAT " was deleted", session_id);
		goto fail;
	}

	query = venture_query_new(VENTURE_TYPE_SESSION_YIELD);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "session-id", VENTURE_FILTER_OP_EQ,
	                                  session_id, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		goto fail;

	yields = venture_database_find(database, query, error);

	if (NULL == yields)
		goto fail;

	/* The units were in hand when the run ended; an open session posted
	 * part-way has them now. */
	g_object_get(session, "ended-at", &when, NULL);

	if (NULL == when)
		when = venture_time_now();

	/* The session, not the yield: every movement a session made is found
	 * by "session:12", which is the question anybody asks of it later. */
	reference = g_strdup_printf("session:%" G_GINT64_FORMAT, session_id);
	inventory = venture_inventory_service_get(database);
	posted = 0;

	for (i = 0; i < yields->len; i++)
	{
		VentureEntity *yield;
		g_autoptr(VentureEntity) txn = NULL;
		gint64 item_id;

		yield = g_ptr_array_index(yields, i);

		/* Money never touches stock; a posted yield is never posted
		 * twice -- that is the whole of the idempotency. */
		if ((sessions_int(yield, "product-id") <= 0) ||
		    (sessions_int(yield, "inventory-txn-id") > 0))
			continue;

		item_id = 0;

		if (!sessions_yield_stock(database, session, yield, &item_id, error))
			goto fail;

		/*
		 * No draws: nothing is consumed, and the output gets a zero
		 * cost layer. A yield was not bought -- its unit value is a
		 * valuation, and carrying it as cost would book the profit
		 * when the herb was picked and none when it sold. The session's
		 * own cost is the run's expense, which the performance report
		 * nets off; spreading it over the units too would count it
		 * twice.
		 */
		if (!venture_inventory_service_produce(inventory, NULL, 0, item_id,
		                                       sessions_int(yield, "quantity"), when,
		                                       reference, actor, &txn, NULL, error))
			goto fail;

		/* Where it landed is kept on the yield, so the page says so and
		 * a later post of the same session cannot pick another shelf. */
		g_object_set(yield, "inventory-txn-id", venture_entity_get_id(txn),
		             "inventory-item-id", item_id, NULL);

		if (!sessions_save_permitted(database, yield, actor, error))
			goto fail;

		posted++;
	}

	if (posted > 0)
	{
		g_autoptr(GDateTime) now = NULL;

		now = venture_time_now();
		g_object_set(session, "posted-at", now, NULL);

		/* A derived write bumps the version: the object returned is
		 * this saved one, not whatever the caller read before. */
		if (!sessions_save_permitted(database, session, actor, error))
			goto fail;
	}

	if (!venture_database_commit(database, error))
		return FALSE;

	if (NULL != out_posted)
		*out_posted = posted;

	if (NULL != out_session)
		*out_session = g_steal_pointer(&session);

	return TRUE;

fail:
	venture_database_rollback(database);
	return FALSE;
}

/* ==========================================================================
 * The post action
 * ========================================================================== */

/*
 * Offered only while the sales module is on, so the page does not show a
 * Post button that can only be refused. Nothing to post is not a reason
 * to hide it: posting is idempotent, and a retried post must succeed.
 * Must not write.
 */
static gboolean
sessions_post_allowed(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	(void)action;
	(void)entity;
	(void)actor;

	if (!sessions_sales_enabled())
	{
		venture_set_error_validation(error, "post",
			"Posting needs the sales module, which keeps stock, and it is off");
		return FALSE;
	}

	return TRUE;
}

static VentureEntity *
sessions_post_invoke(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	VentureEntity *session;

	(void)params;

	session = NULL;

	if (!venture_sessions_post(venture_action_get_data(action),
	                           venture_entity_get_id(entity), actor, NULL,
	                           &session, error))
		return NULL;

	return session;
}

static void
sessions_register_post(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = NULL;
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;

	parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);

	/*
	 * A record action, not a type-level one: the session is the subject,
	 * so the access policy judges the session's own organization and no
	 * organization_id parameter is needed to place it. It takes no
	 * parameters -- where each yield goes is on the records, where the
	 * page shows it.
	 *
	 * Stageable, like craft: a staged action holds only its subject, and
	 * approval performs the whole post afresh in one transaction, posting
	 * whatever is unposted then.
	 */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "session", "name", "post", "label", "Post to stock",
		"description", "Put this session's goods yields into stock, each once, "
		"at no cost, in one transaction; money yields are left as they are",
		"parameters", parameters, "stageable", TRUE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);

	if (!venture_action_registry_register(venture_database_get_action_registry(database),
	                                      action, sessions_post_allowed,
	                                      sessions_post_invoke, database, NULL, &error))
		g_error("Post action registration: %s", error->message);
}

void
venture_sessions_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators and
	 * actions are per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_SESSIONS_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_SESSIONS_STATE_KEY,
	                  GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_SESSION,
	                                    venture_sessions_validate_session,
	                                    NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_SESSION_YIELD,
	                                    venture_sessions_validate_yield,
	                                    NULL, NULL);
	sessions_register_post(database);
}
