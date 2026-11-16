/*
 * test-write-risk.c - Which writes may skip a person's approval
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture_write_risk_for_type() decides what --apply-writes=low-risk lets
 * an agent write without approval. Two failures matter: a record holding
 * money, people or access applied without a person, and ordinary work
 * staged so the setting does nothing. Money is derived from the field
 * table, so a new type with an amount in it is high risk by itself; the
 * walk below holds every registered type to that.
 */

#include <venture.h>

#include <glib.h>

static gboolean
has_money(GType type)
{
	g_autoptr(GTypeClass) klass = g_type_class_ref(type);
	g_autofree GParamSpec **properties = NULL;
	guint n = 0;
	guint i;

	properties = venture_entity_class_list_persistent_properties(
		VENTURE_ENTITY_CLASS(klass), &n);

	for (i = 0; i < n; i++)
	{
		if (VENTURE_TYPE_MONEY == properties[i]->value_type)
			return TRUE;
	}

	return FALSE;
}

/*
 * Every registered type is classified, and every type with an amount of
 * money in its field table is high risk.
 *
 * What breaks if this regresses: a type added later with money in it --
 * the next kind of invoice -- applies without approval under the setting
 * that exists to stop exactly that.
 */
static void
test_every_type_and_money(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(GError) error = NULL;
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	g_auto(GStrv) names = NULL;
	guint low = 0;
	guint high = 0;
	guint i;

	database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, registry, &error));
	context = venture_context_new(config, database);

	names = venture_entity_registry_list_names(registry);
	g_assert_cmpuint(g_strv_length(names), >, 100);

	for (i = 0; NULL != names[i]; i++)
	{
		GType type = venture_entity_registry_lookup_any(registry, names[i]);
		VentureWriteRisk risk;

		g_assert_true(G_TYPE_INVALID != type);
		risk = venture_write_risk_for_type(type);

		if (has_money(type))
		{
			g_test_message("%s carries money", names[i]);
			g_assert_cmpint(risk, ==, VENTURE_WRITE_RISK_HIGH);
		}

		/* a delete of anything is high */
		g_assert_cmpint(venture_write_risk_for_write(type, "DELETE"), ==,
		                VENTURE_WRITE_RISK_HIGH);

		if (VENTURE_WRITE_RISK_LOW == risk)
			low++;
		else
			high++;
	}

	/* the setting is useful only if both classes are populated */
	g_assert_cmpuint(low, >, 20);
	g_assert_cmpuint(high, >, 20);
}

/* The named cases the setting is about, one way and the other. */
static void
test_named_cases(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureContext) context = NULL;
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	static const gchar *const high[] = {
		"invoice", "payment", "expense", "journal", "agent_budget", "quote",
		"user", "api_token", "organization_membership", "team_membership",
		"session", "webhook", "forge", "integration_connection",
		"mail_message", "release", "deployment",
		/* each carries an amount -- a lifetime value, a target, an
		 * estimate -- so money, and high by the rule */
		"contact", "goal", "idea", NULL
	};
	static const gchar *const low[] = {
		"ticket", "ticket_comment", "worklog", "record_link",
		"kb_article", "dashboard", "research_note", "milestone", "sprint",
		"comment", NULL
	};
	guint i;

	database = venture_database_new("sqlite://:memory:", NULL);
	g_assert_true(venture_database_migrate(database, registry, NULL));
	context = venture_context_new(config, database);

	for (i = 0; NULL != high[i]; i++)
	{
		GType type = venture_entity_registry_lookup_any(registry, high[i]);

		g_test_message("%s", high[i]);
		g_assert_true(G_TYPE_INVALID != type);
		g_assert_cmpint(venture_write_risk_for_type(type), ==,
		                VENTURE_WRITE_RISK_HIGH);
	}

	for (i = 0; NULL != low[i]; i++)
	{
		GType type = venture_entity_registry_lookup_any(registry, low[i]);

		g_test_message("%s", low[i]);
		g_assert_true(G_TYPE_INVALID != type);
		g_assert_cmpint(venture_write_risk_for_type(type), ==,
		                VENTURE_WRITE_RISK_LOW);
		g_assert_cmpint(venture_write_risk_for_write(type, "PATCH"), ==,
		                VENTURE_WRITE_RISK_LOW);
	}

	/* and the schema says it, which is where an MCP client reads it */
	{
		g_autoptr(JsonNode) ticket = venture_entity_registry_describe(
			registry, "ticket");
		g_autoptr(JsonNode) invoice = venture_entity_registry_describe(
			registry, "invoice");

		g_assert_cmpstr(json_object_get_string_member(
			json_node_get_object(ticket), "write_risk"), ==, "low");
		g_assert_cmpstr(json_object_get_string_member(
			json_node_get_object(invoice), "write_risk"), ==, "high");
	}
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/write-risk/every-type-and-money",
	                test_every_type_and_money);
	g_test_add_func("/write-risk/named-cases", test_named_cases);

	return g_test_run();
}
