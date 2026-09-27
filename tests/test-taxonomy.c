/*
 * test-taxonomy.c - Category trees, location trees, tags and the custom
 *                   field kinds that let any record join them
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A tree that can hold a loop is a tree every path, subtree and report
 * walks forever -- or, bounded, silently gets wrong -- and the schema has
 * no foreign keys to stop one. So these tests hold the save validators to
 * the rules, on the ordinary save path every writer shares: no loop, no
 * parent in another organization, a category used only by the records its
 * tree groups. They also hold the computed path to its promise (never
 * stored, so a rename shows everywhere at once) and the two new custom
 * field kinds to the same rules the built-in ones follow.
 */

#include <venture.h>

#include <string.h>

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 organization_id;
	gint64		 venture_id;
} Fixture;

/* Saves, expecting success. */
static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

/* Saves, expecting a validation refusal whose message holds @fragment. */
static void
assert_refused(
	Fixture		*fixture,
	gpointer	 record,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;

	g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

/* Re-reads a record, as a second writer would see it. */
static VentureEntity *
reread(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;
	VentureEntity *fresh;

	fresh = venture_database_get(fixture->database, G_OBJECT_TYPE(record),
	                             venture_entity_get_id(VENTURE_ENTITY(record)), &error);
	g_assert_no_error(error);
	g_assert_nonnull(fresh);

	return fresh;
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->organization_id =
		venture_context_get_default_organization_id(fixture->context);

	/* Products need a real venture: references are checked at the save. */
	venture = venture_venture_new();
	g_object_set(venture, "name", "Shop", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), fixture->organization_id);
	save(fixture, venture);
	fixture->venture_id = venture_entity_get_id(VENTURE_ENTITY(venture));
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
}

/* A saved category under @parent_id (0 for a top level). */
static VentureEntity *
category(
	Fixture		*fixture,
	const gchar	*name,
	gint64		 parent_id,
	const gchar	*applies_to
){
	VentureEntity *node;

	node = VENTURE_ENTITY(venture_category_new());
	g_object_set(node, "name", name, "parent-id", parent_id,
	             "applies-to", applies_to, NULL);
	venture_entity_set_organization_id(node, fixture->organization_id);
	save(fixture, node);

	return node;
}

/* A saved location under @parent_id (0 for a top level). */
static VentureEntity *
location(
	Fixture		*fixture,
	const gchar	*name,
	gint64		 parent_id
){
	VentureEntity *node;

	node = VENTURE_ENTITY(venture_location_new());
	g_object_set(node, "name", name, "parent-id", parent_id, "active", TRUE, NULL);
	venture_entity_set_organization_id(node, fixture->organization_id);
	save(fixture, node);

	return node;
}

/* An unsaved product under the fixture's venture. */
static VentureEntity *
product_new(
	Fixture		*fixture,
	const gchar	*name
){
	VentureEntity *product;

	product = VENTURE_ENTITY(venture_product_new());
	g_object_set(product, "name", name, "venture-id", fixture->venture_id, NULL);
	venture_entity_set_organization_id(product, fixture->organization_id);

	return product;
}

/* A second, real organization, for the cross-organization refusals. */
static gint64
other_organization(Fixture *fixture)
{
	g_autoptr(VentureOrganization) organization = NULL;

	organization = venture_organization_new();
	g_object_set(organization, "name", "Elsewhere", "slug", "elsewhere", NULL);
	save(fixture, organization);

	return venture_entity_get_id(VENTURE_ENTITY(organization));
}

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

/*
 * A node cannot be its own parent, nor the child of anything beneath it.
 * What breaks if this regresses: the path, the subtree and every grouping
 * report walk a loop -- forever, or, bounded, to a wrong answer.
 */
static void
test_category_cycles(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) b = NULL;
	g_autoptr(VentureEntity) c = NULL;

	(void)user_data;

	a = category(fixture, "A", 0, NULL);
	b = category(fixture, "B", ID(a), NULL);
	c = category(fixture, "C", ID(b), NULL);

	g_object_set(a, "parent-id", ID(a), NULL);
	assert_refused(fixture, a, "its own parent");

	g_object_set(a, "parent-id", ID(c), NULL);
	assert_refused(fixture, a, "close a loop");

	g_object_set(a, "parent-id", ID(b), NULL);
	assert_refused(fixture, a, "close a loop");

	/* Moving a leaf elsewhere in the tree is ordinary. */
	g_object_set(c, "parent-id", ID(a), NULL);
	save(fixture, c);
}

/*
 * applies-to names a registered type (any module, on or off), is stored
 * as the registry spells it, and a whole tree agrees on it. What breaks:
 * a product filed under an expense head, which neither report shows.
 */
static void
test_category_applies_to(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) root = NULL;
	g_autoptr(VentureEntity) child = NULL;
	g_autofree gchar *spelled = NULL;

	(void)user_data;

	{
		g_autoptr(VentureEntity) nonsense = VENTURE_ENTITY(venture_category_new());

		g_object_set(nonsense, "name", "Nope", "applies-to", "no_such_type", NULL);
		venture_entity_set_organization_id(nonsense, fixture->organization_id);
		assert_refused(fixture, nonsense, "is not a record type");
	}

	root = category(fixture, "Goods", 0, " Product ");
	g_object_get(root, "applies-to", &spelled, NULL);
	g_assert_cmpstr(spelled, ==, "product");

	{
		g_autoptr(VentureEntity) wrong = VENTURE_ENTITY(venture_category_new());

		g_object_set(wrong, "name", "Sales head", "parent-id", ID(root),
		             "applies-to", "sale", NULL);
		venture_entity_set_organization_id(wrong, fixture->organization_id);
		assert_refused(fixture, wrong, "must group the same records");

		/* Blank is "any type", which is not the same as "product". */
		g_object_set(wrong, "applies-to", "", NULL);
		assert_refused(fixture, wrong, "must group the same records");
	}

	child = category(fixture, "Herbs", ID(root), "product");

	/* The tree cannot change type from under its children. */
	g_object_set(root, "applies-to", "sale", NULL);
	assert_refused(fixture, root, "has sub-categories");

	/* A leaf may, once it is on its own. */
	g_object_set(child, "parent-id", (gint64)0, "applies-to", "sale", NULL);
	save(fixture, child);
}

/*
 * A parent in another organization is refused, for categories and
 * locations alike: a tree spanning two organizations would show one's
 * names on the other's pages.
 */
static void
test_cross_organization_parent(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) theirs = NULL;
	g_autoptr(VentureEntity) their_place = NULL;
	gint64 elsewhere;

	(void)user_data;

	elsewhere = other_organization(fixture);

	theirs = VENTURE_ENTITY(venture_category_new());
	g_object_set(theirs, "name", "Theirs", NULL);
	venture_entity_set_organization_id(theirs, elsewhere);
	save(fixture, theirs);

	{
		g_autoptr(VentureEntity) mine = VENTURE_ENTITY(venture_category_new());

		g_object_set(mine, "name", "Mine", "parent-id", ID(theirs), NULL);
		venture_entity_set_organization_id(mine, fixture->organization_id);
		assert_refused(fixture, mine, "another organization");
	}

	their_place = VENTURE_ENTITY(venture_location_new());
	g_object_set(their_place, "name", "Their warehouse", NULL);
	venture_entity_set_organization_id(their_place, elsewhere);
	save(fixture, their_place);

	{
		g_autoptr(VentureEntity) bin = VENTURE_ENTITY(venture_location_new());

		g_object_set(bin, "name", "Bin", "parent-id", ID(their_place), NULL);
		venture_entity_set_organization_id(bin, fixture->organization_id);
		assert_refused(fixture, bin, "another organization");
	}
}

/*
 * The path is computed from the current names, so renaming a parent shows
 * in every child's path with nothing rewritten; the ancestor at a depth
 * is what grouping uses; the subtree is what "in Materials at any depth"
 * filters on, and a deleted node is not in it.
 */
static void
test_path_ancestor_descendants(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) materials = NULL;
	g_autoptr(VentureEntity) herbs = NULL;
	g_autoptr(VentureEntity) rare = NULL;
	g_autoptr(VentureEntity) ore = NULL;
	g_autoptr(VentureEntity) gone = NULL;
	g_autoptr(VentureEntity) fresh = NULL;
	g_autoptr(GArray) below = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *renamed = NULL;
	gboolean seen[4] = { FALSE, FALSE, FALSE, FALSE };
	guint i;

	(void)user_data;

	materials = category(fixture, "Materials", 0, "product");
	herbs = category(fixture, "Herbs", ID(materials), "product");
	rare = category(fixture, "Rare", ID(herbs), "product");
	ore = category(fixture, "Ore", ID(materials), "product");
	gone = category(fixture, "Gone", ID(materials), "product");
	g_assert_true(venture_database_delete(fixture->database, gone, NULL, &error));
	g_assert_no_error(error);

	path = venture_category_path(fixture->database, VENTURE_TYPE_CATEGORY, ID(rare), &error);
	g_assert_no_error(error);
	g_assert_cmpstr(path, ==, "Materials / Herbs / Rare");

	g_assert_cmpint(venture_category_ancestor_at_depth(fixture->database,
		VENTURE_TYPE_CATEGORY, ID(rare), 0, &error), ==, ID(materials));
	g_assert_cmpint(venture_category_ancestor_at_depth(fixture->database,
		VENTURE_TYPE_CATEGORY, ID(rare), 1, &error), ==, ID(herbs));
	/* Shallower than asked is its own group, not nowhere. */
	g_assert_cmpint(venture_category_ancestor_at_depth(fixture->database,
		VENTURE_TYPE_CATEGORY, ID(ore), 3, &error), ==, ID(ore));
	g_assert_no_error(error);

	below = venture_category_descendants(fixture->database, VENTURE_TYPE_CATEGORY,
	                                     ID(materials), TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(below->len, ==, 4);
	g_assert_cmpint(g_array_index(below, gint64, 0), ==, ID(materials));

	for (i = 0; i < below->len; i++)
	{
		gint64 id = g_array_index(below, gint64, i);

		g_assert_cmpint(id, !=, ID(gone));
		seen[0] |= (id == ID(materials));
		seen[1] |= (id == ID(herbs));
		seen[2] |= (id == ID(rare));
		seen[3] |= (id == ID(ore));
	}

	g_assert_true(seen[0] && seen[1] && seen[2] && seen[3]);

	/* Rename the top; the leaf's path follows with no write to the leaf. */
	fresh = reread(fixture, materials);
	g_object_set(fresh, "name", "Reagents", NULL);
	save(fixture, fresh);

	renamed = venture_category_path(fixture->database, VENTURE_TYPE_CATEGORY, ID(rare), &error);
	g_assert_no_error(error);
	g_assert_cmpstr(renamed, ==, "Reagents / Herbs / Rare");

	/* A node that cannot be read is an error, not an empty path. */
	g_assert_null(venture_category_path(fixture->database, VENTURE_TYPE_CATEGORY, 99999, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/*
 * Locations share the tree rules: a loop is refused and the path reads
 * the same way. A character's bag inside the character is the shape the
 * game-economy example relies on.
 */
static void
test_location_tree(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) character = NULL;
	g_autoptr(VentureEntity) bag = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	(void)user_data;

	character = location(fixture, "Alt 1", 0);
	bag = location(fixture, "Bag", ID(character));

	g_object_set(character, "parent-id", ID(bag), NULL);
	assert_refused(fixture, character, "close a loop");

	path = venture_category_path(fixture->database, VENTURE_TYPE_LOCATION, ID(bag), &error);
	g_assert_no_error(error);
	g_assert_cmpstr(path, ==, "Alt 1 / Bag");

	/* An inventory item names its location by reference, checked like
	 * any other: a place that does not exist is refused. */
	{
		g_autoptr(VentureEntity) product = product_new(fixture, "Herb");
		g_autoptr(VentureEntity) item = VENTURE_ENTITY(venture_inventory_item_new());

		save(fixture, product);
		g_object_set(item, "product-id", ID(product), "location-id", (gint64)99999, NULL);
		venture_entity_set_organization_id(item, fixture->organization_id);
		assert_refused(fixture, item, "does not exist");
		g_object_set(item, "location-id", ID(bag), NULL);
		save(fixture, item);
	}
}

/*
 * product.category-id is a reference like any other -- a missing target
 * is refused -- and is also held to the tree's applies-to. A product that
 * already pointed at a category keeps it after the category is deleted,
 * so it can still be edited; only writing a new dangling pointer is
 * refused.
 */
static void
test_product_category_reference(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) goods = NULL;
	g_autoptr(VentureEntity) heads = NULL;
	g_autoptr(VentureEntity) anything = NULL;
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureEntity) fresh = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	goods = category(fixture, "Goods", 0, "product");
	heads = category(fixture, "Heads", 0, "expense");
	anything = category(fixture, "Anything", 0, NULL);

	product = product_new(fixture, "Widget");
	g_object_set(product, "category-id", (gint64)99999, NULL);
	assert_refused(fixture, product, "does not exist");

	g_object_set(product, "category-id", ID(heads), NULL);
	assert_refused(fixture, product, "groups expense records, not product");

	g_object_set(product, "category-id", ID(anything), NULL);
	save(fixture, product);

	g_object_set(product, "category-id", ID(goods), NULL);
	save(fixture, product);

	/* Delete the category; the product is still editable while it keeps
	 * the pointer, and cannot be moved onto another deleted one. */
	g_assert_true(venture_database_delete(fixture->database, goods, NULL, &error));
	g_assert_no_error(error);

	fresh = reread(fixture, product);
	g_object_set(fresh, "notes", "Still sold", NULL);
	save(fixture, fresh);

	g_clear_object(&fresh);
	fresh = reread(fixture, product);
	g_object_set(fresh, "category-id", ID(anything), NULL);
	save(fixture, fresh);
	g_object_set(fresh, "category-id", ID(goods), NULL);
	assert_refused(fixture, fresh, "has been deleted");
}

/*
 * Tags are searchable, like a ticket's: a free-text search finds the
 * product by one of them.
 */
static void
test_tags_searchable(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	product = product_new(fixture, "Peacebloom");
	g_object_set(product, "tags", "herb,farmable,dragonflight", NULL);
	save(fixture, product);

	query = venture_query_new(VENTURE_TYPE_PRODUCT);
	venture_query_set_search(query, "farmable");
	found = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(found->len, ==, 1);
	g_assert_cmpint(ID(g_ptr_array_index(found, 0)), ==, ID(product));
}

/* Defines a custom field on product, expecting success. */
static void
define(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*kind,
	const gchar	*options
){
	g_autoptr(VentureEntity) field = NULL;
	g_autoptr(GError) error = NULL;

	field = venture_custom_fields_service_define(
		venture_custom_fields_service_get(fixture->database),
		fixture->organization_id, "product", name, kind, FALSE, options, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(field);
}

/* Defines a custom field on product, expecting @fragment in the refusal. */
static void
define_refused(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*kind,
	const gchar	*options,
	const gchar	*fragment
){
	g_autoptr(VentureEntity) field = NULL;
	g_autoptr(GError) error = NULL;

	field = venture_custom_fields_service_define(
		venture_custom_fields_service_get(fixture->database),
		fixture->organization_id, "product", name, kind, FALSE, options, NULL, &error);
	g_assert_null(field);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

/*
 * A double custom field takes a finite decimal and nothing else. What
 * breaks: "abc" or an overflow stored as a number every aggregate then
 * reads as zero or infinity.
 */
static void
test_custom_double(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	define(fixture, "weight_kg", "double", NULL);

	product = product_new(fixture, "Anvil");
	venture_entity_set_attribute(product, "weight_kg", "heavy");
	assert_refused(fixture, product, "must be a number");
	venture_entity_set_attribute(product, "weight_kg", "1e400");
	assert_refused(fixture, product, "must be a number");
	venture_entity_set_attribute(product, "weight_kg", "12.5");
	save(fixture, product);

	/* The form draws it as a decimal input, from the generic renderer. */
	specs = venture_custom_fields_form_specs(fixture->database, fixture->organization_id,
	                                         "product", NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(specs->len, ==, 1);
	g_assert_cmpint(venture_field_spec_get_kind(g_ptr_array_index(specs, 0)), ==,
	                VENTURE_FIELD_KIND_DOUBLE);
}

/*
 * A reference custom field names its target in the options, and its
 * values follow the built-in rule: a missing or deleted target is refused
 * when written, a value the record already held is left alone, and a
 * category target is held to its tree's applies-to. What breaks: a plugin
 * type "categorised" by a custom field points at nothing, and nobody
 * finds out until a report quietly drops it.
 */
static void
test_custom_reference(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) goods = NULL;
	g_autoptr(VentureEntity) spare = NULL;
	g_autoptr(VentureEntity) heads = NULL;
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureEntity) fresh = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id_text = NULL;
	g_autofree gchar *spare_text = NULL;

	(void)user_data;

	define_refused(fixture, "shelf", "reference", NULL, "options like");
	define_refused(fixture, "shelf", "reference", "{\"target\":\"no_such_type\"}",
	               "registered record type");
	define(fixture, "shelf", "reference", "{\"target\":\"category\"}");

	goods = category(fixture, "Goods", 0, "product");
	spare = category(fixture, "Spare", 0, NULL);
	heads = category(fixture, "Heads", 0, "expense");
	id_text = g_strdup_printf("%" G_GINT64_FORMAT, ID(goods));
	spare_text = g_strdup_printf("%" G_GINT64_FORMAT, ID(spare));

	product = product_new(fixture, "Crate");
	venture_entity_set_attribute(product, "shelf", "top");
	assert_refused(fixture, product, "must be a record id");
	venture_entity_set_attribute(product, "shelf", "99999");
	assert_refused(fixture, product, "does not exist");
	{
		g_autofree gchar *heads_text = g_strdup_printf("%" G_GINT64_FORMAT, ID(heads));

		venture_entity_set_attribute(product, "shelf", heads_text);
		assert_refused(fixture, product, "groups expense records");
	}
	venture_entity_set_attribute(product, "shelf", id_text);
	save(fixture, product);

	/* The target goes; the product keeps the value and stays editable. */
	g_assert_true(venture_database_delete(fixture->database, goods, NULL, &error));
	g_assert_no_error(error);
	fresh = reread(fixture, product);
	g_assert_cmpstr(venture_entity_get_attribute(fresh, "shelf"), ==, id_text);
	g_object_set(fresh, "notes", "Kept its shelf", NULL);
	save(fixture, fresh);

	/* Writing the deleted target anew, from another value, is refused. */
	g_clear_object(&fresh);
	fresh = reread(fixture, product);
	venture_entity_set_attribute(fresh, "shelf", spare_text);
	save(fixture, fresh);
	g_clear_object(&fresh);
	fresh = reread(fixture, product);
	venture_entity_set_attribute(fresh, "shelf", id_text);
	assert_refused(fixture, fresh, "has been deleted");

	/* The form gets a reference spec naming the target, which is what
	 * lets the generic picker draw it. */
	specs = venture_custom_fields_form_specs(fixture->database, fixture->organization_id,
	                                         "product", NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(specs->len, ==, 1);
	g_assert_cmpint(venture_field_spec_get_kind(g_ptr_array_index(specs, 0)), ==,
	                VENTURE_FIELD_KIND_REFERENCE);
	g_assert_cmpstr(venture_field_spec_get_reference_type(g_ptr_array_index(specs, 0)), ==,
	                "category");
}

int
main(
	int	 argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add("/taxonomy/category-cycles", Fixture, NULL,
	           fixture_set_up, test_category_cycles, fixture_tear_down);
	g_test_add("/taxonomy/category-applies-to", Fixture, NULL,
	           fixture_set_up, test_category_applies_to, fixture_tear_down);
	g_test_add("/taxonomy/cross-organization-parent", Fixture, NULL,
	           fixture_set_up, test_cross_organization_parent, fixture_tear_down);
	g_test_add("/taxonomy/path-ancestor-descendants", Fixture, NULL,
	           fixture_set_up, test_path_ancestor_descendants, fixture_tear_down);
	g_test_add("/taxonomy/location-tree", Fixture, NULL,
	           fixture_set_up, test_location_tree, fixture_tear_down);
	g_test_add("/taxonomy/product-category-reference", Fixture, NULL,
	           fixture_set_up, test_product_category_reference, fixture_tear_down);
	g_test_add("/taxonomy/tags-searchable", Fixture, NULL,
	           fixture_set_up, test_tags_searchable, fixture_tear_down);
	g_test_add("/taxonomy/custom-double", Fixture, NULL,
	           fixture_set_up, test_custom_double, fixture_tear_down);
	g_test_add("/taxonomy/custom-reference", Fixture, NULL,
	           fixture_set_up, test_custom_reference, fixture_tear_down);

	return g_test_run();
}
