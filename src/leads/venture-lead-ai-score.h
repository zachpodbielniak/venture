/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-lead-ai-score.h - An AI score proposal beside the scoring rules
 *
 * The rule formula stays the lead's score. The assistant may read one
 * lead's own record and its activities and propose a score from 0 to 100
 * with reasons in plain words; the proposal is stored on its own and
 * changes nothing until a person accepts it (the accepted score is then an
 * operator's score, kept by the ordinary manual mark) or dismisses it.
 * Each reason must cite the numbered facts it rests on, and a reason that
 * cites none, cites a fact that was not given or names a number the cited
 * facts do not contain is dropped before anyone sees it.
 */
#ifndef VENTURE_LEAD_AI_SCORE_H
#define VENTURE_LEAD_AI_SCORE_H
G_BEGIN_DECLS

/**
 * VENTURE_LEAD_AI_SCORE_MAX_FACTS:
 *
 * The most facts (record fields plus activities) one proposal reads.
 */
#define VENTURE_LEAD_AI_SCORE_MAX_FACTS 40

/**
 * venture_lead_ai_score_enabled:
 * @context: the wiring
 * @organization_id: the lead's organization
 *
 * Returns: whether the leads and AI modules are on, a provider is
 *   configured and the organization's ai-lead-scoring switch is set
 */
gboolean venture_lead_ai_score_enabled(VentureContext *context, gint64 organization_id);

/**
 * venture_lead_ai_score_propose:
 * @context: the wiring
 * @lead: a saved lead
 * @actor: (nullable): who asked, for the audit trail
 * @error: (out) (optional): the switch is off, nothing to read, or the
 *   model's answer could not be grounded
 *
 * Reads the lead and at most %VENTURE_LEAD_AI_SCORE_MAX_FACTS of its facts,
 * asks the configured model on the toolless path, and saves a proposal.
 * A pending proposal for the same lead is replaced (dismissed). The lead
 * itself is not written.
 *
 * Returns: (transfer full) (nullable): the saved proposal
 */
VentureEntity *venture_lead_ai_score_propose(VentureContext *context, VentureEntity *lead,
	const VentureActor *actor, GError **error);

/**
 * venture_lead_ai_score_pending:
 * @database: the database
 * @lead: a saved lead
 *
 * Returns: (transfer full) (nullable): the lead's undecided proposal
 */
VentureEntity *venture_lead_ai_score_pending(VentureDatabase *database, VentureEntity *lead);

/**
 * venture_lead_ai_score_decide:
 * @context: the wiring
 * @proposal: a pending proposal
 * @accept: %TRUE writes the proposed score on the lead through the lead
 *   service as an operator's score; %FALSE dismisses it
 * @actor: (nullable): who decided
 * @error: (out) (optional): already decided, lead converted, or switched off
 *
 * Returns: whether the decision was saved, with the lead in one transaction
 */
gboolean venture_lead_ai_score_decide(VentureContext *context, VentureEntity *proposal,
	gboolean accept, const VentureActor *actor, GError **error);

/**
 * venture_lead_ai_score_check_write:
 * @entity: a record about to be saved
 * @error: (out) (optional): refusal
 *
 * Proposals are written only by this service; the lead service's save
 * hook calls this for every other writer.
 *
 * Returns: whether the write may go ahead
 */
gboolean venture_lead_ai_score_check_write(VentureEntity *entity, GError **error);

G_END_DECLS
#endif
