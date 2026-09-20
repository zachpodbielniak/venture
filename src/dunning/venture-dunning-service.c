/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

/*
 * Overdue-invoice reminders. The receivables module knows due dates and
 * balances, the mail module owns submission, activities own next actions;
 * this service only decides which step of which policy an invoice has
 * reached today and records that decision once. It deliberately does not
 * reuse the sequences engine: a sequence is enrolled per contact and paced
 * from enrollment, a reminder is paced from an invoice's due date and ends
 * the moment the invoice is settled, so the identity that must be unique
 * is (invoice, policy, step offset), which is what dunning_event.dunning_key
 * pins. The offset, not the position: an operator who prepends a step to a
 * policy moves every position, and a position-keyed history then re-sends
 * one reminder and skips another.
 */

struct _VentureDunningService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *permit;
	gchar *base_url;
	gboolean actions;
};

G_DEFINE_FINAL_TYPE(VentureDunningService, venture_dunning_service, G_TYPE_OBJECT)

enum { PROP_0, PROP_DATABASE, PROP_BASE_URL, N_PROPS };
static GParamSpec *properties[N_PROPS];

#define STATUS_QUEUED "queued"
#define STATUS_SENT "sent"
#define STATUS_ESCALATED "escalated"
#define STATUS_SUPPRESSED "suppressed"
#define STATUS_FAILED "failed"
#define STATUS_CANCELLED "cancelled"

/* A reminder that sat in the outbox this long -- mail unconfigured, the relay
 * down for a week -- describes a conversation that has moved on. Sending it
 * on recovery, next to the step queued after it, is the burst the superseded
 * rule exists to prevent, so it is cancelled instead. */
#define VENTURE_DUNNING_STALE_DAYS (7)
#define VENTURE_DUNNING_MAX_STEPS (20)
#define VENTURE_DUNNING_MAX_OFFSET (365)

/* What a test send puts where the customer's pay link would be: the link is
 * the customer's bearer credential and never goes to an operator's inbox. */
#define VENTURE_DUNNING_TEST_LINK "[pay link withheld from test sends]"

typedef struct
{
	gint64 offset;
	gint64 template_id;
} Step;

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureDunningService: %s", message);
	return FALSE;
}

static gint64
number(VentureEntity *e, const gchar *field)
{
	gint64 value = 0;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gboolean
flag(VentureEntity *e, const gchar *field)
{
	gboolean value = FALSE;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gchar *
text(VentureEntity *e, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static GDateTime *
when(VentureEntity *e, const gchar *field)
{
	GDateTime *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static VentureEntity *
new_record(GType type, gint64 org)
{
	VentureEntity *e = g_object_new(type, NULL);
	venture_entity_set_organization_id(e, org);
	return e;
}

static gboolean
enabled(GError **error)
{
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "dunning_policy") == G_TYPE_INVALID)
		return refuse(error, "The dunning module is disabled");
	return TRUE;
}

/* Calendar days from @due to @day, so an offset of -3 is eligible from the
 * third calendar day before the due date and never earlier in that day. */
static gint
calendar_days(GDateTime *day, GDateTime *due)
{
	GDate a, b;
	g_date_clear(&a, 1);
	g_date_clear(&b, 1);
	g_date_set_dmy(&a, (GDateDay)g_date_time_get_day_of_month(day), (GDateMonth)g_date_time_get_month(day), (GDateYear)g_date_time_get_year(day));
	g_date_set_dmy(&b, (GDateDay)g_date_time_get_day_of_month(due), (GDateMonth)g_date_time_get_month(due), (GDateYear)g_date_time_get_year(due));
	return g_date_days_between(&b, &a);
}

/* Midnight UTC of @base's calendar day plus @days: the day a step falls due. */
static GDateTime *
day_after(GDateTime *base, gint64 days)
{
	g_autoptr(GDateTime) midnight = g_date_time_new_utc(g_date_time_get_year(base), g_date_time_get_month(base),
		g_date_time_get_day_of_month(base), 0, 0, 0);
	return g_date_time_add_days(midnight, (gint)days);
}

static gboolean
json_integer(JsonObject *object, const gchar *member, gint64 *value)
{
	JsonNode *node = json_object_get_member(object, member);
	/* A string, a fraction or a boolean is refused rather than coerced:
	 * "7.9" silently becoming 7 is a reminder a day early. */
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return FALSE;
	*value = json_node_get_int(node);
	return TRUE;
}

/* --- Policy steps --------------------------------------------------------- */

static GArray *
parse_steps(const gchar *json, gboolean final_escalation, GError **error)
{
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GArray) steps = g_array_new(FALSE, TRUE, sizeof(Step));
	JsonArray *array;
	guint length;
	guint i;
	if (venture_string_is_empty(json))
		return g_steal_pointer(&steps);
	root = json_from_string(json, error);
	if (root == NULL)
		return NULL;
	if (!JSON_NODE_HOLDS_ARRAY(root))
	{
		refuse(error, "steps must be a JSON array of {offset, template_id}");
		return NULL;
	}
	array = json_node_get_array(root);
	length = json_array_get_length(array);
	if (length > VENTURE_DUNNING_MAX_STEPS)
	{
		refuse(error, "A policy has at most 20 steps");
		return NULL;
	}
	for (i = 0; i < length; i++)
	{
		JsonNode *node = json_array_get_element(array, i);
		JsonNode *template_node;
		JsonObject *object;
		Step step;
		if (!JSON_NODE_HOLDS_OBJECT(node))
		{
			refuse(error, "Each step must be an object with offset and template_id");
			return NULL;
		}
		object = json_node_get_object(node);
		if (!json_object_has_member(object, "offset") || !json_integer(object, "offset", &step.offset))
		{
			refuse(error, "Each step needs an offset in whole days, written as a JSON integer");
			return NULL;
		}
		if (step.offset > VENTURE_DUNNING_MAX_OFFSET || step.offset < -VENTURE_DUNNING_MAX_OFFSET)
		{
			refuse(error, "Step offsets must be within 365 days of the due date");
			return NULL;
		}
		step.template_id = 0;
		template_node = json_object_get_member(object, "template_id");
		if (template_node != NULL && !JSON_NODE_HOLDS_NULL(template_node))
		{
			if (!json_integer(object, "template_id", &step.template_id) || step.template_id <= 0)
			{
				refuse(error, "template_id must name a mail_template by its integer id");
				return NULL;
			}
		}
		else if (!final_escalation || i + 1 != length)
		{
			/* Only the step that escalates sends no email. */
			refuse(error, "Each step must name a mail_template; only an escalating last step may omit it");
			return NULL;
		}
		if (steps->len > 0 && g_array_index(steps, Step, steps->len - 1).offset >= step.offset)
		{
			refuse(error, "Step offsets must strictly increase");
			return NULL;
		}
		g_array_append_val(steps, step);
	}
	return g_steal_pointer(&steps);
}

/* --- Save validators ------------------------------------------------------ */

static gboolean
policy_validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	g_autofree gchar *json = NULL;
	g_autofree gchar *last_sweep = NULL;
	g_autoptr(GArray) steps = NULL;
	guint i;
	(void)previous;
	(void)data;
	json = text(entity, "steps");
	steps = parse_steps(json, flag(entity, "final-escalation"), error);
	if (steps == NULL)
		return FALSE;
	/* An empty default is every invoice in the organization quietly dropping
	 * out of dunning, which looks exactly like a policy that works. */
	if (flag(entity, "is-default") && steps->len == 0)
		return refuse(error, "An organization default policy needs at least one step");
	last_sweep = text(entity, "last-sweep");
	if (!venture_string_is_empty(last_sweep))
		return refuse(error, "last_sweep is the sweep action's answer and is never stored");
	for (i = 0; i < steps->len; i++)
	{
		g_autoptr(VentureEntity) template = NULL;
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_template");
		if (g_array_index(steps, Step, i).template_id <= 0)
			continue;
		if (type == G_TYPE_INVALID)
			return refuse(error, "Reminder templates need the mail module");
		template = venture_database_get(db, type, g_array_index(steps, Step, i).template_id, NULL);
		if (template == NULL || venture_entity_is_deleted(template) ||
			venture_entity_get_organization_id(template) != venture_entity_get_organization_id(entity))
			return refuse(error, "Each step must name a live mail_template of the same organization");
	}
	if (flag(entity, "is-default"))
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DUNNING_POLICY);
		g_autoptr(GPtrArray) rows = NULL;
		venture_query_set_organization(query, venture_entity_get_organization_id(entity));
		venture_query_set_limit(query, 0);
		rows = venture_database_find(db, query, error);
		if (rows == NULL)
			return FALSE;
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *other = g_ptr_array_index(rows, i);
			if (venture_entity_get_id(other) != venture_entity_get_id(entity) && flag(other, "is-default"))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
					"VentureDunningService: dunning_policy #%" G_GINT64_FORMAT " is already the organization default; clear it first",
					venture_entity_get_id(other));
				return FALSE;
			}
		}
	}
	return TRUE;
}

static gboolean
event_validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	VentureDunningService *self = data;
	(void)db;
	(void)previous;
	if (self->permit == entity)
		return TRUE;
	return refuse(error, "Reminder history is written by the sweep; use dunning_policy sweep");
}

static gboolean
save_event(VentureDunningService *self, VentureEntity *event, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->permit = event;
	ok = venture_database_save(self->database, event, actor, error);
	self->permit = NULL;
	return ok;
}

gboolean
venture_dunning_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	(void)database;
	/* Saves are the event validator's business; this guard is for removal.
	 * A deleted event is a step the sweep would count as recorded yet no page
	 * shows, and a purged one is a reminder the customer may receive twice. */
	if (!removal || record == NULL || !VENTURE_IS_DUNNING_EVENT(record))
		return TRUE;
	return refuse(error, "Reminder history cannot be removed; it is what stops a reminder being sent twice");
}

/* --- One sweep ------------------------------------------------------------ */

/* The state of one call: the clocks, the per-sweep caches and the answer. */
typedef struct
{
	gint64 org;
	GDateTime *as_of;
	GDateTime *now;
	gboolean dry_run;
	VentureEntity *default_policy;
	gboolean default_loaded;
	GHashTable *noted;
	guint invoices;
	guint queued;
	guint escalated;
	guint suppressed;
	guint failed;
	JsonArray *failed_invoices;
	JsonArray *warnings;
	JsonArray *plan;
} Sweep;

static void
sweep_clear(Sweep *sweep)
{
	g_clear_pointer(&sweep->as_of, g_date_time_unref);
	g_clear_pointer(&sweep->now, g_date_time_unref);
	g_clear_object(&sweep->default_policy);
	g_clear_pointer(&sweep->noted, g_hash_table_unref);
	g_clear_pointer(&sweep->failed_invoices, json_array_unref);
	g_clear_pointer(&sweep->warnings, json_array_unref);
	g_clear_pointer(&sweep->plan, json_array_unref);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Sweep, sweep_clear)

/* Adds a warning to the sweep's answer; @log also raises it in the log,
 * which is for the conditions an operator must fix rather than read about. */
static void sweep_note(Sweep *sweep, gboolean log, const gchar *format, ...) G_GNUC_PRINTF(3, 4);
static void
sweep_note(Sweep *sweep, gboolean log, const gchar *format, ...)
{
	g_autofree gchar *message = NULL;
	va_list args;
	va_start(args, format);
	message = g_strdup_vprintf(format, args);
	va_end(args);
	json_array_add_string_element(sweep->warnings, message);
	if (log)
		g_warning("dunning: %s", message);
}

/* TRUE the first time @key is seen in this sweep: a missing policy named by
 * four hundred invoices is one warning, not four hundred. */
static gboolean
sweep_first(Sweep *sweep, const gchar *kind, gint64 id)
{
	g_autofree gchar *key = NULL;
	if (sweep == NULL)
		return FALSE;
	key = g_strdup_printf("%s:%" G_GINT64_FORMAT, kind, id);
	return g_hash_table_add(sweep->noted, g_steal_pointer(&key));
}

/* --- Eligibility ---------------------------------------------------------- */

static VentureEntity *
load(VentureDunningService *self, GType type, gint64 id, gint64 org)
{
	g_autoptr(VentureEntity) row = NULL;
	if (id <= 0 || type == G_TYPE_INVALID)
		return NULL;
	row = venture_database_get(self->database, type, id, NULL);
	if (row == NULL || venture_entity_is_deleted(row) || venture_entity_get_organization_id(row) != org)
		return NULL;
	return g_steal_pointer(&row);
}

static VentureEntity *
default_policy(VentureDunningService *self, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DUNNING_POLICY);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
		if (flag(g_ptr_array_index(rows, i), "is-default"))
			return g_object_ref(g_ptr_array_index(rows, i));
	return NULL;
}

/* The default is looked up once per sweep rather than once per invoice. */
static VentureEntity *
organization_default(VentureDunningService *self, Sweep *sweep, gint64 org, GError **error)
{
	if (sweep == NULL)
		return default_policy(self, org, error);
	if (!sweep->default_loaded)
	{
		g_autoptr(GError) local = NULL;
		sweep->default_policy = default_policy(self, org, &local);
		if (local != NULL)
		{
			g_propagate_error(error, g_steal_pointer(&local));
			return NULL;
		}
		sweep->default_loaded = TRUE;
	}
	return sweep->default_policy != NULL ? g_object_ref(sweep->default_policy) : NULL;
}

/* The invoice's own policy, then its customer's, then the organization
 * default; an invoice switched off names none at all. A level naming a
 * deleted policy falls through to the next rather than to nothing: the
 * operator deleted a policy, not the decision to collect. */
static VentureEntity *
resolve_policy(VentureDunningService *self, Sweep *sweep, VentureEntity *invoice, VentureEntity *company, GError **error)
{
	gint64 org = venture_entity_get_organization_id(invoice);
	gint64 id;
	if (flag(invoice, "dunning-disabled"))
		return NULL;
	id = number(invoice, "dunning-policy-id");
	if (id > 0)
	{
		VentureEntity *policy = load(self, VENTURE_TYPE_DUNNING_POLICY, id, org);
		if (policy != NULL)
			return policy;
		if (sweep_first(sweep, "missing", id))
			sweep_note(sweep, TRUE, "invoice #%" G_GINT64_FORMAT " names dunning_policy #%" G_GINT64_FORMAT
				", which is deleted or missing; using its customer's policy or the default",
				venture_entity_get_id(invoice), id);
	}
	if (company != NULL && (id = number(company, "dunning-policy-id")) > 0)
	{
		VentureEntity *policy = load(self, VENTURE_TYPE_DUNNING_POLICY, id, org);
		if (policy != NULL)
			return policy;
		if (sweep_first(sweep, "missing", id))
			sweep_note(sweep, TRUE, "company #%" G_GINT64_FORMAT " names dunning_policy #%" G_GINT64_FORMAT
				", which is deleted or missing; using the organization default",
				venture_entity_get_id(company), id);
	}
	return organization_default(self, sweep, org, error);
}

/* Everything one invoice's decision needs, loaded once. */
typedef struct
{
	VentureEntity *invoice;
	VentureEntity *company;
	VentureEntity *contact;
	VentureEntity *policy;
	GArray *steps;
	GDateTime *due;
	gint days;
	/* Days from the due date to the policy's adoption; G_MININT when unknown. */
	gint adoption;
	VentureMoney *balance;
	gboolean recorded;
	gint64 top_offset;
	gboolean catch_up;
	gboolean emailed;
	gboolean escalated;
	VentureEntity *last_event;
} Dossier;

static void
dossier_clear(Dossier *d)
{
	g_clear_object(&d->invoice);
	g_clear_object(&d->company);
	g_clear_object(&d->contact);
	g_clear_object(&d->policy);
	g_clear_pointer(&d->steps, g_array_unref);
	g_clear_pointer(&d->due, g_date_time_unref);
	g_clear_pointer(&d->balance, venture_money_free);
	g_clear_object(&d->last_event);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Dossier, dossier_clear)

/* Only rows that load live are kept: a soft-deleted customer or contact is
 * simply absent, so nothing written from here names a row the save's
 * reference check refuses. That refusal used to wedge the sweep on one
 * invoice, every day, for every invoice behind it. */
static gboolean
dossier_open(VentureDunningService *self, Sweep *sweep, Dossier *d, VentureEntity *invoice, GDateTime *as_of,
	gboolean with_policy, GError **error)
{
	gint64 org = venture_entity_get_organization_id(invoice);
	memset(d, 0, sizeof(*d));
	d->invoice = g_object_ref(invoice);
	d->company = load(self, VENTURE_TYPE_COMPANY, number(invoice, "company-id"), org);
	d->contact = load(self, VENTURE_TYPE_CONTACT, number(invoice, "contact-id"), org);
	d->due = when(invoice, "due-at");
	d->days = d->due != NULL ? calendar_days(as_of, d->due) : 0;
	d->adoption = G_MININT;
	if (!with_policy)
		return TRUE;
	{
		g_autoptr(GError) local = NULL;
		g_autofree gchar *json = NULL;
		g_autoptr(GDateTime) adopted = NULL;
		d->policy = resolve_policy(self, sweep, invoice, d->company, &local);
		if (local != NULL)
		{
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		if (d->policy == NULL)
			return TRUE;
		json = text(d->policy, "steps");
		d->steps = parse_steps(json, flag(d->policy, "final-escalation"), &local);
		if (d->steps == NULL && sweep_first(sweep, "invalid", venture_entity_get_id(d->policy)))
			sweep_note(sweep, FALSE, "dunning_policy #%" G_GINT64_FORMAT " has invalid steps and was skipped: %s",
				venture_entity_get_id(d->policy), local->message);
		adopted = when(d->policy, "adopted-at");
		if (adopted == NULL && venture_entity_get_created_at(d->policy) != NULL)
			adopted = g_date_time_ref(venture_entity_get_created_at(d->policy));
		if (adopted != NULL && d->due != NULL)
			d->adoption = calendar_days(adopted, d->due);
	}
	return TRUE;
}

static const VentureMoney *
dossier_balance(VentureDunningService *self, Dossier *d)
{
	if (d->balance == NULL)
		d->balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database),
			venture_entity_get_id(d->invoice), NULL, NULL);
	return d->balance;
}

static gboolean
escalating(Dossier *d, guint index)
{
	return flag(d->policy, "final-escalation") && index + 1 == d->steps->len;
}

/* Why no further reminder may go to this invoice, or NULL while it is
 * collectible. Only a customer's opt-out is worth a recorded event; a
 * settled or disputed invoice has simply left dunning. */
static const gchar *
stop_reason(VentureDunningService *self, Dossier *d)
{
	g_autofree gchar *workflow = NULL;
	const VentureMoney *balance;
	gint status = 0;
	g_object_get(d->invoice, "status", &status, "workflow-state", &workflow, NULL);
	if (status == VENTURE_INVOICE_STATUS_PAID)
		return "settled";
	if (status != VENTURE_INVOICE_STATUS_SENT && status != VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
		return "not_issued";
	if (g_strcmp0(workflow, "disputed") == 0)
		return "disputed";
	balance = dossier_balance(self, d);
	if (balance == NULL || venture_money_is_zero(balance) || venture_money_is_negative(balance))
		return "settled";
	if (flag(d->invoice, "dunning-disabled"))
		return "invoice_disabled";
	if (d->company != NULL && flag(d->company, "dunning-opt-out"))
		return "company_opt_out";
	return NULL;
}

static gboolean
recorded_reason(const gchar *reason)
{
	return g_strcmp0(reason, "company_opt_out") == 0;
}

/* A temporary hold: a promise to pay on the invoice or the customer, or the
 * recurring module's collection case saying a person is handling it. Not
 * recorded, because nothing was decided -- when the hold ends the current
 * step goes out and the superseded rule keeps it from being a burst. Both
 * modules chasing one invoice is how a customer gets two different letters
 * the same morning. */
static const gchar *
hold_reason(VentureDunningService *self, Dossier *d, GDateTime *at, GDateTime **until)
{
	g_autoptr(GDateTime) invoice_pause = when(d->invoice, "dunning-paused-until");
	g_autoptr(GDateTime) company_pause = d->company != NULL ? when(d->company, "dunning-paused-until") : NULL;
	GType case_type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "collection_case");
	if (invoice_pause != NULL && calendar_days(invoice_pause, at) > 0)
	{
		if (until != NULL)
			*until = g_date_time_ref(invoice_pause);
		return "paused";
	}
	if (company_pause != NULL && calendar_days(company_pause, at) > 0)
	{
		if (until != NULL)
			*until = g_date_time_ref(company_pause);
		return "paused";
	}
	if (case_type != G_TYPE_INVALID)
	{
		g_autoptr(VentureQuery) query = venture_query_new(case_type);
		g_autoptr(VentureEntity) kase = NULL;
		venture_query_set_organization(query, venture_entity_get_organization_id(d->invoice));
		venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(d->invoice), NULL);
		kase = venture_database_find_one(self->database, query, NULL);
		if (kase != NULL)
		{
			g_autoptr(GDateTime) promised = when(kase, "promised-at");
			gint status = 0;
			g_object_get(kase, "status", &status, NULL);
			/* held and disputed, in VentureCollectionCaseStatus */
			if (status == 1 || status == 2)
				return "collection_case";
			if (promised != NULL && calendar_days(promised, at) > 0)
			{
				if (until != NULL)
					*until = g_date_time_ref(promised);
				return "collection_case";
			}
		}
	}
	return NULL;
}

static gboolean
superseded_like(const gchar *reason)
{
	return g_strcmp0(reason, "superseded") == 0 || g_strcmp0(reason, "before_adoption") == 0;
}

/* What has already been decided for this invoice under this policy. Rows are
 * read by offset-days, which older rows keyed by position also carry, so
 * history written before steps were identified by offset still counts. */
static gboolean
load_history(VentureDunningService *self, Dossier *d, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DUNNING_EVENT);
	g_autoptr(GPtrArray) rows = NULL;
	gboolean top_superseded = FALSE;
	guint i;
	venture_query_set_organization(query, venture_entity_get_organization_id(d->invoice));
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(d->invoice), NULL);
	venture_query_add_filter_int(query, "policy-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(d->policy), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *status = text(row, "delivery-status");
		g_autofree gchar *reason = text(row, "suppressed-reason");
		gint64 offset = number(row, "offset-days");
		gboolean passed_over = superseded_like(reason);
		if (g_strcmp0(status, STATUS_ESCALATED) == 0)
			d->escalated = TRUE;
		else if (!passed_over)
			d->emailed = TRUE;
		if (!d->recorded || offset > d->top_offset)
		{
			d->top_offset = offset;
			top_superseded = passed_over;
		}
		else if (offset == d->top_offset && !passed_over)
			top_superseded = FALSE;
		d->recorded = TRUE;
		g_set_object(&d->last_event, row);
	}
	/* The newest decision was a catch-up: a step that fell due before the
	 * policy was adopted, sent because it was the current one. */
	d->catch_up = d->recorded && !top_superseded && d->adoption != G_MININT && d->top_offset < d->adoption;
	return TRUE;
}

/* The day, counted from the due date, from which step @index may act. After
 * a catch-up reminder the next step keeps the policy's spacing counted from
 * the adoption day, or catching up would be an email today and an escalation
 * tomorrow. */
static gint64
eligible_day(Dossier *d, guint index)
{
	gint64 offset = g_array_index(d->steps, Step, index).offset;
	if (d->catch_up)
		return MAX(offset, (gint64)d->adoption + (offset - d->top_offset));
	return offset;
}

/* Every step at or below the highest recorded offset is done, whatever its
 * own row says: when the newest step failed and its transaction rolled back,
 * the older steps it superseded must not come due again behind it. */
static gint
next_step_index(Dossier *d)
{
	guint i;
	for (i = 0; d->steps != NULL && i < d->steps->len; i++)
		if (!d->recorded || g_array_index(d->steps, Step, i).offset > d->top_offset)
			return (gint)i;
	return -1;
}

static GArray *
pending_steps(Dossier *d)
{
	GArray *pending = g_array_new(FALSE, FALSE, sizeof(guint));
	gint first = next_step_index(d);
	guint i;
	for (i = first < 0 ? d->steps->len : (guint)first; i < d->steps->len; i++)
	{
		if (eligible_day(d, i) > d->days)
			break;
		g_array_append_val(pending, i);
	}
	return pending;
}

/* --- Rendering ------------------------------------------------------------ */

/* The portal refuses to send an invitation over anything but a public HTTPS
 * origin; a pay link is the same bearer credential and gets the same rule. */
static gchar *
public_base(VentureDunningService *self)
{
	g_autoptr(GUri) uri = NULL;
	gchar *base;
	if (venture_string_is_empty(self->base_url))
		return NULL;
	uri = g_uri_parse(self->base_url, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL || g_strcmp0(g_uri_get_scheme(uri), "https") != 0 ||
		venture_string_is_empty(g_uri_get_host(uri)) || g_uri_get_userinfo(uri) != NULL ||
		g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL)
		return NULL;
	base = g_strdup(self->base_url);
	while (g_str_has_suffix(base, "/"))
		base[strlen(base) - 1] = '\0';
	return base;
}

/* The newest unrevoked portal access issued to the very mailbox the reminder
 * goes to. An access issued to someone else at the customer is their
 * credential, and mailing it to the accounts inbox hands it to a stranger. */
static gchar *
live_portal_link(VentureDunningService *self, gint64 org, VentureEntity *company, const gchar *recipient)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *wanted = NULL;
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "customer_portal_access");
	guint i;
	if (type == G_TYPE_INVALID || company == NULL || venture_string_is_empty(recipient))
		return NULL;
	base = public_base(self);
	if (base == NULL)
		return NULL;
	wanted = g_strstrip(g_strdup(recipient));
	query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(company), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(self->database, query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *access = g_ptr_array_index(rows, i);
		g_autofree gchar *token = NULL;
		g_autofree gchar *email = NULL;
		if (flag(access, "revoked"))
			continue;
		email = text(access, "email");
		if (email == NULL || g_ascii_strcasecmp(g_strstrip(email), wanted) != 0)
			continue;
		token = text(access, "token");
		if (venture_string_is_empty(token))
			continue;
		return g_strdup_printf("%s/portal/%s", base, token);
	}
	return NULL;
}

static gchar *
recipient_for(Dossier *d)
{
	g_autofree gchar *to = d->contact != NULL ? text(d->contact, "email") : NULL;
	if (venture_string_is_empty(to) && d->company != NULL)
	{
		g_free(to);
		to = text(d->company, "email");
	}
	if (venture_string_is_empty(to))
		return NULL;
	return g_steal_pointer(&to);
}

/* Invoice owner, then the customer's owner, then the policy's escalation
 * owner. Invoices predating the owner field would otherwise escalate to "". */
static gchar *
escalation_owner(VentureDunningService *self, Dossier *d)
{
	g_autofree gchar *owner = text(d->invoice, "owner");
	if (!venture_string_is_empty(owner))
		return g_steal_pointer(&owner);
	if (d->company != NULL && number(d->company, "owner-user-id") > 0)
	{
		g_autoptr(VentureEntity) user = venture_database_get(self->database, VENTURE_TYPE_USER,
			number(d->company, "owner-user-id"), NULL);
		if (user != NULL && !venture_entity_is_deleted(user))
		{
			g_free(owner);
			owner = text(user, "username");
			if (!venture_string_is_empty(owner))
				return g_steal_pointer(&owner);
		}
	}
	if (d->policy != NULL)
	{
		g_free(owner);
		owner = text(d->policy, "escalation-owner");
		if (!venture_string_is_empty(owner))
			return g_steal_pointer(&owner);
	}
	return g_strdup("");
}

/* The values a reminder template may name. Only public invoice fields
 * enter, plus the customer, the balance and the pay link; the link is a
 * bearer credential and is only ever placed in the private bodies. */
static JsonObject *
render_values(VentureDunningService *self, Dossier *d, const Step *step, guint step_no, const gchar *pay_link)
{
	g_autoptr(JsonNode) base = venture_serializable_to_json(VENTURE_SERIALIZABLE(d->invoice), FALSE);
	JsonObject *values = json_object_ref(json_node_get_object(base));
	const VentureMoney *balance = dossier_balance(self, d);
	g_autoptr(GDateTime) issued = when(d->invoice, "issued-at");
	g_autofree gchar *due_text = d->due ? venture_time_to_date_string(d->due, NULL) : g_strdup("");
	g_autofree gchar *issued_text = issued ? venture_time_to_date_string(issued, NULL) : g_strdup("");
	g_autofree gchar *display = balance ? venture_money_to_display_string(balance, TRUE) : g_strdup("");
	g_autofree gchar *exact = balance ? venture_money_to_string(balance) : g_strdup("");
	g_autofree gchar *customer = d->company ? text(d->company, "name") : NULL;
	g_autofree gchar *contact_name = d->contact ? text(d->contact, "name") : NULL;
	g_autofree gchar *policy_name = text(d->policy, "name");
	/* People read dates, not instants. */
	json_object_set_string_member(values, "due_at", due_text);
	json_object_set_string_member(values, "issued_at", issued_text);
	json_object_set_string_member(values, "open_balance", display);
	json_object_set_string_member(values, "open_balance_exact", exact);
	json_object_set_string_member(values, "customer_name", customer ? customer : "");
	json_object_set_string_member(values, "contact_name", contact_name ? contact_name : "");
	json_object_set_string_member(values, "policy_name", policy_name ? policy_name : "");
	json_object_set_string_member(values, "pay_link", pay_link ? pay_link : "");
	json_object_set_int_member(values, "days_overdue", d->days > 0 ? d->days : 0);
	json_object_set_int_member(values, "days_until_due", d->days < 0 ? -d->days : 0);
	json_object_set_int_member(values, "step", step_no);
	json_object_set_int_member(values, "offset_days", step->offset);
	return values;
}

/* Renders one step. With @private_link the link goes only into the private
 * text and HTML bodies the transport sends, never the stored public ones;
 * without it @pay_link (a placeholder, for a test send) is public. */
static VentureMailMessage *
render_message(VentureDunningService *self, Dossier *d, VentureEntity *template, const Step *step, guint step_no,
	const gchar *pay_link, gboolean private_link, GError **error)
{
	g_autoptr(JsonObject) public_values = render_values(self, d, step, step_no, private_link ? NULL : pay_link);
	g_autoptr(VentureMailMessage) message = venture_mail_template_render_values(VENTURE_MAIL_TEMPLATE(template), public_values, error);
	if (message == NULL)
		return NULL;
	if (private_link && pay_link != NULL)
	{
		g_autoptr(JsonObject) private_values = render_values(self, d, step, step_no, pay_link);
		g_autoptr(VentureMailMessage) private_message = venture_mail_template_render_values(VENTURE_MAIL_TEMPLATE(template), private_values, error);
		g_autofree gchar *private_text = NULL;
		g_autofree gchar *private_html = NULL;
		if (private_message == NULL)
			return NULL;
		g_object_get(private_message, "text-body", &private_text, "html-body", &private_html, NULL);
		g_object_set(message, "private-text-body", private_text,
			"private-html-body", venture_string_is_empty(private_html) ? NULL : private_html, NULL);
	}
	return g_steal_pointer(&message);
}

/* --- Writing decisions ---------------------------------------------------- */

typedef struct
{
	guint queued;
	guint escalated;
	guint suppressed;
	guint failed;
} Tally;

static VentureEntity *
new_event(Dossier *d, GDateTime *now, guint step_no, const Step *step, const gchar *key, const gchar *status)
{
	VentureEntity *event = new_record(VENTURE_TYPE_DUNNING_EVENT, venture_entity_get_organization_id(d->invoice));
	/* queued-at is the real time: a sweep run as of an earlier day still
	 * queued the message now, and staleness is measured from then. */
	g_object_set(event, "invoice-id", venture_entity_get_id(d->invoice), "policy-id", venture_entity_get_id(d->policy),
		"company-id", d->company != NULL ? venture_entity_get_id(d->company) : (gint64)0,
		"contact-id", d->contact != NULL ? venture_entity_get_id(d->contact) : (gint64)0,
		"step", (gint64)step_no, "offset-days", step->offset, "dunning-key", key, "queued-at", now,
		"delivery-status", status, NULL);
	return event;
}

static gboolean
record_status(VentureDunningService *self, Dossier *d, GDateTime *now, guint step_no, const Step *step,
	const gchar *key, const gchar *status, const gchar *reason, const gchar *last_error,
	const VentureActor *actor, Tally *tally, GError **error)
{
	g_autoptr(VentureEntity) event = new_event(d, now, step_no, step, key, status);
	g_object_set(event, "suppressed-reason", reason, "last-error", last_error, NULL);
	if (!save_event(self, event, actor, error))
		return FALSE;
	if (g_strcmp0(status, STATUS_FAILED) == 0)
		tally->failed++;
	else
		tally->suppressed++;
	return TRUE;
}

/* The final step: an owned next action, never another customer email. */
static gboolean
escalate(VentureDunningService *self, Dossier *d, GDateTime *now, GDateTime *as_of, guint step_no, const Step *step,
	const gchar *key, const VentureActor *actor, Tally *tally, GError **error)
{
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) activity = NULL;
	g_autofree gchar *invoice_number = text(d->invoice, "number");
	g_autofree gchar *owner = NULL;
	g_autofree gchar *subject = g_strdup_printf("collect: %s", invoice_number ? invoice_number : "");
	g_autofree gchar *body = NULL;
	const VentureMoney *balance = dossier_balance(self, d);
	g_autofree gchar *display = balance ? venture_money_to_display_string(balance, TRUE) : g_strdup("");
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "activity");
	if (type == G_TYPE_INVALID)
		/* The attempt is still recorded so the invoice is not silently forgotten. */
		return record_status(self, d, now, step_no, step, key, STATUS_FAILED, NULL,
			"Escalation needs the activities module", actor, tally, error);
	owner = escalation_owner(self, d);
	body = g_strdup_printf("Reminder policy reached its escalation step (step %u); %s still outstanding. Call, agree a date or write off.",
		step_no, display);
	activity = new_record(type, venture_entity_get_organization_id(d->invoice));
	g_object_set(activity, "subject", subject, "kind", VENTURE_ACTIVITY_KIND_FOLLOWUP, "body", body,
		"owner", owner, "due-at", as_of, "priority", VENTURE_PRIORITY_HIGH,
		"status", VENTURE_ACTIVITY_STATUS_PLANNED, "related-type", "invoice", "related-id", venture_entity_get_id(d->invoice),
		"company-id", d->company != NULL ? venture_entity_get_id(d->company) : (gint64)0,
		"contact-id", d->contact != NULL ? venture_entity_get_id(d->contact) : (gint64)0, NULL);
	if (!venture_database_save(self->database, activity, actor, error))
		return FALSE;
	event = new_event(d, now, step_no, step, key, STATUS_ESCALATED);
	g_object_set(event, "activity-id", venture_entity_get_id(activity), NULL);
	if (!save_event(self, event, actor, error))
		return FALSE;
	tally->escalated++;
	return TRUE;
}

static gboolean
send_step(VentureDunningService *self, Dossier *d, GDateTime *now, guint step_no, const Step *step,
	const gchar *key, const VentureActor *actor, Tally *tally, GError **error)
{
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) template = NULL;
	g_autofree gchar *recipient = recipient_for(d);
	g_autofree gchar *pay_link = NULL;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(GError) local = NULL;
	gint64 org = venture_entity_get_organization_id(d->invoice);
	if (recipient == NULL)
		return record_status(self, d, now, step_no, step, key, STATUS_SUPPRESSED, "no_recipient", NULL, actor, tally, error);
	template = load(self, venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_template"), step->template_id, org);
	if (template == NULL)
		return record_status(self, d, now, step_no, step, key, STATUS_FAILED, NULL,
			"The step's mail_template is missing", actor, tally, error);
	pay_link = live_portal_link(self, org, d->company, recipient);
	message = render_message(self, d, template, step, step_no, pay_link, TRUE, &local);
	if (message == NULL)
		return record_status(self, d, now, step_no, step, key, STATUS_FAILED, NULL, local->message, actor, tally, error);
	event = new_event(d, now, step_no, step, key, STATUS_QUEUED);
	g_object_set(event, "rendered-balance", (gpointer)dossier_balance(self, d), NULL);
	if (!save_event(self, event, actor, error))
		return FALSE;
	g_object_set(message, "to", recipient, "idempotency-key", key,
		"related-type", "dunning_event", "related-id", venture_entity_get_id(event), NULL);
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(self->database), message, actor, error);
	if (queued == NULL)
		return FALSE;
	g_object_set(event, "mail-message-id", venture_entity_get_id(VENTURE_ENTITY(queued)), NULL);
	if (!save_event(self, event, actor, error))
		return FALSE;
	tally->queued++;
	return TRUE;
}

/* After a per-invoice transaction failed, keep the attempt as evidence in a
 * transaction of its own. Without this a crash between enqueue and commit
 * would look, next sweep, like a step that was never due. */
static gboolean
record_failure(VentureDunningService *self, Dossier *d, GDateTime *now, guint step_no, const Step *step,
	const gchar *key, const gchar *why, const VentureActor *actor)
{
	g_autoptr(VentureEntity) event = new_event(d, now, step_no, step, key, STATUS_FAILED);
	g_autoptr(GError) local = NULL;
	g_object_set(event, "last-error", why, NULL);
	if (save_event(self, event, actor, &local))
		return TRUE;
	/* Nothing is left to hold the evidence; the log has to. */
	g_warning("dunning: could not record the failed attempt at %s: %s", key, local->message);
	return FALSE;
}

static gchar *
step_key(VentureEntity *invoice, VentureEntity *policy, gint64 offset)
{
	return g_strdup_printf("dunning:inv:%" G_GINT64_FORMAT ":policy:%" G_GINT64_FORMAT ":offset:%" G_GINT64_FORMAT,
		venture_entity_get_id(invoice), venture_entity_get_id(policy), offset);
}

/* --- Planning ------------------------------------------------------------- */

typedef enum
{
	PLAN_SEND,
	PLAN_ESCALATE,
	PLAN_SUPPRESS
} PlanKind;

typedef struct
{
	guint index;
	PlanKind kind;
	const gchar *reason;
} Planned;

/*
 * What the sweep would do for this invoice, with no writes. The customer
 * gets the current step only; older due steps are recorded as superseded,
 * or as before_adoption when they fell due before the policy was adopted.
 * A catch-up never escalates straight away: when the current step is the
 * escalation and no email step was ever decided for the invoice, the latest
 * email step is sent instead and the escalation waits its turn.
 */
static GArray *
plan_invoice(Dossier *d, GArray *pending, const gchar *reason)
{
	GArray *planned = g_array_new(FALSE, FALSE, sizeof(Planned));
	guint current = pending->len - 1;
	gboolean catching_up = d->adoption != G_MININT &&
		g_array_index(d->steps, Step, g_array_index(pending, guint, 0)).offset < d->adoption;
	guint k;
	if (catching_up && !d->emailed && pending->len >= 2 && escalating(d, g_array_index(pending, guint, current)))
		current--;
	for (k = 0; k <= current; k++)
	{
		Planned item;
		item.index = g_array_index(pending, guint, k);
		item.reason = NULL;
		if (k == current && escalating(d, item.index))
			/* A customer who opted out of email is still owed an internal
			 * collect task; the opt-out is about mail, not about money. */
			item.kind = PLAN_ESCALATE;
		else if (reason != NULL)
		{
			item.kind = PLAN_SUPPRESS;
			item.reason = reason;
		}
		else if (k == current)
			item.kind = PLAN_SEND;
		else
		{
			item.kind = PLAN_SUPPRESS;
			item.reason = g_array_index(d->steps, Step, item.index).offset < d->adoption ? "before_adoption" : "superseded";
		}
		g_array_append_val(planned, item);
	}
	return planned;
}

static void
describe_plan(VentureDunningService *self, Sweep *sweep, Dossier *d, GArray *planned)
{
	g_autofree gchar *invoice_number = text(d->invoice, "number");
	gint64 org = venture_entity_get_organization_id(d->invoice);
	guint k;
	for (k = 0; k < planned->len; k++)
	{
		const Planned *item = &g_array_index(planned, Planned, k);
		const Step *step = &g_array_index(d->steps, Step, item->index);
		JsonObject *entry = json_object_new();
		json_object_set_int_member(entry, "invoice_id", venture_entity_get_id(d->invoice));
		json_object_set_string_member(entry, "invoice_number", invoice_number ? invoice_number : "");
		json_object_set_int_member(entry, "step", item->index + 1);
		json_object_set_int_member(entry, "offset", step->offset);
		if (item->kind == PLAN_SUPPRESS)
		{
			json_object_set_string_member(entry, "outcome", "suppress");
			json_object_set_string_member(entry, "reason", item->reason);
			sweep->suppressed++;
		}
		else if (item->kind == PLAN_ESCALATE)
		{
			g_autofree gchar *owner = escalation_owner(self, d);
			if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "activity") == G_TYPE_INVALID)
			{
				json_object_set_string_member(entry, "outcome", "fail");
				json_object_set_string_member(entry, "reason", "Escalation needs the activities module");
				sweep->failed++;
			}
			else
			{
				json_object_set_string_member(entry, "outcome", "escalate");
				json_object_set_string_member(entry, "owner", owner);
				sweep->escalated++;
			}
		}
		else
		{
			g_autofree gchar *recipient = recipient_for(d);
			g_autoptr(VentureEntity) template = load(self,
				venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_template"), step->template_id, org);
			g_autoptr(VentureMailMessage) message = NULL;
			g_autoptr(GError) local = NULL;
			g_autofree gchar *subject = NULL;
			if (recipient == NULL)
			{
				json_object_set_string_member(entry, "outcome", "suppress");
				json_object_set_string_member(entry, "reason", "no_recipient");
				sweep->suppressed++;
			}
			else if (template == NULL)
			{
				json_object_set_string_member(entry, "outcome", "fail");
				json_object_set_string_member(entry, "reason", "The step's mail_template is missing");
				sweep->failed++;
			}
			else if ((message = render_message(self, d, template, step, item->index + 1, NULL, TRUE, &local)) == NULL)
			{
				json_object_set_string_member(entry, "outcome", "fail");
				json_object_set_string_member(entry, "reason", local->message);
				sweep->failed++;
			}
			else
			{
				g_object_get(message, "subject", &subject, NULL);
				json_object_set_string_member(entry, "outcome", "send");
				json_object_set_string_member(entry, "recipient", recipient);
				json_object_set_string_member(entry, "subject", subject ? subject : "");
				sweep->queued++;
			}
		}
		json_array_add_object_element(sweep->plan, entry);
	}
}

/* One invoice, one transaction. A failure rolls it back, records the attempt
 * on the current step on its own, and leaves the sweep to go on to the next
 * invoice: one broken invoice must not stop every invoice behind it. */
static void
apply_plan(VentureDunningService *self, Sweep *sweep, Dossier *d, GArray *planned, const VentureActor *actor)
{
	g_autoptr(GError) error = NULL;
	Tally tally;
	guint k;
	memset(&tally, 0, sizeof(tally));
	if (!venture_database_begin(self->database, &error))
		goto fail;
	for (k = 0; k < planned->len; k++)
	{
		const Planned *item = &g_array_index(planned, Planned, k);
		const Step *step = &g_array_index(d->steps, Step, item->index);
		g_autofree gchar *key = step_key(d->invoice, d->policy, step->offset);
		gboolean ok;
		if (item->kind == PLAN_SUPPRESS)
			ok = record_status(self, d, sweep->now, item->index + 1, step, key, STATUS_SUPPRESSED, item->reason, NULL,
				actor, &tally, &error);
		else if (item->kind == PLAN_ESCALATE)
			ok = escalate(self, d, sweep->now, sweep->as_of, item->index + 1, step, key, actor, &tally, &error);
		else
			ok = send_step(self, d, sweep->now, item->index + 1, step, key, actor, &tally, &error);
		if (!ok)
		{
			venture_database_rollback(self->database);
			goto fail;
		}
	}
	if (!venture_database_commit(self->database, &error))
		goto fail;
	sweep->queued += tally.queued;
	sweep->escalated += tally.escalated;
	sweep->suppressed += tally.suppressed;
	sweep->failed += tally.failed;
	return;
fail:
	{
		/* The newest step carries the failure, so the older steps it
		 * superseded count as done behind it rather than coming due again. */
		const Planned *last = &g_array_index(planned, Planned, planned->len - 1);
		const Step *step = &g_array_index(d->steps, Step, last->index);
		g_autofree gchar *key = step_key(d->invoice, d->policy, step->offset);
		const gchar *why = error != NULL ? error->message : "sweep failed";
		if (record_failure(self, d, sweep->now, last->index + 1, step, key, why, actor))
			sweep->failed++;
		json_array_add_int_element(sweep->failed_invoices, venture_entity_get_id(d->invoice));
		sweep_note(sweep, TRUE, "invoice #%" G_GINT64_FORMAT " failed and was skipped: %s",
			venture_entity_get_id(d->invoice), why);
	}
}

/* @touched is set when this invoice had a due step, including suppressed
 * ones, so a book of opted-out invoices cannot write unbounded events. The
 * cheap tests come first: the balance is only computed for an invoice with
 * a step actually due. */
static void
sweep_invoice(VentureDunningService *self, Sweep *sweep, VentureEntity *invoice, const VentureActor *actor, gboolean *touched)
{
	g_auto(Dossier) d;
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) pending = NULL;
	g_autoptr(GArray) planned = NULL;
	const gchar *reason;
	if (!dossier_open(self, sweep, &d, invoice, sweep->as_of, TRUE, &error))
		goto fail;
	if (d.policy == NULL || d.due == NULL || d.steps == NULL || d.steps->len == 0)
		return;
	if (g_array_index(d.steps, Step, 0).offset > d.days)
		return;
	if (!load_history(self, &d, &error))
		goto fail;
	pending = pending_steps(&d);
	if (pending->len == 0)
		return;
	if (hold_reason(self, &d, sweep->as_of, NULL) != NULL)
		return;
	reason = stop_reason(self, &d);
	if (reason != NULL && !recorded_reason(reason))
		return;
	*touched = TRUE;
	planned = plan_invoice(&d, pending, reason);
	if (sweep->dry_run)
		describe_plan(self, sweep, &d, planned);
	else
		apply_plan(self, sweep, &d, planned, actor);
	return;
fail:
	*touched = TRUE;
	json_array_add_int_element(sweep->failed_invoices, venture_entity_get_id(invoice));
	sweep_note(sweep, TRUE, "invoice #%" G_GINT64_FORMAT " could not be evaluated and was skipped: %s",
		venture_entity_get_id(invoice), error != NULL ? error->message : "unknown error");
}

/* The earliest first-step offset of any valid policy, which bounds how
 * recently an invoice can have fallen due and still have a step due now. */
static gboolean
earliest_offset(VentureDunningService *self, Sweep *sweep, gboolean *any, gint64 *earliest, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DUNNING_POLICY);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	*any = FALSE;
	*earliest = 0;
	venture_query_set_organization(query, sweep->org);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *policy = g_ptr_array_index(rows, i);
		g_autofree gchar *json = text(policy, "steps");
		g_autoptr(GError) local = NULL;
		g_autoptr(GArray) steps = parse_steps(json, flag(policy, "final-escalation"), &local);
		if (steps == NULL)
		{
			if (sweep_first(sweep, "invalid", venture_entity_get_id(policy)))
				sweep_note(sweep, FALSE, "dunning_policy #%" G_GINT64_FORMAT " has invalid steps and was skipped: %s",
					venture_entity_get_id(policy), local->message);
			continue;
		}
		if (steps->len == 0)
			continue;
		if (!*any || g_array_index(steps, Step, 0).offset < *earliest)
			*earliest = g_array_index(steps, Step, 0).offset;
		*any = TRUE;
	}
	return TRUE;
}

static GPtrArray *
open_invoices(VentureDunningService *self, gint64 org, gint status, const gchar *due_before, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	venture_query_set_organization(query, org);
	venture_query_add_filter_int(query, "status", VENTURE_FILTER_OP_EQ, status, NULL);
	if (due_before != NULL && !venture_query_add_filter_string(query, "due-at", VENTURE_FILTER_OP_LT, due_before, error))
		return NULL;
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	return venture_database_find(self->database, query, error);
}

JsonNode *
venture_dunning_service_sweep_detailed(VentureDunningService *self, gint64 organization_id, GDateTime *as_of,
	guint limit, gboolean dry_run, const VentureActor *actor, GError **error)
{
	g_auto(Sweep) sweep;
	g_autoptr(GPtrArray) sent = NULL;
	g_autoptr(GPtrArray) partial = NULL;
	g_autoptr(VentureMailer) mailer = NULL;
	g_autofree gchar *due_before = NULL;
	g_autofree gchar *as_of_text = NULL;
	JsonObject *answer;
	JsonNode *node;
	GPtrArray *sets[2];
	gboolean any = FALSE;
	gint64 earliest = 0;
	guint i;
	memset(&sweep, 0, sizeof(sweep));
	g_return_val_if_fail(VENTURE_IS_DUNNING_SERVICE(self), NULL);
	if (self->database == NULL)
		return refuse(error, "The database is gone") ? NULL : NULL;
	if (organization_id <= 0)
		return refuse(error, "An organization is required") ? NULL : NULL;
	if (!enabled(error))
		return NULL;
	if (venture_database_has_transaction(self->database))
		return refuse(error, "The sweep owns one transaction per invoice; call it outside a transaction") ? NULL : NULL;
	if (limit == 0)
		limit = 100;
	limit = MIN(limit, 1000);
	sweep.org = organization_id;
	sweep.dry_run = dry_run;
	sweep.now = venture_time_now();
	sweep.as_of = as_of != NULL ? g_date_time_ref(as_of) : g_date_time_ref(sweep.now);
	if (!dry_run)
	{
		/* A sweep run as of next month would send next month's reminders
		 * today and spend those steps; a preview may look ahead freely. */
		g_autoptr(GDateTime) horizon = g_date_time_add_days(sweep.now, 1);
		if (g_date_time_compare(sweep.as_of, horizon) > 0)
			return refuse(error, "as_of is more than a day in the future; a future sweep would send reminders that are not due yet (use dry_run to preview)") ? NULL : NULL;
	}
	sweep.noted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sweep.failed_invoices = json_array_new();
	sweep.warnings = json_array_new();
	sweep.plan = json_array_new();
	g_object_get(venture_database_get_mail_outbox(self->database), "mailer", &mailer, NULL);
	if (mailer == NULL)
		sweep_note(&sweep, FALSE, "No mail transport is configured: reminders wait in the outbox, and any queued more than %d days when delivery resumes are cancelled as stale",
			VENTURE_DUNNING_STALE_DAYS);
	if (!earliest_offset(self, &sweep, &any, &earliest, error))
		return NULL;
	if (any)
	{
		/* Only invoices due on or before (as_of - earliest offset) can have a
		 * step due; the rest of the ledger is never read. */
		g_autoptr(GDateTime) cutoff = day_after(sweep.as_of, 1 - earliest);
		due_before = venture_time_to_string(cutoff);
		sent = open_invoices(self, organization_id, VENTURE_INVOICE_STATUS_SENT, due_before, error);
		if (sent == NULL)
			return NULL;
		partial = open_invoices(self, organization_id, VENTURE_INVOICE_STATUS_PARTIALLY_PAID, due_before, error);
		if (partial == NULL)
			return NULL;
		sets[0] = sent;
		sets[1] = partial;
		for (i = 0; i < 2; i++)
		{
			guint j;
			for (j = 0; j < sets[i]->len && sweep.invoices < limit; j++)
			{
				gboolean touched = FALSE;
				sweep_invoice(self, &sweep, g_ptr_array_index(sets[i], j), actor, &touched);
				/* Quiet invoices (no step due) do not exhaust the budget; an
				 * opted-out or superseded write still counts as acting. */
				if (touched)
					sweep.invoices++;
			}
		}
	}
	as_of_text = venture_time_to_string(sweep.as_of);
	answer = json_object_new();
	json_object_set_int_member(answer, "organization_id", organization_id);
	json_object_set_string_member(answer, "as_of", as_of_text);
	json_object_set_boolean_member(answer, "dry_run", dry_run);
	json_object_set_int_member(answer, "invoices", sweep.invoices);
	json_object_set_int_member(answer, "queued", sweep.queued);
	json_object_set_int_member(answer, "escalated", sweep.escalated);
	json_object_set_int_member(answer, "suppressed", sweep.suppressed);
	json_object_set_int_member(answer, "failed", sweep.failed);
	json_object_set_array_member(answer, "failed_invoice_ids", json_array_ref(sweep.failed_invoices));
	json_object_set_array_member(answer, "warnings", json_array_ref(sweep.warnings));
	if (dry_run)
		json_object_set_array_member(answer, "plan", json_array_ref(sweep.plan));
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, answer);
	return node;
}

gint
venture_dunning_service_sweep(VentureDunningService *self, gint64 organization_id, GDateTime *as_of, guint limit,
	const VentureActor *actor, GError **error)
{
	g_autoptr(JsonNode) answer = venture_dunning_service_sweep_detailed(self, organization_id, as_of, limit, FALSE, actor, error);
	JsonObject *object;
	if (answer == NULL)
		return -1;
	object = json_node_get_object(answer);
	return (gint)(json_object_get_int_member(object, "queued") + json_object_get_int_member(object, "escalated"));
}

/* --- Retry and test sends ------------------------------------------------- */

static gboolean
retryable(VentureEntity *event)
{
	g_autofree gchar *status = text(event, "delivery-status");
	return g_strcmp0(status, STATUS_FAILED) == 0 || g_strcmp0(status, "dead") == 0 || g_strcmp0(status, "uncertain") == 0;
}

/* Whether any step of the event's policy was decided for its invoice after
 * it: a later offset, or a later attempt at the same one. */
static gboolean
later_event(VentureDunningService *self, VentureEntity *event, gboolean same_offset)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DUNNING_EVENT);
	g_autoptr(VentureEntity) found = NULL;
	venture_query_set_organization(query, venture_entity_get_organization_id(event));
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, number(event, "invoice-id"), NULL);
	venture_query_add_filter_int(query, "policy-id", VENTURE_FILTER_OP_EQ, number(event, "policy-id"), NULL);
	if (same_offset)
	{
		venture_query_add_filter_int(query, "offset-days", VENTURE_FILTER_OP_EQ, number(event, "offset-days"), NULL);
		venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_GT, venture_entity_get_id(event), NULL);
	}
	else
		venture_query_add_filter_int(query, "offset-days", VENTURE_FILTER_OP_GT, number(event, "offset-days"), NULL);
	found = venture_database_find_one(self->database, query, NULL);
	return found != NULL;
}

/* The original key with ":retry:N" appended, N one more than any attempt
 * already made, so every attempt is its own idempotent outbox message. */
static gchar *
retry_key(VentureDunningService *self, VentureEntity *event, GError **error)
{
	g_autofree gchar *key = text(event, "dunning-key");
	g_autofree gchar *prefix = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DUNNING_EVENT);
	g_autoptr(GPtrArray) rows = NULL;
	gchar *marker = key != NULL ? g_strrstr(key, ":retry:") : NULL;
	gint64 highest = 0;
	guint i;
	if (key == NULL)
		return refuse(error, "The reminder has no key") ? NULL : NULL;
	if (marker != NULL)
		*marker = '\0';
	prefix = g_strconcat(key, ":retry:", NULL);
	venture_query_set_organization(query, venture_entity_get_organization_id(event));
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, number(event, "invoice-id"), NULL);
	venture_query_add_filter_int(query, "offset-days", VENTURE_FILTER_OP_EQ, number(event, "offset-days"), NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *other = text(g_ptr_array_index(rows, i), "dunning-key");
		if (other != NULL && g_str_has_prefix(other, prefix))
			highest = MAX(highest, g_ascii_strtoll(other + strlen(prefix), NULL, 10));
	}
	return g_strdup_printf("%s%" G_GINT64_FORMAT, prefix, highest + 1);
}

VentureEntity *
venture_dunning_service_retry(VentureDunningService *self, VentureEntity *event, const VentureActor *actor, GError **error)
{
	g_auto(Dossier) d;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) policy = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *key = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *reason;
	gint64 org;
	gint index = -1;
	gboolean ok;
	Tally tally;
	guint i;
	memset(&d, 0, sizeof(d));
	memset(&tally, 0, sizeof(tally));
	g_return_val_if_fail(VENTURE_IS_DUNNING_SERVICE(self), NULL);
	if (!enabled(error))
		return NULL;
	if (!VENTURE_IS_DUNNING_EVENT(event) || venture_entity_get_id(event) <= 0)
		return refuse(error, "A recorded reminder is required") ? NULL : NULL;
	if (!retryable(event))
		return refuse(error, "Only a failed, dead or uncertain reminder can be retried") ? NULL : NULL;
	if (venture_database_has_transaction(self->database))
		return refuse(error, "A retry owns its transaction; call it outside one") ? NULL : NULL;
	org = venture_entity_get_organization_id(event);
	invoice = load(self, VENTURE_TYPE_INVOICE, number(event, "invoice-id"), org);
	if (invoice == NULL)
		return refuse(error, "The reminder's invoice is gone") ? NULL : NULL;
	policy = load(self, VENTURE_TYPE_DUNNING_POLICY, number(event, "policy-id"), org);
	if (policy == NULL)
		return refuse(error, "The reminder's policy is gone") ? NULL : NULL;
	if (!dossier_open(self, NULL, &d, invoice, now, FALSE, error))
		return NULL;
	d.policy = g_steal_pointer(&policy);
	json = text(d.policy, "steps");
	d.steps = parse_steps(json, flag(d.policy, "final-escalation"), error);
	if (d.steps == NULL)
		return NULL;
	for (i = 0; i < d.steps->len; i++)
		if (g_array_index(d.steps, Step, i).offset == number(event, "offset-days"))
			index = (gint)i;
	if (index < 0)
		return refuse(error, "The policy no longer has a step at this reminder's offset") ? NULL : NULL;
	if (later_event(self, event, FALSE) || later_event(self, event, TRUE))
		return refuse(error, "A later step, or another attempt at this one, is already recorded; retrying would send an older reminder") ? NULL : NULL;
	if (hold_reason(self, &d, now, NULL) != NULL)
		return refuse(error, "Reminders for this invoice are on hold (paused or with a collection case)") ? NULL : NULL;
	reason = stop_reason(self, &d);
	if (reason != NULL && !recorded_reason(reason))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureDunningService: the invoice has left dunning (%s)", reason);
		return NULL;
	}
	key = retry_key(self, event, error);
	if (key == NULL)
		return NULL;
	if (!venture_database_begin(self->database, error))
		return NULL;
	if (escalating(&d, (guint)index))
		ok = escalate(self, &d, now, now, (guint)index + 1, &g_array_index(d.steps, Step, index), key, actor, &tally, &local);
	else if (reason != NULL)
		ok = record_status(self, &d, now, (guint)index + 1, &g_array_index(d.steps, Step, index), key, STATUS_SUPPRESSED,
			reason, NULL, actor, &tally, &local);
	else
		ok = send_step(self, &d, now, (guint)index + 1, &g_array_index(d.steps, Step, index), key, actor, &tally, &local);
	if (!ok)
	{
		venture_database_rollback(self->database);
		record_failure(self, &d, now, (guint)index + 1, &g_array_index(d.steps, Step, index), key, local->message, actor);
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	if (!venture_database_commit(self->database, error))
		return NULL;
	query = venture_query_new(VENTURE_TYPE_DUNNING_EVENT);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "dunning-key", VENTURE_FILTER_OP_EQ, key, NULL);
	return venture_database_find_one(self->database, query, error);
}

static gchar *
actor_email(VentureDunningService *self, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *email = NULL;
	if (actor == NULL || venture_string_is_empty(actor->name))
		return refuse(error, "test_send mails the acting user's own address, and this request names no user") ? NULL : NULL;
	query = venture_query_new(VENTURE_TYPE_USER);
	venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, actor->name, NULL);
	user = venture_database_find_one(self->database, query, NULL);
	if (user == NULL || venture_entity_is_deleted(user))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureDunningService: test_send mails the acting user's own address, and \"%s\" is not a user account", actor->name);
		return NULL;
	}
	email = text(user, "email");
	if (venture_string_is_empty(email))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureDunningService: test_send mails only the acting user, and %s has no email address", actor->name);
		return NULL;
	}
	return g_steal_pointer(&email);
}

VentureMailMessage *
venture_dunning_service_test_send(VentureDunningService *self, VentureEntity *policy, gint64 invoice_id,
	gboolean has_offset, gint64 offset, const VentureActor *actor, GError **error)
{
	g_auto(Dossier) d;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) template = NULL;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *email = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *recipient = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *subject = NULL;
	g_autofree gchar *test_subject = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *key = NULL;
	gint64 org;
	gint index = -1;
	guint i;
	memset(&d, 0, sizeof(d));
	g_return_val_if_fail(VENTURE_IS_DUNNING_SERVICE(self), NULL);
	if (!enabled(error))
		return NULL;
	if (!VENTURE_IS_DUNNING_POLICY(policy) || venture_entity_get_id(policy) <= 0)
		return refuse(error, "A saved dunning_policy is required") ? NULL : NULL;
	email = actor_email(self, actor, error);
	if (email == NULL)
		return NULL;
	org = venture_entity_get_organization_id(policy);
	invoice = load(self, VENTURE_TYPE_INVOICE, invoice_id, org);
	if (invoice == NULL)
		return refuse(error, "invoice_id must name a live invoice of the policy's organization") ? NULL : NULL;
	if (!dossier_open(self, NULL, &d, invoice, now, FALSE, error))
		return NULL;
	d.policy = g_object_ref(policy);
	json = text(policy, "steps");
	d.steps = parse_steps(json, flag(policy, "final-escalation"), error);
	if (d.steps == NULL)
		return NULL;
	if (has_offset)
	{
		for (i = 0; i < d.steps->len; i++)
			if (g_array_index(d.steps, Step, i).offset == offset)
				index = (gint)i;
		if (index < 0)
			return refuse(error, "The policy has no step at that offset") ? NULL : NULL;
	}
	else
	{
		/* The step this invoice would get next, or the first email step. */
		if (d.due != NULL && load_history(self, &d, NULL))
			index = next_step_index(&d);
		if (index >= 0 && (escalating(&d, (guint)index) || g_array_index(d.steps, Step, index).template_id <= 0))
			index = -1;
		for (i = 0; index < 0 && i < d.steps->len; i++)
			if (!escalating(&d, i) && g_array_index(d.steps, Step, i).template_id > 0)
				index = (gint)i;
		if (index < 0)
			return refuse(error, "The policy has no email step to test") ? NULL : NULL;
	}
	if (escalating(&d, (guint)index) || g_array_index(d.steps, Step, index).template_id <= 0)
		return refuse(error, "That step escalates to an owner and sends no email") ? NULL : NULL;
	template = load(self, venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_template"),
		g_array_index(d.steps, Step, index).template_id, org);
	if (template == NULL)
		return refuse(error, "The step's mail_template is missing") ? NULL : NULL;
	recipient = recipient_for(&d);
	link = live_portal_link(self, org, d.company, recipient);
	message = render_message(self, &d, template, &g_array_index(d.steps, Step, index), (guint)index + 1,
		link != NULL ? VENTURE_DUNNING_TEST_LINK : NULL, FALSE, error);
	if (message == NULL)
		return NULL;
	g_object_get(message, "subject", &subject, NULL);
	test_subject = g_strdup_printf("[TEST] %s", subject ? subject : "");
	uuid = g_uuid_string_random();
	key = g_strdup_printf("dunning-test:%s", uuid);
	/* No dunning_event: a test send decides nothing about the invoice, so
	 * it must not count as a step, and the outbox's recheck ignores it. */
	g_object_set(message, "organization-id", org, "to", email, "subject", test_subject, "idempotency-key", key,
		"related-type", "dunning_policy", "related-id", venture_entity_get_id(policy), NULL);
	return venture_mail_outbox_enqueue(venture_database_get_mail_outbox(self->database), message, actor, error);
}

/* --- Outbox hooks --------------------------------------------------------- */

static VentureEntity *
event_for_message(VentureDunningService *self, VentureEntity *message)
{
	g_autofree gchar *related = text(message, "related-type");
	g_autoptr(VentureEntity) event = NULL;
	gint64 id = venture_entity_get_id(message);
	if (g_strcmp0(related, "dunning_event") != 0)
		return NULL;
	event = load(self, VENTURE_TYPE_DUNNING_EVENT, number(message, "related-id"), venture_entity_get_organization_id(message));
	/* The related fields are ordinary columns anybody who may write a
	 * message can fill in. Only the message the sweep recorded on the
	 * event speaks for it; otherwise an editor's own message to their own
	 * address could mark a reminder failed and make a retry legal. */
	if (event == NULL || id <= 0 || number(event, "mail-message-id") != id)
		return NULL;
	return g_steal_pointer(&event);
}

/* The outbox claims with an explicit clock and leases from it; recovering
 * that clock keeps a delivery run as of a given day judged by that day. */
static GDateTime *
delivery_clock(VentureEntity *message)
{
	g_autoptr(GDateTime) lease = when(message, "lease-until");
	if (lease == NULL)
		return venture_time_now();
	return g_date_time_add_seconds(lease, -(gdouble)VENTURE_MAIL_LEASE_SECONDS);
}

/* Why a queued reminder must not go out now, or NULL. */
static const gchar *
cancel_reason(VentureDunningService *self, VentureEntity *event, GDateTime *clock)
{
	g_auto(Dossier) d;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureMoney) rendered = NULL;
	g_autoptr(GDateTime) queued_at = when(event, "queued-at");
	const gchar *reason;
	gint64 org = venture_entity_get_organization_id(event);
	memset(&d, 0, sizeof(d));
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "dunning_event") == G_TYPE_INVALID)
		return "module_disabled";
	invoice = load(self, VENTURE_TYPE_INVOICE, number(event, "invoice-id"), org);
	if (invoice == NULL)
		return "invoice_gone";
	if (!dossier_open(self, NULL, &d, invoice, clock, FALSE, NULL))
		return "invoice_gone";
	reason = stop_reason(self, &d);
	if (reason != NULL)
		return reason;
	reason = hold_reason(self, &d, clock, NULL);
	if (reason != NULL)
		return reason;
	/* A later step queued behind this one means the outbox was down long
	 * enough for the cadence to move on; only the newest may go. */
	if (later_event(self, event, FALSE))
		return "superseded";
	if (d.due == NULL || number(event, "offset-days") > d.days)
		return "no_longer_due";
	g_object_get(event, "rendered-balance", &rendered, NULL);
	if (rendered != NULL && !venture_money_equal(dossier_balance(self, &d), rendered))
		return "balance_changed";
	if (queued_at != NULL && g_date_time_difference(clock, queued_at) > VENTURE_DUNNING_STALE_DAYS * G_TIME_SPAN_DAY)
		return "stale";
	return NULL;
}

/* The eligibility recheck between claim and transport: a reminder queued
 * before the invoice was paid, disputed, paused or the customer opted out,
 * one overtaken by a later step, one whose balance or due date moved, one
 * that waited too long, and one whose module was switched off, is cancelled
 * here, and the event says why. */
static gboolean
before_send(VentureMailOutbox *outbox, VentureMailMessage *message, GError **error, gpointer data)
{
	VentureDunningService *self = data;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(GDateTime) clock = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *reason;
	(void)outbox;
	if (self->database == NULL)
		return FALSE;
	event = event_for_message(self, VENTURE_ENTITY(message));
	if (event == NULL)
		return FALSE;
	clock = delivery_clock(VENTURE_ENTITY(message));
	reason = cancel_reason(self, event, clock);
	if (reason == NULL)
		return FALSE;
	g_object_set(event, "delivery-status", STATUS_CANCELLED, "suppressed-reason", reason, NULL);
	if (!save_event(self, event, NULL, &local))
		g_warning("dunning: reminder event #%" G_GINT64_FORMAT " was cancelled (%s) but could not be updated: %s",
			venture_entity_get_id(event), reason, local->message);
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Reminder cancelled: %s", reason);
	return TRUE;
}

/* The outbox owns delivery state; the event mirrors it so the invoice page,
 * the timeline and the report read one column. */
static void
entity_saved(VentureDatabase *database, VentureEntity *entity, gboolean created, gpointer data)
{
	VentureDunningService *self = data;
	g_autoptr(VentureEntity) event = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *current = NULL;
	g_autofree gchar *last_error = NULL;
	g_autoptr(GDateTime) sent_at = NULL;
	g_autoptr(GError) local = NULL;
	(void)database;
	(void)created;
	if (!VENTURE_IS_MAIL_MESSAGE(entity))
		return;
	event = event_for_message(self, entity);
	if (event == NULL)
		return;
	g_object_get(entity, "state", &state, "sent-at", &sent_at, "last-error", &last_error, NULL);
	current = text(event, "delivery-status");
	if (venture_string_is_empty(state) || g_strcmp0(state, "sending") == 0 || g_strcmp0(state, current) == 0)
		return;
	if (g_strcmp0(current, STATUS_CANCELLED) == 0 && g_strcmp0(state, STATUS_CANCELLED) == 0)
		return;
	g_object_set(event, "delivery-status", state, "sent-at", sent_at, "last-error", last_error, NULL);
	if (!save_event(self, event, NULL, &local))
		g_warning("dunning: reminder event #%" G_GINT64_FORMAT " could not mirror delivery state %s: %s",
			venture_entity_get_id(event), state, local->message);
}

/* --- Actions -------------------------------------------------------------- */

static gint64
param_id(GHashTable *params, const gchar *name)
{
	JsonNode *node = params ? g_hash_table_lookup(params, name) : NULL;
	const gchar *value;
	if (node == NULL || JSON_NODE_HOLDS_NULL(node) || !JSON_NODE_HOLDS_VALUE(node))
		return 0;
	if (json_node_get_value_type(node) != G_TYPE_STRING)
		return json_node_get_int(node);
	value = json_node_get_string(node);
	return value ? g_ascii_strtoll(value, NULL, 10) : 0;
}

static gboolean
param_present(GHashTable *params, const gchar *name)
{
	JsonNode *node = params ? g_hash_table_lookup(params, name) : NULL;
	return node != NULL && !JSON_NODE_HOLDS_NULL(node);
}

static gboolean
param_bool(GHashTable *params, const gchar *name)
{
	JsonNode *node = params ? g_hash_table_lookup(params, name) : NULL;
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return FALSE;
	if (json_node_get_value_type(node) == G_TYPE_BOOLEAN)
		return json_node_get_boolean(node);
	if (json_node_get_value_type(node) == G_TYPE_STRING)
		return g_strcmp0(json_node_get_string(node), "true") == 0;
	return FALSE;
}

static const gchar *
param_string(GHashTable *params, const gchar *name)
{
	JsonNode *node = params ? g_hash_table_lookup(params, name) : NULL;
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}

static gboolean
sweep_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	return enabled(error);
}

static VentureEntity *
sweep_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureDunningService *self = venture_action_get_data(action);
	const gchar *as_of_text = param_string(params, "as_of");
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *serialized = NULL;
	VentureEntity *result;
	/* The placeholder carries the organization the access policy judged
	 * (venture_action_prepare_target() set it from organization_id), so
	 * that is the one swept; a caller never reaches a different one. */
	gint64 org = entity != NULL ? venture_entity_get_organization_id(entity) : 0;
	gint64 limit = param_id(params, "limit");
	if (!venture_string_is_empty(as_of_text))
	{
		as_of = venture_time_from_string(as_of_text, error);
		if (as_of == NULL)
			return NULL;
	}
	if (org <= 0)
		org = param_id(params, "organization_id");
	if (org <= 0)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureEntity) organization = NULL;
		venture_query_add_filter_string(query, "is-default", VENTURE_FILTER_OP_EQ, "true", NULL);
		organization = venture_database_find_one(self->database, query, NULL);
		if (organization == NULL)
		{
			g_clear_object(&query);
			query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
			organization = venture_database_find_one(self->database, query, NULL);
		}
		org = organization != NULL ? venture_entity_get_id(organization) : 1;
	}
	answer = venture_dunning_service_sweep_detailed(self, org, as_of, (guint)CLAMP(limit, 0, 1000),
		param_bool(params, "dry_run"), actor, error);
	if (answer == NULL)
		return NULL;
	/* The answer rides on an unsaved policy, which is what a type-level
	 * action returns; the policy validator refuses to store it. */
	serialized = venture_json_to_string(answer, FALSE);
	result = g_object_new(VENTURE_TYPE_DUNNING_POLICY, NULL);
	venture_entity_set_organization_id(result, org);
	g_object_set(result, "last-sweep", serialized, NULL);
	return result;
}

static gboolean
retry_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)actor;
	if (!enabled(error))
		return FALSE;
	if (!VENTURE_IS_DUNNING_EVENT(entity) || venture_entity_get_id(entity) <= 0 || !retryable(entity))
		return refuse(error, "Only a failed, dead or uncertain reminder can be retried");
	return TRUE;
}

static VentureEntity *
retry_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	(void)params;
	return venture_dunning_service_retry(venture_action_get_data(action), entity, actor, error);
}

static gboolean
test_send_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)actor;
	if (!enabled(error))
		return FALSE;
	if (!VENTURE_IS_DUNNING_POLICY(entity) || venture_entity_get_id(entity) <= 0)
		return refuse(error, "A saved dunning_policy is required");
	return TRUE;
}

static VentureEntity *
test_send_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureMailMessage *message = venture_dunning_service_test_send(venture_action_get_data(action), entity,
		param_id(params, "invoice_id"), param_present(params, "offset"), param_id(params, "offset"), actor, error);
	return message != NULL ? VENTURE_ENTITY(message) : NULL;
}

static void
register_action(VentureActionRegistry *registry, const gchar *type_name, const gchar *name, const gchar *label,
	const gchar *description, GPtrArray *parameters, gboolean stageable, gboolean type_level,
	VentureActionAllowed allowed, VentureActionInvoke invoke, VentureDunningService *self)
{
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", type_name, "name", name,
		"label", label, "description", description, "parameters", parameters, "stageable", stageable,
		"type-level", type_level, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(registry, action, allowed, invoke, self, NULL, &error))
		g_error("Dunning action registration: %s", error->message);
}

void
venture_dunning_actions_register(VentureDatabase *database)
{
	VentureDunningService *self = venture_dunning_service_get(database);
	VentureActionRegistry *registry;
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	VentureFieldSpec *invoice_spec;
	if (self->actions)
		return;
	self->actions = TRUE;
	registry = venture_database_get_action_registry(database);
	g_ptr_array_add(parameters, venture_field_spec_new("as_of", "As of", VENTURE_FIELD_KIND_DATETIME));
	g_ptr_array_add(parameters, venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER));
	g_ptr_array_add(parameters, venture_field_spec_new("limit", "Limit", VENTURE_FIELD_KIND_INTEGER));
	g_ptr_array_add(parameters, venture_field_spec_new("dry_run", "Dry run", VENTURE_FIELD_KIND_BOOLEAN));
	register_action(registry, "dunning_policy", "sweep", "Send due reminders",
		"Enqueue every reminder step that fell due, once; dry_run returns the plan without writing",
		parameters, TRUE, TRUE, sweep_allowed, sweep_invoke, self);
	g_ptr_array_set_size(parameters, 0);
	register_action(registry, "dunning_event", "retry", "Retry reminder",
		"Run a failed, dead or uncertain reminder step again under a new key",
		parameters, TRUE, FALSE, retry_allowed, retry_invoke, self);
	invoice_spec = venture_field_spec_new("invoice_id", "Invoice", VENTURE_FIELD_KIND_INTEGER);
	invoice_spec->required = TRUE;
	g_ptr_array_add(parameters, invoice_spec);
	g_ptr_array_add(parameters, venture_field_spec_new("offset", "Step offset", VENTURE_FIELD_KIND_INTEGER));
	/* Not stageable: it mails nobody but the person asking, and an approval
	 * would send it to the approver's colleague's inbox a day later. */
	register_action(registry, "dunning_policy", "test_send", "Send test reminder",
		"Render one step for an invoice and mail it to your own address; records nothing",
		parameters, FALSE, FALSE, test_send_allowed, test_send_invoke, self);
}

/* --- Timeline and portal -------------------------------------------------- */

static gchar *
describe_event(VentureEntity *event)
{
	g_autofree gchar *status = text(event, "delivery-status");
	g_autofree gchar *reason = text(event, "suppressed-reason");
	g_autoptr(GDateTime) sent_at = when(event, "sent-at");
	g_autoptr(GDateTime) queued_at = when(event, "queued-at");
	g_autofree gchar *day = venture_time_to_date_string(sent_at ? sent_at : queued_at, NULL);
	gint64 step = number(event, "step");
	if (g_strcmp0(status, STATUS_SENT) == 0)
		return g_strdup_printf("reminder sent %s (step %" G_GINT64_FORMAT ")", day ? day : "", step);
	if (g_strcmp0(status, STATUS_ESCALATED) == 0)
		return g_strdup_printf("collection escalated to the owner %s", day ? day : "");
	if (g_strcmp0(status, STATUS_SUPPRESSED) == 0 || g_strcmp0(status, STATUS_CANCELLED) == 0)
		return g_strdup_printf("reminder step %" G_GINT64_FORMAT " %s: %s", step, status, reason ? reason : "");
	return g_strdup_printf("reminder step %" G_GINT64_FORMAT " %s %s", step, status ? status : "", day ? day : "");
}

static GPtrArray *
events_for(VentureDatabase *database, const gchar *field, gint64 id, guint limit)
{
	g_autoptr(VentureQuery) query = NULL;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "dunning_event") == G_TYPE_INVALID)
		return NULL;
	query = venture_query_new(VENTURE_TYPE_DUNNING_EVENT);
	venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, limit);
	return venture_database_find(database, query, NULL);
}

/* What happens next, as a timeline line nobody has to compute by hand from
 * the policy: computed from the policy and the recorded events, never
 * written. */
static void
append_next_reminder(VentureContext *context, gint64 invoice_id, VentureDunningTimelineAdd add, GPtrArray *events)
{
	VentureDatabase *database = venture_context_get_database(context);
	VentureDunningService *self = venture_dunning_service_get(database);
	g_autoptr(VentureEntity) invoice = venture_database_get(database, VENTURE_TYPE_INVOICE, invoice_id, NULL);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) until = NULL;
	g_autoptr(GDateTime) at = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *iso = NULL;
	g_auto(Dossier) d;
	const gchar *hold;
	const gchar *reason;
	gint status = 0;
	gint index;
	memset(&d, 0, sizeof(d));
	if (invoice == NULL || venture_entity_is_deleted(invoice))
		return;
	g_object_get(invoice, "status", &status, NULL);
	if (status != VENTURE_INVOICE_STATUS_SENT && status != VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
		return;
	if (!dossier_open(self, NULL, &d, invoice, now, TRUE, NULL) || d.policy == NULL || d.due == NULL ||
		d.steps == NULL || d.steps->len == 0)
		return;
	reason = stop_reason(self, &d);
	if (reason != NULL)
		return;
	hold = hold_reason(self, &d, now, &until);
	if (hold != NULL)
	{
		g_autofree gchar *day = until != NULL ? venture_time_to_date_string(until, NULL) : NULL;
		body = day != NULL ? g_strdup_printf("reminders paused until %s", day) :
			g_strdup("reminders held by a collection case");
		at = g_date_time_ref(until != NULL ? until : now);
	}
	else
	{
		g_autofree gchar *day = NULL;
		if (!load_history(self, &d, NULL))
			return;
		index = next_step_index(&d);
		if (index < 0)
			return;
		at = day_after(d.due, eligible_day(&d, (guint)index));
		day = venture_time_to_date_string(at, NULL);
		body = escalating(&d, (guint)index) ?
			g_strdup_printf("next: collection escalation on %s (step %d)", day, index + 1) :
			g_strdup_printf("next reminder: step %d on %s", index + 1, day);
	}
	iso = g_date_time_format_iso8601(at);
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "kind");
	json_builder_add_string_value(builder, "reminder");
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, 0);
	json_builder_set_member_name(builder, "invoice_id");
	json_builder_add_int_value(builder, invoice_id);
	json_builder_set_member_name(builder, "actor");
	json_builder_add_string_value(builder, "dunning");
	json_builder_set_member_name(builder, "body");
	json_builder_add_string_value(builder, body);
	json_builder_set_member_name(builder, "delivery_status");
	json_builder_add_string_value(builder, "scheduled");
	json_builder_set_member_name(builder, "when");
	json_builder_add_string_value(builder, iso);
	json_builder_end_object(builder);
	add(events, at, json_builder_get_root(builder));
}

void
venture_dunning_append_timeline(VentureContext *context, const gchar *target_type, gint64 target_id,
	guint limit, VentureDunningTimelineAdd add, GPtrArray *events)
{
	const gchar *field;
	g_autoptr(GPtrArray) rows = NULL;
	gboolean invoice = g_strcmp0(target_type, "invoice") == 0;
	guint i;
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_return_if_fail(add != NULL && events != NULL);
	if (invoice)
		field = "invoice-id";
	else if (g_strcmp0(target_type, "contact") == 0)
		field = "contact-id";
	else if (g_strcmp0(target_type, "company") == 0)
		field = "company-id";
	else
		return;
	if (!venture_context_module_enabled(context, "dunning"))
		return;
	rows = events_for(venture_context_get_database(context), field, target_id, limit ? limit : 100);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(rows, i);
		g_autoptr(JsonBuilder) builder = json_builder_new();
		g_autofree gchar *body = describe_event(event);
		g_autofree gchar *status = text(event, "delivery-status");
		g_autoptr(GDateTime) sent_at = when(event, "sent-at");
		g_autoptr(GDateTime) queued_at = when(event, "queued-at");
		GDateTime *at = sent_at ? sent_at : (queued_at ? queued_at : venture_entity_get_created_at(event));
		g_autofree gchar *iso = at ? g_date_time_format_iso8601(at) : NULL;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder, "reminder");
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, venture_entity_get_id(event));
		json_builder_set_member_name(builder, "invoice_id");
		json_builder_add_int_value(builder, number(event, "invoice-id"));
		json_builder_set_member_name(builder, "actor");
		json_builder_add_string_value(builder, "dunning");
		json_builder_set_member_name(builder, "body");
		json_builder_add_string_value(builder, body);
		json_builder_set_member_name(builder, "delivery_status");
		json_builder_add_string_value(builder, status ? status : "");
		if (iso != NULL)
		{
			json_builder_set_member_name(builder, "when");
			json_builder_add_string_value(builder, iso);
		}
		json_builder_end_object(builder);
		add(events, at, json_builder_get_root(builder));
	}
	if (invoice)
		append_next_reminder(context, target_id, add, events);
}

gchar *
venture_dunning_portal_summary(VentureDatabase *database, VentureEntity *invoice)
{
	GString *out = g_string_new(NULL);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), g_string_free(out, FALSE));
	g_return_val_if_fail(VENTURE_IS_ENTITY(invoice), g_string_free(out, FALSE));
	rows = events_for(database, "invoice-id", venture_entity_get_id(invoice), 0);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(rows, i);
		g_autofree gchar *status = text(event, "delivery-status");
		g_autoptr(GDateTime) sent_at = when(event, "sent-at");
		g_autofree gchar *day = NULL;
		/* A customer sees what reached them, not what was suppressed or why. */
		if (g_strcmp0(status, STATUS_SENT) != 0 || sent_at == NULL)
			continue;
		day = venture_time_to_date_string(sent_at, NULL);
		g_string_append_printf(out, "%sreminder sent %s", out->len ? "; " : "", day);
	}
	return g_string_free(out, FALSE);
}

/* --- Reports -------------------------------------------------------------- */

static gint64
report_org(VentureContext *context, JsonObject *options)
{
	return options != NULL && json_object_has_member(options, "organization_id") ?
		json_object_get_int_member(options, "organization_id") : venture_context_get_default_organization_id(context);
}

static GPtrArray *
all_rows(VentureDatabase *database, GType type, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	return venture_database_find(database, query, error);
}

/* Borrowed rows by id, so each event finds its invoice without a scan. */
static GHashTable *
index_rows(GPtrArray *rows)
{
	GHashTable *index = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	guint i;
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		gint64 *key = g_new(gint64, 1);
		*key = venture_entity_get_id(g_ptr_array_index(rows, i));
		g_hash_table_insert(index, key, g_ptr_array_index(rows, i));
	}
	return index;
}

static VentureEntity *
lookup_row(GHashTable *index, gint64 id)
{
	return g_hash_table_lookup(index, &id);
}

/* Days from issue to payment: the number a reminder policy exists to shrink. */
static gboolean
days_to_pay(VentureEntity *invoice, gdouble *days)
{
	g_autoptr(GDateTime) issued = when(invoice, "issued-at");
	g_autoptr(GDateTime) paid = when(invoice, "paid-at");
	gint status = 0;
	g_object_get(invoice, "status", &status, NULL);
	if (status != VENTURE_INVOICE_STATUS_PAID || issued == NULL || paid == NULL)
		return FALSE;
	*days = (gdouble)g_date_time_difference(paid, issued) / (gdouble)G_TIME_SPAN_DAY;
	return TRUE;
}

static VentureReportResult *
collections(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	gint64 org = report_org(context, options);
	g_autoptr(GPtrArray) policies = all_rows(database, VENTURE_TYPE_DUNNING_POLICY, org, error);
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) companies = NULL;
	g_autoptr(GHashTable) invoice_index = NULL;
	g_autoptr(GHashTable) company_index = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Collections", period);
	guint p;
	if (policies == NULL)
		return NULL;
	events = all_rows(database, VENTURE_TYPE_DUNNING_EVENT, org, error);
	if (events == NULL)
		return NULL;
	invoices = all_rows(database, VENTURE_TYPE_INVOICE, org, error);
	if (invoices == NULL)
		return NULL;
	companies = all_rows(database, VENTURE_TYPE_COMPANY, org, error);
	if (companies == NULL)
		return NULL;
	invoice_index = index_rows(invoices);
	company_index = index_rows(companies);
	venture_report_result_add_column(result, "policy", "Policy", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "step", "Step", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "offset_days", "Offset (days)", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "reminders_sent", "Reminders sent", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "queued", "Queued", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "failed", "Failed", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "escalations", "Escalations", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "suppressed", "Suppressed", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "paid_within_7_days", "Paid within 7 days", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "average_days_before", "Avg days to pay before adoption", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "average_days_after", "Avg days to pay after adoption", VENTURE_REPORT_COLUMN_NUMBER);
	for (p = 0; p < policies->len; p++)
	{
		VentureEntity *policy = g_ptr_array_index(policies, p);
		g_autofree gchar *name = text(policy, "name");
		g_autofree gchar *json = text(policy, "steps");
		g_autoptr(GArray) steps = parse_steps(json, flag(policy, "final-escalation"), NULL);
		g_autoptr(GDateTime) adopted = when(policy, "adopted-at");
		gdouble before_sum = 0, after_sum = 0;
		guint before_n = 0, after_n = 0;
		guint i, s;
		if (adopted == NULL && venture_entity_get_created_at(policy) != NULL)
			adopted = g_date_time_ref(venture_entity_get_created_at(policy));
		/* Before/after is by issue date against adoption, over invoices this
		 * policy governed or would have governed, resolved the way the sweep
		 * resolves: the invoice's own policy, then its customer's, then the
		 * default. */
		for (i = 0; i < invoices->len; i++)
		{
			VentureEntity *invoice = g_ptr_array_index(invoices, i);
			g_autoptr(GDateTime) issued = when(invoice, "issued-at");
			gint64 named = number(invoice, "dunning-policy-id");
			gdouble days;
			if (named <= 0)
			{
				VentureEntity *company = lookup_row(company_index, number(invoice, "company-id"));
				named = company != NULL ? number(company, "dunning-policy-id") : 0;
			}
			if (named > 0 ? named != venture_entity_get_id(policy) : !flag(policy, "is-default"))
				continue;
			if (!days_to_pay(invoice, &days) || issued == NULL)
				continue;
			if (adopted != NULL && g_date_time_compare(issued, adopted) < 0)
			{
				before_sum += days;
				before_n++;
			}
			else
			{
				after_sum += days;
				after_n++;
			}
		}
		for (s = 0; steps != NULL && s < steps->len; s++)
		{
			guint sent = 0, queued = 0, failed = 0, escalations = 0, suppressed = 0, paid_soon = 0;
			for (i = 0; i < events->len; i++)
			{
				VentureEntity *event = g_ptr_array_index(events, i);
				g_autofree gchar *status = NULL;
				g_autoptr(GDateTime) at = NULL;
				g_autoptr(GDateTime) queued_at = NULL;
				VentureEntity *invoice;
				g_autoptr(GDateTime) paid = NULL;
				/* A step is its offset: positions move when a policy is edited. */
				if (number(event, "policy-id") != venture_entity_get_id(policy) ||
					number(event, "offset-days") != g_array_index(steps, Step, s).offset)
					continue;
				queued_at = when(event, "queued-at");
				if (period != NULL && (queued_at == NULL || !venture_date_range_contains(period, queued_at)))
					continue;
				status = text(event, "delivery-status");
				if (g_strcmp0(status, STATUS_ESCALATED) == 0)
				{
					escalations++;
					continue;
				}
				if (g_strcmp0(status, STATUS_QUEUED) == 0)
				{
					queued++;
					continue;
				}
				if (g_strcmp0(status, STATUS_FAILED) == 0 || g_strcmp0(status, "dead") == 0 || g_strcmp0(status, "uncertain") == 0)
				{
					failed++;
					continue;
				}
				if (g_strcmp0(status, STATUS_SENT) != 0)
				{
					suppressed += g_strcmp0(status, STATUS_SUPPRESSED) == 0 || g_strcmp0(status, STATUS_CANCELLED) == 0;
					continue;
				}
				sent++;
				at = when(event, "sent-at");
				if (at == NULL)
					at = queued_at ? g_date_time_ref(queued_at) : NULL;
				invoice = lookup_row(invoice_index, number(event, "invoice-id"));
				if (invoice == NULL || at == NULL)
					continue;
				paid = when(invoice, "paid-at");
				if (paid != NULL && g_date_time_compare(paid, at) >= 0 &&
					g_date_time_difference(paid, at) <= 7 * G_TIME_SPAN_DAY)
					paid_soon++;
			}
			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "policy", name);
			venture_report_result_set_number(result, "step", s + 1);
			venture_report_result_set_number(result, "offset_days", (gdouble)g_array_index(steps, Step, s).offset);
			venture_report_result_set_number(result, "reminders_sent", sent);
			venture_report_result_set_number(result, "queued", queued);
			venture_report_result_set_number(result, "failed", failed);
			venture_report_result_set_number(result, "escalations", escalations);
			venture_report_result_set_number(result, "suppressed", suppressed);
			venture_report_result_set_number(result, "paid_within_7_days", paid_soon);
			venture_report_result_set_number(result, "average_days_before", before_n ? before_sum / before_n : 0);
			venture_report_result_set_number(result, "average_days_after", after_n ? after_sum / after_n : 0);
		}
	}
	return g_steal_pointer(&result);
}

typedef struct
{
	gint days;
	gchar *number;
	gchar *customer;
	VentureMoney *balance;
	gchar *last_step;
	gchar *last_status;
	gchar *last_date;
	gchar *next;
	gchar *next_date;
	gchar *owner;
} WorkRow;

static void
work_row_free(WorkRow *row)
{
	g_free(row->number);
	g_free(row->customer);
	g_clear_pointer(&row->balance, venture_money_free);
	g_free(row->last_step);
	g_free(row->last_status);
	g_free(row->last_date);
	g_free(row->next);
	g_free(row->next_date);
	g_free(row->owner);
	g_free(row);
}

/* Most overdue first: the worklist is read from the top. */
static gint
work_row_compare(gconstpointer a, gconstpointer b)
{
	const WorkRow *left = *(const WorkRow *const *)a;
	const WorkRow *right = *(const WorkRow *const *)b;
	return right->days - left->days;
}

static const gchar *
aging_bucket(gint days)
{
	if (days <= 0)
		return "current";
	if (days <= 30)
		return "1-30";
	if (days <= 60)
		return "31-60";
	if (days <= 90)
		return "61-90";
	return "90+";
}

/* One invoice's line: where it stands and what happens to it next. */
static WorkRow *
work_row(VentureDunningService *self, VentureEntity *invoice, GDateTime *now)
{
	g_auto(Dossier) d;
	g_autoptr(GDateTime) until = NULL;
	WorkRow *row;
	const VentureMoney *balance;
	const gchar *hold;
	const gchar *reason;
	memset(&d, 0, sizeof(d));
	if (!dossier_open(self, NULL, &d, invoice, now, TRUE, NULL) || d.due == NULL)
		return NULL;
	balance = dossier_balance(self, &d);
	if (balance == NULL || venture_money_is_zero(balance) || venture_money_is_negative(balance))
		return NULL;
	if (d.policy != NULL && !load_history(self, &d, NULL))
		return NULL;
	if (d.days < 1 && !d.recorded)
		return NULL;
	row = g_new0(WorkRow, 1);
	row->days = d.days;
	row->number = text(invoice, "number");
	row->customer = d.company != NULL ? text(d.company, "name") : g_strdup("");
	row->balance = venture_money_copy(balance);
	row->owner = escalation_owner(self, &d);
	if (d.last_event != NULL)
	{
		g_autoptr(GDateTime) sent_at = when(d.last_event, "sent-at");
		g_autoptr(GDateTime) queued_at = when(d.last_event, "queued-at");
		g_autofree gchar *status = text(d.last_event, "delivery-status");
		g_autofree gchar *why = text(d.last_event, "suppressed-reason");
		row->last_step = g_strdup_printf("step %" G_GINT64_FORMAT " (offset %" G_GINT64_FORMAT ")",
			number(d.last_event, "step"), number(d.last_event, "offset-days"));
		row->last_status = venture_string_is_empty(why) ? g_strdup(status ? status : "") : g_strdup_printf("%s: %s", status ? status : "", why);
		row->last_date = venture_time_to_date_string(sent_at ? sent_at : queued_at, NULL);
	}
	hold = hold_reason(self, &d, now, &until);
	reason = stop_reason(self, &d);
	if (d.policy == NULL)
		row->next = g_strdup("no reminders");
	else if (d.steps == NULL)
		row->next = g_strdup("policy steps invalid");
	else if (hold != NULL)
	{
		g_autofree gchar *day = until != NULL ? venture_time_to_date_string(until, NULL) : NULL;
		row->next = day != NULL ? g_strdup_printf("paused until %s", day) : g_strdup("held by a collection case");
	}
	else if (reason != NULL && g_strcmp0(reason, "company_opt_out") == 0 && d.escalated)
		row->next = g_strdup("escalated");
	else if (reason != NULL && g_strcmp0(reason, "disputed") == 0)
		row->next = g_strdup("disputed");
	else
	{
		gint index = next_step_index(&d);
		if (index >= 0)
		{
			g_autoptr(GDateTime) at = day_after(d.due, eligible_day(&d, (guint)index));
			row->next = escalating(&d, (guint)index) ?
				g_strdup_printf("escalation (step %d, offset %" G_GINT64_FORMAT ")", index + 1, g_array_index(d.steps, Step, index).offset) :
				g_strdup_printf("step %d (offset %" G_GINT64_FORMAT ")%s", index + 1, g_array_index(d.steps, Step, index).offset,
					reason != NULL ? ", customer opted out" : "");
			row->next_date = venture_time_to_date_string(at, NULL);
		}
		else
			row->next = g_strdup(d.escalated ? "escalated" : "exhausted");
	}
	return row;
}

static VentureReportResult *
worklist(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	VentureDunningService *self = venture_dunning_service_get(database);
	gint64 org = report_org(context, options);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Dunning worklist", period);
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func((GDestroyNotify)work_row_free);
	g_autoptr(GDateTime) now = venture_time_now();
	const gint statuses[] = { VENTURE_INVOICE_STATUS_SENT, VENTURE_INVOICE_STATUS_PARTIALLY_PAID };
	guint i, j;
	for (i = 0; i < G_N_ELEMENTS(statuses); i++)
	{
		g_autoptr(GPtrArray) invoices = open_invoices(self, org, statuses[i], NULL, error);
		if (invoices == NULL)
			return NULL;
		for (j = 0; j < invoices->len; j++)
		{
			WorkRow *row = work_row(self, g_ptr_array_index(invoices, j), now);
			if (row != NULL)
				g_ptr_array_add(rows, row);
		}
	}
	g_ptr_array_sort(rows, work_row_compare);
	venture_report_result_add_column(result, "invoice", "Invoice", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "customer", "Customer", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "days_overdue", "Days overdue", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "aging", "Aging", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "open_balance", "Open balance", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "last_step", "Last step", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "last_status", "Last status", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "last_date", "Last step date", VENTURE_REPORT_COLUMN_DATE);
	venture_report_result_add_column(result, "next", "Next", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "next_date", "Next date", VENTURE_REPORT_COLUMN_DATE);
	venture_report_result_add_column(result, "owner", "Owner", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < rows->len; i++)
	{
		WorkRow *row = g_ptr_array_index(rows, i);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "invoice", row->number);
		venture_report_result_set_text(result, "customer", row->customer);
		venture_report_result_set_number(result, "days_overdue", row->days > 0 ? row->days : 0);
		venture_report_result_set_text(result, "aging", aging_bucket(row->days));
		venture_report_result_set_money(result, "open_balance", row->balance);
		venture_report_result_set_text(result, "last_step", row->last_step);
		venture_report_result_set_text(result, "last_status", row->last_status);
		venture_report_result_set_text(result, "last_date", row->last_date);
		venture_report_result_set_text(result, "next", row->next);
		venture_report_result_set_text(result, "next_date", row->next_date);
		venture_report_result_set_text(result, "owner", row->owner);
	}
	return g_steal_pointer(&result);
}

void
venture_dunning_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "collections", "Collections",
		"Per reminder step: reminders sent, queued, failed, invoices paid within 7 days of it, and average days-to-pay before and after policy adoption",
		collections)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "dunning_worklist", "Dunning worklist",
		"One row per open overdue invoice: aging, balance, the last reminder step and what happens next, and who owns it",
		worklist)));
}

/* --- GObject -------------------------------------------------------------- */

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	VentureDunningService *self = VENTURE_DUNNING_SERVICE(object);
	switch (id)
	{
	case PROP_DATABASE: g_value_set_object(value, self->database); break;
	case PROP_BASE_URL: g_value_set_string(value, self->base_url); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureDunningService *self = VENTURE_DUNNING_SERVICE(object);
	switch (id)
	{
	case PROP_DATABASE:
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
		break;
	case PROP_BASE_URL:
		g_free(self->base_url);
		self->base_url = g_value_dup_string(value);
		break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
finalize(GObject *object)
{
	VentureDunningService *self = VENTURE_DUNNING_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_free(self->base_url);
	G_OBJECT_CLASS(venture_dunning_service_parent_class)->finalize(object);
}

static void
venture_dunning_service_class_init(VentureDunningServiceClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->get_property = get_property;
	object->set_property = set_property;
	object->finalize = finalize;
	properties[PROP_DATABASE] = g_param_spec_object("database", "Database", "Weak owning database",
		VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	properties[PROP_BASE_URL] = g_param_spec_string("base-url", "Base URL",
		"Public HTTPS application URL the customer portal pay link is built from; anything else renders no link",
		NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(object, N_PROPS, properties);
}

static void
venture_dunning_service_init(VentureDunningService *self)
{
	(void)self;
}

VentureDunningService *
venture_dunning_service_get(VentureDatabase *database)
{
	VentureDunningService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-dunning-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_DUNNING_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-dunning-service", self, g_object_unref);
		venture_database_add_save_validator(database, VENTURE_TYPE_DUNNING_POLICY, policy_validate, self, NULL);
		venture_database_add_save_validator(database, VENTURE_TYPE_DUNNING_EVENT, event_validate, self, NULL);
		/* Both emitters are owned by the database, as is this service; the
		 * object-bound connections make the order they die in irrelevant. */
		g_signal_connect_object(database, "entity-saved", G_CALLBACK(entity_saved), self, 0);
		g_signal_connect_object(venture_database_get_mail_outbox(database), "before-send", G_CALLBACK(before_send), self, 0);
		venture_dunning_actions_register(database);
	}
	return self;
}
