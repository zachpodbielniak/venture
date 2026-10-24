/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* An AI score sits beside the rule score as a proposal: it is never the
 * lead's score until a person accepts it, and dismissing it changes
 * nothing on the lead. Written only by the lead AI scoring service. */
static const VentureFieldDecl lead_score_proposal_fields[] = {
	VENTURE_FIELD_REF("lead-id", "Lead", NULL, "lead", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("score", "Proposed score", "0 to 100, from the lead's own record and activities", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("rule-score", "Rule score", "The lead's score when the proposal was made", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("reasons", "Reasons", "One reason per line, each naming the facts it rests on"),
	VENTURE_FIELD("status", "Status", "proposed, accepted or dismissed", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("proposed-at", "Proposed at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("decided-by", "Decided by", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("decided-at", "Decided at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureLeadScoreProposal, venture_lead_score_proposal, lead_score_proposal_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "AI score proposal", "AI score proposals");)
