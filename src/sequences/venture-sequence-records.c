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
			{ 4, "SEQUENCE_CHANNEL_BRANCH", "branch" },
			{ 5, "SEQUENCE_CHANNEL_TEXT", "text" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureSequenceChannel", values);
		 g_once_init_leave(&type_id, id);
	}
	return type_id;
}
GType
venture_sequence_branch_on_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ 0, "SEQUENCE_BRANCH_NONE", "none" },
			{ 1, "SEQUENCE_BRANCH_OPENED", "opened" },
			{ 2, "SEQUENCE_BRANCH_CLICKED", "clicked" },
			{ 3, "SEQUENCE_BRANCH_REPLIED", "replied" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureSequenceBranchOn", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}
GType
venture_sequence_trigger_kind_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ 0, "SEQUENCE_TRIGGER_FORM_SUBMITTED", "form_submitted" },
			{ 1, "SEQUENCE_TRIGGER_TAG_ADDED", "tag_added" },
			{ 2, "SEQUENCE_TRIGGER_DEAL_STAGE_CHANGED", "deal_stage_changed" },
			{ 3, "SEQUENCE_TRIGGER_QUOTE_NOT_ACCEPTED", "quote_not_accepted" },
			{ 4, "SEQUENCE_TRIGGER_INVOICE_PAID", "invoice_paid" },
			{ 5, "SEQUENCE_TRIGGER_BOOKING_MADE", "booking_made" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureSequenceTriggerKind", values);
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
	VENTURE_FIELD("tracking", "Track opens and clicks", "Only takes effect when the organization allows tracking", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	/* A number, not a reference: the topic is the marketing module's and
	 * sequences must not need it. Set, it makes this a marketing journey
	 * that honours the recipient's topic and frequency choices. */
	VENTURE_FIELD("topic-id", "Marketing topic", "Set for a marketing journey: its email steps honour the recipient's topic and frequency choices", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureSequence, venture_sequence, sequence_fields)
static const VentureFieldDecl sequence_step_fields[] = {
	VENTURE_FIELD_REF("survey-form-id", "Personal survey form", "Append a private personal link to email delivery", "form", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("sequence-id", "Sequence id", NULL, "sequence", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("position", "Position", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("delay-days", "Delay days", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("delay-hours", "Delay hours", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("channel", "Channel", NULL, venture_sequence_channel_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("subject", "Subject", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("body", "Body", NULL),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	/* Journeys (docs/journeys.org): a branch step waits its delay, then
	 * goes to yes-position or no-position; any step may jump on or end. */
	VENTURE_FIELD_ENUM("branch-on", "Branch on", "For a branch step: what the latest email must have had by the end of the wait", venture_sequence_branch_on_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("yes-position", "If yes, go to", "Step position when it happened; empty is the next step", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("no-position", "If no, go to", "Step position when it did not; empty is the next step", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("then-position", "Then go to", "Step position after this one; empty is the next step", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("end-after", "End after this step", "The journey finishes here, for the last step of a path", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureSequenceStep, venture_sequence_step, sequence_step_fields)
static const VentureFieldDecl sequence_enrollment_fields[] = {
	VENTURE_FIELD("active-key", "Active key", "Derived enrollment uniqueness", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
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
	/* One event enrols a contact at most once per journey: the key is the
	 * journey, the event's identity and the contact, deleted rows too. */
	VENTURE_FIELD("trigger-key", "Trigger identity", "Derived: journey, event and contact", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_REF("trigger-id", "Trigger", "The trigger that enrolled this contact", "sequence_trigger", VENTURE_COLUMN_FLAG_INDEXED),
};
VENTURE_DEFINE_ENTITY(VentureSequenceEnrollment, venture_sequence_enrollment, sequence_enrollment_fields)
static const VentureFieldDecl sequence_delivery_fields[] = {
	VENTURE_FIELD("private-body", "Private adapter body", "Personal form capability for the mail adapter; never display", VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_SENSITIVE),
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
	VENTURE_FIELD("tracking-token", "Tracking token", "Per-delivery secret behind the open pixel and wrapped links", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("unsubscribe-token", "Unsubscribe token", "Per-delivery secret for the unsubscribe link", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("unsubscribe-email", "Unsubscribe email", "Address that received the unsubscribe link", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("branch-result", "Branch taken", "For a branch step: yes or no", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureSequenceDelivery, venture_sequence_delivery, sequence_delivery_fields)
static const VentureFieldDecl suppression_fields[] = {
	VENTURE_FIELD("email", "Email", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_ENUM("reason", "Reason", NULL, venture_sequence_suppression_reason_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("at", "At", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureSuppression, venture_suppression, suppression_fields)
GType
venture_sequence_tracking_kind_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ 0, "SEQUENCE_TRACKING_OPEN", "open" },
			{ 1, "SEQUENCE_TRACKING_CLICK", "click" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureSequenceTrackingKind", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}
static const VentureFieldDecl sequence_link_fields[] = {
	VENTURE_FIELD_REF("delivery-id", "Delivery id", NULL, "sequence_delivery", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("position", "Position", "Link number within the message, from 1", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("url", "Url", "The original destination", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureSequenceLink, venture_sequence_link, sequence_link_fields)
static const VentureFieldDecl sequence_tracking_event_fields[] = {
	VENTURE_FIELD_REF("delivery-id", "Delivery id", NULL, "sequence_delivery", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("enrollment-id", "Enrollment id", NULL, "sequence_enrollment", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("step-id", "Step id", NULL, "sequence_step", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Contact id", NULL, "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, venture_sequence_tracking_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("link-position", "Link position", "Zero for an open", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("occurred-at", "Occurred at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("dedupe-key", "Dedupe key", "One open per delivery per UTC day", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("machine-reason", "Judged a machine's because", "Empty for a person; such an open never takes a branch", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureSequenceTrackingEvent, venture_sequence_tracking_event, sequence_tracking_event_fields)
/* A named reason to enrol a contact in a journey (docs/journeys.org). */
static const VentureFieldDecl sequence_trigger_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("sequence-id", "Journey", "The sequence the contact is enrolled in", "sequence", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("kind", "When", NULL, venture_sequence_trigger_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("form-id", "Form", "Form submitted: only this form; empty is any form", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tag", "Tag", "Tag added: the tag, any capitals", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("deal-stage", "Deal stage", "Deal stage changed: only moves into this stage, for example won; empty is any move", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("days", "Days", "Quote sent but not accepted: how many days it waits; 7 when empty", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("owner-id", "Owner", "Owner of the enrollments it makes, for call and text steps", "user", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("feed-cursor", "Feed position", "Derived: the last business event this trigger has read", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_TECHNICAL),
};
VENTURE_DEFINE_ENTITY(VentureSequenceTrigger, venture_sequence_trigger, sequence_trigger_fields)
