/*
 * venture-market.c - The market module's rules and its price lookup
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

#define VENTURE_MARKET_STATE_KEY "venture-market-installed"

/* The listing the position mirror is saving right now, on the database:
 * the one save allowed to write the mirror's own fields. */
#define VENTURE_MARKET_MIRROR_PERMIT_KEY "venture-market-mirror-permit"

gboolean
venture_market_listing_outcome_is_closed(VentureListingOutcome outcome)
{
	return VENTURE_LISTING_OUTCOME_OPEN != outcome;
}

/*
 * Refuses a reference being written to a record in another organization.
 * The generic reference check only asks whether the target exists for
 * the writer, and a person in two organizations can see both: a listing
 * of one organization's product under another's is a row in the wrong
 * books and a sale rate for a product nobody here sells. Read as the
 * writer, as production does; a target they cannot see is the generic
 * check's to refuse.
 */
static gboolean
venture_market_same_organization(
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

	target_id = 0;
	g_object_get(entity, property, &target_id, NULL);

	if (target_id <= 0)
		return TRUE;

	if (NULL != previous)
	{
		gint64 was;

		was = 0;
		g_object_get(previous, property, &was, NULL);

		if (was == target_id)
			return TRUE;
	}

	target = venture_database_get(database, target_type, target_id, NULL);

	if ((NULL != target) &&
	    (venture_entity_get_organization_id(target) !=
	     venture_entity_get_organization_id(entity)))
	{
		venture_set_error_validation(error, label,
			"#%" G_GINT64_FORMAT " belongs to another organization", target_id);
		return FALSE;
	}

	return TRUE;
}

/* ==========================================================================
 * Price observations
 * ========================================================================== */

/*
 * An observation names a product and a price that is not negative. Both
 * are declared NOT_NULL, but the generic check covers only strings and
 * times: an integer reference reads 0 and a money field reads NULL when
 * unset, and either would be an observation of nothing.
 */
static gboolean
venture_market_validate_observation(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureMoney) price = NULL;
	gint64 product_id;
	gint64 volume;

	(void)user_data;

	g_object_get(entity, "product-id", &product_id, "price", &price,
	             "volume", &volume, NULL);

	if (product_id <= 0)
	{
		venture_set_error_validation(error, "Product", "is required");
		return FALSE;
	}

	/* The location is held to the organization with every other
	 * location reference, in venture-category.c. */
	if (!venture_market_same_organization(database, entity, previous, "product-id",
	                                      VENTURE_TYPE_PRODUCT, "Product", error))
		return FALSE;

	if (NULL == price)
	{
		venture_set_error_validation(error, "Price", "is required");
		return FALSE;
	}

	if (venture_money_is_negative(price))
	{
		venture_set_error_validation(error, "Price",
			"cannot be negative: an observation is what one unit fetched");
		return FALSE;
	}

	if (volume < 0)
	{
		venture_set_error_validation(error, "Volume",
			"cannot be negative; leave it 0 when nobody counted");
		return FALSE;
	}

	/*
	 * A report's price_source is read as the series: grammar before it is
	 * matched against observation sources, so an observation filed under
	 * "series:min" could never be asked for. Refused when written; a row
	 * saved before the rule stays editable.
	 */
	{
		g_autofree gchar *source = NULL;
		g_autofree gchar *was = NULL;

		g_object_get(entity, "source", &source, NULL);

		if (NULL != previous)
			g_object_get(previous, "source", &was, NULL);

		if ((NULL != source) && g_str_has_prefix(source, VENTURE_MARKETDATA_SERIES_PREFIX) &&
		    ((NULL == previous) || (0 != g_strcmp0(source, was))))
		{
			venture_set_error_validation(error, "Source",
				"cannot start with \"%s\": a price_source starting so asks the price "
				"oracle for market data, so this observation could never be named",
				VENTURE_MARKETDATA_SERIES_PREFIX);
			return FALSE;
		}
	}

	return TRUE;
}

/* ==========================================================================
 * Listings
 * ========================================================================== */

/*
 * Holds one optional amount to the listing's currency, and refuses a
 * negative one. @currency is the unit price's, which is always set by the
 * time this runs.
 */
static gboolean
venture_market_check_amount(
	VentureEntity	 *entity,
	const gchar	 *property,
	const gchar	 *label,
	const gchar	 *currency,
	GError		**error
){
	g_autoptr(VentureMoney) amount = NULL;

	g_object_get(entity, property, &amount, NULL);

	if (NULL == amount)
		return TRUE;

	/* Refused, not converted: a listing is one offer in one market, and
	 * a deposit in another currency than the price would need a rate
	 * nobody gave -- and would make the deposits-lost total a sum of two
	 * units. */
	if (0 != g_strcmp0(venture_money_get_currency(amount), currency))
	{
		venture_set_error_validation(error, label,
			"is in %s but the unit price is in %s; a listing's amounts "
			"share one currency", venture_money_get_currency(amount), currency);
		return FALSE;
	}

	if (venture_money_is_negative(amount))
	{
		venture_set_error_validation(error, label, "cannot be negative");
		return FALSE;
	}

	return TRUE;
}

/* An integer property's value, 0 on a missing @entity. */
static gint64
venture_market_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

/* Whether @property differs between @entity and @previous (or is set on a
 * new @entity), compared as strings; NULL and "" are one value. */
static gboolean
venture_market_string_changed(
	VentureEntity	*entity,
	VentureEntity	*previous,
	const gchar	*property
){
	g_autofree gchar *now = NULL;
	g_autofree gchar *was = NULL;

	g_object_get(entity, property, &now, NULL);

	if (NULL != previous)
		g_object_get(previous, property, &was, NULL);

	return 0 != g_strcmp0(venture_string_is_empty(now) ? "" : now,
	                      venture_string_is_empty(was) ? "" : was);
}

/*
 * The mirror's fields. A listing filed under a data source is the
 * position mirror's: it made the listing, it compares what it wrote
 * (`mirror-state`) with what is there to tell a person's edit from its
 * own, and it finds the listing again by `external-id`. A person writing
 * any of those would make the mirror fight them or lose the listing, so
 * only the save venture_market_save_mirrored_listing() names may. Judged
 * on a change, so a form posting the stored values back is no write.
 */
static gboolean
venture_market_check_mirror_fields(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	GError		**error
){
	gint64 source_id;
	gint64 was_source_id;

	if (g_object_get_data(G_OBJECT(database), VENTURE_MARKET_MIRROR_PERMIT_KEY) == (gpointer)entity)
		return TRUE;

	source_id = venture_market_int(entity, "data-source-id");
	was_source_id = venture_market_int(previous, "data-source-id");

	if (source_id != was_source_id)
	{
		venture_set_error_validation(error, "Data source",
			"is set by the position mirror alone: a listing filed under a data source "
			"is one the mirror made and keeps in step");
		return FALSE;
	}

	if (venture_market_string_changed(entity, previous, "mirror-state"))
	{
		venture_set_error_validation(error, "Mirror state",
			"is what the position mirror last wrote, and only the mirror writes it");
		return FALSE;
	}

	if ((source_id > 0) && venture_market_string_changed(entity, previous, "external-id"))
	{
		venture_set_error_validation(error, "External id",
			"of a mirrored listing is how the position mirror finds it again; "
			"it cannot be changed");
		return FALSE;
	}

	return TRUE;
}

/*
 * An external id: sane text, and not one another listing of the
 * organization carries -- deleted ones included, as the unique index
 * counts them -- said with the listing's number rather than the index's
 * bare constraint failure.
 */
static gboolean
venture_market_check_external_id(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	GError		**error
){
	g_autofree gchar *external_id = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	const gchar *p;
	guint i;

	if (!venture_market_string_changed(entity, previous, "external-id"))
		return TRUE;

	g_object_get(entity, "external-id", &external_id, NULL);

	if (venture_string_is_empty(external_id))
		return TRUE;

	if ((strlen(external_id) > VENTURE_MARKET_MAX_EXTERNAL_ID) ||
	    !g_utf8_validate(external_id, -1, NULL))
	{
		venture_set_error_validation(error, "External id",
			"must be valid text of at most %d bytes", VENTURE_MARKET_MAX_EXTERNAL_ID);
		return FALSE;
	}

	for (p = external_id; '\0' != *p; p++)
	{
		if (g_ascii_iscntrl(*p))
		{
			venture_set_error_validation(error, "External id",
				"cannot contain control characters");
			return FALSE;
		}
	}

	query = venture_query_new(VENTURE_TYPE_LISTING);
	venture_query_set_organization(query, venture_entity_get_organization_id(entity));
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 2);

	if (!venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ,
	                                     external_id, error))
		return FALSE;

	rows = venture_database_find(database, query, error);

	if (NULL == rows)
		return FALSE;

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *other = g_ptr_array_index(rows, i);

		if (venture_entity_get_id(other) != venture_entity_get_id(entity))
		{
			venture_set_error_validation(error, "External id",
				"listing #%" G_GINT64_FORMAT " already has the external id %s%s",
				venture_entity_get_id(other), external_id,
				venture_entity_is_deleted(other) ? " (it is deleted; restore it instead)" : "");
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * A listing's rules, in the order a person would fix them:
 *
 *  - a product, at least one unit, a unit price that is not negative, and
 *    every amount in the unit price's currency;
 *  - sold units between none and all of them;
 *  - the outcome agrees with the count. Sold means every unit sold, and a
 *    listing marked sold with nothing counted is filled to the quantity
 *    (the common case: one item, sold); a count in between is a
 *    contradiction and refused rather than guessed at. Partial means some
 *    and not all. Expired and cancelled mean none -- an offer that sold
 *    three of ten and then lapsed is partial;
 *  - the closing time follows the outcome, the factory's rule: an ended
 *    listing with no closing time is given now, a given one is kept, a
 *    reopened listing loses it, and an open one may not carry one;
 *  - it cannot close before it opened, and cannot expire before it
 *    opened either;
 *  - a bid is in the price's currency, not negative, and not above an
 *    asking price there is one of (an auction with no buyout asks 0);
 *  - the external id is unique, and the mirror's fields are the mirror's.
 */
static gboolean
venture_market_validate_listing(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureMoney) unit_price = NULL;
	g_autoptr(GDateTime) listed_at = NULL;
	g_autoptr(GDateTime) closed_at = NULL;
	VentureListingOutcome outcome;
	gint64 product_id;
	gint64 quantity;
	gint64 sold;
	const gchar *currency;

	(void)user_data;

	g_object_get(entity, "product-id", &product_id, "quantity", &quantity,
	             "quantity-sold", &sold, "unit-price", &unit_price,
	             "outcome", &outcome, NULL);

	/* --- What is offered, and at what --- */

	if (product_id <= 0)
	{
		venture_set_error_validation(error, "Product", "is required");
		return FALSE;
	}

	if (!venture_market_same_organization(database, entity, previous, "product-id",
	                                      VENTURE_TYPE_PRODUCT, "Product", error) ||
	    !venture_market_same_organization(database, entity, previous, "venue-id",
	                                      VENTURE_TYPE_VENUE, "Venue", error) ||
	    !venture_market_same_organization(database, entity, previous,
	                                      "inventory-item-id",
	                                      VENTURE_TYPE_INVENTORY_ITEM, "Stock", error) ||
	    !venture_market_same_organization(database, entity, previous, "sale-id",
	                                      VENTURE_TYPE_SALE, "Sale", error) ||
	    !venture_market_same_organization(database, entity, previous, "data-source-id",
	                                      VENTURE_TYPE_DATA_SOURCE, "Data source", error))
		return FALSE;

	if (!venture_market_check_mirror_fields(database, entity, previous, error) ||
	    !venture_market_check_external_id(database, entity, previous, error))
		return FALSE;

	if (quantity < 1)
	{
		venture_set_error_validation(error, "Quantity",
			"must be at least 1: a listing offers something");
		return FALSE;
	}

	if (NULL == unit_price)
	{
		venture_set_error_validation(error, "Unit price", "is required");
		return FALSE;
	}

	if (venture_money_is_negative(unit_price))
	{
		venture_set_error_validation(error, "Unit price", "cannot be negative");
		return FALSE;
	}

	currency = venture_money_get_currency(unit_price);

	if (!venture_market_check_amount(entity, "deposit", "Deposit", currency, error) ||
	    !venture_market_check_amount(entity, "fees", "Fees", currency, error) ||
	    !venture_market_check_amount(entity, "bid", "Bid", currency, error))
		return FALSE;

	/* A bid above the buyout is an offer nobody would take; an auction
	 * with no buyout at all asks 0, and any bid goes with it. */
	{
		g_autoptr(VentureMoney) bid = NULL;

		g_object_get(entity, "bid", &bid, NULL);

		if ((NULL != bid) && !venture_money_is_zero(unit_price) &&
		    (venture_money_compare(bid, unit_price) > 0))
		{
			venture_set_error_validation(error, "Bid",
				"is above the unit price; an auction's bid is at most its buyout");
			return FALSE;
		}
	}

	/* --- How many sold, and whether the outcome agrees --- */

	if ((sold < 0) || (sold > quantity))
	{
		venture_set_error_validation(error, "Sold",
			"must be between 0 and the quantity (%" G_GINT64_FORMAT ")",
			quantity);
		return FALSE;
	}

	switch (outcome)
	{
	case VENTURE_LISTING_OUTCOME_SOLD:
		if (0 == sold)
		{
			sold = quantity;
			g_object_set(entity, "quantity-sold", sold, NULL);
		}
		else if (sold != quantity)
		{
			venture_set_error_validation(error, "Outcome",
				"sold means all %" G_GINT64_FORMAT " units sold, but %"
				G_GINT64_FORMAT " are counted; mark it partial, or correct "
				"the count", quantity, sold);
			return FALSE;
		}
		break;
	case VENTURE_LISTING_OUTCOME_PARTIAL:
		if ((0 == sold) || (sold == quantity))
		{
			venture_set_error_validation(error, "Outcome",
				"partial means some but not all units sold; %" G_GINT64_FORMAT
				" of %" G_GINT64_FORMAT " is %s", sold, quantity,
				(0 == sold) ? "expired or cancelled" : "sold");
			return FALSE;
		}
		break;
	case VENTURE_LISTING_OUTCOME_EXPIRED:
	case VENTURE_LISTING_OUTCOME_CANCELLED:
		if (0 != sold)
		{
			venture_set_error_validation(error, "Outcome",
				"%s means nothing sold, but %" G_GINT64_FORMAT " units are "
				"counted; mark it partial",
				venture_enum_to_nick(VENTURE_TYPE_LISTING_OUTCOME, (gint)outcome),
				sold);
			return FALSE;
		}
		break;
	case VENTURE_LISTING_OUTCOME_OPEN:
	default:
		break;
	}

	/* --- When it closed --- */

	if (venture_market_listing_outcome_is_closed(outcome))
	{
		g_object_get(entity, "closed-at", &closed_at, NULL);

		if (NULL == closed_at)
		{
			closed_at = venture_time_now();
			g_object_set(entity, "closed-at", closed_at, NULL);
		}
	}
	else
	{
		VentureListingOutcome was;

		was = VENTURE_LISTING_OUTCOME_OPEN;

		if (NULL != previous)
			g_object_get(previous, "outcome", &was, NULL);

		/* Reopened: an offer that is on sale again has not ended, and
		 * its time to sell runs until it does. Only the transition
		 * clears it, so the rule below still refuses a closing time
		 * typed onto a listing that was open all along. */
		if (venture_market_listing_outcome_is_closed(was))
			g_object_set(entity, "closed-at", NULL, NULL);

		g_object_get(entity, "closed-at", &closed_at, NULL);

		if (NULL != closed_at)
		{
			venture_set_error_validation(error, "Closed",
				"an open listing has not closed; set the outcome it "
				"ended with, or clear the closing time");
			return FALSE;
		}
	}

	g_object_get(entity, "listed-at", &listed_at, NULL);

	if ((NULL != listed_at) && (NULL != closed_at) &&
	    (g_date_time_compare(closed_at, listed_at) < 0))
	{
		venture_set_error_validation(error, "Closed",
			"a listing cannot close before it was listed");
		return FALSE;
	}

	{
		g_autoptr(GDateTime) expires_at = NULL;

		g_object_get(entity, "expires-at", &expires_at, NULL);

		if ((NULL != listed_at) && (NULL != expires_at) &&
		    (g_date_time_compare(expires_at, listed_at) < 0))
		{
			venture_set_error_validation(error, "Expires",
				"a listing cannot run out before it was listed");
			return FALSE;
		}
	}

	return TRUE;
}

gboolean
venture_market_save_mirrored_listing(
	VentureDatabase		 *database,
	VentureEntity		 *listing,
	const VentureActor	 *actor,
	GError			**error
){
	gpointer previous;
	gboolean saved;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_LISTING(listing), FALSE);

	/* The permit names this object for the length of this one save; a
	 * nested save of another listing (an automation handler) is not it. */
	previous = g_object_get_data(G_OBJECT(database), VENTURE_MARKET_MIRROR_PERMIT_KEY);
	g_object_set_data(G_OBJECT(database), VENTURE_MARKET_MIRROR_PERMIT_KEY, listing);
	saved = venture_database_save(database, listing, actor, error);
	g_object_set_data(G_OBJECT(database), VENTURE_MARKET_MIRROR_PERMIT_KEY, previous);

	return saved;
}

void
venture_market_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators are
	 * per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_MARKET_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_MARKET_STATE_KEY,
	                  GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_PRICE_OBSERVATION,
	                                    venture_market_validate_observation,
	                                    NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_LISTING,
	                                    venture_market_validate_listing,
	                                    NULL, NULL);
}

/* ==========================================================================
 * The latest price
 * ========================================================================== */

/* How many observations a currency-filtered lookup reads at a time. */
#define VENTURE_MARKET_PRICE_PAGE (200)

gboolean
venture_market_latest_price(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  product_id,
	const gchar	 *source,
	const gchar	 *currency,
	GDateTime	 *at,
	VentureMoney	**out_price,
	VentureEntity	**out_observation,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	VentureEntity *latest;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	if (NULL != out_price)
		*out_price = NULL;

	if (NULL != out_observation)
		*out_observation = NULL;

	query = venture_query_new(VENTURE_TYPE_PRICE_OBSERVATION);
	venture_query_set_organization(query, organization_id);

	if (!venture_query_add_filter_int(query, "product-id", VENTURE_FILTER_OP_EQ,
	                                  product_id, error))
		return FALSE;

	/* Exact, not a search: "market value" and "market value (region)"
	 * are two sources, and a valuation must not quietly blend them. */
	if (!venture_string_is_empty(source) &&
	    !venture_query_add_filter_string(query, "source", VENTURE_FILTER_OP_EQ,
	                                     source, error))
		return FALSE;

	if (NULL != at)
	{
		g_autofree gchar *cutoff = NULL;

		/* The query emitter pads date-time operands, so a cutoff
		 * with no fraction still compares right against one with. */
		cutoff = venture_time_to_string(at);

		if (!venture_query_add_filter_string(query, "observed-at",
		                                     VENTURE_FILTER_OP_LTE, cutoff,
		                                     error))
			return FALSE;
	}

	if (!venture_query_add_order(query, "observed-at", VENTURE_SORT_DESCENDING, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, error))
		return FALSE;

	/* The query cannot filter on a money field's currency -- a filter on
	 * a money field is on its amount -- so the newest rows are read a
	 * page at a time and the first priced in @currency wins. With no
	 * currency the first row does, which is one row read, as before. A
	 * price seen in TICKET never answers a question asked in GOLD. */
	latest = NULL;

	{
		guint offset;

		for (offset = 0; NULL == latest; offset += VENTURE_MARKET_PRICE_PAGE)
		{
			guint i;

			g_clear_pointer(&rows, g_ptr_array_unref);
			venture_query_set_limit(query, venture_string_is_empty(currency)
			                               ? 1 : VENTURE_MARKET_PRICE_PAGE);
			venture_query_set_offset(query, offset);

			rows = venture_database_find(database, query, error);

			if (NULL == rows)
				return FALSE;

			if (0 == rows->len)
				return TRUE;

			for (i = 0; (i < rows->len) && (NULL == latest); i++)
			{
				g_autoptr(VentureMoney) price = NULL;

				g_object_get(g_ptr_array_index(rows, i), "price", &price, NULL);

				if (venture_string_is_empty(currency) ||
				    ((NULL != price) &&
				     (0 == g_strcmp0(venture_money_get_currency(price), currency))))
					latest = g_ptr_array_index(rows, i);
			}

			if (venture_string_is_empty(currency) ||
			    (rows->len < VENTURE_MARKET_PRICE_PAGE))
				break;
		}
	}

	if (NULL == latest)
		return TRUE;

	if (NULL != out_price)
		g_object_get(latest, "price", out_price, NULL);

	if (NULL != out_observation)
		*out_observation = g_object_ref(latest);

	return TRUE;
}

gboolean
venture_market_price_preferring(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  product_id,
	const gchar	 *source,
	const gchar	 *prefer,
	GDateTime	 *at,
	VentureMoney	**out_price,
	VentureEntity	**out_observation,
	GError		**error
){
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(VentureEntity) observation = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	if (NULL != out_price)
		*out_price = NULL;

	if (NULL != out_observation)
		*out_observation = NULL;

	if (!venture_string_is_empty(prefer) &&
	    !venture_market_latest_price(database, organization_id, product_id, source,
	                                 prefer, at, &price, &observation, error))
		return FALSE;

	/* Seen in the preferred currency: that is the answer, however much
	 * newer an observation in another currency is. */
	if (NULL == observation &&
	    !venture_market_latest_price(database, organization_id, product_id, source,
	                                 NULL, at, &price, &observation, error))
		return FALSE;

	if (NULL != out_price)
		*out_price = g_steal_pointer(&price);

	if (NULL != out_observation)
		*out_observation = g_steal_pointer(&observation);

	return TRUE;
}

gboolean
venture_market_valuing_currency(
	VentureDatabase	 *database,
	gint64		  organization_id,
	JsonObject	 *options,
	gchar		**out_currency,
	gboolean	 *out_strict,
	GError		**error
){
	const gchar *named;
	gchar *book;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(NULL != out_currency, FALSE);
	g_return_val_if_fail(NULL != out_strict, FALSE);

	*out_currency = NULL;
	*out_strict = FALSE;

	named = (NULL != options)
		? venture_json_object_get_string(options, "currency", NULL) : NULL;

	if (!venture_string_is_empty(named))
	{
		g_autofree gchar *code = g_ascii_strup(named, -1);

		if (!venture_currency_is_valid(code))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "currency \"%s\" is not a currency code", named);
			return FALSE;
		}

		*out_currency = g_steal_pointer(&code);
		*out_strict = TRUE;
		return TRUE;
	}

	book = venture_posting_service_book_currency(
		venture_database_get_posting_service(database), organization_id, error);

	if (NULL == book)
		return FALSE;

	*out_currency = book;

	return TRUE;
}
