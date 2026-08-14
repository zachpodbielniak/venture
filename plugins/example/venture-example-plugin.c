/*
 * venture-example-plugin.c - A worked example of a VENTURE plugin
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This is a complete plugin, small enough to read in one sitting. It shows
 * the two things a plugin usually wants to do:
 *
 *   - register a record type, which becomes a database table, a REST
 *     resource, a web list view, AI tools and CLI subcommands
 *   - register a report, which appears in the CLI, the web UI, the API and
 *     the AI's report tool
 *
 * Neither needs any routing, SQL, serialisation or form code, because every
 * consumer works from the registries rather than a hardcoded list. That is
 * the whole point of deriving the system from property metadata.
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

gboolean
venture_plugin_register(
	VentureContext	 *context,
	GError		**error
){
	/*
	 * Registering the record type is all that is needed for it to gain a
	 * table, REST CRUD at /api/v1/subscription, a web list view, AI tools
	 * and venturectl subcommands. The table is created on the next
	 * migration, which the server runs at startup.
	 */
	if (!venture_entity_registry_register(
		venture_context_get_entity_registry(context),
		VENTURE_TYPE_SUBSCRIPTION, error))
		return FALSE;

	venture_report_registry_add(
		venture_context_get_report_registry(context),
		VENTURE_REPORT(venture_func_report_new(
			"subscriptions", "Recurring subscriptions",
			"Active subscriptions with their annualised cost",
			venture_subscription_report)));

	return TRUE;
}
