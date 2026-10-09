/*
 * venture-metrics.h - Counters, gauges and histograms for GET /metrics
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One registry per context, rendered in the Prometheus text exposition
 * format (version 0.0.4) by GET /metrics. Two ways in:
 *
 *   - a family the registry holds -- described once, then added to, set
 *     or observed by label set from any thread (a mutex guards it), and
 *   - a collector: a function called on the main thread at each scrape
 *     that writes whole families of its own, for figures read when asked
 *     (a store's size, a venue's age) rather than counted as they happen.
 *
 * Labels are passed pre-rendered (venture_metrics_labels()), and every
 * label a caller sets must come from a bounded set: a route class, a
 * status code, a data source, a venue. Never an instrument, a user or
 * anything a request can choose -- each distinct label set is a series
 * the scraper keeps for weeks.
 */

#ifndef VENTURE_METRICS_H
#define VENTURE_METRICS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VentureMetricsKind:
 * @VENTURE_METRICS_COUNTER: only ever goes up (resets at a restart)
 * @VENTURE_METRICS_GAUGE: a value that is set
 * @VENTURE_METRICS_HISTOGRAM: observations counted into buckets, with
 *   their sum and count
 *
 * What kind of family a name is.
 */
typedef enum
{
	VENTURE_METRICS_COUNTER = 0,
	VENTURE_METRICS_GAUGE,
	VENTURE_METRICS_HISTOGRAM
} VentureMetricsKind;

#define VENTURE_TYPE_METRICS (venture_metrics_get_type())

G_DECLARE_FINAL_TYPE(VentureMetrics, venture_metrics, VENTURE, METRICS, GObject)

/**
 * VentureMetricsCollectFunc:
 * @metrics: the registry being rendered
 * @out: where the exposition text goes
 * @user_data: the collector's data
 *
 * Writes whole families -- a `# HELP`, a `# TYPE` and every sample of
 * each -- with venture_metrics_write_family() and
 * venture_metrics_write_sample(). Called on the main thread, at a scrape.
 */
typedef void (*VentureMetricsCollectFunc) (
	VentureMetrics	*metrics,
	GString		*out,
	gpointer	 user_data
);

/**
 * venture_metrics_new:
 *
 * Returns: (transfer full): an empty registry
 */
VentureMetrics *
venture_metrics_new(void);

/**
 * venture_metrics_for_context:
 * @context: the wiring
 *
 * The context's registry, made on first use. Main thread.
 *
 * Returns: (transfer none): the registry
 */
VentureMetrics *
venture_metrics_for_context(VentureContext *context);

/**
 * venture_metrics_describe:
 * @self: the registry
 * @name: the family's name, `venture_` first, `_total` last for a counter
 * @kind: what kind it is
 * @help: one line saying what it counts
 *
 * Declares a family. A second description of the same name is ignored,
 * so every caller may describe what it uses.
 */
void
venture_metrics_describe(
	VentureMetrics		*self,
	const gchar		*name,
	VentureMetricsKind	 kind,
	const gchar		*help
);

/**
 * venture_metrics_add:
 * @self: the registry
 * @name: a counter described before
 * @labels: (nullable): the label set, from venture_metrics_labels()
 * @delta: how much to add, zero or more
 *
 * Adds to a counter. Any thread.
 */
void
venture_metrics_add(
	VentureMetrics	*self,
	const gchar	*name,
	const gchar	*labels,
	gdouble		 delta
);

/**
 * venture_metrics_set:
 * @self: the registry
 * @name: a gauge described before
 * @labels: (nullable): the label set
 * @value: the value
 *
 * Sets a gauge. Any thread.
 */
void
venture_metrics_set(
	VentureMetrics	*self,
	const gchar	*name,
	const gchar	*labels,
	gdouble		 value
);

/**
 * venture_metrics_observe:
 * @self: the registry
 * @name: a histogram described before
 * @labels: (nullable): the label set
 * @value: the observation, in seconds for a latency
 *
 * Counts an observation into the histogram's buckets (5 ms to 10 s, the
 * request latencies a person notices). Any thread.
 */
void
venture_metrics_observe(
	VentureMetrics	*self,
	const gchar	*name,
	const gchar	*labels,
	gdouble		 value
);

/**
 * venture_metrics_get:
 * @self: the registry
 * @name: a counter or gauge
 * @labels: (nullable): the label set
 *
 * Returns: its value, 0 when it has none; a histogram's count
 */
gdouble
venture_metrics_get(
	VentureMetrics	*self,
	const gchar	*name,
	const gchar	*labels
);

/**
 * venture_metrics_add_collector:
 * @self: the registry
 * @name: names the collector, so a second add replaces the first
 * @func: (scope notified) (closure user_data): writes its families
 * @user_data: (nullable): its data
 * @destroy: (nullable): frees @user_data
 *
 * Adds a function called at every render, after the registry's own
 * families.
 */
void
venture_metrics_add_collector(
	VentureMetrics			*self,
	const gchar			*name,
	VentureMetricsCollectFunc	 func,
	gpointer			 user_data,
	GDestroyNotify			 destroy
);

/**
 * venture_metrics_remove_collector:
 * @self: the registry
 * @name: the collector's name
 */
void
venture_metrics_remove_collector(
	VentureMetrics	*self,
	const gchar	*name
);

/**
 * venture_metrics_render:
 * @self: the registry
 *
 * Every family as Prometheus text: the process's own, then the
 * registry's in name order, then each collector's.
 *
 * Returns: (transfer full): the exposition text
 */
gchar *
venture_metrics_render(VentureMetrics *self);

/**
 * venture_metrics_labels:
 * @first_name: the first label's name
 * @...: its value, then further name-value pairs, %NULL-terminated
 *
 * A label set as the exposition format writes it, `a="x",b="y"`, each
 * value escaped. A %NULL value is written as empty.
 *
 * Returns: (transfer full): the label set
 */
gchar *
venture_metrics_labels(
	const gchar	*first_name,
	...
) G_GNUC_NULL_TERMINATED;

/**
 * venture_metrics_write_family:
 * @out: the text being written
 * @name: the family's name
 * @kind: its kind
 * @help: one line
 *
 * Writes a family's `# HELP` and `# TYPE` lines, for a collector.
 */
void
venture_metrics_write_family(
	GString			*out,
	const gchar		*name,
	VentureMetricsKind	 kind,
	const gchar		*help
);

/**
 * venture_metrics_write_sample:
 * @out: the text being written
 * @name: the sample's name
 * @labels: (nullable): its label set
 * @value: its value
 *
 * Writes one sample line, for a collector.
 */
void
venture_metrics_write_sample(
	GString		*out,
	const gchar	*name,
	const gchar	*labels,
	gdouble		 value
);

/**
 * VENTURE_METRICS_CONTENT_TYPE:
 *
 * What GET /metrics answers with.
 */
#define VENTURE_METRICS_CONTENT_TYPE "text/plain; version=0.0.4; charset=utf-8"

G_END_DECLS

#endif /* VENTURE_METRICS_H */
