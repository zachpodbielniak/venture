/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "venture-reconciliation-private.h"

#include <vk-kernels.h>

gpointer
venture_reconciliation_dup_field(VentureEntity *entity, const gchar *name, GType type)
{
	g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(entity);
	guint i;
	/* The banking service owns cash signs and operational date aliases;
	 * matchers must compare the very amounts its approval will validate. */
	if (g_str_equal(name, "amount") && type == VENTURE_TYPE_MONEY)
	{
		VentureMoney *amount = venture_bank_candidate_amount(entity);
		if (amount != NULL) return amount;
	}
	if (g_str_equal(name, "date") && type == G_TYPE_DATE_TIME)
	{
		g_autoptr(VentureMoney) amount = venture_bank_candidate_amount(entity);
		if (amount != NULL) return venture_bank_candidate_date(entity);
	}
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
/* The calendar day of @entity's date (in the date's own offset) as a
 * Julian day number, or none when it has no date. */
static TlwOptI64
calendar_day(VentureEntity *entity)
{
	g_autoptr(GDateTime) when = venture_reconciliation_dup_field(entity, "date", G_TYPE_DATE_TIME);
	TlwOptI64 day = { FALSE, 0 };
	GDate date;
	if (when == NULL)
		return day;
	g_date_clear(&date, 1);
	g_date_set_dmy(&date, g_date_time_get_day_of_month(when), g_date_time_get_month(when), g_date_time_get_year(when));
	day.has = TRUE;
	day.val = (gint64)g_date_get_julian(&date);
	return day;
}

/* Calendar days between the two dates, none when either is missing
 * (vk_reconcile_distance). */
static TlwOptI64
date_distance(VentureEntity *a, VentureEntity *b)
{
	return vk_reconcile_distance(calendar_day(a), calendar_day(b));
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
/* Equal, within 2% of @a, or apart (vk_reconcile_relation, exact at any
 * amount and exponent); apart when either is missing or the currencies
 * differ. */
static VkReconcileRelation
amount_relation(const VentureMoney *a, const VentureMoney *b)
{
	if (a == NULL || b == NULL ||
		g_strcmp0(venture_money_get_currency(a), venture_money_get_currency(b)) != 0)
		return VK_RECONCILE_RELATION_APART;
	return vk_reconcile_relation(venture_money_get_amount(a), (gint64)venture_money_get_exponent(a),
		venture_money_get_amount(b), (gint64)venture_money_get_exponent(b));
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
		if (vk_reconcile_exact_near(amount_relation(amount, other), date_distance(transaction, candidate)))
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
		VkReconcileRelation relation;
		VkReconcileScore score;
		const gchar *why = NULL;
		if (g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
		if (!same_scope(transaction, candidate)) continue;
		other = venture_reconciliation_dup_field(candidate, "amount", VENTURE_TYPE_MONEY);
		relation = amount_relation(amount, other);
		/* The score table is vk_reconcile_score; whether the descriptions
		 * share a token is Unicode folding, GLib's, and asked only when
		 * the amounts are related at all. */
		score = vk_reconcile_score(relation, date_distance(transaction, candidate), (gint64)unique,
			relation != VK_RECONCILE_RELATION_APART && overlap(transaction, candidate));
		switch (score)
		{
		case VK_RECONCILE_SCORE_UNIQUE:
			why = "Unique equal amount within three calendar days";
			break;
		case VK_RECONCILE_SCORE_CLOSE:
			why = "Equal amount within ten calendar days";
			break;
		case VK_RECONCILE_SCORE_SIMILAR:
			why = "Amount within two percent with description token overlap";
			break;
		case VK_RECONCILE_SCORE_NOTHING:
		default:
			break;
		}
		if (why != NULL)
			g_ptr_array_add(result, venture_match_suggestion_new(candidate, (gint)vk_reconcile_points(score), why,
				relation == VK_RECONCILE_RELATION_EQUAL ? VENTURE_MATCH_EXACT : VENTURE_MATCH_PARTIAL));
	}
	return g_steal_pointer(&result);
}
static void exact_iface_init(VentureReconciliationMatcherInterface *iface) { iface->suggest = exact_suggest; }
VentureExactMatcher *venture_exact_matcher_new(void) { return g_object_new(VENTURE_TYPE_EXACT_MATCHER, NULL); }
