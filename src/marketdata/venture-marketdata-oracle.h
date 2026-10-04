/*
 * venture-marketdata-oracle.h - What something is worth, from market data
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The price oracle answers one question -- what is this instrument (or
 * the product it is) worth, at this venue or in this group, on this
 * basis, at this moment -- from the series stores the feeds module keeps,
 * and only when the caller allows it from the market module's price
 * observations. It never converts a currency: a figure priced in another
 * currency than the one asked for is no answer, and the evidence says so.
 * Nothing observed is not an error: the call succeeds with no price,
 * exactly as venture_market_latest_price() does.
 *
 * Every read happens on the calling thread, which must be the main one:
 * stores are opened through the feeds service. An oracle object keeps the
 * read handles it opened for as long as it lives, so a report that asks
 * about a hundred products opens each store once; make one per report or
 * request and drop it after, so a purged store is never read through a
 * stale handle.
 */

#ifndef VENTURE_MARKETDATA_ORACLE_H
#define VENTURE_MARKETDATA_ORACLE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VentureMarketdataQuestion:
 * @organization_id: the organization asking; instruments, venues and
 *   observations of any other are never read
 * @instrument_id: the instrument, or 0 to ask about @product_id
 * @product_id: the product, asked about through every instrument that
 *   names it (oldest first), when @instrument_id is 0
 * @venue_id: a venue record, or 0
 * @where: (nullable): a venue key or a group key in the instrument's
 *   store, when @venue_id is 0; a key the store knows as a venue is a
 *   venue, anything else is a group. Neither: every venue (the cheapest
 *   for min, all of them for quantity) or, for figures that need a group,
 *   the store's only group
 * @basis: which figure
 * @at: (nullable): the moment to answer for; %NULL means now. A current
 *   figure answers only when it was taken at or before @at; otherwise the
 *   hourly, then the daily, history is read. Region figures are current
 *   only
 * @currency: (nullable): only a figure in this currency answers
 * @prefer_currency: (nullable): among several instruments, the first
 *   answering in this currency wins; any other only when none does
 * @allow_fallback: when the stores have nothing, answer from the newest
 *   price observation of the product (prices only)
 * @fallback_source: (nullable): only observations from this source,
 *   matched exactly
 *
 * One question for the oracle. Fill it with
 * venture_marketdata_question_init() first: it is a plain struct, and a
 * member added later must read as "not asked" for every caller.
 */
typedef struct
{
	gint64			 organization_id;
	gint64			 instrument_id;
	gint64			 product_id;
	gint64			 venue_id;
	const gchar		*where;
	VentureMarketdataBasis	 basis;
	GDateTime		*at;
	const gchar		*currency;
	const gchar		*prefer_currency;
	gboolean		 allow_fallback;
	const gchar		*fallback_source;
} VentureMarketdataQuestion;

/**
 * venture_marketdata_question_init:
 * @question: (out caller-allocates): the question to clear
 *
 * Sets every member to "not asked": no instrument, product or venue, the
 * market basis, now, any currency, no fallback.
 */
void
venture_marketdata_question_init(VentureMarketdataQuestion *question);

/**
 * VentureMarketdataEvidence:
 * @basis: the basis asked for
 * @origin: (nullable): "series" or "observation" for an answer; %NULL
 *   when nothing answered
 * @data_source_id: the data source whose store answered, or 0
 * @instrument_id: the instrument record that answered, or 0
 * @instrument_key: (nullable): its key in the store
 * @venue_key: (nullable): the venue that answered, for a venue's figure
 * @group_key: (nullable): the group that answered, for a group's figure
 *   ("" for the venues with no group)
 * @taken_at: when the figure was taken (Unix seconds), or 0 when unknown
 * @observation_id: the price observation that answered, or 0
 * @observation_source: (nullable): that observation's source
 * @note: (nullable): why nothing answered, or what to know about the
 *   answer
 *
 * Where an answer came from, so a report or a page can say so.
 */
typedef struct
{
	VentureMarketdataBasis	 basis;
	gchar			*origin;
	gint64			 data_source_id;
	gint64			 instrument_id;
	gchar			*instrument_key;
	gchar			*venue_key;
	gchar			*group_key;
	gint64			 taken_at;
	gint64			 observation_id;
	gchar			*observation_source;
	gchar			*note;
} VentureMarketdataEvidence;

#define VENTURE_TYPE_MARKETDATA_EVIDENCE (venture_marketdata_evidence_get_type())

GType
venture_marketdata_evidence_get_type(void) G_GNUC_CONST;

/**
 * venture_marketdata_evidence_copy:
 * @evidence: (nullable): the evidence
 *
 * Returns: (transfer full) (nullable): a copy
 */
VentureMarketdataEvidence *
venture_marketdata_evidence_copy(const VentureMarketdataEvidence *evidence);

/**
 * venture_marketdata_evidence_free:
 * @evidence: (nullable): the evidence
 */
void
venture_marketdata_evidence_free(VentureMarketdataEvidence *evidence);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureMarketdataEvidence, venture_marketdata_evidence_free)

/**
 * venture_marketdata_evidence_to_json:
 * @evidence: the evidence
 *
 * Returns: (transfer full): the evidence as a JSON object, with the basis
 *   by its nick and the time as ISO 8601 (or null)
 */
JsonNode *
venture_marketdata_evidence_to_json(const VentureMarketdataEvidence *evidence);

#define VENTURE_TYPE_MARKETDATA_ORACLE (venture_marketdata_oracle_get_type())

G_DECLARE_FINAL_TYPE(VentureMarketdataOracle, venture_marketdata_oracle,
                     VENTURE, MARKETDATA_ORACLE, GObject)

/**
 * venture_marketdata_oracle_new:
 * @context: the wiring
 *
 * An oracle that keeps the store handles it opens until it is dropped.
 *
 * Returns: (transfer full): a new oracle
 */
VentureMarketdataOracle *
venture_marketdata_oracle_new(VentureContext *context);

/**
 * venture_marketdata_oracle_price:
 * @self: an oracle
 * @question: what to ask; its basis must be a price
 * @out_price: (out) (optional) (nullable) (transfer full): the price, or
 *   %NULL when nothing answered
 * @out_evidence: (out) (optional) (transfer full): where it came from, or
 *   why nothing did
 * @error: (out) (optional): return location for a #GError
 *
 * A price. Refused (%VENTURE_ERROR_INVALID_ARGUMENT) for a number basis, a
 * question naming both a venue record and a key, one naming neither an
 * instrument nor a product, and a group-wide question about a store whose
 * venues are in several groups (name one). An instrument or venue of
 * another organization is %VENTURE_ERROR_NOT_FOUND.
 *
 * Returns: %TRUE on success, including when nothing answered
 */
gboolean
venture_marketdata_oracle_price(
	VentureMarketdataOracle			 *self,
	const VentureMarketdataQuestion		 *question,
	VentureMoney				**out_price,
	VentureMarketdataEvidence		**out_evidence,
	GError					**error
);

/**
 * venture_marketdata_oracle_number:
 * @self: an oracle
 * @question: what to ask; its basis must be sale_rate, sold_per_day or
 *   quantity
 * @out_value: (out): the number, or NAN when nothing answered
 * @out_evidence: (out) (optional) (transfer full): where it came from
 * @error: (out) (optional): return location for a #GError
 *
 * A number. Observations carry no rates, so @allow_fallback is ignored.
 *
 * Returns: %TRUE on success, including when nothing answered
 */
gboolean
venture_marketdata_oracle_number(
	VentureMarketdataOracle			 *self,
	const VentureMarketdataQuestion		 *question,
	gdouble					 *out_value,
	VentureMarketdataEvidence		**out_evidence,
	GError					**error
);

/**
 * venture_marketdata_reference_price:
 * @context: the wiring
 * @question: what to ask
 * @out_price: (out) (optional) (nullable) (transfer full): the price
 * @out_evidence: (out) (optional) (transfer full): where it came from
 * @error: (out) (optional): return location for a #GError
 *
 * venture_marketdata_oracle_price() with an oracle of its own, for one
 * question.
 *
 * Returns: %TRUE on success, including when nothing answered
 */
gboolean
venture_marketdata_reference_price(
	VentureContext				 *context,
	const VentureMarketdataQuestion		 *question,
	VentureMoney				**out_price,
	VentureMarketdataEvidence		**out_evidence,
	GError					**error
);

/**
 * venture_marketdata_reference_number:
 * @context: the wiring
 * @question: what to ask
 * @out_value: (out): the number, or NAN
 * @out_evidence: (out) (optional) (transfer full): where it came from
 * @error: (out) (optional): return location for a #GError
 *
 * venture_marketdata_oracle_number() with an oracle of its own.
 *
 * Returns: %TRUE on success, including when nothing answered
 */
gboolean
venture_marketdata_reference_number(
	VentureContext				 *context,
	const VentureMarketdataQuestion		 *question,
	gdouble					 *out_value,
	VentureMarketdataEvidence		**out_evidence,
	GError					**error
);

/**
 * venture_marketdata_oracle_source_price:
 * @self: an oracle
 * @organization_id: the organization
 * @product_id: the product
 * @basis: the basis a `series:` price source named
 * @where: (nullable): the venue key or group it named
 * @currency: (nullable): the currency of the report's valuation
 * @strict: %TRUE: only @currency; %FALSE: @currency preferred
 * @at: (nullable): the moment to value at
 * @out_price: (out) (nullable) (transfer full): the price, or %NULL
 * @error: (out) (optional): return location for a #GError
 *
 * What a valuing report asks for a `series:` price source: the product
 * through its instruments, never falling back to observations -- a report
 * that named a series asked for market data, and a recorded price in its
 * place would be a different number presented as the same. Shared by
 * recipe_margin and goal_materials so the two cannot disagree.
 *
 * Returns: %TRUE on success, including when nothing answered
 */
gboolean
venture_marketdata_oracle_source_price(
	VentureMarketdataOracle	 *self,
	gint64			  organization_id,
	gint64			  product_id,
	VentureMarketdataBasis	  basis,
	const gchar		 *where,
	const gchar		 *currency,
	gboolean		  strict,
	GDateTime		 *at,
	VentureMoney		**out_price,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_MARKETDATA_ORACLE_H */
