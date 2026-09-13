/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_lead_status_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_LEAD_NEW, "VENTURE_LEAD_NEW", "new" },
			{ VENTURE_LEAD_WORKING, "VENTURE_LEAD_WORKING", "working" },
			{ VENTURE_LEAD_QUALIFIED, "VENTURE_LEAD_QUALIFIED", "qualified" },
			{ VENTURE_LEAD_UNQUALIFIED, "VENTURE_LEAD_UNQUALIFIED", "unqualified" },
			{ VENTURE_LEAD_CONVERTED, "VENTURE_LEAD_CONVERTED", "converted" },
			{ VENTURE_LEAD_RECYCLED, "VENTURE_LEAD_RECYCLED", "recycled" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureLeadStatus", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

static const VentureFieldDecl lead_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("company-name", "Company name", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("email", "Email", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("phone", "Phone", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("website", "Website", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("source", "Source", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("owner", "Owner", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("campaign-id", "Campaign", NULL, "campaign", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL, venture_lead_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("score", "Score", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("unqualified-reason", "Unqualified reason", NULL, VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("converted-company-id", "Converted company", NULL, "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("converted-contact-id", "Converted contact", NULL, "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("converted-deal-id", "Converted deal", NULL, "deal", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("first-seen-at", "First seen at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-activity-at", "Last activity at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("recycle-until", "Recycle until", NULL, VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("notes", "Notes", NULL, VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureLead, venture_lead, lead_fields)

static const VentureFieldDecl lead_form_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("public-token", "Public token", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("fields", "Fields", NULL, VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("redirect-url", "Redirect url", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("honeypot", "Honeypot", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("on-duplicate", "On duplicate", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureLeadForm, venture_lead_form, lead_form_fields)

static const VentureFieldDecl lead_assignment_rule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("source", "Source", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("assignees", "Assignees", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("strategy", "Strategy", NULL, venture_routing_strategy_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cursor", "Cursor", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureLeadAssignmentRule, venture_lead_assignment_rule, lead_assignment_rule_fields)

