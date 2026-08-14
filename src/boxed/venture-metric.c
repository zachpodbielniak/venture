/*
 * venture-metric.c - A single named measurement
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <math.h>

static const GEnumValue venture_metric_kind_values[] = {
	{ VENTURE_METRIC_KIND_MONEY,    "VENTURE_METRIC_KIND_MONEY",    "money" },
	{ VENTURE_METRIC_KIND_COUNT,    "VENTURE_METRIC_KIND_COUNT",    "count" },
	{ VENTURE_METRIC_KIND_RATIO,    "VENTURE_METRIC_KIND_RATIO",    "ratio" },
	{ VENTURE_METRIC_KIND_NUMBER,   "VENTURE_METRIC_KIND_NUMBER",   "number" },
	{ VENTURE_METRIC_KIND_DURATION, "VENTURE_METRIC_KIND_DURATION", "duration" },
	{ VENTURE_METRIC_KIND_TEXT,     "VENTURE_METRIC_KIND_TEXT",     "text" },
	{ 0, NULL, NULL }
};

GType
venture_metric_kind_get_type(void)
{
	static gsize venture_metric_kind_type_id = 0;

	if (g_once_init_enter(&venture_metric_kind_type_id))
	{
		GType registered;

		registered = g_enum_register_static("VentureMetricKind",
		                                    venture_metric_kind_values);
		g_once_init_leave(&venture_metric_kind_type_id, registered);
	}

	return (GType)venture_metric_kind_type_id;
}

G_DEFINE_BOXED_TYPE(VentureMetric, venture_metric,
                    venture_metric_copy, venture_metric_free)

/* --- Construction -------------------------------------------------------- */

/*
 * Common initialisation. Metrics default to higher-is-better because most
 * of them are revenue, units sold or subscribers; the exceptions set the
 * flag explicitly.
 */
static VentureMetric *
venture_metric_new_base(
	const gchar		*key,
	const gchar		*label,
	VentureMetricKind	 kind
){
	VentureMetric *self;

	self = g_new0(VentureMetric, 1);
	self->key = g_strdup(key);
	self->label = g_strdup(label);
	self->kind = kind;
	self->higher_is_better = TRUE;

	return self;
}

VentureMetric *
venture_metric_new_money(
	const gchar		*key,
	const gchar		*label,
	const VentureMoney	*value
){
	VentureMetric *self;

	g_return_val_if_fail(NULL != key, NULL);

	self = venture_metric_new_base(key, label, VENTURE_METRIC_KIND_MONEY);

	/* A missing amount becomes an explicit zero so downstream code never
	 * has to distinguish "no sales" from "not measured". */
	self->money = (NULL != value)
		? venture_money_copy(value)
		: venture_money_new_zero(NULL);

	return self;
}

VentureMetric *
venture_metric_new_count(
	const gchar	*key,
	const gchar	*label,
	gint64		 value
){
	VentureMetric *self;

	g_return_val_if_fail(NULL != key, NULL);

	self = venture_metric_new_base(key, label, VENTURE_METRIC_KIND_COUNT);
	self->number = (gdouble)value;

	return self;
}

VentureMetric *
venture_metric_new_ratio(
	const gchar	*key,
	const gchar	*label,
	gdouble		 value
){
	VentureMetric *self;

	g_return_val_if_fail(NULL != key, NULL);

	self = venture_metric_new_base(key, label, VENTURE_METRIC_KIND_RATIO);
	self->number = value;

	return self;
}

VentureMetric *
venture_metric_new_number(
	const gchar	*key,
	const gchar	*label,
	gdouble		 value
){
	VentureMetric *self;

	g_return_val_if_fail(NULL != key, NULL);

	self = venture_metric_new_base(key, label, VENTURE_METRIC_KIND_NUMBER);
	self->number = value;

	return self;
}

VentureMetric *
venture_metric_new_text(
	const gchar	*key,
	const gchar	*label,
	const gchar	*value
){
	VentureMetric *self;

	g_return_val_if_fail(NULL != key, NULL);

	self = venture_metric_new_base(key, label, VENTURE_METRIC_KIND_TEXT);
	self->text = g_strdup(value);

	return self;
}

VentureMetric *
venture_metric_copy(const VentureMetric *self)
{
	VentureMetric *copy;

	if (NULL == self)
		return NULL;

	copy = g_new0(VentureMetric, 1);
	copy->key = g_strdup(self->key);
	copy->label = g_strdup(self->label);
	copy->kind = self->kind;
	copy->money = venture_money_copy(self->money);
	copy->number = self->number;
	copy->text = g_strdup(self->text);
	copy->has_previous = self->has_previous;
	copy->previous_money = venture_money_copy(self->previous_money);
	copy->previous_number = self->previous_number;
	copy->higher_is_better = self->higher_is_better;
	copy->note = g_strdup(self->note);

	return copy;
}

void
venture_metric_free(VentureMetric *self)
{
	if (NULL == self)
		return;

	g_clear_pointer(&self->key, g_free);
	g_clear_pointer(&self->label, g_free);
	g_clear_pointer(&self->text, g_free);
	g_clear_pointer(&self->note, g_free);
	g_clear_pointer(&self->money, venture_money_free);
	g_clear_pointer(&self->previous_money, venture_money_free);
	g_free(self);
}

/* --- Comparison ---------------------------------------------------------- */

void
venture_metric_set_previous_money(
	VentureMetric		*self,
	const VentureMoney	*value
){
	g_return_if_fail(NULL != self);

	g_clear_pointer(&self->previous_money, venture_money_free);
	self->previous_money = (NULL != value)
		? venture_money_copy(value)
		: venture_money_new_zero(NULL);
	self->has_previous = TRUE;
}

void
venture_metric_set_previous_number(
	VentureMetric	*self,
	gdouble		 value
){
	g_return_if_fail(NULL != self);

	self->previous_number = value;
	self->has_previous = TRUE;
}

gboolean
venture_metric_has_previous(const VentureMetric *self)
{
	g_return_val_if_fail(NULL != self, FALSE);

	return self->has_previous;
}

/*
 * Returns the current and previous values as plain doubles so the change
 * calculation does not need one branch per metric kind. Money is converted
 * only here, at the point where an approximate ratio is genuinely what is
 * wanted -- the underlying amounts remain exact.
 */
static void
venture_metric_comparable_values(
	const VentureMetric	*self,
	gdouble			*out_current,
	gdouble			*out_previous
){
	if (VENTURE_METRIC_KIND_MONEY == self->kind)
	{
		*out_current = (NULL != self->money)
			? venture_money_to_double(self->money) : 0.0;
		*out_previous = (NULL != self->previous_money)
			? venture_money_to_double(self->previous_money) : 0.0;
		return;
	}

	*out_current = self->number;
	*out_previous = self->previous_number;
}

gdouble
venture_metric_get_change_ratio(const VentureMetric *self)
{
	gdouble current;
	gdouble previous;

	g_return_val_if_fail(NULL != self, 0.0);

	if (!self->has_previous)
		return 0.0;

	venture_metric_comparable_values(self, &current, &previous);

	/* Growth from zero has no meaningful percentage. Returning 0.0 lets
	 * the caller distinguish it via has_previous and render "new" instead
	 * of an infinity or a nonsense figure. */
	if (0.0 == previous)
		return 0.0;

	return (current - previous) / fabs(previous);
}

gint
venture_metric_get_direction(const VentureMetric *self)
{
	gdouble current;
	gdouble previous;
	gint raw;

	g_return_val_if_fail(NULL != self, 0);

	if (!self->has_previous)
		return 0;

	venture_metric_comparable_values(self, &current, &previous);

	if (current > previous)
		raw = 1;
	else if (current < previous)
		raw = -1;
	else
		raw = 0;

	/* For a cost or a churn rate, a rise is bad news; invert the sign so
	 * the UI can colour every metric from this one number. */
	return self->higher_is_better ? raw : -raw;
}

void
venture_metric_set_higher_is_better(
	VentureMetric	*self,
	gboolean	 higher_is_better
){
	g_return_if_fail(NULL != self);

	self->higher_is_better = higher_is_better;
}

void
venture_metric_set_note(
	VentureMetric	*self,
	const gchar	*note
){
	g_return_if_fail(NULL != self);

	g_free(self->note);
	self->note = g_strdup(note);
}

/* --- Accessors ----------------------------------------------------------- */

const gchar *
venture_metric_get_key(const VentureMetric *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->key;
}

const gchar *
venture_metric_get_label(const VentureMetric *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	/* Fall back to the key so a metric added by a plugin that forgot a
	 * label still renders something identifiable. */
	return (NULL != self->label) ? self->label : self->key;
}

VentureMetricKind
venture_metric_get_kind(const VentureMetric *self)
{
	g_return_val_if_fail(NULL != self, VENTURE_METRIC_KIND_NUMBER);

	return self->kind;
}

const VentureMoney *
venture_metric_get_money(const VentureMetric *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->money;
}

gdouble
venture_metric_get_number(const VentureMetric *self)
{
	g_return_val_if_fail(NULL != self, 0.0);

	return self->number;
}

const gchar *
venture_metric_get_text(const VentureMetric *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->text;
}

/* --- Rendering ----------------------------------------------------------- */

/*
 * Formats a whole number with thousands separators. Used for counts, where
 * "12,480 units" reads far better than "12480".
 */
static gchar *
venture_metric_format_grouped(gint64 value)
{
	g_autofree gchar *plain = NULL;
	g_autoptr(GString) grouped = NULL;
	gboolean negative;
	gsize length;
	gsize i;

	negative = (value < 0);
	plain = g_strdup_printf("%" G_GUINT64_FORMAT,
	                        negative ? (guint64)(-(value + 1)) + 1
	                                 : (guint64)value);
	length = strlen(plain);
	grouped = g_string_new(negative ? "-" : "");

	for (i = 0; i < length; i++)
	{
		if ((i > 0) && (0 == ((length - i) % 3)))
			g_string_append_c(grouped, ',');

		g_string_append_c(grouped, plain[i]);
	}

	return g_string_free(g_steal_pointer(&grouped), FALSE);
}

gchar *
venture_metric_format_value(const VentureMetric *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	switch (self->kind)
	{
	case VENTURE_METRIC_KIND_MONEY:
		if (NULL == self->money)
			return g_strdup("--");

		return venture_money_to_display_string(self->money, TRUE);

	case VENTURE_METRIC_KIND_COUNT:
		return venture_metric_format_grouped((gint64)self->number);

	case VENTURE_METRIC_KIND_RATIO:
		return g_strdup_printf("%.1f%%", self->number * 100.0);

	case VENTURE_METRIC_KIND_DURATION:
	{
		gint64 seconds;

		seconds = (gint64)self->number;

		/* Pick the largest unit that keeps the number readable rather
		 * than printing "907200 seconds". */
		if (seconds < 60)
			return g_strdup_printf("%" G_GINT64_FORMAT "s", seconds);

		if (seconds < 3600)
			return g_strdup_printf("%" G_GINT64_FORMAT "m", seconds / 60);

		if (seconds < 86400)
			return g_strdup_printf("%.1fh", (gdouble)seconds / 3600.0);

		return g_strdup_printf("%.1f days", (gdouble)seconds / 86400.0);
	}

	case VENTURE_METRIC_KIND_TEXT:
		return g_strdup((NULL != self->text) ? self->text : "--");

	case VENTURE_METRIC_KIND_NUMBER:
	default:
		/* Whole numbers print without a pointless ".00"; fractional
		 * ones keep two places. */
		if (self->number == floor(self->number))
			return venture_metric_format_grouped((gint64)self->number);

		return g_strdup_printf("%.2f", self->number);
	}
}

gchar *
venture_metric_format_change(const VentureMetric *self)
{
	gdouble current;
	gdouble previous;
	gdouble ratio;

	g_return_val_if_fail(NULL != self, NULL);

	if (!self->has_previous)
		return NULL;

	venture_metric_comparable_values(self, &current, &previous);

	if (0.0 == previous)
	{
		/* No baseline to be a percentage of. Say so plainly. */
		return g_strdup((0.0 == current) ? "no change" : "new");
	}

	ratio = venture_metric_get_change_ratio(self);

	return g_strdup_printf("%s%.1f%%", (ratio >= 0.0) ? "+" : "",
	                       ratio * 100.0);
}

JsonNode *
venture_metric_to_json(const VentureMetric *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *formatted = NULL;
	g_autofree gchar *change = NULL;

	g_return_val_if_fail(NULL != self, NULL);

	formatted = venture_metric_format_value(self);
	change = venture_metric_format_change(self);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "key");
	json_builder_add_string_value(builder, self->key);

	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, venture_metric_get_label(self));

	json_builder_set_member_name(builder, "kind");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_METRIC_KIND, (gint)self->kind));

	json_builder_set_member_name(builder, "value");

	switch (self->kind)
	{
	case VENTURE_METRIC_KIND_MONEY:
		json_builder_add_value(builder, venture_money_to_json(self->money));
		break;

	case VENTURE_METRIC_KIND_TEXT:
		json_builder_add_string_value(builder, self->text);
		break;

	case VENTURE_METRIC_KIND_COUNT:
		json_builder_add_int_value(builder, (gint64)self->number);
		break;

	default:
		json_builder_add_double_value(builder, self->number);
		break;
	}

	json_builder_set_member_name(builder, "formatted");
	json_builder_add_string_value(builder, formatted);

	json_builder_set_member_name(builder, "higher_is_better");
	json_builder_add_boolean_value(builder, self->higher_is_better);

	if (self->has_previous)
	{
		json_builder_set_member_name(builder, "change_ratio");
		json_builder_add_double_value(builder,
			venture_metric_get_change_ratio(self));

		json_builder_set_member_name(builder, "change_formatted");
		json_builder_add_string_value(builder, change);

		json_builder_set_member_name(builder, "direction");
		json_builder_add_int_value(builder,
			(gint64)venture_metric_get_direction(self));
	}

	if (NULL != self->note)
	{
		json_builder_set_member_name(builder, "note");
		json_builder_add_string_value(builder, self->note);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}
