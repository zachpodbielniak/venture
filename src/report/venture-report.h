/*
 * venture-report.h - The reporting engine
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A report turns a period and some options into headline metrics and a table
 * of rows. #VentureReportResult is the single shape every report produces,
 * which is what lets the web UI, the CLI, the AI and the CSV exporter render
 * any report -- including one a plugin added this morning -- without knowing
 * anything about it.
 *
 * There are two ways to add a report. A built-in registers a function, which
 * is the whole of what a report usually is. A plugin can instead subclass
 * #VentureReport when it needs state or wants to override how its parameters
 * are described. Both end up in the same registry and are indistinguishable
 * to every consumer.
 */

#ifndef VENTURE_REPORT_H
#define VENTURE_REPORT_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* --- Results ------------------------------------------------------------- */

#define VENTURE_TYPE_REPORT_RESULT (venture_report_result_get_type())

G_DECLARE_FINAL_TYPE(VentureReportResult, venture_report_result,
                     VENTURE, REPORT_RESULT, GObject)

/**
 * VentureReportColumnKind:
 * @VENTURE_REPORT_COLUMN_TEXT: a label
 * @VENTURE_REPORT_COLUMN_NUMBER: a plain number
 * @VENTURE_REPORT_COLUMN_MONEY: a monetary amount
 * @VENTURE_REPORT_COLUMN_PERCENT: a ratio rendered as a percentage
 * @VENTURE_REPORT_COLUMN_DATE: a date
 *
 * How a report column should be formatted and aligned. Numeric kinds are
 * right-aligned and use tabular figures, which is what makes a column of
 * money legible.
 */
typedef enum
{
	VENTURE_REPORT_COLUMN_TEXT = 0,
	VENTURE_REPORT_COLUMN_NUMBER,
	VENTURE_REPORT_COLUMN_MONEY,
	VENTURE_REPORT_COLUMN_PERCENT,
	VENTURE_REPORT_COLUMN_DATE
} VentureReportColumnKind;

#define VENTURE_TYPE_REPORT_COLUMN_KIND (venture_report_column_kind_get_type())

GType
venture_report_column_kind_get_type(void) G_GNUC_CONST;

/**
 * venture_report_result_new:
 * @title: the report's title, already resolved for the period
 * @period: (nullable): the period covered
 *
 * Returns: (transfer full): a new result
 */
VentureReportResult *
venture_report_result_new(
	const gchar		*title,
	const VentureDateRange	*period
);

/**
 * venture_report_result_add_column:
 * @self: a #VentureReportResult
 * @key: the machine key
 * @label: the human label
 * @kind: how to format it
 */
void
venture_report_result_add_column(
	VentureReportResult	*self,
	const gchar		*key,
	const gchar		*label,
	VentureReportColumnKind	 kind
);

/**
 * venture_report_result_add_metric:
 * @self: a #VentureReportResult
 * @metric: (transfer full): a headline figure
 *
 * Adds a metric shown above the table as a tile.
 */
void
venture_report_result_add_metric(
	VentureReportResult	*self,
	VentureMetric		*metric
);

/**
 * venture_report_result_begin_row:
 * @self: a #VentureReportResult
 *
 * Starts a new row. Values are then set by key, so a report that gains a
 * column does not have to keep every row's positional list in step.
 */
void
venture_report_result_begin_row(VentureReportResult *self);

/**
 * venture_report_result_set_text:
 * @self: a #VentureReportResult
 * @key: the column key
 * @value: (nullable): the value
 */
void
venture_report_result_set_text(
	VentureReportResult	*self,
	const gchar		*key,
	const gchar		*value
);

/**
 * venture_report_result_set_money:
 * @self: a #VentureReportResult
 * @key: the column key
 * @value: (nullable): the amount
 */
void
venture_report_result_set_money(
	VentureReportResult	*self,
	const gchar		*key,
	const VentureMoney	*value
);

/**
 * venture_report_result_set_number:
 * @self: a #VentureReportResult
 * @key: the column key
 * @value: the value
 */
void
venture_report_result_set_number(
	VentureReportResult	*self,
	const gchar		*key,
	gdouble			 value
);

/**
 * venture_report_result_get_metrics:
 * @self: a #VentureReportResult
 *
 * Returns: (transfer none) (element-type VentureMetric): the headline figures
 */
GPtrArray *
venture_report_result_get_metrics(VentureReportResult *self);

/**
 * venture_report_result_get_row_count:
 * @self: a #VentureReportResult
 *
 * Returns: the number of rows
 */
guint
venture_report_result_get_row_count(VentureReportResult *self);

/**
 * venture_report_result_get_title:
 * @self: a #VentureReportResult
 *
 * Returns: (transfer none): the title
 */
const gchar *
venture_report_result_get_title(VentureReportResult *self);

/**
 * venture_report_result_set_note:
 * @self: a #VentureReportResult
 * @note: (nullable): a caveat displayed with the report
 *
 * Attaches a caveat. Used where a number needs one -- amounts excluded for
 * being in another currency, expenses still awaiting review -- so a figure
 * is never presented as more complete than it is.
 */
void
venture_report_result_set_note(
	VentureReportResult	*self,
	const gchar		*note
);

/**
 * venture_report_result_append_note:
 * @self: a #VentureReportResult
 * @note: a caveat displayed with the report
 *
 * Adds a caveat without displacing one already attached. A report can earn
 * more than one -- unreviewed expenses and excluded currencies at once --
 * and the reader needs both, not whichever was set last.
 */
void
venture_report_result_append_note(
	VentureReportResult	*self,
	const gchar		*note
);

/**
 * venture_report_result_render:
 * @self: a #VentureReportResult
 * @format: the output format
 *
 * Renders the result. Every format is produced from the same structure, so a
 * report is written once and appears correctly in the browser, the terminal,
 * a CSV export and an org file.
 *
 * Returns: (transfer full): the rendered report
 */
gchar *
venture_report_result_render(
	VentureReportResult	*self,
	VentureOutputFormat	 format
);

/**
 * venture_report_result_to_json:
 * @self: a #VentureReportResult
 *
 * Returns: (transfer full): the result as JSON
 */
JsonNode *
venture_report_result_to_json(VentureReportResult *self);

/* --- Reports ------------------------------------------------------------- */

#define VENTURE_TYPE_REPORT (venture_report_get_type())

G_DECLARE_DERIVABLE_TYPE(VentureReport, venture_report, VENTURE, REPORT, GObject)

/**
 * VentureReportClass:
 * @parent_class: the parent class
 * @generate: produce the result
 * @describe_parameters: describe the options the report accepts, as a JSON
 *   Schema object; used to build the AI tool and the UI's options form
 *
 * The vtable for #VentureReport.
 */
struct _VentureReportClass
{
	GObjectClass parent_class;

	VentureReportResult *(*generate) (VentureReport     *self,
	                                  VentureContext    *context,
	                                  VentureDateRange  *period,
	                                  JsonObject        *options,
	                                  GError           **error);

	JsonNode *(*describe_parameters) (VentureReport *self);

	/*< private >*/
	gpointer padding[6];
};

/**
 * venture_report_get_name:
 * @self: a #VentureReport
 *
 * Returns: (transfer none): the machine name, e.g. "pnl"
 */
const gchar *
venture_report_get_name(VentureReport *self);

/**
 * venture_report_get_title:
 * @self: a #VentureReport
 *
 * Returns: (transfer none): the human title
 */
const gchar *
venture_report_get_title(VentureReport *self);

/**
 * venture_report_get_description:
 * @self: a #VentureReport
 *
 * Returns: (transfer none): what the report answers, in a sentence
 */
const gchar *
venture_report_get_description(VentureReport *self);

/**
 * venture_report_generate:
 * @self: a #VentureReport
 * @context: the wiring
 * @period: (nullable): the period to cover
 * @options: (nullable): report-specific options
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the result, or %NULL on error
 */
VentureReportResult *
venture_report_generate(
	VentureReport		 *self,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_report_describe_parameters:
 * @self: a #VentureReport
 *
 * Returns: (transfer full) (nullable): a JSON Schema object describing the
 *   report's options
 */
JsonNode *
venture_report_describe_parameters(VentureReport *self);

/* --- Function-backed reports --------------------------------------------- */

/**
 * VentureReportFunc:
 * @context: the wiring
 * @period: the period to cover
 * @options: (nullable): report-specific options
 * @error: (out) (optional): return location for a #GError
 *
 * The signature of a report implemented as a function, which is what a
 * report usually is.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL on error
 */
typedef VentureReportResult * (*VentureReportFunc) (
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

#define VENTURE_TYPE_FUNC_REPORT (venture_func_report_get_type())

G_DECLARE_FINAL_TYPE(VentureFuncReport, venture_func_report,
                     VENTURE, FUNC_REPORT, VentureReport)

/**
 * venture_func_report_new:
 * @name: the machine name
 * @title: the human title
 * @description: what the report answers
 * @func: (scope forever): the implementation
 *
 * Returns: (transfer full): a report backed by @func
 */
VentureFuncReport *
venture_func_report_new(
	const gchar		*name,
	const gchar		*title,
	const gchar		*description,
	VentureReportFunc	 func
);

/* --- Registry ------------------------------------------------------------ */

#define VENTURE_TYPE_REPORT_REGISTRY (venture_report_registry_get_type())

G_DECLARE_FINAL_TYPE(VentureReportRegistry, venture_report_registry,
                     VENTURE, REPORT_REGISTRY, GObject)

/**
 * venture_report_registry_new:
 *
 * Returns: (transfer full): an empty registry
 */
VentureReportRegistry *
venture_report_registry_new(void);

/**
 * venture_report_registry_add:
 * @self: a #VentureReportRegistry
 * @report: (transfer full): the report to register
 *
 * Registers a report, replacing any with the same name.
 */
void
venture_report_registry_add(
	VentureReportRegistry	*self,
	VentureReport		*report
);

/**
 * venture_report_registry_remove:
 * @self: a #VentureReportRegistry
 * @name: the report name
 *
 * Withdraws a report. The module registry does this for every report a
 * disabled module owns, so a profit-and-loss is not offered on an install
 * that keeps no books.
 *
 * Returns: %TRUE if a report by that name was registered
 */
gboolean
venture_report_registry_remove(
	VentureReportRegistry	*self,
	const gchar		*name
);

/**
 * venture_report_registry_set_enabled:
 * @self: a #VentureReportRegistry
 * @name: the report name
 * @enabled: whether the report is offered
 *
 * Hides or shows a report without withdrawing it. The module registry
 * hides every report a disabled module owns and shows them again when the
 * module is on, so a profit-and-loss is not offered on an install that
 * keeps no books -- and comes back, with nothing re-registered, when it
 * does. A name that is not registered is remembered, so a report added
 * later under it is hidden too.
 */
void
venture_report_registry_set_enabled(
	VentureReportRegistry	*self,
	const gchar		*name,
	gboolean		 enabled
);

/**
 * venture_report_registry_lookup:
 * @self: a #VentureReportRegistry
 * @name: the report name
 *
 * Returns: (transfer none) (nullable): the report, or %NULL
 */
VentureReport *
venture_report_registry_lookup(
	VentureReportRegistry	*self,
	const gchar		*name
);

/**
 * venture_report_registry_list:
 * @self: a #VentureReportRegistry
 *
 * Returns: (transfer container) (element-type VentureReport): every
 *   registered report, sorted by name
 */
GPtrArray *
venture_report_registry_list(VentureReportRegistry *self);

/**
 * venture_report_registry_register_builtins:
 * @self: a #VentureReportRegistry
 *
 * Registers the reports VENTURE ships with.
 */
void
venture_report_registry_register_builtins(VentureReportRegistry *self);

/**
 * venture_report_registry_describe:
 * @self: a #VentureReportRegistry
 *
 * Returns: (transfer full): a JSON array naming and describing every report,
 *   which is what the AI's report tool advertises
 */
JsonNode *
venture_report_registry_describe(VentureReportRegistry *self);

G_END_DECLS

#endif /* VENTURE_REPORT_H */
