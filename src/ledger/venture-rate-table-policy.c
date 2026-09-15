/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

struct _VentureRateTablePolicy
{
	GObject parent_instance;
	GWeakRef database;
	gint64 organization_id;
};

static void rate_table_iface(VentureExchangePolicyInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(VentureRateTablePolicy, venture_rate_table_policy, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_EXCHANGE_POLICY, rate_table_iface))

static void
venture_rate_table_policy_finalize(GObject *object)
{
	g_weak_ref_clear(&VENTURE_RATE_TABLE_POLICY(object)->database);
	G_OBJECT_CLASS(venture_rate_table_policy_parent_class)->finalize(object);
}

static void
venture_rate_table_policy_init(VentureRateTablePolicy *self)
{
	g_weak_ref_init(&self->database, NULL);
}

static void
venture_rate_table_policy_class_init(VentureRateTablePolicyClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_rate_table_policy_finalize;
}

static const gchar *
rate_table_name(VentureExchangePolicy *self)
{
	(void)self;
	return "exchange_rate";
}

static gint
compare_effective(gconstpointer a, gconstpointer b)
{
	g_autoptr(GDateTime) left = NULL;
	g_autoptr(GDateTime) right = NULL;

	g_object_get(*(VentureEntity *const *)a, "effective-at", &left, NULL);
	g_object_get(*(VentureEntity *const *)b, "effective-at", &right, NULL);
	if (left == NULL || right == NULL)
		return 0;
	return g_date_time_compare(right, left);
}

static VentureMoney *
rate_table_convert(VentureExchangePolicy *policy, const VentureMoney *amount,
	const gchar *currency, GDateTime *when, GError **error)
{
	VentureRateTablePolicy *self = VENTURE_RATE_TABLE_POLICY(policy);
	g_autoptr(VentureDatabase) database = g_weak_ref_get(&self->database);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *when_text = NULL;
	guint i;

	if (database == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
			"The exchange-rate database has been closed");
		return NULL;
	}
	if (amount == NULL || currency == NULL || when == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"An exchange conversion needs an amount, book currency and date");
		return NULL;
	}
	if (g_strcmp0(amount->currency, currency) == 0)
		return venture_money_copy(amount);
	query = venture_query_new(VENTURE_TYPE_EXCHANGE_RATE);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, self->organization_id);
	if (!venture_query_add_filter_string(query, "from-currency", VENTURE_FILTER_OP_EQ, amount->currency, error) ||
		!venture_query_add_filter_string(query, "to-currency", VENTURE_FILTER_OP_EQ, currency, error))
		return NULL;
	rows = venture_database_find(database, query, error);
	if (rows == NULL)
		return NULL;
	g_ptr_array_sort(rows, compare_effective);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) effective = NULL;
		gint64 numerator = 0;
		gint64 denominator = 0;
		if (venture_entity_is_deleted(row))
			continue;
		g_object_get(row, "effective-at", &effective, "rate-numerator", &numerator,
			"rate-denominator", &denominator, NULL);
		if (effective == NULL || g_date_time_compare(effective, when) > 0)
			continue;
		if (numerator <= 0 || denominator <= 0)
			continue;
		{
			return venture_money_convert_at_rate(amount, numerator, denominator, currency, error);
		}
	}
	when_text = g_date_time_format_iso8601(when);
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"No exchange rate from %s to %s on %s; record an exchange_rate",
		amount->currency, currency, when_text);
	return NULL;
}

static void
rate_table_iface(VentureExchangePolicyInterface *iface)
{
	iface->get_name = rate_table_name;
	iface->convert = rate_table_convert;
}

VentureExchangePolicy *
venture_rate_table_policy_new(VentureDatabase *database, gint64 organization_id)
{
	VentureRateTablePolicy *self;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_new(VENTURE_TYPE_RATE_TABLE_POLICY, NULL);
	g_weak_ref_set(&self->database, database);
	self->organization_id = organization_id;
	return VENTURE_EXCHANGE_POLICY(self);
}

gboolean
venture_rate_table_policy_matches(VentureRateTablePolicy *self, VentureDatabase *database,
	gint64 organization_id)
{
	g_autoptr(VentureDatabase) owner = NULL;
	g_return_val_if_fail(VENTURE_IS_RATE_TABLE_POLICY(self), FALSE);
	/* Consent snapshots one database and legal entity, never another table. */
	owner = g_weak_ref_get(&self->database);
	return owner == database && self->organization_id == organization_id;
}
