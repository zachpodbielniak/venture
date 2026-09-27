/*
 * venture-production.h - Bills of materials, crafting and recipe margins
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The production module turns stock into other stock. A `recipe` names
 * what one batch makes; its `recipe_component` rows name what it takes.
 * Crafting -- the `craft` action on a recipe, or
 * venture_production_craft() -- consumes the components and produces the
 * output in one transaction through the inventory service, so the made
 * units carry the consumed components' FIFO cost and the cost of goods
 * sold when they are sold is right. The rules that keep the records
 * honest are save validators installed here, so the form, the API, an
 * approved staged change and the assistant all obey the same ones.
 */

#ifndef VENTURE_PRODUCTION_H
#define VENTURE_PRODUCTION_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_PRODUCTION_MAX_TIMES:
 *
 * The most batches one craft makes. A bound, not a business rule: it
 * keeps quantity x times far from overflow and a typo from issuing a
 * warehouse.
 */
#define VENTURE_PRODUCTION_MAX_TIMES (1000000)

/**
 * venture_production_install:
 * @context: the wiring
 *
 * Installs the save validators for `recipe` (a product it makes, a batch
 * of at least one, one organization, not a component of itself) and
 * `recipe_component` (a recipe and a product, at least one unit, one
 * organization, not the recipe's own output, one line per product), and
 * registers the `craft` action on `recipe`. Called once by the context; a
 * second context over the same database installs nothing twice.
 */
void
venture_production_install(VentureContext *context);

/**
 * venture_production_find_stock:
 * @database: the database to read
 * @organization_id: the organization whose stock counts
 * @product_id: the product
 * @location_id: the location to look in, or 0 for anywhere
 * @out_item_id: (out): the inventory item
 * @error: (out) (optional): return location for a #GError
 *
 * The one inventory item holding @product_id: the one at @location_id
 * when a location is given, else the only one there is. None, or more
 * than one, is refused with a message saying what to create or which
 * location to name -- a craft that guessed which bag to take from would
 * be wrong half the time.
 *
 * Returns: %TRUE with @out_item_id set, or %FALSE with @error set
 */
gboolean
venture_production_find_stock(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  product_id,
	gint64		  location_id,
	gint64		 *out_item_id,
	GError		**error
);

/**
 * venture_production_craft:
 * @database: the database to write
 * @recipe_id: the recipe to make
 * @times: how many batches, 1 to %VENTURE_PRODUCTION_MAX_TIMES
 * @location_id: where the stock is taken from and the output goes, or 0
 *   when each product has exactly one inventory item
 * @occurred_at: (nullable): when it was made; %NULL is now
 * @actor: (nullable): audit actor
 * @out_txn: (out) (optional) (transfer full): the output's inventory
 *   transaction
 * @error: (out) (optional): return location for a #GError
 *
 * Makes @times batches of an active recipe, in one transaction: every
 * consumed component leaves its inventory item as a PRODUCTION
 * transaction of quantity x @times at FIFO cost, every reusable component
 * is checked to be on hand (its own quantity, once -- a hammer makes many
 * nails) and left alone, and output-quantity x @times units arrive in the
 * output's item carrying the consumed cost. Every transaction's reference
 * is "recipe:<id>".
 *
 * A shortage is refused, naming the product, unless its inventory item
 * allows negative stock; a reusable component must really be there
 * whatever its item allows. A missing output item is refused, naming
 * what to create. Any refusal or failure writes nothing.
 *
 * Returns: %TRUE on success, %FALSE with @error set
 */
gboolean
venture_production_craft(
	VentureDatabase		 *database,
	gint64			  recipe_id,
	gint64			  times,
	gint64			  location_id,
	GDateTime		 *occurred_at,
	const VentureActor	 *actor,
	VentureEntity		**out_txn,
	GError			**error
);

/**
 * venture_production_recipe_margin:
 * @context: the wiring
 * @period: (nullable): unused beyond the result's heading; recipes are
 *   not dated
 * @options: (nullable): `venture_id`, `category_id` (and everything
 *   filed beneath it), `price_source`, `as_of`, `organization_id`; see
 *   docs/production.org
 * @error: (out) (optional): return location for a #GError
 *
 * What each active recipe costs to make and what it makes is worth: the
 * consumed components priced at the latest observation from
 * `price_source` (market module on) or at their recorded unit costs
 * (market off), the output valued the same way, profit and margin per
 * batch, cost per unit made, and how many batches the stock on hand
 * allows now. A missing price is named, not read as zero; a recipe priced
 * in two currencies gets a note and no margin.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL with @error set
 */
VentureReportResult *
venture_production_recipe_margin(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_production_register_reports:
 * @registry: the report registry
 *
 * Registers `recipe_margin`. It belongs to the production module, so
 * switching that off hides it.
 */
void
venture_production_register_reports(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_PRODUCTION_H */
