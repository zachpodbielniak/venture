/*
 * venture-production-records.c - Recipes and their components
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/* ==========================================================================
 * Recipes
 *
 * A bill of materials: one batch takes these components and makes
 * output-quantity units of one product. The same record is a workshop's
 * build sheet, a kitchen's recipe card and a crafting profession's recipe
 * in a game; nothing here knows which. Crafting one (the `craft` action in
 * venture-production.c) is what moves stock -- the record itself moves
 * nothing, so editing a recipe never rewrites what was made from it.
 * ========================================================================== */

static const VentureFieldDecl venture_recipe_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What you call this recipe"),
	VENTURE_FIELD_REF("venture-id", "Venture", "Optional: the venture that makes it",
	                  "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("output-product-id", "Makes", "The product one batch makes",
	                  "product", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("output-quantity", "Batch size",
	              "Units of the product one batch makes; at least one",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("category-id", "Category", "Optional: where it is filed",
	                  "category", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active",
	              "Can be crafted; an inactive recipe keeps its history",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureRecipe, venture_recipe, venture_recipe_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Recipe", NULL);)

/* ==========================================================================
 * Recipe components
 *
 * One line of a recipe: a product and how many of it one batch needs.
 * `reusable` is named for the exception rather than the rule on purpose.
 * A boolean has no default a form or an API call can supply, so the zero
 * value has to be the safe one: a component whose box nobody ticked is
 * consumed, and stock goes down. The other spelling -- `consumed`, false
 * meaning "a tool" -- would turn every forgotten tick into free materials.
 * A reusable component (a hammer, a mould, a catalyst) must be on hand to
 * craft but is not used up.
 * ========================================================================== */

static const VentureFieldDecl venture_recipe_component_fields[] = {
	VENTURE_FIELD_REF("recipe-id", "Recipe", "The recipe this line belongs to",
	                  "recipe", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("product-id", "Product", "What the recipe takes",
	                  "product", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("quantity", "Quantity", "Units one batch needs; at least one",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reusable", "Reusable",
	              "A tool or catalyst: must be on hand, is not used up",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

/* "x3" or "x1, reusable" -- the product's name would need the database,
 * which a display name does not have, and the recipe page's related list
 * shows the product beside it anyway. */
static gchar *
venture_recipe_component_display_name(VentureEntity *self)
{
	gint64 quantity;
	gboolean reusable;

	g_object_get(self, "quantity", &quantity, "reusable", &reusable, NULL);

	return g_strdup_printf("x%" G_GINT64_FORMAT "%s", quantity,
	                       reusable ? ", reusable" : "");
}

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureRecipeComponent, venture_recipe_component,
	venture_recipe_component_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = venture_recipe_component_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Recipe component", NULL);)
