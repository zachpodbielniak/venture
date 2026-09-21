/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_lead_routing_action_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_LEAD_ROUTING_ASSIGN_USER, "VENTURE_LEAD_ROUTING_ASSIGN_USER", "assign_user" },
			{ VENTURE_LEAD_ROUTING_ROUND_ROBIN, "VENTURE_LEAD_ROUTING_ROUND_ROBIN", "round_robin" },
			{ VENTURE_LEAD_ROUTING_ASSIGN_VENTURE, "VENTURE_LEAD_ROUTING_ASSIGN_VENTURE", "assign_venture" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureLeadRoutingAction", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

/* Conditions are one "field=value" or "field~pattern" per line (or per
 * semicolon); all must hold. An empty text matches every lead. */
static const VentureFieldDecl lead_routing_rule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. Web inquiries to the sales rota"),
	VENTURE_FIELD("position", "Position", "Rules are tried in ascending order", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("conditions", "Conditions", "field=value or field~pattern, one per line", VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("action", "Action", NULL, venture_lead_routing_action_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("assign-to", "Assign to", "Username for assign_user", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("team-id", "Team", "Rota for round_robin", "team", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture", "Target for assign_venture", "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("territory-id", "Territory", "Optional territory and owning team; existing rule position decides precedence", "sales_territory", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cursor", "Cursor", "Persisted round-robin position", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureLeadRoutingRule, venture_lead_routing_rule, lead_routing_rule_fields)

static const VentureFieldDecl lead_scoring_rule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. Enterprise company size"),
	VENTURE_FIELD("conditions", "Conditions", "field=value or field~pattern, one per line", VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("points", "Points", "Added to the score when the conditions hold; may be negative", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureLeadScoringRule, venture_lead_scoring_rule, lead_scoring_rule_fields)

/* One row per score change, written only by the lead service. */
static const VentureFieldDecl lead_score_history_fields[] = {
	VENTURE_FIELD_REF("lead-id", "Lead", NULL, "lead", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("previous-score", "Previous score", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("score", "Score", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("rule-ids", "Rules fired", "Comma-separated scoring rule ids", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("manual", "Manual", "Set by an operator rather than the formula", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("scored-at", "Scored at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureLeadScoreHistory, venture_lead_score_history, lead_score_history_fields)
