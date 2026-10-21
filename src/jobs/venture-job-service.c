/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Jobs: the work a home-services business does at a customer's address.
 * The records are field tables; this file keeps them honest at the save
 * and gives the buttons a technician presses through the day.
 */
#include "venture.h"
#include <string.h>

static gboolean
refuse(GError **error, const gchar *field, const gchar *message)
{
	venture_set_error_validation(error, field, "%s", message);
	return FALSE;
}

static gint64
number(VentureEntity *e, const gchar *name)
{
	gint64 value = 0;
	g_object_get(e, name, &value, NULL);
	return value;
}

static gint
choice(VentureEntity *e, const gchar *name)
{
	gint value = 0;
	g_object_get(e, name, &value, NULL);
	return value;
}

static gboolean
enabled(const gchar *type)
{
	return venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), type);
}

/* A live row of @type in @org, or NULL with an error naming the field. */
static VentureEntity *
load(VentureDatabase *database, GType type, gint64 id, gint64 org, const gchar *field, GError **error)
{
	g_autoptr(GError) local = NULL;
	VentureEntity *row = venture_database_get(database, type, id, &local);
	if (row == NULL || venture_entity_is_deleted(row) || venture_entity_get_organization_id(row) != org)
	{
		g_clear_object(&row);
		refuse(error, field, "names a record that is not in this business");
		return NULL;
	}
	return row;
}

/* The next free J- number in the business, deleted jobs included so a
 * number is never handed out twice. */
static gchar *
next_number(VentureDatabase *database, gint64 org)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOB);
	gint64 next;
	guint tries;
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	next = venture_database_count(database, query, NULL) + 1;
	for (tries = 0; tries < 1000; tries++, next++)
	{
		g_autoptr(VentureQuery) taken = venture_query_new(VENTURE_TYPE_JOB);
		g_autofree gchar *candidate = g_strdup_printf("J-%" G_GINT64_FORMAT, next);
		venture_query_set_organization(taken, org);
		venture_query_set_include_deleted(taken, TRUE);
		venture_query_add_filter_string(taken, "number", VENTURE_FILTER_OP_EQ, candidate, NULL);
		if (venture_database_count(database, taken, NULL) == 0)
			return g_steal_pointer(&candidate);
	}
	/* A thousand taken in a row means numbers were typed by hand; this
	 * one cannot collide with any of them. */
	{
		g_autofree gchar *uuid = g_uuid_string_random();
		return g_strdup_printf("J-%.8s", uuid);
	}
}

/* An estimate or invoice the job names must be this customer's. */
static gboolean
same_customer(VentureDatabase *database, VentureEntity *job, const gchar *type_name, GType type,
	const gchar *field, const gchar *what, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	gint64 id = number(job, field);
	if (id == 0)
		return TRUE;
	if (!enabled(type_name))
	{
		g_autofree gchar *message = g_strdup_printf("%s are switched off in this install", what);
		return refuse(error, field, message);
	}
	row = load(database, type, id, venture_entity_get_organization_id(job), field, error);
	if (row == NULL)
		return FALSE;
	if (number(row, "company-id") != number(job, "company-id"))
	{
		g_autofree gchar *message = g_strdup_printf("The %s is for another customer", what[0] == 'Q' ? "estimate" : "invoice");
		return refuse(error, field, message);
	}
	return TRUE;
}

static gboolean
validate_job(VentureDatabase *database, VentureEntity *job, VentureEntity *previous, gpointer data, GError **error)
{
	g_autofree gchar *code = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(VentureEntity) company = NULL;
	gint64 org = venture_entity_get_organization_id(job);
	(void)previous;
	(void)data;
	g_object_get(job, "number", &code, "window-start", &start, "window-end", &end, NULL);
	if (venture_string_is_empty(code))
	{
		g_autofree gchar *assigned = next_number(database, org);
		g_object_set(job, "number", assigned, NULL);
	}
	else
	{
		g_strstrip(code);
		g_object_set(job, "number", code, NULL);
	}
	company = load(database, VENTURE_TYPE_COMPANY, number(job, "company-id"), org, "company-id", error);
	if (company == NULL)
		return FALSE;
	if (number(job, "contact-id") != 0)
	{
		g_autoptr(VentureEntity) contact = load(database, VENTURE_TYPE_CONTACT, number(job, "contact-id"), org,
			"contact-id", error);
		if (contact == NULL)
			return FALSE;
		if (number(contact, "company-id") != 0 && number(contact, "company-id") != number(job, "company-id"))
			return refuse(error, "contact-id", "The contact works for another customer");
	}
	if (number(job, "property-id") != 0)
	{
		g_autoptr(VentureEntity) property = load(database, VENTURE_TYPE_PROPERTY, number(job, "property-id"), org,
			"property-id", error);
		g_autofree gchar *address = NULL, *where = NULL;
		if (property == NULL)
			return FALSE;
		if (number(property, "company-id") != number(job, "company-id"))
			return refuse(error, "property-id", "The property belongs to another customer");
		/* The property's address is where the job is, unless the job was
		 * given one of its own. */
		g_object_get(job, "service-address", &where, NULL);
		g_object_get(property, "address", &address, NULL);
		if (venture_string_is_empty(where) && !venture_string_is_empty(address))
			g_object_set(job, "service-address", address, NULL);
	}
	if (number(job, "maintenance-plan-id") != 0)
	{
		g_autoptr(VentureEntity) plan = load(database, VENTURE_TYPE_MAINTENANCE_PLAN, number(job, "maintenance-plan-id"), org,
			"maintenance-plan-id", error);
		if (plan == NULL)
			return FALSE;
		if (number(plan, "company-id") != number(job, "company-id"))
			return refuse(error, "maintenance-plan-id", "The maintenance plan belongs to another customer");
	}
	if (end != NULL && start == NULL)
		return refuse(error, "window-start", "An arrival window needs its start as well as its end");
	if (start != NULL && end != NULL && g_date_time_compare(end, start) <= 0)
		return refuse(error, "window-end", "The arrival window must end after it starts");
	if (!same_customer(database, job, "quote", VENTURE_TYPE_QUOTE, "quote-id", "Quotes", error) ||
	    !same_customer(database, job, "invoice", VENTURE_TYPE_INVOICE, "invoice-id", "Invoices", error))
		return FALSE;
	if (previous == NULL && number(job, "quote-id") != 0)
	{
		g_autofree gchar *address = NULL, *quoted = NULL;
		g_autoptr(VentureEntity) quote = venture_database_get(database, VENTURE_TYPE_QUOTE, number(job, "quote-id"), NULL);
		g_object_get(job, "service-address", &address, NULL);
		if (quote != NULL && venture_string_is_empty(address))
		{
			g_object_get(quote, "service-address", &quoted, NULL);
			g_object_set(job, "service-address", quoted, NULL);
		}
	}
	if (choice(job, "status") == VENTURE_JOB_INVOICED && number(job, "invoice-id") == 0)
		return refuse(error, "invoice-id", "An invoiced job names its invoice");
	return TRUE;
}

static gboolean
validate_technician(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureEntity) job = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gboolean active = FALSE;
	guint i;
	(void)previous;
	(void)data;
	if (venture_entity_is_deleted(row))
		return TRUE;
	job = load(database, VENTURE_TYPE_JOB, number(row, "job-id"), venture_entity_get_organization_id(row), "job-id", error);
	if (job == NULL)
		return FALSE;
	user = venture_database_get(database, VENTURE_TYPE_USER, number(row, "user-id"), NULL);
	if (user != NULL)
		g_object_get(user, "active", &active, NULL);
	if (user == NULL || venture_entity_is_deleted(user) || !active)
		return refuse(error, "user-id", "A technician is a person who can sign in");
	query = venture_query_new(VENTURE_TYPE_JOB_TECHNICIAN);
	venture_query_set_organization(query, venture_entity_get_organization_id(row));
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "job-id", VENTURE_FILTER_OP_EQ, number(row, "job-id"), NULL);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, number(row, "user-id"), NULL);
	rows = venture_database_find(database, query, error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
		if (venture_entity_get_id(g_ptr_array_index(rows, i)) != venture_entity_get_id(row))
			return refuse(error, "user-id", "That technician is on this job already");
	return TRUE;
}

static gboolean
validate_property(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureEntity) company = NULL;
	g_autofree gchar *address = NULL;
	gint64 org = venture_entity_get_organization_id(row);
	(void)previous;
	(void)data;
	g_object_get(row, "address", &address, NULL);
	if (venture_string_is_empty(address) || g_strstrip(address)[0] == '\0')
		return refuse(error, "address", "A property needs its address");
	company = load(database, VENTURE_TYPE_COMPANY, number(row, "company-id"), org, "company-id", error);
	if (company == NULL)
		return FALSE;
	if (number(row, "contact-id") != 0)
	{
		g_autoptr(VentureEntity) contact = load(database, VENTURE_TYPE_CONTACT, number(row, "contact-id"), org,
			"contact-id", error);
		if (contact == NULL)
			return FALSE;
	}
	return TRUE;
}

static gboolean
validate_equipment(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureEntity) property = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gint64 year = number(row, "installed-year");
	(void)previous;
	(void)data;
	property = load(database, VENTURE_TYPE_PROPERTY, number(row, "property-id"), venture_entity_get_organization_id(row),
		"property-id", error);
	if (property == NULL)
		return FALSE;
	/* Blank is "nobody knows"; anything else is a year it could have gone in. */
	if (year != 0 && (year < 1900 || year > g_date_time_get_year(now) + 1))
		return refuse(error, "installed-year", "Give the year it was installed, like 2009, or leave it blank");
	return TRUE;
}

/* A plan is its customer's, at one of their properties, for equipment at
 * that property, with something to call each visit and an interval. */
static gboolean
validate_plan(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureEntity) company = NULL;
	g_autofree gchar *visit = NULL;
	gint64 org = venture_entity_get_organization_id(row);
	gint64 every = number(row, "every-months");
	(void)previous;
	(void)data;
	if (venture_entity_is_deleted(row))
		return TRUE;
	company = load(database, VENTURE_TYPE_COMPANY, number(row, "company-id"), org, "company-id", error);
	if (company == NULL)
		return FALSE;
	/* A derived property must pass the same ownership check as a typed one. */
	if (number(row, "equipment-id") != 0)
	{
		g_autoptr(VentureEntity) equipment = load(database, VENTURE_TYPE_EQUIPMENT, number(row, "equipment-id"), org,
			"equipment-id", error);
		if (equipment == NULL)
			return FALSE;
		if (number(row, "property-id") == 0)
			g_object_set(row, "property-id", number(equipment, "property-id"), NULL);
		else if (number(equipment, "property-id") != number(row, "property-id"))
			return refuse(error, "equipment-id", "That equipment is at another property");
	}
	if (number(row, "property-id") != 0)
	{
		g_autoptr(VentureEntity) property = load(database, VENTURE_TYPE_PROPERTY, number(row, "property-id"), org,
			"property-id", error);
		if (property == NULL)
			return FALSE;
		if (number(property, "company-id") != number(row, "company-id"))
			return refuse(error, "property-id", "The property belongs to another customer");
	}
	g_object_get(row, "visit", &visit, NULL);
	if (venture_string_is_empty(visit) || g_strstrip(visit)[0] == '\0')
	{
		g_autofree gchar *name = NULL;
		g_object_get(row, "name", &name, NULL);
		g_object_set(row, "visit", name, NULL);
	}
	if (every == 0)
		g_object_set(row, "every-months", (gint64)12, NULL);
	else if (every < 1 || every > 120)
		return refuse(error, "every-months", "Give the months between visits, from 1 to 120");
	return TRUE;
}

void
venture_jobs_install(VentureDatabase *database)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	if (g_object_get_data(G_OBJECT(database), "venture-jobs-installed") != NULL)
		return;
	g_object_set_data(G_OBJECT(database), "venture-jobs-installed", GINT_TO_POINTER(1));
	venture_database_add_save_validator(database, VENTURE_TYPE_JOB, validate_job, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_JOB_TECHNICIAN, validate_technician, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_PROPERTY, validate_property, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_EQUIPMENT, validate_equipment, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_MAINTENANCE_PLAN, validate_plan, NULL, NULL);
}

/* --- The day's buttons -------------------------------------------------------- */

typedef struct {
	const gchar *name;
	const gchar *label;
	const gchar *description;
	guint from;		/* bit set of the statuses it may be pressed from */
	VentureJobStatus to;
	const gchar *stamp;	/* the time it records, or NULL */
} JobStep;

#define FROM(s) (1u << (s))

static const JobStep job_steps[] = {
	{ "on_the_way", "On my way", "A technician has set off for the job",
	  FROM(VENTURE_JOB_BOOKED), VENTURE_JOB_ON_THE_WAY, "left-at" },
	{ "arrive", "Arrived", "A technician is at the address",
	  FROM(VENTURE_JOB_BOOKED) | FROM(VENTURE_JOB_ON_THE_WAY), VENTURE_JOB_ON_SITE, "arrived-at" },
	{ "complete", "Done", "The work is finished",
	  FROM(VENTURE_JOB_BOOKED) | FROM(VENTURE_JOB_ON_THE_WAY) | FROM(VENTURE_JOB_ON_SITE), VENTURE_JOB_DONE, "completed-at" },
	{ "invoiced", "Invoiced", "The work is billed: names the invoice, or takes the estimate's",
	  FROM(VENTURE_JOB_DONE), VENTURE_JOB_INVOICED, NULL },
	{ "cancel", "Cancel job", "Called off before the work was done",
	  FROM(VENTURE_JOB_BOOKED) | FROM(VENTURE_JOB_ON_THE_WAY) | FROM(VENTURE_JOB_ON_SITE), VENTURE_JOB_CANCELLED, NULL }
};

static const JobStep *
step_for(VentureAction *action)
{
	g_autofree gchar *name = NULL;
	guint i;
	g_object_get(action, "name", &name, NULL);
	for (i = 0; i < G_N_ELEMENTS(job_steps); i++)
		if (g_strcmp0(job_steps[i].name, name) == 0)
			return &job_steps[i];
	return NULL;
}

static gboolean
step_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	const JobStep *step = step_for(action);
	(void)actor;
	if (!enabled("job"))
		return refuse(error, "module", "The jobs module is switched off");
	if (step == NULL || !VENTURE_IS_JOB(entity))
		return refuse(error, "job", "That is not a job");
	if (!(step->from & FROM(choice(entity, "status"))))
	{
		g_autofree gchar *message = g_strdup_printf("\"%s\" doesn't apply to a job that is %s", step->label,
			venture_enum_to_nick(venture_job_status_get_type(), choice(entity, "status")));
		g_strdelimit(message, "_", ' ');
		return refuse(error, "status", message);
	}
	return TRUE;
}

/* An estimate billed after the work left a draft invoice; the work is
 * done now, so it is issued, dated today and due on the usual thirty days.
 * The save goes through settlement, which posts it like any other issue. */
static gboolean
issue_draft(VentureDatabase *database, VentureEntity *job, gint64 id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = load(database, VENTURE_TYPE_INVOICE, id,
		venture_entity_get_organization_id(job), "invoice-id", error);
	g_autoptr(GDateTime) today = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autoptr(GDateTime) set_due = NULL;
	if (invoice == NULL)
		return FALSE;
	if (choice(invoice, "status") != VENTURE_INVOICE_STATUS_DRAFT)
		return TRUE;
	today = venture_settlement_service_today(venture_settlement_service_get(database));
	if (today == NULL)
		return refuse(error, "invoice-id", "Today's date could not be read in the business's zone");
	g_object_get(invoice, "due-at", &set_due, NULL);
	due = set_due != NULL ? g_date_time_ref(set_due) : g_date_time_add_days(today, 30);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, "issued-at", today, "due-at", due, NULL);
	return venture_database_save(database, invoice, actor, error);
}

/* "Invoiced" names the invoice it was given, else the one the job already
 * carries, else the one its accepted estimate became. */
static gboolean
take_invoice(VentureDatabase *database, VentureEntity *job, GHashTable *params, GError **error)
{
	JsonNode *node = params != NULL ? g_hash_table_lookup(params, "invoice_id") : NULL;
	gint64 invoice = node != NULL && JSON_NODE_HOLDS_VALUE(node) ? json_node_get_int(node) : 0;
	if (invoice == 0)
		invoice = number(job, "invoice-id");
	if (invoice == 0 && number(job, "quote-id") != 0 && enabled("quote"))
	{
		g_autoptr(VentureEntity) quote = load(database, VENTURE_TYPE_QUOTE, number(job, "quote-id"),
			venture_entity_get_organization_id(job), "quote-id", error);
		if (quote == NULL)
			return FALSE;
		invoice = number(quote, "invoice-id");
	}
	if (invoice == 0)
		return refuse(error, "invoice-id", "Name the invoice: the job has none and its estimate has not been invoiced");
	g_object_set(job, "invoice-id", invoice, NULL);
	return same_customer(database, job, "invoice", VENTURE_TYPE_INVOICE, "invoice-id", "Invoices", error);
}

static VentureEntity *
step_invoke_internal(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	const JobStep *step = step_for(action);
	g_autoptr(VentureEntity) job = NULL;
	job = venture_database_get(database, VENTURE_TYPE_JOB, venture_entity_get_id(entity), error);
	if (job == NULL || !step_allowed(action, job, actor, error))
		return NULL;
	if (step->to == VENTURE_JOB_INVOICED &&
		(!take_invoice(database, job, params, error) ||
		 !issue_draft(database, job, number(job, "invoice-id"), actor, error)))
		return NULL;
	g_object_set(job, "status", step->to, NULL);
	if (step->stamp != NULL)
	{
		g_autoptr(GDateTime) now = venture_time_now();
		g_object_set(job, step->stamp, now, NULL);
	}
	if (!venture_database_save(database, job, actor, error))
		return NULL;
	return g_steal_pointer(&job);
}

/* Invoicing owns its boundary because posting approval must start before
 * the first write. The ordinary registry transaction covers other steps.
 * Keep issue, ledger legs, approval consumption and the job in one unit.
 * Linking an already-issued invoice posts nothing and needs no new consent. */
static VentureEntity *
step_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	const JobStep *step = step_for(action);
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureEntity) prepared = NULL, selected = NULL;
	JsonNode *node = params != NULL ? g_hash_table_lookup(params, "invoice_id") : NULL;
	gint64 invoice = node != NULL && JSON_NODE_HOLDS_VALUE(node) ? json_node_get_int(node) : 0;

	if (step == NULL || step->to != VENTURE_JOB_INVOICED)
		return step_invoke_internal(action, entity, params, actor, error);
	prepared = load(database, VENTURE_TYPE_JOB, venture_entity_get_id(entity),
		venture_entity_get_organization_id(entity), "job", error);
	if (prepared == NULL || !step_allowed(action, prepared, actor, error) ||
		!take_invoice(database, prepared, params, error))
		return NULL;
	selected = load(database, VENTURE_TYPE_INVOICE, number(prepared, "invoice-id"),
		venture_entity_get_organization_id(prepared), "invoice-id", error);
	if (selected == NULL)
		return NULL;
	if (choice(selected, "status") == VENTURE_INVOICE_STATUS_DRAFT)
	{
		operation = venture_accounting_operation_begin(database, "jobs.invoiced", entity, NULL,
			g_variant_new("(x)", invoice), venture_entity_get_organization_id(entity), actor, error);
		if (operation == NULL)
			return NULL;
	}
	if (!venture_database_begin(database, error))
		return NULL;
	result = step_invoke_internal(action, entity, params, actor, error);
	if (result == NULL)
	{
		venture_database_rollback(database);
		return NULL;
	}
	if (!venture_database_commit(database, error) ||
		(operation != NULL && !venture_accounting_operation_finish(operation, error)))
		return NULL;
	return g_steal_pointer(&result);
}

/* --- Sending a technician ------------------------------------------------------ */

static gboolean
param_bool(GHashTable *params, const gchar *name)
{
	JsonNode *node = params != NULL ? g_hash_table_lookup(params, name) : NULL;
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return FALSE;
	if (json_node_get_value_type(node) == G_TYPE_BOOLEAN)
		return json_node_get_boolean(node);
	if (json_node_get_value_type(node) == G_TYPE_STRING)
		return g_strcmp0(json_node_get_string(node), "true") == 0 || g_strcmp0(json_node_get_string(node), "1") == 0;
	return json_node_get_int(node) != 0;
}

static gboolean
assign_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	gint status;
	(void)action;
	(void)actor;
	if (!enabled("job"))
		return refuse(error, "module", "The jobs module is switched off");
	if (!VENTURE_IS_JOB(entity))
		return refuse(error, "job", "That is not a job");
	status = choice(entity, "status");
	if (status == VENTURE_JOB_DONE || status == VENTURE_JOB_INVOICED || status == VENTURE_JOB_CANCELLED)
		return refuse(error, "status", "Technicians are sent to jobs that are still to be done");
	return TRUE;
}

/* Puts a person on the job; "lead" makes them the one the customer deals
 * with, and the job has one lead at a time. Sending someone already on it
 * only changes whether they lead. */
static VentureEntity *
assign_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	JsonNode *node = params != NULL ? g_hash_table_lookup(params, "user_id") : NULL;
	gint64 user = node != NULL && JSON_NODE_HOLDS_VALUE(node) ? json_node_get_int(node) : 0;
	gboolean lead = param_bool(params, "lead");
	gint64 org = venture_entity_get_organization_id(entity);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOB_TECHNICIAN);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) mine = NULL;
	guint i;
	if (!assign_allowed(action, entity, actor, error))
		return NULL;
	if (user <= 0)
		return refuse(error, "user_id", "Choose who to send") ? NULL : NULL;
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "job-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(entity), NULL);
	rows = venture_database_find(database, query, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		gboolean was_lead = FALSE;
		g_object_get(row, "lead", &was_lead, NULL);
		if (number(row, "user-id") == user)
			mine = g_object_ref(row);
		else if (lead && was_lead)
		{
			g_object_set(row, "lead", FALSE, NULL);
			if (!venture_database_save(database, row, actor, error))
				return NULL;
		}
	}
	if (mine == NULL)
	{
		mine = g_object_new(VENTURE_TYPE_JOB_TECHNICIAN, "job-id", venture_entity_get_id(entity), "user-id", user, NULL);
		venture_entity_set_organization_id(mine, org);
	}
	g_object_set(mine, "lead", lead, NULL);
	if (!venture_database_save(database, mine, actor, error))
		return NULL;
	return venture_database_get(database, VENTURE_TYPE_JOB, venture_entity_get_id(entity), error);
}

static void
register_assign(VentureActionRegistry *registry, VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	VentureFieldSpec *who = venture_field_spec_new("user_id", "Technician", VENTURE_FIELD_KIND_REFERENCE);
	who->reference_type = g_strdup("user");
	who->required = TRUE;
	g_ptr_array_add(parameters, who);
	g_ptr_array_add(parameters, venture_field_spec_new("lead", "Lead technician", VENTURE_FIELD_KIND_BOOLEAN));
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "job", "name", "assign", "label", "Send a technician",
		"description", "Put a person on the job; lead makes them the one the customer deals with",
		"parameters", parameters, "stageable", FALSE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(registry, action, assign_allowed, assign_invoke, database, NULL, &error))
		g_error("Job action registration: %s", error->message);
}

/* --- Maintenance visits ------------------------------------------------------- */

static gboolean
book_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) ended = NULL;
	(void)action;
	(void)actor;
	if (!enabled("maintenance_plan"))
		return refuse(error, "module", "The jobs module is switched off");
	if (!VENTURE_IS_MAINTENANCE_PLAN(entity))
		return refuse(error, "maintenance_plan", "That is not a maintenance plan");
	g_object_get(entity, "ended-at", &ended, NULL);
	if (ended != NULL)
		return refuse(error, "ended-at", "The plan has ended; its visits are no longer booked");
	return TRUE;
}

/* Makes the visit's job -- the customer, the place, what the visit is,
 * the equipment in its notes -- and moves the plan's next visit on by its
 * interval, so the visit is not due twice. The job's time is set on the
 * job, once the customer has picked one. Returns the job. */
static VentureEntity *
book_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	g_autoptr(VentureEntity) plan = NULL;
	g_autoptr(VentureEntity) job = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autoptr(GDateTime) moved = NULL;
	g_autofree gchar *visit = NULL;
	g_autofree gchar *problem = NULL;
	gint64 org = venture_entity_get_organization_id(entity);
	gint64 every;
	(void)params;
	if (!book_allowed(action, entity, actor, error))
		return NULL;
	plan = venture_database_get(database, VENTURE_TYPE_MAINTENANCE_PLAN, venture_entity_get_id(entity), error);
	if (plan == NULL)
		return NULL;
	g_object_get(plan, "visit", &visit, "next-visit", &due, NULL);
	every = number(plan, "every-months");
	if (every <= 0)
		every = 12;
	if (number(plan, "equipment-id") != 0)
	{
		g_autoptr(VentureEntity) equipment = venture_database_get(database, VENTURE_TYPE_EQUIPMENT,
			number(plan, "equipment-id"), NULL);
		g_autofree gchar *name = equipment != NULL ? venture_entity_get_display_name(equipment) : NULL;
		if (!venture_string_is_empty(name))
			problem = g_strdup_printf("Maintenance visit: %s", name);
	}
	job = g_object_new(VENTURE_TYPE_JOB, "title", visit, "company-id", number(plan, "company-id"),
		"property-id", number(plan, "property-id"), "maintenance-plan-id", venture_entity_get_id(plan),
		"problem", problem != NULL ? problem : "Maintenance visit", NULL);
	venture_entity_set_organization_id(job, org);
	if (!venture_database_save(database, job, actor, error))
		return NULL;
	/* From the day it was due, not from today: a visit booked late does
	 * not push every later one back. */
	if (due == NULL)
		due = venture_time_now();
	moved = g_date_time_add_months(due, (gint)every);
	g_object_set(plan, "next-visit", moved, NULL);
	if (!venture_database_save(database, plan, actor, error))
		return NULL;
	return g_steal_pointer(&job);
}

static void
register_book(VentureActionRegistry *registry, VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "maintenance_plan", "name", "book", "label", "Book the visit",
		"description", "Make the job for the next visit and move the plan's next visit on by its interval",
		"parameters", parameters, "stageable", FALSE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(registry, action, book_allowed, book_invoke, database, NULL, &error))
		g_error("Maintenance plan action registration: %s", error->message);
}

void
venture_jobs_actions_register(VentureDatabase *database)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(database);
	guint i;
	for (i = 0; i < G_N_ELEMENTS(job_steps); i++)
	{
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		g_autoptr(GError) error = NULL;
		if (job_steps[i].to == VENTURE_JOB_INVOICED)
			g_ptr_array_add(parameters, venture_field_spec_new("invoice_id", "Invoice", VENTURE_FIELD_KIND_INTEGER));
		action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
			"type-name", "job", "name", job_steps[i].name, "label", job_steps[i].label,
			"description", job_steps[i].description, "parameters", parameters, "stageable", FALSE,
			"service-transaction", job_steps[i].to == VENTURE_JOB_INVOICED,
			"roles", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!venture_action_registry_register(registry, action, step_allowed, step_invoke, database, NULL, &error))
			g_error("Job action registration: %s", error->message);
	}
	register_assign(registry, database);
	register_book(registry, database);
}
