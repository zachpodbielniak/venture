/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SEQUENCE_JOURNEYS_H
#define VENTURE_SEQUENCE_JOURNEYS_H
G_BEGIN_DECLS

/* Journeys (docs/journeys.org) are sequences: branch steps, jumps, a
 * channel hook, named triggers and an email gate, all inside the one
 * sequence engine. */

/**
 * VENTURE_SEQUENCE_QUOTE_DAYS:
 *
 * How long a sent quote waits unaccepted before its trigger fires, when
 * the trigger names no number of days.
 */
#define VENTURE_SEQUENCE_QUOTE_DAYS 7

/**
 * VENTURE_SEQUENCE_TRIGGER_PAGE:
 *
 * The most business events one trigger reads in one sweep.
 */
#define VENTURE_SEQUENCE_TRIGGER_PAGE 500

/**
 * VentureSequenceChannelExecute:
 * @database: repository, inside the step's transaction
 * @delivery: the unsaved delivery; set its state, error and external id
 * @contact: the enrolled contact
 * @step: the step being run
 * @as_of: the sweep's clock
 * @data: registration data
 * @error: (out) (optional): why the step failed
 *
 * What a channel does. Synchronous, on the main thread, no network in the
 * transaction: a sending channel queues and lets its own adapter send.
 *
 * Returns: FALSE to fail the step with @error's message
 */
typedef gboolean (*VentureSequenceChannelExecute)(VentureDatabase *database, VentureEntity *delivery,
	VentureEntity *contact, VentureEntity *step, GDateTime *as_of, gpointer data, GError **error);
/**
 * venture_sequence_channel_register:
 * @channel: a #VentureSequenceChannel value
 * @execute: what it does
 * @data: (nullable): passed to @execute
 *
 * Replaces the built-in behaviour of a non-email, non-branch channel, or
 * gives a new channel value its behaviour. A later registration for the
 * same channel replaces the earlier one.
 */
void venture_sequence_channel_register(gint channel, VentureSequenceChannelExecute execute, gpointer data);
/**
 * venture_sequence_channel_unregister:
 * @channel: a #VentureSequenceChannel value
 */
void venture_sequence_channel_unregister(gint channel);

/**
 * VentureSequenceEmailGate:
 * @database: repository
 * @sequence: the journey
 * @contact: the recipient
 * @as_of: the sweep's clock
 * @skip_reason: (out) (transfer full): why the email is skipped
 * @data: registration data
 * @error: (out) (optional): persistence failure
 *
 * Asked before an email step is rendered. Marketing installs the one that
 * honours the preference centre.
 *
 * Returns: FALSE only on failure; skipping sets @skip_reason
 */
typedef gboolean (*VentureSequenceEmailGate)(VentureDatabase *database, VentureEntity *sequence,
	VentureEntity *contact, GDateTime *as_of, gchar **skip_reason, gpointer data, GError **error);
/**
 * venture_sequence_add_email_gate:
 * @database: repository
 * @gate: the gate
 * @data: (nullable): passed to @gate
 */
void venture_sequence_add_email_gate(VentureDatabase *database, VentureSequenceEmailGate gate, gpointer data);

/**
 * venture_sequence_service_run_triggers:
 * @self: the service
 * @organization_id: exactly one organization
 * @now: the sweep's clock
 * @actor: (nullable): audit actor
 * @error: (out) (optional): persistence failure
 *
 * Reads the business-event feed for each active trigger after its own
 * position and enrols the contacts named, at most once per event per
 * journey. venture_sequence_service_run_due() calls it first.
 *
 * Returns: enrollments made, or -1
 */
gint venture_sequence_service_run_triggers(VentureSequenceService *self, gint64 organization_id,
	GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_sequence_service_record_open_full:
 * @self: the service
 * @token: the delivery's tracking token
 * @user_agent: (nullable): the request's User-Agent; judged, never stored
 * @now: when the pixel was fetched
 * @actor: (nullable): audit actor
 * @error: (out) (optional): unknown token or persistence failure
 *
 * venture_sequence_service_record_open() with the machine-open judgement:
 * an open judged a machine's is kept with its reason and takes no branch.
 *
 * Returns: TRUE when the token is known and the open committed
 */
gboolean venture_sequence_service_record_open_full(VentureSequenceService *self, const gchar *token,
	const gchar *user_agent, GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_sequence_service_record_click_full:
 * @self: the service
 * @token: the delivery's tracking token
 * @position: link number, from one
 * @user_agent: (nullable): the request's User-Agent; judged, never stored
 * @now: when the link was followed
 * @actor: (nullable): audit actor
 * @error: (out) (optional): unknown token or link, or persistence failure
 * Returns: (transfer full) (nullable): the original URL
 */
gchar *venture_sequence_service_record_click_full(VentureSequenceService *self, const gchar *token,
	gint64 position, const gchar *user_agent, GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_sequence_journeys_install:
 * @database: repository
 *
 * Installs the step validator and the tag and form triggers' hooks.
 */
void venture_sequence_journeys_install(VentureDatabase *database);

G_END_DECLS
#endif
