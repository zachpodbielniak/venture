/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LEAD_RECORDS_H
#define VENTURE_LEAD_RECORDS_H
G_BEGIN_DECLS

/**
 * VentureLeadStatus:
 * @VENTURE_LEAD_NEW: awaiting work
 * @VENTURE_LEAD_WORKING: being worked
 * @VENTURE_LEAD_QUALIFIED: ready to convert
 * @VENTURE_LEAD_UNQUALIFIED: unsuitable, with a reason
 * @VENTURE_LEAD_CONVERTED: linked to CRM records
 * @VENTURE_LEAD_RECYCLED: return to nurturing until a date
 */
typedef enum {
	VENTURE_LEAD_NEW, VENTURE_LEAD_WORKING, VENTURE_LEAD_QUALIFIED,
	VENTURE_LEAD_UNQUALIFIED, VENTURE_LEAD_CONVERTED, VENTURE_LEAD_RECYCLED
} VentureLeadStatus;
/**
 * venture_lead_status_get_type:
 * Returns: the qualification enum type
 */
GType venture_lead_status_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_LEAD (venture_lead_get_type())
VENTURE_DECLARE_ENTITY(VentureLead, venture_lead, LEAD)
#define VENTURE_TYPE_LEAD_FORM (venture_lead_form_get_type())
VENTURE_DECLARE_ENTITY(VentureLeadForm, venture_lead_form, LEAD_FORM)
#define VENTURE_TYPE_LEAD_ASSIGNMENT_RULE (venture_lead_assignment_rule_get_type())
VENTURE_DECLARE_ENTITY(VentureLeadAssignmentRule, venture_lead_assignment_rule, LEAD_ASSIGNMENT_RULE)
/**
 * venture_lead_new:
 * Returns: (transfer full): an unsaved inquiry
 */
/**
 * venture_lead_form_new:
 * Returns: (transfer full): an unsaved public capture form
 */
/**
 * venture_lead_assignment_rule_new:
 * Returns: (transfer full): an unsaved assignment rule
 */
G_END_DECLS
#endif
