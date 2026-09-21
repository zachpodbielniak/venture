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

GType
venture_call_direction_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_CALL_DIRECTION_OUTBOUND, "VENTURE_CALL_DIRECTION_OUTBOUND", "outbound" },
			{ VENTURE_CALL_DIRECTION_INBOUND, "VENTURE_CALL_DIRECTION_INBOUND", "inbound" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureCallDirection", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

GType
venture_call_outcome_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_CALL_OUTCOME_UNKNOWN, "VENTURE_CALL_OUTCOME_UNKNOWN", "unknown" },
			{ VENTURE_CALL_OUTCOME_REACHED, "VENTURE_CALL_OUTCOME_REACHED", "reached" },
			{ VENTURE_CALL_OUTCOME_VOICEMAIL, "VENTURE_CALL_OUTCOME_VOICEMAIL", "voicemail" },
			{ VENTURE_CALL_OUTCOME_NO_ANSWER, "VENTURE_CALL_OUTCOME_NO_ANSWER", "no_answer" },
			{ VENTURE_CALL_OUTCOME_CALLBACK, "VENTURE_CALL_OUTCOME_CALLBACK", "callback" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureCallOutcome", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

static const VentureFieldDecl activity_fields[] = {
	VENTURE_FIELD_NAME("subject", "Subject", "The next action"),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, venture_activity_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("body", "Body", NULL),
	VENTURE_FIELD("owner", "Owner", "Username", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME),
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
	VENTURE_FIELD_ENUM("recurrence", "Repeat", NULL, venture_activity_recurrence_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("lead-id", "Lead", NULL, "lead", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("call-direction", "Call direction", NULL, venture_call_direction_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("call-duration", "Call duration", "Actual seconds, zero to 86400", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("call-occurred-at", "Call occurred", "Actual occurrence, not the planned due time", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("call-outcome", "Call result", "Structured result; outcome retains narrative notes", venture_call_outcome_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("call-recording-url", "Recording URL", "Stable HTTPS address without credentials, query or fragment; never fetched", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("call-transcript", "Transcript", "Optional operator-provided text, up to 65536 characters"),
	VENTURE_FIELD("call-external-source", "Call source", "Versioned authenticated adapter namespace; paired with external identity", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("call-external-id", "External call identity", "Stable within organization and source", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("call-external-key", "Call identity digest", "Organization-scoped replay constraint", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("call-request-hash", "Call request digest", "Replay evidence", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("call-followup-subject", "Followup subject", "Defaults to Follow up plus call subject", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("call-followup-owner", "Followup owner", "Defaults to call owner", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("call-followup-due-at", "Followup due", "Optional; creates one planned followup", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("call-followup-remind-at", "Followup reminder", "Defaults to followup due time", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("call-interaction-id", "Call history", "Service-owned historical interaction", "interaction", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("call-followup-id", "Scheduled followup", "Service-owned planned activity", "activity", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureActivity, venture_activity, activity_fields)

static const VentureFieldDecl activity_type_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, venture_activity_kind_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("default-duration", "Default duration", "Minutes", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureActivityType, venture_activity_type, activity_type_fields)
