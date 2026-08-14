/*
 * test-entity.c - The record base class, the registry and the record types
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The base class is generic, so a bug in it is a bug in every record type at
 * once. These tests pin down the behaviour everything else assumes: that
 * declarative fields really store values, that serialisation round-trips,
 * that sensitive fields never leak, and that the derived money figures are
 * computed the way the reports expect.
 */

#include <venture.h>

/* --- Registry ------------------------------------------------------------ */

static void
test_registry_has_builtins(void)
{
	VentureEntityRegistry *registry;
	g_auto(GStrv) names = NULL;

	registry = venture_entity_registry_get_default();
	names = venture_entity_registry_list_names(registry);

	g_assert_nonnull(names);
	g_assert_cmpuint(g_strv_length(names), ==, 24);

	g_assert_true(g_strv_contains((const gchar * const *)names, "organization"));
	g_assert_true(g_strv_contains((const gchar * const *)names, "venture"));
	g_assert_true(g_strv_contains((const gchar * const *)names, "sale"));
	g_assert_true(g_strv_contains((const gchar * const *)names, "inventory_txn"));
	g_assert_true(g_strv_contains((const gchar * const *)names, "api_token"));
}

static void
test_registry_lookup_singular_and_plural(void)
{
	VentureEntityRegistry *registry;

	registry = venture_entity_registry_get_default();

	/* A caller holding a REST path segment or a table name should not
	 * have to singularise it first. */
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "sale"),
	                 ==, VENTURE_TYPE_SALE);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "sales"),
	                 ==, VENTURE_TYPE_SALE);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "tax_categories"),
	                 ==, VENTURE_TYPE_TAX_CATEGORY);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "nonesuch"),
	                 ==, G_TYPE_INVALID);
}

static void
test_registry_create_unknown_lists_alternatives(void)
{
	VentureEntityRegistry *registry;
	g_autoptr(VentureEntity) entity = NULL;
	g_autoptr(GError) error = NULL;

	registry = venture_entity_registry_get_default();
	entity = venture_entity_registry_create(registry, "widget", &error);

	g_assert_null(entity);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);

	/* The message is fed straight back to the AI as a tool result, so it
	 * has to say what the valid options are. */
	g_assert_nonnull(g_strstr_len(error->message, -1, "sale"));
}

static void
test_registry_rejects_abstract(void)
{
	g_autoptr(VentureEntityRegistry) registry = NULL;
	g_autoptr(GError) error = NULL;

	registry = venture_entity_registry_new();

	g_assert_false(venture_entity_registry_register(registry,
	                                                VENTURE_TYPE_ENTITY,
	                                                &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

static void
test_registry_rejects_duplicate_name(void)
{
	g_autoptr(VentureEntityRegistry) registry = NULL;
	g_autoptr(GError) error = NULL;

	registry = venture_entity_registry_new();

	g_assert_true(venture_entity_registry_register(registry,
	                                               VENTURE_TYPE_SALE, NULL));

	/* Registering the same type twice is a reload and must be tolerated. */
	g_assert_true(venture_entity_registry_register(registry,
	                                               VENTURE_TYPE_SALE, &error));
	g_assert_no_error(error);
}

static void
test_registry_describe(void)
{
	VentureEntityRegistry *registry;
	g_autoptr(JsonNode) description = NULL;
	JsonObject *object;

	registry = venture_entity_registry_get_default();
	description = venture_entity_registry_describe(registry, "expense");

	g_assert_nonnull(description);
	object = json_node_get_object(description);

	g_assert_cmpstr(json_object_get_string_member(object, "name"), ==, "expense");
	g_assert_cmpstr(json_object_get_string_member(object, "table"), ==, "expenses");
	g_assert_cmpuint(json_array_get_length(
		json_object_get_array_member(object, "fields")), >, 5);
}

static void
test_registry_table_names_pluralise_correctly(void)
{
	VentureEntityRegistry *registry;

	registry = venture_entity_registry_get_default();

	g_assert_cmpstr(venture_entity_registry_get_table_name(registry, "venture"),
	                ==, "ventures");
	/* "category" -> "categories", not "categorys". */
	g_assert_cmpstr(venture_entity_registry_get_table_name(registry, "tax_category"),
	                ==, "tax_categories");
	/* "expense" ends in a sibilant and takes -s, not -es. */
	g_assert_cmpstr(venture_entity_registry_get_table_name(registry, "expense"),
	                ==, "expenses");
}

/* --- Identity spine ------------------------------------------------------ */

static void
test_entity_identity(void)
{
	g_autoptr(VentureVenture) venture = NULL;
	VentureEntity *entity;

	venture = venture_venture_new();
	entity = VENTURE_ENTITY(venture);

	g_assert_cmpint(venture_entity_get_id(entity), ==, 0);
	g_assert_false(venture_entity_is_persisted(entity));
	g_assert_false(venture_entity_is_deleted(entity));
	g_assert_cmpint(venture_entity_get_version(entity), ==, 0);

	/* The UUID is generated lazily but must be stable once observed. */
	{
		const gchar *first;
		g_autofree gchar *copy = NULL;

		first = venture_entity_get_uuid(entity);
		g_assert_nonnull(first);
		copy = g_strdup(first);
		g_assert_cmpstr(venture_entity_get_uuid(entity), ==, copy);
	}

	g_assert_nonnull(venture_entity_get_created_at(entity));
	g_assert_cmpstr(venture_entity_get_entity_name(entity), ==, "venture");
	g_assert_cmpstr(venture_entity_get_table_name(entity), ==, "ventures");
}

static void
test_entity_touch_increments_version(void)
{
	g_autoptr(VentureTask) task = NULL;
	VentureEntity *entity;

	task = venture_task_new();
	entity = VENTURE_ENTITY(task);

	venture_entity_touch(entity);
	g_assert_cmpint(venture_entity_get_version(entity), ==, 1);

	venture_entity_touch(entity);
	g_assert_cmpint(venture_entity_get_version(entity), ==, 2);
}

static void
test_entity_display_name_falls_back(void)
{
	g_autoptr(VentureContact) contact = NULL;
	g_autofree gchar *named = NULL;
	g_autofree gchar *unnamed = NULL;

	contact = venture_contact_new();

	/* With nothing to go on, the label still identifies the record. */
	unnamed = venture_entity_get_display_name(VENTURE_ENTITY(contact));
	g_assert_nonnull(unnamed);
	g_assert_true(g_str_has_prefix(unnamed, "contact #"));

	g_object_set(contact, "name", "Ada Lovelace", NULL);
	named = venture_entity_get_display_name(VENTURE_ENTITY(contact));
	g_assert_cmpstr(named, ==, "Ada Lovelace");
}

/* --- Declarative fields -------------------------------------------------- */

static void
test_entity_fields_store_values(void)
{
	g_autoptr(VentureProduct) product = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *genre = NULL;
	g_autoptr(VentureMoney) price = NULL;
	gint64 venture_id;

	product = venture_product_new();

	g_object_set(product,
	             "name", "The Long Way Round",
	             "genre", "science-fiction",
	             "venture-id", (gint64)7,
	             NULL);

	{
		g_autoptr(VentureMoney) set_price = NULL;

		set_price = venture_money_new(1499, "USD", 2);
		g_object_set(product, "list-price", set_price, NULL);
	}

	g_object_get(product,
	             "name", &name,
	             "genre", &genre,
	             "venture-id", &venture_id,
	             "list-price", &price,
	             NULL);

	g_assert_cmpstr(name, ==, "The Long Way Round");
	g_assert_cmpstr(genre, ==, "science-fiction");
	g_assert_cmpint(venture_id, ==, 7);
	g_assert_nonnull(price);
	g_assert_cmpint(venture_money_get_amount(price), ==, 1499);
}

static void
test_entity_unset_field_reads_as_default(void)
{
	g_autoptr(VentureProduct) product = NULL;
	g_autoptr(VentureInventoryItem) item = NULL;
	g_autofree gchar *name = NULL;
	g_autoptr(VentureMoney) price = NULL;
	gint64 reorder_point;

	product = venture_product_new();
	item = venture_inventory_item_new();

	g_object_get(product, "name", &name, "list-price", &price, NULL);
	g_object_get(item, "reorder-point", &reorder_point, NULL);

	/* A never-set field reads as its type's default rather than as
	 * whatever happened to be in memory. */
	g_assert_null(name);
	g_assert_null(price);
	g_assert_cmpint(reorder_point, ==, 0);
}

static void
test_entity_set_field_from_string(void)
{
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) occurred = NULL;
	VentureDeductibility deductibility;

	expense = venture_expense_new();

	/* The form decoder, the CSV importer and the CLI all arrive with
	 * strings, whatever the field's real type. */
	g_assert_true(venture_entity_set_field_from_string(
		VENTURE_ENTITY(expense), "amount", "$42.50", &error));
	g_assert_no_error(error);

	g_assert_true(venture_entity_set_field_from_string(
		VENTURE_ENTITY(expense), "occurred-at", "2026-03-14", &error));
	g_assert_no_error(error);

	g_assert_true(venture_entity_set_field_from_string(
		VENTURE_ENTITY(expense), "deductibility", "partial", &error));
	g_assert_no_error(error);

	g_object_get(expense,
	             "amount", &amount,
	             "occurred-at", &occurred,
	             "deductibility", &deductibility,
	             NULL);

	g_assert_cmpint(venture_money_get_amount(amount), ==, 4250);
	g_assert_cmpint(g_date_time_get_year(occurred), ==, 2026);
	g_assert_cmpint(g_date_time_get_month(occurred), ==, 3);
	g_assert_cmpint(deductibility, ==, VENTURE_DEDUCTIBILITY_PARTIAL);
}

static void
test_entity_set_unknown_field_fails(void)
{
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(GError) error = NULL;

	expense = venture_expense_new();

	g_assert_false(venture_entity_set_field_from_string(
		VENTURE_ENTITY(expense), "not-a-field", "x", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

static void
test_entity_enum_field_rejects_bad_value(void)
{
	g_autoptr(VentureTask) task = NULL;
	g_autoptr(GError) error = NULL;

	task = venture_task_new();

	g_assert_false(venture_entity_set_field_from_string(
		VENTURE_ENTITY(task), "status", "sideways", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION);

	/* The message must enumerate the valid values, since the AI reads it. */
	g_assert_nonnull(g_strstr_len(error->message, -1, "in_progress"));
}

static void
test_entity_enum_field_accepts_spelling_variants(void)
{
	g_autoptr(VentureTask) task = NULL;
	VentureTaskStatus status;

	task = venture_task_new();

	/* Hyphen, underscore and upper case all reach the same value: these
	 * strings come from a CLI flag, a form and an AI in equal measure. */
	g_assert_true(venture_entity_set_field_from_string(
		VENTURE_ENTITY(task), "status", "in-progress", NULL));
	g_object_get(task, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_TASK_STATUS_IN_PROGRESS);

	g_assert_true(venture_entity_set_field_from_string(
		VENTURE_ENTITY(task), "status", "DONE", NULL));
	g_object_get(task, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_TASK_STATUS_DONE);
}

/* --- Custom attributes --------------------------------------------------- */

static void
test_entity_attributes(void)
{
	g_autoptr(VentureProduct) product = NULL;
	g_autoptr(GList) keys = NULL;

	product = venture_product_new();

	venture_entity_set_attribute(VENTURE_ENTITY(product), "etsy_listing", "123456");
	venture_entity_set_attribute(VENTURE_ENTITY(product), "material", "walnut");

	g_assert_cmpstr(venture_entity_get_attribute(VENTURE_ENTITY(product),
	                                             "etsy_listing"), ==, "123456");

	keys = venture_entity_list_attributes(VENTURE_ENTITY(product));
	g_assert_cmpuint(g_list_length(keys), ==, 2);

	/* Setting NULL removes rather than storing an empty value. */
	venture_entity_set_attribute(VENTURE_ENTITY(product), "material", NULL);
	g_assert_null(venture_entity_get_attribute(VENTURE_ENTITY(product),
	                                           "material"));
}

/* --- Serialisation ------------------------------------------------------- */

static void
test_entity_json_round_trip(void)
{
	g_autoptr(VentureSale) original = NULL;
	g_autoptr(VentureSale) restored = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) gross = NULL;

	original = venture_sale_new();
	gross = venture_money_new(2599, "USD", 2);

	g_object_set(original,
	             "venture-id", (gint64)3,
	             "channel", "etsy",
	             "external-id", "ORD-1001",
	             "quantity", (gint64)2,
	             "gross", gross,
	             NULL);

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(original), FALSE);
	g_assert_nonnull(node);

	restored = venture_sale_new();
	g_assert_true(venture_serializable_from_json(VENTURE_SERIALIZABLE(restored),
	                                             node, &error));
	g_assert_no_error(error);

	g_assert_true(venture_entity_equal(VENTURE_ENTITY(original),
	                                   VENTURE_ENTITY(restored)));
}

static void
test_entity_json_uses_underscored_members(void)
{
	g_autoptr(VentureSale) sale = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object;

	sale = venture_sale_new();
	g_object_set(sale, "venture-id", (gint64)3, NULL);

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(sale), FALSE);
	object = json_node_get_object(node);

	/* Properties are hyphenated as GObject requires; the wire format is
	 * underscored. Both vocabularies must never be maintained by hand. */
	g_assert_true(json_object_has_member(object, "venture_id"));
	g_assert_false(json_object_has_member(object, "venture-id"));

	g_assert_cmpstr(json_object_get_string_member(object, "type"), ==, "sale");
	g_assert_true(json_object_has_member(object, "display_name"));
}

static void
test_entity_partial_json_leaves_other_fields(void)
{
	g_autoptr(VentureContact) contact = NULL;
	g_autoptr(JsonNode) patch = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *company = NULL;

	contact = venture_contact_new();
	g_object_set(contact, "name", "Ada", "company", "Analytical", NULL);

	/* An absent member must leave the stored value alone, which is what
	 * gives PATCH semantics for free and stops a partial client blanking
	 * fields it never knew about. */
	patch = venture_json_parse("{\"name\": \"Ada Lovelace\"}", &error);
	g_assert_no_error(error);

	g_assert_true(venture_serializable_from_json(VENTURE_SERIALIZABLE(contact),
	                                             patch, &error));
	g_assert_no_error(error);

	g_object_get(contact, "name", &name, "company", &company, NULL);
	g_assert_cmpstr(name, ==, "Ada Lovelace");
	g_assert_cmpstr(company, ==, "Analytical");
}

static void
test_entity_sensitive_fields_withheld(void)
{
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(JsonNode) public_node = NULL;
	g_autoptr(JsonNode) private_node = NULL;
	const gchar * const *sensitive;

	user = venture_user_new();
	g_object_set(user, "username", "zach", "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "correct horse", 100000, NULL));

	sensitive = venture_serializable_get_sensitive_fields(
		VENTURE_SERIALIZABLE(user));
	g_assert_nonnull(sensitive);
	g_assert_true(g_strv_contains(sensitive, "password_hash"));

	/* Omitted entirely rather than nulled: a client must not be able to
	 * tell "no password set" from "password set but withheld". */
	public_node = venture_serializable_to_json(VENTURE_SERIALIZABLE(user), FALSE);
	g_assert_false(json_object_has_member(json_node_get_object(public_node),
	                                      "password_hash"));

	private_node = venture_serializable_to_json(VENTURE_SERIALIZABLE(user), TRUE);
	g_assert_true(json_object_has_member(json_node_get_object(private_node),
	                                     "password_hash"));
}

static void
test_entity_yaml_export(void)
{
	g_autoptr(VentureIdea) idea = NULL;
	g_autofree gchar *yaml = NULL;

	idea = venture_idea_new();
	g_object_set(idea, "title", "Sell templates", NULL);

	yaml = venture_serializable_to_yaml(VENTURE_SERIALIZABLE(idea), FALSE);

	g_assert_nonnull(yaml);
	g_assert_nonnull(g_strstr_len(yaml, -1, "Sell templates"));
}

/* --- Diffing and duplication --------------------------------------------- */

static void
test_entity_diff_reports_changes(void)
{
	g_autoptr(VentureDeal) before = NULL;
	g_autoptr(VentureDeal) after = NULL;
	g_autoptr(JsonNode) diff = NULL;
	JsonObject *object;

	before = venture_deal_new();
	g_object_set(before, "name", "Big order", "probability", (gint64)25, NULL);

	after = venture_deal_new();
	venture_entity_copy_properties_from(VENTURE_ENTITY(after),
	                                    VENTURE_ENTITY(before), TRUE);
	g_object_set(after, "probability", (gint64)75, NULL);

	diff = venture_entity_diff(VENTURE_ENTITY(before), VENTURE_ENTITY(after));
	object = json_node_get_object(diff);

	g_assert_cmpuint(json_object_get_size(object), ==, 1);
	g_assert_true(json_object_has_member(object, "probability"));

	{
		JsonObject *change;

		change = json_object_get_object_member(object, "probability");
		g_assert_cmpint(json_object_get_int_member(change, "from"), ==, 25);
		g_assert_cmpint(json_object_get_int_member(change, "to"), ==, 75);
	}
}

static void
test_entity_diff_redacts_sensitive(void)
{
	g_autoptr(VentureUser) before = NULL;
	g_autoptr(VentureUser) after = NULL;
	g_autoptr(JsonNode) diff = NULL;
	JsonObject *change;

	before = venture_user_new();
	g_object_set(before, "username", "zach", NULL);
	g_assert_true(venture_user_set_password(before, "one", 100000, NULL));

	after = venture_user_new();
	venture_entity_copy_properties_from(VENTURE_ENTITY(after),
	                                    VENTURE_ENTITY(before), TRUE);
	g_assert_true(venture_user_set_password(after, "two", 100000, NULL));

	diff = venture_entity_diff(VENTURE_ENTITY(before), VENTURE_ENTITY(after));
	change = json_object_get_object_member(json_node_get_object(diff),
	                                       "password_hash");

	/* The operator must learn that the password changed, and must not
	 * learn either hash from an audit log that is itself readable. */
	g_assert_nonnull(change);
	g_assert_true(json_object_get_boolean_member(change, "redacted"));
	g_assert_false(json_object_has_member(change, "from"));
	g_assert_false(json_object_has_member(change, "to"));
}

static void
test_entity_duplicate_is_unsaved(void)
{
	g_autoptr(VentureProduct) original = NULL;
	g_autoptr(VentureEntity) copy = NULL;
	g_autofree gchar *name = NULL;

	original = venture_product_new();
	g_object_set(original, "name", "Print set", NULL);
	venture_entity_set_id(VENTURE_ENTITY(original), 42);
	venture_entity_set_attribute(VENTURE_ENTITY(original), "size", "A3");

	copy = venture_entity_duplicate(VENTURE_ENTITY(original));

	/* The copy must insert as a new row rather than update the original. */
	g_assert_cmpint(venture_entity_get_id(copy), ==, 0);
	g_assert_false(venture_entity_is_persisted(copy));
	g_assert_cmpstr(venture_entity_get_uuid(copy), !=,
	                venture_entity_get_uuid(VENTURE_ENTITY(original)));

	g_object_get(copy, "name", &name, NULL);
	g_assert_cmpstr(name, ==, "Print set");
	g_assert_cmpstr(venture_entity_get_attribute(copy, "size"), ==, "A3");
}

/* --- Validation ---------------------------------------------------------- */

static void
test_entity_validation_requires_not_null_strings(void)
{
	g_autoptr(VentureAccount) account = NULL;
	g_autoptr(GError) error = NULL;

	account = venture_account_new();

	/* Code and name are both declared NOT NULL, and an empty string does
	 * not satisfy that even though a database would accept it. */
	g_assert_false(venture_entity_validate(VENTURE_ENTITY(account), &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);

	g_clear_error(&error);
	g_object_set(account, "code", "4000", "name", "Sales", NULL);

	g_assert_true(venture_entity_validate(VENTURE_ENTITY(account), &error));
	g_assert_no_error(error);
}

static void
test_entity_validation_rejects_whitespace_only(void)
{
	g_autoptr(VentureAccount) account = NULL;
	g_autoptr(GError) error = NULL;

	account = venture_account_new();
	g_object_set(account, "code", "   ", "name", "Sales", NULL);

	g_assert_false(venture_entity_validate(VENTURE_ENTITY(account), &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* --- Derived money figures ----------------------------------------------- */

static void
test_sale_net(void)
{
	g_autoptr(VentureSale) sale = NULL;
	g_autoptr(VentureMoney) net = NULL;
	g_autoptr(GError) error = NULL;

	sale = venture_sale_new();

	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(sale),
		"gross", "25.00", NULL));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(sale),
		"shipping-collected", "5.00", NULL));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(sale),
		"discount", "2.00", NULL));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(sale),
		"fees", "3.50", NULL));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(sale),
		"shipping-cost", "4.25", NULL));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(sale),
		"refunded", "0.00", NULL));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(sale),
		"tax-remitted", "1.25", NULL));

	net = venture_sale_get_net(sale, &error);

	g_assert_no_error(error);
	/* 25.00 + 5.00 - 2.00 - 3.50 - 4.25 - 0.00 - 1.25 = 19.00 */
	g_assert_cmpint(venture_money_get_amount(net), ==, 1900);
}

static void
test_sale_net_with_no_amounts_is_zero(void)
{
	g_autoptr(VentureSale) sale = NULL;
	g_autoptr(VentureMoney) net = NULL;
	g_autoptr(GError) error = NULL;

	sale = venture_sale_new();
	net = venture_sale_get_net(sale, &error);

	g_assert_no_error(error);
	g_assert_true(venture_money_is_zero(net));
}

static void
test_expense_deductible_amount(void)
{
	struct
	{
		VentureDeductibility	 deductibility;
		gint64			 business_use;
		gint64			 expected;
	} cases[] = {
		{ VENTURE_DEDUCTIBILITY_FULL,    100, 10000 },
		{ VENTURE_DEDUCTIBILITY_PARTIAL,  60,  6000 },
		{ VENTURE_DEDUCTIBILITY_NONE,    100,     0 },
		/* Capitalised costs are depreciated, not expensed. */
		{ VENTURE_DEDUCTIBILITY_CAPITAL, 100,     0 },
		/* Unreviewed must never inflate a deduction. */
		{ VENTURE_DEDUCTIBILITY_REVIEW,  100,     0 },
		/* An out-of-range percentage is clamped, not trusted. */
		{ VENTURE_DEDUCTIBILITY_PARTIAL, 150, 10000 },
		{ VENTURE_DEDUCTIBILITY_PARTIAL, -10,     0 }
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(VentureExpense) expense = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) deductible = NULL;
		g_autoptr(GError) error = NULL;

		expense = venture_expense_new();
		amount = venture_money_new(10000, "USD", 2);

		g_object_set(expense,
		             "amount", amount,
		             "deductibility", cases[i].deductibility,
		             "business-use-percent", cases[i].business_use,
		             NULL);

		deductible = venture_expense_get_deductible_amount(expense, &error);

		g_assert_no_error(error);
		g_assert_cmpint(venture_money_get_amount(deductible), ==,
		                cases[i].expected);
	}
}

static void
test_deal_weighted_value(void)
{
	struct
	{
		VentureDealStage	 stage;
		gint64			 probability;
		gint64			 expected;
	} cases[] = {
		{ VENTURE_DEAL_STAGE_LEAD,        25, 2500 },
		{ VENTURE_DEAL_STAGE_NEGOTIATION, 80, 8000 },
		/* A closed outcome overrides the stored probability, which
		 * nobody updates on the way out. */
		{ VENTURE_DEAL_STAGE_WON,         10, 10000 },
		{ VENTURE_DEAL_STAGE_LOST,        90, 0 }
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(VentureDeal) deal = NULL;
		g_autoptr(VentureMoney) value = NULL;
		g_autoptr(VentureMoney) weighted = NULL;
		g_autoptr(GError) error = NULL;

		deal = venture_deal_new();
		value = venture_money_new(10000, "USD", 2);

		g_object_set(deal,
		             "value", value,
		             "stage", cases[i].stage,
		             "probability", cases[i].probability,
		             NULL);

		weighted = venture_deal_get_weighted_value(deal, &error);

		g_assert_no_error(error);
		g_assert_cmpint(venture_money_get_amount(weighted), ==,
		                cases[i].expected);
	}
}

static void
test_campaign_roi(void)
{
	g_autoptr(VentureCampaign) campaign = NULL;
	g_autoptr(VentureMoney) spend = NULL;
	g_autoptr(VentureMoney) revenue = NULL;

	campaign = venture_campaign_new();
	spend = venture_money_new(10000, "USD", 2);
	revenue = venture_money_new(25000, "USD", 2);

	g_object_set(campaign, "spend", spend, "revenue", revenue, NULL);
	g_assert_cmpfloat_with_epsilon(venture_campaign_get_roi(campaign),
	                               1.5, 0.0001);

	/* No spend must not produce an infinity that tops every ranking. */
	g_object_set(campaign, "spend", NULL, NULL);
	g_assert_cmpfloat(venture_campaign_get_roi(campaign), ==, 0.0);
}

static void
test_idea_score(void)
{
	g_autoptr(VentureIdea) idea = NULL;

	idea = venture_idea_new();

	/* Unrated scores zero rather than dividing by zero or flattering an
	 * idea nobody has actually assessed. */
	g_assert_cmpfloat(venture_idea_get_score(idea), ==, 0.0);

	g_object_set(idea,
	             "opportunity", (gint64)8,
	             "confidence", (gint64)6,
	             "effort", (gint64)4,
	             NULL);

	g_assert_cmpfloat_with_epsilon(venture_idea_get_score(idea), 12.0, 0.0001);

	/* Effort divides, so a cheaper idea of equal promise ranks higher. */
	g_object_set(idea, "effort", (gint64)2, NULL);
	g_assert_cmpfloat_with_epsilon(venture_idea_get_score(idea), 24.0, 0.0001);
}

/* --- Credentials --------------------------------------------------------- */

static void
test_user_password(void)
{
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *hash = NULL;

	user = venture_user_new();
	g_object_set(user, "username", "zach", "active", TRUE, NULL);

	g_assert_true(venture_user_set_password(user, "correct horse battery", 100000, NULL));

	g_object_get(user, "password-hash", &hash, NULL);
	g_assert_nonnull(hash);
	g_assert_true(g_str_has_prefix(hash, "pbkdf2-sha256$"));
	/* The plaintext must not survive anywhere in the record. */
	g_assert_null(g_strstr_len(hash, -1, "correct horse battery"));

	g_assert_true(venture_user_check_password(user, "correct horse battery"));
	g_assert_false(venture_user_check_password(user, "wrong"));
	g_assert_false(venture_user_check_password(user, NULL));
}

static void
test_user_inactive_cannot_authenticate(void)
{
	g_autoptr(VentureUser) user = NULL;

	user = venture_user_new();
	g_object_set(user, "username", "zach", "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "hunter2hunter2", 100000, NULL));

	g_assert_true(venture_user_check_password(user, "hunter2hunter2"));

	/* Deactivation must deny access regardless of the password. */
	g_object_set(user, "active", FALSE, NULL);
	g_assert_false(venture_user_check_password(user, "hunter2hunter2"));
}

static void
test_user_empty_password_rejected(void)
{
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(GError) error = NULL;

	user = venture_user_new();

	g_assert_false(venture_user_set_password(user, "", 100000, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_api_token(void)
{
	g_autoptr(VentureApiToken) token = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *prefix = NULL;

	token = venture_api_token_new();
	g_object_set(token, "name", "venturectl on the laptop", NULL);

	secret = venture_api_token_generate(token);

	g_assert_nonnull(secret);
	g_assert_true(g_str_has_prefix(secret, "vk_"));

	g_object_get(token, "token-hash", &hash, "prefix", &prefix, NULL);

	/* Only the hash is stored, so a leaked database yields no usable
	 * tokens. The prefix is deliberately in the clear so a token can be
	 * identified in a list without revealing it. */
	g_assert_nonnull(hash);
	g_assert_null(g_strstr_len(hash, -1, secret + 3));
	g_assert_cmpuint(strlen(prefix), ==, 8);

	g_assert_true(venture_api_token_matches(token, secret));
	/* Accepted with or without the display prefix. */
	g_assert_true(venture_api_token_matches(token, secret + 3));
	g_assert_false(venture_api_token_matches(token, "vk_wrong"));
	g_assert_false(venture_api_token_matches(token, NULL));
}

static void
test_api_token_expiry(void)
{
	g_autoptr(VentureApiToken) token = NULL;
	g_autofree gchar *secret = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) past = NULL;

	token = venture_api_token_new();
	secret = venture_api_token_generate(token);

	g_assert_true(venture_api_token_matches(token, secret));

	now = venture_time_now();
	past = g_date_time_add_days(now, -1);
	g_object_set(token, "expires-at", past, NULL);

	g_assert_false(venture_api_token_matches(token, secret));
}

static void
test_api_token_inactive(void)
{
	g_autoptr(VentureApiToken) token = NULL;
	g_autofree gchar *secret = NULL;

	token = venture_api_token_new();
	secret = venture_api_token_generate(token);

	g_object_set(token, "active", FALSE, NULL);
	g_assert_false(venture_api_token_matches(token, secret));
}

/* --- Audit --------------------------------------------------------------- */

static void
test_audit_entry_for_change(void)
{
	g_autoptr(VentureVenture) target = NULL;
	g_autoptr(VentureAuditEntry) entry = NULL;
	g_autoptr(JsonNode) diff = NULL;
	g_autofree gchar *target_type = NULL;
	g_autofree gchar *target_label = NULL;
	g_autofree gchar *stored_diff = NULL;
	VentureActorKind actor_kind;
	gint64 target_id;

	target = venture_venture_new();
	g_object_set(target, "name", "Etsy shop", NULL);
	venture_entity_set_id(VENTURE_ENTITY(target), 12);
	venture_entity_set_organization_id(VENTURE_ENTITY(target), 3);

	diff = venture_json_parse("{\"status\": {\"from\": \"idea\", \"to\": \"active\"}}",
	                          NULL);

	entry = venture_audit_entry_new_for_change(VENTURE_AUDIT_ACTION_UPDATE,
	                                           VENTURE_ACTOR_KIND_AI,
	                                           "claude-sonnet-5",
	                                           VENTURE_ENTITY(target),
	                                           diff);

	g_object_get(entry,
	             "target-type", &target_type,
	             "target-id", &target_id,
	             "target-label", &target_label,
	             "actor-kind", &actor_kind,
	             "diff", &stored_diff,
	             NULL);

	g_assert_cmpstr(target_type, ==, "venture");
	g_assert_cmpint(target_id, ==, 12);
	g_assert_cmpstr(target_label, ==, "Etsy shop");
	g_assert_cmpint(actor_kind, ==, VENTURE_ACTOR_KIND_AI);
	g_assert_nonnull(g_strstr_len(stored_diff, -1, "active"));

	/* The audit record inherits the organisation of what it describes, so
	 * an organisation-scoped query sees its own history and no one else's. */
	g_assert_cmpint(venture_entity_get_organization_id(VENTURE_ENTITY(entry)),
	                ==, 3);
}

/* --- Field specs --------------------------------------------------------- */

static void
test_entity_field_specs_derived_from_properties(void)
{
	g_autoptr(VentureProduct) product = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	gboolean found_genre = FALSE;
	gboolean found_price = FALSE;
	gboolean found_id = FALSE;
	guint i;

	product = venture_product_new();
	specs = venture_entity_get_field_specs(VENTURE_ENTITY(product));

	g_assert_nonnull(specs);
	g_assert_cmpuint(specs->len, >, 10);

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;
		const gchar *name;

		spec = g_ptr_array_index(specs, i);
		name = venture_field_spec_get_name(spec);

		if (0 == g_strcmp0(name, "genre"))
		{
			found_genre = TRUE;
			g_assert_cmpint(venture_field_spec_get_kind(spec), ==,
			                VENTURE_FIELD_KIND_STRING);
		}

		if (0 == g_strcmp0(name, "list-price"))
		{
			found_price = TRUE;
			g_assert_cmpint(venture_field_spec_get_kind(spec), ==,
			                VENTURE_FIELD_KIND_MONEY);
		}

		if (0 == g_strcmp0(name, "id"))
			found_id = TRUE;
	}

	g_assert_true(found_genre);
	g_assert_true(found_price);
	/* The identity spine is machinery, not data the operator edits, so it
	 * must stay out of generated forms and schemas. */
	g_assert_false(found_id);
}

static void
test_entity_field_spec_enum_choices_from_gtype(void)
{
	g_autoptr(VentureTask) task = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	guint i;

	task = venture_task_new();
	specs = venture_entity_get_field_specs(VENTURE_ENTITY(task));

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		if (0 != g_strcmp0(venture_field_spec_get_name(spec), "status"))
			continue;

		/* The select options and the AI schema come straight from the
		 * registered GEnum, so they cannot drift from the C definition. */
		g_assert_cmpint(venture_field_spec_get_kind(spec), ==,
		                VENTURE_FIELD_KIND_ENUM);
		g_assert_true(g_strv_contains(
			(const gchar * const *)venture_field_spec_get_choices(spec),
			"in_progress"));
		return;
	}

	g_assert_not_reached();
}

static void
test_entity_field_spec_reference_target(void)
{
	g_autoptr(VentureSale) sale = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	guint i;

	sale = venture_sale_new();
	specs = venture_entity_get_field_specs(VENTURE_ENTITY(sale));

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		if (0 != g_strcmp0(venture_field_spec_get_name(spec), "product-id"))
			continue;

		g_assert_cmpint(venture_field_spec_get_kind(spec), ==,
		                VENTURE_FIELD_KIND_REFERENCE);
		g_assert_cmpstr(venture_field_spec_get_reference_type(spec),
		                ==, "product");
		return;
	}

	g_assert_not_reached();
}

/*
 * Field order.
 *
 * GObject hands properties back in an order of its own, so without the
 * declared position every generated surface drifts towards alphabetical --
 * and a list's first columns become whichever fields sort early rather than
 * the ones that identify a record. The symptom is a table of empty cells,
 * which looks like a data problem rather than an ordering one.
 */
static void
test_entity_field_specs_follow_the_declared_order(void)
{
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(GPtrArray) specs = NULL;

	venture = venture_venture_new();
	specs = venture_entity_get_field_specs(VENTURE_ENTITY(venture));
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	g_assert_cmpuint(specs->len, >, 3);

	/* "name" is declared first in the venture field table, and it is what
	 * identifies the record to a person. */
	g_assert_cmpstr(venture_field_spec_get_name(g_ptr_array_index(specs, 0)),
	                ==, "name");
	g_assert_cmpstr(venture_field_spec_get_name(g_ptr_array_index(specs, 1)),
	                ==, "slug");
}

static void
test_entity_field_specs_omit_the_identity_spine(void)
{
	g_autoptr(VentureSale) sale = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	static const gchar *const machinery[] = {
		"id", "uuid", "created-at", "updated-at", "version",
		"organization-id", "deleted-at", "attributes", NULL
	};
	guint i;

	sale = venture_sale_new();
	specs = venture_entity_get_field_specs(VENTURE_ENTITY(sale));

	/*
	 * None of the spine is data the operator types: the entity a record
	 * belongs to is chosen with the picker and rendered as its own
	 * control, and the rest is bookkeeping. A form that offered
	 * "deleted_at" as a field would be offering a way to corrupt it.
	 */
	for (i = 0; i < specs->len; i++)
	{
		const gchar *name;

		name = venture_field_spec_get_name(g_ptr_array_index(specs, i));
		g_assert_false(g_strv_contains(machinery, name));
	}
}


static void
test_entity_field_specs_keep_the_declared_kind(void)
{
	g_autoptr(VentureSale) sale = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	gboolean checked;
	guint i;

	sale = venture_sale_new();
	specs = venture_entity_get_field_specs(VENTURE_ENTITY(sale));
	checked = FALSE;

	/*
	 * A long text field and a short string field are both G_TYPE_STRING,
	 * so a spec that recomputes its kind from the property loses the
	 * difference -- and every Notes field becomes a one-line box. The
	 * declaration is the only place that distinction exists.
	 */
	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		if (0 != g_strcmp0(venture_field_spec_get_name(spec), "notes"))
			continue;

		g_assert_cmpint(venture_field_spec_get_kind(spec), ==,
		                VENTURE_FIELD_KIND_TEXT);
		checked = TRUE;
	}

	g_assert_true(checked);

	/* And a declared name field stays a plain string, so it does not
	 * become a text area for a one-line label. */
	{
		g_autoptr(VentureExpense) expense = NULL;
		g_autoptr(GPtrArray) expense_specs = NULL;

		expense = venture_expense_new();
		expense_specs = venture_entity_get_field_specs(
			VENTURE_ENTITY(expense));
		checked = FALSE;

		for (i = 0; i < expense_specs->len; i++)
		{
			VentureFieldSpec *spec;

			spec = g_ptr_array_index(expense_specs, i);

			if (0 != g_strcmp0(venture_field_spec_get_name(spec),
			                   "description"))
				continue;

			g_assert_cmpint(venture_field_spec_get_kind(spec), ==,
			                VENTURE_FIELD_KIND_STRING);
			checked = TRUE;
		}

		g_assert_true(checked);
	}
}


int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/registry/has-builtins", test_registry_has_builtins);
	g_test_add_func("/registry/lookup-singular-and-plural",
	                test_registry_lookup_singular_and_plural);
	g_test_add_func("/registry/create-unknown-lists-alternatives",
	                test_registry_create_unknown_lists_alternatives);
	g_test_add_func("/registry/rejects-abstract", test_registry_rejects_abstract);
	g_test_add_func("/registry/rejects-duplicate-name",
	                test_registry_rejects_duplicate_name);
	g_test_add_func("/registry/describe", test_registry_describe);
	g_test_add_func("/registry/table-names-pluralise-correctly",
	                test_registry_table_names_pluralise_correctly);

	g_test_add_func("/entity/identity", test_entity_identity);
	g_test_add_func("/entity/touch-increments-version",
	                test_entity_touch_increments_version);
	g_test_add_func("/entity/display-name-falls-back",
	                test_entity_display_name_falls_back);

	g_test_add_func("/entity/fields-store-values", test_entity_fields_store_values);
	g_test_add_func("/entity/unset-field-reads-as-default",
	                test_entity_unset_field_reads_as_default);
	g_test_add_func("/entity/set-field-from-string", test_entity_set_field_from_string);
	g_test_add_func("/entity/set-unknown-field-fails",
	                test_entity_set_unknown_field_fails);
	g_test_add_func("/entity/enum-field-rejects-bad-value",
	                test_entity_enum_field_rejects_bad_value);
	g_test_add_func("/entity/enum-field-accepts-spelling-variants",
	                test_entity_enum_field_accepts_spelling_variants);

	g_test_add_func("/entity/attributes", test_entity_attributes);

	g_test_add_func("/entity/json-round-trip", test_entity_json_round_trip);
	g_test_add_func("/entity/json-uses-underscored-members",
	                test_entity_json_uses_underscored_members);
	g_test_add_func("/entity/partial-json-leaves-other-fields",
	                test_entity_partial_json_leaves_other_fields);
	g_test_add_func("/entity/sensitive-fields-withheld",
	                test_entity_sensitive_fields_withheld);
	g_test_add_func("/entity/yaml-export", test_entity_yaml_export);

	g_test_add_func("/entity/diff-reports-changes", test_entity_diff_reports_changes);
	g_test_add_func("/entity/diff-redacts-sensitive", test_entity_diff_redacts_sensitive);
	g_test_add_func("/entity/duplicate-is-unsaved", test_entity_duplicate_is_unsaved);

	g_test_add_func("/entity/validation-requires-not-null-strings",
	                test_entity_validation_requires_not_null_strings);
	g_test_add_func("/entity/validation-rejects-whitespace-only",
	                test_entity_validation_rejects_whitespace_only);

	g_test_add_func("/records/sale-net", test_sale_net);
	g_test_add_func("/records/sale-net-with-no-amounts-is-zero",
	                test_sale_net_with_no_amounts_is_zero);
	g_test_add_func("/records/expense-deductible-amount",
	                test_expense_deductible_amount);
	g_test_add_func("/records/deal-weighted-value", test_deal_weighted_value);
	g_test_add_func("/records/campaign-roi", test_campaign_roi);
	g_test_add_func("/records/idea-score", test_idea_score);

	g_test_add_func("/records/user-password", test_user_password);
	g_test_add_func("/records/user-inactive-cannot-authenticate",
	                test_user_inactive_cannot_authenticate);
	g_test_add_func("/records/user-empty-password-rejected",
	                test_user_empty_password_rejected);
	g_test_add_func("/records/api-token", test_api_token);
	g_test_add_func("/records/api-token-expiry", test_api_token_expiry);
	g_test_add_func("/records/api-token-inactive", test_api_token_inactive);

	g_test_add_func("/records/audit-entry-for-change", test_audit_entry_for_change);

	g_test_add_func("/entity/field-specs-derived-from-properties",
	                test_entity_field_specs_derived_from_properties);
	g_test_add_func("/entity/field-spec-enum-choices-from-gtype",
	                test_entity_field_spec_enum_choices_from_gtype);
	g_test_add_func("/entity/field-spec-reference-target",
	                test_entity_field_spec_reference_target);

	g_test_add_func("/entity/field-specs-follow-the-declared-order",
	                test_entity_field_specs_follow_the_declared_order);
	g_test_add_func("/entity/field-specs-keep-the-declared-kind",
	                test_entity_field_specs_keep_the_declared_kind);
	g_test_add_func("/entity/field-specs-omit-the-identity-spine",
	                test_entity_field_specs_omit_the_identity_spine);

	return g_test_run();
}
