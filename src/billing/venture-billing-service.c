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
};
G_DEFINE_FINAL_TYPE(VentureBillingService, venture_billing_service, G_TYPE_OBJECT)

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
	(void)self;
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

static VentureMoney *
mrr(VentureEntity *price, gint64 seats, gint state, GError **error)
{
	g_autoptr(VentureMoney) total = price_amount(price, seats, error);
	if (total == NULL)
		return NULL;
	if (state != 1 && state != 2)
		return venture_money_new_zero(venture_money_get_currency(total));
	return venture_money_multiply_rational(total, 1, choice(price, "interval") == 1 ? 12 : 1, error);
}

static GDateTime *
next_period(VentureEntity *price, GDateTime *at)
{
	return g_date_time_add_months(at, choice(price, "interval") == 1 ? 12 : 1);
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
	return g_date_time_add_months(anchor, months + (choice(price, "interval") == 1 ? 12 : 1));
}

static gboolean
price_available(VentureBillingService *self, VentureEntity *price, gint64 org, GError **error)
{
	g_autoptr(VentureEntity) plan = load(self, VENTURE_TYPE_PLAN, number(price, "plan-id"), org, error);
	if (plan == NULL)
		return FALSE;
	if (!flag(price, "active") || !flag(plan, "active"))
		return refuse(error, VENTURE_ERROR_VALIDATION, "plan and price must be active");
	return TRUE;
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
	g_autoptr(GPtrArray) parts = NULL;
	VentureMoney *total;
	gint64 days;
	gint64 elapsed;
	gint period_months;
	gint64 i;
	if (old_amount == NULL)
		return NULL;
	new_amount = price_amount(new_price, new_seats, error);
	if (new_amount == NULL)
		return NULL;
	/* A price change does not change the period already being served. */
	period_months = (g_date_time_get_year(end) - g_date_time_get_year(start)) * 12 +
		g_date_time_get_month(end) - g_date_time_get_month(start);
	period_months = period_months == 12 ? 12 : 1;
	normalized_old = venture_money_multiply_rational(old_amount, period_months,
		choice(old_price, "interval") == 1 ? 12 : 1, error);
	if (normalized_old == NULL)
		return NULL;
	comparable = venture_money_multiply_rational(new_amount, period_months,
		choice(new_price, "interval") == 1 ? 12 : 1, error);
	if (comparable == NULL)
		return NULL;
	delta = venture_money_subtract(comparable, normalized_old, error);
	if (delta == NULL)
		return NULL;
	days = g_date_time_difference(end, start) / G_TIME_SPAN_DAY;
	elapsed = g_date_time_difference(at, start) / G_TIME_SPAN_DAY;
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
	return total;
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
	gint64 org = venture_entity_get_organization_id(sub);
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
		self->database, org, at, error))
		return FALSE;
	amount = price_amount(price, number(sub, "seats"), error);
	if (amount == NULL)
		return FALSE;
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
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Subscription renewal",
		"quantity", 1.0, "unit-price", amount, "product-id", number(price, "product-id"), NULL);
	if (!venture_database_save(self->database, line, actor, error) ||
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
	gint state;
	gint old_state;
	gint kind = 0;
	gboolean scheduled = flag(request, "at-period-end");
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
		if (price == NULL || !price_available(self, price, org, error))
			return FALSE;
		seats = number(request, "seats");
		if (seats == 0)
			seats = 1;
		trial = number(price, "trial-days");
		if (trial < 0 || trial > 366)
			return refuse(error, VENTURE_ERROR_VALIDATION, "trial days must be between zero and 366");
		state = trial > 0 ? 0 : 1;
		sub = new_record(VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, org);
		g_object_get(request, "external-id", &external, NULL);
		start = g_date_time_ref(at);
		end = trial > 0 ? g_date_time_add_days(at, (gint)trial) : next_period(price, at);
		g_object_set(sub, "company-id", number(request, "company-id"), "contact-id", number(request, "contact-id"),
			"plan-price-id", venture_entity_get_id(price), "external-id", external,
			"current-period-start", start, "current-period-end", end, "trial-end", trial > 0 ? end : NULL, "billing-anchor", trial > 0 ? end : start, NULL);
		old_seats = 0;
		old_price = 0;
		old_state = 0;
		before = mrr(price, seats, 0, error);
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
		before = mrr(price, seats, state, error);
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
			}
		}
		else if (g_strcmp0(verb, "change") == 0 || g_strcmp0(verb, "change-seats") == 0)
		{
			if (state != 0 && state != 1 && state != 2)
				return refuse(error, VENTURE_ERROR_VALIDATION, "only live subscriptions may change terms");
			next_price = g_strcmp0(verb, "change") == 0 ?
				load(self, VENTURE_TYPE_PLAN_PRICE, number(request, "plan-price-id"), org, error) : g_object_ref(price);
			if (next_price == NULL || !price_available(self, next_price, org, error))
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
	after = mrr(price, seats, state, error);
	if (after == NULL)
		return FALSE;
	if (prorated == NULL)
		prorated = venture_money_new_zero(venture_money_get_currency(after));
	if (!venture_money_is_zero(prorated))
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
	g_object_set(request, "subscription-id", venture_entity_get_id(sub), "invoice-id", invoice_id,
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
		g_strcmp0(verb, "collect") == 0;
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
		type != VENTURE_TYPE_PLAN_PRICE && type != VENTURE_TYPE_PLAN && type != VENTURE_TYPE_DUNNING_STEP)
		return TRUE;
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
	}
	if (type == VENTURE_TYPE_DUNNING_STEP && number(record, "day-offset") < 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "dunning offset must be nonnegative");
	return TRUE;
}

gboolean
venture_billing_check_removal(VentureDatabase *database, VentureEntity *record, GError **error)
{
	GType type = G_OBJECT_TYPE(record);
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
