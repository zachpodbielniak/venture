/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

/*
 * Usage-based pricing. A metered plan price names a unit, a rate per unit
 * and how many units each period includes; usage_record rows report what
 * a subscription used. At renewal the billing service asks this file for
 * the ended period's line, which goes on the same invoice as the base
 * charge for the new period: base in advance, usage in arrears.
 */

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureBillingService: %s", message);
	return FALSE;
}

static gint64
number(VentureEntity *e, const gchar *field)
{
	gint64 value = 0;
	g_object_get(e, field, &value, NULL);
	return value;
}

static VentureEntity *
load(VentureDatabase *database, GType type, gint64 id, gint64 org, GError **error)
{
	VentureEntity *e;
	if (id <= 0)
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "record is absent from this organization");
		return NULL;
	}
	e = venture_database_get(database, type, id, error);
	if (e == NULL)
		return NULL;
	if (venture_entity_get_organization_id(e) != org || venture_entity_is_deleted(e))
	{
		g_object_unref(e);
		refuse(error, VENTURE_ERROR_NOT_FOUND, "record is absent from this organization");
		return NULL;
	}
	return e;
}

gint64
venture_billing_usage_total(VentureDatabase *database, gint64 organization_id,
	gint64 subscription_id, GDateTime *from, GDateTime *until, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USAGE_RECORD);
	g_autoptr(OrmResult) result = NULL;
	g_autofree gchar *from_text = NULL, *until_text = NULL, *count_sql = NULL, *sql = NULL;
	const gchar *rest;
	GList *params = NULL;
	gint64 total = 0;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), -1);
	g_return_val_if_fail(from != NULL && until != NULL, -1);
	from_text = venture_time_to_string(from);
	until_text = venture_time_to_string(until);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "subscription-id", VENTURE_FILTER_OP_EQ, subscription_id, error) ||
		!venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_GTE, from_text, error) ||
		!venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_LT, until_text, error))
		return -1;
	/* The query compiler writes the WHERE -- soft deletion, scoping and the
	 * normalised time comparison -- and the total is spliced over its
	 * COUNT, as venture_database_sum_money() does. Summed in the database
	 * because a busy subscription reports thousands of rows a period. The
	 * cast is what PostgreSQL needs: it widens SUM(bigint) to numeric. */
	count_sql = venture_query_to_sql(query,
		venture_database_get_backend(database) == VENTURE_DATABASE_BACKEND_POSTGRES ? ORM_DIALECT_POSTGRES : ORM_DIALECT_SQLITE,
		TRUE, &params);
	rest = strstr(count_sql, " FROM ");
	if (rest == NULL)
	{
		g_list_free_full(params, (GDestroyNotify)orm_value_free);
		refuse(error, VENTURE_ERROR_FAILED, "usage query could not be built");
		return -1;
	}
	sql = g_strdup_printf("SELECT CAST(COALESCE(SUM(\"quantity\"), 0) AS BIGINT)%s", rest);
	result = venture_database_query_raw(database, sql, params, error);
	g_list_free_full(params, (GDestroyNotify)orm_value_free);
	if (result == NULL)
		return -1;
	if (orm_result_next(result) && !orm_row_is_null(orm_result_get_row(result), 0))
		total = orm_row_get_integer(orm_result_get_row(result), 0);
	return total;
}

/*
 * The price a period's usage is charged at is the one in force when the
 * period ended. A renewal that switches price has already moved the
 * subscription on by the time the invoice is written, so it is read from
 * the last recorded event -- which a scheduled change leaves on the old
 * price -- rather than from the subscription.
 */
static VentureEntity *
usage_price(VentureDatabase *database, VentureEntity *sub, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SUBSCRIPTION_EVENT);
	g_autoptr(VentureEntity) last = NULL;
	g_autoptr(GError) missing = NULL;
	gint64 price_id = number(sub, "plan-price-id");

	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 1);
	if (!venture_query_add_filter_int(query, "subscription-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(sub), error) ||
		!venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, error))
		return NULL;
	last = venture_database_find_one(database, query, &missing);
	if (last != NULL && number(last, "to-plan-price-id") > 0)
		price_id = number(last, "to-plan-price-id");
	return load(database, VENTURE_TYPE_PLAN_PRICE, price_id, org, error);
}

/*
 * The usage line for [@from, @until) on @invoice: the units beyond those
 * included, at @price's rate. Adds nothing when nothing is over, or the
 * charge rounds to nothing; *@added says whether a line was written.
 */
static gboolean
usage_line(VentureDatabase *database, VentureEntity *sub, VentureEntity *price, VentureEntity *invoice,
	GDateTime *from, GDateTime *until, const VentureActor *actor, gboolean *added, GError **error)
{
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureMoney) rate = NULL, charge = NULL, rounded = NULL;
	g_autofree gchar *unit = NULL, *rate_text = NULL, *over_text = NULL, *used_text = NULL;
	g_autofree gchar *included_text = NULL, *from_day = NULL, *to_day = NULL, *description = NULL;
	gint64 org = venture_entity_get_organization_id(sub);
	gint64 used, included, over;

	*added = FALSE;
	used = venture_billing_usage_total(database, org, venture_entity_get_id(sub), from, until, error);
	if (used < 0)
		return FALSE;
	included = number(price, "included-units");
	over = used - included;
	if (over <= 0)
		return TRUE;
	g_object_get(price, "usage-unit", &unit, "unit-amount", &rate, NULL);
	if (rate == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "a metered price needs a price per unit");
	/* Exact: whole units times the rate, then one rounding, half to even,
	 * to what the currency can be paid in -- a $0.001 rate is common. */
	charge = venture_money_multiply_int(rate, over, error);
	if (charge == NULL)
		return FALSE;
	rounded = venture_money_rescale(charge,
		(guint8)venture_currency_get_exponent(venture_money_get_currency(charge)), error);
	if (rounded == NULL)
		return FALSE;
	if (venture_money_is_zero(rounded))
		return TRUE;
	rate_text = venture_money_to_display_string(rate, TRUE);
	over_text = venture_billing_format_count(over);
	used_text = venture_billing_format_count(used);
	included_text = venture_billing_format_count(included);
	from_day = g_date_time_format(from, "%F");
	to_day = g_date_time_format(until, "%F");
	/* The line reads as the arithmetic, so the customer can check it:
	 * "240 API calls x $0.01 (1,240 used, 1,000 included), ...". */
	if (included > 0)
		description = g_strdup_printf("%s %s \xc3\x97 %s (%s used, %s included), %s to %s",
			over_text, unit, rate_text, used_text, included_text, from_day, to_day);
	else
		description = g_strdup_printf("%s %s \xc3\x97 %s, %s to %s", over_text, unit, rate_text, from_day, to_day);
	line = g_object_new(VENTURE_TYPE_INVOICE_LINE, NULL);
	venture_entity_set_organization_id(line, org);
	/* Quantity stays the exact unity the base line uses; the count is in
	 * the words and the money is already multiplied. */
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", description,
		"quantity", 1.0, "unit-price", rounded, "product-id", number(price, "product-id"),
		"tax-code-id", number(price, "tax-code-id"), NULL);
	if (!venture_database_save(database, line, actor, error))
		return FALSE;
	*added = TRUE;
	return TRUE;
}

gboolean
venture_billing_usage_bill(VentureDatabase *database, VentureEntity *sub,
	VentureEntity *invoice, GDateTime *period_start, const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) start = NULL, end = NULL, trial_end = NULL;
	g_autoptr(VentureEntity) price = NULL;
	gint64 org = venture_entity_get_organization_id(sub);
	gboolean added;

	g_object_get(sub, "current-period-start", &start, "current-period-end", &end, "trial-end", &trial_end, NULL);
	/* Only a renewal bills the period that has ended; a start has none. */
	if (start == NULL || end == NULL || !g_date_time_equal(end, period_start))
		return TRUE;
	/* The period ending is the free trial: what was tried is not charged. */
	if (trial_end != NULL && g_date_time_equal(trial_end, end))
		return TRUE;
	price = usage_price(database, sub, org, error);
	if (price == NULL)
		return FALSE;
	if (!venture_plan_price_is_metered(VENTURE_PLAN_PRICE(price)))
		return TRUE;
	return usage_line(database, sub, price, invoice, start, end, actor, &added, error);
}

gboolean
venture_billing_usage_bill_final(VentureDatabase *database, VentureEntity *sub, GDateTime *until,
	const VentureActor *actor, gint64 *invoice_id, GError **error)
{
	g_autoptr(GDateTime) start = NULL, end = NULL, trial_end = NULL;
	g_autoptr(VentureEntity) price = NULL, plan = NULL, invoice = NULL;
	g_autofree gchar *day = NULL, *number_text = NULL;
	gint64 org = venture_entity_get_organization_id(sub);
	gint state = 0;
	gboolean added = FALSE;

	*invoice_id = 0;
	g_object_get(sub, "current-period-start", &start, "current-period-end", &end, "trial-end", &trial_end,
		"status", &state, NULL);
	/* A trial is not charged, and an ended subscription has billed. */
	if (start == NULL || state == 0 || state >= 4 || g_date_time_compare(until, start) <= 0 ||
		(trial_end != NULL && g_date_time_compare(trial_end, start) > 0))
		return TRUE;
	if (end != NULL && g_date_time_compare(until, end) > 0)
		until = end;
	price = usage_price(database, sub, org, error);
	if (price == NULL)
		return FALSE;
	if (!venture_plan_price_is_metered(VENTURE_PLAN_PRICE(price)))
		return TRUE;
	if (venture_billing_usage_total(database, org, venture_entity_get_id(sub), start, until, error) -
		number(price, "included-units") <= 0)
		return error == NULL || *error == NULL;
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(database)),
		database, org, until, error))
		return FALSE;
	plan = load(database, VENTURE_TYPE_PLAN, number(price, "plan-id"), org, error);
	if (plan == NULL)
		return FALSE;
	day = g_date_time_format(until, "%F");
	/* One final usage invoice per subscription: the number says whose and when. */
	number_text = g_strdup_printf("USAGE-%s-%s", venture_entity_get_uuid(sub), day);
	invoice = g_object_new(VENTURE_TYPE_INVOICE, NULL);
	venture_entity_set_organization_id(invoice, org);
	g_object_set(invoice, "number", number_text, "company-id", number(sub, "company-id"),
		"contact-id", number(sub, "contact-id"), "venture-id", number(plan, "venture-id"),
		"issued-at", until, "due-at", until, NULL);
	if (!venture_database_save(database, invoice, actor, error) ||
		!usage_line(database, sub, price, invoice, start, until, actor, &added, error))
		return FALSE;
	if (!added)
		return venture_database_delete(database, invoice, actor, error);
	if (!venture_settlement_service_transition(venture_settlement_service_get(database),
		VENTURE_INVOICE(invoice), "sent", until, actor, error))
		return FALSE;
	*invoice_id = venture_entity_get_id(invoice);
	return TRUE;
}

gboolean
venture_billing_price_check_metering(VentureEntity *price, GError **error)
{
	g_autoptr(VentureMoney) rate = NULL;
	g_autofree gchar *unit = NULL;
	g_autofree gchar *currency = NULL;
	gboolean metered;

	g_object_get(price, "usage-unit", &unit, "unit-amount", &rate, "currency", &currency, NULL);
	metered = unit != NULL && *unit != '\0';
	if (!metered)
	{
		if (rate != NULL || number(price, "included-units") != 0)
			return refuse(error, VENTURE_ERROR_VALIDATION,
				"a price per unit or included units need a usage unit saying what is counted");
		return TRUE;
	}
	if (rate == NULL || venture_money_is_negative(rate))
		return refuse(error, VENTURE_ERROR_VALIDATION, "a metered price needs a price per unit of zero or more");
	if (g_strcmp0(venture_money_get_currency(rate), currency) != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "a metered price charges usage in its own currency");
	if (number(price, "included-units") < 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "included units cannot be negative");
	return TRUE;
}

/* A report whose time is before the subscription's current period has
 * already been invoiced, or was never going to be. */
static gboolean
usage_billed(VentureEntity *sub, GDateTime *at)
{
	g_autoptr(GDateTime) start = NULL;
	g_object_get(sub, "current-period-start", &start, NULL);
	return start != NULL && at != NULL && g_date_time_compare(at, start) < 0;
}

gboolean
venture_billing_usage_check_save(VentureDatabase *database, VentureEntity *record, GError **error)
{
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(VentureEntity) price = NULL;
	g_autoptr(GDateTime) at = NULL;
	g_autofree gchar *key = NULL;
	gint64 org = venture_entity_get_organization_id(record);
	gint status = 0;

	if (number(record, "quantity") <= 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "usage is a positive whole number of units");
	sub = load(database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, number(record, "subscription-id"), org, error);
	if (sub == NULL)
		return FALSE;
	g_object_get(sub, "status", &status, NULL);
	if (status >= 4)
		return refuse(error, VENTURE_ERROR_VALIDATION, "an ended subscription takes no more usage");
	price = load(database, VENTURE_TYPE_PLAN_PRICE, number(sub, "plan-price-id"), org, error);
	if (price == NULL)
		return FALSE;
	if (!venture_plan_price_is_metered(VENTURE_PLAN_PRICE(price)))
		return refuse(error, VENTURE_ERROR_VALIDATION, "the subscription's price is not metered, so there is no rate to charge usage at");
	g_object_get(record, "occurred-at", &at, "idempotency-key", &key, NULL);
	if (at == NULL)
	{
		at = venture_time_now();
		g_object_set(record, "occurred-at", at, NULL);
	}
	if (usage_billed(sub, at))
		return refuse(error, VENTURE_ERROR_VALIDATION,
			"that period has already been invoiced; usage can only be added to the current period");
	if (venture_entity_is_persisted(record))
	{
		g_autoptr(VentureEntity) previous = venture_database_get(database, VENTURE_TYPE_USAGE_RECORD,
			venture_entity_get_id(record), error);
		g_autoptr(GDateTime) was = NULL;
		if (previous == NULL)
			return FALSE;
		g_object_get(previous, "occurred-at", &was, NULL);
		if (number(previous, "subscription-id") != number(record, "subscription-id"))
			return refuse(error, VENTURE_ERROR_VALIDATION, "usage cannot move to another subscription");
		if (usage_billed(sub, was))
			return refuse(error, VENTURE_ERROR_VALIDATION, "billed usage is kept as it was invoiced");
	}
	/* A repeated report is refused as a conflict naming the first, so a
	 * sender retrying after a lost response learns it landed -- rather than
	 * failing on the unique index with nothing to go on. Deleted rows hold
	 * their key too, as the index does. */
	if (key != NULL && *key != '\0')
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USAGE_RECORD);
		g_autoptr(VentureEntity) earlier = NULL;
		g_autoptr(GError) missing = NULL;
		venture_query_set_organization(query, org);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_set_limit(query, 1);
		if (!venture_query_add_filter_string(query, "idempotency-key", VENTURE_FILTER_OP_EQ, key, error))
			return FALSE;
		earlier = venture_database_find_one(database, query, &missing);
		if (earlier != NULL && venture_entity_get_id(earlier) != venture_entity_get_id(record))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				"VentureBillingService: usage with this idempotency key is already recorded as usage_record %"
				G_GINT64_FORMAT, venture_entity_get_id(earlier));
			return FALSE;
		}
	}
	return TRUE;
}

gboolean
venture_billing_usage_check_removal(VentureDatabase *database, VentureEntity *record, GError **error)
{
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(GDateTime) at = NULL;

	stored = venture_database_get(database, VENTURE_TYPE_USAGE_RECORD, venture_entity_get_id(record), error);
	if (stored == NULL)
		return FALSE;
	sub = venture_database_get(database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, number(stored, "subscription-id"), NULL);
	if (sub == NULL)
		return TRUE;
	g_object_get(stored, "occurred-at", &at, NULL);
	if (usage_billed(sub, at))
		return refuse(error, VENTURE_ERROR_VALIDATION, "billed usage is kept as it was invoiced");
	return TRUE;
}
