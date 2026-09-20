/*
 * venture-customer-health.c - Customer health: the list behind the churn number
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Nothing here is stored. A company's health is read from the records the
 * other modules keep -- interactions, activities, inbound mail, deals,
 * invoices, tickets, reminder events, receipts -- every time it is asked
 * for, against the thresholds the organisation has now. A stored band
 * would be wrong the morning after the thresholds changed; a computed one
 * cannot be. The module's one write is the sweep, and that writes an
 * ordinary activity through the ordinary save.
 */

#include "venture.h"
#include "report/venture-headline-private.h"

#include <string.h>

#define HEALTH_MODULE "customer_health"
#define HEALTH_DEFAULT_TOUCH_DAYS 30
#define HEALTH_DEFAULT_OVERDUE_DAYS 15
#define HEALTH_DEFAULT_OPEN_TICKETS 3
#define HEALTH_SWEEP_DEFAULT_LIMIT 100
#define HEALTH_SWEEP_MAX_LIMIT 1000

/* --- Bands ---------------------------------------------------------------- */

const gchar *
venture_health_band_to_string(VentureHealthBand band)
{
	switch (band)
	{
	case VENTURE_HEALTH_BAND_RED:
		return "red";
	case VENTURE_HEALTH_BAND_AMBER:
		return "amber";
	case VENTURE_HEALTH_BAND_GREEN:
	default:
		return "green";
	}
}

gboolean
venture_health_band_from_string(
	const gchar		*text,
	VentureHealthBand	*out_band
){
	g_return_val_if_fail(NULL != out_band, FALSE);

	if (0 == g_strcmp0(text, "green"))
		*out_band = VENTURE_HEALTH_BAND_GREEN;
	else if (0 == g_strcmp0(text, "amber"))
		*out_band = VENTURE_HEALTH_BAND_AMBER;
	else if (0 == g_strcmp0(text, "red"))
		*out_band = VENTURE_HEALTH_BAND_RED;
	else
		return FALSE;

	return TRUE;
}

/* --- The reading ---------------------------------------------------------- */

struct _VentureCustomerHealth
{
	GObject		 parent_instance;

	gint64		 company_id;
	gchar		*company_name;
	gchar		*owner;
	GDateTime	*last_touch;
	gint64		 days_since_touch;
	gint64		 open_deals;
	gint64		 overdue_invoices;
	gint64		 overdue_days;
	gint64		 open_tickets;
	gint64		 sla_breaches;
	gint64		 dunning_step;
	VentureMoney	*revenue_12m;
	guint		 flags;
	GPtrArray	*reasons;
};

G_DEFINE_FINAL_TYPE(VentureCustomerHealth, venture_customer_health, G_TYPE_OBJECT)

static void
venture_customer_health_finalize(GObject *object)
{
	VentureCustomerHealth *self = VENTURE_CUSTOMER_HEALTH(object);

	g_clear_pointer(&self->company_name, g_free);
	g_clear_pointer(&self->owner, g_free);
	g_clear_pointer(&self->last_touch, g_date_time_unref);
	g_clear_pointer(&self->revenue_12m, venture_money_free);
	g_clear_pointer(&self->reasons, g_ptr_array_unref);

	G_OBJECT_CLASS(venture_customer_health_parent_class)->finalize(object);
}

static void
venture_customer_health_class_init(VentureCustomerHealthClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_customer_health_finalize;
}

static void
venture_customer_health_init(VentureCustomerHealth *self)
{
	self->reasons = g_ptr_array_new_with_free_func(g_free);
}

gint64
venture_customer_health_get_company_id(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), 0);
	return self->company_id;
}

const gchar *
venture_customer_health_get_company_name(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), NULL);
	return self->company_name;
}

const gchar *
venture_customer_health_get_owner(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), NULL);
	return self->owner;
}

GDateTime *
venture_customer_health_get_last_touch(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), NULL);
	return self->last_touch;
}

gint64
venture_customer_health_get_days_since_touch(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), 0);
	return self->days_since_touch;
}

gint64
venture_customer_health_get_open_deals(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), 0);
	return self->open_deals;
}

gint64
venture_customer_health_get_overdue_invoices(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), 0);
	return self->overdue_invoices;
}

gint64
venture_customer_health_get_overdue_days(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), 0);
	return self->overdue_days;
}

gint64
venture_customer_health_get_open_tickets(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), 0);
	return self->open_tickets;
}

gint64
venture_customer_health_get_sla_breaches(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), 0);
	return self->sla_breaches;
}

gint64
venture_customer_health_get_dunning_step(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), 0);
	return self->dunning_step;
}

const VentureMoney *
venture_customer_health_get_revenue_12m(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), NULL);
	return self->revenue_12m;
}

guint
venture_customer_health_get_flags(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), 0);
	return self->flags;
}

VentureHealthBand
venture_customer_health_get_band(VentureCustomerHealth *self)
{
	guint tripped;

	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), VENTURE_HEALTH_BAND_GREEN);

	tripped = ((self->flags & VENTURE_HEALTH_FLAG_QUIET) ? 1 : 0) +
	          ((self->flags & VENTURE_HEALTH_FLAG_OVERDUE) ? 1 : 0) +
	          ((self->flags & VENTURE_HEALTH_FLAG_TICKETS) ? 1 : 0);

	if (0 == tripped)
		return VENTURE_HEALTH_BAND_GREEN;

	return (1 == tripped) ? VENTURE_HEALTH_BAND_AMBER : VENTURE_HEALTH_BAND_RED;
}

GPtrArray *
venture_customer_health_get_reasons(VentureCustomerHealth *self)
{
	g_return_val_if_fail(VENTURE_IS_CUSTOMER_HEALTH(self), NULL);
	return self->reasons;
}

/* --- Thresholds ----------------------------------------------------------- */

void
venture_customer_health_thresholds(
	VentureContext		*context,
	gint64			 organization_id,
	VentureHealthThresholds	*out_thresholds
){
	g_autoptr(VentureEntity) setting = NULL;
	gint64 touch = 0;
	gint64 overdue = 0;
	gint64 tickets = 0;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_return_if_fail(NULL != out_thresholds);

	if (0 == organization_id)
		organization_id = venture_context_get_default_organization_id(context);

	setting = venture_headline_setting_find(venture_context_get_database(context),
	                                        organization_id);

	if (NULL != setting)
		g_object_get(setting, "health-touch-days", &touch,
		             "health-overdue-days", &overdue,
		             "health-open-tickets", &tickets, NULL);

	out_thresholds->touch_days = (touch > 0) ? touch : HEALTH_DEFAULT_TOUCH_DAYS;
	out_thresholds->overdue_days = (overdue > 0) ? overdue : HEALTH_DEFAULT_OVERDUE_DAYS;
	out_thresholds->open_tickets = (tickets > 0) ? tickets : HEALTH_DEFAULT_OPEN_TICKETS;
}

/* --- Reading the records -------------------------------------------------- */

/*
 * Every table is read once for the organisation and bucketed by company,
 * so a report over a thousand customers costs the same handful of queries
 * as a report over one. A module that is off contributes nothing: its
 * type is not registered, and the fetch returns an empty array.
 */
static GPtrArray *
health_fetch(
	VentureContext	 *context,
	const gchar	 *type_name,
	gint64		  organization_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	GType type;

	type = venture_entity_registry_lookup(venture_context_get_entity_registry(context),
	                                      type_name);

	if (G_TYPE_INVALID == type)
		return g_ptr_array_new_with_free_func(g_object_unref);

	query = venture_query_new(type);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	return venture_database_find(venture_context_get_database(context), query, error);
}

static gint64
health_int(
	VentureEntity	*record,
	const gchar	*property
){
	gint64 value = 0;

	g_object_get(record, property, &value, NULL);

	return value;
}

static gint
health_enum(
	VentureEntity	*record,
	const gchar	*property
){
	gint value = 0;

	g_object_get(record, property, &value, NULL);

	return value;
}

static GDateTime *
health_when(
	VentureEntity	*record,
	const gchar	*property
){
	GDateTime *value = NULL;

	g_object_get(record, property, &value, NULL);

	return value;
}

/* Whole days from @since to @until; never negative. */
static gint64
health_days(
	GDateTime	*since,
	GDateTime	*until
){
	GTimeSpan span;

	if ((NULL == since) || (NULL == until))
		return 0;

	span = g_date_time_difference(until, since);

	return (span > 0) ? (gint64)(span / G_TIME_SPAN_DAY) : 0;
}

/* Keeps the later of two touches, ignoring one after the reading's date:
 * a note written tomorrow does not make the customer contacted today. */
static void
health_touch(
	GHashTable	*touches,
	gint64		 company_id,
	GDateTime	*when,
	GDateTime	*as_of
){
	GDateTime *known;

	if ((0 == company_id) || (NULL == when))
		return;

	if (g_date_time_compare(when, as_of) > 0)
		return;

	known = g_hash_table_lookup(touches, GINT_TO_POINTER((gint)company_id));

	if ((NULL == known) || (g_date_time_compare(when, known) > 0))
		g_hash_table_insert(touches, GINT_TO_POINTER((gint)company_id),
		                    g_date_time_ref(when));
}

typedef struct
{
	GHashTable	*touches;		/* company id -> GDateTime */
	GHashTable	*open_deals;		/* company id -> count */
	GHashTable	*overdue_count;		/* company id -> count */
	GHashTable	*overdue_days;		/* company id -> oldest */
	GHashTable	*open_invoices;		/* invoice id -> company id */
	GHashTable	*open_tickets;		/* company id -> count */
	GHashTable	*sla_breaches;		/* company id -> count */
	GHashTable	*dunning_step;		/* company id -> furthest step */
	VentureHeadlineSnapshot	*snapshot;	/* paid revenue, the headline way */
	GHashTable	*usernames;		/* user id -> username */
	GDateTime	*as_of;
	GDateTime	*window_start;
} HealthTables;

static void
health_tables_free(HealthTables *tables)
{
	g_clear_pointer(&tables->touches, g_hash_table_unref);
	g_clear_pointer(&tables->open_deals, g_hash_table_unref);
	g_clear_pointer(&tables->overdue_count, g_hash_table_unref);
	g_clear_pointer(&tables->overdue_days, g_hash_table_unref);
	g_clear_pointer(&tables->open_invoices, g_hash_table_unref);
	g_clear_pointer(&tables->open_tickets, g_hash_table_unref);
	g_clear_pointer(&tables->sla_breaches, g_hash_table_unref);
	g_clear_pointer(&tables->dunning_step, g_hash_table_unref);
	g_clear_pointer(&tables->snapshot, venture_headline_snapshot_free);
	g_clear_pointer(&tables->usernames, g_hash_table_unref);
	g_clear_pointer(&tables->as_of, g_date_time_unref);
	g_clear_pointer(&tables->window_start, g_date_time_unref);
}

static void
health_count(
	GHashTable	*table,
	gint64		 company_id,
	gint64		 by
){
	gint64 current;

	if (0 == company_id)
		return;

	current = GPOINTER_TO_INT(g_hash_table_lookup(table, GINT_TO_POINTER((gint)company_id)));
	g_hash_table_insert(table, GINT_TO_POINTER((gint)company_id),
	                    GINT_TO_POINTER((gint)(current + by)));
}

static void
health_max(
	GHashTable	*table,
	gint64		 company_id,
	gint64		 value
){
	gint64 current;

	if (0 == company_id)
		return;

	current = GPOINTER_TO_INT(g_hash_table_lookup(table, GINT_TO_POINTER((gint)company_id)));

	if (value > current)
		g_hash_table_insert(table, GINT_TO_POINTER((gint)company_id),
		                    GINT_TO_POINTER((gint)value));
}

static gint64
health_lookup(
	GHashTable	*table,
	gint64		 company_id
){
	return GPOINTER_TO_INT(g_hash_table_lookup(table, GINT_TO_POINTER((gint)company_id)));
}

static gboolean
health_tables_load(
	VentureContext	 *context,
	gint64		  organization_id,
	GDateTime	 *as_of,
	HealthTables	 *tables,
	GError		**error
){
	g_autoptr(GPtrArray) interactions = NULL;
	g_autoptr(GPtrArray) activities = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	g_autoptr(GPtrArray) contacts = NULL;
	g_autoptr(GPtrArray) deals = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GHashTable) contact_company = NULL;
	guint i;

	memset(tables, 0, sizeof(*tables));
	tables->touches = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                        (GDestroyNotify)g_date_time_unref);
	tables->open_deals = g_hash_table_new(g_direct_hash, g_direct_equal);
	tables->overdue_count = g_hash_table_new(g_direct_hash, g_direct_equal);
	tables->overdue_days = g_hash_table_new(g_direct_hash, g_direct_equal);
	tables->open_invoices = g_hash_table_new(g_direct_hash, g_direct_equal);
	tables->open_tickets = g_hash_table_new(g_direct_hash, g_direct_equal);
	tables->sla_breaches = g_hash_table_new(g_direct_hash, g_direct_equal);
	tables->dunning_step = g_hash_table_new(g_direct_hash, g_direct_equal);
	tables->usernames = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	tables->as_of = g_date_time_ref(as_of);
	tables->window_start = g_date_time_add_months(as_of, -12);

	/* Touches: what happened, what was done, what was written in. */
	interactions = health_fetch(context, "interaction", organization_id, error);

	if (NULL == interactions)
		return FALSE;

	for (i = 0; i < interactions->len; i++)
	{
		g_autoptr(GDateTime) when = health_when(g_ptr_array_index(interactions, i), "occurred-at");

		health_touch(tables->touches, health_int(g_ptr_array_index(interactions, i), "company-id"),
		             when, as_of);
	}

	activities = health_fetch(context, "activity", organization_id, error);

	if (NULL == activities)
		return FALSE;

	for (i = 0; i < activities->len; i++)
	{
		VentureEntity *activity = g_ptr_array_index(activities, i);
		g_autoptr(GDateTime) when = NULL;

		if (VENTURE_ACTIVITY_STATUS_DONE != health_enum(activity, "status"))
			continue;

		when = health_when(activity, "completed-at");
		health_touch(tables->touches, health_int(activity, "company-id"), when, as_of);
	}

	contacts = health_fetch(context, "contact", organization_id, error);

	if (NULL == contacts)
		return FALSE;

	contact_company = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (i = 0; i < contacts->len; i++)
	{
		VentureEntity *contact = g_ptr_array_index(contacts, i);

		g_hash_table_insert(contact_company,
		                    GINT_TO_POINTER((gint)venture_entity_get_id(contact)),
		                    GINT_TO_POINTER((gint)health_int(contact, "company-id")));
	}

	mail = health_fetch(context, "mail_inbound", organization_id, error);

	if (NULL == mail)
		return FALSE;

	for (i = 0; i < mail->len; i++)
	{
		VentureEntity *message = g_ptr_array_index(mail, i);
		g_autoptr(GDateTime) when = health_when(message, "received-at");
		gint64 company_id;

		company_id = GPOINTER_TO_INT(g_hash_table_lookup(contact_company,
			GINT_TO_POINTER((gint)health_int(message, "contact-id"))));
		health_touch(tables->touches, company_id, when, as_of);
	}

	/* Deals still in play. */
	deals = health_fetch(context, "deal", organization_id, error);

	if (NULL == deals)
		return FALSE;

	for (i = 0; i < deals->len; i++)
	{
		VentureEntity *deal = g_ptr_array_index(deals, i);
		gint stage = health_enum(deal, "stage");

		if ((VENTURE_DEAL_STAGE_WON == stage) || (VENTURE_DEAL_STAGE_LOST == stage))
			continue;

		health_count(tables->open_deals, health_int(deal, "company-id"), 1);
	}

	/* Money owed: issued and not settled, past due. An undated invoice is
	 * not overdue, as the receivables aging says. */
	invoices = health_fetch(context, "invoice", organization_id, error);

	if (NULL == invoices)
		return FALSE;

	for (i = 0; i < invoices->len; i++)
	{
		VentureEntity *invoice = g_ptr_array_index(invoices, i);
		g_autoptr(GDateTime) due = NULL;
		gint status = health_enum(invoice, "status");
		gint64 company_id = health_int(invoice, "company-id");

		if ((VENTURE_INVOICE_STATUS_SENT != status) &&
		    (VENTURE_INVOICE_STATUS_PARTIALLY_PAID != status))
			continue;

		g_hash_table_insert(tables->open_invoices,
		                    GINT_TO_POINTER((gint)venture_entity_get_id(invoice)),
		                    GINT_TO_POINTER((gint)company_id));
		due = health_when(invoice, "due-at");

		if ((NULL == due) || (g_date_time_compare(due, as_of) >= 0))
			continue;

		health_count(tables->overdue_count, company_id, 1);
		health_max(tables->overdue_days, company_id, health_days(due, as_of));
	}

	/* The support queue as it stands. */
	tickets = health_fetch(context, "ticket", organization_id, error);

	if (NULL == tickets)
		return FALSE;

	for (i = 0; i < tickets->len; i++)
	{
		VentureEntity *ticket = g_ptr_array_index(tickets, i);
		gint status = health_enum(ticket, "status");
		gboolean breached = FALSE;

		if ((VENTURE_TICKET_STATUS_DONE == status) ||
		    (VENTURE_TICKET_STATUS_CANCELLED == status))
			continue;

		g_object_get(ticket, "sla-breached", &breached, NULL);
		health_count(tables->open_tickets, health_int(ticket, "company-id"), 1);

		if (breached)
			health_count(tables->sla_breaches, health_int(ticket, "company-id"), 1);
	}

	/* How far the reminder policy has had to go on what is still owed. A
	 * step on an invoice since paid is history, not health. */
	events = health_fetch(context, "dunning_event", organization_id, error);

	if (NULL == events)
		return FALSE;

	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(events, i);
		gint64 company_id;

		company_id = GPOINTER_TO_INT(g_hash_table_lookup(tables->open_invoices,
			GINT_TO_POINTER((gint)health_int(event, "invoice-id"))));
		health_max(tables->dunning_step, company_id, health_int(event, "step"));
	}

	/* What they have paid, as the headline reports count it: one snapshot
	 * scoped to the organisation, its cash read on the first company. */
	{
		g_autoptr(JsonObject) options = json_object_new();

		json_object_set_int_member(options, "organization_id", organization_id);
		tables->snapshot = venture_headline_snapshot_new(context, options, error);
	}

	return NULL != tables->snapshot;
}

static const gchar *
health_username(
	VentureContext	*context,
	HealthTables	*tables,
	gint64		 user_id
){
	g_autoptr(VentureEntity) user = NULL;
	gchar *username = NULL;

	if (0 == user_id)
		return "";

	username = g_hash_table_lookup(tables->usernames, GINT_TO_POINTER((gint)user_id));

	if (NULL != username)
		return username;

	user = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_USER,
	                            user_id, NULL);

	if (NULL != user)
		g_object_get(user, "username", &username, NULL);

	if (NULL == username)
		username = g_strdup("");

	g_hash_table_insert(tables->usernames, GINT_TO_POINTER((gint)user_id), username);

	return username;
}

static VentureCustomerHealth *
health_read(
	VentureContext			*context,
	HealthTables			*tables,
	const VentureHealthThresholds	*thresholds,
	VentureEntity			*company,
	GError				**error
){
	g_autoptr(VentureCustomerHealth) self = NULL;
	GDateTime *touch;
	gint64 company_id;

	company_id = venture_entity_get_id(company);
	self = g_object_new(VENTURE_TYPE_CUSTOMER_HEALTH, NULL);
	self->company_id = company_id;
	g_object_get(company, "name", &self->company_name, NULL);

	if (venture_string_is_empty(self->company_name))
	{
		g_free(self->company_name);
		self->company_name = g_strdup_printf("Company #%" G_GINT64_FORMAT, company_id);
	}

	self->owner = g_strdup(health_username(context, tables,
	                                       health_int(company, "owner-user-id")));

	touch = g_hash_table_lookup(tables->touches, GINT_TO_POINTER((gint)company_id));
	self->last_touch = (NULL != touch) ? g_date_time_ref(touch) : NULL;
	/* Never spoken to counts from the day the company was entered: a
	 * customer added yesterday is not yet quiet. */
	self->days_since_touch = health_days(
		(NULL != touch) ? touch : venture_entity_get_created_at(company), tables->as_of);
	self->open_deals = health_lookup(tables->open_deals, company_id);
	self->overdue_invoices = health_lookup(tables->overdue_count, company_id);
	self->overdue_days = health_lookup(tables->overdue_days, company_id);
	self->open_tickets = health_lookup(tables->open_tickets, company_id);
	self->sla_breaches = health_lookup(tables->sla_breaches, company_id);
	self->dunning_step = health_lookup(tables->dunning_step, company_id);

	/* The trailing year's paid revenue, read the way ltv reads it. */
	if (!venture_headline_snapshot_customer_cash(tables->snapshot, company_id,
	                                             tables->window_start, tables->as_of,
	                                             &self->revenue_12m, error))
		return NULL;

	if (self->days_since_touch >= thresholds->touch_days)
		self->flags |= VENTURE_HEALTH_FLAG_QUIET;

	if (self->overdue_days >= thresholds->overdue_days)
		self->flags |= VENTURE_HEALTH_FLAG_OVERDUE;

	if ((self->open_tickets >= thresholds->open_tickets) || (self->sla_breaches > 0))
		self->flags |= VENTURE_HEALTH_FLAG_TICKETS;

	/* The three reasons, in words, whether or not they count against
	 * the company: the page shows where it stands on each. */
	g_ptr_array_add(self->reasons, (NULL != touch)
		? g_strdup_printf("Last touch %" G_GINT64_FORMAT " days ago, %s %" G_GINT64_FORMAT,
		                  self->days_since_touch,
		                  (self->flags & VENTURE_HEALTH_FLAG_QUIET) ? "over" : "within",
		                  thresholds->touch_days)
		: g_strdup_printf("Last touch never, %" G_GINT64_FORMAT " days since the company was added, %s %" G_GINT64_FORMAT,
		                  self->days_since_touch,
		                  (self->flags & VENTURE_HEALTH_FLAG_QUIET) ? "over" : "within",
		                  thresholds->touch_days));
	g_ptr_array_add(self->reasons, (self->overdue_invoices > 0)
		? g_strdup_printf("%" G_GINT64_FORMAT " overdue invoice%s, the oldest %" G_GINT64_FORMAT " days, %s %" G_GINT64_FORMAT "%s",
		                  self->overdue_invoices, (1 == self->overdue_invoices) ? "" : "s",
		                  self->overdue_days,
		                  (self->flags & VENTURE_HEALTH_FLAG_OVERDUE) ? "over" : "within",
		                  thresholds->overdue_days,
		                  (self->dunning_step > 0) ? "; reminders sent" : "")
		: g_strdup_printf("No overdue invoices (threshold %" G_GINT64_FORMAT " days)",
		                  thresholds->overdue_days));
	g_ptr_array_add(self->reasons,
		g_strdup_printf("%" G_GINT64_FORMAT " open ticket%s, %" G_GINT64_FORMAT " past service level, %s %" G_GINT64_FORMAT,
		                self->open_tickets, (1 == self->open_tickets) ? "" : "s",
		                self->sla_breaches,
		                (self->flags & VENTURE_HEALTH_FLAG_TICKETS) ? "over" : "within",
		                thresholds->open_tickets));

	return g_steal_pointer(&self);
}

static gboolean
health_is_customer(VentureEntity *company)
{
	return VENTURE_IS_COMPANY(company) &&
	       (VENTURE_COMPANY_KIND_CUSTOMER == health_enum(company, "kind"));
}

static gint
health_compare_default(
	gconstpointer	a,
	gconstpointer	b
){
	VentureCustomerHealth *left = *(VentureCustomerHealth *const *)a;
	VentureCustomerHealth *right = *(VentureCustomerHealth *const *)b;
	gint by_band;

	by_band = (gint)venture_customer_health_get_band(right) -
	          (gint)venture_customer_health_get_band(left);

	if (0 != by_band)
		return by_band;

	return g_utf8_collate(left->company_name, right->company_name);
}

static GDateTime *
health_as_of(GDateTime *as_of)
{
	return (NULL != as_of) ? g_date_time_ref(as_of) : venture_time_now();
}

VentureCustomerHealth *
venture_customer_health_for_company(
	VentureContext	 *context,
	VentureEntity	 *company,
	GDateTime	 *as_of,
	GError		**error
){
	g_autoptr(GDateTime) at = NULL;
	VentureHealthThresholds thresholds;
	HealthTables tables;
	VentureCustomerHealth *health;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(company), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	if (!VENTURE_IS_COMPANY(company))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Health is read for a company");
		return NULL;
	}

	at = health_as_of(as_of);
	venture_customer_health_thresholds(context, venture_entity_get_organization_id(company),
	                                   &thresholds);

	if (!health_tables_load(context, venture_entity_get_organization_id(company), at,
	                        &tables, error))
	{
		health_tables_free(&tables);
		return NULL;
	}

	health = health_read(context, &tables, &thresholds, company, error);
	health_tables_free(&tables);

	return health;
}

GPtrArray *
venture_customer_health_compute(
	VentureContext	 *context,
	gint64		  organization_id,
	GDateTime	 *as_of,
	GError		**error
){
	g_autoptr(GPtrArray) companies = NULL;
	g_autoptr(GPtrArray) readings = NULL;
	g_autoptr(GDateTime) at = NULL;
	VentureHealthThresholds thresholds;
	HealthTables tables;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	if (0 == organization_id)
		organization_id = venture_context_get_default_organization_id(context);

	at = health_as_of(as_of);
	companies = health_fetch(context, "company", organization_id, error);

	if (NULL == companies)
		return NULL;

	venture_customer_health_thresholds(context, organization_id, &thresholds);

	if (!health_tables_load(context, organization_id, at, &tables, error))
	{
		health_tables_free(&tables);
		return NULL;
	}

	readings = g_ptr_array_new_with_free_func(g_object_unref);

	for (i = 0; i < companies->len; i++)
	{
		VentureEntity *company = g_ptr_array_index(companies, i);
		VentureCustomerHealth *health;

		if (!health_is_customer(company))
			continue;

		health = health_read(context, &tables, &thresholds, company, error);

		if (NULL == health)
		{
			health_tables_free(&tables);
			return NULL;
		}

		g_ptr_array_add(readings, health);
	}

	health_tables_free(&tables);
	g_ptr_array_sort(readings, health_compare_default);

	return g_steal_pointer(&readings);
}

/* --- The sweep ------------------------------------------------------------ */

static gchar *
health_check_in_subject(VentureCustomerHealth *health)
{
	return g_strdup_printf("check in: %s", health->company_name);
}

/* Whether a planned check-in for the company is already on somebody's
 * list. The subject and the company both have to match: a check-in on
 * another company with the same name is not this one. */
static gboolean
health_check_in_open(
	VentureContext		 *context,
	GType			  activity_type,
	gint64			  organization_id,
	VentureCustomerHealth	 *health,
	gboolean		 *out_open,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *subject = health_check_in_subject(health);

	query = venture_query_new(activity_type);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 1);

	if (!venture_query_add_filter_string(query, "subject", VENTURE_FILTER_OP_EQ, subject, error) ||
	    !venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ,
	                                  health->company_id, error) ||
	    !venture_query_add_filter_int(query, "status", VENTURE_FILTER_OP_EQ,
	                                  VENTURE_ACTIVITY_STATUS_PLANNED, error))
		return FALSE;

	rows = venture_database_find(venture_context_get_database(context), query, error);

	if (NULL == rows)
		return FALSE;

	*out_open = rows->len > 0;

	return TRUE;
}

gint
venture_customer_health_sweep(
	VentureContext		 *context,
	gint64			  organization_id,
	GDateTime		 *as_of,
	guint			  limit,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GPtrArray) readings = NULL;
	g_autoptr(GDateTime) at = NULL;
	VentureDatabase *database;
	GType activity_type;
	guint visited = 0;
	gint created = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), -1);
	g_return_val_if_fail(error == NULL || *error == NULL, -1);

	if (organization_id <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An organization is required");
		return -1;
	}

	if (!venture_context_module_enabled(context, HEALTH_MODULE))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "The customer_health module is off");
		return -1;
	}

	activity_type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(context), "activity");

	if (!venture_context_module_enabled(context, "activities") ||
	    (G_TYPE_INVALID == activity_type))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "A check-in is a next action, and the activities "
		                    "module is off");
		return -1;
	}

	if (0 == limit)
		limit = HEALTH_SWEEP_DEFAULT_LIMIT;

	limit = MIN(limit, HEALTH_SWEEP_MAX_LIMIT);
	at = health_as_of(as_of);
	readings = venture_customer_health_compute(context, organization_id, at, error);

	if (NULL == readings)
		return -1;

	/* One transaction: a save refused part-way through rolls back every
	 * check-in this sweep made, so a rerun starts clean rather than from
	 * half a list. */
	database = venture_context_get_database(context);

	if (!venture_database_begin(database, error))
		return -1;

	for (i = 0; (i < readings->len) && (visited < limit); i++)
	{
		VentureCustomerHealth *health = g_ptr_array_index(readings, i);
		g_autoptr(VentureEntity) activity = NULL;
		g_autofree gchar *subject = NULL;
		g_autofree gchar *body = NULL;
		gboolean open = FALSE;

		/* Worst first, so the red ones are at the front; the first
		 * non-red is the end of the list. */
		if (VENTURE_HEALTH_BAND_RED != venture_customer_health_get_band(health))
			break;

		visited++;

		if (!health_check_in_open(context, activity_type, organization_id, health,
		                          &open, error))
		{
			venture_database_rollback(database);
			return -1;
		}

		if (open)
			continue;

		subject = health_check_in_subject(health);
		body = g_strdup_printf("%s is at risk. %s. %s. %s.",
		                       health->company_name,
		                       (const gchar *)g_ptr_array_index(health->reasons, 0),
		                       (const gchar *)g_ptr_array_index(health->reasons, 1),
		                       (const gchar *)g_ptr_array_index(health->reasons, 2));
		activity = g_object_new(activity_type, NULL);
		venture_entity_set_organization_id(activity, organization_id);
		g_object_set(activity, "subject", subject,
		             "kind", VENTURE_ACTIVITY_KIND_FOLLOWUP,
		             "body", body,
		             "owner", health->owner,
		             "due-at", at,
		             "priority", VENTURE_PRIORITY_HIGH,
		             "status", VENTURE_ACTIVITY_STATUS_PLANNED,
		             "related-type", "company",
		             "related-id", health->company_id,
		             "company-id", health->company_id,
		             NULL);

		if (!venture_database_save(database, activity, actor, error))
		{
			venture_database_rollback(database);
			return -1;
		}

		created++;
	}

	if (!venture_database_commit(database, error))
		return -1;

	return created;
}

/* --- The company page ----------------------------------------------------- */

void
venture_customer_health_append_block(
	VentureContext	*context,
	GString		*content,
	VentureEntity	*record
){
	g_autoptr(VentureCustomerHealth) health = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *band;
	guint i;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_return_if_fail(NULL != content);
	g_return_if_fail(VENTURE_IS_ENTITY(record));

	if (!venture_context_module_enabled(context, HEALTH_MODULE) ||
	    !health_is_customer(record))
		return;

	health = venture_customer_health_for_company(context, record, NULL, &error);

	if (NULL == health)
	{
		g_string_append(content, "<section class=\"card customer-health\">"
		                         "<h2>Health</h2><p class=\"notice negative\">");
		venture_html_escape_append(content, error->message);
		g_string_append(content, "</p></section>");
		return;
	}

	band = venture_health_band_to_string(venture_customer_health_get_band(health));
	g_string_append(content, "<section class=\"card customer-health\"><h2>Health</h2>"
	                         "<p><span class=\"badge health-band ");
	g_string_append(content, band);
	g_string_append(content, "\">");
	g_string_append(content, band);
	g_string_append(content, "</span></p><ul class=\"health-reasons\">");

	for (i = 0; i < health->reasons->len; i++)
	{
		g_string_append(content, "<li>");
		venture_html_escape_append(content, g_ptr_array_index(health->reasons, i));
		g_string_append(content, "</li>");
	}

	g_string_append(content, "</ul><p><a href=\"/reports/customer_health\">"
	                         "Customer health report</a></p></section>");
}

/* --- The report ----------------------------------------------------------- */

typedef struct
{
	const gchar		*key;
	const gchar		*label;
	VentureReportColumnKind	 kind;
} HealthColumn;

static const HealthColumn health_columns[] = {
	{ "company", "Company", VENTURE_REPORT_COLUMN_TEXT },
	{ "owner", "Owner", VENTURE_REPORT_COLUMN_TEXT },
	{ "band", "Band", VENTURE_REPORT_COLUMN_TEXT },
	{ "last_touch", "Last touch", VENTURE_REPORT_COLUMN_DATE },
	{ "days_since_touch", "Days since touch", VENTURE_REPORT_COLUMN_NUMBER },
	{ "open_deals", "Open deals", VENTURE_REPORT_COLUMN_NUMBER },
	{ "overdue_invoices", "Overdue invoices", VENTURE_REPORT_COLUMN_NUMBER },
	{ "overdue_days", "Oldest overdue (days)", VENTURE_REPORT_COLUMN_NUMBER },
	{ "open_tickets", "Open tickets", VENTURE_REPORT_COLUMN_NUMBER },
	{ "sla_breaches", "SLA breaches", VENTURE_REPORT_COLUMN_NUMBER },
	{ "dunning_step", "Dunning step", VENTURE_REPORT_COLUMN_NUMBER },
	{ "revenue_12m", "Revenue, trailing 12 months", VENTURE_REPORT_COLUMN_MONEY }
};

typedef struct
{
	const HealthColumn	*column;
	gboolean		 descending;
} HealthSort;

static gint64
health_number_for(
	VentureCustomerHealth	*health,
	const gchar		*key
){
	if (0 == strcmp(key, "days_since_touch"))
		return health->days_since_touch;
	if (0 == strcmp(key, "open_deals"))
		return health->open_deals;
	if (0 == strcmp(key, "overdue_invoices"))
		return health->overdue_invoices;
	if (0 == strcmp(key, "overdue_days"))
		return health->overdue_days;
	if (0 == strcmp(key, "open_tickets"))
		return health->open_tickets;
	if (0 == strcmp(key, "sla_breaches"))
		return health->sla_breaches;
	if (0 == strcmp(key, "dunning_step"))
		return health->dunning_step;
	if (0 == strcmp(key, "band"))
		return (gint64)venture_customer_health_get_band(health);
	if (0 == strcmp(key, "last_touch"))
		return (NULL != health->last_touch) ? g_date_time_to_unix(health->last_touch) : 0;

	return 0;
}

static gint
health_compare_sorted(
	gconstpointer	a,
	gconstpointer	b,
	gpointer	user_data
){
	VentureCustomerHealth *left = *(VentureCustomerHealth *const *)a;
	VentureCustomerHealth *right = *(VentureCustomerHealth *const *)b;
	const HealthSort *sort = user_data;
	const gchar *key = sort->column->key;
	gint result;

	if (0 == strcmp(key, "company"))
		result = g_utf8_collate(left->company_name, right->company_name);
	else if (0 == strcmp(key, "owner"))
		result = g_utf8_collate(left->owner, right->owner);
	else if (0 == strcmp(key, "revenue_12m"))
	{
		/* Nothing paid sorts below any amount; amounts in the same
		 * currency compare as money. */
		if ((NULL == left->revenue_12m) || (NULL == right->revenue_12m))
			result = (NULL != left->revenue_12m) - (NULL != right->revenue_12m);
		else
			result = venture_money_compare(left->revenue_12m, right->revenue_12m);
	}
	else
	{
		gint64 l = health_number_for(left, key);
		gint64 r = health_number_for(right, key);

		result = (l > r) - (l < r);
	}

	if (0 == result)
		return health_compare_default(a, b);

	return sort->descending ? -result : result;
}

static const HealthColumn *
health_column_named(const gchar *key)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(health_columns); i++)
	{
		if (0 == strcmp(health_columns[i].key, key))
			return &health_columns[i];
	}

	return NULL;
}

static VentureReportResult *
venture_report_customer_health(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) readings = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	VentureHealthThresholds thresholds;
	HealthSort sort = { NULL, FALSE };
	VentureHealthBand band_filter = VENTURE_HEALTH_BAND_GREEN;
	gboolean filter_band = FALSE;
	const gchar *owner_filter = NULL;
	const gchar *requested;
	gint64 organization_id;
	gint64 counts[3] = { 0, 0, 0 };
	guint i;

	organization_id = (NULL != options)
		? venture_json_object_get_int(options, "organization_id", 0) : 0;

	if (0 == organization_id)
		organization_id = venture_context_get_default_organization_id(context);

	/* Health is read as of a moment, not over a period: the period's end,
	 * or now if the period is still running, or the as_of asked for. */
	if ((NULL != options) && json_object_has_member(options, "as_of"))
	{
		as_of = venture_period_report_as_of(options, error);

		if (NULL == as_of)
			return NULL;
	}
	else
	{
		g_autoptr(GDateTime) now = venture_time_now();
		GDateTime *end = (NULL != period) ? venture_date_range_get_end(period) : NULL;

		as_of = ((NULL == end) || (g_date_time_compare(end, now) > 0))
			? g_date_time_ref(now) : g_date_time_ref(end);
	}

	requested = (NULL != options) ? venture_json_object_get_string(options, "band", NULL) : NULL;

	if (!venture_string_is_empty(requested))
	{
		if (!venture_health_band_from_string(requested, &band_filter))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not a band; use green, amber or red", requested);
			return NULL;
		}

		filter_band = TRUE;
	}

	requested = (NULL != options) ? venture_json_object_get_string(options, "sort", NULL) : NULL;

	if (!venture_string_is_empty(requested))
	{
		sort.descending = ('-' == requested[0]);
		sort.column = health_column_named(sort.descending ? requested + 1 : requested);

		if (NULL == sort.column)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not a column of the customer health report",
			            requested);
			return NULL;
		}
	}

	owner_filter = (NULL != options) ? venture_json_object_get_string(options, "owner", NULL) : NULL;
	readings = venture_customer_health_compute(context, organization_id, as_of, error);

	if (NULL == readings)
		return NULL;

	if (NULL != sort.column)
		g_ptr_array_sort_with_data(readings, health_compare_sorted, &sort);

	venture_customer_health_thresholds(context, organization_id, &thresholds);
	result = venture_report_result_new("Customer health", period);

	for (i = 0; i < G_N_ELEMENTS(health_columns); i++)
		venture_report_result_add_column(result, health_columns[i].key,
		                                 health_columns[i].label,
		                                 health_columns[i].kind);

	/* The counts are of every customer, filtered or not, so the card and
	 * a filtered page do not disagree about how many are at risk. */
	for (i = 0; i < readings->len; i++)
	{
		VentureCustomerHealth *health = g_ptr_array_index(readings, i);
		g_autofree gchar *touch = NULL;
		VentureHealthBand band = venture_customer_health_get_band(health);

		counts[band]++;

		if (filter_band && (band != band_filter))
			continue;

		if (!venture_string_is_empty(owner_filter) &&
		    (0 != g_strcmp0(owner_filter, health->owner)))
			continue;

		touch = (NULL != health->last_touch)
			? venture_time_to_date_string(health->last_touch, NULL) : NULL;

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "company", health->company_name);
		venture_report_result_set_text(result, "owner", health->owner);
		venture_report_result_set_text(result, "band", venture_health_band_to_string(band));
		venture_report_result_set_text(result, "last_touch", touch);
		venture_report_result_set_number(result, "days_since_touch", (gdouble)health->days_since_touch);
		venture_report_result_set_number(result, "open_deals", (gdouble)health->open_deals);
		venture_report_result_set_number(result, "overdue_invoices", (gdouble)health->overdue_invoices);
		venture_report_result_set_number(result, "overdue_days", (gdouble)health->overdue_days);
		venture_report_result_set_number(result, "open_tickets", (gdouble)health->open_tickets);
		venture_report_result_set_number(result, "sla_breaches", (gdouble)health->sla_breaches);
		venture_report_result_set_number(result, "dunning_step", (gdouble)health->dunning_step);
		venture_report_result_set_money(result, "revenue_12m", health->revenue_12m);
	}

	{
		VentureMetric *metric;

		metric = venture_metric_new_count("red", "At risk", counts[VENTURE_HEALTH_BAND_RED]);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
		metric = venture_metric_new_count("amber", "Watch", counts[VENTURE_HEALTH_BAND_AMBER]);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
		venture_report_result_add_metric(result,
			venture_metric_new_count("green", "Healthy", counts[VENTURE_HEALTH_BAND_GREEN]));
		venture_report_result_add_metric(result,
			venture_metric_new_count("customers", "Customers", readings->len));
	}

	{
		g_autofree gchar *note = NULL;
		g_autofree gchar *when = venture_time_to_date_string(as_of, NULL);

		note = g_strdup_printf(
			"As of %s. A company is quiet after %" G_GINT64_FORMAT " days without an "
			"interaction, a completed activity or inbound mail; an invoice counts "
			"against it %" G_GINT64_FORMAT " days past due; the queue counts against it "
			"at %" G_GINT64_FORMAT " open tickets or any missed service level. One "
			"reason is amber, two or more are red. Revenue is applied receipts less "
			"refunds in the twelve months before, in the book currency.",
			when, thresholds.touch_days, thresholds.overdue_days, thresholds.open_tickets);
		venture_report_result_set_note(result, note);
	}

	return g_steal_pointer(&result);
}

void
venture_customer_health_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"customer_health", "Customer health",
		"Every customer's last touch, open deals, overdue invoices, tickets, "
		"reminder step and trailing revenue, banded green, amber or red",
		venture_report_customer_health)));
}
