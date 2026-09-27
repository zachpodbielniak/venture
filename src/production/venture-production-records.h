/*
 * venture-production-records.h - Recipes and their components
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The production module's two record types. A recipe is a bill of
 * materials -- what one batch makes -- and a component is one line of it.
 * Both are field tables and nothing else: the rules that span rows live
 * in venture-production.c as save validators, so every writer obeys them.
 */

#ifndef VENTURE_PRODUCTION_RECORDS_H
#define VENTURE_PRODUCTION_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_RECIPE (venture_recipe_get_type())
VENTURE_DECLARE_ENTITY(VentureRecipe, venture_recipe, RECIPE)

#define VENTURE_TYPE_RECIPE_COMPONENT (venture_recipe_component_get_type())
VENTURE_DECLARE_ENTITY(VentureRecipeComponent, venture_recipe_component, RECIPE_COMPONENT)

G_END_DECLS

#endif /* VENTURE_PRODUCTION_RECORDS_H */
