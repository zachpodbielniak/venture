/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LEAD_ROUTING_RECORDS_H
#define VENTURE_LEAD_ROUTING_RECORDS_H
G_BEGIN_DECLS

/**
 * VentureLeadRoutingAction:
 * @VENTURE_LEAD_ROUTING_ASSIGN_USER: assign the lead to the rule's username
 * @VENTURE_LEAD_ROUTING_ROUND_ROBIN: rotate over the active members of the rule's team
 * @VENTURE_LEAD_ROUTING_ASSIGN_VENTURE: place the lead under the rule's venture
 */
typedef enum {
	VENTURE_LEAD_ROUTING_ASSIGN_USER, VENTURE_LEAD_ROUTING_ROUND_ROBIN,
	VENTURE_LEAD_ROUTING_ASSIGN_VENTURE
} VentureLeadRoutingAction;
/**
 * venture_lead_routing_action_get_type:
 * Returns: the routing action enum type
 */
GType venture_lead_routing_action_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_LEAD_ROUTING_RULE (venture_lead_routing_rule_get_type())
VENTURE_DECLARE_ENTITY(VentureLeadRoutingRule, venture_lead_routing_rule, LEAD_ROUTING_RULE)
#define VENTURE_TYPE_LEAD_SCORING_RULE (venture_lead_scoring_rule_get_type())
VENTURE_DECLARE_ENTITY(VentureLeadScoringRule, venture_lead_scoring_rule, LEAD_SCORING_RULE)
#define VENTURE_TYPE_LEAD_SCORE_HISTORY (venture_lead_score_history_get_type())
VENTURE_DECLARE_ENTITY(VentureLeadScoreHistory, venture_lead_score_history, LEAD_SCORE_HISTORY)
/**
 * venture_lead_routing_rule_new:
 * Returns: (transfer full): an unsaved routing rule
 */
/**
 * venture_lead_scoring_rule_new:
 * Returns: (transfer full): an unsaved scoring rule
 */
/**
 * venture_lead_score_history_new:
 * Returns: (transfer full): an unsaved score history row; only the lead service persists them
 */
G_END_DECLS
#endif
