/*
 * venture-external-runs.h - Coding work done somewhere else, on the record
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * VENTURE's own runners record every attempt at a ticket as a forge_run:
 * what was asked, what came back, the branch and the pull request, what it
 * cost. Work done by an agent fleet outside VENTURE was invisible to all of
 * that -- the Runs page, the delivery report, and above all the agent
 * budgets, which sum the runs' own cost. An external run is the same
 * record, written by the outside runner through two calls: one when it
 * starts, which the budgets may refuse, and reports as it goes and when it
 * finishes. Generic writes to forge_run stay refused: a run record is
 * evidence, and these two calls are the only way in for an outside runner.
 */

#ifndef VENTURE_EXTERNAL_RUNS_H
#define VENTURE_EXTERNAL_RUNS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_external_run_start:
 * @context: the wiring
 * @ticket: the ticket the work is for
 * @fields: (nullable): `provider`, `model`, `branch`, `prompt`
 * @actor: (nullable): who is starting it
 * @error: (out) (optional): return location for a #GError
 *
 * Starts an external run on @ticket: a forge_run with runner `external`,
 * state `running`. The agent budgets covering the ticket's repository are
 * asked first, as for VENTURE's own runners: an exhausted hard-stop budget
 * refuses with %VENTURE_ERROR_CONFLICT (409), naming it, and a budget
 * crossing its warning line tells the owners once.
 *
 * Since: 0.10.0
 *
 * Returns: (transfer full) (nullable): the run, or %NULL with @error
 */
VentureEntity *
venture_external_run_start(
	VentureContext		 *context,
	VentureEntity		 *ticket,
	JsonObject		 *fields,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_external_run_report:
 * @context: the wiring
 * @run: an external run that has not finished
 * @fields: what changed: `state` (`running`, `succeeded`, `failed`,
 *   `cancelled`), `branch`, `pull_request_number`, `provider`, `model`,
 *   `input_tokens`, `output_tokens`, `turns`, `cost` (an amount with its
 *   currency, e.g. "0.42 USD"), `summary`, `failure_reason`, `log`
 * @actor: (nullable): who is reporting
 * @error: (out) (optional): return location for a #GError
 *
 * Records progress or the end of an external run. A finished state stamps
 * finished-at, after which the run takes no more reports; a run VENTURE's
 * own runners made is refused (%VENTURE_ERROR_PERMISSION_DENIED). Once it
 * has a cost the budgets are asked again, so crossing a line is announced
 * as it would be for an internal run.
 *
 * Since: 0.10.0
 *
 * Returns: %TRUE when recorded
 */
gboolean
venture_external_run_report(
	VentureContext		 *context,
	VentureEntity		 *run,
	JsonObject		 *fields,
	const VentureActor	 *actor,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_EXTERNAL_RUNS_H */
