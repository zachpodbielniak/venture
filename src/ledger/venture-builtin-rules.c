/*
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "venture.h"
#include "ledger/venture-ledger-private.h"

G_DECLARE_FINAL_TYPE(VentureBuiltinPostingRule, venture_builtin_posting_rule,
	VENTURE, BUILTIN_POSTING_RULE, GObject)
struct _VentureBuiltinPostingRule { GObject parent_instance; gchar *name; };
static void builtin_iface_init(VenturePostingRuleInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureBuiltinPostingRule, venture_builtin_posting_rule, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_POSTING_RULE, builtin_iface_init))

static const gchar *
builtin_name(VenturePostingRule *rule)
{
	return ((VentureBuiltinPostingRule *)rule)->name;
}

static gint64
default_account(VentureDatabase *db, gint64 org, const gchar *code,
	const gchar *name, VentureAccountKind kind, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(VentureAccount) created = NULL;
	g_autofree gchar *scoped_code = g_strdup_printf("%" G_GINT64_FORMAT ":%s", org, code);

	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	found = venture_database_find_one(db, query, error);
	if (NULL != found)
		return venture_entity_get_id(found);
	if (NULL != error && NULL != *error)
		return 0;
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, scoped_code, NULL);
	found = venture_database_find_one(db, query, error);
	if (NULL != found)
		return venture_entity_get_id(found);
	if (NULL != error && NULL != *error)
		return 0;
	/* Existing account codes are globally unique. Namespace new charts
	 * without changing that constraint or borrowing another entity's cash. */
	created = venture_account_new();
	g_object_set(created, "organization-id", org, "code", scoped_code,
		"name", name, "kind", kind, "active", TRUE, NULL);
	if (!venture_database_save(db, VENTURE_ENTITY(created), NULL, error))
		return 0;
	return venture_entity_get_id(VENTURE_ENTITY(created));
}

static GPtrArray *
builtin_build(VenturePostingRule *rule, VentureDatabase *db, VentureEntity *source, GError **error)
{
	const gchar *name = builtin_name(rule);
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) magnitude = NULL;
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
	gint64 debit;
	gint64 credit;
	gint64 org = venture_entity_get_organization_id(source);
	guint i;

	if (org <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Posting requires a legal entity");
		return NULL;
	}
	if (g_str_equal(name, "sale") && VENTURE_IS_SALE(source))
		/* Preserve Venture's documented net-proceeds convention. Detailed
		 * tax and revenue recognition policies belong in replacement rules. */
		amount = venture_sale_get_net(VENTURE_SALE(source), error);
	else if (g_str_equal(name, "expense") && VENTURE_IS_EXPENSE(source))
		g_object_get(source, "amount", &amount, NULL);
	else
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"This rule does not accept the source record type");
	if (NULL == amount)
		return NULL;
	if (amount->amount == G_MININT64)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Amount magnitude overflows");
		return NULL;
	}
	magnitude = venture_money_abs(amount);
	if (VENTURE_IS_EXPENSE(source))
	{
		debit = default_account(db, org, "6900", "General expenses", VENTURE_ACCOUNT_KIND_EXPENSE, error);
		if (debit == 0)
			return NULL;
		credit = 0;
		if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "cash-account-id") != NULL)
			g_object_get(source, "cash-account-id", &credit, NULL);
		if (credit <= 0)
			credit = default_account(db, org, "1000", "Cash", VENTURE_ACCOUNT_KIND_ASSET, error);
	}
	else
	{
		debit = default_account(db, org, "1000", "Cash", VENTURE_ACCOUNT_KIND_ASSET, error);
		if (debit == 0)
			return NULL;
		credit = default_account(db, org, "4000", "Sales", VENTURE_ACCOUNT_KIND_INCOME, error);
	}
	if (credit == 0)
		return NULL;
	for (i = 0; i < 2; i++)
	{
		VentureJournalLine *row = venture_journal_line_new();
		gboolean debit_side = (i == 0) != (amount->amount < 0);

		g_object_set(row, "account-id", i == 0 ? debit : credit, "amount", magnitude,
			"organization-id", org, "side", debit_side ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT, NULL);
		g_ptr_array_add(rows, row);
	}
	return g_steal_pointer(&rows);
}

static void
builtin_iface_init(VenturePostingRuleInterface *iface)
{
	iface->get_name = builtin_name;
	iface->build_lines = builtin_build;
}
static void
venture_builtin_posting_rule_set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	if (id == 1)
		((VentureBuiltinPostingRule *)object)->name = g_value_dup_string(value);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void
venture_builtin_posting_rule_get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_string(value, ((VentureBuiltinPostingRule *)object)->name);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void
venture_builtin_posting_rule_finalize(GObject *object)
{
	g_free(((VentureBuiltinPostingRule *)object)->name);
	G_OBJECT_CLASS(venture_builtin_posting_rule_parent_class)->finalize(object);
}
static void
venture_builtin_posting_rule_class_init(VentureBuiltinPostingRuleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->set_property = venture_builtin_posting_rule_set_property;
	object_class->get_property = venture_builtin_posting_rule_get_property;
	object_class->finalize = venture_builtin_posting_rule_finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_string("name", "Name", "Posting rule key", NULL,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void venture_builtin_posting_rule_init(VentureBuiltinPostingRule *self) { (void)self; }

void
venture_ledger_register_rules(VenturePostingRuleRegistry *registry)
{
	static const gchar *const names[] = { "sale", "expense" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++)
		venture_posting_rule_registry_add(registry, g_object_new(
			venture_builtin_posting_rule_get_type(), "name", names[i], NULL));
}
