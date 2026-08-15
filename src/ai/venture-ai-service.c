/*
 * venture-ai-service.c - AI as a participant, with bounded authority
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <ai-glib.h>

#include <string.h>

/* ==========================================================================
 * Confirmations
 * ========================================================================== */

struct _VentureAiConfirmation
{
	GObject parent_instance;

	gchar				*id;
	gchar				*summary;
	gchar				*prompt;
	gchar				*tool_name;
	JsonNode			*diff;
	VentureConfirmationState	 state;
	GDateTime			*created_at;
	GDateTime			*expires_at;

	/* The record with the change already applied in memory, held until a
	 * decision is made. Staging the object rather than the instruction
	 * means approval cannot re-interpret the request differently from
	 * what the diff showed. */
	VentureEntity			*staged;
	gboolean			 is_delete;
};

G_DEFINE_FINAL_TYPE(VentureAiConfirmation, venture_ai_confirmation, G_TYPE_OBJECT)

static void
venture_ai_confirmation_finalize(GObject *object)
{
	VentureAiConfirmation *self;

	self = VENTURE_AI_CONFIRMATION(object);

	g_clear_pointer(&self->id, g_free);
	g_clear_pointer(&self->summary, g_free);
	g_clear_pointer(&self->prompt, g_free);
	g_clear_pointer(&self->tool_name, g_free);
	g_clear_pointer(&self->diff, json_node_unref);
	g_clear_pointer(&self->created_at, g_date_time_unref);
	g_clear_pointer(&self->expires_at, g_date_time_unref);
	g_clear_object(&self->staged);

	G_OBJECT_CLASS(venture_ai_confirmation_parent_class)->finalize(object);
}

static void
venture_ai_confirmation_class_init(VentureAiConfirmationClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_ai_confirmation_finalize;
}

static void
venture_ai_confirmation_init(VentureAiConfirmation *self)
{
	self->state = VENTURE_CONFIRMATION_STATE_PENDING;
}

const gchar *
venture_ai_confirmation_get_id(VentureAiConfirmation *self)
{
	g_return_val_if_fail(VENTURE_IS_AI_CONFIRMATION(self), NULL);

	return self->id;
}

const gchar *
venture_ai_confirmation_get_summary(VentureAiConfirmation *self)
{
	g_return_val_if_fail(VENTURE_IS_AI_CONFIRMATION(self), NULL);

	return self->summary;
}

JsonNode *
venture_ai_confirmation_get_diff(VentureAiConfirmation *self)
{
	g_return_val_if_fail(VENTURE_IS_AI_CONFIRMATION(self), NULL);

	return self->diff;
}

VentureConfirmationState
venture_ai_confirmation_get_state(VentureAiConfirmation *self)
{
	g_return_val_if_fail(VENTURE_IS_AI_CONFIRMATION(self),
	                     VENTURE_CONFIRMATION_STATE_EXPIRED);

	return self->state;
}

JsonNode *
venture_ai_confirmation_to_json(VentureAiConfirmation *self)
{
	g_autoptr(JsonBuilder) builder = NULL;

	g_return_val_if_fail(VENTURE_IS_AI_CONFIRMATION(self), NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, self->id);

	json_builder_set_member_name(builder, "summary");
	json_builder_add_string_value(builder, self->summary);

	json_builder_set_member_name(builder, "tool");
	json_builder_add_string_value(builder, self->tool_name);

	json_builder_set_member_name(builder, "state");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_CONFIRMATION_STATE,
		                     (gint)self->state));

	if (NULL != self->diff)
	{
		json_builder_set_member_name(builder, "diff");
		json_builder_add_value(builder, json_node_ref(self->diff));
	}

	if (NULL != self->expires_at)
	{
		g_autofree gchar *text = NULL;

		text = venture_time_to_string(self->expires_at);
		json_builder_set_member_name(builder, "expires_at");
		json_builder_add_string_value(builder, text);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* ==========================================================================
 * Service
 * ========================================================================== */

struct _VentureAiService
{
	GObject parent_instance;

	VentureContext		*context;
	AiProvider		*provider;
	AiToolExecutor		*executor;
	VentureAiPolicy		 policy;
	gchar			*system_prompt;
	gint			 max_tokens;
	gint64			 confirmation_ttl;

	/* Confirmation id -> VentureAiConfirmation. */
	GHashTable		*pending;

	/* The prompt currently being answered, so a staged change and its
	 * audit entry can record what caused it. */
	gchar			*current_prompt;
	VentureAuthPrincipal	*current_principal;
};

G_DEFINE_FINAL_TYPE(VentureAiService, venture_ai_service, G_TYPE_OBJECT)

static void
venture_ai_service_finalize(GObject *object)
{
	VentureAiService *self;

	self = VENTURE_AI_SERVICE(object);

	g_clear_object(&self->context);
	g_clear_object(&self->provider);
	g_clear_object(&self->executor);
	g_clear_pointer(&self->system_prompt, g_free);
	g_clear_pointer(&self->current_prompt, g_free);
	g_clear_pointer(&self->pending, g_hash_table_unref);

	G_OBJECT_CLASS(venture_ai_service_parent_class)->finalize(object);
}

static void
venture_ai_service_class_init(VentureAiServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_ai_service_finalize;
}

static void
venture_ai_service_init(VentureAiService *self)
{
	self->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      g_object_unref);
}

VentureAiPolicy
venture_ai_service_get_policy(VentureAiService *self)
{
	g_return_val_if_fail(VENTURE_IS_AI_SERVICE(self),
	                     VENTURE_AI_POLICY_READ_ONLY);

	return self->policy;
}

/* --- Tool helpers -------------------------------------------------------- */

/*
 * Renders a tool result. Every tool answers in the same shape so the model
 * does not have to learn one convention per tool, and errors come back as
 * readable text rather than as a failed call the model cannot recover from.
 */
static gchar *
venture_ai_tool_result(JsonNode *node)
{
	return venture_json_to_string(node, FALSE);
}

static gchar *
venture_ai_tool_error(const gchar *format, ...) G_GNUC_PRINTF(1, 2);

static gchar *
venture_ai_tool_error(const gchar *format, ...)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *message = NULL;
	va_list args;

	va_start(args, format);
	message = g_strdup_vprintf(format, args);
	va_end(args);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "error");
	json_builder_add_string_value(builder, message);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_ai_tool_result(node);
}

/*
 * Parses a tool's JSON input. ai-glib hands the arguments over as a JsonNode
 * already, so this is mostly a guard against a model sending something other
 * than an object.
 */
static JsonObject *
venture_ai_tool_input(AiToolUse *tool_use)
{
	JsonNode *input;

	input = ai_tool_use_get_input(tool_use);

	if ((NULL == input) || !JSON_NODE_HOLDS_OBJECT(input))
		return NULL;

	return json_node_get_object(input);
}

/* --- Read tools ---------------------------------------------------------- */

static gchar *
venture_ai_tool_list_types(
	AiToolUse	 *tool_use,
	GCancellable	 *cancellable,
	GError		**error,
	gpointer	  user_data
){
	VentureAiService *self;
	g_autoptr(JsonNode) node = NULL;

	self = user_data;
	node = venture_entity_registry_describe_all(
		venture_context_get_entity_registry(self->context));

	return venture_ai_tool_result(node);
}

static gchar *
venture_ai_tool_query(
	AiToolUse	 *tool_use,
	GCancellable	 *cancellable,
	GError		**error,
	gpointer	  user_data
){
	VentureAiService *self;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) records = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) local_error = NULL;
	JsonObject *input;
	const gchar *type_name;
	guint i;

	self = user_data;
	input = venture_ai_tool_input(tool_use);

	if (NULL == input)
		return venture_ai_tool_error("The arguments must be an object");

	type_name = venture_json_object_get_string(input, "type", NULL);

	if (NULL == type_name)
		return venture_ai_tool_error("A \"type\" is required");

	query = venture_query_new_for_name(
		venture_context_get_entity_registry(self->context), type_name,
		&local_error);

	if (NULL == query)
		return venture_ai_tool_error("%s", local_error->message);

	if (!venture_query_apply_json(query, ai_tool_use_get_input(tool_use),
	                              &local_error))
		return venture_ai_tool_error("%s", local_error->message);

	venture_query_set_organization(query,
		venture_context_get_default_organization_id(self->context));

	/* A hard ceiling regardless of what was asked for: a model that
	 * requests every row would blow its own context and learn nothing. */
	if ((0 == venture_query_get_limit(query)) ||
	    (venture_query_get_limit(query) > 200))
		venture_query_set_limit(query, 50);

	records = venture_database_find(venture_context_get_database(self->context),
	                                query, &local_error);

	if (NULL == records)
		return venture_ai_tool_error("%s", local_error->message);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder, (gint64)records->len);

	json_builder_set_member_name(builder, "records");
	json_builder_begin_array(builder);

	for (i = 0; i < records->len; i++)
	{
		/* Sensitive fields are withheld from the model exactly as they
		 * are from an API client. */
		json_builder_add_value(builder,
			venture_serializable_to_json(
				VENTURE_SERIALIZABLE(g_ptr_array_index(records, i)), FALSE));
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_ai_tool_result(node);
}

static gchar *
venture_ai_tool_report(
	AiToolUse	 *tool_use,
	GCancellable	 *cancellable,
	GError		**error,
	gpointer	  user_data
){
	VentureAiService *self;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) local_error = NULL;
	VentureReport *report;
	JsonObject *input;
	const gchar *name;

	self = user_data;
	input = venture_ai_tool_input(tool_use);

	if (NULL == input)
		return venture_ai_tool_error("The arguments must be an object");

	name = venture_json_object_get_string(input, "report", NULL);

	if (NULL == name)
	{
		g_autoptr(JsonNode) available = NULL;

		/* Naming what is available turns a dead end into a usable
		 * next step for the model. */
		available = venture_report_registry_describe(
			venture_context_get_report_registry(self->context));

		return venture_ai_tool_result(available);
	}

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(self->context), name);

	if (NULL == report)
		return venture_ai_tool_error("There is no report called \"%s\"", name);

	period = venture_context_parse_period(self->context,
		venture_json_object_get_string(input, "period", NULL), &local_error);

	if (NULL == period)
		return venture_ai_tool_error("%s", local_error->message);

	result = venture_report_generate(report, self->context, period, input,
	                                 &local_error);

	if (NULL == result)
		return venture_ai_tool_error("%s", local_error->message);

	node = venture_report_result_to_json(result);

	return venture_ai_tool_result(node);
}

/* --- Write tools --------------------------------------------------------- */

/*
 * Stages a change rather than applying it, and returns the tool result that
 * tells the model so.
 *
 * The staged object already carries the change, so approving it later cannot
 * apply something different from what the diff showed.
 */
static gchar *
venture_ai_stage_change(
	VentureAiService	*self,
	const gchar		*tool_name,
	VentureEntity		*staged,
	JsonNode		*diff,
	gboolean		 is_delete,
	const gchar		*summary
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GDateTime) now = NULL;
	VentureAiConfirmation *confirmation;

	confirmation = g_object_new(VENTURE_TYPE_AI_CONFIRMATION, NULL);
	confirmation->id = venture_generate_token(8);
	confirmation->summary = g_strdup(summary);
	confirmation->tool_name = g_strdup(tool_name);
	confirmation->prompt = g_strdup(self->current_prompt);
	confirmation->diff = (NULL != diff) ? json_node_ref(diff) : NULL;
	confirmation->staged = g_object_ref(staged);
	confirmation->is_delete = is_delete;

	now = venture_time_now();
	confirmation->created_at = g_date_time_ref(now);
	confirmation->expires_at = g_date_time_add_seconds(now,
		(gdouble)self->confirmation_ttl);

	g_hash_table_insert(self->pending, g_strdup(confirmation->id),
	                    confirmation);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder, "awaiting_approval");

	json_builder_set_member_name(builder, "confirmation_id");
	json_builder_add_string_value(builder, confirmation->id);

	json_builder_set_member_name(builder, "summary");
	json_builder_add_string_value(builder, summary);

	if (NULL != diff)
	{
		json_builder_set_member_name(builder, "diff");
		json_builder_add_value(builder, json_node_ref(diff));
	}

	/* Telling the model plainly that nothing has changed stops it
	 * reporting success to the operator. */
	json_builder_set_member_name(builder, "note");
	json_builder_add_string_value(builder,
		"Nothing has been changed yet. This is waiting for the operator to "
		"approve it. Tell them what it will do and that it needs their "
		"approval; do not claim it is done.");

	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_ai_tool_result(node);
}

/*
 * Applies a write immediately, used under the autonomous policy and by an
 * approval.
 */
static gboolean
venture_ai_apply(
	VentureAiService	 *self,
	VentureEntity		 *record,
	gboolean		  is_delete,
	const gchar		 *prompt,
	VentureAuthPrincipal	 *principal,
	GError			**error
){
	VentureActor actor;

	actor.kind = VENTURE_ACTOR_KIND_AI;
	actor.name = (NULL != principal) ? principal->name : "ai";
	actor.prompt = prompt;
	actor.request_id = NULL;

	if (is_delete)
	{
		return venture_database_delete(
			venture_context_get_database(self->context), record, &actor,
			error);
	}

	return venture_database_save(venture_context_get_database(self->context),
	                             record, &actor, error);
}

static gchar *
venture_ai_tool_create(
	AiToolUse	 *tool_use,
	GCancellable	 *cancellable,
	GError		**error,
	gpointer	  user_data
){
	VentureAiService *self;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(JsonNode) diff = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *summary = NULL;
	g_autofree gchar *label = NULL;
	JsonObject *input;
	const gchar *type_name;

	self = user_data;
	input = venture_ai_tool_input(tool_use);

	if (NULL == input)
		return venture_ai_tool_error("The arguments must be an object");

	type_name = venture_json_object_get_string(input, "type", NULL);

	if (NULL == type_name)
		return venture_ai_tool_error("A \"type\" is required");

	record = venture_entity_registry_create(
		venture_context_get_entity_registry(self->context), type_name,
		&local_error);

	if (NULL == record)
		return venture_ai_tool_error("%s", local_error->message);

	if (json_object_has_member(input, "values"))
	{
		if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(record),
			json_object_get_member(input, "values"), &local_error))
			return venture_ai_tool_error("%s", local_error->message);
	}

	venture_entity_set_organization_id(record,
		venture_context_get_default_organization_id(self->context));

	/* Validate before staging, so the operator is never asked to approve
	 * something that would fail anyway. */
	if (!venture_entity_validate(record, &local_error))
		return venture_ai_tool_error("%s", local_error->message);

	label = venture_entity_get_display_name(record);
	summary = g_strdup_printf("Create %s \"%s\"", type_name, label);
	diff = venture_serializable_to_json(VENTURE_SERIALIZABLE(record), FALSE);

	if (VENTURE_AI_POLICY_AUTONOMOUS == self->policy)
	{
		if (!venture_ai_apply(self, record, FALSE, self->current_prompt,
		                      self->current_principal, &local_error))
			return venture_ai_tool_error("%s", local_error->message);

		{
			g_autoptr(JsonNode) node = NULL;

			node = venture_serializable_to_json(
				VENTURE_SERIALIZABLE(record), FALSE);

			return venture_ai_tool_result(node);
		}
	}

	return venture_ai_stage_change(self, "venture_create", record, diff,
	                               FALSE, summary);
}

static gchar *
venture_ai_tool_update(
	AiToolUse	 *tool_use,
	GCancellable	 *cancellable,
	GError		**error,
	gpointer	  user_data
){
	VentureAiService *self;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(JsonNode) diff = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *summary = NULL;
	g_autofree gchar *label = NULL;
	JsonObject *input;
	GType entity_type;
	const gchar *type_name;
	gint64 id;

	self = user_data;
	input = venture_ai_tool_input(tool_use);

	if (NULL == input)
		return venture_ai_tool_error("The arguments must be an object");

	type_name = venture_json_object_get_string(input, "type", NULL);
	id = venture_json_object_get_int(input, "id", 0);

	if ((NULL == type_name) || (0 == id))
		return venture_ai_tool_error("A \"type\" and an \"id\" are required");

	entity_type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(self->context), type_name);

	if (G_TYPE_INVALID == entity_type)
		return venture_ai_tool_error("There is no record type called \"%s\"",
		                             type_name);

	record = venture_database_get(venture_context_get_database(self->context),
	                              entity_type, id, &local_error);

	if (NULL == record)
		return venture_ai_tool_error("There is no %s with id %" G_GINT64_FORMAT,
		                             type_name, id);

	/* Keep the original so the diff shows what actually changes. */
	original = venture_database_get(venture_context_get_database(self->context),
	                                entity_type, id, NULL);

	if (json_object_has_member(input, "values"))
	{
		if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(record),
			json_object_get_member(input, "values"), &local_error))
			return venture_ai_tool_error("%s", local_error->message);
	}

	if (!venture_entity_validate(record, &local_error))
		return venture_ai_tool_error("%s", local_error->message);

	diff = venture_entity_diff(original, record);

	if (0 == json_object_get_size(json_node_get_object(diff)))
		return venture_ai_tool_error("That would not change anything");

	label = venture_entity_get_display_name(record);
	summary = g_strdup_printf("Update %s \"%s\"", type_name, label);

	if (VENTURE_AI_POLICY_AUTONOMOUS == self->policy)
	{
		if (!venture_ai_apply(self, record, FALSE, self->current_prompt,
		                      self->current_principal, &local_error))
			return venture_ai_tool_error("%s", local_error->message);

		{
			g_autoptr(JsonNode) node = NULL;

			node = venture_serializable_to_json(
				VENTURE_SERIALIZABLE(record), FALSE);

			return venture_ai_tool_result(node);
		}
	}

	return venture_ai_stage_change(self, "venture_update", record, diff,
	                               FALSE, summary);
}

static gchar *
venture_ai_tool_delete(
	AiToolUse	 *tool_use,
	GCancellable	 *cancellable,
	GError		**error,
	gpointer	  user_data
){
	VentureAiService *self;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *summary = NULL;
	g_autofree gchar *label = NULL;
	JsonObject *input;
	GType entity_type;
	const gchar *type_name;
	gint64 id;

	self = user_data;
	input = venture_ai_tool_input(tool_use);

	if (NULL == input)
		return venture_ai_tool_error("The arguments must be an object");

	type_name = venture_json_object_get_string(input, "type", NULL);
	id = venture_json_object_get_int(input, "id", 0);

	if ((NULL == type_name) || (0 == id))
		return venture_ai_tool_error("A \"type\" and an \"id\" are required");

	entity_type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(self->context), type_name);

	if (G_TYPE_INVALID == entity_type)
		return venture_ai_tool_error("There is no record type called \"%s\"",
		                             type_name);

	record = venture_database_get(venture_context_get_database(self->context),
	                              entity_type, id, &local_error);

	if (NULL == record)
		return venture_ai_tool_error("There is no %s with id %" G_GINT64_FORMAT,
		                             type_name, id);

	label = venture_entity_get_display_name(record);
	summary = g_strdup_printf("Delete %s \"%s\" (recoverable)", type_name,
	                          label);

	/*
	 * Deletion is staged even under the autonomous policy. Everything
	 * else the AI does is a value a person can eyeball afterwards;
	 * removing a record is the one action where noticing late is
	 * materially worse, and soft deletion alone is not reason enough to
	 * let a model do it unattended.
	 */
	return venture_ai_stage_change(self, "venture_delete", record, NULL,
	                               TRUE, summary);
}

/* --- Tool registration --------------------------------------------------- */

/*
 * Builds the parameter schema shared by the query and mutation tools. The
 * record types and their fields are described by the registry, so the model
 * is told what actually exists rather than guessing.
 */
static AiTool *
venture_ai_make_tool(
	VentureAiService	*self,
	const gchar		*name,
	const gchar		*description
){
	AiTool *tool;

	tool = ai_tool_new(name, description);

	return tool;
}

static void
venture_ai_service_register_tools(VentureAiService *self)
{
	g_autoptr(AiTool) list_types = NULL;
	g_autoptr(AiTool) query = NULL;
	g_autoptr(AiTool) report = NULL;

	list_types = venture_ai_make_tool(self, "venture_list_types",
		"List every record type in this VENTURE instance with its fields, "
		"types and permitted values. Call this before querying or changing "
		"anything if you are unsure what exists.");

	query = venture_ai_make_tool(self, "venture_query",
		"Query records. Filters name a field, a comparison and a value; "
		"periods use the same vocabulary everywhere (this_month, last_month, "
		"this_quarter, ytd, fy, 2026-03, 2026-01-01..2026-03-31).");
	ai_tool_add_parameter(query, "type", "string",
		"The record type, e.g. sale, expense, venture, contact", TRUE);
	ai_tool_add_parameter(query, "filters", "array",
		"Filters, each {\"field\":..., \"op\":..., \"value\":...}. "
		"Operators: eq ne lt lte gt gte like ilike in not_in is_null "
		"not_null between", FALSE);
	ai_tool_add_parameter(query, "period", "string",
		"Restrict to a period, e.g. this_month", FALSE);
	ai_tool_add_parameter(query, "period_field", "string",
		"Which date field the period applies to; defaults to occurred_at",
		FALSE);
	ai_tool_add_parameter(query, "search", "string",
		"Free text matched against the type's searchable fields", FALSE);
	ai_tool_add_parameter(query, "limit", "integer",
		"How many records to return; at most 200", FALSE);

	report = venture_ai_make_tool(self, "venture_report",
		"Run a report. Call with no arguments to see what reports exist. "
		"Reports do the money arithmetic correctly, including currency and "
		"deductibility, so prefer one over totalling query results yourself.");
	ai_tool_add_parameter(report, "report", "string",
		"The report name, e.g. pnl, ventures, categories, tax, inventory",
		FALSE);
	ai_tool_add_parameter(report, "period", "string",
		"The period to cover, e.g. this_month", FALSE);
	ai_tool_add_parameter(report, "group_by", "string",
		"For the categories report, the product field to group by; "
		"defaults to genre", FALSE);

	ai_tool_executor_register_callback(self->executor, list_types,
		venture_ai_tool_list_types, self, NULL);
	ai_tool_executor_register_callback(self->executor, query,
		venture_ai_tool_query, self, NULL);
	ai_tool_executor_register_callback(self->executor, report,
		venture_ai_tool_report, self, NULL);

	/*
	 * Under a read-only policy the mutation tools are not registered at
	 * all, rather than registered and refused. A model cannot misuse a
	 * capability it was never told about, and refusing calls it believes
	 * it has wastes turns.
	 */
	if (VENTURE_AI_POLICY_READ_ONLY == self->policy)
		return;

	{
		g_autoptr(AiTool) create = NULL;
		g_autoptr(AiTool) update = NULL;
		g_autoptr(AiTool) remove = NULL;

		create = venture_ai_make_tool(self, "venture_create",
			"Create a record. Unless the policy is autonomous this stages "
			"the change for the operator to approve and does NOT create "
			"anything; say so rather than reporting success.");
		ai_tool_add_parameter(create, "type", "string",
			"The record type to create", TRUE);
		ai_tool_add_parameter(create, "values", "object",
			"Field values, using the field names venture_list_types reports. "
			"Money may be written as \"12.34\" or \"12.34 EUR\"; dates as "
			"YYYY-MM-DD", TRUE);

		update = venture_ai_make_tool(self, "venture_update",
			"Change fields on an existing record. Unless the policy is "
			"autonomous this stages the change for approval and does NOT "
			"apply it.");
		ai_tool_add_parameter(update, "type", "string", "The record type",
		                      TRUE);
		ai_tool_add_parameter(update, "id", "integer",
			"The record's numeric id", TRUE);
		ai_tool_add_parameter(update, "values", "object",
			"Only the fields to change; omitted fields are left alone", TRUE);

		remove = venture_ai_make_tool(self, "venture_delete",
			"Delete a record. This always stages the deletion for the "
			"operator to approve, whatever the policy. Deletion is "
			"recoverable but must still be confirmed by a person.");
		ai_tool_add_parameter(remove, "type", "string", "The record type",
		                      TRUE);
		ai_tool_add_parameter(remove, "id", "integer",
			"The record's numeric id", TRUE);

		ai_tool_executor_register_callback(self->executor, create,
			venture_ai_tool_create, self, NULL);
		ai_tool_executor_register_callback(self->executor, update,
			venture_ai_tool_update, self, NULL);
		ai_tool_executor_register_callback(self->executor, remove,
			venture_ai_tool_delete, self, NULL);
	}
}

/* --- Construction -------------------------------------------------------- */

/*
 * Builds the system prompt. It states the operating rules plainly, because a
 * model that has been told the policy behaves far better than one that
 * discovers it by having a call refused.
 */
static gchar *
venture_ai_service_build_prompt(VentureAiService *self)
{
	g_autoptr(GString) prompt = NULL;
	g_autofree gchar *extra = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *timezone = NULL;

	g_object_get(venture_context_get_config(self->context),
	             "ai-system-prompt-extra", &extra,
	             "locale-default-currency", &currency,
	             "locale-timezone", &timezone,
	             NULL);

	prompt = g_string_new(
		"You are the assistant inside VENTURE, an ERP and CRM the operator "
		"uses to run several small business ventures at once: books, an "
		"Etsy shop, newsletters and whatever else they add.\n\n"
		"Use the tools rather than guessing. venture_list_types tells you "
		"what record types and fields exist. venture_report does the money "
		"arithmetic -- currency handling, deductibility, net versus gross -- "
		"correctly, so prefer a report over adding up query results.\n\n"
		"Be exact about money. Never estimate a figure you could look up, "
		"and never present a gross amount as revenue: net is what reaches "
		"the bank.\n\n");

	if (VENTURE_AI_POLICY_READ_ONLY == self->policy)
	{
		g_string_append(prompt,
			"You cannot change anything. If the operator asks you to, "
			"explain what you would change and tell them to make it "
			"themselves.\n\n");
	}
	else if (VENTURE_AI_POLICY_CONFIRM_WRITES == self->policy)
	{
		g_string_append(prompt,
			"Changes you make are STAGED, not applied. When a tool returns "
			"awaiting_approval, nothing has changed: describe what will "
			"happen and tell the operator it is waiting for their approval. "
			"Never say a change is done when it is staged.\n\n");
	}
	else
	{
		g_string_append(prompt,
			"Your changes apply immediately, except deletions, which always "
			"wait for approval. Be correspondingly careful, and say plainly "
			"what you changed.\n\n");
	}

	g_string_append_printf(prompt,
		"Amounts default to %s. Dates and periods are interpreted in %s.\n",
		currency, timezone);

	if (!venture_string_is_empty(extra))
		g_string_append_printf(prompt, "\n%s\n", extra);

	return g_string_free(g_steal_pointer(&prompt), FALSE);
}

/*
 * Creates the provider named in configuration. Returns NULL when the
 * provider is unknown or has no credentials, which is a normal state rather
 * than a failure.
 */
static AiProvider *
venture_ai_service_create_provider(
	VentureAiService	 *self,
	GError			**error
){
	VentureConfig *config;
	g_autofree gchar *provider_name = NULL;
	g_autofree gchar *model = NULL;
	AiProvider *provider = NULL;

	config = venture_context_get_config(self->context);
	g_object_get(config, "ai-provider", &provider_name, "ai-model", &model,
	             NULL);

	if (0 == g_strcmp0(provider_name, "claude"))
		provider = AI_PROVIDER(ai_claude_client_new());
	else if (0 == g_strcmp0(provider_name, "openai"))
		provider = AI_PROVIDER(ai_openai_client_new());
	else if (0 == g_strcmp0(provider_name, "gemini"))
		provider = AI_PROVIDER(ai_gemini_client_new());
	else if (0 == g_strcmp0(provider_name, "grok"))
		provider = AI_PROVIDER(ai_grok_client_new());
	else if (0 == g_strcmp0(provider_name, "ollama"))
		provider = AI_PROVIDER(ai_ollama_client_new());
	else if (0 == g_strcmp0(provider_name, "claude-code"))
		provider = AI_PROVIDER(ai_claude_code_client_new());
	else if (0 == g_strcmp0(provider_name, "opencode"))
		provider = AI_PROVIDER(ai_opencode_client_new());

	if (NULL == provider)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "\"%s\" is not a provider I know. Use claude, openai, "
		            "gemini, grok, ollama, claude-code or opencode.",
		            provider_name);
		return NULL;
	}

	if (!venture_string_is_empty(model))
		ai_client_set_model(AI_CLIENT(provider), model);

	return provider;
}

VentureAiService *
venture_ai_service_new(
	VentureContext	 *context,
	GError		**error
){
	g_autoptr(VentureAiService) self = NULL;
	VentureConfig *config;
	gint64 max_tokens;
	gboolean enabled;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	config = venture_context_get_config(context);
	g_object_get(config, "ai-enabled", &enabled, NULL);

	if (!enabled)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "AI is disabled in configuration");
		return NULL;
	}

	self = g_object_new(VENTURE_TYPE_AI_SERVICE, NULL);
	self->context = g_object_ref(context);
	self->policy = venture_config_get_ai_policy(config);

	g_object_get(config,
	             "ai-max-tokens", &max_tokens,
	             "ai-confirmation-ttl", &self->confirmation_ttl,
	             NULL);
	self->max_tokens = (gint)max_tokens;

	self->provider = venture_ai_service_create_provider(self, error);

	if (NULL == self->provider)
		return NULL;

	self->executor = ai_tool_executor_new();
	venture_ai_service_register_tools(self);
	self->system_prompt = venture_ai_service_build_prompt(self);

	return g_steal_pointer(&self);
}

/* --- Answering ----------------------------------------------------------- */

gchar *
venture_ai_service_answer_in_thread(
	VentureAiService	 *self,
	GPtrArray		 *history,
	const gchar		 *message,
	VentureAuthPrincipal	 *principal,
	GError			**error
){
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *reply = NULL;
	GList *messages = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_AI_SERVICE(self), NULL);

	if (venture_string_is_empty(message))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Ask a question first");
		return NULL;
	}

	/* Held for the duration so a tool call can record what prompted it. */
	g_free(self->current_prompt);
	self->current_prompt = g_strdup(message);
	self->current_principal = principal;

	/*
	 * The stored transcript is replayed ahead of the new question, oldest
	 * first, so a resumed thread picks up mid-conversation rather than the
	 * model meeting "what about the other one?" with no idea what the one
	 * was. Tool exchanges are deliberately not replayed: their results
	 * were true when they ran, and a stale record read is worse context
	 * than no record at all -- the model can just call the tool again.
	 */
	for (i = 0; (NULL != history) && (i < history->len); i++)
	{
		VentureChatMessage *stored;
		g_autofree gchar *body = NULL;
		VentureChatRole role;

		stored = g_ptr_array_index(history, i);
		g_object_get(stored, "role", &role, "body", &body, NULL);

		if (venture_string_is_empty(body))
			continue;

		messages = g_list_append(messages,
			(VENTURE_CHAT_ROLE_ASSISTANT == role)
				? ai_message_new_assistant(body)
				: ai_message_new_user(body));
	}

	messages = g_list_append(messages, ai_message_new_user(message));

	reply = ai_tool_executor_run(self->executor, self->provider, messages,
	                             self->system_prompt, self->max_tokens, NULL,
	                             &local_error);

	g_list_free_full(messages, g_object_unref);
	self->current_principal = NULL;

	if (NULL == reply)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_AI,
		            "%s", (NULL != local_error) ? local_error->message
		                                        : "the provider failed");
		return NULL;
	}

	return g_steal_pointer(&reply);
}

gchar *
venture_ai_service_answer(
	VentureAiService	 *self,
	const gchar		 *message,
	VentureAuthPrincipal	 *principal,
	GError			**error
){
	return venture_ai_service_answer_in_thread(self, NULL, message,
	                                           principal, error);
}

/* --- Confirmations ------------------------------------------------------- */

/*
 * Marks anything past its deadline as expired. A staged change that has sat
 * unapproved for an hour almost certainly refers to a conversation the
 * operator has moved on from, and applying it later would surprise them.
 */
static void
venture_ai_service_expire(VentureAiService *self)
{
	g_autoptr(GDateTime) now = NULL;
	GHashTableIter iter;
	gpointer value;

	now = venture_time_now();
	g_hash_table_iter_init(&iter, self->pending);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		VentureAiConfirmation *confirmation;

		confirmation = value;

		if (VENTURE_CONFIRMATION_STATE_PENDING != confirmation->state)
			continue;

		if ((NULL != confirmation->expires_at) &&
		    (g_date_time_compare(now, confirmation->expires_at) >= 0))
			confirmation->state = VENTURE_CONFIRMATION_STATE_EXPIRED;
	}
}

GPtrArray *
venture_ai_service_list_pending(VentureAiService *self)
{
	GPtrArray *pending;
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_AI_SERVICE(self), NULL);

	venture_ai_service_expire(self);

	pending = g_ptr_array_new();
	g_hash_table_iter_init(&iter, self->pending);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		VentureAiConfirmation *confirmation;

		confirmation = value;

		if (VENTURE_CONFIRMATION_STATE_PENDING == confirmation->state)
			g_ptr_array_add(pending, confirmation);
	}

	return pending;
}

gboolean
venture_ai_service_approve(
	VentureAiService	 *self,
	const gchar		 *confirmation_id,
	VentureAuthPrincipal	 *principal,
	GError			**error
){
	VentureAiConfirmation *confirmation;

	g_return_val_if_fail(VENTURE_IS_AI_SERVICE(self), FALSE);
	g_return_val_if_fail(NULL != confirmation_id, FALSE);

	venture_ai_service_expire(self);
	confirmation = g_hash_table_lookup(self->pending, confirmation_id);

	if (NULL == confirmation)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no pending change with id %s", confirmation_id);
		return FALSE;
	}

	if (VENTURE_CONFIRMATION_STATE_PENDING != confirmation->state)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "That change is already %s",
		            venture_enum_to_nick(VENTURE_TYPE_CONFIRMATION_STATE,
		                                 (gint)confirmation->state));
		return FALSE;
	}

	if (!venture_ai_apply(self, confirmation->staged, confirmation->is_delete,
	                      confirmation->prompt, principal, error))
	{
		/* A failed apply is recorded as failed rather than left
		 * pending, so it cannot be retried into a different outcome
		 * than the diff described. */
		confirmation->state = VENTURE_CONFIRMATION_STATE_FAILED;
		return FALSE;
	}

	confirmation->state = VENTURE_CONFIRMATION_STATE_APPROVED;

	return TRUE;
}

gboolean
venture_ai_service_reject(
	VentureAiService	 *self,
	const gchar		 *confirmation_id,
	VentureAuthPrincipal	 *principal,
	GError			**error
){
	VentureAiConfirmation *confirmation;

	g_return_val_if_fail(VENTURE_IS_AI_SERVICE(self), FALSE);
	g_return_val_if_fail(NULL != confirmation_id, FALSE);

	confirmation = g_hash_table_lookup(self->pending, confirmation_id);

	if (NULL == confirmation)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no pending change with id %s", confirmation_id);
		return FALSE;
	}

	confirmation->state = VENTURE_CONFIRMATION_STATE_REJECTED;

	/* The refusal is audited too: knowing what the AI proposed and was
	 * denied is as useful as knowing what it did. */
	{
		g_autoptr(VentureAuditEntry) entry = NULL;

		entry = venture_audit_entry_new_for_change(
			VENTURE_AUDIT_ACTION_REJECT, VENTURE_ACTOR_KIND_USER,
			(NULL != principal) ? principal->name : NULL,
			confirmation->staged, confirmation->diff);
		g_object_set(entry, "prompt", confirmation->prompt,
		             "source", "ai", NULL);

		venture_database_save(venture_context_get_database(self->context),
		                      VENTURE_ENTITY(entry), NULL, NULL);
	}

	return TRUE;
}

JsonNode *
venture_ai_service_describe_tools(VentureAiService *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	GList *tools;
	GList *iter;

	g_return_val_if_fail(VENTURE_IS_AI_SERVICE(self), NULL);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	tools = ai_tool_executor_get_tools(self->executor);

	for (iter = tools; NULL != iter; iter = iter->next)
	{
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder,
		                              ai_tool_get_name(iter->data));
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder,
		                              ai_tool_get_description(iter->data));
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}
