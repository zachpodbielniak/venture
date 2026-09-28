/*
 * venture-sessions.h - Sessions, their yields, posting and performance
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The sessions module measures effort by its outcome. A `session` is a
 * time-boxed run -- a farming route, a market day, a study block, a shift
 * -- and its `session_yield` rows are what it produced: units of a
 * product, or an amount of money. The `post` action puts a session's
 * product yields into stock and its money yields into the holding at its
 * location, once each, in one transaction; the
 * `session_performance` report divides what each kind of run yielded by
 * the hours it took. The rules that keep the records honest are save
 * validators and a write guard installed here, so every writer obeys them.
 */

#ifndef VENTURE_SESSIONS_H
#define VENTURE_SESSIONS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_SESSIONS_MAX_MINUTES:
 *
 * The longest session, in minutes: a year. A bound, not a business rule
 * -- it catches an end date typed in the wrong year before a run of
 * 525,600 hours makes every per-hour figure nonsense.
 */
#define VENTURE_SESSIONS_MAX_MINUTES (366 * 24 * 60)

/**
 * venture_sessions_install:
 * @context: the wiring
 *
 * Installs the save validators for `session` (minutes derived from the
 * two times, an end not before the start, references in the session's
 * organization, `posted-at` written only by posting) and `session_yield`
 * (exactly one of goods or money, a posted yield's goods frozen, stock
 * that holds the yielded product), and registers the `post` action on
 * `session`. Called once by the context; a second context over the same
 * database installs nothing twice.
 */
void
venture_sessions_install(VentureContext *context);

/**
 * venture_sessions_check_write:
 * @database: the database being written
 * @entity: the record being saved or removed
 * @removal: whether this is a delete, purge or restore
 * @error: (out) (optional): return location for a #GError
 *
 * The removal half of the module's rules, in the database's subsystem
 * guard list: a posted yield, and a session with posted yields, cannot be
 * deleted -- the stock they put on the shelf would stay there with
 * nothing saying where it came from. Every other write passes.
 *
 * Returns: %TRUE when the write may proceed
 */
gboolean
venture_sessions_check_write(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	gboolean	  removal,
	GError		**error
);

/**
 * venture_sessions_post:
 * @database: the database to write
 * @session_id: the session whose yields to post
 * @actor: (nullable): audit actor
 * @out_posted: (out) (optional): how many yields this call posted
 * @out_session: (out) (optional) (transfer full): the session as it
 *   stands afterwards
 * @error: (out) (optional): return location for a #GError
 *
 * Puts every product yield of the session that has not been posted yet
 * into stock, in one transaction: each becomes a positive PRODUCTION
 * inventory transaction referenced "session:<id>", with a zero cost layer
 * (a yield was not bought), into the yield's own inventory item or the
 * one item holding the product at the session's location. Every unposted
 * money yield goes into the holding at the session's location -- debit
 * the holding, credit session income, in the yield's currency -- as a
 * journal for a posted currency or a holding movement for a memo one; a
 * session with no location, or the ledger module off, leaves its money
 * yields unposted. Each posted yield is stamped with what it made and the
 * session with the time.
 *
 * Posting is idempotent: a yield already posted is never posted again,
 * and a session with nothing new to post succeeds with @out_posted 0 and
 * writes nothing. A yield whose stock cannot be found -- none, or more
 * than one, at the location -- refuses the whole post; any refusal or
 * failure writes nothing. With the sales module off there is no stock to
 * post into, and the post is refused.
 *
 * Returns: %TRUE on success, %FALSE with @error set
 */
gboolean
venture_sessions_post(
	VentureDatabase		 *database,
	gint64			  session_id,
	const VentureActor	 *actor,
	guint			 *out_posted,
	VentureEntity		**out_session,
	GError			**error
);

/**
 * venture_sessions_performance:
 * @context: the wiring
 * @period: (nullable): bounds `started-at`
 * @options: (nullable): `group_by` (activity, category, location or
 *   venture), `category_depth`, `venture_id`, `price_source`, `as_of`,
 *   `organization_id`; see docs/sessions.org
 * @error: (out) (optional): return location for a #GError
 *
 * How each kind of run did: sessions, hours (finished sessions only; open
 * ones counted apart), units yielded, what the goods were worth, money
 * yielded directly, cost, net and net per hour -- one row per group and
 * currency, never a sum across currencies. Goods are valued at their
 * recorded unit value, else the latest price observed from
 * `price_source` (market module on) or the product's list price (off); a
 * product with no value is named, and the figures it would have changed
 * are left blank rather than read as zero.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL with @error set
 */
VentureReportResult *
venture_sessions_performance(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_sessions_register_reports:
 * @registry: the report registry
 *
 * Registers `session_performance`. It belongs to the sessions module, so
 * switching that off hides it.
 */
void
venture_sessions_register_reports(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_SESSIONS_H */
