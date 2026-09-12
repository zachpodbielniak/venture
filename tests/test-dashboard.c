/*
 * test-dashboard.c - Dashboards and their widgets
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A dashboard is a page of widgets; a widget is a declarative question
 * answered by a kind. Everything here is about the checks that keep a
 * definition honest -- a kind that exists, a type that exists, one home
 * page -- and about a widget giving the same answer as JSON and as HTML,
 * within the viewer's scope, whichever door it is read through.
 */

#include <venture.h>

#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

/* --- A context, no server ------------------------------------------------- */

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
} Fixture;

static void
file_under_default(
	Fixture		*fixture,
	gpointer	 record
){
	venture_entity_set_organization_id(VENTURE_ENTITY(record),
		venture_context_get_default_organization_id(fixture->context));
}

static gint64
create_ticket(
	Fixture			*fixture,
	const gchar		*title,
	VentureTicketStatus	 status,
	const gchar		*assignee
){
	g_autoptr(VentureTicket) ticket = NULL;

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", title, "status", status,
	             "assignee", assignee, NULL);
	file_under_default(fixture, ticket);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, NULL));

	return venture_entity_get_id(VENTURE_ENTITY(ticket));
}

/*
 * A user to own things. References are checked at the save, so an owner
 * has to be a real row.
 */
static gint64
create_user(
	VentureDatabase	*database,
	const gchar	*username
){
	g_autoptr(VentureUser) user = NULL;

	user = venture_user_new();
	g_object_set(user, "username", username, "role", VENTURE_USER_ROLE_EDITOR,
	             "active", TRUE, NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(user), NULL,
	                                    NULL));

	return venture_entity_get_id(VENTURE_ENTITY(user));
}

static VentureDashboard *
create_dashboard(
	Fixture		*fixture,
	const gchar	*name
){
	VentureDashboard *dashboard;

	dashboard = venture_dashboard_new();
	g_object_set(dashboard, "name", name, NULL);
	file_under_default(fixture, dashboard);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(dashboard), NULL, NULL));

	return dashboard;
}

/*
 * A widget with its settings given as property pairs, saved.
 */
static VentureDashboardWidget *
create_widget(
	Fixture			*fixture,
	VentureDashboard	*dashboard,
	const gchar		*kind,
	const gchar		*first_property,
	...
){
	VentureDashboardWidget *widget;
	g_autoptr(GError) error = NULL;
	va_list args;

	widget = venture_dashboard_widget_new();
	g_object_set(widget, "dashboard-id",
	             venture_entity_get_id(VENTURE_ENTITY(dashboard)),
	             "kind", kind, NULL);

	va_start(args, first_property);
	g_object_set_valist(G_OBJECT(widget), first_property, args);
	va_end(args);

	file_under_default(fixture, widget);

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(widget), NULL,
	                           &error))
		g_error("saving a %s widget: %s", kind, error->message);

	return widget;
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	/* The entity registry is process-wide and the last context to apply
	 * a configuration wins; leave it with everything on for the next
	 * test file's sake. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}

/*
 * The dashboards module owns its two types; the kind catalogue has the
 * built-in kinds and says which belong to a module.
 */
static void
test_dashboard_module_and_kinds(void)
{
	g_autoptr(VentureModuleRegistry) registry = NULL;
	g_auto(GStrv) names = NULL;
	VentureModule *module;
	VentureWidgetKindRegistry *kinds;
	const VentureWidgetKindInfo *info;
	const gchar *const *types;

	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);

	module = venture_module_registry_lookup(registry, "dashboards");
	g_assert_nonnull(module);
	types = venture_module_get_entity_names(module);
	g_assert_cmpuint(g_strv_length((gchar **)types), ==, 2);
	g_assert_true(g_strv_contains(types, "dashboard"));
	g_assert_true(g_strv_contains(types, "dashboard_widget"));

	kinds = venture_widget_kind_registry_get_default();
	names = venture_widget_kind_registry_list_names(kinds);
	g_assert_cmpuint(g_strv_length(names), >=, 16);
	g_assert_true(g_strv_contains((const gchar *const *)names, "count"));
	g_assert_true(g_strv_contains((const gchar *const *)names, "note"));

	info = venture_widget_kind_registry_lookup(kinds, "environments");
	g_assert_nonnull(info);
	g_assert_cmpstr(info->module, ==, "factory");

	info = venture_widget_kind_registry_lookup(kinds, "list");
	g_assert_nonnull(info);
	g_assert_null(info->module);
	g_assert_true(g_strv_contains(info->uses, "entity_type"));

	g_assert_null(venture_widget_kind_registry_lookup(kinds, "no-such"));
}

/*
 * A plugin can add a kind; a malformed or duplicate one is refused.
 */
static VentureWidgetResult *
plugin_kind_func(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	VentureWidgetResult *result;

	(void)context;
	(void)widget;
	(void)scope;
	(void)error;

	result = venture_widget_result_new();
	result->title = g_strdup("Plugin");
	result->html = g_strdup(user_data);
	result->data = json_node_new(JSON_NODE_NULL);

	return result;
}

static void
test_dashboard_plugin_kind(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const VentureWidgetKindInfo info = {
		"test_plugin_kind", "Test", "A kind from a test", NULL, NULL,
		plugin_kind_func
	};
	static const VentureWidgetKindInfo bad_name = {
		"Not A Name", "Bad", NULL, NULL, NULL, plugin_kind_func
	};
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) widget = NULL;
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureWidgetKindRegistry *kinds;

	(void)user_data;

	kinds = venture_widget_kind_registry_get_default();

	g_assert_false(venture_widget_kind_registry_add(kinds, &bad_name, NULL,
	                                                NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Registered once per process; a second test file in the same
	 * binary would see it already there. */
	if (NULL == venture_widget_kind_registry_lookup(kinds, "test_plugin_kind"))
	{
		g_assert_true(venture_widget_kind_registry_add(kinds, &info,
			(gpointer)"<b>from the plugin</b>", NULL, &error));
		g_assert_no_error(error);
	}

	g_assert_false(venture_widget_kind_registry_add(kinds, &info, NULL, NULL,
	                                                &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);

	dashboard = create_dashboard(fixture, "Plugged");
	widget = create_widget(fixture, dashboard, "test_plugin_kind",
	                       "title", "Mine", NULL);
	result = venture_dashboard_render_widget(fixture->context, widget, NULL);
	g_assert_null(result->error);
	g_assert_cmpstr(result->title, ==, "Mine");
	g_assert_cmpstr(result->html, ==, "<b>from the plugin</b>");
}

/*
 * The slug is the address: derived from the name when blank, normalised
 * when typed, and never shared by two living dashboards.
 *
 * What breaks if this regresses: two pages at one URL, and the second
 * one unreachable.
 */
static void
test_dashboard_slug(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) first = NULL;
	g_autoptr(VentureDashboard) second = NULL;
	g_autoptr(VentureDashboard) found = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;

	(void)user_data;

	first = create_dashboard(fixture, "Month End Close");
	g_object_get(first, "slug", &slug, NULL);
	g_assert_cmpstr(slug, ==, "month-end-close");

	found = venture_dashboard_find_by_slug(fixture->database,
	                                       "month-end-close", &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(found)), ==,
	                venture_entity_get_id(VENTURE_ENTITY(first)));

	second = venture_dashboard_new();
	g_object_set(second, "name", "Another", "slug", "Month End Close", NULL);
	file_under_default(fixture, second);
	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(second), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);

	g_object_set(second, "slug", "Another One", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(second), NULL, &error));
	g_assert_no_error(error);
	g_clear_pointer(&slug, g_free);
	g_object_get(second, "slug", &slug, NULL);
	g_assert_cmpstr(slug, ==, "another-one");

	g_clear_object(&found);
	found = venture_dashboard_find_by_slug(fixture->database, "nowhere",
	                                       &error);
	g_assert_null(found);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/*
 * A widget must name a kind, a type, a report and options that make
 * sense -- at the save, so every writer is held to it. A type whose
 * module is off is accepted: the widget outlives the switch.
 */
static void
test_dashboard_widget_validation(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) widget = NULL;
	g_autoptr(GError) error = NULL;
	gint64 dashboard_id;

	(void)user_data;

	dashboard = create_dashboard(fixture, "Checked");
	dashboard_id = venture_entity_get_id(VENTURE_ENTITY(dashboard));

	widget = venture_dashboard_widget_new();
	file_under_default(fixture, widget);
	g_object_set(widget, "dashboard-id", dashboard_id, "kind", "sparkline",
	             NULL);
	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(widget), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "sparkline"));
	g_clear_error(&error);

	g_object_set(widget, "kind", "list", "entity-type", "widgets", NULL);
	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(widget), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	g_object_set(widget, "entity-type", "ticket", "report-name", "nope", NULL);
	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(widget), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	g_object_set(widget, "report-name", NULL, "options", "not json", NULL);
	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(widget), NULL, &error));
	g_clear_error(&error);

	g_object_set(widget, "options", "[1,2]", NULL);
	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(widget), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	g_object_set(widget, "options", "{\"days\": 7}", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(widget), NULL, &error));
	g_assert_no_error(error);

	/* With the factory off "release" is hidden -- and still accepted:
	 * the widget outlives the switch. */
	venture_config_set_module_enabled(fixture->config, "factory", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "factory"));
	g_object_set(widget, "entity-type", "release", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(widget), NULL, &error));
	g_assert_no_error(error);

	/* A hidden report likewise. */
	g_object_set(widget, "kind", "metric", "entity-type", NULL,
	             "report-name", "lead_time", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(widget), NULL, &error));
	g_assert_no_error(error);
}

/*
 * Counting, listing and breaking down, from the same seeded tickets, with
 * the viewer substituted into the filter.
 */
static void
test_dashboard_count_list_breakdown(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) count = NULL;
	g_autoptr(VentureDashboardWidget) mine = NULL;
	g_autoptr(VentureDashboardWidget) list = NULL;
	g_autoptr(VentureDashboardWidget) breakdown = NULL;
	g_autoptr(VentureWidgetResult) result = NULL;
	VentureWidgetScope scope = { NULL, 0, 7, "zach", 0 };
	JsonObject *data;

	(void)user_data;

	create_ticket(fixture, "One", VENTURE_TICKET_STATUS_TODO, "zach");
	create_ticket(fixture, "Two", VENTURE_TICKET_STATUS_TODO, "someone");
	create_ticket(fixture, "Three", VENTURE_TICKET_STATUS_DONE, "zach");
	create_ticket(fixture, "Four", VENTURE_TICKET_STATUS_BLOCKED, "zach");

	dashboard = create_dashboard(fixture, "Work");

	count = create_widget(fixture, dashboard, "count",
	                      "entity-type", "ticket",
	                      "filter", "status__not_in=done,cancelled", NULL);
	result = venture_dashboard_render_widget(fixture->context, count, &scope);
	g_assert_null(result->error);
	data = json_node_get_object(result->data);
	g_assert_cmpint(json_object_get_int_member(data, "count"), ==, 3);
	g_assert_cmpstr(result->title, ==, "Tickets");
	g_assert_nonnull(strstr(result->html, ">3</a>"));
	g_assert_cmpstr(result->link, ==,
	                "/e/ticket?status__not_in=done,cancelled");

	/* {me} is the viewer, whoever that is. */
	g_clear_pointer(&result, venture_widget_result_free);
	mine = create_widget(fixture, dashboard, "count",
	                     "entity-type", "ticket",
	                     "filter", "assignee={me}&status__ne=done",
	                     "title", "Mine", NULL);
	result = venture_dashboard_render_widget(fixture->context, mine, &scope);
	g_assert_null(result->error);
	g_assert_cmpstr(result->title, ==, "Mine");
	g_assert_cmpint(json_object_get_int_member(
		json_node_get_object(result->data), "count"), ==, 2);

	{
		VentureWidgetScope other = { NULL, 0, 8, "someone", 0 };

		g_clear_pointer(&result, venture_widget_result_free);
		result = venture_dashboard_render_widget(fixture->context, mine,
		                                         &other);
		g_assert_cmpint(json_object_get_int_member(
			json_node_get_object(result->data), "count"), ==, 1);
	}

	g_clear_pointer(&result, venture_widget_result_free);
	list = create_widget(fixture, dashboard, "list",
	                     "entity-type", "ticket", "order", "-id",
	                     "limit", (gint64)2, "columns", "status,assignee",
	                     NULL);
	result = venture_dashboard_render_widget(fixture->context, list, &scope);
	g_assert_null(result->error);
	data = json_node_get_object(result->data);
	{
		JsonArray *rows;
		JsonObject *row;

		rows = json_object_get_array_member(data, "rows");
		g_assert_cmpuint(json_array_get_length(rows), ==, 2);
		row = json_array_get_object_element(rows, 0);
		g_assert_cmpstr(json_object_get_string_member(row, "label"), ==,
		                "Four");
		g_assert_cmpstr(json_object_get_string_member(row, "status"), ==,
		                "Blocked");
		g_assert_cmpstr(json_object_get_string_member(row, "assignee"), ==,
		                "zach");
	}
	g_assert_nonnull(strstr(result->html, "href=\"/e/ticket/4\""));
	g_assert_nonnull(strstr(result->html, "Blocked"));

	g_clear_pointer(&result, venture_widget_result_free);
	breakdown = create_widget(fixture, dashboard, "breakdown",
	                          "entity-type", "ticket", "field", "status",
	                          NULL);
	result = venture_dashboard_render_widget(fixture->context, breakdown,
	                                         &scope);
	g_assert_null(result->error);
	data = json_node_get_object(result->data);
	g_assert_cmpint(json_object_get_int_member(data, "total"), ==, 4);
	{
		JsonArray *groups;
		guint i;
		gboolean saw_todo;

		groups = json_object_get_array_member(data, "groups");
		saw_todo = FALSE;

		for (i = 0; i < json_array_get_length(groups); i++)
		{
			JsonObject *group;

			group = json_array_get_object_element(groups, i);

			if (0 == g_strcmp0(json_object_get_string_member(group, "value"),
			                   "todo"))
			{
				saw_todo = TRUE;
				g_assert_cmpint(json_object_get_int_member(group, "count"),
				                ==, 2);
			}
		}

		g_assert_true(saw_todo);
	}
	g_assert_nonnull(strstr(result->html, "href=\"/e/ticket?status=todo\""));
	g_assert_cmpstr(result->title, ==, "Tickets by Status");

	/* A breakdown on a field that is not an enum is a clear refusal. */
	g_clear_pointer(&result, venture_widget_result_free);
	g_object_set(breakdown, "field", "title", NULL);
	result = venture_dashboard_render_widget(fixture->context, breakdown,
	                                         &scope);
	g_assert_nonnull(result->error);
	g_assert_null(result->html);
}

/*
 * Words and buttons: a note's bullets and links, and an action whose
 * href would run script is dropped rather than rendered.
 */
static void
test_dashboard_note_and_actions(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) note = NULL;
	g_autoptr(VentureDashboardWidget) actions = NULL;
	g_autoptr(VentureWidgetResult) result = NULL;
	VentureWidgetScope scope = { NULL, 0, 3, "zach", 0 };

	(void)user_data;

	dashboard = create_dashboard(fixture, "Words");

	note = create_widget(fixture, dashboard, "note",
	                     "body", "Read /reports/pnl first.\n\n- one <b>\n- two",
	                     NULL);
	result = venture_dashboard_render_widget(fixture->context, note, &scope);
	g_assert_null(result->error);
	g_assert_nonnull(strstr(result->html,
		"<a href=\"/reports/pnl\">/reports/pnl</a> first."));
	g_assert_nonnull(strstr(result->html, "<ul><li>one &lt;b&gt;</li>"));
	g_assert_null(strstr(result->html, "<b>"));

	g_clear_pointer(&result, venture_widget_result_free);
	actions = create_widget(fixture, dashboard, "actions",
	                        "body",
	                        "New ticket | /e/ticket/new\n"
	                        "My board | /tickets?assignee={me}\n"
	                        "Evil | javascript:alert(1)\n"
	                        "Elsewhere | https://example.com/x\n"
	                        "Also evil | //example.com/x",
	                        NULL);
	result = venture_dashboard_render_widget(fixture->context, actions,
	                                         &scope);
	g_assert_null(result->error);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(result->data)),
	                 ==, 3);
	g_assert_nonnull(strstr(result->html, "href=\"/tickets?assignee=zach\""));
	g_assert_null(strstr(result->html, "javascript"));
	g_assert_null(strstr(result->html, "href=\"//example.com"));
}

/*
 * A report's figure, its table and its bars, from the same result.
 */
static void
test_dashboard_report_kinds(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) metric = NULL;
	g_autoptr(VentureDashboardWidget) report = NULL;
	g_autoptr(VentureDashboardWidget) chart = NULL;
	g_autoptr(VentureWidgetResult) result = NULL;

	(void)user_data;

	dashboard = create_dashboard(fixture, "Figures");

	metric = create_widget(fixture, dashboard, "metric",
	                       "report-name", "pnl", "field", "revenue", NULL);
	result = venture_dashboard_render_widget(fixture->context, metric, NULL);
	g_assert_null(result->error);
	g_assert_cmpstr(result->title, ==, "Net revenue");
	g_assert_cmpstr(result->link, ==, "/reports/pnl");
	g_assert_true(JSON_NODE_HOLDS_OBJECT(result->data));

	g_clear_pointer(&result, venture_widget_result_free);
	g_object_set(metric, "field", "no_such_metric", NULL);
	result = venture_dashboard_render_widget(fixture->context, metric, NULL);
	g_assert_nonnull(result->error);

	g_clear_pointer(&result, venture_widget_result_free);
	report = create_widget(fixture, dashboard, "report",
	                       "report-name", "pnl", "period", "this_year",
	                       "options", "{\"table\": false}", NULL);
	result = venture_dashboard_render_widget(fixture->context, report, NULL);
	g_assert_null(result->error);
	g_assert_nonnull(strstr(result->html, "stat-value"));
	g_assert_null(strstr(result->html, "<table"));

	g_clear_pointer(&result, venture_widget_result_free);
	chart = create_widget(fixture, dashboard, "chart",
	                      "report-name", "ventures", "columns", "venture",
	                      "field", "revenue", NULL);
	result = venture_dashboard_render_widget(fixture->context, chart, NULL);
	g_assert_null(result->error);
	g_assert_nonnull(strstr(result->html, "bar-list"));
	g_assert_cmpstr(json_object_get_string_member(
		json_node_get_object(result->data), "value_column"), ==, "revenue");

	/* A column the report does not have is said so. */
	g_clear_pointer(&result, venture_widget_result_free);
	g_object_set(chart, "field", "moonshine", NULL);
	result = venture_dashboard_render_widget(fixture->context, chart, NULL);
	g_assert_nonnull(result->error);
	g_assert_nonnull(strstr(result->error, "moonshine"));
}

/*
 * A widget for a module that is off says so in place; an unknown kind
 * too. Neither takes the page down.
 */
static void
test_dashboard_widget_off_and_unknown(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) environments = NULL;
	g_autoptr(VentureDashboardWidget) list = NULL;
	g_autoptr(VentureWidgetResult) result = NULL;

	(void)user_data;

	dashboard = create_dashboard(fixture, "Off");
	venture_config_set_module_enabled(fixture->config, "factory", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "factory"));

	environments = create_widget(fixture, dashboard, "environments", NULL);
	result = venture_dashboard_render_widget(fixture->context, environments,
	                                         NULL);
	g_assert_nonnull(result->error);
	g_assert_nonnull(strstr(result->error, "factory"));
	g_assert_cmpstr(result->title, ==, "Environments");

	g_clear_pointer(&result, venture_widget_result_free);
	list = create_widget(fixture, dashboard, "list",
	                     "entity-type", "release", "title", "Shipped", NULL);
	result = venture_dashboard_render_widget(fixture->context, list, NULL);
	g_assert_nonnull(result->error);
	g_assert_nonnull(strstr(result->error, "factory"));
	g_assert_cmpstr(result->title, ==, "Shipped");

	/* The kind is checked at the save, so an unknown one can only get
	 * here by the catalogue changing under a stored row. */
	g_object_set(list, "kind", "vanished", NULL);
	g_clear_pointer(&result, venture_widget_result_free);
	result = venture_dashboard_render_widget(fixture->context, list, NULL);
	g_assert_nonnull(result->error);
	g_assert_nonnull(strstr(result->error, "vanished"));
	g_assert_true(JSON_NODE_HOLDS_NULL(result->data));
}

/*
 * Only one dashboard is home; marking one unmarks the rest, whichever
 * door the write came through.
 */
static void
test_dashboard_one_home(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) first = NULL;
	g_autoptr(VentureDashboard) second = NULL;
	g_autoptr(VentureDashboard) home = NULL;
	g_autoptr(VentureEntity) reloaded = NULL;
	gboolean is_home = TRUE;
	gint64 owner;

	(void)user_data;

	first = create_dashboard(fixture, "First");
	second = create_dashboard(fixture, "Second");

	g_assert_null(venture_dashboard_find_home(fixture->database, 0));

	g_object_set(first, "home", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(first), NULL, NULL));
	home = venture_dashboard_find_home(fixture->database, 0);
	g_assert_nonnull(home);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(home)), ==,
	                venture_entity_get_id(VENTURE_ENTITY(first)));

	g_object_set(second, "home", TRUE, NULL);
	{
		g_autoptr(GError) error = NULL;

		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(second), NULL,
		                                    &error));
		g_assert_no_error(error);
	}

	reloaded = venture_database_get(fixture->database, VENTURE_TYPE_DASHBOARD,
		venture_entity_get_id(VENTURE_ENTITY(first)), NULL);
	g_object_get(reloaded, "home", &is_home, NULL);
	g_assert_false(is_home);

	g_clear_object(&home);
	home = venture_dashboard_find_home(fixture->database, 0);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(home)), ==,
	                venture_entity_get_id(VENTURE_ENTITY(second)));

	/* A personal home is nobody else's home. */
	owner = create_user(fixture->database, "owner-of-second");
	g_object_set(second, "personal", TRUE, "owner-user-id", owner, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(second), NULL, NULL));
	g_clear_object(&home);
	home = venture_dashboard_find_home(fixture->database, owner + 1);
	g_assert_null(home);
	home = venture_dashboard_find_home(fixture->database, owner);
	g_assert_nonnull(home);
	g_assert_false(venture_dashboard_is_visible_to(second, owner + 1));
	g_assert_true(venture_dashboard_is_visible_to(second, owner));
}

/*
 * Every shipped template imports, an export re-imports to the same shape,
 * and importing twice yields two dashboards rather than a refusal.
 */
static void
test_dashboard_templates_and_export(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	const VentureDashboardTemplate *templates;
	g_autoptr(GError) error = NULL;
	gsize n_templates;
	gsize i;
	gint64 owner;

	(void)user_data;

	templates = venture_dashboard_get_templates(&n_templates);
	g_assert_cmpuint(n_templates, >=, 4);
	owner = create_user(fixture->database, "importer");

	for (i = 0; i < n_templates; i++)
	{
		g_autoptr(VentureDashboard) dashboard = NULL;
		g_autoptr(VentureDashboard) again = NULL;
		g_autoptr(VentureDashboard) copy = NULL;
		g_autoptr(JsonNode) exported = NULL;
		g_autoptr(JsonNode) re_exported = NULL;
		g_autoptr(GPtrArray) widgets = NULL;
		g_autoptr(GPtrArray) copied = NULL;
		g_autofree gchar *slug = NULL;
		g_autofree gchar *again_slug = NULL;

		dashboard = venture_dashboard_create_from_template(fixture->context,
			templates[i].name, owner, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(dashboard);

		widgets = venture_dashboard_list_widgets(fixture->database,
			venture_entity_get_id(VENTURE_ENTITY(dashboard)), &error);
		g_assert_no_error(error);
		g_assert_cmpuint(widgets->len, >, 3);

		/* Positions follow the file's order. */
		{
			gint64 first = 0;
			gint64 second = 0;

			g_object_get(g_ptr_array_index(widgets, 0), "position", &first,
			             NULL);
			g_object_get(g_ptr_array_index(widgets, 1), "position", &second,
			             NULL);
			g_assert_cmpint(first, <, second);
		}

		exported = venture_dashboard_export(fixture->database, dashboard,
		                                    &error);
		g_assert_no_error(error);
		g_assert_false(json_object_has_member(json_node_get_object(exported),
		                                      "id"));
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
			json_node_get_object(exported), "widgets")), ==, widgets->len);

		copy = venture_dashboard_import(fixture->context, exported, owner,
		                                NULL, &error);
		g_assert_no_error(error);
		copied = venture_dashboard_list_widgets(fixture->database,
			venture_entity_get_id(VENTURE_ENTITY(copy)), NULL);
		g_assert_cmpuint(copied->len, ==, widgets->len);

		re_exported = venture_dashboard_export(fixture->database, copy, NULL);
		{
			g_autofree gchar *a = NULL;
			g_autofree gchar *b = NULL;

			/* Same shape apart from the slug, which had to differ. */
			json_object_set_string_member(json_node_get_object(exported),
			                              "slug", "x");
			json_object_set_string_member(json_node_get_object(re_exported),
			                              "slug", "x");
			a = venture_json_to_string(exported, FALSE);
			b = venture_json_to_string(re_exported, FALSE);
			g_assert_cmpstr(a, ==, b);
		}

		g_object_get(dashboard, "slug", &slug, NULL);
		g_object_get(copy, "slug", &again_slug, NULL);
		g_assert_cmpstr(slug, !=, again_slug);
		g_assert_true(g_str_has_prefix(again_slug, slug));

		again = venture_dashboard_create_from_template(fixture->context,
			templates[i].name, owner, NULL, &error);
		g_assert_no_error(error);
	}

	{
		g_autoptr(VentureDashboard) none = NULL;

		none = venture_dashboard_create_from_template(fixture->context,
			"no-such-template", 0, NULL, &error);
		g_assert_null(none);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
		g_clear_error(&error);
	}

	/* A definition with a widget the validator refuses leaves nothing
	 * behind. */
	{
		g_autoptr(JsonNode) bad = NULL;
		g_autoptr(VentureDashboard) none = NULL;
		g_autoptr(VentureDashboard) ghost = NULL;

		bad = venture_json_parse(
			"{\"name\": \"Broken\", \"widgets\": [{\"kind\": \"note\"},"
			" {\"kind\": \"list\", \"entity_type\": \"unicorn\"}]}", NULL);
		none = venture_dashboard_import(fixture->context, bad, 0, NULL,
		                                &error);
		g_assert_null(none);
		g_assert_nonnull(error);
		g_assert_nonnull(strstr(error->message, "unicorn"));
		g_clear_error(&error);

		ghost = venture_dashboard_find_by_slug(fixture->database, "broken",
		                                       NULL);
		g_assert_null(ghost);
	}

	/* A definition naming a setting that does not exist is refused, so a
	 * misspelt one in a file cannot silently do nothing. */
	{
		g_autoptr(JsonNode) bad = NULL;
		g_autoptr(VentureDashboard) none = NULL;

		bad = venture_json_parse(
			"{\"name\": \"Typo\", \"widgets\": [{\"kind\": \"note\","
			" \"bodyy\": \"x\"}]}", NULL);
		none = venture_dashboard_import(fixture->context, bad, 0, NULL,
		                                &error);
		g_assert_null(none);
		g_assert_nonnull(strstr(error->message, "bodyy"));
		g_clear_error(&error);
	}
}

/*
 * Moving a widget swaps it with its neighbour, normalising positions
 * that were never set first -- the first move on a fresh page must do
 * what it looks like.
 */
static void
test_dashboard_move_widget(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) a = NULL;
	g_autoptr(VentureDashboardWidget) b = NULL;
	g_autoptr(VentureDashboardWidget) c = NULL;
	g_autoptr(GPtrArray) widgets = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	dashboard = create_dashboard(fixture, "Moving");
	a = create_widget(fixture, dashboard, "note", "title", "A", NULL);
	b = create_widget(fixture, dashboard, "note", "title", "B", NULL);
	c = create_widget(fixture, dashboard, "note", "title", "C", NULL);

	g_assert_true(venture_dashboard_move_widget(fixture->database, c, -1, NULL,
	                                            &error));
	g_assert_no_error(error);

	widgets = venture_dashboard_list_widgets(fixture->database,
		venture_entity_get_id(VENTURE_ENTITY(dashboard)), NULL);
	g_assert_cmpuint(widgets->len, ==, 3);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(widgets, 0)), ==,
	                venture_entity_get_id(VENTURE_ENTITY(a)));
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(widgets, 1)), ==,
	                venture_entity_get_id(VENTURE_ENTITY(c)));
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(widgets, 2)), ==,
	                venture_entity_get_id(VENTURE_ENTITY(b)));

	/* Moving the first one up has nowhere to go and is not an error. */
	g_assert_true(venture_dashboard_move_widget(fixture->database, a, -1, NULL,
	                                            &error));
	g_assert_no_error(error);
}

/*
 * The grid. Placed widgets keep their cells; unplaced ones flow into the
 * first free cells; a placement that overlaps or falls off the edge is
 * treated as unplaced rather than breaking the page; placing is refused
 * onto a taken cell; nudging moves by one; arranging closes gaps.
 *
 * What breaks if this regresses: two cards drawn on top of each other,
 * or a page that stops rendering because a layout lost a column.
 */
static const VentureWidgetPlacement *
placement_of(
	GPtrArray		*placements,
	VentureDashboardWidget	*widget
){
	guint i;

	for (i = 0; i < placements->len; i++)
	{
		const VentureWidgetPlacement *placement;

		placement = g_ptr_array_index(placements, i);

		if (venture_entity_get_id(VENTURE_ENTITY(placement->widget)) ==
		    venture_entity_get_id(VENTURE_ENTITY(widget)))
			return placement;
	}

	g_error("widget %" G_GINT64_FORMAT " is not in the layout",
	        venture_entity_get_id(VENTURE_ENTITY(widget)));

	return NULL;
}

static void
test_dashboard_grid_layout(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) a = NULL;
	g_autoptr(VentureDashboardWidget) b = NULL;
	g_autoptr(VentureDashboardWidget) c = NULL;
	g_autoptr(VentureDashboardWidget) d = NULL;
	g_autoptr(GPtrArray) placements = NULL;
	g_autoptr(GError) error = NULL;
	const VentureWidgetPlacement *placement;

	(void)user_data;

	dashboard = create_dashboard(fixture, "Grid");

	/* Three columns. a is wide and unplaced, b is a single unplaced, c
	 * asks for column 3 row 1 and gets it, d is full width. */
	a = create_widget(fixture, dashboard, "note", "title", "A",
	                  "span", VENTURE_WIDGET_SPAN_WIDE, NULL);
	b = create_widget(fixture, dashboard, "note", "title", "B", NULL);
	c = create_widget(fixture, dashboard, "note", "title", "C",
	                  "grid-col", (gint64)3, "grid-row", (gint64)1, NULL);
	d = create_widget(fixture, dashboard, "note", "title", "D",
	                  "span", VENTURE_WIDGET_SPAN_FULL, NULL);

	placements = venture_dashboard_layout(fixture->database, dashboard,
	                                      &error);
	g_assert_no_error(error);
	g_assert_cmpuint(placements->len, ==, 4);

	placement = placement_of(placements, c);
	g_assert_true(placement->placed);
	g_assert_cmpuint(placement->col, ==, 3);
	g_assert_cmpuint(placement->row, ==, 1);

	/* a flows into row 1 columns 1-2, beside c. */
	placement = placement_of(placements, a);
	g_assert_false(placement->placed);
	g_assert_cmpuint(placement->col, ==, 1);
	g_assert_cmpuint(placement->row, ==, 1);
	g_assert_cmpuint(placement->width, ==, 2);

	/* b goes to the first free cell, row 2 column 1; d needs a whole
	 * row and takes row 3. */
	placement = placement_of(placements, b);
	g_assert_cmpuint(placement->row, ==, 2);
	g_assert_cmpuint(placement->col, ==, 1);
	placement = placement_of(placements, d);
	g_assert_cmpuint(placement->row, ==, 3);
	g_assert_cmpuint(placement->width, ==, 3);

	/* The layout is sorted by row then column. */
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(
		((VentureWidgetPlacement *)g_ptr_array_index(placements, 0))->widget)),
		==, venture_entity_get_id(VENTURE_ENTITY(a)));

	/* Placing b where c is: refused, naming c. */
	g_assert_false(venture_dashboard_place_widget(fixture->database, dashboard,
		b, 3, 1, 1, 1, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_nonnull(strstr(error->message, "C"));
	g_clear_error(&error);

	/* Off the edge: refused. */
	g_assert_false(venture_dashboard_place_widget(fixture->database, dashboard,
		b, 3, 2, 2, 1, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* Free: placed and saved. */
	g_assert_true(venture_dashboard_place_widget(fixture->database, dashboard,
		b, 2, 2, 2, 2, NULL, &error));
	g_assert_no_error(error);
	g_clear_pointer(&placements, g_ptr_array_unref);
	placements = venture_dashboard_layout(fixture->database, dashboard, NULL);
	placement = placement_of(placements, b);
	g_assert_true(placement->placed);
	g_assert_cmpuint(placement->col, ==, 2);
	g_assert_cmpuint(placement->height, ==, 2);

	/* d, unplaced and full width, now flows below b's two rows. */
	placement = placement_of(placements, d);
	g_assert_cmpuint(placement->row, ==, 4);

	/* A stored placement that overlaps -- written straight to the row,
	 * as the API could -- is treated as unplaced, and the page still
	 * has one card per cell. */
	g_object_set(c, "grid-col", (gint64)1, "grid-row", (gint64)1, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(c),
	                                    NULL, NULL));
	g_clear_pointer(&placements, g_ptr_array_unref);
	placements = venture_dashboard_layout(fixture->database, dashboard, NULL);
	placement = placement_of(placements, c);
	g_assert_true(placement->placed);
	placement = placement_of(placements, a);
	g_assert_false(placement->placed);
	g_assert_true((placement->row != 1) || (placement->col == 3) ||
	              (placement->col > 1));
	{
		guint i;
		guint j;

		for (i = 0; i < placements->len; i++)
		{
			const VentureWidgetPlacement *p;

			p = g_ptr_array_index(placements, i);

			for (j = i + 1; j < placements->len; j++)
			{
				const VentureWidgetPlacement *q;

				q = g_ptr_array_index(placements, j);
				g_assert_false((p->col < q->col + q->width) &&
				               (q->col < p->col + p->width) &&
				               (p->row < q->row + q->height) &&
				               (q->row < p->row + p->height));
			}
		}
	}

	/* Nudging. b up lands on cells a merely flowed into, which is
	 * allowed -- a flows on; b left would land on c, which asked for
	 * its cell, and is refused; c left off the edge does nothing. */
	g_assert_true(venture_dashboard_nudge_widget(fixture->database, dashboard,
		b, "up", NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_dashboard_nudge_widget(fixture->database, dashboard,
		b, "left", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_nonnull(strstr(error->message, "C"));
	g_clear_error(&error);
	g_assert_true(venture_dashboard_nudge_widget(fixture->database, dashboard,
		c, "left", NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_dashboard_nudge_widget(fixture->database, dashboard,
		c, "sideways", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* The layout losing a column: a placed column-3 widget no longer
	 * fits and flows; nothing is lost. */
	g_object_set(dashboard, "layout", VENTURE_DASHBOARD_LAYOUT_TWO_COLUMNS,
	             NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(dashboard), NULL, NULL));
	g_object_set(c, "grid-col", (gint64)3, "grid-row", (gint64)1, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(c),
	                                    NULL, NULL));
	g_clear_pointer(&placements, g_ptr_array_unref);
	placements = venture_dashboard_layout(fixture->database, dashboard, NULL);
	g_assert_cmpuint(placements->len, ==, 4);
	placement = placement_of(placements, c);
	g_assert_false(placement->placed);
	g_assert_cmpuint(placement->col + placement->width - 1, <=, 2);
	placement = placement_of(placements, d);
	g_assert_cmpuint(placement->width, ==, 2);

	/* Tidy: everything written down in flow order, no gaps. */
	g_assert_true(venture_dashboard_arrange(fixture->database, dashboard, NULL,
	                                        &error));
	g_assert_no_error(error);
	g_clear_pointer(&placements, g_ptr_array_unref);
	placements = venture_dashboard_layout(fixture->database, dashboard, NULL);
	{
		guint i;

		for (i = 0; i < placements->len; i++)
			g_assert_true(((VentureWidgetPlacement *)
				g_ptr_array_index(placements, i))->placed);
	}
	placement = placement_of(placements, a);
	g_assert_cmpuint(placement->row, ==, 1);
	g_assert_cmpuint(placement->col, ==, 1);
}

/* --- Over HTTP ------------------------------------------------------------ */

typedef struct
{
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gchar			*state_dir;
	gchar			*cookie;
	guint16			 port;
} ServerFixture;

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	RequestResult *outcome;

	outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                  result, &outcome->error);
	outcome->done = TRUE;
}

static guint
server_request_full(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *content_type,
	const gchar	 *body,
	gboolean	  with_cookie,
	gchar		**out_body,
	gchar		**out_location
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (with_cookie && (NULL != fixture->cookie))
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie",
			fixture->cookie);

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL, request_done,
	                                 &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	if (NULL != out_location)
		*out_location = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Location"));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

static guint
server_get(
	ServerFixture	 *fixture,
	const gchar	 *path,
	gchar		**out_body
){
	return server_request_full(fixture, "GET", path, NULL, NULL, TRUE,
	                           out_body, NULL);
}

static guint
server_post_form(
	ServerFixture	 *fixture,
	const gchar	 *path,
	const gchar	 *form,
	gchar		**out_location
){
	return server_request_full(fixture, "POST", path,
	                           "application/x-www-form-urlencoded", form, TRUE,
	                           NULL, out_location);
}

static void
server_fixture_set_up(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	(void)user_data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "dashboard-test-secret", TRUE);

	fixture->state_dir = g_dir_make_tmp("venture-dashboard-XXXXXX", NULL);
	fixture->port = (guint16)(20000 + ((getpid() + 12289) % 20000));

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)fixture->port,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             "security-password-iterations", (gint64)100000,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);

	fixture->session = soup_session_new();

	user = venture_user_new();
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER,
	             "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 100000,
	                                        NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	{
		g_autoptr(SoupMessage) message = NULL;
		g_autofree gchar *url = NULL;
		g_autoptr(GBytes) bytes = NULL;
		RequestResult outcome = { FALSE, NULL, NULL };

		url = g_strdup_printf("http://127.0.0.1:%u/login", fixture->port);
		message = soup_message_new("POST", url);
		soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
		bytes = g_bytes_new_static("username=owner&password=owner-password-1",
		                           strlen("username=owner&password=owner-password-1"));
		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
		soup_session_send_and_read_async(fixture->session, message,
		                                 G_PRIORITY_DEFAULT, NULL,
		                                 request_done, &outcome);

		while (!outcome.done)
			g_main_context_iteration(NULL, TRUE);

		g_clear_pointer(&outcome.body, g_bytes_unref);
		g_clear_error(&outcome.error);

		set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));
	}

	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	fixture->cookie = g_steal_pointer(&set_cookie);
}

static void
server_fixture_tear_down(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_pointer(&fixture->cookie, g_free);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_unsetenv("VENTURE_TEST_SESSION_SECRET");

	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}

/*
 * The whole loop through the browser: create from a template, see it,
 * see its widgets evaluated, add a widget, move it, refresh one card,
 * export it, read it over the API, make it home, delete it.
 */
static void
test_dashboard_http_round_trip(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *location = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *dashboard;
	JsonArray *widgets;
	gint64 first_widget;

	(void)user_data;

	g_assert_cmpuint(server_get(fixture, "/dashboards", &page), ==,
	                 SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "No dashboards yet"));
	g_assert_nonnull(strstr(page, "value=\"factory\""));
	g_clear_pointer(&page, g_free);

	g_assert_cmpuint(server_post_form(fixture, "/dashboards",
	                                  "template=work", &location), ==,
	                 SOUP_STATUS_FOUND);
	g_assert_cmpstr(location, ==, "/dashboards/my-work");

	g_assert_cmpuint(server_get(fixture, "/dashboards/my-work", &page), ==,
	                 SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "<h1>My work</h1>"));
	g_assert_nonnull(strstr(page, "widget-grid cols-3"));
	g_assert_nonnull(strstr(page, "widget-count"));
	g_assert_nonnull(strstr(page, "widget-note"));
	/* The sidebar lists it under Views. */
	g_assert_nonnull(strstr(page, "<div class=\"nav-section\">Views</div>"));
	g_assert_nonnull(strstr(page, "href=\"/dashboards/my-work\""));
	g_clear_pointer(&page, g_free);

	g_assert_cmpuint(server_get(fixture, "/api/v1/dashboards/my-work", &body),
	                 ==, SOUP_STATUS_OK);
	node = venture_json_parse(body, &error);
	g_assert_no_error(error);
	dashboard = json_node_get_object(node);
	g_assert_cmpstr(json_object_get_string_member(dashboard, "slug"), ==,
	                "my-work");
	g_assert_true(json_object_get_boolean_member(dashboard, "personal"));
	widgets = json_object_get_array_member(dashboard, "widgets");
	g_assert_cmpuint(json_array_get_length(widgets), >, 5);
	{
		JsonObject *widget;

		widget = json_array_get_object_element(widgets, 0);
		first_widget = json_object_get_int_member(widget, "id");
		g_assert_cmpstr(json_object_get_string_member(widget, "kind"), ==,
		                "count");
		g_assert_true(json_object_has_member(widget, "data"));
		g_assert_cmpint(json_object_get_int_member(
			json_object_get_object_member(widget, "data"), "count"), ==, 0);
		g_assert_true(json_object_get_null_member(widget, "error"));
	}
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&node, json_node_unref);

	/* One card, as the refresh fetches it. */
	{
		g_autofree gchar *path = NULL;

		path = g_strdup_printf("/dashboards/my-work/widgets/%" G_GINT64_FORMAT,
		                       first_widget);
		g_assert_cmpuint(server_get(fixture, path, &page), ==, SOUP_STATUS_OK);
		g_assert_true(g_str_has_prefix(page, "<div class=\"card dash-card "
		                                     "widget widget-count"));
		g_assert_null(strstr(page, "<html"));
		g_clear_pointer(&page, g_free);
	}

	/* Add a widget through the form, then move it up. */
	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(server_post_form(fixture, "/dashboards/my-work/widgets",
		"kind=list&entity_type=ticket&title=Newest&order=-id&limit=3&span=wide",
		&location), ==, SOUP_STATUS_FOUND);
	g_assert_cmpstr(location, ==, "/dashboards/my-work/edit");

	g_assert_cmpuint(server_get(fixture, "/dashboards/my-work/edit", &page),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "<h2>Newest</h2>"));
	g_assert_nonnull(strstr(page, "span-wide"));
	g_assert_nonnull(strstr(page, "Save settings"));
	g_clear_pointer(&page, g_free);

	g_assert_cmpuint(server_get(fixture, "/api/v1/dashboards/my-work?data=0",
	                            &body), ==, SOUP_STATUS_OK);
	node = venture_json_parse(body, NULL);
	widgets = json_object_get_array_member(json_node_get_object(node),
	                                       "widgets");
	{
		JsonObject *last;
		g_autofree gchar *path = NULL;
		g_autoptr(JsonNode) moved = NULL;
		g_autofree gchar *moved_body = NULL;
		g_autofree gchar *editor_check = NULL;
		JsonArray *after;

		last = json_array_get_object_element(widgets,
			json_array_get_length(widgets) - 1);
		g_assert_cmpstr(json_object_get_string_member(last, "title"), ==,
		                "Newest");
		g_assert_false(json_object_has_member(last, "data"));

		/* Placed by the drag's route onto a free cell, the widget reads
		 * back with the cell; onto a taken one, it is refused with the
		 * other widget named, and nothing moves. */
		path = g_strdup_printf("/dashboards/my-work/widgets/%" G_GINT64_FORMAT
		                       "/place", json_object_get_int_member(last, "id"));
		{
			g_autofree gchar *first_path = NULL;

			/* The first widget claims its cell; a flowed neighbour
			 * would not have blocked anything. */
			first_path = g_strdup_printf("/dashboards/my-work/widgets/%"
			                             G_GINT64_FORMAT "/place", first_widget);
			g_clear_pointer(&location, g_free);
			g_assert_cmpuint(server_post_form(fixture, first_path,
				"col=1&row=1", &location), ==, SOUP_STATUS_FOUND);
		}

		g_clear_pointer(&location, g_free);
		g_assert_cmpuint(server_post_form(fixture, path,
			"col=1&row=1&width=1&height=1", &location), ==, 409);
		g_assert_cmpuint(server_post_form(fixture, path,
			"col=2&row=9&async=1", &location), ==, SOUP_STATUS_OK);

		g_assert_cmpuint(server_get(fixture,
			"/api/v1/dashboards/my-work?data=0", &moved_body), ==,
			SOUP_STATUS_OK);
		moved = venture_json_parse(moved_body, NULL);
		after = json_object_get_array_member(json_node_get_object(moved),
		                                     "widgets");
		{
			guint i;
			gboolean seen = FALSE;

			for (i = 0; i < json_array_get_length(after); i++)
			{
				JsonObject *w;

				w = json_array_get_object_element(after, i);

				if (0 == g_strcmp0(json_object_get_string_member(w, "title"),
				                   "Newest"))
				{
					seen = TRUE;
					g_assert_cmpint(json_object_get_int_member(w, "grid_col"),
					                ==, 2);
					g_assert_cmpint(json_object_get_int_member(w, "grid_row"),
					                ==, 9);
				}
			}

			g_assert_true(seen);
		}

		/* And nudged, as the buttons do: wider would run off the
		 * three-column edge and is refused; taller is fine. */
		g_clear_pointer(&path, g_free);
		path = g_strdup_printf("/dashboards/my-work/widgets/%" G_GINT64_FORMAT
		                       "/move", json_object_get_int_member(last, "id"));
		g_clear_pointer(&location, g_free);
		g_assert_cmpuint(server_post_form(fixture, path, "direction=wider",
		                                  &location), ==, 422);
		g_clear_pointer(&location, g_free);
		g_assert_cmpuint(server_post_form(fixture, path, "direction=taller",
		                                  &location), ==, SOUP_STATUS_FOUND);
		g_assert_cmpstr(location, ==, "/dashboards/my-work/edit");

		/* The editor draws the cells and the cards' places. */
		{
			g_autofree gchar *editor = NULL;

			g_assert_cmpuint(server_get(fixture, "/dashboards/my-work/edit",
			                            &editor), ==, SOUP_STATUS_OK);
			g_assert_nonnull(strstr(editor, "data-grid-editor"));
			g_assert_nonnull(strstr(editor, "data-cell data-col=\"1\" "
			                                "data-row=\"1\""));
			g_assert_nonnull(strstr(editor, "grid-column:2 / span 2;"
			                                "grid-row:9 / span 2"));
			g_assert_nonnull(strstr(editor, "Tidy"));
		}

		/* Tidy closes the gap row 9 left. */
		g_clear_pointer(&location, g_free);
		g_assert_cmpuint(server_post_form(fixture, "/dashboards/my-work/arrange",
		                                  "", &location), ==, SOUP_STATUS_FOUND);
		g_clear_pointer(&editor_check, g_free);
		g_assert_cmpuint(server_get(fixture, "/dashboards/my-work",
		                            &editor_check), ==, SOUP_STATUS_OK);
		g_assert_null(strstr(editor_check, "grid-row:9 "));
	}
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&node, json_node_unref);

	/* The export is the definition, as a download. */
	g_assert_cmpuint(server_get(fixture, "/dashboards/my-work/export", &body),
	                 ==, SOUP_STATUS_OK);
	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);
	g_assert_false(json_object_has_member(json_node_get_object(node), "id"));
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&node, json_node_unref);

	/* Making it home puts it at /, with the built-in still at /overview. */
	g_assert_cmpuint(server_get(fixture, "/", &page), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "<h1>Dashboard</h1>"));
	g_clear_pointer(&page, g_free);

	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(server_post_form(fixture, "/dashboards/my-work",
		"name=My+work&slug=my-work&purpose=work&layout=two_columns&home=true"
		"&personal=true", &location), ==, SOUP_STATUS_FOUND);

	g_assert_cmpuint(server_get(fixture, "/", &page), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "<h1>My work</h1>"));
	g_assert_nonnull(strstr(page, "widget-grid cols-2"));
	g_assert_nonnull(strstr(page, "href=\"/overview\""));
	g_clear_pointer(&page, g_free);

	g_assert_cmpuint(server_get(fixture, "/overview", &page), ==,
	                 SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "<h1>Dashboard</h1>"));
	g_clear_pointer(&page, g_free);

	/* The catalogue and the templates, over the API. */
	g_assert_cmpuint(server_get(fixture, "/api/v1/widget-kinds", &body), ==,
	                 SOUP_STATUS_OK);
	node = venture_json_parse(body, NULL);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), >=, 16);
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&node, json_node_unref);

	g_assert_cmpuint(server_get(fixture, "/api/v1/dashboard-templates", &body),
	                 ==, SOUP_STATUS_OK);
	node = venture_json_parse(body, NULL);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), >=, 4);
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&node, json_node_unref);

	/* From a template over the API, and the list. */
	g_assert_cmpuint(server_request_full(fixture, "POST",
		"/api/v1/dashboards/from-template", "application/json",
		"{\"template\": \"factory\"}", TRUE, &body, NULL), ==,
		SOUP_STATUS_CREATED);
	node = venture_json_parse(body, NULL);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(node),
	                                              "slug"), ==, "factory");
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&node, json_node_unref);

	g_assert_cmpuint(server_request_full(fixture, "POST",
		"/api/v1/dashboards/nonsense", "application/json", "{}", TRUE, NULL,
		NULL), ==, SOUP_STATUS_NOT_FOUND);

	g_assert_cmpuint(server_get(fixture, "/api/v1/dashboards", &body), ==,
	                 SOUP_STATUS_OK);
	node = venture_json_parse(body, NULL);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), ==, 2);
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&node, json_node_unref);

	/* The factory page renders with its module on, every widget
	 * answering; and with it off, the factory widgets say so and the
	 * rest still work. */
	g_assert_cmpuint(server_get(fixture, "/dashboards/factory", &page), ==,
	                 SOUP_STATUS_OK);
	g_assert_null(strstr(page, "which is off"));
	/* The rendered element, not the two words: the stylesheet and the
	 * script are inlined into every page, and either may mention a
	 * class name without anything on the page carrying it. */
	g_assert_null(strstr(page, "<div class=\"notice negative\">"));
	g_assert_nonnull(strstr(page, "widget-environments"));
	g_clear_pointer(&page, g_free);

	venture_config_set_module_enabled(fixture->config, "factory", FALSE);
	g_assert_cmpuint(server_get(fixture, "/dashboards/factory", &page), ==,
	                 SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "which is off"));
	g_assert_nonnull(strstr(page, "widget-actions"));
	g_clear_pointer(&page, g_free);
	venture_config_set_module_enabled(fixture->config, "factory", TRUE);

	/* And gone. */
	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(server_post_form(fixture, "/dashboards/factory/delete",
	                                  "", &location), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_get(fixture, "/dashboards/factory", NULL), ==,
	                 SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_get(fixture, "/api/v1/dashboards/factory", NULL),
	                 ==, SOUP_STATUS_NOT_FOUND);
}

/*
 * Somebody else's personal dashboard does not exist, as far as the
 * viewer can tell: NOT_FOUND on the page, the API and the fragment.
 */
static void
test_dashboard_http_personal_is_private(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) widget = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;

	(void)user_data;

	dashboard = venture_dashboard_new();
	g_object_set(dashboard, "name", "Theirs", "personal", TRUE,
	             "owner-user-id", create_user(fixture->database, "them"),
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(dashboard),
		venture_context_get_default_organization_id(fixture->context));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(dashboard), NULL, NULL));

	widget = venture_dashboard_widget_new();
	g_object_set(widget, "dashboard-id",
	             venture_entity_get_id(VENTURE_ENTITY(dashboard)),
	             "kind", "note", "body", "secret plans", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(widget), NULL, NULL));

	g_assert_cmpuint(server_get(fixture, "/dashboards/theirs", NULL), ==,
	                 SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_get(fixture, "/api/v1/dashboards/theirs", NULL),
	                 ==, SOUP_STATUS_NOT_FOUND);
	path = g_strdup_printf("/dashboards/theirs/widgets/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(VENTURE_ENTITY(widget)));
	g_assert_cmpuint(server_get(fixture, path, NULL), ==,
	                 SOUP_STATUS_NOT_FOUND);

	g_assert_cmpuint(server_get(fixture, "/dashboards", &page), ==,
	                 SOUP_STATUS_OK);
	g_assert_null(strstr(page, "Theirs"));
	g_assert_null(strstr(page, "secret plans"));
}

/*
 * With the module off the pages answer 404, the API too, the sidebar
 * loses its entry, and / is the built-in overview whatever is marked
 * home.
 */
static void
test_dashboard_http_module_off(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *page = NULL;
	g_autofree gchar *location = NULL;

	(void)user_data;

	g_assert_cmpuint(server_post_form(fixture, "/dashboards",
	                                  "template=overview", &location), ==,
	                 SOUP_STATUS_FOUND);
	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(server_post_form(fixture, "/dashboards/overview",
		"name=Overview&home=true", &location), ==, SOUP_STATUS_FOUND);

	g_assert_cmpuint(server_get(fixture, "/", &page), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "<h1>Overview</h1>"));
	g_clear_pointer(&page, g_free);

	venture_config_set_module_enabled(fixture->config, "dashboards", FALSE);

	g_assert_cmpuint(server_get(fixture, "/dashboards", NULL), ==,
	                 SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_get(fixture, "/dashboards/overview", NULL), ==,
	                 SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_get(fixture, "/api/v1/dashboards", NULL), ==,
	                 SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_get(fixture, "/api/v1/dashboard", NULL), ==,
	                 SOUP_STATUS_NOT_FOUND);

	g_assert_cmpuint(server_get(fixture, "/", &page), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "<h1>Dashboard</h1>"));
	g_assert_null(strstr(page, "href=\"/dashboards\""));
	g_clear_pointer(&page, g_free);

	venture_config_set_module_enabled(fixture->config, "dashboards", TRUE);

	g_assert_cmpuint(server_get(fixture, "/", &page), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "<h1>Overview</h1>"));
}

/*
 * Two cards trade places.
 *
 * The gesture exists because a placement refuses a taken cell, and a
 * tidy page has no spare cell to move a card through: without a swap
 * the only way to reorder a full grid is to shuffle everything to the
 * bottom and back. Both writes are one transaction, or the first would
 * land on cells the second still holds.
 *
 * What breaks if this regresses: a full dashboard that cannot be
 * rearranged at all, or a swap that half-applies and leaves two cards
 * on one cell.
 */
static void
test_dashboard_swap(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) a = NULL;
	g_autoptr(VentureDashboardWidget) b = NULL;
	g_autoptr(VentureDashboardWidget) wide = NULL;
	g_autoptr(GPtrArray) placements = NULL;
	g_autoptr(GError) error = NULL;
	const VentureWidgetPlacement *placement;

	(void)user_data;

	dashboard = create_dashboard(fixture, "Swap");

	a = create_widget(fixture, dashboard, "note", "title", "A",
	                  "grid-col", (gint64)1, "grid-row", (gint64)1, NULL);
	b = create_widget(fixture, dashboard, "note", "title", "B",
	                  "grid-col", (gint64)3, "grid-row", (gint64)2, NULL);
	wide = create_widget(fixture, dashboard, "note", "title", "Wide",
	                     "grid-col", (gint64)1, "grid-row", (gint64)3,
	                     "grid-width", (gint64)2, NULL);

	g_assert_true(venture_dashboard_swap_widgets(fixture->database, dashboard,
	                                             a, b, NULL, &error));
	g_assert_no_error(error);

	placements = venture_dashboard_layout(fixture->database, dashboard, NULL);
	placement = placement_of(placements, a);
	g_assert_cmpuint(placement->col, ==, 3);
	g_assert_cmpuint(placement->row, ==, 2);
	placement = placement_of(placements, b);
	g_assert_cmpuint(placement->col, ==, 1);
	g_assert_cmpuint(placement->row, ==, 1);

	/* Both are still placed, and nothing landed on top of anything. */
	g_assert_true(placement_of(placements, a)->placed);
	g_assert_true(placement_of(placements, b)->placed);

	/* Swapping back returns them, so the operation is its own inverse. */
	g_assert_true(venture_dashboard_swap_widgets(fixture->database, dashboard,
	                                             b, a, NULL, &error));
	g_clear_pointer(&placements, g_ptr_array_unref);
	placements = venture_dashboard_layout(fixture->database, dashboard, NULL);
	g_assert_cmpuint(placement_of(placements, a)->col, ==, 1);
	g_assert_cmpuint(placement_of(placements, b)->col, ==, 3);

	/* Different sizes are not an exchange: one of them would have to
	 * end up somewhere neither card asked for. */
	g_assert_false(venture_dashboard_swap_widgets(fixture->database, dashboard,
	                                              a, wide, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_nonnull(strstr(error->message, "same size"));
	g_clear_error(&error);

	/* And the refusal changed nothing. */
	g_clear_pointer(&placements, g_ptr_array_unref);
	placements = venture_dashboard_layout(fixture->database, dashboard, NULL);
	g_assert_cmpuint(placement_of(placements, a)->col, ==, 1);
	g_assert_cmpuint(placement_of(placements, wide)->row, ==, 3);

	/* A widget cannot trade with itself. */
	g_assert_false(venture_dashboard_swap_widgets(fixture->database, dashboard,
	                                              a, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/dashboard/module-and-kinds",
	                test_dashboard_module_and_kinds);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/dashboard/plugin-kind", test_dashboard_plugin_kind);
	ADD("/dashboard/slug", test_dashboard_slug);
	ADD("/dashboard/widget-validation", test_dashboard_widget_validation);
	ADD("/dashboard/count-list-breakdown",
	    test_dashboard_count_list_breakdown);
	ADD("/dashboard/note-and-actions", test_dashboard_note_and_actions);
	ADD("/dashboard/report-kinds", test_dashboard_report_kinds);
	ADD("/dashboard/widget-off-and-unknown",
	    test_dashboard_widget_off_and_unknown);
	ADD("/dashboard/one-home", test_dashboard_one_home);
	ADD("/dashboard/templates-and-export",
	    test_dashboard_templates_and_export);
	ADD("/dashboard/move-widget", test_dashboard_move_widget);
	ADD("/dashboard/grid-layout", test_dashboard_grid_layout);
	ADD("/dashboard/swap", test_dashboard_swap);

#undef ADD
#define ADD(path, func) \
	g_test_add(path, ServerFixture, NULL, server_fixture_set_up, func, \
	           server_fixture_tear_down)

	ADD("/dashboard/http/round-trip", test_dashboard_http_round_trip);
	ADD("/dashboard/http/personal-is-private",
	    test_dashboard_http_personal_is_private);
	ADD("/dashboard/http/module-off", test_dashboard_http_module_off);

	return g_test_run();
}
