/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/* A policy's steps are one JSON document rather than child rows so that a
 * policy is edited and audited as one thing; a step is only meaningful in
 * the order of its offset. */
static const VentureFieldDecl policy_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("steps", "Steps", "JSON array of {\"offset\": days relative to due (negative is before), \"template_id\": mail_template}",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("is-default", "Organization default", "Applies to every invoice that names no other policy; one per organization",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("final-escalation", "Final step escalates", "The last step creates a collect next action for the invoice owner instead of an email",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("adopted-at", "Adopted", "Invoices issued before this date are the before-policy baseline in the collections report",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureDunningPolicy, venture_dunning_policy, policy_fields)

static const VentureFieldDecl event_fields[] = {
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("policy-id", "Policy", NULL, "dunning_policy", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("company-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("step", "Step", "1-based position in the policy", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("offset-days", "Offset", "Days relative to the due date when this step became due", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("dunning-key", "Dunning key", "Durable invoice and step identity; a sweep never repeats one",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("queued-at", "Queued at", "Sweep time that produced this step", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("sent-at", "Sent at", "Relay acceptance time mirrored from the outbox", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("delivery-status", "Delivery status", "queued, sent, failed, dead, uncertain, cancelled, escalated or suppressed",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("suppressed-reason", "Suppressed reason", "Why no email went out for this step",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("mail-message-id", "Mail message", NULL, "mail_message", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("activity-id", "Next action", NULL, "activity", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("last-error", "Last error", "Retained render or delivery failure")
};
VENTURE_DEFINE_ENTITY(VentureDunningEvent, venture_dunning_event, event_fields)
