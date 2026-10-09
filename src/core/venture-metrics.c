/*
 * venture-metrics.c - Counters, gauges and histograms for GET /metrics
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The registry is the one piece of /metrics any thread may write: the
 * HTTP middleware on the main loop, the feeds service after a run, and
 * anything later. Everything it holds sits behind one mutex, and the lock
 * is never held across a call out -- collectors run after the registry's
 * own families have been written and the lock let go, so a collector
 * that reads the database (on the main thread, at a scrape) can never
 * wait on a thread that is waiting to add to a counter.
 */

#include "venture.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

/* The latency buckets, in seconds: from a cached page to a report that
 * makes somebody wait. */
static const gdouble metrics_buckets[] = {
	0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0
};

#define METRICS_BUCKETS G_N_ELEMENTS(metrics_buckets)

typedef struct
{
	gdouble	 value;				/* counter, gauge */
	guint64	 buckets[METRICS_BUCKETS];	/* histogram: per bucket, not cumulative */
	gdouble	 sum;
	guint64	 count;
} MetricSeries;

typedef struct
{
	gchar			*name;
	VentureMetricsKind	 kind;
	gchar			*help;
	GHashTable		*series;	/* labels ("" for none) -> MetricSeries */
} MetricFamily;

typedef struct
{
	gchar				*name;
	VentureMetricsCollectFunc	 func;
	gpointer			 user_data;
	GDestroyNotify			 destroy;
} MetricCollector;

struct _VentureMetrics
{
	GObject		 parent_instance;

	GMutex		 lock;
	GHashTable	*families;	/* name -> MetricFamily */
	GPtrArray	*collectors;	/* MetricCollector, main thread only */
	gint64		 created_at;	/* wall clock, for a process start /proc cannot give */
};

G_DEFINE_FINAL_TYPE(VentureMetrics, venture_metrics, G_TYPE_OBJECT)

#define METRICS_CONTEXT_KEY "venture-metrics"

static void
metric_family_free(gpointer data)
{
	MetricFamily *family = data;

	g_free(family->name);
	g_free(family->help);
	g_clear_pointer(&family->series, g_hash_table_unref);
	g_free(family);
}

static void
metric_collector_free(gpointer data)
{
	MetricCollector *collector = data;

	if (NULL != collector->destroy)
		collector->destroy(collector->user_data);

	g_free(collector->name);
	g_free(collector);
}

static void
venture_metrics_finalize(GObject *object)
{
	VentureMetrics *self = VENTURE_METRICS(object);

	g_clear_pointer(&self->collectors, g_ptr_array_unref);
	g_clear_pointer(&self->families, g_hash_table_unref);
	g_mutex_clear(&self->lock);

	G_OBJECT_CLASS(venture_metrics_parent_class)->finalize(object);
}

static void
venture_metrics_class_init(VentureMetricsClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_metrics_finalize;
}

static void
venture_metrics_init(VentureMetrics *self)
{
	g_mutex_init(&self->lock);
	self->families = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, metric_family_free);
	self->collectors = g_ptr_array_new_with_free_func(metric_collector_free);
	self->created_at = g_get_real_time() / G_USEC_PER_SEC;
}

VentureMetrics *
venture_metrics_new(void)
{
	return g_object_new(VENTURE_TYPE_METRICS, NULL);
}

VentureMetrics *
venture_metrics_for_context(VentureContext *context)
{
	VentureMetrics *metrics;

	g_return_val_if_fail(G_IS_OBJECT(context), NULL);

	metrics = g_object_get_data(G_OBJECT(context), METRICS_CONTEXT_KEY);

	if (NULL == metrics)
	{
		metrics = venture_metrics_new();
		g_object_set_data_full(G_OBJECT(context), METRICS_CONTEXT_KEY, metrics, g_object_unref);
	}

	return metrics;
}

void
venture_metrics_describe(
	VentureMetrics		*self,
	const gchar		*name,
	VentureMetricsKind	 kind,
	const gchar		*help
){
	MetricFamily *family;

	g_return_if_fail(VENTURE_IS_METRICS(self));
	g_return_if_fail(NULL != name);

	g_mutex_lock(&self->lock);

	if (NULL == g_hash_table_lookup(self->families, name))
	{
		family = g_new0(MetricFamily, 1);
		family->name = g_strdup(name);
		family->kind = kind;
		family->help = g_strdup((NULL != help) ? help : "");
		family->series = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_hash_table_insert(self->families, family->name, family);
	}

	g_mutex_unlock(&self->lock);
}

/* The series of @name for @labels, made if new. Called with the lock held;
 * NULL for a family never described, which is a programming error the
 * caller is told of once the lock is let go. */
static MetricSeries *
metrics_series(
	VentureMetrics		*self,
	const gchar		*name,
	const gchar		*labels,
	VentureMetricsKind	 kind
){
	MetricFamily *family;
	MetricSeries *series;

	family = g_hash_table_lookup(self->families, name);

	if ((NULL == family) || (family->kind != kind))
		return NULL;

	if (NULL == labels)
		labels = "";

	series = g_hash_table_lookup(family->series, labels);

	if (NULL == series)
	{
		series = g_new0(MetricSeries, 1);
		g_hash_table_insert(family->series, g_strdup(labels), series);
	}

	return series;
}

void
venture_metrics_add(
	VentureMetrics	*self,
	const gchar	*name,
	const gchar	*labels,
	gdouble		 delta
){
	MetricSeries *series;

	g_return_if_fail(VENTURE_IS_METRICS(self));
	g_return_if_fail(NULL != name);

	g_mutex_lock(&self->lock);
	series = metrics_series(self, name, labels, VENTURE_METRICS_COUNTER);

	/* A counter only goes up: a negative delta would read as a restart. */
	if ((NULL != series) && (delta > 0))
		series->value += delta;

	g_mutex_unlock(&self->lock);

	g_return_if_fail(NULL != series);
}

void
venture_metrics_set(
	VentureMetrics	*self,
	const gchar	*name,
	const gchar	*labels,
	gdouble		 value
){
	MetricSeries *series;

	g_return_if_fail(VENTURE_IS_METRICS(self));
	g_return_if_fail(NULL != name);

	g_mutex_lock(&self->lock);
	series = metrics_series(self, name, labels, VENTURE_METRICS_GAUGE);

	if (NULL != series)
		series->value = value;

	g_mutex_unlock(&self->lock);

	g_return_if_fail(NULL != series);
}

void
venture_metrics_observe(
	VentureMetrics	*self,
	const gchar	*name,
	const gchar	*labels,
	gdouble		 value
){
	MetricSeries *series;
	guint i;

	g_return_if_fail(VENTURE_IS_METRICS(self));
	g_return_if_fail(NULL != name);

	g_mutex_lock(&self->lock);
	series = metrics_series(self, name, labels, VENTURE_METRICS_HISTOGRAM);

	if (NULL != series)
	{
		for (i = 0; i < METRICS_BUCKETS; i++)
		{
			if (value <= metrics_buckets[i])
			{
				series->buckets[i]++;
				break;
			}
		}

		series->sum += value;
		series->count++;
	}

	g_mutex_unlock(&self->lock);

	g_return_if_fail(NULL != series);
}

gdouble
venture_metrics_get(
	VentureMetrics	*self,
	const gchar	*name,
	const gchar	*labels
){
	MetricFamily *family;
	MetricSeries *series;
	gdouble value;

	g_return_val_if_fail(VENTURE_IS_METRICS(self), 0);
	g_return_val_if_fail(NULL != name, 0);

	value = 0;
	g_mutex_lock(&self->lock);
	family = g_hash_table_lookup(self->families, name);
	series = (NULL != family) ? g_hash_table_lookup(family->series, (NULL != labels) ? labels : "")
	                          : NULL;

	if (NULL != series)
		value = (VENTURE_METRICS_HISTOGRAM == family->kind) ? (gdouble)series->count : series->value;

	g_mutex_unlock(&self->lock);

	return value;
}

void
venture_metrics_add_collector(
	VentureMetrics			*self,
	const gchar			*name,
	VentureMetricsCollectFunc	 func,
	gpointer			 user_data,
	GDestroyNotify			 destroy
){
	MetricCollector *collector;

	g_return_if_fail(VENTURE_IS_METRICS(self));
	g_return_if_fail(NULL != name);
	g_return_if_fail(NULL != func);

	venture_metrics_remove_collector(self, name);

	collector = g_new0(MetricCollector, 1);
	collector->name = g_strdup(name);
	collector->func = func;
	collector->user_data = user_data;
	collector->destroy = destroy;
	g_ptr_array_add(self->collectors, collector);
}

void
venture_metrics_remove_collector(
	VentureMetrics	*self,
	const gchar	*name
){
	guint i;

	g_return_if_fail(VENTURE_IS_METRICS(self));
	g_return_if_fail(NULL != name);

	for (i = 0; i < self->collectors->len; i++)
	{
		MetricCollector *collector = g_ptr_array_index(self->collectors, i);

		if (0 == g_strcmp0(collector->name, name))
		{
			g_ptr_array_remove_index(self->collectors, i);
			return;
		}
	}
}

/* --- Writing ------------------------------------------------------------------------ */

/* A label value escaped as the format asks: backslash, quote, newline. */
static void
metrics_append_escaped(
	GString		*out,
	const gchar	*text
){
	const gchar *p;

	for (p = (NULL != text) ? text : ""; '\0' != *p; p++)
	{
		if ('\\' == *p)
			g_string_append(out, "\\\\");
		else if ('"' == *p)
			g_string_append(out, "\\\"");
		else if ('\n' == *p)
			g_string_append(out, "\\n");
		else
			g_string_append_c(out, *p);
	}
}

gchar *
venture_metrics_labels(
	const gchar	*first_name,
	...
){
	GString *out;
	const gchar *name;
	va_list args;

	out = g_string_new(NULL);
	va_start(args, first_name);

	for (name = first_name; NULL != name; name = va_arg(args, const gchar *))
	{
		const gchar *value = va_arg(args, const gchar *);

		if (0 != out->len)
			g_string_append_c(out, ',');

		g_string_append(out, name);
		g_string_append(out, "=\"");
		metrics_append_escaped(out, value);
		g_string_append_c(out, '"');
	}

	va_end(args);

	return g_string_free(out, FALSE);
}

static const gchar *
metrics_kind_name(VentureMetricsKind kind)
{
	switch (kind)
	{
	case VENTURE_METRICS_GAUGE:
		return "gauge";
	case VENTURE_METRICS_HISTOGRAM:
		return "histogram";
	case VENTURE_METRICS_COUNTER:
	default:
		return "counter";
	}
}

void
venture_metrics_write_family(
	GString			*out,
	const gchar		*name,
	VentureMetricsKind	 kind,
	const gchar		*help
){
	const gchar *p;

	g_return_if_fail(NULL != out);
	g_return_if_fail(NULL != name);

	g_string_append_printf(out, "# HELP %s ", name);

	/* HELP escapes only backslash and newline. */
	for (p = (NULL != help) ? help : ""; '\0' != *p; p++)
	{
		if ('\\' == *p)
			g_string_append(out, "\\\\");
		else if ('\n' == *p)
			g_string_append(out, "\\n");
		else
			g_string_append_c(out, *p);
	}

	g_string_append_printf(out, "\n# TYPE %s %s\n", name, metrics_kind_name(kind));
}

/* A value as Go's strconv would print it: integers plainly, the rest with
 * enough digits to come back exactly, and the three specials spelt out.
 * printf's %g would write 1e+06 for a million bytes, which parses, but
 * reads like an error to a person looking at the page. */
static void
metrics_append_value(
	GString	*out,
	gdouble	 value
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	if (isnan(value))
		g_string_append(out, "NaN");
	else if (isinf(value))
		g_string_append(out, (value > 0) ? "+Inf" : "-Inf");
	else if ((value == floor(value)) && (fabs(value) < 9007199254740992.0))
		g_string_append_printf(out, "%" G_GINT64_FORMAT, (gint64)value);
	else
		g_string_append(out, g_ascii_dtostr(buffer, sizeof(buffer), value));
}

void
venture_metrics_write_sample(
	GString		*out,
	const gchar	*name,
	const gchar	*labels,
	gdouble		 value
){
	g_return_if_fail(NULL != out);
	g_return_if_fail(NULL != name);

	g_string_append(out, name);

	if ((NULL != labels) && ('\0' != labels[0]))
		g_string_append_printf(out, "{%s}", labels);

	g_string_append_c(out, ' ');
	metrics_append_value(out, value);
	g_string_append_c(out, '\n');
}

static void
metrics_write_histogram(
	GString			*out,
	const gchar		*name,
	const gchar		*labels,
	const MetricSeries	*series
){
	g_autofree gchar *bucket_name = g_strconcat(name, "_bucket", NULL);
	g_autofree gchar *sum_name = g_strconcat(name, "_sum", NULL);
	g_autofree gchar *count_name = g_strconcat(name, "_count", NULL);
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	guint64 cumulative;
	guint i;

	cumulative = 0;

	for (i = 0; i <= METRICS_BUCKETS; i++)
	{
		g_autofree gchar *with_le = NULL;
		const gchar *le;

		if (i < METRICS_BUCKETS)
		{
			cumulative += series->buckets[i];
			/* %g: "0.005", where the round-trip form is 0.0050000000000000001 */
			le = g_ascii_formatd(buffer, sizeof(buffer), "%g", metrics_buckets[i]);
		}
		else
		{
			cumulative = series->count;
			le = "+Inf";
		}

		with_le = ((NULL != labels) && ('\0' != labels[0]))
			? g_strdup_printf("%s,le=\"%s\"", labels, le)
			: g_strdup_printf("le=\"%s\"", le);
		venture_metrics_write_sample(out, bucket_name, with_le, (gdouble)cumulative);
	}

	venture_metrics_write_sample(out, sum_name, labels, series->sum);
	venture_metrics_write_sample(out, count_name, labels, (gdouble)series->count);
}

/* --- The process -------------------------------------------------------------------- */

/* The whole of a small /proc file, or NULL off Linux. */
static gchar *
metrics_read_proc(const gchar *path)
{
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return NULL;

	return text;
}

/*
 * When the process started, in Unix seconds: its start in clock ticks
 * after boot (field 22 of /proc/self/stat, counted after the command
 * name, which may itself hold spaces and parentheses) plus the boot time.
 */
static gint64
metrics_process_start(VentureMetrics *self)
{
	g_autofree gchar *stat = metrics_read_proc("/proc/self/stat");
	g_autofree gchar *system = metrics_read_proc("/proc/stat");
	g_auto(GStrv) fields = NULL;
	const gchar *after;
	const gchar *btime;
	gint64 boot;
	gint64 ticks;
	glong hz;

	if ((NULL == stat) || (NULL == system))
		return self->created_at;

	after = strrchr(stat, ')');
	btime = strstr(system, "\nbtime ");
	hz = sysconf(_SC_CLK_TCK);

	if ((NULL == after) || (NULL == btime) || (hz <= 0))
		return self->created_at;

	/* After ") ": field 3 (state) is index 0, so field 22 is index 19. */
	fields = g_strsplit(after + 2, " ", 0);

	if (g_strv_length(fields) < 20)
		return self->created_at;

	boot = g_ascii_strtoll(btime + 7, NULL, 10);
	ticks = g_ascii_strtoll(fields[19], NULL, 10);

	return boot + ticks / hz;
}

static void
metrics_write_process(
	VentureMetrics	*self,
	GString		*out
){
	g_autofree gchar *statm = metrics_read_proc("/proc/self/statm");
	g_autofree gchar *labels = NULL;
	gint64 now;
	gint64 start;

	now = g_get_real_time() / G_USEC_PER_SEC;
	start = metrics_process_start(self);

	labels = venture_metrics_labels("version", venture_get_version_string(), NULL);
	venture_metrics_write_family(out, "venture_build_info", VENTURE_METRICS_GAUGE,
	                             "The running build; always 1, the version is the label");
	venture_metrics_write_sample(out, "venture_build_info", labels, 1);

	venture_metrics_write_family(out, "process_start_time_seconds", VENTURE_METRICS_GAUGE,
	                             "When the process started, Unix seconds");
	venture_metrics_write_sample(out, "process_start_time_seconds", NULL, (gdouble)start);

	venture_metrics_write_family(out, "venture_uptime_seconds", VENTURE_METRICS_GAUGE,
	                             "Seconds since the process started");
	venture_metrics_write_sample(out, "venture_uptime_seconds", NULL, (gdouble)MAX(now - start, 0));

	/* statm is in pages: size, resident, shared, ... */
	if (NULL != statm)
	{
		g_auto(GStrv) pages = g_strsplit(statm, " ", 0);
		glong page_size = sysconf(_SC_PAGESIZE);

		if ((g_strv_length(pages) >= 2) && (page_size > 0))
		{
			venture_metrics_write_family(out, "process_virtual_memory_bytes", VENTURE_METRICS_GAUGE,
			                             "Virtual memory size in bytes");
			venture_metrics_write_sample(out, "process_virtual_memory_bytes", NULL,
			                             (gdouble)g_ascii_strtoll(pages[0], NULL, 10) * (gdouble)page_size);
			venture_metrics_write_family(out, "process_resident_memory_bytes", VENTURE_METRICS_GAUGE,
			                             "Resident memory size in bytes");
			venture_metrics_write_sample(out, "process_resident_memory_bytes", NULL,
			                             (gdouble)g_ascii_strtoll(pages[1], NULL, 10) * (gdouble)page_size);
		}
	}

	{
		g_autoptr(GDir) fds = g_dir_open("/proc/self/fd", 0, NULL);

		if (NULL != fds)
		{
			guint count = 0;

			while (NULL != g_dir_read_name(fds))
				count++;

			venture_metrics_write_family(out, "process_open_fds", VENTURE_METRICS_GAUGE,
			                             "Open file descriptors");
			venture_metrics_write_sample(out, "process_open_fds", NULL, (gdouble)count);
		}
	}
}

/* --- Rendering --------------------------------------------------------------------- */

static gint
metrics_compare_strings(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

gchar *
venture_metrics_render(VentureMetrics *self)
{
	g_autoptr(GPtrArray) names = NULL;
	GHashTableIter iter;
	gpointer key;
	GString *out;
	guint i;

	g_return_val_if_fail(VENTURE_IS_METRICS(self), NULL);

	out = g_string_new(NULL);
	metrics_write_process(self, out);

	g_mutex_lock(&self->lock);

	/* In name order, and each family's series in label order: a scrape
	 * that reads the same as the last is easy to diff by eye. */
	names = g_ptr_array_new();
	g_hash_table_iter_init(&iter, self->families);

	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(names, key);

	g_ptr_array_sort(names, metrics_compare_strings);

	for (i = 0; i < names->len; i++)
	{
		MetricFamily *family = g_hash_table_lookup(self->families, g_ptr_array_index(names, i));
		g_autoptr(GPtrArray) labels = g_ptr_array_new();
		gpointer labels_key;
		guint j;

		venture_metrics_write_family(out, family->name, family->kind, family->help);
		g_hash_table_iter_init(&iter, family->series);

		while (g_hash_table_iter_next(&iter, &labels_key, NULL))
			g_ptr_array_add(labels, labels_key);

		g_ptr_array_sort(labels, metrics_compare_strings);

		for (j = 0; j < labels->len; j++)
		{
			const gchar *set = g_ptr_array_index(labels, j);
			MetricSeries *series = g_hash_table_lookup(family->series, set);

			if (VENTURE_METRICS_HISTOGRAM == family->kind)
				metrics_write_histogram(out, family->name, set, series);
			else
				venture_metrics_write_sample(out, family->name, set, series->value);
		}
	}

	g_mutex_unlock(&self->lock);

	for (i = 0; i < self->collectors->len; i++)
	{
		MetricCollector *collector = g_ptr_array_index(self->collectors, i);

		collector->func(self, out, collector->user_data);
	}

	return g_string_free(out, FALSE);
}
