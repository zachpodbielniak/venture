/*
 * venture-metric.h - A single named measurement
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #VentureMetric is one number with everything needed to render and
 * compare it: a machine key, a human label, the value, how to format it, and
 * optionally the same measurement for the preceding period so a change can
 * be shown without the caller recomputing it.
 *
 * This is the unit of currency between the reporting engine, the dashboard
 * tiles, the CLI table renderer and the AI's report tool. A venture type
 * declares which metrics it exposes; a report produces them; every consumer
 * renders them the same way without knowing what they mean.
 */

#ifndef VENTURE_METRIC_H
#define VENTURE_METRIC_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "boxed/venture-money.h"

G_BEGIN_DECLS

#define VENTURE_TYPE_METRIC (venture_metric_get_type())

/**
 * VentureMetricKind:
 * @VENTURE_METRIC_KIND_MONEY: a monetary amount
 * @VENTURE_METRIC_KIND_COUNT: a whole-number tally
 * @VENTURE_METRIC_KIND_RATIO: a fraction rendered as a percentage
 * @VENTURE_METRIC_KIND_NUMBER: a plain number
 * @VENTURE_METRIC_KIND_DURATION: a span of seconds
 * @VENTURE_METRIC_KIND_TEXT: a non-numeric value, such as a best seller's
 *   title
 *
 * How a metric's value should be interpreted and formatted.
 */
typedef enum
{
	VENTURE_METRIC_KIND_MONEY = 0,
	VENTURE_METRIC_KIND_COUNT,
	VENTURE_METRIC_KIND_RATIO,
	VENTURE_METRIC_KIND_NUMBER,
	VENTURE_METRIC_KIND_DURATION,
	VENTURE_METRIC_KIND_TEXT
} VentureMetricKind;

#define VENTURE_TYPE_METRIC_KIND (venture_metric_kind_get_type())

GType
venture_metric_kind_get_type(void) G_GNUC_CONST;

/**
 * VentureMetric:
 *
 * Opaque. Use the accessors.
 */
struct _VentureMetric
{
	/*< private >*/
	gchar			*key;
	gchar			*label;
	VentureMetricKind	 kind;

	VentureMoney		*money;
	gdouble			 number;
	gchar			*text;

	gboolean		 has_previous;
	VentureMoney		*previous_money;
	gdouble			 previous_number;

	/* TRUE when a larger value is a better outcome. Cost per acquisition
	 * and refund rate set this FALSE, so the UI colours a rise in them
	 * red rather than green. */
	gboolean		 higher_is_better;

	gchar			*note;
};

GType
venture_metric_get_type(void) G_GNUC_CONST;

/* --- Construction -------------------------------------------------------- */

/**
 * venture_metric_new_money:
 * @key: a stable machine key, e.g. "gross_revenue"
 * @label: a human label, e.g. "Gross revenue"
 * @value: (nullable): the amount; %NULL is treated as zero
 *
 * Returns: (transfer full): a new monetary metric
 */
VentureMetric *
venture_metric_new_money(
	const gchar		*key,
	const gchar		*label,
	const VentureMoney	*value
);

/**
 * venture_metric_new_count:
 * @key: a stable machine key
 * @label: a human label
 * @value: the tally
 *
 * Returns: (transfer full): a new count metric
 */
VentureMetric *
venture_metric_new_count(
	const gchar	*key,
	const gchar	*label,
	gint64		 value
);

/**
 * venture_metric_new_ratio:
 * @key: a stable machine key
 * @label: a human label
 * @value: the ratio, where 1.0 means 100%
 *
 * Returns: (transfer full): a new ratio metric
 */
VentureMetric *
venture_metric_new_ratio(
	const gchar	*key,
	const gchar	*label,
	gdouble		 value
);

/**
 * venture_metric_new_number:
 * @key: a stable machine key
 * @label: a human label
 * @value: the value
 *
 * Returns: (transfer full): a new numeric metric
 */
VentureMetric *
venture_metric_new_number(
	const gchar	*key,
	const gchar	*label,
	gdouble		 value
);

/**
 * venture_metric_new_text:
 * @key: a stable machine key
 * @label: a human label
 * @value: (nullable): the text
 *
 * Returns: (transfer full): a new textual metric
 */
VentureMetric *
venture_metric_new_text(
	const gchar	*key,
	const gchar	*label,
	const gchar	*value
);

/**
 * venture_metric_copy:
 * @self: (nullable): a #VentureMetric
 *
 * Returns: (transfer full) (nullable): a deep copy
 */
VentureMetric *
venture_metric_copy(const VentureMetric *self);

/**
 * venture_metric_free:
 * @self: (nullable): a #VentureMetric
 *
 * Frees @self. Safe to call with %NULL.
 */
void
venture_metric_free(VentureMetric *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureMetric, venture_metric_free)

/* --- Comparison against a previous period -------------------------------- */

/**
 * venture_metric_set_previous_money:
 * @self: a #VentureMetric
 * @value: (nullable): the same measurement over the preceding period
 *
 * Attaches a comparison value so venture_metric_get_change_ratio() and the
 * dashboard delta indicator have something to work with.
 */
void
venture_metric_set_previous_money(
	VentureMetric		*self,
	const VentureMoney	*value
);

/**
 * venture_metric_set_previous_number:
 * @self: a #VentureMetric
 * @value: the same measurement over the preceding period
 */
void
venture_metric_set_previous_number(
	VentureMetric	*self,
	gdouble		 value
);

/**
 * venture_metric_has_previous:
 * @self: a #VentureMetric
 *
 * Returns: %TRUE if a comparison value is available
 */
gboolean
venture_metric_has_previous(const VentureMetric *self);

/**
 * venture_metric_get_change_ratio:
 * @self: a #VentureMetric
 *
 * Computes the fractional change from the previous period, so 0.25 means a
 * 25% rise. Growth from a previous value of zero is undefined rather than
 * infinite, and reports as 0.0 with venture_metric_has_previous() still
 * %TRUE -- the UI shows "new" in that case rather than a meaningless
 * percentage.
 *
 * Returns: the fractional change
 */
gdouble
venture_metric_get_change_ratio(const VentureMetric *self);

/**
 * venture_metric_get_direction:
 * @self: a #VentureMetric
 *
 * Reports whether the change is good, bad or neutral, taking
 * #VentureMetric's higher-is-better flag into account.
 *
 * Returns: 1 if the change is favourable, -1 if unfavourable, 0 if flat or
 *   incomparable
 */
gint
venture_metric_get_direction(const VentureMetric *self);

/**
 * venture_metric_set_higher_is_better:
 * @self: a #VentureMetric
 * @higher_is_better: whether a rise is a good outcome
 *
 * Defaults to %TRUE. Set it %FALSE for costs, refunds and churn.
 */
void
venture_metric_set_higher_is_better(
	VentureMetric	*self,
	gboolean	 higher_is_better
);

/**
 * venture_metric_set_note:
 * @self: a #VentureMetric
 * @note: (nullable): a short caveat shown beneath the value
 */
void
venture_metric_set_note(
	VentureMetric	*self,
	const gchar	*note
);

/* --- Accessors ----------------------------------------------------------- */

/**
 * venture_metric_get_key:
 * @self: a #VentureMetric
 *
 * Returns: (transfer none): the machine key
 */
const gchar *
venture_metric_get_key(const VentureMetric *self);

/**
 * venture_metric_get_label:
 * @self: a #VentureMetric
 *
 * Returns: (transfer none): the human label
 */
const gchar *
venture_metric_get_label(const VentureMetric *self);

/**
 * venture_metric_get_kind:
 * @self: a #VentureMetric
 *
 * Returns: the metric kind
 */
VentureMetricKind
venture_metric_get_kind(const VentureMetric *self);

/**
 * venture_metric_get_money:
 * @self: a #VentureMetric
 *
 * Returns: (transfer none) (nullable): the amount, for a money metric
 */
const VentureMoney *
venture_metric_get_money(const VentureMetric *self);

/**
 * venture_metric_get_number:
 * @self: a #VentureMetric
 *
 * Returns: the numeric value, for any non-money numeric metric
 */
gdouble
venture_metric_get_number(const VentureMetric *self);

/**
 * venture_metric_get_text:
 * @self: a #VentureMetric
 *
 * Returns: (transfer none) (nullable): the text, for a text metric
 */
const gchar *
venture_metric_get_text(const VentureMetric *self);

/* --- Rendering ----------------------------------------------------------- */

/**
 * venture_metric_format_value:
 * @self: a #VentureMetric
 *
 * Formats the value according to its kind: money with a currency symbol,
 * ratios as a percentage to one decimal place, counts with thousands
 * separators, durations as a human span.
 *
 * Returns: (transfer full): the formatted value
 */
gchar *
venture_metric_format_value(const VentureMetric *self);

/**
 * venture_metric_format_change:
 * @self: a #VentureMetric
 *
 * Formats the period-over-period change, for example "+12.4%".
 *
 * Returns: (transfer full) (nullable): the formatted change, or %NULL if
 *   there is nothing to compare against
 */
gchar *
venture_metric_format_change(const VentureMetric *self);

/**
 * venture_metric_to_json:
 * @self: a #VentureMetric
 *
 * Serialises the metric, including its formatted forms so that a consumer
 * which does not know the encoding still renders a correct value.
 *
 * Returns: (transfer full): a new #JsonNode
 */
JsonNode *
venture_metric_to_json(const VentureMetric *self);

G_END_DECLS

#endif /* VENTURE_METRIC_H */
