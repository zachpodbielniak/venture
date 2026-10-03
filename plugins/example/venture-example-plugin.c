/*
 * venture-example-plugin.c - A worked example of a VENTURE plugin
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This is a complete plugin, small enough to read in one sitting. It shows
 * the things a plugin usually wants to do:
 *
 *   - register a record type, which becomes a database table, a REST
 *     resource, a web list view, AI tools and CLI subcommands
 *   - register a report, which appears in the CLI, the web UI, the API and
 *     the AI's report tool
 *   - register a posting rule, so an explicitly recorded renewal uses the
 *     same atomic journal service as built-in documents
 *   - register an automation handler, so a rule can call
 *     `venture->subscription_costs()` like any built-in step
 *   - add a page of its own, with a sidebar row, through a web extension
 *
 * The first three need no routing, SQL, serialisation or form code, because
 * every consumer works from the registries rather than a hardcoded list.
 * That is the whole point of deriving the system from property metadata.
 * The page is the one thing written by hand, because it is the one thing
 * the field table cannot say.
 *
 * Build it with the tree (`make plugins`) or compile the same source on
 * demand by dropping it in a plugin directory as a .c file -- crispy will
 * compile and cache it, and the entry points are identical.
 */

#include <venture/venture.h>

/* ==========================================================================
 * A record type
 * ========================================================================== */

/*
 * A subscription the business pays for. Real enough to be useful: knowing
 * what renews when, and what it costs annually, is a question the built-in
 * expense record answers badly because an expense is a past event and a
 * subscription is a standing commitment.
 */

#define VENTURE_TYPE_SUBSCRIPTION (venture_subscription_get_type())

VENTURE_DECLARE_ENTITY(VentureSubscription, venture_subscription, SUBSCRIPTION)

static const VentureFieldDecl venture_subscription_fields[] = {
	VENTURE_FIELD_NAME("name", "Service", "What you are paying for"),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", "What each renewal costs"),
	VENTURE_FIELD("cadence", "Cadence",
	              "monthly, quarterly or yearly",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("renews-at", "Renews", "The next renewal date",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("vendor", "Vendor", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("cancel-url", "Cancel at", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureSubscription, venture_subscription,
                      venture_subscription_fields)

/* An explicit renewal event, not an automatic posting of a standing
 * commitment. The host owns journal validation and persistence. */
G_DECLARE_FINAL_TYPE(VentureSubscriptionPostingRule, venture_subscription_posting_rule,
	VENTURE, SUBSCRIPTION_POSTING_RULE, GObject)
struct _VentureSubscriptionPostingRule { GObject parent_instance; };
static void subscription_posting_iface(VenturePostingRuleInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureSubscriptionPostingRule, venture_subscription_posting_rule, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_POSTING_RULE, subscription_posting_iface))

static const gchar *
subscription_posting_name(VenturePostingRule *self)
{
	(void)self;
	return "subscription-renewal";
}

static GPtrArray *
subscription_posting_lines(VenturePostingRule *self, VentureDatabase *db, VentureEntity *source, GError **error)
{
	g_autoptr(VentureExpense) expense = venture_expense_new();
	g_autoptr(VentureMoney) amount = NULL;
	VenturePostingRule *expense_rule;

	(void)self;
	if (!VENTURE_IS_SUBSCRIPTION(source))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A renewal must name a subscription");
		return NULL;
	}
	g_object_get(source, "amount", &amount, NULL);
	g_object_set(expense, "amount", amount, "organization-id",
		venture_entity_get_organization_id(source), NULL);
	expense_rule = venture_posting_rule_registry_lookup(venture_posting_service_get_rules(
		venture_database_get_posting_service(db)), "expense");
	if (NULL == expense_rule)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"The expense posting rule is unavailable");
		return NULL;
	}
	/* Reuse this install's account policy; the source link remains the
	 * subscription, because post_document builds the journal header. */
	return venture_posting_rule_build_lines(expense_rule, db, VENTURE_ENTITY(expense), error);
}
static void
subscription_posting_iface(VenturePostingRuleInterface *iface)
{
	iface->get_name = subscription_posting_name;
	iface->build_lines = subscription_posting_lines;
}
static void venture_subscription_posting_rule_init(VentureSubscriptionPostingRule *self) { (void)self; }
static void venture_subscription_posting_rule_class_init(VentureSubscriptionPostingRuleClass *klass) { (void)klass; }

/* ==========================================================================
 * A report over it
 * ========================================================================== */

/*
 * Multiplies a per-renewal amount up to an annual figure. Exact integer
 * arithmetic, because "what do my subscriptions cost me a year" is a number
 * that ends up in a budget.
 */
static VentureMoney *
venture_subscription_annualise(
	const VentureMoney	 *amount,
	const gchar		 *cadence,
	GError			**error
){
	gint64 renewals_per_year;

	if (NULL == amount)
		return venture_money_new_zero(NULL);

	if (0 == g_strcmp0(cadence, "yearly"))
		renewals_per_year = 1;
	else if (0 == g_strcmp0(cadence, "quarterly"))
		renewals_per_year = 4;
	else if (0 == g_strcmp0(cadence, "weekly"))
		renewals_per_year = 52;
	else
		renewals_per_year = 12;

	return venture_money_multiply_int(amount, renewals_per_year, error);
}

static VentureReportResult *
venture_subscription_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) subscriptions = NULL;
	g_autoptr(VentureMoney) annual_total = NULL;
	gint64 active_count;
	guint i;

	query = venture_query_new(VENTURE_TYPE_SUBSCRIPTION);
	venture_query_set_organization(query,
		venture_context_get_default_organization_id(context));
	venture_query_set_limit(query, 0);

	subscriptions = venture_database_find(
		venture_context_get_database(context), query, error);

	if (NULL == subscriptions)
		return NULL;

	result = venture_report_result_new("Recurring subscriptions", period);
	annual_total = venture_money_new_zero(NULL);
	active_count = 0;

	venture_report_result_add_column(result, "service", "Service",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "vendor", "Vendor",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "cadence", "Cadence",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "amount", "Each",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "annual", "Per year",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	for (i = 0; i < subscriptions->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) annual = NULL;
		g_autoptr(VentureMoney) running = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *vendor = NULL;
		g_autofree gchar *cadence = NULL;
		VentureEntity *subscription;
		gboolean active;

		subscription = g_ptr_array_index(subscriptions, i);

		g_object_get(subscription,
		             "name", &name,
		             "vendor", &vendor,
		             "cadence", &cadence,
		             "amount", &amount,
		             "active", &active,
		             NULL);

		/* A cancelled subscription stays in the table for its history
		 * but must not inflate the annual figure. */
		if (!active)
			continue;

		active_count++;

		annual = venture_subscription_annualise(amount, cadence, NULL);

		if (NULL != annual)
		{
			running = venture_money_add(annual_total, annual, NULL);

			if (NULL != running)
			{
				g_clear_pointer(&annual_total, venture_money_free);
				annual_total = g_steal_pointer(&running);
			}
		}

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "service", name);
		venture_report_result_set_text(result, "vendor", vendor);
		venture_report_result_set_text(result, "cadence", cadence);
		venture_report_result_set_money(result, "amount", amount);
		venture_report_result_set_money(result, "annual", annual);
	}

	{
		VentureMetric *metric;

		metric = venture_metric_new_money("annual", "Annual cost",
		                                  annual_total);
		/* Spending more is not an improvement. */
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}

	/* Counts the rows that were actually counted, not the rows fetched: a
	 * cancelled subscription stays in the table for its history and must
	 * not appear in a figure labelled "active". */
	venture_report_result_add_metric(result,
		venture_metric_new_count("count", "Active subscriptions",
		                         active_count));

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * An automation handler
 * ========================================================================== */

/*
 * `venture->subscription_costs()`: the report's two figures, for a rule
 * that wants to say "you are now paying X a year for N subscriptions" in
 * a weekly message. It answers in the shape every handler does -- count,
 * summary, detail -- so `{pipe->count}` means the same thing after it as
 * after a built-in.
 */
static gboolean
venture_example_subscription_costs(
	VentureContext	 *context,
	const gchar	 *name,
	GVariant	 *params,
	GVariant	**result,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) report = NULL;
	g_autofree gchar *annual_text = NULL;
	g_autofree gchar *summary = NULL;
	g_autofree gchar *detail = NULL;
	GPtrArray *metrics;
	gint64 active;
	guint i;

	(void)name;
	(void)params;
	(void)user_data;

	period = venture_context_parse_period(context, "this_month", error);

	if (NULL == period)
		return FALSE;

	report = venture_subscription_report(context, period, NULL, error);

	if (NULL == report)
		return FALSE;

	active = 0;
	metrics = venture_report_result_get_metrics(report);

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric;

		metric = g_ptr_array_index(metrics, i);

		if (0 == g_strcmp0(venture_metric_get_key(metric), "count"))
			active = (gint64)venture_metric_get_number(metric);
		else if (0 == g_strcmp0(venture_metric_get_key(metric), "annual"))
			annual_text = venture_metric_format_value(metric);
	}

	summary = g_strdup_printf("%" G_GINT64_FORMAT " active subscription%s, "
	                          "%s a year", active, (1 == active) ? "" : "s",
	                          (NULL != annual_text) ? annual_text : "nothing");
	detail = venture_report_result_render(report, VENTURE_OUTPUT_FORMAT_TEXT);

	if (NULL != result)
		*result = venture_automation_result_new(active, summary, detail);

	return TRUE;
}

/* ==========================================================================
 * A page of its own
 * ========================================================================== */

/*
 * GET /example/subscriptions: what the subscriptions cost per year, as a
 * page. Every route carries its own checks -- the module first, so a
 * switched-off plugin answers like a switched-off built-in, then the
 * session -- because a route with no guard looks exactly like one with.
 */
static HtmxResponse *
venture_example_page(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) report = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	VentureWebServer *server;
	VentureContext *context;
	HtmxResponse *refusal;
	GPtrArray *metrics;
	guint i;

	(void)params;

	server = user_data;
	context = venture_web_server_get_context(server);

	refusal = venture_web_server_require_module(server, request, "example");

	if (NULL != refusal)
		return refusal;

	refusal = venture_web_server_require_page(server, request,
	                                          VENTURE_USER_ROLE_VIEWER);

	if (NULL != refusal)
		return refusal;

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Subscription costs</h1>"
	                       "<span class=\"subtitle\">What the active "
	                       "subscriptions cost, per year</span></div></div>"
	                       "<section class=\"card\"><div class=\"card-body\">");

	period = venture_context_parse_period(context, "this_month", &error);
	report = (NULL != period)
		? venture_subscription_report(context, period, NULL, &error) : NULL;

	if (NULL == report)
	{
		g_string_append(content, "<p class=\"muted\">");
		venture_html_escape_append(content, error->message);
		g_string_append(content, "</p>");
	}
	else
	{
		metrics = venture_report_result_get_metrics(report);

		g_string_append(content, "<dl>");

		for (i = 0; i < metrics->len; i++)
		{
			g_autofree gchar *value = NULL;
			VentureMetric *metric;

			metric = g_ptr_array_index(metrics, i);
			value = venture_metric_format_value(metric);

			/* Escaped: a label or a figure is text, never markup. */
			g_string_append(content, "<dt>");
			venture_html_escape_append(content, venture_metric_get_label(metric));
			g_string_append(content, "</dt><dd>");
			venture_html_escape_append(content, value);
			g_string_append(content, "</dd>");
		}

		g_string_append(content, "</dl><p><a href=\"/e/subscription\">"
		                          "Every subscription</a></p>");
	}

	g_string_append(content, "</div></section>");

	return venture_web_server_render_page(server, request,
	                                      "/example/subscriptions",
	                                      "Subscription costs", content->str);
}

/*
 * Runs once for every web server built over the context, after the
 * server's own routes: the route first, then the row that points at it --
 * a row is refused unless something already serves its path.
 */
static gboolean
venture_example_web(
	VentureWebServer	 *server,
	gpointer		  user_data,
	GError			**error
){
	(void)user_data;

	venture_web_server_add_classified_route(server, HTMX_METHOD_GET,
		"/example/subscriptions", VENTURE_DATA_CLASS_TENANT,
		VENTURE_HOSTED_ROUTE_NONE, venture_example_page, server);

	/* An icon by name, never markup; the module hides the row when the
	 * plugin is switched off. */
	return venture_web_server_add_nav_link(server, "/example/subscriptions",
	                                       "Subscription costs", "calendar",
	                                       "example", error);
}

/* ==========================================================================
 * Entry points
 * ========================================================================== */

/**
 * venture_plugin_info:
 *
 * Optional. Shown in `venturectl`'s plugin list and at /api/v1/plugins.
 *
 * Returns: (transfer none): a one-line description
 */
const gchar *
venture_plugin_info(void);

const gchar *
venture_plugin_info(void)
{
	return "Tracks recurring subscriptions and what they cost per year";
}

/**
 * venture_plugin_register:
 * @context: the wiring, giving access to every registry
 * @error: (out) (optional): return location for a #GError
 *
 * Required. Called once when the plugin is loaded.
 *
 * Returns: %TRUE if the plugin registered successfully
 */
gboolean
venture_plugin_register(
	VentureContext	 *context,
	GError		**error
);

/*
 * The plugin as a module.
 *
 * Declaring one is what lets an install switch the plugin off with
 * `modules.example.enabled: false` exactly as it switches a built-in
 * module, and what hides the subscription type and its report when it
 * does. The description must outlive the plugin, which a static does.
 */
static GType (*const venture_example_types[]) (void) = {
	venture_subscription_get_type,
	NULL
};

static const gchar *const venture_example_requires[] = { "finance", NULL };
static const gchar *const venture_example_reports[] = { "subscriptions", NULL };

static const VentureModuleInfo venture_example_module = {
	"example", "Example plugin",
	"Recurring subscriptions, from the worked example plugin.",
	venture_example_requires, NULL,
	venture_example_types, venture_example_reports, NULL, FALSE
};

gboolean
venture_plugin_register(
	VentureContext	 *context,
	GError		**error
){
	/*
	 * Registering the module registers the record type with it, which
	 * is all that is needed for the type to gain a table, REST CRUD at
	 * /api/v1/subscription, a web list view, AI tools and venturectl
	 * subcommands. The table is created on the next migration, which
	 * the server runs at startup.
	 *
	 * A module that requires one the configuration turned off is
	 * refused here, and that is "not now" rather than "broken": the
	 * plugin reports it and the server carries on.
	 */
	if (!venture_context_register_module(context, &venture_example_module,
	                                     error))
		return FALSE;

	venture_report_registry_add(
		venture_context_get_report_registry(context),
		VENTURE_REPORT(venture_func_report_new(
			"subscriptions", "Recurring subscriptions",
			"Active subscriptions with their annualised cost",
			venture_subscription_report)));

	venture_posting_rule_registry_add(venture_posting_service_get_rules(
		venture_database_get_posting_service(venture_context_get_database(context))),
		g_object_new(venture_subscription_posting_rule_get_type(), NULL));

	/* The report was added after the module was applied; apply again so
	 * a disabled module hides it too. */
	venture_context_apply_modules(context);

	/*
	 * A step automations can call. The registry is the context's, so it
	 * outlives every rebuild of the automation engine; a name a built-in
	 * or another plugin already holds is refused rather than replaced.
	 */
	if (!venture_automation_handler_registry_add(
		venture_context_get_automation_handlers(context),
		"subscription_costs",
		"Active subscriptions and what they cost per year",
		venture_example_subscription_costs, NULL, NULL, error))
		return FALSE;

	/* The server does not exist yet when plugins load, so the page is
	 * left as an extension for every server built later. */
	venture_context_add_web_extension(context, venture_example_web, NULL,
	                                  NULL);

	return TRUE;
}
