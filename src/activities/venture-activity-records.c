/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_activity_kind_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_ACTIVITY_KIND_TASK, "VENTURE_ACTIVITY_KIND_TASK", "task" },
			{ VENTURE_ACTIVITY_KIND_CALL, "VENTURE_ACTIVITY_KIND_CALL", "call" },
			{ VENTURE_ACTIVITY_KIND_MEETING, "VENTURE_ACTIVITY_KIND_MEETING", "meeting" },
			{ VENTURE_ACTIVITY_KIND_EMAIL, "VENTURE_ACTIVITY_KIND_EMAIL", "email" },
			{ VENTURE_ACTIVITY_KIND_FOLLOWUP, "VENTURE_ACTIVITY_KIND_FOLLOWUP", "followup" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureActivityKind", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

GType
venture_activity_status_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_ACTIVITY_STATUS_PLANNED, "VENTURE_ACTIVITY_STATUS_PLANNED", "planned" },
			{ VENTURE_ACTIVITY_STATUS_DONE, "VENTURE_ACTIVITY_STATUS_DONE", "done" },
			{ VENTURE_ACTIVITY_STATUS_CANCELLED, "VENTURE_ACTIVITY_STATUS_CANCELLED", "cancelled" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureActivityStatus", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

GType
venture_activity_recurrence_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_ACTIVITY_RECURRENCE_NONE, "VENTURE_ACTIVITY_RECURRENCE_NONE", "none" },
			{ VENTURE_ACTIVITY_RECURRENCE_DAILY, "VENTURE_ACTIVITY_RECURRENCE_DAILY", "daily" },
			{ VENTURE_ACTIVITY_RECURRENCE_WEEKLY, "VENTURE_ACTIVITY_RECURRENCE_WEEKLY", "weekly" },
			{ VENTURE_ACTIVITY_RECURRENCE_MONTHLY, "VENTURE_ACTIVITY_RECURRENCE_MONTHLY", "monthly" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureActivityRecurrence", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

static const VentureFieldDecl activity_fields[] = {
	VENTURE_FIELD_NAME("subject", "Subject", "The next action"),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, venture_activity_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("body", "Body", NULL),
	VENTURE_FIELD("owner", "Owner", "Username", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("due-at", "Due", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("starts-at", "Starts", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("ends-at", "Ends", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("priority", "Priority", NULL, venture_priority_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("status", "Status", "Complete through VentureActivityService", venture_activity_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("completed-at", "Completed", "Stamped by the service", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("outcome", "Outcome", "What happened"),
	VENTURE_FIELD("related-type", "Related type", "Registered company, contact, deal, lead, ticket or invoice", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("related-id", "Related record", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("company-id", "Company", NULL, "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("deal-id", "Deal", NULL, "deal", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("remind-at", "Remind", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("reminded-at", "Reminder delivered", "Stamped by the sweep", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("recurrence", "Repeat", NULL, venture_activity_recurrence_get_type, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureActivity, venture_activity, activity_fields)

static const VentureFieldDecl activity_type_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, venture_activity_kind_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("default-duration", "Default duration", "Minutes", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureActivityType, venture_activity_type, activity_type_fields)
