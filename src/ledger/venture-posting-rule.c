/*
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "venture.h"

G_DEFINE_INTERFACE(VenturePostingRule, venture_posting_rule, G_TYPE_OBJECT)
static void
venture_posting_rule_default_init(VenturePostingRuleInterface *iface)
{
	(void)iface;
}

const gchar *
venture_posting_rule_get_name(VenturePostingRule *self)
{
	g_return_val_if_fail(VENTURE_IS_POSTING_RULE(self), NULL);
	return VENTURE_POSTING_RULE_GET_IFACE(self)->get_name(self);
}

GPtrArray *
venture_posting_rule_build_lines(VenturePostingRule *self, VentureDatabase *db,
	VentureEntity *source, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_POSTING_RULE(self), NULL);
	if (NULL == VENTURE_POSTING_RULE_GET_IFACE(self)->build_lines)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
			"The posting rule cannot build lines");
		return NULL;
	}
	return VENTURE_POSTING_RULE_GET_IFACE(self)->build_lines(self, db, source, error);
}

struct _VenturePostingRuleRegistry
{
	GObject parent_instance;
	GHashTable *rules;
	gchar *revision;
};
G_DEFINE_FINAL_TYPE(VenturePostingRuleRegistry, venture_posting_rule_registry, G_TYPE_OBJECT)

static void
venture_posting_rule_registry_finalize(GObject *object)
{
	g_hash_table_unref(VENTURE_POSTING_RULE_REGISTRY(object)->rules);
	g_free(VENTURE_POSTING_RULE_REGISTRY(object)->revision);
	G_OBJECT_CLASS(venture_posting_rule_registry_parent_class)->finalize(object);
}
static void
venture_posting_rule_registry_class_init(VenturePostingRuleRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_posting_rule_registry_finalize;
}
static void
venture_posting_rule_registry_init(VenturePostingRuleRegistry *self)
{
	self->rules = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	self->revision = g_uuid_string_random();
}
VenturePostingRuleRegistry *
venture_posting_rule_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_POSTING_RULE_REGISTRY, NULL);
}
void
venture_posting_rule_registry_add(VenturePostingRuleRegistry *self, VenturePostingRule *rule)
{
	const gchar *name;

	g_return_if_fail(VENTURE_IS_POSTING_RULE_REGISTRY(self));
	g_return_if_fail(VENTURE_IS_POSTING_RULE(rule));
	name = venture_posting_rule_get_name(rule);
	g_return_if_fail(NULL != name && '\0' != *name);
	/* Names do not capture replaced callback implementations. A fresh
	 * process or registry mutation invalidates outstanding consent. */
	g_hash_table_replace(self->rules, g_strdup(name), rule);
	g_free(self->revision);
	self->revision = g_uuid_string_random();
}
VenturePostingRule *
venture_posting_rule_registry_lookup(VenturePostingRuleRegistry *self, const gchar *name)
{
	g_return_val_if_fail(VENTURE_IS_POSTING_RULE_REGISTRY(self), NULL);
	g_return_val_if_fail(NULL != name, NULL);
	return g_hash_table_lookup(self->rules, name);
}
gboolean
venture_posting_rule_registry_remove(VenturePostingRuleRegistry *self, const gchar *name)
{
	g_return_val_if_fail(VENTURE_IS_POSTING_RULE_REGISTRY(self), FALSE);
	if (!g_hash_table_remove(self->rules, name)) return FALSE;
	g_free(self->revision);
	self->revision = g_uuid_string_random();
	return TRUE;
}
const gchar *
venture_posting_rule_registry_get_revision(VenturePostingRuleRegistry *self)
{
	g_return_val_if_fail(VENTURE_IS_POSTING_RULE_REGISTRY(self), NULL);
	return self->revision;
}

static gint
compare_rules(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(venture_posting_rule_get_name(*(VenturePostingRule *const *)a),
		venture_posting_rule_get_name(*(VenturePostingRule *const *)b));
}
GPtrArray *
venture_posting_rule_registry_list(VenturePostingRuleRegistry *self)
{
	GPtrArray *result = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;

	g_hash_table_iter_init(&iter, self->rules);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(result, value);
	g_ptr_array_sort(result, compare_rules);
	return result;
}

G_DEFINE_INTERFACE(VentureExchangePolicy, venture_exchange_policy, G_TYPE_OBJECT)
static void
venture_exchange_policy_default_init(VentureExchangePolicyInterface *iface)
{
	(void)iface;
}
const gchar *
venture_exchange_policy_get_name(VentureExchangePolicy *self)
{
	g_return_val_if_fail(VENTURE_IS_EXCHANGE_POLICY(self), NULL);
	if (NULL == VENTURE_EXCHANGE_POLICY_GET_IFACE(self)->get_name)
		return NULL;
	return VENTURE_EXCHANGE_POLICY_GET_IFACE(self)->get_name(self);
}
VentureMoney *
venture_exchange_policy_convert(VentureExchangePolicy *self, const VentureMoney *amount,
	const gchar *currency, GDateTime *when, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_EXCHANGE_POLICY(self), NULL);
	if (NULL == VENTURE_EXCHANGE_POLICY_GET_IFACE(self)->convert)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
			"The exchange policy cannot value this amount");
		return NULL;
	}
	return VENTURE_EXCHANGE_POLICY_GET_IFACE(self)->convert(self, amount, currency, when, error);
}
