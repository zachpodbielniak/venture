/*
 * venture-automation.c - Scheduled automations, driven by podomation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The `venture` pod module is defined here rather than as a loadable .so,
 * because it needs the #VentureContext and an out-of-process module could
 * not be handed one. podomation's module manager accepts an already-built
 * instance, which is exactly the hook that makes this work.
 */

#include "venture.h"

#include <podomation.h>

#include <string.h>

/* ==========================================================================
 * The venture pod module
 * ========================================================================== */

#define VENTURE_TYPE_POD_MODULE (venture_pod_module_get_type())

G_DECLARE_FINAL_TYPE(VenturePodModule, venture_pod_module,
                     VENTURE, POD_MODULE, PodModule)

struct _VenturePodModule
{
	PodModule parent_instance;

	VentureContext *context;
};

static void venture_pod_module_source_init(PodEventSourceInterface *iface);
static void venture_pod_module_handler_init(PodEventHandlerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(VenturePodModule, venture_pod_module,
	POD_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(POD_TYPE_EVENT_SOURCE,
	                      venture_pod_module_source_init)
	G_IMPLEMENT_INTERFACE(POD_TYPE_EVENT_HANDLER,
	                      venture_pod_module_handler_init))

/*
 * What a built-in handler is handed. The built-ins only ever read the
 * context, and they predate the registry: giving them this rather than the
 * pod module is what lets the registry call them with no pod in sight --
 * a direct invocation, or a reload between two engines.
 */
typedef struct
{
	VentureContext *context;
} VentureAutomationCall;

/*
 * The three record-change events. Record changes become events, so a rule
 * can say "when a sale is recorded, do X" without the write path knowing
 * automations exist. Only the database raises these; venture_automation_emit()
 * refuses them, because a plugin raising on_created for a record nobody
 * wrote would set off every rule that trusts the event.
 */
static const gchar *const venture_automation_record_events[] = {
	"on_created", "on_updated", "on_deleted", NULL
};

/* --- Event source --------------------------------------------------------- */

static gboolean
venture_pod_module_start(
	PodEventSource	 *source,
	GMainContext	 *context,
	GError		**error
){
	/* Events are pushed in by the database's signals rather than polled,
	 * so there is no source of our own to attach. */
	return TRUE;
}

static void
venture_pod_module_stop(PodEventSource *source)
{
}

static PodEventKind
venture_pod_module_get_event_kind(PodEventSource *source)
{
	return POD_EVENT_KIND_CUSTOM;
}

/*
 * Asked of the module, not fixed in it: a plugin may have registered an
 * event since the engine was built. podomation does not check a binding
 * against this list -- a pod may bind a name nothing raises -- so this is
 * the reference the REPL and the rules editor read.
 */
static const gchar *const *
venture_pod_module_get_supported_events(PodEventSource *source)
{
	VenturePodModule *self;

	self = VENTURE_POD_MODULE(source);

	if (NULL == self->context)
		return venture_automation_record_events;

	return venture_automation_handler_registry_get_events(
		venture_context_get_automation_handlers(self->context));
}

static void
venture_pod_module_source_init(PodEventSourceInterface *iface)
{
	iface->start = venture_pod_module_start;
	iface->stop = venture_pod_module_stop;
	iface->get_event_kind = venture_pod_module_get_event_kind;
	iface->get_supported_events = venture_pod_module_get_supported_events;
}

/* --- Event handler -------------------------------------------------------- */

/*
 * Reads the n-th positional argument of a DSL call as a string. The public
 * venture_automation_argument() is the one implementation; this name stays
 * because every built-in handler's .inc file is written against it.
 */
static const gchar *
venture_pod_module_argument(
	GVariant	*params,
	gsize		 index
){
	return venture_automation_argument(params, index);
}

/*
 * Collects `field=value` arguments from @first onwards into a hash table.
 *
 * This form exists because podomation's DSL interpolates `{...}` inside
 * string literals, which makes a JSON argument unwritable: every brace in
 * `{"title": "x"}` reads as a variable reference and the rule fails to
 * interpolate rather than doing anything useful. `field=value` has no such
 * collision, and it is the same syntax venturectl takes.
 *
 * Returns: (transfer full) (nullable): the pairs, or %NULL if there are none
 */
static GHashTable *
venture_pod_module_pairs(
	GVariant	*params,
	gsize		 first
){
	GHashTable *pairs;
	gsize i;

	pairs = NULL;

	for (i = first; ; i++)
	{
		const gchar *argument;
		const gchar *equals;

		argument = venture_pod_module_argument(params, i);

		if (NULL == argument)
			break;

		equals = strchr(argument, '=');

		if (NULL == equals)
		{
			g_warning("venture: \"%s\" is not a field=value pair", argument);
			continue;
		}

		if (NULL == pairs)
		{
			pairs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			                              g_free);
		}

		g_hash_table_insert(pairs,
		                    g_strndup(argument, (gsize)(equals - argument)),
		                    g_strdup(equals + 1));
	}

	return pairs;
}

/*
 * Builds the result dictionary a handler returns. Every handler answers in
 * the same shape so a pipe stage downstream can reach {pipe->count} and
 * {pipe->summary} regardless of which handler produced it.
 */
static GVariant *
venture_pod_module_result(
	gint64		 count,
	const gchar	*summary,
	const gchar	*detail
){
	return venture_automation_result_new(count, summary, detail);
}

static gboolean
venture_pod_module_handle_query(
	VentureAutomationCall	 *self,
	GVariant		 *params,
	gboolean		  count_only,
	GVariant		**result
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *summary = NULL;
	const gchar *type_name;
	const gchar *filter_json;
	gint64 count;

	type_name = venture_pod_module_argument(params, 0);
	filter_json = venture_pod_module_argument(params, 1);

	if (NULL == type_name)
	{
		g_warning("venture->query needs a record type");
		return FALSE;
	}

	query = venture_query_new_for_name(
		venture_context_get_entity_registry(self->context), type_name, &error);

	if (NULL == query)
	{
		g_warning("venture->query: %s", error->message);
		return FALSE;
	}

	if (venture_string_is_empty(filter_json))
	{
		/* Nothing to filter on: the count is of everything. */
	}
	else if ('{' == filter_json[0])
	{
		g_autoptr(JsonNode) node = NULL;

		node = venture_json_parse(filter_json, &error);

		if ((NULL == node) ||
		    !venture_query_apply_json(query, node, &error))
		{
			g_warning("venture->query: %s", error->message);
			return FALSE;
		}
	}
	else
	{
		g_autoptr(GHashTable) pairs = NULL;

		/* `venture->query("sale", "channel=etsy", "gross__gte=50")` --
		 * the same filter syntax venturectl and the REST API take. */
		pairs = venture_pod_module_pairs(params, 1);

		if ((NULL != pairs) &&
		    !venture_query_apply_query_string(query, pairs, &error))
		{
			g_warning("venture->query: %s", error->message);
			return FALSE;
		}
	}

	venture_query_set_organization(query,
		venture_context_get_default_organization_id(self->context));

	count = venture_database_count(venture_context_get_database(self->context),
	                               query, &error);

	if (count < 0)
	{
		g_warning("venture->query: %s", error->message);
		return FALSE;
	}

	summary = g_strdup_printf("%" G_GINT64_FORMAT " %s record%s", count,
	                          type_name, (1 == count) ? "" : "s");

	if (NULL != result)
		*result = venture_pod_module_result(count, summary, NULL);

	(void)count_only;

	return TRUE;
}

static gboolean
venture_pod_module_handle_report(
	VentureAutomationCall	 *self,
	GVariant		 *params,
	GVariant		**result
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) report_result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *rendered = NULL;
	g_autofree gchar *summary = NULL;
	VentureReport *report;
	const gchar *name;
	const gchar *period_text;

	name = venture_pod_module_argument(params, 0);
	period_text = venture_pod_module_argument(params, 1);

	if (NULL == name)
	{
		g_warning("venture->report needs a report name");
		return FALSE;
	}

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(self->context), name);

	if (NULL == report)
	{
		g_warning("venture->report: there is no report called \"%s\"", name);
		return FALSE;
	}

	period = venture_context_parse_period(self->context, period_text, &error);

	if (NULL == period)
	{
		g_warning("venture->report: %s", error->message);
		return FALSE;
	}

	report_result = venture_report_generate(report, self->context, period,
	                                        NULL, &error);

	if (NULL == report_result)
	{
		g_warning("venture->report: %s", error->message);
		return FALSE;
	}

	/* The plain-text rendering is what an automation wants to put in a
	 * notification or a log line. */
	rendered = venture_report_result_render(report_result,
	                                        VENTURE_OUTPUT_FORMAT_TEXT);
	summary = g_strdup_printf("%s (%s)",
	                          venture_report_result_get_title(report_result),
	                          venture_date_range_get_label(period));

	if (NULL != result)
	{
		*result = venture_pod_module_result(
			(gint64)venture_report_result_get_row_count(report_result),
			summary, rendered);
	}

	return TRUE;
}

static gboolean
venture_pod_module_handle_create(
	VentureAutomationCall	 *self,
	GVariant		 *params,
	GVariant		**result
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(JsonNode) values = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *label = NULL;
	VentureActor actor;
	const gchar *type_name;
	const gchar *values_json;

	type_name = venture_pod_module_argument(params, 0);
	values_json = venture_pod_module_argument(params, 1);

	if (NULL == type_name)
	{
		g_warning("venture->create needs a record type");
		return FALSE;
	}

	record = venture_entity_registry_create(
		venture_context_get_entity_registry(self->context), type_name, &error);

	if (NULL == record)
	{
		g_warning("venture->create: %s", error->message);
		return FALSE;
	}

	if (venture_string_is_empty(values_json))
	{
		g_warning("venture->create needs at least one field to set");
		return FALSE;
	}

	if ('{' == values_json[0])
	{
		values = venture_json_parse(values_json, &error);

		if ((NULL == values) ||
		    !venture_serializable_from_json(VENTURE_SERIALIZABLE(record),
		                                    values, &error))
		{
			g_warning("venture->create: %s", error->message);
			return FALSE;
		}
	}
	else
	{
		g_autoptr(GHashTable) pairs = NULL;
		GHashTableIter iter;
		gpointer key;
		gpointer value;

		/* `venture->create("idea", "title=A pop-up shop",
		 * "status=researching")` -- the same syntax venturectl takes,
		 * and the only one the DSL can express without its `{...}`
		 * interpolation eating the braces. */
		pairs = venture_pod_module_pairs(params, 1);

		if (NULL == pairs)
		{
			g_warning("venture->create needs at least one field to set");
			return FALSE;
		}

		g_hash_table_iter_init(&iter, pairs);

		while (g_hash_table_iter_next(&iter, &key, &value))
		{
			if (!venture_entity_set_field_from_string(record, key, value,
			                                          &error))
			{
				g_warning("venture->create: %s", error->message);
				return FALSE;
			}
		}
	}

	venture_entity_set_organization_id(record,
		venture_context_get_default_organization_id(self->context));

	/*
	 * Automations write as the automation actor, so anything they change
	 * is distinguishable in the audit log from a person's edit or an
	 * AI's. That distinction is the whole reason the actor exists.
	 */
	actor.kind = VENTURE_ACTOR_KIND_AUTOMATION;
	actor.name = "automation";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;

	if (!venture_database_save(venture_context_get_database(self->context),
	                           record, &actor, &error))
	{
		g_warning("venture->create: %s", error->message);
		return FALSE;
	}

	label = venture_entity_get_display_name(record);

	if (NULL != result)
		*result = venture_pod_module_result(1, label, NULL);

	return TRUE;
}

/*
 * Reports inventory items at or below their reorder point, which is the
 * automation people actually want first.
 */
static gboolean
venture_pod_module_handle_low_stock(
	VentureAutomationCall	 *self,
	GVariant		 *params,
	GVariant		**result
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) report_result = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	gint64 count;

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(self->context), "inventory");

	if (NULL == report)
		return FALSE;

	period = venture_context_parse_period(self->context, "this_month", &error);
	report_result = venture_report_generate(report, self->context, period,
	                                        NULL, &error);

	if (NULL == report_result)
	{
		g_warning("venture->low_stock: %s", error->message);
		return FALSE;
	}

	count = 0;

	{
		GPtrArray *metrics;
		guint i;

		metrics = venture_report_result_get_metrics(report_result);

		for (i = 0; i < metrics->len; i++)
		{
			VentureMetric *metric;

			metric = g_ptr_array_index(metrics, i);

			if (0 == g_strcmp0(venture_metric_get_key(metric),
			                   "below_reorder"))
			{
				count = (gint64)venture_metric_get_number(metric);
				break;
			}
		}
	}

	if (NULL != result)
	{
		g_autofree gchar *summary = NULL;
		g_autofree gchar *detail = NULL;

		summary = g_strdup_printf("%" G_GINT64_FORMAT
		                          " item%s at or below the reorder point",
		                          count, (1 == count) ? "" : "s");

		/*
		 * The rendered report goes along as the detail, because an
		 * alert that says only "3 items are low" sends you off to
		 * look up which three. The count is what a rule branches on;
		 * the detail is what it puts in the message.
		 */
		detail = venture_report_result_render(report_result,
		                                      VENTURE_OUTPUT_FORMAT_TEXT);

		*result = venture_pod_module_result(count, summary, detail);
	}

	return TRUE;
}

#include "assets/venture-assets-automation.inc"
#include "mail/venture-mail-automation.inc"
#include "recurring/venture-recurring-automation.inc"
#include "bankfeed/venture-bankfeed-automation.inc"
#include "commerce/venture-commerce-automation.inc"
#include "report/venture-report-pack-automation.inc"
#include "dunning/venture-dunning-automation.inc"
#include "backup/venture-backup-automation.inc"

static gboolean
venture_pod_module_handle_count(
	VentureAutomationCall	 *self,
	GVariant		 *params,
	GVariant		**result
){
	return venture_pod_module_handle_query(self, params, TRUE, result);
}

static gboolean
venture_pod_module_handle_query_all(
	VentureAutomationCall	 *self,
	GVariant		 *params,
	GVariant		**result
){
	return venture_pod_module_handle_query(self, params, FALSE, result);
}

/* ==========================================================================
 * The built-in handlers, in the registry
 * ========================================================================== */

typedef gboolean (*VentureBuiltinHandler) (
	VentureAutomationCall	 *self,
	GVariant		 *params,
	GVariant		**result
);

/*
 * Registered first and in this order, which is the order the rules editor
 * lists and an "available handlers" refusal names. A built-in reports its
 * own failures with g_warning() and returns FALSE without an error, which
 * the registry turns into "see the log".
 */
static const struct
{
	const gchar		*name;
	VentureBuiltinHandler	 func;
	const gchar		*description;
} venture_automation_builtins[] = {
	{ "query", venture_pod_module_handle_query_all,
	  "Count the records of a type matching a filter" },
	{ "count", venture_pod_module_handle_count,
	  "Count the records of a type matching a filter" },
	{ "report", venture_pod_module_handle_report,
	  "Run a report for a period and hand on its text" },
	{ "create", venture_pod_module_handle_create,
	  "Create a record from field=value pairs" },
	{ "low_stock", venture_pod_module_handle_low_stock,
	  "Inventory items at or below their reorder point" },
	{ "assets_run_period", venture_pod_module_assets_run,
	  "Post a period's depreciation" },
	{ "mail_deliver", venture_pod_module_handle_mail,
	  "Deliver the mail that is due" },
	{ "mail_sync", venture_pod_module_handle_mail_sync,
	  "Fetch new mail from the mailboxes" },
	{ "recurring_run", venture_pod_module_recurring_run,
	  "Raise the recurring documents that are due" },
	{ "collections_run", venture_pod_module_collections_run,
	  "Collect the payments that are due" },
	{ "bankfeed_sync", venture_pod_module_handle_bankfeed,
	  "Fetch new bank transactions" },
	{ "commerce_import", venture_pod_module_handle_commerce,
	  "Import orders from the connected shops" },
	{ "report_packs_run", venture_pod_module_handle_report_packs,
	  "Deliver the report packs that are due" },
	{ "dunning_sweep", venture_pod_module_dunning_sweep,
	  "Send the overdue-invoice reminders" },
	{ "backups_run", venture_pod_module_handle_backups,
	  "Take the backups that are due" },
};

/*
 * Calls a built-in. @user_data is its index in the table above, so the
 * table stays const and nothing is allocated per handler.
 */
static gboolean
venture_automation_builtin_trampoline(
	VentureContext	 *context,
	const gchar	 *name,
	GVariant	 *params,
	GVariant	**result,
	gpointer	  user_data,
	GError		**error
){
	VentureAutomationCall call;
	guint index;

	(void)name;
	(void)error;

	index = GPOINTER_TO_UINT(user_data);
	call.context = context;

	return venture_automation_builtins[index].func(&call, params, result);
}

void
venture_automation_register_builtins(VentureAutomationHandlerRegistry *registry)
{
	g_autoptr(GError) error = NULL;
	gsize i;

	g_return_if_fail(VENTURE_IS_AUTOMATION_HANDLER_REGISTRY(registry));

	for (i = 0; NULL != venture_automation_record_events[i]; i++)
	{
		if (!venture_automation_handler_registry_add_event(registry,
			venture_automation_record_events[i],
			"A record was written or deleted", &error))
		{
			/* Only a registry that already had it; nothing to undo. */
			g_debug("venture_automation: %s", error->message);
			g_clear_error(&error);
		}
	}

	for (i = 0; i < G_N_ELEMENTS(venture_automation_builtins); i++)
	{
		if (!venture_automation_handler_registry_add(registry,
			venture_automation_builtins[i].name,
			venture_automation_builtins[i].description,
			venture_automation_builtin_trampoline,
			GUINT_TO_POINTER((guint)i), NULL, &error))
		{
			g_debug("venture_automation: %s", error->message);
			g_clear_error(&error);
		}
	}
}

/* ==========================================================================
 * Handlers an exec plugin provides
 * ========================================================================== */

typedef struct
{
	gchar	*plugin;
	gchar	*command;
} VentureExecHandler;

static void
venture_exec_handler_free(gpointer data)
{
	VentureExecHandler *handler;

	handler = data;

	g_free(handler->plugin);
	g_free(handler->command);
	g_free(handler);
}

/*
 * Runs an exec plugin as a handler.
 *
 * The rule's positional arguments go to the program as `params.args`, a
 * list of strings in order. What comes back is read as follows: the last
 * `result` message gives count, summary and detail; without one, count is
 * how many messages were neither a `log` nor a `result`, and summary is
 * empty; a missing detail is the `log` lines, one per line. Nothing else a
 * program writes is applied -- a `record` here changes no record -- so an
 * automation that wants a write says so in its own pipeline, where the
 * reader of the rule can see it.
 *
 * The run blocks the main loop for as long as the program takes, up to the
 * manifest's `timeout`. Handlers are synchronous by design (see
 * venture_automation_build_engine()), and the exec run's own deadline is
 * what bounds this one.
 */
static gboolean
venture_automation_exec_handler(
	VentureContext	 *context,
	const gchar	 *name,
	GVariant	 *params,
	GVariant	**result,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureExecResult) run = NULL;
	g_autoptr(JsonObject) request = NULL;
	g_autoptr(GString) logs = NULL;
	VentureExecHandler *handler;
	VenturePluginManager *manager;
	JsonArray *args;
	GPtrArray *messages;
	const gchar *summary;
	const gchar *detail;
	const gchar *argument;
	gboolean answered;
	gint64 count;
	gint64 others;
	gsize i;

	handler = user_data;
	manager = venture_context_get_plugin_manager(context);

	if (NULL == manager)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "venture->%s is provided by the %s plugin, and plugins "
		            "are not loaded", name, handler->plugin);
		return FALSE;
	}

	args = json_array_new();

	for (i = 0; NULL != (argument = venture_automation_argument(params, i)); i++)
		json_array_add_string_element(args, argument);

	request = json_object_new();
	json_object_set_array_member(request, "args", args);

	/* plugins.allow_exec is checked again in here, at every run. */
	run = venture_plugin_manager_run_exec(manager, handler->plugin,
	                                      handler->command, request, NULL,
	                                      NULL, error);

	if ((NULL == run) || !venture_exec_result_check(run, error))
	{
		g_prefix_error(error, "venture->%s: ", name);
		return FALSE;
	}

	messages = venture_exec_result_get_messages(run);
	logs = g_string_new(NULL);
	answered = FALSE;
	summary = NULL;
	detail = NULL;
	count = 0;
	others = 0;

	for (i = 0; i < messages->len; i++)
	{
		VentureJsonlMessage *message;

		message = g_ptr_array_index(messages, i);

		switch (venture_jsonl_message_get_kind(message))
		{
		case VENTURE_JSONL_MESSAGE_RESULT:
			/* The last word wins, so a program may revise its answer. */
			answered = TRUE;
			count = venture_jsonl_message_get_int(message, "count", 0);
			summary = venture_jsonl_message_get_string(message, "summary");
			detail = venture_jsonl_message_get_string(message, "detail");
			break;

		case VENTURE_JSONL_MESSAGE_LOG:
			if (0 != logs->len)
				g_string_append_c(logs, '\n');

			g_string_append(logs,
				venture_jsonl_message_get_string(message, "message"));
			break;

		default:
			others++;
			break;
		}
	}

	if (!answered)
		count = others;

	if ((NULL == detail) && (0 != logs->len))
		detail = logs->str;

	if (NULL != result)
		*result = g_variant_ref_sink(
			venture_automation_result_new(count, summary, detail));

	return TRUE;
}

/*
 * The `automation_handler` provides kind: an exec plugin naming a handler
 * a rule can call. Only an exec plugin may provide one -- a native or
 * crispy plugin registers its handler in code -- and the entry's keys are
 * a closed list, because a misspelt one would load a plugin that quietly
 * runs the wrong command.
 */
static gboolean
venture_automation_accept_handler(
	VenturePluginManager	 *manager,
	VenturePluginManifest	 *manifest,
	JsonObject		 *entry,
	gpointer		  user_data,
	GError			**error
){
	static const gchar *const allowed[] = {
		"kind", "name", "command", "description", NULL
	};
	g_autoptr(GList) members = NULL;
	g_autofree gchar *fallback = NULL;
	VentureExecHandler *handler;
	VentureContext *context;
	const gchar *plugin;
	const gchar *name;
	const gchar *command;
	const gchar *description;
	GList *l;

	(void)user_data;

	plugin = venture_plugin_manifest_get_name(manifest);
	members = json_object_get_members(entry);

	for (l = members; NULL != l; l = l->next)
	{
		if (!g_strv_contains(allowed, l->data))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "an automation_handler entry has no \"%s\" "
			            "(it takes name, command and description)",
			            (const gchar *)l->data);
			return FALSE;
		}
	}

	name = json_object_has_member(entry, "name") &&
	       JSON_NODE_HOLDS_VALUE(json_object_get_member(entry, "name"))
		? json_object_get_string_member(entry, "name") : NULL;
	command = json_object_has_member(entry, "command") &&
	          JSON_NODE_HOLDS_VALUE(json_object_get_member(entry, "command"))
		? json_object_get_string_member(entry, "command") : NULL;
	description = json_object_has_member(entry, "description") &&
	              JSON_NODE_HOLDS_VALUE(json_object_get_member(entry, "description"))
		? json_object_get_string_member(entry, "description") : NULL;

	if (!venture_automation_name_is_valid(name))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		                    "an automation_handler needs a name: a lower-case "
		                    "letter followed by lower-case letters, digits "
		                    "and underscores");
		return FALSE;
	}

	if (json_object_has_member(entry, "command") &&
	    (venture_string_is_empty(command) || (strlen(command) > 256)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "automation_handler %s has an empty or overlong "
		            "command", name);
		return FALSE;
	}

	if (NULL == venture_plugin_manager_lookup_exec(manager, plugin))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		                    "an automation_handler is run as a program, so "
		                    "only an exec plugin may provide one; a native or "
		                    "crispy plugin registers its handler in code");
		return FALSE;
	}

	if (NULL == description)
	{
		fallback = g_strdup_printf("Runs the %s plugin", plugin);
		description = fallback;
	}

	handler = g_new0(VentureExecHandler, 1);
	handler->plugin = g_strdup(plugin);

	/* The handler's own name is the default command, so one program can
	 * answer several handlers by reading `command`. */
	handler->command = g_strdup((NULL != command) ? command : name);

	context = venture_plugin_manager_get_context(manager);

	if (!venture_automation_handler_registry_add(
		venture_context_get_automation_handlers(context), name, description,
		venture_automation_exec_handler, handler, venture_exec_handler_free,
		error))
	{
		venture_exec_handler_free(handler);
		return FALSE;
	}

	return TRUE;
}

void
venture_automation_register_provides(VenturePluginProvidesRegistry *registry)
{
	g_autoptr(GError) error = NULL;

	g_return_if_fail(VENTURE_IS_PLUGIN_PROVIDES_REGISTRY(registry));

	if (!venture_plugin_provides_registry_add(registry, "automation_handler",
		"A step an automation rule can call, run as the plugin's program",
		venture_automation_accept_handler, NULL, NULL, &error))
	{
		g_debug("venture_automation: %s", error->message);
	}
}

/* --- The module's handler interface -------------------------------------- */

static gboolean
venture_pod_module_handle_event(
	PodEventHandler	 *handler,
	const gchar	 *event_name,
	GVariant	 *event_data,
	GVariant	 *params,
	GVariant	**result
){
	g_autoptr(GError) error = NULL;
	VentureAutomationHandlerRegistry *registry;
	VenturePodModule *self;

	self = VENTURE_POD_MODULE(handler);

	g_return_val_if_fail(NULL != event_name, FALSE);

	(void)event_data;

	if (NULL == self->context)
	{
		g_warning("venture pod module has no context");
		return FALSE;
	}

	registry = venture_context_get_automation_handlers(self->context);

	if (!venture_automation_handler_registry_has(registry, event_name))
	{
		g_warning("venture has no handler called \"%s\"", event_name);
		return FALSE;
	}

	if (venture_automation_handler_registry_call(registry, self->context,
	                                             event_name, params, result,
	                                             &error))
		return TRUE;

	/*
	 * VENTURE_ERROR_AUTOMATION is the registry saying the handler logged
	 * its own reason, as every built-in does. Anything else is a reason
	 * nobody has written down yet, and a pod has nowhere else to put it.
	 */
	if (!g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION))
		g_warning("%s", error->message);

	return FALSE;
}

/*
 * Asked of the module rather than fixed in it, like the events: a plugin
 * may have registered a handler since the engine was built.
 */
static const gchar *const *
venture_pod_module_get_supported_handlers(PodEventHandler *handler)
{
	static const gchar *const none[] = { NULL };
	VenturePodModule *self;

	self = VENTURE_POD_MODULE(handler);

	if (NULL == self->context)
		return none;

	return venture_automation_handler_registry_get_names(
		venture_context_get_automation_handlers(self->context));
}

static void
venture_pod_module_handler_init(PodEventHandlerInterface *iface)
{
	iface->handle_event = venture_pod_module_handle_event;
	iface->get_supported_handlers = venture_pod_module_get_supported_handlers;
}

/* --- Module boilerplate --------------------------------------------------- */

static const gchar *
venture_pod_module_get_name(PodModule *module)
{
	return "venture";
}

static const gchar *
venture_pod_module_get_description(PodModule *module)
{
	return "Query, report on and create VENTURE records, and react to "
	       "record changes";
}

/*
 * Each pod gets its own instance, and each must carry the context. Without
 * this override a pod would be constructed with no context and every handler
 * call would fail.
 */
static PodModule *
venture_pod_module_create_instance(
	PodModule	*module,
	const gchar	*constructor_name,
	GPtrArray	*constructor_args
){
	VenturePodModule *self;
	VenturePodModule *instance;

	self = VENTURE_POD_MODULE(module);

	instance = g_object_new(VENTURE_TYPE_POD_MODULE, NULL);
	instance->context = self->context;

	return POD_MODULE(instance);
}

/*
 * podomation's default activate() returns FALSE, on the reasonable grounds
 * that a module which has not said how to start itself has not started. This
 * module has nothing to start: its events are pushed in by the database's
 * signals rather than polled from a source of its own. Activation therefore
 * succeeds as long as the instance actually has a context to work with --
 * without which every handler call would fail anyway, and failing here says
 * so at startup instead of at three in the morning.
 */
static gboolean
venture_pod_module_activate(PodModule *module)
{
	VenturePodModule *self;

	self = VENTURE_POD_MODULE(module);

	if (NULL == self->context)
	{
		g_warning("venture pod module was constructed without a context");
		return FALSE;
	}

	return TRUE;
}

static void
venture_pod_module_deactivate(PodModule *module)
{
}

static void
venture_pod_module_class_init(VenturePodModuleClass *klass)
{
	PodModuleClass *module_class;

	module_class = POD_MODULE_CLASS(klass);
	module_class->get_name = venture_pod_module_get_name;
	module_class->get_description = venture_pod_module_get_description;
	module_class->create_instance = venture_pod_module_create_instance;
	module_class->activate = venture_pod_module_activate;
	module_class->deactivate = venture_pod_module_deactivate;
}

static void
venture_pod_module_init(VenturePodModule *self)
{
}

/* ==========================================================================
 * The engine
 * ========================================================================== */

struct _VentureAutomation
{
	GObject parent_instance;

	VentureContext	*context;
	PodEngine	*engine;
	gboolean	 running;
	guint		 dispatching;
	gulong		 saved_handler;
	gulong		 deleted_handler;
};

G_DEFINE_FINAL_TYPE(VentureAutomation, venture_automation, G_TYPE_OBJECT)

static void
venture_automation_finalize(GObject *object)
{
	VentureAutomation *self;

	self = VENTURE_AUTOMATION(object);

	if (NULL != self->context)
	{
		VentureDatabase *database;

		database = venture_context_get_database(self->context);

		if (0 != self->saved_handler)
			g_signal_handler_disconnect(database, self->saved_handler);

		if (0 != self->deleted_handler)
			g_signal_handler_disconnect(database, self->deleted_handler);
	}

	g_clear_object(&self->engine);
	g_clear_object(&self->context);

	G_OBJECT_CLASS(venture_automation_parent_class)->finalize(object);
}

static void
venture_automation_class_init(VentureAutomationClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_automation_finalize;
}

static void
venture_automation_init(VentureAutomation *self)
{
}

/*
 * Turns a database signal into a pod event. This is the bridge that lets a
 * rule react to a record being written without the write path knowing
 * automations exist.
 */
static void
venture_automation_on_entity_saved(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gboolean	 created,
	gpointer	 user_data
){
	venture_automation_emit_entity_event(VENTURE_AUTOMATION(user_data),
		created ? "on_created" : "on_updated", entity);
}

static void
venture_automation_on_entity_deleted(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gpointer	 user_data
){
	venture_automation_emit_entity_event(VENTURE_AUTOMATION(user_data),
	                                     "on_deleted", entity);
}

/*
 * Builds a fresh engine with every module registered: the venture module as
 * a live instance, podomation's own from disk. Shared by construction and
 * reload, because a reload IS a reconstruction -- pods hold state, and the
 * only honest way to unload a rule is to rebuild the world without it.
 *
 * Returns: %TRUE on success
 */
static gboolean
venture_automation_build_engine(
	VentureAutomation	 *self,
	GError			**error
){
	g_auto(GStrv) module_paths = NULL;
	VenturePodModule *module;
	PodModuleManager *modules;
	gsize i;

	g_object_get(venture_context_get_config(self->context),
	             "automation-module-paths", &module_paths, NULL);

	self->engine = pod_engine_new();

	/*
	 * Handlers run synchronously, on the thread that dispatched them.
	 *
	 * podomation's default handler timeout is 30 seconds, and a non-zero
	 * timeout selects a different dispatch path entirely: the engine
	 * fires handle_event_async and then blocks in g_main_loop_run() on
	 * the default context. VenturePodModule implements only the
	 * synchronous handle_event, so podomation's default async wrapper
	 * runs it on a GTask worker thread -- and that handler writes to the
	 * database and reads the cascade guard, neither of which is safe off
	 * the main thread. Worse, the nested main loop dispatches the next
	 * HTTP request while the current one is still on the stack.
	 *
	 * Zero restores the direct call. The cost is that a wedged handler
	 * has no deadline; the alternative was a data race and a re-entrant
	 * request path, which is a far worse trade for a single-operator
	 * server whose handlers are its own code.
	 */
	pod_config_set_handler_timeout_seconds(pod_engine_get_config(self->engine),
	                                       0);

	modules = pod_engine_get_module_manager(self->engine);

	/*
	 * The venture module is registered as an already-built instance
	 * rather than loaded from a .so, because it needs the context and an
	 * out-of-process module could not be handed one.
	 *
	 * Registration takes the module's reference on success and leaves it
	 * with us on failure, so this deliberately holds a plain pointer
	 * rather than an autoptr: an autoptr here unrefs a module the manager
	 * already owns, and every later pod construction then runs against
	 * freed memory.
	 */
	module = g_object_new(VENTURE_TYPE_POD_MODULE, NULL);
	module->context = self->context;

	if (!pod_module_manager_register(modules, POD_MODULE(module)))
	{
		g_object_unref(module);
		g_clear_object(&self->engine);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION,
		                    "Cannot register the venture automation module");
		return FALSE;
	}

	/* podomation's own modules -- timer, cron, log, http, notifications --
	 * come from wherever they are installed. */
	{
		const gchar *development_path;

		development_path = g_getenv("VENTURE_POD_MODULE_PATH");

		if (NULL != development_path)
		{
			g_auto(GStrv) parts = NULL;

			/* Colon-separated, like every other *_PATH. A development
			 * run wants the build tree's own modules and podomation's
			 * -- cron, timer, log and the rest -- at once. */
			parts = g_strsplit(development_path, ":", -1);

			pod_module_manager_load_from_paths(modules,
				(const gchar *const *)parts);
		}
	}

	pod_module_manager_load_from_directory(modules, VENTURE_PODMODULEDIR);

	for (i = 0; (NULL != module_paths) && (NULL != module_paths[i]); i++)
		pod_module_manager_load_from_directory(modules, module_paths[i]);

	return TRUE;
}

VentureAutomation *
venture_automation_new(
	VentureContext	 *context,
	GError		**error
){
	g_autoptr(VentureAutomation) self = NULL;
	VentureDatabase *database;
	gboolean enabled;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	/* The module registry folds automation.enabled in. */
	enabled = venture_context_module_enabled(context, "automation");

	if (!enabled)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "The automation module is disabled in "
		                    "configuration");
		return NULL;
	}

	self = g_object_new(VENTURE_TYPE_AUTOMATION, NULL);
	self->context = g_object_ref(context);
	venture_data_class_declare_resource(G_OBJECT(self), VENTURE_DATA_CLASS_PLATFORM);
	if (!venture_tenant_service_check_resource(venture_tenant_service_get(
	        venture_context_get_database(context)), G_OBJECT(self), TRUE, error)) return NULL;


	if (!venture_automation_build_engine(self, error))
		return NULL;

	/* Record changes become events. These outlive any one engine: they
	 * reference the automation object, so a reload swaps the engine
	 * underneath them without re-wiring. */
	database = venture_context_get_database(context);
	self->saved_handler = g_signal_connect(database, "entity-saved",
		G_CALLBACK(venture_automation_on_entity_saved), self);
	self->deleted_handler = g_signal_connect(database, "entity-deleted",
		G_CALLBACK(venture_automation_on_entity_deleted), self);

	return g_steal_pointer(&self);
}

gboolean
venture_automation_reload(
	VentureAutomation	 *self,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_AUTOMATION(self), FALSE);

	/*
	 * A reload is a rebuild, not a re-parse: pods hold timers, counters
	 * and circuit-breaker state, and parsing new rules into a running
	 * engine would stack them beside the old ones. Tearing the engine
	 * down and rebuilding from the file is the only path where what runs
	 * afterwards is exactly what the file says.
	 */
	venture_automation_stop(self);
	g_clear_object(&self->engine);

	if (!venture_automation_build_engine(self, error))
		return FALSE;

	return venture_automation_start(self, error);
}

gboolean
venture_automation_validate_dsl(
	const gchar	 *dsl,
	gchar		**out_message
){
	g_autoptr(PodDslParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	PodDslProgram *program;

	g_return_val_if_fail(NULL != dsl, FALSE);

	/*
	 * A fresh parser with no engine behind it: validation must never
	 * disturb what is running, and a syntax check does not need modules
	 * resolved -- an unknown module name surfaces at load, with the
	 * running rules still intact.
	 */
	parser = pod_dsl_parser_new();
	program = pod_dsl_parser_parse(parser, dsl, &error);

	if (NULL == program)
	{
		if (NULL != out_message)
			*out_message = g_strdup((NULL != error)
				? error->message : "unparseable");
		return FALSE;
	}

	pod_dsl_program_free(program);

	if (NULL != out_message)
		*out_message = NULL;

	return TRUE;
}

/*
 * The public header documents venture_automation_describe_modules().
 * @self: a #VentureAutomation
 *
 * Returns: (transfer full): a JSON array of the loaded pod modules, each
 *   with its name and description -- the reference the rules editor shows
 */
JsonNode *
venture_automation_describe_modules(VentureAutomation *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	GList *modules;
	GList *l;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION(self), NULL);

	modules = pod_module_manager_list_modules(
		pod_engine_get_module_manager(self->engine));

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (l = modules; NULL != l; l = l->next)
	{
		PodModule *module;

		module = l->data;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder,
		                              pod_module_get_name(module));
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder,
		                              pod_module_get_description(module));
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	g_list_free(modules);

	return json_builder_get_root(builder);
}

gboolean
venture_automation_load_dsl(
	VentureAutomation	 *self,
	const gchar		 *dsl,
	GError			**error
){
	g_autoptr(GError) local_error = NULL;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION(self), FALSE);
	g_return_val_if_fail(NULL != dsl, FALSE);

	if (!pod_engine_parse_dsl(self->engine, dsl, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION,
		            "Cannot parse the automation rules: %s",
		            (NULL != local_error) ? local_error->message
		                                  : "unknown failure");
		return FALSE;
	}

	return TRUE;
}

gboolean
venture_automation_start(
	VentureAutomation	 *self,
	GError			**error
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *dsl = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *configured = NULL;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION(self), FALSE);

	g_object_get(venture_context_get_config(self->context),
	             "automation-pods-file", &configured, NULL);

	path = venture_config_resolve_path(
		venture_context_get_config(self->context), configured);

	if (!g_file_test(path, G_FILE_TEST_EXISTS))
	{
		/* An install with no automations yet simply has none; that is
		 * not a failure to report. */
		g_debug("venture_automation: no rules at %s", path);
		return TRUE;
	}

	if (!g_file_get_contents(path, &dsl, NULL, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION,
		            "Cannot read %s: %s", path, local_error->message);
		return FALSE;
	}

	if (!venture_automation_load_dsl(self, dsl, error))
	{
		g_prefix_error(error, "%s: ", path);
		return FALSE;
	}

	/*
	 * Embedded, so the engine shares the server's main loop rather than
	 * running on a thread of its own. An automation that writes therefore
	 * serialises with web requests through the same database lock.
	 */
	if (!pod_engine_start_embedded(self->engine, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION,
		            "Cannot start the automation engine: %s",
		            (NULL != local_error) ? local_error->message
		                                  : "unknown failure");
		return FALSE;
	}

	self->running = TRUE;
	g_message("Automations running: %u pod%s",
	          venture_automation_get_pod_count(self),
	          (1 == venture_automation_get_pod_count(self)) ? "" : "s");

	return TRUE;
}

void
venture_automation_stop(VentureAutomation *self)
{
	g_return_if_fail(VENTURE_IS_AUTOMATION(self));

	if (!self->running)
		return;

	pod_engine_stop(self->engine);
	self->running = FALSE;
}

gboolean
venture_automation_is_running(VentureAutomation *self)
{
	g_return_val_if_fail(VENTURE_IS_AUTOMATION(self), FALSE);

	return self->running;
}

gboolean
venture_automation_is_dispatching(VentureAutomation *self)
{
	if (NULL == self)
		return FALSE;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION(self), FALSE);

	return 0 != self->dispatching;
}

guint
venture_automation_get_pod_count(VentureAutomation *self)
{
	GPtrArray *pods;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION(self), 0);

	pods = pod_engine_get_pods(self->engine);

	return (NULL != pods) ? pods->len : 0;
}

/*
 * Hands one event to every pod built on the venture module.
 *
 * The cascade guard lives here, so it holds for a plugin's event exactly
 * as for a record change: an automation's own work -- a write, or an event
 * a handler raises -- does not trigger automations. Without it, the
 * obvious and reasonable-looking rule
 *
 *     watch->on_created => venture->create("research_note", ...);
 *
 * creates a record, which raises on_created, which creates a record,
 * until the disk is full. There is no useful depth limit to pick instead:
 * one level of cascade is as arbitrary as ten, and a rule that genuinely
 * needs to chain can call the next step in its own pipeline, where the
 * author can see it.
 *
 * @payload is borrowed, and must not be floating.
 */
static void
venture_automation_fire(
	VentureAutomation	*self,
	const gchar		*event_name,
	GVariant		*payload
){
	GPtrArray *pods;
	guint i;

	if (0 != self->dispatching)
	{
		g_debug("venture_automation: not raising %s from inside a handler",
		        event_name);
		return;
	}

	pods = pod_engine_get_pods(self->engine);

	if (NULL == pods)
		return;

	self->dispatching++;

	for (i = 0; i < pods->len; i++)
	{
		PodPod *pod;
		gpointer source;

		pod = g_ptr_array_index(pods, i);
		source = pod_pod_get_source(pod);

		/* Only pods built on the venture module care about these; a
		 * timer pod must not receive them. */
		if (!VENTURE_IS_POD_MODULE(source))
			continue;

		g_signal_emit_by_name(source, "event-fired", event_name, payload);
	}

	self->dispatching--;
}

void
venture_automation_emit_entity_event(
	VentureAutomation	*self,
	const gchar		*event_name,
	VentureEntity		*entity
){
	g_autoptr(GVariant) payload = NULL;
	g_autofree gchar *label = NULL;

	g_return_if_fail(VENTURE_IS_AUTOMATION(self));
	g_return_if_fail(NULL != event_name);
	g_return_if_fail(VENTURE_IS_ENTITY(entity));

	if (!self->running)
		return;

	/* An audit entry is itself a record, so emitting an event for one
	 * would make every automation that writes trigger itself. */
	if (VENTURE_IS_AUDIT_ENTRY(entity))
		return;
	if (venture_access_policy_record_is_personal(venture_database_get_access_policy(
		venture_context_get_database(self->context)), entity)) return;

	/* Checked here as well as in fire(), so a write made from inside a
	 * handler does not even build its payload. */
	if (0 != self->dispatching)
	{
		g_debug("venture_automation: not raising %s for %s from inside a "
		        "handler", event_name, venture_entity_get_entity_name(entity));
		return;
	}

	label = venture_entity_get_display_name(entity);
	payload = g_variant_ref_sink(
		g_variant_new_parsed("{'type': <%s>, 'id': <%x>, 'label': <%s>}",
			venture_entity_get_entity_name(entity),
			venture_entity_get_id(entity),
			label));

	venture_automation_fire(self, event_name, payload);
}

gboolean
venture_automation_emit(
	VentureContext	 *context,
	const gchar	 *event_name,
	GVariant	 *data,
	GError		**error
){
	g_autoptr(GVariant) payload = NULL;
	VentureAutomation *self;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(NULL != event_name, FALSE);

	/* Take the caller's floating reference first, so every return below
	 * releases it rather than leaking it. */
	if (NULL != data)
		payload = g_variant_ref_sink(data);

	if (g_strv_contains(venture_automation_record_events, event_name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION,
		            "%s is raised by the database when a record is "
		            "written; it cannot be emitted", event_name);
		return FALSE;
	}

	/*
	 * Judged before anything else, including whether automation is on:
	 * a misspelt event must be found the first time it is emitted, not
	 * the first time somebody switches automation on and wonders why the
	 * rule never fires.
	 */
	if (!venture_automation_handler_registry_has_event(
		venture_context_get_automation_handlers(context), event_name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION,
		            "There is no automation event called \"%s\"; register "
		            "it with venture_automation_handler_registry_add_event() "
		            "first", event_name);
		return FALSE;
	}

	if ((NULL != payload) &&
	    !g_variant_is_of_type(payload, G_VARIANT_TYPE_VARDICT))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The data for %s must be a dictionary (a{sv}), not %s",
		            event_name, g_variant_get_type_string(payload));
		return FALSE;
	}

	self = venture_context_get_automation(context);

	/* Automation off, or not started: an event nobody listens to has
	 * been delivered to everybody who is listening. */
	if ((NULL == self) || !self->running)
		return TRUE;

	if (NULL == payload)
		payload = g_variant_ref_sink(g_variant_new("a{sv}", NULL));

	venture_automation_fire(self, event_name, payload);

	return TRUE;
}

gboolean
venture_automation_invoke(
	VentureAutomation	  *self,
	const gchar		  *handler,
	const gchar *const	  *arguments,
	GVariant		 **result,
	GError			 **error
){
	g_autoptr(GVariant) params = NULL;
	PodModule *module;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION(self), FALSE);
	g_return_val_if_fail(NULL != handler, FALSE);

	module = pod_module_manager_get_module(
		pod_engine_get_module_manager(self->engine), "venture");

	if ((NULL == module) || !VENTURE_IS_POD_MODULE(module))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION,
		                    "The venture automation module is not registered");
		return FALSE;
	}

	/*
	 * The DSL passes positional arguments as a tuple of strings, so a
	 * direct invocation builds the same shape. A handler must not be able
	 * to tell the difference between this and a real trigger, or testing
	 * a rule this way would prove nothing about how it runs.
	 */
	{
		GVariantBuilder builder;

		g_variant_builder_init(&builder, G_VARIANT_TYPE_TUPLE);

		for (i = 0; (NULL != arguments) && (NULL != arguments[i]); i++)
			g_variant_builder_add(&builder, "s", arguments[i]);

		/* An empty tuple is not a valid GVariant type, so a handler
		 * with no arguments gets NULL, which is what it already
		 * tolerates. */
		params = (0 == i) ? NULL
		                  : g_variant_ref_sink(g_variant_builder_end(&builder));

		if (0 == i)
			g_variant_builder_clear(&builder);
	}

	/*
	 * Through the same registry a pod's call reaches, so the handler is
	 * the same one -- but straight to it, so its error comes back to the
	 * caller instead of being written to the log, and an unknown name is
	 * refused with the list of names that exist.
	 */
	return venture_automation_handler_registry_call(
		venture_context_get_automation_handlers(self->context),
		self->context, handler, params, result, error);
}

JsonNode *
venture_automation_describe(VentureAutomation *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	GPtrArray *pods;
	guint i;

	g_return_val_if_fail(VENTURE_IS_AUTOMATION(self), NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "running");
	json_builder_add_boolean_value(builder, self->running);

	json_builder_set_member_name(builder, "pods");
	json_builder_begin_array(builder);

	pods = pod_engine_get_pods(self->engine);

	for (i = 0; (NULL != pods) && (i < pods->len); i++)
	{
		json_builder_add_string_value(builder,
			pod_pod_get_name(g_ptr_array_index(pods, i)));
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}
