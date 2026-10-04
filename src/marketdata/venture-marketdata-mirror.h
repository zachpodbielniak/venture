/*
 * venture-marketdata-mirror.h - The operator's accounts and positions as records
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A data source that pushes account operations keeps the operator's
 * accounts and open positions in its series store (venture-series-
 * accounts.h). Two things cross from there into the books, both written
 * here and nowhere else:
 *
 *  - an account becomes a `location` -- a character, a warband bank, a
 *    seller account -- found again by its `external-ref`, so stock and
 *    money can be filed under it;
 *  - a position becomes a `listing`, kept in step while it is open and
 *    closed as sold, partial, expired or cancelled when it is gone,
 *    judged from the store's external ledger. That is what makes the sale
 *    rate, the undercut alert and the expiry widget see the operator's
 *    real auctions.
 *
 * The mirror runs on the main thread after every run of a source (a feeds
 * hook), bounded per run; it never touches a listing it did not make and
 * never overwrites a field a person changed. docs/market-data.org
 * ("Accounts and positions in the books") has every rule.
 */

#ifndef VENTURE_MARKETDATA_MIRROR_H
#define VENTURE_MARKETDATA_MIRROR_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_MARKETDATA_MIRROR_HOOK:
 *
 * The feeds hook's name; also the actor name the mirror writes as.
 */
#define VENTURE_MARKETDATA_MIRROR_HOOK "mirror"

/**
 * VENTURE_MARKETDATA_MIRROR_MAX_WRITES:
 *
 * How many records one mirror pass writes by default -- listings,
 * locations and the venues, instruments and products it promotes on the
 * way -- before it stops and leaves the rest to the next run. A source's
 * `mirror_max_writes` setting changes it, up to
 * %VENTURE_MARKETDATA_MIRROR_WRITES_LIMIT.
 */
#define VENTURE_MARKETDATA_MIRROR_MAX_WRITES (500)

/**
 * VENTURE_MARKETDATA_MIRROR_WRITES_LIMIT:
 *
 * The most `mirror_max_writes` may say.
 */
#define VENTURE_MARKETDATA_MIRROR_WRITES_LIMIT (5000)

/**
 * VENTURE_MARKETDATA_MIRROR_GRACE_HOURS:
 *
 * How long a position that vanished before it expired waits for the
 * ledger row that says what became of it before it is closed as
 * cancelled, by default; the `mirror_grace_hours` setting changes it.
 */
#define VENTURE_MARKETDATA_MIRROR_GRACE_HOURS (48)

/**
 * VENTURE_MARKETDATA_MIRROR_MAX_GRACE_HOURS:
 *
 * The longest `mirror_grace_hours` may say: thirty days.
 */
#define VENTURE_MARKETDATA_MIRROR_MAX_GRACE_HOURS (720)

/**
 * VENTURE_MARKETDATA_MIRROR_BATCH:
 *
 * How many records the mirror writes in one transaction.
 */
#define VENTURE_MARKETDATA_MIRROR_BATCH (50)

/**
 * VENTURE_MARKETDATA_MIRROR_MAX_LISTINGS:
 *
 * The most mirrored listings one pass reads: the source's open ones, and
 * again the closed ones whose ledger rows a vanished one might share.
 */
#define VENTURE_MARKETDATA_MIRROR_MAX_LISTINGS (20000)

/**
 * VENTURE_MARKETDATA_MAX_NAMESPACE:
 *
 * The longest `account_namespace` a source may set, in bytes.
 */
#define VENTURE_MARKETDATA_MAX_NAMESPACE (64)

/**
 * venture_marketdata_mirror_install:
 * @context: the wiring
 *
 * Installs the save validators for a location's `external-ref` and for
 * the mirror's data_source settings, and, with SQLite, the feeds hook
 * that runs the mirror after every run. After the feeds module and the
 * alerts, called by the context.
 */
void
venture_marketdata_mirror_install(VentureContext *context);

/**
 * venture_marketdata_account_ref:
 * @data_source: the data_source record
 * @key: the account's key in its store
 *
 * The `external-ref` of the location an account of @data_source is
 * promoted to: "<namespace>:<key>", where the namespace is the source's
 * `account_namespace` setting or, when it sets none, the source's uuid --
 * so two sources describing the same characters share their locations
 * only when told to.
 *
 * Returns: (transfer full) (nullable): the reference; %NULL for an empty
 *   key or settings that do not parse
 */
gchar *
venture_marketdata_account_ref(
	VentureEntity	*data_source,
	const gchar	*key
);

/**
 * venture_marketdata_promote_account:
 * @context: the wiring
 * @organization_id: whose source it is
 * @data_source_id: the source whose store knows the account
 * @key: the account's key in that store
 * @actor: (nullable): who promotes it
 * @out_location: (out) (optional) (transfer full): the location
 * @error: (out) (optional): return location for a #GError
 *
 * Makes the `location` an account stands for, or finds it, or restores
 * it: kind from the account's kind (character, shared, guild, other),
 * inside a location for its group (a realm, a region) when it names one,
 * `external-ref` from venture_marketdata_account_ref(). When the account
 * trades on a venue the store knows, the venue is promoted too and its
 * `location-id` set to this location -- only when it has none, so an
 * existing link is never taken. An existing location is returned as it
 * is; nothing is overwritten from the store.
 *
 * Errors: CONFIG with the marketdata or feeds module off or without
 * SQLite; NOT_FOUND for another organization's source, an empty store or
 * an account the store has not seen.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_marketdata_promote_account(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  data_source_id,
	const gchar		 *key,
	const VentureActor	 *actor,
	VentureEntity		**out_location,
	GError			**error
);

/**
 * venture_marketdata_mirror_positions:
 * @context: the wiring
 * @organization_id: whose source it is
 * @data_source_id: the source to mirror
 * @out_report: (out) (optional) (transfer full): what the pass did
 * @error: (out) (optional): return location for a #GError
 *
 * One pass of the mirror for one source, on the main thread, outside any
 * transaction: promotes its accounts (setting `auto_promote_accounts`,
 * default true), makes or updates a listing for every position, and
 * closes the ones whose positions are gone, at most the source's
 * `mirror_max_writes` records. The feeds hook calls this after every
 * run; calling it by hand continues where a capped pass stopped.
 *
 * The report: {data_source_id, accounts, positions, promoted, venues_linked,
 * products_created, created, updated, reopened, closed{sold, partial,
 * expired, cancelled}, waiting, left_alone, not_mirrored, failed,
 * over_cap, notes[]}.
 *
 * Errors: CONFLICT inside an automation handler or a transaction; CONFIG
 * with the market, marketdata or feeds module off or without SQLite;
 * NOT_FOUND for another organization's source. A source that switched
 * the mirror off (`mirror_positions: false`) or has stored nothing yet is
 * a report with a note, not an error.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_marketdata_mirror_positions(
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  data_source_id,
	JsonNode	**out_report,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_MARKETDATA_MIRROR_H */
