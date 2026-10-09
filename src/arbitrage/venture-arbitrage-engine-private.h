/*
 * venture-arbitrage-engine-private.h - What the engine's sources share
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Small readers every part of the engine needs the same way: a registry
 * name, a percent or an amount given as a JSON number or a string, money
 * and ratios written into an answer. Not installed: include
 * "arbitrage/venture-arbitrage-engine-private.h" only from src/arbitrage.
 */

#ifndef VENTURE_ARBITRAGE_ENGINE_PRIVATE_H
#define VENTURE_ARBITRAGE_ENGINE_PRIVATE_H

#include "venture.h"

G_BEGIN_DECLS

/* The registries' database keys: the last context over a database wins. */
#define VENTURE_ARBITRAGE_FEES_KEY "venture-arbitrage-fee-models"
#define VENTURE_ARBITRAGE_STRATEGIES_KEY "venture-arbitrage-strategies"

gboolean
venture_arbitrage_name_check(
	const gchar	 *what,
	const gchar	 *name,
	GError		**error
);

/* YAML (or JSON) text that must be a mapping; empty is an empty object.
 * @what names it in a refusal ("Fee parameters"), @example shows one. */
JsonObject *
venture_arbitrage_parse_mapping(
	const gchar	 *text,
	const gchar	 *what,
	const gchar	 *example,
	GError		**error
);

/* A member as text: a string as it is, a number written out; NULL when
 * absent or null. */
gchar *
venture_arbitrage_node_text(JsonNode *node);

/* A percent member in parts per million; absent is @fallback. */
gboolean
venture_arbitrage_member_percent(
	JsonObject	 *object,
	const gchar	 *member,
	gint64		  fallback,
	gboolean	  allow_negative,
	gint64		 *out_ppm,
	GError		**error
);

/* An amount member read in @currency when it names none; absent is NULL
 * and TRUE. Another currency than @currency is refused. */
gboolean
venture_arbitrage_member_money(
	JsonObject	 *object,
	const gchar	 *member,
	const gchar	 *currency,
	VentureMoney	**out,
	GError		**error
);

/* A boolean member: true/false, yes/no, on/off, 1/0; absent is @fallback. */
gboolean
venture_arbitrage_member_bool(
	JsonObject	 *object,
	const gchar	 *member,
	gboolean	  fallback,
	gboolean	 *out,
	GError		**error
);

/* Writers for an answer: null for a missing value, never a zero. */
void
venture_arbitrage_set_money(
	JsonObject		*object,
	const gchar		*member,
	const VentureMoney	*money
);

void
venture_arbitrage_set_ratio(
	JsonObject	*object,
	const gchar	*member,
	gdouble		 value
);

/* A money member of an answer; NULL when null or absent. */
VentureMoney *
venture_arbitrage_get_money(
	JsonObject	*object,
	const gchar	*member
);

/* A ratio member of an answer; NAN when null or absent. */
gdouble
venture_arbitrage_get_ratio(
	JsonObject	*object,
	const gchar	*member
);

/* The venue save validator: a fee model and its parameters, when written. */
gboolean
venture_arbitrage_fees_validate_venue(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
);

/* The scan with every candidate kept (craft_arbitrage shows losses too). */
JsonNode *
venture_arbitrage_scan_run_full(
	VentureContext	 *context,
	gint64		  organization_id,
	JsonObject	 *options,
	gboolean	  keep_all,
	GError		**error
);

/* One CSV field, quoted when it must be and defused when a spreadsheet
 * would run it as a formula: every CSV the module writes uses it. */
void
venture_arbitrage_csv_field(
	GString		*out,
	const gchar	*text
);

/* The built-in strategies, export formats; registered by the registries'
 * constructors. */
void
venture_arbitrage_register_builtin_strategies(VentureArbitrageStrategyRegistry *registry);

void
venture_arbitrage_register_builtin_exports(VentureExportFormatRegistry *registry);

G_END_DECLS

#endif /* VENTURE_ARBITRAGE_ENGINE_PRIVATE_H */
