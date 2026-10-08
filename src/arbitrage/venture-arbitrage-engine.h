/*
 * venture-arbitrage-engine.h - Finding opportunities: fee models,
 * strategies, the scan, export formats and the calculators
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three registries hang off the #VentureContext, each with built-ins
 * registered first and each open to plugins, which register at load with
 * plain functions -- no GObject subclass to write:
 *
 *  - fee models: what a venue charges (`percent`, `commission`, `none`);
 *    a venue names one in `fee-model` and gives it `fee-params` YAML;
 *  - strategies: how opportunities are found (`spread`, `transform`,
 *    `deal`, `cover`, `back_lay`); each has a scan, which reads the series
 *    stores and adds opportunities, and a plan, which turns one into the
 *    request `venture_arbitrage_record()` takes;
 *  - export formats: how a set of opportunities is written out (`csv`,
 *    `shopping_list`).
 *
 * The scan runs on the main thread, against the stores' read handles and
 * only their precomputed, indexed columns, and every strategy is bounded
 * (see %VENTURE_ARBITRAGE_SCAN_CANDIDATES): a whole region is never read
 * row by row on a request. Nothing here adds across currencies, nothing
 * unquoted is ever read as zero, and the only way from an opportunity to
 * the books is a plan handed to the `record` action.
 */

#ifndef VENTURE_ARBITRAGE_ENGINE_H
#define VENTURE_ARBITRAGE_ENGINE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_ARBITRAGE_NAME_MAX:
 *
 * The longest fee model, strategy or export format name, in bytes. Names
 * are lower case letters, digits and underscores, starting with a letter.
 */
#define VENTURE_ARBITRAGE_NAME_MAX (64)

/**
 * VENTURE_ARBITRAGE_SCAN_TOP_MAX:
 *
 * The most opportunities one scan returns (option `top`, 50 by default).
 */
#define VENTURE_ARBITRAGE_SCAN_TOP_MAX (500)

/**
 * VENTURE_ARBITRAGE_SCAN_CANDIDATES:
 *
 * The most instruments (or events, or recipes) a strategy considers per
 * data source in one scan. Candidates are chosen by an indexed query --
 * the cheapest against their region first -- and each costs one indexed
 * read per venue set; past the bound the scan says so in a note rather
 * than stall the single-threaded server reading a whole region.
 */
#define VENTURE_ARBITRAGE_SCAN_CANDIDATES (500)

/**
 * VENTURE_ARBITRAGE_SCAN_SOURCES:
 *
 * The most data sources one scan reads.
 */
#define VENTURE_ARBITRAGE_SCAN_SOURCES (20)

/**
 * VENTURE_ARBITRAGE_MAX_UNITS:
 *
 * The most units one opportunity is priced for.
 */
#define VENTURE_ARBITRAGE_MAX_UNITS (1000000)

/* ==========================================================================
 * Fee models
 * ========================================================================== */

/**
 * VentureFeeSide:
 * @VENTURE_FEE_SIDE_SELL: what selling (or a winning bet) costs
 * @VENTURE_FEE_SIDE_BUY: what buying costs
 *
 * Which side of a trade a fee is asked about.
 */
typedef enum
{
	VENTURE_FEE_SIDE_SELL = 0,
	VENTURE_FEE_SIDE_BUY
} VentureFeeSide;

/**
 * VentureFeeQuote:
 * @fee: what the venue keeps when the trade fills, in the amount's
 *   currency; zero, never %NULL, after a successful quote
 * @deposit: (nullable): what listing costs up front, or %NULL for nothing
 * @deposit_refundable: whether a listing that sells gets @deposit back
 * @commission: the share of net winnings an exchange keeps, [0, 1)
 *
 * What a fee model says a trade costs.
 */
typedef struct
{
	VentureMoney	*fee;
	VentureMoney	*deposit;
	gboolean	 deposit_refundable;
	gdouble		 commission;
} VentureFeeQuote;

/**
 * venture_fee_quote_clear:
 * @quote: a quote
 *
 * Frees what @quote holds and zeroes it.
 */
void
venture_fee_quote_clear(VentureFeeQuote *quote);

/**
 * VentureFeeModelComputeFunc:
 * @params: the venue's parameters, already validated
 * @side: which side of the trade
 * @amount: the gross amount of the trade -- units times price; for a
 *   commission model, the net winnings
 * @units: how many units
 * @reference: (nullable): the price a deposit is a share of, for all the
 *   units, when the venue bases it on a reference rather than the listing
 *   price; a scan passes the sell venue's market value
 * @attrs: (nullable): the instrument's attributes as its source stored
 *   them -- what a model keyed on the thing itself reads, such as an
 *   auction house's vendor price; %NULL when unknown
 * @out: (out caller-allocates): the quote, zeroed by the caller
 * @user_data: the data given at registration
 * @error: (out) (optional): return location for a #GError
 *
 * Prices one trade. Runs on the main thread inside a scan or a
 * calculator; must not touch the database.
 *
 * Returns: %TRUE on success
 */
typedef gboolean (*VentureFeeModelComputeFunc)(JsonObject		 *params,
                                               VentureFeeSide		  side,
                                               const VentureMoney	 *amount,
                                               gint64			  units,
                                               const VentureMoney	 *reference,
                                               JsonObject		 *attrs,
                                               VentureFeeQuote		 *out,
                                               gpointer			  user_data,
                                               GError			**error);

/**
 * VentureFeeModelValidateFunc:
 * @params: the parameters a venue is being saved with
 * @user_data: the data given at registration
 * @error: (out) (optional): return location for a #GError
 *
 * Judges parameters when a venue writes its fee model or parameters.
 * Refuse unknown names: a misspelled `cut_percnt` read as no cut is the
 * fee nobody charged.
 *
 * Returns: %TRUE when @params are acceptable
 */
typedef gboolean (*VentureFeeModelValidateFunc)(JsonObject	 *params,
                                                gpointer	  user_data,
                                                GError		**error);

#define VENTURE_TYPE_FEE_MODEL_REGISTRY (venture_fee_model_registry_get_type())
G_DECLARE_FINAL_TYPE(VentureFeeModelRegistry, venture_fee_model_registry,
                     VENTURE, FEE_MODEL_REGISTRY, GObject)

/**
 * venture_fee_model_registry_new:
 *
 * A registry holding the built-in models, `percent`, `commission` and
 * `none`.
 *
 * Returns: (transfer full): the registry
 */
VentureFeeModelRegistry *
venture_fee_model_registry_new(void);

/**
 * venture_fee_model_registry_add:
 * @self: the registry
 * @name: the model's name, as a venue's `fee-model` names it
 * @description: one sentence for the pages and the docs
 * @compute: (scope notified): prices a trade
 * @validate: (nullable) (scope notified): judges parameters; %NULL
 *   accepts only an empty set
 * @user_data: (closure): handed to both
 * @destroy: (nullable): frees @user_data with the registry; not called
 *   when the registration is refused
 * @error: (out) (optional): return location for a #GError
 *
 * Registers a fee model. A name already taken is
 * %VENTURE_ERROR_ALREADY_EXISTS (a plugin cannot replace a built-in), a
 * name that is not lower case letters, digits and underscores is
 * %VENTURE_ERROR_INVALID_ARGUMENT.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_fee_model_registry_add(
	VentureFeeModelRegistry		 *self,
	const gchar			 *name,
	const gchar			 *description,
	VentureFeeModelComputeFunc	  compute,
	VentureFeeModelValidateFunc	  validate,
	gpointer			  user_data,
	GDestroyNotify			  destroy,
	GError				**error
);

/**
 * venture_fee_model_registry_remove:
 * @self: the registry
 * @name: a fee model's name
 *
 * Takes a registration back out. A plugin whose load fails has every name it added removed again (see venture_plugin_manager_load_file()).
 *
 * Returns: %TRUE when there was one to remove
 */
gboolean
venture_fee_model_registry_remove(
	VentureFeeModelRegistry	*self,
	const gchar	*name
);

/**
 * venture_fee_model_registry_has:
 * @self: the registry
 * @name: (nullable): a model name
 *
 * Returns: whether @name is registered
 */
gboolean
venture_fee_model_registry_has(
	VentureFeeModelRegistry	*self,
	const gchar		*name
);

/**
 * venture_fee_model_registry_dup_names:
 * @self: the registry
 *
 * Returns: (transfer full): the names, in registration order
 */
gchar **
venture_fee_model_registry_dup_names(VentureFeeModelRegistry *self);

/**
 * venture_fee_model_registry_get_description:
 * @self: the registry
 * @name: a model name
 *
 * Returns: (transfer none) (nullable): its description
 */
const gchar *
venture_fee_model_registry_get_description(
	VentureFeeModelRegistry	*self,
	const gchar		*name
);

/**
 * venture_fee_model_parse_params:
 * @text: (nullable): YAML (or JSON) mapping parameter names to values
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the parameters as an object, empty
 *   for empty text; %NULL with %VENTURE_ERROR_VALIDATION for text that is
 *   not a mapping
 */
JsonObject *
venture_fee_model_parse_params(
	const gchar	 *text,
	GError		**error
);

/**
 * venture_fee_model_registry_validate:
 * @self: the registry
 * @name: (nullable): the model; empty or %NULL is none, which takes no
 *   parameters
 * @params_text: (nullable): the parameters, YAML
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when @name is registered and accepts @params_text
 */
gboolean
venture_fee_model_registry_validate(
	VentureFeeModelRegistry	 *self,
	const gchar		 *name,
	const gchar		 *params_text,
	GError			**error
);

/**
 * venture_fee_model_registry_compute:
 * @self: the registry
 * @name: (nullable): the model; empty or %NULL is `none`
 * @params: (nullable): its parameters
 * @side: which side
 * @amount: the gross amount
 * @units: how many units, at least one
 * @reference: (nullable): the deposit reference for all the units
 * @attrs: (nullable): the instrument's stored attributes
 * @out: (out caller-allocates): the quote; clear with
 *   venture_fee_quote_clear()
 * @error: (out) (optional): return location for a #GError
 *
 * An unregistered model is %VENTURE_ERROR_NOT_FOUND naming it: a fee
 * nobody can compute is never read as no fee.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_fee_model_registry_compute(
	VentureFeeModelRegistry	 *self,
	const gchar		 *name,
	JsonObject		 *params,
	VentureFeeSide		  side,
	const VentureMoney	 *amount,
	gint64			  units,
	const VentureMoney	 *reference,
	JsonObject		 *attrs,
	VentureFeeQuote		 *out,
	GError			**error
);

/**
 * venture_context_get_fee_models:
 * @context: the wiring
 *
 * Returns: (transfer none): the context's fee model registry, made with
 *   the built-ins the first time it is asked for
 */
VentureFeeModelRegistry *
venture_context_get_fee_models(VentureContext *context);

/* ==========================================================================
 * Strategies and the scan
 * ========================================================================== */

/**
 * VentureArbitrageScan:
 *
 * One scan in progress, handed to a strategy's scan function: the
 * question, the stores, the venues' fee models, and where to put what it
 * finds. Valid only during the call.
 */
typedef struct _VentureArbitrageScan VentureArbitrageScan;

/**
 * VentureArbitrageScanFunc:
 * @scan: the scan
 * @user_data: the data given at registration
 * @error: (out) (optional): return location for a #GError
 *
 * Finds opportunities and adds each with venture_arbitrage_scan_add() or
 * venture_arbitrage_scan_add_flip(). A source that cannot be read, a row
 * that cannot be judged and a bound reached are notes, not errors; an
 * error ends the scan.
 *
 * Returns: %TRUE on success
 */
typedef gboolean (*VentureArbitrageScanFunc)(VentureArbitrageScan	 *scan,
                                             gpointer			  user_data,
                                             GError			**error);

/**
 * VentureArbitragePlanFunc:
 * @context: the wiring
 * @organization_id: the organization the trade is for
 * @opportunity: one opportunity this strategy's scan found
 * @actor: (nullable): who is recording it; promotions are theirs
 * @user_data: the data given at registration
 * @error: (out) (optional): return location for a #GError
 *
 * Turns an opportunity into the request the `record` action takes (see
 * venture_arbitrage_record()): a name, the strategy, the expected
 * figures and planned legs naming venue and instrument records. A
 * strategy with no plan of its own uses venture_arbitrage_plan_legs(),
 * which reads the opportunity's `legs`.
 *
 * Returns: (transfer full) (nullable): the request
 */
typedef JsonObject *(*VentureArbitragePlanFunc)(VentureContext		 *context,
                                                gint64			  organization_id,
                                                JsonObject		 *opportunity,
                                                const VentureActor	 *actor,
                                                gpointer		  user_data,
                                                GError			**error);

#define VENTURE_TYPE_ARBITRAGE_STRATEGY_REGISTRY (venture_arbitrage_strategy_registry_get_type())
G_DECLARE_FINAL_TYPE(VentureArbitrageStrategyRegistry, venture_arbitrage_strategy_registry,
                     VENTURE, ARBITRAGE_STRATEGY_REGISTRY, GObject)

/**
 * venture_arbitrage_strategy_registry_new:
 *
 * Returns: (transfer full): a registry holding the built-in strategies
 */
VentureArbitrageStrategyRegistry *
venture_arbitrage_strategy_registry_new(void);

/**
 * venture_arbitrage_strategy_registry_add:
 * @self: the registry
 * @name: the strategy's name; it is what a trade's `strategy` says
 * @label: a few words for a tab
 * @description: one sentence
 * @options: (nullable) (array zero-terminated=1): option names this
 *   strategy reads beyond the common ones; each must be lower case and
 *   underscored, and a common name is refused
 * @scan: (scope notified): finds opportunities
 * @plan: (nullable) (scope notified): turns one into a record request;
 *   %NULL uses venture_arbitrage_plan_legs()
 * @user_data: (closure): handed to both
 * @destroy: (nullable): frees @user_data with the registry
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success; a taken name is %VENTURE_ERROR_ALREADY_EXISTS
 */
gboolean
venture_arbitrage_strategy_registry_add(
	VentureArbitrageStrategyRegistry	 *self,
	const gchar				 *name,
	const gchar				 *label,
	const gchar				 *description,
	const gchar *const			 *options,
	VentureArbitrageScanFunc		  scan,
	VentureArbitragePlanFunc		  plan,
	gpointer				  user_data,
	GDestroyNotify				  destroy,
	GError					**error
);

/**
 * venture_arbitrage_strategy_registry_remove:
 * @self: the registry
 * @name: a strategy's name
 *
 * Takes a registration back out. A plugin whose load fails has every name it added removed again (see venture_plugin_manager_load_file()).
 *
 * Returns: %TRUE when there was one to remove
 */
gboolean
venture_arbitrage_strategy_registry_remove(
	VentureArbitrageStrategyRegistry	*self,
	const gchar	*name
);

/**
 * venture_arbitrage_strategy_registry_has:
 * @self: the registry
 * @name: (nullable): a strategy name
 *
 * Returns: whether @name is registered
 */
gboolean
venture_arbitrage_strategy_registry_has(
	VentureArbitrageStrategyRegistry	*self,
	const gchar				*name
);

/**
 * venture_arbitrage_strategy_registry_dup_names:
 * @self: the registry
 *
 * Returns: (transfer full): the names, built-ins first
 */
gchar **
venture_arbitrage_strategy_registry_dup_names(VentureArbitrageStrategyRegistry *self);

/**
 * venture_arbitrage_strategy_registry_get_label:
 * @self: the registry
 * @name: a strategy name
 *
 * Returns: (transfer none) (nullable): its label
 */
const gchar *
venture_arbitrage_strategy_registry_get_label(
	VentureArbitrageStrategyRegistry	*self,
	const gchar				*name
);

/**
 * venture_arbitrage_strategy_registry_get_description:
 * @self: the registry
 * @name: a strategy name
 *
 * Returns: (transfer none) (nullable): its description
 */
const gchar *
venture_arbitrage_strategy_registry_get_description(
	VentureArbitrageStrategyRegistry	*self,
	const gchar				*name
);

/**
 * venture_context_get_arbitrage_strategies:
 * @context: the wiring
 *
 * Returns: (transfer none): the context's strategy registry
 */
VentureArbitrageStrategyRegistry *
venture_context_get_arbitrage_strategies(VentureContext *context);

/**
 * venture_arbitrage_scan_option_names:
 *
 * The options every scan takes -- the report options of
 * `arbitrage_scan`, the query parameters of /arbitrage and the keys of a
 * preset's options -- in the order the docs list them.
 *
 * Returns: (transfer none): a %NULL-terminated list
 */
const gchar *const *
venture_arbitrage_scan_option_names(void);

/**
 * venture_arbitrage_scan_options_normalise:
 * @context: the wiring
 * @organization_id: whose presets and records may be named
 * @options: (nullable): the options as given: integers as numbers or
 *   digit strings, money as "10.00 GOLD", ratios as strings
 * @error: (out) (optional): return location for a #GError
 *
 * Reads a question: a `preset_id` is loaded first (its record's strategy,
 * source, venue sets and options) and anything given beside it wins;
 * every value is checked and written back in one spelling. An unknown
 * name is refused naming it -- a misspelt filter is a filter not applied
 * -- as is an unknown strategy.
 *
 * Returns: (transfer full) (nullable): the normalised options
 */
JsonObject *
venture_arbitrage_scan_options_normalise(
	VentureContext	 *context,
	gint64		  organization_id,
	JsonObject	 *options,
	GError		**error
);

/**
 * venture_arbitrage_scan_run:
 * @context: the wiring
 * @organization_id: whose sources, venues and records; 0 is the default
 *   organization
 * @options: (nullable): the question (see
 *   venture_arbitrage_scan_options_normalise())
 * @error: (out) (optional): return location for a #GError
 *
 * Runs one strategy over the organization's data sources and answers
 * `{available, strategy, options, notes[], rows[], examined, excluded,
 * truncated}`. Rows that pass the filters are sorted (option `sort`) and
 * cut to `top`. With the module off, feeds off or no SQLite the answer is
 * `available: false` with a note, not an error. Main thread only.
 *
 * Returns: (transfer full) (nullable): the answer
 */
JsonNode *
venture_arbitrage_scan_run(
	VentureContext	 *context,
	gint64		  organization_id,
	JsonObject	 *options,
	GError		**error
);

/**
 * venture_arbitrage_scan_get_context:
 * @scan: the scan
 *
 * Returns: (transfer none): the wiring
 */
VentureContext *
venture_arbitrage_scan_get_context(VentureArbitrageScan *scan);

/**
 * venture_arbitrage_scan_get_organization_id:
 * @scan: the scan
 *
 * Returns: the organization asked about
 */
gint64
venture_arbitrage_scan_get_organization_id(VentureArbitrageScan *scan);

/**
 * venture_arbitrage_scan_get_options:
 * @scan: the scan
 *
 * Returns: (transfer none): the normalised options, the strategy's own
 *   included
 */
JsonObject *
venture_arbitrage_scan_get_options(VentureArbitrageScan *scan);

/**
 * venture_arbitrage_scan_get_now:
 * @scan: the scan
 *
 * Returns: the Unix time the scan judges ages against
 */
gint64
venture_arbitrage_scan_get_now(VentureArbitrageScan *scan);

/**
 * venture_arbitrage_scan_get_units:
 * @scan: the scan
 *
 * Returns: the units each opportunity is priced for (option `units`)
 */
gint64
venture_arbitrage_scan_get_units(VentureArbitrageScan *scan);

/**
 * venture_arbitrage_scan_get_sources:
 * @scan: the scan
 *
 * Returns: (transfer none) (element-type VentureEntity): the data sources
 *   asked about, live and the organization's
 */
GPtrArray *
venture_arbitrage_scan_get_sources(VentureArbitrageScan *scan);

/**
 * venture_arbitrage_scan_get_buy_venues:
 * @scan: the scan
 *
 * The venue keys to buy at: `buy_venues` narrowed to `venue_group`'s
 * venues when the question names one, the group's venues when it names
 * only a group. A strategy reading candidates from an index filters by
 * this, never by the `buy_venues` option text, which knows nothing of a
 * group.
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): the keys,
 *   %NULL for every venue (an empty array is none)
 */
const gchar *const *
venture_arbitrage_scan_get_buy_venues(VentureArbitrageScan *scan);

/**
 * venture_arbitrage_scan_venue_allowed:
 * @scan: the scan
 * @buy: the buy side's set (else the sell side's)
 * @venue_key: a venue's key
 *
 * Returns: whether the question's venue set for that side (`buy_venues`
 *   or `sell_venues`, empty meaning every venue, narrowed to
 *   `venue_group`'s venues when it names one) takes @venue_key
 */
gboolean
venture_arbitrage_scan_venue_allowed(
	VentureArbitrageScan	*scan,
	gboolean		 buy,
	const gchar		*venue_key
);

/**
 * venture_arbitrage_scan_add_note:
 * @scan: the scan
 * @note: a sentence for the answer's notes; repeated notes are kept once
 */
void
venture_arbitrage_scan_add_note(
	VentureArbitrageScan	*scan,
	const gchar		*note
);

/**
 * venture_arbitrage_scan_add:
 * @scan: the scan
 * @opportunity: (transfer full): one opportunity, in the shape the docs
 *   give (at least `key`; `net`, `capital` and `currency` for the
 *   filters; `legs` for the default plan)
 *
 * Adds a candidate. The scan applies the filters, so a strategy adds what
 * it found and need not judge it.
 */
void
venture_arbitrage_scan_add(
	VentureArbitrageScan	*scan,
	JsonObject		*opportunity
);

/**
 * VentureArbitrageVenue:
 * @record_id: the venue record, 0 when the venue was never promoted
 * @name: (nullable): its name, the record's else the store's
 * @currency: (nullable): what it trades in
 * @fee_model: (nullable): its fee model, %NULL for none
 * @fee_params: (nullable): the model's parameters
 * @transfer_cost: (nullable): what moving one lot to or from it costs
 * @transfer_hours: how long that takes
 * @problem: (nullable): why its fees cannot be computed (an unregistered
 *   model, unreadable parameters); a row there is blanked naming it
 *
 * What the scan knows of one venue from its record.
 */
typedef struct
{
	gint64		 record_id;
	gchar		*name;
	gchar		*currency;
	gchar		*fee_model;
	JsonObject	*fee_params;
	VentureMoney	*transfer_cost;
	gint64		 transfer_hours;
	gchar		*problem;
} VentureArbitrageVenue;

/**
 * venture_arbitrage_scan_venue:
 * @scan: the scan
 * @data_source_id: the source the venue is in
 * @venue_key: its key in that source's store
 *
 * Returns: (transfer none): what the scan knows of the venue, read once
 *   per scan
 */
const VentureArbitrageVenue *
venture_arbitrage_scan_venue(
	VentureArbitrageScan	*scan,
	gint64			 data_source_id,
	const gchar		*venue_key
);

/**
 * venture_arbitrage_scan_fees:
 * @scan: the scan
 * @data_source_id: the venue's source
 * @venue_key: the venue
 * @side: which side
 * @amount: the gross amount
 * @units: how many units
 * @reference: (nullable): a deposit's reference for all the units
 * @instrument_key: (nullable): what is traded, so a model can read its
 *   stored attributes (a vendor price); %NULL when the side is not one
 *   instrument
 * @out: (out caller-allocates): the quote
 * @error: (out) (optional): return location for a #GError
 *
 * Prices one side at a venue with its own fee model. A venue with no
 * record charges nothing (and the row says so in its warnings); a venue
 * whose model cannot be computed is an error naming it, which a strategy
 * turns into a blanked row.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_scan_fees(
	VentureArbitrageScan	 *scan,
	gint64			  data_source_id,
	const gchar		 *venue_key,
	VentureFeeSide		  side,
	const VentureMoney	 *amount,
	gint64			  units,
	const VentureMoney	 *reference,
	const gchar		 *instrument_key,
	VentureFeeQuote		 *out,
	GError			**error
);

/**
 * venture_arbitrage_scan_convert:
 * @scan: the scan
 * @amount: an amount
 * @currency: the currency wanted
 * @error: (out) (optional): return location for a #GError
 *
 * Converts through the organization's `exchange_rate` table on the scan's
 * date, never a guessed or inverted rate. The same currency is a copy.
 *
 * Returns: (transfer full) (nullable): the amount in @currency, or %NULL
 *   with %VENTURE_ERROR_VALIDATION when there is no rate
 */
VentureMoney *
venture_arbitrage_scan_convert(
	VentureArbitrageScan	 *scan,
	const VentureMoney	 *amount,
	const gchar		 *currency,
	GError			**error
);

/**
 * VentureArbitrageSide:
 * @data_source_id: the venue's source
 * @venue_key: the venue
 * @instrument_key: what is bought or sold there
 * @instrument_name: (nullable): its name
 * @units: how many units
 * @unit_price: what one unit costs or fetches
 * @amount: (nullable): the total; %NULL is @unit_price times @units
 * @reference: (nullable): a deposit's reference price for one unit
 * @taken_at: when the price was seen (Unix seconds)
 * @interval_seconds: how often the venue updates, 0 for the default
 * @depth: the share of @units on offer at the price, or NAN
 *
 * One side of a flip, in the venue's own currency.
 */
typedef struct
{
	gint64			 data_source_id;
	const gchar		*venue_key;
	const gchar		*instrument_key;
	const gchar		*instrument_name;
	gint64			 units;
	const VentureMoney	*unit_price;
	const VentureMoney	*amount;
	const VentureMoney	*reference;
	gint64			 taken_at;
	gint64			 interval_seconds;
	gdouble			 depth;
} VentureArbitrageSide;

/**
 * VentureArbitrageMarket:
 * @sale_rate: the share of listings that sell at the sell venue, or NAN
 * @sold_per_day: units it sells a day, or NAN
 * @dispersion: sigma/mu of its price, or NAN
 * @venues: how many venues quote the instrument
 *
 * What is known of the market a flip sells into.
 */
typedef struct
{
	gdouble	sale_rate;
	gdouble	sold_per_day;
	gdouble	dispersion;
	guint	venues;
} VentureArbitrageMarket;

/**
 * venture_arbitrage_scan_add_flip:
 * @scan: the scan
 * @key: the opportunity's key, unique within the strategy
 * @title: what a person reads in the table
 * @buy: where the units are bought
 * @sell: where they are sold
 * @market: what is known of the sell side's market
 * @extra: (nullable) (transfer none): members merged into the opportunity
 * @error: (out) (optional): return location for a #GError
 *
 * The whole flip in one call, for a strategy that buys at one venue and
 * sells at another (or the same): each venue's fee model, the transfer
 * cost between them, the expected relists and listing loss, net, ROI,
 * ROI per day, annualised ROI, expected value and confidence, legs for
 * the plan. The sell side is converted into the buy side's currency
 * through an `exchange_rate`; with none the candidate is skipped with a
 * note, and a venue whose fees cannot be computed blanks the row naming
 * it. A market that never sells is left out and counted.
 *
 * Returns: %TRUE unless something failed outright
 */
gboolean
venture_arbitrage_scan_add_flip(
	VentureArbitrageScan		 *scan,
	const gchar			 *key,
	const gchar			 *title,
	const VentureArbitrageSide	 *buy,
	const VentureArbitrageSide	 *sell,
	const VentureArbitrageMarket	 *market,
	JsonObject			 *extra,
	GError				**error
);

#ifdef VENTURE_HAVE_SQLITE
/**
 * venture_arbitrage_scan_open_store:
 * @scan: the scan
 * @data_source_id: one of the scan's sources
 *
 * A read handle on the source's store, opened once per scan. A store that
 * has stored nothing yet, or cannot be read, is a note and %NULL.
 *
 * Returns: (transfer none) (nullable): the handle
 */
VentureSeriesStore *
venture_arbitrage_scan_open_store(
	VentureArbitrageScan	*scan,
	gint64			 data_source_id
);
#endif

/**
 * venture_arbitrage_add_trends:
 * @context: the wiring
 * @rows: (element-type JsonObject): a scan's rows
 *
 * Gives each row with a single buy venue a `trend`: the buy venue's
 * lowest price each day over the last 14 days, in minor units, null for
 * a day with none (or in another currency), and `trend_currency`. The
 * pages draw it as a sparkline; a report has no use for it, so it is not
 * an option. Main thread; a row whose store cannot be read is left alone.
 */
void
venture_arbitrage_add_trends(
	VentureContext	*context,
	JsonArray	*rows
);

/**
 * venture_arbitrage_plan_legs:
 * @context: the wiring
 * @organization_id: the trade's organization
 * @opportunity: an opportunity whose `legs` name venues and instruments
 *   by source and key
 * @actor: (nullable): who promotes the venues and instruments
 * @error: (out) (optional): return location for a #GError
 *
 * The default plan: promotes each leg's venue and instrument to records
 * (idempotent; deleted ones are restored), and writes the request
 * `{name, strategy, expected, legs[]}` with every leg planned. `expected`
 * carries the opportunity's profit, ROI, confidence, data age and key, so
 * the performance report can compare what was expected with what came.
 *
 * Returns: (transfer full) (nullable): the request
 */
JsonObject *
venture_arbitrage_plan_legs(
	VentureContext		 *context,
	gint64			  organization_id,
	JsonObject		 *opportunity,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_arbitrage_plan:
 * @context: the wiring
 * @organization_id: the trade's organization
 * @options: (nullable): the question that found it
 * @key: the opportunity's key
 * @actor: (nullable): who is planning
 * @out_opportunity: (out) (optional) (transfer full): the opportunity as
 *   it stands now
 * @error: (out) (optional): return location for a #GError
 *
 * Runs the question again, narrowed to the opportunity's instrument, and
 * hands the opportunity with @key to its strategy's plan. An opportunity
 * that is no longer there -- the prices moved -- is
 * %VENTURE_ERROR_NOT_FOUND saying so: what is recorded is what the data
 * says now, never what a page showed a minute ago.
 *
 * Returns: (transfer full) (nullable): the record request
 */
JsonObject *
venture_arbitrage_plan(
	VentureContext		 *context,
	gint64			  organization_id,
	JsonObject		 *options,
	const gchar		 *key,
	const VentureActor	 *actor,
	JsonObject		**out_opportunity,
	GError			**error
);

/**
 * venture_arbitrage_record_opportunity:
 * @context: the wiring
 * @organization_id: the trade's organization
 * @options: (nullable): the question that found it
 * @key: the opportunity's key
 * @actor: (nullable): who records it
 * @role: the caller's role, which the action checks
 * @error: (out) (optional): return location for a #GError
 *
 * Plans the opportunity and performs the `record` action with the plan:
 * the one path from an opportunity to the books, so every check the
 * action makes -- the organization's roles, a second actor's approval,
 * the legs' own validators -- applies.
 *
 * Returns: (transfer full) (nullable): the planned trade
 */
VentureEntity *
venture_arbitrage_record_opportunity(
	VentureContext		 *context,
	gint64			  organization_id,
	JsonObject		 *options,
	const gchar		 *key,
	const VentureActor	 *actor,
	VentureUserRole		  role,
	GError			**error
);

/**
 * venture_arbitrage_stage_opportunity:
 * @context: the wiring
 * @organization_id: the trade's organization
 * @options: (nullable): the question that found it
 * @key: the opportunity's key
 * @actor: (nullable): who proposes it
 * @role: the proposer's role
 * @via: (nullable): the surface it came through ("rest-api" when %NULL)
 * @error: (out) (optional): return location for a #GError
 *
 * Plans the opportunity as venture_arbitrage_record_opportunity() does and
 * stages the `record` action with that plan instead of performing it:
 * nothing reaches the books until a second person approves, and approval
 * performs the same parameters. Planning promotes the venues and
 * instruments the legs name (idempotent records of what the store saw),
 * because the legs must name records; the trade and its legs wait.
 *
 * Returns: (transfer none) (nullable): the pending confirmation
 */
VentureConfirmation *
venture_arbitrage_stage_opportunity(
	VentureContext		 *context,
	gint64			  organization_id,
	JsonObject		 *options,
	const gchar		 *key,
	const VentureActor	 *actor,
	VentureUserRole		  role,
	const gchar		 *via,
	GError			**error
);

/* ==========================================================================
 * Export formats
 * ========================================================================== */

/**
 * VentureExportFunc:
 * @opportunities: (element-type JsonObject): the rows a scan answered
 * @options: (nullable): the question that found them
 * @user_data: the data given at registration
 * @error: (out) (optional): return location for a #GError
 *
 * Writes a set of opportunities out.
 *
 * Returns: (transfer full) (nullable): the file's bytes
 */
typedef GBytes *(*VentureExportFunc)(JsonArray	 *opportunities,
                                     JsonObject	 *options,
                                     gpointer	  user_data,
                                     GError	**error);

#define VENTURE_TYPE_EXPORT_FORMAT_REGISTRY (venture_export_format_registry_get_type())
G_DECLARE_FINAL_TYPE(VentureExportFormatRegistry, venture_export_format_registry,
                     VENTURE, EXPORT_FORMAT_REGISTRY, GObject)

/**
 * venture_export_format_registry_new:
 *
 * Returns: (transfer full): a registry holding `csv` and `shopping_list`
 */
VentureExportFormatRegistry *
venture_export_format_registry_new(void);

/**
 * venture_export_format_registry_add:
 * @self: the registry
 * @name: the format's name, as `format=` asks for it
 * @label: a few words for a link
 * @content_type: the MIME type served, e.g. "text/csv; charset=utf-8"
 * @extension: the file name's extension, without the dot
 * @func: (scope notified): writes the file
 * @user_data: (closure): handed to @func
 * @destroy: (nullable): frees @user_data with the registry
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success; a taken name is %VENTURE_ERROR_ALREADY_EXISTS
 */
gboolean
venture_export_format_registry_add(
	VentureExportFormatRegistry	 *self,
	const gchar			 *name,
	const gchar			 *label,
	const gchar			 *content_type,
	const gchar			 *extension,
	VentureExportFunc		  func,
	gpointer			  user_data,
	GDestroyNotify			  destroy,
	GError				**error
);

/**
 * venture_export_format_registry_remove:
 * @self: the registry
 * @name: a export format's name
 *
 * Takes a registration back out. A plugin whose load fails has every name it added removed again (see venture_plugin_manager_load_file()).
 *
 * Returns: %TRUE when there was one to remove
 */
gboolean
venture_export_format_registry_remove(
	VentureExportFormatRegistry	*self,
	const gchar	*name
);

/**
 * venture_export_format_registry_has:
 * @self: the registry
 * @name: (nullable): a format name
 *
 * Returns: whether @name is registered
 */
gboolean
venture_export_format_registry_has(
	VentureExportFormatRegistry	*self,
	const gchar			*name
);

/**
 * venture_export_format_registry_dup_names:
 * @self: the registry
 *
 * Returns: (transfer full): the names, built-ins first
 */
gchar **
venture_export_format_registry_dup_names(VentureExportFormatRegistry *self);

/**
 * venture_export_format_registry_get_label:
 * @self: the registry
 * @name: a format
 *
 * Returns: (transfer none) (nullable): its label
 */
const gchar *
venture_export_format_registry_get_label(
	VentureExportFormatRegistry	*self,
	const gchar			*name
);

/**
 * venture_export_format_registry_get_content_type:
 * @self: the registry
 * @name: a format
 *
 * Returns: (transfer none) (nullable): its MIME type
 */
const gchar *
venture_export_format_registry_get_content_type(
	VentureExportFormatRegistry	*self,
	const gchar			*name
);

/**
 * venture_export_format_registry_get_extension:
 * @self: the registry
 * @name: a format
 *
 * Returns: (transfer none) (nullable): its file extension
 */
const gchar *
venture_export_format_registry_get_extension(
	VentureExportFormatRegistry	*self,
	const gchar			*name
);

/**
 * venture_export_format_registry_export:
 * @self: the registry
 * @name: a format
 * @opportunities: (element-type JsonObject): the rows
 * @options: (nullable): the question
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the file; an unknown format is
 *   %VENTURE_ERROR_NOT_FOUND naming the known ones
 */
GBytes *
venture_export_format_registry_export(
	VentureExportFormatRegistry	 *self,
	const gchar			 *name,
	JsonArray			 *opportunities,
	JsonObject			 *options,
	GError				**error
);

/**
 * venture_context_get_export_formats:
 * @context: the wiring
 *
 * Returns: (transfer none): the context's export format registry
 */
VentureExportFormatRegistry *
venture_context_get_export_formats(VentureContext *context);

/* ==========================================================================
 * Calculators
 * ========================================================================== */

/**
 * venture_arbitrage_calculate:
 * @calculator: "surebet", "back_lay" or "flip"
 * @input: the form's fields (see docs/arbitrage.org, "Calculators")
 * @error: (out) (optional): return location for a #GError
 *
 * The /arbitrage/calc page's arithmetic, on the server, with nothing
 * read from the database: the page and its JSON twin both call this.
 *
 * Returns: (transfer full) (nullable): the figures
 */
JsonNode *
venture_arbitrage_calculate(
	const gchar	 *calculator,
	JsonObject	 *input,
	GError		**error
);

/* ==========================================================================
 * Installation
 * ========================================================================== */

/**
 * venture_arbitrage_engine_install:
 * @context: the wiring
 *
 * Hands the database this context's registries (the last context over a
 * database wins, as the tests build several) and, once per database,
 * installs the save validators that hold a venue's fee model and an
 * `arbitrage_strategy` preset to what is registered. Called by
 * venture_arbitrage_install().
 */
void
venture_arbitrage_engine_install(VentureContext *context);

G_END_DECLS

#endif /* VENTURE_ARBITRAGE_ENGINE_H */
