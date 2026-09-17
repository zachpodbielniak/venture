/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LEAD_ROUTING_H
#define VENTURE_LEAD_ROUTING_H
G_BEGIN_DECLS
/**
 * venture_lead_conditions_validate:
 * @conditions: (nullable): rule text, one =field=value= or =field~pattern= per line or semicolon
 * @error: (out) (optional): error
 *
 * Checks the syntax of a rule's conditions, including that every pattern
 * compiles, so a malformed rule is refused when written rather than when a
 * lead arrives.
 * Returns: whether the text is a valid condition list
 */
gboolean venture_lead_conditions_validate(const gchar *conditions, GError **error);
/**
 * venture_lead_conditions_match:
 * @database: repository used to resolve referenced records by name
 * @lead: the lead under evaluation, saved or not
 * @conditions: (nullable): condition text as stored on a rule
 * @matched: (out): whether every condition holds; empty text always matches
 * @error: (out) (optional): error
 *
 * Fields are lead properties in column spelling (=source=, =company_name=,
 * =campaign_id=, =status=), references by their target's name (=campaign=,
 * =venture=) or custom attributes (=country=, =company_size=). Equality is
 * case-insensitive; patterns are case-insensitive regular expressions.
 * Returns: whether evaluation succeeded
 */
gboolean venture_lead_conditions_match(VentureDatabase *database, VentureEntity *lead,
	const gchar *conditions, gboolean *matched, GError **error);
/**
 * venture_lead_routing_validate_rule:
 * @database: the repository
 * @rule: a routing or scoring rule being written
 * @error: (out) (optional): error
 *
 * Refuses malformed conditions and actions without their target.
 * Returns: whether the rule may be stored
 */
gboolean venture_lead_routing_validate_rule(VentureDatabase *database, VentureEntity *rule, GError **error);
/**
 * venture_lead_routing_apply:
 * @database: the repository, inside the caller's transaction
 * @lead: the lead to route; owner, venture and routing reference are updated in place
 * @rule_name: (out) (transfer full) (nullable): the matched rule's name, or %NULL
 * @configured: (out): whether the organization has any active routing rule
 * @error: (out) (optional): error
 *
 * Evaluates active routing rules in position order; the first match acts.
 * Round-robin advances the rule's persisted cursor over the team's active
 * members. No match clears the routing reference and leaves the lead as is.
 * Returns: whether evaluation succeeded
 */
gboolean venture_lead_routing_apply(VentureDatabase *database, VentureEntity *lead,
	gchar **rule_name, gboolean *configured, GError **error);
/**
 * venture_lead_scoring_compute:
 * @database: the repository
 * @lead: the lead to score
 * @score: (out): the sum of points of matching active scoring rules
 * @rule_ids: (out) (transfer full): comma-separated ids of the rules that fired
 * @error: (out) (optional): error
 * Returns: whether evaluation succeeded
 */
gboolean venture_lead_scoring_compute(VentureDatabase *database, VentureEntity *lead,
	gint64 *score, gchar **rule_ids, GError **error);
G_END_DECLS
#endif
