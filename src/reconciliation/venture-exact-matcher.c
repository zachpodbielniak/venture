/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "venture-reconciliation-private.h"

gpointer
venture_reconciliation_dup_field(VentureEntity *entity, const gchar *name, GType type)
{
	g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(entity);
	guint i;
	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		GParamSpec *pspec;
		gpointer value = NULL;
		if (g_strcmp0(venture_field_spec_get_name(spec), name) != 0 ||
			(venture_field_spec_get_flags(spec) & VENTURE_COLUMN_FLAG_SENSITIVE))
			continue;
		pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(entity), name);
		if (pspec != NULL && G_PARAM_SPEC_VALUE_TYPE(pspec) == type)
			g_object_get(entity, name, &value, NULL);
		return value;
	}
	return NULL;
}

static gboolean
same_scope(VentureEntity *transaction, VentureEntity *candidate)
{
	return transaction != candidate &&
		!(G_OBJECT_TYPE(transaction) == G_OBJECT_TYPE(candidate) &&
		venture_entity_get_id(transaction) != 0 &&
		venture_entity_get_id(transaction) == venture_entity_get_id(candidate)) &&
		venture_entity_get_organization_id(transaction) == venture_entity_get_organization_id(candidate);
}
static gint
date_distance(VentureEntity *a, VentureEntity *b)
{
	g_autoptr(GDateTime) ad = venture_reconciliation_dup_field(a, "date", G_TYPE_DATE_TIME);
	g_autoptr(GDateTime) bd = venture_reconciliation_dup_field(b, "date", G_TYPE_DATE_TIME);
	GDate ag, bg;
	if (ad == NULL || bd == NULL)
		return G_MAXINT;
	g_date_clear(&ag, 1);
	g_date_clear(&bg, 1);
	g_date_set_dmy(&ag, g_date_time_get_day_of_month(ad), g_date_time_get_month(ad), g_date_time_get_year(ad));
	g_date_set_dmy(&bg, g_date_time_get_day_of_month(bd), g_date_time_get_month(bd), g_date_time_get_year(bd));
	return ABS(g_date_days_between(&ag, &bg));
}
static gboolean
overlap(VentureEntity *a, VentureEntity *b)
{
	g_autofree gchar *ad = venture_reconciliation_dup_field(a, "description", G_TYPE_STRING);
	g_autofree gchar *bd = venture_reconciliation_dup_field(b, "description", G_TYPE_STRING);
	g_auto(GStrv) at = NULL;
	g_auto(GStrv) bt = NULL;
	guint i, j;
	if (ad == NULL || bd == NULL)
		return FALSE;
	at = g_str_tokenize_and_fold(ad, NULL, NULL);
	bt = g_str_tokenize_and_fold(bd, NULL, NULL);
	for (i = 0; at[i] != NULL; i++)
		for (j = 0; bt[j] != NULL; j++)
			if (g_str_equal(at[i], bt[j]))
				return TRUE;
	return FALSE;
}
/* Widen before subtracting or multiplying: even INT64_MIN is a valid input. */
static gint
amount_relation(const VentureMoney *a, const VentureMoney *b)
{
	__int128 av, bv, delta, base;
	guint ae, be;
	if (a == NULL || b == NULL ||
		g_strcmp0(venture_money_get_currency(a), venture_money_get_currency(b)) != 0)
		return 0;
	av = venture_money_get_amount(a);
	bv = venture_money_get_amount(b);
	ae = venture_money_get_exponent(a);
	be = venture_money_get_exponent(b);
	/* Bound widening by the money type's supported precision. */
	if (ae > VENTURE_MONEY_MAX_EXPONENT || be > VENTURE_MONEY_MAX_EXPONENT)
		return 0;
	while (ae < be) { av *= 10; ae++; }
	while (be < ae) { bv *= 10; be++; }
	if (av == bv)
		return 2;
	delta = av - bv;
	if (delta < 0) delta = -delta;
	base = av < 0 ? -av : av;
	return delta * 100 <= base * 2 ? 1 : 0;
}
struct _VentureExactMatcher { GObject parent_instance; };
static void exact_iface_init(VentureReconciliationMatcherInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureExactMatcher, venture_exact_matcher, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_RECONCILIATION_MATCHER, exact_iface_init))
static void
exact_get(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (id == 1) g_value_set_string(value, "exact");
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
venture_exact_matcher_class_init(VentureExactMatcherClass *klass)
{
	G_OBJECT_CLASS(klass)->get_property = exact_get;
	g_object_class_override_property(G_OBJECT_CLASS(klass), 1, "name");
}
static void venture_exact_matcher_init(VentureExactMatcher *self) { (void)self; }
static GPtrArray *
exact_suggest(VentureReconciliationMatcher *self, VentureDatabase *db,
	VentureEntity *transaction, GPtrArray *candidates, GCancellable *cancellable, GError **error)
{
	g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) amount = venture_reconciliation_dup_field(transaction, "amount", VENTURE_TYPE_MONEY);
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint i, unique = 0;
	(void)self;
	(void)db;
	for (i = 0; i < candidates->len; i++)
	{
		VentureEntity *candidate = g_ptr_array_index(candidates, i);
		g_autoptr(VentureMoney) other = NULL;
		if (!same_scope(transaction, candidate)) continue;
		other = venture_reconciliation_dup_field(candidate, "amount", VENTURE_TYPE_MONEY);
		if (amount_relation(amount, other) == 2 && date_distance(transaction, candidate) <= 3)
		{
			gint64 id = venture_entity_get_id(candidate);
			g_autofree gchar *key = id != 0 ? g_strdup_printf("%s:%" G_GINT64_FORMAT, G_OBJECT_TYPE_NAME(candidate), id)
				: g_strdup_printf("%p", (void *)candidate);
			if (!g_hash_table_contains(seen, key))
			{
				g_hash_table_add(seen, g_steal_pointer(&key));
				unique++;
			}
		}
	}
	for (i = 0; i < candidates->len; i++)
	{
		VentureEntity *candidate = g_ptr_array_index(candidates, i);
		g_autoptr(VentureMoney) other = NULL;
		gint relation, days, score = 0;
		const gchar *why = NULL;
		if (g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
		if (!same_scope(transaction, candidate)) continue;
		other = venture_reconciliation_dup_field(candidate, "amount", VENTURE_TYPE_MONEY);
		relation = amount_relation(amount, other);
		days = date_distance(transaction, candidate);
		if (relation == 2 && days <= 3 && unique == 1)
		{ score = 100; why = "Unique equal amount within three calendar days"; }
		else if (relation == 2 && days <= 10)
		{ score = 70; why = "Equal amount within ten calendar days"; }
		else if (relation != 0 && overlap(transaction, candidate))
		{ score = 40; why = "Amount within two percent with description token overlap"; }
		if (score != 0)
			g_ptr_array_add(result, venture_match_suggestion_new(candidate, score, why,
				relation == 2 ? VENTURE_MATCH_EXACT : VENTURE_MATCH_PARTIAL));
	}
	return g_steal_pointer(&result);
}
static void exact_iface_init(VentureReconciliationMatcherInterface *iface) { iface->suggest = exact_suggest; }
VentureExactMatcher *venture_exact_matcher_new(void) { return g_object_new(VENTURE_TYPE_EXACT_MATCHER, NULL); }
