/*
 * venture-marketdata.h - Venues, instruments, promotion and price sources
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The marketdata module joins the series stores the feeds module fills to
 * the books. Its records -- venue, instrument, watchlist and
 * watchlist_entry -- are held to their rules by the save validators
 * installed here. Promotion turns a venue or an instrument a store has
 * seen into a record, once, however many times it is asked. And the
 * `series:` price-source grammar is parsed here, the one place that
 * decides whether a report's price_source names an observation source or
 * a question for the price oracle (venture-marketdata-oracle.h).
 */

#ifndef VENTURE_MARKETDATA_H
#define VENTURE_MARKETDATA_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_MARKETDATA_SERIES_PREFIX:
 *
 * What a price source starts with when it is a question for the price
 * oracle rather than the exact name of an observation source:
 * `series:<basis>[@<venue key or group>]`.
 */
#define VENTURE_MARKETDATA_SERIES_PREFIX "series:"

/**
 * VENTURE_MARKETDATA_MAX_KEY_LENGTH:
 *
 * The longest namespace, key or group a venue or instrument may carry, in
 * bytes: the series store's own limit for a key.
 */
#define VENTURE_MARKETDATA_MAX_KEY_LENGTH (512)

/**
 * venture_marketdata_install:
 * @context: the wiring
 *
 * Installs the save validators for venues, instruments, watchlists and
 * their entries: the derived, unique `external-ref`, references kept in
 * the record's organization, an instrument tree with no loop, amounts that
 * are not negative and a watchlist that names an instrument once. Called
 * once by the context; a second context over the same database installs
 * nothing twice.
 */
void
venture_marketdata_install(VentureContext *context);

/**
 * venture_marketdata_basis_is_number:
 * @basis: a basis
 *
 * Returns: %TRUE for the bases that answer with a number rather than a
 *   price: sale_rate, sold_per_day and quantity
 */
gboolean
venture_marketdata_basis_is_number(VentureMarketdataBasis basis);

/**
 * venture_marketdata_external_ref:
 * @namespace_: (nullable): the namespace
 * @key: (nullable): the key
 *
 * Joins a namespace and a key into the reference that makes a venue or an
 * instrument unique in its organization: "wow-item:2770", or ":2770" with
 * no namespace, so a key that happens to contain a colon cannot pass for
 * a namespaced one.
 *
 * Returns: (transfer full) (nullable): the reference, or %NULL when @key
 *   is empty -- a record typed in with no key has no reference
 */
gchar *
venture_marketdata_external_ref(
	const gchar	*namespace_,
	const gchar	*key
);

/**
 * venture_marketdata_find_by_ref:
 * @database: the database to read
 * @type: %VENTURE_TYPE_VENUE or %VENTURE_TYPE_INSTRUMENT
 * @organization_id: the organization
 * @external_ref: the reference
 * @error: (out) (optional): return location for a #GError
 *
 * The record with @external_ref in the organization, deleted or not: the
 * unique index counts deleted rows, so a lookup that skipped them would
 * try to insert a duplicate and fail on every retry.
 *
 * Returns: (transfer full) (nullable): the record, or %NULL (with @error
 *   unset) when there is none
 */
VentureEntity *
venture_marketdata_find_by_ref(
	VentureDatabase	 *database,
	GType		  type,
	gint64		  organization_id,
	const gchar	 *external_ref,
	GError		**error
);

/**
 * venture_marketdata_promote_instrument:
 * @context: the wiring
 * @organization_id: the organization the record belongs to; the data
 *   source must be one of its own
 * @data_source_id: the source whose store has seen the instrument
 * @key: the instrument's key in that store
 * @actor: (nullable): who is responsible
 * @out_instrument: (out) (optional) (transfer full): the record
 * @error: (out) (optional): return location for a #GError
 *
 * Makes an instrument record from the store's row: its name, kind,
 * namespace (the row's, else the source's instrument namespace), the
 * category whose path matches the row's, its attributes, and its parent,
 * promoted first. Idempotent: a record with the same reference is
 * returned as it is -- edits made to it are kept -- and a deleted one is
 * restored rather than duplicated.
 *
 * Needs the marketdata module and a store to read, which needs the feeds
 * module (and a build with SQLite).
 *
 * Returns: %TRUE on success
 */
gboolean
venture_marketdata_promote_instrument(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  data_source_id,
	const gchar		 *key,
	const VentureActor	 *actor,
	VentureEntity		**out_instrument,
	GError			**error
);

/**
 * venture_marketdata_promote_venue:
 * @context: the wiring
 * @organization_id: the organization the record belongs to
 * @data_source_id: the source whose store has seen the venue
 * @key: the venue's key in that store
 * @actor: (nullable): who is responsible
 * @out_venue: (out) (optional) (transfer full): the record
 * @error: (out) (optional): return location for a #GError
 *
 * Makes a venue record from the store's row -- name, kind, namespace,
 * group and currency -- under the same rules as
 * venture_marketdata_promote_instrument().
 *
 * Returns: %TRUE on success
 */
gboolean
venture_marketdata_promote_venue(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  data_source_id,
	const gchar		 *key,
	const VentureActor	 *actor,
	VentureEntity		**out_venue,
	GError			**error
);

/**
 * venture_marketdata_known_keys:
 * @context: the wiring
 * @data_source: the data source
 *
 * The keys of the instrument records filed under @data_source, for a
 * source that tracks only known instruments: promoting an instrument, or
 * typing one in with its key, is how it becomes known. Read under a
 * trusted scope, because the feeds service freezes every organization's
 * sources whoever saved last. Empty while the marketdata module is off.
 *
 * Returns: (transfer full) (element-type utf8): the keys
 */
GPtrArray *
venture_marketdata_known_keys(
	VentureContext	*context,
	VentureEntity	*data_source
);

/**
 * venture_marketdata_parse_price_source:
 * @text: (nullable): a report's price_source
 * @out_series: (out): %TRUE when @text is a `series:` question
 * @out_basis: (out) (optional): the basis it asks for
 * @out_where: (out) (optional) (transfer full) (nullable): the venue key
 *   or group after `@`, or %NULL for none
 * @error: (out) (optional): return location for a #GError
 *
 * Parses `series:<basis>[@<venue key or group>]`. Run before anything
 * looks a price source up as an observation source: the grammar is
 * decided first, so `series:min` is never matched against an observation
 * source of that name (which the market module refuses to save). Text
 * that does not start with the prefix is not a series source and not an
 * error. A basis that is a number rather than a price -- sale_rate,
 * sold_per_day, quantity -- is refused, as are an empty basis and an empty
 * place after `@`.
 *
 * Returns: %TRUE when @text is a valid price source of either kind
 */
gboolean
venture_marketdata_parse_price_source(
	const gchar		 *text,
	gboolean		 *out_series,
	VentureMarketdataBasis	 *out_basis,
	gchar			**out_where,
	GError			**error
);

/**
 * venture_marketdata_series_available:
 * @context: the wiring
 * @error: (out) (optional): return location for a #GError
 *
 * Whether a `series:` price source can be answered here: the marketdata
 * module (whose instruments map products to store rows) and the feeds
 * module (whose stores hold the figures) are on, in a build with SQLite.
 * A report asked such a question when it cannot be answered refuses it,
 * naming what is off, rather than reading every product as unpriced.
 *
 * Returns: %TRUE when it can, %FALSE with @error set when it cannot
 */
gboolean
venture_marketdata_series_available(
	VentureContext	 *context,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_MARKETDATA_H */
