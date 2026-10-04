/*
 * venture-marketdata-alerts.h - Alert rules over market data, and their hits
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An alert rule says what somebody wants to hear about a data source's
 * series store; an alert hit is one time it fired. The work is split the
 * way the feeds module splits everything:
 *
 * - on the main thread, every enabled rule of a source's organization is
 *   frozen into plain data with the source -- the instrument keys its
 *   scope comes to, the venue, the threshold in minor units, the open
 *   listings an undercut rule compares -- whenever the feeds service
 *   freezes sources (a rule, watchlist, listing or venue saved queues it);
 * - on the feeds worker, after each unit's batch is committed, the frozen
 *   rules are evaluated against the store's writer handle: candidates, as
 *   plain JSON on the run, and nothing else -- the worker never touches
 *   the database;
 * - back on the main thread, after the run record is written and outside
 *   any transaction, each candidate is held to its rule's cooldown and a
 *   per-run cap, written as an alert_hit by the system, and told to the
 *   rule's recipient's inbox. The hit's own creation is what webhooks
 *   (`alert_hit.created`) and automation (`on_created`) hear; there is no
 *   separate alert event.
 *
 * The evaluation is one function of a store handle and a frozen rule, so
 * the `evaluate` action (a rule tested now, from a read handle on the main
 * thread) and the worker agree by construction.
 */

#ifndef VENTURE_MARKETDATA_ALERTS_H
#define VENTURE_MARKETDATA_ALERTS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_ALERTS_HOOK:
 *
 * The name the alerts register their feeds hook under, and the name of
 * the payload they attach to a run.
 */
#define VENTURE_ALERTS_HOOK "alerts"

/**
 * VENTURE_ALERTS_MAX_HITS_PER_RUN:
 *
 * The most alert hits one feed run (or one evaluation) writes. Past it the
 * rest are counted and the run's notes say how many: a rule written too
 * loosely on its first sync would otherwise fill an inbox and fire a
 * webhook per instrument the store holds.
 */
#define VENTURE_ALERTS_MAX_HITS_PER_RUN (100)

/**
 * VENTURE_ALERTS_MAX_CANDIDATES:
 *
 * The most candidates the worker hands back from one run. Larger than the
 * hit cap because the cooldown, judged on the main thread, drops some.
 */
#define VENTURE_ALERTS_MAX_CANDIDATES (1000)

/**
 * VENTURE_ALERTS_MAX_RULES:
 *
 * The most rules frozen with one source; the oldest first.
 */
#define VENTURE_ALERTS_MAX_RULES (500)

/**
 * VENTURE_ALERTS_MAX_LISTINGS:
 *
 * The most open listings frozen with one source for undercut rules, read
 * newest first: past it the oldest are the ones left out, and the run's
 * notes (or the evaluation's) say the bound was reached. An operator
 * mirroring a game's auction house holds thousands of positions; the
 * listing just posted is the one most likely to be undercut.
 */
#define VENTURE_ALERTS_MAX_LISTINGS (20000)

/**
 * VENTURE_ALERTS_MAX_ACCOUNT_ROWS:
 *
 * The most positions, and separately the most inbound rows, one
 * evaluation reads from a store for the account kinds. Past it the rest
 * are not judged and the notes say so.
 */
#define VENTURE_ALERTS_MAX_ACCOUNT_ROWS (100000)

/**
 * VENTURE_ALERTS_MAX_EXPIRING_HOURS:
 *
 * The furthest ahead a position_expiring or inbound_expiring rule looks:
 * thirty days, the longest a game keeps a mail.
 */
#define VENTURE_ALERTS_MAX_EXPIRING_HOURS (720)

/**
 * VENTURE_ALERTS_MAX_STALE_DAYS:
 *
 * The longest an account_stale rule may wait: a year.
 */
#define VENTURE_ALERTS_MAX_STALE_DAYS (365)

/**
 * VENTURE_ALERTS_MAX_ROWS:
 *
 * The most store rows one rule examines at one venue per commit -- a
 * category scope over a large store.
 */
#define VENTURE_ALERTS_MAX_ROWS (5000)

/**
 * VENTURE_ALERTS_ENTRY_WINDOW:
 *
 * How far back, in seconds, an evaluation asked for by hand looks for
 * entries: a day. A feed run looks at what it brought.
 */
#define VENTURE_ALERTS_ENTRY_WINDOW (86400)

/**
 * VENTURE_ALERTS_MAX_PATTERN:
 *
 * The longest entry_match pattern, in characters. A pattern is plain text
 * matched as a substring ignoring case -- never a regular expression, so
 * nobody's rule can make the worker backtrack for an hour.
 */
#define VENTURE_ALERTS_MAX_PATTERN (200)

/**
 * VENTURE_ALERTS_MAX_WINDOW_HOURS:
 *
 * The longest spike window: the hourly history the store keeps by default
 * (14 days).
 */
#define VENTURE_ALERTS_MAX_WINDOW_HOURS (336)

/**
 * VENTURE_ALERTS_MAX_COOLDOWN_MINUTES:
 *
 * The longest cooldown: thirty days.
 */
#define VENTURE_ALERTS_MAX_COOLDOWN_MINUTES (43200)

/**
 * venture_marketdata_alerts_install:
 * @context: the wiring
 *
 * The alert_rule and alert_hit save validators and the `evaluate` action,
 * once per database; and this context's feeds hook and the listeners that
 * refreeze sources when a rule, a watchlist or its entries, a venue, a
 * listing or a category changes. Called by venture_context_new() after
 * the feeds module is installed.
 */
void
venture_marketdata_alerts_install(VentureContext *context);

/**
 * venture_marketdata_alerts_evaluate:
 * @context: the wiring
 * @rule: an alert_rule record
 * @record: %TRUE to write what fires as hits, under the cooldown and the
 *   cap, exactly as a feed run would; %FALSE to only say what would
 * @out_report: (out) (optional) (transfer full): what happened, as JSON:
 *   `{"candidates", "written", "cooled", "over_cap", "sources",
 *   "matches": [{"message", "venue_key", "instrument_key", ...}],
 *   "hits": [ids], "notes": [...]}`
 * @error: (out) (optional): return location for a #GError
 *
 * Evaluates one rule now, on the main thread, against the newest snapshot
 * of every venue in the store of each data source it covers (the rule's
 * own, else every source of its organization), and the entries that
 * arrived in the last %VENTURE_ALERTS_ENTRY_WINDOW seconds. The same
 * function a feed run uses, over a read handle instead of the writer, so
 * "test this rule" answers what the next run would. A disabled rule is
 * evaluated too -- trying one before switching it on is the point.
 *
 * Refused inside an automation handler: a hit written there raises no
 * on_created (the cascade guard), so a rule evaluated from a rule would
 * fire nothing anybody hears. Refused while the marketdata or feeds module
 * is off.
 *
 * Returns: %TRUE on success, including when nothing fired
 */
gboolean
venture_marketdata_alerts_evaluate(
	VentureContext	 *context,
	VentureEntity	 *rule,
	gboolean	  record,
	JsonNode	**out_report,
	GError		**error
);

/**
 * venture_marketdata_alerts_evaluate_at:
 * @context: the wiring
 * @rule: an alert_rule record
 * @record: %TRUE to write what fires as hits; %FALSE to only say what would
 * @now: the moment to judge at, Unix seconds: what "expires within",
 *   "not seen for" and "already expired" are measured from, and the
 *   time a hit about an account is observed at
 * @out_report: (out) (optional) (transfer full): as for
 *   venture_marketdata_alerts_evaluate()
 * @error: (out) (optional): return location for a #GError
 *
 * venture_marketdata_alerts_evaluate() at a moment of the caller's
 * choosing rather than the wall clock -- what a test needs to put an
 * expiry exactly on a rule's boundary. The market kinds judge each
 * venue's newest snapshot whatever @now is; @now only moves the account
 * kinds and the entries' look-back.
 *
 * Returns: %TRUE on success, including when nothing fired
 */
gboolean
venture_marketdata_alerts_evaluate_at(
	VentureContext	 *context,
	VentureEntity	 *rule,
	gboolean	  record,
	gint64		  now,
	JsonNode	**out_report,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_MARKETDATA_ALERTS_H */
