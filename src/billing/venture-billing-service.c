/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureBillingService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
	gboolean busy;
	gint64 trial_reminder_days;	/* billing.trial_reminder_days; zero sends none */
	gboolean price_change_notices;	/* billing.price_change_notices */
};
G_DEFINE_FINAL_TYPE(VentureBillingService, venture_billing_service, G_TYPE_OBJECT)

/* Numbered away from 1 so another property can be added beside the
 * database without renumbering these. */
enum
{
	PROP_TRIAL_REMINDER_DAYS = 10,
	PROP_PRICE_CHANGE_NOTICES
};

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureBillingService: %s", message);
	return FALSE;
}

static gint64
number(VentureEntity *e, const gchar *field)
{
	gint64 value;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gint
choice(VentureEntity *e, const gchar *field)
{
	gint value;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gboolean
flag(VentureEntity *e, const gchar *field)
{
	gboolean value;
	g_object_get(e, field, &value, NULL);
	return value;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_BILLING_SERVICE(object)->database);
	else if (id == PROP_TRIAL_REMINDER_DAYS)
		g_value_set_int64(value, VENTURE_BILLING_SERVICE(object)->trial_reminder_days);
	else if (id == PROP_PRICE_CHANGE_NOTICES)
		g_value_set_boolean(value, VENTURE_BILLING_SERVICE(object)->price_change_notices);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureBillingService *self = VENTURE_BILLING_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else if (id == PROP_TRIAL_REMINDER_DAYS)
		self->trial_reminder_days = g_value_get_int64(value);
	else if (id == PROP_PRICE_CHANGE_NOTICES)
		self->price_change_notices = g_value_get_boolean(value);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureBillingService *self = VENTURE_BILLING_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_billing_service_parent_class)->finalize(object);
}

static gboolean
veto_accumulator(GSignalInvocationHint *hint, GValue *accumulator, const GValue *value, gpointer data)
{
	if (g_value_get_boxed(value) == NULL)
		return TRUE;
	g_value_set_boxed(accumulator, g_value_get_boxed(value));
	return FALSE;
}

static void
venture_billing_service_class_init(VentureBillingServiceClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->get_property = get_property;
	object->set_property = set_property;
	object->finalize = finalize;
	g_object_class_install_property(object, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	/* Bound from the configuration by the context; a database with no
	 * context -- a test, a tool -- keeps these defaults. */
	g_object_class_install_property(object, PROP_TRIAL_REMINDER_DAYS,
		g_param_spec_int64("trial-reminder-days", "Trial reminder days",
			"Days before a free trial ends that its customer is reminded; zero or less sends none",
			G_MININT64, G_MAXINT64, 3, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, PROP_PRICE_CHANGE_NOTICES,
		g_param_spec_boolean("price-change-notices", "Price change notices",
			"Mail the customer when a subscription's price or seats change",
			TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * VentureBillingService::changing:
	 * @self: the service
	 * @event: a detached snapshot of the validated proposed event
	 *
	 * Emitted after the tentative subscription save and before event persistence, inside the transaction.
	 * Handlers run before the class closure; the first owned GError veto rolls
	 * back the complete operation. Do not perform external effects here.
	 * Returns: (transfer full) (nullable): an error to veto
	 */
	g_signal_new("changing", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
		veto_accumulator, NULL, NULL, G_TYPE_ERROR, 1, VENTURE_TYPE_SUBSCRIPTION_EVENT);
}

static void
venture_billing_service_init(VentureBillingService *self)
{
	self->trial_reminder_days = 3;
	self->price_change_notices = TRUE;
}

static VentureEntity *
new_record(GType type, gint64 org)
{
	VentureEntity *e = g_object_new(type, NULL);
	venture_entity_set_organization_id(e, org);
	return e;
}

static VentureEntity *
load(VentureBillingService *self, GType type, gint64 id, gint64 org, GError **error)
{
	VentureEntity *e = venture_database_get(self->database, type, id, error);
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

static gboolean
write_record(VentureBillingService *self, VentureEntity *e, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = e;
	ok = venture_database_save(self->database, e, actor, error);
	self->writing = NULL;
	return ok;
}

static GPtrArray *
rows(VentureBillingService *self, GType type, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) q = venture_query_new(type);
	venture_query_set_organization(q, org);
	venture_query_set_limit(q, 0);
	if (type == VENTURE_TYPE_DUNNING_STEP &&
		!venture_query_add_order(q, "day-offset", VENTURE_SORT_ASCENDING, error))
		return NULL;
	if (!venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;
	return venture_database_find(self->database, q, error);
}

static VentureMoney *
price_amount(VentureEntity *price, gint64 seats, GError **error)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *currency = NULL;
	g_object_get(price, "amount", &amount, "currency", &currency, NULL);
	if (amount == NULL || venture_money_get_amount(amount) < 0 || seats <= 0 ||
		g_strcmp0(currency, venture_money_get_currency(amount)) != 0)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "price amount, currency and positive seats must agree");
		return NULL;
	}
	return venture_money_multiply_rational(amount, flag(price, "per-seat") ? seats : 1, 1, error);
}

static GDateTime *
next_period(VentureEntity *price, GDateTime *at)
{
	return g_date_time_add_months(at, venture_billing_interval_months(choice(price, "interval")));
}

static GDateTime *
anchored_period(VentureEntity *sub, VentureEntity *price, GDateTime *start)
{
	g_autoptr(GDateTime) anchor = NULL;
	gint months;
	g_object_get(sub, "billing-anchor", &anchor, NULL);
	if (anchor == NULL)
		return next_period(price, start);
	months = (g_date_time_get_year(start) - g_date_time_get_year(anchor)) * 12 +
		g_date_time_get_month(start) - g_date_time_get_month(anchor);
	return g_date_time_add_months(anchor, months + venture_billing_interval_months(choice(price, "interval")));
}

/*
 * A price is available to @company_id when it and its plan are active and
 * the plan is the customer's venture's, or shared. A plan that names a
 * venture is that venture's product; a customer with no venture may be
 * put on anything, as before ventures were set.
 */
static gboolean
price_available(VentureBillingService *self, VentureEntity *price, gint64 org, gint64 company_id, GError **error)
{
	g_autoptr(VentureEntity) plan = load(self, VENTURE_TYPE_PLAN, number(price, "plan-id"), org, error);
	g_autoptr(VentureEntity) customer = NULL;
	gint64 plan_venture;
	if (plan == NULL)
		return FALSE;
	if (!flag(price, "active") || !flag(plan, "active"))
		return refuse(error, VENTURE_ERROR_VALIDATION, "plan and price must be active");
	plan_venture = number(plan, "venture-id");
	if (plan_venture == 0 || company_id == 0)
		return TRUE;
	customer = load(self, VENTURE_TYPE_COMPANY, company_id, org, error);
	if (customer == NULL)
		return FALSE;
	if (number(customer, "venture-id") != 0 && number(customer, "venture-id") != plan_venture)
		return refuse(error, VENTURE_ERROR_VALIDATION, "that plan is sold by another venture than this customer's");
	return TRUE;
}

/*
 * How many months the period being served runs. A price change does not
 * change it, and it is whole months -- 1, 3, 6 or 12 -- so each price is
 * scaled to it and a monthly and a yearly price compare over the same days.
 */
static gint
served_months(GDateTime *start, GDateTime *end)
{
	gint months = (g_date_time_get_year(end) - g_date_time_get_year(start)) * 12 +
		g_date_time_get_month(end) - g_date_time_get_month(start);
	return months == 3 || months == 6 || months == 12 ? months : 1;
}

static VentureMoney *
over_period(VentureEntity *price, const VentureMoney *amount, gint period_months, GError **error)
{
	return venture_money_multiply_rational(amount, period_months,
		venture_billing_interval_months(choice(price, "interval")), error);
}

/*
 * The part of @delta that falls on the days from @at to @end: @delta is
 * allocated over every day of the period and only the remaining days are
 * summed, so remainder cents stay on the days they were first given to.
 * A change and an immediate cancellation both go through here, which is
 * what makes a cancellation's credit the exact mirror of a downgrade.
 */
static VentureMoney *
remaining_share(const VentureMoney *delta, GDateTime *start, GDateTime *end, GDateTime *at,
	gint64 *days_left, GError **error)
{
	g_autoptr(GPtrArray) parts = NULL;
	VentureMoney *total;
	gint64 days = g_date_time_difference(end, start) / G_TIME_SPAN_DAY;
	gint64 elapsed = g_date_time_difference(at, start) / G_TIME_SPAN_DAY;
	gint64 i;
	if (days <= 0 || days > 366 || elapsed < 0 || elapsed >= days)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "change date must be inside the current period; renew first if due");
		return NULL;
	}
	parts = venture_money_allocate_evenly(delta, (gsize)days, error);
	if (parts == NULL)
		return NULL;
	total = venture_money_new_zero(venture_money_get_currency(delta));
	for (i = elapsed; i < days; i++)
	{
		VentureMoney *next = venture_money_add(total, g_ptr_array_index(parts, (guint)i), error);
		venture_money_free(total);
		if (next == NULL)
			return NULL;
		total = next;
	}
	if (days_left != NULL)
		*days_left = days - elapsed;
	return total;
}

static VentureMoney *
proration(VentureEntity *old_price, VentureEntity *new_price, gint64 old_seats,
	gint64 new_seats, GDateTime *start, GDateTime *end, GDateTime *at, GError **error)
{
	g_autoptr(VentureMoney) old_amount = price_amount(old_price, old_seats, error);
	g_autoptr(VentureMoney) new_amount = NULL;
	g_autoptr(VentureMoney) comparable = NULL;
	g_autoptr(VentureMoney) normalized_old = NULL;
	g_autoptr(VentureMoney) delta = NULL;
	gint period_months;
	if (old_amount == NULL)
		return NULL;
	new_amount = price_amount(new_price, new_seats, error);
	if (new_amount == NULL)
		return NULL;
	period_months = served_months(start, end);
	normalized_old = over_period(old_price, old_amount, period_months, error);
	if (normalized_old == NULL)
		return NULL;
	comparable = over_period(new_price, new_amount, period_months, error);
	if (comparable == NULL)
		return NULL;
	delta = venture_money_subtract(comparable, normalized_old, error);
	if (delta == NULL)
		return NULL;
	return remaining_share(delta, start, end, at, NULL, error);
}

/*
 * A discount chosen for a new subscription must be the plan's own, still
 * offered on @at, and say either a percent or an amount -- not both, not
 * neither. Refused here so every surface is held to it.
 */
static gboolean
discount_available(VentureBillingService *self, gint64 discount_id, VentureEntity *price, gint64 org,
	GDateTime *at, GError **error)
{
	g_autoptr(VentureEntity) discount = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) ends = NULL;
	gint64 percent;
	if (discount_id == 0)
		return TRUE;
	discount = load(self, VENTURE_TYPE_PLAN_DISCOUNT, discount_id, org, error);
	if (discount == NULL)
		return FALSE;
	if (number(discount, "plan-id") != number(price, "plan-id"))
		return refuse(error, VENTURE_ERROR_VALIDATION, "that discount belongs to another plan");
	if (!flag(discount, "active"))
		return refuse(error, VENTURE_ERROR_VALIDATION, "that discount is no longer offered");
	g_object_get(discount, "ends-at", &ends, "amount-off", &amount, NULL);
	if (ends != NULL && g_date_time_compare(at, ends) > 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "that discount's offer has ended");
	percent = number(discount, "percent-off");
	if ((percent > 0) == (amount != NULL && venture_money_get_amount(amount) > 0) || percent < 0 || percent > 100 ||
		number(discount, "periods") < 0)
		return refuse(error, VENTURE_ERROR_VALIDATION,
			"a discount takes either a percent from 1 to 100 or an amount, for zero or more periods");
	return TRUE;
}

/*
 * @amount less @discount. Never below zero: an amount off larger than the
 * price makes the invoice free, not a credit.
 */
static VentureMoney *
discount_take(VentureEntity *discount, const VentureMoney *amount, GError **error)
{
	g_autoptr(VentureMoney) off = NULL;
	gint64 percent = number(discount, "percent-off");
	VentureMoney *result;
	g_object_get(discount, "amount-off", &off, NULL);
	if (percent > 0)
		return venture_money_multiply_rational(amount, 100 - percent, 100, error);
	if (off == NULL)
		return venture_money_copy(amount);
	result = venture_money_subtract(amount, off, error);
	if (result != NULL && venture_money_get_amount(result) < 0)
	{
		venture_money_free(result);
		result = venture_money_new_zero(venture_money_get_currency(amount));
	}
	return result;
}

/*
 * A start naming a quote is that accepted quote's subscription, for its
 * customer, and there is only one: the quote's own record says whether it
 * started one, and a subscription already naming it -- a start made
 * around the quote -- is caught as well.
 */
static gboolean
quote_available(VentureBillingService *self, gint64 quote_id, gint64 company_id, gint64 org, GError **error)
{
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(VentureQuery) query = NULL;
	gint status = 0;
	gint64 taken;
	if (quote_id == 0)
		return TRUE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "quote") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_VALIDATION, "a subscription from a quote needs the quotes module");
	quote = load(self, VENTURE_TYPE_QUOTE, quote_id, org, error);
	if (quote == NULL)
		return FALSE;
	g_object_get(quote, "status", &status, NULL);
	if (status != VENTURE_QUOTE_ACCEPTED)
		return refuse(error, VENTURE_ERROR_VALIDATION, "only an accepted quote starts a subscription");
	if (number(quote, "company-id") != company_id)
		return refuse(error, VENTURE_ERROR_VALIDATION, "the quote is for another customer");
	query = venture_query_new(VENTURE_TYPE_CUSTOMER_SUBSCRIPTION);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_int(query, "quote-id", VENTURE_FILTER_OP_EQ, quote_id, error))
		return FALSE;
	taken = venture_database_count(self->database, query, error);
	if (taken < 0)
		return FALSE;
	if (taken > 0 || number(quote, "subscription-id") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "a subscription was already started from this quote");
	return TRUE;
}

/*
 * @amount less the subscription's discount, when it still covers this
 * invoice; counts the invoice against it.
 */
static VentureMoney *
apply_discount(VentureBillingService *self, VentureEntity *sub, VentureMoney *amount, gint64 org,
	gchar **label, GError **error)
{
	g_autoptr(VentureEntity) discount = NULL;
	gint64 used = number(sub, "discount-periods-used"), periods;
	VentureMoney *result;
	if (number(sub, "discount-id") == 0)
		return venture_money_copy(amount);
	discount = load(self, VENTURE_TYPE_PLAN_DISCOUNT, number(sub, "discount-id"), org, error);
	if (discount == NULL)
		return NULL;
	periods = number(discount, "periods");
	if (periods > 0 && used >= periods)
		return venture_money_copy(amount);
	result = discount_take(discount, amount, error);
	if (result == NULL)
		return NULL;
	g_object_set(sub, "discount-periods-used", used + 1, NULL);
	*label = venture_entity_get_display_name(discount);
	return result;
}

/*
 * The invoices a subscription has issued for its periods, from its start
 * and renewal events -- a collection or a cancellation names an invoice
 * too, but bills no period. @last is the newest such invoice and
 * @last_start the first day of the period it billed: a renewal's event
 * carries the period it closed, so the billed one starts where that ended.
 */
static gboolean
billed_periods(VentureBillingService *self, VentureEntity *sub, gint64 *billed, gint64 *last,
	GDateTime **last_start, GError **error)
{
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(GPtrArray) events = NULL;
	guint i;
	*billed = 0;
	*last = 0;
	*last_start = NULL;
	if (!venture_entity_is_persisted(sub))
		return TRUE;
	q = venture_query_new(VENTURE_TYPE_SUBSCRIPTION_EVENT);
	venture_query_set_organization(q, venture_entity_get_organization_id(sub));
	venture_query_set_limit(q, 0);
	if (!venture_query_add_filter_int(q, "subscription-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(sub), error) ||
		!venture_query_add_filter_int(q, "invoice-id", VENTURE_FILTER_OP_GT, 0, error) ||
		!venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;
	events = venture_database_find(self->database, q, error);
	if (events == NULL)
		return FALSE;
	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(events, i);
		gint kind = choice(event, "kind");
		/* Created and renewed are the kinds that bill a period. */
		if (kind != 0 && kind != 1)
			continue;
		(*billed)++;
		*last = number(event, "invoice-id");
		g_clear_pointer(last_start, g_date_time_unref);
		g_object_get(event, kind == 1 ? "period-end" : "period-start", last_start, NULL);
	}
	return TRUE;
}

/*
 * The subscription's discount, when it covered the period being served.
 * Invoices are counted from the start, where a discount is chosen, and a
 * discount covers the first N of them; so the period's invoice was
 * discounted when no more than N have been issued. @issuing counts an
 * invoice this action issues whose event is not written yet. The
 * discounted-invoice counter on the subscription cannot answer this: it
 * stops at N, and the Nth and the N+1th invoice both leave it at N.
 */
static gboolean
period_discount(VentureBillingService *self, VentureEntity *sub, gint64 issuing, VentureEntity **discount,
	GError **error)
{
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(GDateTime) last_start = NULL;
	gint64 billed, last, periods;
	*discount = NULL;
	if (number(sub, "discount-id") == 0)
		return TRUE;
	found = load(self, VENTURE_TYPE_PLAN_DISCOUNT, number(sub, "discount-id"), venture_entity_get_organization_id(sub), error);
	if (found == NULL || !billed_periods(self, sub, &billed, &last, &last_start, error))
		return FALSE;
	periods = number(found, "periods");
	if (periods == 0 || billed + issuing <= periods)
		*discount = g_steal_pointer(&found);
	return TRUE;
}

/*
 * Monthly recurring revenue: what the period being served is billed at,
 * its discount taken off while the discount covers it, divided by the
 * period's months. A discount that has run out leaves the list price.
 */
static VentureMoney *
mrr(VentureBillingService *self, VentureEntity *sub, VentureEntity *price, gint64 seats, gint state,
	gint64 issuing, GError **error)
{
	g_autoptr(VentureMoney) total = price_amount(price, seats, error);
	g_autoptr(VentureEntity) discount = NULL;
	if (total == NULL)
		return NULL;
	if (state != 1 && state != 2)
		return venture_money_new_zero(venture_money_get_currency(total));
	if (!period_discount(self, sub, issuing, &discount, error))
		return NULL;
	if (discount != NULL)
	{
		VentureMoney *discounted = discount_take(discount, total, error);
		if (discounted == NULL)
			return NULL;
		g_clear_pointer(&total, venture_money_free);
		total = discounted;
	}
	return venture_money_multiply_rational(total, 1, venture_billing_interval_months(choice(price, "interval")), error);
}

/*
 * A discount is the plan's offer: moving to another price of the same
 * plan keeps it, moving to another plan ends it.
 */
static void
keep_discount_for(VentureEntity *sub, VentureEntity *from, VentureEntity *to)
{
	if (number(from, "plan-id") != number(to, "plan-id"))
		g_object_set(sub, "discount-id", (gint64)0, "discount-periods-used", (gint64)0, NULL);
}

static gboolean
issue(VentureBillingService *self, VentureEntity *sub, VentureEntity *price,
	GDateTime *at, GDateTime *period_start, const VentureActor *actor, gint64 *invoice_id, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) plan = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) adjustment = NULL;
	g_autofree gchar *date = g_date_time_format(period_start, "%F");
	g_autofree gchar *invoice_number = NULL;
	g_autofree gchar *discount_label = NULL, *description = NULL, *plan_name = NULL, *price_name = NULL;
	gint64 org = venture_entity_get_organization_id(sub);
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
		self->database, org, at, error))
		return FALSE;
	amount = price_amount(price, number(sub, "seats"), error);
	if (amount == NULL)
		return FALSE;
	{
		VentureMoney *discounted = apply_discount(self, sub, amount, org, &discount_label, error);
		if (discounted == NULL)
			return FALSE;
		g_clear_pointer(&amount, venture_money_free);
		amount = discounted;
		/* The count of discounted invoices is the subscription's; a start
		 * saved the row before issuing, so it is saved again here. */
		if (discount_label != NULL && !write_record(self, sub, actor, error))
			return FALSE;
	}
	g_object_get(sub, "pending-adjustment", &adjustment, NULL);
	if (adjustment != NULL && venture_money_get_amount(adjustment) > 0)
	{
		VentureMoney *combined = venture_money_add(amount, adjustment, error);
		if (combined == NULL)
			return FALSE;
		g_clear_pointer(&amount, venture_money_free);
		amount = combined;
	}
	plan = load(self, VENTURE_TYPE_PLAN, number(price, "plan-id"), org, error);
	if (plan == NULL)
		return FALSE;
	invoice_number = g_strdup_printf("BILL-%s-%s", venture_entity_get_uuid(sub), date);
	invoice = new_record(VENTURE_TYPE_INVOICE, org);
	g_object_set(invoice, "number", invoice_number, "company-id", number(sub, "company-id"),
		"contact-id", number(sub, "contact-id"), "venture-id", number(plan, "venture-id"),
		"issued-at", at, "due-at", at, NULL);
	if (!venture_database_save(self->database, invoice, actor, error))
		return FALSE;
	line = new_record(VENTURE_TYPE_INVOICE_LINE, org);
	/* Seats have already been multiplied with VentureMoney. Invoice quantity
	 * is the existing exact unity value, never a monetary floating point path. */
	/* The line says what was bought, for which period, and any discount,
	 * so the customer's invoice reads as the plan and not "renewal". */
	g_object_get(plan, "name", &plan_name, NULL);
	price_name = venture_entity_get_display_name(price);
	{
		g_autoptr(GDateTime) until = next_period(price, period_start);
		g_autofree gchar *to = g_date_time_format(until, "%F");
		GString *text = g_string_new(NULL);
		g_string_append_printf(text, "%s \xe2\x80\x94 %s", plan_name != NULL ? plan_name : "Subscription", price_name);
		if (flag(price, "per-seat") && number(sub, "seats") > 1)
			g_string_append_printf(text, " \xc3\x97 %" G_GINT64_FORMAT " seats", number(sub, "seats"));
		g_string_append_printf(text, ", %s to %s", date, to);
		if (discount_label != NULL)
			g_string_append_printf(text, "; %s", discount_label);
		description = g_string_free(text, FALSE);
	}
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", description,
		"quantity", 1.0, "unit-price", amount, "product-id", number(price, "product-id"),
		"tax-code-id", number(price, "tax-code-id"), NULL);
	if (!venture_database_save(self->database, line, actor, error) ||
		!venture_billing_usage_bill(self->database, sub, invoice, period_start, actor, error) ||
		!venture_settlement_service_transition(venture_settlement_service_get(self->database),
			VENTURE_INVOICE(invoice), "sent", at, actor, error))
		return FALSE;
	if (adjustment != NULL && venture_money_get_amount(adjustment) < 0)
	{
		g_autoptr(VentureEntity) credit = new_record(VENTURE_TYPE_CUSTOMER_CREDIT, org);
		g_autoptr(VentureEntity) allocation = new_record(VENTURE_TYPE_PAYMENT_ALLOCATION, org);
		g_autoptr(VentureMoney) credit_amount = venture_money_multiply_rational(adjustment, -1, 1, error);
		g_autoptr(VentureMoney) difference = NULL;
		const VentureMoney *applied;
		if (credit_amount == NULL)
			return FALSE;
		g_object_set(credit, "customer-id", number(sub, "company-id"), "kind", "credit_note", "date", at,
			"amount", credit_amount, "reference", "Subscription proration", NULL);
		if (!venture_database_save(self->database, credit, actor, error))
			return FALSE;
		/* Price versions may use different exponents in the same currency.
		 * Compare their values, never their unscaled integer coefficients. */
		difference = venture_money_subtract(credit_amount, amount, error);
		if (difference == NULL)
			return FALSE;
		applied = venture_money_get_amount(difference) < 0 ? credit_amount : amount;
		if (!venture_money_is_zero(applied))
		{
			g_object_set(allocation, "credit-id", venture_entity_get_id(credit), "invoice-id", venture_entity_get_id(invoice),
				"date", at, "amount", applied, NULL);
			if (!venture_database_save(self->database, allocation, actor, error))
				return FALSE;
		}
	}
	*invoice_id = venture_entity_get_id(invoice);
	return TRUE;
}

/*
 * What an immediate cancellation gives back. The net is the unused days of
 * the period at the price being served, less its discount when the
 * discount covered this period, prorated exactly as a downgrade to nothing
 * would be; any part-period difference still carried from a change this
 * period is folded in, because no renewal will ever settle it. The tax is
 * the same share of it the period's invoice charged, so a customer gets
 * back the tax paid on the days they will not use and the return shows it
 * credited.
 */
typedef struct
{
	VentureMoney *net;
	VentureMoney *tax;
	gint64 jurisdiction;
	gint64 invoice;
	gint64 days_left;
} UnusedCredit;

static void
unused_credit_clear(UnusedCredit *credit)
{
	g_clear_pointer(&credit->net, venture_money_free);
	g_clear_pointer(&credit->tax, venture_money_free);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(UnusedCredit, unused_credit_clear)

/* The invoice line a period's invoice was issued with: issue() writes one. */
static VentureEntity *
period_line(VentureBillingService *self, gint64 invoice, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
	g_autoptr(GPtrArray) lines = NULL;
	venture_query_set_organization(q, org);
	venture_query_set_limit(q, 1);
	if (!venture_query_add_filter_int(q, "invoice-id", VENTURE_FILTER_OP_EQ, invoice, error) ||
		!venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;
	lines = venture_database_find(self->database, q, error);
	if (lines == NULL)
		return NULL;
	if (lines->len == 0)
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "the period's invoice has no line");
		return NULL;
	}
	return g_object_ref(g_ptr_array_index(lines, 0));
}

/*
 * Fills @out with what cancelling @sub at @at credits; a zero net when
 * nothing is owed back. Only a period that was actually invoiced is
 * credited: a trial, a period nobody billed, and a date outside the
 * period (the period was served, or has not begun) give nothing.
 */
static gboolean
unused_credit(VentureBillingService *self, VentureEntity *sub, VentureEntity *price, GDateTime *at,
	UnusedCredit *out, GError **error)
{
	g_autoptr(VentureMoney) charge = price_amount(price, number(sub, "seats"), error);
	g_autoptr(VentureMoney) normalized = NULL, delta = NULL, unused = NULL, pending = NULL, net = NULL;
	g_autoptr(VentureEntity) discount = NULL, line = NULL;
	g_autoptr(GDateTime) start = NULL, end = NULL, billed_start = NULL;
	g_autoptr(VentureMoney) line_net = NULL, line_tax = NULL;
	gint64 billed = 0, invoice = 0;
	gint state = choice(sub, "status");
	if (charge == NULL)
		return FALSE;
	out->net = venture_money_new_zero(venture_money_get_currency(charge));
	g_object_get(sub, "current-period-start", &start, "current-period-end", &end, "pending-adjustment", &pending, NULL);
	if (state == 0 || state >= 4 || start == NULL || end == NULL ||
		g_date_time_compare(at, start) < 0 || g_date_time_compare(at, end) >= 0)
		return TRUE;
	if (!billed_periods(self, sub, &billed, &invoice, &billed_start, error))
		return FALSE;
	if (invoice == 0 || billed_start == NULL || g_date_time_compare(billed_start, start) != 0)
		return TRUE;
	if (!period_discount(self, sub, 0, &discount, error))
		return FALSE;
	if (discount != NULL)
	{
		VentureMoney *discounted = discount_take(discount, charge, error);
		if (discounted == NULL)
			return FALSE;
		g_clear_pointer(&charge, venture_money_free);
		charge = discounted;
	}
	normalized = over_period(price, charge, served_months(start, end), error);
	if (normalized == NULL)
		return FALSE;
	/* Negated before it is spread, as a downgrade to nothing would be:
	 * spreading the positive amount and negating the sum can round a
	 * remainder cent onto another day. */
	delta = venture_money_negate(normalized);
	unused = remaining_share(delta, start, end, at, &out->days_left, error);
	if (unused == NULL)
		return FALSE;
	net = pending != NULL ? venture_money_add(unused, pending, error) : venture_money_copy(unused);
	if (net == NULL)
		return FALSE;
	if (venture_money_get_amount(net) >= 0)
		return TRUE;
	g_clear_pointer(&out->net, venture_money_free);
	out->net = venture_money_negate(net);
	out->invoice = invoice;
	line = period_line(self, invoice, venture_entity_get_organization_id(sub), error);
	if (line == NULL)
		return FALSE;
	g_object_get(line, "income-amount", &line_net, "tax-amount", &line_tax, "tax-jurisdiction-id", &out->jurisdiction, NULL);
	if (line_net != NULL && line_tax != NULL && venture_money_get_amount(line_net) > 0 &&
		venture_money_get_amount(line_tax) > 0)
	{
		g_autoptr(VentureMoney) scaled = venture_money_rescale(line_tax, venture_money_get_exponent(line_net), error);
		if (scaled == NULL)
			return FALSE;
		out->tax = venture_money_multiply_rational(out->net, venture_money_get_amount(scaled),
			venture_money_get_amount(line_net), error);
		if (out->tax == NULL)
			return FALSE;
	}
	return TRUE;
}

/*
 * Issues the credit note for @credit and applies it to what is still owed
 * on the period's invoice -- a customer who never paid for the period is
 * not paid out for the part of it they will not use. What is not applied
 * stays on the customer's account as credit.
 */
static gboolean
issue_unused_credit(VentureBillingService *self, VentureEntity *sub, const UnusedCredit *credit,
	const VentureMoney *gross, GDateTime *at, const VentureActor *actor, GError **error)
{
	gint64 org = venture_entity_get_organization_id(sub);
	g_autoptr(VentureEntity) note = new_record(VENTURE_TYPE_CUSTOMER_CREDIT, org);
	g_autoptr(VentureMoney) balance = NULL;
	g_autofree gchar *reference = g_strdup_printf("Cancelled with %" G_GINT64_FORMAT " days of the period unused",
		credit->days_left);
	g_object_set(note, "customer-id", number(sub, "company-id"), "kind", "credit_note", "date", at,
		"amount", gross, "reference", reference, NULL);
	if (credit->tax != NULL && !venture_money_is_zero(credit->tax))
		g_object_set(note, "tax-amount", credit->tax, "tax-jurisdiction-id", credit->jurisdiction, NULL);
	if (!venture_database_save(self->database, note, actor, error))
		return FALSE;
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database),
		credit->invoice, NULL, error);
	if (balance == NULL)
		return FALSE;
	if (venture_money_get_amount(balance) > 0)
	{
		g_autoptr(VentureEntity) allocation = new_record(VENTURE_TYPE_PAYMENT_ALLOCATION, org);
		g_object_set(allocation, "credit-id", venture_entity_get_id(note), "invoice-id", credit->invoice,
			"date", at, "amount", venture_money_compare(gross, balance) < 0 ? gross : balance, NULL);
		if (!venture_database_save(self->database, allocation, actor, error))
			return FALSE;
	}
	return TRUE;
}

static VentureMoney *
unused_gross(const UnusedCredit *credit, GError **error)
{
	if (credit->tax == NULL)
		return venture_money_copy(credit->net);
	return venture_money_add(credit->net, credit->tax, error);
}

VentureMoney *
venture_billing_service_cancel_credit(VentureBillingService *self, VentureCustomerSubscription *subscription,
	GDateTime *at, gint64 *days_left, GError **error)
{
	g_auto(UnusedCredit) credit = { NULL, NULL, 0, 0, 0 };
	g_autoptr(VentureEntity) price = NULL;
	VentureEntity *sub = VENTURE_ENTITY(subscription);

	g_return_val_if_fail(VENTURE_IS_BILLING_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_SUBSCRIPTION(subscription), NULL);
	g_return_val_if_fail(at != NULL, NULL);
	if (self->database == NULL)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "service is unavailable");
		return NULL;
	}
	price = load(self, VENTURE_TYPE_PLAN_PRICE, number(sub, "plan-price-id"), venture_entity_get_organization_id(sub), error);
	if (price == NULL || !unused_credit(self, sub, price, at, &credit, error))
		return NULL;
	if (days_left != NULL)
		*days_left = credit.days_left;
	return unused_gross(&credit, error);
}

/* --- Customer notices ----------------------------------------------------
 *
 * The customer hears about two things before an invoice surprises them: a
 * free trial about to turn into a charge, and a change to what they pay.
 * Both are queued through the outbox inside the instruction's own
 * transaction, so a notice exists exactly when the change it describes was
 * committed, and both carry an idempotency key, so a repeated sweep or a
 * retried request mails nobody twice. Queuing is not sending: delivery is
 * the outbox's, after commit, like every other message.
 */

static gboolean
mail_available(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message") != G_TYPE_INVALID;
}

/*
 * Where a subscription's mail goes: its billing contact, who is the person
 * who agreed to it, else the customer company's address. NULL when neither
 * has one -- a notice nobody can receive is skipped, never an error that
 * stops the change it describes.
 */
static gchar *
customer_address(VentureBillingService *self, VentureEntity *sub, gint64 org, gchar **name)
{
	static const struct { const gchar *field; const gchar *type; } sources[] = {
		{ "contact-id", "contact" }, { "company-id", "company" }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(sources); i++)
	{
		g_autoptr(VentureEntity) who = NULL;
		g_autofree gchar *email = NULL;
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), sources[i].type);
		if (type == G_TYPE_INVALID || number(sub, sources[i].field) <= 0)
			continue;
		who = venture_database_get(self->database, type, number(sub, sources[i].field), NULL);
		if (who == NULL || venture_entity_is_deleted(who) || venture_entity_get_organization_id(who) != org)
			continue;
		g_object_get(who, "email", &email, NULL);
		if (venture_string_is_empty(email))
			continue;
		g_strstrip(email);
		if (strpbrk(email, "\r\n") != NULL || strchr(email, '@') == NULL)
			continue;
		*name = venture_entity_get_display_name(who);
		return g_steal_pointer(&email);
	}
	return NULL;
}

/*
 * What the next invoice will charge, worked out the way issue() will work
 * it out -- the price it switches to at renewal, the discount while it
 * still covers invoices, the part-period difference carried to it, and
 * the price's tax unless the customer is exempt -- without writing anything: the discount is counted on a copy. A credit
 * larger than the charge leaves nothing to pay, never a negative invoice.
 * NULL with no error when nothing more will be invoiced.
 */
static VentureMoney *
preview_next_invoice(VentureBillingService *self, VentureEntity *sub, gint64 org, GError **error)
{
	g_autoptr(VentureEntity) copy = venture_entity_duplicate(sub);
	g_autoptr(VentureEntity) price = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) discounted = NULL;
	g_autoptr(VentureMoney) adjustment = NULL;
	g_autofree gchar *label = NULL;
	gint state = choice(sub, "status");
	if (state > 2 || flag(sub, "cancel-at-period-end"))
		return NULL;
	price = load(self, VENTURE_TYPE_PLAN_PRICE, number(sub, "plan-price-id"), org, error);
	if (price == NULL)
		return NULL;
	if (number(sub, "pending-plan-price-id") != 0)
	{
		g_autoptr(VentureEntity) next = load(self, VENTURE_TYPE_PLAN_PRICE, number(sub, "pending-plan-price-id"), org, error);
		if (next == NULL)
			return NULL;
		keep_discount_for(copy, price, next);
		g_set_object(&price, next);
	}
	amount = price_amount(price, number(sub, "seats"), error);
	if (amount == NULL)
		return NULL;
	discounted = apply_discount(self, copy, amount, org, &label, error);
	if (discounted == NULL)
		return NULL;
	g_object_get(sub, "pending-adjustment", &adjustment, NULL);
	/* A charge carried from a change is on the invoice line and taxed with
	 * it; a credit is applied after, untaxed -- as issue() does. */
	if (adjustment != NULL && venture_money_get_amount(adjustment) > 0)
	{
		VentureMoney *charged = venture_money_add(discounted, adjustment, error);
		if (charged == NULL)
			return NULL;
		g_clear_pointer(&discounted, venture_money_free);
		discounted = charged;
		g_clear_pointer(&adjustment, venture_money_free);
	}
	if (number(price, "tax-code-id") != 0)
	{
		g_autoptr(VentureEntity) customer = load(self, VENTURE_TYPE_COMPANY, number(sub, "company-id"), org, error);
		g_autoptr(VentureEntity) code = NULL;
		if (customer == NULL)
			return NULL;
		/* An exempt customer's invoice freezes no tax, whatever the price says. */
		if (!flag(customer, "tax-exempt"))
		{
			g_autoptr(VentureMoney) tax = NULL;
			VentureMoney *taxed;
			code = load(self, VENTURE_TYPE_TAX_CODE, number(price, "tax-code-id"), org, error);
			if (code == NULL)
				return NULL;
			tax = venture_tax_code_levy(VENTURE_TAX_CODE(code), discounted, error);
			if (tax == NULL)
				return NULL;
			taxed = venture_money_add(discounted, tax, error);
			if (taxed == NULL)
				return NULL;
			g_clear_pointer(&discounted, venture_money_free);
			discounted = taxed;
		}
	}
	if (adjustment != NULL && !venture_money_is_zero(adjustment))
	{
		VentureMoney *total = venture_money_add(discounted, adjustment, error);
		if (total == NULL)
			return NULL;
		if (venture_money_get_amount(total) < 0)
		{
			venture_money_free(total);
			total = venture_money_new_zero(venture_money_get_currency(discounted));
		}
		g_clear_pointer(&discounted, venture_money_free);
		discounted = total;
	}
	return g_steal_pointer(&discounted);
}

/* "Starter at $30.00 a month per seat, 2 seats" -- the terms as the
 * customer would say them. */
static gchar *
terms_text(VentureBillingService *self, gint64 price_id, gint64 seats, gint64 org)
{
	g_autoptr(VentureEntity) price = load(self, VENTURE_TYPE_PLAN_PRICE, price_id, org, NULL);
	g_autoptr(VentureEntity) plan = NULL;
	g_autofree gchar *plan_name = NULL, *price_name = NULL;
	GString *text;
	if (price == NULL)
		return g_strdup("your previous plan");
	plan = load(self, VENTURE_TYPE_PLAN, number(price, "plan-id"), org, NULL);
	if (plan != NULL)
		g_object_get(plan, "name", &plan_name, NULL);
	price_name = venture_entity_get_display_name(price);
	text = g_string_new(NULL);
	g_string_append_printf(text, "%s at %s", plan_name != NULL ? plan_name : "Your plan", price_name);
	if (flag(price, "per-seat"))
		g_string_append_printf(text, ", %" G_GINT64_FORMAT " seat%s", seats, seats == 1 ? "" : "s");
	return g_string_free(text, FALSE);
}

static gboolean
queue_notice(VentureBillingService *self, VentureEntity *sub, gint64 org, const gchar *to,
	const gchar *subject, const gchar *body, const gchar *key, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(VentureMailMessage) queued = NULL;
	venture_entity_set_organization_id(VENTURE_ENTITY(message), org);
	g_object_set(message, "to", to, "subject", subject, "text-body", body, "idempotency-key", key,
		"related-type", "customer_subscription", "related-id", venture_entity_get_id(sub), NULL);
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(self->database), message, actor, error);
	return queued != NULL;
}

static gchar *
trial_reminder_key(VentureEntity *sub)
{
	return g_strdup_printf("trial-reminder:%s", venture_entity_get_uuid(sub));
}

/*
 * A trial that ends within the configured number of days tells its
 * customer the date and what the first invoice will be. Keyed by the
 * subscription, so it is sent once whatever the sweep cadence. A trial
 * already ended is the renewal's business, and one set to end at renewal
 * will never be invoiced, so neither is reminded.
 */
static gboolean
trial_reminder(VentureBillingService *self, VentureEntity *sub, GDateTime *at, const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) trial_end = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *to = NULL, *name = NULL, *key = NULL, *day = NULL, *money = NULL, *terms = NULL;
	g_autofree gchar *subject = NULL, *body = NULL;
	gint64 org = venture_entity_get_organization_id(sub);
	gint64 days = MIN(self->trial_reminder_days, 366);
	if (days <= 0 || choice(sub, "status") != 0 || flag(sub, "cancel-at-period-end") || !mail_available())
		return TRUE;
	g_object_get(sub, "trial-end", &trial_end, NULL);
	if (trial_end == NULL || g_date_time_compare(trial_end, at) <= 0 ||
		g_date_time_difference(trial_end, at) > days * G_TIME_SPAN_DAY)
		return TRUE;
	to = customer_address(self, sub, org, &name);
	if (to == NULL)
		return TRUE;
	amount = preview_next_invoice(self, sub, org, error);
	if (amount == NULL)
		return error == NULL || *error == NULL;
	key = trial_reminder_key(sub);
	day = venture_time_to_date_string(trial_end, NULL);
	money = venture_money_to_display_string(amount, TRUE);
	terms = terms_text(self, number(sub, "pending-plan-price-id") != 0 ? number(sub, "pending-plan-price-id") :
		number(sub, "plan-price-id"), number(sub, "seats"), org);
	subject = g_strdup_printf("Your trial ends on %s", day);
	body = g_strdup_printf("Hello %s,\n\nYour trial ends on %s; your first invoice will be %s.\n\n"
		"That is for %s. Nothing needs doing to carry on.\n", name, day, money, terms);
	return queue_notice(self, sub, org, to, subject, body, key, actor, error);
}

/*
 * A change to what the customer pays -- another price now or at renewal,
 * or a different number of seats -- says what changed, when it takes
 * effect and what the next invoice will be. Keyed by the change's event,
 * so each change is told once and two changes are told twice.
 */
static gboolean
price_change_notice(VentureBillingService *self, VentureEntity *sub, VentureEntity *event, gint64 from_price,
	gint64 from_seats, gboolean scheduled, const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) effective = NULL;
	g_autoptr(GDateTime) renews = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *to = NULL, *name = NULL, *key = NULL, *before = NULL, *after = NULL;
	g_autofree gchar *effective_day = NULL, *renew_day = NULL, *subject = NULL;
	GString *body;
	gint64 org = venture_entity_get_organization_id(sub);
	gint64 to_price = scheduled ? number(sub, "pending-plan-price-id") : number(sub, "plan-price-id");
	gint64 seats = number(sub, "seats");
	if (!self->price_change_notices || !mail_available() || (to_price == from_price && seats == from_seats))
		return TRUE;
	to = customer_address(self, sub, org, &name);
	if (to == NULL)
		return TRUE;
	g_object_get(sub, "current-period-end", &renews, NULL);
	if (scheduled)
		effective = g_date_time_ref(renews);
	else
		g_object_get(event, "at", &effective, NULL);
	amount = preview_next_invoice(self, sub, org, error);
	if (amount == NULL && error != NULL && *error != NULL)
		return FALSE;
	before = terms_text(self, from_price, from_seats, org);
	after = terms_text(self, to_price, seats, org);
	effective_day = venture_time_to_date_string(effective, NULL);
	renew_day = venture_time_to_date_string(renews, NULL);
	subject = g_strdup(scheduled ? "Your subscription changes at renewal" : "Your subscription has changed");
	body = g_string_new(NULL);
	g_string_append_printf(body, "Hello %s,\n\nYour subscription is changing from %s to %s.\n\n", name, before, after);
	g_string_append_printf(body, scheduled ? "This takes effect on %s, when it renews; until then nothing changes.\n"
		: "This took effect on %s.\n", effective_day);
	if (amount != NULL)
	{
		g_autofree gchar *money = venture_money_to_display_string(amount, TRUE);
		g_autoptr(VentureMoney) adjustment = NULL;
		g_string_append_printf(body, "Your next invoice, on %s, will be %s", renew_day, money);
		g_object_get(sub, "pending-adjustment", &adjustment, NULL);
		if (adjustment != NULL && venture_money_get_amount(adjustment) > 0)
		{
			g_autofree gchar *part = venture_money_to_display_string(adjustment, TRUE);
			g_string_append_printf(body, ", including %s for the rest of this period on the new terms", part);
		}
		else if (adjustment != NULL && venture_money_get_amount(adjustment) < 0)
		{
			g_autoptr(VentureMoney) credit = venture_money_multiply_rational(adjustment, -1, 1, NULL);
			g_autofree gchar *part = credit != NULL ? venture_money_to_display_string(credit, TRUE) : NULL;
			if (part != NULL)
				g_string_append_printf(body, ", after a credit of %s for the rest of this period", part);
		}
		g_string_append(body, ".\n");
	}
	else
		g_string_append_printf(body, "No further invoice is due: the subscription ends on %s.\n", renew_day);
	key = g_strdup_printf("price-change:%s", venture_entity_get_uuid(event));
	{
		g_autofree gchar *text = g_string_free(body, FALSE);
		return queue_notice(self, sub, org, to, subject, text, key, actor, error);
	}
}

/*
 * A code the customer quoted, to the discount it names: one of the chosen
 * price's plan, compared without regard to case, since customers type codes
 * however they read them. A code nobody offers, retired or past its date is
 * refused in words the customer could be told; the rest of the discount's
 * rules are then discount_available()'s, as for a chosen discount.
 */
static gboolean
resolve_discount_code(VentureBillingService *self, VentureEntity *request, VentureEntity *price, gint64 org,
	GDateTime *at, GError **error)
{
	g_autofree gchar *code = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(GPtrArray) offers = NULL;
	g_autofree gchar *why = NULL;
	guint i;
	g_object_get(request, "discount-code", &code, NULL);
	if (code == NULL || *g_strstrip(code) == '\0')
		return TRUE;
	if (number(request, "discount-id") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "choose a discount or enter a code, not both");
	q = venture_query_new(VENTURE_TYPE_PLAN_DISCOUNT);
	venture_query_set_organization(q, org);
	venture_query_set_limit(q, 0);
	if (!venture_query_add_filter_int(q, "plan-id", VENTURE_FILTER_OP_EQ, number(price, "plan-id"), error) ||
		!venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;
	offers = venture_database_find(self->database, q, error);
	if (offers == NULL)
		return FALSE;
	for (i = 0; i < offers->len; i++)
	{
		VentureEntity *offer = g_ptr_array_index(offers, i);
		g_autofree gchar *candidate = NULL;
		g_autoptr(GDateTime) ends = NULL;
		g_object_get(offer, "code", &candidate, "ends-at", &ends, NULL);
		if (candidate == NULL || g_ascii_strcasecmp(g_strstrip(candidate), code) != 0)
			continue;
		if (!flag(offer, "active"))
		{
			g_free(why);
			why = g_strdup_printf("the code %s is no longer offered", code);
			continue;
		}
		if (ends != NULL && g_date_time_compare(at, ends) > 0)
		{
			g_autofree gchar *day = venture_time_to_date_string(ends, NULL);
			g_free(why);
			why = g_strdup_printf("the code %s expired on %s", code, day);
			continue;
		}
		g_object_set(request, "discount-id", venture_entity_get_id(offer), NULL);
		return TRUE;
	}
	if (why == NULL)
		why = g_strdup_printf("the code %s is not one this plan offers", code);
	return refuse(error, VENTURE_ERROR_VALIDATION, why);
}

static gboolean
perform(VentureBillingService *self, VentureEntity *request, const VentureActor *actor, GError **error)
{
	g_autofree gchar *verb = NULL;
	g_autofree gchar *external = NULL;
	g_autoptr(GDateTime) at = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(GDateTime) last = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(VentureEntity) price = NULL;
	g_autoptr(VentureEntity) next_price = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) snapshot = NULL;
	g_autoptr(VentureMoney) before = NULL;
	g_autoptr(VentureMoney) after = NULL;
	g_autoptr(VentureMoney) prorated = NULL;
	g_autoptr(GError) veto = NULL;
	gint64 org = venture_entity_get_organization_id(request);
	gint64 id = number(request, "subscription-id");
	gint64 seats;
	gint64 old_seats;
	gint64 old_price;
	gint64 invoice_id = 0;
	gint64 final_usage = 0;
	gint state;
	gint old_state;
	gint kind = 0;
	gint64 issuing = 0;
	gboolean scheduled = flag(request, "at-period-end");
	gboolean credited = FALSE;
	g_object_get(request, "action", &verb, "at", &at, NULL);
	if (at == NULL || org <= 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "an organization and effective date are required");
	if (g_strcmp0(verb, "start") == 0)
	{
		g_autoptr(VentureEntity) customer = load(self, VENTURE_TYPE_COMPANY, number(request, "company-id"), org, error);
		gint64 trial;
		if (customer == NULL)
			return FALSE;
		if (number(request, "contact-id") != 0)
		{
			g_autoptr(VentureEntity) contact = load(self, VENTURE_TYPE_CONTACT, number(request, "contact-id"), org, error);
			if (contact == NULL)
				return FALSE;
			if (number(contact, "company-id") != number(request, "company-id"))
				return refuse(error, VENTURE_ERROR_VALIDATION, "billing contact must belong to the subscription customer");
		}
		price = load(self, VENTURE_TYPE_PLAN_PRICE, number(request, "plan-price-id"), org, error);
		if (price == NULL || !price_available(self, price, org, number(request, "company-id"), error))
			return FALSE;
		seats = number(request, "seats");
		if (seats == 0)
			seats = 1;
		trial = number(price, "trial-days");
		if (trial < 0 || trial > 366)
			return refuse(error, VENTURE_ERROR_VALIDATION, "trial days must be between zero and 366");
		/* Billing from the first day, when the customer wants to pay now. */
		if (flag(request, "skip-trial"))
			trial = 0;
		if (!resolve_discount_code(self, request, price, org, at, error) ||
			!discount_available(self, number(request, "discount-id"), price, org, at, error) ||
			!quote_available(self, number(request, "quote-id"), number(request, "company-id"), org, error))
			return FALSE;
		state = trial > 0 ? 0 : 1;
		sub = new_record(VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, org);
		g_object_get(request, "external-id", &external, NULL);
		start = g_date_time_ref(at);
		end = trial > 0 ? g_date_time_add_days(at, (gint)trial) : next_period(price, at);
		g_object_set(sub, "company-id", number(request, "company-id"), "contact-id", number(request, "contact-id"),
			"plan-price-id", venture_entity_get_id(price), "external-id", external,
			"current-period-start", start, "current-period-end", end, "trial-end", trial > 0 ? end : NULL, "billing-anchor", trial > 0 ? end : start,
			"discount-id", number(request, "discount-id"), "discount-periods-used", (gint64)0,
			"quote-id", number(request, "quote-id"), NULL);
		old_seats = 0;
		old_price = 0;
		old_state = 0;
		before = mrr(self, sub, price, seats, 0, 0, error);
	}
	else
	{
		sub = load(self, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, id, org, error);
		if (sub == NULL)
			return FALSE;
		if (number(request, "expected-version") != 0 &&
			number(request, "expected-version") != venture_entity_get_version(sub))
			return refuse(error, VENTURE_ERROR_CONFLICT, "subscription changed since this action was staged");
		price = load(self, VENTURE_TYPE_PLAN_PRICE, number(sub, "plan-price-id"), org, error);
		if (price == NULL)
			return FALSE;
		g_object_get(sub, "current-period-start", &start, "current-period-end", &end, "last-event-at", &last, NULL);
		if (start == NULL || end == NULL || (last != NULL && g_date_time_compare(at, last) < 0))
			return refuse(error, VENTURE_ERROR_VALIDATION, "events must be chronological and periods complete");
		old_state = state = choice(sub, "status");
		old_seats = seats = number(sub, "seats");
		old_price = venture_entity_get_id(price);
		before = mrr(self, sub, price, seats, state, 0, error);
		if (before == NULL)
			return FALSE;
		if (g_strcmp0(verb, "renew") == 0)
		{
			GDateTime *next;
			if (state != 0 && state != 1 && !(state < 4 && flag(sub, "cancel-at-period-end")))
				return refuse(error, VENTURE_ERROR_VALIDATION, "only active or trialing subscriptions renew");
			if (g_date_time_compare(at, end) < 0)
				return TRUE;
			if (flag(sub, "cancel-at-period-end"))
			{
				if (last != NULL && g_date_time_compare(end, last) < 0)
					return refuse(error, VENTURE_ERROR_VALIDATION, "scheduled cancellation predates a later action; cancel immediately instead");
				g_clear_pointer(&at, g_date_time_unref);
				at = g_date_time_ref(end);
				/* The ended period's usage is billed as it closes; the
				 * invoice is never named on the cancel event. */
				if (!venture_billing_usage_bill_final(self->database, sub, at, actor, &final_usage, error))
					return FALSE;
				state = 4;
				kind = 7;
				g_object_set(sub, "cancelled-at", at, "cancel-at-period-end", FALSE, NULL);
			}
			else
			{
				if (number(sub, "pending-plan-price-id") != 0)
				{
					next_price = load(self, VENTURE_TYPE_PLAN_PRICE, number(sub, "pending-plan-price-id"), org, error);
					if (next_price == NULL)
						return FALSE;
					keep_discount_for(sub, price, next_price);
					g_set_object(&price, next_price);
					g_object_set(sub, "plan-price-id", venture_entity_get_id(price), "pending-plan-price-id", (gint64)0, "billing-anchor", end, NULL);
				}
				if (!issue(self, sub, price, at, end, actor, &invoice_id, error))
					return FALSE;
				next = anchored_period(sub, price, end);
				g_object_set(sub, "current-period-start", end, "current-period-end", next, "pending-adjustment", NULL, NULL);
				g_date_time_unref(next);
				state = 1;
				kind = 1;
				issuing = 1;
			}
		}
		else if (g_strcmp0(verb, "change") == 0 || g_strcmp0(verb, "change-seats") == 0)
		{
			if (state != 0 && state != 1 && state != 2)
				return refuse(error, VENTURE_ERROR_VALIDATION, "only live subscriptions may change terms");
			next_price = g_strcmp0(verb, "change") == 0 ?
				load(self, VENTURE_TYPE_PLAN_PRICE, number(request, "plan-price-id"), org, error) : g_object_ref(price);
			/* Only a new price is judged against the customer's venture;
			 * changing seats keeps the terms already agreed. */
			if (next_price == NULL || !price_available(self, next_price, org,
				g_strcmp0(verb, "change") == 0 ? number(sub, "company-id") : 0, error))
				return FALSE;
			if (g_strcmp0(verb, "change-seats") == 0)
				seats = number(request, "seats");
			prorated = proration(price, next_price, old_seats, seats, start, end, at, error);
			if (prorated == NULL)
				return FALSE;
			kind = g_strcmp0(verb, "change-seats") == 0 ? 4 : (venture_money_get_amount(prorated) < 0 ? 3 : 2);
			if (scheduled)
			{
				if (kind == 4)
					return refuse(error, VENTURE_ERROR_VALIDATION, "seat changes are immediate");
				g_object_set(sub, "pending-plan-price-id", venture_entity_get_id(next_price), NULL);
				g_clear_pointer(&prorated, venture_money_free);
				prorated = venture_money_new_zero(venture_money_get_currency(before));
			}
			else
			{
				keep_discount_for(sub, price, next_price);
				g_set_object(&price, next_price);
				g_object_set(sub, "plan-price-id", venture_entity_get_id(price), NULL);
				if (state == 0)
				{
					g_clear_pointer(&prorated, venture_money_free);
					prorated = venture_money_new_zero(venture_money_get_currency(before));
				}
			}
		}
		else if (g_strcmp0(verb, "pause") == 0 && (state == 0 || state == 1 || state == 2))
		{
			state = 3;
			kind = 5;
		}
		else if (g_strcmp0(verb, "resume") == 0 && state == 3)
		{
			state = 1;
			kind = 6;
			g_object_set(sub, "past-due-at", NULL, NULL);
		}
		else if (g_strcmp0(verb, "cancel") == 0 && state < 4)
		{
			kind = 7;
			if (scheduled)
				g_object_set(sub, "cancel-at-period-end", TRUE, NULL);
			else
			{
				g_auto(UnusedCredit) credit = { NULL, NULL, 0, 0, 0 };
				g_autoptr(VentureMoney) gross = NULL;
				if (!unused_credit(self, sub, price, at, &credit, error))
					return FALSE;
				/* What was used up to now is billed as the subscription ends. */
				if (!venture_billing_usage_bill_final(self->database, sub, at, actor, &final_usage, error))
					return FALSE;
				if (!venture_money_is_zero(credit.net))
				{
					gross = unused_gross(&credit, error);
					if (gross == NULL || !issue_unused_credit(self, sub, &credit, gross, at, actor, error))
						return FALSE;
					/* The carried difference is inside the credit now. */
					g_object_set(sub, "pending-adjustment", NULL, NULL);
					prorated = venture_money_negate(gross);
					credited = TRUE;
				}
				state = 4;
				g_object_set(sub, "cancelled-at", at, "cancel-at-period-end", FALSE, NULL);
			}
		}
		else if (g_strcmp0(verb, "mark-payment-failed") == 0 && state == 1)
		{
			state = 2;
			kind = 8;
			g_object_set(sub, "past-due-at", at, NULL);
		}
		else if (g_strcmp0(verb, "recover") == 0 && state == 2)
		{
			state = 1;
			kind = 9;
			g_object_set(sub, "past-due-at", NULL, NULL);
		}
		else if (g_strcmp0(verb, "collect") == 0 && (state == 1 || state == 2))
		{
			g_autoptr(GPtrArray) methods = NULL;
			g_autoptr(GPtrArray) events = NULL;
			g_autoptr(VentureMoney) balance = NULL;
			g_autoptr(VenturePayment) payment = NULL;
			gint64 invoice = 0;
			guint m;
			gboolean authorized = FALSE;
			methods = rows(self, VENTURE_TYPE_CUSTOMER_PAYMENT_METHOD, org, error);
			if (methods == NULL)
				return FALSE;
			for (m = 0; m < methods->len; m++)
			{
				VentureEntity *method = g_ptr_array_index(methods, m);
				g_autofree gchar *method_name = NULL;
				g_object_get(method, "method", &method_name, NULL);
				/* A stored mandate is not proof that a processor collected cash.
				 * This path records only an operator-confirmed manual receipt. */
				if (number(method, "company-id") == number(sub, "company-id") &&
					flag(method, "authorized") && g_strcmp0(method_name, "manual") == 0)
					authorized = TRUE;
			}
			if (!authorized)
				return refuse(error, VENTURE_ERROR_VALIDATION, "collection requires an authorized manual payment method; processor methods need verified settlement");
			events = rows(self, VENTURE_TYPE_SUBSCRIPTION_EVENT, org, error);
			if (events == NULL)
				return FALSE;
			for (m = events->len; m > 0; m--)
			{
				VentureEntity *history = g_ptr_array_index(events, m - 1);
				if (number(history, "subscription-id") == venture_entity_get_id(sub) && number(history, "invoice-id") > 0)
				{
					invoice = number(history, "invoice-id");
					break;
				}
			}
			if (invoice == 0)
				return refuse(error, VENTURE_ERROR_VALIDATION, "collection requires a billed invoice");
			balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database), invoice, NULL, error);
			if (balance == NULL)
				return FALSE;
			if (!venture_money_is_zero(balance))
			{
				payment = venture_payment_new();
				venture_entity_set_organization_id(VENTURE_ENTITY(payment), org);
				g_object_set(payment, "customer-id", number(sub, "company-id"), "invoice-id", invoice,
					"amount", balance, "date", at, "method", "manual", NULL);
				if (!venture_settlement_service_apply_payment(venture_settlement_service_get(self->database),
					payment, NULL, actor, error))
					return FALSE;
			}
			invoice_id = invoice;
			kind = 10;
			if (state == 2)
			{
				state = 1;
				g_object_set(sub, "past-due-at", NULL, NULL);
			}
		}
		else
			return refuse(error, VENTURE_ERROR_VALIDATION, "action is not allowed in this subscription state");
	}
	if (before == NULL)
		return FALSE;
	/* A start that bills now issues its invoice after the subscription is
	 * written, below; its MRR counts that invoice already. */
	if (old_price == 0 && state == 1)
		issuing = 1;
	after = mrr(self, sub, price, seats, state, issuing, error);
	if (after == NULL)
		return FALSE;
	if (prorated == NULL)
		prorated = venture_money_new_zero(venture_money_get_currency(after));
	/* A change carries its difference to the next invoice; a cancellation
	 * has none, and has credited its difference already. */
	if (!credited && !venture_money_is_zero(prorated))
	{
		g_autoptr(VentureMoney) pending = NULL;
		g_autoptr(VentureMoney) combined = NULL;
		g_object_get(sub, "pending-adjustment", &pending, NULL);
		if (pending == NULL)
			pending = venture_money_new_zero(venture_money_get_currency(prorated));
		combined = venture_money_add(pending, prorated, error);
		if (combined == NULL)
			return FALSE;
		g_object_set(sub, "pending-adjustment", combined, NULL);
	}
	g_object_set(sub, "status", state, "seats", seats, "last-event-at", at, NULL);
	/* Insertion gives the event a real referent; veto still rolls it back. */
	if (!write_record(self, sub, actor, error))
		return FALSE;
	if (old_price == 0 && state == 1 && !issue(self, sub, price, at, start, actor, &invoice_id, error))
		return FALSE;
	event = new_record(VENTURE_TYPE_SUBSCRIPTION_EVENT, org);
	g_object_set(event, "subscription-id", venture_entity_get_id(sub), "kind", kind, "at", at,
		"from-plan-price-id", old_price, "to-plan-price-id", venture_entity_get_id(price),
		"from-seats", old_seats, "to-seats", seats, "proration-amount", prorated, "invoice-id", invoice_id,
		"from-status", old_state, "to-status", state, "from-mrr", before, "to-mrr", after,
		"period-start", start, "period-end", end, NULL);
	snapshot = g_object_new(VENTURE_TYPE_SUBSCRIPTION_EVENT, NULL);
	venture_entity_copy_properties_from(snapshot, event, FALSE);
	/* Veto subscribers may inspect this change, but cannot borrow its consent. */
	venture_accounting_operation_suspend(self->database);
	g_signal_emit_by_name(self, "changing", snapshot, &veto);
	venture_accounting_operation_resume(self->database);
	if (veto != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&veto));
		return FALSE;
	}
	if (!write_record(self, event, actor, error))
		return FALSE;
	if ((g_strcmp0(verb, "change") == 0 || g_strcmp0(verb, "change-seats") == 0) &&
		!price_change_notice(self, sub, event, old_price, old_seats, scheduled, actor, error))
		return FALSE;
	/* The request reports the invoice it caused, the final usage invoice
	 * included; only the event must never name the latter. */
	g_object_set(request, "subscription-id", venture_entity_get_id(sub), "invoice-id",
		invoice_id != 0 ? invoice_id : final_usage,
		"proration-amount", prorated, "processed", (gint64)1, NULL);
	return TRUE;
}

static gboolean
sweep(VentureBillingService *self, VentureEntity *request, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) subscriptions = NULL;
	g_autoptr(GPtrArray) steps = NULL;
	g_autoptr(GDateTime) at = NULL;
	g_autofree gchar *verb = NULL;
	gint64 org = venture_entity_get_organization_id(request);
	gint64 processed = 0;
	guint i;
	gboolean dry = flag(request, "dry-run");
	g_object_get(request, "action", &verb, "at", &at, NULL);
	subscriptions = rows(self, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, org, error);
	if (subscriptions == NULL)
		return FALSE;
	if (g_strcmp0(verb, "dunning-sweep") == 0)
	{
		steps = rows(self, VENTURE_TYPE_DUNNING_STEP, org, error);
		if (steps == NULL)
			return FALSE;
	}
	for (i = 0; i < subscriptions->len; i++)
	{
		VentureEntity *sub = g_ptr_array_index(subscriptions, i);
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(VentureEntity) action = NULL;
		gint state = choice(sub, "status");
		gint64 id = venture_entity_get_id(sub);
		if (steps == NULL)
		{
			if (!dry && !trial_reminder(self, sub, at, actor, error))
				return FALSE;
			if (state != 0 && state != 1 && !(state < 4 && flag(sub, "cancel-at-period-end")))
				continue;
			g_object_get(sub, "current-period-end", &date, NULL);
			if (date == NULL || g_date_time_compare(date, at) > 0)
				continue;
			{
				g_autoptr(VentureEntity) current = g_object_ref(sub);
				guint periods = 0;
				while (g_date_time_compare(date, at) <= 0)
				{
					g_autoptr(VentureEntity) price = NULL;
					GDateTime *next;
					if (++periods > 1200)
						return refuse(error, VENTURE_ERROR_VALIDATION, "renewal sweep exceeds 1200 periods per subscription");
					if (!dry)
					{
						g_clear_object(&action);
						action = new_record(VENTURE_TYPE_BILLING_REQUEST, org);
						g_object_set(action, "action", "renew", "subscription-id", id, "at", at, NULL);
						if (!perform(self, action, actor, error))
							return FALSE;
						g_clear_object(&current);
						current = load(self, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, id, org, error);
						if (current == NULL)
							return FALSE;
					}
					processed++;
					if (flag(current, "cancel-at-period-end") || choice(current, "status") >= 4)
						break;
					if (dry)
					{
						gint64 price_id = number(current, "pending-plan-price-id");
						price = load(self, VENTURE_TYPE_PLAN_PRICE,
							price_id != 0 ? price_id : number(current, "plan-price-id"), org, error);
						if (price == NULL)
							return FALSE;
						next = anchored_period(current, price, date);
						g_clear_pointer(&date, g_date_time_unref);
						date = next;
					}
					else
					{
						g_clear_pointer(&date, g_date_time_unref);
						g_object_get(current, "current-period-end", &date, NULL);
					}
				}
			}
		}
		else if (state == 2 || state == 3)
		{
			guint j;
			g_object_get(sub, "past-due-at", &date, NULL);
			if (date == NULL)
				continue;
			for (j = 0; j < steps->len; j++)
			{
				VentureEntity *step = g_ptr_array_index(steps, j);
				g_autofree gchar *stamp = NULL;
				g_autofree gchar *key = NULL;
				g_autoptr(VentureQuery) q = NULL;
				g_autoptr(VentureEntity) notice = NULL;
				gint action_kind = choice(step, "action");
				if (!flag(step, "active") || g_date_time_difference(at, date) / G_TIME_SPAN_DAY < number(step, "day-offset"))
					continue;
				stamp = g_date_time_format_iso8601(date);
				key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s", id, venture_entity_get_id(step), stamp);
				q = venture_query_new(VENTURE_TYPE_BILLING_NOTICE);
				venture_query_set_organization(q, org);
				if (!venture_query_add_filter_string(q, "delivery-key", VENTURE_FILTER_OP_EQ, key, error))
					return FALSE;
				if (venture_database_count(self->database, q, error) > 0)
					continue;
				if (!dry)
				{
					notice = new_record(VENTURE_TYPE_BILLING_NOTICE, org);
					g_object_set(notice, "subscription-id", id, "dunning-step-id", venture_entity_get_id(step),
						"channel", "email", "at", at, "past-due-at", date, "delivery-key", key, NULL);
					if (!write_record(self, notice, actor, error))
						return FALSE;
					if (action_kind == 3 || (action_kind == 2 && state == 2))
					{
						g_clear_object(&action);
						action = new_record(VENTURE_TYPE_BILLING_REQUEST, org);
						g_object_set(action, "action", action_kind == 2 ? "pause" : "cancel", "subscription-id", id, "at", at, NULL);
						if (!perform(self, action, actor, error))
							return FALSE;
					}
				}
				processed++;
				if (action_kind == 2)
					state = 3;
				if (action_kind == 3)
					break;
			}
		}
	}
	g_object_set(request, "processed", processed, NULL);
	return TRUE;
}

gboolean
venture_billing_service_execute(VentureBillingService *self, VentureBillingRequest *request,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) copy = NULL;
	g_autofree gchar *verb = NULL;
	gboolean ok;
	gboolean can_post;
	if (self->database == NULL || self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "service is unavailable or already executing");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "customer_subscription") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_VALIDATION, "billing module is disabled");
	if (venture_entity_is_persisted(VENTURE_ENTITY(request)))
		return refuse(error, VENTURE_ERROR_VALIDATION, "completed requests are immutable");
	copy = g_object_new(VENTURE_TYPE_BILLING_REQUEST, NULL);
	venture_entity_copy_properties_from(copy, VENTURE_ENTITY(request), FALSE);
	if (!venture_entity_validate(copy, error))
		return FALSE;
	if (venture_entity_get_organization_id(copy) <= 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "a valid instruction and one organization are required");
	g_object_get(copy, "action", &verb, NULL);
	if (flag(copy, "dry-run") && g_strcmp0(verb, "renew-sweep") != 0 && g_strcmp0(verb, "dunning-sweep") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "dry-run is available for sweeps only");
	/* Changes to future terms and dunning state do not themselves post money. */
	can_post = g_strcmp0(verb, "renew") == 0 || g_strcmp0(verb, "renew-sweep") == 0 ||
		g_strcmp0(verb, "collect") == 0 ||
		/* Cancelling now credits the unused days of the period. */
		(g_strcmp0(verb, "cancel") == 0 && !flag(copy, "at-period-end"));
	if (g_strcmp0(verb, "start") == 0)
	{
		g_autoptr(VentureEntity) price = load(self, VENTURE_TYPE_PLAN_PRICE,
			number(copy, "plan-price-id"), venture_entity_get_organization_id(copy), error);
		if (price == NULL) return FALSE;
		can_post = number(price, "trial-days") == 0;
	}
	if (!flag(copy, "dry-run") && can_post)
	{
		operation = venture_accounting_operation_begin(self->database, "billing.execute", copy, NULL,
			NULL, venture_entity_get_organization_id(copy), actor, error);
		if (operation == NULL)
			return FALSE;
	}
	if (!venture_database_begin(self->database, error))
		return FALSE;
	self->busy = TRUE;
	ok = g_strcmp0(verb, "renew-sweep") == 0 || g_strcmp0(verb, "dunning-sweep") == 0 ?
		sweep(self, copy, actor, error) : perform(self, copy, actor, error);
	if (ok && !flag(copy, "dry-run"))
		ok = write_record(self, copy, actor, error);
	if (ok)
		ok = venture_database_commit(self->database, error);
	else
		venture_database_rollback(self->database);
	self->busy = FALSE;
	if (ok && operation != NULL)
		ok = venture_accounting_operation_finish(operation, error);
	if (ok)
		venture_entity_copy_properties_from(VENTURE_ENTITY(request), copy, FALSE);
	return ok;
}

gboolean
venture_billing_service_move_customers(VentureBillingService *self, VenturePlanPrice *from, VenturePlanPrice *to,
	gboolean at_period_end, GDateTime *at, const VentureActor *actor, guint *moved, GError **error)
{
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(GPtrArray) subs = NULL;
	VentureEntity *old_price = VENTURE_ENTITY(from);
	VentureEntity *new_price = VENTURE_ENTITY(to);
	gint64 org = venture_entity_get_organization_id(old_price);
	guint i, count = 0;

	g_return_val_if_fail(VENTURE_IS_BILLING_SERVICE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_PLAN_PRICE(from) && VENTURE_IS_PLAN_PRICE(to), FALSE);
	g_return_val_if_fail(at != NULL, FALSE);
	if (moved != NULL)
		*moved = 0;
	if (self->database == NULL || self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "service is unavailable or already executing");
	if (venture_entity_get_organization_id(new_price) != org ||
		number(old_price, "plan-id") != number(new_price, "plan-id") ||
		venture_entity_get_id(old_price) == venture_entity_get_id(new_price))
		return refuse(error, VENTURE_ERROR_VALIDATION, "customers move to another price of the same plan");
	q = venture_query_new(VENTURE_TYPE_CUSTOMER_SUBSCRIPTION);
	venture_query_set_organization(q, org);
	venture_query_set_limit(q, 0);
	if (!venture_query_add_filter_int(q, "plan-price-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(old_price), error) ||
		!venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	subs = venture_database_find(self->database, q, error);
	if (subs == NULL)
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	for (i = 0; i < subs->len; i++)
	{
		VentureEntity *sub = g_ptr_array_index(subs, i);
		g_autoptr(VentureEntity) instruction = NULL;
		g_autoptr(GError) refusal = NULL;
		gint state = choice(sub, "status");
		/* Trialing, active and past due: the customers still on it. A
		 * paused subscription cannot change terms and stays put. */
		if (state > 2)
			continue;
		instruction = new_record(VENTURE_TYPE_BILLING_REQUEST, org);
		g_object_set(instruction, "action", "change", "subscription-id", venture_entity_get_id(sub),
			"plan-price-id", venture_entity_get_id(new_price), "at-period-end", at_period_end, "at", at,
			"expected-version", venture_entity_get_version(sub), NULL);
		if (!venture_billing_service_execute(self, VENTURE_BILLING_REQUEST(instruction), actor, &refusal))
		{
			g_autoptr(VentureEntity) customer = NULL;
			g_autofree gchar *name = NULL;
			const gchar *why = refusal->message;
			/* The refusal abandoned the transaction; read the name after it. */
			venture_database_rollback(self->database);
			customer = venture_database_get(self->database, VENTURE_TYPE_COMPANY, number(sub, "company-id"), NULL);
			name = customer != NULL ? venture_entity_get_display_name(customer)
				: g_strdup_printf("subscription #%" G_GINT64_FORMAT, venture_entity_get_id(sub));
			if (g_str_has_prefix(why, "VentureBillingService: "))
				why += strlen("VentureBillingService: ");
			g_set_error(error, VENTURE_ERROR, refusal->code,
				"Nobody was moved: %s could not be moved (%s)", name, why);
			return FALSE;
		}
		count++;
	}
	if (!venture_database_commit(self->database, error))
		return FALSE;
	if (moved != NULL)
		*moved = count;
	return TRUE;
}

static gboolean
price_used(VentureDatabase *db, VentureEntity *price, gboolean *used, GError **error)
{
	static const gchar *const fields[] = { "plan-price-id", "pending-plan-price-id", "from-plan-price-id", "to-plan-price-id" };
	guint i;
	*used = FALSE;
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
	{
		g_autoptr(VentureQuery) q = venture_query_new(i < 2 ? VENTURE_TYPE_CUSTOMER_SUBSCRIPTION : VENTURE_TYPE_SUBSCRIPTION_EVENT);
		gint64 count;
		venture_query_set_organization(q, venture_entity_get_organization_id(price));
		venture_query_set_include_deleted(q, TRUE);
		if (!venture_query_add_filter_int(q, fields[i], VENTURE_FILTER_OP_EQ, venture_entity_get_id(price), error))
			return FALSE;
		count = venture_database_count(db, q, error);
		if (count < 0)
			return FALSE;
		if (count > 0)
		{
			*used = TRUE;
			return TRUE;
		}
	}
	return TRUE;
}

gboolean
venture_billing_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VentureBillingService *self;
	GType type = G_OBJECT_TYPE(record);
	*handled = FALSE;
	if (type != VENTURE_TYPE_CUSTOMER_SUBSCRIPTION && type != VENTURE_TYPE_SUBSCRIPTION_EVENT &&
		type != VENTURE_TYPE_BILLING_NOTICE && type != VENTURE_TYPE_BILLING_REQUEST &&
		type != VENTURE_TYPE_PLAN_PRICE && type != VENTURE_TYPE_PLAN && type != VENTURE_TYPE_DUNNING_STEP &&
		type != VENTURE_TYPE_USAGE_RECORD)
		return TRUE;
	if (type == VENTURE_TYPE_USAGE_RECORD)
		return venture_billing_usage_check_save(database, record, error);
	self = venture_billing_service_get(database);
	if (self->writing == record)
	{
		self->writing = NULL;
		return TRUE;
	}
	if (type == VENTURE_TYPE_BILLING_REQUEST)
	{
		*handled = TRUE;
		return venture_billing_service_execute(self, VENTURE_BILLING_REQUEST(record), actor, error);
	}
	if (type == VENTURE_TYPE_CUSTOMER_SUBSCRIPTION || type == VENTURE_TYPE_SUBSCRIPTION_EVENT || type == VENTURE_TYPE_BILLING_NOTICE)
		return refuse(error, VENTURE_ERROR_VALIDATION, "subscription state and history may only be written through the service");
	if (type == VENTURE_TYPE_PLAN_PRICE && venture_entity_is_persisted(record))
	{
		g_autoptr(VentureEntity) previous = venture_database_get(database, type, venture_entity_get_id(record), error);
		g_autoptr(JsonNode) diff = NULL;
		gboolean used;
		if (previous == NULL || !price_used(database, previous, &used, error))
			return FALSE;
		g_object_set(previous, "active", flag(record, "active"), NULL);
		diff = venture_entity_diff(previous, record);
		if (used && (json_object_get_size(json_node_get_object(diff)) != 0 ||
			venture_entity_get_organization_id(previous) != venture_entity_get_organization_id(record)))
			return refuse(error, VENTURE_ERROR_VALIDATION, "referenced prices are immutable; create a new price version");
	}
	if (type == VENTURE_TYPE_PLAN_PRICE)
	{
		g_autoptr(VentureMoney) amount = price_amount(record, 1, error);
		g_autoptr(VentureEntity) plan = NULL;
		if (amount == NULL)
			return FALSE;
		plan = load(self, VENTURE_TYPE_PLAN, number(record, "plan-id"), venture_entity_get_organization_id(record), error);
		if (plan == NULL)
			return FALSE;
		if (number(record, "trial-days") < 0 || number(record, "trial-days") > 366)
			return refuse(error, VENTURE_ERROR_VALIDATION, "trial days must be between zero and 366");
		if (!venture_billing_price_check_metering(record, error))
			return FALSE;
	}
	if (type == VENTURE_TYPE_DUNNING_STEP && number(record, "day-offset") < 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "dunning offset must be nonnegative");
	return TRUE;
}

gboolean
venture_billing_check_removal(VentureDatabase *database, VentureEntity *record, GError **error)
{
	GType type = G_OBJECT_TYPE(record);
	if (type == VENTURE_TYPE_USAGE_RECORD)
		return venture_billing_usage_check_removal(database, record, error);
	if (type == VENTURE_TYPE_PLAN_PRICE)
	{
		gboolean used;
		if (!price_used(database, record, &used, error))
			return FALSE;
		if (used)
			return refuse(error, VENTURE_ERROR_VALIDATION, "referenced prices cannot be removed");
	}
	if (type == VENTURE_TYPE_CUSTOMER_SUBSCRIPTION || type == VENTURE_TYPE_SUBSCRIPTION_EVENT ||
		type == VENTURE_TYPE_BILLING_NOTICE || type == VENTURE_TYPE_BILLING_REQUEST)
		return refuse(error, VENTURE_ERROR_VALIDATION, "billing history cannot be deleted, restored or purged");
	return TRUE;
}

gboolean
venture_billing_prepare_request(VentureBillingService *self, VentureBillingRequest *request, GError **error)
{
	g_autoptr(VentureEntity) sub = NULL;
	VentureEntity *e = VENTURE_ENTITY(request);
	gint64 id = number(e, "subscription-id");
	gint64 expected = number(e, "expected-version");
	if (id == 0)
		return TRUE;
	sub = load(self, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, id, venture_entity_get_organization_id(e), error);
	if (sub == NULL)
		return FALSE;
	if (expected != 0 && expected != venture_entity_get_version(sub))
		return refuse(error, VENTURE_ERROR_CONFLICT, "subscription changed before this action could be staged");
	g_object_set(e, "expected-version", venture_entity_get_version(sub), NULL);
	return TRUE;
}

VentureMoney *
venture_billing_service_next_invoice(VentureBillingService *self, VentureCustomerSubscription *subscription,
	GError **error)
{
	VentureEntity *sub = VENTURE_ENTITY(subscription);
	g_return_val_if_fail(VENTURE_IS_BILLING_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_SUBSCRIPTION(subscription), NULL);
	return preview_next_invoice(self, sub, venture_entity_get_organization_id(sub), error);
}

VentureEntity *
venture_billing_service_trial_reminder(VentureBillingService *self, VentureCustomerSubscription *subscription)
{
	g_autoptr(VentureQuery) q = NULL;
	g_autofree gchar *key = NULL;
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message");
	g_return_val_if_fail(VENTURE_IS_BILLING_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_SUBSCRIPTION(subscription), NULL);
	if (type == G_TYPE_INVALID || self->database == NULL)
		return NULL;
	key = trial_reminder_key(VENTURE_ENTITY(subscription));
	q = venture_query_new(type);
	venture_query_set_organization(q, venture_entity_get_organization_id(VENTURE_ENTITY(subscription)));
	venture_query_set_include_deleted(q, TRUE);
	if (!venture_query_add_filter_string(q, "idempotency-key", VENTURE_FILTER_OP_EQ, key, NULL))
		return NULL;
	return venture_database_find_one(self->database, q, NULL);
}
