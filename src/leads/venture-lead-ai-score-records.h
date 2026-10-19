/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LEAD_AI_SCORE_RECORDS_H
#define VENTURE_LEAD_AI_SCORE_RECORDS_H
G_BEGIN_DECLS
#define VENTURE_TYPE_LEAD_SCORE_PROPOSAL (venture_lead_score_proposal_get_type())
VENTURE_DECLARE_ENTITY(VentureLeadScoreProposal, venture_lead_score_proposal, LEAD_SCORE_PROPOSAL)
/**
 * venture_lead_score_proposal_new:
 * Returns: (transfer full): an unsaved AI score proposal; only the lead AI
 *   scoring service persists them
 */
G_END_DECLS
#endif
