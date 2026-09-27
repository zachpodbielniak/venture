/*
 * test-goals.c - Goals, their steps, progress and the materials still needed
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A goal is worth recording only if three things hold. Its progress must
 * be measured from its start toward its target, whichever way that runs
 * -- a weight to lose is not 106% of the way to 80 kg. Its dates must
 * follow its state and nothing else must move it: a step ticked done does
 * not rewrite the goal. And the shopping list for the steps still ahead
 * must be the books' kind of arithmetic: crafts multiplied out, tools
 * counted once, stock taken off, money per currency, a missing price named
 * and never read as zero. These tests hold all three.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 organization_id;
	gint64		 venture_id;
} Fixture;

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

/* Saves, expecting success; the refusal's reason is the failure. */
static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

/* Saves, expecting a refusal whose message holds @fragment. */
static void
save_refused(
	Fixture		*fixture,
	gpointer	 record,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;
	gboolean saved;

	saved = venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error);
	g_assert_nonnull(error);
	g_assert_false(saved);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
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

	/* A game's gold: four minor digits, never to be added to dollars. */
	g_assert_true(venture_currency_register("GOLD", 4, NULL, FALSE, NULL, &error));
	g_assert_no_error(error);

	venture = venture_venture_new();
	g_object_set(venture, "name", "Alchemy", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), fixture->organization_id);
	save(fixture, venture);
	fixture->venture_id = ID(venture);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	venture_currency_clear_registered();

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

static VentureMoney *
money_of(const gchar *text)
{
	VentureMoney *money;

	money = venture_money_from_string(text, NULL, NULL);
	g_assert_nonnull(money);

	return money;
}

/* --- Records --------------------------------------------------------------- */

static gint64
organization(
	Fixture		*fixture,
	const gchar	*slug
){
	g_autoptr(VentureEntity) other = NULL;

	other = VENTURE_ENTITY(venture_organization_new());
	g_object_set(other, "name", slug, "slug", slug, NULL);
	save(fixture, other);

	return ID(other);
}

static gint64
category(
	Fixture		*fixture,
	const gchar	*name,
	gint64		 parent_id
){
	g_autoptr(VentureEntity) record = NULL;

	record = VENTURE_ENTITY(venture_category_new());
	g_object_set(record, "name", name, "parent-id", parent_id, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

/* An unsaved goal in the default organization. */
static VentureEntity *
goal_new(
	Fixture		*fixture,
	const gchar	*name,
	gdouble		 start,
	gdouble		 current,
	gdouble		 target
){
	VentureEntity *record;

	record = VENTURE_ENTITY(venture_goal_new());
	g_object_set(record, "name", name, "metric", "level", "unit", "skill",
	             "start-value", start, "current-value", current,
	             "target-value", target, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);

	return record;
}

static gint64
goal(
	Fixture		*fixture,
	const gchar	*name,
	gdouble		 start,
	gdouble		 current,
	gdouble		 target,
	gint64		 parent_id
){
	g_autoptr(VentureEntity) record = NULL;

	record = goal_new(fixture, name, start, current, target);
	g_object_set(record, "parent-id", parent_id, NULL);
	save(fixture, record);

	return ID(record);
}

static VentureEntity *
step_new(
	Fixture		*fixture,
	gint64		 goal_id,
	const gchar	*name
){
	VentureEntity *record;

	record = VENTURE_ENTITY(venture_goal_step_new());
	g_object_set(record, "goal-id", goal_id, "name", name, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);

	return record;
}

/* A saved recipe step: @repetitions crafts of @recipe_id. */
static gint64
craft_step(
	Fixture		*fixture,
	gint64		 goal_id,
	const gchar	*name,
	gint64		 recipe_id,
	gint64		 repetitions,
	gboolean	 done
){
	g_autoptr(VentureEntity) record = NULL;

	record = step_new(fixture, goal_id, name);
	g_object_set(record, "recipe-id", recipe_id, "repetitions", repetitions,
	             "done", done, NULL);
	save(fixture, record);

	return ID(record);
}

static VentureEntity *
reread(
	Fixture	*fixture,
	GType	 type,
	gint64	 id
){
	VentureEntity *record;

	record = venture_database_get(fixture->database, type, id, NULL);
	g_assert_nonnull(record);

	return record;
}

static gint64
product_costing(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*cost
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) amount = NULL;

	amount = (NULL != cost) ? money_of(cost) : NULL;
	record = VENTURE_ENTITY(venture_product_new());
	g_object_set(record, "name", name, "cost", amount, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

static gint64
location(
	Fixture		*fixture,
	const gchar	*name
){
	g_autoptr(VentureEntity) record = NULL;

	record = VENTURE_ENTITY(venture_location_new());
	g_object_set(record, "name", name, "active", TRUE, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

/* A stock item for @product_id at @location_id holding @units. */
static gint64
stock(
	Fixture		*fixture,
	gint64		 product_id,
	gint64		 location_id,
	gint64		 units,
	const gchar	*unit_cost
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) when = NULL;

	cost = (NULL != unit_cost) ? money_of(unit_cost) : NULL;
	record = VENTURE_ENTITY(venture_inventory_item_new());
	g_object_set(record, "product-id", product_id, "location-id", location_id,
	             "unit-cost", cost, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);

	if (units > 0)
	{
		g_autoptr(VentureMoney) layer = NULL;

		layer = money_of("0.00 USD");
		when = venture_time_from_string("2026-03-01", NULL);
		g_assert_true(venture_inventory_service_receive(
			venture_inventory_service_get(fixture->database), ID(record), units,
			layer, when, 0, "seed", NULL, &error));
		g_assert_no_error(error);
	}

	return ID(record);
}

static gint64
recipe(
	Fixture		*fixture,
	const gchar	*name,
	gint64		 output_id
){
	g_autoptr(VentureEntity) record = NULL;

	record = VENTURE_ENTITY(venture_recipe_new());
	g_object_set(record, "name", name, "output-product-id", output_id,
	             "output-quantity", (gint64)1, "active", TRUE, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

static void
component(
	Fixture		*fixture,
	gint64		 recipe_id,
	gint64		 product_id,
	gint64		 quantity,
	gboolean	 reusable
){
	g_autoptr(VentureEntity) record = NULL;

	record = VENTURE_ENTITY(venture_recipe_component_new());
	g_object_set(record, "recipe-id", recipe_id, "product-id", product_id,
	             "quantity", quantity, "reusable", reusable, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);
}

static void
observe(
	Fixture		*fixture,
	gint64		 product_id,
	const gchar	*source,
	const gchar	*price
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) at = NULL;

	amount = money_of(price);
	at = venture_time_from_string("2026-03-01T00:00:00Z", NULL);
	record = VENTURE_ENTITY(venture_price_observation_new());
	g_object_set(record, "product-id", product_id, "source", source,
	             "price", amount, "observed-at", at, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);
}

/* --- Reading a report -------------------------------------------------------- */

static VentureReportResult *
run_report(
	Fixture		 *fixture,
	const gchar	 *name,
	JsonObject	 *options,
	GError		**error
){
	VentureReport *report;

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), name);
	g_assert_nonnull(report);

	return venture_report_generate(report, fixture->context, NULL, options, error);
}

static VentureReportResult *
run_ok(
	Fixture		*fixture,
	const gchar	*name,
	JsonObject	*options
){
	g_autoptr(GError) error = NULL;
	VentureReportResult *result;

	result = run_report(fixture, name, options, &error);

	if (NULL == result)
		g_error("%s refused: %s", name, error->message);

	return result;
}

static void
run_refused(
	Fixture		*fixture,
	const gchar	*name,
	JsonObject	*options,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) result = NULL;

	result = run_report(fixture, name, options, &error);
	g_assert_null(result);
	g_assert_nonnull(error);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

static const gchar *
cell_text(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);

	if ((NULL == value) || !G_VALUE_HOLDS_STRING(value))
		return NULL;

	return g_value_get_string(value);
}

static gboolean
has_cell(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	return NULL != venture_report_result_get_cell(result, row, key);
}

/* The row whose @key is @text and, when @currency is not NULL, whose
 * currency is that. */
static guint
row_of(
	VentureReportResult	*result,
	const gchar		*key,
	const gchar		*text,
	const gchar		*currency
){
	guint i;

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		if ((0 == g_strcmp0(cell_text(result, i, key), text)) &&
		    ((NULL == currency) || (0 == g_strcmp0(cell_text(result, i, "currency"), currency))))
			return i;
	}

	g_error("no row with %s = %s", key, text);
	return 0;
}

static gboolean
has_row(
	VentureReportResult	*result,
	const gchar		*key,
	const gchar		*text
){
	guint i;

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		if (0 == g_strcmp0(cell_text(result, i, key), text))
			return TRUE;
	}

	return FALSE;
}

static gdouble
cell_number(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);
	g_assert_nonnull(value);
	g_assert_true(G_VALUE_HOLDS_DOUBLE(value));

	return g_value_get_double(value);
}

static gchar *
cell_money(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);

	if ((NULL == value) || !G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY) ||
	    (NULL == g_value_get_boxed(value)))
		return NULL;

	return venture_money_to_string(g_value_get_boxed(value));
}

#define ASSERT_CELL_MONEY(result, row, key, expected) G_STMT_START { \
	g_autofree gchar *cell_money_text = cell_money(result, row, key); \
	g_assert_cmpstr(cell_money_text, ==, expected); \
} G_STMT_END

#define ASSERT_CONTAINS(text, fragment) G_STMT_START { \
	const gchar *contains_text = (text); \
	if ((NULL == contains_text) || (NULL == strstr(contains_text, (fragment)))) \
		g_error("expected \"%s\" in: %s", (fragment), \
		        (NULL != contains_text) ? contains_text : "(null)"); \
} G_STMT_END

/* ==========================================================================
 * Validators
 * ========================================================================== */

/*
 * A goal needs a distance to cover, a tree without loops in one
 * organization, and an achieved time that follows its status: stamped on
 * achieving, kept when given, cleared on leaving achieved, refused when
 * typed onto a goal that never was. What breaks: a percentage divided by
 * zero, a sub-goal tree every path walks forever, or a report counting a
 * goal as achieved that is still open.
 */
static void
test_goal_validators(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) flat = NULL;
	g_autoptr(VentureEntity) empty = NULL;
	g_autoptr(VentureEntity) top = NULL;
	g_autoptr(VentureEntity) loop = NULL;
	g_autoptr(VentureEntity) self_parent = NULL;
	g_autoptr(VentureEntity) foreign = NULL;
	g_autoptr(VentureEntity) cross = NULL;
	g_autoptr(VentureEntity) cross_venture = NULL;
	g_autoptr(VentureEntity) typed = NULL;
	g_autoptr(VentureEntity) done = NULL;
	g_autoptr(VentureEntity) given = NULL;
	g_autoptr(GDateTime) stamp = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 parent;
	gint64 child;
	gint64 grandchild;
	gint64 other;
	gint64 other_venture;

	(void)user_data;

	/* No distance: refused, including the 0-to-0 goal nobody targeted. */
	flat = goal_new(fixture, "Flat", 10, 10, 10);
	save_refused(fixture, flat, "must differ from the start");
	empty = goal_new(fixture, "Empty", 0, 0, 0);
	save_refused(fixture, empty, "must differ from the start");

	/* A target below the start is a goal like any other. */
	goal(fixture, "Lose weight", 90, 88, 80, 0);

	/* The tree: no loop, no self, no parent from elsewhere. */
	parent = goal(fixture, "Reach 300", 1, 1, 300, 0);
	child = goal(fixture, "Reach 150", 1, 1, 150, parent);
	grandchild = goal(fixture, "Reach 75", 1, 1, 75, child);

	top = reread(fixture, VENTURE_TYPE_GOAL, parent);
	g_object_set(top, "parent-id", grandchild, NULL);
	save_refused(fixture, top, "close a loop");

	self_parent = reread(fixture, VENTURE_TYPE_GOAL, child);
	g_object_set(self_parent, "parent-id", child, NULL);
	save_refused(fixture, self_parent, "its own parent");

	other = organization(fixture, "elsewhere");
	foreign = goal_new(fixture, "Theirs", 0, 0, 5);
	venture_entity_set_organization_id(foreign, other);
	save(fixture, foreign);
	cross = goal_new(fixture, "Under theirs", 0, 0, 5);
	g_object_set(cross, "parent-id", ID(foreign), NULL);
	save_refused(fixture, cross, "another organization");

	{
		g_autoptr(VentureEntity) venture = NULL;

		venture = VENTURE_ENTITY(venture_venture_new());
		g_object_set(venture, "name", "Their venture", NULL);
		venture_entity_set_organization_id(venture, other);
		save(fixture, venture);
		other_venture = ID(venture);
	}

	cross_venture = goal_new(fixture, "For their venture", 0, 0, 5);
	g_object_set(cross_venture, "venture-id", other_venture, NULL);
	save_refused(fixture, cross_venture, "another organization");

	/* Achieved-at: refused on a goal that is not achieved. */
	when = venture_time_from_string("2026-03-10T12:00:00Z", NULL);
	typed = goal_new(fixture, "Typed", 0, 0, 5);
	g_object_set(typed, "achieved-at", when, NULL);
	save_refused(fixture, typed, "is for an achieved goal");

	/* Stamped when achieved; kept through an edit; cleared on reopening. */
	done = reread(fixture, VENTURE_TYPE_GOAL, child);
	g_object_set(done, "status", VENTURE_GOAL_STATUS_ACHIEVED, NULL);
	save(fixture, done);
	g_clear_object(&done);
	done = reread(fixture, VENTURE_TYPE_GOAL, child);
	g_object_get(done, "achieved-at", &stamp, NULL);
	g_assert_nonnull(stamp);
	g_object_set(done, "notes", "levelled", NULL);
	save(fixture, done);
	g_clear_object(&done);
	done = reread(fixture, VENTURE_TYPE_GOAL, child);
	{
		g_autoptr(GDateTime) kept = NULL;

		g_object_get(done, "achieved-at", &kept, NULL);
		g_assert_true(venture_time_equal(kept, stamp));
	}
	g_object_set(done, "status", VENTURE_GOAL_STATUS_ACTIVE, NULL);
	save(fixture, done);
	g_clear_object(&done);
	done = reread(fixture, VENTURE_TYPE_GOAL, child);
	{
		g_autoptr(GDateTime) cleared = NULL;

		g_object_get(done, "achieved-at", &cleared, NULL);
		g_assert_null(cleared);
	}

	/* A date given with the status is kept, not replaced by now. */
	given = goal_new(fixture, "Given", 0, 5, 5);
	g_object_set(given, "status", VENTURE_GOAL_STATUS_ACHIEVED, "achieved-at", when, NULL);
	save(fixture, given);
	{
		g_autoptr(VentureEntity) back = NULL;
		g_autoptr(GDateTime) kept = NULL;

		back = reread(fixture, VENTURE_TYPE_GOAL, ID(given));
		g_object_get(back, "achieved-at", &kept, NULL);
		g_assert_true(venture_time_equal(kept, when));

		/* Abandoned after achieving: it is no longer achieved. */
		g_object_set(back, "status", VENTURE_GOAL_STATUS_ABANDONED, NULL);
		save(fixture, back);
		g_clear_object(&back);
		g_clear_pointer(&kept, g_date_time_unref);
		back = reread(fixture, VENTURE_TYPE_GOAL, ID(given));
		g_object_get(back, "achieved-at", &kept, NULL);
		g_assert_null(kept);
	}
}

/*
 * A step belongs to a goal of its own organization, moves the goal's way,
 * counts crafts that are not negative, names a recipe only while
 * production is on and only its own organization's, and carries a done
 * time that follows done. Ticking it done changes nothing on the goal.
 * What breaks: a shopping list multiplied by -5, a step in another
 * organization's goal, or a goal whose current value moved under its
 * owner.
 */
static void
test_step_validators(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) orphan = NULL;
	g_autoptr(VentureEntity) negative = NULL;
	g_autoptr(VentureEntity) backwards = NULL;
	g_autoptr(VentureEntity) downhill = NULL;
	g_autoptr(VentureEntity) typed = NULL;
	g_autoptr(VentureEntity) ticked = NULL;
	g_autoptr(VentureEntity) cross = NULL;
	g_autoptr(VentureEntity) foreign_goal = NULL;
	g_autoptr(VentureEntity) theirs = NULL;
	g_autoptr(VentureEntity) off = NULL;
	g_autoptr(VentureEntity) goal_after = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 skill;
	gint64 weight;
	gint64 potion;
	gint64 brew;
	gint64 step;
	gint64 other;
	gdouble current;

	(void)user_data;

	skill = goal(fixture, "Alchemy 300", 1, 1, 300, 0);
	weight = goal(fixture, "Lose weight", 90, 90, 80, 0);

	orphan = step_new(fixture, 0, "Nowhere");
	save_refused(fixture, orphan, "is required");

	negative = step_new(fixture, skill, "Negative");
	g_object_set(negative, "repetitions", (gint64)-5, NULL);
	save_refused(fixture, negative, "cannot be negative");

	/* The goal's way: up for a skill, down for a weight. */
	backwards = step_new(fixture, skill, "Backwards");
	g_object_set(backwards, "from-value", 25.0, "to-value", 1.0, NULL);
	save_refused(fixture, backwards, "opposite way");
	downhill = step_new(fixture, weight, "First 5 kg");
	g_object_set(downhill, "from-value", 90.0, "to-value", 85.0, NULL);
	save(fixture, downhill);

	/* done-at: refused when typed on an open step; stamped, then cleared. */
	when = venture_time_from_string("2026-03-10T12:00:00Z", NULL);
	typed = step_new(fixture, skill, "Typed");
	g_object_set(typed, "done-at", when, NULL);
	save_refused(fixture, typed, "is for a done step");

	potion = product_costing(fixture, "Minor Healing Potion", NULL);
	brew = recipe(fixture, "Brew potion", potion);
	step = craft_step(fixture, skill, "1 to 25", brew, 20, FALSE);
	ticked = reread(fixture, VENTURE_TYPE_GOAL_STEP, step);
	g_object_set(ticked, "done", TRUE, NULL);
	save(fixture, ticked);
	g_clear_object(&ticked);
	ticked = reread(fixture, VENTURE_TYPE_GOAL_STEP, step);
	{
		g_autoptr(GDateTime) stamp = NULL;

		g_object_get(ticked, "done-at", &stamp, NULL);
		g_assert_nonnull(stamp);
	}

	/* Nothing moved the goal: a derived write here would be a second
	 * save nobody made. */
	goal_after = reread(fixture, VENTURE_TYPE_GOAL, skill);
	g_object_get(goal_after, "current-value", &current, NULL);
	g_assert_cmpfloat(current, ==, 1.0);
	g_assert_cmpint(venture_entity_get_version(goal_after), ==, 1);

	g_object_set(ticked, "done", FALSE, NULL);
	save(fixture, ticked);
	g_clear_object(&ticked);
	ticked = reread(fixture, VENTURE_TYPE_GOAL_STEP, step);
	{
		g_autoptr(GDateTime) cleared = NULL;

		g_object_get(ticked, "done-at", &cleared, NULL);
		g_assert_null(cleared);
	}

	/* Another organization's goal, and another organization's recipe. */
	other = organization(fixture, "elsewhere");
	foreign_goal = goal_new(fixture, "Theirs", 0, 0, 5);
	venture_entity_set_organization_id(foreign_goal, other);
	save(fixture, foreign_goal);
	cross = step_new(fixture, ID(foreign_goal), "Into theirs");
	save_refused(fixture, cross, "another organization");

	{
		g_autoptr(VentureEntity) their_product = NULL;

		their_product = VENTURE_ENTITY(venture_product_new());
		g_object_set(their_product, "name", "Their potion", NULL);
		venture_entity_set_organization_id(their_product, other);
		save(fixture, their_product);
		theirs = VENTURE_ENTITY(venture_recipe_new());
		g_object_set(theirs, "name", "Their brew", "output-product-id",
		             ID(their_product), "output-quantity", (gint64)1, "active", TRUE,
		             NULL);
		venture_entity_set_organization_id(theirs, other);
		save(fixture, theirs);
	}

	{
		g_autoptr(VentureEntity) poached = NULL;

		poached = step_new(fixture, skill, "Their recipe");
		g_object_set(poached, "recipe-id", ID(theirs), "repetitions", (gint64)1, NULL);
		save_refused(fixture, poached, "another organization");
	}

	/* Production off: a recipe newly named is refused, naming the switch;
	 * a step that already names one stays editable. */
	venture_config_set_module_enabled(fixture->config, "production", FALSE);
	off = step_new(fixture, skill, "While off");
	g_object_set(off, "recipe-id", brew, "repetitions", (gint64)3, NULL);
	save_refused(fixture, off, "production module");
	g_clear_object(&ticked);
	ticked = reread(fixture, VENTURE_TYPE_GOAL_STEP, step);
	g_object_set(ticked, "notes", "still editable", NULL);
	save(fixture, ticked);
}

/* ==========================================================================
 * goal_progress
 * ========================================================================== */

/* as_of = the goal's creation plus @days, as a timestamp. */
static gchar *
days_after_creation(
	Fixture	*fixture,
	gint64	 goal_id,
	gint	 days
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GDateTime) when = NULL;

	record = reread(fixture, VENTURE_TYPE_GOAL, goal_id);
	when = g_date_time_add_days(venture_entity_get_created_at(record), days);

	return venture_time_to_string(when);
}

/*
 * The arithmetic, both ways round. A skill 1 to 300 at 150.5 is half
 * done with 149.5 to go; a weight 90 to 80 at 85 is half done with 5 to
 * go (not 106% of 80); one past its target reads above 100% with a
 * negative remainder and a note to mark it achieved. The forecast extends
 * the pace from creation to as_of: half done after ten days lands twenty
 * days after creation, and after a due date fifteen days out it says so;
 * past the due date it is overdue. Steps are counted done of total. What
 * breaks: a weight goal reading as done before it starts, a forecast from
 * the wrong origin, or a due date read in the wrong day.
 */
static void
test_progress_math(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(VentureEntity) paced_record = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autoptr(GDateTime) expected = NULL;
	g_autofree gchar *as_of = NULL;
	g_autofree gchar *expected_text = NULL;
	gint64 skill;
	gint64 paced;
	guint row;
	gdouble days;

	(void)user_data;

	skill = goal(fixture, "Alchemy", 1, 150.5, 300, 0);
	goal(fixture, "Weight", 90, 85, 80, 0);
	goal(fixture, "Overshot", 0, 12, 10, 0);
	craft_step(fixture, skill, "One", 0, 0, TRUE);
	craft_step(fixture, skill, "Two", 0, 0, FALSE);
	craft_step(fixture, skill, "Three", 0, 0, FALSE);

	/* 0 to 100, half way; due fifteen days after it was set. */
	paced = goal(fixture, "Paced", 0, 50, 100, 0);
	paced_record = reread(fixture, VENTURE_TYPE_GOAL, paced);
	{
		g_autoptr(GDateTime) later = NULL;
		g_autoptr(GDateTime) utc = NULL;

		later = g_date_time_add_days(venture_entity_get_created_at(paced_record), 15);
		utc = g_date_time_to_utc(later);
		due = g_date_time_new_utc(g_date_time_get_year(utc), g_date_time_get_month(utc),
		                          g_date_time_get_day_of_month(utc), 0, 0, 0.0);
	}
	g_object_set(paced_record, "due-on", due, NULL);
	save(fixture, paced_record);

	as_of = days_after_creation(fixture, paced, 10);
	options = json_object_new();
	json_object_set_string_member(options, "as_of", as_of);
	result = run_ok(fixture, "goal_progress", options);

	row = row_of(result, "goal", "Alchemy", NULL);
	g_assert_cmpfloat_with_epsilon(cell_number(result, row, "percent"), 0.5, 1e-9);
	g_assert_cmpfloat_with_epsilon(cell_number(result, row, "remaining"), 149.5, 1e-9);
	g_assert_cmpfloat(cell_number(result, row, "steps_done"), ==, 1.0);
	g_assert_cmpfloat(cell_number(result, row, "steps_total"), ==, 3.0);
	g_assert_cmpstr(cell_text(result, row, "metric"), ==, "level");
	g_assert_cmpstr(cell_text(result, row, "status"), ==, "active");

	row = row_of(result, "goal", "Weight", NULL);
	g_assert_cmpfloat_with_epsilon(cell_number(result, row, "percent"), 0.5, 1e-9);
	g_assert_cmpfloat_with_epsilon(cell_number(result, row, "remaining"), 5.0, 1e-9);

	row = row_of(result, "goal", "Overshot", NULL);
	g_assert_cmpfloat_with_epsilon(cell_number(result, row, "percent"), 1.2, 1e-9);
	g_assert_cmpfloat_with_epsilon(cell_number(result, row, "remaining"), -2.0, 1e-9);
	g_assert_false(has_cell(result, row, "forecast"));
	ASSERT_CONTAINS(cell_text(result, row, "note"), "mark it achieved");

	/* Half way in ten days: twenty days from creation, after the due. */
	row = row_of(result, "goal", "Paced", NULL);
	expected = g_date_time_add_days(venture_entity_get_created_at(paced_record), 20);
	expected_text = venture_time_to_date_string(expected,
		venture_context_get_timezone(fixture->context));
	g_assert_cmpstr(cell_text(result, row, "forecast"), ==, expected_text);
	ASSERT_CONTAINS(cell_text(result, row, "note"), "after the due date");
	days = cell_number(result, row, "days_left");
	g_assert_true((days >= 4.0) && (days <= 5.0));

	/* A month on, it is overdue. */
	g_clear_pointer(&result, g_object_unref);
	g_clear_pointer(&as_of, g_free);
	as_of = days_after_creation(fixture, paced, 30);
	json_object_set_string_member(options, "as_of", as_of);
	result = run_ok(fixture, "goal_progress", options);
	row = row_of(result, "goal", "Paced", NULL);
	g_assert_cmpfloat(cell_number(result, row, "days_left"), <, 0.0);
	ASSERT_CONTAINS(cell_text(result, row, "note"), "overdue");

	/* Achieved goals and goals with no progress get no forecast. */
	{
		g_autoptr(VentureEntity) record = NULL;

		goal(fixture, "Idle", 0, 0, 10, 0);
		record = reread(fixture, VENTURE_TYPE_GOAL, skill);
		g_object_set(record, "status", VENTURE_GOAL_STATUS_ACHIEVED, NULL);
		save(fixture, record);
		g_clear_pointer(&result, g_object_unref);
		result = run_ok(fixture, "goal_progress", options);
		g_assert_false(has_cell(result, row_of(result, "goal", "Alchemy", NULL), "forecast"));
		row = row_of(result, "goal", "Idle", NULL);
		g_assert_false(has_cell(result, row, "forecast"));
		ASSERT_CONTAINS(cell_text(result, row, "note"), "no progress");
	}
}

/*
 * Which goals: sub-goals read as paths under their parent, a category
 * takes everything filed beneath it, a status or a list of statuses
 * narrows, and a venture narrows. A status that is not one is refused.
 * What breaks: a category filter that forgets its sub-categories, or a
 * misspelt status answering with every goal.
 */
static void
test_progress_filters(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(VentureEntity) record = NULL;
	gint64 professions;
	gint64 alchemy_category;
	gint64 savings;
	gint64 parent;
	gint64 child;

	(void)user_data;

	professions = category(fixture, "Professions", 0);
	alchemy_category = category(fixture, "Alchemy", professions);
	savings = category(fixture, "Savings", 0);

	parent = goal(fixture, "Alchemy 300", 1, 100, 300, 0);
	child = goal(fixture, "Alchemy 150", 1, 100, 150, parent);
	record = reread(fixture, VENTURE_TYPE_GOAL, child);
	g_object_set(record, "category-id", alchemy_category, NULL);
	save(fixture, record);
	g_clear_object(&record);
	record = goal_new(fixture, "Save 5000", 0, 1000, 5000);
	g_object_set(record, "category-id", savings, "venture-id", fixture->venture_id,
	             "status", VENTURE_GOAL_STATUS_PAUSED, NULL);
	save(fixture, record);

	result = run_ok(fixture, "goal_progress", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);
	g_assert_true(has_row(result, "goal", "Alchemy 300 / Alchemy 150"));

	/* Professions takes Alchemy beneath it. */
	options = json_object_new();
	json_object_set_int_member(options, "category_id", professions);
	g_clear_pointer(&result, g_object_unref);
	result = run_ok(fixture, "goal_progress", options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	g_assert_true(has_row(result, "goal", "Alchemy 300 / Alchemy 150"));

	g_clear_pointer(&options, json_object_unref);
	options = json_object_new();
	json_object_set_string_member(options, "status", "paused");
	g_clear_pointer(&result, g_object_unref);
	result = run_ok(fixture, "goal_progress", options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	g_assert_true(has_row(result, "goal", "Save 5000"));
	ASSERT_CONTAINS(cell_text(result, 0, "note"), "paused");

	json_object_set_string_member(options, "status", "active, paused");
	g_clear_pointer(&result, g_object_unref);
	result = run_ok(fixture, "goal_progress", options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);

	g_clear_pointer(&options, json_object_unref);
	options = json_object_new();
	json_object_set_int_member(options, "venture_id", fixture->venture_id);
	g_clear_pointer(&result, g_object_unref);
	result = run_ok(fixture, "goal_progress", options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);

	json_object_set_string_member(options, "status", "finished");
	run_refused(fixture, "goal_progress", options, "not \"finished\"");
}

/* ==========================================================================
 * goal_materials
 * ========================================================================== */

typedef struct
{
	gint64	herb;
	gint64	vial;
	gint64	pestle;
	gint64	potion;
	gint64	elixir;
	gint64	skill;
	gint64	sub;
	gint64	bank;
	gint64	bag;
} Apothecary;

/*
 * Two recipes and a levelling goal. A potion takes 2 herbs, 1 vial and a
 * pestle (a tool, 1); an elixir takes 3 herbs and 2 pestles. The goal has
 * 10 potions to craft (open), 100 more (done), and a sub-goal with 5
 * elixirs (open). An achieved goal elsewhere wants 1,000 potions. So the
 * open need is 20 + 15 = 35 herbs, 10 vials and 2 pestles -- the largest
 * single ask, not 1 + 2. Five herbs are on hand, three in the bank and
 * two in the bag.
 */
static void
apothecary(
	Fixture		*fixture,
	Apothecary	*out
){
	gint64 potion_recipe;
	gint64 elixir_recipe;
	gint64 finished;

	out->herb = product_costing(fixture, "Peacebloom", NULL);
	out->vial = product_costing(fixture, "Empty Vial", NULL);
	out->pestle = product_costing(fixture, "Pestle", NULL);
	out->potion = product_costing(fixture, "Minor Healing Potion", NULL);
	out->elixir = product_costing(fixture, "Elixir", NULL);

	potion_recipe = recipe(fixture, "Brew potion", out->potion);
	component(fixture, potion_recipe, out->herb, 2, FALSE);
	component(fixture, potion_recipe, out->vial, 1, FALSE);
	component(fixture, potion_recipe, out->pestle, 1, TRUE);
	elixir_recipe = recipe(fixture, "Brew elixir", out->elixir);
	component(fixture, elixir_recipe, out->herb, 3, FALSE);
	component(fixture, elixir_recipe, out->pestle, 2, TRUE);

	out->skill = goal(fixture, "Alchemy 300", 1, 1, 300, 0);
	out->sub = goal(fixture, "Alchemy 150", 1, 1, 150, out->skill);
	craft_step(fixture, out->skill, "Potions", potion_recipe, 10, FALSE);
	craft_step(fixture, out->skill, "Potions, done", potion_recipe, 100, TRUE);
	craft_step(fixture, out->sub, "Elixirs", elixir_recipe, 5, FALSE);
	craft_step(fixture, out->skill, "Unsaid", potion_recipe, 0, FALSE);

	{
		g_autoptr(VentureEntity) record = NULL;

		finished = goal(fixture, "Old goal", 0, 10, 10, 0);
		craft_step(fixture, finished, "Old potions", potion_recipe, 1000, FALSE);
		record = reread(fixture, VENTURE_TYPE_GOAL, finished);
		g_object_set(record, "status", VENTURE_GOAL_STATUS_ACHIEVED, NULL);
		save(fixture, record);
	}

	out->bank = location(fixture, "Bank");
	out->bag = location(fixture, "Bag");
	stock(fixture, out->herb, out->bank, 3, NULL);
	stock(fixture, out->herb, out->bag, 2, NULL);
}

/*
 * The quantities: crafts multiplied out and summed across steps, tools at
 * their largest single need, done steps and closed goals left out, a goal
 * taking its sub-goals, on hand across every location taken off -- or not,
 * when asked. What breaks: a pestle bought three times, the done step's
 * hundred potions on the list, or the bank's herbs forgotten.
 */
static void
test_materials_quantities(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;
	Apothecary shop;
	guint row;

	(void)user_data;

	apothecary(fixture, &shop);

	result = run_ok(fixture, "goal_materials", NULL);
	row = row_of(result, "product", "Peacebloom", NULL);
	g_assert_cmpfloat(cell_number(result, row, "consumed"), ==, 35.0);
	g_assert_cmpfloat(cell_number(result, row, "needed"), ==, 35.0);
	g_assert_cmpfloat(cell_number(result, row, "on_hand"), ==, 5.0);
	g_assert_cmpfloat(cell_number(result, row, "to_acquire"), ==, 30.0);
	row = row_of(result, "product", "Empty Vial", NULL);
	g_assert_cmpfloat(cell_number(result, row, "needed"), ==, 10.0);
	row = row_of(result, "product", "Pestle", NULL);
	g_assert_cmpfloat(cell_number(result, row, "reusable"), ==, 2.0);
	g_assert_cmpfloat(cell_number(result, row, "needed"), ==, 2.0);
	g_assert_false(has_row(result, "product", "Minor Healing Potion"));

	/* The sub-goal alone: its elixirs only, no vials. */
	options = json_object_new();
	json_object_set_int_member(options, "goal_id", shop.sub);
	g_clear_pointer(&result, g_object_unref);
	result = run_ok(fixture, "goal_materials", options);
	row = row_of(result, "product", "Peacebloom", NULL);
	g_assert_cmpfloat(cell_number(result, row, "needed"), ==, 15.0);
	g_assert_false(has_row(result, "product", "Empty Vial"));

	/* The parent takes the sub-goal with it. */
	json_object_set_int_member(options, "goal_id", shop.skill);
	g_clear_pointer(&result, g_object_unref);
	result = run_ok(fixture, "goal_materials", options);
	g_assert_cmpfloat(cell_number(result, row_of(result, "product", "Peacebloom", NULL),
	                              "needed"), ==, 35.0);

	/* Stock ignored when asked, as a string the way a URL sends it. */
	json_object_set_string_member(options, "include_on_hand", "false");
	g_clear_pointer(&result, g_object_unref);
	result = run_ok(fixture, "goal_materials", options);
	row = row_of(result, "product", "Peacebloom", NULL);
	g_assert_cmpfloat(cell_number(result, row, "to_acquire"), ==, 35.0);
	g_assert_false(has_cell(result, row, "on_hand"));

	json_object_set_string_member(options, "include_on_hand", "perhaps");
	run_refused(fixture, "goal_materials", options, "true or false");

	/* A goal asked for by id must be there. */
	json_object_remove_member(options, "include_on_hand");
	json_object_set_int_member(options, "goal_id", 99999);
	run_refused(fixture, "goal_materials", options, "not found");
}

/*
 * Pricing. With the market on, the latest price seen: herbs in gold,
 * vials in dollars -- two totals, never one -- and the pestle, never seen
 * priced, named with its cost blank and the totals labelled as priced
 * lines only. With the market off, recorded costs: a stock item's unit
 * cost, else the product's. price_source with the market off is refused.
 * What breaks: gold added to dollars, a pestle bought for nothing, or a
 * partial bill passed off as the whole.
 */
static void
test_materials_pricing(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;
	Apothecary shop;
	guint row;

	(void)user_data;

	apothecary(fixture, &shop);
	observe(fixture, shop.herb, "market value", "1.5000 GOLD");
	observe(fixture, shop.vial, "market value", "0.25 USD");
	observe(fixture, shop.herb, "vendor", "9.0000 GOLD");

	options = json_object_new();
	json_object_set_string_member(options, "price_source", "market value");
	result = run_ok(fixture, "goal_materials", options);

	row = row_of(result, "product", "Peacebloom", NULL);
	ASSERT_CELL_MONEY(result, row, "unit_price", "1.5000 GOLD");
	ASSERT_CELL_MONEY(result, row, "cost", "45.0000 GOLD");
	g_assert_cmpstr(cell_text(result, row, "priced_by"), ==, "market value");
	row = row_of(result, "product", "Empty Vial", NULL);
	ASSERT_CELL_MONEY(result, row, "cost", "2.50 USD");
	row = row_of(result, "product", "Pestle", NULL);
	g_assert_false(has_cell(result, row, "cost"));
	ASSERT_CONTAINS(cell_text(result, row, "note"), "no price");

	row = row_of(result, "product", "Total of priced lines", "GOLD");
	ASSERT_CELL_MONEY(result, row, "cost", "45.0000 GOLD");
	ASSERT_CONTAINS(cell_text(result, row, "note"), "Pestle");
	row = row_of(result, "product", "Total of priced lines", "USD");
	ASSERT_CELL_MONEY(result, row, "cost", "2.50 USD");
	g_assert_false(has_row(result, "product", "Total"));

	/* Every thing priced: a plain Total. */
	observe(fixture, shop.pestle, "market value", "4.0000 GOLD");
	g_clear_pointer(&result, g_object_unref);
	result = run_ok(fixture, "goal_materials", options);
	row = row_of(result, "product", "Total", "GOLD");
	ASSERT_CELL_MONEY(result, row, "cost", "53.0000 GOLD");

	/* Market off: recorded costs, and price_source refused. */
	venture_config_set_module_enabled(fixture->config, "market", FALSE);
	run_refused(fixture, "goal_materials", options, "market module is off");
	{
		g_autoptr(VentureEntity) vial = NULL;
		g_autoptr(VentureMoney) cost = NULL;

		/* The vial's own cost; the herb's stock item's carrying cost. */
		cost = money_of("0.30 USD");
		vial = reread(fixture, VENTURE_TYPE_PRODUCT, shop.vial);
		g_object_set(vial, "cost", cost, NULL);
		save(fixture, vial);
		stock(fixture, shop.herb, 0, 0, "2.0000 GOLD");
	}
	g_clear_pointer(&result, g_object_unref);
	result = run_ok(fixture, "goal_materials", NULL);
	row = row_of(result, "product", "Empty Vial", NULL);
	ASSERT_CELL_MONEY(result, row, "cost", "3.00 USD");
	g_assert_cmpstr(cell_text(result, row, "priced_by"), ==, "recorded cost");
	row = row_of(result, "product", "Peacebloom", NULL);
	ASSERT_CELL_MONEY(result, row, "unit_price", "2.0000 GOLD");
}

/*
 * The production module off: the report refuses, naming the switch,
 * rather than answering an empty list that reads as nothing to buy. A
 * recipe deleted since its step named it is left out and named. What
 * breaks: "nothing to buy" when the truth is "cannot tell".
 */
static void
test_materials_production_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) brew = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *notes = NULL;
	Apothecary shop;
	gint64 lone;
	gint64 lone_recipe;

	(void)user_data;

	apothecary(fixture, &shop);
	lone = product_costing(fixture, "Lone", NULL);
	lone_recipe = recipe(fixture, "Lone brew", lone);
	craft_step(fixture, shop.skill, "Lone step", lone_recipe, 2, FALSE);
	brew = reread(fixture, VENTURE_TYPE_RECIPE, lone_recipe);
	g_assert_true(venture_database_delete(fixture->database, brew, NULL, &error));
	g_assert_no_error(error);

	result = run_ok(fixture, "goal_materials", NULL);
	notes = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TEXT);
	ASSERT_CONTAINS(notes, "Lone step");

	venture_config_set_module_enabled(fixture->config, "production", FALSE);
	run_refused(fixture, "goal_materials", NULL, "production module");

	/* goal_progress needs nothing but core. */
	g_clear_pointer(&result, g_object_unref);
	result = run_ok(fixture, "goal_progress", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(result), >, 0);
}

/*
 * The module needs only core and suggests production and market.
 * Switching it off hides its types and reports; switching it back
 * restores them; production off leaves it on. What breaks: a report that
 * vanishes for good, or a savings goal impossible without a workshop.
 */
static void
test_module_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureEntityRegistry *types;
	VentureReportRegistry *reports;
	VentureModule *goals;

	(void)user_data;

	goals = venture_module_registry_lookup(venture_context_get_modules(fixture->context),
	                                       "goals");
	g_assert_nonnull(goals);
	g_assert_true(g_strv_contains(venture_module_get_requires(goals), "core"));
	g_assert_false(g_strv_contains(venture_module_get_requires(goals), "production"));
	g_assert_true(g_strv_contains(venture_module_get_suggests(goals), "production"));
	g_assert_true(g_strv_contains(venture_module_get_suggests(goals), "market"));
	g_assert_true(g_strv_contains(venture_module_get_reports(goals), "goal_progress"));
	g_assert_true(g_strv_contains(venture_module_get_reports(goals), "goal_materials"));
	g_assert_true(g_strv_contains(venture_module_get_entity_names(goals), "goal"));
	g_assert_true(g_strv_contains(venture_module_get_entity_names(goals), "goal_step"));

	types = venture_context_get_entity_registry(fixture->context);
	reports = venture_context_get_report_registry(fixture->context);

	venture_config_set_module_enabled(fixture->config, "goals", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "goals"));
	g_assert_true(G_TYPE_INVALID == venture_entity_registry_lookup(types, "goal"));
	g_assert_true(G_TYPE_INVALID == venture_entity_registry_lookup(types, "goal_step"));
	g_assert_null(venture_report_registry_lookup(reports, "goal_progress"));
	g_assert_null(venture_report_registry_lookup(reports, "goal_materials"));

	venture_config_set_module_enabled(fixture->config, "goals", TRUE);
	g_assert_nonnull(venture_report_registry_lookup(reports, "goal_materials"));

	venture_config_set_module_enabled(fixture->config, "production", FALSE);
	g_assert_true(venture_context_module_enabled(fixture->context, "goals"));
	g_assert_true(VENTURE_TYPE_GOAL == venture_entity_registry_lookup(types, "goal"));
}

/*
 * A progress widget on a goal: with options.start_field it measures from
 * the start, so a weight of 90 toward 80 at 85 is 50%, not 106%; without
 * one it is the plain value over target it always was. What breaks: every
 * downward goal on a dashboard reading as done before it starts.
 */
static void
test_dashboard_start_field(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) widget = NULL;
	g_autoptr(VentureDashboardWidget) bad = NULL;
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(GError) error = NULL;
	JsonArray *lines;
	gint64 weight;

	(void)user_data;

	weight = goal(fixture, "Weight", 90, 85, 80, 0);

	dashboard = venture_dashboard_new();
	g_object_set(dashboard, "name", "Goals", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(dashboard), fixture->organization_id);
	save(fixture, dashboard);

	widget = venture_dashboard_widget_new();
	g_object_set(widget, "dashboard-id", ID(dashboard), "kind", "progress",
	             "entity-type", "goal", "field", "current_value", "record-id", weight,
	             "options", "{\"target_field\": \"target_value\", "
	                        "\"start_field\": \"start_value\"}", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(widget), fixture->organization_id);
	save(fixture, widget);

	result = venture_dashboard_render_widget(fixture->context, widget, NULL);
	g_assert_null(result->error);
	lines = json_object_get_array_member(json_node_get_object(result->data), "lines");
	g_assert_cmpfloat(json_object_get_double_member(
		json_array_get_object_element(lines, 0), "percent"), ==, 50.0);
	g_assert_nonnull(strstr(result->html, "width:50%"));
	g_assert_nonnull(strstr(result->html, " from "));
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(result->data),
	                                              "start_field"), ==, "start_value");

	/* Without the start it is value over target, as before. */
	g_clear_pointer(&result, venture_widget_result_free);
	g_object_set(widget, "options", "{\"target_field\": \"target_value\"}", NULL);
	result = venture_dashboard_render_widget(fixture->context, widget, NULL);
	g_assert_null(result->error);
	lines = json_object_get_array_member(json_node_get_object(result->data), "lines");
	g_assert_cmpfloat(json_object_get_double_member(
		json_array_get_object_element(lines, 0), "percent"), ==, 106.25);
	g_assert_false(json_object_has_member(json_array_get_object_element(lines, 0), "start"));

	/* A start field that is not a number is refused at the save. */
	bad = venture_dashboard_widget_new();
	g_object_set(bad, "dashboard-id", ID(dashboard), "kind", "progress",
	             "entity-type", "goal", "field", "current_value",
	             "options", "{\"target_field\": \"target_value\", "
	                        "\"start_field\": \"metric\"}", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(bad), fixture->organization_id);
	g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(bad), NULL,
	                                     &error));
	ASSERT_CONTAINS(error->message, "options.start_field");
}

/* ==========================================================================
 * HTTP
 * ========================================================================== */

typedef struct
{
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gchar			*state_dir;
	gchar			*cookie;
	guint16			 port;
} ServerFixture;

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	RequestResult *outcome;

	outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                  result, &outcome->error);
	outcome->done = TRUE;
}

static guint
server_request(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *content_type,
	const gchar	 *body,
	gchar		**out_body
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != fixture->cookie)
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie",
			fixture->cookie);

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL, request_done,
	                                 &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

static void
server_fixture_set_up(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	(void)user_data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "goals-test-secret", TRUE);

	fixture->state_dir = g_dir_make_tmp("venture-goals-XXXXXX", NULL);
	fixture->port = (guint16)(20000 + ((getpid() + 11369) % 20000));

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)fixture->port,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             "security-password-iterations", (gint64)100000,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);

	fixture->session = soup_session_new();

	user = venture_user_new();
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER,
	             "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 100000,
	                                        NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	{
		g_autoptr(SoupMessage) message = NULL;
		g_autofree gchar *url = NULL;
		g_autoptr(GBytes) bytes = NULL;
		RequestResult outcome = { FALSE, NULL, NULL };

		url = g_strdup_printf("http://127.0.0.1:%u/login", fixture->port);
		message = soup_message_new("POST", url);
		soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
		bytes = g_bytes_new_static("username=owner&password=owner-password-1",
		                           strlen("username=owner&password=owner-password-1"));
		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
		soup_session_send_and_read_async(fixture->session, message,
		                                 G_PRIORITY_DEFAULT, NULL,
		                                 request_done, &outcome);

		while (!outcome.done)
			g_main_context_iteration(NULL, TRUE);

		g_clear_pointer(&outcome.body, g_bytes_unref);
		g_clear_error(&outcome.error);

		set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));
	}

	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	fixture->cookie = g_steal_pointer(&set_cookie);
}

static void
server_fixture_tear_down(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_pointer(&fixture->cookie, g_free);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_unsetenv("VENTURE_TEST_SESSION_SECRET");

	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}

/* Saves @record under the default organization, expecting success. */
static void
server_save(
	ServerFixture	*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	venture_entity_set_organization_id(VENTURE_ENTITY(record),
		venture_context_get_default_organization_id(fixture->context));

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

/* The number of rows in a JSON report body. */
static guint
json_rows(const gchar *body)
{
	g_autoptr(JsonNode) node = NULL;

	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);

	return json_array_get_length(json_object_get_array_member(
		json_node_get_object(node), "rows"));
}

/*
 * Every door reaches goals: the API forwards status to goal_progress and
 * goal_id and include_on_hand to goal_materials (an option dropped on the
 * way would answer a different question), both report pages offer their
 * options, the goal page lists its steps, and the record API applies the
 * step validator. What breaks: an option the page and the API never pass,
 * or a door around the rules.
 */
static void
test_http(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) herb = NULL;
	g_autoptr(VentureEntity) potion = NULL;
	g_autoptr(VentureEntity) brew = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) skill = NULL;
	g_autoptr(VentureEntity) paused = NULL;
	g_autoptr(VentureEntity) step = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *request = NULL;
	guint status;

	(void)user_data;

	herb = VENTURE_ENTITY(venture_product_new());
	g_object_set(herb, "name", "Peacebloom", NULL);
	server_save(fixture, herb);
	potion = VENTURE_ENTITY(venture_product_new());
	g_object_set(potion, "name", "Potion", NULL);
	server_save(fixture, potion);
	brew = VENTURE_ENTITY(venture_recipe_new());
	g_object_set(brew, "name", "Brew", "output-product-id", ID(potion),
	             "output-quantity", (gint64)1, "active", TRUE, NULL);
	server_save(fixture, brew);
	line = VENTURE_ENTITY(venture_recipe_component_new());
	g_object_set(line, "recipe-id", ID(brew), "product-id", ID(herb),
	             "quantity", (gint64)2, NULL);
	server_save(fixture, line);

	skill = VENTURE_ENTITY(venture_goal_new());
	g_object_set(skill, "name", "Alchemy 300", "start-value", 1.0,
	             "current-value", 40.0, "target-value", 300.0, NULL);
	server_save(fixture, skill);
	paused = VENTURE_ENTITY(venture_goal_new());
	g_object_set(paused, "name", "Savings", "start-value", 0.0,
	             "target-value", 5000.0, "status", VENTURE_GOAL_STATUS_PAUSED, NULL);
	server_save(fixture, paused);
	step = VENTURE_ENTITY(venture_goal_step_new());
	g_object_set(step, "goal-id", ID(skill), "name", "1 to 25", "recipe-id", ID(brew),
	             "repetitions", (gint64)20, NULL);
	server_save(fixture, step);

	g_assert_cmpuint(server_request(fixture, "GET",
		"/api/v1/reports/goal_progress?period=all&status=paused", NULL, NULL, &body),
		==, SOUP_STATUS_OK);
	g_assert_cmpuint(json_rows(body), ==, 1);
	g_assert_nonnull(strstr(body, "Savings"));

	g_clear_pointer(&body, g_free);
	path = g_strdup_printf("/api/v1/reports/goal_materials?period=all&goal_id=%"
	                       G_GINT64_FORMAT "&include_on_hand=false", ID(skill));
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, NULL, &body),
		==, SOUP_STATUS_OK);
	g_assert_cmpuint(json_rows(body), ==, 1);
	g_assert_nonnull(strstr(body, "Peacebloom"));
	g_assert_nonnull(strstr(body, "include_on_hand is false"));

	/* The pages: each offers its questions. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_request(fixture, "GET",
		"/reports/goal_progress?period=all&status=active", NULL, NULL, &body),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "name=\"status\""));
	g_assert_nonnull(strstr(body, "Alchemy 300"));

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_request(fixture, "GET",
		"/reports/goal_materials?period=all", NULL, NULL, &body), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "name=\"goal_id\""));
	g_assert_nonnull(strstr(body, "name=\"include_on_hand\""));
	g_assert_nonnull(strstr(body, "To acquire"));

	/* The goal page lists its steps. */
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/goal/%" G_GINT64_FORMAT, ID(skill));
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, NULL, &body),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "1 to 25"));

	/* The record API is the same validator: a goal with no distance. */
	g_clear_pointer(&body, g_free);
	status = server_request(fixture, "POST", "/api/v1/goal", "application/json",
	                        "{\"name\":\"Flat\",\"start_value\":3,\"target_value\":3}",
	                        &body);
	g_assert_cmpuint(status, ==, 422);
	g_assert_nonnull(strstr(body, "must differ from the start"));

	g_clear_pointer(&body, g_free);
	request = g_strdup_printf("{\"goal_id\":%" G_GINT64_FORMAT ",\"name\":\"Bad\","
	                          "\"repetitions\":-1}", ID(skill));
	status = server_request(fixture, "POST", "/api/v1/goal_step", "application/json",
	                        request, &body);
	g_assert_cmpuint(status, ==, 422);
	g_assert_nonnull(strstr(body, "cannot be negative"));
}

int
main(
	int	 argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/goals/validators/goal", test_goal_validators);
	ADD("/goals/validators/step", test_step_validators);
	ADD("/goals/progress/math", test_progress_math);
	ADD("/goals/progress/filters", test_progress_filters);
	ADD("/goals/materials/quantities", test_materials_quantities);
	ADD("/goals/materials/pricing", test_materials_pricing);
	ADD("/goals/materials/production-off", test_materials_production_off);
	ADD("/goals/module-off", test_module_off);
	ADD("/goals/dashboard/start-field", test_dashboard_start_field);
	g_test_add("/goals/http", ServerFixture, NULL, server_fixture_set_up,
	           test_http, server_fixture_tear_down);

#undef ADD

	return g_test_run();
}
