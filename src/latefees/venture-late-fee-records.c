/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/* One row per organization: the switches. Off until a person turns it on,
 * and approval rather than auto-apply once they do. */
static const VentureFieldDecl setting_fields[] = {
	VENTURE_FIELD("enabled", "Late fees on", "Off by default; with it off no fee is proposed, added or mentioned to a customer",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("auto-apply", "Apply without approval", "Explicit opt-in; otherwise each due fee is proposed for the owner to approve or skip",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("default-schedule-id", "Default schedule", "Applies to every invoice whose customer, type or own setting names no other",
		"late_fee_schedule", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("enabled-at", "Charging since", "Occurrences that fell due before this day are never charged; set when late fees are switched on",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("paused", "Paused", "Organization-wide pause: nothing is added and no fee wording reaches a customer",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("paused-until", "Paused until", "First day fees may apply again; empty while paused means indefinitely",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("pause-reason", "Pause reason", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureLateFeeSetting, venture_late_fee_setting, setting_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Late fee settings", "Late fee settings");)

/* The tiers are one JSON document, like a reminder policy's steps, so a
 * schedule is edited and audited as one thing. */
static const VentureFieldDecl schedule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Residential, Commercial net 30"),
	VENTURE_FIELD("tiers", "Tiers", "JSON array, in order: {\"day\": N, \"every\": E (recurring), \"type\": flat|percent_open|percent_original|markup, "
		"\"amount\": \"25.00 USD\" or \"percent\": \"1.5\", \"cap\": money, \"round\": half_even|up|down|whole, \"conditions\": {...}}; at most 20",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("grace-days", "Grace days", "No fee on or before this many days late", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("minimum-balance", "Minimum balance", "No fee while the open balance, fees excluded, is below this"),
	VENTURE_FIELD("max-percent", "Cap: percent of original", "Total fees never exceed this percent of the original invoice, for example 10",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("max-total", "Cap: maximum total", "Total fees on one invoice never exceed this"),
	VENTURE_FIELD("invoice-types", "Invoice types", "Comma separated invoice types this schedule governs when nothing more specific names one",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("adopted-at", "Adopted", "Occurrences before this day are never charged; the creation time when empty",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", "Late-fee limits vary by jurisdiction; record the rule this schedule follows")
};
VENTURE_DEFINE_ENTITY(VentureLateFeeSchedule, venture_late_fee_schedule, schedule_fields)

/* One occurrence of one tier for one invoice, and every decision about it.
 * The occurrence key is what makes a replayed sweep write nothing. */
static const VentureFieldDecl fee_fields[] = {
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("company-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("schedule-id", "Schedule", NULL, "late_fee_schedule", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tier", "Tier", "1-based position in the schedule when recorded; display only", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("trigger-day", "Days late", "The occurrence's day, counted from the due date", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("occurrence-key", "Occurrence key", "Invoice, schedule, trigger and day; a sweep never writes one twice",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("due-on", "Due on", "The day the fee may be added", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Amount", "What is (or was) charged"),
	VENTURE_FIELD_MONEY("noticed-amount", "Amount announced", "What the advance notice told the customer; never exceeded"),
	VENTURE_FIELD("basis", "Basis", "How the amount was reached", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("status", "Status", "scheduled, awaiting_notice, proposed, applied, skipped, waived, cancelled or capped",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("reason", "Reason", "Why it was skipped, waived or cancelled", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("notice-event-id", "Advance notice", "The reminder that stated this fee", "dunning_event", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("notice-skipped", "Notice skipped", "The owner let this fee proceed without the advance notice",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("invoice-event-id", "Adjustment", "The late_fee invoice event that added it", "invoice_event", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("waiver-credit-id", "Waiver", "The credit that reversed it", "customer_credit", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("decided-by", "Decided by", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("decided-at", "Decided at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("note", "Note to the customer", "Sent with a waiver")
};
VENTURE_DEFINE_ENTITY(VentureLateFee, venture_late_fee, fee_fields)
