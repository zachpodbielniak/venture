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
	if (end != NULL && start == NULL)
		return refuse(error, "window-start", "An arrival window needs its start as well as its end");
	if (start != NULL && end != NULL && g_date_time_compare(end, start) <= 0)
		return refuse(error, "window-end", "The arrival window must end after it starts");
	if (!same_customer(database, job, "quote", VENTURE_TYPE_QUOTE, "quote-id", "Quotes", error) ||
	    !same_customer(database, job, "invoice", VENTURE_TYPE_INVOICE, "invoice-id", "Invoices", error))
		return FALSE;
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
	return TRUE;
}

static VentureEntity *
step_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	const JobStep *step = step_for(action);
	g_autoptr(VentureEntity) job = NULL;
	if (!step_allowed(action, entity, actor, error))
		return NULL;
	job = venture_database_get(database, VENTURE_TYPE_JOB, venture_entity_get_id(entity), error);
	if (job == NULL)
		return NULL;
	if (step->to == VENTURE_JOB_INVOICED && !take_invoice(database, job, params, error))
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
			"service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!venture_action_registry_register(registry, action, step_allowed, step_invoke, database, NULL, &error))
			g_error("Job action registration: %s", error->message);
	}
}
