/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LIGHTSITE_DEPARTURE_H
#define VENTURE_LIGHTSITE_DEPARTURE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

/**
 * VentureDepartureAction:
 * @VENTURE_DEPARTURE_ACTION_LEAVE: the owner chose to leave
 * @VENTURE_DEPARTURE_ACTION_CLOSE: the leaving steps are done
 * @VENTURE_DEPARTURE_ACTION_RETURN: the owner came back
 * @VENTURE_DEPARTURE_ACTION_PURGE: delete everything for good
 */
typedef enum
{
	VENTURE_DEPARTURE_ACTION_LEAVE,
	VENTURE_DEPARTURE_ACTION_CLOSE,
	VENTURE_DEPARTURE_ACTION_RETURN,
	VENTURE_DEPARTURE_ACTION_PURGE
} VentureDepartureAction;

/**
 * VentureDepartureOutcome:
 * @VENTURE_DEPARTURE_OUTCOME_CHANGED: the state moves (HTTP 200)
 * @VENTURE_DEPARTURE_OUTCOME_UNCHANGED: a repeat; nothing moves (HTTP 200)
 * @VENTURE_DEPARTURE_OUTCOME_CONFLICT: the wrong state (HTTP 409)
 * @VENTURE_DEPARTURE_OUTCOME_GONE: a return that can no longer happen:
 *   purged, or past six calendar months from left_at (HTTP 410)
 */
typedef enum
{
	VENTURE_DEPARTURE_OUTCOME_CHANGED,
	VENTURE_DEPARTURE_OUTCOME_UNCHANGED,
	VENTURE_DEPARTURE_OUTCOME_CONFLICT,
	VENTURE_DEPARTURE_OUTCOME_GONE
} VentureDepartureOutcome;

/**
 * venture_departure_action_from_string:
 * @name: (nullable): "leave", "close", "return" or "purge", exactly
 * @action: (out): the action
 *
 * Returns: whether @name is one of the four
 */
gboolean venture_departure_action_from_string(const gchar *name, VentureDepartureAction *action);

/**
 * venture_departure_decide:
 * @state: the business's state now
 * @action: what Lightsite asks for
 * @left_at: (nullable): when the business closed
 * @now: the time of the call
 * @next: (out): the state after the call; @state unless the outcome is
 *   %VENTURE_DEPARTURE_OUTCOME_CHANGED
 *
 * The whole leaving rule, with no storage: which move each action makes
 * from each state, and the window a closed business may come back in,
 * which ends six calendar months after @left_at (a later day of the
 * month than the target month has is its last day).
 *
 * Returns: the outcome
 */
VentureDepartureOutcome venture_departure_decide(VentureDepartureState state, VentureDepartureAction action,
	GDateTime *left_at, GDateTime *now, VentureDepartureState *next);

/**
 * venture_lightsite_departure_read:
 * @database: the database
 * @organization_id: the business
 * @error: (out) (optional): %VENTURE_ERROR_PERMISSION_DENIED unless the
 *   caller is the trusted service, %VENTURE_ERROR_NOT_FOUND for no such
 *   organization
 *
 * Returns: (transfer full) (nullable): {"organization_id", "state",
 *   "left_at", "returned_at", "purged_at"}, times ISO 8601 or null
 */
JsonNode *venture_lightsite_departure_read(VentureDatabase *database, gint64 organization_id, GError **error);

/**
 * venture_lightsite_departure_act:
 * @database: the database
 * @organization_id: the business
 * @action: what Lightsite asks for
 * @actor: (nullable): who, for the audit
 * @gone: (out) (optional): %TRUE when the refusal is a 410, not a 409
 * @error: (out) (optional): as venture_lightsite_departure_read(), and
 *   %VENTURE_ERROR_CONFLICT for the wrong state
 *
 * Applies venture_departure_decide() and stores the change, setting
 * left_at on close, returned_at on return and purged_at on purge. A repeat
 * stores nothing.
 *
 * Returns: (transfer full) (nullable): the state after, as the read
 */
JsonNode *venture_lightsite_departure_act(VentureDatabase *database, gint64 organization_id,
	VentureDepartureAction action, const VentureActor *actor, gboolean *gone, GError **error);

G_END_DECLS
#endif
