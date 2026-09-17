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
 * is (invoice, step), which is what dunning_event.dunning_key pins.
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
#define STATUS_ESCALATED "escalated"
#define STATUS_SUPPRESSED "suppressed"
#define STATUS_FAILED "failed"
#define STATUS_CANCELLED "cancelled"

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

/* --- Policy steps --------------------------------------------------------- */

static GArray *
parse_steps(const gchar *json, GError **error)
{
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GArray) steps = g_array_new(FALSE, TRUE, sizeof(Step));
	JsonArray *array;
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
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *node = json_array_get_element(array, i);
		JsonObject *object;
		Step step;
		if (!JSON_NODE_HOLDS_OBJECT(node))
		{
			refuse(error, "Each step must be an object with offset and template_id");
			return NULL;
		}
		object = json_node_get_object(node);
		if (!json_object_has_member(object, "offset") || !json_object_has_member(object, "template_id"))
		{
			refuse(error, "Each step needs an offset in days and a template_id");
			return NULL;
		}
		step.offset = venture_json_object_get_int(object, "offset", 0);
		step.template_id = venture_json_object_get_int(object, "template_id", 0);
		if (step.template_id <= 0)
		{
			refuse(error, "Each step must name a mail_template");
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
	g_autoptr(GArray) steps = NULL;
	guint i;
	(void)previous;
	(void)data;
	json = text(entity, "steps");
	steps = parse_steps(json, error);
	if (steps == NULL)
		return FALSE;
	for (i = 0; i < steps->len; i++)
	{
		g_autoptr(VentureEntity) template = NULL;
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_template");
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

/* The invoice's own policy, then its customer's, then the organization
 * default; an invoice switched off names none at all. */
static VentureEntity *
resolve_policy(VentureDunningService *self, VentureEntity *invoice, VentureEntity *company, GError **error)
{
	gint64 org = venture_entity_get_organization_id(invoice);
	gint64 id;
	if (flag(invoice, "dunning-disabled"))
		return NULL;
	id = number(invoice, "dunning-policy-id");
	if (id <= 0 && company != NULL)
		id = number(company, "dunning-policy-id");
	if (id > 0)
		return load(self, VENTURE_TYPE_DUNNING_POLICY, id, org);
	return default_policy(self, org, error);
}

/* Why no further reminder may go to this invoice, or NULL while it is
 * collectible. Only a customer's opt-out is worth a recorded event; a
 * settled or disputed invoice has simply left dunning. */
static const gchar *
stop_reason(VentureDunningService *self, VentureEntity *invoice, VentureEntity *company)
{
	g_autofree gchar *workflow = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	gint status = 0;
	g_object_get(invoice, "status", &status, "workflow-state", &workflow, NULL);
	if (status == VENTURE_INVOICE_STATUS_PAID)
		return "settled";
	if (status != VENTURE_INVOICE_STATUS_SENT && status != VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
		return "not_issued";
	if (g_strcmp0(workflow, "disputed") == 0)
		return "disputed";
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database),
		venture_entity_get_id(invoice), NULL, NULL);
	if (balance == NULL || venture_money_is_zero(balance) || venture_money_is_negative(balance))
		return "settled";
	if (flag(invoice, "dunning-disabled"))
		return "invoice_disabled";
	if (company != NULL && flag(company, "dunning-opt-out"))
		return "company_opt_out";
	return NULL;
}

static gboolean
recorded_reason(const gchar *reason)
{
	return g_strcmp0(reason, "company_opt_out") == 0;
}

/* --- Rendering ------------------------------------------------------------ */

static gchar *
live_portal_link(VentureDunningService *self, gint64 org, gint64 company_id)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "customer_portal_access");
	g_autofree gchar *base = NULL;
	guint i;
	if (type == G_TYPE_INVALID || company_id <= 0 || venture_string_is_empty(self->base_url))
		return NULL;
	query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ, company_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(self->database, query, NULL);
	base = g_strdup(self->base_url);
	while (g_str_has_suffix(base, "/"))
		base[strlen(base) - 1] = '\0';
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *access = g_ptr_array_index(rows, i);
		g_autofree gchar *token = NULL;
		if (flag(access, "revoked"))
			continue;
		token = text(access, "token");
		if (venture_string_is_empty(token))
			continue;
		return g_strdup_printf("%s/portal/%s", base, token);
	}
	return NULL;
}

/* The values a reminder template may name. Only public invoice fields
 * enter, plus the customer, the balance and the pay link; the link is a
 * bearer credential and is only ever placed in the private body. */
static JsonObject *
render_values(VentureDunningService *self, VentureEntity *invoice, VentureEntity *company,
	VentureEntity *contact, VentureEntity *policy, const Step *step, guint step_no, gint days, const gchar *pay_link)
{
	g_autoptr(JsonNode) base = venture_serializable_to_json(VENTURE_SERIALIZABLE(invoice), FALSE);
	JsonObject *values = json_object_ref(json_node_get_object(base));
	g_autoptr(VentureMoney) balance = venture_settlement_service_invoice_balance(
		venture_settlement_service_get(self->database), venture_entity_get_id(invoice), NULL, NULL);
	g_autoptr(GDateTime) due = when(invoice, "due-at");
	g_autoptr(GDateTime) issued = when(invoice, "issued-at");
	g_autofree gchar *due_text = due ? venture_time_to_date_string(due, NULL) : g_strdup("");
	g_autofree gchar *issued_text = issued ? venture_time_to_date_string(issued, NULL) : g_strdup("");
	g_autofree gchar *display = balance ? venture_money_to_display_string(balance, TRUE) : g_strdup("");
	g_autofree gchar *exact = balance ? venture_money_to_string(balance) : g_strdup("");
	g_autofree gchar *customer = company ? text(company, "name") : NULL;
	g_autofree gchar *contact_name = contact ? text(contact, "name") : NULL;
	g_autofree gchar *policy_name = text(policy, "name");
	/* People read dates, not instants. */
	json_object_set_string_member(values, "due_at", due_text);
	json_object_set_string_member(values, "issued_at", issued_text);
	json_object_set_string_member(values, "open_balance", display);
	json_object_set_string_member(values, "open_balance_exact", exact);
	json_object_set_string_member(values, "customer_name", customer ? customer : "");
	json_object_set_string_member(values, "contact_name", contact_name ? contact_name : "");
	json_object_set_string_member(values, "policy_name", policy_name ? policy_name : "");
	json_object_set_string_member(values, "pay_link", pay_link ? pay_link : "");
	json_object_set_int_member(values, "days_overdue", days > 0 ? days : 0);
	json_object_set_int_member(values, "days_until_due", days < 0 ? -days : 0);
	json_object_set_int_member(values, "step", step_no);
	json_object_set_int_member(values, "offset_days", step->offset);
	return values;
}

static VentureEntity *
new_event(VentureDunningService *self, VentureEntity *invoice, VentureEntity *policy,
	guint step_no, const Step *step, const gchar *key, GDateTime *as_of, const gchar *status)
{
	VentureEntity *event = new_record(VENTURE_TYPE_DUNNING_EVENT, venture_entity_get_organization_id(invoice));
	(void)self;
	g_object_set(event, "invoice-id", venture_entity_get_id(invoice), "policy-id", venture_entity_get_id(policy),
		"company-id", number(invoice, "company-id"), "contact-id", number(invoice, "contact-id"),
		"step", (gint64)step_no, "offset-days", step->offset, "dunning-key", key, "queued-at", as_of,
		"delivery-status", status, NULL);
	return event;
}

static gboolean
record_suppressed(VentureDunningService *self, VentureEntity *invoice, VentureEntity *policy, guint step_no,
	const Step *step, const gchar *key, GDateTime *as_of, const gchar *reason, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) event = new_event(self, invoice, policy, step_no, step, key, as_of, STATUS_SUPPRESSED);
	g_object_set(event, "suppressed-reason", reason, NULL);
	return save_event(self, event, actor, error);
}

/* The final step: an owned next action, never another customer email. */
static gboolean
escalate(VentureDunningService *self, VentureEntity *invoice, VentureEntity *policy, guint step_no,
	const Step *step, const gchar *key, GDateTime *as_of, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) event = new_event(self, invoice, policy, step_no, step, key, as_of, STATUS_ESCALATED);
	g_autoptr(VentureEntity) activity = NULL;
	g_autofree gchar *invoice_number = text(invoice, "number");
	g_autofree gchar *owner = text(invoice, "owner");
	g_autofree gchar *subject = g_strdup_printf("collect: %s", invoice_number ? invoice_number : "");
	g_autofree gchar *body = NULL;
	g_autoptr(VentureMoney) balance = venture_settlement_service_invoice_balance(
		venture_settlement_service_get(self->database), venture_entity_get_id(invoice), NULL, NULL);
	g_autofree gchar *display = balance ? venture_money_to_display_string(balance, TRUE) : g_strdup("");
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "activity");
	if (type == G_TYPE_INVALID)
	{
		/* The attempt is still recorded so the invoice is not silently forgotten. */
		g_object_set(event, "delivery-status", STATUS_FAILED, "last-error", "Escalation needs the activities module", NULL);
		return save_event(self, event, actor, error);
	}
	body = g_strdup_printf("Reminder policy exhausted after %u steps; %s still outstanding. Call, agree a date or write off.",
		step_no - 1, display);
	activity = new_record(type, venture_entity_get_organization_id(invoice));
	g_object_set(activity, "subject", subject, "kind", VENTURE_ACTIVITY_KIND_FOLLOWUP, "body", body,
		"owner", owner ? owner : "", "due-at", as_of, "priority", VENTURE_PRIORITY_HIGH,
		"status", VENTURE_ACTIVITY_STATUS_PLANNED, "related-type", "invoice", "related-id", venture_entity_get_id(invoice),
		"company-id", number(invoice, "company-id"), "contact-id", number(invoice, "contact-id"), NULL);
	if (!venture_database_save(self->database, activity, actor, error))
		return FALSE;
	g_object_set(event, "activity-id", venture_entity_get_id(activity), NULL);
	return save_event(self, event, actor, error);
}

static gboolean
send_step(VentureDunningService *self, VentureEntity *invoice, VentureEntity *company, VentureEntity *contact,
	VentureEntity *policy, guint step_no, const Step *step, const gchar *key, GDateTime *as_of, gint days,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) event = new_event(self, invoice, policy, step_no, step, key, as_of, STATUS_QUEUED);
	g_autoptr(VentureEntity) template = NULL;
	g_autofree gchar *recipient = contact ? text(contact, "email") : NULL;
	g_autofree gchar *pay_link = NULL;
	g_autoptr(JsonObject) public_values = NULL;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(GError) local = NULL;
	gint64 org = venture_entity_get_organization_id(invoice);
	if (venture_string_is_empty(recipient) && company != NULL)
	{
		g_free(recipient);
		recipient = text(company, "email");
	}
	if (venture_string_is_empty(recipient))
	{
		g_object_set(event, "delivery-status", STATUS_SUPPRESSED, "suppressed-reason", "no_recipient", NULL);
		return save_event(self, event, actor, error);
	}
	template = load(self, venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_template"), step->template_id, org);
	if (template == NULL)
	{
		g_object_set(event, "delivery-status", STATUS_FAILED, "last-error", "The step's mail_template is missing", NULL);
		return save_event(self, event, actor, error);
	}
	pay_link = live_portal_link(self, org, number(invoice, "company-id"));
	public_values = render_values(self, invoice, company, contact, policy, step, step_no, days, NULL);
	message = venture_mail_template_render_values(VENTURE_MAIL_TEMPLATE(template), public_values, &local);
	if (message == NULL)
	{
		g_object_set(event, "delivery-status", STATUS_FAILED, "last-error", local->message, NULL);
		return save_event(self, event, actor, error);
	}
	if (pay_link != NULL)
	{
		/* The portal link is a bearer token: it travels only in the private
		 * body the transport sends, never in the stored public bodies. */
		g_autoptr(JsonObject) private_values = render_values(self, invoice, company, contact, policy, step, step_no, days, pay_link);
		g_autoptr(VentureMailMessage) private_message = venture_mail_template_render_values(VENTURE_MAIL_TEMPLATE(template), private_values, &local);
		g_autofree gchar *private_text = NULL;
		if (private_message == NULL)
		{
			g_object_set(event, "delivery-status", STATUS_FAILED, "last-error", local->message, NULL);
			return save_event(self, event, actor, error);
		}
		private_text = text(VENTURE_ENTITY(private_message), "text-body");
		g_object_set(message, "private-text-body", private_text, NULL);
	}
	if (!save_event(self, event, actor, error))
		return FALSE;
	g_object_set(message, "to", recipient, "idempotency-key", key,
		"related-type", "dunning_event", "related-id", venture_entity_get_id(event), NULL);
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(self->database), message, actor, error);
	if (queued == NULL)
		return FALSE;
	g_object_set(event, "mail-message-id", venture_entity_get_id(VENTURE_ENTITY(queued)), NULL);
	return save_event(self, event, actor, error);
}

/* After a per-invoice transaction failed, keep the attempt as evidence in a
 * transaction of its own. Without this a crash between enqueue and commit
 * would look, next sweep, like a step that was never due. */
static void
record_failure(VentureDunningService *self, VentureEntity *invoice, VentureEntity *policy, guint step_no,
	const Step *step, const gchar *key, GDateTime *as_of, const gchar *why, const VentureActor *actor)
{
	g_autoptr(VentureEntity) event = new_event(self, invoice, policy, step_no, step, key, as_of, STATUS_FAILED);
	g_autoptr(GError) ignored = NULL;
	g_object_set(event, "last-error", why, NULL);
	save_event(self, event, actor, &ignored);
}

static gchar *
step_key(VentureEntity *invoice, guint step_no)
{
	return g_strdup_printf("dunning:inv:%" G_GINT64_FORMAT ":step:%u", venture_entity_get_id(invoice), step_no);
}

static GHashTable *
recorded_steps(VentureDunningService *self, VentureEntity *invoice, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DUNNING_EVENT);
	g_autoptr(GPtrArray) rows = NULL;
	GHashTable *steps;
	guint i;
	venture_query_set_organization(query, venture_entity_get_organization_id(invoice));
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(invoice), NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return NULL;
	steps = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (i = 0; i < rows->len; i++)
		g_hash_table_add(steps, GINT_TO_POINTER((gint)number(g_ptr_array_index(rows, i), "step")));
	return steps;
}

/* One invoice, one transaction. Returns sends plus escalations, or -1. */
static gint
sweep_invoice(VentureDunningService *self, VentureEntity *invoice, GDateTime *as_of, const VentureActor *actor, GError **error)
{
	gint64 org = venture_entity_get_organization_id(invoice);
	g_autoptr(VentureEntity) company = load(self, VENTURE_TYPE_COMPANY, number(invoice, "company-id"), org);
	g_autoptr(VentureEntity) contact = load(self, VENTURE_TYPE_CONTACT, number(invoice, "contact-id"), org);
	g_autoptr(VentureEntity) policy = NULL;
	g_autoptr(GArray) steps = NULL;
	g_autoptr(GHashTable) done = NULL;
	g_autoptr(GDateTime) due = when(invoice, "due-at");
	g_autofree gchar *json = NULL;
	g_autoptr(GArray) pending = g_array_new(FALSE, FALSE, sizeof(guint));
	const gchar *reason;
	gint days;
	gint acted = 0;
	guint i;
	guint latest;
	policy = resolve_policy(self, invoice, company, error);
	if (policy == NULL)
		return (error && *error) ? -1 : 0;
	if (due == NULL)
		return 0;
	json = text(policy, "steps");
	steps = parse_steps(json, NULL);
	if (steps == NULL || steps->len == 0)
		return 0;
	reason = stop_reason(self, invoice, company);
	if (reason != NULL && !recorded_reason(reason))
		return 0;
	days = calendar_days(as_of, due);
	done = recorded_steps(self, invoice, error);
	if (done == NULL)
		return -1;
	for (i = 0; i < steps->len; i++)
	{
		guint step_no = i + 1;
		if (g_array_index(steps, Step, i).offset > days)
			break;
		if (!g_hash_table_contains(done, GINT_TO_POINTER((gint)step_no)))
			g_array_append_val(pending, step_no);
	}
	if (pending->len == 0)
		return 0;
	latest = g_array_index(pending, guint, pending->len - 1);
	if (!venture_database_begin(self->database, error))
		return -1;
	for (i = 0; i < pending->len; i++)
	{
		guint step_no = g_array_index(pending, guint, i);
		const Step *step = &g_array_index(steps, Step, step_no - 1);
		g_autofree gchar *key = step_key(invoice, step_no);
		gboolean ok;
		if (reason != NULL)
			ok = record_suppressed(self, invoice, policy, step_no, step, key, as_of, reason, actor, error);
		else if (step_no != latest)
			/* Several steps fell due since the last sweep: the customer gets
			 * the current one, not a burst of every message they missed. */
			ok = record_suppressed(self, invoice, policy, step_no, step, key, as_of, "superseded", actor, error);
		else if (step_no == steps->len && flag(policy, "final-escalation"))
		{
			ok = escalate(self, invoice, policy, step_no, step, key, as_of, actor, error);
			acted += ok;
		}
		else
		{
			ok = send_step(self, invoice, company, contact, policy, step_no, step, key, as_of, days, actor, error);
			acted += ok;
		}
		if (!ok)
		{
			g_autofree gchar *why = g_strdup(error && *error ? (*error)->message : "sweep failed");
			venture_database_rollback(self->database);
			record_failure(self, invoice, policy, step_no, step, key, as_of, why, actor);
			return -1;
		}
	}
	if (!venture_database_commit(self->database, error))
		return -1;
	return acted;
}

static GPtrArray *
open_invoices(VentureDunningService *self, gint64 org, gint status, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	venture_query_set_organization(query, org);
	venture_query_add_filter_int(query, "status", VENTURE_FILTER_OP_EQ, status, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	return venture_database_find(self->database, query, error);
}

gint
venture_dunning_service_sweep(VentureDunningService *self, gint64 organization_id, GDateTime *as_of, guint limit,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) clock = NULL;
	g_autoptr(GPtrArray) sent = NULL;
	g_autoptr(GPtrArray) partial = NULL;
	guint visited = 0;
	guint i;
	gint total = 0;
	const gint statuses[] = { VENTURE_INVOICE_STATUS_SENT, VENTURE_INVOICE_STATUS_PARTIALLY_PAID };
	GPtrArray *sets[2];
	g_return_val_if_fail(VENTURE_IS_DUNNING_SERVICE(self), -1);
	if (self->database == NULL)
		return refuse(error, "The database is gone") ? -1 : -1;
	if (organization_id <= 0)
		return refuse(error, "An organization is required") ? -1 : -1;
	if (!enabled(error))
		return -1;
	if (venture_database_has_transaction(self->database))
		return refuse(error, "The sweep owns one transaction per invoice; call it outside a transaction") ? -1 : -1;
	if (limit == 0)
		limit = 100;
	limit = MIN(limit, 1000);
	clock = as_of ? g_date_time_ref(as_of) : venture_time_now();
	sent = open_invoices(self, organization_id, statuses[0], error);
	if (sent == NULL)
		return -1;
	partial = open_invoices(self, organization_id, statuses[1], error);
	if (partial == NULL)
		return -1;
	sets[0] = sent;
	sets[1] = partial;
	for (i = 0; i < 2; i++)
	{
		guint j;
		for (j = 0; j < sets[i]->len && visited < limit; j++)
		{
			gint n = sweep_invoice(self, g_ptr_array_index(sets[i], j), clock, actor, error);
			if (n < 0)
				return -1;
			/* Bounded by invoices that produced something, so a large book of
			 * quiet invoices does not exhaust the budget before the due ones. */
			if (n > 0)
				visited++;
			total += n;
		}
	}
	return total;
}

/* --- Outbox hooks --------------------------------------------------------- */

static VentureEntity *
event_for_message(VentureDunningService *self, VentureEntity *message)
{
	g_autofree gchar *related = text(message, "related-type");
	if (g_strcmp0(related, "dunning_event") != 0)
		return NULL;
	return load(self, VENTURE_TYPE_DUNNING_EVENT, number(message, "related-id"), venture_entity_get_organization_id(message));
}

/* The eligibility recheck between claim and transport: a reminder queued
 * before the invoice was paid, disputed or the customer opted out is
 * cancelled here, and the event says why. */
static gboolean
before_send(VentureMailOutbox *outbox, VentureMailMessage *message, GError **error, gpointer data)
{
	VentureDunningService *self = data;
	g_autoptr(VentureEntity) event = event_for_message(self, VENTURE_ENTITY(message));
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(GError) ignored = NULL;
	const gchar *reason;
	gint64 org;
	(void)outbox;
	if (event == NULL || self->database == NULL)
		return FALSE;
	org = venture_entity_get_organization_id(event);
	invoice = load(self, VENTURE_TYPE_INVOICE, number(event, "invoice-id"), org);
	if (invoice == NULL)
		reason = "invoice_gone";
	else
	{
		company = load(self, VENTURE_TYPE_COMPANY, number(invoice, "company-id"), org);
		reason = stop_reason(self, invoice, company);
	}
	if (reason == NULL)
		return FALSE;
	g_object_set(event, "delivery-status", STATUS_CANCELLED, "suppressed-reason", reason, NULL);
	save_event(self, event, NULL, &ignored);
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
	g_autoptr(GError) ignored = NULL;
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
	save_event(self, event, NULL, &ignored);
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
	gint64 org = param_id(params, "organization_id");
	gint64 limit = param_id(params, "limit");
	if (!venture_string_is_empty(as_of_text))
	{
		as_of = venture_time_from_string(as_of_text, error);
		if (as_of == NULL)
			return NULL;
	}
	if (org <= 0 && entity != NULL)
		org = venture_entity_get_organization_id(entity);
	if (org <= 0)
		org = 1;
	if (venture_dunning_service_sweep(self, org, as_of, (guint)CLAMP(limit, 0, 1000), actor, error) < 0)
		return NULL;
	return entity ? g_object_ref(entity) : g_object_new(VENTURE_TYPE_DUNNING_POLICY, NULL);
}

void
venture_dunning_actions_register(VentureDatabase *database)
{
	VentureDunningService *self = venture_dunning_service_get(database);
	VentureActionRegistry *registry;
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	if (self->actions)
		return;
	self->actions = TRUE;
	registry = venture_database_get_action_registry(database);
	g_ptr_array_add(parameters, venture_field_spec_new("as_of", "As of", VENTURE_FIELD_KIND_DATETIME));
	g_ptr_array_add(parameters, venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER));
	g_ptr_array_add(parameters, venture_field_spec_new("limit", "Limit", VENTURE_FIELD_KIND_INTEGER));
	action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "dunning_policy", "name", "sweep",
		"label", "Send due reminders", "description", "Enqueue every reminder step that fell due, once",
		"parameters", parameters, "stageable", TRUE, "type-level", TRUE, "service-transaction", TRUE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(registry, action, sweep_allowed, sweep_invoke, self, NULL, &error))
		g_error("Dunning action registration: %s", error->message);
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
	if (g_strcmp0(status, "sent") == 0)
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

void
venture_dunning_append_timeline(VentureContext *context, const gchar *target_type, gint64 target_id,
	guint limit, VentureDunningTimelineAdd add, GPtrArray *events)
{
	const gchar *field;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_return_if_fail(add != NULL && events != NULL);
	if (g_strcmp0(target_type, "invoice") == 0)
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
		if (g_strcmp0(status, "sent") != 0 || sent_at == NULL)
			continue;
		day = venture_time_to_date_string(sent_at, NULL);
		g_string_append_printf(out, "%sreminder sent %s", out->len ? "; " : "", day);
	}
	return g_string_free(out, FALSE);
}

/* --- Report --------------------------------------------------------------- */

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

static VentureEntity *
find_by_id(GPtrArray *rows, gint64 id)
{
	guint i;
	for (i = 0; rows != NULL && i < rows->len; i++)
		if (venture_entity_get_id(g_ptr_array_index(rows, i)) == id)
			return g_ptr_array_index(rows, i);
	return NULL;
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
	venture_report_result_add_column(result, "policy", "Policy", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "step", "Step", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "offset_days", "Offset (days)", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "reminders_sent", "Reminders sent", VENTURE_REPORT_COLUMN_NUMBER);
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
		g_autoptr(GArray) steps = parse_steps(json, NULL);
		g_autoptr(GDateTime) adopted = when(policy, "adopted-at");
		gdouble before_sum = 0, after_sum = 0;
		guint before_n = 0, after_n = 0;
		guint i, s;
		if (adopted == NULL && venture_entity_get_created_at(policy) != NULL)
			adopted = g_date_time_ref(venture_entity_get_created_at(policy));
		/* Before/after is by issue date against adoption, over invoices this
		 * policy governed or would have governed: the default policy sees
		 * every invoice naming none, a named policy only those naming it. */
		for (i = 0; i < invoices->len; i++)
		{
			VentureEntity *invoice = g_ptr_array_index(invoices, i);
			g_autoptr(GDateTime) issued = when(invoice, "issued-at");
			gint64 named = number(invoice, "dunning-policy-id");
			gdouble days;
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
			guint sent = 0, escalations = 0, suppressed = 0, paid_soon = 0;
			for (i = 0; i < events->len; i++)
			{
				VentureEntity *event = g_ptr_array_index(events, i);
				g_autofree gchar *status = NULL;
				g_autoptr(GDateTime) at = NULL;
				g_autoptr(GDateTime) queued_at = NULL;
				VentureEntity *invoice;
				g_autoptr(GDateTime) paid = NULL;
				if (number(event, "policy-id") != venture_entity_get_id(policy) || number(event, "step") != (gint64)(s + 1))
					continue;
				status = text(event, "delivery-status");
				if (g_strcmp0(status, STATUS_ESCALATED) == 0)
				{
					escalations++;
					continue;
				}
				if (g_strcmp0(status, "sent") != 0)
				{
					suppressed += g_strcmp0(status, STATUS_SUPPRESSED) == 0 || g_strcmp0(status, STATUS_CANCELLED) == 0;
					continue;
				}
				sent++;
				at = when(event, "sent-at");
				queued_at = when(event, "queued-at");
				if (at == NULL)
					at = queued_at ? g_date_time_ref(queued_at) : NULL;
				invoice = find_by_id(invoices, number(event, "invoice-id"));
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
			venture_report_result_set_number(result, "escalations", escalations);
			venture_report_result_set_number(result, "suppressed", suppressed);
			venture_report_result_set_number(result, "paid_within_7_days", paid_soon);
			venture_report_result_set_number(result, "average_days_before", before_n ? before_sum / before_n : 0);
			venture_report_result_set_number(result, "average_days_after", after_n ? after_sum / after_n : 0);
		}
	}
	return g_steal_pointer(&result);
}

void
venture_dunning_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("collections", "Collections",
		"Per reminder step: reminders sent, invoices paid within 7 days of it, and average days-to-pay before and after policy adoption",
		collections)));
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
		"Public application URL the customer portal pay link is built from; empty renders no link",
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
		g_signal_connect(database, "entity-saved", G_CALLBACK(entity_saved), self);
		g_signal_connect(venture_database_get_mail_outbox(database), "before-send", G_CALLBACK(before_send), self);
		venture_dunning_actions_register(database);
	}
	return self;
}
