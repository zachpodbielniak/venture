/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureRecurringService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
	gboolean busy;
	gboolean actions;
};

struct _VentureCollectionService
{
	GObject parent_instance;
	VentureDatabase *database;
	GWeakRef context;
	VentureEntity *writing;
	gboolean busy;
	gboolean actions;
};

G_DEFINE_FINAL_TYPE(VentureRecurringService, venture_recurring_service, G_TYPE_OBJECT)
G_DEFINE_FINAL_TYPE(VentureCollectionService, venture_collection_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureRecurringService: %s", message);
	return FALSE;
}

static gint64
number(VentureEntity *e, const gchar *field)
{
	gint64 value = 0;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gint
choice(VentureEntity *e, const gchar *field)
{
	gint value = 0;
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

static JsonNode *
param_node(GHashTable *params, const gchar *name)
{
	return params ? g_hash_table_lookup(params, name) : NULL;
}

static const gchar *
param_string(GHashTable *params, const gchar *name)
{
	JsonNode *node = param_node(params, name);
	if (node == NULL || JSON_NODE_HOLDS_NULL(node))
		return NULL;
	if (JSON_NODE_HOLDS_VALUE(node) && G_TYPE_STRING == json_node_get_value_type(node))
		return json_node_get_string(node);
	return NULL;
}

static gboolean
param_bool(GHashTable *params, const gchar *name)
{
	JsonNode *node = param_node(params, name);
	if (node == NULL || JSON_NODE_HOLDS_NULL(node))
		return FALSE;
	if (JSON_NODE_HOLDS_VALUE(node) && G_TYPE_BOOLEAN == json_node_get_value_type(node))
		return json_node_get_boolean(node);
	if (JSON_NODE_HOLDS_VALUE(node) && G_TYPE_STRING == json_node_get_value_type(node))
		return g_strcmp0(json_node_get_string(node), "true") == 0;
	return FALSE;
}

static GDateTime *
param_date(GHashTable *params, const gchar *name, GError **error)
{
	const gchar *text = param_string(params, name);
	if (venture_string_is_empty(text))
		return venture_time_now();
	return venture_time_from_string(text, error);
}

static gint64
param_id(GHashTable *params, const gchar *name)
{
	JsonNode *node = param_node(params, name);
	const gchar *text;
	if (node == NULL || JSON_NODE_HOLDS_NULL(node))
		return 0;
	if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) != G_TYPE_STRING)
		return json_node_get_int(node);
	text = json_node_get_string(node);
	return text ? g_ascii_strtoll(text, NULL, 10) : 0;
}

static gboolean
empty_text(VentureEntity *entity, const gchar *field)
{
	g_autofree gchar *value = NULL;
	g_object_get(entity, field, &value, NULL);
	return venture_string_is_empty(value);
}

static gchar *
day_text(GDateTime *date)
{
	return date ? g_date_time_format(date, "%F") : g_strdup("");
}

static gint
day_compare(GDateTime *a, GDateTime *b)
{
	g_autofree gchar *left = day_text(a);
	g_autofree gchar *right = day_text(b);
	return g_strcmp0(left, right);
}

static GDateTime *
schedule_start(VentureEntity *schedule)
{
	g_autoptr(GDateTime) start = NULL;
	g_autofree gchar *name = NULL;
	g_autoptr(GTimeZone) zone = NULL;
	g_object_get(schedule, "start-at", &start, "timezone", &name, NULL);
	zone = g_time_zone_new_identifier(venture_string_is_empty(name) ? "UTC" : name);
	return start != NULL && zone != NULL ? g_date_time_to_timezone(start, zone) : NULL;
}

static GDateTime *
cycle_date(GDateTime *start, gint frequency, gint64 index)
{
	/* Date APIs take gint offsets; reject wraparound before multiplication. */
	if (start == NULL || index < 0 || index > G_MAXINT / 7)
		return NULL;
	if (frequency == 1)
		return g_date_time_add_days(start, 7 * (gint)index);
	if (frequency == 2)
		return g_date_time_add_days(start, (gint)index);
	if (frequency == 3)
		return g_date_time_add_years(start, (gint)index);
	return g_date_time_add_months(start, (gint)index);
}

static VentureEntity *
new_record(GType type, gint64 org)
{
	VentureEntity *e = g_object_new(type, NULL);
	venture_entity_set_organization_id(e, org);
	return e;
}

static gboolean
schedule_validate(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	VentureRecurringService *self = data;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) next = NULL;
	g_autofree gchar *zone_name = NULL;
	g_autoptr(GTimeZone) zone = NULL;
	(void)database;
	if (!VENTURE_IS_RECURRING_SCHEDULE(entity))
		return TRUE;
	g_object_get(entity, "start-at", &start, "next-run-at", &next, NULL);
	if (start == NULL)
		return refuse(error, "A schedule needs a start date");
	g_object_get(entity, "timezone", &zone_name, NULL);
	zone = g_time_zone_new_identifier(venture_string_is_empty(zone_name) ? "UTC" : zone_name);
	if (zone == NULL)
		return refuse(error, "A schedule needs a valid IANA timezone");
	if (number(entity, "cycle-index") < 0 || number(entity, "cycle-index") > G_MAXINT / 7)
		return refuse(error, "Schedule cycle index is outside the calendar range");
	if (next == NULL)
		g_object_set(entity, "next-run-at", start, NULL);
	if (self->writing == entity)
		return TRUE;
	if (previous != NULL && number(entity, "cycle-index") != number(previous, "cycle-index"))
		return refuse(error, "Use VentureRecurringService to advance a schedule");
	return TRUE;
}

static gboolean
occurrence_validate(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	VentureRecurringService *self = data;
	(void)database;
	(void)previous;
	if (!VENTURE_IS_RECURRING_OCCURRENCE(entity))
		return TRUE;
	if (self->writing == entity)
		return TRUE;
	return refuse(error, "Recurring occurrences are written by VentureRecurringService");
}

static void
permit(VentureRecurringService *self, VentureEntity *entity)
{
	self->writing = entity;
}

static void
release_permit(VentureRecurringService *self)
{
	self->writing = NULL;
}

static gboolean
save_owned(VentureRecurringService *self, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	gboolean ok;
	permit(self, entity);
	ok = venture_database_save(self->database, entity, actor, error);
	release_permit(self);
	return ok;
}

static JsonObject *
parse_template(VentureEntity *schedule, GError **error)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_object_get(schedule, "template", &text, NULL);
	if (venture_string_is_empty(text))
	{
		refuse(error, "A schedule template is required");
		return NULL;
	}
	node = venture_json_parse(text, error);
	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
	{
		if (error && *error == NULL)
			refuse(error, "The schedule template must be a JSON object");
		return NULL;
	}
	return json_object_ref(json_node_get_object(node));
}

static void
copy_member(JsonObject *object, const gchar *name, JsonNode *node, gpointer data)
{
	if (g_strcmp0(name, "lines") != 0)
		json_object_set_member(data, name, json_node_copy(node));
}

static gboolean
apply_object(VentureEntity *entity, JsonObject *object, GError **error)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonObject) copy = json_object_new();
	gint64 organization_id = venture_entity_get_organization_id(entity);
	json_object_foreach_member(object, copy_member, copy);
	json_node_take_object(node, g_steal_pointer(&copy));
	if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(entity), node, error))
		return FALSE;
	if (venture_entity_get_organization_id(entity) != organization_id)
		return refuse(error, "A template cannot change its target organization");
	return TRUE;
}

static JsonArray *
template_lines(JsonObject *object)
{
	JsonNode *node = json_object_get_member(object, "lines");
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
		return NULL;
	return json_node_get_array(node);
}

static gboolean
postable(VentureRecurringService *self, gint64 org, GDateTime *date, GError **error)
{
	return venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
		self->database, org, date, error);
}

static VentureEntity *
find_occurrence(VentureRecurringService *self, gint64 org, const gchar *key, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_RECURRING_OCCURRENCE);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_string(query, "occurrence-key", VENTURE_FILTER_OP_EQ, key, NULL);
	return venture_database_find_one(self->database, query, error);
}

static gboolean
write_occurrence(VentureRecurringService *self, VentureEntity *schedule, const gchar *key,
	gint status, const gchar *type_name, gint64 document_id, const gchar *message,
	GDateTime *at, const VentureActor *actor, GError **error)
{
	g_autoptr(GError) local = NULL;
	g_autoptr(VentureEntity) row = find_occurrence(self, venture_entity_get_organization_id(schedule), key, &local);
	if (local != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	if (row == NULL)
		row = new_record(VENTURE_TYPE_RECURRING_OCCURRENCE, venture_entity_get_organization_id(schedule));
	g_object_set(row, "schedule-id", venture_entity_get_id(schedule), "occurrence-key", key,
		"status", status, "document-type", type_name ? type_name : "", "document-id", document_id,
		"generated-at", at, "schedule-version", venture_entity_get_version(schedule), "error", message, NULL);
	return save_owned(self, row, actor, error);
}

static gboolean
advance(VentureRecurringService *self, VentureEntity *schedule, gint64 index, GDateTime *at,
	const gchar *err, const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) next = NULL;
	gint frequency = choice(schedule, "frequency");
	start = schedule_start(schedule);
	next = cycle_date(start, frequency, index + 1);
	g_object_set(schedule, "cycle-index", index + 1, "next-run-at", next,
		"last-generated-at", at, "last-error", err, NULL);
	return save_owned(self, schedule, actor, error);
}

static gboolean
add_invoice_lines(VentureRecurringService *self, gint64 org, gint64 invoice_id, JsonArray *lines,
	const VentureActor *actor, GError **error)
{
	guint i;
	if (lines == NULL || json_array_get_length(lines) == 0)
		return refuse(error, "An invoice template needs lines");
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		g_autoptr(VentureEntity) line = new_record(VENTURE_TYPE_INVOICE_LINE, org);
		JsonNode *node = json_array_get_element(lines, i);
		if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node) ||
			!venture_serializable_from_json(VENTURE_SERIALIZABLE(line), node, error))
			return FALSE;
		g_object_set(line, "invoice-id", invoice_id, "position", (gint64)i, NULL);
		if (!venture_database_save(self->database, line, actor, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
generate_invoice(VentureRecurringService *self, VentureEntity *schedule, JsonObject *object,
	GDateTime *at, gboolean auto_post, gint64 *document_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = new_record(VENTURE_TYPE_INVOICE, venture_entity_get_organization_id(schedule));
	g_autofree gchar *stamp = day_text(at);
	g_autofree gchar *fallback = g_strdup_printf("R%" G_GINT64_FORMAT "-%s", venture_entity_get_id(schedule), stamp);
	if (!apply_object(invoice, object, error))
		return FALSE;
	if (empty_text(invoice, "number"))
		g_object_set(invoice, "number", fallback, NULL);
	g_object_set(invoice, "issued-at", at, "due-at", at, NULL);
	if (!venture_database_save(self->database, invoice, actor, error) ||
		!add_invoice_lines(self, venture_entity_get_organization_id(schedule),
			venture_entity_get_id(invoice), template_lines(object), actor, error))
		return FALSE;
	if (auto_post && !venture_settlement_service_transition(venture_settlement_service_get(self->database),
		VENTURE_INVOICE(invoice), "sent", at, actor, error))
		return FALSE;
	*document_id = venture_entity_get_id(invoice);
	return TRUE;
}

static gboolean
generate_bill(VentureRecurringService *self, VentureEntity *schedule, JsonObject *object,
	GDateTime *at, gboolean auto_post, gint64 *document_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) bill = new_record(VENTURE_TYPE_VENDOR_BILL, venture_entity_get_organization_id(schedule));
	g_autofree gchar *stamp = day_text(at);
	g_autofree gchar *fallback = g_strdup_printf("B%" G_GINT64_FORMAT "-%s", venture_entity_get_id(schedule), stamp);
	JsonArray *lines = template_lines(object);
	guint i;
	if (!apply_object(bill, object, error))
		return FALSE;
	if (empty_text(bill, "number"))
		g_object_set(bill, "number", fallback, NULL);
	g_object_set(bill, "bill-date", at, "due-date", at, "status", "draft", NULL);
	if (empty_text(bill, "currency"))
	{
		g_autoptr(VentureEntity) organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION,
			venture_entity_get_organization_id(schedule), NULL);
		g_autofree gchar *currency = NULL;
		if (organization != NULL)
			g_object_get(organization, "default-currency", &currency, NULL);
		g_object_set(bill, "currency", currency != NULL && currency[0] != '\0' ? currency : NULL, NULL);
		if (empty_text(bill, "currency"))
			return refuse(error, "A bill template needs a currency");
	}
	if (!venture_database_save(self->database, bill, actor, error))
		return FALSE;
	if (lines == NULL || json_array_get_length(lines) == 0)
		return refuse(error, "A bill template needs lines");
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		g_autoptr(VentureEntity) line = new_record(VENTURE_TYPE_VENDOR_BILL_LINE, venture_entity_get_organization_id(schedule));
		JsonNode *node = json_array_get_element(lines, i);
		if (!JSON_NODE_HOLDS_OBJECT(node) || !venture_serializable_from_json(VENTURE_SERIALIZABLE(line), node, error))
			return FALSE;
		g_object_set(line, "bill-id", venture_entity_get_id(bill), "position", (gint64)i, NULL);
		if (!venture_database_save(self->database, line, actor, error))
			return FALSE;
	}
	if (auto_post)
	{
		g_autoptr(VentureEntity) event = new_record(VENTURE_TYPE_VENDOR_BILL_EVENT, venture_entity_get_organization_id(schedule));
		g_object_set(event, "bill-id", venture_entity_get_id(bill), "vendor-id", number(bill, "company-id"),
			"kind", "approve", "state", "approved", "date", at, NULL);
		if (!venture_database_save(self->database, event, actor, error))
			return FALSE;
	}
	*document_id = venture_entity_get_id(bill);
	return TRUE;
}

static gboolean
generate_expense(VentureRecurringService *self, VentureEntity *schedule, JsonObject *object,
	GDateTime *at, const gchar *key, gint64 *document_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) expense = new_record(VENTURE_TYPE_EXPENSE, venture_entity_get_organization_id(schedule));
	if (!apply_object(expense, object, error))
		return FALSE;
	g_object_set(expense, "occurred-at", at, "external-id", key, NULL);
	if (!venture_database_save(self->database, expense, actor, error))
		return FALSE;
	*document_id = venture_entity_get_id(expense);
	return TRUE;
}

static gboolean
generate_journal(VentureRecurringService *self, VentureEntity *schedule, JsonObject *object,
	GDateTime *at, const gchar *key, gboolean auto_post, gint64 *document_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
	JsonArray *lines = template_lines(object);
	guint i;
	venture_entity_set_organization_id(VENTURE_ENTITY(journal), venture_entity_get_organization_id(schedule));
	if (!apply_object(VENTURE_ENTITY(journal), object, error))
		return FALSE;
	g_object_set(journal, "occurred-at", at, "source-type", "recurring_schedule",
		"source-id", venture_entity_get_id(schedule), "posting-key", key, NULL);
	if (lines == NULL)
		return refuse(error, "A journal template needs lines");
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		g_autoptr(VentureJournalLine) line = venture_journal_line_new();
		JsonNode *node = json_array_get_element(lines, i);
		venture_entity_set_organization_id(VENTURE_ENTITY(line), venture_entity_get_organization_id(schedule));
		if (!JSON_NODE_HOLDS_OBJECT(node) || !venture_serializable_from_json(VENTURE_SERIALIZABLE(line), node, error))
			return FALSE;
		g_ptr_array_add(rows, g_steal_pointer(&line));
	}
	if (auto_post)
	{
		g_autoptr(VentureJournal) posted = venture_posting_service_post(
			venture_database_get_posting_service(self->database), journal, rows, NULL, actor, error);
		if (posted == NULL)
			return FALSE;
		*document_id = venture_entity_get_id(VENTURE_ENTITY(posted));
		return TRUE;
	}
	if (!venture_database_save(self->database, VENTURE_ENTITY(journal), actor, error))
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(rows, i);
		g_object_set(line, "journal-id", venture_entity_get_id(VENTURE_ENTITY(journal)), NULL);
		if (!venture_database_save(self->database, line, actor, error))
			return FALSE;
	}
	*document_id = venture_entity_get_id(VENTURE_ENTITY(journal));
	return TRUE;
}

static gboolean
generate_one(VentureRecurringService *self, VentureEntity *schedule, GDateTime *at, const gchar *key,
	gboolean dry_run, gint *created, const VentureActor *actor, GError **error)
{
	g_autoptr(JsonObject) object = NULL;
	g_autoptr(GError) closed = NULL;
	gint kind = choice(schedule, "kind");
	gboolean auto_post = flag(schedule, "auto-post");
	gint64 document_id = 0;
	const gchar *type_name = "invoice";
	gboolean ok;
	if (!postable(self, venture_entity_get_organization_id(schedule), at, &closed))
	{
		if (!dry_run && !write_occurrence(self, schedule, key, 1, NULL, 0, closed ? closed->message : "closed period", at, actor, error))
			return FALSE;
		return TRUE;
	}
	object = parse_template(schedule, error);
	if (object == NULL)
		return FALSE;
	if (dry_run)
	{
		(*created)++;
		return TRUE;
	}
	if (kind == 1)
	{
		type_name = "vendor_bill";
		ok = generate_bill(self, schedule, object, at, auto_post, &document_id, actor, error);
	}
	else if (kind == 2)
	{
		type_name = "expense";
		ok = generate_expense(self, schedule, object, at, key, &document_id, actor, error);
	}
	else if (kind == 3)
	{
		type_name = "journal";
		ok = generate_journal(self, schedule, object, at, key, auto_post, &document_id, actor, error);
	}
	else
		ok = generate_invoice(self, schedule, object, at, auto_post, &document_id, actor, error);
	if (!ok)
		return write_occurrence(self, schedule, key, 2, NULL, 0, error && *error ? (*error)->message : "failed", at, actor, error) && FALSE;
	if (!write_occurrence(self, schedule, key, 0, type_name, document_id, NULL, at, actor, error))
		return FALSE;
	(*created)++;
	return TRUE;
}

static gint
run_schedule(VentureRecurringService *self, VentureEntity *schedule, GDateTime *as_of,
	gboolean dry_run, const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(GDateTime) local_as_of = NULL;
	gint64 index;
	gint created = 0;
	gint frequency;
	guint n;
	if (flag(schedule, "paused"))
		return 0;
	g_object_get(schedule, "end-at", &end, "cycle-index", &index, NULL);
	start = schedule_start(schedule);
	frequency = choice(schedule, "frequency");
	if (start == NULL)
		return refuse(error, "A schedule needs a start date") ? -1 : -1;
	local_as_of = g_date_time_to_timezone(as_of, g_date_time_get_timezone(start));
	if (end != NULL)
	{
		GDateTime *local_end = g_date_time_to_timezone(end, g_date_time_get_timezone(start));
		g_date_time_unref(end);
		end = local_end;
	}
	for (n = 0; n < 1200; n++)
	{
		g_autoptr(GDateTime) at = cycle_date(start, frequency, index);
		g_autofree gchar *stamp = NULL;
		g_autofree gchar *key = NULL;
		g_autoptr(VentureEntity) existing = NULL;
		if (at == NULL || day_compare(at, local_as_of) > 0)
			break;
		if (end != NULL && day_compare(at, end) > 0)
			break;
		stamp = day_text(at);
		key = g_strdup_printf("%" G_GINT64_FORMAT ":%s", venture_entity_get_id(schedule), stamp);
		existing = find_occurrence(self, venture_entity_get_organization_id(schedule), key, error);
		if (error && *error)
			return -1;
		if (existing != NULL && choice(existing, "status") == 0)
		{
			if (!dry_run && !advance(self, schedule, index, at, NULL, actor, error))
				return -1;
			index++;
			continue;
		}
		if (existing != NULL && choice(existing, "status") == 1)
		{
			if (!dry_run && !advance(self, schedule, index, at, NULL, actor, error))
				return -1;
			index++;
			continue;
		}
		if (!generate_one(self, schedule, at, key, dry_run, &created, actor, error))
			return -1;
		if (!dry_run && !advance(self, schedule, index, at, NULL, actor, error))
			return -1;
		index++;
	}
	return created;
}

static void
get_recurring_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_RECURRING_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_recurring_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureRecurringService *self = VENTURE_RECURRING_SERVICE(object);
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
recurring_finalize(GObject *object)
{
	VentureRecurringService *self = VENTURE_RECURRING_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_recurring_service_parent_class)->finalize(object);
}

static void
venture_recurring_service_class_init(VentureRecurringServiceClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->get_property = get_recurring_property;
	object->set_property = set_recurring_property;
	object->finalize = recurring_finalize;
	g_object_class_install_property(object, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_recurring_service_init(VentureRecurringService *self)
{
	(void)self;
}

gboolean
venture_recurring_service_pause(VentureRecurringService *self, VentureEntity *schedule,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_RECURRING_SERVICE(self), FALSE);
	g_object_set(schedule, "paused", TRUE, NULL);
	return save_owned(self, schedule, actor, error);
}

gboolean
venture_recurring_service_resume(VentureRecurringService *self, VentureEntity *schedule,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_RECURRING_SERVICE(self), FALSE);
	g_object_set(schedule, "paused", FALSE, NULL);
	return save_owned(self, schedule, actor, error);
}

gint
venture_recurring_service_run(VentureRecurringService *self, gint64 organization_id,
	GDateTime *as_of, gboolean dry_run, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autofree gchar *as_of_text = NULL;
	g_autoptr(GString) effective_days = g_string_new(NULL);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) clock = NULL;
	gboolean can_post = FALSE;
	gint total = 0;
	guint i;
	g_return_val_if_fail(VENTURE_IS_RECURRING_SERVICE(self), -1);
	if (self->busy)
		return refuse(error, "A recurring sweep is already running") ? -1 : -1;
	if (organization_id <= 0)
		return refuse(error, "An organization is required") ? -1 : -1;
	/* An implicit sweep runs as of the business date (locale.timezone), so
	 * an occurrence dated the process zone's later calendar day waits for
	 * the next sweep instead of being posted and refused as future-dated. */
	clock = as_of ? g_date_time_ref(as_of) : venture_settlement_service_today(venture_settlement_service_get(self->database));
	as_of_text = as_of != NULL ? g_date_time_format_iso8601(clock) : g_date_time_format(clock, "%F");
	query = venture_query_new(VENTURE_TYPE_RECURRING_SCHEDULE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return -1;
	/* Draft-only schedules and read-only previews do not request posting consent. */
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *schedule = g_ptr_array_index(rows, i);
		if (as_of == NULL && !flag(schedule, "paused"))
		{
			g_autoptr(GDateTime) start = schedule_start(schedule);
			if (start != NULL)
			{
				g_autoptr(GDateTime) local = g_date_time_to_timezone(clock, g_date_time_get_timezone(start));
				g_autofree gchar *day = g_date_time_format(local, "%F");
				/* UTC midnight and each schedule's local midnight invalidate
				 * omitted-date consent without depending on retry seconds. */
				g_string_append_printf(effective_days, "%" G_GINT64_FORMAT ":%s;",
					venture_entity_get_id(schedule), day);
			}
		}
		if (!flag(schedule, "paused") && (choice(schedule, "kind") == 2 || flag(schedule, "auto-post")))
			can_post = TRUE;
	}
	if (!dry_run && can_post)
	{
		operation = venture_accounting_operation_begin(self->database, "recurring.run", NULL, NULL,
			g_variant_new("(ssb)", as_of_text, effective_days->str, dry_run), organization_id, actor, error);
		if (operation == NULL)
			return -1;
		/* The preflight determines whether posting is possible; execution
		 * must read the rows protected by the acquired transaction snapshot. */
		g_clear_pointer(&rows, g_ptr_array_unref);
		rows = venture_database_find(self->database, query, error);
		if (rows == NULL) return -1;
	}
	self->busy = TRUE;
	if (!dry_run && !venture_database_begin(self->database, error))
	{
		self->busy = FALSE;
		return -1;
	}
	for (i = 0; i < rows->len; i++)
	{
		gint n = run_schedule(self, g_ptr_array_index(rows, i), clock, dry_run, actor, error);
		if (n < 0)
		{
			if (!dry_run)
				venture_database_rollback(self->database);
			self->busy = FALSE;
			return -1;
		}
		total += n;
	}
	if (!dry_run && !venture_database_commit(self->database, error))
	{
		self->busy = FALSE;
		return -1;
	}
	self->busy = FALSE;
	if (operation != NULL && !venture_accounting_operation_finish(operation, error))
		return -1;
	return total;
}

static JsonArray *
documents_from_json(const gchar *payload, GError **error)
{
	g_autoptr(JsonNode) node = venture_json_parse(payload, error);
	JsonNode *docs;
	if (node == NULL)
		return NULL;
	if (JSON_NODE_HOLDS_ARRAY(node))
		return json_array_ref(json_node_get_array(node));
	if (!JSON_NODE_HOLDS_OBJECT(node))
	{
		refuse(error, "Batch JSON must be an object with documents or an array");
		return NULL;
	}
	docs = json_object_get_member(json_node_get_object(node), "documents");
	if (docs == NULL || !JSON_NODE_HOLDS_ARRAY(docs))
	{
		refuse(error, "Batch JSON needs a documents array");
		return NULL;
	}
	return json_array_ref(json_node_get_array(docs));
}

static gchar **
split_csv_line(const gchar *line, guint *count, GError **error)
{
	GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
	GString *cur = g_string_new(NULL);
	gboolean quoted = FALSE;
	const gchar *p;
	for (p = line; *p; p++)
	{
		if (*p == '"')
		{
			if (quoted && p[1] == '"')
			{
				g_string_append_c(cur, '"');
				p++;
			}
			else
				quoted = !quoted;
		}
		else if (*p == ',' && !quoted)
		{
			g_ptr_array_add(parts, g_string_free(cur, FALSE));
			cur = g_string_new(NULL);
		}
		else if (*p != '\r')
			g_string_append_c(cur, *p);
	}
	if (quoted)
	{
		g_string_free(cur, TRUE);
		g_ptr_array_unref(parts);
		refuse(error, "CSV has an unterminated quoted field; multiline fields are not supported");
		return NULL;
	}
	g_ptr_array_add(parts, g_string_free(cur, FALSE));
	*count = parts->len;
	g_ptr_array_add(parts, NULL);
	return (gchar **)g_ptr_array_free(parts, FALSE);
}

static JsonArray *
documents_from_csv(const gchar *payload, const gchar *kind, GError **error)
{
	g_auto(GStrv) lines = g_strsplit(payload, "\n", 0);
	g_auto(GStrv) headers = NULL;
	JsonArray *docs = json_array_new();
	GHashTable *invoices = g_hash_table_new(g_str_hash, g_str_equal);
	guint header_n = 0;
	guint i;
	if (lines == NULL || lines[0] == NULL)
	{
		json_array_unref(docs);
		g_hash_table_unref(invoices);
		refuse(error, "CSV needs a header row");
		return NULL;
	}
	headers = split_csv_line(lines[0], &header_n, error);
	if (headers == NULL)
		goto invalid;
	for (i = 0; i < header_n; i++)
	{
		guint j;
		if (venture_string_is_empty(headers[i]))
		{
			refuse(error, "CSV headers must be nonempty");
			goto invalid;
		}
		for (j = 0; j < i; j++)
			if (g_strcmp0(headers[i], headers[j]) == 0)
			{
				refuse(error, "CSV headers must be unique");
				goto invalid;
			}
	}
	for (i = 1; lines[i] != NULL; i++)
	{
		g_auto(GStrv) cols = NULL;
		g_autoptr(JsonObject) object = json_object_new();
		g_autoptr(JsonObject) line = json_object_new();
		guint n = 0;
		guint c;
		const gchar *number = NULL;
		if (venture_string_is_empty(lines[i]) || lines[i][0] == '\0')
			continue;
		cols = split_csv_line(lines[i], &n, error);
		if (cols == NULL)
			goto invalid;
		if (n != header_n)
		{
			refuse(error, "CSV rows must have the same field count as the header");
			goto invalid;
		}
		for (c = 0; c < header_n && c < n; c++)
		{
			json_object_set_string_member(object, headers[c], cols[c]);
			if (g_strcmp0(headers[c], "description") == 0 || g_strcmp0(headers[c], "quantity") == 0 ||
				g_strcmp0(headers[c], "unit_price") == 0)
				json_object_set_string_member(line, headers[c], cols[c]);
			if (g_strcmp0(headers[c], "number") == 0)
				number = cols[c];
		}
		if (g_strcmp0(kind, "invoice") == 0 && !venture_string_is_empty(number) &&
			g_hash_table_contains(invoices, number))
		{
			JsonObject *existing = g_hash_table_lookup(invoices, number);
			JsonNode *lines_node = json_object_get_member(existing, "lines");
			/* Group only rows describing the same invoice header. Otherwise a
			 * second customer's line could be billed to the first customer. */
			for (c = 0; c < header_n; c++)
			{
				if (g_strcmp0(headers[c], "description") == 0 || g_strcmp0(headers[c], "quantity") == 0 ||
					g_strcmp0(headers[c], "unit_price") == 0)
					continue;
				if (g_strcmp0(venture_json_object_get_string(existing, headers[c], NULL), cols[c]) != 0)
				{
					refuse(error, "CSV rows sharing an invoice number must agree on header fields");
					goto invalid;
				}
			}
			json_array_add_object_element(json_node_get_array(lines_node), json_object_ref(line));
			continue;
		}
		if (g_strcmp0(kind, "invoice") == 0)
		{
			JsonArray *invoice_lines = json_array_new();
			json_array_add_object_element(invoice_lines, json_object_ref(line));
			json_object_set_array_member(object, "lines", invoice_lines);
			if (!venture_string_is_empty(number))
				g_hash_table_insert(invoices, (gpointer)json_object_get_string_member(object, "number"), object);
		}
		json_array_add_object_element(docs, json_object_ref(object));
	}
	g_hash_table_unref(invoices);
	return docs;
invalid:
	g_hash_table_unref(invoices);
	json_array_unref(docs);
	return NULL;
}

static gboolean
seen_value(GHashTable *seen, const gchar *value, GError **error, const gchar *message)
{
	if (venture_string_is_empty(value))
		return TRUE;
	if (g_hash_table_contains(seen, value))
		return refuse(error, message);
	g_hash_table_add(seen, g_strdup(value));
	return TRUE;
}

static gboolean
existing_string(VentureDatabase *db, GType type, gint64 org, const gchar *field, const gchar *value)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) row = NULL;
	if (venture_string_is_empty(value))
		return FALSE;
	query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, NULL);
	row = venture_database_find_one(db, query, NULL);
	return row != NULL;
}

static gboolean
create_batch_invoice(VentureRecurringService *self, JsonObject *object, gint64 org, gboolean post,
	JsonArray *created, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = new_record(VENTURE_TYPE_INVOICE, org);
	g_autoptr(GDateTime) at = NULL;
	const gchar *issued;
	gint64 id;
	if (!apply_object(invoice, object, error))
		return FALSE;
	if (number(invoice, "company-id") <= 0)
		return refuse(error, "Each invoice needs company_id");
	if (empty_text(invoice, "number"))
		return refuse(error, "Each invoice needs a number");
	issued = venture_json_object_get_string(object, "issued_at", NULL);
	if (!venture_string_is_empty(issued))
	{
		at = venture_time_from_string(issued, error);
		if (at == NULL)
			return FALSE;
		/* Preserve explicit payment terms from the batch document. */
		g_object_set(invoice, "issued-at", at, NULL);
		if (!json_object_has_member(object, "due_at"))
			g_object_set(invoice, "due-at", at, NULL);
	}
	{
		g_autofree gchar *n = NULL;
		g_object_get(invoice, "number", &n, NULL);
		if (existing_string(self->database, VENTURE_TYPE_INVOICE, org, "number", n))
			return refuse(error, "Duplicate invoice number");
	}
	if (!venture_database_save(self->database, invoice, actor, error) ||
		!add_invoice_lines(self, org, venture_entity_get_id(invoice), template_lines(object), actor, error))
		return FALSE;
	if (post)
	{
		g_autoptr(GDateTime) when = NULL;
		g_object_get(invoice, "issued-at", &when, NULL);
		if (when == NULL)
			when = venture_time_now();
		if (!venture_settlement_service_transition(venture_settlement_service_get(self->database),
			VENTURE_INVOICE(invoice), "sent", when, actor, error))
			return FALSE;
	}
	id = venture_entity_get_id(invoice);
	json_array_add_int_element(created, id);
	return TRUE;
}

static gboolean
create_batch_expense(VentureRecurringService *self, JsonObject *object, gint64 org,
	JsonArray *created, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) expense = new_record(VENTURE_TYPE_EXPENSE, org);
	g_autofree gchar *external = NULL;
	if (!apply_object(expense, object, error))
		return FALSE;
	g_object_get(expense, "external-id", &external, NULL);
	if (!venture_string_is_empty(external) && existing_string(self->database, VENTURE_TYPE_EXPENSE, org, "external-id", external))
		return refuse(error, "Duplicate expense external_id");
	if (!venture_database_save(self->database, expense, actor, error))
		return FALSE;
	json_array_add_int_element(created, venture_entity_get_id(expense));
	return TRUE;
}

static JsonNode *
batch_for_organization(VentureRecurringService *self, gint64 org, const gchar *kind,
	const gchar *format, const gchar *payload, gboolean post, gboolean dry_run,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(JsonArray) docs = NULL;
	g_autoptr(JsonArray) created = json_array_new();
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	JsonBuilder *builder;
	JsonNode *result;
	guint i;
	g_return_val_if_fail(VENTURE_IS_RECURRING_SERVICE(self), NULL);
	if (venture_string_is_empty(payload))
	{
		refuse(error, "A batch payload is required");
		return NULL;
	}
	if (g_strcmp0(kind, "invoice") != 0 && g_strcmp0(kind, "expense") != 0)
	{
		refuse(error, "Batch kind must be invoice or expense");
		return NULL;
	}
	docs = (g_strcmp0(format, "csv") == 0) ? documents_from_csv(payload, kind, error) : documents_from_json(payload, error);
	if (docs == NULL)
		return NULL;
	if (org <= 0)
	{
		refuse(error, "A batch requires an organization");
		return NULL;
	}
	/* Generated invoice IDs do not exist at approval time; consent binds the payload. */
	if (post || g_strcmp0(kind, "expense") == 0)
	{
		operation = venture_accounting_operation_begin(self->database, "batch.create", NULL, NULL,
			g_variant_new("(sssbb)", kind, format ? format : "json", payload, post, dry_run), org, actor, error);
		if (operation == NULL)
			return NULL;
	}
	if (!venture_database_begin(self->database, error))
		return NULL;
	for (i = 0; i < json_array_get_length(docs); i++)
	{
		JsonNode *item = json_array_get_element(docs, i);
		JsonObject *object;
		const gchar *identity;
		gboolean ok;
		if (!JSON_NODE_HOLDS_OBJECT(item))
		{
			refuse(error, "Every batch document must be an object");
			venture_database_rollback(self->database);
			return NULL;
		}
		object = json_node_get_object(item);
		if (g_strcmp0(kind, "invoice") == 0)
		{
			identity = venture_json_object_get_string(object, "number", NULL);
			if (!seen_value(seen, identity, error, "Duplicate invoice number in payload"))
			{
				venture_database_rollback(self->database);
				return NULL;
			}
			ok = create_batch_invoice(self, object, org, post, created, actor, error);
		}
		else
		{
			identity = venture_json_object_get_string(object, "external_id", NULL);
			if (!seen_value(seen, identity, error, "Duplicate expense external_id in payload"))
			{
				venture_database_rollback(self->database);
				return NULL;
			}
			ok = create_batch_expense(self, object, org, created, actor, error);
		}
		if (!ok)
		{
			venture_database_rollback(self->database);
			return NULL;
		}
	}
	if (dry_run)
		venture_database_rollback(self->database);
	else if (!venture_database_commit(self->database, error))
		return NULL;
	/* A dry run abandons the whole nested transaction, including consent use. */
	if (!dry_run && operation != NULL && !venture_accounting_operation_finish(operation, error))
		return NULL;
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "created");
	json_builder_add_int_value(builder, (gint64)json_array_get_length(created));
	json_builder_set_member_name(builder, "dry_run");
	json_builder_add_boolean_value(builder, dry_run);
	json_builder_end_object(builder);
	result = json_builder_get_root(builder);
	g_object_unref(builder);
	json_object_set_array_member(json_node_get_object(result), "ids", json_array_ref(created));
	return result;
}

/* Legacy direct callers use the same explicitly flagged default as context. */
static gint64
default_organization(VentureRecurringService *self)
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
	return organization != NULL ? venture_entity_get_id(organization) : 0;
}

JsonNode *
venture_recurring_service_batch(VentureRecurringService *self, const gchar *kind,
	const gchar *format, const gchar *payload, gboolean post, gboolean dry_run,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_RECURRING_SERVICE(self), NULL);
	return batch_for_organization(self, default_organization(self), kind, format,
		payload, post, dry_run, actor, error);
}

static void
get_collection_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_COLLECTION_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_collection_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureCollectionService *self = VENTURE_COLLECTION_SERVICE(object);
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
collection_finalize(GObject *object)
{
	VentureCollectionService *self = VENTURE_COLLECTION_SERVICE(object);
	g_weak_ref_clear(&self->context);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_collection_service_parent_class)->finalize(object);
}

static void
venture_collection_service_class_init(VentureCollectionServiceClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->get_property = get_collection_property;
	object->set_property = set_collection_property;
	object->finalize = collection_finalize;
	g_object_class_install_property(object, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_collection_service_init(VentureCollectionService *self)
{
	g_weak_ref_init(&self->context, NULL);
}

void
venture_collection_service_set_context(VentureCollectionService *self, VentureContext *context)
{
	g_return_if_fail(VENTURE_IS_COLLECTION_SERVICE(self));
	g_weak_ref_set(&self->context, context);
}

static gint
days_between(GDateTime *later, GDateTime *earlier)
{
	return (gint)(g_date_time_difference(later, earlier) / G_TIME_SPAN_DAY);
}

static VentureEntity *
find_notice(VentureDatabase *db, gint64 org, const gchar *key)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COLLECTION_NOTICE);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_string(query, "occurrence-key", VENTURE_FILTER_OP_EQ, key, NULL);
	return venture_database_find_one(db, query, NULL);
}

static VentureEntity *
ensure_case(VentureCollectionService *self, gint64 org, gint64 invoice_id, gint64 policy_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COLLECTION_CASE);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_set_organization(query, org);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, NULL);
	row = venture_database_find_one(self->database, query, error);
	if (error && *error)
		return NULL;
	if (row != NULL)
		return g_steal_pointer(&row);
	row = new_record(VENTURE_TYPE_COLLECTION_CASE, org);
	g_object_set(row, "invoice-id", invoice_id, "policy-id", policy_id, "status", 0, NULL);
	if (!venture_database_save(self->database, row, actor, error))
		return NULL;
	return g_steal_pointer(&row);
}

static gboolean
stopped(VentureCollectionService *self, VentureEntity *invoice, VentureEntity *kase, GDateTime *as_of)
{
	g_autoptr(VentureMoney) balance = NULL;
	g_autofree gchar *workflow = NULL;
	gint status;
	g_object_get(invoice, "status", &status, "workflow-state", &workflow, NULL);
	/* Drafts are internal proposals, not customer obligations to collect. */
	if (status != VENTURE_INVOICE_STATUS_SENT && status != VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
		return TRUE;
	if (g_strcmp0(workflow, "disputed") == 0)
		return TRUE;
	if (kase != NULL)
	{
		g_autoptr(GDateTime) promised = NULL;
		gint case_status = choice(kase, "status");
		if (case_status == 1 || case_status == 2)
			return TRUE;
		g_object_get(kase, "promised-at", &promised, NULL);
		if (promised != NULL && day_compare(promised, as_of) > 0)
			return TRUE;
	}
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database),
		venture_entity_get_id(invoice), NULL, NULL);
	return balance == NULL || venture_money_is_zero(balance) || venture_money_get_amount(balance) <= 0;
}

static gboolean
queue_notice(VentureCollectionService *self, VentureContext *context, VentureEntity *invoice,
	VentureEntity *policy, VentureEntity *step, VentureEntity *kase, gint action, const gchar *key,
	GDateTime *as_of, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) notice = NULL;
	g_autofree gchar *email = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *subject = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(VentureContext) reporting_context = NULL;
	gint64 org = venture_entity_get_organization_id(invoice);
	gint64 company_id = 0;
	existing = find_notice(self->database, org, key);
	if (existing != NULL)
		return TRUE;
	g_object_get(invoice, "company-id", &company_id, "number", &number, NULL);
	company = venture_database_get(self->database, VENTURE_TYPE_COMPANY, company_id, error);
	if (company == NULL)
		return FALSE;
	g_object_get(company, "email", &email, NULL);
	notice = new_record(VENTURE_TYPE_COLLECTION_NOTICE, org);
	g_object_set(notice, "invoice-id", venture_entity_get_id(invoice), "policy-id", policy ? venture_entity_get_id(policy) : 0,
		"step-id", step ? venture_entity_get_id(step) : 0, "case-id", kase ? venture_entity_get_id(kase) : 0,
		"customer-id", company_id, "occurrence-key", key, "action", action, "queued-at", as_of, NULL);
	if (venture_string_is_empty(email))
	{
		g_object_set(notice, "last-error", "Customer has no email", NULL);
		return venture_database_save(self->database, notice, actor, error);
	}
	subject = g_strdup_printf("%s %s", action == 1 ? "Statement" : action == 2 ? "Escalation" : "Reminder", number ? number : "");
	if (action == 1)
	{
		VentureReport *report;
		g_autoptr(JsonObject) options = json_object_new();
		g_autoptr(VentureReportResult) statement = NULL;
		g_autoptr(VentureMoney) balance = NULL;
		g_autofree gchar *as_of_text = g_date_time_format_iso8601(as_of);
		g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, company_id);
		reporting_context = context != NULL ? g_object_ref(context) : g_weak_ref_get(&self->context);
		if (reporting_context == NULL)
			return refuse(error, "Statement delivery requires a reporting context");
		report = venture_report_registry_lookup(venture_context_get_report_registry(reporting_context), "customer_statement");
		if (report == NULL)
			return refuse(error, "Customer statements are unavailable");
		json_object_set_string_member(options, "customer_id", id_text);
		json_object_set_int_member(options, "organization_id", org);
		json_object_set_string_member(options, "as_of", as_of_text);
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database),
			venture_entity_get_id(invoice), NULL, error);
		if (balance == NULL)
			return FALSE;
		json_object_set_string_member(options, "currency", venture_money_get_currency(balance));
		statement = venture_report_generate(report, reporting_context, NULL, options, error);
		if (statement == NULL)
			return FALSE;
		/* Use the report's canonical renderer instead of an empty cover letter. */
		body = venture_report_result_render(statement, VENTURE_OUTPUT_FORMAT_ORG);
	}
	else
		body = g_strdup_printf("Invoice %s is overdue. Please arrange payment.", number ? number : "");
	message = venture_mail_message_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(message), org);
	g_object_set(message, "to", email, "subject", subject, "text-body", body, "html-body", "",
		"idempotency-key", key, "related-type", "invoice", "related-id", venture_entity_get_id(invoice), NULL);
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(self->database), message, actor, error);
	if (queued == NULL)
	{
		g_object_set(notice, "last-error", error && *error ? (*error)->message : "enqueue failed", NULL);
		g_clear_error(error);
		return venture_database_save(self->database, notice, actor, error);
	}
	g_object_set(notice, "mail-message-id", venture_entity_get_id(VENTURE_ENTITY(queued)), NULL);
	if (kase != NULL)
		g_object_set(kase, "last-notice-at", as_of, NULL);
	if (kase != NULL && !venture_database_save(self->database, kase, actor, error))
		return FALSE;
	return venture_database_save(self->database, notice, actor, error);
}

static gint
run_policy_invoices(VentureCollectionService *self, VentureContext *context, VentureEntity *policy,
	GDateTime *as_of, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) invoices = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(VentureQuery) steps_query = venture_query_new(VENTURE_TYPE_COLLECTION_STEP);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) steps = NULL;
	gint queued = 0;
	guint i;
	guint s;
	gint64 org = venture_entity_get_organization_id(policy);
	venture_query_set_organization(invoices, org);
	rows = venture_database_find(self->database, invoices, error);
	if (rows == NULL)
		return -1;
	venture_query_set_organization(steps_query, org);
	venture_query_add_filter_int(steps_query, "policy-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(policy), NULL);
	steps = venture_database_find(self->database, steps_query, error);
	if (steps == NULL)
		return -1;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *invoice = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) due = NULL;
		g_autoptr(VentureEntity) kase = NULL;
		gint days;
		g_object_get(invoice, "due-at", &due, NULL);
		if (due == NULL)
			continue;
		kase = ensure_case(self, org, venture_entity_get_id(invoice), venture_entity_get_id(policy), actor, error);
		if (kase == NULL)
			return -1;
		if (stopped(self, invoice, kase, as_of))
			continue;
		days = days_between(as_of, due);
		for (s = 0; s < steps->len; s++)
		{
			VentureEntity *step = g_ptr_array_index(steps, s);
			gint64 offset;
			gint action;
			g_autofree gchar *key = NULL;
			if (!flag(step, "active"))
				continue;
			g_object_get(step, "day-offset", &offset, "action", &action, NULL);
			/* Offsets share the same threshold on either side of the due date. */
			if (days < offset)
				continue;
			key = g_strdup_printf("inv:%" G_GINT64_FORMAT ":step:%" G_GINT64_FORMAT,
				venture_entity_get_id(invoice), venture_entity_get_id(step));
			{
				g_autoptr(VentureEntity) before = find_notice(self->database, org, key);
				if (!queue_notice(self, context, invoice, policy, step, kase, action, key, as_of, actor, error))
					return -1;
				if (before == NULL)
					queued++;
			}
		}
	}
	if (number(policy, "statement-day") > 0 && g_date_time_get_day_of_month(as_of) >= number(policy, "statement-day"))
	{
		g_autofree gchar *month = g_date_time_format(as_of, "%Y-%m");
		GHashTable *customers = g_hash_table_new(g_direct_hash, g_direct_equal);
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *invoice = g_ptr_array_index(rows, i);
			gint64 customer = number(invoice, "company-id");
			g_autofree gchar *key = NULL;
			g_autoptr(VentureEntity) kase = NULL;
			if (customer <= 0 || g_hash_table_contains(customers, GINT_TO_POINTER(customer)))
				continue;
			kase = ensure_case(self, org, venture_entity_get_id(invoice), venture_entity_get_id(policy), actor, error);
			if (kase == NULL)
			{
				g_hash_table_unref(customers);
				return -1;
			}
			if (stopped(self, invoice, kase, as_of))
				continue;
			/* A paid first invoice must not hide this customer's open invoices. */
			g_hash_table_add(customers, GINT_TO_POINTER(customer));
			key = g_strdup_printf("stmt:%" G_GINT64_FORMAT ":%s", customer, month);
			{
				g_autoptr(VentureEntity) before = find_notice(self->database, org, key);
				if (!queue_notice(self, context, invoice, policy, NULL, kase, 1, key, as_of, actor, error))
				{
					g_hash_table_unref(customers);
					return -1;
				}
				if (before == NULL)
					queued++;
			}
		}
		g_hash_table_unref(customers);
	}
	return queued;
}

gint
venture_collection_service_run(VentureCollectionService *self, VentureContext *context,
	gint64 organization_id, GDateTime *as_of, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) policies = NULL;
	g_autoptr(GDateTime) clock = NULL;
	gint total = 0;
	guint i;
	g_return_val_if_fail(VENTURE_IS_COLLECTION_SERVICE(self), -1);
	if (organization_id <= 0)
		return refuse(error, "An organization is required") ? -1 : -1;
	clock = as_of ? g_date_time_ref(as_of) : venture_time_now();
	query = venture_query_new(VENTURE_TYPE_COLLECTION_POLICY);
	venture_query_set_organization(query, organization_id);
	policies = venture_database_find(self->database, query, error);
	if (policies == NULL)
		return -1;
	if (!venture_database_begin(self->database, error))
		return -1;
	for (i = 0; i < policies->len; i++)
	{
		VentureEntity *policy = g_ptr_array_index(policies, i);
		gint n;
		if (!flag(policy, "active"))
			continue;
		n = run_policy_invoices(self, context, policy, clock, actor, error);
		if (n < 0)
		{
			venture_database_rollback(self->database);
			return -1;
		}
		total += n;
	}
	if (!venture_database_commit(self->database, error))
		return -1;
	return total;
}

static VentureReportResult *
worklist_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Collections worklist", period);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gint64 org = options ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	guint i;
	(void)error;
	if (org <= 0)
		org = venture_context_get_default_organization_id(context);
	venture_query_set_organization(query, org);
	rows = venture_database_find(db, query, error);
	if (rows == NULL)
		return NULL;
	venture_report_result_add_column(result, "invoice", "Invoice", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "owner", "Owner", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "due", "Due", VENTURE_REPORT_COLUMN_DATE);
	venture_report_result_add_column(result, "promised", "Promised", VENTURE_REPORT_COLUMN_DATE);
	venture_report_result_add_column(result, "notes", "Dispute notes", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *invoice = g_ptr_array_index(rows, i);
		g_autoptr(VentureMoney) balance = NULL;
		g_autoptr(GDateTime) due = NULL;
		g_autoptr(VentureQuery) cases = venture_query_new(VENTURE_TYPE_COLLECTION_CASE);
		g_autoptr(VentureEntity) kase = NULL;
		g_autofree gchar *number = NULL;
		g_autofree gchar *owner = NULL;
		g_autofree gchar *notes = NULL;
		g_autoptr(GDateTime) promised = NULL;
		gint status;
		g_object_get(invoice, "status", &status, "due-at", &due, "number", &number, NULL);
		if (status == VENTURE_INVOICE_STATUS_DRAFT || status == VENTURE_INVOICE_STATUS_PAID ||
			status == VENTURE_INVOICE_STATUS_VOID || due == NULL || day_compare(due, now) >= 0)
			continue;
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(db),
			venture_entity_get_id(invoice), NULL, NULL);
		if (balance == NULL || venture_money_get_amount(balance) <= 0)
			continue;
		venture_query_set_organization(cases, org);
		venture_query_add_filter_int(cases, "invoice-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(invoice), NULL);
		kase = venture_database_find_one(db, cases, NULL);
		if (kase != NULL)
			g_object_get(kase, "owner", &owner, "dispute-notes", &notes, "promised-at", &promised, NULL);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "invoice", number);
		venture_report_result_set_text(result, "owner", owner);
		venture_report_result_set_text(result, "notes", notes);
	}
	return g_steal_pointer(&result);
}

void
venture_recurring_register_reports(VentureReportRegistry *registry)
{
	VentureReport *worklist = VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "collections_worklist", "Collections worklist",
		"Overdue invoices with owner, promised payment and dispute notes.", worklist_report));
	venture_report_registry_add(registry, worklist);
}

static gboolean
schedule_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	(void)actor;
	g_object_get(action, "name", &name, NULL);
	if (g_strcmp0(name, "run") == 0)
		return TRUE;
	if (!VENTURE_IS_RECURRING_SCHEDULE(entity) || venture_entity_get_id(entity) <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A saved schedule is required");
		return FALSE;
	}
	return TRUE;
}

static VentureEntity *
schedule_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureRecurringService *self = venture_action_get_data(action);
	g_autofree gchar *name = NULL;
	const gchar *as_of_text = param_string(params, "as_of");
	g_autoptr(GDateTime) as_of = !venture_string_is_empty(as_of_text) ?
		venture_time_from_string(as_of_text, error) : NULL;
	gint64 org;
	g_object_get(action, "name", &name, NULL);
	if (!venture_string_is_empty(as_of_text) && as_of == NULL)
		return NULL;
	if (g_strcmp0(name, "pause") == 0)
		return venture_recurring_service_pause(self, entity, actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "resume") == 0)
		return venture_recurring_service_resume(self, entity, actor, error) ? g_object_ref(entity) : NULL;
	/* The organization the access policy judged, which prepare_target set
	 * from organization_id; the parameter and the default only for callers
	 * that ran without a prepared subject. */
	org = venture_entity_get_organization_id(entity);
	if (org <= 0)
		org = param_id(params, "organization_id");
	if (org <= 0)
		org = default_organization(self);
	if (venture_recurring_service_run(self, org, as_of, param_bool(params, "dry_run"), actor, error) < 0)
		return NULL;
	return entity ? g_object_ref(entity) : g_object_new(VENTURE_TYPE_RECURRING_SCHEDULE, NULL);
}

static gboolean
always_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	(void)error;
	return TRUE;
}

static VentureEntity *
policy_run(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureCollectionService *self = venture_action_get_data(action);
	g_autoptr(GDateTime) as_of = param_date(params, "as_of", error);
	gint64 org = entity != NULL ? venture_entity_get_organization_id(entity) : 0;
	if (as_of == NULL)
		return NULL;
	if (org <= 0)
		org = param_id(params, "organization_id");
	if (org <= 0)
		org = default_organization(venture_recurring_service_get(self->database));
	if (venture_collection_service_run(self, NULL, org, as_of, actor, error) < 0)
		return NULL;
	return entity ? g_object_ref(entity) : g_object_new(VENTURE_TYPE_COLLECTION_POLICY, NULL);
}

static VentureEntity *
batch_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureRecurringService *self = venture_action_get_data(action);
	g_autoptr(VentureAccountingOperation) operation = NULL;
	gboolean saved = VENTURE_IS_FINANCIAL_BATCH(entity);
	gboolean dry_run = param_bool(params, "dry_run");
	gboolean post;
	g_autofree gchar *type_name = NULL;
	g_autofree gchar *stored = NULL;
	g_autofree gchar *stored_format = NULL;
	gboolean stored_post = FALSE;
	/* A type-level batch runs where the access policy placed its subject. */
	gint64 org = entity != NULL ? venture_entity_get_organization_id(entity) : 0;
	const gchar *kind = param_string(params, "kind");
	const gchar *format = param_string(params, "format");
	const gchar *payload = param_string(params, "payload");
	g_autoptr(JsonNode) result = NULL;
	g_object_get(action, "type-name", &type_name, NULL);
	if (g_strcmp0(type_name, "expense") == 0)
		kind = "expense";
	else if (g_strcmp0(type_name, "invoice") == 0)
		kind = "invoice";
	if (VENTURE_IS_FINANCIAL_BATCH(entity))
	{
		gint stored_kind = 0;
		/* Keep borrowed views alive through the import; every g_object_get
		 * output must point at storage of the property's actual type. */
		g_object_get(entity, "payload", &stored, "format", &stored_format, "kind", &stored_kind, "auto-post", &stored_post, NULL);
		if (payload == NULL)
			payload = stored;
		if (kind == NULL)
			kind = stored_kind == 1 ? "expense" : "invoice";
		if (format == NULL)
			format = stored_format;
		org = venture_entity_get_organization_id(entity);
	}
	if (org <= 0)
		org = param_id(params, "organization_id");
	if (org <= 0)
		org = default_organization(self);
	if (format == NULL)
		format = "json";
	post = param_node(params, "post") != NULL ? param_bool(params, "post") : stored_post;
	if (saved && (post || g_strcmp0(kind, "expense") == 0))
	{
		operation = venture_accounting_operation_begin(self->database, "batch.apply", entity, NULL,
			g_variant_new("(sssbb)", kind ? kind : "invoice", format, payload ? payload : "", post, dry_run), org, actor, error);
		if (operation == NULL) return NULL;
	}
	if (saved && !venture_database_begin(self->database, error)) return NULL;
	result = batch_for_organization(self, org, kind ? kind : "invoice", format, payload,
		param_node(params, "post") != NULL ? param_bool(params, "post") : stored_post,
		param_bool(params, "dry_run"), actor, error);
	if (result == NULL)
	{
		if (saved) venture_database_rollback(self->database);
		return NULL;
	}
	if (VENTURE_IS_FINANCIAL_BATCH(entity))
	{
		g_autofree gchar *text = venture_json_to_string(result, FALSE);
		if (dry_run)
		{
			venture_database_rollback(self->database);
			return g_object_ref(entity);
		}
		g_object_set(entity, "last-result", text, NULL);
		if (!venture_database_save(self->database, entity, actor, error))
		{
			venture_database_rollback(self->database);
			return NULL;
		}
		if (!venture_database_commit(self->database, error)) return NULL;
		if (operation != NULL && !venture_accounting_operation_finish(operation, error)) return NULL;
		return g_object_ref(entity);
	}
	return g_object_new(g_strcmp0(kind, "expense") == 0 ? VENTURE_TYPE_EXPENSE : VENTURE_TYPE_INVOICE, NULL);
}

static void
register_one(VentureActionRegistry *registry, const gchar *type_name, const gchar *name, const gchar *label,
	gboolean type_level, GPtrArray *parameters, VentureActionAllowed allowed, VentureActionInvoke invoke,
	gpointer data)
{
	g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", type_name, "name", name,
		"label", label, "description", label, "parameters", parameters, "stageable", TRUE,
		"type-level", type_level, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(GError) error = NULL;
	if (!venture_action_registry_register(registry, action, allowed, invoke, data, NULL, &error))
		g_error("Recurring action registration: %s", error->message);
}

void
venture_recurring_register_actions(VentureDatabase *database)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(database);
	VentureRecurringService *recurring = venture_recurring_service_get(database);
	VentureCollectionService *collections = venture_collection_service_get(database);
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	if (recurring->actions)
		return;
	recurring->actions = TRUE;
	g_ptr_array_add(parameters, venture_field_spec_new("as_of", "As of", VENTURE_FIELD_KIND_DATETIME));
	g_ptr_array_add(parameters, venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER));
	g_ptr_array_add(parameters, venture_field_spec_new("dry_run", "Dry run", VENTURE_FIELD_KIND_BOOLEAN));
	register_one(registry, "recurring_schedule", "pause", "Pause", FALSE, NULL, schedule_allowed, schedule_invoke, recurring);
	register_one(registry, "recurring_schedule", "resume", "Resume", FALSE, NULL, schedule_allowed, schedule_invoke, recurring);
	register_one(registry, "recurring_schedule", "run", "Run due", TRUE, parameters, schedule_allowed, schedule_invoke, recurring);
	g_ptr_array_set_size(parameters, 0);
	g_ptr_array_add(parameters, venture_field_spec_new("as_of", "As of", VENTURE_FIELD_KIND_DATETIME));
	g_ptr_array_add(parameters, venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER));
	register_one(registry, "collection_policy", "run", "Run reminders", TRUE, parameters, always_allowed, policy_run, collections);
	g_ptr_array_set_size(parameters, 0);
	g_ptr_array_add(parameters, venture_field_spec_new("payload", "Payload", VENTURE_FIELD_KIND_TEXT));
	g_ptr_array_add(parameters, venture_field_spec_new("format", "Format", VENTURE_FIELD_KIND_STRING));
	g_ptr_array_add(parameters, venture_field_spec_new("kind", "Kind", VENTURE_FIELD_KIND_STRING));
	g_ptr_array_add(parameters, venture_field_spec_new("post", "Post", VENTURE_FIELD_KIND_BOOLEAN));
	g_ptr_array_add(parameters, venture_field_spec_new("dry_run", "Dry run", VENTURE_FIELD_KIND_BOOLEAN));
	register_one(registry, "financial_batch", "apply", "Apply", FALSE, parameters, always_allowed, batch_invoke, recurring);
	/* A saved batch already belongs to an organization; a type-level one
	 * names it, or an organization member could never create one. */
	g_ptr_array_add(parameters, venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER));
	register_one(registry, "invoice", "batch_create", "Batch create", TRUE, parameters, always_allowed, batch_invoke, recurring);
	register_one(registry, "expense", "batch_create", "Batch create", TRUE, parameters, always_allowed, batch_invoke, recurring);
}

static void
install_recurring(VentureRecurringService *self)
{
	if (self->database == NULL)
		return;
	venture_database_add_save_validator(self->database, VENTURE_TYPE_RECURRING_SCHEDULE, schedule_validate, self, NULL);
	venture_database_add_save_validator(self->database, VENTURE_TYPE_RECURRING_OCCURRENCE, occurrence_validate, self, NULL);
	venture_recurring_register_actions(self->database);
}

VentureRecurringService *
venture_recurring_service_get(VentureDatabase *database)
{
	VentureRecurringService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-recurring-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_RECURRING_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-recurring-service", self, g_object_unref);
		install_recurring(self);
	}
	return self;
}

VentureCollectionService *
venture_collection_service_get(VentureDatabase *database)
{
	VentureCollectionService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-collection-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_COLLECTION_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-collection-service", self, g_object_unref);
	}
	venture_recurring_service_get(database);
	return self;
}
