/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
GType
venture_sequence_goal_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ 0, "SEQUENCE_GOAL_NONE", "none" },
			{ 1, "SEQUENCE_GOAL_REPLY", "reply" },
			{ 2, "SEQUENCE_GOAL_MEETING", "meeting" },
			{ 3, "SEQUENCE_GOAL_DEAL_WON", "deal_won" },
			{ 4, "SEQUENCE_GOAL_CUSTOM", "custom" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureSequenceGoal", values);
		 g_once_init_leave(&type_id, id);
	}
	return type_id;
}
GType
venture_sequence_channel_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ 0, "SEQUENCE_CHANNEL_EMAIL", "email" },
			{ 1, "SEQUENCE_CHANNEL_CALL_TASK", "call_task" },
			{ 2, "SEQUENCE_CHANNEL_SMS_TASK", "sms_task" },
			{ 3, "SEQUENCE_CHANNEL_WAIT", "wait" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureSequenceChannel", values);
		 g_once_init_leave(&type_id, id);
	}
	return type_id;
}
GType
venture_sequence_status_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ 0, "SEQUENCE_STATUS_ACTIVE", "active" },
			{ 1, "SEQUENCE_STATUS_PAUSED", "paused" },
			{ 2, "SEQUENCE_STATUS_COMPLETED", "completed" },
			{ 3, "SEQUENCE_STATUS_EXITED", "exited" },
			{ 4, "SEQUENCE_STATUS_FAILED", "failed" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureSequenceStatus", values);
		 g_once_init_leave(&type_id, id);
	}
	return type_id;
}
GType
venture_sequence_delivery_state_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ 0, "SEQUENCE_DELIVERY_STATE_PENDING", "pending" },
			{ 1, "SEQUENCE_DELIVERY_STATE_SENT", "sent" },
			{ 2, "SEQUENCE_DELIVERY_STATE_FAILED", "failed" },
			{ 3, "SEQUENCE_DELIVERY_STATE_SKIPPED", "skipped" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureSequenceDeliveryState", values);
		 g_once_init_leave(&type_id, id);
	}
	return type_id;
}
GType
venture_sequence_suppression_reason_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ 0, "SEQUENCE_SUPPRESSION_REASON_UNSUBSCRIBED", "unsubscribed" },
			{ 1, "SEQUENCE_SUPPRESSION_REASON_BOUNCED", "bounced" },
			{ 2, "SEQUENCE_SUPPRESSION_REASON_COMPLAINED", "complained" },
			{ 3, "SEQUENCE_SUPPRESSION_REASON_MANUAL", "manual" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureSequenceSuppressionReason", values);
		 g_once_init_leave(&type_id, id);
	}
	return type_id;
}
static const VentureFieldDecl sequence_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("venture-id", "Venture id", NULL, "venture", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("goal", "Goal", NULL, venture_sequence_goal_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("exit-on-reply", "Exit on reply", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("exit-on-unsubscribe", "Exit on unsubscribe", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("exit-on-deal-won", "Exit on deal won", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("timezone", "Timezone", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("send-window-start", "Send window start", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("send-window-end", "Send window end", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("weekdays", "Weekdays", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureSequence, venture_sequence, sequence_fields)
static const VentureFieldDecl sequence_step_fields[] = {
	VENTURE_FIELD_REF("sequence-id", "Sequence id", NULL, "sequence", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("position", "Position", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("delay-days", "Delay days", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("delay-hours", "Delay hours", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("channel", "Channel", NULL, venture_sequence_channel_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("subject", "Subject", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("body", "Body", NULL),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureSequenceStep, venture_sequence_step, sequence_step_fields)
static const VentureFieldDecl sequence_enrollment_fields[] = {
	VENTURE_FIELD_REF("sequence-id", "Sequence id", NULL, "sequence", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Contact id", NULL, "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("lead-id", "Lead id", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("deal-id", "Deal id", NULL, "deal", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("status", "Status", NULL, venture_sequence_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("current-step", "Current step", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("next-run-at", "Next run at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("enrolled-by", "Enrolled by", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("enrolled-at", "Enrolled at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("exit-reason", "Exit reason", NULL),
	VENTURE_FIELD("goal-met-at", "Goal met at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("enrollment-reason", "Enrollment reason", NULL),
	VENTURE_FIELD_REF("owner-id", "Owner id", NULL, "user", VENTURE_COLUMN_FLAG_INDEXED),
};
VENTURE_DEFINE_ENTITY(VentureSequenceEnrollment, venture_sequence_enrollment, sequence_enrollment_fields)
static const VentureFieldDecl sequence_delivery_fields[] = {
	VENTURE_FIELD_REF("enrollment-id", "Enrollment id", NULL, "sequence_enrollment", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("step-id", "Step id", NULL, "sequence_step", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("channel", "Channel", NULL, venture_sequence_channel_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("subject", "Subject", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("body", "Body", NULL),
	VENTURE_FIELD_ENUM("state", "State", NULL, venture_sequence_delivery_state_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("scheduled-at", "Scheduled at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("executed-at", "Executed at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("error", "Error", NULL),
	VENTURE_FIELD("external-message-id", "External message id", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("delivery-key", "Delivery key", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
};
VENTURE_DEFINE_ENTITY(VentureSequenceDelivery, venture_sequence_delivery, sequence_delivery_fields)
static const VentureFieldDecl suppression_fields[] = {
	VENTURE_FIELD("email", "Email", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_ENUM("reason", "Reason", NULL, venture_sequence_suppression_reason_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("at", "At", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureSuppression, venture_suppression, suppression_fields)
